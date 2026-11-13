/* The C ABI between the native script host and the managed scripting runtime (BasicRenderer.Scripting.dll).
 *
 * Rules (see docs/scripting.md):
 *   - Pure C. Every type crossing this boundary is blittable and has its layout pinned by the static_asserts at the end
 *     of this header and by the size fields of BRScriptApiV1, which the managed side checks at the handshake.
 *   - Tables are append-only. A new entry goes at the end of its table and bumps BR_SCRIPT_ABI_VERSION; existing
 *     entries never move or change signature. The managed side refuses a table smaller than it expects.
 *   - Strings are UTF-8, NUL-terminated, and owned by the caller for the duration of the call only.
 *   - Every call into the API happens on the thread that drives ScriptHost::Tick (the host's main thread).
 *   - Conventions match the renderer: right-handed, -Z forward, metres, radians.
 */
#ifndef BASICRENDERER_SCRIPTING_SCRIPT_ABI_H
#define BASICRENDERER_SCRIPTING_SCRIPT_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define BR_SCRIPT_CALL __cdecl
#else
#define BR_SCRIPT_CALL
#endif

#define BR_SCRIPT_ABI_VERSION 1u

/* Status codes returned by API entries. Negative is failure; BRScriptApiV1::get_last_error describes it. */
typedef int32_t BRScriptStatus;
#define BR_SCRIPT_OK 0
#define BR_SCRIPT_E_INVALID_ARGUMENT (-1)
#define BR_SCRIPT_E_STALE_HANDLE (-2)
#define BR_SCRIPT_E_NOT_FOUND (-3)
#define BR_SCRIPT_E_TYPE_MISMATCH (-4)
#define BR_SCRIPT_E_UNSUPPORTED (-5)
#define BR_SCRIPT_E_BUSY (-6)
#define BR_SCRIPT_E_INTERNAL (-100)

/* Log levels, matching spdlog's ordering. */
#define BR_SCRIPT_LOG_TRACE 0
#define BR_SCRIPT_LOG_DEBUG 1
#define BR_SCRIPT_LOG_INFO 2
#define BR_SCRIPT_LOG_WARN 3
#define BR_SCRIPT_LOG_ERROR 4

/* Scenario lifecycle, reported through BRScriptApiV1::report_state. Terminal states are >= PASSED. */
#define BR_SCRIPT_STATE_IDLE 0
#define BR_SCRIPT_STATE_COMPILING 1
#define BR_SCRIPT_STATE_RUNNING 2
#define BR_SCRIPT_STATE_PASSED 3
#define BR_SCRIPT_STATE_FAILED 4
#define BR_SCRIPT_STATE_COMPILE_ERROR 5

typedef struct BRVec3 {
    float x, y, z;
} BRVec3;

typedef struct BRQuat {
    float x, y, z, w;
} BRQuat;

typedef struct BRTransform {
    BRVec3 position;
    BRQuat rotation;
    BRVec3 scale;
} BRTransform;

/* Entity lifecycle (entity_get_state). A model is PENDING until its file has loaded and it has been placed. */
#define BR_SCRIPT_ENTITY_PENDING 0
#define BR_SCRIPT_ENTITY_READY 1
#define BR_SCRIPT_ENTITY_FAILED 2

/* Light types: the renderer's Components::LightType values. */
#define BR_SCRIPT_LIGHT_POINT 0
#define BR_SCRIPT_LIGHT_SPOT 1
#define BR_SCRIPT_LIGHT_DIRECTIONAL 2

/* Setting value types (setting_get, setting_set). */
#define BR_SCRIPT_SETTING_BOOL 1  /* int32_t, 0 or 1 */
#define BR_SCRIPT_SETTING_INT32 2
#define BR_SCRIPT_SETTING_UINT32 3
#define BR_SCRIPT_SETTING_FLOAT 4
#define BR_SCRIPT_SETTING_UINT8 5  /* passed as uint32_t */

/* A light's parameters. type and castsShadows are fixed at creation; light_set changes the rest. Position and
 * direction are the light's transform: it shines along its local +Z. */
typedef struct BRLightDesc {
    int32_t type;
    int32_t castsShadows;
    BRVec3 color;             /* linear RGB; normalized by the renderer, so brightness is intensity alone */
    float intensity;
    float range;              /* point and spot: where the falloff reaches zero; 0 picks one from intensity */
    float sourceRadius;
    float innerConeRadians;   /* spot: half-angles */
    float outerConeRadians;
} BRLightDesc;

typedef struct BRCameraProjection {
    float fovYRadians;
    float aspect;
    float zNear;
    float zFar;
} BRCameraProjection;

