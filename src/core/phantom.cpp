#include "medgl/renderer.hpp"

#include <cmath>

namespace medgl {

VolumeData MakePhantom()
{
    VolumeData volume;
    volume.shape = {96, 96, 96};
    volume.spacing = {1, 1, 1.2f};
    volume.voxels.assign(volume.shape.VoxelCount(), -1000);
    for (std::uint32_t slice = 0; slice < volume.shape.depth; ++slice) {
        for (std::uint32_t row = 0; row < volume.shape.height; ++row) {
            for (std::uint32_t column = 0; column < volume.shape.width; ++column) {
                const float horizontal = (static_cast<float>(column) - 47.5f) / 31;
                const float vertical = (static_cast<float>(row) - 47.5f) / 38;
                const float depth = (static_cast<float>(slice) - 47.5f) / 32;
                const float radius = horizontal * horizontal + vertical * vertical + depth * depth;
                float intensity = -1000;
                if (radius < 1) intensity = 50;
                if (radius > 0.7f && radius < 1) intensity = 900;
                const float organHorizontal = (static_cast<float>(column) - 38) / 10;
                const float organVertical = (static_cast<float>(row) - 48) / 15;
                const float organDepth = (static_cast<float>(slice) - 43) / 12;
                const float organ = organHorizontal * organHorizontal + organVertical * organVertical + organDepth * organDepth;
                if (organ < 1) intensity = 450;
                if (column > 53 && column < 61 && row > 41 && row < 54 && slice > 15 && slice < 78) intensity = 1250;
                volume.voxels[volume.shape.LinearIndex(column, row, slice)] = intensity;
            }
        }
    }
    return volume;
}

}