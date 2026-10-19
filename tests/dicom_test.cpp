#include "medgl/dicom.hpp"

#include <gdcmAttribute.h>
#include <gdcmImageChangeTransferSyntax.h>
#include <gdcmImageReader.h>
#include <gdcmImageWriter.h>
#include <gdcmReader.h>
#include <gdcmUIDGenerator.h>
#include <gdcmWriter.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void Near(float actual, float expected, const char* message) {
    Require(std::abs(actual - expected) < 1e-4f, message);
}

void Reject(const std::function<void()>& action) {
    try { action(); }
    catch (const std::exception& exception) {
        const std::string message = exception.what();
        Require(message.find("slice-") == std::string::npos &&
                message.find("SecretPatient") == std::string::npos &&
                message.find(".dcm") == std::string::npos, "Error leaked identifying data");
        return;
    }
    throw std::runtime_error("Expected rejection");
}

void Text(gdcm::DataSet& data, std::uint16_t group, std::uint16_t element,
          gdcm::VR vr, std::string value) {
    if (value.size() % 2) value.push_back(vr == gdcm::VR::UI ? '\0' : ' ');
    gdcm::DataElement entry(gdcm::Tag(group, element));
    entry.SetVR(vr);
    entry.SetByteValue(value.data(), static_cast<std::uint32_t>(value.size()));
    data.Replace(entry);
}

template <std::uint16_t Group, std::uint16_t Element>
void Short(gdcm::DataSet& data, unsigned short value) {
    gdcm::Attribute<Group, Element> attribute;
    attribute.SetValue(value);
    data.Replace(attribute.GetAsDataElement());
}

std::vector<std::filesystem::path> Paths(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(root))
        if (entry.is_regular_file()) files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    return files;
}

void Rewrite(const std::filesystem::path& path, const std::function<void(gdcm::DataSet&)>& edit) {
    gdcm::Reader reader;
    {
        std::ifstream input(path, std::ios::binary);
        reader.SetStream(input);
        Require(reader.Read(), "Fixture read failed");
    }
    edit(reader.GetFile().GetDataSet());
    gdcm::Writer writer;
    writer.SetFile(reader.GetFile());
    std::ofstream output(path, std::ios::binary);
    writer.SetStream(output);
    Require(writer.Write(), "Fixture rewrite failed");
}

