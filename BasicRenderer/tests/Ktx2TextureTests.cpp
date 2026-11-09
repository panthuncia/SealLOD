// The KTX2 mip provider, against files libktx (KTX-Software) writes: block-compressed with zstd supercompression, an
// uncompressed array, and a Basis Universal (UASTC) texture it must transcode. Set BR_KTX2_SAMPLE to a real .ktx2 to
// also read one whole.

#include <BasicRenderer/Assets/TexturePipeline.h>

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <ktx.h>

#include <rhi.h>

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& what) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what.c_str());
        ++g_failures;
    }
}

struct Fixture {
    std::shared_ptr<const std::vector<std::uint8_t>> file;
    // The bytes put in, [item][level].
    std::vector<std::vector<std::vector<std::uint8_t>>> images;
};

std::vector<std::uint8_t> RandomBytes(std::size_t count, std::mt19937& random) {
    std::vector<std::uint8_t> bytes(count);
    for (auto& byte : bytes) byte = static_cast<std::uint8_t>(random());
    return bytes;
}

// Writes a 2D KTX2 with libktx. blockBytes/blockSize describe the format's storage (BC7: 16 bytes per 4x4 block).
Fixture WriteKtx2(std::uint32_t vkFormat, std::uint32_t width, std::uint32_t height, std::uint32_t levels, std::uint32_t layers,
    std::uint32_t blockSize, std::uint32_t blockBytes, bool zstd) {
    ktxTextureCreateInfo info{};
    info.vkFormat = vkFormat;
    info.baseWidth = width;
    info.baseHeight = height;
    info.baseDepth = 1;
    info.numDimensions = 2;
    info.numLevels = levels;
    info.numLayers = layers;
    info.numFaces = 1;
    info.isArray = layers > 1 ? KTX_TRUE : KTX_FALSE;
    info.generateMipmaps = KTX_FALSE;
    ktxTexture2* texture = nullptr;
    if (ktxTexture2_Create(&info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &texture) != KTX_SUCCESS) {
        Check(false, "ktxTexture2_Create");
        return {};
    }
    std::mt19937 random(vkFormat * 31u + width);
    Fixture fixture;
    fixture.images.resize(layers);
    for (std::uint32_t layer = 0; layer < layers; ++layer) {
        for (std::uint32_t level = 0; level < levels; ++level) {
            const std::uint32_t w = (std::max)(1u, width >> level);
            const std::uint32_t h = (std::max)(1u, height >> level);
            const std::size_t size = static_cast<std::size_t>((w + blockSize - 1) / blockSize) * ((h + blockSize - 1) / blockSize) * blockBytes;
            auto bytes = RandomBytes(size, random);
            Check(ktxTexture_SetImageFromMemory(ktxTexture(texture), level, layer, 0, bytes.data(), bytes.size()) == KTX_SUCCESS,
                "ktxTexture_SetImageFromMemory");
            fixture.images[layer].push_back(std::move(bytes));
        }
    }
    if (zstd) Check(ktxTexture2_DeflateZstd(texture, 5) == KTX_SUCCESS, "ktxTexture2_DeflateZstd");
    ktx_uint8_t* written = nullptr;
    ktx_size_t writtenSize = 0;
    Check(ktxTexture_WriteToMemory(ktxTexture(texture), &written, &writtenSize) == KTX_SUCCESS, "ktxTexture_WriteToMemory");
    fixture.file = std::make_shared<const std::vector<std::uint8_t>>(written, written + writtenSize);
    std::free(written);
    ktxTexture_Destroy(ktxTexture(texture));
    return fixture;
}

void CheckBytes(const TextureSourceData& data, const Fixture& fixture, std::uint32_t levels, std::uint32_t topMip, const std::string& name) {
    for (std::size_t item = 0; item < fixture.images.size(); ++item) {
        for (std::uint32_t level = 0; level < levels; ++level) {
            const auto& got = data.subresources[item * levels + level];
            const std::string where = name + " item " + std::to_string(item) + " level " + std::to_string(level);
            if (level < topMip && fixture.images.size() == 1) continue;
            Check(got != nullptr, where + " is present");
            if (got) Check(*got == fixture.images[item][level], where + " matches what libktx was given");
        }
    }
}

void TestBc7Zstd() {
    constexpr std::uint32_t kBc7Srgb = 146;
    // 64x32: 7 levels, a full chain, the smallest levels one block each.
    const Fixture fixture = WriteKtx2(kBc7Srgb, 64, 32, 7, 1, 4, 16, true);
    if (!fixture.file) return;
    Check(br::textures::LooksLikeKtx2(*fixture.file), "BC7: identifier recognised");
    auto provider = br::textures::MakeKtx2TextureMipProvider(br::textures::MakeMemoryTextureByteSource(fixture.file), "bc7");
    const auto shape = provider->DescribeTexture();
    Check(shape.has_value(), "BC7: described");
    if (!shape) return;
    Check(shape->desc.format == rhi::Format::BC7_UNorm_sRGB, "BC7: format is BC7 sRGB");
    Check(shape->isBlockCompressed, "BC7: block compressed");
    Check(shape->hasFullMipChain, "BC7: full chain");
    Check(shape->desc.imageDimensions.size() == 7, "BC7: 7 subresources");
    Check(shape->desc.imageDimensions[0].width == 64 && shape->desc.imageDimensions[0].height == 32, "BC7: top level 64x32");
    Check(shape->desc.imageDimensions[0].slicePitch == 16u * 8u * 16u, "BC7: top level pitch");

    const auto whole = provider->ReadTextureMips(0);
    Check(whole != nullptr, "BC7: whole read");
    if (whole) CheckBytes(*whole, fixture, 7, 0, "BC7 whole");

    const auto window = provider->ReadTextureMips(3);
    Check(window != nullptr, "BC7: window read");
    if (window) {
        Check(!window->subresources[0] && !window->subresources[2], "BC7: window leaves out levels above it");
        CheckBytes(*window, fixture, 7, 3, "BC7 window");
    }
}

