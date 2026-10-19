#include "medgl/renderer.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

int main()
{
    try {
        medgl::VolumeData volume;
        volume.shape = {16, 16, 16};
        volume.spacing = {1, 1, 2};
        volume.voxels.assign(volume.shape.VoxelCount(), 100);
        medgl::VolumeRenderer renderer;
        renderer.Load(volume);
        const auto loadTiming = renderer.LastLoadTimings();
        if (!std::isfinite(loadTiming.totalMs) || loadTiming.totalMs <= 0
            || std::abs(loadTiming.totalMs - loadTiming.validationMs - loadTiming.uploadMs) > 0.001)
            throw std::runtime_error("Load timing intervals do not add up");
        auto settings = medgl::FitView(volume);
        settings.width = 65;
        settings.height = 33;
        settings.transfer = {{0, {1, 0, 0, 0.02f}}, {1, {1, 0, 0, 0.02f}}};
        const auto image = renderer.Render(settings);
        image.Validate();
        const auto timing = renderer.LastRenderTimings();
        if (!std::isfinite(timing.kernelMs) || timing.kernelMs <= 0 || timing.readbackMs <= 0
            || std::abs(timing.totalMs - timing.prepareMs - timing.kernelWallMs - timing.readbackMs) > 0.001)
            throw std::runtime_error("Render timing intervals are invalid");
        if (renderer.Render(settings).pixels != image.pixels)
            throw std::runtime_error("Timing instrumentation changed repeat render output");
        const auto center = (16 * 65 + 32) * 3;
        const int expected = static_cast<int>((1 - std::pow(0.98, 32)) * 255 + 0.5);
        if (std::abs(static_cast<int>(image.pixels[center]) - expected) > 1)
            throw std::runtime_error("Uniform volume opacity does not match Beer-Lambert reference");
        if (image.pixels[center + 1] != 0 || image.pixels[center + 2] != 0 || image.pixels[0] != 0)
            throw std::runtime_error("RGB layout or volume/background boundary is incorrect");
        settings.stepMm = 1.3f;
        const auto second = renderer.Render(settings);
        if (std::abs(static_cast<int>(second.pixels[center]) - expected) > 1)
            throw std::runtime_error("Opacity changed with physical sample step");
        settings.opacityScale = 0;
        settings.background = {0, 0, 1};
        const auto transparent = renderer.Render(settings);
        if (transparent.pixels[center] != 0 || transparent.pixels[center + 2] != 255)
            throw std::runtime_error("Transparent volume must reveal background");
        settings.opacityScale = 1;
        settings.background = {0, 0, 0};
        settings.rotation = {0, std::sqrt(0.5f), 0, std::sqrt(0.5f)};
        const auto rotated = renderer.Render(settings);
        const int expectedRotated = static_cast<int>((1 - std::pow(0.98, 16)) * 255 + 0.5);
        if (std::abs(static_cast<int>(rotated.pixels[center]) - expectedRotated) > 1)
            throw std::runtime_error("Quaternion camera did not rotate physical ray");
        volume.direction = {0, 0, 1, 0, 1, 0, -1, 0, 0};
        volume.origin = {50, -30, 12};
        renderer.Load(volume);
        settings = medgl::FitView(volume);
        settings.width = 65;
        settings.height = 33;
        settings.lighting = true;
        settings.transfer = {{0, {1, 0, 0, 0.02f}}, {1, {1, 0, 0, 0.02f}}};
        const auto oriented = renderer.Render(settings);
        if (std::abs(static_cast<int>(oriented.pixels[center]) - expectedRotated) > 1)
            throw std::runtime_error("Patient origin/direction or zero-gradient lighting incorrect");
        renderer.Unload();
        bool rejected = false;
        try { renderer.Render(settings); } catch (const std::logic_error&) { rejected = true; }
        if (!rejected) throw std::runtime_error("Unloaded render was accepted");
        std::cout << "[PASS] CUDA VR: analytic opacity, step invariance, RGB shape, transparency, unload\n";
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
    return 0;
}