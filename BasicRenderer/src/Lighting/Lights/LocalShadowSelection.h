#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "BasicRenderer/Extensions/ShaderBuffers.h"

// Which spot and point lights get virtual shadow maps, and the cameras of their faces. A face is one perspective
// virtual shadow map of 16K texels with 8 mips: a spot light has one, a point light six (a cube fixed to the world
// axes). Faces own page-table slices in runs (CLodVirtualShadowLocalSlicesPerFace each), so a light's faces are
// consecutive and its first slice names them all (LightInfo::shadowViewInfoIndex).
namespace br::lighting {

	// A spot light whose cone is wider than this is shadowed with a cube, as a point light is: a single perspective
	// face spreads its texels too thinly toward the edges of a wide cone.
	inline constexpr float kLocalShadowMaxSpotHalfAngleRadians = 1.0471976f; // 60 degrees
	inline constexpr uint32_t kLocalShadowCubeFaceCount = 6u;
	// Cube faces see this many texels of their coarsest mip (128 across) past the 90 degree edge on every side. A
	// receiver looks in the face its direction's dominant axis picks, so it is at least this far inside that face, and
	// its filter taps and normal offset (2.5 texels at most) stay in the face's own map: no seams between faces.
	inline constexpr float kLocalShadowCubeGuardTexels = 4.0f;
	inline constexpr float kLocalShadowCoarsestMipTexels = 128.0f;

	uint32_t LocalShadowFaceCount(const LightInfo& light);

	// The near plane of a light's faces. Casters closer to the light than this cast nothing.
	float LocalShadowNearPlane(const LightInfo& light);

	// A spot light's single face looks down the light, its field of view covering the cone. A cube's faces look
	// along +X, -X, +Y, -Y, +Z, -Z in that order. virtualResolution is the faces' mip 0 resolution. lodResolution sets
	// their LOD threshold: the shadowResolution setting, as for the sun's clips, whose geometry detail is likewise set
	// coarser than the 16K they render at.
	std::vector<CameraInfo> BuildLocalShadowFaceCameras(const LightInfo& light, uint32_t virtualResolution, uint32_t lodResolution);

	struct LocalShadowCandidate {
		float score = 0.0f;
		uint32_t faceCount = 0;
		bool selectedLastFrame = false;
	};

	// How much a light that already has faces outscores one that does not, so that two lights of similar score do
	// not trade places every frame.
	inline constexpr float kLocalShadowSelectionHysteresis = 1.25f;

	// Indices of the candidates to shadow: highest score first (selected ones multiplied by hysteresis), skipping any
	// that do not fit in what remains of faceBudget.
	std::vector<uint32_t> SelectLocalShadowLights(
		std::span<const LocalShadowCandidate> candidates, uint32_t faceBudget, float hysteresis = kLocalShadowSelectionHysteresis);

	// The first run of count free faces, marked used; used.size() when there is none.
	uint32_t AllocateLocalShadowFaces(std::span<bool> used, uint32_t count);
	void FreeLocalShadowFaces(std::span<bool> used, uint32_t first, uint32_t count);

	// How much shadowing a light is worth from cameraPosition: its range over its distance, so a light the camera is
	// inside, or a large light nearby, ranks high.
	float LocalShadowScore(const LightInfo& light, const DirectX::XMFLOAT3& cameraPosition);

} // namespace br::lighting
