#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "medgl/dicom.hpp"
#include "medgl/geometry.hpp"
#include "medgl/image_codec.hpp"
#include "medgl/renderer.hpp"
#include "appearance_presets.hpp"

#include <cuda_runtime_api.h>
#include <nlohmann/json.hpp>
#include <png.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

constexpr std::uint64_t MemoryToleranceBytes = 64ull * 1024 * 1024;
constexpr std::uint64_t MaxVoxels = 512ull * 1024 * 1024;
constexpr double BytesPerMiB = 1024.0 * 1024.0;
constexpr int WarmupFrames = 2;
constexpr int JpegQuality = 85;

struct Options {
    fs::path dataRoot;
    fs::path output = "out/validation/vr-benchmark.json";
    std::string series;
    std::string preset = "ct-bone";
    std::uint32_t frames = 20;
    std::uint32_t cycles = 10;
    std::uint32_t width = 512;
    std::uint32_t height = 512;
    bool allowInstanceUidMismatch = false;
    bool comparePng = false;
    bool comparePngFast = false;
    bool help = false;
};

[[noreturn]] void Fail() { throw std::runtime_error("Benchmark operation failed"); }
void Require(bool condition) { if (!condition) Fail(); }
void CheckCuda(cudaError_t result) { Require(result == cudaSuccess); }
void Synchronize() { CheckCuda(cudaDeviceSynchronize()); }

double ElapsedMs(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void ValidateDuration(double value) { Require(std::isfinite(value) && value >= 0); }

std::uint32_t ParseCount(const std::string& text, std::uint32_t maximum) {
    std::uint32_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    Require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size());
    Require(value >= 1 && value <= maximum);
    return value;
}

Options ParseOptions(int argc, char** argv) {
    Options options;
    std::set<std::string> seen;
    for (int index = 1; index < argc; ++index) {
        const std::string argument(argv[index]);
        Require(seen.insert(argument).second);
        if (argument == "--help") {
            options.help = true;
        } else if (argument == "--allow-instance-uid-mismatch") {
            options.allowInstanceUidMismatch = true;
        } else if (argument == "--compare-png") {
            options.comparePng = true;
        } else if (argument == "--compare-png-fast") {
            options.comparePngFast = true;
        } else {
            Require(argument == "--data-root" || argument == "--series" || argument == "--output"
                || argument == "--frames" || argument == "--cycles" || argument == "--width"
                || argument == "--height" || argument == "--preset");
            Require(index + 1 < argc);
            const std::string value(argv[++index]);
            Require(!value.empty() && value.rfind("--", 0) != 0);
            if (argument == "--data-root") options.dataRoot = value;
            else if (argument == "--series") options.series = value;
            else if (argument == "--output") options.output = value;
            else if (argument == "--preset") options.preset = value;
            else if (argument == "--frames") options.frames = ParseCount(value, 200);
            else if (argument == "--cycles") options.cycles = ParseCount(value, 100);
            else if (argument == "--width") options.width = ParseCount(value, 2048);
            else if (argument == "--height") options.height = ParseCount(value, 2048);
        }
    }
    Require(!options.dataRoot.empty() || (options.series.empty() && !options.allowInstanceUidMismatch));
    return options;
}

void PrintUsage() {
    std::cout << "Usage: medgl_vr_benchmark [--data-root directory] [--series series-N] "
        "[--allow-instance-uid-mismatch] [--output out/validation/vr-benchmark.json] "
        "[--frames 20] [--cycles 10] [--width 512] [--height 512] [--preset ct-bone] "
        "[--compare-png] [--compare-png-fast]\n"
        "Limits: frames 1..200, cycles 1..100, width/height 1..2048. "
        "No data root uses a phantom. Exit codes: 0 passed, 2 validation failed, 1 error.\n";
}

class FileHandle {
public:
    explicit FileHandle(HANDLE handle) : handle_(handle) { Require(handle_ != INVALID_HANDLE_VALUE); }
    ~FileHandle() { CloseHandle(handle_); }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    HANDLE Get() const { return handle_; }
private:
    HANDLE handle_;
};

