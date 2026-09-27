#include "Runtime/Publication/PersistentRendererPublication.h"
#include "Render/PublicationBindingBundle.h"
#include <BasicTelemetry/Telemetry.h>
#include <BasicTelemetry/Tracy.h>
#include <atomic>
#include <stdexcept>
#include <unordered_set>

namespace br::render {

void PreparePersistentRendererPublication(
    const std::shared_ptr<const PublishedRendererState>& base, PublishedRendererState& successor) {
    BT_ZONE_SCOPE("BR.Publication.PreparePersistentBindings");
    const auto previous = base ? base->persistentPublication : nullptr;
    bool changed = !previous;
    for (const auto owner : {PublishedFragmentKind::Materials,PublishedFragmentKind::Geometry}) {
        const auto index = static_cast<size_t>(owner);
        const auto prior = base && base->resourceCatalog ? base->resourceCatalog->ownerShards[index] : nullptr;
        const auto next = successor.resourceCatalog ? successor.resourceCatalog->ownerShards[index] : nullptr;
        changed |= prior != next;
    }
    if (!changed) { successor.persistentPublication = previous; return; }
    auto publication = previous ? std::make_shared<PersistentRendererPublication>(*previous)
                                : std::make_shared<PersistentRendererPublication>();
    const auto graphBase = previous ? previous->graph : [] {
        org::persistent::GraphProgram bootstrap;
        return bootstrap.Select();
    }();
    org::persistent::GraphEditTransaction edit(graphBase);
    for (const auto owner : {PublishedFragmentKind::Materials,PublishedFragmentKind::Geometry}) {
        const auto index = static_cast<size_t>(owner);
        const auto prior = base && base->resourceCatalog ? base->resourceCatalog->ownerShards[index] : nullptr;
        const auto next = successor.resourceCatalog ? successor.resourceCatalog->ownerShards[index] : nullptr;
        if (previous && prior == next) continue;
        std::unordered_set<PublishedResourceKey,PublishedResourceKey::Hasher> retained;
        if (next) for (const auto& [key,selection] : next->selections) {
            retained.insert(key);
            if (!selection.bindingBundle || !selection.initialStates)
                throw std::runtime_error("Persistent producer selection lacks exact binding/state snapshots");
            const auto snapshots = selection.bindingBundle->Bindings();
            if (snapshots.size() != selection.initialStates->size())
                throw std::runtime_error("Persistent producer selection snapshot enumeration mismatch");
            auto& slots = publication->slots[key];
            for (size_t member = 0; member < snapshots.size(); ++member) {
                auto initial = selection.initialStates->at(member);
                if (initial.graphResourceID != snapshots[member]->resourceID)
                    throw std::runtime_error("Persistent producer selection state belongs to another snapshot");
                const bool replacing = member < slots.size();
                initial.graphResourceID = replacing ? uint64_t{slots[member].index} + 1u : 0u;
                auto binding = org::persistent::BindingVersion::FromSnapshot(snapshots[member],initial,
                    selection.contentVersion,selection.contentVersion);
                if (replacing) edit.ReplaceBinding(slots[member],std::move(binding));
                else {
                    // AddResource allocates the stable identity; replace once to
                    // stamp admission with that identity rather than a dense index.
                    auto declarationBinding = binding;
                    declarationBinding.admission.reset();
                    const auto slot = edit.AddResource(binding.shape,std::move(declarationBinding));
                    if (snapshots[member]->description.type == rhi::ResourceType::Buffer) {
                        auto contract = org::persistent::NativeBindingContract::Capture(snapshots[member]->description);
                        // These producer tables grow independently of declarations;
                        // invocation/command range checks remain authoritative.
                        contract.minimumBufferBytes = 1;
                        contract.maximumBufferBytes = UINT64_MAX;
                        edit.SetNativeBindingContract(slot,std::move(contract));
                    }
                    initial.graphResourceID = uint64_t{slot.index} + 1u;
                    edit.ReplaceSnapshot(slot,snapshots[member],initial,selection.contentVersion,selection.contentVersion);
                    slots.push_back(slot);
                }
                basic_telemetry::AddCounter("BR.Publication.PersistentBindingEdits");
            }
            while (slots.size() > snapshots.size()) { edit.RemoveResource(slots.back()); slots.pop_back(); }
        }
        std::erase_if(publication->slots,[&](const auto& entry) {
            if (entry.first.owner != owner || retained.contains(entry.first)) return false;
            for (const auto slot : entry.second) edit.RemoveResource(slot);
            return true;
        });
    }
    org::experimental::CompileWorkspace workspace;
    const std::atomic_bool cancelled{false};
    publication->graph = edit.Build(workspace,cancelled);
    if (!publication->graph) throw std::runtime_error("Persistent renderer publication failed to build");
    successor.persistentPublication = std::move(publication);
}

} // namespace br::render
