#pragma once

class PSOManager;
class CommandSignatureManager;
class SettingsManager;
class RendererECSManager;
struct RenderContext;

// Narrow dependency bundle for geometry/material producer passes. The owning
// renderer (or an external SARP module host) supplies these services; producer
// passes must not discover process-global managers themselves.
struct ProducerPassServices {
    PSOManager* pipelines = nullptr;
    CommandSignatureManager* commandSignatures = nullptr;
    SettingsManager* settings = nullptr;
    RendererECSManager* ecs = nullptr;
    RenderContext* renderContext = nullptr;

    bool IsValid() const noexcept {
        return pipelines && commandSignatures && settings && ecs && renderContext;
    }
};
