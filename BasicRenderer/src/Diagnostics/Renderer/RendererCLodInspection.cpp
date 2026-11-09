#include <BasicRenderer/Renderer.h>
#include "OpenRenderGraph/Runtime.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "OpenRenderGraph/OpenRenderGraph.h"
#include "Render/Runtime/IStatisticsService.h"
#include <BasicRenderer/Extensions/BuiltinResources.h>
#include <BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h>
#include "Runtime/Frame/RendererFrameInputs.h"
#include "Scene/Objects/ObjectManager.h"
#include "VirtualGeometry/GeometryStorage/GeometryBufferStateArtifacts.h"

namespace {
	// Both resources are read inside the pass that resolves pixels, so the keys and the clusters they index are one frame's.
	constexpr const char* kInspectionAnchorPass = "EvaluateMaterialGroupsPass";

	struct PendingInspection final : Renderer::CLodInspectionRequest {
		mutable std::mutex mutex;
		Renderer::CLodInspectionFrame frame;
		std::vector<std::string> waiting;  // readbacks not yet arrived, by name
		std::function<void(Renderer::CLodInspectionFrame&&)> done;
		std::vector<std::string> required;

		bool Required(const std::string& name) const { return std::ranges::find(required, name) != required.end(); }

		// Each readback calls this once, holding the mutex; the last one hands the frame over.
		void Arrived(const std::string& name)
		{
			std::erase(waiting, name);
			if (waiting.empty()) {
				Deliver();
			}
		}

		void Deliver()
		{
			if (!done) {
				return;
			}
			frame.missingReadbacks = waiting;
			auto callback = std::move(done);
			done = nullptr;
			callback(std::move(frame));
		}

		bool FinishWithAvailable() override
		{
			std::scoped_lock lock(mutex);
			if (!done || std::ranges::any_of(waiting, [this](const std::string& name) { return Required(name); })) {
				return false;
			}
			Deliver();
			return true;
		}

		std::string Missing() const override
		{
			std::scoped_lock lock(mutex);
			std::string names;
			for (const auto& name : waiting) {
				names += (names.empty() ? "" : ", ") + name;
			}
			return names;
		}
	};
}

