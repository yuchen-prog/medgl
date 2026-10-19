#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "medgl/dicom.hpp"

#include <nlohmann/json.hpp>
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Json = nlohmann::json;
namespace fs = std::filesystem;

void Require(bool condition) { if (!condition) throw std::runtime_error("Benchmark regression failed"); }

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("medgl-benchmark-test-" + std::to_string(GetCurrentProcessId())
            + "-" + std::to_string(stamp));
        Require(fs::create_directory(path));
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    fs::path path;
};

std::wstring Quote(const std::wstring& argument) {
    std::wstring quoted = L"\"";
    std::size_t slashes = 0;
    for (const auto character : argument) {
        if (character == L'\\') {
            ++slashes;
        } else {
            quoted.append(character == L'\"' ? slashes * 2 + 1 : slashes, L'\\');
            quoted += character;
            slashes = 0;
        }
    }
    quoted.append(slashes * 2, L'\\');
    return quoted + L'\"';
}

DWORD Run(const fs::path& executable, const std::vector<std::wstring>& arguments) {
    std::wstring command = Quote(executable.native());
    for (const auto& argument : arguments) command += L" " + Quote(argument);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    Require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
        0, nullptr, nullptr, &startup, &process) != 0);
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 180000);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 10000);
        CloseHandle(process.hProcess);
        Require(false);
    }
    DWORD exitCode = 0;
    const bool received = GetExitCodeProcess(process.hProcess, &exitCode) != 0;
    CloseHandle(process.hProcess);
    Require(received);
    return exitCode;
}

Json ReadJson(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    Require(static_cast<bool>(stream));
    Json report;
    stream >> report;
    return report;
}

void AssertClose(double actual, double expected) {
    Require(std::isfinite(actual) && std::abs(actual - expected) <= 1e-6 * (std::max)(1.0, std::abs(expected)));
}

void AssertNoIdentifyingFields(const Json& value) {
    if (value.is_object()) {
        for (const auto& entry : value.items()) {
            Require(entry.key() != "dataRoot" && entry.key() != "filename" && entry.key() != "path"
                && entry.key() != "patientName" && entry.key() != "patientId" && entry.key() != "seriesUid"
                && entry.key() != "studyUid" && entry.key() != "instanceUid" && entry.key() != "seriesId");
            AssertNoIdentifyingFields(entry.value());
        }
    } else if (value.is_array()) {
        for (const auto& element : value) AssertNoIdentifyingFields(element);
    }
}

void AssertReport(const Json& report, std::size_t frames, std::size_t cycles) {
    Require(report.at("version") == 1 && report.at("passed").get<bool>());
    Require(report.at("notForClinicalUse").get<bool>() && !report.at("externallyValidated").get<bool>());
    Require(!report.at("imagesSaved").get<bool>() && report.at("input").at("kind") == "phantom");
    Require(report.at("device").at("index") == 0 && report.at("device").at("totalMemoryBytes").get<std::uint64_t>() > 0);
    Require(report.at("scanMs") == 0 && report.at("volumeLoadMs").get<double>() >= 0);
    Require(report.at("cases").size() == 6);
    const std::array<const char*, 3> cameras{"anterior", "left", "inferior"};
    const std::array<const char*, 8> metrics{"prepareMs", "kernelMs", "kernelWallMs", "readbackMs",
        "totalMs", "encodeMs", "renderPlusEncodeMs", "encodedBytes"};
    std::size_t caseIndex = 0;
    for (const auto& testCase : report.at("cases")) {
        const bool jpeg = caseIndex < cameras.size();
        const std::string camera = cameras[caseIndex % cameras.size()];
        Require(testCase.at("camera") == camera);
        Require(testCase.at("name") == std::string(jpeg ? "preview-jpeg-" : "final-png-") + camera);
        Require(testCase.at("format") == (jpeg ? "jpeg" : "png"));
        Require(testCase.at("settings").at("stepMm") == (jpeg ? 1.5 : 0.5));
        Require(testCase.at("settings").at("quaternionXyzw").size() == 4);
        Require(testCase.at("frames").size() == frames && testCase.at("warmups").size() == 2);
        Require(testCase.at("passed").get<bool>() && testCase.at("pixelsDeterministic").get<bool>()
            && testCase.at("encodedDeterministic").get<bool>());
        for (const auto& frame : testCase.at("frames")) {
            Require(frame.at("valid").get<bool>() && frame.at("pixelsMatchReference").get<bool>()
                && frame.at("payloadMatchesReference").get<bool>());
            Require(frame.at("nonblankPixels").get<std::size_t>() >= 1 && frame.at("encodedBytes").get<std::size_t>() > 0);
            Require(frame.at("pixelChecksum").get<std::string>().size() == 16
                && frame.at("encodedChecksum").get<std::string>().size() == 16);
            AssertClose(frame.at("renderPlusEncodeMs").get<double>(),
                frame.at("totalMs").get<double>() + frame.at("encodeMs").get<double>());
        }
        for (const auto metric : metrics) {
            std::vector<double> samples;
            double sum = 0;
            for (const auto& frame : testCase.at("frames")) {
                const double sample = frame.at(metric).get<double>();
                Require(std::isfinite(sample) && sample >= 0);
                samples.push_back(sample);
                sum += sample;
            }
            std::sort(samples.begin(), samples.end());
            const auto& stats = testCase.at("stats").at(metric);
            const auto count = samples.size();
            Require(stats.at("count") == count);
            AssertClose(stats.at("min").get<double>(), samples.front());
            AssertClose(stats.at("max").get<double>(), samples.back());
            AssertClose(stats.at("mean").get<double>(), sum / static_cast<double>(count));
            AssertClose(stats.at("median").get<double>(), count % 2 != 0 ? samples[count / 2]
                : (samples[count / 2 - 1] + samples[count / 2]) / 2);
            AssertClose(stats.at("p95").get<double>(), samples[(95 * count + 99) / 100 - 1]);
        }
        ++caseIndex;
    }
    const auto& memory = report.at("memory");
    Require(memory.at("completedCycles") == cycles && memory.at("cycles").size() == cycles);
    Require(memory.at("globalMemoryNotProcessSpecific").get<bool>() && memory.at("externalWorkloadCanChange").get<bool>());
    Require(!memory.at("processLocalResourceTrackerAvailable").get<bool>() && memory.at("passed").get<bool>());
    Require(memory.at("toleranceBytes") == 64ull * 1024 * 1024);
    for (const auto& cycle : memory.at("cycles")) {
        Require(cycle.at("passed").get<bool>() && cycle.at("positiveAllocation").get<bool>());
        Require(cycle.at("renders").size() == 6 && cycle.at("postUnload").at("freeBytes").get<std::uint64_t>() > 0);
        Require(cycle.at("loaded").at("freeBytes").get<std::uint64_t>()
            < cycle.at("beforeLoadPostUnload").at("freeBytes").get<std::uint64_t>());
        Require(std::abs(cycle.at("driftAfterUnloadMiB").get<double>()) <= 64);
    }
    AssertNoIdentifyingFields(report);
}

