#pragma once

// Static-import message and payload types.
//
// These structs/enums are the vocabulary the static streaming pipeline passes
// between its stages: cell prepare jobs, prepared cells and groups, transaction
// batches, publication results, retirement and ownership deltas.
//

#include <array>
#include <BasicRenderer/Streaming/ObjectRequests.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <BasicRenderer/Streaming/StaticSceneArtifacts.h>
#include <BasicRenderer/StaticImport/StaticAssetSource.h>
#include <BasicRenderer/StaticImport/StaticImportExtension.h>
#include <BasicRenderer/StaticImport/StaticSceneRecipe.h>
#include <BasicRenderer/StaticImport/StaticStreamTarget.h>
#include <BasicRenderer/StaticImport/StaticCellGeometry.h>
#include <BasicRenderer/StaticImport/Advanced/StaticImportCapacityBroker.h>
#include <BasicRenderer/StaticImport/Advanced/StaticImportRequests.h>
#include <BasicRenderer/StaticImport/Advanced/StaticImportWorkScheduler.h>
#include <BasicRenderer/StaticImport/Advanced/StaticImportSupport.h>

namespace br::staticimport
{

	// Named by types declared before it below.
	struct StaticRecipeVariantBinding;

	// What a ready group carries into publication besides its build infos: the draw
	// workloads its parts feed, its placement count and whether it publishes
	// directly as static scene content. The part names and skinned-shape indices
	// describe the parts for diagnostics.
	struct StaticReadyGroupObject
	{
		std::vector<std::string> partNames;
		std::vector<std::uint32_t> partSkinnedShapeIndices;
		std::vector<DrawWorkloadKey> directWorkloadKeys;
		std::string modelPath;
		std::size_t instanceCount{ 1 };
		bool directStatic{ false };
	};

	struct PreparedDirectStaticImportBatch;

	struct StaticImportCallbackHandoff
	{
		using WorkSource = StaticImportWorkSource;
		std::mutex mutex;
		std::function<void(WorkSource)> wake;

		void Invoke(WorkSource source)
		{
			std::lock_guard lock(mutex);
			if (wake) wake(source);
		}

		void Clear()
		{
			std::lock_guard lock(mutex);
			wake = {};
		}
	};

	using StaticTemplateRef = std::shared_ptr<const br::render::StaticMeshTemplateRef>;

	using StaticTemplateRefMap = std::unordered_map<DirectStaticTemplateKey, StaticTemplateRef, DirectStaticTemplateKeyHash>;

	using StaticTemplateRefSnapshot = std::shared_ptr<const StaticTemplateRefMap>;

	struct StaticRecipeVariantKey
	{
		std::string modelPath;
		std::uint64_t materialOverrideHash{ 0 };
		bool operator==(const StaticRecipeVariantKey&) const = default;
	};

	struct StaticRecipeVariantLookupKey
	{
		std::string_view modelPath;
		std::uint64_t materialOverrideHash{ 0 };
	};

	struct StaticRecipeVariantKeyHash
	{
		using is_transparent = void;
		std::size_t operator()(const StaticRecipeVariantKey& key) const noexcept
		{
			return (*this)(StaticRecipeVariantLookupKey{ key.modelPath, key.materialOverrideHash });
		}
		std::size_t operator()(const StaticRecipeVariantLookupKey& key) const noexcept
		{
			auto hash = std::hash<std::string_view>{}(key.modelPath);
			hash ^= std::hash<std::uint64_t>{}(key.materialOverrideHash) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
			return hash;
		}
	};

	struct StaticRecipeVariantKeyEqual
	{
		using is_transparent = void;
		bool operator()(const StaticRecipeVariantKey& lhs, const StaticRecipeVariantKey& rhs) const noexcept { return lhs == rhs; }
		bool operator()(const StaticRecipeVariantKey& lhs, const StaticRecipeVariantLookupKey& rhs) const noexcept
		{
			return lhs.materialOverrideHash == rhs.materialOverrideHash && lhs.modelPath == rhs.modelPath;
		}
		bool operator()(const StaticRecipeVariantLookupKey& lhs, const StaticRecipeVariantKey& rhs) const noexcept { return (*this)(rhs, lhs); }
	};

