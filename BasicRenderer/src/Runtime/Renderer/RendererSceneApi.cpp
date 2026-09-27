#include <BasicRenderer/Renderer.h>
#include "BasicRenderer/Scene/Scene.h"
#include "OpenRenderGraph/OpenRenderGraph.h"
#include "Materials/MaterialManager.h"
#include "Scene/Objects/IndirectCommandBufferManager.h"
#include "Runtime/Device/DeviceManager.h"
#include "Diagnostics/Menu/Menu.h"
#include "Scene/Objects/ObjectManager.h"
#include <spdlog/spdlog.h>

void Renderer::CheckDebugMessages() {
    auto device = DeviceManager::GetInstance().GetDevice();
    if (device) {
        device.CheckDebugMessages();
    }
}

void Renderer::SetEnvironment(std::string environmentName) {
	setEnvironment(environmentName);
}

std::shared_ptr<Scene>& Renderer::GetCurrentScene() {
    return currentScene;
}

void Renderer::SetCurrentScene(std::shared_ptr<Scene> newScene) {
	if (!newScene) {
    InvalidateSceneOverlapState();
        ClearExternalSnapshotMeshRegistrations();
        m_sceneRenderBridge.Clear(GetSceneIngestionServices());
        if (currentScene) {
            currentScene->Deactivate();
        }
        currentScene.reset();
        rebuildRenderGraph = true;
        IsSceneReadyForFrame(true);
        return;
    }

	if (currentScene != newScene) {
		ClearExternalSnapshotMeshRegistrations();
		m_sceneRenderBridge.Clear(GetSceneIngestionServices());
        if (currentScene) {
            currentScene->Deactivate();
        }
	}

    InvalidateSceneOverlapState();

	newScene->GetRoot().add<Components::ActiveScene>();
    currentScene = newScene;
    //currentScene->SetDepthMap(m_depthMap);
    currentScene->Activate(m_sceneIngestionServices);
    currentScene->PropagateTransforms();
    if (m_sceneRenderOverlapEnabled) {
        BootstrapCommittedSceneSnapshot();
    } else {
        RunSceneBridgeSyncStage();
    }
	m_warnedNullScene = false;
	m_warnedMissingPrimaryCamera = false;
	rebuildRenderGraph = true;
}

std::shared_ptr<Scene> Renderer::AppendScene(std::shared_ptr<Scene> scene) {
	if (!scene) {
        spdlog::warn("Renderer: attempted to append a null scene. Ignoring append request.");
        return nullptr;
    }

	if (m_sceneTaskInFlight.load()) {
        spdlog::warn("Renderer: attempted to append a scene while async scene overlap work is running. Ignoring append request for v1 overlap safety.");
        return nullptr;
    }

	if (!currentScene) {
        spdlog::warn("Renderer: attempted to append a scene while no current scene exists. Ignoring append request.");
        return nullptr;
    }

	auto appendedScene = GetCurrentScene()->AppendScene(scene);
	if (!appendedScene) {
		return nullptr;
	}

	currentScene->PropagateTransforms();
	InvalidateSceneOverlapState();
	if (m_sceneRenderOverlapEnabled) {
		BootstrapCommittedSceneSnapshot();
	} else {
		RunSceneBridgeSyncStage();
	}

	{
		org::BufferBase::ScopedBackingMutation appendBackingMutation;
		(void)org::PublishReadyDeferredBackingResizes(true);
	}
	org::runtime::FlushUploadPolicies();
	if (m_pMaterialManager) {
		m_pMaterialManager->CommitGpuVisibleSnapshot();
	}
	if (m_pIndirectCommandBufferManager && m_pObjectManager) {
		m_pIndirectCommandBufferManager->PublishDesiredState(
			m_pObjectManager->DesiredBufferStateRequirement(),
			m_pObjectManager->GetResidentInstanceDrawRecordCount(),
			m_context.publishedRendererState);
	}

	m_warnedNullScene = false;
	m_warnedMissingPrimaryCamera = false;
	rebuildRenderGraph = true;

	return appendedScene;
}

InputManager& Renderer::GetInputManager() {
    return inputManager;
}

bool Renderer::HandleMenuInput(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    return Menu::GetInstance().HandleInput(hwnd, message, wParam, lParam);
}

void Renderer::SetInputMode(InputMode mode) {
    static WASDContext wasdContext;
    static OrbitalCameraContext orbitalContext;
    switch (mode) {
    case InputMode::wasd:
        inputManager.SetInputContext(&wasdContext);
        break;
    case InputMode::orbital:
        inputManager.SetInputContext(&orbitalContext);
        break;
    }
    SetupInputHandlers();
}

void Renderer::SetCameraSpeed(float speed) {
    if (setCameraSpeed) {
        setCameraSpeed(speed);
    }
}

void Renderer::WaitForAsyncPreparation() {
    if (currentRenderGraph) currentRenderGraph->WaitForPreparation();
}

void Renderer::MoveForward() {
    spdlog::info("Moving forward!");
}

void Renderer::SetupInputHandlers() {
	auto& context = *inputManager.GetCurrentContext();
    context.SetActionHandler(InputAction::MoveForward, [this](float magnitude, const InputData& inputData) {
        //spdlog::info("Moving forward!");
        movementState.forwardMagnitude = magnitude * getCameraSpeed();
        });

    context.SetActionHandler(InputAction::MoveBackward, [this](float magnitude, const InputData& inputData) {
        //spdlog::info("Moving forward!");
        movementState.backwardMagnitude = magnitude * getCameraSpeed();
        });

    context.SetActionHandler(InputAction::MoveRight, [this](float magnitude, const InputData& inputData) {
        //spdlog::info("Moving right!");
        movementState.rightMagnitude = magnitude * getCameraSpeed();
        });

    context.SetActionHandler(InputAction::MoveLeft, [this](float magnitude, const InputData& inputData) {
        //spdlog::info("Moving right!");
        movementState.leftMagnitude = magnitude * getCameraSpeed();
        });

    context.SetActionHandler(InputAction::MoveUp, [this](float magnitude, const InputData& inputData) {
        //spdlog::info("Moving up!");
        movementState.upMagnitude = magnitude * getCameraSpeed();
        });

    context.SetActionHandler(InputAction::MoveDown, [this](float magnitude, const InputData& inputData) {
        //spdlog::info("Moving up!");
        movementState.downMagnitude = magnitude * getCameraSpeed();
        });

    context.SetActionHandler(InputAction::RotateCamera, [this](float magnitude, const InputData& inputData) {
        horizontalAngle -= static_cast<float>(inputData.mouseDeltaX) * 0.005f;
        verticalAngle -= static_cast<float>(inputData.mouseDeltaY) * 0.005f;
        });

    context.SetActionHandler(InputAction::ZoomIn, [](float magnitude, const InputData& inputData) {
        // TODO
        });

    context.SetActionHandler(InputAction::ZoomOut, [](float magnitude, const InputData& inputData) {
        // TODO
        });

	context.SetActionHandler(InputAction::Reset, [this](float magnitude, const InputData& inputData) {
        m_shaderReloadRequested = true;
		});

    context.SetActionHandler(InputAction::X, [](float magnitude, const InputData& inputData) {
        });

    context.SetActionHandler(InputAction::Z, [](float magnitude, const InputData& inputData) {
        });
}
