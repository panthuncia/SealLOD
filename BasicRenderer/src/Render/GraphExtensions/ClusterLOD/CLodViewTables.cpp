#include "Render/GraphExtensions/ClusterLOD/CLodViewTables.h"

#include <algorithm>
#include <atomic>
#include <stdexcept>

#include <spdlog/spdlog.h>

#include "Render/RenderContext.h"
#include "Render/ViewStateArtifacts.h"
#include "Resources/PixelBuffer.h"

namespace {

uint32_t LiveDescriptor(const org::PixelBuffer& resource, org::BindlessViewRequest view) {
    if (!resource.HasAnyDescriptorSlots()) return 0xFFFFFFFFu;
    return view.kind == org::BindlessViewKind::UnorderedAccess
        ? resource.GetUAVShaderVisibleInfo(view.mip, view.slice).slot.index
        : resource.GetSRVInfo(view.mip, view.slice).slot.index;
}

bool Declares(const org::PassPrepareContext& preparation, const org::PixelBuffer& resource) {
    return preparation.resourceSlots
        && preparation.resourceSlots->Find(resource.GetGlobalResourceID()) != preparation.resourceSlots->end();
}

} // namespace

uint32_t ResolveCLodViewDescriptor(const org::PassPrepareContext& preparation, const org::PixelBuffer& resource,
    org::BindlessViewRequest view) {
    const auto id = resource.GetGlobalResourceID();
    if (Declares(preparation, resource)) return preparation.ResolveView(org::ResourceBindingToken{ id, id }, view).index;
    static std::atomic<uint32_t> reported{ 0 };
    if (reported.fetch_add(1, std::memory_order_relaxed) < 8)
        spdlog::warn("CLod view table embeds '{}' which the preparing pass does not declare; using its current descriptor slot",
            resource.GetName());
    return LiveDescriptor(resource, view);
}

template<class Row>
void CLodPreparedViewTable<Row>::AppendRevision(const org::PassPrepareContext& preparation, std::vector<uint64_t>& out) const {
    // Row bytes with descriptor fields unresolved, then each binding's
    // identity. Declared bindings are tracked by the framework once Publish()
    // resolves them; undeclared ones contribute their current slot here.
    uint64_t hash = 0xcbf29ce484222325ull ^ m_rows.size();
    const auto* bytes = reinterpret_cast<const unsigned char*>(m_rows.data());
    for (size_t i = 0; i < m_rows.size() * sizeof(Row); ++i) {
        hash ^= bytes[i];
        hash *= 0x100000001b3ull;
    }
    out.push_back(hash);
    out.push_back(m_bindings.size());
    for (const auto& binding : m_bindings) {
        out.push_back(binding.resource->GetGlobalResourceID());
        out.push_back((uint64_t{ static_cast<uint32_t>(binding.view.kind) } << 32) | binding.view.slice);
        if (!Declares(preparation, *binding.resource)) out.push_back(LiveDescriptor(*binding.resource, binding.view));
    }
}

template<class Row>
uint32_t CLodPreparedViewTable<Row>::Publish(const org::PassPrepareContext& preparation,
    const org::PreparedTablePublisher& publisher) const {
    if (m_rows.empty()) return 0xFFFFFFFFu;
    auto rows = m_rows;
    for (const auto& binding : m_bindings)
        rows[binding.row].*binding.field = ResolveCLodViewDescriptor(preparation, *binding.resource, binding.view);
    return publisher.Publish(preparation, std::span<const Row>(rows));
}

template class CLodPreparedViewTable<CLodViewRasterInfo>;
template class CLodPreparedViewTable<CLodViewDepthSRVIndex>;

CLodViewRasterInfoTable BuildCLodVisibilityViewRasterInfo(std::span<const br::render::PreparedViewFrameData> views,
    uint32_t viewCount, CLodRasterOutputKind outputKind, bool bindVisibility) {
    CLodViewRasterInfoTable table(viewCount);
    const bool virtualShadow = outputKind == CLodRasterOutputKind::VirtualShadow;
    const auto virtualResolution = virtualShadow ? CLodVirtualShadowBuildRuntimeResolutionConfig().virtualResolution : 0u;
    for (const auto& view : views) {
        const auto camera = view.cameraBufferIndex;
        if (camera >= table.size()) continue;
        auto& info = table[camera];
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
        if (bindVisibility) table.BindView(camera, &CLodViewRasterInfo::visibilityUAVDescriptorIndex, view.visibilityBuffer,
            { org::BindlessViewKind::UnorderedAccess });
    }
    return table;
}

CLodViewDepthTable BuildCLodViewDepthTable(std::span<const br::render::PreparedViewFrameData> views, bool useHistoryDepth) {
    CLodViewDepthTable table(CLodMaxViewDepthIndices);
    for (uint32_t i = 0; i < CLodMaxViewDepthIndices; ++i) table[i].cameraBufferIndex = i;
    for (const auto& view : views) {
        const auto camera = view.cameraBufferIndex;
        if (camera >= CLodMaxViewDepthIndices) continue;
        const auto linearDepthMap = !useHistoryDepth || static_cast<bool>(view.depthHistory) ? view.linearDepthMap : nullptr;
        if (!linearDepthMap) continue;
        const uint32_t slices = linearDepthMap->GetNumSRVSlices();
        if (slices == 0) continue;
        const uint32_t requested = view.depthBufferArrayIndex >= 0 ? static_cast<uint32_t>(view.depthBufferArrayIndex) : 0u;
        table.BindView(camera, &CLodViewDepthSRVIndex::linearDepthSRVIndex, linearDepthMap,
            { org::BindlessViewKind::ShaderResource, UINT32_MAX, 0, (std::min)(requested, slices - 1) });
    }
    return table;
}

const UpdateContext& CLodPreparationSnapshot(const org::PassPrepareContext& preparation) {
    const auto* context = preparation.preparationData ? preparation.preparationData->Get<UpdateContext>() : nullptr;
    if (!context) throw std::logic_error("CLod view tables require the owned render snapshot");
    return *context;
}
