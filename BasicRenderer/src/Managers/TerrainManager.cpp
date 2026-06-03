#include "Managers/TerrainManager.h"

#include <algorithm>
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
        result.normalChannels = { 0u, 1u, 2u };
        result.fallbackColor = { 0.45f, 0.42f, 0.36f, 1.0f };
        result.uvScale = kDefaultTerrainLayerUvScale;
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

}

std::unique_ptr<TerrainManager> TerrainManager::CreateUnique()
{
    return std::unique_ptr<TerrainManager>(new TerrainManager());
}

TerrainManager::TerrainManager()
{
    m_sets = DynamicStructuredBuffer<TerrainSetGPU>::CreateShared(1, "Builtin::Terrain::Sets", true);
    m_layers = DynamicStructuredBuffer<TerrainLayerGPU>::CreateShared(1, "Builtin::Terrain::Layers", true);
    m_layerRefs = DynamicStructuredBuffer<TerrainLayerRefGPU>::CreateShared(1, "Builtin::Terrain::LayerRefs", true);
    m_regions = DynamicStructuredBuffer<TerrainRegionGPU>::CreateShared(1, "Builtin::Terrain::Regions", true);
    m_weightBlocks = DynamicStructuredBuffer<std::uint32_t>::CreateShared(1, "Builtin::Terrain::WeightBlocks", true);
    m_textureGroup = std::make_shared<ResourceGroup>("Builtin::Terrain::TextureGroup");
    rg::memory::SetResourceUsageHint(*m_sets, "Terrain material buffers");
    rg::memory::SetResourceUsageHint(*m_layers, "Terrain material buffers");
    rg::memory::SetResourceUsageHint(*m_layerRefs, "Terrain material buffers");
    rg::memory::SetResourceUsageHint(*m_regions, "Terrain material buffers");
    rg::memory::SetResourceUsageHint(*m_weightBlocks, "Terrain material buffers");
    m_sets->UpdateAt(0u, MakeEmptySet());
    m_layers->UpdateAt(0u, MakeFallbackLayer());
    m_layerRefs->UpdateAt(0u, MakeFallbackLayerRef());
    m_regions->UpdateAt(0u, MakeFallbackRegion());
    m_weightBlocks->UpdateAt(0u, MakeFallbackWeightBlock());
}

std::uint32_t TerrainManager::SetActiveTerrain(const TerrainMaterialDesc& desc, TextureFactory* textureFactory)
{
    ClearActiveTerrain();

    std::int32_t minRegionX = 0;
    std::int32_t minRegionY = 0;
    std::uint32_t regionCountX = 0;
    std::uint32_t regionCountY = 0;
    auto denseRegions = BuildDenseRegions(desc.regions, minRegionX, minRegionY, regionCountX, regionCountY);
    if (denseRegions.empty()) {
        denseRegions.push_back(MakeFallbackRegion());
    }

    const std::uint32_t layerCount = (std::max)(1u, static_cast<std::uint32_t>(desc.layers.size()));
    m_layers->Resize(layerCount);
    std::uint32_t snowLayerCount = 0;
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
            if (source.diffuse) {
                source.diffuse->SetGenerateMipmaps(true);
                if (textureFactory) {
                    source.diffuse->EnsureUploaded(*textureFactory);
                }
                if (auto image = source.diffuse->ImagePtr()) {
                    layer.diffuseTextureIndex = image->GetSRVInfo(0).slot.index;
                    layer.diffuseSamplerIndex = source.diffuse->SamplerDescriptorIndex();
                    m_textureGroup->AddResource(image);
                    m_layerTextures.push_back(source.diffuse);
                }
            }
            if (source.normal) {
                source.normal->SetGenerateMipmaps(true);
                if (textureFactory) {
                    source.normal->EnsureUploaded(*textureFactory);
                }
                if (auto image = source.normal->ImagePtr()) {
                    layer.normalTextureIndex = image->GetSRVInfo(0).slot.index;
                    layer.normalSamplerIndex = source.normal->SamplerDescriptorIndex();
                    layer.normalChannels = NormalChannelsForTexture(source.normal);
                    m_textureGroup->AddResource(image);
                    m_layerTextures.push_back(source.normal);
                }
            }
        }
        m_layers->UpdateAt(i, layer);
    }

    const auto layerRefCount = (std::max)(1u, static_cast<std::uint32_t>(desc.layerRefs.size()));
    m_layerRefs->Resize(layerRefCount);
    for (std::uint32_t i = 0; i < layerRefCount; ++i) {
        TerrainLayerRefGPU layerRef = MakeFallbackLayerRef();
        if (i < desc.layerRefs.size()) {
            layerRef.layerIndex = desc.layerRefs[i].layerIndex;
        }
        m_layerRefs->UpdateAt(i, layerRef);
    }

    const auto weightBlockCount = (std::max)(1u, static_cast<std::uint32_t>(desc.weightBlocks.size()));
    m_weightBlocks->Resize(weightBlockCount);
    for (std::uint32_t i = 0; i < weightBlockCount; ++i) {
        auto weightBlock = MakeFallbackWeightBlock();
        if (i < desc.weightBlocks.size()) {
            weightBlock = desc.weightBlocks[i];
        }
        m_weightBlocks->UpdateAt(i, weightBlock);
    }

    const auto regionCount = static_cast<std::uint32_t>(denseRegions.size());
    m_regions->Resize(regionCount);
    for (std::uint32_t i = 0; i < regionCount; ++i) {
        m_regions->UpdateAt(i, denseRegions[i]);
    }

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
    spdlog::info(
        "Terrain close-landscape material active: layers={} snowLayers={} regions={} layerRefs={} weightWords={} regionSize={} lodLandBlend=disabled",
        layerCount,
        snowLayerCount,
        regionCount,
        layerRefCount,
        weightBlockCount,
        set.regionSizeWorld);
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
