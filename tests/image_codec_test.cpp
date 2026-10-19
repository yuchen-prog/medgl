#include "medgl/image_codec.hpp"
#include "medgl/renderer.hpp"

#include <png.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<std::uint8_t> LegacyPng(const medgl::RgbImage& image) {
    png_image description{};
    description.version = PNG_IMAGE_VERSION;
    description.width = image.width;
    description.height = image.height;
    description.format = PNG_FORMAT_RGB;
    png_alloc_size_t bytes = 0;
    if (!png_image_write_to_memory(&description, nullptr, &bytes, 0, image.pixels.data(), 0, nullptr)) {
        png_image_free(&description);
        throw std::runtime_error("Legacy PNG sizing failed");
    }
    std::vector<std::uint8_t> encoded(bytes);
    if (!png_image_write_to_memory(&description, encoded.data(), &bytes, 0, image.pixels.data(), 0, nullptr)) {
        png_image_free(&description);
        throw std::runtime_error("Legacy PNG write failed");
    }
    png_image_free(&description);
    encoded.resize(bytes);
    return encoded;
}

medgl::RgbImage Decode(const std::vector<std::uint8_t>& encoded) {
    png_image description{};
    description.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&description, encoded.data(), encoded.size())) {
        png_image_free(&description);
        throw std::runtime_error("PNG read failed");
    }
    description.format = PNG_FORMAT_RGB;
    medgl::RgbImage result{description.width, description.height, {}};
    result.pixels.resize(result.RequiredBytes());
    if (!png_image_finish_read(&description, nullptr, result.pixels.data(), 0, nullptr)) {
        png_image_free(&description);
        throw std::runtime_error("PNG decode failed");
    }
    png_image_free(&description);
    result.Validate();
    return result;
}

medgl::RgbImage DecodeWic(const std::vector<std::uint8_t>& encoded) {
    using Microsoft::WRL::ComPtr;
    Require(encoded.size() <= (std::numeric_limits<DWORD>::max)(), "WIC input too large");
    ComPtr<IWICImagingFactory> factory;
    Require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(factory.GetAddressOf()))), "WIC factory failed");
    ComPtr<IWICStream> stream;
    Require(SUCCEEDED(factory->CreateStream(stream.GetAddressOf())), "WIC stream failed");
    auto input = encoded;
    Require(SUCCEEDED(stream->InitializeFromMemory(input.data(), static_cast<DWORD>(input.size()))),
        "WIC memory stream failed");
    ComPtr<IWICBitmapDecoder> decoder;
    Require(SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad,
        decoder.GetAddressOf())), "WIC decoder failed");
    ComPtr<IWICBitmapFrameDecode> frame;
    Require(SUCCEEDED(decoder->GetFrame(0, frame.GetAddressOf())), "WIC frame failed");
    ComPtr<IWICFormatConverter> converter;
    Require(SUCCEEDED(factory->CreateFormatConverter(converter.GetAddressOf())), "WIC converter failed");
    Require(SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat24bppRGB,
        WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)), "WIC RGB conversion failed");
    medgl::RgbImage result;
    Require(SUCCEEDED(converter->GetSize(&result.width, &result.height)), "WIC dimensions failed");
    result.pixels.resize(result.RequiredBytes());
    Require(result.pixels.size() <= (std::numeric_limits<UINT>::max)(), "WIC output too large");
    Require(SUCCEEDED(converter->CopyPixels(nullptr, static_cast<UINT>(result.StrideBytes()),
        static_cast<UINT>(result.pixels.size()), result.pixels.data())), "WIC RGB read failed");
    return result;
}

std::vector<std::uint8_t> Metadata(const std::vector<std::uint8_t>& encoded) {
    std::vector<std::uint8_t> result;
    for (std::size_t offset = 8; offset < encoded.size();) {
        Require(encoded.size() - offset >= 12, "Truncated PNG chunk");
        const auto length = static_cast<std::size_t>(png_get_uint_32(encoded.data() + offset));
        Require(length <= encoded.size() - offset - 12, "Invalid PNG chunk length");
        const auto type = encoded.data() + offset + 4;
        if (!(type[0] == 'I' && type[1] == 'D' && type[2] == 'A' && type[3] == 'T'))
            result.insert(result.end(), encoded.begin() + offset, encoded.begin() + offset + length + 12);
        offset += length + 12;
    }
    return result;
}

