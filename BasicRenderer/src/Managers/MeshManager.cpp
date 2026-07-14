#include "Managers/MeshManager.h"

#include "Managers/Singletons/ResourceManager.h"
#include "Managers/Singletons/TaskSchedulerManager.h"
#include "Managers/Singletons/DirectStorageManager.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Mesh/Mesh.h"
#include "Resources/ResourceGroup.h"
#include "Resources/Buffers/BufferView.h"
#include "Mesh/MeshInstance.h"
#include "Managers/SkeletonManager.h"
#include "Resources/Buffers/DynamicBuffer.h"
#include "Resources/Buffers/PagePool.h"
#include "Managers/ViewManager.h"
#include "Import/CLodCache.h"
#include "Materials/Material.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Utilities/CachePathUtilities.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <span>
#include <unordered_set>
#include <cassert>
#include <tracy/Tracy.hpp>

#include "../../generated/BuiltinResources.h"
#include "Render/MemoryIntrospectionAPI.h"

namespace {

uint64_t CLodStreamingNowMs()
{
	using Clock = std::chrono::steady_clock;
	return static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
}

bool SarpClodImportDebugLoggingEnabled()
{
	static const bool enabled = [] {
		char* value = nullptr;
		size_t length = 0;
		if (_dupenv_s(&value, &length, "SARP_DEBUG_CLOD_IMPORT") != 0 || value == nullptr) {
			return false;
		}
		const bool result = length > 1 && value[0] != '0';
		std::free(value);
		return result;
	}();
	return enabled;
}

std::string NarrowDebugPath(const std::wstring& path)
{
	return ws2s(path);
}

size_t ReserveBytesWithImportHeadroom(size_t requestedBytes, size_t minimumHeadroomBytes) {
	if (requestedBytes == 0) {
		return 0;
	}
	return requestedBytes + std::max(requestedBytes * 3u, minimumHeadroomBytes);
}

}

MeshManager::MeshManager() {
	auto& resourceManager = ResourceManager::GetInstance();

	try {
		auto& settingsManager = SettingsManager::GetInstance();
		m_clodStreamingDirectStorageEnabled.store(
			settingsManager.getSettingGetter<bool>(CLodStreamingEnableDirectStorageSettingName)(),
			std::memory_order_release);
		m_clodStreamingDirectStorageSubscription = settingsManager.addObserver<bool>(
			CLodStreamingEnableDirectStorageSettingName,
			[this](const bool& enabled) {
				m_clodStreamingDirectStorageEnabled.store(enabled, std::memory_order_release);
			});
	}
	catch (const std::exception&) {
		m_clodStreamingDirectStorageEnabled.store(true, std::memory_order_release);
	}

	{
		auto result = DeviceManager::GetInstance().GetDevice().CreateTimeline(
			m_clodDirectStorageCompletionFencePtr,
			0,
			"CLodDirectStorageCompletionFence");
		if (result == rhi::Result::Ok && m_clodDirectStorageCompletionFencePtr) {
			m_clodDirectStorageCompletionFenceHandle = m_clodDirectStorageCompletionFencePtr.Get();
		}
	}

	m_perMeshBuffers = DynamicBuffer::CreateShared(sizeof(PerMeshCB), 1, "PerMeshBuffers");
	m_perMeshInstanceBuffers = DynamicBuffer::CreateShared(sizeof(PerMeshInstanceCB), 1, "perMeshInstanceBuffers");

	// Cluster LOD data
	m_perMeshInstanceClodOffsets = DynamicBuffer::CreateShared(sizeof(MeshInstanceClodOffsets), 10000, "perMeshInstanceClodOffsets");
	m_clodSharedGroupChunks = DynamicBuffer::CreateShared(sizeof(ClusterLODGroupChunk), 10000, "clodSharedGroupChunks");
	m_clodMeshMetadata = DynamicBuffer::CreateShared(sizeof(CLodMeshMetadata), 10000, "clodMeshMetadata");
	m_clodHierarchyLevelInfos = DynamicBuffer::CreateShared(sizeof(CLodHierarchyLevelInfo), 10000, "clodHierarchyLevelInfos");
	m_clusterLODGroups = DynamicBuffer::CreateShared(sizeof(ClusterLODGroup), 10000, "clusterLODGroups");
	m_clusterLODSegments = DynamicBuffer::CreateShared(sizeof(ClusterLODGroupSegment), 10000, "clusterLODSegments");
	//m_clusterLODMeshletBounds = DynamicBuffer::CreateShared(sizeof(BoundingSphere), 10000, "clusterLODMeshletBounds", false, true);
	m_clusterLODNodes = DynamicBuffer::CreateShared(sizeof(ClusterLODNode), 10000, "clusterLODNodes");
	m_clusterLODAssemblyTransforms = DynamicBuffer::CreateShared(sizeof(ClusterLODAssemblyTransform), 10000, "clusterLODAssemblyTransforms");
	m_clusterLODAssemblyInstances = DynamicBuffer::CreateShared(sizeof(ClusterLODAssemblyInstance), 10000, "clusterLODAssemblyInstances");
	m_clusterLODAssemblyBoneRemaps = DynamicBuffer::CreateShared(sizeof(ClusterLODAssemblyBoneRemap), 10000, "clusterLODAssemblyBoneRemaps");
	m_clusterLODAssemblyBoneRemapIndices = DynamicBuffer::CreateShared(sizeof(uint32_t), 10000, "clusterLODAssemblyBoneRemapIndices");
	m_clodGroupPageMap = DynamicBuffer::CreateShared(sizeof(GroupPageMapEntry), 10000, "clodGroupPageMap");

	m_clodSharedGroupChunks->SetUploadPolicyTag(rg::runtime::UploadPolicyTag::Coalesced);
	m_clodGroupPageMap->SetUploadPolicyTag(rg::runtime::UploadPolicyTag::Coalesced);

	// Tag resources for memory statistics
	rg::memory::SetResourceUsageHint(*m_perMeshBuffers, "PerMesh, PerMeshInstance, PerObject");
	rg::memory::SetResourceUsageHint(*m_perMeshInstanceBuffers, "PerMesh, PerMeshInstance, PerObject");
	rg::memory::SetResourceUsageHint(*m_perMeshInstanceClodOffsets, "Cluster LOD data");
	rg::memory::SetResourceUsageHint(*m_clodSharedGroupChunks, "Cluster LOD data");
	rg::memory::SetResourceUsageHint(*m_clodMeshMetadata, "Cluster LOD data");
	rg::memory::SetResourceUsageHint(*m_clodHierarchyLevelInfos, "Cluster LOD data");
	rg::memory::SetResourceUsageHint(*m_clusterLODGroups, "Cluster LOD data");
	rg::memory::SetResourceUsageHint(*m_clusterLODSegments, "Cluster LOD data");
	rg::memory::SetResourceUsageHint(*m_clusterLODNodes, "Cluster LOD data");
	rg::memory::SetResourceUsageHint(*m_clusterLODAssemblyTransforms, "Cluster LOD data");
	rg::memory::SetResourceUsageHint(*m_clusterLODAssemblyInstances, "Cluster LOD data");
	rg::memory::SetResourceUsageHint(*m_clusterLODAssemblyBoneRemaps, "Cluster LOD data");
	rg::memory::SetResourceUsageHint(*m_clusterLODAssemblyBoneRemapIndices, "Cluster LOD data");
	rg::memory::SetResourceUsageHint(*m_clodGroupPageMap, "Cluster LOD streaming");

	m_resources[Builtin::PerMeshBuffer] = m_perMeshBuffers;
	m_resources[Builtin::PerMeshInstanceBuffer] = m_perMeshInstanceBuffers;

	m_resources[Builtin::CLod::Offsets] = m_perMeshInstanceClodOffsets;
	m_resources[Builtin::CLod::GroupChunks] = m_clodSharedGroupChunks;
	m_resources[Builtin::CLod::MeshMetadata] = m_clodMeshMetadata;
	m_resources[CLodLevelInfosBufferId] = m_clodHierarchyLevelInfos;
	m_resources[Builtin::CLod::Groups] = m_clusterLODGroups;
	m_resources[Builtin::CLod::Segments] = m_clusterLODSegments;
	//m_resources[Builtin::CLod::MeshletBounds] = m_clusterLODMeshletBounds;
	m_resources[Builtin::CLod::Nodes] = m_clusterLODNodes;
	m_resources[Builtin::CLod::AssemblyTransforms] = m_clusterLODAssemblyTransforms;
	m_resources[Builtin::CLod::AssemblyInstances] = m_clusterLODAssemblyInstances;
	m_resources[Builtin::CLod::AssemblyBoneRemaps] = m_clusterLODAssemblyBoneRemaps;
	m_resources[Builtin::CLod::AssemblyBoneRemapIndices] = m_clusterLODAssemblyBoneRemapIndices;
	m_resources[Builtin::CLod::GroupPageMap] = m_clodGroupPageMap;

	// Page pool
	{
		PagePool::Config ppConfig;
		ppConfig.pageSize     = 256 * 1024;         // 256 KB
		ppConfig.slabSize     = 256 * 1024 * 1024;  // 256 MB
		ppConfig.numStreamingSlabs = 16;
		ppConfig.debugName    = "CLodPagePool";
		m_clodPagePool = std::make_unique<PagePool>(ppConfig);
	}
	rg::memory::SetResourceUsageHint(*m_clodPagePool->GetPageTableBuffer(), "Cluster LOD streaming");
	m_resources[Builtin::CLod::PageTable] = m_clodPagePool->GetPageTableBuffer();
	// Slab buffers are registered dynamically as they're allocated.
	// The PagePoolSlabBase descriptor is resolved per-pass from the first slab.

}

MeshManager::~MeshManager() {
}

void MeshManager::InvalidateCLodDiskStreamingPipeline() {
	std::vector<DirectStorageAsyncRequestHandle> pendingDirectStorageUploads;
	pendingDirectStorageUploads.reserve(m_clodPendingDirectStorageUploads.size());
	for (const auto& pendingUpload : m_clodPendingDirectStorageUploads) {
		if (pendingUpload.uploadHandle.IsValid()) {
			pendingDirectStorageUploads.push_back(pendingUpload.uploadHandle);
		}
	}
	if (!pendingDirectStorageUploads.empty()) {
		std::string waitMessage;
		if (!DirectStorageManager::GetInstance().WaitForRequests(pendingDirectStorageUploads, &waitMessage) && !waitMessage.empty()) {
			spdlog::warn("CLod streaming: DirectStorage invalidate wait reported '{}'", waitMessage);
		}
		m_clodPendingDirectStorageUploads.clear();
	}
	m_clodPendingDirectStorageLaunches.clear();

	// Bump generation so in-flight IO tasks produce stale results that will be rejected.
	m_clodDiskStreamingGeneration.fetch_add(1, std::memory_order_release);

	{
		std::lock_guard<std::mutex> lock(m_clodDiskStreamingMutex);
		m_clodDiskStreamingRequests.clear();
		m_clodDiskStreamingQueuedGroups.clear();
	}
	{
		std::lock_guard<std::mutex> lock(m_clodDiskStreamingResultsMutex);
		m_clodDiskStreamingResults.clear();
		m_clodDiskStreamingCompletions.clear();
	}
}

void MeshManager::DispatchCLodDiskStreamingBatch() {
	// Drain up to kMaxIoBatchSize highest-priority requests from the pending queue.
	std::vector<CLodDiskStreamingRequest> batch;
	{
		std::lock_guard<std::mutex> lock(m_clodDiskStreamingMutex);
		if (m_clodDiskStreamingRequests.empty()) {
			return;
		}

		const uint32_t queuedRequestCount = static_cast<uint32_t>(m_clodDiskStreamingRequests.size());
		const uint32_t queuedOrInFlightCount = static_cast<uint32_t>(m_clodDiskStreamingQueuedGroups.size());
		const uint32_t dispatchedOrInFlightCount =
			queuedOrInFlightCount > queuedRequestCount ? queuedOrInFlightCount - queuedRequestCount : 0u;
		const uint32_t adaptiveDispatchedTarget = std::clamp<uint32_t>(
			kMinAdaptiveDispatchedIoGroups + queuedRequestCount / 4u,
			kMinAdaptiveDispatchedIoGroups,
			kMaxAdaptiveDispatchedIoGroups);
		if (dispatchedOrInFlightCount >= adaptiveDispatchedTarget) {
			return;
		}

		// Sort so highest-priority requests are at the back.
		std::sort(m_clodDiskStreamingRequests.begin(), m_clodDiskStreamingRequests.end(),
			[](const CLodDiskStreamingRequest& a, const CLodDiskStreamingRequest& b) {
				return a.priority < b.priority;
			});

		const uint32_t dispatchHeadroom = adaptiveDispatchedTarget - dispatchedOrInFlightCount;
		const uint32_t toDrain = std::min<uint32_t>(
			std::min<uint32_t>(kMaxIoBatchSize, dispatchHeadroom),
			static_cast<uint32_t>(m_clodDiskStreamingRequests.size()));
		if (toDrain == 0u) {
			return;
		}
		// Take from the back (highest priority).
		batch.reserve(toDrain);
		for (uint32_t i = 0; i < toDrain; ++i) {
			batch.push_back(std::move(m_clodDiskStreamingRequests[m_clodDiskStreamingRequests.size() - 1 - i]));
		}
		m_clodDiskStreamingRequests.resize(m_clodDiskStreamingRequests.size() - toDrain);
	}

	// Dispatch each request as a fire-and-forget IO task on the dedicated IO
	// thread pool. Each task captures its request by move, performs the disk
	// read, and pushes the result directly into the shared results vector.
	auto& scheduler = TaskSchedulerManager::GetInstance();
	for (auto& request : batch) {
		scheduler.QueueIoTask("CLodDiskStreaming",
			[this, request = std::move(request)]() mutable {
			CLodDiskStreamingResult result{};
			result.groupGlobalIndex = request.groupGlobalIndex;
			result.cacheSource = request.cacheSource;
			result.segmentNeedsFetch = request.segmentNeedsFetch;
			result.meshPageIndices = request.meshPageIndices;
			result.generation = request.generation;
			const auto sharedState = request.sharedState;
			const auto* pageDiskLocators = sharedState != nullptr ? &sharedState->pageDiskLocators : nullptr;
			const uint32_t locatorCount = pageDiskLocators != nullptr ? static_cast<uint32_t>(pageDiskLocators->size()) : 0u;

			{
				ZoneScopedN("CLodDiskStreaming::ValidateInputs");
				if (pageDiskLocators == nullptr ||
					pageDiskLocators->empty() ||
					std::any_of(request.meshPageIndices.begin(), request.meshPageIndices.end(), [locatorCount](uint32_t pageIndex) { return pageIndex >= locatorCount; })) {
					result.success = false;
					std::lock_guard<std::mutex> resultsLock(m_clodDiskStreamingResultsMutex);
					m_clodDiskStreamingResults.push_back(std::move(result));
					return;
				}
			}

			struct TLContainerState {
				std::wstring containerFileName;
				std::string sourceIdentifier;
				std::ifstream file;
				uint32_t pageCount = 0;
				bool valid = false;
			};
			thread_local TLContainerState tls;
			auto ensureLegacyStreamOpen = [&]() -> bool {
				ZoneScopedN("CLodDiskStreaming::OpenLegacyStream");
				if (!tls.valid
					|| tls.containerFileName != request.cacheSource.containerFileName
					|| tls.sourceIdentifier != request.cacheSource.sourceIdentifier) {
					tls.file.close();
					tls.valid = false;
					tls.containerFileName = request.cacheSource.containerFileName;
					tls.sourceIdentifier = request.cacheSource.sourceIdentifier;
					tls.pageCount = 0;
					if (CLodCache::OpenContainerFile(request.cacheSource, tls.file, tls.pageCount)) {
						tls.valid = true;
					}
				}
				return tls.valid && tls.pageCount == locatorCount;
			};

			CLodCache::LoadedGroupPayload payload{};
			bool loaded = false;
			const std::wstring& cachedContainerPath = sharedState->resolvedContainerPath;
			std::wstring fallbackContainerPath;
			if (cachedContainerPath.empty() && !request.cacheSource.containerFileName.empty()) {
				ZoneScopedN("CLodDiskStreaming::ResolveContainerPathFallback");
				fallbackContainerPath = CLodCache::ResolveContainerPath(request.cacheSource);
			}
			const std::wstring& containerPath = cachedContainerPath.empty() ? fallbackContainerPath : cachedContainerPath;
			const bool clodDirectStorageEnabled = m_clodStreamingDirectStorageEnabled.load(std::memory_order_acquire);
			const bool clodGpuDirectStorageEnabled =
				clodDirectStorageEnabled &&
				DirectStorageManager::GetInstance().CanServiceQueue(DirectStorageQueueKind::Gpu) &&
				request.preAllocatedPages.size() == request.meshPageIndices.size();
			if (clodGpuDirectStorageEnabled && !containerPath.empty()) {
				ZoneScopedN("CLodDiskStreaming::PrepareGpuDirectStorage");
				if (request.prefetchedLayout.has_value() && request.prefetchedLayout->IsValid()) {
					result.groupChunkMetadata = request.prefetchedLayout->groupChunkMetadata;
					result.directStoragePageBlobSizes = request.prefetchedLayout->pageBlobSizes;
					result.directStoragePageBlobOffsets = request.prefetchedLayout->pageBlobOffsets;
					loaded = true;
				}

				if (!loaded) {
					CLodCache::GroupPayloadLayoutMetadata layout;
					loaded = CLodCache::GetMeshPagePayloadLayout(
						std::span<const ClusterLODGroupDiskLocator>(pageDiskLocators->data(), pageDiskLocators->size()),
						std::span<const uint32_t>(request.meshPageIndices.data(), request.meshPageIndices.size()),
						layout);
					if (loaded) {
						result.groupChunkMetadata = layout.groupChunkMetadata;
						result.directStoragePageBlobSizes = std::move(layout.pageBlobSizes);
						result.directStoragePageBlobOffsets = std::move(layout.pageBlobOffsets);
					}
				}
				if (loaded) {
					result.uploadPathLabel = "DirectStorageGpuDirect";
					result.directStorageGpuUploadPending = true;
				}
				else {
					tls.file.clear();
					spdlog::debug(
						"CLod streaming: DirectStorage GPU upload prep fallback for group {}",
						request.groupGlobalIndex);
				}
			}

			if (!loaded && !containerPath.empty()) {
				ZoneScopedN("CLodDiskStreaming::LoadMappedCpu");
				loaded = CLodCache::LoadMeshPagesSelectiveMapped(
					containerPath,
					std::span<const ClusterLODGroupDiskLocator>(pageDiskLocators->data(), pageDiskLocators->size()),
					std::span<const uint32_t>(request.meshPageIndices.data(), request.meshPageIndices.size()),
					request.segmentNeedsFetch,
					payload);
				if (loaded) {
					result.uploadPathLabel = "MemoryMappedCpuReadThenCpuUpload";
				}
			}

			if (!loaded && clodDirectStorageEnabled && DirectStorageManager::GetInstance().CanServiceQueue(DirectStorageQueueKind::SystemMemory)) {
				if (!containerPath.empty()) {
					std::string directStorageMessage;
					loaded = CLodCache::LoadMeshPagesSelectiveDirectStorage(
						containerPath,
						std::span<const ClusterLODGroupDiskLocator>(pageDiskLocators->data(), pageDiskLocators->size()),
						std::span<const uint32_t>(request.meshPageIndices.data(), request.meshPageIndices.size()),
						request.segmentNeedsFetch,
						payload,
						&directStorageMessage);
					if (loaded) {
						result.uploadPathLabel = "DirectStorageSystemMemoryThenCpuUpload";
					}
					if (!loaded) {
						tls.file.clear();
						spdlog::debug(
							"CLod streaming: DirectStorage page read fallback for group {}: {}",
							request.groupGlobalIndex,
							directStorageMessage);
					}
				}
			}

			if (!loaded) {
				ZoneScopedN("CLodDiskStreaming::LoadLegacyStream");
				if (ensureLegacyStreamOpen()) {
					loaded = CLodCache::LoadMeshPagesSelective(
						tls.file,
						std::span<const ClusterLODGroupDiskLocator>(pageDiskLocators->data(), pageDiskLocators->size()),
						std::span<const uint32_t>(request.meshPageIndices.data(), request.meshPageIndices.size()),
						request.segmentNeedsFetch,
						payload);
				}
			}

			if (loaded) {
				if (!result.directStorageGpuUploadPending) {
					result.groupChunkMetadata = payload.groupChunkMetadata;
					result.pageBlobs = std::move(payload.pageBlobs);
				}
				result.preAllocatedPages = std::move(request.preAllocatedPages);
				result.success = true;

				// Child layout prefetch is now handled on the main thread where
				// group -> mesh-page intervals are available.
			}
			else {
				result.success = false;
				tls.file.clear();
			}

			{
				ZoneScopedN("CLodDiskStreaming::PublishResult");
				std::lock_guard<std::mutex> resultsLock(m_clodDiskStreamingResultsMutex);
				m_clodDiskStreamingResults.push_back(std::move(result));
			}
		});
	}
}

