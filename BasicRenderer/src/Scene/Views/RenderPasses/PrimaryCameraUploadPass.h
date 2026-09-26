#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Streaming/ViewStateArtifacts.h"

#include <memory>
#include <vector>

namespace org { class Buffer; class Resource; }

namespace br::render {

struct PreparedPrimaryCameraUpload {
    org::PreparedResourceReference staging;
    org::PreparedResourceReference cameraDestination;
    org::PreparedResourceReference cullingDestination;
    std::uint64_t revision = 0;
    std::uint64_t frameNumber = 0;
};

class PrimaryCameraUploadPass final
    : public org::TypedRenderGraphPass<PrimaryCameraUploadPass, PreparedPrimaryCameraUpload> {
public:
    PrimaryCameraUploadPass(std::shared_ptr<org::Resource> cameraDestination,
        std::shared_ptr<org::Resource> cullingDestination, std::uint32_t frameSlotCount);

    void Declare(org::PassBuilder& builder);
    PreparedPrimaryCameraUpload Prepare(const org::PassPrepareContext& preparation);
    static void Record(const PreparedPrimaryCameraUpload& data, org::PassRecordContext& recording);

private:
    std::shared_ptr<org::Resource> m_cameraDestination;
    std::shared_ptr<org::Resource> m_cullingDestination;
    std::vector<std::shared_ptr<org::Buffer>> m_staging;
};

} // namespace br::render