	struct StaticRecipeSceneVariantSlot
	{
		std::string modelPath;
		br::staticimport::StaticMaterialVariant materialVariant;
		std::mutex bindingMutex;
		std::shared_ptr<const StaticRecipeVariantBinding> binding;
		std::atomic<const StaticRecipeVariantBinding*> readyBinding{ nullptr };
		std::atomic_bool assetPrimeRequested{ false };
	};

	struct StaticRecipeSceneBinding
	{
		std::mutex variantsMutex;
		std::vector<std::shared_ptr<StaticRecipeSceneVariantSlot>> variants;
		std::unordered_map<std::uint64_t, std::uint32_t> variantByGroupID;
		std::unordered_map<StaticRecipeVariantKey, std::shared_ptr<StaticRecipeSceneVariantSlot>,
			StaticRecipeVariantKeyHash, StaticRecipeVariantKeyEqual> variantByKey;
	};

	struct DirectStaticReadyGroup
	{
		std::uint64_t groupID{ 0 };
		std::size_t placementCount{ 0 };
		StaticReadyGroupObject sceneObject;
		std::vector<br::render::StaticGroupBuildInfo> buildInfos;
		std::shared_ptr<const StaticRecipeVariantBinding> recipeVariantBinding;
	};

	struct PendingStaticGroup
	{
		StaticCellKey cellKey;
		// The group's material variant. A recipe-bound group keeps only the hash
		// here and shares its overrides through MaterialVariant().
		br::staticimport::StaticMaterialVariant materialVariant;
		std::string modelPath;
		std::string_view mappedModelPath;
		std::string name;
		std::vector<DirectX::XMMATRIX> instanceWorldMatrices;
		std::vector<std::uint32_t> placementIDs;
		std::vector<br::staticimport::recipe::PreparedPart> recipePreparedParts;
		std::shared_ptr<const br::staticimport::recipe::CellRecipeView> mappedRecipe;
		std::shared_ptr<StaticRecipeSceneBinding> recipeSceneBinding;
		std::shared_ptr<StaticRecipeSceneVariantSlot> recipeVariantSlot;
		br::staticimport::recipe::RecipeRange mappedPreparedParts;
		std::uint32_t recipeVariantIndex{ std::numeric_limits<std::uint32_t>::max() };
		bool recipePreparedAuthoritative{ false };
		br::staticimport::recipe::Group::ImportCost recipeImportCost;
		std::size_t placementCount{ 0 };
		StaticImportResidencyClass residencyClass{ StaticImportResidencyClass::CellStatic };
		std::uint64_t residencyGeneration{ 0 };
		[[nodiscard]] std::string_view ModelPath() const { return mappedModelPath.empty() ? std::string_view{ modelPath } : mappedModelPath; }
		[[nodiscard]] const br::staticimport::StaticMaterialVariant& MaterialVariant() const
		{
			if (recipeVariantSlot) return recipeVariantSlot->materialVariant;
			if (recipeSceneBinding && recipeVariantIndex < recipeSceneBinding->variants.size()) {
				return recipeSceneBinding->variants[recipeVariantIndex]->materialVariant;
			}
			return materialVariant;
		}
	};

