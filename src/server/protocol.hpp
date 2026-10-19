#pragma once

#include "medgl/renderer.hpp"
#include "medgl/geometry.hpp"
#include "appearance_presets.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace medgl::server {

using Json = nlohmann::json;
constexpr std::size_t JsonLimit = 64 * 1024;
constexpr std::size_t ImageLimit = 32 * 1024 * 1024;
constexpr std::uint32_t ViewportLimit = 2048;
constexpr std::uint64_t SafeIntegerLimit = 9007199254740991ULL;

class InputError : public std::runtime_error {
public:
    explicit InputError(const char* message) : std::runtime_error(message) {}
};

inline Json ParseJson(const std::string& text) {
    if (text.size() > JsonLimit) {
        throw InputError("JSON exceeds its allowed size.");
    }
    auto result = Json::parse(text,
        [](int depth, Json::parse_event_t, Json&) {
            if (depth > 16) throw InputError("JSON exceeds its allowed nesting depth.");
            return true;
        }, false);
    if (result.is_discarded()) throw InputError("Malformed JSON request.");
    return result;
}

inline const Json& Required(const Json& object, const char* name) {
    if (!object.is_object() || !object.contains(name)) {
        throw InputError("A required field is missing.");
    }
    return object.at(name);
}

inline std::string Text(const Json& object, const char* name, std::size_t limit = 128) {
    const auto& value = Required(object, name);
    if (!value.is_string()) {
        throw InputError("Expected a string field.");
    }
    auto result = value.get<std::string>();
    if (result.empty() || result.size() > limit) {
        throw InputError("String field is outside its allowed size.");
    }
    return result;
}

inline std::uint64_t Integer(const Json& value, std::uint64_t maximum) {
    if (!value.is_number_integer() ||
        (!value.is_number_unsigned() && value.get<std::int64_t>() < 0)) {
        throw InputError("Expected a nonnegative integer.");
    }
    const auto result = value.get<std::uint64_t>();
    if (result > maximum) {
        throw InputError("Integer field is outside its allowed range.");
    }
    return result;
}

inline float Number(const Json& value, float minimum, float maximum) {
    if (!value.is_number()) {
        throw InputError("Expected a numeric field.");
    }
    const double result = value.get<double>();
    if (!std::isfinite(result) || result < static_cast<double>(minimum) ||
        result > static_cast<double>(maximum)) {
        throw InputError("Numeric field is outside its allowed range.");
    }
    return static_cast<float>(result);
}

template<std::size_t Count>
std::array<float, Count> Numbers(const Json& value, float minimum, float maximum) {
    if (!value.is_array() || value.size() != Count) {
        throw InputError("Array field has an invalid size.");
    }
    std::array<float, Count> result{};
    for (std::size_t index = 0; index < Count; ++index) {
        result[index] = Number(value.at(index), minimum, maximum);
    }
    return result;
}

inline const AppearancePreset& FindPreset(const std::string& name) {
    const auto canonical = name == "ct-soft" ? "ct-soft-tissue" : name;
    for (const auto& preset : AppearancePresets())
        if (preset.id == canonical) return preset;
    throw InputError("Unknown transfer-function preset.");
}

inline std::vector<TransferPoint> Preset(const std::string& name) {
    return FindPreset(name).transfer;
}

inline void ApplyPreset(RenderSettings& settings, const std::string& name) {
    const auto& preset = FindPreset(name);
    settings.transfer = preset.transfer;
    settings.windowWidth = preset.windowWidth;
    settings.windowLevel = preset.windowLevel;
    settings.opacityScale = preset.opacityScale;
    settings.lighting = preset.lighting;
}

inline Json TransferJson(const std::vector<TransferPoint>& points) {
    Json result = Json::array();
    for (const auto& point : points) result.push_back({{"position", point.position}, {"rgba", point.rgba}});
    return result;
}

