#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include <BasicRenderer/Renderer.h>

#include "ViewerHierarchy.h"

// Says what drew each pixel, by joining one frame's visibility and clusters to the hierarchies on disk, and paints
// that as a false-colour image. Painting on the CPU means a new view needs no shader and no renderer change.
namespace clodcapture {

struct PixelInfo {
	float depth = 0.0f;
	uint32_t cluster = 0;
	uint32_t group = 0;
	int16_t mesh = -1;
	uint8_t primitive = 0;  // triangle, or cube of the voxel cluster
	uint8_t flags = 0;
};
inline constexpr uint8_t kPixelHit = 1u, kPixelVoxel = 2u;

// What the view looked like when the frame was requested. A later change invalidates the frame.
struct ViewStamp {
	uint64_t cameraGeneration = 0;
	uint64_t cutGeneration = 0;
	float pixelWorldPerDepth = 0.0f;  // world size of a pixel at unit view depth
	float threshold = 1.0f;
	float worldScale = 1.0f;
};

struct InspectFrame {
	uint32_t width = 0, height = 0;
	uint64_t frame = 0;
	ViewStamp stamp;
	std::vector<PixelInfo> pixels;
	std::vector<std::byte> clusters;
	uint32_t hitPixels = 0, voxelPixels = 0, unknownPixels = 0;

	float VoxelFraction() const { return hitPixels != 0u ? static_cast<float>(voxelPixels) / static_cast<float>(hitPixels) : 0.0f; }
	const PixelInfo* At(uint32_t x, uint32_t y) const { return x < width && y < height ? &pixels[static_cast<size_t>(y) * width + x] : nullptr; }
};

// The cut a pixel is drawn under, in world units.
inline float PixelCut(const PixelInfo& pixel, const ViewStamp& stamp)
{
	return stamp.threshold * stamp.pixelWorldPerDepth * pixel.depth;
}

// Fields of the packed visible cluster a voxel pixel needs to find its cube.
struct VisibleClusterFields {
	uint32_t instance = 0, localMeshlet = 0, group = 0, voxelPage = 0;
	bool voxel = false;
};
VisibleClusterFields DecodeVisibleCluster(const std::array<uint32_t, 4>& words);

std::shared_ptr<InspectFrame> DecodeInspection(Renderer::CLodInspectionFrame&& input, const HierarchyIndex& hierarchy, const ViewStamp& stamp);

enum class OverlayMode : int {
	Off,
	Path,
	Mesh,
	Group,
	Cluster,
	Primitive,
	Depth,
	VoxelLevel,
	CellPixels,
	Window,
	NextSwitch,
	HandOver,
	Selection,
	Count,
};

struct OverlayInfo {
	std::string_view name;
	std::string_view legend;
};
const OverlayInfo& DescribeOverlay(OverlayMode mode);

struct Selection {
	int32_t mesh = -1;
	int32_t group = -1;
};

// Colours one value in [0, 1] from blue through green to red. For legends too.
uint32_t RampColor(float value);

// Writes width * height RGBA8. Pixels nothing drew stay transparent.
void PaintOverlay(const InspectFrame& frame, const HierarchyIndex& hierarchy, OverlayMode mode, const Selection& selection, float opacity, std::vector<uint32_t>& outPixels);

}
