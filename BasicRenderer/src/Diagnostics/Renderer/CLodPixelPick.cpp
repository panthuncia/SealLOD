#include <BasicRenderer/Diagnostics/CLodPixelPick.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <format>
#include <mutex>
#include <sstream>

#include <BasicRenderer/Assets/ClusterLODShaderTypes.h>
#include <BasicRenderer/Assets/Geometry/Mesh.h>
#include <BasicRenderer/Assets/Material.h>
#include <BasicRenderer/Assets/MaterialDescription.h>
#include <BasicRenderer/Diagnostics/CLodTelemetry.h>
#include <BasicRenderer/Extensions/ShaderBuffers.h>
#include <BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h>

namespace br::diagnostics {
	namespace {
		// visibilityPacking.hlsli: 31 depth bits, 26 cluster bits, 7 triangle bits.
		constexpr std::uint32_t kTriangleBits = 7u, kClusterBits = 26u;
		constexpr std::size_t kSurfaceRecordBytes = 32u;
		constexpr std::uint32_t kVoxelFlag = 0x80000000u;

		template<class T>
		bool Row(const std::vector<std::byte>* table, std::size_t index, T& out)
		{
			if (table == nullptr || (index + 1u) * sizeof(T) > table->size()) {
				return false;
			}
			std::memcpy(&out, table->data() + index * sizeof(T), sizeof(T));
			return true;
		}

		template<class T>
		bool Row(const std::shared_ptr<const std::vector<std::byte>>& table, std::size_t index, T& out)
		{
			return Row(table.get(), index, out);
		}

		void FillSource(std::uint64_t identity, std::string& name, std::string& source, std::string& prim, std::string& subset, std::shared_ptr<Mesh>& mesh)
		{
			std::scoped_lock lock(g_clodMeshSources.mutex);
			const auto known = g_clodMeshSources.meshes.find(identity);
			if (known == g_clodMeshSources.meshes.end()) {
				return;
			}
			name = known->second.name;
			source = known->second.cacheSourceIdentifier;
			prim = known->second.cachePrimPath;
			subset = known->second.cacheSubsetName;
			mesh = known->second.mesh.lock();
		}

		std::string Index(std::uint32_t value)
		{
			return value == kCLodPickInvalid ? std::string("-") : std::to_string(value);
		}
	}

