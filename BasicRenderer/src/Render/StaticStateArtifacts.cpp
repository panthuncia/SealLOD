#include "Render/StaticStateArtifacts.h"

#include <algorithm>
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

    std::unordered_set<ArtifactKey, ArtifactKey::Hasher> expected;
    expected.reserve(input->transactionKeys.size());
    for (const auto key : input->transactionKeys) {
        if (key.kind != ArtifactKind::StaticTransaction || !expected.insert(key).second) {
            return ArtifactBuildResult::Failure("static scene transaction set is invalid");
        }
    }
    if (context.dependencies.size() != expected.size()) {
        return ArtifactBuildResult::Failure("static scene dependency closure is incomplete");
    }

    auto scene = std::make_shared<PublishedStaticSceneState>();
    scene->sourceFingerprint = input->sourceFingerprint;
    scene->sceneGeneration = context.generation;
    scene->desiredPlacementCount = input->desiredPlacementCount;
    scene->materializedPlacementCount = input->materializedPlacementCount;
    scene->retiredPlacementCount = input->retiredPlacementCount;
    scene->activeGroupIDs = input->activeGroupIDs;
    std::ranges::sort(scene->activeGroupIDs);
    if (std::ranges::adjacent_find(scene->activeGroupIDs) != scene->activeGroupIDs.end()) {
        return ArtifactBuildResult::Failure("static scene contains duplicate active groups");
    }
    std::unordered_set<std::uint64_t> activeGroups(
        scene->activeGroupIDs.begin(), scene->activeGroupIDs.end());
    const auto desiredActiveGroups = activeGroups;
    std::unordered_set<std::uint64_t> materializedActiveGroups;
    materializedActiveGroups.reserve(activeGroups.size());
    scene->transactions.reserve(context.dependencies.size());
    for (const auto& dependency : context.dependencies) {
        if (!expected.contains(dependency.key)) {
            return ArtifactBuildResult::Failure("static scene contains an unexpected transaction");
        }
        const auto transaction = dependency.payload.Get<PublishedStaticTransaction>();
        if (!transaction || transaction->transactionID != dependency.key.primaryID) {
            return ArtifactBuildResult::Failure("static scene transaction payload mismatch");
        }
        for (const auto& group : transaction->groups) {
            if (!desiredActiveGroups.contains(group.groupID)) continue;
            if (!materializedActiveGroups.insert(group.groupID).second) {
                return ArtifactBuildResult::Failure(
                    "static scene active group is owned by multiple transactions");
            }
            activeGroups.erase(group.groupID);
            ++scene->groupCount;
            scene->drawRecordCount += group.drawRecordCount;
            scene->activeEntryCount += group.activeEntryCount;
            scene->publishedPlacementCount += group.placementCount;
        }
        scene->transactions.push_back(*transaction);
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
    root->fragment.dependencyClosure = context.dependencies;
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
