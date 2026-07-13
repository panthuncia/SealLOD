#include "Animation/Skeleton.h"

#include <spdlog/spdlog.h>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <algorithm>
#include <cmath>

#include "Scene/Components.h"

namespace {
    bool MatrixNearlyEqual(DirectX::XMMATRIX lhs, DirectX::XMMATRIX rhs, float tolerance = 1.0e-5f)
    {
        for (uint32_t row = 0; row < 4; ++row) {
            const auto diff = DirectX::XMVectorAbs(DirectX::XMVectorSubtract(lhs.r[row], rhs.r[row]));
            if (DirectX::XMVectorGetX(diff) > tolerance ||
                DirectX::XMVectorGetY(diff) > tolerance ||
                DirectX::XMVectorGetZ(diff) > tolerance ||
                DirectX::XMVectorGetW(diff) > tolerance) {
                return false;
            }
        }
        return true;
    }
}

Skeleton::Matrix Skeleton::ComposeTRS_(const Components::Position& p,
    const Components::Rotation& r,
    const Components::Scale& s)
{
    using namespace DirectX;

    XMMATRIX S = XMMatrixScalingFromVector(s.scale);
    XMMATRIX R = XMMatrixRotationQuaternion(r.rot);
    XMMATRIX T = XMMatrixTranslationFromVector(p.pos);

    return S * R * T;
}

Skeleton::Matrix Skeleton::ComposeTRS_(const Components::Transform& t)
{
    return ComposeTRS_(t.pos, t.rot, t.scale);
}

Skeleton::Skeleton(const std::vector<flecs::entity>& nodes,
    const std::vector<Matrix>& inverseBindMatrices)
{
    m_isBaseSkeleton = true;

    if (nodes.empty()) {
        spdlog::warn("Skeleton: constructed with 0 nodes");
        return;
    }

    if (!inverseBindMatrices.empty() && inverseBindMatrices.size() != nodes.size()) {
        spdlog::warn("Skeleton: inverseBindMatrices size ({}) != nodes size ({})",
            inverseBindMatrices.size(), nodes.size());
    }

    BuildBaseFromNodes_(nodes);

    // Copy inverse binds. SkeletonManager will upload these when base becomes active.
    m_inverseBindMatrices = inverseBindMatrices;

    // Base skeleton could keep an idle pose buffer for debugging,
    // but runtime skinning must use instances.
}

Skeleton::Skeleton(std::vector<std::string> boneNames,
    std::vector<int32_t> parentIndices,
    std::vector<Matrix> inverseBindMatrices,
    std::vector<Components::Transform> restLocalTransforms,
    std::vector<Matrix> rootParentGlobals,
    std::vector<uint32_t> windSimulationGroupIndices,
    std::string windProfileIdentity,
    DynamicWindMetadata dynamicWindMetadata)
{
    m_isBaseSkeleton = true;
    m_boneNames = std::move(boneNames);
    m_parentIndices = std::move(parentIndices);
    if (m_parentIndices.size() != m_boneNames.size()) {
        spdlog::warn("Skeleton: parent index count ({}) != bone name count ({}); treating all bones as roots",
            m_parentIndices.size(),
            m_boneNames.size());
        m_parentIndices.assign(m_boneNames.size(), -1);
    }
    m_restLocalTransforms = std::move(restLocalTransforms);
    if (!m_restLocalTransforms.empty() && m_restLocalTransforms.size() != m_boneNames.size()) {
        spdlog::warn("Skeleton: rest local transform count ({}) != bone name count ({}); using identity rest transforms",
            m_restLocalTransforms.size(),
            m_boneNames.size());
        m_restLocalTransforms.clear();
    }
    if (m_restLocalTransforms.empty()) {
        m_restLocalTransforms.resize(m_boneNames.size());
    }
    m_rootParentGlobals = std::move(rootParentGlobals);
    if (m_rootParentGlobals.size() != m_boneNames.size()) {
        m_rootParentGlobals.assign(m_boneNames.size(), DirectX::XMMatrixIdentity());
    }
    m_inverseBindMatrices = std::move(inverseBindMatrices);
    if (m_inverseBindMatrices.size() != m_boneNames.size()) {
        spdlog::warn("Skeleton: inverse bind count ({}) != bone name count ({}); using identity inverse binds",
            m_inverseBindMatrices.size(),
            m_boneNames.size());
        m_inverseBindMatrices.assign(m_boneNames.size(), DirectX::XMMatrixIdentity());
    }
    m_windSimulationGroupIndices = std::move(windSimulationGroupIndices);
    if (!m_windSimulationGroupIndices.empty() && m_windSimulationGroupIndices.size() != m_boneNames.size()) {
        spdlog::warn("Skeleton: wind simulation group count ({}) != bone name count ({}); disabling wind metadata",
            m_windSimulationGroupIndices.size(), m_boneNames.size());
        m_windSimulationGroupIndices.clear();
    }
    m_windProfileIdentity = std::move(windProfileIdentity);
    m_dynamicWindMetadata = std::move(dynamicWindMetadata);
    BuildEvalOrder_();
}

