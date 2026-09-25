#include "Render/AsyncStateGraph.h"
#include "Render/CapacityProvider.h"
#include "Render/PublishedRendererState.h"
#include "Render/RendererStateRequestService.h"
#include "Managers/Singletons/RendererECSManager.h"
#include "Render/Runtime/StreamingUploadTypes.h"
#include "Render/VersionedGpuBufferArtifacts.h"
#include "Render/TextureImageTableArtifacts.h"
#include "Render/StaticStateArtifacts.h"
#include "Render/ObjectBufferStateArtifacts.h"
#include "Render/GeometryResidencyStateArtifacts.h"
#include "Render/MaterialStateArtifacts.h"
#include "Render/ViewStateArtifacts.h"
#include "Render/PoseStateArtifacts.h"
#include "Render/LightStateArtifacts.h"
#include "Render/SceneSourceStateStore.h"
#include "Resources/Resolvers/PublishedStateResourceResolver.h"
#include "Resources/Buffers/Buffer.h"
#include "Utilities/TripleGenerationMailbox.h"
#include "RenderPasses/PreparedRenderIndirect.h"

#include <atomic>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <source_location>
#include <thread>

using namespace br::render;

namespace {
void Check(bool condition,
    const std::source_location location = std::source_location::current()) {
    if (condition) return;
    // Also written to a file: an abort racing another thread's output can lose
    // the console message, which makes an intermittent failure hard to place.
    if (auto* sink = std::fopen("check_failure.txt", "a")) {
        std::fprintf(sink, "check failed at %s:%u\n", location.file_name(), location.line());
        std::fclose(sink);
    }
    std::fprintf(stderr, "check failed at %s:%u\n", location.file_name(), location.line());
    std::fflush(stderr);
    std::abort();
}

// For awaiters a test expects to reach their milestone: a terminal dispatch is
// itself the failure, so it is asserted rather than silently ignored.
const auto ExpectNoTermination = [](const char* label) {
    return [label](const ArtifactTermination& termination) {
        std::fprintf(stderr, "unexpected terminal dispatch: %s (%s)%c", label,
            termination.error.c_str(), 10);
        Check(false);
    };
};

// The mirror case: an awaiter a test expects to terminate must never be handed
// a reached snapshot.
const auto ExpectNoReach = [](const char* label) {
    return [label](const ArtifactSnapshot&) {
        std::fprintf(stderr, "unexpected ready dispatch: %s%c", label, 10);
        Check(false);
    };
};


struct Value { std::uint64_t value = 0; };
ArtifactPayload Payload(std::uint64_t value) {
    return ArtifactPayload::Make(std::make_shared<const Value>(Value{ value }));
}

ArtifactSnapshot FragmentSnapshot(PublishedFragmentKind kind, ArtifactAddress address,
    std::uint64_t revision, std::uint64_t generation,
    std::vector<ArtifactSnapshot> dependencies = {}) {
    auto artifact = std::make_shared<RendererStateFragmentArtifact>();
    artifact->kind = kind;
    artifact->fragment.revision = revision;
    artifact->fragment.dependencyClosure = std::move(dependencies);
    return { address, revision, generation, ArtifactReadiness::UploadSubmitted,
        ArtifactPayload::Make<RendererStateFragmentArtifact>(std::move(artifact)) };
}
}

