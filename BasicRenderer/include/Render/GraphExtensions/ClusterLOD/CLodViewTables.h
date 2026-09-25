#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "Render/BindlessResourceViews.h"
#include "Render/PreparedTablePublisher.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "ShaderBuffers.h"

namespace org { class PixelBuffer; }
namespace br::render { struct PreparedViewFrameData; }
struct UpdateContext;

// Per-view GPU tables read by the CLod shaders (CLodViewRasterInfo,
// CLodViewDepthSRVIndex) embed bindless descriptor indices of view resources.
// Those indices belong to the backing a frame binds, and the render graph hands
// out new ones whenever it re-realizes a resource, so the tables are built from
// the view snapshot during preparation and published through
// org::PreparedTablePublisher: descriptor fields are bound to resources and
// resolved against the frame's frozen bindings at publish time. Passes put
// AppendRevision() into their RecipeRevision/InvocationRevision; together with
// the framework's tracking of the bindings Publish() resolves, that rebuilds a
// table exactly when a row or one of its descriptors changes.
template<class Row>
class CLodPreparedViewTable {
public:
    explicit CLodPreparedViewTable(size_t rowCount = 0) : m_rows(rowCount) {}

    size_t size() const noexcept { return m_rows.size(); }
    bool empty() const noexcept { return m_rows.empty(); }
    Row& operator[](size_t row) { return m_rows.at(row); }
    const Row& operator[](size_t row) const { return m_rows.at(row); }
    std::span<const Row> Rows() const noexcept { return m_rows; }

    // The field receives the index of `view` of `resource` when published.
    void BindView(size_t row, uint32_t Row::* field, std::shared_ptr<org::PixelBuffer> resource, org::BindlessViewRequest view) {
        if (row >= m_rows.size() || !resource) return;
        m_bindings.push_back({ row, field, std::move(resource), view });
    }

    void AppendRevision(const org::PassPrepareContext& preparation, std::vector<uint64_t>& out) const;
    // SRV index of the published table (0xFFFFFFFF when there are no rows).
    uint32_t Publish(const org::PassPrepareContext& preparation, const org::PreparedTablePublisher& publisher) const;

private:
    struct Binding {
        size_t row = 0;
        uint32_t Row::* field = nullptr;
        std::shared_ptr<org::PixelBuffer> resource;
        org::BindlessViewRequest view;
    };
    std::vector<Row> m_rows;
    std::vector<Binding> m_bindings;
};

using CLodViewRasterInfoTable = CLodPreparedViewTable<CLodViewRasterInfo>;
using CLodViewDepthTable = CLodPreparedViewTable<CLodViewDepthSRVIndex>;

extern template class CLodPreparedViewTable<CLodViewRasterInfo>;
extern template class CLodPreparedViewTable<CLodViewDepthSRVIndex>;

// Descriptor index of `view` of `resource` for the frame being prepared: from
// the frozen bindings when the pass declares the resource, otherwise the
// resource's current slot (reported, since the graph does not synchronize an
// undeclared resource for the pass).
uint32_t ResolveCLodViewDescriptor(const org::PassPrepareContext& preparation, const org::PixelBuffer& resource,
    org::BindlessViewRequest view);

// Rows shared by the passes that raster into per-view visibility buffers (or
// virtual shadow pages): the visibility UAV, scissor = visibility size, unit
// viewport scale. Virtual shadow output uses the virtual resolution for
// directional shadow views and binds no descriptors. Passes whose shaders only
// read the rows' scissors pass bindVisibility = false.
CLodViewRasterInfoTable BuildCLodVisibilityViewRasterInfo(std::span<const br::render::PreparedViewFrameData> views,
    uint32_t viewCount, CLodRasterOutputKind outputKind, bool bindVisibility = true);

// Linear-depth SRV per camera slot for occlusion tests. The history pass only
// uses depth whose history selection is valid.
CLodViewDepthTable BuildCLodViewDepthTable(std::span<const br::render::PreparedViewFrameData> views, bool useHistoryDepth);

// The owned render snapshot CLod preparation reads its views from.
const UpdateContext& CLodPreparationSnapshot(const org::PassPrepareContext& preparation);
