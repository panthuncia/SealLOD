#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <span>
#include <cstdint>

#include <flecs.h>
#include <DirectXMath.h>

#include "Animation/Animation.h"
#include "Animation/AnimationController.h"
#include "Animation/DynamicWindMetadata.h"
#include "Animation/SkeletonArtifact.h"
#include "Scene/Components.h"

// Skeleton has two modes:
//  - Base skeleton (asset/template): immutable data shared across instances
//  - Instance skeleton (runtime pose): owns AnimationControllers + pose buffer (bone matrices)
class Skeleton : public std::enable_shared_from_this<Skeleton> {
public:
    using Matrix = DirectX::XMMATRIX;

    // Creates a BASE skeleton from imported bone entities + inverse bind matrices.
    // This constructor extracts the CPU-only skeleton topology and rest pose and does NOT allocate GPU buffers.
    Skeleton(const std::vector<flecs::entity>& nodes,
        const std::vector<Matrix>& inverseBindMatrices);

    Skeleton(std::vector<std::string> boneNames,
        std::vector<int32_t> parentIndices,
        std::vector<Matrix> inverseBindMatrices,
        std::vector<Components::Transform> restLocalTransforms = {},
        std::vector<Matrix> rootParentGlobals = {},
        std::vector<uint32_t> windSimulationGroupIndices = {},
        std::string windProfileIdentity = {},
        DynamicWindMetadata dynamicWindMetadata = {},
        std::vector<uint32_t> evaluationOrder = {},
        std::vector<SkeletonWindBoneInvariant> windBoneInvariants = {},
        std::vector<Matrix> bindGlobalMatrices = {},
        std::vector<SkeletonLodVariant> lodVariants = {});

    // Creates an INSTANCE skeleton referencing an existing base skeleton.
    explicit Skeleton(const std::shared_ptr<Skeleton>& baseSkeleton);

    Skeleton(const Skeleton& other);
    Skeleton& operator=(const Skeleton& other) = delete;

    ~Skeleton() = default;

    // Creates a runtime instance by default. If retainIsBaseSkeleton=true and this is a base,
    // performs a deep copy of base data (You probably don't want this).
    std::shared_ptr<Skeleton> CopySkeleton(bool retainIsBaseSkeleton = false);

    bool IsBaseSkeleton() const noexcept { return m_isBaseSkeleton; }

    // Returns the base skeleton (for instances); returns itself for base skeletons.
    std::shared_ptr<Skeleton> GetBaseSkeletonShared() const;

    // Animation library lives on the BASE skeleton
    void AddAnimation(const std::shared_ptr<Animation>& animation);
    void DeleteAllAnimations();

    // Bind an animation onto this INSTANCE.
    // If called on a BASE skeleton, it logs a warning and does nothing.
    void SetAnimation(size_t index);
    void SetAnimationSpeed(float speed);
    size_t GetAnimationCount() const noexcept;
    size_t GetActiveAnimationIndex() const noexcept;
    float GetCurrentAnimationConservativeBoundsScale() const noexcept;

    // Tick/evaluate pose into the instance-owned pose buffer.
    // Writes directly into m_boneMatrices.
    // If called on a BASE skeleton, it logs a warning and does nothing.
	// Force parameter can be used to force update even if animation is paused.
    void UpdateTransforms(float elapsedSeconds, bool force = false);
    void SetExternalPose(std::span<const Matrix> boneMatrices, float conservativeBoundsScale = 1.0f);
    bool HasExternalPose() const noexcept { return m_externalPose; }

    // Marks pose dirty. SkeletonManager can use this to decide uploads.
    bool IsPoseDirty() const noexcept { return m_poseDirty; }
    void ClearPoseDirty() noexcept { m_poseDirty = false; }

    // Pose buffer (INSTANCE): final bone matrices in skeleton space (global pose).
    std::span<const Matrix> GetBoneMatrices() const noexcept { return m_boneMatrices; }
    std::span<Matrix>       GetBoneMatricesMutable() noexcept { return m_boneMatrices; }