bool MeshManager::AddMesh(std::shared_ptr<Mesh>& mesh, bool useMeshletReorderedVertices) {
	if (!mesh) {
		return false;
	}
	if (mesh->GetPerMeshBufferView() != nullptr) {
		return true;
	}

	mesh->SetCurrentMeshManager(this);

	const auto& pageDiskLocators = mesh->GetCLodPageDiskLocators();

	std::unique_ptr<BufferView> postSkinningView = nullptr;
	std::unique_ptr<BufferView> preSkinningView = nullptr;
	size_t vertexByteSize = mesh->GetPerMeshCBData().vertexByteSize;
	std::vector<std::unique_ptr<BufferView>> clodPreSkinningChunkViews;
	std::vector<std::unique_ptr<BufferView>> clodPostSkinningChunkViews;
	std::vector<std::unique_ptr<BufferView>> clodMeshletVertexChunkViews;
	std::vector<std::unique_ptr<BufferView>> clodCompressedPositionChunkViews;
	std::vector<std::unique_ptr<BufferView>> clodCompressedNormalChunkViews;
	std::vector<std::unique_ptr<BufferView>> clodCompressedMeshletVertexChunkViews;
 	std::vector<std::unique_ptr<BufferView>> clodMeshletChunkViews;
	std::vector<std::unique_ptr<BufferView>> clodMeshletTriangleChunkViews;
	std::vector<std::unique_ptr<BufferView>> clodMeshletBoundsChunkViews;

	const bool hasDiskBackedGroupChunks = !pageDiskLocators.empty() && mesh->HasCLodDiskStreamingSource();
	const bool hasCLodHierarchy = mesh->IsCLodMesh() &&
		!mesh->GetCLodGroups().empty() &&
		!mesh->GetCLodSegments().empty() &&
		!mesh->GetCLodNodes().empty();
	const bool hasClassicGeometry = vertexByteSize != 0u && mesh->GetPerMeshCBData().numVertices != 0u;
	const bool hasData = hasClassicGeometry || (hasCLodHierarchy && hasDiskBackedGroupChunks);
	if (!hasData) {
		std::string materialName;
		if (mesh->material) {
			materialName = mesh->material->ToCacheDescription().name;
		}
		spdlog::warn(
			"Loading mesh with no associated geometry or disk-backed CLOD payload, skipping globalID={} material='{}' clod={} groups={} segments={} nodes={} pageLocators={} hasCacheSource={}",
			mesh->GetGlobalID(),
			materialName,
			mesh->IsCLodMesh() ? 1 : 0,
			mesh->GetCLodGroups().size(),
			mesh->GetCLodSegments().size(),
			mesh->GetCLodNodes().size(),
			pageDiskLocators.size(),
			mesh->HasCLodDiskStreamingSource() ? 1 : 0);
		return true; //Empty mesh? Nothing to upload.
	}
	if (mesh->GetPerMeshCBData().vertexFlags & VertexFlags::VERTEX_SKINNED) {
		unsigned int skinningVertexByteSize = mesh->GetSkinningVertexSize();
		//preSkinningView = m_preSkinningVertices->AddData(skinningVertices.data(), numVertices * skinningVertexByteSize, skinningVertexByteSize);
	}
	else {
		//postSkinningView = m_postSkinningVertices->AddData(vertices.data(), numVertices * vertexByteSize, vertexByteSize);
		//meshletBoundsView = m_meshletBoundsBuffer->AddData(mesh->GetMeshletBounds().data(), mesh->GetMeshletCount() * sizeof(BoundingSphere), sizeof(BoundingSphere));
	}

	uint32_t totalPageMapEntries = 0;
	for (const auto& group : mesh->GetCLodGroups()) {
		const uint64_t pageEnd = static_cast<uint64_t>(group.pageMapBase) + static_cast<uint64_t>(group.pageCount);
		if (pageEnd > static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)())) {
			spdlog::warn("CLOD mesh page-map allocation exceeds uint32_t range; skipping mesh");
			return false;
		}
		totalPageMapEntries = std::max(totalPageMapEntries, static_cast<uint32_t>(pageEnd));
	}

	// Per mesh buffer
	auto perMeshBufferView = m_perMeshBuffers->AddData(&mesh->GetPerMeshCBData(), sizeof(PerMeshCB), sizeof(PerMeshCB));
	if (!perMeshBufferView) {
		spdlog::error("MeshManager::AddMesh: failed to allocate logical PerMesh buffer view for mesh globalID={}", mesh->GetGlobalID());
		return false;
	}
	mesh->SetPerMeshBufferView(std::move(perMeshBufferView));

	// Cluster LOD hierarchy data is shared per mesh; per-instance state stores only indirection/instance IDs.
	auto clusterLODGroupsView = m_clusterLODGroups->AddData(mesh->GetCLodGroups().data(), mesh->GetCLodGroups().size() * sizeof(ClusterLODGroup), sizeof(ClusterLODGroup));
	auto clusterLODSegmentsView = m_clusterLODSegments->AddData(mesh->GetCLodSegments().data(), mesh->GetCLodSegments().size() * sizeof(ClusterLODGroupSegment), sizeof(ClusterLODGroupSegment));
	
	auto clusterLODNodesView = m_clusterLODNodes->AddData(mesh->GetCLodNodes().data(), mesh->GetCLodNodes().size() * sizeof(ClusterLODNode), sizeof(ClusterLODNode));
	std::unique_ptr<BufferView> clusterLODAssemblyTransformsView = nullptr;
	if (!mesh->GetCLodAssemblyTransforms().empty()) {
		clusterLODAssemblyTransformsView = m_clusterLODAssemblyTransforms->AddData(
			mesh->GetCLodAssemblyTransforms().data(),
			mesh->GetCLodAssemblyTransforms().size() * sizeof(ClusterLODAssemblyTransform),
			sizeof(ClusterLODAssemblyTransform));
	}
	std::unique_ptr<BufferView> clusterLODAssemblyInstancesView = nullptr;
	if (!mesh->GetCLodAssemblyInstances().empty()) {
		clusterLODAssemblyInstancesView = m_clusterLODAssemblyInstances->AddData(
			mesh->GetCLodAssemblyInstances().data(),
			mesh->GetCLodAssemblyInstances().size() * sizeof(ClusterLODAssemblyInstance),
			sizeof(ClusterLODAssemblyInstance));
	}
	std::unique_ptr<BufferView> clusterLODAssemblyBoneRemapIndicesView = nullptr;
	if (!mesh->GetCLodAssemblyBoneRemapIndices().empty()) {
		clusterLODAssemblyBoneRemapIndicesView = m_clusterLODAssemblyBoneRemapIndices->AddData(
			mesh->GetCLodAssemblyBoneRemapIndices().data(),
			mesh->GetCLodAssemblyBoneRemapIndices().size() * sizeof(uint32_t),
			sizeof(uint32_t));
	}
	std::unique_ptr<BufferView> clusterLODAssemblyBoneRemapsView = nullptr;
	if (!mesh->GetCLodAssemblyBoneRemaps().empty()) {
		// CLOD wind skinning keeps vertex joint indices local to each assembly transform.
		// Validate the local-to-expanded-joint tables at the CPU/GPU upload boundary so
		// malformed ranges cannot masquerade as a matrix-orientation problem in shaders.
		if (mesh->HasBaseSkin() && mesh->GetBaseSkin()->HasWindSimulationGroups()) {
			const auto& transforms = mesh->GetCLodAssemblyTransforms();
			const auto& remaps = mesh->GetCLodAssemblyBoneRemaps();
			const auto& indices = mesh->GetCLodAssemblyBoneRemapIndices();
			const uint32_t expandedBoneCount = mesh->GetBaseSkin()->GetBoneCount();
			size_t emptyRemaps = 0;
			size_t invalidRanges = 0;
			size_t invalidTargets = 0;
			uint32_t minRemapCount = (std::numeric_limits<uint32_t>::max)();
			uint32_t maxRemapCount = 0;
			uint32_t maxTargetJoint = 0;

			for (const ClusterLODAssemblyBoneRemap& remap : remaps) {
				if (remap.remapIndexBase == CLOD_ASSEMBLY_BONE_REMAP_SENTINEL || remap.remapIndexCount == 0u) {
					++emptyRemaps;
					continue;
				}
				minRemapCount = std::min(minRemapCount, remap.remapIndexCount);
				maxRemapCount = std::max(maxRemapCount, remap.remapIndexCount);
				const uint64_t remapEnd = static_cast<uint64_t>(remap.remapIndexBase) + remap.remapIndexCount;
				if (remapEnd > indices.size()) {
					++invalidRanges;
					continue;
				}
				for (uint32_t localJoint = 0; localJoint < remap.remapIndexCount; ++localJoint) {
					const uint32_t targetJoint = indices[remap.remapIndexBase + localJoint];
					maxTargetJoint = std::max(maxTargetJoint, targetJoint);
					invalidTargets += targetJoint >= expandedBoneCount ? 1u : 0u;
				}
			}

			if (minRemapCount == (std::numeric_limits<uint32_t>::max)()) {
				minRemapCount = 0;
			}
			uint32_t maxVertexJoint = 0;
			for (uint32_t joint : mesh->GetSkinningDebugJoints()) {
				maxVertexJoint = std::max(maxVertexJoint, joint);
			}
			spdlog::info(
				"CLOD skin remap telemetry: mesh={} transforms={} remaps={} indices={} expandedBones={} empty={} invalidRanges={} invalidTargets={} remapCount=[{},{}] maxTarget={} maxVertexJoint={}",
				mesh->GetGlobalID(), transforms.size(), remaps.size(), indices.size(), expandedBoneCount,
				emptyRemaps, invalidRanges, invalidTargets, minRemapCount, maxRemapCount,
				maxTargetJoint, maxVertexJoint);
		}
		std::vector<ClusterLODAssemblyBoneRemap> gpuBoneRemaps = mesh->GetCLodAssemblyBoneRemaps();
		const uint64_t globalRemapIndexBase = clusterLODAssemblyBoneRemapIndicesView != nullptr
			? clusterLODAssemblyBoneRemapIndicesView->GetOffset() / sizeof(uint32_t)
			: 0u;
		if (globalRemapIndexBase > (std::numeric_limits<uint32_t>::max)()) {
			spdlog::error("MeshManager::AddMesh: CLOD assembly bone-remap index base exceeds uint32_t range for mesh globalID={}", mesh->GetGlobalID());
			return false;
		}
		for (ClusterLODAssemblyBoneRemap& remap : gpuBoneRemaps) {
			if (remap.remapIndexBase == CLOD_ASSEMBLY_BONE_REMAP_SENTINEL) {
				continue;
			}
			const uint64_t rebasedIndex = globalRemapIndexBase + remap.remapIndexBase;
			if (rebasedIndex > (std::numeric_limits<uint32_t>::max)()) {
				spdlog::error("MeshManager::AddMesh: CLOD assembly bone-remap index exceeds uint32_t range for mesh globalID={}", mesh->GetGlobalID());
				return false;
			}
			// remapIndexBase is mesh-local in cached/prebuilt CLOD data, but the shader
			// indexes the globally aggregated remap-index buffer. Rebase exactly once at
			// upload so every shader consumer can use the stored index directly.
			remap.remapIndexBase = static_cast<uint32_t>(rebasedIndex);
		}
		clusterLODAssemblyBoneRemapsView = m_clusterLODAssemblyBoneRemaps->AddData(
			gpuBoneRemaps.data(),
			gpuBoneRemaps.size() * sizeof(ClusterLODAssemblyBoneRemap),
			sizeof(ClusterLODAssemblyBoneRemap));
	}
	if (!clusterLODGroupsView || !clusterLODSegmentsView || !clusterLODNodesView) {
		spdlog::error("MeshManager::AddMesh: failed to allocate logical CLOD hierarchy views for mesh globalID={}", mesh->GetGlobalID());
		return false;
	}
	if ((!mesh->GetCLodAssemblyTransforms().empty() && !clusterLODAssemblyTransformsView) ||
		(!mesh->GetCLodAssemblyInstances().empty() && !clusterLODAssemblyInstancesView) ||
		(!mesh->GetCLodAssemblyBoneRemaps().empty() && !clusterLODAssemblyBoneRemapsView) ||
		(!mesh->GetCLodAssemblyBoneRemapIndices().empty() && !clusterLODAssemblyBoneRemapIndicesView)) {
		spdlog::error("MeshManager::AddMesh: failed to allocate logical CLOD assembly views for mesh globalID={}", mesh->GetGlobalID());
		return false;
	}

	// Create shared streaming state (once per mesh, before hierarchy CPU data is released)
	{
		const uint32_t groupsBase = static_cast<uint32_t>(clusterLODGroupsView->GetOffset() / sizeof(ClusterLODGroup));
		const auto& groupChunkHints = mesh->GetCLodGroupChunkHints();
		std::vector<ClusterLODGroupChunk> baselineGroupChunks(groupChunkHints.size());
		std::vector<ClusterLODGroupChunk> materializedGroupChunks(groupChunkHints.size());
		std::vector<uint8_t> groupResidentFlags(groupChunkHints.size(), 1u);

		for (size_t groupIndex = 0; groupIndex < groupChunkHints.size(); ++groupIndex)
		{
			ClusterLODGroupChunk chunk{};
			const auto& hint = groupChunkHints[groupIndex];
			const ClusterLODGroup* group = groupIndex < mesh->GetCLodGroups().size()
				? &mesh->GetCLodGroups()[groupIndex]
				: nullptr;
			const uint32_t streamablePageCount = group != nullptr ? group->pageCount : hint.pageCount;
			chunk.groupVertexCount = hint.groupVertexCount;
			chunk.meshletCount = hint.meshletCount;
			chunk.meshletTrianglesByteCount = hint.meshletTrianglesByteCount;

			// Fresh chunks with streamable pages start non-resident, so expose zero counts to the GPU.
			bool hasRuntimeChunkData = (streamablePageCount == 0u);
			baselineGroupChunks[groupIndex] = chunk;

			if (!hasRuntimeChunkData) {
				chunk.groupVertexCount = 0;
				chunk.meshletCount = 0;
				chunk.meshletTrianglesByteCount = 0;
				groupResidentFlags[groupIndex] = 0u;
			}

			materializedGroupChunks[groupIndex] = chunk;
		}

		std::unique_ptr<BufferView> sharedGroupChunksView = nullptr;
		if (!materializedGroupChunks.empty())
		{
			sharedGroupChunksView = m_clodSharedGroupChunks->AddData(
				materializedGroupChunks.data(),
				materializedGroupChunks.size() * sizeof(ClusterLODGroupChunk),
				sizeof(ClusterLODGroupChunk));
			if (!sharedGroupChunksView) {
				spdlog::error("MeshManager::AddMesh: failed to allocate logical shared group chunks view for mesh globalID={}", mesh->GetGlobalID());
				return false;
			}
		}

		auto sharedState = std::make_shared<CLodSharedStreamingState>();
		sharedState->mesh = mesh.get();
		sharedState->maxTraversalDepth = mesh->GetCLodMaxTraversalDepth();
		sharedState->vertexByteSize = static_cast<uint32_t>(mesh->GetPerMeshCBData().vertexByteSize);
		sharedState->cacheSource = mesh->GetCLodCacheSource();
		if (!sharedState->cacheSource.containerFileName.empty()) {
			sharedState->resolvedContainerPath = CLodCache::ResolveContainerPath(sharedState->cacheSource);
		}
		sharedState->pageDiskLocators = mesh->GetCLodPageDiskLocators();
		sharedState->groupChunkHints = mesh->GetCLodGroupChunkHints();

		// Move hierarchy data into the shared state before the mesh releases its CPU copies.
		sharedState->groups = mesh->GetCLodGroups();
		sharedState->segments = mesh->GetCLodSegments();
		sharedState->groupPageReferences = mesh->GetCLodGroupPageReferences();
		sharedState->groupPageReferenceOffsets = mesh->GetCLodGroupPageReferenceOffsets();

		// Cache parent-child mapping and error values for streaming snapshots.
		{
			const auto& summary = mesh->GetCLodRuntimeSummary();
			sharedState->parentGroupByLocal = summary.parentGroupByLocal;
			sharedState->childrenByLocalParent.resize(sharedState->parentGroupByLocal.size());
			for (uint32_t childLocal = 0; childLocal < static_cast<uint32_t>(sharedState->parentGroupByLocal.size()); ++childLocal) {
				const int32_t parentLocal = sharedState->parentGroupByLocal[childLocal];
				if (parentLocal < 0) {
					continue;
				}
				const uint32_t parentLocalU32 = static_cast<uint32_t>(parentLocal);
				if (parentLocalU32 >= sharedState->childrenByLocalParent.size() || parentLocalU32 == childLocal) {
					continue;
				}
				sharedState->childrenByLocalParent[parentLocalU32].push_back(childLocal);
			}
			sharedState->groupErrorByLocal = summary.groupErrorByLocal;
			sharedState->coarsestRanges = summary.coarsestRanges;
		}

		std::unique_ptr<BufferView> hierarchyLevelInfoView = nullptr;
		uint32_t hierarchyLevelInfoBase = 0;
		const auto& lodNodeRanges = mesh->GetCLodLodNodeRanges();
		const auto& lodLevelRoots = mesh->GetCLodLodLevelRoots();
		if (!lodLevelRoots.empty()) {
			std::vector<CLodHierarchyLevelInfo> levelInfos(lodLevelRoots.size());
			for (size_t levelIndex = 0; levelIndex < lodLevelRoots.size(); ++levelIndex) {
				const ClusterLODNodeRangeAlloc range = levelIndex < lodNodeRanges.size()
					? lodNodeRanges[levelIndex]
					: ClusterLODNodeRangeAlloc{};
				levelInfos[levelIndex].rootNode = lodLevelRoots[levelIndex];
				levelInfos[levelIndex].nodeRangeOffset = range.offset;
				levelInfos[levelIndex].nodeRangeCount = range.count;
			}
			hierarchyLevelInfoView = m_clodHierarchyLevelInfos->AddData(
				levelInfos.data(),
				levelInfos.size() * sizeof(CLodHierarchyLevelInfo),
				sizeof(CLodHierarchyLevelInfo));
			if (!hierarchyLevelInfoView) {
				spdlog::error("MeshManager::AddMesh: failed to allocate logical hierarchy level info view for mesh globalID={}", mesh->GetGlobalID());
				return false;
			}
			if (hierarchyLevelInfoView != nullptr) {
				hierarchyLevelInfoBase = static_cast<uint32_t>(hierarchyLevelInfoView->GetOffset() / sizeof(CLodHierarchyLevelInfo));
			}
		}

		// Allocate a contiguous range in the GroupPageMap buffer.
		std::unique_ptr<BufferView> pageMapView = nullptr;
		uint32_t pageMapGlobalBase = 0;
		if (totalPageMapEntries > 0) {
			std::vector<GroupPageMapEntry> initialPageMapEntries(totalPageMapEntries); // zero-init
			pageMapView = m_clodGroupPageMap->AddData(
				initialPageMapEntries.data(),
				totalPageMapEntries * sizeof(GroupPageMapEntry),
				sizeof(GroupPageMapEntry));
			if (!pageMapView) {
				spdlog::error("MeshManager::AddMesh: failed to allocate logical page-map view for mesh globalID={}", mesh->GetGlobalID());
				return false;
			}
			if (pageMapView) {
				pageMapGlobalBase = static_cast<uint32_t>(pageMapView->GetOffset() / sizeof(GroupPageMapEntry));
			}
		}

		CLodMeshMetadata clodMeshMetadata{};
		clodMeshMetadata.groupsBase = groupsBase;
		clodMeshMetadata.segmentsBase = static_cast<uint32_t>(clusterLODSegmentsView->GetOffset() / sizeof(ClusterLODGroupSegment));
		clodMeshMetadata.lodNodesBase = static_cast<uint32_t>(clusterLODNodesView->GetOffset() / sizeof(ClusterLODNode));
		clodMeshMetadata.rootNode = mesh->GetCLodRootNodeIndex();
		clodMeshMetadata.groupChunkTableBase = (sharedGroupChunksView != nullptr)
			? static_cast<uint32_t>(sharedGroupChunksView->GetOffset() / sizeof(ClusterLODGroupChunk))
			: 0u;
		clodMeshMetadata.groupChunkTableCount = static_cast<uint32_t>(materializedGroupChunks.size());
		clodMeshMetadata.pageMapBase = pageMapGlobalBase;
		clodMeshMetadata.lodLevelInfoBase = hierarchyLevelInfoBase;
		clodMeshMetadata.lodLevelCount = static_cast<uint32_t>(lodLevelRoots.size());
		clodMeshMetadata.maxDepth = mesh->GetCLodMaxDepth();
		clodMeshMetadata.assemblyTransformBase = clusterLODAssemblyTransformsView != nullptr
			? static_cast<uint32_t>(clusterLODAssemblyTransformsView->GetOffset() / sizeof(ClusterLODAssemblyTransform))
			: 0u;
		clodMeshMetadata.assemblyTransformCount = static_cast<uint32_t>(mesh->GetCLodAssemblyTransforms().size());
		clodMeshMetadata.assemblyInstanceBase = clusterLODAssemblyInstancesView != nullptr
			? static_cast<uint32_t>(clusterLODAssemblyInstancesView->GetOffset() / sizeof(ClusterLODAssemblyInstance))
			: 0u;
		clodMeshMetadata.assemblyInstanceCount = static_cast<uint32_t>(mesh->GetCLodAssemblyInstances().size());
		clodMeshMetadata.assemblyBoneRemapBase = clusterLODAssemblyBoneRemapsView != nullptr
			? static_cast<uint32_t>(clusterLODAssemblyBoneRemapsView->GetOffset() / sizeof(ClusterLODAssemblyBoneRemap))
			: 0u;
		clodMeshMetadata.assemblyBoneRemapCount = static_cast<uint32_t>(mesh->GetCLodAssemblyBoneRemaps().size());
		sharedState->ownedMeshMetadataView = m_clodMeshMetadata->AddData(&clodMeshMetadata, sizeof(CLodMeshMetadata), sizeof(CLodMeshMetadata));
		if (!sharedState->ownedMeshMetadataView) {
			spdlog::error("MeshManager::AddMesh: failed to allocate logical mesh metadata view for mesh globalID={}", mesh->GetGlobalID());
			return false;
		}
		if (sharedState->ownedMeshMetadataView != nullptr) {
			sharedState->clodMeshMetadataIndex = static_cast<uint32_t>(sharedState->ownedMeshMetadataView->GetOffset() / sizeof(CLodMeshMetadata));
		}
		{
			const auto& nodes = mesh->GetCLodNodes();
			const auto& lodLevelRootsForLog = mesh->GetCLodLodLevelRoots();
			const uint32_t rootNode = mesh->GetCLodRootNodeIndex();
			const ClusterLODNode* root = rootNode < nodes.size() ? &nodes[rootNode] : nullptr;
			spdlog::debug(
				"CLOD mesh upload root: mesh={} groups={} nodes={} rootNode={} rootKind={} rootChildren={} rootError={} rootCullSphere=({},{},{},{}) rootLodSphere=({},{},{},{}) lodLevels={} maxDepth={} maxTraversalDepth={}",
				mesh->GetGlobalID(),
				mesh->GetCLodGroups().size(),
				nodes.size(),
				rootNode,
				root != nullptr ? root->range.isGroup : 0xFFFFFFFFu,
				(root != nullptr && root->range.isGroup == CLOD_NODE_INTERNAL) ? (root->range.countMinusOne + 1u) : 0u,
				root != nullptr ? root->traversalMetric.maxQuadricError : 0.0f,
				root != nullptr ? root->traversalMetric.cullingSphere.x : 0.0f,
				root != nullptr ? root->traversalMetric.cullingSphere.y : 0.0f,
				root != nullptr ? root->traversalMetric.cullingSphere.z : 0.0f,
				root != nullptr ? root->traversalMetric.cullingSphere.w : 0.0f,
				root != nullptr ? root->traversalMetric.lodBoundingSphere.x : 0.0f,
				root != nullptr ? root->traversalMetric.lodBoundingSphere.y : 0.0f,
				root != nullptr ? root->traversalMetric.lodBoundingSphere.z : 0.0f,
				root != nullptr ? root->traversalMetric.lodBoundingSphere.w : 0.0f,
				lodLevelRootsForLog.size(),
				mesh->GetCLodMaxDepth(),
				mesh->GetCLodMaxTraversalDepth());
			for (uint32_t levelIndex = 0u; levelIndex < std::min<uint32_t>(static_cast<uint32_t>(lodLevelRootsForLog.size()), 8u); ++levelIndex)
			{
				const uint32_t levelRootNode = lodLevelRootsForLog[levelIndex];
				const ClusterLODNode* levelRoot = levelRootNode < nodes.size() ? &nodes[levelRootNode] : nullptr;
				spdlog::debug(
					"CLOD mesh upload lod root: mesh={} level={} node={} kind={} children={} error={} lodRadius={}",
					mesh->GetGlobalID(),
					levelIndex,
					levelRootNode,
					levelRoot != nullptr ? levelRoot->range.isGroup : 0xFFFFFFFFu,
					(levelRoot != nullptr && levelRoot->range.isGroup == CLOD_NODE_INTERNAL) ? (levelRoot->range.countMinusOne + 1u) : 0u,
					levelRoot != nullptr ? levelRoot->traversalMetric.maxQuadricError : 0.0f,
					levelRoot != nullptr ? levelRoot->traversalMetric.lodBoundingSphere.w : 0.0f);
			}
		}
		sharedState->groupsBase = groupsBase;
		sharedState->groupCount = static_cast<uint32_t>(materializedGroupChunks.size());
		sharedState->ownedGroupChunksView = std::move(sharedGroupChunksView);
		sharedState->groupChunksView = sharedState->ownedGroupChunksView.get();
		sharedState->baselineGroupChunks = std::move(baselineGroupChunks);
		sharedState->groupResidentFlags = std::move(groupResidentFlags);
		sharedState->ownedPageMapView = std::move(pageMapView);
		sharedState->pageMapGlobalBase = pageMapGlobalBase;
		sharedState->totalPageMapEntries = totalPageMapEntries;
		sharedState->pageMapEntriesCPU.resize(totalPageMapEntries);
		sharedState->residentGroupAllocations.resize(sharedState->groupCount);

		m_clodSharedStreamingStateByMesh[mesh.get()] = sharedState;
		m_clodSharedStreamingRangesDirty = true;
		if (SarpClodImportDebugLoggingEnabled()) {
			std::uint32_t coarsestGroups = 0u;
			std::uint32_t coarsestPages = 0u;
			for (const auto& range : sharedState->coarsestRanges) {
				coarsestGroups += range.groupCount;
				const auto& clodGroups = mesh->GetCLodGroups();
				const uint32_t rangeEnd = std::min<uint32_t>(
					range.firstGroup + range.groupCount,
					static_cast<uint32_t>(clodGroups.size()));
				for (uint32_t groupIndex = range.firstGroup; groupIndex < rangeEnd; ++groupIndex) {
					coarsestPages += clodGroups[groupIndex].pageCount;
				}
			}
			const ClusterLODPrebuiltData prebuilt = mesh->GetClusterLODPrebuiltData();
			spdlog::info(
				"SARPDBG AddMesh CLOD mesh={} groupsBase={} groups={} coarsestRanges={} coarsestGroups={} coarsestPages={} pages={} parts={} rootPart={} container='{}' maxTraversalDepth={} rootNode={}",
				mesh->GetGlobalID(),
				sharedState->groupsBase,
				sharedState->groupCount,
				sharedState->coarsestRanges.size(),
				coarsestGroups,
				coarsestPages,
				sharedState->pageDiskLocators.size(),
				prebuilt.partRecords.size(),
				prebuilt.rootPartIndex,
				NarrowDebugPath(sharedState->cacheSource.containerFileName),
				sharedState->maxTraversalDepth,
				mesh->GetCLodRootNodeIndex());
		}
		PublishCLodStreamingDomainEventForSharedState(CLodStreamingDomainEventKind::SharedMeshAdded, sharedState);
	}

	mesh->SetCLodBufferViews(
		std::move(clusterLODGroupsView),
		std::move(clusterLODSegmentsView),
		std::move(clusterLODNodesView),
		std::move(clusterLODAssemblyTransformsView),
		std::move(clusterLODAssemblyInstancesView),
		std::move(clusterLODAssemblyBoneRemapsView),
		std::move(clusterLODAssemblyBoneRemapIndicesView));
	mesh->ReleaseCLodChunkUploadData();
	mesh->ReleaseCLodHierarchyCpuData();
	mesh->ReleaseCLodGroupChunkMetadataCpuData();

	return true;
}

