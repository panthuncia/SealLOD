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

constexpr size_t kStaticTransformPageElements = 256;
constexpr size_t kStaticDrawRecordPageElements = 1024;

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

std::vector<Components::ObjectDrawInfo::BufferRange> ToBufferRanges(const std::vector<DynamicBuffer::PagedAllocation>& pages) {
	std::vector<Components::ObjectDrawInfo::BufferRange> ranges;
	ranges.reserve(pages.size());
	for (const auto& page : pages) {
		if (page.IsValid()) {
			ranges.push_back(ToBufferRange(page));
		}
	}
	return ranges;
}

size_t PagedElementOffset(
	const std::vector<DynamicBuffer::PagedAllocation>& pages,
	size_t elementIndex,
	size_t pageElementCount)
{
	if (pages.empty() || pageElementCount == 0) {
		return 0;
	}
	const size_t pageIndex = elementIndex / pageElementCount;
	const size_t indexInPage = elementIndex % pageElementCount;
	if (pageIndex >= pages.size()) {
		return 0;
	}
	return pages[pageIndex].offset + indexInPage * pages[pageIndex].stride;
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

}

ObjectManager::ObjectManager() {
	auto& resourceManager = ResourceManager::GetInstance();
	m_perObjectBuffers = DynamicBuffer::CreateShared(sizeof(PerObjectCB), 10000, "perObjectBuffers<PerObjectCB>");
	m_perInstanceTransformBuffers = DynamicBuffer::CreateShared(sizeof(PerInstanceTransformCB), 10000, "perInstanceTransformBuffers<PerInstanceTransformCB>");
	m_instanceDrawRecordBuffers = DynamicBuffer::CreateShared(sizeof(InstanceDrawRecordCB), 10000, "instanceDrawRecordBuffers<InstanceDrawRecordCB>");
	m_masterIndirectCommandsBuffer = DynamicBuffer::CreateShared(sizeof(DispatchMeshIndirectCommand), 10000, "masterIndirectCommandsBuffer<IndirectCommand>");

	m_normalMatrixBuffer = DynamicBuffer::CreateShared(sizeof(DirectX::XMFLOAT4X4), 10000, "normalMatrixBuffer");

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
				AppendActiveDrawSetRemoval(drawInfo, workloadKey, drawRecordIndex);
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

std::vector<Components::ObjectDrawInfo> ObjectManager::AddStaticGroupsBulk(const std::vector<StaticGroupBuildInfo>& groups) {
	std::vector<Components::ObjectDrawInfo> drawInfos;
	if (groups.empty()) {
		return drawInfos;
	}

	const auto importBegin = std::chrono::steady_clock::now();
	++m_stats.staticDirectBulkAddCalls;
	m_stats.staticDirectGroupsSubmitted += groups.size();

	struct GroupTransformRange {
		size_t first = 0;
		size_t count = 0;
	};

	struct PendingDrawRecord {
		size_t groupIndex = 0;
		std::vector<DrawWorkloadKey> workloadKeys;
	};

	drawInfos.resize(groups.size());

	struct ScopeBuild {
		std::uint64_t id = 0;
		std::vector<size_t> groupIndices;
		std::vector<PerObjectCB> perObjectCBs;
		std::vector<DirectX::XMFLOAT4X4> normalMatrices;
		std::vector<DynamicBuffer::PagedAllocation> perObjectPages;
		std::vector<DynamicBuffer::PagedAllocation> instanceTransformPages;
		std::vector<DynamicBuffer::PagedAllocation> normalMatrixPages;
		std::vector<InstanceDrawRecordCB> drawRecords;
		std::vector<PendingDrawRecord> pendingDrawRecords;
		std::vector<DynamicBuffer::PagedAllocation> drawRecordPages;
	};

	std::vector<GroupTransformRange> transformRanges(groups.size());
	std::vector<ScopeBuild> scopes;
	std::unordered_map<std::uint64_t, size_t> scopeIndices;
	scopes.reserve(groups.size());

	const auto transformBuildBegin = std::chrono::steady_clock::now();
	size_t expectedDrawRecords = 0;
	for (size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
		const auto& group = groups[groupIndex];
		const std::uint64_t scopeID = group.allocationScopeID != 0 ? group.allocationScopeID : group.stableGroupID;
		auto [scopeIt, inserted] = scopeIndices.emplace(scopeID, scopes.size());
		if (inserted) {
			ScopeBuild scope;
			scope.id = scopeID;
			scopes.push_back(std::move(scope));
		}
		auto& scope = scopes[scopeIt->second];
		scope.groupIndices.push_back(groupIndex);

		GroupTransformRange range;
		range.first = scope.perObjectCBs.size();
		range.count = group.instanceTransforms.size();
		transformRanges[groupIndex] = range;
		expectedDrawRecords += group.instanceTransforms.size() * group.meshTemplates.size();

		for (const auto& matrix : group.instanceTransforms) {
			PerObjectCB perObject{};
			perObject.modelMatrix = matrix;
			perObject.prevModelMatrix = matrix;
			perObject.modelInverseMatrix = DirectX::XMMatrixInverse(nullptr, matrix);
			const auto determinant = DirectX::XMMatrixDeterminant(matrix);
			perObject.objectFlags = (DirectX::XMVectorGetX(determinant) < 0.0f) ? OBJECT_FLAG_REVERSE_WINDING : 0u;
			scope.perObjectCBs.push_back(perObject);
			scope.normalMatrices.push_back(ComputeNormalMatrixStorage(matrix));
		}
	}
	m_stats.staticDirectTransformBuildUs += static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - transformBuildBegin).count());

	size_t totalTransformRows = 0;
	for (const auto& scope : scopes) {
		totalTransformRows += scope.perObjectCBs.size();
	}

	m_stats.staticDirectTransformRows += totalTransformRows;
	m_stats.perObjectRowsAllocated += totalTransformRows;
	m_stats.perInstanceTransformRowsAllocated += totalTransformRows;
	m_stats.normalMatrixRowsAllocated += totalTransformRows;
	for (const auto& group : groups) {
		m_stats.meshTemplateRowsReferenced += group.meshTemplates.size();
	}

	std::uint64_t reserveUs = 0;
	const auto pageUploadBegin = std::chrono::steady_clock::now();
	for (auto& scope : scopes) {
		if (scope.perObjectCBs.empty()) {
			continue;
		}
		scope.normalMatrixPages = m_normalMatrixBuffer->AddDataPaged(
			scope.normalMatrices.data(),
			scope.normalMatrices.size(),
			sizeof(DirectX::XMFLOAT4X4),
			kStaticTransformPageElements);
		for (size_t i = 0; i < scope.perObjectCBs.size(); ++i) {
			scope.perObjectCBs[i].normalMatrixBufferIndex = static_cast<uint32_t>(
				PagedElementOffset(scope.normalMatrixPages, i, kStaticTransformPageElements) / sizeof(DirectX::XMFLOAT4X4));
		}
		scope.perObjectPages = m_perObjectBuffers->AddDataPaged(
			scope.perObjectCBs.data(),
			scope.perObjectCBs.size(),
			sizeof(PerObjectCB),
			kStaticTransformPageElements);
		scope.instanceTransformPages = m_perInstanceTransformBuffers->AddDataPaged(
			scope.perObjectCBs.data(),
			scope.perObjectCBs.size(),
			sizeof(PerInstanceTransformCB),
			kStaticTransformPageElements);
		m_stats.bulkReservedPerObjectBytes += scope.perObjectCBs.size() * sizeof(PerObjectCB);
		m_stats.bulkReservedInstanceTransformBytes += scope.perObjectCBs.size() * sizeof(PerInstanceTransformCB);
		m_stats.bulkReservedNormalMatrixRows += scope.normalMatrices.size();
	}
	m_stats.staticDirectPageUploadUs += static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - pageUploadBegin).count());

	std::unordered_map<DrawWorkloadKey, std::vector<unsigned int>, DrawWorkloadKey::Hasher> activeDrawSetInserts;
	std::vector<std::vector<std::vector<DrawWorkloadKey>>> cachedWorkloadKeysByGroup;
	cachedWorkloadKeysByGroup.reserve(groups.size());

	const auto workloadBuildBegin = std::chrono::steady_clock::now();
	for (const auto& group : groups) {
		auto& cachedGroupWorkloads = cachedWorkloadKeysByGroup.emplace_back();
		cachedGroupWorkloads.reserve(group.meshTemplates.size());
		for (const auto& meshTemplate : group.meshTemplates) {
			auto& workloadKeys = cachedGroupWorkloads.emplace_back();
			if (!meshTemplate.mesh || !meshTemplate.material) {
				continue;
			}
			ForEachMeshDrawWorkload(*meshTemplate.mesh, *meshTemplate.material, [&](const DrawWorkloadKey& workloadKey) {
				workloadKeys.push_back(workloadKey);
			});
			++m_stats.staticDirectWorkloadCacheMisses;
		}
	}
	m_stats.staticDirectWorkloadBuildUs += static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - workloadBuildBegin).count());

	const auto drawRecordBuildBegin = std::chrono::steady_clock::now();
	for (size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
		const auto& group = groups[groupIndex];
		auto& drawInfo = drawInfos[groupIndex];
		const std::uint64_t scopeID = group.allocationScopeID != 0 ? group.allocationScopeID : group.stableGroupID;
		auto scopeIndexIt = scopeIndices.find(scopeID);
		if (scopeIndexIt == scopeIndices.end()) {
			continue;
		}
		auto& scope = scopes[scopeIndexIt->second];
		if (groupIndex >= transformRanges.size()) {
			continue;
		}
		const auto range = transformRanges[groupIndex];
		if (range.count == 0 || group.meshTemplates.empty()) {
			continue;
		}

		drawInfo.perObjectCBRange = {
			PagedElementOffset(scope.perObjectPages, range.first, kStaticTransformPageElements),
			range.count * sizeof(PerObjectCB),
			sizeof(PerObjectCB),
			range.count
		};
		drawInfo.perInstanceTransformRange = {
			PagedElementOffset(scope.instanceTransformPages, range.first, kStaticTransformPageElements),
			range.count * sizeof(PerInstanceTransformCB),
			sizeof(PerInstanceTransformCB),
			range.count
		};
		drawInfo.normalMatrixRange = {
			PagedElementOffset(scope.normalMatrixPages, range.first, kStaticTransformPageElements),
			range.count * sizeof(DirectX::XMFLOAT4X4),
			sizeof(DirectX::XMFLOAT4X4),
			range.count
		};
		drawInfo.perObjectCBIndex = static_cast<uint32_t>(drawInfo.perObjectCBRange.offset / sizeof(PerObjectCB));
		drawInfo.normalMatrixIndex = static_cast<uint32_t>(drawInfo.normalMatrixRange.offset / sizeof(DirectX::XMFLOAT4X4));

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
			const uint32_t instanceTransformIndex = static_cast<uint32_t>(
				PagedElementOffset(scope.instanceTransformPages, transformOrdinal, kStaticTransformPageElements) / sizeof(PerInstanceTransformCB));
			for (size_t meshTemplateIndex = 0; meshTemplateIndex < group.meshTemplates.size(); ++meshTemplateIndex) {
				const auto& meshTemplate = group.meshTemplates[meshTemplateIndex];
				if (!meshTemplate.mesh || !meshTemplate.material) {
					continue;
				}
				InstanceDrawRecordCB drawRecord{};
				drawRecord.meshTemplateIndex = meshTemplate.meshTemplateIndex;
				drawRecord.instanceTransformIndex = instanceTransformIndex;
				drawRecord.clodOffsetIndex = meshTemplate.clodOffsetIndex;
				drawRecord.flags = 0u;
				scope.drawRecords.push_back(drawRecord);

				PendingDrawRecord pending;
				pending.groupIndex = groupIndex;
				if (groupIndex < cachedWorkloadKeysByGroup.size()
					&& meshTemplateIndex < cachedWorkloadKeysByGroup[groupIndex].size()) {
					pending.workloadKeys = cachedWorkloadKeysByGroup[groupIndex][meshTemplateIndex];
					++m_stats.staticDirectWorkloadCacheHits;
				}
				scope.pendingDrawRecords.push_back(std::move(pending));
			}
		}
	}
	m_stats.staticDirectDrawRecordBuildUs += static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - drawRecordBuildBegin).count());

	const auto drawRecordUploadBegin = std::chrono::steady_clock::now();
	for (auto& scope : scopes) {
		if (scope.drawRecords.empty()) {
			continue;
		}
		const auto reserveBegin = std::chrono::steady_clock::now();
		const auto drawRecordBytes = scope.drawRecords.size() * sizeof(InstanceDrawRecordCB);
		m_stats.bulkReservedDrawRecordBytes += drawRecordBytes;
		reserveUs += static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - reserveBegin).count());

		scope.drawRecordPages = m_instanceDrawRecordBuffers->AddDataPaged(
			scope.drawRecords.data(),
			scope.drawRecords.size(),
			sizeof(InstanceDrawRecordCB),
			kStaticDrawRecordPageElements);
		m_stats.instanceDrawRecordsAllocated += scope.drawRecords.size();
		m_stats.staticDirectDrawRecords += scope.drawRecords.size();
		for (size_t drawRecordViewIndex = 0; drawRecordViewIndex < scope.drawRecords.size() && drawRecordViewIndex < scope.pendingDrawRecords.size(); ++drawRecordViewIndex) {
			const auto& pending = scope.pendingDrawRecords[drawRecordViewIndex];
			auto& drawInfo = drawInfos[pending.groupIndex];
			const auto drawRecordOffset = PagedElementOffset(scope.drawRecordPages, drawRecordViewIndex, kStaticDrawRecordPageElements);
			const auto drawRecordIndex = static_cast<unsigned int>(drawRecordOffset / sizeof(InstanceDrawRecordCB));
			if (drawRecordIndex > 0xFFFFFFu) {
				spdlog::warn("ObjectManager::AddStaticGroupsBulk: instance draw record index {} exceeds packed visible-cluster 24-bit capacity", drawRecordIndex);
			}
			m_stats.maxDrawRecordIndex = std::max<std::uint64_t>(m_stats.maxDrawRecordIndex, drawRecordIndex);

			drawInfo.drawInfo.indices.push_back(drawRecordIndex);
			drawInfo.drawInfo.drawWorkloadKeysPerDraw.push_back(pending.workloadKeys);
			drawInfo.instanceDrawRecordIndices.push_back(drawRecordIndex);
			for (const auto& workloadKey : pending.workloadKeys) {
				activeDrawSetInserts[workloadKey].push_back(drawRecordIndex);
				AppendActiveDrawSetRemoval(drawInfo, workloadKey, drawRecordIndex);
			}
		}

		for (const auto groupIndex : scope.groupIndices) {
			auto& drawInfo = drawInfos[groupIndex];
			const auto drawCount = drawInfo.instanceDrawRecordIndices.size();
			if (drawCount == 0) {
				continue;
			}
			drawInfo.instanceDrawRecordRange = {
				static_cast<uint64_t>(drawInfo.instanceDrawRecordIndices.front()) * sizeof(InstanceDrawRecordCB),
				drawCount * sizeof(InstanceDrawRecordCB),
				sizeof(InstanceDrawRecordCB),
				drawCount
			};
		}

		if (!scope.groupIndices.empty()) {
			auto& ownerDrawInfo = drawInfos[scope.groupIndices.front()];
			ownerDrawInfo.ownedPerObjectCBPages = ToBufferRanges(scope.perObjectPages);
			ownerDrawInfo.ownedPerInstanceTransformPages = ToBufferRanges(scope.instanceTransformPages);
			ownerDrawInfo.ownedNormalMatrixPages = ToBufferRanges(scope.normalMatrixPages);
			ownerDrawInfo.ownedInstanceDrawRecordPages = ToBufferRanges(scope.drawRecordPages);
		}
	}
	m_stats.staticDirectDrawRecordUploadUs += static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - drawRecordUploadBegin).count());

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

	const auto finalizeBegin = std::chrono::steady_clock::now();
	for (const auto& drawInfo : drawInfos) {
		if (!drawInfo.instanceDrawRecordIndices.empty()) {
			++m_stats.staticDirectGroupsImported;
		}
	}
	m_stats.staticDirectFinalizeUs += static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - finalizeBegin).count());
	m_stats.staticDirectImportUs += static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - importBegin).count());

	return drawInfos;
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