    // Inverse bind matrices (BASE): shared across all instances of this base skeleton.
    std::span<const Matrix> GetInverseBindMatrices() const;

    // Skeleton topology (BASE):
    uint32_t GetBoneCount() const noexcept;
    std::span<const std::string> GetBoneNames() const;
    std::span<const int32_t> GetParentIndices() const;
    std::span<const Matrix> GetRootParentGlobals() const;
    std::span<const Matrix> GetBindGlobalMatrices() const;
    std::span<const uint32_t> GetWindSimulationGroupIndices() const;
    std::string_view GetWindProfileIdentity() const;
    const DynamicWindMetadata& GetDynamicWindMetadata() const;
    std::span<const SkeletonWindBoneInvariant> GetWindBoneInvariants() const;
	std::span<const SkeletonLodVariant> GetSkeletonLodVariants() const;
    bool HasWindSimulationGroups() const { return !GetWindSimulationGroupIndices().empty(); }

    // Optional hook for SkeletonManager (or draw-data) to store an instance slot/index.
    uint32_t GetSkinningInstanceSlot() const noexcept { return m_skinningInstanceSlot; }
    void     SetSkinningInstanceSlot(uint32_t slot) noexcept { m_skinningInstanceSlot = slot; }
    uint32_t GetSkinningGPUFlags() const noexcept;
    void     SetSkinningGPUFlags(uint32_t flags) noexcept { m_skinningGPUFlags = flags; }

private:
    // Shared (BASE) data
    // Valid only when m_isBaseSkeleton==true
    std::vector<std::string> m_boneNames;          // bone i name
    std::vector<int32_t>     m_parentIndices;      // bone i parent index, -1 if root
    //std::vector<Matrix>      m_restLocalMatrices;  // bone i rest local matrix
	std::vector<Components::Transform> m_restLocalTransforms; // Decomposed rest local transforms
    std::vector<uint32_t>    m_evalOrder;          // parent-before-children order
    std::vector<Matrix>      m_inverseBindMatrices;
    std::vector<Matrix>      m_rootParentGlobals; // Transforms to apply to root nodes based on external hierarchy
    std::vector<Matrix>      m_bindGlobalMatrices;
    std::vector<uint32_t>    m_windSimulationGroupIndices;
    std::string              m_windProfileIdentity;
    DynamicWindMetadata      m_dynamicWindMetadata;
    std::vector<SkeletonWindBoneInvariant> m_windBoneInvariants;
	std::vector<SkeletonLodVariant> m_lodVariants;

    // Animation library (base)
public:
    std::vector<std::shared_ptr<Animation>> animations;
    std::unordered_map<std::string, std::shared_ptr<Animation>> animationsByName;
    std::vector<float> m_animationConservativeBoundsScales;

private:
    // Per-instance data
    std::shared_ptr<Skeleton> m_baseSkeleton;          // null for base skeleton
    std::vector<AnimationController> m_controllers;    // one per bone
    std::vector<Matrix> m_boneMatrices;                // final global pose
    bool m_poseDirty = true;
    bool m_externalPose = false;

    float  m_animationSpeed = 1.0f;
    size_t m_activeAnimationIndex = size_t(-1);
    float m_currentAnimationConservativeBoundsScale = 1.0f;

    uint32_t m_skinningInstanceSlot = 0xFFFFFFFF;
    uint32_t m_skinningGPUFlags = 0;

    bool m_isBaseSkeleton = false;

private:
    // Helpers
    void BuildBaseFromNodes_(const std::vector<flecs::entity>& nodes);
    void BuildEvalOrder_();
    void EnsureInstanceBuffersSized_();

    static Matrix ComposeTRS_(const Components::Position& p,
        const Components::Rotation& r,
        const Components::Scale& s);

    static Matrix ComposeTRS_(const Components::Transform& t);
    static float ComputeAnimationConservativeBoundsScale_(const Animation& animation);
};
