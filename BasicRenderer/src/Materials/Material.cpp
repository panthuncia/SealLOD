#include "Materials/Material.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <spdlog/spdlog.h>
#include "Render/PSOFlags.h"
#include "Utilities/Utilities.h"
#include "Materials/MaterialFlags.h"
#include "Resources/PixelBuffer.h"
#include "Render/MemoryIntrospectionAPI.h"

namespace {
    bool GltfMaterialDebugLoggingEnabled() {
        static const bool enabled = [] {
            char* value = nullptr;
            size_t valueLength = 0;
            const bool isSet =
                _dupenv_s(&value, &valueLength, "SARP_GLTF_MATERIAL_LOG") == 0 &&
                value != nullptr &&
                value[0] != '\0' &&
                value[0] != '0';
            std::free(value);
            return isSet;
        }();
        return enabled;
    }

    bool NormalTextureNeedsReconstructedZ(rhi::Format format) {
        switch (format) {
        case rhi::Format::BC5_UNorm:
        case rhi::Format::BC5_SNorm:
        case rhi::Format::R8G8_UNorm:
        case rhi::Format::R8G8_SNorm:
            return true;
        default:
            return false;
        }
    }

    bool IsDxt5Format(rhi::Format format) {
        switch (format) {
        case rhi::Format::BC3_Typeless:
        case rhi::Format::BC3_UNorm:
        case rhi::Format::BC3_UNorm_sRGB:
            return true;
        default:
            return false;
        }
    }

    const char* NormalTextureFormatName(rhi::Format format) {
        switch (format) {
        case rhi::Format::BC3_Typeless: return "BC3_Typeless/DXT5";
        case rhi::Format::BC3_UNorm: return "BC3_UNorm/DXT5";
        case rhi::Format::BC3_UNorm_sRGB: return "BC3_UNorm_sRGB/DXT5";
        case rhi::Format::BC5_UNorm: return "BC5_UNorm";
        case rhi::Format::BC5_SNorm: return "BC5_SNorm";
        case rhi::Format::R8G8_UNorm: return "R8G8_UNorm";
        case rhi::Format::R8G8_SNorm: return "R8G8_SNorm";
        default: return "other";
        }
    }

    bool HasReconstructedZChannels(const std::vector<uint32_t>& channels) {
        return channels.size() >= 3u && channels[0] == 0u && channels[1] == 1u && channels[2] == 4u;
    }

    bool RequestsRgbNormalChannels(const std::vector<uint32_t>& channels) {
        return channels.size() >= 3u && channels[0] == 0u && channels[1] == 1u && channels[2] == 2u;
    }

    uint32_t FirstChannelOrDefault(const std::vector<uint32_t>& channels, uint32_t fallback) {
        return channels.empty() ? fallback : channels[0];
    }

    bool HeightAtlasStreamingEnabledForMaterialUpload() {
        char* value = nullptr;
        size_t valueSize = 0;
        if (_dupenv_s(&value, &valueSize, "SARP_HEIGHT_ATLAS_STREAMING") != 0 || value == nullptr || *value == '\0') {
            if (value) {
                std::free(value);
            }
            return true;
        }
        std::string text(value);
        std::free(value);
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return text != "0" && text != "false" && text != "no" && text != "off";
    }

    DirectX::XMUINT3 RgbChannelsOrDefault(const std::vector<uint32_t>& channels) {
        return DirectX::XMUINT3{
            channels.size() > 0u ? channels[0] : 0u,
            channels.size() > 1u ? channels[1] : 1u,
            channels.size() > 2u ? channels[2] : 2u
        };
    }

