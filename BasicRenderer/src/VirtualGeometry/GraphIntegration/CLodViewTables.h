#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "Render/DeclaredTableLayout.h"
#include "Render/PreparedTablePublisher.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/ShaderBuffers.h"

namespace org { class PixelBuffer; }
namespace br::render { struct PreparedViewFrameData; }

// Per-camera inputs are copied during Update. Declare binds the table fields,
// and Publish resolves those tokens against the frame's frozen backing.

// Owned depth selection for passes that declare per-camera SRVs. Update copies
// resource identity and slice; Declare binds the table destinations once.
class CLodDeclaredViewDepthTable {
public:
    CLodDeclaredViewDepthTable();
    bool Update(std::span<const br::render::PreparedViewFrameData> views, bool useHistoryDepth);
    void Declare(org::PassBuilder& builder);
    // Publishes the rows through `layout`, the one the pass's bindings carry. Cameras without depth history get index 0.
    uint32_t Publish(const org::PassPrepareContext& preparation, const org::PreparedTablePublisher& publisher,
        const org::DeclaredTableLayout<CLodViewDepthSRVIndex>& layout) const;
    org::DeclaredTableLayout<CLodViewDepthSRVIndex> Layout() const { return m_layout; }
    std::span<const CLodViewDepthSRVIndex> Rows() const { return m_rows; }
private:
    struct Input {
        uint32_t camera = 0, slice = 0;
        std::shared_ptr<org::PixelBuffer> depth;
        bool operator==(const Input&) const = default;
    };
    std::vector<Input> m_inputs;
    // Cameras whose depth history isn't valid yet. Their depth is declared anyway, so the declaration doesn't
    // change when history appears, but they publish index 0 ("no depth").
    std::vector<uint32_t> m_withoutHistory;
    std::vector<CLodViewDepthSRVIndex> m_rows;
    org::DeclaredTableLayout<CLodViewDepthSRVIndex> m_layout;
};

// What one declaration of view raster rows declared: the layout, its row count, and the cameras whose visibility buffer
// it routes. While a newer declaration builds on a worker, frames prepare against an older one, so they publish the live
// rows through this one, and run only the views it can serve.
struct CLodViewRasterDeclaration {
    org::DeclaredTableLayout<CLodViewRasterInfo> layout;
    size_t rows = 0;
    bool routesVisibility = false;
    std::vector<uint32_t> visibilityCameras; // Sorted.
    bool Routes(uint32_t camera) const;
    // Whether a view of this camera can run against the declaration: it has a row, and if it has a visibility buffer
    // that the table routes, the declaration routes it.
    bool Admits(uint32_t camera, bool hasVisibility) const;
    // The live rows cut or padded to the declared count, with an empty row (no scissor, no visibility) for each camera
    // in `liveVisibilityCameras` that the declaration doesn't route. Then published through the layout.
    uint32_t Publish(const org::PassPrepareContext& preparation, const org::PreparedTablePublisher& publisher,
        std::span<const CLodViewRasterInfo> live, std::span<const uint32_t> liveVisibilityCameras) const;
};

class CLodDeclaredViewRasterTable {
public:
    bool Update(std::span<const br::render::PreparedViewFrameData> views, uint32_t viewCount,
        CLodRasterOutputKind outputKind, bool bindVisibility = true);
    void Declare(org::PassBuilder& builder);
    // What the last Declare declared; the pass keeps it in its bindings.
    CLodViewRasterDeclaration Declaration() const { return m_declaration; }
    // This frame's rows, published through `declared` (the bindings' declaration).
    uint32_t Publish(const org::PassPrepareContext& preparation, const org::PreparedTablePublisher& publisher,
        const CLodViewRasterDeclaration& declared) const;
    // Whether a current view can run against `declared`.
    bool Admits(const CLodViewRasterDeclaration& declared, uint32_t camera) const;
private:
    struct Input {
        uint32_t camera = 0;
        std::shared_ptr<org::PixelBuffer> visibility;
        bool operator==(const Input&) const = default;
    };
    std::vector<uint32_t> InputCameras() const;
    std::vector<Input> m_inputs;
    std::vector<CLodViewRasterInfo> m_rows;
    bool m_bindVisibility = true;
    CLodViewRasterDeclaration m_declaration;
};

// Descriptor-free rows also feed the shared culling/page-job buffers.
std::vector<CLodViewRasterInfo> BuildCLodVisibilityViewRasterInfoRows(
    std::span<const br::render::PreparedViewFrameData> views,
    uint32_t viewCount, CLodRasterOutputKind outputKind);
