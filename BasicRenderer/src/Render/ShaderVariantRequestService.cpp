#include "Render/ShaderVariantRequestService.h"

#include "Managers/Singletons/PSOManager.h"

bool ShaderVariantRequestService::RequestShaderVariant(const ShaderVariantRequest& request) const
{
	const ShaderVariantRequest normalized = NormalizeShaderVariantRequest(request);
	auto& psoManager = PSOManager::GetInstance();

	switch (normalized.kind) {
	case ShaderVariantKind::MaterialEvaluation:
		return psoManager.TryGetMaterialEvalPSO(normalized.materialCompileFlags) != nullptr;
	case ShaderVariantKind::ClusterLODRaster:
		return psoManager.TryGetClusterLODRasterPSO(normalized.materialRasterFlags, normalized.wireframe) != nullptr;
	case ShaderVariantKind::ClusterLODVirtualShadowRaster:
		return psoManager.TryGetClusterLODVirtualShadowRasterPSO(normalized.materialRasterFlags, normalized.wireframe) != nullptr;
	case ShaderVariantKind::ClusterLODVirtualShadowReyesRaster:
		return psoManager.TryGetClusterLODVirtualShadowReyesRasterPSO(normalized.materialRasterFlags, normalized.wireframe) != nullptr;
	case ShaderVariantKind::ClusterLODDeepVisibilityRaster:
		return psoManager.TryGetClusterLODDeepVisibilityRasterPSO(normalized.materialRasterFlags, normalized.wireframe) != nullptr;
	case ShaderVariantKind::ClusterLODAVBOITOccupancy:
		return psoManager.TryGetClusterLODAVBOITOccupancyPSO(normalized.materialRasterFlags, normalized.wireframe) != nullptr;
	case ShaderVariantKind::ClusterLODAVBOITRaster:
		return psoManager.TryGetClusterLODAVBOITRasterPSO(normalized.materialRasterFlags, normalized.wireframe) != nullptr;
	case ShaderVariantKind::ClusterLODAVBOITShade:
		return psoManager.TryGetClusterLODAVBOITShadePSO(normalized.materialRasterFlags, normalized.wireframe) != nullptr;
	case ShaderVariantKind::ClusterLODSoftwareRaster:
		return psoManager.TryGetClusterLODSoftwareRasterPSO(normalized.materialRasterFlags, normalized.rasterOutputKind) != nullptr;
	}

	return false;
}
