#pragma once

#include <DirectXMath.h>
#include <string>
#include <array>
#include <unordered_set>
#include <functional>
#include "Resources/Texture.h"
#include "Managers/Singletons/ResourceManager.h"
#include "Materials/BlendState.h"
#include "Render/PSOFlags.h"
#include "Render/RenderPhase.h"
#include "Materials/MaterialFlags.h"
#include "Materials/MaterialDescription.h"
#include "Materials/MaterialTextureStreaming.h"
#include "../generated/BuiltinRenderPasses.h"
#include "Materials/TechniqueDescriptor.h"
#include "Factories/TextureFactory.h"

struct TransparencyPick { bool isTransparent = false; bool masked = false; };

inline TransparencyPick PickTransparency(const MaterialDescription& d) {
    TransparencyPick t{};
    const bool hasOpacityTex = (d.opacity.texture != nullptr);
    const bool explicitBlend = (d.blendState == BlendState::BLEND_STATE_BLEND);
    const bool alphaFactor = (d.opacity.factor.Get() < 1.0f);

    t.isTransparent = hasOpacityTex || explicitBlend || alphaFactor || d.blendState == BlendState::BLEND_STATE_MASK;
    if (!t.isTransparent) return t;

    // Heuristic: prefer masked if alphaCutoff provided and we have an alpha-carrying tex
    const bool cutoff = (d.alphaCutoff > 0.0f);
    const bool hasAlphaCandidate = hasOpacityTex || (d.baseColor.texture != nullptr);
    t.masked = ((!explicitBlend) && cutoff && hasAlphaCandidate) || d.blendState == BlendState::BLEND_STATE_MASK;
    return t;
}

inline bool PickDescriptionDoubleSided(const MaterialDescription& d) {
    return d.forceDoubleSided ||
        (d.baseColor.texture && !d.baseColor.texture->Meta().alphaIsAllOpaque) ||
        d.opacity.factor.Get() < 1.0f;
}

