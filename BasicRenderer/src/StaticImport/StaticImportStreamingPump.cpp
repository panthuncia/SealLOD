// Method definitions for StaticImportStreamingPump.

#include <BasicRenderer/Streaming/GeometryRequests.h>
#include <BasicRenderer/Streaming/MaterialRequests.h>
#include <BasicRenderer/Streaming/ObjectRequests.h>
#include <BasicRenderer/Streaming/StaticObjectRequestService.h>
#include <BasicRenderer/Streaming/RendererStateRequestService.h>
#include <BasicRenderer/Pipeline/MeshDrawWorkload.h>
#include <BasicRenderer/Streaming/PublishedRendererState.h>
#include <BasicRenderer/Streaming/SceneAssetRequestService.h>
#include <BasicRenderer/Streaming/StaticGeometryRequestService.h>
#include <BasicRenderer/Streaming/StaticMaterialRequestService.h>
#include <BasicRenderer/Streaming/StaticWorkloadRequestService.h>
#include <BasicRenderer/Streaming/WorkloadRequests.h>
#include "StaticImport/StaticImportStreamingPump.h"
// ComposeDirectStaticRasterFlags.
#include <BasicRenderer/StaticImport/Advanced/StaticImportPreparation.h>

namespace br::staticimport
{
	StaticImportStreamingPump::StaticImportStreamingPump(const StaticStreamingContext& ctx, StaticImportCoordinator& coordinator, StaticGroupLifecycleStore& lifecycle, StaticImportCapacityBroker& capacity, Hooks hooks)
	{
		_shared = &ctx.shared;
		_bus = &ctx.bus;
		_importState = &ctx.importState;
		_drains = &ctx.drains;
		_counters = &ctx.counters;
		_settings = &ctx.settings;
		_extensions = ctx.extensions;
		_coordinator = &coordinator;
		_lifecycle = &lifecycle;
		_capacity = &capacity;
		_hooks = std::move(hooks);
	}

