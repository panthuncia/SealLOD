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
#include <spdlog/spdlog.h>

namespace {

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

}

ObjectManager::ObjectManager() {
	auto& resourceManager = ResourceManager::GetInstance();
	m_perObjectBuffers = DynamicBuffer::CreateShared(sizeof(PerObjectCB), 10000, "perObjectBuffers<PerObjectCB>");
	m_perInstanceTransformBuffers = DynamicBuffer::CreateShared(sizeof(PerInstanceTransformCB), 10000, "perInstanceTransformBuffers<PerInstanceTransformCB>");
	m_instanceDrawRecordBuffers = DynamicBuffer::CreateShared(sizeof(InstanceDrawRecordCB), 10000, "instanceDrawRecordBuffers<InstanceDrawRecordCB>");
	m_drawRecordVisibilityGenerationBuffer = DynamicBuffer::CreateShared(sizeof(std::uint32_t), 10000, "drawRecordVisibilityGenerationBuffer<uint>");
	m_masterIndirectCommandsBuffer = DynamicBuffer::CreateShared(sizeof(DispatchMeshIndirectCommand), 10000, "masterIndirectCommandsBuffer<IndirectCommand>");

	m_normalMatrixBuffer = DynamicBuffer::CreateShared(sizeof(DirectX::XMFLOAT4X4), 10000, "normalMatrixBuffer");

	rg::memory::SetResourceUsageHint(*m_perObjectBuffers, "PerMesh, PerMeshInstance, PerObject");
	rg::memory::SetResourceUsageHint(*m_perInstanceTransformBuffers, "PerMesh, InstanceDrawRecord, PerInstanceTransform");
	rg::memory::SetResourceUsageHint(*m_instanceDrawRecordBuffers, "PerMesh, InstanceDrawRecord, PerInstanceTransform");
	rg::memory::SetResourceUsageHint(*m_drawRecordVisibilityGenerationBuffer, "PerMesh, InstanceDrawRecord, VisibilityGeneration");
	rg::memory::SetResourceUsageHint(*m_normalMatrixBuffer, "PerMesh, PerMeshInstance, PerObject");

	rg::memory::SetResourceUsageHint(*m_masterIndirectCommandsBuffer, "Indirect command buffers");

	m_resources[Builtin::PerObjectBuffer] = m_perObjectBuffers;
	m_resources[Builtin::PerInstanceTransformBuffer] = m_perInstanceTransformBuffers;
	m_resources[Builtin::InstanceDrawRecordBuffer] = m_instanceDrawRecordBuffers;
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
	if (!buffer || size == 0) {
		return;
	}
	{
		std::lock_guard lock(m_deferredRetireMutex);
		m_deferredRetireQueue.push_back(DeferredBufferRangeRetire{
			buffer,
			offset,
			size,
			retireFrame
		});
		m_deferredRetireQueueDepth.store(m_deferredRetireQueue.size(), std::memory_order_relaxed);
	}
	m_deferredRetireRangesQueued.fetch_add(1, std::memory_order_relaxed);
	m_deferredRetireBytesQueued.fetch_add(size, std::memory_order_relaxed);
	m_deferredRetireCv.notify_one();
}

void ObjectManager::EnqueueDeferredBufferRangeRetires(
	const std::shared_ptr<DynamicBuffer>& buffer,
	const std::vector<Components::ObjectDrawInfo::BufferRange>& ranges,
	std::uint64_t retireFrame)
{
	if (!buffer) {
		return;
	}
	for (const auto& range : ranges) {
		if (range.IsValid()) {
			EnqueueDeferredBufferRangeRetire(buffer, range.offset, range.size, retireFrame);
		}
	}
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
	const auto liveEntries = static_cast<std::uint64_t>(buffer->LiveSize());
	if (totalEntries < 65536u || totalEntries <= liveEntries) {
		return;
	}

	const auto staleEntries = totalEntries - liveEntries;
	if (staleEntries < 16384u && staleEntries * 100u < totalEntries * 35u) {
		return;
	}

	ActiveDrawSetCompactionJob job;
	job.workloadKey = workloadKey;
	job.buffer = buffer;
	job.activeSetRevision = buffer->MutationRevision();
	job.visibilityRevision = m_drawRecordVisibilityRevision;
	job.entries = buffer->SnapshotActiveEntries();
	job.visibilityGenerations = m_drawRecordVisibilityGenerations;

	{
		std::lock_guard lock(m_activeDrawSetCompactionMutex);
		if (m_activeDrawSetCompactionQueued.contains(workloadKey)) {
			return;
		}
		m_activeDrawSetCompactionQueued.insert(workloadKey);
		m_activeDrawSetCompactionJobs.push_back(std::move(job));
	}
	++m_stats.activeDrawSetCompactionJobsQueued;
	m_stats.activeDrawSetCompactionInputEntries += totalEntries;
	m_activeDrawSetCompactionCv.notify_one();
}

