#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Render/RenderContext.h"
#include "Menu/Menu.h"

struct MenuFrameData {
	std::shared_ptr<const PreparedImGuiDrawData> drawData;
	org::PreparedDescriptorReference target{};
	DirectX::XMUINT2 outputResolution{};
};

struct MenuBindings { org::ResourceBindingToken target; };

class MenuRenderPass final : public org::TypedRenderGraphPass<MenuRenderPass, MenuFrameData, MenuBindings> {
public:
	MenuBindings Declare(org::PassBuilder& builder) {
		return {builder.BindRenderTarget(org::ResourceIdentifier{Builtin::PresentationColor})};
	}

	MenuFrameData Prepare(const MenuBindings& bindings, const org::PassPrepareContext& preparation) const {
		const auto* context = preparation.preparationData
			? preparation.preparationData->Get<RenderContext>() : nullptr;
		if (!context) return {};
		return {
			.drawData = context->uiDrawData,
			.target = preparation.CaptureView(bindings.target,
				{org::BindlessViewKind::RenderTarget}),
			.outputResolution = context->outputResolution,
		};
	}

	static void Record(const MenuBindings&, const MenuFrameData& data, org::PassRecordContext& recording) {
		if (data.drawData) Menu::RecordPreparedDrawData(
			*data.drawData, recording.Commands(), recording.Resolve(data.target), data.outputResolution);
	}
};
