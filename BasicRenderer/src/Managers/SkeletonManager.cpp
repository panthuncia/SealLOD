#include "Managers/SkeletonManager.h"

#include <BasicTelemetry/Telemetry.h>
#include <optional>

#include "Animation/Skeleton.h"
#include "Resources/Buffers/BufferView.h"
#include "Render/Runtime/IUploadService.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "../../generated/BuiltinResources.h"
#include "Resources/Buffers/DynamicStructuredBuffer.h"
#include "Managers/Singletons/TaskSchedulerManager.h"
#include "Resources/Resolvers/PublishedStateResourceResolver.h"
#include "Render/PoseStateArtifacts.h"

#include <algorithm>
#include <DirectXMath.h>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

struct MatrixUploadSpan {
    size_t offsetBytes = 0;
    const DirectX::XMMATRIX* data = nullptr;
    uint32_t matrixCount = 0;
};

void DispatchUpload(org::runtime::IUploadService& uploadService, const void* data, size_t size,
    org::runtime::UploadTarget target, size_t offset, const char* file, int line) {
#if BUILD_TYPE == BUILD_TYPE_DEBUG
    uploadService.UploadData(data, size, std::move(target), offset, file, line);
#else
    (void)file;
    (void)line;
    uploadService.UploadData(data, size, std::move(target), offset);
#endif
}

void UploadMatrixSpans(org::runtime::IUploadService& uploadService,
    const std::shared_ptr<org::DynamicBuffer>& target, std::vector<MatrixUploadSpan>& spans,
    std::vector<org::runtime::UploadRegion>& regions) {
    std::erase_if(spans, [](const MatrixUploadSpan& span) {
        return span.data == nullptr || span.matrixCount == 0;
    });
    if (spans.empty()) {
        return;
    }

    std::sort(spans.begin(), spans.end(), [](const MatrixUploadSpan& a, const MatrixUploadSpan& b) {
        return a.offsetBytes < b.offsetBytes;
    });

    // One batched upload per palette buffer: the upload service merges
    // contiguous regions into single queue entries and shares one heap
    // allocation and map for the whole batch, so no CPU-side staging copy.
    regions.clear();
    regions.reserve(spans.size());
    for (const auto& span : spans) {
        regions.push_back({ span.data,
            static_cast<size_t>(span.matrixCount) * sizeof(DirectX::XMMATRIX),
            span.offsetBytes });
    }
#if BUILD_TYPE == BUILD_TYPE_DEBUG
    uploadService.UploadDataBatch(org::runtime::UploadTarget::FromShared(target), regions, __FILE__, __LINE__);
#else
    uploadService.UploadDataBatch(org::runtime::UploadTarget::FromShared(target), regions);
#endif
}

} // namespace

static uint32_t BytesToMatrixIndex(size_t byteOffset) {
    return static_cast<uint32_t>(byteOffset / sizeof(DirectX::XMMATRIX));
}

