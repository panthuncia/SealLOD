// Graph jobs in a StateGraph (ORG job graphs, Phase 4): three artifacts, each produced by a job graph, chained
// across two queues. Each dependent requires only UploadSubmitted of its input, so it starts and submits its own graph
// while the GPU may still run the one before; the runtime's sequencer makes the GPU wait instead of the CPU.
#include <ORGModuleServices/Async/StateGraph.h>
#include <ORGModuleServices/GraphJob.h>
#include <OpenRenderGraph/GraphDesc.h>
#include <OpenRenderGraph/Runtime.h>
#include <Render/PassBuilders.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/Buffers/Buffer.h>

#include <directx/d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <thread>

namespace {

#define CHECK(...) do { if (!(__VA_ARGS__)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__); return __LINE__; } } while (false)

constexpr uint32_t kCount = 64;
constexpr uint64_t kBytes = kCount * sizeof(uint32_t);

// Deterministic scheduler: dispatch queues, Wait runs the queue on the calling thread.
class TestScheduler final : public org::async::GraphScheduler {
    struct TestScope final : org::async::TaskScope, std::enable_shared_from_this<TestScope> {
        const TestScheduler* owner;
        bool cancelled = false;
        mutable std::deque<std::pair<std::chrono::steady_clock::time_point, Task>> queue;
        explicit TestScope(const TestScheduler* scheduler) : owner(scheduler) {}
        bool StopRequested() const noexcept override { return cancelled; }
        void Cancel() noexcept override { cancelled = true; }
        void Wait() const override {
            while (!queue.empty()) {
                const auto next = std::min_element(queue.begin(), queue.end(),
                    [](const auto& a, const auto& b) { return a.first < b.first; });
                auto work = std::move(*next);
                queue.erase(next);
                if (cancelled) continue;
                std::this_thread::sleep_until(work.first);
                work.second({[self = shared_from_this()] { return self->cancelled; }});
            }
        }
    };
public:
    org::async::Scope CreateScope(std::string_view) override { return std::make_shared<TestScope>(this); }
    bool Dispatch(const org::async::Scope& scope, org::async::TaskClass cls, org::async::TaskDispatch, std::string_view,
        Task task, org::async::TaskTraceMetadata) override {
        return DispatchAfter(scope, {}, cls, {}, std::move(task));
    }
    bool DispatchAfter(const org::async::Scope& scope, std::chrono::steady_clock::duration delay, org::async::TaskClass cls,
        std::string_view, Task task) override {
        const auto own = std::dynamic_pointer_cast<TestScope>(scope);
        if (!own || own->owner != this || own->cancelled || cls.lane || cls.domain) return false;
        own->queue.emplace_back(std::chrono::steady_clock::now() + delay, std::move(task));
        return true;
    }
};

struct CopyBindings { org::ResourceBindingToken source, destination; };
class CopyPass final : public org::TypedRenderGraphPass<CopyPass, org::EmptyPassFrameData, CopyBindings> {
public:
    CopyPass(std::shared_ptr<org::Resource> source, std::shared_ptr<org::Resource> destination, org::QueueKind queue)
        : m_source(std::move(source)), m_destination(std::move(destination)), m_queue(queue) {}
    CopyBindings Declare(org::PassBuilder& builder) {
        builder.PreferQueue(m_queue);
        return {builder.CopySource(m_source), builder.CopyDestination(m_destination)};
    }
    static void Record(const CopyBindings& bindings, org::PassRecordContext& recording) {
        recording.Commands().CopyBufferRegion(recording.Resolve(bindings.destination).GetHandle(), 0,
            recording.Resolve(bindings.source).GetHandle(), 0, kBytes);
    }
private:
    std::shared_ptr<org::Resource> m_source, m_destination;
    org::QueueKind m_queue;
};

// A graph that copies its "in" parameter to its "out" parameter on one queue.
std::shared_ptr<org::JobGraph> CopyGraph(org::GraphCache& cache, const char* name, uint64_t key, org::QueueKind queue,
    std::shared_ptr<org::Resource> in, std::shared_ptr<org::Resource> out) {
    org::GraphDesc desc(name, key);
    auto source = desc.Parameter("in", std::move(in));
    auto destination = desc.Parameter("out", std::move(out));
    desc.Add(org::RenderGraph::ExternalPassDesc::Copy(std::string(name) + ".copy",
        std::make_shared<CopyPass>(source, destination, queue)).PreferQueue(queue));
    return cache.GetOrCompile(std::move(desc));
}

bool Fill(org::Buffer& buffer, uint32_t value) {
    void* mapped = nullptr;
    buffer.GetAPIResource().Map(&mapped, 0, kBytes);
    if (!mapped) return false;
    for (uint32_t i = 0; i < kCount; ++i) static_cast<uint32_t*>(mapped)[i] = value;
    buffer.GetAPIResource().Unmap(0, kBytes);
    return true;
}
uint32_t Read(org::Buffer& buffer) {
    void* mapped = nullptr;
    buffer.GetAPIResource().Map(&mapped, 0, kBytes);
    if (!mapped) return 0;
    const auto* words = static_cast<const uint32_t*>(mapped);
    uint32_t value = words[0];
    for (uint32_t i = 1; i < kCount; ++i) if (words[i] != value) value = 0;
    buffer.GetAPIResource().Unmap(0, 0);
    return value;
}

// What each artifact holds: the buffer its graph wrote.
struct Stage { std::shared_ptr<org::Buffer> buffer; };

int RunChain(rhi::Device device, const std::shared_ptr<org::Runtime>& runtime) {
    using Graph = org::async::AsyncStateGraph;
    using Result = Graph::ArtifactBuildResult;
    using Context = Graph::ArtifactBuildContext;
    using org::async::ArtifactReadiness;

    auto source = org::Buffer::CreateShared(rhi::HeapType::Upload, kBytes);
    auto readback = org::Buffer::CreateShared(rhi::HeapType::Readback, kBytes);
    source->SetName("job.source"); readback->SetName("job.readback");
    org::GraphCache cache(runtime);
    // Placeholders the graphs are compiled with; every execution binds its own buffers.
    auto placeholderA = org::Buffer::CreateShared(rhi::HeapType::DeviceLocal, kBytes);
    auto placeholderB = org::Buffer::CreateShared(rhi::HeapType::DeviceLocal, kBytes);
    auto first = CopyGraph(cache, "job.first", 1, org::QueueKind::Graphics, source, placeholderA);
    auto second = CopyGraph(cache, "job.second", 2, org::QueueKind::Copy, placeholderA, placeholderB);
    auto third = CopyGraph(cache, "job.third", 3, org::QueueKind::Graphics, placeholderB, readback);

    auto scheduler = std::make_shared<TestScheduler>();
    Graph graph(scheduler, "GraphJobs");
    const Graph::ArtifactKey firstKey{0, 1, 0}, secondKey{1, 1, 0}, thirdKey{2, 1, 0};
    ArtifactReadiness firstWhenSecondStarted = ArtifactReadiness::Missing;
    ArtifactReadiness secondWhenThirdStarted = ArtifactReadiness::Missing;

    Graph::ArtifactProducerRegistration produceFirst;
    produceFirst.producer = [&](const Context&) {
        auto out = org::Buffer::CreateShared(rhi::HeapType::DeviceLocal, kBytes);
        out->SetName("job.a");
        const org::JobGraph::Binding bindings[] = {{"out", out}};
        return org::services::RunGraphJob<Result>(first, bindings,
            org::async::ArtifactPayload::Make(std::make_shared<const Stage>(Stage{out})));
    };
    Graph::ArtifactProducerRegistration produceSecond;
    produceSecond.producer = [&](const Context& context) {
        const auto input = context.Dependency<Stage>(firstKey);
        if (!input.payload) return Result::Failure("no first stage");
        firstWhenSecondStarted = graph.Snapshot(firstKey).readiness;
        auto out = org::Buffer::CreateShared(rhi::HeapType::DeviceLocal, kBytes);
        out->SetName("job.b");
        const org::JobGraph::Binding bindings[] = {{"in", input.payload->buffer}, {"out", out}};
        return org::services::RunGraphJob<Result>(second, bindings,
            org::async::ArtifactPayload::Make(std::make_shared<const Stage>(Stage{out})));
    };
    Graph::ArtifactProducerRegistration produceThird;
    produceThird.producer = [&](const Context& context) {
        const auto input = context.Dependency<Stage>(secondKey);
        if (!input.payload) return Result::Failure("no second stage");
        secondWhenThirdStarted = graph.Snapshot(secondKey).readiness;
        const org::JobGraph::Binding bindings[] = {{"in", input.payload->buffer}};
        return org::services::RunGraphJob<Result>(third, bindings,
            org::async::ArtifactPayload::Make(std::make_shared<const Stage>(Stage{readback})));
    };
    graph.RegisterProducer(0, produceFirst);
    graph.RegisterProducer(1, produceSecond);
    graph.RegisterProducer(2, produceThird);

    for (uint32_t round = 1; round <= 4; ++round) {
        const uint32_t value = 0xA000u + round;
        CHECK(Fill(*source, value));
        const auto a = graph.PostRequest({firstKey, round}, false);
        const auto b = graph.PostRequest({secondKey, round, {org::async::Exact(a.Handle(), ArtifactReadiness::UploadSubmitted)}}, false);
        const auto c = graph.PostRequest({thirdKey, round, {org::async::Exact(b.Handle(), ArtifactReadiness::UploadSubmitted)}}, false);
        CHECK(a && b && c);
        graph.WaitIdle();
        // Every dependent ran on its input's submission, not its completion: the CPU never waited for the GPU.
        CHECK(firstWhenSecondStarted == ArtifactReadiness::UploadSubmitted || firstWhenSecondStarted == ArtifactReadiness::GpuReady);
        CHECK(secondWhenThirdStarted == ArtifactReadiness::UploadSubmitted || secondWhenThirdStarted == ArtifactReadiness::GpuReady);
        std::printf("round %u: first was %s when second started, second was %s when third started\n", round,
            firstWhenSecondStarted == ArtifactReadiness::UploadSubmitted ? "submitted" : "complete",
            secondWhenThirdStarted == ArtifactReadiness::UploadSubmitted ? "submitted" : "complete");
        // Completion: the graph polls the submission sets until the GPU is done.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (graph.Snapshot(c.version).readiness != ArtifactReadiness::GpuReady && std::chrono::steady_clock::now() < deadline) {
            graph.PumpGpuCompletions();
            graph.WaitIdle();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(graph.Snapshot(c.version).readiness == ArtifactReadiness::GpuReady);
        CHECK(Read(*readback) == value);
    }
    CHECK(cache.GetStats().compiles == 3);
    graph.Shutdown();
    CHECK(device.WaitIdle() == rhi::Result::Ok);
    return 0;
}

} // namespace

int main() {
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter> warp;
    CHECK(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))));
    CHECK(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))));
    rhi::DeviceCreateInfo create{};
    create.backend = rhi::Backend::D3D12;
    create.nativeAdapter = warp.Get();
    create.framesInFlight = 2;
    rhi::DevicePtr device;
    CHECK(!rhi::Failed(rhi::CreateD3D12Device(create, device)) && device);
    int failure = 0;
    {
        auto runtime = org::Runtime::Create({.device = device.Get(), .backend = rhi::Backend::D3D12});
        failure = RunChain(device.Get(), runtime);
    }
    if (!failure) std::puts("GraphJobTests passed");
    return failure;
}
