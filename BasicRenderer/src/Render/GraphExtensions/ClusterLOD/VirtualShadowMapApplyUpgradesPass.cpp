#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapApplyUpgradesPass.h"

#include "BuiltinResources.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/RenderContext.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"
#include <spdlog/spdlog.h>

#include "../shaders/PerPassRootConstants/clodVirtualShadowApplyUpgradesRootConstants.h"

VirtualShadowMapApplyUpgradesPass::VirtualShadowMapApplyUpgradesPass(
    std::shared_ptr<Buffer> inputsBuffer,
    std::shared_ptr<Buffer> inputCountBuffer,
    std::shared_ptr<PixelBuffer> pageTableTexture,
    std::shared_ptr<Buffer> pageMetadataBuffer,
    std::shared_ptr<Buffer> dirtyPageFlagsBuffer,
    std::shared_ptr<Buffer> statsBuffer,
    DrainEventsFn drainEvents,
    QueueStatsFn queueStats)
    : m_inputsBuffer(std::move(inputsBuffer))
    , m_inputCountBuffer(std::move(inputCountBuffer))
    , m_pageTableTexture(std::move(pageTableTexture))
    , m_pageMetadataBuffer(std::move(pageMetadataBuffer))
    , m_dirtyPageFlagsBuffer(std::move(dirtyPageFlagsBuffer))
    , m_statsBuffer(std::move(statsBuffer))
    , m_drainEvents(std::move(drainEvents))
    , m_queueStats(std::move(queueStats))
{
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"CLodVirtualShadowApplyExactUpgradesCSMain",
        {},
        "CLod.VirtualShadow.ApplyUpgrades.PSO");
}

void VirtualShadowMapApplyUpgradesPass::DeclareResourceUsages(ComputePassBuilder* builder)
{
    builder->WithShaderResource(m_inputsBuffer, m_inputCountBuffer)
        .WithUnorderedAccess(m_pageTableTexture, m_pageMetadataBuffer, m_dirtyPageFlagsBuffer, m_statsBuffer)
        .WithConstantBuffer(Builtin::PerFrameBuffer);
}

void VirtualShadowMapApplyUpgradesPass::Setup() {}

void VirtualShadowMapApplyUpgradesPass::Update(const UpdateExecutionContext&)
{
    // Startup graph recompilation can update this pass more than once before
    // it executes. Retain a drained CPU batch until the dispatch is recorded
    // instead of replacing it with an empty second update.
    if (m_pendingBatchDispatched) {
        m_pendingBatchDispatched = false;
        m_pendingInputCount = 0u;
    }
    if (m_pendingInputCount == 0u) {
        std::vector<CLodVirtualShadowUpgradeInvalidationInput> inputs;
        if (m_drainEvents) {
            inputs = m_drainEvents(CLodVirtualShadowMaxInvalidationInputs);
        }
        m_pendingInputCount = static_cast<uint32_t>(inputs.size());
        if (!inputs.empty()) {
            BUFFER_UPLOAD(
                inputs.data(),
                static_cast<uint32_t>(inputs.size() * sizeof(inputs.front())),
                rg::runtime::UploadTarget::FromShared(m_inputsBuffer),
                0);
        }
        BUFFER_UPLOAD(
            &m_pendingInputCount,
            sizeof(m_pendingInputCount),
            rg::runtime::UploadTarget::FromShared(m_inputCountBuffer),
            0);
    }
    if (++m_telemetryFrameCounter >= 120u) {
        m_telemetryFrameCounter = 0u;
        if (m_queueStats) {
            const auto stats = m_queueStats();
            spdlog::info(
                "CLOD VSM exact-upgrade CPU: observed={} deduplicated={} lateResident={} activeGroups={} activePages={} promotions(with={},without={}) events(queued={},uploaded={},backlog={},stale={}) oldestTick={} cleared={}",
                stats.dependenciesObserved,
                stats.dependenciesDeduplicated,
                stats.lateResidentDependencies,
                stats.activeDependencyGroups,
                stats.activeDependencyPairs,
                stats.promotionsWithDependencies,
                stats.promotionsWithoutDependencies,
                stats.eventsQueued,
                stats.eventsUploaded,
                stats.queuedEvents,
                stats.staleEvents,
                stats.oldestQueuedTick,
                stats.clearedDependencies);
        }
    }
}

PassReturn VirtualShadowMapApplyUpgradesPass::Execute(PassExecutionContext& executionContext)
{
    (void)executionContext;
    if (m_pendingInputCount == 0u) {
        return {};
    }
    // The admission pass consumes this uploaded batch immediately before its
    // upgrade phase so application and scheduling share one ordered dispatch.
    m_pendingBatchDispatched = true;
    return {};
}

void VirtualShadowMapApplyUpgradesPass::Cleanup() {}
