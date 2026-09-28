#include "Runtime/StateGraph/RendererGraphAdapters.h"
#include "Runtime/Scheduling/GraphSchedulerAdapter.h"
#include <BasicRenderer/Streaming/StaticSceneArtifacts.h>
#include <BasicRenderer/Streaming/VersionedGpuBuffer.h>
#include "Render/Runtime/StreamingUploadTypes.h"

namespace br::render {

AsyncStateGraphHostHooks::AsyncStateGraphHostHooks()
    : scheduling{static_cast<std::uint32_t>(TaskLane::Count), static_cast<std::uint32_t>(TaskDomain::Count),
        {static_cast<std::uint32_t>(TaskLane::Streaming), static_cast<std::uint32_t>(TaskDomain::GraphControl)},
        {static_cast<std::uint32_t>(TaskLane::Streaming), static_cast<std::uint32_t>(TaskDomain::RendererState)}},
      producerDispatch(scheduling.domainCount, org::async::TaskDispatch::Cpu) {}

AsyncStateGraph::AsyncStateGraph(TaskSchedulerManager& scheduler, std::string_view name)
    : AsyncStateGraph(br::MakeGraphSchedulerAdapter(scheduler), name, MakeRendererGraphHooks()) {}

AsyncStateGraphHostHooks MakeRendererGraphHooks() {
    AsyncStateGraphHostHooks hooks;
    for (auto& policy : hooks.artifactPolicies) policy.pinExactContent = false;
    hooks.artifactPolicies[static_cast<std::size_t>(ArtifactKind::ActiveDrawList)].pinExactContent = true;
    hooks.artifactPolicies[static_cast<std::size_t>(ArtifactKind::StaticTransaction)].allowCoalescing = false;
    hooks.artifactPolicies[static_cast<std::size_t>(ArtifactKind::BufferVersion)].traceResourceLifetime = true;
    // Static-group preparation is capacity-admitted separately from import
    // service drains; other graph producers use direct CPU scheduling.
    hooks.producerDispatch[static_cast<std::size_t>(TaskDomain::StaticGroupPreparation)] = org::async::TaskDispatch::Controlled;
    hooks.createTraceSession = MakeRendererGraphTrace;
    hooks.versionsRetired = [] { NotifyVersionedGpuBufferFrameRetirement(); };
    hooks.traceAcceptedInput = [](AsyncStateGraph& graph, ArtifactKey key,
        std::uint64_t revision, std::uint64_t generation, const ArtifactPayload& input) {
        if (key.kind == ArtifactKind::StaticTransaction) {
            if (const auto transaction = input.Get<StaticTransactionBuildInput>()) {
                graph.TraceEvent(AsyncStateGraphTraceEventID::StaticTransactionContents,
                    key, revision, generation,
                    {{transaction->groupCount, transaction->placementCount,
                        transaction->drawRecordCount, transaction->activeEntryCount,
                        transaction->streamGeneration}});
                for (const auto& group : transaction->groups)
                    graph.TraceEvent(AsyncStateGraphTraceEventID::StaticGroupTransactionLinked,
                        key, revision, generation,
                        {{group.placementCount, group.drawRecordCount, group.activeEntryCount}},
                        {ArtifactKind::StaticGroup, group.groupID, 0}, transaction->streamGeneration);
            }
        } else if (key.kind == ArtifactKind::StaticScene) {
            if (const auto scene = input.Get<StaticSceneBuildInput>())
                graph.TraceEvent(AsyncStateGraphTraceEventID::StaticSceneContents,
                    key, revision, generation,
                    {{scene->pages.size(), scene->desiredPlacementCount,
                        scene->materializedPlacementCount, scene->retiredPlacementCount}});
        }
    };
    return hooks;
}

std::shared_ptr<const GpuSubmissionSet> MakeGpuSubmissionSet(
    const std::shared_ptr<org::TrackedUploadTicket>& ticket) {
    if (!ticket) return {};
    auto token = std::make_shared<GpuSubmissionSet>();
    GpuQueueSubmission submission;
    {
        std::lock_guard lock(ticket->timelineMutex);
        submission.timelineOwner = ticket->timelineOwner;
        submission.value = ticket->timelineValue;
    }
    token->isComplete = [ticket] { return ticket->Complete(); };
    token->isSubmitted = [ticket] {
        const auto state = ticket->state.load(std::memory_order_acquire);
        return state == org::TrackedUploadTicketState::Submitted ||
            state == org::TrackedUploadTicketState::Completed;
    };
    submission.currentTimelineOwner = [ticket] {
        std::lock_guard lock(ticket->timelineMutex);
        return ticket->timelineOwner;
    };
    submission.currentValue = [ticket] {
        std::lock_guard lock(ticket->timelineMutex);
        return ticket->timelineValue;
    };
    token->submissions.push_back(std::move(submission));
    token->subscribe = [ticket](std::function<void()> callback) {
        ticket->SetChangeCallback(callback);
        // Atomically installing on the ticket and then reconciling graph state
        // makes the observation level-triggered across registration races.
        if (callback) callback();
    };
    token->completionNotificationsAreAuthoritative = true;
    token->cancel = [ticket] { return ticket->Cancel(); };
    return token;
}

} // namespace br::render
