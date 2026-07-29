#include "Managers/ObjectManager.h"


#include <DirectXMath.h>

#include "Managers/Singletons/ResourceManager.h"
#include "Resources/Buffers/LazyDynamicStructuredBuffer.h"
#include "Resources/Buffers/DynamicBuffer.h"
#include "Resources/Buffers/SortedUnsignedIntBuffer.h"
#include "Mesh/MeshInstance.h"
#include "Utilities/MathUtils.h"
#include "../shaders/Common/defines.h"
#include "../../generated/BuiltinResources.h"
#include "Materials/Material.h"
#include "Render/DrawWorkload.h"
#include "Resources/components.h"
#include "Managers/Singletons/RendererECSManager.h"
#include "Render/MemoryIntrospectionAPI.h"

#include <chrono>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <span>
#include <meshoptimizer.h>
#include <spdlog/spdlog.h>

namespace {

BoundingSphere FitBoundingSpheres(std::span<const BoundingSphere> spheres)
{
	BoundingSphere result{};
	if (spheres.empty()) return result;
	const meshopt_Bounds fitted = meshopt_computeSphereBounds(
		&spheres.front().sphere.x,
		spheres.size(),
		sizeof(BoundingSphere),
		&spheres.front().sphere.w,
		sizeof(BoundingSphere));
	result.sphere = DirectX::XMFLOAT4(
		fitted.center[0], fitted.center[1], fitted.center[2],
		fitted.radius * (1.0f + 1.0e-5f));
	return result;
}

size_t ReserveBytesWithStaticImportHeadroom(size_t requestedBytes, size_t minimumHeadroomBytes) {
	if (requestedBytes == 0) {
		return 0;
	}
	return requestedBytes + (std::max)(requestedBytes, minimumHeadroomBytes);
}

DirectX::XMFLOAT4X4 ComputeNormalMatrixStorage(const DirectX::XMMATRIX& modelMatrix) {
	const DirectX::XMMATRIX upperLeft3x3 = DirectX::XMMatrixSet(
		DirectX::XMVectorGetX(modelMatrix.r[0]), DirectX::XMVectorGetY(modelMatrix.r[0]), DirectX::XMVectorGetZ(modelMatrix.r[0]), 0.0f,
		DirectX::XMVectorGetX(modelMatrix.r[1]), DirectX::XMVectorGetY(modelMatrix.r[1]), DirectX::XMVectorGetZ(modelMatrix.r[1]), 0.0f,
		DirectX::XMVectorGetX(modelMatrix.r[2]), DirectX::XMVectorGetY(modelMatrix.r[2]), DirectX::XMVectorGetZ(modelMatrix.r[2]), 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f);
	DirectX::XMFLOAT4X4 stored{};
	DirectX::XMStoreFloat4x4(&stored, DirectX::XMMatrixTranspose(DirectX::XMMatrixInverse(nullptr, upperLeft3x3)));
	return stored;
}

Components::ObjectDrawInfo::BufferRange ToBufferRange(const DynamicBuffer::PagedAllocation& page) {
	return Components::ObjectDrawInfo::BufferRange{
		page.offset,
		page.allocationSize,
		page.stride,
		page.allocationSize != 0 && page.stride != 0 ? page.allocationSize / page.stride : 0
	};
}

std::size_t SumCounts(const std::vector<std::size_t>& counts) {
	std::size_t total = 0;
	for (const auto count : counts) {
		total += count;
	}
	return total;
}

void ResetPreparedStaticGroupsPlanForReuse(ObjectManager::PreparedStaticGroupsBulkPlan& plan)
{
	plan.transformRows = 0;
	plan.drawRecords = 0;
	plan.preparedBytes = 0;
	plan.prepareUs = 0;
	plan.transformBuildUs = 0;
	plan.workloadBuildUs = 0;
	plan.drawRecordBuildUs = 0;
}

void ResetStaticImportBuildBatchForReuse(ObjectManager::StaticImportBuildBatch& build)
{
	ResetPreparedStaticGroupsPlanForReuse(build.prepared);
	build.transformCounts.clear();
	build.drawRecordCounts.clear();
	build.activeReserveCounts.clear();
	build.drawRecords = 0;
	build.activeInsertIndices = 0;
	build.preparedBytes = 0;
	build.buildUs = 0;
	build.finalized = false;
}

ObjectManager::StaticImportBuildBatch AcquireStaticImportBuildScratch()
{
	return {};
}

void RetireStaticImportBuildScratch(ObjectManager::StaticImportBuildBatch& build) {
	ResetStaticImportBuildBatchForReuse(build);
}

void AppendActiveDrawSetRemoval(
	Components::ObjectDrawInfo& drawInfo,
	const DrawWorkloadKey& workloadKey,
	unsigned int drawRecordIndex)
{
	for (auto& bucket : drawInfo.activeDrawSetRemovals) {
		if (bucket.workloadKey == workloadKey) {
			bucket.indices.push_back(drawRecordIndex);
			return;
		}
	}
	auto& bucket = drawInfo.activeDrawSetRemovals.emplace_back();
	bucket.workloadKey = workloadKey;
	bucket.indices.push_back(drawRecordIndex);
}

std::vector<DrawWorkloadKey> ResolveStaticTemplateWorkloadKeys(const ObjectManager::StaticMeshTemplateRef& meshTemplate)
{
	if (!meshTemplate.workloadKeys.empty()) {
		return meshTemplate.workloadKeys;
	}

	std::vector<DrawWorkloadKey> workloadKeys;
	if (!meshTemplate.mesh || !meshTemplate.material) {
		return workloadKeys;
	}

	ForEachMeshDrawWorkload(*meshTemplate.mesh, *meshTemplate.material, [&](const DrawWorkloadKey& workloadKey) {
		workloadKeys.push_back(workloadKey);
	});
	return workloadKeys;
}

std::vector<DrawWorkloadKey> ResolveStaticTemplateWorkloadKeys(const ObjectManager::PreparedStaticMeshTemplateRef& meshTemplate)
{
	return meshTemplate.workloadKeys;
}

void PrepareStaticGroupsBulkPlanInPlace(
	ObjectManager::PreparedStaticGroupsBulkPlan& plan,
	const std::vector<ObjectManager::StaticGroupBuildInfo>& groups)
{
	ResetPreparedStaticGroupsPlanForReuse(plan);
	if (groups.empty()) {
		plan.groups.clear();
		return;
	}

	const auto prepareBegin = std::chrono::steady_clock::now();
	plan.groups.resize(groups.size());

	const auto transformBuildBegin = std::chrono::steady_clock::now();
	for (std::size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
		const auto& group = groups[groupIndex];
		auto& prepared = plan.groups[groupIndex];
		prepared.stableGroupID = group.stableGroupID;
		prepared.allocationScopeID = group.allocationScopeID;
		prepared.meshTemplates.clear();
		prepared.perObjectCBs.clear();
		prepared.normalMatrices.clear();
		prepared.workloadKeysByMeshTemplate.clear();
		prepared.meshTemplates.reserve(group.meshTemplates.size());
		for (const auto& meshTemplate : group.meshTemplates) {
			auto& preparedTemplate = prepared.meshTemplates.emplace_back();
			preparedTemplate.meshTemplateIndex = meshTemplate.meshTemplateIndex;
			preparedTemplate.clodOffsetIndex = meshTemplate.clodOffsetIndex;
			preparedTemplate.skinnedAssemblyTypeSlot = meshTemplate.skinnedAssemblyTypeSlot;
			preparedTemplate.skinnedAssemblyBounds = meshTemplate.skinnedAssemblyBounds;
			preparedTemplate.skinnedBoundsScale = meshTemplate.skinnedBoundsScale;
			preparedTemplate.workloadKeys = ResolveStaticTemplateWorkloadKeys(meshTemplate);
		}
		prepared.perObjectCBs.reserve(group.instanceTransforms.size());
		prepared.normalMatrices.reserve(group.instanceTransforms.size());
		prepared.workloadKeysByMeshTemplate.reserve(prepared.meshTemplates.size());

		for (const auto& matrix : group.instanceTransforms) {
			PerObjectCB perObject{};
			perObject.modelMatrix = matrix;
			perObject.prevModelMatrix = matrix;
			perObject.modelInverseMatrix = DirectX::XMMatrixInverse(nullptr, matrix);
			const auto determinant = DirectX::XMMatrixDeterminant(matrix);
			perObject.objectFlags = (DirectX::XMVectorGetX(determinant) < 0.0f) ? OBJECT_FLAG_REVERSE_WINDING : 0u;
			prepared.perObjectCBs.push_back(perObject);
			prepared.normalMatrices.push_back(ComputeNormalMatrixStorage(matrix));
		}

		plan.transformRows += prepared.perObjectCBs.size();
		plan.drawRecords += prepared.perObjectCBs.size() * prepared.meshTemplates.size();
		plan.preparedBytes += prepared.perObjectCBs.size() * sizeof(PerObjectCB);
		plan.preparedBytes += prepared.perObjectCBs.size() * sizeof(DirectX::XMFLOAT4X4);
		plan.preparedBytes += prepared.perObjectCBs.size() * prepared.meshTemplates.size() * sizeof(InstanceDrawRecordCB);
	}
	plan.transformBuildUs = static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - transformBuildBegin).count());

	const auto workloadBuildBegin = std::chrono::steady_clock::now();
	for (auto& prepared : plan.groups) {
		for (const auto& meshTemplate : prepared.meshTemplates) {
			prepared.workloadKeysByMeshTemplate.push_back(ResolveStaticTemplateWorkloadKeys(meshTemplate));
		}
	}
	plan.workloadBuildUs = static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - workloadBuildBegin).count());
	plan.prepareUs = static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - prepareBegin).count());
}

}

ObjectManager::ObjectManager() {
	auto& resourceManager = ResourceManager::GetInstance();
	m_perObjectBuffers = DynamicBuffer::CreateShared(sizeof(PerObjectCB), 10000, "perObjectBuffers<PerObjectCB>");
	m_perInstanceTransformBuffers = DynamicBuffer::CreateShared(sizeof(PerInstanceTransformCB), 10000, "perInstanceTransformBuffers<PerInstanceTransformCB>");
	m_instanceDrawRecordBuffers = DynamicBuffer::CreateShared(sizeof(InstanceDrawRecordCB), 10000, "instanceDrawRecordBuffers<InstanceDrawRecordCB>");
	m_drawRecordVisibilityGenerationSidecar = DynamicStructuredBuffer<std::uint32_t>::CreateShared(10000, "drawRecordVisibilityGenerationSidecar<uint>");
	m_skinnedAssemblyPlacements = DynamicStructuredBuffer<SkinnedAssemblyPlacementGPU>::CreateShared(1024, "skinnedAssemblyPlacements");
	m_activeSkinnedAssemblyPlacements = SortedUnsignedIntBuffer::CreateActiveDrawSetShared(1024, "activeSkinnedAssemblyPlacements");
	m_masterIndirectCommandsBuffer = DynamicBuffer::CreateShared(sizeof(DispatchMeshIndirectCommand), 10000, "masterIndirectCommandsBuffer<IndirectCommand>");

	m_normalMatrixBuffer = DynamicBuffer::CreateShared(sizeof(DirectX::XMFLOAT4X4), 10000, "normalMatrixBuffer");

	rg::memory::SetResourceUsageHint(*m_perObjectBuffers, "PerMesh, PerMeshInstance, PerObject");
	rg::memory::SetResourceUsageHint(*m_perInstanceTransformBuffers, "PerMesh, InstanceDrawRecord, PerInstanceTransform");
	rg::memory::SetResourceUsageHint(*m_instanceDrawRecordBuffers, "PerMesh, InstanceDrawRecord, PerInstanceTransform");
	rg::memory::SetResourceUsageHint(*m_drawRecordVisibilityGenerationSidecar, "PerMesh, InstanceDrawRecord, VisibilityGeneration");
	rg::memory::SetResourceUsageHint(*m_normalMatrixBuffer, "PerMesh, PerMeshInstance, PerObject");

	rg::memory::SetResourceUsageHint(*m_masterIndirectCommandsBuffer, "Indirect command buffers");

	m_resources[Builtin::PerObjectBuffer] = m_perObjectBuffers;
	m_resources[Builtin::PerInstanceTransformBuffer] = m_perInstanceTransformBuffers;
	m_resources[Builtin::InstanceDrawRecordBuffer] = m_instanceDrawRecordBuffers;
	m_resources[Builtin::SkinnedAssemblyPlacements] = m_skinnedAssemblyPlacements;
	m_resources[Builtin::ActiveSkinnedAssemblyPlacements] = m_activeSkinnedAssemblyPlacements;
	m_resources[Builtin::NormalMatrixBuffer] = m_normalMatrixBuffer;
	m_resources[Builtin::IndirectCommandBuffers::Master] = m_masterIndirectCommandsBuffer;

	StartDeferredRetireWorker();
	StartActiveDrawSetCompactionWorker();
}

ObjectManager::~ObjectManager() {
	StopActiveDrawSetCompactionWorker();
	StopDeferredRetireWorker();
}

void ObjectManager::StartDeferredRetireWorker() {
	m_deferredRetireStop.store(false, std::memory_order_release);
	m_deferredRetireWorker = std::thread([this]() {
		DeferredRetireWorkerMain();
	});
}

void ObjectManager::StopDeferredRetireWorker() {
	m_deferredRetireStop.store(true, std::memory_order_release);
	m_deferredRetireCv.notify_all();
	if (m_deferredRetireWorker.joinable()) {
		m_deferredRetireWorker.join();
	}

	std::deque<DeferredBufferRangeRetire> pending;
	{
		std::lock_guard lock(m_deferredRetireMutex);
		pending.swap(m_deferredRetireQueue);
		m_deferredRetireQueueDepth.store(0, std::memory_order_relaxed);
	}

	const auto begin = std::chrono::steady_clock::now();
	std::uint64_t retiredRanges = 0;
	std::uint64_t retiredBytes = 0;
	for (const auto& retire : pending) {
		if (retire.buffer && retire.size != 0) {
			retire.buffer->DeallocateRange(retire.offset, retire.size);
			++retiredRanges;
			retiredBytes += retire.size;
		}
	}
	if (retiredRanges != 0) {
		m_deferredRetireRangesRetired.fetch_add(retiredRanges, std::memory_order_relaxed);
		m_deferredRetireBytesRetired.fetch_add(retiredBytes, std::memory_order_relaxed);
		m_deferredRetireWorkerUs.fetch_add(static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count()),
			std::memory_order_relaxed);
	}
}

void ObjectManager::DeferredRetireWorkerMain() {
	while (true) {
		std::vector<DeferredBufferRangeRetire> ready;
		{
			std::unique_lock lock(m_deferredRetireMutex);
			m_deferredRetireCv.wait(lock, [this]() {
				if (m_deferredRetireStop.load(std::memory_order_acquire)) {
					return true;
				}
				const auto completedFrame = m_deferredRetireCompletedFrame.load(std::memory_order_acquire);
				for (const auto& retire : m_deferredRetireQueue) {
					if (retire.retireFrame <= completedFrame) {
						return true;
					}
				}
				return false;
			});

			if (m_deferredRetireStop.load(std::memory_order_acquire)) {
				break;
			}

			const auto completedFrame = m_deferredRetireCompletedFrame.load(std::memory_order_acquire);
			for (auto it = m_deferredRetireQueue.begin(); it != m_deferredRetireQueue.end();) {
				if (it->retireFrame <= completedFrame) {
					ready.push_back(std::move(*it));
					it = m_deferredRetireQueue.erase(it);
				}
				else {
					++it;
				}
			}
			m_deferredRetireQueueDepth.store(m_deferredRetireQueue.size(), std::memory_order_relaxed);
		}

		if (ready.empty()) {
			continue;
		}

		const auto begin = std::chrono::steady_clock::now();
		std::uint64_t retiredRanges = 0;
		std::uint64_t retiredBytes = 0;
		for (const auto& retire : ready) {
			if (retire.buffer && retire.size != 0) {
				retire.buffer->DeallocateRange(retire.offset, retire.size);
				++retiredRanges;
				retiredBytes += retire.size;
			}
		}
		m_deferredRetireRangesRetired.fetch_add(retiredRanges, std::memory_order_relaxed);
		m_deferredRetireBytesRetired.fetch_add(retiredBytes, std::memory_order_relaxed);
		m_deferredRetireWorkerUs.fetch_add(static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count()),
			std::memory_order_relaxed);
	}
}

void ObjectManager::EnqueueDeferredBufferRangeRetire(
	const std::shared_ptr<DynamicBuffer>& buffer,
	std::uint64_t offset,
	std::uint64_t size,
	std::uint64_t retireFrame)
{
	std::vector<DeferredBufferRangeRetire> retires;
	retires.push_back(DeferredBufferRangeRetire{
			buffer,
			offset,
			size,
			retireFrame
		});
	EnqueueDeferredBufferRangeRetires(std::move(retires));
}

void ObjectManager::EnqueueDeferredBufferRangeRetires(
	const std::shared_ptr<DynamicBuffer>& buffer,
	const std::vector<Components::ObjectDrawInfo::BufferRange>& ranges,
	std::uint64_t retireFrame)
{
	if (!buffer) {
		return;
	}
	std::vector<DeferredBufferRangeRetire> retires;
	retires.reserve(ranges.size());
	for (const auto& range : ranges) {
		if (range.IsValid()) {
			retires.push_back(DeferredBufferRangeRetire{
				buffer,
				range.offset,
				range.size,
				retireFrame
			});
		}
	}
	EnqueueDeferredBufferRangeRetires(std::move(retires));
}

void ObjectManager::EnqueueDeferredBufferRangeRetires(std::vector<DeferredBufferRangeRetire> retires)
{
	if (retires.empty()) {
		return;
	}

	std::uint64_t queuedRanges = 0;
	std::uint64_t queuedBytes = 0;
	{
		std::lock_guard lock(m_deferredRetireMutex);
		for (auto& retire : retires) {
			if (!retire.buffer || retire.size == 0) {
				continue;
			}
			queuedBytes += retire.size;
			++queuedRanges;
			m_deferredRetireQueue.push_back(std::move(retire));
		}
		m_deferredRetireQueueDepth.store(m_deferredRetireQueue.size(), std::memory_order_relaxed);
	}
	if (queuedRanges == 0) {
		return;
	}
	m_deferredRetireRangesQueued.fetch_add(queuedRanges, std::memory_order_relaxed);
	m_deferredRetireBytesQueued.fetch_add(queuedBytes, std::memory_order_relaxed);
	m_deferredRetireCv.notify_one();
}

void ObjectManager::StartActiveDrawSetCompactionWorker() {
	m_activeDrawSetCompactionStop.store(false, std::memory_order_release);
	m_activeDrawSetCompactionWorker = std::thread([this]() {
		ActiveDrawSetCompactionWorkerMain();
	});
}

void ObjectManager::StopActiveDrawSetCompactionWorker() {
	m_activeDrawSetCompactionStop.store(true, std::memory_order_release);
	m_activeDrawSetCompactionCv.notify_all();
	if (m_activeDrawSetCompactionWorker.joinable()) {
		m_activeDrawSetCompactionWorker.join();
	}

	std::lock_guard lock(m_activeDrawSetCompactionMutex);
	m_activeDrawSetCompactionRequests.clear();
	m_activeDrawSetCompactionJobs.clear();
	m_activeDrawSetCompactionResults.clear();
	m_activeDrawSetCompactionQueued.clear();
}

