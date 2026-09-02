#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "Render/AsyncStateGraph.h"

namespace br::render {

struct StaticTransactionGroup {
    std::uint64_t groupID = 0;
    // Host admission identity for this group. A transaction may gather groups
    // admitted under distinct latest-wins tickets.
    std::uint64_t admissionTicketID = 0;
    std::uint64_t drawRecordCount = 0;
    std::uint64_t activeEntryCount = 0;
    std::uint64_t placementCount = 0;
    // Host-defined immutable ownership data. The graph never interprets it;
    // transaction/page/scene snapshots retain it transitively so lifecycle
    // consumers can use the selected scene cut without replaying acknowledgements.
    std::shared_ptr<const void> ownership;
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

// Static pages are publication/coalescing units, not allocation pages.  A 1024-way
// directory made a 100k-group scene submit hundreds of tiny graph mutations at
// every progressive checkpoint.  Sixty-four pages retain independent progress
// and bounded root fan-out while amortizing graph admission over useful batches.
inline constexpr std::size_t kStaticScenePageCount = 64;
inline constexpr std::uint32_t kStaticScenePageMaxSuccessorDepth = 32;

[[nodiscard]] std::size_t StaticScenePageIndex(std::uint64_t groupID) noexcept;
[[nodiscard]] std::uint64_t StaticSceneGroupDigest(std::uint64_t groupID) noexcept;
[[nodiscard]] std::uint64_t StaticScenePlacementDigest(
    std::uint64_t groupDigest, std::uint64_t placementCount, std::uint64_t groupCount) noexcept;

struct PublishedStaticScenePage;

struct StaticScenePageBuildInput {
    std::uint32_t pageIndex = 0;
    std::uint64_t sourceFingerprint = 0;
    // Successor pages merge these mutations into the exact immutable base page.
    // An empty base version denotes the initial page build.
    ArtifactVersionID basePage;
    std::shared_ptr<const PublishedStaticScenePage> basePagePayload;
    std::vector<StaticSceneGroupOwner> groupOwners;
    std::vector<std::uint64_t> removedGroupIDs;
};

struct PublishedStaticScenePage {
    std::uint32_t pageIndex = 0;
    std::uint64_t sourceFingerprint = 0;
    std::uint64_t pageGeneration = 0;
    std::uint32_t successorDepth = 0;
    std::uint64_t groupDigest = 0;
    std::uint64_t groupCount = 0;
    std::uint64_t drawRecordCount = 0;
    std::uint64_t activeEntryCount = 0;
    std::uint64_t placementCount = 0;
    // Successors retain their immutable base and store only their own sorted
    // mutation layer. This bounds page-build work by delta size rather than by
    // total page membership.
    std::shared_ptr<const PublishedStaticScenePage> basePage;
    std::vector<StaticSceneGroupOwner> groupOwners;
    std::vector<StaticTransactionGroup> groups;
    std::vector<std::uint64_t> removedGroupIDs;

    [[nodiscard]] bool ContainsGroup(std::uint64_t groupID) const noexcept;
    [[nodiscard]] const StaticSceneGroupOwner* FindOwner(std::uint64_t groupID) const noexcept;
    [[nodiscard]] const StaticTransactionGroup* FindGroup(std::uint64_t groupID) const noexcept;
    void MaterializeOwners(std::vector<StaticSceneGroupOwner>& out) const;
    void MaterializeGroups(std::vector<StaticTransactionGroup>& out) const;
};

struct StaticScenePageRef {
    std::uint32_t pageIndex = 0;
    ArtifactVersionID page;
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
    // Every page version supplied here must already have reached the requested
    // graph milestone. Pages own the exact transaction membership closure; the
    // scene root only selects a bounded directory of immutable pages.
    std::vector<StaticScenePageRef> pages;
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
    std::array<std::shared_ptr<const PublishedStaticScenePage>, kStaticScenePageCount> pages{};

    [[nodiscard]] bool ContainsGroup(std::uint64_t groupID) const noexcept;
    [[nodiscard]] const StaticTransactionGroup* FindGroup(std::uint64_t groupID) const noexcept;
};

void RegisterStaticStateProducers(AsyncStateGraph& graph);

} // namespace br::render
