#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Streaming/ViewStateArtifacts.h"

#include <memory>
#include <vector>

namespace org { class Buffer; class Resource; }

namespace br::render {

struct PrimaryCameraUploadBindings {
    org::ResourceBindingToken cameraDestination, cullingDestination;
    // One staging buffer per frame slot.
    std::vector<org::ResourceBindingToken> staging;
};

// This run's upload: Update writes the frame slot's staging buffer and these values, and Record reads them when the pass is
// recorded at execution (after Update in the same run). A kept recording of Visibility records this pass again at each
// submission with the frame data it was prepared with, so the slot cannot be frame data (M6 step 8).
struct PrimaryCameraUploadRun {
    std::uint32_t stagingSlot = 0;
    std::uint64_t revision = 0;
    std::uint64_t frameNumber = 0;
};

struct PreparedPrimaryCameraUpload {
    std::shared_ptr<const PrimaryCameraUploadRun> run;
    bool operator==(const PreparedPrimaryCameraUpload&) const = default;
};

class PrimaryCameraUploadPass final
    : public org::TypedRenderGraphPass<PrimaryCameraUploadPass, PreparedPrimaryCameraUpload, PrimaryCameraUploadBindings> {
public:
    PrimaryCameraUploadPass(std::shared_ptr<org::Resource> cameraDestination,
        std::shared_ptr<org::Resource> cullingDestination, std::uint32_t frameSlotCount);

    PrimaryCameraUploadBindings Declare(org::PassBuilder& builder);
    void Update(const org::UpdateExecutionContext& execution) override;
    // Recorded at each run (it copies this run's staging slot), so only fixed objects name its recording.
    void Revision(std::vector<std::uint64_t>&) const {}
    PreparedPrimaryCameraUpload Prepare(const PrimaryCameraUploadBindings&, const org::PassPrepareContext& preparation) const;
    static void Record(const PrimaryCameraUploadBindings& bindings, const PreparedPrimaryCameraUpload& data,
        org::PassRecordContext& recording);

private:
    std::shared_ptr<org::Resource> m_cameraDestination;
    std::shared_ptr<org::Resource> m_cullingDestination;
    std::vector<std::shared_ptr<org::Buffer>> m_staging;
    std::shared_ptr<PrimaryCameraUploadRun> m_run = std::make_shared<PrimaryCameraUploadRun>();
};

} // namespace br::render
