#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/PreparedEnvironmentDispatch.h"
#include "Managers/EnvironmentManager.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/RenderContext.h"
#include "Resources/Texture.h"
#include "Managers/EnvironmentManager.h"
#include "Interfaces/IDynamicDeclaredResources.h"

#include <vector>

struct EnvironmentConversionBindings {
    struct Job { org::ResourceBindingToken source, destination; uint32_t size = 0; };
    std::vector<Job> jobs;
};

class EnvironmentConversionPass : public org::TypedRenderGraphPass<EnvironmentConversionPass,
    br::render::PreparedEnvironmentDispatch, EnvironmentConversionBindings>, public org::IDynamicDeclaredResources {
public:
    EnvironmentConversionPass() {

        CreateEnvironmentConversionPSO();
    }

    EnvironmentConversionBindings Declare(org::PassBuilder& builder) {
        EnvironmentConversionBindings bindings;
        builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        for (const auto& j : m_pending) {
            if (!j->work.srcTexture || !j->work.dstCubemap) continue;
            bindings.jobs.push_back({builder.BindShaderResource(j->work.srcTexture),
                builder.BindUnorderedAccess(j->work.dstCubemap), j->work.dstCubemap->GetWidth()});
        }

        m_declaredResourcesChanged = false;
        return bindings;
    }



    void Update(const org::UpdateExecutionContext& context) override {
        const auto* input = context.hostData->Get<UpdateContext>();
        m_work = input->environmentWork.conversion;
        auto pending = m_work.Pending();
        if (pending != m_pending) { m_pending = std::move(pending); m_declaredResourcesChanged = true; }
    }

    br::render::PreparedEnvironmentDispatch Prepare(const EnvironmentConversionBindings& bindings,
        const org::PassPrepareContext& preparation) const {
        br::render::PreparedEnvironmentDispatch data;
        if (m_pending.empty()) return data;
        const auto* context = preparation.preparationData->Get<UpdateContext>();
        data.resourceHeap = context->textureDescriptorHeap.GetHandle();
        data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
        data.program = preparation.CaptureProgram(m_pso);
        data.constantCount = 4;
        for (const auto& job : bindings.jobs) {
            const auto size = job.size;
            const auto src = preparation.ResolveView(job.source,
                {org::BindlessViewKind::ShaderResource}).index;
            for (uint32_t face = 0; face < 6; ++face)
                data.faces.push_back({{src, preparation.ResolveView(job.destination,
                    {org::BindlessViewKind::UnorderedAccess, UINT32_MAX, 0, face}).index,
                    face, size, 0}, (size + 7) / 8});
        }
        m_work.Reserve(m_pending, preparation);
        m_pending.clear(); m_declaredResourcesChanged = true;
        return data;
    }

    static void Record(const EnvironmentConversionBindings&, const br::render::PreparedEnvironmentDispatch& data,
        org::PassRecordContext& recording) {
        br::render::RecordEnvironmentDispatch(data, recording);
    }

    bool DeclaredResourcesChanged() const override {
        return m_declaredResourcesChanged;
    }



private:
    mutable br::render::EnvironmentConversionWorkQueue m_work;
    mutable br::render::EnvironmentConversionWorkQueue::Snapshot m_pending;
    mutable bool m_declaredResourcesChanged = true;

    org::PipelineState m_pso;

    void CreateEnvironmentConversionPSO() {
        auto dev = DeviceManager::GetInstance().GetDevice();

        rhi::StaticSamplerDesc s{};
        s.visibility = rhi::ShaderStage::Compute;
        s.set = 0;   // space0
        s.binding = 0;   // s0
        s.arrayCount = 1;
        s.sampler.minFilter = rhi::Filter::Linear;
        s.sampler.magFilter = rhi::Filter::Linear;
        s.sampler.mipFilter = rhi::MipFilter::Linear;
        s.sampler.addressU = rhi::AddressMode::Clamp;
        s.sampler.addressV = rhi::AddressMode::Clamp;
        s.sampler.addressW = rhi::AddressMode::Clamp;

        rhi::PushConstantRangeDesc pc{};
        pc.visibility = rhi::ShaderStage::Compute;
        pc.num32BitValues = 4;    // SrcEnvSrvIndex, DstFaceUavIndex, Face, Size
        pc.set = 0;    // space0
        pc.binding = 0;    // b0

        rhi::PipelineLayoutDesc ld{};
        ld.flags = rhi::PipelineLayoutFlags::PF_None;
        ld.pushConstants = { &pc, 1 };
        ld.staticSamplers = { &s, 1 };
        auto layout = std::make_shared<rhi::PipelineLayoutPtr>();
        auto result = dev.CreatePipelineLayout(ld, *layout);
        if (!*layout || !layout->Get().IsValid()) throw std::runtime_error("EnvConvert: layout failed");
        layout->Get().SetName("EnvConvert.ComputeLayout");

        ShaderInfoBundle sib;
        sib.computeShader = { L"shaders/envToCubemap.hlsl", L"CSMain", L"cs_6_6" };
        auto compiled = PSOManager::GetInstance().CompileShaders(sib);

        rhi::SubobjLayout soLayout{ layout->Get().GetHandle() };
        rhi::SubobjShader soCS{ rhi::ShaderStage::Compute, rhi::DXIL(compiled.computeShader.Get()), "CSMain" };

        const rhi::PipelineStreamItem items[] = {
            rhi::Make(soLayout),
            rhi::Make(soCS),
        };
        rhi::PipelinePtr pipeline;
        result = dev.CreatePipeline(items, (uint32_t)std::size(items), pipeline);
        if (Failed(result)) {
            throw std::runtime_error("EnvConvert: PSO failed");
        }
        pipeline->SetName("EnvConvert.ComputePSO");
        m_pso = org::PipelineState(std::move(pipeline), compiled.resourceIDsHash,
            compiled.resourceDescriptorSlots, layout, soLayout.layout);
    }
};