	struct PreparedStaticGroup
	{
		std::uint64_t groupID{ 0 };
		br::staticimport::StaticMaterialVariant materialVariant;
		std::string modelPath;
		std::string_view mappedModelPath;
		std::string name;
		std::vector<DirectX::XMMATRIX> instanceWorldMatrices;
		std::vector<std::uint32_t> placementIDs;
		std::vector<br::staticimport::recipe::PreparedPart> recipePreparedParts;
		std::shared_ptr<const br::staticimport::recipe::CellRecipeView> mappedRecipe;
		std::shared_ptr<StaticRecipeSceneBinding> recipeSceneBinding;
		std::shared_ptr<StaticRecipeSceneVariantSlot> recipeVariantSlot;
		br::staticimport::recipe::RecipeRange mappedPreparedParts;
		std::uint32_t recipeVariantIndex{ std::numeric_limits<std::uint32_t>::max() };
		bool recipePreparedAuthoritative{ false };
		br::staticimport::recipe::Group::ImportCost recipeImportCost;
		std::size_t placementCount{ 0 };
		StaticImportResidencyClass residencyClass{ StaticImportResidencyClass::CellStatic };
		std::uint64_t residencyGeneration{ 0 };
		[[nodiscard]] std::string_view ModelPath() const { return mappedModelPath.empty() ? std::string_view{ modelPath } : mappedModelPath; }
	};

	enum class StaticGroupLifecycleStage : std::uint8_t
	{
		Discovered,
		Runnable,
		Preparing,
		Blocked,
		Ready,
		Published,
		Cancelled
	};

	struct StaticGroupLifecycleRecord
	{
		std::uint64_t groupID{ 0 };
		PendingStaticGroup descriptor;
		std::atomic<StaticGroupLifecycleStage> stage{
			StaticGroupLifecycleStage::Discovered };
		std::atomic<std::uint64_t> intentRevision{ 0 };
		std::optional<StaticImportCapacityBroker::Reservation> capacityReservation;
	};

	struct PreparedStaticCell
	{
		StaticCellKey key;
		std::uint64_t generation{ 0 };
		// Extensions whose PrepareCell requested OnCellHostPrepared for this cell.
		struct HostCellPrepare
		{
			std::size_t extension{ 0 };
			std::uint64_t value{ 0 };
		};
		std::vector<HostCellPrepare> hostCellPrepares;
		std::vector<PreparedStaticGroup> groups;
		std::vector<std::shared_ptr<StaticGroupLifecycleRecord>> lifecycleGroups;
		std::size_t placementCount{ 0 };
		std::size_t skippedCount{ 0 };
		std::uint64_t prepareUs{ 0 };
		std::shared_ptr<StaticRecipeSceneBinding> recipeSceneBinding;
		bool worldspaceResident{ false };
	};

	struct StaticGroupIntentToken
	{
		std::uint64_t groupID{ 0 };
		StaticCellKey cellKey;
		std::uint64_t residencyEpoch{ 0 };
		std::uint64_t revision{ 0 };
		std::atomic_bool valid{ true };
	};

	struct PreparedDirectStaticImport
	{
		std::uint64_t generation{ 0 };
		std::uint64_t groupID{ 0 };
		std::uint64_t admissionTicketID{ 0 };
		StaticCellKey cellKey;
		StaticImportResidencyClass residencyClass{ StaticImportResidencyClass::CellStatic };
		std::uint64_t residencyGeneration{ 0 };
		std::shared_ptr<StaticGroupIntentToken> intentToken;
		// Set on host imports (residency HostImport): the extension that submitted the
		// import and its payload for it.
		br::staticimport::IStaticImportExtension* hostOwner{ nullptr };
		br::staticimport::StaticHostPayload hostPayload;
		std::size_t placementCount{ 0 };
		std::size_t firstBuildGroup{ 0 };
		std::size_t submittedGroupCount{ 0 };
		bool recipePrepared{ false };
		// Recipe groups carry an authoritative resource summary.  Preserve it
		// through graph preparation so packet assembly does not walk mapped rows,
		// templates, and workload lists a second time merely to recover the same
		// publication-budget values.
		br::staticimport::recipe::Group::ImportCost recipeImportCost;
		// One per registered extension (IStaticImportExtension::CollectGroupHold).
		std::vector<br::staticimport::StaticGroupHold> extensionHolds;
		DirectStaticReadyGroup ready;
		br::render::StaticImportPacket objectPacket;
		std::uint64_t prepareUs{ 0 };
	};