fs::path HandlePath(HANDLE handle) {
    const DWORD required = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    Require(required > 0 && required < 32768);
    std::wstring path(static_cast<std::size_t>(required), L'\0');
    const DWORD length = GetFinalPathNameByHandleW(handle, path.data(), required,
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    Require(length > 0 && length < required);
    path.resize(static_cast<std::size_t>(length));
    if (path.rfind(L"\\\\?\\UNC\\", 0) == 0) path = L"\\\\" + path.substr(8);
    else if (path.rfind(L"\\\\?\\", 0) == 0) path.erase(0, 4);
    return fs::path(path).lexically_normal();
}

fs::path CanonicalExisting(const fs::path& path) {
    const FileHandle handle(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    return HandlePath(handle.Get());
}

fs::path CanonicalOutput(const fs::path& path) {
    const auto normalized = fs::weakly_canonical(fs::absolute(path));
    fs::path existing = normalized;
    fs::path suffix;
    while (!fs::exists(existing)) {
        Require(existing.has_filename() && existing != existing.parent_path());
        suffix = suffix.empty() ? existing.filename() : existing.filename() / suffix;
        existing = existing.parent_path();
    }
    const auto resolved = CanonicalExisting(existing);
    return suffix.empty() ? resolved : (resolved / suffix).lexically_normal();
}

bool EqualIgnoringCase(const fs::path& first, const fs::path& second) {
    const auto& firstText = first.native();
    const auto& secondText = second.native();
    Require(firstText.size() <= static_cast<std::size_t>((std::numeric_limits<int>::max)())
        && secondText.size() <= static_cast<std::size_t>((std::numeric_limits<int>::max)()));
    return CompareStringOrdinal(firstText.data(), static_cast<int>(firstText.size()),
        secondText.data(), static_cast<int>(secondText.size()), TRUE) == CSTR_EQUAL;
}

bool Inside(const fs::path& root, const fs::path& candidate) {
    auto part = candidate.begin();
    for (const auto& component : root) {
        if (component.empty()) continue;
        if (part == candidate.end() || !EqualIgnoringCase(component, *part)) return false;
        ++part;
    }
    return true;
}

void CheckOutputPath(const fs::path& output, const fs::path& root) {
    Require(!output.filename().empty());
    for (const auto& component : output.relative_path()) {
        const auto text = component.native();
        Require(text.find(L':') == std::wstring::npos);
        Require(!text.empty() && text.back() != L'.' && text.back() != L' ');
    }
    Require(root.empty() || !Inside(root, CanonicalOutput(output)));
    if (fs::exists(output)) {
        Require(fs::is_regular_file(output) && fs::hard_link_count(output) == 1);
    }
}

void WriteReport(const fs::path& output, const fs::path& root, const Json& report) {
    CheckOutputPath(output, root);
    fs::create_directories(output.parent_path());
    CheckOutputPath(output, root);
    const bool exists = fs::exists(output);
    const FileHandle handle(CreateFileW(output.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES, 0,
        nullptr, exists ? OPEN_EXISTING : CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    Require(GetFileInformationByHandle(handle.Get(), &info) != 0);
    Require(info.nNumberOfLinks == 1 && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0);
    Require(root.empty() || !Inside(root, HandlePath(handle.Get())));
    const std::string text = report.dump(2) + '\n';
    LARGE_INTEGER beginning{};
    Require(SetFilePointerEx(handle.Get(), beginning, nullptr, FILE_BEGIN) != 0);
    std::size_t position = 0;
    while (position < text.size()) {
        const DWORD count = static_cast<DWORD>((std::min)(text.size() - position, std::size_t{1024 * 1024}));
        DWORD written = 0;
        Require(WriteFile(handle.Get(), text.data() + position, count, &written, nullptr) != 0);
        Require(written == count);
        position += static_cast<std::size_t>(written);
    }
    Require(SetEndOfFile(handle.Get()) != 0 && FlushFileBuffers(handle.Get()) != 0);
}

Json Statistics(std::vector<double> values) {
    Require(!values.empty());
    for (const double value : values) ValidateDuration(value);
    std::sort(values.begin(), values.end());
    const std::size_t count = values.size();
    const double median = count % 2 != 0 ? values[count / 2]
        : (values[count / 2 - 1] + values[count / 2]) / 2;
    const std::size_t rank = (95 * count + 99) / 100;
    const double mean = std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(count);
    ValidateDuration(mean);
    return {{"count", count}, {"median", median}, {"p95", values[rank - 1]},
        {"mean", mean}, {"min", values.front()}, {"max", values.back()}};
}

std::string Checksum(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t value = 14695981039346656037ull;
    for (const auto byte : bytes) {
        value ^= byte;
        value *= 1099511628211ull;
    }
    std::ostringstream text;
    text << std::hex << std::setfill('0') << std::setw(16) << value;
    return text.str();
}

std::size_t NonblankPixels(const medgl::RgbImage& image) {
    image.Validate();
    std::size_t count = 0;
    for (std::size_t index = 0; index < image.pixels.size(); index += 3) {
        if (image.pixels[index] != 0 || image.pixels[index + 1] != 0 || image.pixels[index + 2] != 0) ++count;
    }
    return count;
}

std::uint32_t BigEndian32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    Require(offset <= bytes.size() && bytes.size() - offset >= 4);
    return (static_cast<std::uint32_t>(bytes[offset]) << 24)
        | (static_cast<std::uint32_t>(bytes[offset + 1]) << 16)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 8)
        | static_cast<std::uint32_t>(bytes[offset + 3]);
}

bool PayloadValid(const std::vector<std::uint8_t>& bytes, bool jpeg, std::uint32_t width, std::uint32_t height) {
    if (!jpeg) {
        const std::array<std::uint8_t, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
        if (bytes.size() < 45 || !std::equal(signature.begin(), signature.end(), bytes.begin())) return false;
        const std::array<std::uint8_t, 4> ihdr{73, 72, 68, 82};
        const std::array<std::uint8_t, 12> iend{0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130};
        return BigEndian32(bytes, 8) == 13 && std::equal(ihdr.begin(), ihdr.end(), bytes.begin() + 12)
            && BigEndian32(bytes, 16) == width && BigEndian32(bytes, 20) == height
            && std::equal(iend.begin(), iend.end(), bytes.end() - 12);
    }
    if (bytes.size() < 4 || bytes[0] != 255 || bytes[1] != 216
        || bytes[bytes.size() - 2] != 255 || bytes.back() != 217) return false;
    std::size_t position = 2;
    bool dimensionsMatch = false;
    while (position + 4 <= bytes.size()) {
        if (bytes[position++] != 255) return false;
        while (position < bytes.size() && bytes[position] == 255) ++position;
        if (position >= bytes.size()) return false;
        const auto marker = bytes[position++];
        if (marker == 218) return dimensionsMatch;
        if (bytes.size() - position < 2) return false;
        const std::size_t length = static_cast<std::size_t>(bytes[position]) * 256 + bytes[position + 1];
        if (length < 2 || length > bytes.size() - position) return false;
        if (marker == 192 || marker == 193 || marker == 194) {
            if (length < 8) return false;
            const auto encodedHeight = static_cast<std::uint32_t>(bytes[position + 3]) * 256 + bytes[position + 4];
            const auto encodedWidth = static_cast<std::uint32_t>(bytes[position + 5]) * 256 + bytes[position + 6];
            dimensionsMatch = encodedWidth == width && encodedHeight == height;
        }
        position += length;
    }
    return false;
}

Json SettingsJson(const medgl::RenderSettings& settings) {
    Json transfer = Json::array();
    for (const auto& point : settings.transfer) transfer.push_back({{"position", point.position}, {"rgba", point.rgba}});
    return {{"width", settings.width}, {"height", settings.height}, {"target", settings.target},
        {"quaternionXyzw", settings.rotation}, {"orthoHeightMm", settings.orthoHeight},
        {"windowWidth", settings.windowWidth}, {"windowLevel", settings.windowLevel},
        {"stepMm", settings.stepMm}, {"referenceStepMm", settings.referenceStepMm},
        {"opacityScale", settings.opacityScale}, {"lighting", settings.lighting},
        {"backgroundRgb", settings.background}, {"transfer", transfer}};
}

void ValidateSettings(const medgl::RenderSettings& settings) {
    Require(settings.width >= 1 && settings.width <= 2048 && settings.height >= 1 && settings.height <= 2048);
    Require(std::isfinite(settings.orthoHeight) && settings.orthoHeight > 0);
    Require(std::isfinite(settings.windowWidth) && settings.windowWidth > 0 && std::isfinite(settings.windowLevel));
    Require(std::isfinite(settings.stepMm) && settings.stepMm >= 0.02f);
    Require(std::isfinite(settings.referenceStepMm) && settings.referenceStepMm > 0);
    Require(std::isfinite(settings.opacityScale) && settings.opacityScale >= 0 && settings.opacityScale <= 10);
    for (const auto value : settings.target) Require(std::isfinite(value));
    double quaternionLength = 0;
    for (const auto value : settings.rotation) {
        Require(std::isfinite(value));
        quaternionLength += static_cast<double>(value) * static_cast<double>(value);
    }
    Require(std::abs(quaternionLength - 1.0) <= 1e-4);
    for (const auto value : settings.background) Require(std::isfinite(value) && value == 0);
    Require(settings.transfer.size() >= 2 && settings.transfer.size() <= 128);
    float previous = -1;
    for (const auto& point : settings.transfer) {
        Require(std::isfinite(point.position) && point.position >= 0 && point.position <= 1 && point.position > previous);
        for (const auto value : point.rgba) Require(std::isfinite(value) && value >= 0 && value <= 1);
        previous = point.position;
    }
}

struct Camera {
    const char* name;
    medgl::StandardView view;
};

constexpr std::array<Camera, 3> Cameras{{{"anterior", medgl::StandardView::Anterior},
    {"left", medgl::StandardView::Left}, {"inferior", medgl::StandardView::Inferior}}};

medgl::RenderSettings CaseSettings(const medgl::RenderSettings& base, const Camera& camera, bool jpeg) {
    auto settings = base;
    settings.rotation = medgl::StandardViewRotation(camera.view);
    settings.stepMm = jpeg ? 1.5f : 0.5f;
    ValidateSettings(settings);
    return settings;
}

Json LoadTimingsJson(const medgl::LoadTimings& timings) {
    ValidateDuration(timings.validationMs);
    ValidateDuration(timings.uploadMs);
    ValidateDuration(timings.totalMs);
    return {{"validationMs", timings.validationMs}, {"uploadMs", timings.uploadMs}, {"totalMs", timings.totalMs}};
}

Json RunCase(medgl::VolumeRenderer& renderer, const medgl::RenderSettings& settings,
    const Camera& camera, bool jpeg, std::uint32_t frameCount) {
    std::vector<std::uint8_t> referencePixels, referencePayload;
    bool pixelsStable = true, payloadStable = true, valid = true;
    Json warmups = Json::array(), frames = Json::array();
    std::array<std::vector<double>, 8> measurements;
    for (std::uint32_t index = 0; index < frameCount + static_cast<std::uint32_t>(WarmupFrames); ++index) {
        const auto image = renderer.Render(settings);
        const auto timings = renderer.LastRenderTimings();
        const auto encodingStart = Clock::now();
        const auto payload = jpeg ? medgl::EncodeJpeg(image, JpegQuality) : medgl::EncodePng(image);
        const double encodeMs = ElapsedMs(encodingStart);
        const std::array<double, 8> sample{timings.prepareMs, timings.kernelMs, timings.kernelWallMs,
            timings.readbackMs, timings.totalMs, encodeMs, timings.totalMs + encodeMs,
            static_cast<double>(payload.size())};
        for (const auto value : sample) ValidateDuration(value);
        const auto nonblank = NonblankPixels(image);
        const bool frameValid = image.width == settings.width && image.height == settings.height
            && nonblank >= 1 && PayloadValid(payload, jpeg, settings.width, settings.height);
        if (index == 0) {
            referencePixels = image.pixels;
            referencePayload = payload;
        }
        const bool samePixels = image.pixels == referencePixels;
        const bool samePayload = payload == referencePayload;
        pixelsStable = pixelsStable && samePixels;
        payloadStable = payloadStable && samePayload;
        valid = valid && frameValid;
        Json frame{{"pixelChecksum", Checksum(image.pixels)}, {"encodedChecksum", Checksum(payload)},
            {"nonblankPixels", nonblank}, {"encodedBytes", payload.size()}, {"valid", frameValid},
            {"pixelsMatchReference", samePixels}, {"payloadMatchesReference", samePayload}};
        if (index < static_cast<std::uint32_t>(WarmupFrames)) {
            frame["index"] = index;
            warmups.push_back(std::move(frame));
            continue;
        }
        frame["index"] = index - static_cast<std::uint32_t>(WarmupFrames);
        frame["prepareMs"] = timings.prepareMs;
        frame["kernelMs"] = timings.kernelMs;
        frame["kernelWallMs"] = timings.kernelWallMs;
        frame["readbackMs"] = timings.readbackMs;
        frame["totalMs"] = timings.totalMs;
        frame["encodeMs"] = encodeMs;
        frame["renderPlusEncodeMs"] = timings.totalMs + encodeMs;
        frames.push_back(std::move(frame));
        for (std::size_t metric = 0; metric < measurements.size(); ++metric) measurements[metric].push_back(sample[metric]);
    }
    const std::array<const char*, 8> names{"prepareMs", "kernelMs", "kernelWallMs", "readbackMs",
        "totalMs", "encodeMs", "renderPlusEncodeMs", "encodedBytes"};
    Json stats = Json::object();
    for (std::size_t metric = 0; metric < names.size(); ++metric) stats[names[metric]] = Statistics(measurements[metric]);
    return {{"name", std::string(jpeg ? "preview-jpeg-" : "final-png-") + camera.name},
        {"camera", camera.name}, {"format", jpeg ? "jpeg" : "png"},
        {"jpegQuality", jpeg ? Json(JpegQuality) : Json(nullptr)}, {"settings", SettingsJson(settings)},
        {"warmupFrames", WarmupFrames}, {"warmups", warmups}, {"measuredFrames", frameCount},
        {"frames", frames}, {"stats", stats}, {"pixelsDeterministic", pixelsStable},
        {"encodedDeterministic", payloadStable}, {"framesValid", valid},
        {"passed", pixelsStable && payloadStable && valid}};
}

std::vector<std::uint8_t> LegacyPng(const medgl::RgbImage& image) {
    image.Validate();
    png_image description{};
    description.version = PNG_IMAGE_VERSION;
    description.width = image.width;
    description.height = image.height;
    description.format = PNG_FORMAT_RGB;
    png_alloc_size_t bytes = 0;
    if (!png_image_write_to_memory(&description, nullptr, &bytes, 0, image.pixels.data(), 0, nullptr)) {
        png_image_free(&description);
        Fail();
    }
    std::vector<std::uint8_t> encoded(bytes);
    if (!png_image_write_to_memory(&description, encoded.data(), &bytes, 0, image.pixels.data(), 0, nullptr)) {
        png_image_free(&description);
        Fail();
    }
    png_image_free(&description);
    encoded.resize(bytes);
    return encoded;
}

std::pair<std::size_t, unsigned> CompareDecoded(const std::vector<std::uint8_t>& encoded,
    const medgl::RgbImage& expected) {
    png_image description{};
    description.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&description, encoded.data(), encoded.size())) {
        png_image_free(&description);
        Fail();
    }
    if (description.width != expected.width || description.height != expected.height) {
        png_image_free(&description);
        Fail();
    }
    description.format = PNG_FORMAT_RGB;
    std::vector<std::uint8_t> decoded(expected.RequiredBytes());
    if (!png_image_finish_read(&description, nullptr, decoded.data(), 0, nullptr)) {
        png_image_free(&description);
        Fail();
    }
    png_image_free(&description);
    std::size_t mismatches = 0;
    unsigned maximumError = 0;
    for (std::size_t index = 0; index < decoded.size(); ++index) {
        const auto error = static_cast<unsigned>(std::abs(static_cast<int>(decoded[index])
            - static_cast<int>(expected.pixels[index])));
        if (error != 0) ++mismatches;
        maximumError = (std::max)(maximumError, error);
    }
    return {mismatches, maximumError};
}

