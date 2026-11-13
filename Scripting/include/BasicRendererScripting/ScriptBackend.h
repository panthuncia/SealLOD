#pragma once

// ScriptBackend: what the scene half of the ABI (ScriptAbi.h, model_instantiate onward) calls. ScriptHost forwards
// those entries here, so the host core needs no renderer. RendererScriptBackend.h implements it over BasicRenderer;
// a test can implement it with a fake. Every method runs on the thread that drives ScriptHost::Tick.
//
// Errors: return a BR_SCRIPT_E_* status and describe it with SetScriptError; the script sees both.

#include <string>
#include <string_view>

#include "BasicRendererScripting/ScriptAbi.h"

namespace br::scripting {

    // Sets the message BRScriptApiV1::get_last_error returns on this thread. Returns status, for `return SetScriptError(...)`.
    BRScriptStatus SetScriptError(BRScriptStatus status, std::string message);

    class ScriptBackend {
    public:
        virtual ~ScriptBackend() = default;

        // Lifecycle, called by ScriptHost: a scenario starts compiling (clear what the previous one left), a scenario
        // reached a terminal state (give the camera back), the host is shutting down (release everything).
        virtual void OnScenarioStarting() {}
        virtual void OnScenarioFinished() {}
        virtual void Shutdown() {}

        virtual BRScriptStatus InstantiateModel(std::string_view, std::string_view, const BRTransform&, BRScriptHandle&)
        {
            return Unsupported("model_instantiate");
        }
        virtual BRScriptStatus GetEntityState(BRScriptHandle, int32_t&) { return Unsupported("entity_get_state"); }
        virtual BRScriptStatus DestroyEntity(BRScriptHandle) { return Unsupported("entity_destroy"); }
        virtual BRScriptStatus SetTransform(BRScriptHandle, const BRTransform&) { return Unsupported("entity_set_transform"); }
        virtual BRScriptStatus GetTransform(BRScriptHandle, BRTransform&) { return Unsupported("entity_get_transform"); }
        virtual BRScriptStatus SetParent(BRScriptHandle, BRScriptHandle) { return Unsupported("entity_set_parent"); }
        virtual BRScriptStatus GetWorldMatrix(BRScriptHandle, float*) { return Unsupported("entity_get_world_matrix"); }

        virtual BRScriptStatus GetCamera(BRScriptHandle&) { return Unsupported("camera_get"); }
        virtual BRScriptStatus SetCameraProjection(float, float, float) { return Unsupported("camera_set_projection"); }
        virtual BRScriptStatus GetCameraProjection(BRCameraProjection&) { return Unsupported("camera_get_projection"); }
        virtual BRScriptStatus SetCameraInputEnabled(bool) { return Unsupported("camera_set_input_enabled"); }

        virtual BRScriptStatus CreateLight(const BRLightDesc&, const BRTransform&, std::string_view, BRScriptHandle&) { return Unsupported("light_create"); }
        virtual BRScriptStatus SetLight(BRScriptHandle, const BRLightDesc&) { return Unsupported("light_set"); }
        virtual BRScriptStatus GetLight(BRScriptHandle, BRLightDesc&) { return Unsupported("light_get"); }

        virtual BRScriptStatus SetEnvironment(std::string_view) { return Unsupported("scene_set_environment"); }
        virtual BRScriptStatus GetSceneStatus(BRSceneStatus&) { return Unsupported("scene_get_status"); }

        virtual BRScriptStatus GetSetting(std::string_view, int32_t, void*) { return Unsupported("setting_get"); }
        virtual BRScriptStatus SetSetting(std::string_view, int32_t, const void*) { return Unsupported("setting_set"); }

        // A control command's reply document (see BRScriptApiV1::command_invoke).
        virtual std::string InvokeCommand(std::string_view)
        {
            return R"({"ok":false,"error":"this host has no control commands"})";
        }

    protected:
        static BRScriptStatus Unsupported(const char* entry)
        {
            return SetScriptError(BR_SCRIPT_E_UNSUPPORTED, std::string(entry) + " is not available: this host has no scene");
        }
    };

} // namespace br::scripting