	struct StaticImportTransactionCost
	{
		std::uint64_t groups{ 0 };
		std::uint64_t bytes{ 0 };
		std::uint64_t drawRecords{ 0 };
		std::uint64_t activeInsertIndices{ 0 };

		void Add(const StaticImportTransactionCost& other)
		{
			groups += other.groups;
			bytes += other.bytes;
			drawRecords += other.drawRecords;
			activeInsertIndices += other.activeInsertIndices;
		}
	};

	struct StaticImportGroupAck
	{
		struct UnpublishedGroup
		{
			std::uint64_t groupID{ 0 };
			std::uint64_t generation{ 0 };
		};

		std::uint64_t ticketID{ 0 };
		std::vector<UnpublishedGroup> unpublished;
		std::vector<std::uint64_t> requeue;
		std::vector<std::uint64_t> dropped;
	};

	struct PreparedDirectStaticImportBatch
	{
		std::uint64_t generation{ 0 };
		std::uint64_t publishTicketID{ 0 };
		std::uint64_t requiredMaterialRevision{ 0 };
		br::render::ArtifactVersionHandle requiredMaterial{};
		std::vector<PreparedDirectStaticImport> imports;
		std::vector<std::uint64_t> groupIDs;
		// Exact graph leases for immutable StaticGroup fragments contributing to
		// this DTO. Address release may run independently once these are retained.
		std::vector<br::render::ArtifactVersionHandle> preparedGroupVersions;
		br::render::StaticImportBuildBatch transactionBuild;
		StaticImportTransactionCost transactionCost;
		std::size_t placementCount{ 0 };
		std::uint64_t buildUs{ 0 };
		std::uint64_t prepareUs{ 0 };
	};

	struct StaticImportTransactionPrepareJob
	{
		PreparedDirectStaticImportBatch batch;
	};

	struct MaterializedDirectStaticImportBatch
	{
		PreparedDirectStaticImportBatch batch;
		br::render::MaterializedStaticImportTransaction transaction;
	};

	enum class StaticImportMaterializedDecisionKind : std::uint8_t
	{
		Publish,
		Cancel
	};

	struct StaticImportMaterializedPublishDecision
	{
		StaticImportMaterializedDecisionKind kind{ StaticImportMaterializedDecisionKind::Cancel };
		MaterializedDirectStaticImportBatch item;
		std::vector<StaticImportGroupAck> acks;
		std::uint64_t targetGeneration{ 0 };
		std::uint64_t livenessEpoch{ 0 };
		StaticImportTransactionCost cost;
	};

	struct StaticImportPublicationJob
	{
		std::list<StaticImportMaterializedPublishDecision> decisions;
		StaticImportTransactionCost cost;
	};

	struct StaticGraphGroupOwnership
	{
		enum class RetirementState : std::uint8_t { Active, Tombstoned, Retired };
		void MarkApplied() const noexcept
		{
			appliedCounted.store(true, std::memory_order_release);
		}
		bool ConsumeApplied() const noexcept
		{
			return appliedCounted.exchange(false, std::memory_order_acq_rel);
		}
		bool TryClaimRetirement() const noexcept
		{
			auto expected = RetirementState::Active;
			return retirementState.compare_exchange_strong(expected, RetirementState::Tombstoned,
				std::memory_order_acq_rel, std::memory_order_acquire);
		}
		void RollbackRetirement() const noexcept
		{
			auto expected = RetirementState::Tombstoned;
			(void)retirementState.compare_exchange_strong(expected, RetirementState::Active,
				std::memory_order_acq_rel, std::memory_order_acquire);
		}
		bool CompleteRetirement() const noexcept
		{
			auto expected = RetirementState::Tombstoned;
			return retirementState.compare_exchange_strong(expected, RetirementState::Retired,
				std::memory_order_acq_rel, std::memory_order_acquire);
		}
		mutable std::atomic<RetirementState> retirementState{ RetirementState::Active };
		mutable std::atomic<bool> appliedCounted{ false };
		std::uint64_t groupID{ 0 };
		StaticCellKey cellKey;
		std::uint64_t generation{ 0 };
		std::size_t instanceCount{ 0 };
		StaticImportResidencyClass residencyClass{ StaticImportResidencyClass::CellStatic };
		std::vector<DrawWorkloadKey> workloadKeys;
		std::shared_ptr<const StaticRecipeVariantBinding> recipeVariantBinding;
		std::vector<br::staticimport::StaticGroupHold> extensionHolds;
		std::shared_ptr<const br::render::StaticObjectRemovalPayload> removalPayload;
	};

