#include "BasicRendererScripting/RendererScriptBackend.h"

#include <BasicRenderer/Renderer.h>
#include <BasicRenderer/Assets/CachePaths.h>
#include <BasicRenderer/Assets/Import/ModelLoader.h>
#include <BasicRenderer/Diagnostics/ControlCommandRegistry.h>
#include <BasicRenderer/Extensions/SettingAccess.h>
#include <BasicRenderer/Scene/Components.h>
#include <BasicRenderer/Scene/Scene.h>

#include <DirectXMath.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../HandleTable.h"

namespace br::scripting {

    namespace {

        using namespace DirectX;

        XMMATRIX ToMatrix(const BRTransform& t)
        {
            const XMVECTOR scale = XMVectorSet(t.scale.x, t.scale.y, t.scale.z, 0.0f);
            const XMVECTOR rotation = XMQuaternionNormalize(XMVectorSet(t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w));
            const XMVECTOR position = XMVectorSet(t.position.x, t.position.y, t.position.z, 0.0f);
            // The renderer's own composition (Scene.cpp's transform system): scale, then rotation, then translation.
            return XMMatrixScalingFromVector(scale) * XMMatrixRotationQuaternion(rotation) * XMMatrixTranslationFromVector(position);
        }

        XMVECTOR Rotation(const BRTransform& t)
        {
            const XMVECTOR q = XMVectorSet(t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w);
            return XMVectorGetX(XMVector4LengthSq(q)) > 1e-12f ? XMQuaternionNormalize(q) : XMQuaternionIdentity();
        }

        XMFLOAT3 Float3(const BRVec3& v) { return { v.x, v.y, v.z }; }

        bool ValidTransform(const BRTransform& t)
        {
            const float values[] = { t.position.x, t.position.y, t.position.z, t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w,
                t.scale.x, t.scale.y, t.scale.z };
            for (float v : values) {
                if (!std::isfinite(v)) return false;
            }
            return true;
        }

        // Models load on their own threads (LoadImportedAsset runs on the asset-import lane elsewhere, so it is thread
        // safe); entities are only created at the safe point. Payloads are shared by every instance of a path and kept
        // across scenarios, so a reload does not import again.
        class ModelCache {
        public:
            using Payload = std::shared_ptr<const br::import::ImportedAssetPayload>;
            enum class Poll { Loading, Ready, Failed };

            void Request(const std::string& path)
            {
                if (m_entries.contains(path)) return;
                auto& entry = m_entries[path];
                entry.result = std::async(std::launch::async, [path]() {
                    Result result;
                    try {
                        auto payload = br::import::LoadImportedAsset(path);
                        if (!payload) result.error = std::format("could not load '{}'", path);
                        else if (payload->parts.empty()) result.error = std::format("'{}' has no renderable parts", path);
                        else result.payload = std::make_shared<const br::import::ImportedAssetPayload>(std::move(*payload));
                    } catch (const std::exception& e) {
                        result.error = std::format("loading '{}' threw: {}", path, e.what());
                    }
                    return result;
                }).share();
            }

            Poll Get(const std::string& path, Payload& payload, std::string& error)
            {
                const auto found = m_entries.find(path);
                if (found == m_entries.end()) {
                    error = "not requested";
                    return Poll::Failed;
                }
                auto& future = found->second.result;
                if (future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return Poll::Loading;
                const auto& result = future.get();
                if (!result.payload) {
                    error = result.error;
                    return Poll::Failed;
                }
                payload = result.payload;
                return Poll::Ready;
            }

            void Forget(const std::string& path) { m_entries.erase(path); }

            void Clear()
            {
                for (auto& [path, entry] : m_entries) {
                    if (entry.result.valid()) entry.result.wait();
                }
                m_entries.clear();
            }

        private:
            struct Result {
                Payload payload;
                std::string error;
            };
            struct Entry {
                std::shared_future<Result> result;
            };
            std::unordered_map<std::string, Entry> m_entries;
        };

