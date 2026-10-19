#include "medgl/dicom.hpp"

#include <gdcmAttribute.h>
#include <gdcmByteValue.h>
#include <gdcmDataSet.h>
#include <gdcmImageReader.h>
#include <gdcmImageWriter.h>
#include <gdcmReader.h>
#include <gdcmTrace.h>
#include <gdcmUIDGenerator.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <streambuf>
#include <tuple>
#include <utility>

namespace medgl {
namespace {

constexpr const char* CtStorage = "1.2.840.10008.5.1.4.1.1.2";
constexpr std::size_t MaxFiles = 10000;
constexpr std::size_t MaxVoxels = 512ull * 1024 * 1024;
using Vector3 = std::array<double, 3>;
using GroupKey = std::tuple<std::string, std::string, std::string>;

class DicomError : public std::runtime_error {
public:
    explicit DicomError(const char* message) : std::runtime_error(message) {}
};

[[noreturn]] void Fail(const char* message) { throw DicomError(message); }

class NullBuffer : public std::streambuf {
    int_type overflow(int_type value) override { return traits_type::not_eof(value); }
    std::streamsize xsputn(const char*, std::streamsize count) override { return count; }
};

class QuietGdcm {
public:
    QuietGdcm()
        : lock(Mutex()), sink(&buffer), debug(gdcm::Trace::GetDebugFlag()),
          warning(gdcm::Trace::GetWarningFlag()), error(gdcm::Trace::GetErrorFlag()),
          debugStream(gdcm::Trace::GetDebugStream()),
          warningStream(gdcm::Trace::GetWarningStream()),
          errorStream(gdcm::Trace::GetErrorStream()) {
        gdcm::Trace::SetDebugStream(sink);
        gdcm::Trace::SetWarningStream(sink);
        gdcm::Trace::SetErrorStream(sink);
        gdcm::Trace::SetDebug(false);
        gdcm::Trace::SetWarning(false);
        gdcm::Trace::SetError(false);
    }

