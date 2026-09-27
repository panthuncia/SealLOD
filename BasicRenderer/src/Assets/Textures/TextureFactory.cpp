#include "Assets/Textures/TextureFactory.h"
#include "Assets/Textures/RenderPasses/TextureProcessingPasses.h"

#include <algorithm>
#include <stdexcept>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

#include <OpenRenderGraph/OpenRenderGraph.h>

#include "Assets/Textures/Processing/TextureProcessingManager.h"
#include "Resources/PixelBuffer.h"
#include "Resources/Sampler.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/ReadbackRequest.h"
#include "Render/Runtime/IReadbackService.h"
#include "Render/Runtime/IUploadService.h"
#include "ThirdParty/stb/stb_image.h"
#include "rhi_helpers.h"
#include "BasicRenderer/Extensions/Buffers/LazyDynamicStructuredBuffer.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Runtime/Device/DeviceManager.h"
#include "Materials/TextureStreaming/MaterialTextureTransferService.h"
#include "Utilities/Utilities.h"

#define A_CPU
#include "../shaders/FidelityFX/ffx_a.h"
#include "../shaders/FidelityFX/ffx_spd.h"


TextureFactory::TextureFactory(std::shared_ptr<org::runtime::IUploadService> uploadService)
        : m_uploadService(std::move(uploadService)) {
		m_mipmappingPass = std::make_shared<MipmappingPass>();
		m_bc7CompressionPass = std::make_shared<BC7CompressionPass>();
		m_bc7CompressionCopyPass = std::make_shared<BC7CompressionCopyPass>();
		m_bc7CompressionReadbackPass = std::make_shared<BC7CompressionReadbackPass>();
    }

