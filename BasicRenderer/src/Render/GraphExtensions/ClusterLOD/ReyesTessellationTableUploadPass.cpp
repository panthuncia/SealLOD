#include "Render/GraphExtensions/ClusterLOD/ReyesTessellationTableUploadPass.h"

#include "Render/GraphExtensions/ClusterLOD/ReyesTessellationTable.h"
#include "Render/Runtime/UploadTypes.h"
#include "Resources/Buffers/Buffer.h"
#include "OpenRenderGraph/OpenRenderGraph.h"

ReyesTessellationTableUploadPass::ReyesTessellationTableUploadPass(
    std::shared_ptr<org::Buffer> tessTableConfigsBuffer,
    std::shared_ptr<org::Buffer> tessTableVerticesBuffer,
    std::shared_ptr<org::Buffer> tessTableTrianglesBuffer)
    : m_tessTableConfigsBuffer(std::move(tessTableConfigsBuffer))
    , m_tessTableVerticesBuffer(std::move(tessTableVerticesBuffer))
    , m_tessTableTrianglesBuffer(std::move(tessTableTrianglesBuffer)) {
}

void ReyesTessellationTableUploadPass::Declare(org::PassBuilder& builder)
{
    builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    builder.WithShaderResource(
        m_tessTableConfigsBuffer,
        m_tessTableVerticesBuffer,
        m_tessTableTrianglesBuffer);
}

void ReyesTessellationTableUploadPass::Update(const org::UpdateExecutionContext& executionContext)
{
    (void)executionContext;

    const auto& tableData = GetReyesTessellationTableData();
    const auto uploadOnce = [this](size_t index, const auto& bytes,
        const std::shared_ptr<org::Buffer>& target) {
        if (!target) return;
        const auto generation = target->GetBackingGeneration();
        if (m_uploadedGenerations[index] == generation) return;
        UploadBufferData(
            bytes.data(),
            static_cast<uint32_t>(bytes.size() * sizeof(bytes.front())),
            org::runtime::UploadTarget::FromShared(target),
            0);
        m_uploadedGenerations[index] = generation;
    };
    uploadOnce(0, tableData.configs, m_tessTableConfigsBuffer);
    uploadOnce(1, tableData.vertices, m_tessTableVerticesBuffer);
    uploadOnce(2, tableData.triangles, m_tessTableTrianglesBuffer);
}