Skeleton::Skeleton(const std::shared_ptr<Skeleton>& baseSkeleton)
{
    if (!baseSkeleton) {
        spdlog::error("Skeleton(instance): baseSkeleton is null");
        return;
    }
    if (!baseSkeleton->IsBaseSkeleton()) {
        // Allow instance-from-instance by taking its base.
        m_baseSkeleton = baseSkeleton->GetBaseSkeletonShared();
    }
    else {
        m_baseSkeleton = baseSkeleton;
    }

    m_isBaseSkeleton = false;

    EnsureInstanceBuffersSized_();
    UpdateTransforms(0.0f, true);
    // Start at rest pose
    m_poseDirty = true;
}

Skeleton::Skeleton(const Skeleton& other)
{
    // Copy semantics:
    // - If other is base: deep copy base data (rare).
    // - If other is instance: new instance referencing same base, copying playback state.
    if (other.IsBaseSkeleton()) {
        m_isBaseSkeleton = true;
        m_boneNames = other.m_boneNames;
        m_parentIndices = other.m_parentIndices;
        m_restLocalTransforms = other.m_restLocalTransforms;
        m_evalOrder = other.m_evalOrder;
        m_inverseBindMatrices = other.m_inverseBindMatrices;
		m_rootParentGlobals = other.m_rootParentGlobals;
		m_windSimulationGroupIndices = other.m_windSimulationGroupIndices;
		m_windProfileIdentity = other.m_windProfileIdentity;
		m_dynamicWindMetadata = other.m_dynamicWindMetadata;
        m_skinningGPUFlags = other.m_skinningGPUFlags;

        animations = other.animations;
        animationsByName = other.animationsByName;
        m_animationConservativeBoundsScales = other.m_animationConservativeBoundsScales;

        m_poseDirty = true;
        return;
    }

    m_isBaseSkeleton = false;
    m_baseSkeleton = other.GetBaseSkeletonShared();
    m_animationSpeed = other.m_animationSpeed;
    m_activeAnimationIndex = other.m_activeAnimationIndex;
    m_currentAnimationConservativeBoundsScale = other.m_currentAnimationConservativeBoundsScale;
    m_externalPose = other.m_externalPose;
    m_skinningGPUFlags = other.m_skinningGPUFlags;

    EnsureInstanceBuffersSized_();

    // Copy controller state where it makes sense
    m_controllers = other.m_controllers;
    m_boneMatrices = other.m_boneMatrices;
    m_poseDirty = true;
}

std::shared_ptr<Skeleton> Skeleton::CopySkeleton(bool retainIsBaseSkeleton)
{
    if (retainIsBaseSkeleton && IsBaseSkeleton()) {
        // Deep copy base skeleton
        return std::make_shared<Skeleton>(*this);
    }

    // Default: return a runtime instance referencing this base (or this instance's base).
    if (IsBaseSkeleton()) {
        return std::make_shared<Skeleton>(shared_from_this());
    }
    return std::make_shared<Skeleton>(GetBaseSkeletonShared());
}