void ObjectManager::PublishActiveDrawSetCompactionResults(std::size_t maxResults) {
	if (maxResults == 0) {
		return;
	}

	std::vector<ActiveDrawSetCompactionResult> results;
	results.reserve(maxResults);
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
			++m_stats.activeDrawSetCompactionJobsStale;
			if (buffer) {
				MaybeQueueActiveDrawSetCompaction(result.workloadKey, buffer);
			}
			continue;
		}

		const auto publishBegin = std::chrono::steady_clock::now();
		buffer->AssignActiveSnapshot(std::move(result.entries));
		m_stats.activeDrawSetCompactionJobsPublished += 1;
		m_stats.activeDrawSetCompactionOutputEntries += buffer->LiveSize();
		m_stats.activeDrawSetCompactionPublishUs += static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - publishBegin).count());
		++m_drawSetDeclarationRevision;
	}
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

std::uint32_t ObjectManager::ActivateDrawRecord(std::uint32_t drawRecordIndex) {
	const auto generation = ActivateDrawRecordCPU(drawRecordIndex);
	const auto requiredBytes = (static_cast<std::size_t>(drawRecordIndex) + 1u) * sizeof(std::uint32_t);
	m_drawRecordVisibilityGenerationBuffer->ReserveBytes(requiredBytes);
	m_drawRecordVisibilityGenerationBuffer->StageWriteRange(
		&generation,
		sizeof(generation),
		static_cast<std::size_t>(drawRecordIndex) * sizeof(std::uint32_t));
	return generation;
}