void ObjectManager::ActiveDrawSetCompactionWorkerMain() {
	while (true) {
		ActiveDrawSetCompactionJob job;
		{
			std::unique_lock lock(m_activeDrawSetCompactionMutex);
			m_activeDrawSetCompactionCv.wait(lock, [this]() {
				return m_activeDrawSetCompactionStop.load(std::memory_order_acquire) ||
					!m_activeDrawSetCompactionJobs.empty();
			});
			if (m_activeDrawSetCompactionStop.load(std::memory_order_acquire)) {
				break;
			}
			job = std::move(m_activeDrawSetCompactionJobs.front());
			m_activeDrawSetCompactionJobs.pop_front();
		}

		ZoneScopedN("ObjectManager::ActiveDrawSetCompactionWorker");
		ZoneValue(job.entries.size());
		const auto begin = std::chrono::steady_clock::now();
		std::vector<SortedUnsignedIntBuffer::ActiveDrawSetEntry> compacted;
		compacted.reserve(job.entries.size());
		for (const auto& entry : job.entries) {
			if (entry.generation == 0u || entry.drawRecordIndex >= job.visibilityGenerations.size()) {
				continue;
			}
			if (job.visibilityGenerations[entry.drawRecordIndex] == entry.generation) {
				compacted.push_back(entry);
			}
		}

		ActiveDrawSetCompactionResult result;
		result.workloadKey = job.workloadKey;
		result.buffer = std::move(job.buffer);
		result.entries = std::move(compacted);
		result.activeSetRevision = job.activeSetRevision;
		result.visibilityRevision = job.visibilityRevision;
		result.inputEntries = job.entries.size();
		result.buildUs = static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count());

		{
			std::lock_guard lock(m_activeDrawSetCompactionMutex);
			m_activeDrawSetCompactionResults.push_back(std::move(result));
		}
	}
}

void ObjectManager::MaybeQueueActiveDrawSetCompaction(
	const DrawWorkloadKey& workloadKey,
	const std::shared_ptr<SortedUnsignedIntBuffer>& buffer)
{
	if (!buffer || !buffer->ActiveEntryMode()) {
		return;
	}

	const auto totalEntries = static_cast<std::uint64_t>(buffer->Size());
	const auto staleEntries = static_cast<std::uint64_t>(buffer->ActiveTombstoneEstimate());
	if (totalEntries < 1024u || staleEntries == 0) {
		return;
	}

	if (staleEntries < 256u && staleEntries * 100u < totalEntries) {
		return;
	}

	{
		std::lock_guard lock(m_activeDrawSetCompactionMutex);
		if (m_activeDrawSetCompactionQueued.contains(workloadKey)) {
			return;
		}
		m_activeDrawSetCompactionQueued.insert(workloadKey);
		m_activeDrawSetCompactionRequests.push_back(workloadKey);
	}
}

void ObjectManager::PumpActiveDrawSetCompactionRequests(std::size_t maxRequests) {
	if (maxRequests == 0) {
		return;
	}

	std::vector<ActiveDrawSetCompactionJob> jobs;
	jobs.reserve(maxRequests);
	for (std::size_t i = 0; i < maxRequests; ++i) {
		DrawWorkloadKey workloadKey;
		{
			std::lock_guard lock(m_activeDrawSetCompactionMutex);
			if (m_activeDrawSetCompactionRequests.empty()) {
				break;
			}
			workloadKey = m_activeDrawSetCompactionRequests.front();
			m_activeDrawSetCompactionRequests.pop_front();
		}

		auto activeDrawSetIt = m_activeDrawSetIndices.find(workloadKey);
		auto buffer = activeDrawSetIt != m_activeDrawSetIndices.end() ? activeDrawSetIt->second : nullptr;
		if (!buffer || !buffer->ActiveEntryMode()) {
			std::lock_guard lock(m_activeDrawSetCompactionMutex);
			m_activeDrawSetCompactionQueued.erase(workloadKey);
			continue;
		}

		const auto totalEntries = static_cast<std::uint64_t>(buffer->Size());
		const auto staleEntries = static_cast<std::uint64_t>(buffer->ActiveTombstoneEstimate());
		if (totalEntries < 1024u ||
			staleEntries == 0 ||
			(staleEntries < 256u && staleEntries * 100u < totalEntries)) {
			std::lock_guard lock(m_activeDrawSetCompactionMutex);
			m_activeDrawSetCompactionQueued.erase(workloadKey);
			continue;
		}

		ZoneScopedN("ObjectManager::PumpActiveDrawSetCompactionRequests::BuildSnapshot");
		ZoneValue(totalEntries);
		ActiveDrawSetCompactionJob job;
		job.workloadKey = workloadKey;
		job.buffer = buffer;
		job.activeSetRevision = buffer->MutationRevision();
		job.visibilityRevision = m_drawRecordVisibilityRevision;
		job.entries = buffer->SnapshotActiveEntries();
		job.visibilityGenerations = m_drawRecordVisibilityGenerations;
		m_stats.activeDrawSetCompactionInputEntries += job.entries.size();
		jobs.push_back(std::move(job));
	}

	if (jobs.empty()) {
		return;
	}

	{
		std::lock_guard lock(m_activeDrawSetCompactionMutex);
		for (auto& job : jobs) {
			m_activeDrawSetCompactionJobs.push_back(std::move(job));
			++m_stats.activeDrawSetCompactionJobsQueued;
		}
	}
	m_activeDrawSetCompactionCv.notify_one();
}

std::vector<ObjectManager::ActiveDrawSetCompactionPublishResult> ObjectManager::PublishActiveDrawSetCompactionResults(std::size_t maxResults) {
	std::vector<ActiveDrawSetCompactionPublishResult> published;
	if (maxResults == 0) {
		return published;
	}

	PumpActiveDrawSetCompactionRequests(maxResults);

	std::vector<ActiveDrawSetCompactionResult> results;
	results.reserve(maxResults);
	published.reserve(maxResults);
	{
		std::lock_guard lock(m_activeDrawSetCompactionMutex);
		while (!m_activeDrawSetCompactionResults.empty() && results.size() < maxResults) {
			results.push_back(std::move(m_activeDrawSetCompactionResults.front()));
			m_activeDrawSetCompactionResults.pop_front();
		}
	}

	for (auto& result : results) {
		ZoneScopedN("ObjectManager::PublishActiveDrawSetCompactionResult");
		ZoneValue(result.entries.size());
		TracyPlot("ObjectManager.ActiveCompaction.ResultInputEntries", static_cast<int64_t>(result.inputEntries));
		TracyPlot("ObjectManager.ActiveCompaction.ResultOutputEntries", static_cast<int64_t>(result.entries.size()));
		{
			std::lock_guard lock(m_activeDrawSetCompactionMutex);
			m_activeDrawSetCompactionQueued.erase(result.workloadKey);
		}
		++m_stats.activeDrawSetCompactionJobsBuilt;
		m_stats.activeDrawSetCompactionWorkerUs += result.buildUs;

		auto buffer = result.buffer;
		if (!buffer ||
			buffer->MutationRevision() != result.activeSetRevision ||
			m_drawRecordVisibilityRevision != result.visibilityRevision) {
			TracyPlot("ObjectManager.ActiveCompaction.ResultStale", int64_t{ 1 });
			++m_stats.activeDrawSetCompactionJobsStale;
			if (buffer) {
				MaybeQueueActiveDrawSetCompaction(result.workloadKey, buffer);
			}
			continue;
		}
		TracyPlot("ObjectManager.ActiveCompaction.ResultStale", int64_t{ 0 });

		const auto publishBegin = std::chrono::steady_clock::now();
		const auto inputEntries = result.inputEntries;
		buffer->AssignActiveSnapshot(std::move(result.entries));
		m_stats.activeDrawSetCompactionJobsPublished += 1;
		m_stats.activeDrawSetCompactionOutputEntries += buffer->LiveSize();
		m_stats.activeDrawSetCompactionPublishUs += static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - publishBegin).count());
		++m_drawSetDeclarationRevision;
		published.push_back(ActiveDrawSetCompactionPublishResult{
			.workloadKey = result.workloadKey,
			.activeSpan = static_cast<std::uint32_t>((std::min<std::uint64_t>)(
				buffer->Size(),
				std::numeric_limits<std::uint32_t>::max())),
			.inputEntries = inputEntries,
			.outputEntries = buffer->LiveSize()
		});
	}

	return published;
}

std::vector<ObjectManager::ActiveDrawSetDebugStats> ObjectManager::SnapshotActiveDrawSetDebugStats() const {
	std::vector<ActiveDrawSetDebugStats> stats;
	stats.reserve(m_activeDrawSetIndices.size());
	for (const auto& [workloadKey, buffer] : m_activeDrawSetIndices) {
		if (!buffer) {
			continue;
		}

		ActiveDrawSetDebugStats row{};
		row.workloadKey = workloadKey;
		row.span = buffer->Size();
		row.liveSize = buffer->LiveSize();
		row.tombstoneEstimate = buffer->ActiveTombstoneEstimate();

		if (buffer->ActiveEntryMode()) {
			const auto entries = buffer->SnapshotActiveEntries();
			for (const auto& entry : entries) {
				if (entry.generation == 0u || entry.drawRecordIndex >= m_drawRecordVisibilityGenerations.size()) {
					++row.cpuGenerationOutOfRange;
					continue;
				}
				if (m_drawRecordVisibilityGenerations[entry.drawRecordIndex] == entry.generation) {
					++row.cpuGenerationMatches;
				} else {
					++row.cpuGenerationStale;
				}
			}
		} else {
			row.cpuGenerationMatches = row.span;
		}

		stats.push_back(row);
	}

	std::sort(stats.begin(), stats.end(), [](const auto& lhs, const auto& rhs) {
		const auto lhsBad = lhs.cpuGenerationStale + lhs.cpuGenerationOutOfRange;
		const auto rhsBad = rhs.cpuGenerationStale + rhs.cpuGenerationOutOfRange;
		if (lhsBad != rhsBad) {
			return lhsBad > rhsBad;
		}
		return lhs.span > rhs.span;
	});

	return stats;
}

void ObjectManager::PublishDeferredRetireCompletedFrame(std::uint64_t completedFrame, std::uint64_t retireDelayFrames) {
	m_deferredRetireDelayFrames.store((std::max<std::uint64_t>)(1u, retireDelayFrames), std::memory_order_release);
	auto observed = m_deferredRetireCompletedFrame.load(std::memory_order_acquire);
	while (completedFrame > observed &&
		!m_deferredRetireCompletedFrame.compare_exchange_weak(
			observed,
			completedFrame,
			std::memory_order_acq_rel,
			std::memory_order_acquire)) {
	}
	if (completedFrame >= observed) {
		m_deferredRetireCv.notify_all();
	}
}

std::uint64_t ObjectManager::MakeDeferredRetireFrame() const {
	return m_deferredRetireCompletedFrame.load(std::memory_order_acquire)
		+ m_deferredRetireDelayFrames.load(std::memory_order_acquire);
}

ObjectManager::Stats ObjectManager::GetStats() const {
	auto stats = m_stats;
	stats.deferredRetireRangesQueued = m_deferredRetireRangesQueued.load(std::memory_order_relaxed);
	stats.deferredRetireRangesRetired = m_deferredRetireRangesRetired.load(std::memory_order_relaxed);
	stats.deferredRetireBytesQueued = m_deferredRetireBytesQueued.load(std::memory_order_relaxed);
	stats.deferredRetireBytesRetired = m_deferredRetireBytesRetired.load(std::memory_order_relaxed);
	stats.deferredRetireQueueDepth = m_deferredRetireQueueDepth.load(std::memory_order_relaxed);
	stats.deferredRetireWorkerUs = m_deferredRetireWorkerUs.load(std::memory_order_relaxed);
	return stats;
}

std::shared_ptr<SortedUnsignedIntBuffer> ObjectManager::EnsureActiveDrawSetIndices(const DrawWorkloadKey& workloadKey, std::size_t initialCapacity) {
	auto it = m_activeDrawSetIndices.find(workloadKey);
	if (it != m_activeDrawSetIndices.end()) {
		return it->second;
	}

	auto debugName =
		"activeDrawSetIndices(flags=" + std::to_string(static_cast<uint64_t>(workloadKey.compileFlags))
		+ ", phase=" + std::to_string(workloadKey.renderPhase.hash)
		+ ", clodOnly=" + std::to_string(workloadKey.clodOnly ? 1 : 0) + ")";
	const auto capacity = (std::max<std::uint64_t>)(1u, static_cast<std::uint64_t>(initialCapacity));
	auto buffer = SortedUnsignedIntBuffer::CreateActiveDrawSetShared(capacity, debugName);
	rg::memory::SetResourceUsageHint(*buffer, "PerMesh, PerMeshInstance, PerObject");
	buffer->GetECSEntity().add<Components::IsActiveDrawSetIndices>();
	buffer->GetECSEntity().set<Components::Resource>({ buffer });
	buffer->GetECSEntity().add<Components::ParticipatesInPass>(
		RendererECSManager::GetInstance().GetRenderPhaseEntity(workloadKey.renderPhase));
	if (workloadKey.clodOnly) {
		buffer->GetECSEntity().add<Components::CLodOnlyDrawWorkload>();
	}
	else {
		buffer->GetECSEntity().add<Components::GeneralDrawWorkload>();
	}
	m_activeDrawSetIndices[workloadKey] = buffer;
	++m_drawSetDeclarationRevision;
	return buffer;
}

std::uint32_t ObjectManager::ActivateDrawRecordCPU(std::uint32_t drawRecordIndex) {
	if (drawRecordIndex >= m_drawRecordVisibilityGenerations.size()) {
		m_drawRecordVisibilityGenerations.resize(static_cast<std::size_t>(drawRecordIndex) + 1u, 0u);
	}
	auto generation = m_drawRecordVisibilityGenerations[drawRecordIndex] + 1u;
	if (generation == 0u) {
		generation = 1u;
	}
	m_drawRecordVisibilityGenerations[drawRecordIndex] = generation;
	++m_drawRecordVisibilityRevision;
	return generation;
}

std::uint32_t ObjectManager::AdvanceDrawRecordVisibilityGenerationCPU(std::uint32_t drawRecordIndex) {
	if (drawRecordIndex >= m_drawRecordVisibilityGenerations.size()) {
		return 0u;
	}
	auto generation = m_drawRecordVisibilityGenerations[drawRecordIndex] + 1u;
	if (generation == 0u) {
		generation = 1u;
	}
	m_drawRecordVisibilityGenerations[drawRecordIndex] = generation;
	return generation;
}

std::uint32_t ObjectManager::ActivateDrawRecord(std::uint32_t drawRecordIndex) {
	const auto previousGenerationRows = m_drawRecordVisibilityGenerations.size();
	const auto previousSidecarRows = m_drawRecordVisibilityGenerationSidecar
		? m_drawRecordVisibilityGenerationSidecar->Data().size()
		: 0u;
	const auto generation = ActivateDrawRecordCPU(drawRecordIndex);
	const auto requiredRows = static_cast<std::size_t>(drawRecordIndex) + 1u;
	if (requiredRows > previousGenerationRows || requiredRows > previousSidecarRows) {
		ZoneScopedN("ObjectManager::ActivateDrawRecord::StageVisibilityGenerationFullAfterGrow");
		m_drawRecordVisibilityGenerationSidecar->EnsureSize(m_drawRecordVisibilityGenerations.size(), 0u);
		m_drawRecordVisibilityGenerationSidecar->StageRange(
			0u,
			std::span<const std::uint32_t>(
				m_drawRecordVisibilityGenerations.data(),
				m_drawRecordVisibilityGenerations.size()));
	} else {
		m_drawRecordVisibilityGenerationSidecar->StageRange(
			drawRecordIndex,
			std::span<const std::uint32_t>(&generation, 1u));
	}
	return generation;
}

void ObjectManager::AssignStaticImportTransactionGenerations(MaterializedStaticImportTransaction& transaction) {
	MaterializedStaticImportTransaction* transactions[] = { &transaction };
	AssignStaticImportTransactionGenerations(std::span<MaterializedStaticImportTransaction*>(transactions));
}

void ObjectManager::AssignStaticImportTransactionGenerations(std::span<MaterializedStaticImportTransaction*> transactions) {
	if (transactions.empty()) {
		return;
	}

	std::unordered_map<std::uint32_t, std::uint32_t> generationByDrawRecordIndex;
	std::size_t activatedDrawRecords = 0;
	const auto activate = [this, &generationByDrawRecordIndex, &activatedDrawRecords](
		MaterializedStaticImportTransaction& transaction,
		std::uint32_t drawRecordIndex) -> std::uint32_t {
		transaction.reservation.visibilityDirtyStart =
			(std::min)(transaction.reservation.visibilityDirtyStart, static_cast<std::size_t>(drawRecordIndex));
		transaction.reservation.visibilityDirtyEnd =
			(std::max)(transaction.reservation.visibilityDirtyEnd, static_cast<std::size_t>(drawRecordIndex) + 1u);

		const auto it = generationByDrawRecordIndex.find(drawRecordIndex);
		if (it != generationByDrawRecordIndex.end()) {
			return it->second;
		}

		const auto generation = ActivateDrawRecordCPU(drawRecordIndex);
		generationByDrawRecordIndex.emplace(drawRecordIndex, generation);
		++activatedDrawRecords;
		return generation;
	};

	for (auto* transactionPtr : transactions) {
		if (!transactionPtr) {
			continue;
		}

		auto& transaction = *transactionPtr;
		transaction.reservation.visibilityDirtyStart = std::numeric_limits<std::size_t>::max();
		transaction.reservation.visibilityDirtyEnd = 0;

		for (const auto& drawInfo : transaction.drawInfos) {
			for (const auto drawRecordIndex : drawInfo.instanceDrawRecordIndices) {
				activate(transaction, drawRecordIndex);
			}
		}
		for (auto& [workloadKey, entries] : transaction.activeDrawSetInserts) {
			(void)workloadKey;
			for (auto& entry : entries) {
				entry.generation = activate(transaction, entry.drawRecordIndex);
			}
		}
	}

	TracyPlot("ObjectManager.StaticImportTransaction.PublishActivatedDrawRecords", static_cast<int64_t>(activatedDrawRecords));
}

void ObjectManager::TombstoneDrawRecord(std::uint32_t drawRecordIndex) {
	if (drawRecordIndex >= m_drawRecordVisibilityGenerations.size()) {
		return;
	}
	const auto generation = AdvanceDrawRecordVisibilityGenerationCPU(drawRecordIndex);
	++m_drawRecordVisibilityRevision;
	m_drawRecordVisibilityGenerationSidecar->StageRange(
		drawRecordIndex,
		std::span<const std::uint32_t>(&generation, 1u));
}

void ObjectManager::TombstoneDrawRecords(std::span<const std::uint32_t> drawRecordIndices) {
	if (drawRecordIndices.empty()) {
		return;
	}
	TracyPlot("ObjectManager.TombstoneDrawRecords.Requested", static_cast<int64_t>(drawRecordIndices.size()));

	std::vector<std::uint32_t> sortedIndices;
	sortedIndices.reserve(drawRecordIndices.size());
	for (const auto drawRecordIndex : drawRecordIndices) {
		if (drawRecordIndex >= m_drawRecordVisibilityGenerations.size()) {
			continue;
		}
		sortedIndices.push_back(drawRecordIndex);
	}
	if (sortedIndices.empty()) {
		TracyPlot("ObjectManager.TombstoneDrawRecords.Valid", int64_t{ 0 });
		return;
	}
	++m_drawRecordVisibilityRevision;
	TracyPlot("ObjectManager.TombstoneDrawRecords.Valid", static_cast<int64_t>(sortedIndices.size()));

	std::sort(sortedIndices.begin(), sortedIndices.end());
	sortedIndices.erase(std::unique(sortedIndices.begin(), sortedIndices.end()), sortedIndices.end());
	TracyPlot("ObjectManager.TombstoneDrawRecords.Unique", static_cast<int64_t>(sortedIndices.size()));
	for (const auto drawRecordIndex : sortedIndices) {
		AdvanceDrawRecordVisibilityGenerationCPU(drawRecordIndex);
	}

	std::size_t runStart = sortedIndices.front();
	std::size_t runCount = 1;
	std::size_t stagedRuns = 0;
	const auto stageRun = [this](std::size_t start, std::size_t count) {
		if (count == 0) {
			return;
		}
		m_drawRecordVisibilityGenerationSidecar->StageRange(
			start,
			std::span<const std::uint32_t>(
				m_drawRecordVisibilityGenerations.data() + start,
				count));
	};

	for (std::size_t i = 1; i < sortedIndices.size(); ++i) {
		const auto index = static_cast<std::size_t>(sortedIndices[i]);
		if (index == runStart + runCount) {
			++runCount;
			continue;
		}
		stageRun(runStart, runCount);
		++stagedRuns;
		runStart = index;
		runCount = 1;
	}
	stageRun(runStart, runCount);
	++stagedRuns;
	TracyPlot("ObjectManager.TombstoneDrawRecords.StagedRuns", static_cast<int64_t>(stagedRuns));
}

