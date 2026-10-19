#pragma once

#include "medgl/data_contracts.hpp"

#include <array>
#include <memory>
#include <vector>

namespace medgl {

struct VolumeData {
    VolumeShape shape;
    std::array<float, 3> spacing{1, 1, 1};
    std::array<float, 3> origin{0, 0, 0};
    std::array<float, 9> direction{1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::vector<float> voxels;
};

struct TransferPoint {
    float position;
    std::array<float, 4> rgba;
};

struct CropBox {
    bool enabled = false;
    std::array<float, 3> minimum{0, 0, 0};
    std::array<float, 3> maximum{1, 1, 1};
};

struct ClipPlane {
    bool enabled = false;
    std::array<float, 3> normal{0, 0, 1};
    float offsetMm = 0;
    bool keepPositive = true;
};

struct RenderSettings {
    std::uint32_t width = 512;
    std::uint32_t height = 512;
    std::array<float, 3> target{0, 0, 0};
    std::array<float, 4> rotation{0, 0, 0, 1};
    float orthoHeight = 100;
    float windowWidth = 1000;
    float windowLevel = 400;
    float stepMm = 0.5f;
    float referenceStepMm = 1;
    float opacityScale = 1;
    bool lighting = false;
    CropBox cropBox;
    ClipPlane clipPlane;
    std::array<float, 3> background{0, 0, 0};
    std::vector<TransferPoint> transfer{{0, {0, 0, 0, 0}}, {1, {1, 1, 1, 0.1f}}};
};

struct RenderTimings {
    double prepareMs = 0;
    double kernelMs = 0;
    double kernelWallMs = 0;
    double readbackMs = 0;
    double totalMs = 0;
};

struct LoadTimings {
    double validationMs = 0;
    double uploadMs = 0;
    double totalMs = 0;
};

class VolumeRenderer {
public:
    VolumeRenderer();
    ~VolumeRenderer();
    VolumeRenderer(const VolumeRenderer&) = delete;
    VolumeRenderer& operator=(const VolumeRenderer&) = delete;
    void Load(const VolumeData& volume);
    void Unload();
    RgbImage Render(const RenderSettings& settings);
    RenderTimings LastRenderTimings() const;
    LoadTimings LastLoadTimings() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

RenderSettings FitView(const VolumeData& volume);
VolumeData MakePhantom();

}