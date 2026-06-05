#include "Managers/TerrainManager.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <unordered_map>

#include <spdlog/spdlog.h>

#include "../generated/BuiltinResources.h"
#include "Factories/TextureFactory.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "Resources/Resolvers/ResourceGroupResolver.h"

namespace {
    constexpr std::uint32_t kInvalidDescriptor = 0xffffffffu;

    TerrainRegionGPU MakeFallbackRegion()
    {
        TerrainRegionGPU result{};
        result.weightSampleSide = 19;
        return result;
    }

    TerrainLayerRefGPU MakeFallbackLayerRef()
    {
        return TerrainLayerRefGPU{};
    }

    std::uint32_t MakeFallbackWeightBlock()
    {
        return 0u;
    }

    TerrainLayerGPU MakeFallbackLayer()
    {
        TerrainLayerGPU result{};
        result.diffuseTextureIndex = kInvalidDescriptor;
        result.diffuseSamplerIndex = kInvalidDescriptor;
        result.normalTextureIndex = kInvalidDescriptor;
        result.normalSamplerIndex = kInvalidDescriptor;
        result.heightTextureIndex = kInvalidDescriptor;
        result.heightSamplerIndex = kInvalidDescriptor;
        result.stochasticLayerIndex = kInvalidDescriptor;
        result.normalChannels = { 0u, 1u, 2u };
        result.fallbackColor = { 0.45f, 0.42f, 0.36f, 1.0f };
        result.uvScale = kDefaultTerrainLayerUvScale;
        result.heightScale = 1.0f;
        return result;
    }

    TerrainStochasticLayerGPU MakeFallbackStochasticLayer()
    {
        TerrainStochasticLayerGPU result{};
        result.diffuseGaussianTextureIndex = kInvalidDescriptor;
        result.diffuseInverseLutTextureIndex = kInvalidDescriptor;
        result.diffuseInverseLutSamplerIndex = kInvalidDescriptor;
        result.normalGaussianTextureIndex = kInvalidDescriptor;
        result.normalInverseLutTextureIndex = kInvalidDescriptor;
        result.normalInverseLutSamplerIndex = kInvalidDescriptor;
        result.heightGaussianTextureIndex = kInvalidDescriptor;
        result.heightInverseLutTextureIndex = kInvalidDescriptor;
        result.heightInverseLutSamplerIndex = kInvalidDescriptor;
        result.stochasticScale = kDefaultTerrainStochasticScale;
        result.diffuseLutHeight = 1.0f;
        result.normalLutHeight = 1.0f;
        result.heightLutHeight = 1.0f;
        result.diffuseColorSpaceOrigin = { 0.0f, 0.0f, 0.0f, 0.0f };
        result.diffuseColorSpaceVector0 = { 1.0f, 0.0f, 0.0f, 0.0f };
        result.diffuseColorSpaceVector1 = { 0.0f, 1.0f, 0.0f, 0.0f };
        result.diffuseColorSpaceVector2 = { 0.0f, 0.0f, 1.0f, 0.0f };
        return result;
    }

    TerrainSetGPU MakeEmptySet()
    {
        TerrainSetGPU result{};
        result.regionSizeWorld = kDefaultTerrainRegionSizeWorld;
        return result;
    }

