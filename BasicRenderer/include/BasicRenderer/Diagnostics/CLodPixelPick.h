#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <BasicRenderer/Renderer.h>

class Mesh;
class Material;

// What drew one pixel of a CLodInspectionFrame, decoded on the CPU the way clodResolveMeshlet.hlsli's
// LoadMeshletResolveData and clodResolveCommon.hlsli's CLodResolvePatchKey do on the GPU. Every index the shader walks is
// kept, so a wrong material can be traced to the table that chose it.
namespace br::diagnostics {

	inline constexpr std::uint32_t kCLodPickInvalid = 0xFFFFFFFFu;

	struct CLodPickPaletteEntry {
		std::uint32_t materialDataIndex = 0;
		std::uint32_t materialEvalCompileFlagsID = 0;
		std::uint32_t materialReyesEvalCompileFlagsID = 0;
		std::uint32_t rasterBucketIndex = 0;
	};

	struct CLodPixelPick {
		std::uint32_t x = 0, y = 0;
		std::uint64_t frame = 0;
		bool hit = false;
		std::string error;  // why the decode stopped early, when it did

		// Visibility key.
		float depth = 0.0f;
		std::uint32_t keyCluster = 0;   // as written: a visible cluster, or a Reyes patch index above the base
		std::uint32_t keyTriangle = 0;  // triangle, or micro-triangle of a patch
		bool reyesPatch = false;
		std::uint32_t reyesPatchIndex = kCLodPickInvalid;
		std::uint32_t reyesDiceMaterialIndex = kCLodPickInvalid;  // the material the dice entry was built with
		std::uint32_t reyesDiceFlags = 0;

		// Visible cluster.
		std::uint32_t visibleCluster = kCLodPickInvalid;
		std::uint32_t triangle = 0;
		bool voxel = false;
		std::uint32_t drawRecord = kCLodPickInvalid;
		std::uint32_t localMeshlet = 0;
		std::uint32_t globalGroup = kCLodPickInvalid;
		std::uint32_t assemblyTransform = kCLodPickInvalid;  // side band .x; invalid when not reached through a portal
		std::uint32_t sideBandPaletteIndex = kCLodPickInvalid;  // side band .y
		bool hasSideBand = false;

		// Draw record and mesh template.
		std::uint32_t meshTemplate = kCLodPickInvalid;
		std::uint32_t instanceTransform = kCLodPickInvalid;
		std::uint32_t clodOffset = kCLodPickInvalid;
		std::uint32_t perMesh = kCLodPickInvalid;
		std::uint32_t paletteBase = 0, paletteCount = 0;
		std::uint32_t perMeshMaterialDataIndex = kCLodPickInvalid;  // the draw mesh's own material (slot 0 of a palette)
		std::uint32_t perMeshReyesEvalCompileFlagsID = 0;

		// Mesh metadata: the draw's mesh, and the part owning the cluster's group (the same unless a portal reached it).
		std::uint32_t drawMetadata = kCLodPickInvalid;
		std::uint64_t drawMeshIdentity = 0;
		std::uint32_t drawMaterialSlotCount = 0;
		std::uint32_t partMetadata = kCLodPickInvalid;
		std::uint64_t partMeshIdentity = 0;
		std::uint32_t partMaterialSlotCount = 0;
		std::uint32_t partPerMesh = kCLodPickInvalid;

		// The material the resolve shader picks, and the palette it picks from.
		std::uint32_t paletteIndexUsed = 0;  // after the clamp CLodResolveMaterial applies
		std::uint32_t resolvedMaterialDataIndex = kCLodPickInvalid;
		std::vector<CLodPickPaletteEntry> palette;

		// The surface record the frame actually wrote, when the frame has records.
		bool hasRecord = false;
		std::uint64_t recordObjectId = 0;
		std::uint64_t recordSourceMaterialId = 0;
		std::uint32_t recordMaterialTableIndex = kCLodPickInvalid;
		std::uint32_t recordSemanticFamilyAndPayload = 0;
		std::uint32_t recordFlags = 0;
		std::uint32_t recordDiagnosticReason = 0;

		// CPU objects from g_clodMeshSources, when still alive.
		std::string drawMeshName, partMeshName;
		std::string drawCacheSource, drawCachePrimPath, drawCacheSubset;
		std::string partCacheSource, partCachePrimPath, partCacheSubset;
		std::shared_ptr<Mesh> drawMesh, partMesh;
	};

	CLodPixelPick PickCLodPixel(const Renderer::CLodInspectionFrame& frame, std::uint32_t x, std::uint32_t y);

	// One line per field worth reading, including the draw mesh's material palette by name and texture.
	std::string FormatCLodPixelPick(const CLodPixelPick& pick);

	// The fields of a material a material question needs: name, textures, displacement and sampling mode.
	std::string DescribeMaterialForPick(const Material& material);
}
