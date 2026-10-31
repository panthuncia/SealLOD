#include "VirtualGeometry/GraphIntegration/CLodViewTables.h"

#include <algorithm>
#include <bit>
#include "Render/PassBuilders.h"
#include "BasicRenderer/Streaming/ViewStateArtifacts.h"
#include "Resources/PixelBuffer.h"

CLodDeclaredViewDepthTable::CLodDeclaredViewDepthTable() : m_rows(CLodMaxViewDepthIndices) {
    for (uint32_t i = 0; i < CLodMaxViewDepthIndices; ++i) m_rows[i].cameraBufferIndex = i;
}

bool CLodDeclaredViewDepthTable::Update(std::span<const br::render::PreparedViewFrameData> views, bool useHistoryDepth) {
    std::vector<Input> inputs;
    m_withoutHistory.clear();
    std::vector<CLodViewDepthSRVIndex> rows(CLodMaxViewDepthIndices);
    for (uint32_t i = 0; i < CLodMaxViewDepthIndices; ++i) rows[i].cameraBufferIndex = i;
    for (const auto& view : views) {
        const auto camera = view.cameraBufferIndex;
        if (camera >= rows.size()) continue;
        // Depth history is this same resource, read before this frame writes it.
        const auto& depth = view.linearDepthMap;
        if (!depth) continue;
        if (useHistoryDepth && !view.depthHistory) m_withoutHistory.push_back(camera);
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
    const org::PreparedTablePublisher& publisher, const org::DeclaredTableLayout<CLodViewDepthSRVIndex>& layout) const {
    if (m_withoutHistory.empty())
        return layout.Publish(preparation, publisher, std::span<const CLodViewDepthSRVIndex>(m_rows));
    auto rows = layout.Resolve(preparation, std::span<const CLodViewDepthSRVIndex>(m_rows));
    for (const auto camera : m_withoutHistory) rows[camera].linearDepthSRVIndex = 0;
    return publisher.Publish(preparation, std::span<const CLodViewDepthSRVIndex>(rows));
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
    // The row count is part of the declaration. It grows in powers of two, so new views (shadow cascades, say)
    // only rarely make the pass declare again.
    rows.resize((std::max<size_t>)(16u, std::bit_ceil(rows.size())));
    std::vector<Input> inputs;
    if (bindVisibility && outputKind != CLodRasterOutputKind::VirtualShadow)
        for (const auto& view : views)
            if (view.cameraBufferIndex < rows.size() && view.visibilityBuffer)
                inputs.push_back({view.cameraBufferIndex, view.visibilityBuffer});
    const bool changed = inputs != m_inputs || rows.size() != m_rows.size();
    m_inputs = std::move(inputs);
    m_rows = std::move(rows);
    m_bindVisibility = bindVisibility && outputKind != CLodRasterOutputKind::VirtualShadow;
    return changed;
}

void CLodDeclaredViewRasterTable::Declare(org::PassBuilder& builder) {
    m_declaration = {};
    m_declaration.layout = org::DeclaredTableLayout<CLodViewRasterInfo>(m_rows.size());
    m_declaration.rows = m_rows.size();
    m_declaration.routesVisibility = m_bindVisibility;
    for (const auto& input : m_inputs)
        builder.UnorderedAccess(input.visibility, org::UavView{},
            m_declaration.layout.Field(input.camera, &CLodViewRasterInfo::visibilityUAVDescriptorIndex));
    m_declaration.visibilityCameras = InputCameras();
}

std::vector<uint32_t> CLodDeclaredViewRasterTable::InputCameras() const {
    std::vector<uint32_t> cameras;
    for (const auto& input : m_inputs) cameras.push_back(input.camera);
    std::sort(cameras.begin(), cameras.end());
    return cameras;
}

uint32_t CLodDeclaredViewRasterTable::Publish(const org::PassPrepareContext& preparation,
    const org::PreparedTablePublisher& publisher, const CLodViewRasterDeclaration& declared) const {
    const auto cameras = InputCameras();
    return declared.Publish(preparation, publisher, m_rows, cameras);
}

bool CLodDeclaredViewRasterTable::Admits(const CLodViewRasterDeclaration& declared, uint32_t camera) const {
    const bool hasVisibility = std::any_of(m_inputs.begin(), m_inputs.end(),
        [&](const Input& input) { return input.camera == camera; });
    return declared.Admits(camera, hasVisibility);
}

bool CLodViewRasterDeclaration::Routes(uint32_t camera) const {
    return std::binary_search(visibilityCameras.begin(), visibilityCameras.end(), camera);
}

bool CLodViewRasterDeclaration::Admits(uint32_t camera, bool hasVisibility) const {
    return camera < rows && (!routesVisibility || !hasVisibility || Routes(camera));
}

uint32_t CLodViewRasterDeclaration::Publish(const org::PassPrepareContext& preparation,
    const org::PreparedTablePublisher& publisher, std::span<const CLodViewRasterInfo> live,
    std::span<const uint32_t> liveVisibilityCameras) const {
    std::vector<CLodViewRasterInfo> rows(live.begin(), live.begin() + (std::min)(live.size(), this->rows));
    rows.resize(this->rows);
    if (routesVisibility)
        for (const auto camera : liveVisibilityCameras)
            if (camera < rows.size() && !Routes(camera)) rows[camera] = {};
    return layout.Publish(preparation, publisher, std::span<const CLodViewRasterInfo>(rows));
}