    std::vector<TerrainRegionGPU> BuildDenseRegions(
        const std::vector<TerrainRegionDesc>& source,
        std::int32_t& minRegionX,
        std::int32_t& minRegionY,
        std::uint32_t& regionCountX,
        std::uint32_t& regionCountY)
    {
        if (source.empty()) {
            minRegionX = 0;
            minRegionY = 0;
            regionCountX = 0;
            regionCountY = 0;
            return {};
        }

        std::int32_t maxRegionX = std::numeric_limits<std::int32_t>::min();
        std::int32_t maxRegionY = std::numeric_limits<std::int32_t>::min();
        minRegionX = std::numeric_limits<std::int32_t>::max();
        minRegionY = std::numeric_limits<std::int32_t>::max();
        for (const auto& region : source) {
            minRegionX = (std::min)(minRegionX, region.regionX);
            minRegionY = (std::min)(minRegionY, region.regionY);
            maxRegionX = (std::max)(maxRegionX, region.regionX);
            maxRegionY = (std::max)(maxRegionY, region.regionY);
        }

        regionCountX = static_cast<std::uint32_t>(maxRegionX - minRegionX + 1);
        regionCountY = static_cast<std::uint32_t>(maxRegionY - minRegionY + 1);
        std::vector<TerrainRegionGPU> dense(static_cast<std::size_t>(regionCountX) * regionCountY, MakeFallbackRegion());

        for (const auto& region : source) {
            const auto localX = static_cast<std::uint32_t>(region.regionX - minRegionX);
            const auto localY = static_cast<std::uint32_t>(region.regionY - minRegionY);
            const auto index = static_cast<std::size_t>(localY) * regionCountX + localX;
            auto& out = dense[index];
            out.regionX = region.regionX;
            out.regionY = region.regionY;
            out.layerRefStart = region.layerRefStart;
            out.layerRefCount = region.layerRefCount;
            out.weightBlockStart = region.weightBlockStart;
            out.weightSampleSide = region.weightSampleSide;
        }

        return dense;
    }

    DirectX::XMUINT3 NormalChannelsForTexture(const std::shared_ptr<TextureAsset>& texture)
    {
        if (!texture) {
            return { 0u, 1u, 2u };
        }

        switch (texture->Description().format) {
        case rhi::Format::BC5_UNorm:
        case rhi::Format::BC5_SNorm:
        case rhi::Format::R8G8_UNorm:
        case rhi::Format::R8G8_SNorm:
            return { 0u, 1u, 4u };
        default:
            return { 0u, 1u, 2u };
        }
    }

    bool UploadTerrainTexture(
        const std::shared_ptr<TextureAsset>& texture,
        TextureFactory* textureFactory,
        std::shared_ptr<ResourceGroup>& textureGroup,
        std::vector<std::shared_ptr<TextureAsset>>& retainedTextures,
        bool generateMipmaps,
        std::uint32_t& textureIndex,
        std::uint32_t& samplerIndex)
    {
        textureIndex = kInvalidDescriptor;
        samplerIndex = kInvalidDescriptor;
        if (!texture) {
            return false;
        }

        texture->SetGenerateMipmaps(generateMipmaps);
        if (textureFactory) {
            texture->EnsureUploaded(*textureFactory);
        }
        if (auto image = texture->ImagePtr()) {
            textureIndex = image->GetSRVInfo(0).slot.index;
            samplerIndex = texture->SamplerDescriptorIndex();
            textureGroup->AddResource(image);
            retainedTextures.push_back(texture);
            return true;
        }
        return false;
    }

}

std::unique_ptr<TerrainManager> TerrainManager::CreateUnique()
{
    return std::unique_ptr<TerrainManager>(new TerrainManager());
}