void ObjectManager::AppendActiveDrawSetEntries(
	const DrawWorkloadKey& workloadKey,
	const std::vector<SortedUnsignedIntBuffer::ActiveDrawSetEntry>& entries)
{
	if (entries.empty()) {
		return;
	}
	auto buffer = EnsureActiveDrawSetIndices(workloadKey, entries.size());
	buffer->AppendActiveEntries(entries);
	buffer->SetLiveSize(buffer->LiveSize() + entries.size());
}

Components::ObjectDrawInfo ObjectManager::AddObject(const PerObjectCB& perObjectCB, const Components::MeshInstances* meshInstances) {
	std::vector<ObjectBuildInfo> objects;
	objects.push_back({ perObjectCB, meshInstances, nullptr });
	auto drawInfos = AddObjectsBulk(objects);
	return drawInfos.empty() ? Components::ObjectDrawInfo{} : std::move(drawInfos.front());
}

std::vector<Components::ObjectDrawInfo> ObjectManager::AddObjectsBulk(const std::vector<ObjectBuildInfo>& objects) {
	std::vector<Components::ObjectDrawInfo> drawInfos;
	if (objects.empty()) {
		return drawInfos;
	}

	++m_stats.bulkAddCalls;
	m_stats.objectsSubmitted += objects.size();

	struct PendingDrawRecord {
		size_t objectIndex = 0;
		std::vector<DrawWorkloadKey> workloadKeys;
	};

	drawInfos.resize(objects.size());

	struct ObjectTransformRange {
		size_t first = 0;
		size_t count = 0;
	};

	std::vector<PerObjectCB> perObjectCBs;
	std::vector<DirectX::XMFLOAT4X4> normalMatrices;
	std::vector<ObjectTransformRange> transformRanges;
	transformRanges.reserve(objects.size());

	for (const auto& object : objects) {
		ObjectTransformRange range;
		range.first = perObjectCBs.size();
		if (object.instanceTransforms && !object.instanceTransforms->transforms.empty()) {
			range.count = object.instanceTransforms->transforms.size();
			for (const auto& transform : object.instanceTransforms->transforms) {
				auto perObject = object.perObjectCB;
				perObject.modelMatrix = transform.matrix;
				perObject.prevModelMatrix = transform.matrix;
				perObject.modelInverseMatrix = DirectX::XMMatrixInverse(nullptr, transform.matrix);
				const auto determinant = DirectX::XMMatrixDeterminant(transform.matrix);
				perObject.objectFlags = (DirectX::XMVectorGetX(determinant) < 0.0f) ? OBJECT_FLAG_REVERSE_WINDING : 0u;
				perObjectCBs.push_back(perObject);
				normalMatrices.push_back(ComputeNormalMatrixStorage(transform.matrix));
			}
		} else {
			range.count = 1;
			perObjectCBs.push_back(object.perObjectCB);
			normalMatrices.push_back(ComputeNormalMatrixStorage(object.perObjectCB.modelMatrix));
		}
		transformRanges.push_back(range);
	}

	m_stats.perObjectRowsAllocated += perObjectCBs.size();
	m_stats.perInstanceTransformRowsAllocated += perObjectCBs.size();
	m_stats.normalMatrixRowsAllocated += normalMatrices.size();

	std::uint64_t reserveUs = 0;
	if (!perObjectCBs.empty()) {
		const auto reserveBegin = std::chrono::steady_clock::now();
		const auto perObjectBytes = perObjectCBs.size() * sizeof(PerObjectCB);
		const auto instanceTransformBytes = perObjectCBs.size() * sizeof(PerInstanceTransformCB);
		const auto normalMatrixBytes = normalMatrices.size() * sizeof(DirectX::XMFLOAT4X4);
		m_perObjectBuffers->ReserveBytes(perObjectBytes);
		m_perInstanceTransformBuffers->ReserveBytes(instanceTransformBytes);
		m_normalMatrixBuffer->ReserveBytes(normalMatrixBytes);
		m_stats.bulkReservedPerObjectBytes += perObjectBytes;
		m_stats.bulkReservedInstanceTransformBytes += instanceTransformBytes;
		m_stats.bulkReservedNormalMatrixRows += normalMatrices.size();
		reserveUs += static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - reserveBegin).count());
	}

	auto normalMatrixViews = m_normalMatrixBuffer->AddDataBatch(normalMatrices.data(), normalMatrices.size(), sizeof(DirectX::XMFLOAT4X4));
	for (size_t i = 0; i < perObjectCBs.size() && i < normalMatrixViews.size(); ++i) {
		perObjectCBs[i].normalMatrixBufferIndex = static_cast<uint32_t>(normalMatrixViews[i]->GetOffset() / sizeof(DirectX::XMFLOAT4X4));
	}
	auto perObjectViews = m_perObjectBuffers->AddDataBatch(perObjectCBs.data(), perObjectCBs.size(), sizeof(PerObjectCB));
	auto instanceTransformViews = m_perInstanceTransformBuffers->AddDataBatch(perObjectCBs.data(), perObjectCBs.size(), sizeof(PerInstanceTransformCB));

	std::vector<InstanceDrawRecordCB> drawRecords;
	std::vector<PendingDrawRecord> pendingDrawRecords;
	std::unordered_map<DrawWorkloadKey, std::vector<SortedUnsignedIntBuffer::ActiveDrawSetEntry>, DrawWorkloadKey::Hasher> activeDrawSetInserts;

	size_t expectedDraws = 0;
	for (const auto& object : objects) {
		if (object.meshInstances) {
			const size_t instanceCount = (object.instanceTransforms && !object.instanceTransforms->transforms.empty())
				? object.instanceTransforms->transforms.size()
				: 1u;
			m_stats.meshTemplateRowsReferenced += object.meshInstances->meshInstances.size();
			expectedDraws += object.meshInstances->meshInstances.size() * instanceCount;
		}
	}
	drawRecords.reserve(expectedDraws);
	pendingDrawRecords.reserve(expectedDraws);

	for (size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex) {
		const auto& object = objects[objectIndex];
		auto& drawInfo = drawInfos[objectIndex];

		if (objectIndex >= transformRanges.size()) {
			continue;
		}
		const auto transformRange = transformRanges[objectIndex];
		if (transformRange.count == 0 ||
			transformRange.first >= perObjectViews.size() ||
			transformRange.first >= normalMatrixViews.size()) {
			continue;
		}

		auto& perObjectCBview = perObjectViews[transformRange.first];
		auto& normalMatrixView = normalMatrixViews[transformRange.first];
		const uint32_t perObjectIndex = static_cast<uint32_t>(perObjectCBview->GetOffset() / sizeof(PerObjectCB));

		drawInfo.perObjectCBView = perObjectCBview;
		drawInfo.perObjectCBIndex = perObjectIndex;
		drawInfo.normalMatrixView = normalMatrixView;
		drawInfo.normalMatrixIndex = static_cast<uint32_t>(normalMatrixView->GetOffset() / sizeof(DirectX::XMFLOAT4X4));
		drawInfo.perObjectCBViews.reserve(transformRange.count);
		drawInfo.perInstanceTransformViews.reserve(transformRange.count);
		drawInfo.normalMatrixViews.reserve(transformRange.count);
		for (size_t i = 0; i < transformRange.count; ++i) {
			const auto transformViewIndex = transformRange.first + i;
			if (transformViewIndex < perObjectViews.size()) {
				drawInfo.perObjectCBViews.push_back(perObjectViews[transformViewIndex]);
			}
			if (transformViewIndex < instanceTransformViews.size()) {
				drawInfo.perInstanceTransformViews.push_back(instanceTransformViews[transformViewIndex]);
			}
			if (transformViewIndex < normalMatrixViews.size()) {
				drawInfo.normalMatrixViews.push_back(normalMatrixViews[transformViewIndex]);
			}
		}

		if (object.meshInstances == nullptr) {
			continue;
		}

		drawInfo.drawInfo.indices.reserve(object.meshInstances->meshInstances.size());
		drawInfo.drawInfo.views.reserve(object.meshInstances->meshInstances.size());
		drawInfo.drawInfo.drawWorkloadKeysPerDraw.reserve(object.meshInstances->meshInstances.size());
		drawInfo.perMeshInstanceBufferIndices.reserve(object.meshInstances->meshInstances.size());
		drawInfo.instanceDrawRecordIndices.reserve(object.meshInstances->meshInstances.size() * transformRange.count);
		drawInfo.instanceDrawRecordViews.reserve(object.meshInstances->meshInstances.size() * transformRange.count);

		for (size_t transformIndex = 0; transformIndex < transformRange.count; ++transformIndex) {
			const auto transformViewIndex = transformRange.first + transformIndex;
			if (transformViewIndex >= perObjectViews.size() || transformViewIndex >= instanceTransformViews.size()) {
				continue;
			}
			const uint32_t meshPerObjectIndex = static_cast<uint32_t>(perObjectViews[transformViewIndex]->GetOffset() / sizeof(PerObjectCB));
			const uint32_t instanceTransformIndex = static_cast<uint32_t>(instanceTransformViews[transformViewIndex]->GetOffset() / sizeof(PerInstanceTransformCB));
			for (size_t meshInstanceIndex = 0; meshInstanceIndex < object.meshInstances->meshInstances.size(); ++meshInstanceIndex) {
				auto& meshInstance = object.meshInstances->meshInstances[meshInstanceIndex];
				if (!meshInstance) {
					continue;
				}
				auto& mesh = meshInstance->GetMesh();
				if (meshInstance->GetPerMeshInstanceBufferView() == nullptr) {
					spdlog::warn(
						"ObjectManager::AddObjectsBulk: skipping mesh instance with no per-mesh-instance buffer view objectIndex={} meshInstanceIndex={}",
						objectIndex,
						meshInstanceIndex);
					continue;
				}
				const uint32_t perMeshInstanceBufferIndex = static_cast<uint32_t>(meshInstance->GetPerMeshInstanceBufferOffset() / sizeof(PerMeshInstanceCB));
				if (transformIndex == 0) {
					meshInstance->SetPerObjectBufferIndex(meshPerObjectIndex);
				}
				InstanceDrawRecordCB drawRecord{};
				drawRecord.meshTemplateIndex = perMeshInstanceBufferIndex;
				drawRecord.instanceTransformIndex = instanceTransformIndex;
				drawRecord.clodOffsetIndex = perMeshInstanceBufferIndex;
				drawRecord.skinnedAssemblyPlacementIndex = 0xFFFFFFFFu;
				drawRecord.skinningTypeSlot = meshInstance->GetPerMeshInstanceBufferData().skinningInstanceSlot;
				drawRecords.push_back(drawRecord);
				if (transformIndex == 0) {
					drawInfo.perMeshInstanceBufferIndices.push_back(perMeshInstanceBufferIndex);
				}
				PendingDrawRecord pendingDrawRecord;
				pendingDrawRecord.objectIndex = objectIndex;
				auto material = meshInstance->GetEffectiveMaterial();
				if (!material) {
					material = mesh->material;
				}
				ForEachMeshDrawWorkload(*mesh, *material, [&](const DrawWorkloadKey& workloadKey) {
					pendingDrawRecord.workloadKeys.push_back(workloadKey);
				});
				pendingDrawRecords.push_back(std::move(pendingDrawRecord));
			}
		}
	}

	if (!drawRecords.empty()) {
		const auto reserveBegin = std::chrono::steady_clock::now();
		const auto drawRecordBytes = drawRecords.size() * sizeof(InstanceDrawRecordCB);
		m_instanceDrawRecordBuffers->ReserveBytes(drawRecordBytes);
		m_stats.bulkReservedDrawRecordBytes += drawRecordBytes;
		reserveUs += static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - reserveBegin).count());
		auto drawRecordViews = m_instanceDrawRecordBuffers->AddDataBatch(drawRecords.data(), drawRecords.size(), sizeof(InstanceDrawRecordCB));
		m_stats.instanceDrawRecordsAllocated += drawRecordViews.size();
		for (size_t drawRecordViewIndex = 0; drawRecordViewIndex < drawRecordViews.size() && drawRecordViewIndex < pendingDrawRecords.size(); ++drawRecordViewIndex) {
			const auto& pendingDrawRecord = pendingDrawRecords[drawRecordViewIndex];
			auto& drawInfo = drawInfos[pendingDrawRecord.objectIndex];
			auto& view = drawRecordViews[drawRecordViewIndex];
			const auto drawRecordIndex = static_cast<unsigned int>(view->GetOffset() / sizeof(InstanceDrawRecordCB));
			if (drawRecordIndex > 0xFFFFFFu) {
				spdlog::warn("ObjectManager::AddObjectsBulk: instance draw record index {} exceeds packed visible-cluster 24-bit capacity", drawRecordIndex);
			}
			m_stats.maxDrawRecordIndex = std::max<std::uint64_t>(m_stats.maxDrawRecordIndex, drawRecordIndex);
			const auto drawRecordGeneration = ActivateDrawRecord(drawRecordIndex);

			drawInfo.drawInfo.indices.push_back(drawRecordIndex);
			drawInfo.drawInfo.views.push_back(view);
			drawInfo.drawInfo.drawWorkloadKeysPerDraw.push_back(pendingDrawRecord.workloadKeys);
			drawInfo.instanceDrawRecordIndices.push_back(drawRecordIndex);
			drawInfo.instanceDrawRecordViews.push_back(view);
			for (const auto& workloadKey : pendingDrawRecord.workloadKeys) {
				activeDrawSetInserts[workloadKey].push_back(SortedUnsignedIntBuffer::ActiveDrawSetEntry{
					.drawRecordIndex = drawRecordIndex,
					.generation = drawRecordGeneration
				});
				AppendActiveDrawSetRemoval(drawInfo, workloadKey, drawRecordIndex);
			}
		}
	}

	++m_stats.bulkReserveCalls;
	m_stats.bulkReserveUs += reserveUs;

	for (const auto& [workloadKey, entries] : activeDrawSetInserts) {
		if (!entries.empty()) {
			const auto insertBegin = std::chrono::steady_clock::now();
			AppendActiveDrawSetEntries(workloadKey, entries);
			const auto insertEnd = std::chrono::steady_clock::now();
			m_stats.activeDrawSetInsertCalls += 1;
			m_stats.activeDrawSetInsertIndices += entries.size();
			m_stats.activeDrawSetInsertUs += static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::microseconds>(insertEnd - insertBegin).count());
		}
	}

	return drawInfos;
}

ObjectManager::PreparedStaticGroupsBulkPlan ObjectManager::PrepareStaticGroupsBulkPlan(const std::vector<StaticGroupBuildInfo>& groups) {
	PreparedStaticGroupsBulkPlan plan;
	PrepareStaticGroupsBulkPlanInPlace(plan, groups);
	return plan;
}

void ObjectManager::PrepareStaticGroupCommitResourcesAsync(const PreparedStaticGroupsBulkPlan& plan) {
	ZoneScopedN("ObjectManager::PrepareStaticGroupCommitResourcesAsync");
	if (plan.groups.empty()) {
		return;
	}

	const size_t transformRows = static_cast<size_t>(plan.transformRows);
	const size_t drawRecords = static_cast<size_t>(plan.drawRecords);
	TracyPlot("ObjectManager.StaticCommitResourceRequest.Groups", static_cast<int64_t>(plan.groups.size()));
	TracyPlot("ObjectManager.StaticCommitResourceRequest.TransformRows", static_cast<int64_t>(transformRows));
	TracyPlot("ObjectManager.StaticCommitResourceRequest.DrawRecords", static_cast<int64_t>(drawRecords));
	if (transformRows != 0) {
		{
			ZoneScopedN("ObjectManager::PrepareStaticGroupCommitResourcesAsync::RequestPerObject");
			const auto bytes = ReserveBytesWithStaticImportHeadroom(transformRows * sizeof(PerObjectCB), 512ull * 1024ull);
			TracyPlot("ObjectManager.StaticCommitResourceRequest.PerObjectBytes", static_cast<int64_t>(bytes));
			m_perObjectBuffers->RequestAsyncReserveBytes(bytes);
		}
		{
			ZoneScopedN("ObjectManager::PrepareStaticGroupCommitResourcesAsync::RequestInstanceTransform");
			const auto bytes = ReserveBytesWithStaticImportHeadroom(transformRows * sizeof(PerInstanceTransformCB), 512ull * 1024ull);
			TracyPlot("ObjectManager.StaticCommitResourceRequest.InstanceTransformBytes", static_cast<int64_t>(bytes));
			m_perInstanceTransformBuffers->RequestAsyncReserveBytes(bytes);
		}
		{
			ZoneScopedN("ObjectManager::PrepareStaticGroupCommitResourcesAsync::RequestNormalMatrix");
			const auto bytes = ReserveBytesWithStaticImportHeadroom(transformRows * sizeof(DirectX::XMFLOAT4X4), 512ull * 1024ull);
			TracyPlot("ObjectManager.StaticCommitResourceRequest.NormalMatrixBytes", static_cast<int64_t>(bytes));
			m_normalMatrixBuffer->RequestAsyncReserveBytes(bytes);
		}
	}
	if (drawRecords != 0) {
		{
			ZoneScopedN("ObjectManager::PrepareStaticGroupCommitResourcesAsync::RequestInstanceDrawRecord");
			const auto bytes = ReserveBytesWithStaticImportHeadroom(drawRecords * sizeof(InstanceDrawRecordCB), 1024ull * 1024ull);
			TracyPlot("ObjectManager.StaticCommitResourceRequest.InstanceDrawRecordBytes", static_cast<int64_t>(bytes));
			m_instanceDrawRecordBuffers->RequestAsyncReserveBytes(bytes);
		}
	}
}

ObjectManager::StaticImportPacketPlan ObjectManager::PrepareStaticImportPacketPlan(const std::vector<StaticGroupBuildInfo>& groups) {
	StaticImportPacketPlan plan;
	plan.prepared = PrepareStaticGroupsBulkPlan(groups);
	return plan;
}

void ObjectManager::RequestStaticImportPacketResources(const StaticImportPacketPlan& plan) {
	ZoneScopedN("ObjectManager::RequestStaticImportPacketResources");
	PrepareStaticGroupCommitResourcesAsync(plan.prepared);
}

ObjectManager::StaticImportBuildBatch ObjectManager::PrepareStaticImportBuildBatch(const std::vector<StaticGroupBuildInfo>& groups) {
	StaticImportBuildBatch build = AcquireStaticImportBuildScratch();
	PrepareStaticGroupsBulkPlanInPlace(build.prepared, groups);
	FinalizeStaticImportBuildBatch(build);
	return build;
}