void PatchMetaIdentifier(const std::filesystem::path& path, unsigned short element, bool malformed = false) {
    gdcm::Reader reader;
    {
        std::ifstream input(path, std::ios::binary);
        reader.SetStream(input);
        Require(reader.Read(), "Fixture meta read failed");
    }
    const auto* value = reader.GetFile().GetHeader().GetDataElement(gdcm::Tag(0x0002, element)).GetByteValue();
    Require(value != nullptr, "Fixture meta UID is missing");
    std::string identifier(value->GetPointer(), value->GetLength());
    const auto digit = identifier.find_last_of("0123456789");
    Require(digit != std::string::npos, "Fixture UID has no numeric component");
    std::ifstream input(path, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();
    const auto offset = bytes.find(identifier);
    Require(offset != std::string::npos, "Fixture meta UID was not located");
    bytes[offset + digit] = malformed ? 'X' : (identifier[digit] == '9' ? '8' : '9');
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    Require(static_cast<bool>(output), "Fixture UID patch failed");
}

std::filesystem::path Pair(const std::filesystem::path& root, const std::string& name,
                           const std::vector<std::filesystem::path>& files) {
    const auto directory = root / name;
    std::filesystem::create_directories(directory);
    std::filesystem::copy_file(files[0], directory / "first.dcm");
    std::filesystem::copy_file(files[37], directory / "second.dcm");
    return directory;
}

float At(const medgl::VolumeData& volume, unsigned column, unsigned row, unsigned slice) {
    return volume.voxels[(static_cast<std::size_t>(slice) * volume.shape.height + row) *
                         volume.shape.width + column];
}

void Invalid(const std::filesystem::path& root, const std::string& name,
             const std::vector<std::filesystem::path>& files,
             const std::function<void(gdcm::DataSet&)>& edit) {
    const auto directory = Pair(root, name, files);
    Rewrite(directory / "second.dcm", edit);
    medgl::DicomCatalog catalog(directory);
    const auto entries = catalog.Scan();
    Require(!entries.empty(), "Invalid fixture was not discovered");
    for (const auto& entry : entries) {
        Require(!entry.supported && !entry.reason.empty(), "Invalid series reported as supported");
        Reject([&] { catalog.Load(entry.id); });
    }
}

void PackedPixels(const std::filesystem::path& path, unsigned bits, bool signedPixels,
                  bool extended, bool compressed, bool bigEndian = false) {
    gdcm::ImageReader reader;
    {
        std::ifstream input(path, std::ios::binary);
        reader.SetStream(input);
        Require(reader.Read(), "Pixel fixture read failed");
    }
    gdcm::ImageWriter writer;
    writer.SetFile(reader.GetFile());
    auto& image = writer.GetImage();
    image = reader.GetImage();
    image.SetPixelFormat(gdcm::PixelFormat(1, 16, static_cast<unsigned short>(bits),
                                         static_cast<unsigned short>(bits - 1), signedPixels ? 1 : 0));
    image.SetSlope(2);
    image.SetIntercept(-1000);
    const std::uint32_t range = 1u << bits;
    const std::int32_t values[4] = {signedPixels ? -1 : 1,
        signedPixels ? -static_cast<std::int32_t>(range / 2) : static_cast<std::int32_t>(range - 1),
        0, static_cast<std::int32_t>(range / 2 - 1)};
    std::vector<std::uint16_t> pixels(64 * 64);
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        const auto value = values[index % 4];
        pixels[index] = static_cast<std::uint16_t>(extended ? value : value & (range - 1));
    }
    gdcm::DataElement data(gdcm::Tag(0x7fe0, 0x0010));
    data.SetVR(gdcm::VR::OW);
    data.SetByteValue(reinterpret_cast<const char*>(pixels.data()),
                      static_cast<std::uint32_t>(pixels.size() * sizeof(std::uint16_t)));
    image.SetTransferSyntax(gdcm::TransferSyntax::ExplicitVRLittleEndian);
    image.SetDataElement(data);
    if (compressed) {
        gdcm::ImageChangeTransferSyntax converter;
        converter.SetInput(image);
        converter.SetTransferSyntax(gdcm::TransferSyntax::RLELossless);
        Require(converter.Change(), "RLE fixture compression failed");
        image = converter.GetOutput();
    } else if (bigEndian) {
        gdcm::ImageChangeTransferSyntax converter;
        converter.SetInput(image);
        converter.SetTransferSyntax(gdcm::TransferSyntax::ExplicitVRBigEndian);
        Require(converter.Change(), "Big-endian fixture conversion failed");
        image = converter.GetOutput();
    }
    std::ofstream output(path, std::ios::binary);
    writer.SetStream(output);
    Require(writer.Write(), "Pixel fixture write failed");
    output.close();
    gdcm::Reader verify;
    std::ifstream input(path, std::ios::binary);
    verify.SetStream(input);
    Require(verify.Read(), "Pixel fixture verification failed");
    const auto expected = compressed ? gdcm::TransferSyntax::RLELossless :
        (bigEndian ? gdcm::TransferSyntax::ExplicitVRBigEndian : gdcm::TransferSyntax::ExplicitVRLittleEndian);
    Require(verify.GetFile().GetHeader().GetDataSetTransferSyntax() == expected,
            "Pixel fixture transfer syntax differs from the intended decode path");
}

void CheckPixelValues(const medgl::VolumeData& pixels, unsigned bits, bool signedPixels) {
    const float range = static_cast<float>(1u << bits);
    for (unsigned slice = 0; slice < 2; ++slice) {
        Near(At(pixels, 0, 0, slice), signedPixels ? -1002.f : -998.f, "Pixel sign / rescale");
        Near(At(pixels, 1, 0, slice), signedPixels ? -range - 1000 : (range - 1) * 2 - 1000, "Pixel extrema");
        Near(At(pixels, 2, 0, slice), -1000, "Pixel zero");
        Near(At(pixels, 3, 0, slice), (range / 2 - 1) * 2 - 1000, "Pixel positive maximum");
    }
}