std::shared_ptr<Skeleton> Skeleton::GetBaseSkeletonShared() const
{
    if (m_isBaseSkeleton) {
        return std::const_pointer_cast<Skeleton>(shared_from_this());
    }
    if (!m_baseSkeleton) {
        // defensive
        spdlog::warn("Skeleton(instance): missing base skeleton pointer");
        return nullptr;
    }
    return m_baseSkeleton;
}

std::span<const uint32_t> Skeleton::GetWindSimulationGroupIndices() const
{
    if (m_isBaseSkeleton) return m_windSimulationGroupIndices;
    return m_baseSkeleton ? std::span<const uint32_t>(m_baseSkeleton->m_windSimulationGroupIndices) : std::span<const uint32_t>{};
}

std::string_view Skeleton::GetWindProfileIdentity() const
{
    if (m_isBaseSkeleton) return m_windProfileIdentity;
    return m_baseSkeleton ? std::string_view(m_baseSkeleton->m_windProfileIdentity) : std::string_view{};
}

const DynamicWindMetadata& Skeleton::GetDynamicWindMetadata() const
{
    if (m_isBaseSkeleton) return m_dynamicWindMetadata;
    static const DynamicWindMetadata empty;
    return m_baseSkeleton ? m_baseSkeleton->m_dynamicWindMetadata : empty;
}

uint32_t Skeleton::GetSkinningGPUFlags() const noexcept
{
    if (m_isBaseSkeleton) {
        return m_skinningGPUFlags;
    }
    return m_baseSkeleton ? m_baseSkeleton->m_skinningGPUFlags : m_skinningGPUFlags;
}

