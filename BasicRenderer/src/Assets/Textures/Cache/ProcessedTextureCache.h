#pragma once

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

inline bool HasFlag(const FileHeader& header, HeaderFlags flag) {
	return (header.flags & static_cast<uint32_t>(flag)) != 0;
}

inline bool IsConditionedCacheExtension(std::wstring_view extension) {
	return extension == kExtension;
}

}