        enum class Kind { Model, Light, Camera };
        enum class State { Pending, Ready, Failed };

        struct Record {
            Kind kind = Kind::Model;
            State state = State::Pending;
            std::string name;
            std::string path;   // models
            std::string error;
            BRTransform transform{ {}, { 0, 0, 0, 1 }, { 1, 1, 1 } };
            bool transformDirty = false;
            BRScriptHandle parent = 0;
            bool parentDirty = false;
            BRLightDesc light{};
            bool lightDirty = false;
            bool destroyRequested = false;
            flecs::entity entity;
            XMFLOAT4X4 world{};
            bool hasWorld = false;
        };

        class RendererScriptBackend final : public ScriptBackend {
        public:
            explicit RendererScriptBackend(const RendererScriptBackendOptions& options)
                : m_renderer(*options.renderer), m_commands(options.commands)
            {
                br::render::SceneMutationHookOptions hookOptions;
                hookOptions.waitForInFlightSceneTask = options.waitForSceneTask;
                m_hook = m_renderer.AddSceneMutationHook([this](Scene& scene) { Apply(scene); }, hookOptions);
            }

            // --- Lifecycle ---------------------------------------------------------------------------------------

            void OnScenarioStarting() override
            {
                // The previous scenario's entities go at the next safe point; its camera control ends now.
                m_records.ForEach([](BRScriptHandle, Record& record) {
                    if (record.kind != Kind::Camera) record.destroyRequested = true;
                });
                m_pendingProjection.reset();
                RestoreCameraInput();
            }

            void OnScenarioFinished() override { RestoreCameraInput(); }

            void Shutdown() override
            {
                // Outside Update the scene may be in use by the asynchronous update, so nothing is destroyed here: the
                // entities go with the scene. Only the backend's references are dropped.
                RestoreCameraInput();
                m_hook.Reset();
                m_records = {};
                m_models.Clear();
            }

            // --- Entities ----------------------------------------------------------------------------------------

            BRScriptStatus InstantiateModel(std::string_view path, std::string_view name, const BRTransform& transform, BRScriptHandle& out) override
            {
                if (!ValidTransform(transform)) return SetScriptError(BR_SCRIPT_E_INVALID_ARGUMENT, "model_instantiate: transform is not finite");
                std::error_code ec;
                auto absolute = std::filesystem::absolute(std::filesystem::path(std::u8string(path.begin(), path.end())), ec);
                if (ec || !std::filesystem::exists(absolute, ec)) {
                    return SetScriptError(BR_SCRIPT_E_NOT_FOUND, std::format("model not found: {}", path));
                }
                Record record;
                record.kind = Kind::Model;
                record.path = absolute.lexically_normal().string();
                record.name = name.empty() ? absolute.stem().string() : std::string(name);
                record.transform = transform;
                m_models.Request(record.path);
                out = m_records.Add(std::move(record));
                return BR_SCRIPT_OK;
            }

            BRScriptStatus GetEntityState(BRScriptHandle handle, int32_t& state) override
            {
                Record* record = Find(handle);
                if (!record) return Stale(handle);
                switch (record->state) {
                case State::Pending: state = BR_SCRIPT_ENTITY_PENDING; break;
                case State::Ready: state = BR_SCRIPT_ENTITY_READY; break;
                case State::Failed:
                    state = BR_SCRIPT_ENTITY_FAILED;
                    SetScriptError(BR_SCRIPT_OK, record->error);
                    break;
                }
                return BR_SCRIPT_OK;
            }

            BRScriptStatus DestroyEntity(BRScriptHandle handle) override
            {
                Record* record = Find(handle);
                if (!record) return Stale(handle);
                if (record->kind == Kind::Camera) return SetScriptError(BR_SCRIPT_E_UNSUPPORTED, "the camera cannot be destroyed");
                if (record->state != State::Ready) {
                    // Never reached the scene: nothing to release there.
                    DetachChildren(handle);
                    m_records.Remove(handle);
                    return BR_SCRIPT_OK;
                }
                record->destroyRequested = true;
                return BR_SCRIPT_OK;
            }

