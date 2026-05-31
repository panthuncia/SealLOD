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
	m_masterIndirectCommandsBuffer = DynamicBuffer::CreateShared(sizeof(DispatchMeshIndirectCommand), 10000, "masterIndirectCommandsBuffer<IndirectCommand>");

	m_normalMatrixBuffer = LazyDynamicStructuredBuffer<DirectX::XMFLOAT4X4>::CreateShared(10000, "normalMatrixBuffer");

	rg::memory::SetResourceUsageHint(*m_perObjectBuffers, "PerMesh, PerMeshInstance, PerObject");
	rg::memory::SetResourceUsageHint(*m_normalMatrixBuffer, "PerMesh, PerMeshInstance, PerObject");

	rg::memory::SetResourceUsageHint(*m_masterIndirectCommandsBuffer, "Indirect command buffers");

	m_resources[Builtin::PerObjectBuffer] = m_perObjectBuffers;
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

void ObjectManager::BeginAddObjectBatch() {
	BeginAddObjectBatch(0, 0);
}

void ObjectManager::BeginAddObjectBatch(std::size_t expectedObjects, std::size_t expectedDraws) {
	if (m_addObjectBatchDepth == 0u) {
		m_batchedActiveDrawSetInserts.clear();
		if (expectedObjects > 0u) {
			m_perObjectBuffers->ReserveBytes(expectedObjects * sizeof(PerObjectCB));
			const auto normalMatrixTarget = static_cast<uint32_t>(m_normalMatrixBuffer->Size() + expectedObjects);
			m_normalMatrixBuffer->Resize(normalMatrixTarget);
		}
		if (expectedDraws > 0u) {
			m_masterIndirectCommandsBuffer->ReserveBytes(expectedDraws * sizeof(DispatchMeshIndirectCommand));
		}
	}
	++m_addObjectBatchDepth;
}

void ObjectManager::EndAddObjectBatch() {
	if (m_addObjectBatchDepth == 0u) {
		return;
	}
	--m_addObjectBatchDepth;
	if (m_addObjectBatchDepth != 0u) {
		return;
	}

	for (const auto& [workloadKey, indices] : m_batchedActiveDrawSetInserts) {
		if (indices.empty()) {
			continue;
		}
		EnsureActiveDrawSetIndices(workloadKey)->InsertMany(indices);
	}
	m_batchedActiveDrawSetInserts.clear();
}

Components::ObjectDrawInfo ObjectManager::AddObject(const PerObjectCB& perObjectCB, const Components::MeshInstances* meshInstances) {

	Components::ObjectDrawInfo drawInfo;

	std::shared_ptr<BufferView> perObjectCBview = m_perObjectBuffers->AddData(&perObjectCB, sizeof(PerObjectCB), sizeof(PerObjectCB));

	// Patch all mesh instances with their perObject index
	uint32_t perObjectIndex = static_cast<uint32_t>(perObjectCBview->GetOffset() / sizeof(PerObjectCB));
	if (meshInstances) {
		for (auto& inst : meshInstances->meshInstances) {
			inst->SetPerObjectBufferIndex(perObjectIndex);
		}
	}

	if (meshInstances != nullptr) {
		std::vector<unsigned int> indices;
		std::vector<std::shared_ptr<BufferView>> views;
		std::vector<std::vector<DrawWorkloadKey>> drawWorkloadKeysPerDraw;
		// For each mesh, add an indirect command to the draw set buffer
		for (auto& meshInstance : meshInstances->meshInstances) {
			auto& mesh = meshInstance->GetMesh();
			const uint32_t perMeshInstanceBufferIndex = static_cast<uint32_t>(meshInstance->GetPerMeshInstanceBufferOffset() / sizeof(PerMeshInstanceCB));
			DispatchMeshIndirectCommand command = {};
			command.perObjectBufferIndex = static_cast<uint32_t>(perObjectCBview->GetOffset() / sizeof(PerObjectCB));
			command.perMeshBufferIndex = meshInstance->GetPerMeshBufferIndex();
			command.perMeshInstanceBufferIndex = perMeshInstanceBufferIndex;
			command.dispatchMeshArguments.ThreadGroupCountX = 0; //DivRoundUp(mesh->GetMeshletCount(), AS_GROUP_SIZE);
			command.dispatchMeshArguments.ThreadGroupCountY = 1;
			command.dispatchMeshArguments.ThreadGroupCountZ = 1;
			std::shared_ptr<BufferView> view = m_masterIndirectCommandsBuffer->AddData(&command, sizeof(DispatchMeshIndirectCommand), sizeof(DispatchMeshIndirectCommand));
			views.push_back(view);
			unsigned int index = static_cast<uint32_t>(view->GetOffset() / sizeof(DispatchMeshIndirectCommand));
			indices.push_back(index);
			drawInfo.perMeshInstanceBufferIndices.push_back(perMeshInstanceBufferIndex);
            std::vector<DrawWorkloadKey> workloadKeysForDraw;
			auto material = meshInstance->GetEffectiveMaterial();
			if (!material) {
				material = mesh->material;
			}
			ForEachMeshDrawWorkload(*mesh, *material, [&](const DrawWorkloadKey& workloadKey) {
				if (m_addObjectBatchDepth != 0u) {
					EnsureActiveDrawSetIndices(workloadKey);
					m_batchedActiveDrawSetInserts[workloadKey].push_back(index);
				}
				else {
					EnsureActiveDrawSetIndices(workloadKey)->Insert(index);
				}
                workloadKeysForDraw.push_back(workloadKey);
            });
            drawWorkloadKeysPerDraw.push_back(std::move(workloadKeysForDraw));
		}

		Components::IndirectDrawInfo info;
		info.indices = indices;
		info.views = views;
		info.drawWorkloadKeysPerDraw = drawWorkloadKeysPerDraw;
		drawInfo.drawInfo = info;
	}

	auto normalMatrixView = m_normalMatrixBuffer->Add(ComputeNormalMatrixStorage(perObjectCB.modelMatrix));
	drawInfo.normalMatrixView = normalMatrixView;
	drawInfo.perObjectCBView = perObjectCBview;
	drawInfo.normalMatrixIndex = static_cast<uint32_t>(normalMatrixView->GetOffset() / sizeof(DirectX::XMFLOAT4X4));
	drawInfo.perObjectCBIndex = static_cast<uint32_t>(perObjectCBview->GetOffset() / sizeof(PerObjectCB));
	return drawInfo;
}

void ObjectManager::RemoveObject(const Components::ObjectDrawInfo* drawInfo) {
#ifdef _DEBUG
	if (drawInfo == nullptr) {
		throw std::runtime_error("ObjectDrawInfo is null");
		return;
	}
#endif // _DEBUG

	m_perObjectBuffers->Deallocate(drawInfo->perObjectCBView.get());

	// Remove the object's draw set commands from the draw set buffers
	auto& views = drawInfo;
	unsigned int i = 0;
	for (auto view : views->drawInfo.views) {
		unsigned int index = static_cast<uint32_t>(view->GetOffset() / sizeof(DispatchMeshIndirectCommand));
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
		m_masterIndirectCommandsBuffer->Deallocate(view.get());
		++i;
	}

	m_normalMatrixBuffer->Remove(drawInfo->normalMatrixView.get());
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
