#include "medgl/dicom_audit.hpp"
#include "medgl/geometry.hpp"

#include <gdcmAttribute.h>
#include <gdcmImageReader.h>
#include <gdcmReader.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace medgl {
namespace {

using Json = nlohmann::json;
using Vector3 = std::array<double, 3>;
using Key = std::tuple<std::string, std::string, std::string>;
constexpr const char* CtStorage = "1.2.840.10008.5.1.4.1.1.2";
constexpr std::size_t MaxFiles = 10000;
constexpr std::size_t MaxVoxels = 512ull * 1024 * 1024;

class AuditError : public std::runtime_error {
public:
    explicit AuditError(const char* message) : std::runtime_error(message) {}
};

[[noreturn]] void Fail(const char* message) { throw AuditError(message); }

std::string Trim(std::string value) {
    const auto padding = [](char character) { return character == ' ' || character == '\0'; };
    while (!value.empty() && padding(value.back())) value.pop_back();
    const auto first = std::find_if_not(value.begin(), value.end(), padding);
    value.erase(value.begin(), first);
    return value;
}

std::string Text(const gdcm::DataSet& data, std::uint16_t group, std::uint16_t element) {
    const gdcm::Tag tag(group, element);
    if (!data.FindDataElement(tag)) return {};
    const auto* bytes = data.GetDataElement(tag).GetByteValue();
    if (!bytes || bytes->GetLength() > 1024) Fail("Invalid audit metadata text.");
    return Trim(std::string(bytes->GetPointer(), bytes->GetLength()));
}

template <std::size_t Count>
std::array<double, Count> DecimalValues(const std::string& text) {
    std::array<double, Count> result{};
    std::istringstream components(text);
    components.imbue(std::locale::classic());
    for (auto& value : result) {
        std::string component;
        if (!std::getline(components, component, '\\')) Fail("Invalid audit decimal multiplicity.");
        component = Trim(component);
        if (component.empty() || component.size() > 16) Fail("Invalid audit decimal value.");
        std::istringstream numeric(component);
        numeric.imbue(std::locale::classic());
        if (!(numeric >> value) || !std::isfinite(value)) Fail("Invalid audit decimal value.");
        numeric >> std::ws;
        if (!numeric.eof()) Fail("Invalid audit decimal value.");
    }
    if (!components.eof()) Fail("Invalid audit decimal multiplicity.");
    return result;
}

template <std::uint16_t Group, std::uint16_t Element>
unsigned Short(const gdcm::DataSet& data) {
    const gdcm::Tag tag(Group, Element);
    if (!data.FindDataElement(tag)) Fail("Missing audit pixel format.");
    const auto& entry = data.GetDataElement(tag);
    const auto* bytes = entry.GetByteValue();
    if (!bytes || bytes->GetLength() != 2 ||
        (entry.GetVR() != gdcm::VR::US && entry.GetVR() != gdcm::VR::INVALID))
        Fail("Invalid audit pixel format.");
    gdcm::Attribute<Group, Element> attribute;
    attribute.SetFromDataElement(entry);
    return attribute.GetValue();
}

Key GroupKey(const gdcm::DataSet& data) {
    return {Text(data, 0x0020, 0x000d), Text(data, 0x0020, 0x000e), Text(data, 0x0020, 0x0052)};
}

const std::set<gdcm::Tag>& Tags() {
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

bool Inside(const std::filesystem::path& root, const std::filesystem::path& path) {
    auto candidate = path.begin();
    for (const auto& component : root) {
        if (candidate == path.end() || component != *candidate) return false;
        ++candidate;
    }
    return true;
}

void CheckPath(const std::filesystem::path& root, const std::filesystem::path& path) {
    if (!Inside(root, std::filesystem::canonical(path))) Fail("Audit input escapes the selected directory.");
}

using Groups = std::map<Key, std::vector<std::filesystem::path>>;

Groups Discover(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> files;
    for (std::filesystem::recursive_directory_iterator entry(root), end; entry != end; ++entry) {
        CheckPath(root, entry->path());
        if (entry->is_symlink()) { entry.disable_recursion_pending(); continue; }
        if (!entry->is_regular_file()) continue;
        if (files.size() == MaxFiles) Fail("Audit exceeds the input file limit.");
        files.push_back(entry->path());
    }
    std::sort(files.begin(), files.end());
    Groups groups;
    for (const auto& path : files) {
        std::ifstream input(path, std::ios::binary);
        if (!input) Fail("Cannot read audit input.");
        gdcm::Reader reader;
        reader.SetStream(input);
        const bool read = reader.ReadSelectedTags(Tags());
        const auto& data = reader.GetFile().GetDataSet();
        if (!read && !data.FindDataElement(gdcm::Tag(0x0020, 0x000e))) continue;
        groups[GroupKey(data)].push_back(path);
    }
    return groups;
}

std::size_t Select(const std::vector<SeriesInfo>& entries, const std::string& id) {
    std::size_t selected = entries.size();
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (!id.empty() ? entries[index].id == id : entries[index].supported) {
            if (selected != entries.size()) Fail("Audit requires an explicit series selection.");
            selected = index;
        }
    }
    if (selected == entries.size() || !entries[selected].supported)
        Fail("Selected audit series is unavailable or unsupported.");
    return selected;
}

struct Slice {
    std::filesystem::path path;
    Key key;
    std::string instance;
    unsigned rows = 0, columns = 0, bits = 0, representation = 0;
    Vector3 position{}, axisX{}, axisY{};
    std::array<double, 2> spacing{};
    double slope = 0, intercept = 0;
    bool instanceMismatch = false;
};

Slice Metadata(const gdcm::File& file, DicomReadOptions options) {
    const auto& data = file.GetDataSet();
    if (Text(data, 0x0008, 0x0016) != CtStorage || Text(data, 0x0008, 0x0060) != "CT" ||
        (data.FindDataElement(gdcm::Tag(0x0028, 0x0008)) && Text(data, 0x0028, 0x0008) != "1") ||
        data.FindDataElement(gdcm::Tag(0x5200, 0x9229)) || data.FindDataElement(gdcm::Tag(0x5200, 0x9230)))
        Fail("Audit supports conventional single-frame CT only.");
    Slice slice;
    slice.key = GroupKey(data);
    slice.instance = Text(data, 0x0008, 0x0018);
    if (std::get<0>(slice.key).empty() || std::get<1>(slice.key).empty() ||
        std::get<2>(slice.key).empty() || slice.instance.empty()) Fail("Missing audit group metadata.");
    const auto storage = Text(file.GetHeader(), 0x0002, 0x0002);
    const auto instance = Text(file.GetHeader(), 0x0002, 0x0003);
    if (!storage.empty() && storage != CtStorage) Fail("Inconsistent audit storage metadata.");
    slice.instanceMismatch = !instance.empty() && instance != slice.instance;
    if (slice.instanceMismatch && !options.allowInstanceUidMismatch)
        Fail("Audit instance mismatch requires explicit compatibility permission.");
    slice.rows = Short<0x0028, 0x0010>(data);
    slice.columns = Short<0x0028, 0x0011>(data);
    slice.bits = Short<0x0028, 0x0101>(data);
    slice.representation = Short<0x0028, 0x0103>(data);
    if (!slice.rows || !slice.columns || Short<0x0028, 0x0002>(data) != 1 ||
        Short<0x0028, 0x0100>(data) != 16 || slice.bits < 12 || slice.bits > 16 ||
        Short<0x0028, 0x0102>(data) != slice.bits - 1 || slice.representation > 1)
        Fail("Unsupported audit sign or bit format.");
    const auto photometric = Text(data, 0x0028, 0x0004);
    if (photometric != "MONOCHROME1" && photometric != "MONOCHROME2")
        Fail("Unsupported audit photometric format.");
    slice.position = DecimalValues<3>(Text(data, 0x0020, 0x0032));
    const auto orientation = DecimalValues<6>(Text(data, 0x0020, 0x0037));
    std::copy_n(orientation.begin(), 3, slice.axisX.begin());
    std::copy_n(orientation.begin() + 3, 3, slice.axisY.begin());
    slice.spacing = DecimalValues<2>(Text(data, 0x0028, 0x0030));
    slice.slope = DecimalValues<1>(Text(data, 0x0028, 0x1053))[0];
    slice.intercept = DecimalValues<1>(Text(data, 0x0028, 0x1052))[0];
    const auto rescaleType = Text(data, 0x0028, 0x1054);
    if (slice.spacing[0] <= 0 || slice.spacing[1] <= 0 || slice.slope == 0 ||
        (!rescaleType.empty() && rescaleType != "HU") || data.FindDataElement(gdcm::Tag(0x0028, 0x3000)))
        Fail("Unsupported audit modality transform or spacing.");
    return slice;
}

bool Same(const Slice& left, const Slice& right) {
    return left.key == right.key && left.instance == right.instance && left.rows == right.rows &&
        left.columns == right.columns && left.bits == right.bits && left.representation == right.representation &&
        left.position == right.position && left.axisX == right.axisX && left.axisY == right.axisY &&
        left.spacing == right.spacing && left.slope == right.slope && left.intercept == right.intercept &&
        left.instanceMismatch == right.instanceMismatch;
}

double Dot(const Vector3& left, const Vector3& right) {
    double result = 0;
    for (std::size_t axis = 0; axis < 3; ++axis) result += left[axis] * right[axis];
    return result;
}

Vector3 Difference(const Vector3& left, const Vector3& right) {
    Vector3 result{};
    for (std::size_t axis = 0; axis < 3; ++axis) result[axis] = left[axis] - right[axis];
    return result;
}

struct Stats {
    std::size_t count = 0, nonfinite = 0;
    double minimum = std::numeric_limits<double>::infinity();
    double maximum = -std::numeric_limits<double>::infinity();
    double mean = 0;

