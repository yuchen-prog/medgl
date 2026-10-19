#include "medgl/renderer.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void Check() {
    medgl::VolumeData volume;
    volume.shape = {16, 16, 16};
    volume.spacing = {1, 1, 2};
    volume.origin = {41, -23, 7};
    volume.direction = {0, 0, 1, 1, 0, 0, 0, 1, 0};
    volume.voxels.assign(volume.shape.VoxelCount(), 100);
    medgl::VolumeRenderer renderer;
    renderer.Load(volume);
    auto settings = medgl::FitView(volume);
    settings.width = 65;
    settings.height = 65;
    settings.rotation = {0.5f, 0.5f, 0.5f, 0.5f};
    settings.transfer = {{0, {1, 0, 0, 0.02f}}, {1, {1, 0, 0, 0.02f}}};
    settings.lighting = false;
    const auto original = renderer.Render(settings);
    const auto center = (32 * 65 + 32) * 3;
    const int full = static_cast<int>((1 - std::pow(0.98, 32)) * 255 + 0.5);
    Require(std::abs(static_cast<int>(original.pixels[center]) - full) <= 1, "Full depth opacity mismatch");
    settings.cropBox.enabled = true;
    Require(renderer.Render(settings).pixels == original.pixels, "Full crop changed pixels");
    settings.cropBox.maximum[2] = 0.5f;
    const auto half = renderer.Render(settings);
    const int expected = static_cast<int>((1 - std::pow(0.98, 16)) * 255 + 0.5);
    Require(std::abs(static_cast<int>(half.pixels[center]) - expected) <= 1, "Half depth opacity mismatch");
    settings.stepMm = 1.3f;
    Require(std::abs(static_cast<int>(renderer.Render(settings).pixels[center]) - expected) <= 1,
        "Crop physical step mismatch");
    settings.rotation = {-0.5f, -0.5f, 0.5f, 0.5f};
    Require(std::abs(static_cast<int>(renderer.Render(settings).pixels[center]) - expected) <= 1,
        "Reverse crop ray opacity mismatch");
    settings.cropBox.minimum[2] = 0.25f;
    const int slab = static_cast<int>((1 - std::pow(0.98, 8)) * 255 + 0.5);
    settings.lighting = true;
    Require(std::abs(static_cast<int>(renderer.Render(settings).pixels[center]) - slab) <= 1,
        "Interior crop interval or uniform lighting mismatch");
    settings.cropBox.minimum[2] = 0.499f;
    settings.transfer = {{0, {1, 0, 0, 0.8f}}, {1, {1, 0, 0, 0.8f}}};
    const int thin = static_cast<int>((1 - std::pow(0.2, 0.032)) * 255 + 0.5);
    Require(std::abs(static_cast<int>(renderer.Render(settings).pixels[center]) - thin) <= 1,
        "Sub-step crop interval lost its physical length");
    settings.transfer = {{0, {1, 0, 0, 0.02f}}, {1, {1, 0, 0, 0.02f}}};
    settings.cropBox.minimum[2] = 0;
    settings.rotation = {0.5f, 0.5f, 0.5f, 0.5f};
    settings.lighting = false;
    settings.cropBox.minimum[0] = 0.75f;
    settings.background = {0, 0, 1};
    const auto excluded = renderer.Render(settings);
    Require(excluded.pixels[center] == 0 && excluded.pixels[center + 2] == 255,
        "Excluded parallel ray did not reveal background");
    settings.cropBox.enabled = false;
    settings.background = {0, 0, 0};
    settings.stepMm = 0.5f;
    Require(renderer.Render(settings).pixels == original.pixels, "Disabled crop changed output");
    for (int invalid = 0; invalid < 5; ++invalid) {
        settings.cropBox = {};
        if (invalid == 0) settings.cropBox.minimum[0] = -0.1f;
        if (invalid == 1) settings.cropBox.maximum[1] = 1.1f;
        if (invalid == 2) settings.cropBox.minimum[2] = 1;
        if (invalid == 3) settings.cropBox.maximum[0] = 0;
        if (invalid == 4) settings.cropBox.minimum[1] = std::numeric_limits<float>::quiet_NaN();
        bool rejected = false;
        try { renderer.Render(settings); } catch (const std::invalid_argument&) { rejected = true; }
        Require(rejected, "Invalid crop accepted");
    }
}

