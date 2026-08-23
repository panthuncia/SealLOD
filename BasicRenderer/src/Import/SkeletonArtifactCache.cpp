#include "Import/SkeletonArtifactCache.h"

#include "Animation/Skeleton.h"
#include "Mesh/ClusterLODTypes.h"
#include "ShaderBuffers.h"
#include "Utilities/CachePathUtilities.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <queue>
#include <sstream>
#include <unordered_map>

#include <bcrypt.h>
#include <lz4.h>
#include <spdlog/spdlog.h>
#include <windows.h>

namespace {

constexpr auto kMagic = SKELETON_ARTIFACT_MAGIC;
constexpr std::uint32_t kInvalidGroup = 0xFFFFFFFFu;
constexpr std::uint32_t kWindFlagTrunk = 1u << 0u;
constexpr std::size_t kArtifactSectionCount = 13u;

struct ArtifactHeader
{
	std::array<char, 8> magic = kMagic;
	std::uint32_t schemaVersion = SKELETON_ARTIFACT_SCHEMA_VERSION;
	std::uint32_t headerBytes = sizeof(ArtifactHeader);
	std::uint64_t uncompressedBytes = 0;
	std::uint64_t compressedBytes = 0;
	std::uint32_t jointCount = 0;
	std::uint32_t groupCount = 0;
	std::array<std::uint8_t, 32> digest{};
	std::array<std::uint64_t, kArtifactSectionCount> sectionOffsets{};
};

template<class T> void WritePod(std::vector<std::byte>& out, const T& value)
{
	const auto* begin = reinterpret_cast<const std::byte*>(&value);
	out.insert(out.end(), begin, begin + sizeof(T));
}

template<class T> bool ReadPod(const std::vector<std::byte>& in, std::size_t& offset, T& value)
{
	if (offset + sizeof(T) > in.size()) return false;
	std::memcpy(&value, in.data() + offset, sizeof(T));
	offset += sizeof(T);
	return true;
}

template<class T> void WriteVector(std::vector<std::byte>& out, const std::vector<T>& values)
{
	WritePod(out, static_cast<std::uint64_t>(values.size()));
	if (!values.empty()) {
		const auto* begin = reinterpret_cast<const std::byte*>(values.data());
		out.insert(out.end(), begin, begin + values.size() * sizeof(T));
	}
}

template<class T> bool ReadVector(const std::vector<std::byte>& in, std::size_t& offset, std::vector<T>& values)
{
	std::uint64_t count = 0;
	if (!ReadPod(in, offset, count) || count > (std::numeric_limits<std::size_t>::max)() / sizeof(T)) return false;
	const std::size_t bytes = static_cast<std::size_t>(count) * sizeof(T);
	if (offset + bytes > in.size()) return false;
	values.resize(static_cast<std::size_t>(count));
	if (bytes != 0) std::memcpy(values.data(), in.data() + offset, bytes);
	offset += bytes;
	return true;
}

void WriteString(std::vector<std::byte>& out, const std::string& value)
{
	WritePod(out, static_cast<std::uint64_t>(value.size()));
	const auto* begin = reinterpret_cast<const std::byte*>(value.data());
	out.insert(out.end(), begin, begin + value.size());
}

bool ReadString(const std::vector<std::byte>& in, std::size_t& offset, std::string& value)
{
	std::uint64_t count = 0;
	if (!ReadPod(in, offset, count) || count > in.size() - offset) return false;
	value.assign(reinterpret_cast<const char*>(in.data() + offset), static_cast<std::size_t>(count));
	offset += static_cast<std::size_t>(count);
	return true;
}

void WriteStrings(std::vector<std::byte>& out, const std::vector<std::string>& values)
{
	WritePod(out, static_cast<std::uint64_t>(values.size()));
	for (const auto& value : values) WriteString(out, value);
}

bool ReadStrings(const std::vector<std::byte>& in, std::size_t& offset, std::vector<std::string>& values)
{
	std::uint64_t count = 0;
	if (!ReadPod(in, offset, count) || count > 10'000'000u) return false;
	values.resize(static_cast<std::size_t>(count));
	for (auto& value : values) if (!ReadString(in, offset, value)) return false;
	return true;
}

void WriteLodVariants(std::vector<std::byte>& out, const std::vector<SkeletonLodVariant>& variants)
{
	WritePod(out, static_cast<std::uint64_t>(variants.size()));
	for (const auto& variant : variants) {
		WritePod(out, variant.level);
		WritePod(out, variant.targetBoneCount);
		WritePod(out, variant.animatedBoneCount);
		WritePod(out, variant.normalizedQuality);
		WritePod(out, variant.collapseError);
		WritePod(out, variant.semanticBoneCounts);
		WriteVector(out, variant.parentIndices);
		WriteVector(out, variant.evaluationOrder);
		WriteVector(out, variant.restLocalMatrices);
		WriteVector(out, variant.inverseBindMatrices);
		WriteVector(out, variant.bindGlobalMatrices);
		WriteVector(out, variant.windSimulationGroupIndices);
		WriteVector(out, variant.windBoneInvariants);
		WriteVector(out, variant.windBones);
		WriteVector(out, variant.windResponseScales);
		WriteVector(out, variant.baseToLodBone);
		WriteVector(out, variant.lodToBaseBone);
	}
}

bool ReadLodVariants(const std::vector<std::byte>& in, std::size_t& offset, std::vector<SkeletonLodVariant>& variants)
{
	std::uint64_t count = 0;
	if (!ReadPod(in, offset, count) || count > 16u) return false;
	variants.resize(static_cast<std::size_t>(count));
	for (auto& variant : variants) {
		if (!ReadPod(in, offset, variant.level) || !ReadPod(in, offset, variant.targetBoneCount) ||
			!ReadPod(in, offset, variant.animatedBoneCount) || !ReadPod(in, offset, variant.normalizedQuality) ||
			!ReadPod(in, offset, variant.collapseError) || !ReadPod(in, offset, variant.semanticBoneCounts) ||
			!ReadVector(in, offset, variant.parentIndices) ||
			!ReadVector(in, offset, variant.evaluationOrder) || !ReadVector(in, offset, variant.restLocalMatrices) ||
			!ReadVector(in, offset, variant.inverseBindMatrices) || !ReadVector(in, offset, variant.bindGlobalMatrices) ||
			!ReadVector(in, offset, variant.windSimulationGroupIndices) || !ReadVector(in, offset, variant.windBoneInvariants) ||
			!ReadVector(in, offset, variant.windBones) || !ReadVector(in, offset, variant.windResponseScales) ||
			!ReadVector(in, offset, variant.baseToLodBone) || !ReadVector(in, offset, variant.lodToBaseBone)) return false;
	}
	return true;
}

std::string NormalizeProfileIdentity(std::string value)
{
	std::ranges::replace(value, '\\', '/');
	std::ranges::transform(value, value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	constexpr std::string_view marker = "sarpoverrideassets/";
	if (const auto markerPos = value.find(marker); markerPos != std::string::npos) value.erase(0, markerPos);
	return value;
}

std::array<std::uint8_t, 32> Sha256(std::span<const std::byte> bytes)
{
	std::array<std::uint8_t, 32> result{};
	BCRYPT_ALG_HANDLE algorithm = nullptr;
	BCRYPT_HASH_HANDLE hash = nullptr;
	DWORD objectBytes = 0;
	DWORD copied = 0;
	if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
		BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &copied, 0) < 0) {
		if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
		return result;
	}
	std::vector<std::uint8_t> object(objectBytes);
	if (BCryptCreateHash(algorithm, &hash, object.data(), objectBytes, nullptr, 0, 0) >= 0) {
		BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())), static_cast<ULONG>(bytes.size()), 0);
		BCryptFinishHash(hash, result.data(), static_cast<ULONG>(result.size()), 0);
	}
	if (hash) BCryptDestroyHash(hash);
	BCryptCloseAlgorithmProvider(algorithm, 0);
	return result;
}