    DirectX::XMUINT4 RgbaChannelsOrDefault(const std::vector<uint32_t>& channels) {
        return DirectX::XMUINT4{
            channels.size() > 0u ? channels[0] : 0u,
            channels.size() > 1u ? channels[1] : 1u,
            channels.size() > 2u ? channels[2] : 2u,
            channels.size() > 3u ? channels[3] : 3u
        };
    }
}

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
    bool brniflyDynamicDecal,
    bool brniflyModelSpaceNormals)
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
    m_brniflyDynamicDecal(brniflyDynamicDecal),
    m_brniflyModelSpaceNormals(brniflyModelSpaceNormals)
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
    m_heightSourcePath = desc.heightMapFromBaseColorAlpha ? desc.baseColor.sourcePath : desc.heightMap.sourcePath;
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
    desc.enableGeometricDisplacement =
        (m_materialData.materialFlags & MaterialFlags::MATERIAL_GEOMETRIC_DISPLACEMENT) != 0u;
    desc.geometricDisplacementOptIn = m_geometricDisplacementOptIn;
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
    desc.heightMapFromBaseColorAlpha = (m_materialData.materialFlags & MaterialFlags::MATERIAL_HEIGHT_FROM_BASE_ALPHA) != 0u;
    if (desc.heightMapFromBaseColorAlpha) {
        desc.heightMap = {};
        desc.heightMap.channels = { 3u };
        desc.heightMap.uvSetIndex = m_baseColorUvSetIndex;
        desc.heightMap.sourcePath = {};
    } else {
        desc.heightMap = TextureAndConstant{ m_heightMap, 1.0f, m_heightChannel };
        desc.heightMap.uvSetIndex = m_heightUvSetIndex;
        desc.heightMap.sourcePath = sourcePathFor(m_heightSourcePath, m_heightMap);
        if (static_cast<ObjectSurfaceSamplingMode>(m_materialData.objectSurfaceSamplingMode) == ObjectSurfaceSamplingMode::AtlasBakedHeight) {
            desc.heightMap.uvSetName = "__object_reyes_atlas_height";
        }
    }
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
    desc.glintEnabled = m_materialData.glintEnabled != 0u;
    desc.glintParameters = m_materialData.glintParameters;
    desc.objectSurfaceSamplingMode = static_cast<ObjectSurfaceSamplingMode>(m_materialData.objectSurfaceSamplingMode);
    desc.objectSurfaceUseTriplanarProjection =
        (m_materialData.materialFlags & MaterialFlags::MATERIAL_OBJECT_TRIPLANAR_STOCHASTIC) != 0u;
    desc.objectSurfaceUseTripleTapStochastic = desc.objectSurfaceUseTriplanarProjection;
    desc.objectSurfaceTexelDensity = m_materialData.objectSurfaceTexelDensity;
    desc.staticTextureOverrideSourceName = m_staticTextureOverrideSourceName;
    desc.brniflyVertexAlpha = m_brniflyVertexAlpha;
    desc.brniflyZBufferWrite = m_brniflyZBufferWrite;
    desc.brniflyDecal = m_brniflyDecal;
    desc.brniflyDynamicDecal = m_brniflyDynamicDecal;
    desc.brniflyModelSpaceNormals = m_brniflyModelSpaceNormals;
    desc.materialModel = m_materialModel;
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

void Material::MergeReyesUvDensity(DirectX::XMFLOAT2 density) {
    if (std::isfinite(density.x) && density.x > 0.0f) {
        m_materialData.reyesUvDensity.x = std::max(m_materialData.reyesUvDensity.x, density.x);
    }
    if (std::isfinite(density.y) && density.y > 0.0f) {
        m_materialData.reyesUvDensity.y = std::max(m_materialData.reyesUvDensity.y, density.y);
    }
}

void Material::SetTerrainSetIndex(uint32_t index, bool terrainParallaxCapable) {
    m_materialData.materialFlags |= MaterialFlags::MATERIAL_TERRAIN;
    m_technique.compileFlags |= MaterialCompileFlags::MaterialCompileTerrain;
    m_materialData.terrainSetIndex = index;
    m_materialData.reyesUvDensity.x = std::max(m_materialData.reyesUvDensity.x, 1.0f);
    m_materialData.reyesUvDensity.y = std::max(m_materialData.reyesUvDensity.y, 1.0f);
    if (m_materialData.geometricDisplacementEnabled != 0u) {
        m_materialData.materialFlags &= ~MaterialFlags::MATERIAL_PARALLAX;
        m_materialData.materialFlags |= MaterialFlags::MATERIAL_GEOMETRIC_DISPLACEMENT;
        m_technique.compileFlags |= MaterialCompileFlags::MaterialCompileGeometricDisplacement;
        m_technique.compileFlags = static_cast<MaterialCompileFlags>(
            static_cast<uint64_t>(m_technique.compileFlags) &
            ~static_cast<uint64_t>(MaterialCompileFlags::MaterialCompileParallax));
        m_technique.rasterFlags |= MaterialRasterFlags::MaterialRasterFlagsGeometricDisplacement;
    }
    else if (terrainParallaxCapable) {
        m_materialData.materialFlags |= MaterialFlags::MATERIAL_PARALLAX | MaterialFlags::MATERIAL_TEXTURED;
        m_materialData.materialFlags &= ~MaterialFlags::MATERIAL_GEOMETRIC_DISPLACEMENT;
        m_technique.compileFlags |= MaterialCompileFlags::MaterialCompileParallax;
        m_technique.compileFlags = static_cast<MaterialCompileFlags>(
            static_cast<uint64_t>(m_technique.compileFlags) &
            ~static_cast<uint64_t>(MaterialCompileFlags::MaterialCompileGeometricDisplacement));
        m_technique.rasterFlags = static_cast<MaterialRasterFlags>(
            static_cast<uint32_t>(m_technique.rasterFlags) &
            ~static_cast<uint32_t>(MaterialRasterFlags::MaterialRasterFlagsGeometricDisplacement));
    }
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
    EnsureTexturesUploaded(factory, TextureUploadAdvanceMode::AllowBlockingFallback);
}

