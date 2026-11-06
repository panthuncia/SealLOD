#include "BasicRenderer/Assets/Import/CLodOpacityMicromapCache.h"

#include "BasicRenderer/Assets/CachePack.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace CLodCache {

namespace {

constexpr uint32_t kMagic = 0x4D4F4C43u; // "CLOM"
constexpr uint32_t kMaxLevel = 12u;

#pragma pack(push, 1)
struct FileHeader {
	uint32_t magic;
	uint32_t version;
	uint32_t identity;
	uint32_t subdivisionLevel;
	uint16_t format;
	uint16_t padding0;
	uint32_t groupCount;
	uint64_t containerBytes;
	uint64_t groupTableOffset;
	uint8_t reserved[24];
};
struct GroupEntry {
	uint64_t blobOffset; // 0: no micromaps.
	uint32_t blobBytes;
	uint32_t clusterCount;
	uint32_t micromapCount;
	uint32_t indicesOffset; // In the blob; descs start at 0.
	uint32_t dataOffset;
	uint32_t dataBytes;
};
#pragma pack(pop)
static_assert(sizeof(FileHeader) == 64);
static_assert(sizeof(GroupEntry) == 32);

constexpr uint64_t kBlobAlignment = 256u;

uint64_t AlignUp(uint64_t value, uint64_t alignment)
{
	return (value + alignment - 1u) / alignment * alignment;
}

struct Fnv {
	uint64_t value = 1469598103934665603ull;
	void Bytes(const void* data, size_t size)
	{
		const auto* bytes = static_cast<const uint8_t*>(data);
		for (size_t i = 0; i < size; ++i) {
			value ^= bytes[i];
			value *= 1099511628211ull;
		}
	}
	void U32(uint32_t v) { Bytes(&v, sizeof(v)); }
	void F32(float v) { U32(std::bit_cast<uint32_t>(v)); }
	void Text(std::string_view text)
	{
		U32(static_cast<uint32_t>(text.size()));
		Bytes(text.data(), text.size());
	}
};

// Bytes of one micromap's data: 2 bits per micro-triangle, 4^level of them.
uint64_t EntryBytes(uint32_t level)
{
	return std::max<uint64_t>(1u, ((1ull << (2u * level)) * 2u + 7u) / 8u);
}

bool Fail(std::string* error, std::string message)
{
	if (error) *error = std::move(message);
	return false;
}

} // namespace

uint32_t OpacityMicromapIdentity(const OpacityMicromapMaterialKey& key)
{
	Fnv hash;
	hash.U32(kOpacityMicromapCacheVersion);
	hash.Text(key.baseColorTexture);
	hash.Text(key.opacityTexture);
	hash.U32(key.baseColorUvSet);
	hash.U32(key.opacityUvSet);
	hash.F32(key.alphaCutoff);
	hash.F32(key.alphaFactor);
	// 31 bits: the BVH marks a BLAS whose clusters disagree with the top bit (CLOD_RT_OMM_KEY_CONFLICT).
	const uint32_t folded = static_cast<uint32_t>(hash.value ^ (hash.value >> 32u)) & 0x7FFFFFFFu;
	return folded != 0u ? folded : 1u;
}

std::wstring OpacityMicromapPath(const std::wstring& containerPath, uint32_t identity)
{
	std::filesystem::path path(containerPath);
	path.replace_extension();
	wchar_t suffix[32]{};
	swprintf_s(suffix, L".%08x.clodomm", identity);
	path += suffix;
	return path.wstring();
}

uint64_t OpacityMicromapContainerBytes(const std::wstring& containerPath)
{
	CachePack::Entry entry{};
	if (CachePack::Find(std::filesystem::path(containerPath), entry)) return entry.size;
	std::error_code ec;
	const uint64_t bytes = std::filesystem::file_size(std::filesystem::path(containerPath), ec);
	return ec ? 0u : bytes;
}

