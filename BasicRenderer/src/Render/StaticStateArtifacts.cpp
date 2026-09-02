#include "Render/StaticStateArtifacts.h"

#include <algorithm>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "Render/PublishedRendererState.h"

namespace br::render {

namespace {

std::uint64_t MixStaticSceneValue(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31u);
}

} // namespace

std::size_t StaticScenePageIndex(std::uint64_t groupID) noexcept {
    return static_cast<std::size_t>(MixStaticSceneValue(groupID) &
        (kStaticScenePageCount - 1u));
}

std::uint64_t StaticSceneGroupDigest(std::uint64_t groupID) noexcept {
    return MixStaticSceneValue(groupID);
}

std::uint64_t StaticScenePlacementDigest(std::uint64_t groupDigest,
    std::uint64_t placementCount, std::uint64_t groupCount) noexcept {
    return MixStaticSceneValue(groupDigest ^ MixStaticSceneValue(placementCount) ^
        MixStaticSceneValue(groupCount));
}

bool PublishedStaticScenePage::ContainsGroup(std::uint64_t groupID) const noexcept {
    return FindGroup(groupID) != nullptr;
}

const StaticSceneGroupOwner* PublishedStaticScenePage::FindOwner(
    std::uint64_t groupID) const noexcept {
    if (std::ranges::binary_search(removedGroupIDs, groupID)) return nullptr;
    const auto it = std::ranges::lower_bound(groupOwners, groupID, {},
        &StaticSceneGroupOwner::groupID);
    if (it != groupOwners.end() && it->groupID == groupID) return std::addressof(*it);
    return basePage ? basePage->FindOwner(groupID) : nullptr;
}

const StaticTransactionGroup* PublishedStaticScenePage::FindGroup(
    std::uint64_t groupID) const noexcept {
    if (std::ranges::binary_search(removedGroupIDs, groupID)) return nullptr;
    const auto it = std::ranges::lower_bound(groups, groupID, {},
        &StaticTransactionGroup::groupID);
    if (it != groups.end() && it->groupID == groupID) return std::addressof(*it);
    return basePage ? basePage->FindGroup(groupID) : nullptr;
}

void PublishedStaticScenePage::MaterializeGroups(
    std::vector<StaticTransactionGroup>& out) const {
    std::map<std::uint64_t, StaticTransactionGroup> effective;
    std::vector<const PublishedStaticScenePage*> layers;
    for (auto layer = this; layer != nullptr; layer = layer->basePage.get()) {
        layers.push_back(layer);
    }
    for (auto layer = layers.rbegin(); layer != layers.rend(); ++layer) {
        for (const auto groupID : (*layer)->removedGroupIDs) effective.erase(groupID);
        for (const auto& group : (*layer)->groups) effective.insert_or_assign(group.groupID, group);
    }
    out.clear();
    out.reserve(effective.size());
    for (auto& [_, group] : effective) out.push_back(std::move(group));
}

void PublishedStaticScenePage::MaterializeOwners(
    std::vector<StaticSceneGroupOwner>& out) const {
    std::map<std::uint64_t, StaticSceneGroupOwner> effective;
    std::vector<const PublishedStaticScenePage*> layers;
    for (auto layer = this; layer != nullptr; layer = layer->basePage.get()) {
        layers.push_back(layer);
    }
    for (auto layer = layers.rbegin(); layer != layers.rend(); ++layer) {
        for (const auto groupID : (*layer)->removedGroupIDs) effective.erase(groupID);
        for (const auto& owner : (*layer)->groupOwners) {
            effective.insert_or_assign(owner.groupID, owner);
        }
    }
    out.clear();
    out.reserve(effective.size());
    for (auto& [_, owner] : effective) out.push_back(std::move(owner));
}

bool PublishedStaticSceneState::ContainsGroup(std::uint64_t groupID) const noexcept {
    return FindGroup(groupID) != nullptr;
}

const StaticTransactionGroup* PublishedStaticSceneState::FindGroup(
    std::uint64_t groupID) const noexcept {
    const auto& page = pages[StaticScenePageIndex(groupID)];
    return page ? page->FindGroup(groupID) : nullptr;
}

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

