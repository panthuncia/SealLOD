#include "TextureProcessingPasses.h"

#include <algorithm>
#include <stdexcept>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

#include <OpenRenderGraph/OpenRenderGraph.h>

#include "Assets/Textures/Processing/TextureProcessingManager.h"
#include "Resources/PixelBuffer.h"
#include "Resources/Sampler.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/ReadbackRequest.h"
#include "Render/Runtime/IReadbackService.h"
#include "Render/Runtime/IUploadService.h"
#include "ThirdParty/stb/stb_image.h"
#include "rhi_helpers.h"
#include "BasicRenderer/Extensions/Buffers/LazyDynamicStructuredBuffer.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Runtime/Device/DeviceManager.h"
#include "Materials/TextureStreaming/MaterialTextureTransferService.h"
#include "Utilities/Utilities.h"

#define A_CPU
#include "../shaders/FidelityFX/ffx_a.h"
#include "../shaders/FidelityFX/ffx_spd.h"


bool TextureFactory::MipmappingPass::TryGetValueType(const org::PixelBuffer& tex, MipmapValueType& outValueType)
{
    const auto& desc = tex.GetDescription();
    const rhi::Format format = desc.uavFormat != rhi::Format::Unknown
        ? desc.uavFormat
        : rhi::helpers::stripSrgb(desc.format);

    switch (format) {
    case rhi::Format::R8_UNorm:
    case rhi::Format::R8_SNorm:
    case rhi::Format::R16_Float:
    case rhi::Format::R16_UNorm:
    case rhi::Format::R16_SNorm:
    case rhi::Format::R32_Float:
        outValueType = MipmapValueType::Float1;
        return true;

    case rhi::Format::R8G8_UNorm:
    case rhi::Format::R8G8_SNorm:
    case rhi::Format::R16G16_Float:
    case rhi::Format::R16G16_UNorm:
    case rhi::Format::R16G16_SNorm:
    case rhi::Format::R32G32_Float:
        outValueType = MipmapValueType::Float2;
        return true;

    case rhi::Format::R8G8B8A8_UNorm:
    case rhi::Format::R8G8B8A8_SNorm:
    case rhi::Format::R16G16B16A16_Float:
    case rhi::Format::R16G16B16A16_UNorm:
    case rhi::Format::R16G16B16A16_SNorm:
    case rhi::Format::R32G32B32A32_Float:
        outValueType = MipmapValueType::Float4;
        return true;

    default:
        return false;
    }
}

org::PipelineState TextureFactory::MipmappingPass::CreatePipeline(MipmapValueType valueType, bool isArray) const
{
    auto& psoManager = PSOManager::GetInstance();
    auto& layout = psoManager.GetComputeRootSignature();

    std::vector<DxcDefine> defines;
    if (valueType == MipmapValueType::Float1) {
        defines.push_back(DxcDefine{ L"MIPMAP_FLOAT1", L"1" });
    }
    else if (valueType == MipmapValueType::Float2) {
        defines.push_back(DxcDefine{ L"MIPMAP_FLOAT2", L"1" });
    }

    if (isArray) {
        defines.push_back(DxcDefine{ L"MIPMAP_ARRAY", L"1" });
    }

    const char* debugName = nullptr;
    switch (valueType) {
    case MipmapValueType::Float1:
        debugName = isArray ? "MipmapSPD[Float1Array]" : "MipmapSPD[Float12D]";
        break;
    case MipmapValueType::Float2:
        debugName = isArray ? "MipmapSPD[Float2Array]" : "MipmapSPD[Float22D]";
        break;
    case MipmapValueType::Float4:
        debugName = isArray ? "MipmapSPD[Float4Array]" : "MipmapSPD[Float42D]";
        break;
    }

    return psoManager.MakeComputePipeline(
        layout.GetHandle(),
        L"shaders/Utilities/mipmapping.hlsl",
        L"MipmapCSMain",
        std::move(defines),
        debugName);
}

org::PipelineState& TextureFactory::MipmappingPass::GetOrCreateAlphaPipeline(
    const wchar_t* entryPoint,
    org::PipelineState& pso,
    bool& hasPso,
    const char* debugName)
{
    if (!hasPso) {
        auto& psoManager = PSOManager::GetInstance();
        pso = psoManager.MakeComputePipeline(
            psoManager.GetComputeRootSignature().GetHandle(),
            L"shaders/Utilities/alphaCoverageMipmapping.hlsl",
            entryPoint,
            {},
            debugName);
        hasPso = true;
    }
    return pso;
}