/* What the renderer still has to do before a frame is representative (scene_get_status). Stable means pendingWork,
 * pendingShaderCompiles and pendingModels are 0 and workloadSignature has not changed for some frames. */
typedef struct BRSceneStatus {
    uint64_t frame;               /* frames the renderer has rendered */
    uint64_t pendingWork;         /* queued streaming, uploads, texture reloads and background tasks */
    uint64_t workloadSignature;   /* changes while textures reach full resolution and geometry becomes resident */
    uint32_t pendingShaderCompiles;
    uint32_t pendingModels;       /* script models still loading */
} BRSceneStatus;

/* An entity reference: index in the low 32 bits, generation in the high 32. 0 is never a valid handle. A handle whose
 * entity was destroyed returns BR_SCRIPT_E_STALE_HANDLE from every entry; it never aliases a newer entity. */
typedef uint64_t BRScriptHandle;

/* Native services, filled by the host and handed to the runtime at initialization. */
typedef struct BRScriptApiV1 {
    uint32_t structSize;    /* sizeof(BRScriptApiV1) as the host compiled it */
    uint32_t version;       /* BR_SCRIPT_ABI_VERSION as the host compiled it */
    uint32_t vec3Size;      /* layout handshake: sizeof(BRVec3), sizeof(BRQuat), sizeof(BRTransform) */
    uint32_t quatSize;
    uint32_t transformSize;
    uint32_t reserved0;
    void* context;          /* opaque; passed back as the first argument of every entry */

    /* --- version 1 --- */
    void(BR_SCRIPT_CALL* log)(void* context, int32_t level, const char* message);
    /* Copies the calling thread's last error (UTF-8, NUL-terminated, truncated to capacity) and returns the length
     * the full message needs, excluding the NUL. */
    int32_t(BR_SCRIPT_CALL* get_last_error)(void* context, char* buffer, int32_t capacity);
    /* The runtime reports every scenario state change. resultJson is the result document for terminal states and
     * NULL otherwise. */
    void(BR_SCRIPT_CALL* report_state)(void* context, int32_t state, const char* resultJson);
    /* A script asks the host application to exit with this code once the current frame ends. */
    void(BR_SCRIPT_CALL* request_exit)(void* context, int32_t exitCode);
    /* Host-application extension tables (for example a game-specific API). NULL when the host has none by that
     * name, or none at least that version. */
    const void*(BR_SCRIPT_CALL* get_extension)(void* context, const char* name, uint32_t minimumVersion);

    /* Scene. Writes are buffered and applied at the renderer's next scene safe point, before that frame's scene
     * update; reads return what the script last wrote (transforms) or what the renderer last rendered (world matrices,
     * the camera before the script first moves it). Every entity a scenario creates is destroyed when the next
     * scenario starts or the host shuts down. */

    /* Places a model file (glTF/GLB, USD, NIF, *.brscene.json) as a movable entity. Returns at once with a handle in
     * BR_SCRIPT_ENTITY_PENDING; the entity appears once the file has loaded. Loads are cached by path. */
    BRScriptStatus(BR_SCRIPT_CALL* model_instantiate)(void* context, const char* path, const char* name, const BRTransform* transform,
        BRScriptHandle* outHandle);
    BRScriptStatus(BR_SCRIPT_CALL* entity_get_state)(void* context, BRScriptHandle handle, int32_t* outState);
    /* Destroys a model or light (and detaches its children). The camera cannot be destroyed. */
    BRScriptStatus(BR_SCRIPT_CALL* entity_destroy)(void* context, BRScriptHandle handle);
    /* The local transform: relative to the parent, or the world without one. The camera ignores scale. */
    BRScriptStatus(BR_SCRIPT_CALL* entity_set_transform)(void* context, BRScriptHandle handle, const BRTransform* transform);
    BRScriptStatus(BR_SCRIPT_CALL* entity_get_transform)(void* context, BRScriptHandle handle, BRTransform* outTransform);
    /* 0 parents to the scene root. The child keeps its local transform, so it moves to the same place under the new parent. */
    BRScriptStatus(BR_SCRIPT_CALL* entity_set_parent)(void* context, BRScriptHandle handle, BRScriptHandle parent);
    /* The world matrix the renderer last propagated (16 floats, row-major, row vectors: translation in 12..14).
     * BR_SCRIPT_E_NOT_FOUND until the entity has been through a frame. */
    BRScriptStatus(BR_SCRIPT_CALL* entity_get_world_matrix)(void* context, BRScriptHandle handle, float* outMatrix16);

    /* The primary camera as an entity: its transform is its pose (it looks along local -Z). */
    BRScriptStatus(BR_SCRIPT_CALL* camera_get)(void* context, BRScriptHandle* outHandle);
    /* A value <= 0 keeps that field; aspect follows the window. */
    BRScriptStatus(BR_SCRIPT_CALL* camera_set_projection)(void* context, float fovYRadians, float zNear, float zFar);
    BRScriptStatus(BR_SCRIPT_CALL* camera_get_projection)(void* context, BRCameraProjection* outProjection);
    /* The fly camera (keyboard and mouse). The host turns it back on when a scenario ends. */
    BRScriptStatus(BR_SCRIPT_CALL* camera_set_input_enabled)(void* context, int32_t enabled);

    BRScriptStatus(BR_SCRIPT_CALL* light_create)(void* context, const BRLightDesc* desc, const BRTransform* transform, const char* name,
        BRScriptHandle* outHandle);
    BRScriptStatus(BR_SCRIPT_CALL* light_set)(void* context, BRScriptHandle handle, const BRLightDesc* desc);
    BRScriptStatus(BR_SCRIPT_CALL* light_get)(void* context, BRScriptHandle handle, BRLightDesc* outDesc);

    BRScriptStatus(BR_SCRIPT_CALL* scene_set_environment)(void* context, const char* name);
    BRScriptStatus(BR_SCRIPT_CALL* scene_get_status)(void* context, BRSceneStatus* outStatus);

    /* Renderer settings by name. type is BR_SCRIPT_SETTING_*; a setting of another type is BR_SCRIPT_E_TYPE_MISMATCH. */
    BRScriptStatus(BR_SCRIPT_CALL* setting_get)(void* context, const char* name, int32_t type, void* outValue);
    BRScriptStatus(BR_SCRIPT_CALL* setting_set)(void* context, const char* name, int32_t type, const void* value);

    /* Runs a host control command (the control pipe's, TESTING.md): requestJson is {"command": ..., fields...}. Returns
     * the reply document ({"ok": bool, "error"?: ...}), valid until the next call; never NULL. */
    const char*(BR_SCRIPT_CALL* command_invoke)(void* context, const char* requestJson);
} BRScriptApiV1;

