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

#include <algorithm>
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
    // The work of each job, in the same order. Executions take work only from it (PreparesOnlyDeclaredWork).
    br::render::EnvironmentPrefilterWorkQueue::Snapshot declared;
};
// Indices into EnvironmentFilterBindings::jobs of the jobs this execution took.
struct EnvironmentFilterFrame {
    std::vector<uint32_t> jobs;
};

class EnvironmentFilterPass : public org::TypedRenderGraphPass<EnvironmentFilterPass,
    EnvironmentFilterFrame, EnvironmentFilterBindings>, public org::IDynamicDeclaredResources {
public:
    // Runs in the environment job graph (EnvironmentJobGraph): it takes its work queue directly.
    explicit EnvironmentFilterPass(br::render::EnvironmentPrefilterWorkQueue work) : m_work(std::move(work)) {}

    EnvironmentFilterBindings Declare(org::PassBuilder& builder) {
        EnvironmentFilterBindings bindings;
        bindings.pipeline = builder.Pipeline(PipelineDesc());
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
    EnvironmentFilterFrame Prepare(const EnvironmentFilterBindings& bindings,
        const org::PassPrepareContext& preparation) {
        EnvironmentFilterFrame frame;
        const auto taken = m_work.ReserveAvailable(bindings.declared, preparation);
        for (const auto& entry : taken)
            frame.jobs.push_back(static_cast<uint32_t>(
                std::find(bindings.declared.begin(), bindings.declared.end(), entry) - bindings.declared.begin()));
        return frame;
    }

    static void Record(const EnvironmentFilterBindings& bindings, const EnvironmentFilterFrame& frame,
        org::PassRecordContext& context) {
        bool bound = false;
        for (const auto index : frame.jobs) {
            const auto& job = bindings.jobs[index];
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
    bool PreparesOnlyDeclaredWork() const noexcept override { return true; }



private:
    br::render::EnvironmentPrefilterWorkQueue m_work;
    br::render::EnvironmentPrefilterWorkQueue::Snapshot m_pending;
    bool m_declaredResourcesChanged = true;

    static org::ComputePipelineDesc PipelineDesc() {
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

        org::PipelineLayoutSpec layout;
        layout.flags = rhi::PipelineLayoutFlags::PF_None;
        layout.pushConstants = {pc};
        layout.staticSamplers = {s};
        layout.name = "EnvFilter.ComputeLayout";

        org::ComputePipelineDesc desc;
        desc.shader = {L"shaders/blurEnvironment.hlsl", L"CSMain", L"cs_6_6"};
        desc.name = "EnvFilter.ComputePSO";
        desc.layout = org::PipelineLayoutRef::Custom(std::move(layout));
        return desc;
    }
};