inline Json VolumeHistogramJson(const VolumeData& volume) {
    if (volume.voxels.empty()) throw InputError("Histogram requires a volume");
    double minimum = std::numeric_limits<double>::infinity();
    double maximum = -minimum;
    for (float value : volume.voxels) {
        if (!std::isfinite(value)) throw InputError("Histogram requires finite intensities");
        minimum = std::min(minimum, static_cast<double>(value));
        maximum = std::max(maximum, static_cast<double>(value));
    }
    std::array<std::uint64_t, 256> counts{};
    for (float value : volume.voxels) {
        const auto index = maximum == minimum ? 0 : std::min<std::size_t>(255,
            static_cast<std::size_t>((value - minimum) / (maximum - minimum) * counts.size()));
        ++counts[index];
    }
    return {{"minimum", minimum}, {"maximum", maximum}, {"counts", counts},
        {"voxelCount", volume.voxels.size()}};
}

inline Json PresetCatalogJson() {
    Json catalog = Json::array();
    for (const auto& preset : AppearancePresets()) {
        Json colors = Json::array();
        for (const auto& point : preset.transfer)
            colors.push_back({point.rgba[0], point.rgba[1], point.rgba[2]});
        catalog.push_back({{"id", preset.id}, {"name", preset.name},
            {"windowWidth", preset.windowWidth}, {"windowLevel", preset.windowLevel},
            {"opacityScale", preset.opacityScale}, {"lighting", preset.lighting},
            {"colors", std::move(colors)}, {"transferFunction", TransferJson(preset.transfer)}});
    }
    return {{"presets", std::move(catalog)}};
}

struct RenderRequest {
    std::uint64_t seq = 0;
    std::uint64_t revision = 0;
    bool finalQuality = false;
    RenderSettings settings;
    std::chrono::steady_clock::time_point receivedAt = std::chrono::steady_clock::now();
};

inline RenderRequest ParseRender(const Json& message) {
    if (!message.is_object()) {
        throw InputError("Expected a render object.");
    }
    if (message.contains("type") && Text(message, "type") != "render") {
        throw InputError("Unknown message type.");
    }
    if (message.contains("protocolVersion") &&
        Integer(message.at("protocolVersion"), SafeIntegerLimit) != 1) {
        throw InputError("Unsupported protocol version.");
    }
    RenderRequest result;
    result.seq = Integer(Required(message, "seq"), SafeIntegerLimit);
    result.revision = Integer(Required(message, "sceneRevision"), SafeIntegerLimit);
    const auto quality = Text(message, "quality");
    if (quality != "preview" && quality != "final") {
        throw InputError("Unknown render quality.");
    }
    result.finalQuality = quality == "final";
    result.settings.width = static_cast<std::uint32_t>(
        Integer(Required(message, "width"), ViewportLimit));
    result.settings.height = static_cast<std::uint32_t>(
        Integer(Required(message, "height"), ViewportLimit));
    if (result.settings.width == 0 || result.settings.height == 0) {
        throw InputError("Viewport dimensions must be positive.");
    }
    const auto& camera = Required(message, "camera");
    result.settings.target = Numbers<3>(Required(camera, "targetMm"), -100000, 100000);
    result.settings.rotation = Numbers<4>(Required(camera, "rotationXyzw"), -1, 1);
    double normSquared = 0;
    for (const auto component : result.settings.rotation) {
        normSquared += static_cast<double>(component) * component;
    }
    if (std::abs(normSquared - 1.0) > 0.01) {
        throw InputError("Camera quaternion must have unit length.");
    }
    const auto inverseNorm = static_cast<float>(1.0 / std::sqrt(normSquared));
    for (auto& component : result.settings.rotation) {
        component *= inverseNorm;
    }
    result.settings.orthoHeight = Number(Required(camera, "orthoHeightMm"), 0.1f, 10000);
    const auto& style = Required(message, "style");
    result.settings.transfer = Preset(Text(style, "presetId"));
    result.settings.windowWidth = Number(Required(style, "windowWidth"), 0.01f, 100000);
    result.settings.windowLevel = Number(Required(style, "windowLevel"), -100000, 100000);
    result.settings.opacityScale = Number(Required(style, "opacityScale"), 0, 10);
    result.settings.background = Numbers<3>(Required(style, "backgroundRgb"), 0, 1);
    if (style.contains("lighting")) {
        if (!style.at("lighting").is_boolean()) {
            throw InputError("Lighting must be a boolean.");
        }
        result.settings.lighting = style.at("lighting").get<bool>();
    }
    if (style.contains("transferFunction")) {
        const auto& points = style.at("transferFunction");
        if (!points.is_array() || points.size() < 2 || points.size() > 64) {
            throw InputError("Transfer function must have between 2 and 64 points.");
        }
        result.settings.transfer.clear();
        float previous = -1;
        for (const auto& point : points) {
            const auto position = Number(Required(point, "position"), 0, 1);
            if (position <= previous) {
                throw InputError("Transfer positions must be strictly increasing.");
            }
            result.settings.transfer.push_back(
                {position, Numbers<4>(Required(point, "rgba"), 0, 1)});
            previous = position;
        }
    }
    if (message.contains("cropBox")) {
        const auto& crop = message.at("cropBox");
        const auto& enabled = Required(crop, "enabled");
        if (!enabled.is_boolean()) throw InputError("Crop enabled must be a boolean.");
        result.settings.cropBox.enabled = enabled.get<bool>();
        result.settings.cropBox.minimum = Numbers<3>(Required(crop, "minimum"), 0, 1);
        result.settings.cropBox.maximum = Numbers<3>(Required(crop, "maximum"), 0, 1);
        for (std::size_t axis = 0; axis < 3; ++axis)
            if (result.settings.cropBox.minimum[axis] >= result.settings.cropBox.maximum[axis])
                throw InputError("Crop minimum must be less than maximum.");
    }
    if (message.contains("clipPlane")) {
        const auto& plane = message.at("clipPlane");
        const auto& enabled = Required(plane, "enabled");
        const auto& keepPositive = Required(plane, "keepPositive");
        if (!enabled.is_boolean() || !keepPositive.is_boolean())
            throw InputError("Clip plane flags must be booleans.");
        auto& clip = result.settings.clipPlane;
        clip.enabled = enabled.get<bool>();
        clip.keepPositive = keepPositive.get<bool>();
        clip.normal = Numbers<3>(Required(plane, "normal"), -1, 1);
        double lengthSquared = 0;
        for (float component : clip.normal) lengthSquared += static_cast<double>(component) * component;
        if (std::abs(lengthSquared - 1) > 0.001) throw InputError("Clip plane normal must have unit length.");
        const auto inverse = static_cast<float>(1 / std::sqrt(lengthSquared));
        for (auto& component : clip.normal) component *= inverse;
        clip.offsetMm = Number(Required(plane, "offsetMm"), -1000000, 1000000);
    }
    result.settings.stepMm = result.finalQuality ? 0.5f : 1.5f;
    result.settings.referenceStepMm = 1;
    return result;
}