org::PipelineState& TextureFactory::MipmappingPass::GetOrCreatePipeline(MipmapValueType valueType, bool isArray)
{
    switch (valueType) {
    case MipmapValueType::Float1:
        if (isArray) {
            if (!m_hasPsoFloat1_Array) {
                m_psoFloat1_Array = CreatePipeline(valueType, true);
                m_hasPsoFloat1_Array = true;
            }
            return m_psoFloat1_Array;
        }
        if (!m_hasPsoFloat1_2D) {
            m_psoFloat1_2D = CreatePipeline(valueType, false);
            m_hasPsoFloat1_2D = true;
        }
        return m_psoFloat1_2D;

    case MipmapValueType::Float2:
        if (isArray) {
            if (!m_hasPsoFloat2_Array) {
                m_psoFloat2_Array = CreatePipeline(valueType, true);
                m_hasPsoFloat2_Array = true;
            }
            return m_psoFloat2_Array;
        }
        if (!m_hasPsoFloat2_2D) {
            m_psoFloat2_2D = CreatePipeline(valueType, false);
            m_hasPsoFloat2_2D = true;
        }
        return m_psoFloat2_2D;

    case MipmapValueType::Float4:
        if (isArray) {
            if (!m_hasPsoFloat4_Array) {
                m_psoFloat4_Array = CreatePipeline(valueType, true);
                m_hasPsoFloat4_Array = true;
            }
            return m_psoFloat4_Array;
        }
        if (!m_hasPsoFloat4_2D) {
            m_psoFloat4_2D = CreatePipeline(valueType, false);
            m_hasPsoFloat4_2D = true;
        }
        return m_psoFloat4_2D;
    }

    throw std::runtime_error("MipmappingPass: unsupported pipeline variant");
}

