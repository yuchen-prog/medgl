#include "medgl/dicom_audit.hpp"

#include <gdcmAttribute.h>
#include <gdcmImageChangeTransferSyntax.h>
#include <gdcmImageReader.h>
#include <gdcmImageWriter.h>
#include <gdcmReader.h>
#include <gdcmTrace.h>
#include <gdcmWriter.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>
#include <streambuf>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::map<std::filesystem::path, std::uint64_t> Hashes(const std::filesystem::path& root) {
    std::map<std::filesystem::path, std::uint64_t> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) continue;
        std::ifstream input(entry.path(), std::ios::binary);
        Require(static_cast<bool>(input), "Cannot hash synthetic input");
        std::uint64_t hash = 14695981039346656037ull;
        char byte = 0;
        while (input.get(byte)) { hash ^= static_cast<unsigned char>(byte); hash *= 1099511628211ull; }
        Require(input.eof(), "Synthetic hash read failed");
        result.emplace(entry.path(), hash);
    }
    return result;
}

void CheckPhantom(const std::filesystem::path& root) {
    medgl::WriteSyntheticCt(root);
    const auto before = Hashes(root);
    medgl::DicomCatalog catalog(root);
    const auto entries = catalog.Scan();
    Require(entries.size() == 1 && entries[0].supported, "Synthetic selection failed");
    auto volume = catalog.Load(entries[0].id);
    const auto report = medgl::CheckDicomVolume(root, volume, entries[0].id);
    Require(report.at("passed") == true, "Original-stored audit did not pass");
    Require(report.at("comparison").at("pixelsCompared") == 64 * 64 * 64, "Not every voxel was compared");
    Require(report.at("comparison").at("mismatchCount") == 0, "Unexpected HU mismatch");
    Require(report.at("originalHu").at("min") == -1000, "Modality rescale was not applied exactly once");
    Require(report.at("originalHu").at("max") == 700, "Original HU maximum differs");
    Require(report.at("geometry").at("strictlyAscending") == true, "IPP projection order differs");
    Require(report.at("selectedSliceStatistics").size() == 3, "Missing first/middle/last slice statistics");
    Require(report.at("scope") == "numerical-and-geometric-self-audit" &&
            report.at("independentExternalTool") == false, "Audit overstates independence");
    const auto encoded = report.dump();
    Require(encoded.find(root.string()) == std::string::npos && encoded.find("slice-") == std::string::npos &&
            encoded.find("1.2.840.") == std::string::npos, "Report contains input paths or identifiers");
    const auto standalone = medgl::AuditDicomVolume(root);
    Require(standalone.at("passed") == true, "Fresh-catalog audit failed");
    volume.voxels[12345] += 7;
    const auto corrupted = medgl::CheckDicomVolume(root, volume, entries[0].id);
    Require(corrupted.at("passed") == false && corrupted.at("comparison").at("mismatchCount") == 1 &&
            corrupted.at("comparison").at("maxAbsErrorHu") == 7, "Loaded corruption was not detected");
    volume.voxels[12345] -= 7;
    volume.origin[0] += 1;
    const auto shifted = medgl::CheckDicomVolume(root, volume, entries[0].id);
    Require(shifted.at("passed") == false && shifted.at("geometry").at("passed") == false &&
            shifted.at("comparison").at("mismatchCount") == 0, "Geometric corruption was not detected");
    volume.origin[0] -= 1;
    volume.voxels[7] = std::numeric_limits<float>::quiet_NaN();
    const auto nonfinite = medgl::CheckDicomVolume(root, volume, entries[0].id);
    Require(nonfinite.at("passed") == false && nonfinite.at("comparison").at("nonfiniteLoadedCount") == 1 &&
            nonfinite.at("comparison").at("maxAbsErrorHu").is_null(), "Nonfinite loaded HU was not detected");
    Require(before == Hashes(root), "Audit modified original synthetic file contents");
}

std::vector<std::filesystem::path> Paths(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(root))
        if (entry.is_regular_file()) files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    return files;
}

