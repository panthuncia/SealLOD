#include "Import/SkeletonArtifactValidation.h"

#include "Utilities/CachePathUtilities.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include <Windows.h>
#include <bcrypt.h>
#include <lz4.h>

namespace {

constexpr std::array<char, 8> kMagic{ 'B', 'R', 'S', 'K', 'E', 'L', '0', '3' };
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

void SetError(std::string* error, std::string value)
{
	if (error != nullptr) *error = std::move(value);
}

bool EmptyId(const SkeletonArtifactId& id)
{
	return std::ranges::all_of(id.digest, [](std::uint8_t byte) { return byte == 0u; });
}

std::string IdString(const SkeletonArtifactId& id)
{
	static constexpr char hex[] = "0123456789abcdef";
	std::string result(id.digest.size() * 2u, '0');
	for (std::size_t i = 0; i < id.digest.size(); ++i) {
		result[i * 2u] = hex[id.digest[i] >> 4u];
		result[i * 2u + 1u] = hex[id.digest[i] & 15u];
	}
	return result;
}

std::filesystem::path ArtifactPath(const SkeletonArtifactId& id)
{
	return GetCacheFilePath(s2ws("skeleton_" + IdString(id) + ".brskel"), L"skeleton");
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
		if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
		return result;
	}
	std::vector<std::uint8_t> object(objectBytes);
	if (BCryptCreateHash(algorithm, &hash, object.data(), objectBytes, nullptr, 0, 0) >= 0) {
		BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())), static_cast<ULONG>(bytes.size()), 0);
		BCryptFinishHash(hash, result.data(), static_cast<ULONG>(result.size()), 0);
	}
	if (hash != nullptr) BCryptDestroyHash(hash);
	BCryptCloseAlgorithmProvider(algorithm, 0);
	return result;
}

}

namespace SkeletonArtifactValidation {

bool Validate(const SkeletonArtifactReference& reference, std::string* error)
{
	if (reference.jointCount == 0u || EmptyId(reference.id)) {
		SetError(error, "empty artifact reference");
		return false;
	}
	if (reference.schemaVersion != SKELETON_ARTIFACT_SCHEMA_VERSION) {
		SetError(error, "artifact reference schema is stale");
		return false;
	}

	const auto path = ArtifactPath(reference.id);
	std::ifstream stream(path, std::ios::binary | std::ios::ate);
	if (!stream.is_open()) {
		SetError(error, "artifact file is missing");
		return false;
	}
	const std::streampos end = stream.tellg();
	if (end < static_cast<std::streamoff>(sizeof(ArtifactHeader))) {
		SetError(error, "artifact header is truncated");
		return false;
	}
	stream.seekg(0, std::ios::beg);
	ArtifactHeader header;
	if (!stream.read(reinterpret_cast<char*>(&header), sizeof(header))) {
		SetError(error, "artifact header could not be read");
		return false;
	}
	if (header.magic != kMagic || header.schemaVersion != SKELETON_ARTIFACT_SCHEMA_VERSION ||
		header.headerBytes != sizeof(ArtifactHeader) || header.digest != reference.id.digest ||
		header.jointCount != reference.jointCount || header.uncompressedBytes == 0u || header.compressedBytes == 0u ||
		header.uncompressedBytes > static_cast<std::uint64_t>((std::numeric_limits<int>::max)()) ||
		header.compressedBytes > static_cast<std::uint64_t>((std::numeric_limits<int>::max)())) {
		SetError(error, "artifact header is invalid or stale");
		return false;
	}
	const auto expectedBytes = static_cast<std::uint64_t>(sizeof(ArtifactHeader)) + header.compressedBytes;
	if (static_cast<std::uint64_t>(static_cast<std::streamoff>(end)) != expectedBytes) {
		SetError(error, "artifact file length does not match its header");
		return false;
	}

	std::vector<char> compressed(static_cast<std::size_t>(header.compressedBytes));
	if (!stream.read(compressed.data(), static_cast<std::streamsize>(compressed.size()))) {
		SetError(error, "artifact payload is truncated");
		return false;
	}
	std::vector<std::byte> raw(static_cast<std::size_t>(header.uncompressedBytes));
	if (LZ4_decompress_safe(
			compressed.data(),
			reinterpret_cast<char*>(raw.data()),
			static_cast<int>(compressed.size()),
			static_cast<int>(raw.size())) != static_cast<int>(raw.size())) {
		SetError(error, "artifact payload cannot be decompressed");
		return false;
	}
	if (Sha256(raw) != reference.id.digest) {
		SetError(error, "artifact payload digest does not match its reference");
		return false;
	}
	return true;
}

}
