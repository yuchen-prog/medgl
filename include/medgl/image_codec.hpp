#pragma once

#include "medgl/data_contracts.hpp"

#include <cstdint>
#include <vector>

namespace medgl {

enum class PngCompression { Default, Fast };

std::vector<std::uint8_t> EncodePng(const RgbImage& image);
std::vector<std::uint8_t> EncodePng(const RgbImage& image, PngCompression compression);
std::vector<std::uint8_t> EncodeJpeg(const RgbImage& image, int quality = 85);

}