int main() {
    std::set_terminate([] {
        const char* detail = "unknown";
        std::string message;
        if (auto current = std::current_exception()) {
            try { std::rethrow_exception(current); }
            catch (const std::exception& exception) { message = exception.what(); detail = message.c_str(); }
            catch (...) { detail = "non-std exception"; }
        } else detail = "no active exception";
        // Also written to a file: an abort racing another thread's output can lose
    // the console message, which makes an intermittent failure hard to place.
    if (auto* sink = std::fopen("check_failure.txt", "a")) {
            std::fprintf(sink, "terminate: %s\n", detail);
            std::fclose(sink);
        }
        std::abort();
    });
    {
        // A fresh list, and every layout switch, invalidate root arguments.
        // Exercise the shared raster recorder without relying on a previous pass.
        struct CommandState {
            rhi::PipelineLayoutHandle layout{};
            bool constantsValid = false;
            unsigned draws = 0;
        } state;
        rhi::CommandListVTable table{};
        table.beginPass = +[](rhi::CommandList*, const rhi::PassBeginInfo&) noexcept {};
        table.endPass = +[](rhi::CommandList*) noexcept {};
        table.setPrimitiveTopology = +[](rhi::CommandList*, rhi::PrimitiveTopology) noexcept {};
        table.bindLayout = +[](rhi::CommandList* list, rhi::PipelineLayoutHandle layout) noexcept {
            auto& state = *static_cast<CommandState*>(list->impl);
            state.layout = layout;
            state.constantsValid = false;
        };
        table.bindPipeline = +[](rhi::CommandList*, rhi::PipelineHandle) noexcept {};
        table.pushConstants = +[](rhi::CommandList* list, rhi::ShaderStage, uint32_t,
            uint32_t binding, uint32_t, uint32_t count, const void* data) noexcept {
            auto& state = *static_cast<CommandState*>(list->impl);
            Check(state.layout.valid());
            if (binding == MiscUintRootSignatureIndex) {
                Check(count == NumMiscUintRootConstants);
                Check(static_cast<const unsigned int*>(data)[0] == 1234u);
                state.constantsValid = true;
            }
        };
        table.executeIndirect = +[](rhi::CommandList* list, rhi::CommandSignatureHandle,
            rhi::ResourceHandle, uint64_t, rhi::ResourceHandle, uint64_t, uint32_t) noexcept {
            auto& state = *static_cast<CommandState*>(list->impl);
            Check(state.constantsValid);
            ++state.draws;
        };
        rhi::CommandList commands;
        commands.impl = &state;
        commands.vt = &table;
        auto owner = std::make_shared<int>(0);
        auto bindings = std::make_shared<org::FrozenExecutionBindings>(
            std::vector<org::FrozenExecutionBindings::ResourceBinding>{
                {rhi::Resource(rhi::ResourceHandle{1, 1}), owner}});
        org::RecordingContext recording(commands, bindings);
        recording.SetPreparedDependencies(std::make_shared<org::PreparedDependencySnapshot>(
            std::vector<org::CapturedPipeline>{{{1, 1}, owner, {1, 1}}, {{2, 1}, owner, {2, 1}}},
            std::vector<std::shared_ptr<const rhi::WorkGraphPtr>>{},
            std::vector<org::PreparedDependencySnapshot::DescriptorBinding>{},
            std::vector<std::shared_ptr<const void>>{},
            std::vector<std::shared_ptr<const org::PreparedLifecycleEffect>>{}));
        PreparedRenderIndirectSequence data;
        data.constants[0] = 1234u;
        data.arguments = {0};
        data.steps.resize(2);
        data.steps[0].program.program = {0};
        data.steps[1].program.program = {1};
        RecordPreparedRenderIndirectSequence(data, recording);
        Check(state.draws == 2);
    }
    {
        br::TripleGenerationMailbox<Value> mailbox;
        mailbox.ProducerValue().value = 1;
        mailbox.Publish(1);
        Check(mailbox.ConsumeLatest() && mailbox.ConsumerValue()->value == 1);
        Check(mailbox.ConsumeLatest() == nullptr);

        mailbox.ProducerValue().value = 2;
        mailbox.Publish(2);
        mailbox.ProducerValue().value = 3;
        mailbox.Publish(3);
        const auto* latest = mailbox.ConsumeLatest();
        Check(latest && latest->value == 3);
        Check(mailbox.ConsumedGeneration() == 3);
        Check(mailbox.PublishedGeneration() == 3);
    }
    {
        br::TripleGenerationMailbox<Value> mailbox;
        constexpr std::uint64_t finalGeneration = 100000;
        std::atomic_bool producerDone{ false };
        std::thread producer([&] {
            for (std::uint64_t generation = 1; generation <= finalGeneration; ++generation) {
                mailbox.ProducerValue().value = generation;
                mailbox.Publish(generation);
            }
            producerDone.store(true, std::memory_order_release);
        });
        std::uint64_t observed = 0;
        while (!producerDone.load(std::memory_order_acquire) ||
            mailbox.ConsumedGeneration() < mailbox.PublishedGeneration()) {
            if (const auto* value = mailbox.ConsumeLatest()) {
                Check(value->value > observed);
                observed = value->value;
            }
        }
        producer.join();
        Check(observed == finalGeneration);
    }
    {
        VersionedGpuBufferJournal journal(sizeof(std::uint32_t));
        const std::uint32_t initialRows[]{ 10, 20, 30 };
        journal.Initialize(std::as_bytes(std::span(initialRows)), 3, 4);
        auto capture = journal.CaptureDesired();
        Check(capture.writeSequence == 1 && capture.elementCount == 3 &&
			capture.capacity == 4 && capture.image &&
            capture.image->ByteSize() == sizeof(initialRows));
		const auto initialDesired = capture.image->Materialize();

        auto published = std::make_shared<PublishedGpuBufferVersion>();
        published->writeSequence = capture.writeSequence;
        published->elementCount = capture.elementCount;
        published->capacity = capture.capacity;
        published->elementStride = sizeof(std::uint32_t);
        published->image = capture.image;
        journal.Acknowledge(published);

        journal.RequestCapacity(8);
        const std::uint32_t replacement = 200;
        journal.AppendWrite(1, std::as_bytes(std::span(&replacement, 1)), 4);
        capture = journal.CaptureDesired();
        Check(capture.previous == published &&
			capture.writes.size() == 2 && capture.capacity == 8 && capture.elementCount == 4 &&
			capture.journalBaseSequence == published->writeSequence && capture.image);
		Check(std::memcmp(initialDesired->data(), initialRows, sizeof(initialRows)) == 0);
		const auto capturedImage = capture.image;
		const auto capturedDesired = capturedImage->Materialize();
		const auto* desiredRows = reinterpret_cast<const std::uint32_t*>(capturedDesired->data());
		Check(desiredRows[0] == 10 && desiredRows[1] == 200 &&
			desiredRows[2] == 30 && desiredRows[3] == 0);
		const std::uint32_t overlap[]{ 300, 400 };
		journal.AppendWrite(1, std::as_bytes(std::span(overlap)), 4);
		// Captures are immutable even while the producer advances its authoritative shadow.
		desiredRows = reinterpret_cast<const std::uint32_t*>(capturedDesired->data());
		Check(desiredRows[1] == 200 && desiredRows[2] == 30);
		const auto repeatedCapture = journal.CaptureDesired();
        Check(repeatedCapture.writes.size() == capture.writes.size() + 1u &&
			repeatedCapture.image != capturedImage);
		const std::uint32_t lowRangeReplacement = 11;
		journal.AppendWrite(0, std::as_bytes(std::span(&lowRangeReplacement, 1)), 1);
		const auto sparseCapture = journal.CaptureDesired();
		Check(sparseCapture.elementCount == 4 && sparseCapture.image &&
			sparseCapture.image->ByteSize() == 4 * sizeof(std::uint32_t));
		const auto sparseBytes = sparseCapture.image->Materialize();
		const auto* sparseRows = reinterpret_cast<const std::uint32_t*>(
			sparseBytes->data());
		Check(sparseRows[0] == 11 && sparseRows[1] == 300 &&
			sparseRows[2] == 400 && sparseRows[3] == 0);

		VersionedGpuBufferJournal crossPageSparseJournal(1);
		const std::byte first{ 0x2a };
		crossPageSparseJournal.Initialize(std::span(&first, 1), 1,
			VersionedGpuBufferImage::PageBytes + 8u);
		const std::byte distant{ 0x5b };
		crossPageSparseJournal.AppendWrite(
			VersionedGpuBufferImage::PageBytes + 5u, std::span(&distant, 1),
			VersionedGpuBufferImage::PageBytes + 6u);
		const auto crossPageSparse = crossPageSparseJournal.CaptureDesired();
		const auto crossPageBytes = crossPageSparse.image->Materialize();
		Check(crossPageBytes->size() == VersionedGpuBufferImage::PageBytes + 6u &&
			(*crossPageBytes)[0] == first &&
			(*crossPageBytes)[VersionedGpuBufferImage::PageBytes - 1u] == std::byte{} &&
			(*crossPageBytes)[VersionedGpuBufferImage::PageBytes + 5u] == distant);

        VersionedGpuBufferBuildInput input{};
        input.elementStride = sizeof(std::uint32_t);
        input.elementCount = capture.elementCount;
        input.capacity = capture.capacity;
        input.writeSequence = capture.writeSequence;
        input.previous = capture.previous;
        input.writes = capture.writes;
		input.image = capture.image;
		input.journalBaseSequence = capture.journalBaseSequence;
        std::string error;
        const auto replay = ReplayVersionedGpuBufferAuthoritativeState(input, error);
		Check(replay && error.empty());
        const auto* rows = reinterpret_cast<const std::uint32_t*>(replay->data());
        Check(rows[0] == 10 && rows[1] == 200 && rows[2] == 30 && rows[3] == 0);

		const std::uint32_t compactedRows[]{ 7, 8 };
		const auto replacementSequence = journal.ReplaceImage(
			std::as_bytes(std::span(compactedRows)), 2, 8);
		const auto compacted = journal.CaptureDesired();
		Check(compacted.writeSequence == replacementSequence &&
			compacted.elementCount == 2 && compacted.capacity == 8 &&
			compacted.image && compacted.image->ByteSize() == sizeof(compactedRows));
		const auto compactedBytes = compacted.image->Materialize();
		const auto* compactedImage = reinterpret_cast<const std::uint32_t*>(
			compactedBytes->data());
		Check(compactedImage[0] == 7 && compactedImage[1] == 8);
    }

    {
        const auto first = AsyncStateGraph::AllocateSuspensionIdentity();
        const auto second = AsyncStateGraph::AllocateSuspensionIdentity();
        Check(first != 0 && second != 0 && first != second);

        auto pool = std::make_shared<VersionedGpuBufferBackingPool>();
        std::atomic_uint retirementWakes{ 0 };
        std::atomic_uint64_t observedIdentity{ 0 };
        const auto identity = pool->SubscribeAvailability(
            [&](std::uint64_t satisfied) {
                observedIdentity.store(satisfied, std::memory_order_release);
                retirementWakes.fetch_add(1, std::memory_order_acq_rel);
            });
        Check(identity != 0);
        Check(identity != first && identity != second);
        Check(retirementWakes.load(std::memory_order_acquire) == 0);
        NotifyVersionedGpuBufferFrameRetirement();
        Check(retirementWakes.load(std::memory_order_acquire) == 1);
        Check(observedIdentity.load(std::memory_order_acquire) == identity);
        NotifyVersionedGpuBufferFrameRetirement();
        Check(retirementWakes.load(std::memory_order_acquire) == 1);
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
        const auto initialShadow = ReplayVersionedGpuBufferAuthoritativeState(initial, error);
        Check(initialShadow && error.empty());

        auto previous = std::make_shared<PublishedGpuBufferVersion>();
        previous->writeSequence = 1;
        previous->image = VersionedGpuBufferImage::FromBytes(*initialShadow);
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
        const std::uint32_t successorRows[]{ 10, 200, 30, 40 };
        successor.image = VersionedGpuBufferImage::FromBytes(
            std::as_bytes(std::span(successorRows)));
        successor.writes = { { 2, sizeof(std::uint32_t), sizeof(std::uint32_t) },
            { 3, 3 * sizeof(std::uint32_t), sizeof(std::uint32_t) } };
        const auto successorShadow = ReplayVersionedGpuBufferAuthoritativeState(successor, error);
        Check(successorShadow && error.empty());
        const auto* rows = reinterpret_cast<const std::uint32_t*>(successorShadow->data());
        Check(rows[0] == 10 && rows[1] == 200 && rows[2] == 30 && rows[3] == 40);

        successor.writeSequence = 4;
        Check(!ReplayVersionedGpuBufferAuthoritativeState(successor, error));
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
        const auto fullShadow = ReplayVersionedGpuBufferAuthoritativeState(fullSuccessor, error);
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
    Check(scheduler.DomainConcurrency(TaskDomain::GraphControl) == 1);
    Check(scheduler.DomainConcurrency(TaskDomain::GraphPublication) == 1);

    // A sealed object cut must survive newer buffer publications without
    // rebuilding its old ABI/revision DTO against the successor payload.
    {
        AsyncStateGraph objects(scheduler, "ObjectCutReplacement");
        objects.RegisterProducer(ArtifactKind::BufferVersion, {
            TaskLane::Streaming, TaskDomain::General, "ObjectBufferTestVersion",
            [](const ArtifactBuildContext& context) {
                return ArtifactBuildResult::Ready(context.input);
            }
        });
        RegisterObjectBufferStateProducer(objects);
        const ArtifactKey bufferKey{ArtifactKind::BufferVersion, 0xee10, 1};
        const ArtifactKey placementKey{ArtifactKind::BufferVersion, 0xee10,
            kObjectSkinnedPlacementVariant};
        const ArtifactKey activePlacementKey{ArtifactKind::BufferVersion, 0xee10,
            kObjectActiveSkinnedPlacementVariant};
        const ArtifactKey rootKey{ArtifactKind::DrawRecordPage, 0xee10, 0};
        const auto bufferPayload = [](std::uint64_t revision) {
            auto version = std::make_shared<PublishedGpuBufferVersion>();
            version->revision = revision;
            version->elementStride = sizeof(std::uint32_t);
            version->resource = org::Buffer::CreateSharedUnmaterialized(rhi::HeapType::DeviceLocal, 16);
            auto fragment = std::make_shared<RendererStateFragmentArtifact>();
            fragment->fragment.payload = ArtifactPayload::Make<PublishedGpuBufferVersion>(version);
            return ArtifactPayload::Make<RendererStateFragmentArtifact>(fragment);
        };
        Check(objects.Request(bufferKey, 1, {}, bufferPayload(1), 1));
        Check(objects.Request(placementKey, 1, {}, bufferPayload(1), 2));
        Check(objects.Request(activePlacementKey, 1, {}, bufferPayload(1), 3));
        objects.WaitIdle();
        const auto firstBuffer = objects.Snapshot(bufferKey);
        auto input = std::make_shared<ObjectBufferStateBuildInput>();
        input->coveredMutationGeneration = 17;
        input->placementRecords = std::make_shared<const std::vector<SkinnedAssemblyPlacementGPU>>(
            std::vector<SkinnedAssemblyPlacementGPU>(3));
        input->activePlacementEntries =
            std::make_shared<const std::vector<PublishedActiveSkinnedPlacement>>(
                std::vector<PublishedActiveSkinnedPlacement>{{2, 7}});
        input->buffers.push_back({bufferKey, 1, sizeof(std::uint32_t), kObjectDrawRecordVariant});
        input->buffers.push_back({placementKey, 1, sizeof(std::uint32_t),
            kObjectSkinnedPlacementVariant});
        input->buffers.push_back({activePlacementKey, 1, sizeof(std::uint32_t),
            kObjectActiveSkinnedPlacementVariant});
        Check(objects.Request(rootKey, 1,
            {Exact(firstBuffer.Version(), ArtifactReadiness::UploadSubmitted),
             Exact(objects.Snapshot(placementKey).Version(), ArtifactReadiness::UploadSubmitted),
             Exact(objects.Snapshot(activePlacementKey).Version(), ArtifactReadiness::UploadSubmitted)},
            ArtifactPayload::Make<ObjectBufferStateBuildInput>(input), 17));
        objects.WaitIdle();
        const auto firstRoot = objects.Snapshot(rootKey);
        Check(firstRoot.readiness == ArtifactReadiness::GpuReady);
        Check(objects.Request(bufferKey, 2, {}, bufferPayload(2), 2));
        objects.WaitIdle();
        const auto retainedRoot = objects.Snapshot(rootKey);
        Check(retainedRoot.readiness == ArtifactReadiness::GpuReady);
        Check(retainedRoot.generation == firstRoot.generation);
        const auto fragment = retainedRoot.payload.Get<RendererStateFragmentArtifact>();
        const auto state = fragment->fragment.payload.Get<PublishedObjectBufferState>();
        Check(state && state->coveredMutationGeneration == 17);
        Check(state->versions.size() == 3 && state->versions[0]->revision == 1);
        Check(state->FindVersion(kObjectSkinnedPlacementVariant) == state->versions[1]);
        Check(state->FindVersion(kObjectActiveSkinnedPlacementVariant) == state->versions[2]);
        Check(state->placementRecords == input->placementRecords &&
            state->activePlacementEntries == input->activePlacementEntries &&
            state->activePlacementEntries->front().drawRecordIndex == 2 &&
            state->activePlacementEntries->front().generation == 7);
        auto nextInput = std::make_shared<ObjectBufferStateBuildInput>();
        nextInput->coveredMutationGeneration = 18;
        nextInput->placementRecords = std::make_shared<const std::vector<SkinnedAssemblyPlacementGPU>>(
            std::vector<SkinnedAssemblyPlacementGPU>(1));
        nextInput->activePlacementEntries =
            std::make_shared<const std::vector<PublishedActiveSkinnedPlacement>>();
        nextInput->buffers.push_back({bufferKey, 2, sizeof(std::uint32_t), kObjectDrawRecordVariant});
        nextInput->buffers.push_back({placementKey, 1, sizeof(std::uint32_t),
            kObjectSkinnedPlacementVariant});
        nextInput->buffers.push_back({activePlacementKey, 1, sizeof(std::uint32_t),
            kObjectActiveSkinnedPlacementVariant});
        Check(objects.Request(rootKey, 2,
            {Exact(objects.Snapshot(bufferKey).Version(), ArtifactReadiness::UploadSubmitted),
             Exact(objects.Snapshot(placementKey).Version(), ArtifactReadiness::UploadSubmitted),
             Exact(objects.Snapshot(activePlacementKey).Version(), ArtifactReadiness::UploadSubmitted)},
            ArtifactPayload::Make<ObjectBufferStateBuildInput>(nextInput), 18));
        objects.WaitIdle();
        const auto nextRoot = objects.Snapshot(rootKey);
        Check(nextRoot.readiness == ArtifactReadiness::GpuReady);
        const auto nextState = nextRoot.payload.Get<RendererStateFragmentArtifact>()
            ->fragment.payload.Get<PublishedObjectBufferState>();
        Check(nextState->coveredMutationGeneration == 18 && nextState->versions[0]->revision == 2);
        Check(state->versions[0]->revision == 1);
        Check(state->placementRecords->size() == 3 && state->activePlacementEntries->size() == 1);
        objects.Shutdown();
    }

    // Dependency selection must not leave payloads in process-lifetime worker
    // TLS. Keep the scheduler alive after destroying each graph to catch this.
    for (bool readyGate : {false, true}) {
        std::weak_ptr<const Value> lifetime;
        {
            AsyncStateGraph ownership(scheduler, "SnapshotLifetime");
            ownership.RegisterProducer(ArtifactKind::Generic, {
                TaskLane::Streaming, TaskDomain::General, "OwnedSnapshotProducer",
                [](const ArtifactBuildContext& context) {
                    return ArtifactBuildResult::Ready(context.input);
                }
            });
            const ArtifactKey source{ArtifactKind::Generic, 0xee01, 0};
            const ArtifactKey consumer{ArtifactKind::Generic, 0xee02, 0};
            auto payload = std::make_shared<const Value>(Value{42});
            lifetime = payload;
            Check(ownership.Request(source, 1, {}, ArtifactPayload::Make(std::move(payload)), 42));
            ownership.WaitIdle();
            auto requirement = readyGate ? ReadyGate(source, ArtifactReadiness::GpuReady)
                : LatestAtLeast(source, 1, ArtifactReadiness::GpuReady);
            Check(ownership.Request(consumer, 1, {requirement}, Payload(7), 7));
            ownership.WaitIdle();
            Check(ownership.Snapshot(consumer).readiness == ArtifactReadiness::GpuReady);
            ownership.Shutdown();
        }
        Check(lifetime.expired());
    }

    // A completed one-shot gate is not a live dependency on an archived
    // source version. Reclaim that version while the consumer stays current.
    {
        AsyncStateGraph gates(scheduler, "RetiredReadyGate");
        gates.RegisterProducer(ArtifactKind::Generic, {
            TaskLane::Streaming, TaskDomain::General, "GateProducer",
            [](const ArtifactBuildContext& context) {
                return ArtifactBuildResult::Ready(context.input);
            }
        });
        const ArtifactKey source{ArtifactKind::Generic, 0xee03, 0};
        const ArtifactKey consumer{ArtifactKind::Generic, 0xee04, 0};
        const ArtifactKey absent{ArtifactKind::Generic, 0xee05, 0};
        std::weak_ptr<const Value> lifetime;
        auto payload = std::make_shared<const Value>(Value{42});
        lifetime = payload;
        Check(gates.SubmitLatestIntent(source, 1, {},
            ArtifactPayload::Make(std::move(payload)), 42) == ArtifactRequestStatus::Accepted);
        gates.WaitIdle();
        Check(gates.SubmitLatestIntent(consumer, 1,
            {ReadyGate(source, ArtifactReadiness::GpuReady)}, Payload(7), 7) ==
            ArtifactRequestStatus::Accepted);
        gates.WaitIdle();
        Check(gates.SubmitLatestIntent(source, 2, {}, Payload(43), 43) ==
            ArtifactRequestStatus::Accepted);
        gates.WaitIdle();
        Check(lifetime.expired());
        Check(gates.Diagnose(consumer).blockers.empty());
        Check(gates.Diagnose(consumer).blockerChain.find(" <- ") == std::string::npos);
        Check(gates.SubmitLatestIntent(consumer, 2,
            {ReadyGate(absent, ArtifactReadiness::GpuReady)}, Payload(8), 8) ==
            ArtifactRequestStatus::Accepted);
        const auto blockedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (gates.Snapshot(consumer).readiness != ArtifactReadiness::Blocked) {
            Check(std::chrono::steady_clock::now() < blockedDeadline);
            std::this_thread::yield();
        }
        Check(!gates.Diagnose(consumer).blockers.empty());
        gates.Shutdown();
    }

    // Posted requests return a predicted handle: the generation is allocated by
    // the caller and adopted by the drain, so the handle is usable immediately as
    // an exact requirement or an await target.
    {
        AsyncStateGraph posted(scheduler, "PostedRequests");
        posted.RegisterProducer(ArtifactKind::Generic, {
            TaskLane::Streaming, TaskDomain::General, "PostedProducer",
            [](const ArtifactBuildContext& context) {
                return ArtifactBuildResult::Ready(context.input);
            }
        });
        const ArtifactKey source{ArtifactKind::Generic, 0xef01, 0};
        const ArtifactKey consumer{ArtifactKind::Generic, 0xef02, 0};
        // Await continuations are dispatched as scheduler tasks, so WaitIdle
        // returning does not by itself mean the continuation has run.
        const auto waitFor = [](const std::atomic_bool& flag, const char* label) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!flag.load(std::memory_order_acquire)) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    std::fprintf(stderr, "await never dispatched: %s%c", label, 10);
                    Check(false);
                }
                std::this_thread::yield();
            }
        };

        // A fresh revision adopts the predicted generation verbatim.
        const auto first = posted.PostRequest({source, 1, {}, Payload(1), 1});
        Check(first.status == ArtifactRequestStatus::Accepted);
        Check(first.version.generation != 0);
        // The sibling consumer is posted before the drain has applied the source.
        const auto consumed = posted.PostRequest({consumer, 1,
            {Exact(first.version, ArtifactReadiness::GpuReady)}, Payload(2), 2});
        Check(consumed.status == ArtifactRequestStatus::Accepted);
        std::atomic_bool sourceReady{false};
        auto awaiter = posted.AwaitExact(first.Handle(), ArtifactReadiness::GpuReady,
            TaskLane::Streaming, TaskDomain::General,
            [&sourceReady](const ArtifactSnapshot&) {
                sourceReady.store(true, std::memory_order_release);
            }, ExpectNoTermination("sourceReady"));
        posted.WaitIdle();
        const auto installed = posted.Snapshot(source);
        Check(installed.generation == first.version.generation);
        Check(installed.readiness == ArtifactReadiness::GpuReady);
        // Snapshot by the predicted version resolves, and the sibling requirement
        // bound to it, so the consumer completed rather than blocking.
        Check(posted.Snapshot(first.version).generation == first.version.generation);
        Check(posted.Snapshot(consumer).readiness == ArtifactReadiness::GpuReady);
        waitFor(sourceReady, "sourceReady");

        // Two posts of the same (address, revision) reserve one generation, so a
        // consumer comparing its handle with delivered snapshots sees one identity.
        const auto aliasedFirst = posted.PostRequest({source, 5, {}, Payload(5), 5});
        const auto aliasedSecond = posted.PostRequest({source, 5, {}, Payload(5), 5});
        Check(aliasedSecond.version.generation == aliasedFirst.version.generation);
        std::atomic_bool aliasReady{false};
        auto aliasAwaiter = posted.AwaitExact(aliasedSecond.Handle(),
            ArtifactReadiness::GpuReady, TaskLane::Streaming, TaskDomain::General,
            [&aliasReady](const ArtifactSnapshot&) {
                aliasReady.store(true, std::memory_order_release);
            }, ExpectNoTermination("aliasReady"));
        posted.WaitIdle();
        waitFor(aliasReady, "aliasReady");
        Check(posted.Snapshot(aliasedSecond.version).readiness == ArtifactReadiness::GpuReady);

        // Re-requesting a released revision may assign a fresh ABA generation on
        // the drain; the reserved prediction then becomes an alias and an awaiter
        // registered against it still fires on the installed version.
        {
            (void)posted.PostRequest({source, 7, {}, Payload(7), 7});
            posted.WaitIdle();
            posted.Release(source);
            posted.WaitIdle();
        }
        const auto reissued = posted.PostRequest({source, 7, {}, Payload(7), 7});
        std::atomic_bool reissuedReady{false};
        auto reissuedAwaiter = posted.AwaitExact(reissued.Handle(),
            ArtifactReadiness::GpuReady, TaskLane::Streaming, TaskDomain::General,
            [&reissuedReady](const ArtifactSnapshot& snapshot) {
                reissuedReady.store(snapshot.readiness == ArtifactReadiness::GpuReady,
                    std::memory_order_release);
            }, ExpectNoTermination("reissuedReady"));
        posted.WaitIdle();
        waitFor(reissuedReady, "reissuedReady");
        Check(posted.Snapshot(reissued.version).readiness == ArtifactReadiness::GpuReady);

        // A conflicting recipe for a known revision denotes the installed version
        // (as the synchronous ConflictingRevision status did): it must not poison it.
        const auto conflicting = posted.PostRequest({source, 5, {}, Payload(99), 99});
        Check(conflicting.version.generation == aliasedFirst.version.generation);
        posted.WaitIdle();
        Check(posted.Snapshot(conflicting.version).readiness == ArtifactReadiness::GpuReady);

        // A rejection of a version nobody else holds is observable on the predicted
        // version as a terminal state instead of stranding its consumers: revision
        // 1 posted right after revision 3 of a fresh address is stale.
        const ArtifactKey staleAddress{ArtifactKind::Generic, 0xef03, 0};
        const auto newer = posted.PostRequest({staleAddress, 3, {}, Payload(3), 3});
        const auto stale = posted.PostRequest({staleAddress, 1, {}, Payload(1), 1});
        std::atomic_bool conflictTerminal{false};
        std::atomic_bool conflictRefused{false};
        auto staleAwaiter = posted.AwaitExact(stale.Handle(), ArtifactReadiness::GpuReady,
            TaskLane::Streaming, TaskDomain::General,
            ExpectNoReach("staleReached"),
            [&conflictTerminal, &conflictRefused](const ArtifactTermination& termination) {
                conflictRefused.store(termination.Refused() &&
                    termination.readiness == ArtifactReadiness::Failed,
                    std::memory_order_release);
                conflictTerminal.store(true, std::memory_order_release);
            });
        posted.WaitIdle();
        Check(posted.Snapshot(stale.version).readiness == ArtifactReadiness::Failed);
        Check(posted.Snapshot(newer.version).readiness == ArtifactReadiness::GpuReady);
        waitFor(conflictTerminal, "conflictTerminal");
        // The refusal is delivered as a refusal, not as an ordinary build failure.
        Check(conflictRefused.load(std::memory_order_acquire));
        // A retry of the refused revision does not inherit the refusal.
        Check(posted.PostRequest({staleAddress, 1, {}, Payload(1), 1}).version.generation !=
            stale.version.generation);

        // The reason refusals have to be archived versions rather than a side
        // table: a consumer that chained the predicted handle as a requirement
        // never registers an awaiter, so the only way it can learn of the
        // refusal is through dependency selection. With the refusal invisible to
        // SelectSnapshot this consumer stayed Blocked forever.
        const ArtifactKey refusedSource{ArtifactKind::Generic, 0xef04, 0};
        const ArtifactKey refusedConsumer{ArtifactKind::Generic, 0xef05, 0};
        (void)posted.PostRequest({refusedSource, 4, {}, Payload(4), 4});
        const auto refused = posted.PostRequest({refusedSource, 2, {}, Payload(2), 2});
        std::atomic_bool consumerTerminal{false};
        const auto dependent = posted.PostRequest({refusedConsumer, 1,
            {Exact(refused.Handle(), ArtifactReadiness::CpuReady)}, Payload(1), 1});
        auto dependentAwaiter = posted.AwaitExact(dependent.Handle(),
            ArtifactReadiness::CpuReady, TaskLane::Streaming, TaskDomain::General,
            ExpectNoReach("dependentReached"),
            [&consumerTerminal](const ArtifactTermination& termination) {
                // Propagated, so it is a genuine failure of this version rather
                // than a refusal of the request that created it.
                consumerTerminal.store(!termination.Refused() &&
                    termination.readiness == ArtifactReadiness::Failed,
                    std::memory_order_release);
            });
        posted.WaitIdle();
        waitFor(consumerTerminal, "consumerTerminal");
        Check(posted.Snapshot(dependent.version).readiness == ArtifactReadiness::Failed);

        // Validation that needs no graph state stays synchronous, so these
        // rejections never become a tombstone the caller has to observe later.
        const auto unfingerprinted = posted.PostRequest({source, 6, {}, Payload(6), 0});
        Check(unfingerprinted.status == ArtifactRequestStatus::MissingFingerprint);
        Check(!unfingerprinted.version);
        posted.Shutdown();
    }

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
    // Requests are applied by the graph control drain, which may already have
    // started or finished V1 by the time V2 is applied; V2 is never Missing.
    Check(immediateV2Snapshot.readiness != ArtifactReadiness::Missing);
    graph.WaitIdle();
    Check(graph.Snapshot(immediateV1.version).revision == 1);
    Check(graph.Snapshot(immediateV2.version).revision == 2);

	// Exact milestone waits are level-triggered and keyed by immutable version.
	// Registration before completion and registration after completion both
	// dispatch exactly once without an address-wide callback scan.
	const ArtifactKey awaitedKey{ ArtifactKind::Generic, 881, 0 };
	auto awaited = graph.Request(awaitedKey, 1, {}, Payload(41), 41);
	Check(awaited);
	std::atomic_uint32_t awaitedCallbacks{ 0 };
	std::atomic_uint64_t awaitedValue{ 0 };
	auto firstAwaiter = graph.AwaitExact(awaited.Handle(), ArtifactReadiness::GpuReady,
		TaskLane::Streaming, TaskDomain::RendererState,
		[&](const ArtifactSnapshot& snapshot) {
			const auto value = snapshot.payload.Get<Value>();
			if (value) awaitedValue.store(value->value, std::memory_order_release);
			awaitedCallbacks.fetch_add(1, std::memory_order_acq_rel);
		}, ExpectNoTermination("awaited"));
	// Registration is posted to the graph control drain, so the snapshot handed
	// back is advisory and may be empty. Delivery is what the contract promises.
	Check(firstAwaiter.subscription != 0);
	graph.WaitIdle();
	Check(awaitedCallbacks.load(std::memory_order_acquire) == 1);
	Check(awaitedValue.load(std::memory_order_acquire) == 1);
	auto secondAwaiter = graph.AwaitExact(awaited.Handle(), ArtifactReadiness::GpuReady,
		TaskLane::Streaming, TaskDomain::RendererState,
		[&](const ArtifactSnapshot&) {
			awaitedCallbacks.fetch_add(1, std::memory_order_acq_rel);
		}, ExpectNoTermination("awaitedSecond"));
	graph.WaitIdle();
	Check(awaitedCallbacks.load(std::memory_order_acquire) == 2);

	// Publication is an external renderer-state transition. Exact waiters must
	// receive it just like producer/GPU transitions; otherwise callers that
	// serialize publication can remain pinned forever after MarkPublished.
	std::atomic_uint32_t publishedCallbacks{ 0 };
	auto publishedAwaiter = graph.AwaitExact(awaited.Handle(), ArtifactReadiness::Published,
		TaskLane::Streaming, TaskDomain::RendererState,
		[&](const ArtifactSnapshot& snapshot) {
			if (snapshot.readiness == ArtifactReadiness::Published) {
				publishedCallbacks.fetch_add(1, std::memory_order_acq_rel);
			}
		}, ExpectNoTermination("published"));
	Check(publishedAwaiter.subscription != 0);
	graph.MarkPublished(awaited.version);
	graph.WaitIdle();
	Check(publishedCallbacks.load(std::memory_order_acquire) == 1);

	// Dropping the RAII registration before a blocked version advances prevents
	// dispatch and releases its waiter pin.
	const ArtifactKey awaitDependency{ ArtifactKind::Generic, 879, 0 };
	const ArtifactKey cancelledAwaitKey{ ArtifactKind::Generic, 880, 0 };
	const ArtifactVersionID futureDependency{ awaitDependency, 1, 1 };
	auto cancelledAwait = graph.Request(cancelledAwaitKey, 1,
		{ Exact(futureDependency, ArtifactReadiness::GpuReady) }, Payload(1), 1);
	Check(cancelledAwait);
	std::atomic_bool cancelledAwaitCalled{ false };
	auto cancelledRegistration = graph.AwaitExact(cancelledAwait.Handle(),
		ArtifactReadiness::GpuReady, TaskLane::Streaming, TaskDomain::RendererState,
		[&](const ArtifactSnapshot&) {
			cancelledAwaitCalled.store(true, std::memory_order_release);
		},
		[&](const ArtifactTermination&) {
			cancelledAwaitCalled.store(true, std::memory_order_release);
		});
	Check(cancelledRegistration.subscription != 0);
	cancelledRegistration.Reset();
	Check(graph.Request(awaitDependency, 1, {}, Payload(1), 1));
	graph.WaitIdle();
	Check(!cancelledAwaitCalled.load(std::memory_order_acquire));

	// Dependency-driven Latest rebuilds preserve the caller's logical source
	// revision and mint only a new immutable generation. They must not consume
	// revision 2 and make the caller's next source mutation stale.
	const ArtifactKey generationDependency{ ArtifactKind::Generic, 877, 0 };
	const ArtifactKey generationConsumer{ ArtifactKind::Generic, 878, 0 };
	auto latestDependencyV1 = graph.Request(generationDependency, 1, {}, Payload(10), 10);
	Check(latestDependencyV1);
	graph.WaitIdle();
	auto latestConsumerV1 = graph.Request(generationConsumer, 1,
		{ Latest(generationDependency, ArtifactReadiness::GpuReady) }, Payload(1), 11);
	Check(latestConsumerV1);
	graph.WaitIdle();
	const auto firstLatestGeneration = graph.Snapshot(generationConsumer).generation;
	Check(graph.Request(generationDependency, 2, {}, Payload(20), 20));
	graph.WaitIdle();
	const auto rebuiltLatest = graph.Snapshot(generationConsumer);
	Check(rebuiltLatest.revision == 1);
	Check(rebuiltLatest.generation != firstLatestGeneration);
	Check(graph.Snapshot(latestConsumerV1.version).generation == firstLatestGeneration);
	Check(graph.Request(generationConsumer, 2,
		{ Latest(generationDependency, ArtifactReadiness::GpuReady) }, Payload(2), 12));
	graph.WaitIdle();
	Check(graph.Snapshot(generationConsumer).revision == 2);

    // LatestAtLeast remains satisfied by the newest ready archived version
    // while the address cursor is blocked on a newer desired revision.
    const ArtifactKey archivedLatestSource{ ArtifactKind::Generic, 875, 0 };
    const ArtifactKey archivedLatestConsumer{ ArtifactKind::Generic, 876, 0 };
    auto archivedSourceV1 = graph.Request(
        archivedLatestSource, 1, {}, Payload(10), 10);
    Check(archivedSourceV1);
    graph.WaitIdle();
    const ArtifactVersionID unavailableDependency{
        { ArtifactKind::Generic, 874, 0 }, 1, 0xabcdefu };
    auto blockedSourceV2 = graph.Request(archivedLatestSource, 2,
        { Exact(unavailableDependency, ArtifactReadiness::GpuReady) }, Payload(20), 20);
    Check(blockedSourceV2);
    auto archivedConsumer = graph.Request(archivedLatestConsumer, 1,
        { LatestAtLeast(archivedLatestSource, 1, ArtifactReadiness::GpuReady) },
        Payload(1), 31);
    Check(archivedConsumer);
    graph.WaitIdle();
    const auto archivedConsumerSnapshot = graph.Snapshot(archivedConsumer.version);
    Check(archivedConsumerSnapshot.readiness == ArtifactReadiness::GpuReady);
    Check(archivedConsumerSnapshot.payload.Get<Value>()->value == 2);
    graph.Cancel(archivedLatestSource);

    // A blocked LatestAtLeast consumer itself retains the ready archive that
    // currently satisfies its lower bound. Caller handles are not required:
    // reclaiming that version while a newer source revision is also blocked
    // creates a dependency/capacity stalemate.
    const ArtifactKey retainedLatestSource{ ArtifactKind::Generic, 872, 0 };
    const ArtifactKey retainedLatestConsumer{ ArtifactKind::Generic, 873, 0 };
    const ArtifactKey retainedLatestGate{ ArtifactKind::Generic, 871, 0 };
    auto retainedSourceV1 = graph.Request(retainedLatestSource, 1, {}, Payload(10), 10);
    Check(retainedSourceV1);
    graph.WaitIdle();
    const auto retainedSourceV1ID = retainedSourceV1.version;
    auto retainedConsumer = graph.Request(retainedLatestConsumer, 1, {
        LatestAtLeast(retainedLatestSource, 1, ArtifactReadiness::GpuReady),
        Exact(ArtifactVersionID{ retainedLatestGate, 1, 0 }, ArtifactReadiness::GpuReady)
    }, Payload(1), 32);
    Check(retainedConsumer);
    retainedSourceV1.lease.reset();
    Check(graph.Request(retainedLatestSource, 2,
        { Exact(unavailableDependency, ArtifactReadiness::GpuReady) }, Payload(20), 20));
    graph.WaitIdle();
    Check(graph.Snapshot(retainedSourceV1ID).readiness == ArtifactReadiness::GpuReady);
    Check(graph.Request(retainedLatestGate, 1, {}, Payload(1), 1));
    graph.WaitIdle();
    Check(graph.Snapshot(retainedConsumer.version).readiness == ArtifactReadiness::GpuReady);
    graph.Cancel(retainedLatestSource);

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
    leasedV1.lease.reset();
    Check(graph.Request({ ArtifactKind::Generic, 897, 0 }, 1));
    graph.WaitIdle();
    Check(graph.Stats().reclaimedVersions > reclaimedBeforeLeaseRelease);
    Check(graph.Snapshot(expiredLeaseID).readiness == ArtifactReadiness::Missing);

    // A permanent request signature is a conflict-detection tombstone, not an
    // owner. In particular, copying an Exact requirement into that signature
    // must not pin the dependency after the live recipe and handles advance.
    const ArtifactKey signatureDependency{ ArtifactKind::Generic, 895, 0 };
    const ArtifactKey signatureConsumer{ ArtifactKind::Generic, 896, 0 };
    auto signatureDependencyV1 = graph.Request(signatureDependency, 1, {}, Payload(1), 1);
    Check(signatureDependencyV1);
    graph.WaitIdle();
    const auto signatureDependencyV1ID = signatureDependencyV1.version;
    auto signatureConsumerV1 = graph.Request(signatureConsumer, 1,
        { Exact(signatureDependencyV1.version, ArtifactReadiness::GpuReady) }, Payload(1), 1);
    Check(signatureConsumerV1);
    graph.WaitIdle();
    Check(graph.Request(signatureConsumer, 2, {}, Payload(2), 2));
    Check(graph.Request(signatureDependency, 2, {}, Payload(2), 2));
    signatureDependencyV1.lease.reset();
    signatureConsumerV1.lease.reset();
    graph.WaitIdle();
    Check(graph.Snapshot(signatureDependencyV1ID).readiness == ArtifactReadiness::Missing);

    // A live exact consumer owns its dependency even after every caller-side
    // handle has been dropped and the dependency address advances. The pin is
    // held by the graph recipe, not by a copied requirement or signature.
    const ArtifactKey pinnedDependency{ ArtifactKind::Generic, 883, 0 };
    const ArtifactKey pinnedConsumer{ ArtifactKind::Generic, 884, 0 };
    auto pinnedDependencyV1 = graph.Request(pinnedDependency, 1, {}, Payload(7), 7);
    Check(pinnedDependencyV1);
    graph.WaitIdle();
    const auto pinnedDependencyV1ID = pinnedDependencyV1.version;
    auto pinnedConsumerV1 = graph.Request(pinnedConsumer, 1,
        { Exact(pinnedDependencyV1.version, ArtifactReadiness::GpuReady) }, Payload(1), 1);
    Check(pinnedConsumerV1);
    pinnedDependencyV1.lease.reset();
    pinnedConsumerV1.lease.reset();
    Check(graph.Request(pinnedDependency, 2, {}, Payload(8), 8));
    graph.WaitIdle();
    Check(graph.Snapshot(pinnedDependencyV1ID).payload.Get<Value>() != nullptr);
    Check(graph.Snapshot(pinnedConsumer).payload.Get<Value>() != nullptr);

    // Work queued outside the graph must carry one strong handle across the
    // handoff into an Exact edge. Advancing the producer address while that
    // work waits must not reclaim the captured predecessor.
    const ArtifactKey handoffDependency{ ArtifactKind::Generic, 890, 0 };
    const ArtifactKey handoffConsumer{ ArtifactKind::Generic, 891, 0 };
    auto handoffV1 = graph.Request(handoffDependency, 1, {}, Payload(11), 11);
    Check(handoffV1);
    graph.WaitIdle();
    auto queuedHandoff = handoffV1.Handle();
    handoffV1.lease.reset();
    Check(graph.Request(handoffDependency, 2, {}, Payload(12), 12));
    graph.WaitIdle();
    Check(graph.Snapshot(queuedHandoff.version).payload.Get<Value>() != nullptr);
    auto handoffConsumerV1 = graph.Request(handoffConsumer, 1,
        { Exact(queuedHandoff, ArtifactReadiness::GpuReady) }, Payload(1), 1);
    Check(handoffConsumerV1);
    queuedHandoff = {};
    graph.WaitIdle();
    Check(graph.Snapshot(handoffConsumerV1.version).payload.Get<Value>() != nullptr);

    // Release closes desired state, not the signature tombstone. Repeating an
    // identical request must recreate a live node instead of returning a
    // false AlreadyDesired result for an address that no longer exists.
    const ArtifactKey reusableAddress{ ArtifactKind::Generic, 894, 0 };
    auto reusableV1 = graph.Request(reusableAddress, 1, {}, Payload(1), 0x771u);
    Check(reusableV1);
    graph.WaitIdle();
    reusableV1.lease.reset();
    graph.Release(reusableAddress);
    graph.WaitIdle();
    auto recreatedV1 = graph.Request(reusableAddress, 1, {}, Payload(1), 0x771u);
    Check(recreatedV1 && recreatedV1.status == ArtifactRequestStatus::Accepted);
    graph.WaitIdle();
    Check(graph.Snapshot(recreatedV1.version).payload.Get<Value>() != nullptr);

    // Batch release applies each address once, leaves caller-held immutable
    // versions readable, and permits every released address to be requested
    // again with its original signature.
    const ArtifactKey batchReleaseA{ ArtifactKind::Generic, 0xf006, 0 };
    const ArtifactKey batchReleaseB{ ArtifactKind::Generic, 0xf007, 0 };
    auto batchReleaseAV1 = graph.Request(batchReleaseA, 1, {}, Payload(31), 0x781u);
    auto batchReleaseBV1 = graph.Request(batchReleaseB, 1, {}, Payload(32), 0x782u);
    Check(batchReleaseAV1 && batchReleaseBV1);
    graph.WaitIdle();
    const auto batchReleaseAVersion = batchReleaseAV1.version;
    const auto batchReleaseBVersion = batchReleaseBV1.version;
    const std::array releasedAddresses{ batchReleaseA, batchReleaseB, batchReleaseA };
    graph.ReleaseBatch(releasedAddresses);
    graph.WaitIdle();
    Check(graph.Snapshot(batchReleaseAVersion).payload.Get<Value>() != nullptr);
    Check(graph.Snapshot(batchReleaseBVersion).payload.Get<Value>() != nullptr);
    batchReleaseAV1.lease.reset();
    batchReleaseBV1.lease.reset();
    graph.WaitIdle();
    Check(graph.Request(batchReleaseA, 1, {}, Payload(31), 0x781u));
    Check(graph.Request(batchReleaseB, 1, {}, Payload(32), 0x782u));
    graph.WaitIdle();

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
    auto observation = graph.ObserveWithSnapshot(exactDependency,
        [&](std::uint64_t sequence, const ArtifactSnapshot&) {
            observedSequence.store(sequence, std::memory_order_release);
        });
	std::atomic_uint64_t kindObservedSequence{ 0 };
	std::atomic_bool kindObservedExactRevision{ false };
	auto kindObservation = graph.ObserveKind(ArtifactKind::Generic,
		[&](std::uint64_t sequence, const ArtifactSnapshot& snapshot) {
			if (snapshot.key == exactDependency && snapshot.revision == 3) {
				kindObservedExactRevision.store(true, std::memory_order_release);
			}
			kindObservedSequence.store(sequence, std::memory_order_release);
		});
    Check(observation.subscription != 0);
    Check(observation.snapshot.revision == 2);
	Check(kindObservation.subscription != 0);
	Check(kindObservation.snapshot.readiness == ArtifactReadiness::Missing);
    Check(graph.Request(exactDependency, 3));
    graph.WaitIdle();
    Check(observedSequence.load(std::memory_order_acquire) != 0);
	Check(kindObservedSequence.load(std::memory_order_acquire) != 0);
	Check(kindObservedExactRevision.load(std::memory_order_acquire));
    observation.Reset();
	kindObservation.Reset();

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

    // A ReadyGate that has observed a ready version is level-triggered and
    // exact thereafter. A newer desired dependency may block indefinitely
    // without revoking the earlier milestone from a newly requested consumer.
    const ArtifactKey advancingGateDependency{ ArtifactKind::Generic, 1310, 0 };
    const ArtifactKey missingGateBlocker{ ArtifactKind::Generic, 1311, 0 };
    const ArtifactKey advancingGateConsumer{ ArtifactKind::Generic, 1312, 0 };
    // Retain the completed version while advancing the address. ReadyGate can
    // latch an older milestone only while some explicit owner keeps that
    // immutable version alive; historical readiness is not itself a pin.
    const auto advancingGateV1 = graph.Request(advancingGateDependency, 1);
    Check(advancingGateV1);
    graph.WaitIdle();
    Check(graph.Request(advancingGateDependency, 2, {
        Exact(ArtifactVersionID{ missingGateBlocker, 1, 0 }, ArtifactReadiness::GpuReady)
    }));
    Check(graph.Request(advancingGateConsumer, 1, {
        ReadyGate(advancingGateDependency, ArtifactReadiness::GpuReady)
    }));
    graph.WaitIdle();
    const auto latchedGateResult = graph.Snapshot(advancingGateConsumer);
    Check(latchedGateResult.readiness == ArtifactReadiness::GpuReady);
    Check(latchedGateResult.payload.Get<Value>() &&
        latchedGateResult.payload.Get<Value>()->value == 2);

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
    Check(latestV2.revision == 1);
    Check(latestV2.generation != latestV1.generation);
    Check(latestV2.payload.Get<Value>() && latestV2.payload.Get<Value>()->value == 3);
    const auto retainedLatestV1 = graph.Snapshot(latestV1Request.version);
    Check(retainedLatestV1.payload.Get<Value>() && retainedLatestV1.payload.Get<Value>()->value == 2);

    // Optional Latest requirements remain live requirements after every
    // immutable successor promotion. Terrain intentionally publishes once
    // with fallback rows, then rebuilds repeatedly as texture bindings arrive.
    // A promoted successor must not replace that policy with its latched exact
    // closure or subsequent binding revisions will be missed.
    const ArtifactKey optionalLatestDependency{ ArtifactKind::Generic, 313, 0 };
    const ArtifactKey optionalLatestConsumer{ ArtifactKind::Generic, 314, 0 };
    const auto optionalInitial = graph.Request(optionalLatestConsumer, 1, {
        Latest(optionalLatestDependency, ArtifactReadiness::GpuReady,
            DependencyPolicy::Optional)
    });
    Check(optionalInitial);
    graph.WaitIdle();
    const auto optionalInitialSnapshot = graph.Snapshot(optionalLatestConsumer);
    Check(optionalInitialSnapshot.readiness == ArtifactReadiness::GpuReady);
    Check(graph.Request(optionalLatestDependency, 1));
    graph.WaitIdle();
    const auto optionalFirstAdvance = graph.Snapshot(optionalLatestConsumer);
    Check(optionalFirstAdvance.generation != optionalInitialSnapshot.generation);
    Check(graph.Request(optionalLatestDependency, 2));
    graph.WaitIdle();
    const auto optionalSecondAdvance = graph.Snapshot(optionalLatestConsumer);
    Check(optionalSecondAdvance.generation != optionalFirstAdvance.generation);

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
    Check(scheduler.Submit(holdDrainScope, TaskLane::Streaming, TaskDomain::GraphControl,
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
    // Release is posted; its effect is observable once the drain applies it.
    graph.Release(serialKey);
    graph.WaitIdle();
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

    // Template batches commit exactly once on the graph-publication lane and
    // publish refs in the same deterministic order as their deduplicated keys.
    std::atomic_uint32_t templateBatchCommits{ 0 };
    auto templateBatchReservation = std::make_shared<StaticTemplateBatchReservation>(
        [&](bool commit, PublishedStaticTemplateBatch& published) {
            if (!commit) return true;
            templateBatchCommits.fetch_add(1, std::memory_order_relaxed);
            for (const auto key : published.templateKeys) {
                ObjectManager::StaticMeshTemplateRef ref;
                ref.meshTemplateIndex = static_cast<std::uint32_t>(key);
                published.templateRefs.push_back(std::move(ref));
            }
            return true;
        });
    auto templateBatchInput = std::make_shared<StaticTemplateBatchBuildInput>();
    templateBatchInput->sourceFingerprint = 0xb471u;
    templateBatchInput->templateKeys = { 7, 3, 11 };
    templateBatchInput->reservation = templateBatchReservation;
    const ArtifactKey templateBatchKey{ ArtifactKind::StaticTemplateBatch, 1, 0 };
    Check(graph.Request(templateBatchKey, 1, {},
        ArtifactPayload::Make<StaticTemplateBatchBuildInput>(std::move(templateBatchInput)),
        0xb471u));
    graph.WaitIdle();
    const auto templateBatch = graph.Snapshot(templateBatchKey)
        .payload.Get<PublishedStaticTemplateBatch>();
    Check(templateBatch && templateBatchCommits.load(std::memory_order_relaxed) == 1);
    Check(templateBatch && templateBatch->templateKeys == std::vector<std::uint64_t>({ 3, 7, 11 }));
    Check(templateBatch && templateBatch->templateRefs.size() == 3 &&
        templateBatch->templateRefs[1].meshTemplateIndex == 7);

    // Visibility is derived from exact generation/seal/admission coverage. An
    // incomplete cell keeps the fallback visible; a complete successor closes
    // the cut and hides it in the same serialized acceptance.
    std::vector<StaticVisibilityDecision> acceptedVisibility;
    const auto makeVisibilityReservation = [&] {
        return std::make_shared<StaticVisibilityReservation>(
            [&](bool commit, std::span<const StaticVisibilityDecision> decisions,
                PublishedStaticVisibility&) {
                if (commit) acceptedVisibility.assign(decisions.begin(), decisions.end());
                return true;
            });
    };
    const StaticCoverageUnit coverageUnit{ 0x3cu, 4, -2 };
    const ArtifactKey visibilityKey{ ArtifactKind::StaticVisibility, 1, 0 };
    auto incompleteVisibility = std::make_shared<StaticVisibilityBuildInput>();
    incompleteVisibility->sourceFingerprint = 0x5151u;
    incompleteVisibility->coverage = { { coverageUnit, 9, 2, 1, true } };
    incompleteVisibility->fallbackPolicies = { { 42, 9, { coverageUnit } } };
    incompleteVisibility->reservation = makeVisibilityReservation();
    Check(graph.Request(visibilityKey, 1, {},
        ArtifactPayload::Make<StaticVisibilityBuildInput>(std::move(incompleteVisibility)),
        0x5151u));
    graph.WaitIdle();
    Check(acceptedVisibility.size() == 1 && acceptedVisibility.front().fallbackVisible);
    auto completeVisibility = std::make_shared<StaticVisibilityBuildInput>();
    completeVisibility->sourceFingerprint = 0x5152u;
    completeVisibility->coverage = { { coverageUnit, 9, 2, 2, true } };
    completeVisibility->fallbackPolicies = { { 42, 9, { coverageUnit } } };
    completeVisibility->reservation = makeVisibilityReservation();
    Check(graph.Request(visibilityKey, 2, {},
        ArtifactPayload::Make<StaticVisibilityBuildInput>(std::move(completeVisibility)),
        0x5152u));
    graph.WaitIdle();
    Check(acceptedVisibility.size() == 1 && !acceptedVisibility.front().fallbackVisible);

    const ArtifactKey staticTransactionA{ ArtifactKind::StaticTransaction, 101, 7 };
    const ArtifactKey staticTransactionB{ ArtifactKind::StaticTransaction, 102, 7 };
    const ArtifactKey staticScene{ ArtifactKind::StaticScene, 1, 0 };
    auto staticA = std::make_shared<StaticTransactionBuildInput>();
    auto staticOwnership = std::make_shared<std::uint64_t>(0x5a17u);
    staticA->transactionID = 101;
    staticA->streamGeneration = 7;
    staticA->sourceFingerprint = 1001;
    staticA->groups = { { 10001, 0, 5, 10, 2, staticOwnership }, { 10002, 0, 6, 12, 3 } };
    staticA->groupCount = 2;
    staticA->drawRecordCount = 11;
    staticA->activeEntryCount = 22;
    staticA->placementCount = 5;
    auto staticB = std::make_shared<StaticTransactionBuildInput>();
    staticB->transactionID = 102;
    staticB->streamGeneration = 7;
    staticB->sourceFingerprint = 1002;
    staticB->groups = { { 10003, 0, 4, 8, 1 }, { 10004, 0, 4, 8, 4 }, { 10005, 0, 5, 10, 5 } };
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
    struct RequestedStaticPages {
        std::vector<StaticScenePageRef> refs;
        std::vector<ArtifactRequirement> requirements;
    };
    std::array<std::uint64_t, kStaticScenePageCount> staticPageRevisions{};
    const auto requestStaticPages = [&](std::vector<StaticSceneGroupOwner> owners,
        std::uint64_t fingerprintBase) {
        RequestedStaticPages result;
        std::array<std::vector<StaticSceneGroupOwner>, kStaticScenePageCount> buckets;
        for (const auto& owner : owners) buckets[StaticScenePageIndex(owner.groupID)].push_back(owner);
        for (std::size_t pageIndex = 0; pageIndex < buckets.size(); ++pageIndex) {
            if (buckets[pageIndex].empty()) continue;
            auto input = std::make_shared<StaticScenePageBuildInput>();
            input->pageIndex = static_cast<std::uint32_t>(pageIndex);
            input->sourceFingerprint = fingerprintBase ^ pageIndex;
            input->groupOwners = std::move(buckets[pageIndex]);
            std::vector<ArtifactRequirement> transactionRequirements;
            for (const auto& owner : input->groupOwners) {
                if (std::ranges::none_of(transactionRequirements,
                    [&](const ArtifactRequirement& requirement) {
                        return requirement.key == owner.transaction.address &&
                            requirement.minimumRevision == owner.transaction.revision &&
                            requirement.requiredGeneration == owner.transaction.generation;
                    })) {
                    transactionRequirements.push_back(Exact(
                        owner.transaction, ArtifactReadiness::GpuReady));
                }
            }
            const ArtifactKey key{ ArtifactKind::StaticScenePage, pageIndex + 1u, 0 };
            const auto pageFingerprint = fingerprintBase ^ pageIndex;
            const auto page = graph.Request(key, ++staticPageRevisions[pageIndex],
                std::move(transactionRequirements),
                ArtifactPayload::Make<StaticScenePageBuildInput>(std::move(input)),
                pageFingerprint == 0 ? 1u : pageFingerprint);
            Check(page);
            result.refs.push_back({ static_cast<std::uint32_t>(pageIndex), page.version });
            result.requirements.push_back(Exact(page.version, ArtifactReadiness::CpuReady));
        }
        return result;
    };
    auto initialPages = requestStaticPages({
        { 10001, staticVersionA.version }, { 10002, staticVersionA.version },
        { 10003, staticVersionB.version }, { 10004, staticVersionB.version },
        { 10005, staticVersionB.version } }, 8001);
    auto staticSceneInput = std::make_shared<StaticSceneBuildInput>();
    staticSceneInput->sourceFingerprint = 9001;
    staticSceneInput->publishRoot = true;
    staticSceneInput->desiredPlacementCount = 15;
    staticSceneInput->materializedPlacementCount = 15;
    staticSceneInput->pages = initialPages.refs;
    Check(graph.Request(staticScene, 1, std::move(initialPages.requirements),
        ArtifactPayload::Make<StaticSceneBuildInput>(std::move(staticSceneInput)), 9001));
    graph.WaitIdle();
    const auto staticRoot = graph.Snapshot(staticScene)
        .payload.Get<RendererStateFragmentArtifact>();
    Check(staticRoot && staticRoot->kind == PublishedFragmentKind::Geometry);
    Check(staticRoot->publishRoot);
    const auto staticPublished = staticRoot->fragment.payload.Get<PublishedStaticSceneState>();
    Check(staticPublished && staticPublished->ContainsGroup(10001) &&
        staticPublished->ContainsGroup(10005) && !staticPublished->ContainsGroup(99999));
    Check(staticPublished->ownership && staticPublished->ownership->groups.size() == 5);
    Check(std::ranges::all_of(staticPublished->pages,
        [](const auto& page) { return !page; }));
    const auto* ownedStaticGroup = staticPublished->FindGroup(10001);
    Check(ownedStaticGroup && ownedStaticGroup->ownership == staticOwnership);
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
    staticC->groups = { { 10001, 0, 7, 14, 2 } };
    staticC->groupCount = 1;
    staticC->drawRecordCount = 7;
    staticC->activeEntryCount = 14;
    staticC->placementCount = 2;
    const auto staticVersionC = graph.Request(staticTransactionC, 1, {},
        ArtifactPayload::Make<StaticTransactionBuildInput>(std::move(staticC)), 1005);
    Check(staticVersionC);

    // Page successors retain unchanged groups from an exact immutable base while
    // replacing and removing only the groups named by the delta.
    const auto deltaPageIndex = StaticScenePageIndex(10001);
    const auto basePageRef = std::ranges::find(initialPages.refs,
        static_cast<std::uint32_t>(deltaPageIndex), &StaticScenePageRef::pageIndex);
    Check(basePageRef != initialPages.refs.end());
    const ArtifactKey deltaPageKey{ ArtifactKind::StaticScenePage, deltaPageIndex + 1u, 0 };
    auto replacementDelta = std::make_shared<StaticScenePageBuildInput>();
    replacementDelta->pageIndex = static_cast<std::uint32_t>(deltaPageIndex);
    replacementDelta->sourceFingerprint = 8101;
    replacementDelta->basePage = basePageRef->page;
    replacementDelta->basePagePayload = graph.Snapshot(deltaPageKey).payload.Get<PublishedStaticScenePage>();
    replacementDelta->groupOwners.push_back({ 10001, staticVersionC.version });
    const auto replacedPage = graph.Request(deltaPageKey, ++staticPageRevisions[deltaPageIndex],
        { Exact(staticVersionC.version, ArtifactReadiness::GpuReady) },
        ArtifactPayload::Make<StaticScenePageBuildInput>(std::move(replacementDelta)), 8101);
    Check(replacedPage);
    graph.WaitIdle();
    const auto replacedPagePayload = graph.Snapshot(deltaPageKey).payload.Get<PublishedStaticScenePage>();
    Check(replacedPagePayload && replacedPagePayload->ContainsGroup(10001));
    const auto* replacedGroup = replacedPagePayload ? replacedPagePayload->FindGroup(10001) : nullptr;
    Check(replacedGroup && replacedGroup->drawRecordCount == 7);

    auto removalDelta = std::make_shared<StaticScenePageBuildInput>();
    removalDelta->pageIndex = static_cast<std::uint32_t>(deltaPageIndex);
    removalDelta->sourceFingerprint = 8102;
    removalDelta->basePage = replacedPage.version;
    removalDelta->basePagePayload = replacedPagePayload;
    removalDelta->removedGroupIDs.push_back(10001);
    const auto removedPage = graph.Request(deltaPageKey, ++staticPageRevisions[deltaPageIndex],
        {},
        ArtifactPayload::Make<StaticScenePageBuildInput>(std::move(removalDelta)), 8102);
    Check(removedPage);
    graph.WaitIdle();
    const auto removedPagePayload = graph.Snapshot(deltaPageKey).payload.Get<PublishedStaticScenePage>();
    Check(removedPagePayload && !removedPagePayload->ContainsGroup(10001));
    std::vector<StaticTransactionGroup> effectiveGroups;
    removedPagePayload->MaterializeGroups(effectiveGroups);
    Check(std::ranges::none_of(effectiveGroups,
        [](const auto& group) { return group.groupID == 10001; }));

    auto supersededPages = requestStaticPages({
        { 10001, staticVersionC.version }, { 10002, staticVersionA.version } }, 8002);
    auto supersededGroupScene = std::make_shared<StaticSceneBuildInput>();
    supersededGroupScene->sourceFingerprint = 9002;
    supersededGroupScene->publishRoot = true;
    supersededGroupScene->desiredPlacementCount = 5;
    supersededGroupScene->materializedPlacementCount = 5;
    supersededGroupScene->pages = supersededPages.refs;
    Check(graph.Request(staticScene, 2, supersededPages.requirements,
        ArtifactPayload::Make<StaticSceneBuildInput>(std::move(supersededGroupScene)), 9002));
    graph.WaitIdle();
    const auto supersededRoot = graph.Snapshot(staticScene)
        .payload.Get<RendererStateFragmentArtifact>();
    const auto supersededPublished = supersededRoot
        ? supersededRoot->fragment.payload.Get<PublishedStaticSceneState>() : nullptr;
    Check(supersededPublished && supersededPublished->groupCount == 2 &&
        supersededPublished->publishedPlacementCount == 5);
    Check(supersededPublished && supersededPublished->ContainsGroup(10001) &&
        supersededPublished->ContainsGroup(10002) &&
        !supersededPublished->ContainsGroup(10003));
    Check(supersededPublished->ownership &&
        supersededPublished->FindGroup(10002)->ownership ==
            staticPublished->FindGroup(10002)->ownership);

    // The authoritative renderer path adds one coherent resource closure at
    // the scene root. Historical transactions stay metadata-only, so replacing
    // a material/object/indirect successor does not invalidate every placement
    // transaction or retain one GPU generation per transaction.
    const auto registerFragmentProducer = [&graph](
        ArtifactKind kind, PublishedFragmentKind fragmentKind) {
        graph.RegisterProducer(kind, {
            TaskLane::Streaming, TaskDomain::General, "StaticSceneResourceRoot",
            [fragmentKind](const ArtifactBuildContext& context) {
                auto root = std::make_shared<RendererStateFragmentArtifact>();
                root->kind = fragmentKind;
                root->fragment.revision = context.revision;
                return ArtifactBuildResult::Ready(
                    ArtifactPayload::Make<RendererStateFragmentArtifact>(std::move(root)));
            }
        });
    };
    registerFragmentProducer(ArtifactKind::MaterialTable, PublishedFragmentKind::Materials);
    registerFragmentProducer(ArtifactKind::DrawRecordPage, PublishedFragmentKind::DrawRecords);
    registerFragmentProducer(ArtifactKind::IndirectWorkload,
        PublishedFragmentKind::IndirectWorkloads);
    registerFragmentProducer(ArtifactKind::GeometryBufferState,
        PublishedFragmentKind::Geometry);
    const ArtifactKey materialRoot{ ArtifactKind::MaterialTable, 0, 0 };
    const ArtifactKey objectRoot{ ArtifactKind::DrawRecordPage, 0, 0 };
    const ArtifactKey indirectRoot{ ArtifactKind::IndirectWorkload, 0, 0 };
    const ArtifactKey geometryBufferRoot{ ArtifactKind::GeometryBufferState, 0, 0 };
    const auto materialRootVersion = graph.Request(materialRoot, 1, {}, Payload(1), 9101);
    const auto objectRootVersion = graph.Request(objectRoot, 1, {}, Payload(1), 9102);
    const auto indirectRootVersion = graph.Request(indirectRoot, 1, {}, Payload(1), 9103);
    const auto geometryBufferRootVersion = graph.Request(
        geometryBufferRoot, 1, {}, Payload(1), 9104);
    Check(materialRootVersion && objectRootVersion && indirectRootVersion &&
        geometryBufferRootVersion);
    auto closedScene = std::make_shared<StaticSceneBuildInput>();
    closedScene->sourceFingerprint = 9003;
    closedScene->publishRoot = true;
    closedScene->requireResourceClosure = true;
    closedScene->desiredPlacementCount = 5;
    closedScene->materializedPlacementCount = 5;
    closedScene->pages = supersededPages.refs;
    auto closedRequirements = supersededPages.requirements;
    closedRequirements.push_back(Exact(materialRootVersion.version, ArtifactReadiness::GpuReady));
    closedRequirements.push_back(Exact(objectRootVersion.version, ArtifactReadiness::GpuReady));
    closedRequirements.push_back(Exact(indirectRootVersion.version, ArtifactReadiness::GpuReady));
    closedRequirements.push_back(Exact(
        geometryBufferRootVersion.version, ArtifactReadiness::GpuReady));
    Check(graph.Request(staticScene, 3, std::move(closedRequirements),
        ArtifactPayload::Make<StaticSceneBuildInput>(std::move(closedScene)), 9003));
    graph.WaitIdle();
    const auto closedRoot = graph.Snapshot(staticScene)
        .payload.Get<RendererStateFragmentArtifact>();
    Check(closedRoot && closedRoot->fragment.dependencyClosure.size() ==
        supersededPages.refs.size() + 4);

    const ArtifactKey mismatchedTransaction{ ArtifactKind::StaticTransaction, 103, 7 };
    auto mismatchedInput = std::make_shared<StaticTransactionBuildInput>();
    mismatchedInput->transactionID = 103;
    mismatchedInput->streamGeneration = 7;
    mismatchedInput->sourceFingerprint = 1003;
    mismatchedInput->groups = { { 10006, 0, 1, 2, 2 } };
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

    // Binding preparation is selectable only after worker publication, and
    // superseded or rejected jobs must not replace the previous valid cut.
    {
        RendererStatePublisher gated(2);
        auto base = std::make_shared<PublishedRendererState>();
        gated.Bootstrap(base, 2);
        std::vector<std::function<void()>> jobs;
        gated.SetPreparationScheduler([&](std::function<void()>&& work) { jobs.push_back(std::move(work)); return true; });
        auto makePatch = [](uint64_t revision) {
            PublishedStatePatch patch;
            PublishedStateFragment fragment;
            fragment.revision = revision;
            patch.fragments[static_cast<size_t>(PublishedFragmentKind::Materials)] = fragment;
            PublishedResourceSelection selection;
            selection.resources = std::make_shared<const PublishedResourceSelection::ResourceList>();
            PublishedResourceKey key; key.owner = PublishedFragmentKind::Materials;
            patch.catalogSelections.emplace_back(key, std::move(selection));
            return patch;
        };
        auto retiredOwner = std::make_shared<int>(1);
        const std::weak_ptr<int> retiredOwnerLifetime = retiredOwner;
        auto firstPatch = makePatch(1);
        firstPatch.catalogSelections.front().second.lifetimeHolds.push_back(retiredOwner);
        retiredOwner.reset();
        Check(gated.PublishPatch(std::move(firstPatch)));
        Check(jobs.size() == 1);
        auto waiting = gated.Commit(0); Check(!waiting.committed); waiting.RunDeferred();
        Check(gated.PublishPatch(makePatch(2)));
        Check(jobs.size() == 2);
        jobs[0]();
        waiting = gated.Commit(1); Check(!waiting.committed); waiting.RunDeferred();
        Check(retiredOwnerLifetime.expired()); // Completed callable is still in jobs.
        jobs[1]();
        auto ready = gated.Commit(0); Check(ready.committed && ready.state->materials.revision == 2); ready.RunDeferred();
        gated.SetPreparationScheduler([](std::function<void()>&&) { return false; });
        Check(!gated.PublishPatch(makePatch(3)));
        waiting = gated.Commit(1); Check(!waiting.committed && waiting.state->materials.revision == 2); waiting.RunDeferred();
        gated.Shutdown();
    }

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
    const auto missingResourceIdentity = resolver.CaptureDeclarationState()->resourceSetIdentity;
    Check(resolver.Resolve().empty());
	PublishedStateResourceResolver stagedResolver(source, PublishedResourceKey{}, {}, false);
	const auto stagedFallbackIdentity = stagedResolver.CaptureDeclarationState()->resourceSetIdentity;
	Check(stagedResolver.Resolve().empty());
	auto epochAdvance = std::make_shared<PublishedRendererState>(*publisher.Active());
	epochAdvance->epoch = publisher.ActiveEpoch() + 1u;
	Check(publisher.PublishCandidate({ publisher.ActiveEpoch(), epochAdvance }));
	auto epochCommit = publisher.Commit(1);
	epochCommit.RunDeferred();
	Check(resolver.CaptureDeclarationState()->resourceSetIdentity == missingResourceIdentity);
	Check(stagedResolver.CaptureDeclarationState()->resourceSetIdentity == stagedFallbackIdentity);
	stagedResolver.SetPublishedEnabled(true);
	Check(stagedResolver.CaptureDeclarationState()->resourceSetIdentity == stagedFallbackIdentity);

	// Resolve must observe a newly committed lease even when the graph reuses
	// its layout and does not capture declaration state first.
	RendererStatePublisher directResolvePublisher(2);
	const PublishedResourceKey directResolveKey{
		PublishedFragmentKind::TextureImages, PublishedResourceUsage::ShaderResource,
		0, 0, kTextureImageTableBufferVariant };
	PublishedStateResourceResolver directResolver(
		directResolvePublisher.ResourceSource(), directResolveKey);
	const auto directMissingIdentity = directResolver.CaptureDeclarationState()->resourceSetIdentity;
	Check(directResolver.Resolve().empty());
	auto directResource = org::Buffer::CreateSharedUnmaterialized(
		rhi::HeapType::DeviceLocal, sizeof(std::uint32_t), false);
	auto directResources = std::make_shared<PublishedResourceCatalog::ResourceList>();
	directResources->push_back(directResource);
	PublishedStatePatch directPatch;
	directPatch.catalogOwnerMask = PublishedFragmentMask(PublishedFragmentKind::TextureImages);
	directPatch.catalogEntries.emplace_back(directResolveKey, directResources);
	PublishedStateFragment directFragment;
	directFragment.revision = 1;
	directFragment.publicationRoot = {
		{ ArtifactKind::TextureImageTable, 0, 0 }, 1, 1 };
	directPatch.fragments[static_cast<std::size_t>(PublishedFragmentKind::TextureImages)] =
		directFragment;
	Check(directResolvePublisher.PublishPatch(std::move(directPatch)));
	auto directCommit = directResolvePublisher.Commit(0);
	Check(directCommit.committed);
	directCommit.RunDeferred();
	Check(directResolver.CaptureDeclarationState()->resourceSetIdentity != directMissingIdentity);
	const auto directlyResolved = directResolver.Resolve();
	Check(directlyResolved.size() == 1 && directlyResolved.front() == directResource);
	const auto firstDeclarationState = directResolver.CaptureDeclarationState();
	PublishedStatePatch contentOnlyPatch;
	contentOnlyPatch.catalogOwnerMask = PublishedFragmentMask(PublishedFragmentKind::TextureImages);
	contentOnlyPatch.catalogEntries.emplace_back(directResolveKey, directResources);
	PublishedStateFragment contentOnlyFragment = directFragment;
	contentOnlyFragment.revision = 2;
	contentOnlyFragment.publicationRoot.revision = 2;
	contentOnlyPatch.fragments[static_cast<std::size_t>(PublishedFragmentKind::TextureImages)] =
		contentOnlyFragment;
	Check(directResolvePublisher.PublishPatch(std::move(contentOnlyPatch)));
	auto contentOnlyCommit = directResolvePublisher.Commit(1);
	Check(contentOnlyCommit.committed);
	contentOnlyCommit.RunDeferred();
	const auto contentOnlyState = directResolver.CaptureDeclarationState();
	Check(contentOnlyState->resourceSetIdentity == firstDeclarationState->resourceSetIdentity);
	Check(contentOnlyState->contentRevision != firstDeclarationState->contentRevision);

    // Two workers capturing different manifests through cloned resolvers must
    // never return whichever manifest won the shared declaration-cache store.
    const org::ResolverCaptureContext oldCapture(directCommit.lease);
    const org::ResolverCaptureContext newCapture(contentOnlyCommit.lease);
    auto resolverClone = directResolver.Clone();
    std::atomic_bool captureMismatch{false};
    std::jthread oldReader([&] {
        for (int i = 0; i < 2000; ++i) {
            const auto captured = directResolver.CaptureDeclarationState(oldCapture);
            if (captured->contentRevision != firstDeclarationState->contentRevision ||
                captured->publicationLease) captureMismatch = true;
        }
    });
    std::jthread newReader([&] {
        for (int i = 0; i < 2000; ++i) {
            const auto captured = resolverClone->CaptureDeclarationState(newCapture);
            if (captured->contentRevision != contentOnlyState->contentRevision ||
                captured->publicationLease) captureMismatch = true;
        }
    });
    oldReader.join(); newReader.join();
    Check(!captureMismatch);
    Check(directResolver.CaptureDeclarationState(oldCapture)->dependencyIdentity ==
        resolverClone->CaptureDeclarationState(newCapture)->dependencyIdentity);
    PublishedStateResourceResolver independentPolicy(directResolvePublisher.ResourceSource(), directResolveKey, {}, false);
    Check(independentPolicy.CaptureDeclarationState(newCapture)->dependencyIdentity !=
        directResolver.CaptureDeclarationState(newCapture)->dependencyIdentity);
    const org::ResolverCaptureContext bootstrapCapture(std::shared_ptr<const PublishedManifestLease>{});
    Check(directResolver.CaptureDeclarationState(bootstrapCapture)->resources->empty());

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

    // Production worker publication: frame commit only selects a complete,
    // exact-base successor. Independent results rebase off the owner thread.
    {
        RendererStatePublisher workerPublisher(2);
        std::vector<std::function<void()>> publicationJobs;
        workerPublisher.SetPreparationScheduler([&](std::function<void()>&& job) {
            publicationJobs.push_back(std::move(job)); return true;
        });
        uint32_t interfaceBuilds = 0;
        workerPublisher.SetExecutablePreparation([&](const auto& base,auto& state) {
            Check(state.epoch == base->epoch + 1);
            ++interfaceBuilds;
        });
        Check(workerPublisher.PublishPatch(materialPatch));
        Check(workerPublisher.PublishPatch(terrainPatch));
        Check(publicationJobs.size() == 2);
        auto waiting = workerPublisher.Commit(0);
        Check(!waiting.committed && waiting.state->epoch == 0);
        waiting.RunDeferred();
        auto materialJob = std::move(publicationJobs[0]); materialJob();
        auto terrainJob = std::move(publicationJobs[1]); terrainJob();
        auto selected = workerPublisher.Commit(0);
        Check(selected.committed && selected.state->materials.revision == 10);
        Check(selected.state->terrain.revision == 0 && interfaceBuilds == 2);
        const auto held = selected.lease;
        selected.RunDeferred();
        auto rebasing = workerPublisher.Commit(1);
        Check(!rebasing.committed && publicationJobs.size() == 3 && interfaceBuilds == 2);
        rebasing.RunDeferred();
        auto rebaseJob = std::move(publicationJobs[2]); rebaseJob();
        auto joined = workerPublisher.Commit(1);
        Check(joined.committed && joined.state->epoch == 2 && interfaceBuilds == 3);
        Check(joined.state->materials.revision == 10 && joined.state->terrain.revision == 20);
        Check(held->state->terrain.revision == 0);
        joined.RunDeferred();

        workerPublisher.SetExecutablePreparation([](const auto&,auto&) {
            throw std::runtime_error("injected executable publication failure");
        });
        Check(workerPublisher.PublishPatch(materialPatch));
        auto failedJob = std::move(publicationJobs.back()); failedJob();
        auto failed = workerPublisher.Commit(0);
        Check(!failed.committed && failed.state->epoch == 2);
        failed.RunDeferred();

        workerPublisher.SetExecutablePreparation({});
        workerPublisher.SetPreparationScheduler([](std::function<void()>&&) { return false; });
        Check(!workerPublisher.PublishPatch(materialPatch));
        auto rejected = workerPublisher.Commit(0);
        Check(!rejected.committed && rejected.state->epoch == 2);
        rejected.RunDeferred();
    }

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

	// Catalog publication is an immutable overlay: unchanged owners share the
	// previous catalog, owner replacement hides old keys, and long chains are
	// compacted without changing effective lookup results.
	RendererStatePublisher catalogPublisher(2);
	const PublishedResourceKey materialResourceKey{
		PublishedFragmentKind::Materials, PublishedResourceUsage::ShaderResource, 0, 0, 1 };
	const PublishedResourceKey replacementMaterialKey{
		PublishedFragmentKind::Materials, PublishedResourceUsage::ShaderResource, 0, 0, 2 };
	const PublishedResourceKey terrainResourceKey{
		PublishedFragmentKind::Terrain, PublishedResourceUsage::ShaderResource, 0, 0, 1 };
	auto materialResources = std::make_shared<PublishedResourceCatalog::ResourceList>();
	auto terrainResources = std::make_shared<PublishedResourceCatalog::ResourceList>();
	PublishedStatePatch initialCatalogPatch;
	initialCatalogPatch.catalogEntries = {
		{ materialResourceKey, materialResources }, { terrainResourceKey, terrainResources } };
	PublishedStateFragment initialMaterials;
	initialMaterials.revision = 1;
	initialMaterials.publicationRoot = { { ArtifactKind::MaterialTable, 77, 0 }, 1, 1 };
	PublishedStateFragment initialTerrain;
	initialTerrain.revision = 1;
	initialTerrain.publicationRoot = { { ArtifactKind::TerrainState, 78, 0 }, 1, 2 };
	initialCatalogPatch.fragments[static_cast<std::size_t>(PublishedFragmentKind::Materials)] =
		initialMaterials;
	initialCatalogPatch.fragments[static_cast<std::size_t>(PublishedFragmentKind::Terrain)] =
		initialTerrain;
	Check(catalogPublisher.PublishPatch(std::move(initialCatalogPatch)));
	auto initialCatalogCommit = catalogPublisher.Commit(0);
	Check(initialCatalogCommit.committed);
	Check(initialCatalogCommit.state->resourceCatalog->Find(materialResourceKey) == materialResources);
	Check(initialCatalogCommit.state->resourceCatalog->Find(terrainResourceKey) == terrainResources);
	initialCatalogCommit.RunDeferred();

	PublishedStatePatch replaceMaterialCatalog;
	replaceMaterialCatalog.catalogOwnerMask = PublishedFragmentMask(PublishedFragmentKind::Materials);
	auto replacementResources = std::make_shared<PublishedResourceCatalog::ResourceList>();
	replaceMaterialCatalog.catalogEntries = { { replacementMaterialKey, replacementResources } };
	initialMaterials.revision = 2;
	initialMaterials.publicationRoot.revision = 2;
	initialMaterials.publicationRoot.generation = 3;
	replaceMaterialCatalog.fragments[static_cast<std::size_t>(PublishedFragmentKind::Materials)] =
		initialMaterials;
	Check(catalogPublisher.PublishPatch(std::move(replaceMaterialCatalog)));
	auto replacementCommit = catalogPublisher.Commit(1);
	Check(replacementCommit.committed);
	Check(!replacementCommit.state->resourceCatalog->Find(materialResourceKey));
	Check(replacementCommit.state->resourceCatalog->Find(replacementMaterialKey) == replacementResources);
	Check(replacementCommit.state->resourceCatalog->Find(terrainResourceKey) == terrainResources);
	replacementCommit.RunDeferred();

	for (std::uint64_t revision = 2; revision <= 40; ++revision) {
		PublishedStatePatch overlayPatch;
		PublishedStateFragment terrain = initialTerrain;
		terrain.revision = revision;
		terrain.publicationRoot.revision = revision;
		terrain.publicationRoot.generation = 100 + revision;
		overlayPatch.fragments[static_cast<std::size_t>(PublishedFragmentKind::Terrain)] = terrain;
		overlayPatch.catalogEntries = { { terrainResourceKey, terrainResources } };
		Check(catalogPublisher.PublishPatch(std::move(overlayPatch)));
		auto overlayCommit = catalogPublisher.Commit(revision % 2u);
		Check(overlayCommit.committed);
		overlayCommit.RunDeferred();
	}
	for (const auto& shard : catalogPublisher.Active()->resourceCatalog->ownerShards)
		Check(static_cast<bool>(shard));
	Check(catalogPublisher.Active()->resourceCatalog->Find(replacementMaterialKey) == replacementResources);
	Check(catalogPublisher.Active()->resourceCatalog->Find(terrainResourceKey) == terrainResources);

    // Ordinary publication is monotonic per fragment family even when
    // completions arrive in arbitrary order. Each frame lease remains an
    // immutable snapshot while later commits independently advance slots.
    RendererStatePublisher monotonicPublisher(3);
    struct FragmentArrival {
        PublishedFragmentKind kind;
        std::uint64_t revision;
    };
    std::vector<FragmentArrival> arrivals;
    for (std::size_t index = 0; index < kPublishedFragmentCount; ++index) {
        for (std::uint64_t revision = 1; revision <= 12; ++revision) {
            arrivals.push_back({ static_cast<PublishedFragmentKind>(index), revision });
        }
    }
    std::mt19937 random{ 0x5A17u };
    std::ranges::shuffle(arrivals, random);
    std::array<std::uint64_t, kPublishedFragmentCount> committedRevisions{};
    std::shared_ptr<const PublishedManifestLease> capturedLease;
    PublishedStateFragment capturedGeometry;
    for (std::size_t arrivalIndex = 0; arrivalIndex < arrivals.size(); ++arrivalIndex) {
        const auto [kind, revision] = arrivals[arrivalIndex];
        const auto kindIndex = static_cast<std::size_t>(kind);
        PublishedStatePatch patch;
        patch.sourceEpoch = monotonicPublisher.ActiveEpoch();
        PublishedStateFragment fragment;
        fragment.revision = revision;
        fragment.publicationRoot = { { static_cast<ArtifactKind>(
            static_cast<std::uint16_t>(ArtifactKind::MaterialTable) + kindIndex),
            9000u + kindIndex, 0 }, revision, 10000u + arrivalIndex };
        patch.fragments[kindIndex] = fragment;
        Check(monotonicPublisher.PublishPatch(std::move(patch)));
        auto commit = monotonicPublisher.Commit(arrivalIndex % 3u);
        const auto& committed = commit.state->Fragment(kind);
        Check(committed.revision >= committedRevisions[kindIndex]);
        committedRevisions[kindIndex] = committed.revision;
        if (!capturedLease && kind == PublishedFragmentKind::Geometry && commit.committed) {
            capturedLease = commit.lease;
            capturedGeometry = committed;
        }
        if (capturedLease) {
            Check(capturedLease->state->geometry.publicationRoot ==
                capturedGeometry.publicationRoot);
        }
        commit.RunDeferred();
    }
    Check(monotonicPublisher.Stats().rejectedFragmentRegressions != 0);

    // The production regression sequence must never allow revision 7 to
    // replace already-visible revision 10 for the same static-scene address.
    RendererStatePublisher geometrySequencePublisher(2);
    const ArtifactAddress geometryAddress{ ArtifactKind::StaticScene, 9100, 0 };
    for (const std::uint64_t revision : { 5u, 6u, 7u, 9u, 10u }) {
        PublishedStatePatch patch;
        PublishedStateFragment fragment;
        fragment.revision = revision;
        fragment.publicationRoot = { geometryAddress, revision, 20000u + revision };
        patch.fragments[static_cast<std::size_t>(PublishedFragmentKind::Geometry)] = fragment;
        Check(geometrySequencePublisher.PublishPatch(std::move(patch)));
        auto commit = geometrySequencePublisher.Commit(revision % 2u);
        Check(commit.committed && commit.state->geometry.revision == revision);
        commit.RunDeferred();
    }
    const auto revision10Lease = geometrySequencePublisher.Commit(0).lease;
    PublishedStatePatch regressingGeometry;
    PublishedStateFragment revision7Geometry;
    revision7Geometry.revision = 7;
    revision7Geometry.publicationRoot = { geometryAddress, 7, 30007 };
    regressingGeometry.fragments[static_cast<std::size_t>(PublishedFragmentKind::Geometry)] =
        revision7Geometry;
    Check(geometrySequencePublisher.PublishPatch(regressingGeometry));
    auto rejectedGeometryRegression = geometrySequencePublisher.Commit(1);
    Check(!rejectedGeometryRegression.committed);
    Check(rejectedGeometryRegression.state->geometry.revision == 10);
    Check(revision10Lease->state->geometry.revision == 10);
    rejectedGeometryRegression.RunDeferred();

    // Rollback is deliberately a separate, reason-bearing operation.
    regressingGeometry.policy = ManifestPublicationPolicy::ExplicitRollback;
    Check(!geometrySequencePublisher.PublishPatch(regressingGeometry));
    regressingGeometry.reason = "test-only recovery rollback";
    Check(geometrySequencePublisher.PublishPatch(regressingGeometry));
    auto explicitRollback = geometrySequencePublisher.Commit(0);
    Check(explicitRollback.committed && explicitRollback.state->geometry.revision == 7);
    Check(geometrySequencePublisher.Stats().explicitRollbacks == 1);
    Check(revision10Lease->state->geometry.revision == 10);
    explicitRollback.RunDeferred();

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
    Check(manifestState->materials.publicationBundle != nullptr);
    Check(manifestState->publicationBundle != nullptr);
    Check(manifestState->materials.publicationBundle->root ==
        manifestState->materials.publicationRoot);
    Check(manifestState->materials.dependencyClosure.empty());
    Check(std::ranges::contains(manifestState->publicationBundle->parents,
        manifestState->materials.publicationBundle));
    Check(manifestState->publicationBundle->versions.empty());
    graph.MarkPublished(manifestState->materials.publicationRoot);
    graph.WaitIdle();
    Check(graph.Snapshot(manifestState->materials.publicationRoot).readiness ==
        ArtifactReadiness::Published);
    manifestCommit.RunDeferred();

    // Reproduce the selector failure: geometry 10 is visible, then a newer
    // material closure arrives whose exact dependency is geometry 7. The old
    // two-fragment score used to beat the one-fragment geometry-10 closure.
    const ArtifactAddress selectorGeometryAddress{ ArtifactKind::StaticScene, 9200, 0 };
    const auto geometry10 = FragmentSnapshot(PublishedFragmentKind::Geometry,
        selectorGeometryAddress, 10, 40010);
    requestService.OnArtifactReady(geometry10);
    graph.WaitIdle();
    auto geometry10Commit = manifestPublisher.Commit(1);
    Check(geometry10Commit.committed && geometry10Commit.state->geometry.revision == 10);
    geometry10Commit.RunDeferred();

    const auto geometry7 = FragmentSnapshot(PublishedFragmentKind::Geometry,
        selectorGeometryAddress, 7, 40007);
    const auto materialDependingOnGeometry7 = FragmentSnapshot(PublishedFragmentKind::Materials,
        { ArtifactKind::MaterialTable, 9201, 0 }, 100, 40100, { geometry7 });
    requestService.OnArtifactReady(materialDependingOnGeometry7);
    graph.WaitIdle();
    auto oldClosureCommit = manifestPublisher.Commit(0);
    Check(oldClosureCommit.state->geometry.revision == 10);
    Check(oldClosureCommit.state->geometry.publicationRoot == geometry10.Version());
    oldClosureCommit.RunDeferred();

    const auto materialDependingOnGeometry10 = FragmentSnapshot(PublishedFragmentKind::Materials,
        { ArtifactKind::MaterialTable, 9201, 0 }, 101, 40101, { geometry10 });
    requestService.OnArtifactReady(materialDependingOnGeometry10);
    graph.WaitIdle();
    auto coherentSuccessorCommit = manifestPublisher.Commit(1);
    Check(coherentSuccessorCommit.committed);
    Check(coherentSuccessorCommit.state->geometry.revision == 10);
    Check(coherentSuccessorCommit.state->materials.revision == 101);
    coherentSuccessorCommit.RunDeferred();
    // Historical manifest inputs must not turn metadata bases into permanent
    // semantic consumers. Keep the graph alive while retiring all real frames.
    {
        auto pin = std::make_shared<int>(1);
        const std::weak_ptr<int> retiredPin = pin;
        auto pinned = std::make_shared<PublishedRendererState>(*manifestPublisher.Active());
        pinned->epoch++;
        pinned->bindingBundle = std::make_shared<const org::PublicationBindingBundle>(
            std::vector<org::PublicationBindingBundle::Snapshot>{},
            std::vector<std::shared_ptr<const void>>{pin});
        pin.reset();
        Check(manifestPublisher.PublishCandidate({ manifestPublisher.ActiveEpoch(), pinned }));
        auto pinnedCommit = manifestPublisher.Commit(0);
        pinnedCommit.RunDeferred();
        PublishedStateResourceResolver idleResolver(manifestPublisher.ResourceSource(), PublishedResourceKey{});
        auto declaration = idleResolver.CaptureDeclarationState();
        const std::weak_ptr<const org::ResolverDeclarationState> declarationLifetime = declaration;
        declaration.reset();
        Check(declarationLifetime.expired()); // An idle resolver cache is not a consumer.
        const auto archivedDeclaration = idleResolver.CaptureDeclarationState();
        Check(!archivedDeclaration->publicationLease);
        requestService.OnArtifactReady(FragmentSnapshot(PublishedFragmentKind::Materials,
            { ArtifactKind::MaterialTable, 9201, 0 }, 102, 40102, { geometry10 }));
        graph.WaitIdle();
        auto rotated = manifestPublisher.Commit(1);
        Check(rotated.committed && rotated.state->materials.revision == 102);
        rotated.RunDeferred();
        pinned.reset();
        pinnedCommit.state.reset();
        pinnedCommit.lease.reset();
        Check(!retiredPin.expired()); // The selected frame still consumes it.
        manifestPublisher.ReleaseFrameSlot(0);
        Check(retiredPin.expired()); // Graph history must not consume it.
    }
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

    const auto traceDirectory = std::filesystem::temp_directory_path() /
        ("sarp_async_graph_trace_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    graph.StartTrace({ .maximumEvents = 10'000 });
    Check(graph.TraceActive());
    Check(static_cast<bool>(graph.Request(
        { ArtifactKind::MaterialTable, 101, 0 }, 8, {}, Payload(88), 88)));
    graph.WaitIdle();
    const auto traceReport = graph.StopTraceAndWriteReport(traceDirectory);
    Check(!graph.TraceActive());
    Check(traceReport.capturedEvents != 0 && traceReport.droppedEvents == 0);
    Check(std::filesystem::exists(traceReport.eventsCsv));
    Check(std::filesystem::exists(traceReport.staticGroupCsv));
    Check(std::filesystem::exists(traceReport.chromeTraceJson));
    Check(std::filesystem::exists(traceReport.summaryMarkdown));
    const auto readFile = [](const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), {});
    };
    const auto eventsCsv = readFile(traceReport.eventsCsv);
    Check(eventsCsv.contains("RequestAccepted"));
    Check(eventsCsv.contains("BuildCompleted"));
    Check(eventsCsv.contains("StateChanged"));
    Check(eventsCsv.contains("TaskScheduler"));
    Check(eventsCsv.contains("SchedulerTaskQueued"));
    Check(eventsCsv.contains("SchedulerTaskStarted"));
    Check(eventsCsv.contains("SchedulerTaskCompleted"));
    Check(readFile(traceReport.chromeTraceJson).contains("\"traceEvents\""));
    const auto traceSummary = readFile(traceReport.summaryMarkdown);
    Check(traceSummary.contains("Producer timing by artifact kind"));
    Check(traceSummary.contains("Unified scheduler execution"));

    graph.StartTrace({ .maximumEvents = 2 });
    Check(static_cast<bool>(graph.Request(
        { ArtifactKind::MaterialTable, 102, 0 }, 9, {}, Payload(99), 99)));
    graph.WaitIdle();
    const auto boundedTrace = graph.StopTraceAndWriteReport(traceDirectory / "bounded");
    Check(boundedTrace.capturedEvents == 2);
    Check(boundedTrace.droppedEvents != 0);

    graph.StartTrace({ .maximumEvents = 10'000 });
    std::atomic_bool traceWritersRunning{ true };
    std::atomic_uint64_t traceWriterCalls{ 0 };
    std::vector<std::jthread> traceWriters;
    for (std::uint64_t thread = 0; thread < 4; ++thread) {
        traceWriters.emplace_back([&, thread] {
            while (traceWritersRunning.load(std::memory_order_acquire)) {
                graph.TraceEvent(AsyncStateGraphTraceEventID::GrassCellIntentAccepted,
                    { ArtifactKind::GrassCell, thread, 0 },
                    traceWriterCalls.fetch_add(1, std::memory_order_relaxed) + 1);
            }
        });
    }
    while (traceWriterCalls.load(std::memory_order_acquire) < 10'000) std::this_thread::yield();
    const auto concurrentStopTrace = graph.StopTraceAndWriteReport(traceDirectory / "concurrent_stop");
    traceWritersRunning.store(false, std::memory_order_release);
    traceWriters.clear();
    Check(!graph.TraceActive());
    Check(concurrentStopTrace.capturedEvents != 0);
    Check(std::filesystem::exists(concurrentStopTrace.eventsCsv));

    {
        AsyncStateGraph latestGraph(scheduler, "LatestIntentTests");
        std::atomic_bool firstStarted{ false };
        std::atomic_bool releaseFirst{ false };
        latestGraph.RegisterProducer(ArtifactKind::Generic, {
            TaskLane::Streaming, TaskDomain::GraphPublication, "LatestIntentProducer",
            [&](const ArtifactBuildContext& context) {
                if (context.revision == 1) {
                    firstStarted.store(true, std::memory_order_release);
                    while (!releaseFirst.load(std::memory_order_acquire)) std::this_thread::yield();
                }
                return ArtifactBuildResult::Ready(Payload(context.revision));
            } });
        const ArtifactKey latestKey{ ArtifactKind::Generic, 0xf001, 0 };
        Check(latestGraph.SubmitLatestIntent(latestKey, 1, {}, Payload(1), 1) ==
            ArtifactRequestStatus::Accepted);
        while (!firstStarted.load(std::memory_order_acquire)) std::this_thread::yield();
        for (std::uint64_t revision = 2; revision <= 100; ++revision) {
            Check(latestGraph.SubmitLatestIntent(latestKey, revision, {}, Payload(revision), revision) ==
                ArtifactRequestStatus::Accepted);
        }
        releaseFirst.store(true, std::memory_order_release);
        latestGraph.WaitIdle();
        Check(latestGraph.Snapshot(latestKey).revision == 100);
        Check(latestGraph.Stats().coalescedIntents == 98);
        Check(latestGraph.Stats().supersededBuilds != 0);
        std::vector<ArtifactIntent> batch;
        batch.push_back({ { ArtifactKind::Generic, 0xf002, 0 }, 1, {}, Payload(11), 11 });
        batch.push_back({ { ArtifactKind::Generic, 0xf003, 0 }, 1, {}, Payload(12), 12 });
        const auto batchResults = latestGraph.SubmitLatestIntentBatch(std::move(batch));
        Check(batchResults.size() == 2);
        Check(static_cast<bool>(batchResults[0]));
        Check(static_cast<bool>(batchResults[1]));
        latestGraph.WaitIdle();
        Check(latestGraph.Snapshot(ArtifactKey{ ArtifactKind::Generic, 0xf002, 0 }).revision == 1);
        Check(latestGraph.Snapshot(ArtifactKey{ ArtifactKind::Generic, 0xf003, 0 }).revision == 1);
        Check(latestGraph.Stats().intentBatches == 1);

		const ArtifactKey absentDependency{ ArtifactKind::Generic, 0xf004, 0 };
		const ArtifactKey blockedLatestKey{ ArtifactKind::Generic, 0xf005, 0 };
		Check(latestGraph.SubmitLatestIntent(blockedLatestKey, 1, {
			Exact(ArtifactVersionID{ absentDependency, 1, 1 },
				ArtifactReadiness::GpuReady)
		}, Payload(1), 101) == ArtifactRequestStatus::Accepted);
		while (latestGraph.Snapshot(blockedLatestKey).readiness !=
			ArtifactReadiness::Blocked) std::this_thread::yield();
		Check(latestGraph.SubmitLatestIntent(
			blockedLatestKey, 2, {}, Payload(2), 102) ==
			ArtifactRequestStatus::Accepted);
		latestGraph.WaitIdle();
		const auto promotedLatest = latestGraph.Snapshot(blockedLatestKey);
		Check(promotedLatest.revision == 2 &&
			promotedLatest.readiness == ArtifactReadiness::GpuReady &&
			promotedLatest.payload.Get<Value>() &&
			promotedLatest.payload.Get<Value>()->value == 2);
        latestGraph.Shutdown();
    }

    {
        AsyncStateGraph eventGraph(scheduler, "SuspensionAcceptanceTests");
        std::atomic_uint32_t earlyAttempts{ 0 };
        std::atomic_uint32_t lateAttempts{ 0 };
        eventGraph.RegisterProducer(ArtifactKind::Generic, {
            TaskLane::Streaming, TaskDomain::GraphPublication, "SuspendingProducer",
            [&](const ArtifactBuildContext& context) {
                auto& attempts = context.key.primaryID == 0xf101 ? earlyAttempts : lateAttempts;
                if (attempts.fetch_add(1, std::memory_order_acq_rel) == 0) {
                    return ArtifactBuildResult::Suspend(ArtifactSuspension::External(
                        context.key.primaryID == 0xf101 ? 0xa101 : 0xa102,
                        "test external operation"));
                }
                return ArtifactBuildResult::Ready(Payload(context.revision));
            } });

        // The notification may race ahead of suspension registration.
        eventGraph.NotifySuspensionSatisfied(0xa101);
        const ArtifactKey earlyKey{ ArtifactKind::Generic, 0xf101, 0 };
        Check(static_cast<bool>(eventGraph.Request(earlyKey, 1, {}, Payload(1), 1)));
        eventGraph.WaitIdle();
        Check(earlyAttempts.load(std::memory_order_acquire) == 2);
        Check(eventGraph.Snapshot(earlyKey).readiness == ArtifactReadiness::GpuReady);

        const ArtifactKey lateKey{ ArtifactKind::Generic, 0xf102, 0 };
        Check(static_cast<bool>(eventGraph.Request(lateKey, 1, {}, Payload(1), 1)));
        while (lateAttempts.load(std::memory_order_acquire) != 1) std::this_thread::yield();
        while (eventGraph.Snapshot(lateKey).readiness != ArtifactReadiness::Blocked)
            std::this_thread::yield();
        eventGraph.NotifySuspensionSatisfied(0xa102);
        eventGraph.WaitIdle();
        Check(lateAttempts.load(std::memory_order_acquire) == 2);
        Check(eventGraph.Snapshot(lateKey).readiness == ArtifactReadiness::GpuReady);
        eventGraph.Shutdown();
    }

    {
        // Draw publication depends on resident geometry. ResidentGeometryCoverage
        // releases only waiters its coverage reaches, and never moves backwards.
        std::vector<std::uint64_t> notified;
        ResidentGeometryCoverage registry([&](std::uint64_t identity) { notified.push_back(identity); });
        const auto low = registry.AwaitIdentity(10);
        const auto high = registry.AwaitIdentity(20);
        Check(low != 0 && high != 0 && low != high);
        registry.Observe(15);
        Check(notified.size() == 1 && notified.front() == low);
        registry.Observe(12);
        Check(registry.Resident() == 15 && notified.size() == 1);
        Check(registry.AwaitIdentity(15) == 0);
        registry.Observe(20);
        Check(notified.size() == 2 && notified.back() == high);
    }

    {
        // A draw-records root holds an exact requirement on its coverage gate, so
        // it is not built until a committed Geometry root covers its templates:
        // not after partial coverage, and without being rejected or rebuilt.
        AsyncStateGraph gateGraph(scheduler, "GeometryCoverageGateTests");
        RegisterGeometryCoverageGateProducer(gateGraph);
        std::atomic_uint32_t consumerBuilds{ 0 };
        gateGraph.RegisterProducer(ArtifactKind::Generic, {
            TaskLane::Streaming, TaskDomain::GraphPublication, "GatedConsumer",
            [&](const ArtifactBuildContext& context) {
                consumerBuilds.fetch_add(1, std::memory_order_acq_rel);
                return ArtifactBuildResult::Ready(Payload(context.revision));
            } });
        auto resident = std::make_shared<ResidentGeometryCoverage>(gateGraph.MakeSuspensionNotifier());
        constexpr std::uint64_t kRequired = 40;
        auto gateInput = std::make_shared<GeometryCoverageGateInput>();
        gateInput->coverage = kRequired;
        gateInput->resident = resident;
        const ArtifactKey gateKey{ ArtifactKind::GeometryCoverageGate, kRequired, 0 };
        const auto gate = gateGraph.Request(gateKey, 1, {},
            ArtifactPayload::Make<GeometryCoverageGateInput>(std::move(gateInput)), kRequired);
        Check(static_cast<bool>(gate));
        const ArtifactKey consumerKey{ ArtifactKind::Generic, 0xf301, 0 };
        Check(static_cast<bool>(gateGraph.Request(consumerKey, 1,
            { Exact(gate.Handle(), ArtifactReadiness::CpuReady) }, Payload(1), 1)));
        while (gateGraph.Snapshot(gateKey).readiness != ArtifactReadiness::Blocked)
            std::this_thread::yield();
        resident->Observe(kRequired - 1);
        gateGraph.WaitIdle();
        Check(gateGraph.Snapshot(gateKey).readiness == ArtifactReadiness::Blocked);
        Check(consumerBuilds.load(std::memory_order_acquire) == 0);
        resident->Observe(kRequired + 5);
        gateGraph.WaitIdle();
        Check(gateGraph.Snapshot(gateKey).readiness == ArtifactReadiness::GpuReady);
        Check(gateGraph.Snapshot(consumerKey).readiness == ArtifactReadiness::GpuReady);
        Check(consumerBuilds.load(std::memory_order_acquire) == 1);

        // Coverage that is already resident satisfies a new gate immediately.
        auto coveredInput = std::make_shared<GeometryCoverageGateInput>();
        coveredInput->coverage = kRequired + 5;
        coveredInput->resident = resident;
        const ArtifactKey coveredKey{ ArtifactKind::GeometryCoverageGate, kRequired + 5, 0 };
        Check(static_cast<bool>(gateGraph.Request(coveredKey, 1, {},
            ArtifactPayload::Make<GeometryCoverageGateInput>(std::move(coveredInput)), kRequired + 5)));
        gateGraph.WaitIdle();
        Check(gateGraph.Snapshot(coveredKey).readiness == ArtifactReadiness::GpuReady);
        gateGraph.Shutdown();
    }

    {
        AsyncStateGraph acceptanceGraph(scheduler, "AcceptanceOrderingTests");
        std::atomic_bool acceptanceStarted{ false };
        std::atomic_bool releaseAcceptance{ false };
        acceptanceGraph.RegisterProducer(ArtifactKind::Generic, {
            TaskLane::Streaming, TaskDomain::GraphPublication, "AcceptanceProducer",
            [&](const ArtifactBuildContext& context) {
                auto result = ArtifactBuildResult::Ready(Payload(context.revision));
                result.acceptance = { TaskLane::Streaming, TaskDomain::RendererState,
                    [&](const ArtifactSnapshot&) {
                        acceptanceStarted.store(true, std::memory_order_release);
                        while (!releaseAcceptance.load(std::memory_order_acquire))
                            std::this_thread::yield();
                    } };
                return result;
            } });
        const ArtifactKey acceptanceKey{ ArtifactKind::Generic, 0xf201, 0 };
        Check(static_cast<bool>(acceptanceGraph.Request(
            acceptanceKey, 1, {}, Payload(1), 1)));
        while (!acceptanceStarted.load(std::memory_order_acquire)) std::this_thread::yield();
        // Readiness cannot escape GraphControl before the exact acceptance
        // acknowledgement has returned from RendererState.
        Check(acceptanceGraph.Snapshot(acceptanceKey).readiness ==
            ArtifactReadiness::Preparing);
        releaseAcceptance.store(true, std::memory_order_release);
        acceptanceGraph.WaitIdle();
        Check(acceptanceGraph.Snapshot(acceptanceKey).readiness == ArtifactReadiness::GpuReady);
        acceptanceGraph.Shutdown();
    }

    {
        CapacityProvider capacity(scheduler, "CapacityProviderTests", 1,
            TaskLane::Streaming, TaskDomain::GraphControl);
        std::mutex leaseMutex;
        CapacityLease blocker;
        std::atomic_bool blockerGranted{ false };
        Check(capacity.AcquireAsync({ 1, 0, 0, 1,
            { { ArtifactKind::Generic, 0xfc00, 0 }, 1, 1 } },
            [&](CapacityLease lease) {
                std::lock_guard lock(leaseMutex);
                blocker = std::move(lease);
                blockerGranted.store(true, std::memory_order_release);
            }));
        while (!blockerGranted.load(std::memory_order_acquire)) std::this_thread::yield();

        std::mutex orderMutex;
        std::vector<std::uint64_t> order;
        const auto enqueue = [&](std::uint64_t sequence, std::int32_t priority) {
            Check(capacity.AcquireAsync({ 1, priority, sequence, 1,
                { { ArtifactKind::Generic, 0xfc00 + sequence, 0 }, 1, sequence } },
                [&, sequence](CapacityLease lease) {
                    lease.Reset();
                    {
                        std::lock_guard lock(orderMutex);
                        order.push_back(sequence);
                    }
                }));
        };
        enqueue(3, 0);
        enqueue(2, 1);
        enqueue(1, 1);
        Check(capacity.Pending() == 3);
        {
            std::lock_guard lock(leaseMutex);
            blocker.Reset();
        }
        for (;;) {
            {
                std::lock_guard lock(orderMutex);
                if (order.size() == 3) break;
            }
            std::this_thread::yield();
        }
        {
            std::lock_guard lock(orderMutex);
            Check((order == std::vector<std::uint64_t>{ 1, 2, 3 }));
        }
        Check(capacity.Available() == 1);
        capacity.Shutdown();
    }

    {
        std::atomic_uint32_t commits{ 0 };
        std::atomic_uint32_t cancels{ 0 };
        {
            auto reservation = std::make_shared<MaterialUsageReservation>(
                std::make_shared<PublishedMaterialUsageBatch>(),
                [&](bool commit) {
                    (commit ? commits : cancels).fetch_add(1, std::memory_order_relaxed);
                    return true;
                });
            Check(reservation->Commit());
            Check(reservation->Commit());
        }
        Check(commits.load(std::memory_order_relaxed) == 1);
        Check(cancels.load(std::memory_order_relaxed) == 0);
        {
            auto reservation = std::make_shared<MaterialUsageReservation>(
                std::make_shared<PublishedMaterialUsageBatch>(),
                [&](bool commit) {
                    (commit ? commits : cancels).fetch_add(1, std::memory_order_relaxed);
                    return true;
                });
        }
        Check(commits.load(std::memory_order_relaxed) == 1);
        Check(cancels.load(std::memory_order_relaxed) == 1);

        commits.store(0, std::memory_order_relaxed);
        auto reservation = std::make_shared<MaterialUsageReservation>(
            std::make_shared<PublishedMaterialUsageBatch>(),
            [&](bool commit) {
                Check(commit);
                commits.fetch_add(1, std::memory_order_relaxed);
                return true;
            });
        std::vector<std::thread> committers;
        for (unsigned index = 0; index != 8; ++index) {
            committers.emplace_back([reservation] { Check(reservation->Commit()); });
        }
        for (auto& committer : committers) committer.join();
        Check(commits.load(std::memory_order_relaxed) == 1);

        RegisterMaterialUsageBatchProducer(graph);
        auto admitted = std::make_shared<PublishedMaterialUsageBatch>();
        admitted->sourceFingerprint = 0xabcdu;
        admitted->materialSlots = { { 17u, 3u }, { 29u, 4u } };
        commits.store(0, std::memory_order_relaxed);
        auto graphReservation = std::make_shared<MaterialUsageReservation>(admitted,
            [&](bool commit) {
                Check(commit);
                commits.fetch_add(1, std::memory_order_relaxed);
                return true;
            });
        auto input = std::make_shared<MaterialUsageBatchBuildInput>();
        input->sourceFingerprint = admitted->sourceFingerprint;
        input->reservation = graphReservation;
        const ArtifactKey batchKey{ ArtifactKind::MaterialUsageBatch, 0xfa11u, 0 };
        const auto batchRequest = graph.Request(batchKey, 1, {},
            ArtifactPayload::Make<MaterialUsageBatchBuildInput>(std::move(input)),
            admitted->sourceFingerprint);
        Check(batchRequest);
        graph.WaitIdle();
        const auto batchSnapshot = graph.Snapshot(batchRequest.version);
        const auto publishedBatch = batchSnapshot.payload.Get<PublishedMaterialUsageBatch>();
        Check(ArtifactReachedMilestone(batchSnapshot.readiness, ArtifactReadiness::CpuReady));
        Check(publishedBatch && publishedBatch->materialSlots == admitted->materialSlots);
        Check(commits.load(std::memory_order_relaxed) == 1);

        RegisterMaterialRowProducer(graph);
        auto row = std::make_shared<MaterialRowArtifact>();
        row->materialID = 17u;
        row->materialSlot = 3u;
        row->sourceRevision = 7u;
        commits.store(0, std::memory_order_relaxed);
        auto rowReservation = std::make_shared<MaterialRowReservation>(row,
            [&](bool commit) {
                Check(commit);
                commits.fetch_add(1, std::memory_order_relaxed);
                return true;
            });
        auto rowInput = std::make_shared<MaterialRowInput>();
        rowInput->materialID = row->materialID;
        rowInput->materialSlot = row->materialSlot;
        rowInput->sourceRevision = row->sourceRevision;
        rowInput->reservation = rowReservation;
        const ArtifactKey rowKey{ ArtifactKind::Material, row->materialID, 0 };
        const auto rowRequest = graph.Request(rowKey, row->sourceRevision, {},
            ArtifactPayload::Make<MaterialRowInput>(std::move(rowInput)), 0x7711u);
        Check(rowRequest);
        graph.WaitIdle();
        const auto rowSnapshot = graph.Snapshot(rowRequest.version);
        const auto publishedRow = rowSnapshot.payload.Get<MaterialRowArtifact>();
        Check(ArtifactReachedMilestone(rowSnapshot.readiness, ArtifactReadiness::CpuReady));
        Check(publishedRow && publishedRow->materialID == row->materialID &&
            publishedRow->materialSlot == row->materialSlot &&
            publishedRow->sourceRevision == row->sourceRevision);
        Check(commits.load(std::memory_order_relaxed) == 1);
    }

    {
        RegisterGeometryResidencyStateProducer(graph);
        const ArtifactAddress key{ ArtifactKind::GeometryResidency, 0xfeedu, 0 };
        const auto request = [&](std::uint64_t revision,
            GeometryResidencyDeltaInput value, ArtifactVersionHandle predecessor = {}) {
            std::vector<ArtifactRequirement> requirements;
            if (predecessor) requirements.push_back(Exact(predecessor));
            auto input = std::make_shared<const GeometryResidencyDeltaInput>(std::move(value));
            return graph.Request(key, revision, std::move(requirements),
                ArtifactPayload::Make<GeometryResidencyDeltaInput>(std::move(input)), revision);
        };
        auto first = request(1, { GeometryResidencyDeltaKind::AddOrReplace,
            { 10, 4, 3, {} }, {} });
        Check(first);
        graph.WaitIdle();
        graph.MarkPublished(first.version);
        graph.WaitIdle();
        auto second = request(2, { GeometryResidencyDeltaKind::AddOrReplace,
            { 20, 8, 7, {} }, {} }, first.Handle());
        Check(second);
        graph.WaitIdle();
        graph.MarkPublished(second.version);
        graph.WaitIdle();
        auto snapshot = graph.Snapshot(second.version);
        auto root = snapshot.payload.Get<RendererStateFragmentArtifact>();
        auto state = root
            ? root->fragment.payload.Get<PublishedGeometryResidencyState>() : nullptr;
        Check(state && state->activeRanges.size() == 2);
        Check(state->maxTraversalDepth == 7 && state->maxGroupIndex == 28);

        auto third = request(3, { GeometryResidencyDeltaKind::Remove,
            { 20, 8, 7, {} }, {} }, second.Handle());
        Check(third);
        graph.WaitIdle();
        snapshot = graph.Snapshot(third.version);
        root = snapshot.payload.Get<RendererStateFragmentArtifact>();
        state = root ? root->fragment.payload.Get<PublishedGeometryResidencyState>() : nullptr;
        Check(state && state->activeRanges.size() == 1);
        Check(state->activeRanges.front().groupsBase == 10);
        Check(state->maxTraversalDepth == 3 && state->maxGroupIndex == 14);

        // The exact predecessor remains immutable while a successor is live.
        snapshot = graph.Snapshot(second.version);
        root = snapshot.payload.Get<RendererStateFragmentArtifact>();
        state = root ? root->fragment.payload.Get<PublishedGeometryResidencyState>() : nullptr;
        Check(state && state->activeRanges.size() == 2 && state->maxTraversalDepth == 7);

        // Queue multiple deltas before their predecessors publish. Promotion
        // must resolve each exact archived predecessor without losing history.
        auto fourth = request(4, { GeometryResidencyDeltaKind::AddOrReplace,
            { 30, 2, 5, {} }, {} }, third.Handle());
        auto fifth = request(5, { GeometryResidencyDeltaKind::AddOrReplace,
            { 40, 3, 9, {} }, {} }, fourth.Handle());
        Check(fourth && fifth);
        graph.MarkPublished(third.version);
        graph.WaitIdle();
        graph.MarkPublished(fourth.version);
        graph.WaitIdle();
        graph.MarkPublished(fifth.version);
        graph.WaitIdle();
        snapshot = graph.Snapshot(fifth.version);
        root = snapshot.payload.Get<RendererStateFragmentArtifact>();
        state = root ? root->fragment.payload.Get<PublishedGeometryResidencyState>() : nullptr;
        Check(state && state->activeRanges.size() == 3);
        Check(state->maxTraversalDepth == 9 && state->maxGroupIndex == 43);

        // A residency publication owns the geometry storage version whose
        // physical page indices it exposes. Use an aliasing shared pointer so
        // this test can verify lifetime without constructing GPU resources.
        auto storageSentinel = std::make_shared<int>(42);
        std::weak_ptr<int> weakStorage = storageSentinel;
        std::shared_ptr<PagePool> pagePoolLease(
            storageSentinel, reinterpret_cast<PagePool*>(storageSentinel.get()));
        GeometryResidencyDeltaInput retainedInput{
            GeometryResidencyDeltaKind::AddOrReplace, { 50, 1, 4, {} }, {}};
        retainedInput.pagePool = pagePoolLease;
        retainedInput.storageGeneration = 77;
        auto retained = request(6, std::move(retainedInput), fifth.Handle());
        Check(retained);
        pagePoolLease.reset();
        storageSentinel.reset();
        graph.WaitIdle();
        snapshot = graph.Snapshot(retained.version);
        root = snapshot.payload.Get<RendererStateFragmentArtifact>();
        state = root ? root->fragment.payload.Get<PublishedGeometryResidencyState>() : nullptr;
        Check(state && state->pagePool && state->storageGeneration == 77);
        Check(!weakStorage.expired());
    }

    {
        RegisterViewStateProducer(graph);
        const ArtifactAddress key{ ArtifactKind::ViewFamily, 1u, 0u };
        auto firstInput = std::make_shared<ViewFamilyBuildInput>();
        firstInput->revision = 11;
        firstInput->cameraBufferSize = 4;
        firstInput->resourceLayoutRevision = 11;
        firstInput->views.push_back({ .id = 17, .cameraBufferIndex = 3, .primary = true });
        const auto first = graph.Request(key, 11, {},
            ArtifactPayload::Make<ViewFamilyBuildInput>(firstInput), 11);
        Check(first);
        graph.WaitIdle();
        auto snapshot = graph.Snapshot(first.version);
        auto root = snapshot.payload.Get<RendererStateFragmentArtifact>();
        auto family = root ? root->fragment.payload.Get<PublishedViewFamilyState>() : nullptr;
        Check(family && family->views.size() == 1 && family->views.front().id == 17);

        auto secondInput = std::make_shared<ViewFamilyBuildInput>(*firstInput);
        secondInput->revision = 12;
        secondInput->resourceLayoutRevision = 12;
        secondInput->views.front().id = 23;
        const auto second = graph.Request(key, 12, {},
            ArtifactPayload::Make<ViewFamilyBuildInput>(secondInput), 12);
        Check(second);
        graph.WaitIdle();
        snapshot = graph.Snapshot(second.version);
        root = snapshot.payload.Get<RendererStateFragmentArtifact>();
        family = root ? root->fragment.payload.Get<PublishedViewFamilyState>() : nullptr;
        Check(family && family->resourceLayoutRevision == 12 &&
            family->views.front().id == 23);
        snapshot = graph.Snapshot(first.version);
        root = snapshot.payload.Get<RendererStateFragmentArtifact>();
        family = root ? root->fragment.payload.Get<PublishedViewFamilyState>() : nullptr;
        Check(family && family->views.front().id == 17);
    }

    {
        RegisterPoseStateProducer(graph);
        const ArtifactAddress key{ ArtifactKind::PoseState, 1u, 0u };
        auto input = std::make_shared<PoseStateBuildInput>();
        input->activeInstanceRevision = 7;
        input->activeInstances.push_back({ .instanceSlot = 9, .boneCount = 42 });
        input->tableImages.push_back(std::make_shared<const std::vector<std::byte>>(
            std::initializer_list<std::byte>{std::byte{0x33}}));
        const auto request = graph.Request(key, 7, {},
            ArtifactPayload::Make<PoseStateBuildInput>(input), 7);
        Check(request);
        graph.WaitIdle();
        const auto snapshot = graph.Snapshot(request.version);
        const auto root = snapshot.payload.Get<RendererStateFragmentArtifact>();
        const auto poses = root ? root->fragment.payload.Get<PublishedPoseState>() : nullptr;
        Check(poses && poses->activeInstanceRevision == 7 &&
            poses->activeInstances.front().instanceSlot == 9 &&
            poses->tableImages.front()->front() == std::byte{0x33});
    }

    {
        RegisterLightStateProducer(graph);
        const ArtifactAddress viewKey{ ArtifactKind::ViewFamily, 2u, 0u };
        auto viewInput = std::make_shared<ViewFamilyBuildInput>();
        viewInput->revision = 21;
        viewInput->resourceLayoutRevision = 4;
        viewInput->views.push_back({ .id = 91, .cameraBufferIndex = 2 });
        const auto selectedViews = graph.Request(viewKey, 21, {},
            ArtifactPayload::Make<ViewFamilyBuildInput>(viewInput), 21);
        Check(selectedViews);

        const ArtifactAddress key{ ArtifactKind::LightTable, 1u, 0u };
        auto input = std::make_shared<LightTableBuildInput>();
        input->revision = 5;
        input->lightCount = 3;
        input->tableImages.push_back(std::make_shared<const std::vector<std::byte>>(
            std::initializer_list<std::byte>{std::byte{0x44}}));
        const auto request = graph.Request(key, 5,
            { Exact(selectedViews.Handle()) },
            ArtifactPayload::Make<LightTableBuildInput>(input), 5);
        Check(request);
        graph.WaitIdle();
        const auto snapshot = graph.Snapshot(request.version);
        const auto root = snapshot.payload.Get<RendererStateFragmentArtifact>();
        const auto lights = root ? root->fragment.payload.Get<PublishedLightTableState>() : nullptr;
        Check(lights && lights->revision == 5 && lights->lightCount == 3 &&
            lights->tableImages.front()->front() == std::byte{0x44});

        auto newerViewInput = std::make_shared<ViewFamilyBuildInput>(*viewInput);
        newerViewInput->revision = 22;
        newerViewInput->views.front().id = 92;
        Check(graph.Request(viewKey, 22, {},
            ArtifactPayload::Make<ViewFamilyBuildInput>(newerViewInput), 22));
        graph.WaitIdle();
        const auto retainedLight = graph.Snapshot(request.version);
        const auto retainedRoot = retainedLight.payload.Get<RendererStateFragmentArtifact>();
        const auto retainedState = retainedRoot
            ? retainedRoot->fragment.payload.Get<PublishedLightTableState>() : nullptr;
        Check(retainedState && retainedState->revision == 5);
    }

    {
        flecs::world sourceWorld;
        SceneSourceStateStore::PhaseMap phases;
        SceneSourceStateStore store;
        store.Configure(sourceWorld, phases);
        {
            auto lease = store.BeginBatch(10, 100);
            Check(&lease.World() == &sourceWorld);
            Check(&lease.Phases() == &phases);
            Check(lease.Revision() == 10);
            lease.Commit();
        }
        bool rejectedOutOfOrder = false;
        try {
            (void)store.BeginBatch(9, 90);
        } catch (const std::logic_error&) {
            rejectedOutOfOrder = true;
        }
        Check(rejectedOutOfOrder);
        {
            auto abandoned = store.BeginBatch(11, 110);
            Check(!abandoned.IsReplay());
        }
        {
            // An abandoned batch does not advance the committed revision.
            auto lease = store.BeginBatch(10, 100);
            Check(lease.IsReplay());
        }
        {
            auto lease = store.BeginBatch(11, 111);
            Check(lease.Revision() == 11);
            lease.Commit();
        }
        {
            auto replay = store.BeginBatch(11, 111);
            Check(replay.IsReplay());
        }
        bool rejectedConflict = false;
        try {
            (void)store.BeginBatch(11, 112);
        } catch (const std::logic_error&) {
            rejectedConflict = true;
        }
        Check(rejectedConflict);
        {
            auto internal = store.BeginBatch(0, 120);
            Check(internal.Revision() == 12);
            internal.Commit();
        }
        store.Reset();
        Check(!store.Available());
    }

    graph.Shutdown();
    scheduler.Cleanup();
    return 0;
}
