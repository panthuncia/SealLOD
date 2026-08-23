#include "Render/GraphExtensions/ClusterLOD/ReyesTessellationTableUploadPass.h"

#include "Render/GraphExtensions/ClusterLOD/ReyesTessellationTable.h"
#include "Render/Runtime/UploadServiceAccess.h"
#include "Resources/Buffers/Buffer.h"
#include "OpenRenderGraph/OpenRenderGraph.h"

ReyesTessellationTableUploadPass::ReyesTessellationTableUploadPass(
    std::shared_ptr<Buffer> tessTableConfigsBuffer,
    std::shared_ptr<Buffer> tessTableVerticesBuffer,
    std::shared_ptr<Buffer> tessTableTrianglesBuffer)
    : m_tessTableConfigsBuffer(std::move(tessTableConfigsBuffer))
    , m_tessTableVerticesBuffer(std::move(tessTableVerticesBuffer))
    , m_tessTableTrianglesBuffer(std::move(tessTableTrianglesBuffer)) {
}

void ReyesTessellationTableUploadPass::DeclareResourceUsages(ComputePassBuilder* builder)
{
    builder->WithShaderResource(
        m_tessTableConfigsBuffer,
        m_tessTableVerticesBuffer,
        m_tessTableTrianglesBuffer);
}

void ReyesTessellationTableUploadPass::Setup() {}

void ReyesTessellationTableUploadPass::Update(const UpdateExecutionContext& executionContext)
{
    (void)executionContext;

    if (m_uploaded) {
        return;
    }

    const auto& tableData = GetReyesTessellationTableData();
    BUFFER_UPLOAD(
        tableData.configs.data(),
        static_cast<uint32_t>(tableData.configs.size() * sizeof(CLodReyesTessTableConfigEntry)),
        org::runtime::UploadTarget::FromShared(m_tessTableConfigsBuffer),
        0);
    BUFFER_UPLOAD(
        tableData.vertices.data(),
        static_cast<uint32_t>(tableData.vertices.size() * sizeof(uint32_t)),
        org::runtime::UploadTarget::FromShared(m_tessTableVerticesBuffer),
        0);
    BUFFER_UPLOAD(
        tableData.triangles.data(),
        static_cast<uint32_t>(tableData.triangles.size() * sizeof(uint32_t)),
        org::runtime::UploadTarget::FromShared(m_tessTableTrianglesBuffer),
        0);

    m_uploaded = true;
}

PassReturn ReyesTessellationTableUploadPass::Execute(PassExecutionContext& executionContext)
{
    (void)executionContext;
    return {};
}

void ReyesTessellationTableUploadPass::Cleanup() {}