bool SaveOpacityMicromaps(const std::wstring& path, const OpacityMicromapFileInfo& info, std::span<const OpacityMicromapGroupInput> groups, std::string* error)
{
	if (info.identity == 0u || info.subdivisionLevel > kMaxLevel) return Fail(error, "bad identity or level");
	std::vector<GroupEntry> entries(info.groupCount, GroupEntry{});
	uint64_t offset = AlignUp(sizeof(FileHeader) + sizeof(GroupEntry) * uint64_t(info.groupCount), kBlobAlignment);
	for (const OpacityMicromapGroupInput& group : groups) {
		if (group.groupLocalIndex >= info.groupCount) return Fail(error, "group index past the mesh's groups");
		if (group.indices.size() != uint64_t(group.clusterCount) * kOpacityMicromapMaxTrianglesPerCluster) return Fail(error, "group indices don't fill its clusters");
		GroupEntry& entry = entries[group.groupLocalIndex];
		if (entry.blobOffset != 0u) return Fail(error, "a group given twice");
		entry.clusterCount = group.clusterCount;
		entry.micromapCount = static_cast<uint32_t>(group.descs.size());
		entry.indicesOffset = static_cast<uint32_t>(AlignUp(group.descs.size() * sizeof(OpacityMicromapDesc), 16u));
		entry.dataOffset = entry.indicesOffset + group.clusterCount * kOpacityMicromapIndexUnitBytes;
		entry.dataBytes = static_cast<uint32_t>(group.data.size());
		entry.blobBytes = entry.dataOffset + entry.dataBytes;
		entry.blobOffset = offset;
		offset = AlignUp(offset + entry.blobBytes, kBlobAlignment);
	}

	std::filesystem::path temporary(path);
	temporary += L".tmp";
	{
		std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
		if (!file) return Fail(error, "can't create the file");
		FileHeader header{};
		header.magic = kMagic;
		header.version = kOpacityMicromapCacheVersion;
		header.identity = info.identity;
		header.subdivisionLevel = info.subdivisionLevel;
		header.format = info.format;
		header.groupCount = info.groupCount;
		header.containerBytes = info.containerBytes;
		header.groupTableOffset = sizeof(FileHeader);
		file.write(reinterpret_cast<const char*>(&header), sizeof(header));
		file.write(reinterpret_cast<const char*>(entries.data()), static_cast<std::streamsize>(entries.size() * sizeof(GroupEntry)));
		std::vector<char> zeros(kBlobAlignment, 0);
		const auto padTo = [&](uint64_t position) {
			const uint64_t at = static_cast<uint64_t>(file.tellp());
			if (position > at) file.write(zeros.data(), static_cast<std::streamsize>(position - at));
		};
		for (const OpacityMicromapGroupInput& group : groups) {
			const GroupEntry& entry = entries[group.groupLocalIndex];
			padTo(entry.blobOffset);
			file.write(reinterpret_cast<const char*>(group.descs.data()), static_cast<std::streamsize>(group.descs.size() * sizeof(OpacityMicromapDesc)));
			padTo(entry.blobOffset + entry.indicesOffset);
			file.write(reinterpret_cast<const char*>(group.indices.data()), static_cast<std::streamsize>(group.indices.size() * sizeof(int32_t)));
			file.write(reinterpret_cast<const char*>(group.data.data()), static_cast<std::streamsize>(group.data.size()));
		}
		padTo(offset);
		if (!file) return Fail(error, "write failed");
	}
	std::error_code ec;
	std::filesystem::rename(temporary, path, ec);
	if (ec) {
		std::filesystem::remove(temporary, ec);
		return Fail(error, "can't move the file into place");
	}
	CachePack::NoteFileChanged(std::filesystem::path(path));
	return true;
}

struct OpacityMicromapFile::Mapping {
	HANDLE file = INVALID_HANDLE_VALUE;
	HANDLE mapping = nullptr;
	const std::byte* view = nullptr;
	uint64_t bytes = 0;
	bool owned = true; // False for a view into the cache pack.
	~Mapping()
	{
		if (view && owned) UnmapViewOfFile(view);
		if (mapping) CloseHandle(mapping);
		if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
	}
};

OpacityMicromapFile::~OpacityMicromapFile() = default;

