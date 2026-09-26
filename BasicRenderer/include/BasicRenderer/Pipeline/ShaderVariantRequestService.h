#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

#include "BasicRenderer/Assets/TechniqueDescriptor.h"
#include <BasicRenderer/Pipeline/MaterialEvalVariants.h>
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Pipeline/RasterBucketFlags.h"

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
	bool singleView{ false };

	bool operator==(const ShaderVariantRequest&) const = default;
};

inline ShaderVariantRequest NormalizeShaderVariantRequest(ShaderVariantRequest request)
{
	if (request.kind == ShaderVariantKind::MaterialEvaluation) {
		request.materialCompileFlags = GetMaterialEvaluationShaderKey(request.materialCompileFlags);
		request.materialRasterFlags = MaterialRasterFlagsNone;
		request.rasterOutputKind = CLodRasterOutputKind::VisibilityBuffer;
		request.wireframe = false;
		request.singleView = false;
	} else if (request.kind != ShaderVariantKind::ClusterLODSoftwareRaster) {
		request.materialCompileFlags = MaterialCompileNone;
		request.rasterOutputKind = CLodRasterOutputKind::VisibilityBuffer;
	}
	if (request.kind != ShaderVariantKind::ClusterLODRaster) {
		request.singleView = false;
	}
	return request;
}

class ShaderVariantRequestService {
public:
	struct BatchRequestResult {
		std::size_t requested{ 0 };
		std::size_t unique{ 0 };
		std::size_t ready{ 0 };
		std::size_t pending{ 0 };
	};

	bool RequestShaderVariant(const ShaderVariantRequest& request) const;
	BatchRequestResult RequestShaderVariants(const std::vector<ShaderVariantRequest>& requests) const;
};