void ObjectManager::FinalizeStaticImportBuildBatch(StaticImportBuildBatch& build) {
	if (build.finalized) {
		return;
	}

	build.transformCounts.clear();
	build.drawRecordCounts.clear();
	build.activeReserveCounts.clear();
	build.transformCounts.reserve(build.prepared.groups.size());
	build.drawRecordCounts.reserve(build.prepared.groups.size());
	build.drawRecords = 0;
	build.activeInsertIndices = 0;
	build.preparedBytes = build.prepared.preparedBytes;

	for (const auto& group : build.prepared.groups) {
		const auto transforms = group.perObjectCBs.size();
		const auto records = transforms * group.meshTemplates.size();
		build.transformCounts.push_back(transforms);
		build.drawRecordCounts.push_back(records);
		build.drawRecords += records;
		for (std::size_t meshIndex = 0; meshIndex < group.meshTemplates.size(); ++meshIndex) {
			if (meshIndex >= group.workloadKeysByMeshTemplate.size()) {
				continue;
			}
			for (const auto& workloadKey : group.workloadKeysByMeshTemplate[meshIndex]) {
				build.activeReserveCounts[workloadKey] += transforms;
				build.activeInsertIndices += transforms;
			}
		}
	}
	build.finalized = true;
}

void ObjectManager::RequestStaticImportTransactionResources(const StaticImportBuildBatch& build) {
	ZoneScopedN("ObjectManager::RequestStaticImportTransactionResources");
	PrepareStaticGroupCommitResourcesAsync(build.prepared);
}

ObjectManager::StaticImportResourceProbe ObjectManager::CreateStaticImportResourceProbe() const {
	ZoneScopedN("ObjectManager::CreateStaticImportResourceProbe");
	StaticImportResourceProbe probe;
	{
		ZoneScopedN("ObjectManager::CreateStaticImportResourceProbe::SnapshotNormalMatrix");
		probe.normalMatrix = m_normalMatrixBuffer->SnapshotAllocationProbe();
		TracyPlot("ObjectManager.StaticImportResourceProbe.NormalMatrixFreeBlocks", static_cast<int64_t>(probe.normalMatrix.freeBlocks.size()));
	}
	{
		ZoneScopedN("ObjectManager::CreateStaticImportResourceProbe::SnapshotPerObject");
		probe.perObject = m_perObjectBuffers->SnapshotAllocationProbe();
		TracyPlot("ObjectManager.StaticImportResourceProbe.PerObjectFreeBlocks", static_cast<int64_t>(probe.perObject.freeBlocks.size()));
	}
	{
		ZoneScopedN("ObjectManager::CreateStaticImportResourceProbe::SnapshotInstanceTransform");
		probe.instanceTransform = m_perInstanceTransformBuffers->SnapshotAllocationProbe();
		TracyPlot("ObjectManager.StaticImportResourceProbe.InstanceTransformFreeBlocks", static_cast<int64_t>(probe.instanceTransform.freeBlocks.size()));
	}
	{
		ZoneScopedN("ObjectManager::CreateStaticImportResourceProbe::SnapshotInstanceDrawRecord");
		probe.instanceDrawRecord = m_instanceDrawRecordBuffers->SnapshotAllocationProbe();
		TracyPlot("ObjectManager.StaticImportResourceProbe.InstanceDrawRecordFreeBlocks", static_cast<int64_t>(probe.instanceDrawRecord.freeBlocks.size()));
	}
	return probe;
}

ObjectManager::StaticImportResourceProbeStatus ObjectManager::ProbeStaticImportTransactionResources(
	StaticImportBuildBatch& build,
	StaticImportResourceProbe& probe)
{
	ZoneScopedN("ObjectManager::ProbeStaticImportTransactionResources");
	{
		ZoneScopedN("ObjectManager::ProbeStaticImportTransactionResources::FinalizeBuildBatch");
		FinalizeStaticImportBuildBatch(build);
		if (build.prepared.groups.empty()) {
			return StaticImportResourceProbeStatus::Empty;
		}
	}

	{
		ZoneScopedN("ObjectManager::ProbeStaticImportTransactionResources::RequestResources");
		RequestStaticImportTransactionResources(build);
	}
	{
		ZoneScopedN("ObjectManager::ProbeStaticImportTransactionResources::RequestActiveDrawSetReserves");
		for (const auto& [workloadKey, count] : build.activeReserveCounts) {
			auto buffer = EnsureActiveDrawSetIndices(workloadKey, static_cast<std::size_t>(count));
			buffer->RequestAsyncReserveCapacity(static_cast<std::uint64_t>(buffer->Size()) + count);
		}
	}

	{
		ZoneScopedN("ObjectManager::ProbeStaticImportTransactionResources::ProbeResources");
		const std::size_t transformRows = SumCounts(build.transformCounts);
		const std::size_t drawRecordRows = SumCounts(build.drawRecordCounts);
		const std::size_t normalMatrixBytes = transformRows * sizeof(DirectX::XMFLOAT4X4);
		const std::size_t perObjectBytes = transformRows * sizeof(PerObjectCB);
		const std::size_t instanceTransformBytes = transformRows * sizeof(PerInstanceTransformCB);
		const std::size_t instanceDrawRecordBytes = drawRecordRows * sizeof(InstanceDrawRecordCB);

		if (!DynamicBuffer::CanConsumeAllocationProbeBytes(
				probe.normalMatrix,
				normalMatrixBytes)) {
			return StaticImportResourceProbeStatus::PendingNormalMatrix;
		}
		if (!DynamicBuffer::CanConsumeAllocationProbeBytes(
				probe.perObject,
				perObjectBytes)) {
			return StaticImportResourceProbeStatus::PendingPerObject;
		}
		if (!DynamicBuffer::CanConsumeAllocationProbeBytes(
				probe.instanceTransform,
				instanceTransformBytes)) {
			return StaticImportResourceProbeStatus::PendingInstanceTransform;
		}
		if (!DynamicBuffer::CanConsumeAllocationProbeBytes(
				probe.instanceDrawRecord,
				instanceDrawRecordBytes)) {
			return StaticImportResourceProbeStatus::PendingDrawRecord;
		}

		const bool consumedNormalMatrix = DynamicBuffer::TryConsumeAllocationProbeBytes(
			probe.normalMatrix,
			normalMatrixBytes);
		const bool consumedPerObject = DynamicBuffer::TryConsumeAllocationProbeBytes(
			probe.perObject,
			perObjectBytes);
		const bool consumedInstanceTransform = DynamicBuffer::TryConsumeAllocationProbeBytes(
			probe.instanceTransform,
			instanceTransformBytes);
		const bool consumedInstanceDrawRecord = DynamicBuffer::TryConsumeAllocationProbeBytes(
			probe.instanceDrawRecord,
			instanceDrawRecordBytes);
		assert(consumedNormalMatrix);
		assert(consumedPerObject);
		assert(consumedInstanceTransform);
		assert(consumedInstanceDrawRecord);
		(void)consumedNormalMatrix;
		(void)consumedPerObject;
		(void)consumedInstanceTransform;
		(void)consumedInstanceDrawRecord;
	}

	return StaticImportResourceProbeStatus::Ready;
}

ObjectManager::StaticImportReservationStatus ObjectManager::TryReserveStaticImportTransaction(
	StaticImportBuildBatch build,
	StaticImportReservation& reservation)
{
	ZoneScopedN("ObjectManager::TryReserveStaticImportTransaction");
	std::vector<StaticImportBuildBatch> builds;
	builds.push_back(std::move(build));
	std::vector<StaticImportReservation> reservations;
	const auto statuses = TryReserveStaticImportTransactionsBatch(builds, reservations);
	if (!reservations.empty()) {
		reservation = std::move(reservations.front());
	} else {
		reservation = {};
	}
	return statuses.empty() ? StaticImportReservationStatus::Empty : statuses.front();
}

std::vector<ObjectManager::StaticImportReservationStatus> ObjectManager::TryReserveStaticImportTransactionsBatch(
	std::vector<StaticImportBuildBatch>& builds,
	std::vector<StaticImportReservation>& reservations)
{
	ZoneScopedN("ObjectManager::TryReserveStaticImportTransactionsBatch");
	std::vector<StaticImportBuildBatch*> buildPtrs;
	buildPtrs.reserve(builds.size());
	for (auto& build : builds) {
		buildPtrs.push_back(std::addressof(build));
	}
	auto statuses = TryReserveStaticImportTransactionsInPlace(buildPtrs, reservations);
	for (std::size_t i = 0; i < builds.size() && i < reservations.size() && i < statuses.size(); ++i) {
		if (statuses[i] == StaticImportReservationStatus::Ready) {
			reservations[i].build = std::move(builds[i]);
		}
	}
	return statuses;
}

std::vector<ObjectManager::StaticImportReservationStatus> ObjectManager::TryReserveStaticImportTransactionsInPlace(
	std::span<StaticImportBuildBatch*> builds,
	std::vector<StaticImportReservation>& reservations)
{
	ZoneScopedN("ObjectManager::TryReserveStaticImportTransactionsInPlace");
	std::vector<StaticImportReservationStatus> statuses(builds.size(), StaticImportReservationStatus::Empty);
	reservations.clear();
	reservations.resize(builds.size());
	if (builds.empty()) {
		return statuses;
	}

	std::unordered_map<DrawWorkloadKey, std::uint64_t, DrawWorkloadKey::Hasher> activeReserveCounts;
	for (auto* buildPtr : builds) {
		if (!buildPtr) {
			continue;
		}
		auto& build = *buildPtr;
		FinalizeStaticImportBuildBatch(build);
		if (build.prepared.groups.empty()) {
			continue;
		}
		for (const auto& [workloadKey, count] : build.activeReserveCounts) {
			activeReserveCounts[workloadKey] += count;
		}
	}
	for (const auto& [workloadKey, count] : activeReserveCounts) {
		auto buffer = EnsureActiveDrawSetIndices(workloadKey, static_cast<std::size_t>(count));
		buffer->RequestAsyncReserveCapacity(static_cast<std::uint64_t>(buffer->Size()) + count);
	}

	std::size_t readyCount = 0;
	std::uint64_t readyDrawRecords = 0;
	std::uint64_t readyBytes = 0;
	std::size_t pendingCount = 0;
	for (std::size_t buildIndex = 0; buildIndex < builds.size(); ++buildIndex) {
		auto* buildPtr = builds[buildIndex];
		auto& reservation = reservations[buildIndex];
		reservation = {};
		if (!buildPtr) {
			statuses[buildIndex] = StaticImportReservationStatus::Empty;
			continue;
		}
		auto& build = *buildPtr;
		if (build.prepared.groups.empty()) {
			statuses[buildIndex] = StaticImportReservationStatus::Empty;
			continue;
		}

		std::vector<DynamicBuffer::PagedAllocation> normalRanges;
		std::vector<DynamicBuffer::PagedAllocation> perObjectRanges;
		std::vector<DynamicBuffer::PagedAllocation> instanceTransformRanges;
		std::vector<DynamicBuffer::PagedAllocation> drawRecordRanges;
		if (!m_normalMatrixBuffer->TryAllocateRangesBatch(
				build.transformCounts,
				sizeof(DirectX::XMFLOAT4X4),
				normalRanges,
				DynamicBuffer::ReadyResizePublishMode::DoNotPublish)) {
			statuses[buildIndex] = StaticImportReservationStatus::PendingResources;
			++pendingCount;
			continue;
		}
		if (!m_perObjectBuffers->TryAllocateRangesBatch(
				build.transformCounts,
				sizeof(PerObjectCB),
				perObjectRanges,
				DynamicBuffer::ReadyResizePublishMode::DoNotPublish)) {
			m_normalMatrixBuffer->DeallocatePages(normalRanges);
			statuses[buildIndex] = StaticImportReservationStatus::PendingResources;
			++pendingCount;
			continue;
		}
		if (!m_perInstanceTransformBuffers->TryAllocateRangesBatch(
				build.transformCounts,
				sizeof(PerInstanceTransformCB),
				instanceTransformRanges,
				DynamicBuffer::ReadyResizePublishMode::DoNotPublish)) {
			m_normalMatrixBuffer->DeallocatePages(normalRanges);
			m_perObjectBuffers->DeallocatePages(perObjectRanges);
			statuses[buildIndex] = StaticImportReservationStatus::PendingResources;
			++pendingCount;
			continue;
		}
		if (!m_instanceDrawRecordBuffers->TryAllocateRangesBatch(
				build.drawRecordCounts,
				sizeof(InstanceDrawRecordCB),
				drawRecordRanges,
				DynamicBuffer::ReadyResizePublishMode::DoNotPublish)) {
			m_normalMatrixBuffer->DeallocatePages(normalRanges);
			m_perObjectBuffers->DeallocatePages(perObjectRanges);
			m_perInstanceTransformBuffers->DeallocatePages(instanceTransformRanges);
			statuses[buildIndex] = StaticImportReservationStatus::PendingResources;
			++pendingCount;
			continue;
		}

		reservation.id = m_nextStaticImportTransactionID++;
		reservation.transformCounts = build.transformCounts;
		reservation.drawRecordCounts = build.drawRecordCounts;
		reservation.normalMatrixRanges = std::move(normalRanges);
		reservation.perObjectRanges = std::move(perObjectRanges);
		reservation.instanceTransformRanges = std::move(instanceTransformRanges);
		reservation.instanceDrawRecordRanges = std::move(drawRecordRanges);
		reservation.perObjectBuffer = m_perObjectBuffers;
		reservation.instanceTransformBuffer = m_perInstanceTransformBuffers;
		reservation.normalMatrixBuffer = m_normalMatrixBuffer;
		reservation.instanceDrawRecordBuffer = m_instanceDrawRecordBuffers;
		reservation.groupCount = build.prepared.groups.size();
		reservation.preparedBytes = build.preparedBytes;
		reservation.drawRecords = build.drawRecords;

		statuses[buildIndex] = StaticImportReservationStatus::Ready;
		++readyCount;
		readyDrawRecords += reservation.drawRecords;
		readyBytes += reservation.preparedBytes;
	}

	TracyPlot("ObjectManager.StaticImportTransaction.BatchReserved", static_cast<int64_t>(readyCount));
	TracyPlot("ObjectManager.StaticImportTransaction.BatchReservedDrawRecords", static_cast<int64_t>(readyDrawRecords));
	TracyPlot("ObjectManager.StaticImportTransaction.BatchReservedBytes", static_cast<int64_t>(readyBytes));
	TracyPlot("ObjectManager.StaticImportTransaction.BatchPendingResources", static_cast<int64_t>(pendingCount));
	TracyPlot("ObjectManager.StaticImportTransaction.BatchBlockedAfterPending", int64_t{ 0 });
	return statuses;
}

ObjectManager::MaterializedStaticImportTransaction ObjectManager::MaterializeStaticImportTransaction(StaticImportReservation reservation) const {
	return MaterializeStaticImportTransaction(std::move(reservation), reservation.build);
}

ObjectManager::MaterializedStaticImportTransaction ObjectManager::MaterializeStaticImportTransaction(
	StaticImportReservation&& reservation,
	StaticImportBuildBatch& buildScratch) const
{
	ZoneScopedN("ObjectManager::MaterializeStaticImportTransaction");

	const auto materializeBegin = std::chrono::steady_clock::now();
	MaterializedStaticImportTransaction transaction;
	const auto& build = buildScratch;
	const bool reservationOwnsBuildScratch = std::addressof(buildScratch) == std::addressof(reservation.build);
	const auto groupCount = build.prepared.groups.size();

	transaction.drawInfos.resize(groupCount);
	transaction.perObjectRows.reserve(static_cast<std::size_t>(build.prepared.transformRows));
	transaction.normalRows.reserve(static_cast<std::size_t>(build.prepared.transformRows));
	transaction.drawRecordRows.reserve(static_cast<std::size_t>(reservation.drawRecords));
	
	for (std::size_t groupIndex = 0; groupIndex < groupCount; ++groupIndex) {
		ZoneScopedN("ObjectManager::MaterializeStaticImportTransaction::ProcessGroup");
		const auto& group = build.prepared.groups[groupIndex];
		auto& drawInfo = transaction.drawInfos[groupIndex];
		const auto transformCount = group.perObjectCBs.size();
		const auto perObjectRange = groupIndex < reservation.perObjectRanges.size()
			? reservation.perObjectRanges[groupIndex]
			: DynamicBuffer::PagedAllocation{};
		const auto instanceTransformRange = groupIndex < reservation.instanceTransformRanges.size()
			? reservation.instanceTransformRanges[groupIndex]
			: DynamicBuffer::PagedAllocation{};
		const auto normalRange = groupIndex < reservation.normalMatrixRanges.size()
			? reservation.normalMatrixRanges[groupIndex]
			: DynamicBuffer::PagedAllocation{};
		const auto drawRecordRange = groupIndex < reservation.instanceDrawRecordRanges.size()
			? reservation.instanceDrawRecordRanges[groupIndex]
			: DynamicBuffer::PagedAllocation{};

		drawInfo.perObjectCBRange = ToBufferRange(perObjectRange);
		drawInfo.perInstanceTransformRange = ToBufferRange(instanceTransformRange);
		drawInfo.normalMatrixRange = ToBufferRange(normalRange);
		drawInfo.instanceDrawRecordRange = ToBufferRange(drawRecordRange);
		drawInfo.perObjectCBIndex = static_cast<std::uint32_t>(drawInfo.perObjectCBRange.offset / sizeof(PerObjectCB));
		drawInfo.normalMatrixIndex = static_cast<std::uint32_t>(drawInfo.normalMatrixRange.offset / sizeof(DirectX::XMFLOAT4X4));
		drawInfo.perMeshInstanceBufferIndices.reserve(group.meshTemplates.size());
		drawInfo.instanceDrawRecordIndices.reserve(transformCount * group.meshTemplates.size());
		drawInfo.drawInfo.indices.reserve(transformCount * group.meshTemplates.size());
		drawInfo.drawInfo.drawWorkloadKeysPerDraw.reserve(transformCount * group.meshTemplates.size());

		{
			ZoneScopedN("ObjectManager::MaterializeStaticImportTransaction::SetupPerObjectAndNormalRows");
			const auto groupTransformFirst = transaction.perObjectRows.size();
			for (std::size_t i = 0; i < transformCount; ++i) {
				auto perObject = group.perObjectCBs[i];
				perObject.normalMatrixBufferIndex = static_cast<std::uint32_t>(
					(normalRange.offset + i * sizeof(DirectX::XMFLOAT4X4)) / sizeof(DirectX::XMFLOAT4X4));
				transaction.perObjectRows.push_back(perObject);
				if (i < group.normalMatrices.size()) {
					transaction.normalRows.push_back(group.normalMatrices[i]);
				}
			}
		}

		for (const auto& meshTemplate : group.meshTemplates) {
			drawInfo.perMeshInstanceBufferIndices.push_back(meshTemplate.meshTemplateIndex);
		}

		struct TypeBounds {
			std::uint32_t slot;
			std::vector<BoundingSphere> components;
			BoundingSphere bounds;
			float scale;
		};
		std::vector<TypeBounds> skinnedTypes;
		for (const auto& meshTemplate : group.meshTemplates) {
			if (meshTemplate.skinnedAssemblyTypeSlot == 0xFFFFFFFFu) continue;
			auto found = std::ranges::find(skinnedTypes, meshTemplate.skinnedAssemblyTypeSlot, &TypeBounds::slot);
			if (found == skinnedTypes.end()) {
				skinnedTypes.push_back({ meshTemplate.skinnedAssemblyTypeSlot,
					{ meshTemplate.skinnedAssemblyBounds }, meshTemplate.skinnedAssemblyBounds, meshTemplate.skinnedBoundsScale });
			} else {
				found->components.push_back(meshTemplate.skinnedAssemblyBounds);
				found->scale = (std::max)(found->scale, meshTemplate.skinnedBoundsScale);
			}
		}
		for (auto& type : skinnedTypes) type.bounds = FitBoundingSpheres(type.components);
		const std::size_t skinnedPlacementBase = transaction.skinnedAssemblyPlacements.size();
		for (const auto& type : skinnedTypes) {
			for (std::uint32_t transformIndex = 0; transformIndex < transformCount; ++transformIndex) {
				MaterializedStaticImportTransaction::PendingSkinnedAssemblyPlacement pending{};
				pending.groupIndex = groupIndex;
				pending.placement.instanceTransformIndex = static_cast<std::uint32_t>(
					instanceTransformRange.offset / sizeof(PerInstanceTransformCB)) + transformIndex;
				pending.placement.skinningTypeSlot = type.slot;
				const std::uint64_t stable = group.stableGroupID;
				pending.placement.stableSceneID = static_cast<std::uint32_t>(
					(stable ^ (stable >> 32u)) * 747796405u + transformIndex * 2891336453u + type.slot * 277803737u);
				pending.placement.localBoundingSphere = type.bounds.sphere;
				pending.placement.boundsScale = type.scale;
				transaction.skinnedAssemblyPlacements.push_back(pending);
			}
		}

		{
			ZoneScopedN("ObjectManager::MaterializeStaticImportTransaction::SetupDrawRecordRows");
			std::size_t localDrawRecordOrdinal = 0;
			for (std::size_t transformIndex = 0; transformIndex < transformCount; ++transformIndex) {
				const auto instanceTransformOffset = instanceTransformRange.offset + transformIndex * sizeof(PerInstanceTransformCB);
				for (std::size_t meshTemplateIndex = 0; meshTemplateIndex < group.meshTemplates.size(); ++meshTemplateIndex) {
					const auto& meshTemplate = group.meshTemplates[meshTemplateIndex];
					const auto drawRecordOffset = drawRecordRange.offset + localDrawRecordOrdinal * sizeof(InstanceDrawRecordCB);
					const auto drawRecordIndex = static_cast<unsigned int>(drawRecordOffset / sizeof(InstanceDrawRecordCB));

					InstanceDrawRecordCB record{};
					record.meshTemplateIndex = meshTemplate.meshTemplateIndex;
					record.instanceTransformIndex = static_cast<std::uint32_t>(instanceTransformOffset / sizeof(PerInstanceTransformCB));
					record.clodOffsetIndex = meshTemplate.clodOffsetIndex;
					record.skinnedAssemblyPlacementIndex = 0xFFFFFFFFu;
					record.skinningTypeSlot = meshTemplate.skinnedAssemblyTypeSlot;
					if (meshTemplate.skinnedAssemblyTypeSlot != 0xFFFFFFFFu) {
						const auto typeIt = std::ranges::find(
							skinnedTypes,
							meshTemplate.skinnedAssemblyTypeSlot,
							&TypeBounds::slot);
						if (typeIt != skinnedTypes.end()) {
							const auto typeOrdinal = static_cast<std::size_t>(
								std::distance(skinnedTypes.begin(), typeIt));
							const auto pendingPlacementIndex =
								skinnedPlacementBase + typeOrdinal * transformCount + transformIndex;
							if (pendingPlacementIndex < transaction.skinnedAssemblyPlacements.size()) {
								transaction.skinnedAssemblyPlacements[pendingPlacementIndex]
									.drawRecordRowIndices.push_back(transaction.drawRecordRows.size());
							}
						}
					}
					transaction.drawRecordRows.push_back(record);

					drawInfo.drawInfo.indices.push_back(drawRecordIndex);
					drawInfo.instanceDrawRecordIndices.push_back(drawRecordIndex);
					if (meshTemplateIndex < group.workloadKeysByMeshTemplate.size()) {
						const auto& workloadKeys = group.workloadKeysByMeshTemplate[meshTemplateIndex];
						drawInfo.drawInfo.drawWorkloadKeysPerDraw.push_back(workloadKeys);
						for (const auto& workloadKey : workloadKeys) {
							transaction.activeDrawSetInserts[workloadKey].push_back(SortedUnsignedIntBuffer::ActiveDrawSetEntry{
								.drawRecordIndex = drawRecordIndex,
								.generation = 0u
							});
							AppendActiveDrawSetRemoval(drawInfo, workloadKey, drawRecordIndex);
						}
					} else {
						drawInfo.drawInfo.drawWorkloadKeysPerDraw.push_back({});
					}
					++localDrawRecordOrdinal;
				}
			}
		}
	}

	{
		ZoneScopedN("ObjectManager::MaterializeStaticImportTransaction::BuildRemovalPayloads");
		transaction.removalPayloads.reserve(transaction.drawInfos.size());
		for (const auto& drawInfo : transaction.drawInfos) {
			transaction.removalPayloads.push_back(BuildStaticObjectRemovalPayload(std::span<const Components::ObjectDrawInfo>(&drawInfo, 1)));
		}
	}

	transaction.materializeUs = static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - materializeBegin).count());
	{
		ZoneScopedN("ObjectManager::MaterializeStaticImportTransaction::RetireBuildData");
		RetireStaticImportBuildScratch(buildScratch);
	}
	if (reservationOwnsBuildScratch) {
		reservation.build = {};
	}
	transaction.reservation = std::move(reservation);
	return transaction;
}