void ObjectManager::RemoveObjectsBulk(const std::vector<const Components::ObjectDrawInfo*>& drawInfos) {
	if (drawInfos.empty()) {
		return;
	}

	const auto removeBegin = std::chrono::steady_clock::now();
	++m_stats.bulkRemoveCalls;
	m_stats.bulkRemoveObjects += drawInfos.size();

	std::unordered_map<DrawWorkloadKey, std::vector<unsigned int>, DrawWorkloadKey::Hasher> activeDrawSetRemoves;
	std::uint64_t pageDeallocUs = 0;
	std::uint64_t collectUs = 0;
	const auto deallocateOwnedRanges = [&pageDeallocUs](const std::shared_ptr<DynamicBuffer>& buffer, const std::vector<Components::ObjectDrawInfo::BufferRange>& ranges) {
		if (!buffer) {
			return;
		}
		const auto begin = std::chrono::steady_clock::now();
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
		for (const auto& range : sortedRanges) {
			if (range.IsValid()) {
				buffer->DeallocateRange(range.offset, range.size);
			}
		}
		pageDeallocUs += static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count());
	};

	for (const auto* drawInfo : drawInfos) {
		if (!drawInfo) {
			continue;
		}

	if (!drawInfo->ownedPerObjectCBPages.empty()) {
		deallocateOwnedRanges(m_perObjectBuffers, drawInfo->ownedPerObjectCBPages);
	} else if (!drawInfo->perObjectCBViews.empty()) {
		for (const auto& view : drawInfo->perObjectCBViews) {
			m_perObjectBuffers->Deallocate(view.get());
		}
	} else if (drawInfo->perObjectCBView) {
		m_perObjectBuffers->Deallocate(drawInfo->perObjectCBView.get());
	} else if (drawInfo->perObjectCBRange.IsValid() && !drawInfo->instanceDrawRecordViews.empty()) {
		m_perObjectBuffers->DeallocateRange(drawInfo->perObjectCBRange.offset, drawInfo->perObjectCBRange.size);
	}
	if (!drawInfo->ownedPerInstanceTransformPages.empty()) {
		deallocateOwnedRanges(m_perInstanceTransformBuffers, drawInfo->ownedPerInstanceTransformPages);
	} else if (!drawInfo->perInstanceTransformViews.empty()) {
		for (const auto& view : drawInfo->perInstanceTransformViews) {
			m_perInstanceTransformBuffers->Deallocate(view.get());
		}
	} else if (drawInfo->perInstanceTransformRange.IsValid() && !drawInfo->instanceDrawRecordViews.empty()) {
		m_perInstanceTransformBuffers->DeallocateRange(drawInfo->perInstanceTransformRange.offset, drawInfo->perInstanceTransformRange.size);
	}

	auto& views = drawInfo;
	const auto collectBegin = std::chrono::steady_clock::now();
	if (!drawInfo->activeDrawSetRemovals.empty()) {
		for (const auto& bucket : drawInfo->activeDrawSetRemovals) {
			auto& indices = activeDrawSetRemoves[bucket.workloadKey];
			indices.insert(indices.end(), bucket.indices.begin(), bucket.indices.end());
		}
	} else {
		for (size_t i = 0; i < views->drawInfo.indices.size(); ++i) {
			const unsigned int index = views->drawInfo.indices[i];
			if (i >= views->drawInfo.drawWorkloadKeysPerDraw.size()) {
				continue;
			}
			for (const auto& workloadKey : views->drawInfo.drawWorkloadKeysPerDraw[i]) {
				activeDrawSetRemoves[workloadKey].push_back(index);
			}
		}
	}
	collectUs += static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - collectBegin).count());
	if (!drawInfo->ownedInstanceDrawRecordPages.empty()) {
		deallocateOwnedRanges(m_instanceDrawRecordBuffers, drawInfo->ownedInstanceDrawRecordPages);
	} else {
		for (auto view : views->drawInfo.views) {
			m_instanceDrawRecordBuffers->Deallocate(view.get());
		}
		if (drawInfo->instanceDrawRecordRange.IsValid() && drawInfo->drawInfo.views.empty()) {
			m_instanceDrawRecordBuffers->DeallocateRange(drawInfo->instanceDrawRecordRange.offset, drawInfo->instanceDrawRecordRange.size);
		}
	}

	if (!drawInfo->ownedNormalMatrixPages.empty()) {
		deallocateOwnedRanges(m_normalMatrixBuffer, drawInfo->ownedNormalMatrixPages);
	} else if (!drawInfo->normalMatrixViews.empty()) {
		for (const auto& view : drawInfo->normalMatrixViews) {
			m_normalMatrixBuffer->Deallocate(view.get());
		}
	} else if (drawInfo->normalMatrixView) {
		m_normalMatrixBuffer->Deallocate(drawInfo->normalMatrixView.get());
	} else if (drawInfo->normalMatrixRange.IsValid() && !drawInfo->instanceDrawRecordViews.empty()) {
		m_normalMatrixBuffer->DeallocateRange(drawInfo->normalMatrixRange.offset, drawInfo->normalMatrixRange.size);
	}
	}

	m_stats.bulkRemovePageDeallocUs += pageDeallocUs;
	m_stats.bulkRemoveCollectUs += collectUs;

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
		const auto activeRemoveBegin = std::chrono::steady_clock::now();
		activeDrawSetIt->second->RemoveMany(indices);
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