// TODO: Inheritance from external parents currently disabled- is it correct to apply these if the renderable entity is already being scaled based on the same parent?
// TODO: Currently bakes external parent transforms at construction time. Instead, we should pull this from flecs to support animated parents.
// How to handle cases where external parent is deleted? Should entities store weak_ptr to child skeleton to notify it of changes?
void Skeleton::BuildBaseFromNodes_(const std::vector<flecs::entity>& nodes)
{
    using namespace DirectX;

    const size_t n = nodes.size();

    m_boneNames.resize(n);
    m_parentIndices.assign(n, -1);
    m_restLocalTransforms.resize(n);
    m_rootParentGlobals.assign(n, XMMatrixIdentity());
    m_evalOrder.clear();

    // Map flecs entity id -> bone index
    std::unordered_map<uint64_t, uint32_t> idToIndex;
    idToIndex.reserve(n);

    for (uint32_t i = 0; i < (uint32_t)n; ++i) {
        idToIndex[nodes[i].id()] = i;
    }

    auto isBoneEntity = [&](const flecs::entity& ent) -> bool {
        if (!ent.is_valid()) return false;
        return idToIndex.find(ent.id()) != idToIndex.end();
        };

    auto composeEntityLocalTRS = [&](const flecs::entity& ent) -> Matrix {
        Components::Position pos{};
        Components::Rotation rot{};
        Components::Scale    sca{};

        if (ent.has<Components::Position>()) pos = ent.get<Components::Position>();
        if (ent.has<Components::Rotation>()) rot = ent.get<Components::Rotation>();
        if (ent.has<Components::Scale>())    sca = ent.get<Components::Scale>();

        return ComposeTRS_(pos, rot, sca);
        };

    // Next link in the "external parent chain":
    // - returns parent entity if valid and NOT a bone
    // - returns invalid if parent is invalid OR parent is a bone (stop before entering skeleton)
    auto nextExternalParent = [&](const flecs::entity& ent) -> flecs::entity {
        if (!ent.is_valid()) return flecs::entity{};
        flecs::entity p = ent.parent();
        if (!p.is_valid()) return flecs::entity{};
        if (isBoneEntity(p)) return flecs::entity{};
        return p;
        };

    auto computeExternalParentGlobal = [&](flecs::entity parentEnt) -> Matrix {
        if (!parentEnt.is_valid()) return XMMatrixIdentity();
        if (isBoneEntity(parentEnt)) return XMMatrixIdentity();

		// Cycle detection using Floyd's Tortoise and Hare algorithm- maybe overkill but robust
        flecs::entity tort = parentEnt;
        flecs::entity hare = parentEnt;
        flecs::entity meet = flecs::entity{};

        for (;;) {
            tort = nextExternalParent(tort);
            if (!tort.is_valid()) break;

            hare = nextExternalParent(hare);
            if (!hare.is_valid()) break;
            hare = nextExternalParent(hare);
            if (!hare.is_valid()) break;

            if (tort.id() == hare.id()) {
                meet = tort;
                break;
            }
        }

        // If a cycle exists, find the entry point and truncate before entering it
        flecs::entity cycleEntry = flecs::entity{};
        if (meet.is_valid()) {
            flecs::entity a = parentEnt;
            flecs::entity b = meet;
            while (a.is_valid() && b.is_valid() && a.id() != b.id()) {
                a = nextExternalParent(a);
                b = nextExternalParent(b);
            }
            if (a.is_valid() && b.is_valid() && a.id() == b.id()) {
                cycleEntry = a;
                const char* nm = cycleEntry.name();
                spdlog::warn(
                    "Skeleton: cycle detected in external-parent chain at entity '{}' (id={}). "
                    "Truncating chain before cycle.",
                    (nm && nm[0] != '\0') ? nm : "<unnamed>",
                    (uint64_t)cycleEntry.id());
            }
            else {
                spdlog::warn("Skeleton: cycle detected in external-parent chain, but failed to locate cycle entry. Ignoring chain.");
                return XMMatrixIdentity();
            }
        }

        // Collect chain nodes from immediate parent upward, stopping at:
        // - bone boundary (nextExternalParent returns invalid)
        // - invalid
        // - cycle entry (exclusive) if cycle detected
        std::vector<flecs::entity> chain;
        chain.reserve(8);

        flecs::entity cur = parentEnt;
        while (cur.is_valid() && !isBoneEntity(cur)) {
            if (cycleEntry.is_valid() && cur.id() == cycleEntry.id()) {
                break; // truncate before the cycle begins
            }
            chain.push_back(cur);
            cur = nextExternalParent(cur);
        }

        // Compose from outermost -> innermost
        Matrix out = XMMatrixIdentity();
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            const Matrix local = composeEntityLocalTRS(*it);
            out = XMMatrixMultiply(local, out);
        }
        return out;
        };

    // Extract name, parent index, rest local matrix, and root external-parent global
    for (uint32_t i = 0; i < (uint32_t)n; ++i) {
        const flecs::entity& e = nodes[i];

        // Name for animation lookup
        if (e.has<Components::AnimationName>()) {
            m_boneNames[i] = e.get<Components::AnimationName>().name;
        }
        else if (e.name() && e.name()[0] != '\0') {
            m_boneNames[i] = e.name();
        }
        else {
            m_boneNames[i] = "bone_" + std::to_string(i);
        }

        // Parent
        flecs::entity p = e.parent();
        if (p.is_valid()) {
            auto it = idToIndex.find(p.id());
            if (it != idToIndex.end()) {
                m_parentIndices[i] = (int32_t)it->second;
            }
            else {
                // Bone root with external parent chain
                m_rootParentGlobals[i] = computeExternalParentGlobal(p);
            }
        }

        // Rest local TRS (fallback to identity if missing)
        Components::Position pos{};
        Components::Rotation rot{};
        Components::Scale    sca{};

        if (e.has<Components::Position>()) pos = e.get<Components::Position>();
        if (e.has<Components::Rotation>()) rot = e.get<Components::Rotation>();
        if (e.has<Components::Scale>())    sca = e.get<Components::Scale>();

        m_restLocalTransforms[i] = Components::Transform(pos, rot, sca);
    }

    BuildEvalOrder_();
}


