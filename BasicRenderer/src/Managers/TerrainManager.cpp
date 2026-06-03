#include "Managers/TerrainManager.h"

#include <algorithm>
#include <limits>
#include <unordered_map>

#include <spdlog/spdlog.h>

#include "../generated/BuiltinResources.h"
#include "Factories/TextureFactory.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "Resources/Resolvers/ResourceGroupResolver.h"
#include "Resources/Sampler.h"

namespace {
    constexpr std::uint32_t kInvalidDescriptor = 0xffffffffu;

    std::uint32_t LayerCountForQuadrant(const TerrainQuadrantDesc& desc)
    {
        std::uint32_t count = 1;
        for (std::uint32_t i = 1; i < kTerrainMaxBlendLayers; ++i) {
            if (desc.layerIndices[i] != 0u) {
                count = i + 1u;
            }
        }
        return count;
    }

    TerrainQuadrantGPU MakeFallbackQuadrant()
    {
        TerrainQuadrantGPU result{};
        result.layerCount = 1;
        result.weightAtlasStride = 19;
        return result;
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
        result.weightAtlas0TextureIndex = kInvalidDescriptor;
        result.weightAtlas1TextureIndex = kInvalidDescriptor;
        result.weightAtlas2TextureIndex = kInvalidDescriptor;
        result.weightAtlasSamplerIndex = kInvalidDescriptor;
        return result;
    }