std::shared_ptr<Renderer::CLodInspectionRequest> Renderer::StartCLodInspectionFrame(std::function<void(CLodInspectionFrame&&)> done)
{
	if (currentRenderGraph == nullptr || !m_materialEvaluationInputs.visibleClusters || !m_context.publishedRendererState) {
		return nullptr;
	}
	auto* readback = currentRenderGraph->GetReadbackService();
	auto visibility = currentRenderGraph->RequestResourcePtr(org::ResourceIdentifier{ std::string(Builtin::PrimaryCamera::VisibilityTexture) }, true);
	if (readback == nullptr || !visibility) {
		return nullptr;
	}

	// The anchor must be a pass this graph prepares, or ORG drops the capture after 64 frames without a callback.
	std::string anchor = kInspectionAnchorPass;
	bool pathTraced = false;
	if (const auto* statistics = currentRenderGraph->GetStatisticsService()) {
		const auto& names = statistics->GetPassNames();
		if (!names.empty() && std::ranges::find(names, anchor) == names.end()) {
			const auto candidate = std::ranges::find_if(names, [](const std::string& name) { return name.find("EvaluateMaterialGroups") != std::string::npos; });
			// Path tracing replaces material evaluation and writes the surfaces itself; it has no visibility keys.
			pathTraced = candidate == names.end() && std::ranges::find(names, std::string("PathTracePass")) != names.end();
			if (pathTraced) {
				anchor = "PathTracePass";
			}
			else if (candidate == names.end()) {
				static std::once_flag logged;
				std::call_once(logged, [&] {
					std::string list;
					for (const auto& name : names) {
						list += (list.empty() ? "" : ", ") + name;
					}
					spdlog::error("CLod inspection: the graph has no '{}' pass to read after. Passes: {}", anchor, list);
				});
				return nullptr;
			}
			else {
				anchor = *candidate;
			}
		}
	}

	auto pending = std::make_shared<PendingInspection>();
	pending->frame.frame = m_totalFramesRendered;
	pending->frame.pathTraced = pathTraced;
	pending->frame.reyesPatchIndexBase = CLodReyesPatchVisibilityIndexBase(m_materialEvaluationInputs.visibleClusterCapacity);
	pending->done = std::move(done);

	const auto published = m_context.publishedRendererState;
	if (const auto objects = published->drawRecords.payload.Get<br::render::PublishedObjectBufferState>()) {
		if (const auto drawVersion = objects->FindVersion(br::render::kObjectDrawRecordVariant)) {
			pending->frame.drawRecords = drawVersion->MaterializeCpuImage();
		}
	}
	if (const auto geometry = published->geometry.selectedState.Get<br::render::PublishedGeometryBufferState>()) {
		// Catalog variants from MeshManager's geometry buffer definitions.
		constexpr std::uint64_t kPerMeshVariant = 1u, kMeshTemplateVariant = 2u, kClodOffsetsVariant = 3u, kMeshMetadataVariant = 5u,
			kGroupsVariant = 7u, kAssemblyMaterialBasesVariant = 17u, kMaterialPalettesVariant = 18u;
		for (std::size_t index = 0; index < geometry->buffers.size() && index < geometry->versions.size(); ++index) {
			if (!geometry->versions[index]) {
				continue;
			}
			const auto image = [&] { return geometry->versions[index]->MaterializeCpuImage(); };
			switch (geometry->buffers[index].catalogVariant) {
			case kPerMeshVariant: pending->frame.perMeshes = image(); break;
			case kMeshTemplateVariant: pending->frame.meshTemplates = image(); break;
			case kClodOffsetsVariant: pending->frame.clodOffsets = image(); break;
			case kMeshMetadataVariant: pending->frame.meshMetadata = image(); break;
			case kGroupsVariant: pending->frame.groups = image(); break;
			case kAssemblyMaterialBasesVariant: pending->frame.assemblyMaterialBases = image(); break;
			case kMaterialPalettesVariant: pending->frame.materialPalettes = image(); break;
			default: break;
			}
		}
	}
	if (!pending->frame.drawRecords || !pending->frame.clodOffsets || !pending->frame.meshMetadata) {
		return nullptr;
	}

	// Optional inputs: each one present is read in the same pass, so all of them describe one frame.
	auto records = currentRenderGraph->RequestResourcePtr(org::ResourceIdentifier{ std::string(Builtin::Surface::Records) }, true);
	const auto sideBand = m_materialEvaluationInputs.visibleClusterTransformIndices;
	const auto diceQueue = m_materialEvaluationInputs.reyesDiceQueue;
	if (pathTraced) {
		if (!records) {
			return nullptr;
		}
		// Records are a flat buffer with one row per pixel; the identity target gives the frame's size.
		auto identity = currentRenderGraph->RequestResourcePtr(org::ResourceIdentifier{ std::string(Builtin::Surface::Identity) }, true);
		if (!identity) {
			return nullptr;
		}
		pending->waiting = pending->required = { "surface records", "surface identity" };
		readback->RequestReadbackCapture(anchor, records.get(), org::RangeSpec{}, [pending](org::ReadbackCaptureResult&& result) {
			std::scoped_lock lock(pending->mutex);
			pending->frame.surfaceRecords = std::move(result.data);
			pending->Arrived("surface records");
		});
		readback->RequestReadbackCapture(anchor, identity.get(), org::RangeSpec{}, [pending](org::ReadbackCaptureResult&& result) {
			std::scoped_lock lock(pending->mutex);
			pending->frame.width = result.width;
			pending->frame.height = result.height;
			pending->Arrived("surface identity");
		});
		return pending;
	}
	pending->waiting = pending->required = { "visibility", "visible clusters" };
	if (sideBand) pending->waiting.push_back("side band");
	if (diceQueue) pending->waiting.push_back("reyes dice queue");
	if (records) pending->waiting.push_back("surface records");

	readback->RequestReadbackCapture(anchor, visibility.get(), org::RangeSpec{},
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
			pending->Arrived("visibility");
		});
	const auto readBuffer = [&, anchor](org::Resource* resource, std::vector<std::byte> Renderer::CLodInspectionFrame::*target, std::string name) {
		readback->RequestReadbackCapture(anchor, resource, org::RangeSpec{},
			[pending, target, name = std::move(name)](org::ReadbackCaptureResult&& result) {
				std::scoped_lock lock(pending->mutex);
				pending->frame.*target = std::move(result.data);
				pending->Arrived(name);
			});
	};
	readBuffer(m_materialEvaluationInputs.visibleClusters.get(), &CLodInspectionFrame::visibleClusters, "visible clusters");
	if (sideBand) {
		readBuffer(sideBand.get(), &CLodInspectionFrame::visibleClusterSideBand, "side band");
	}
	if (diceQueue) {
		readBuffer(diceQueue.get(), &CLodInspectionFrame::reyesDiceQueue, "reyes dice queue");
	}
	if (records) {
		readBuffer(records.get(), &CLodInspectionFrame::surfaceRecords, "surface records");
	}
	return pending;
}
