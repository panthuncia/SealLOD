#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Render/PassBuilders.h"
#include "BuiltinResources.h"

struct PresentationCopyBindings {
	org::ResourceBindingToken source{}, destination{};
};

struct PresentationCopyFrameData {
	org::PreparedResourceReference source{}, destination{};
};

class PresentationCopyPass : public org::TypedRenderGraphPass<
	PresentationCopyPass, PresentationCopyFrameData, PresentationCopyBindings> {
public:
	PresentationCopyBindings Declare(org::PassBuilder& builder) {
		return {
			builder.BindCopySource(org::ResourceIdentifier{Builtin::PresentationColor}),
			builder.BindCopyDestination(org::ResourceIdentifier{Builtin::Backbuffer})};
	}

	PresentationCopyFrameData Prepare(const PresentationCopyBindings& bindings,
		const org::PassPrepareContext& preparation) const {
		return {preparation.CaptureResource(bindings.source),
			preparation.CaptureResource(bindings.destination)};
	}

	static void Record(const PresentationCopyBindings&,
		const PresentationCopyFrameData& data, org::PassRecordContext& recording) {
		rhi::TextureCopyRegion source{};
		source.texture = recording.Resolve(data.source).GetHandle();
		rhi::TextureCopyRegion destination{};
		destination.texture = recording.Resolve(data.destination).GetHandle();
		recording.Commands().CopyTextureRegion(destination, source);
	}
};

class PresentPass : public org::TypedRenderGraphPass<PresentPass> {
public:
	void Declare(org::PassBuilder& builder) {
		builder.WithPresent(Builtin::Backbuffer);
	}
	static void Record(org::PassRecordContext&) {}
};

// Terminal scene-graph declaration. It freezes the slot-owned presentation
// image in CopySource state; swapchain acquisition, destination transitions,
// copying, and Present belong to the FIFO presentation tail.
class PresentationReadyPass : public org::TypedRenderGraphPass<PresentationReadyPass> {
public:
	void Declare(org::PassBuilder& builder) {
		builder.BindCopySource(org::ResourceIdentifier{Builtin::PresentationColor});
	}
	static void Record(org::PassRecordContext&) {}
};