SkeletonManager::SkeletonManager(std::shared_ptr<org::runtime::IUploadService> uploadService,
    uint32_t transientWindMatrixCapacity)
    : m_uploadService(std::move(uploadService)) {
    m_lifetimeToken = std::make_shared<std::atomic_bool>(true);
    // Palette outputs are frame-written GPU resources. Give their backing its final
    // configured size before the first frame snapshot can select it; growing these
    // resources while a frame is executing would mutate renderer-visible backing
    // outside publication. Immutable inverse-bind input takes the versioned path.
    constexpr uint32_t staticPaletteHeadroom = 32768u;
    const auto paletteCapacity = (std::max)(transientWindMatrixCapacity, 1u);
    m_inverseBindMatrices = org::DynamicBuffer::CreateShared(sizeof(DirectX::XMMATRIX), 1, "InverseBindMatricesPacked");
    m_inverseBindMatrices->EnableVersionedGraphJournal();
    m_inverseBindMatrices->SetVersionedGraphExclusive(true);
    m_boneTransforms = org::DynamicBuffer::CreateShared(sizeof(DirectX::XMMATRIX),
        static_cast<size_t>(paletteCapacity) * 2u + staticPaletteHeadroom * 2u,
        "BoneSkinMatricesPacked", false, true);
    // TODO: This only exists to project skinned voxel samples back to object-space for voxel sample reconstruction.
    // Maybe we could avoid this if we changed the normal skinning path as well?
    m_inverseSkinMatrices = org::DynamicBuffer::CreateShared(sizeof(DirectX::XMMATRIX),
        static_cast<size_t>(paletteCapacity) + staticPaletteHeadroom,
        "InverseSkinMatricesPacked", false, true);

    const auto instanceCapacity = static_cast<uint64_t>(kProceduralWindTransientSlotBase) + paletteCapacity;
    m_instanceInfo = DynamicStructuredBuffer<SkinningInstanceGPUInfo>::CreateShared(
        static_cast<uint32_t>((std::min)(instanceCapacity,
            static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)()))),
        "SkinningInstanceInfo", true);

    org::memory::SetResourceUsageHint(*m_inverseBindMatrices, "Skinning data");
    org::memory::SetResourceUsageHint(*m_boneTransforms, "Skinning data");
    org::memory::SetResourceUsageHint(*m_inverseSkinMatrices, "Skinning data");
    org::memory::SetResourceUsageHint(*m_instanceInfo, "Skinning data");

    // Expose via resource provider keys
    m_resources[Builtin::SkeletonResources::InverseBindMatrices] = m_inverseBindMatrices;
    m_resources[Builtin::SkeletonResources::BoneTransforms] = m_boneTransforms;
    m_resources[Builtin::SkeletonResources::InverseSkinMatrices] = m_inverseSkinMatrices;
    m_resources[Builtin::SkeletonResources::SkinningInstanceInfo] = m_instanceInfo;
    const auto source = br::render::PublishedStateSource::ProcessSource();
    const auto addPublished = [&](org::ResourceIdentifier key, std::shared_ptr<org::Resource> fallback,
        std::uint64_t variant) {
        m_resolvers[key] = std::make_shared<PublishedStateResourceResolver>(source,
            br::render::PublishedResourceKey{ br::render::PublishedFragmentKind::Poses,
                br::render::PublishedResourceUsage::ShaderResource, 0, 0, variant },
            std::move(fallback));
    };
    addPublished(Builtin::SkeletonResources::InverseBindMatrices, m_inverseBindMatrices,
        br::render::PoseInverseBindTableVariant);
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

std::vector<SkeletonManager::ActiveInstanceView> SkeletonManager::GetActiveInstanceViews() const {
    std::vector<ActiveInstanceView> result;
    result.reserve(m_instances.size());
    for (const auto& [skeleton, record] : m_instances) {
        result.push_back({
            const_cast<Skeleton*>(skeleton),
            record.instanceSlot,
            record.transformOffsetMatrices,
            record.inverseSkinOffsetMatrices,
            record.boneCount
        });
    }
    std::ranges::sort(result, {}, &ActiveInstanceView::instanceSlot);
    return result;
}

SkeletonManager::TransientWindRegion SkeletonManager::ReserveTransientWindRegion(uint32_t matrixCapacity) {
	if (m_transientWindRegion.valid || matrixCapacity == 0u) {
		return m_transientWindRegion;
	}
	const size_t bytes = static_cast<size_t>(matrixCapacity) * sizeof(DirectX::XMMATRIX);
	// Keep two equal forward-skin regions in one packed resource. Wind writes the
	// current half while motion-vector reconstruction reads the untouched half
	// produced by the previous frame.
	m_transientWindTransformsView = m_boneTransforms->Allocate(bytes * 2u, sizeof(DirectX::XMMATRIX));
	m_transientWindInverseSkinView = m_inverseSkinMatrices->Allocate(bytes, sizeof(DirectX::XMMATRIX));
	if (!m_transientWindTransformsView || !m_transientWindInverseSkinView) {
		m_transientWindTransformsView.reset();
		m_transientWindInverseSkinView.reset();
		return {};
	}
	m_transientWindAllocationBaseMatrices = BytesToMatrixIndex(m_transientWindTransformsView->GetOffset());
	const uint32_t currentIndex = static_cast<uint32_t>(m_lastBegunFrame == std::numeric_limits<uint64_t>::max()
		? 0u
		: (m_lastBegunFrame & 1u));
	m_transientWindRegion.transformBaseMatrices = m_transientWindAllocationBaseMatrices + currentIndex * matrixCapacity;
	m_transientWindRegion.previousTransformBaseMatrices =
		m_transientWindAllocationBaseMatrices + (1u - currentIndex) * matrixCapacity;
	m_transientWindRegion.inverseSkinBaseMatrices = BytesToMatrixIndex(m_transientWindInverseSkinView->GetOffset());
	m_transientWindRegion.capacityMatrices = matrixCapacity;
	m_transientWindRegion.valid = true;
	return m_transientWindRegion;
}