void Text(gdcm::DataSet& data, std::uint16_t group, std::uint16_t element,
          gdcm::VR vr, std::string value) {
    if (value.size() % 2) value.push_back(vr == gdcm::VR::UI ? '\0' : ' ');
    gdcm::DataElement entry(gdcm::Tag(group, element));
    entry.SetVR(vr);
    entry.SetByteValue(value.data(), static_cast<std::uint32_t>(value.size()));
    data.Replace(entry);
}

void Rewrite(const std::filesystem::path& path, const std::function<void(gdcm::DataSet&)>& edit) {
    gdcm::Reader reader;
    {
        std::ifstream input(path, std::ios::binary);
        reader.SetStream(input);
        Require(reader.Read(), "Synthetic metadata read failed");
    }
    edit(reader.GetFile().GetDataSet());
    gdcm::Writer writer;
    writer.SetFile(reader.GetFile());
    std::ofstream output(path, std::ios::binary);
    writer.SetStream(output);
    Require(writer.Write(), "Synthetic metadata write failed");
}

std::filesystem::path Pair(const std::filesystem::path& root, const std::string& name,
                           const std::vector<std::filesystem::path>& source) {
    const auto directory = root / name;
    std::filesystem::create_directories(directory);
    std::filesystem::copy_file(source[0], directory / "later-name.dcm");
    std::filesystem::copy_file(source[37], directory / "earlier-name.dcm");
    return directory;
}

void PackedPixels(const std::filesystem::path& path, unsigned bits, bool signedPixels,
                  bool extended, gdcm::TransferSyntax syntax, double slope, double intercept) {
    gdcm::ImageReader reader;
    {
        std::ifstream input(path, std::ios::binary);
        reader.SetStream(input);
        Require(reader.Read(), "Synthetic pixel read failed");
    }
    gdcm::ImageWriter writer;
    writer.SetFile(reader.GetFile());
    auto& image = writer.GetImage();
    image = reader.GetImage();
    image.SetPixelFormat(gdcm::PixelFormat(1, 16, static_cast<unsigned short>(bits),
        static_cast<unsigned short>(bits - 1), static_cast<unsigned short>(signedPixels ? 1 : 0)));
    image.SetSlope(slope);
    image.SetIntercept(intercept);
    const std::uint32_t modulus = std::uint32_t{1} << bits;
    const std::int32_t values[4] = {signedPixels ? -1 : 1,
        signedPixels ? -static_cast<std::int32_t>(modulus / 2) : static_cast<std::int32_t>(modulus - 1),
        0, static_cast<std::int32_t>(modulus / 2 - 1)};
    std::vector<std::uint16_t> pixels(64 * 64);
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        const auto value = values[index % 4];
        pixels[index] = static_cast<std::uint16_t>(extended ? value : value & (modulus - 1));
    }
    gdcm::DataElement pixelData(gdcm::Tag(0x7fe0, 0x0010));
    pixelData.SetVR(gdcm::VR::OW);
    pixelData.SetByteValue(reinterpret_cast<const char*>(pixels.data()),
                          static_cast<std::uint32_t>(pixels.size() * sizeof(std::uint16_t)));
    image.SetTransferSyntax(gdcm::TransferSyntax::ExplicitVRLittleEndian);
    image.SetDataElement(pixelData);
    if (syntax != gdcm::TransferSyntax::ExplicitVRLittleEndian) {
        gdcm::ImageChangeTransferSyntax converter;
        converter.SetInput(image);
        converter.SetTransferSyntax(syntax);
        Require(converter.Change(), "Synthetic transfer syntax conversion failed");
        image = converter.GetOutput();
    }
    {
        std::ofstream output(path, std::ios::binary);
        writer.SetStream(output);
        Require(writer.Write(), "Synthetic packed pixels write failed");
    }
    gdcm::Reader verify;
    std::ifstream input(path, std::ios::binary);
    verify.SetStream(input);
    Require(verify.Read() && verify.GetFile().GetHeader().GetDataSetTransferSyntax() == syntax,
            "Synthetic pixel transfer syntax was not preserved");
}