void ObjectManager::PublishSkinnedAssemblyPlacements(MaterializedStaticImportTransaction& transaction) {
	if (transaction.skinnedAssemblyPlacements.empty()) return;
	std::vector<SortedUnsignedIntBuffer::ActiveDrawSetEntry> activeEntries;
	activeEntries.reserve(transaction.skinnedAssemblyPlacements.size());
	for (auto& pending : transaction.skinnedAssemblyPlacements) {
		const auto placementIndex = AllocateSkinnedAssemblyPlacement(pending.placement);
		pending.placement = m_skinnedAssemblyPlacementCPU[placementIndex];
		for (const auto rowIndex : pending.drawRecordRowIndices) {
			if (rowIndex < transaction.drawRecordRows.size()) {
				transaction.drawRecordRows[rowIndex].skinnedAssemblyPlacementIndex = placementIndex;
			}
		}
		if (pending.groupIndex < transaction.drawInfos.size()) {
			transaction.drawInfos[pending.groupIndex].skinnedAssemblyPlacementIndices.push_back(placementIndex);
		}
		if (pending.groupIndex < transaction.removalPayloads.size()) {
			transaction.removalPayloads[pending.groupIndex].skinnedAssemblyPlacementIndices.push_back(placementIndex);
		}
		activeEntries.push_back({ placementIndex, pending.placement.generation });
	}
	m_skinnedAssemblyPlacements->ReplaceData(m_skinnedAssemblyPlacementCPU);
	m_activeSkinnedAssemblyPlacements->AppendActiveEntries(activeEntries);
	m_activeSkinnedAssemblyPlacements->SetLiveSize(m_activeSkinnedAssemblyPlacements->LiveSize() + activeEntries.size());
	spdlog::info("Skinned assembly placements: published={} total={} activeEntries={}.",
		activeEntries.size(), m_skinnedAssemblyPlacementCPU.size(), m_activeSkinnedAssemblyPlacements->Size());
}

std::uint32_t ObjectManager::AllocateSkinnedAssemblyPlacement(SkinnedAssemblyPlacementGPU placement) {
	while (!m_freeSkinnedAssemblyPlacementIndices.empty()) {
		const auto placementIndex = m_freeSkinnedAssemblyPlacementIndices.back();
		m_freeSkinnedAssemblyPlacementIndices.pop_back();
		if (placementIndex >= m_skinnedAssemblyPlacementCPU.size() ||
			placementIndex >= m_skinnedAssemblyPlacementFree.size() ||
			m_skinnedAssemblyPlacementFree[placementIndex] == 0u) {
			continue;
		}

		placement.generation = m_skinnedAssemblyPlacementCPU[placementIndex].generation;
		if (placement.generation == 0u) {
			placement.generation = 1u;
		}
		m_skinnedAssemblyPlacementCPU[placementIndex] = placement;
		m_skinnedAssemblyPlacementFree[placementIndex] = 0u;
		return placementIndex;
	}

	placement.generation = 1u;
	const auto placementIndex = static_cast<std::uint32_t>(m_skinnedAssemblyPlacementCPU.size());
	m_skinnedAssemblyPlacementCPU.push_back(placement);
	m_skinnedAssemblyPlacementFree.push_back(0u);
	return placementIndex;
}

void ObjectManager::FreeSkinnedAssemblyPlacement(std::uint32_t placementIndex) {
	if (placementIndex >= m_skinnedAssemblyPlacementCPU.size()) {
		return;
	}
	if (m_skinnedAssemblyPlacementFree.size() < m_skinnedAssemblyPlacementCPU.size()) {
		m_skinnedAssemblyPlacementFree.resize(m_skinnedAssemblyPlacementCPU.size(), 0u);
	}
	if (m_skinnedAssemblyPlacementFree[placementIndex] != 0u) {
		return;
	}

	auto& placement = m_skinnedAssemblyPlacementCPU[placementIndex];
	++placement.generation;
	if (placement.generation == 0u) {
		++placement.generation;
	}
	m_skinnedAssemblyPlacementFree[placementIndex] = 1u;
	m_freeSkinnedAssemblyPlacementIndices.push_back(placementIndex);
}

ObjectManager::StaticImportPublishResult ObjectManager::PublishStaticImportTransaction(MaterializedStaticImportTransaction transaction) {
	ZoneScopedN("ObjectManager::PublishStaticImportTransaction");
	ZoneValue(static_cast<int64_t>(transaction.reservation.drawRecords));
	StaticImportPublishResult result;
	result.transactionID = transaction.reservation.id;
	result.drawRecords = transaction.reservation.drawRecords;
	result.preparedBytes = transaction.reservation.preparedBytes;
	TracyPlot("ObjectManager.StaticImportTransaction.PublishInputGroups", static_cast<int64_t>(transaction.reservation.groupCount));
	TracyPlot("ObjectManager.StaticImportTransaction.PublishInputDrawRecords", static_cast<int64_t>(transaction.reservation.drawRecords));
	TracyPlot("ObjectManager.StaticImportTransaction.PublishInputBytes", static_cast<int64_t>(transaction.reservation.preparedBytes));

	const auto publishBegin = std::chrono::steady_clock::now();
	// Placement indices are part of the draw-record contract. Publish and patch
	// them before staging draw records so every assembly component shares the
	// fitted assembly-wide coarse culling sphere.
	PublishSkinnedAssemblyPlacements(transaction);
	{
		ZoneScopedN("ObjectManager::PublishStaticImportTransaction::StageUploadRows");
		if (!transaction.normalRows.empty() && !transaction.reservation.normalMatrixRanges.empty()) {
			ZoneScopedN("ObjectManager::PublishStaticImportTransaction::StageUploadRows::NormalMatrices");
			if (const auto& range = transaction.reservation.normalMatrixRanges.front(); range.IsValid()) {
				ZoneValue(static_cast<int64_t>(transaction.normalRows.size()));
				m_normalMatrixBuffer->StageWriteRange(transaction.normalRows.data(), transaction.normalRows.size() * sizeof(DirectX::XMFLOAT4X4), range.offset);
			}
		}
		if (!transaction.perObjectRows.empty() && !transaction.reservation.perObjectRanges.empty()) {
			ZoneScopedN("ObjectManager::PublishStaticImportTransaction::StageUploadRows::PerObject");
			if (const auto& range = transaction.reservation.perObjectRanges.front(); range.IsValid()) {
				ZoneValue(static_cast<int64_t>(transaction.perObjectRows.size()));
				m_perObjectBuffers->StageWriteRange(transaction.perObjectRows.data(), transaction.perObjectRows.size() * sizeof(PerObjectCB), range.offset);
			}
		}
		if (!transaction.perObjectRows.empty() && !transaction.reservation.instanceTransformRanges.empty()) {
			ZoneScopedN("ObjectManager::PublishStaticImportTransaction::StageUploadRows::InstanceTransforms");
			if (const auto& range = transaction.reservation.instanceTransformRanges.front(); range.IsValid()) {
				ZoneValue(static_cast<int64_t>(transaction.perObjectRows.size()));
				m_perInstanceTransformBuffers->StageWriteRange(transaction.perObjectRows.data(), transaction.perObjectRows.size() * sizeof(PerInstanceTransformCB), range.offset);
			}
		}
		if (!transaction.drawRecordRows.empty() && !transaction.reservation.instanceDrawRecordRanges.empty()) {
			ZoneScopedN("ObjectManager::PublishStaticImportTransaction::StageUploadRows::DrawRecords");
			if (const auto& range = transaction.reservation.instanceDrawRecordRanges.front(); range.IsValid()) {
				ZoneValue(static_cast<int64_t>(transaction.drawRecordRows.size()));
				m_instanceDrawRecordBuffers->StageWriteRange(transaction.drawRecordRows.data(), transaction.drawRecordRows.size() * sizeof(InstanceDrawRecordCB), range.offset);
			}
		}
	}

	AssignStaticImportTransactionGenerations(transaction);

	if (transaction.reservation.visibilityDirtyStart < transaction.reservation.visibilityDirtyEnd) {
		ZoneScopedN("ObjectManager::PublishStaticImportTransaction::StageVisibilityGenerations");
		const auto previousSidecarRows = m_drawRecordVisibilityGenerationSidecar
			? m_drawRecordVisibilityGenerationSidecar->Data().size()
			: 0u;
		ZoneValue(static_cast<int64_t>(transaction.reservation.visibilityDirtyEnd - transaction.reservation.visibilityDirtyStart));
		if (transaction.reservation.visibilityDirtyEnd > previousSidecarRows) {
			ZoneScopedN("ObjectManager::PublishStaticImportTransaction::StageVisibilityGenerations::FullAfterGrow");
			m_drawRecordVisibilityGenerationSidecar->EnsureSize(transaction.reservation.visibilityDirtyEnd, 0u);
			m_drawRecordVisibilityGenerationSidecar->StageRange(
				0u,
				std::span<const std::uint32_t>(
					m_drawRecordVisibilityGenerations.data(),
					m_drawRecordVisibilityGenerations.size()));
		} else {
			ZoneScopedN("ObjectManager::PublishStaticImportTransaction::StageVisibilityGenerations::DirtyRange");
			const auto dirtyCount = transaction.reservation.visibilityDirtyEnd - transaction.reservation.visibilityDirtyStart;
			m_drawRecordVisibilityGenerationSidecar->StageRange(
				transaction.reservation.visibilityDirtyStart,
				std::span<const std::uint32_t>(
					m_drawRecordVisibilityGenerations.data() + transaction.reservation.visibilityDirtyStart,
					dirtyCount));
		}
	}

	{
		ZoneScopedN("ObjectManager::PublishStaticImportTransaction::ActiveDrawSetInserts");
		TracyPlot("ObjectManager.StaticImportTransaction.ActiveWorkloads", static_cast<int64_t>(transaction.activeDrawSetInserts.size()));
		for (const auto& [workloadKey, entries] : transaction.activeDrawSetInserts) {
			if (entries.empty()) {
				continue;
			}
			ZoneScopedN("ObjectManager::PublishStaticImportTransaction::ActiveDrawSetInserts::AppendWorkload");
			ZoneValue(static_cast<int64_t>(entries.size()));
			auto buffer = EnsureActiveDrawSetIndices(workloadKey, entries.size());
			buffer->AppendActiveEntries(entries);
			buffer->SetLiveSize(buffer->LiveSize() + entries.size());
			result.activeDrawSetSpans[workloadKey] = buffer->Size();
		}
	}

	{
		ZoneScopedN("ObjectManager::PublishStaticImportTransaction::FinalizeResult");
		result.groupsImported = 0;
		for (const auto& drawInfo : transaction.drawInfos) {
			if (!drawInfo.instanceDrawRecordIndices.empty()) {
				++result.groupsImported;
			}
		}
		result.drawInfos = std::move(transaction.drawInfos);
		result.removalPayloads = std::move(transaction.removalPayloads);
	}

	++m_stats.staticDirectBulkAddCalls;
	m_stats.staticDirectGroupsSubmitted += transaction.reservation.groupCount;
	m_stats.staticDirectGroupsImported += result.groupsImported;
	m_stats.staticDirectTransformRows += transaction.perObjectRows.size();
	m_stats.staticDirectDrawRecords += transaction.drawRecordRows.size();
	m_stats.staticDirectPacketPublishUs += static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - publishBegin).count());
	m_stats.staticDirectImportUs += static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - publishBegin).count());
	m_stats.bulkReservedPerObjectBytes += transaction.perObjectRows.size() * sizeof(PerObjectCB);
	m_stats.bulkReservedInstanceTransformBytes += transaction.perObjectRows.size() * sizeof(PerInstanceTransformCB);
	m_stats.bulkReservedNormalMatrixRows += transaction.normalRows.size();
	m_stats.bulkReservedDrawRecordBytes += transaction.drawRecordRows.size() * sizeof(InstanceDrawRecordCB);
	m_stats.instanceDrawRecordsAllocated += transaction.drawRecordRows.size();
	++m_stats.bulkReserveCalls;

	TracyPlot("ObjectManager.StaticImportTransaction.PublishedGroups", static_cast<int64_t>(result.groupsImported));
	TracyPlot("ObjectManager.StaticImportTransaction.PublishedDrawRecords", static_cast<int64_t>(result.drawRecords));
	return result;
}

