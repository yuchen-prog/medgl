#include "medgl/dicom_audit.hpp"

#include <gdcmTrace.h>

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <streambuf>

namespace {

class NullBuffer : public std::streambuf {
    int_type overflow(int_type value) override { return traits_type::not_eof(value); }
    std::streamsize xsputn(const char*, std::streamsize count) override { return count; }
};

bool Inside(const std::filesystem::path& root, const std::filesystem::path& path) {
    auto candidate = path.begin();
    for (const auto& component : root) {
        if (candidate == path.end() || component != *candidate) return false;
        ++candidate;
    }
    return true;
}

int Run(int argc, char** argv) {
    std::filesystem::path root, output;
    std::string series;
    medgl::DicomReadOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument(argv[index]);
        if (argument == "--allow-instance-uid-mismatch" && !options.allowInstanceUidMismatch) {
            options.allowInstanceUidMismatch = true;
        } else if ((argument == "--data-root" || argument == "--series" || argument == "--output") && index + 1 < argc) {
            const std::string value(argv[++index]);
            if (value.empty() || value.rfind("--", 0) == 0) throw std::invalid_argument("Invalid arguments");
            if (argument == "--data-root" && root.empty()) root = value;
            else if (argument == "--output" && output.empty()) output = value;
            else if (argument == "--series" && series.empty()) series = value;
            else throw std::invalid_argument("Duplicate arguments");
        } else {
            throw std::invalid_argument("Invalid arguments");
        }
    }
    if (root.empty() || output.empty()) {
        std::cerr << "Usage: medgl_dicom_audit --data-root directory [--series series-N] "
                     "[--allow-instance-uid-mismatch] --output report.json\n";
        return 1;
    }
    const auto canonicalRoot = std::filesystem::canonical(root);
    const auto canonicalOutput = std::filesystem::weakly_canonical(std::filesystem::absolute(output));
    if (Inside(canonicalRoot, canonicalOutput)) throw std::invalid_argument("Output overlaps input");
    if (std::filesystem::exists(canonicalOutput) && std::filesystem::hard_link_count(canonicalOutput) > 1)
        throw std::invalid_argument("Output has hard-link aliases");
    const auto report = medgl::AuditDicomVolume(canonicalRoot, series, options);
    std::filesystem::create_directories(canonicalOutput.parent_path());
    std::ofstream stream(canonicalOutput, std::ios::binary | std::ios::trunc);
    if (!stream) throw std::runtime_error("Output unavailable");
    stream << report.dump(2) << '\n';
    stream.flush();
    if (!stream) throw std::runtime_error("Output incomplete");
    return report.at("passed").get<bool>() ? 0 : 2;
}

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
    try { return Run(argc, argv); }
    catch (...) {
        std::cerr << "Offline DICOM audit failed; no identifying details are logged.\n";
        return 1;
    }
}