Json ComparePng(medgl::VolumeRenderer& renderer, const medgl::RenderSettings& base,
    std::uint32_t frameCount, bool fastComparison = false) {
    Json cases = Json::array();
    bool passed = true;
    const char* baselineMetric = fastComparison ? "defaultMs" : "legacyMs";
    const char* candidateMetric = fastComparison ? "fastMs" : "singleWriteMs";
    for (const auto& camera : Cameras) {
        const auto settings = CaseSettings(base, camera, false);
        const auto image = renderer.Render(settings);
        const auto inputPixels = image.pixels;
        std::vector<double> legacySamples, singleSamples;
        Json samples = Json::array();
        bool casePassed = true;
        for (std::uint32_t index = 0; index < frameCount + WarmupFrames; ++index) {
            std::vector<std::uint8_t> legacy, single;
            double legacyMs = 0, singleMs = 0;
            const auto encodeLegacy = [&] {
                const auto start = Clock::now();
                legacy = fastComparison ? medgl::EncodePng(image) : LegacyPng(image);
                legacyMs = ElapsedMs(start);
            };
            const auto encodeSingle = [&] {
                const auto start = Clock::now();
                single = medgl::EncodePng(image, fastComparison ? medgl::PngCompression::Fast
                                                               : medgl::PngCompression::Default);
                singleMs = ElapsedMs(start);
            };
            if (index % 2 == 0) { encodeLegacy(); encodeSingle(); }
            else { encodeSingle(); encodeLegacy(); }
            ValidateDuration(legacyMs);
            ValidateDuration(singleMs);
            const auto legacyDiff = CompareDecoded(legacy, image);
            const auto singleDiff = CompareDecoded(single, image);
            const bool valid = legacyDiff.first == 0 && singleDiff.first == 0
                && (fastComparison || legacy == single) && inputPixels == image.pixels;
            casePassed = casePassed && valid;
            if (index < WarmupFrames) continue;
            legacySamples.push_back(legacyMs);
            singleSamples.push_back(singleMs);
            samples.push_back({{baselineMetric, legacyMs}, {candidateMetric, singleMs},
                {fastComparison ? "defaultFirst" : "legacyFirst", index % 2 == 0},
                {fastComparison ? "defaultBytes" : "legacyBytes", legacy.size()},
                {fastComparison ? "fastBytes" : "singleWriteBytes", single.size()},
                {fastComparison ? "defaultMismatchBytes" : "legacyMismatchBytes", legacyDiff.first},
                {fastComparison ? "fastMismatchBytes" : "singleWriteMismatchBytes", singleDiff.first},
                {"maxChannelError", (std::max)(legacyDiff.second, singleDiff.second)},
                {"bitstreamEqual", legacy == single}, {"passed", valid}});
        }
        passed = passed && casePassed;
        cases.push_back({{"camera", camera.name}, {"settings", SettingsJson(settings)},
            {"warmupPairs", WarmupFrames}, {"measuredPairs", frameCount}, {"samples", samples},
            {baselineMetric, Statistics(legacySamples)}, {candidateMetric, Statistics(singleSamples)},
            {"passed", casePassed}});
    }
    return {{"passed", passed}, {"cases", cases}, {"sameRgbBuffer", true}, {"alternatingOrder", true},
        {"compressionParametersChanged", fastComparison}, {"imagesSaved", false},
        {"decoder", "libpng RGB8; shares libpng with encoder, not an independent implementation"},
        {"timing", "Encoding only; per-case RGB rendered once; decoding and comparison excluded; payload allocation included"}};
}

