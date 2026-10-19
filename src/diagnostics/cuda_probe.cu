#include <cuda_runtime.h>

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void CheckCuda(cudaError_t result, const char* operation)
{
    if (result != cudaSuccess) {
        throw std::runtime_error(std::string(operation) + ": " + cudaGetErrorString(result));
    }
}

class DeviceFloatBuffer {
public:
    explicit DeviceFloatBuffer(std::size_t count)
    {
        CheckCuda(cudaMalloc(reinterpret_cast<void**>(&m_data), count * sizeof(float)),
                  "cudaMalloc");
    }

    ~DeviceFloatBuffer()
    {
        if (m_data != nullptr) {
            cudaFree(m_data);
        }
    }

    DeviceFloatBuffer(const DeviceFloatBuffer&) = delete;
    DeviceFloatBuffer& operator=(const DeviceFloatBuffer&) = delete;

    float* Data() const { return m_data; }

    void Release()
    {
        CheckCuda(cudaFree(m_data), "cudaFree");
        m_data = nullptr;
    }

private:
    float* m_data = nullptr;
};

__global__ void ScaleBiasKernel(const float* input, float* output, int count)
{
    const auto index = blockIdx.x * blockDim.x + threadIdx.x;
    if (index < static_cast<unsigned int>(count)) {
        output[index] = input[index] * 2.0f + 1.0f;
    }
}

void RunProbe()
{
    int deviceCount = 0;
    CheckCuda(cudaGetDeviceCount(&deviceCount), "cudaGetDeviceCount");
    if (deviceCount == 0) {
        throw std::runtime_error("No CUDA-capable device was detected");
    }
    CheckCuda(cudaSetDevice(0), "cudaSetDevice");

    cudaDeviceProp properties{};
    CheckCuda(cudaGetDeviceProperties(&properties, 0), "cudaGetDeviceProperties");
    int driverVersion = 0;
    int runtimeVersion = 0;
    CheckCuda(cudaDriverGetVersion(&driverVersion), "cudaDriverGetVersion");
    CheckCuda(cudaRuntimeGetVersion(&runtimeVersion), "cudaRuntimeGetVersion");
    std::size_t freeBytes = 0;
    std::size_t totalBytes = 0;
    CheckCuda(cudaMemGetInfo(&freeBytes, &totalBytes), "cudaMemGetInfo");
    std::cout << "Device 0: " << properties.name << '\n'
              << "Compute capability: " << properties.major << '.' << properties.minor << '\n'
              << "Driver/runtime API versions: " << driverVersion << '/' << runtimeVersion << '\n'
              << "Free/total memory MiB: " << freeBytes / (1024 * 1024)
              << '/' << totalBytes / (1024 * 1024) << '\n';

    constexpr int sampleCount = 1 << 20;
    constexpr int threadsPerBlock = 256;
    const std::size_t byteCount = static_cast<std::size_t>(sampleCount) * sizeof(float);
    std::vector<float> input(sampleCount);
    std::vector<float> output(sampleCount);
    for (int index = 0; index < sampleCount; ++index) {
        input[index] = static_cast<float>(index % 97) * 0.25f;
    }

    DeviceFloatBuffer deviceInput(sampleCount);
    DeviceFloatBuffer deviceOutput(sampleCount);
    CheckCuda(cudaMemcpy(deviceInput.Data(), input.data(), byteCount, cudaMemcpyHostToDevice),
              "cudaMemcpy H2D");
    ScaleBiasKernel<<<(sampleCount + threadsPerBlock - 1) / threadsPerBlock, threadsPerBlock>>>(
        deviceInput.Data(), deviceOutput.Data(), sampleCount);
    CheckCuda(cudaGetLastError(), "ScaleBiasKernel launch");
    CheckCuda(cudaDeviceSynchronize(), "ScaleBiasKernel execution");
    CheckCuda(cudaMemcpy(output.data(), deviceOutput.Data(), byteCount, cudaMemcpyDeviceToHost),
              "cudaMemcpy D2H");

    for (int index = 0; index < sampleCount; ++index) {
        const float expected = input[index] * 2.0f + 1.0f;
        if (output[index] != expected) {
            throw std::runtime_error("GPU result mismatch at index " + std::to_string(index));
        }
    }
    deviceOutput.Release();
    deviceInput.Release();
    std::cout << "[PASS] H2D, CUDA kernel, D2H and " << sampleCount
              << " verified results. This is not a VR renderer.\n";
}

}

int main()
{
    try {
        RunProbe();
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] CUDA diagnostic: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}