namespace {
    void UploadTextureData(
        org::runtime::IUploadService& uploadService,
        const std::shared_ptr<org::Resource>& dstTexture,
        const org::TextureDescription& desc,
        const std::vector<std::shared_ptr<std::vector<uint8_t>>>& initialData,
        unsigned int mipLevels)
    {
        if (initialData.empty()) return;

        const uint32_t faces = desc.isCubemap ? 6u : 1u;
        const uint32_t arraySlices = faces * static_cast<uint32_t>(desc.arraySize);
        const uint32_t numSubres = arraySlices * static_cast<uint32_t>(mipLevels);

#if BUILD_TYPE == BUILD_TYPE_DEBUG
        if (desc.imageDimensions.size() < numSubres) {
            throw std::runtime_error("UploadTextureData: desc.imageDimensions is smaller than expected subresource count.");
        }
#endif

        // The upload is posted to the render thread's upload instance, so the
        // source bytes (caller vectors and any RGB->RGBA expansion) must stay
        // alive until the owner records the copy: the payload owns them.
        struct TextureUploadPayload {
            std::vector<rhi::helpers::SubresourceData> srd;
            std::vector<std::vector<stbi_uc>> expandedImages;
            std::vector<std::shared_ptr<std::vector<uint8_t>>> fullInitial;
        };
        auto payload = std::make_shared<TextureUploadPayload>();
        // Dense SubresourceData table (nullptr entries allowed; skipped by uploader)
        auto& srd = payload->srd;
        srd.resize(numSubres);
        auto& expandedImages = payload->expandedImages;
        expandedImages.reserve(numSubres);
        // Pad/trim caller-provided data to full subresource count.
        auto& fullInitial = payload->fullInitial;
        fullInitial.assign(numSubres, nullptr);
        {
            const size_t toCopy = std::min<size_t>(initialData.size(), static_cast<size_t>(numSubres));
            std::copy_n(initialData.begin(), toCopy, fullInitial.begin());
        }

        const uint32_t baseW = static_cast<uint32_t>(desc.imageDimensions[0].width);
        const uint32_t baseH = static_cast<uint32_t>(desc.imageDimensions[0].height);

        auto RawMatchesStoredPitches = [](uint32_t w, uint32_t h, uint32_t ch,
            size_t storedRowPitch, size_t storedSlicePitch) -> bool
            {
                const size_t rawRow = static_cast<size_t>(w) * static_cast<size_t>(ch);
                const size_t rawSlice = rawRow * static_cast<size_t>(h);
                return (rawRow == storedRowPitch) && (rawSlice == storedSlicePitch);
            };

        for (uint32_t a = 0; a < arraySlices; ++a) {
            for (uint32_t m = 0; m < static_cast<uint32_t>(mipLevels); ++m) {
                const uint32_t subIdx = m + a * static_cast<uint32_t>(mipLevels);

                const auto& dims = desc.imageDimensions[subIdx];
                const size_t storedRowPitch = static_cast<size_t>(dims.rowPitch);
                const size_t storedSlicePitch = static_cast<size_t>(dims.slicePitch);

                const auto& buf = fullInitial[subIdx];
                const stbi_uc* imageData = buf ? reinterpret_cast<const stbi_uc*>(buf->data()) : nullptr;
                const size_t imageBytes = buf ? buf->size() : 0;

                auto& out = srd[subIdx];

                if (!imageData) {
                    out.pData = nullptr;
                    out.rowPitch = out.slicePitch = 0;
                    continue;
                }

#if BUILD_TYPE == BUILD_TYPE_DEBUG
                if (imageBytes < storedSlicePitch) {
                    throw std::runtime_error("UploadTextureData: subresource buffer smaller than expected slicePitch.");
                }
#endif

                const bool blockCompressed = rhi::helpers::IsBlockCompressed(desc.format);

                if (blockCompressed) {
                    out.pData = imageData;
                    out.rowPitch = static_cast<uint32_t>(storedRowPitch);
                    out.slicePitch = static_cast<uint32_t>(storedSlicePitch);
                    continue;
                }

                uint32_t channels = desc.channels;

                // Pick a width/height that makes "raw tightly packed" match the stored pitches, if possible.
                // This keeps behavior compatible whether imageDimensions stores per-mip sizes or base sizes.
                uint32_t w = static_cast<uint32_t>(dims.width);
                uint32_t h = static_cast<uint32_t>(dims.height);

                bool rawPacked =
                    RawMatchesStoredPitches(w, h, channels, storedRowPitch, storedSlicePitch);

                if (!rawPacked) {
                    const uint32_t w2 = std::max(1u, w >> m);
                    const uint32_t h2 = std::max(1u, h >> m);
                    rawPacked = RawMatchesStoredPitches(w2, h2, channels, storedRowPitch, storedSlicePitch);
                    if (rawPacked) { w = w2; h = h2; }
                }

                if (!rawPacked) {
                    const uint32_t w3 = std::max(1u, baseW >> m);
                    const uint32_t h3 = std::max(1u, baseH >> m);
                    rawPacked = RawMatchesStoredPitches(w3, h3, channels, storedRowPitch, storedSlicePitch);
                    if (rawPacked) { w = w3; h = h3; }
                }

                if (!rawPacked) {
                    // Pre-padded / compressed / otherwise non-raw layout: pass pitches through unchanged.
                    out.pData = imageData;
                    out.rowPitch = static_cast<uint32_t>(storedRowPitch);
                    out.slicePitch = static_cast<uint32_t>(storedSlicePitch);
                    continue;
                }

                // Tightly packed: optionally expand RGB -> RGBA for upload convenience.
                const stbi_uc* ptr = imageData;
                if (channels == 3) {
                    expandedImages.emplace_back(ExpandImageData(imageData, w, h)); // returns RGBA8
                    ptr = expandedImages.back().data();
                    channels = 4;
                }

                out.pData = ptr;
                out.rowPitch = w * channels;
                out.slicePitch = out.rowPitch * h;
            }
        }

        std::shared_ptr<const std::vector<rhi::helpers::SubresourceData>> subresources(payload, &payload->srd);
#if BUILD_TYPE == BUILD_TYPE_DEBUG
        uploadService.PostTextureSubresources(
            org::runtime::UploadTarget::FromShared(dstTexture),
            desc.format,
            baseW,
            baseH,
            /*depthOrLayers*/ 1,
            static_cast<uint32_t>(mipLevels),
            arraySlices,
            std::move(subresources), payload, __FILE__, __LINE__);
#else
        uploadService.PostTextureSubresources(
            org::runtime::UploadTarget::FromShared(dstTexture), desc.format, baseW, baseH,
            1, static_cast<uint32_t>(mipLevels), arraySlices, std::move(subresources), payload);
#endif
    }
}

namespace {

    namespace detail
    {
        float Unorm8ToFloat(uint8_t v) noexcept
        {
            return float(v) * (1.0f / 255.0f);
        }

        uint8_t FloatToUnorm8(float x) noexcept
        {
            x = std::clamp(x, 0.0f, 1.0f);
            // round-to-nearest
            const int v = int(x * 255.0f + 0.5f);
            return static_cast<uint8_t>(std::clamp(v, 0, 255));
        }

        // sRGB <-> Linear conversion for scalar in [0,1].
        float SrgbToLinear(float c) noexcept
        {
            if (c <= 0.04045f) return c / 12.92f;
            return std::pow((c + 0.055f) / 1.055f, 2.4f);
        }

        float LinearToSrgb(float c) noexcept
        {
            c = std::clamp(c, 0.0f, 1.0f);
            if (c <= 0.0031308f) return 12.92f * c;
            return 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
        }

        // LUT for 8-bit sRGB -> linear float to avoid pow() on decode.
        const std::array<float, 256>& Srgb8ToLinearLUT()
        {
            static const std::array<float, 256> lut = [] {
                std::array<float, 256> t{};
                for (int i = 0; i < 256; ++i) {
                    const float s = float(i) * (1.0f / 255.0f);
                    t[size_t(i)] = SrgbToLinear(s);
                }
                return t;
                }();
            return lut;
        }

        float Srgb8ToLinear(uint8_t v) noexcept
        {
            return Srgb8ToLinearLUT()[size_t(v)];
        }

        uint8_t LinearToSrgb8(float linear) noexcept
        {
            return FloatToUnorm8(LinearToSrgb(linear));
        }
    }

