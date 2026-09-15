#include "Mesh/MeshInstance.h"
#include "Managers/MeshManager.h"
#include "Render/PoseInstanceRegistrationService.h"
#include "Materials/Material.h"

#include <algorithm>

std::shared_ptr<MeshInstance> MeshInstance::CreateFrozenCopy(const MeshInstance& source) {
    auto frozen = std::shared_ptr<MeshInstance>(new MeshInstance(source.m_mesh));
    frozen->m_materialOverride = source.m_materialOverride;
    frozen->m_perMeshInstanceBufferData = source.m_perMeshInstanceBufferData;
    frozen->m_animationSpeed = source.m_animationSpeed;
    if (source.m_skeleton) {
        frozen->m_skeleton = std::make_shared<Skeleton>(*source.m_skeleton);
    } else {
        frozen->m_skeleton.reset();
    }
    return frozen;
}

MeshInstance::~MeshInstance() {
    ReleaseSkinningInstance_();
}

void MeshInstance::InitializeBoundsFromMesh_()
{
    if (m_mesh != nullptr) {
        m_perMeshInstanceBufferData.boundingSphere = m_mesh->GetPerMeshCBData().boundingSphere;
    }
}

void MeshInstance::ReleaseSkinningInstance_() {
    if (m_poseRegistration == nullptr || m_skeleton == nullptr) {
        return;
    }

    auto lifetime = m_poseRegistrationLifetime.lock();
    if (!lifetime || !lifetime->load(std::memory_order_acquire)) {
        m_poseRegistration = nullptr;
        m_poseRegistrationLifetime.reset();
        return;
    }

    if (m_skeleton->GetSkinningInstanceSlot() == 0xFFFFFFFFu) {
        return;
    }

    m_poseRegistration->Release(m_skeleton.get());
}

void MeshInstance::SetPoseRegistrationService(br::render::PoseInstanceRegistrationService* service) {
    m_poseRegistration = service;
    if (service != nullptr) {
        m_poseRegistrationLifetime = service->GetLifetimeToken();
    }
    else {
        m_poseRegistrationLifetime.reset();
    }
}

std::shared_ptr<Material> MeshInstance::GetEffectiveMaterial() const {
    if (m_materialOverride) {
        return m_materialOverride;
    }
    return m_mesh ? m_mesh->material : nullptr;
}

void MeshInstance::SetMaterialOverride(std::shared_ptr<Material> material) {
    m_materialOverride = std::move(material);
}

void MeshInstance::SyncSkinningStateFromSkeleton() {
    if (m_skeleton != nullptr) {
        m_perMeshInstanceBufferData.skinningInstanceSlot = m_skeleton->GetSkinningInstanceSlot();
        m_perMeshInstanceBufferData.skinnedBoundsScale = (std::max)(
            m_mesh->GetSkinnedTraversalBoundsScale(),
            m_skeleton->GetCurrentAnimationConservativeBoundsScale());
        m_perMeshInstanceBufferData.boundingSphere =
            m_mesh->GetAnimatedBoundingSphere(m_skeleton->GetActiveAnimationIndex());
    }
    else {
        m_perMeshInstanceBufferData.skinningInstanceSlot = 0xFFFFFFFF;
        m_perMeshInstanceBufferData.skinnedBoundsScale = m_mesh->GetSkinnedTraversalBoundsScale();
        m_perMeshInstanceBufferData.boundingSphere = m_mesh->GetPerMeshCBData().boundingSphere;
    }

    if (m_pCurrentMeshManager != nullptr && m_perMeshInstanceBufferView != nullptr) {
        m_pCurrentMeshManager->UpdatePerMeshInstanceBuffer(m_perMeshInstanceBufferView, m_perMeshInstanceBufferData);
    }
}

