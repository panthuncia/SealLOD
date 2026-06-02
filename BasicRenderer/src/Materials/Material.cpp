#include "Materials/Material.h"
#include <string>
#include <spdlog/spdlog.h>
#include "Render/PSOFlags.h"
#include "Utilities/Utilities.h"
#include "Materials/MaterialFlags.h"
#include "Resources/PixelBuffer.h"
#include "Render/MemoryIntrospectionAPI.h"

Material::Material(const std::string& name,
    MaterialFlags materialFlags, PSOFlags psoFlags)
    : m_name(name), m_psoFlags(psoFlags) {
    auto& resourceManager = ResourceManager::GetInstance();
    m_materialData.materialFlags = materialFlags;
}

Material::Material(const std::string& name,
    MaterialFlags materialFlags, PSOFlags psoFlags,
    std::shared_ptr<TextureAsset> baseColorTexture,
    std::shared_ptr<TextureAsset> normalTexture,
    std::shared_ptr<TextureAsset> aoMap,
    std::shared_ptr<TextureAsset> heightMap,
    std::shared_ptr<TextureAsset> metallicTexture,
    std::shared_ptr<TextureAsset> roughnessTexture,
    std::shared_ptr<TextureAsset> emissiveTexture,
    std::shared_ptr<TextureAsset> opacityTexture,
    float metallicFactor,
    float roughnessFactor,
    DirectX::XMFLOAT4 baseColorFactor,
    DirectX::XMFLOAT4 emissiveFactor,
	std::vector<uint32_t> baseColorChannels,
	std::vector<uint32_t> normalChannels,
	std::vector<uint32_t> aoChannel,
	std::vector<uint32_t> heightChannel,
	std::vector<uint32_t> metallicChannel,
	std::vector<uint32_t> roughnessChannel,
	std::vector<uint32_t> emissiveChannels,
    uint32_t baseColorUvSetIndex,
    uint32_t normalUvSetIndex,
    uint32_t aoUvSetIndex,
    uint32_t heightUvSetIndex,
    uint32_t metallicUvSetIndex,
    uint32_t roughnessUvSetIndex,
    uint32_t emissiveUvSetIndex,
    uint32_t opacityUvSetIndex,
	float heightMapScale,
	float geometricDisplacementMin,
	float geometricDisplacementMax,
	bool geometricDisplacementEnabled,
    TechniqueDescriptor technique,
    OpenPBRMaterialParameters openPBRMaterial,
    OpenPBRTextureBindings openPBRTextures,
    float alphaCutoff,
    bool brniflyVertexAlpha,
    bool brniflyZBufferWrite,
    bool brniflyDecal,
    bool brniflyDynamicDecal)
    : m_name(name),
    m_psoFlags(psoFlags),
    m_baseColorTexture(baseColorTexture),
    m_normalTexture(normalTexture),
    m_aoMap(aoMap),
    m_heightMap(heightMap),
    m_metallicTexture(metallicTexture),
    m_roughnessTexture(roughnessTexture),
    m_emissiveTexture(emissiveTexture),
	m_opacityTexture(opacityTexture),
    m_baseColorUvSetIndex(baseColorUvSetIndex),
    m_normalUvSetIndex(normalUvSetIndex),
    m_aoUvSetIndex(aoUvSetIndex),
    m_heightUvSetIndex(heightUvSetIndex),
    m_metallicUvSetIndex(metallicUvSetIndex),
    m_roughnessUvSetIndex(roughnessUvSetIndex),
    m_emissiveUvSetIndex(emissiveUvSetIndex),
    m_opacityUvSetIndex(opacityUvSetIndex),
    m_metallicFactor(metallicFactor),
    m_roughnessFactor(roughnessFactor),
    m_baseColorFactor(baseColorFactor),
    m_emissiveFactor(emissiveFactor),
	m_technique(technique),
    m_openPBRMaterial(openPBRMaterial),
    m_openPBRTextures(openPBRTextures),
    m_brniflyVertexAlpha(brniflyVertexAlpha),
    m_brniflyZBufferWrite(brniflyZBufferWrite),
    m_brniflyDecal(brniflyDecal),
    m_brniflyDynamicDecal(brniflyDynamicDecal)
{
    m_materialData.materialFlags = materialFlags;
    m_materialData.ambientStrength = 0.5f;
    m_materialData.specularStrength = 2.0f;
    m_materialData.heightMapScale = heightMapScale;
    m_materialData.textureScale = 1.0f;
    m_materialData.geometricDisplacementMin = geometricDisplacementMin;
    m_materialData.geometricDisplacementMax = geometricDisplacementMax;
    m_materialData.geometricDisplacementEnabled = geometricDisplacementEnabled ? 1u : 0u;
    m_materialData.baseColorFactor = baseColorFactor;
    m_materialData.emissiveFactor = emissiveFactor;
    m_materialData.metallicFactor = metallicFactor;
    m_materialData.roughnessFactor = roughnessFactor;
    m_materialData.alphaCutoff = alphaCutoff;
	m_baseColorChannels = baseColorChannels;
	m_normalChannels = normalChannels;
	m_aoChannel = aoChannel;
	m_heightChannel = heightChannel;
	m_metallicChannel = metallicChannel;
	m_roughnessChannel = roughnessChannel;
	m_emissiveChannels = emissiveChannels;
    m_materialData.baseColorUvSetIndex = m_baseColorUvSetIndex;
    m_materialData.normalUvSetIndex = m_normalUvSetIndex;
    m_materialData.aoUvSetIndex = m_aoUvSetIndex;
    m_materialData.heightUvSetIndex = m_heightUvSetIndex;
    m_materialData.metallicUvSetIndex = m_metallicUvSetIndex;
    m_materialData.roughnessUvSetIndex = m_roughnessUvSetIndex;
    m_materialData.emissiveUvSetIndex = m_emissiveUvSetIndex;
    m_materialData.opacityUvSetIndex = m_opacityUvSetIndex;

}

