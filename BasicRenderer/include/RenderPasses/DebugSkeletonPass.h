#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>

#include <DirectXMath.h>

#include "Animation/Skeleton.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Managers/Singletons/RendererECSManager.h"
#include "Mesh/MeshInstance.h"
#include "Render/RenderContext.h"
#include "RenderPasses/Base/RenderPass.h"
#include "Resources/Buffers/DynamicStructuredBuffer.h"
#include "Scene/Scene.h"
#include "ShaderBuffers.h"

class DebugSkeletonPass final : public RenderPass {
public:
    DebugSkeletonPass()
    {
        m_lineBuffer = DynamicStructuredBuffer<SkeletonDebugLine>::CreateShared(65536, "Debug Skeleton Lines");
        CreatePSO();

        auto& ecsWorld = RendererECSManager::GetInstance().GetWorld();
        m_meshInstancesQuery = ecsWorld.query_builder<Components::Matrix, Components::ObjectDrawInfo, Components::MeshInstances>()
            .cached()
            .cache_kind(flecs::QueryCacheAll)
            .build();
    }

    void DeclareResourceUsages(RenderPassBuilder* builder) override
    {
        builder->WithShaderResource(Builtin::CameraBuffer, m_lineBuffer)
            .WithRenderTarget(Builtin::Backbuffer);
        builder->WithConstantBuffer(Builtin::PerFrameBuffer);
    }

    void Setup() override {}

    PassReturn Execute(PassExecutionContext& executionContext) override
    {
        auto* renderContext = executionContext.hostData->Get<RenderContext>();
        auto& context = *renderContext;
        auto& commandList = executionContext.commandList;

        BuildLines();
        if (m_drawRanges.empty()) {
            return {};
        }

        m_lineBuffer->ReplaceData(std::move(m_lines));
        m_lines.clear();

        commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());

        rhi::PassBeginInfo passInfo{};
        rhi::ColorAttachment colorAttachment{};
        colorAttachment.rtv = { context.rtvHeap.GetHandle(), context.frameIndex };
        colorAttachment.loadOp = rhi::LoadOp::Load;
        colorAttachment.storeOp = rhi::StoreOp::Store;
        passInfo.colors = { &colorAttachment };
        passInfo.width = context.outputResolution.x;
        passInfo.height = context.outputResolution.y;
        passInfo.debugName = "Debug Skeleton Overlay";
        commandList.BeginPass(passInfo);

        commandList.SetPrimitiveTopology(rhi::PrimitiveTopology::LineList);
        commandList.BindLayout(PSOManager::GetInstance().GetRootSignature().GetHandle());
        commandList.BindPipeline(m_pso->GetHandle());
        BindResourceDescriptorIndices(commandList, m_resourceDescriptorBindings);

        uint32_t rootConstants[NumMiscUintRootConstants] = {};
        rootConstants[0] = m_lineBuffer->GetSRVInfo(0).slot.index;
        rootConstants[1] = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::PerFrameBuffer)->GetCBVInfo().slot.index;
        rootConstants[2] = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::CameraBuffer)->GetSRVInfo(0).slot.index;
        commandList.PushConstants(
            rhi::ShaderStage::AllGraphics,
            0,
            MiscUintRootSignatureIndex,
            0,
            3,
            rootConstants);

        constexpr uint32_t kLinesPerMeshShaderGroup = 32u;
        for (const auto& range : m_drawRanges) {
            if (range.lineCount == 0u) {
                continue;
            }

            rootConstants[3] = range.lineOffset;
            rootConstants[4] = range.lineCount;
            commandList.PushConstants(
                rhi::ShaderStage::AllGraphics,
                0,
                MiscUintRootSignatureIndex,
                3,
                2,
                rootConstants + 3);

            const uint32_t groupCount = (range.lineCount + kLinesPerMeshShaderGroup - 1u) / kLinesPerMeshShaderGroup;
            commandList.DispatchMesh(groupCount, 1, 1);
        }

        return {};
    }

    void Cleanup() override {}