void SkeletonManager::BeginFrame(uint64_t frameNumber) {
	if (m_lastBegunFrame == frameNumber) {
		return;
	}
	BT_ZONE_SCOPE("SkeletonManager::BeginFrame");
	BT_ZONE_VALUE(static_cast<int64_t>(m_uploadedLastFrame.size()));
	m_lastBegunFrame = frameNumber;

	// A CPU palette only gains history when a new pose is uploaded below. Until
	// then previous == current, preventing a stopped animation from emitting the
	// same motion vector repeatedly. Only instances uploaded last frame can have
	// previous != current, so only they are rewritten.
	for (const Skeleton* skeleton : m_uploadedLastFrame) {
		auto found = m_instances.find(skeleton);
		if (found == m_instances.end()) continue;
		auto& rec = found->second;
		rec.previousTransformOffsetMatrices = rec.transformOffsetMatrices;
		SkinningInstanceGPUInfo info{};
		info.transformOffsetMatrices = rec.transformOffsetMatrices;
		info.invBindOffsetMatrices = rec.invBindOffsetMatrices;
		info.inverseSkinOffsetMatrices = rec.inverseSkinOffsetMatrices;
		info.boneCount = rec.boneCount;
		info.sourceBoneCount = rec.boneCount;
		info.flags = rec.base->GetSkinningGPUFlags() | kSkinningInstanceFlagRowVectorSkinMatrix;
		if (rec.base->HasWindSimulationGroups()) {
			info.flags |= kSkinningInstanceFlagProceduralWindType;
			info.pad0 = rec.instanceSlot;
		}
		info.previousTransformOffsetMatrices = rec.previousTransformOffsetMatrices;
		m_instanceInfo->UpdateAt(rec.instanceSlot, info);
	}
	m_uploadedLastFrame.clear();

	if (m_transientWindRegion.valid) {
		const uint32_t currentIndex = static_cast<uint32_t>(frameNumber & 1u);
		m_transientWindRegion.transformBaseMatrices =
			m_transientWindAllocationBaseMatrices + currentIndex * m_transientWindRegion.capacityMatrices;
		m_transientWindRegion.previousTransformBaseMatrices =
			m_transientWindAllocationBaseMatrices + (1u - currentIndex) * m_transientWindRegion.capacityMatrices;
	}
}