    void Add(double value) {
        if (!std::isfinite(value)) { ++nonfinite; return; }
        ++count;
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
        mean += (value - mean) / static_cast<double>(count);
    }

    Json Report() const {
        return {{"finiteCount", count}, {"nonfiniteCount", nonfinite},
                {"min", count ? Json(minimum) : Json(nullptr)},
                {"max", count ? Json(maximum) : Json(nullptr)},
                {"mean", count ? Json(mean) : Json(nullptr)}};
    }
};

Json Matrix(const Affine4d& affine) {
    Json rows = Json::array();
    for (std::size_t row = 0; row < 4; ++row)
        rows.push_back({affine[row * 4], affine[row * 4 + 1], affine[row * 4 + 2], affine[row * 4 + 3]});
    return rows;
}

Json Compare(const std::filesystem::path& directory, const VolumeData& loaded,
             const std::vector<SeriesInfo>& entries, std::size_t selected, DicomReadOptions options) {
    const auto root = std::filesystem::canonical(directory);
    const auto groups = Discover(root);
    if (groups.size() != entries.size()) Fail("Independent discovery differs from the fresh catalog.");
    auto group = groups.begin();
    auto chosen = groups.end();
    for (std::size_t index = 0; index < entries.size(); ++index, ++group) {
        if (entries[index].id != "series-" + std::to_string(index + 1) ||
            entries[index].fileCount != group->second.size())
            Fail("Fresh catalog group ordering or membership changed.");
        if (index == selected) chosen = group;
    }
    if (chosen == groups.end() || chosen->second.size() < 2) Fail("Audit needs at least two slices.");
    std::vector<Slice> slices;
    std::set<std::string> instances;
    for (const auto& path : chosen->second) {
        CheckPath(root, path);
        std::ifstream input(path, std::ios::binary);
        if (!input) Fail("Cannot read audit header.");
        gdcm::Reader reader;
        reader.SetStream(input);
        if (!reader.ReadSelectedTags(Tags())) Fail("Cannot parse audit header.");
        auto slice = Metadata(reader.GetFile(), options);
        slice.path = path;
        if (slice.key != chosen->first || !instances.insert(slice.instance).second)
            Fail("Audit group membership or instance uniqueness failed.");
        slices.push_back(std::move(slice));
    }
    const Slice reference = slices.front();
    Vector3 normal{reference.axisX[1] * reference.axisY[2] - reference.axisX[2] * reference.axisY[1],
                   reference.axisX[2] * reference.axisY[0] - reference.axisX[0] * reference.axisY[2],
                   reference.axisX[0] * reference.axisY[1] - reference.axisX[1] * reference.axisY[0]};
    const double length = std::sqrt(Dot(normal, normal));
    if (!std::isfinite(length) || length == 0) Fail("Invalid audit slice normal.");
    for (auto& component : normal) component /= length;
    std::sort(slices.begin(), slices.end(), [&](const Slice& left, const Slice& right) {
        return Dot(Difference(left.position, reference.position), normal) <
               Dot(Difference(right.position, reference.position), normal);
    });
    const auto& first = slices.front();
    const std::size_t plane = static_cast<std::size_t>(first.columns) * first.rows;
    if (plane > MaxVoxels / slices.size() || loaded.shape.width != first.columns ||
        loaded.shape.height != first.rows || loaded.shape.depth != slices.size() ||
        loaded.voxels.size() != plane * slices.size())
        Fail("Selected group dimensions do not match the loaded grid.");
    ValidateGeometry(loaded);
    const double step = Dot(Difference(slices[1].position, first.position), normal);
    if (!std::isfinite(step) || step <= 1e-4) Fail("Invalid audit slice gap.");
    const std::array<double, 3> spacing{first.spacing[1], first.spacing[0], step};
    const std::array<double, 3> spacingTolerances{std::max(1e-5, spacing[0] * 1e-5),
        std::max(1e-5, spacing[1] * 1e-5), std::max(1e-5, spacing[2] * 1e-5)};
    Affine4d expectedAffine{};
    double originError = 0, directionError = 0, spacingError = 0;
    bool gridMatches = true;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        originError = std::max(originError, std::abs(loaded.origin[axis] - first.position[axis]));
        spacingError = std::max(spacingError, std::abs(loaded.spacing[axis] - spacing[axis]));
        gridMatches = gridMatches && std::abs(loaded.spacing[axis] - spacing[axis]) <= spacingTolerances[axis];
        const std::array<double, 3> direction{first.axisX[axis], first.axisY[axis], normal[axis]};
        for (std::size_t column = 0; column < 3; ++column) {
            expectedAffine[axis * 4 + column] = direction[column] * spacing[column];
            directionError = std::max(directionError, std::abs(loaded.direction[axis * 3 + column] - direction[column]));
        }
        expectedAffine[axis * 4 + 3] = first.position[axis];
    }
    expectedAffine[15] = 1;
    const auto loadedAffine = VoxelToPatientAffine(loaded);
    double affineError = 0, positionError = 0, gapError = 0, lateralDrift = 0, orientationError = 0;
    double accumulatedSpacingError = 0, inPlaneSpacingError = 0, previous = 0;
    bool ascending = true;
    Stats gaps, slopes, intercepts;
    Json projections = Json::array();
    for (std::size_t index = 0; index < expectedAffine.size(); ++index)
        affineError = std::max(affineError, std::abs(loadedAffine[index] - expectedAffine[index]));
    for (std::size_t index = 0; index < slices.size(); ++index) {
        const auto& slice = slices[index];
        if (slice.rows != first.rows || slice.columns != first.columns)
            Fail("Independent slice dimensions are inconsistent.");
        const auto delta = Difference(slice.position, first.position);
        const double projection = Dot(delta, normal);
        projections.push_back(projection);
        if (index) {
            const double gap = projection - previous;
            ascending = ascending && gap > 1e-4;
            gaps.Add(gap);
            gapError = std::max(gapError, std::abs(gap - step));
        }
        previous = projection;
        accumulatedSpacingError = std::max(accumulatedSpacingError, std::abs(projection - static_cast<double>(index) * step));
        Vector3 lateral{};
        double positionSquared = 0;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            lateral[axis] = delta[axis] - normal[axis] * projection;
            const double residual = loadedAffine[axis * 4 + 3] + loadedAffine[axis * 4 + 2] *
                static_cast<double>(index) - slice.position[axis];
            positionSquared += residual * residual;
            orientationError = std::max({orientationError, std::abs(slice.axisX[axis] - first.axisX[axis]),
                                        std::abs(slice.axisY[axis] - first.axisY[axis])});
        }
        orientationError = std::max({orientationError, std::abs(Dot(slice.axisX, slice.axisX) - 1),
                                    std::abs(Dot(slice.axisY, slice.axisY) - 1), std::abs(Dot(slice.axisX, slice.axisY))});
        for (std::size_t axis = 0; axis < 2; ++axis) {
            const double error = std::abs(slice.spacing[axis] - first.spacing[axis]);
            inPlaneSpacingError = std::max(inPlaneSpacingError, error);
            gridMatches = gridMatches && error <= std::max(1e-5, first.spacing[axis] * 1e-5);
        }
        lateralDrift = std::max(lateralDrift, std::sqrt(Dot(lateral, lateral)));
        positionError = std::max(positionError, std::sqrt(positionSquared));
        slopes.Add(slice.slope);
        intercepts.Add(slice.intercept);
    }
    const double gapTolerance = std::max(0.01, step * 0.001);
    const bool geometryPassed = gridMatches && originError <= 0.01 && directionError <= 1e-4 &&
        orientationError <= 1e-4 && ascending && gapError <= gapTolerance &&
        accumulatedSpacingError <= gapTolerance && lateralDrift <= 0.01 && positionError <= gapTolerance + 0.01;

    Stats originalHu, roundedHu, loadedHu;
    std::size_t compared = 0, mismatches = 0, nonfinite = 0, signedSlices = 0, unsignedSlices = 0;
    std::size_t encapsulatedSlices = 0, instanceMismatches = 0;
    double maxAbsError = 0, maxAbsDoubleError = 0;
    std::array<std::size_t, 5> bitCounts{};
    Json samples = Json::array();
    for (std::size_t sliceIndex = 0; sliceIndex < slices.size(); ++sliceIndex) {
        const auto& slice = slices[sliceIndex];
        CheckPath(root, slice.path);
        std::ifstream input(slice.path, std::ios::binary);
        if (!input) Fail("Cannot read audit pixels.");
        gdcm::ImageReader reader;
        reader.SetStream(input);
        if (!reader.Read()) Fail("Independent audit codec could not decode a slice.");
        if (!Same(Metadata(reader.GetFile(), options), slice)) Fail("Audit metadata changed during decoding.");
        const auto& image = reader.GetImage();
        const auto& format = image.GetPixelFormat();
        if (image.GetNumberOfDimensions() < 2 || image.GetDimension(0) != slice.columns ||
            image.GetDimension(1) != slice.rows ||
            (image.GetNumberOfDimensions() > 2 && image.GetDimension(2) != 1) ||
            format.GetSamplesPerPixel() != 1 || format.GetBitsAllocated() != 16 ||
            format.GetBitsStored() != slice.bits || format.GetHighBit() != slice.bits - 1 ||
            format.GetPixelRepresentation() != slice.representation || image.GetBufferLength() != plane * 2)
            Fail("Independent decoded pixels have an unsupported format.");
        std::vector<char> buffer(plane * 2);
        if (!image.GetBuffer(buffer.data())) Fail("Independent audit pixel decompression failed.");
        if (image.GetTransferSyntax().IsEncapsulated()) ++encapsulatedSlices;
        if (slice.representation) ++signedSlices; else ++unsignedSlices;
        if (slice.instanceMismatch) ++instanceMismatches;
        ++bitCounts[slice.bits - 12];
        const std::uint32_t modulus = std::uint32_t{1} << slice.bits;
        const std::uint32_t mask = modulus - 1;
        Stats sliceOriginal, sliceLoaded;
        std::size_t sliceMismatches = 0;
        double sliceMaxError = 0;
        for (std::size_t pixel = 0; pixel < plane; ++pixel) {
            std::uint16_t word = 0;
            std::memcpy(&word, buffer.data() + pixel * sizeof(word), sizeof(word));
            const std::uint32_t masked = word & mask;
            const std::int32_t stored = slice.representation && masked >= modulus / 2 ?
                static_cast<std::int32_t>(masked) - static_cast<std::int32_t>(modulus) :
                static_cast<std::int32_t>(masked);
            const double expected = static_cast<double>(stored) * slice.slope + slice.intercept;
            if (!std::isfinite(expected) || std::abs(expected) > std::numeric_limits<float>::max())
                Fail("Independent HU values exceed the loaded numeric range.");
            const float rounded = static_cast<float>(expected);
            const float actual = loaded.voxels[sliceIndex * plane + pixel];
            ++compared;
            originalHu.Add(expected);
            roundedHu.Add(rounded);
            loadedHu.Add(actual);
            sliceOriginal.Add(expected);
            sliceLoaded.Add(actual);
            if (!std::isfinite(actual)) { ++nonfinite; ++mismatches; ++sliceMismatches; continue; }
            const double error = std::abs(static_cast<double>(actual) - rounded);
            maxAbsError = std::max(maxAbsError, error);
            sliceMaxError = std::max(sliceMaxError, error);
            maxAbsDoubleError = std::max(maxAbsDoubleError, std::abs(static_cast<double>(actual) - expected));
            if (actual != rounded) { ++mismatches; ++sliceMismatches; }
        }
        if (sliceIndex == 0 || sliceIndex == slices.size() / 2 || sliceIndex + 1 == slices.size())
            samples.push_back({{"sliceIndex", sliceIndex}, {"projectionRelativeMm", projections[sliceIndex]},
                               {"originalHu", sliceOriginal.Report()}, {"loadedHu", sliceLoaded.Report()},
                               {"mismatchCount", sliceMismatches}, {"maxAbsErrorHu", sliceMaxError}});
    }
    const auto unsupported = static_cast<std::size_t>(std::count_if(entries.begin(), entries.end(),
        [](const SeriesInfo& entry) { return !entry.supported; }));
    Json warnings = Json::array();
    if (instanceMismatches) warnings.push_back("file-meta-instance-mismatch-explicitly-allowed");
    if (unsupported) warnings.push_back("unsupported-catalog-series-not-audited");
    return {{"schemaVersion", 1}, {"scope", "numerical-and-geometric-self-audit"},
            {"independentExternalTool", false}, {"clinicalValidation", false},
            {"sharedDependencies", {"GDCM decoding", "MedGL geometry API"}},
            {"passed", geometryPassed && mismatches == 0},
            {"selection", {{"method", "fresh-catalog-lexicographic-study-series-frame"},
                           {"catalogOrdinal", selected + 1}, {"groupCount", entries.size()},
                           {"groupOrderAndCountsVerified", true}, {"unsupportedSeriesCount", unsupported}}},
            {"dimensions", {first.columns, first.rows, slices.size()}}, {"spacingMm", spacing},
            {"direction", {{first.axisX[0], first.axisY[0], normal[0]},
                           {first.axisX[1], first.axisY[1], normal[1]},
                           {first.axisX[2], first.axisY[2], normal[2]}}},
            {"geometry", {{"passed", geometryPassed}, {"coordinateSystem", "LPS"},
                          {"affineLayout", "row-major; voxel centers; millimeters"},
                          {"originalVoxelToPatientAffine", Matrix(expectedAffine)},
                          {"loadedVoxelToPatientAffine", Matrix(loadedAffine)},
                          {"sortedIppProjectionsRelativeMm", projections}, {"strictlyAscending", ascending},
                          {"sliceGapsMm", gaps.Report()}, {"maxGapSpacingErrorMm", gapError},
                          {"maxAccumulatedSpacingErrorMm", accumulatedSpacingError},
                          {"maxInPlaneSpacingErrorMm", inPlaneSpacingError}, {"maxLateralDriftMm", lateralDrift},
                          {"maxOrientationComponentError", orientationError}, {"maxOriginErrorMm", originError},
                          {"maxDirectionComponentError", directionError}, {"maxSpacingErrorMm", spacingError},
                          {"maxAffineElementError", affineError}, {"maxSlicePositionErrorMm", positionError},
                          {"gapToleranceMm", gapTolerance}, {"lateralToleranceMm", 0.01},
                          {"originToleranceMm", 0.01}, {"spacingTolerancesMm", spacingTolerances},
                          {"inPlaneSpacingTolerancesMm", {spacingTolerances[0], spacingTolerances[1]}},
                          {"slicePositionToleranceMm", gapTolerance + 0.01},
                          {"affineElementErrorDiagnosticOnly", true},
                          {"orientationTolerance", 1e-4}}},
            {"originalHu", originalHu.Report()}, {"originalHuRoundedToFloat", roundedHu.Report()},
            {"loadedHu", loadedHu.Report()},
            {"comparison", {{"pixelsCompared", compared}, {"mismatchCount", mismatches},
                            {"nonfiniteLoadedCount", nonfinite}, {"maxAbsErrorHu", nonfinite ? Json(nullptr) : Json(maxAbsError)},
                            {"maxFiniteAbsErrorHu", maxAbsError}, {"maxFiniteAbsErrorAgainstDoubleHu", maxAbsDoubleError},
                            {"criterion", "exact equality after one double modality transform rounded to float"}}},
            {"rescale", {{"slope", slopes.Report()}, {"intercept", intercepts.Report()}, {"applications", 1},
                         {"source", "original dataset RescaleSlope and RescaleIntercept tags"}}},
            {"pixelFormat", {{"signedSliceCount", signedSlices}, {"unsignedSliceCount", unsignedSlices},
                             {"bitsStored12Through16Counts", bitCounts}, {"unsupportedSelectedSliceCount", 0}}},
            {"codec", {{"decodedSliceCount", slices.size()}, {"encapsulatedSliceCount", encapsulatedSlices},
                       {"uncompressedSliceCount", slices.size() - encapsulatedSlices}, {"unsupportedSelectedSliceCount", 0}}},
            {"compatibility", {{"allowInstanceUidMismatch", options.allowInstanceUidMismatch},
                               {"instanceMismatchSliceCount", instanceMismatches}}},
            {"selectedSliceStatistics", samples}, {"warnings", warnings}};
}

}

nlohmann::json AuditDicomVolume(const std::filesystem::path& root, const std::string& seriesId,
                              DicomReadOptions options) {
    try {
        DicomCatalog catalog(root, options);
        const auto entries = catalog.Scan();
        const auto selected = Select(entries, seriesId);
        const auto loaded = catalog.Load(entries[selected].id);
        return Compare(root, loaded, entries, selected, options);
    } catch (const AuditError&) { throw; }
    catch (...) { Fail("Offline DICOM audit could not complete."); }
}

nlohmann::json CheckDicomVolume(const std::filesystem::path& root, const VolumeData& loaded,
                              const std::string& seriesId, DicomReadOptions options) {
    try {
        DicomCatalog catalog(root, options);
        const auto entries = catalog.Scan();
        return Compare(root, loaded, entries, Select(entries, seriesId), options);
    } catch (const AuditError&) { throw; }
    catch (...) { Fail("Offline DICOM audit could not complete."); }
}

}