struct MemorySnapshot {
    std::uint64_t freeBytes;
    std::uint64_t totalBytes;
    std::uint64_t Used() const { return totalBytes - freeBytes; }
    Json ToJson() const { return {{"freeBytes", freeBytes}, {"totalBytes", totalBytes}, {"usedBytes", Used()}}; }
};

MemorySnapshot Snapshot() {
    std::size_t freeBytes = 0, totalBytes = 0;
    CheckCuda(cudaMemGetInfo(&freeBytes, &totalBytes));
    Require(freeBytes <= totalBytes && totalBytes <= static_cast<std::size_t>((std::numeric_limits<std::int64_t>::max)()));
    return {static_cast<std::uint64_t>(freeBytes), static_cast<std::uint64_t>(totalBytes)};
}

std::int64_t Difference(std::uint64_t after, std::uint64_t before) {
    return static_cast<std::int64_t>(after) - static_cast<std::int64_t>(before);
}

bool WithinMargin(const MemorySnapshot& baseline, const MemorySnapshot& sample) {
    const auto drift = Difference(sample.Used(), baseline.Used());
    return baseline.totalBytes == sample.totalBytes
        && std::abs(drift) <= static_cast<std::int64_t>(MemoryToleranceBytes);
}

void Pan(medgl::RenderSettings& settings, float horizontal, float vertical) {
    const auto& quaternion = settings.rotation;
    const float qx = quaternion[0], qy = quaternion[1], qz = quaternion[2], qw = quaternion[3];
    const std::array<float, 3> right{1 - 2 * (qy * qy + qz * qz),
        2 * (qx * qy + qw * qz), 2 * (qx * qz - qw * qy)};
    const std::array<float, 3> up{2 * (qx * qy - qw * qz),
        1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz + qw * qx)};
    for (std::size_t axis = 0; axis < settings.target.size(); ++axis) {
        settings.target[axis] += settings.orthoHeight * (horizontal * right[axis] + vertical * up[axis]);
    }
    ValidateSettings(settings);
}