void MeshManager::RemoveMesh(Mesh* mesh) {
	if (mesh == nullptr) {
		return;
	}

	// Deallocate the per mesh buffer view
	auto& perMeshBufferView = mesh->GetPerMeshBufferView();
	if (perMeshBufferView != nullptr) {
		m_perMeshBuffers->Deallocate(perMeshBufferView.get());
	}

	if (auto sharedStateIt = m_clodSharedStreamingStateByMesh.find(mesh);
		sharedStateIt != m_clodSharedStreamingStateByMesh.end() && sharedStateIt->second) {
		sharedStateIt->second->mesh = nullptr;
	}

	mesh->SetPerMeshBufferView(nullptr);
	mesh->SetCurrentMeshManager(nullptr);
}

void MeshManager::AddMeshesBulk(const std::vector<std::shared_ptr<Mesh>>& meshes, bool useMeshletReorderedVertices) {
	ZoneScopedN("MeshManager::AddMeshesBulk");
	ZoneValue(static_cast<int64_t>(meshes.size()));
	if (meshes.empty()) {
		return;
	}

	size_t meshRowsToAdd = 0;
	size_t clodGroupsBytes = 0;
	size_t clodSegmentsBytes = 0;
	size_t clodNodesBytes = 0;
	size_t clodAssemblyTransformsBytes = 0;
	size_t clodAssemblyInstancesBytes = 0;
	size_t clodAssemblyBoneRemapsBytes = 0;
	size_t clodAssemblyBoneRemapIndicesBytes = 0;
	size_t clodSharedGroupChunkBytes = 0;
	size_t clodHierarchyLevelInfoBytes = 0;
	size_t clodMeshMetadataBytes = 0;
	size_t clodPageMapBytes = 0;

	{
		ZoneScopedN("MeshManager::AddMeshesBulk::MeasureResources");
		for (const auto& mesh : meshes) {
			if (!mesh || mesh->GetPerMeshBufferView()) {
				continue;
			}
			++meshRowsToAdd;
			clodGroupsBytes += mesh->GetCLodGroups().size() * sizeof(ClusterLODGroup);
			clodSegmentsBytes += mesh->GetCLodSegments().size() * sizeof(ClusterLODGroupSegment);
			clodNodesBytes += mesh->GetCLodNodes().size() * sizeof(ClusterLODNode);
			clodAssemblyTransformsBytes += mesh->GetCLodAssemblyTransforms().size() * sizeof(ClusterLODAssemblyTransform);
			clodAssemblyInstancesBytes += mesh->GetCLodAssemblyInstances().size() * sizeof(ClusterLODAssemblyInstance);
			clodAssemblyBoneRemapsBytes += mesh->GetCLodAssemblyBoneRemaps().size() * sizeof(ClusterLODAssemblyBoneRemap);
			clodAssemblyBoneRemapIndicesBytes += mesh->GetCLodAssemblyBoneRemapIndices().size() * sizeof(uint32_t);
			clodSharedGroupChunkBytes += mesh->GetCLodGroupChunkHints().size() * sizeof(ClusterLODGroupChunk);
			clodHierarchyLevelInfoBytes += mesh->GetCLodLodLevelRoots().size() * sizeof(CLodHierarchyLevelInfo);
			clodMeshMetadataBytes += sizeof(CLodMeshMetadata);

			uint32_t totalPageMapEntries = 0;
			for (const auto& group : mesh->GetCLodGroups()) {
				totalPageMapEntries = std::max(totalPageMapEntries, group.pageMapBase + group.pageCount);
			}
			clodPageMapBytes += static_cast<size_t>(totalPageMapEntries) * sizeof(GroupPageMapEntry);
		}
	}
	TracyPlot("MeshManager.AddMeshesBulk.MeshRows", static_cast<int64_t>(meshRowsToAdd));

	if (meshRowsToAdd == 0) {
		return;
	}

	{
		ZoneScopedN("MeshManager::AddMeshesBulk::CheckAsyncCapacity");
		const bool capacityReady =
			m_perMeshBuffers->CanAllocateBytes(meshRowsToAdd * sizeof(PerMeshCB)) &&
			m_clusterLODGroups->CanAllocateBytes(clodGroupsBytes) &&
			m_clusterLODSegments->CanAllocateBytes(clodSegmentsBytes) &&
			m_clusterLODNodes->CanAllocateBytes(clodNodesBytes) &&
			m_clusterLODAssemblyTransforms->CanAllocateBytes(clodAssemblyTransformsBytes) &&
			m_clusterLODAssemblyInstances->CanAllocateBytes(clodAssemblyInstancesBytes) &&
			m_clusterLODAssemblyBoneRemaps->CanAllocateBytes(clodAssemblyBoneRemapsBytes) &&
			m_clusterLODAssemblyBoneRemapIndices->CanAllocateBytes(clodAssemblyBoneRemapIndicesBytes) &&
			m_clodSharedGroupChunks->CanAllocateBytes(clodSharedGroupChunkBytes) &&
			m_clodHierarchyLevelInfos->CanAllocateBytes(clodHierarchyLevelInfoBytes) &&
			m_clodMeshMetadata->CanAllocateBytes(clodMeshMetadataBytes) &&
			m_clodGroupPageMap->CanAllocateBytes(clodPageMapBytes);
		TracyPlot("MeshManager.AddMeshesBulk.AsyncCapacityReady", capacityReady ? int64_t{ 1 } : int64_t{ 0 });
		if (!capacityReady) {
			m_perMeshBuffers->RequestAsyncReserveBytes(ReserveBytesWithImportHeadroom(meshRowsToAdd * sizeof(PerMeshCB), 512ull * 1024ull));
			m_clusterLODGroups->RequestAsyncReserveBytes(ReserveBytesWithImportHeadroom(clodGroupsBytes, 2ull * 1024ull * 1024ull));
			m_clusterLODSegments->RequestAsyncReserveBytes(ReserveBytesWithImportHeadroom(clodSegmentsBytes, 512ull * 1024ull));
			m_clusterLODNodes->RequestAsyncReserveBytes(ReserveBytesWithImportHeadroom(clodNodesBytes, 2ull * 1024ull * 1024ull));
			m_clusterLODAssemblyTransforms->RequestAsyncReserveBytes(ReserveBytesWithImportHeadroom(clodAssemblyTransformsBytes, 512ull * 1024ull));
			m_clusterLODAssemblyInstances->RequestAsyncReserveBytes(ReserveBytesWithImportHeadroom(clodAssemblyInstancesBytes, 512ull * 1024ull));
			m_clusterLODAssemblyBoneRemaps->RequestAsyncReserveBytes(ReserveBytesWithImportHeadroom(clodAssemblyBoneRemapsBytes, 512ull * 1024ull));
			m_clusterLODAssemblyBoneRemapIndices->RequestAsyncReserveBytes(ReserveBytesWithImportHeadroom(clodAssemblyBoneRemapIndicesBytes, 512ull * 1024ull));
			m_clodSharedGroupChunks->RequestAsyncReserveBytes(ReserveBytesWithImportHeadroom(clodSharedGroupChunkBytes, 512ull * 1024ull));
			m_clodHierarchyLevelInfos->RequestAsyncReserveBytes(ReserveBytesWithImportHeadroom(clodHierarchyLevelInfoBytes, 256ull * 1024ull));
			m_clodMeshMetadata->RequestAsyncReserveBytes(ReserveBytesWithImportHeadroom(clodMeshMetadataBytes, 256ull * 1024ull));
			m_clodGroupPageMap->RequestAsyncReserveBytes(ReserveBytesWithImportHeadroom(clodPageMapBytes, 512ull * 1024ull));
			return;
		}
	}

	{
		ZoneScopedN("MeshManager::AddMeshesBulk::AddMeshes");
		for (auto mesh : meshes) {
			if (!mesh || mesh->GetPerMeshBufferView()) {
				continue;
			}
			AddMesh(mesh, useMeshletReorderedVertices);
		}
	}
}

void MeshManager::PrepareStaticMeshTemplateResourcesAsync(const std::vector<StaticMeshTemplateRequest>& requests) {
	ZoneScopedN("MeshManager::PrepareStaticMeshTemplateResourcesAsync");
	ZoneValue(static_cast<int64_t>(requests.size()));
	if (requests.empty()) {
		return;
	}

	size_t meshRowsToAdd = 0;
	size_t clodGroupsBytes = 0;
	size_t clodSegmentsBytes = 0;
	size_t clodNodesBytes = 0;
	size_t clodAssemblyTransformsBytes = 0;
	size_t clodAssemblyInstancesBytes = 0;
	size_t clodAssemblyBoneRemapsBytes = 0;
	size_t clodAssemblyBoneRemapIndicesBytes = 0;
	size_t clodSharedGroupChunkBytes = 0;
	size_t clodHierarchyLevelInfoBytes = 0;
	size_t clodMeshMetadataBytes = 0;
	size_t clodPageMapBytes = 0;
	size_t templateRowsToAdd = 0;
	std::unordered_set<Mesh*> uniqueMeshes;
	uniqueMeshes.reserve(requests.size());

	{
		ZoneScopedN("MeshManager::PrepareStaticMeshTemplateResourcesAsync::MeasureResources");
		for (const auto& request : requests) {
			const auto& mesh = request.mesh;
			if (!mesh) {
				continue;
			}
			if (uniqueMeshes.insert(mesh.get()).second && !mesh->GetPerMeshBufferView()) {
				++meshRowsToAdd;
				clodGroupsBytes += mesh->GetCLodGroups().size() * sizeof(ClusterLODGroup);
				clodSegmentsBytes += mesh->GetCLodSegments().size() * sizeof(ClusterLODGroupSegment);
				clodNodesBytes += mesh->GetCLodNodes().size() * sizeof(ClusterLODNode);
				clodAssemblyTransformsBytes += mesh->GetCLodAssemblyTransforms().size() * sizeof(ClusterLODAssemblyTransform);
				clodAssemblyInstancesBytes += mesh->GetCLodAssemblyInstances().size() * sizeof(ClusterLODAssemblyInstance);
				clodAssemblyBoneRemapsBytes += mesh->GetCLodAssemblyBoneRemaps().size() * sizeof(ClusterLODAssemblyBoneRemap);
				clodAssemblyBoneRemapIndicesBytes += mesh->GetCLodAssemblyBoneRemapIndices().size() * sizeof(uint32_t);
				clodSharedGroupChunkBytes += mesh->GetCLodGroupChunkHints().size() * sizeof(ClusterLODGroupChunk);
				clodHierarchyLevelInfoBytes += mesh->GetCLodLodLevelRoots().size() * sizeof(CLodHierarchyLevelInfo);
				clodMeshMetadataBytes += sizeof(CLodMeshMetadata);

				uint32_t totalPageMapEntries = 0;
				for (const auto& group : mesh->GetCLodGroups()) {
					totalPageMapEntries = std::max(totalPageMapEntries, group.pageMapBase + group.pageCount);
				}
				clodPageMapBytes += static_cast<size_t>(totalPageMapEntries) * sizeof(GroupPageMapEntry);
			}
			if (request.material) {
				++templateRowsToAdd;
			}
		}
	}
	TracyPlot("MeshManager.StaticTemplate.AsyncMeshRows", static_cast<int64_t>(meshRowsToAdd));
	TracyPlot("MeshManager.StaticTemplate.AsyncTemplateRows", static_cast<int64_t>(templateRowsToAdd));

	if (meshRowsToAdd != 0) {
		ZoneScopedN("MeshManager::PrepareStaticMeshTemplateResourcesAsync::RequestMeshResizes");
		m_perMeshBuffers->RequestAsyncReserveBytes(ReserveBytesWithImportHeadroom(meshRowsToAdd * sizeof(PerMeshCB), 512ull * 1024ull));
		m_clusterLODGroups->RequestAsyncReserveBytes(ReserveBytesWithImportHeadroom(clodGroupsBytes, 2ull * 1024ull * 1024ull));
		m_clusterLODSegments->RequestAsyncReserveBytes(ReserveBytesWithImportHeadroom(clodSegmentsBytes, 512ull * 1024ull));
		m_clusterLODNodes->RequestAsyncReserveBytes(ReserveBytesWithImportHeadroom(clodNodesBytes, 2ull * 1024ull * 1024ull));
		m_clusterLODAssemblyTransforms->RequestAsyncReserveBytes(ReserveBytesWithImportHeadroom(clodAssemblyTransformsBytes, 512ull * 1024ull));
		m_clusterLODAssemblyInstances->RequestAsyncReserveBytes(ReserveBytesWithImportHeadroom(clodAssemblyInstancesBytes, 512ull * 1024ull));
		m_clusterLODAssemblyBoneRemaps->RequestAsyncReserveBytes(ReserveBytesWithImportHeadroom(clodAssemblyBoneRemapsBytes, 512ull * 1024ull));
		m_clusterLODAssemblyBoneRemapIndices->RequestAsyncReserveBytes(ReserveBytesWithImportHeadroom(clodAssemblyBoneRemapIndicesBytes, 512ull * 1024ull));
		m_clodSharedGroupChunks->RequestAsyncReserveBytes(ReserveBytesWithImportHeadroom(clodSharedGroupChunkBytes, 512ull * 1024ull));
		m_clodHierarchyLevelInfos->RequestAsyncReserveBytes(ReserveBytesWithImportHeadroom(clodHierarchyLevelInfoBytes, 256ull * 1024ull));
		m_clodMeshMetadata->RequestAsyncReserveBytes(ReserveBytesWithImportHeadroom(clodMeshMetadataBytes, 256ull * 1024ull));
		m_clodGroupPageMap->RequestAsyncReserveBytes(ReserveBytesWithImportHeadroom(clodPageMapBytes, 512ull * 1024ull));
	}
	if (templateRowsToAdd != 0) {
		ZoneScopedN("MeshManager::PrepareStaticMeshTemplateResourcesAsync::RequestTemplateResizes");
		m_perMeshInstanceBuffers->RequestAsyncReserveBytes(ReserveBytesWithImportHeadroom(templateRowsToAdd * sizeof(PerMeshInstanceCB), 256ull * 1024ull));
		m_perMeshInstanceClodOffsets->RequestAsyncReserveBytes(ReserveBytesWithImportHeadroom(templateRowsToAdd * sizeof(MeshInstanceClodOffsets), 256ull * 1024ull));
	}
}