bool TextureFactory::MipmappingPass::EnqueueJob(const std::shared_ptr<org::PixelBuffer>& tex, bool isSrgb, bool preserveAlphaCoverage) {
    if (!tex) return false;

    if (tex->IsBlockCompressed()) {
        spdlog::warn("MipmappingPass: skipping block compressed texture");
        return false;
    }

    const uint32_t mipLevels = tex->GetMipLevels();
    if (mipLevels <= 1) return false;

    const uint32_t w = tex->GetInternalWidth();
    const uint32_t h = tex->GetInternalHeight();

    // SPD limit. The alpha-coverage path generates mips sequentially and is not bound by SPD's 4K setup.
    if (!preserveAlphaCoverage && (w > 4096 || h > 4096)) {
        spdlog::warn("MipmappingPass: skipping >4K texture ({}x{}) for now", w, h);
        return false;
    }

    Job j{};
    j.texture = tex;
    j.handoff = m_handoff;
    j.isSrgb = isSrgb;
    j.preserveAlphaCoverage = preserveAlphaCoverage;

    const uint32_t faces = tex->IsCubemap() ? 6u : 1u;
    const uint32_t slices = faces * tex->GetArraySize();
    j.sliceCount = (std::max)(1u, slices);
    j.isArray = (j.sliceCount > 1);

    if (!TryGetValueType(*tex, j.valueType)) {
        const auto& desc = tex->GetDescription();
        const rhi::Format format = desc.uavFormat != rhi::Format::Unknown
            ? desc.uavFormat
            : rhi::helpers::stripSrgb(desc.format);
        spdlog::warn("MipmappingPass: unsupported UAV format {} for GPU mip generation", static_cast<uint32_t>(format));
        return false;
    }

    if (j.preserveAlphaCoverage && (j.isArray || j.valueType != MipmapValueType::Float4)) {
        spdlog::warn("MipmappingPass: alpha coverage mip generation is only supported for non-array float4 textures; falling back to SPD");
        j.preserveAlphaCoverage = false;
    }

    // SPD setup
    unsigned int workGroupOffset[2]{};
    unsigned int numWorkGroupsAndMips[2]{};
    unsigned int rectInfo[4]{ 0, 0, w, h };
    unsigned int tg[2]{};

    SpdSetup(tg, workGroupOffset, numWorkGroupsAndMips, rectInfo);

    const uint32_t maxGen = 12u;
    j.mipsToGenerate = j.preserveAlphaCoverage
        ? (mipLevels - 1u)
        : (std::min)(mipLevels - 1u, maxGen);

    // Build constants (store CPU copy; upload happens in Update())
    MipmapSpdConstants c{};
    c.srcSize[0] = w;
    c.srcSize[1] = h;
    c.mips = (std::min)(j.mipsToGenerate, 12u);
    c.numWorkGroups = numWorkGroupsAndMips[0];
    c.workGroupOffset[0] = workGroupOffset[0];
    c.workGroupOffset[1] = workGroupOffset[1];
    c.invInputSize[0] = 1.0f / float((std::max)(1u, w));
    c.invInputSize[1] = 1.0f / float((std::max)(1u, h));
    c.flags = isSrgb ? 1u : 0u;
    c.srcMip = 0;

    for (uint32_t i = 0; i < 12u; ++i) {
        c.mipUavDescriptorIndices[i] = 0;
    }

    j.dispatchThreadGroupCountXY[0] = tg[0];
    j.dispatchThreadGroupCountXY[1] = tg[1];

    // Each publication owns its immutable CPU-written range through retirement.
    j.constantsBuffer = org::LazyDynamicStructuredBuffer<MipmapSpdConstants>::CreateShared(1, "Mipmap job constants");
    if (m_uploads) {
        j.constantsBuffer->SetUploadPolicyTag(org::runtime::UploadPolicyTag::Immediate);
        j.constantsBuffer->PinUploadService(m_uploads);
    }
    j.constantsView = j.constantsBuffer->Add();
    j.constantsIndex = static_cast<uint32_t>(j.constantsView->GetOffset() / sizeof(MipmapSpdConstants));
    j.cpuConstants = c;

    j.constantsBuffer->UpdateView(j.constantsView.get(), &j.cpuConstants);

    // Per-job counter buffer: RWStructuredBuffer<uint> with elementCount = sliceCount
    j.counter = CreateIndexedStructuredBuffer(
        /*numElements=*/ j.sliceCount,
        /*stride=*/ sizeof(uint32_t),
        /*uav=*/ true);

    if (j.preserveAlphaCoverage) {
        constexpr uint32_t kStatsWordsPerMip = 260u;
        j.alphaStats = CreateIndexedStructuredBuffer(
            static_cast<size_t>(mipLevels) * kStatsWordsPerMip,
            sizeof(uint32_t),
            true);
        j.alphaScales = CreateIndexedStructuredBuffer(
            mipLevels,
            sizeof(float),
            true);
    }

    m_declaredResourcesChanged = true;
    m_jobs.Enqueue(std::move(j));
    return true;
}