inline TechniqueDescriptor PickTechnique(const MaterialDescription& d) { // TODO: The alpha-test/double-sided logic is wrong here
    TechniqueDescriptor tech{};
    const auto transparency = PickTransparency(d);
	tech.passes.insert(Engine::Primary::ShadowMapsPass); // All materials cast shadows
    if (transparency.isTransparent && !transparency.masked) { // OIT transparency
		tech.compileFlags |= MaterialCompileFlags::MaterialCompileBlend;
		tech.compileFlags |= MaterialCompileFlags::MaterialCompileDoubleSided;
        tech.passes.insert(Engine::Primary::OITAccumulationPass);
    }
    else {
        if (transparency.isTransparent) {
			tech.compileFlags |= MaterialCompileFlags::MaterialCompileAlphaTest;
			tech.compileFlags |= MaterialCompileFlags::MaterialCompileDoubleSided;
			tech.rasterFlags |= MaterialRasterFlags::MaterialRasterFlagsAlphaTest;
			tech.rasterFlags |= MaterialRasterFlags::MaterialRasterFlagsDoubleSided;
		}
		tech.passes.insert(Engine::Primary::GBufferPass);
    }
    if (PickDescriptionDoubleSided(d)) {
		tech.compileFlags |= MaterialCompileFlags::MaterialCompileDoubleSided;
		tech.rasterFlags |= MaterialRasterFlags::MaterialRasterFlagsDoubleSided;
	}
	if (d.baseColor.texture) {
		tech.compileFlags |= MaterialCompileFlags::MaterialCompileBaseColorTexture;
	}
	if (d.normal.texture) {
		tech.compileFlags |= MaterialCompileFlags::MaterialCompileNormalMap;
	}
	if (d.aoMap.texture) {
		tech.compileFlags |= MaterialCompileFlags::MaterialCompileAOTexture;
	}
    if (d.metallic.texture) {
        tech.compileFlags |= MaterialCompileFlags::MaterialCompileMetallicTexture;
    }
    if (d.roughness.texture) {
        tech.compileFlags |= MaterialCompileFlags::MaterialCompileRoughnessTexture;
	}
	if (d.emissive.texture) {
		tech.compileFlags |= MaterialCompileFlags::MaterialCompileEmissiveTexture;
	}
    if (d.opacity.texture) {
        tech.compileFlags |= MaterialCompileFlags::MaterialCompileOpacityTexture;
    }
	if (d.heightMap.texture || (d.heightMapFromBaseColorAlpha && d.baseColor.texture)) {
        if (d.heightMapFromBaseColorAlpha && d.baseColor.texture) {
            tech.compileFlags |= MaterialCompileFlags::MaterialCompileHeightFromBaseAlpha;
        }
        if (d.enableGeometricDisplacement) {
            tech.compileFlags |= MaterialCompileFlags::MaterialCompileGeometricDisplacement;
            tech.rasterFlags |= MaterialRasterFlags::MaterialRasterFlagsGeometricDisplacement;
        }
        else {
            tech.compileFlags |= MaterialCompileFlags::MaterialCompileParallax;
        }
	}
    if (d.openPBRTextures.coatColor.texture) {
        tech.compileFlags |= MaterialCompileFlags::MaterialCompileOpenPBRCoatColorTexture;
    }
    if (d.openPBRTextures.coatWeight.texture) {
        tech.compileFlags |= MaterialCompileFlags::MaterialCompileOpenPBRCoatWeightTexture;
    }
    if (d.openPBRTextures.coatRoughness.texture) {
        tech.compileFlags |= MaterialCompileFlags::MaterialCompileOpenPBRCoatRoughnessTexture;
    }
    if (d.openPBRTextures.fuzzColor.texture) {
        tech.compileFlags |= MaterialCompileFlags::MaterialCompileOpenPBRFuzzColorTexture;
    }
    if (d.openPBRTextures.fuzzWeight.texture) {
        tech.compileFlags |= MaterialCompileFlags::MaterialCompileOpenPBRFuzzWeightTexture;
    }
    if (d.openPBRTextures.fuzzRoughness.texture) {
        tech.compileFlags |= MaterialCompileFlags::MaterialCompileOpenPBRFuzzRoughnessTexture;
    }
    if (IsMaterialTextureStreamingEnabledSetting() &&
        (d.baseColor.texture ||
         d.normal.texture ||
         d.aoMap.texture ||
         d.heightMap.texture ||
         (d.heightMapFromBaseColorAlpha && d.baseColor.texture) ||
         d.metallic.texture ||
         d.roughness.texture ||
         d.emissive.texture ||
         d.opacity.texture ||
         d.openPBRTextures.coatColor.texture ||
         d.openPBRTextures.coatWeight.texture ||
         d.openPBRTextures.coatRoughness.texture ||
         d.openPBRTextures.fuzzColor.texture ||
         d.openPBRTextures.fuzzWeight.texture ||
         d.openPBRTextures.fuzzRoughness.texture)) {
        tech.compileFlags |= MaterialCompileFlags::MaterialCompileTextureStreaming;
    }

    return tech;
}

