#pragma once
#include <unordered_set>
#include <cstdint>

#include "Render/RenderPhase.h"
#include "Render/RasterBucketFlags.h"

enum MaterialCompileFlags : uint64_t {
	MaterialCompileNone = 0,
	MaterialCompileBlend = 1ull << 0,
	MaterialCompileAlphaTest = 1ull << 1,
	MaterialCompileDoubleSided = 1ull << 2,
	MaterialCompileBaseColorTexture = 1ull << 3,
	MaterialCompileNormalMap = 1ull << 4,
	MaterialCompileMetallicTexture = 1ull << 5,
	MaterialCompileRoughnessTexture = 1ull << 6,
	MaterialCompileAOTexture = 1ull << 7,
	MaterialCompileEmissiveTexture = 1ull << 8,
	MaterialCompileParallax = 1ull << 9,
	MaterialCompileGeometricDisplacement = 1ull << 10,
	MaterialCompileOpacityTexture = 1ull << 11,
	MaterialCompileOpenPBRCoatColorTexture = 1ull << 12,
	MaterialCompileOpenPBRCoatWeightTexture = 1ull << 13,
	MaterialCompileOpenPBRCoatRoughnessTexture = 1ull << 14,
	MaterialCompileOpenPBRFuzzColorTexture = 1ull << 15,
	MaterialCompileOpenPBRFuzzWeightTexture = 1ull << 16,
	MaterialCompileOpenPBRFuzzRoughnessTexture = 1ull << 17,
	MaterialCompileVoxel = 1ull << 18,
	MaterialCompileTextureStreaming = 1ull << 19,
	MaterialCompileHeightFromBaseAlpha = 1ull << 20,
};

// |= operator for MaterialCompileFlags
inline MaterialCompileFlags operator|=(MaterialCompileFlags& a, MaterialCompileFlags b) {
	a = static_cast<MaterialCompileFlags>(static_cast<uint64_t>(a) | static_cast<uint64_t>(b));
	return a;
}

struct TechniqueDescriptor {
	std::unordered_set<RenderPhase> passes; // Which render passes (that do per-object work) this technique participates in
	MaterialCompileFlags compileFlags = static_cast<MaterialCompileFlags>(0); // Any difference here requires a separate material eval PSO
	// metadata: alphaMode, domain (Deferred/Forward), feature bits (aniso, parallax)?
	// TechniqueDescriptor::Hasher- just use the compileFlags as the hash, the passes are calculated from that
	MaterialRasterFlags rasterFlags = MaterialRasterFlagsNone; // Any difference here requires a separate raster PSO
	struct Hasher {
		size_t operator()(TechniqueDescriptor const& td) const noexcept {
			return std::hash<uint64_t>()(static_cast<uint64_t>(td.compileFlags));
		}
	};
	// Equality operator
	bool operator==(TechniqueDescriptor const& o) const noexcept {
		return (compileFlags == o.compileFlags);
	}

};

struct DrawWorkloadKey {
    MaterialCompileFlags compileFlags = MaterialCompileFlags::MaterialCompileNone;
    RenderPhase renderPhase;
    bool clodOnly = false;

    bool operator==(const DrawWorkloadKey& other) const noexcept {
        return compileFlags == other.compileFlags
            && renderPhase == other.renderPhase
            && clodOnly == other.clodOnly;
    }

    struct Hasher {
        size_t operator()(const DrawWorkloadKey& key) const noexcept {
            size_t seed = std::hash<uint64_t>()(static_cast<uint64_t>(key.compileFlags));
            seed ^= RenderPhase::Hasher{}(key.renderPhase) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            seed ^= std::hash<bool>()(key.clodOnly) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            return seed;
        }
    };
};
