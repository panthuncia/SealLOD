#include <BasicRenderer/Renderer.h>
#include "OpenRenderGraph/Runtime.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "OpenRenderGraph/OpenRenderGraph.h"
#include "Runtime/Frame/RendererFrameInputs.h"
#include "Scene/Objects/ObjectManager.h"
#include "VirtualGeometry/GeometryStorage/GeometryBufferStateArtifacts.h"

namespace {
	// Both resources are read inside the pass that resolves pixels, so the keys and the clusters they index are one frame's.
	constexpr const char* kInspectionAnchorPass = "EvaluateMaterialGroupsPass";

	struct PendingInspection {
		std::mutex mutex;
		Renderer::CLodInspectionFrame frame;
		bool hasVisibility = false;
		bool hasClusters = false;
		std::function<void(Renderer::CLodInspectionFrame&&)> done;

		void TryFinish()
		{
			if (hasVisibility && hasClusters && done) {
				auto callback = std::move(done);
				done = nullptr;
				callback(std::move(frame));
			}
		}
	};
}

bool Renderer::RequestCLodInspectionFrame(std::function<void(CLodInspectionFrame&&)> done)
{
	if (currentRenderGraph == nullptr || !m_materialEvaluationInputs.visibleClusters || !m_context.publishedRendererState) {
		return false;
	}
	auto* readback = currentRenderGraph->GetReadbackService();
	auto visibility = currentRenderGraph->RequestResourcePtr(org::ResourceIdentifier{ std::string(Builtin::PrimaryCamera::VisibilityTexture) }, true);
	if (readback == nullptr || !visibility) {
		return false;
	}

	auto pending = std::make_shared<PendingInspection>();
	pending->frame.frame = m_totalFramesRendered;
	pending->done = std::move(done);

	const auto published = m_context.publishedRendererState;
	if (const auto objects = published->drawRecords.payload.Get<br::render::PublishedObjectBufferState>()) {
		if (const auto drawVersion = objects->FindVersion(br::render::kObjectDrawRecordVariant)) {
			pending->frame.drawRecords = drawVersion->MaterializeCpuImage();
		}
	}
	if (const auto geometry = published->geometry.selectedState.Get<br::render::PublishedGeometryBufferState>()) {
		// Catalog variants from MeshManager's geometry buffer definitions.
		constexpr std::uint64_t kClodOffsetsVariant = 3u, kMeshMetadataVariant = 5u;
		for (std::size_t index = 0; index < geometry->buffers.size() && index < geometry->versions.size(); ++index) {
			const auto variant = geometry->buffers[index].catalogVariant;
			if (variant == kClodOffsetsVariant) {
				pending->frame.clodOffsets = geometry->versions[index]->MaterializeCpuImage();
			}
			else if (variant == kMeshMetadataVariant) {
				pending->frame.meshMetadata = geometry->versions[index]->MaterializeCpuImage();
			}
		}
	}
	if (!pending->frame.drawRecords || !pending->frame.clodOffsets || !pending->frame.meshMetadata) {
		return false;
	}

	readback->RequestReadbackCapture(kInspectionAnchorPass, visibility.get(), org::RangeSpec{},
		[pending](org::ReadbackCaptureResult&& result) {
			std::scoped_lock lock(pending->mutex);
			if (result.format == rhi::Format::R32G32_UInt && !result.layouts.empty() && result.width != 0u && result.height != 0u) {
				const auto& layout = result.layouts.front();
				const auto rowPitch = static_cast<std::size_t>(layout.rowPitch);
				const auto offset = static_cast<std::size_t>(layout.offset);
				const std::size_t rowBytes = static_cast<std::size_t>(result.width) * sizeof(std::uint64_t);
				if (offset + rowPitch * (result.height - 1u) + rowBytes <= result.data.size()) {
					pending->frame.width = result.width;
					pending->frame.height = result.height;
					pending->frame.visibility.resize(static_cast<std::size_t>(result.width) * result.height);
					for (std::uint32_t row = 0; row < result.height; ++row) {
						std::memcpy(pending->frame.visibility.data() + static_cast<std::size_t>(row) * result.width, result.data.data() + offset + rowPitch * row, rowBytes);
					}
				}
			}
			pending->hasVisibility = true;
			pending->TryFinish();
		});
	readback->RequestReadbackCapture(kInspectionAnchorPass, m_materialEvaluationInputs.visibleClusters.get(), org::RangeSpec{},
		[pending](org::ReadbackCaptureResult&& result) {
			std::scoped_lock lock(pending->mutex);
			pending->frame.visibleClusters = std::move(result.data);
			pending->hasClusters = true;
			pending->TryFinish();
		});
	return true;
}