TextureMipmappingBindings TextureFactory::MipmappingPass::Declare(org::PassBuilder& declaration)
{
    declaration.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    auto* builder = &declaration;
    m_declaredResourcesChanged = false;
    const auto declaredJobs = std::make_shared<const org::runtime::FrameWorkQueue<Job>::Snapshot>(m_jobs.Pending());
    TextureMipmappingBindings bindings;
    bindings.declaredJobs = declaredJobs;
    bindings.jobs.reserve(declaredJobs->size());
    for (const auto& entry : *declaredJobs) {
        const auto& j = entry->work;
        auto& views = bindings.jobs.emplace_back();
        views.constants = builder->ShaderResource(j.constantsBuffer).View();
        auto tex = j.texture;
        if (!tex) continue;
        views.hasTexture = true;
        views.preserveAlphaCoverage = j.preserveAlphaCoverage;
        views.isSrgb = j.isSrgb;
        views.constantsIndex = j.constantsIndex;
        views.dispatchThreadGroupCountXY[0] = j.dispatchThreadGroupCountXY[0];
        views.dispatchThreadGroupCountXY[1] = j.dispatchThreadGroupCountXY[1];
        views.sliceCount = j.sliceCount;
        views.mipsToGenerate = j.mipsToGenerate;
        views.width = tex->GetInternalWidth();
        views.height = tex->GetInternalHeight();

        // SPD reads only mip0. Alpha coverage mode reads each previous mip in sequence.
        if (j.preserveAlphaCoverage) {
            std::vector<org::SrvView> requests;
            requests.reserve(j.mipsToGenerate);
            for (uint32_t mip = 0; mip < j.mipsToGenerate; ++mip) requests.push_back({.mip = mip});
            auto use = builder->ShaderResource(Subresources(tex, org::Mip{ 0, j.mipsToGenerate }),
                std::span<const org::SrvView>(requests));
            views.alphaSources = std::move(use.views);
            bindings.alphaReset = GetOrCreateAlphaPipeline(L"AlphaMipResetStatsCS", m_psoAlphaReset, m_hasPsoAlphaReset, "AlphaMip[ResetStats]");
            bindings.alphaDownsample = GetOrCreateAlphaPipeline(L"AlphaMipDownsampleCS", m_psoAlphaDownsample, m_hasPsoAlphaDownsample, "AlphaMip[Downsample]");
            bindings.alphaResolveScale = GetOrCreateAlphaPipeline(L"AlphaMipResolveScaleCS", m_psoAlphaResolveScale, m_hasPsoAlphaResolveScale, "AlphaMip[ResolveScale]");
            bindings.alphaApplyScale = GetOrCreateAlphaPipeline(L"AlphaMipApplyScaleCS", m_psoAlphaApplyScale, m_hasPsoAlphaApplyScale, "AlphaMip[ApplyScale]");
        }
        else {
            views.source = builder->ShaderResource(Subresources(tex, org::Mip{ 0, 1 }),
                org::SrvView{j.isArray ? static_cast<uint32_t>(org::SRVViewType::Texture2DArray) : UINT32_MAX}).View();
            views.pipeline = GetOrCreatePipeline(j.valueType, j.isArray);
        }
        if (j.mipsToGenerate > 0) {
            std::vector<org::UavView> requests;
            requests.reserve(j.mipsToGenerate);
            for (uint32_t mip = 1; mip <= j.mipsToGenerate; ++mip) requests.push_back({.mip = mip});
            auto use = builder->UnorderedAccess(Subresources(tex, org::FromMip{ 1 }),
                std::span<const org::UavView>(requests));
            views.outputs = std::move(use.views);
        }

        // Counter is UAV for the dispatch
        if (j.preserveAlphaCoverage) {
            views.alphaStats = builder->UnorderedAccess(j.alphaStats).View();
            views.alphaScales = builder->UnorderedAccess(j.alphaScales).View();
        }
        else {
            views.counter = builder->UnorderedAccess(j.counter).View();
        }
    }
    return bindings;
}

TextureMipmappingFrame TextureFactory::MipmappingPass::Prepare(const TextureMipmappingBindings& bindings,
    const org::PassPrepareContext& preparation)
{
    // Publications arriving after declaration belong to a later build.
    const auto declaredJobs = std::static_pointer_cast<const org::runtime::FrameWorkQueue<Job>::Snapshot>(bindings.declaredJobs);
    if (!declaredJobs || declaredJobs->empty()) return {};
    const auto& jobs = *declaredJobs;
    // An earlier execution of this build may have taken some (or all) of the declared jobs already.
    const auto taken = m_jobs.ReserveAvailable(jobs, preparation);
    if (taken.empty()) return {};
    TextureMipmappingFrame frame;
    frame.jobs.reserve(taken.size());
    // Both are in queue order.
    size_t jobIndex = 0;
    for (const auto& entry : taken) {
        while (jobs[jobIndex] != entry) ++jobIndex;
        frame.jobs.push_back(static_cast<uint32_t>(jobIndex));
        const auto& j = entry->work;
        const auto& views = bindings.jobs.at(jobIndex);
        if (!j.texture) continue;
        // Each job's SPD constants carry the bindless indices of its mip UAVs.
        auto constants = j.cpuConstants;
        for (uint32_t mip = 0; mip < constants.mips; ++mip)
            constants.mipUavDescriptorIndices[mip] = preparation.Resolve(views.outputs.at(mip)).index;
        j.constantsBuffer->UpdateView(j.constantsView.get(), &constants);
    }
    m_declaredResourcesChanged = true;
    return frame;
}

