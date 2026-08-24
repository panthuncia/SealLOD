#pragma once

#include <cstdint>
#include <vector>

#include "Render/AsyncStateGraph.h"

namespace br::render {

struct StaticTransactionBuildInput {
    std::uint64_t transactionID = 0;
    std::uint64_t streamGeneration = 0;
    std::uint64_t sourceFingerprint = 0;
    std::uint64_t groupCount = 0;
    std::uint64_t drawRecordCount = 0;
    std::uint64_t activeEntryCount = 0;
};

struct PublishedStaticTransaction {
    std::uint64_t transactionID = 0;
    std::uint64_t streamGeneration = 0;
    std::uint64_t sourceFingerprint = 0;
    std::uint64_t groupCount = 0;
    std::uint64_t drawRecordCount = 0;
    std::uint64_t activeEntryCount = 0;
    std::vector<ArtifactSnapshot> dependencyClosure;
};

struct StaticSceneBuildInput {
    std::uint64_t sourceFingerprint = 0;
    std::vector<ArtifactKey> transactionKeys;
};

struct PublishedStaticSceneState {
    std::uint64_t sourceFingerprint = 0;
    std::uint64_t groupCount = 0;
    std::uint64_t drawRecordCount = 0;
    std::uint64_t activeEntryCount = 0;
    std::vector<PublishedStaticTransaction> transactions;
};

void RegisterStaticStateProducers(AsyncStateGraph& graph);

} // namespace br::render
