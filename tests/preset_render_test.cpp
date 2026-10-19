#include "medgl/renderer.hpp"
#include "appearance_presets.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <set>
#include <stdexcept>

int main()
{
    try {
        auto volume = medgl::MakePhantom();
        for (std::uint32_t slice = 35; slice < 62; ++slice)
            for (std::uint32_t row = 35; row < 62; ++row)
                for (std::uint32_t column = 65; column < 72; ++column)
                    volume.voxels[volume.shape.LinearIndex(column, row, slice)] = -750;
        medgl::VolumeRenderer renderer;
        renderer.Load(volume);
        std::set<std::uint64_t> checksums;
        for (const auto& preset : medgl::server::AppearancePresets()) {
            auto settings = medgl::FitView(volume);
            settings.width = 129;
            settings.height = 97;
            settings.windowWidth = preset.windowWidth;
            settings.windowLevel = preset.windowLevel;
            settings.opacityScale = preset.opacityScale;
            settings.lighting = preset.lighting;
            settings.transfer = preset.transfer;
            const auto image = renderer.Render(settings);
            image.Validate();
            if (std::none_of(image.pixels.begin(), image.pixels.end(), [](std::uint8_t value) { return value > 20; }))
                throw std::runtime_error("Appearance preset produced a blank phantom");
            std::uint64_t checksum = 14695981039346656037ULL;
            for (auto value : image.pixels) checksum = (checksum ^ value) * 1099511628211ULL;
            if (!checksums.insert(checksum).second) throw std::runtime_error("Appearance presets produced identical frames");
            if (preset.id == "ct-grayscale") {
                for (std::size_t offset = 0; offset < image.pixels.size(); offset += 3)
                    if (image.pixels[offset] != image.pixels[offset + 1] || image.pixels[offset] != image.pixels[offset + 2])
                        throw std::runtime_error("Grayscale preset produced colored pixels");
            }
            medgl::VolumeData air;
            air.shape = {8, 8, 8};
            air.voxels.assign(air.shape.VoxelCount(), -1000);
            renderer.Load(air);
            auto airSettings = medgl::FitView(air);
            airSettings.width = 17;
            airSettings.height = 17;
            airSettings.windowWidth = preset.windowWidth;
            airSettings.windowLevel = preset.windowLevel;
            airSettings.transfer = preset.transfer;
            const auto airImage = renderer.Render(airSettings);
            if (std::any_of(airImage.pixels.begin(), airImage.pixels.end(), [](std::uint8_t value) { return value != 0; }))
                throw std::runtime_error("Appearance preset tinted exterior air");
            renderer.Load(volume);
            std::cout << "[PASS] " << preset.id << ": nonblank, distinct, exterior air transparent\n";
        }
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
    return 0;
}