Json RunMemoryCycles(medgl::VolumeRenderer& renderer, const medgl::VolumeData& volume,
    const medgl::RenderSettings& base, std::uint32_t cycleCount) {
    renderer.Unload();
    Synchronize();
    const auto baseline = Snapshot();
    auto final = baseline;
    std::uint64_t minFree = baseline.freeBytes, maxFree = baseline.freeBytes;
    std::uint64_t minUsed = baseline.Used(), maxUsed = baseline.Used();
    std::uint64_t minPostUsed = baseline.Used(), maxPostUsed = baseline.Used();
    bool bounded = true, positiveAllocations = true, framesValid = true, nondecreasing = true;
    auto previous = baseline;
    Json cycles = Json::array();
    const auto account = [&](const MemorySnapshot& sample) {
        minFree = (std::min)(minFree, sample.freeBytes);
        maxFree = (std::max)(maxFree, sample.freeBytes);
        minUsed = (std::min)(minUsed, sample.Used());
        maxUsed = (std::max)(maxUsed, sample.Used());
    };
    for (std::uint32_t cycle = 0; cycle < cycleCount; ++cycle) {
        renderer.Unload();
        Synchronize();
        const auto before = Snapshot();
        account(before);
        renderer.Load(volume);
        Synchronize();
        const auto loaded = Snapshot();
        account(loaded);
        const bool allocated = loaded.totalBytes == before.totalBytes && loaded.freeBytes < before.freeBytes;
        positiveAllocations = positiveAllocations && allocated;
        std::uint64_t peakUsed = (std::max)(before.Used(), loaded.Used());
        Json renders = Json::array();
        bool cycleFramesValid = true;
        for (const bool jpeg : {true, false}) {
            for (const auto& camera : Cameras) {
                auto settings = CaseSettings(base, camera, jpeg);
                const float horizontal = 0.02f * static_cast<float>(static_cast<int>(cycle % 3) - 1);
                const float vertical = jpeg ? -0.015f : 0.015f;
                Pan(settings, horizontal, vertical);
                const auto image = renderer.Render(settings);
                const auto nonblank = NonblankPixels(image);
                const bool valid = image.width == settings.width && image.height == settings.height && nonblank >= 1;
                cycleFramesValid = cycleFramesValid && valid;
                Synchronize();
                const auto rendered = Snapshot();
                account(rendered);
                peakUsed = (std::max)(peakUsed, rendered.Used());
                renders.push_back({{"camera", camera.name}, {"stepMm", settings.stepMm},
                    {"panFraction", {horizontal, vertical}}, {"quaternionXyzw", settings.rotation},
                    {"pixelChecksum", Checksum(image.pixels)}, {"nonblankPixels", nonblank},
                    {"valid", valid}, {"memory", rendered.ToJson()}});
            }
        }
        renderer.Unload();
        Synchronize();
        const auto post = Snapshot();
        account(post);
        minPostUsed = (std::min)(minPostUsed, post.Used());
        maxPostUsed = (std::max)(maxPostUsed, post.Used());
        const bool within = WithinMargin(baseline, before) && WithinMargin(baseline, post);
        bounded = bounded && within;
        nondecreasing = nondecreasing && post.Used() >= previous.Used();
        previous = post;
        final = post;
        framesValid = framesValid && cycleFramesValid;
        cycles.push_back({{"index", cycle}, {"beforeLoadPostUnload", before.ToJson()},
            {"loaded", loaded.ToJson()}, {"positiveAllocation", allocated},
            {"sampledPeakUsedBytes", peakUsed}, {"renders", renders}, {"postUnload", post.ToJson()},
            {"driftAfterUnloadMiB", static_cast<double>(Difference(post.Used(), baseline.Used())) / BytesPerMiB},
            {"withinTolerance", within}, {"framesValid", cycleFramesValid},
            {"passed", within && allocated && cycleFramesValid}});
    }
    const bool spreadBounded = maxPostUsed - minPostUsed <= MemoryToleranceBytes;
    const bool passed = bounded && spreadBounded && positiveAllocations && framesValid;
    return {{"requestedCycles", cycleCount}, {"completedCycles", cycles.size()}, {"cycles", cycles},
        {"baseline", baseline.ToJson()}, {"afterFinalUnload", final.ToJson()},
        {"baselineStage", "After context, performance cases and codecs warmed; renderer unloaded and device synchronized"},
        {"globalMemoryNotProcessSpecific", true}, {"externalWorkloadCanChange", true},
        {"processLocalResourceTrackerAvailable", false}, {"isolatedGpuRecommended", true},
        {"measurement", "cudaMemGetInfo snapshots after synchronization; peak is sampled, not a true allocation high-water mark"},
        {"interpretation", "A failed check indicates observed global memory instability or invalid frames, not proof of a renderer-only leak"},
        {"timedAsPerformanceCases", false}, {"toleranceBytes", MemoryToleranceBytes},
        {"minFreeBytes", minFree}, {"maxFreeBytes", maxFree}, {"minUsedBytes", minUsed}, {"maxUsedBytes", maxUsed},
        {"minPostUnloadUsedBytesIncludingBaseline", minPostUsed}, {"maxPostUnloadUsedBytesIncludingBaseline", maxPostUsed},
        {"postUnloadSpreadBytes", maxPostUsed - minPostUsed},
        {"differenceBeforeAfter", {{"freeBytes", Difference(final.freeBytes, baseline.freeBytes)},
            {"usedBytes", Difference(final.Used(), baseline.Used())}}},
        {"driftAfterUnloadMiB", static_cast<double>(Difference(final.Used(), baseline.Used())) / BytesPerMiB},
        {"nondecreasingPostUnloadUsed", nondecreasing},
        {"observedGrowthBeyondTolerance", nondecreasing && Difference(final.Used(), baseline.Used())
            > static_cast<std::int64_t>(MemoryToleranceBytes)},
        {"bounded", bounded && spreadBounded}, {"positiveAllocationEveryCycle", positiveAllocations},
        {"framesValid", framesValid}, {"passed", passed}};
}