std::vector<MeshManager::StaticMeshTemplateRegistration> MeshManager::AddStaticMeshTemplatesBulk(const std::vector<StaticMeshTemplateRequest>& requests) {
	ZoneScopedN("MeshManager::AddStaticMeshTemplatesBulk");
	ZoneValue(static_cast<int64_t>(requests.size()));
	std::vector<StaticMeshTemplateRegistration> registrations(requests.size());
	if (requests.empty()) {
		return registrations;
	}

	std::vector<PerMeshInstanceCB> perMeshInstanceRows;
	std::vector<MeshInstanceClodOffsets> clodOffsetRows;
	std::vector<size_t> validRequestIndices;
	std::vector<std::shared_ptr<CLodSharedStreamingState>> sharedStates;
	perMeshInstanceRows.reserve(requests.size());
	clodOffsetRows.reserve(requests.size());
	validRequestIndices.reserve(requests.size());
	sharedStates.reserve(requests.size());

	{
		ZoneScopedN("MeshManager::AddStaticMeshTemplatesBulk::BuildRows");
		for (size_t requestIndex = 0; requestIndex < requests.size(); ++requestIndex) {
			const auto& request = requests[requestIndex];
			if (!request.mesh || !request.mesh->GetPerMeshBufferView()) {
				continue;
			}

			PerMeshInstanceCB row{};
			row.boundingSphere = request.mesh->GetPerMeshCBData().boundingSphere;
			row.skinningInstanceSlot = 0xFFFFFFFFu;
			row.skinnedBoundsScale = 1.0f;
			if ((request.mesh->GetPerMeshCBData().vertexFlags & VertexFlags::VERTEX_SKINNED) != 0u &&
				(!request.mesh->HasBaseSkin() || !request.mesh->GetBaseSkin()->HasWindSimulationGroups())) {
				static std::atomic_uint32_t loggedMissingWindSkin{ 0u };
				if (loggedMissingWindSkin.fetch_add(1u, std::memory_order_relaxed) < 16u) {
					spdlog::info("MeshManager: skinned static template hasBaseSkin={} windGroups={}.",
						request.mesh->HasBaseSkin(),
						request.mesh->HasBaseSkin() && request.mesh->GetBaseSkin()->HasWindSimulationGroups());
				}
			}
			if (m_skeletonManager && request.mesh->HasBaseSkin() &&
				request.mesh->GetBaseSkin()->HasWindSimulationGroups()) {
				const auto& base = request.mesh->GetBaseSkin();
				auto [it, inserted] = m_windTypeSkeletons.try_emplace(base.get());
				if (inserted || !it->second) {
					it->second = base->CopySkeleton();
					m_skeletonManager->AcquireSkinningInstance(it->second);
				}
				row.skinningInstanceSlot = it->second->GetSkinningInstanceSlot();
				row.skinnedBoundsScale = it->second->GetCurrentAnimationConservativeBoundsScale();
				if (inserted) {
					spdlog::info(
						"MeshManager: registered static procedural-wind template type slot={} bones={} vertexFlags=0x{:X} skinned={}.",
						row.skinningInstanceSlot,
						it->second->GetBoneCount(),
						request.mesh->GetPerMeshCBData().vertexFlags,
						(request.mesh->GetPerMeshCBData().vertexFlags & VertexFlags::VERTEX_SKINNED) != 0u);
				}
			}
			row.perMeshBufferIndex = static_cast<uint32_t>(request.mesh->GetPerMeshBufferView()->GetOffset() / sizeof(PerMeshCB));

			std::shared_ptr<CLodSharedStreamingState> sharedState;
			if (auto sharedIt = m_clodSharedStreamingStateByMesh.find(request.mesh.get()); sharedIt != m_clodSharedStreamingStateByMesh.end()) {
				sharedState = sharedIt->second;
			}

			MeshInstanceClodOffsets clodOffsets{};
			clodOffsets.clodMeshMetadataIndex = sharedState ? sharedState->clodMeshMetadataIndex : 0u;

			validRequestIndices.push_back(requestIndex);
			perMeshInstanceRows.push_back(row);
			clodOffsetRows.push_back(clodOffsets);
			sharedStates.push_back(std::move(sharedState));
		}
	}
	TracyPlot("MeshManager.StaticTemplate.ValidRows", static_cast<int64_t>(validRequestIndices.size()));

	if (perMeshInstanceRows.empty()) {
		return registrations;
	}

	const auto perMeshInstanceBytes = perMeshInstanceRows.size() * sizeof(PerMeshInstanceCB);
	const auto clodOffsetBytes = clodOffsetRows.size() * sizeof(MeshInstanceClodOffsets);
	{
		ZoneScopedN("MeshManager::AddStaticMeshTemplatesBulk::CheckAsyncCapacity");
		const bool capacityReady =
			m_perMeshInstanceBuffers->CanAllocateBytes(perMeshInstanceBytes) &&
			m_perMeshInstanceClodOffsets->CanAllocateBytes(clodOffsetBytes);
		TracyPlot("MeshManager.StaticTemplate.AsyncCapacityReady", capacityReady ? int64_t{ 1 } : int64_t{ 0 });
		if (!capacityReady) {
			m_perMeshInstanceBuffers->RequestAsyncReserveBytes(ReserveBytesWithImportHeadroom(perMeshInstanceBytes, 256ull * 1024ull));
			m_perMeshInstanceClodOffsets->RequestAsyncReserveBytes(ReserveBytesWithImportHeadroom(clodOffsetBytes, 256ull * 1024ull));
			for (const auto requestIndex : validRequestIndices) {
				registrations[requestIndex].pendingResources = true;
			}
			return registrations;
		}
	}

	std::pair<size_t, size_t> perMeshInstanceRange;
	std::pair<size_t, size_t> clodOffsetRange;
	{
		ZoneScopedN("MeshManager::AddStaticMeshTemplatesBulk::UploadInstanceRows");
		perMeshInstanceRange = m_perMeshInstanceBuffers->AddDataRange(
			perMeshInstanceRows.data(),
			perMeshInstanceRows.size(),
			sizeof(PerMeshInstanceCB));
	}
	{
		ZoneScopedN("MeshManager::AddStaticMeshTemplatesBulk::UploadClodOffsetRows");
		clodOffsetRange = m_perMeshInstanceClodOffsets->AddDataRange(
			clodOffsetRows.data(),
			clodOffsetRows.size(),
			sizeof(MeshInstanceClodOffsets));
	}

	{
		ZoneScopedN("MeshManager::AddStaticMeshTemplatesBulk::BuildRegistrations");
		for (size_t rowIndex = 0; rowIndex < validRequestIndices.size(); ++rowIndex) {
			const auto requestIndex = validRequestIndices[rowIndex];
			const auto& request = requests[requestIndex];
			const auto meshTemplateIndex = static_cast<uint32_t>(
				(perMeshInstanceRange.first + rowIndex * sizeof(PerMeshInstanceCB)) / sizeof(PerMeshInstanceCB));
			const auto clodOffsetIndex = static_cast<uint32_t>(
				(clodOffsetRange.first + rowIndex * sizeof(MeshInstanceClodOffsets)) / sizeof(MeshInstanceClodOffsets));

			registrations[requestIndex].meshTemplateIndex = meshTemplateIndex;
			registrations[requestIndex].clodOffsetIndex = clodOffsetIndex;
			registrations[requestIndex].skinnedAssemblyTypeSlot = perMeshInstanceRows[rowIndex].skinningInstanceSlot;
			registrations[requestIndex].skinnedAssemblyBounds = perMeshInstanceRows[rowIndex].boundingSphere;
			registrations[requestIndex].skinnedBoundsScale = perMeshInstanceRows[rowIndex].skinnedBoundsScale;
			registrations[requestIndex].valid = true;

			if (request.mesh) {
				m_activeMeshletCount += request.mesh->GetCLodMeshletCount();
			}

			auto& sharedState = sharedStates[rowIndex];
			if (sharedState) {
				const bool wasInactive = sharedState->activeInstanceCount == 0u;
				sharedState->activeInstanceCount++;
				if (wasInactive) {
					const uint32_t meshTraversalDepth = sharedState->maxTraversalDepth;
					uint32_t cachedDepth = m_clodActiveMaxTraversalDepth.load(std::memory_order_acquire);
					while (meshTraversalDepth > cachedDepth
						&& !m_clodActiveMaxTraversalDepth.compare_exchange_weak(
							cachedDepth,
							meshTraversalDepth,
							std::memory_order_release,
							std::memory_order_acquire)) {
					}
				}

				if (sharedState->groupCount > 0u) {
					CLodStreamingInstanceState state{};
					state.instance = nullptr;
					state.meshInstanceIndex = meshTemplateIndex;
					state.groupsBase = sharedState->groupsBase;
					state.groupCount = sharedState->groupCount;
					state.sharedMeshState = sharedState;
					m_clodStreamingStateByInstanceIndex[state.meshInstanceIndex] = std::move(state);
					if (wasInactive) {
						PublishCLodStreamingDomainEventForSharedState(CLodStreamingDomainEventKind::ActiveRangeAdded, sharedState);
					}
				}
			}
		}
	}

	return registrations;
}

bool MeshManager::AddMeshInstance(MeshInstance* mesh, bool useMeshletReorderedVertices) {
	if (mesh == nullptr || !mesh->GetMesh()) {
		return false;
	}
	if (mesh->GetPerMeshInstanceBufferView() != nullptr) {
		return true;
	}
	if (!mesh->GetMesh()->GetPerMeshBufferView()) {
		auto baseMesh = mesh->GetMesh();
		if (!AddMesh(baseMesh, useMeshletReorderedVertices) || !baseMesh->GetPerMeshBufferView()) {
			spdlog::warn("MeshManager::AddMeshInstance: base mesh registration unavailable for mesh instance");
			return false;
		}
	}

	mesh->SetCurrentMeshManager(this);
	(void)useMeshletReorderedVertices;

	auto perMeshInstanceBufferView = m_perMeshInstanceBuffers->AddData(&mesh->GetPerMeshInstanceBufferData(), sizeof(PerMeshInstanceCB), sizeof(PerMeshInstanceCB));
	if (!perMeshInstanceBufferView) {
		spdlog::error("MeshManager::AddMeshInstance: failed to allocate logical per-mesh-instance buffer view");
		return false;
	}
	mesh->SetBufferViewUsingBaseMesh(std::move(perMeshInstanceBufferView));

	uint32_t bitsToAllocate = mesh->GetMesh()->GetCLodMeshletCount();
	m_activeMeshletCount += bitsToAllocate;

	auto& overridePerMeshView = mesh->GetPerMeshOverrideBufferView();
	uint32_t perMeshIndex = overridePerMeshView
		? static_cast<uint32_t>(overridePerMeshView->GetOffset() / sizeof(PerMeshCB))
		: static_cast<uint32_t>(mesh->GetMesh()->GetPerMeshBufferView()->GetOffset() / sizeof(PerMeshCB));
	mesh->SetPerMeshBufferIndex(perMeshIndex);

	auto meshPtr = mesh->GetMesh().get();
	auto sharedStateIt = m_clodSharedStreamingStateByMesh.find(meshPtr);
	std::shared_ptr<CLodSharedStreamingState> sharedState;
	if (sharedStateIt != m_clodSharedStreamingStateByMesh.end()) {
		sharedState = sharedStateIt->second;
	}

	bool sharedStateWasInactive = false;
	if (sharedState) {
		sharedStateWasInactive = sharedState->activeInstanceCount == 0u;
		sharedState->activeInstanceCount++;
		if (sharedStateWasInactive) {
			const uint32_t meshTraversalDepth = sharedState->maxTraversalDepth;
			uint32_t cachedDepth = m_clodActiveMaxTraversalDepth.load(std::memory_order_acquire);
			while (meshTraversalDepth > cachedDepth
				&& !m_clodActiveMaxTraversalDepth.compare_exchange_weak(
					cachedDepth,
					meshTraversalDepth,
					std::memory_order_release,
					std::memory_order_acquire)) {
			}
		}
	}

	MeshInstanceClodOffsets clodOffsets = {};
	clodOffsets.clodMeshMetadataIndex = (sharedState != nullptr) ? sharedState->clodMeshMetadataIndex : 0u;
	//clodOffsets.rootGroup = mesh->GetMesh()->GetCLodRootGroup();
	auto clodOffsetsView = m_perMeshInstanceClodOffsets->AddData(&clodOffsets, sizeof(MeshInstanceClodOffsets), sizeof(MeshInstanceClodOffsets)); // Indexable by mesh instance
	if (!clodOffsetsView) {
		spdlog::error("MeshManager::AddMeshInstance: failed to allocate logical CLOD offsets view");
		return false;
	}

	mesh->SetCLodBufferViews(std::move(clodOffsetsView));

	if (sharedState != nullptr && sharedState->groupCount > 0u) {
		CLodStreamingInstanceState state{};
		state.instance = mesh;
		state.meshInstanceIndex = static_cast<uint32_t>(mesh->GetPerMeshInstanceBufferOffset() / sizeof(PerMeshInstanceCB));
		state.groupsBase = sharedState->groupsBase;
		state.groupCount = sharedState->groupCount;
		state.sharedMeshState = sharedState;

		m_clodStreamingStateByInstanceIndex[state.meshInstanceIndex] = std::move(state);
		m_clodStreamingInstanceIndexByPtr[mesh] = static_cast<uint32_t>(mesh->GetPerMeshInstanceBufferOffset() / sizeof(PerMeshInstanceCB));
		if (sharedStateWasInactive) {
			if (SarpClodImportDebugLoggingEnabled()) {
				spdlog::info(
					"SARPDBG AddMeshInstance ActiveRangeAdded mesh={} meshInstanceIndex={} groupsBase={} groups={} coarsestRanges={}",
					mesh->GetMesh()->GetGlobalID(),
					static_cast<uint32_t>(mesh->GetPerMeshInstanceBufferOffset() / sizeof(PerMeshInstanceCB)),
					sharedState->groupsBase,
					sharedState->groupCount,
					sharedState->coarsestRanges.size());
			}
			PublishCLodStreamingDomainEventForSharedState(CLodStreamingDomainEventKind::ActiveRangeAdded, sharedState);
		}
	}
	return true;
}

void MeshManager::RemoveMeshInstance(MeshInstance* mesh) {

	// Things to remove:
	// - Post-skinning vertices
	// - Per-mesh instance buffer
	// - Meshlet bounds

	auto perMeshInstanceBufferView = mesh->GetPerMeshInstanceBufferView();
	if (perMeshInstanceBufferView != nullptr) {
		m_perMeshInstanceBuffers->Deallocate(perMeshInstanceBufferView);
	}
	mesh->SetBufferViews(nullptr);
	m_activeMeshletCount -= mesh->GetMesh()->GetCLodMeshletCount();

	auto clodBuffersView = mesh->GetCLodOffsetsView();
	if (clodBuffersView != nullptr) {
		m_perMeshInstanceClodOffsets->Deallocate(clodBuffersView);
	}
	mesh->SetCLodBufferViews(nullptr);

	auto itLookup = m_clodStreamingInstanceIndexByPtr.find(mesh);
	if (itLookup != m_clodStreamingInstanceIndexByPtr.end()) {
		auto itState = m_clodStreamingStateByInstanceIndex.find(itLookup->second);
		if (itState != m_clodStreamingStateByInstanceIndex.end()) {
			auto sharedMeshState = itState->second.sharedMeshState;
			if (sharedMeshState != nullptr && sharedMeshState->activeInstanceCount > 0u) {
				sharedMeshState->activeInstanceCount--;
				if (sharedMeshState->activeInstanceCount == 0u) {
					const uint32_t removedTraversalDepth = sharedMeshState->maxTraversalDepth;
					// Keep shared CLod template/range state alive after the last
					// instance leaves. CLodStreamingSystem owns delayed page
					// residency and page-map clearing for these global group
					// indices; deleting the range here can strand stale streaming
					// state until a later allocation reuses the same indices.
					// TODO: add an explicit streaming-system retire callback if
					// the asset cache starts evicting shared Mesh objects.
					m_clodSharedStreamingRangesDirty = true;
					PublishCLodStreamingDomainEventForSharedState(CLodStreamingDomainEventKind::ActiveRangeRemoved, sharedMeshState);
					if (removedTraversalDepth >= m_clodActiveMaxTraversalDepth.load(std::memory_order_acquire)) {
						RecomputeCLodActiveMaxTraversalDepth();
					}
				}
			}
		}
		m_clodStreamingStateByInstanceIndex.erase(itLookup->second);
		m_clodStreamingInstanceIndexByPtr.erase(itLookup);
	}
}

void MeshManager::RecomputeCLodActiveMaxTraversalDepth()
{
	uint32_t maxTraversalDepth = 0u;
	for (const auto& [_, sharedState] : m_clodSharedStreamingStateByMesh) {
		if (sharedState == nullptr || sharedState->activeInstanceCount == 0u) {
			continue;
		}

		maxTraversalDepth = std::max(maxTraversalDepth, sharedState->maxTraversalDepth);
	}

	m_clodActiveMaxTraversalDepth.store(maxTraversalDepth, std::memory_order_release);
}