            BRScriptStatus SetTransform(BRScriptHandle handle, const BRTransform& transform) override
            {
                Record* record = Find(handle);
                if (!record) return Stale(handle);
                if (!ValidTransform(transform)) return SetScriptError(BR_SCRIPT_E_INVALID_ARGUMENT, "entity_set_transform: transform is not finite");
                record->transform = transform;
                record->transformDirty = true;
                return BR_SCRIPT_OK;
            }

            BRScriptStatus GetTransform(BRScriptHandle handle, BRTransform& transform) override
            {
                Record* record = Find(handle);
                if (!record) return Stale(handle);
                transform = record->transform;
                return BR_SCRIPT_OK;
            }

            BRScriptStatus SetParent(BRScriptHandle handle, BRScriptHandle parent) override
            {
                Record* record = Find(handle);
                if (!record) return Stale(handle);
                if (record->kind == Kind::Camera) return SetScriptError(BR_SCRIPT_E_UNSUPPORTED, "the camera cannot be parented");
                if (parent != 0) {
                    if (parent == handle) return SetScriptError(BR_SCRIPT_E_INVALID_ARGUMENT, "an entity cannot be its own parent");
                    if (!Find(parent)) return Stale(parent);
                    for (BRScriptHandle ancestor = parent; ancestor != 0;) {
                        Record* above = Find(ancestor);
                        if (!above) break;
                        if (above->parent == handle) return SetScriptError(BR_SCRIPT_E_INVALID_ARGUMENT, "that parent would make a cycle");
                        ancestor = above->parent;
                    }
                }
                record->parent = parent;
                record->parentDirty = true;
                return BR_SCRIPT_OK;
            }

            BRScriptStatus GetWorldMatrix(BRScriptHandle handle, float* out) override
            {
                Record* record = Find(handle);
                if (!record) return Stale(handle);
                if (!record->hasWorld) return SetScriptError(BR_SCRIPT_E_NOT_FOUND, "the entity has not been rendered yet");
                std::memcpy(out, &record->world, sizeof(float) * 16);
                return BR_SCRIPT_OK;
            }

            // --- Camera ------------------------------------------------------------------------------------------

            BRScriptStatus GetCamera(BRScriptHandle& out) override
            {
                if (!Find(m_cameraHandle)) {
                    Record record;
                    record.kind = Kind::Camera;
                    record.name = "camera";
                    record.state = m_cameraCache ? State::Ready : State::Pending;
                    if (m_cameraCache) record.transform = m_cameraCache->transform;
                    m_cameraHandle = m_records.Add(std::move(record));
                }
                out = m_cameraHandle;
                return BR_SCRIPT_OK;
            }

            BRScriptStatus SetCameraProjection(float fovYRadians, float zNear, float zFar) override
            {
                if (fovYRadians > 0 && !(fovYRadians > 0.01f && fovYRadians < 3.0f)) {
                    return SetScriptError(BR_SCRIPT_E_INVALID_ARGUMENT, "camera_set_projection: the field of view must be in (0.01, 3) radians");
                }
                auto projection = m_pendingProjection.value_or(BRCameraProjection{});
                if (m_cameraCache && !m_pendingProjection) projection = m_cameraCache->projection;
                if (fovYRadians > 0) projection.fovYRadians = fovYRadians;
                if (zNear > 0) projection.zNear = zNear;
                if (zFar > 0) projection.zFar = zFar;
                m_pendingProjection = projection;
                return BR_SCRIPT_OK;
            }

            BRScriptStatus GetCameraProjection(BRCameraProjection& out) override
            {
                if (m_pendingProjection) {
                    out = *m_pendingProjection;
                    if (m_cameraCache) out.aspect = m_cameraCache->projection.aspect;
                    return BR_SCRIPT_OK;
                }
                if (!m_cameraCache) return SetScriptError(BR_SCRIPT_E_NOT_FOUND, "the scene has no camera yet");
                out = m_cameraCache->projection;
                return BR_SCRIPT_OK;
            }