ArtifactBuildResult BuildStaticScenePage(const ArtifactBuildContext& context) {
    const auto input = context.input.Get<StaticScenePageBuildInput>();
    if (!input || input->pageIndex >= kStaticScenePageCount ||
        context.key.primaryID != static_cast<std::uint64_t>(input->pageIndex) + 1u) {
        return ArtifactBuildResult::Failure("static scene page immutable input is invalid");
    }

    const auto* basePage = input->basePagePayload.get();
    if (static_cast<bool>(input->basePage) != static_cast<bool>(basePage) ||
        (basePage && basePage->pageIndex != input->pageIndex)) {
        return ArtifactBuildResult::Failure("static scene page base payload is inconsistent");
    }
    std::map<ArtifactVersionID, const PublishedStaticTransaction*> transactions;
    for (const auto& dependency : context.dependencies) {
        if (dependency.key.kind == ArtifactKind::StaticScenePage) {
            return ArtifactBuildResult::Failure("static scene page cannot depend on its own address");
        } else if (dependency.key.kind == ArtifactKind::StaticTransaction) {
            const auto transaction = dependency.payload.Get<PublishedStaticTransaction>();
            if (!transaction || transaction->transactionID != dependency.key.primaryID) {
                return ArtifactBuildResult::Failure("static scene page transaction dependency is invalid");
            }
            transactions.emplace(dependency.Version(), transaction.get());
        }
    }
    auto removals = input->removedGroupIDs;
    std::ranges::sort(removals);
    if (std::ranges::adjacent_find(removals) != removals.end()) {
        return ArtifactBuildResult::Failure("static scene page contains duplicate removals");
    }
    auto owners = input->groupOwners;
    std::ranges::sort(owners, {}, &StaticSceneGroupOwner::groupID);
    if (std::ranges::adjacent_find(owners, {}, &StaticSceneGroupOwner::groupID) != owners.end()) {
        return ArtifactBuildResult::Failure("static scene page contains duplicate owners");
    }
    for (const auto& owner : owners) {
        if (owner.groupID == 0 || StaticScenePageIndex(owner.groupID) != input->pageIndex ||
            owner.transaction.address.kind != ArtifactKind::StaticTransaction || !owner.transaction ||
            std::ranges::binary_search(removals, owner.groupID)) {
            return ArtifactBuildResult::Failure("static scene page group owner is invalid");
        }
    }
    auto page = std::make_shared<PublishedStaticScenePage>();
    page->pageIndex = input->pageIndex;
    page->sourceFingerprint = input->sourceFingerprint;
    page->pageGeneration = context.generation;
    page->successorDepth = basePage ? basePage->successorDepth + 1u : 0u;
    page->basePage = input->basePagePayload;
    page->groupOwners = std::move(owners);
    page->removedGroupIDs = std::move(removals);
    if (basePage) {
        page->groupDigest = basePage->groupDigest;
        page->groupCount = basePage->groupCount;
        page->drawRecordCount = basePage->drawRecordCount;
        page->activeEntryCount = basePage->activeEntryCount;
        page->placementCount = basePage->placementCount;
    }
    page->groups.reserve(page->groupOwners.size());
    for (const auto& owner : page->groupOwners) {
        const auto transactionIt = transactions.find(owner.transaction);
        if (transactionIt == transactions.end()) {
            return ArtifactBuildResult::Failure("static scene page changed owner transaction is missing");
        }
        const auto& groups = transactionIt->second->groups;
        const auto groupIt = std::ranges::lower_bound(groups, owner.groupID, {},
            &StaticTransactionGroup::groupID);
        if (groupIt == groups.end() || groupIt->groupID != owner.groupID) {
            return ArtifactBuildResult::Failure("static scene page transaction omits its group");
        }
        if (const auto* oldGroup = basePage ? basePage->FindGroup(owner.groupID) : nullptr) {
            page->drawRecordCount -= oldGroup->drawRecordCount;
            page->activeEntryCount -= oldGroup->activeEntryCount;
            page->placementCount -= oldGroup->placementCount;
        } else {
            page->groupDigest ^= StaticSceneGroupDigest(owner.groupID);
            ++page->groupCount;
        }
        page->drawRecordCount += groupIt->drawRecordCount;
        page->activeEntryCount += groupIt->activeEntryCount;
        page->placementCount += groupIt->placementCount;
        page->groups.push_back(*groupIt);
    }
    for (const auto groupID : page->removedGroupIDs) {
        const auto* oldGroup = basePage ? basePage->FindGroup(groupID) : nullptr;
        if (!oldGroup) continue;
        page->groupDigest ^= StaticSceneGroupDigest(groupID);
        --page->groupCount;
        page->drawRecordCount -= oldGroup->drawRecordCount;
        page->activeEntryCount -= oldGroup->activeEntryCount;
        page->placementCount -= oldGroup->placementCount;
    }
    // Immutable deltas make the common update proportional to its mutation set,
    // but an unbounded shared_ptr chain would retain every historical layer and
    // turn owner lookup into progressively more work. Periodically flatten the
    // effective page into a fresh immutable snapshot to bound both costs.
    if (page->successorDepth >= kStaticScenePageMaxSuccessorDepth) {
        std::vector<StaticSceneGroupOwner> effectiveOwners;
        std::vector<StaticTransactionGroup> effectiveGroups;
        page->MaterializeOwners(effectiveOwners);
        page->MaterializeGroups(effectiveGroups);
        page->basePage.reset();
        page->successorDepth = 0;
        page->groupOwners = std::move(effectiveOwners);
        page->groups = std::move(effectiveGroups);
        page->removedGroupIDs.clear();
    }
    return ArtifactBuildResult::Ready(
        ArtifactPayload::Make<PublishedStaticScenePage>(std::move(page)));
}