ObjectManager::StaticImportBulkPublishResult ObjectManager::PublishStaticImportTransactionsBulk(std::span<MaterializedStaticImportTransaction*> transactions) {
	ZoneScopedN("ObjectManager::PublishStaticImportTransactionsBulk");
	StaticImportBulkPublishResult result;
	if (transactions.empty()) {
		return result;
	}
	ZoneValue(static_cast<int64_t>(transactions.size()));

	std::uint64_t inputGroups = 0;
	std::uint64_t transformRows = 0;
	std::uint64_t drawRecordRows = 0;
	std::unordered_map<DrawWorkloadKey, std::vector<SortedUnsignedIntBuffer::ActiveDrawSetEntry>, DrawWorkloadKey::Hasher> activeDrawSetInserts;

	for (const auto* transactionPtr : transactions) {
		assert(transactionPtr);
		const auto& transaction = *transactionPtr;
		inputGroups += transaction.reservation.groupCount;
		result.drawRecords += transaction.reservation.drawRecords;
		result.preparedBytes += transaction.reservation.preparedBytes;
		transformRows += transaction.perObjectRows.size();
		drawRecordRows += transaction.drawRecordRows.size();
	}
	result.transactionID = transactions.front()->reservation.id;
	TracyPlot("ObjectManager.StaticImportTransaction.BulkInputTransactions", static_cast<int64_t>(transactions.size()));
	TracyPlot("ObjectManager.StaticImportTransaction.BulkInputGroups", static_cast<int64_t>(inputGroups));
	TracyPlot("ObjectManager.StaticImportTransaction.BulkInputDrawRecords", static_cast<int64_t>(result.drawRecords));
	TracyPlot("ObjectManager.StaticImportTransaction.BulkInputBytes", static_cast<int64_t>(result.preparedBytes));

	const auto publishBegin = std::chrono::steady_clock::now();
	for (auto* transactionPtr : transactions) {
		assert(transactionPtr);
		PublishSkinnedAssemblyPlacements(*transactionPtr);
	}
	const auto firstValidRange = [](const std::vector<DynamicBuffer::PagedAllocation>& ranges) -> const DynamicBuffer::PagedAllocation* {
		for (const auto& range : ranges) {
			if (range.IsValid()) {
				return &range;
			}
		}
		return nullptr;
	};
	{
		ZoneScopedN("ObjectManager::PublishStaticImportTransactionsBulk::StageUploadRows");
		for (const auto* transactionPtr : transactions) {
			assert(transactionPtr);
			const auto& transaction = *transactionPtr;
			if (!transaction.normalRows.empty()) {
				if (const auto* range = firstValidRange(transaction.reservation.normalMatrixRanges)) {
					m_normalMatrixBuffer->StageWriteRange(
						transaction.normalRows.data(),
						transaction.normalRows.size() * sizeof(DirectX::XMFLOAT4X4),
						range->offset);
				}
			}
			if (!transaction.perObjectRows.empty()) {
				if (const auto* range = firstValidRange(transaction.reservation.perObjectRanges)) {
					m_perObjectBuffers->StageWriteRange(
						transaction.perObjectRows.data(),
						transaction.perObjectRows.size() * sizeof(PerObjectCB),
						range->offset);
				}
				if (const auto* range = firstValidRange(transaction.reservation.instanceTransformRanges)) {
					m_perInstanceTransformBuffers->StageWriteRange(
						transaction.perObjectRows.data(),
						transaction.perObjectRows.size() * sizeof(PerInstanceTransformCB),
						range->offset);
				}
			}
			if (!transaction.drawRecordRows.empty()) {
				if (const auto* range = firstValidRange(transaction.reservation.instanceDrawRecordRanges)) {
					m_instanceDrawRecordBuffers->StageWriteRange(
						transaction.drawRecordRows.data(),
						transaction.drawRecordRows.size() * sizeof(InstanceDrawRecordCB),
						range->offset);
				}
			}
		}
	}

	AssignStaticImportTransactionGenerations(transactions);

	std::size_t visibilityDirtyStart = std::numeric_limits<std::size_t>::max();
	std::size_t visibilityDirtyEnd = 0;
	for (const auto* transactionPtr : transactions) {
		assert(transactionPtr);
		const auto& transaction = *transactionPtr;
		visibilityDirtyStart = (std::min)(visibilityDirtyStart, transaction.reservation.visibilityDirtyStart);
		visibilityDirtyEnd = (std::max)(visibilityDirtyEnd, transaction.reservation.visibilityDirtyEnd);
	}

	if (visibilityDirtyStart < visibilityDirtyEnd) {
		ZoneScopedN("ObjectManager::PublishStaticImportTransactionsBulk::StageVisibilityGenerations");
		const auto previousSidecarRows = m_drawRecordVisibilityGenerationSidecar
			? m_drawRecordVisibilityGenerationSidecar->Data().size()
			: 0u;
		if (visibilityDirtyEnd > previousSidecarRows) {
			m_drawRecordVisibilityGenerationSidecar->EnsureSize(visibilityDirtyEnd, 0u);
		}
		const auto dirtyCount = visibilityDirtyEnd - visibilityDirtyStart;
		TracyPlot("ObjectManager.StaticImportTransaction.BulkVisibilityDirtyRows", static_cast<int64_t>(dirtyCount));
		m_drawRecordVisibilityGenerationSidecar->StageRange(
			visibilityDirtyStart,
			std::span<const std::uint32_t>(
				m_drawRecordVisibilityGenerations.data() + visibilityDirtyStart,
				dirtyCount));
	}

	for (auto* transactionPtr : transactions) {
		assert(transactionPtr);
		auto& transaction = *transactionPtr;
		for (auto& [workloadKey, entries] : transaction.activeDrawSetInserts) {
			if (entries.empty()) {
				continue;
			}
			auto& dst = activeDrawSetInserts[workloadKey];
			dst.reserve(dst.size() + entries.size());
			dst.insert(
				dst.end(),
				std::make_move_iterator(entries.begin()),
				std::make_move_iterator(entries.end()));
		}
	}
	{
		ZoneScopedN("ObjectManager::PublishStaticImportTransactionsBulk::ActiveDrawSetInserts");
		TracyPlot("ObjectManager.StaticImportTransaction.BulkActiveWorkloads", static_cast<int64_t>(activeDrawSetInserts.size()));
		for (const auto& [workloadKey, entries] : activeDrawSetInserts) {
			if (entries.empty()) {
				continue;
			}
			const auto insertBegin = std::chrono::steady_clock::now();
			auto buffer = EnsureActiveDrawSetIndices(workloadKey, entries.size());
			buffer->AppendActiveEntries(entries);
			buffer->SetLiveSize(buffer->LiveSize() + entries.size());
			result.activeDrawSetSpans[workloadKey] = buffer->Size();
			m_stats.activeDrawSetInsertCalls += 1;
			m_stats.activeDrawSetInsertIndices += entries.size();
			m_stats.activeDrawSetInsertUs += static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - insertBegin).count());
		}
	}

	{
		ZoneScopedN("ObjectManager::PublishStaticImportTransactionsBulk::FinalizeResult");
		result.transactions.reserve(transactions.size());
		for (auto* transactionPtr : transactions) {
			assert(transactionPtr);
			auto& transaction = *transactionPtr;
			StaticImportTransactionPublishRecord record;
			record.transactionID = transaction.reservation.id;
			record.drawRecords = transaction.reservation.drawRecords;
			record.preparedBytes = transaction.reservation.preparedBytes;
			for (const auto& drawInfo : transaction.drawInfos) {
				if (!drawInfo.instanceDrawRecordIndices.empty()) {
					++record.groupsImported;
				}
			}
			record.removalPayloads = std::move(transaction.removalPayloads);
			result.groupsImported += record.groupsImported;
			result.transactions.push_back(std::move(record));
		}
		TracyPlot("ObjectManager.StaticImportTransaction.BulkTransactionRecords", static_cast<int64_t>(result.transactions.size()));
	}

	const auto publishUs = static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - publishBegin).count());
	++m_stats.staticDirectBulkAddCalls;
	m_stats.staticDirectGroupsSubmitted += inputGroups;
	m_stats.staticDirectGroupsImported += result.groupsImported;
	m_stats.staticDirectTransformRows += transformRows;
	m_stats.staticDirectDrawRecords += drawRecordRows;
	m_stats.staticDirectPacketPublishUs += publishUs;
	m_stats.staticDirectImportUs += publishUs;
	m_stats.bulkReservedPerObjectBytes += transformRows * sizeof(PerObjectCB);
	m_stats.bulkReservedInstanceTransformBytes += transformRows * sizeof(PerInstanceTransformCB);
	m_stats.bulkReservedNormalMatrixRows += transformRows;
	m_stats.bulkReservedDrawRecordBytes += drawRecordRows * sizeof(InstanceDrawRecordCB);
	m_stats.instanceDrawRecordsAllocated += drawRecordRows;
	m_stats.bulkReserveCalls += transactions.size();

	TracyPlot("ObjectManager.StaticImportTransaction.BulkPublishedGroups", static_cast<int64_t>(result.groupsImported));
	TracyPlot("ObjectManager.StaticImportTransaction.BulkPublishedDrawRecords", static_cast<int64_t>(result.drawRecords));
	return result;
}

void ObjectManager::CancelStaticImportTransaction(StaticImportReservation reservation, std::uint64_t retireFrame) {
	ZoneScopedN("ObjectManager::CancelStaticImportTransaction");
	const auto frame = retireFrame == 0 ? MakeDeferredRetireFrame() : retireFrame;
	std::vector<DeferredBufferRangeRetire> retires;
	const auto addRanges = [&retires, frame](const std::shared_ptr<DynamicBuffer>& buffer, const std::vector<DynamicBuffer::PagedAllocation>& ranges) {
		if (!buffer) {
			return;
		}
		for (const auto& range : ranges) {
			if (!range.IsValid()) {
				continue;
			}
			retires.push_back(DeferredBufferRangeRetire{
				.buffer = buffer,
				.offset = range.offset,
				.size = range.allocationSize,
				.retireFrame = frame
			});
		}
	};
	addRanges(reservation.perObjectBuffer, reservation.perObjectRanges);
	addRanges(reservation.instanceTransformBuffer, reservation.instanceTransformRanges);
	addRanges(reservation.normalMatrixBuffer, reservation.normalMatrixRanges);
	addRanges(reservation.instanceDrawRecordBuffer, reservation.instanceDrawRecordRanges);
	EnqueueDeferredBufferRangeRetires(std::move(retires));
	TracyPlot("ObjectManager.StaticImportTransaction.CanceledGroups", static_cast<int64_t>(reservation.groupCount));
}

ObjectManager::StaticImportPacket ObjectManager::BuildStaticImportPacket(StaticImportPacketPlan plan) {
	StaticImportPacket packet;
	if (plan.prepared.groups.empty()) {
		return packet;
	}

	const auto packetBuildBegin = std::chrono::steady_clock::now();
	packet.groupCount = plan.prepared.groups.size();
	packet.transformRows = plan.prepared.transformRows;
	packet.drawRecords = plan.prepared.drawRecords;
	packet.preparedBytes = plan.prepared.preparedBytes;
	packet.prepareUs = plan.prepared.prepareUs;
	packet.transformBuildUs = plan.prepared.transformBuildUs;
	packet.workloadBuildUs = plan.prepared.workloadBuildUs;
	packet.drawInfos.resize(plan.prepared.groups.size());
	packet.transformRanges.resize(plan.prepared.groups.size());

	const auto scopeBuildBegin = std::chrono::steady_clock::now();
	std::unordered_map<std::uint64_t, size_t> scopeIndices;
	packet.scopes.reserve(plan.prepared.groups.size());
	for (size_t groupIndex = 0; groupIndex < plan.prepared.groups.size(); ++groupIndex) {
		const auto& group = plan.prepared.groups[groupIndex];
		const std::uint64_t scopeID = group.allocationScopeID != 0 ? group.allocationScopeID : group.stableGroupID;
		auto [scopeIt, inserted] = scopeIndices.emplace(scopeID, packet.scopes.size());
		if (inserted) {
			StaticImportPacket::Scope scope;
			scope.id = scopeID;
			packet.scopes.push_back(std::move(scope));
		}
		auto& scope = packet.scopes[scopeIt->second];
		scope.groupIndices.push_back(groupIndex);

		StaticImportPacket::GroupTransformRange range;
		range.first = scope.perObjectCBs.size();
		range.count = group.perObjectCBs.size();
		packet.transformRanges[groupIndex] = range;
		scope.perObjectCBs.insert(scope.perObjectCBs.end(), group.perObjectCBs.begin(), group.perObjectCBs.end());
		scope.normalMatrices.insert(scope.normalMatrices.end(), group.normalMatrices.begin(), group.normalMatrices.end());
	}
	packet.scopeBuildUs = static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - scopeBuildBegin).count());

	const auto drawRecordBuildBegin = std::chrono::steady_clock::now();
	for (size_t groupIndex = 0; groupIndex < plan.prepared.groups.size(); ++groupIndex) {
		const auto& group = plan.prepared.groups[groupIndex];
		auto& drawInfo = packet.drawInfos[groupIndex];
		const std::uint64_t scopeID = group.allocationScopeID != 0 ? group.allocationScopeID : group.stableGroupID;
		auto scopeIndexIt = scopeIndices.find(scopeID);
		if (scopeIndexIt == scopeIndices.end() || groupIndex >= packet.transformRanges.size()) {
			continue;
		}
		auto& scope = packet.scopes[scopeIndexIt->second];
		const auto range = packet.transformRanges[groupIndex];
		if (range.count == 0 || group.meshTemplates.empty()) {
			continue;
		}

		drawInfo.perMeshInstanceBufferIndices.reserve(group.meshTemplates.size());
		drawInfo.instanceDrawRecordIndices.reserve(range.count * group.meshTemplates.size());
		drawInfo.instanceDrawRecordViews.reserve(range.count * group.meshTemplates.size());
		drawInfo.drawInfo.indices.reserve(range.count * group.meshTemplates.size());
		drawInfo.drawInfo.views.reserve(range.count * group.meshTemplates.size());
		drawInfo.drawInfo.drawWorkloadKeysPerDraw.reserve(range.count * group.meshTemplates.size());
		for (const auto& meshTemplate : group.meshTemplates) {
			drawInfo.perMeshInstanceBufferIndices.push_back(meshTemplate.meshTemplateIndex);
		}

		for (size_t transformIndex = 0; transformIndex < range.count; ++transformIndex) {
			const auto transformOrdinal = range.first + transformIndex;
			for (size_t meshTemplateIndex = 0; meshTemplateIndex < group.meshTemplates.size(); ++meshTemplateIndex) {
				const auto& meshTemplate = group.meshTemplates[meshTemplateIndex];
				StaticImportPacket::PatchableDrawRecord record;
				record.groupIndex = groupIndex;
				record.scopeTransformOrdinal = transformOrdinal;
				record.meshTemplateIndex = meshTemplate.meshTemplateIndex;
				record.clodOffsetIndex = meshTemplate.clodOffsetIndex;
				record.skinnedAssemblyTypeSlot = meshTemplate.skinnedAssemblyTypeSlot;
				record.skinnedAssemblyBounds = meshTemplate.skinnedAssemblyBounds;
				record.skinnedBoundsScale = meshTemplate.skinnedBoundsScale;
				if (meshTemplateIndex < group.workloadKeysByMeshTemplate.size()) {
					record.workloadKeys = group.workloadKeysByMeshTemplate[meshTemplateIndex];
				}
				scope.drawRecords.push_back(std::move(record));
			}
		}
	}
	packet.drawRecordBuildUs = static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - drawRecordBuildBegin).count());
	packet.packetBuildUs = static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - packetBuildBegin).count());
	return packet;
}