	CLodPixelPick PickCLodPixel(const Renderer::CLodInspectionFrame& frame, std::uint32_t x, std::uint32_t y)
	{
		CLodPixelPick pick;
		pick.x = x;
		pick.y = y;
		pick.frame = frame.frame;
		if (x >= frame.width || y >= frame.height) {
			pick.error = "pixel outside the captured frame";
			return pick;
		}
		const std::size_t pixelIndex = static_cast<std::size_t>(y) * frame.width + x;

		if ((pixelIndex + 1u) * kSurfaceRecordBytes <= frame.surfaceRecords.size()) {
			std::array<std::uint32_t, 8> words{};
			std::memcpy(words.data(), frame.surfaceRecords.data() + pixelIndex * kSurfaceRecordBytes, sizeof(words));
			pick.hasRecord = true;
			pick.recordObjectId = static_cast<std::uint64_t>(words[0]) | (static_cast<std::uint64_t>(words[1]) << 32u);
			pick.recordSourceMaterialId = static_cast<std::uint64_t>(words[2]) | (static_cast<std::uint64_t>(words[3]) << 32u);
			pick.recordMaterialTableIndex = words[4];
			pick.recordSemanticFamilyAndPayload = words[5];
			pick.recordFlags = words[6];
			pick.recordDiagnosticReason = words[7];
		}

		if (frame.visibility.size() < static_cast<std::size_t>(frame.width) * frame.height) {
			pick.hit = pick.hasRecord && pick.recordObjectId != 0u;
			pick.error = frame.pathTraced ? "path traced: the frame has surface records but no visibility keys"
			                              : "the frame has no visibility keys";
			return pick;
		}
		const std::uint64_t key = frame.visibility[pixelIndex];
		if (key == ~std::uint64_t{ 0 }) {
			pick.error = "no geometry (cleared visibility key)";
			return pick;
		}
		pick.hit = true;
		pick.keyTriangle = static_cast<std::uint32_t>(key & ((1u << kTriangleBits) - 1u));
		pick.keyCluster = static_cast<std::uint32_t>((key >> kTriangleBits) & ((1u << kClusterBits) - 1u));
		const std::uint32_t depthBits = static_cast<std::uint32_t>(key >> (kTriangleBits + kClusterBits)) << 1u;
		std::memcpy(&pick.depth, &depthBits, sizeof(pick.depth));

		pick.visibleCluster = pick.keyCluster;
		pick.triangle = pick.keyTriangle;
		if (pick.keyCluster >= frame.reyesPatchIndexBase) {
			pick.reyesPatch = true;
			pick.reyesPatchIndex = pick.keyCluster - frame.reyesPatchIndexBase;
			CLodReyesDiceQueueEntry dice{};
			if (!Row(&frame.reyesDiceQueue, pick.reyesPatchIndex, dice)) {
				pick.error = "Reyes patch key outside the read dice queue";
				return pick;
			}
			pick.visibleCluster = dice.visibleClusterIndex;
			pick.triangle = dice.sourcePrimitiveAndSplitConfig & 0xFFFFu;
			pick.reyesDiceMaterialIndex = dice.materialIndex;
			pick.reyesDiceFlags = dice.flags;
		}

		std::array<std::uint32_t, 4> cluster{};
		if (!Row(&frame.visibleClusters, pick.visibleCluster, cluster)) {
			pick.error = "visible cluster outside the read buffer";
			return pick;
		}
		pick.drawRecord = cluster[0] >> PackedVisibleClusterViewBits;
		pick.localMeshlet = cluster[1] & 0x3FFFu;
		pick.globalGroup = ((cluster[1] >> 14u) & 0x3FFFFu) | ((cluster[2] & 0x3u) << 18u) | (((cluster[3] >> 30u) & 0x1u) << 20u);
		pick.voxel = (cluster[3] & kVoxelFlag) != 0u;

		std::array<std::uint32_t, 2> sideBand{};
		if (Row(&frame.visibleClusterSideBand, pick.visibleCluster, sideBand)) {
			pick.hasSideBand = true;
			pick.assemblyTransform = sideBand[0];
			pick.sideBandPaletteIndex = sideBand[1];
		}

		InstanceDrawRecordCB draw{};
		if (!Row(frame.drawRecords, pick.drawRecord, draw)) {
			pick.error = "draw record outside the published table";
			return pick;
		}
		pick.meshTemplate = draw.meshTemplateIndex;
		pick.instanceTransform = draw.instanceTransformIndex;
		pick.clodOffset = draw.clodOffsetIndex;

		PerMeshInstanceCB meshTemplate{};
		if (Row(frame.meshTemplates, draw.meshTemplateIndex, meshTemplate)) {
			pick.perMesh = meshTemplate.perMeshBufferIndex;
			pick.paletteBase = meshTemplate.materialPaletteBase;
			pick.paletteCount = meshTemplate.materialPaletteCount;
		}
		PerMeshCB perMesh{};
		if (Row(frame.perMeshes, pick.perMesh, perMesh)) {
			pick.perMeshMaterialDataIndex = perMesh.materialDataIndex;
			pick.perMeshReyesEvalCompileFlagsID = perMesh.materialReyesEvalCompileFlagsID;
		}

		MeshInstanceClodOffsets offsets{};
		CLodMeshMetadata metadata{};
		if (Row(frame.clodOffsets, draw.clodOffsetIndex, offsets) && Row(frame.meshMetadata, offsets.clodMeshMetadataIndex, metadata)) {
			pick.drawMetadata = offsets.clodMeshMetadataIndex;
			pick.drawMeshIdentity = (static_cast<std::uint64_t>(metadata.meshIdentityHi) << 32u) | metadata.meshIdentityLo;
			pick.drawMaterialSlotCount = metadata.materialSlotCount;
		}
		// GPU copy of the group: the CPU's firstGroupVertex field carries partIndex, the metadata row of the owning part.
		ClusterLODGroup group{};
		CLodMeshMetadata partMetadata{};
		if (Row(frame.groups, pick.globalGroup, group) && Row(frame.meshMetadata, group.firstGroupVertex, partMetadata)) {
			pick.partMetadata = group.firstGroupVertex;
			pick.partMeshIdentity = (static_cast<std::uint64_t>(partMetadata.meshIdentityHi) << 32u) | partMetadata.meshIdentityLo;
			pick.partMaterialSlotCount = partMetadata.materialSlotCount;
			pick.partPerMesh = partMetadata.perMeshBufferIndex;
		}

		// CLodResolveMaterial: a palette only when it has more than one row; the side band's index, clamped.
		pick.resolvedMaterialDataIndex = pick.perMeshMaterialDataIndex;
		if (pick.paletteCount > 1u) {
			for (std::uint32_t slot = 0; slot < pick.paletteCount; ++slot) {
				CLodMaterialPaletteEntry entry{};
				if (!Row(frame.materialPalettes, pick.paletteBase + slot, entry)) {
					break;
				}
				pick.palette.push_back({ entry.materialDataIndex, entry.materialEvalCompileFlagsID, entry.materialReyesEvalCompileFlagsID, entry.rasterBucketIndex });
			}
			const std::uint32_t requested = pick.hasSideBand ? pick.sideBandPaletteIndex : 0u;
			pick.paletteIndexUsed = (std::min)(requested, pick.paletteCount - 1u);
			if (pick.paletteIndexUsed < pick.palette.size()) {
				pick.resolvedMaterialDataIndex = pick.palette[pick.paletteIndexUsed].materialDataIndex;
			}
		}

		FillSource(pick.drawMeshIdentity, pick.drawMeshName, pick.drawCacheSource, pick.drawCachePrimPath, pick.drawCacheSubset, pick.drawMesh);
		if (pick.partMeshIdentity != 0u && pick.partMeshIdentity != pick.drawMeshIdentity) {
			FillSource(pick.partMeshIdentity, pick.partMeshName, pick.partCacheSource, pick.partCachePrimPath, pick.partCacheSubset, pick.partMesh);
		}
		return pick;
	}

