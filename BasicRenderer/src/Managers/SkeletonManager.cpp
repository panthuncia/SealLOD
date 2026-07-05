#include "Managers/SkeletonManager.h"

#include "Animation/Skeleton.h"
#include "Resources/Buffers/BufferView.h"
#include "Render/Runtime/UploadServiceAccess.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "../../generated/BuiltinResources.h"
#include "Resources/Buffers/DynamicStructuredBuffer.h"
#include "Managers/Singletons/TaskSchedulerManager.h"

#include <algorithm>
#include <DirectXMath.h>
#include <vector>

namespace {

struct MatrixUploadSpan {
    size_t offsetBytes = 0;
    const DirectX::XMMATRIX* data = nullptr;
    uint32_t matrixCount = 0;
};

void UploadMatrixSpans(const std::shared_ptr<DynamicBuffer>& target, std::vector<MatrixUploadSpan>& spans) {
    std::erase_if(spans, [](const MatrixUploadSpan& span) {
        return span.data == nullptr || span.matrixCount == 0;
    });
    if (spans.empty()) {
        return;
    }

    std::sort(spans.begin(), spans.end(), [](const MatrixUploadSpan& a, const MatrixUploadSpan& b) {
        return a.offsetBytes < b.offsetBytes;
    });

    std::vector<DirectX::XMMATRIX> staging;
    for (size_t groupStart = 0; groupStart < spans.size();) {
        size_t groupEnd = groupStart + 1;
        size_t groupEndOffset = spans[groupStart].offsetBytes +
            static_cast<size_t>(spans[groupStart].matrixCount) * sizeof(DirectX::XMMATRIX);

        while (groupEnd < spans.size() && spans[groupEnd].offsetBytes == groupEndOffset) {
            groupEndOffset += static_cast<size_t>(spans[groupEnd].matrixCount) * sizeof(DirectX::XMMATRIX);
            ++groupEnd;
        }

        const auto& first = spans[groupStart];
        if (groupEnd == groupStart + 1) {
            BUFFER_UPLOAD(first.data,
                static_cast<size_t>(first.matrixCount) * sizeof(DirectX::XMMATRIX),
                rg::runtime::UploadTarget::FromShared(target),
                first.offsetBytes);
        }
        else {
            const size_t matrixCount = (groupEndOffset - first.offsetBytes) / sizeof(DirectX::XMMATRIX);
            staging.clear();
            staging.reserve(matrixCount);
            for (size_t i = groupStart; i < groupEnd; ++i) {
                const auto& span = spans[i];
                staging.insert(staging.end(), span.data, span.data + span.matrixCount);
            }

            BUFFER_UPLOAD(staging.data(),
                staging.size() * sizeof(DirectX::XMMATRIX),
                rg::runtime::UploadTarget::FromShared(target),
                first.offsetBytes);
        }

        groupStart = groupEnd;
    }
}

} // namespace

static uint32_t BytesToMatrixIndex(size_t byteOffset) {
    return static_cast<uint32_t>(byteOffset / sizeof(DirectX::XMMATRIX));
}