	StaticStreamBudget StaticImportStreamingPump::MakeStaticStreamBudget() const
	{
		StaticStreamBudget budget;
		budget.totalUs = _settings->frameBudgetUs;
		const auto burstBudget = (std::max)(budget.totalUs,
			static_cast<std::uint64_t>(_settings->burstFrameBudgetUs));
		// The coordinator owns ready groups after preparation.  Looking only at the
		// hand-off queues makes the adaptive budget fall back to 2 ms precisely when
		// a large scene has accumulated its biggest publish backlog.
		const auto serviceStats = GetStatsSnapshot();
		const auto authoritativeBacklog = (std::max)(
			StaticPreparationQueueDepth(),
			static_cast<std::uint64_t>(serviceStats.pendingGroups));
		const bool aboveHighWater = authoritativeBacklog >=
			_settings->highWaterDepth;
		const bool hasFrameHeadroom = _counters->previousHostFrameTickUs == 0 ||
			_counters->previousHostFrameTickUs <= _settings->burstHeadroomUs;
		if (aboveHighWater && hasFrameHeadroom) budget.totalUs = burstBudget;
		budget.removeUs = static_cast<std::uint64_t>(
			static_cast<double>(budget.totalUs) * static_cast<double>(_settings->removeBudgetFraction));
		basic_telemetry::SetGauge("SARP.StaticImport.AdaptiveBudgetUs",
			static_cast<std::int64_t>(budget.totalUs));
		basic_telemetry::SetGauge("SARP.StaticImport.AdaptiveBudgetHighWater",
			aboveHighWater ? 1 : 0);
		basic_telemetry::SetGauge("SARP.StaticImport.AdaptiveBudgetBacklog",
			static_cast<std::int64_t>((std::min)(
				authoritativeBacklog,
				static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()))));
		return budget;
	}

	StaticDesiredCellArea StaticImportStreamingPump::BuildDesiredStaticCells(const br::staticimport::StaticStreamTarget& target) const
	{
		StaticDesiredCellArea cells;
		if (target.interiorCellFormID != 0) {
			cells.kind = StaticDesiredCellAreaKind::Interior;
			cells.interior = StaticCellKey{ 0, target.interiorCellFormID, 0, 0 };
			return cells;
		}
		if (target.worldFormID == 0) {
			return cells;
		}
		cells.kind = StaticDesiredCellAreaKind::ExteriorRect;
		cells.exterior = MakeStaticCellRect(
			target.worldFormID,
			target.cellX,
			target.cellY,
			_settings->cellRadius);
		for (const auto* extension : _extensions) {
			extension->ExpandDesiredCellRect(target, cells.exterior);
		}
		return cells;
	}

	void StaticImportStreamingPump::SetExtensions(std::span<br::staticimport::IStaticImportExtension* const> extensions)
	{
		_extensions = extensions;
	}

	void StaticImportStreamingPump::ScheduleStaticTemplatePublicationRetry()
	{
		if (_shared->staticImportServiceStop.load(std::memory_order_acquire)) return;
		bool expected = false;
		if (!_importState->staticTemplatePublicationRetryScheduled.compare_exchange_strong(
				expected, true, std::memory_order_acq_rel)) return;
		const auto submitted = TaskSchedulerManager::GetInstance().ScheduleAfter(
			_shared->staticStreamingScope, std::chrono::milliseconds(50), TaskLane::Streaming,
			TaskDomain::StaticImportControl, "SARP::StaticImport::TemplateDependencyWake",
			[this](const br::TaskContext& context) {
				_importState->staticTemplatePublicationRetryScheduled.store(false, std::memory_order_release);
				if (!context.StopRequested() &&
					!_shared->staticImportServiceStop.load(std::memory_order_acquire)) {
					_shared->staticImportWorkScheduler.Signal(StaticImportWorkSource::TemplatePublication);
					_drains->ScheduleWorkerDrains();
				}
			});
		if (!submitted) {
			_importState->staticTemplatePublicationRetryScheduled.store(false, std::memory_order_release);
		}
	}

	void StaticImportStreamingPump::UpdateStaticStreaming(const br::staticimport::StaticStreamTarget& target)
	{
		const auto updateBegin = std::chrono::steady_clock::now();
		if (!_settings->streamingEnabled || !target.valid || !_hooks.ensureStaticWorldSnapshot()) {
			for (auto* extension : _extensions) {
				extension->OnStreamTargetCleared();
			}
			UpdateCameraTarget(target);
			_counters->staticStreamUpdateUs += ElapsedUs(updateBegin);
			return;
		}
		for (auto* extension : _extensions) {
			extension->OnStreamTargetUpdated(target);
		}
		UpdateStaticStreamingDirectImportService(target, updateBegin);
	}

	void StaticImportStreamingPump::DrainTemplateRequests(const StaticStreamBudget& budget)
	{
		// A template batch is an exact dependency closure. This acceptance path is
		// now an off-thread serialized lane, so use the configured bounded batch
		// instead of the former 32-row main-thread latency cap. Larger batches also
		// avoid serializing streaming behind dozens of MaterialUsageBatch barriers.
		const auto configuredTemplateRequests = static_cast<std::size_t>(
			_settings->maxTemplateRequestsPerFrame);
		const auto maxTemplateRequestsPerFrame = configuredTemplateRequests == 0
			? std::size_t{ 256 }
			: (std::min)(configuredTemplateRequests, std::size_t{ 256 });
		std::vector<StaticTemplateRequest> requests;
		std::shared_ptr<const br::render::PublishedMaterialUsageBatch> materialAdmission;
		if (!_importState->pendingStaticTemplateMaterialRequests.empty()) {
			auto* graphRequests = _shared->hasSceneIngestionServices
				? _shared->sceneIngestionServices.execution.stateRequests : nullptr;
			if (!graphRequests || !_importState->pendingStaticTemplateMaterialVersion) return;
			const auto completion = graphRequests->Snapshot(
				_importState->pendingStaticTemplateMaterialVersion.version);
			if (completion.readiness == br::render::ArtifactReadiness::Failed ||
				completion.readiness == br::render::ArtifactReadiness::Cancelled) {
				spdlog::warn("Static template material admission will be recaptured for artifact {}:{}",
					completion.key.primaryID, completion.revision);
				// Texture bindings may advance between CaptureMaterialUsage and graph
				// dependency selection. That invalidates this immutable material row, not
				// the mesh/material asset or its StaticTemplate identity. Treating the
				// batch failure as terminal poisoned every dependent StaticVariant and
				// left otherwise valid groups permanently counted as pending. Releasing
				// the version rolls the reservation back; a fresh capture below observes
				// the successor texture descriptors.
				for (auto& request : _importState->pendingStaticTemplateMaterialRequests) {
					(void)_drains->PushStreaming(
						_bus->staticImportTemplateRequestOutput, std::move(request));
				}
				_importState->pendingStaticTemplateMaterialRequests.clear();
				_importState->pendingStaticTemplateMaterialVersion = {};
				_importState->pendingStaticTemplateMaterialAwaiter.Reset();
				basic_telemetry::AddCounter(
					"SARP.StaticImportService.MaterialAdmissionRecaptures");
				_importState->staticTemplatePublicationNeedsBackoff.store(true, std::memory_order_release);
				ScheduleStaticTemplatePublicationRetry();
				return;
			}
			materialAdmission = completion.payload
				.Get<br::render::PublishedMaterialUsageBatch>();
			if (!materialAdmission) {
				const auto now = std::chrono::steady_clock::now();
				if (_importState->lastStaticTemplateMaterialDiagnostic ==
						std::chrono::steady_clock::time_point{} ||
					now - _importState->lastStaticTemplateMaterialDiagnostic >= std::chrono::seconds(5)) {
					_importState->lastStaticTemplateMaterialDiagnostic = now;
					const auto diagnostic = graphRequests->Diagnose(
						_importState->pendingStaticTemplateMaterialVersion.version.address);
					spdlog::error(
						"Static template material batch is stalled: version={}:{}:{} readiness={} requests={} blockers={} chain='{}'",
						static_cast<unsigned>(_importState->pendingStaticTemplateMaterialVersion.version.address.kind),
						_importState->pendingStaticTemplateMaterialVersion.version.generation,
						_importState->pendingStaticTemplateMaterialVersion.version.revision,
						static_cast<unsigned>(completion.readiness),
						_importState->pendingStaticTemplateMaterialRequests.size(),
						diagnostic.blockers.size(), diagnostic.blockerChain);
				}
				ScheduleStaticTemplatePublicationRetry();
				return;
			}
			requests = std::move(_importState->pendingStaticTemplateMaterialRequests);
			_importState->pendingStaticTemplateMaterialVersion = {};
			_importState->pendingStaticTemplateMaterialAwaiter.Reset();
		} else {
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainTemplateRequests::DrainQueue");
			const auto drainLimit = maxTemplateRequestsPerFrame;
			_bus->staticImportTemplateRequestOutput.Drain(requests, drainLimit);
			BT_PLOT("SARP.StaticImportService.TemplateRequestsDrained", static_cast<int64_t>(requests.size()));
		}
		if (requests.empty()) {
			return;
		}
		if (!DirectStaticImportAvailable()) {
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainTemplateRequests::RequeueUnavailable");
			for (auto& request : requests) {
				(void)_drains->PushStreaming(_bus->staticImportTemplateRequestOutput, std::move(request));
			}
			return;
		}

		BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainTemplateRequests");
		// Template publication can grow GPU-backed DynamicBuffers. Keep it on
		// the renderer update path, before render graph compile/execute.
		std::vector<StaticTemplateRequest> leftovers;
		if ((maxTemplateRequestsPerFrame != 0 && requests.size() > maxTemplateRequestsPerFrame) || budget.TotalExhausted()) {
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainTemplateRequests::SplitBudgetLeftovers");
			const auto firstLeftover = budget.TotalExhausted()
				? requests.begin()
				: requests.begin() + static_cast<std::ptrdiff_t>(maxTemplateRequestsPerFrame);
			leftovers.insert(
				leftovers.end(),
				std::make_move_iterator(firstLeftover),
				std::make_move_iterator(requests.end()));
			requests.erase(firstLeftover, requests.end());
		}
		BT_PLOT("SARP.StaticImportService.TemplateRequestsProcessing", static_cast<int64_t>(requests.size()));
		BT_PLOT("SARP.StaticImportService.TemplateRequestLeftovers", static_cast<int64_t>(leftovers.size()));
		if (requests.empty()) {
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainTemplateRequests::RequeueBudgetLeftovers");
			for (auto& request : leftovers) {
				(void)_drains->PushStreaming(_bus->staticImportTemplateRequestOutput, std::move(request));
			}
			return;
		}
		const auto templateDrainBegin = std::chrono::steady_clock::now();
		auto* meshManager = _shared->sceneIngestionServices.meshes;
		auto* materialManager = _shared->sceneIngestionServices.materials;
		const bool textureProductionAvailable = _shared->sceneIngestionServices.assetRequests &&
			_shared->sceneIngestionServices.assetRequests->CanUploadTextures();
		{
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainTemplateRequests::PublicationWork");

			std::vector<std::shared_ptr<Mesh>> meshesToRegister;
			std::vector<DirectStaticTemplateKey> keys;
			std::vector<std::shared_ptr<Mesh>> meshes;
			std::vector<std::shared_ptr<Material>> materials;
			std::vector<StaticTemplateRequest> templateSourceRequests;
			std::vector<br::render::StaticMeshTemplateRequest> templateRequests;
			std::vector<StaticTemplateRequest> retryRequests;
			std::unordered_set<DirectStaticTemplateKey, DirectStaticTemplateKeyHash> queuedKeys;
			std::unordered_set<Mesh*> queuedMeshesToRegister;
			StaticTemplateRefsPublished publishedRefs;
			meshesToRegister.reserve(requests.size());
			keys.reserve(requests.size());
			meshes.reserve(requests.size());
			materials.reserve(requests.size());
			templateSourceRequests.reserve(requests.size());
			templateRequests.reserve(requests.size());
			retryRequests.reserve(requests.size());
			queuedKeys.reserve(requests.size());
			queuedMeshesToRegister.reserve(requests.size());

			{
				BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainTemplateRequests::ClassifyRequests");
				std::size_t invalidRequests = 0;
				std::size_t duplicateKeys = 0;
				for (const auto& request : requests) {
					if (!request.mesh || !request.material) {
						publishedRefs.failedKeys.push_back(request.key);
						++invalidRequests;
						continue;
					}
					if (!queuedKeys.insert(request.key).second) {
						++duplicateKeys;
						continue;
					}
					if (!request.mesh->GetPerMeshBufferView() && queuedMeshesToRegister.insert(request.mesh.get()).second) {
						meshesToRegister.push_back(request.mesh);
					}
				}
				BT_PLOT("SARP.StaticImportService.TemplateInvalidRequests", static_cast<int64_t>(invalidRequests));
				BT_PLOT("SARP.StaticImportService.TemplateDuplicateKeys", static_cast<int64_t>(duplicateKeys));
				BT_PLOT("SARP.StaticImportService.TemplateMeshesToRegister", static_cast<int64_t>(meshesToRegister.size()));
			}
			if (!meshesToRegister.empty()) {
				BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainTemplateRequests::AddMeshesBulk");
				BT_ZONE_VALUE(static_cast<int64_t>(meshesToRegister.size()));
				meshManager->AddMeshesBulk(meshesToRegister, false);
			}

			{
				BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainTemplateRequests::PrepareTemplateRows");
				std::size_t missingMeshBuffer = 0;
				std::size_t missingMaterialHeightTexture = 0;
				std::unordered_map<Material*, std::uint32_t> materialUsageCounts;
				std::unordered_map<Material*, std::shared_ptr<Material>> materialOwners;
				std::unordered_map<std::uint32_t, Material*> materialsByID;
				std::unordered_map<Material*, std::uint32_t> materialDataIndices;
				materialUsageCounts.reserve(requests.size());
				materialOwners.reserve(requests.size());
				materialsByID.reserve(requests.size());
				materialDataIndices.reserve(requests.size());
				{
					BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainTemplateRequests::PrepareTemplateRows::BatchMaterialUsage");
					for (const auto& request : requests) {
						if (request.mesh && request.material &&
							request.mesh->GetPerMeshBufferView()) {
							++materialUsageCounts[request.material.get()];
							materialOwners.try_emplace(request.material.get(), request.material);
							materialsByID.try_emplace(request.material->GetMaterialID(), request.material.get());
						}
					}
					if (!materialAdmission && !materialUsageCounts.empty()) {
						auto input = std::make_shared<br::render::MaterialUsageBatchBuildInput>();
						input->refreshTextureBindings = textureProductionAvailable;
						std::vector<br::render::MaterialUsageCapture> captures;
						captures.reserve(materialUsageCounts.size());
						std::vector<std::pair<std::uint32_t, std::uint32_t>> identity;
						identity.reserve(materialUsageCounts.size());
						for (const auto& [material, usageCount] : materialUsageCounts) {
							const auto owner = materialOwners.find(material);
							if (owner == materialOwners.end()) continue;
							materialManager->RegisterMaterialSource(owner->second);
							auto capture = materialManager->CaptureMaterialUsage(
								*owner->second, usageCount, input->refreshTextureBindings);
							input->entries.push_back(capture.entry);
							captures.push_back(std::move(capture));
							identity.emplace_back(material->GetMaterialID(), usageCount);
						}
						input->reservation = materialManager->ReserveMaterialUsage(captures);
						std::vector<br::render::ArtifactRequirement> materialRequirements;
						std::unordered_set<br::render::ArtifactAddress,
							br::render::ArtifactAddress::Hasher> selectedBindings;
						for (const auto& entry : input->entries) {
							for (const auto& binding : entry.textureBindings) {
								const br::render::ArtifactAddress address{
									br::render::ArtifactKind::TextureBinding,
									binding.streamingTextureID, 0 };
								if (!selectedBindings.insert(address).second) continue;
								// CaptureMaterialUsage observes a texture revision, but texture
								// refresh can publish a successor before this batch reaches the
								// graph.  An unleased ExactSnapshot then points at a reclaimed
								// version forever.  The material row uses stable descriptor slots,
								// so admit the newest binding at or beyond the captured revision;
								// the producer still verifies those descriptor identities.
								materialRequirements.push_back(br::render::LatestAtLeast(
									address, binding.bindingRevision,
									br::render::ArtifactReadiness::UploadSubmitted));
							}
						}
						std::ranges::sort(identity);
						std::uint64_t fingerprint = 1469598103934665603ull;
						for (const auto& [materialID, usageCount] : identity) {
							fingerprint ^= materialID + 0x9e3779b97f4a7c15ull +
								(fingerprint << 6u) + (fingerprint >> 2u);
							fingerprint ^= usageCount + 0x9e3779b97f4a7c15ull +
								(fingerprint << 6u) + (fingerprint >> 2u);
						}
						for (const auto& entry : input->entries) {
							for (const auto& binding : entry.textureBindings) {
								fingerprint ^= binding.streamingTextureID + 0x9e3779b97f4a7c15ull +
									(fingerprint << 6u) + (fingerprint >> 2u);
								fingerprint ^= binding.bindingRevision + 0x9e3779b97f4a7c15ull +
									(fingerprint << 6u) + (fingerprint >> 2u);
							}
						}
						input->sourceFingerprint = fingerprint;
						const br::render::ArtifactKey artifact{
							br::render::ArtifactKind::MaterialUsageBatch,
							++_importState->nextStaticTemplateMaterialBatchID,
							0 };
						if (auto* graphRequests = _shared->sceneIngestionServices.execution.stateRequests; graphRequests) {
							auto requestResult = graphRequests->Request(
								artifact, 1, std::move(materialRequirements),
								br::render::ArtifactPayload::Make<br::render::MaterialUsageBatchBuildInput>(
									std::move(input)),
								fingerprint == 0 ? 1u : fingerprint);
							if (requestResult) {
								// Keep the graph lease alive until the next-frame acceptance
								// poll consumes the payload. Retaining only VersionID allowed
								// reclamation to turn the poll into a permanent Missing state.
								_importState->pendingStaticTemplateMaterialVersion = requestResult.Handle();
								_importState->pendingStaticTemplateMaterialAwaiter = graphRequests->AwaitExact(
									// Acceptance is applied before a Ready completion is exposed at
									// CpuReady. MaterialUsageBatch is a non-root selector and therefore
									// is not independently promoted to Published.
									requestResult.Handle(), br::render::ArtifactReadiness::CpuReady,
									TaskLane::Streaming, TaskDomain::StaticImport,
									[handoff = std::weak_ptr(_importState->staticImportCallbackHandoff)](
										const br::render::ArtifactSnapshot&) {
										if (const auto wake = handoff.lock()) {
											wake->Invoke(StaticImportWorkSource::TemplatePublication);
										}
									},
									// The acceptance poll reads the payload and
									// treats a version without one as not yet
									// ready, so the wake is what it needs either
									// way; without it the poll never runs again.
									[handoff = std::weak_ptr(_importState->staticImportCallbackHandoff)](
										const br::render::ArtifactTermination& termination) {
										basic_telemetry::AddCounter(
											"SARP.StaticImport.MaterialUsageBatchTerminated");
										spdlog::warn("StaticImport: material usage batch terminated: {}",
											termination.error);
										if (const auto wake = handoff.lock()) {
											wake->Invoke(StaticImportWorkSource::TemplatePublication);
										}
									});
								_importState->pendingStaticTemplateMaterialRequests = std::move(requests);
								for (auto& request : leftovers) {
									(void)_drains->PushStreaming(
										_bus->staticImportTemplateRequestOutput, std::move(request));
								}
								return;
							}
						}
					}
					if (materialAdmission) {
						for (const auto& [materialID, slot] : materialAdmission->materialSlots) {
							const auto material = materialsByID.find(materialID);
							if (material != materialsByID.end())
								materialDataIndices.emplace(material->second, slot);
						}
					}
				}
				for (const auto& request : requests) {
				if (!request.mesh || !request.material) {
						continue;
					}
					if (!request.mesh->GetPerMeshBufferView()) {
						++missingMeshBuffer;
						retryRequests.push_back(request);
						continue;
					}
					{
						uint32_t materialDataIndex = 0;
						BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainTemplateRequests::PrepareTemplateRows::MaterialUsage");
						{
							BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainTemplateRequests::PrepareTemplateRows::MaterialUsage::LookupBatchedIndex");
							const auto materialIndexIt = materialDataIndices.find(request.material.get());
							if (materialIndexIt == materialDataIndices.end()) {
								retryRequests.push_back(request);
								continue;
							}
							materialDataIndex = materialIndexIt->second;
						}
						if (!IsMaterialReadyForTemplatePublication(*request.material)) {
							BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainTemplateRequests::PrepareTemplateRows::MaterialUsage::HeightDependencyPending");
							materialManager->DecrementMaterialUsageCount(*request.material);
							++missingMaterialHeightTexture;
							retryRequests.push_back(request);
							continue;
						}
						{
							BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainTemplateRequests::PrepareTemplateRows::MaterialUsage::SetIndex");
							request.mesh->SetMaterialDataIndex(materialDataIndex);
							if (request.material->IsObjectReyesAtlasHeightMaterial()) {
								static std::atomic<std::uint32_t> loggedAtlasAssignments{ 0u };
								if (loggedAtlasAssignments.fetch_add(1u, std::memory_order_relaxed) < 64u) {
									const auto desc = request.material->ToCacheDescription();
									spdlog::info(
										"SARP height atlas streaming: assigned atlas material mesh={} material={} slot={} base='{}' height='{}' heightIndex={} objectSurfaceMode={}",
										request.mesh->GetGlobalID(),
										request.material->GetMaterialID(),
										materialDataIndex,
										desc.baseColor.sourcePath,
										desc.heightMap.sourcePath,
										request.material->GetData().heightMapIndex,
										request.material->GetData().objectSurfaceSamplingMode);
								}
							}
							const auto materialEvalVariants =
								ComposeMaterialEvalVariantSet(*request.mesh, *request.material);
							const auto materialEvalCompileFlagsID =
								materialManager->AcquireCompileFlagsSlot(materialEvalVariants.regular);
							request.mesh->SetMaterialEvalCompileFlagsID(materialEvalCompileFlagsID);
							const auto materialReyesEvalCompileFlagsID =
								materialEvalVariants.hasDistinctReyes
									? materialManager->AcquireCompileFlagsSlot(materialEvalVariants.reyes)
									: materialEvalCompileFlagsID;
							request.mesh->SetMaterialReyesEvalCompileFlagsID(materialReyesEvalCompileFlagsID);
						}
					}
					{
						BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainTemplateRequests::PrepareTemplateRows::RasterBucket");
						const auto rasterBucketIndex = materialManager->AcquireRasterBucket(ComposeDirectStaticRasterFlags(*request.mesh, *request.material));
						request.mesh->SetRasterBucketIndex(rasterBucketIndex);
					}
					keys.push_back(request.key);
					meshes.push_back(request.mesh);
					materials.push_back(request.material);
					templateSourceRequests.push_back(request);
					templateRequests.push_back({ request.mesh, request.material });
				}
				BT_PLOT("SARP.StaticImportService.TemplateMissingMeshBuffer", static_cast<int64_t>(missingMeshBuffer));
				BT_PLOT("SARP.StaticImportService.TemplateMissingMaterialHeightTexture", static_cast<int64_t>(missingMaterialHeightTexture));
				BT_PLOT("SARP.StaticImportService.TemplateRowsReady", static_cast<int64_t>(templateRequests.size()));
			}
			if (templateRequests.empty()) {
				BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainTemplateRequests::PublishExistingOnly");
				if (!publishedRefs.refs.empty() || !publishedRefs.failedKeys.empty()) {
					CompleteStaticTemplateArtifacts(std::move(publishedRefs));
				}
				for (auto& request : retryRequests) {
					(void)_drains->PushStreaming(_bus->staticImportTemplateRequestOutput, std::move(request));
				}
				if (!retryRequests.empty()) {
					_importState->staticTemplatePublicationNeedsBackoff.store(true, std::memory_order_release);
				}
				for (auto& request : leftovers) {
					(void)_drains->PushStreaming(_bus->staticImportTemplateRequestOutput, std::move(request));
				}
				return;
			}

			const auto templateBegin = std::chrono::steady_clock::now();
			std::vector<br::render::StaticMeshTemplateRegistration> registrations;
			{
				BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainTemplateRequests::AddStaticMeshTemplatesBulk");
				BT_ZONE_VALUE(static_cast<int64_t>(templateRequests.size()));
				registrations = meshManager->AddStaticMeshTemplatesBulk(templateRequests);
			}
			{
				BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainTemplateRequests::PublishTemplateRefs");
				std::size_t validRegistrations = 0;
				for (std::size_t i = 0; i < registrations.size() && i < keys.size(); ++i) {
					if (!registrations[i].valid) {
						if (registrations[i].pendingResources) {
							if (i < templateSourceRequests.size()) {
								const auto& pendingRequest = templateSourceRequests[i];
								if (pendingRequest.material) {
									materialManager->DecrementMaterialUsageCount(*pendingRequest.material);
								}
								if (pendingRequest.mesh && pendingRequest.material) {
									const auto materialEvalVariants =
										ComposeMaterialEvalVariantSet(*pendingRequest.mesh, *pendingRequest.material);
									materialManager->ReleaseCompileFlagsSlot(materialEvalVariants.regular);
									if (materialEvalVariants.hasDistinctReyes) {
										materialManager->ReleaseCompileFlagsSlot(materialEvalVariants.reyes);
									}
									materialManager->ReleaseRasterBucket(ComposeDirectStaticRasterFlags(*pendingRequest.mesh, *pendingRequest.material));
								}
								retryRequests.push_back(templateSourceRequests[i]);
							}
							continue;
						}
						publishedRefs.failedKeys.push_back(keys[i]);
						continue;
					}
					br::render::StaticMeshTemplateRef ref;
					ref.meshTemplateIndex = registrations[i].meshTemplateIndex;
					ref.clodOffsetIndex = registrations[i].clodOffsetIndex;
					ref.meshIdentity = meshes[i] ? meshes[i]->GetGlobalID() : 0u;
					ref.skinnedAssemblyTypeSlot = registrations[i].skinnedAssemblyTypeSlot;
					ref.skinnedAssemblyBounds = registrations[i].skinnedAssemblyBounds;
					ref.skinnedBoundsScale = registrations[i].skinnedBoundsScale;
					ref.mesh = meshes[i];
					ref.material = materials[i];
					if (ref.mesh && ref.material) {
						BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainTemplateRequests::PublishTemplateRefs::ResolveWorkloads");
						ForEachMeshDrawWorkload(*ref.mesh, *ref.material, [&](const DrawWorkloadKey& workloadKey) {
							ref.workloadKeys.push_back(workloadKey);
						});
					}
					publishedRefs.refs.emplace(keys[i],
						std::make_shared<const br::render::StaticMeshTemplateRef>(std::move(ref)));
					++_counters->staticDirectTemplatesCreated;
					++validRegistrations;
				}
				BT_PLOT("SARP.StaticImportService.TemplateRegistrationsValid", static_cast<int64_t>(validRegistrations));
				BT_PLOT("SARP.StaticImportService.TemplateRefsPublished", static_cast<int64_t>(publishedRefs.refs.size()));
			}
			_counters->staticDirectTemplateUs += ElapsedUs(templateBegin);

			if (!publishedRefs.refs.empty() || !publishedRefs.failedKeys.empty()) {
				BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainTemplateRequests::QueuePublishedRefs");
				CompleteStaticTemplateArtifacts(std::move(publishedRefs));
			}
			if (!retryRequests.empty()) {
				BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainTemplateRequests::RequeuePendingMeshBuffers");
				for (auto& request : retryRequests) {
					(void)_drains->PushStreaming(_bus->staticImportTemplateRequestOutput, std::move(request));
				}
				_importState->staticTemplatePublicationNeedsBackoff.store(true, std::memory_order_release);
			}
			if (!leftovers.empty()) {
				BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainTemplateRequests::RequeueLeftovers");
				for (auto& request : leftovers) {
					(void)_drains->PushStreaming(_bus->staticImportTemplateRequestOutput, std::move(request));
				}
			}
		}
		budget.ChargeElapsed(templateDrainBegin);
	}

	StaticImportServiceStats StaticImportStreamingPump::GetStatsSnapshot() const
	{
		BT_ZONE_SCOPE("SARP::LiveSceneBridge::GetStatsSnapshot");
		std::lock_guard lock(_importState->staticImportServiceStatsMutex);
		auto stats = _importState->staticImportServicePublishedStats;
		stats.readyGroupsQueued = _bus->staticImportGroupAssemblyJobs.Size() +
			_bus->staticImportCompletedOutput.Size();
		stats.assetPublishRequestsQueued = _bus->staticImportAssetPublishJobs.Size();
		stats.templateRequestsQueued = _bus->staticImportTemplateRequestOutput.Size();
		stats.materialVariantRequestsQueued = _bus->staticImportMaterialVariantJobs.Size();
		stats.shaderVariantRequestsQueued = _bus->staticImportShaderVariantJobs.Size();
		stats.removalDiffsQueued = _bus->staticImportRemovalOutput.Size();
		stats.retirementRequestsQueued = _bus->staticRetirementOwnershipRequests.Size() +
			_bus->staticRetirementPayloadRequests.Size();
		{
			std::lock_guard retirementLock(_importState->staticRetirementMutex);
			stats.retirementsAwaitingPublication = _importState->staticRetirementsAwaitingPublication.size();
		}
		stats.retirementCompletionsQueued = _bus->staticRetirementCompletions.Size();
		return stats;
	}

	std::uint64_t StaticImportStreamingPump::StaticPreparationQueueDepth() const
	{
		std::uint64_t extensionWork = 0;
		for (const auto* extension : _extensions) {
			extensionWork += extension->QueuedWorkCount();
		}
		return extensionWork + static_cast<std::uint64_t>(
			_bus->staticImportCellPrepareJobs.Size()
			+ _bus->staticImportPreparedCellResults.Size()
			+ _shared->graphCompletions->StaticGroupCount()
			+ _bus->staticImportGroupArtifactReleaseJobs.Size()
			+ _bus->staticImportGroupAssemblyJobs.Size()
			+ _bus->staticImportAssetPublishJobs.Size()
			+ _bus->staticImportAssetPublishResults.Size()
			+ _bus->staticImportMaterialVariantJobs.Size()
			+ _bus->staticImportMaterialVariantResults.Size()
			+ _bus->staticImportShaderVariantJobs.Size()
			+ _bus->staticImportShaderVariantResults.Size()
			+ _bus->staticImportCompletedOutput.Size()
			+ _bus->staticImportTransactionMaterializeJobs.Size()
			+ _importState->staticImportTransactionMaterializeBacklog.size()
			+ _bus->staticImportMaterializedValidationRequests.Size()
			+ _bus->staticImportMaterializedPublishDecisions.Size()
			+ _bus->staticImportPublicationJobs.Size()
			+ (_importState->staticImportPublicationCarryPending.load(std::memory_order_acquire) ? 1u : 0u)
			+ _bus->staticImportPublicationResults.Size()
			+ _bus->staticImportGpuReadyPublicationResults.Size()
			+ _importState->staticImportGpuPendingPublications.size()
			+ _importState->staticImportMaterializedDecisionBacklog.size()
			+ _importState->staticImportMaterialGraphPendingDecisions.size());
	}

	bool StaticImportStreamingPump::IsMaterialReadyForTemplatePublication(const Material& material) const
	{
		for (const auto* extension : _extensions) {
			if (!extension->IsMaterialReadyForTemplatePublication(material)) {
				return false;
			}
		}
		return true;
	}

	void StaticImportStreamingPump::ApplyDirectStaticWorkloadDeltas( const std::unordered_map<DrawWorkloadKey, std::int64_t, DrawWorkloadKey::Hasher>& deltas, std::unordered_map<DrawWorkloadKey, std::uint32_t, DrawWorkloadKey::Hasher>* dirtyWorkloadSpans)
	{
		if (!_shared->hasSceneIngestionServices ||
			!_shared->sceneIngestionServices.objects) {
			return;
		}
		auto* objectManager = _shared->sceneIngestionServices.objects;
		for (const auto& [workloadKey, delta] : deltas) {
			(void)delta;
			const auto activeDrawSet = objectManager->TryGetActiveDrawSetIndices(workloadKey);
			// CLOD object culling scans the append-only active-set span, not the
			// estimated live count. Use the same span as the indirect max command
			// count so tombstone estimate drift cannot clip unrelated live draws
			// that share the workload, such as terrain.
			const auto count = activeDrawSet
				? activeDrawSet->Size()
				: 0u;
			_importState->directStaticWorkloadCounts[workloadKey] = count;
			const auto clampedCount = static_cast<std::uint32_t>((std::min<std::uint64_t>)(count, std::numeric_limits<std::uint32_t>::max()));
			if (dirtyWorkloadSpans) {
				(*dirtyWorkloadSpans)[workloadKey] = clampedCount;
				continue;
			}
			if (auto* workloads = _shared->sceneIngestionServices.workloads) {
				workloads->PublishCount(workloadKey, clampedCount);
			}
		}
	}

	void StaticImportStreamingPump::FlushDirectStaticWorkloadCountUpdates( const std::unordered_map<DrawWorkloadKey, std::uint32_t, DrawWorkloadKey::Hasher>& dirtyWorkloadSpans)
	{
		if (dirtyWorkloadSpans.empty() ||
			!_shared->hasSceneIngestionServices ||
			!_shared->sceneIngestionServices.workloads) {
			return;
		}
		BT_ZONE_SCOPE("SARP::LiveSceneBridge::FlushDirectStaticWorkloadCountUpdates");
		auto* workloads = _shared->sceneIngestionServices.workloads;
		std::vector<WorkloadCountUpdate> updates;
		updates.reserve(dirtyWorkloadSpans.size());
		for (const auto& [workloadKey, span] : dirtyWorkloadSpans) {
			_importState->directStaticWorkloadCounts[workloadKey] = span;
			updates.push_back(WorkloadCountUpdate{
				.workloadKey = workloadKey,
				.count = span
			});
		}
		workloads->PublishCounts(updates);
		BT_PLOT("SARP.StaticImportService.WorkloadCountFlushes", static_cast<int64_t>(updates.size()));
	}

	bool StaticImportStreamingPump::TrimPreparedBatchToDesiredCells( PreparedDirectStaticImportBatch& batch, const StaticDesiredCellArea& desiredCells, const br::staticimport::StaticStreamTarget& target, std::uint64_t generation, std::vector<StaticImportGroupAck>* droppedAcks)
	{
		const auto shouldKeep = [&](const PreparedDirectStaticImport& prepared) {
			if (prepared.residencyClass == StaticImportResidencyClass::HostGroup) {
				return target.valid && target.interiorCellFormID == 0 && target.worldFormID == prepared.cellKey.worldFormID;
			}
			if (prepared.hostOwner) {
				return target.valid &&
					prepared.hostOwner->IsHostImportCurrent(target, prepared.cellKey, prepared.hostPayload);
			}
			return desiredCells.contains(prepared.cellKey);
		};
		bool removedAny = false;
		bool keptAll = true;
		for (const auto& prepared : batch.imports) {
			const bool keep = shouldKeep(prepared);
			removedAny = removedAny || !keep;
			keptAll = keptAll && keep;
		}
		if (keptAll) {
			batch.generation = generation;
			for (auto& prepared : batch.imports) {
				prepared.generation = generation;
			}
			return !batch.imports.empty();
		}

		const auto appendDroppedAck = [droppedAcks](
			const PreparedDirectStaticImport& prepared) {
			if (!droppedAcks) return;
			auto found = std::ranges::find(
				*droppedAcks, prepared.admissionTicketID,
				&StaticImportGroupAck::ticketID);
			if (found == droppedAcks->end()) {
				found = droppedAcks->emplace(droppedAcks->end());
				found->ticketID = prepared.admissionTicketID;
			}
			found->dropped.push_back(prepared.groupID);
		};
		PreparedDirectStaticImportBatch trimmed;
		trimmed.generation = generation;
		trimmed.publishTicketID = batch.publishTicketID;
		trimmed.requiredMaterialRevision = batch.requiredMaterialRevision;
		trimmed.requiredMaterial = batch.requiredMaterial;
		trimmed.preparedGroupVersions = std::move(batch.preparedGroupVersions);
		trimmed.buildUs = batch.buildUs;
		trimmed.prepareUs = batch.prepareUs;
		trimmed.transactionBuild.buildUs = batch.transactionBuild.buildUs;
		trimmed.transactionBuild.prepared.prepareUs = batch.transactionBuild.prepared.prepareUs;
		trimmed.transactionBuild.prepared.transformBuildUs = batch.transactionBuild.prepared.transformBuildUs;
		trimmed.transactionBuild.prepared.workloadBuildUs = batch.transactionBuild.prepared.workloadBuildUs;
		trimmed.transactionBuild.prepared.drawRecordBuildUs = batch.transactionBuild.prepared.drawRecordBuildUs;

		for (auto& prepared : batch.imports) {
			if (!shouldKeep(prepared)) {
				appendDroppedAck(prepared);
				continue;
			}
			const auto firstGroup = prepared.firstBuildGroup;
			const auto groupCount = prepared.submittedGroupCount;
			if (groupCount == 0 || firstGroup + groupCount > batch.transactionBuild.prepared.groups.size()) {
				appendDroppedAck(prepared);
				continue;
			}
			prepared.generation = generation;
			prepared.firstBuildGroup = trimmed.transactionBuild.prepared.groups.size();
			trimmed.transactionBuild.prepared.groups.insert(
				trimmed.transactionBuild.prepared.groups.end(),
				std::make_move_iterator(batch.transactionBuild.prepared.groups.begin() + static_cast<std::ptrdiff_t>(firstGroup)),
				std::make_move_iterator(batch.transactionBuild.prepared.groups.begin() + static_cast<std::ptrdiff_t>(firstGroup + groupCount)));
			trimmed.placementCount += prepared.placementCount;
			trimmed.groupIDs.push_back(prepared.groupID);
			trimmed.imports.push_back(std::move(prepared));
		}

		if (trimmed.imports.empty()) {
			batch = {};
			return false;
		}
		RecomputePreparedStaticPlanTotals(trimmed.transactionBuild.prepared);
		br::render::StaticObjectRequestService::FinalizeStaticImportBuildBatch(trimmed.transactionBuild);
		trimmed.transactionCost = StaticImportPreparedBatchCost(trimmed);
		batch = std::move(trimmed);
		return removedAny;
	}

	void StaticImportStreamingPump::CompleteStaticTemplateArtifacts(StaticTemplateRefsPublished packet)
	{
		if (!_importState->staticTemplateProducerRegistered || !_importState->staticTemplateSuspensionNotifier) {
			(void)_drains->PushBlocking(
				_bus->staticImportTemplateRefsPublished, std::move(packet));
			_drains->ScheduleCoordinatorDrain();
			return;
		}
		std::vector<std::uint64_t> notifications;
		{
			std::lock_guard lock(_importState->staticTemplateGraphMutex);
			for (auto& [key, ref] : packet.refs) {
				auto& completion = _importState->staticTemplateGraphCompletions[key];
				completion.status = StaticTemplateExternalCompletion::Status::Ready;
				completion.ref = *ref;
				if (completion.suspensionIdentity != 0) {
					notifications.push_back(completion.suspensionIdentity);
				}
				_importState->staticTemplateGraphSources.erase(key);
			}
			for (const auto& key : packet.failedKeys) {
				auto& completion = _importState->staticTemplateGraphCompletions[key];
				completion.status = StaticTemplateExternalCompletion::Status::Failed;
				completion.error = "static template GPU-row construction failed";
				if (completion.suspensionIdentity != 0) {
					notifications.push_back(completion.suspensionIdentity);
				}
				_importState->staticTemplateGraphSources.erase(key);
			}
		}
		for (const auto identity : notifications) {
			_importState->staticTemplateSuspensionNotifier(identity);
		}
		// The selector artifact and the coordinator snapshot are two consumers of
		// the same immutable completion. Previously the graph-enabled path notified
		// only the selector. Its exact CpuReady awaiter eventually mirrored successful
		// refs, but it cannot satisfy on a terminal Failed artifact, leaving the
		// coordinator dependency—and every StaticVariant using it—blocked forever.
		// Publish the original ready/failed packet directly to coordinator state;
		// duplicate successful delivery from an already-registered awaiter is
		// idempotent (`insert_or_assign` plus keyed waiter removal).
		(void)_drains->PushBlocking(
			_bus->staticImportTemplateRefsPublished, std::move(packet));
		_drains->ScheduleCoordinatorDrain();
	}

	void StaticImportStreamingPump::QueueMaterializedDecisionCleanup(std::list<StaticImportMaterializedPublishDecision>& decisions)
	{
		if (decisions.empty()) {
			return;
		}

		{
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::QueueMaterializedDecisionCleanup");
			BT_ZONE_VALUE(static_cast<int64_t>(decisions.size()));
			std::lock_guard lock(_importState->staticImportServiceMutex);
			_importState->staticImportMaterializedDecisionCleanupBacklog.splice(
				_importState->staticImportMaterializedDecisionCleanupBacklog.end(),
				decisions);
			BT_PLOT("SARP.StaticDirect.MaterializedDecisionCleanupBacklog", static_cast<int64_t>(_importState->staticImportMaterializedDecisionCleanupBacklog.size()));
		}
		_shared->staticImportWorkScheduler.Signal(
			StaticImportWorkSource::MaterializedDecisionCleanup);
		_drains->ScheduleWorkerDrains();
	}

	void StaticImportStreamingPump::QueueRemovalCleanup( std::vector<StaticRemovalDiff>& removals, std::vector<br::render::StaticObjectRemovalPayload>& removalPayloads)
	{
		if (removals.empty() && removalPayloads.empty()) {
			return;
		}

		{
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainRemovals::QueueRemovalCleanup");
			BT_ZONE_VALUE(static_cast<int64_t>(removals.size() + removalPayloads.size()));
			std::lock_guard lock(_importState->staticImportServiceMutex);
			if (_importState->staticImportRemovalDiffCleanupBacklog.empty()) {
				_importState->staticImportRemovalDiffCleanupBacklog.swap(removals);
			} else {
				_importState->staticImportRemovalDiffCleanupBacklog.insert(
					_importState->staticImportRemovalDiffCleanupBacklog.end(),
					std::make_move_iterator(removals.begin()),
					std::make_move_iterator(removals.end()));
				removals.clear();
			}
			if (_importState->staticImportRemovalPayloadCleanupBacklog.empty()) {
				_importState->staticImportRemovalPayloadCleanupBacklog.swap(removalPayloads);
			} else {
				_importState->staticImportRemovalPayloadCleanupBacklog.insert(
					_importState->staticImportRemovalPayloadCleanupBacklog.end(),
					std::make_move_iterator(removalPayloads.begin()),
					std::make_move_iterator(removalPayloads.end()));
				removalPayloads.clear();
			}
			BT_PLOT("SARP.StaticDirect.RemovalDiffCleanupBacklog", static_cast<int64_t>(_importState->staticImportRemovalDiffCleanupBacklog.size()));
			BT_PLOT("SARP.StaticDirect.RemovalPayloadCleanupBacklog", static_cast<int64_t>(_importState->staticImportRemovalPayloadCleanupBacklog.size()));
		}
		_shared->staticImportWorkScheduler.Signal(StaticImportWorkSource::RemovalCleanup);
		_drains->ScheduleWorkerDrains();
	}

	bool StaticImportStreamingPump::FlushGroupAckBacklog()
	{
		bool pushedAny = false;
		while (!_importState->staticImportGroupAckBacklog.empty()) {
			if (!_bus->staticImportGroupAcks.TryPush(std::move(_importState->staticImportGroupAckBacklog.front()))) {
				break;
			}
			_importState->staticImportGroupAckBacklog.pop_front();
			pushedAny = true;
		}
		if (pushedAny) {
			_drains->ScheduleCoordinatorDrain();
		}
		BT_PLOT("SARP.StaticDirect.GroupAckBacklog", static_cast<int64_t>(_importState->staticImportGroupAckBacklog.size()));
		return pushedAny;
	}

	bool StaticImportStreamingPump::FlushTransactionMaterializeBacklog()
	{
		bool pushedAny = false;
		while (!_importState->staticImportTransactionMaterializeBacklog.empty()) {
			if (!_bus->staticImportTransactionMaterializeJobs.TryPush(std::move(_importState->staticImportTransactionMaterializeBacklog.front()))) {
				break;
			}
			_importState->staticImportTransactionMaterializeBacklog.pop_front();
			_shared->staticImportWorkScheduler.Publish(
				StaticImportWorkSource::TransactionMaterialize);
			pushedAny = true;
		}
		if (pushedAny) {
			_drains->ScheduleWorkerDrains();
		}
		BT_PLOT(
			"SARP.StaticDirect.TransactionMaterializeBacklog",
			static_cast<int64_t>(_importState->staticImportTransactionMaterializeBacklog.size()));
		return pushedAny;
	}

	void StaticImportStreamingPump::EnsureWorkersStarted()
	{
		if (_importState->staticImportServiceWorkersStarted) {
			return;
		}
		_shared->staticImportServiceStop.store(false, std::memory_order_release);
		const auto workerCount = (std::max)(1u,
			TaskSchedulerManager::GetInstance().WorkerCount());
		// Keep a bounded ready buffer beyond the domain's admitted slots. Other
		// StaticImport producers (notably host extension work) share the domain; limiting
		// this bridge to the domain limit lets those external tasks occupy every slot
		// while no bridge continuation is ready to take the next completion edge.
		_importState->staticImportWorkerConcurrency = workerCount;
		_lifecycle->Configure(workerCount);
		_capacity->Configure(
			{ .preparedBytes = _settings->preparedReservationByteCeiling },
			[this] { _drains->ScheduleCoordinatorDrain(); });
		_shared->staticImportWorkScheduler.SetMaxParallelism(
			StaticImportWorkSource::HostPrepare, (std::min)(2u, workerCount));
		_shared->staticImportWorkScheduler.SetMaxParallelism(
			// Cell discovery/classification is the feeder for every StaticGroup
			// producer. Capping it at two made the producer pool input-bound on
			// machines configured with more StaticImport workers. These jobs have
			// immutable recipe/snapshot inputs and publish through an MPSC handoff,
			// so their useful parallelism is the configured worker capacity.
			StaticImportWorkSource::CellPrepare, workerCount);
		_shared->staticImportWorkScheduler.SetMaxParallelism(
			// Assembly cooperatively claims up to 32 source tickets and drains the
			// matching queue entries as one transaction batch. Multiple consumers
			// can otherwise pop entries covered by another consumer's additional
			// claims, permanently separating scheduler tickets from queued work.
			// Row assembly is CPU-only and batched; materialization remains parallel.
			StaticImportWorkSource::GroupAssembly, 1u);
		_shared->staticImportWorkScheduler.SetMaxParallelism(
			StaticImportWorkSource::TransactionMaterialize, (std::min)(8u, workerCount));
		_shared->staticImportWorkScheduler.SetMaxParallelism(
			StaticImportWorkSource::Publication, 1u);
		_shared->staticImportWorkScheduler.SetMaxParallelism(
			StaticImportWorkSource::AssetPublish, (std::min)(4u, workerCount));
		_importState->staticImportServiceWorkersStarted = true;
		_drains->ScheduleCoordinatorDrain();
		const auto explicitCreditLimit =
			_settings->preparationCreditLimit;
		if (explicitCreditLimit != 0) {
			spdlog::info(
				"SARP static import service: scheduler drains enabled (concurrency={} compatibility_preparation_credits={})",
				workerCount, explicitCreditLimit);
		} else {
			spdlog::info(
				"SARP static import service: scheduler drains enabled (concurrency={} prepared_reservation_bytes={})",
				workerCount,
				_settings->preparedReservationByteCeiling);
		}
	}

	void StaticImportStreamingPump::UpdateCameraTarget(const br::staticimport::StaticStreamTarget& target)
	{
		StaticImportCameraTarget published;
		published.key = MakeStaticStreamKey(target, _settings->cellRadius);
		published.valid = target.valid;
		{
			std::lock_guard lock(_importState->staticImportServiceCameraMutex);
			if (_importState->staticImportServiceLastPublishedTarget &&
				_importState->staticImportServiceLastPublishedTarget->valid == published.valid &&
				_importState->staticImportServiceLastPublishedTarget->key == published.key) {
				return;
			}
			published.stream = target;
			published.generation = _importState->staticImportServiceTargetGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
			_importState->staticImportServicePendingTarget = published;
			_importState->staticImportServiceLastPublishedTarget = published;
			_importState->staticImportServiceHasPendingTarget.store(true, std::memory_order_release);
		}
		_drains->ScheduleCoordinatorDrain();
	}

	void StaticImportStreamingPump::UpdateStaticStreamingDirectImportService( const br::staticimport::StaticStreamTarget& target, std::chrono::steady_clock::time_point updateBegin)
	{
		BT_ZONE_SCOPE("SARP::LiveSceneBridge::UpdateStaticStreamingDirectImportService");
		EnsureWorkersStarted();
		{
			std::exception_ptr failure;
			bool hasPendingRetirements = false;
			{
				std::lock_guard lock(_importState->staticRetirementMutex);
				failure = std::exchange(_importState->staticRetirementFailure, {});
				hasPendingRetirements = !_importState->staticRetirementsAwaitingPublication.empty();
			}
			if (failure) std::rethrow_exception(failure);
			if (hasPendingRetirements) {
				_shared->staticImportWorkScheduler.Signal(StaticImportWorkSource::RetirementCommit);
				_drains->ScheduleWorkerDrains();
			}
		}
		const auto queueBudget = MakeStaticStreamBudget();
		std::size_t applied = 0;
		std::size_t pendingOrSkipped = 0;
		std::size_t upsertsThisFrame = 0;
		std::unordered_map<DrawWorkloadKey, std::uint32_t, DrawWorkloadKey::Hasher> dirtyWorkloadSpans;
		_counters->lastStaticCellsEntered = 0;
		_counters->lastStaticCellsLeft = 0;
		_counters->lastStaticCellsPreparedThisFrame = 0;
		_counters->lastStaticGroupsImportedThisFrame = 0;
		_counters->lastStaticCellsRemovedThisFrame = 0;
		_counters->lastStaticStreamBudgetUsedUs = 0;
		_counters->lastStaticStreamBudgetOverrunUs = 0;

		if (target.valid && target.interiorCellFormID == 0 && target.worldFormID == 0) {
			BT_PLOT("SARP.StaticImportService.Update.SkippedMissingWorld", int64_t{ 1 });
			_counters->staticStreamUpdateUs += ElapsedUs(updateBegin);
			return;
		}

		{
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::UpdateStaticStreamingDirectImportService::PublishCamera");
			UpdateCameraTarget(target);
		}
		auto currentDesiredStaticCells = BuildDesiredStaticCells(target);
		// Queued requests are level-triggered. A pending material batch is not:
		// its exact-version awaiter owns the next wake. Re-signalling that passive
		// wait every frame creates an endless TemplatePublication drain with no
		// possible progress and can starve the actual publication graph.
		if (!_bus->staticImportTemplateRequestOutput.Empty()) {
			_shared->staticImportWorkScheduler.Signal(StaticImportWorkSource::TemplatePublication);
			_drains->ScheduleWorkerDrains();
		}
		// Completion reconciliation is level-triggered correctness work. It must
		// run even when earlier source-mutation batching consumed the discretionary
		// frame budget; DrainReadyDiffs limits new work itself.
		// Skipping the consumer here allowed a full decision queue to remain ready
		// forever with no subsequent callback responsible for ownership transfer.
		{
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::UpdateStaticStreamingDirectImportService::DrainReadyDiffs");
			DrainReadyDiffs(
				applied,
				pendingOrSkipped,
				upsertsThisFrame,
				queueBudget,
				currentDesiredStaticCells,
				target,
				std::addressof(dirtyWorkloadSpans));
		}
		// Keep the publication handoff observable even after it stops producing
		// scopes.  A one-shot scope count cannot distinguish an idle lane from a
		// lost edge with immutable work stranded between the coordinator, host
		// reconciliation, and the graph cut awaiters.
		BT_PLOT("SARP.StaticPublication.ValidationQueue",
			static_cast<int64_t>(_bus->staticImportMaterializedValidationRequests.Size()));
		BT_PLOT("SARP.StaticPublication.DecisionQueue",
			static_cast<int64_t>(_bus->staticImportMaterializedPublishDecisions.Size()));
		BT_PLOT("SARP.StaticPublication.DecisionBacklog",
			static_cast<int64_t>(_importState->staticImportMaterializedDecisionBacklog.size()));
		BT_PLOT("SARP.StaticPublication.JobQueue",
			static_cast<int64_t>(_bus->staticImportPublicationJobs.Size()));
		BT_PLOT("SARP.StaticPublication.ResultQueue",
			static_cast<int64_t>(_bus->staticImportPublicationResults.Size()));
		BT_PLOT("SARP.StaticPublication.GpuReadyQueue",
			static_cast<int64_t>(_bus->staticImportGpuReadyPublicationResults.Size()));
		BT_PLOT("SARP.StaticPublication.GpuPendingCuts",
			static_cast<int64_t>(_importState->staticImportGpuPendingPublications.size()));
		BT_PLOT("SARP.StaticPublication.AdmissionPending",
			_importState->staticImportPublicationAdmissionPending.load(std::memory_order_acquire) ? int64_t{ 1 } : int64_t{ 0 });
		FlushDirectStaticWorkloadCountUpdates(dirtyWorkloadSpans);

		StaticImportServiceStats stats;
		{
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::UpdateStaticStreamingDirectImportService::StatsSnapshot");
			stats = GetStatsSnapshot();
		}
		_counters->staticStreamGeneration = stats.generation;
		_importState->lastStaticStreamKey = MakeStaticStreamKey(target, _settings->cellRadius);
		_counters->lastStaticActiveCells = stats.desiredCells;
		_counters->lastStaticVisitedPlacements = applied + stats.pendingPlacements;
		_counters->lastStaticAppliedPlacements = _importState->staticAppliedPlacementCount;
		_counters->lastStaticPendingOrSkippedPlacements = stats.pendingPlacements;
		_counters->lastStaticUpsertsThisFrame = upsertsThisFrame;
		_counters->lastStaticCellsEntered = stats.cellsEntered;
		_counters->lastStaticCellsLeft = stats.cellsLeft;
		_counters->lastStaticCellsPreparedThisFrame = stats.cellsPrepared;
		_counters->lastStaticStreamBudgetUsedUs = queueBudget.ElapsedUs();
		if (!queueBudget.Unlimited() && _counters->lastStaticStreamBudgetUsedUs >= queueBudget.totalUs) {
			++_counters->staticStreamBudgetExhaustedFrames;
			_counters->lastStaticStreamBudgetOverrunUs = _counters->lastStaticStreamBudgetUsedUs - queueBudget.totalUs;
			_counters->staticStreamMaxBudgetOverrunUs = (std::max)(_counters->staticStreamMaxBudgetOverrunUs, _counters->lastStaticStreamBudgetOverrunUs);
		}
		if (_importState->benchmarkDiagnosticsEnabled) {
			_counters->staticStreamMaxImportQueueDepth = (std::max<std::uint64_t>)(
				_counters->staticStreamMaxImportQueueDepth,
				static_cast<std::uint64_t>(stats.pendingGroups));
			_counters->staticStreamMaxRemovalQueueDepth = (std::max<std::uint64_t>)(
				_counters->staticStreamMaxRemovalQueueDepth,
				static_cast<std::uint64_t>(stats.removalDiffsQueued));
			BT_PLOT("SARP.StaticImportService.PendingGroups", static_cast<int64_t>(stats.pendingGroups));
			BT_PLOT("SARP.StaticImportService.PendingPlacements", static_cast<int64_t>(stats.pendingPlacements));
			BT_PLOT("SARP.StaticImportService.ReadyGroups", static_cast<int64_t>(stats.readyGroupsQueued));
			BT_PLOT("SARP.StaticImportService.AssetPublishRequests", static_cast<int64_t>(stats.assetPublishRequestsQueued));
			BT_PLOT("SARP.StaticImportService.TemplateRequests", static_cast<int64_t>(stats.templateRequestsQueued));
			BT_PLOT("SARP.StaticImportService.MaterialVariantRequests", static_cast<int64_t>(stats.materialVariantRequestsQueued));
			BT_PLOT("SARP.StaticImportService.ShaderVariantRequests", static_cast<int64_t>(stats.shaderVariantRequestsQueued));
			BT_PLOT("SARP.StaticRetirement.Queued", static_cast<int64_t>(
				stats.removalDiffsQueued + stats.retirementRequestsQueued));
			BT_PLOT("SARP.StaticRetirement.AwaitingPublication", static_cast<int64_t>(
				stats.retirementsAwaitingPublication));
			BT_PLOT("SARP.StaticRetirement.CompletionBacklog", static_cast<int64_t>(
				stats.retirementCompletionsQueued));
			LogStaticStreamStatus(target);
		}
		_counters->staticStreamUpdateUs += ElapsedUs(updateBegin);
	}

	void StaticImportStreamingPump::DrainRemovals( const StaticStreamBudget& budget, const StaticDesiredCellArea& currentDesiredCells, const br::staticimport::StaticStreamTarget& target, std::unordered_map<DrawWorkloadKey, std::uint32_t, DrawWorkloadKey::Hasher>* dirtyWorkloadSpans)
	{
		// Compatibility-only entry point for downstream branches. RetirementApply
		// exclusively owns the queue; never drain or mutate ObjectManager here.
		(void)budget;
		(void)currentDesiredCells;
		(void)target;
		(void)dirtyWorkloadSpans;
		_shared->staticImportWorkScheduler.Signal(StaticImportWorkSource::RetirementApply);
		_drains->ScheduleWorkerDrains();
		return;

		std::vector<StaticRemovalDiff> removals;
		{
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainRemovals::SwapQueueLock");
			_bus->staticImportRemovalOutput.Drain(removals, 4096);
		}
		if (removals.empty()) {
			return;
		}

		BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainRemovals");
		const auto removeBegin = std::chrono::steady_clock::now();
		const auto maxCells = _settings->maxRemoveCellsPerFrame;
		std::vector<StaticRemovalDiff> leftovers;
		if (maxCells != 0 && removals.size() > maxCells) {
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainRemovals::SplitBudgetLeftovers");
			leftovers.insert(
				leftovers.end(),
				std::make_move_iterator(removals.begin() + static_cast<std::ptrdiff_t>(maxCells)),
				std::make_move_iterator(removals.end()));
			removals.erase(removals.begin() + static_cast<std::ptrdiff_t>(maxCells), removals.end());
		}
		BT_PLOT("SARP.StaticImportService.RemovalCells", static_cast<int64_t>(removals.size()));
		BT_PLOT("SARP.StaticImportService.RemovalLeftoverCells", static_cast<int64_t>(leftovers.size()));
		std::unordered_map<DrawWorkloadKey, std::int64_t, DrawWorkloadKey::Hasher> workloadDeltas;
		std::vector<br::render::StaticObjectRemovalPayload> removalPayloads;
		std::vector<StaticImportGroupAck::UnpublishedGroup> graphUnpublishedGroups;
		std::size_t removalGroupIDs = 0;
		std::size_t desiredCellsSkipped = 0;
		std::size_t staleGenerationGroupsSkipped = 0;
		std::size_t cellMismatchGroupsSkipped = 0;
		std::size_t queuedNearCells = 0;
		std::size_t queuedFullRadiusCandidateCells = 0;
		std::size_t currentNearCells = 0;
		std::size_t currentNearAppliedCells = 0;
		std::size_t currentNearAppliedGroups = 0;
		std::size_t appliedCells = 0;
		std::size_t appliedGroups = 0;
		for (const auto& removal : removals) {
			removalGroupIDs += removal.groupIDs.size();
			if (removal.distanceToTarget <= removal.targetKey.radius) {
				++queuedNearCells;
			}
			if (removal.desiredCellsAtQueue != 0 &&
				removal.leftCellsAtQueue > (std::max<std::size_t>)(1, removal.desiredCellsAtQueue / 2)) {
				++queuedFullRadiusCandidateCells;
			}
		}
		removalPayloads.reserve(removalGroupIDs);
		graphUnpublishedGroups.reserve(removalGroupIDs);
		const auto publishedStateSource = br::render::PublishedStateSource::ProcessSource();
		const auto publishedManifest = publishedStateSource ? publishedStateSource->Load() : nullptr;
		const auto publishedStaticScene = publishedManifest
			? publishedManifest->geometry.payload.Get<br::render::PublishedStaticSceneState>()
			: nullptr;
		{
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainRemovals::CollectDrawInfosAndDeltas");
			for (const auto& removal : removals) {
				const auto currentDistance = StaticCellDistanceToTarget(target, removal.cellKey);
				const auto currentRadius = static_cast<unsigned int>(_settings->cellRadius);
				const bool nearCurrentCamera = currentDistance <= currentRadius;
				if (nearCurrentCamera) {
					++currentNearCells;
				}
				if (currentDesiredCells.contains(removal.cellKey)) {
					++desiredCellsSkipped;
					continue;
				}
				std::size_t appliedGroupsForCell = 0;
				{
					BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainRemovals::CollectDrawInfosAndDeltas::Cell");
					BT_ZONE_VALUE(removal.groupIDs.size());
					for (const auto groupID : removal.groupIDs) {
						const auto* selectedGroup = publishedStaticScene
							? publishedStaticScene->FindGroup(groupID) : nullptr;
						const auto graphOwnership = selectedGroup && selectedGroup->ownership
							? std::static_pointer_cast<const StaticGraphGroupOwnership>(
								selectedGroup->ownership)
							: nullptr;
						// A removal is defined against the selected immutable scene cut.
						// GPU-ready-but-unselected transactions are not visible and therefore
						// must not be retired through a host acknowledgement record.
						if (!graphOwnership) continue;
						if (!(graphOwnership->cellKey == removal.cellKey)) {
							++cellMismatchGroupsSkipped;
							continue;
						}
						if (removal.generation != 0 && graphOwnership->generation > removal.generation) {
							++staleGenerationGroupsSkipped;
							continue;
						}
						const auto workloadKeys = graphOwnership->recipeVariantBinding
							? std::span<const DrawWorkloadKey>{ graphOwnership->recipeVariantBinding->workloadKeys }
							: std::span<const DrawWorkloadKey>{ graphOwnership->workloadKeys };
						for (const auto& workloadKey : workloadKeys) {
							workloadDeltas[workloadKey] -= 1;
						}
						for (std::size_t extensionIndex = 0; extensionIndex < _extensions.size(); ++extensionIndex) {
							_extensions[extensionIndex]->OnGroupRemovalApplied(groupID,
								br::staticimport::StaticGroupHoldAt(graphOwnership->extensionHolds, extensionIndex));
						}
						(void)graphOwnership->TryClaimRetirement();
						if (graphOwnership->removalPayload) {
							removalPayloads.push_back(*graphOwnership->removalPayload);
						}
						if (graphOwnership->ConsumeApplied()) {
							_importState->staticAppliedPlacementCount -= (std::min)(
								_importState->staticAppliedPlacementCount,
								graphOwnership->instanceCount);
							if (_importState->staticRuntimeGroupActiveCount != 0) {
								--_importState->staticRuntimeGroupActiveCount;
							}
						}
						++_counters->staticDirectGroupsRemoved;
						graphUnpublishedGroups.push_back({ groupID, graphOwnership->generation });
						++appliedGroups;
						++appliedGroupsForCell;
					}
				}
				if (appliedGroupsForCell != 0) {
					++appliedCells;
					if (nearCurrentCamera) {
						++currentNearAppliedCells;
						currentNearAppliedGroups += appliedGroupsForCell;
						if (_importState->benchmarkDiagnosticsEnabled &&
							_importState->staticRemovalTelemetryLogCount.fetch_add(1, std::memory_order_relaxed) < 64) {
							spdlog::warn(
								"SARP static import telemetry: applied near-cell removal cell={} currentTarget={} queuedTarget={} currentDistance={} queuedDistance={} groupsApplied={} groupsQueued={} removalGeneration={} currentGeneration={} desiredAtQueue={} leftAtQueue={}",
								FormatStaticCellKey(removal.cellKey),
								FormatStaticStreamKey(MakeStaticStreamKey(target, _settings->cellRadius)),
								FormatStaticStreamKey(removal.targetKey),
								currentDistance,
								removal.distanceToTarget,
								appliedGroupsForCell,
								removal.groupIDs.size(),
								removal.generation,
								_importState->staticImportServiceTargetGeneration.load(std::memory_order_relaxed),
								removal.desiredCellsAtQueue,
								removal.leftCellsAtQueue);
						}
					}
				}
				{
					BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainRemovals::NotifyCellInvalidated");
					for (auto* extension : _extensions) {
						extension->OnCellRemovalApplied(removal.cellKey, removal.generation);
					}
				}
			}
		}
		BT_PLOT("SARP.StaticImportService.RemovalGroupIDs", static_cast<int64_t>(removalGroupIDs));
		BT_PLOT("SARP.StaticImportService.RemovalDrawInfos", static_cast<int64_t>(removalPayloads.size()));
		BT_PLOT("SARP.StaticImportService.RemovalDesiredCellsSkipped", static_cast<int64_t>(desiredCellsSkipped));
		BT_PLOT("SARP.StaticImportService.RemovalStaleGenerationGroupsSkipped", static_cast<int64_t>(staleGenerationGroupsSkipped));
		BT_PLOT("SARP.StaticImportService.RemovalCellMismatchGroupsSkipped", static_cast<int64_t>(cellMismatchGroupsSkipped));
		BT_PLOT("SARP.StaticImportService.RemovalQueuedNearCells", static_cast<int64_t>(queuedNearCells));
		BT_PLOT("SARP.StaticImportService.RemovalQueuedFullRadiusCandidateCells", static_cast<int64_t>(queuedFullRadiusCandidateCells));
		BT_PLOT("SARP.StaticImportService.RemovalCurrentNearCells", static_cast<int64_t>(currentNearCells));
		BT_PLOT("SARP.StaticImportService.RemovalCurrentNearAppliedCells", static_cast<int64_t>(currentNearAppliedCells));
		BT_PLOT("SARP.StaticImportService.RemovalCurrentNearAppliedGroups", static_cast<int64_t>(currentNearAppliedGroups));
		BT_PLOT("SARP.StaticImportService.RemovalAppliedCells", static_cast<int64_t>(appliedCells));
		BT_PLOT("SARP.StaticImportService.RemovalAppliedGroups", static_cast<int64_t>(appliedGroups));
		if (!removalPayloads.empty() && _shared->hasSceneIngestionServices && _shared->sceneIngestionServices.objects) {
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainRemovals::RemoveObjectsBulk");
			BT_ZONE_VALUE(removalPayloads.size());
			auto* objectManager = _shared->sceneIngestionServices.objects;
			br::render::RemoveObjectsBulkOptions removalOptions;
			removalOptions.deferBufferRangeRetirement = true;
			// Static active draw sets are append-only/tombstone-based. Keep draw-record
			// indices non-reusable until the active-set lifetime model is fully exact;
			// reusing them while stale active entries can still exist can invalidate
			// otherwise-live work through the generation sidecar.
			removalOptions.retireInstanceDrawRecordRanges = false;
			removalOptions.retireFrame = objectManager->MakeDeferredRetireFrame();
			objectManager->RemoveStaticObjectsBulk(removalPayloads, removalOptions);
		}
		{
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainRemovals::EraseStaticObjects");
			BT_ZONE_VALUE(0);
		}
		{
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainRemovals::ApplyWorkloadDeltas");
			BT_PLOT("SARP.StaticImportService.RemovalWorkloadDeltaBuckets", static_cast<int64_t>(workloadDeltas.size()));
			ApplyDirectStaticWorkloadDeltas(workloadDeltas, dirtyWorkloadSpans);
		}
		if (!leftovers.empty()) {
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainRemovals::RequeueLeftovers");
			for (auto& leftover : leftovers) {
				(void)_drains->PushStreaming(_bus->staticImportRemovalOutput, std::move(leftover));
			}
		}
		QueueRemovalCleanup(removals, removalPayloads);
		if (!graphUnpublishedGroups.empty()) {
			StaticImportGroupAck graphAck;
			graphAck.unpublished = std::move(graphUnpublishedGroups);
			QueueStaticImportGroupAckForCoordinator(std::move(graphAck));
		}
		{
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainRemovals::FinalizeStatsAndBudget");
			_counters->staticStreamRemoveUs += ElapsedUs(removeBegin);
			budget.ChargeElapsed(removeBegin);
		}
	}

	void StaticImportStreamingPump::DrainReadyDiffs( std::size_t& applied, std::size_t& pendingOrSkipped, std::size_t& upsertsThisFrame, const StaticStreamBudget& budget, const StaticDesiredCellArea& currentDesiredCells, const br::staticimport::StaticStreamTarget& target, std::unordered_map<DrawWorkloadKey, std::uint32_t, DrawWorkloadKey::Hasher>* dirtyWorkloadSpans)
	{
		BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs");
		if (!_shared->hasSceneIngestionServices || !_shared->sceneIngestionServices.objects) {
			return;
		}
		{
			std::vector<StaticRetirementCompletion> completions;
			_bus->staticRetirementCompletions.Drain(completions, 4096);
			for (auto& completion : completions) {
				_importState->staticAppliedPlacementCount -= (std::min)(
					_importState->staticAppliedPlacementCount, completion.placements);
				_importState->staticRuntimeGroupActiveCount -= (std::min)(
					_importState->staticRuntimeGroupActiveCount, completion.groups);
				_counters->staticDirectGroupsRemoved += completion.groups;
				_counters->lastStaticCellsRemovedThisFrame += completion.groups;
			}
			BT_PLOT("SARP.StaticRetirement.CompletedBatches", static_cast<int64_t>(completions.size()));
		}

		{
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::ApplySceneOwnershipDeltas");
			std::vector<StaticSceneOwnershipDelta> deltas;
			_bus->staticSceneOwnershipDeltas.Drain(deltas, 4096);
			std::size_t queuedRetirements = 0;
			// Retirements are handed to the worker queue as one batch (one lock, no
			// per-ownership zone), and a burst of deltas is split across frames by an
			// ownership budget so a scene checkpoint cannot own the whole frame.
			constexpr std::size_t kOwnershipBudgetPerFrame = 2048;
			std::vector<StaticRetirementOwnershipRequest> retirements;
			std::size_t processedOwnerships = 0;
			std::size_t processedDeltas = 0;
			for (auto& delta : deltas) {
				if (processedOwnerships >= kOwnershipBudgetPerFrame) break;
				++processedDeltas;
				processedOwnerships += delta.removed.size() + delta.added.size() + delta.retireOnly.size();
				for (const auto& ownership : delta.removed) {
					if (!ownership || ownership->residencyClass != StaticImportResidencyClass::CellStatic) continue;
					retirements.push_back({ ownership, false });
					++queuedRetirements;
				}
				for (const auto& ownership : delta.added) {
					if (!ownership || ownership->residencyClass != StaticImportResidencyClass::CellStatic) continue;
					if (!currentDesiredCells.contains(ownership->cellKey)) {
						retirements.push_back({ ownership, true });
						++queuedRetirements;
						continue;
					}
					_importState->directStaticPrepareRetries.erase(ownership->groupID);
					ownership->MarkApplied();
					_importState->staticAppliedPlacementCount += ownership->instanceCount;
					++_importState->staticRuntimeGroupActiveCount;
					for (auto* extension : _extensions) {
						extension->OnCellGroupPublished(ownership->cellKey, ownership->generation, ownership->groupID);
					}
					applied += ownership->instanceCount;
					++upsertsThisFrame;
					++_counters->staticDirectGroupsImported;
					++_counters->lastStaticGroupsImportedThisFrame;
					++_counters->staticDirectImportBatchesCommitted;
					if (auto* requests = _shared->sceneIngestionServices.execution.stateRequests;
						requests && requests->TraceActive()) {
						requests->TraceEvent(br::render::AsyncStateGraphTraceEventID::StaticGroupBridgeApplied,
							{ br::render::ArtifactKind::StaticGroup, ownership->groupID, 0 },
							ownership->generation, 0,
							{ { ownership->instanceCount, 0, 0 } });
					}
				}
				for (const auto& ownership : delta.retireOnly) {
					if (!ownership || ownership->residencyClass != StaticImportResidencyClass::CellStatic ||
						!ownership->removalPayload) continue;
					retirements.push_back({ ownership, false });
					++queuedRetirements;
				}
			}
			if (processedDeltas < deltas.size()) {
				BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::ApplySceneOwnershipDeltas::Deferred");
				BT_ZONE_VALUE(static_cast<int64_t>(deltas.size() - processedDeltas));
				for (std::size_t index = deltas.size(); index > processedDeltas; --index) {
					(void)_bus->staticSceneOwnershipDeltas.TryPushFront(std::move(deltas[index - 1u]));
				}
			}
			if (!retirements.empty()) {
				BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::ApplySceneOwnershipDeltas::QueueRetirements");
				BT_ZONE_VALUE(static_cast<int64_t>(retirements.size()));
				_bus->staticRetirementOwnershipRequests.PushDeferredRange(retirements.begin(), retirements.end());
			}
			if (queuedRetirements != 0) {
				_shared->staticImportWorkScheduler.Signal(StaticImportWorkSource::RetirementApply);
				_drains->ScheduleWorkerDrains();
			}
			BT_PLOT("SARP.StaticGraph.SceneOwnershipDeltaBatches", static_cast<int64_t>(deltas.size()));
			BT_PLOT("SARP.StaticGraph.SceneOwnershipRetirements", static_cast<int64_t>(queuedRetirements));
		}

		(void)FlushGroupAckBacklog();
		(void)FlushTransactionMaterializeBacklog();

		auto* objectManager = _shared->sceneIngestionServices.objects;
		for (auto* extension : _extensions) {
			extension->DrainFrameWork(target);
		}
		const auto pushAck = [this](StaticImportGroupAck ack) {
			QueueStaticImportGroupAckForCoordinator(std::move(ack));
		};
		{
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::MaterializedStage");
			if (_shared->graphCompletions->materialTableWake.exchange(false, std::memory_order_acq_rel)) {
				BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::WakeGraphPendingMaterializedDecisions");
				BT_ZONE_VALUE(static_cast<int64_t>(_importState->staticImportMaterialGraphPendingDecisions.size()));
				_importState->staticImportMaterializedDecisionBacklog.splice(
					_importState->staticImportMaterializedDecisionBacklog.end(),
					_importState->staticImportMaterialGraphPendingDecisions);
			}
			std::vector<StaticImportMaterializedPublishDecision> drainedDecisions;
			{
				BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::DrainMaterializedDecisions");
				_bus->staticImportMaterializedPublishDecisions.Drain(
					drainedDecisions,
					static_cast<std::size_t>(_settings->maxMaterializedDecisionDrainPerFrame));
				for (auto& decision : drainedDecisions) {
					_importState->staticImportMaterializedDecisionBacklog.push_back(std::move(decision));
				}
				BT_PLOT("SARP.StaticDirect.MaterializedDecisionsDrained", static_cast<int64_t>(drainedDecisions.size()));
				BT_PLOT("SARP.StaticDirect.MaterializedDecisionBacklog", static_cast<int64_t>(_importState->staticImportMaterializedDecisionBacklog.size()));
			}
			std::list<StaticImportMaterializedPublishDecision> publishDecisions;
			std::list<StaticImportMaterializedPublishDecision> discardDecisions;
			StaticImportTransactionCost publishCost;
			// These are immutable worker-publication cut bounds, not render-frame
			// budgets. They bound cancellation latency and bridge bookkeeping while
			// allowing publication throughput to remain independent of frame time.
			const auto maximumPublicationCutGroups = static_cast<std::uint64_t>(
				_settings->maxPublicationCutGroups);
			const auto maximumPublicationCutBytes = static_cast<std::uint64_t>(
				_settings->maxPublicationCutBytes);
			const auto transactionCost = [](const MaterializedDirectStaticImportBatch& item) {
				StaticImportTransactionCost cost;
				cost.groups = item.transaction.reservation.groupCount;
				cost.bytes = item.transaction.reservation.preparedBytes;
				cost.drawRecords = item.transaction.reservation.drawRecords;
				cost.activeInsertIndices = item.batch.transactionCost.activeInsertIndices;
				return cost;
			};
			{
				BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::SelectMaterializedDecisionsForPublish");
				BT_ZONE_VALUE(static_cast<int64_t>(_importState->staticImportMaterializedDecisionBacklog.size()));
				std::size_t cancelDecisions = 0;
				std::size_t publishDecisionCount = 0;
				std::size_t revalidationRequests = 0;
				std::size_t budgetLeftovers = 0;
				std::size_t costFallbacks = 0;
				std::size_t budgetChecks = 0;
				while (!_importState->staticImportMaterializedDecisionBacklog.empty()) {
					// Always reconcile at least one completed transaction. Further
					// materialization/publication remains budgeted. This makes readiness
					// level-triggered and prevents an already-full completion queue from
					// depending on a future source mutation to make progress.
					const auto publishBegin = std::chrono::steady_clock::now();
					auto decisionIt = _importState->staticImportMaterializedDecisionBacklog.begin();
					auto& decision = *decisionIt;
					const auto requiredMaterialRevision =
						decision.item.batch.requiredMaterialRevision;
					{
						BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::SelectMaterializedDecisionsForPublish::PeekDecision");
						BT_ZONE_VALUE(static_cast<int64_t>(_importState->staticImportMaterializedDecisionBacklog.size()));
					}

					bool decisionStale = false;
					{
						BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::SelectMaterializedDecisionsForPublish::CheckDecisionFreshness");
						std::size_t staleDecisionGroups = 0;
						const bool coordinatorLivenessUnchanged =
							decision.livenessEpoch != 0 &&
							decision.livenessEpoch == _importState->staticImportCoordinatorLivenessEpoch.load(std::memory_order_acquire) &&
							decision.targetGeneration == _importState->staticImportServiceTargetGeneration.load(std::memory_order_acquire);
						if (!coordinatorLivenessUnchanged) {
							std::optional<StaticCellKey> lastCell;
							bool lastCellDesired = false;
							for (const auto& prepared : decision.item.batch.imports) {
								if (prepared.hostOwner) {
									if (!prepared.hostOwner->IsHostImportCurrent(target, prepared.cellKey, prepared.hostPayload)) {
										++staleDecisionGroups;
									}
									continue;
								}
								if (prepared.residencyClass == StaticImportResidencyClass::HostGroup) {
									if (!std::ranges::all_of(_extensions, [&](const auto* extension) {
											return extension->IsHostGroupCurrent(target, prepared.cellKey, prepared.residencyGeneration);
										})) {
										++staleDecisionGroups;
									}
								} else {
									if (!lastCell || !(*lastCell == prepared.cellKey)) {
										lastCell = prepared.cellKey;
										lastCellDesired = currentDesiredCells.contains(prepared.cellKey);
									}
									if (!lastCellDesired) {
										++staleDecisionGroups;
										continue;
									}
								}
							}
						}
						BT_PLOT("SARP.StaticDirect.MaterializedLivenessFastPath",
							coordinatorLivenessUnchanged ? int64_t{ 1 } : int64_t{ 0 });
						// Revision zero is the startup/fallback material state.  It has no
						// graph artifact to wait for and is therefore immediately usable.
						// Once a material root exists, reconcile the exact immutable version
						// captured by this transaction instead of polling the process-global
						// published revision.
						BT_PLOT("SARP.StaticGraph.RequiredMaterialRevision",
							static_cast<int64_t>(requiredMaterialRevision));
						decisionStale = decision.kind == StaticImportMaterializedDecisionKind::Publish &&
							staleDecisionGroups != 0;
						BT_PLOT("SARP.StaticDirect.MaterializedDecisionStaleGroups", static_cast<int64_t>(staleDecisionGroups));
						BT_PLOT("SARP.StaticDirect.MaterializedDecisionStale", decisionStale ? int64_t{ 1 } : int64_t{ 0 });
					}
					// Material tables are a cumulative latest-wins resource. An exact
					// version captured while preparing this immutable transaction may be
					// superseded before upload and can therefore never become ready. Do
					// not make host activation depend on that obsolete version. The
					// StaticScene publication root below already gates on the current
					// MaterialTable at UploadSubmitted, which is the coherent boundary
					// that prevents the renderer from observing missing material rows.

					if (decisionStale) {
						BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::RevalidateStaleMaterializedDecision");
						(void)_drains->PushBlocking(_bus->staticImportMaterializedValidationRequests, std::move(decision.item));
						discardDecisions.splice(
							discardDecisions.end(), _importState->staticImportMaterializedDecisionBacklog, decisionIt);
						_drains->ScheduleCoordinatorDrain();
						++revalidationRequests;
						continue;
					}
					if (decision.kind == StaticImportMaterializedDecisionKind::Cancel) {
						BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::CancelMaterializedDecision");
						BT_ZONE_VALUE(static_cast<int64_t>(decision.item.batch.groupIDs.empty()
							? decision.item.batch.imports.size()
							: decision.item.batch.groupIDs.size()));
						++cancelDecisions;
						++_counters->staticDirectImportBatchesStale;
						pendingOrSkipped += decision.item.batch.placementCount;
						{
							BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::CancelMaterializedDecision::CancelReservation");
							objectManager->CancelStaticImportTransaction(std::move(decision.item.transaction.reservation));
						}
						{
							BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::CancelMaterializedDecision::PushAck");
							for (auto& ack : decision.acks) {
								pushAck(std::move(ack));
							}
						}
						discardDecisions.splice(
							discardDecisions.end(), _importState->staticImportMaterializedDecisionBacklog, decisionIt);
						budget.ChargeElapsed(publishBegin);
						continue;
					}
					auto itemCost = decision.cost;
					{
						BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::SelectMaterializedDecisionsForPublish::ResolveDecisionCost");
						if (itemCost.groups == 0 &&
							itemCost.bytes == 0 &&
							itemCost.drawRecords == 0 &&
							itemCost.activeInsertIndices == 0) {
							++costFallbacks;
							itemCost = transactionCost(decision.item);
						}
					}
					BT_PLOT("SARP.StaticDirect.MaterializedItemGroups", static_cast<int64_t>(itemCost.groups));
					BT_PLOT("SARP.StaticDirect.MaterializedItemBytes", static_cast<int64_t>(itemCost.bytes));
					BT_PLOT("SARP.StaticDirect.MaterializedItemDrawRecords", static_cast<int64_t>(itemCost.drawRecords));
					BT_PLOT("SARP.StaticDirect.MaterializedItemActiveIndices", static_cast<int64_t>(itemCost.activeInsertIndices));
					bool publicationCutFull = false;
					{
						BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::SelectMaterializedDecisionsForPublish::CheckCutBounds");
						++budgetChecks;
						const auto candidateGroups = publishCost.groups + itemCost.groups;
						const auto candidateBytes = publishCost.bytes + itemCost.bytes;
						publicationCutFull = !publishDecisions.empty() &&
							(candidateGroups > maximumPublicationCutGroups ||
							 candidateBytes > maximumPublicationCutBytes);
					}
					if (publicationCutFull) {
						BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::MaterializedPublicationCutSplit");
						budgetLeftovers = _importState->staticImportMaterializedDecisionBacklog.size();
						break;
					}
					{
						BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::SelectMaterializedDecisionsForPublish::AcceptPublishDecision");
						publishCost.Add(itemCost);
						publishDecisions.splice(publishDecisions.end(), _importState->staticImportMaterializedDecisionBacklog, decisionIt);
					}
					++publishDecisionCount;
				}
				BT_PLOT("SARP.StaticDirect.MaterializedCancelDecisions", static_cast<int64_t>(cancelDecisions));
				BT_PLOT("SARP.StaticDirect.MaterializedPublishDecisions", static_cast<int64_t>(publishDecisionCount));
				BT_PLOT("SARP.StaticDirect.MaterializedRevalidationRequests", static_cast<int64_t>(revalidationRequests));
				BT_PLOT("SARP.StaticDirect.MaterializedDecisionCostFallbacks", static_cast<int64_t>(costFallbacks));
				BT_PLOT("SARP.StaticDirect.MaterializedDecisionBudgetChecks", static_cast<int64_t>(budgetChecks));
				BT_PLOT("SARP.StaticGraph.MaterializedDecisionsPending", static_cast<int64_t>(_importState->staticImportMaterialGraphPendingDecisions.size()));
				BT_PLOT("SARP.StaticDirect.MaterializedPublishSelected", static_cast<int64_t>(publishDecisions.size()));
				BT_PLOT("SARP.StaticDirect.MaterializedPublishLeftovers", static_cast<int64_t>(budgetLeftovers));
				BT_PLOT("SARP.StaticDirect.MaterializedPublishGroups", static_cast<int64_t>(publishCost.groups));
				BT_PLOT("SARP.StaticDirect.MaterializedPublishBytes", static_cast<int64_t>(publishCost.bytes));
				BT_PLOT("SARP.StaticDirect.MaterializedPublishDrawRecords", static_cast<int64_t>(publishCost.drawRecords));
				BT_PLOT("SARP.StaticDirect.MaterializedPublishActiveIndices", static_cast<int64_t>(publishCost.activeInsertIndices));
			}
			QueueMaterializedDecisionCleanup(discardDecisions);

			if (!publishDecisions.empty()) {
				StaticImportPublicationJob job;
				job.cost = publishCost;
				job.decisions = std::move(publishDecisions);
				(void)_drains->PushWorker(_bus->staticImportPublicationJobs, std::move(job),
					StaticImportWorkSource::Publication);
			}

			StaticImportPublicationResult submittedPublication;
			while (_bus->staticImportPublicationResults.TryPop(submittedPublication)) {
				auto pendingResult = std::make_shared<StaticImportPublicationResult>(
					std::move(submittedPublication));
				// Indirect publication needs the cut's immutable workload extents in
				// order to consume the exact DrawRecords root. This is dependency
				// declaration, not group visibility acknowledgement, and therefore
				// must precede the GPU-ready cut acceptance below.
				for (const auto& [workloadKey, span] :
					pendingResult->publication.activeDrawSetSpans) {
					_importState->directStaticWorkloadCounts[workloadKey] = span;
					if (dirtyWorkloadSpans) {
						(*dirtyWorkloadSpans)[workloadKey] = span;
					} else if (_shared->sceneIngestionServices.workloads) {
						_shared->sceneIngestionServices.workloads->PublishCount(workloadKey, span);
					}
				}
				PendingStaticImportPublication pending;
				pending.result = std::move(pendingResult);
				_importState->staticImportGpuPendingPublications.push_back(std::move(pending));
			}

			// The publication worker registered exact awaiters when it admitted the
			// cut (RegisterPublicationCutAwaiters); they flip gpuReady. A result
			// without a flag was never admitted through the graph and is ready now.
			// The update path never queries the state graph here.
			std::vector<std::shared_ptr<StaticImportPublicationResult>> readyPublications;
			auto retiredAwaiters = std::make_shared<std::vector<br::render::ArtifactAwaiter>>();
			// Publication does not wait for a cut to become GPU-ready before
			// publishing the next, so several can be pending here. Commit strictly
			// in publication order: each result carries absolute workload spans, and
			// committing an older cut after a newer one would regress them.
			for (auto pendingIt = _importState->staticImportGpuPendingPublications.begin();
				pendingIt != _importState->staticImportGpuPendingPublications.end();) {
				const auto& result = pendingIt->result;
				const bool ready = !result || !result->gpuReady ||
					result->gpuReady->load(std::memory_order_acquire);
				if (!ready) break;
				if (result) {
					for (auto& awaiter : result->awaiters) retiredAwaiters->push_back(std::move(awaiter));
					result->awaiters.clear();
					readyPublications.push_back(result);
				}
				pendingIt = _importState->staticImportGpuPendingPublications.erase(pendingIt);
			}
			if (!retiredAwaiters->empty()) {
				// Unsubscribing an exact awaiter enters the graph mutex; never on this thread.
				if (!TaskSchedulerManager::GetInstance().Submit(_shared->staticStreamingScope,
					TaskLane::Background, TaskDomain::Cleanup, "SARP::StaticImport::ReleaseCutAwaiters",
					[retiredAwaiters](const br::TaskContext&) { retiredAwaiters->clear(); })) {
					retiredAwaiters->clear();
				}
			}
			for (const auto& readyPublicationPtr : readyPublications) {
				auto& readyPublication = *readyPublicationPtr;
				auto publishDecisions = std::move(readyPublication.decisions);
				auto publishResult = std::move(readyPublication.publication);
				const auto publishCost = readyPublication.cost;
				_counters->staticDirectImportUs += readyPublication.publishUs;
				_counters->staticDirectPreparedBytes += publishResult.preparedBytes;
				// The journals are the authoritative mutable mailbox. Renderer::Update
				// drains them once at the GPU-visible snapshot boundary after all bridge
				// batches for this frame have landed. Publishing here minted an object
				// graph generation for every host import slice and defeated coalescing.
				{
					BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::AccumulatePublishStats");
					std::size_t publishResultRemovalPayloads = 0;
					for (const auto& record : publishResult.transactions) {
						publishResultRemovalPayloads += record.removalPayloads.size();
					}
					BT_PLOT("SARP.StaticDirect.PublishResultTransactions", static_cast<int64_t>(publishResult.transactions.size()));
					BT_PLOT("SARP.StaticDirect.PublishResultRemovalPayloads", static_cast<int64_t>(publishResultRemovalPayloads));
					BT_PLOT("SARP.StaticDirect.PublishResultPreparedBytes", static_cast<int64_t>(publishResult.preparedBytes));
					for (const auto& decision : publishDecisions) {
						_counters->staticDirectPreparedImportUs += decision.item.batch.prepareUs;
					}
				}

				{
					BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::RecordWorkloadSpans");
					BT_ZONE_VALUE(static_cast<int64_t>(publishResult.activeDrawSetSpans.size()));
					for (auto& [workloadKey, span] : publishResult.activeDrawSetSpans) {
						_importState->directStaticWorkloadCounts[workloadKey] = span;
						if (dirtyWorkloadSpans) {
							(*dirtyWorkloadSpans)[workloadKey] = span;
						} else if (_shared->sceneIngestionServices.workloads) {
							_shared->sceneIngestionServices.workloads->PublishCount(workloadKey, span);
						}
					}
				}

				{
					BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::CommitBridgeRecords");
					BT_ZONE_VALUE(static_cast<int64_t>(publishDecisions.size()));
					for (auto& ack : readyPublication.bridgeAcks) {
						QueueStaticImportGroupAckForCoordinator(std::move(ack));
					}
					std::size_t committedGroups = 0;
					std::size_t droppedGroups = 0;
					std::size_t committedDrawInfos = 0;
					std::size_t transactionCursor = 0;
					std::vector<StaticImportGroupAck> publishAcks;
					publishAcks.reserve(publishDecisions.size());
					for (auto& decision : publishDecisions) {
						BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::CommitBridgeRecords::Decision");
						auto& item = decision.item;
						auto* publishRecord = transactionCursor < publishResult.transactions.size()
							? std::addressof(publishResult.transactions[transactionCursor])
							: nullptr;
						++transactionCursor;
						const bool hasBridgeSpecial = std::ranges::any_of(
							item.batch.imports, [](const PreparedDirectStaticImport& prepared) {
								return prepared.hostOwner != nullptr ||
									prepared.residencyClass == StaticImportResidencyClass::HostGroup;
							});
						if (!hasBridgeSpecial) continue;
						std::size_t removalPayloadCursor = 0;
						StaticImportGroupAck publishAck;
						publishAck.ticketID = item.batch.publishTicketID;
						const auto transactionCut = std::ranges::find(
							readyPublication.transactionCuts, item.batch.publishTicketID,
							&StaticImportPublicationResult::TransactionCut::ticketID);
						publishAck.dropped.reserve(item.batch.imports.size());
						for (auto& prepared : item.batch.imports) {
							auto& ready = prepared.ready;
							const auto drawInfoCount = prepared.submittedGroupCount;
							br::render::StaticObjectRemovalPayload removalPayload;
							std::shared_ptr<const StaticGraphGroupOwnership> graphOwnership;
							if (transactionCut != readyPublication.transactionCuts.end() &&
								transactionCut->input) {
								const auto groupIt = std::ranges::find(
									transactionCut->input->groups, prepared.groupID,
									&br::render::StaticTransactionGroup::groupID);
								if (groupIt != transactionCut->input->groups.end() && groupIt->ownership) {
									graphOwnership = std::static_pointer_cast<const StaticGraphGroupOwnership>(
										groupIt->ownership);
								}
							}
							if (graphOwnership && publishRecord) {
								removalPayloadCursor = (std::min)(
									publishRecord->removalPayloads.size(),
									removalPayloadCursor + drawInfoCount);
							}
							if (!graphOwnership && publishRecord) {
								for (std::size_t i = 0; i < drawInfoCount && removalPayloadCursor < publishRecord->removalPayloads.size(); ++i, ++removalPayloadCursor) {
									auto& groupPayload = publishRecord->removalPayloads[removalPayloadCursor];
									if (!groupPayload.drawRecordIndices.empty()) {
										AppendStaticObjectRemovalPayload(removalPayload, groupPayload);
									}
								}
							}
							const auto removalOwnership = graphOwnership
								? graphOwnership->removalPayload
								: (removalPayload.drawRecordIndices.empty()
									? std::shared_ptr<const br::render::StaticObjectRemovalPayload>{}
									: std::make_shared<const br::render::StaticObjectRemovalPayload>(
										std::move(removalPayload)));
							if (!removalOwnership || removalOwnership->drawRecordIndices.empty()) {
								++_counters->staticDirectImportBatchesDropped;
								++droppedGroups;
								pendingOrSkipped += prepared.placementCount;
								continue;
							}
							committedDrawInfos += removalOwnership->drawInfoCount;

							if (prepared.residencyClass == StaticImportResidencyClass::HostGroup) {
								removalPayload = *removalOwnership;
								br::staticimport::StaticHostGroupPublication publication{
									.groupID = prepared.groupID,
									.residencyGeneration = prepared.residencyGeneration,
									.destructionPayload = removalPayload,
									.workloadKeys = ready.sceneObject.directWorkloadKeys,
									.placementCount = ready.sceneObject.instanceCount,
								};
								auto publishResult = br::staticimport::StaticHostGroupPublishResult::NotOwned;
								for (auto* extension : _extensions) {
									publishResult = extension->PublishHostGroup(publication, currentDesiredCells);
									if (publishResult != br::staticimport::StaticHostGroupPublishResult::NotOwned) break;
								}
								if (publishResult != br::staticimport::StaticHostGroupPublishResult::Accepted) {
									_importState->staticImportRemovalPayloadCleanupBacklog.push_back(std::move(removalPayload));
									_shared->staticImportWorkScheduler.Signal(StaticImportWorkSource::RemovalCleanup);
									_drains->ScheduleWorkerDrains();
									publishAck.dropped.push_back(prepared.groupID);
									continue;
								}
								if (auto* requests = _shared->hasSceneIngestionServices
									? _shared->sceneIngestionServices.execution.stateRequests : nullptr;
									requests && requests->TraceActive()) {
									requests->TraceEvent(br::render::AsyncStateGraphTraceEventID::StaticGroupBridgeApplied,
										{ br::render::ArtifactKind::StaticGroup, prepared.groupID, 0 },
										prepared.generation, 0,
										{ { prepared.placementCount, item.batch.publishTicketID, 1 } });
								}
								++_counters->staticDirectGroupsImported;
								++_counters->lastStaticGroupsImportedThisFrame;
								++_counters->staticDirectImportBatchesCommitted;
								++committedGroups;
								continue;
							}

							if (prepared.hostOwner) {
								removalPayload = *removalOwnership;
								br::staticimport::StaticHostImportPublication publication{
									.cell = prepared.cellKey,
									.groupID = prepared.groupID,
									.payload = prepared.hostPayload,
									.destructionPayload = removalPayload,
									.workloadKeys = ready.sceneObject.directWorkloadKeys,
								};
								prepared.hostOwner->PublishHostImport(publication);
								applied += prepared.placementCount;
								++upsertsThisFrame;
								++committedGroups;
								continue;
							}

							// Normal groups are never reached: they are committed by immutable
							// StaticScene ownership deltas on the graph-worker path.
						}
						if (!publishAck.requeue.empty() ||
							!publishAck.dropped.empty()) {
							publishAcks.push_back(std::move(publishAck));
						}
					}
					{
						BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::CommitBridgeRecords::PushAcks");
						BT_ZONE_VALUE(static_cast<int64_t>(publishAcks.size()));
						for (auto& ack : publishAcks) {
							QueueStaticImportGroupAckForCoordinator(std::move(ack));
						}
					}
					BT_PLOT("SARP.StaticDirect.CommitBridgeGroups", static_cast<int64_t>(committedGroups));
					BT_PLOT("SARP.StaticDirect.CommitBridgeDroppedGroups", static_cast<int64_t>(droppedGroups));
					BT_PLOT("SARP.StaticDirect.CommitBridgeDrawInfos", static_cast<int64_t>(committedDrawInfos));
				}
				{
					BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::RecordAsyncPublishCost");
					const auto publishElapsedUs = readyPublication.publishUs;
					const auto measuredUsPerGroup = static_cast<double>(publishElapsedUs) /
						static_cast<double>((std::max<std::uint64_t>)(publishCost.groups, 1u));
					// Follow increases immediately, but decay slowly so a single fast
					// batch does not recreate a large whole-frame outlier on the next pass.
					_counters->staticDirectPublishUsPerGroupEstimate = (std::max)(
						measuredUsPerGroup,
						_counters->staticDirectPublishUsPerGroupEstimate * 0.875);
					// CPU publication completed on StaticImport workers; bridge bookkeeping
					// must not charge that elapsed time to the render-thread budget.
					BT_PLOT("SARP.StaticDirect.AsyncPublishUs", static_cast<int64_t>(publishElapsedUs));
					BT_PLOT("SARP.StaticDirect.PublishUsPerGroupEstimate",
						static_cast<int64_t>(std::ceil(_counters->staticDirectPublishUsPerGroupEstimate)));
				}
				QueueMaterializedDecisionCleanup(publishDecisions);
			}
		}
		if (budget.TotalExhausted()) {
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::ExitBudgetExhaustedBeforeReservation");
			return;
		}

		{
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::CompletedBacklogStage");
			std::vector<PreparedDirectStaticImportBatch> drained;
			{
				BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::DrainCompletedBatches");
				const auto maxCompletedBatchDrain = static_cast<std::size_t>(
					_settings->maxCompletedBatchDrainPerFrame);
				const auto completedDepthBeforeDrain = _bus->staticImportCompletedOutput.Size();
				_bus->staticImportCompletedOutput.Drain(drained, maxCompletedBatchDrain);
				BT_PLOT("SARP.StaticDirect.CompletedOutputDepthBeforeDrain", static_cast<int64_t>(completedDepthBeforeDrain));
				BT_PLOT("SARP.StaticDirect.CompletedBatchesDrained", static_cast<int64_t>(drained.size()));
			}
			{
				BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::AppendResourcePendingBacklog");
				BT_ZONE_VALUE(static_cast<int64_t>(drained.size()));
				for (auto& batch : drained) {
					_importState->staticImportResourcePendingBatchBacklog.push_back(std::move(batch));
				}
				BT_PLOT("SARP.StaticDirect.ResourcePendingBacklogDepth", static_cast<int64_t>(_importState->staticImportResourcePendingBatchBacklog.size()));
			}
		}

		std::size_t reservedThisFrame = 0;
		std::vector<PreparedDirectStaticImportBatch> reservationBatches;
		StaticImportTransactionCost reservationCost;
		const auto batchCost = [](const PreparedDirectStaticImportBatch& batch) {
			auto cost = batch.transactionCost;
			if (cost.groups == 0 &&
				cost.bytes == 0 &&
				cost.drawRecords == 0 &&
				cost.activeInsertIndices == 0) {
				const auto& build = batch.transactionBuild;
				cost.groups = build.prepared.groups.size();
				cost.bytes = build.preparedBytes != 0 ? build.preparedBytes : build.prepared.preparedBytes;
				cost.drawRecords = build.drawRecords != 0 ? build.drawRecords : build.prepared.drawRecords;
				cost.activeInsertIndices = build.activeInsertIndices;
			}
			return cost;
		};
		const auto wouldExceedReservationFrameBudget = [this](const StaticImportTransactionCost& current, const StaticImportTransactionCost& next) {
			const auto maxGroups = static_cast<std::uint64_t>(_settings->maxImportGroupsPerFrame);
			const auto maxBytes = static_cast<std::uint64_t>(_settings->maxImportPublishBytesPerFrame);
			const auto maxDrawRecords = static_cast<std::uint64_t>(_settings->maxImportDrawRecordsPerFrame);
			const auto maxActiveInsertIndices = static_cast<std::uint64_t>(_settings->maxActiveInsertIndicesPerFrame);
			return (maxGroups != 0 && current.groups + next.groups > maxGroups) ||
				(maxBytes != 0 && current.bytes + next.bytes > maxBytes) ||
				(maxDrawRecords != 0 && current.drawRecords + next.drawRecords > maxDrawRecords) ||
				(maxActiveInsertIndices != 0 && current.activeInsertIndices + next.activeInsertIndices > maxActiveInsertIndices);
		};
		const auto makeBatchAcks = [](const PreparedDirectStaticImportBatch& batch, bool requeue) {
			std::vector<StaticImportGroupAck> acks;
			acks.reserve(batch.imports.size());
			for (const auto& prepared : batch.imports) {
				auto found = std::ranges::find(
					acks, prepared.admissionTicketID,
					&StaticImportGroupAck::ticketID);
				if (found == acks.end()) {
					found = acks.insert(acks.end(), {});
					found->ticketID = prepared.admissionTicketID;
				}
				auto& dst = requeue ? found->requeue : found->dropped;
				dst.push_back(prepared.groupID);
			}
			return acks;
		};
		{
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::ResourceGateAndSelectForReservation");
			const auto selectionBegin = std::chrono::steady_clock::now();
			BT_PLOT("SARP.StaticDirect.ResourcePendingBacklogBeforeProbe", static_cast<int64_t>(_importState->staticImportResourcePendingBatchBacklog.size()));
			BT_PLOT("SARP.StaticDirect.ReadyBacklogBeforeSelection", int64_t{ 0 });
			std::size_t maxSelectionBatches = 0;
			std::size_t maxSelectionInspections = 0;
			std::size_t reserveHint = 0;
			std::size_t pendingBacklogAtStart = 0;
			{
				BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::PrepareReservationSelection");
				maxSelectionBatches = static_cast<std::size_t>(_settings->maxReadyReservationBatchesPerFrame);
				maxSelectionInspections = maxSelectionBatches == 0
					? std::size_t{ 0 }
					: (std::max<std::size_t>)(maxSelectionBatches, maxSelectionBatches * 4u);
				pendingBacklogAtStart = _importState->staticImportResourcePendingBatchBacklog.size();
				reserveHint = maxSelectionBatches == 0
					? pendingBacklogAtStart
					: (std::min)(pendingBacklogAtStart, maxSelectionBatches);
				reservationBatches.reserve(reserveHint);
			}
			std::size_t staleReadyBatches = 0;
			std::size_t budgetSplitReadyBatches = 0;
			std::size_t unfinalizedReadyBatches = 0;
			std::size_t selectedBatchLimitSplits = 0;
			std::size_t inspectedBatchLimitSplits = 0;
			std::size_t selectionIterations = 0;
			std::size_t costFallbacks = 0;
			std::size_t resourceReadyBatches = 0;
			std::size_t resourcePendingBatches = 0;
			std::size_t resourceEmptyBatches = 0;
			std::size_t pendingNormalMatrix = 0;
			std::size_t pendingPerObject = 0;
			std::size_t pendingInstanceTransform = 0;
			std::size_t pendingDrawRecord = 0;
			// The probe is created only once a batch survives trimming, so frames that
			// merely drop stale batches never touch the allocators.
			std::optional<br::render::StaticImportResourceProbe> resourceProbe;
			const bool probeAllowed = pendingBacklogAtStart != 0 && !budget.TotalExhausted();
			while (probeAllowed &&
				!_importState->staticImportResourcePendingBatchBacklog.empty() &&
				selectionIterations < pendingBacklogAtStart &&
				!budget.TotalExhausted()) {
				if (maxSelectionInspections != 0 && selectionIterations >= maxSelectionInspections) {
					BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::ReservationInspectionLimitSplit");
					++inspectedBatchLimitSplits;
					break;
				}
				if (maxSelectionBatches != 0 && reservationBatches.size() >= maxSelectionBatches) {
					BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::ReservationBatchLimitSplit");
					++selectedBatchLimitSplits;
					break;
				}
				++selectionIterations;
				PreparedDirectStaticImportBatch batch = [&]() {
					BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::PopPendingResourceBatch");
					auto batch = std::move(_importState->staticImportResourcePendingBatchBacklog.front());
					_importState->staticImportResourcePendingBatchBacklog.pop_front();
					return batch;
				}();
				bool staleBatch = false;
				{
					BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::SelectReadyBatchesForReservation::TrimToDesiredCells");
					std::vector<StaticImportGroupAck> droppedAcks;
					const auto currentGeneration = _importState->staticImportServiceTargetGeneration.load(std::memory_order_acquire);
					staleBatch = !TrimPreparedBatchToDesiredCells(
						batch,
						currentDesiredCells,
						target,
						currentGeneration,
						std::addressof(droppedAcks));
					for (auto& droppedAck : droppedAcks) {
						pushAck(std::move(droppedAck));
					}
				}
				if (staleBatch) {
					BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::DropUndesiredReadyBatch");
					const auto skippedPlacements = batch.placementCount;
					++staleReadyBatches;
					++_counters->staticDirectImportBatchesStale;
					pendingOrSkipped += skippedPlacements;
					{
						BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::DestroyUndesiredReadyBatch");
						batch = {};
					}
					continue;
				}
				bool emptyBatch = false;
				{
					BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::CheckEmptyResourceBatch");
					emptyBatch = batch.imports.empty() || batch.transactionBuild.prepared.groups.empty();
				}
				if (emptyBatch) {
					BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::DropEmptyReadyBatch");
					++staleReadyBatches;
					++resourceEmptyBatches;
					++_counters->staticDirectImportBatchesStale;
					{
						BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::DropEmptyReadyBatch::PushAck");
						for (auto& ack : makeBatchAcks(batch, false))
							pushAck(std::move(ack));
					}
					{
						BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::DestroyEmptyReadyBatch");
						batch = {};
					}
					continue;
				}

				if (!batch.transactionBuild.finalized) {
					BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::UnfinalizedReadyBatch");
					++unfinalizedReadyBatches;
				}
				br::render::StaticImportResourceProbeStatus probeStatus = br::render::StaticImportResourceProbeStatus::Ready;
				{
					if (!resourceProbe) {
						BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::CreateResourceProbe");
						resourceProbe.emplace(objectManager->CreateStaticImportResourceProbe());
					}
					BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::ProbeResourceBatch");
					probeStatus = objectManager->ProbeStaticImportTransactionResources(batch.transactionBuild, *resourceProbe);
				}
				if (probeStatus != br::render::StaticImportResourceProbeStatus::Ready) {
					switch (probeStatus) {
					case br::render::StaticImportResourceProbeStatus::PendingNormalMatrix:
						++resourcePendingBatches;
						++pendingNormalMatrix;
						break;
					case br::render::StaticImportResourceProbeStatus::PendingPerObject:
						++resourcePendingBatches;
						++pendingPerObject;
						break;
					case br::render::StaticImportResourceProbeStatus::PendingInstanceTransform:
						++resourcePendingBatches;
						++pendingInstanceTransform;
						break;
					case br::render::StaticImportResourceProbeStatus::PendingDrawRecord:
						++resourcePendingBatches;
						++pendingDrawRecord;
						break;
					case br::render::StaticImportResourceProbeStatus::Empty:
						++resourceEmptyBatches;
						for (auto& ack : makeBatchAcks(batch, false))
							pushAck(std::move(ack));
						continue;
					case br::render::StaticImportResourceProbeStatus::Ready:
						break;
					}
					{
						BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::RequeuePendingResourceBatch");
						_importState->staticImportResourcePendingBatchBacklog.push_back(std::move(batch));
					}
					continue;
				}
				++resourceReadyBatches;
				StaticImportTransactionCost cost;
				{
					BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::SelectReadyBatchesForReservation::ResolveCost");
					cost = batchCost(batch);
					if (batch.transactionCost.groups == 0 &&
						batch.transactionCost.bytes == 0 &&
						batch.transactionCost.drawRecords == 0 &&
						batch.transactionCost.activeInsertIndices == 0) {
						++costFallbacks;
					}
				}
				{
					BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::SelectReadyBatchesForReservation::CheckFrameBudget");
					if (!reservationBatches.empty() && wouldExceedReservationFrameBudget(reservationCost, cost)) {
						BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::ReservationFrameBudgetSplit");
						++budgetSplitReadyBatches;
						_importState->staticImportResourcePendingBatchBacklog.push_front(std::move(batch));
						break;
					}
				}
				{
					BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::AccumulateReservationCost");
					reservationCost.Add(cost);
				}
				{
					BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::SelectReadyBatchesForReservation::AcceptBatch");
					reservationBatches.push_back(std::move(batch));
				}
			}
			if (resourceProbe) {
				BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::DestroyResourceProbe");
				resourceProbe.reset();
			}
			const auto selectionUs = ElapsedUs(selectionBegin);
			{
				BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::PublishReservationSelectionTelemetry");
				BT_PLOT("SARP.StaticDirect.ReadyBatchesStale", static_cast<int64_t>(staleReadyBatches));
				BT_PLOT("SARP.StaticDirect.ReadyBatchesBudgetSplit", static_cast<int64_t>(budgetSplitReadyBatches));
				BT_PLOT("SARP.StaticDirect.ReadyBatchesUnfinalized", static_cast<int64_t>(unfinalizedReadyBatches));
				BT_PLOT("SARP.StaticDirect.ReadyBatchesSelectionBatchLimitSplits", static_cast<int64_t>(selectedBatchLimitSplits));
				BT_PLOT("SARP.StaticDirect.ReadyBatchesSelectionInspectionLimitSplits", static_cast<int64_t>(inspectedBatchLimitSplits));
				BT_PLOT("SARP.StaticDirect.ReadyBatchSelectionIterations", static_cast<int64_t>(selectionIterations));
				BT_PLOT("SARP.StaticDirect.ReadyBatchSelectionCostFallbacks", static_cast<int64_t>(costFallbacks));
				BT_PLOT("SARP.StaticDirect.ReadyBatchSelectionUs", static_cast<int64_t>(selectionUs));
				BT_PLOT("SARP.StaticDirect.ResourceProbeInspected", static_cast<int64_t>(selectionIterations));
				BT_PLOT("SARP.StaticDirect.ResourceProbeReady", static_cast<int64_t>(resourceReadyBatches));
				BT_PLOT("SARP.StaticDirect.ResourceProbePending", static_cast<int64_t>(resourcePendingBatches));
				BT_PLOT("SARP.StaticDirect.ResourceProbeStale", static_cast<int64_t>(staleReadyBatches));
				BT_PLOT("SARP.StaticDirect.ResourceProbeEmpty", static_cast<int64_t>(resourceEmptyBatches));
				BT_PLOT("SARP.StaticDirect.ResourceProbePendingNormalMatrix", static_cast<int64_t>(pendingNormalMatrix));
				BT_PLOT("SARP.StaticDirect.ResourceProbePendingPerObject", static_cast<int64_t>(pendingPerObject));
				BT_PLOT("SARP.StaticDirect.ResourceProbePendingInstanceTransform", static_cast<int64_t>(pendingInstanceTransform));
				BT_PLOT("SARP.StaticDirect.ResourceProbePendingDrawRecord", static_cast<int64_t>(pendingDrawRecord));
				BT_PLOT("SARP.StaticDirect.ReservationSelectedBatches", static_cast<int64_t>(reservationBatches.size()));
				BT_PLOT("SARP.StaticDirect.ReservationSelectedGroups", static_cast<int64_t>(reservationCost.groups));
				BT_PLOT("SARP.StaticDirect.ReservationSelectedBytes", static_cast<int64_t>(reservationCost.bytes));
				BT_PLOT("SARP.StaticDirect.ReservationSelectedDrawRecords", static_cast<int64_t>(reservationCost.drawRecords));
				BT_PLOT("SARP.StaticDirect.ReservationSelectedActiveIndices", static_cast<int64_t>(reservationCost.activeInsertIndices));
				BT_PLOT("SARP.StaticDirect.ResourcePendingBacklogAfterProbe", static_cast<int64_t>(_importState->staticImportResourcePendingBatchBacklog.size()));
				BT_PLOT("SARP.StaticDirect.ReadyBacklogAfterSelection", int64_t{ 0 });
			}
		}

		// Once a completed batch has been detached from the pending-resource
		// backlog, it must leave this scope through exactly one ownership path:
		// reservation, explicit requeue, or acknowledgement.  The old second
		// budget gate silently destroyed the selected vector when selection
		// itself consumed the remaining frame budget, leaving coordinator
		// groups permanently marked ready with no queue capable of waking them.
		if (!reservationBatches.empty() && budget.TotalExhausted()) {
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::RequeueSelectionAfterBudgetExhaustion");
			for (auto it = reservationBatches.rbegin(); it != reservationBatches.rend(); ++it) {
				_importState->staticImportResourcePendingBatchBacklog.push_front(std::move(*it));
			}
			reservationBatches.clear();
		}

		if (!reservationBatches.empty()) {
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::QueueTransactionPreparation");
			BT_ZONE_VALUE(static_cast<int64_t>(reservationBatches.size()));
			const auto reserveBegin = std::chrono::steady_clock::now();

			{
				BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::QueueTransactionPreparationJobs");
				for (std::size_t i = 0; i < reservationBatches.size(); ++i) {
					StaticImportTransactionPrepareJob materializeJob;
					materializeJob.batch = std::move(reservationBatches[i]);
					if (QueueStaticImportTransactionMaterializeJob(std::move(materializeJob))) {
						++reservedThisFrame;
						++_counters->staticDirectImportBatchesCompleted;
					}
				}
				BT_PLOT("SARP.StaticDirect.TransactionPrepareJobsQueued", static_cast<int64_t>(reservedThisFrame));
			}
			{
				BT_ZONE_SCOPE("SARP::LiveSceneBridge::DrainReadyDiffs::ChargeReservationBudget");
				budget.ChargeElapsed(reserveBegin);
				BT_PLOT("SARP.StaticDirect.ReservationBudgetUsedUs", static_cast<int64_t>(budget.ElapsedUs()));
			}
		}
		BT_PLOT("SARP.StaticDirect.TransactionReservedThisFrame", static_cast<int64_t>(reservedThisFrame));
	}

	void StaticImportStreamingPump::LogStaticStreamStatus(const br::staticimport::StaticStreamTarget& target)
	{
		const auto now = std::chrono::steady_clock::now();
		if ((now - _importState->lastStaticStreamLog) >= std::chrono::seconds(2)) {
			_importState->lastStaticStreamLog = now;
			const auto objectStats = CaptureObjectManagerStats();
			const auto serviceStats = GetStatsSnapshot();
			const auto hostObjects = _hooks.hostObjectCounters
				? _hooks.hostObjectCounters() : StaticHostObjectCounters{};
			const auto publicationWork = _shared->staticImportWorkScheduler.Snapshot(
				StaticImportWorkSource::Publication);
			spdlog::info(
				"SARP static ownership: completed={} resource_backlog={} materialize_jobs={} materialize_backlog={} validation={} decisions={} decision_backlog={} graph_pending={} group_acks={} group_ack_backlog={} group_results={} template_requests={} transaction_ready(q={} events={} groups={} accepted={} rejected={}) publication(admission_pending={} required_coverage={} observed_coverage={} result_q={} gpu_ready_q={} gpu_pending={} work_published={} work_claimed={} work_active={})",
				_bus->staticImportCompletedOutput.Size(),
				_importState->staticImportResourcePendingBatchBacklog.size(),
				_bus->staticImportTransactionMaterializeJobs.Size(),
				_importState->staticImportTransactionMaterializeBacklog.size(),
				_bus->staticImportMaterializedValidationRequests.Size(),
				_bus->staticImportMaterializedPublishDecisions.Size(),
				_importState->staticImportMaterializedDecisionBacklog.size(),
				_importState->staticImportMaterialGraphPendingDecisions.size(),
				_bus->staticImportGroupAcks.Size(),
				_importState->staticImportGroupAckBacklog.size(),
				_shared->graphCompletions->StaticGroupCount(),
				_bus->staticImportTemplateRequestOutput.Size(),
				_bus->staticTransactionReadyEvents.Size(),
				_importState->staticTransactionReadyEventsPublished.load(std::memory_order_relaxed),
				_importState->staticTransactionReadyGroupsPublished.load(std::memory_order_relaxed),
				_importState->staticTransactionReadyEventsAccepted.load(std::memory_order_relaxed),
				_importState->staticTransactionReadyEventsRejected.load(std::memory_order_relaxed),
				_importState->staticImportPublicationAdmissionPending.load(std::memory_order_acquire),
				_importState->staticImportPublicationRequiredCoverage.load(std::memory_order_acquire),
				_importState->staticImportPublicationObservedCoverage.load(std::memory_order_acquire),
				_bus->staticImportPublicationResults.Size(),
				_bus->staticImportGpuReadyPublicationResults.Size(),
				_importState->staticImportGpuPendingPublications.size(),
				publicationWork.published,
				publicationWork.claimed,
				publicationWork.active);
			spdlog::info(
				"SARP static streamer: world={:08X} cell=({}, {}) interior={:08X} radius={} active_cells={} entered={} left={} placements={} applied={} pending_or_skipped={} live_static_objects={} pending_static={} queues(prep={} import={} remove={} max_import={} max_remove={}) svc(pending_groups={} pending_places={} ready_tracked={} ready_q={} materialize_q={} validation_q={} backlog={} acks={} prepared={} wait_asset={} wait_variant={} wait_template={} wait_shader={} requeued={} ack_pub={} ack_req={} ack_drop={} stale={} unsupported={}) budget(last_used={}us last_over={}us exhausted_frames={} max_over={}us) frame_work(prepared_cells={} imported_groups={} removed_cells={}) prep_cache(hit={} miss={}) bg(queued={} completed={} stale={} integrated={}) perf_ms(index={} update={} same_cell={} diff={} visit={} remove={} leave={} enter={} prepare={} prune={} pump={} retry={} resolve={} matrix={} object_create={} entity_create={} direct_build={} direct_import={} direct_template={} transform={} asset_lookup={} object_calls={} entity_calls={}) om_ms(transform={} page_upload={} workload={} draw_build={} draw_upload={} active_insert={} finalize={} remove_collect={} remove_pages={} active_remove={} bulk_remove={}) om_packet_ms(resize={} scope={} normal_patch={} packet_build={} packet_publish={}) om_counts(insert_calls={} insert_indices={} remove_calls={} remove_indices={} bulk_remove_calls={} bulk_remove_objects={}) om_compact(queued={} built={} published={} stale={} input={} output={} worker_ms={} publish_ms={}) om_retire(queue={} queued={} retired={} queued_mb={} retired_mb={} worker_ms={}) direct(groups_submitted={} groups_imported={} groups_removed={} templates_created={} templates_reused={} transforms={} draw_records={})",
				target.worldFormID,
				target.cellX,
				target.cellY,
				target.interiorCellFormID,
				_settings->cellRadius,
				_counters->lastStaticActiveCells,
				_counters->lastStaticCellsEntered,
				_counters->lastStaticCellsLeft,
				_counters->lastStaticVisitedPlacements,
				_counters->lastStaticAppliedPlacements,
				_counters->lastStaticPendingOrSkippedPlacements,
				_hooks.liveStaticObjectCount(),
				serviceStats.pendingPlacements,
				StaticPreparationQueueDepth(),
				serviceStats.pendingGroups,
				serviceStats.removalDiffsQueued,
				_counters->staticStreamMaxImportQueueDepth,
				_counters->staticStreamMaxRemovalQueueDepth,
				serviceStats.pendingGroups,
				serviceStats.pendingPlacements,
				serviceStats.readyGroupsTracked,
				serviceStats.readyGroupsQueued,
				_bus->staticImportTransactionMaterializeJobs.Size(),
				_bus->staticImportMaterializedValidationRequests.Size(),
				_importState->staticImportResourcePendingBatchBacklog.size(),
				_bus->staticImportGroupAcks.Size(),
				serviceStats.groupsPrepared,
				serviceStats.groupsWaitingAssets,
				serviceStats.groupsWaitingVariants,
				serviceStats.groupsWaitingTemplates,
				serviceStats.groupsWaitingShaderVariants,
				serviceStats.groupsRequeued,
				serviceStats.groupsAckPublished,
				serviceStats.groupsAckRequeued,
				serviceStats.groupsAckDropped,
				serviceStats.staleGroupsDropped,
				serviceStats.unsupportedGroupsDropped,
				_counters->lastStaticStreamBudgetUsedUs,
				_counters->lastStaticStreamBudgetOverrunUs,
				_counters->staticStreamBudgetExhaustedFrames,
				_counters->staticStreamMaxBudgetOverrunUs,
				_counters->lastStaticCellsPreparedThisFrame,
				_counters->lastStaticGroupsImportedThisFrame,
				_counters->lastStaticCellsRemovedThisFrame,
				0,
				0,
				_bus->staticImportCellPrepareJobs.Size(),
				_bus->staticImportPreparedCellResults.Size(),
				serviceStats.staleGroupsDropped,
				serviceStats.cellsPrepared,
				_counters->staticIndexLoadMs,
				UsToMs(_counters->staticStreamUpdateUs),
				UsToMs(_counters->staticSameCellFastPathUs),
				UsToMs(_counters->staticCellDiffUs),
				UsToMs(_counters->staticStreamVisitUs),
				UsToMs(_counters->staticStreamRemoveUs),
				UsToMs(_counters->staticCellLeaveUs),
				UsToMs(_counters->staticCellEnterUs),
				UsToMs(_counters->staticCellPrepareUs),
				UsToMs(_counters->staticStreamPruneUs),
				UsToMs(_counters->staticStreamPumpUs),
				UsToMs(_counters->staticPendingAssetRetryUs),
				UsToMs(_counters->staticPlacementResolveUs),
				UsToMs(_counters->staticPlacementMatrixUs),
				UsToMs(_counters->staticObjectCreateUs),
				UsToMs(hostObjects.entityCreateUs),
				UsToMs(_counters->staticDirectBuildUs),
				UsToMs(_counters->staticDirectImportUs),
				UsToMs(_counters->staticDirectTemplateUs),
				UsToMs(_counters->staticTransformUs),
				UsToMs(hostObjects.assetLookupUs),
				hostObjects.objectCreateCalls,
				hostObjects.entityCreateCalls,
				UsToMs(objectStats.staticDirectTransformBuildUs),
				UsToMs(objectStats.staticDirectPageUploadUs),
				UsToMs(objectStats.staticDirectWorkloadBuildUs),
				UsToMs(objectStats.staticDirectDrawRecordBuildUs),
				UsToMs(objectStats.staticDirectDrawRecordUploadUs),
				UsToMs(objectStats.activeDrawSetInsertUs),
				UsToMs(objectStats.staticDirectFinalizeUs),
				UsToMs(objectStats.bulkRemoveCollectUs),
				UsToMs(objectStats.bulkRemovePageDeallocUs),
				UsToMs(objectStats.activeDrawSetRemoveUs),
				UsToMs(objectStats.bulkRemoveUs),
				UsToMs(objectStats.staticDirectResizePublishUs),
				UsToMs(objectStats.staticDirectScopeBuildUs),
				UsToMs(objectStats.staticDirectNormalPatchUs),
				UsToMs(objectStats.staticDirectPacketBuildUs),
				UsToMs(objectStats.staticDirectPacketPublishUs),
				objectStats.activeDrawSetInsertCalls,
				objectStats.activeDrawSetInsertIndices,
				objectStats.activeDrawSetRemoveCalls,
				objectStats.activeDrawSetRemoveIndices,
				objectStats.bulkRemoveCalls,
				objectStats.bulkRemoveObjects,
				objectStats.activeDrawSetCompactionJobsQueued,
				objectStats.activeDrawSetCompactionJobsBuilt,
				objectStats.activeDrawSetCompactionJobsPublished,
				objectStats.activeDrawSetCompactionJobsStale,
				objectStats.activeDrawSetCompactionInputEntries,
				objectStats.activeDrawSetCompactionOutputEntries,
				UsToMs(objectStats.activeDrawSetCompactionWorkerUs),
				UsToMs(objectStats.activeDrawSetCompactionPublishUs),
				objectStats.deferredRetireQueueDepth,
				objectStats.deferredRetireRangesQueued,
				objectStats.deferredRetireRangesRetired,
				objectStats.deferredRetireBytesQueued / (1024 * 1024),
				objectStats.deferredRetireBytesRetired / (1024 * 1024),
				UsToMs(objectStats.deferredRetireWorkerUs),
				_counters->staticDirectGroupsSubmitted,
				_counters->staticDirectGroupsImported,
				_counters->staticDirectGroupsRemoved,
				_counters->staticDirectTemplatesCreated,
				_counters->staticDirectTemplatesReused,
				_counters->staticDirectTransformRows,
				_counters->staticDirectDrawRecords);
			if (serviceStats.pendingGroups <= 8) {
				for (const auto& diagnostic : serviceStats.pendingGroupDiagnostics) {
					spdlog::info("SARP static pending detail: {}", diagnostic);
				}
			}
		}
	}

}