void Material::EnsureTexturesUploaded(const TextureFactory& factory, TextureUploadAdvanceMode mode) {
    if (m_baseColorTexture) {
        m_baseColorTexture->SetGenerateMipmaps(true);
        m_baseColorTexture->EnsureUploaded(factory, mode);
    }
    if (m_normalTexture) {
        m_normalTexture->SetGenerateMipmaps(true);
        m_normalTexture->EnsureUploaded(factory, mode);
	}
    if (m_aoMap) {
        m_aoMap->SetGenerateMipmaps(true);
        m_aoMap->EnsureUploaded(factory, mode);
    }
    if (m_heightMap) {
        const bool hasExplicitObjectReyesAtlasMips =
            m_materialData.objectSurfaceSamplingMode == static_cast<std::uint32_t>(ObjectSurfaceSamplingMode::AtlasBakedHeight);
        m_heightMap->SetGenerateMipmaps(!hasExplicitObjectReyesAtlasMips);
        const bool streamObjectReyesAtlasHeight =
            hasExplicitObjectReyesAtlasMips && HeightAtlasStreamingEnabledForMaterialUpload();
        if (!streamObjectReyesAtlasHeight) {
            const auto heightUploadMode =
                (mode == TextureUploadAdvanceMode::NonBlocking &&
                 m_materialData.geometricDisplacementEnabled != 0u)
                ? TextureUploadAdvanceMode::AllowBlockingFallback
                : mode;
            m_heightMap->EnsureUploaded(factory, heightUploadMode);
        }
    }
    if (m_metallicTexture) {
        m_metallicTexture->SetGenerateMipmaps(true);
        m_metallicTexture->EnsureUploaded(factory, mode);
    }
    if (m_roughnessTexture) {
        m_roughnessTexture->SetGenerateMipmaps(true);
        m_roughnessTexture->EnsureUploaded(factory, mode);
    }
    if (m_emissiveTexture) {
        m_emissiveTexture->SetGenerateMipmaps(true);
        m_emissiveTexture->EnsureUploaded(factory, mode);
	}
    if (m_opacityTexture) {
        m_opacityTexture->SetGenerateMipmaps(true);
        m_opacityTexture->EnsureUploaded(factory, mode);
	}

    auto ensureOpenPBRTexture = [&](std::shared_ptr<TextureAsset> const& texture) {
        if (texture) {
            texture->SetGenerateMipmaps(true);
            texture->EnsureUploaded(factory, mode);
        }
    };

    ensureOpenPBRTexture(m_openPBRTextures.coatColor.texture);
    ensureOpenPBRTexture(m_openPBRTextures.coatWeight.texture);
    ensureOpenPBRTexture(m_openPBRTextures.coatRoughness.texture);
    ensureOpenPBRTexture(m_openPBRTextures.fuzzColor.texture);
    ensureOpenPBRTexture(m_openPBRTextures.fuzzWeight.texture);
    ensureOpenPBRTexture(m_openPBRTextures.fuzzRoughness.texture);

    RefreshTextureBindings();
}

