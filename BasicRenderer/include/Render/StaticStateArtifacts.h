#pragma once

#include <cstdint>
#include <vector>

#include "Render/AsyncStateGraph.h"

namespace br::render {

struct StaticTransactionGroup {
    std::uint64_t groupID = 0;
    std::uint64_t drawRecordCount = 0;
    std::uint64_t activeEntryCount = 0;
    std::uint64_t placementCount = 0;
};

struct StaticTransactionBuildInput {
    std::uint64_t transactionID = 0;
    std::uint64_t streamGeneration = 0;
    std::uint64_t sourceFingerprint = 0;
    std::vector<StaticTransactionGroup> groups;
    std::uint64_t groupCount = 0;
    std::uint64_t drawRecordCount = 0;
    std::uint64_t activeEntryCount = 0;
    std::uint64_t placementCount = 0;
};

struct PublishedStaticTransaction {
    std::uint64_t transactionID = 0;
    std::uint64_t streamGeneration = 0;
    std::uint64_t sourceFingerprint = 0;
    std::uint64_t transactionGeneration = 0;
    std::vector<StaticTransactionGroup> groups;
    std::uint64_t groupCount = 0;
    std::uint64_t drawRecordCount = 0;
    std::uint64_t activeEntryCount = 0;
    std::uint64_t placementCount = 0;
    std::vector<ArtifactSnapshot> dependencyClosure;
};

struct StaticSceneGroupOwner {
    std::uint64_t groupID = 0;
    ArtifactVersionID transaction;
};

struct StaticSceneBuildInput {
    std::uint64_t sourceFingerprint = 0;
    bool publishRoot = false;
    // Active renderer paths require one coherent material, object-buffer, and
    // indirect/active-list root in addition to the immutable transactions.
    bool requireResourceClosure = false;
    std::uint64_t desiredPlacementCount = 0;
    std::uint64_t materializedPlacementCount = 0;
    std::uint64_t retiredPlacementCount = 0;
    // This is the authoritative membership edge. A transaction may remain in
    // the closure for its other groups after one group is superseded by a
    // newer transaction, so a bare set of transaction addresses is ambiguous.
    std::vector<StaticSceneGroupOwner> groupOwners;
};

struct PublishedStaticSceneState {
    std::uint64_t sourceFingerprint = 0;
    std::uint64_t sceneGeneration = 0;
    std::uint64_t placementSetDigest = 0;
    std::uint64_t desiredPlacementCount = 0;
    std::uint64_t materializedPlacementCount = 0;
    std::uint64_t publishedPlacementCount = 0;
    std::uint64_t retiredPlacementCount = 0;
    std::uint64_t groupCount = 0;
    std::uint64_t drawRecordCount = 0;
    std::uint64_t activeEntryCount = 0;
    std::vector<std::uint64_t> activeGroupIDs;
    std::vector<PublishedStaticTransaction> transactions;
};

void RegisterStaticStateProducers(AsyncStateGraph& graph);

} // namespace br::render