SkeletonManager::SkeletonManager() {
    m_lifetimeToken = std::make_shared<std::atomic_bool>(true);
    m_inverseBindMatrices = DynamicBuffer::CreateShared(sizeof(DirectX::XMMATRIX), 1, "InverseBindMatricesPacked");
    m_boneTransforms = DynamicBuffer::CreateShared(sizeof(DirectX::XMMATRIX), 1, "BoneSkinMatricesPacked");
    // TODO: This only exists to project skinned voxel samples back to object-space for voxel sample reconstruction.
    // Maybe we could avoid this if we changed the normal skinning path as well?
    m_inverseSkinMatrices = DynamicBuffer::CreateShared(sizeof(DirectX::XMMATRIX), 1, "InverseSkinMatricesPacked");

    m_instanceInfo = DynamicStructuredBuffer<SkinningInstanceGPUInfo>::CreateShared(64, "SkinningInstanceInfo");

    rg::memory::SetResourceUsageHint(*m_inverseBindMatrices, "Skinning data");
    rg::memory::SetResourceUsageHint(*m_boneTransforms, "Skinning data");
    rg::memory::SetResourceUsageHint(*m_inverseSkinMatrices, "Skinning data");
    rg::memory::SetResourceUsageHint(*m_instanceInfo, "Skinning data");

    // Expose via resource provider keys
    m_resources[Builtin::SkeletonResources::InverseBindMatrices] = m_inverseBindMatrices;
    m_resources[Builtin::SkeletonResources::BoneTransforms] = m_boneTransforms;
    m_resources[Builtin::SkeletonResources::InverseSkinMatrices] = m_inverseSkinMatrices;
    m_resources[Builtin::SkeletonResources::SkinningInstanceInfo] = m_instanceInfo;
}

SkeletonManager::~SkeletonManager() {
    if (m_lifetimeToken) {
        m_lifetimeToken->store(false, std::memory_order_release);
    }
}

SkeletonManager::BaseRecord& SkeletonManager::AcquireBase(const std::shared_ptr<Skeleton>& baseSkeleton) {
    auto it = m_bases.find(baseSkeleton.get());
    if (it != m_bases.end()) {
        it->second.refCount++;
        return it->second;
    }

    BaseRecord rec;
    rec.boneCount = baseSkeleton->GetBoneCount();
    rec.refCount = 1;

    // Allocate + upload inverse binds once
    const size_t bytes = rec.boneCount * sizeof(DirectX::XMMATRIX);
    rec.invBindView = m_inverseBindMatrices->AddData(baseSkeleton->GetInverseBindMatrices().data(), bytes, sizeof(DirectX::XMMATRIX));
    rec.invBindOffsetMatrices = BytesToMatrixIndex(rec.invBindView->GetOffset());

    auto [insIt, _ok] = m_bases.emplace(baseSkeleton.get(), std::move(rec));
    return insIt->second;
}

void SkeletonManager::ReleaseBase(const Skeleton* baseSkeleton) {
    auto it = m_bases.find(baseSkeleton);
    if (it == m_bases.end())
        return;

    auto& rec = it->second;
    if (--rec.refCount > 0)
        return;

    if (rec.invBindView)
        m_inverseBindMatrices->Deallocate(rec.invBindView.get());

    m_bases.erase(it);
}

uint32_t SkeletonManager::AllocateInstanceSlot() {
    if (!m_freeInstanceSlots.empty()) {
        uint32_t slot = m_freeInstanceSlots.back();
        m_freeInstanceSlots.pop_back();
        return slot;
    }
    return m_slotsUsed++; // grows as needed
}

void SkeletonManager::FreeInstanceSlot(uint32_t slot) {
    if (slot != kInvalidSlot)
        m_freeInstanceSlots.push_back(slot);
}

