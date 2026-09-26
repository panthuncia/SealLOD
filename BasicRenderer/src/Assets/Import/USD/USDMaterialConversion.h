#pragma once

#include "Assets/Import/USD/USDImportState.h"
#include <BasicRenderer/Assets/Material.h>
#include <BasicRenderer/Assets/Import/MeshPreprocessData.h>

namespace USDLoader {
    std::string NormalizeObjectReyesWhitelistPath(std::string_view path);
    bool MaterialUsesWhitelistedTexture(const MaterialDescription& desc,
        const std::vector<std::string>& textureWhitelist);
    bool ObjectReyesAtlasBakedHeightNifListed(const InMemoryStageOptions& stageOptions);
    bool SupportsObjectReyesGeometricDisplacementCandidate(const MaterialDescription& desc);
    bool SupportsPotentialObjectReyesHeightSidecar(const MaterialDescription& desc);
    std::string BuildMaterialTextureSignature(const MaterialDescription& desc);
    void ApplyBrniflyMaterialMetadata(MaterialDescription& result, const pxr::UsdPrim& prim);
    void LoadSourcePathTextures(MaterialDescription& result, const pxr::UsdStageRefPtr& stage, bool loadMaterialTextures);
    void ProcessMaterial(const pxr::UsdShadeMaterial& material, const pxr::UsdStageRefPtr& stage,
        const InMemoryStageOptions& stageOptions, bool isUSDZ, const std::string& directory,
        bool loadMaterialTextures);
    std::vector<MeshUvSetData> BuildMaterialUvSetDescriptors(const pxr::UsdShadeMaterial& material);
    uint32_t ResolveUvSetIndexForBinding(const TextureAndConstant& binding,
        const std::vector<MeshUvSetData>& uvSets, const std::string& materialPath, const char* slotName);
    std::string BuildResolvedMaterialCacheKey(const std::string& materialPath,
        const MaterialDescription& resolvedDesc);
    std::shared_ptr<Material> ResolveDefaultUsdMaterial(bool forceDoubleSided);
}