void MeshInstance::SetBufferViews(std::unique_ptr<BufferView> perMeshInstanceBufferView) {
	m_perMeshInstanceBufferView = std::move(perMeshInstanceBufferView);
	if (!m_perMeshInstanceBufferView) {
        return; // nothing to update
    }
    InitializeBoundsFromMesh_();

	if (m_pCurrentMeshManager != nullptr) {
		m_pCurrentMeshManager->UpdatePerMeshInstanceBuffer(m_perMeshInstanceBufferView, m_perMeshInstanceBufferData);
	}
}

void MeshInstance::SetBufferViewUsingBaseMesh(std::unique_ptr<BufferView> perMeshInstanceBufferView) {
	m_perMeshInstanceBufferView = std::move(perMeshInstanceBufferView);
    InitializeBoundsFromMesh_();

	if (m_perMeshInstanceBufferView == nullptr) {
		return; // no need to update
	}

	if (m_pCurrentMeshManager != nullptr) {
		m_pCurrentMeshManager->UpdatePerMeshInstanceBuffer(m_perMeshInstanceBufferView, m_perMeshInstanceBufferData);
	}
}

void MeshInstance::SetSkeleton(std::shared_ptr<Skeleton> skeleton) {
    if (m_skeleton == skeleton) {
        SyncSkinningStateFromSkeleton();
        return;
    }

    ReleaseSkinningInstance_();
	m_skeleton = skeleton;
    if (m_skeleton != nullptr) {
        m_skeleton->SetAnimationSpeed(m_animationSpeed);
        if (m_poseRegistration != nullptr) {
            m_poseRegistration->Acquire(m_skeleton);
            m_perMeshInstanceBufferData.skinningInstanceSlot = m_skeleton->GetSkinningInstanceSlot();
        }
    }
    SyncSkinningStateFromSkeleton();
}

void MeshInstance::SetPerObjectBufferIndex(uint32_t index) {
	m_perMeshInstanceBufferData.perObjectBufferIndex = index;
	if (m_pCurrentMeshManager && m_perMeshInstanceBufferView) {
		m_pCurrentMeshManager->UpdatePerMeshInstanceBuffer(m_perMeshInstanceBufferView, m_perMeshInstanceBufferData);
	}
}
void MeshInstance::SetPerMeshBufferIndex(uint32_t index) {
	m_perMeshInstanceBufferData.perMeshBufferIndex = index;
	if (m_pCurrentMeshManager && m_perMeshInstanceBufferView) {
		m_pCurrentMeshManager->UpdatePerMeshInstanceBuffer(m_perMeshInstanceBufferView, m_perMeshInstanceBufferData);
	}
}
void MeshInstance::SetExpectedClodMeshMetadataIndex(uint32_t index) {
	m_perMeshInstanceBufferData.expectedClodMeshMetadataIndex = index;
	if (m_pCurrentMeshManager && m_perMeshInstanceBufferView) {
		m_pCurrentMeshManager->UpdatePerMeshInstanceBuffer(m_perMeshInstanceBufferView, m_perMeshInstanceBufferData);
	}
}
void MeshInstance::SetExpectedClodMeshIdentity(uint64_t identity) {
	m_perMeshInstanceBufferData.expectedClodMeshIdentityLo = static_cast<uint32_t>(identity);
	m_perMeshInstanceBufferData.expectedClodMeshIdentityHi = static_cast<uint32_t>(identity >> 32u);
	if (m_pCurrentMeshManager && m_perMeshInstanceBufferView) {
		m_pCurrentMeshManager->UpdatePerMeshInstanceBuffer(m_perMeshInstanceBufferView, m_perMeshInstanceBufferData);
	}
}

void MeshInstance::SetSkinningInstanceSlot(uint32_t slot) {
	m_perMeshInstanceBufferData.skinningInstanceSlot = slot;
    if (m_skeleton != nullptr) {
        m_skeleton->SetSkinningInstanceSlot(slot);
    }
	if (m_pCurrentMeshManager && m_perMeshInstanceBufferView) {
		m_pCurrentMeshManager->UpdatePerMeshInstanceBuffer(m_perMeshInstanceBufferView, m_perMeshInstanceBufferData);
	}
}
