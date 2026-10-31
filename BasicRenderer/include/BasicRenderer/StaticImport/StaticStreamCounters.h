#pragma once

// Static-streaming telemetry counters.

#include <cstddef>
#include <cstdint>

namespace br::staticimport
{
	// Object-creation counters of the host's own scene objects, which the pipeline
	// reports alongside its own.
	struct StaticHostObjectCounters
	{
		std::uint64_t entityCreateUs{ 0 };
		std::uint64_t assetLookupUs{ 0 };
		std::uint64_t objectCreateCalls{ 0 };
		std::uint64_t entityCreateCalls{ 0 };
	};

	struct StaticStreamCounters
	{
		std::size_t lastStaticVisitedPlacements{ 0 };
		std::size_t lastStaticAppliedPlacements{ 0 };
		std::size_t lastStaticPendingOrSkippedPlacements{ 0 };
		std::size_t lastStaticUpsertsThisFrame{ 0 };
		std::uint64_t lastStaticGroupsImportedThisFrame{ 0 };
		std::uint64_t lastStaticGroupsRemovedThisFrame{ 0 };
		std::uint64_t lastStaticStreamBudgetUsedUs{ 0 };
		std::uint64_t lastStaticStreamBudgetOverrunUs{ 0 };
		std::uint64_t staticStreamUpdateUs{ 0 };
		std::uint64_t staticStreamVisitUs{ 0 };
		std::uint64_t staticStreamRemoveUs{ 0 };
		std::uint64_t staticStreamPruneUs{ 0 };
		std::uint64_t staticStreamPumpUs{ 0 };
		std::uint64_t staticPendingAssetRetryUs{ 0 };
		std::uint64_t staticStreamBudgetExhaustedFrames{ 0 };
		std::uint64_t staticStreamMaxImportQueueDepth{ 0 };
		std::uint64_t staticStreamMaxRemovalQueueDepth{ 0 };
		std::uint64_t staticStreamMaxBudgetOverrunUs{ 0 };
		std::uint64_t staticPlacementResolveUs{ 0 };
		std::uint64_t staticPlacementMatrixUs{ 0 };
		std::uint64_t staticObjectCreateUs{ 0 };
		std::uint64_t staticDirectBuildUs{ 0 };
		std::uint64_t staticDirectImportUs{ 0 };
		std::uint64_t staticDirectTemplateUs{ 0 };
		std::uint64_t staticDirectGroupsSubmitted{ 0 };
		std::uint64_t staticDirectGroupsImported{ 0 };
		std::uint64_t staticDirectGroupsRemoved{ 0 };
		std::uint64_t staticDirectTemplatesCreated{ 0 };
		std::uint64_t staticDirectTemplatesReused{ 0 };
		std::uint64_t staticDirectTransformRows{ 0 };
		std::uint64_t staticDirectDrawRecords{ 0 };
		std::uint64_t staticDirectImportBatchesCompleted{ 0 };
		std::uint64_t staticDirectImportBatchesCommitted{ 0 };
		std::uint64_t staticDirectImportBatchesStale{ 0 };
		std::uint64_t staticDirectImportBatchesDropped{ 0 };
		std::uint64_t staticDirectPreparedImportUs{ 0 };
		std::uint64_t staticDirectPreparedBytes{ 0 };
		double staticDirectPublishUsPerGroupEstimate{ 50.0 };
		std::uint64_t staticTransformUs{ 0 };
		// The host's previous frame tick time, recorded by the host; the streaming budget
		// only bursts while it stays within StaticImportSettings::burstHeadroomUs.
		std::uint64_t previousHostFrameTickUs{ 0 };
	};
}
