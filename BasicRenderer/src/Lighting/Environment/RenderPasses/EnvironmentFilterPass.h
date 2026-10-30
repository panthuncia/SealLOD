#pragma once

#include <filesystem>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "Runtime/Device/DeviceManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BasicRenderer/Assets/Texture.h"
#include "Utilities/Utilities.h"
#include "Interfaces/IDynamicDeclaredResources.h"

#include <vector>
#include <span>
#include <utility>

struct EnvironmentFilterBindings {
    struct Job {
        org::DeclaredViewToken source;
        std::vector<org::DeclaredViewToken> destinationFaces;
        uint32_t baseResolution = 0, mipCount = 0;
    };
    org::PipelineState pipeline;
    std::vector<Job> jobs;
};
struct EnvironmentFilterFrame {
    bool active = false;
};

class EnvironmentFilterPass : public org::TypedRenderGraphPass<EnvironmentFilterPass,
    EnvironmentFilterFrame, EnvironmentFilterBindings>, public org::IDynamicDeclaredResources {
public:
    EnvironmentFilterPass() {
        CreatePrefilterPSO();
    }

    EnvironmentFilterBindings Declare(org::PassBuilder& builder) {
        EnvironmentFilterBindings bindings;
        bindings.pipeline = m_pso;
        for (const auto& j : m_pending) {
            if (!j->work.srcCubemap || !j->work.dstPrefilteredCubemap) continue;
            EnvironmentFilterBindings::Job job{};
            job.source = builder.ShaderResource(j->work.srcCubemap).View();
            job.baseResolution = j->work.baseResolution;
            job.mipCount = j->work.dstPrefilteredCubemap->GetNumUAVMipLevels();
            std::vector<org::UavView> views;
            views.reserve(job.mipCount * 6);
            for (uint32_t mip = 0; mip < job.mipCount; ++mip)
                for (uint32_t face = 0; face < 6; ++face) views.push_back({UINT32_MAX, mip, face});
            auto destination = builder.UnorderedAccess(j->work.dstPrefilteredCubemap, std::span<const org::UavView>(views));
            for (uint32_t view = 0; view < views.size(); ++view) job.destinationFaces.push_back(destination.View(view));
            bindings.jobs.push_back(std::move(job));
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

    EnvironmentFilterFrame Prepare(const EnvironmentFilterBindings&,
        const org::PassPrepareContext& preparation) {
        if (m_pending.empty()) return {};
        m_work.Reserve(m_pending, preparation);
        m_pending.clear(); m_declaredResourcesChanged = true;
        return {true};
    }

    static void Record(const EnvironmentFilterBindings& bindings, const EnvironmentFilterFrame& frame,
        org::PassRecordContext& context) {
        if (!frame.active) return;
        bool bound = false;
        for (const auto& job : bindings.jobs) {
            const auto src = context.Index(job.source);
            const auto mipCount = job.mipCount;
            for (uint32_t mip = 0; mip < mipCount; ++mip) {
                const auto size = std::max(1u, job.baseResolution >> mip);
                const auto roughness = as_uint(mipCount > 1 ? float(mip) / float(mipCount - 1) : 0.0f);
                for (uint32_t face = 0; face < 6; ++face) {
                    if (!bound) { context.BindComputePipeline(bindings.pipeline); bound = true; }
                    const std::array<uint32_t, 5> constants{src, context.Index(job.destinationFaces[mip * 6 + face]),
                        face, size, roughness};
                    context.PushConstants(0, constants);
                    context.Commands().Dispatch((size + 7) / 8, (size + 7) / 8, 1);
                }
            }
        }
    }

    bool DeclaredResourcesChanged() const override {
        return m_declaredResourcesChanged;
    }



private:
    br::render::EnvironmentPrefilterWorkQueue m_work;
    br::render::EnvironmentPrefilterWorkQueue::Snapshot m_pending;
    bool m_declaredResourcesChanged = true;

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