Material::~Material() {
}

void Material::SetLogicalTextureSourcePaths(const MaterialDescription& desc)
{
    m_baseColorSourcePath = desc.baseColor.sourcePath;
    m_normalSourcePath = desc.normal.sourcePath;
    m_aoSourcePath = desc.aoMap.sourcePath;
    m_heightSourcePath = desc.heightMap.sourcePath;
    m_roughnessSourcePath = desc.roughness.sourcePath;
    m_metallicSourcePath = desc.metallic.sourcePath;
    m_emissiveSourcePath = desc.emissive.sourcePath;
    m_opacitySourcePath = desc.opacity.sourcePath;
}

void Material::ForEachReferencedTexture(const std::function<void(const std::shared_ptr<TextureAsset>&)>& visitor) const {
    auto visitTexture = [&](const std::shared_ptr<TextureAsset>& texture) {
        if (texture) {
            visitor(texture);
        }
    };

    visitTexture(m_baseColorTexture);
    visitTexture(m_normalTexture);
    visitTexture(m_aoMap);
    visitTexture(m_heightMap);
    visitTexture(m_metallicTexture);
    visitTexture(m_roughnessTexture);
    visitTexture(m_emissiveTexture);
    visitTexture(m_opacityTexture);

    visitTexture(m_openPBRTextures.coatColor.texture);
    visitTexture(m_openPBRTextures.coatWeight.texture);
    visitTexture(m_openPBRTextures.coatRoughness.texture);
    visitTexture(m_openPBRTextures.fuzzColor.texture);
    visitTexture(m_openPBRTextures.fuzzWeight.texture);
    visitTexture(m_openPBRTextures.fuzzRoughness.texture);
}