private:
    struct SkeletonDebugLine {
        DirectX::XMFLOAT4 startWorld;
        DirectX::XMFLOAT4 endWorld;
        DirectX::XMFLOAT4 color;
    };

    struct DrawRange {
        uint32_t lineOffset = 0;
        uint32_t lineCount = 0;
    };

    static DirectX::XMFLOAT4 TransformJointOrigin(DirectX::FXMMATRIX boneMatrix, DirectX::CXMMATRIX objectMatrix)
    {
        const DirectX::XMMATRIX localToWorld = DirectX::XMMatrixMultiply(boneMatrix, objectMatrix);
        const DirectX::XMVECTOR world = DirectX::XMVector3TransformCoord(DirectX::XMVectorZero(), localToWorld);
        DirectX::XMFLOAT4 out{};
        DirectX::XMStoreFloat4(&out, DirectX::XMVectorSetW(world, 1.0f));
        return out;
    }

    static DirectX::XMFLOAT4 ColorForSkeleton(uint32_t skeletonIndex)
    {
        constexpr DirectX::XMFLOAT4 kColors[] = {
            { 0.10f, 0.95f, 1.00f, 1.00f },
            { 1.00f, 0.86f, 0.10f, 1.00f },
            { 0.60f, 1.00f, 0.25f, 1.00f },
            { 1.00f, 0.35f, 0.75f, 1.00f },
            { 0.55f, 0.70f, 1.00f, 1.00f },
        };
        return kColors[skeletonIndex % std::size(kColors)];
    }

    void BuildLines()
    {
        m_lines.clear();
        m_drawRanges.clear();

        uint32_t skeletonIndex = 0;
        m_meshInstancesQuery.each([&](flecs::entity, Components::Matrix matrix, Components::ObjectDrawInfo, Components::MeshInstances meshInstances) {
            for (const auto& meshInstance : meshInstances.meshInstances) {
                if (!meshInstance || !meshInstance->HasSkin()) {
                    continue;
                }

                const auto skin = meshInstance->GetSkin();
                if (!skin) {
                    continue;
                }

                const auto boneMatrices = skin->GetBoneMatrices();
                const auto parentIndices = skin->GetParentIndices();
                const uint32_t boneCount = (std::min)(
                    static_cast<uint32_t>(boneMatrices.size()),
                    static_cast<uint32_t>(parentIndices.size()));
                if (boneCount == 0u) {
                    continue;
                }

                const uint32_t rangeStart = static_cast<uint32_t>(m_lines.size());
                const DirectX::XMFLOAT4 color = ColorForSkeleton(skeletonIndex++);
                for (uint32_t boneIndex = 0; boneIndex < boneCount; ++boneIndex) {
                    const int32_t parentIndex = parentIndices[boneIndex];
                    if (parentIndex < 0 || static_cast<uint32_t>(parentIndex) >= boneCount) {
                        continue;
                    }

                    SkeletonDebugLine line{};
                    line.startWorld = TransformJointOrigin(boneMatrices[static_cast<uint32_t>(parentIndex)], matrix.matrix);
                    line.endWorld = TransformJointOrigin(boneMatrices[boneIndex], matrix.matrix);
                    line.color = color;
                    m_lines.push_back(line);
                }

                const uint32_t rangeCount = static_cast<uint32_t>(m_lines.size()) - rangeStart;
                if (rangeCount != 0u) {
                    m_drawRanges.push_back(DrawRange{
                        .lineOffset = rangeStart,
                        .lineCount = rangeCount,
                    });
                }
            }
        });
    }

    void CreatePSO()
    {
        auto dev = DeviceManager::GetInstance().GetDevice();

        ShaderInfoBundle sib;
        sib.meshShader = { L"shaders/debugSkeleton.hlsl", L"MSMain", L"ms_6_6" };
        sib.pixelShader = { L"shaders/debugSkeleton.hlsl", L"PSMain", L"ps_6_6" };
        auto compiled = PSOManager::GetInstance().CompileShaders(sib);
        m_resourceDescriptorBindings = compiled.resourceDescriptorSlots;

        auto& layout = PSOManager::GetInstance().GetRootSignature();
        rhi::SubobjLayout soLayout{ layout.GetHandle() };
        rhi::SubobjShader soMS{ rhi::ShaderStage::Mesh, rhi::DXIL(compiled.meshShader.Get()), "MSMain" };
        rhi::SubobjShader soPS{ rhi::ShaderStage::Pixel, rhi::DXIL(compiled.pixelShader.Get()), "PSMain" };

        rhi::RasterState rs{};
        rs.fill = rhi::FillMode::Solid;
        rs.cull = rhi::CullMode::None;
        rs.frontCCW = false;
        rhi::SubobjRaster soRaster{ rs };

        rhi::BlendState bs{};
        bs.alphaToCoverage = false;
        bs.independentBlend = false;
        bs.numAttachments = 1;
        auto& attachment = bs.attachments[0];
        attachment.enable = true;
        attachment.srcColor = rhi::BlendFactor::SrcAlpha;
        attachment.dstColor = rhi::BlendFactor::InvSrcAlpha;
        attachment.colorOp = rhi::BlendOp::Add;
        attachment.srcAlpha = rhi::BlendFactor::One;
        attachment.dstAlpha = rhi::BlendFactor::InvSrcAlpha;
        attachment.alphaOp = rhi::BlendOp::Add;
        attachment.writeMask = rhi::ColorWriteEnable::All;
        rhi::SubobjBlend soBlend{ bs };

        rhi::DepthStencilState ds{};
        ds.depthEnable = false;
        ds.depthWrite = false;
        ds.depthFunc = rhi::CompareOp::Always;
        rhi::SubobjDepth soDepth{ ds };

        rhi::RenderTargets rts{};
        rts.count = 1;
        rts.formats[0] = rhi::Format::R8G8B8A8_UNorm;
        rhi::SubobjRTVs soRTVs{ rts };
        rhi::SubobjSample soSample{ rhi::SampleDesc{ 1, 0 } };
        rhi::SubobjPrimitiveTopology soTopology{ rhi::PrimitiveTopology::LineList };

        const rhi::PipelineStreamItem items[] = {
            rhi::Make(soLayout),
            rhi::Make(soMS),
            rhi::Make(soPS),
            rhi::Make(soRaster),
            rhi::Make(soBlend),
            rhi::Make(soDepth),
            rhi::Make(soRTVs),
            rhi::Make(soSample),
            rhi::Make(soTopology),
        };

        const auto result = dev.CreatePipeline(items, static_cast<uint32_t>(std::size(items)), m_pso);
        if (Failed(result)) {
            throw std::runtime_error("Failed to create DebugSkeleton PSO");
        }
        m_pso->SetName("DebugSkeleton.PSO");
    }

    std::shared_ptr<DynamicStructuredBuffer<SkeletonDebugLine>> m_lineBuffer;
    std::vector<SkeletonDebugLine> m_lines;
    std::vector<DrawRange> m_drawRanges;
    flecs::query<Components::Matrix, Components::ObjectDrawInfo, Components::MeshInstances> m_meshInstancesQuery;
    rhi::PipelinePtr m_pso;
    PipelineResources m_resourceDescriptorBindings;
};
