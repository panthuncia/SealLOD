#pragma once

// Static-import budgets and caps.
//
// The static import pipeline reads these once, when its runtime is constructed, and
// keeps them for its lifetime. A host fills the struct from its own configuration
// (SARP maps its SARP_STATIC_* environment variables onto it); the defaults below are
// the values the pipeline uses when the host leaves a field untouched.
//
// Zero means "unbounded" for the per-frame/per-pass caps and the transaction limits
// that document it, matching how the pipeline has always interpreted them.

#include <cstdint>

namespace br::staticimport
{
	// How the pipeline uses the static scene recipe cache (StaticSceneRecipe.h).
	enum class StaticRecipeCacheMode : std::uint8_t
	{
		Off,       // No cache: lookups miss and the host groups every batch itself.
		Read,      // Cached recipes are used; cache misses are not written back.
		ReadWrite  // Cache misses are compiled once their assets are ready and persisted.
	};

	struct StaticImportSettings
	{
		// Whether the runtime imports at all; Tick does nothing while it is false.
		bool streamingEnabled = true;
		std::uint32_t logFrameSpikeMs = 8;

		// Recipe cache.
		StaticRecipeCacheMode recipeCacheMode = StaticRecipeCacheMode::ReadWrite;

		// Per-frame streaming budget. A backlog above highWaterDepth raises the
		// budget to burstFrameBudgetUs while the previous tick stayed under
		// burstHeadroomUs; removeBudgetFraction of the budget goes to removals.
		std::uint32_t frameBudgetUs = 2000;
		std::uint32_t burstFrameBudgetUs = 4000;
		std::uint32_t highWaterDepth = 4096;
		std::uint32_t burstHeadroomUs = 12000;
		float removeBudgetFraction = 0.5F;

		// Group intake. maxGroupChangesDrainPerPass counts the queued group changes
		// (one AddOrUpdateGroups / RemoveGroups / AddPreparedGroups call each) the
		// coordinator takes per pass; maxRemoveGroupsPerFrame the removed groups one
		// retirement pass applies.
		std::uint32_t maxGroupChangesDrainPerPass = 64;
		std::uint32_t maxTemplateRequestsPerFrame = 256;
		std::uint32_t maxRemoveGroupsPerFrame = 0;

		// Group preparation packets and credits.
		std::uint32_t preparationPacketGroups = 256;
		std::uint32_t preparationPacketTargetUs = 1000;
		std::uint32_t preparationPacketPlacements = 16384;
		std::uint32_t preparationPacketBytes = 16u * 1024u * 1024u;
		// A non-zero explicit ceiling replaces the worker-count-derived credit window.
		std::uint32_t preparationCreditLimit = 0;
		std::uint32_t preparedUnpublishedByteCeiling = 64u * 1024u * 1024u;
		std::uint32_t preparedReservationByteCeiling = 256u * 1024u * 1024u;

		// Per-frame import reservation.
		std::uint32_t maxImportGroupsPerFrame = 0;
		std::uint32_t maxImportPublishBytesPerFrame = 8u * 1024u * 1024u;
		std::uint32_t maxImportDrawRecordsPerFrame = 128u * 1024u;
		std::uint32_t maxActiveInsertIndicesPerFrame = 256u * 1024u;
		std::uint32_t maxReadyReservationBatchesPerFrame = 10000;
		std::uint32_t completedOutputQueueCapacity = 0;
		std::uint32_t maxCompletedBatchDrainPerFrame = 512;

		// Transaction materialization chunks.
		std::uint32_t maxTransactionGroups = 2048;
		std::uint32_t maxTransactionPublishBytes = 16u * 1024u * 1024u;
		std::uint32_t maxTransactionDrawRecords = 128u * 1024u;
		std::uint32_t maxTransactionActiveInsertIndices = 256u * 1024u;
		std::uint32_t maxMaterializedValidationDrainPerPass = 8;
		std::uint32_t maxMaterializedDecisionDrainPerFrame = 1024;

		// Publication cuts.
		std::uint32_t maxPublicationCutGroups = 8192;
		std::uint32_t maxPublicationCutBytes = 64u * 1024u * 1024u;
	};
}
