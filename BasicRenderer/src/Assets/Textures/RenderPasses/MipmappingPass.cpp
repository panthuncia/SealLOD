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

void TextureFactory::MipmappingPass::EnqueueJob(const std::shared_ptr<org::PixelBuffer>& tex, bool isSrgb, bool preserveAlphaCoverage) {
    if (!tex) return;

    if (tex->IsBlockCompressed()) {
        spdlog::warn("MipmappingPass: skipping block compressed texture");
        return;
    }

    const uint32_t mipLevels = tex->GetMipLevels();
    if (mipLevels <= 1) return;

    const uint32_t w = tex->GetInternalWidth();
    const uint32_t h = tex->GetInternalHeight();

    // SPD limit. The alpha-coverage path generates mips sequentially and is not bound by SPD's 4K setup.
    if (!preserveAlphaCoverage && (w > 4096 || h > 4096)) {
        spdlog::warn("MipmappingPass: skipping >4K texture ({}x{}) for now", w, h);
        return;
    }

    Job j{};
    j.texture = tex;
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
        return;
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
}


void TextureFactory::MipmappingPass::Declare(org::PassBuilder& declaration)
{
    declaration.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    auto* builder = &declaration;
    m_declaredResourcesChanged = false;
    m_declaredJobs = m_jobs.Pending();
    m_declaredJobViews.clear();
    m_declaredJobViews.reserve(m_declaredJobs.size());
    for (const auto& entry : m_declaredJobs) {
        const auto& j = entry->work;
        auto& views = m_declaredJobViews.emplace_back();
        views.constants = builder->ShaderResource(j.constantsBuffer).View();
        auto tex = j.texture;
        if (!tex) continue;

        // SPD reads only mip0. Alpha coverage mode reads each previous mip in sequence.
        if (j.preserveAlphaCoverage) {
            std::vector<org::SrvView> requests;
            requests.reserve(j.mipsToGenerate);
            for (uint32_t mip = 0; mip < j.mipsToGenerate; ++mip) requests.push_back({.mip = mip});
            auto use = builder->ShaderResource(Subresources(tex, org::Mip{ 0, j.mipsToGenerate }),
                std::span<const org::SrvView>(requests));
            views.alphaSources = std::move(use.views);
        }
        else {
            views.source = builder->ShaderResource(Subresources(tex, org::Mip{ 0, 1 }),
                org::SrvView{j.isArray ? static_cast<uint32_t>(org::SRVViewType::Texture2DArray) : UINT32_MAX}).View();
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
}

br::render::PreparedComputePipelineSequence TextureFactory::MipmappingPass::Prepare(const org::PassPrepareContext& preparation)
{
    br::render::PreparedComputePipelineSequence data{};
    // Publications arriving after declaration belong to the next frame.
    const auto jobs = m_declaredJobs;
    if (jobs.empty()) return data;
    const auto& context = *preparation.preparationData->Get<UpdateContext>();
    data.resourceHeap = context.textureDescriptorHeap.GetHandle();
    data.samplerHeap = context.samplerDescriptorHeap.GetHandle();
    const auto append = [&](const org::PreparedProgramBinding& binding, const auto& constants,
        uint32_t x, uint32_t y, uint32_t z, bool barrier) {
        br::render::PreparedComputePipelineSequence::Step step{};
        step.program = binding.program;
        step.descriptorIndices = binding.descriptorIndices;
        std::copy(std::begin(constants), std::end(constants), step.constants.begin());
        step.groupsX = x; step.groupsY = y; step.groupsZ = z;
        step.uavBarrierAfter = barrier;
        data.steps.push_back(std::move(step));
    };
    // Process all jobs queued for this frame
    for (size_t jobIndex = 0; jobIndex < jobs.size(); ++jobIndex) {
        const auto& entry = jobs[jobIndex];
        const auto& j = entry->work;
        const auto& views = m_declaredJobViews.at(jobIndex);
        const uint32_t constantsSrvIndex = preparation.Resolve(views.constants).index;
        if (!j.texture) continue;
        auto constants = j.cpuConstants;
        for (uint32_t mip = 0; mip < constants.mips; ++mip)
            constants.mipUavDescriptorIndices[mip] = preparation.Resolve(views.outputs.at(mip)).index;
        j.constantsBuffer->UpdateView(j.constantsView.get(), &constants);

        if (j.preserveAlphaCoverage) {
            org::PipelineState& resetPso = GetOrCreateAlphaPipeline(L"AlphaMipResetStatsCS", m_psoAlphaReset, m_hasPsoAlphaReset, "AlphaMip[ResetStats]");
            org::PipelineState& downsamplePso = GetOrCreateAlphaPipeline(L"AlphaMipDownsampleCS", m_psoAlphaDownsample, m_hasPsoAlphaDownsample, "AlphaMip[Downsample]");
            org::PipelineState& resolvePso = GetOrCreateAlphaPipeline(L"AlphaMipResolveScaleCS", m_psoAlphaResolveScale, m_hasPsoAlphaResolveScale, "AlphaMip[ResolveScale]");
            org::PipelineState& applyPso = GetOrCreateAlphaPipeline(L"AlphaMipApplyScaleCS", m_psoAlphaApplyScale, m_hasPsoAlphaApplyScale, "AlphaMip[ApplyScale]");

            const auto reset = preparation.CaptureProgramBinding(resetPso);
            const auto downsample = preparation.CaptureProgramBinding(downsamplePso);
            const auto resolve = preparation.CaptureProgramBinding(resolvePso);
            const auto apply = preparation.CaptureProgramBinding(applyPso);
            constexpr uint32_t kStatsWordsPerMip = 260u;
            const uint32_t statsUav = preparation.Resolve(views.alphaStats).index;
            const uint32_t scalesUav = preparation.Resolve(views.alphaScales).index;
            const uint32_t flags = j.isSrgb ? 1u : 0u;

            for (uint32_t mip = 1; mip <= j.mipsToGenerate; ++mip) {
                const uint32_t srcMip = mip - 1u;
                const uint32_t srcW = (std::max)(1u, j.texture->GetInternalWidth() >> srcMip);
                const uint32_t srcH = (std::max)(1u, j.texture->GetInternalHeight() >> srcMip);
                const uint32_t dstW = (std::max)(1u, j.texture->GetInternalWidth() >> mip);
                const uint32_t dstH = (std::max)(1u, j.texture->GetInternalHeight() >> mip);
                const uint32_t statsBase = mip * kStatsWordsPerMip;

                unsigned int root[NumMiscUintRootConstants]{};
                root[UintRootConstant2] = statsUav;
                root[UintRootConstant3] = scalesUav;
                root[UintRootConstant4] = srcW;
                root[UintRootConstant5] = srcH;
                root[UintRootConstant6] = dstW;
                root[UintRootConstant7] = dstH;
                root[UintRootConstant8] = statsBase;
                root[UintRootConstant9] = mip;
                root[UintRootConstant10] = flags;

                append(reset, root, (kStatsWordsPerMip + 255u) / 256u, 1, 1, true);

                root[UintRootConstant0] = preparation.Resolve(views.alphaSources.at(srcMip)).index;
                root[UintRootConstant1] = preparation.Resolve(views.outputs.at(srcMip)).index;
                append(downsample, root, (dstW + 7u) / 8u, (dstH + 7u) / 8u, 1, true);

                append(resolve, root, 1, 1, 1, true);

                append(apply, root, (dstW + 7u) / 8u, (dstH + 7u) / 8u, 1, true);
            }
        }
        else {
            // Pick SRV (2D vs array)
            const uint32_t srcSrvIndex = preparation.Resolve(views.source).index;

            org::PipelineState& pso = GetOrCreatePipeline(j.valueType, j.isArray);

            const auto binding = preparation.CaptureProgramBinding(pso);

            unsigned int root[NumMiscUintRootConstants]{};
            root[UintRootConstant0] = preparation.Resolve(views.counter).index;
            root[UintRootConstant1] = srcSrvIndex;
            root[UintRootConstant2] = constantsSrvIndex;
            root[UintRootConstant3] = j.constantsIndex;

            append(binding, root, j.dispatchThreadGroupCountXY[0],
                j.dispatchThreadGroupCountXY[1], j.sliceCount, false);
        }
    }

    m_jobs.Reserve(jobs, preparation);
    m_declaredResourcesChanged = true;
    return data;
}