void ValidateVolume(const medgl::VolumeData& volume) {
    Require(volume.shape.width != 0 && volume.shape.height != 0 && volume.shape.depth != 0);
    std::uint64_t count = volume.shape.width;
    Require(count <= MaxVoxels / volume.shape.height);
    count *= volume.shape.height;
    Require(count <= MaxVoxels / volume.shape.depth);
    count *= volume.shape.depth;
    Require(count <= MaxVoxels && count == volume.voxels.size());
    medgl::ValidateGeometry(volume);
    for (const auto voxel : volume.voxels) Require(std::isfinite(voxel));
}

Json VersionJson(int version) {
    return {{"raw", version}, {"major", version / 1000}, {"minor", (version % 1000) / 10}};
}

Json BuildJson() {
#ifdef NDEBUG
    constexpr bool ndebug = true;
#else
    constexpr bool ndebug = false;
#endif
    Json build{{"projectVersion", "0.1.0"}, {"benchmarkVersion", 1}, {"date", __DATE__}, {"time", __TIME__},
        {"compiledWithNdebug", ndebug}, {"configuration", ndebug ? "Release" : "Debug"},
        {"configurationEvidence", "NDEBUG; optimization flags are not inferred"},
        {"cudaToolkit", VersionJson(CUDART_VERSION)}};
#ifdef _MSC_VER
    build["compiler"] = "MSVC";
    build["compilerVersion"] = _MSC_FULL_VER;
    build["cppStandard"] = _MSVC_LANG;
#else
    build["compiler"] = "Other C++ compiler";
    build["cppStandard"] = __cplusplus;
#endif
    return build;
}

