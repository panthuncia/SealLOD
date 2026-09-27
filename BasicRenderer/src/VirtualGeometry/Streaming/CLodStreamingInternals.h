#pragma once

#include <cstdint>
#include <limits>

#include "VirtualGeometry/Streaming/CLodStreamingSystem.h"

namespace
{
constexpr uint64_t kInvalidCLodMeshPageKey = (std::numeric_limits<uint64_t>::max)();

uint64_t CLodReadyCompletionStorageBytes(
	const br::render::CLodDiskStreamingCompletion& completion)
{
	uint64_t bytes = 0u;
	for (const auto& blob : completion.pageBlobs) {
		bytes += blob.capacity();
	}
	bytes += completion.mappedPageBlobSizes.capacity() *
		sizeof(uint32_t);
	bytes += completion.mappedPageBlobOffsets.capacity() *
		sizeof(uint64_t);
	bytes += completion.meshPageIndices.capacity() *
		sizeof(uint32_t);
	bytes += completion.preAllocatedPages.capacity() *
		sizeof(uint32_t);
	return bytes;
}
}