std::vector<Components::ObjectDrawInfo> ObjectManager::PublishStaticImportPacket(StaticImportPacket packet) {
	ZoneScopedN("ObjectManager::PublishStaticImportPacket");
	std::vector<Components::ObjectDrawInfo> drawInfos;
	if (packet.drawInfos.empty()) {
		return drawInfos;
	}
	TracyPlot("ObjectManager.StaticImportPacket.Groups", static_cast<int64_t>(packet.groupCount));
	TracyPlot("ObjectManager.StaticImportPacket.TransformRows", static_cast<int64_t>(packet.transformRows));
	TracyPlot("ObjectManager.StaticImportPacket.DrawRecords", static_cast<int64_t>(packet.drawRecords));
	TracyPlot("ObjectManager.StaticImportPacket.PreparedBytes", static_cast<int64_t>(packet.preparedBytes));
	TracyPlot("ObjectManager.StaticImportPacket.Scopes", static_cast<int64_t>(packet.scopes.size()));

	const auto importBegin = std::chrono::steady_clock::now();
	const auto resizeBegin = std::chrono::steady_clock::now();
	{
		ZoneScopedN("ObjectManager::PublishStaticImportPacket::PublishResizes");
		(void)PublishReadyDeferredBackingResizes(false);
	}
	m_stats.staticDirectResizePublishUs += static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - resizeBegin).count());

	{
		ZoneScopedN("ObjectManager::PublishStaticImportPacket::PublishBuildStats");
		++m_stats.staticDirectBulkAddCalls;
		m_stats.staticDirectGroupsSubmitted += packet.groupCount;
		m_stats.staticDirectTransformBuildUs += packet.transformBuildUs;
		m_stats.staticDirectWorkloadBuildUs += packet.workloadBuildUs;
		m_stats.staticDirectScopeBuildUs += packet.scopeBuildUs;
		m_stats.staticDirectDrawRecordBuildUs += packet.drawRecordBuildUs;
		m_stats.staticDirectPacketBuildUs += packet.packetBuildUs;

		size_t totalTransformRows = 0;
		for (const auto& scope : packet.scopes) {
			totalTransformRows += scope.perObjectCBs.size();
		}

		m_stats.staticDirectTransformRows += totalTransformRows;
		m_stats.perObjectRowsAllocated += totalTransformRows;
		m_stats.perInstanceTransformRowsAllocated += totalTransformRows;
		m_stats.normalMatrixRowsAllocated += totalTransformRows;
		for (const auto& drawInfo : packet.drawInfos) {
			m_stats.meshTemplateRowsReferenced += drawInfo.perMeshInstanceBufferIndices.size();
		}
	}

	{
		ZoneScopedN("ObjectManager::PublishStaticImportPacket::MoveDrawInfos");
		drawInfos = std::move(packet.drawInfos);
	}
	std::unordered_map<DrawWorkloadKey, std::vector<SortedUnsignedIntBuffer::ActiveDrawSetEntry>, DrawWorkloadKey::Hasher> activeDrawSetInserts;
	std::vector<StaticImportPacket::Scope*> scopeByGroup(drawInfos.size(), nullptr);
	for (auto& scope : packet.scopes) {
		for (const auto groupIndex : scope.groupIndices) {
			if (groupIndex < scopeByGroup.size()) {
				scopeByGroup[groupIndex] = &scope;
			}
		}
	}

	std::vector<size_t> transformCounts(drawInfos.size(), 0);
	std::vector<size_t> transformFlatFirstByGroup(drawInfos.size(), 0);
	std::vector<PerObjectCB> packetPerObjectRows;
	std::vector<DirectX::XMFLOAT4X4> packetNormalRows;
	packetPerObjectRows.reserve(static_cast<size_t>(packet.transformRows));
	packetNormalRows.reserve(static_cast<size_t>(packet.transformRows));

	std::vector<DynamicBuffer::PagedAllocation> normalMatrixRanges;
	std::vector<DynamicBuffer::PagedAllocation> perObjectRanges;
	std::vector<DynamicBuffer::PagedAllocation> instanceTransformRanges;
	const auto pageUploadBegin = std::chrono::steady_clock::now();
	{
		ZoneScopedN("ObjectManager::PublishStaticImportPacket::TransformPages");
		{
			ZoneScopedN("ObjectManager::PublishStaticImportPacket::TransformPages::BuildGroupRangePayloads");
			for (size_t groupIndex = 0; groupIndex < drawInfos.size(); ++groupIndex) {
				if (groupIndex >= packet.transformRanges.size()) {
					continue;
				}
				const auto* scope = scopeByGroup[groupIndex];
				const auto range = packet.transformRanges[groupIndex];
				if (!scope ||
					range.count == 0 ||
					range.first + range.count > scope->perObjectCBs.size() ||
					range.first + range.count > scope->normalMatrices.size()) {
					continue;
				}
				transformFlatFirstByGroup[groupIndex] = packetPerObjectRows.size();
				transformCounts[groupIndex] = range.count;
				packetPerObjectRows.insert(
					packetPerObjectRows.end(),
					scope->perObjectCBs.begin() + range.first,
					scope->perObjectCBs.begin() + range.first + range.count);
				packetNormalRows.insert(
					packetNormalRows.end(),
					scope->normalMatrices.begin() + range.first,
					scope->normalMatrices.begin() + range.first + range.count);
			}
		}

		{
			ZoneScopedN("ObjectManager::PublishStaticImportPacket::TransformPages::AllocateNormalRangesBatch");
			normalMatrixRanges = m_normalMatrixBuffer->AllocateRangesBatch(
				transformCounts,
				sizeof(DirectX::XMFLOAT4X4));
		}
		{
			ZoneScopedN("ObjectManager::PublishStaticImportPacket::TransformPages::AllocatePerObjectRangesBatch");
			perObjectRanges = m_perObjectBuffers->AllocateRangesBatch(
				transformCounts,
				sizeof(PerObjectCB));
		}
		{
			ZoneScopedN("ObjectManager::PublishStaticImportPacket::TransformPages::AllocateInstanceTransformRangesBatch");
			instanceTransformRanges = m_perInstanceTransformBuffers->AllocateRangesBatch(
				transformCounts,
				sizeof(PerInstanceTransformCB));
		}

		{
			ZoneScopedN("ObjectManager::PublishStaticImportPacket::TransformPages::PatchNormalIndices");
			const auto normalPatchBegin = std::chrono::steady_clock::now();
			for (size_t groupIndex = 0; groupIndex < drawInfos.size(); ++groupIndex) {
				if (groupIndex >= normalMatrixRanges.size() || !normalMatrixRanges[groupIndex].IsValid()) {
					continue;
				}
				const auto rowCount = transformCounts[groupIndex];
				const auto flatFirst = transformFlatFirstByGroup[groupIndex];
				for (size_t i = 0; i < rowCount; ++i) {
					packetPerObjectRows[flatFirst + i].normalMatrixBufferIndex = static_cast<uint32_t>(
						(normalMatrixRanges[groupIndex].offset + i * sizeof(DirectX::XMFLOAT4X4)) / sizeof(DirectX::XMFLOAT4X4));
				}
			}
			m_stats.staticDirectNormalPatchUs += static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - normalPatchBegin).count());
		}

		const auto firstValidRange = [](const std::vector<DynamicBuffer::PagedAllocation>& ranges) -> const DynamicBuffer::PagedAllocation* {
			for (const auto& range : ranges) {
				if (range.IsValid()) {
					return &range;
				}
			}
			return nullptr;
		};
		if (const auto* firstNormalRange = firstValidRange(normalMatrixRanges)) {
			ZoneScopedN("ObjectManager::PublishStaticImportPacket::TransformPages::StageNormalRange");
			m_normalMatrixBuffer->StageWriteRange(
				packetNormalRows.data(),
				packetNormalRows.size() * sizeof(DirectX::XMFLOAT4X4),
				firstNormalRange->offset);
		}
		if (const auto* firstPerObjectRange = firstValidRange(perObjectRanges)) {
			ZoneScopedN("ObjectManager::PublishStaticImportPacket::TransformPages::StagePerObjectRange");
			m_perObjectBuffers->StageWriteRange(
				packetPerObjectRows.data(),
				packetPerObjectRows.size() * sizeof(PerObjectCB),
				firstPerObjectRange->offset);
		}
		if (const auto* firstInstanceTransformRange = firstValidRange(instanceTransformRanges)) {
			ZoneScopedN("ObjectManager::PublishStaticImportPacket::TransformPages::StageInstanceTransformRange");
			m_perInstanceTransformBuffers->StageWriteRange(
				packetPerObjectRows.data(),
				packetPerObjectRows.size() * sizeof(PerInstanceTransformCB),
				firstInstanceTransformRange->offset);
		}

		{
			ZoneScopedN("ObjectManager::PublishStaticImportPacket::TransformPages::FinalizeGroupRanges");
			for (size_t groupIndex = 0; groupIndex < drawInfos.size(); ++groupIndex) {
				if (groupIndex >= perObjectRanges.size() ||
					groupIndex >= instanceTransformRanges.size() ||
					groupIndex >= normalMatrixRanges.size()) {
					continue;
				}
				auto& drawInfo = drawInfos[groupIndex];
				drawInfo.perObjectCBRange = ToBufferRange(perObjectRanges[groupIndex]);
				drawInfo.perInstanceTransformRange = ToBufferRange(instanceTransformRanges[groupIndex]);
				drawInfo.normalMatrixRange = ToBufferRange(normalMatrixRanges[groupIndex]);
				drawInfo.perObjectCBIndex = static_cast<uint32_t>(drawInfo.perObjectCBRange.offset / sizeof(PerObjectCB));
				drawInfo.normalMatrixIndex = static_cast<uint32_t>(drawInfo.normalMatrixRange.offset / sizeof(DirectX::XMFLOAT4X4));
			}
		}

		m_stats.bulkReservedPerObjectBytes += packetPerObjectRows.size() * sizeof(PerObjectCB);
		m_stats.bulkReservedInstanceTransformBytes += packetPerObjectRows.size() * sizeof(PerInstanceTransformCB);
		m_stats.bulkReservedNormalMatrixRows += packetNormalRows.size();
	}
	m_stats.staticDirectPageUploadUs += static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - pageUploadBegin).count());

	// Publish exactly one skinned-assembly placement for each transform/type pair.
	// Material/subset draw records deliberately do not participate in this list.
	{
		std::vector<SortedUnsignedIntBuffer::ActiveDrawSetEntry> activePlacements;
		for (std::size_t groupIndex = 0; groupIndex < drawInfos.size() && groupIndex < packet.transformRanges.size(); ++groupIndex) {
			struct TypeBounds {
				std::uint32_t slot;
				std::vector<BoundingSphere> components;
				BoundingSphere bounds;
				float scale;
			};
			std::vector<TypeBounds> types;
			for (const auto& scope : packet.scopes) {
				for (const auto& record : scope.drawRecords) {
					if (record.groupIndex != groupIndex || record.skinnedAssemblyTypeSlot == 0xFFFFFFFFu) continue;
					auto found = std::ranges::find(types, record.skinnedAssemblyTypeSlot, &TypeBounds::slot);
					if (found == types.end()) {
						types.push_back({ record.skinnedAssemblyTypeSlot,
							{ record.skinnedAssemblyBounds }, record.skinnedAssemblyBounds, record.skinnedBoundsScale });
					} else {
						found->components.push_back(record.skinnedAssemblyBounds);
						found->scale = (std::max)(found->scale, record.skinnedBoundsScale);
					}
				}
			}
			for (auto& type : types) type.bounds = FitBoundingSpheres(type.components);
			if (types.empty() || groupIndex >= instanceTransformRanges.size() || !instanceTransformRanges[groupIndex].IsValid()) continue;
			const auto transformCount = packet.transformRanges[groupIndex].count;
			const auto transformBase = static_cast<std::uint32_t>(instanceTransformRanges[groupIndex].offset / sizeof(PerInstanceTransformCB));
			std::uint64_t stableGroupID = groupIndex;
			for (const auto& scope : packet.scopes) {
				if (std::ranges::find(scope.groupIndices, groupIndex) != scope.groupIndices.end()) {
					stableGroupID = scope.id;
					break;
				}
			}
			for (const auto& type : types) {
				for (std::uint32_t local = 0; local < transformCount; ++local) {
					SkinnedAssemblyPlacementGPU placement{};
					placement.instanceTransformIndex = transformBase + local;
					placement.skinningTypeSlot = type.slot;
					const std::uint64_t stable = stableGroupID;
					placement.stableSceneID = static_cast<std::uint32_t>((stable ^ (stable >> 32u)) * 747796405u + local * 2891336453u + type.slot * 277803737u);
					placement.generation = 1u;
					placement.localBoundingSphere = type.bounds.sphere;
					placement.boundsScale = type.scale;
					const auto placementIndex = AllocateSkinnedAssemblyPlacement(placement);
					placement = m_skinnedAssemblyPlacementCPU[placementIndex];
					drawInfos[groupIndex].skinnedAssemblyPlacementIndices.push_back(placementIndex);
					activePlacements.push_back({ placementIndex, placement.generation });
				}
			}
		}
		if (!activePlacements.empty()) {
			m_skinnedAssemblyPlacements->ReplaceData(m_skinnedAssemblyPlacementCPU);
			m_activeSkinnedAssemblyPlacements->AppendActiveEntries(activePlacements);
			m_activeSkinnedAssemblyPlacements->SetLiveSize(m_activeSkinnedAssemblyPlacements->LiveSize() + activePlacements.size());
			spdlog::info("Skinned assembly placements: published={} total={} activeEntries={}.", activePlacements.size(), m_skinnedAssemblyPlacementCPU.size(), m_activeSkinnedAssemblyPlacements->Size());
		}
	}

	const auto drawRecordUploadBegin = std::chrono::steady_clock::now();
	{
		ZoneScopedN("ObjectManager::PublishStaticImportPacket::DrawRecordPages");
		std::vector<std::vector<const StaticImportPacket::PatchableDrawRecord*>> drawRecordsByGroup(drawInfos.size());
		std::vector<size_t> drawRecordCounts(drawInfos.size(), 0);
		{
			ZoneScopedN("ObjectManager::PublishStaticImportPacket::DrawRecordPages::BuildGroupRecordLists");
			for (const auto& scope : packet.scopes) {
				for (const auto& sourceRecord : scope.drawRecords) {
					if (sourceRecord.groupIndex >= drawInfos.size() || sourceRecord.groupIndex >= packet.transformRanges.size()) {
						continue;
					}
					const auto range = packet.transformRanges[sourceRecord.groupIndex];
					if (sourceRecord.scopeTransformOrdinal < range.first ||
						sourceRecord.scopeTransformOrdinal >= range.first + range.count) {
						continue;
					}
					drawRecordsByGroup[sourceRecord.groupIndex].push_back(&sourceRecord);
				}
			}
			for (size_t groupIndex = 0; groupIndex < drawRecordsByGroup.size(); ++groupIndex) {
				drawRecordCounts[groupIndex] = drawRecordsByGroup[groupIndex].size();
			}
		}

		std::vector<DynamicBuffer::PagedAllocation> instanceDrawRecordRanges;
		{
			ZoneScopedN("ObjectManager::PublishStaticImportPacket::DrawRecordPages::AllocateDrawRecordRangesBatch");
			instanceDrawRecordRanges = m_instanceDrawRecordBuffers->AllocateRangesBatch(
				drawRecordCounts,
				sizeof(InstanceDrawRecordCB));
		}

		std::vector<InstanceDrawRecordCB> packetDrawRecords;
		packetDrawRecords.reserve(static_cast<size_t>(packet.drawRecords));
		std::size_t visibilityDirtyStart = std::numeric_limits<std::size_t>::max();
		std::size_t visibilityDirtyEnd = 0;
		bool visibilitySidecarExtended = false;
		{
			ZoneScopedN("ObjectManager::PublishStaticImportPacket::DrawRecordPages::EnsureVisibilityGenerationSidecar");
			const auto previousSidecarRows = m_drawRecordVisibilityGenerationSidecar
				? m_drawRecordVisibilityGenerationSidecar->Data().size()
				: 0u;
			const auto firstRangeIt = std::find_if(
				instanceDrawRecordRanges.begin(),
				instanceDrawRecordRanges.end(),
				[](const auto& range) { return range.IsValid(); });
			if (firstRangeIt != instanceDrawRecordRanges.end()) {
				const auto lastRangeIt = std::find_if(
					instanceDrawRecordRanges.rbegin(),
					instanceDrawRecordRanges.rend(),
					[](const auto& range) { return range.IsValid(); });
				const auto maxDrawRecordIndexExclusive = static_cast<std::size_t>(
					(lastRangeIt->offset + lastRangeIt->size) / sizeof(InstanceDrawRecordCB));
				visibilitySidecarExtended = maxDrawRecordIndexExclusive > previousSidecarRows;
				if (maxDrawRecordIndexExclusive > m_drawRecordVisibilityGenerations.size()) {
					m_drawRecordVisibilityGenerations.resize(maxDrawRecordIndexExclusive, 0u);
				}
				m_drawRecordVisibilityGenerationSidecar->EnsureSize(maxDrawRecordIndexExclusive, 0u);
			}
		}
		{
			ZoneScopedN("ObjectManager::PublishStaticImportPacket::DrawRecordPages::MaterializeAndPatchDrawInfos");
			for (size_t groupIndex = 0; groupIndex < drawRecordsByGroup.size(); ++groupIndex) {
				if (groupIndex >= instanceDrawRecordRanges.size() ||
					groupIndex >= instanceTransformRanges.size() ||
					groupIndex >= packet.transformRanges.size() ||
					!instanceDrawRecordRanges[groupIndex].IsValid() ||
					!instanceTransformRanges[groupIndex].IsValid()) {
					continue;
				}
				auto& drawInfo = drawInfos[groupIndex];
				const auto transformRange = packet.transformRanges[groupIndex];
				const auto& drawRecordRange = instanceDrawRecordRanges[groupIndex];
				drawInfo.instanceDrawRecordRange = ToBufferRange(drawRecordRange);
				for (size_t drawRecordOrdinal = 0; drawRecordOrdinal < drawRecordsByGroup[groupIndex].size(); ++drawRecordOrdinal) {
					const auto& sourceRecord = *drawRecordsByGroup[groupIndex][drawRecordOrdinal];
					const auto localTransformOrdinal = sourceRecord.scopeTransformOrdinal - transformRange.first;
					const auto instanceTransformOffset =
						instanceTransformRanges[groupIndex].offset + localTransformOrdinal * sizeof(PerInstanceTransformCB);
					const auto drawRecordOffset =
						drawRecordRange.offset + drawRecordOrdinal * sizeof(InstanceDrawRecordCB);
					const auto drawRecordIndex = static_cast<unsigned int>(drawRecordOffset / sizeof(InstanceDrawRecordCB));
					if (drawRecordIndex > 0xFFFFFFu) {
						spdlog::warn("ObjectManager::PublishStaticImportPacket: instance draw record index {} exceeds packed visible-cluster 24-bit capacity", drawRecordIndex);
					}
					m_stats.maxDrawRecordIndex = std::max<std::uint64_t>(m_stats.maxDrawRecordIndex, drawRecordIndex);
					const auto drawRecordGeneration = ActivateDrawRecordCPU(drawRecordIndex);
					visibilityDirtyStart = (std::min)(visibilityDirtyStart, static_cast<std::size_t>(drawRecordIndex));
					visibilityDirtyEnd = (std::max)(visibilityDirtyEnd, static_cast<std::size_t>(drawRecordIndex) + 1u);

					InstanceDrawRecordCB drawRecord{};
					drawRecord.meshTemplateIndex = sourceRecord.meshTemplateIndex;
					drawRecord.instanceTransformIndex = static_cast<uint32_t>(instanceTransformOffset / sizeof(PerInstanceTransformCB));
					drawRecord.clodOffsetIndex = sourceRecord.clodOffsetIndex;
					drawRecord.skinnedAssemblyPlacementIndex = 0xFFFFFFFFu;
					drawRecord.skinningTypeSlot = sourceRecord.skinnedAssemblyTypeSlot;
					if (sourceRecord.skinnedAssemblyTypeSlot != 0xFFFFFFFFu) {
						for (const auto placementIndex : drawInfo.skinnedAssemblyPlacementIndices) {
							if (placementIndex >= m_skinnedAssemblyPlacementCPU.size()) continue;
							const auto& placement = m_skinnedAssemblyPlacementCPU[placementIndex];
							if (placement.instanceTransformIndex == drawRecord.instanceTransformIndex &&
								placement.skinningTypeSlot == sourceRecord.skinnedAssemblyTypeSlot) {
								drawRecord.skinnedAssemblyPlacementIndex = placementIndex;
								break;
							}
						}
					}
					packetDrawRecords.push_back(drawRecord);

					drawInfo.drawInfo.indices.push_back(drawRecordIndex);
					drawInfo.drawInfo.drawWorkloadKeysPerDraw.push_back(sourceRecord.workloadKeys);
					drawInfo.instanceDrawRecordIndices.push_back(drawRecordIndex);
					if (!sourceRecord.workloadKeys.empty()) {
						++m_stats.staticDirectWorkloadCacheHits;
					}
					for (const auto& workloadKey : sourceRecord.workloadKeys) {
						activeDrawSetInserts[workloadKey].push_back(SortedUnsignedIntBuffer::ActiveDrawSetEntry{
							.drawRecordIndex = drawRecordIndex,
							.generation = drawRecordGeneration
						});
						AppendActiveDrawSetRemoval(drawInfo, workloadKey, drawRecordIndex);
					}
				}
			}
		}

		if (visibilityDirtyStart < visibilityDirtyEnd) {
			if (visibilitySidecarExtended) {
				ZoneScopedN("ObjectManager::PublishStaticImportPacket::DrawRecordPages::StageVisibilityGenerationFullAfterGrow");
				TracyPlot(
					"ObjectManager.StaticImportPacket.VisibilityGenerationFullRows",
					static_cast<int64_t>(m_drawRecordVisibilityGenerations.size()));
				m_drawRecordVisibilityGenerationSidecar->StageRange(
					0u,
					std::span<const std::uint32_t>(
						m_drawRecordVisibilityGenerations.data(),
						m_drawRecordVisibilityGenerations.size()));
			} else {
				ZoneScopedN("ObjectManager::PublishStaticImportPacket::DrawRecordPages::StageVisibilityGenerationRange");
				const auto dirtyCount = visibilityDirtyEnd - visibilityDirtyStart;
				TracyPlot(
					"ObjectManager.StaticImportPacket.VisibilityGenerationDirtyRows",
					static_cast<int64_t>(dirtyCount));
				m_drawRecordVisibilityGenerationSidecar->StageRange(
					visibilityDirtyStart,
					std::span<const std::uint32_t>(
						m_drawRecordVisibilityGenerations.data() + visibilityDirtyStart,
						dirtyCount));
			}
		}

		if (!packetDrawRecords.empty()) {
			ZoneScopedN("ObjectManager::PublishStaticImportPacket::DrawRecordPages::StageDrawRecordRange");
			const auto firstRangeIt = std::find_if(
				instanceDrawRecordRanges.begin(),
				instanceDrawRecordRanges.end(),
				[](const auto& range) { return range.IsValid(); });
			if (firstRangeIt != instanceDrawRecordRanges.end()) {
				m_instanceDrawRecordBuffers->StageWriteRange(
					packetDrawRecords.data(),
					packetDrawRecords.size() * sizeof(InstanceDrawRecordCB),
					firstRangeIt->offset);
			}
		}

		const auto drawRecordBytes = packetDrawRecords.size() * sizeof(InstanceDrawRecordCB);
		m_stats.bulkReservedDrawRecordBytes += drawRecordBytes;
		m_stats.instanceDrawRecordsAllocated += packetDrawRecords.size();
		m_stats.staticDirectDrawRecords += packetDrawRecords.size();
	}
	m_stats.staticDirectDrawRecordUploadUs += static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - drawRecordUploadBegin).count());

	++m_stats.bulkReserveCalls;

	{
		ZoneScopedN("ObjectManager::PublishStaticImportPacket::ActiveDrawSetInserts");
		TracyPlot("ObjectManager.StaticImportPacket.ActiveWorkloadBuckets", static_cast<int64_t>(activeDrawSetInserts.size()));
		for (const auto& [workloadKey, entries] : activeDrawSetInserts) {
			if (!entries.empty()) {
				ZoneScopedN("ObjectManager::PublishStaticImportPacket::ActiveDrawSetInserts::InsertMany");
				ZoneValue(entries.size());
				const auto insertBegin = std::chrono::steady_clock::now();
				AppendActiveDrawSetEntries(workloadKey, entries);
				const auto insertEnd = std::chrono::steady_clock::now();
				m_stats.activeDrawSetInsertCalls += 1;
				m_stats.activeDrawSetInsertIndices += entries.size();
				m_stats.activeDrawSetInsertUs += static_cast<std::uint64_t>(
					std::chrono::duration_cast<std::chrono::microseconds>(insertEnd - insertBegin).count());
			}
		}
	}

	{
		ZoneScopedN("ObjectManager::PublishStaticImportPacket::FinalizeStats");
		const auto finalizeBegin = std::chrono::steady_clock::now();
		for (const auto& drawInfo : drawInfos) {
			if (!drawInfo.instanceDrawRecordIndices.empty()) {
				++m_stats.staticDirectGroupsImported;
			}
		}
		m_stats.staticDirectFinalizeUs += static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - finalizeBegin).count());
	}
	m_stats.staticDirectPacketPublishUs += static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - importBegin).count());
	m_stats.staticDirectImportUs += static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - importBegin).count());

	return drawInfos;
}

std::vector<Components::ObjectDrawInfo> ObjectManager::CommitPreparedStaticGroupsBulk(const PreparedStaticGroupsBulkPlan& plan) {
	StaticImportPacketPlan packetPlan;
	packetPlan.prepared = plan;
	return PublishStaticImportPacket(BuildStaticImportPacket(std::move(packetPlan)));
}

std::vector<Components::ObjectDrawInfo> ObjectManager::AddStaticGroupsBulk(const std::vector<StaticGroupBuildInfo>& groups) {
	auto plan = PrepareStaticGroupsBulkPlan(groups);
	return CommitPreparedStaticGroupsBulk(plan);
}