void ObjectManager::TombstoneDrawRecord(std::uint32_t drawRecordIndex) {
	if (drawRecordIndex >= m_drawRecordVisibilityGenerations.size()) {
		return;
	}
	m_drawRecordVisibilityGenerations[drawRecordIndex] = 0u;
	++m_drawRecordVisibilityRevision;
	const std::uint32_t zero = 0u;
	m_drawRecordVisibilityGenerationBuffer->StageWriteRange(
		&zero,
		sizeof(zero),
		static_cast<std::size_t>(drawRecordIndex) * sizeof(std::uint32_t));
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
				const uint32_t perMeshInstanceBufferIndex = static_cast<uint32_t>(meshInstance->GetPerMeshInstanceBufferOffset() / sizeof(PerMeshInstanceCB));
				if (transformIndex == 0) {
					meshInstance->SetPerObjectBufferIndex(meshPerObjectIndex);
				}
				InstanceDrawRecordCB drawRecord{};
				drawRecord.meshTemplateIndex = perMeshInstanceBufferIndex;
				drawRecord.instanceTransformIndex = instanceTransformIndex;
				drawRecord.clodOffsetIndex = perMeshInstanceBufferIndex;
				drawRecord.flags = 0u;
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
	if (groups.empty()) {
		return plan;
	}

	const auto prepareBegin = std::chrono::steady_clock::now();
	plan.groups.reserve(groups.size());

	const auto transformBuildBegin = std::chrono::steady_clock::now();
	for (const auto& group : groups) {
		auto& prepared = plan.groups.emplace_back();
		prepared.stableGroupID = group.stableGroupID;
		prepared.allocationScopeID = group.allocationScopeID;
		prepared.meshTemplates = group.meshTemplates;
		prepared.perObjectCBs.reserve(group.instanceTransforms.size());
		prepared.normalMatrices.reserve(group.instanceTransforms.size());
		prepared.workloadKeysByMeshTemplate.reserve(group.meshTemplates.size());

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
	return plan;
}

void ObjectManager::PrepareStaticGroupCommitResourcesAsync(const PreparedStaticGroupsBulkPlan& plan) {
	if (plan.groups.empty()) {
		return;
	}

	const size_t transformRows = static_cast<size_t>(plan.transformRows);
	const size_t drawRecords = static_cast<size_t>(plan.drawRecords);
	if (transformRows != 0) {
		m_perObjectBuffers->RequestAsyncReserveBytes(
			ReserveBytesWithStaticImportHeadroom(transformRows * sizeof(PerObjectCB), 512ull * 1024ull));
		m_perInstanceTransformBuffers->RequestAsyncReserveBytes(
			ReserveBytesWithStaticImportHeadroom(transformRows * sizeof(PerInstanceTransformCB), 512ull * 1024ull));
		m_normalMatrixBuffer->RequestAsyncReserveBytes(
			ReserveBytesWithStaticImportHeadroom(transformRows * sizeof(DirectX::XMFLOAT4X4), 512ull * 1024ull));
	}
	if (drawRecords != 0) {
		m_instanceDrawRecordBuffers->RequestAsyncReserveBytes(
			ReserveBytesWithStaticImportHeadroom(drawRecords * sizeof(InstanceDrawRecordCB), 1024ull * 1024ull));
	}
}

void ObjectManager::PublishPreparedStaticGroupCommitResourceResizes(bool wait) {
	(void)m_perObjectBuffers->PublishReadyAsyncResize(wait);
	(void)m_perInstanceTransformBuffers->PublishReadyAsyncResize(wait);
	(void)m_normalMatrixBuffer->PublishReadyAsyncResize(wait);
	(void)m_instanceDrawRecordBuffers->PublishReadyAsyncResize(wait);
}

ObjectManager::StaticImportPacketPlan ObjectManager::PrepareStaticImportPacketPlan(const std::vector<StaticGroupBuildInfo>& groups) {
	StaticImportPacketPlan plan;
	plan.prepared = PrepareStaticGroupsBulkPlan(groups);
	return plan;
}

void ObjectManager::RequestStaticImportPacketResources(const StaticImportPacketPlan& plan) {
	PrepareStaticGroupCommitResourcesAsync(plan.prepared);
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
				if (!meshTemplate.mesh || !meshTemplate.material) {
					continue;
				}
				StaticImportPacket::PatchableDrawRecord record;
				record.groupIndex = groupIndex;
				record.scopeTransformOrdinal = transformOrdinal;
				record.meshTemplateIndex = meshTemplate.meshTemplateIndex;
				record.clodOffsetIndex = meshTemplate.clodOffsetIndex;
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
		PublishPreparedStaticGroupCommitResourceResizes(false);
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
		std::vector<std::uint32_t> packetVisibilityGenerations;
		packetVisibilityGenerations.reserve(static_cast<size_t>(packet.drawRecords));
		std::uint32_t firstVisibilityGenerationIndex = 0;
		{
			ZoneScopedN("ObjectManager::PublishStaticImportPacket::DrawRecordPages::ReserveVisibilityGenerationRange");
			const auto firstRangeIt = std::find_if(
				instanceDrawRecordRanges.begin(),
				instanceDrawRecordRanges.end(),
				[](const auto& range) { return range.IsValid(); });
			if (firstRangeIt != instanceDrawRecordRanges.end()) {
				const auto lastRangeIt = std::find_if(
					instanceDrawRecordRanges.rbegin(),
					instanceDrawRecordRanges.rend(),
					[](const auto& range) { return range.IsValid(); });
				firstVisibilityGenerationIndex = static_cast<std::uint32_t>(
					firstRangeIt->offset / sizeof(InstanceDrawRecordCB));
				const auto maxDrawRecordIndexExclusive = static_cast<std::size_t>(
					(lastRangeIt->offset + lastRangeIt->size) / sizeof(InstanceDrawRecordCB));
				if (maxDrawRecordIndexExclusive > m_drawRecordVisibilityGenerations.size()) {
					m_drawRecordVisibilityGenerations.resize(maxDrawRecordIndexExclusive, 0u);
				}
				m_drawRecordVisibilityGenerationBuffer->ReserveBytes(
					maxDrawRecordIndexExclusive * sizeof(std::uint32_t));
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
					packetVisibilityGenerations.push_back(drawRecordGeneration);

					InstanceDrawRecordCB drawRecord{};
					drawRecord.meshTemplateIndex = sourceRecord.meshTemplateIndex;
					drawRecord.instanceTransformIndex = static_cast<uint32_t>(instanceTransformOffset / sizeof(PerInstanceTransformCB));
					drawRecord.clodOffsetIndex = sourceRecord.clodOffsetIndex;
					drawRecord.flags = 0u;
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

		if (!packetVisibilityGenerations.empty()) {
			ZoneScopedN("ObjectManager::PublishStaticImportPacket::DrawRecordPages::StageVisibilityGenerationRange");
			m_drawRecordVisibilityGenerationBuffer->StageWriteRange(
				packetVisibilityGenerations.data(),
				packetVisibilityGenerations.size() * sizeof(std::uint32_t),
				static_cast<std::size_t>(firstVisibilityGenerationIndex) * sizeof(std::uint32_t));
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

	const auto addRange = [&payload](const std::shared_ptr<DynamicBuffer>& buffer, const Components::ObjectDrawInfo::BufferRange& range) {
		if (buffer && range.IsValid()) {
			payload.bufferRanges.push_back(StaticObjectRemovalPayload::BufferRetireRange{ buffer, range });
		}
	};
	const auto addRanges = [&addRange](const std::shared_ptr<DynamicBuffer>& buffer, const std::vector<Components::ObjectDrawInfo::BufferRange>& ranges) {
		for (const auto& range : ranges) {
			addRange(buffer, range);
		}
	};
	const auto addView = [&payload](const std::shared_ptr<DynamicBuffer>& buffer, const std::shared_ptr<BufferView>& view) {
		if (!buffer || !view) {
			return;
		}
		payload.bufferRanges.push_back(StaticObjectRemovalPayload::BufferRetireRange{
			buffer,
			Components::ObjectDrawInfo::BufferRange{ view->GetOffset(), view->GetSize(), 1, view->GetSize() }
		});
	};

	for (const auto& drawInfo : drawInfos) {
		if (!drawInfo.ownedPerObjectCBPages.empty()) {
			addRanges(m_perObjectBuffers, drawInfo.ownedPerObjectCBPages);
		} else if (!drawInfo.perObjectCBViews.empty()) {
			for (const auto& view : drawInfo.perObjectCBViews) {
				addView(m_perObjectBuffers, view);
			}
		} else if (drawInfo.perObjectCBView) {
			addView(m_perObjectBuffers, drawInfo.perObjectCBView);
		} else {
			addRange(m_perObjectBuffers, drawInfo.perObjectCBRange);
		}

		if (!drawInfo.ownedPerInstanceTransformPages.empty()) {
			addRanges(m_perInstanceTransformBuffers, drawInfo.ownedPerInstanceTransformPages);
		} else if (!drawInfo.perInstanceTransformViews.empty()) {
			for (const auto& view : drawInfo.perInstanceTransformViews) {
				addView(m_perInstanceTransformBuffers, view);
			}
		} else {
			addRange(m_perInstanceTransformBuffers, drawInfo.perInstanceTransformRange);
		}

		if (!drawInfo.ownedInstanceDrawRecordPages.empty()) {
			addRanges(m_instanceDrawRecordBuffers, drawInfo.ownedInstanceDrawRecordPages);
		} else if (!drawInfo.drawInfo.views.empty()) {
			for (const auto& view : drawInfo.drawInfo.views) {
				addView(m_instanceDrawRecordBuffers, view);
			}
		} else {
			addRange(m_instanceDrawRecordBuffers, drawInfo.instanceDrawRecordRange);
		}

		if (!drawInfo.ownedNormalMatrixPages.empty()) {
			addRanges(m_normalMatrixBuffer, drawInfo.ownedNormalMatrixPages);
		} else if (!drawInfo.normalMatrixViews.empty()) {
			for (const auto& view : drawInfo.normalMatrixViews) {
				addView(m_normalMatrixBuffer, view);
			}
		} else if (drawInfo.normalMatrixView) {
			addView(m_normalMatrixBuffer, drawInfo.normalMatrixView);
		} else {
			addRange(m_normalMatrixBuffer, drawInfo.normalMatrixRange);
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

	std::unordered_map<DrawWorkloadKey, std::vector<unsigned int>, DrawWorkloadKey::Hasher> activeDrawSetRemoves;
	std::uint64_t pageDeallocUs = 0;
	std::uint64_t collectUs = 0;
	const auto retireOrDeallocateRange = [this, &options, &pageDeallocUs](
		const std::shared_ptr<DynamicBuffer>& buffer,
		std::uint64_t offset,
		std::uint64_t size)
	{
		if (!buffer) {
			return;
		}
		if (options.deferBufferRangeRetirement) {
			EnqueueDeferredBufferRangeRetire(buffer, offset, size, options.retireFrame);
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
				if (retireRange.range.IsValid()) {
					retireOrDeallocateRange(retireRange.buffer, retireRange.range.offset, retireRange.range.size);
				}
			}
			const auto collectBegin = std::chrono::steady_clock::now();
			for (const auto& bucket : payload.activeDrawSetRemovals) {
				auto& indices = activeDrawSetRemoves[bucket.workloadKey];
				indices.insert(indices.end(), bucket.indices.begin(), bucket.indices.end());
			}
			collectUs += static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - collectBegin).count());
			for (const auto drawRecordIndex : payload.drawRecordIndices) {
				TombstoneDrawRecord(drawRecordIndex);
			}
		}
	}

	m_stats.bulkRemovePageDeallocUs += pageDeallocUs;
	m_stats.bulkRemoveCollectUs += collectUs;
	TracyPlot("ObjectManager.RemoveObjectsBulk.ActiveBuckets", static_cast<int64_t>(activeDrawSetRemoves.size()));

	for (const auto& [workloadKey, indices] : activeDrawSetRemoves) {
		if (indices.empty()) {
			continue;
		}
		auto activeDrawSetIt = m_activeDrawSetIndices.find(workloadKey);
		if (activeDrawSetIt == m_activeDrawSetIndices.end() || !activeDrawSetIt->second) {
			spdlog::warn(
				"ObjectManager::RemoveObjectsBulk: missing active draw set while removing {} indices flags={} phase={} clodOnly={}",
				indices.size(),
				static_cast<std::uint64_t>(workloadKey.compileFlags),
				workloadKey.renderPhase.hash,
				workloadKey.clodOnly);
			continue;
		}
		ZoneScopedN("ObjectManager::RemoveStaticObjectsBulk::ActiveDrawSetTombstone");
		ZoneValue(indices.size());
		const auto activeRemoveBegin = std::chrono::steady_clock::now();
		const auto currentLive = activeDrawSetIt->second->LiveSize();
		activeDrawSetIt->second->SetLiveSize(currentLive > indices.size() ? currentLive - indices.size() : 0u);
		MaybeQueueActiveDrawSetCompaction(workloadKey, activeDrawSetIt->second);
		const auto activeRemoveEnd = std::chrono::steady_clock::now();
		m_stats.activeDrawSetRemoveCalls += 1;
		m_stats.activeDrawSetRemoveIndices += indices.size();
		m_stats.activeDrawSetRemoveUs += static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::microseconds>(activeRemoveEnd - activeRemoveBegin).count());
	}

	m_stats.bulkRemoveUs += static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - removeBegin).count());
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
