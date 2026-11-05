#include "Analyzers/Analyzers.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <lz4.h>

namespace clodeval {

namespace {
	constexpr double kStreamingPageBytes = 256.0 * 1024.0;

	uint64_t Lz4CompressedBytes(std::span<const std::byte> bytes)
	{
		if (bytes.empty()) {
			return 0u;
		}
		std::vector<char> compressed(static_cast<size_t>(LZ4_compressBound(static_cast<int>(bytes.size()))));
		return static_cast<uint64_t>(LZ4_compress_default(
			reinterpret_cast<const char*>(bytes.data()), compressed.data(), static_cast<int>(bytes.size()), static_cast<int>(compressed.size())));
	}

	void AppendBytes(std::vector<std::byte>& out, const void* data, size_t size)
	{
		const auto* bytes = static_cast<const std::byte*>(data);
		out.insert(out.end(), bytes, bytes + size);
	}

	uint32_t Quantize(float value, float minimum, float maximum, uint32_t levels)
	{
		if (maximum <= minimum) {
			return 0u;
		}
		const double normalized = (static_cast<double>(value) - minimum) / (static_cast<double>(maximum) - minimum);
		return static_cast<uint32_t>(std::clamp(std::round(normalized * (levels - 1u)), 0.0, static_cast<double>(levels - 1u)));
	}

	float Dequantize(uint32_t code, float minimum, float maximum, uint32_t levels)
	{
		return minimum + (maximum - minimum) * (static_cast<float>(code) / static_cast<float>(levels - 1u));
	}

	class MemoryPagesAnalyzer final : public IAnalyzer {
	public:
		const AnalyzerInfo& Info() const override
		{
			static const AnalyzerInfo info{ "memory.pages", 1, "Where cache bytes go, and what voxel attribute encoding could save" };
			return info;
		}