    ~QuietGdcm() {
        gdcm::Trace::SetDebugStream(debugStream);
        gdcm::Trace::SetWarningStream(warningStream);
        gdcm::Trace::SetErrorStream(errorStream);
        gdcm::Trace::SetDebug(debug);
        gdcm::Trace::SetWarning(warning);
        gdcm::Trace::SetError(error);
    }

private:
    static std::mutex& Mutex() { static std::mutex mutex; return mutex; }
    std::unique_lock<std::mutex> lock;
    NullBuffer buffer;
    std::ostream sink;
    bool debug, warning, error;
    std::ostream &debugStream, &warningStream, &errorStream;
};

std::string Trim(std::string text) {
    while (!text.empty() && (text.back() == ' ' || text.back() == '\0')) text.pop_back();
    const auto first = text.find_first_not_of(' ');
    return first == std::string::npos ? std::string{} : text.substr(first);
}

std::string Text(const gdcm::DataSet& data, std::uint16_t group, std::uint16_t element) {
    const gdcm::Tag tag(group, element);
    if (!data.FindDataElement(tag)) return {};
    const auto* bytes = data.GetDataElement(tag).GetByteValue();
    if (!bytes) Fail("Malformed DICOM text tag.");
    if (bytes->GetLength() > 1024) Fail("Oversized DICOM text tag.");
    return Trim(std::string(bytes->GetPointer(), bytes->GetLength()));
}

bool ValidUid(const std::string& value) {
    if (value.empty() || value.size() > 64) return false;
    std::size_t start = 0;
    while (start < value.size()) {
        const auto end = value.find('.', start);
        const auto length = (end == std::string::npos ? value.size() : end) - start;
        if (!length || (length > 1 && value[start] == '0')) return false;
        for (std::size_t index = start; index < start + length; ++index)
            if (value[index] < '0' || value[index] > '9') return false;
        if (end == std::string::npos) return true;
        start = end + 1;
    }
    return false;
}

std::vector<std::string> Components(const std::string& text) {
    std::vector<std::string> values;
    std::size_t start = 0;
    for (;;) {
        const auto end = text.find('\\', start);
        values.push_back(Trim(text.substr(start, end == std::string::npos ? end : end - start)));
        if (end == std::string::npos) return values;
        start = end + 1;
    }
}

double Decimal(const std::string& text) {
    if (text.empty() || text.size() > 16) Fail("Missing or malformed DICOM numeric tag.");
    const char* first = text.data();
    const char* last = first + text.size();
    if (*first == '+') {
        ++first;
        if (first == last || *first == '+' || *first == '-') Fail("Malformed DICOM numeric sign.");
    }
    double value = 0;
    const auto result = std::from_chars(first, last, value);
    if (result.ec != std::errc{} || result.ptr != last || !std::isfinite(value))
        Fail("Missing or nonfinite DICOM numeric tag.");
    return value;
}

template <std::size_t Count>
std::array<double, Count> Decimals(const std::string& text) {
    const auto parts = Components(text);
    if (parts.size() != Count) Fail("Incorrect DICOM numeric tag multiplicity.");
    std::array<double, Count> values{};
    for (std::size_t index = 0; index < Count; ++index) values[index] = Decimal(parts[index]);
    return values;
}

template <std::uint16_t Group, std::uint16_t Element>
unsigned Short(const gdcm::DataSet& data) {
    const gdcm::Tag tag(Group, Element);
    if (!data.FindDataElement(tag)) Fail("Missing DICOM pixel format tag.");
    const auto& entry = data.GetDataElement(tag);
    const auto* bytes = entry.GetByteValue();
    if (!bytes || bytes->GetLength() != 2) Fail("Malformed DICOM pixel format tag.");
    if (entry.GetVR() != gdcm::VR::US && entry.GetVR() != gdcm::VR::INVALID)
        Fail("Incorrect DICOM pixel format tag representation.");
    gdcm::Attribute<Group, Element> attribute;
    attribute.SetFromDataElement(entry);
    return attribute.GetValue();
}

double Dot(const Vector3& left, const Vector3& right) {
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

Vector3 Subtract(const Vector3& left, const Vector3& right) {
    return {left[0] - right[0], left[1] - right[1], left[2] - right[2]};
}

Vector3 Cross(const Vector3& left, const Vector3& right) {
    return {left[1] * right[2] - left[2] * right[1],
            left[2] * right[0] - left[0] * right[2],
            left[0] * right[1] - left[1] * right[0]};
}

float Float(double value) {
    if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
        Fail("DICOM values exceed the volume numeric range.");
    return static_cast<float>(value);
}

struct Slice {
    std::filesystem::path path;
    GroupKey group;
    std::string sop, imageType, photometric, kvp, reason;
    unsigned rows = 0, columns = 0, bitsStored = 0, representation = 0;
    Vector3 position{}, axisX{}, axisY{};
    std::array<double, 2> pixelSpacing{};
    double slope = 0, intercept = 0;
    bool instanceUidMismatch = false;
};

Slice ParseSlice(const gdcm::DataSet& data, const std::filesystem::path& path) {
    Slice slice;
    slice.path = path;
    try {
        slice.group = {Text(data, 0x0020, 0x000d), Text(data, 0x0020, 0x000e),
                       Text(data, 0x0020, 0x0052)};
        if (Text(data, 0x0008, 0x0016) != CtStorage || Text(data, 0x0008, 0x0060) != "CT")
            Fail("Only conventional single-frame CT is supported.");
        if (!ValidUid(std::get<0>(slice.group)) || !ValidUid(std::get<1>(slice.group)) ||
            !ValidUid(std::get<2>(slice.group))) Fail("Missing or malformed DICOM group identifiers.");
        slice.sop = Text(data, 0x0008, 0x0018);
        if (!ValidUid(slice.sop)) Fail("Missing or malformed DICOM instance identifier.");
        const auto frames = Text(data, 0x0028, 0x0008);
        if (data.FindDataElement(gdcm::Tag(0x0028, 0x0008)) && frames != "1")
            Fail("Multiframe CT is not supported.");
        if (data.FindDataElement(gdcm::Tag(0x5200, 0x9229)) ||
            data.FindDataElement(gdcm::Tag(0x5200, 0x9230)))
            Fail("Enhanced CT is not supported.");
        slice.imageType = Text(data, 0x0008, 0x0008);
        const auto types = Components(slice.imageType);
        if (types.size() < 3 || types[0].empty() || types[1].empty() || types[2].empty())
            Fail("Missing or malformed CT image type.");
        for (const auto& type : types)
            if (type.find("LOCALIZER") != std::string::npos || type.find("SCOUT") != std::string::npos ||
                type.find("TOPOGRAM") != std::string::npos || type.find("MIXED") != std::string::npos ||
                type.find("ENERGY") != std::string::npos || type.find("SPECTRAL") != std::string::npos)
                Fail("Localizer, mixed-type and multienergy CT are not supported.");
        const auto energy = Text(data, 0x0018, 0x9361);
        if ((data.FindDataElement(gdcm::Tag(0x0018, 0x9361)) && energy != "NO") ||
            data.FindDataElement(gdcm::Tag(0x0018, 0x9362)))
            Fail("Multienergy CT is not supported.");
        if (data.FindDataElement(gdcm::Tag(0x0018, 0x1120)) &&
            std::abs(Decimal(Text(data, 0x0018, 0x1120))) > 1e-4)
            Fail("Gantry tilt is not supported.");
        slice.kvp = Text(data, 0x0018, 0x0060);
        if (!slice.kvp.empty()) {
            const double kvp = Decimal(slice.kvp);
            if (kvp <= 0) Fail("Malformed CT acquisition energy.");
            slice.kvp = std::to_string(kvp);
        }
        slice.rows = Short<0x0028, 0x0010>(data);
        slice.columns = Short<0x0028, 0x0011>(data);
        slice.bitsStored = Short<0x0028, 0x0101>(data);
        slice.representation = Short<0x0028, 0x0103>(data);
        if (!slice.rows || !slice.columns || Short<0x0028, 0x0002>(data) != 1 ||
            Short<0x0028, 0x0100>(data) != 16 || slice.bitsStored < 12 || slice.bitsStored > 16 ||
            Short<0x0028, 0x0102>(data) != slice.bitsStored - 1 || slice.representation > 1)
            Fail("Only aligned 12-16-bit single-channel CT pixels are supported.");
        slice.photometric = Text(data, 0x0028, 0x0004);
        if (slice.photometric != "MONOCHROME1" && slice.photometric != "MONOCHROME2")
            Fail("Only monochrome CT is supported.");
        slice.position = Decimals<3>(Text(data, 0x0020, 0x0032));
        const auto orientation = Decimals<6>(Text(data, 0x0020, 0x0037));
        std::copy_n(orientation.begin(), 3, slice.axisX.begin());
        std::copy_n(orientation.begin() + 3, 3, slice.axisY.begin());
        if (std::abs(Dot(slice.axisX, slice.axisX) - 1) > 1e-4 ||
            std::abs(Dot(slice.axisY, slice.axisY) - 1) > 1e-4 ||
            std::abs(Dot(slice.axisX, slice.axisY)) > 1e-4)
            Fail("Invalid CT orientation vectors.");
        slice.pixelSpacing = Decimals<2>(Text(data, 0x0028, 0x0030));
        for (const double spacing : slice.pixelSpacing)
            if (spacing <= 0 || Float(spacing) == 0) Fail("Invalid CT pixel spacing.");
        for (const double coordinate : slice.position) Float(coordinate);
        slice.slope = Decimal(Text(data, 0x0028, 0x1053));
        slice.intercept = Decimal(Text(data, 0x0028, 0x1052));
        if (slice.slope == 0) Fail("CT rescale slope must be nonzero.");
        const double range = static_cast<double>(1u << slice.bitsStored);
        const double minimum = slice.representation ? -range / 2 : 0;
        const double maximum = slice.representation ? range / 2 - 1 : range - 1;
        Float(minimum * slice.slope + slice.intercept);
        Float(maximum * slice.slope + slice.intercept);
        const auto rescaleType = Text(data, 0x0028, 0x1054);
        if (!rescaleType.empty() && rescaleType != "HU") Fail("Non-HU CT rescale is not supported.");
        if (data.FindDataElement(gdcm::Tag(0x0028, 0x3000)))
            Fail("CT modality lookup tables are not supported.");
    } catch (const DicomError& exception) {
        slice.reason = exception.what();
    }
    return slice;
}

const std::set<gdcm::Tag>& HeaderTags() {
    static const std::set<gdcm::Tag> tags{
        {0x0008, 0x0008}, {0x0008, 0x0016}, {0x0008, 0x0018}, {0x0008, 0x0060},
        {0x0018, 0x0060}, {0x0018, 0x1120}, {0x0018, 0x9361}, {0x0018, 0x9362},
        {0x0020, 0x000d}, {0x0020, 0x000e}, {0x0020, 0x0032}, {0x0020, 0x0037}, {0x0020, 0x0052},
        {0x0028, 0x0002}, {0x0028, 0x0004}, {0x0028, 0x0008}, {0x0028, 0x0010}, {0x0028, 0x0011},
        {0x0028, 0x0030}, {0x0028, 0x0100}, {0x0028, 0x0101}, {0x0028, 0x0102}, {0x0028, 0x0103},
        {0x0028, 0x1052}, {0x0028, 0x1053}, {0x0028, 0x1054}, {0x0028, 0x3000},
        {0x5200, 0x9229}, {0x5200, 0x9230}};
    return tags;
}

bool CheckStorage(const gdcm::File& file, const Slice& slice, DicomReadOptions options) {
    const auto& meta = file.GetHeader();
    const auto storage = Text(meta, 0x0002, 0x0002);
    const auto instance = Text(meta, 0x0002, 0x0003);
    if (!storage.empty() && storage != CtStorage)
        Fail("Inconsistent CT file and dataset identifiers.");
    if (!instance.empty() && !ValidUid(instance))
        Fail("Malformed CT file instance identifier.");
    const bool mismatch = !instance.empty() && instance != slice.sop;
    if (mismatch && !options.allowInstanceUidMismatch)
        Fail("Inconsistent CT file and dataset identifiers.");
    return mismatch;
}

std::optional<Slice> ReadHeader(const std::filesystem::path& path, DicomReadOptions options) {
    QuietGdcm quiet;
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) Fail("Cannot read a catalog file.");
        gdcm::Reader reader;
        reader.SetStream(input);
        const bool read = reader.ReadSelectedTags(HeaderTags());
        const auto& data = reader.GetFile().GetDataSet();
        if (!read) {
            if (!data.FindDataElement(gdcm::Tag(0x0020, 0x000e))) return std::nullopt;
            auto slice = ParseSlice(data, path);
            slice.reason = "Malformed DICOM header.";
            return slice;
        }
        auto slice = ParseSlice(data, path);
        if (slice.reason.empty()) {
            try { slice.instanceUidMismatch = CheckStorage(reader.GetFile(), slice, options); }
            catch (const DicomError& exception) { slice.reason = exception.what(); }
        }
        return slice;
    } catch (const DicomError&) { throw; }
    catch (...) { Fail("DICOM header processing failed."); }
}

bool Inside(const std::filesystem::path& root, const std::filesystem::path& path) {
    auto rootPart = root.begin();
    auto pathPart = path.begin();
    for (; rootPart != root.end(); ++rootPart, ++pathPart)
        if (pathPart == path.end() || *rootPart != *pathPart) return false;
    return true;
}

void CheckRoot(const std::filesystem::path& root) {
    if (!std::filesystem::is_directory(root) || std::filesystem::canonical(root) != root)
        Fail("The catalog root is unavailable or has changed.");
}

void CheckPath(const std::filesystem::path& root, const std::filesystem::path& path) {
    if (!Inside(root, std::filesystem::canonical(path)))
        Fail("A catalog entry escapes the selected directory.");
}

std::vector<std::filesystem::path> Files(const std::filesystem::path& root) {
    CheckRoot(root);
    std::vector<std::filesystem::path> files;
    for (std::filesystem::recursive_directory_iterator entry(root), end; entry != end; ++entry) {
        CheckPath(root, entry->path());
        if (entry->is_symlink()) { entry.disable_recursion_pending(); continue; }
        if (!entry->is_regular_file()) continue;
        if (files.size() == MaxFiles) Fail("The catalog exceeds the 10000-file limit.");
        files.push_back(entry->path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

struct Geometry {
    Vector3 normal;
    double step;
};

Geometry Validate(std::vector<Slice>& slices) {
    if (slices.size() < 2) Fail("A CT volume requires at least two slices.");
    for (const auto& slice : slices)
        if (!slice.reason.empty()) throw DicomError(slice.reason.c_str());
    const Slice reference = slices.front();
    const std::size_t plane = static_cast<std::size_t>(reference.rows) * reference.columns;
    if (plane > MaxVoxels / slices.size()) Fail("The CT volume exceeds the 512M-voxel limit.");
    auto normal = Cross(reference.axisX, reference.axisY);
    const double length = std::sqrt(Dot(normal, normal));
    for (auto& coordinate : normal) coordinate /= length;
    std::set<std::string> instances;
    for (const auto& slice : slices) {
        if (slice.group != reference.group || slice.rows != reference.rows ||
            slice.columns != reference.columns || slice.bitsStored != reference.bitsStored ||
            slice.representation != reference.representation || slice.photometric != reference.photometric)
            Fail("Inconsistent CT identifiers, dimensions or pixel formats.");
        if (slice.imageType != reference.imageType || slice.kvp != reference.kvp)
            Fail("Mixed-type or multienergy CT is not supported.");
        if (!instances.insert(slice.sop).second) Fail("Duplicate CT instance identifiers.");
        for (std::size_t axis = 0; axis < 3; ++axis)
            if (std::abs(slice.axisX[axis] - reference.axisX[axis]) > 1e-4 ||
                std::abs(slice.axisY[axis] - reference.axisY[axis]) > 1e-4)
                Fail("Inconsistent CT slice orientation.");
        for (std::size_t axis = 0; axis < 2; ++axis)
            if (std::abs(slice.pixelSpacing[axis] - reference.pixelSpacing[axis]) >
                std::max(1e-5, reference.pixelSpacing[axis] * 1e-5))
                Fail("Inconsistent CT pixel spacing.");
        if (!std::isfinite(Dot(Subtract(slice.position, reference.position), normal)))
            Fail("CT geometry exceeds the numeric range.");
    }
    std::sort(slices.begin(), slices.end(), [&](const Slice& left, const Slice& right) {
        return Dot(Subtract(left.position, reference.position), normal) <
               Dot(Subtract(right.position, reference.position), normal);
    });
    const auto origin = slices.front().position;
    const double step = Dot(Subtract(slices[1].position, origin), normal);
    if (step <= 1e-4 || Float(step) == 0) Fail("Duplicate or invalid CT slice positions.");
    const double tolerance = std::max(0.01, step * 0.001);
    double previous = 0;
    for (std::size_t index = 0; index < slices.size(); ++index) {
        const auto delta = Subtract(slices[index].position, origin);
        const double distance = Dot(delta, normal);
        if (index && distance - previous <= 1e-4) Fail("Duplicate CT slice positions.");
        if ((index && std::abs((distance - previous) - step) > tolerance) ||
            std::abs(distance - static_cast<double>(index) * step) > tolerance)
            Fail("Irregular CT slice spacing.");
        Vector3 lateral{};
        for (std::size_t axis = 0; axis < 3; ++axis) lateral[axis] = delta[axis] - normal[axis] * distance;
        if (std::sqrt(Dot(lateral, lateral)) > 0.01)
            Fail("Gantry tilt or lateral slice drift is not supported.");
        previous = distance;
    }
    return {normal, step};
}

bool SameHeader(const Slice& left, const Slice& right) {
    return left.reason.empty() && left.group == right.group && left.sop == right.sop &&
        left.imageType == right.imageType && left.kvp == right.kvp &&
        left.rows == right.rows && left.columns == right.columns && left.bitsStored == right.bitsStored &&
        left.representation == right.representation && left.photometric == right.photometric &&
        left.position == right.position && left.axisX == right.axisX && left.axisY == right.axisY &&
        left.pixelSpacing == right.pixelSpacing && left.slope == right.slope && left.intercept == right.intercept;
}

void Decode(const Slice& slice, float* destination, DicomReadOptions options) {
    QuietGdcm quiet;
    try {
        std::ifstream input(slice.path, std::ios::binary);
        if (!input) Fail("Cannot read a CT slice.");
        gdcm::ImageReader reader;
        reader.SetStream(input);
        if (!reader.Read()) Fail("Cannot decode a CT slice.");
        if (!SameHeader(ParseSlice(reader.GetFile().GetDataSet(), slice.path), slice))
            Fail("CT slice metadata changed during loading.");
        CheckStorage(reader.GetFile(), slice, options);
        const auto& image = reader.GetImage();
        const auto& format = image.GetPixelFormat();
        const std::size_t count = static_cast<std::size_t>(slice.rows) * slice.columns;
        if (image.GetDimension(0) != slice.columns || image.GetDimension(1) != slice.rows ||
            (image.GetNumberOfDimensions() > 2 && image.GetDimension(2) != 1) ||
            format.GetSamplesPerPixel() != 1 || format.GetBitsAllocated() != 16 ||
            format.GetBitsStored() != slice.bitsStored || format.GetHighBit() != slice.bitsStored - 1 ||
            format.GetPixelRepresentation() != slice.representation || image.GetBufferLength() != count * 2 ||
            image.GetSlope() != slice.slope || image.GetIntercept() != slice.intercept)
            Fail("Decoded CT pixels do not match the validated header.");
        std::vector<char> buffer(count * 2);
        if (!image.GetBuffer(buffer.data())) Fail("CT pixel decompression failed.");
        const std::uint32_t range = 1u << slice.bitsStored;
        const std::uint32_t mask = range - 1;
        const std::uint32_t sign = range >> 1;
        for (std::size_t index = 0; index < count; ++index) {
            std::uint16_t word = 0;
            std::memcpy(&word, buffer.data() + index * 2, sizeof(word));
            const auto stored = static_cast<std::int32_t>(word & mask);
            const auto value = slice.representation && (word & sign) ?
                stored - static_cast<std::int32_t>(range) : stored;
            destination[index] = Float(value * image.GetSlope() + image.GetIntercept());
        }
    } catch (const DicomError&) { throw; }
    catch (...) { Fail("CT decoding failed."); }
}

void SetText(gdcm::DataSet& data, std::uint16_t group, std::uint16_t element,
             gdcm::VR vr, std::string value) {
    if (value.size() % 2) value.push_back(vr == gdcm::VR::UI ? '\0' : ' ');
    gdcm::DataElement entry(gdcm::Tag(group, element));
    entry.SetVR(vr);
    entry.SetByteValue(value.data(), static_cast<std::uint32_t>(value.size()));
    data.Replace(entry);
}

int PhantomHu(unsigned column, unsigned row, unsigned slice) {
    const double axisX = (static_cast<double>(column) - 31) / 25;
    const double axisY = (static_cast<double>(row) - 30) / 21;
    const double axisZ = (static_cast<double>(slice) - 32) / 26;
    const double radius = axisX * axisX + axisY * axisY + axisZ * axisZ;
    if (radius > 1) return -1000;
    if (radius > 0.80) return 700;
    const double lesionX = (static_cast<double>(column) - 42) / 6;
    const double lesionY = (static_cast<double>(row) - 25) / 5;
    const double lesionZ = (static_cast<double>(slice) - 36) / 8;
    if (lesionX * lesionX + lesionY * lesionY + lesionZ * lesionZ <= 1) return 300;
    const double cavityX = (static_cast<double>(column) - 22) / 7;
    const double cavityY = (static_cast<double>(row) - 35) / 8;
    const double cavityZ = (static_cast<double>(slice) - 27) / 10;
    if (cavityX * cavityX + cavityY * cavityY + cavityZ * cavityZ <= 1) return -800;
    return 40;
}

}

struct DicomCatalog::Impl {
    struct Group { GroupKey key; std::vector<Slice> slices; };
    std::filesystem::path root;
    std::map<GroupKey, std::string> ids;
    std::map<std::string, Group> groups;
    std::size_t nextId = 1;
    DicomReadOptions options;
};

DicomCatalog::DicomCatalog(std::filesystem::path root, DicomReadOptions options) : m_impl(std::make_unique<Impl>()) {
    m_impl->options = options;
    try {
        m_impl->root = std::filesystem::canonical(root);
        CheckRoot(m_impl->root);
    } catch (const DicomError&) { throw; }
    catch (...) { Fail("Cannot open the selected DICOM directory."); }
}

DicomCatalog::~DicomCatalog() = default;

std::vector<SeriesInfo> DicomCatalog::Scan() {
    try {
        std::map<GroupKey, std::vector<Slice>> discovered;
        for (const auto& path : Files(m_impl->root)) {
            auto slice = ReadHeader(path, m_impl->options);
            if (slice) discovered[slice->group].push_back(std::move(*slice));
        }
        std::map<std::string, Impl::Group> groups;
        std::vector<SeriesInfo> result;
        for (auto& entry : discovered) {
            auto found = m_impl->ids.find(entry.first);
            if (found == m_impl->ids.end())
                found = m_impl->ids.emplace(entry.first, "series-" + std::to_string(m_impl->nextId++)).first;
            SeriesInfo info{found->second, "CT series", entry.second.size(), true, {}, {}};
            if (std::any_of(entry.second.begin(), entry.second.end(), [](const Slice& slice) { return slice.instanceUidMismatch; }))
                info.warning = "Compatibility mode: file-meta instance UID differs from dataset UID. Original files are unchanged.";
            try { Validate(entry.second); }
            catch (const DicomError& exception) {
                info.supported = false;
                info.description = "Unsupported DICOM series";
                info.reason = exception.what();
            }
            groups.emplace(info.id, Impl::Group{entry.first, std::move(entry.second)});
            result.push_back(std::move(info));
        }
        m_impl->groups.swap(groups);
        return result;
    } catch (const DicomError&) { throw; }
    catch (...) { Fail("DICOM catalog scanning failed."); }
}

VolumeData DicomCatalog::Load(const std::string& id) const {
    try {
        const auto found = m_impl->groups.find(id);
        if (found == m_impl->groups.end()) Fail("Unknown DICOM series identifier.");
        CheckRoot(m_impl->root);
        std::vector<Slice> slices;
        for (const auto& previous : found->second.slices) {
            CheckPath(m_impl->root, previous.path);
            auto slice = ReadHeader(previous.path, m_impl->options);
            if (!slice || slice->group != found->second.key) Fail("A catalog slice is missing or has changed.");
            slices.push_back(std::move(*slice));
        }
        const auto geometry = Validate(slices);
        const auto& first = slices.front();
        VolumeData volume;
        volume.shape = {first.columns, first.rows, static_cast<std::uint32_t>(slices.size())};
        volume.spacing = {Float(first.pixelSpacing[1]), Float(first.pixelSpacing[0]), Float(geometry.step)};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            volume.origin[axis] = Float(first.position[axis]);
            volume.direction[axis * 3] = Float(first.axisX[axis]);
            volume.direction[axis * 3 + 1] = Float(first.axisY[axis]);
            volume.direction[axis * 3 + 2] = Float(geometry.normal[axis]);
        }
        const std::size_t plane = static_cast<std::size_t>(first.columns) * first.rows;
        volume.voxels.resize(plane * slices.size());
        for (std::size_t index = 0; index < slices.size(); ++index) {
            CheckPath(m_impl->root, slices[index].path);
            Decode(slices[index], volume.voxels.data() + index * plane, m_impl->options);
        }
        return volume;
    } catch (const DicomError&) { throw; }
    catch (...) { Fail("DICOM volume loading failed."); }
}

void WriteSyntheticCt(const std::filesystem::path& root) {
    try {
        std::filesystem::create_directories(root);
        const auto directory = std::filesystem::canonical(root);
        if (!std::filesystem::is_empty(directory)) Fail("Synthetic CT output requires an empty directory.");
        QuietGdcm quiet;
        gdcm::UIDGenerator generator;
        const std::string study = generator.Generate();
        const std::string series = generator.Generate();
        const std::string frame = generator.Generate();
        for (unsigned slice = 0; slice < 64; ++slice) {
            gdcm::ImageWriter writer;
            auto& data = writer.GetFile().GetDataSet();
            SetText(data, 0x0008, 0x0008, gdcm::VR::CS, "ORIGINAL\\PRIMARY\\AXIAL");
            SetText(data, 0x0008, 0x0016, gdcm::VR::UI, CtStorage);
            SetText(data, 0x0008, 0x0018, gdcm::VR::UI, generator.Generate());
            SetText(data, 0x0008, 0x0060, gdcm::VR::CS, "CT");
            SetText(data, 0x0008, 0x103e, gdcm::VR::LO, "Synthetic CT phantom");
            SetText(data, 0x0020, 0x000d, gdcm::VR::UI, study);
            SetText(data, 0x0020, 0x000e, gdcm::VR::UI, series);
            SetText(data, 0x0020, 0x0052, gdcm::VR::UI, frame);
            SetText(data, 0x0020, 0x0013, gdcm::VR::IS, std::to_string(slice + 1));
            SetText(data, 0x0028, 0x1054, gdcm::VR::LO, "HU");
            auto& image = writer.GetImage();
            image.SetNumberOfDimensions(2);
            image.SetDimension(0, 64);
            image.SetDimension(1, 64);
            image.SetPixelFormat(gdcm::PixelFormat(gdcm::PixelFormat::INT16));
            image.SetPhotometricInterpretation(gdcm::PhotometricInterpretation::MONOCHROME2);
            image.SetTransferSyntax(gdcm::TransferSyntax::ExplicitVRLittleEndian);
            const double spacing[3] = {1, 1, 1.5};
            const double origin[3] = {0, 0, slice * 1.5};
            const double orientation[6] = {1, 0, 0, 0, 1, 0};
            image.SetSpacing(spacing);
            image.SetOrigin(origin);
            image.SetSpacing(2, spacing[2]);
            image.SetOrigin(2, origin[2]);
            image.SetDirectionCosines(orientation);
            image.SetSlope(2);
            image.SetIntercept(-1000);
            if (image.GetOrigin(2) != origin[2]) Fail("Synthetic CT origin could not be set.");
            SetText(data, 0x0020, 0x0032, gdcm::VR::DS, "0\\0\\" + std::to_string(slice * 1.5));
            SetText(data, 0x0020, 0x0037, gdcm::VR::DS, "1\\0\\0\\0\\1\\0");
            SetText(data, 0x0028, 0x0030, gdcm::VR::DS, "1\\1");
            SetText(data, 0x0028, 0x1052, gdcm::VR::DS, "-1000");
            SetText(data, 0x0028, 0x1053, gdcm::VR::DS, "2");
            std::vector<std::int16_t> pixels(64 * 64);
            for (unsigned row = 0; row < 64; ++row)
                for (unsigned column = 0; column < 64; ++column)
                    pixels[static_cast<std::size_t>(row) * 64 + column] =
                        static_cast<std::int16_t>((PhantomHu(column, row, slice) + 1000) / 2);
            gdcm::DataElement pixelData(gdcm::Tag(0x7fe0, 0x0010));
            pixelData.SetVR(gdcm::VR::OW);
            pixelData.SetByteValue(reinterpret_cast<const char*>(pixels.data()),
                                   static_cast<std::uint32_t>(pixels.size() * sizeof(std::int16_t)));
            image.SetDataElement(pixelData);
            const auto number = (slice * 37 + 17) % 64;
            const auto path = directory / ("slice-" + std::to_string(100 + number) + ".dcm");
            std::ofstream output(path, std::ios::binary);
            if (!output) Fail("Cannot create synthetic CT output.");
            writer.SetStream(output);
            if (!writer.Write()) Fail("Synthetic CT writing failed.");
            output.flush();
            if (!output) Fail("Synthetic CT output could not be completed.");
        }
    } catch (const DicomError&) { throw; }
    catch (...) { Fail("Synthetic CT generation failed."); }
}

}