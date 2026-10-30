#include "Runtime/GraphIntegration/TextureJobGraph.h"

#include "Assets/Textures/TextureFactory.h"
#include "OpenRenderGraph/GraphHost.h"
#include "OpenRenderGraph/Runtime.h"
#include "Render/PassExecutionContext.h"
#include "Render/RenderGraph/RenderGraph.h"
#include "Render/RenderGraph/RenderGraphExtension.h"
#include "Render/Runtime/IUploadService.h"
#include "Render/Runtime/ScopedActiveGraphServices.h"

#include <BasicTelemetry/Tracy.h>
#include <spdlog/spdlog.h>

namespace br::render {

class TextureJobGraph::Extension final : public org::IRenderGraphExtension {
public:
    Extension(TextureFactory& textures, std::shared_ptr<org::runtime::IUploadService> uploads)
        : m_textures(&textures), m_uploads(std::move(uploads)) {}

    void OnRegistryReset(org::ResourceRegistry* registry) override {
        org::runtime::UploadResolveContext context;
        context.registry = registry;
        context.epoch = 0;
        m_uploads->SetUploadResolveContext(context);
    }

    void GatherStructuralPasses(org::RenderGraph&, std::vector<org::ExternalPassDesc>& out) override {
        if (auto pass = m_uploads->GetUploadPass())
            out.push_back(org::ExternalPassDesc::Render("TextureJobs::Uploads", pass)
                .At(org::ExternalInsertPoint::Begin(0)));
        if (auto pass = m_textures->GetMipmappingPass())
            out.push_back(org::ExternalPassDesc::Compute("TextureJobs::Mipmapping", pass)
                .At(org::ExternalInsertPoint::Begin(1)));
        if (auto pass = m_textures->GetMipHandoffPass())
            out.push_back(org::ExternalPassDesc::Render("TextureJobs::MipHandoff", pass)
                .At(org::ExternalInsertPoint::Begin(2)));
        if (auto pass = m_textures->GetBC7CompressionPass())
            out.push_back(org::ExternalPassDesc::Compute("TextureJobs::BC7Compression", pass)
                .At(org::ExternalInsertPoint::Begin(3)));
        if (auto pass = m_textures->GetBC7CompressionCopyPass())
            out.push_back(org::ExternalPassDesc::Render("TextureJobs::BC7CompressionCopy", pass)
                .At(org::ExternalInsertPoint::Begin(4)));
        if (auto pass = m_textures->GetBC7CompressionReadbackPass())
            out.push_back(org::ExternalPassDesc::Copy("TextureJobs::BC7CompressionReadback", pass)
                .At(org::ExternalInsertPoint::Begin(5))
                .PinToQueue(static_cast<org::QueueSlotIndex>(2)));
    }

private:
    TextureFactory* m_textures;
    std::shared_ptr<org::runtime::IUploadService> m_uploads;
};

TextureJobGraph::TextureJobGraph(std::shared_ptr<org::Runtime> runtime, TextureFactory& textures, uint32_t slots)
    : m_textures(&textures), m_runtime(std::move(runtime)), m_uploads(org::runtime::CreateGraphUploadService()), m_slotPoints(slots) {
    m_uploads->Initialize();
    m_graph = std::make_unique<org::RenderGraph>(m_runtime);
    m_graph->SetDrivesFrameServices(false);
    m_graph->SetUploadService(m_uploads);
    m_graph->RegisterExtension(std::make_unique<Extension>(textures, m_uploads), "TextureJobs");
    org::runtime::ScopedActiveGraphServices services(m_graph->GetUploadService(), m_graph->GetDescriptorService());
    org::GraphHost::Build(*m_graph, [](org::RenderGraph& graph) { graph.PrepareExtensionsForBuild(); });
}

TextureJobGraph::~TextureJobGraph() {
    // Its executions complete before the graph goes (the graph retires its queue timelines with it).
    for (auto& points : m_slotPoints)
        for (auto& [timeline, value] : points)
            if (timeline && *timeline) timeline->Get().HostWait(value);
    m_graph.reset();
    if (m_uploads) m_uploads->Cleanup();
    spdlog::info("TextureJobGraph: {} executions recorded work ({} batches)", m_executionsWithWork, m_batches);
}

void TextureJobGraph::Execute(uint32_t slot, uint64_t frameFenceValue, rhi::Device device) {
    BT_ZONE_SCOPE("BR.TextureJobs.Execute");
    if (!m_textures->HasPendingGpuJobs()) {
        m_graph->RetirePersistentExecutions();
        return;
    }
    auto& points = m_slotPoints.at(slot);
    {
        // A full frame cycle has passed since this slot's last execution: it has almost always completed.
        BT_ZONE_SCOPE("BR.TextureJobs.WaitForSlot");
        for (auto& [timeline, value] : points)
            if (timeline && *timeline) timeline->Get().HostWait(value);
        points.clear();
    }
    m_uploads->ProcessDeferredReleases(static_cast<uint8_t>(slot));

    org::runtime::ScopedActiveGraphServices services(m_graph->GetUploadService(), m_graph->GetDescriptorService());
    org::UpdateExecutionContext update{};
    update.frameIndex = static_cast<uint8_t>(slot);
    update.preparationSlot = static_cast<uint8_t>(slot);
    update.frameFenceValue = frameFenceValue;
    org::GraphHost::PrepareFrame(*m_graph, update, device);
    org::PassExecutionContext execute{};
    execute.device = device;
    execute.frameIndex = static_cast<uint8_t>(slot);
    execute.executionSlot = slot;
    execute.frameFenceValue = frameFenceValue;
    org::GraphHost::ExecuteFrame(*m_graph, execute);
    if (const auto batches = m_graph->LastPersistentExecuteTimings().batches) {
        ++m_executionsWithWork;
        m_batches += batches;
    }

    const auto& queues = m_graph->GetQueueRegistry();
    for (size_t index = 0; index < queues.SlotCount(); ++index) {
        const auto queue = static_cast<org::QueueSlotIndex>(static_cast<uint8_t>(index));
        if (const auto next = queues.GetCurrentFenceValue(queue); next > 1)
            points.emplace_back(queues.GetFenceOwner(queue), next - 1);
    }
}

} // namespace br::render
