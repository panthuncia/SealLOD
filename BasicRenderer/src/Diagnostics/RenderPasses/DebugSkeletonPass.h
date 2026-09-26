#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <stdexcept>
#include <unordered_set>
#include <vector>

#include <DirectXMath.h>
#include <spdlog/spdlog.h>

#include "BasicRenderer/Scene/Animation/Skeleton.h"
#include "Runtime/Device/DeviceManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "Diagnostics/Scene/DebugSceneSnapshotService.h"
#include "BasicRenderer/Assets/Geometry/MeshInstance.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include <BasicRenderer/Extensions/Resources/DynamicStructuredBuffer.h>
#include "BasicRenderer/Scene/Scene.h"
#include "BasicRenderer/Extensions/ShaderBuffers.h"

struct SkeletonDebugLine {
    DirectX::XMFLOAT4 startWorld;
    DirectX::XMFLOAT4 endWorld;
    DirectX::XMFLOAT4 color;
};

struct DebugSkeletonFrameData {
    struct DrawRange { uint32_t lineOffset = 0, lineCount = 0; };
    rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
    org::PreparedDescriptorReference target{};
    DirectX::XMUINT2 outputResolution{};
    rhi::PipelineLayoutHandle layout{};
    org::PreparedProgramBinding program{};
    org::PreparedResourceReference lineResource{};
    std::array<uint32_t, 3> constants{};
    std::vector<DrawRange> ranges;
};

struct DebugSkeletonBindings {
    org::ResourceBindingToken lines, perFrame, camera, target;
};