void TestRgba8Array() {
    constexpr std::uint32_t kRgba8 = 37;
    const Fixture fixture = WriteKtx2(kRgba8, 8, 8, 4, 3, 1, 4, false);
    if (!fixture.file) return;
    auto provider = br::textures::MakeKtx2TextureMipProvider(br::textures::MakeMemoryTextureByteSource(fixture.file), "rgba8-array");
    const auto shape = provider->DescribeTexture();
    Check(shape.has_value(), "RGBA8 array: described");
    if (!shape) return;
    Check(shape->desc.format == rhi::Format::R8G8B8A8_UNorm, "RGBA8 array: format");
    Check(!shape->isBlockCompressed, "RGBA8 array: not block compressed");
    Check(shape->desc.isArray && shape->desc.arraySize == 3, "RGBA8 array: 3 layers");
    Check(shape->desc.imageDimensions.size() == 12, "RGBA8 array: 12 subresources");
    // An array always loads whole, whatever top mip is asked for.
    const auto data = provider->ReadTextureMips(2);
    Check(data != nullptr, "RGBA8 array: read");
    if (data) CheckBytes(*data, fixture, 4, 0, "RGBA8 array");
}

void TestUastcTranscode() {
    constexpr std::uint32_t kRgba8Srgb = 43;
    Fixture fixture = WriteKtx2(kRgba8Srgb, 32, 32, 1, 1, 1, 4, false);
    if (!fixture.file) return;
    // Re-encode the RGBA8 fixture as UASTC with libktx's own encoder.
    ktxTexture2* texture = nullptr;
    if (ktxTexture2_CreateFromMemory(fixture.file->data(), fixture.file->size(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &texture) != KTX_SUCCESS) {
        Check(false, "UASTC: reload fixture");
        return;
    }
    ktxBasisParams params{};
    params.structSize = sizeof(params);
    params.uastc = KTX_TRUE;
    params.threadCount = 1;
    const bool encoded = ktxTexture2_CompressBasisEx(texture, &params) == KTX_SUCCESS;
    Check(encoded, "UASTC: libktx encodes");
    ktx_uint8_t* written = nullptr;
    ktx_size_t writtenSize = 0;
    if (encoded) Check(ktxTexture_WriteToMemory(ktxTexture(texture), &written, &writtenSize) == KTX_SUCCESS, "UASTC: write");
    ktxTexture_Destroy(ktxTexture(texture));
    if (!written) return;
    auto file = std::make_shared<const std::vector<std::uint8_t>>(written, written + writtenSize);
    std::free(written);

    auto provider = br::textures::MakeKtx2TextureMipProvider(br::textures::MakeMemoryTextureByteSource(file), "uastc");
    const auto shape = provider->DescribeTexture();
    Check(shape.has_value(), "UASTC: described");
    if (!shape) return;
    Check(shape->desc.format == rhi::Format::BC7_UNorm_sRGB, "UASTC: transcoded to BC7 sRGB");
    Check(shape->isBlockCompressed, "UASTC: block compressed after transcode");
    const auto data = provider->ReadTextureMips(0);
    Check(data && data->subresources.size() == 1 && data->subresources[0] && data->subresources[0]->size() == 8u * 8u * 16u,
        "UASTC: one 32x32 BC7 level");
}

void TestNotKtx2() {
    const std::vector<std::uint8_t> png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 0, 0, 0, 13 };
    Check(!br::textures::LooksLikeKtx2(png), "PNG is not KTX2");
    Check(!br::textures::LooksLikeKtx2(std::vector<std::uint8_t>{ 0xAB, 'K', 'T', 'X' }), "short input is not KTX2");
    auto provider = br::textures::MakeKtx2TextureMipProvider(
        br::textures::MakeMemoryTextureByteSource(std::make_shared<const std::vector<std::uint8_t>>(png)), "png");
    Check(!provider->DescribeTexture().has_value(), "PNG does not describe as KTX2");
}

void TestSample(const char* path) {
    if (!br::textures::FileLooksLikeKtx2(path)) {
        Check(false, std::string("sample is not KTX2: ") + path);
        return;
    }
    auto provider = br::textures::MakeKtx2TextureMipProvider(br::textures::MakeFileTextureByteSource(path), path);
    const auto shape = provider->DescribeTexture();
    Check(shape.has_value(), "sample: described");
    if (!shape) return;
    const auto data = provider->ReadTextureMips(0);
    std::uint64_t bytes = 0;
    for (const auto& subresource : data->subresources) bytes += subresource ? subresource->size() : 0;
    std::printf("sample %s: %ux%u format=%u subresources=%zu fullChain=%d bc=%d bytes=%llu\n", path,
        shape->desc.imageDimensions[0].width, shape->desc.imageDimensions[0].height, static_cast<unsigned>(shape->desc.format),
        shape->desc.imageDimensions.size(), shape->hasFullMipChain, shape->isBlockCompressed, static_cast<unsigned long long>(bytes));
}

} // namespace

int main() {
    TestBc7Zstd();
    TestRgba8Array();
    TestUastcTranscode();
    TestNotKtx2();
    if (const char* sample = std::getenv("BR_KTX2_SAMPLE")) TestSample(sample);
    if (g_failures == 0) std::printf("Ktx2TextureTests: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
