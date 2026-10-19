#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace medgl {

struct VolumeShape {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 0;

    std::size_t VoxelCount() const;
    std::size_t FloatBytes() const;
    std::size_t LinearIndex(std::uint32_t column, std::uint32_t row,
                            std::uint32_t slice) const;
};

struct RgbImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;

    std::size_t StrideBytes() const;
    std::size_t RequiredBytes() const;
    void Validate() const;
};

}