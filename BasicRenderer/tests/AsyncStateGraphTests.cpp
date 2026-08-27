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
            repeatedCapture.writes.back().bytes && capture.writes.back().bytes &&
            *repeatedCapture.writes.back().bytes == *capture.writes.back().bytes);

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

        // RequestSnapshot successors carry a complete immutable image rather
        // than a journal delta. They may advance independently of the prior
        // journal sequence and must replace, not silently reuse, its contents.
        VersionedGpuBufferBuildInput fullSuccessor{};
        fullSuccessor.elementStride = sizeof(std::uint32_t);
        fullSuccessor.elementCount = 3;
        fullSuccessor.capacity = 4;
        fullSuccessor.writeSequence = 9;
        fullSuccessor.previous = previous;
        const std::uint32_t fullRows[]{ 70, 80, 90 };
        fullSuccessor.bytes.resize(sizeof(fullRows));
        std::memcpy(fullSuccessor.bytes.data(), fullRows, sizeof(fullRows));
        const auto fullShadow = ReplayVersionedGpuBufferShadow(fullSuccessor, error);
        Check(fullShadow && error.empty());
        Check(std::memcmp(fullShadow->data(), fullRows, sizeof(fullRows)) == 0);
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

    // Request returns an exact handle that is resolvable immediately, even
    // before the producer runs and even while the version is queued behind a
    // predecessor. A handle must never transiently alias Missing state.
    const ArtifactKey immediateKey{ ArtifactKind::Generic, 899, 0 };
    const auto immediateV1 = graph.Request(immediateKey, 1, {}, Payload(1), 1);
    const auto immediateV2 = graph.Request(immediateKey, 2, {}, Payload(2), 2);
    Check(immediateV1 && immediateV2);
    const auto immediateV1Snapshot = graph.Snapshot(immediateV1.version);
    const auto immediateV2Snapshot = graph.Snapshot(immediateV2.version);
    Check(immediateV1Snapshot.readiness != ArtifactReadiness::Missing);
    Check(immediateV2Snapshot.readiness == ArtifactReadiness::Blocked);
    graph.WaitIdle();
    Check(graph.Snapshot(immediateV1.version).revision == 1);
    Check(graph.Snapshot(immediateV2.version).revision == 2);

    // Returned version IDs own their archive entry until the last copied lease
    // is released. Once superseded and unreferenced, the payload is reclaimed
    // without allowing its revision/generation identity to be reused.
    const ArtifactKey leasedKey{ ArtifactKind::Generic, 898, 0 };
    auto leasedV1 = graph.Request(leasedKey, 1, {}, Payload(1), 1);
    Check(leasedV1);
    graph.WaitIdle();
    const ArtifactVersionID expiredLeaseID{
        leasedV1.version.address, leasedV1.version.revision, leasedV1.version.generation };
    auto leasedV2 = graph.Request(leasedKey, 2, {}, Payload(2), 2);
    Check(leasedV2);
    graph.WaitIdle();
    Check(graph.Snapshot(leasedV1.version).payload.Get<Value>() != nullptr);
    const auto reclaimedBeforeLeaseRelease = graph.Stats().reclaimedVersions;
    leasedV1.version.lease.reset();
    Check(graph.Request({ ArtifactKind::Generic, 897, 0 }, 1));
    graph.WaitIdle();
    Check(graph.Stats().reclaimedVersions > reclaimedBeforeLeaseRelease);
    Check(graph.Snapshot(expiredLeaseID).readiness == ArtifactReadiness::Missing);

    std::atomic_uint readySubscriberA{ 0 };
    std::atomic_uint readySubscriberB{ 0 };
    const ArtifactKey subscribedKey{ ArtifactKind::Generic, 900, 0 };
    const auto subscriptionA = graph.AddReadyCallback(
        [&](const ArtifactSnapshot& artifact) {
            if (artifact.key == subscribedKey) ++readySubscriberA;
        });
    const auto subscriptionB = graph.AddReadyCallback(
        [&](const ArtifactSnapshot& artifact) {
            if (artifact.key == subscribedKey) ++readySubscriberB;
        });
    Check(subscriptionA != 0 && subscriptionB != 0 && subscriptionA != subscriptionB);
    Check(graph.Request(subscribedKey, 1));
    graph.WaitIdle();
    Check(readySubscriberA.load() == 1 && readySubscriberB.load() == 1);
    graph.RemoveReadyCallback(subscriptionB);
    Check(graph.Request(subscribedKey, 2));
    graph.WaitIdle();
    Check(readySubscriberA.load() == 2 && readySubscriberB.load() == 1);
    graph.RemoveReadyCallback(subscriptionA);

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
    const auto stampedV1Request = graph.Request(stampedDependent, 1, {
        { stampedDependency, 1, ArtifactReadiness::GpuReady, DependencyPolicy::AllOf }
    });
    Check(stampedV1Request);
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
    const auto retainedStampedV1 = graph.Snapshot(stampedV1Request.version);
    Check(retainedStampedV1.payload.Get<Value>() &&
        retainedStampedV1.payload.Get<Value>()->value == 1);

    // Exact snapshots and readiness gates retain the immutable dependency
    // selected for the build; only Latest consumers rebuild on advancement.
    const ArtifactKey exactDependency{ ArtifactKind::Generic, 304, 0 };
    const ArtifactKey exactConsumer{ ArtifactKind::Generic, 305, 0 };
    Check(graph.Request(exactDependency, 1));
    graph.WaitIdle();
    Check(graph.Request(exactConsumer, 1, {
        { exactDependency, 1, ArtifactReadiness::GpuReady, DependencyPolicy::AllOf, 0,
            DependencyInvalidationPolicy::ExactSnapshot }
    }));
    graph.WaitIdle();
    Check(graph.Snapshot(exactConsumer).payload.Get<Value>()->value == 2);
    Check(graph.Request(exactDependency, 2));
    graph.WaitIdle();
    Check(graph.Snapshot(exactConsumer).payload.Get<Value>()->value == 2);
    const auto exactV1 = graph.Snapshot(ArtifactVersionID{
        exactDependency, 1, graph.Request(exactDependency, 1).version.generation });
    Check(exactV1.readiness == ArtifactReadiness::GpuReady);
    Check(exactV1.payload.Get<Value>() && exactV1.payload.Get<Value>()->value == 1);
    const ArtifactKey lateExactConsumer{ ArtifactKind::Generic, 310, 0 };
    Check(graph.Request(lateExactConsumer, 1, {
        Exact(exactV1.Version(), ArtifactReadiness::GpuReady)
    }));
    graph.WaitIdle();
    Check(graph.Snapshot(lateExactConsumer).payload.Get<Value>()->value == 2);

    std::atomic_uint64_t observedSequence{ 0 };
    const auto observation = graph.ObserveWithSnapshot(exactDependency,
        [&](std::uint64_t sequence, const ArtifactSnapshot&) {
            observedSequence.store(sequence, std::memory_order_release);
        });
    Check(observation.subscription != 0);
    Check(observation.snapshot.revision == 2);
    Check(graph.Request(exactDependency, 3));
    graph.WaitIdle();
    Check(observedSequence.load(std::memory_order_acquire) != 0);
    graph.RemoveReadyCallback(observation.subscription);

    const ArtifactKey gateDependency{ ArtifactKind::Generic, 306, 0 };
    const ArtifactKey gateConsumer{ ArtifactKind::Generic, 307, 0 };
    Check(graph.Request(gateConsumer, 1, {
        { gateDependency, 1, ArtifactReadiness::GpuReady, DependencyPolicy::AllOf, 0,
            DependencyInvalidationPolicy::ReadyGate }
    }));
    Check(graph.Snapshot(gateConsumer).readiness == ArtifactReadiness::Blocked);
    Check(graph.Request(gateDependency, 1));
    graph.WaitIdle();
    Check(graph.Snapshot(gateConsumer).payload.Get<Value>()->value == 2);

    const ArtifactKey addressGateDependency{ ArtifactKind::Generic, 1308, 0 };
    const ArtifactKey addressGateConsumer{ ArtifactKind::Generic, 1309, 0 };
    Check(graph.Request(addressGateDependency, 1));
    graph.WaitIdle();
    Check(graph.Request(addressGateDependency, 2));
    graph.WaitIdle();
    Check(graph.Request(addressGateConsumer, 1, {
        ReadyGate(addressGateDependency, ArtifactReadiness::GpuReady)
    }));
    graph.WaitIdle();
    Check(graph.Snapshot(addressGateConsumer).payload.Get<Value>()->value == 3);
    Check(graph.Request(addressGateDependency, 3));
    graph.WaitIdle();
    Check(graph.Snapshot(addressGateConsumer).revision == 1);

    // A completed Latest consumer advances by creating a new immutable version.
    // The old exact handle must retain its original dependency closure.
    const ArtifactKey latestDependency{ ArtifactKind::Generic, 311, 0 };
    const ArtifactKey latestConsumer{ ArtifactKind::Generic, 312, 0 };
    Check(graph.Request(latestDependency, 1));
    graph.WaitIdle();
    const auto latestV1Request = graph.Request(latestConsumer, 1, {
        Latest(latestDependency, ArtifactReadiness::GpuReady)
    });
    Check(latestV1Request);
    graph.WaitIdle();
    const auto latestV1 = graph.Snapshot(latestV1Request.version);
    Check(latestV1.payload.Get<Value>() && latestV1.payload.Get<Value>()->value == 2);
    Check(graph.Request(latestDependency, 2));
    graph.WaitIdle();
    const auto latestV2 = graph.Snapshot(latestConsumer);
    Check(latestV2.revision == 2);
    Check(latestV2.payload.Get<Value>() && latestV2.payload.Get<Value>()->value == 4);
    const auto retainedLatestV1 = graph.Snapshot(latestV1Request.version);
    Check(retainedLatestV1.payload.Get<Value>() && retainedLatestV1.payload.Get<Value>()->value == 2);
    Check(graph.Request(gateDependency, 2));
    graph.WaitIdle();
    Check(graph.Snapshot(gateConsumer).payload.Get<Value>()->value == 2);

    const ArtifactKey heldDependency{ ArtifactKind::Generic, 308, 0 };
    const ArtifactKey holdConsumer{ ArtifactKind::Generic, 309, 0 };
    Check(graph.Request(holdConsumer, 1, {
        { heldDependency, 1, ArtifactReadiness::GpuReady, DependencyPolicy::AllOf, 0,
            DependencyInvalidationPolicy::LifetimeHold }
    }));
    graph.WaitIdle();
    Check(graph.Snapshot(holdConsumer).payload.Get<Value>()->value == 1);
    Check(graph.Request(heldDependency, 1));
    graph.WaitIdle();
    Check(graph.Snapshot(holdConsumer).payload.Get<Value>()->value == 1);

    // Replacing a queued request can replace its dependency closure. The queue
    // consumer must revalidate that current closure instead of building with a
    // partial dependency snapshot validated by the earlier request.
    const ArtifactKey queuedDependency{ ArtifactKind::Generic, 301, 0 };
    const ArtifactKey addedDependency{ ArtifactKind::Generic, 302, 0 };
    const ArtifactKey queuedRoot{ ArtifactKind::TerrainState, 303, 0 };
    Check(graph.Request(queuedDependency, 1));
    graph.WaitIdle();
    std::atomic_bool holdDrainStarted{ false };
    std::atomic_bool releaseHeldDrain{ false };
    auto holdDrainScope = scheduler.CreateScope("QueuedClosureReplacementBarrier");
    Check(scheduler.Submit(holdDrainScope, TaskLane::Streaming, TaskDomain::RendererState,
        "QueuedClosureReplacementBarrier", [&](const br::TaskContext&) {
            holdDrainStarted.store(true, std::memory_order_release);
            while (!releaseHeldDrain.load(std::memory_order_acquire)) std::this_thread::yield();
        }));
    const auto holdDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!holdDrainStarted.load(std::memory_order_acquire) &&
        std::chrono::steady_clock::now() < holdDeadline) std::this_thread::yield();
    Check(holdDrainStarted.load(std::memory_order_acquire));
    std::atomic_uint queuedRootBuilds{ 0 };
    graph.RegisterProducer(ArtifactKind::TerrainState, {
        TaskLane::Streaming, TaskDomain::General, "QueuedClosureReplacementProducer",
        [&queuedRootBuilds](const ArtifactBuildContext& context) {
            ++queuedRootBuilds;
            Check(context.dependencies.size() == context.revision);
            return ArtifactBuildResult::Ready(Payload(context.revision));
        }
    });
    Check(graph.Request(queuedRoot, 1, {
        { queuedDependency, 1, ArtifactReadiness::GpuReady, DependencyPolicy::AllOf }
    }));
    Check(graph.Request(queuedRoot, 2, {
        { queuedDependency, 1, ArtifactReadiness::GpuReady, DependencyPolicy::AllOf },
        { addedDependency, 1, ArtifactReadiness::GpuReady, DependencyPolicy::AllOf }
    }));
    releaseHeldDrain.store(true, std::memory_order_release);
    holdDrainScope.Wait();
    graph.WaitIdle();
    Check(queuedRootBuilds.load(std::memory_order_acquire) == 1);
    Check(graph.Snapshot(queuedRoot).readiness == ArtifactReadiness::Blocked);
    Check(graph.Request(addedDependency, 1));
    graph.WaitIdle();
    Check(queuedRootBuilds.load(std::memory_order_acquire) == 2);
    Check(graph.Snapshot(queuedRoot).readiness == ArtifactReadiness::GpuReady);

    graph.RegisterTypedProducer<Value, Value>(ArtifactKind::ActiveDrawList,
        TaskLane::Streaming, TaskDomain::General, "TypedProducer",
        [](const ArtifactBuildContext&, std::shared_ptr<const Value> input) {
            return ArtifactBuildResult::Ready(Payload(input->value));
        });
    const ArtifactKey typedArtifact{ ArtifactKind::ActiveDrawList, 31, 0 };
    const auto wrongTypedRequest = graph.Request(typedArtifact, 1, {},
        ArtifactPayload::Make<std::uint64_t>(std::make_shared<const std::uint64_t>(1)));
    Check(wrongTypedRequest.status == ArtifactRequestStatus::TypeMismatch);
    const auto missingFingerprint = graph.Request(typedArtifact, 1, {}, Payload(31));
    Check(missingFingerprint.status == ArtifactRequestStatus::MissingFingerprint);
    Check(graph.Request(typedArtifact, 1, {}, Payload(31), 31));
    graph.WaitIdle();
    Check(graph.Snapshot(typedArtifact).payload.Get<Value>()->value == 31);

    std::atomic_bool sameKeyBuildStarted{ false };
    std::atomic_bool releaseSameKeyBuild{ false };
    std::atomic_uint sameKeyActiveBuilds{ 0 };
    std::atomic_uint sameKeyMaxActiveBuilds{ 0 };
    std::atomic_uint sameKeyBuildCount{ 0 };
    graph.RegisterProducer(ArtifactKind::BufferVersion, {
        TaskLane::Streaming, TaskDomain::General, "SameKeySerialProducer",
        [&](const ArtifactBuildContext& context) {
            const auto active = sameKeyActiveBuilds.fetch_add(1, std::memory_order_acq_rel) + 1u;
            auto observed = sameKeyMaxActiveBuilds.load(std::memory_order_acquire);
            while (observed < active && !sameKeyMaxActiveBuilds.compare_exchange_weak(
                observed, active, std::memory_order_acq_rel)) {}
            const auto buildIndex = sameKeyBuildCount.fetch_add(1, std::memory_order_acq_rel);
            if (buildIndex == 0) {
                sameKeyBuildStarted.store(true, std::memory_order_release);
                while (!releaseSameKeyBuild.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
            }
            sameKeyActiveBuilds.fetch_sub(1, std::memory_order_acq_rel);
            return ArtifactBuildResult::Ready(Payload(context.revision));
        }
    });
    const ArtifactKey serialKey{ ArtifactKind::BufferVersion, 33, 0 };
    Check(graph.Request(serialKey, 1));
    const auto serialDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!sameKeyBuildStarted.load(std::memory_order_acquire) &&
        std::chrono::steady_clock::now() < serialDeadline) std::this_thread::yield();
    Check(sameKeyBuildStarted.load(std::memory_order_acquire));
    Check(graph.Request(serialKey, 2));
    Check(sameKeyBuildCount.load(std::memory_order_acquire) == 1);
    releaseSameKeyBuild.store(true, std::memory_order_release);
    graph.WaitIdle();
    Check(graph.Snapshot(serialKey).payload.Get<Value>()->value == 2);
    Check(sameKeyBuildCount.load(std::memory_order_acquire) == 2);
    Check(sameKeyMaxActiveBuilds.load(std::memory_order_acquire) == 1);
    graph.Release(serialKey);
    Check(graph.Snapshot(serialKey).readiness == ArtifactReadiness::Missing);

    const ArtifactKey fingerprinted{ ArtifactKind::Generic, 32, 0 };
    Check(graph.Request(fingerprinted, 1, {}, Payload(1), 100));
    graph.WaitIdle();
    const auto conflict = graph.Request(fingerprinted, 1, {}, Payload(2), 200);
    Check(conflict.status == ArtifactRequestStatus::ConflictingRevision);
    const auto requirementConflict = graph.Request(fingerprinted, 1,
        { { dependency, 2, ArtifactReadiness::GpuReady } }, Payload(1), 100);
    Check(requirementConflict.status == ArtifactRequestStatus::ConflictingRevision);
    Check(graph.Snapshot(fingerprinted).payload.Get<Value>()->value == 1);

    const ArtifactKey staticTransactionA{ ArtifactKind::StaticTransaction, 101, 7 };
    const ArtifactKey staticTransactionB{ ArtifactKind::StaticTransaction, 102, 7 };
    const ArtifactKey staticScene{ ArtifactKind::StaticScene, 1, 0 };
    auto staticA = std::make_shared<StaticTransactionBuildInput>();
    staticA->transactionID = 101;
    staticA->streamGeneration = 7;
    staticA->sourceFingerprint = 1001;
    staticA->groups = { { 10001, 5, 10, 2 }, { 10002, 6, 12, 3 } };
    staticA->groupCount = 2;
    staticA->drawRecordCount = 11;
    staticA->activeEntryCount = 22;
    staticA->placementCount = 5;
    auto staticB = std::make_shared<StaticTransactionBuildInput>();
    staticB->transactionID = 102;
    staticB->streamGeneration = 7;
    staticB->sourceFingerprint = 1002;
    staticB->groups = { { 10003, 4, 8, 1 }, { 10004, 4, 8, 4 }, { 10005, 5, 10, 5 } };
    staticB->groupCount = 3;
    staticB->drawRecordCount = 13;
    staticB->activeEntryCount = 26;
    staticB->placementCount = 10;
    const auto staticVersionA = graph.Request(staticTransactionA, 1, {},
        ArtifactPayload::Make<StaticTransactionBuildInput>(std::move(staticA)), 1001);
    const auto staticVersionB = graph.Request(staticTransactionB, 1, {},
        ArtifactPayload::Make<StaticTransactionBuildInput>(std::move(staticB)), 1002);
    Check(staticVersionA);
    Check(staticVersionB);
    auto staticSceneInput = std::make_shared<StaticSceneBuildInput>();
    staticSceneInput->sourceFingerprint = 9001;
    staticSceneInput->publishRoot = true;
    staticSceneInput->desiredPlacementCount = 15;
    staticSceneInput->materializedPlacementCount = 15;
    staticSceneInput->groupOwners = {
        { 10001, staticVersionA.version }, { 10002, staticVersionA.version },
        { 10003, staticVersionB.version }, { 10004, staticVersionB.version },
        { 10005, staticVersionB.version } };
    Check(graph.Request(staticScene, 1, {
        { staticTransactionA, 1, ArtifactReadiness::GpuReady, DependencyPolicy::AllOf },
        { staticTransactionB, 1, ArtifactReadiness::GpuReady, DependencyPolicy::AllOf },
    }, ArtifactPayload::Make<StaticSceneBuildInput>(std::move(staticSceneInput)), 9001));
    graph.WaitIdle();
    const auto staticRoot = graph.Snapshot(staticScene)
        .payload.Get<RendererStateFragmentArtifact>();
    Check(staticRoot && staticRoot->kind == PublishedFragmentKind::Geometry);
    Check(staticRoot->publishRoot);
    const auto staticPublished = staticRoot->fragment.payload.Get<PublishedStaticSceneState>();
    Check(staticPublished && staticPublished->transactions.size() == 2);
    Check(staticPublished->transactions[0].transactionID == 101);
    Check(staticPublished->groupCount == 5 && staticPublished->drawRecordCount == 24 &&
        staticPublished->activeEntryCount == 48);
    Check(staticPublished->publishedPlacementCount == 15 &&
        staticPublished->desiredPlacementCount == 15 &&
        staticPublished->materializedPlacementCount == 15 &&
        staticPublished->placementSetDigest != 0);

    // A transaction can remain in the closure for one live group after another
    // of its groups is removed and re-added by a newer transaction. The exact
    // group-owner edge must select the successor rather than treating both
    // immutable transaction histories as competing owners.
    const ArtifactKey staticTransactionC{ ArtifactKind::StaticTransaction, 105, 7 };
    auto staticC = std::make_shared<StaticTransactionBuildInput>();
    staticC->transactionID = 105;
    staticC->streamGeneration = 8;
    staticC->sourceFingerprint = 1005;
    staticC->groups = { { 10001, 7, 14, 2 } };
    staticC->groupCount = 1;
    staticC->drawRecordCount = 7;
    staticC->activeEntryCount = 14;
    staticC->placementCount = 2;
    const auto staticVersionC = graph.Request(staticTransactionC, 1, {},
        ArtifactPayload::Make<StaticTransactionBuildInput>(std::move(staticC)), 1005);
    Check(staticVersionC);
    auto supersededGroupScene = std::make_shared<StaticSceneBuildInput>();
    supersededGroupScene->sourceFingerprint = 9002;
    supersededGroupScene->publishRoot = true;
    supersededGroupScene->desiredPlacementCount = 5;
    supersededGroupScene->materializedPlacementCount = 5;
    supersededGroupScene->groupOwners = {
        { 10001, staticVersionC.version }, { 10002, staticVersionA.version } };
    Check(graph.Request(staticScene, 2, {
        Exact(staticVersionA.version, ArtifactReadiness::GpuReady),
        Exact(staticVersionC.version, ArtifactReadiness::GpuReady),
    }, ArtifactPayload::Make<StaticSceneBuildInput>(std::move(supersededGroupScene)), 9002));
    graph.WaitIdle();
    const auto supersededRoot = graph.Snapshot(staticScene)
        .payload.Get<RendererStateFragmentArtifact>();
    const auto supersededPublished = supersededRoot
        ? supersededRoot->fragment.payload.Get<PublishedStaticSceneState>() : nullptr;
    Check(supersededPublished && supersededPublished->groupCount == 2 &&
        supersededPublished->publishedPlacementCount == 5);
    Check(supersededPublished && std::ranges::all_of(
        supersededPublished->transactions,
        [](const PublishedStaticTransaction& transaction) {
            return transaction.groups.size() == 1;
        }));

    const ArtifactKey mismatchedTransaction{ ArtifactKind::StaticTransaction, 103, 7 };
    auto mismatchedInput = std::make_shared<StaticTransactionBuildInput>();
    mismatchedInput->transactionID = 103;
    mismatchedInput->streamGeneration = 7;
    mismatchedInput->sourceFingerprint = 1003;
    mismatchedInput->groups = { { 10006, 1, 2, 2 } };
    mismatchedInput->groupCount = 1;
    mismatchedInput->placementCount = 3;
    Check(graph.Request(mismatchedTransaction, 1, {},
        ArtifactPayload::Make<StaticTransactionBuildInput>(std::move(mismatchedInput)), 1003));
    graph.WaitIdle();
    Check(graph.Snapshot(mismatchedTransaction).readiness == ArtifactReadiness::Failed);
    Check(graph.Diagnose(mismatchedTransaction).error.find("placement") != std::string::npos);

    const ArtifactKey emptyTransaction{ ArtifactKind::StaticTransaction, 104, 7 };
    auto emptyInput = std::make_shared<StaticTransactionBuildInput>();
    emptyInput->transactionID = 104;
    emptyInput->streamGeneration = 7;
    emptyInput->sourceFingerprint = 1004;
    Check(graph.Request(emptyTransaction, 1, {},
        ArtifactPayload::Make<StaticTransactionBuildInput>(std::move(emptyInput)), 1004));
    graph.WaitIdle();
    const auto emptyPublished = graph.Snapshot(emptyTransaction)
        .payload.Get<PublishedStaticTransaction>();
    Check(emptyPublished && emptyPublished->groupCount == 0 &&
        emptyPublished->placementCount == 0);

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
    }, Payload(41), 41));
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
    Check(graph.RequestExpressions(fallbackConsumer, 1, {
        FirstReady{ {
            { preferredBinding, 1, ArtifactReadiness::GpuReady },
            { fallbackBinding, 1, ArtifactReadiness::GpuReady },
        } },
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
    auto trackedToken = MakeGpuSubmissionSet(trackedUpload);
    Check(trackedToken && !trackedToken->Complete());
    timelineValue->store(9, std::memory_order_release);
    Check(trackedToken->Complete());
    Check(trackedUpload->state.load() == org::TrackedUploadTicketState::Completed);

    auto stagedTicket = std::make_shared<org::TrackedUploadTicket>();
    auto stagedTimeline = std::make_shared<std::atomic_uint64_t>(0);
    graph.RegisterProducer(ArtifactKind::ViewLifetime, {
        TaskLane::Streaming, TaskDomain::RendererState, "SubmissionMilestoneProducer",
        [stagedTicket](const ArtifactBuildContext& context) {
            return ArtifactBuildResult::Ready(
                Payload(context.revision), MakeGpuSubmissionSet(stagedTicket));
        }
    });
    const ArtifactKey stagedKey{ ArtifactKind::ViewLifetime, 90, 0 };
    Check(graph.Request(stagedKey, 1));
    graph.WaitIdle();
    Check(graph.Snapshot(stagedKey).readiness == ArtifactReadiness::CpuReady);
    auto stagedQueued = org::TrackedUploadTicketState::Queued;
    Check(stagedTicket->state.compare_exchange_strong(
        stagedQueued, org::TrackedUploadTicketState::Claimed));
    stagedTicket->NotifyChanged();
    graph.WaitIdle();
    Check(graph.Snapshot(stagedKey).readiness == ArtifactReadiness::CpuReady);
    {
        std::lock_guard lock(stagedTicket->timelineMutex);
        stagedTicket->timelineOwner = stagedTimeline;
        stagedTicket->timelineValue = 11;
        stagedTicket->isTimelineComplete = [stagedTimeline](std::uint64_t value) {
            return stagedTimeline->load(std::memory_order_acquire) >= value;
        };
    }
    auto stagedClaimed = org::TrackedUploadTicketState::Claimed;
    Check(stagedTicket->state.compare_exchange_strong(
        stagedClaimed, org::TrackedUploadTicketState::Submitted));
    stagedTicket->NotifyChanged();
    graph.WaitIdle();
    Check(graph.Snapshot(stagedKey).readiness == ArtifactReadiness::UploadSubmitted);
    stagedTimeline->store(11, std::memory_order_release);
    stagedTicket->NotifyChanged();
    graph.WaitIdle();
    Check(graph.Snapshot(stagedKey).readiness == ArtifactReadiness::GpuReady);

    std::atomic_bool gpuComplete{ false };
	std::atomic_uint gpuBuilds{ 0 };
    auto gpuChangeCallback = std::make_shared<std::function<void()>>();
    graph.RegisterProducer(ArtifactKind::TextureBinding, {
        TaskLane::Streaming, TaskDomain::TextureProcessing, "GpuProducer",
        [&gpuComplete, &gpuBuilds, gpuChangeCallback](const ArtifactBuildContext& context) {
			gpuBuilds.fetch_add(1, std::memory_order_relaxed);
            auto token = std::make_shared<GpuSubmissionSet>();
            token->submissions.push_back({ {}, context.revision });
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
    const auto gpuV7Request = graph.Request(gpuKey, 7);
    Check(gpuV7Request);
    graph.WaitIdle();
    Check(graph.Snapshot(gpuKey).readiness == ArtifactReadiness::UploadSubmitted);
	Check(graph.Request(gpuKey, 8));
	graph.WaitIdle();
	const auto executingGpu = graph.Snapshot(gpuKey);
	Check(executingGpu.revision == 8);
	Check(executingGpu.readiness == ArtifactReadiness::UploadSubmitted);
	Check(gpuBuilds.load(std::memory_order_relaxed) == 2);
	const auto submittedV7 = graph.Snapshot(gpuV7Request.version);
    Check(submittedV7.revision == 7);
	Check(submittedV7.readiness == ArtifactReadiness::UploadSubmitted);
    // No completion callback is fired when this synthetic timeline advances.
    // Level-triggered graph reconciliation must still observe it after the old
    // finite recovery window would have expired.
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    gpuComplete.store(true, std::memory_order_release);
	for (std::size_t attempt = 0; attempt < 100 &&
		graph.Snapshot(gpuKey).readiness != ArtifactReadiness::GpuReady; ++attempt) {
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	const auto completedGpuArtifact = graph.Snapshot(gpuKey);
	Check(completedGpuArtifact.revision == 8);
	Check(completedGpuArtifact.readiness == ArtifactReadiness::GpuReady);
	Check(completedGpuArtifact.gpuSubmissions && completedGpuArtifact.gpuSubmissions->Complete());
	Check(gpuBuilds.load(std::memory_order_relaxed) == 2);

    // The upload may complete while ApplyCompletion is still running and the
    // current drain is marked scheduled. The notification must hand off to a
    // successor drain instead of being lost when the current drain goes idle.
    auto completesDuringSubscribe = std::make_shared<std::atomic_bool>(false);
    graph.RegisterProducer(ArtifactKind::MeshTable, {
        TaskLane::Streaming, TaskDomain::TextureProcessing, "ImmediateGpuNotificationProducer",
        [completesDuringSubscribe](const ArtifactBuildContext& context) {
            auto token = std::make_shared<GpuSubmissionSet>();
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
    Check(commit0.lease && commit0.lease->state == commit0.state);
    Check(commit0.lease->epoch == 1 && commit0.lease->frameSlot == 0);
    const auto frame0Lease = commit0.lease;
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
    Check(commit2.lease && commit2.lease->state == commit2.state);
    Check(commit2.lease->sequence > frame0Lease->sequence);
    // Advancing publication cannot retarget a previously committed frame.
    Check(frame0Lease->state->epoch == 1 && frame0Lease->frameSlot == 0);
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
    const auto missingResourceVersion = resolver.GetContentVersion();
    Check(resolver.Resolve().empty());
	PublishedStateResourceResolver stagedResolver(source, PublishedResourceKey{}, {}, false);
	const auto stagedFallbackVersion = stagedResolver.GetContentVersion();
	Check(stagedResolver.Resolve().empty());
	auto epochAdvance = std::make_shared<PublishedRendererState>(*publisher.Active());
	epochAdvance->epoch = publisher.ActiveEpoch() + 1u;
	Check(publisher.PublishCandidate({ publisher.ActiveEpoch(), epochAdvance }));
	auto epochCommit = publisher.Commit(1);
	epochCommit.RunDeferred();
	Check(resolver.GetContentVersion() == missingResourceVersion);
	Check(stagedResolver.GetContentVersion() == stagedFallbackVersion);
	stagedResolver.SetPublishedEnabled(true);
    Check(stagedResolver.GetContentVersion() != stagedFallbackVersion);

    // Independent fragments submitted against the same source snapshot rebase
    // together. Only an explicitly named exact precondition may reject a patch.
    RendererStatePublisher patchPublisher(2);
    PublishedStatePatch materialPatch;
    materialPatch.sourceEpoch = 0;
    PublishedStateFragment materialFragment;
    materialFragment.revision = 10;
    materialFragment.publicationRoot = { { ArtifactKind::MaterialTable, 10, 0 }, 10, 1 };
    materialPatch.fragments[static_cast<std::size_t>(PublishedFragmentKind::Materials)] =
        materialFragment;
    PublishedStatePatch terrainPatch;
    terrainPatch.sourceEpoch = 0;
    PublishedStateFragment terrainFragment;
    terrainFragment.revision = 20;
    terrainFragment.publicationRoot = { { ArtifactKind::TerrainState, 20, 0 }, 20, 1 };
    terrainPatch.fragments[static_cast<std::size_t>(PublishedFragmentKind::Terrain)] =
        terrainFragment;
    Check(patchPublisher.PublishPatch(materialPatch));
    Check(patchPublisher.PublishPatch(terrainPatch));
    auto rebasedCommit = patchPublisher.Commit(0);
    Check(rebasedCommit.committed && rebasedCommit.state->epoch == 1);
    Check(rebasedCommit.state->materials.revision == 10);
    Check(rebasedCommit.state->terrain.revision == 20);
    rebasedCommit.RunDeferred();

    PublishedStatePatch staleButIndependent;
    staleButIndependent.sourceEpoch = 0;
    PublishedStateFragment geometryFragment;
    geometryFragment.revision = 30;
    geometryFragment.publicationRoot = { { ArtifactKind::StaticScene, 30, 0 }, 30, 1 };
    staleButIndependent.fragments[static_cast<std::size_t>(PublishedFragmentKind::Geometry)] =
        geometryFragment;
    Check(patchPublisher.PublishPatch(staleButIndependent));
    auto staleRebaseCommit = patchPublisher.Commit(1);
    Check(staleRebaseCommit.committed && staleRebaseCommit.state->epoch == 2);
    Check(staleRebaseCommit.state->geometry.revision == 30);
    Check(patchPublisher.Stats().rebasedPatches == 1);
    staleRebaseCommit.RunDeferred();

    PublishedStatePatch conflictingPatch;
    conflictingPatch.sourceEpoch = 2;
    PublishedStateFragment drawFragment;
    drawFragment.revision = 40;
    conflictingPatch.fragments[static_cast<std::size_t>(PublishedFragmentKind::DrawRecords)] =
        drawFragment;
    conflictingPatch.preconditions.push_back({ PublishedFragmentKind::Materials,
        { { ArtifactKind::MaterialTable, 999, 0 }, 999, 1 } });
    Check(patchPublisher.PublishPatch(conflictingPatch));
    auto rejectedPatchCommit = patchPublisher.Commit(0);
    Check(!rejectedPatchCommit.committed && rejectedPatchCommit.state->epoch == 2);
    Check(rejectedPatchCommit.state->drawRecords.revision == 0);
    Check(patchPublisher.Stats().rejectedPatchPreconditions == 1);
    rejectedPatchCommit.RunDeferred();

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
    Check(requestService.Request({ ArtifactKind::MaterialTable, 100, 0 }, 7, {}, Payload(77), 77));
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