    std::vector<std::shared_ptr<std::vector<uint8_t>>> BuildMipChain2D(
        const std::shared_ptr<std::vector<uint8_t>>& base,
        uint32_t baseW,
        uint32_t baseH,
        uint32_t channels,
        uint32_t mipLevels,
        bool isSRGB,
        bool premultiplyAlpha = true)
    {
        if (!base) throw std::runtime_error("BuildMipChain2D: null base data.");
        if (channels == 0 || channels > 4) {
            throw std::runtime_error("BuildMipChain2D: unsupported channel count (expected 1..4).");
        }
        if (mipLevels == 0) {
            throw std::runtime_error("BuildMipChain2D: mipLevels must be > 0.");
        }

        const size_t expectedBaseBytes = static_cast<size_t>(baseW) * baseH * channels;
        if (base->size() < expectedBaseBytes) {
            throw std::runtime_error("BuildMipChain2D: base data buffer is smaller than width*height*channels.");
        }

        // If premultiplyAlpha is requested, require an alpha channel
        if (premultiplyAlpha && channels < 2) {
            premultiplyAlpha = false;
        }

        std::vector<std::shared_ptr<std::vector<uint8_t>>> chain;
        chain.reserve(mipLevels);
        chain.push_back(base);

        uint32_t prevW = baseW;
        uint32_t prevH = baseH;

        auto decodeColor = [&](uint8_t v) noexcept -> float {
            // Interpret color channels as sRGB if isSRGB, otherwise treat as linear UNORM.
            return isSRGB ? detail::Srgb8ToLinear(v) : detail::Unorm8ToFloat(v);
            };
        auto encodeColor = [&](float linear) noexcept -> uint8_t {
            // Store back as sRGB if isSRGB, otherwise store as linear UNORM.
            return isSRGB ? detail::LinearToSrgb8(linear) : detail::FloatToUnorm8(linear);
            };

        for (uint32_t level = 1; level < mipLevels; ++level) {
            const uint32_t w = std::max(1u, prevW >> 1);
            const uint32_t h = std::max(1u, prevH >> 1);

            auto dst = std::make_shared<std::vector<uint8_t>>();
            dst->resize(static_cast<size_t>(w) * h * channels);

            const uint8_t* src = chain.back()->data();

            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    const uint32_t sx0 = std::min(prevW - 1, x * 2 + 0);
                    const uint32_t sx1 = std::min(prevW - 1, x * 2 + 1);
                    const uint32_t sy0 = std::min(prevH - 1, y * 2 + 0);
                    const uint32_t sy1 = std::min(prevH - 1, y * 2 + 1);

                    const uint32_t idx00 = (sy0 * prevW + sx0) * channels;
                    const uint32_t idx10 = (sy0 * prevW + sx1) * channels;
                    const uint32_t idx01 = (sy1 * prevW + sx0) * channels;
                    const uint32_t idx11 = (sy1 * prevW + sx1) * channels;

                    const uint32_t dstIdx = (y * w + x) * channels;

                    // Accumulate in linear floats.
                    // Supports:
                    //  - channels==1: treat channel 0 as "color" (sRGB if isSRGB)
                    //  - channels==2: treat channel 0 as "color", channel 1 as alpha (linear)
                    //  - channels==3: RGB color
                    //  - channels==4: RGBA color (alpha linear)
                    float acc[4] = { 0, 0, 0, 0 };

                    auto accumulateSample = [&](uint32_t srcIdx)
                        {
                            float a = 1.0f;
                            if (channels == 2) {
                                a = detail::Unorm8ToFloat(src[srcIdx + 1]);
                            }
                            else if (channels == 4) {
                                a = detail::Unorm8ToFloat(src[srcIdx + 3]);
                            }

                            // Decode color channels to linear
                            float c0 = (channels >= 1) ? decodeColor(src[srcIdx + 0]) : 0.0f;
                            float c1 = (channels >= 3) ? decodeColor(src[srcIdx + 1]) : 0.0f;
                            float c2 = (channels >= 3) ? decodeColor(src[srcIdx + 2]) : 0.0f;

                            if (premultiplyAlpha && (channels == 2 || channels == 4)) {
                                c0 *= a;
                                c1 *= a;
                                c2 *= a;
                            }

                            // Accumulate
                            acc[0] += c0;
                            if (channels >= 3) {
                                acc[1] += c1;
                                acc[2] += c2;
                            }
                            if (channels == 2) {
                                acc[1] += a;
                            }
                            else if (channels == 4) {
                                acc[3] += a;
                            }
                        };

                    accumulateSample(idx00);
                    accumulateSample(idx10);
                    accumulateSample(idx01);
                    accumulateSample(idx11);

                    const float inv4 = 0.25f;
                    // Average
                    acc[0] *= inv4;
                    acc[1] *= inv4;
                    acc[2] *= inv4;
                    acc[3] *= inv4;

                    // Unpremultiply if needed
                    if (premultiplyAlpha && channels == 4) {
                        const float a = acc[3];
                        if (a > 0.0f) {
                            acc[0] /= a;
                            acc[1] /= a;
                            acc[2] /= a;
                        }
                    }
                    if (premultiplyAlpha && channels == 2) {
                        const float a = acc[1];
                        if (a > 0.0f) {
                            acc[0] /= a;
                        }
                    }

                    // Write out
                    if (channels == 1) {
                        (*dst)[dstIdx + 0] = encodeColor(acc[0]);
                    }
                    else if (channels == 2) {
                        (*dst)[dstIdx + 0] = encodeColor(acc[0]);               // luma/color
                        (*dst)[dstIdx + 1] = detail::FloatToUnorm8(acc[1]);     // alpha
                    }
                    else if (channels == 3) {
                        (*dst)[dstIdx + 0] = encodeColor(acc[0]);
                        (*dst)[dstIdx + 1] = encodeColor(acc[1]);
                        (*dst)[dstIdx + 2] = encodeColor(acc[2]);
                    }
                    else { // channels == 4
                        (*dst)[dstIdx + 0] = encodeColor(acc[0]);
                        (*dst)[dstIdx + 1] = encodeColor(acc[1]);
                        (*dst)[dstIdx + 2] = encodeColor(acc[2]);
                        (*dst)[dstIdx + 3] = detail::FloatToUnorm8(acc[3]);     // alpha stays linear
                    }
                }
            }

