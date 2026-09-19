#pragma once

#include <filesystem>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/PreparedEnvironmentDispatch.h"
#include "Managers/EnvironmentManager.h"
#include "Render/PreparedPass.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/RenderContext.h"
#include "Resources/Texture.h"
#include "Utilities/Utilities.h"
#include "Interfaces/IDynamicDeclaredResources.h"

#include <vector>

struct EnvironmentFilterBindings {
    struct Job {
        org::ResourceBindingToken source, destination;
        uint32_t baseResolution = 0, mipCount = 0;
    };
    std::vector<Job> jobs;
};

class EnvironmentFilterPass : public org::TypedRenderGraphPass<EnvironmentFilterPass,
    br::render::PreparedEnvironmentDispatch, EnvironmentFilterBindings>, public org::IDynamicDeclaredResources {
public:
    EnvironmentFilterPass() {
        CreatePrefilterPSO();
    }

    EnvironmentFilterBindings Declare(org::PassBuilder& builder) {
        EnvironmentFilterBindings bindings;
        for (const auto& j : m_pending) {
            if (!j->work.srcCubemap || !j->work.dstPrefilteredCubemap) continue;
            bindings.jobs.push_back({builder.BindShaderResource(j->work.srcCubemap),
                builder.BindUnorderedAccess(j->work.dstPrefilteredCubemap), j->work.baseResolution,
                j->work.dstPrefilteredCubemap->GetNumUAVMipLevels()});
        }

        m_declaredResourcesChanged = false;
        return bindings;
    }



    void Update(const org::UpdateExecutionContext& context) override {
        const auto* input = context.hostData->Get<UpdateContext>();
        m_work = input->environmentWork.prefilter;
        auto pending = m_work.Pending();
        if (pending != m_pending) { m_pending = std::move(pending); m_declaredResourcesChanged = true; }
    }

    br::render::PreparedEnvironmentDispatch Prepare(const EnvironmentFilterBindings& bindings,
        const org::PassPrepareContext& preparation) const {
        br::render::PreparedEnvironmentDispatch data;
        if (m_pending.empty()) return data;
        const auto* context = preparation.preparationData->Get<UpdateContext>();
        data.resourceHeap = context->textureDescriptorHeap.GetHandle();
        data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
        data.program = preparation.CaptureProgram(m_pso);
        data.constantCount = 5;
        for (const auto& job : bindings.jobs) {
            const auto src = preparation.ResolveView(job.source,
                {org::BindlessViewKind::ShaderResource}).index;
            const auto mipCount = job.mipCount;
            for (uint32_t mip = 0; mip < mipCount; ++mip) {
                const auto size = std::max(1u, job.baseResolution >> mip);
                const auto roughness = as_uint(mipCount > 1 ? float(mip) / float(mipCount - 1) : 0.0f);
                for (uint32_t face = 0; face < 6; ++face)
                    data.faces.push_back({{src, preparation.ResolveView(job.destination,
                        {org::BindlessViewKind::UnorderedAccess, UINT32_MAX, mip, face}).index,
                        face, size, roughness}, (size + 7) / 8});
            }
        }
        m_work.Reserve(m_pending, preparation);
        m_pending.clear(); m_declaredResourcesChanged = true;
        return data;
    }

    static void Record(const EnvironmentFilterBindings&, const br::render::PreparedEnvironmentDispatch& data,
        org::PassRecordContext& recording) {
        br::render::RecordEnvironmentDispatch(data, recording);
    }

    bool DeclaredResourcesChanged() const override {
        return m_declaredResourcesChanged;
    }



private:
    mutable br::render::EnvironmentPrefilterWorkQueue m_work;
    mutable br::render::EnvironmentPrefilterWorkQueue::Snapshot m_pending;
    mutable bool m_declaredResourcesChanged = true;

    org::PipelineState m_pso;

    void CreatePrefilterPSO() {
        auto dev = DeviceManager::GetInstance().GetDevice();

        // Static sampler s0 (linear clamp)
        rhi::StaticSamplerDesc s{};
        s.visibility = rhi::ShaderStage::Compute;
        s.set = 0;  // space0
        s.binding = 0;  // s0
        s.arrayCount = 1;
        s.sampler.minFilter = rhi::Filter::Linear;
        s.sampler.magFilter = rhi::Filter::Linear;
        s.sampler.mipFilter = rhi::MipFilter::Linear;
        s.sampler.addressU = rhi::AddressMode::Clamp;
        s.sampler.addressV = rhi::AddressMode::Clamp;
        s.sampler.addressW = rhi::AddressMode::Clamp;

        // Push constants: 5x uint32 (last is roughness bits)
        rhi::PushConstantRangeDesc pc{};
        pc.visibility = rhi::ShaderStage::Compute;
        pc.num32BitValues = 5;   // SrcSrv, DstUav, Face, Size, RoughnessBits
        pc.set = 0;   // space0
        pc.binding = 0;   // b0

        rhi::PipelineLayoutDesc ld{};
        ld.flags = rhi::PipelineLayoutFlags::PF_None;
        ld.pushConstants = { &pc, 1 };
        ld.staticSamplers = { &s, 1 };
        auto layout = std::make_shared<rhi::PipelineLayoutPtr>();
        auto result = dev.CreatePipelineLayout(ld, *layout);
        if (!*layout || !layout->Get().IsValid()) throw std::runtime_error("EnvFilter: layout failed");
        layout->Get().SetName("EnvFilter.ComputeLayout");

        // Compile compute shader
        ShaderInfoBundle sib;
        sib.computeShader = { L"shaders/blurEnvironment.hlsl", L"CSMain", L"cs_6_6" };
        auto compiled = PSOManager::GetInstance().CompileShaders(sib);

        // Create compute PSO
        rhi::SubobjLayout soLayout{ layout->Get().GetHandle() };
        rhi::SubobjShader soCS{ rhi::ShaderStage::Compute, rhi::DXIL(compiled.computeShader.Get()), "CSMain" };

        const rhi::PipelineStreamItem items[] = {
            rhi::Make(soLayout),
            rhi::Make(soCS),
        };
        rhi::PipelinePtr pipeline;
        result = dev.CreatePipeline(items, (uint32_t)std::size(items), pipeline);
        if (Failed(result)) {
            throw std::runtime_error("EnvFilter: PSO failed");
        }
        pipeline->SetName("EnvFilter.ComputePSO");
        m_pso = org::PipelineState(std::move(pipeline), compiled.resourceIDsHash,
            compiled.resourceDescriptorSlots, layout, soLayout.layout);
    }
};