uint32_t SkeletonManager::AcquireSkinningInstance(const std::shared_ptr<Skeleton>& skinningInstance) {
    // Expect: skinningInstance is NOT a base skeleton.
    // It should reference a base skeleton (see "Skeleton type changes" section below).
    auto it = m_instances.find(skinningInstance.get());
    if (it != m_instances.end()) {
        it->second.refCount++;
        return it->second.instanceSlot;
    }

    // Identify base skeleton
    auto baseShared = skinningInstance->GetBaseSkeletonShared();
    auto& baseRec = AcquireBase(baseShared);

    InstanceRecord rec;
    rec.base = baseShared.get();
    rec.boneCount = baseRec.boneCount;
    rec.refCount = 1;
    rec.dirty = true;

    // Allocate transforms region (unique per skinning instance)
    const size_t bytes = rec.boneCount * sizeof(DirectX::XMMATRIX);
    rec.transformsView = m_boneTransforms->Allocate(bytes, sizeof(DirectX::XMMATRIX));
    rec.transformOffsetMatrices = BytesToMatrixIndex(rec.transformsView->GetOffset());
    rec.inverseSkinView = m_inverseSkinMatrices->Allocate(bytes, sizeof(DirectX::XMMATRIX));
    rec.inverseSkinOffsetMatrices = BytesToMatrixIndex(rec.inverseSkinView->GetOffset());
    rec.invBindOffsetMatrices = baseRec.invBindOffsetMatrices;

    // Allocate instance slot and write GPU info
    rec.instanceSlot = AllocateInstanceSlot();
    SkinningInstanceGPUInfo info;
    info.transformOffsetMatrices = rec.transformOffsetMatrices;
    info.invBindOffsetMatrices = rec.invBindOffsetMatrices;
    info.inverseSkinOffsetMatrices = rec.inverseSkinOffsetMatrices;
    info.boneCount = rec.boneCount;
    info.flags = baseShared->GetSkinningGPUFlags();

    m_instanceInfo->UpdateAt(rec.instanceSlot, info);

    // Store slot on the instance so renderables can grab it without querying manager
    skinningInstance->SetSkinningInstanceSlot(rec.instanceSlot);

    auto [insIt, _ok] = m_instances.emplace(skinningInstance.get(), std::move(rec));
    m_iterationListDirty = true;
    return insIt->second.instanceSlot;
}

void SkeletonManager::ReleaseSkinningInstance(Skeleton* skinningInstance) {
    auto it = m_instances.find(skinningInstance);
    if (it == m_instances.end())
        return;

    auto& rec = it->second;
    if (--rec.refCount > 0)
        return;

    if (rec.transformsView)
        m_boneTransforms->Deallocate(rec.transformsView.get());
    if (rec.inverseSkinView)
        m_inverseSkinMatrices->Deallocate(rec.inverseSkinView.get());

    FreeInstanceSlot(rec.instanceSlot);

    // Decrement base usage
    ReleaseBase(rec.base);

    // Clear instance's slot so stale data can't be used accidentally
    skinningInstance->SetSkinningInstanceSlot(kInvalidSlot);

    m_instances.erase(it);
    m_iterationListDirty = true;
}

void SkeletonManager::UpdateInstanceTransforms(Skeleton& inst) {
    auto it = m_instances.find(&inst);
    if (it == m_instances.end())
        return;

    auto& rec = it->second;
    if (!rec.transformsView)
        return;

    const size_t bytes = rec.boneCount * sizeof(DirectX::XMMATRIX);
    std::vector<DirectX::XMMATRIX> skinMatrices(rec.boneCount);
    std::vector<DirectX::XMMATRIX> inverseSkinMatrices(rec.boneCount);
    const auto boneMatrices = inst.GetBoneMatrices();
    const auto inverseBindMatrices = rec.base->GetInverseBindMatrices();
    for (uint32_t boneIndex = 0; boneIndex < rec.boneCount; ++boneIndex) {
        skinMatrices[boneIndex] = DirectX::XMMatrixMultiply(boneMatrices[boneIndex], inverseBindMatrices[boneIndex]);
        inverseSkinMatrices[boneIndex] = DirectX::XMMatrixInverse(
            nullptr,
            skinMatrices[boneIndex]);
    }
    BUFFER_UPLOAD(skinMatrices.data(), bytes,
        rg::runtime::UploadTarget::FromShared(m_boneTransforms),
        rec.transformsView->GetOffset());
    BUFFER_UPLOAD(inverseSkinMatrices.data(), bytes,
        rg::runtime::UploadTarget::FromShared(m_inverseSkinMatrices),
        rec.inverseSkinView->GetOffset());

    rec.dirty = false;
    inst.ClearPoseDirty();
}