void CheckCases(const fs::path& executable, const fs::path& directory) {
    const auto output = directory / "nested" / "report.json";
    const std::vector<std::wstring> quick{L"--frames", L"2", L"--cycles", L"3", L"--output", output.native(),
        L"--compare-png", L"--compare-png-fast"};
    Require(Run(executable, quick) == 0);
    const auto first = ReadJson(output);
    AssertReport(first, 2, 3);
    const auto& comparison = first.at("pngComparison");
    Require(comparison.at("passed").get<bool>() && comparison.at("sameRgbBuffer").get<bool>()
        && comparison.at("alternatingOrder").get<bool>() && !comparison.at("compressionParametersChanged").get<bool>());
    Require(comparison.at("cases").size() == 3);
    for (const auto& testCase : comparison.at("cases")) {
        Require(testCase.at("passed").get<bool>() && testCase.at("samples").size() == 2);
        Require(testCase.at("warmupPairs") == 2 && testCase.at("measuredPairs") == 2);
        for (const auto& sample : testCase.at("samples")) {
            Require(sample.at("passed").get<bool>() && sample.at("bitstreamEqual").get<bool>());
            Require(sample.at("legacyMismatchBytes") == 0 && sample.at("singleWriteMismatchBytes") == 0
                && sample.at("maxChannelError") == 0 && sample.at("legacyBytes") == sample.at("singleWriteBytes"));
        }
        for (const auto metric : {"legacyMs", "singleWriteMs"}) {
            const auto& samples = testCase.at("samples");
            const double firstMs = samples[0].at(metric).get<double>();
            const double secondMs = samples[1].at(metric).get<double>();
            Require(std::isfinite(firstMs) && firstMs >= 0 && std::isfinite(secondMs) && secondMs >= 0);
            AssertClose(testCase.at(metric).at("median").get<double>(), (firstMs + secondMs) / 2);
            AssertClose(testCase.at(metric).at("p95").get<double>(), (std::max)(firstMs, secondMs));
        }
    }
    Require(Run(executable, quick) == 0);
    const auto second = ReadJson(output);
    AssertReport(second, 2, 3);
    const auto& fast = second.at("pngFastComparison");
    Require(fast.at("passed").get<bool>() && fast.at("compressionParametersChanged").get<bool>());
    Require(fast.at("sameRgbBuffer").get<bool>() && fast.at("alternatingOrder").get<bool>());
    Require(fast.at("cases").size() == 3);
    for (const auto& testCase : fast.at("cases")) {
        Require(testCase.at("passed").get<bool>() && testCase.at("samples").size() == 2);
        Require(testCase.at("samples")[0].at("defaultFirst").get<bool>()
            && !testCase.at("samples")[1].at("defaultFirst").get<bool>());
        for (const auto& sample : testCase.at("samples")) {
            Require(sample.at("passed").get<bool>() && sample.at("maxChannelError") == 0);
            Require(sample.at("defaultMismatchBytes") == 0 && sample.at("fastMismatchBytes") == 0);
            Require(sample.at("defaultBytes").get<std::size_t>() > 0 && sample.at("fastBytes").get<std::size_t>() > 0);
        }
        for (const auto metric : {"defaultMs", "fastMs"}) {
            const double firstMs = testCase.at("samples")[0].at(metric).get<double>();
            const double secondMs = testCase.at("samples")[1].at(metric).get<double>();
            Require(std::isfinite(firstMs) && firstMs >= 0 && std::isfinite(secondMs) && secondMs >= 0);
            AssertClose(testCase.at(metric).at("median").get<double>(), (firstMs + secondMs) / 2);
            AssertClose(testCase.at(metric).at("p95").get<double>(), (std::max)(firstMs, secondMs));
        }
    }
    for (std::size_t index = 0; index < first.at("cases").size(); ++index) {
        Require(first.at("cases")[index].at("frames")[0].at("pixelChecksum")
            == second.at("cases")[index].at("frames")[0].at("pixelChecksum"));
    }
    Require(Run(executable, {L"--frames", L"1", L"--cycles", L"1", L"--width", L"64", L"--height", L"64",
        L"--output", output.native()}) == 0);
    AssertReport(ReadJson(output), 1, 1);
    Require(ReadJson(output).at("pngComparison").is_null());
    Require(ReadJson(output).at("pngFastComparison").is_null());
    const std::array<std::vector<std::wstring>, 17> invalid{{
        {L"--frames", L"0"}, {L"--frames", L"201"}, {L"--frames", L"NaN"}, {L"--frames", L"2.5"},
        {L"--cycles", L"0"}, {L"--cycles", L"101"}, {L"--width", L"0"}, {L"--width", L"2049"},
        {L"--height", L"2049"}, {L"--height", L"inf"}, {L"--frames", L"-1"},
        {L"--frames", L"2", L"--frames", L"3"}, {L"--preset", L"not-a-preset"}, {L"--unknown"},
        {L"--frames"}, {L"--series", L"series-1"}, {L"--allow-instance-uid-mismatch"}
    }};
    for (auto arguments : invalid) {
        arguments.push_back(L"--output");
        arguments.push_back((directory / "invalid.json").native());
        Require(Run(executable, arguments) == 1);
        Require(!fs::exists(directory / "invalid.json"));
    }
    const auto multiple = directory / "multiple-series";
    medgl::WriteSyntheticCt(multiple / "first");
    medgl::WriteSyntheticCt(multiple / "second");
    const auto multipleReport = directory / "multiple-report.json";
    const std::vector<std::wstring> multipleArguments{L"--data-root", multiple.native(),
        L"--output", multipleReport.native(), L"--frames", L"1", L"--cycles", L"1",
        L"--width", L"64", L"--height", L"64"};
    Require(Run(executable, multipleArguments) == 1 && !fs::exists(multipleReport));
    auto selectedArguments = multipleArguments;
    selectedArguments.insert(selectedArguments.end(), {L"--series", L"series-2"});
    Require(Run(executable, selectedArguments) == 0);
    Require(ReadJson(multipleReport).at("input").at("dimensions") == Json::array({64, 64, 64}));
    const auto inputRoot = directory / "SOURCE";
    fs::create_directory(inputRoot);
    Require(Run(executable, {L"--data-root", inputRoot.native(), L"--output",
        (directory / "source" / "report.json").native()}) == 1);
    Require(!fs::exists(inputRoot / "report.json"));
    const auto source = inputRoot / "fixture.bin";
    {
        std::ofstream stream(source, std::ios::binary);
        stream << "Non-DICOM fixture must not change";
        stream.flush();
        Require(static_cast<bool>(stream));
    }
    const auto alias = directory / "alias.json";
    fs::create_hard_link(source, alias);
    Require(Run(executable, {L"--output", alias.native()}) == 1);
    std::ifstream sourceStream(source, std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(sourceStream)), std::istreambuf_iterator<char>());
    Require(contents == "Non-DICOM fixture must not change");
    Require(Run(executable, {L"--output", source.native() + L":alternate"}) == 1);
    const auto directoryAlias = directory / "source-alias";
    if (CreateSymbolicLinkW(directoryAlias.c_str(), inputRoot.c_str(),
        SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != 0) {
        Require(Run(executable, {L"--data-root", inputRoot.native(), L"--output",
            (directoryAlias / "report.json").native()}) == 1);
        Require(!fs::exists(inputRoot / "report.json"));
        fs::remove(directoryAlias);
    } else {
        std::cout << "Directory symlink check skipped: unprivileged symlink creation unavailable.\n";
    }
}

}

int main(int argc, char** argv) {
    try {
        Require(argc == 2);
        const auto executable = fs::canonical(argv[1]);
        TemporaryDirectory temporary;
        CheckCases(executable, temporary.path);
        std::cout << "Benchmark report, CLI and output protection regressions passed.\n";
        return 0;
    } catch (...) {
        std::cerr << "Benchmark report regression failed; no identifying details are logged.\n";
        return 1;
    }
}