void Skeleton::BuildEvalOrder_()
{
    const uint32_t n = (uint32_t)m_parentIndices.size();
    m_evalOrder.clear();
    m_evalOrder.reserve(n);

    // Build children adjacency
    std::vector<std::vector<uint32_t>> children;
    children.resize(n);

    std::vector<uint32_t> roots;
    roots.reserve(n);

    for (uint32_t i = 0; i < n; ++i) {
        int32_t p = m_parentIndices[i];
        if (p < 0) roots.push_back(i);
        else children[(uint32_t)p].push_back(i);
    }

    // BFS (parents before children)
    std::queue<uint32_t> q;
    for (uint32_t r : roots) q.push(r);

    while (!q.empty()) {
        uint32_t cur = q.front();
        q.pop();
        m_evalOrder.push_back(cur);
        for (uint32_t c : children[cur]) q.push(c);
    }

    if (m_evalOrder.size() != n) {
        spdlog::warn("Skeleton: evalOrder size mismatch ({} vs {}) - possible cycle or orphaned bone",
            m_evalOrder.size(), n);
        // Fallback: ensure all bones appear at least once
        std::vector<uint8_t> seen(n, 0);
        for (auto idx : m_evalOrder) if (idx < n) seen[idx] = 1;
        for (uint32_t i = 0; i < n; ++i) if (!seen[i]) m_evalOrder.push_back(i);
    }
}

void Skeleton::AddAnimation(const std::shared_ptr<Animation>& animation)
{
    auto base = GetBaseSkeletonShared();
    if (!base) return;

    if (!base->IsBaseSkeleton()) {
        spdlog::error("Skeleton::AddAnimation: base resolution failed");
        return;
    }

    if (base->animationsByName.find(animation->name) != base->animationsByName.end()) {
        spdlog::error("Duplicate animation names are not allowed in a single skeleton: {}", animation->name);
        return;
    }

    base->animations.push_back(animation);
    base->animationsByName[animation->name] = animation;
    base->m_animationConservativeBoundsScales.push_back(ComputeAnimationConservativeBoundsScale_(*animation));
}

void Skeleton::DeleteAllAnimations()
{
    auto base = GetBaseSkeletonShared();
    if (!base) return;

    base->animations.clear();
    base->animationsByName.clear();
    base->m_animationConservativeBoundsScales.clear();
}

uint32_t Skeleton::GetBoneCount() const noexcept
{
    if (m_isBaseSkeleton) return (uint32_t)m_parentIndices.size();
    auto base = m_baseSkeleton;
    return base ? (uint32_t)base->m_parentIndices.size() : 0u;
}

void Skeleton::EnsureInstanceBuffersSized_()
{
    if (m_isBaseSkeleton) return;

    auto base = GetBaseSkeletonShared();
    if (!base || !base->IsBaseSkeleton()) {
        spdlog::error("Skeleton(instance): invalid base skeleton");
        return;
    }

    const uint32_t boneCount = base->GetBoneCount();
    m_controllers.resize(boneCount);
    m_boneMatrices.resize(boneCount, DirectX::XMMatrixIdentity());

    // Match speed setting
    for (auto& c : m_controllers) {
        c.SetAnimationSpeed(m_animationSpeed);
    }
}

std::span<const Skeleton::Matrix> Skeleton::GetInverseBindMatrices() const
{
    if (m_isBaseSkeleton) {
        return m_inverseBindMatrices;
    }
    if (m_baseSkeleton) {
        return m_baseSkeleton->m_inverseBindMatrices;
    }
    return {};
}

std::span<const std::string> Skeleton::GetBoneNames() const
{
    if (m_isBaseSkeleton) return m_boneNames;
    if (m_baseSkeleton) return m_baseSkeleton->m_boneNames;
    return {};
}

