#pragma once

#include <array>
#include <memory>

#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"

#include "RenderPasses/Base/TypedRenderGraphPass.h"

namespace org { class Buffer; }

class ReyesTessellationTableUploadPass final
    : public org::TypedRenderGraphPass<ReyesTessellationTableUploadPass> {
public:
    ReyesTessellationTableUploadPass(
        std::shared_ptr<org::Buffer> tessTableConfigsBuffer,
        std::shared_ptr<org::Buffer> tessTableVerticesBuffer,
        std::shared_ptr<org::Buffer> tessTableTrianglesBuffer);

    void Declare(org::PassBuilder& builder);
    void Update(const org::UpdateExecutionContext& executionContext) override;
    static void Record(org::PassRecordContext&) {}
    // Kept recordings (M6 step 8): it records nothing; Update uploads the tables once per backing.
    void Revision(std::vector<uint64_t>&) const {}

private:
    std::shared_ptr<org::Buffer> m_tessTableConfigsBuffer;
    std::shared_ptr<org::Buffer> m_tessTableVerticesBuffer;
    std::shared_ptr<org::Buffer> m_tessTableTrianglesBuffer;
    // Upload each concrete backing once. Resource wrappers can survive a
    // resize/rebuild while their backing allocation changes.
    std::array<uint64_t, 3> m_uploadedGenerations{
        UINT64_MAX, UINT64_MAX, UINT64_MAX };
};
