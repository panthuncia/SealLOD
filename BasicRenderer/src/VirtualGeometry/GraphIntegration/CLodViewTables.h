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
    uint32_t Publish(const org::PassPrepareContext& preparation, const org::PreparedTablePublisher& publisher) const;
    org::DeclaredTableLayout<CLodViewDepthSRVIndex> Layout() const { return m_layout; }
    std::span<const CLodViewDepthSRVIndex> Rows() const { return m_rows; }
private:
    struct Input {
        uint32_t camera = 0, slice = 0;
        std::shared_ptr<org::PixelBuffer> depth;
        bool operator==(const Input&) const = default;
    };
    std::vector<Input> m_inputs;
    std::vector<CLodViewDepthSRVIndex> m_rows;
    org::DeclaredTableLayout<CLodViewDepthSRVIndex> m_layout;
};

class CLodDeclaredViewRasterTable {
public:
    bool Update(std::span<const br::render::PreparedViewFrameData> views, uint32_t viewCount,
        CLodRasterOutputKind outputKind, bool bindVisibility = true);
    void Declare(org::PassBuilder& builder);
    uint32_t Publish(const org::PassPrepareContext& preparation, const org::PreparedTablePublisher& publisher) const;
    void AppendRowRevision(std::vector<uint64_t>& out) const;
    org::DeclaredTableLayout<CLodViewRasterInfo> Layout() const { return m_layout; }
    std::span<const CLodViewRasterInfo> Rows() const { return m_rows; }
private:
    struct Input {
        uint32_t camera = 0;
        std::shared_ptr<org::PixelBuffer> visibility;
        bool operator==(const Input&) const = default;
    };
    std::vector<Input> m_inputs;
    std::vector<CLodViewRasterInfo> m_rows;
    org::DeclaredTableLayout<CLodViewRasterInfo> m_layout;
};

// Descriptor-free rows also feed the shared culling/page-job buffers.
std::vector<CLodViewRasterInfo> BuildCLodVisibilityViewRasterInfoRows(
    std::span<const br::render::PreparedViewFrameData> views,
    uint32_t viewCount, CLodRasterOutputKind outputKind);
