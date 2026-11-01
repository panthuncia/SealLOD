#include "Runtime/GraphIntegration/TextureJobGraph.h"

#include "Assets/Textures/TextureFactory.h"
#include "OpenRenderGraph/GraphDesc.h"
#include "OpenRenderGraph/GraphInstance.h"
#include "OpenRenderGraph/Runtime.h"
#include "Render/RenderGraph/RenderGraph.h"
#include "Render/Runtime/IUploadService.h"

#include "Resources/PixelBuffer.h"

#include <BasicTelemetry/Tracy.h>
#include <cstdlib>
#include <cstring>
#include <spdlog/spdlog.h>

namespace br::render {

TextureJobGraph::TextureJobGraph(std::shared_ptr<org::Runtime> runtime, TextureFactory& textures, uint32_t framesInFlight)
    : m_textures(&textures), m_runtime(std::move(runtime)) {
    org::GraphDesc desc("TextureJobs");
    desc.SetFramesInFlight(framesInFlight);
    // The instance adds its own upload pass ahead of these.
    if (auto pass = textures.GetMipmappingPass())
        desc.Add(org::ExternalPassDesc::Compute("TextureJobs::Mipmapping", pass).At(org::ExternalInsertPoint::Begin(1)));
    if (auto pass = textures.GetMipHandoffPass())
        desc.Add(org::ExternalPassDesc::Render("TextureJobs::MipHandoff", pass).At(org::ExternalInsertPoint::Begin(2)));
    if (auto pass = textures.GetBC7CompressionPass())
        desc.Add(org::ExternalPassDesc::Compute("TextureJobs::BC7Compression", pass).At(org::ExternalInsertPoint::Begin(3)));
    if (auto pass = textures.GetBC7CompressionCopyPass())
        desc.Add(org::ExternalPassDesc::Render("TextureJobs::BC7CompressionCopy", pass).At(org::ExternalInsertPoint::Begin(4)));
    if (auto pass = textures.GetBC7CompressionReadbackPass())
        desc.Add(org::ExternalPassDesc::Copy("TextureJobs::BC7CompressionReadback", pass)
            .At(org::ExternalInsertPoint::Begin(5))
            .PinToQueue(static_cast<org::QueueSlotIndex>(2)));
    m_instance = org::GraphInstance::Create(m_runtime, std::move(desc));
}

TextureJobGraph::~TextureJobGraph() {
    // The instance waits for its executions when it goes.
    m_instance.reset();
    spdlog::info("TextureJobGraph: {} executions recorded work ({} batches)", m_executionsWithWork, m_batches);
}

const std::shared_ptr<org::runtime::IUploadService>& TextureJobGraph::Uploads() const noexcept { return m_instance->Uploads(); }

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

void TextureJobGraph::Execute() {
    BT_ZONE_SCOPE("BR.TextureJobs.Execute");
    if (!m_textures->HasPendingGpuJobs()) {
        m_instance->Retire();
        return;
    }
    m_instance->Prepare().Submit();
    if (const auto batches = m_instance->Graph()->LastPersistentExecuteTimings().batches) {
        ++m_executionsWithWork;
        m_batches += batches;
    }
}

} // namespace br::render
