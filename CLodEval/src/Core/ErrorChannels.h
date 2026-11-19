#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <BasicRenderer/Assets/ClusterLODTypes.h>

// Error channels and the runtime's LOD selection rule (docs/clod-simplification-validation.md, "Stage 3"). The builder stores
// each group's measured geometric and appearance errors in the traversal nodes (ClusterLODTraversalMetric); the runtime
// selects with CLodSelectionError (workGraphCulling.hlsl). This reads them the way MeshManager::AddMesh uploads them and
// applies the same rule, so quality.appearance's cut sweep can evaluate it.
namespace clodeval {

enum class CutGeometricChannel : uint32_t { Quadric = 0u, Measured = 1u, Max = 2u };  // CLOD_LOD_GEOMETRIC_CHANNEL_*

// A selection rule to sweep: the runtime's clodLodGeometricChannel and clodLodAppearanceWeight.
struct CutRule {
	std::string name;
	CutGeometricChannel geometric = CutGeometricChannel::Quadric;
	float appearanceWeight = 0.0f;
};

// Per group, what the runtime would read: the measured geometric and appearance errors, or (present false, or groups no
// segment leaf names) the group's stored error and 0, under which every rule selects as the quadric error does.
struct GroupErrorChannels {
	bool present = false;
	std::vector<float> measured;
	std::vector<float> appearance;
};
GroupErrorChannels ReadGroupErrorChannels(const ClusterLODPrebuiltData& data);

// CLodSelectionError, in the same float arithmetic.
float SelectionError(float quadricError, float measuredError, float appearanceError, const CutRule& rule);

// "quadric", "measured" or "max"; false for anything else.
bool ParseCutGeometricChannel(const std::string& text, CutGeometricChannel& out);
const char* CutGeometricChannelName(CutGeometricChannel channel);

}
