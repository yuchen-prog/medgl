#include "medgl/data_contracts.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

void Expect(bool condition, const char* description)
{
    if (!condition) {
        throw std::runtime_error(description);
    }
}

template <typename Exception, typename Action>
void ExpectThrows(Action action, const char* description)
{
    try {
        action();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(description);
}

void TestVolumeLayout()
{
    const medgl::VolumeShape shape{3, 4, 5};
    Expect(shape.VoxelCount() == 60, "Voxel count is incorrect");
    Expect(shape.FloatBytes() == 240, "Float32 storage size is incorrect");
    Expect(shape.LinearIndex(1, 0, 0) == 1, "X must be the fastest axis");
    Expect(shape.LinearIndex(0, 1, 0) == 3, "Row stride is incorrect");
    Expect(shape.LinearIndex(0, 0, 1) == 12, "Slice stride is incorrect");
    Expect(shape.LinearIndex(2, 3, 4) == 59, "Last voxel index is incorrect");
    std::cout << "[PASS] X-fast volume layout and float32 byte count\n";
}

void TestInvalidDimensions()
{
    ExpectThrows<std::invalid_argument>([] {
        medgl::VolumeShape{0, 4, 5}.VoxelCount();
    }, "Zero volume width must be rejected");
    ExpectThrows<std::invalid_argument>([] {
        medgl::VolumeShape{3, 0, 5}.VoxelCount();
    }, "Zero volume height must be rejected");
    ExpectThrows<std::invalid_argument>([] {
        medgl::VolumeShape{3, 4, 0}.VoxelCount();
    }, "Zero volume depth must be rejected");
    const medgl::VolumeShape shape{3, 4, 5};
    ExpectThrows<std::out_of_range>([&shape] { shape.LinearIndex(3, 0, 0); },
                                   "Out-of-range column must be rejected");
    ExpectThrows<std::out_of_range>([&shape] { shape.LinearIndex(0, 4, 0); },
                                   "Out-of-range row must be rejected");
    ExpectThrows<std::out_of_range>([&shape] { shape.LinearIndex(0, 0, 5); },
                                   "Out-of-range slice must be rejected");
    std::cout << "[PASS] Empty dimensions and out-of-bounds indices rejected\n";
}

void TestOverflow()
{
    const auto maximum = std::numeric_limits<std::uint32_t>::max();
    ExpectThrows<std::overflow_error>([maximum] {
        medgl::VolumeShape{maximum, maximum, maximum}.VoxelCount();
    }, "Voxel count overflow must be rejected");
    ExpectThrows<std::overflow_error>([maximum] {
        medgl::VolumeShape{maximum, maximum, 1}.FloatBytes();
    }, "Float32 allocation overflow must be rejected");
    ExpectThrows<std::overflow_error>([maximum] {
        medgl::RgbImage{maximum, maximum, {}}.RequiredBytes();
    }, "RGB allocation overflow must be rejected");
    std::cout << "[PASS] Checked buffer-size multiplication\n";
}

void TestRgbContract()
{
    medgl::RgbImage image{321, 123, {}};
    Expect(image.StrideBytes() == 963, "RGB stride is incorrect");
    Expect(image.RequiredBytes() == 118449, "RGB byte count is incorrect");
    image.pixels.resize(image.RequiredBytes());
    image.Validate();
    image.pixels.pop_back();
    ExpectThrows<std::invalid_argument>([&image] { image.Validate(); },
                                       "A truncated RGB buffer must be rejected");
    image.pixels.resize(image.RequiredBytes() + 1);
    ExpectThrows<std::invalid_argument>([&image] { image.Validate(); },
                                       "An oversized RGB buffer must be rejected");
    ExpectThrows<std::invalid_argument>([] {
        medgl::RgbImage{0, 123, {}}.RequiredBytes();
    }, "Zero image width must be rejected");
    ExpectThrows<std::invalid_argument>([] {
        medgl::RgbImage{321, 0, {}}.RequiredBytes();
    }, "Zero image height must be rejected");
    std::cout << "[PASS] Actual odd-width, non-square RGB dimensions\n";
}

}

int main()
{
    static_assert(sizeof(void*) == 8, "Windows x64 is required");
    static_assert(sizeof(float) == 4, "Volume storage requires float32");
    static_assert(sizeof(std::int16_t) == 2, "DICOM integer input requires int16");
    try {
        TestVolumeLayout();
        TestInvalidDimensions();
        TestOverflow();
        TestRgbContract();
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
    std::cout << "[PASS] Host contracts only. CUDA was not exercised.\n";
    return 0;
}