class Material {
public:
    static std::shared_ptr<Material> CreateShared(const MaterialDescription& desc) {
        uint32_t materialFlags = 0;
        uint32_t psoFlags = 0;
        OpenPBRMaterialParameters canonicalOpenPBR = BuildCanonicalOpenPBRMaterial(desc);
        const auto transparency = PickTransparency(desc);
        materialFlags |= MaterialFlags::MATERIAL_PBR; // TODO: Non-PBR materials
        BlendState blendState = BlendState::BLEND_STATE_OPAQUE; // Default blend state
        if (transparency.masked) {
            materialFlags |= MaterialFlags::MATERIAL_ALPHA_TEST;
        }
        if (desc.baseColor.texture) {
            if (!desc.baseColor.texture->Meta().alphaIsAllOpaque) {
                materialFlags |= MaterialFlags::MATERIAL_DOUBLE_SIDED;
                blendState = BlendState::BLEND_STATE_MASK; // Use mask blending for alpha-tested materials
            }
            materialFlags |= MaterialFlags::MATERIAL_BASE_COLOR_TEXTURE | MaterialFlags::MATERIAL_TEXTURED;
        }
        if (desc.metallic.texture) {
            materialFlags |= MaterialFlags::MATERIAL_PBR | MaterialFlags::MATERIAL_METALLIC_TEXTURE | MaterialFlags::MATERIAL_TEXTURED;
        }
        if (desc.roughness.texture) {
            materialFlags |= MaterialFlags::MATERIAL_PBR | MaterialFlags::MATERIAL_ROUGHNESS_TEXTURE | MaterialFlags::MATERIAL_TEXTURED;
        }
        if (desc.emissive.texture) {
            materialFlags |= MaterialFlags::MATERIAL_EMISSIVE_TEXTURE | MaterialFlags::MATERIAL_TEXTURED;
        }
        if (desc.normal.texture) {
            materialFlags |= MaterialFlags::MATERIAL_NORMAL_MAP | MaterialFlags::MATERIAL_TEXTURED;
        }
        if (desc.heightMapFromBaseColorAlpha && desc.baseColor.texture) {
            materialFlags |= MaterialFlags::MATERIAL_PARALLAX | MaterialFlags::MATERIAL_TEXTURED | MaterialFlags::MATERIAL_HEIGHT_FROM_BASE_ALPHA;
        }
        if (desc.heightMap.texture) {
            materialFlags |= MaterialFlags::MATERIAL_PARALLAX | MaterialFlags::MATERIAL_TEXTURED;
        }
        if (desc.enableGeometricDisplacement && (desc.heightMap.texture || (desc.heightMapFromBaseColorAlpha && desc.baseColor.texture))) {
            materialFlags |= MaterialFlags::MATERIAL_GEOMETRIC_DISPLACEMENT;
        }
        auto diffuseColor = desc.diffuseColor;
        auto emissiveColor = desc.emissiveColor;
        if (desc.opacity.texture) { // TODO: How can we tell if this should be used as a mask or as a blend?
            materialFlags |= MaterialFlags::MATERIAL_OPACITY_TEXTURE | MaterialFlags::MATERIAL_TEXTURED;
            blendState = BlendState::BLEND_STATE_BLEND; // Use blend state for opacity
        }
        if (desc.opacity.factor.Get() < 1.0f) {
            materialFlags |= MaterialFlags::MATERIAL_DOUBLE_SIDED;
            diffuseColor.w = desc.opacity.factor.Get(); // Use opacity factor as alpha
            blendState = BlendState::BLEND_STATE_BLEND; // Use blend state for opacity
        }
        if (desc.forceDoubleSided) {
            materialFlags |= MaterialFlags::MATERIAL_DOUBLE_SIDED;
        }
        if (desc.openPBRTextures.coatColor.texture ||
            desc.openPBRTextures.coatWeight.texture ||
            desc.openPBRTextures.coatRoughness.texture ||
            desc.openPBRTextures.fuzzColor.texture ||
            desc.openPBRTextures.fuzzWeight.texture ||
            desc.openPBRTextures.fuzzRoughness.texture) {
            materialFlags |= MaterialFlags::MATERIAL_TEXTURED;
        }
        if (desc.negateNormals) {
            materialFlags |= MaterialFlags::MATERIAL_NEGATE_NORMALS;
        }
        if (desc.invertNormalGreen) {
            materialFlags |= MaterialFlags::MATERIAL_INVERT_NORMAL_GREEN;
        }
        const float emissiveScalar = desc.emissive.factor.Get();
        emissiveColor.x *= emissiveScalar;
        emissiveColor.y *= emissiveScalar;
        emissiveColor.z *= emissiveScalar;
		TechniqueDescriptor technique = PickTechnique(desc);

        auto material = CreateShared(
            desc.name,
            static_cast<MaterialFlags>(materialFlags),
            static_cast<PSOFlags>(psoFlags),
            desc.baseColor.texture,
            desc.normal.texture,
            desc.aoMap.texture,
            desc.heightMapFromBaseColorAlpha && !desc.heightMap.texture ? desc.baseColor.texture : desc.heightMap.texture,
            desc.metallic.texture,
            desc.roughness.texture,
            desc.emissive.texture,
            desc.opacity.texture,
            desc.metallic.factor.Get(),
            desc.roughness.factor.Get(),
            diffuseColor,
            emissiveColor,
            desc.baseColor.channels,
            desc.normal.channels,
            desc.aoMap.channels,
            desc.heightMapFromBaseColorAlpha && !desc.heightMap.texture ? std::vector<uint32_t>{ 3u } : desc.heightMap.channels,
            desc.metallic.channels,
            desc.roughness.channels,
            desc.emissive.channels,
            desc.baseColor.uvSetIndex,
            desc.normal.uvSetIndex,
            desc.aoMap.uvSetIndex,
            desc.heightMapFromBaseColorAlpha && !desc.heightMap.texture ? desc.baseColor.uvSetIndex : desc.heightMap.uvSetIndex,
            desc.metallic.uvSetIndex,
            desc.roughness.uvSetIndex,
            desc.emissive.uvSetIndex,
            desc.opacity.uvSetIndex,
			desc.heightMapScale,
			desc.geometricDisplacementMin,
			desc.geometricDisplacementMax,
			desc.enableGeometricDisplacement,
            technique,
            canonicalOpenPBR,
            desc.openPBRTextures,
            desc.alphaCutoff,
            desc.brniflyVertexAlpha,
            desc.brniflyZBufferWrite,
            desc.brniflyDecal,
            desc.brniflyDynamicDecal,
            desc.brniflyModelSpaceNormals
        );
        material->m_materialModel = desc.materialModel;
        material->SetLogicalTextureSourcePaths(desc);
        return material;
    }
    ~Material();

