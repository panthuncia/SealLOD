#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "Runtime/Device/DeviceManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BasicRenderer/Assets/Texture.h"
#include "Interfaces/IDynamicDeclaredResources.h"

#include <algorithm>
#include <vector>
#include <array>
#include <span>
#include <utility>

struct EnvironmentConversionBindings {
    struct Job { org::DeclaredViewToken source; std::array<org::DeclaredViewToken, 6> faces; uint32_t size = 0; };
    org::PipelineState pipeline;
    std::vector<Job> jobs;
    // The work of each job, in the same order. Executions take work only from it (PreparesOnlyDeclaredWork).
    br::render::EnvironmentConversionWorkQueue::Snapshot declared;
};
// Indices into EnvironmentConversionBindings::jobs of the jobs this execution took.
struct EnvironmentConversionFrame {
    std::vector<uint32_t> jobs;
};

class EnvironmentConversionPass : public org::TypedRenderGraphPass<EnvironmentConversionPass,
    EnvironmentConversionFrame, EnvironmentConversionBindings>, public org::IDynamicDeclaredResources {
public:
    // Runs in the environment job graph (EnvironmentJobGraph): it takes its work queue directly.
    explicit EnvironmentConversionPass(br::render::EnvironmentConversionWorkQueue work) : m_work(std::move(work)) {

        CreateEnvironmentConversionPSO();
    }

    EnvironmentConversionBindings Declare(org::PassBuilder& builder) {
        EnvironmentConversionBindings bindings;
        bindings.pipeline = m_pso;
        builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        for (const auto& j : m_pending) {
            if (!j->work.srcTexture || !j->work.dstCubemap) continue;
            std::array<org::UavView, 6> views{};
            for (uint32_t face = 0; face < views.size(); ++face) views[face].slice = face;
            auto destination = builder.UnorderedAccess(j->work.dstCubemap, std::span<const org::UavView>(views));
            EnvironmentConversionBindings::Job job{};
            job.source = builder.ShaderResource(j->work.srcTexture).View();
            for (uint32_t face = 0; face < job.faces.size(); ++face) job.faces[face] = destination.View(face);
            job.size = j->work.dstCubemap->GetWidth();
            bindings.jobs.push_back(std::move(job));
            bindings.declared.push_back(j);
        }

        m_declaredResourcesChanged = false;
        return bindings;
    }



    void Update(const org::UpdateExecutionContext&) override {
        auto pending = m_work.Pending();
        if (pending != m_pending) { m_pending = std::move(pending); m_declaredResourcesChanged = true; }
    }

    // Takes only jobs the bindings declare: while a new declaration builds, executions still get the old one. Taking a
    // job leaves the queue's pending work smaller, so the next Update redeclares without it.
    EnvironmentConversionFrame Prepare(const EnvironmentConversionBindings& bindings,
        const org::PassPrepareContext& preparation) {
        EnvironmentConversionFrame frame;
        const auto taken = m_work.ReserveAvailable(bindings.declared, preparation);
        for (const auto& entry : taken)
            frame.jobs.push_back(static_cast<uint32_t>(
                std::find(bindings.declared.begin(), bindings.declared.end(), entry) - bindings.declared.begin()));
        return frame;
    }

    static void Record(const EnvironmentConversionBindings& bindings, const EnvironmentConversionFrame& frame,
        org::PassRecordContext& context) {
        if (frame.jobs.empty()) return;
        context.BindComputePipeline(bindings.pipeline);
        for (const auto index : frame.jobs) {
            const auto& job = bindings.jobs[index];
            const auto size = job.size;
            const auto src = context.Index(job.source);
            for (uint32_t face = 0; face < 6; ++face) {
                const std::array<uint32_t, 4> constants{src, context.Index(job.faces[face]), face, size};
                context.PushConstants(0, constants);
                context.Commands().Dispatch((size + 7) / 8, (size + 7) / 8, 1);
            }
        }
    }

    bool DeclaredResourcesChanged() const override {
        return m_declaredResourcesChanged;
    }
    bool PreparesOnlyDeclaredWork() const noexcept override { return true; }



private:
    br::render::EnvironmentConversionWorkQueue m_work;
    br::render::EnvironmentConversionWorkQueue::Snapshot m_pending;
    bool m_declaredResourcesChanged = true;

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
