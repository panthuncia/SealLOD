#pragma once

#include <algorithm>
#include <cstdint>
#include <DirectXMath.h>
#include <vector>
#include <memory>
#include <string>
#include <cctype>

#include "Materials/BlendState.h"
#include "Utilities/DefaultOptional.h"

class TextureAsset;

enum class MaterialModel : uint32_t {
    LegacyPreviewSurface = 0,
    OpenPBR = 1,
};

struct TextureAndConstant {
	TextureAndConstant() = default;
    TextureAndConstant(std::shared_ptr<TextureAsset> tex, float f) : texture(tex), factor(f) {
    }
    TextureAndConstant(std::shared_ptr<TextureAsset> tex, float f, std::vector<uint32_t> ch) : texture(tex), factor(f), channels(std::move(ch)) {
	}
    std::shared_ptr<TextureAsset> texture;  // null if none
    DefaultedOptional<float> factor = DefaultedOptional<float>(1.0f);
	std::vector<uint32_t> channels; // For swizzling texture channels, e.g. R, G, B, A
	uint32_t uvSetIndex = 0;
	std::string uvSetName;
	std::string sourcePath;
};

struct OpenPBRTextureBindings {
    TextureAndConstant coatColor = {};
    TextureAndConstant coatWeight = {};
    TextureAndConstant coatRoughness = {};
    TextureAndConstant fuzzColor = {};
    TextureAndConstant fuzzWeight = {};
    TextureAndConstant fuzzRoughness = {};
};

struct OpenPBRMaterialParameters {
    float baseWeight = 1.0f;
    DirectX::XMFLOAT3 baseColor = { 0.8f, 0.8f, 0.8f };
    float baseDiffuseRoughness = 0.0f;
    float baseMetalness = 0.0f;

    float subsurfaceWeight = 0.0f;
    DirectX::XMFLOAT3 subsurfaceColor = { 0.8f, 0.8f, 0.8f };
    float subsurfaceRadius = 1.0f;
    DirectX::XMFLOAT3 subsurfaceRadiusScale = { 1.0f, 0.5f, 0.25f };
    float subsurfaceScatterAnisotropy = 0.0f;

    float specularWeight = 1.0f;
    DirectX::XMFLOAT3 specularColor = { 1.0f, 1.0f, 1.0f };
    float specularRoughness = 0.3f;
    float specularRoughnessAnisotropy = 0.0f;
    float specularIor = 1.5f;
    DirectX::XMFLOAT2 specularAnisotropyRotationCosSin = { 1.0f, 0.0f };

    float coatWeight = 0.0f;
    DirectX::XMFLOAT3 coatColor = { 1.0f, 1.0f, 1.0f };
    float coatRoughness = 0.0f;
    float coatRoughnessAnisotropy = 0.0f;
    float coatIor = 1.6f;
    float coatDarkening = 1.0f;
    DirectX::XMFLOAT2 coatAnisotropyRotationCosSin = { 1.0f, 0.0f };

    float fuzzWeight = 0.0f;
    DirectX::XMFLOAT3 fuzzColor = { 1.0f, 1.0f, 1.0f };
    float fuzzRoughness = 0.5f;

    float transmissionWeight = 0.0f;
    DirectX::XMFLOAT3 transmissionColor = { 1.0f, 1.0f, 1.0f };
    float transmissionDepth = 0.0f;
    DirectX::XMFLOAT3 transmissionScatter = { 0.0f, 0.0f, 0.0f };
    float transmissionScatterAnisotropy = 0.0f;
    float transmissionDispersionScale = 0.0f;
    float transmissionDispersionAbbeNumber = 20.0f;

    float thinFilmWeight = 0.0f;
    float thinFilmThickness = 0.5f;
    float thinFilmIor = 1.4f;

    float emissionLuminance = 0.0f;
    DirectX::XMFLOAT3 emissionColor = { 1.0f, 1.0f, 1.0f };

    float geometryOpacity = 1.0f;
    bool geometryThinWalled = false;
};

struct MaterialDescription {
    MaterialModel materialModel = MaterialModel::LegacyPreviewSurface;
    std::string name;
    DirectX::XMFLOAT4   diffuseColor = { 1,1,1,1 };
    DirectX::XMFLOAT4   emissiveColor = { 0,0,0,1 };
	float alphaCutoff = 0.5f;
    float heightMapScale = 0.05f;
    float geometricDisplacementMin = 0.0f;
    float geometricDisplacementMax = 0.0f;
	bool negateNormals = false; // Some materials may require this
	bool invertNormalGreen = false; // For OpenGL compatibility
	bool forceDoubleSided = false;
    bool enableGeometricDisplacement = false;
	bool geometricDisplacementOptIn = false;
	bool brniflyVertexAlpha = false;
	bool brniflyZBufferWrite = true;
	bool brniflyDecal = false;
	bool brniflyDynamicDecal = false;
	bool brniflyModelSpaceNormals = false;
	bool heightMapFromBaseColorAlpha = false;
	bool geometricHeightRemapRequired = false;
	bool geometricHeightRemapSucceeded = false;
	bool forceVoxelMaterial = false;
	BlendState blendState = BlendState::BLEND_STATE_UNKNOWN; // By default, infer from other properties
    TextureAndConstant  baseColor = {};
    TextureAndConstant  metallic = { nullptr, 0.0f };
    TextureAndConstant  roughness = { nullptr, 0.5f };
    TextureAndConstant  emissive = {};
    TextureAndConstant  opacity = { nullptr, 1.0f };
	TextureAndConstant  aoMap = {};
	TextureAndConstant  heightMap = {};
    TextureAndConstant	normal = {};
    OpenPBRMaterialParameters openPBR = {};
    OpenPBRTextureBindings openPBRTextures = {};
    bool glintEnabled = false;
    DirectX::XMFLOAT4 glintParameters = { 1.5f, 0.0f, 0.015f, 2.0f };
};

inline OpenPBRMaterialParameters TranslateLegacyMaterialDescriptionToOpenPBR(const MaterialDescription& desc) {
    OpenPBRMaterialParameters result = {};

    const float baseColorScale = desc.baseColor.factor.Get();
    result.baseColor = {
        desc.diffuseColor.x * baseColorScale,
        desc.diffuseColor.y * baseColorScale,
        desc.diffuseColor.z * baseColorScale,
    };
    result.baseMetalness = std::clamp(desc.metallic.factor.Get(), 0.0f, 1.0f);
    result.specularRoughness = std::clamp(desc.roughness.factor.Get(), 0.0f, 1.0f);

    const float emissiveScale = desc.emissive.factor.Get();
    const DirectX::XMFLOAT3 emissive = {
        desc.emissiveColor.x * emissiveScale,
        desc.emissiveColor.y * emissiveScale,
        desc.emissiveColor.z * emissiveScale,
    };
    const float emissivePeak = std::max({ emissive.x, emissive.y, emissive.z });
    if (emissivePeak > 0.0f) {
        result.emissionLuminance = emissivePeak;
        const float invPeak = 1.0f / emissivePeak;
        result.emissionColor = {
            emissive.x * invPeak,
            emissive.y * invPeak,
            emissive.z * invPeak,
        };
    }

    result.geometryOpacity = std::clamp(desc.diffuseColor.w * desc.opacity.factor.Get(), 0.0f, 1.0f);
    return result;
}

inline OpenPBRMaterialParameters BuildCanonicalOpenPBRMaterial(const MaterialDescription& desc) {
    if (desc.materialModel == MaterialModel::OpenPBR) {
        return desc.openPBR;
    }

    return TranslateLegacyMaterialDescriptionToOpenPBR(desc);
}