void CheckPlane() {
    medgl::VolumeData volume;
    volume.shape = {16, 16, 16};
    volume.spacing = {1, 1, 2};
    volume.origin = {41, -23, 7};
    volume.direction = {0, 0, 1, 1, 0, 0, 0, 1, 0};
    volume.voxels.assign(volume.shape.VoxelCount(), 100);
    medgl::VolumeRenderer renderer;
    renderer.Load(volume);
    auto settings = medgl::FitView(volume);
    settings.width = settings.height = 65;
    settings.rotation = {0.5f, 0.5f, 0.5f, 0.5f};
    settings.transfer = {{0, {1, 0, 0, 0.02f}}, {1, {1, 0, 0, 0.02f}}};
    const auto original = renderer.Render(settings);
    const auto center = (32 * 65 + 32) * 3;
    const auto checkDepth = [&](float length) {
        const int expected = static_cast<int>((1 - std::pow(0.98, length)) * 255 + 0.5);
        Require(std::abs(static_cast<int>(renderer.Render(settings).pixels[center]) - expected) <= 1,
            "Clip plane physical opacity mismatch");
    };
    settings.clipPlane = {true, {1, 0, 0}, 0, true};
    checkDepth(16);
    settings.clipPlane.offsetMm = 4;
    settings.stepMm = 1.3f;
    checkDepth(12);
    settings.clipPlane.keepPositive = false;
    checkDepth(20);
    settings.rotation = {-0.5f, -0.5f, 0.5f, 0.5f};
    checkDepth(20);
    settings.clipPlane = {true, {std::sqrt(0.5f), std::sqrt(0.5f), 0}, 0, true};
    checkDepth(16);
    settings.rotation = {0.5f, 0.5f, 0.5f, 0.5f};
    const auto oblique = renderer.Render(settings);
    const float shift = 4 * settings.orthoHeight / settings.height;
    const int offCenter = static_cast<int>((1 - std::pow(0.98, 16 + shift)) * 255 + 0.5);
    Require(std::abs(static_cast<int>(oblique.pixels[center + 4 * 3]) - offCenter) <= 1,
        "Oblique plane off-center LPS intersection mismatch");
    const auto target = settings.target;
    settings.target[1] += 2;
    checkDepth(18);
    settings.target = target;
    settings.cropBox = {true, {0, 0, 0.25f}, {1, 1, 0.75f}};
    checkDepth(8);
    settings.cropBox = {};
    settings.clipPlane = {true, {0, 1, 0}, 1, true};
    checkDepth(0);
    settings.clipPlane.keepPositive = false;
    checkDepth(32);
    settings.clipPlane = {true, {1, 0, 0}, 15.99f, true};
    checkDepth(0.01f);
    settings.clipPlane.enabled = false;
    settings.rotation = {0.5f, 0.5f, 0.5f, 0.5f};
    settings.stepMm = 0.5f;
    Require(renderer.Render(settings).pixels == original.pixels, "Disabled plane changed pixels");
    for (int invalid = 0; invalid < 5; ++invalid) {
        settings.clipPlane = {};
        if (invalid == 0) settings.clipPlane.normal = {0, 0, 0};
        if (invalid == 1) settings.clipPlane.normal = {0, 0, 2};
        if (invalid == 2) settings.clipPlane.normal[0] = std::numeric_limits<float>::quiet_NaN();
        if (invalid == 3) settings.clipPlane.offsetMm = std::numeric_limits<float>::infinity();
        if (invalid == 4) settings.clipPlane.offsetMm = 1000001;
        bool rejected = false;
        try { renderer.Render(settings); } catch (const std::invalid_argument&) { rejected = true; }
        Require(rejected, "Invalid clip plane accepted");
    }
}

}

int main() {
    try {
        Check();
        CheckPlane();
        std::cout << "Crop GPU regressions passed: affine axes, physical opacity, full/disabled equality, background and validation.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Crop regression failed: " << error.what() << '\n';
        return 1;
    }
}