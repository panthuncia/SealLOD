#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Diagnostics/Menu/Menu.h"

struct MenuFrameData {
	std::shared_ptr<const PreparedImGuiDrawData> drawData;
	DirectX::XMUINT2 outputResolution{};
};

struct MenuBindings { org::DeclaredViewToken target; };

class MenuRenderPass final : public org::TypedRenderGraphPass<MenuRenderPass, MenuFrameData, MenuBindings> {
public:
	MenuBindings Declare(org::PassBuilder& builder) {
        return {builder.RenderTarget(org::ResourceIdentifier{Builtin::PresentationColor}).View()};
	}

	MenuFrameData Prepare(const MenuBindings&, const org::PassPrepareContext& preparation) const {
		const auto* context = preparation.preparationData
			? preparation.preparationData->Get<RenderContext>() : nullptr;
		if (!context) return {};
		return {
			.drawData = context->uiDrawData,
			.outputResolution = context->outputResolution,
		};
	}

	static void Record(const MenuBindings& bindings, const MenuFrameData& data, org::PassRecordContext& recording) {
		if (!data.drawData) return;
		Menu::RecordPreparedDrawData(*data.drawData, recording.Commands(), recording.View(bindings.target), data.outputResolution);
		recording.RestoreDescriptorHeaps();
	}
};