std::array<float, 3> Subtract(const std::array<float, 3>& a, const std::array<float, 3>& b)
{
	return { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
}

std::array<float, 3> Cross(const std::array<float, 3>& a, const std::array<float, 3>& b)
{
	return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
}

float LengthSquared(const std::array<float, 3>& value)
{
	return value[0] * value[0] + value[1] * value[1] + value[2] * value[2];
}

std::array<float, 3> Normalize(const std::array<float, 3>& value, const std::array<float, 3>& fallback)
{
	const float lengthSquared = LengthSquared(value);
	if (!(lengthSquared > 1.0e-12f) || !std::isfinite(lengthSquared)) return fallback;
	const float inverseLength = 1.0f / std::sqrt(lengthSquared);
	return { value[0] * inverseLength, value[1] * inverseLength, value[2] * inverseLength };
}

SkeletonLodVariant BuildLodVariant(const SkeletonArtifactData& source, std::uint32_t level, const std::vector<bool>& keep)
{
	SkeletonLodVariant result;
	result.level = level;
	const std::uint32_t count = static_cast<std::uint32_t>(source.jointNames.size());
	result.baseToLodBone.assign(count, 0xFFFFFFFFu);
	for (std::uint32_t bone : source.evaluationOrder) {
		if (bone < keep.size() && keep[bone]) {
			result.baseToLodBone[bone] = static_cast<std::uint32_t>(result.lodToBaseBone.size());
			result.lodToBaseBone.push_back(bone);
		}
	}
	const std::uint32_t fallback = result.lodToBaseBone.empty() ? 0xFFFFFFFFu : 0u;
	for (std::uint32_t bone : source.evaluationOrder) {
		if (result.baseToLodBone[bone] != 0xFFFFFFFFu) continue;
		std::int32_t ancestor = source.parentIndices[bone];
		while (ancestor >= 0 && result.baseToLodBone[static_cast<std::uint32_t>(ancestor)] == 0xFFFFFFFFu)
			ancestor = source.parentIndices[static_cast<std::uint32_t>(ancestor)];
		result.baseToLodBone[bone] = ancestor >= 0 ? result.baseToLodBone[static_cast<std::uint32_t>(ancestor)] : fallback;
	}
	result.windResponseScales.assign(result.lodToBaseBone.size(), 1.0f);
	std::vector<std::uint32_t> contributorCounts(result.lodToBaseBone.size(), 0u);
	for (std::uint32_t base = 0; base < count; ++base) {
		const std::uint32_t compact = result.baseToLodBone[base];
		if (compact >= result.lodToBaseBone.size()) continue;
		const std::uint32_t retainedBase = result.lodToBaseBone[compact];
		if (source.windSimulationGroupIndices[base] == source.windSimulationGroupIndices[retainedBase])
			++contributorCounts[compact];
	}
	for (std::uint32_t compact = 0; compact < result.lodToBaseBone.size(); ++compact) {
		const std::uint32_t base = result.lodToBaseBone[compact];
		std::int32_t parent = source.parentIndices[base];
		while (parent >= 0 && result.baseToLodBone[static_cast<std::uint32_t>(parent)] == compact)
			parent = source.parentIndices[static_cast<std::uint32_t>(parent)];
		const std::int32_t compactParent = parent >= 0 ? static_cast<std::int32_t>(result.baseToLodBone[static_cast<std::uint32_t>(parent)]) : -1;
		result.parentIndices.push_back(compactParent);
		result.evaluationOrder.push_back(compact);
		result.inverseBindMatrices.push_back(source.inverseBindMatrices[base]);
		result.bindGlobalMatrices.push_back(source.bindGlobalMatrices[base]);
		DirectX::XMMATRIX local = DirectX::XMLoadFloat4x4(&source.bindGlobalMatrices[base]);
		if (compactParent >= 0) local = DirectX::XMMatrixMultiply(local,
			DirectX::XMMatrixInverse(nullptr, DirectX::XMLoadFloat4x4(&result.bindGlobalMatrices[static_cast<std::uint32_t>(compactParent)])));
		DirectX::XMFLOAT4X4 localStored;
		DirectX::XMStoreFloat4x4(&localStored, local);
		result.restLocalMatrices.push_back(localStored);
		result.windSimulationGroupIndices.push_back(source.windSimulationGroupIndices[base]);
		result.windBoneInvariants.push_back(source.windBoneInvariants[base]);
		DynamicWindBoneData windBone = source.dynamicWindMetadata.bones[base];
		if (windBone.chainOriginBoneIndex < result.baseToLodBone.size())
			windBone.chainOriginBoneIndex = result.baseToLodBone[windBone.chainOriginBoneIndex];
		result.windBones.push_back(windBone);
		result.windResponseScales[compact] = static_cast<float>((std::max)(1u, contributorCounts[compact]));
	}
	result.targetBoneCount = static_cast<std::uint32_t>(result.lodToBaseBone.size());
	for (const auto base : result.lodToBaseBone) {
		const auto groupIndex = source.windSimulationGroupIndices[base];
		if (groupIndex == kInvalidGroup || groupIndex >= source.dynamicWindMetadata.groups.size()) continue;
		++result.animatedBoneCount;
		const auto role = static_cast<std::uint32_t>(source.dynamicWindMetadata.groups[groupIndex].role);
		if (role < result.semanticBoneCounts.size()) ++result.semanticBoneCounts[role];
	}
	for (std::uint32_t base = 0; base < count; ++base) {
		const auto compact = result.baseToLodBone[base];
		if (compact >= result.lodToBaseBone.size()) continue;
		const auto retained = result.lodToBaseBone[compact];
		const auto& a = source.bindGlobalMatrices[base];
		const auto& b = source.bindGlobalMatrices[retained];
		const float dx = a._41 - b._41, dy = a._42 - b._42, dz = a._43 - b._43;
		result.collapseError = (std::max)(result.collapseError, std::sqrt(dx * dx + dy * dy + dz * dz));
	}
	return result;
}

#if 0 // Replaced by the variable-length, error-ranked generator below.
void BuildSkeletonLodsLegacy(SkeletonArtifactData& data)
{
	const std::uint32_t count = static_cast<std::uint32_t>(data.jointNames.size());
	if (count == 0u) return;
	for (std::uint32_t groupIndex = 0; groupIndex < data.dynamicWindMetadata.groups.size(); ++groupIndex) {
		auto& group = data.dynamicWindMetadata.groups[groupIndex];
		if (group.profileGroupId == 0xFFFFFFFFu) group.profileGroupId = groupIndex;
		if (group.role == DynamicWindSimulationGroupRole::Unassigned) {
			group.role = (group.flags & DynamicWindMetadata::GroupFlagTrunk) != 0u
				? DynamicWindSimulationGroupRole::Trunk : DynamicWindSimulationGroupRole::DetailBranch;
		}
		if (group.lastAnimatedLod == 0xFFFFFFFFu)
			group.lastAnimatedLod = group.role == DynamicWindSimulationGroupRole::Trunk ? 5u :
				(group.role == DynamicWindSimulationGroupRole::AttachedBranch ? 0u : 1u);
	}
	auto roleForBone = [&](std::uint32_t bone) {
		const auto group = data.windSimulationGroupIndices[bone];
		return group < data.dynamicWindMetadata.groups.size()
			? data.dynamicWindMetadata.groups[group].role : DynamicWindSimulationGroupRole::Unassigned;
	};
	auto lastAnimatedLodForBone = [&](std::uint32_t bone) {
		const auto group = data.windSimulationGroupIndices[bone];
		return group < data.dynamicWindMetadata.groups.size() ? data.dynamicWindMetadata.groups[group].lastAnimatedLod : 5u;
	};
	std::vector<bool> lod0(count, true);
	std::vector<bool> lod1(count, true);
	std::vector<bool> lod2(count, false);
	for (std::uint32_t bone = 0; bone < count; ++bone) {
		lod1[bone] = lastAnimatedLodForBone(bone) >= 1u;
		lod2[bone] = lastAnimatedLodForBone(bone) >= 2u || data.parentIndices[bone] < 0;
	}
	for (std::uint32_t bone = 0; bone < count; ++bone) if (lod2[bone]) {
		for (std::int32_t parent = data.parentIndices[bone]; parent >= 0; parent = data.parentIndices[static_cast<std::uint32_t>(parent)])
			lod2[static_cast<std::uint32_t>(parent)] = true;
	}
	std::vector<std::uint32_t> retainedChildren(count, 0u);
	for (std::uint32_t bone = 0; bone < count; ++bone) if (lod2[bone] && data.parentIndices[bone] >= 0 && lod2[static_cast<std::uint32_t>(data.parentIndices[bone])])
		++retainedChildren[static_cast<std::uint32_t>(data.parentIndices[bone])];
	auto reduced = [&](std::uint32_t stride, const std::vector<bool>& parentKeep) {
		std::vector<bool> keep(count, false);
		for (std::uint32_t bone = 0; bone < count; ++bone) if (parentKeep[bone]) {
			const bool mandatory = data.parentIndices[bone] < 0 || retainedChildren[bone] != 1u;
			const auto& wind = data.dynamicWindMetadata.bones[bone];
			keep[bone] = mandatory || wind.indexInBoneChain % stride == 0u;
		}
		return keep;
	};
	std::vector<bool> lod3 = reduced(2u, lod2);
	std::vector<bool> lod4 = reduced(4u, lod3);
	std::vector<bool> lod5(count, false);
	for (std::uint32_t bone = 0; bone < count; ++bone) if (data.parentIndices[bone] < 0) lod5[bone] = true;
	std::vector<std::uint32_t> trunk;
	for (std::uint32_t bone : data.evaluationOrder) if (roleForBone(bone) == DynamicWindSimulationGroupRole::Trunk) trunk.push_back(bone);
	if (!trunk.empty()) {
		lod5[trunk.front()] = true;
		std::vector<float> trunkArc(count, 0.0f);
		float maximumTrunkArc = 0.0f;
		for (const auto bone : data.evaluationOrder) {
			if (roleForBone(bone) != DynamicWindSimulationGroupRole::Trunk) continue;
			const auto parent = data.parentIndices[bone];
			if (parent >= 0 && roleForBone(static_cast<std::uint32_t>(parent)) == DynamicWindSimulationGroupRole::Trunk) {
				const auto& childBind = data.bindGlobalMatrices[bone];
				const auto& parentBind = data.bindGlobalMatrices[static_cast<std::uint32_t>(parent)];
				const float dx = childBind._41 - parentBind._41;
				const float dy = childBind._42 - parentBind._42;
				const float dz = childBind._43 - parentBind._43;
				trunkArc[bone] = trunkArc[static_cast<std::uint32_t>(parent)] + std::sqrt(dx * dx + dy * dy + dz * dz);
			}
			maximumTrunkArc = (std::max)(maximumTrunkArc, trunkArc[bone]);
		}

		float crownPosition = 0.75f;
		if (maximumTrunkArc > 0.0f) {
			std::vector<float> attachmentPositions;
			for (std::uint32_t bone = 0; bone < count; ++bone) {
				const auto role = roleForBone(bone);
				if (role != DynamicWindSimulationGroupRole::DetailBranch && role != DynamicWindSimulationGroupRole::AttachedBranch) continue;
				const auto parent = data.parentIndices[bone];
				if (parent >= 0 && roleForBone(static_cast<std::uint32_t>(parent)) == role) continue;
				for (auto ancestor = parent; ancestor >= 0; ancestor = data.parentIndices[static_cast<std::uint32_t>(ancestor)]) {
					if (roleForBone(static_cast<std::uint32_t>(ancestor)) == DynamicWindSimulationGroupRole::Trunk) {
						attachmentPositions.push_back(trunkArc[static_cast<std::uint32_t>(ancestor)] / maximumTrunkArc);
						break;
					}
				}
			}
			if (!attachmentPositions.empty()) {
				const auto middle = attachmentPositions.begin() + attachmentPositions.size() / 2u;
				std::nth_element(attachmentPositions.begin(), middle, attachmentPositions.end());
				crownPosition = *middle;
			}
		}
		crownPosition = std::clamp(crownPosition, 0.60f, 0.85f);
		const auto crown = (std::min_element)(trunk.begin(), trunk.end(), [&](const auto left, const auto right) {
			const float leftPosition = maximumTrunkArc > 0.0f ? trunkArc[left] / maximumTrunkArc : 0.0f;
			const float rightPosition = maximumTrunkArc > 0.0f ? trunkArc[right] / maximumTrunkArc : 0.0f;
			return std::abs(leftPosition - crownPosition) < std::abs(rightPosition - crownPosition);
		});
		lod5[*crown] = true;
	}
	data.lodVariants.clear();
	data.lodVariants.push_back(BuildLodVariant(data, 0u, lod0));
	data.lodVariants.push_back(BuildLodVariant(data, 1u, lod1));
	data.lodVariants.push_back(BuildLodVariant(data, 2u, lod2));
	data.lodVariants.push_back(BuildLodVariant(data, 3u, lod3));
	data.lodVariants.push_back(BuildLodVariant(data, 4u, lod4));
	data.lodVariants.push_back(BuildLodVariant(data, 5u, lod5));
}
#endif

float BoneReductionScore(const SkeletonArtifactData& data, std::uint32_t bone)
{
	const auto parent = data.parentIndices[bone];
	if (parent < 0) return (std::numeric_limits<float>::max)();
	const auto& position = data.bindGlobalMatrices[bone];
	const auto& parentPosition = data.bindGlobalMatrices[static_cast<std::uint32_t>(parent)];
	const std::array<float, 3> incoming{ position._41 - parentPosition._41, position._42 - parentPosition._42, position._43 - parentPosition._43 };
	float segmentLength = std::sqrt(LengthSquared(incoming));
	float curvature = 0.0f;
	std::uint32_t childCount = 0u;
	for (std::uint32_t child = 0; child < data.parentIndices.size(); ++child) if (data.parentIndices[child] == static_cast<std::int32_t>(bone)) {
		++childCount;
		const auto& childPosition = data.bindGlobalMatrices[child];
		const std::array<float, 3> outgoing{ childPosition._41 - position._41, childPosition._42 - position._42, childPosition._43 - position._43 };
		segmentLength += std::sqrt(LengthSquared(outgoing));
		const auto inDirection = Normalize(incoming, { 1.0f, 0.0f, 0.0f });
		const auto outDirection = Normalize(outgoing, inDirection);
		curvature = (std::max)(curvature, 1.0f - std::clamp(
			inDirection[0] * outDirection[0] + inDirection[1] * outDirection[1] + inDirection[2] * outDirection[2], -1.0f, 1.0f));
	}
	const auto groupIndex = data.windSimulationGroupIndices[bone];
	const float priority = groupIndex < data.dynamicWindMetadata.groups.size()
		? (std::max)(0.01f, data.dynamicWindMetadata.groups[groupIndex].reductionPriority) : 1.0f;
	// Branch points sort first and therefore survive until the end of a phase.
	return priority * (segmentLength * (1.0f + 4.0f * curvature) + (childCount != 1u ? 1.0e12f : 0.0f));
}

std::vector<std::uint32_t> MakeGeometricTargets(
	std::uint32_t high,
	std::uint32_t low,
	float maximumRatio,
	std::span<const std::uint32_t> explicitTargets)
{
	std::vector<std::uint32_t> targets{ high };
	if (high <= low) return targets;
	const auto steps = (std::max)(1u, static_cast<std::uint32_t>(std::ceil(
		std::log(static_cast<float>(high) / static_cast<float>((std::max)(1u, low))) / std::log(maximumRatio))));
	for (std::uint32_t step = 1; step < steps; ++step) {
		const float t = static_cast<float>(step) / static_cast<float>(steps);
		targets.push_back(static_cast<std::uint32_t>(std::lround(
			std::exp(std::lerp(std::log(static_cast<float>(high)), std::log(static_cast<float>(low)), t)))));
	}
	for (const auto target : explicitTargets) if (target < high && target > low) targets.push_back(target);
	targets.push_back(low);
	std::ranges::sort(targets, std::greater{});
	targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
	return targets;
}

void BuildSkeletonLods(SkeletonArtifactData& data)
{
	const std::uint32_t count = static_cast<std::uint32_t>(data.jointNames.size());
	if (count == 0u) return;
	for (std::uint32_t groupIndex = 0; groupIndex < data.dynamicWindMetadata.groups.size(); ++groupIndex) {
		auto& group = data.dynamicWindMetadata.groups[groupIndex];
		if (group.profileGroupId == kInvalidGroup) group.profileGroupId = groupIndex;
		if (group.role == DynamicWindSimulationGroupRole::Unassigned)
			group.role = (group.flags & DynamicWindMetadata::GroupFlagTrunk) != 0u
				? DynamicWindSimulationGroupRole::Trunk : DynamicWindSimulationGroupRole::DetailBranch;
		if (!(group.reductionPriority > 0.0f)) group.reductionPriority =
			group.role == DynamicWindSimulationGroupRole::Trunk ? 4.0f :
			group.role == DynamicWindSimulationGroupRole::DetailBranch ? 2.0f : 1.0f;
		if (group.role == DynamicWindSimulationGroupRole::Trunk)
			group.minimumDriverCount = (std::max)(2u, group.minimumDriverCount);
	}
	const auto roleForBone = [&](std::uint32_t bone) {
		const auto group = data.windSimulationGroupIndices[bone];
		return group < data.dynamicWindMetadata.groups.size()
			? data.dynamicWindMetadata.groups[group].role : DynamicWindSimulationGroupRole::Unassigned;
	};

	std::vector<bool> full(count, true), authored(count, true), trunk(count, false), minimum(count, false);
	for (std::uint32_t bone = 0; bone < count; ++bone) {
		const auto role = roleForBone(bone);
		authored[bone] = role != DynamicWindSimulationGroupRole::AttachedBranch;
		trunk[bone] = data.parentIndices[bone] < 0 || role == DynamicWindSimulationGroupRole::Trunk;
		if (data.parentIndices[bone] < 0) minimum[bone] = true;
	}
	std::vector<std::uint32_t> trunkBones;
	for (const auto bone : data.evaluationOrder) if (roleForBone(bone) == DynamicWindSimulationGroupRole::Trunk) trunkBones.push_back(bone);
	if (!trunkBones.empty()) {
		minimum[trunkBones.front()] = true;
		std::vector<float> arc(count, 0.0f);
		float maximumArc = 0.0f;
		for (const auto bone : data.evaluationOrder) if (roleForBone(bone) == DynamicWindSimulationGroupRole::Trunk) {
			const auto parent = data.parentIndices[bone];
			if (parent >= 0 && roleForBone(static_cast<std::uint32_t>(parent)) == DynamicWindSimulationGroupRole::Trunk) {
				const auto& a = data.bindGlobalMatrices[bone]; const auto& b = data.bindGlobalMatrices[static_cast<std::uint32_t>(parent)];
				const float dx = a._41 - b._41, dy = a._42 - b._42, dz = a._43 - b._43;
				arc[bone] = arc[static_cast<std::uint32_t>(parent)] + std::sqrt(dx * dx + dy * dy + dz * dz);
			}
			maximumArc = (std::max)(maximumArc, arc[bone]);
		}
		const auto crown = (std::min_element)(trunkBones.begin(), trunkBones.end(), [&](auto a, auto b) {
			return std::abs(arc[a] - maximumArc * 0.75f) < std::abs(arc[b] - maximumArc * 0.75f);
		});
		minimum[*crown] = true;
	}

	struct Phase { const std::vector<bool>* high; const std::vector<bool>* low; };
	const std::array phases{ Phase{ &full, &authored }, Phase{ &authored, &trunk }, Phase{ &trunk, &minimum } };
	std::vector<std::vector<bool>> retainedSets;
	for (const auto& phase : phases) {
		const auto highCount = static_cast<std::uint32_t>(std::ranges::count(*phase.high, true));
		const auto lowCount = static_cast<std::uint32_t>(std::ranges::count(*phase.low, true));
		auto targets = MakeGeometricTargets(highCount, lowCount,
			std::clamp(data.dynamicWindMetadata.maximumAdjacentBoneRatio, 1.05f, 8.0f),
			data.dynamicWindMetadata.skeletonLodTargetBoneCounts);
		std::vector<std::uint32_t> optional;
		for (std::uint32_t bone = 0; bone < count; ++bone) if ((*phase.high)[bone] && !(*phase.low)[bone]) optional.push_back(bone);
		std::ranges::sort(optional, [&](auto a, auto b) {
			const float left = BoneReductionScore(data, a), right = BoneReductionScore(data, b);
			return left != right ? left > right : a < b;
		});
		for (const auto target : targets) {
			std::vector<bool> keep = *phase.low;
			const auto desiredOptional = target > lowCount ? (std::min)(target - lowCount, static_cast<std::uint32_t>(optional.size())) : 0u;
			for (std::uint32_t i = 0; i < desiredOptional; ++i) keep[optional[i]] = true;
			if (retainedSets.empty() || keep != retainedSets.back()) retainedSets.push_back(std::move(keep));
		}
	}
	const auto maximumVariants = std::clamp(data.dynamicWindMetadata.maximumLodVariants, 2u, 16u);
	const std::array mandatoryCounts{
		static_cast<std::uint32_t>(std::ranges::count(full, true)),
		static_cast<std::uint32_t>(std::ranges::count(authored, true)),
		static_cast<std::uint32_t>(std::ranges::count(trunk, true)),
		static_cast<std::uint32_t>(std::ranges::count(minimum, true)) };
	while (retainedSets.size() > maximumVariants) {
		// Mandatory phase endpoints have exact semantic counts; discard the least useful
		// intermediate transition while retaining both ends and the ordering.
		std::size_t remove = 1u;
		float smallestLogGap = (std::numeric_limits<float>::max)();
		for (std::size_t i = 1; i + 1 < retainedSets.size(); ++i) {
			const auto candidateCount = static_cast<std::uint32_t>(std::ranges::count(retainedSets[i], true));
			if (std::ranges::find(mandatoryCounts, candidateCount) != mandatoryCounts.end()) continue;
			const float previous = static_cast<float>(std::ranges::count(retainedSets[i - 1], true));
			const float next = static_cast<float>(std::ranges::count(retainedSets[i + 1], true));
			const float gap = std::log(previous / next);
			if (gap < smallestLogGap) { smallestLogGap = gap; remove = i; }
		}
		retainedSets.erase(retainedSets.begin() + static_cast<std::ptrdiff_t>(remove));
	}
	data.lodVariants.clear();
	for (std::uint32_t level = 0; level < retainedSets.size(); ++level)
		data.lodVariants.push_back(BuildLodVariant(data, level, retainedSets[level]));
	const float minimumLog = std::log(static_cast<float>((std::max)(1u, data.lodVariants.back().targetBoneCount)));
	const float range = (std::max)(1.0e-6f, std::log(static_cast<float>(data.lodVariants.front().targetBoneCount)) - minimumLog);
	for (auto& variant : data.lodVariants)
		variant.normalizedQuality = std::clamp((std::log(static_cast<float>(variant.targetBoneCount)) - minimumLog) / range, 0.0f, 1.0f);
}

bool BuildArtifact(const ClusterLODAssemblySkeletonData& source, SkeletonArtifactData& out, std::string& error)
{
	const std::size_t count = source.jointNames.size();
	if (count == 0 || source.parentIndices.size() != count || source.inverseBindMatrices.size() != count ||
		source.restLocalMatrices.size() != count || source.bindGlobalMatrices.size() != count) {
		error = "skeleton arrays are not aligned";
		return false;
	}
	out.jointNames = source.jointNames;
	out.parentIndices = source.parentIndices;
	out.inverseBindMatrices = source.inverseBindMatrices;
	out.bindGlobalMatrices = source.bindGlobalMatrices;
	out.windSimulationGroupIndices = source.windSimulationGroupIndices;
	if (out.windSimulationGroupIndices.empty()) out.windSimulationGroupIndices.assign(count, kInvalidGroup);
	if (out.windSimulationGroupIndices.size() != count) { error = "wind group array is not aligned"; return false; }
	out.windProfileIdentity = NormalizeProfileIdentity(source.windProfileIdentity);
	out.dynamicWindMetadata = source.dynamicWindMetadata;
	if (out.dynamicWindMetadata.bones.empty()) out.dynamicWindMetadata.bones.resize(count);
	if (out.dynamicWindMetadata.bones.size() != count) { error = "DynamicWind bone array is not aligned"; return false; }

	out.restLocalTransforms.resize(count);
	for (std::size_t i = 0; i < count; ++i) {
		const auto finiteMatrix = [](const DirectX::XMFLOAT4X4& value) {
			const float* elements = &value._11;
			return std::all_of(elements, elements + 16, [](float element) { return std::isfinite(element); });
		};
		if (!finiteMatrix(source.restLocalMatrices[i]) || !finiteMatrix(source.inverseBindMatrices[i]) ||
			!finiteMatrix(source.bindGlobalMatrices[i])) {
			error = "skeleton contains a non-finite transform";
			return false;
		}
		const auto matrix = DirectX::XMLoadFloat4x4(&source.restLocalMatrices[i]);
		DirectX::XMVECTOR scale, rotation, translation;
		if (!DirectX::XMMatrixDecompose(&scale, &rotation, &translation, matrix)) { error = "rest transform decomposition failed"; return false; }
		DirectX::XMStoreFloat3(&out.restLocalTransforms[i].position, translation);
		DirectX::XMStoreFloat4(&out.restLocalTransforms[i].rotation, DirectX::XMQuaternionNormalize(rotation));
		DirectX::XMStoreFloat3(&out.restLocalTransforms[i].scale, scale);
	}

	std::vector<std::vector<std::uint32_t>> children(count);
	std::vector<std::uint32_t> depths(count, 0u);
	for (std::uint32_t i = 0; i < count; ++i) {
		const auto parent = out.parentIndices[i];
		if (parent < 0) out.rootIndices.push_back(i);
		else if (static_cast<std::size_t>(parent) < count && static_cast<std::uint32_t>(parent) != i) children[parent].push_back(i);
		else { error = "invalid skeleton parent"; return false; }
	}
	std::queue<std::uint32_t> queue;
	for (auto root : out.rootIndices) queue.push(root);
	while (!queue.empty()) {
		const auto joint = queue.front(); queue.pop();
		out.evaluationOrder.push_back(joint);
		out.maximumHierarchyDepth = (std::max)(out.maximumHierarchyDepth, depths[joint]);
		for (auto child : children[joint]) { depths[child] = depths[joint] + 1u; queue.push(child); }
	}
	if (out.evaluationOrder.size() != count) { error = "skeleton hierarchy contains a cycle"; return false; }

	std::vector<std::array<float, 3>> origins(count);
	std::vector<std::array<float, 3>> bindAxes(count);
	for (std::size_t i = 0; i < count; ++i) {
		const auto& matrix = out.bindGlobalMatrices[i];
		origins[i] = { matrix._41, matrix._42, matrix._43 };
		bindAxes[i] = Normalize({ matrix._31, matrix._32, matrix._33 }, { 0.0f, 0.0f, 1.0f });
	}
	std::vector<std::int32_t> preferredChild(count, -1);
	std::vector<int> preferredScore(count, -1);
	for (std::size_t child = 0; child < count; ++child) {
		const auto parent = out.parentIndices[child];
		if (parent < 0) continue;
		int score = out.windSimulationGroupIndices[child] != kInvalidGroup &&
			out.windSimulationGroupIndices[child] == out.windSimulationGroupIndices[parent] ? 2 : 0;
		const auto& childWind = out.dynamicWindMetadata.bones[child];
		const auto& parentWind = out.dynamicWindMetadata.bones[parent];
		if (childWind.chainOriginBoneIndex != kInvalidGroup && childWind.chainOriginBoneIndex == parentWind.chainOriginBoneIndex) {
			score += 4;
			if (childWind.indexInBoneChain == parentWind.indexInBoneChain + 1u) score += 8;
		}
		if (score > preferredScore[parent]) { preferredScore[parent] = score; preferredChild[parent] = static_cast<std::int32_t>(child); }
	}
	out.windBoneInvariants.resize(count);
	for (auto joint : out.evaluationOrder) {
		auto axis = bindAxes[joint];
		if (preferredChild[joint] >= 0) axis = Normalize(Subtract(origins[preferredChild[joint]], origins[joint]), axis);
		else if (out.parentIndices[joint] >= 0) axis = Normalize(Subtract(origins[joint], origins[out.parentIndices[joint]]), axis);
		const auto parentAxis = out.parentIndices[joint] >= 0
			? std::array<float, 3>{ out.windBoneInvariants[out.parentIndices[joint]].branchAxis.x, out.windBoneInvariants[out.parentIndices[joint]].branchAxis.y, out.windBoneInvariants[out.parentIndices[joint]].branchAxis.z }
			: std::array<float, 3>{ 0.0f, 0.0f, 1.0f };
		auto tangent = Cross(axis, parentAxis);
		if (LengthSquared(tangent) <= 1.0e-8f) tangent = Cross(axis, std::abs(axis[2]) < 0.9f ? std::array<float, 3>{ 0, 0, 1 } : std::array<float, 3>{ 1, 0, 0 });
		auto& invariant = out.windBoneInvariants[joint];
		invariant.bindOrigin = { origins[joint][0], origins[joint][1], origins[joint][2] };
		invariant.branchAxis = { axis[0], axis[1], axis[2] };
		tangent = Normalize(tangent, { 1, 0, 0 });
		invariant.branchTangent = { tangent[0], tangent[1], tangent[2] };
		invariant.phaseSeed = joint * 2891336453u + 277803737u;
		const auto group = out.windSimulationGroupIndices[joint];
		if (group < out.dynamicWindMetadata.groups.size() && (out.dynamicWindMetadata.groups[group].flags & DynamicWindMetadata::GroupFlagTrunk)) invariant.flags |= kWindFlagTrunk;
		const auto& bone = out.dynamicWindMetadata.bones[joint];
		const float denominator = static_cast<float>(bone.chainBoneCount > 1u ? bone.chainBoneCount - 1u : 1u);
		invariant.normalizedChainPosition = std::clamp(static_cast<float>(bone.indexInBoneChain) / denominator, 0.0f, 1.0f);
	}
	out.validationFlags = SKELETON_ARTIFACT_VALID_FINITE | SKELETON_ARTIFACT_VALID_HIERARCHY | SKELETON_ARTIFACT_VALID_WIND;
	BuildSkeletonLods(out);
	return true;
}

std::vector<std::byte> Serialize(
	const SkeletonArtifactData& data,
	std::array<std::uint64_t, kArtifactSectionCount>* sectionOffsets = nullptr)
{
	std::vector<std::byte> out;
	std::size_t section = 0;
	const auto beginSection = [&] {
		if (sectionOffsets && section < sectionOffsets->size()) (*sectionOffsets)[section] = out.size();
		++section;
	};
	WritePod(out, SKELETON_ARTIFACT_SCHEMA_VERSION);
	beginSection();
	WriteStrings(out, data.jointNames);
	beginSection();
	WriteVector(out, data.parentIndices);
	WriteVector(out, data.evaluationOrder);
	WriteVector(out, data.rootIndices);
	beginSection();
	WriteVector(out, data.restLocalTransforms);
	beginSection();
	WriteVector(out, data.inverseBindMatrices);
	beginSection();
	WriteVector(out, data.bindGlobalMatrices);
	beginSection();
	WriteVector(out, data.windSimulationGroupIndices);
	beginSection();
	WriteVector(out, data.windBoneInvariants);
	beginSection();
	WriteString(out, data.windProfileIdentity);
	beginSection();
	WritePod(out, data.dynamicWindMetadata.enabled);
	WritePod(out, data.dynamicWindMetadata.groundCover);
	WritePod(out, data.dynamicWindMetadata.gustAttenuation);
	WritePod(out, data.dynamicWindMetadata.attachedBranchProfileGroupId);
	WritePod(out, data.dynamicWindMetadata.maximumLodVariants);
	WritePod(out, data.dynamicWindMetadata.maximumAdjacentBoneRatio);
	WritePod(out, data.dynamicWindMetadata.skeletonLodQualityBias);
	WriteVector(out, data.dynamicWindMetadata.skeletonLodTargetBoneCounts);
	beginSection();
	WriteVector(out, data.dynamicWindMetadata.groups);
	beginSection();
	WriteVector(out, data.dynamicWindMetadata.bones);
	beginSection();
	WriteLodVariants(out, data.lodVariants);
	beginSection();
	WritePod(out, data.maximumHierarchyDepth);
	WritePod(out, data.validationFlags);
	return out;
}

bool Deserialize(
	const std::vector<std::byte>& bytes,
	SkeletonArtifactData& data,
	const std::array<std::uint64_t, kArtifactSectionCount>& sectionOffsets)
{
	if (sectionOffsets.front() < sizeof(std::uint32_t) || sectionOffsets.back() >= bytes.size() ||
		!std::ranges::is_sorted(sectionOffsets)) return false;
	std::size_t offset = 0;
	std::uint32_t schema = 0;
	return ReadPod(bytes, offset, schema) && schema == SKELETON_ARTIFACT_SCHEMA_VERSION &&
		ReadStrings(bytes, offset, data.jointNames) && ReadVector(bytes, offset, data.parentIndices) &&
		ReadVector(bytes, offset, data.evaluationOrder) && ReadVector(bytes, offset, data.rootIndices) &&
		ReadVector(bytes, offset, data.restLocalTransforms) && ReadVector(bytes, offset, data.inverseBindMatrices) &&
		ReadVector(bytes, offset, data.bindGlobalMatrices) && ReadVector(bytes, offset, data.windSimulationGroupIndices) &&
		ReadVector(bytes, offset, data.windBoneInvariants) && ReadString(bytes, offset, data.windProfileIdentity) &&
		ReadPod(bytes, offset, data.dynamicWindMetadata.enabled) && ReadPod(bytes, offset, data.dynamicWindMetadata.groundCover) &&
		ReadPod(bytes, offset, data.dynamicWindMetadata.gustAttenuation) && ReadPod(bytes, offset, data.dynamicWindMetadata.attachedBranchProfileGroupId) &&
		ReadPod(bytes, offset, data.dynamicWindMetadata.maximumLodVariants) &&
		ReadPod(bytes, offset, data.dynamicWindMetadata.maximumAdjacentBoneRatio) &&
		ReadPod(bytes, offset, data.dynamicWindMetadata.skeletonLodQualityBias) &&
		ReadVector(bytes, offset, data.dynamicWindMetadata.skeletonLodTargetBoneCounts) &&
		ReadVector(bytes, offset, data.dynamicWindMetadata.groups) &&
		ReadVector(bytes, offset, data.dynamicWindMetadata.bones) &&
		ReadLodVariants(bytes, offset, data.lodVariants) && ReadPod(bytes, offset, data.maximumHierarchyDepth) &&
		ReadPod(bytes, offset, data.validationFlags) && offset == bytes.size();
}

// Artifacts are content-addressed and immutable. Keep the decoded payload and
// constructed base skeleton alive for the process lifetime so disjoint CLod
// metadata-cache entries cannot cause the same artifact to be read again.
struct RegistryEntry { std::shared_ptr<const SkeletonArtifactData> data; std::shared_ptr<Skeleton> skeleton; };
std::mutex gRegistryMutex;
std::unordered_map<SkeletonArtifactId, RegistryEntry> gRegistry;
std::array<std::mutex, 64> gArtifactLoadMutexes;
std::array<std::mutex, 64> gSkeletonBuildMutexes;
std::atomic<std::uint64_t> gTempCounter{ 0 };

std::size_t RegistryStripe(const SkeletonArtifactId& id)
{
	return std::hash<SkeletonArtifactId>{}(id) % gArtifactLoadMutexes.size();
}

std::wstring ArtifactPath(const SkeletonArtifactId& id)
{
	return GetCacheFilePath(s2ws("skeleton_" + id.ToString() + ".brskel"), L"skeleton");
}

}