void SkeletonManager::EnsureTransientWindInstanceSlots(uint32_t drawRecordCapacity) {
	const uint64_t required = static_cast<uint64_t>(kProceduralWindTransientSlotBase) + drawRecordCapacity;
	if (required <= std::numeric_limits<uint32_t>::max()) {
		m_instanceInfo->EnsureSize(static_cast<uint32_t>(required));
	}
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
    rec.transformsView = m_boneTransforms->Allocate(bytes * 2u, sizeof(DirectX::XMMATRIX));
    rec.transformOffsetsMatrices[0] = BytesToMatrixIndex(rec.transformsView->GetOffset());
    rec.transformOffsetsMatrices[1] = rec.transformOffsetsMatrices[0] + rec.boneCount;
    rec.transformOffsetMatrices = rec.transformOffsetsMatrices[0];
    rec.previousTransformOffsetMatrices = rec.transformOffsetMatrices;
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
	info.sourceBoneCount = rec.boneCount;
    // Retained for cache/metadata compatibility. Shader-visible skin palettes now
    // use one canonical row-vector layout and never branch on this flag.
    info.flags = baseShared->GetSkinningGPUFlags() | kSkinningInstanceFlagRowVectorSkinMatrix;
	info.previousTransformOffsetMatrices = rec.previousTransformOffsetMatrices;
	if (baseShared->HasWindSimulationGroups()) {
		info.flags |= kSkinningInstanceFlagProceduralWindType;
		info.pad0 = rec.instanceSlot; // Wind type IDs are stable persistent assembly slots.
		spdlog::info(
			"SkeletonManager: registered procedural-wind assembly type slot={} bones={} flags=0x{:X} profile='{}'",
			rec.instanceSlot,
			rec.boneCount,
			info.flags,
			baseShared->GetWindProfileIdentity());
	}

    m_instanceInfo->UpdateAt(rec.instanceSlot, info);

    // Store slot on the instance so renderables can grab it without querying manager
    skinningInstance->SetSkinningInstanceSlot(rec.instanceSlot);

    auto [insIt, _ok] = m_instances.emplace(skinningInstance.get(), std::move(rec));
	++m_activeInstanceRevision;
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
	++m_activeInstanceRevision;
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
        // Row-vector skinning is: bindPosition * inverseBind * animatedGlobal.
        const DirectX::XMMATRIX skinMatrix =
            DirectX::XMMatrixMultiply(inverseBindMatrices[boneIndex], boneMatrices[boneIndex]);
        // Convert once at the CPU upload boundary. StructuredBuffer<row_major matrix>
        // then exposes the intended row-vector matrix directly to every shader.
        skinMatrices[boneIndex] = DirectX::XMMatrixTranspose(skinMatrix);
        inverseSkinMatrices[boneIndex] = DirectX::XMMatrixTranspose(
            DirectX::XMMatrixInverse(nullptr, skinMatrix));
    }
	if (rec.hasTransformHistory) {
		rec.previousTransformOffsetMatrices = rec.transformOffsetMatrices;
		rec.currentTransformIndex ^= 1u;
	}
	rec.transformOffsetMatrices = rec.transformOffsetsMatrices[rec.currentTransformIndex];
    DispatchUpload(UploadService(), skinMatrices.data(), bytes,
        org::runtime::UploadTarget::FromShared(m_boneTransforms),
		static_cast<size_t>(rec.transformOffsetMatrices) * sizeof(DirectX::XMMATRIX), __FILE__, __LINE__);
    DispatchUpload(UploadService(), inverseSkinMatrices.data(), bytes,
        org::runtime::UploadTarget::FromShared(m_inverseSkinMatrices),
        rec.inverseSkinView->GetOffset(), __FILE__, __LINE__);
	SkinningInstanceGPUInfo info = (*m_instanceInfo)[rec.instanceSlot];
	info.transformOffsetMatrices = rec.transformOffsetMatrices;
	info.previousTransformOffsetMatrices = rec.hasTransformHistory
		? rec.previousTransformOffsetMatrices
		: rec.transformOffsetMatrices;
	m_instanceInfo->UpdateAt(rec.instanceSlot, info);
	rec.hasTransformHistory = true;

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
    BT_ZONE_SCOPE("SkeletonManager::TickAnimations");
    if (m_iterationListDirty) {
        RebuildIterationList();
    }

    // Externally posed skeletons (every SARP actor) are advanced by the scene
    // bridge; only clip-driven skeletons need a tick. Dirty state comes from
    // SetExternalPose / UpdateTransforms themselves, never from ticking.
    m_animatedScratch.clear();
    for (auto& entry : m_iterationList) {
        if (!entry.skeleton->HasExternalPose()) m_animatedScratch.push_back(entry.skeleton);
    }
    if (m_animatedScratch.empty()) return;
    TaskSchedulerManager::GetInstance().ParallelFor("SkeletonTick", m_animatedScratch.size(),
        [this, elapsedSeconds](size_t i) {
            m_animatedScratch[i]->UpdateTransforms(elapsedSeconds);
        });
}

