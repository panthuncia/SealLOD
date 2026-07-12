#pragma once

#include <cstdint>
#include <vector>

struct DynamicWindSimulationGroupData
{
	std::uint32_t flags = 0u;
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
	std::vector<DynamicWindSimulationGroupData> groups;
	std::vector<DynamicWindBoneData> bones;
};