	struct StaticSceneOwnershipDelta
	{
		std::vector<std::shared_ptr<const StaticGraphGroupOwnership>> added;
		std::vector<std::shared_ptr<const StaticGraphGroupOwnership>> removed;
		std::vector<std::shared_ptr<const StaticGraphGroupOwnership>> retireOnly;
	};

	struct StaticRetirementOwnershipRequest
	{
		std::shared_ptr<const StaticGraphGroupOwnership> ownership;
		bool unpublish{ false };
	};

	struct StaticRetirementCompletion
	{
		std::size_t groups{ 0 };
		std::size_t placements{ 0 };
		std::vector<StaticImportGroupAck::UnpublishedGroup> unpublished;
	};

	struct StaticImportPublicationResult
	{
		struct TransactionCut
		{
			std::uint64_t ticketID{ 0 };
			br::render::ArtifactVersionHandle version;
			std::shared_ptr<const br::render::StaticTransactionBuildInput> input;
		};
		std::list<StaticImportMaterializedPublishDecision> decisions;
		br::render::StaticImportBulkPublishResult publication;
		StaticImportTransactionCost cost;
		br::render::ArtifactVersionHandle objectBufferCut;
		// Exact object buffer versions of that cut. Transactions wait on these, not
		// on the draw-records root, which waits for resident scene geometry.
		std::vector<br::render::ArtifactVersionHandle> objectBufferVersions;
		std::vector<TransactionCut> transactionCuts;
		std::vector<StaticImportGroupAck> bridgeAcks;
		std::uint64_t requiredObjectMutationGeneration = 0;
		std::uint64_t publishUs{ 0 };
		// Registered by the publication worker when the cut is admitted. The
		// bridge polls gpuReady on the update path instead of entering the graph.
		std::vector<br::render::ArtifactAwaiter> awaiters;
		std::shared_ptr<std::atomic_bool> gpuReady;
	};

	struct StaticTransactionReady
	{
		std::uint64_t ticketID{ 0 };
		br::render::ArtifactVersionID version;
		std::shared_ptr<const br::render::StaticTransactionBuildInput> input;
		std::size_t nextGroupIndex{ 0 };
	};

	enum class StaticImportDependencyResultStatus
	{
		Ready,
		Pending,
		Failed
	};

	struct StaticRemovalDiff
	{
		StaticCellKey cellKey;
		std::uint64_t generation{ 0 };
		std::vector<std::uint64_t> groupIDs;
	};

	struct StaticRecipeVariantBinding
	{
		std::unordered_map<std::uint64_t, br::render::StaticMeshTemplateRef> templatesByOrdinal;
		std::vector<std::vector<br::render::PreparedStaticMeshTemplateRef>> preparedTemplatesByPart;
		std::vector<std::vector<std::uint64_t>> templateOrdinalsByPart;
		std::vector<std::vector<DrawWorkloadKey>> uniqueWorkloadKeysByPart;
		std::vector<std::vector<std::uint32_t>> workloadRouteIndicesByPart;
		std::vector<std::vector<br::render::PreparedStaticGroupInfo::WorkloadRouteRange>> workloadRouteRangesByPart;
		std::vector<std::vector<std::uint32_t>> workloadRouteOccurrencesByPart;
		std::vector<br::render::StaticGroupBuildInfo> shaderBuildInfos;
		std::vector<DrawWorkloadKey> workloadKeys;
		// One per registered extension (IStaticImportExtension::DescribeVariantBinding).
		std::vector<br::staticimport::StaticGroupHold> extensionHolds;
		mutable std::atomic_bool shadersReady{ false };
	};