namespace SkeletonArtifactCache {

std::optional<SkeletonArtifactReference> Save(const ClusterLODAssemblySkeletonData& source, std::string* error)
{
	SkeletonArtifactData data;
	std::string localError;
	if (!BuildArtifact(source, data, localError)) { if (error) *error = localError; return std::nullopt; }
	std::array<std::uint64_t, kArtifactSectionCount> sectionOffsets{};
	auto raw = Serialize(data, &sectionOffsets);
	SkeletonArtifactId id{ Sha256(raw) };
	if (id.Empty()) {
		if (error) *error = "SHA-256 generation failed";
		return std::nullopt;
	}
	SkeletonArtifactReference reference{ id, SKELETON_ARTIFACT_SCHEMA_VERSION, static_cast<std::uint32_t>(data.jointNames.size()) };
	const std::wstring path = ArtifactPath(id);
	if (std::filesystem::exists(path)) {
		if (Load(reference, &localError)) return reference;
		std::error_code ec; std::filesystem::remove(path, ec);
	}
	std::vector<char> compressed(static_cast<std::size_t>(LZ4_compressBound(static_cast<int>(raw.size()))));
	const int compressedBytes = LZ4_compress_default(reinterpret_cast<const char*>(raw.data()), compressed.data(), static_cast<int>(raw.size()), static_cast<int>(compressed.size()));
	if (compressedBytes <= 0) { if (error) *error = "LZ4 compression failed"; return std::nullopt; }
	compressed.resize(static_cast<std::size_t>(compressedBytes));
	ArtifactHeader header;
	header.uncompressedBytes = raw.size(); header.compressedBytes = compressed.size();
	header.jointCount = reference.jointCount; header.groupCount = static_cast<std::uint32_t>(data.dynamicWindMetadata.groups.size()); header.digest = id.digest;
	header.sectionOffsets = sectionOffsets;
	const auto temp = path + L"." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(gTempCounter.fetch_add(1)) + L".tmp";
	{
		std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
		stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
		stream.write(compressed.data(), static_cast<std::streamsize>(compressed.size()));
		if (!stream.good()) { if (error) *error = "artifact write failed"; std::filesystem::remove(temp); return std::nullopt; }
	}
	if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) {
		std::filesystem::remove(temp);
		if (!Load(reference, &localError)) { if (error) *error = "artifact publication failed: " + localError; return std::nullopt; }
	}
	spdlog::info("Skeleton artifact: stored id={} joints={} bytes={} compressed={}", id.ToString(), reference.jointCount, raw.size(), compressed.size());
	return reference;
}

