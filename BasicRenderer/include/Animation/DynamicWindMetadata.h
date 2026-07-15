#pragma once

#include <cstdint>
#include <vector>

enum class DynamicWindSimulationGroupRole : std::uint32_t
{
	Unassigned = 0u,
	Trunk = 1u,
	DetailBranch = 2u,
	AttachedBranch = 3u,
};

struct DynamicWindSimulationGroupData
{
	std::uint32_t flags = 0u;
	DynamicWindSimulationGroupRole role = DynamicWindSimulationGroupRole::Unassigned;
	std::uint32_t profileGroupId = 0xFFFFFFFFu;
	std::uint32_t lastAnimatedLod = 0xFFFFFFFFu;
	float influence = 1.0f;
	float minInfluence = 0.0f;
	float maxInfluence = 0.0f;
	float shiftTop = 0.0f;
};

struct DynamicWindBoneData
{
	std::uint32_t chainOriginBoneIndex = 0xFFFFFFFFu;
	std::uint32_t indexInBoneChain = 0u;
	std::uint32_t chainBoneCount = 0u;
	float chainLength = 0.0f;
};

struct DynamicWindMetadata
{
	static constexpr std::uint32_t GroupFlagDualInfluence = 1u << 0u;
	static constexpr std::uint32_t GroupFlagTrunk = 1u << 1u;

	bool enabled = false;
	bool groundCover = false;
	float gustAttenuation = 0.0f;
	std::uint32_t attachedBranchProfileGroupId = 1u;
	std::vector<DynamicWindSimulationGroupData> groups;
	std::vector<DynamicWindBoneData> bones;
};