            BRScriptStatus SetCameraInputEnabled(bool enabled) override
            {
                if (!m_inputBeforeScript) m_inputBeforeScript = m_renderer.GetCameraInputEnabled();
                m_renderer.SetCameraInputEnabled(enabled);
                return BR_SCRIPT_OK;
            }

            // --- Lights ------------------------------------------------------------------------------------------

            BRScriptStatus CreateLight(const BRLightDesc& desc, const BRTransform& transform, std::string_view name, BRScriptHandle& out) override
            {
                if (desc.type < BR_SCRIPT_LIGHT_POINT || desc.type > BR_SCRIPT_LIGHT_DIRECTIONAL) {
                    return SetScriptError(BR_SCRIPT_E_INVALID_ARGUMENT, std::format("light_create: unknown light type {}", desc.type));
                }
                if (!ValidTransform(transform)) return SetScriptError(BR_SCRIPT_E_INVALID_ARGUMENT, "light_create: transform is not finite");
                Record record;
                record.kind = Kind::Light;
                record.name = name.empty() ? "script light" : std::string(name);
                record.light = desc;
                record.transform = transform;
                out = m_records.Add(std::move(record));
                return BR_SCRIPT_OK;
            }

            BRScriptStatus SetLight(BRScriptHandle handle, const BRLightDesc& desc) override
            {
                Record* record = Find(handle);
                if (!record) return Stale(handle);
                if (record->kind != Kind::Light) return SetScriptError(BR_SCRIPT_E_TYPE_MISMATCH, "light_set: the entity is not a light");
                const int32_t type = record->light.type;
                const int32_t shadows = record->light.castsShadows;
                record->light = desc;
                record->light.type = type;
                record->light.castsShadows = shadows;
                record->lightDirty = record->state == State::Ready || record->lightDirty;
                return BR_SCRIPT_OK;
            }

            BRScriptStatus GetLight(BRScriptHandle handle, BRLightDesc& out) override
            {
                Record* record = Find(handle);
                if (!record) return Stale(handle);
                if (record->kind != Kind::Light) return SetScriptError(BR_SCRIPT_E_TYPE_MISMATCH, "light_get: the entity is not a light");
                out = record->light;
                return BR_SCRIPT_OK;
            }

            // --- Scene, settings, commands -----------------------------------------------------------------------

            BRScriptStatus SetEnvironment(std::string_view name) override
            {
                m_renderer.SetEnvironment(std::string(name));
                return BR_SCRIPT_OK;
            }

            BRScriptStatus GetSceneStatus(BRSceneStatus& out) override
            {
                // The demo sampler's readiness criterion (BasicRenderer.cpp, BuildDemoSamplingReadiness): no blocking
                // work, and the workload signature unchanged. The asynchronous scene update is not counted: it is
                // normally running when a script asks.
                const auto snapshot = m_renderer.GetSamplingReadinessSnapshot(false);
                out = {};
                out.frame = m_renderer.GetTotalFramesRendered();
                out.pendingWork = uint64_t(snapshot.pendingTextureReloads) + snapshot.queuedClodRequests + snapshot.inFlightClodGroups +
                    snapshot.completedClodResults + snapshot.pendingDirectStorageLaunches + snapshot.pendingDirectStorageUploads +
                    snapshot.ioTasks + snapshot.backgroundTasks + snapshot.shaderCompileTasks + snapshot.deferredRetireQueueDepth;
                uint64_t signature = 1469598103934665603ull;
                for (const uint64_t value : { uint64_t(snapshot.fullResolutionTextures), uint64_t(snapshot.residentClodGroups),
                         snapshot.drawRecordsAllocated }) {
                    signature = (signature ^ value) * 1099511628211ull;
                }
                out.workloadSignature = signature;
                out.pendingShaderCompiles = m_renderer.GetPendingShaderCompileCount();
                uint32_t pendingModels = 0;
                m_records.ForEach([&](BRScriptHandle, Record& record) {
                    if (record.kind == Kind::Model && record.state == State::Pending) ++pendingModels;
                });
                out.pendingModels = pendingModels;
                return BR_SCRIPT_OK;
            }

