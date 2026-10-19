#include "medgl/dicom.hpp"
#include "medgl/image_codec.hpp"
#include "medgl/renderer.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int count, char** arguments)
{
    try {
        std::filesystem::path dataRoot;
        std::filesystem::path output = "out/frame.png";
        std::string seriesId;
        bool listOnly = false;
        medgl::DicomReadOptions readOptions;
        for (int index = 1; index < count; ++index) {
            const std::string option = arguments[index];
            if (option == "--list") { listOnly = true; continue; }
            if (option == "--allow-instance-uid-mismatch") {
                readOptions.allowInstanceUidMismatch = true;
                continue;
            }
            if (option == "--help") {
                std::cout << "medgl_cli [--data-root directory --series series-N] [--list] [--output file.png] [--allow-instance-uid-mismatch]\n";
                return 0;
            }
            if (++index >= count) throw std::invalid_argument("Option requires a value");
            if (option == "--data-root") dataRoot = std::filesystem::u8path(arguments[index]);
            else if (option == "--series") seriesId = arguments[index];
            else if (option == "--output") output = std::filesystem::u8path(arguments[index]);
            else throw std::invalid_argument("Unknown option");
        }
        medgl::VolumeData volume;
        if (dataRoot.empty()) {
            if (listOnly) throw std::invalid_argument("Listing requires --data-root");
            volume = medgl::MakePhantom();
        } else {
            medgl::DicomCatalog catalog(dataRoot, readOptions);
            const auto series = catalog.Scan();
            for (const auto& entry : series) {
                std::cout << entry.id << " files=" << entry.fileCount << " supported=" << entry.supported;
                if (!entry.supported) std::cout << " reason=" << entry.reason;
                std::cout << '\n';
                if (!entry.warning.empty()) std::cout << "[WARNING] " << entry.warning << '\n';
            }
            if (listOnly) return 0;
            if (seriesId.empty()) {
                if (series.size() != 1 || !series.front().supported) throw std::invalid_argument("Select a supported sequence with --series");
                seriesId = series.front().id;
            }
            volume = catalog.Load(seriesId);
        }
        const auto range = std::minmax_element(volume.voxels.begin(), volume.voxels.end());
        std::cout << "Volume: " << volume.shape.width << 'x' << volume.shape.height << 'x' << volume.shape.depth
                  << " spacing(mm)=" << volume.spacing[0] << ',' << volume.spacing[1] << ',' << volume.spacing[2]
                  << " intensityRange=" << *range.first << ',' << *range.second << '\n';
        medgl::VolumeRenderer renderer;
        renderer.Load(volume);
        auto settings = medgl::FitView(volume);
        settings.lighting = true;
        settings.transfer = {{0, {0, 0, 0, 0}}, {0.35f, {0.65f, 0.55f, 0.45f, 0}},
            {0.55f, {0.9f, 0.85f, 0.75f, 0.08f}}, {1, {1, 1, 1, 0.65f}}};
        const auto frame = renderer.Render(settings);
        if (std::all_of(frame.pixels.begin(), frame.pixels.end(), [](std::uint8_t pixel) { return pixel == 0; }))
            throw std::runtime_error("Rendered frame is blank");
        const auto encoded = medgl::EncodePng(frame);
        if (output.has_parent_path()) std::filesystem::create_directories(output.parent_path());
        std::ofstream stream(output, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
        if (!stream) throw std::runtime_error("Could not write PNG output");
        std::cout << "[PASS] CUDA VR PNG generated, " << encoded.size() << " bytes\n";
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
    return 0;
}