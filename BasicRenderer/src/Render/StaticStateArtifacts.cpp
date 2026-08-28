#include "Render/StaticStateArtifacts.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "Render/PublishedRendererState.h"

namespace br::render {
namespace {

ArtifactBuildResult BuildStaticTransaction(const ArtifactBuildContext& context) {
    const auto input = context.input.Get<StaticTransactionBuildInput>();
    if (!input) return ArtifactBuildResult::Failure("static transaction immutable input missing");
    if (input->transactionID == 0 || input->transactionID != context.key.primaryID) {
        return ArtifactBuildResult::Failure("static transaction identity mismatch");
    }
    if (input->groupCount != input->groups.size()) {
        return ArtifactBuildResult::Failure("static transaction group closure is incomplete");
    }
    auto groups = input->groups;
    std::ranges::sort(groups, {}, &StaticTransactionGroup::groupID);
    if (std::ranges::adjacent_find(groups, {}, &StaticTransactionGroup::groupID) != groups.end() ||
        (!groups.empty() && groups.front().groupID == 0)) {
        return ArtifactBuildResult::Failure("static transaction contains duplicate groups");
    }
    std::uint64_t placementCount = 0;
    for (const auto& group : groups) placementCount += group.placementCount;
    if (placementCount != input->placementCount) {
        return ArtifactBuildResult::Failure("static transaction placement count is inconsistent");
    }

    auto transaction = std::make_shared<PublishedStaticTransaction>();
    transaction->transactionID = input->transactionID;
    transaction->streamGeneration = input->streamGeneration;
    transaction->sourceFingerprint = input->sourceFingerprint;
    transaction->transactionGeneration = context.generation;
    transaction->groups = std::move(groups);
    transaction->groupCount = input->groupCount;
    transaction->drawRecordCount = input->drawRecordCount;
    transaction->activeEntryCount = input->activeEntryCount;
    transaction->placementCount = input->placementCount;
    transaction->dependencyClosure = context.dependencies;
    return ArtifactBuildResult::Ready(
        ArtifactPayload::Make<PublishedStaticTransaction>(std::move(transaction)));
}

ArtifactBuildResult BuildStaticScene(const ArtifactBuildContext& context) {
    const auto input = context.input.Get<StaticSceneBuildInput>();
    if (!input) return ArtifactBuildResult::Failure("static scene immutable input missing");

    std::unordered_map<std::uint64_t, ArtifactVersionID> owners;
    std::unordered_map<ArtifactKey, ArtifactVersionID, ArtifactKey::Hasher> expected;
    owners.reserve(input->groupOwners.size());
    expected.reserve(input->groupOwners.size());
    for (const auto& owner : input->groupOwners) {
        if (owner.groupID == 0 || owner.transaction.address.kind != ArtifactKind::StaticTransaction ||
            !owner.transaction) {
            return ArtifactBuildResult::Failure("static scene group owner is invalid");
        }
        if (!owners.emplace(owner.groupID, owner.transaction).second) {
            return ArtifactBuildResult::Failure("static scene contains duplicate active groups");
        }
        const auto [versionIt, inserted] = expected.emplace(
            owner.transaction.address, owner.transaction);
        if (!inserted && versionIt->second != owner.transaction) {
            return ArtifactBuildResult::Failure(
                "static scene selects multiple versions of one transaction");
        }
    }
    const auto expectedResourceRoots = input->requireResourceClosure ? 3u : 0u;
    if (context.dependencies.size() != expected.size() + expectedResourceRoots) {
        return ArtifactBuildResult::Failure("static scene dependency closure is incomplete");
    }

    bool hasMaterialRoot = false;
    bool hasObjectBufferRoot = false;
    bool hasIndirectRoot = false;
    for (const auto& dependency : context.dependencies) {
        if (dependency.key.kind == ArtifactKind::StaticTransaction) continue;
        const auto root = dependency.payload.Get<RendererStateFragmentArtifact>();
        if (!root) {
            return ArtifactBuildResult::Failure(
                "static scene resource dependency is not a published fragment");
        }
        switch (dependency.key.kind) {
        case ArtifactKind::MaterialTable:
            hasMaterialRoot = !hasMaterialRoot &&
                root->kind == PublishedFragmentKind::Materials;
            break;
        case ArtifactKind::DrawRecordPage:
            hasObjectBufferRoot = !hasObjectBufferRoot &&
                root->kind == PublishedFragmentKind::DrawRecords;
            break;
        case ArtifactKind::IndirectWorkload:
            hasIndirectRoot = !hasIndirectRoot &&
                root->kind == PublishedFragmentKind::IndirectWorkloads;
            break;
        default:
            return ArtifactBuildResult::Failure(
                "static scene contains an unexpected resource dependency");
        }
    }
    if (input->requireResourceClosure &&
        (!hasMaterialRoot || !hasObjectBufferRoot || !hasIndirectRoot)) {
        return ArtifactBuildResult::Failure(
            "static scene renderer resource closure is incomplete");
    }

    auto scene = std::make_shared<PublishedStaticSceneState>();
    scene->sourceFingerprint = input->sourceFingerprint;
    scene->sceneGeneration = context.generation;
    scene->desiredPlacementCount = input->desiredPlacementCount;
    scene->materializedPlacementCount = input->materializedPlacementCount;
    scene->retiredPlacementCount = input->retiredPlacementCount;
    scene->activeGroupIDs.reserve(owners.size());
    for (const auto& [groupID, owner] : owners) {
        (void)owner;
        scene->activeGroupIDs.push_back(groupID);
    }
    std::ranges::sort(scene->activeGroupIDs);
    std::unordered_set<std::uint64_t> activeGroups(
        scene->activeGroupIDs.begin(), scene->activeGroupIDs.end());
    const auto desiredActiveGroups = activeGroups;
    std::unordered_set<std::uint64_t> materializedActiveGroups;
    materializedActiveGroups.reserve(activeGroups.size());
    scene->transactions.reserve(context.dependencies.size());
    for (const auto& dependency : context.dependencies) {
        if (dependency.key.kind != ArtifactKind::StaticTransaction) continue;
        const auto expectedVersion = expected.find(dependency.key);
        if (expectedVersion == expected.end() || expectedVersion->second != dependency.Version()) {
            return ArtifactBuildResult::Failure("static scene contains an unexpected transaction");
        }
        const auto transaction = dependency.payload.Get<PublishedStaticTransaction>();
        if (!transaction || transaction->transactionID != dependency.key.primaryID) {
            return ArtifactBuildResult::Failure("static scene transaction payload mismatch");
        }
        auto selectedTransaction = *transaction;
        selectedTransaction.groups.clear();
        selectedTransaction.groupCount = 0;
        selectedTransaction.drawRecordCount = 0;
        selectedTransaction.activeEntryCount = 0;
        selectedTransaction.placementCount = 0;
        for (const auto& group : transaction->groups) {
            const auto owner = owners.find(group.groupID);
            if (owner == owners.end() || owner->second != dependency.Version()) continue;
            if (!materializedActiveGroups.insert(group.groupID).second) {
                return ArtifactBuildResult::Failure(
                    "static scene active group is owned by multiple transactions");
            }
            activeGroups.erase(group.groupID);
            selectedTransaction.groups.push_back(group);
            ++selectedTransaction.groupCount;
            selectedTransaction.drawRecordCount += group.drawRecordCount;
            selectedTransaction.activeEntryCount += group.activeEntryCount;
            selectedTransaction.placementCount += group.placementCount;
            ++scene->groupCount;
            scene->drawRecordCount += group.drawRecordCount;
            scene->activeEntryCount += group.activeEntryCount;
            scene->publishedPlacementCount += group.placementCount;
        }
        scene->transactions.push_back(std::move(selectedTransaction));
    }
    if (!activeGroups.empty()) {
        return ArtifactBuildResult::Failure("static scene active group closure is incomplete");
    }
    if (scene->materializedPlacementCount != scene->publishedPlacementCount ||
        scene->desiredPlacementCount != scene->publishedPlacementCount) {
        return ArtifactBuildResult::Failure("static scene placement closure is incomplete");
    }
    std::ranges::sort(scene->transactions, {}, &PublishedStaticTransaction::transactionID);
    std::uint64_t digest = 1469598103934665603ull;
    for (const auto groupID : scene->activeGroupIDs) {
        digest ^= groupID + 0x9e3779b97f4a7c15ull + (digest << 6u) + (digest >> 2u);
    }
    digest ^= scene->publishedPlacementCount + 0x9e3779b97f4a7c15ull +
        (digest << 6u) + (digest >> 2u);
    scene->placementSetDigest = digest;

    auto root = std::make_shared<RendererStateFragmentArtifact>();
    root->kind = PublishedFragmentKind::Geometry;
    root->publishRoot = input->publishRoot;
    root->fragment.revision = context.revision;
    // Resource roots are one-time ReadyGate admission dependencies. They prove
    // that this membership cut has a schedulable resource closure, but they do
    // not become exact manifest coupling: materials, draw records, and indirect
    // workloads advance independently and recombine with geometry at manifest
    // selection. Transaction versions remain the authoritative exact closure.
    std::ranges::copy_if(context.dependencies,
        std::back_inserter(root->fragment.dependencyClosure),
        [](const ArtifactSnapshot& dependency) {
            return dependency.key.kind == ArtifactKind::StaticTransaction;
        });
    root->fragment.payload = ArtifactPayload::Make<PublishedStaticSceneState>(std::move(scene));
    return ArtifactBuildResult::Ready(
        ArtifactPayload::Make<RendererStateFragmentArtifact>(std::move(root)));
}

} // namespace

void RegisterStaticStateProducers(AsyncStateGraph& graph) {
    graph.RegisterProducer(ArtifactKind::StaticTransaction, {
        TaskLane::Streaming, TaskDomain::General,
        "StaticStateArtifact::BuildTransaction", BuildStaticTransaction });
    graph.RegisterProducer(ArtifactKind::StaticScene, {
        TaskLane::Streaming, TaskDomain::General,
        "StaticStateArtifact::BuildScene", BuildStaticScene });
}

} // namespace br::render
