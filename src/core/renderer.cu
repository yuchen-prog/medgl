#include "medgl/renderer.hpp"

#include <cuda_runtime.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <string>

namespace medgl {
namespace {

using TimerClock = std::chrono::steady_clock;

double Milliseconds(TimerClock::time_point start, TimerClock::time_point end)
{
    return std::chrono::duration<double, std::milli>(end - start).count();
}

void Check(cudaError_t status, const char* operation)
{
    if (status != cudaSuccess) {
        throw std::runtime_error(std::string(operation) + ": " + cudaGetErrorString(status));
    }
}

struct Parameters {
    cudaTextureObject_t texture;
    float3 size;
    float3 cropMinimum;
    float3 cropMaximum;
    bool clipEnabled;
    float3 clipNormal;
    float clipConstant;
    float3 rayStart;
    float3 pixelRight;
    float3 pixelDown;
    float3 rayDirection;
    float step;
    float referenceStep;
    float opacity;
    float windowWidth;
    float windowLevel;
    float3 background;
    float3 spacing;
    float3 lightDirection;
    bool lighting;
    int width;
    int height;
};

__device__ float3 Add(float3 first, float3 second)
{
    return make_float3(first.x + second.x, first.y + second.y, first.z + second.z);
}

__device__ float3 Scale(float3 value, float factor)
{
    return make_float3(value.x * factor, value.y * factor, value.z * factor);
}

__device__ float Sample(cudaTextureObject_t texture, float3 point, float3 size)
{
    return tex3D<float>(texture, (point.x + 0.5f) / size.x,
        (point.y + 0.5f) / size.y, (point.z + 0.5f) / size.z);
}

__device__ bool IntersectAxis(float start, float direction, float minimum, float maximum,
                             float& nearDistance, float& farDistance)
{
    if (fabsf(direction) < 1e-8f) {
        return start >= minimum && start <= maximum;
    }
    const float first = (minimum - start) / direction;
    const float second = (maximum - start) / direction;
    nearDistance = fmaxf(nearDistance, fminf(first, second));
    farDistance = fminf(farDistance, fmaxf(first, second));
    return nearDistance < farDistance;
}

__global__ void RenderKernel(Parameters parameters, const float4* lookup, unsigned char* image)
{
    const int column = blockIdx.x * blockDim.x + threadIdx.x;
    const int row = blockIdx.y * blockDim.y + threadIdx.y;
    if (column >= parameters.width || row >= parameters.height) {
        return;
    }
    const float3 start = Add(parameters.rayStart,
        Add(Scale(parameters.pixelRight, static_cast<float>(column)),
            Scale(parameters.pixelDown, static_cast<float>(row))));
    float nearDistance = 0;
    float farDistance = 1e20f;
    bool intersects = IntersectAxis(start.x, parameters.rayDirection.x, parameters.cropMinimum.x, parameters.cropMaximum.x, nearDistance, farDistance)
        && IntersectAxis(start.y, parameters.rayDirection.y, parameters.cropMinimum.y, parameters.cropMaximum.y, nearDistance, farDistance)
        && IntersectAxis(start.z, parameters.rayDirection.z, parameters.cropMinimum.z, parameters.cropMaximum.z, nearDistance, farDistance);
    if (intersects && parameters.clipEnabled) {
        const float signedStart = start.x * parameters.clipNormal.x + start.y * parameters.clipNormal.y
            + start.z * parameters.clipNormal.z + parameters.clipConstant;
        const float rate = parameters.rayDirection.x * parameters.clipNormal.x
            + parameters.rayDirection.y * parameters.clipNormal.y + parameters.rayDirection.z * parameters.clipNormal.z;
        if (fabsf(rate) < 1e-7f) {
            intersects = signedStart >= 0;
        } else {
            const float crossing = -signedStart / rate;
            if (rate > 0) nearDistance = fmaxf(nearDistance, crossing);
            else farDistance = fminf(farDistance, crossing);
            intersects = nearDistance < farDistance;
        }
    }
    float3 color = make_float3(0, 0, 0);
    float alpha = 0;
    if (intersects) {
        for (float distance = nearDistance; distance < farDistance && alpha < 0.995f; distance += parameters.step) {
            const float length = fminf(parameters.step, farDistance - distance);
            const float3 point = Add(start, Scale(parameters.rayDirection, distance + length * 0.5f));
            const float intensity = Sample(parameters.texture, point, parameters.size);
            const float position = fminf(1.0f, fmaxf(0.0f, (intensity - parameters.windowLevel) / parameters.windowWidth + 0.5f)) * 255;
            const int lower = static_cast<int>(position);
            const float blend = position - lower;
            const float4 first = lookup[lower];
            const float4 second = lookup[min(lower + 1, 255)];
            const float4 sample = make_float4(first.x + blend * (second.x - first.x),
                first.y + blend * (second.y - first.y), first.z + blend * (second.z - first.z),
                first.w + blend * (second.w - first.w));
            const float exponent = length / parameters.referenceStep * parameters.opacity;
            const float sampleAlpha = exponent == 0 ? 0 : 1 - powf(1 - sample.w, exponent);
            const float weight = (1 - alpha) * sampleAlpha;
            float illumination = 1;
            if (parameters.lighting && sampleAlpha > 0.001f) {
                const float3 gradient = make_float3(
                    (Sample(parameters.texture, Add(point, make_float3(1, 0, 0)), parameters.size) - Sample(parameters.texture, Add(point, make_float3(-1, 0, 0)), parameters.size)) / parameters.spacing.x,
                    (Sample(parameters.texture, Add(point, make_float3(0, 1, 0)), parameters.size) - Sample(parameters.texture, Add(point, make_float3(0, -1, 0)), parameters.size)) / parameters.spacing.y,
                    (Sample(parameters.texture, Add(point, make_float3(0, 0, 1)), parameters.size) - Sample(parameters.texture, Add(point, make_float3(0, 0, -1)), parameters.size)) / parameters.spacing.z);
                const float magnitude = sqrtf(gradient.x * gradient.x + gradient.y * gradient.y + gradient.z * gradient.z);
                if (magnitude > 1e-5f) {
                    const float diffuse = fabsf((gradient.x * parameters.lightDirection.x + gradient.y * parameters.lightDirection.y + gradient.z * parameters.lightDirection.z) / magnitude);
                    illumination = 0.25f + 0.7f * diffuse + 0.15f * powf(diffuse, 24);
                }
            }
            color = Add(color, make_float3(sample.x * weight * illumination, sample.y * weight * illumination, sample.z * weight * illumination));
            alpha += weight;
        }
    }
    color = Add(color, Scale(parameters.background, 1 - alpha));
    const auto offset = (static_cast<std::size_t>(row) * parameters.width + column) * 3;
    image[offset] = static_cast<unsigned char>(fminf(255, fmaxf(0, color.x * 255 + 0.5f)));
    image[offset + 1] = static_cast<unsigned char>(fminf(255, fmaxf(0, color.y * 255 + 0.5f)));
    image[offset + 2] = static_cast<unsigned char>(fminf(255, fmaxf(0, color.z * 255 + 0.5f)));
}

float3 ToFloat3(const std::array<float, 3>& value)
{
    return make_float3(value[0], value[1], value[2]);
}

std::array<float, 3> Rotate(const std::array<float, 4>& quaternion, std::array<float, 3> value)
{
    double norm = 0;
    for (float component : quaternion) {
        if (!std::isfinite(component)) throw std::invalid_argument("Invalid camera rotation");
        norm += static_cast<double>(component) * component;
    }
    if (norm < 1e-12) throw std::invalid_argument("Camera quaternion must be nonzero");
    const float inverse = static_cast<float>(1 / std::sqrt(norm));
    const float horizontal = quaternion[0] * inverse;
    const float vertical = quaternion[1] * inverse;
    const float depth = quaternion[2] * inverse;
    const float scalar = quaternion[3] * inverse;
    const std::array<float, 3> cross{2 * (vertical * value[2] - depth * value[1]),
        2 * (depth * value[0] - horizontal * value[2]), 2 * (horizontal * value[1] - vertical * value[0])};
    return {value[0] + scalar * cross[0] + vertical * cross[2] - depth * cross[1],
        value[1] + scalar * cross[1] + depth * cross[0] - horizontal * cross[2],
        value[2] + scalar * cross[2] + horizontal * cross[1] - vertical * cross[0]};
}

float3 PatientToLocal(const std::array<float, 9>& direction, std::array<float, 3> value)
{
    return make_float3(direction[0] * value[0] + direction[3] * value[1] + direction[6] * value[2],
        direction[1] * value[0] + direction[4] * value[1] + direction[7] * value[2],
        direction[2] * value[0] + direction[5] * value[1] + direction[8] * value[2]);
}

}

struct VolumeRenderer::Impl {
    cudaArray_t array = nullptr;
    cudaTextureObject_t texture = 0;
    unsigned char* output = nullptr;
    std::size_t outputCapacity = 0;
    float4* lookup = nullptr;
    cudaEvent_t kernelStart = nullptr;
    cudaEvent_t kernelEnd = nullptr;
    RenderTimings renderTimings;
    LoadTimings loadTimings;
    VolumeShape shape;
    std::array<float, 3> spacing{};
    std::array<float, 3> origin{};
    std::array<float, 9> direction{};