std::shared_ptr<const SkeletonArtifactData> Load(const SkeletonArtifactReference& reference, std::string* error)
{
	if (reference.Empty() || reference.schemaVersion != SKELETON_ARTIFACT_SCHEMA_VERSION) { if (error) *error = "invalid artifact reference"; return {}; }
	{
		std::lock_guard lock(gRegistryMutex);
		if (auto found = gRegistry.find(reference.id); found != gRegistry.end() && found->second.data) {
			if (found->second.data->jointNames.size() == reference.jointCount) return found->second.data;
			if (error) *error = "artifact reference joint count does not match cached payload";
			return {};
		}
	}
	// Coalesce concurrent readers for the same content ID.  The second cache
	// check is required because another request may have completed while this
	// one waited for the stripe.
	std::lock_guard loadLock(gArtifactLoadMutexes[RegistryStripe(reference.id)]);
	{
		std::lock_guard lock(gRegistryMutex);
		if (auto found = gRegistry.find(reference.id); found != gRegistry.end() && found->second.data) {
			if (found->second.data->jointNames.size() == reference.jointCount) return found->second.data;
			if (error) *error = "artifact reference joint count does not match cached payload";
			return {};
		}
	}
	std::ifstream stream(ArtifactPath(reference.id), std::ios::binary);
	ArtifactHeader header;
	if (!stream.read(reinterpret_cast<char*>(&header), sizeof(header)) || header.magic != kMagic || header.schemaVersion != SKELETON_ARTIFACT_SCHEMA_VERSION ||
		header.headerBytes != sizeof(ArtifactHeader) ||
		header.digest != reference.id.digest || header.jointCount != reference.jointCount || header.uncompressedBytes > static_cast<std::uint64_t>((std::numeric_limits<int>::max)()) ||
		header.compressedBytes > static_cast<std::uint64_t>((std::numeric_limits<int>::max)())) { if (error) *error = "invalid artifact header"; return {}; }
	std::vector<char> compressed(static_cast<std::size_t>(header.compressedBytes));
	if (!stream.read(compressed.data(), static_cast<std::streamsize>(compressed.size()))) { if (error) *error = "truncated artifact"; return {}; }
	std::vector<std::byte> raw(static_cast<std::size_t>(header.uncompressedBytes));
	if (LZ4_decompress_safe(compressed.data(), reinterpret_cast<char*>(raw.data()), static_cast<int>(compressed.size()), static_cast<int>(raw.size())) != static_cast<int>(raw.size()) || Sha256(raw) != reference.id.digest) {
		if (error) *error = "artifact digest or decompression failure"; return {};
	}
	auto data = std::make_shared<SkeletonArtifactData>();
	if (!Deserialize(raw, *data, header.sectionOffsets) || data->jointNames.size() != reference.jointCount) { if (error) *error = "artifact payload is invalid"; return {}; }
	{
		std::lock_guard lock(gRegistryMutex);
		auto& entry = gRegistry[reference.id];
		if (entry.data) {
			if (entry.data->jointNames.size() == reference.jointCount) return entry.data;
			if (error) *error = "artifact reference joint count does not match cached payload";
			return {};
		}
		entry.data = data;
	}
	spdlog::info("Skeleton artifact: loaded id={} joints={} bytes={} compressed={}", reference.id.ToString(), reference.jointCount, raw.size(), compressed.size());
	return data;
}