void MeshManager::ProcessCLodDiskStreamingIO() {
	ZoneScopedN("MeshManager::ProcessCLodDiskStreamingIO");

	// Dispatch pending IO requests across the task scheduler's IO workers.
	{
		ZoneScopedN("MeshManager::ProcessCLodDiskStreamingIO::DispatchBatch");
		DispatchCLodDiskStreamingBatch();
	}

	// Drain completed results into a local vector under the results lock.
	std::vector<CLodDiskStreamingResult> localResults;
	{
		ZoneScopedN("MeshManager::ProcessCLodDiskStreamingIO::DrainResults");
		std::lock_guard<std::mutex> resultsLock(m_clodDiskStreamingResultsMutex);
		if (!m_clodDiskStreamingResults.empty()) {
			localResults = std::move(m_clodDiskStreamingResults);
			m_clodDiskStreamingResults.clear();
		}
	}

	const uint64_t currentGeneration = m_clodDiskStreamingGeneration.load(std::memory_order_acquire);
	std::vector<CLodDiskStreamingCompletion> newCompletions;
	std::vector<uint32_t> finishedGroups;
	newCompletions.reserve(localResults.size() + m_clodPendingDirectStorageUploads.size());
	finishedGroups.reserve(localResults.size() + m_clodPendingDirectStorageUploads.size());

	{
		ZoneScopedN("MeshManager::ProcessCLodDiskStreamingIO::ApplyResults");
		for (auto& result : localResults) {
			// Reject stale results from a previous generation (pre-rebuild IO).
			if (result.generation != currentGeneration) {
				spdlog::info("CLod streaming: rejecting stale IO result for group {} (gen {} vs current {})",
					result.groupGlobalIndex, result.generation, currentGeneration);
				newCompletions.push_back({ result.groupGlobalIndex, false });
				finishedGroups.push_back(result.groupGlobalIndex);
				continue;
			}

			DiskStreamingApplyResult applyResult;
			{
				ZoneScopedN("MeshManager::ProcessCLodDiskStreamingIO::ApplyResults::ApplyOne");
				std::lock_guard<std::mutex> residencyLock(m_clodResidencyMutex);
				CLodDiskStreamingCompletion completion{};
				applyResult = PrepareCompletedCLodDiskStreamingResult(result, result.preAllocatedPages, completion);
				if (applyResult == DiskStreamingApplyResult::Prepared) {
					newCompletions.push_back(std::move(completion));
				}
			}

			if (applyResult == DiskStreamingApplyResult::DeferredPendingUpload) {
				continue;
			}
			if (applyResult == DiskStreamingApplyResult::FailedPermanent) {
				newCompletions.push_back({ result.groupGlobalIndex, false });
			}
			finishedGroups.push_back(result.groupGlobalIndex);
		}
	}

	{
		ZoneScopedN("MeshManager::ProcessCLodDiskStreamingIO::FinalizeDirectStorageUploads");
		FinalizePendingCLodDirectStorageUploads(currentGeneration, newCompletions, finishedGroups);
	}

	if (!finishedGroups.empty()) {
		ZoneScopedN("MeshManager::ProcessCLodDiskStreamingIO::ClearQueuedGroups");
		std::lock_guard<std::mutex> lock(m_clodDiskStreamingMutex);
		for (uint32_t groupGlobalIndex : finishedGroups) {
			m_clodDiskStreamingQueuedGroups.erase(groupGlobalIndex);
		}
	}

	if (!newCompletions.empty()) {
		ZoneScopedN("MeshManager::ProcessCLodDiskStreamingIO::PublishCompletions");
		std::lock_guard<std::mutex> resultsLock(m_clodDiskStreamingResultsMutex);
		for (auto& completion : newCompletions) {
			m_clodDiskStreamingCompletions.push_back(std::move(completion));
		}
	}

	if (SarpClodImportDebugLoggingEnabled() &&
		(!localResults.empty() || !newCompletions.empty() || !finishedGroups.empty() || HasPendingCLodDirectStorageLaunches() || HasPendingCLodDirectStorageUploads())) {
		spdlog::info(
			"SARPDBG ProcessCLodDiskStreamingIO localResults={} newCompletions={} finishedGroups={} pendingLaunches={} pendingUploads={}",
			localResults.size(),
			newCompletions.size(),
			finishedGroups.size(),
			HasPendingCLodDirectStorageLaunches() ? 1 : 0,
			HasPendingCLodDirectStorageUploads() ? 1 : 0);
	}
}



void MeshManager::RebuildCLodSharedStreamingRangeIndex() {
	if (!m_clodSharedStreamingRangesDirty) {
		return;
	}

	m_clodSharedStreamingRanges.clear();
	m_clodSharedStreamingRanges.reserve(m_clodSharedStreamingStateByMesh.size());

	for (const auto& [_, sharedState] : m_clodSharedStreamingStateByMesh) {
		if (sharedState == nullptr || sharedState->groupCount == 0u) {
			continue;
		}

		CLodSharedStreamingRange range{};
		range.begin = sharedState->groupsBase;
		range.end = sharedState->groupsBase + sharedState->groupCount;
		range.state = sharedState;
		m_clodSharedStreamingRanges.push_back(std::move(range));
	}

	std::sort(m_clodSharedStreamingRanges.begin(), m_clodSharedStreamingRanges.end(), [](const CLodSharedStreamingRange& a, const CLodSharedStreamingRange& b) {
		return a.begin < b.begin;
	});

	m_clodSharedStreamingRangesDirty = false;
}

void MeshManager::PublishCLodStreamingDomainEvent(CLodStreamingDomainEvent event) {
	if (event.groupCount == 0u && event.kind != CLodStreamingDomainEventKind::FullReset) {
		return;
	}

	{
		std::lock_guard lock(m_clodStreamingDomainEventsMutex);
		m_clodStreamingDomainEvents.push_back(std::move(event));
	}
	m_clodStreamingDomainEventGeneration.fetch_add(1u, std::memory_order_release);
}

void MeshManager::PublishCLodStreamingDomainEventForSharedState(
	CLodStreamingDomainEventKind kind,
	const std::shared_ptr<CLodSharedStreamingState>& sharedState) {
	if (sharedState == nullptr || sharedState->groupCount == 0u) {
		return;
	}

	CLodStreamingDomainEvent event{};
	event.kind = kind;
	event.groupsBase = sharedState->groupsBase;
	event.groupCount = sharedState->groupCount;
	event.coarsestRanges.reserve(sharedState->coarsestRanges.size());
	for (const auto& localRange : sharedState->coarsestRanges) {
		if (localRange.groupCount == 0u || localRange.firstGroup >= sharedState->groupCount) {
			continue;
		}
		const uint32_t clampedCount = std::min<uint32_t>(
			localRange.groupCount,
			sharedState->groupCount - localRange.firstGroup);
		if (clampedCount == 0u) {
			continue;
		}
		CLodActiveGroupRange range{};
		range.groupsBase = sharedState->groupsBase + localRange.firstGroup;
		range.groupCount = clampedCount;
		event.coarsestRanges.push_back(range);
	}
	PublishCLodStreamingDomainEvent(std::move(event));
}

void MeshManager::DrainCLodStreamingDomainEvents(std::vector<CLodStreamingDomainEvent>& outEvents, uint64_t& outGeneration) {
	outEvents.clear();
	{
		std::lock_guard lock(m_clodStreamingDomainEventsMutex);
		outEvents.swap(m_clodStreamingDomainEvents);
	}
	outGeneration = m_clodStreamingDomainEventGeneration.load(std::memory_order_acquire);
}

bool MeshManager::TryGetCLodParentGroup(uint32_t groupGlobalIndex, uint32_t& outParentGlobalIndex) const {
	uint32_t localIndex = 0u;
	auto sharedState = const_cast<MeshManager*>(this)->FindCLodSharedStreamingStateByGlobalGroup(groupGlobalIndex, localIndex);
	if (sharedState == nullptr || localIndex >= sharedState->parentGroupByLocal.size()) {
		return false;
	}

	const int32_t parentLocal = sharedState->parentGroupByLocal[localIndex];
	if (parentLocal < 0) {
		return false;
	}

	const uint32_t parentLocalU32 = static_cast<uint32_t>(parentLocal);
	if (parentLocalU32 >= sharedState->groupCount || parentLocalU32 == localIndex) {
		return false;
	}

	outParentGlobalIndex = sharedState->groupsBase + parentLocalU32;
	return true;
}

void MeshManager::GetCLodChildGroups(uint32_t parentGroupGlobalIndex, std::vector<uint32_t>& outChildGroups) const {
	outChildGroups.clear();

	uint32_t localIndex = 0u;
	auto sharedState = const_cast<MeshManager*>(this)->FindCLodSharedStreamingStateByGlobalGroup(parentGroupGlobalIndex, localIndex);
	if (sharedState == nullptr || localIndex >= sharedState->childrenByLocalParent.size()) {
		return;
	}

	const auto& localChildren = sharedState->childrenByLocalParent[localIndex];
	outChildGroups.reserve(localChildren.size());
	for (uint32_t childLocal : localChildren) {
		if (childLocal < sharedState->groupCount) {
			outChildGroups.push_back(sharedState->groupsBase + childLocal);
		}
	}
}

std::shared_ptr<MeshManager::CLodSharedStreamingState> MeshManager::FindCLodSharedStreamingStateByGlobalGroup(uint32_t groupGlobalIndex, uint32_t& outGroupLocalIndex) {
	outGroupLocalIndex = 0u;
	RebuildCLodSharedStreamingRangeIndex();

	if (m_clodSharedStreamingRanges.empty()) {
		return nullptr;
	}

	auto it = std::upper_bound(
		m_clodSharedStreamingRanges.begin(),
		m_clodSharedStreamingRanges.end(),
		groupGlobalIndex,
		[](uint32_t value, const CLodSharedStreamingRange& range) {
			return value < range.begin;
		});

	if (it == m_clodSharedStreamingRanges.begin()) {
		return nullptr;
	}

	--it;
	if (groupGlobalIndex < it->begin || groupGlobalIndex >= it->end || it->state == nullptr) {
		return nullptr;
	}

	outGroupLocalIndex = groupGlobalIndex - it->begin;
	return it->state;
}

std::vector<uint32_t> MeshManager::GetCLodGroupMeshPageIndices(const CLodSharedStreamingState& state, uint32_t groupLocalIndex) const {
	std::vector<uint32_t> meshPageIndices;
	if (groupLocalIndex >= state.groups.size()) {
		return meshPageIndices;
	}

	if (groupLocalIndex + 1u < state.groupPageReferenceOffsets.size()) {
		const uint32_t refBegin = state.groupPageReferenceOffsets[groupLocalIndex];
		const uint32_t refEnd = state.groupPageReferenceOffsets[groupLocalIndex + 1u];
		if (refBegin <= refEnd && refEnd <= state.groupPageReferences.size()) {
			meshPageIndices.assign(state.groupPageReferences.begin() + refBegin, state.groupPageReferences.begin() + refEnd);
		}
	}

	const ClusterLODGroup& group = state.groups[groupLocalIndex];
	if (meshPageIndices.empty() && group.pageCount > 0u) {
		meshPageIndices.reserve(group.pageCount);
		for (uint32_t pageIndex = 0; pageIndex < group.pageCount; ++pageIndex) {
			meshPageIndices.push_back(group.pageMapBase + pageIndex);
		}
	}
	return meshPageIndices;
}

std::vector<uint32_t> MeshManager::GetCLodGroupPageMapOffsets(const CLodSharedStreamingState& state, uint32_t groupLocalIndex) const {
	std::vector<uint32_t> pageMapOffsets;
	if (groupLocalIndex >= state.groups.size()) {
		return pageMapOffsets;
	}

	const ClusterLODGroup& group = state.groups[groupLocalIndex];
	const std::vector<uint32_t> meshPageIndices = GetCLodGroupMeshPageIndices(state, groupLocalIndex);
	const uint32_t pageCount = !meshPageIndices.empty()
		? static_cast<uint32_t>(meshPageIndices.size())
		: group.pageCount;
	if (pageCount == 0u) {
		return pageMapOffsets;
	}
	if (group.pageCount != pageCount) {
		spdlog::warn(
			"CLod streaming: group {} has page-map count {} but {} streamable mesh pages",
			state.groupsBase + groupLocalIndex,
			group.pageCount,
			pageCount);
	}

	pageMapOffsets.reserve(pageCount);
	for (uint32_t pageOffset = 0u; pageOffset < pageCount; ++pageOffset) {
		pageMapOffsets.push_back(group.pageMapBase + pageOffset);
	}
	return pageMapOffsets;
}

	bool MeshManager::QueueCLodDiskStreamingRequest(uint32_t groupGlobalIndex, const std::shared_ptr<CLodSharedStreamingState>& state, uint32_t groupLocalIndex, bool& outQueued, const std::vector<bool>& segmentNeedsFetch, const std::vector<uint32_t>& preAllocatedPages, uint32_t priority, const CLodCache::GroupPayloadLayoutMetadata* prefetchedLayout) {
	outQueued = false;
	if (state == nullptr || groupLocalIndex >= state->groupChunkHints.size()) {
		return false;
	}
	if (groupLocalIndex >= state->residentGroupAllocations.size()) {
		return false;
	}

	const auto& residentAllocations = state->residentGroupAllocations[groupLocalIndex];
	const auto& sourceChunk = state->groupChunkHints[groupLocalIndex];

	// The page-pool path considers a group "ready" when the page allocation is valid.
	// Zero-meshlet voxel groups can still own streamable pages, so readiness is based
	// on pageCount rather than meshletCount.
	const bool hasRequiredAllocations =
		IsCLodGroupResident(*state, groupLocalIndex) &&
		(!residentAllocations.pageAllocations.empty() || sourceChunk.pageCount == 0u);

	if (hasRequiredAllocations) {
		return true;
	}

	if (state->cacheSource.containerFileName.empty()) {
		return false;
	}

	const auto& pageDiskLocators = state->pageDiskLocators;
	if (pageDiskLocators.empty() ||
		groupLocalIndex >= state->groups.size()) {
		return false;
	}
	const ClusterLODGroup& group = state->groups[groupLocalIndex];
	std::vector<uint32_t> meshPageIndices = GetCLodGroupMeshPageIndices(*state, groupLocalIndex);
	if (std::any_of(meshPageIndices.begin(), meshPageIndices.end(), [&](uint32_t pageIndex) { return pageIndex >= pageDiskLocators.size(); })) {
		return false;
	}
	if ((!segmentNeedsFetch.empty() && segmentNeedsFetch.size() != meshPageIndices.size()) ||
		(!preAllocatedPages.empty() && preAllocatedPages.size() != meshPageIndices.size())) {
		spdlog::warn(
			"CLod streaming: refusing to queue group {} because request page arrays do not cover all group pages",
			groupGlobalIndex);
		return false;
	}

	{
		std::lock_guard<std::mutex> lock(m_clodDiskStreamingMutex);
		if (!m_clodDiskStreamingQueuedGroups.insert(groupGlobalIndex).second) {
			outQueued = true;
			return false;
		}

		CLodDiskStreamingRequest request{};
		request.groupGlobalIndex = groupGlobalIndex;
		request.groupLocalIndex = groupLocalIndex;
		request.groupsBase = state->groupsBase;
		request.cacheSource = state->cacheSource;
		request.sharedState = state;
		request.pageMapBase = group.pageMapBase;
		request.pageCount = static_cast<uint32_t>(meshPageIndices.size());
		request.meshPageIndices = std::move(meshPageIndices);
		if (prefetchedLayout != nullptr && prefetchedLayout->IsValid()) {
			request.prefetchedLayout = *prefetchedLayout;
		}
		request.segmentNeedsFetch = segmentNeedsFetch;
		request.preAllocatedPages = preAllocatedPages;
		request.generation = m_clodDiskStreamingGeneration.load(std::memory_order_acquire);
		request.priority = priority;
		m_clodDiskStreamingRequests.push_back(std::move(request));
	}

	outQueued = true;
	return false;
}