	std::string DescribeMaterialForPick(const Material& material)
	{
		const MaterialDescription desc = material.ToCacheDescription();
		const auto& data = material.GetData();
		std::string text = std::format("'{}' id={} slot={} srcId=0x{:016x} sampling={} displace={} [{}, {}] heightScale={}",
			desc.name, material.GetMaterialID(), data.openPBRMaterialDataIndex, desc.sourceMaterialIdentity,
			static_cast<int>(desc.objectSurfaceSamplingMode), desc.enableGeometricDisplacement ? 1 : 0,
			desc.geometricDisplacementMin, desc.geometricDisplacementMax, desc.heightMapScale);
		const auto texture = [&](const char* label, const TextureAndConstant& binding) {
			if (!binding.sourcePath.empty()) {
				text += std::format(" {}='{}'", label, binding.sourcePath);
			}
			if (!binding.uvSetName.empty()) {
				text += std::format(" {}Uv='{}'", label, binding.uvSetName);
			}
		};
		texture("base", desc.baseColor);
		texture("normal", desc.normal);
		texture("height", desc.heightMap);
		if (!desc.staticTextureOverrideSourceName.empty()) {
			text += std::format(" overrideName='{}'", desc.staticTextureOverrideSourceName);
		}
		return text;
	}

	std::string FormatCLodPixelPick(const CLodPixelPick& pick)
	{
		std::ostringstream out;
		out << std::format("pixel ({}, {}) frame {}\n", pick.x, pick.y, pick.frame);
		if (pick.hasRecord) {
			out << std::format("record: object=0x{:016x} sourceMaterial=0x{:016x} materialTable={} family/payload=0x{:08x} flags=0x{:08x} reason={}\n",
				pick.recordObjectId, pick.recordSourceMaterialId, Index(pick.recordMaterialTableIndex), pick.recordSemanticFamilyAndPayload,
				pick.recordFlags, pick.recordDiagnosticReason);
		}
		if (!pick.hit || pick.visibleCluster == kCLodPickInvalid) {
			out << (pick.hit ? "" : "no hit: ") << pick.error << "\n";
			return out.str();
		}
		out << std::format("key: depth={} cluster={} triangle={}{}\n", pick.depth, pick.keyCluster, pick.keyTriangle,
			pick.reyesPatch ? std::format(" (Reyes patch {} -> cluster {}, dice material {}, dice flags 0x{:x})", pick.reyesPatchIndex,
				pick.visibleCluster, Index(pick.reyesDiceMaterialIndex), pick.reyesDiceFlags) : std::string());
		out << std::format("cluster {}: draw {} meshlet {} group {} voxel={} sideBand=({}, {})\n", Index(pick.visibleCluster), Index(pick.drawRecord),
			pick.localMeshlet, Index(pick.globalGroup), pick.voxel ? 1 : 0, Index(pick.assemblyTransform), Index(pick.sideBandPaletteIndex));
		out << std::format("draw: template {} transform {} clodOffset {} perMesh {} perMeshMaterial {} palette base={} count={}\n",
			Index(pick.meshTemplate), Index(pick.instanceTransform), Index(pick.clodOffset), Index(pick.perMesh), Index(pick.perMeshMaterialDataIndex),
			pick.paletteBase, pick.paletteCount);
		out << std::format("draw mesh: '{}' identity=0x{:016x} metadata {} materialSlots={} cache='{}' prim='{}' subset='{}'\n", pick.drawMeshName,
			pick.drawMeshIdentity, Index(pick.drawMetadata), pick.drawMaterialSlotCount, pick.drawCacheSource, pick.drawCachePrimPath, pick.drawCacheSubset);
		if (pick.partMetadata != pick.drawMetadata) {
			out << std::format("part mesh: '{}' identity=0x{:016x} metadata {} materialSlots={} perMesh {} cache='{}' prim='{}' subset='{}'\n",
				pick.partMeshName, pick.partMeshIdentity, Index(pick.partMetadata), pick.partMaterialSlotCount, Index(pick.partPerMesh),
				pick.partCacheSource, pick.partCachePrimPath, pick.partCacheSubset);
		}
		out << std::format("resolve: palette index {} -> materialData {}{}\n", pick.paletteIndexUsed, Index(pick.resolvedMaterialDataIndex),
			pick.hasRecord && pick.recordMaterialTableIndex != pick.resolvedMaterialDataIndex ? "  ** differs from the surface record **" : "");
		for (std::size_t slot = 0; slot < pick.palette.size(); ++slot) {
			const auto& entry = pick.palette[slot];
			out << std::format("  palette[{}]: materialData {} eval {} reyesEval {} bucket {}\n", slot, entry.materialDataIndex,
				entry.materialEvalCompileFlagsID, entry.materialReyesEvalCompileFlagsID, entry.rasterBucketIndex);
		}
		const auto describeMesh = [&](const char* label, const std::shared_ptr<Mesh>& mesh) {
			if (!mesh) {
				return;
			}
			if (mesh->material) {
				out << label << " material: " << DescribeMaterialForPick(*mesh->material) << "\n";
			}
			const auto& palette = mesh->GetMaterialPalette();
			for (std::size_t slot = 0; slot < palette.size(); ++slot) {
				if (palette[slot]) {
					out << std::format("  {} palette[{}]: ", label, slot) << DescribeMaterialForPick(*palette[slot]) << "\n";
				}
			}
		};
		describeMesh("draw mesh", pick.drawMesh);
		describeMesh("part mesh", pick.partMesh);
		if (!pick.error.empty()) {
			out << "stopped: " << pick.error << "\n";
		}
		return out.str();
	}
}