TerrainManager::TerrainManager()
{
    m_sets = DynamicStructuredBuffer<TerrainSetGPU>::CreateShared(1, "Builtin::Terrain::Sets", true);
    m_layers = DynamicStructuredBuffer<TerrainLayerGPU>::CreateShared(1, "Builtin::Terrain::Layers", true);
    m_stochasticLayers = DynamicStructuredBuffer<TerrainStochasticLayerGPU>::CreateShared(1, "Builtin::Terrain::StochasticLayers", true);
    m_layerRefs = DynamicStructuredBuffer<TerrainLayerRefGPU>::CreateShared(1, "Builtin::Terrain::LayerRefs", true);
    m_regions = DynamicStructuredBuffer<TerrainRegionGPU>::CreateShared(1, "Builtin::Terrain::Regions", true);
    m_weightBlocks = DynamicStructuredBuffer<std::uint32_t>::CreateShared(1, "Builtin::Terrain::WeightBlocks", true);
    m_textureGroup = std::make_shared<ResourceGroup>("Builtin::Terrain::TextureGroup");
    rg::memory::SetResourceUsageHint(*m_sets, "Terrain material buffers");
    rg::memory::SetResourceUsageHint(*m_layers, "Terrain material buffers");
    rg::memory::SetResourceUsageHint(*m_stochasticLayers, "Terrain material buffers");
    rg::memory::SetResourceUsageHint(*m_layerRefs, "Terrain material buffers");
    rg::memory::SetResourceUsageHint(*m_regions, "Terrain material buffers");
    rg::memory::SetResourceUsageHint(*m_weightBlocks, "Terrain material buffers");
    m_sets->UpdateAt(0u, MakeEmptySet());
    m_layers->UpdateAt(0u, MakeFallbackLayer());
    m_stochasticLayers->UpdateAt(0u, MakeFallbackStochasticLayer());
    m_layerRefs->UpdateAt(0u, MakeFallbackLayerRef());
    m_regions->UpdateAt(0u, MakeFallbackRegion());
    m_weightBlocks->UpdateAt(0u, MakeFallbackWeightBlock());
}

