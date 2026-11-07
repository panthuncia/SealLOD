#include "ViewerInspect.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#include <BasicRenderer/Diagnostics/CLodTelemetry.h>
#include <BasicRenderer/Extensions/ShaderBuffers.h>

namespace clodcapture {
	namespace {
		constexpr uint32_t kTriangleBits = 7u, kClusterBits = 26u;
		constexpr uint32_t kReyesClusterBase = 1u << 25u;

		uint32_t Pack(float r, float g, float b, float a)
		{
			const auto channel = [](float value) { return static_cast<uint32_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f); };
			return channel(r) | (channel(g) << 8u) | (channel(b) << 16u) | (channel(a) << 24u);
		}

		uint32_t HashColor(uint32_t value, uint32_t alpha)
		{
			value = (value ^ 0x9E3779B9u) * 0x85EBCA6Bu;
			value ^= value >> 13u;
			value *= 0xC2B2AE35u;
			value ^= value >> 16u;
			const uint32_t r = 64u + (value & 0xFFu) * 191u / 255u;
			const uint32_t g = 64u + ((value >> 8u) & 0xFFu) * 191u / 255u;
			const uint32_t b = 64u + ((value >> 16u) & 0xFFu) * 191u / 255u;
			return r | (g << 8u) | (b << 16u) | (alpha << 24u);
		}

		// What a coloured mode paints where it has nothing to say, so the shape stays readable without competing.
		uint32_t Muted(const PixelInfo& pixel, uint32_t alpha)
		{
			const uint32_t tint = (pixel.flags & kPixelVoxel) != 0u ? 0x505050u : 0x383838u;
			return tint | (alpha << 24u);
		}

		float SafeLog2(float value)
		{
			return std::log2(std::max(value, 1.0e-12f));
		}

