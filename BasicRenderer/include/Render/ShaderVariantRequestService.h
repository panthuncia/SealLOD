#pragma once

#include <cstdint>

#include "Materials/TechniqueDescriptor.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RasterBucketFlags.h"

enum class ShaderVariantKind : std::uint8_t {
	MaterialEvaluation,
	ClusterLODRaster,
	ClusterLODVirtualShadowRaster,
	ClusterLODVirtualShadowReyesRaster,
	ClusterLODDeepVisibilityRaster,
	ClusterLODAVBOITOccupancy,
	ClusterLODAVBOITRaster,
	ClusterLODAVBOITShade,
	ClusterLODSoftwareRaster,
};

struct ShaderVariantRequest {
	ShaderVariantKind kind{ ShaderVariantKind::MaterialEvaluation };
	MaterialCompileFlags materialCompileFlags{ MaterialCompileNone };
	MaterialRasterFlags materialRasterFlags{ MaterialRasterFlagsNone };
	CLodRasterOutputKind rasterOutputKind{ CLodRasterOutputKind::VisibilityBuffer };
	bool wireframe{ false };

	bool operator==(const ShaderVariantRequest&) const = default;
};

inline MaterialCompileFlags GetMaterialEvaluationShaderKey(MaterialCompileFlags flags)
{
	constexpr std::uint64_t shaderAffectingFlags =
		MaterialCompileFlags::MaterialCompileBlend |
		MaterialCompileFlags::MaterialCompileAlphaTest |
		MaterialCompileFlags::MaterialCompileDoubleSided |
		MaterialCompileFlags::MaterialCompileBaseColorTexture |
		MaterialCompileFlags::MaterialCompileNormalMap |
		MaterialCompileFlags::MaterialCompileMetallicTexture |
		MaterialCompileFlags::MaterialCompileRoughnessTexture |
		MaterialCompileFlags::MaterialCompileAOTexture |
		MaterialCompileFlags::MaterialCompileEmissiveTexture |
		MaterialCompileFlags::MaterialCompileParallax |
		MaterialCompileFlags::MaterialCompileGeometricDisplacement |
		MaterialCompileFlags::MaterialCompileOpacityTexture |
		MaterialCompileFlags::MaterialCompileOpenPBRCoatColorTexture |
		MaterialCompileFlags::MaterialCompileOpenPBRCoatWeightTexture |
		MaterialCompileFlags::MaterialCompileOpenPBRCoatRoughnessTexture |
		MaterialCompileFlags::MaterialCompileOpenPBRFuzzColorTexture |
		MaterialCompileFlags::MaterialCompileOpenPBRFuzzWeightTexture |
		MaterialCompileFlags::MaterialCompileOpenPBRFuzzRoughnessTexture |
		MaterialCompileFlags::MaterialCompileVoxel |
		MaterialCompileFlags::MaterialCompileTextureStreaming |
		MaterialCompileFlags::MaterialCompileHeightFromBaseAlpha |
		MaterialCompileFlags::MaterialCompileTerrain |
		MaterialCompileFlags::MaterialCompileClodReyesPatch |
		MaterialCompileFlags::MaterialCompileClodVertexColor |
		MaterialCompileFlags::MaterialCompileClodSkinning;
	return static_cast<MaterialCompileFlags>(static_cast<std::uint64_t>(flags) & shaderAffectingFlags);
}

inline ShaderVariantRequest NormalizeShaderVariantRequest(ShaderVariantRequest request)
{
	if (request.kind == ShaderVariantKind::MaterialEvaluation) {
		request.materialCompileFlags = GetMaterialEvaluationShaderKey(request.materialCompileFlags);
		request.materialRasterFlags = MaterialRasterFlagsNone;
		request.rasterOutputKind = CLodRasterOutputKind::VisibilityBuffer;
		request.wireframe = false;
	} else if (request.kind != ShaderVariantKind::ClusterLODSoftwareRaster) {
		request.materialCompileFlags = MaterialCompileNone;
		request.rasterOutputKind = CLodRasterOutputKind::VisibilityBuffer;
	}
	return request;
}

class ShaderVariantRequestService {
public:
	bool RequestShaderVariant(const ShaderVariantRequest& request) const;
};