std::uint32_t TerrainManager::SetActiveTerrain(const TerrainMaterialDesc& desc, TextureFactory* textureFactory)
{
    const auto totalBegin = std::chrono::steady_clock::now();
    ClearActiveTerrain();

    const auto denseBegin = std::chrono::steady_clock::now();
    std::int32_t minRegionX = 0;
    std::int32_t minRegionY = 0;
    std::uint32_t regionCountX = 0;
    std::uint32_t regionCountY = 0;
    auto denseRegions = BuildDenseRegions(desc.regions, minRegionX, minRegionY, regionCountX, regionCountY);
    if (denseRegions.empty()) {
        denseRegions.push_back(MakeFallbackRegion());
    }
    const auto denseEnd = std::chrono::steady_clock::now();

    const std::uint32_t layerCount = (std::max)(1u, static_cast<std::uint32_t>(desc.layers.size()));
    m_layers->Resize(layerCount);
    std::vector<TerrainLayerGPU> layers;
    layers.reserve(layerCount);
    std::vector<TerrainStochasticLayerGPU> stochasticLayers;
    stochasticLayers.reserve(desc.layers.size());
    std::uint32_t snowLayerCount = 0;
    std::uint32_t stochasticLayerCount = 0;
    const auto layersBegin = std::chrono::steady_clock::now();
    for (std::uint32_t i = 0; i < layerCount; ++i) {
        TerrainLayerGPU layer = MakeFallbackLayer();
        if (i < desc.layers.size()) {
            const auto& source = desc.layers[i];
            layer.fallbackColor = source.fallbackColor;
            layer.uvScale = source.uvScale;
            layer.flags = source.flags;
            if ((source.flags & TERRAIN_LAYER_FLAG_SNOW) != 0u) {
                ++snowLayerCount;
            }
            UploadTerrainTexture(
                source.diffuse,
                textureFactory,
                m_textureGroup,
                m_layerTextures,
                true,
                layer.diffuseTextureIndex,
                layer.diffuseSamplerIndex);
            if (UploadTerrainTexture(
                    source.normal,
                    textureFactory,
                    m_textureGroup,
                    m_layerTextures,
                    true,
                    layer.normalTextureIndex,
                    layer.normalSamplerIndex)) {
                layer.normalChannels = NormalChannelsForTexture(source.normal);
            }
            UploadTerrainTexture(
                source.height,
                textureFactory,
                m_textureGroup,
                m_layerTextures,
                true,
                layer.heightTextureIndex,
                layer.heightSamplerIndex);

            TerrainStochasticLayerGPU stochastic = MakeFallbackStochasticLayer();
            stochastic.stochasticScale = source.stochastic.scale > 0.0f
                ? source.stochastic.scale
                : kDefaultTerrainStochasticScale;
            bool hasStochastic = false;
            std::uint32_t textureIndex = kInvalidDescriptor;
            std::uint32_t samplerIndex = kInvalidDescriptor;
            if (UploadTerrainTexture(
                    source.stochastic.diffuse.gaussian,
                    textureFactory,
                    m_textureGroup,
                    m_layerTextures,
                    true,
                    textureIndex,
                    samplerIndex)) {
                stochastic.diffuseGaussianTextureIndex = textureIndex;
                stochastic.diffuseFlags |= TERRAIN_STOCHASTIC_FLAG_DIFFUSE;
                hasStochastic = true;
            }
            if (UploadTerrainTexture(
                    source.stochastic.diffuse.inverseLut,
                    textureFactory,
                    m_textureGroup,
                    m_layerTextures,
                    false,
                    textureIndex,
                    samplerIndex)) {
                stochastic.diffuseInverseLutTextureIndex = textureIndex;
                stochastic.diffuseInverseLutSamplerIndex = samplerIndex;
                stochastic.diffuseLutHeight = static_cast<float>((std::max)(1u, source.stochastic.diffuse.lutHeight));
                stochastic.diffuseFlags |= source.stochastic.diffuse.flags;
                hasStochastic = hasStochastic && stochastic.diffuseGaussianTextureIndex != kInvalidDescriptor;
            }
            else {
                stochastic.diffuseFlags &= ~TERRAIN_STOCHASTIC_FLAG_DIFFUSE;
                hasStochastic = stochastic.normalFlags != 0u;
            }
            if ((stochastic.diffuseFlags & TERRAIN_STOCHASTIC_FLAG_DIFFUSE_COLOR_SPACE) != 0u) {
                stochastic.diffuseColorSpaceOrigin = {
                    source.stochastic.diffuse.colorSpaceOrigin.x,
                    source.stochastic.diffuse.colorSpaceOrigin.y,
                    source.stochastic.diffuse.colorSpaceOrigin.z,
                    0.0f
                };
                stochastic.diffuseColorSpaceVector0 = {
                    source.stochastic.diffuse.colorSpaceVector0.x,
                    source.stochastic.diffuse.colorSpaceVector0.y,
                    source.stochastic.diffuse.colorSpaceVector0.z,
                    0.0f
                };
                stochastic.diffuseColorSpaceVector1 = {
                    source.stochastic.diffuse.colorSpaceVector1.x,
                    source.stochastic.diffuse.colorSpaceVector1.y,
                    source.stochastic.diffuse.colorSpaceVector1.z,
                    0.0f
                };
                stochastic.diffuseColorSpaceVector2 = {
                    source.stochastic.diffuse.colorSpaceVector2.x,
                    source.stochastic.diffuse.colorSpaceVector2.y,
                    source.stochastic.diffuse.colorSpaceVector2.z,
                    0.0f
                };
            }
            if (UploadTerrainTexture(
                    source.stochastic.normal.gaussian,
                    textureFactory,
                    m_textureGroup,
                    m_layerTextures,
                    true,
                    textureIndex,
                    samplerIndex)) {
                stochastic.normalGaussianTextureIndex = textureIndex;
                stochastic.normalFlags |= TERRAIN_STOCHASTIC_FLAG_NORMAL;
                hasStochastic = true;
            }
            if (UploadTerrainTexture(
                    source.stochastic.normal.inverseLut,
                    textureFactory,
                    m_textureGroup,
                    m_layerTextures,
                    false,
                    textureIndex,
                    samplerIndex)) {
                stochastic.normalInverseLutTextureIndex = textureIndex;
                stochastic.normalInverseLutSamplerIndex = samplerIndex;
                stochastic.normalLutHeight = static_cast<float>((std::max)(1u, source.stochastic.normal.lutHeight));
                stochastic.normalFlags |= source.stochastic.normal.flags;
                hasStochastic = hasStochastic && stochastic.normalGaussianTextureIndex != kInvalidDescriptor;
            }
            else {
                stochastic.normalFlags &= ~TERRAIN_STOCHASTIC_FLAG_NORMAL;
                hasStochastic = (stochastic.diffuseFlags & TERRAIN_STOCHASTIC_FLAG_DIFFUSE) != 0u;
            }
            if (UploadTerrainTexture(
                    source.stochastic.height.gaussian,
                    textureFactory,
                    m_textureGroup,
                    m_layerTextures,
                    true,
                    textureIndex,
                    samplerIndex)) {
                stochastic.heightGaussianTextureIndex = textureIndex;
                stochastic.heightFlags |= TERRAIN_STOCHASTIC_FLAG_HEIGHT;
                hasStochastic = true;
            }
            if (UploadTerrainTexture(
                    source.stochastic.height.inverseLut,
                    textureFactory,
                    m_textureGroup,
                    m_layerTextures,
                    false,
                    textureIndex,
                    samplerIndex)) {
                stochastic.heightInverseLutTextureIndex = textureIndex;
                stochastic.heightInverseLutSamplerIndex = samplerIndex;
                stochastic.heightLutHeight = static_cast<float>((std::max)(1u, source.stochastic.height.lutHeight));
                stochastic.heightFlags |= source.stochastic.height.flags;
                hasStochastic = hasStochastic && stochastic.heightGaussianTextureIndex != kInvalidDescriptor;
            }
            else {
                stochastic.heightFlags &= ~TERRAIN_STOCHASTIC_FLAG_HEIGHT;
                hasStochastic = ((stochastic.diffuseFlags & TERRAIN_STOCHASTIC_FLAG_DIFFUSE) != 0u) ||
                    ((stochastic.normalFlags & TERRAIN_STOCHASTIC_FLAG_NORMAL) != 0u);
            }
            if (hasStochastic) {
                layer.stochasticLayerIndex = static_cast<std::uint32_t>(stochasticLayers.size());
                stochasticLayers.push_back(stochastic);
                ++stochasticLayerCount;
            }
        }
        layers.push_back(layer);
    }
    m_layers->ReplaceData(std::move(layers));
    const auto layersEnd = std::chrono::steady_clock::now();

    if (stochasticLayers.empty()) {
        stochasticLayers.push_back(MakeFallbackStochasticLayer());
    }
    m_stochasticLayers->ReplaceData(std::move(stochasticLayers));
    const auto stochasticEnd = std::chrono::steady_clock::now();

    const auto layerRefCount = (std::max)(1u, static_cast<std::uint32_t>(desc.layerRefs.size()));
    std::vector<TerrainLayerRefGPU> layerRefs;
    layerRefs.reserve(layerRefCount);
    for (std::uint32_t i = 0; i < layerRefCount; ++i) {
        TerrainLayerRefGPU layerRef = MakeFallbackLayerRef();
        if (i < desc.layerRefs.size()) {
            layerRef.layerIndex = desc.layerRefs[i].layerIndex;
        }
        layerRefs.push_back(layerRef);
    }
    m_layerRefs->ReplaceData(std::move(layerRefs));
    const auto layerRefsEnd = std::chrono::steady_clock::now();

    const auto weightBlockCount = (std::max)(1u, static_cast<std::uint32_t>(desc.weightBlocks.size()));
    if (desc.weightBlocks.empty()) {
        m_weightBlocks->ReplaceData(std::vector<std::uint32_t>{ MakeFallbackWeightBlock() });
    }
    else {
        m_weightBlocks->ReplaceData(desc.weightBlocks);
    }
    const auto weightBlocksEnd = std::chrono::steady_clock::now();

    const auto regionCount = static_cast<std::uint32_t>(denseRegions.size());
    m_regions->ReplaceData(std::move(denseRegions));
    const auto regionsEnd = std::chrono::steady_clock::now();

    TerrainSetGPU set = MakeEmptySet();
    set.minRegionX = minRegionX;
    set.minRegionY = minRegionY;
    set.regionCountX = regionCountX;
    set.regionCountY = regionCountY;
    set.regionBase = 0;
    set.regionCount = regionCount;
    set.layerBase = 0;
    set.layerCount = layerCount;
    set.layerRefBase = 0;
    set.layerRefCount = layerRefCount;
    set.weightBlockBase = 0;
    set.weightBlockCount = weightBlockCount;
    set.regionSizeWorld = desc.regionSizeWorld > 0.0f ? desc.regionSizeWorld : kDefaultTerrainRegionSizeWorld;
    m_sets->UpdateAt(0u, set);
    const auto totalEnd = std::chrono::steady_clock::now();
    const auto elapsedMs = [](auto begin, auto end) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();
    };
    spdlog::info(
        "Terrain close-landscape material active: layers={} snowLayers={} stochasticLayers={} regions={} layerRefs={} weightWords={} regionSize={} lodLandBlend=disabled",
        layerCount,
        snowLayerCount,
        stochasticLayerCount,
        regionCount,
        layerRefCount,
        weightBlockCount,
        set.regionSizeWorld);
    spdlog::info(
        "Terrain close-landscape material timing: total={}ms denseRegions={}ms layersAndTextures={}ms stochasticUpload={}ms layerRefs={}ms weightBlocks={}ms regions={}ms",
        elapsedMs(totalBegin, totalEnd),
        elapsedMs(denseBegin, denseEnd),
        elapsedMs(layersBegin, layersEnd),
        elapsedMs(layersEnd, stochasticEnd),
        elapsedMs(stochasticEnd, layerRefsEnd),
        elapsedMs(layerRefsEnd, weightBlocksEnd),
        elapsedMs(weightBlocksEnd, regionsEnd));
    return 0u;
}