            chain.push_back(std::move(dst));
            prevW = w;
            prevH = h;
        }

        return chain;
    }

    uint32_t CalcMipCount(uint32_t w, uint32_t h) noexcept {
        uint32_t levels = 1;
        while (w > 1 || h > 1) {
            w = std::max(1u, w >> 1);
            h = std::max(1u, h >> 1);
            ++levels;
        }
        return levels;
    }

    uint32_t GetTextureMipLevelCount(const org::TextureDescription& desc) noexcept
    {
        if (desc.imageDimensions.empty()) {
            return 1u;
        }

        const uint32_t faces = desc.isCubemap ? 6u : 1u;
        const uint32_t slices = faces * (std::max)(1u, desc.arraySize);
        if (slices > 0u && desc.imageDimensions.size() > slices && (desc.imageDimensions.size() % slices) == 0u) {
            return static_cast<uint32_t>(desc.imageDimensions.size() / slices);
        }

        return 1u;
    }

    void ExpandDescriptionToFullMipChain(org::TextureDescription& desc)
    {
        if (desc.imageDimensions.empty()) {
            return;
        }

        const uint32_t baseW = desc.imageDimensions[0].width;
        const uint32_t baseH = desc.imageDimensions[0].height;
        const uint32_t faces = desc.isCubemap ? 6u : 1u;
        const uint32_t slices = faces * (std::max)(1u, desc.arraySize);
        const uint32_t mipLevels = CalcMipCount(baseW, baseH);
        desc.imageDimensions.resize(static_cast<size_t>(slices) * mipLevels);
        const bool blockCompressed = rhi::helpers::IsBlockCompressed(desc.format);

        for (uint32_t s = 0; s < slices; ++s) {
            for (uint32_t m = 0; m < mipLevels; ++m) {
                const uint32_t w = (std::max)(1u, baseW >> m);
                const uint32_t h = (std::max)(1u, baseH >> m);
                const uint32_t idx = m + s * mipLevels;

                desc.imageDimensions[idx].width = w;
                desc.imageDimensions[idx].height = h;
                if (blockCompressed) {
                    const uint32_t blocksWide = (w + 3u) / 4u;
                    const uint32_t blocksHigh = (h + 3u) / 4u;
                    desc.imageDimensions[idx].rowPitch = static_cast<uint64_t>(blocksWide) * 16u;
                    desc.imageDimensions[idx].slicePitch = desc.imageDimensions[idx].rowPitch * blocksHigh;
                }
                else {
                    desc.imageDimensions[idx].rowPitch = uint64_t(w) * desc.channels;
                    desc.imageDimensions[idx].slicePitch = desc.imageDimensions[idx].rowPitch * h;
                }
            }
        }
    }

    uint32_t CalcAlphaCoverageExportMipCount(uint32_t baseW, uint32_t baseH) noexcept
    {
        constexpr uint32_t kMinRepresentableAlphaMaskTexels = 16u;

        uint32_t levels = 1u;
        uint32_t w = baseW;
        uint32_t h = baseH;
        while (w > 1u || h > 1u) {
            const uint32_t nextW = (std::max)(1u, w >> 1u);
            const uint32_t nextH = (std::max)(1u, h >> 1u);
            if (nextW * nextH < kMinRepresentableAlphaMaskTexels) {
                break;
            }

            w = nextW;
            h = nextH;
            ++levels;
        }

        return levels;
    }

    void ResizeDescriptionMipChain(org::TextureDescription& desc, uint32_t mipLevels)
    {
        if (desc.imageDimensions.empty() || mipLevels == 0u) {
            return;
        }

        const uint32_t baseW = desc.imageDimensions[0].width;
        const uint32_t baseH = desc.imageDimensions[0].height;
        const uint32_t faces = desc.isCubemap ? 6u : 1u;
        const uint32_t slices = faces * (std::max)(1u, desc.arraySize);
        desc.imageDimensions.resize(static_cast<size_t>(slices) * mipLevels);
        const bool blockCompressed = rhi::helpers::IsBlockCompressed(desc.format);

        for (uint32_t s = 0; s < slices; ++s) {
            for (uint32_t m = 0; m < mipLevels; ++m) {
                const uint32_t w = (std::max)(1u, baseW >> m);
                const uint32_t h = (std::max)(1u, baseH >> m);
                const uint32_t idx = m + s * mipLevels;

                desc.imageDimensions[idx].width = w;
                desc.imageDimensions[idx].height = h;
                if (blockCompressed) {
                    const uint32_t blocksWide = (w + 3u) / 4u;
                    const uint32_t blocksHigh = (h + 3u) / 4u;
                    desc.imageDimensions[idx].rowPitch = static_cast<uint64_t>(blocksWide) * 16u;
                    desc.imageDimensions[idx].slicePitch = desc.imageDimensions[idx].rowPitch * blocksHigh;
                }
                else {
                    desc.imageDimensions[idx].rowPitch = uint64_t(w) * desc.channels;
                    desc.imageDimensions[idx].slicePitch = desc.imageDimensions[idx].rowPitch * h;
                }
            }
        }
    }

    bool ShouldPreserveAlphaCoverage(const TextureFileMeta& meta, const org::TextureDescription& desc)
    {
        if (!meta.processing.isParticipatingMaterialTexture || meta.alphaIsAllOpaque) {
            return false;
        }
        if (desc.isArray || desc.isCubemap || desc.channels != 4 || desc.imageDimensions.empty()) {
            return false;
        }

        switch (rhi::helpers::stripSrgb(desc.format)) {
        case rhi::Format::R8G8B8A8_UNorm:
            return true;
        default:
            return false;
        }
    }

    std::shared_ptr<org::Buffer> CreateRawByteAddressBuffer(uint64_t bufferSize, bool unorderedAccess, std::string_view debugName)
    {
        auto buffer = org::Buffer::CreateSharedUnmaterialized(rhi::HeapType::DeviceLocal, bufferSize, unorderedAccess);

        org::BufferBase::DescriptorRequirements requirements{};
        requirements.createSRV = true;
        requirements.createUAV = unorderedAccess;
        requirements.srvDesc = rhi::SrvDesc{
            .dimension = rhi::SrvDim::Buffer,
            .formatOverride = rhi::Format::R32_Typeless,
            .buffer = {
                .kind = rhi::BufferViewKind::Raw,
                .firstElement = 0,
                .numElements = static_cast<uint32_t>(bufferSize / 4u),
                .structureByteStride = 0,
            },
        };
        requirements.uavDesc = rhi::UavDesc{
            .dimension = rhi::UavDim::Buffer,
            .formatOverride = rhi::Format::R32_Typeless,
            .buffer = {
                .kind = rhi::BufferViewKind::Raw,
                .firstElement = 0,
                .numElements = static_cast<uint32_t>(bufferSize / 4u),
                .structureByteStride = 0,
                .counterOffsetInBytes = 0,
            },
        };

        buffer->SetDescriptorRequirements(requirements);
        buffer->Materialize();
        if (!debugName.empty()) {
            buffer->SetName(std::string(debugName));
        }
        return buffer;
    }

    org::TextureDescription BuildBc7CompressedDescription(const TextureSourceData& preparedSourceData, const TextureFileMeta& meta)
    {
        org::TextureDescription desc = preparedSourceData.desc;
        desc.format = meta.preferSRGB ? rhi::Format::BC7_UNorm_sRGB : rhi::Format::BC7_UNorm;
        desc.channels = 4;
        desc.generateMipMaps = false;
        desc.hasUAV = false;
        desc.hasNonShaderVisibleUAV = false;
        desc.uavFormat = rhi::Format::Unknown;

        for (auto& dims : desc.imageDimensions) {
            const uint32_t blocksWide = (dims.width + 3u) / 4u;
            const uint32_t blocksHigh = (dims.height + 3u) / 4u;
            dims.rowPitch = static_cast<uint64_t>(blocksWide) * 16u;
            dims.slicePitch = dims.rowPitch * blocksHigh;
        }

        return desc;
    }

    std::vector<rhi::CopyableFootprint> BuildBc7CompressionFootprints(const org::TextureDescription& desc, uint64_t& totalBytes)
    {
        if (desc.imageDimensions.empty()) {
            throw std::runtime_error("BuildBc7CompressionSubresources: texture description has no image dimensions");
        }

        std::vector<rhi::helpers::SubresourceData> dummySubresources(desc.imageDimensions.size());
        for (auto& subresource : dummySubresources) {
            subresource.pData = reinterpret_cast<const void*>(1);
        }

        const uint32_t mipLevels = static_cast<uint32_t>(desc.imageDimensions.size());
        const rhi::Span<const rhi::helpers::SubresourceData> srcSpan{ dummySubresources.data(), static_cast<uint32_t>(dummySubresources.size()) };
        const auto plan = rhi::helpers::PlanTextureUploadSubresources(
            desc.format,
            desc.imageDimensions[0].width,
            desc.imageDimensions[0].height,
            1,
            mipLevels,
            1,
            srcSpan);

        std::vector<rhi::CopyableFootprint> footprints;
        footprints.reserve(plan.footprints.size());
        for (size_t index = 0; index < plan.footprints.size(); ++index) {
            const auto& footprint = plan.footprints[index];

            rhi::CopyableFootprint copyableFootprint{};
            copyableFootprint.offset = footprint.offset;
            copyableFootprint.rowPitch = footprint.rowPitch;
            copyableFootprint.width = footprint.width;
            copyableFootprint.height = footprint.height;
            copyableFootprint.depth = footprint.depth;
            footprints.push_back(copyableFootprint);
        }

        totalBytes = plan.totalSize;
        return footprints;
    }


}