MaterialDescription Material::ToCacheDescription() const
{
    auto sourcePathFor = [](const std::string& logicalPath, const std::shared_ptr<TextureAsset>& texture) {
        if (!logicalPath.empty()) {
            return logicalPath;
        }
        return texture ? texture->Meta().filePath : std::string{};
    };

    MaterialDescription desc{};
    desc.name = m_name;
    desc.diffuseColor = m_baseColorFactor;
    desc.emissiveColor = m_emissiveFactor;
    desc.alphaCutoff = m_materialData.alphaCutoff;
    desc.heightMapScale = m_materialData.heightMapScale;
    desc.geometricDisplacementMin = m_materialData.geometricDisplacementMin;
    desc.geometricDisplacementMax = m_materialData.geometricDisplacementMax;
    desc.enableGeometricDisplacement = m_materialData.geometricDisplacementEnabled != 0u;
    desc.forceDoubleSided = (m_materialData.materialFlags & MaterialFlags::MATERIAL_DOUBLE_SIDED) != 0u;
    desc.negateNormals = (m_materialData.materialFlags & MaterialFlags::MATERIAL_NEGATE_NORMALS) != 0u;
    desc.invertNormalGreen = (m_materialData.materialFlags & MaterialFlags::MATERIAL_INVERT_NORMAL_GREEN) != 0u;
    if ((m_materialData.materialFlags & MaterialFlags::MATERIAL_OPACITY_TEXTURE) != 0u) {
        desc.blendState = BlendState::BLEND_STATE_BLEND;
    } else if ((m_materialData.materialFlags & MaterialFlags::MATERIAL_ALPHA_TEST) != 0u) {
        desc.blendState = BlendState::BLEND_STATE_MASK;
    }

    desc.baseColor = TextureAndConstant{ m_baseColorTexture, 1.0f, m_baseColorChannels };
    desc.baseColor.uvSetIndex = m_baseColorUvSetIndex;
    desc.baseColor.sourcePath = sourcePathFor(m_baseColorSourcePath, m_baseColorTexture);
    desc.normal = TextureAndConstant{ m_normalTexture, 1.0f, m_normalChannels };
    desc.normal.uvSetIndex = m_normalUvSetIndex;
    desc.normal.sourcePath = sourcePathFor(m_normalSourcePath, m_normalTexture);
    desc.aoMap = TextureAndConstant{ m_aoMap, 1.0f, m_aoChannel };
    desc.aoMap.uvSetIndex = m_aoUvSetIndex;
    desc.aoMap.sourcePath = sourcePathFor(m_aoSourcePath, m_aoMap);
    desc.heightMap = TextureAndConstant{ m_heightMap, 1.0f, m_heightChannel };
    desc.heightMap.uvSetIndex = m_heightUvSetIndex;
    desc.heightMap.sourcePath = sourcePathFor(m_heightSourcePath, m_heightMap);
    desc.metallic = TextureAndConstant{ m_metallicTexture, m_metallicFactor, m_metallicChannel };
    desc.metallic.uvSetIndex = m_metallicUvSetIndex;
    desc.metallic.sourcePath = sourcePathFor(m_metallicSourcePath, m_metallicTexture);
    desc.roughness = TextureAndConstant{ m_roughnessTexture, m_roughnessFactor, m_roughnessChannel };
    desc.roughness.uvSetIndex = m_roughnessUvSetIndex;
    desc.roughness.sourcePath = sourcePathFor(m_roughnessSourcePath, m_roughnessTexture);
    desc.emissive = TextureAndConstant{ m_emissiveTexture, 1.0f, m_emissiveChannels };
    desc.emissive.uvSetIndex = m_emissiveUvSetIndex;
    desc.emissive.sourcePath = sourcePathFor(m_emissiveSourcePath, m_emissiveTexture);
    desc.opacity = TextureAndConstant{ m_opacityTexture, m_baseColorFactor.w };
    desc.opacity.uvSetIndex = m_opacityUvSetIndex;
    desc.opacity.sourcePath = sourcePathFor(m_opacitySourcePath, m_opacityTexture);
    desc.openPBR = m_openPBRMaterial;
    desc.openPBRTextures = m_openPBRTextures;
    desc.brniflyVertexAlpha = m_brniflyVertexAlpha;
    desc.brniflyZBufferWrite = m_brniflyZBufferWrite;
    desc.brniflyDecal = m_brniflyDecal;
    desc.brniflyDynamicDecal = m_brniflyDynamicDecal;
    desc.materialModel = MaterialModel::OpenPBR;
    return desc;
}

