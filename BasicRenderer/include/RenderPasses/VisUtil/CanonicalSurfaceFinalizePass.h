#pragma once

#include "Managers/Singletons/PSOManager.h"
#include "Render/RenderContext.h"
#include "RenderPasses/Base/ComputePass.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"

class CanonicalSurfaceFinalizePass final : public ComputePass {
public:
    CanonicalSurfaceFinalizePass()
    {
        m_pso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"shaders/Surface/canonicalSurfaceFinalize.hlsl",
            L"CanonicalSurfaceFinalizeCS", {}, "CanonicalSurfaceFinalizeCS");
    }

    void DeclareResourceUsages(ComputePassBuilder* builder) override
    {
        builder->WithShaderResource(
            Builtin::GBuffer::Normals,
            Builtin::GBuffer::Albedo,
            Builtin::GBuffer::Coat,
            Builtin::GBuffer::Fuzz,
            Builtin::GBuffer::MetallicRoughness,
            Builtin::GBuffer::Emissive,
            Builtin::GBuffer::MotionVectors,
            Builtin::PrimaryCamera::ProjectedDepthTexture)
            .WithUnorderedAccess(
                Builtin::Surface::BaseColorOpacity,
                Builtin::Surface::NormalRoughness,
                Builtin::Surface::SpecularAo,
                Builtin::Surface::Emissive,
                Builtin::Surface::Motion,
                Builtin::Surface::DeviceDepth,
                Builtin::Surface::Identity,
                Builtin::Surface::Payload0,
                Builtin::Surface::Payload1,
                Builtin::Surface::Records);
    }

    void Setup() override
    {
        const std::string_view inputs[] = {
            Builtin::GBuffer::Normals, Builtin::GBuffer::Albedo, Builtin::GBuffer::Coat,
            Builtin::GBuffer::Fuzz, Builtin::GBuffer::MetallicRoughness, Builtin::GBuffer::Emissive,
            Builtin::GBuffer::MotionVectors, Builtin::PrimaryCamera::ProjectedDepthTexture
        };
        for (size_t i = 0; i < std::size(inputs); ++i)
            m_inputs[i] = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(inputs[i]);

        const std::string_view outputs[] = {
            Builtin::Surface::BaseColorOpacity, Builtin::Surface::NormalRoughness,
            Builtin::Surface::SpecularAo, Builtin::Surface::Emissive, Builtin::Surface::Motion,
            Builtin::Surface::DeviceDepth, Builtin::Surface::Identity, Builtin::Surface::Payload0,
            Builtin::Surface::Payload1, Builtin::Surface::Records
        };
        for (size_t i = 0; i < std::size(outputs); ++i)
            m_outputs[i] = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(outputs[i]);
        const auto* dimensionsSource = m_resourceRegistryView->RequestPtr<PixelBuffer>(Builtin::Surface::BaseColorOpacity);
        m_width = dimensionsSource->GetWidth();
        m_height = dimensionsSource->GetHeight();
    }

    PassReturn Execute(PassExecutionContext& executionContext) override
    {
        auto* renderContext = executionContext.hostData->Get<RenderContext>();
        auto& commandList = executionContext.commandList;
        commandList.SetDescriptorHeaps(
            renderContext->textureDescriptorHeap.GetHandle(), renderContext->samplerDescriptorHeap.GetHandle());
        commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
        commandList.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());

        uint32_t constants[NumMiscUintRootConstants]{};
        for (uint32_t i = 0; i < std::size(m_inputs); ++i)
            constants[i] = m_inputs[i]->GetSRVInfo(0).slot.index;
        for (uint32_t i = 0; i < std::size(m_outputs); ++i)
            constants[8 + i] = m_outputs[i]->GetUAVShaderVisibleInfo(0).slot.index;
        constants[18] = m_width;
        constants[19] = m_height;
        commandList.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex,
            0, NumMiscUintRootConstants, constants);
        commandList.Dispatch((constants[18] + 7u) / 8u, (constants[19] + 7u) / 8u, 1);
        return {};
    }

    void Cleanup() override {}

private:
    PipelineState m_pso;
    GloballyIndexedResource* m_inputs[8]{};
    GloballyIndexedResource* m_outputs[10]{};
    uint32_t m_width = 0;
    uint32_t m_height = 0;
};