MeshManager::DiskStreamingApplyResult MeshManager::PrepareCompletedCLodDiskStreamingResult(
	CLodDiskStreamingResult& result,
	const std::vector<uint32_t>& preAllocatedPages,
	CLodDiskStreamingCompletion& outCompletion) {
	outCompletion = {};
	outCompletion.groupGlobalIndex = result.groupGlobalIndex;
	outCompletion.generation = result.generation;
	if (!result.success) {
		return DiskStreamingApplyResult::FailedPermanent;
	}

	uint32_t localIndex = 0u;
	auto sharedState = FindCLodSharedStreamingStateByGlobalGroup(result.groupGlobalIndex, localIndex);
	if (sharedState == nullptr) {
		return DiskStreamingApplyResult::FailedPermanent;
	}

	if (localIndex >= sharedState->baselineGroupChunks.size() ||
		localIndex >= sharedState->residentGroupAllocations.size()) {
		return DiskStreamingApplyResult::FailedPermanent;
	}

	// Start with baseline chunk and apply any disk-delivered metadata overrides.
	ClusterLODGroupChunk chunk = sharedState->baselineGroupChunks[localIndex];
	if (result.groupChunkMetadata.has_value()) {
		chunk = result.groupChunkMetadata.value();
	}

	const auto& meshGroups = sharedState->groups;
	if (localIndex >= meshGroups.size()) {
		return DiskStreamingApplyResult::FailedPermanent;
	}
	const uint32_t sCount = static_cast<uint32_t>(result.meshPageIndices.size());
	if (result.directStorageGpuUploadPending) {
		if (preAllocatedPages.size() != sCount ||
			result.directStoragePageBlobSizes.size() != sCount ||
			result.directStoragePageBlobOffsets.size() != sCount ||
			(!result.segmentNeedsFetch.empty() && result.segmentNeedsFetch.size() != sCount)) {
			spdlog::error(
				"CLod streaming: DirectStorage GPU upload for group {} has mismatched page metadata (pages={}, allocs={}, sizes={}, offsets={}, fetchMask={})",
				result.groupGlobalIndex,
				sCount,
				preAllocatedPages.size(),
				result.directStoragePageBlobSizes.size(),
				result.directStoragePageBlobOffsets.size(),
				result.segmentNeedsFetch.size());
			return DiskStreamingApplyResult::FailedPermanent;
		}

		if (m_clodPagePool == nullptr) {
			spdlog::error(
				"CLod streaming: DirectStorage GPU upload for group {} has no page pool",
				result.groupGlobalIndex);
			return DiskStreamingApplyResult::FailedPermanent;
		}

		CLodPendingDirectStorageLaunch launch{};
		launch.groupGlobalIndex = result.groupGlobalIndex;
		launch.generation = result.generation;
		launch.cacheSource = result.cacheSource;
		launch.sharedState = sharedState;
		launch.groupLocalIndex = localIndex;
		launch.chunk = chunk;
		launch.meshPageIndices = result.meshPageIndices;
		launch.segmentNeedsFetch = result.segmentNeedsFetch;
		launch.pageIds = preAllocatedPages;
		launch.pageAllocations.resize(sCount);
		launch.pageMapEntries.resize(sCount);
		launch.uploadPathLabel = result.uploadPathLabel;
		launch.prefetchedChildLayouts = std::move(result.prefetchedChildLayouts);

		for (uint32_t ci = 0; ci < sCount; ++ci) {
			const uint32_t page = preAllocatedPages[ci];
			if (page >= m_clodPagePool->GetTotalPageCount()) {
				spdlog::error(
					"CLod streaming: DirectStorage GPU upload for group {} page {} targets invalid physical page {}",
					result.groupGlobalIndex,
					ci,
					page);
				return DiskStreamingApplyResult::FailedPermanent;
			}

			const PagePool::PageAllocation allocation{ page, 1u };
			launch.pageAllocations[ci] = allocation;
			launch.pageMapEntries[ci].slabDescriptorIndex = m_clodPagePool->GetSlabDescriptorIndex(allocation);
			launch.pageMapEntries[ci].slabByteOffset =
				static_cast<uint32_t>(m_clodPagePool->PageToSlabByteOffset(page));
			if (launch.pageMapEntries[ci].slabDescriptorIndex == 0u) {
				spdlog::error(
					"CLod streaming: DirectStorage GPU upload for group {} page {} has no valid slab descriptor",
					result.groupGlobalIndex,
					ci);
				return DiskStreamingApplyResult::FailedPermanent;
			}

			const bool needsFetch = result.segmentNeedsFetch.empty()
				|| ci >= static_cast<uint32_t>(result.segmentNeedsFetch.size())
				|| result.segmentNeedsFetch[ci];
			if (!needsFetch) {
				continue;
			}

			const uint32_t blobSize = result.directStoragePageBlobSizes[ci];
			if (blobSize == 0u || blobSize > m_clodPagePool->GetPageSize()) {
				spdlog::error(
					"CLod streaming: DirectStorage GPU upload for group {} page {} has invalid payload size {}",
					result.groupGlobalIndex,
					ci,
					blobSize);
				return DiskStreamingApplyResult::FailedPermanent;
			}

			const uint32_t slabIndex = m_clodPagePool->PageToSlabIndex(page);
			auto slab = m_clodPagePool->GetSlab(slabIndex);
			if (!slab) {
				spdlog::error(
					"CLod streaming: DirectStorage GPU upload for group {} page {} has no destination slab {}",
					result.groupGlobalIndex,
					ci,
					slabIndex);
				return DiskStreamingApplyResult::FailedPermanent;
			}

			br::DirectStorageBufferRegionCopy copy{};
			copy.sourceOffset = result.directStoragePageBlobOffsets[ci];
			copy.sourceSizeBytes = blobSize;
			copy.uncompressedSizeBytes = blobSize;
			copy.destinationResource = slab->GetAPIResource();
			copy.destinationOffset = m_clodPagePool->PageToSlabByteOffset(page);
			launch.copies.push_back(copy);
			++launch.fetchedPageCount;
			launch.totalBlobBytes += blobSize;
		}

		if (launch.fetchedPageCount == 0u) {
			outCompletion.groupGlobalIndex = result.groupGlobalIndex;
			outCompletion.success = true;
			outCompletion.payloadKind = CLodDiskStreamingPayloadKind::ReusedExistingPages;
			outCompletion.chunk = chunk;
			outCompletion.meshPageIndices = std::move(launch.meshPageIndices);
			outCompletion.segmentNeedsFetch = std::move(launch.segmentNeedsFetch);
			outCompletion.preAllocatedPages = std::move(launch.pageIds);
			outCompletion.pageAllocations = std::move(launch.pageAllocations);
			outCompletion.pageMapEntries = std::move(launch.pageMapEntries);
			outCompletion.generation = result.generation;
			outCompletion.totalStreamedBytes = 0u;
			outCompletion.fetchedPageCount = 0u;
			outCompletion.uploadPathLabel = "ReusedExistingPages";
			outCompletion.prefetchedChildLayouts = std::move(launch.prefetchedChildLayouts);
			return DiskStreamingApplyResult::Prepared;
		}

		spdlog::debug(
			"CLod streaming: group {} prepared DirectStorage GPU launch (fetchedPages={}/{}, bytes={}, reusedPages={})",
			result.groupGlobalIndex,
			launch.fetchedPageCount,
			sCount,
			launch.totalBlobBytes,
			sCount - launch.fetchedPageCount);
		static std::atomic<uint64_t> s_preparedDirectStorageGpuLaunchCount{ 0 };
		const uint64_t preparedLaunchCount = s_preparedDirectStorageGpuLaunchCount.fetch_add(1, std::memory_order_relaxed) + 1u;
		if (preparedLaunchCount == 1u || (preparedLaunchCount % 256u) == 0u) {
			spdlog::info(
				"CLod streaming DirectStorage GPU prepared: totalPrepared={} latestGroup={} fetchedPages={}/{} bytes={}",
				preparedLaunchCount,
				result.groupGlobalIndex,
				launch.fetchedPageCount,
				sCount,
				launch.totalBlobBytes);
		}

		std::lock_guard<std::mutex> lock(m_clodDiskStreamingResultsMutex);
		m_clodPendingDirectStorageLaunches.push_back(std::move(launch));
		return DiskStreamingApplyResult::DeferredPendingUpload;
	}

	if (result.pageBlobs.size() != sCount) {
		spdlog::error("CLod streaming: group {} (local {}) expected {} page blobs but got {}",
			result.groupGlobalIndex, localIndex, sCount, result.pageBlobs.size());
		return DiskStreamingApplyResult::FailedPermanent;
	}

	size_t totalBlobBytes = 0;
	uint32_t fetchedPageCount = 0;
	for (uint32_t ci = 0; ci < sCount; ++ci) {
		const bool needsFetch = result.segmentNeedsFetch.empty()
			|| ci >= static_cast<uint32_t>(result.segmentNeedsFetch.size())
			|| result.segmentNeedsFetch[ci];
		const size_t blobSize = result.pageBlobs[ci].size();
		if (needsFetch) {
			if (blobSize == 0u) {
				spdlog::error(
					"CLod streaming: group {} page {} was marked for fetch but has no payload bytes",
					result.groupGlobalIndex,
					ci);
				return DiskStreamingApplyResult::FailedPermanent;
			}
			++fetchedPageCount;
			totalBlobBytes += blobSize;
		}
	}

	spdlog::debug(
		"CLod streaming: group {} prepared via {} (fetchedPages={}/{}, bytes={}, reusedPages={})",
		result.groupGlobalIndex,
		fetchedPageCount == 0u ? "ReusedExistingPages" : result.uploadPathLabel.c_str(),
		fetchedPageCount,
		sCount,
		totalBlobBytes,
		sCount - fetchedPageCount);

	outCompletion.groupGlobalIndex = result.groupGlobalIndex;
	outCompletion.success = true;
	outCompletion.payloadKind = CLodDiskStreamingPayloadKind::CpuPageBlobs;
	outCompletion.chunk = chunk;
	outCompletion.meshPageIndices = result.meshPageIndices;
	outCompletion.segmentNeedsFetch = result.segmentNeedsFetch;
	outCompletion.pageBlobs = std::move(result.pageBlobs);
	outCompletion.totalStreamedBytes = static_cast<uint64_t>(totalBlobBytes);
	outCompletion.fetchedPageCount = fetchedPageCount;
	outCompletion.uploadPathLabel = result.uploadPathLabel;
	outCompletion.prefetchedChildLayouts = std::move(result.prefetchedChildLayouts);
	return DiskStreamingApplyResult::Prepared;
}

void MeshManager::FinalizePendingCLodDirectStorageUploads(
	uint64_t currentGeneration,
	std::vector<CLodDiskStreamingCompletion>& outCompletions,
	std::vector<uint32_t>& outFinishedGroups) {
	const std::size_t pendingBefore = m_clodPendingDirectStorageUploads.size();
	std::size_t readyCount = 0u;
	std::size_t failedCount = 0u;
	std::size_t waitingForDsFenceCount = 0u;
	uint64_t worstDsReadyMs = 0u;
	uint32_t worstDsReadyGroup = 0u;
	uint64_t worstPublishMs = 0u;
	uint32_t worstPublishGroup = 0u;
	for (size_t uploadIndex = 0; uploadIndex < m_clodPendingDirectStorageUploads.size();) {
		auto& pendingUpload = m_clodPendingDirectStorageUploads[uploadIndex];

		auto finishUpload = [&](bool success) {
			const uint64_t nowMs = CLodStreamingNowMs();
			if (success) {
				++readyCount;
			} else {
				++failedCount;
			}
			if (pendingUpload.launchQueuedMs != 0u) {
				const uint64_t publishMs = nowMs - pendingUpload.launchQueuedMs;
				if (publishMs > worstPublishMs) {
					worstPublishMs = publishMs;
					worstPublishGroup = pendingUpload.groupGlobalIndex;
				}
			}
			CLodDiskStreamingCompletion completion{};
			completion.groupGlobalIndex = pendingUpload.groupGlobalIndex;
			completion.success = success;
			if (success) {
				completion.payloadKind = CLodDiskStreamingPayloadKind::GpuPagesReady;
				completion.chunk = pendingUpload.chunk;
				completion.meshPageIndices = pendingUpload.meshPageIndices;
				completion.preAllocatedPages = pendingUpload.pageIds;
				completion.segmentNeedsFetch = pendingUpload.segmentNeedsFetch;
				completion.pageAllocations = pendingUpload.pageAllocations;
				completion.pageMapEntries = pendingUpload.pageMapEntries;
				completion.generation = pendingUpload.generation;
				completion.totalStreamedBytes = pendingUpload.totalBlobBytes;
				completion.fetchedPageCount = pendingUpload.fetchedPageCount;
				completion.uploadPathLabel = pendingUpload.uploadPathLabel;
				completion.prefetchedChildLayouts = std::move(pendingUpload.prefetchedChildLayouts);
			}
			outCompletions.push_back(std::move(completion));
			outFinishedGroups.push_back(pendingUpload.groupGlobalIndex);
			if (uploadIndex + 1u < m_clodPendingDirectStorageUploads.size()) {
				m_clodPendingDirectStorageUploads[uploadIndex] = std::move(m_clodPendingDirectStorageUploads.back());
			}
			m_clodPendingDirectStorageUploads.pop_back();
		};

		const bool isStale = pendingUpload.generation != currentGeneration;
		const DirectStorageAsyncRequestStatus uploadStatus = DirectStorageManager::GetInstance().PollRequest(pendingUpload.uploadHandle);
		if (uploadStatus.state == DirectStorageAsyncRequestState::Pending) {
			++waitingForDsFenceCount;
			++uploadIndex;
			continue;
		}
		if (uploadStatus.state == DirectStorageAsyncRequestState::Ready) {
			if (pendingUpload.dsReadyMs == 0u) {
				pendingUpload.dsReadyMs = CLodStreamingNowMs();
			}
			if (pendingUpload.launchQueuedMs != 0u) {
				const uint64_t readyMs = pendingUpload.dsReadyMs - pendingUpload.launchQueuedMs;
				if (readyMs > worstDsReadyMs) {
					worstDsReadyMs = readyMs;
					worstDsReadyGroup = pendingUpload.groupGlobalIndex;
				}
			}
		}

		if (isStale) {
			spdlog::info("CLod streaming: discarding stale DirectStorage upload for group {} after completion (gen {} vs current {})",
				pendingUpload.groupGlobalIndex,
				pendingUpload.generation,
				currentGeneration);
			finishUpload(false);
			continue;
		}

		if (uploadStatus.state != DirectStorageAsyncRequestState::Ready) {
			spdlog::error(
				"CLod streaming: DirectStorage page-pool upload failed for group {}: {}",
				pendingUpload.groupGlobalIndex,
				uploadStatus.message);
			finishUpload(false);
			continue;
		}

		spdlog::debug(
			"CLod streaming: group {} prepared via {} after DirectStorage upload (fetchedPages={}/{}, bytes={}, reusedPages={})",
			pendingUpload.groupGlobalIndex,
			pendingUpload.fetchedPageCount == 0u ? "ReusedExistingPages" : pendingUpload.uploadPathLabel.c_str(),
			pendingUpload.fetchedPageCount,
			static_cast<uint32_t>(pendingUpload.meshPageIndices.size()),
			pendingUpload.totalBlobBytes,
			static_cast<uint32_t>(pendingUpload.meshPageIndices.size()) - pendingUpload.fetchedPageCount);

		finishUpload(true);
	}
	if (SarpClodImportDebugLoggingEnabled() && (pendingBefore != 0u || readyCount != 0u || failedCount != 0u)) {
		spdlog::info(
			"SARPDBG FinalizePendingCLodDirectStorageUploads pendingBefore={} ready={} failed={} pendingAfter={}",
			pendingBefore,
			readyCount,
			failedCount,
			m_clodPendingDirectStorageUploads.size());
	}
	if (readyCount != 0u || failedCount != 0u) {
		spdlog::info(
			"CLod streaming DirectStorage GPU finalized: pendingBefore={} ready={} failed={} waitingDsFence={} pendingAfter={} worstDsReadyMs={} group={} worstPublishMs={} group={}",
			pendingBefore,
			readyCount,
			failedCount,
			waitingForDsFenceCount,
			m_clodPendingDirectStorageUploads.size(),
			worstDsReadyMs,
			worstDsReadyGroup,
			worstPublishMs,
			worstPublishGroup);
	}
	else if (pendingBefore != 0u && SarpClodImportDebugLoggingEnabled()) {
		spdlog::info(
			"SARPDBG CLod DirectStorage pending timing: pending={} waitingDsFence={} worstDsReadyMs={} group={}",
			pendingBefore,
			waitingForDsFenceCount,
			worstDsReadyMs,
			worstDsReadyGroup);
	}
}

void MeshManager::DrainCompletedCLodDiskStreamingGroups(std::vector<CLodDiskStreamingCompletion>& outCompletions) {
	std::lock_guard<std::mutex> resultsLock(m_clodDiskStreamingResultsMutex);
	outCompletions = std::move(m_clodDiskStreamingCompletions);
	m_clodDiskStreamingCompletions.clear();
}

bool MeshManager::FreeCLodGroupEviction(uint32_t groupGlobalIndex) {
	return EvictCLodGroupResidency(groupGlobalIndex, false);
}

bool MeshManager::EvictCLodGroupResidency(uint32_t groupGlobalIndex, bool clearPageMapEntries) {
	uint32_t localIndex = 0u;
	auto sharedState = FindCLodSharedStreamingStateByGlobalGroup(groupGlobalIndex, localIndex);
	if (sharedState == nullptr) {
		return false;
	}

	std::lock_guard<std::mutex> residencyLock(m_clodResidencyMutex);
	const bool evicted = ApplyCLodGroupEviction(*sharedState, localIndex, clearPageMapEntries);
	return evicted;
}

bool MeshManager::CommitCLodGroupResidency(
	uint32_t groupGlobalIndex,
	const ClusterLODGroupChunk& chunk,
	std::span<const uint32_t> meshPageIndices,
	std::span<const GroupPageMapEntry> pageMapEntries,
	std::span<const PagePool::PageAllocation> pageAllocations,
	uint64_t streamedBytes) {
	uint32_t localIndex = 0u;
	auto sharedState = FindCLodSharedStreamingStateByGlobalGroup(groupGlobalIndex, localIndex);
	if (sharedState == nullptr ||
		localIndex >= sharedState->baselineGroupChunks.size() ||
		localIndex >= sharedState->residentGroupAllocations.size() ||
		localIndex >= sharedState->groupResidentFlags.size()) {
		return false;
	}

	const auto expectedMeshPageIndices = GetCLodGroupMeshPageIndices(*sharedState, localIndex);
	const auto expectedPageMapOffsets = GetCLodGroupPageMapOffsets(*sharedState, localIndex);
	if (meshPageIndices.size() != expectedMeshPageIndices.size() ||
		pageMapEntries.size() != expectedMeshPageIndices.size() ||
		pageAllocations.size() != expectedMeshPageIndices.size() ||
		expectedPageMapOffsets.size() != expectedMeshPageIndices.size()) {
		spdlog::warn(
			"CLod streaming: refusing to commit group {} residency because payload page counts do not match expected group pages (meshPages={}, pageMapEntries={}, allocations={}, pageMapOffsets={}, expected={})",
			groupGlobalIndex,
			meshPageIndices.size(),
			pageMapEntries.size(),
			pageAllocations.size(),
			expectedPageMapOffsets.size(),
			expectedMeshPageIndices.size());
		return false;
	}
	if (!expectedMeshPageIndices.empty() && sharedState->ownedPageMapView == nullptr) {
		spdlog::warn(
			"CLod streaming: refusing to commit group {} residency because the mesh page-map view is missing",
			groupGlobalIndex);
		return false;
	}

	for (size_t i = 0; i < expectedMeshPageIndices.size(); ++i) {
		if (meshPageIndices[i] != expectedMeshPageIndices[i] ||
			expectedPageMapOffsets[i] >= sharedState->pageMapEntriesCPU.size() ||
			!pageAllocations[i].IsValid() ||
			pageMapEntries[i].slabDescriptorIndex == 0u) {
			spdlog::warn(
				"CLod streaming: refusing to commit group {} residency because page {} is not fully renderable",
				groupGlobalIndex,
				i);
			return false;
		}
		if (m_clodPagePool != nullptr) {
			const uint32_t expectedSlabDescriptor = m_clodPagePool->GetSlabDescriptorIndex(pageAllocations[i]);
			const uint32_t expectedSlabByteOffset =
				static_cast<uint32_t>(m_clodPagePool->PageToSlabByteOffset(pageAllocations[i].firstPageID));
			if (pageMapEntries[i].slabDescriptorIndex != expectedSlabDescriptor ||
				pageMapEntries[i].slabByteOffset != expectedSlabByteOffset) {
				spdlog::warn(
					"CLod streaming: refusing to commit group {} residency because page {} map entry points at slab/offset {}:{} but allocation page {} resolves to {}:{}",
					groupGlobalIndex,
					i,
					pageMapEntries[i].slabDescriptorIndex,
					pageMapEntries[i].slabByteOffset,
					pageAllocations[i].firstPageID,
					expectedSlabDescriptor,
					expectedSlabByteOffset);
				return false;
			}
		}
	}

	std::lock_guard<std::mutex> residencyLock(m_clodResidencyMutex);

	auto& residentAllocations = sharedState->residentGroupAllocations[localIndex];
	const bool wasResident = IsCLodGroupResident(*sharedState, localIndex);
	const uint32_t previousAllocationCount = static_cast<uint32_t>(residentAllocations.pageAllocations.size());

	residentAllocations.Reset();
	residentAllocations.pageAllocations.assign(pageAllocations.begin(), pageAllocations.end());

	if (sharedState->ownedPageMapView) {
		for (size_t i = 0; i < meshPageIndices.size(); ++i) {
			const uint32_t pageMapOffset = expectedPageMapOffsets[i];
			if (pageMapOffset < sharedState->pageMapEntriesCPU.size()) {
				const GroupPageMapEntry previousEntry = sharedState->pageMapEntriesCPU[pageMapOffset];
				sharedState->pageMapEntriesCPU[pageMapOffset] = pageMapEntries[i];
				if (m_clodPageMapWriteCallback) {
					CLodPageMapWriteEvent event{};
					event.reason = CLodPageMapWriteReason::Commit;
					event.groupGlobalIndex = groupGlobalIndex;
					event.groupLocalIndex = localIndex;
					event.groupsBase = sharedState->groupsBase;
					event.meshPageIndex = meshPageIndices[i];
					event.pageMapOffset = pageMapOffset;
					event.physicalPage = pageAllocations[i].firstPageID;
					event.slabDescriptorIndex = pageMapEntries[i].slabDescriptorIndex;
					event.slabByteOffset = pageMapEntries[i].slabByteOffset;
					event.previousSlabDescriptorIndex = previousEntry.slabDescriptorIndex;
					event.previousSlabByteOffset = previousEntry.slabByteOffset;
					m_clodPageMapWriteCallback(event);
				}
			}
		}
		for (size_t i = 0; i < expectedPageMapOffsets.size(); ++i) {
			UploadCLodGroupPageMapRange(*sharedState, expectedPageMapOffsets[i], std::span<const GroupPageMapEntry>(&pageMapEntries[i], 1));
		}
	}

	sharedState->baselineGroupChunks[localIndex] = chunk;
	sharedState->groupResidentFlags[localIndex] = 1u;

	if (!wasResident) {
		m_debugResidentGroups.fetch_add(1u, std::memory_order_relaxed);
	}
	if (streamedBytes != 0u) {
		m_debugTotalStreamedBytes.fetch_add(streamedBytes, std::memory_order_relaxed);
	}
	const uint32_t newAllocationCount = static_cast<uint32_t>(residentAllocations.pageAllocations.size());
	if (newAllocationCount >= previousAllocationCount) {
		m_debugResidentAllocations.fetch_add(newAllocationCount - previousAllocationCount, std::memory_order_relaxed);
	} else {
		const uint32_t diff = previousAllocationCount - newAllocationCount;
		const uint32_t prev = m_debugResidentAllocations.load(std::memory_order_relaxed);
		m_debugResidentAllocations.store(prev >= diff ? prev - diff : 0u, std::memory_order_relaxed);
	}
	UploadCLodGroupChunk(*sharedState, localIndex);
	return true;
}

void MeshManager::SetCLodPageMapWriteCallback(std::function<void(const CLodPageMapWriteEvent&)> fn) {
	m_clodPageMapWriteCallback = std::move(fn);
}

bool MeshManager::IsCLodGroupDiskIOQueued(uint32_t groupGlobalIndex) const {
	std::lock_guard<std::mutex> lock(m_clodDiskStreamingMutex);
	return m_clodDiskStreamingQueuedGroups.count(groupGlobalIndex) != 0;
}

