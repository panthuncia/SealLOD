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

constexpr std::array<char, 8> kMagic{ 'B', 'R', 'S', 'K', 'E', 'L', '0', '1' };
constexpr std::uint32_t kInvalidGroup = 0xFFFFFFFFu;
constexpr std::uint32_t kWindFlagTrunk = 1u << 0u;
constexpr std::size_t kArtifactSectionCount = 12u;

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
	beginSection();
	WriteVector(out, data.dynamicWindMetadata.groups);
	beginSection();
	WriteVector(out, data.dynamicWindMetadata.bones);
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
		ReadPod(bytes, offset, data.dynamicWindMetadata.gustAttenuation) && ReadVector(bytes, offset, data.dynamicWindMetadata.groups) &&
		ReadVector(bytes, offset, data.dynamicWindMetadata.bones) && ReadPod(bytes, offset, data.maximumHierarchyDepth) &&
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

bool SkeletonArtifactId::Empty() const noexcept { return std::ranges::all_of(digest, [](std::uint8_t b) { return b == 0; }); }

std::string SkeletonArtifactId::ToString() const
{
	static constexpr char hex[] = "0123456789abcdef";
	std::string result(digest.size() * 2u, '0');
	for (std::size_t i = 0; i < digest.size(); ++i) { result[i * 2] = hex[digest[i] >> 4]; result[i * 2 + 1] = hex[digest[i] & 15]; }
	return result;
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
		std::move(bindGlobals));
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