std::shared_ptr<Skeleton> ResolveSkeleton(const SkeletonArtifactReference& reference, std::string* error)
{
	if (reference.Empty() || reference.schemaVersion != SKELETON_ARTIFACT_SCHEMA_VERSION) {
		if (error) *error = "invalid artifact reference";
		return {};
	}
	{
		std::lock_guard lock(gRegistryMutex);
		if (auto found = gRegistry.find(reference.id); found != gRegistry.end() && found->second.skeleton) {
			if (found->second.skeleton->GetBoneCount() == reference.jointCount) return found->second.skeleton;
			if (error) *error = "artifact reference joint count does not match cached skeleton";
			return {};
		}
	}
	std::lock_guard buildLock(gSkeletonBuildMutexes[RegistryStripe(reference.id)]);
	{
		std::lock_guard lock(gRegistryMutex);
		if (auto found = gRegistry.find(reference.id); found != gRegistry.end() && found->second.skeleton) {
			if (found->second.skeleton->GetBoneCount() == reference.jointCount) return found->second.skeleton;
			if (error) *error = "artifact reference joint count does not match cached skeleton";
			return {};
		}
	}
	auto data = Load(reference, error);
	if (!data) return {};
	std::vector<DirectX::XMMATRIX> inverseBinds; inverseBinds.reserve(data->inverseBindMatrices.size());
	for (const auto& matrix : data->inverseBindMatrices) inverseBinds.push_back(DirectX::XMLoadFloat4x4(&matrix));
	std::vector<DirectX::XMMATRIX> bindGlobals; bindGlobals.reserve(data->bindGlobalMatrices.size());
	for (const auto& matrix : data->bindGlobalMatrices) bindGlobals.push_back(DirectX::XMLoadFloat4x4(&matrix));
	std::vector<Components::Transform> rest; rest.reserve(data->restLocalTransforms.size());
	for (const auto& packed : data->restLocalTransforms) rest.emplace_back(Components::Position(packed.position), Components::Rotation(DirectX::XMLoadFloat4(&packed.rotation)), Components::Scale(packed.scale));
	auto skeleton = std::make_shared<Skeleton>(data->jointNames, data->parentIndices, std::move(inverseBinds), std::move(rest), std::vector<DirectX::XMMATRIX>{},
		data->windSimulationGroupIndices, data->windProfileIdentity, data->dynamicWindMetadata, data->evaluationOrder, data->windBoneInvariants,
		std::move(bindGlobals), data->lodVariants);
	// Retain the historical metadata bit for artifact compatibility. SkeletonManager
	// converts computed palettes to the canonical shader-native layout before upload.
	skeleton->SetSkinningGPUFlags(kSkinningInstanceFlagRowVectorSkinMatrix);
	{
		std::lock_guard lock(gRegistryMutex);
		auto& entry = gRegistry[reference.id];
		if (entry.skeleton) {
			if (entry.skeleton->GetBoneCount() == reference.jointCount) return entry.skeleton;
			if (error) *error = "artifact reference joint count does not match cached skeleton";
			return {};
		}
		entry.skeleton = skeleton;
	}
	return skeleton;
}

}
