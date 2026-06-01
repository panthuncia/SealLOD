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
#include <spdlog/spdlog.h>

namespace {

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

}

ObjectManager::ObjectManager() {
	auto& resourceManager = ResourceManager::GetInstance();
	m_perObjectBuffers = DynamicBuffer::CreateShared(sizeof(PerObjectCB), 10000, "perObjectBuffers<PerObjectCB>");
	m_perInstanceTransformBuffers = DynamicBuffer::CreateShared(sizeof(PerInstanceTransformCB), 10000, "perInstanceTransformBuffers<PerInstanceTransformCB>");
	m_instanceDrawRecordBuffers = DynamicBuffer::CreateShared(sizeof(InstanceDrawRecordCB), 10000, "instanceDrawRecordBuffers<InstanceDrawRecordCB>");
	m_masterIndirectCommandsBuffer = DynamicBuffer::CreateShared(sizeof(DispatchMeshIndirectCommand), 10000, "masterIndirectCommandsBuffer<IndirectCommand>");

	m_normalMatrixBuffer = LazyDynamicStructuredBuffer<DirectX::XMFLOAT4X4>::CreateShared(10000, "normalMatrixBuffer");

	rg::memory::SetResourceUsageHint(*m_perObjectBuffers, "PerMesh, PerMeshInstance, PerObject");
	rg::memory::SetResourceUsageHint(*m_perInstanceTransformBuffers, "PerMesh, InstanceDrawRecord, PerInstanceTransform");
	rg::memory::SetResourceUsageHint(*m_instanceDrawRecordBuffers, "PerMesh, InstanceDrawRecord, PerInstanceTransform");
	rg::memory::SetResourceUsageHint(*m_normalMatrixBuffer, "PerMesh, PerMeshInstance, PerObject");

	rg::memory::SetResourceUsageHint(*m_masterIndirectCommandsBuffer, "Indirect command buffers");

	m_resources[Builtin::PerObjectBuffer] = m_perObjectBuffers;
	m_resources[Builtin::PerInstanceTransformBuffer] = m_perInstanceTransformBuffers;
	m_resources[Builtin::InstanceDrawRecordBuffer] = m_instanceDrawRecordBuffers;
	m_resources[Builtin::NormalMatrixBuffer] = m_normalMatrixBuffer;
	m_resources[Builtin::IndirectCommandBuffers::Master] = m_masterIndirectCommandsBuffer;
}