		uint32_t WithAlpha(uint32_t color, uint32_t alpha)
		{
			return (color & 0x00FFFFFFu) | (alpha << 24u);
		}
	}

	VisibleClusterFields DecodeVisibleCluster(const std::array<uint32_t, 4>& words)
	{
		VisibleClusterFields fields;
		fields.instance = words[0] >> 8u;
		fields.localMeshlet = words[1] & 0x3FFFu;
		// Global group index, 21 bits (visibleClusterPacking.hlsli, CLodVisibleClusterGroupID).
		fields.group = ((words[1] >> 14u) & 0x3FFFFu) | ((words[2] & 0x3u) << 18u) | (((words[3] >> 30u) & 0x1u) << 20u);
		fields.voxelPage = (words[2] >> 2u) & 0xFFFFFu;
		fields.voxel = (words[3] & 0x80000000u) != 0u;
		return fields;
	}

	std::shared_ptr<InspectFrame> DecodeInspection(Renderer::CLodInspectionFrame&& input, const HierarchyIndex& hierarchy, const ViewStamp& stamp)
	{
		auto frame = std::make_shared<InspectFrame>();
		frame->width = input.width;
		frame->height = input.height;
		frame->frame = input.frame;
		frame->stamp = stamp;
		frame->pixels.resize(input.visibility.size());
		frame->clusters = std::move(input.visibleClusters);

		const size_t clusterCount = frame->clusters.size() / (sizeof(uint32_t) * 4u);
		const size_t metadataCount = input.meshMetadata->size() / sizeof(CLodMeshMetadata);

		// Group indices are global, and a cluster drawn through a portal belongs to the part, not to the draw's mesh. So
		// the mesh is the registered one whose group range holds the group: ranges sorted by base, each as long as the
		// cache says.
		struct GroupRange {
			uint32_t base = 0;
			int32_t mesh = -1;
		};
		std::vector<GroupRange> ranges;
		if (hierarchy.Ready()) {
			std::scoped_lock lock(g_clodMeshSources.mutex);
			for (size_t row = 0; row < metadataCount; ++row) {
				CLodMeshMetadata metadata{};
				std::memcpy(&metadata, input.meshMetadata->data() + row * sizeof(CLodMeshMetadata), sizeof(metadata));
				const uint64_t identity = (static_cast<uint64_t>(metadata.meshIdentityHi) << 32u) | metadata.meshIdentityLo;
				const auto known = g_clodMeshSources.meshes.find(identity);
				if (known == g_clodMeshSources.meshes.end()) {
					continue;
				}
				const int32_t mesh = hierarchy.Find(known->second.cacheSourceIdentifier, known->second.cachePrimPath, known->second.cacheSubsetName);
				if (mesh >= 0) {
					ranges.push_back({ metadata.groupsBase, mesh });
				}
			}
		}
		std::ranges::sort(ranges, {}, &GroupRange::base);
		const auto resolveGroup = [&](uint32_t globalGroup, uint32_t& outLocal) -> int32_t {
			const auto after = std::ranges::upper_bound(ranges, globalGroup, {}, &GroupRange::base);
			if (after == ranges.begin()) {
				return -1;
			}
			const GroupRange& range = *std::prev(after);
			outLocal = globalGroup - range.base;
			return outLocal < hierarchy.Meshes()[range.mesh].groups.size() ? range.mesh : -1;
		};

		for (size_t index = 0; index < input.visibility.size(); ++index) {
			const uint64_t key = input.visibility[index];
			if (key == ~uint64_t{ 0 }) {
				continue;
			}
			PixelInfo& pixel = frame->pixels[index];
			pixel.primitive = static_cast<uint8_t>(key & ((1u << kTriangleBits) - 1u));
			pixel.cluster = static_cast<uint32_t>((key >> kTriangleBits) & ((1u << kClusterBits) - 1u));
			const uint32_t depthBits = static_cast<uint32_t>(key >> (kTriangleBits + kClusterBits)) << 1u;
			std::memcpy(&pixel.depth, &depthBits, sizeof(pixel.depth));
			pixel.flags = kPixelHit;
			++frame->hitPixels;
			if (pixel.cluster >= clusterCount || pixel.cluster >= kReyesClusterBase) {
				++frame->unknownPixels;
				continue;
			}
			std::array<uint32_t, 4> words{};
			std::memcpy(words.data(), frame->clusters.data() + pixel.cluster * sizeof(words), sizeof(words));
			const VisibleClusterFields fields = DecodeVisibleCluster(words);
			if (fields.voxel) {
				pixel.flags |= kPixelVoxel;
				++frame->voxelPixels;
			}
			uint32_t localGroup = 0;
			const int32_t mesh = resolveGroup(fields.group, localGroup);
			pixel.group = localGroup;
			pixel.mesh = static_cast<int16_t>(mesh);
			if (mesh < 0) {
				pixel.mesh = -1;
				++frame->unknownPixels;
			}
		}
		return frame;
	}

	const OverlayInfo& DescribeOverlay(OverlayMode mode)
	{
		static const OverlayInfo infos[] = {
			{ "Off", "" },
			{ "Path: triangle or voxel", "Red: voxels. Green: triangles." },
			{ "Mesh", "One colour per cached mesh. Shows which of parts and assemblies is drawing." },
			{ "Group", "One colour per hierarchy group." },
			{ "Cluster", "One colour per visible cluster." },
			{ "Triangle or cube", "One colour per triangle, or per voxel cube. Cube edges are the 4x4x4 cell blocks." },
			{ "LOD depth", "Blue: leaves (depth 0). Red: the deepest level in any mesh." },
			{ "Voxel level", "Grey: triangles. Blue: first voxel group above triangles. Red: most voxel steps above them." },
			{ "Cell size in pixels", "Voxel width over world pixel, on log2. Blue: 1/8 px. Green: 1 px. Red: 8 px. Grey: triangles." },
			{ "Position in the group's window", "How far the cut is through the range this group draws in. Blue: it just appeared. Red: it is about to go." },
			{ "Cut multiple to next switch", "How much the cut has to grow to replace this group. Red: right now. Blue: 16x or more." },
			{ "Cut where this became voxels", "The hand-over error of each pixel's part of the mesh, on log scale across all meshes. Blue: finest. Red: coarsest. Grey: never." },
			{ "Merge: node level or part", "Merge roots. Node geometry by merge level, blue: over leaves, red: the root. White: a part opened through a portal. Grey: not a merge root." },
			{ "Selection", "Yellow: selected group. Cyan: its children. Magenta: its parents. Grey: rest of that mesh." },
		};
		return infos[static_cast<int>(mode)];
	}

	uint32_t RampColor(float value)
	{
		// Turbo, as Google's polynomial approximation.
		const float x = std::clamp(value, 0.0f, 1.0f);
		const float x2 = x * x, x3 = x2 * x, x4 = x3 * x, x5 = x4 * x;
		const float r = 0.13572138f + 4.61539260f * x - 42.66032258f * x2 + 132.13108234f * x3 - 152.94239396f * x4 + 59.28637943f * x5;
		const float g = 0.09140261f + 2.19418839f * x + 4.84296658f * x2 - 14.18503333f * x3 + 4.27729857f * x4 + 2.82956604f * x5;
		const float b = 0.10667330f + 12.64194608f * x - 60.58204836f * x2 + 110.36276771f * x3 - 89.90310912f * x4 + 27.34824973f * x5;
		return Pack(r, g, b, 1.0f);
	}

	void PaintOverlay(const InspectFrame& frame, const HierarchyIndex& hierarchy, OverlayMode mode, const Selection& selection, float opacity, std::vector<uint32_t>& outPixels)
	{
		outPixels.assign(frame.pixels.size(), 0u);
		const uint32_t alpha = static_cast<uint32_t>(std::clamp(opacity, 0.0f, 1.0f) * 255.0f + 0.5f);
		const std::vector<MeshInfo>& meshes = hierarchy.Meshes();
		const float maxDepth = static_cast<float>(std::max(hierarchy.MaxDepth(), 1));
		const float maxVoxelLevel = static_cast<float>(std::max(hierarchy.MaxVoxelLevel(), 1));
		const float handOverLow = SafeLog2(hierarchy.MinHandOver() * frame.stamp.worldScale);
		const float handOverHigh = SafeLog2(hierarchy.MaxHandOver() * frame.stamp.worldScale);
		const float worldScale = frame.stamp.worldScale;

		std::vector<uint32_t> selectedChildren, selectedParents;
		if (selection.mesh >= 0 && static_cast<size_t>(selection.mesh) < meshes.size() && selection.group >= 0) {
			selectedChildren = meshes[selection.mesh].Children(static_cast<uint32_t>(selection.group));
			selectedParents = meshes[selection.mesh].Parents(static_cast<uint32_t>(selection.group));
		}

		for (size_t index = 0; index < frame.pixels.size(); ++index) {
			const PixelInfo& pixel = frame.pixels[index];
			if ((pixel.flags & kPixelHit) == 0u) {
				continue;
			}
			const bool voxel = (pixel.flags & kPixelVoxel) != 0u;
			uint32_t& out = outPixels[index];
			if (mode == OverlayMode::Path) {
				out = voxel ? Pack(0.95f, 0.15f, 0.15f, opacity) : Pack(0.10f, 0.95f, 0.20f, opacity);
				continue;
			}
			if (mode == OverlayMode::Cluster) {
				out = HashColor(pixel.cluster, alpha);
				continue;
			}
			if (mode == OverlayMode::Primitive) {
				out = HashColor(pixel.cluster * 131u + pixel.primitive, alpha);
				continue;
			}
			if (pixel.mesh < 0) {
				out = 0xFF00FFu | (alpha << 24u);  // nothing on disk describes it, which should never go unnoticed
				continue;
			}
			const MeshInfo& mesh = meshes[pixel.mesh];
			const GroupInfo& group = mesh.groups[pixel.group];
			const float cut = PixelCut(pixel, frame.stamp);
			switch (mode) {
			case OverlayMode::Mesh:
				out = HashColor(static_cast<uint32_t>(pixel.mesh) * 977u, alpha);
				break;
			case OverlayMode::Group:
				out = HashColor(static_cast<uint32_t>(pixel.mesh) * 7919u + pixel.group, alpha);
				break;
			case OverlayMode::Depth:
				out = WithAlpha(RampColor(static_cast<float>(group.depth) / maxDepth), alpha);
				break;
			case OverlayMode::VoxelLevel:
				out = group.voxel ? WithAlpha(RampColor(static_cast<float>(group.voxelLevel) / maxVoxelLevel), alpha) : Muted(pixel, alpha);
				break;
			case OverlayMode::CellPixels: {
				if (!group.voxel || pixel.depth <= 0.0f) {
					out = Muted(pixel, alpha);
					break;
				}
				const float pixels = group.voxelWidth * worldScale / (frame.stamp.pixelWorldPerDepth * pixel.depth);
				out = WithAlpha(RampColor((SafeLog2(pixels) + 3.0f) / 6.0f), alpha);
				break;
			}
			case OverlayMode::Window: {
				const float enter = group.enterError * worldScale;
				const float leave = group.leaveError * worldScale;
				if (!std::isfinite(leave) || leave <= 0.0f) {
					out = Muted(pixel, alpha);
				}
				else if (enter <= 0.0f) {
					// Leaves have no lower end, so measure the last factor of 16 before they go.
					out = WithAlpha(RampColor(1.0f - std::clamp(SafeLog2(leave / std::max(cut, 1.0e-12f)) / 4.0f, 0.0f, 1.0f)), alpha);
				}
				else {
					const float span = std::max(SafeLog2(leave) - SafeLog2(enter), 1.0e-3f);
					out = WithAlpha(RampColor((SafeLog2(cut) - SafeLog2(enter)) / span), alpha);
				}
				break;
			}
			case OverlayMode::NextSwitch: {
				const float leave = group.leaveError * worldScale;
				out = std::isfinite(leave) ? WithAlpha(RampColor(1.0f - std::clamp(SafeLog2(leave / std::max(cut, 1.0e-12f)) / 4.0f, 0.0f, 1.0f)), alpha) : Muted(pixel, alpha);
				break;
			}
			case OverlayMode::HandOver: {
				const float error = group.firstVoxelError * worldScale;
				out = std::isfinite(error) && handOverHigh > handOverLow ? WithAlpha(RampColor((SafeLog2(error) - handOverLow) / (handOverHigh - handOverLow)), alpha) : Muted(pixel, alpha);
				break;
			}
			case OverlayMode::Merge: {
				if (group.mergeNode >= 0 && mesh.merge) {
					const float level = static_cast<float>(mesh.merge->nodes[static_cast<uint32_t>(group.mergeNode)].level);
					out = WithAlpha(RampColor((level - 1.0f) / std::max(static_cast<float>(hierarchy.MaxMergeLevel()) - 1.0f, 1.0f)), alpha);
				}
				else if (!mesh.portalUse.empty()) {
					out = voxel ? Pack(0.75f, 0.75f, 0.75f, opacity) : Pack(0.95f, 0.95f, 0.95f, opacity);
				}
				else {
					out = Muted(pixel, alpha);
				}
				break;
			}
			case OverlayMode::Selection: {
				if (pixel.mesh != selection.mesh) {
					out = 0x202020u | (alpha << 24u);
				}
				else if (static_cast<int32_t>(pixel.group) == selection.group) {
					out = Pack(1.0f, 0.9f, 0.1f, opacity);
				}
				else if (std::ranges::find(selectedChildren, pixel.group) != selectedChildren.end()) {
					out = Pack(0.1f, 0.9f, 1.0f, opacity);
				}
				else if (std::ranges::find(selectedParents, pixel.group) != selectedParents.end()) {
					out = Pack(1.0f, 0.2f, 0.9f, opacity);
				}
				else {
					out = 0x606060u | (alpha << 24u);
				}
				break;
			}
			default:
				break;
			}
		}
	}

}