            BRScriptStatus GetSetting(std::string_view name, int32_t type, void* out) override
            {
                const std::string key(name);
                if (const auto status = CheckSettingType(key, type); status != BR_SCRIPT_OK) return status;
                switch (type) {
                case BR_SCRIPT_SETTING_BOOL: *static_cast<int32_t*>(out) = br::extensions::MakeSettingGetter<bool>(key)() ? 1 : 0; break;
                case BR_SCRIPT_SETTING_INT32: *static_cast<int32_t*>(out) = br::extensions::MakeSettingGetter<int32_t>(key)(); break;
                case BR_SCRIPT_SETTING_UINT32: *static_cast<uint32_t*>(out) = br::extensions::MakeSettingGetter<uint32_t>(key)(); break;
                case BR_SCRIPT_SETTING_FLOAT: *static_cast<float*>(out) = br::extensions::MakeSettingGetter<float>(key)(); break;
                case BR_SCRIPT_SETTING_UINT8: *static_cast<uint32_t*>(out) = br::extensions::MakeSettingGetter<uint8_t>(key)(); break;
                }
                return BR_SCRIPT_OK;
            }

            BRScriptStatus SetSetting(std::string_view name, int32_t type, const void* value) override
            {
                const std::string key(name);
                if (const auto status = CheckSettingType(key, type); status != BR_SCRIPT_OK) return status;
                switch (type) {
                case BR_SCRIPT_SETTING_BOOL: br::extensions::MakeSettingSetter<bool>(key)(*static_cast<const int32_t*>(value) != 0); break;
                case BR_SCRIPT_SETTING_INT32: br::extensions::MakeSettingSetter<int32_t>(key)(*static_cast<const int32_t*>(value)); break;
                case BR_SCRIPT_SETTING_UINT32: br::extensions::MakeSettingSetter<uint32_t>(key)(*static_cast<const uint32_t*>(value)); break;
                case BR_SCRIPT_SETTING_FLOAT: br::extensions::MakeSettingSetter<float>(key)(*static_cast<const float*>(value)); break;
                case BR_SCRIPT_SETTING_UINT8: {
                    const uint32_t wide = *static_cast<const uint32_t*>(value);
                    if (wide > 255) return SetScriptError(BR_SCRIPT_E_INVALID_ARGUMENT, std::format("setting '{}' takes 0..255", key));
                    br::extensions::MakeSettingSetter<uint8_t>(key)(static_cast<uint8_t>(wide));
                    break;
                }
                }
                return BR_SCRIPT_OK;
            }

            std::string InvokeCommand(std::string_view requestJson) override
            {
                if (!m_commands) return R"({"ok":false,"error":"this host has no control commands"})";
                nlohmann::json request = nlohmann::json::parse(requestJson, nullptr, false);
                if (request.is_discarded() || !request.is_object()) return R"({"ok":false,"error":"the request is not a JSON object"})";
                return m_commands->Invoke(request).dump();
            }

        private:
            // --- The safe point ----------------------------------------------------------------------------------

