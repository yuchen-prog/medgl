#pragma once

#include "medgl/renderer.hpp"

#include <string>
#include <vector>

namespace medgl::server {

struct AppearancePreset {
    std::string id;
    std::string name;
    float windowWidth;
    float windowLevel;
    float opacityScale;
    bool lighting;
    std::vector<TransferPoint> transfer;
};

inline const std::vector<AppearancePreset>& AppearancePresets() {
    static const std::vector<AppearancePreset> presets{
        {"ct-bone", "CT / Bone", 1800, 400, 1, true,
            {{0, {0, 0, 0, 0}}, {0.35f, {0.55f, 0.32f, 0.19f, 0}},
             {0.5f, {0.82f, 0.66f, 0.44f, 0.035f}}, {0.7f, {0.95f, 0.87f, 0.68f, 0.09f}},
             {1, {1, 0.98f, 0.9f, 0.28f}}}},
        {"ct-heart", "CT / Heart (contrast)", 800, 200, 1, true,
            {{0, {0, 0, 0, 0}}, {0.25f, {0.35f, 0.05f, 0.04f, 0}},
             {0.35f, {0.65f, 0.12f, 0.08f, 0.01f}}, {0.5f, {0.9f, 0.3f, 0.18f, 0.035f}},
             {0.75f, {1, 0.6f, 0.4f, 0.08f}}, {1, {1, 0.9f, 0.72f, 0.1f}}}},
        {"ct-lung", "CT / Lung", 1500, -600, 1, true,
            {{0, {0, 0, 0, 0}}, {0.266667f, {0.12f, 0.25f, 0.32f, 0}},
             {0.3f, {0.3f, 0.48f, 0.58f, 0.0015f}}, {0.366667f, {0.5f, 0.7f, 0.8f, 0.008f}},
             {0.466667f, {0.75f, 0.9f, 0.94f, 0.025f}}, {0.7f, {0.8f, 0.6f, 0.5f, 0.008f}},
             {0.833333f, {0.7f, 0.35f, 0.25f, 0.002f}}, {1, {0.7f, 0.35f, 0.25f, 0}}}},
        {"ct-skin", "CT / Skin", 800, -200, 1, true,
            {{0, {0, 0, 0, 0}}, {0.35f, {0.5f, 0.27f, 0.16f, 0}},
             {0.5f, {0.8f, 0.46f, 0.3f, 0.018f}}, {0.65f, {0.96f, 0.7f, 0.5f, 0.04f}},
             {0.8f, {1, 0.82f, 0.64f, 0.03f}}, {1, {1, 0.9f, 0.75f, 0.08f}}}},
        {"ct-soft-tissue", "CT / Soft tissue", 400, 40, 1, true,
            {{0, {0, 0, 0, 0}}, {0.15f, {0.3f, 0.12f, 0.09f, 0}},
             {0.35f, {0.65f, 0.3f, 0.22f, 0.008f}}, {0.6f, {0.9f, 0.58f, 0.44f, 0.025f}},
             {0.8f, {1, 0.8f, 0.65f, 0.05f}}, {1, {1, 0.9f, 0.8f, 0.06f}}}},
        {"ct-angiography", "CT / Vessels (contrast)", 1000, 300, 1, true,
            {{0, {0, 0, 0, 0}}, {0.35f, {0.4f, 0.04f, 0.03f, 0}},
             {0.5f, {0.85f, 0.15f, 0.08f, 0.025f}}, {0.7f, {1, 0.45f, 0.2f, 0.1f}},
             {1, {1, 0.88f, 0.62f, 0.2f}}}},
        {"ct-grayscale", "CT / Grayscale", 1000, 400, 1, true,
            {{0, {0, 0, 0, 0}}, {0.35f, {0.35f, 0.35f, 0.35f, 0}},
             {0.55f, {0.65f, 0.65f, 0.65f, 0.02f}}, {1, {1, 1, 1, 0.25f}}}},
        {"ct-all", "CT / Composite", 2000, 300, 1, true,
            {{0, {0, 0, 0, 0}}, {0.2f, {0.3f, 0.15f, 0.1f, 0}},
             {0.4f, {0.65f, 0.3f, 0.2f, 0.01f}}, {0.6f, {0.9f, 0.65f, 0.45f, 0.04f}},
             {0.8f, {0.98f, 0.88f, 0.72f, 0.12f}}, {1, {1, 1, 0.94f, 0.3f}}}}
    };
    return presets;
}

}