std::shared_ptr<const OpacityMicromapFile> OpacityMicromapFile::Open(const std::wstring& path, uint32_t expectedIdentity, uint64_t expectedContainerBytes, std::string* error)
{
	auto mapping = std::make_unique<Mapping>();
	CachePack::Entry packed{};
	if (CachePack::Find(std::filesystem::path(path), packed)) {
		// The pack's view lives as long as the process.
		mapping->view = packed.data;
		mapping->bytes = packed.size;
		mapping->owned = false;
	}
	else {
		mapping->file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (mapping->file == INVALID_HANDLE_VALUE) {
			Fail(error, "no file");
			return nullptr;
		}
		LARGE_INTEGER size{};
		if (!GetFileSizeEx(mapping->file, &size)) {
			Fail(error, "unreadable");
			return nullptr;
		}
		mapping->bytes = static_cast<uint64_t>(size.QuadPart);
		if (mapping->bytes >= sizeof(FileHeader)) {
			mapping->mapping = CreateFileMappingW(mapping->file, nullptr, PAGE_READONLY, 0, 0, nullptr);
			if (!mapping->mapping) {
				Fail(error, "can't map");
				return nullptr;
			}
			mapping->view = static_cast<const std::byte*>(MapViewOfFile(mapping->mapping, FILE_MAP_READ, 0, 0, 0));
			if (!mapping->view) {
				Fail(error, "can't map");
				return nullptr;
			}
		}
	}
	if (mapping->bytes < sizeof(FileHeader) || mapping->view == nullptr) {
		Fail(error, "truncated");
		return nullptr;
	}

	FileHeader header{};
	std::memcpy(&header, mapping->view, sizeof(header));
	if (header.magic != kMagic || header.version != kOpacityMicromapCacheVersion) {
		Fail(error, "another format or version");
		return nullptr;
	}
	if (header.identity != expectedIdentity || header.containerBytes != expectedContainerBytes) {
		Fail(error, "stale: baked for another material or container");
		return nullptr;
	}
	if (header.subdivisionLevel > kMaxLevel || header.groupTableOffset != sizeof(FileHeader)
		|| header.groupTableOffset + uint64_t(header.groupCount) * sizeof(GroupEntry) > mapping->bytes) {
		Fail(error, "malformed header");
		return nullptr;
	}

	std::shared_ptr<OpacityMicromapFile> result(new OpacityMicromapFile());
	result->m_info.identity = header.identity;
	result->m_info.subdivisionLevel = header.subdivisionLevel;
	result->m_info.format = header.format;
	result->m_info.containerBytes = header.containerBytes;
	result->m_info.groupCount = header.groupCount;
	result->m_groups.resize(header.groupCount);
	const std::byte* table = mapping->view + header.groupTableOffset;
	for (uint32_t g = 0; g < header.groupCount; ++g) {
		GroupEntry entry{};
		std::memcpy(&entry, table + uint64_t(g) * sizeof(GroupEntry), sizeof(entry));
		if (entry.blobOffset == 0u) continue;
		const uint64_t descsBytes = uint64_t(entry.micromapCount) * sizeof(OpacityMicromapDesc);
		const uint64_t indicesBytes = uint64_t(entry.clusterCount) * kOpacityMicromapIndexUnitBytes;
		if (entry.blobOffset + entry.blobBytes > mapping->bytes || descsBytes > entry.indicesOffset
			|| uint64_t(entry.indicesOffset) + indicesBytes != entry.dataOffset || uint64_t(entry.dataOffset) + entry.dataBytes != entry.blobBytes) {
			Fail(error, "malformed group " + std::to_string(g));
			return nullptr;
		}
		OpacityMicromapGroupView& view = result->m_groups[g];
		view.clusterCount = entry.clusterCount;
		view.blob = { mapping->view + entry.blobOffset, entry.blobBytes };
		view.descsOffset = 0u;
		view.indicesOffset = entry.indicesOffset;
		view.dataOffset = entry.dataOffset;
		view.descs = { reinterpret_cast<const OpacityMicromapDesc*>(view.blob.data()), entry.micromapCount };
		view.indices = view.blob.subspan(entry.indicesOffset, indicesBytes);
		view.data = view.blob.subspan(entry.dataOffset, entry.dataBytes);
		for (const OpacityMicromapDesc& desc : view.descs) {
			if (desc.subdivisionLevel > kMaxLevel || desc.format != header.format
				|| uint64_t(desc.byteOffset) + EntryBytes(desc.subdivisionLevel) > entry.dataBytes) {
				Fail(error, "malformed micromap in group " + std::to_string(g));
				return nullptr;
			}
			++view.levelCounts[desc.subdivisionLevel];
		}
		for (uint64_t i = 0; i < indicesBytes / 4u; ++i) {
			int32_t index = 0;
			std::memcpy(&index, view.indices.data() + i * 4u, 4u);
			if (index < -4 || (index >= 0 && static_cast<uint32_t>(index) >= entry.micromapCount)) {
				Fail(error, "index out of range in group " + std::to_string(g));
				return nullptr;
			}
		}
	}
	result->m_mapping = std::move(mapping);
	return result;
}

bool OpacityMicromapFile::Group(uint32_t groupLocalIndex, OpacityMicromapGroupView& out) const
{
	if (groupLocalIndex >= m_groups.size() || m_groups[groupLocalIndex].blob.empty()) return false;
	out = m_groups[groupLocalIndex];
	return true;
}

}
