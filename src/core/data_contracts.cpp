#include "medgl/data_contracts.hpp"

#include <limits>
#include <stdexcept>

namespace medgl {
namespace {

std::size_t CheckedMultiply(std::size_t first, std::size_t second)
{
    if (first != 0 && second > std::numeric_limits<std::size_t>::max() / first) {
        throw std::overflow_error("Buffer size exceeds the addressable range");
    }
    return first * second;
}

void RequirePositive(std::uint32_t dimension)
{
    if (dimension == 0) {
        throw std::invalid_argument("Image and volume dimensions must be positive");
    }
}

}

std::size_t VolumeShape::VoxelCount() const
{
    RequirePositive(width);
    RequirePositive(height);
    RequirePositive(depth);
    return CheckedMultiply(CheckedMultiply(width, height), depth);
}

std::size_t VolumeShape::FloatBytes() const
{
    static_assert(sizeof(float) == 4, "Volume storage requires float32");
    return CheckedMultiply(VoxelCount(), sizeof(float));
}

std::size_t VolumeShape::LinearIndex(std::uint32_t column, std::uint32_t row,
                                   std::uint32_t slice) const
{
    VoxelCount();
    if (column >= width || row >= height || slice >= depth) {
        throw std::out_of_range("Voxel index is outside the volume");
    }
    return column + static_cast<std::size_t>(width) *
        (row + static_cast<std::size_t>(height) * slice);
}

std::size_t RgbImage::StrideBytes() const
{
    RequirePositive(width);
    return CheckedMultiply(width, 3);
}

std::size_t RgbImage::RequiredBytes() const
{
    RequirePositive(height);
    return CheckedMultiply(StrideBytes(), height);
}

void RgbImage::Validate() const
{
    if (pixels.size() != RequiredBytes()) {
        throw std::invalid_argument("RGB buffer does not match its actual dimensions");
    }
}

}