class DebugSkeletonPass final
    : public org::TypedRenderGraphPass<DebugSkeletonPass, DebugSkeletonFrameData, DebugSkeletonBindings> {
public:
    explicit DebugSkeletonPass(std::shared_ptr<br::render::DebugSceneSnapshotService> snapshots)
        : m_snapshots(std::move(snapshots)) {
        m_lineBuffer = DynamicStructuredBuffer<SkeletonDebugLine>::CreateShared(65536, "Debug Skeleton Lines");
        CreatePSO();

    }

    DebugSkeletonBindings Declare(org::PassBuilder& declaration)
    {
        auto* builder = &declaration;
        return {builder->BindShaderResource(m_lineBuffer),
            builder->BindConstantBuffer(Builtin::PerFrameBuffer),
            builder->BindShaderResource(Builtin::CameraBuffer),
            builder->BindRenderTarget(org::ResourceIdentifier{Builtin::PresentationColor})};
    }

    void Update(const org::UpdateExecutionContext&) override
    {
        BuildLines();
        if (m_drawRanges.empty()) return;
        m_lineBuffer->ReplaceData(std::move(m_lines));
        m_lines.clear();
    }

    DebugSkeletonFrameData Prepare(const DebugSkeletonBindings& bindings,
        const org::PassPrepareContext& preparation) const
    {
        DebugSkeletonFrameData data{};
        if (m_drawRanges.empty()) return data;
        const auto* context = preparation.preparationData->Get<UpdateContext>();
        data.resourceHeap = context->textureDescriptorHeap.GetHandle();
        data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
        data.target = preparation.CaptureView(bindings.target,
            {org::BindlessViewKind::RenderTarget});
        data.outputResolution = context->outputResolution;
        data.layout = PSOManager::GetInstance().GetRootSignature().GetHandle();
        data.program = preparation.CaptureProgramBinding(m_pso, m_resourceDescriptorBindings);
        data.lineResource = preparation.CaptureResource(bindings.lines);
        data.constants = {
            preparation.ResolveView(bindings.lines, {org::BindlessViewKind::ShaderResource}).index,
            preparation.ResolveView(bindings.perFrame, {org::BindlessViewKind::ConstantBuffer}).index,
            preparation.ResolveView(bindings.camera, {org::BindlessViewKind::ShaderResource}).index };
        data.ranges.reserve(m_drawRanges.size());
        for (const auto& range : m_drawRanges)
            data.ranges.push_back({range.lineOffset, range.lineCount});
        return data;
    }

    static void Record(const DebugSkeletonBindings&, const DebugSkeletonFrameData& data, org::PassRecordContext& recording)
    {
        if (data.ranges.empty()) return;
        auto& commandList = recording.Commands();
        commandList.SetDescriptorHeaps(data.resourceHeap, data.samplerHeap);

        rhi::PassBeginInfo passInfo{};
        rhi::ColorAttachment colorAttachment{};
        colorAttachment.rtv = recording.Resolve(data.target);
        colorAttachment.loadOp = rhi::LoadOp::Load;
        colorAttachment.storeOp = rhi::StoreOp::Store;
        passInfo.colors = { &colorAttachment };
        passInfo.width = data.outputResolution.x;
        passInfo.height = data.outputResolution.y;
        passInfo.debugName = "Debug Skeleton Overlay";
        commandList.BeginPass(passInfo);

        commandList.SetPrimitiveTopology(rhi::PrimitiveTopology::LineList);
        commandList.BindLayout(data.layout);
        commandList.BindPipeline(recording.Resolve(data.program.program));
        if (!data.program.descriptorIndices.empty())
            commandList.PushConstants(rhi::ShaderStage::AllGraphics, 0,
                org::shaderapi::kResourceDescriptorIndicesRootParameter, 0,
                static_cast<uint32_t>(data.program.descriptorIndices.size()),
                data.program.descriptorIndices.data());

        uint32_t rootConstants[NumMiscUintRootConstants] = {};
        std::copy(data.constants.begin(), data.constants.end(), rootConstants);
        commandList.PushConstants(
            rhi::ShaderStage::AllGraphics,
            0,
            MiscUintRootSignatureIndex,
            0,
            3,
            rootConstants);

        constexpr uint32_t kLinesPerMeshShaderGroup = 32u;
        for (const auto& range : data.ranges) {
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
        commandList.EndPass();
    }

private:
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

    static bool MatrixIsNearlyIdentity(DirectX::FXMMATRIX matrix)
    {
        const DirectX::XMMATRIX identity = DirectX::XMMatrixIdentity();
        constexpr float kTolerance = 1.0e-5f;
        for (uint32_t row = 0; row < 4; ++row) {
            const DirectX::XMVECTOR diff = DirectX::XMVectorAbs(DirectX::XMVectorSubtract(matrix.r[row], identity.r[row]));
            if (DirectX::XMVectorGetX(diff) > kTolerance ||
                DirectX::XMVectorGetY(diff) > kTolerance ||
                DirectX::XMVectorGetZ(diff) > kTolerance ||
                DirectX::XMVectorGetW(diff) > kTolerance) {
                return false;
            }
        }
        return true;
    }

    static float Distance(DirectX::XMFLOAT4 a, DirectX::XMFLOAT4 b)
    {
        const float x = a.x - b.x;
        const float y = a.y - b.y;
        const float z = a.z - b.z;
        return std::sqrt(x * x + y * y + z * z);
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

    static std::string_view LeafName(std::string_view name)
    {
        const size_t slash = name.find_last_of('/');
        return slash == std::string_view::npos ? name : name.substr(slash + 1u);
    }

    static std::string_view SkeletonInstancePrefix(std::string_view name)
    {
        const size_t bracket = name.find(']');
        return bracket == std::string_view::npos ? std::string_view{} : name.substr(0u, bracket + 1u);
    }

    enum class LineClass : uint32_t {
        SameInstance,
        CrossInstance,
        LongSameInstance,
        LongCrossInstance,
        LongRootRegion,
        RootParent,
    };

    static DirectX::XMFLOAT4 ColorForLineClass(LineClass lineClass, DirectX::XMFLOAT4 skeletonColor)
    {
        switch (lineClass) {
        case LineClass::RootParent:
            return { 1.00f, 0.05f, 0.10f, 1.00f };
        case LineClass::LongRootRegion:
            return { 1.00f, 0.42f, 0.00f, 1.00f };
        case LineClass::LongCrossInstance:
            return { 1.00f, 0.00f, 0.85f, 1.00f };
        case LineClass::CrossInstance:
            return { 0.10f, 1.00f, 0.25f, 1.00f };
        case LineClass::LongSameInstance:
            return { 1.00f, 0.95f, 0.05f, 1.00f };
        case LineClass::SameInstance:
        default:
            return skeletonColor;
        }
    }

    static const char* LineClassName(LineClass lineClass)
    {
        switch (lineClass) {
        case LineClass::RootParent:
            return "root-parent";
        case LineClass::LongRootRegion:
            return "long-root-region";
        case LineClass::LongCrossInstance:
            return "long-cross-instance";
        case LineClass::CrossInstance:
            return "cross-instance";
        case LineClass::LongSameInstance:
            return "long-same-instance";
        case LineClass::SameInstance:
        default:
            return "same-instance";
        }
    }

    void BuildLines()
    {
        m_lines.clear();
        m_drawRanges.clear();

        struct EmittedLineDiagnostic {
            uint32_t skeletonIndex = 0;
            uint32_t child = 0;
            int32_t parent = -1;
            float length = 0.0f;
            float startDistanceFromRoot = 0.0f;
            bool rootParentLine = false;
            bool crossInstanceLine = false;
            LineClass lineClass = LineClass::SameInstance;
            std::string childName;
            std::string parentName;
        };
        std::vector<EmittedLineDiagnostic> emittedLineDiagnostics;
        uint32_t diagnosticRootParentLineCount = 0;
        uint32_t diagnosticRootRegionLineCount = 0;
        uint32_t diagnosticCrossInstanceLineCount = 0;
        uint32_t diagnosticLongCrossInstanceLineCount = 0;
        uint32_t diagnosticLongSameInstanceLineCount = 0;

        uint32_t skeletonIndex = 0;
        for (const auto& source : m_snapshots->CaptureSkeletons()) {
                const auto& boneMatrices = source.boneMatrices;
                const auto& parentIndices = source.parentIndices;
                const auto& rootParentGlobals = source.rootParentGlobals;
                const uint32_t boneCount = (std::min)(
                    static_cast<uint32_t>(boneMatrices.size()),
                    static_cast<uint32_t>(parentIndices.size()));
                if (boneCount == 0u) {
                    continue;
                }

                const uint32_t rangeStart = static_cast<uint32_t>(m_lines.size());
                const uint32_t currentSkeletonIndex = skeletonIndex;
                const DirectX::XMFLOAT4 color = ColorForSkeleton(skeletonIndex++);
                const auto& boneNames = source.boneNames;
                const DirectX::XMFLOAT4 skeletonRootWorld = TransformJointOrigin(boneMatrices[0], source.objectMatrix);
                for (uint32_t boneIndex = 0; boneIndex < boneCount; ++boneIndex) {
                    const int32_t parentIndex = parentIndices[boneIndex];

                    SkeletonDebugLine line{};
                    bool rootParentLine = false;
                    if (parentIndex < 0) {
                        if (boneIndex >= rootParentGlobals.size() || MatrixIsNearlyIdentity(rootParentGlobals[boneIndex])) {
                            continue;
                        }
                        line.startWorld = TransformJointOrigin(rootParentGlobals[boneIndex], source.objectMatrix);
                        rootParentLine = true;
                    }
                    else if (static_cast<uint32_t>(parentIndex) < boneCount) {
                        line.startWorld = TransformJointOrigin(boneMatrices[static_cast<uint32_t>(parentIndex)], source.objectMatrix);
                    }
                    else {
                        continue;
                    }
                    line.endWorld = TransformJointOrigin(boneMatrices[boneIndex], source.objectMatrix);

                    const float lineLength = Distance(line.startWorld, line.endWorld);
                    const float startDistanceFromRoot = Distance(line.startWorld, skeletonRootWorld);
                    const bool startsNearRoot = startDistanceFromRoot <= 2.0f;
                    const bool longLine = lineLength >= 4.0f;
                    bool crossInstanceLine = false;
                    if (parentIndex >= 0 &&
                        static_cast<uint32_t>(parentIndex) < boneNames.size() &&
                        boneIndex < boneNames.size()) {
                        const std::string_view childPrefix = SkeletonInstancePrefix(boneNames[boneIndex]);
                        const std::string_view parentPrefix = SkeletonInstancePrefix(boneNames[static_cast<uint32_t>(parentIndex)]);
                        crossInstanceLine = !childPrefix.empty() && !parentPrefix.empty() && childPrefix != parentPrefix;
                    }

                    LineClass lineClass = LineClass::SameInstance;
                    if (rootParentLine) {
                        lineClass = LineClass::RootParent;
                    }
                    else if (startsNearRoot && longLine) {
                        lineClass = LineClass::LongRootRegion;
                    }
                    else if (crossInstanceLine && longLine) {
                        lineClass = LineClass::LongCrossInstance;
                    }
                    else if (crossInstanceLine) {
                        lineClass = LineClass::CrossInstance;
                    }
                    else if (longLine) {
                        lineClass = LineClass::LongSameInstance;
                    }

                    line.color = ColorForLineClass(lineClass, color);
                    m_lines.push_back(line);

                    if (!m_loggedLineDiagnostics) {
                        if (rootParentLine) {
                            ++diagnosticRootParentLineCount;
                        }
                        if (lineClass == LineClass::LongRootRegion) {
                            ++diagnosticRootRegionLineCount;
                        }
                        if (crossInstanceLine) {
                            ++diagnosticCrossInstanceLineCount;
                        }
                        if (lineClass == LineClass::LongCrossInstance) {
                            ++diagnosticLongCrossInstanceLineCount;
                        }
                        if (lineClass == LineClass::LongSameInstance) {
                            ++diagnosticLongSameInstanceLineCount;
                        }
                        if (longLine || rootParentLine || startsNearRoot || crossInstanceLine) {
                            std::string childName = boneIndex < boneNames.size()
                                ? boneNames[boneIndex]
                                : ("bone_" + std::to_string(boneIndex));
                            childName = std::string(LeafName(childName));

                            std::string parentName = "<root-parent>";
                            if (parentIndex >= 0 && static_cast<uint32_t>(parentIndex) < boneNames.size()) {
                                parentName = boneNames[static_cast<uint32_t>(parentIndex)];
                                parentName = std::string(LeafName(parentName));
                            }

                            emittedLineDiagnostics.push_back(EmittedLineDiagnostic{
                                .skeletonIndex = currentSkeletonIndex,
                                .child = boneIndex,
                                .parent = parentIndex,
                                .length = lineLength,
                                .startDistanceFromRoot = startDistanceFromRoot,
                                .rootParentLine = rootParentLine,
                                .crossInstanceLine = crossInstanceLine,
                                .lineClass = lineClass,
                                .childName = std::move(childName),
                                .parentName = std::move(parentName),
                            });
                        }
                    }
                }

                const uint32_t rangeCount = static_cast<uint32_t>(m_lines.size()) - rangeStart;
                if (rangeCount != 0u) {
                    m_drawRanges.push_back(DrawRange{
                        .lineOffset = rangeStart,
                        .lineCount = rangeCount,
                    });
                }
        }

        if (!m_loggedLineDiagnostics && !m_lines.empty()) {
            m_loggedLineDiagnostics = true;
            std::sort(emittedLineDiagnostics.begin(), emittedLineDiagnostics.end(), [](const auto& lhs, const auto& rhs) {
                return lhs.length > rhs.length;
            });
            spdlog::info(
                "DebugSkeleton overlay diagnostics: skeletons={} lines={} rootParentLines={} longLinesStartingNearRoot={} crossInstanceLines={} longCrossInstanceLines={} longSameInstanceLines={}",
                skeletonIndex,
                m_lines.size(),
                diagnosticRootParentLineCount,
                diagnosticRootRegionLineCount,
                diagnosticCrossInstanceLineCount,
                diagnosticLongCrossInstanceLineCount,
                diagnosticLongSameInstanceLineCount);

            const size_t detailCount = (std::min<size_t>)(emittedLineDiagnostics.size(), 32u);
            for (size_t i = 0; i < detailCount; ++i) {
                const auto& diagnostic = emittedLineDiagnostics[i];
                spdlog::info(
                    "  DebugSkeleton emitted line: skel={} class={} child={} '{}' parent={} '{}' length={:.4f} startDistRoot={:.4f} rootParent={} crossInstance={}",
                    diagnostic.skeletonIndex,
                    LineClassName(diagnostic.lineClass),
                    diagnostic.child,
                    diagnostic.childName,
                    diagnostic.parent,
                    diagnostic.parentName,
                    diagnostic.length,
                    diagnostic.startDistanceFromRoot,
                    diagnostic.rootParentLine,
                    diagnostic.crossInstanceLine);
            }
        }
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

        m_pso = std::make_shared<rhi::PipelinePtr>();
        const auto result = dev.CreatePipeline(items, static_cast<uint32_t>(std::size(items)), *m_pso);
        if (Failed(result)) {
            throw std::runtime_error("Failed to create DebugSkeleton PSO");
        }
        (*m_pso)->SetName("DebugSkeleton.PSO");
    }

    std::shared_ptr<DynamicStructuredBuffer<SkeletonDebugLine>> m_lineBuffer;
    std::vector<SkeletonDebugLine> m_lines;
    std::vector<DrawRange> m_drawRanges;
    std::shared_ptr<br::render::DebugSceneSnapshotService> m_snapshots;
    std::shared_ptr<rhi::PipelinePtr> m_pso;
    org::PipelineResources m_resourceDescriptorBindings;
    bool m_loggedLineDiagnostics = false;
};