ObjectManager::StaticObjectRemovalPayload ObjectManager::BuildStaticObjectRemovalPayload(std::span<const Components::ObjectDrawInfo> drawInfos) const {
	StaticObjectRemovalPayload payload;
	payload.drawInfoCount = drawInfos.size();

	const auto addRange = [&payload](
		const std::shared_ptr<DynamicBuffer>& buffer,
		const Components::ObjectDrawInfo::BufferRange& range,
		StaticObjectRemovalPayload::BufferKind kind)
	{
		if (buffer && range.IsValid()) {
			payload.bufferRanges.push_back(StaticObjectRemovalPayload::BufferRetireRange{ buffer, range, kind });
		}
	};
	const auto addRanges = [&addRange](
		const std::shared_ptr<DynamicBuffer>& buffer,
		const std::vector<Components::ObjectDrawInfo::BufferRange>& ranges,
		StaticObjectRemovalPayload::BufferKind kind)
	{
		for (const auto& range : ranges) {
			addRange(buffer, range, kind);
		}
	};
	const auto addView = [&payload](
		const std::shared_ptr<DynamicBuffer>& buffer,
		const std::shared_ptr<BufferView>& view,
		StaticObjectRemovalPayload::BufferKind kind)
	{
		if (!buffer || !view) {
			return;
		}
		payload.bufferRanges.push_back(StaticObjectRemovalPayload::BufferRetireRange{
			buffer,
			Components::ObjectDrawInfo::BufferRange{ view->GetOffset(), view->GetSize(), 1, view->GetSize() },
			kind
		});
	};

	for (const auto& drawInfo : drawInfos) {
		if (!drawInfo.ownedPerObjectCBPages.empty()) {
			addRanges(m_perObjectBuffers, drawInfo.ownedPerObjectCBPages, StaticObjectRemovalPayload::BufferKind::PerObject);
		} else if (!drawInfo.perObjectCBViews.empty()) {
			for (const auto& view : drawInfo.perObjectCBViews) {
				addView(m_perObjectBuffers, view, StaticObjectRemovalPayload::BufferKind::PerObject);
			}
		} else if (drawInfo.perObjectCBView) {
			addView(m_perObjectBuffers, drawInfo.perObjectCBView, StaticObjectRemovalPayload::BufferKind::PerObject);
		} else {
			addRange(m_perObjectBuffers, drawInfo.perObjectCBRange, StaticObjectRemovalPayload::BufferKind::PerObject);
		}

		if (!drawInfo.ownedPerInstanceTransformPages.empty()) {
			addRanges(m_perInstanceTransformBuffers, drawInfo.ownedPerInstanceTransformPages, StaticObjectRemovalPayload::BufferKind::InstanceTransform);
		} else if (!drawInfo.perInstanceTransformViews.empty()) {
			for (const auto& view : drawInfo.perInstanceTransformViews) {
				addView(m_perInstanceTransformBuffers, view, StaticObjectRemovalPayload::BufferKind::InstanceTransform);
			}
		} else {
			addRange(m_perInstanceTransformBuffers, drawInfo.perInstanceTransformRange, StaticObjectRemovalPayload::BufferKind::InstanceTransform);
		}

		if (!drawInfo.ownedInstanceDrawRecordPages.empty()) {
			addRanges(m_instanceDrawRecordBuffers, drawInfo.ownedInstanceDrawRecordPages, StaticObjectRemovalPayload::BufferKind::InstanceDrawRecord);
		} else if (!drawInfo.drawInfo.views.empty()) {
			for (const auto& view : drawInfo.drawInfo.views) {
				addView(m_instanceDrawRecordBuffers, view, StaticObjectRemovalPayload::BufferKind::InstanceDrawRecord);
			}
		} else {
			addRange(m_instanceDrawRecordBuffers, drawInfo.instanceDrawRecordRange, StaticObjectRemovalPayload::BufferKind::InstanceDrawRecord);
		}

		if (!drawInfo.ownedNormalMatrixPages.empty()) {
			addRanges(m_normalMatrixBuffer, drawInfo.ownedNormalMatrixPages, StaticObjectRemovalPayload::BufferKind::NormalMatrix);
		} else if (!drawInfo.normalMatrixViews.empty()) {
			for (const auto& view : drawInfo.normalMatrixViews) {
				addView(m_normalMatrixBuffer, view, StaticObjectRemovalPayload::BufferKind::NormalMatrix);
			}
		} else if (drawInfo.normalMatrixView) {
			addView(m_normalMatrixBuffer, drawInfo.normalMatrixView, StaticObjectRemovalPayload::BufferKind::NormalMatrix);
		} else {
			addRange(m_normalMatrixBuffer, drawInfo.normalMatrixRange, StaticObjectRemovalPayload::BufferKind::NormalMatrix);
		}

		if (!drawInfo.activeDrawSetRemovals.empty()) {
			payload.activeDrawSetRemovals.insert(
				payload.activeDrawSetRemovals.end(),
				drawInfo.activeDrawSetRemovals.begin(),
				drawInfo.activeDrawSetRemovals.end());
		} else {
			for (size_t i = 0; i < drawInfo.drawInfo.indices.size(); ++i) {
				const auto index = drawInfo.drawInfo.indices[i];
				if (i >= drawInfo.drawInfo.drawWorkloadKeysPerDraw.size()) {
					continue;
				}
				for (const auto& workloadKey : drawInfo.drawInfo.drawWorkloadKeysPerDraw[i]) {
					auto bucketIt = std::find_if(
						payload.activeDrawSetRemovals.begin(),
						payload.activeDrawSetRemovals.end(),
						[&workloadKey](const auto& bucket) { return bucket.workloadKey == workloadKey; });
					if (bucketIt == payload.activeDrawSetRemovals.end()) {
						auto& bucket = payload.activeDrawSetRemovals.emplace_back();
						bucket.workloadKey = workloadKey;
						bucket.indices.push_back(index);
					} else {
						bucketIt->indices.push_back(index);
					}
				}
			}
		}

		if (!drawInfo.instanceDrawRecordIndices.empty()) {
			payload.drawRecordIndices.insert(
				payload.drawRecordIndices.end(),
				drawInfo.instanceDrawRecordIndices.begin(),
				drawInfo.instanceDrawRecordIndices.end());
		} else {
			payload.drawRecordIndices.insert(
				payload.drawRecordIndices.end(),
				drawInfo.drawInfo.indices.begin(),
				drawInfo.drawInfo.indices.end());
		}
		payload.skinnedAssemblyPlacementIndices.insert(
			payload.skinnedAssemblyPlacementIndices.end(),
			drawInfo.skinnedAssemblyPlacementIndices.begin(),
			drawInfo.skinnedAssemblyPlacementIndices.end());
	}

	return payload;
}

void ObjectManager::RemoveObject(const Components::ObjectDrawInfo* drawInfo) {
#ifdef _DEBUG
	if (drawInfo == nullptr) {
		throw std::runtime_error("ObjectDrawInfo is null");
		return;
	}
#endif // _DEBUG

	RemoveObjectsBulk({ drawInfo });
}

void ObjectManager::RemoveObjectsBulk(
	const std::vector<const Components::ObjectDrawInfo*>& drawInfos,
	const RemoveObjectsBulkOptions& options)
{
	ZoneScopedN("ObjectManager::RemoveObjectsBulk");
	if (drawInfos.empty()) {
		return;
	}
	ZoneValue(drawInfos.size());
	std::vector<StaticObjectRemovalPayload> payloads;
	payloads.reserve(drawInfos.size());
	for (const auto* drawInfo : drawInfos) {
		if (!drawInfo) {
			continue;
		}
		payloads.push_back(BuildStaticObjectRemovalPayload(std::span<const Components::ObjectDrawInfo>(drawInfo, 1)));
	}
	RemoveStaticObjectsBulk(payloads, options);
}


void ObjectManager::RemoveObjectsBulk(const std::vector<const Components::ObjectDrawInfo *> &drawInfos) {
	RemoveObjectsBulk(drawInfos, {});
}

void ObjectManager::RemoveStaticObjectsBulk(
	std::span<const StaticObjectRemovalPayload> payloads,
	const RemoveObjectsBulkOptions& options)
{
	ZoneScopedN("ObjectManager::RemoveStaticObjectsBulk");
	if (payloads.empty()) {
		return;
	}
	ZoneValue(payloads.size());
	const auto removeBegin = std::chrono::steady_clock::now();
	++m_stats.bulkRemoveCalls;
	for (const auto& payload : payloads) {
		m_stats.bulkRemoveObjects += payload.drawInfoCount;
	}

	std::unordered_map<DrawWorkloadKey, std::size_t, DrawWorkloadKey::Hasher> activeDrawSetRemoveCounts;
	std::vector<DeferredBufferRangeRetire> deferredRetires;
	std::vector<std::uint32_t> tombstoneDrawRecordIndices;
	std::uint64_t pageDeallocUs = 0;
	std::uint64_t collectUs = 0;
	std::size_t totalBufferRanges = 0;
	std::size_t skippedInstanceDrawRecordRanges = 0;
	std::size_t totalDrawRecordIndices = 0;
	std::size_t totalSkinnedAssemblyPlacements = 0;
	for (const auto& payload : payloads) {
		totalBufferRanges += payload.bufferRanges.size();
		totalDrawRecordIndices += payload.drawRecordIndices.size();
		totalSkinnedAssemblyPlacements += payload.skinnedAssemblyPlacementIndices.size();
	}
	TracyPlot("ObjectManager.RemoveStaticObjectsBulk.Payloads", static_cast<int64_t>(payloads.size()));
	TracyPlot("ObjectManager.RemoveStaticObjectsBulk.BufferRanges", static_cast<int64_t>(totalBufferRanges));
	TracyPlot("ObjectManager.RemoveStaticObjectsBulk.DrawRecordIndices", static_cast<int64_t>(totalDrawRecordIndices));
	if (options.deferBufferRangeRetirement) {
		deferredRetires.reserve(totalBufferRanges);
	}
	tombstoneDrawRecordIndices.reserve(totalDrawRecordIndices);

	const auto retireOrDeallocateRange = [this, &options, &pageDeallocUs, &deferredRetires](
		const std::shared_ptr<DynamicBuffer>& buffer,
		std::uint64_t offset,
		std::uint64_t size)
	{
		if (!buffer) {
			return;
		}
		if (options.deferBufferRangeRetirement) {
			deferredRetires.push_back(DeferredBufferRangeRetire{
				buffer,
				offset,
				size,
				options.retireFrame
			});
			return;
		}
		const auto begin = std::chrono::steady_clock::now();
		buffer->DeallocateRange(offset, size);
		pageDeallocUs += static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count());
	};
	const auto retireOrDeallocateView = [&retireOrDeallocateRange](const std::shared_ptr<DynamicBuffer>& buffer, const std::shared_ptr<BufferView>& view) {
		if (buffer && view) {
			retireOrDeallocateRange(buffer, view->GetOffset(), view->GetSize());
		}
	};
	const auto retireOrDeallocateOwnedRanges = [this, &options, &pageDeallocUs](const std::shared_ptr<DynamicBuffer>& buffer, const std::vector<Components::ObjectDrawInfo::BufferRange>& ranges) {
		if (!buffer) {
			return;
		}
		std::vector<Components::ObjectDrawInfo::BufferRange> sortedRanges;
		sortedRanges.reserve(ranges.size());
		for (const auto& range : ranges) {
			if (range.IsValid()) {
				sortedRanges.push_back(range);
			}
		}
		std::sort(sortedRanges.begin(), sortedRanges.end(), [](const auto& lhs, const auto& rhs) {
			return lhs.offset < rhs.offset;
		});
		if (options.deferBufferRangeRetirement) {
			EnqueueDeferredBufferRangeRetires(buffer, sortedRanges, options.retireFrame);
			return;
		}
		const auto begin = std::chrono::steady_clock::now();
		for (const auto& range : sortedRanges) {
			if (range.IsValid()) {
				buffer->DeallocateRange(range.offset, range.size);
			}
		}
		pageDeallocUs += static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count());
	};

	{
		ZoneScopedN("ObjectManager::RemoveObjectsBulk::CollectRetiresAndActiveRemovals");
		for (const auto& payload : payloads) {
			for (const auto& retireRange : payload.bufferRanges) {
				if (!options.retireInstanceDrawRecordRanges &&
					retireRange.kind == StaticObjectRemovalPayload::BufferKind::InstanceDrawRecord) {
					++skippedInstanceDrawRecordRanges;
					continue;
				}
				if (retireRange.range.IsValid()) {
					retireOrDeallocateRange(retireRange.buffer, retireRange.range.offset, retireRange.range.size);
				}
			}
			const auto collectBegin = std::chrono::steady_clock::now();
			for (const auto& bucket : payload.activeDrawSetRemovals) {
				activeDrawSetRemoveCounts[bucket.workloadKey] += bucket.indices.size();
			}
			collectUs += static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - collectBegin).count());
			tombstoneDrawRecordIndices.insert(
				tombstoneDrawRecordIndices.end(),
				payload.drawRecordIndices.begin(),
				payload.drawRecordIndices.end());
		}
	}
	if (!deferredRetires.empty()) {
		ZoneScopedN("ObjectManager::RemoveStaticObjectsBulk::EnqueueDeferredRetiresBatch");
		EnqueueDeferredBufferRangeRetires(std::move(deferredRetires));
	}
	TracyPlot(
		"ObjectManager.RemoveStaticObjectsBulk.SkippedInstanceDrawRecordRetireRanges",
		static_cast<int64_t>(skippedInstanceDrawRecordRanges));
	if (!tombstoneDrawRecordIndices.empty()) {
		ZoneScopedN("ObjectManager::RemoveStaticObjectsBulk::TombstoneDrawRecordsBatch");
		ZoneValue(tombstoneDrawRecordIndices.size());
		TombstoneDrawRecords(tombstoneDrawRecordIndices);
	}

	if (totalSkinnedAssemblyPlacements != 0u) {
		std::size_t invalidated = 0u;
			for (const auto& payload : payloads) {
			for (const auto placementIndex : payload.skinnedAssemblyPlacementIndices) {
				if (placementIndex >= m_skinnedAssemblyPlacementCPU.size()) continue;
				FreeSkinnedAssemblyPlacement(placementIndex);
				++invalidated;
			}
		}
		if (invalidated != 0u) {
			m_skinnedAssemblyPlacements->ReplaceData(m_skinnedAssemblyPlacementCPU);
			const auto currentLive = m_activeSkinnedAssemblyPlacements->LiveSize();
			m_activeSkinnedAssemblyPlacements->SetLiveSize(currentLive > invalidated ? currentLive - invalidated : 0u);
			m_activeSkinnedAssemblyPlacements->AddActiveTombstoneEstimate(invalidated);
			const auto stale = m_activeSkinnedAssemblyPlacements->ActiveTombstoneEstimate();
			const auto span = m_activeSkinnedAssemblyPlacements->Size();
			if (stale >= 256u && stale * 3u >= span) {
				auto entries = m_activeSkinnedAssemblyPlacements->SnapshotActiveEntries();
				std::erase_if(entries, [this](const SortedUnsignedIntBuffer::ActiveDrawSetEntry& entry) {
					return entry.drawRecordIndex >= m_skinnedAssemblyPlacementCPU.size() ||
						m_skinnedAssemblyPlacementCPU[entry.drawRecordIndex].generation != entry.generation;
				});
				m_activeSkinnedAssemblyPlacements->AssignActiveSnapshot(std::move(entries));
			}
			spdlog::info("Skinned assembly placements: invalidated={} live={} activeEntries={} staleEstimate={}.",
				invalidated, m_activeSkinnedAssemblyPlacements->LiveSize(),
				m_activeSkinnedAssemblyPlacements->Size(), m_activeSkinnedAssemblyPlacements->ActiveTombstoneEstimate());
		}
	}

	m_stats.bulkRemovePageDeallocUs += pageDeallocUs;
	m_stats.bulkRemoveCollectUs += collectUs;
	TracyPlot("ObjectManager.RemoveObjectsBulk.ActiveBuckets", static_cast<int64_t>(activeDrawSetRemoveCounts.size()));

	std::size_t activeRemoveIndexCount = 0;
	for (const auto& [workloadKey, removeCount] : activeDrawSetRemoveCounts) {
		if (removeCount == 0) {
			continue;
		}
		activeRemoveIndexCount += removeCount;
		auto activeDrawSetIt = m_activeDrawSetIndices.find(workloadKey);
		if (activeDrawSetIt == m_activeDrawSetIndices.end() || !activeDrawSetIt->second) {
			spdlog::warn(
				"ObjectManager::RemoveObjectsBulk: missing active draw set while removing {} indices flags={} phase={} clodOnly={}",
				removeCount,
				static_cast<std::uint64_t>(workloadKey.compileFlags),
				workloadKey.renderPhase.hash,
				workloadKey.clodOnly);
			continue;
		}
		ZoneScopedN("ObjectManager::RemoveStaticObjectsBulk::ActiveDrawSetTombstone");
		ZoneValue(removeCount);
		const auto activeRemoveBegin = std::chrono::steady_clock::now();
		const auto currentLive = activeDrawSetIt->second->LiveSize();
		activeDrawSetIt->second->SetLiveSize(currentLive > removeCount ? currentLive - removeCount : 0u);
		activeDrawSetIt->second->AddActiveTombstoneEstimate(removeCount);
		TracyPlot("ObjectManager.RemoveStaticObjectsBulk.ActiveSetSize", static_cast<int64_t>(activeDrawSetIt->second->Size()));
		TracyPlot("ObjectManager.RemoveStaticObjectsBulk.ActiveSetLiveSize", static_cast<int64_t>(activeDrawSetIt->second->LiveSize()));
		TracyPlot("ObjectManager.RemoveStaticObjectsBulk.ActiveSetTombstoneEstimate", static_cast<int64_t>(activeDrawSetIt->second->ActiveTombstoneEstimate()));
		MaybeQueueActiveDrawSetCompaction(workloadKey, activeDrawSetIt->second);
		const auto activeRemoveEnd = std::chrono::steady_clock::now();
		m_stats.activeDrawSetRemoveCalls += 1;
		m_stats.activeDrawSetRemoveIndices += removeCount;
		m_stats.activeDrawSetRemoveUs += static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::microseconds>(activeRemoveEnd - activeRemoveBegin).count());
	}
	TracyPlot("ObjectManager.RemoveStaticObjectsBulk.ActiveRemoveIndices", static_cast<int64_t>(activeRemoveIndexCount));

	m_stats.bulkRemoveUs += static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - removeBegin).count());
}

void ObjectManager::RemoveStaticObjectsBulk(std::span<const StaticObjectRemovalPayload> payloads) {
	RemoveStaticObjectsBulk(payloads, {});
}

void ObjectManager::UpdatePerObjectBuffer(BufferView* view, PerObjectCB& data) {
	std::lock_guard<std::mutex> lock(m_objectUpdateMutex);
	m_perObjectBuffers->UpdateView(view, &data);
}

void ObjectManager::UpdateNormalMatrixBuffer(BufferView* view, void* data) {
	std::lock_guard<std::mutex> lock(m_normalMatrixUpdateMutex);
	m_normalMatrixBuffer->UpdateView(view, data);
}

rg::runtime::BulkWriteHandle ObjectManager::BeginPerObjectBulkWrite() {
	return m_perObjectBuffers->BeginBulkWrite();
}

void ObjectManager::EndPerObjectBulkWrite(size_t dirtyOffset, size_t dirtySize) {
	m_perObjectBuffers->EndBulkWrite(dirtyOffset, dirtySize);
}

rg::runtime::BulkWriteHandle ObjectManager::BeginPerInstanceTransformBulkWrite() {
	return m_perInstanceTransformBuffers->BeginBulkWrite();
}

void ObjectManager::EndPerInstanceTransformBulkWrite(size_t dirtyOffset, size_t dirtySize) {
	m_perInstanceTransformBuffers->EndBulkWrite(dirtyOffset, dirtySize);
}

rg::runtime::BulkWriteHandle ObjectManager::BeginNormalMatrixBulkWrite() {
	return m_normalMatrixBuffer->BeginBulkWrite();
}

void ObjectManager::EndNormalMatrixBulkWrite(size_t dirtyOffset, size_t dirtySize) {
	m_normalMatrixBuffer->EndBulkWrite(dirtyOffset, dirtySize);
}

std::shared_ptr<Resource> ObjectManager::ProvideResource(ResourceIdentifier const& key) {
	return m_resources[key];
}

std::vector<ResourceIdentifier> ObjectManager::GetSupportedKeys() {
	std::vector<ResourceIdentifier> keys;
	keys.reserve(m_resources.size());
	for (auto const& [key, _] : m_resources)
		keys.push_back(key);

	return keys;
}
