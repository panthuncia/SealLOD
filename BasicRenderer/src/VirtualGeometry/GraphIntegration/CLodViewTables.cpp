#include "VirtualGeometry/GraphIntegration/CLodViewTables.h"

#include <algorithm>
#include "Render/PassBuilders.h"
#include "BasicRenderer/Streaming/ViewStateArtifacts.h"
#include "Resources/PixelBuffer.h"

CLodDeclaredViewDepthTable::CLodDeclaredViewDepthTable() : m_rows(CLodMaxViewDepthIndices) {
    for (uint32_t i = 0; i < CLodMaxViewDepthIndices; ++i) m_rows[i].cameraBufferIndex = i;
}

bool CLodDeclaredViewDepthTable::Update(std::span<const br::render::PreparedViewFrameData> views, bool useHistoryDepth) {
    std::vector<Input> inputs;
    std::vector<CLodViewDepthSRVIndex> rows(CLodMaxViewDepthIndices);
    for (uint32_t i = 0; i < CLodMaxViewDepthIndices; ++i) rows[i].cameraBufferIndex = i;
    for (const auto& view : views) {
        const auto camera = view.cameraBufferIndex;
        if (camera >= rows.size()) continue;
        const auto depth = !useHistoryDepth || view.depthHistory ? view.linearDepthMap : nullptr;
        if (!depth) continue;
        const uint32_t slices = depth->GetNumSRVSlices();
        if (!slices) continue;
        const uint32_t requested = view.depthBufferArrayIndex >= 0 ? static_cast<uint32_t>(view.depthBufferArrayIndex) : 0u;
        inputs.push_back({camera, (std::min)(requested, slices - 1), depth});
    }
    const bool changed = inputs != m_inputs;
    m_inputs = std::move(inputs);
    m_rows = std::move(rows);
    return changed;
}

void CLodDeclaredViewDepthTable::Declare(org::PassBuilder& builder) {
    m_layout = org::DeclaredTableLayout<CLodViewDepthSRVIndex>(m_rows.size());
    for (const auto& input : m_inputs)
        builder.ShaderResource(input.depth, org::SrvView{UINT32_MAX, 0, input.slice},
            m_layout.Field(input.camera, &CLodViewDepthSRVIndex::linearDepthSRVIndex));
}

uint32_t CLodDeclaredViewDepthTable::Publish(const org::PassPrepareContext& preparation,
    const org::PreparedTablePublisher& publisher) const {
    return m_layout.Publish(preparation, publisher, std::span<const CLodViewDepthSRVIndex>(m_rows));
}

std::vector<CLodViewRasterInfo> BuildCLodVisibilityViewRasterInfoRows(
    std::span<const br::render::PreparedViewFrameData> views,
    uint32_t viewCount, CLodRasterOutputKind outputKind) {
    std::vector<CLodViewRasterInfo> rows(viewCount);
    const bool virtualShadow = outputKind == CLodRasterOutputKind::VirtualShadow;
    const auto virtualResolution = virtualShadow ? CLodVirtualShadowBuildRuntimeResolutionConfig().virtualResolution : 0u;
    for (const auto& view : views) {
        const auto camera = view.cameraBufferIndex;
        if (camera >= rows.size()) continue;
        auto& info = rows[camera];
        info.scissorMinX = 0;
        info.scissorMinY = 0;
        if (virtualShadow) {
            if (view.shadow && view.lightType == Components::LightType::Directional) {
                info.scissorMaxX = virtualResolution;
                info.scissorMaxY = virtualResolution;
                info.viewportScaleX = 1.0f;
                info.viewportScaleY = 1.0f;
            }
            continue;
        }
        if (!view.visibilityBuffer) continue;
        info.scissorMaxX = view.visibilityBuffer->GetWidth();
        info.scissorMaxY = view.visibilityBuffer->GetHeight();
        info.viewportScaleX = 1.0f;
        info.viewportScaleY = 1.0f;
    }
    return rows;
}

bool CLodDeclaredViewRasterTable::Update(std::span<const br::render::PreparedViewFrameData> views, uint32_t viewCount,
    CLodRasterOutputKind outputKind, bool bindVisibility) {
    auto rows = BuildCLodVisibilityViewRasterInfoRows(views, viewCount, outputKind);
    std::vector<Input> inputs;
    if (bindVisibility && outputKind != CLodRasterOutputKind::VirtualShadow)
        for (const auto& view : views)
            if (view.cameraBufferIndex < rows.size() && view.visibilityBuffer)
                inputs.push_back({view.cameraBufferIndex, view.visibilityBuffer});
    const bool changed = inputs != m_inputs || rows.size() != m_rows.size();
    m_inputs = std::move(inputs);
    m_rows = std::move(rows);
    return changed;
}

void CLodDeclaredViewRasterTable::Declare(org::PassBuilder& builder) {
    m_layout = org::DeclaredTableLayout<CLodViewRasterInfo>(m_rows.size());
    for (const auto& input : m_inputs)
        builder.UnorderedAccess(input.visibility, org::UavView{},
            m_layout.Field(input.camera, &CLodViewRasterInfo::visibilityUAVDescriptorIndex));
}

uint32_t CLodDeclaredViewRasterTable::Publish(const org::PassPrepareContext& preparation,
    const org::PreparedTablePublisher& publisher) const {
    return m_layout.Publish(preparation, publisher, std::span<const CLodViewRasterInfo>(m_rows));
}

void CLodDeclaredViewRasterTable::AppendRowRevision(std::vector<uint64_t>& out) const {
    uint64_t hash = 0xcbf29ce484222325ull ^ m_rows.size();
    const auto* bytes = reinterpret_cast<const unsigned char*>(m_rows.data());
    for (size_t i = 0; i < m_rows.size() * sizeof(CLodViewRasterInfo); ++i) {
        hash ^= bytes[i];
        hash *= 0x100000001b3ull;
    }
    out.push_back(hash);
}
