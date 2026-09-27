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

struct MaterialTextureStreamingStats {
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