    ~Impl()
    {
        if (texture) cudaDestroyTextureObject(texture);
        if (array) cudaFreeArray(array);
        if (output) cudaFree(output);
        if (lookup) cudaFree(lookup);
        if (kernelStart) cudaEventDestroy(kernelStart);
        if (kernelEnd) cudaEventDestroy(kernelEnd);
    }
};

VolumeRenderer::VolumeRenderer() : m_impl(std::make_unique<Impl>()) {}
VolumeRenderer::~VolumeRenderer() = default;

void VolumeRenderer::Unload()
{
    m_impl = std::make_unique<Impl>();
}

void VolumeRenderer::Load(const VolumeData& volume)
{
    const auto loadStart = TimerClock::now();
    if (volume.voxels.size() != volume.shape.VoxelCount()) {
        throw std::invalid_argument("Volume buffer size mismatch");
    }
    for (float spacing : volume.spacing) {
        if (!std::isfinite(spacing) || spacing <= 0) throw std::invalid_argument("Invalid spacing");
    }
    for (float component : volume.origin) if (!std::isfinite(component)) throw std::invalid_argument("Invalid origin");
    for (float component : volume.direction) if (!std::isfinite(component)) throw std::invalid_argument("Invalid direction");
    for (int first = 0; first < 3; ++first) {
        for (int second = 0; second < 3; ++second) {
            float dot = 0;
            for (int row = 0; row < 3; ++row) dot += volume.direction[row * 3 + first] * volume.direction[row * 3 + second];
            if (std::abs(dot - (first == second ? 1.0f : 0.0f)) > 1e-4f)
                throw std::invalid_argument("Volume directions must be orthonormal");
        }
    }
    for (float voxel : volume.voxels) if (!std::isfinite(voxel)) throw std::invalid_argument("Non-finite voxel");
    const auto validationEnd = TimerClock::now();
    auto replacement = std::make_unique<Impl>();
    replacement->shape = volume.shape;
    replacement->spacing = volume.spacing;
    replacement->origin = volume.origin;
    replacement->direction = volume.direction;
    const auto extent = make_cudaExtent(volume.shape.width, volume.shape.height, volume.shape.depth);
    const auto description = cudaCreateChannelDesc<float>();
    Check(cudaMalloc3DArray(&replacement->array, &description, extent), "cudaMalloc3DArray");
    cudaMemcpy3DParms copy{};
    copy.srcPtr = make_cudaPitchedPtr(const_cast<float*>(volume.voxels.data()),
        volume.shape.width * sizeof(float), volume.shape.width, volume.shape.height);
    copy.dstArray = replacement->array;
    copy.extent = extent;
    copy.kind = cudaMemcpyHostToDevice;
    Check(cudaMemcpy3D(&copy), "Volume upload");
    cudaResourceDesc resource{};
    resource.resType = cudaResourceTypeArray;
    resource.res.array.array = replacement->array;
    cudaTextureDesc texture{};
    texture.normalizedCoords = 1;
    texture.filterMode = cudaFilterModeLinear;
    texture.readMode = cudaReadModeElementType;
    texture.addressMode[0] = texture.addressMode[1] = texture.addressMode[2] = cudaAddressModeClamp;
    Check(cudaCreateTextureObject(&replacement->texture, &resource, &texture, nullptr), "Create volume texture");
    Check(cudaMalloc(reinterpret_cast<void**>(&replacement->lookup), sizeof(float4) * 256), "Allocate transfer lookup");
    Check(cudaEventCreate(&replacement->kernelStart), "Create kernel start timer");
    Check(cudaEventCreate(&replacement->kernelEnd), "Create kernel end timer");
    m_impl = std::move(replacement);
    const auto loadEnd = TimerClock::now();
    m_impl->loadTimings = {Milliseconds(loadStart, validationEnd), Milliseconds(validationEnd, loadEnd),
        Milliseconds(loadStart, loadEnd)};
}

RgbImage VolumeRenderer::Render(const RenderSettings& settings)
{
    m_impl->renderTimings = {};
    const auto renderStart = TimerClock::now();
    if (!m_impl->texture) throw std::logic_error("No volume loaded");
    if (settings.width == 0 || settings.height == 0 || settings.width > 2048 || settings.height > 2048)
        throw std::invalid_argument("Viewport must be between 1 and 2048 pixels");
    if (!std::isfinite(settings.stepMm) || settings.stepMm < 0.02f || !std::isfinite(settings.referenceStepMm)
        || settings.referenceStepMm <= 0 || !std::isfinite(settings.windowWidth) || settings.windowWidth <= 0
        || !std::isfinite(settings.windowLevel) || !std::isfinite(settings.orthoHeight) || settings.orthoHeight <= 0
        || !std::isfinite(settings.opacityScale) || settings.opacityScale < 0 || settings.opacityScale > 10)
        throw std::invalid_argument("Invalid render settings");
    if (settings.transfer.size() < 2) throw std::invalid_argument("Transfer function needs two points");
    if (settings.transfer.size() > 128) throw std::invalid_argument("Too many transfer points");
    for (float component : settings.target) if (!std::isfinite(component)) throw std::invalid_argument("Invalid target");
    for (float component : settings.background) if (!std::isfinite(component) || component < 0 || component > 1)
        throw std::invalid_argument("Invalid background");
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto minimum = settings.cropBox.minimum[axis];
        const auto maximum = settings.cropBox.maximum[axis];
        if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum < 0 || maximum > 1 || minimum >= maximum)
            throw std::invalid_argument("Crop ranges must be ordered within [0,1]");
    }
    double normalLengthSquared = 0;
    for (float component : settings.clipPlane.normal) {
        if (!std::isfinite(component)) throw std::invalid_argument("Invalid clip plane normal");
        normalLengthSquared += static_cast<double>(component) * component;
    }
    if (std::abs(normalLengthSquared - 1) > 0.001 || !std::isfinite(settings.clipPlane.offsetMm)
        || std::abs(settings.clipPlane.offsetMm) > 1000000)
        throw std::invalid_argument("Clip plane needs a unit LPS normal and a finite offset");
    std::vector<float4> lookup(256);
    for (std::size_t index = 0; index < settings.transfer.size(); ++index) {
        const auto& point = settings.transfer[index];
        if (!std::isfinite(point.position) || point.position < 0 || point.position > 1
            || (index && point.position <= settings.transfer[index - 1].position))
            throw std::invalid_argument("Transfer points must be strictly ordered in [0,1]");
        for (float component : point.rgba) if (!std::isfinite(component) || component < 0 || component > 1)
            throw std::invalid_argument("Transfer components must be in [0,1]");
    }
    for (int index = 0; index < 256; ++index) {
        const float position = index / 255.0f;
        auto upper = std::upper_bound(settings.transfer.begin(), settings.transfer.end(), position,
            [](float value, const TransferPoint& point) { return value < point.position; });
        const auto& first = upper == settings.transfer.begin() ? settings.transfer.front() : *(upper - 1);
        const auto& second = upper == settings.transfer.end() ? settings.transfer.back() : *upper;
        const float blend = second.position == first.position ? 0 : (position - first.position) / (second.position - first.position);
        lookup[index] = make_float4(first.rgba[0] + blend * (second.rgba[0] - first.rgba[0]),
            first.rgba[1] + blend * (second.rgba[1] - first.rgba[1]),
            first.rgba[2] + blend * (second.rgba[2] - first.rgba[2]),
            first.rgba[3] + blend * (second.rgba[3] - first.rgba[3]));
    }
    Check(cudaMemcpy(m_impl->lookup, lookup.data(), sizeof(float4) * 256, cudaMemcpyHostToDevice), "Upload transfer lookup");
    RgbImage image{settings.width, settings.height, {}};
    image.pixels.resize(image.RequiredBytes());
    if (image.RequiredBytes() > m_impl->outputCapacity) {
        unsigned char* replacement = nullptr;
        Check(cudaMalloc(reinterpret_cast<void**>(&replacement), image.RequiredBytes()), "Allocate frame");
        if (m_impl->output) cudaFree(m_impl->output);
        m_impl->output = replacement;
        m_impl->outputCapacity = image.RequiredBytes();
    }
    const float pixelSize = settings.orthoHeight / settings.height;
    const auto right = Rotate(settings.rotation, {1, 0, 0});
    const auto down = Rotate(settings.rotation, {0, -1, 0});
    const auto forward = Rotate(settings.rotation, {0, 0, 1});
    const float3 size = make_float3(static_cast<float>(m_impl->shape.width), static_cast<float>(m_impl->shape.height), static_cast<float>(m_impl->shape.depth));
    const float distance = std::sqrt(std::pow(size.x * m_impl->spacing[0], 2) + std::pow(size.y * m_impl->spacing[1], 2) + std::pow(size.z * m_impl->spacing[2], 2));
    Parameters parameters{};
    parameters.texture = m_impl->texture;
    parameters.size = size;
    parameters.cropMinimum = make_float3(-0.5f, -0.5f, -0.5f);
    parameters.cropMaximum = make_float3(size.x - 0.5f, size.y - 0.5f, size.z - 0.5f);
    if (settings.cropBox.enabled) {
        parameters.cropMinimum = make_float3(settings.cropBox.minimum[0] * size.x - 0.5f,
            settings.cropBox.minimum[1] * size.y - 0.5f, settings.cropBox.minimum[2] * size.z - 0.5f);
        parameters.cropMaximum = make_float3(settings.cropBox.maximum[0] * size.x - 0.5f,
            settings.cropBox.maximum[1] * size.y - 0.5f, settings.cropBox.maximum[2] * size.z - 0.5f);
    }
    std::array<float, 3> patientStart{};
    for (int axis = 0; axis < 3; ++axis) {
        patientStart[axis] = settings.target[axis] - m_impl->origin[axis] - forward[axis] * distance
            - right[axis] * (settings.width - 1) * pixelSize / 2 - down[axis] * (settings.height - 1) * pixelSize / 2;
    }
    const auto localStart = PatientToLocal(m_impl->direction, patientStart);
    const auto localRight = PatientToLocal(m_impl->direction, right);
    const auto localDown = PatientToLocal(m_impl->direction, down);
    const auto localForward = PatientToLocal(m_impl->direction, forward);
    parameters.rayStart = make_float3(localStart.x / m_impl->spacing[0], localStart.y / m_impl->spacing[1], localStart.z / m_impl->spacing[2]);
    parameters.pixelRight = make_float3(localRight.x * pixelSize / m_impl->spacing[0], localRight.y * pixelSize / m_impl->spacing[1], localRight.z * pixelSize / m_impl->spacing[2]);
    parameters.pixelDown = make_float3(localDown.x * pixelSize / m_impl->spacing[0], localDown.y * pixelSize / m_impl->spacing[1], localDown.z * pixelSize / m_impl->spacing[2]);
    parameters.rayDirection = make_float3(localForward.x / m_impl->spacing[0], localForward.y / m_impl->spacing[1], localForward.z / m_impl->spacing[2]);
    parameters.clipEnabled = settings.clipPlane.enabled;
    const auto localNormal = PatientToLocal(m_impl->direction, settings.clipPlane.normal);
    const float side = settings.clipPlane.keepPositive ? 1.0f : -1.0f;
    parameters.clipNormal = make_float3(localNormal.x * m_impl->spacing[0] * side,
        localNormal.y * m_impl->spacing[1] * side, localNormal.z * m_impl->spacing[2] * side);
    parameters.clipConstant = -side * settings.clipPlane.offsetMm
        - parameters.clipNormal.x * (size.x - 1) / 2
        - parameters.clipNormal.y * (size.y - 1) / 2
        - parameters.clipNormal.z * (size.z - 1) / 2;
    parameters.spacing = ToFloat3(m_impl->spacing);
    parameters.lightDirection = localForward;
    parameters.lighting = settings.lighting;
    parameters.step = settings.stepMm;
    parameters.referenceStep = settings.referenceStepMm;
    parameters.opacity = settings.opacityScale;
    parameters.windowWidth = settings.windowWidth;
    parameters.windowLevel = settings.windowLevel;
    parameters.background = ToFloat3(settings.background);
    parameters.width = static_cast<int>(settings.width);
    parameters.height = static_cast<int>(settings.height);
    const dim3 block(16, 16);
    const auto preparationEnd = TimerClock::now();
    Check(cudaEventRecord(m_impl->kernelStart), "Record kernel start");
    RenderKernel<<<dim3((settings.width + 15) / 16, (settings.height + 15) / 16), block>>>(parameters, m_impl->lookup, m_impl->output);
    Check(cudaGetLastError(), "Render launch");
    Check(cudaEventRecord(m_impl->kernelEnd), "Record kernel end");
    Check(cudaEventSynchronize(m_impl->kernelEnd), "Render execution");
    float kernelMs = 0;
    Check(cudaEventElapsedTime(&kernelMs, m_impl->kernelStart, m_impl->kernelEnd), "Measure kernel duration");
    const auto kernelEnd = TimerClock::now();
    Check(cudaMemcpy(image.pixels.data(), m_impl->output, image.RequiredBytes(), cudaMemcpyDeviceToHost), "Frame readback");
    const auto renderEnd = TimerClock::now();
    m_impl->renderTimings = {Milliseconds(renderStart, preparationEnd), kernelMs,
        Milliseconds(preparationEnd, kernelEnd), Milliseconds(kernelEnd, renderEnd),
        Milliseconds(renderStart, renderEnd)};
    return image;
}

RenderTimings VolumeRenderer::LastRenderTimings() const { return m_impl->renderTimings; }
LoadTimings VolumeRenderer::LastLoadTimings() const { return m_impl->loadTimings; }

RenderSettings FitView(const VolumeData& volume)
{
    volume.shape.VoxelCount();
    RenderSettings settings;
    const std::array<float, 3> center{(volume.shape.width - 1) * volume.spacing[0] / 2,
        (volume.shape.height - 1) * volume.spacing[1] / 2, (volume.shape.depth - 1) * volume.spacing[2] / 2};
    for (int row = 0; row < 3; ++row)
        settings.target[row] = volume.origin[row] + volume.direction[row * 3] * center[0]
            + volume.direction[row * 3 + 1] * center[1] + volume.direction[row * 3 + 2] * center[2];
    settings.orthoHeight = std::max({volume.shape.width * volume.spacing[0], volume.shape.height * volume.spacing[1], volume.shape.depth * volume.spacing[2]}) * 1.4f;
    return settings;
}

}