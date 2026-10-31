// The static import runtime (see StaticImportRuntime.h).

#include <BasicRenderer/StaticImport/StaticImportRuntime.h>


#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <shared_mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <BasicTelemetry/Tracy.h>

#include <BasicRenderer/Assets/CachePaths.h>
#include <BasicRenderer/Pipeline/ShaderVariantRequestService.h>
#include <BasicRenderer/StaticImport/StaticSceneRecipe.h>
#include <BasicRenderer/Streaming/MaterialRequests.h>
#include <BasicRenderer/Streaming/ObjectRequests.h>
#include <BasicRenderer/Streaming/PublishedRendererState.h>
#include <BasicRenderer/Streaming/RendererStateArtifacts.h>
#include <BasicRenderer/Streaming/RendererStateRequestService.h>
#include <BasicRenderer/Streaming/SceneAssetRequestService.h>
#include <BasicRenderer/Streaming/SerializedTaskPump.h>
#include <BasicRenderer/Streaming/StaticGeometryRequestService.h>
#include <BasicRenderer/Streaming/StaticMaterialRequestService.h>
#include <BasicRenderer/Streaming/StaticObjectRequestService.h>
#include <BasicRenderer/Streaming/StaticSceneArtifacts.h>
#include <BasicRenderer/Streaming/TaskScheduler.h>

#include <BasicRenderer/StaticImport/Advanced/StaticImportSupport.h>
#include <BasicRenderer/StaticImport/Advanced/StaticImportCapacityBroker.h>
#include <BasicRenderer/StaticImport/Advanced/StaticImportIdentity.h>
#include <BasicRenderer/StaticImport/Advanced/StaticImportRequests.h>
#include <BasicRenderer/StaticImport/Advanced/StaticImportWorkScheduler.h>
#include <BasicRenderer/StaticImport/Advanced/StaticImportPreparation.h>
#include "StaticImport/StaticGroupLifecycleStore.h"
#include "StaticImport/StaticImportCoordinator.h"
#include "StaticImport/StaticImportDrainScheduler.h"
#include "StaticImport/StaticImportPipelineAccess.h"
#include "StaticImport/StaticImportStreamingPump.h"
#include "StaticImport/StaticStreamingContext.h"

namespace br::staticimport
{
	class StaticImportRuntime::Impl
	{
	public:
		Impl(
			StaticImportShared& shared,
			const br::staticimport::StaticImportSettings& settings,
			br::staticimport::IStaticAssetSource& assets,
			br::staticimport::IStaticPlacementSource& placements,
			Hooks hooks)
			// Every subsystem takes its collaborators here. This list must follow the
			// member declaration order, which is itself the dependency order -- the
			// pump needs the coordinator, which needs the drain scheduler, and so on.
			: _shared(shared)
			, _staticImportSettings(settings)
			, _staticAssets(assets)
			, _placements(placements)
			, _hooks(std::move(hooks))
			, _workBus(_staticImportSettings)
			, _drains(_shared, _importState, _staticImportCoordinatorPump,
				[this](StaticImportWorkSource source, const br::TaskContext& context) {
					StaticImportServiceWorkerDrain(source, context);
				})
			, _staticImportAccess(_shared, _workBus, _importState, _drains, _coordinator, _pump)
			, _coordinator(
				StaticStreamingContext{ _shared, _workBus, _importState, _drains, _counters, _staticImportSettings, _staticAssets, _placements, _staticExtensions },
				_staticImportLifecycleStore, _staticImportCapacityBroker,
				_staticImportCoordinatorPump,
				{
					.buildDesiredStaticCells = [this](const br::staticimport::StaticStreamTarget& target) {
						return _pump.BuildDesiredStaticCells(target);
					},
				})
			, _pump(
				StaticStreamingContext{ _shared, _workBus, _importState, _drains, _counters, _staticImportSettings, _staticAssets, _placements, _staticExtensions },
				_coordinator, _staticImportLifecycleStore, _staticImportCapacityBroker,
				{
					.ensureStaticWorldSnapshot = [this] { return EnsurePlacementsReady(); },
					.liveStaticObjectCount = [this] { return LiveStaticObjectCount(); },
					.hostObjectCounters = _hooks.hostObjectCounters,
				})
		{
			_shared.staticStreamingScope = TaskSchedulerManager::GetInstance().CreateScope("SARP.StaticStreaming");
			_staticImportCoordinatorPump.Configure(
				[this](br::SerializedTaskPump::Task task) {
					return TaskSchedulerManager::GetInstance().Submit(
						_shared.staticStreamingScope, TaskLane::Streaming, TaskDomain::StaticImportControl,
						"SARP::StaticImport::CoordinatorDrain",
						[this, task = std::move(task)](const br::TaskContext& context) mutable {
							if (!context.StopRequested() &&
								!_shared.staticImportServiceStop.load(std::memory_order_acquire)) task();
						});
				},
				[this] { _coordinator.Drain(); },
				[this] {
					if (!_shared.staticImportServiceStop.load(std::memory_order_acquire)) {
						spdlog::error("SARP static import coordinator pump rejected a drain");
					}
				},
				[this](std::chrono::steady_clock::duration delay, br::SerializedTaskPump::Task task) {
					return TaskSchedulerManager::GetInstance().ScheduleAfter(
						_shared.staticStreamingScope, delay, TaskLane::Streaming,
						TaskDomain::StaticImportControl, "SARP::StaticImport::DependencyPoll",
						[this, task = std::move(task)](const br::TaskContext& context) mutable {
							if (!context.StopRequested() &&
								!_shared.staticImportServiceStop.load(std::memory_order_acquire)) task();
						});
				},
				br::SerializedTaskPump::HandoffMode::Resubmit);

			_shared.graphCompletions->staticGroupWake = [this] {
				// Prepared StaticGroup artifacts are consumed by the coordinator, not
				// by the streaming-update drain. Waking only the latter loses the edge
				// when the camera target is unchanged: UpdateStaticImportServiceCameraTarget
				// has nothing to publish, so no coordinator task is scheduled. A burst can
				// then remain indefinitely in GraphCompletionHandoff while the graph-side
				// producer window is full.
				_drains.ScheduleCoordinatorDrain();
			};
			_importState.staticImportCallbackHandoff->wake = [this](StaticImportWorkSource source) {
				if (_shared.staticImportServiceStop.load(std::memory_order_acquire) ||
					(source == StaticImportWorkSource::Publication &&
						!_importState.staticImportPublicationAdmissionPending.load(std::memory_order_acquire))) return;
				_shared.staticImportWorkScheduler.Signal(source);
				_drains.ScheduleWorkerDrains();
			};
			const std::weak_ptr assetWake = _importState.staticImportCallbackHandoff;
			_staticAssets.SetReadyRenderableAssetsCallback([assetWake] {
				if (const auto handoff = assetWake.lock()) {
					handoff->Invoke(StaticImportWorkSource::SpeculativeAssetPublish);
					handoff->Invoke(StaticImportWorkSource::RecipeCompile);
				}
			});
		}

		~Impl()
		{
			Shutdown();
		}

		bool AddExtension(br::staticimport::IStaticImportExtension& extension)
		{
			if (_started || _shutdown ||
				std::ranges::find(_staticExtensions, std::addressof(extension)) != _staticExtensions.end()) {
				return false;
			}
			_staticExtensions.push_back(std::addressof(extension));
			_coordinator.SetExtensions(_staticExtensions);
			_pump.SetExtensions(_staticExtensions);
			extension.OnAttached(_staticImportAccess);
			return true;
		}

		bool RemoveExtension(br::staticimport::IStaticImportExtension& extension)
		{
			const auto found = std::ranges::find(_staticExtensions, std::addressof(extension));
			if (_started || _shutdown || found == _staticExtensions.end()) {
				return false;
			}
			_staticImportAccess.ReleaseHostWorkSources(extension);
			_staticExtensions.erase(found);
			_coordinator.SetExtensions(_staticExtensions);
			_pump.SetExtensions(_staticExtensions);
			return true;
		}

		void Shutdown()
		{
			if (_shutdown) return;
			_shutdown = true;
			_shared.staticImportServiceStop.store(true, std::memory_order_release);
			_staticImportCapacityBroker.Stop();
			_staticImportCoordinatorPump.Stop();
			_importState.staticImportCallbackHandoff->Clear();
			_staticAssets.SetReadyRenderableAssetsCallback({});
			if (_shared.hasSceneIngestionServices && _shared.sceneIngestionServices.objects) {
				_shared.sceneIngestionServices.objects->SetDesiredBufferStateReadyCallback({});
			}
			if (_hooks.detachStateGraph) _hooks.detachStateGraph();
			_shared.graphCompletions->alive.store(false, std::memory_order_release);
			_graphMaterialObservation.Reset();
			_graphStaticGroupObservation.Reset();
			_graphStaticVariantObservation.Reset();
			if (_hooks.stopping) _hooks.stopping();
			try {
				_shared.staticStreamingScope.CancelAndWait();
			} catch (const std::exception& ex) {
				spdlog::error("SARP static streaming shutdown: task failed: {}", ex.what());
			} catch (...) {
				spdlog::error("SARP static streaming shutdown: task failed with unknown exception");
			}
			// The publication-critical workers are gone, so this is now the sole
			// persistence consumer.  Publish at most one bounded batch; cache loss is
			// acceptable at shutdown, scene correctness is not coupled to this flush.
			if (!_workBus.staticSceneRecipePersistenceQueue.Empty()) {
				DrainStaticSceneRecipePersistence();
			}
		}

		void AttachServices(::br::render::StaticSceneIngestionAccess services)
		{
			if (_hooks.detachStateGraph) _hooks.detachStateGraph();
			_graphMaterialObservation.Reset();
			_graphStaticGroupObservation.Reset();
			_graphStaticVariantObservation.Reset();
			if (_shared.hasSceneIngestionServices && _shared.sceneIngestionServices.objects) {
				_shared.sceneIngestionServices.objects->SetDesiredBufferStateReadyCallback({});
			}
			_shared.sceneIngestionServices = std::move(services);
			_shared.staticSceneRenderSettings = StaticSceneRenderSettings::Capture();
			_shared.hasSceneIngestionServices = true;
			_started = true;
			_shared.graphCompletions->alive.store(true, std::memory_order_release);
			if (_shared.hasSceneIngestionServices) {
				if (auto* objects = _shared.sceneIngestionServices.objects) {
					const std::weak_ptr publicationWake = _importState.staticImportCallbackHandoff;
					objects->SetDesiredBufferStateReadyCallback([publicationWake] {
						if (const auto handoff = publicationWake.lock()) {
							handoff->Invoke(StaticImportWorkSource::Publication);
							handoff->Invoke(StaticImportWorkSource::RetirementCommit);
						}
					});
				}
				if (auto* requests = _shared.sceneIngestionServices.execution.stateRequests) {
					if (!_importState.staticGroupProducerRegistered) {
						_importState.staticGroupSuspensionNotifier = requests->MakeSuspensionNotifier();
						requests->RegisterProducer(br::render::ArtifactKind::StaticGroup, {
							TaskLane::Streaming,
							TaskDomain::StaticGroupPreparation,
							"SARP::StaticGroup::Prepare",
							[this](const br::render::ArtifactBuildContext& context) {
								return BuildStaticGroupArtifact(context);
							}
						});
						_importState.staticGroupProducerRegistered = true;
					}
					if (!_importState.staticTemplateProducerRegistered) {
						_importState.staticTemplateSuspensionNotifier = requests->MakeSuspensionNotifier();
						requests->RegisterProducer(br::render::ArtifactKind::StaticTemplate, {
							TaskLane::Streaming,
							TaskDomain::StaticImport,
							"SARP::StaticTemplate::Build",
							[this](const br::render::ArtifactBuildContext& context) {
								return BuildStaticTemplateArtifact(context);
							}
						});
						_importState.staticTemplateProducerRegistered = true;
					}
					if (!_importState.staticVariantProducerRegistered) {
						requests->RegisterProducer(br::render::ArtifactKind::StaticVariant, {
							TaskLane::Streaming,
							TaskDomain::StaticGroupPreparation,
							"SARP::StaticVariant::ResolveDependencies",
							[this](const br::render::ArtifactBuildContext& context) {
								return BuildStaticVariantArtifact(context);
							}
						});
						_importState.staticVariantProducerRegistered = true;
					}
					if (_hooks.attachStateGraph) _hooks.attachStateGraph(*requests);
					const auto handoff = _shared.graphCompletions;
					_graphMaterialObservation = requests->ObserveKind(
						br::render::ArtifactKind::MaterialTable,
						[handoff](std::uint64_t, const br::render::ArtifactSnapshot&) {
							if (!handoff->alive.load(std::memory_order_acquire)) return;
							handoff->materialTableWake.store(true, std::memory_order_release);
						});
					const std::weak_ptr completionHandoff = _shared.graphCompletions;
					_graphStaticGroupObservation = requests->ObserveKind(
						br::render::ArtifactKind::StaticGroup,
						[completionHandoff](std::uint64_t,
							const br::render::ArtifactSnapshot& snapshot) {
							// GpuReady carries the prepared group artifact consumed by the static
							// import coordinator. Published is a second notification for the same
							// version after its payload has already been consumed; queueing it can
							// leave inert snapshots in the handoff once scene loading quiesces and
							// falsely keep benchmark/sampling readiness non-zero.
							if (snapshot.readiness != br::render::ArtifactReadiness::GpuReady &&
								snapshot.readiness != br::render::ArtifactReadiness::Failed &&
								snapshot.readiness != br::render::ArtifactReadiness::Cancelled &&
								snapshot.readiness != br::render::ArtifactReadiness::Superseded) return;
							if (const auto handoff = completionHandoff.lock())
								handoff->PushStaticGroup(snapshot);
						});
					_graphStaticVariantObservation = requests->ObserveKind(
						br::render::ArtifactKind::StaticVariant,
						[completionHandoff](std::uint64_t,
							const br::render::ArtifactSnapshot& snapshot) {
							if (snapshot.readiness != br::render::ArtifactReadiness::Failed &&
								snapshot.readiness != br::render::ArtifactReadiness::Cancelled &&
								snapshot.readiness != br::render::ArtifactReadiness::Superseded) return;
							if (const auto handoff = completionHandoff.lock())
								handoff->PushTerminalStaticVariant(snapshot);
						});
				}
			}
		}

		void UpdateStreaming(const br::staticimport::StaticStreamTarget& target)
		{
			_started = true;
			_pump.UpdateStaticStreaming(target);
		}

		// Builds the groups' records on the caller's thread; the coordinator seals
		// them (ApplyGroupChanges) once the workers started.
		void AddOrUpdateGroups(std::vector<StaticImportGroup> groups)
		{
			StaticGroupChanges change;
			change.adds.reserve(groups.size());
			for (auto& source : groups) {
				if (source.groupID == 0 || source.assetKey.empty() || source.instanceWorldMatrices.empty()) {
					continue;
				}
				PreparedStaticGroup group;
				group.groupID = source.groupID;
				group.modelPath = std::move(source.assetKey);
				group.name = std::move(source.name);
				group.placementCount = source.instanceWorldMatrices.size();
				group.instanceWorldMatrices.reserve(source.instanceWorldMatrices.size());
				for (const auto& world : source.instanceWorldMatrices) {
					group.instanceWorldMatrices.push_back(DirectX::XMLoadFloat4x4(&world));
				}
				group.materialVariant = _staticAssets.MakeMaterialVariant(
					source.materialOverrideHash, std::move(source.materialOverrides));
				change.adds.push_back(MakeStaticGroupLifecycleRecord(
					group, StaticCellKey{}, source.batchKey != 0 ? source.batchKey : source.groupID));
			}
			if (change.adds.empty()) return;
			(void)_drains.PushBlocking(_workBus.staticImportGroupChanges, std::move(change));
		}

		void RemoveGroups(std::span<const std::uint64_t> groupIDs)
		{
			StaticGroupChanges change;
			change.removes.reserve(groupIDs.size());
			for (const auto groupID : groupIDs) {
				if (groupID != 0) change.removes.push_back(groupID);
			}
			if (change.removes.empty()) return;
			(void)_drains.PushBlocking(_workBus.staticImportGroupChanges, std::move(change));
		}

