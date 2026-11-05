#pragma once

#include <string>
#include <cstdint>
#include <string_view>

namespace br::processed_texture_cache {

inline constexpr uint32_t kMagic = 0x45545850u; // 'PTXE'
inline constexpr uint32_t kVersion = 1u;
inline constexpr wchar_t kExtension[] = L".dstexcache";

enum HeaderFlags : uint32_t {
	FlagIsCubemap = 1u << 0,
	FlagIsArray = 1u << 1,
	FlagHasFullMipChain = 1u << 2,
	FlagIsBlockCompressed = 1u << 3,
};

// Where one subresource sits in a cache file. Files hold subresources as a D3D12 copy expects them, and
// this reproduces ID3D12Device::GetCopyableFootprints for 2D textures without a device, so tools with no
// GPU write the same bytes. Mip and array order are the caller's: pass subresources in D3D12 order.
struct SubresourcePlacement {
	uint64_t offset = 0;
	uint32_t rowPitch = 0;
	uint32_t rowCount = 0;
	uint32_t rowSize = 0;
};

// blockWidth and blockHeight are 1 for uncompressed formats, and blockBytes is then bytes per texel.
inline SubresourcePlacement PlaceSubresource(uint64_t endOfPrevious, uint32_t width, uint32_t height,
	uint32_t blockWidth, uint32_t blockHeight, uint32_t blockBytes) {
	constexpr uint64_t kPlacementAlignment = 512u;  // D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT
	constexpr uint32_t kPitchAlignment = 256u;      // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT
	SubresourcePlacement placement{};
	placement.offset = (endOfPrevious + kPlacementAlignment - 1u) / kPlacementAlignment * kPlacementAlignment;
	placement.rowSize = (width + blockWidth - 1u) / blockWidth * blockBytes;
	placement.rowPitch = (placement.rowSize + kPitchAlignment - 1u) / kPitchAlignment * kPitchAlignment;
	placement.rowCount = (height + blockHeight - 1u) / blockHeight;
	return placement;
}

// The last row is not padded, as in GetCopyableFootprints.
inline uint64_t EndOfSubresource(const SubresourcePlacement& placement) {
	return placement.offset + uint64_t{ placement.rowPitch } * (placement.rowCount - 1u) + placement.rowSize;
}

#pragma pack(push, 1)
struct FileHeader {
	uint32_t magic = kMagic;
	uint32_t version = kVersion;
	uint32_t headerSize = sizeof(FileHeader);
	uint32_t flags = 0;
	uint32_t format = 0;
	uint32_t channels = 0;
	uint32_t baseWidth = 0;
	uint32_t baseHeight = 0;
	uint32_t mipLevels = 0;
	uint32_t arraySize = 0;
	uint32_t totalArraySlices = 0;
	uint32_t subresourceCount = 0;
	uint64_t dataOffset = sizeof(FileHeader);
	uint64_t dataSizeBytes = 0;
};
#pragma pack(pop)

// The header of a cache file without opening it, if an earlier run read it. Any thread.
[[nodiscard]] bool TryFindIndexedHeader(const std::wstring& path, FileHeader& header);
void RecordIndexedHeader(const std::wstring& path, const FileHeader& header);

inline bool HasFlag(const FileHeader& header, HeaderFlags flag) {
	return (header.flags & static_cast<uint32_t>(flag)) != 0;
}

inline bool IsConditionedCacheExtension(std::wstring_view extension) {
	return extension == kExtension;
}

}