		void Run(const AnalysisContext& context, MetricSink& sink) const override
		{
			const CachedMesh& mesh = context.mesh;
			std::map<std::string, uint64_t> triangleStreams;
			std::map<std::string, uint64_t> voxelStreams;
			uint64_t trianglePageBytes = 0;
			uint64_t voxelPageBytes = 0;
			uint64_t triangleCount = 0;
			uint64_t voxelClusters = 0;
			uint64_t voxelCubes = 0;
			uint64_t voxelCells = 0;
			std::vector<double> triangleFill;
			std::vector<double> voxelFill;
			std::vector<double> cellsPerCube;
			std::vector<double> cubesPerCluster;

			// Candidate voxel attribute encoding: oct axis 16:16, sigmas 8:8, opacity 8, uv 16:16 in each
			// page's own uv range. 13 bytes against today's 28. Reported with the error it would add.
			constexpr uint64_t kQuantizedCellBytes = 13u;
			std::vector<std::byte> rawAttributes;
			std::vector<std::byte> quantizedAttributes;
			double maxUvError = 0.0;
			double maxSigmaError = 0.0;
			double maxOpacityError = 0.0;

			for (uint32_t pageIndex = 0; pageIndex < mesh.PageCount(); ++pageIndex) {
				const auto bytes = mesh.Page(pageIndex);
				if (mesh.IsVoxelPage(pageIndex)) {
					CLodCache::VoxelPage page;
					if (!CLodCache::VoxelPage::Parse(bytes, page)) {
						continue;
					}
					voxelPageBytes += bytes.size();
					voxelFill.push_back(static_cast<double>(bytes.size()) / kStreamingPageBytes);
					for (const CLodCache::PageStream& stream : page.Streams()) {
						voxelStreams[stream.name] += stream.sizeBytes;
					}
					voxelClusters += page.ClusterCount();
					voxelCubes += page.CubeCount();
					voxelCells += page.AttributeCount();
					for (uint32_t cluster = 0; cluster < page.ClusterCount(); ++cluster) {
						cubesPerCluster.push_back(page.Cluster(cluster).cubeCount);
					}
					for (uint32_t cube = 0; cube < page.CubeCount(); ++cube) {
						cellsPerCube.push_back(std::popcount(page.Cube(cube).occupancyMask));
					}

					float uvMin[2] = { 1.0e30f, 1.0e30f };
					float uvMax[2] = { -1.0e30f, -1.0e30f };
					for (uint32_t index = 0; index < page.AttributeCount(); ++index) {
						const CLodVoxelAttributeSample attribute = page.Attribute(index);
						uvMin[0] = std::min(uvMin[0], attribute.uv.x);
						uvMin[1] = std::min(uvMin[1], attribute.uv.y);
						uvMax[0] = std::max(uvMax[0], attribute.uv.x);
						uvMax[1] = std::max(uvMax[1], attribute.uv.y);
					}
					for (uint32_t index = 0; index < page.AttributeCount(); ++index) {
						const CLodVoxelAttributeSample attribute = page.Attribute(index);
						AppendBytes(rawAttributes, &attribute, sizeof(attribute));

						const uint16_t axisX = static_cast<uint16_t>(Quantize(attribute.sggxAxisAndSigmas.x, -1.0f, 1.0f, 65536u));
						const uint16_t axisY = static_cast<uint16_t>(Quantize(attribute.sggxAxisAndSigmas.y, -1.0f, 1.0f, 65536u));
						const uint8_t sigmaPerp = static_cast<uint8_t>(Quantize(attribute.sggxAxisAndSigmas.z, 0.0f, 1.0f, 256u));
						const uint8_t sigmaParallel = static_cast<uint8_t>(Quantize(attribute.sggxAxisAndSigmas.w, 0.0f, 1.0f, 256u));
						const uint8_t opacity = static_cast<uint8_t>(Quantize(attribute.opacity, 0.0f, 1.0f, 256u));
						const uint16_t uvX = static_cast<uint16_t>(Quantize(attribute.uv.x, uvMin[0], uvMax[0], 65536u));
						const uint16_t uvY = static_cast<uint16_t>(Quantize(attribute.uv.y, uvMin[1], uvMax[1], 65536u));
						AppendBytes(quantizedAttributes, &axisX, sizeof(axisX));
						AppendBytes(quantizedAttributes, &axisY, sizeof(axisY));
						AppendBytes(quantizedAttributes, &sigmaPerp, sizeof(sigmaPerp));
						AppendBytes(quantizedAttributes, &sigmaParallel, sizeof(sigmaParallel));
						AppendBytes(quantizedAttributes, &opacity, sizeof(opacity));
						AppendBytes(quantizedAttributes, &uvX, sizeof(uvX));
						AppendBytes(quantizedAttributes, &uvY, sizeof(uvY));

						maxUvError = std::max(maxUvError, std::abs(static_cast<double>(Dequantize(uvX, uvMin[0], uvMax[0], 65536u)) - attribute.uv.x));
						maxUvError = std::max(maxUvError, std::abs(static_cast<double>(Dequantize(uvY, uvMin[1], uvMax[1], 65536u)) - attribute.uv.y));
						maxSigmaError = std::max(maxSigmaError, std::abs(static_cast<double>(Dequantize(sigmaPerp, 0.0f, 1.0f, 256u)) - attribute.sggxAxisAndSigmas.z));
						maxSigmaError = std::max(maxSigmaError, std::abs(static_cast<double>(Dequantize(sigmaParallel, 0.0f, 1.0f, 256u)) - attribute.sggxAxisAndSigmas.w));
						maxOpacityError = std::max(maxOpacityError, std::abs(static_cast<double>(Dequantize(opacity, 0.0f, 1.0f, 256u)) - attribute.opacity));
					}
				}
				else {
					CLodCache::TrianglePage page;
					if (!CLodCache::TrianglePage::Parse(bytes, page)) {
						continue;
					}
					trianglePageBytes += bytes.size();
					triangleFill.push_back(static_cast<double>(bytes.size()) / kStreamingPageBytes);
					for (const CLodCache::PageStream& stream : page.Streams()) {
						triangleStreams[stream.name] += stream.sizeBytes;
					}
					for (uint32_t meshlet = 0; meshlet < page.MeshletCount(); ++meshlet) {
						triangleCount += page.Meshlet(meshlet).triangleCount;
					}
				}
			}

			const double containerBytes = static_cast<double>(mesh.ContainerBytes());
			sink.Emit("memory.total.bytes", containerBytes + static_cast<double>(mesh.MetadataBytes()), Unit::Bytes, Better::Lower, {}, Rollup::Sum);
			sink.Emit("memory.metadata.bytes", static_cast<double>(mesh.MetadataBytes()), Unit::Bytes, Better::Lower, {}, Rollup::Sum);
			sink.Emit("memory.triangle_pages.bytes", static_cast<double>(trianglePageBytes), Unit::Bytes, Better::Lower, {}, Rollup::Sum);
			sink.Emit("memory.voxel_pages.bytes", static_cast<double>(voxelPageBytes), Unit::Bytes, Better::Lower, {}, Rollup::Sum);
			sink.Emit("memory.triangle_pages.count", static_cast<double>(mesh.Prebuilt().trianglePageCount), Unit::Count, Better::Lower, {}, Rollup::Sum);
			sink.Emit("memory.voxel_pages.count", static_cast<double>(mesh.Prebuilt().voxelPageCount), Unit::Count, Better::Lower, {}, Rollup::Sum);
			if (containerBytes > 0.0) {
				sink.Emit("memory.voxel_share", static_cast<double>(voxelPageBytes) / containerBytes, Unit::Ratio, Better::Lower);
			}
			if (triangleCount != 0u) {
				sink.Emit("memory.triangle_pages.bytes_per_triangle",
					static_cast<double>(trianglePageBytes) / static_cast<double>(triangleCount), Unit::Bytes, Better::Lower);
			}
			for (const auto& [name, bytes] : triangleStreams) {
				sink.Emit("memory.stream.bytes", static_cast<double>(bytes), Unit::Bytes, Better::Lower, { { "kind", "triangle" }, { "stream", name } }, Rollup::Sum);
			}
			for (const auto& [name, bytes] : voxelStreams) {
				sink.Emit("memory.stream.bytes", static_cast<double>(bytes), Unit::Bytes, Better::Lower, { { "kind", "voxel" }, { "stream", name } }, Rollup::Sum);
			}
			EmitDistribution(sink, "memory.page_fill", triangleFill, Unit::Ratio, Better::Higher, { { "kind", "triangle" } });
			EmitDistribution(sink, "memory.page_fill", voxelFill, Unit::Ratio, Better::Higher, { { "kind", "voxel" } });

			if (voxelCells == 0u) {
				return;
			}
			sink.Emit("memory.voxel.clusters", static_cast<double>(voxelClusters), Unit::Count, Better::Lower, {}, Rollup::Sum);
			sink.Emit("memory.voxel.cubes", static_cast<double>(voxelCubes), Unit::Count, Better::Lower, {}, Rollup::Sum);
			sink.Emit("memory.voxel.cells", static_cast<double>(voxelCells), Unit::Count, Better::Lower, {}, Rollup::Sum);
			sink.Emit("memory.voxel.bytes_per_cell", static_cast<double>(voxelPageBytes) / static_cast<double>(voxelCells), Unit::Bytes, Better::Lower);
			EmitDistribution(sink, "memory.voxel.cells_per_cube", cellsPerCube, Unit::Count, Better::Higher);
			EmitDistribution(sink, "memory.voxel.cubes_per_cluster", cubesPerCluster, Unit::Count, Better::Higher);

			// Estimates only: nothing here changes the cache, and the renderer reads attributes uncompressed.
			const auto emitWhatIf = [&](const char* scheme, uint64_t bytes) {
				sink.Emit("memory.whatif.voxel_attributes.bytes", static_cast<double>(bytes), Unit::Bytes, Better::Lower, { { "scheme", scheme } }, Rollup::Sum);
			};
			emitWhatIf("current", rawAttributes.size());
			emitWhatIf("current_lz4", Lz4CompressedBytes(rawAttributes));
			emitWhatIf("quantized_13b", voxelCells * kQuantizedCellBytes);
			emitWhatIf("quantized_13b_lz4", Lz4CompressedBytes(quantizedAttributes));
			sink.Emit("memory.whatif.quantized_13b.max_uv_error", maxUvError, Unit::None, Better::Lower);
			sink.Emit("memory.whatif.quantized_13b.max_sigma_error", maxSigmaError, Unit::None, Better::Lower);
			sink.Emit("memory.whatif.quantized_13b.max_opacity_error", maxOpacityError, Unit::None, Better::Lower);
		}
	};

