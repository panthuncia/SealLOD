#pragma once

// The ScriptBackend over BasicRenderer: scripts' entities are flecs entities in the renderer's current scene. Writes
// are buffered and applied from a Renderer scene mutation hook, the point in Update where the asynchronous scene update
// is not running. Link BasicRenderer::ScriptingRenderer.

#include <memory>

#include "BasicRendererScripting/ScriptBackend.h"

class Renderer;
namespace br::diagnostics { class ControlCommandRegistry; }

namespace br::scripting {

    struct RendererScriptBackendOptions {
        Renderer* renderer = nullptr;
        // What command_invoke (ctx.Command) reaches. Null: scripts have no commands.
        br::diagnostics::ControlCommandRegistry* commands = nullptr;
        // Wait for a still-running asynchronous scene update rather than skip a frame, so the writes of every frame
        // reach that frame's scene update. Off trades that determinism for never stalling a frame.
        bool waitForSceneTask = true;
    };

    // The renderer must outlive the backend (destroy the ScriptHost before Renderer::Cleanup).
    std::unique_ptr<ScriptBackend> CreateRendererScriptBackend(const RendererScriptBackendOptions& options);

} // namespace br::scripting
