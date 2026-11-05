#include <BasicRenderer/Diagnostics/ExternalUi.h>

#include <utility>

#include "Diagnostics/Menu/Menu.h"
#include "Runtime/Device/DeviceManager.h"

namespace br::diagnostics {

	void SetExternalUiPanel(std::function<void()> panel)
	{
		Menu::GetInstance().SetExternalPanel(std::move(panel));
	}

	void SetRendererMenuEnabled(bool enabled)
	{
		Menu::GetInstance().SetMenuEnabled(enabled);
	}

	ExternalUiDescriptor AllocateExternalUiDescriptor()
	{
		Menu& menu = Menu::GetInstance();
		ExternalUiDescriptor descriptor;
		descriptor.index = menu.AllocateImGuiDescriptor();
		descriptor.cpuHandle = menu.GetImGuiCpuDescriptorHandle(descriptor.index);
		descriptor.gpuHandle = static_cast<uint64_t>(menu.GetImGuiGpuDescriptorHandle(descriptor.index));
		return descriptor;
	}

	void FreeExternalUiDescriptor(uint32_t index)
	{
		Menu::GetInstance().FreeImGuiDescriptor(index);
	}

	ID3D12Device* GetExternalUiDevice()
	{
		return rhi::dx12::get_device(DeviceManager::GetInstance().GetDevice());
	}

}