std::span<const int32_t> Skeleton::GetParentIndices() const
{
    if (m_isBaseSkeleton) return m_parentIndices;
    if (m_baseSkeleton) return m_baseSkeleton->m_parentIndices;
    return {};
}

std::span<const Skeleton::Matrix> Skeleton::GetRootParentGlobals() const
{
    if (m_isBaseSkeleton) return m_rootParentGlobals;
    if (m_baseSkeleton) return m_baseSkeleton->m_rootParentGlobals;
    return {};
}

void Skeleton::SetAnimation(size_t index)
{
    if (m_isBaseSkeleton) {
        spdlog::warn("Skeleton::SetAnimation called on base skeleton - ignored");
        return;
    }

    auto base = GetBaseSkeletonShared();
    if (!base || !base->IsBaseSkeleton()) return;

    if (base->animations.size() <= index) {
        spdlog::error("Skeleton::SetAnimation index out of range");
        return;
    }

    EnsureInstanceBuffersSized_();

    auto& anim = base->animations[index];

    // Bind clips by bone name
    const uint32_t boneCount = base->GetBoneCount();
    for (uint32_t i = 0; i < boneCount; ++i) {
        auto& ctrl = m_controllers[i];
        ctrl.reset();
        ctrl.SetAnimationSpeed(m_animationSpeed);

        auto it = anim->nodesMap.find(base->m_boneNames[i]);
        if (it != anim->nodesMap.end()) {
            ctrl.setAnimationClip(it->second);
        }
        else {
            ctrl.setAnimationClip(nullptr); // fall back to rest pose
        }
    }

    m_activeAnimationIndex = index;
    if (index < base->m_animationConservativeBoundsScales.size()) {
        m_currentAnimationConservativeBoundsScale = base->m_animationConservativeBoundsScales[index];
    }
    else {
        m_currentAnimationConservativeBoundsScale = ComputeAnimationConservativeBoundsScale_(*anim);
    }
    m_poseDirty = true;
}

void Skeleton::SetAnimationSpeed(float speed)
{
    m_animationSpeed = speed;

    if (m_isBaseSkeleton) {
        // Base skeleton doesn't run controllers
		spdlog::warn("Skeleton::SetAnimationSpeed called on base skeleton - ignored");
        return;
    }

    for (auto& c : m_controllers) {
        c.SetAnimationSpeed(speed);
    }
    m_poseDirty = true;
}

size_t Skeleton::GetAnimationCount() const noexcept
{
    auto base = GetBaseSkeletonShared();
    if (!base) {
        return 0;
    }

    return base->animations.size();
}

size_t Skeleton::GetActiveAnimationIndex() const noexcept
{
    if (m_isBaseSkeleton) {
        return size_t(-1);
    }

    return m_activeAnimationIndex;
}

float Skeleton::GetCurrentAnimationConservativeBoundsScale() const noexcept
{
    if (HasWindSimulationGroups()) return 1.0f;
    return m_isBaseSkeleton ? 1.0f : m_currentAnimationConservativeBoundsScale;
}