	struct StaticImportCellPrepareJob
	{
		StaticCellKey key;
		std::uint64_t generation{ 0 };
		std::shared_ptr<br::staticimport::recipe::CellRecipeView> mappedRecipe;
		std::shared_ptr<StaticRecipeSceneBinding> recipeSceneBinding;
		std::shared_ptr<br::staticimport::recipe::MappedRecipePack> mappedRecipePack;
	};

	struct StaticSceneRecipeCompileJob
	{
		std::shared_ptr<br::staticimport::recipe::CellRecipe> recipe;
		std::size_t nextGroupIndex{ 0 };
		std::vector<br::staticimport::recipe::PreparedPart> preparedParts;
		std::vector<std::uint64_t> unsupportedGroupIDs;
	};

	struct StaticImportAssetPublishResult
	{
		StaticAssetPublishRequest request;
		StaticImportDependencyResultStatus status{ StaticImportDependencyResultStatus::Pending };
	};

	struct StaticImportMaterialVariantResult
	{
		StaticMaterialVariantRequest request;
		StaticImportDependencyResultStatus status{ StaticImportDependencyResultStatus::Pending };
	};

	struct StaticImportShaderVariantResult
	{
		StaticShaderVariantRequest request;
		StaticImportDependencyResultStatus status{ StaticImportDependencyResultStatus::Pending };
	};

	struct StaticTemplateRefsPublished
	{
		StaticTemplateRefMap refs;
		std::vector<DirectStaticTemplateKey> failedKeys;
	};

	struct StaticGroupSuspensionRequest
	{
		std::uint64_t groupID{ 0 };
		std::uint64_t revision{ 0 };
		std::uint64_t identity{ 0 };
		bool externalGraphGate{ false };
		std::vector<StaticImportDependencyRequest> dependencies;
	};

	struct StaticGroupAssemblyJob
	{
		br::render::ArtifactSnapshot snapshot;
		br::render::ArtifactVersionHandle version;
		std::uint64_t targetGeneration{ 0 };
		std::unordered_map<std::uint64_t, std::uint64_t> publishTickets;
	};

	struct StaticScenePageReady
	{
		std::uint32_t pageIndex{ 0 };
		br::render::ArtifactSnapshot snapshot;
	};

	struct StaticSceneRootPublished
	{
		br::render::ArtifactVersionID version;
		std::shared_ptr<const std::array<br::render::ArtifactVersionID,
			br::render::kStaticScenePageCount>> pages;
		std::shared_ptr<const br::render::PublishedStaticSceneState> scene;
	};

	using StaticTemplateFailureSet = std::unordered_set<DirectStaticTemplateKey, DirectStaticTemplateKeyHash>;

	using StaticTemplateFailureSnapshot = std::shared_ptr<const StaticTemplateFailureSet>;

	struct PendingStaticRetirement
	{
		std::uint64_t requiredMutationGeneration{ 0 };
		std::vector<std::shared_ptr<const StaticGraphGroupOwnership>> ownerships;
		std::vector<StaticImportGroupAck::UnpublishedGroup> unpublished;
		std::vector<StaticCellKey> invalidatedCells;
	};

	struct PendingStaticImportPublication
	{
		std::shared_ptr<StaticImportPublicationResult> result;
		std::vector<br::render::ArtifactAwaiter> awaiters;
	};

	struct StaticTemplateExternalCompletion
	{
		enum class Status : std::uint8_t { Pending, Ready, Failed };
		Status status{ Status::Pending };
		std::uint64_t suspensionIdentity{ 0 };
		std::uint64_t sourceFingerprint{ 0 };
		br::render::StaticMeshTemplateRef ref;
		std::string error;
		bool requestQueued{ false };
	};

	struct StaticImportCameraTarget
	{
		br::staticimport::StaticStreamTarget stream;
		StaticStreamKey key;
		std::uint64_t generation{ 0 };
		bool valid{ false };
	};

