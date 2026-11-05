#pragma once

#include <cstdint>
#include <functional>

struct ID3D12Device;

// For tools that link the renderer and draw their own ImGui windows in its frame.
namespace br::diagnostics {

// Drawn every frame between ImGui's NewFrame and Render.
void SetExternalUiPanel(std::function<void()> panel);

// Whether the renderer's own menu shows. It starts on.
void SetRendererMenuEnabled(bool enabled);

// A slot in ImGui's descriptor heap, for an SRV the tool creates at cpuHandle and draws with gpuHandle as its ImTextureID.
struct ExternalUiDescriptor {
	uint32_t index = 0;
	uint64_t cpuHandle = 0;
	uint64_t gpuHandle = 0;
};
ExternalUiDescriptor AllocateExternalUiDescriptor();
void FreeExternalUiDescriptor(uint32_t index);

ID3D12Device* GetExternalUiDevice();

}