void Skeleton::UpdateTransforms(float elapsedSeconds, bool force)
{
    if (m_isBaseSkeleton) {
        spdlog::warn("Skeleton::UpdateTransforms called on base skeleton - ignored");
        return;
    }

    if (m_externalPose && !force) {
        m_poseDirty = true;
        return;
    }

    auto base = GetBaseSkeletonShared();
    if (!base || !base->IsBaseSkeleton()) return;

    EnsureInstanceBuffersSized_();

    const uint32_t boneCount = base->GetBoneCount();
    if (boneCount == 0) return;

    // Evaluate local -> global in parent-before-children order
    // local = anim local TRS if clip exists else rest local
    // global = local * parentGlobal
    for (uint32_t idx : base->m_evalOrder) {
        if (idx >= boneCount) continue;

        Components::Transform localTrs = (idx < base->m_restLocalTransforms.size())
            ? base->m_restLocalTransforms[idx]
            : Components::Transform{};

        // If a clip is bound, use animated channels
        auto& ctrl = m_controllers[idx];
        if (ctrl.animationClip) {
            const auto& clip = ctrl.animationClip;
            const auto& trs = ctrl.GetUpdatedTransform(elapsedSeconds, force);
            if (!clip->positionKeyframes.empty()) {
                localTrs.pos = trs.pos;
            }
            if (!clip->rotationKeyframes.empty()) {
                localTrs.rot = trs.rot;
            }
            if (!clip->scaleKeyframes.empty()) {
                localTrs.scale = trs.scale;
            }
        }
        Matrix local = ComposeTRS_(localTrs);


        const int32_t p = base->m_parentIndices[idx];
        if (p < 0) {
            Matrix rootParent = DirectX::XMMatrixIdentity();
            if (idx < base->m_rootParentGlobals.size()) {
                rootParent = base->m_rootParentGlobals[idx];
            }
            // Root joints may live under non-bone scene nodes (common in glTF).
            // Those ancestors are part of the joint's bind/global space and must
            // be carried into the runtime bone matrix before inverse binds are applied.
            m_boneMatrices[idx] = DirectX::XMMatrixMultiply(local, rootParent);
        }
        else {
            m_boneMatrices[idx] = DirectX::XMMatrixMultiply(local, m_boneMatrices[(uint32_t)p]);
        }
    }

    m_poseDirty = true;
}

void Skeleton::SetExternalPose(std::span<const Matrix> boneMatrices, float conservativeBoundsScale)
{
    if (m_isBaseSkeleton) {
        spdlog::warn("Skeleton::SetExternalPose called on base skeleton - ignored");
        return;
    }

    EnsureInstanceBuffersSized_();
    const auto copyCount = (std::min)(m_boneMatrices.size(), boneMatrices.size());
    bool changed = !m_externalPose ||
        std::abs(m_currentAnimationConservativeBoundsScale - conservativeBoundsScale) > 1.0e-5f ||
        boneMatrices.size() != m_boneMatrices.size();

    if (!changed) {
        for (size_t i = 0; i < copyCount; ++i) {
            if (!MatrixNearlyEqual(m_boneMatrices[i], boneMatrices[i])) {
                changed = true;
                break;
            }
        }
    }

    if (!changed && copyCount < m_boneMatrices.size()) {
        const auto identity = DirectX::XMMatrixIdentity();
        for (size_t i = copyCount; i < m_boneMatrices.size(); ++i) {
            if (!MatrixNearlyEqual(m_boneMatrices[i], identity)) {
                changed = true;
                break;
            }
        }
    }

    m_externalPose = true;
    if (!changed) {
        return;
    }

    for (size_t i = 0; i < copyCount; ++i) {
        m_boneMatrices[i] = boneMatrices[i];
    }
    for (size_t i = copyCount; i < m_boneMatrices.size(); ++i) {
        m_boneMatrices[i] = DirectX::XMMatrixIdentity();
    }

    m_currentAnimationConservativeBoundsScale = conservativeBoundsScale;
    m_poseDirty = true;
}

float Skeleton::ComputeAnimationConservativeBoundsScale_(const Animation& animation)
{
    float conservativeScale = 1.0f;
    bool foundScaleKey = false;

    for (const auto& [_, clip] : animation.nodesMap) {
        if (!clip) {
            continue;
        }

        for (const auto& keyframe : clip->scaleKeyframes) {
            const float maxAxisScale = std::max({
                std::abs(DirectX::XMVectorGetX(keyframe.value)),
                std::abs(DirectX::XMVectorGetY(keyframe.value)),
                std::abs(DirectX::XMVectorGetZ(keyframe.value))
                });
            if (!foundScaleKey || maxAxisScale > conservativeScale) {
                conservativeScale = maxAxisScale;
                foundScaleKey = true;
            }
        }
    }

    return foundScaleKey ? conservativeScale : 1.0f;
}