            void Apply(Scene& scene)
            {
                try {
                    if (scene.GetSceneID() != m_sceneID) {
                        if (m_sceneID != 0) OnSceneReplaced();
                        m_sceneID = scene.GetSceneID();
                    }
                    ApplyDestroys(scene);
                    RefreshCamera(scene);
                    RefreshWorldMatrices();
                    ApplyCreations(scene);
                    ApplyParents(scene);
                    ApplyTransformsAndLights();
                    if (m_pendingProjection && scene.HasUsablePrimaryCamera() && m_cameraCache) {
                        const auto& p = *m_pendingProjection;
                        scene.SetCameraProjection(p.fovYRadians, m_cameraCache->projection.aspect, p.zNear, p.zFar);
                        m_cameraCache->projection = { p.fovYRadians, m_cameraCache->projection.aspect, p.zNear, p.zFar };
                        m_pendingProjection.reset();
                    }
                } catch (const std::exception& e) {
                    spdlog::error("[script] applying scene changes failed: {}", e.what());
                }
            }

            void OnSceneReplaced()
            {
                // The entities went with the old scene.
                m_records.ForEach([](BRScriptHandle, Record& record) {
                    if (record.kind == Kind::Camera) return;
                    record.entity = {};
                    record.state = State::Failed;
                    record.error = "the scene was replaced";
                    record.destroyRequested = false;
                });
            }

            void ApplyDestroys(Scene& scene)
            {
                std::vector<BRScriptHandle> doomed;
                m_records.ForEach([&](BRScriptHandle handle, Record& record) {
                    if (record.destroyRequested) doomed.push_back(handle);
                });
                for (const auto handle : doomed) {
                    DetachChildren(handle);
                    Record* record = Find(handle);
                    if (record->state == State::Ready && record->entity.is_alive()) {
                        if (record->kind == Kind::Model) scene.ReleaseModel(record->entity);
                        else record->entity.destruct();
                    }
                    m_records.Remove(handle);
                }
            }

            // Children of a destroyed entity go back under the scene root, keeping their local transforms.
            void DetachChildren(BRScriptHandle parent)
            {
                Record* removed = Find(parent);
                const bool live = removed && removed->state == State::Ready && removed->entity.is_alive();
                m_records.ForEach([&](BRScriptHandle, Record& record) {
                    if (record.parent != parent) return;
                    record.parent = 0;
                    record.parentDirty = true;
                    if (live && record.state == State::Ready && record.entity.is_alive()) {
                        // Before the parent is destroyed, which would destroy its flecs children with it.
                        if (auto& scene = m_renderer.GetCurrentScene()) record.entity.child_of(scene->GetRoot());
                    }
                });
            }

            void RefreshCamera(Scene& scene)
            {
                if (!scene.HasUsablePrimaryCamera()) {
                    m_cameraCache.reset();
                    return;
                }
                auto camera = scene.GetPrimaryCamera();
                CameraCache cache;
                const auto& position = camera.get<Components::Position>().pos;
                const auto& rotation = camera.get<Components::Rotation>().rot;
                cache.transform.position = { XMVectorGetX(position), XMVectorGetY(position), XMVectorGetZ(position) };
                cache.transform.rotation = { XMVectorGetX(rotation), XMVectorGetY(rotation), XMVectorGetZ(rotation), XMVectorGetW(rotation) };
                cache.transform.scale = { 1, 1, 1 };
                const auto& lens = camera.get<Components::Camera>();
                cache.projection = { lens.fov, lens.aspect, lens.zNear, lens.zFar };
                XMStoreFloat4x4(&cache.world, camera.get<Components::Matrix>().matrix);
                m_cameraCache = cache;

                if (Record* record = Find(m_cameraHandle)) {
                    record->entity = camera;
                    record->state = State::Ready;
                    record->world = cache.world;
                    record->hasWorld = true;
                    // Until the script moves it, the camera's transform is wherever the renderer has it.
                    if (!record->transformDirty) record->transform = cache.transform;
                }
            }

            void RefreshWorldMatrices()
            {
                m_records.ForEach([](BRScriptHandle, Record& record) {
                    if (record.kind == Kind::Camera || record.state != State::Ready || !record.entity.is_alive()) return;
                    if (const auto* matrix = record.entity.try_get<Components::Matrix>()) {
                        XMStoreFloat4x4(&record.world, matrix->matrix);
                        record.hasWorld = true;
                    }
                });
            }

