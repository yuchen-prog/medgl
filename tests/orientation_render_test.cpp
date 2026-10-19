#include "medgl/geometry.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {

double Dot(const medgl::Point3d& first, const medgl::Point3d& second)
{
    return first[0] * second[0] + first[1] * second[1] + first[2] * second[2];
}

void Verify(const std::array<float, 9>& direction)
{
    medgl::VolumeData volume;
    volume.shape = {17, 17, 17};
    volume.spacing = {0.8f, 1.3f, 2};
    volume.origin = {50, -40, 12};
    volume.direction = direction;
    volume.voxels.assign(volume.shape.VoxelCount(), -1000);
    const medgl::Point3d markers[]{{4, 7, 6}, {12, 3, 10}, {9, 12, 13}};
    medgl::VolumeRenderer renderer;
    const medgl::StandardView views[]{medgl::StandardView::Anterior, medgl::StandardView::Posterior,
        medgl::StandardView::Left, medgl::StandardView::Right, medgl::StandardView::Superior, medgl::StandardView::Inferior};
    const medgl::Point3d rights[]{{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {-1, 0, 0}, {1, 0, 0}};
    const medgl::Point3d upAxes[]{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};
    const auto target = medgl::VoxelToPatient(volume, {8, 8, 8});
    for (int view = 0; view < 6; ++view) {
        auto settings = medgl::FitView(volume);
        settings.width = 129;
        settings.height = 97;
        settings.orthoHeight = 60;
        settings.windowWidth = 2000;
        settings.windowLevel = 0;
        settings.stepMm = 0.2f;
        settings.rotation = medgl::StandardViewRotation(views[view]);
        settings.transfer = {{0, {0, 0, 0, 0}}, {0.3f, {1, 1, 1, 0}},
            {0.55f, {1, 1, 1, 0.25f}}, {1, {1, 1, 1, 0.25f}}};
        const double pixelMm = settings.orthoHeight / settings.height;
        for (int marker = 0; marker < 3; ++marker) {
            auto isolated = volume;
            for (int slice = -1; slice <= 1; ++slice)
                for (int row = -1; row <= 1; ++row)
                    for (int column = -1; column <= 1; ++column)
                        isolated.voxels[isolated.shape.LinearIndex(
                            static_cast<std::uint32_t>(static_cast<int>(markers[marker][0]) + column),
                            static_cast<std::uint32_t>(static_cast<int>(markers[marker][1]) + row),
                            static_cast<std::uint32_t>(static_cast<int>(markers[marker][2]) + slice))] = 100;
            renderer.Load(isolated);
            const auto image = renderer.Render(settings);
            const auto patient = medgl::VoxelToPatient(volume, markers[marker]);
            const medgl::Point3d delta{patient[0] - target[0], patient[1] - target[1], patient[2] - target[2]};
            const double expectedColumn = (settings.width - 1) / 2.0 + Dot(delta, rights[view]) / pixelMm;
            const double expectedRow = (settings.height - 1) / 2.0 - Dot(delta, upAxes[view]) / pixelMm;
            double weight = 0, columnSum = 0, rowSum = 0;
            for (std::uint32_t row = 0; row < image.height; ++row) {
                for (std::uint32_t column = 0; column < image.width; ++column) {
                    const auto offset = (static_cast<std::size_t>(row) * image.width + column) * 3;
                    const int value = image.pixels[offset];
                    if (value > 40) {
                        weight += value;
                        columnSum += column * value;
                        rowSum += row * value;
                    }
                }
            }
            if (weight == 0 || std::abs(columnSum / weight - expectedColumn) > 2 || std::abs(rowSum / weight - expectedRow) > 2) {
                std::cerr << "view=" << view << " marker=" << marker << " weight=" << weight
                          << " expected=" << expectedColumn << ',' << expectedRow
                          << " actual=" << (weight ? columnSum / weight : -1) << ',' << (weight ? rowSum / weight : -1) << '\n';
                throw std::runtime_error("GPU LPS marker projection is mirrored, swapped or shifted");
            }
        }
    }
}

}

int main()
{
    try {
        Verify({0.6f, -0.8f, 0, 0.8f, 0.6f, 0, 0, 0, 1});
        Verify({0, 0, 1, 1, 0, 0, 0, 1, 0});
        std::cout << "[PASS] Six GPU views: isolated asymmetric markers, oblique/permuted axes, anisotropic spacing\n";
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
    return 0;
}