		// The pump's readiness gate: loads the placement source and, once it is
		// available, opens the recipe cache for its environment.
		bool EnsurePlacementsReady()
		{
			if (_placementsReady) {
				return true;
			}
			const auto begin = std::chrono::steady_clock::now();
			if (!_placements.EnsureReady()) {
				return false;
			}
			if (_staticImportSettings.recipeCacheMode != br::staticimport::StaticRecipeCacheMode::Off) {
				_importState.staticSceneRecipeStore = std::make_unique<br::staticimport::recipe::Store>(
					GetCacheRootPath(), _placements.RecipeEnvironment());
			} else {
				_importState.staticSceneRecipeStore.reset();
			}
			{
				std::lock_guard bindingLock(_importState.staticRecipeVariantBindingMutex);
				_importState.staticRecipeVariantBindings.clear();
			}
			_counters.staticIndexLoadMs = static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - begin).count());
			_placementsReady = true;
			return true;
		}

		enum class DirectStaticBuildResult
		{
			PendingAsset,
			Ready,
			EmptyOrUnsupported
		};

		enum class DirectStaticPendingReason
		{
			AssetNotReady,
			TemplateNotReady
		};

		using StaticCellKey = StaticCellKey;
		using StaticCellKeyHash = StaticCellKeyHash;
		using StaticImportWorkSource = StaticImportWorkSource;
		using StaticImportWorkScheduler = StaticImportWorkScheduler;
		using StaticImportCapacityBroker = StaticImportCapacityBroker;

		using StaticStreamKey = StaticStreamKey;

		// Immutable host ownership retained by StaticTransaction -> page -> scene.
		// The graph never interprets this payload; it makes the selected scene cut
		// the authoritative source for unload and replacement lifetime.
		static void TrimPreparedDirectStaticImportForTransactionService(PreparedDirectStaticImport& prepared)
		{
			prepared.ready.buildInfos.clear();
			auto directWorkloadKeys = std::move(prepared.ready.sceneObject.directWorkloadKeys);
			const auto instanceCount = prepared.ready.sceneObject.instanceCount;
			const auto directStatic = prepared.ready.sceneObject.directStatic;
			prepared.ready.sceneObject = StaticReadyGroupObject{};
			prepared.ready.sceneObject.instanceCount = instanceCount;
			prepared.ready.sceneObject.directStatic = directStatic;
			prepared.ready.sceneObject.directWorkloadKeys = std::move(directWorkloadKeys);
			prepared.objectPacket = {};
		}

		static PreparedDirectStaticImport MakeStaticImportPublicationView(
			const PreparedDirectStaticImport& source,
			std::uint64_t generation)
		{
			PreparedDirectStaticImport view;
			view.generation = generation;
			view.groupID = source.groupID;
			view.admissionTicketID = source.admissionTicketID;
			view.cellKey = source.cellKey;
			view.batchKey = source.batchKey;
			view.groupRevision = source.groupRevision;
			view.residencyClass = source.residencyClass;
			view.residencyGeneration = source.residencyGeneration;
			view.intentToken = source.intentToken;
			view.hostOwner = source.hostOwner;
			view.hostPayload = source.hostPayload;
			view.placementCount = source.placementCount;
			view.firstBuildGroup = 0;
			view.submittedGroupCount = source.submittedGroupCount;
			view.recipePrepared = source.recipePrepared;
			view.extensionHolds = source.extensionHolds;
			view.ready.groupID = source.ready.groupID;
			view.ready.placementCount = source.ready.placementCount;
			view.ready.sceneObject.instanceCount = source.ready.sceneObject.instanceCount;
			view.ready.sceneObject.directStatic = source.ready.sceneObject.directStatic;
			view.ready.sceneObject.directWorkloadKeys =
				source.ready.sceneObject.directWorkloadKeys;
			view.ready.recipeVariantBinding = source.ready.recipeVariantBinding;
			view.prepareUs = source.prepareUs;
			return view;
		}

		static br::render::PreparedStaticGroupInfo MakeStaticImportGroupView(
			const br::render::PreparedStaticGroupInfo& source,
			std::shared_ptr<const void> owner)
		{
			br::render::PreparedStaticGroupInfo view;
			view.stableGroupID = source.stableGroupID;
			view.allocationScopeID = source.allocationScopeID;
			view.mappedPerObjectCBs = source.PerObjectRows();
			view.mappedNormalMatrices = source.NormalRows();
			view.mappedMeshTemplates = source.MeshTemplates();
			view.mappedUniqueWorkloadKeys = source.UniqueWorkloadKeys();
			view.mappedWorkloadRouteIndices = source.WorkloadRouteIndices();
			view.mappedWorkloadRouteRanges = source.WorkloadRouteRanges();
			view.mappedWorkloadRouteOccurrences = source.WorkloadRouteOccurrences();
			view.mappedRecipeOwner = owner;
			view.mappedTemplateOwner = std::move(owner);
			view.mappedRecipeSemantics = source.IsRecipeView();
			return view;
		}

		struct PublishedStaticVariantArtifact
		{
			std::uint64_t gateID{ 0 };
			std::uint64_t fingerprint{ 0 };
			StaticTemplateRefSnapshot templateRefs;
		};

		std::vector<br::staticimport::StaticGroupHold> CollectStaticGroupHolds(
			const DirectStaticReadyGroup& ready,
			const StaticCellKey& sourceCell) const
		{
			std::vector<br::staticimport::StaticGroupHold> holds;
			holds.reserve(_staticExtensions.size());
			for (std::size_t extensionIndex = 0; extensionIndex < _staticExtensions.size(); ++extensionIndex) {
				br::staticimport::StaticGroupHoldSource source{ .cell = sourceCell };
				if (ready.recipeVariantBinding) {
					source.variantHold = std::addressof(
						br::staticimport::StaticGroupHoldAt(ready.recipeVariantBinding->extensionHolds, extensionIndex));
				} else {
					source.buildInfos = ready.buildInfos;
				}
				holds.push_back(_staticExtensions[extensionIndex]->CollectGroupHold(source));
			}
			return holds;
		}

		static DirectStaticTemplateKey MakeDirectStaticTemplateKey(const Mesh& mesh, const Material& material)
		{
			return DirectStaticTemplateKey{
				.meshID = mesh.GetGlobalID(),
				.materialID = material.GetMaterialID(),
				.rasterFlags = static_cast<std::uint64_t>(ComposeDirectStaticRasterFlags(const_cast<Mesh&>(mesh), material))
			};
		}

		std::vector<StaticImportDependencyRequest> CollectDirectStaticShaderVariantDependencies(
			const DirectStaticReadyGroup& ready) const
		{
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::BindRecipe::CheckShaderDependencies");
			std::vector<StaticImportDependencyRequest> dependencies;
			if (ready.recipeVariantBinding &&
				ready.recipeVariantBinding->shadersReady.load(std::memory_order_acquire)) {
				return dependencies;
			}
			if (!_shared.hasSceneIngestionServices || !_shared.sceneIngestionServices.shaderVariants) {
				dependencies.push_back(MakeStaticImportDependencyRequest(StaticShaderVariantRequest{}));
				return dependencies;
			}

			DirectStaticShaderVariantOptions options{
				.softwareRasterMode = _shared.staticSceneRenderSettings.softwareRasterMode,
				.vsmRasterMode = _shared.staticSceneRenderSettings.vsmRasterMode,
				.transparencyMode = _shared.staticSceneRenderSettings.transparencyMode,
				.reyesEnabled = _shared.staticSceneRenderSettings.reyesEnabled
			};
			const auto& buildInfos = ready.recipeVariantBinding ? ready.recipeVariantBinding->shaderBuildInfos : ready.buildInfos;
			auto warmupRequests = CollectDirectStaticShaderVariantRequests(buildInfos, options);
			_shared.sceneIngestionServices.shaderVariants->RequestShaderVariants(warmupRequests);
			for (const auto& request : warmupRequests) {
				if (_shared.sceneIngestionServices.shaderVariants->RequestShaderVariant(request)) {
					continue;
				}
				dependencies.push_back(MakeStaticImportDependencyRequest(StaticShaderVariantRequest{ .request = request }));
			}
			if (dependencies.empty() && ready.recipeVariantBinding) {
				ready.recipeVariantBinding->shadersReady.store(true, std::memory_order_release);
			}
			return dependencies;
		}

		DirectStaticBuildResult BuildDirectStaticGroupFromReadyAssets(
			std::uint64_t groupID,
			const PendingStaticGroup& group,
			const StaticTemplateRefMap& templateRefs,
			DirectStaticReadyGroup& outReady,
			DirectStaticPendingReason* outPendingReason = nullptr)
		{
			if (group.instanceWorldMatrices.empty()) {
				return DirectStaticBuildResult::EmptyOrUnsupported;
			}

			const auto* parts = _staticAssets.TryGetReadyParts(group.ModelPath(), group.MaterialVariant());
			if (!parts || parts->empty()) {
				if (outPendingReason) {
					*outPendingReason = DirectStaticPendingReason::AssetNotReady;
				}
				return DirectStaticBuildResult::PendingAsset;
			}

			outReady.groupID = groupID;
			outReady.placementCount = group.placementCount;
			outReady.sceneObject.modelPath = group.ModelPath();
			outReady.sceneObject.instanceCount = group.placementCount;
			outReady.sceneObject.directStatic = true;
			outReady.sceneObject.partNames.reserve(parts->size());
			outReady.sceneObject.partSkinnedShapeIndices.reserve(parts->size());
			outReady.buildInfos.reserve(parts->size());

			auto classifiedParts = ClassifyDirectStaticGroupFromReadyParts(
				groupID,
				group.batchKey,
				group.instanceWorldMatrices,
				*parts);
			for (auto& classified : classifiedParts) {
				auto buildInfo = std::move(classified.buildInfo);
				bool missingTemplate = false;
				for (auto& meshTemplate : buildInfo.meshTemplates) {
					const auto key = MakeDirectStaticTemplateKey(*meshTemplate.mesh, *meshTemplate.material);
					auto templateIt = templateRefs.find(key);
					if (templateIt == templateRefs.end()) {
						missingTemplate = true;
						continue;
					}
					meshTemplate = *templateIt->second;
				}
				if (missingTemplate) {
					if (outPendingReason) {
						*outPendingReason = DirectStaticPendingReason::TemplateNotReady;
					}
					return DirectStaticBuildResult::PendingAsset;
				}
				for (const auto& meshTemplate : buildInfo.meshTemplates) {
					for (const auto& workloadKey : meshTemplate.workloadKeys) {
						outReady.sceneObject.directWorkloadKeys.push_back(workloadKey);
					}
				}
				outReady.sceneObject.partNames.push_back(std::move(classified.name));
				outReady.sceneObject.partSkinnedShapeIndices.push_back(classified.skinnedShapeIndex);
				outReady.buildInfos.push_back(std::move(buildInfo));
			}

			return outReady.buildInfos.empty()
				? DirectStaticBuildResult::EmptyOrUnsupported
				: DirectStaticBuildResult::Ready;
		}

		DirectStaticBuildResult BindDirectStaticGroupFromRecipe(
			std::uint64_t groupID,
			const PendingStaticGroup& group,
			const StaticTemplateRefMap& templateRefs,
			DirectStaticReadyGroup& outReady,
			br::render::PreparedStaticGroupsBulkPlan& outPlan,
			DirectStaticPendingReason* outPendingReason = nullptr)
		{
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::BindRecipe::ResolveVariantAndMapParts");
			const bool hasMappedParts = group.mappedRecipe && group.mappedPreparedParts.count != 0;
			if (group.recipePreparedParts.empty() && !hasMappedParts) return DirectStaticBuildResult::EmptyOrUnsupported;
			const StaticRecipeVariantLookupKey variantLookupKey{ group.ModelPath(), group.materialVariant.hash };
			std::shared_ptr<StaticRecipeSceneVariantSlot> sceneVariant = group.recipeVariantSlot;
			if (!sceneVariant && group.recipeSceneBinding && group.recipeVariantIndex < group.recipeSceneBinding->variants.size()) {
				sceneVariant = group.recipeSceneBinding->variants[group.recipeVariantIndex];
			}
			std::shared_ptr<const StaticRecipeVariantBinding> variantBinding;
			std::unique_lock<std::mutex> sceneVariantLock;
			if (sceneVariant) {
				if (sceneVariant->readyBinding.load(std::memory_order_acquire)) {
					variantBinding = sceneVariant->binding;
				} else {
					{
						BT_ZONE_SCOPE("SARP::LiveSceneBridge::BindRecipe::WaitVariantBinding");
						sceneVariantLock = std::unique_lock{ sceneVariant->bindingMutex };
					}
					if (sceneVariant->readyBinding.load(std::memory_order_relaxed)) variantBinding = sceneVariant->binding;
				}
			}
			{
				if (!variantBinding) {
					std::shared_lock lock(_importState.staticRecipeVariantBindingMutex);
					if (const auto found = _importState.staticRecipeVariantBindings.find(variantLookupKey);
						found != _importState.staticRecipeVariantBindings.end()) variantBinding = found->second;
				}
			}
			std::unique_lock<std::mutex> variantBuildLock;
			if (!variantBinding) {
				const auto stripe = StaticRecipeVariantKeyHash{}(variantLookupKey) %
					_importState.staticRecipeVariantBuildMutexes.size();
				variantBuildLock = std::unique_lock{ _importState.staticRecipeVariantBuildMutexes[stripe] };
				// Another group with the same variant may have completed while this
				// worker waited for its keyed build slot.
				std::shared_lock lock(_importState.staticRecipeVariantBindingMutex);
				if (const auto found = _importState.staticRecipeVariantBindings.find(variantLookupKey);
					found != _importState.staticRecipeVariantBindings.end()) variantBinding = found->second;
			}
			if (!variantBinding) {
				BT_ZONE_SCOPE("SARP::LiveSceneBridge::BindRecipe::BuildVariantBinding");
				const auto* parts = _staticAssets.TryGetReadyParts(group.ModelPath(), group.MaterialVariant());
				if (!parts) {
					if (outPendingReason) *outPendingReason = DirectStaticPendingReason::AssetNotReady;
					return DirectStaticBuildResult::PendingAsset;
				}
				auto candidate = std::make_shared<StaticRecipeVariantBinding>();
				const auto meshCount = std::transform_reduce(parts->begin(), parts->end(), std::size_t{ 0 }, std::plus{},
					[](const auto& part) { return part.meshes.size(); });
				candidate->templatesByOrdinal.reserve(meshCount);
				candidate->workloadKeys.reserve(meshCount);
				candidate->preparedTemplatesByPart.resize(parts->size());
				candidate->templateOrdinalsByPart.resize(parts->size());
				candidate->uniqueWorkloadKeysByPart.resize(parts->size());
				candidate->workloadRouteIndicesByPart.resize(parts->size());
				candidate->workloadRouteRangesByPart.resize(parts->size());
				candidate->workloadRouteOccurrencesByPart.resize(parts->size());
				for (std::size_t partIndex = 0; partIndex < parts->size(); ++partIndex) {
					const auto& part = (*parts)[partIndex];
					candidate->preparedTemplatesByPart[partIndex].reserve(part.meshes.size());
					candidate->templateOrdinalsByPart[partIndex].reserve(part.meshes.size());
					const bool unsupportedSkin = std::ranges::any_of(part.meshes, [](const auto& mesh) {
						return mesh && mesh->HasBaseSkin() && !mesh->GetBaseSkin()->HasWindSimulationGroups();
					});
					if (unsupportedSkin) continue;
					br::render::StaticGroupBuildInfo shaderBuild;
					shaderBuild.meshTemplates.reserve(part.meshes.size());
					for (std::size_t meshIndex = 0; meshIndex < part.meshes.size(); ++meshIndex) {
						const auto& mesh = part.meshes[meshIndex];
						if (!mesh || (mesh->HasBaseSkin() && !mesh->GetBaseSkin()->HasWindSimulationGroups())) continue;
						auto material = mesh->material ? mesh->material : Material::GetDefaultMaterial();
						if (!material) continue;
						const auto found = templateRefs.find(MakeDirectStaticTemplateKey(*mesh, *material));
						if (found == templateRefs.end()) {
							if (outPendingReason) *outPendingReason = DirectStaticPendingReason::TemplateNotReady;
							return DirectStaticBuildResult::PendingAsset;
						}
						const auto ordinal = (static_cast<std::uint64_t>(partIndex) << 32u) | meshIndex;
						const auto templateIt = candidate->templatesByOrdinal.emplace(ordinal, *found->second).first;
						const auto& resolved = templateIt->second;
						auto& prepared = candidate->preparedTemplatesByPart[partIndex].emplace_back();
						prepared.meshTemplateIndex = resolved.meshTemplateIndex;
						prepared.clodOffsetIndex = resolved.clodOffsetIndex;
						// Preserve the immutable source-mesh identity across the recipe binding.
						// Draw records use this to validate that later CLOD/page-map evaluation
						// still resolves to geometry owned by the template that authored the row.
						prepared.meshIdentity = resolved.meshIdentity;
						prepared.skinnedAssemblyTypeSlot = resolved.skinnedAssemblyTypeSlot;
						prepared.skinnedAssemblyBounds = resolved.skinnedAssemblyBounds;
						prepared.skinnedBoundsScale = resolved.skinnedBoundsScale;
						prepared.mappedWorkloadKeys = resolved.workloadKeys;
						candidate->templateOrdinalsByPart[partIndex].push_back(ordinal);
						shaderBuild.meshTemplates.push_back(*found->second);
						candidate->workloadKeys.insert(candidate->workloadKeys.end(), found->second->workloadKeys.begin(), found->second->workloadKeys.end());
					}
					if (!shaderBuild.meshTemplates.empty()) candidate->shaderBuildInfos.push_back(std::move(shaderBuild));
				}
				candidate->extensionHolds.reserve(_staticExtensions.size());
				for (const auto* extension : _staticExtensions) {
					candidate->extensionHolds.push_back(extension->DescribeVariantBinding(candidate->shaderBuildInfos));
				}
				{
					BT_ZONE_SCOPE("SARP::LiveSceneBridge::BindRecipe::BuildVariantWorkloadRoutes");
					for (std::size_t partIndex = 0;
						partIndex < candidate->preparedTemplatesByPart.size(); ++partIndex) {
						auto& uniqueKeys = candidate->uniqueWorkloadKeysByPart[partIndex];
						auto& routeIndices = candidate->workloadRouteIndicesByPart[partIndex];
						auto& routeRanges = candidate->workloadRouteRangesByPart[partIndex];
						auto& routeOccurrences = candidate->workloadRouteOccurrencesByPart[partIndex];
						const auto& templates = candidate->preparedTemplatesByPart[partIndex];
						routeRanges.reserve(templates.size());
						for (const auto& preparedTemplate : templates) {
							const auto first = routeIndices.size();
							for (const auto& workloadKey : preparedTemplate.WorkloadKeys()) {
								const auto found = std::ranges::find(uniqueKeys, workloadKey);
								std::size_t routeIndex = 0;
								if (found == uniqueKeys.end()) {
									routeIndex = uniqueKeys.size();
									uniqueKeys.push_back(workloadKey);
									routeOccurrences.push_back(0u);
								} else {
									routeIndex = static_cast<std::size_t>(
										std::distance(uniqueKeys.begin(), found));
								}
								routeIndices.push_back(static_cast<std::uint32_t>(routeIndex));
								++routeOccurrences[routeIndex];
							}
							routeRanges.push_back({
								.first = static_cast<std::uint32_t>(first),
								.count = static_cast<std::uint32_t>(routeIndices.size() - first) });
						}
					}
				}
				{
					std::lock_guard lock(_importState.staticRecipeVariantBindingMutex);
					variantBinding = _importState.staticRecipeVariantBindings.try_emplace(
						StaticRecipeVariantKey{ std::string{ group.ModelPath() }, group.materialVariant.hash }, candidate).first->second;
				}
			}
			if (sceneVariant && !sceneVariant->readyBinding.load(std::memory_order_relaxed)) {
				sceneVariant->binding = variantBinding;
				sceneVariant->readyBinding.store(variantBinding.get(), std::memory_order_release);
			}
			if (sceneVariantLock.owns_lock()) sceneVariantLock.unlock();

			outReady.groupID = groupID;
			outReady.placementCount = group.placementCount;
			outReady.sceneObject.instanceCount = group.placementCount;
			outReady.sceneObject.directStatic = true;
			outReady.recipeVariantBinding = variantBinding;
			outPlan.groups.reserve(outPlan.groups.size() +
				(hasMappedParts ? group.mappedPreparedParts.count : group.recipePreparedParts.size()));
			auto bindPart = [&](std::uint64_t stableGroupID, std::uint64_t allocationScopeID, std::uint32_t rowStride,
				std::span<const std::byte> cachedRows, std::span<const br::staticimport::recipe::Matrix4x4> cachedNormals,
				auto cachedTemplates, std::shared_ptr<const void> mappedOwner) {
				if (rowStride != sizeof(PerObjectCB) || cachedRows.size() % sizeof(PerObjectCB) != 0 ||
					cachedNormals.size() != cachedRows.size() / sizeof(PerObjectCB)) {
					return DirectStaticBuildResult::EmptyOrUnsupported;
				}
				br::render::PreparedStaticGroupInfo prepared;
				prepared.stableGroupID = stableGroupID;
				prepared.allocationScopeID = allocationScopeID;
				prepared.mappedTemplateOwner = variantBinding;
				if (mappedOwner) {
					if (reinterpret_cast<std::uintptr_t>(cachedRows.data()) % alignof(PerObjectCB) != 0) return DirectStaticBuildResult::EmptyOrUnsupported;
					prepared.mappedPerObjectCBs = { reinterpret_cast<const PerObjectCB*>(cachedRows.data()), cachedRows.size() / sizeof(PerObjectCB) };
					prepared.mappedNormalMatrices = { reinterpret_cast<const DirectX::XMFLOAT4X4*>(cachedNormals.data()), cachedNormals.size() };
					prepared.mappedRecipeOwner = std::move(mappedOwner);
					prepared.mappedRecipeSemantics = true;
				} else {
					prepared.perObjectCBs.resize(cachedRows.size() / sizeof(PerObjectCB));
					std::memcpy(prepared.perObjectCBs.data(), cachedRows.data(), cachedRows.size());
					prepared.normalMatrices.resize(cachedNormals.size());
					for (std::size_t i = 0; i < cachedNormals.size(); ++i) {
						std::memcpy(std::addressof(prepared.normalMatrices[i]), cachedNormals[i].values, sizeof(DirectX::XMFLOAT4X4));
					}
				}

				if (cachedTemplates.empty()) return DirectStaticBuildResult::Ready;
				const auto partIndex = cachedTemplates.front().partIndex;
				if (partIndex >= variantBinding->preparedTemplatesByPart.size() ||
					partIndex >= variantBinding->templateOrdinalsByPart.size()) return DirectStaticBuildResult::EmptyOrUnsupported;
				const auto& partTemplates = variantBinding->preparedTemplatesByPart[partIndex];
				const auto& partOrdinals = variantBinding->templateOrdinalsByPart[partIndex];
				if (partTemplates.size() != cachedTemplates.size() || partOrdinals.size() != cachedTemplates.size()) {
					return DirectStaticBuildResult::EmptyOrUnsupported;
				}
				for (std::size_t templateIndex = 0; templateIndex < cachedTemplates.size(); ++templateIndex) {
					const auto& cachedTemplate = cachedTemplates[templateIndex];
					const auto ordinal = (static_cast<std::uint64_t>(cachedTemplate.partIndex) << 32u) | cachedTemplate.meshIndex;
					if (partOrdinals[templateIndex] != ordinal) return DirectStaticBuildResult::EmptyOrUnsupported;
				}
				prepared.mappedMeshTemplates = partTemplates;
				if (partIndex >= variantBinding->uniqueWorkloadKeysByPart.size() ||
					partIndex >= variantBinding->workloadRouteIndicesByPart.size() ||
					partIndex >= variantBinding->workloadRouteRangesByPart.size() ||
					partIndex >= variantBinding->workloadRouteOccurrencesByPart.size()) {
					return DirectStaticBuildResult::EmptyOrUnsupported;
				}
				prepared.mappedUniqueWorkloadKeys = variantBinding->uniqueWorkloadKeysByPart[partIndex];
				prepared.mappedWorkloadRouteIndices = variantBinding->workloadRouteIndicesByPart[partIndex];
				prepared.mappedWorkloadRouteRanges = variantBinding->workloadRouteRangesByPart[partIndex];
				prepared.mappedWorkloadRouteOccurrences = variantBinding->workloadRouteOccurrencesByPart[partIndex];
				outPlan.groups.push_back(std::move(prepared));
				return DirectStaticBuildResult::Ready;
			};
			if (hasMappedParts) {
				BT_ZONE_SCOPE("SARP::LiveSceneBridge::BindRecipe::MapPreparedParts");
				const auto& view = *group.mappedRecipe;
				for (const auto& cached : view.PreparedParts().subspan(group.mappedPreparedParts.first, group.mappedPreparedParts.count)) {
					const auto result = bindPart(cached.stableGroupID, cached.allocationScopeID, cached.perObjectRowStride,
						view.PerObjectRows().subspan(cached.perObjectRows.first, cached.perObjectRows.count),
						view.NormalMatrices().subspan(cached.normalMatrices.first, cached.normalMatrices.count),
						view.Templates().subspan(cached.templates.first, cached.templates.count), group.mappedRecipe);
					if (result != DirectStaticBuildResult::Ready) return result;
				}
			} else {
				for (const auto& cached : group.recipePreparedParts) {
					const auto result = bindPart(cached.stableGroupID, cached.allocationScopeID, cached.perObjectRowStride,
						cached.perObjectRows, cached.normalMatrices, cached.templates, {});
					if (result != DirectStaticBuildResult::Ready) return result;
				}
			}
			return DirectStaticBuildResult::Ready;
		}

		void CollectMissingDirectStaticTemplateRequestsFromReadyParts(
			const PendingStaticGroup& group,
			const StaticTemplateRefMap& templateRefs,
			std::vector<StaticTemplateRequest>& outRequests) const
		{
			const auto* parts = _staticAssets.TryGetReadyParts(group.ModelPath(), group.MaterialVariant());
			if (!parts || parts->empty()) {
				return;
			}
			std::unordered_set<DirectStaticTemplateKey, DirectStaticTemplateKeyHash> queuedKeys;
			queuedKeys.reserve(parts->size() * 2);
			for (const auto& part : *parts) {
				if (part.meshes.empty()) {
					continue;
				}
				const bool hasSkinnedMesh = std::any_of(part.meshes.begin(), part.meshes.end(), [](const auto& mesh) {
					return mesh && mesh->HasBaseSkin() && !mesh->GetBaseSkin()->HasWindSimulationGroups();
				});
				if (hasSkinnedMesh) {
					continue;
				}
				for (const auto& mesh : part.meshes) {
					if (!mesh || (mesh->HasBaseSkin() && !mesh->GetBaseSkin()->HasWindSimulationGroups())) {
						continue;
					}
					auto material = mesh->material ? mesh->material : Material::GetDefaultMaterial();
					if (!material) {
						continue;
					}
					const auto key = MakeDirectStaticTemplateKey(*mesh, *material);
					if (templateRefs.contains(key) || !queuedKeys.insert(key).second) {
						continue;
					}
					outRequests.push_back(StaticTemplateRequest{
						.key = key,
						.mesh = mesh,
						.material = material
					});
				}
			}
		}

		bool StaticImportTransactionWouldExceedChunk(
			const StaticImportTransactionCost& current,
			const StaticImportTransactionCost& next) const
		{
			const auto maxGroups = static_cast<std::uint64_t>(_staticImportSettings.maxTransactionGroups);
			const auto maxBytes = static_cast<std::uint64_t>(_staticImportSettings.maxTransactionPublishBytes);
			const auto maxDrawRecords = static_cast<std::uint64_t>(_staticImportSettings.maxTransactionDrawRecords);
			const auto maxActiveInsertIndices = static_cast<std::uint64_t>(_staticImportSettings.maxTransactionActiveInsertIndices);
			return (maxGroups != 0 && current.groups + next.groups > maxGroups) ||
				(maxBytes != 0 && current.bytes + next.bytes > maxBytes) ||
				(maxDrawRecords != 0 && current.drawRecords + next.drawRecords > maxDrawRecords) ||
				(maxActiveInsertIndices != 0 && current.activeInsertIndices + next.activeInsertIndices > maxActiveInsertIndices);
		}

		br::render::ArtifactBuildResult BuildStaticVariantArtifact(
			const br::render::ArtifactBuildContext& context)
		{
			BT_ZONE_SCOPE("SARP::StaticVariant::ResolveDependencies");
			const auto input = context.input.Get<StaticVariantArtifactInput>();
			if (!input || !input->representative || input->fingerprint == 0 ||
				context.key != StaticVariantArtifactAddress(
					input->representative->descriptor, _shared.staticSceneRenderSettings) ||
				input->fingerprint != StaticVariantFingerprint(
					input->representative->descriptor, _shared.staticSceneRenderSettings)) {
				return br::render::ArtifactBuildResult::Failure("static variant artifact identity mismatch");
			}
			if (context.stopRequested && context.stopRequested())
				return br::render::ArtifactBuildResult::Cancelled();

			const auto templateRefs = std::atomic_load_explicit(
				std::addressof(_importState.staticTemplateGraphSnapshot), std::memory_order_acquire);
			StaticImportGroupPrepareJob job;
			job.generation = input->target.generation;
			job.target = input->target;
			job.inputs.push_back({ .groupID = input->gateID, .record = input->representative });
			job.templateRefs = templateRefs;
			auto prepared = PrepareStaticImportGroupJob(std::move(job));
			if (!prepared.blockedGroups.empty()) {
				auto dependencies = std::move(prepared.blockedGroups.front().dependencies);
				const auto failedTemplates = std::atomic_load_explicit(
					std::addressof(_importState.staticTemplateGraphFailedSnapshot),
					std::memory_order_acquire);
				if (failedTemplates && std::ranges::any_of(dependencies,
						[&](const StaticImportDependencyRequest& dependency) {
							return dependency.key.kind == StaticImportDependencyKind::Template &&
								failedTemplates->contains(
									DirectStaticTemplateKeyFromDependency(dependency.key));
						})) {
					basic_telemetry::AddCounter(
						"SARP.StaticImportCoordinator.VariantTerminalTemplateFailures");
					return br::render::ArtifactBuildResult::Failure(
						"static variant depends on a failed template");
				}
				const auto identity = br::render::AllocateArtifactSuspensionIdentity();
				if (!_drains.PushBlocking(_workBus.staticGroupSuspensionRequests,
						StaticGroupSuspensionRequest{
							.groupID = input->gateID,
							.revision = context.revision,
							.identity = identity,
							.externalGraphGate = true,
							.dependencies = std::move(dependencies) })) {
					return br::render::ArtifactBuildResult::Failure("static variant dependency handoff failed");
				}
				// This queue is consumed only by the serialized coordinator. The graph
				// producer can be the last active source in a loading burst, so relying on
				// an unrelated completion to wake that coordinator strands the suspension
				// (and every StaticGroup sharing this variant) permanently.
				basic_telemetry::AddCounter(
					"SARP.StaticImportCoordinator.VariantSuspensionHandoffs");
				_drains.ScheduleCoordinatorDrain();
				return br::render::ArtifactBuildResult::Suspend(
					br::render::ArtifactSuspension::External(identity,
						"static variant dependencies"));
			}
			if (!prepared.unsupported.empty()) {
				spdlog::warn("SARP static variant failed: model='{}' group=0x{:016X} reason={}",
					input->representative->descriptor.modelPath, input->gateID,
					prepared.unsupported.front().reason);
				return br::render::ArtifactBuildResult::Failure(
					prepared.unsupported.front().reason.empty()
						? "static variant is unsupported" : prepared.unsupported.front().reason);
			}
			if (prepared.ready.empty()) {
				return br::render::ArtifactBuildResult::Failure("static variant produced no ready group");
			}
			// Every StaticGroup packet requires its variant exactly, so this is the
			// graph edge that keeps placements invisible until each texture their
			// templates sample is bound to a real image (a coverage-preserving one for
			// alpha-tested materials) or has explicitly failed. Needs() replaces this
			// node's requirements; the variant is requested with none of its own.
			std::vector<br::render::ArtifactRequirement> textureGates;
			{
				BT_ZONE_SCOPE("SARP::StaticVariant::ResolveDependencies::TextureDisplayGates");
				std::unordered_set<const Material*> materials;
				const auto appendMaterial = [&](const std::shared_ptr<Material>& material) {
					if (material && materials.insert(material.get()).second)
						br::render::AppendTextureDisplayRequirements(*material, textureGates);
				};
				for (const auto& ready : prepared.ready) {
					for (const auto& buildInfo : ready.ready.buildInfos)
						for (const auto& meshTemplate : buildInfo.meshTemplates) appendMaterial(meshTemplate.material);
					if (ready.ready.recipeVariantBinding)
						for (const auto& [ordinal, meshTemplate] : ready.ready.recipeVariantBinding->templatesByOrdinal)
							appendMaterial(meshTemplate.material);
				}
			}
			const bool texturesDisplayable = std::ranges::all_of(textureGates, [&](const auto& gate) {
				return std::ranges::any_of(context.dependencies,
					[&](const br::render::ArtifactSnapshot& dependency) { return dependency.key == gate.key; });
			});
			if (!texturesDisplayable) {
				basic_telemetry::AddCounter("SARP.StaticVariant.TextureDisplayWaits");
				return br::render::ArtifactBuildResult::Needs(std::move(textureGates));
			}
			auto published = std::make_shared<PublishedStaticVariantArtifact>();
			published->gateID = input->gateID;
			published->fingerprint = input->fingerprint;
			published->templateRefs = templateRefs;
			BT_PLOT("SARP.StaticVariant.Ready", int64_t{ 1 });
			return br::render::ArtifactBuildResult::Ready(
				br::render::ArtifactPayload::Make<PublishedStaticVariantArtifact>(std::move(published)));
		}

		br::render::ArtifactBuildResult BuildStaticGroupArtifact(
			const br::render::ArtifactBuildContext& context)
		{
			std::shared_ptr<const StaticGroupArtifactBuildInput> input;
			{
				BT_ZONE_SCOPE("SARP::StaticGroup::Prepare::ValidateInput");
				input = context.input.Get<StaticGroupArtifactBuildInput>();
				if (!input || input->intentTokens.empty() ||
					input->job.inputs.size() != input->intentTokens.size() ||
					context.key.kind != br::render::ArtifactKind::StaticGroup ||
					context.key.primaryID != input->packetID) {
					return br::render::ArtifactBuildResult::Failure(
						"static group packet artifact input is invalid");
				}
				if (context.stopRequested && context.stopRequested()) {
					return br::render::ArtifactBuildResult::Cancelled();
				}
			}

			StaticImportGroupPrepareJob job;
			std::vector<std::shared_ptr<StaticGroupIntentToken>> liveTokens;
			{
				BT_ZONE_SCOPE("SARP::StaticGroup::Prepare::FilterLiveInputs");
				job = input->job;
				BT_PLOT("SARP.StaticImportPacket.InputGroups", static_cast<int64_t>(job.inputs.size()));
				liveTokens.reserve(input->intentTokens.size());
				std::vector<DirectStaticPrepareInput> liveInputs;
				liveInputs.reserve(job.inputs.size());
				for (std::size_t index = 0; index < job.inputs.size(); ++index) {
					const auto& token = input->intentTokens[index];
					if (!token || !job.inputs[index].record ||
						!token->valid.load(std::memory_order_acquire) ||
						token->groupID != job.inputs[index].groupID) {
						continue;
					}
					liveTokens.push_back(token);
					liveInputs.push_back(std::move(job.inputs[index]));
				}
				job.inputs = std::move(liveInputs);
			}
			if (job.inputs.empty()) return br::render::ArtifactBuildResult::Cancelled();
			{
				BT_ZONE_SCOPE("SARP::StaticGroup::Prepare::ResolveVariantPayload");
				for (const auto& preparedInput : job.inputs) {
					const auto expected = StaticVariantArtifactAddress(
						preparedInput.Group(), _shared.staticSceneRenderSettings);
					const auto found = std::ranges::find_if(context.dependencies,
						[&](const auto& dependency) { return dependency.key == expected; });
					if (found == context.dependencies.end() ||
						!found->payload.Get<PublishedStaticVariantArtifact>() ||
						!found->payload.Get<PublishedStaticVariantArtifact>()->templateRefs) {
						return br::render::ArtifactBuildResult::Failure(
							"static group exact variant dependency has no published payload");
					}
				}
				// Template snapshots are published monotonically. The current snapshot
				// contains the union of every variant's resolved templates, while an
				// earlier variant payload may predate another variant in this packet.
				job.templateRefs = std::atomic_load_explicit(
					std::addressof(_importState.staticTemplateGraphSnapshot), std::memory_order_acquire);
				if (!job.templateRefs) {
					return br::render::ArtifactBuildResult::Failure(
						"static group template snapshot is unavailable");
				}
			}
			std::shared_ptr<StaticImportGroupPrepareResult> prepared;
			{
				BT_ZONE_SCOPE("SARP::StaticGroup::Prepare::ProcessGroupsBatch");
				prepared = std::make_shared<StaticImportGroupPrepareResult>(
					PrepareStaticImportGroupJob(std::move(job)));
			}
			BT_PLOT("SARP.StaticImportPacket.ReadyGroups", static_cast<int64_t>(prepared->ready.size()));
			BT_PLOT("SARP.StaticImportPacket.BlockedGroups", static_cast<int64_t>(prepared->blockedGroups.size()));
			BT_PLOT("SARP.StaticImportPacket.UnsupportedGroups", static_cast<int64_t>(prepared->unsupported.size()));
			if (!prepared->blockedGroups.empty()) {
				BT_PLOT("SARP.StaticImportPacket.PostVariantDependencyMisses",
					static_cast<int64_t>(prepared->blockedGroups.size()));
				return br::render::ArtifactBuildResult::Failure(
					"static group variant published before all dependencies resolved");
			}
			// StaticGroup packets are the immutable preparation boundary. Complete the count
			// vectors and workload totals before publishing the artifact so neither
			// the coordinator nor the publication assembler repeats normalization.
			{
				BT_ZONE_SCOPE("SARP::StaticGroup::Prepare::FinalizeBuildBatch");
				if (!prepared->transactionBuild.prepared.groups.empty()) {
					{
						BT_ZONE_SCOPE("SARP::StaticGroup::Prepare::FinalizeBuildBatch::RecomputePlanTotals");
						RecomputePreparedStaticPlanTotals(prepared->transactionBuild.prepared);
					}
					{
						BT_ZONE_SCOPE("SARP::StaticGroup::Prepare::FinalizeBuildBatch::FinalizeObjectManagerBatch");
						br::render::StaticObjectRequestService::FinalizeStaticImportBuildBatch(prepared->transactionBuild);
					}
				}
			}
			{
				BT_ZONE_SCOPE("SARP::StaticGroup::Prepare::AssociateIntentTokens");
				std::unordered_map<std::uint64_t, std::shared_ptr<StaticGroupIntentToken>> tokensByGroup;
				tokensByGroup.reserve(liveTokens.size());
				for (const auto& token : liveTokens) {
					if (token) tokensByGroup.emplace(token->groupID, token);
				}
				for (auto& ready : prepared->ready) {
					if (const auto token = tokensByGroup.find(ready.groupID); token != tokensByGroup.end())
						ready.intentToken = token->second;
				}
				for (auto& blocked : prepared->blockedGroups) {
					if (const auto token = tokensByGroup.find(blocked.groupID); token != tokensByGroup.end())
						blocked.intentToken = token->second;
				}
			}
			{
				BT_ZONE_SCOPE("SARP::StaticGroup::Prepare::AssembleArtifact");
				auto published = std::make_shared<PreparedStaticGroupArtifact>();
				published->packetID = input->packetID;
				published->cellKey = input->job.inputs.front().Group().cellKey;
				published->packetRevision = context.revision;
				for (const auto& source : input->job.inputs)
					published->placementCount += source.Group().placementCount;
				published->intentTokens = std::move(liveTokens);
				published->result = std::move(*prepared);
				return br::render::ArtifactBuildResult::Ready(
					br::render::ArtifactPayload::Make<PreparedStaticGroupArtifact>(
						std::move(published)));
			}
		}

		br::render::ArtifactBuildResult BuildStaticTemplateArtifact(
			const br::render::ArtifactBuildContext& context)
		{
			const auto input = context.input.Get<StaticTemplateArtifactBuildInput>();
			if (!input) {
				return br::render::ArtifactBuildResult::Failure(
					"static template artifact input is invalid");
			}
			if (context.key != StaticTemplateArtifactAddress(input->key) ||
				input->sourceFingerprint != StaticTemplateSourceFingerprint(input->key)) {
				return br::render::ArtifactBuildResult::Failure(
					"static template artifact identity mismatch");
			}

			// The template waits on its own material only: the first build requests
			// that material's usage admission and requires it, and the graph runs the
			// build again once the admission is accepted. The checkpoint holds the
			// admission's lease, so the requirement keeps resolving on later builds.
			const auto admission = context.checkpoint.Get<StaticTemplateMaterialAdmission>();
			StaticTemplateRequest requestToQueue;
			std::shared_ptr<Material> materialToAdmit;
			std::shared_ptr<Mesh> meshToPrepare;
			std::uint64_t suspensionIdentity = 0;
			{
				std::lock_guard lock(_importState.staticTemplateGraphMutex);
				auto& completion = _importState.staticTemplateGraphCompletions[input->key];
				if (completion.sourceFingerprint != 0 &&
					completion.sourceFingerprint != input->sourceFingerprint) {
					return br::render::ArtifactBuildResult::Failure(
						"conflicting static template source fingerprint");
				}
				completion.sourceFingerprint = input->sourceFingerprint;
				if (completion.status == StaticTemplateExternalCompletion::Status::Ready) {
					auto published = std::make_shared<PublishedStaticTemplateArtifact>();
					published->key = input->key;
					published->ref = completion.ref;
					published->sourceFingerprint = completion.sourceFingerprint;
					return br::render::ArtifactBuildResult::Ready(
						br::render::ArtifactPayload::Make<PublishedStaticTemplateArtifact>(
							std::move(published)));
				}
				if (completion.status == StaticTemplateExternalCompletion::Status::Failed) {
					return br::render::ArtifactBuildResult::Failure(completion.error.empty()
						? "static template construction failed" : completion.error);
				}
				if (completion.suspensionIdentity == 0) {
					completion.suspensionIdentity =
						br::render::AllocateArtifactSuspensionIdentity();
				}
				suspensionIdentity = completion.suspensionIdentity;
				if (!completion.requestQueued) {
					const auto source = _importState.staticTemplateGraphSources.find(input->key);
					if (source == _importState.staticTemplateGraphSources.end() ||
						!source->second.mesh || !source->second.material) {
						return br::render::ArtifactBuildResult::Failure(
							"static template source version is no longer available");
					}
					if (!admission) {
						materialToAdmit = source->second.material;
						meshToPrepare = source->second.mesh;
					} else {
						const auto batch = context.Dependency<br::render::PublishedMaterialUsageBatch>(
							admission->batch.version.address);
						if (!batch) {
							return br::render::ArtifactBuildResult::Failure(
								"static template material admission is missing");
						}
						const auto materialID = source->second.material->GetMaterialID();
						const auto slot = std::ranges::find_if(batch.payload->materialSlots,
							[materialID](const auto& entry) { return entry.first == materialID; });
						if (slot == batch.payload->materialSlots.end()) {
							return br::render::ArtifactBuildResult::Failure(
								"static template material admission has no slot for its material");
						}
						completion.requestQueued = true;
						requestToQueue = source->second;
						requestToQueue.materialDataIndex = slot->second;
					}
				}
			}
			if (materialToAdmit) {
				auto* graphRequests = _shared.sceneIngestionServices.execution.stateRequests;
				auto* materials = _shared.sceneIngestionServices.materials;
				if (!graphRequests || !materials) {
					return br::render::ArtifactBuildResult::Failure(
						"static template material admission services are unavailable");
				}
				if (_pump.DirectStaticImportAvailable()) {
					const std::vector<br::render::StaticMeshTemplateRequest> resources{
						br::render::StaticMeshTemplateRequest{ meshToPrepare, materialToAdmit }
					};
					_shared.sceneIngestionServices.meshes
						->PrepareStaticMeshTemplateResourcesAsync(resources);
				}
				materials->RegisterMaterialSource(materialToAdmit);
				const bool refreshTextureBindings = _shared.sceneIngestionServices.assetRequests &&
					_shared.sceneIngestionServices.assetRequests->CanUploadTextures();
				auto batchInput = std::make_shared<br::render::MaterialUsageBatchBuildInput>();
				batchInput->capture = [materials, material = std::move(materialToAdmit), refreshTextureBindings]() {
					std::vector<br::render::MaterialUsageCapture> captures;
					captures.push_back(materials->CaptureMaterialUsage(*material, 1u, refreshTextureBindings));
					auto captured = std::make_shared<br::render::MaterialUsageAdmission>();
					captured->reservation = materials->ReserveMaterialUsage(captures);
					captured->entries.push_back(std::move(captures.front().entry));
					return std::shared_ptr<const br::render::MaterialUsageAdmission>(std::move(captured));
				};
				const br::render::ArtifactKey batchKey{
					br::render::ArtifactKind::MaterialUsageBatch,
					_importState.nextStaticTemplateMaterialBatchID.fetch_add(1, std::memory_order_relaxed) + 1,
					0 };
				const auto requested = graphRequests->Request(batchKey, 1, {},
					br::render::ArtifactPayload::Make<br::render::MaterialUsageBatchBuildInput>(
						std::move(batchInput)),
					batchKey.primaryID);
				if (!requested) {
					return br::render::ArtifactBuildResult::Failure(
						"static template material admission request was rejected");
				}
				auto checkpoint = std::make_shared<StaticTemplateMaterialAdmission>();
				checkpoint->batch = requested.Handle();
				std::vector<br::render::ArtifactRequirement> requirements{
					br::render::Exact(checkpoint->batch, br::render::ArtifactReadiness::CpuReady) };
				return br::render::ArtifactBuildResult::Needs(std::move(requirements),
					br::render::ArtifactPayload::Make<StaticTemplateMaterialAdmission>(std::move(checkpoint)));
			}
			if (requestToQueue.mesh) {
				if (_drains.PushStreaming(
						_workBus.staticImportTemplateRequestOutput, std::move(requestToQueue))) {
					_shared.staticImportWorkScheduler.Signal(
						StaticImportWorkSource::TemplatePublication);
					_drains.ScheduleWorkerDrains();
				}
			}
			return br::render::ArtifactBuildResult::Suspend(
				br::render::ArtifactSuspension::External(
					suspensionIdentity, "static template external GPU-row construction"),
				context.checkpoint);
		}

		bool ProcessStaticImportServiceCellJobForWorker()
		{
			StaticImportCellPrepareJob job;
			if (!_workBus.staticImportCellPrepareJobs.TryPop(job)) {
				return false;
			}

			BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportServiceWorker::PrepareCell");
			BT_ZONE_VALUE(job.generation);
			auto prepared = PrepareStaticCellData(job);
			BT_PLOT("SARP.StaticImportService.Worker.PreparedCellGroups", static_cast<int64_t>(prepared.groups.size()));
			BuildPreparedStaticCellLifecycle(prepared);
			(void)_drains.PushBlocking(_workBus.staticImportPreparedCellResults, std::move(prepared));
			return true;
		}

		bool ProcessStaticImportServiceAssetPublishJobForWorker()
		{
			StaticAssetPublishRequest request;
			if (!_workBus.staticImportAssetPublishJobs.TryPop(request)) {
				return false;
			}

			BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportServiceWorker::ProcessAssetPublish");
			const auto status = _staticAssets.PublishRenderableAssetIfReady(request.modelPath);
			StaticImportDependencyResultStatus resultStatus = StaticImportDependencyResultStatus::Pending;
			if (status == br::staticimport::StaticAssetStatus::Ready) {
				resultStatus = StaticImportDependencyResultStatus::Ready;
			} else if (status == br::staticimport::StaticAssetStatus::Failed ||
				status == br::staticimport::StaticAssetStatus::Unsupported ||
				status == br::staticimport::StaticAssetStatus::Invalid) {
				resultStatus = StaticImportDependencyResultStatus::Failed;
			}
			(void)_drains.PushBlocking(_workBus.staticImportAssetPublishResults, StaticImportAssetPublishResult{
				.request = std::move(request),
				.status = resultStatus
			});
			BT_PLOT("SARP.StaticImportService.Worker.AssetPublishPending", resultStatus == StaticImportDependencyResultStatus::Pending ? int64_t{ 1 } : int64_t{ 0 });
			return true;
		}

		bool ProcessStaticImportServiceMaterialVariantJobForWorker()
		{
			StaticMaterialVariantRequest request;
			if (!_workBus.staticImportMaterialVariantJobs.TryPop(request)) {
				return false;
			}

			BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportServiceWorker::ProcessMaterialVariant");
			BT_ZONE_TEXT(request.modelPath.data(), request.modelPath.size());
			bool ready = false;
			{
				BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportServiceWorker::ProcessMaterialVariant::RequestMaterialVariant");
				ready = _staticAssets.RequestMaterialVariant(request.modelPath, request.variant);
			}
			{
				BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportServiceWorker::ProcessMaterialVariant::PushResult");
				(void)_drains.PushBlocking(_workBus.staticImportMaterialVariantResults, StaticImportMaterialVariantResult{
					.request = std::move(request),
					.status = ready ? StaticImportDependencyResultStatus::Ready : StaticImportDependencyResultStatus::Pending
				});
			}
			BT_PLOT("SARP.StaticImportService.Worker.MaterialVariantOverrides", static_cast<int64_t>(_staticAssets.MaterialVariantOverrideCount(request.variant)));
			if (ready && !_workBus.staticSceneRecipeCompileJobs.Empty()) {
				_shared.staticImportWorkScheduler.Signal(StaticImportWorkSource::RecipeCompile);
			}
			return true;
		}

		bool ProcessStaticImportServiceShaderVariantJobForWorker()
		{
			StaticShaderVariantRequest request;
			if (!_workBus.staticImportShaderVariantJobs.TryPop(request)) {
				return false;
			}

			BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportServiceWorker::ProcessShaderVariant");
			StaticImportDependencyResultStatus status = StaticImportDependencyResultStatus::Failed;
			if (_shared.hasSceneIngestionServices && _shared.sceneIngestionServices.shaderVariants) {
				status = _shared.sceneIngestionServices.shaderVariants->RequestShaderVariant(request.request)
					? StaticImportDependencyResultStatus::Ready
					: StaticImportDependencyResultStatus::Pending;
			}
			(void)_drains.PushBlocking(_workBus.staticImportShaderVariantResults, StaticImportShaderVariantResult{
				.request = std::move(request),
				.status = status
			});
			BT_PLOT("SARP.StaticImportService.Worker.ShaderVariantPending", status == StaticImportDependencyResultStatus::Pending ? int64_t{ 1 } : int64_t{ 0 });
			return true;
		}

		bool ProcessStaticImportGroupAssemblyJobForWorker()
		{
			StaticGroupAssemblyJob firstJob;
			if (!_workBus.staticImportGroupAssemblyJobs.TryPop(firstJob)) return false;
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportAssembler::BuildPublicationBatch");

			// Adaptive preparation packets target roughly 1 ms and are usually much
			// smaller than the 512-group transaction ceiling. Four packets fragmented
			// a warm scene into hundreds of object-buffer and graph cuts even after all
			// groups were CPU-ready. Source-specific consumers now reserve execution
			// slots without pre-claiming tickets, so a 32-packet claim still leaves
			// independent consumers while amortizing reservation and publication cuts.
			constexpr std::uint64_t maxAssemblyPacketsPerClaim = 32;
			std::vector<StaticGroupAssemblyJob> jobs;
			jobs.reserve(maxAssemblyPacketsPerClaim);
			jobs.push_back(std::move(firstJob));
			const auto additional = _shared.staticImportWorkScheduler.ClaimAdditional(
				StaticImportWorkSource::GroupAssembly,
				maxAssemblyPacketsPerClaim - 1u);
			for (std::uint64_t index = 0; index < additional; ++index) {
				StaticGroupAssemblyJob job;
				if (!_workBus.staticImportGroupAssemblyJobs.TryPop(job)) break;
				jobs.push_back(std::move(job));
			}

			PreparedDirectStaticImportBatch batch;
			const auto flushBatch = [&]() {
				if (batch.imports.empty()) return;
				RecomputePreparedStaticPlanTotals(batch.transactionBuild.prepared);
				br::render::StaticObjectRequestService::FinalizeStaticImportBuildBatch(batch.transactionBuild);
				if (_shared.hasSceneIngestionServices && _shared.sceneIngestionServices.materials) {
					auto* materialManager = _shared.sceneIngestionServices.materials;
					batch.requiredMaterialRevision =
						materialManager->DesiredPublishedStateRevision();
					batch.requiredMaterial = materialManager->DesiredPublishedStateHandle();
				}
				if (auto* requests = _shared.hasSceneIngestionServices
					? _shared.sceneIngestionServices.execution.stateRequests : nullptr;
					requests && requests->TraceActive()) {
					for (const auto& prepared : batch.imports) {
						requests->TraceEvent(
							br::render::AsyncStateGraphTraceEventID::StaticGroupBatchQueued,
							{ br::render::ArtifactKind::StaticGroup, prepared.groupID, 0 },
							prepared.generation, 0,
							{ { batch.publishTicketID, batch.imports.size() } });
					}
				}
				(void)_drains.PushBlocking(
					_workBus.staticImportCompletedOutput, std::move(batch));
				batch = {};
			};

			for (auto& job : jobs) {
				const auto artifact = job.snapshot.payload.Get<PreparedStaticGroupArtifact>();
				if (!artifact) continue;
				const auto& result = artifact->result;
				bool chargedPacketPreparation = false;
				for (const auto& sourceReady : result.ready) {
					const auto ticketIt = job.publishTickets.find(sourceReady.groupID);
					const auto tokenIt = std::ranges::find_if(artifact->intentTokens,
						[&](const auto& token) { return token && token->groupID == sourceReady.groupID; });
					if (ticketIt == job.publishTickets.end() ||
						tokenIt == artifact->intentTokens.end() ||
						!(*tokenIt)->valid.load(std::memory_order_acquire)) {
						if (ticketIt != job.publishTickets.end()) {
							StaticImportGroupAck ack;
							ack.ticketID = ticketIt->second;
							ack.dropped.push_back(sourceReady.groupID);
							(void)_drains.PushBlocking(_workBus.staticImportGroupAcks, std::move(ack));
						}
						continue;
					}
					const auto firstGroup = sourceReady.firstBuildGroup;
					const auto groupEnd = (std::min)(
						result.transactionBuild.prepared.groups.size(),
						firstGroup + sourceReady.submittedGroupCount);
					if (groupEnd - firstGroup != sourceReady.submittedGroupCount) {
						StaticImportGroupAck ack;
						ack.ticketID = ticketIt->second;
						ack.dropped.push_back(sourceReady.groupID);
						(void)_drains.PushBlocking(_workBus.staticImportGroupAcks, std::move(ack));
						continue;
					}
					const auto importCost = sourceReady.recipePrepared
						? StaticImportTransactionCost{
							.groups = sourceReady.submittedGroupCount,
							.bytes = sourceReady.recipeImportCost.preparedBytes,
							.drawRecords = sourceReady.recipeImportCost.drawRecords,
							.activeInsertIndices = sourceReady.recipeImportCost.activeInsertIndices
						}
						: StaticImportPreparedRangeCost(
							result.transactionBuild.prepared, firstGroup,
							sourceReady.submittedGroupCount);
					if (!batch.imports.empty() &&
						StaticImportTransactionWouldExceedChunk(batch.transactionCost, importCost)) {
						flushBatch();
					}
					if (batch.imports.empty()) {
						batch.generation = job.targetGeneration;
						batch.publishTicketID = ticketIt->second;
					}

					auto ready = MakeStaticImportPublicationView(
						sourceReady, job.targetGeneration);
					ready.admissionTicketID = ticketIt->second;
					ready.firstBuildGroup = batch.transactionBuild.prepared.groups.size();
					std::shared_ptr<const void> artifactOwner = artifact;
					for (std::size_t groupIndex = firstGroup; groupIndex < groupEnd; ++groupIndex) {
						batch.transactionBuild.prepared.groups.push_back(
							MakeStaticImportGroupView(
								result.transactionBuild.prepared.groups[groupIndex], artifactOwner));
					}
					batch.transactionCost.Add(importCost);
					batch.placementCount += ready.placementCount;
					if (!chargedPacketPreparation) {
						batch.buildUs += result.buildUs;
						batch.prepareUs += result.prepareUs;
						batch.transactionBuild.buildUs += result.transactionBuild.buildUs;
						batch.transactionBuild.prepared.prepareUs += result.transactionBuild.prepared.prepareUs;
						batch.transactionBuild.prepared.transformBuildUs += result.transactionBuild.prepared.transformBuildUs;
						batch.transactionBuild.prepared.workloadBuildUs += result.transactionBuild.prepared.workloadBuildUs;
						batch.transactionBuild.prepared.drawRecordBuildUs += result.transactionBuild.prepared.drawRecordBuildUs;
						chargedPacketPreparation = true;
					}
					batch.groupIDs.push_back(ready.groupID);
					if (job.version) batch.preparedGroupVersions.push_back(job.version);
					TrimPreparedDirectStaticImportForTransactionService(ready);
					batch.imports.push_back(std::move(ready));
				}
			}
			flushBatch();
			return true;
		}

		bool ProcessStaticImportGroupArtifactReleaseForWorker()
		{
			std::vector<br::render::ArtifactKey> keys;
			_workBus.staticImportGroupArtifactReleaseJobs.Drain(keys, 4096);
			std::vector<std::shared_ptr<StaticGroupLifecycleRecord>> retiredRecords;
			_workBus.staticImportLifecycleReleaseJobs.Drain(retiredRecords, 4096);
			if (keys.empty() && retiredRecords.empty()) return false;
			BT_ZONE_SCOPE(
				"SARP::LiveSceneBridge::StaticImportServiceWorker::ReleaseGroupArtifacts");
			if (!keys.empty()) if (auto* requests = _shared.hasSceneIngestionServices
				? _shared.sceneIngestionServices.execution.stateRequests : nullptr)
				requests->ReleaseBatch(keys);
			// retiredRecords intentionally destructs here on a cleanup worker.
			if (!_workBus.staticImportGroupArtifactReleaseJobs.Empty() ||
				!_workBus.staticImportLifecycleReleaseJobs.Empty())
				_shared.staticImportWorkScheduler.Signal(
					StaticImportWorkSource::GroupArtifactRelease);
			return true;
		}

		bool ProcessStaticImportTransactionMaterializeJobForWorker()
		{
			StaticImportTransactionPrepareJob job;
			if (!_workBus.staticImportTransactionMaterializeJobs.TryPop(job)) {
				return false;
			}
			if (!_shared.hasSceneIngestionServices || !_shared.sceneIngestionServices.objects) {
				return false;
			}

			BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportServiceWorker::PrepareTransaction");
			auto* objectManager = _shared.sceneIngestionServices.objects;
			br::render::StaticImportReservation reservation;
			std::vector<br::render::StaticImportReservation> reservations;
			br::render::StaticImportBuildBatch* build =
				std::addressof(job.batch.transactionBuild);
			const auto statuses = objectManager->TryReserveStaticImportTransactionsInPlace(
				std::span<br::render::StaticImportBuildBatch*>{ std::addressof(build), 1u },
				reservations);
			const auto status = statuses.empty()
				? br::render::StaticImportReservationStatus::Empty
				: statuses.front();
			if (status == br::render::StaticImportReservationStatus::PendingResources) {
				// The host-side allocation probe is only a snapshot. Another worker may
				// consume the generation before this job commits its ranges. Return the
				// immutable batch to the graph/resource gate instead of waiting here.
				(void)_drains.PushBlocking(
					_workBus.staticImportCompletedOutput, std::move(job.batch));
				return true;
			}
			if (status == br::render::StaticImportReservationStatus::Empty ||
				reservations.empty()) {
				for (const auto& prepared : job.batch.imports) {
					StaticImportGroupAck ack;
					ack.ticketID = prepared.admissionTicketID;
					ack.dropped.push_back(prepared.groupID);
					(void)_drains.PushBlocking(
						_workBus.staticImportGroupAcks, std::move(ack));
				}
				return true;
			}
			reservation = std::move(reservations.front());
			MaterializedDirectStaticImportBatch result;
			result.batch = std::move(job.batch);
			result.transaction = objectManager->MaterializeStaticImportTransaction(
				std::move(reservation),
				result.batch.transactionBuild);
			// The reservation fixes every destination range, so immutable CPU rows can
			// enter the versioned journals here without waiting for host publication.
			// Skinned draw records are the sole exception: their placement indices are
			// assigned at activation and patched before those rows are staged.
			objectManager->StageStaticImportTransactionUploads(
				result.transaction,
				result.transaction.skinnedAssemblyPlacements.empty());
			if (auto* requests = _shared.sceneIngestionServices.execution.stateRequests;
				requests && requests->TraceActive()) {
				for (const auto& prepared : result.batch.imports) {
					requests->TraceEvent(br::render::AsyncStateGraphTraceEventID::StaticGroupMaterialized,
						{ br::render::ArtifactKind::StaticGroup, prepared.groupID, 0 },
						prepared.generation, 0,
						{ { result.batch.publishTicketID, result.batch.transactionCost.groups,
							result.batch.transactionCost.bytes } });
				}
			}
			(void)_drains.PushBlocking(_workBus.staticImportMaterializedValidationRequests, std::move(result));
			return true;
		}

		StaticImportGroupPrepareResult PrepareStaticImportGroupJob(
			StaticImportGroupPrepareJob job)
		{
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportServiceWorker::ProcessGroupBatch");
			BT_PLOT("SARP.StaticImportService.GroupWorker.Inputs", static_cast<int64_t>(job.inputs.size()));
			StaticImportGroupPrepareResult result;
			result.generation = job.generation;
			result.inputGroupIDs.reserve(job.inputs.size());
			for (const auto& input : job.inputs) result.inputGroupIDs.push_back(input.groupID);
			const StaticTemplateRefMap emptyTemplateRefs;
			const auto& templateRefs = job.templateRefs ? *job.templateRefs : emptyTemplateRefs;

			const auto buildBegin = std::chrono::steady_clock::now();
			std::vector<br::render::StaticGroupBuildInfo> flatBuildInfos;
			br::render::PreparedStaticGroupsBulkPlan recipeObjectPlan;
			result.ready.reserve(job.inputs.size());
			{
				BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportServiceWorker::ClassifyGroups");
				for (const auto& input : job.inputs) {
					auto rejectGroup = [&result, &input](std::string reason) {
						const auto* descriptor = input.record ? std::addressof(input.Group()) : nullptr;
						result.unsupported.push_back({ input.groupID,
							descriptor ? descriptor->placementCount : 0,
							descriptor ? std::string{ descriptor->ModelPath() } : std::string{},
							std::move(reason) });
					};
					if (!input.record) {
						rejectGroup("missing lifecycle record");
						continue;
					}
					const auto& group = input.Group();
					auto blockGroup = [&result, &input, &rejectGroup](
						std::vector<StaticImportDependencyRequest> dependencies,
						std::shared_ptr<const StaticGroupPreparationCheckpoint> checkpoint) {
						if (dependencies.empty()) {
							rejectGroup("dependency discovery produced no requests");
							return;
						}
						result.blockedGroups.push_back(StaticImportBlockedGroup{
							.groupID = input.groupID,
							.dependencies = std::move(dependencies),
							.checkpoint = std::move(checkpoint)
						});
					};
					const auto checkpointStage = input.checkpoint
						? input.checkpoint->stage
						: StaticGroupPreparationCheckpoint::Stage::DiscoverDependencies;
					// A prepared recipe can probe its namespace/global variant binding first.
					// Once that immutable binding exists, rescanning every imported mesh merely
					// to rediscover that all templates are present is O(groups * meshes) work.
					// BindDirectStaticGroupFromRecipe reports the first genuinely missing
					// template; only that cold-variant path performs the complete request scan
					// below. Uncached groups retain eager dependency discovery.
					if (checkpointStage == StaticGroupPreparationCheckpoint::Stage::DiscoverDependencies &&
						!group.recipePreparedAuthoritative) {
						std::vector<StaticTemplateRequest> missingTemplateRequests;
						CollectMissingDirectStaticTemplateRequestsFromReadyParts(
							group, templateRefs, missingTemplateRequests);
						if (!missingTemplateRequests.empty()) {
							std::vector<StaticImportDependencyRequest> dependencies;
							dependencies.reserve(missingTemplateRequests.size());
							for (auto& request : missingTemplateRequests) {
								dependencies.push_back(
									MakeStaticImportDependencyRequest(std::move(request)));
							}
							auto checkpoint = std::make_shared<StaticGroupPreparationCheckpoint>();
							checkpoint->stage = StaticGroupPreparationCheckpoint::Stage::BuildFinal;
							blockGroup(std::move(dependencies), std::move(checkpoint));
							continue;
						}
					}
					DirectStaticReadyGroup ready;
					DirectStaticPendingReason pendingReason = DirectStaticPendingReason::AssetNotReady;
					const bool hasPreparedRecipe = group.recipePreparedAuthoritative;
					const auto recipePlanFirstGroup = recipeObjectPlan.groups.size();
					const auto buildResult = hasPreparedRecipe
						? BindDirectStaticGroupFromRecipe(
							input.groupID, group, templateRefs, ready, recipeObjectPlan, std::addressof(pendingReason))
						: BuildDirectStaticGroupFromReadyAssets(
							input.groupID, group, templateRefs, ready, std::addressof(pendingReason));
					if (buildResult == DirectStaticBuildResult::Ready) {
						auto shaderDependencies = CollectDirectStaticShaderVariantDependencies(ready);
						if (!shaderDependencies.empty()) {
							if (hasPreparedRecipe) recipeObjectPlan.groups.resize(recipePlanFirstGroup);
							auto checkpoint = std::make_shared<StaticGroupPreparationCheckpoint>();
							checkpoint->stage = StaticGroupPreparationCheckpoint::Stage::BuildFinal;
							blockGroup(std::move(shaderDependencies), std::move(checkpoint));
							continue;
						}
						PreparedDirectStaticImport preparedImport;
						preparedImport.generation = job.generation;
						preparedImport.groupID = input.groupID;
						preparedImport.cellKey = group.cellKey;
						preparedImport.batchKey = group.batchKey;
						preparedImport.groupRevision = group.groupRevision;
						preparedImport.residencyClass = group.residencyClass;
						preparedImport.residencyGeneration = group.residencyGeneration;
						preparedImport.placementCount = group.placementCount;
						preparedImport.recipePrepared = hasPreparedRecipe;
						if (hasPreparedRecipe) preparedImport.recipeImportCost = group.recipeImportCost;
						preparedImport.firstBuildGroup = hasPreparedRecipe ? recipePlanFirstGroup : flatBuildInfos.size();
						preparedImport.submittedGroupCount = hasPreparedRecipe
							? recipeObjectPlan.groups.size() - recipePlanFirstGroup
							: ready.buildInfos.size();
						preparedImport.ready = std::move(ready);
						preparedImport.extensionHolds = CollectStaticGroupHolds(
							preparedImport.ready,
							preparedImport.cellKey);
						if (hasPreparedRecipe) {
							recipeObjectPlan.transformRows += group.recipeImportCost.transformRows;
							recipeObjectPlan.drawRecords += group.recipeImportCost.drawRecords;
							recipeObjectPlan.preparedBytes += group.recipeImportCost.preparedBytes;
						} else {
							flatBuildInfos.insert(
								flatBuildInfos.end(),
								std::make_move_iterator(preparedImport.ready.buildInfos.begin()),
								std::make_move_iterator(preparedImport.ready.buildInfos.end()));
						}
						preparedImport.ready.buildInfos.clear();
						result.ready.push_back(std::move(preparedImport));
						continue;
					}
					if (hasPreparedRecipe) recipeObjectPlan.groups.resize(recipePlanFirstGroup);
					if (buildResult == DirectStaticBuildResult::EmptyOrUnsupported) {
						rejectGroup("asset contains no supported renderable parts");
						continue;
					}
					if (const auto unsupported = _staticAssets.KnownUnsupportedStaticImportReason(group.ModelPath())) {
						rejectGroup(*unsupported);
						continue;
					}
					if (_staticAssets.HasKnownFailure(group.ModelPath())) {
						rejectGroup("asset cache failed unexpectedly");
						continue;
					}

					if (pendingReason == DirectStaticPendingReason::TemplateNotReady) {
						std::vector<StaticTemplateRequest> missingTemplateRequests;
						CollectMissingDirectStaticTemplateRequestsFromReadyParts(group, templateRefs, missingTemplateRequests);
						std::vector<StaticImportDependencyRequest> dependencies;
						dependencies.reserve(missingTemplateRequests.size());
						for (auto& request : missingTemplateRequests) {
							dependencies.push_back(MakeStaticImportDependencyRequest(std::move(request)));
						}
						auto checkpoint = std::make_shared<StaticGroupPreparationCheckpoint>();
						checkpoint->stage = StaticGroupPreparationCheckpoint::Stage::BuildFinal;
						blockGroup(std::move(dependencies), std::move(checkpoint));
					} else if (!group.MaterialVariant().overrides) {
						const auto status = _staticAssets.QueueOrPollRenderableAsset(group.ModelPath());
						if (status == br::staticimport::StaticAssetStatus::Failed ||
							status == br::staticimport::StaticAssetStatus::Unsupported ||
							status == br::staticimport::StaticAssetStatus::Invalid) {
							rejectGroup("asset publish failed");
						} else {
							std::vector<StaticImportDependencyRequest> dependencies;
							dependencies.push_back(MakeStaticImportDependencyRequest(_staticAssets, StaticAssetPublishRequest{
								.modelPath = std::string{ group.ModelPath() }
							}));
							blockGroup(std::move(dependencies), input.checkpoint);
						}
					} else {
						const auto baseStatus = _staticAssets.QueueOrPollRenderableAsset(group.ModelPath());
						if (baseStatus == br::staticimport::StaticAssetStatus::Ready) {
							std::vector<StaticImportDependencyRequest> dependencies;
							// The request shares the group's overrides; its hash is their
							// content hash, which keys the material variant dependency.
							const auto& materialVariant = group.MaterialVariant();
							dependencies.push_back(MakeStaticImportDependencyRequest(_staticAssets, StaticMaterialVariantRequest{
								.modelPath = std::string{ group.ModelPath() },
								.variant = {
									.hash = _staticAssets.MaterialVariantContentHash(materialVariant),
									.overrides = materialVariant.overrides
								}
							}));
							blockGroup(std::move(dependencies), input.checkpoint);
						} else if (baseStatus == br::staticimport::StaticAssetStatus::Failed ||
							baseStatus == br::staticimport::StaticAssetStatus::Unsupported ||
							baseStatus == br::staticimport::StaticAssetStatus::Invalid) {
							rejectGroup("base asset publish failed before material variant");
						} else {
							std::vector<StaticImportDependencyRequest> dependencies;
							dependencies.push_back(MakeStaticImportDependencyRequest(_staticAssets, StaticAssetPublishRequest{
								.modelPath = std::string{ group.ModelPath() }
							}));
							blockGroup(std::move(dependencies), input.checkpoint);
						}
					}
				}
			}
			result.buildUs = ElapsedUs(buildBegin);

			if (!flatBuildInfos.empty() || !recipeObjectPlan.groups.empty()) {
				const auto prepareBegin = std::chrono::steady_clock::now();
				br::render::PreparedStaticGroupsBulkPlan objectPlan = std::move(recipeObjectPlan);
				{
					BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportServiceWorker::PrepareObjectPlan");
					const auto recipeGroupCount = objectPlan.groups.size();
					if (!flatBuildInfos.empty()) {
						auto legacyPlan = br::render::StaticObjectRequestService::PrepareStaticGroupsBulkPlan(flatBuildInfos);
						objectPlan.transformRows += legacyPlan.transformRows;
						objectPlan.drawRecords += legacyPlan.drawRecords;
						objectPlan.preparedBytes += legacyPlan.preparedBytes;
						objectPlan.groups.insert(
							objectPlan.groups.end(),
							std::make_move_iterator(legacyPlan.groups.begin()),
							std::make_move_iterator(legacyPlan.groups.end()));
						for (auto& preparedImport : result.ready) {
							if (!preparedImport.recipePrepared) preparedImport.firstBuildGroup += recipeGroupCount;
						}
					}
					if (objectPlan.preparedBytes == 0 && !objectPlan.groups.empty()) {
						RecomputePreparedStaticPlanTotals(objectPlan);
					}
				}
				result.prepareUs = ElapsedUs(prepareBegin);
				if (_shared.hasSceneIngestionServices && _shared.sceneIngestionServices.objects) {
					BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportServiceWorker::RequestPacketResources");
					br::render::StaticImportBuildBatch resourcePlan;
					{
						BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportServiceWorker::RequestPacketResources::MovePreparedPlan");
						BT_PLOT("SARP.StaticImportService.RequestPacketResources.Groups", static_cast<int64_t>(objectPlan.groups.size()));
						BT_PLOT("SARP.StaticImportService.RequestPacketResources.TransformRows", static_cast<int64_t>(objectPlan.transformRows));
						BT_PLOT("SARP.StaticImportService.RequestPacketResources.DrawRecords", static_cast<int64_t>(objectPlan.drawRecords));
						BT_PLOT("SARP.StaticImportService.RequestPacketResources.PreparedBytes", static_cast<int64_t>(objectPlan.preparedBytes));
						resourcePlan.prepared = std::move(objectPlan);
					}
					resourcePlan.buildUs = result.buildUs;
					{
						BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportServiceWorker::RequestPacketResources::RequestObjectManagerResources");
						_shared.sceneIngestionServices.objects->RequestStaticImportTransactionResources(resourcePlan);
					}
					result.transactionBuild = std::move(resourcePlan);
				} else {
					result.transactionBuild.prepared = std::move(objectPlan);
					result.transactionBuild.buildUs = result.buildUs;
				}
			}

			std::size_t templateDependencies = 0;
			std::size_t assetDependencies = 0;
			std::size_t materialVariantDependencies = 0;
			std::size_t shaderVariantDependencies = 0;
			for (const auto& blockedGroup : result.blockedGroups) {
				for (const auto& dependency : blockedGroup.dependencies) {
					templateDependencies += dependency.key.kind == StaticImportDependencyKind::Template ? 1 : 0;
					assetDependencies += dependency.key.kind == StaticImportDependencyKind::Asset ? 1 : 0;
					materialVariantDependencies += dependency.key.kind == StaticImportDependencyKind::MaterialVariant ? 1 : 0;
					shaderVariantDependencies += dependency.key.kind == StaticImportDependencyKind::ShaderVariant ? 1 : 0;
				}
			}
			BT_PLOT("SARP.StaticImportService.GroupWorker.Ready", static_cast<int64_t>(result.ready.size()));
			BT_PLOT("SARP.StaticImportService.GroupWorker.Blocked", static_cast<int64_t>(result.blockedGroups.size()));
			BT_PLOT("SARP.StaticImportService.GroupWorker.TemplateDependencies", static_cast<int64_t>(templateDependencies));
			BT_PLOT("SARP.StaticImportService.GroupWorker.AssetDependencies", static_cast<int64_t>(assetDependencies));
			BT_PLOT("SARP.StaticImportService.GroupWorker.MaterialVariantDependencies", static_cast<int64_t>(materialVariantDependencies));
			BT_PLOT("SARP.StaticImportService.GroupWorker.ShaderVariantDependencies", static_cast<int64_t>(shaderVariantDependencies));
			return result;
		}

		bool ProcessStaticImportMaterializedDecisionCleanupForWorker()
		{
			std::list<StaticImportMaterializedPublishDecision> cleanup;
			{
				std::lock_guard lock(_importState.staticImportServiceMutex);
				if (_importState.staticImportMaterializedDecisionCleanupBacklog.empty()) {
					return false;
				}
				cleanup.splice(cleanup.end(), _importState.staticImportMaterializedDecisionCleanupBacklog);
			}

			{
				BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportServiceWorker::MaterializedDecisionCleanup");
				BT_ZONE_VALUE(static_cast<int64_t>(cleanup.size()));
				BT_PLOT("SARP.StaticDirect.MaterializedDecisionCleanup", static_cast<int64_t>(cleanup.size()));
				cleanup.clear();
			}
			return true;
		}

		bool ProcessStaticImportRemovalCleanupForWorker()
		{
			std::vector<br::render::StaticObjectRemovalPayload> removalPayloads;
			{
				std::lock_guard lock(_importState.staticImportServiceMutex);
				if (_importState.staticImportRemovalPayloadCleanupBacklog.empty()) {
					return false;
				}
				removalPayloads.swap(_importState.staticImportRemovalPayloadCleanupBacklog);
			}

			{
				BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportServiceWorker::RemovalCleanup");
				BT_ZONE_VALUE(static_cast<int64_t>(removalPayloads.size()));
				BT_PLOT("SARP.StaticDirect.RemovalPayloadCleanup", static_cast<int64_t>(removalPayloads.size()));
				removalPayloads.clear();
			}
			return true;
		}

		bool TryAdmitStaticImportPublicationCut(StaticImportPublicationResult& result)
		{
			if (!_shared.hasSceneIngestionServices || !_shared.sceneIngestionServices.objects) return false;
			auto* objectManager = _shared.sceneIngestionServices.objects;
			(void)objectManager->PublishDesiredBufferState();
			const auto objectCut = objectManager->DesiredBufferStateCut();
			_importState.staticImportPublicationRequiredCoverage.store(
				result.requiredObjectMutationGeneration, std::memory_order_release);
			_importState.staticImportPublicationObservedCoverage.store(
				objectCut.coveredMutationGeneration, std::memory_order_release);
			if (!objectCut) {
				basic_telemetry::AddCounter(
					"SARP.StaticImport.PublicationAdmissionMissingObjectCut");
				return false;
			}
			if (objectCut.coveredMutationGeneration < result.requiredObjectMutationGeneration) {
				basic_telemetry::AddCounter(
					"SARP.StaticImport.PublicationAdmissionStaleObjectCut");
				return false;
			}
			auto* requests = _shared.sceneIngestionServices.execution.stateRequests;
			if (!requests) return false;
			result.objectBufferCut = objectCut.version;
			result.objectBufferVersions = objectCut.bufferVersions;
			// A transaction needs the object data submitted for upload, not
			// published. The draw-records root only builds once the scene geometry
			// covering these records is resident, and that geometry is cut from
			// pages this transaction feeds, so depending on the root would form a
			// cycle. UploadSubmitted is the milestone the root itself requires of
			// these versions, which is what waiting on the root used to imply.
			//
			// Ordering only: the transaction never reads these buffers. An Exact
			// edge made every transaction pin the object-buffer versions of its cut
			// for as long as its groups stayed live, so each cut of a long load
			// retained a full set of object-buffer backings. A ready gate
			// authorizes the build without pinning; this result's handles keep the
			// versions alive until the transaction has built.
			std::vector<br::render::ArtifactRequirement> objectBufferRequirements;
			objectBufferRequirements.reserve(result.objectBufferVersions.size());
			for (const auto& version : result.objectBufferVersions) {
				if (version) objectBufferRequirements.push_back(
					br::render::ReadyGate(version.version, br::render::ArtifactReadiness::UploadSubmitted));
			}
			for (auto& transactionCut : result.transactionCuts) {
				if (transactionCut.version || !transactionCut.input) continue;
				const auto& input = transactionCut.input;
				const auto request = requests->Request(
					{ br::render::ArtifactKind::StaticTransaction, input->transactionID, 0 },
					(std::max<std::uint64_t>)(input->streamGeneration, 1u),
					objectBufferRequirements,
					br::render::ArtifactPayload::Make<br::render::StaticTransactionBuildInput>(input),
					input->sourceFingerprint == 0 ? 1u : input->sourceFingerprint);
				if (!request) {
					// Posting only refuses on shutdown or on a malformed intent
					// (unknown input type, missing fingerprint). A real refusal of
					// an admitted cut now arrives on the version, through
					// abandonPublication below. Shutdown is not an error.
					if (request.status == br::render::ArtifactRequestStatus::ShuttingDown) {
						return false;
					}
					throw std::runtime_error("graph rejected a static publication transaction cut");
				}
				transactionCut.version = request.Handle();
			}
			RegisterPublicationCutAwaiters(result, *requests);
			return true;
		}

		// GPU readiness of an admitted cut is observed through exact awaiters
		// registered here, on the publication worker. Their continuations publish
		// transaction-ready events and flip the result's ready flag; the bridge's
		// update path only reads that flag.
		// The update path emits a job every frame from whatever decisions arrived,
		// but each cut waits for its own object-buffer root and ObjectManager seals
		// roots only every few frames. Publishing jobs one by one left dozens queued
		// behind that cadence. Fold every queued job, in order, into this cut up to
		// the cut bounds; a job that does not fit is carried to the next cut.
		void MergeQueuedPublicationJobs(StaticImportPublicationJob& job)
		{
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportServiceWorker::MergeQueuedPublicationJobs");
			const auto maximumGroups = static_cast<std::uint64_t>(
				_staticImportSettings.maxPublicationCutGroups);
			const auto maximumBytes = static_cast<std::uint64_t>(
				_staticImportSettings.maxPublicationCutBytes);
			std::size_t merged = 0;
			StaticImportPublicationJob next;
			while (job.cost.groups < maximumGroups && job.cost.bytes < maximumBytes &&
				_workBus.staticImportPublicationJobs.TryPop(next)) {
				if (job.cost.groups + next.cost.groups > maximumGroups ||
					job.cost.bytes + next.cost.bytes > maximumBytes) {
					_importState.staticImportPublicationCarryJob.emplace(std::move(next));
					_importState.staticImportPublicationCarryPending.store(true, std::memory_order_release);
					break;
				}
				job.decisions.splice(job.decisions.end(), next.decisions);
				job.cost.Add(next.cost);
				++merged;
			}
			BT_PLOT("SARP.StaticPublication.MergedJobs", static_cast<int64_t>(merged));
		}

		void RegisterPublicationCutAwaiters(StaticImportPublicationResult& result,
			br::render::RendererStateRequestService& requests)
		{
			result.awaiters.clear();
			result.gpuReady = std::make_shared<std::atomic_bool>(false);
			const auto finishPublication = [this, ready = result.gpuReady] {
				ready->store(true, std::memory_order_release);
				_shared.staticImportWorkScheduler.Signal(StaticImportWorkSource::Publication);
				_drains.ScheduleWorkerDrains();
			};
			// A cut whose version terminated never becomes GPU-ready. Do not mark
			// the publication ready: publishing a StaticTransactionReady for it
			// would advertise membership for groups whose transaction was never
			// built.
			const auto abandonPublication = [this](
					const br::render::ArtifactTermination& termination) {
				basic_telemetry::AddCounter("SARP.StaticImport.PublicationCutTerminated");
				spdlog::warn("StaticImport: publication cut ({},{}) rev {} terminated: {}",
					static_cast<unsigned>(termination.version.address.kind),
					termination.version.address.primaryID, termination.version.revision,
					termination.error);
				_shared.staticImportWorkScheduler.Signal(StaticImportWorkSource::Publication);
				_drains.ScheduleWorkerDrains();
			};
			if (result.transactionCuts.empty()) {
				std::vector<br::render::ArtifactVersionHandle> versions;
				for (const auto& version : result.objectBufferVersions) if (version) versions.push_back(version);
				if (versions.empty()) {
					finishPublication();
					return;
				}
				auto pending = std::make_shared<std::atomic_size_t>(versions.size());
				for (auto& version : versions) {
					result.awaiters.push_back(requests.AwaitExact(
						std::move(version), br::render::ArtifactReadiness::UploadSubmitted,
						TaskLane::FrameCritical, TaskDomain::RendererState,
						[finishPublication, pending](const br::render::ArtifactSnapshot&) {
							if (pending->fetch_sub(1, std::memory_order_acq_rel) == 1u) finishPublication();
						},
						abandonPublication));
				}
				return;
			}
			auto remaining = std::make_shared<std::atomic_size_t>(result.transactionCuts.size());
			result.awaiters.reserve(result.transactionCuts.size());
			for (const auto& cut : result.transactionCuts) {
				result.awaiters.push_back(requests.AwaitExact(
					cut.version, br::render::ArtifactReadiness::GpuReady,
					TaskLane::Streaming, TaskDomain::StaticImportControl,
					[this, remaining, finishPublication,
					 ticketID = cut.ticketID, input = cut.input](
						const br::render::ArtifactSnapshot& snapshot) {
						_importState.staticTransactionReadyEventsPublished.fetch_add(
							1, std::memory_order_relaxed);
						_importState.staticTransactionReadyGroupsPublished.fetch_add(
							input ? input->groups.size() : 0, std::memory_order_relaxed);
						(void)_drains.PushBlocking(_workBus.staticTransactionReadyEvents,
							StaticTransactionReady{ ticketID, snapshot.Version(), input });
						_drains.ScheduleCoordinatorDrain();
						if (remaining->fetch_sub(1, std::memory_order_acq_rel) == 1u) {
							finishPublication();
						}
					},
					abandonPublication));
			}
		}

		bool ProcessStaticImportPublicationJobForWorker()
		{
			// Publication mutates ordered append journals. Although the surrounding
			// worker pool is parallel, one consumer must preserve queue order.
			std::unique_lock publicationLock(_importState.staticImportPublicationWorkerMutex, std::try_to_lock);
			if (!publicationLock.owns_lock()) {
				// Preserve the level-triggered claim without blocking shutdown behind a
				// different acceptance task. Publication itself is capped at one consumer,
				// so this retry represents cross-source contention only.
				if (_importState.staticImportPublicationPendingAdmission ||
					_importState.staticImportPublicationCarryPending.load(std::memory_order_acquire) ||
					!_workBus.staticImportPublicationJobs.Empty()) {
					_shared.staticImportWorkScheduler.Signal(StaticImportWorkSource::Publication);
					_drains.ScheduleWorkerDrains();
				}
				return false;
			}
			if (_importState.staticImportPublicationPendingAdmission) {
				if (!TryAdmitStaticImportPublicationCut(*_importState.staticImportPublicationPendingAdmission)) {
					return false;
				}
				(void)_drains.PushStreaming(_workBus.staticImportPublicationResults,
					std::move(*_importState.staticImportPublicationPendingAdmission));
				_importState.staticImportPublicationPendingAdmission.reset();
				_importState.staticImportPublicationAdmissionPending.store(false, std::memory_order_release);
				return true;
			}
			// Cuts do not wait for their predecessor to become GPU-ready. The graph
			// pins each cut's exact object-buffer version, ObjectManager paces its own
			// roots, and the coordinator validates every group against its admission
			// ticket, so completion order across cuts does not matter. The update path
			// commits results in publication order (see DrainReadyDiffs).
			StaticImportPublicationJob job;
			if (auto& carry = _importState.staticImportPublicationCarryJob) {
				job = std::move(*carry);
				carry.reset();
				_importState.staticImportPublicationCarryPending.store(false, std::memory_order_release);
			} else if (!_workBus.staticImportPublicationJobs.TryPop(job)) {
				return false;
			}
			if (!_shared.hasSceneIngestionServices || !_shared.sceneIngestionServices.objects) return false;
			MergeQueuedPublicationJobs(job);

			BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportServiceWorker::PublishTransactionCut");
			const auto begin = std::chrono::steady_clock::now();
			std::vector<br::render::MaterializedStaticImportTransaction*> transactions;
			transactions.reserve(job.decisions.size());
			for (auto& decision : job.decisions) {
				transactions.push_back(std::addressof(decision.item.transaction));
			}
			StaticImportPublicationResult result;
			auto* objectManager = _shared.sceneIngestionServices.objects;
			result.publication = objectManager->PublishStaticImportTransactionsBulk(transactions);
			result.requiredObjectMutationGeneration =
				result.publication.mutationCoverageGeneration;
			// Construct immutable graph ownership on the publication worker while
			// the materialized batches and ObjectManager retirement payloads are
			// already colocated. The main thread must not rebuild this closure.
			if (_shared.sceneIngestionServices.execution.stateRequests) {
				std::size_t transactionIndex = 0;
				result.transactionCuts.reserve(job.decisions.size());
				for (auto& decision : job.decisions) {
					auto& batch = decision.item.batch;
					auto* published = transactionIndex < result.publication.transactions.size()
						? std::addressof(result.publication.transactions[transactionIndex]) : nullptr;
					++transactionIndex;
					std::size_t removalCursor = 0;
					auto input = std::make_shared<br::render::StaticTransactionBuildInput>();
					input->transactionID = batch.publishTicketID;
					input->streamGeneration = batch.generation;
					for (auto& prepared : batch.imports) {
						br::render::StaticObjectRemovalPayload removal;
						if (published) {
							for (std::size_t i = 0;
								i < prepared.submittedGroupCount &&
								removalCursor < published->removalPayloads.size();
								++i, ++removalCursor) {
								const auto& part = published->removalPayloads[removalCursor];
								if (!part.drawRecordIndices.empty()) {
									auto partCopy = part;
									AppendStaticObjectRemovalPayload(removal, partCopy);
								}
							}
						}
						if (prepared.hostOwner) continue;
						if (removal.drawRecordIndices.empty()) {
							StaticImportGroupAck bridgeAck;
							bridgeAck.ticketID = prepared.admissionTicketID;
							bridgeAck.dropped.push_back(prepared.groupID);
							result.bridgeAcks.push_back(std::move(bridgeAck));
							continue;
						}
						auto ownership = std::make_shared<StaticGraphGroupOwnership>();
						ownership->groupID = prepared.groupID;
						ownership->cellKey = prepared.cellKey;
						ownership->generation = prepared.generation;
						ownership->batchKey = prepared.batchKey;
						ownership->groupRevision = prepared.groupRevision;
						ownership->instanceCount = prepared.ready.sceneObject.instanceCount;
						ownership->residencyClass = prepared.residencyClass;
						ownership->workloadKeys = prepared.ready.sceneObject.directWorkloadKeys;
						ownership->recipeVariantBinding = prepared.ready.recipeVariantBinding;
						ownership->extensionHolds = prepared.extensionHolds;
						ownership->removalPayload =
							std::make_shared<const br::render::StaticObjectRemovalPayload>(
								std::move(removal));
						const auto drawRecords = ownership->removalPayload->drawInfoCount;
						input->groups.push_back({ prepared.groupID,
							prepared.admissionTicketID, drawRecords, drawRecords,
							prepared.placementCount, std::move(ownership) });
						input->drawRecordCount += drawRecords;
						input->activeEntryCount += drawRecords;
						input->placementCount += prepared.placementCount;
					}
					if (input->groups.empty()) continue;
					input->groupCount = input->groups.size();
					std::uint64_t fingerprint = input->transactionID ^ input->streamGeneration;
					for (const auto& group : input->groups) {
						fingerprint ^= group.groupID + 0x9e3779b97f4a7c15ull +
							(fingerprint << 6u) + (fingerprint >> 2u);
					}
					input->sourceFingerprint = fingerprint;
					result.transactionCuts.push_back({ batch.publishTicketID,
						{}, std::move(input) });
				}
			}
			result.publishUs = ElapsedUs(begin);
			result.cost = job.cost;
			result.decisions = std::move(job.decisions);
			if (TryAdmitStaticImportPublicationCut(result)) {
				(void)_drains.PushStreaming(_workBus.staticImportPublicationResults, std::move(result));
			} else {
				_importState.staticImportPublicationPendingAdmission.emplace(std::move(result));
				_importState.staticImportPublicationAdmissionPending.store(true, std::memory_order_release);
				// Close the completion-before-subscription window. The readiness edge
				// may have fired after the first probe but before the pending flag was
				// visible to the callback.
				if (TryAdmitStaticImportPublicationCut(
						*_importState.staticImportPublicationPendingAdmission)) {
					(void)_drains.PushStreaming(
						_workBus.staticImportPublicationResults,
						std::move(*_importState.staticImportPublicationPendingAdmission));
					_importState.staticImportPublicationPendingAdmission.reset();
					_importState.staticImportPublicationAdmissionPending.store(false, std::memory_order_release);
				}
			}
			return true;
		}

		bool ProcessStaticSceneRecipeCompileForWorker()
		{
			StaticSceneRecipeCompileJob job;
			if (!_workBus.staticSceneRecipeCompileJobs.TryPop(job) || !job.recipe) return false;
			BT_ZONE_SCOPE("SARP::StaticSceneRecipe::CompileRuntimeMiss");
			for (; job.nextGroupIndex < job.recipe->groups.size(); ++job.nextGroupIndex) {
				auto& source = job.recipe->groups[job.nextGroupIndex];
				const auto variant = _staticAssets.MakeMaterialVariant(source.materialOverrideHash,
					std::vector<br::staticimport::recipe::MaterialOverride>(source.materialOverrides));
				const auto* parts = _staticAssets.TryGetReadyParts(source.modelPath, variant);
				if (!parts) {
					if (const auto unsupported = _staticAssets.KnownUnsupportedStaticImportReason(source.modelPath)) {
						spdlog::warn("SARP runtime recipe skips unsupported model='{}' placements={} reason={}",
							source.modelPath, source.placementFormIDs.size(), *unsupported);
						job.unsupportedGroupIDs.push_back(source.stableGroupID);
						continue;
					}
					if (_staticAssets.HasKnownFailure(source.modelPath)) {
						spdlog::error("SARP runtime recipe compilation failed unexpectedly for model='{}'; cell will not be published as authoritative",
							source.modelPath);
						std::lock_guard lock(_importState.staticSceneRecipeCompileMutex);
						_importState.staticSceneRecipeCompiles.erase({ job.recipe->key, job.recipe->sourceFingerprint });
						return true;
					}
					(void)_staticAssets.QueueOrPollRenderableAsset(source.modelPath);
					(void)_workBus.staticSceneRecipeCompileJobs.TryPush(std::move(job));
					return false;
				}
				std::vector<DirectX::XMMATRIX> matrices;
				matrices.reserve(source.instanceWorldMatrices.size());
				for (const auto& stored : source.instanceWorldMatrices) {
					DirectX::XMFLOAT4X4 value;
					std::memcpy(std::addressof(value), stored.values, sizeof(value));
					matrices.push_back(DirectX::XMLoadFloat4x4(std::addressof(value)));
				}
				auto cached = PrepareStaticRecipeGroupFromReadyParts(
					source.stableGroupID, job.batchKey, matrices, *parts);
				if (cached.parts.empty()) {
					if (!ClassifyDirectStaticGroupFromReadyParts(
							source.stableGroupID, job.batchKey, matrices, *parts).empty()) {
						spdlog::error("SARP runtime recipe preparation unexpectedly produced no parts for model='{}'", source.modelPath);
						std::lock_guard lock(_importState.staticSceneRecipeCompileMutex);
						_importState.staticSceneRecipeCompiles.erase({ job.recipe->key, job.recipe->sourceFingerprint });
						return true;
					}
					spdlog::warn("SARP runtime recipe skips model with no supported parts='{}' placements={}",
						source.modelPath, source.placementFormIDs.size());
					job.unsupportedGroupIDs.push_back(source.stableGroupID);
					continue;
				}
				job.preparedParts.insert(job.preparedParts.end(),
					std::make_move_iterator(cached.parts.begin()),
					std::make_move_iterator(cached.parts.end()));
				source.preparedAuthoritative = true;
			}
			DiscardUnsupportedStaticRecipeGroups(*job.recipe, job.unsupportedGroupIDs);
			job.recipe->preparedParts = std::move(job.preparedParts);
			br::staticimport::recipe::FinalizeImportCosts(*job.recipe, sizeof(InstanceDrawRecordCB), _staticImportSettings);
			{
				std::lock_guard lock(_importState.staticSceneRecipeCompileMutex);
				_importState.staticSceneRecipeCompiles.erase({ job.recipe->key, job.recipe->sourceFingerprint });
			}
			QueueStaticSceneRecipePersistence(std::move(job.recipe));
			return true;
		}

		void RecordStaticRetirementFailure(std::exception_ptr failure)
		{
			std::lock_guard lock(_importState.staticRetirementMutex);
			if (!_importState.staticRetirementFailure) _importState.staticRetirementFailure = std::move(failure);
		}

		bool ProcessStaticRetirementApplyForWorker()
		{
			std::unique_lock publicationLock(_importState.staticImportPublicationWorkerMutex, std::try_to_lock);
			if (!publicationLock.owns_lock()) {
				_shared.staticImportWorkScheduler.Signal(StaticImportWorkSource::RetirementApply);
				return false;
			}
			if (!_shared.hasSceneIngestionServices || !_shared.sceneIngestionServices.objects) return false;

			// Removals are per group now; the cell-named setting caps groups per pass.
			const auto configuredCap = _staticImportSettings.maxRemoveCellsPerFrame;
			const auto batchCap = configuredCap == 0
				? std::size_t{ 4096 } : (std::min<std::size_t>)(4096, configuredCap);
			std::vector<StaticGroupRemoval> removals;
			_workBus.staticImportRemovalOutput.Drain(removals, batchCap);
			std::vector<StaticRetirementOwnershipRequest> requestedOwnerships;
			_workBus.staticRetirementOwnershipRequests.Drain(requestedOwnerships, 4096);
			std::vector<br::render::StaticObjectRemovalPayload> requestedPayloads;
			_workBus.staticRetirementPayloadRequests.Drain(requestedPayloads, 4096);
			if (removals.empty() && requestedOwnerships.empty() && requestedPayloads.empty()) return false;

			const auto source = br::render::PublishedStateSource::ProcessSource();
			const auto manifest = source ? source->Load() : nullptr;
			const auto scene = manifest
				? manifest->geometry.payload.Get<br::render::PublishedStaticSceneState>() : nullptr;

			PendingStaticRetirement pending;
			std::vector<br::render::StaticObjectRemovalPayload> payloads = std::move(requestedPayloads);
			const auto claim = [&](const std::shared_ptr<const StaticGraphGroupOwnership>& ownership,
				bool acknowledge) {
				if (!ownership || !ownership->TryClaimRetirement()) return;
				pending.ownerships.push_back(ownership);
				if (ownership->removalPayload) payloads.push_back(*ownership->removalPayload);
				if (acknowledge) pending.unpublished.push_back({ ownership->groupID, ownership->groupRevision });
			};
			for (const auto& removal : removals) {
				// A removal claims the published ownership of its group only when that
				// ownership comes from an add the removal follows. A re-add of the same
				// id after the removal carries a larger revision and is left alone: its
				// publication replaces (and so retires) whatever older ownership the
				// scene still holds.
				const auto* group = scene ? scene->FindGroup(removal.groupID) : nullptr;
				auto ownership = group && group->ownership
					? std::static_pointer_cast<const StaticGraphGroupOwnership>(group->ownership) : nullptr;
				if (!ownership || ownership->groupRevision > removal.groupRevision) continue;
				if (ownership->residencyClass == StaticImportResidencyClass::CellStatic &&
					ownership->cellKey != StaticCellKey{} &&
					std::ranges::none_of(pending.invalidatedCells, [&](const StaticCellKey& cell) {
						return cell == ownership->cellKey;
					})) {
					pending.invalidatedCells.push_back(ownership->cellKey);
				}
				claim(ownership, true);
			}
			for (const auto& request : requestedOwnerships)
				claim(request.ownership, request.unpublish);
			if (pending.ownerships.empty() && payloads.empty()) {
				if (_workBus.staticImportRemovalOutput.Size() != 0 || _workBus.staticRetirementOwnershipRequests.Size() != 0 ||
					_workBus.staticRetirementPayloadRequests.Size() != 0)
					_shared.staticImportWorkScheduler.Signal(StaticImportWorkSource::RetirementApply);
				return true;
			}

			try {
				auto* objectManager = _shared.sceneIngestionServices.objects;
				br::render::RemoveObjectsBulkOptions options;
				options.deferBufferRangeRetirement = true;
				options.retireInstanceDrawRecordRanges = false;
				options.retireFrame = objectManager->MakeDeferredRetireFrame();
				pending.requiredMutationGeneration =
					objectManager->RemoveStaticObjectsBulk(payloads, options).mutationCoverageGeneration;
			} catch (...) {
				for (const auto& ownership : pending.ownerships) ownership->RollbackRetirement();
				RecordStaticRetirementFailure(std::current_exception());
				return true;
			}
			{
				std::lock_guard lock(_importState.staticRetirementMutex);
				_importState.staticRetirementsAwaitingPublication.push_back(std::move(pending));
			}
			_shared.staticImportWorkScheduler.Signal(StaticImportWorkSource::RetirementCommit);
			if (_workBus.staticImportRemovalOutput.Size() != 0 || _workBus.staticRetirementOwnershipRequests.Size() != 0 ||
				_workBus.staticRetirementPayloadRequests.Size() != 0)
				_shared.staticImportWorkScheduler.Signal(StaticImportWorkSource::RetirementApply);
			_drains.ScheduleWorkerDrains();
			return true;
		}

		bool ProcessStaticRetirementCommitForWorker()
		{
			const auto source = br::render::PublishedStateSource::ProcessSource();
			const auto manifest = source ? source->Load() : nullptr;
			const auto objectState = manifest
				? manifest->drawRecords.payload.Get<br::render::PublishedObjectBufferState>() : nullptr;
			const auto indirectState = manifest
				? manifest->indirectWorkloads.payload.Get<br::render::PublishedIndirectState>() : nullptr;
			if (!manifest || !objectState || !indirectState ||
				indirectState->drawRecordsRoot != manifest->drawRecords.publicationRoot) return false;

			std::vector<PendingStaticRetirement> completed;
			{
				std::lock_guard lock(_importState.staticRetirementMutex);
				while (!_importState.staticRetirementsAwaitingPublication.empty() &&
					objectState->coveredMutationGeneration >=
						_importState.staticRetirementsAwaitingPublication.front().requiredMutationGeneration) {
					completed.push_back(std::move(_importState.staticRetirementsAwaitingPublication.front()));
					_importState.staticRetirementsAwaitingPublication.pop_front();
				}
			}
			if (completed.empty()) return false;
			for (auto& batch : completed) {
				StaticRetirementCompletion completion;
				completion.unpublished = std::move(batch.unpublished);
				for (const auto& ownership : batch.ownerships) {
					if (!ownership || !ownership->CompleteRetirement()) continue;
					if (ownership->residencyClass == StaticImportResidencyClass::CellStatic) {
						for (std::size_t extensionIndex = 0; extensionIndex < _staticExtensions.size(); ++extensionIndex) {
							_staticExtensions[extensionIndex]->OnGroupRetired(ownership->groupID,
								br::staticimport::StaticGroupHoldAt(ownership->extensionHolds, extensionIndex));
						}
					}
					if (ownership->ConsumeApplied()) {
						++completion.groups;
						completion.placements += ownership->instanceCount;
					}
				}
				for (const auto& cell : batch.invalidatedCells) {
					for (auto* extension : _staticExtensions) {
						extension->OnCellRetired(cell);
					}
				}
				if (!completion.unpublished.empty()) {
					StaticImportGroupAck ack;
					ack.unpublished = completion.unpublished;
					(void)_drains.PushBlocking(_workBus.staticImportGroupAcks, std::move(ack));
					_drains.ScheduleCoordinatorDrain();
				}
				(void)_drains.PushBlocking(_workBus.staticRetirementCompletions, std::move(completion), false);
			}
			return true;
		}

		bool ProcessStaticImportWorkerSource(StaticImportWorkSource source)
		{
			switch (source) {
			case StaticImportWorkSource::Publication:
				return ProcessStaticImportPublicationJobForWorker();
			case StaticImportWorkSource::TemplatePublication: {
				// Both publication sources are off-thread serialized acceptance lanes.
				// Waiting here is bounded by the current acceptance cut and preserves the
				// level-triggered ticket; timer polling created an unbounded control queue.
				std::lock_guard publicationLock(_importState.staticImportPublicationWorkerMutex);
				const auto before = _workBus.staticImportTemplateRequestOutput.Size();
				_importState.staticTemplatePublicationNeedsBackoff.store(false, std::memory_order_release);
				_pump.DrainTemplateRequests(StaticStreamBudget{});
				const bool hasRunnableInput = !_workBus.staticImportTemplateRequestOutput.Empty();
				if (hasRunnableInput) {
					if (_importState.staticTemplatePublicationNeedsBackoff.load(std::memory_order_acquire)) {
						_pump.ScheduleStaticTemplatePublicationRetry();
					} else {
						_shared.staticImportWorkScheduler.Signal(StaticImportWorkSource::TemplatePublication);
					}
				}
				// Only queued input merits another claim: requests arrive with their
				// material already admitted by their template's build.
				return before != _workBus.staticImportTemplateRequestOutput.Size() || hasRunnableInput;
			}
			case StaticImportWorkSource::RetirementApply:
				return ProcessStaticRetirementApplyForWorker();
			case StaticImportWorkSource::RetirementCommit:
				return ProcessStaticRetirementCommitForWorker();
			case StaticImportWorkSource::MaterializedDecisionCleanup:
				return ProcessStaticImportMaterializedDecisionCleanupForWorker();
			case StaticImportWorkSource::RemovalCleanup:
				return ProcessStaticImportRemovalCleanupForWorker();
			case StaticImportWorkSource::GroupArtifactRelease:
				return ProcessStaticImportGroupArtifactReleaseForWorker();
			case StaticImportWorkSource::GroupAssembly:
				return ProcessStaticImportGroupAssemblyJobForWorker();
			case StaticImportWorkSource::TransactionMaterialize:
				return ProcessStaticImportTransactionMaterializeJobForWorker();
			case StaticImportWorkSource::AssetPublish:
				return ProcessStaticImportServiceAssetPublishJobForWorker();
			case StaticImportWorkSource::MaterialVariant:
				return ProcessStaticImportServiceMaterialVariantJobForWorker();
			case StaticImportWorkSource::ShaderVariant:
				return ProcessStaticImportServiceShaderVariantJobForWorker();
			case StaticImportWorkSource::HostDependency:
			case StaticImportWorkSource::HostPrepare:
				return _staticImportAccess.RunHostWork(source);
			case StaticImportWorkSource::CellPrepare:
				return ProcessStaticImportServiceCellJobForWorker();
			case StaticImportWorkSource::RecipeCompile:
				return ProcessStaticSceneRecipeCompileForWorker();
			case StaticImportWorkSource::SpeculativeAssetPublish: {
				BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportServiceWorker::PublishSpeculativeAssets");
				std::unique_lock sweepLock(_importState.staticImportSpeculativeAssetPublishMutex, std::try_to_lock);
				if (!sweepLock.owns_lock()) return false;
				const auto published = _staticAssets.PublishReadyRenderableAssets(16);
				if (published != 0) {
					_importState.staticImportAssetReadinessGeneration.fetch_add(1,
						std::memory_order_release);
					_drains.ScheduleCoordinatorDrain();
				}
				// A "ready" cache entry can still be temporarily inadmissible. Only
				// continue a coalesced sweep after this claim made forward progress;
				// otherwise the next cache completion edge will wake the source. Re-
				// signalling an unproductive claim here creates an unbounded drain loop.
				if (published != 0 && _staticAssets.HasReadyRenderableAssets()) {
					_shared.staticImportWorkScheduler.Signal(
						StaticImportWorkSource::SpeculativeAssetPublish);
				}
				return published != 0;
			}
			case StaticImportWorkSource::Count:
				break;
			}
			return false;
		}

		void StaticImportServiceWorkerDrain(
			StaticImportWorkSource source,
			const br::TaskContext& context)
		{
			BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportServiceWorker::SourceDrain");
			BT_ZONE_VALUE(static_cast<std::int64_t>(source));
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
			std::uint32_t processed = 0;
			// The cooperative deadline is the fairness boundary. A second item-count
			// quota made cheap packets resubmit thousands of drains and changed total
			// throughput with packet size and machine speed.
			while (std::chrono::steady_clock::now() < deadline &&
				!context.StopRequested() && !_shared.staticImportServiceStop.load(std::memory_order_acquire)) {
				auto claim = _shared.staticImportWorkScheduler.TryClaim(source);
				if (!claim) break;
				bool productive = false;
				try {
					BT_ZONE_SCOPE("SARP::LiveSceneBridge::StaticImportServiceWorker::Tick");
					productive = ProcessStaticImportWorkerSource(claim->source);
				} catch (const std::exception& ex) {
					spdlog::error("SARP static import worker: job failed with exception: {}", ex.what());
				} catch (...) {
					spdlog::error("SARP static import worker: job failed with unknown exception");
				}
				_shared.staticImportWorkScheduler.Complete(*claim, productive);
				if (!productive) {
					basic_telemetry::AddCounter("SARP.StaticImport.WorkerStaleClaim");
					break;
				}
				++processed;
			}
			_importState.staticImportSourceDrainsScheduled[static_cast<std::size_t>(source)].fetch_sub(
				1, std::memory_order_acq_rel);
			_importState.staticImportWorkerDrainsActive.fetch_sub(1, std::memory_order_acq_rel);
			if (processed != 0) {
				_drains.ScheduleCoordinatorDrain();
			}
			// Decrement-before-recheck closes both sides of the enqueue/exit race:
			// either this drain sees the published ticket, or its producer sees the slot.
			if (_shared.staticImportWorkScheduler.HasPending(source)) _drains.ScheduleWorkerDrains();
		}

		void DrainStaticSceneRecipePersistence()
		{
			std::vector<std::shared_ptr<const br::staticimport::recipe::CellRecipe>> pending;
			_workBus.staticSceneRecipePersistenceQueue.Drain(pending, 64);
			if (!pending.empty() && _importState.staticSceneRecipeStore) {
				std::unordered_map<std::uint64_t,
					std::vector<std::shared_ptr<const br::staticimport::recipe::CellRecipe>>> namespaces;
				for (auto& recipe : pending) {
					const auto key = recipe->key.interiorCellFormID != 0
						? (1ull << 63u) | recipe->key.interiorCellFormID
						: recipe->key.worldFormID;
					namespaces[key].push_back(std::move(recipe));
				}
				for (const auto& [namespaceKey, recipes] : namespaces) {
					if (StaticSceneRecipePersistenceDisabled(namespaceKey)) continue;
					std::vector<const br::staticimport::recipe::CellRecipe*> views;
					views.reserve(recipes.size());
					for (const auto& recipe : recipes) views.push_back(recipe.get());
					const auto publish = _importState.staticSceneRecipeStore->PublishCells(views);
					BT_PLOT("SARP.StaticSceneRecipe.RuntimePublishBytes", static_cast<int64_t>(publish.bytesWritten));
					BT_PLOT("SARP.StaticSceneRecipe.RuntimePublishSuccess", publish.success ? int64_t{ 1 } : int64_t{ 0 });
					if (!publish.success) {
						{
							std::lock_guard lock(_importState.staticSceneRecipePersistenceStateMutex);
							_importState.staticSceneRecipePersistenceDisabledNamespaces.insert(namespaceKey);
						}
						spdlog::warn("SARP static recipe runtime persistence disabled for namespace 0x{:016X} after publish failure: {}",
							namespaceKey, publish.reason);
					}
				}
			}
			_importState.staticSceneRecipePersistenceScheduled.store(false, std::memory_order_release);
			if (!_workBus.staticSceneRecipePersistenceQueue.Empty()) {
				ScheduleStaticSceneRecipePersistence();
			}
		}

		bool StaticSceneRecipePersistenceDisabled(std::uint64_t namespaceKey) const
		{
			std::lock_guard lock(_importState.staticSceneRecipePersistenceStateMutex);
			return _importState.staticSceneRecipePersistenceDisabledNamespaces.contains(namespaceKey);
		}

		static std::uint64_t StaticSceneRecipeNamespaceKey(
			const br::staticimport::recipe::CellKey& key)
		{
			return key.interiorCellFormID != 0
				? (1ull << 63u) | key.interiorCellFormID : key.worldFormID;
		}

		void ScheduleStaticSceneRecipePersistence()
		{
			if (_shared.staticImportServiceStop.load(std::memory_order_acquire)) return;
			bool expected = false;
			if (!_importState.staticSceneRecipePersistenceScheduled.compare_exchange_strong(
				expected, true, std::memory_order_acq_rel)) return;
			const auto submitted = TaskSchedulerManager::GetInstance().ScheduleAfter(
				_shared.staticStreamingScope, std::chrono::milliseconds(100), TaskLane::Background,
				TaskDomain::Cleanup, "SARP::StaticSceneRecipe::PersistBatch",
				[this](const br::TaskContext& context) {
					if (!context.StopRequested() && !_shared.staticImportServiceStop.load(std::memory_order_acquire)) {
						DrainStaticSceneRecipePersistence();
					} else {
						_importState.staticSceneRecipePersistenceScheduled.store(false, std::memory_order_release);
					}
				});
			if (!submitted) _importState.staticSceneRecipePersistenceScheduled.store(false, std::memory_order_release);
		}

		void QueueStaticSceneRecipePersistence(
			std::shared_ptr<const br::staticimport::recipe::CellRecipe> recipe)
		{
			if (!recipe || StaticSceneRecipePersistenceDisabled(
				StaticSceneRecipeNamespaceKey(recipe->key))) return;
			(void)_workBus.staticSceneRecipePersistenceQueue.TryPush(std::move(recipe));
			ScheduleStaticSceneRecipePersistence();
		}

		PreparedStaticCell PrepareStaticCellData(const StaticImportCellPrepareJob& job)
		{
			BT_ZONE_SCOPE("SARP::StaticImportWorker::PrepareStaticCellData");
			const auto prepareBegin = std::chrono::steady_clock::now();
			const auto& cellKey = job.key;
			PreparedStaticCell prepared;
			prepared.key = cellKey;
			prepared.generation = job.generation;
			prepared.recipeSceneBinding = job.recipeSceneBinding;
			if (!_placements.IsRealCell(cellKey)) {
				BT_PLOT("SARP.StaticSceneRecipe.NonexistentCellSkipped", int64_t{ 1 });
				prepared.prepareUs = ElapsedUs(prepareBegin);
				return prepared;
			}
			for (std::size_t extensionIndex = 0; extensionIndex < _staticExtensions.size(); ++extensionIndex) {
				const auto request = _staticExtensions[extensionIndex]->PrepareCell(cellKey);
				if (request.requested) {
					prepared.hostCellPrepares.push_back(PreparedStaticCell::HostCellPrepare{
						.extension = extensionIndex,
						.value = request.value,
					});
				}
			}

			const br::staticimport::recipe::CellKey recipeKey{
				cellKey.worldFormID, cellKey.interiorCellFormID, cellKey.cellX, cellKey.cellY };
			std::shared_ptr<br::staticimport::recipe::CellRecipe> cellRecipe;
			std::shared_ptr<br::staticimport::recipe::CellRecipeView> mappedRecipe = job.mappedRecipe;
			if (_importState.staticSceneRecipeStore && !mappedRecipe) {
				BT_ZONE_SCOPE("SARP::StaticSceneRecipe::LoadCell");
				auto pack = job.mappedRecipePack;
				if (!pack) pack = _importState.staticSceneRecipeStore->OpenNamespace(recipeKey).pack;
				if (pack) {
					auto scene = pack->LookupCells(std::span{ &recipeKey, 1 });
					BT_PLOT("SARP.StaticSceneRecipe.MappedBytes", static_cast<int64_t>(scene.mappedBytes));
					if (!scene.cells.empty() && scene.cells.front().status == br::staticimport::recipe::LoadStatus::Hit) {
						mappedRecipe = std::make_shared<br::staticimport::recipe::CellRecipeView>(std::move(scene.cells.front().view));
					}
				}
				BT_PLOT("SARP.StaticSceneRecipe.CacheHit", mappedRecipe ? int64_t{ 1 } : int64_t{ 0 });
			}

			if (!cellRecipe && !mappedRecipe) {
				BT_ZONE_SCOPE("SARP::StaticImportWorker::BuildMissGroups");
				cellRecipe = std::make_shared<br::staticimport::recipe::CellRecipe>(
					_placements.BuildCellRecipe(recipeKey));
				if (_importState.staticSceneRecipeStore &&
					_staticImportSettings.recipeCacheMode == br::staticimport::StaticRecipeCacheMode::ReadWrite) {
					// Live ingestion moves strings and placement arrays out of its owning
					// recipe below.  The fallback compiler therefore receives an independent
					// owning copy and can wait for assets without racing scene ingestion.
					auto compileRecipe = std::make_shared<br::staticimport::recipe::CellRecipe>(*cellRecipe);
					const StaticSceneRecipeCompileKey compileKey{
						compileRecipe->key, compileRecipe->sourceFingerprint };
					bool inserted = false;
					{
						std::lock_guard lock(_importState.staticSceneRecipeCompileMutex);
						inserted = _importState.staticSceneRecipeCompiles.insert(compileKey).second;
					}
					if (inserted) {
						(void)_workBus.staticSceneRecipeCompileJobs.TryPush({ std::move(compileRecipe),
							static_cast<std::uint64_t>(StaticCellKeyHash{}(cellKey)) });
						_shared.staticImportWorkScheduler.Publish(StaticImportWorkSource::RecipeCompile);
						_drains.ScheduleWorkerDrains();
					}
				}
			}

			if (mappedRecipe) {
				BT_ZONE_SCOPE("SARP::StaticSceneRecipe::MaterializeMappedMetadata");
				prepared.skippedCount = static_cast<std::size_t>(mappedRecipe->SkippedPlacements());
				prepared.placementCount = prepared.skippedCount;
				prepared.groups.reserve(mappedRecipe->Groups().size());
				const auto mappedParts = mappedRecipe->PreparedParts();
				std::size_t nextPreparedPart = 0;
				for (const auto& source : mappedRecipe->Groups()) {
					auto& group = prepared.groups.emplace_back();
					group.groupID = source.stableGroupID;
					group.materialVariant.hash = source.materialOverrideHash;
					group.mappedModelPath = mappedRecipe->String(source.modelPath);
					group.placementCount = static_cast<std::size_t>(source.placementFormIDs.count);
					group.recipePreparedAuthoritative = (source.flags & 1u) != 0;
					group.recipeImportCost = source.importCost;
					group.mappedRecipe = mappedRecipe;
					group.recipeSceneBinding = job.recipeSceneBinding;
					if (job.recipeSceneBinding) {
						const StaticRecipeVariantLookupKey lookupKey{
							group.mappedModelPath, source.materialOverrideHash };
						std::lock_guard variantsLock(job.recipeSceneBinding->variantsMutex);
						if (const auto found = job.recipeSceneBinding->variantByKey.find(lookupKey);
							found != job.recipeSceneBinding->variantByKey.end()) {
							group.recipeVariantSlot = found->second;
						} else {
							auto slot = std::make_shared<StaticRecipeSceneVariantSlot>();
							slot->modelPath = group.mappedModelPath;
							slot->materialVariant = _staticAssets.MakeMaterialVariant(*mappedRecipe, source);
							{
								std::shared_lock bindingLock(_importState.staticRecipeVariantBindingMutex);
								if (const auto cached = _importState.staticRecipeVariantBindings.find(lookupKey);
									cached != _importState.staticRecipeVariantBindings.end()) {
									slot->binding = cached->second;
									slot->readyBinding.store(slot->binding.get(), std::memory_order_relaxed);
								}
							}
							group.recipeVariantIndex = static_cast<std::uint32_t>(job.recipeSceneBinding->variants.size());
							job.recipeSceneBinding->variants.push_back(slot);
							job.recipeSceneBinding->variantByKey.emplace(
								StaticRecipeVariantKey{ slot->modelPath, slot->materialVariant.hash }, slot);
							group.recipeVariantSlot = std::move(slot);
						}
					}
					// Large scenes discover variants in independent cell jobs. Start the
					// shared base-asset request exactly once when the slot first appears,
					// giving callback-driven publication a head start over group admission.
					if (group.recipeVariantSlot &&
						!group.recipeVariantSlot->readyBinding.load(std::memory_order_acquire) &&
						!group.recipeVariantSlot->assetPrimeRequested.exchange(true, std::memory_order_acq_rel)) {
						(void)_staticAssets.QueueOrPollRenderableAsset(group.mappedModelPath);
					}
					if (nextPreparedPart < mappedParts.size() &&
						mappedParts[nextPreparedPart].sourceGroupID == source.stableGroupID) {
						const auto first = nextPreparedPart;
						while (nextPreparedPart < mappedParts.size() &&
							mappedParts[nextPreparedPart].sourceGroupID == source.stableGroupID) {
							++nextPreparedPart;
						}
						group.mappedPreparedParts = { first, nextPreparedPart - first };
					} else {
						const auto found = std::ranges::find(mappedParts, source.stableGroupID,
							&br::staticimport::recipe::MappedPreparedPart::sourceGroupID);
						if (found != mappedParts.end()) {
							const auto first = static_cast<std::size_t>(std::distance(mappedParts.begin(), found));
							auto last = found;
							while (last != mappedParts.end() && last->sourceGroupID == source.stableGroupID) ++last;
							group.mappedPreparedParts = { first, static_cast<std::size_t>(std::distance(found, last)) };
						}
					}
					prepared.placementCount += group.placementCount;
					if (!group.recipeVariantSlot &&
						group.recipeVariantIndex == std::numeric_limits<std::uint32_t>::max()) {
						group.materialVariant = _staticAssets.MakeMaterialVariant(*mappedRecipe, source);
					}
				}
				BT_PLOT("SARP.StaticSceneRecipe.OwningMatrixRows", int64_t{ 0 });
				BT_PLOT("SARP.StaticSceneRecipe.OwningPreparedRowBytes", int64_t{ 0 });
				BT_PLOT("SARP.StaticImportWorker.CellGroups", static_cast<int64_t>(prepared.groups.size()));
				BT_PLOT("SARP.StaticImportWorker.CellSkippedPlacements", static_cast<int64_t>(prepared.skippedCount));
				prepared.prepareUs = ElapsedUs(prepareBegin);
				return prepared;
			}

			prepared.skippedCount = static_cast<std::size_t>(cellRecipe->skippedPlacements);
			prepared.placementCount = prepared.skippedCount;
			prepared.groups.reserve(cellRecipe->groups.size());
			std::unordered_map<std::uint64_t, std::vector<br::staticimport::recipe::PreparedPart>> preparedPartsByGroup;
			for (auto& part : cellRecipe->preparedParts) {
				preparedPartsByGroup[part.sourceGroupID].push_back(std::move(part));
			}
			for (auto& source : cellRecipe->groups) {
				auto& group = prepared.groups.emplace_back();
				group.groupID = source.stableGroupID;
				group.modelPath = std::move(source.modelPath);
				group.name = std::move(source.name);
				group.placementCount = source.placementFormIDs.size();
				group.recipePreparedAuthoritative = source.preparedAuthoritative;
				group.recipeImportCost = source.importCost;
				if (auto it = preparedPartsByGroup.find(source.stableGroupID); it != preparedPartsByGroup.end()) {
					group.recipePreparedParts = std::move(it->second);
				}
				prepared.placementCount += group.placementCount;
				group.instanceWorldMatrices.reserve(source.instanceWorldMatrices.size());
				for (const auto& matrix : source.instanceWorldMatrices) {
					group.instanceWorldMatrices.push_back(DirectX::XMLoadFloat4x4(
						reinterpret_cast<const DirectX::XMFLOAT4X4*>(matrix.values)));
				}
				group.placementIDs.assign(source.placementFormIDs.begin(), source.placementFormIDs.end());
				group.materialVariant = _staticAssets.MakeMaterialVariant(
					source.materialOverrideHash, std::move(source.materialOverrides));
			}
			BT_PLOT("SARP.StaticImportWorker.CellGroups", static_cast<int64_t>(prepared.groups.size()));
			BT_PLOT("SARP.StaticImportWorker.CellSkippedPlacements", static_cast<int64_t>(prepared.skippedCount));
			prepared.prepareUs = ElapsedUs(prepareBegin);
			return prepared;
		}

		std::size_t LiveStaticObjectCount() const
		{
			const auto source = br::render::PublishedStateSource::ProcessSource();
			const auto manifest = source ? source->Load() : nullptr;
			const auto graphScene = manifest
				? manifest->geometry.payload.Get<br::render::PublishedStaticSceneState>()
				: nullptr;
			// The selected scene is the visibility authority for graph-owned static
			// groups (including host groups). Legacy ECS statics remain additive.
			std::size_t residentHostGroups = 0;
			if (!graphScene) {
				for (const auto* extension : _staticExtensions) {
					residentHostGroups += extension->ResidentHostGroupCount();
				}
			}
			return graphScene
				? static_cast<std::size_t>(graphScene->groupCount)
				: _importState.staticRuntimeGroupActiveCount + residentHostGroups;
		}

		// Declared in dependency order: each of these is constructed with the ones
		// above it, so the order is load-bearing rather than incidental.
		StaticImportShared& _shared;
		const br::staticimport::StaticImportSettings _staticImportSettings;
		br::staticimport::IStaticAssetSource& _staticAssets;
		br::staticimport::IStaticPlacementSource& _placements;
		Hooks _hooks;
		StaticImportWorkBus _workBus;
		StaticImportPipelineState _importState;
		StaticStreamCounters _counters;
		br::SerializedTaskPump _staticImportCoordinatorPump;
		StaticImportCapacityBroker _staticImportCapacityBroker;
		StaticGroupLifecycleStore _staticImportLifecycleStore;
		StaticImportDrainScheduler _drains;
		StaticImportPipelineAccess _staticImportAccess;
		// In registration order; frozen once the pipeline starts.
		std::vector<br::staticimport::IStaticImportExtension*> _staticExtensions;
		StaticImportCoordinator _coordinator;
		StaticImportStreamingPump _pump;
		br::render::ArtifactObservation _graphMaterialObservation;
		br::render::ArtifactObservation _graphStaticGroupObservation;
		br::render::ArtifactObservation _graphStaticVariantObservation;
		bool _placementsReady{ false };
		bool _started{ false };
		bool _shutdown{ false };
	};

	StaticImportRuntime::StaticImportRuntime(
		StaticImportShared& shared,
		const br::staticimport::StaticImportSettings& settings,
		br::staticimport::IStaticAssetSource& assets,
		br::staticimport::IStaticPlacementSource& placements,
		Hooks hooks)
		: _impl(std::make_unique<Impl>(shared, settings, assets, placements, std::move(hooks)))
	{
	}

	StaticImportRuntime::~StaticImportRuntime() = default;

	bool StaticImportRuntime::AddExtension(br::staticimport::IStaticImportExtension& extension)
	{
		return _impl->AddExtension(extension);
	}

	bool StaticImportRuntime::RemoveExtension(br::staticimport::IStaticImportExtension& extension)
	{
		return _impl->RemoveExtension(extension);
	}

	IStaticImportAdvancedAccess& StaticImportRuntime::AdvancedAccess()
	{
		return _impl->_staticImportAccess;
	}

	void StaticImportRuntime::AttachServices(br::render::StaticSceneIngestionAccess services)
	{
		_impl->AttachServices(std::move(services));
	}

	void StaticImportRuntime::UpdateStreaming(const br::staticimport::StaticStreamTarget& target)
	{
		_impl->UpdateStreaming(target);
	}

	void StaticImportRuntime::AddOrUpdateGroups(std::vector<StaticImportGroup> groups)
	{
		_impl->AddOrUpdateGroups(std::move(groups));
	}

	void StaticImportRuntime::RemoveGroups(std::span<const std::uint64_t> groupIDs)
	{
		_impl->RemoveGroups(groupIDs);
	}

	void StaticImportRuntime::Shutdown()
	{
		_impl->Shutdown();
	}

	void StaticImportRuntime::SetBenchmarkDiagnosticsEnabled(bool enabled)
	{
		_impl->_importState.benchmarkDiagnosticsEnabled = enabled;
	}

	bool StaticImportRuntime::BenchmarkDiagnosticsEnabled() const
	{
		return _impl->_importState.benchmarkDiagnosticsEnabled;
	}

	const br::staticimport::StaticImportSettings& StaticImportRuntime::Settings() const
	{
		return _impl->_staticImportSettings;
	}

	StaticDesiredCellArea StaticImportRuntime::BuildDesiredStaticCells(const br::staticimport::StaticStreamTarget& target) const
	{
		return _impl->_pump.BuildDesiredStaticCells(target);
	}

	bool StaticImportRuntime::DirectStaticImportAvailable() const
	{
		return _impl->_pump.DirectStaticImportAvailable();
	}

	std::vector<StaticImportDependencyRequest> StaticImportRuntime::CollectShaderVariantDependencies(const DirectStaticReadyGroup& ready) const
	{
		return _impl->CollectDirectStaticShaderVariantDependencies(ready);
	}

	std::size_t StaticImportRuntime::LiveStaticObjectCount() const
	{
		return _impl->LiveStaticObjectCount();
	}

	StaticImportServiceStats StaticImportRuntime::GetStatsSnapshot() const
	{
		return _impl->_pump.GetStatsSnapshot();
	}

	std::uint64_t StaticImportRuntime::PreparationQueueDepth() const
	{
		return _impl->_pump.StaticPreparationQueueDepth();
	}

	StaticStreamBudget StaticImportRuntime::MakeStreamBudget() const
	{
		return _impl->_pump.MakeStaticStreamBudget();
	}

	br::render::ObjectStorageStats StaticImportRuntime::CaptureObjectManagerStats() const
	{
		return _impl->_pump.CaptureObjectManagerStats();
	}

	StaticStreamCounters& StaticImportRuntime::Counters()
	{
		return _impl->_counters;
	}

	const StaticStreamCounters& StaticImportRuntime::Counters() const
	{
		return _impl->_counters;
	}

	StaticImportQueueDepths StaticImportRuntime::QueueDepths() const
	{
		const auto& bus = _impl->_workBus;
		const auto& state = _impl->_importState;
		return StaticImportQueueDepths{
			.cellPrepareJobs = bus.staticImportCellPrepareJobs.Size(),
			.preparedCellResults = bus.staticImportPreparedCellResults.Size(),
			.groupChanges = bus.staticImportGroupChanges.Size(),
			.transactionMaterializeJobs = bus.staticImportTransactionMaterializeJobs.Size(),
			.transactionMaterializeBacklog = state.staticImportTransactionMaterializeBacklog.size(),
			.materializedValidationRequests = bus.staticImportMaterializedValidationRequests.Size(),
			.resourcePendingBatchBacklog = state.staticImportResourcePendingBatchBacklog.size(),
			.groupAcks = bus.staticImportGroupAcks.Size(),
			.groupAckBacklog = state.staticImportGroupAckBacklog.size(),
		};
	}

	std::optional<StaticStreamKey> StaticImportRuntime::LastStreamKey() const
	{
		return _impl->_importState.lastStaticStreamKey;
	}

	std::uint64_t StaticImportRuntime::GraphDesiredPlacementSetDigest() const
	{
		return _impl->_importState.graphDesiredPlacementSetDigest.load(std::memory_order_acquire);
	}
}