	class VoxelDistributionAnalyzer final : public IAnalyzer {
	public:
		const AnalyzerInfo& Info() const override
		{
			static const AnalyzerInfo info{ "voxel.distribution", 1, "Voxel widths, predicted on-screen size at the LOD cut, and cell attributes" };
			return info;
		}

		void Run(const AnalysisContext& context, MetricSink& sink) const override
		{
			const CachedMesh& mesh = context.mesh;
			const ClusterLODPrebuiltData& prebuilt = mesh.Prebuilt();
			std::vector<double> widths;
			std::vector<double> representationOverError;
			std::vector<double> predictedFootprint;
			std::vector<double> firstLevelFootprint;
			std::vector<double> opacities;
			std::vector<double> sigmaPerp;
			std::vector<double> sigmaParallel;
			uint64_t groupsWithTerminalSegments = 0;
			uint64_t voxelChildrenOfTriangles = 0;
			int32_t firstVoxelDepth = -1;

			for (uint32_t groupIndex = 0; groupIndex < prebuilt.groups.size(); ++groupIndex) {
				const ClusterLODGroup& group = prebuilt.groups[groupIndex];
				const bool isVoxel = (group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u;
				if (!isVoxel) {
					for (uint32_t segmentIndex = group.firstSegment; segmentIndex < group.firstSegment + group.segmentCount; ++segmentIndex) {
						const int32_t refined = prebuilt.segments[segmentIndex].refinedGroup;
						if (refined >= 0 && (prebuilt.groups[static_cast<size_t>(refined)].flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u) {
							++voxelChildrenOfTriangles;
						}
					}
					continue;
				}
				firstVoxelDepth = firstVoxelDepth < 0 ? group.depth : std::min(firstVoxelDepth, group.depth);

				float groupWidth = 0.0f;
				for (uint32_t segmentIndex = group.firstSegment; segmentIndex < group.firstSegment + group.segmentCount; ++segmentIndex) {
					const ClusterLODGroupSegment& segment = prebuilt.segments[segmentIndex];
					uint32_t meshPage = 0;
					CLodCache::VoxelPage page;
					if (segment.meshletCount == 0u || !CLodCache::ResolveSegmentMeshPage(prebuilt, groupIndex, segment, meshPage) ||
						!CLodCache::VoxelPage::Parse(mesh.Page(meshPage), page)) {
						continue;
					}
					for (uint32_t cluster = 0; cluster < page.ClusterCount(); ++cluster) {
						groupWidth = std::max(groupWidth, page.Cluster(cluster).aabbMinAndVoxelWidth.w);
					}
				}
				if (groupWidth <= 0.0f) {
					continue;
				}
				widths.push_back(groupWidth);
				if (group.bounds.error > 0.0f && group.bounds.error < 1.0e19f) {
					representationOverError.push_back(static_cast<double>(group.representationError) / group.bounds.error);
				}

				// A group draws from the distance where its own error reaches the pixel threshold down to
				// where its children's error drops below it. So it is largest on screen right as it hands
				// over to its children, at width/childError pixels per threshold pixel.
				float maxChildError = 0.0f;
				bool refinesToTriangles = false;
				bool hasTerminal = false;
				for (uint32_t segmentIndex = group.firstSegment; segmentIndex < group.firstSegment + group.segmentCount; ++segmentIndex) {
					const int32_t refined = prebuilt.segments[segmentIndex].refinedGroup;
					if (refined < 0) {
						hasTerminal = true;
						continue;
					}
					const ClusterLODGroup& child = prebuilt.groups[static_cast<size_t>(refined)];
					if (child.bounds.error < 1.0e19f) {
						maxChildError = std::max(maxChildError, child.bounds.error);
					}
					refinesToTriangles = refinesToTriangles || (child.flags & CLOD_GROUP_FLAG_IS_VOXEL) == 0u;
				}
				if (hasTerminal) {
					++groupsWithTerminalSegments;
				}
				else if (maxChildError > 0.0f) {
					const double footprint = static_cast<double>(groupWidth) / maxChildError;
					predictedFootprint.push_back(footprint);
					if (refinesToTriangles) {
						firstLevelFootprint.push_back(footprint);
					}
				}
			}

			for (uint32_t pageIndex = prebuilt.voxelPageBase; pageIndex < prebuilt.voxelPageBase + prebuilt.voxelPageCount; ++pageIndex) {
				CLodCache::VoxelPage page;
				if (!CLodCache::VoxelPage::Parse(mesh.Page(pageIndex), page)) {
					continue;
				}
				for (uint32_t index = 0; index < page.AttributeCount(); ++index) {
					const CLodVoxelAttributeSample attribute = page.Attribute(index);
					opacities.push_back(attribute.opacity);
					sigmaPerp.push_back(attribute.sggxAxisAndSigmas.z);
					sigmaParallel.push_back(attribute.sggxAxisAndSigmas.w);
				}
			}

			if (widths.empty()) {
				return;
			}
			sink.Emit("voxel.first_depth", firstVoxelDepth, Unit::Count, Better::Neutral);
			sink.Emit("voxel.groups_with_terminal_segments", static_cast<double>(groupsWithTerminalSegments), Unit::Count, Better::Lower, {}, Rollup::Sum);
			sink.Emit("voxel.children_of_triangle_groups", static_cast<double>(voxelChildrenOfTriangles), Unit::Count, Better::Lower, {}, Rollup::Sum);
			EmitDistribution(sink, "voxel.width", widths, Unit::MeshUnits, Better::Neutral);
			EmitDistribution(sink, "voxel.representation_over_group_error", representationOverError, Unit::Ratio, Better::Neutral);
			// Edge length only and on the optical axis: real footprints are checked against captures in voxel.footprint.
			EmitDistribution(sink, "voxel.predicted_footprint_px", predictedFootprint, Unit::Pixels, Better::Lower);
			EmitDistribution(sink, "voxel.predicted_footprint_px", firstLevelFootprint, Unit::Pixels, Better::Lower, { { "level", "first" } });
			EmitDistribution(sink, "voxel.cell_opacity", opacities, Unit::Ratio, Better::Neutral);
			EmitDistribution(sink, "voxel.cell_sigma_perp", sigmaPerp, Unit::None, Better::Neutral);
			EmitDistribution(sink, "voxel.cell_sigma_parallel", sigmaParallel, Unit::None, Better::Neutral);
		}
	};
}

std::unique_ptr<IAnalyzer> MakeMemoryPagesAnalyzer() { return std::make_unique<MemoryPagesAnalyzer>(); }
std::unique_ptr<IAnalyzer> MakeVoxelDistributionAnalyzer() { return std::make_unique<VoxelDistributionAnalyzer>(); }

}