std::shared_ptr<org::PixelBuffer> TextureFactory::CreateAlwaysResidentPixelBuffer(
    org::TextureDescription desc,
    TextureInitialData initialData,
    std::string_view debugName,
    bool preserveAlphaCoverage,
    bool forceSrgbMipEncoding,
    uint32_t maxMipLevels)
    const {
    if (initialData.Empty()) {
        throw std::runtime_error("CreateAlwaysResidentPixelBuffer: initialData is empty. Use PixelBuffer::CreateShared for data-less textures.");
    }
    if (desc.imageDimensions.empty()) {
        throw std::runtime_error("CreateAlwaysResidentPixelBuffer: desc.imageDimensions must contain at least the base level dimensions.");
    }
    if (desc.channels == 0) {
        throw std::runtime_error("CreateAlwaysResidentPixelBuffer: desc.channels must be set.");
    }

    const uint32_t baseW = desc.imageDimensions[0].width;
    const uint32_t baseH = desc.imageDimensions[0].height;
    const bool doMipmapping = desc.generateMipMaps && !rhi::helpers::IsBlockCompressed(desc.format); // TODO: BC mip gen
    preserveAlphaCoverage = preserveAlphaCoverage
        && doMipmapping
        && !desc.isArray
        && !desc.isCubemap
        && desc.channels == 4
        && rhi::helpers::stripSrgb(desc.format) == rhi::Format::R8G8B8A8_UNorm;

    // if caller asked for mipmaps but only provided mip0 for a single-slice 2D texture, generate full chain on CPU.
    if (doMipmapping) {
        // Build full imageDimensions for *all* subresources so UploadTextureData can compute pitches safely.
        const uint32_t faces = desc.isCubemap ? 6u : 1u;
        const uint32_t slices = faces * uint32_t(desc.arraySize);
        const uint32_t fullMipLevels = CalcMipCount(baseW, baseH);
        const uint32_t mipLevels = maxMipLevels == 0u
            ? fullMipLevels
            : (std::min)(fullMipLevels, maxMipLevels);

        desc.imageDimensions.resize(size_t(slices) * mipLevels);
        for (uint32_t s = 0; s < slices; ++s) {
            for (uint32_t m = 0; m < mipLevels; ++m) {
                const uint32_t w = (std::max)(1u, baseW >> m);
                const uint32_t h = (std::max)(1u, baseH >> m);
                const uint32_t idx = m + s * mipLevels;

                desc.imageDimensions[idx].width = w;
                desc.imageDimensions[idx].height = h;
                desc.imageDimensions[idx].rowPitch = uint64_t(w) * desc.channels;
                desc.imageDimensions[idx].slicePitch = desc.imageDimensions[idx].rowPitch * h;
            }
        }

        // Expand initialData to [slice0 mip0.., slice1 mip0..] with only mip0 filled.
        TextureInitialData gpuInit;
        gpuInit.subresources.assign(size_t(slices) * mipLevels, nullptr);

        if (initialData.subresources.size() == 1) {
            gpuInit.subresources[0] = initialData.subresources[0];
        }
        else {
            // If caller provided mip0 for each slice, copy those
            for (uint32_t s = 0; s < slices && s < initialData.subresources.size(); ++s) {
                gpuInit.subresources[s * mipLevels + 0] = initialData.subresources[s];
            }
        }

        initialData = std::move(gpuInit);
    }

    if (doMipmapping) {
        desc.hasUAV = true; // need UAV mips for GPU mipgen
        if (rhi::helpers::IsSRGB(desc.format)) {
            desc.uavFormat = rhi::Format::Unknown;
        }
    }
    auto pb = org::PixelBuffer::CreateShared(desc);
	org::memory::SetResourceUsageHint(*pb, "Non-material texture assets");

    if (!debugName.empty()) {
        pb->SetName(std::string(debugName));
		org::memory::SetResourceMemoryIdentifier(*pb, std::string(debugName));
    }

    if (!m_uploadService) throw std::runtime_error("TextureFactory upload service generation is unavailable");
    UploadTextureData(*m_uploadService, pb, desc, initialData.subresources, pb->GetMipLevels());

    // Enqueue GPU mipgen (only if mipLevels > 1)
    if (doMipmapping && pb->GetMipLevels() > 1) {
        const bool isSrgb = rhi::helpers::IsSRGB(desc.format) || forceSrgbMipEncoding;
        std::static_pointer_cast<MipmappingPass>(m_mipmappingPass)->EnqueueJob(pb, isSrgb, preserveAlphaCoverage);
    }

    return pb;
}