void Check(const medgl::RgbImage& image) {
    const auto original = image.pixels;
    const auto legacy = LegacyPng(image);
    const auto encoded = medgl::EncodePng(image);
    const auto decodedLegacy = Decode(legacy);
    const auto decoded = Decode(encoded);
    const auto decodedWic = DecodeWic(encoded);
    Require(decoded.width == image.width && decoded.height == image.height, "PNG dimensions changed");
    Require(decodedLegacy.pixels == original && decoded.pixels == original, "PNG RGB bytes changed");
    Require(decodedWic.width == image.width && decodedWic.height == image.height
        && decodedWic.pixels == original, "Independent WIC RGB bytes changed");
    Require(encoded == legacy, "Default PNG bitstream or color metadata changed");
    Require(medgl::EncodePng(image) == encoded, "Repeated PNG encoding changed");
    const auto fast = medgl::EncodePng(image, medgl::PngCompression::Fast);
    const auto fastDecoded = Decode(fast);
    const auto fastWic = DecodeWic(fast);
    Require(fastDecoded.width == image.width && fastDecoded.height == image.height
        && fastDecoded.pixels == original, "Fast PNG libpng RGB changed");
    Require(fastWic.width == image.width && fastWic.height == image.height
        && fastWic.pixels == original, "Fast PNG WIC RGB changed");
    Require(Metadata(fast) == Metadata(encoded), "Fast PNG color metadata changed");
    Require(medgl::EncodePng(image, medgl::PngCompression::Fast) == fast, "Repeated fast PNG changed");
    Require(image.pixels == original, "PNG encoder modified the input");
}

void Patterns() {
    for (const auto dimensions : {std::pair<std::uint32_t, std::uint32_t>{1, 1}, {1, 37}, {39, 1},
                                 {65, 33}, {257, 259}, {512, 512}, {2048, 2048}}) {
        medgl::RgbImage image{dimensions.first, dimensions.second, {}};
        image.pixels.resize(image.RequiredBytes());
        Check(image);
        std::fill(image.pixels.begin(), image.pixels.end(), 255);
        Check(image);
        for (std::size_t index = 0; index < image.pixels.size(); ++index)
            image.pixels[index] = static_cast<std::uint8_t>((index / 3 + index % 3 * 73) % 256);
        Check(image);
        std::uint32_t state = 0x73ab219du;
        for (auto& value : image.pixels) {
            state = state * 1664525u + 1013904223u;
            value = static_cast<std::uint8_t>(state >> 24);
        }
        Check(image);
    }
    for (const medgl::RgbImage image : {medgl::RgbImage{0, 1, {}}, {1, 0, {}},
                                      {2, 2, {1, 2, 3}}, {0xffffffffu, 0xffffffffu, {}}}) {
        for (const auto mode : {medgl::PngCompression::Default, medgl::PngCompression::Fast}) {
            bool rejected = false;
            try { medgl::EncodePng(image, mode); } catch (const std::exception&) { rejected = true; }
            Require(rejected, "Invalid PNG input accepted");
        }
    }
    bool rejected = false;
    try { medgl::EncodePng({1, 1, {0, 0, 0}}, static_cast<medgl::PngCompression>(99)); }
    catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected, "Invalid PNG mode accepted");
}

void RenderedFrames() {
    const auto volume = medgl::MakePhantom();
    medgl::VolumeRenderer renderer;
    renderer.Load(volume);
    auto settings = medgl::FitView(volume);
    settings.width = 257;
    settings.height = 193;
    settings.lighting = true;
    for (const bool transparent : {false, true}) {
        settings.opacityScale = transparent ? 0.f : 1.f;
        settings.background = transparent ? std::array<float, 3>{0.1f, 0.4f, 0.8f}
                                          : std::array<float, 3>{0, 0, 0};
        Check(renderer.Render(settings));
    }
}

}

int main() {
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) {
        std::cerr << "WIC COM initialization failed.\n";
        return 1;
    }
    try {
        Patterns();
        RenderedFrames();
        std::cout << "PNG single-write libpng/WIC RGB roundtrip, legacy bitstream, input validation and CUDA frames passed.\n";
        CoUninitialize();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PNG regression failed: " << error.what() << '\n';
        CoUninitialize();
        return 1;
    }
}