void TextureFactory::MipmappingPass::Record(const TextureMipmappingBindings& bindings, const TextureMipmappingFrame& frame,
    org::PassRecordContext& context)
{
    // The jobs this execution took.
    for (const auto jobIndex : frame.jobs) {
        const auto& views = bindings.jobs.at(jobIndex);
        if (!views.hasTexture) continue;

        if (views.preserveAlphaCoverage) {
            constexpr uint32_t kStatsWordsPerMip = 260u;
            const uint32_t statsUav = context.Index(views.alphaStats);
            const uint32_t scalesUav = context.Index(views.alphaScales);
            const uint32_t flags = views.isSrgb ? 1u : 0u;
            const auto dispatch = [&](const org::PipelineState& pipeline, const br::render::MiscConstants& root,
                uint32_t x, uint32_t y) {
                br::render::DispatchCompute(context, pipeline, root, x, y);
                context.UavBarrier();
            };

            for (uint32_t mip = 1; mip <= views.mipsToGenerate; ++mip) {
                const uint32_t srcMip = mip - 1u;
                const uint32_t srcW = (std::max)(1u, views.width >> srcMip);
                const uint32_t srcH = (std::max)(1u, views.height >> srcMip);
                const uint32_t dstW = (std::max)(1u, views.width >> mip);
                const uint32_t dstH = (std::max)(1u, views.height >> mip);
                const uint32_t statsBase = mip * kStatsWordsPerMip;

                br::render::MiscConstants root{};
                root[UintRootConstant2] = statsUav;
                root[UintRootConstant3] = scalesUav;
                root[UintRootConstant4] = srcW;
                root[UintRootConstant5] = srcH;
                root[UintRootConstant6] = dstW;
                root[UintRootConstant7] = dstH;
                root[UintRootConstant8] = statsBase;
                root[UintRootConstant9] = mip;
                root[UintRootConstant10] = flags;

                dispatch(bindings.alphaReset, root, (kStatsWordsPerMip + 255u) / 256u, 1);

                root[UintRootConstant0] = context.Index(views.alphaSources.at(srcMip));
                root[UintRootConstant1] = context.Index(views.outputs.at(srcMip));
                dispatch(bindings.alphaDownsample, root, (dstW + 7u) / 8u, (dstH + 7u) / 8u);

                dispatch(bindings.alphaResolveScale, root, 1, 1);

                dispatch(bindings.alphaApplyScale, root, (dstW + 7u) / 8u, (dstH + 7u) / 8u);
            }
        }
        else {
            br::render::MiscConstants root{};
            root[UintRootConstant0] = context.Index(views.counter);
            root[UintRootConstant1] = context.Index(views.source);
            root[UintRootConstant2] = context.Index(views.constants);
            root[UintRootConstant3] = views.constantsIndex;

            br::render::DispatchCompute(context, views.pipeline, root, views.dispatchThreadGroupCountXY[0],
                views.dispatchThreadGroupCountXY[1], views.sliceCount);
        }
    }
}

void TextureFactory::MipHandoffPass::EnqueueJob(std::shared_ptr<org::PixelBuffer> texture) {
    if (!texture) return;
    if (!m_completedShared && m_completed)
        m_completedShared = std::make_shared<const std::function<void(const org::PixelBuffer&)>>(m_completed);
    m_jobs.Enqueue({std::move(texture), m_completedShared});
}

TextureMipHandoffBindings TextureFactory::MipHandoffPass::Declare(org::PassBuilder& declaration) {
    declaration.PreferQueue(org::QueueKind::Graphics);
    m_declaredResourcesChanged = false;
    const auto declaredJobs = std::make_shared<const org::runtime::FrameWorkQueue<Job>::Snapshot>(m_jobs.Pending());
    TextureMipHandoffBindings bindings;
    bindings.declaredJobs = declaredJobs;
    for (const auto& entry : *declaredJobs) {
        if (!entry->work.texture) continue;
        (void)declaration.ShaderResource(entry->work.texture);
        ++bindings.textures;
    }
    return bindings;
}

org::EmptyPassFrameData TextureFactory::MipHandoffPass::Prepare(const TextureMipHandoffBindings& bindings,
    const org::PassPrepareContext& preparation) {
        const auto declaredJobs = std::static_pointer_cast<const org::runtime::FrameWorkQueue<Job>::Snapshot>(bindings.declaredJobs);
    if (!declaredJobs || declaredJobs->empty()) return {};
    // An earlier execution of this build may have taken some (or all) of the declared jobs already.
    if (!m_jobs.ReserveAvailable(*declaredJobs, preparation).empty()) m_declaredResourcesChanged = true;
    return {};
}