void Run(const std::filesystem::path& root) {
    const auto phantom = root / "phantom";
    medgl::WriteSyntheticCt(phantom);
    const auto files = Paths(phantom);
    Require(files.size() == 64, "Synthetic file count");
    {
        gdcm::Reader reader;
        std::ifstream input(files.front(), std::ios::binary);
        reader.SetStream(input);
        Require(reader.Read(), "Shuffled fixture read");
        const auto* bytes = reader.GetFile().GetDataSet().GetDataElement(gdcm::Tag(0x0020, 0x0032)).GetByteValue();
        Require(bytes != nullptr, "Synthetic IPP missing");
        const std::string position(bytes->GetPointer(), bytes->GetLength());
        Require(position.find("4.5") != std::string::npos,
                "Filenames were not shuffled relative to geometry");
    }
    medgl::DicomCatalog catalog(phantom);
    Reject([&] { catalog.Load("missing"); });
    const auto entries = catalog.Scan();
    for (const auto& entry : entries)
        if (!entry.supported) std::cerr << "Synthetic scan rejection: " << entry.reason << '\n';
    Require(entries.size() == 1 && entries[0].supported && entries[0].fileCount == 64,
            "Synthetic CT catalog result");
    Require(entries[0].id == "series-1" && entries[0].reason.empty(), "Opaque series ID");
    Require(catalog.Scan()[0].id == entries[0].id, "Series ID changed on rescan");
    const auto volume = catalog.Load(entries[0].id);
    Require(volume.shape.width == 64 && volume.shape.height == 64 && volume.shape.depth == 64 &&
            volume.voxels.size() == 64 * 64 * 64, "Volume shape or x-fast storage");
    Near(volume.spacing[0], 1, "Spacing X");
    Near(volume.spacing[1], 1, "Spacing Y");
    Near(volume.spacing[2], 1.5f, "Spacing Z");
    for (std::size_t axis = 0; axis < 3; ++axis) Near(volume.origin[axis], 0, "Sorted origin");
    for (std::size_t index = 0; index < 9; ++index)
        Near(volume.direction[index], index % 4 == 0 ? 1.f : 0.f, "Direction columns");
    Near(At(volume, 0, 0, 0), -1000, "Air HU: rescale applied once");
    Near(At(volume, 31, 30, 32), 40, "Soft tissue HU");
    Near(At(volume, 42, 25, 36), 300, "Asymmetric lesion HU / x-fast indexing");
    Near(At(volume, 22, 35, 27), -800, "Asymmetric cavity HU / slice ordering");
    Near(At(volume, 55, 30, 32), 700, "Shell HU");
    Near(At(volume, 25, 42, 36), 40, "Transposed lesion must not match");
    {
        std::ofstream text(phantom / "not-dicom.txt");
        text << "Not a DICOM file";
    }
    Require(catalog.Scan().size() == 1 && catalog.Scan()[0].fileCount == 64, "Non-DICOM file was cataloged");
    const auto pair = Pair(root, "two-slice", files);
    medgl::DicomCatalog pairCatalog(pair);
    Require(pairCatalog.Scan()[0].supported, "Two-slice series rejected");
    const auto pairVolume = pairCatalog.Load("series-1");
    Require(pairVolume.shape.depth == 2, "Two-slice load failed");
    Near(pairVolume.spacing[2], 1.5f, "Two-slice spacing");
    Near(pairVolume.origin[2], 4.5f, "Two-slice sorted origin");

    const auto mismatch = Pair(root, "meta-instance-mismatch", files);
    for (const auto& path : Paths(mismatch)) PatchMetaIdentifier(path, 0x0003);
    medgl::DicomCatalog strictMismatch(mismatch);
    Require(!strictMismatch.Scan()[0].supported, "Default policy accepted inconsistent instance UIDs");
    Reject([&] { strictMismatch.Load("series-1"); });
    const medgl::DicomReadOptions compatibility{true};
    medgl::DicomCatalog compatibleMismatch(mismatch, compatibility);
    const auto compatibleEntries = compatibleMismatch.Scan();
    Require(compatibleEntries[0].supported && !compatibleEntries[0].warning.empty(), "Compatibility policy or warning missing");
    const auto compatibleVolume = compatibleMismatch.Load("series-1");
    Require(compatibleVolume.voxels == pairVolume.voxels, "Compatibility policy changed decoded voxels");
    Require(!strictMismatch.Scan()[0].supported, "Compatibility load rewrote original fixture");
    const auto wrongStorage = Pair(root, "meta-class-mismatch", files);
    PatchMetaIdentifier(wrongStorage / "first.dcm", 0x0002);
    medgl::DicomCatalog compatibleClass(wrongStorage, compatibility);
    Require(!compatibleClass.Scan()[0].supported, "Compatibility policy accepted a class mismatch");
    Reject([&] { compatibleClass.Load("series-1"); });
    const auto malformedInstance = Pair(root, "meta-instance-malformed", files);
    PatchMetaIdentifier(malformedInstance / "first.dcm", 0x0003, true);
    medgl::DicomCatalog compatibleMalformed(malformedInstance, compatibility);
    Require(!compatibleMalformed.Scan()[0].supported, "Compatibility policy accepted malformed UID");
    Reject([&] { compatibleMalformed.Load("series-1"); });
    Rewrite(mismatch / "second.dcm", [](gdcm::DataSet& data) { Text(data, 0x0020, 0x0032, gdcm::VR::DS, "0\\0\\4.5"); });
    Require(!compatibleMismatch.Scan()[0].supported, "Compatibility policy accepted duplicate geometry");
    Reject([&] { compatibleMismatch.Load("series-1"); });

    const auto oblique = Pair(root, "oriented", files);
    for (const auto& path : Paths(oblique)) PackedPixels(path, 12, true, false, false);
    Rewrite(oblique / "first.dcm", [](gdcm::DataSet& data) {
        Text(data, 0x0020, 0x0032, gdcm::VR::DS, "7\\10\\20");
        Text(data, 0x0020, 0x0037, gdcm::VR::DS, "0\\1\\0\\0\\0\\1");
        Text(data, 0x0028, 0x0030, gdcm::VR::DS, "2\\0.5");
        Text(data, 0x0028, 0x0004, gdcm::VR::CS, "MONOCHROME1");
        Text(data, 0x0028, 0x1053, gdcm::VR::DS, "-2");
    });
    Rewrite(oblique / "second.dcm", [](gdcm::DataSet& data) {
        Text(data, 0x0020, 0x0032, gdcm::VR::DS, "5.5\\10\\20");
        Text(data, 0x0020, 0x0037, gdcm::VR::DS, "0\\1\\0\\0\\0\\1");
        Text(data, 0x0028, 0x0030, gdcm::VR::DS, "2\\0.5");
        Text(data, 0x0028, 0x0004, gdcm::VR::CS, "MONOCHROME1");
        Text(data, 0x0028, 0x1053, gdcm::VR::DS, "-2");
    });
    medgl::DicomCatalog orientedCatalog(oblique);
    Require(orientedCatalog.Scan()[0].supported, "Oriented series rejected");
    const auto oriented = orientedCatalog.Load("series-1");
    const float direction[9] = {0, 0, 1, 1, 0, 0, 0, 1, 0};
    for (std::size_t index = 0; index < 9; ++index) Near(oriented.direction[index], direction[index], "Nonidentity direction");
    Near(oriented.origin[0], 5.5f, "Oriented sort X");
    Near(oriented.origin[1], 10, "Oriented origin Y");
    Near(oriented.origin[2], 20, "Oriented origin Z");
    Near(oriented.spacing[0], 0.5f, "PixelSpacing column order");
    Near(oriented.spacing[1], 2, "PixelSpacing row order");
    Near(oriented.spacing[2], 1.5f, "Oriented slice spacing");
    Near(oriented.voxels[0], -998, "MONOCHROME1 and negative slope must retain quantitative HU");
    Near(oriented.voxels[1], 3096, "Negative slope must be applied once");

    const auto one = root / "one-slice";
    std::filesystem::create_directories(one);
    std::filesystem::copy_file(files[0], one / "single.dcm");
    medgl::DicomCatalog singleCatalog(one);
    Require(!singleCatalog.Scan()[0].supported, "Single slice was supported");
    Reject([&] { singleCatalog.Load("series-1"); });

    Invalid(root, "missing-position", files, [](gdcm::DataSet& data) { data.Remove(gdcm::Tag(0x0020, 0x0032)); });
    Invalid(root, "missing-orientation", files, [](gdcm::DataSet& data) { data.Remove(gdcm::Tag(0x0020, 0x0037)); });
    Invalid(root, "missing-spacing", files, [](gdcm::DataSet& data) { data.Remove(gdcm::Tag(0x0028, 0x0030)); });
    Invalid(root, "missing-rows", files, [](gdcm::DataSet& data) { data.Remove(gdcm::Tag(0x0028, 0x0010)); });
    Invalid(root, "missing-columns", files, [](gdcm::DataSet& data) { data.Remove(gdcm::Tag(0x0028, 0x0011)); });
    Invalid(root, "missing-frame", files, [](gdcm::DataSet& data) { data.Remove(gdcm::Tag(0x0020, 0x0052)); });
    Invalid(root, "missing-slope", files, [](gdcm::DataSet& data) { data.Remove(gdcm::Tag(0x0028, 0x1053)); });
    Invalid(root, "missing-intercept", files, [](gdcm::DataSet& data) { data.Remove(gdcm::Tag(0x0028, 0x1052)); });
    Invalid(root, "zero-slope", files, [](gdcm::DataSet& data) { Text(data, 0x0028, 0x1053, gdcm::VR::DS, "0"); });
    Invalid(root, "bad-sign", files, [](gdcm::DataSet& data) { Text(data, 0x0028, 0x1053, gdcm::VR::DS, "+-2"); });
    Invalid(root, "gantry-tilt", files, [](gdcm::DataSet& data) { Text(data, 0x0018, 0x1120, gdcm::VR::DS, "10"); });
    Invalid(root, "nonfinite", files, [](gdcm::DataSet& data) { Text(data, 0x0020, 0x0032, gdcm::VR::DS, "0\\0\\nan"); });
    Invalid(root, "high-bit", files, [](gdcm::DataSet& data) { Short<0x0028, 0x0102>(data, 14); });
    Invalid(root, "localizer", files, [](gdcm::DataSet& data) { Text(data, 0x0008, 0x0008, gdcm::VR::CS, "ORIGINAL\\PRIMARY\\LOCALIZER"); });
    Invalid(root, "mixed-type", files, [](gdcm::DataSet& data) { Text(data, 0x0008, 0x0008, gdcm::VR::CS, "DERIVED\\SECONDARY\\AXIAL"); });
    Invalid(root, "multienergy", files, [](gdcm::DataSet& data) { Text(data, 0x0018, 0x9361, gdcm::VR::CS, "YES"); });
    Invalid(root, "enhanced", files, [](gdcm::DataSet& data) { Text(data, 0x0008, 0x0016, gdcm::VR::UI, "1.2.840.10008.5.1.4.1.1.2.1"); });
    Invalid(root, "multiframe", files, [](gdcm::DataSet& data) { Text(data, 0x0028, 0x0008, gdcm::VR::IS, "2"); });
    Invalid(root, "non-ct", files, [](gdcm::DataSet& data) { Text(data, 0x0008, 0x0060, gdcm::VR::CS, "MR"); });
    Invalid(root, "drift", files, [](gdcm::DataSet& data) { Text(data, 0x0020, 0x0032, gdcm::VR::DS, "1\\0\\6"); });
    Invalid(root, "duplicate-position", files, [](gdcm::DataSet& data) { Text(data, 0x0020, 0x0032, gdcm::VR::DS, "0\\0\\4.5"); });
    Invalid(root, "bad-iop", files, [](gdcm::DataSet& data) { Text(data, 0x0020, 0x0037, gdcm::VR::DS, "1\\0\\0\\1\\0\\0"); });
    Invalid(root, "different-iop", files, [](gdcm::DataSet& data) { Text(data, 0x0020, 0x0037, gdcm::VR::DS, "0\\1\\0\\1\\0\\0"); });
    Invalid(root, "different-spacing", files, [](gdcm::DataSet& data) { Text(data, 0x0028, 0x0030, gdcm::VR::DS, "2\\1"); });
    Invalid(root, "nonpositive-spacing", files, [](gdcm::DataSet& data) { Text(data, 0x0028, 0x0030, gdcm::VR::DS, "0\\1"); });
    Invalid(root, "different-dimensions", files, [](gdcm::DataSet& data) { Short<0x0028, 0x0011>(data, 63); });
    Invalid(root, "rgb", files, [](gdcm::DataSet& data) { Short<0x0028, 0x0002>(data, 3); });
    Invalid(root, "bits-allocated", files, [](gdcm::DataSet& data) { Short<0x0028, 0x0100>(data, 8); });
    Invalid(root, "bits-stored", files, [](gdcm::DataSet& data) { Short<0x0028, 0x0101>(data, 11); });
    Invalid(root, "representation", files, [](gdcm::DataSet& data) { Short<0x0028, 0x0103>(data, 2); });
    Invalid(root, "hu-overflow", files, [](gdcm::DataSet& data) { Text(data, 0x0028, 0x1053, gdcm::VR::DS, "1e308"); });

    const auto irregular = Pair(root, "irregular", files);
    std::filesystem::copy_file(files[20], irregular / "third.dcm");
    medgl::DicomCatalog irregularCatalog(irregular);
    Require(!irregularCatalog.Scan()[0].supported, "Irregular spacing was supported");
    Reject([&] { irregularCatalog.Load("series-1"); });
    const auto duplicates = Pair(root, "duplicate-instance", files);
    std::filesystem::copy_file(files[0], duplicates / "duplicate.dcm");
    medgl::DicomCatalog duplicateCatalog(duplicates);
    Require(!duplicateCatalog.Scan()[0].supported, "Duplicate instance was supported");

    const auto oversized = Pair(root, "voxel-limit", files);
    for (const auto& path : Paths(oversized)) Rewrite(path, [](gdcm::DataSet& data) {
        Short<0x0028, 0x0010>(data, 65535);
        Short<0x0028, 0x0011>(data, 65535);
    });
    medgl::DicomCatalog oversizedCatalog(oversized);
    Require(!oversizedCatalog.Scan()[0].supported, "Voxel limit not enforced");
    Reject([&] { oversizedCatalog.Load("series-1"); });

    const auto grouped = root / "groups";
    Pair(grouped, "first", files);
    gdcm::UIDGenerator generator;
    medgl::DicomCatalog groupCatalog(grouped);
    const auto originalId = groupCatalog.Scan()[0].id;
    const auto groupB = Pair(grouped, "second", files);
    const std::string otherFrame = generator.Generate();
    for (const auto& path : Paths(groupB)) Rewrite(path, [&](gdcm::DataSet& data) {
        Text(data, 0x0020, 0x0052, gdcm::VR::UI, otherFrame);
        Text(data, 0x0010, 0x0010, gdcm::VR::PN, "SecretPatient");
        Text(data, 0x0010, 0x0020, gdcm::VR::LO, "SecretPatient");
        Text(data, 0x0008, 0x103e, gdcm::VR::LO, "SecretPatient");
    });
    const auto groupEntries = groupCatalog.Scan();
    Require(groupEntries.size() == 2, "Frame UID groups were merged");
    Require(groupCatalog.Load(originalId).shape.depth == 2, "Original opaque ID lost after insertion");
    for (const auto& entry : groupEntries) {
        Require(entry.supported, "Frame group unsupported");
        Require(entry.description == "CT series" && entry.id.find("series-") == 0 && entry.reason.empty(),
                "Public metadata leaked identifiers");
    }
    const auto groupC = Pair(grouped, "third", files);
    const std::string otherStudy = generator.Generate();
    for (const auto& path : Paths(groupC)) Rewrite(path, [&](gdcm::DataSet& data) {
        Text(data, 0x0020, 0x000d, gdcm::VR::UI, otherStudy);
    });
    Require(groupCatalog.Scan().size() == 3, "Study UID groups were merged");
    const auto stableCount = groupCatalog.Load(originalId).voxels.size();
    Require(stableCount == 64 * 64 * 2, "Stable opaque ID points to wrong volume");

    const auto identity = Pair(root, "identity-rescale", files);
    for (const auto& path : Paths(identity)) Rewrite(path, [](gdcm::DataSet& data) {
        Text(data, 0x0028, 0x1053, gdcm::VR::DS, "1");
        Text(data, 0x0028, 0x1052, gdcm::VR::DS, "0");
    });
    medgl::DicomCatalog identityCatalog(identity);
    Require(identityCatalog.Scan()[0].supported, "Explicit identity rescale rejected");
    Near(identityCatalog.Load("series-1").voxels[0], 0, "Identity rescale");

    const auto variation = Pair(root, "slice-rescale", files);
    for (const auto& path : Paths(variation)) PackedPixels(path, 12, true, false, false);
    Rewrite(variation / "second.dcm", [](gdcm::DataSet& data) {
        Text(data, 0x0028, 0x1053, gdcm::VR::DS, "3");
        Text(data, 0x0028, 0x1052, gdcm::VR::DS, "5");
    });
    medgl::DicomCatalog variationCatalog(variation);
    Require(variationCatalog.Scan()[0].supported, "Per-slice rescale rejected");
    const auto varied = variationCatalog.Load("series-1");
    Near(At(varied, 0, 0, 0), -1002, "First slice rescale");
    Near(At(varied, 0, 0, 1), 2, "Second slice rescale");

    const auto changed = Pair(root, "changed", files);
    medgl::DicomCatalog changedCatalog(changed);
    Require(changedCatalog.Scan()[0].supported, "Changed fixture initialization");
    Rewrite(changed / "first.dcm", [&](gdcm::DataSet& data) {
        Text(data, 0x0020, 0x000e, gdcm::VR::UI, generator.Generate());
    });
    Reject([&] { changedCatalog.Load("series-1"); });

    const auto noPixels = Pair(root, "no-pixels", files);
    Rewrite(noPixels / "second.dcm", [](gdcm::DataSet& data) { data.Remove(gdcm::Tag(0x7fe0, 0x0010)); });
    medgl::DicomCatalog headerCatalog(noPixels);
    Require(headerCatalog.Scan()[0].supported, "Scan attempted pixel decoding");
    Reject([&] { headerCatalog.Load("series-1"); });

    for (unsigned bits = 12; bits <= 16; ++bits)
        for (const bool signedPixels : {false, true})
            for (const bool extended : {false, true}) {
                const auto name = "pixels-" + std::to_string(bits) + (signedPixels ? "-signed" : "-unsigned") +
                                  (extended ? "-extended" : "-masked");
                const auto directory = Pair(root, name, files);
                for (const auto& path : Paths(directory)) PackedPixels(path, bits, signedPixels, extended, false);
                medgl::DicomCatalog pixelCatalog(directory);
                Require(pixelCatalog.Scan()[0].supported, "Pixel series rejected");
                CheckPixelValues(pixelCatalog.Load("series-1"), bits, signedPixels);
            }

    for (const unsigned bits : {12u, 16u})
        for (const bool signedPixels : {false, true}) {
            const auto directory = Pair(root, "rle-" + std::to_string(bits) + (signedPixels ? "-signed" : "-unsigned"), files);
            for (const auto& path : Paths(directory)) PackedPixels(path, bits, signedPixels, true, true);
            medgl::DicomCatalog rleCatalog(directory);
            Require(rleCatalog.Scan()[0].supported, "Compressed series rejected");
            CheckPixelValues(rleCatalog.Load("series-1"), bits, signedPixels);
        }

    const auto bigEndian = Pair(root, "big-endian", files);
    for (const auto& path : Paths(bigEndian)) PackedPixels(path, 16, true, true, false, true);
    medgl::DicomCatalog endianCatalog(bigEndian);
    Require(endianCatalog.Scan()[0].supported, "Big-endian CT rejected");
    CheckPixelValues(endianCatalog.Load("series-1"), 16, true);

    const auto fileLimit = root / "file-limit";
    std::filesystem::create_directories(fileLimit);
    for (unsigned index = 0; index <= 10000; ++index) {
        std::ofstream file(fileLimit / (std::to_string(index) + ".txt"));
        Require(static_cast<bool>(file), "File-limit fixture failed");
    }
    medgl::DicomCatalog limitCatalog(fileLimit);
    Reject([&] { limitCatalog.Scan(); });

    const auto escape = Pair(root, "escape", files);
    std::error_code linkError;
    std::filesystem::create_directory_symlink(phantom, escape / "outside", linkError);
    if (!linkError) {
        medgl::DicomCatalog escapeCatalog(escape);
        Reject([&] { escapeCatalog.Scan(); });
    } else {
        std::cout << "SKIP: symlink escape test (symlink creation unavailable)\n";
    }

    Rewrite(pair / "first.dcm", [](gdcm::DataSet& data) { data.Remove(gdcm::Tag(0x0028, 0x1052)); });
    Reject([&] { pairCatalog.Load("series-1"); });
    Reject([&] { medgl::WriteSyntheticCt(phantom); });
}

}

int main(int argc, char** argv) {
    try {
        const auto base = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("out/tests/dicom");
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        const auto root = base / ("run-" + std::to_string(stamp));
        std::filesystem::create_directories(root);
        Run(root);
        std::cout << "DICOM tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "DICOM tests failed: " << exception.what() << '\n';
        return 1;
    }
}