inline Json CropBoxJson(const RenderSettings& settings) {
    return {{"enabled", settings.cropBox.enabled}, {"minimum", settings.cropBox.minimum},
        {"maximum", settings.cropBox.maximum}};
}

inline Json ClipPlaneJson(const RenderSettings& settings) {
    const auto& plane = settings.clipPlane;
    return {{"enabled", plane.enabled}, {"normal", plane.normal}, {"offsetMm", plane.offsetMm},
        {"keepPositive", plane.keepPositive}};
}

inline Json CameraJson(const RenderSettings& settings) {
    return {{"targetMm", settings.target}, {"rotationXyzw", settings.rotation},
            {"orthoHeightMm", settings.orthoHeight}};
}

inline Json StandardViewsJson() {
    const StandardView views[]{StandardView::Anterior, StandardView::Posterior, StandardView::Left,
        StandardView::Right, StandardView::Superior, StandardView::Inferior};
    const char* ids[]{"anterior", "posterior", "left", "right", "superior", "inferior"};
    const char* names[]{"Anterior", "Posterior", "Left", "Right", "Superior", "Inferior"};
    Json result = Json::array();
    for (int index = 0; index < 6; ++index)
        result.push_back({{"id", ids[index]}, {"name", names[index]},
            {"rotationXyzw", StandardViewRotation(views[index])}});
    return result;
}

inline Json StyleJson(const RenderSettings& settings) {
    return {{"presetId", "ct-bone"}, {"windowWidth", settings.windowWidth},
            {"windowLevel", settings.windowLevel}, {"opacityScale", settings.opacityScale},
            {"lighting", settings.lighting}, {"backgroundRgb", settings.background},
            {"transferFunction", TransferJson(settings.transfer)}};
}

inline Json ErrorJson(const char* code, const char* message) {
    return {{"error", {{"code", code}, {"message", message}}}};
}

}