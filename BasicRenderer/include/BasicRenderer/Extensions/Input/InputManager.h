#pragma once

#include <Windowsx.h>
#include <functional>
#include <BasicRenderer/Extensions/Input/InputContext.h>
#include <spdlog/spdlog.h>

class InputManager {
public:
    void SetInputContext(InputContext* context) {
        currentContext = context;
    }

    void ProcessInput(UINT message, WPARAM wParam, LPARAM lParam) {        
        if (currentContext) {
            currentContext->ProcessInput(message, wParam, lParam);
        }
    }

    void RegisterAction(InputAction action, std::function<void(float magnitude, const InputData& inputData)> handler) {
        if (currentContext) {
            currentContext->SetActionHandler(action, handler);
        }
    }

    InputContext* GetCurrentContext() {
        return currentContext;
    }

private:
    InputContext* currentContext = nullptr;
};