ArtifactBuildResult BuildStaticScene(const ArtifactBuildContext& context) {
    const auto input = context.input.Get<StaticSceneBuildInput>();
    if (!input) return ArtifactBuildResult::Failure("static scene immutable input missing");

    std::array<ArtifactVersionID, kStaticScenePageCount> expectedPages{};
    for (const auto& page : input->pages) {
        if (page.pageIndex >= kStaticScenePageCount || !page.page ||
            page.page.address.kind != ArtifactKind::StaticScenePage ||
            page.page.address.primaryID != static_cast<std::uint64_t>(page.pageIndex) + 1u ||
            expectedPages[page.pageIndex]) {
            return ArtifactBuildResult::Failure("static scene page reference is invalid");
        }
        expectedPages[page.pageIndex] = page.page;
    }
    const auto expectedResourceRoots = input->requireResourceClosure ? 3u : 0u;
    if (context.dependencies.size() != input->pages.size() + expectedResourceRoots) {
        return ArtifactBuildResult::Failure("static scene dependency closure is incomplete");
    }

    bool hasMaterialRoot = false;
    bool hasObjectBufferRoot = false;
    bool hasIndirectRoot = false;
    for (const auto& dependency : context.dependencies) {
        if (dependency.key.kind == ArtifactKind::StaticScenePage) continue;
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
    for (const auto& dependency : context.dependencies) {
        if (dependency.key.kind != ArtifactKind::StaticScenePage) continue;
        const auto pageIndex = dependency.key.primaryID - 1u;
        if (pageIndex >= kStaticScenePageCount ||
            expectedPages[pageIndex] != dependency.Version()) {
            return ArtifactBuildResult::Failure("static scene contains an unexpected page");
        }
        const auto page = dependency.payload.Get<PublishedStaticScenePage>();
        if (!page || page->pageIndex != pageIndex) {
            return ArtifactBuildResult::Failure("static scene page payload mismatch");
        }
        scene->pages[pageIndex] = page;
        scene->groupCount += page->groupCount;
        scene->drawRecordCount += page->drawRecordCount;
        scene->activeEntryCount += page->activeEntryCount;
        scene->publishedPlacementCount += page->placementCount;
        scene->placementSetDigest ^= page->groupDigest;
    }
    if (scene->materializedPlacementCount != scene->publishedPlacementCount ||
        scene->desiredPlacementCount != scene->publishedPlacementCount) {
        return ArtifactBuildResult::Failure("static scene placement closure is incomplete");
    }
    scene->placementSetDigest = StaticScenePlacementDigest(scene->placementSetDigest,
        scene->publishedPlacementCount, scene->groupCount);

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
            return dependency.key.kind == ArtifactKind::StaticScenePage;
        });
    root->fragment.payload = ArtifactPayload::Make<PublishedStaticSceneState>(std::move(scene));
    return ArtifactBuildResult::Ready(
        ArtifactPayload::Make<RendererStateFragmentArtifact>(std::move(root)));
}

} // namespace

void RegisterStaticStateProducers(AsyncStateGraph& graph) {
    graph.RegisterProducer(ArtifactKind::StaticTransaction, {
        TaskLane::FrameCritical, TaskDomain::GraphPublication,
        "StaticStateArtifact::BuildTransaction", BuildStaticTransaction });
    graph.RegisterProducer(ArtifactKind::StaticScenePage, {
        TaskLane::FrameCritical, TaskDomain::GraphPublication,
        "StaticStateArtifact::BuildScenePage", BuildStaticScenePage });
    graph.RegisterProducer(ArtifactKind::StaticScene, {
        TaskLane::FrameCritical, TaskDomain::GraphPublication,
        "StaticStateArtifact::BuildScene", BuildStaticScene });
}

} // namespace br::render
