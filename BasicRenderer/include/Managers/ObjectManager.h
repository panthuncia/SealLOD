#pragma once

#include <memory>
#include <optional>
#include <mutex>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "Resources/Buffers/LazyDynamicStructuredBuffer.h"
#include "Resources/Buffers/DynamicStructuredBuffer.h"
#include "ShaderBuffers.h"
#include "Resources/Buffers/SortedUnsignedIntBuffer.h"
#include "Render/IndirectCommand.h"
#include "Scene/Components.h"
#include "Interfaces/IResourceProvider.h"
#include "Materials/TechniqueDescriptor.h"
#include "Render/Runtime/BufferUploadPolicy.h"

class BufferView;
class DynamicBuffer;

class ObjectManager : public IResourceProvider {
public:
	static std::unique_ptr<ObjectManager> CreateUnique() {
		return std::unique_ptr<ObjectManager>(new ObjectManager());
	}

	struct ObjectBuildInfo {
		PerObjectCB perObjectCB{};
		const Components::MeshInstances* meshInstances = nullptr;
		const Components::InstanceTransforms* instanceTransforms = nullptr;
	};

	Components::ObjectDrawInfo AddObject(const PerObjectCB& perObjectCB, const Components::MeshInstances* meshInstances);
	std::vector<Components::ObjectDrawInfo> AddObjectsBulk(const std::vector<ObjectBuildInfo>& objects);
	void RemoveObject(const Components::ObjectDrawInfo* drawInfo);
	void UpdatePerObjectBuffer(BufferView*, PerObjectCB& data);
	void UpdateNormalMatrixBuffer(BufferView* view, void* data);

	rg::runtime::BulkWriteHandle BeginPerObjectBulkWrite();
	void EndPerObjectBulkWrite(size_t dirtyOffset, size_t dirtySize);
	rg::runtime::BulkWriteHandle BeginNormalMatrixBulkWrite();
	void EndNormalMatrixBulkWrite(size_t dirtyOffset, size_t dirtySize);

	std::shared_ptr<DynamicBuffer>& GetPerObjectBuffers() {
		return m_perObjectBuffers;
	}

	std::shared_ptr<Resource> ProvideResource(ResourceIdentifier const& key) override;
	std::vector<ResourceIdentifier> GetSupportedKeys() override;
	std::shared_ptr<SortedUnsignedIntBuffer> TryGetActiveDrawSetIndices(const DrawWorkloadKey& workloadKey) {
		auto it = m_activeDrawSetIndices.find(workloadKey);
		return it != m_activeDrawSetIndices.end() ? it->second : nullptr;
	}
	std::shared_ptr<SortedUnsignedIntBuffer> GetActiveDrawSetIndices(const DrawWorkloadKey& workloadKey) {
		auto buffer = TryGetActiveDrawSetIndices(workloadKey);
		if (!buffer) {
			throw std::runtime_error("Active draw set indices for given flags not found");
		}
		return buffer;
	}
	std::shared_ptr<SortedUnsignedIntBuffer> GetActiveDrawSetIndices(MaterialCompileFlags flags, const RenderPhase& renderPhase, bool clodOnly = false) {
        return GetActiveDrawSetIndices(DrawWorkloadKey { flags, renderPhase, clodOnly });
    }
    uint64_t GetDrawSetDeclarationRevision() const { return m_drawSetDeclarationRevision; }

private:
	ObjectManager();
	std::unordered_map<ResourceIdentifier, std::shared_ptr<Resource>, ResourceIdentifier::Hasher> m_resources;
	std::shared_ptr<DynamicBuffer> m_perObjectBuffers; // Per object constant buffer
	std::shared_ptr<DynamicBuffer> m_masterIndirectCommandsBuffer; // Indirect draw command buffer
	std::shared_ptr<LazyDynamicStructuredBuffer<DirectX::XMFLOAT4X4>> m_normalMatrixBuffer; // Normal matrices for each object
	std::unordered_map<DrawWorkloadKey, std::shared_ptr<SortedUnsignedIntBuffer>, DrawWorkloadKey::Hasher> m_activeDrawSetIndices; // Indices into m_drawSetCommandsBuffer for active objects per workload
	std::shared_ptr<LazyDynamicStructuredBuffer<PerMeshInstanceCB>> m_perMeshInstanceBuffers; // Indices into m_perObjectBuffers for each mesh instance in each object
    uint64_t m_drawSetDeclarationRevision = 1u;
	std::mutex m_objectUpdateMutex; // Mutex for thread safety
	std::mutex m_normalMatrixUpdateMutex; // Mutex for thread safety

	std::shared_ptr<SortedUnsignedIntBuffer> EnsureActiveDrawSetIndices(const DrawWorkloadKey& workloadKey);
};