void SkeletonManager::RebuildIterationList() {
    m_iterationList.clear();
    m_iterationList.reserve(m_instances.size());
    for (auto& [ptr, rec] : m_instances) {
        m_iterationList.push_back({ const_cast<Skeleton*>(ptr), &rec });
    }
    m_iterationListDirty = false;
}

void SkeletonManager::TickAnimations(float elapsedSeconds) {
    if (m_iterationListDirty) {
        RebuildIterationList();
    }

    TaskSchedulerManager::GetInstance().ParallelFor("SkeletonTick", m_iterationList.size(),
        [this, elapsedSeconds](size_t i) {
            auto& entry = m_iterationList[i];
            entry.skeleton->UpdateTransforms(elapsedSeconds);
            entry.record->dirty = true;
        });
}

void SkeletonManager::UpdateAllDirtyInstances() {
    if (m_iterationListDirty) {
        RebuildIterationList();
    }

    struct PendingSkeletonUpload {
        Skeleton* skeleton = nullptr;
        InstanceRecord* record = nullptr;
        std::vector<DirectX::XMMATRIX> skinMatrices;
        std::vector<DirectX::XMMATRIX> inverseSkinMatrices;
    };

    std::vector<PendingSkeletonUpload> pending;
    pending.reserve(m_iterationList.size());
    for (auto& entry : m_iterationList) {
        if (!entry.record->transformsView) {
            continue;
        }
        if (entry.record->dirty || entry.skeleton->IsPoseDirty()) {
            pending.push_back({ entry.skeleton, entry.record, {}, {} });
        }
    }

    if (pending.empty()) {
        return;
    }

    TaskSchedulerManager::GetInstance().ParallelFor("SkeletonUpload", pending.size(),
        [&pending](size_t i) {
            auto& upload = pending[i];
            auto& rec = *upload.record;
            upload.skinMatrices.resize(rec.boneCount);
            upload.inverseSkinMatrices.resize(rec.boneCount);

            const auto boneMatrices = upload.skeleton->GetBoneMatrices();
            const auto inverseBindMatrices = rec.base->GetInverseBindMatrices();
            for (uint32_t boneIndex = 0; boneIndex < rec.boneCount; ++boneIndex) {
                upload.skinMatrices[boneIndex] = DirectX::XMMatrixMultiply(boneMatrices[boneIndex], inverseBindMatrices[boneIndex]);
                upload.inverseSkinMatrices[boneIndex] = DirectX::XMMatrixInverse(
                    nullptr,
                    upload.skinMatrices[boneIndex]);
            }
        });

    std::vector<MatrixUploadSpan> boneMatrixSpans;
    std::vector<MatrixUploadSpan> inverseSkinSpans;
    boneMatrixSpans.reserve(pending.size());
    inverseSkinSpans.reserve(pending.size());

    for (auto& upload : pending) {
        auto& rec = *upload.record;
        boneMatrixSpans.push_back({
            rec.transformsView->GetOffset(),
            upload.skinMatrices.data(),
            rec.boneCount
        });
        inverseSkinSpans.push_back({
            rec.inverseSkinView->GetOffset(),
            upload.inverseSkinMatrices.data(),
            rec.boneCount
        });
    }

    UploadMatrixSpans(m_boneTransforms, boneMatrixSpans);
    UploadMatrixSpans(m_inverseSkinMatrices, inverseSkinSpans);

    for (auto& upload : pending) {
        upload.record->dirty = false;
        upload.skeleton->ClearPoseDirty();
    }
}

std::shared_ptr<Resource> SkeletonManager::ProvideResource(ResourceIdentifier const& key) {
    return m_resources[key];
}

std::vector<ResourceIdentifier> SkeletonManager::GetSupportedKeys() {
    std::vector<ResourceIdentifier> keys;
    keys.reserve(m_resources.size());
    for (auto const& [key, _] : m_resources) keys.push_back(key);
    return keys;
}