void SkeletonManager::UpdateAllDirtyInstances() {
    BT_ZONE_SCOPE("SkeletonManager::UpdateAllDirtyInstances");
    if (m_iterationListDirty) {
        RebuildIterationList();
    }

    struct PendingSkeletonUpload {
        Skeleton* skeleton = nullptr;
        InstanceRecord* record = nullptr;
        size_t scratchOffset = 0; // into m_skinScratch / m_inverseSkinScratch
    };

    std::vector<PendingSkeletonUpload> pending;
    pending.reserve(m_iterationList.size());
    size_t totalBones = 0;
    for (auto& entry : m_iterationList) {
        if (!entry.record->transformsView) {
            continue;
        }
        if (entry.record->dirty || entry.skeleton->IsPoseDirty()) {
            pending.push_back({ entry.skeleton, entry.record, totalBones });
            totalBones += entry.record->boneCount;
        }
    }

    if (pending.empty()) {
        return;
    }
    BT_ZONE_VALUE(static_cast<int64_t>(pending.size()));
    std::optional<basic_telemetry::Scope> phaseScope;
    static const basic_telemetry::Callsite skinCallsite("SkeletonManager::UpdateAllDirtyInstances::Skin");
    static const basic_telemetry::Callsite uploadCallsite("SkeletonManager::UpdateAllDirtyInstances::Upload");
    static const basic_telemetry::Callsite infoCallsite("SkeletonManager::UpdateAllDirtyInstances::InstanceInfo");
    phaseScope.emplace(skinCallsite);

    // One arena for every palette built this frame; reused across frames.
    if (m_skinScratch.size() < totalBones) m_skinScratch.resize(totalBones);
    if (m_inverseSkinScratch.size() < totalBones) m_inverseSkinScratch.resize(totalBones);
    DirectX::XMMATRIX* const skinScratch = m_skinScratch.data();
    DirectX::XMMATRIX* const inverseSkinScratch = m_inverseSkinScratch.data();
    TaskSchedulerManager::GetInstance().ParallelFor("SkeletonUpload", pending.size(),
        [&pending, skinScratch, inverseSkinScratch](size_t i) {
            auto& upload = pending[i];
            auto& rec = *upload.record;
            auto* skinMatrices = skinScratch + upload.scratchOffset;
            auto* inverseSkinMatrices = inverseSkinScratch + upload.scratchOffset;

            const auto boneMatrices = upload.skeleton->GetBoneMatrices();
            const auto inverseBindMatrices = rec.base->GetInverseBindMatrices();
            for (uint32_t boneIndex = 0; boneIndex < rec.boneCount; ++boneIndex) {
                // Row-vector skinning is: bindPosition * inverseBind * animatedGlobal.
                const DirectX::XMMATRIX skinMatrix =
                    DirectX::XMMatrixMultiply(inverseBindMatrices[boneIndex], boneMatrices[boneIndex]);
                // Match UpdateInstanceTransforms: GPU buffers store matrices in the
                // shader-native row-vector layout, so shader consumers load directly.
                skinMatrices[boneIndex] = DirectX::XMMatrixTranspose(skinMatrix);
                inverseSkinMatrices[boneIndex] = DirectX::XMMatrixTranspose(
                    DirectX::XMMatrixInverse(nullptr, skinMatrix));
            }
        });

    std::vector<MatrixUploadSpan> boneMatrixSpans;
    std::vector<MatrixUploadSpan> inverseSkinSpans;
    boneMatrixSpans.reserve(pending.size());
    inverseSkinSpans.reserve(pending.size());

    for (auto& upload : pending) {
        auto& rec = *upload.record;
		if (rec.hasTransformHistory) {
			rec.previousTransformOffsetMatrices = rec.transformOffsetMatrices;
			rec.currentTransformIndex ^= 1u;
		}
		rec.transformOffsetMatrices = rec.transformOffsetsMatrices[rec.currentTransformIndex];
        boneMatrixSpans.push_back({
			static_cast<size_t>(rec.transformOffsetMatrices) * sizeof(DirectX::XMMATRIX),
            skinScratch + upload.scratchOffset,
            rec.boneCount
        });
        inverseSkinSpans.push_back({
            rec.inverseSkinView->GetOffset(),
            inverseSkinScratch + upload.scratchOffset,
            rec.boneCount
        });
    }

    phaseScope.emplace(uploadCallsite);
    UploadMatrixSpans(UploadService(), m_boneTransforms, boneMatrixSpans, m_uploadRegionScratch);
    UploadMatrixSpans(UploadService(), m_inverseSkinMatrices, inverseSkinSpans, m_uploadRegionScratch);

    phaseScope.emplace(infoCallsite);
    for (auto& upload : pending) {
		auto& rec = *upload.record;
		SkinningInstanceGPUInfo info = (*m_instanceInfo)[rec.instanceSlot];
		info.transformOffsetMatrices = rec.transformOffsetMatrices;
		info.previousTransformOffsetMatrices = rec.hasTransformHistory
			? rec.previousTransformOffsetMatrices
			: rec.transformOffsetMatrices;
		m_instanceInfo->UpdateAt(rec.instanceSlot, info);
		rec.hasTransformHistory = true;
        upload.record->dirty = false;
        upload.skeleton->ClearPoseDirty();
        m_uploadedLastFrame.push_back(upload.skeleton);
    }
}

