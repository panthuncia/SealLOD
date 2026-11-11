#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct MaterialTextureStreamingRecord {
	std::string identifier;
	uint32_t streamingTextureID = 0;
	uint32_t imageDescriptorIndex = UINT32_MAX;
	uint64_t imageResourceID = 0;
	uint64_t residentBytes = 0;
	uint32_t residentWidth = 0;
	uint32_t residentHeight = 0;
	uint32_t expectedResidentWidth = 0;
	uint32_t expectedResidentHeight = 0;
	uint32_t totalMipCount = 0;
	uint32_t residentTopMip = 0;
	uint32_t residentMipCount = 0;
	uint32_t requestedTopMip = 0;
	uint32_t feedbackTopMip = UINT32_MAX;
	bool eligible = false;
	bool enabled = false;
	bool alphaTested = false;
};

// The residency policy's budget controller, from its last tick (TextureStreamingManager::RunResidencyPolicyTick).
struct TextureStreamingBudgetStats {
	bool measured = false;
	uint64_t driverBudgetBytes = 0;    // what the OS offers this process of local VRAM
	uint64_t budgetBytes = 0;          // what applies: the setting, or the driver's
	uint64_t usageBytes = 0;           // the process's local VRAM use
	uint64_t nonWindowBytes = 0;       // of it, what is not a streamed window, smoothed
	uint64_t allowanceBytes = 0;       // what the windows are fitted to
	uint64_t residentBytes = 0;        // the windows' bytes
	uint64_t committedBytes = 0;       // with the windows on their way
	uint64_t wantedBytes = 0;          // what the policy wants at the current pull-back
	uint64_t visibleWantedBytes = 0;   // of it, what is on screen asked for
	uint64_t unconstrainedBytes = 0;   // what demand would take with no pull-back
	float heldPullback = 0.0f;         // in mips, of demand not reported in the last second
	float visiblePullback = 0.0f;      // in mips, of what is on screen
	uint32_t textures = 0;
	uint32_t visibleTextures = 0;
	uint32_t heldTextures = 0;
	uint64_t upgrades = 0, downgrades = 0, pressureDowngrades = 0;  // since the start
	uint64_t materialReports = 0, textureReports = 0;                // since the start
};

struct MaterialTextureStreamingStats {
	TextureStreamingBudgetStats budget = {};
	uint32_t uniqueMaterialTextureCount = 0;
	uint32_t uniqueStreamableTextureCount = 0;
	uint32_t uniqueStreamingEnabledTextureCount = 0;
	uint32_t fullResolutionResidentTextureCount = 0;
	uint32_t streamableFullResolutionResidentTextureCount = 0;
	uint32_t pendingReloadTextureCount = 0;
	uint64_t totalResidentBytes = 0;
	uint64_t streamableResidentBytes = 0;
	std::vector<uint32_t> residentTopMipHistogram = {};
	std::vector<uint32_t> requestedTopMipHistogram = {};
	std::vector<uint32_t> feedbackTopMipHistogram = {};
	uint32_t texturesWithoutFeedback = 0;
	std::vector<uint64_t> residentBytesByTopMip = {};
	uint32_t residentShapeMismatchTextureCount = 0;
	uint64_t residentShapeMismatchBytes = 0;
	uint32_t distinctPreparedTextureCount = 0;
	uint64_t distinctPreparedTextureBytes = 0;
	uint32_t activeMaterialResourceCount = 0;
	uint64_t activeMaterialResourceBytes = 0;
	uint32_t externallyManagedActiveResourceCount = 0;
	uint64_t externallyManagedActiveResourceBytes = 0;
	uint32_t graphManagedParticipatingActiveResourceCount = 0;
	uint64_t graphManagedParticipatingActiveResourceBytes = 0;
	uint32_t alphaTestedTextureCount = 0;
	uint32_t alphaTestedMipCapViolationCount = 0;
	uint32_t idleCoarseningDisabledTextureCount = 0;
	uint32_t residencyConstrainedTextureCount = 0;
	uint32_t residencyConstraintViolationCount = 0;
	std::vector<uint64_t> publishedResourceIDs = {};
	std::vector<uint64_t> participatingPublishedResourceIDs = {};
	std::vector<MaterialTextureStreamingRecord> largestResidentTextures = {};
};

struct MaterialTextureStreamingReadinessStats {
	uint32_t fullResolutionResidentTextureCount = 0;
	uint32_t pendingReloadTextureCount = 0;
};