bool MeshManager::HasPendingCLodDirectStorageLaunches() const {
	std::lock_guard<std::mutex> lock(m_clodDiskStreamingResultsMutex);
	return !m_clodPendingDirectStorageLaunches.empty();
}

bool MeshManager::HasPendingCLodDirectStorageUploads() const {
	std::lock_guard<std::mutex> lock(m_clodDiskStreamingResultsMutex);
	return !m_clodPendingDirectStorageUploads.empty();
}

std::pair<std::size_t, std::size_t> MeshManager::GetPendingCLodDirectStorageCounts() const {
	std::lock_guard<std::mutex> lock(m_clodDiskStreamingResultsMutex);
	return { m_clodPendingDirectStorageLaunches.size(), m_clodPendingDirectStorageUploads.size() };
}

void MeshManager::SetCLodStreamingUploadFunction(PagePool::UploadFn fn) {
	m_clodStreamingUploadFn = std::move(fn);
	if (m_clodPagePool != nullptr) {
		m_clodPagePool->SetUploadFunction(m_clodStreamingUploadFn);
	}
}

bool MeshManager::LaunchPendingCLodDirectStorageUploads(rhi::Timeline waitTimeline, uint64_t waitValue) {
	if (!waitTimeline.IsValid() || waitValue == 0 || !m_clodDirectStorageCompletionFenceHandle.IsValid()) {
		return false;
	}

	std::vector<CLodPendingDirectStorageLaunch> launches;
	{
		std::lock_guard<std::mutex> lock(m_clodDiskStreamingResultsMutex);
		if (m_clodPendingDirectStorageLaunches.empty()) {
			return false;
		}
		launches = std::move(m_clodPendingDirectStorageLaunches);
		m_clodPendingDirectStorageLaunches.clear();
	}
	if (SarpClodImportDebugLoggingEnabled()) {
		spdlog::info(
			"SARPDBG LaunchPendingCLodDirectStorageUploads launches={} waitValue={}",
			launches.size(),
			waitValue);
	}

	std::vector<CLodPendingDirectStorageUpload> activeUploads;
	std::vector<CLodDiskStreamingCompletion> failedCompletions;
	std::vector<uint32_t> failedGroups;
	const uint64_t launchQueuedMs = CLodStreamingNowMs();
	activeUploads.reserve(launches.size());
	failedCompletions.reserve(launches.size());
	failedGroups.reserve(launches.size());

	for (auto& launch : launches) {
		const uint64_t completionValue =
			m_clodDirectStorageCompletionFenceCounter.fetch_add(1, std::memory_order_relaxed) + 1;

		std::string directStorageMessage;
		DirectStorageAsyncRequestHandle uploadHandle =
			DirectStorageManager::GetInstance().EnqueueUploadBufferRegionsFromFileAfterFence(
				launch.sharedState != nullptr && !launch.sharedState->resolvedContainerPath.empty()
					? launch.sharedState->resolvedContainerPath
					: CLodCache::ResolveContainerPath(launch.cacheSource),
				launch.copies,
				DirectStorageFencePoint{ waitTimeline, waitValue },
				DirectStorageFenceWaitMode::BeforeGpuWork,
				DirectStorageFencePoint{ m_clodDirectStorageCompletionFenceHandle, completionValue },
				&directStorageMessage);

		if (!uploadHandle.IsValid()) {
			spdlog::error(
				"CLod streaming: DirectStorage Queue3 page-pool launch failed for group {}: {}",
				launch.groupGlobalIndex,
				directStorageMessage);
			failedCompletions.push_back({ launch.groupGlobalIndex, false });
			failedGroups.push_back(launch.groupGlobalIndex);
			continue;
		}

		CLodPendingDirectStorageUpload pendingUpload{};
		pendingUpload.groupGlobalIndex = launch.groupGlobalIndex;
		pendingUpload.generation = launch.generation;
		pendingUpload.sharedState = std::move(launch.sharedState);
		pendingUpload.groupLocalIndex = launch.groupLocalIndex;
		pendingUpload.chunk = launch.chunk;
		pendingUpload.pageAllocations = std::move(launch.pageAllocations);
		pendingUpload.pageMapEntries = std::move(launch.pageMapEntries);
		pendingUpload.meshPageIndices = std::move(launch.meshPageIndices);
		pendingUpload.segmentNeedsFetch = std::move(launch.segmentNeedsFetch);
		pendingUpload.fetchedPageCount = launch.fetchedPageCount;
		pendingUpload.totalBlobBytes = launch.totalBlobBytes;
		pendingUpload.uploadPathLabel = std::move(launch.uploadPathLabel);
		pendingUpload.uploadHandle = std::move(uploadHandle);
		pendingUpload.launchQueuedMs = launchQueuedMs;
		pendingUpload.pageIds = std::move(launch.pageIds);
		pendingUpload.prefetchedChildLayouts = std::move(launch.prefetchedChildLayouts);
		activeUploads.push_back(std::move(pendingUpload));
	}

	{
		std::lock_guard<std::mutex> lock(m_clodDiskStreamingResultsMutex);
		m_clodPendingDirectStorageUploads.insert(
			m_clodPendingDirectStorageUploads.end(),
			std::make_move_iterator(activeUploads.begin()),
			std::make_move_iterator(activeUploads.end()));
		m_clodDiskStreamingCompletions.insert(
			m_clodDiskStreamingCompletions.end(),
			failedCompletions.begin(),
			failedCompletions.end());
	}
	if (!failedGroups.empty()) {
		std::lock_guard<std::mutex> lock(m_clodDiskStreamingMutex);
		for (uint32_t group : failedGroups) {
			m_clodDiskStreamingQueuedGroups.erase(group);
		}
	}
	if (SarpClodImportDebugLoggingEnabled()) {
		spdlog::info(
			"SARPDBG LaunchPendingCLodDirectStorageUploads activeUploads={} failed={} pendingUploadsNow={}",
			activeUploads.size(),
			failedGroups.size(),
			HasPendingCLodDirectStorageUploads() ? 1 : 0);
	}
	if (!activeUploads.empty() || !failedGroups.empty()) {
		spdlog::info(
			"CLod streaming DirectStorage GPU launched: activeUploads={} failed={} pendingUploadsNow={}",
			activeUploads.size(),
			failedGroups.size(),
			HasPendingCLodDirectStorageUploads() ? 1 : 0);
	}

	return true;
}

bool MeshManager::QueueCLodGroupDiskIO(uint32_t groupGlobalIndex, const std::vector<bool>& segmentNeedsFetch, const std::vector<uint32_t>& preAllocatedPages, uint32_t priority, const CLodCache::GroupPayloadLayoutMetadata* prefetchedLayout) {
	CLodGroupDiskIOBatchRequest request{};
	request.groupGlobalIndex = groupGlobalIndex;
	request.segmentNeedsFetch = segmentNeedsFetch;
	request.preAllocatedPages = preAllocatedPages;
	request.priority = priority;
	if (prefetchedLayout != nullptr && prefetchedLayout->IsValid()) {
		request.prefetchedLayout = *prefetchedLayout;
	}

	std::vector<CLodGroupDiskIOBatchRequest> requests;
	requests.push_back(std::move(request));
	std::vector<bool> queuedByRequest;
	QueueCLodGroupDiskIOBatch(requests, &queuedByRequest);
	return !queuedByRequest.empty() && queuedByRequest[0];
}

uint32_t MeshManager::QueueCLodGroupDiskIOBatch(const std::vector<CLodGroupDiskIOBatchRequest>& requests, std::vector<bool>* outQueuedByRequest) {
	struct PreparedRequest {
		uint32_t sourceIndex = 0u;
		CLodDiskStreamingRequest request;
	};

	if (outQueuedByRequest != nullptr) {
		outQueuedByRequest->assign(requests.size(), false);
	}

	std::vector<PreparedRequest> prepared;
	prepared.reserve(requests.size());

	for (uint32_t requestIndex = 0; requestIndex < static_cast<uint32_t>(requests.size()); ++requestIndex) {
		const auto& batchRequest = requests[requestIndex];
		uint32_t localIndex = 0u;
		auto sharedState = FindCLodSharedStreamingStateByGlobalGroup(batchRequest.groupGlobalIndex, localIndex);
		if (sharedState == nullptr) {
			continue;
		}

		if (localIndex >= sharedState->groupChunkHints.size() ||
			localIndex >= sharedState->residentGroupAllocations.size()) {
			continue;
		}

		const auto& sourceChunk = sharedState->groupChunkHints[localIndex];
		const auto& residentAllocations = sharedState->residentGroupAllocations[localIndex];
		const bool hasRequiredAllocations =
			IsCLodGroupResident(*sharedState, localIndex) &&
			(!residentAllocations.pageAllocations.empty() || sourceChunk.pageCount == 0u);
		if (hasRequiredAllocations || sharedState->cacheSource.containerFileName.empty()) {
			continue;
		}

		const auto& pageDiskLocators = sharedState->pageDiskLocators;
		if (localIndex >= sharedState->groups.size() ||
			pageDiskLocators.empty()) {
			continue;
		}
		const ClusterLODGroup& group = sharedState->groups[localIndex];
		std::vector<uint32_t> meshPageIndices = GetCLodGroupMeshPageIndices(*sharedState, localIndex);
		if (std::any_of(meshPageIndices.begin(), meshPageIndices.end(), [&](uint32_t pageIndex) { return pageIndex >= pageDiskLocators.size(); })) {
			continue;
		}
		if ((!batchRequest.segmentNeedsFetch.empty() && batchRequest.segmentNeedsFetch.size() != meshPageIndices.size()) ||
			(!batchRequest.preAllocatedPages.empty() && batchRequest.preAllocatedPages.size() != meshPageIndices.size())) {
			spdlog::warn(
				"CLod streaming: refusing to queue group {} because request page arrays do not cover all group pages",
				batchRequest.groupGlobalIndex);
			continue;
		}

		PreparedRequest preparedRequest{};
		preparedRequest.sourceIndex = requestIndex;
		preparedRequest.request.groupGlobalIndex = batchRequest.groupGlobalIndex;
		preparedRequest.request.groupLocalIndex = localIndex;
		preparedRequest.request.groupsBase = sharedState->groupsBase;
		preparedRequest.request.cacheSource = sharedState->cacheSource;
		preparedRequest.request.sharedState = sharedState;
		preparedRequest.request.pageMapBase = group.pageMapBase;
		preparedRequest.request.pageCount = static_cast<uint32_t>(meshPageIndices.size());
		preparedRequest.request.meshPageIndices = std::move(meshPageIndices);
		if (batchRequest.prefetchedLayout.has_value() && batchRequest.prefetchedLayout->IsValid()) {
			preparedRequest.request.prefetchedLayout = batchRequest.prefetchedLayout;
		}
		preparedRequest.request.segmentNeedsFetch = batchRequest.segmentNeedsFetch;
		preparedRequest.request.preAllocatedPages = batchRequest.preAllocatedPages;
		preparedRequest.request.childLayoutPrefetchGroups = batchRequest.childLayoutPrefetchGroups;
		preparedRequest.request.generation = m_clodDiskStreamingGeneration.load(std::memory_order_acquire);
		preparedRequest.request.priority = batchRequest.priority;
		prepared.push_back(std::move(preparedRequest));
	}

	uint32_t queuedCount = 0u;
	if (!prepared.empty()) {
		std::lock_guard<std::mutex> lock(m_clodDiskStreamingMutex);
		for (auto& preparedRequest : prepared) {
			const uint32_t groupGlobalIndex = preparedRequest.request.groupGlobalIndex;
			auto queuedIt = std::find_if(
				m_clodDiskStreamingRequests.begin(),
				m_clodDiskStreamingRequests.end(),
				[groupGlobalIndex](const CLodDiskStreamingRequest& request) {
					return request.groupGlobalIndex == groupGlobalIndex;
				});
			if (queuedIt != m_clodDiskStreamingRequests.end()) {
				if (preparedRequest.request.priority >= queuedIt->priority) {
					*queuedIt = std::move(preparedRequest.request);
				} else {
					queuedIt->priority = std::max(queuedIt->priority, preparedRequest.request.priority);
				}
				if (outQueuedByRequest != nullptr) {
					(*outQueuedByRequest)[preparedRequest.sourceIndex] = true;
				}
				continue;
			}

			const auto [_, inserted] = m_clodDiskStreamingQueuedGroups.insert(groupGlobalIndex);
			if (outQueuedByRequest != nullptr) {
				(*outQueuedByRequest)[preparedRequest.sourceIndex] = true;
			}
			if (inserted) {
				m_clodDiskStreamingRequests.push_back(std::move(preparedRequest.request));
				++queuedCount;
			}
		}
	}

	if (SarpClodImportDebugLoggingEnabled() && (!requests.empty() || queuedCount != 0u)) {
		std::size_t queuedGroupCount = 0u;
		{
			std::lock_guard<std::mutex> lock(m_clodDiskStreamingMutex);
			queuedGroupCount = m_clodDiskStreamingQueuedGroups.size();
		}
		spdlog::info(
			"SARPDBG QueueCLodGroupDiskIOBatch requested={} prepared={} queued={} queuedGroups={}",
			requests.size(),
			prepared.size(),
			queuedCount,
			queuedGroupCount);
	}

	return queuedCount;
}

bool MeshManager::TryGetCLodGroupPayloadLayout(uint32_t groupGlobalIndex, CLodCache::GroupPayloadLayoutMetadata& outLayout, std::string* outMessage) {
	outLayout.Clear();
	if (outMessage) {
		outMessage->clear();
	}

	uint32_t groupLocalIndex = 0u;
	auto sharedState = FindCLodSharedStreamingStateByGlobalGroup(groupGlobalIndex, groupLocalIndex);
	if (sharedState == nullptr) {
		if (outMessage) {
			*outMessage = "streaming state not found for group";
		}
		return false;
	}

	if (sharedState->cacheSource.containerFileName.empty()) {
		if (outMessage) {
			*outMessage = "mesh has no CLod disk streaming source";
		}
		return false;
	}

	const auto& pageDiskLocators = sharedState->pageDiskLocators;
	if (groupLocalIndex >= sharedState->groups.size() ||
		pageDiskLocators.empty()) {
		if (outMessage) {
			*outMessage = "mesh page disk locator missing";
		}
		return false;
	}
	std::vector<uint32_t> meshPageIndices = GetCLodGroupMeshPageIndices(*sharedState, groupLocalIndex);
	if (std::any_of(meshPageIndices.begin(), meshPageIndices.end(), [&](uint32_t pageIndex) { return pageIndex >= pageDiskLocators.size(); })) {
		if (outMessage) {
			*outMessage = "mesh page disk locator missing";
		}
		return false;
	}

	std::ifstream file;
	uint32_t pageCount = 0u;
	if (!CLodCache::OpenContainerFile(sharedState->cacheSource, file, pageCount)) {
		if (outMessage) {
			*outMessage = "failed to open CLod container";
		}
		return false;
	}

	if (pageCount != pageDiskLocators.size()) {
		if (outMessage) {
			*outMessage = "mesh page locator count does not match CLod container";
		}
		return false;
	}

	if (!CLodCache::GetMeshPagePayloadLayout(
		std::span<const ClusterLODGroupDiskLocator>(pageDiskLocators.data(), pageDiskLocators.size()),
		std::span<const uint32_t>(meshPageIndices.data(), meshPageIndices.size()),
		outLayout)) {
		if (outMessage) {
			*outMessage = "failed to read CLod mesh page payload layout";
		}
		return false;
	}
	outLayout.groupChunkMetadata = groupLocalIndex < sharedState->baselineGroupChunks.size()
		? std::optional<ClusterLODGroupChunk>(sharedState->baselineGroupChunks[groupLocalIndex])
		: std::nullopt;

	if (outMessage) {
		*outMessage = "read CLod group payload layout";
	}
	return true;
}

MeshManager::CLodGroupStreamingInfo MeshManager::GetCLodGroupStreamingInfo(uint32_t groupGlobalIndex) const {
	CLodGroupStreamingInfo info{};

	uint32_t localIndex = 0u;
	auto sharedState = const_cast<MeshManager*>(this)->FindCLodSharedStreamingStateByGlobalGroup(groupGlobalIndex, localIndex);
	if (sharedState == nullptr) {
		return info;
	}

	if (localIndex >= sharedState->groupChunkHints.size()) {
		return info;
	}

	info.hint = sharedState->groupChunkHints[localIndex];
	info.groupsBase = sharedState->groupsBase;
	if (localIndex < sharedState->groups.size()) {
		const ClusterLODGroup& group = sharedState->groups[localIndex];
		info.group = group;
		info.pageMapBase = group.pageMapBase;
		info.meshPageIndices = GetCLodGroupMeshPageIndices(*sharedState, localIndex);
		info.pageCount = static_cast<uint32_t>(info.meshPageIndices.size());
		const uint32_t firstSegment = group.firstSegment;
		const uint32_t segmentCount = group.segmentCount;
		if (firstSegment < sharedState->segments.size()) {
			const uint32_t clampedSegmentCount = std::min<uint32_t>(
				segmentCount,
				static_cast<uint32_t>(sharedState->segments.size() - firstSegment));
			info.segments.assign(
				sharedState->segments.begin() + firstSegment,
				sharedState->segments.begin() + firstSegment + clampedSegmentCount);
			info.referencedPageSegments.reserve(info.segments.size());
			for (uint32_t segmentOffset = 0u; segmentOffset < static_cast<uint32_t>(info.segments.size()); ++segmentOffset) {
				const ClusterLODGroupSegment& segment = info.segments[segmentOffset];
				if (segment.meshletCount == 0u || segment.pageIndex < group.pageMapBase) {
					continue;
				}
				const uint32_t localPageIndex = segment.pageIndex - group.pageMapBase;
				if (localPageIndex >= static_cast<uint32_t>(info.meshPageIndices.size())) {
					continue;
				}

				CLodGroupStreamingInfo::ReferencedPageSegment referenced{};
				referenced.meshPageIndex = info.meshPageIndices[localPageIndex];
				referenced.sourceGroupLocalIndex = localIndex;
				referenced.sourceGroupGlobalIndex = sharedState->groupsBase + localIndex;
				referenced.segmentGlobalIndex = firstSegment + segmentOffset;
				referenced.segment = segment;
				info.referencedPageSegments.push_back(referenced);
			}
		}
	}
	info.vertexByteSize = sharedState->vertexByteSize;
	info.valid = true;
	return info;
}

void MeshManager::ZeroCLodGroupChunkCounts(ClusterLODGroupChunk& chunk) {
	chunk.groupVertexCount = 0;
	chunk.meshletCount = 0;
	chunk.meshletTrianglesByteCount = 0;
}

bool MeshManager::IsCLodGroupResident(const CLodSharedStreamingState& state, uint32_t groupLocalIndex) const {
	if (groupLocalIndex >= state.groupResidentFlags.size()) {
		return false;
	}
	if (groupLocalIndex < state.groups.size() &&
		state.groups[groupLocalIndex].pageCount != 0u &&
		(groupLocalIndex >= state.residentGroupAllocations.size() ||
			state.residentGroupAllocations[groupLocalIndex].pageAllocations.empty())) {
		return false;
	}
	return state.groupResidentFlags[groupLocalIndex] != 0u;
}

bool MeshManager::IsCLodMeshPageReferencedByResidentGroup(const CLodSharedStreamingState& state, uint32_t meshPageIndex) const {
	const size_t groupCount = std::min(state.groups.size(), state.groupResidentFlags.size());
	for (size_t groupLocalIndex = 0; groupLocalIndex < groupCount; ++groupLocalIndex) {
		if (state.groupResidentFlags[groupLocalIndex] == 0u) {
			continue;
		}

		const std::vector<uint32_t> meshPageIndices = GetCLodGroupMeshPageIndices(
			state,
			static_cast<uint32_t>(groupLocalIndex));
		if (std::find(meshPageIndices.begin(), meshPageIndices.end(), meshPageIndex) != meshPageIndices.end()) {
			return true;
		}
	}
	return false;
}