void Material::RefreshTextureBindings() {
    const bool textureStreamingEnabled = IsMaterialTextureStreamingEnabledSetting();
    m_materialData.baseColorStreamingTextureID = 0u;
    m_materialData.normalStreamingTextureID = 0u;
    m_materialData.metallicStreamingTextureID = 0u;
    m_materialData.roughnessStreamingTextureID = 0u;
    m_materialData.emissiveStreamingTextureID = 0u;
    m_materialData.aoStreamingTextureID = 0u;
    m_materialData.heightStreamingTextureID = 0u;
    m_materialData.opacityStreamingTextureID = 0u;
    m_materialData.heightMapIndex = 0u;
    m_materialData.heightSamplerIndex = 0u;

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
            m_materialData.baseColorChannels = RgbaChannelsOrDefault(m_baseColorChannels);
            m_materialData.baseColorUvSetIndex = m_baseColorUvSetIndex;
            annotateMaterialTexture(m_baseColorTexture, "BaseColorTexture");
        }
    }
    if (m_normalTexture != nullptr) {
        auto image = m_normalTexture->ImagePtr();
        if (image) {
            const auto normalFormat = m_normalTexture->Format();
            if (NormalTextureNeedsReconstructedZ(normalFormat)) {
                if (!HasReconstructedZChannels(m_normalChannels)) {
                    spdlog::warn(
                        "Material '{}' normal texture '{}' uses {} but requested channels ({},{},{}); forcing reconstructed-Z channels (0,1,4).",
                        m_name,
                        !m_normalSourcePath.empty() ? m_normalSourcePath : m_normalTexture->Meta().filePath,
                        NormalTextureFormatName(normalFormat),
                        m_normalChannels.size() > 0u ? m_normalChannels[0] : 0u,
                        m_normalChannels.size() > 1u ? m_normalChannels[1] : 0u,
                        m_normalChannels.size() > 2u ? m_normalChannels[2] : 0u);
                }
                m_normalChannels = { 0u, 1u, 4u };
            }
            else if (IsDxt5Format(normalFormat) && RequestsRgbNormalChannels(m_normalChannels)) {
                spdlog::warn(
                    "Material '{}' normal texture '{}' uses {} but requested RGB channels (0,1,2); DXT5 normal maps usually need an explicit packed-channel convention.",
                    m_name,
                    !m_normalSourcePath.empty() ? m_normalSourcePath : m_normalTexture->Meta().filePath,
                    NormalTextureFormatName(normalFormat));
            }
            m_materialData.normalTextureIndex = image->GetSRVInfo(0).slot.index;
            m_materialData.normalSamplerIndex = m_normalTexture->SamplerDescriptorIndex();
            m_materialData.normalStreamingTextureID = textureStreamingEnabled ? m_normalTexture->GetStreamingTextureID() : 0u;
            m_materialData.normalChannels = RgbChannelsOrDefault(m_normalChannels);
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
            m_materialData.aoChannel = FirstChannelOrDefault(m_aoChannel, 0u);
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
            m_materialData.heightChannel = FirstChannelOrDefault(m_heightChannel, 0u);
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
            m_materialData.metallicChannel = FirstChannelOrDefault(m_metallicChannel, 0u);
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
            m_materialData.roughnessChannel = FirstChannelOrDefault(m_roughnessChannel, 0u);
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
            m_materialData.emissiveChannels = RgbChannelsOrDefault(m_emissiveChannels);
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

    if (GltfMaterialDebugLoggingEnabled()) {
        static std::atomic<uint32_t> loggedUploads{ 0u };
        const uint32_t logIndex = loggedUploads.fetch_add(1u, std::memory_order_relaxed);
        if (logIndex < 512u) {
			auto baseImage = m_baseColorTexture ? m_baseColorTexture->ImagePtr() : nullptr;
			const auto basePending = m_baseColorTexture
				? m_baseColorTexture->GetPendingDebugInfo()
				: TexturePendingDebugInfo{};
            spdlog::info(
                "SARPDBG material upload id={} name='{}' flags=0x{:x} baseIndex={} baseSampler={} baseFallback={} baseUsable={} baseBackingValid={} baseStreamingID={} baseBindingRevision={} basePending={} basePlaceholder={} baseDirectStorage={} normalIndex={} normalFallback={} normalUsable={} mrIndex=({}, {}) aoIndex={} opacityIndex={} uv(base,normal,mr,ao)=({}, {}, {}, {}) channels(base,normal,mr,ao)=({}, {}, {}, {}; {}, {}, {}; {}, {}; {})",
                m_materialID,
                m_name,
                m_materialData.materialFlags,
                m_materialData.baseColorTextureIndex,
                m_materialData.baseColorSamplerIndex,
                m_baseColorTexture ? m_baseColorTexture->IsUsingFallbackImage() : false,
                m_baseColorTexture ? m_baseColorTexture->HasUsableImage() : false,
				baseImage ? baseImage->HasValidBackingResource() : false,
				m_materialData.baseColorStreamingTextureID,
				basePending.bindingRevision,
				m_baseColorTexture ? m_baseColorTexture->HasPendingUploadWork() : false,
				basePending.hasPlaceholder,
				basePending.directStorageState,
                m_materialData.normalTextureIndex,
                m_normalTexture ? m_normalTexture->IsUsingFallbackImage() : false,
                m_normalTexture ? m_normalTexture->HasUsableImage() : false,
                m_materialData.metallicTextureIndex,
                m_materialData.roughnessTextureIndex,
                m_materialData.aoMapIndex,
                m_materialData.opacityTextureIndex,
                m_materialData.baseColorUvSetIndex,
                m_materialData.normalUvSetIndex,
                m_materialData.metallicUvSetIndex,
                m_materialData.aoUvSetIndex,
                m_materialData.baseColorChannels.x,
                m_materialData.baseColorChannels.y,
                m_materialData.baseColorChannels.z,
                m_materialData.baseColorChannels.w,
                m_materialData.normalChannels.x,
                m_materialData.normalChannels.y,
                m_materialData.normalChannels.z,
                m_materialData.metallicChannel,
                m_materialData.roughnessChannel,
                m_materialData.aoChannel);
        }
    }
}