void Reject(const std::function<void()>& action) {
    try { action(); }
    catch (const std::exception& exception) {
        const std::string message = exception.what();
        Require(message.find("SecretPatient") == std::string::npos && message.find(".dcm") == std::string::npos &&
                message.find("1.2.840.") == std::string::npos && message.find("medgl-audit-synthetic") == std::string::npos,
                "Audit exception contains identifying input");
        return;
    }
    throw std::runtime_error("Expected audit rejection");
}

void PatchMetaInstance(const std::filesystem::path& path) {
    gdcm::Reader reader;
    {
        std::ifstream input(path, std::ios::binary);
        reader.SetStream(input);
        Require(reader.Read(), "Synthetic instance metadata read failed");
    }
    const auto* value = reader.GetFile().GetHeader().GetDataElement(gdcm::Tag(0x0002, 0x0003)).GetByteValue();
    Require(value != nullptr, "Synthetic instance metadata missing");
    const std::string identifier(value->GetPointer(), value->GetLength());
    const auto digit = identifier.find_last_of("0123456789");
    Require(digit != std::string::npos, "Synthetic instance has no digit");
    std::ifstream input(path, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();
    const auto offset = bytes.find(identifier);
    Require(offset != std::string::npos, "Synthetic file-meta instance not found");
    bytes[offset + digit] = identifier[digit] == '9' ? '8' : '9';
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    Require(static_cast<bool>(output), "Synthetic instance patch failed");
}

void CheckPacked(const std::filesystem::path& root, const std::vector<std::filesystem::path>& source) {
    const std::array<gdcm::TransferSyntax, 3> syntaxes{gdcm::TransferSyntax::ExplicitVRLittleEndian,
        gdcm::TransferSyntax::ExplicitVRBigEndian, gdcm::TransferSyntax::RLELossless};
    for (unsigned bits = 12; bits <= 16; ++bits) {
        for (bool signedPixels : {false, true}) {
            for (std::size_t syntaxIndex = 0; syntaxIndex < syntaxes.size(); ++syntaxIndex) {
                for (bool extended : {false, true}) {
                    if (!signedPixels && extended) continue;
                    const auto pair = Pair(root, "packed-" + std::to_string(bits) + "-" + std::to_string(signedPixels) +
                        "-" + std::to_string(syntaxIndex) + "-" + std::to_string(extended), source);
                    for (const auto& path : Paths(pair))
                        PackedPixels(path, bits, signedPixels, extended, syntaxes[syntaxIndex], 0.7, -1024.25);
                    const auto before = Hashes(pair);
                    medgl::DicomCatalog catalog(pair);
                    const auto entries = catalog.Scan();
                    Require(entries.size() == 1 && entries[0].supported, "Packed fixture is unsupported");
                    auto volume = catalog.Load(entries[0].id);
                    const auto report = medgl::CheckDicomVolume(pair, volume, entries[0].id);
                    Require(report.at("passed") == true && report.at("comparison").at("pixelsCompared") == 8192 &&
                            report.at("comparison").at("mismatchCount") == 0, "Packed all-pixel comparison failed");
                    Require(report.at("pixelFormat").at("bitsStored12Through16Counts")[bits - 12] == 2 &&
                            report.at("pixelFormat").at(signedPixels ? "signedSliceCount" : "unsignedSliceCount") == 2,
                            "Packed bit/sign counts differ");
                    Require(report.at("codec").at("encapsulatedSliceCount") == (syntaxIndex == 2 ? 2 : 0),
                            "Packed codec counts differ");
                    const double modulus = static_cast<double>(std::uint32_t{1} << bits);
                    const double values[4] = {signedPixels ? -1.0 : 1.0,
                        signedPixels ? -modulus / 2 : modulus - 1, 0, modulus / 2 - 1};
                    for (std::size_t index = 0; index < volume.voxels.size(); ++index)
                        Require(volume.voxels[index] == static_cast<float>(values[index % 4] * 0.7 - 1024.25),
                                "Synthetic stored-pixel oracle differs");
                    volume.voxels[0] = static_cast<float>(volume.voxels[0] * 0.7 - 1024.25);
                    const auto twice = medgl::CheckDicomVolume(pair, volume, entries[0].id);
                    Require(twice.at("passed") == false && twice.at("comparison").at("mismatchCount") == 1,
                            "Double modality rescale was not detected");
                    Require(before == Hashes(pair), "Packed audit modified original inputs");
                }
            }
        }
    }
}

void CheckCli(const std::filesystem::path& root, const std::filesystem::path& executable) {
    const auto log = root / "cli-log.txt";
    const auto invoke = [&](const std::filesystem::path& directory, const std::filesystem::path& output,
                            const std::string& extra) {
        const std::string command = "\"\"" + executable.string() + "\" --data-root \"" + directory.string() +
            "\" --output \"" + output.string() + "\" " + extra + " >\"" + log.string() + "\" 2>&1\"";
        return std::system(command.c_str());
    };
    const auto input = root / "phantom";
    const auto before = Hashes(input);
    const auto output = root / "reports" / "audit.json";
    Require(invoke(input, output, "--series series-1") == 0, "CLI synthetic audit failed");
    {
        std::ifstream stream(output, std::ios::binary);
        nlohmann::json report;
        stream >> report;
        Require(report.at("passed") == true && report.at("comparison").at("pixelsCompared") == 262144,
                "CLI machine-readable report differs");
        Require(std::filesystem::file_size(log) == 0, "Successful CLI emitted identifying or diagnostic output");
    }
    const auto original = Paths(input).front();
    Require(invoke(input, original, {}) == 1, "CLI allowed output to overwrite an input file");
    const auto alias = root / "aliased-report.json";
    std::filesystem::create_hard_link(original, alias);
    Require(invoke(input, alias, {}) == 1, "CLI allowed output to overwrite an input hard-link alias");
    Require(before == Hashes(input), "CLI modified original input bytes");
    const auto strictOutput = root / "strict-rejected.json";
    Require(invoke(root / "instance-mismatch", strictOutput, {}) == 1 && !std::filesystem::exists(strictOutput),
            "CLI strict UID policy did not fail closed");
    Require(invoke(root / "instance-mismatch", root / "compatible.json", "--allow-instance-uid-mismatch") == 0,
            "CLI explicit UID compatibility failed");
    Require(invoke(root / "multiple", root / "ambiguous.json", {}) == 1,
            "CLI implicit ambiguous selection did not fail closed");
    {
        std::ifstream stream(log, std::ios::binary);
        const std::string contents((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        Require(contents == "Offline DICOM audit failed; no identifying details are logged.\n" ||
                contents == "Offline DICOM audit failed; no identifying details are logged.\r\n",
                "CLI error log was not sanitized");
    }
}

void Run(const std::filesystem::path& root, const std::filesystem::path& executable) {
    const auto phantom = root / "phantom";
    CheckPhantom(phantom);
    const auto source = Paths(phantom);
    medgl::DicomCatalog thresholdCatalog(phantom);
    thresholdCatalog.Scan();
    const auto baselineVolume = thresholdCatalog.Load("series-1");
    const auto baselineReport = medgl::CheckDicomVolume(phantom, baselineVolume, "series-1");
    const auto& tolerances = baselineReport.at("geometry");
    Require(tolerances.at("originToleranceMm") == 0.01 &&
        tolerances.at("spacingTolerancesMm")[0] == 1e-5 &&
        tolerances.at("slicePositionToleranceMm") == 0.02 &&
        tolerances.at("affineElementErrorDiagnosticOnly") == true, "Geometry thresholds are not explicit");
    auto perturbed = baselineVolume;
    perturbed.origin[0] += 0.005f;
    Require(medgl::CheckDicomVolume(phantom, perturbed, "series-1").at("geometry").at("passed") == true,
        "Origin within the published tolerance failed");
    perturbed = baselineVolume;
    perturbed.origin[0] += 0.011f;
    Require(medgl::CheckDicomVolume(phantom, perturbed, "series-1").at("geometry").at("passed") == false,
        "Origin outside the published tolerance passed");
    perturbed = baselineVolume;
    perturbed.spacing[0] += 0.000005f;
    Require(medgl::CheckDicomVolume(phantom, perturbed, "series-1").at("geometry").at("passed") == true,
        "Spacing within the published tolerance failed");
    perturbed = baselineVolume;
    perturbed.spacing[0] += 0.00002f;
    Require(medgl::CheckDicomVolume(phantom, perturbed, "series-1").at("geometry").at("passed") == false,
        "Spacing outside the published tolerance passed");
    CheckPacked(root, source);
    const auto variable = Pair(root, "variable-rescale", source);
    PackedPixels(variable / "later-name.dcm", 12, true, false,
                 gdcm::TransferSyntax::ExplicitVRLittleEndian, -0.5, 100.25);
    PackedPixels(variable / "earlier-name.dcm", 12, true, false,
                 gdcm::TransferSyntax::ExplicitVRLittleEndian, 2, -1000);
    Rewrite(variable / "later-name.dcm", [](gdcm::DataSet& data) {
        Text(data, 0x0028, 0x1053, gdcm::VR::DS, "-5E-1");
        Text(data, 0x0028, 0x1052, gdcm::VR::DS, "+100.25");
    });
    const auto variableReport = medgl::AuditDicomVolume(variable);
    Require(variableReport.at("passed") == true && variableReport.at("rescale").at("slope").at("min") == -0.5 &&
            variableReport.at("rescale").at("slope").at("max") == 2, "Per-slice rescale tags differ");

    const auto oblique = Pair(root, "oblique-anisotropic", source);
    std::size_t sliceIndex = 0;
    for (const auto& path : Paths(oblique)) {
        const auto index = sliceIndex++;
        Rewrite(path, [index](gdcm::DataSet& data) {
            Text(data, 0x0020, 0x0037, gdcm::VR::DS, "0.6\\0.8\\0\\0\\0\\1");
            Text(data, 0x0020, 0x0032, gdcm::VR::DS, index ? "8\\-18.5\\30" : "10\\-20\\30");
            Text(data, 0x0028, 0x0030, gdcm::VR::DS, "0.7\\1.2");
        });
    }
    const auto obliqueReport = medgl::AuditDicomVolume(oblique);
    Require(obliqueReport.at("passed") == true && obliqueReport.at("spacingMm")[0] == 1.2 &&
            obliqueReport.at("spacingMm")[1] == 0.7 && obliqueReport.at("spacingMm")[2] == 2.5,
            "Oblique IPP sorting or row/column spacing differs");
    Require(obliqueReport.at("geometry").at("originalVoxelToPatientAffine")[0][3] == 8,
            "LPS affine origin was not sorted by normalized normal");

    const auto compatibility = Pair(root, "instance-mismatch", source);
    for (const auto& path : Paths(compatibility)) PatchMetaInstance(path);
    const auto before = Hashes(compatibility);
    Reject([&] { medgl::AuditDicomVolume(compatibility); });
    const auto allowed = medgl::AuditDicomVolume(compatibility, {}, medgl::DicomReadOptions{true});
    Require(allowed.at("passed") == true && allowed.at("compatibility").at("instanceMismatchSliceCount") == 2 &&
            allowed.at("warnings").size() == 1, "Explicit instance mismatch compatibility differs");
    Require(before == Hashes(compatibility), "Compatibility audit modified original inputs");

    const auto multiple = root / "multiple";
    const auto valid = Pair(multiple, "valid", source);
    const auto other = Pair(multiple, "other", source);
    for (const auto& path : Paths(other))
        PackedPixels(path, 16, true, false, gdcm::TransferSyntax::ExplicitVRLittleEndian, 3, -900);
    for (const auto& path : Paths(valid)) Rewrite(path, [](gdcm::DataSet& data) {
        Text(data, 0x0020, 0x000d, gdcm::VR::UI, "1.2.840.999.3");
        Text(data, 0x0020, 0x000e, gdcm::VR::UI, "1.2.840.999.31");
        Text(data, 0x0020, 0x0052, gdcm::VR::UI, "1.2.840.999.32");
        Text(data, 0x0010, 0x0010, gdcm::VR::PN, "SecretPatient");
    });
    for (const auto& path : Paths(other)) Rewrite(path, [](gdcm::DataSet& data) {
        Text(data, 0x0020, 0x000d, gdcm::VR::UI, "1.2.840.999.2");
        Text(data, 0x0020, 0x000e, gdcm::VR::UI, "1.2.840.999.21");
        Text(data, 0x0020, 0x0052, gdcm::VR::UI, "1.2.840.999.22");
    });
    const auto unsupported = Pair(multiple, "unsupported", source);
    for (const auto& path : Paths(unsupported)) Rewrite(path, [](gdcm::DataSet& data) {
        Text(data, 0x0020, 0x000d, gdcm::VR::UI, "1.2.840.999.1");
        Text(data, 0x0020, 0x000e, gdcm::VR::UI, "1.2.840.999.11");
        Text(data, 0x0020, 0x0052, gdcm::VR::UI, "1.2.840.999.12");
        gdcm::Attribute<0x0028, 0x0101> bits;
        bits.SetValue(8);
        data.Replace(bits.GetAsDataElement());
    });
    {
        std::ofstream text(multiple / "not-dicom.txt", std::ios::binary);
        text << "SecretPatient: not DICOM";
    }
    Reject([&] { medgl::AuditDicomVolume(multiple); });
    Reject([&] { medgl::AuditDicomVolume(multiple, "series-1"); });
    Reject([&] { medgl::AuditDicomVolume(multiple, "unknown"); });
    for (const auto& id : {"series-2", "series-3"}) {
        const auto report = medgl::AuditDicomVolume(multiple, id);
        Require(report.at("passed") == true && report.at("selection").at("groupCount") == 3 &&
                report.at("selection").at("unsupportedSeriesCount") == 1, "Fresh multigroup ordinal selection differs");
        Require(report.at("rescale").at("slope").at("min") == (std::string(id) == "series-2" ? 3 : 2),
            "Same-geometry groups were mapped to the wrong catalog series");
        const auto encoded = report.dump();
        Require(encoded.find("SecretPatient") == std::string::npos && encoded.find("1.2.840.") == std::string::npos &&
                encoded.find(".dcm") == std::string::npos, "Multigroup report contains identifying data");
    }
        medgl::DicomCatalog multipleCatalog(multiple);
        multipleCatalog.Scan();
        const auto wrongGroup = multipleCatalog.Load("series-2");
        Require(medgl::CheckDicomVolume(multiple, wrongGroup, "series-3").at("passed") == false,
            "Cross-group loaded pixels were accepted for identical grid geometry");
        if (!executable.empty()) CheckCli(root, executable);
}

class NullBuffer : public std::streambuf {
    int_type overflow(int_type value) override { return traits_type::not_eof(value); }
    std::streamsize xsputn(const char*, std::streamsize count) override { return count; }
};

}

int main(int argc, char** argv) {
    static NullBuffer buffer;
    static std::ostream sink(&buffer);
    gdcm::Trace::SetDebugStream(sink);
    gdcm::Trace::SetWarningStream(sink);
    gdcm::Trace::SetErrorStream(sink);
    gdcm::Trace::SetDebug(false);
    gdcm::Trace::SetWarning(false);
    gdcm::Trace::SetError(false);
    const auto root = std::filesystem::temp_directory_path() /
        ("medgl-audit-synthetic-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    int status = 0;
    try {
        Require(argc <= 2, "Audit test accepts only an optional CLI executable");
        Run(root, argc == 2 ? std::filesystem::path(argv[1]) : std::filesystem::path{});
        std::cout << "Synthetic DICOM audit tests passed\n";
    }
    catch (const std::exception& exception) { std::cerr << exception.what() << '\n'; status = 1; }
    std::error_code error;
    std::filesystem::remove_all(root, error);
    return status;
}