int Run(const Options& options) {
    fs::path root;
    if (!options.dataRoot.empty()) {
        root = CanonicalExisting(fs::canonical(options.dataRoot));
        Require(fs::is_directory(root));
    }
    const auto output = CanonicalOutput(options.output);
    CheckOutputPath(output, root);
    const auto& presets = medgl::server::AppearancePresets();
    const auto preset = std::find_if(presets.begin(), presets.end(), [&](const auto& value) { return value.id == options.preset; });
    Require(preset != presets.end());

    double scanMs = 0;
    double volumeLoadMs = 0;
    medgl::VolumeData volume;
    if (root.empty()) {
        const auto start = Clock::now();
        volume = medgl::MakePhantom();
        volumeLoadMs = ElapsedMs(start);
    } else {
        medgl::DicomReadOptions readOptions;
        readOptions.allowInstanceUidMismatch = options.allowInstanceUidMismatch;
        medgl::DicomCatalog catalog(root, readOptions);
        const auto scanStart = Clock::now();
        const auto series = catalog.Scan();
        scanMs = ElapsedMs(scanStart);
        const auto supportedCount = std::count_if(series.begin(), series.end(),
            [](const auto& value) { return value.supported; });
        Require(!options.series.empty() || supportedCount == 1);
        const auto selected = std::find_if(series.begin(), series.end(), [&](const auto& value) {
            return value.supported && (options.series.empty() || value.id == options.series);
        });
        Require(selected != series.end());
        const auto loadStart = Clock::now();
        volume = catalog.Load(selected->id);
        volumeLoadMs = ElapsedMs(loadStart);
    }
    ValidateDuration(scanMs);
    ValidateDuration(volumeLoadMs);
    ValidateVolume(volume);
    auto base = medgl::FitView(volume);
    base.width = options.width;
    base.height = options.height;
    base.windowWidth = preset->windowWidth;
    base.windowLevel = preset->windowLevel;
    base.opacityScale = preset->opacityScale;
    base.lighting = preset->lighting;
    base.transfer = preset->transfer;
    base.background = {0, 0, 0};
    ValidateSettings(base);

    CheckCuda(cudaSetDevice(0));
    CheckCuda(cudaFree(nullptr));
    Synchronize();
    cudaDeviceProp properties{};
    CheckCuda(cudaGetDeviceProperties(&properties, 0));
    int driverVersion = 0, runtimeVersion = 0;
    CheckCuda(cudaDriverGetVersion(&driverVersion));
    CheckCuda(cudaRuntimeGetVersion(&runtimeVersion));
    const auto contextBaseline = Snapshot();

    medgl::VolumeRenderer renderer;
    renderer.Load(volume);
    const auto initialLoadTimings = LoadTimingsJson(renderer.LastLoadTimings());
    Json cases = Json::array();
    bool casesPassed = true;
    for (const bool jpeg : {true, false}) {
        for (const auto& camera : Cameras) {
            auto result = RunCase(renderer, CaseSettings(base, camera, jpeg), camera, jpeg, options.frames);
            casesPassed = casesPassed && result.at("passed").get<bool>();
            cases.push_back(std::move(result));
        }
    }
    const Json pngComparison = options.comparePng ? ComparePng(renderer, base, options.frames) : Json(nullptr);
    const Json pngFastComparison = options.comparePngFast ? ComparePng(renderer, base, options.frames, true) : Json(nullptr);
    const auto memory = RunMemoryCycles(renderer, volume, base, options.cycles);
    const bool passed = casesPassed && memory.at("passed").get<bool>()
        && (!options.comparePng || pngComparison.at("passed").get<bool>())
        && (!options.comparePngFast || pngFastComparison.at("passed").get<bool>());
    const Json report{{"version", 1}, {"passed", passed}, {"performanceCasesPassed", casesPassed},
        {"notForClinicalUse", true}, {"externallyValidated", false}, {"imagesSaved", false},
        {"build", BuildJson()},
        {"device", {{"index", 0}, {"name", std::string(properties.name)},
            {"computeCapability", {{"major", properties.major}, {"minor", properties.minor}}},
            {"driverVersion", VersionJson(driverVersion)}, {"runtimeVersion", VersionJson(runtimeVersion)},
            {"totalMemoryBytes", properties.totalGlobalMem}}},
        {"input", {{"kind", root.empty() ? "phantom" : "dicom"}, {"readOnly", true},
            {"allowInstanceUidMismatch", options.allowInstanceUidMismatch}, {"maxVoxels", MaxVoxels},
            {"dimensions", {volume.shape.width, volume.shape.height, volume.shape.depth}}, {"spacingMm", volume.spacing}}},
        {"preset", {{"id", preset->id}, {"name", preset->name}}},
        {"settings", SettingsJson(base)},
        {"scanMs", scanMs}, {"volumeLoadMs", volumeLoadMs}, {"initialRendererLoadTimings", initialLoadTimings},
        {"coldProcessNote", "First scan and CPU volume load in this process; OS file cache may be warm. This is not a true cold-cache measurement"},
        {"timingNotes", {{"kernelMs", "CUDA events around the kernel"},
            {"kernelWallMs", "CPU steady clock including launch and event synchronization"},
            {"prepareMs", "CPU validation, transfer lookup build/upload, output preparation and camera setup before kernel launch"},
            {"readbackMs", "CPU steady clock around synchronous D2H after kernel event synchronization"},
            {"uploadMs", "Renderer setup: allocation, volume H2D, texture/events creation and replacement-resource destruction; not pure copy time"},
            {"encodeMs", "CPU steady clock around encoding only; pixel checks and hashing excluded"},
            {"renderPlusEncodeMs", "Sum of renderer totalMs and CPU encodeMs, not an end-to-end wall interval"}}},
        {"statistics", {{"p95", "Nearest-rank: sorted[ceil(0.95 * count) - 1]"},
            {"median", "Middle sample, or mean of the two middle samples"}, {"timingUnits", "ms"},
            {"checksumAlgorithm", "FNV-1a-64 (non-cryptographic); determinism uses exact byte comparisons"},
            {"payloadValidation", "PNG/JPEG signatures, encoded dimensions and end markers; no independent decoder"}}},
        {"warmupFramesPerCase", WarmupFrames}, {"measuredFramesPerCase", options.frames},
        {"cases", cases}, {"pngComparison", pngComparison}, {"pngFastComparison", pngFastComparison},
        {"contextWarmedMemoryBeforeFirstLoad", contextBaseline.ToJson()}, {"memory", memory}};
    WriteReport(output, root, report);
    std::cout << (passed ? "VR benchmark passed.\n" : "VR benchmark completed with failed validation.\n");
    return passed ? 0 : 2;
}

}

int main(int argc, char** argv) {
    try {
        const auto options = ParseOptions(argc, argv);
        if (options.help) { PrintUsage(); return 0; }
        return Run(options);
    } catch (...) {
        std::cerr << "VR benchmark failed; input paths and identifying details are not logged.\n";
        return 1;
    }
}