std::shared_ptr<SortedUnsignedIntBuffer> ObjectManager::EnsureActiveDrawSetIndices(const DrawWorkloadKey& workloadKey) {
	auto it = m_activeDrawSetIndices.find(workloadKey);
	if (it != m_activeDrawSetIndices.end()) {
		return it->second;
	}

	auto debugName =
		"activeDrawSetIndices(flags=" + std::to_string(static_cast<uint64_t>(workloadKey.compileFlags))
		+ ", phase=" + std::to_string(workloadKey.renderPhase.hash)
		+ ", clodOnly=" + std::to_string(workloadKey.clodOnly ? 1 : 0) + ")";
	auto buffer = SortedUnsignedIntBuffer::CreateShared(1, debugName);
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
		m_perObjectBuffers->ReserveBytes(perObjectBytes);
		m_perInstanceTransformBuffers->ReserveBytes(instanceTransformBytes);
		m_normalMatrixBuffer->ReserveAdditional(normalMatrices.size());
		m_stats.bulkReservedPerObjectBytes += perObjectBytes;
		m_stats.bulkReservedInstanceTransformBytes += instanceTransformBytes;
		m_stats.bulkReservedNormalMatrixRows += normalMatrices.size();
		reserveUs += static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - reserveBegin).count());
	}

	auto normalMatrixViews = m_normalMatrixBuffer->AddMany(normalMatrices.data(), normalMatrices.size());
	for (size_t i = 0; i < perObjectCBs.size() && i < normalMatrixViews.size(); ++i) {
		perObjectCBs[i].normalMatrixBufferIndex = static_cast<uint32_t>(normalMatrixViews[i]->GetOffset() / sizeof(DirectX::XMFLOAT4X4));
	}
	auto perObjectViews = m_perObjectBuffers->AddDataBatch(perObjectCBs.data(), perObjectCBs.size(), sizeof(PerObjectCB));
	auto instanceTransformViews = m_perInstanceTransformBuffers->AddDataBatch(perObjectCBs.data(), perObjectCBs.size(), sizeof(PerInstanceTransformCB));

	std::vector<InstanceDrawRecordCB> drawRecords;
	std::vector<PendingDrawRecord> pendingDrawRecords;
	std::unordered_map<DrawWorkloadKey, std::vector<unsigned int>, DrawWorkloadKey::Hasher> activeDrawSetInserts;

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

			drawInfo.drawInfo.indices.push_back(drawRecordIndex);
			drawInfo.drawInfo.views.push_back(view);
			drawInfo.drawInfo.drawWorkloadKeysPerDraw.push_back(pendingDrawRecord.workloadKeys);
			drawInfo.instanceDrawRecordIndices.push_back(drawRecordIndex);
			drawInfo.instanceDrawRecordViews.push_back(view);
			for (const auto& workloadKey : pendingDrawRecord.workloadKeys) {
				activeDrawSetInserts[workloadKey].push_back(drawRecordIndex);
			}
		}
	}

	++m_stats.bulkReserveCalls;
	m_stats.bulkReserveUs += reserveUs;

	for (const auto& [workloadKey, indices] : activeDrawSetInserts) {
		if (!indices.empty()) {
			const auto insertBegin = std::chrono::steady_clock::now();
			EnsureActiveDrawSetIndices(workloadKey)->InsertMany(indices);
			const auto insertEnd = std::chrono::steady_clock::now();
			m_stats.activeDrawSetInsertCalls += 1;
			m_stats.activeDrawSetInsertIndices += indices.size();
			m_stats.activeDrawSetInsertUs += static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::microseconds>(insertEnd - insertBegin).count());
		}
	}

	return drawInfos;
}

void ObjectManager::RemoveObject(const Components::ObjectDrawInfo* drawInfo) {
#ifdef _DEBUG
	if (drawInfo == nullptr) {
		throw std::runtime_error("ObjectDrawInfo is null");
		return;
	}
#endif // _DEBUG

	if (!drawInfo->perObjectCBViews.empty()) {
		for (const auto& view : drawInfo->perObjectCBViews) {
			m_perObjectBuffers->Deallocate(view.get());
		}
	} else {
		m_perObjectBuffers->Deallocate(drawInfo->perObjectCBView.get());
	}
	for (const auto& view : drawInfo->perInstanceTransformViews) {
		m_perInstanceTransformBuffers->Deallocate(view.get());
	}

	// Remove the object's draw set commands from the draw set buffers
	auto& views = drawInfo;
	unsigned int i = 0;
	for (auto view : views->drawInfo.views) {
		unsigned int index = static_cast<uint32_t>(view->GetOffset() / sizeof(InstanceDrawRecordCB));
        for (const auto& workloadKey : views->drawInfo.drawWorkloadKeysPerDraw[i]) {
            auto activeDrawSetIt = m_activeDrawSetIndices.find(workloadKey);
            if (activeDrawSetIt == m_activeDrawSetIndices.end() || !activeDrawSetIt->second) {
                spdlog::warn(
                    "ObjectManager::RemoveObject: missing active draw set while removing indirect index={} flags={} phase={} clodOnly={}",
                    index,
                    static_cast<std::uint64_t>(workloadKey.compileFlags),
                    workloadKey.renderPhase.hash,
                    workloadKey.clodOnly);
                continue;
            }
		    activeDrawSetIt->second->Remove(index);
        }
		m_instanceDrawRecordBuffers->Deallocate(view.get());
		++i;
	}

	if (!drawInfo->normalMatrixViews.empty()) {
		for (const auto& view : drawInfo->normalMatrixViews) {
			m_normalMatrixBuffer->Remove(view.get());
		}
	} else {
		m_normalMatrixBuffer->Remove(drawInfo->normalMatrixView.get());
	}
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
