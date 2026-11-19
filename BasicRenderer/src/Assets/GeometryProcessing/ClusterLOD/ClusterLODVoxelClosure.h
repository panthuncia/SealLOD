#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace clod_detail {

// Which groups a voxel pass replaces by voxels (CLOD_GROUP_FLAG_IS_VOXEL). A voxel group may only be refined from voxel
// parents. Seeds and their ancestors (replaceMask) are closed upwards already. Below them, a group that is built
// (buildMask) and has no terminal segment is replaced too, unless one of its DAG parents stays triangles: then it, and
// everything below it, keeps its triangles and only provides its voxel payload to its voxel parents. That parent is a
// sibling that is not a seed, typically a group simplification left terminal (stuck, error FLT_MAX), which can never be
// one. parents[g] lists the groups with a segment refining into g.
inline std::vector<uint8_t> ComputeVoxelReplaceClosure(
	std::span<const std::vector<uint32_t>> parents,
	std::span<const uint8_t> replaceMask,
	std::span<const uint8_t> buildMask,
	std::span<const uint32_t> terminalSegmentCounts,
	uint32_t* keptTriangleGroups = nullptr)
{
	const size_t groupCount = replaceMask.size();
	std::vector<uint8_t> replace(groupCount, 0u);
	for (size_t group = 0; group < groupCount; ++group)
	{
		replace[group] = replaceMask[group] != 0u || (buildMask[group] != 0u && terminalSegmentCounts[group] == 0u) ? 1u : 0u;
	}
	uint32_t kept = 0u;
	for (bool cleared = true; cleared;)
	{
		cleared = false;
		for (size_t group = 0; group < groupCount; ++group)
		{
			if (replace[group] == 0u || replaceMask[group] != 0u)
			{
				continue;
			}
			const std::vector<uint32_t>& groupParents = parents[group];
			if (std::any_of(groupParents.begin(), groupParents.end(), [&](uint32_t parent) { return parent >= groupCount || replace[parent] == 0u; }))
			{
				replace[group] = 0u;
				++kept;
				cleared = true;
			}
		}
	}
	if (keptTriangleGroups != nullptr)
	{
		*keptTriangleGroups = kept;
	}
	return replace;
}

}