    void SetHeightmap(std::shared_ptr<TextureAsset> heightmap);
    void SetTextureScale(float scale);
    void SetHeightmapScale(float scale);
    void SetCompileFlagsID(uint32_t id);
    void SetOpenPBRMaterialDataIndex(uint32_t index);
    void SetTerrainSetIndex(uint32_t index);
    void SetRasterBucketIndex(uint32_t index);
    PSOFlags GetPSOFlags() const { return m_psoFlags; }
    MaterialFlags GetMaterialFlags() const { return static_cast<MaterialFlags>(m_materialData.materialFlags); }
    static std::shared_ptr<Material> GetDefaultMaterial();
    TechniqueDescriptor const& Technique() const { return m_technique; }
    OpenPBRMaterialParameters const& GetOpenPBRMaterial() const { return m_openPBRMaterial; }
    OpenPBRTextureBindings const& GetOpenPBRTextures() const { return m_openPBRTextures; }
    bool BrniflyVertexAlpha() const { return m_brniflyVertexAlpha; }
    bool BrniflyZBufferWrite() const { return m_brniflyZBufferWrite; }
    bool BrniflyDecal() const { return m_brniflyDecal; }
    bool BrniflyDynamicDecal() const { return m_brniflyDynamicDecal; }
    bool BrniflyModelSpaceNormals() const { return m_brniflyModelSpaceNormals; }
    MaterialDescription ToCacheDescription() const;
    void SetLogicalTextureSourcePaths(const MaterialDescription& desc);
    static void DestroyDefaultMaterial() {
        defaultMaterial.reset();
    }
    uint32_t GetMaterialID() const { return m_materialID; }
    PerMaterialCB const& GetData() const { return m_materialData; }
    void EnsureTexturesUploaded(const TextureFactory& factory);
    void EnsureTexturesUploaded(const TextureFactory& factory, TextureUploadAdvanceMode mode);
    void ForEachReferencedTexture(const std::function<void(const std::shared_ptr<TextureAsset>&)>& visitor) const;
private:
	inline static std::atomic<uint32_t> globalMaterialCount;
	const uint32_t m_materialID = globalMaterialCount.fetch_add(1, std::memory_order_relaxed);

