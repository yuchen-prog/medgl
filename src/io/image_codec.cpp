#include "medgl/image_codec.hpp"

#include <png.h>
#include <turbojpeg.h>

#include <limits>
#include <memory>
#include <stdexcept>

namespace medgl {

std::vector<std::uint8_t> EncodePng(const RgbImage& image)
{
    return EncodePng(image, PngCompression::Default);
}

std::vector<std::uint8_t> EncodePng(const RgbImage& image, PngCompression compression)
{
    image.Validate();
    if (compression != PngCompression::Default && compression != PngCompression::Fast) {
        throw std::invalid_argument("Invalid PNG compression mode");
    }
    const auto rawBytes = image.RequiredBytes();
    if (rawBytes > PNG_UINT_31_MAX || image.height > PNG_UINT_31_MAX - rawBytes) {
        throw std::invalid_argument("PNG image exceeds the supported buffer size");
    }
    png_image description{};
    description.version = PNG_IMAGE_VERSION;
    description.width = image.width;
    description.height = image.height;
    description.format = PNG_FORMAT_RGB;
    description.flags = compression == PngCompression::Fast ? PNG_IMAGE_FLAG_FAST : 0;
    png_alloc_size_t bytes = PNG_IMAGE_PNG_SIZE_MAX(description);
    if (bytes < rawBytes || bytes > std::vector<std::uint8_t>().max_size()) {
        throw std::overflow_error("PNG output capacity exceeds the supported buffer size");
    }
    std::vector<std::uint8_t> encoded(bytes);
    if (!png_image_write_to_memory(&description, encoded.data(), &bytes, 0, image.pixels.data(), 0, nullptr)) {
        png_image_free(&description);
        throw std::runtime_error("PNG encoding failed");
    }
    png_image_free(&description);
    encoded.resize(bytes);
    return encoded;
}

std::vector<std::uint8_t> EncodeJpeg(const RgbImage& image, int quality)
{
    image.Validate();
    if (quality < 1 || quality > 100 || image.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max())
        || image.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())
        || image.StrideBytes() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::invalid_argument("Invalid JPEG dimensions or quality");
    const auto compressor = tjInitCompress();
    if (!compressor) throw std::runtime_error("JPEG compressor initialization failed");
    const auto cleanup = [](void* handle) { tjDestroy(handle); };
    std::unique_ptr<void, decltype(cleanup)> owner(compressor, cleanup);
    unsigned char* output = nullptr;
    unsigned long length = 0;
    const int result = tjCompress2(compressor, image.pixels.data(), static_cast<int>(image.width),
        static_cast<int>(image.StrideBytes()), static_cast<int>(image.height), TJPF_RGB,
        &output, &length, TJSAMP_444, quality, TJFLAG_FASTDCT);
    const auto release = [](unsigned char* buffer) { tjFree(buffer); };
    std::unique_ptr<unsigned char, decltype(release)> buffer(output, release);
    if (result != 0) throw std::runtime_error("JPEG encoding failed");
    return {output, output + length};
}

}