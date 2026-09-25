#pragma once

#include <atomic>
#include <memory>
#include "Mesh/Mesh.h"
#include "Animation/Skeleton.h"

class Material;
namespace br::render { class PoseInstanceRegistrationService; }

class MeshInstance {
public:

	static std::shared_ptr<MeshInstance> CreateShared(std::shared_ptr<Mesh> mesh) {
		return std::shared_ptr<MeshInstance>(new MeshInstance(mesh));
	}
    // Capture editable instance state while retaining immutable mesh and
    // material artifacts. The copy owns no manager registration or views.
    static std::shared_ptr<MeshInstance> CreateFrozenCopy(const MeshInstance& source);
    static std::unique_ptr<MeshInstance> CreateUnique(std::shared_ptr<Mesh> mesh) {
        return std::unique_ptr<MeshInstance>(new MeshInstance(mesh));
    }

    ~MeshInstance();

	org::BufferView* GetPerMeshInstanceBufferView() { return m_perMeshInstanceBufferView.get(); }

	void SetBufferViews(std::unique_ptr<org::BufferView> perMeshInstanceBufferView);
    void SetBufferViewUsingBaseMesh(std::unique_ptr<org::BufferView> perMeshInstanceBufferView);

    void SetSkeleton(std::shared_ptr<Skeleton> skeleton);
    void SyncSkinningStateFromSkeleton();

    std::shared_ptr<Skeleton> GetSkin() const {
        return m_skeleton;
    }

    std::shared_ptr<Mesh>& GetMesh() {
        return m_mesh;
    }

    std::shared_ptr<Material> GetEffectiveMaterial() const;
    std::shared_ptr<Material> GetMaterialOverride() const { return m_materialOverride; }
    void SetMaterialOverride(std::shared_ptr<Material> material);
    bool HasMaterialOverride() const { return m_materialOverride != nullptr; }

	uint64_t GetPerMeshInstanceBufferOffset() const {
		return m_perMeshInstanceBufferView->GetOffset();
	}

    bool HasSkin() const { return m_skeleton != nullptr; }

    void SetCurrentMeshManager(MeshManager* manager) {
        m_pCurrentMeshManager = manager;
    }

    void SetPoseRegistrationService(br::render::PoseInstanceRegistrationService* service);

	const PerMeshInstanceCB& GetPerMeshInstanceBufferData() const {
		return m_perMeshInstanceBufferData;
	}

	void SetAnimationSpeed(float speed) {
		m_animationSpeed = speed;
		if (m_skeleton != nullptr) {
			m_skeleton->SetAnimationSpeed(speed);
            SyncSkinningStateFromSkeleton();
		}
	}

    void SetPerObjectBufferIndex(uint32_t index);
    void SetPerMeshBufferIndex(uint32_t index);
    void SetExpectedClodMeshMetadataIndex(uint32_t index);
    void SetExpectedClodMeshIdentity(uint64_t identity);
    uint32_t GetPerMeshBufferIndex() const { return m_perMeshInstanceBufferData.perMeshBufferIndex; }
	void SetSkinningInstanceSlot(uint32_t slot);

    std::unique_ptr<org::BufferView>& GetPerMeshOverrideBufferView() { return m_perMeshOverrideBufferView; }
    void SetPerMeshOverrideBufferView(std::unique_ptr<org::BufferView> view) { m_perMeshOverrideBufferView = std::move(view); }

    void SetCLodBufferViews(std::unique_ptr<org::BufferView> perMeshInstanceClodOffsetsView) {
        m_perMeshInstanceClodOffsetsView = std::move(perMeshInstanceClodOffsetsView);
    }


    const org::BufferView* GetCLodOffsetsView() const {
        return m_perMeshInstanceClodOffsetsView.get();
    }

private:
    void ReleaseSkinningInstance_();
    void InitializeBoundsFromMesh_();

    MeshInstance(std::shared_ptr<Mesh> mesh)
        : m_mesh(mesh) {
        InitializeBoundsFromMesh_();
        if (mesh->HasBaseSkin()) {
            SetSkeleton(mesh->GetBaseSkin()->CopySkeleton());
        }
    }
    PerMeshInstanceCB m_perMeshInstanceBufferData = {};
    std::shared_ptr<Mesh> m_mesh;
    std::shared_ptr<Material> m_materialOverride;
    std::shared_ptr<Skeleton> m_skeleton; // Runtime skeleton; may be shared by a skeleton variant set.
    MeshManager* m_pCurrentMeshManager = nullptr;
    br::render::PoseInstanceRegistrationService* m_poseRegistration = nullptr;
    std::weak_ptr<std::atomic_bool> m_poseRegistrationLifetime;
    std::unique_ptr<org::BufferView> m_perMeshInstanceBufferView;
    std::unique_ptr<org::BufferView> m_perMeshOverrideBufferView;

    std::unique_ptr<org::BufferView> m_perMeshInstanceClodOffsetsView = nullptr;

	float m_animationSpeed = 1.0f;
};
