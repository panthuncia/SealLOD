#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Diagnostics/Menu/Menu.h"

struct MenuFrameData {
	std::shared_ptr<const PreparedImGuiDrawData> drawData;
	org::PreparedDescriptorReference target{};
	DirectX::XMUINT2 outputResolution{};
};

struct MenuBindings { org::DeclaredViewToken target; };

class MenuRenderPass final : public org::TypedRenderGraphPass<MenuRenderPass, MenuFrameData, MenuBindings> {
public:
	MenuBindings Declare(org::PassBuilder& builder) {
        return {builder.RenderTarget(org::ResourceIdentifier{Builtin::PresentationColor}).View()};
	}

	MenuFrameData Prepare(const MenuBindings& bindings, const org::PassPrepareContext& preparation) const {
		const auto* context = preparation.preparationData
			? preparation.preparationData->Get<RenderContext>() : nullptr;
		if (!context) return {};
		return {
			.drawData = context->uiDrawData,
            .target = preparation.Capture(bindings.target),
			.outputResolution = context->outputResolution,
		};
	}

	static void Record(const MenuBindings&, const MenuFrameData& data, org::PassRecordContext& recording) {
		if (data.drawData) Menu::RecordPreparedDrawData(
			*data.drawData, recording.Commands(), recording.Resolve(data.target), data.outputResolution);
	}
};