br::render::VersionedGpuBufferJournal::Capture SkeletonManager::CaptureInverseBindGraphState() const {
    return m_inverseBindMatrices->CaptureVersionedGraphState();
}

void SkeletonManager::AcknowledgeInverseBindGraphState(
    const std::shared_ptr<const br::render::PublishedGpuBufferVersion>& version) {
    if (version) m_inverseBindMatrices->AcknowledgeVersionedGraphState(version);
}

std::shared_ptr<org::Resource> SkeletonManager::ProvideResource(org::ResourceIdentifier const& key) {
    return m_resources[key];
}

std::vector<org::ResourceIdentifier> SkeletonManager::GetSupportedKeys() {
    std::vector<org::ResourceIdentifier> keys;
    keys.reserve(m_resources.size());
    for (auto const& [key, _] : m_resources) keys.push_back(key);
    return keys;
}
std::vector<std::shared_ptr<const std::vector<std::byte>>> SkeletonManager::CapturePoseTableImages() const {
    std::vector<std::shared_ptr<const std::vector<std::byte>>> result;
    result.reserve(4);
    const auto capture = [&](const auto& buffer) {
        result.push_back(std::make_shared<const std::vector<std::byte>>(
            buffer->CaptureCpuShadowBytes()));
    };
    capture(m_inverseBindMatrices);
    capture(m_boneTransforms);
    capture(m_inverseSkinMatrices);
    capture(m_instanceInfo);
    return result;
}

void SkeletonManager::SetUploadService(std::shared_ptr<org::runtime::IUploadService> uploadService) {
    m_uploadService = std::move(uploadService);
}

org::runtime::IUploadService& SkeletonManager::UploadService() const {
    if (!m_uploadService) throw std::runtime_error("SkeletonManager upload service generation is unavailable");
    return *m_uploadService;
}
std::vector<org::ResourceIdentifier> SkeletonManager::GetSupportedResolverKeys() {
    std::vector<org::ResourceIdentifier> keys;
    keys.reserve(m_resolvers.size());
    for (const auto& [key, _] : m_resolvers) keys.push_back(key);
    return keys;
}
std::shared_ptr<org::IResourceResolver> SkeletonManager::ProvideResolver(org::ResourceIdentifier const& key) {
    const auto found = m_resolvers.find(key);
    return found == m_resolvers.end() ? nullptr : found->second;
}
