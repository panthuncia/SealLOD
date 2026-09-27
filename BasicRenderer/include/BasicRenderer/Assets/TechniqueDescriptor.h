#pragma once

#include <unordered_set>

#include <BasicRenderer/Pipeline/DrawWorkload.h>
#include "BasicRenderer/Pipeline/RasterBucketFlags.h"

struct TechniqueDescriptor {
	std::unordered_set<RenderPhase> passes; // Which render passes (those that do per-object work) this technique participates in.
	MaterialCompileFlags compileFlags = static_cast<MaterialCompileFlags>(0); // Any difference here requires a separate material eval PSO.
	MaterialRasterFlags rasterFlags = MaterialRasterFlagsNone; // Any difference here requires a separate raster PSO.
	struct Hasher {
		size_t operator()(TechniqueDescriptor const& td) const noexcept {
			return std::hash<uint64_t>()(static_cast<uint64_t>(td.compileFlags));
		}
	};
	bool operator==(TechniqueDescriptor const& o) const noexcept {
		return (compileFlags == o.compileFlags);
	}
};