            void ApplyCreations(Scene& scene)
            {
                m_records.ForEach([&](BRScriptHandle, Record& record) {
                    if (record.state != State::Pending || record.kind == Kind::Camera) return;
                    if (record.kind == Kind::Model) {
                        ModelCache::Payload payload;
                        std::string error;
                        switch (m_models.Get(record.path, payload, error)) {
                        case ModelCache::Poll::Loading: return;
                        case ModelCache::Poll::Failed:
                            record.state = State::Failed;
                            record.error = error;
                            m_models.Forget(record.path);   // a later instantiate retries
                            spdlog::error("[script] {}", error);
                            return;
                        case ModelCache::Poll::Ready:
                            record.entity = scene.InstantiateModel(*payload, s2ws(record.name), ToMatrix(record.transform));
                            break;
                        }
                    } else {
                        record.entity = CreateLightEntity(scene, record);
                        record.lightDirty = false;
                    }
                    record.state = State::Ready;
                    record.transformDirty = true;   // the shadow transform is authoritative from the first frame
                    record.parentDirty = record.parentDirty || record.parent != 0;
                });
            }

            flecs::entity CreateLightEntity(Scene& scene, const Record& record)
            {
                const auto& light = record.light;
                const std::wstring name = s2ws(record.name);
                const XMFLOAT3 position = Float3(record.transform.position);
                XMFLOAT3 direction;
                XMStoreFloat3(&direction, XMVector3Rotate(XMVectorSet(0, 0, 1, 0), Rotation(record.transform)));
                const XMFLOAT3 color = Float3(light.color);
                const bool shadows = light.castsShadows != 0;
                switch (light.type) {
                case BR_SCRIPT_LIGHT_DIRECTIONAL: return scene.CreateDirectionalLightECS(name, color, light.intensity, direction, shadows);
                case BR_SCRIPT_LIGHT_SPOT:
                    return scene.CreateSpotLightECS(name, position, color, light.intensity, direction, light.innerConeRadians, light.outerConeRadians,
                        light.range, light.sourceRadius, shadows);
                default: return scene.CreatePointLightECS(name, position, color, light.intensity, light.range, light.sourceRadius, shadows);
                }
            }

            void ApplyParents(Scene& scene)
            {
                m_records.ForEach([&](BRScriptHandle, Record& record) {
                    if (!record.parentDirty || record.state != State::Ready || !record.entity.is_alive()) return;
                    if (record.parent == 0) {
                        record.entity.child_of(scene.GetRoot());
                    } else {
                        Record* parent = Find(record.parent);
                        if (!parent || parent->state == State::Failed) {
                            record.parent = 0;
                            record.entity.child_of(scene.GetRoot());
                        } else if (parent->state != State::Ready || !parent->entity.is_alive()) {
                            return;   // waits for the parent to appear
                        } else {
                            record.entity.child_of(parent->entity);
                        }
                    }
                    record.parentDirty = false;
                    record.transformDirty = true;
                });
            }

            void ApplyTransformsAndLights()
            {
                m_records.ForEach([&](BRScriptHandle, Record& record) {
                    if (record.state != State::Ready || !record.entity.is_alive()) return;
                    if (record.transformDirty) {
                        const auto& t = record.transform;
                        record.entity.set<Components::Position>(XMFLOAT3{ t.position.x, t.position.y, t.position.z });
                        record.entity.set<Components::Rotation>(Components::Rotation(Rotation(t)));
                        if (record.kind != Kind::Camera) {
                            record.entity.set<Components::Scale>(XMFLOAT3{ t.scale.x, t.scale.y, t.scale.z });
                        }
                        record.transformDirty = false;
                    }
                    if (record.kind == Kind::Light && record.lightDirty) {
                        if (const auto* current = record.entity.try_get<Components::Light>()) {
                            Components::Light light = *current;
                            const auto& desc = record.light;
                            XMVECTOR color = XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(&desc.color));
                            if (XMVectorGetX(XMVector3LengthSq(color)) < 1e-12f) color = XMVectorSet(1, 1, 1, 0);
                            light.color = Float3(desc.color);
                            light.lightInfo.color = XMVectorSetW(XMVector3Normalize(color), desc.intensity);
                            if (desc.range > 0) {
                                light.range = desc.range;
                                light.lightInfo.range = desc.range;
                            }
                            light.lightInfo.sourceRadius = std::max(desc.sourceRadius, 0.0f);
                            light.lightInfo.innerConeCos = std::cos(desc.innerConeRadians);
                            light.lightInfo.outerConeCos = std::cos(desc.outerConeRadians);
                            record.entity.set<Components::Light>(light);
                        }
                        record.lightDirty = false;
                    }
                });
            }