    std::string m_name;
    std::shared_ptr<TextureAsset> m_baseColorTexture;
    std::shared_ptr<TextureAsset> m_normalTexture;
    std::shared_ptr<TextureAsset> m_aoMap;
    std::shared_ptr<TextureAsset> m_heightMap;
    std::shared_ptr<TextureAsset> m_roughnessTexture;
    std::shared_ptr<TextureAsset> m_metallicTexture;
    std::shared_ptr<TextureAsset> m_emissiveTexture;
    std::shared_ptr<TextureAsset> m_opacityTexture;
    std::string m_baseColorSourcePath;
    std::string m_normalSourcePath;
    std::string m_aoSourcePath;
    std::string m_heightSourcePath;
    std::string m_roughnessSourcePath;
    std::string m_metallicSourcePath;
    std::string m_emissiveSourcePath;
    std::string m_opacitySourcePath;
    std::vector<uint32_t> m_baseColorChannels;
    std::vector<uint32_t> m_normalChannels;
    std::vector<uint32_t> m_aoChannel;
    std::vector<uint32_t> m_heightChannel;
    std::vector<uint32_t> m_metallicChannel;
    std::vector<uint32_t> m_roughnessChannel;
    std::vector<uint32_t> m_emissiveChannels;
    uint32_t m_baseColorUvSetIndex = 0;
    uint32_t m_normalUvSetIndex = 0;
    uint32_t m_aoUvSetIndex = 0;
    uint32_t m_heightUvSetIndex = 0;
    uint32_t m_metallicUvSetIndex = 0;
    uint32_t m_roughnessUvSetIndex = 0;
    uint32_t m_emissiveUvSetIndex = 0;
    uint32_t m_opacityUvSetIndex = 0;
    float m_metallicFactor;
    float m_roughnessFactor;
    DirectX::XMFLOAT4 m_baseColorFactor;
    DirectX::XMFLOAT4 m_emissiveFactor;
    PerMaterialCB m_materialData = { 0 };
    PSOFlags m_psoFlags;
    TechniqueDescriptor m_technique;
    MaterialModel m_materialModel = MaterialModel::LegacyPreviewSurface;
    OpenPBRMaterialParameters m_openPBRMaterial = {};
    OpenPBRTextureBindings m_openPBRTextures = {};
    bool m_brniflyVertexAlpha = false;
    bool m_brniflyZBufferWrite = true;
    bool m_brniflyDecal = false;
    bool m_brniflyDynamicDecal = false;
    bool m_brniflyModelSpaceNormals = false;

    Material(const std::string& name,
        MaterialFlags materialFlags, PSOFlags psoFlags);

    Material(const std::string& name,
        MaterialFlags materialFlags, PSOFlags psoFlags,
        std::shared_ptr<TextureAsset> baseColorTexture,
        std::shared_ptr<TextureAsset> normalTexture,
        std::shared_ptr<TextureAsset> aoMap,
        std::shared_ptr<TextureAsset> heightMap,
        std::shared_ptr<TextureAsset> metallicTexture,
        std::shared_ptr<TextureAsset> m_roughnessTexture,
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
        bool brniflyModelSpaceNormals);

    static std::shared_ptr<Material> CreateShared(const std::string& name,
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
        bool brniflyModelSpaceNormals) {
        return std::shared_ptr<Material>(new Material(name, materialFlags, psoFlags,
            baseColorTexture, normalTexture, aoMap, heightMap,
            metallicTexture, roughnessTexture, emissiveTexture, opacityTexture,
            metallicFactor, roughnessFactor, baseColorFactor, emissiveFactor,
            baseColorChannels, normalChannels, aoChannel, heightChannel,
            metallicChannel, roughnessChannel, emissiveChannels,
            baseColorUvSetIndex, normalUvSetIndex, aoUvSetIndex, heightUvSetIndex,
			metallicUvSetIndex, roughnessUvSetIndex, emissiveUvSetIndex, opacityUvSetIndex,
			heightMapScale, geometricDisplacementMin, geometricDisplacementMax, geometricDisplacementEnabled,
            technique,
            openPBRMaterial,
            openPBRTextures,
            alphaCutoff,
            brniflyVertexAlpha,
            brniflyZBufferWrite,
            brniflyDecal,
            brniflyDynamicDecal,
            brniflyModelSpaceNormals));
    }

    inline static std::shared_ptr<Material> defaultMaterial;
};