void TerrainManager::ClearActiveTerrain()
{
    if (m_textureGroup) {
        for (auto& texture : m_layerTextures) {
            if (texture && texture->ImagePtr()) {
                m_textureGroup->RemoveResource(texture->ImagePtr().get());
            }
        }
    }
    m_layerTextures.clear();
    m_sets->UpdateAt(0u, MakeEmptySet());
    m_layers->UpdateAt(0u, MakeFallbackLayer());
    m_stochasticLayers->UpdateAt(0u, MakeFallbackStochasticLayer());
    m_layerRefs->UpdateAt(0u, MakeFallbackLayerRef());
    m_regions->UpdateAt(0u, MakeFallbackRegion());
    m_weightBlocks->UpdateAt(0u, MakeFallbackWeightBlock());
}

std::shared_ptr<Resource> TerrainManager::ProvideResource(ResourceIdentifier const& key)
{
    const auto text = key.ToString();
    if (text == Builtin::Terrain::Sets) {
        return m_sets;
    }
    if (text == Builtin::Terrain::Layers) {
        return m_layers;
    }
    if (text == Builtin::Terrain::StochasticLayers) {
        return m_stochasticLayers;
    }
    if (text == Builtin::Terrain::LayerRefs) {
        return m_layerRefs;
    }
    if (text == Builtin::Terrain::Regions) {
        return m_regions;
    }
    if (text == Builtin::Terrain::WeightBlocks) {
        return m_weightBlocks;
    }
    return nullptr;
}

std::vector<ResourceIdentifier> TerrainManager::GetSupportedKeys()
{
    return {
        Builtin::Terrain::Sets,
        Builtin::Terrain::Layers,
        Builtin::Terrain::StochasticLayers,
        Builtin::Terrain::LayerRefs,
        Builtin::Terrain::Regions,
        Builtin::Terrain::WeightBlocks,
    };
}

std::vector<ResourceIdentifier> TerrainManager::GetSupportedResolverKeys()
{
    return { Builtin::Terrain::TextureGroup };
}

std::shared_ptr<IResourceResolver> TerrainManager::ProvideResolver(ResourceIdentifier const& key)
{
    if (key.ToString() == Builtin::Terrain::TextureGroup) {
        return std::make_shared<ResourceGroupResolver>(m_textureGroup);
    }
    return nullptr;
}