void Material::SetHeightmap(std::shared_ptr<TextureAsset> heightmap) {
    m_materialData.materialFlags |= MaterialFlags::MATERIAL_PARALLAX;
    m_heightMap = heightmap;
    auto image = heightmap ? heightmap->ImagePtr() : nullptr;
    if (!image) {
        return;
    }
    if (!heightmap->IsUsingFallbackImage() && heightmap->HasUsableImage()) {
        image->SetName("HeightMap");
        rg::memory::SetResourceUsageHint(*image, "Material textures");
    }
    m_materialData.heightMapIndex = image->GetSRVInfo(0).slot.index;
    m_materialData.heightSamplerIndex = heightmap->SamplerDescriptorIndex();
}

void Material::SetTextureScale(float scale) {
    m_materialData.textureScale = scale;
}

void Material::SetHeightmapScale(float scale) {
    m_materialData.heightMapScale = scale;
}

void Material::SetCompileFlagsID(uint32_t id) {
    m_materialData.compileFlagsID = id;
}

void Material::SetOpenPBRMaterialDataIndex(uint32_t index) {
    m_materialData.openPBRMaterialDataIndex = index;
}

void Material::SetRasterBucketIndex(uint32_t index) {
    m_materialData.rasterBuckedIndex = index;
}

std::shared_ptr<Material> Material::GetDefaultMaterial() {
    if (defaultMaterial) {
        return defaultMaterial;
    }

    MaterialDescription desc = {};
	desc.name = "DefaultMaterial";
	desc.alphaCutoff = 0.5f;
	desc.diffuseColor = { 1.0f, 1.0f, 1.0f, 1.0f };
	desc.emissiveColor = { 0.0f, 0.0f, 0.0f, 1.0f };
	desc.baseColor = { nullptr, 1.0f, { 0, 1, 2, 3 } };
	desc.metallic = { nullptr, 0.0f, { 0 } };
	desc.roughness = { nullptr, 0.5f, { 0 } };
	desc.emissive = { nullptr, 1.0f, { 0, 1, 2 } };
	desc.opacity = { nullptr, 1.0f, { 0 } };
	desc.aoMap = { nullptr, 1.0f, { 0 } };
	desc.heightMap = { nullptr, 1.0f, { 0 } };
	desc.normal = { nullptr, 1.0f, { 0, 1, 2 } };

    defaultMaterial = Material::CreateShared(desc);

    return defaultMaterial;
}

