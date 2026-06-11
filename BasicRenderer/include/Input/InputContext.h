#pragma once

#include <Windows.h>
#include <functional>
#include <unordered_map>
#include <iostream>
#include <spdlog/spdlog.h>
#include <cctype>

#include "Input/InputAction.h"

enum class InputMode {
    wasd,
    orbital
};

class InputManager;

class InputContext {
public:
    virtual void ProcessInput(UINT message, WPARAM wParam, LPARAM lParam) = 0;

    void SetActionHandler(InputAction action, std::function<void(float magnitude, const InputData&)> handler) {
        actionHandlers[action] = handler;
    }

	virtual ~InputContext() = default;

protected:
    void TriggerAction(InputAction action, float magnitude, InputData inputData) {

        if (actionHandlers.find(action) != actionHandlers.end()) {
            actionHandlers[action](magnitude, inputData);
        }
    }

private:
    std::unordered_map<InputAction, std::function<void(float magnitude, const InputData&)>> actionHandlers;
};

class WASDContext : public InputContext {
public:
    void ProcessInput(UINT message, WPARAM wParam, LPARAM lParam) override {
        float magnitude = (message == WM_KEYDOWN) ? 1.0f : 0.0f;
        InputData inputData = {};
        inputData.mouseX = GET_X_LPARAM(lParam);
        inputData.mouseY = GET_Y_LPARAM(lParam);
        switch (message) {
        case WM_KEYDOWN: {
            WPARAM key = toupper(static_cast<int>(wParam));
            switch (key) {
			case 'W':
                TriggerAction(InputAction::MoveForward, magnitude, inputData);
				break;
			case 'S':
				TriggerAction(InputAction::MoveBackward, magnitude, inputData);
				break;
			case 'A':
				TriggerAction(InputAction::MoveLeft, magnitude, inputData);
				break;
			case 'D':
				TriggerAction(InputAction::MoveRight, magnitude, inputData);
				break;
			case 16:
				TriggerAction(InputAction::MoveDown, magnitude, inputData);
				break;
			case ' ':
				TriggerAction(InputAction::MoveUp, magnitude, inputData);
				break;
			case 'R':
				TriggerAction(InputAction::Reset, magnitude, inputData);
				break;
            case 'X':
                TriggerAction(InputAction::X, magnitude, inputData);
                break;
            case 'Z':
                TriggerAction(InputAction::Z, magnitude, inputData);
                break;
            }
           

            break;
        } case WM_KEYUP: {
            WPARAM key = toupper(static_cast<int>(wParam));
			switch (key) {
			case 'W':
				TriggerAction(InputAction::MoveForward, 0.0f, inputData);
				break;
			case 'S':
				TriggerAction(InputAction::MoveBackward, 0.0f, inputData);
				break;
			case 'A':
				TriggerAction(InputAction::MoveLeft, 0.0f, inputData);
				break;
			case 'D':
				TriggerAction(InputAction::MoveRight, 0.0f, inputData);
				break;
			case 16:
				TriggerAction(InputAction::MoveDown, 0.0f, inputData);
				break;
			case ' ':
				TriggerAction(InputAction::MoveUp, 0.0f, inputData);
				break;
			}
			break;
        }
        case WM_LBUTTONDOWN:
            mousedown = true;
            lastMouseX = inputData.mouseX;
            lastMouseY = inputData.mouseY;
            break;
        case WM_LBUTTONUP:
            mousedown = false;
            break;
        case WM_MOUSEMOVE:
            if (mousedown) {

                inputData.mouseDeltaX = inputData.mouseX - lastMouseX;
                inputData.mouseDeltaY = inputData.mouseY - lastMouseY;
                lastMouseX = inputData.mouseX;
                lastMouseY = inputData.mouseY;

                inputData.scrollDelta = GET_WHEEL_DELTA_WPARAM(wParam);
                TriggerAction(InputAction::RotateCamera, 1.0, inputData);
            }
            break;
            // Ignore mouse scroll in this context
        default:
            break;
        }
    }
private:
    int lastMouseX;
    int lastMouseY;
    bool mousedown = false;
};

class OrbitalCameraContext : public InputContext {
public:
    void ProcessInput(UINT message, WPARAM wParam, LPARAM lParam) override {
        InputData inputData = {};
        switch (message) {
        case WM_MOUSEMOVE:
            TriggerAction(InputAction::RotateCamera, 1.0, inputData);
            break;
        case WM_MOUSEWHEEL:
            if (GET_WHEEL_DELTA_WPARAM(wParam) > 0) {
                TriggerAction(InputAction::ZoomIn, 1.0, inputData);
            }
            else {
                TriggerAction(InputAction::ZoomOut, 1.0, inputData);
            }
            break;
            // Ignore WASD keys in this context
        default:
            break;
        }
    }
private:
};
