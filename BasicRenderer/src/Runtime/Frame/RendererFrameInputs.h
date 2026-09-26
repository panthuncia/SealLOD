#pragma once

#include "Render/PassExecutionContext.h"
#include "BasicRenderer/Extensions/RenderContext.h"

#include <memory>
#include <stdexcept>
#include <typeindex>

namespace br::render {

// Owned host publication for one accepted logical frame. The top-level context
// values are immutable and never alias Renderer::m_context. Persistent scene
// data is selected through the manifest lease; the remaining mutable handles
// are narrow, explicitly declared services whose reservations are frame-owned.
// The graph request and every raw IHostExecutionData view retain this owner.
class RendererFrameInputs final : public org::IHostExecutionData {
public:
    RendererFrameInputs(
        std::shared_ptr<const UpdateContext> update,
        std::shared_ptr<const RenderContext> render,
        PrimaryCameraFrameUpload primaryCameraUpload)
        : m_update(std::move(update)), m_render(std::move(render)),
          m_primaryCameraUpload(std::move(primaryCameraUpload)) {
        if (!m_update || !m_render)
            throw std::invalid_argument("Accepted renderer frame inputs require both update and render values");
        if (m_update->frameNumber != m_render->frameNumber ||
            m_update->frameSlot != m_render->frameSlot)
            throw std::invalid_argument("Accepted renderer frame inputs contain mismatched frame identities");
    }

    const void* TryGet(std::type_index type) const noexcept override {
        if (type == std::type_index(typeid(UpdateContext))) return m_update.get();
        if (type == std::type_index(typeid(RenderContext))) return m_render.get();
        if (type == std::type_index(typeid(RendererFrameInputs))) return this;
        return nullptr;
    }

    const std::shared_ptr<const UpdateContext>& Update() const noexcept { return m_update; }
    const std::shared_ptr<const RenderContext>& Render() const noexcept { return m_render; }
    uint64_t FrameNumber() const noexcept { return m_update->frameNumber; }
    uint32_t FrameSlot() const noexcept { return m_update->frameSlot; }
    const PrimaryCameraFrameUpload& PrimaryCameraUpload() const noexcept {
        return m_primaryCameraUpload;
    }

private:
    std::shared_ptr<const UpdateContext> m_update;
    std::shared_ptr<const RenderContext> m_render;
    PrimaryCameraFrameUpload m_primaryCameraUpload{};
};

} // namespace br::render
