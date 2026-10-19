#pragma once

#include "medgl/dicom.hpp"

#include <nlohmann/json.hpp>

namespace medgl {

nlohmann::json AuditDicomVolume(const std::filesystem::path& root,
                              const std::string& seriesId = {},
                              DicomReadOptions options = {});
nlohmann::json CheckDicomVolume(const std::filesystem::path& root,
                              const VolumeData& loaded,
                              const std::string& seriesId,
                              DicomReadOptions options = {});

}