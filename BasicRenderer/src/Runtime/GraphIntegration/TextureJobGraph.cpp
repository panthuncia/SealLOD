#include "Runtime/GraphIntegration/TextureJobGraph.h"

#include "Assets/Textures/TextureFactory.h"
#include "OpenRenderGraph/GraphHost.h"
#include "OpenRenderGraph/Runtime.h"
#include "Render/PassExecutionContext.h"
#include "Render/RenderGraph/RenderGraph.h"
#include "Render/RenderGraph/RenderGraphExtension.h"
#include "Render/Runtime/IUploadService.h"
#include "Render/Runtime/ScopedActiveGraphServices.h"

#include "Resources/PixelBuffer.h"

#include <BasicTelemetry/Tracy.h>
#include <cstdlib>
#include <cstring>
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

namespace {
// The texture's last mip, read back on the graphics queue after its handoff left it in shader-resource state (and returned
// there). Empty on failure.
std::vector<uint8_t> ReadMip(rhi::Device device, org::PixelBuffer& texture, uint32_t mip) {
    const auto handle = texture.GetAPIResource().GetHandle();
    rhi::FootprintRangeDesc range{};
    range.texture = handle;
    range.firstMip = mip;
    rhi::CopyableFootprint footprint{};
    const auto info = device.GetCopyableFootprints(range, &footprint, 1);
    rhi::ResourcePtr readback;
    rhi::CommandAllocatorPtr allocator;
    rhi::CommandListPtr list;
    rhi::TimelinePtr done;
    if (rhi::Failed(device.CreateCommittedResource(
            rhi::helpers::ResourceDesc::Buffer(info.totalBytes, rhi::HeapType::Readback, {}, "TextureJobSelfTestReadback"), readback))
        || rhi::Failed(device.CreateCommandAllocator(rhi::QueueKind::Graphics, allocator))
        || rhi::Failed(device.CreateCommandList(rhi::QueueKind::Graphics, allocator.Get(), list))
        || rhi::Failed(device.CreateTimeline(done, 0, "TextureJobSelfTest")))
        return {};
    rhi::TextureBarrier toCopy{};
    toCopy.texture = handle;
    toCopy.range = {mip, 1, 0, 1};
    toCopy.beforeSync = rhi::ResourceSyncState::All;
    toCopy.afterSync = rhi::ResourceSyncState::Copy;
    toCopy.beforeAccess = rhi::ResourceAccessType::ShaderResource;
    toCopy.afterAccess = rhi::ResourceAccessType::CopySource;
    toCopy.beforeLayout = rhi::ResourceLayout::ShaderResource;
    toCopy.afterLayout = rhi::ResourceLayout::CopySource;
    auto back = toCopy;
    std::swap(back.beforeSync, back.afterSync);
    std::swap(back.beforeAccess, back.afterAccess);
    std::swap(back.beforeLayout, back.afterLayout);
    rhi::BarrierBatch barriers{};
    barriers.textures = {&toCopy, 1u};
    list->Barriers(barriers);
    rhi::BufferTextureCopyFootprint copy{};
    copy.texture = handle;
    copy.buffer = readback->GetHandle();
    copy.mip = mip;
    copy.arraySlice = 0;
    copy.footprint = footprint;
    list->CopyTextureToBuffer(copy);
    barriers.textures = {&back, 1u};
    list->Barriers(barriers);
    list->End();
    rhi::CommandList commands = list.Get();
    const rhi::TimelinePoint signal{done->GetHandle(), 1};
    rhi::SubmitDesc submit{};
    submit.signals = {&signal, 1u};
    if (rhi::Failed(device.GetQueue(rhi::QueueKind::Graphics).Submit({&commands, 1u}, submit))
        || done->HostWait(1, 10000) != rhi::Result::Ok)
        return {};
    void* mapped = nullptr;
    readback->Map(&mapped, 0, info.totalBytes);
    if (!mapped) return {};
    std::vector<uint8_t> texel(8);
    std::memcpy(texel.data(), static_cast<const uint8_t*>(mapped) + footprint.offset, texel.size());
    readback->Unmap(0, 0);
    return texel;
}
}

void TextureJobGraph::StartSelfTestIfRequested() {
    const char* requested = std::getenv("BR_TEXTURE_JOB_SELF_TEST");
    if (!requested || std::strcmp(requested, "1") != 0) return;
    // A 64x64 RGBA8 checkerboard of black and white texels: every mip from the first is mid-grey.
    constexpr uint32_t size = 64;
    auto texels = std::make_shared<std::vector<uint8_t>>(size_t{size} * size * 4u);
    for (uint32_t y = 0; y < size; ++y)
        for (uint32_t x = 0; x < size; ++x)
            std::memset(texels->data() + (size_t{y} * size + x) * 4u, ((x + y) & 1u) ? 0xFF : 0x00, 4);
    org::TextureDescription desc{};
    desc.format = rhi::Format::R8G8B8A8_UNorm;
    desc.channels = 4;
    desc.hasSRV = true;
    desc.generateMipMaps = true;
    desc.imageDimensions.push_back({size, size, uint64_t{size} * 4u, uint64_t{size} * size * 4u});
    m_selfTestTexture = m_textures->CreateAlwaysResidentPixelBuffer(desc,
        TextureFactory::TextureInitialData::FromBytes({texels}), "TextureJobSelfTest");
    std::weak_ptr<org::PixelBuffer> weak = m_selfTestTexture;
    const auto device = m_runtime->Device();
    const bool pending = m_textures->WhenMipsReady(*m_selfTestTexture, [weak, device] {
        const auto texture = weak.lock();
        if (!texture) return;
        const auto top = ReadMip(device, *texture, 0), first = ReadMip(device, *texture, 1);
        if (top.size() == 8 && first.size() == 8)
            spdlog::info("TextureJobGraph self-test: mip 0 = {} {} {} {} | {} {} {} {}, mip 1 = {} {} {} {}", top[0], top[1], top[2],
                top[3], top[4], top[5], top[6], top[7], first[0], first[1], first[2], first[3]);
        auto texel = ReadMip(device, *texture, texture->GetMipLevels() - 1u);
        texel.resize((std::min)(texel.size(), size_t{4}));
        const bool grey = texel.size() == 4 && std::all_of(texel.begin(), texel.end(),
            [](uint8_t value) { return value >= 124 && value <= 131; });
        if (grey)
            spdlog::info("TextureJobGraph self-test: passed (mip {} = {} {} {} {})", texture->GetMipLevels() - 1u,
                texel[0], texel[1], texel[2], texel[3]);
        else if (texel.size() == 4)
            spdlog::error("TextureJobGraph self-test: FAILED (mip {} = {} {} {} {}, expected about 128)",
                texture->GetMipLevels() - 1u, texel[0], texel[1], texel[2], texel[3]);
        else
            spdlog::error("TextureJobGraph self-test: FAILED (readback)");
    });
    if (!pending) spdlog::error("TextureJobGraph self-test: FAILED (no GPU mip job was queued)");
    else spdlog::info("TextureJobGraph self-test: queued");
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