void Material::EnsureTexturesUploaded(const TextureFactory& factory) {
    const bool textureStreamingEnabled = IsMaterialTextureStreamingEnabledSetting();
    m_materialData.baseColorStreamingTextureID = 0u;
    m_materialData.normalStreamingTextureID = 0u;
    m_materialData.metallicStreamingTextureID = 0u;
    m_materialData.roughnessStreamingTextureID = 0u;
    m_materialData.emissiveStreamingTextureID = 0u;
    m_materialData.aoStreamingTextureID = 0u;
    m_materialData.heightStreamingTextureID = 0u;
    m_materialData.opacityStreamingTextureID = 0u;

    if (m_baseColorTexture) {
        m_baseColorTexture->SetGenerateMipmaps(true);
        m_baseColorTexture->EnsureUploaded(factory);
    }
    if (m_normalTexture) {
        m_normalTexture->SetGenerateMipmaps(true);
        m_normalTexture->EnsureUploaded(factory);
	}
    if (m_aoMap) {
        m_aoMap->SetGenerateMipmaps(true);
        m_aoMap->EnsureUploaded(factory);
    }
    if (m_heightMap) {
        m_heightMap->SetGenerateMipmaps(true);
        m_heightMap->EnsureUploaded(factory);
    }
    if (m_metallicTexture) {
        m_metallicTexture->SetGenerateMipmaps(true);
        m_metallicTexture->EnsureUploaded(factory);
    }
    if (m_roughnessTexture) {
        m_roughnessTexture->SetGenerateMipmaps(true);
        m_roughnessTexture->EnsureUploaded(factory);
    }
    if (m_emissiveTexture) {
        m_emissiveTexture->SetGenerateMipmaps(true);
        m_emissiveTexture->EnsureUploaded(factory);
	}
    if (m_opacityTexture) {
        m_opacityTexture->SetGenerateMipmaps(true);
        m_opacityTexture->EnsureUploaded(factory);
	}

    auto ensureOpenPBRTexture = [&](std::shared_ptr<TextureAsset> const& texture) {
        if (texture) {
            texture->SetGenerateMipmaps(true);
            texture->EnsureUploaded(factory);
        }
    };

    ensureOpenPBRTexture(m_openPBRTextures.coatColor.texture);
    ensureOpenPBRTexture(m_openPBRTextures.coatWeight.texture);
    ensureOpenPBRTexture(m_openPBRTextures.coatRoughness.texture);
    ensureOpenPBRTexture(m_openPBRTextures.fuzzColor.texture);
    ensureOpenPBRTexture(m_openPBRTextures.fuzzWeight.texture);
    ensureOpenPBRTexture(m_openPBRTextures.fuzzRoughness.texture);

    auto annotateMaterialTexture = [](const std::shared_ptr<TextureAsset>& texture, const char* name) {
        if (!texture || texture->IsUsingFallbackImage() || !texture->HasUsableImage()) {
            return;
        }

        auto image = texture->ImagePtr();
        if (!image) {
            return;
        }

        rg::memory::SetResourceUsageHint(*image, "Material textures");
        image->SetName(name);
    };

    if (m_baseColorTexture != nullptr) {
        auto image = m_baseColorTexture->ImagePtr();
        if (image) {
            m_materialData.baseColorTextureIndex = image->GetSRVInfo(0).slot.index;
            m_materialData.baseColorSamplerIndex = m_baseColorTexture->SamplerDescriptorIndex();
            m_materialData.baseColorStreamingTextureID = textureStreamingEnabled ? m_baseColorTexture->GetStreamingTextureID() : 0u;
            m_materialData.baseColorChannels = { m_baseColorChannels[0], m_baseColorChannels[1], m_baseColorChannels[2], m_baseColorChannels[3] };
            m_materialData.baseColorUvSetIndex = m_baseColorUvSetIndex;
            annotateMaterialTexture(m_baseColorTexture, "BaseColorTexture");
        }
    }
    if (m_normalTexture != nullptr) {
        auto image = m_normalTexture->ImagePtr();
        if (image) {
            m_materialData.normalTextureIndex = image->GetSRVInfo(0).slot.index;
            m_materialData.normalSamplerIndex = m_normalTexture->SamplerDescriptorIndex();
            m_materialData.normalStreamingTextureID = textureStreamingEnabled ? m_normalTexture->GetStreamingTextureID() : 0u;
            m_materialData.normalChannels = { m_normalChannels[0], m_normalChannels[1], m_normalChannels[2] };
            m_materialData.normalUvSetIndex = m_normalUvSetIndex;
            annotateMaterialTexture(m_normalTexture, "NormalTexture");
        }
    }
    if (m_aoMap != nullptr) {
        auto image = m_aoMap->ImagePtr();
        if (image) {
            m_materialData.aoMapIndex = image->GetSRVInfo(0).slot.index;
            m_materialData.aoSamplerIndex = m_aoMap->SamplerDescriptorIndex();
            m_materialData.aoStreamingTextureID = textureStreamingEnabled ? m_aoMap->GetStreamingTextureID() : 0u;
            m_materialData.aoChannel = m_aoChannel[0];
            m_materialData.aoUvSetIndex = m_aoUvSetIndex;
            annotateMaterialTexture(m_aoMap, "AOMap");
        }
    }
    if (m_heightMap != nullptr) {
        auto image = m_heightMap->ImagePtr();
        if (image) {
            m_materialData.heightMapIndex = image->GetSRVInfo(0).slot.index;
            m_materialData.heightSamplerIndex = m_heightMap->SamplerDescriptorIndex();
            m_materialData.heightStreamingTextureID = textureStreamingEnabled ? m_heightMap->GetStreamingTextureID() : 0u;
            m_materialData.heightChannel = m_heightChannel[0];
            m_materialData.heightUvSetIndex = m_heightUvSetIndex;
            annotateMaterialTexture(m_heightMap, "HeightMap");
        }
    }
    if (m_metallicTexture != nullptr) {
        auto image = m_metallicTexture->ImagePtr();
        if (image) {
            m_materialData.metallicTextureIndex = image->GetSRVInfo(0).slot.index;
            m_materialData.metallicSamplerIndex = m_metallicTexture->SamplerDescriptorIndex();
            m_materialData.metallicStreamingTextureID = textureStreamingEnabled ? m_metallicTexture->GetStreamingTextureID() : 0u;
            m_materialData.metallicChannel = m_metallicChannel[0];
            m_materialData.metallicUvSetIndex = m_metallicUvSetIndex;
            annotateMaterialTexture(m_metallicTexture, "MetallicTexture");
        }
    }
    if (m_roughnessTexture != nullptr) {
        auto image = m_roughnessTexture->ImagePtr();
        if (image) {
            m_materialData.roughnessTextureIndex = image->GetSRVInfo(0).slot.index;
            m_materialData.roughnessSamplerIndex = m_roughnessTexture->SamplerDescriptorIndex();
            m_materialData.roughnessStreamingTextureID = textureStreamingEnabled ? m_roughnessTexture->GetStreamingTextureID() : 0u;
            m_materialData.roughnessChannel = m_roughnessChannel[0];
            m_materialData.roughnessUvSetIndex = m_roughnessUvSetIndex;
            annotateMaterialTexture(m_roughnessTexture, "RoughnessTexture");
        }
    }
    if (m_metallicTexture == m_roughnessTexture && m_metallicTexture != nullptr && !m_roughnessTexture->IsUsingFallbackImage() && m_roughnessTexture->HasUsableImage()) {
        if (auto image = m_roughnessTexture->ImagePtr()) {
            image->SetName("MetallicRoughnessTexture");
        }
    }

    if (m_emissiveTexture != nullptr) {
        auto image = m_emissiveTexture->ImagePtr();
        if (image) {
            m_materialData.emissiveTextureIndex = image->GetSRVInfo(0).slot.index;
            m_materialData.emissiveSamplerIndex = m_emissiveTexture->SamplerDescriptorIndex();
            m_materialData.emissiveStreamingTextureID = textureStreamingEnabled ? m_emissiveTexture->GetStreamingTextureID() : 0u;
            m_materialData.emissiveChannels = { m_emissiveChannels[0], m_emissiveChannels[1], m_emissiveChannels[2] };
            m_materialData.emissiveUvSetIndex = m_emissiveUvSetIndex;
            annotateMaterialTexture(m_emissiveTexture, "EmissiveTexture");
        }
    }

    if (m_opacityTexture != nullptr) {
        auto image = m_opacityTexture->ImagePtr();
        if (image) {
            m_materialData.opacityTextureIndex = image->GetSRVInfo(0).slot.index;
            m_materialData.opacitySamplerIndex = m_opacityTexture->SamplerDescriptorIndex();
            m_materialData.opacityStreamingTextureID = textureStreamingEnabled ? m_opacityTexture->GetStreamingTextureID() : 0u;
            m_materialData.opacityUvSetIndex = m_opacityUvSetIndex;
            annotateMaterialTexture(m_opacityTexture, "OpacityTexture");
        }
    }

    auto nameOpenPBRTexture = [](std::shared_ptr<TextureAsset> const& texture, const char* name) {
        if (texture != nullptr && !texture->IsUsingFallbackImage() && texture->HasUsableImage()) {
            auto image = texture->ImagePtr();
            if (!image) {
                return;
            }
            rg::memory::SetResourceUsageHint(*image, "Material textures");
            image->SetName(name);
        }
    };

    nameOpenPBRTexture(m_openPBRTextures.coatColor.texture, "OpenPBRCoatColorTexture");
    nameOpenPBRTexture(m_openPBRTextures.coatWeight.texture, "OpenPBRCoatWeightTexture");
    nameOpenPBRTexture(m_openPBRTextures.coatRoughness.texture, "OpenPBRCoatRoughnessTexture");
    nameOpenPBRTexture(m_openPBRTextures.fuzzColor.texture, "OpenPBRFuzzColorTexture");
    nameOpenPBRTexture(m_openPBRTextures.fuzzWeight.texture, "OpenPBRFuzzWeightTexture");
    nameOpenPBRTexture(m_openPBRTextures.fuzzRoughness.texture, "OpenPBRFuzzRoughnessTexture");
}
