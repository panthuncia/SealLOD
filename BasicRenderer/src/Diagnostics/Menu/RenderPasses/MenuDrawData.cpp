#include <BasicRenderer/Scene/Scene.h>
#include "Diagnostics/Menu/Menu.h"

PreparedImGuiDrawData::PreparedImGuiDrawData(const ImDrawData& source, rhi::Backend selectedBackend,
        rhi::DescriptorHeapHandle heap) : backend(selectedBackend), resourceHeap(heap) {
        drawData.Valid = source.Valid;
        drawData.DisplayPos = source.DisplayPos;
        drawData.DisplaySize = source.DisplaySize;
        drawData.FramebufferScale = source.FramebufferScale;
        drawData.TotalIdxCount = source.TotalIdxCount;
        drawData.TotalVtxCount = source.TotalVtxCount;
		// Texture update requests belong to the owner-thread preparation step.
		// The copied packet must never ask a recording worker to enter ImGui's
		// mutable renderer backend or inspect the global platform texture list.
		drawData.Textures = nullptr;
        lists.reserve(source.CmdListsCount);
        for (const auto* list : source.CmdLists) {
            if (!list) continue;
            lists.emplace_back(list->CloneOutput());
            for (const auto& command : lists.back()->CmdBuffer) {
                if (command.UserCallback
                    && command.UserCallback != ImDrawCallback_ResetRenderState) {
                    throw std::invalid_argument(
                        "ImGui draw data contains a borrowed user callback; convert it to an owned menu command");
                }
            }
            drawData.CmdLists.push_back(lists.back().get());
        }
        drawData.CmdListsCount = drawData.CmdLists.Size;
    }

std::shared_ptr<const PreparedImGuiDrawData> Menu::PrepareDrawData(
    const RenderContext& context) {
    if (m_imguiBackend == rhi::Backend::Null) return {};
    Render(context, {});
    const auto* source = ImGui::GetDrawData();
    if (!source || !source->Valid || source->CmdListsCount == 0) return {};
	// ImGui 1.92 defers font-atlas and user-texture realization through the
	// ImDrawData texture journal.  Deep-copying only the draw lists leaves their
	// ImTextureRef values unresolved (TexID == 0), while forwarding the journal
	// would make delayed recording mutate the global ImGui backend.  Drain it
	// once on the preparation owner, then capture commands with stable IDs.
	if (source->Textures) {
		for (ImTextureData* texture : *source->Textures) {
			if (!texture) continue;
			if (m_imguiBackend == rhi::Backend::D3D12)
				ImGui_ImplDX12_UpdateTexture(texture);
		}
	}
    return std::make_shared<const PreparedImGuiDrawData>(*source, m_imguiBackend,
        g_pd3dSrvDescHeap ? g_pd3dSrvDescHeap->GetHandle() : rhi::DescriptorHeapHandle{});
}

void Menu::RecordPreparedDrawData(const PreparedImGuiDrawData& data,
    rhi::CommandList commandList, rhi::DescriptorSlot rtv,
    DirectX::XMUINT2 outputResolution) {
    if (!commandList || !rtv.heap.valid() || data.drawData.CmdListsCount == 0) return;
    if (data.backend == rhi::Backend::D3D12) {
        if (!data.resourceHeap.valid()) return;
        commandList.SetDescriptorHeaps(data.resourceHeap, std::nullopt);
    }
    rhi::ColorAttachment attachment{};
    attachment.loadOp = rhi::LoadOp::Load;
    attachment.rtv = rtv;
    rhi::PassBeginInfo beginInfo{};
    beginInfo.colors = {&attachment};
    beginInfo.width = outputResolution.x;
    beginInfo.height = outputResolution.y;
    commandList.BeginPass(beginInfo);
    if (data.backend == rhi::Backend::D3D12)
        ImGui_ImplDX12_RenderDrawData(const_cast<ImDrawData*>(&data.drawData),
            rhi::dx12::get_cmd_list(commandList));
#if BASICRENDERER_HAS_IMGUI_VULKAN
    else if (data.backend == rhi::Backend::Vulkan)
        ImGui_ImplVulkan_RenderDrawData(const_cast<ImDrawData*>(&data.drawData),
            rhi::vulkan::get_cmd_list(commandList));
#endif
    commandList.EndPass();
}

