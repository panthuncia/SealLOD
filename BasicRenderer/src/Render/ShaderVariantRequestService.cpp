#include "Render/ShaderVariantRequestService.h"

#include <unordered_set>

#include "Managers/Singletons/PSOManager.h"

namespace {

struct ShaderVariantRequestHash {
	std::size_t operator()(const ShaderVariantRequest& request) const noexcept
	{
		std::size_t seed = 0;
		auto combine = [&](std::uint64_t value) {
			seed ^= static_cast<std::size_t>(value) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
		};

		combine(static_cast<std::uint8_t>(request.kind));
		combine(static_cast<std::uint64_t>(request.materialCompileFlags));
		combine(static_cast<std::uint64_t>(request.materialRasterFlags));
		combine(static_cast<std::uint32_t>(request.rasterOutputKind));
		combine(request.wireframe ? 1u : 0u);
		combine(request.singleView ? 1u : 0u);
		return seed;
	}
};

}

bool ShaderVariantRequestService::RequestShaderVariant(const ShaderVariantRequest& request) const
{
	const ShaderVariantRequest normalized = NormalizeShaderVariantRequest(request);
	auto& psoManager = PSOManager::GetInstance();

	switch (normalized.kind) {
	case ShaderVariantKind::MaterialEvaluation:
		return psoManager.TryGetMaterialEvalPSO(normalized.materialCompileFlags) != nullptr;
	case ShaderVariantKind::ClusterLODRaster:
		return psoManager.TryGetClusterLODRasterPSO(
			normalized.materialRasterFlags, normalized.wireframe, normalized.singleView) != nullptr;
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

ShaderVariantRequestService::BatchRequestResult ShaderVariantRequestService::RequestShaderVariants(
	const std::vector<ShaderVariantRequest>& requests) const
{
	BatchRequestResult result;
	result.requested = requests.size();

	std::unordered_set<ShaderVariantRequest, ShaderVariantRequestHash> uniqueRequests;
	uniqueRequests.reserve(requests.size());
	for (ShaderVariantRequest request : requests) {
		uniqueRequests.insert(NormalizeShaderVariantRequest(request));
	}

	result.unique = uniqueRequests.size();
	for (const ShaderVariantRequest& request : uniqueRequests) {
		if (RequestShaderVariant(request)) {
			++result.ready;
		}
		else {
			++result.pending;
		}
	}

	return result;
}
