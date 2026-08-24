#include "Render/AsyncStateGraph.h"
#include "Render/PublishedRendererState.h"
#include "Render/RendererStateRequestService.h"
#include "Managers/Singletons/RendererECSManager.h"
#include "Render/Runtime/StreamingUploadTypes.h"
#include "Render/VersionedGpuBufferArtifacts.h"
#include "Render/StaticStateArtifacts.h"
#include "Resources/Resolvers/PublishedStateResourceResolver.h"

#include <atomic>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <source_location>
#include <thread>

using namespace br::render;

namespace {
void Check(bool condition,
    const std::source_location location = std::source_location::current()) {
    if (condition) return;
    std::fprintf(stderr, "check failed at %s:%u\n", location.file_name(), location.line());
    std::abort();
}

struct Value { std::uint64_t value = 0; };
ArtifactPayload Payload(std::uint64_t value) {
    return ArtifactPayload::Make(std::make_shared<const Value>(Value{ value }));
}
}

int main() {
    {
        VersionedGpuBufferJournal journal(sizeof(std::uint32_t));
        const std::uint32_t initialRows[]{ 10, 20, 30 };
        journal.Initialize(std::as_bytes(std::span(initialRows)), 3, 4);
        auto capture = journal.CaptureDesired();
        Check(capture.writeSequence == 1 && capture.elementCount == 3 &&
            capture.capacity == 4 && capture.initialBytes.size() == sizeof(initialRows));

        auto published = std::make_shared<PublishedGpuBufferVersion>();
        published->writeSequence = capture.writeSequence;
        published->elementCount = capture.elementCount;
        published->capacity = capture.capacity;
        published->elementStride = sizeof(std::uint32_t);
        published->cpuShadow = std::make_shared<const std::vector<std::byte>>(capture.initialBytes);
        journal.Acknowledge(published);

        journal.RequestCapacity(8);
        const std::uint32_t replacement = 200;
        journal.AppendWrite(1, std::as_bytes(std::span(&replacement, 1)), 4);
        capture = journal.CaptureDesired();
        Check(capture.previous == published && capture.initialBytes.empty() &&
            capture.writes.size() == 2 && capture.capacity == 8 && capture.elementCount == 4);
        const auto repeatedCapture = journal.CaptureDesired();
        Check(repeatedCapture.writes.size() == capture.writes.size() &&
            repeatedCapture.writes.back().bytes == capture.writes.back().bytes);

        VersionedGpuBufferBuildInput input{};
        input.elementStride = sizeof(std::uint32_t);
        input.elementCount = capture.elementCount;
        input.capacity = capture.capacity;
        input.writeSequence = capture.writeSequence;
        input.previous = capture.previous;
        input.writes = capture.writes;
        std::string error;
        const auto replay = ReplayVersionedGpuBufferShadow(input, error);
        Check(replay && error.empty());
        const auto* rows = reinterpret_cast<const std::uint32_t*>(replay->data());
        Check(rows[0] == 10 && rows[1] == 200 && rows[2] == 30 && rows[3] == 0);
    }
    {
        VersionedGpuBufferBuildInput initial{};
        initial.elementStride = sizeof(std::uint32_t);
        initial.elementCount = 3;
        initial.capacity = 4;
        initial.writeSequence = 1;
        const std::uint32_t initialRows[]{ 10, 20, 30 };
        initial.bytes.resize(sizeof(initialRows));
        std::memcpy(initial.bytes.data(), initialRows, sizeof(initialRows));
        std::string error;
        const auto initialShadow = ReplayVersionedGpuBufferShadow(initial, error);
        Check(initialShadow && error.empty());

        auto previous = std::make_shared<PublishedGpuBufferVersion>();
        previous->writeSequence = 1;
        previous->cpuShadow = initialShadow;
        VersionedGpuBufferBuildInput successor{};
        successor.elementStride = sizeof(std::uint32_t);
        successor.elementCount = 4;
        successor.capacity = 8;
        successor.writeSequence = 3;
        successor.previous = previous;
        const std::uint32_t replacement = 200;
        const std::uint32_t appended = 40;
        auto replacementBytes = std::make_shared<std::vector<std::byte>>(sizeof(replacement));
        auto appendedBytes = std::make_shared<std::vector<std::byte>>(sizeof(appended));
        std::memcpy(replacementBytes->data(), &replacement, sizeof(replacement));
        std::memcpy(appendedBytes->data(), &appended, sizeof(appended));
        successor.writes = {
            { 2, 1, replacementBytes },
            { 3, 3, appendedBytes }
        };
        const auto successorShadow = ReplayVersionedGpuBufferShadow(successor, error);
        Check(successorShadow && error.empty());
        const auto* rows = reinterpret_cast<const std::uint32_t*>(successorShadow->data());
        Check(rows[0] == 10 && rows[1] == 200 && rows[2] == 30 && rows[3] == 40);

        successor.writeSequence = 4;
        Check(!ReplayVersionedGpuBufferShadow(successor, error));
        Check(!error.empty());
    }

    auto& scheduler = TaskSchedulerManager::GetInstance();
    TaskSchedulerManager::Config config{};
    config.workerCount = 2;
    config.blockingThreadCount = 1;
    config.staticConcurrency = 1;
    config.shaderConcurrency = 1;
    config.reserveRenderCpu = false;
    scheduler.Initialize(config);
    Check(scheduler.DomainConcurrency(TaskDomain::RendererState) == 1);

    AsyncStateGraph graph(scheduler, "AsyncStateGraphTests");
    RegisterStaticStateProducers(graph);
    graph.RegisterProducer(ArtifactKind::Generic, {
        TaskLane::Streaming, TaskDomain::General, "TestProducer",
        [](const ArtifactBuildContext& context) {
            std::uint64_t value = context.revision;
            for (const auto& dependency : context.dependencies) {
                if (const auto input = dependency.payload.Get<Value>()) value += input->value;
            }
            return ArtifactBuildResult::Ready(Payload(value));
        }
    });

    const ArtifactKey dependency{ ArtifactKind::Generic, 1, 0 };
    const ArtifactKey dependent{ ArtifactKind::Generic, 2, 0 };
    Check(graph.Request(dependent, 3, { { dependency, 2, ArtifactReadiness::GpuReady,
        DependencyPolicy::AllOf } }));
    Check(graph.Request(dependency, 2));
    graph.WaitIdle();
    const auto built = graph.Snapshot(dependent);
    Check(built.readiness == ArtifactReadiness::GpuReady);
    Check(built.revision == 3);
    Check(built.payload.Get<Value>() && built.payload.Get<Value>()->value == 5);

    const ArtifactKey stampedDependency{ ArtifactKind::Generic, 30, 0 };
    const ArtifactKey stampedDependent{ ArtifactKind::Material, 31, 0 };
    Check(graph.Request(stampedDependency, 1));
    graph.WaitIdle();
    std::atomic_bool stampedBuildStarted{ false };
    std::atomic_bool releaseStampedBuild{ false };
    std::atomic_uint stampedBuildCount{ 0 };
    graph.RegisterProducer(ArtifactKind::Material, {
        TaskLane::Streaming, TaskDomain::General, "DependencyStampProducer",
        [&stampedBuildStarted, &releaseStampedBuild, &stampedBuildCount](
            const ArtifactBuildContext& context) {
            const auto attempt = stampedBuildCount.fetch_add(1, std::memory_order_relaxed);
            if (attempt == 0) {
                stampedBuildStarted.store(true, std::memory_order_release);
                while (!releaseStampedBuild.load(std::memory_order_acquire)) std::this_thread::yield();
            }
            Check(context.dependencies.size() == 1);
            const auto value = context.dependencies.front().payload.Get<Value>();
            Check(static_cast<bool>(value));
            return ArtifactBuildResult::Ready(Payload(value->value));
        }
    });
    Check(graph.Request(stampedDependent, 1, {
        { stampedDependency, 1, ArtifactReadiness::GpuReady, DependencyPolicy::AllOf }
    }));
    const auto stampDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!stampedBuildStarted.load(std::memory_order_acquire) &&
        std::chrono::steady_clock::now() < stampDeadline) std::this_thread::yield();
    Check(stampedBuildStarted.load(std::memory_order_acquire));
    Check(graph.Request(stampedDependency, 2));
    releaseStampedBuild.store(true, std::memory_order_release);
    graph.WaitIdle();
    const auto rebuiltFromCurrentDependency = graph.Snapshot(stampedDependent);
    Check(rebuiltFromCurrentDependency.readiness == ArtifactReadiness::GpuReady);
    Check(static_cast<bool>(rebuiltFromCurrentDependency.payload.Get<Value>()));
    Check(rebuiltFromCurrentDependency.payload.Get<Value>()->value == 2);
    Check(stampedBuildCount.load(std::memory_order_relaxed) == 2);
    Check(graph.Stats().staleCompletions != 0);

    const ArtifactKey fingerprinted{ ArtifactKind::Generic, 32, 0 };
    Check(graph.Request(fingerprinted, 1, {}, Payload(1), 100));
    graph.WaitIdle();
    const auto conflict = graph.Request(fingerprinted, 1, {}, Payload(2), 200);
    Check(conflict.status == ArtifactRequestStatus::ConflictingRevision);
    Check(graph.Snapshot(fingerprinted).payload.Get<Value>()->value == 1);

    const ArtifactKey staticTransactionA{ ArtifactKind::StaticTransaction, 101, 7 };
    const ArtifactKey staticTransactionB{ ArtifactKind::StaticTransaction, 102, 7 };
    const ArtifactKey staticScene{ ArtifactKind::StaticScene, 1, 0 };
    auto staticA = std::make_shared<StaticTransactionBuildInput>();
    staticA->transactionID = 101;
    staticA->streamGeneration = 7;
    staticA->sourceFingerprint = 1001;
    staticA->groups = { { 10001, 5, 10 }, { 10002, 6, 12 } };
    staticA->groupCount = 2;
    staticA->drawRecordCount = 11;
    staticA->activeEntryCount = 22;
    auto staticB = std::make_shared<StaticTransactionBuildInput>();
    staticB->transactionID = 102;
    staticB->streamGeneration = 7;
    staticB->sourceFingerprint = 1002;
    staticB->groups = { { 10003, 4, 8 }, { 10004, 4, 8 }, { 10005, 5, 10 } };
    staticB->groupCount = 3;
    staticB->drawRecordCount = 13;
    staticB->activeEntryCount = 26;
    Check(graph.Request(staticTransactionA, 1, {},
        ArtifactPayload::Make<StaticTransactionBuildInput>(std::move(staticA))));
    Check(graph.Request(staticTransactionB, 1, {},
        ArtifactPayload::Make<StaticTransactionBuildInput>(std::move(staticB))));
    auto staticSceneInput = std::make_shared<StaticSceneBuildInput>();
    staticSceneInput->sourceFingerprint = 9001;
    staticSceneInput->transactionKeys = { staticTransactionB, staticTransactionA };
    staticSceneInput->activeGroupIDs = { 10001, 10002, 10003, 10004, 10005 };
    Check(graph.Request(staticScene, 1, {
        { staticTransactionA, 1, ArtifactReadiness::GpuReady, DependencyPolicy::AllOf },
        { staticTransactionB, 1, ArtifactReadiness::GpuReady, DependencyPolicy::AllOf },
    }, ArtifactPayload::Make<StaticSceneBuildInput>(std::move(staticSceneInput))));
    graph.WaitIdle();
    const auto staticRoot = graph.Snapshot(staticScene)
        .payload.Get<RendererStateFragmentArtifact>();
    Check(staticRoot && staticRoot->kind == PublishedFragmentKind::Geometry);
    Check(!staticRoot->publishRoot);
    const auto staticPublished = staticRoot->fragment.payload.Get<PublishedStaticSceneState>();
    Check(staticPublished && staticPublished->transactions.size() == 2);
    Check(staticPublished->transactions[0].transactionID == 101);
    Check(staticPublished->groupCount == 5 && staticPublished->drawRecordCount == 24 &&
        staticPublished->activeEntryCount == 48);

    graph.RegisterProducer(ArtifactKind::MeshTable, {
        TaskLane::Streaming, TaskDomain::General, "InputAndAlternativesProducer",
        [](const ArtifactBuildContext& context) {
            const auto input = context.input.Get<Value>();
            Check(input && input->value == 41);
            Check(context.dependencies.size() == 2);
            return ArtifactBuildResult::Ready(Payload(input->value + 1));
        }
    });
    const ArtifactKey alternativeA{ ArtifactKind::Generic, 20, 0 };
    const ArtifactKey alternativeB{ ArtifactKind::Generic, 21, 0 };
    const ArtifactKey alternativeC{ ArtifactKind::Generic, 22, 0 };
    const ArtifactKey alternativeD{ ArtifactKind::Generic, 23, 0 };
    const ArtifactKey alternativesRoot{ ArtifactKind::MeshTable, 24, 0 };
    Check(graph.Request(alternativesRoot, 1, {
        { alternativeA, 1, ArtifactReadiness::GpuReady, DependencyPolicy::AnyOf, 1 },
        { alternativeB, 1, ArtifactReadiness::GpuReady, DependencyPolicy::AnyOf, 1 },
        { alternativeC, 1, ArtifactReadiness::GpuReady, DependencyPolicy::AnyOf, 2 },
        { alternativeD, 1, ArtifactReadiness::GpuReady, DependencyPolicy::AnyOf, 2 },
    }, Payload(41)));
    Check(graph.Request(alternativeB, 1));
    Check(graph.Request(alternativeD, 1));
    graph.WaitIdle();
    Check(graph.Snapshot(alternativesRoot).payload.Get<Value>()->value == 42);

    std::atomic_uint fallbackBuilds{ 0 };
    graph.RegisterProducer(ArtifactKind::Material, {
        TaskLane::Streaming, TaskDomain::General, "OrderedFallbackProducer",
        [&fallbackBuilds](const ArtifactBuildContext& context) {
            fallbackBuilds.fetch_add(1, std::memory_order_relaxed);
            Check(context.dependencies.size() == 1);
            return ArtifactBuildResult::Ready(Payload(context.dependencies.front().key.primaryID));
        }
    });
    const ArtifactKey preferredBinding{ ArtifactKind::Generic, 40, 0 };
    const ArtifactKey fallbackBinding{ ArtifactKind::Generic, 41, 0 };
    const ArtifactKey fallbackConsumer{ ArtifactKind::Material, 42, 0 };
    Check(graph.Request(fallbackBinding, 1));
    graph.WaitIdle();
    Check(graph.Request(fallbackConsumer, 1, {
        { preferredBinding, 1, ArtifactReadiness::GpuReady, DependencyPolicy::FallbackAllowed, 1 },
        { fallbackBinding, 1, ArtifactReadiness::GpuReady, DependencyPolicy::FallbackAllowed, 1 },
    }));
    graph.WaitIdle();
    Check(graph.Snapshot(fallbackConsumer).payload.Get<Value>()->value == fallbackBinding.primaryID);
    Check(graph.Request(preferredBinding, 1));
    graph.WaitIdle();
    Check(graph.Snapshot(fallbackConsumer).payload.Get<Value>()->value == preferredBinding.primaryID);
    Check(fallbackBuilds.load(std::memory_order_relaxed) == 2);

    const ArtifactKey cycleA{ ArtifactKind::Generic, 10, 0 };
    const ArtifactKey cycleB{ ArtifactKind::Generic, 11, 0 };
    Check(graph.Request(cycleA, 1, { { cycleB, 1 } }));
    Check(graph.Request(cycleB, 1, { { cycleA, 1 } }));
    graph.WaitIdle();
    const auto cycleDiagnostic = graph.Diagnose(cycleB);
    Check(cycleDiagnostic.artifact.readiness == ArtifactReadiness::Failed);
    Check(cycleDiagnostic.error.find("dependency cycle") != std::string::npos);
    Check(graph.Snapshot(cycleA).readiness == ArtifactReadiness::Failed);

    auto cancelledUpload = std::make_shared<org::TrackedUploadTicket>();
    Check(cancelledUpload->Cancel());
    Check(cancelledUpload->state.load() == org::TrackedUploadTicketState::Cancelled);

    auto trackedUpload = std::make_shared<org::TrackedUploadTicket>();
    auto expectedQueued = org::TrackedUploadTicketState::Queued;
    Check(trackedUpload->state.compare_exchange_strong(
        expectedQueued, org::TrackedUploadTicketState::Claimed));
    auto timelineValue = std::make_shared<std::atomic_uint64_t>(0);
    {
        std::lock_guard lock(trackedUpload->timelineMutex);
        trackedUpload->timelineOwner = timelineValue;
        trackedUpload->timelineValue = 9;
        trackedUpload->isTimelineComplete = [timelineValue](std::uint64_t value) {
            return timelineValue->load(std::memory_order_acquire) >= value;
        };
    }
    auto expectedClaimed = org::TrackedUploadTicketState::Claimed;
    Check(trackedUpload->state.compare_exchange_strong(
        expectedClaimed, org::TrackedUploadTicketState::Submitted));
    auto trackedToken = MakeGpuDependencyToken(trackedUpload);
    Check(trackedToken && !trackedToken->Complete());
    timelineValue->store(9, std::memory_order_release);
    Check(trackedToken->Complete());
    Check(trackedUpload->state.load() == org::TrackedUploadTicketState::Completed);

    std::atomic_bool gpuComplete{ false };
	std::atomic_uint gpuBuilds{ 0 };
    auto gpuChangeCallback = std::make_shared<std::function<void()>>();
    graph.RegisterProducer(ArtifactKind::TextureBinding, {
        TaskLane::Streaming, TaskDomain::TextureProcessing, "GpuProducer",
        [&gpuComplete, &gpuBuilds, gpuChangeCallback](const ArtifactBuildContext& context) {
			gpuBuilds.fetch_add(1, std::memory_order_relaxed);
            auto token = std::make_shared<GpuDependencyToken>();
            token->value = context.revision;
            token->isComplete = [&gpuComplete] { return gpuComplete.load(std::memory_order_acquire); };
            token->subscribe = [gpuChangeCallback](std::function<void()> callback) {
                *gpuChangeCallback = std::move(callback);
            };
            token->cancel = [gpuChangeCallback] {
                if (*gpuChangeCallback) (*gpuChangeCallback)();
                return true;
            };
            return ArtifactBuildResult::Ready(Payload(context.revision), std::move(token));
        }
    });
    const ArtifactKey gpuKey{ ArtifactKind::TextureBinding, 1, 0 };
    Check(graph.Request(gpuKey, 7));
    graph.WaitIdle();
    Check(graph.Snapshot(gpuKey).readiness == ArtifactReadiness::UploadSubmitted);
	Check(graph.Request(gpuKey, 8));
	graph.WaitIdle();
	const auto supersededGpu = graph.Snapshot(gpuKey);
	Check(supersededGpu.revision == 8);
	Check(supersededGpu.readiness == ArtifactReadiness::UploadSubmitted);
	Check(gpuBuilds.load(std::memory_order_relaxed) == 2);
    gpuComplete.store(true, std::memory_order_release);
    graph.PumpGpuCompletions();
    graph.WaitIdle();
    Check(graph.Snapshot(gpuKey).readiness == ArtifactReadiness::GpuReady);

    // The upload may complete while ApplyCompletion is still running and the
    // current drain is marked scheduled. The notification must hand off to a
    // successor drain instead of being lost when the current drain goes idle.
    auto completesDuringSubscribe = std::make_shared<std::atomic_bool>(false);
    graph.RegisterProducer(ArtifactKind::MeshTable, {
        TaskLane::Streaming, TaskDomain::TextureProcessing, "ImmediateGpuNotificationProducer",
        [completesDuringSubscribe](const ArtifactBuildContext& context) {
            auto token = std::make_shared<GpuDependencyToken>();
            token->isComplete = [completesDuringSubscribe] {
                return completesDuringSubscribe->load(std::memory_order_acquire);
            };
            token->subscribe = [completesDuringSubscribe](std::function<void()> callback) {
                completesDuringSubscribe->store(true, std::memory_order_release);
                callback();
            };
            return ArtifactBuildResult::Ready(Payload(context.revision), std::move(token));
        }
    });
    const ArtifactKey immediateGpuKey{ ArtifactKind::MeshTable, 91, 0 };
    Check(graph.Request(immediateGpuKey, 1));
    graph.WaitIdle();
    Check(graph.Snapshot(immediateGpuKey).readiness == ArtifactReadiness::GpuReady);

    std::atomic_uint retryAttempts{ 0 };
    graph.RegisterProducer(ArtifactKind::Mesh, {
        TaskLane::Streaming, TaskDomain::General, "RetryProducer",
        [&retryAttempts](const ArtifactBuildContext& context) {
            if (retryAttempts.fetch_add(1, std::memory_order_relaxed) == 0)
                return ArtifactBuildResult::Retry(std::chrono::milliseconds(2));
            return ArtifactBuildResult::Ready(Payload(context.revision));
        }
    });
    const ArtifactKey retryKey{ ArtifactKind::Mesh, 4, 0 };
    Check(graph.Request(retryKey, 1));
    graph.WaitIdle();
    Check(graph.Snapshot(retryKey).readiness == ArtifactReadiness::GpuReady);
    Check(retryAttempts.load(std::memory_order_relaxed) == 2);

    RendererStatePublisher publisher(3);
    auto candidateState = std::make_shared<PublishedRendererState>();
    candidateState->epoch = 1;
    Check(publisher.PublishCandidate({ 0, candidateState }));
    auto commit0 = publisher.Commit(0);
    Check(commit0.state->epoch == 1);
    commit0.RunDeferred();
    auto staleState = std::make_shared<PublishedRendererState>();
    staleState->epoch = 2;
    Check(publisher.PublishCandidate({ 0, staleState }));
    auto commit1 = publisher.Commit(1);
    Check(commit1.state->epoch == 1);
    commit1.RunDeferred();
    Check(publisher.Stats().rejectedBaseEpoch == 1);

    auto nextState = std::make_shared<PublishedRendererState>();
    nextState->epoch = 2;
    Check(publisher.PublishCandidate({ 1, nextState }));
    auto commit2 = publisher.Commit(2);
    Check(commit2.state->epoch == 2);
    commit2.RunDeferred();
    // Slots 0 and 1 retain epoch 1 until their respective fences are released.
    Check(candidateState.use_count() >= 3);
    publisher.ReleaseFrameSlot(0);
    publisher.ReleaseFrameSlot(1);

    const ArtifactSnapshot materialArtifact{
        { ArtifactKind::MaterialTable, 1, 0 }, 12, 1, ArtifactReadiness::GpuReady, Payload(12) };
    Check(!publisher.PublishArtifact(materialArtifact));
    auto manifestCandidateState = std::make_shared<PublishedRendererState>(*publisher.Active());
    manifestCandidateState->materials.revision = 12;
    manifestCandidateState->materials.payload = Payload(12);
    manifestCandidateState->materials.dependencyClosure = { materialArtifact };
    auto manifestPayload = std::make_shared<FrameManifestPayload>();
    manifestPayload->baseEpoch = publisher.ActiveEpoch();
    manifestPayload->state = manifestCandidateState;
    const ArtifactSnapshot manifestArtifact{
        { ArtifactKind::FrameManifest, 0, 0 }, 13, 1, ArtifactReadiness::GpuReady,
        ArtifactPayload::Make<FrameManifestPayload>(manifestPayload) };
    Check(publisher.PublishArtifact(manifestArtifact));
    auto materialCommit = publisher.Commit(0);
    const auto materialState = materialCommit.state;
    Check(materialState->materials.revision == 12);
    Check(materialState->materials.payload.Get<Value>()->value == 12);
    materialCommit.RunDeferred();
    auto source = publisher.ResourceSource();
    PublishedStateResourceResolver resolver(source, PublishedResourceKey{});
    Check(resolver.GetContentVersion() != 0);
    Check(resolver.Resolve().empty());
	PublishedStateResourceResolver stagedResolver(source, PublishedResourceKey{}, {}, false);
	const auto stagedFallbackVersion = stagedResolver.GetContentVersion();
	Check(stagedResolver.Resolve().empty());
	auto epochAdvance = std::make_shared<PublishedRendererState>(*publisher.Active());
	epochAdvance->epoch = publisher.ActiveEpoch() + 1u;
	Check(publisher.PublishCandidate({ publisher.ActiveEpoch(), epochAdvance }));
	auto epochCommit = publisher.Commit(1);
	epochCommit.RunDeferred();
	Check(stagedResolver.GetContentVersion() == stagedFallbackVersion);
	stagedResolver.SetPublishedEnabled(true);
	Check(stagedResolver.GetContentVersion() != stagedFallbackVersion);

    RendererStatePublisher manifestPublisher(2);
    RendererStateRequestService requestService(graph, manifestPublisher);
    graph.SetReadyCallback([&requestService](const ArtifactSnapshot& artifact) {
        requestService.OnArtifactReady(artifact);
    });
    manifestPublisher.SetCandidateRejectedCallback([&requestService](std::uint64_t epoch) {
        requestService.OnCandidateRejected(epoch);
    });
    graph.RegisterProducer(ArtifactKind::MaterialTable, {
        TaskLane::Streaming, TaskDomain::General, "MaterialFragmentProducer",
        [](const ArtifactBuildContext& context) {
            const auto input = context.input.Get<Value>();
            auto artifact = std::make_shared<RendererStateFragmentArtifact>();
            artifact->kind = PublishedFragmentKind::Materials;
            artifact->fragment.revision = context.revision;
            artifact->fragment.payload = Payload(input ? input->value : 0);
            return ArtifactBuildResult::Ready(
                ArtifactPayload::Make<RendererStateFragmentArtifact>(std::move(artifact)));
        }
    });
    Check(requestService.Request({ ArtifactKind::MaterialTable, 100, 0 }, 7, {}, Payload(77)));
    graph.WaitIdle();
    auto manifestCommit = manifestPublisher.Commit(0);
    const auto manifestState = manifestCommit.state;
    Check(manifestState->epoch == 1);
    Check(manifestState->materials.revision == 7);
    Check(manifestState->materials.payload.Get<Value>()->value == 77);
    manifestCommit.RunDeferred();
    requestService.Stop();

    auto& ecs = RendererECSManager::GetInstance();
    ecs.Initialize();
    Check(&ecs.GetWorld() != nullptr);
    std::atomic_bool rejectedWorkerAccess{ false };
    auto ecsScope = scheduler.CreateScope("RendererECSOwnershipTest");
    Check(scheduler.Submit(ecsScope, TaskLane::Streaming, TaskDomain::RendererState,
        "RendererECSOwnershipTest", [&ecs, &rejectedWorkerAccess](const br::TaskContext&) {
            try { (void)ecs.GetWorld(); }
            catch (const std::runtime_error&) {
                rejectedWorkerAccess.store(true, std::memory_order_release);
            }
        }));
    ecsScope.Wait();
    Check(rejectedWorkerAccess.load(std::memory_order_acquire));
    ecs.Cleanup();

    graph.Shutdown();
    scheduler.Cleanup();
    return 0;
}