	struct StaticImportServiceStats
	{
		std::uint64_t generation{ 0 };
		std::size_t desiredCells{ 0 };
		std::size_t activeCells{ 0 };
		std::size_t pendingCellsQueued{ 0 };
		std::size_t pendingGroups{ 0 };
		std::size_t readyGroupsTracked{ 0 };
		std::size_t pendingPlacements{ 0 };
		std::size_t readyGroupsQueued{ 0 };
		std::size_t assetPublishRequestsQueued{ 0 };
		std::size_t templateRequestsQueued{ 0 };
		std::size_t materialVariantRequestsQueued{ 0 };
		std::size_t shaderVariantRequestsQueued{ 0 };
		std::size_t removalDiffsQueued{ 0 };
		std::size_t retirementRequestsQueued{ 0 };
		std::size_t retirementsAwaitingPublication{ 0 };
		std::size_t retirementCompletionsQueued{ 0 };
		std::size_t pendingHostCells{ 0 };
		std::uint64_t cellsEntered{ 0 };
		std::uint64_t cellsLeft{ 0 };
		std::uint64_t cellsPrepared{ 0 };
		std::uint64_t groupsPrepared{ 0 };
		std::uint64_t groupsWaitingAssets{ 0 };
		std::uint64_t groupsWaitingVariants{ 0 };
		std::uint64_t groupsWaitingTemplates{ 0 };
		std::uint64_t groupsWaitingShaderVariants{ 0 };
		std::uint64_t staleGroupsDropped{ 0 };
		std::uint64_t unsupportedGroupsDropped{ 0 };
		std::uint64_t groupsRequeued{ 0 };
		std::uint64_t groupsAckPublished{ 0 };
		std::uint64_t groupsAckRequeued{ 0 };
		std::uint64_t groupsAckDropped{ 0 };
		std::vector<std::string> pendingGroupDiagnostics;
		std::uint64_t cellDiffUs{ 0 };
		std::uint64_t cellPrepareUs{ 0 };
		std::uint64_t groupBuildUs{ 0 };
		std::uint64_t objectPlanPrepareUs{ 0 };
	};

	struct StaticSceneRecipeCompileKey
	{
		br::staticimport::recipe::CellKey cell;
		std::uint64_t sourceFingerprint{ 0 };
		friend bool operator==(const StaticSceneRecipeCompileKey&, const StaticSceneRecipeCompileKey&) = default;
	};

	struct StaticSceneRecipeCompileKeyHash
	{
		std::size_t operator()(const StaticSceneRecipeCompileKey& key) const noexcept
		{
			std::uint64_t value = key.sourceFingerprint ^ key.cell.worldFormID;
			const auto mix = [&value](std::uint64_t item) {
				value ^= item + 0x9e3779b97f4a7c15ull + (value << 6u) + (value >> 2u);
			};
			mix(key.cell.interiorCellFormID);
			mix(static_cast<std::uint32_t>(key.cell.cellX));
			mix(static_cast<std::uint32_t>(key.cell.cellY));
			return static_cast<std::size_t>(value);
		}
	};

	struct StaticGroupPreparationCheckpoint
	{
		enum class Stage : std::uint8_t { DiscoverDependencies, BuildFinal };
		Stage stage{ Stage::DiscoverDependencies };
	};

	struct DirectStaticPrepareInput
	{
		std::uint64_t groupID{ 0 };
		std::shared_ptr<StaticGroupLifecycleRecord> record;
		std::shared_ptr<const StaticGroupPreparationCheckpoint> checkpoint;

		[[nodiscard]] const PendingStaticGroup& Group() const
		{
			return record->descriptor;
		}
	};

	struct StaticTemplateArtifactBuildInput
	{
		DirectStaticTemplateKey key;
		std::uint64_t sourceFingerprint{ 0 };
	};

