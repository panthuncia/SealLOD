#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BuiltinResources.h"

struct PresentationCopyBindings {
	org::ResourceBindingToken source{}, destination{};
};

class PresentationCopyPass : public org::TypedRenderGraphPass<
	PresentationCopyPass, org::EmptyPassFrameData, PresentationCopyBindings> {
public:
	PresentationCopyBindings Declare(org::PassBuilder& builder) {
		return {
			builder.CopySource(org::ResourceIdentifier{Builtin::PresentationColor}),
			builder.CopyDestination(org::ResourceIdentifier{Builtin::Backbuffer})};
	}

	static void Record(const PresentationCopyBindings& bindings, org::PassRecordContext& recording) {
		rhi::TextureCopyRegion source{};
		source.texture = recording.Resolve(bindings.source).GetHandle();
		rhi::TextureCopyRegion destination{};
		destination.texture = recording.Resolve(bindings.destination).GetHandle();
		recording.Commands().CopyTextureRegion(destination, source);
	}
};

class PresentPass : public org::TypedRenderGraphPass<PresentPass> {
public:
	void Declare(org::PassBuilder& builder) {
		builder.Present(Builtin::Backbuffer);
	}
	static void Record(org::PassRecordContext&) {}
};

// Terminal scene-graph declaration. It freezes the slot-owned presentation
// image in CopySource state; swapchain acquisition, destination transitions,
// copying, and Present belong to the FIFO presentation tail.
class PresentationReadyPass : public org::TypedRenderGraphPass<PresentationReadyPass> {
public:
	void Declare(org::PassBuilder& builder) {
		builder.CopySource(org::ResourceIdentifier{Builtin::PresentationColor});
	}
	static void Record(org::PassRecordContext&) {}
};
