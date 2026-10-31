#pragma once

// The static-import work bus.
//
// Every BoundedMpscQueue that connects the static streaming stages lives here, in one
// object the runtime owns. Producers and consumers cover the coordinator, the worker
// pool, the streaming pump and telemetry, so all of them hold a reference to this
// single bus.

#include <cstdint>
#include <BasicRenderer/Streaming/ObjectRequests.h>
#include <BasicRenderer/StaticImport/StaticImportSettings.h>
#include <memory>

#include <BasicRenderer/StaticImport/Advanced/StreamingQueues.h>
#include <BasicRenderer/StaticImport/Advanced/StaticImportMessages.h>

namespace br::staticimport
{

	struct StaticImportWorkBus
	{
		explicit StaticImportWorkBus(const br::staticimport::StaticImportSettings& settings)
			: staticImportCompletedOutput{ settings.completedOutputQueueCapacity }
		{
		}

		BoundedMpscQueue<std::shared_ptr<const br::staticimport::recipe::BatchRecipe>>
			staticSceneRecipePersistenceQueue{ 0 };
		BoundedMpscQueue<StaticSceneRecipeCompileJob> staticSceneRecipeCompileJobs{ 0 };
		BoundedMpscQueue<StaticGroupChanges> staticImportGroupChanges{ 0 };

		BoundedMpscQueue<StaticGroupSuspensionRequest> staticGroupSuspensionRequests{ 0 };
		BoundedMpscQueue<br::render::ArtifactKey> staticImportGroupArtifactReleaseJobs{ 0 };
		BoundedMpscQueue<std::shared_ptr<StaticGroupLifecycleRecord>>
			staticImportLifecycleReleaseJobs{ 0 };
		BoundedMpscQueue<StaticGroupAssemblyJob> staticImportGroupAssemblyJobs{ 0 };
		BoundedMpscQueue<StaticAssetPublishRequest> staticImportAssetPublishJobs{ 4096 };
		BoundedMpscQueue<StaticImportAssetPublishResult> staticImportAssetPublishResults{ 0 };
		BoundedMpscQueue<StaticMaterialVariantRequest> staticImportMaterialVariantJobs{ 4096 };
		BoundedMpscQueue<StaticImportMaterialVariantResult> staticImportMaterialVariantResults{ 0 };
		BoundedMpscQueue<StaticShaderVariantRequest> staticImportShaderVariantJobs{ 4096 };
		BoundedMpscQueue<StaticImportShaderVariantResult> staticImportShaderVariantResults{ 0 };

		BoundedMpscQueue<StaticTemplateRequest> staticImportTemplateRequestOutput{ 0 };
		BoundedMpscQueue<StaticTemplateRefsPublished> staticImportTemplateRefsPublished{ 0 };
		BoundedMpscQueue<StaticGroupRemoval> staticImportRemovalOutput{ 0 };
		BoundedMpscQueue<StaticRetirementOwnershipRequest>
			staticRetirementOwnershipRequests{ 0 };
		BoundedMpscQueue<br::render::StaticObjectRemovalPayload>
			staticRetirementPayloadRequests{ 0 };
		BoundedMpscQueue<StaticRetirementCompletion> staticRetirementCompletions{ 0 };
		BoundedMpscQueue<StaticImportGroupAck> staticImportGroupAcks{ 0 };
		BoundedMpscQueue<StaticTransactionReady> staticTransactionReadyEvents{ 0 };
		BoundedMpscQueue<StaticScenePageReady> staticScenePageReadyEvents{ 0 };
		BoundedMpscQueue<StaticSceneRootPublished> staticSceneRootPublishedEvents{ 0 };
		BoundedMpscQueue<StaticSceneOwnershipDelta> staticSceneOwnershipDeltas{ 0 };
		BoundedMpscQueue<PreparedDirectStaticImportBatch> staticImportCompletedOutput;
		BoundedMpscQueue<StaticImportTransactionPrepareJob> staticImportTransactionMaterializeJobs{ 1024 };
		BoundedMpscQueue<MaterializedDirectStaticImportBatch> staticImportMaterializedValidationRequests{ 0 };
		BoundedMpscQueue<StaticImportMaterializedPublishDecision> staticImportMaterializedPublishDecisions{ 0 };
		BoundedMpscQueue<StaticImportPublicationJob> staticImportPublicationJobs{ 256 };
		BoundedMpscQueue<StaticImportPublicationResult> staticImportPublicationResults{ 0 };
		BoundedMpscQueue<std::shared_ptr<StaticImportPublicationResult>>
			staticImportGpuReadyPublicationResults{ 0 };
	};
}
