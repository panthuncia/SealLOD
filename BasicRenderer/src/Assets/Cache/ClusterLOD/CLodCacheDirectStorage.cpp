#include <BasicRenderer/Assets/Import/CLodCache.h>

// DirectStorage lives in the renderer, so these two stay out of the CLodCacheFormat library.
#if BASICRENDERER_HAS_DIRECTSTORAGE
#include "Runtime/IO/DirectStorageManager.h"
#endif

namespace CLodCache {

	bool LoadMeshPagesSelectiveDirectStorage(
		const std::wstring& containerPath,
		std::span<const ClusterLODGroupDiskLocator> pageLocators,
		uint32_t firstPage,
		uint32_t pageCount,
		const std::vector<bool>& pageNeedsFetch,
		LoadedGroupPayload& outPayload,
		std::string* outMessage)
	{
#if !BASICRENDERER_HAS_DIRECTSTORAGE
		(void)containerPath;
		(void)pageLocators;
		(void)firstPage;
		(void)pageCount;
		(void)pageNeedsFetch;
		(void)outPayload;
		if (outMessage) {
			*outMessage = "DirectStorage support is not compiled into this target";
		}
		return false;
#else
		if (outMessage) {
			outMessage->clear();
		}
		if (!DirectStorageManager::GetInstance().CanServiceQueue(DirectStorageQueueKind::SystemMemory)) {
			if (outMessage) {
				*outMessage = "DirectStorage system-memory queue unavailable";
			}
			return false;
		}
		outPayload.pageBlobs.assign(pageCount, {});
		const uint64_t endPage = static_cast<uint64_t>(firstPage) + static_cast<uint64_t>(pageCount);
		if (endPage > pageLocators.size()) {
			return false;
		}
		for (uint32_t pageOffset = 0; pageOffset < pageCount; ++pageOffset) {
			if (!pageNeedsFetch.empty() &&
				pageOffset < static_cast<uint32_t>(pageNeedsFetch.size()) &&
				!pageNeedsFetch[pageOffset]) {
				continue;
			}
			const ClusterLODGroupDiskLocator& locator = pageLocators[firstPage + pageOffset];
			std::string readMessage;
			if (!DirectStorageManager::GetInstance().ReadFileRegionToMemory(
				containerPath,
				locator.blobOffset,
				locator.blobSizeBytes,
				outPayload.pageBlobs[pageOffset],
				&readMessage)) {
				if (outMessage) {
					*outMessage = readMessage.empty() ? "DirectStorage CLod page read failed" : readMessage;
				}
				return false;
			}
		}
		if (outMessage) {
			*outMessage = "loaded selected CLod mesh pages through DirectStorage";
		}
		return true;
#endif
	}

	bool LoadMeshPagesSelectiveDirectStorage(
		const std::wstring& containerPath,
		std::span<const ClusterLODGroupDiskLocator> pageLocators,
		std::span<const uint32_t> meshPageIndices,
		const std::vector<bool>& pageNeedsFetch,
		LoadedGroupPayload& outPayload,
		std::string* outMessage)
	{
#if !BASICRENDERER_HAS_DIRECTSTORAGE
		(void)containerPath;
		(void)pageLocators;
		(void)meshPageIndices;
		(void)pageNeedsFetch;
		(void)outPayload;
		if (outMessage) {
			*outMessage = "DirectStorage support is not compiled into this target";
		}
		return false;
#else
		if (outMessage) {
			outMessage->clear();
		}
		if (!DirectStorageManager::GetInstance().CanServiceQueue(DirectStorageQueueKind::SystemMemory)) {
			if (outMessage) {
				*outMessage = "DirectStorage system-memory queue unavailable";
			}
			return false;
		}
		outPayload.pageBlobs.assign(meshPageIndices.size(), {});
		for (uint32_t pageOffset = 0; pageOffset < static_cast<uint32_t>(meshPageIndices.size()); ++pageOffset) {
			if (!pageNeedsFetch.empty() &&
				pageOffset < static_cast<uint32_t>(pageNeedsFetch.size()) &&
				!pageNeedsFetch[pageOffset]) {
				continue;
			}
			const uint32_t meshPageIndex = meshPageIndices[pageOffset];
			if (meshPageIndex >= pageLocators.size()) {
				return false;
			}
			const ClusterLODGroupDiskLocator& locator = pageLocators[meshPageIndex];
			std::string readMessage;
			if (!DirectStorageManager::GetInstance().ReadFileRegionToMemory(
				containerPath,
				locator.blobOffset,
				locator.blobSizeBytes,
				outPayload.pageBlobs[pageOffset],
				&readMessage)) {
				if (outMessage) {
					*outMessage = readMessage.empty() ? "DirectStorage CLod page read failed" : readMessage;
				}
				return false;
			}
		}
		if (outMessage) {
			*outMessage = "loaded selected CLod mesh pages through DirectStorage";
		}
		return true;
#endif
	}

}