/* Runtime entry points, filled by the managed runtime at initialization. */
typedef struct BRScriptRuntimeCallbacks {
    uint32_t structSize;    /* set by the host before initialization; the runtime fills no more than this */
    uint32_t version;       /* set by the runtime: the ABI version it was built against */

    /* --- version 1 --- */
    /* Starts compiling and running a scenario. sourcePath is a .cs file or a directory of them; scenarioName may be
     * NULL or empty when the sources define exactly one scenario. optionsJson may be NULL. Compilation is
     * asynchronous; the scenario's first frame is the first tick after it compiles. Returns BR_SCRIPT_E_BUSY while
     * a scenario is compiling or running. */
    BRScriptStatus(BR_SCRIPT_CALL* run_scenario)(const char* sourcePath, const char* scenarioName, const char* optionsJson);
    /* Advances the scenario by one frame. Called once per host frame, before the renderer's update. */
    void(BR_SCRIPT_CALL* tick)(uint64_t hostFrameIndex, double deltaSeconds);
    /* Cancels the running or compiling scenario: it ends FAILED with this reason (NULL: "stopped"). */
    void(BR_SCRIPT_CALL* stop)(const char* reason);
    /* Stops any scenario and releases the runtime's references to the API table. No call follows. */
    void(BR_SCRIPT_CALL* shutdown)(void);
} BRScriptRuntimeCallbacks;

/* The managed entry point: BasicRenderer.Scripting.Runtime.ScriptRuntime.Initialize. Returns BR_SCRIPT_OK, or a
 * failure after logging why through api->log. */
typedef BRScriptStatus(BR_SCRIPT_CALL* BRScriptRuntimeInitializeFn)(const BRScriptApiV1* api, BRScriptRuntimeCallbacks* callbacks);

#ifdef __cplusplus
}

/* The managed mirrors (Managed/BasicRenderer.Scripting/Interop/NativeAbi.cs) assume exactly these layouts. */
static_assert(sizeof(BRVec3) == 12);
static_assert(sizeof(BRQuat) == 16);
static_assert(sizeof(BRTransform) == 40);
static_assert(sizeof(BRScriptHandle) == 8);
static_assert(sizeof(BRLightDesc) == 40);
static_assert(sizeof(BRCameraProjection) == 16);
static_assert(sizeof(BRSceneStatus) == 32);
#if INTPTR_MAX == INT64_MAX
static_assert(sizeof(BRScriptApiV1) == 32 + 24 * 8, "BRScriptApiV1 is append-only: update the managed mirror");
static_assert(sizeof(BRScriptRuntimeCallbacks) == 8 + 4 * 8, "BRScriptRuntimeCallbacks is append-only: update the managed mirror");
#endif
#endif

#endif