void MeshManager::DeallocateCLodGroupChunkAllocations(CLodSharedStreamingState& state, uint32_t groupLocalIndex) {
	if (groupLocalIndex >= state.residentGroupAllocations.size()) {
		return;
	}

	auto& residentAllocations = state.residentGroupAllocations[groupLocalIndex];

	// Page ownership is managed by CLodStreamingSystem's page LRU.
	// We only clear metadata here, the pages themselves are returned to the
	// LRU by the streaming system when it evicts the group.

	// Clear page-pool fields in the baseline chunk so the shader sees zeros.
	if (groupLocalIndex < state.baselineGroupChunks.size()) {
		auto& chunk = state.baselineGroupChunks[groupLocalIndex];
		ZeroCLodGroupChunkCounts(chunk);
	}

	residentAllocations.Reset();
}

void MeshManager::ReleaseAllCLodGroupChunkAllocations(CLodSharedStreamingState& state) {
	for (uint32_t groupLocalIndex = 0u; groupLocalIndex < state.groupCount; ++groupLocalIndex) {
		DeallocateCLodGroupChunkAllocations(state, groupLocalIndex);
	}
}

void MeshManager::UploadCLodGroupChunkTable(const CLodSharedStreamingState& state) {
	if (state.groupChunksView == nullptr || state.baselineGroupChunks.empty()) {
		return;
	}

	std::vector<ClusterLODGroupChunk> materializedGroupChunks = state.baselineGroupChunks;
	const size_t groupCount = std::min(materializedGroupChunks.size(), state.groupResidentFlags.size());
	for (size_t i = 0; i < groupCount; ++i) {
		if (state.groupResidentFlags[i] == 0u) {
			ZeroCLodGroupChunkCounts(materializedGroupChunks[i]);
		}
	}

	if (m_clodStreamingUploadFn) {
		m_clodStreamingUploadFn(
			materializedGroupChunks.data(),
			materializedGroupChunks.size() * sizeof(ClusterLODGroupChunk),
			rg::runtime::UploadTarget::FromShared(m_clodSharedGroupChunks),
			state.groupChunksView->GetOffset());
		return;
	}

	m_clodSharedGroupChunks->UpdateView(state.groupChunksView, materializedGroupChunks.data());
}

void MeshManager::UploadCLodGroupChunk(const CLodSharedStreamingState& state, uint32_t groupLocalIndex) {
	if (state.groupChunksView == nullptr
		|| groupLocalIndex >= state.baselineGroupChunks.size()
		|| groupLocalIndex >= state.groupResidentFlags.size()) {
		return;
	}

	ClusterLODGroupChunk materializedGroupChunk = state.baselineGroupChunks[groupLocalIndex];
	if (state.groupResidentFlags[groupLocalIndex] == 0u) {
		ZeroCLodGroupChunkCounts(materializedGroupChunk);
	}

	const size_t byteOffset =
		state.groupChunksView->GetOffset() + static_cast<size_t>(groupLocalIndex) * sizeof(ClusterLODGroupChunk);
	const size_t byteSize = sizeof(ClusterLODGroupChunk);
	if (m_clodStreamingUploadFn) {
		m_clodStreamingUploadFn(
			&materializedGroupChunk,
			byteSize,
			rg::runtime::UploadTarget::FromShared(m_clodSharedGroupChunks),
			byteOffset);
		return;
	}

	auto bulkWrite = m_clodSharedGroupChunks->BeginBulkWrite();
	if (bulkWrite.data != nullptr && byteOffset + byteSize <= bulkWrite.capacity) {
		std::memcpy(bulkWrite.data + byteOffset, &materializedGroupChunk, byteSize);
		m_clodSharedGroupChunks->EndBulkWrite(byteOffset, byteSize);
		return;
	}

	UploadCLodGroupChunkTable(state);
}

void MeshManager::UploadCLodGroupPageMapRange(
	CLodSharedStreamingState& state,
	uint32_t pageMapOffset,
	std::span<const GroupPageMapEntry> pageMapEntries) {
	if (state.ownedPageMapView == nullptr || pageMapEntries.empty()) {
		return;
	}

	const size_t byteOffset =
		state.ownedPageMapView->GetOffset() + static_cast<size_t>(pageMapOffset) * sizeof(GroupPageMapEntry);
	const size_t byteSize = pageMapEntries.size_bytes();
	if (m_clodStreamingUploadFn) {
		m_clodStreamingUploadFn(
			pageMapEntries.data(),
			byteSize,
			rg::runtime::UploadTarget::FromShared(m_clodGroupPageMap),
			byteOffset);
		return;
	}

	auto bulkWrite = m_clodGroupPageMap->BeginBulkWrite();
	if (bulkWrite.data != nullptr && byteOffset + byteSize <= bulkWrite.capacity) {
		std::memcpy(bulkWrite.data + byteOffset, pageMapEntries.data(), byteSize);
		m_clodGroupPageMap->EndBulkWrite(byteOffset, byteSize);
		return;
	}

	if (m_clodStreamingUploadFn) {
		m_clodStreamingUploadFn(
			state.pageMapEntriesCPU.data(),
			state.pageMapEntriesCPU.size() * sizeof(GroupPageMapEntry),
			rg::runtime::UploadTarget::FromShared(m_clodGroupPageMap),
			state.ownedPageMapView->GetOffset());
	} else {
		m_clodGroupPageMap->UpdateView(state.ownedPageMapView.get(), state.pageMapEntriesCPU.data());
	}
}

bool MeshManager::ApplyCLodGroupEviction(CLodSharedStreamingState& state, uint32_t groupLocalIndex, bool clearPageMapEntries) {
	if (state.groupChunksView == nullptr || groupLocalIndex >= state.groupCount || groupLocalIndex >= state.baselineGroupChunks.size()) {
		return false;
	}

	if (groupLocalIndex >= state.groupResidentFlags.size()) {
		return false;
	}

	std::vector<uint32_t> meshPageIndices;
	std::vector<uint32_t> pageMapOffsets;
	if (clearPageMapEntries) {
		meshPageIndices = GetCLodGroupMeshPageIndices(state, groupLocalIndex);
		pageMapOffsets = GetCLodGroupPageMapOffsets(state, groupLocalIndex);
	}

	auto clearUnreferencedPageMapEntries = [&]() {
		if (!clearPageMapEntries || state.ownedPageMapView == nullptr) {
			return;
		}

		const GroupPageMapEntry zeroEntry{};
		for (uint32_t pageOffset = 0u; pageOffset < static_cast<uint32_t>(pageMapOffsets.size()); ++pageOffset) {
			const uint32_t pageMapOffset = pageMapOffsets[pageOffset];
			if (pageMapOffset >= state.pageMapEntriesCPU.size()) {
				continue;
			}
			const GroupPageMapEntry previousEntry = state.pageMapEntriesCPU[pageMapOffset];
			state.pageMapEntriesCPU[pageMapOffset] = zeroEntry;
			if (previousEntry.slabDescriptorIndex != 0u && m_clodPageMapWriteCallback) {
				CLodPageMapWriteEvent event{};
				event.reason = CLodPageMapWriteReason::EvictClear;
				event.groupGlobalIndex = state.groupsBase + groupLocalIndex;
				event.groupLocalIndex = groupLocalIndex;
				event.groupsBase = state.groupsBase;
				event.meshPageIndex = pageOffset < static_cast<uint32_t>(meshPageIndices.size()) ? meshPageIndices[pageOffset] : 0u;
				event.pageMapOffset = pageMapOffset;
				event.slabDescriptorIndex = zeroEntry.slabDescriptorIndex;
				event.slabByteOffset = zeroEntry.slabByteOffset;
				event.previousSlabDescriptorIndex = previousEntry.slabDescriptorIndex;
				event.previousSlabByteOffset = previousEntry.slabByteOffset;
				m_clodPageMapWriteCallback(event);
			}
			UploadCLodGroupPageMapRange(state, pageMapOffset, std::span<const GroupPageMapEntry>(&zeroEntry, 1));
		}
	};

	if (!IsCLodGroupResident(state, groupLocalIndex)) {
		UploadCLodGroupChunk(state, groupLocalIndex);
		clearUnreferencedPageMapEntries();
		return true; // Already non-resident.
	}

	state.groupResidentFlags[groupLocalIndex] = 0u;
	{
		uint32_t prev = m_debugResidentGroups.load(std::memory_order_relaxed);
		if (prev > 0u) m_debugResidentGroups.store(prev - 1u, std::memory_order_relaxed);
	}
	// Subtract allocation stats before deallocation zeroes the views.
	if (groupLocalIndex < state.residentGroupAllocations.size()) {
		auto& allocs = state.residentGroupAllocations[groupLocalIndex];
		const uint32_t ac = static_cast<uint32_t>(allocs.pageAllocations.size());
		{
			uint32_t prevAllocs = m_debugResidentAllocations.load(std::memory_order_relaxed);
			m_debugResidentAllocations.store((prevAllocs >= ac) ? (prevAllocs - ac) : 0u, std::memory_order_relaxed);
		}
	}
	DeallocateCLodGroupChunkAllocations(state, groupLocalIndex);
	UploadCLodGroupChunk(state, groupLocalIndex);
	clearUnreferencedPageMapEntries();
	return true;
}

void MeshManager::GetCLodActiveUniqueAssetGroupRanges(std::vector<CLodActiveGroupRange>& outRanges, uint32_t& outMaxGroupIndex) const {
	outRanges.clear();
	outMaxGroupIndex = 0u;

	std::unordered_set<uint64_t> seenRanges;
	seenRanges.reserve(m_clodStreamingStateByInstanceIndex.size());

	for (const auto& [_, state] : m_clodStreamingStateByInstanceIndex) {
		if (state.groupCount == 0u) {
			continue;
		}

		const uint64_t key = (static_cast<uint64_t>(state.groupsBase) << 32ull) | static_cast<uint64_t>(state.groupCount);
		if (!seenRanges.insert(key).second) {
			continue;
		}

		CLodActiveGroupRange range{};
		range.groupsBase = state.groupsBase;
		range.groupCount = state.groupCount;
		outRanges.push_back(range);

		const uint32_t rangeEnd = state.groupsBase + state.groupCount;
		outMaxGroupIndex = std::max(outMaxGroupIndex, rangeEnd);
	}
}

void MeshManager::GetCLodCoarsestUniqueAssetGroupRanges(std::vector<CLodActiveGroupRange>& outRanges) const {
	outRanges.clear();

	std::unordered_set<uint64_t> seenRanges;
	seenRanges.reserve(m_clodStreamingStateByInstanceIndex.size());

	for (const auto& [_, state] : m_clodStreamingStateByInstanceIndex) {
		if (state.groupCount == 0u || state.sharedMeshState == nullptr) {
			continue;
		}

		const uint64_t key = (static_cast<uint64_t>(state.groupsBase) << 32ull) | static_cast<uint64_t>(state.groupCount);
		if (!seenRanges.insert(key).second) {
			continue;
		}

		const auto& coarsestRanges = state.sharedMeshState->coarsestRanges;
		if (coarsestRanges.empty()) {
			continue;
		}

		for (const auto& localRange : coarsestRanges) {
			if (localRange.groupCount == 0u || localRange.firstGroup >= state.groupCount) {
				continue;
			}

			const uint32_t clampedCount = std::min<uint32_t>(
				localRange.groupCount,
				state.groupCount - localRange.firstGroup);
			if (clampedCount == 0u) {
				continue;
			}

			CLodActiveGroupRange range{};
			range.groupsBase = state.groupsBase + localRange.firstGroup;
			range.groupCount = clampedCount;
			outRanges.push_back(range);
		}
	}
}

void MeshManager::GetCLodStreamingDomainSnapshot(CLodStreamingDomainSnapshot& outSnapshot) const {
	outSnapshot.activeRanges.clear();
	outSnapshot.coarsestRanges.clear();
	outSnapshot.maxGroupIndex = 0;

	std::unordered_set<uint64_t> seenRanges;
	seenRanges.reserve(m_clodStreamingStateByInstanceIndex.size());

	for (const auto& [_, state] : m_clodStreamingStateByInstanceIndex) {
		if (state.groupCount == 0u) {
			continue;
		}

		const uint64_t key = (static_cast<uint64_t>(state.groupsBase) << 32ull) | static_cast<uint64_t>(state.groupCount);
		if (!seenRanges.insert(key).second) {
			continue;
		}

		// Active range (same as GetCLodActiveUniqueAssetGroupRanges)
		CLodActiveGroupRange activeRange{};
		activeRange.groupsBase = state.groupsBase;
		activeRange.groupCount = state.groupCount;
		outSnapshot.activeRanges.push_back(activeRange);

		const uint32_t rangeEnd = state.groupsBase + state.groupCount;
		outSnapshot.maxGroupIndex = std::max(outSnapshot.maxGroupIndex, rangeEnd);

		if (state.sharedMeshState == nullptr) {
			continue;
		}

		// Coarsest ranges (same as GetCLodCoarsestUniqueAssetGroupRanges)
		for (const auto& localRange : state.sharedMeshState->coarsestRanges) {
			if (localRange.groupCount == 0u || localRange.firstGroup >= state.groupCount) {
				continue;
			}
			const uint32_t clampedCount = std::min<uint32_t>(
				localRange.groupCount,
				state.groupCount - localRange.firstGroup);
			if (clampedCount == 0u) {
				continue;
			}
			CLodActiveGroupRange coarsest{};
			coarsest.groupsBase = state.groupsBase + localRange.firstGroup;
			coarsest.groupCount = clampedCount;
			outSnapshot.coarsestRanges.push_back(coarsest);
		}
	}
}

void MeshManager::PatchCLodGroupError(uint32_t groupGlobalIndex, float error) {
	// bounds.error is at byte offset 16 within ClusterLODGroup:
	// clodBounds { float center[3]; float radius; float error; }, so error is at offset 16
	constexpr size_t errorFieldOffset = 16;
	const size_t byteOffset = static_cast<size_t>(groupGlobalIndex) * sizeof(ClusterLODGroup) + errorFieldOffset;
	auto handle = m_clusterLODGroups->BeginBulkWrite();
	if (handle.data && byteOffset + sizeof(float) <= handle.capacity) {
		std::memcpy(handle.data + byteOffset, &error, sizeof(float));
		m_clusterLODGroups->EndBulkWrite(byteOffset, sizeof(float));
	}
}

MeshManager::CLodStreamingDebugStats MeshManager::GetCLodStreamingDebugStats() const {
	CLodStreamingDebugStats stats{};
	stats.residentGroups = m_debugResidentGroups.load(std::memory_order_relaxed);
	stats.residentAllocations = m_debugResidentAllocations.load(std::memory_order_relaxed);
	stats.residentAllocationBytes = m_clodPagePool
		? static_cast<uint64_t>(stats.residentAllocations) * m_clodPagePool->GetPageSize()
		: 0ull;
	stats.totalStreamedBytes = m_debugTotalStreamedBytes.load(std::memory_order_relaxed);
	{
		std::lock_guard<std::mutex> lock(m_clodDiskStreamingMutex);
		stats.queuedRequests = static_cast<uint32_t>(m_clodDiskStreamingRequests.size());
		stats.queuedOrInFlightGroups = static_cast<uint32_t>(m_clodDiskStreamingQueuedGroups.size());
		stats.dispatchedOrInFlightGroups =
			stats.queuedOrInFlightGroups > stats.queuedRequests
				? stats.queuedOrInFlightGroups - stats.queuedRequests
				: 0u;
	}
	{
		std::lock_guard<std::mutex> lock(m_clodDiskStreamingResultsMutex);
		stats.completedResults = static_cast<uint32_t>(m_clodDiskStreamingResults.size());
		stats.pendingDirectStorageLaunches = static_cast<uint32_t>(m_clodPendingDirectStorageLaunches.size());
		stats.pendingDirectStorageUploads = static_cast<uint32_t>(m_clodPendingDirectStorageUploads.size());

		auto getCompletedResultSizeBytes = [](const CLodDiskStreamingResult& result) -> uint64_t {
			uint64_t total = 0;
			if (result.directStorageGpuUploadPending) {
				for (uint32_t i = 0; i < static_cast<uint32_t>(result.directStoragePageBlobSizes.size()); ++i) {
					const bool needsFetch = result.segmentNeedsFetch.empty()
						|| i >= static_cast<uint32_t>(result.segmentNeedsFetch.size())
						|| result.segmentNeedsFetch[i];
					if (needsFetch) {
						total += static_cast<uint64_t>(result.directStoragePageBlobSizes[i]);
					}
				}
			} else {
				for (const auto& blob : result.pageBlobs) {
					total += static_cast<uint64_t>(blob.size());
				}
			}
			return total;
		};
		for (const auto& result : m_clodDiskStreamingResults) {
			stats.completedResultBytes += getCompletedResultSizeBytes(result);
		}
	}

	return stats;
}

void MeshManager::GetCLodRayTracingResidencySnapshot(CLodRayTracingResidencySnapshot& outSnapshot) const {
	outSnapshot.residentGroups.clear();
	outSnapshot.pagePool = m_clodPagePool.get();
	outSnapshot.pagePoolGeneration = m_clodDiskStreamingGeneration.load(std::memory_order_acquire);

	const_cast<MeshManager*>(this)->RebuildCLodSharedStreamingRangeIndex();

	std::lock_guard<std::mutex> residencyLock(m_clodResidencyMutex);
	for (const CLodSharedStreamingRange& range : m_clodSharedStreamingRanges) {
		const auto& state = range.state;
		if (!state) {
			continue;
		}

		const uint32_t groupCount = std::min<uint32_t>(
			state->groupCount,
			static_cast<uint32_t>(state->groups.size()));
		for (uint32_t localGroupIndex = 0; localGroupIndex < groupCount; ++localGroupIndex) {
			if (!IsCLodGroupResident(*state, localGroupIndex)) {
				continue;
			}

			if (localGroupIndex >= state->residentGroupAllocations.size() ||
				localGroupIndex >= state->baselineGroupChunks.size()) {
				continue;
			}

			const ClusterLODGroup& group = state->groups[localGroupIndex];
			std::vector<uint32_t> meshPageIndices = GetCLodGroupMeshPageIndices(*state, localGroupIndex);
			if (meshPageIndices.size() != state->residentGroupAllocations[localGroupIndex].pageAllocations.size()) {
				continue;
			}

			CLodRayTracingResidentGroup rtGroup{};
			rtGroup.groupGlobalIndex = range.begin + localGroupIndex;
			rtGroup.groupLocalIndex = localGroupIndex;
			rtGroup.group = group;
			rtGroup.chunk = state->baselineGroupChunks[localGroupIndex];
			rtGroup.meshPageIndices = std::move(meshPageIndices);
			rtGroup.pageAllocations = state->residentGroupAllocations[localGroupIndex].pageAllocations;

			const uint32_t firstSegment = group.firstSegment;
			const uint32_t segmentCount = group.segmentCount;
			if (firstSegment < state->segments.size()) {
				const uint32_t clampedSegmentCount = std::min<uint32_t>(
					segmentCount,
					static_cast<uint32_t>(state->segments.size() - firstSegment));
				rtGroup.segments.assign(
					state->segments.begin() + firstSegment,
					state->segments.begin() + firstSegment + clampedSegmentCount);
			}

			outSnapshot.residentGroups.push_back(std::move(rtGroup));
		}
	}
}

void MeshManager::UpdatePerMeshBuffer(std::unique_ptr<BufferView>& view, PerMeshCB& data) {
	if (!view || !view->GetBuffer()) {
		return;
	}
	view->GetBuffer()->UpdateView(view.get(), &data);
}

std::unique_ptr<BufferView> MeshManager::AllocatePerMeshOverrideBuffer(const PerMeshCB& data) {
	return m_perMeshBuffers->AddData(&data, sizeof(PerMeshCB), sizeof(PerMeshCB));
}

void MeshManager::ReleasePerMeshOverrideBuffer(std::unique_ptr<BufferView>& view) {
	if (view != nullptr) {
		m_perMeshBuffers->Deallocate(view.get());
		view.reset();
	}
}

void MeshManager::UpdatePerMeshInstanceBuffer(std::unique_ptr<BufferView>& view, PerMeshInstanceCB& data) {
	if (!view || !view->GetBuffer()) {
		return;
	}
	view->GetBuffer()->UpdateView(view.get(), &data);
}

std::shared_ptr<Resource> MeshManager::ProvideResource(ResourceIdentifier const& key) {
	return m_resources[key];
}

std::vector<ResourceIdentifier> MeshManager::GetSupportedKeys() {
	std::vector<ResourceIdentifier> keys;
	keys.reserve(m_resources.size());
	for (auto const& [key, _] : m_resources)
		keys.push_back(key);

	return keys;
}
