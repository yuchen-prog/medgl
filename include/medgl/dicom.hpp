#pragma once

#include "medgl/renderer.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace medgl {

struct DicomReadOptions {
    bool allowInstanceUidMismatch = false;
};

struct SeriesInfo {
    std::string id;
    std::string description;
    std::size_t fileCount;
    bool supported;
    std::string reason;
    std::string warning;
};

class DicomCatalog {
public:
    explicit DicomCatalog(std::filesystem::path root, DicomReadOptions options = {});
    ~DicomCatalog();
    std::vector<SeriesInfo> Scan();
    VolumeData Load(const std::string& id) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

void WriteSyntheticCt(const std::filesystem::path& root);

}