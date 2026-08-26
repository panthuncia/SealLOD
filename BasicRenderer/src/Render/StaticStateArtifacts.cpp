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
    if (input->groups.empty() || input->groupCount != input->groups.size()) {
        return ArtifactBuildResult::Failure("static transaction group closure is incomplete");
    }
    auto groups = input->groups;
    std::ranges::sort(groups, {}, &StaticTransactionGroup::groupID);
    if (std::ranges::adjacent_find(groups, {}, &StaticTransactionGroup::groupID) != groups.end() ||
        groups.front().groupID == 0) {
        return ArtifactBuildResult::Failure("static transaction contains duplicate groups");
    }

    auto transaction = std::make_shared<PublishedStaticTransaction>();
    transaction->transactionID = input->transactionID;
    transaction->streamGeneration = input->streamGeneration;
    transaction->sourceFingerprint = input->sourceFingerprint;
    transaction->groups = std::move(groups);
    transaction->groupCount = input->groupCount;
    transaction->drawRecordCount = input->drawRecordCount;
    transaction->activeEntryCount = input->activeEntryCount;
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
    scene->activeGroupIDs = input->activeGroupIDs;
    std::ranges::sort(scene->activeGroupIDs);
    if (std::ranges::adjacent_find(scene->activeGroupIDs) != scene->activeGroupIDs.end()) {
        return ArtifactBuildResult::Failure("static scene contains duplicate active groups");
    }
    std::unordered_set<std::uint64_t> activeGroups(
        scene->activeGroupIDs.begin(), scene->activeGroupIDs.end());
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
            if (!activeGroups.erase(group.groupID)) continue;
            ++scene->groupCount;
            scene->drawRecordCount += group.drawRecordCount;
            scene->activeEntryCount += group.activeEntryCount;
        }
        scene->transactions.push_back(*transaction);
    }
    if (!activeGroups.empty()) {
        return ArtifactBuildResult::Failure("static scene active group closure is incomplete");
    }
    std::ranges::sort(scene->transactions, {}, &PublishedStaticTransaction::transactionID);

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