            // --- Helpers -----------------------------------------------------------------------------------------

            Record* Find(BRScriptHandle handle) { return handle ? m_records.Find(handle) : nullptr; }

            static BRScriptStatus Stale(BRScriptHandle handle)
            {
                return SetScriptError(BR_SCRIPT_E_STALE_HANDLE, std::format("entity handle 0x{:x} is not live (destroyed, or from another run)", handle));
            }

            BRScriptStatus CheckSettingType(const std::string& name, int32_t type)
            {
                using br::extensions::SettingValueType;
                const SettingValueType actual = br::extensions::GetSettingValueType(name);
                if (actual == SettingValueType::Missing) return SetScriptError(BR_SCRIPT_E_NOT_FOUND, std::format("no setting named '{}'", name));
                const SettingValueType expected = type == BR_SCRIPT_SETTING_BOOL ? SettingValueType::Bool
                    : type == BR_SCRIPT_SETTING_INT32 ? SettingValueType::Int32
                    : type == BR_SCRIPT_SETTING_UINT32 ? SettingValueType::UInt32
                    : type == BR_SCRIPT_SETTING_FLOAT ? SettingValueType::Float
                    : type == BR_SCRIPT_SETTING_UINT8 ? SettingValueType::UInt8
                    : SettingValueType::Missing;
                if (expected == SettingValueType::Missing) return SetScriptError(BR_SCRIPT_E_INVALID_ARGUMENT, std::format("unknown setting type {}", type));
                if (actual != expected) {
                    static constexpr const char* kNames[] = { "missing", "bool", "float", "uint", "int", "byte", "a type scripts cannot use" };
                    return SetScriptError(BR_SCRIPT_E_TYPE_MISMATCH, std::format("setting '{}' is {}, not {}", name, kNames[static_cast<int>(actual)],
                        kNames[static_cast<int>(expected)]));
                }
                return BR_SCRIPT_OK;
            }

            void RestoreCameraInput()
            {
                if (m_inputBeforeScript) {
                    m_renderer.SetCameraInputEnabled(*m_inputBeforeScript);
                    m_inputBeforeScript.reset();
                }
            }

            struct CameraCache {
                BRTransform transform{};
                BRCameraProjection projection{};
                XMFLOAT4X4 world{};
            };

            Renderer& m_renderer;
            br::diagnostics::ControlCommandRegistry* m_commands = nullptr;
            br::render::SceneMutationSubscription m_hook;
            detail::HandleTable<Record> m_records;
            ModelCache m_models;
            BRScriptHandle m_cameraHandle = 0;
            std::optional<CameraCache> m_cameraCache;
            std::optional<BRCameraProjection> m_pendingProjection;
            std::optional<bool> m_inputBeforeScript;
            uint64_t m_sceneID = 0;
        };

    } // namespace

    std::unique_ptr<ScriptBackend> CreateRendererScriptBackend(const RendererScriptBackendOptions& options)
    {
        if (!options.renderer) throw std::invalid_argument("CreateRendererScriptBackend: renderer is null");
        return std::make_unique<RendererScriptBackend>(options);
    }

} // namespace br::scripting