	struct PublishedStaticTemplateArtifact
	{
		DirectStaticTemplateKey key;
		br::render::StaticMeshTemplateRef ref;
		std::uint64_t sourceFingerprint{ 0 };
	};

	// A static template build's checkpoint: the lease on its material's usage admission.
	struct StaticTemplateMaterialAdmission
	{
		br::render::ArtifactVersionHandle batch;
	};

	struct StaticImportBlockedGroup
	{
		std::uint64_t groupID{ 0 };
		std::shared_ptr<StaticGroupIntentToken> intentToken;
		std::vector<StaticImportDependencyRequest> dependencies;
		std::shared_ptr<const StaticGroupPreparationCheckpoint> checkpoint;
	};

	struct DelayedStaticImportDependencyRequest
	{
		StaticImportDependencyRequest request;
		std::chrono::steady_clock::time_point dueAt{};
	};

	struct StaticImportGroupPrepareJob
	{
		std::uint64_t generation{ 0 };
		StaticImportCameraTarget target;
		std::vector<DirectStaticPrepareInput> inputs;
		StaticTemplateRefSnapshot templateRefs;
	};

	struct StaticImportGroupPrepareResult
	{
		struct UnsupportedGroup
		{
			std::uint64_t groupID{ 0 };
			std::size_t placementCount{ 0 };
			std::string modelPath;
			std::string reason;
		};
		std::uint64_t generation{ 0 };
		std::vector<std::uint64_t> inputGroupIDs;
		std::vector<PreparedDirectStaticImport> ready;
		br::render::StaticImportBuildBatch transactionBuild;
		std::vector<StaticImportBlockedGroup> blockedGroups;
		std::vector<UnsupportedGroup> unsupported;
		std::uint64_t buildUs{ 0 };
		std::uint64_t prepareUs{ 0 };
	};

	struct StaticVariantArtifactInput
	{
		std::uint64_t gateID{ 0 };
		std::uint64_t fingerprint{ 0 };
		StaticImportCameraTarget target;
		std::shared_ptr<StaticGroupLifecycleRecord> representative;
	};

	struct StaticGroupSuspensionState
	{
		std::uint64_t revision{ 0 };
		std::uint64_t identity{ 0 };
		std::unordered_set<StaticImportDependencyKey, StaticImportDependencyKeyHash> remaining;
	};

	struct StaticGroupArtifactBuildInput
	{
		StaticImportGroupPrepareJob job;
		std::uint64_t packetID{ 0 };
		std::vector<std::shared_ptr<StaticGroupIntentToken>> intentTokens;
	};

	struct PreparedStaticGroupArtifact
	{
		std::uint64_t packetID{ 0 };
		StaticCellKey cellKey;
		std::uint64_t packetRevision{ 0 };
		std::uint64_t placementCount{ 0 };
		std::vector<std::shared_ptr<StaticGroupIntentToken>> intentTokens;
		StaticImportGroupPrepareResult result;
	};

	struct StaticStreamBudget
	{
		std::uint64_t totalUs{ 0 };
		std::uint64_t removeUs{ 0 };
		mutable std::uint64_t usedUs{ 0 };

		[[nodiscard]] bool Unlimited() const
		{
			return totalUs == 0;
		}

		[[nodiscard]] std::uint64_t ElapsedUs() const
		{
			if (Unlimited()) {
				return 0;
			}
			return usedUs;
		}

		void ChargeElapsed(std::chrono::steady_clock::time_point begin) const
		{
			if (Unlimited()) {
				return;
			}
			usedUs += static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now() - begin)
					.count());
		}

		[[nodiscard]] bool TotalExhausted() const
		{
			return !Unlimited() && ElapsedUs() >= totalUs;
		}

		[[nodiscard]] std::uint64_t RemainingUs() const
		{
			return Unlimited() || ElapsedUs() >= totalUs ? 0 : totalUs - ElapsedUs();
		}

		[[nodiscard]] bool RemoveExhausted() const
		{
			return !Unlimited() && removeUs > 0 && ElapsedUs() >= removeUs;
		}
	};
}