    std::vector<TerrainQuadrantGPU> BuildDenseQuadrants(
        const std::vector<TerrainQuadrantDesc>& source,
        std::int32_t& minCellX,
        std::int32_t& minCellY,
        std::uint32_t& cellCountX,
        std::uint32_t& cellCountY)
    {
        if (source.empty()) {
            minCellX = 0;
            minCellY = 0;
            cellCountX = 0;
            cellCountY = 0;
            return {};
        }

        std::int32_t maxCellX = std::numeric_limits<std::int32_t>::min();
        std::int32_t maxCellY = std::numeric_limits<std::int32_t>::min();
        minCellX = std::numeric_limits<std::int32_t>::max();
        minCellY = std::numeric_limits<std::int32_t>::max();
        for (const auto& q : source) {
            minCellX = (std::min)(minCellX, q.cellX);
            minCellY = (std::min)(minCellY, q.cellY);
            maxCellX = (std::max)(maxCellX, q.cellX);
            maxCellY = (std::max)(maxCellY, q.cellY);
        }

        cellCountX = static_cast<std::uint32_t>(maxCellX - minCellX + 1);
        cellCountY = static_cast<std::uint32_t>(maxCellY - minCellY + 1);
        std::vector<TerrainQuadrantGPU> dense(static_cast<std::size_t>(cellCountX) * cellCountY * 4u, MakeFallbackQuadrant());

        for (const auto& q : source) {
            if (q.quadrant >= 4u) {
                continue;
            }
            const auto localX = static_cast<std::uint32_t>(q.cellX - minCellX);
            const auto localY = static_cast<std::uint32_t>(q.cellY - minCellY);
            const auto index = (static_cast<std::size_t>(localY) * cellCountX + localX) * 4u + q.quadrant;
            auto& out = dense[index];
            out.cellX = q.cellX;
            out.cellY = q.cellY;
            out.quadrant = q.quadrant;
            out.layerCount = LayerCountForQuadrant(q);
            out.weightAtlasX = q.weightAtlasX;
            out.weightAtlasY = q.weightAtlasY;
            out.weightAtlasStride = q.weightAtlasStride;
            for (std::uint32_t i = 0; i < kTerrainMaxBlendLayers; ++i) {
                out.layerIndices[i] = q.layerIndices[i];
            }
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

    std::shared_ptr<Sampler> GetTerrainWeightSampler()
    {
        rhi::SamplerDesc samplerDesc{};
        samplerDesc.minFilter = rhi::Filter::Linear;
        samplerDesc.magFilter = rhi::Filter::Linear;
        samplerDesc.mipFilter = rhi::MipFilter::Nearest;
        samplerDesc.addressU = rhi::AddressMode::Clamp;
        samplerDesc.addressV = rhi::AddressMode::Clamp;
        samplerDesc.addressW = rhi::AddressMode::Clamp;
        samplerDesc.mipLodBias = 0.0f;
        samplerDesc.minLod = 0.0f;
        samplerDesc.maxLod = 0.0f;
        samplerDesc.maxAnisotropy = 1;
        samplerDesc.compareEnable = false;
        samplerDesc.compareOp = rhi::CompareOp::Always;
        samplerDesc.reduction = rhi::ReductionMode::Standard;
        samplerDesc.borderPreset = rhi::BorderPreset::TransparentBlack;
        return Sampler::CreateSampler(samplerDesc);
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
    m_quadrants = DynamicStructuredBuffer<TerrainQuadrantGPU>::CreateShared(1, "Builtin::Terrain::Quadrants", true);
    m_textureGroup = std::make_shared<ResourceGroup>("Builtin::Terrain::TextureGroup");
    rg::memory::SetResourceUsageHint(*m_sets, "Terrain material buffers");
    rg::memory::SetResourceUsageHint(*m_layers, "Terrain material buffers");
    rg::memory::SetResourceUsageHint(*m_quadrants, "Terrain material buffers");
    m_sets->UpdateAt(0u, MakeEmptySet());
    m_layers->UpdateAt(0u, MakeFallbackLayer());
    m_quadrants->UpdateAt(0u, MakeFallbackQuadrant());
}

std::uint32_t TerrainManager::SetActiveTerrain(const TerrainMaterialDesc& desc, TextureFactory* textureFactory)
{
    ClearActiveTerrain();

    std::int32_t minCellX = 0;
    std::int32_t minCellY = 0;
    std::uint32_t cellCountX = 0;
    std::uint32_t cellCountY = 0;
    auto denseQuadrants = BuildDenseQuadrants(desc.quadrants, minCellX, minCellY, cellCountX, cellCountY);
    if (denseQuadrants.empty()) {
        denseQuadrants.push_back(MakeFallbackQuadrant());
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

    const auto quadrantCount = static_cast<std::uint32_t>(denseQuadrants.size());
    m_quadrants->Resize(quadrantCount);
    for (std::uint32_t i = 0; i < quadrantCount; ++i) {
        m_quadrants->UpdateAt(i, denseQuadrants[i]);
    }

    m_weightAtlas0 = CreateWeightAtlasTexture(
        "Terrain Weight Atlas 0",
        desc.weightAtlasWidth,
        desc.weightAtlasHeight,
        desc.weights0Rgba8,
        textureFactory);
    m_weightAtlas1 = CreateWeightAtlasTexture(
        "Terrain Weight Atlas 1",
        desc.weightAtlasWidth,
        desc.weightAtlasHeight,
        desc.weights1Rgba8,
        textureFactory);
    m_weightAtlas2 = CreateWeightAtlasTexture(
        "Terrain Weight Atlas 2",
        desc.weightAtlasWidth,
        desc.weightAtlasHeight,
        desc.weights2Rgba8,
        textureFactory);

    TerrainSetGPU set = MakeEmptySet();
    set.minCellX = minCellX;
    set.minCellY = minCellY;
    set.cellCountX = cellCountX;
    set.cellCountY = cellCountY;
    set.quadrantBase = 0;
    set.quadrantCount = quadrantCount;
    set.layerBase = 0;
    set.layerCount = layerCount;
    set.weightAtlasWidth = desc.weightAtlasWidth;
    set.weightAtlasHeight = desc.weightAtlasHeight;
    if (m_weightAtlas0 && m_weightAtlas0->ImagePtr()) {
        set.weightAtlas0TextureIndex = m_weightAtlas0->ImagePtr()->GetSRVInfo(0).slot.index;
        set.weightAtlasSamplerIndex = m_weightAtlas0->SamplerDescriptorIndex();
        m_textureGroup->AddResource(m_weightAtlas0->ImagePtr());
    }
    if (m_weightAtlas1 && m_weightAtlas1->ImagePtr()) {
        set.weightAtlas1TextureIndex = m_weightAtlas1->ImagePtr()->GetSRVInfo(0).slot.index;
        if (set.weightAtlasSamplerIndex == kInvalidDescriptor) {
            set.weightAtlasSamplerIndex = m_weightAtlas1->SamplerDescriptorIndex();
        }
        m_textureGroup->AddResource(m_weightAtlas1->ImagePtr());
    }
    if (m_weightAtlas2 && m_weightAtlas2->ImagePtr()) {
        set.weightAtlas2TextureIndex = m_weightAtlas2->ImagePtr()->GetSRVInfo(0).slot.index;
        if (set.weightAtlasSamplerIndex == kInvalidDescriptor) {
            set.weightAtlasSamplerIndex = m_weightAtlas2->SamplerDescriptorIndex();
        }
        m_textureGroup->AddResource(m_weightAtlas2->ImagePtr());
    }
    m_sets->UpdateAt(0u, set);
    spdlog::info(
        "Terrain close-landscape material active: layers={} snowLayers={} quadrants={} weightAtlas={}x{} lodLandBlend=disabled",
        layerCount,
        snowLayerCount,
        quadrantCount,
        desc.weightAtlasWidth,
        desc.weightAtlasHeight);
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
        if (m_weightAtlas0 && m_weightAtlas0->ImagePtr()) {
            m_textureGroup->RemoveResource(m_weightAtlas0->ImagePtr().get());
        }
        if (m_weightAtlas1 && m_weightAtlas1->ImagePtr()) {
            m_textureGroup->RemoveResource(m_weightAtlas1->ImagePtr().get());
        }
        if (m_weightAtlas2 && m_weightAtlas2->ImagePtr()) {
            m_textureGroup->RemoveResource(m_weightAtlas2->ImagePtr().get());
        }
    }
    m_layerTextures.clear();
    m_weightAtlas0.reset();
    m_weightAtlas1.reset();
    m_weightAtlas2.reset();
    m_sets->UpdateAt(0u, MakeEmptySet());
    m_layers->UpdateAt(0u, MakeFallbackLayer());
    m_quadrants->UpdateAt(0u, MakeFallbackQuadrant());
}

std::shared_ptr<TextureAsset> TerrainManager::CreateWeightAtlasTexture(
    const char* name,
    std::uint32_t width,
    std::uint32_t height,
    const std::vector<std::uint8_t>& rgba8,
    TextureFactory* textureFactory)
{
    if (width == 0u || height == 0u || rgba8.size() < static_cast<std::size_t>(width) * height * 4u) {
        return nullptr;
    }

    TextureDescription textureDesc{};
    textureDesc.channels = 4;
    textureDesc.format = rhi::Format::R8G8B8A8_UNorm;
    textureDesc.hasSRV = true;
    textureDesc.imageDimensions.push_back(ImageDimensions{
        width,
        height,
        static_cast<std::uint64_t>(width) * 4u,
        static_cast<std::uint64_t>(width) * height * 4u,
    });
    TextureAsset::BytesList bytes;
    bytes.push_back(std::make_shared<std::vector<std::uint8_t>>(rgba8));
    auto texture = TextureAsset::CreateShared(textureDesc, std::move(bytes), GetTerrainWeightSampler(), TextureFileMeta{});
    texture->SetName(name);
    if (textureFactory) {
        texture->EnsureUploaded(*textureFactory);
    }
    if (auto image = texture->ImagePtr()) {
        rg::memory::SetResourceUsageHint(*image, "Terrain weight atlases");
    }
    return texture;
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
    if (text == Builtin::Terrain::Quadrants) {
        return m_quadrants;
    }
    return nullptr;
}

std::vector<ResourceIdentifier> TerrainManager::GetSupportedKeys()
{
    return {
        Builtin::Terrain::Sets,
        Builtin::Terrain::Layers,
        Builtin::Terrain::Quadrants,
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