void TextureFactory::SetReadbackService(
    std::shared_ptr<org::runtime::IReadbackService> readbackService)
{
    std::static_pointer_cast<BC7CompressionReadbackPass>(m_bc7CompressionReadbackPass)
        ->SetReadbackService(std::move(readbackService));
}

bool TextureFactory::SubmitBC7CompressionJob(
    const std::shared_ptr<TextureProcessingJobHandle>& handle,
    std::string_view debugName) const
{
    constexpr uint32_t MaxBC7CompressionJobsInFlight = 4u;

    if (!handle) {
        return false;
    }

    std::shared_ptr<TextureSourceData> preparedSourceData;
    TextureFileMeta requestMeta;
    std::string fallbackName;
    {
        std::scoped_lock lock(handle->mutex);
        preparedSourceData = handle->preparedSourceData;
        requestMeta = handle->requestMeta;
        fallbackName = handle->processingKey;
    }

    if (!preparedSourceData || preparedSourceData->desc.imageDimensions.empty()) {
        return false;
    }

    if (preparedSourceData->isBlockCompressed || preparedSourceData->desc.isArray || preparedSourceData->desc.isCubemap) {
        return false;
    }

    const std::string jobName = debugName.empty() ? fallbackName : std::string(debugName);

    auto* readbackPass = std::static_pointer_cast<BC7CompressionReadbackPass>(m_bc7CompressionReadbackPass).get();
    if (!readbackPass || !readbackPass->HasReadbackService()) {
        spdlog::warn(
            "TextureFactory: BC7 compression job '{}' skipped because readback service is unavailable",
            jobName);
        return false;
    }

    uint32_t inFlightJobs = m_bc7InFlightJobs->load(std::memory_order_acquire);
    while (inFlightJobs < MaxBC7CompressionJobsInFlight &&
        !m_bc7InFlightJobs->compare_exchange_weak(
            inFlightJobs,
            inFlightJobs + 1u,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
    }
    if (inFlightJobs >= MaxBC7CompressionJobsInFlight) {
        return false;
    }

    auto job = std::make_shared<BC7CompressionJob>();
    job->inFlightCounter = m_bc7InFlightJobs;

    const bool preserveAlphaCoverage = ShouldPreserveAlphaCoverage(requestMeta, preparedSourceData->desc);
    const uint32_t preparedMipLevels = GetTextureMipLevelCount(preparedSourceData->desc);

    org::TextureDescription workingDesc = preparedSourceData->desc;
    workingDesc.format = rhi::helpers::stripSrgb(workingDesc.format);
    workingDesc.generateMipMaps = preserveAlphaCoverage && preparedMipLevels == 1u && requestMeta.processing.requestMipChain;
    workingDesc.hasUAV = false;
    workingDesc.uavFormat = rhi::Format::Unknown;

    auto workingTexture = CreateAlwaysResidentPixelBuffer(
        workingDesc,
        TextureInitialData::FromBytes(preparedSourceData->subresources),
        jobName.empty() ? std::string_view("Texture[BC7Working]") : std::string_view(jobName),
        preserveAlphaCoverage,
        requestMeta.preferSRGB,
        requestMeta.processing.maxMipLevels);

    const uint32_t fullGeneratedMipLevels = CalcMipCount(
        preparedSourceData->desc.imageDimensions[0].width,
        preparedSourceData->desc.imageDimensions[0].height);
    const uint32_t generatedMipLevels = workingDesc.generateMipMaps
        ? (requestMeta.processing.maxMipLevels == 0u
            ? fullGeneratedMipLevels
            : (std::min)(fullGeneratedMipLevels, requestMeta.processing.maxMipLevels))
        : preparedMipLevels;
    if (workingTexture->GetMipLevels() != generatedMipLevels) {
        spdlog::error(
            "TextureFactory: BC7 working texture mip mismatch for '{}': expected {} mips but resource created {}",
            jobName,
            generatedMipLevels,
            workingTexture->GetMipLevels());
        return false;
    }

    const uint32_t requestedCompressedMipLevels = preserveAlphaCoverage && requestMeta.processing.requestMipChain
        ? CalcAlphaCoverageExportMipCount(
            preparedSourceData->desc.imageDimensions[0].width,
            preparedSourceData->desc.imageDimensions[0].height)
        : generatedMipLevels;
    const uint32_t compressedMipLevels = requestMeta.processing.maxMipLevels == 0u
        ? requestedCompressedMipLevels
        : (std::min)(requestedCompressedMipLevels, requestMeta.processing.maxMipLevels);

    TextureSourceData compressionLayoutSource = *preparedSourceData;
    if (compressionLayoutSource.desc.imageDimensions.size() != compressedMipLevels) {
        ExpandDescriptionToFullMipChain(compressionLayoutSource.desc);
        ResizeDescriptionMipChain(compressionLayoutSource.desc, compressedMipLevels);
    }

    org::TextureDescription compressedDesc = BuildBc7CompressedDescription(compressionLayoutSource, requestMeta);
    auto compressedTexture = org::PixelBuffer::CreateShared(compressedDesc);
    org::memory::SetResourceUsageHint(*compressedTexture, "Texture processing compressed outputs");
    if (!jobName.empty()) {
        compressedTexture->SetName(jobName + "[BC7]");
        org::memory::SetResourceMemoryIdentifier(*compressedTexture, jobName);
    }

    if (compressedTexture->GetMipLevels() != compressedMipLevels) {
        spdlog::error(
            "TextureFactory: BC7 compressed texture mip mismatch for '{}': expected {} mips but resource created {}",
            jobName,
            compressedMipLevels,
            compressedTexture->GetMipLevels());
        return false;
    }

    if (preserveAlphaCoverage && compressedMipLevels < generatedMipLevels) {
        spdlog::info(
            "TextureFactory: alpha coverage BC7 export for '{}' trimmed mip chain from {} to {} levels",
            jobName,
            generatedMipLevels,
            compressedMipLevels);
    }

    uint64_t outputByteSize = 0;
    auto footprints = BuildBc7CompressionFootprints(compressedDesc, outputByteSize);
    auto blockBuffer = CreateRawByteAddressBuffer(
        outputByteSize,
        true,
        jobName.empty() ? std::string_view("Texture[BC7Blocks]") : std::string_view(jobName + "[BC7Blocks]"));

    job->debugName = jobName;
    job->handle = handle;
    job->workingTexture = std::move(workingTexture);
    job->compressedTexture = std::move(compressedTexture);
    job->blockBuffer = std::move(blockBuffer);
    job->outputHasFullMipChain = compressedMipLevels == CalcMipCount(
        preparedSourceData->desc.imageDimensions[0].width,
        preparedSourceData->desc.imageDimensions[0].height);
    job->subresources.reserve(footprints.size());
    for (uint32_t mip = 0; mip < static_cast<uint32_t>(footprints.size()); ++mip) {
        BC7CompressionSubresource subresource{};
        subresource.footprint = footprints[mip];
        subresource.mip = mip;
        subresource.slice = 0;
        job->subresources.push_back(subresource);
    }
    job->outputByteSize = outputByteSize;

    std::static_pointer_cast<BC7CompressionPass>(m_bc7CompressionPass)->EnqueueJob(job);
    std::static_pointer_cast<BC7CompressionCopyPass>(m_bc7CompressionCopyPass)->EnqueueJob(job);
    readbackPass->EnqueueJob(job);
    return true;
}

std::shared_ptr<org::PixelBuffer> TextureFactory::CreateMaterialResidentPixelBuffer(
	org::TextureDescription desc,
	TextureInitialData initialData,
	std::string_view debugName,
	uint32_t maxMipLevels) const
{
	if (!m_materialTextureTransferService) {
		throw std::runtime_error("material texture transfer service is not initialized");
	}
	if (initialData.Empty() || desc.imageDimensions.empty() || desc.channels == 0) {
		throw std::runtime_error("CreateMaterialResidentPixelBuffer received incomplete texture data");
	}

	const uint32_t slices = (desc.isCubemap ? 6u : 1u) * (std::max)(1u, desc.arraySize);
	const uint32_t sourceMipLevels = GetTextureMipLevelCount(desc);
	if (desc.generateMipMaps && !rhi::helpers::IsBlockCompressed(desc.format) &&
		slices == 1u && sourceMipLevels == 1u && initialData.subresources.size() == 1u) {
		uint32_t mipLevels = CalcMipCount(desc.imageDimensions[0].width, desc.imageDimensions[0].height);
		if (maxMipLevels != 0u) mipLevels = (std::min)(mipLevels, maxMipLevels);
		initialData.subresources = BuildMipChain2D(
			initialData.subresources.front(),
			desc.imageDimensions[0].width,
			desc.imageDimensions[0].height,
			desc.channels,
			mipLevels,
			rhi::helpers::IsSRGB(desc.format));
		ResizeDescriptionMipChain(desc, mipLevels);
	}
	desc.generateMipMaps = false;
	desc.hasUAV = false;
	desc.hasNonShaderVisibleUAV = false;
	desc.initialLayout = rhi::ResourceLayout::Common;
	auto image = org::PixelBuffer::CreateShared(desc);
	org::memory::SetResourceUsageHint(*image, "Material textures");
	if (!debugName.empty()) image->SetName(std::string(debugName));
	if (!debugName.empty()) org::memory::SetResourceMemoryIdentifier(*image, std::string(debugName));
	m_materialTextureTransferService->EnqueueUpload(image, desc, std::move(initialData));
	return image;
}
