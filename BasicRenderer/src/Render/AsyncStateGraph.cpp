#include "Render/AsyncStateGraph.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <format>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include <spdlog/spdlog.h>
#include <tbb/concurrent_queue.h>

#include <BasicTelemetry/Telemetry.h>

#include "Render/Runtime/StreamingUploadTypes.h"

namespace br::render {
namespace {

bool Satisfies(ArtifactReadiness actual, ArtifactReadiness required) {
    if (actual == ArtifactReadiness::Published) return true;
    if (actual == ArtifactReadiness::Failed || actual == ArtifactReadiness::Cancelled ||
        actual == ArtifactReadiness::Superseded) return false;
    return static_cast<unsigned>(actual) >= static_cast<unsigned>(required);
}

std::string KeyString(const ArtifactKey& key) {
    return std::format("{}:{}:{}", static_cast<unsigned>(key.kind), key.primaryID, key.variantID);
}

void HashRequestValue(std::uint64_t& hash, std::uint64_t value) {
    hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
}

std::uint64_t CanonicalRequestFingerprint(const ArtifactKey& key,
    std::uint64_t revision, const std::vector<ArtifactRequirement>& requirements) {
    std::uint64_t hash = 1469598103934665603ull;
    HashRequestValue(hash, static_cast<std::uint64_t>(key.kind));
    HashRequestValue(hash, key.primaryID);
    HashRequestValue(hash, key.variantID);
    HashRequestValue(hash, revision);
    for (const auto& requirement : requirements) {
        HashRequestValue(hash, static_cast<std::uint64_t>(requirement.key.kind));
        HashRequestValue(hash, requirement.key.primaryID);
        HashRequestValue(hash, requirement.key.variantID);
        HashRequestValue(hash, requirement.minimumRevision);
        HashRequestValue(hash, static_cast<std::uint64_t>(requirement.requiredReadiness));
        HashRequestValue(hash, static_cast<std::uint64_t>(requirement.policy));
        HashRequestValue(hash, requirement.alternativeGroup);
        HashRequestValue(hash, static_cast<std::uint64_t>(requirement.invalidation));
    }
    return hash == 0 ? 1u : hash;
}

} // namespace

std::size_t ArtifactKey::Hasher::operator()(const ArtifactKey& key) const noexcept {
    auto value = static_cast<std::size_t>(key.kind);
    value ^= std::hash<std::uint64_t>{}(key.primaryID) + 0x9e3779b9u + (value << 6u) + (value >> 2u);
    value ^= std::hash<std::uint64_t>{}(key.variantID) + 0x9e3779b9u + (value << 6u) + (value >> 2u);
    return value;
}

ArtifactBuildResult ArtifactBuildResult::Ready(ArtifactPayload payload,
    std::shared_ptr<const GpuSubmissionSet> gpuSubmissions) {
    ArtifactBuildResult result;
    result.outcome = Outcome::Ready;
    result.payload = std::move(payload);
    result.gpuSubmissions = std::move(gpuSubmissions);
    return result;
}

ArtifactBuildResult ArtifactBuildResult::Needs(std::vector<ArtifactRequirement> requirements,
    ArtifactPayload checkpoint) {
    ArtifactBuildResult result;
    result.outcome = Outcome::NeedsDependencies;
    result.requirements = std::move(requirements);
    result.checkpoint = std::move(checkpoint);
    return result;
}

ArtifactBuildResult ArtifactBuildResult::Retry(std::chrono::steady_clock::duration delay,
    ArtifactPayload checkpoint) {
    ArtifactBuildResult result;
    result.outcome = Outcome::RetryAfter;
    result.retryDelay = delay;
    result.checkpoint = std::move(checkpoint);
    return result;
}

ArtifactBuildResult ArtifactBuildResult::Failure(std::string error) {
    ArtifactBuildResult result;
    result.outcome = Outcome::Failed;
    result.error = std::move(error);
    return result;
}

ArtifactBuildResult ArtifactBuildResult::Cancelled() {
    ArtifactBuildResult result;
    result.outcome = Outcome::Cancelled;
    return result;
}

std::shared_ptr<const GpuSubmissionSet> MakeGpuSubmissionSet(
    const std::shared_ptr<org::TrackedUploadTicket>& ticket) {
    if (!ticket) return {};
    auto token = std::make_shared<GpuSubmissionSet>();
    GpuQueueSubmission submission;
    {
        std::lock_guard lock(ticket->timelineMutex);
        submission.timelineOwner = ticket->timelineOwner;
        submission.value = ticket->timelineValue;
    }
    token->isComplete = [ticket] { return ticket->Complete(); };
    submission.currentTimelineOwner = [ticket] {
        std::lock_guard lock(ticket->timelineMutex);
        return ticket->timelineOwner;
    };
    submission.currentValue = [ticket] {
        std::lock_guard lock(ticket->timelineMutex);
        return ticket->timelineValue;
    };
    token->submissions.push_back(std::move(submission));
    token->subscribe = [ticket](std::function<void()> callback) {
        auto fired = std::make_shared<std::atomic_bool>(false);
        auto notify = [callback = std::move(callback), fired]() mutable {
            if (!fired->exchange(true, std::memory_order_acq_rel) && callback) callback();
        };
        ticket->SetChangeCallback(notify);
        // Completion can race callback installation. Recheck after subscribing so
        // an already-complete ticket cannot strand its graph node indefinitely.
        if (ticket->Complete()) notify();
    };
    token->cancel = [ticket] { return ticket->Cancel(); };
    return token;
}

struct AsyncStateGraph::Impl : std::enable_shared_from_this<Impl> {
    struct Node {
        ArtifactKey key;
        std::uint64_t desiredRevision = 0;
        std::uint64_t producedRevision = 0;
        std::uint64_t generation = 0;
        std::uint64_t requestFingerprint = 0;
        ArtifactReadiness state = ArtifactReadiness::Missing;
        ArtifactPayload payload;
        ArtifactPayload input;
        ArtifactPayload checkpoint;
        std::vector<ArtifactRequirement> requirements;
        std::vector<ArtifactSnapshot> resolvedDependencies;
        std::shared_ptr<const GpuSubmissionSet> gpuSubmissions;
        std::shared_ptr<const GpuSubmissionSet> waitingGpuSubmissions;
        std::optional<std::chrono::steady_clock::time_point> retryAt;
        std::string error;
        std::chrono::steady_clock::time_point stateSince = std::chrono::steady_clock::now();
        bool buildInFlight = false;
        bool desired = true;
        bool terminalFailure = false;
        bool published = false;
        std::chrono::steady_clock::time_point queuedAt{};
        std::chrono::steady_clock::time_point buildStartedAt{};
        std::chrono::steady_clock::time_point uploadSubmittedAt{};
        std::uint8_t gpuRecoveryChecksRemaining = 0;
    };

    struct Completion {
        ArtifactKey key;
        std::uint64_t revision = 0;
        std::uint64_t generation = 0;
        std::vector<ArtifactSnapshot> dependencies;
        ArtifactBuildResult result;
    };

    TaskSchedulerManager& scheduler;
    TaskScope scope;
    mutable std::mutex mutex;
    std::unordered_map<ArtifactKey, Node, ArtifactKey::Hasher> nodes;
    std::unordered_map<ArtifactKey, std::unordered_set<ArtifactKey, ArtifactKey::Hasher>, ArtifactKey::Hasher> waiters;
    std::unordered_map<ArtifactKind, ArtifactProducerRegistration> producers;
    std::deque<ArtifactKey> pending;
    std::deque<Completion> completions;
    tbb::concurrent_queue<ArtifactKey> gpuSignals;
    std::deque<ArtifactKey> gpuRecovery;
    std::unordered_map<std::uint64_t, std::function<void(const ArtifactSnapshot&)>> readyCallbacks;
    std::uint64_t nextReadyCallback = 0;
    AsyncStateGraphStats stats;
    std::atomic_bool drainScheduled{ false };
    std::atomic_bool shuttingDown{ false };

    Impl(TaskSchedulerManager& schedulerIn, std::string_view name)
        : scheduler(schedulerIn), scope(schedulerIn.CreateScope(name)) {}

    void SetState(Node& node, ArtifactReadiness state) {
        node.state = state;
        node.stateSince = std::chrono::steady_clock::now();
    }

    ArtifactSnapshot MakeSnapshot(const Node& node) const {
        return { node.key, node.producedRevision, node.generation, node.state, node.payload,
            node.gpuSubmissions };
    }

    void RemoveWaiterEdges(const Node& node) {
        for (const auto& requirement : node.requirements) {
            if (auto found = waiters.find(requirement.key); found != waiters.end()) {
                found->second.erase(node.key);
                if (found->second.empty()) waiters.erase(found);
            }
        }
    }

    void InstallWaiterEdges(const Node& node) {
        for (const auto& requirement : node.requirements) waiters[requirement.key].insert(node.key);
    }

    bool FindPath(const ArtifactKey& from, const ArtifactKey& target,
        std::unordered_set<ArtifactKey, ArtifactKey::Hasher>& visited,
        std::vector<ArtifactKey>& path) const {
        if (!visited.insert(from).second) return false;
        path.push_back(from);
        if (from == target) return true;
        const auto found = nodes.find(from);
        if (found != nodes.end()) {
            for (const auto& requirement : found->second.requirements) {
                if (requirement.policy == DependencyPolicy::Optional ||
                    requirement.invalidation == DependencyInvalidationPolicy::LifetimeHold) continue;
                if (FindPath(requirement.key, target, visited, path)) return true;
            }
        }
        path.pop_back();
        return false;
    }

    std::vector<ArtifactKey> DetectCycle(const Node& node) const {
        for (const auto& requirement : node.requirements) {
            if (requirement.policy == DependencyPolicy::Optional ||
                requirement.invalidation == DependencyInvalidationPolicy::LifetimeHold) continue;
            std::unordered_set<ArtifactKey, ArtifactKey::Hasher> visited;
            std::vector<ArtifactKey> path;
            if (!FindPath(requirement.key, node.key, visited, path)) continue;
            path.insert(path.begin(), node.key);
            return path;
        }
        return {};
    }

    std::string CycleString(const std::vector<ArtifactKey>& cycle) const {
        std::string message;
        for (const auto& key : cycle) {
            if (!message.empty()) message += " -> ";
            message += KeyString(key);
        }
        return message;
    }

    void FailCycle(const std::vector<ArtifactKey>& cycle) {
        const auto error = "dependency cycle: " + CycleString(cycle);
        std::unordered_set<ArtifactKey, ArtifactKey::Hasher> unique;
        for (const auto& key : cycle) {
            if (!unique.insert(key).second) continue;
            const auto found = nodes.find(key);
            if (found == nodes.end()) continue;
            found->second.error = error;
            found->second.terminalFailure = true;
            ++found->second.generation;
            found->second.retryAt.reset();
            SetState(found->second, ArtifactReadiness::Failed);
            ++stats.failed;
        }
        ++stats.cycles;
    }

    bool RequirementSatisfied(const ArtifactRequirement& requirement) const {
        const auto found = nodes.find(requirement.key);
        return found != nodes.end() && found->second.producedRevision >= requirement.minimumRevision &&
            Satisfies(found->second.state, requirement.requiredReadiness);
    }

    bool DependenciesSatisfied(const Node& node) const {
        std::unordered_map<std::uint32_t, bool> anyGroups;
        std::unordered_map<std::uint32_t, bool> fallbackGroups;
        for (const auto& requirement : node.requirements) {
            const bool satisfied = RequirementSatisfied(requirement);
            if (requirement.invalidation == DependencyInvalidationPolicy::LifetimeHold) continue;
            switch (requirement.policy) {
            case DependencyPolicy::Optional: break;
            case DependencyPolicy::AnyOf:
                anyGroups[requirement.alternativeGroup] |= satisfied;
                break;
            case DependencyPolicy::AllOf:
                if (!satisfied) return false;
                break;
            case DependencyPolicy::FallbackAllowed:
                fallbackGroups[requirement.alternativeGroup] |= satisfied;
                break;
            }
        }
        for (const auto& [_, satisfied] : anyGroups) if (!satisfied) return false;
        for (const auto& [_, satisfied] : fallbackGroups) if (!satisfied) return false;
        return true;
    }

    void AppendBlockerChain(const ArtifactKey& key,
        std::unordered_set<ArtifactKey, ArtifactKey::Hasher>& visited,
        std::string& output) const {
        if (!visited.insert(key).second) {
            output += KeyString(key) + " (cycle)";
            return;
        }
        const auto found = nodes.find(key);
        if (found == nodes.end()) {
            output += KeyString(key) + " (missing)";
            return;
        }
        const auto& node = found->second;
        output += std::format("{} rev {}/{} state {}", KeyString(key), node.producedRevision,
            node.desiredRevision, static_cast<unsigned>(node.state));
        if (!node.error.empty()) output += " error=" + node.error;
        if (node.retryAt) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                *node.retryAt - std::chrono::steady_clock::now()).count();
            output += std::format(" retry-in={}ms", (std::max)(std::int64_t{ 0 }, remaining));
        }
        if (node.state == ArtifactReadiness::UploadSubmitted && node.waitingGpuSubmissions) {
            output += std::format(" gpu-value={}", node.waitingGpuSubmissions->MaximumTimelineValue());
			const auto detail = node.waitingGpuSubmissions->Describe();
			if (!detail.empty()) output += " " + detail;
        }
        for (const auto& requirement : node.requirements) {
            if (requirement.policy == DependencyPolicy::Optional || RequirementSatisfied(requirement)) continue;
            output += " <- [";
            AppendBlockerChain(requirement.key, visited, output);
            output += "]";
        }
        visited.erase(key);
    }

    std::vector<ArtifactSnapshot> DependencySnapshots(const Node& node) const {
        std::vector<ArtifactSnapshot> result;
        result.reserve(node.requirements.size());
        std::unordered_set<std::uint64_t> selectedGroups;
        for (const auto& requirement : node.requirements) {
            const bool alternative = requirement.policy == DependencyPolicy::AnyOf ||
                requirement.policy == DependencyPolicy::FallbackAllowed;
            const auto groupIdentity = (static_cast<std::uint64_t>(requirement.policy) << 32u) |
                requirement.alternativeGroup;
            if (alternative && selectedGroups.contains(groupIdentity)) continue;
            const auto found = nodes.find(requirement.key);
            if (found != nodes.end() && RequirementSatisfied(requirement)) {
                result.push_back(MakeSnapshot(found->second));
                if (alternative) selectedGroups.insert(groupIdentity);
            }
        }
        return result;
    }

    void QueueNode(Node& node) {
        if (!node.desired || node.terminalFailure || node.buildInFlight ||
            node.state == ArtifactReadiness::Queued ||
            node.state == ArtifactReadiness::UploadSubmitted) return;
        if (node.producedRevision == node.desiredRevision &&
            (node.state == ArtifactReadiness::GpuReady ||
             node.state == ArtifactReadiness::Published)) return;
        if (!DependenciesSatisfied(node)) {
            SetState(node, ArtifactReadiness::Blocked);
            return;
        }
        SetState(node, ArtifactReadiness::Queued);
        node.queuedAt = std::chrono::steady_clock::now();
        pending.push_back(node.key);
    }

    void WakeWaiters(const ArtifactKey& key) {
        const auto found = waiters.find(key);
        if (found == waiters.end()) return;
        const auto keys = found->second;
        for (const auto& waiter : keys) {
            const auto nodeFound = nodes.find(waiter);
            if (nodeFound == nodes.end()) continue;
            auto& node = nodeFound->second;

            bool keyIsSelected = false;
            DependencyInvalidationPolicy invalidation = DependencyInvalidationPolicy::Latest;
            for (const auto& requirement : node.requirements) {
                if (requirement.key != key) continue;
                invalidation = requirement.invalidation;
                if (requirement.policy == DependencyPolicy::AnyOf ||
                    requirement.policy == DependencyPolicy::FallbackAllowed) {
                    for (const auto& candidate : node.requirements) {
                        if (candidate.policy == requirement.policy &&
                            candidate.alternativeGroup == requirement.alternativeGroup &&
                            RequirementSatisfied(candidate)) {
                            keyIsSelected = candidate.key == key;
                            break;
                        }
                    }
                } else {
                    keyIsSelected = RequirementSatisfied(requirement);
                }
                break;
            }

            const auto current = nodes.find(key);
            const bool exactSelectionAlreadyUsed = current != nodes.end() &&
                std::ranges::any_of(node.resolvedDependencies,
                    [&](const ArtifactSnapshot& dependency) {
                        return dependency.key == key &&
                            dependency.revision == current->second.producedRevision &&
                            dependency.generation == current->second.generation;
                    });
            const bool consumerMustBeRebuilt =
                node.buildInFlight ||
                node.state == ArtifactReadiness::GpuReady ||
                node.state == ArtifactReadiness::Published ||
                node.state == ArtifactReadiness::UploadSubmitted;
            if (consumerMustBeRebuilt && keyIsSelected && !exactSelectionAlreadyUsed &&
                invalidation == DependencyInvalidationPolicy::Latest) {
                ++node.generation;
                node.retryAt.reset();
                if (node.waitingGpuSubmissions) {
                    (void)node.waitingGpuSubmissions->Cancel();
                    if (node.state == ArtifactReadiness::UploadSubmitted && stats.gpuWaiting) {
                        --stats.gpuWaiting;
                    }
                    node.waitingGpuSubmissions.reset();
                }
                node.gpuSubmissions.reset();
                SetState(node, ArtifactReadiness::Superseded);
            }
            QueueNode(node);
        }
    }

    void SupersedeDependents(const ArtifactKey& key,
        std::unordered_set<ArtifactKey, ArtifactKey::Hasher>& visited) {
        if (!visited.insert(key).second) return;
        const auto found = waiters.find(key);
        if (found == waiters.end()) return;
        const auto dependentKeys = found->second;
        for (const auto& dependentKey : dependentKeys) {
            const auto dependent = nodes.find(dependentKey);
            if (dependent == nodes.end()) continue;
            auto& node = dependent->second;
            const bool rebuildsForLatest = std::ranges::any_of(node.requirements,
                [&](const ArtifactRequirement& requirement) {
                    return requirement.key == key &&
                        requirement.invalidation == DependencyInvalidationPolicy::Latest;
                });
            if (!rebuildsForLatest) continue;
            const bool hasResolvedSelection = !node.resolvedDependencies.empty();
            const bool selectedThisDependency = std::ranges::any_of(
                node.resolvedDependencies,
                [&](const ArtifactSnapshot& dependency) { return dependency.key == key; });
            if (hasResolvedSelection && !selectedThisDependency &&
                (node.state == ArtifactReadiness::GpuReady ||
                 node.state == ArtifactReadiness::Published ||
                 node.state == ArtifactReadiness::UploadSubmitted)) {
                continue;
            }
            ++node.generation;
            node.retryAt.reset();
            if (node.waitingGpuSubmissions) {
                (void)node.waitingGpuSubmissions->Cancel();
                if (node.state == ArtifactReadiness::UploadSubmitted && stats.gpuWaiting) {
                    --stats.gpuWaiting;
                }
                node.waitingGpuSubmissions.reset();
            }
            node.gpuSubmissions.reset();
            SetState(node, ArtifactReadiness::Superseded);
            if (!node.buildInFlight) QueueNode(node);
            SupersedeDependents(dependentKey, visited);
        }
    }

    bool DependenciesStillMatch(const Completion& completion) const {
        for (const auto& dependency : completion.dependencies) {
            const auto requirement = std::ranges::find_if(
                nodes.at(completion.key).requirements,
                [&](const ArtifactRequirement& value) { return value.key == dependency.key; });
            if (requirement != nodes.at(completion.key).requirements.end() &&
                requirement->invalidation != DependencyInvalidationPolicy::Latest) continue;
            const auto found = nodes.find(dependency.key);
            if (found == nodes.end()) return false;
            const auto& current = found->second;
            if (current.generation != dependency.generation ||
                current.producedRevision != dependency.revision ||
                !Satisfies(current.state, dependency.readiness)) return false;
        }
        return true;
    }

    void ScheduleDrain(std::chrono::steady_clock::duration delay = {}) {
        if (shuttingDown.load(std::memory_order_acquire) || drainScheduled.exchange(true)) return;
        auto weak = weak_from_this();
        auto body = [weak](const TaskContext&) {
            if (auto self = weak.lock()) self->Drain();
        };
        const bool submitted = delay == std::chrono::steady_clock::duration{}
            ? scheduler.Submit(scope, TaskLane::Streaming, TaskDomain::RendererState,
                "AsyncStateGraph::Drain", std::move(body))
            : scheduler.ScheduleAfter(scope, delay, TaskLane::Streaming, TaskDomain::RendererState,
                "AsyncStateGraph::DelayedDrain", std::move(body));
        if (!submitted) drainScheduled.store(false, std::memory_order_release);
    }

    void SubmitBuild(const ArtifactProducerRegistration& registration, ArtifactBuildContext context) {
        auto weak = weak_from_this();
        const auto key = context.key;
        const auto revision = context.revision;
        const auto generation = context.generation;
        auto dependencyStamp = context.dependencies;
        const bool submitted = scheduler.Submit(scope, registration.lane, registration.domain,
            registration.taskName.empty() ? "AsyncStateGraph::Build" : registration.taskName,
            [weak, registration, context = std::move(context), key, revision, generation,
                dependencyStamp = std::move(dependencyStamp)](const TaskContext& cancellation) mutable {
                auto self = weak.lock();
                if (!self) return;
                context.stopRequested = [cancellation] { return cancellation.StopRequested(); };
                ArtifactBuildResult result;
                try {
                    result = cancellation.StopRequested()
                        ? ArtifactBuildResult::Cancelled()
                        : registration.producer(context);
                } catch (const std::exception& exception) {
                    result = ArtifactBuildResult::Failure(exception.what());
                } catch (...) {
                    result = ArtifactBuildResult::Failure("unknown producer exception");
                }
                {
                    std::lock_guard lock(self->mutex);
                    self->completions.push_back({ key, revision, generation,
                        std::move(dependencyStamp), std::move(result) });
                }
                self->ScheduleDrain();
            });
        if (!submitted) {
            {
                std::lock_guard lock(mutex);
                completions.push_back({ key, revision, generation, std::move(dependencyStamp),
                    ArtifactBuildResult::Failure("scheduler rejected producer") });
            }
            ScheduleDrain();
        }
    }

    void ApplyCompletion(Completion completion, std::vector<ArtifactSnapshot>& ready) {
        const auto found = nodes.find(completion.key);
        if (found == nodes.end()) return;
        auto& node = found->second;
        if (node.generation != completion.generation || !DependenciesStillMatch(completion)) {
            ++stats.staleCompletions;
            basic_telemetry::AddCounter("SARP.AsyncStateGraph.StaleCompletions");
            node.buildInFlight = false;
            QueueNode(node);
            return;
        }
        node.buildInFlight = false;
        ++stats.buildsCompleted;
        if (node.buildStartedAt != std::chrono::steady_clock::time_point{}) {
            const auto buildDuration = std::chrono::steady_clock::now() - node.buildStartedAt;
            stats.buildMicros += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(buildDuration).count());
            basic_telemetry::Record("SARP.AsyncStateGraph.BuildLatencyNs",
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(buildDuration).count()));
        }
        auto& result = completion.result;
        if (result.outcome == ArtifactBuildResult::Outcome::Ready) {
            const auto producer = producers.find(node.key.kind);
            if (producer != producers.end() &&
                producer->second.outputType != std::type_index(typeid(void)) &&
                result.payload.Type() != producer->second.outputType) {
                result = ArtifactBuildResult::Failure("artifact output type mismatch");
            }
        }
        switch (result.outcome) {
        case ArtifactBuildResult::Outcome::Ready:
            node.terminalFailure = false;
            node.payload = std::move(result.payload);
            node.resolvedDependencies = completion.dependencies;
            node.checkpoint = {};
            node.retryAt.reset();
            node.producedRevision = completion.revision;
            node.published = false;
            node.gpuSubmissions = std::move(result.gpuSubmissions);
            if (node.gpuSubmissions && !node.gpuSubmissions->Complete()) {
                node.waitingGpuSubmissions = node.gpuSubmissions;
                SetState(node, ArtifactReadiness::UploadSubmitted);
                node.uploadSubmittedAt = std::chrono::steady_clock::now();
                node.gpuRecoveryChecksRemaining = 8;
                ++stats.gpuWaiting;
                gpuRecovery.push_back(node.key);
                if (node.waitingGpuSubmissions->subscribe) {
                    auto weak = weak_from_this();
                    const auto key = node.key;
                    node.waitingGpuSubmissions->subscribe([weak, key] {
                        if (auto self = weak.lock()) {
                            {
                                self->gpuSignals.push(key);
                            }
                            self->ScheduleDrain();
                        }
                    });
                }
                ready.push_back(MakeSnapshot(node));
                WakeWaiters(node.key);
            } else {
                node.waitingGpuSubmissions.reset();
                SetState(node, node.published ? ArtifactReadiness::Published
                                             : ArtifactReadiness::GpuReady);
                ready.push_back(MakeSnapshot(node));
                WakeWaiters(node.key);
                if (node.desiredRevision > node.producedRevision) QueueNode(node);
            }
            break;
        case ArtifactBuildResult::Outcome::NeedsDependencies: {
            if (completion.revision != node.desiredRevision) {
                QueueNode(node);
                break;
            }
            node.terminalFailure = false;
            RemoveWaiterEdges(node);
            node.requirements = std::move(result.requirements);
            node.checkpoint = std::move(result.checkpoint);
            node.retryAt.reset();
            InstallWaiterEdges(node);
            if (const auto cycle = DetectCycle(node); !cycle.empty()) {
                FailCycle(cycle);
            } else {
                QueueNode(node);
            }
            break;
        }
        case ArtifactBuildResult::Outcome::RetryAfter:
            if (completion.revision != node.desiredRevision) {
                QueueNode(node);
                break;
            }
            node.terminalFailure = false;
            node.checkpoint = std::move(result.checkpoint);
            node.retryAt = std::chrono::steady_clock::now() + result.retryDelay;
            SetState(node, ArtifactReadiness::Blocked);
            ++stats.retries;
            break;
        case ArtifactBuildResult::Outcome::Cancelled:
            SetState(node, ArtifactReadiness::Cancelled);
            ++stats.cancelled;
            ready.push_back(MakeSnapshot(node));
            break;
        case ArtifactBuildResult::Outcome::Failed:
            node.error = std::move(result.error);
            node.terminalFailure = true;
            SetState(node, ArtifactReadiness::Failed);
            ++stats.failed;
            ready.push_back(MakeSnapshot(node));
            break;
        }
    }

    void Drain() {
        struct PendingGpuSignal {
            ArtifactKey key;
            std::uint64_t generation = 0;
            std::shared_ptr<const GpuSubmissionSet> token;
        };
        constexpr std::size_t maxTransitions = 128;
        constexpr auto maxDuration = std::chrono::milliseconds(2);
        const auto started = std::chrono::steady_clock::now();
        std::size_t transitions = 0;
        std::vector<PendingGpuSignal> signalledGpu;
        {
            std::lock_guard lock(mutex);
            ArtifactKey key;
            while (gpuSignals.try_pop(key) && transitions++ < maxTransitions &&
                std::chrono::steady_clock::now() - started < maxDuration) {
                const auto found = nodes.find(key);
                if (found != nodes.end() && found->second.state == ArtifactReadiness::UploadSubmitted &&
                    found->second.waitingGpuSubmissions) {
                    signalledGpu.push_back({ key, found->second.generation,
                        found->second.waitingGpuSubmissions });
                }
            }
            // Completion callbacks are the primary wakeup path. Keep a bounded,
            // rotating recovery cursor as a safety net for callbacks lost across
            // upload-service shutdown/replacement or an unlucky subscription race.
            constexpr std::size_t maxGpuRecoveryChecks = 32;
            const auto recoveryChecks = (std::min)(maxGpuRecoveryChecks, gpuRecovery.size());
            for (std::size_t index = 0; index < recoveryChecks; ++index) {
                const auto recoveryKey = gpuRecovery.front();
                gpuRecovery.pop_front();
                const auto found = nodes.find(recoveryKey);
                if (found == nodes.end() || found->second.state != ArtifactReadiness::UploadSubmitted ||
                    !found->second.waitingGpuSubmissions) continue;
                signalledGpu.push_back({ recoveryKey, found->second.generation,
                    found->second.waitingGpuSubmissions });
                if (found->second.gpuRecoveryChecksRemaining != 0 &&
                    --found->second.gpuRecoveryChecksRemaining != 0) {
                    gpuRecovery.push_back(recoveryKey);
                }
            }
        }
        // Complete() may synchronously notify ticket subscribers. Never call
        // it while holding the graph mutex; subscribers are allowed to queue
        // another completion signal immediately.
        std::vector<PendingGpuSignal> completedGpu;
        completedGpu.reserve(signalledGpu.size());
        for (auto& signal : signalledGpu) {
            if (signal.token && signal.token->Complete()) completedGpu.push_back(std::move(signal));
        }
        std::vector<std::pair<ArtifactProducerRegistration, ArtifactBuildContext>> builds;
        std::vector<ArtifactSnapshot> ready;
        std::vector<std::function<void(const ArtifactSnapshot&)>> callbacks;
        std::optional<std::chrono::steady_clock::duration> retryDelay;
        bool hasImmediateWork = false;
        {
            std::lock_guard lock(mutex);
            const auto now = std::chrono::steady_clock::now();
            for (const auto& signal : completedGpu) {
                const auto found = nodes.find(signal.key);
                if (found == nodes.end()) continue;
                auto& node = found->second;
                if (node.generation != signal.generation ||
                    node.state != ArtifactReadiness::UploadSubmitted ||
                    node.waitingGpuSubmissions != signal.token) continue;
                if (node.uploadSubmittedAt != std::chrono::steady_clock::time_point{}) {
                    const auto gpuWait = now - node.uploadSubmittedAt;
                    stats.gpuWaitMicros += static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(gpuWait).count());
                    basic_telemetry::Record("SARP.AsyncStateGraph.GpuLatencyNs",
                        static_cast<std::uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(gpuWait).count()));
                }
                node.waitingGpuSubmissions.reset();
                SetState(node, node.published ? ArtifactReadiness::Published
                                             : ArtifactReadiness::GpuReady);
                ready.push_back(MakeSnapshot(node));
                WakeWaiters(node.key);
                if (node.desiredRevision > node.producedRevision) QueueNode(node);
                if (stats.gpuWaiting) --stats.gpuWaiting;
            }
            for (auto& [_, node] : nodes) {
                if (!node.retryAt) continue;
                if (*node.retryAt <= now) {
                    node.retryAt.reset();
                    QueueNode(node);
                } else {
                    const auto remaining = *node.retryAt - now;
                    retryDelay = retryDelay ? (std::min)(*retryDelay, remaining) : remaining;
                }
            }
            while (!completions.empty() && transitions++ < maxTransitions &&
                std::chrono::steady_clock::now() - started < maxDuration) {
                auto completion = std::move(completions.front());
                completions.pop_front();
                const auto completedKey = completion.key;
                ApplyCompletion(std::move(completion), ready);
                const auto completed = nodes.find(completedKey);
                if (completed != nodes.end() && !completed->second.desired &&
                    !completed->second.buildInFlight) {
                    const auto dependents = waiters.find(completedKey);
                    if (dependents == waiters.end() || dependents->second.empty()) {
                        nodes.erase(completed);
                    }
                }
            }
            while (!pending.empty() && transitions++ < maxTransitions &&
                std::chrono::steady_clock::now() - started < maxDuration) {
                const auto key = pending.front();
                pending.pop_front();
                auto found = nodes.find(key);
                if (found == nodes.end()) continue;
                auto& node = found->second;
                if (node.state != ArtifactReadiness::Queued || node.buildInFlight) continue;
                // A newer request may replace a queued node's dependency set before
                // this queue entry is consumed. Revalidate the current closure here;
                // QueueNode's earlier check only applies to the request that queued it.
                if (!DependenciesSatisfied(node)) {
                    SetState(node, ArtifactReadiness::Blocked);
                    continue;
                }
                const auto producer = producers.find(node.key.kind);
                if (producer == producers.end() || !producer->second.producer) {
                    node.error = "no producer registered";
                    SetState(node, ArtifactReadiness::Failed);
                    ++stats.failed;
                    ready.push_back(MakeSnapshot(node));
                    continue;
                }
                node.buildInFlight = true;
                node.buildStartedAt = std::chrono::steady_clock::now();
                if (node.queuedAt != std::chrono::steady_clock::time_point{}) {
                    const auto queueWait = node.buildStartedAt - node.queuedAt;
                    stats.queueWaitMicros += static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(queueWait).count());
                    basic_telemetry::Record("SARP.AsyncStateGraph.QueueLatencyNs",
                        static_cast<std::uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(queueWait).count()));
                }
                SetState(node, ArtifactReadiness::Preparing);
                ++stats.buildsStarted;
                ArtifactBuildContext context{ node.key, node.desiredRevision, node.generation,
                    DependencySnapshots(node), node.input, node.checkpoint, {} };
                builds.emplace_back(producer->second, std::move(context));
            }
            callbacks.reserve(readyCallbacks.size());
            for (const auto& [_, callback] : readyCallbacks) callbacks.push_back(callback);
            hasImmediateWork = !pending.empty() || !completions.empty() || !gpuSignals.empty();
            const auto afterWork = std::chrono::steady_clock::now();
            if (!gpuRecovery.empty()) {
                const auto recoveryDelay = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::milliseconds(2));
                retryDelay = retryDelay ? (std::min)(*retryDelay, recoveryDelay) : recoveryDelay;
            }
            for (const auto& [_, node] : nodes) {
                if (!node.retryAt) continue;
                const auto remaining = *node.retryAt > afterWork
                    ? *node.retryAt - afterWork
                    : std::chrono::steady_clock::duration::zero();
                retryDelay = retryDelay ? (std::min)(*retryDelay, remaining) : remaining;
            }
            if (basic_telemetry::Enabled()) {
                std::array<std::uint64_t, static_cast<std::size_t>(ArtifactKind::FrameManifest) + 1u>
                    kindCounts{};
                std::uint64_t maxBlockerAgeMicros = 0;
                std::uint64_t maxFanout = 0;
                for (const auto& [nodeKey, node] : nodes) {
                    const auto kindIndex = static_cast<std::size_t>(nodeKey.kind);
                    if (kindIndex < kindCounts.size()) ++kindCounts[kindIndex];
                    if (node.state == ArtifactReadiness::Blocked) {
                        maxBlockerAgeMicros = (std::max)(maxBlockerAgeMicros,
                            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                afterWork - node.stateSince).count()));
                    }
                    if (const auto fanout = waiters.find(nodeKey); fanout != waiters.end()) {
                        maxFanout = (std::max)(maxFanout,
                            static_cast<std::uint64_t>(fanout->second.size()));
                    }
                }
                basic_telemetry::SetGauge("SARP.AsyncStateGraph.Nodes",
                    static_cast<std::int64_t>(nodes.size()));
                basic_telemetry::SetGauge("SARP.AsyncStateGraph.Pending",
                    static_cast<std::int64_t>(pending.size()));
                basic_telemetry::SetGauge("SARP.AsyncStateGraph.GpuWaiting",
                    static_cast<std::int64_t>(stats.gpuWaiting));
                basic_telemetry::SetGauge("SARP.AsyncStateGraph.BlockerAgeMicros",
                    static_cast<std::int64_t>(maxBlockerAgeMicros));
                basic_telemetry::SetGauge("SARP.AsyncStateGraph.Fanout",
                    static_cast<std::int64_t>(maxFanout));
                for (std::size_t index = 0; index < kindCounts.size(); ++index) {
                    basic_telemetry::SetGauge(
                        std::format("SARP.AsyncStateGraph.Kind.{}.Nodes", index),
                        static_cast<std::int64_t>(kindCounts[index]));
                }
            }
        }
        // Publish the idle state before the final signal check. A GPU callback
        // may have observed drainScheduled=true after hasImmediateWork was
        // sampled above. Rechecking the lock-free queue after clearing the flag
        // closes that handoff race: either this drain sees the signal, or the
        // callback sees false and schedules the successor drain itself.
        drainScheduled.store(false, std::memory_order_release);
        hasImmediateWork = hasImmediateWork || !gpuSignals.empty();
        for (auto& [registration, context] : builds) SubmitBuild(registration, std::move(context));
        for (const auto& callback : callbacks) {
            if (callback) for (const auto& snapshot : ready) callback(snapshot);
        }
        if (hasImmediateWork) ScheduleDrain();
        else if (retryDelay) ScheduleDrain((std::max)(*retryDelay,
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(1))));
    }
};

AsyncStateGraph::AsyncStateGraph(TaskSchedulerManager& scheduler, std::string_view name)
    : m_impl(std::make_shared<Impl>(scheduler, name)) {}

AsyncStateGraph::~AsyncStateGraph() { Shutdown(); }

void AsyncStateGraph::RegisterProducer(ArtifactKind kind, ArtifactProducerRegistration registration) {
    std::lock_guard lock(m_impl->mutex);
    m_impl->producers[kind] = std::move(registration);
}

ArtifactRequestResult AsyncStateGraph::Request(ArtifactKey key, std::uint64_t desiredRevision,
    std::vector<ArtifactRequirement> requirements, ArtifactPayload input,
    std::uint64_t requestFingerprint) {
    if (m_impl->shuttingDown.load(std::memory_order_acquire)) {
        return { ArtifactRequestStatus::ShuttingDown, 0 };
    }
    ArtifactRequestResult result;
    {
        std::lock_guard lock(m_impl->mutex);
        const auto producer = m_impl->producers.find(key.kind);
        if (producer != m_impl->producers.end() &&
            producer->second.inputType != std::type_index(typeid(void)) &&
            (!input.Valid() || input.Type() != producer->second.inputType)) {
            return { ArtifactRequestStatus::TypeMismatch, 0 };
        }
        if (input.Valid() && requestFingerprint == 0) {
            return { ArtifactRequestStatus::MissingFingerprint, 0 };
        }
        if (requestFingerprint == 0) {
            requestFingerprint = CanonicalRequestFingerprint(key, desiredRevision, requirements);
        }
        auto& node = m_impl->nodes[key];
        node.key = key;
        node.desired = true;
        node.terminalFailure = false;
        if (desiredRevision < node.desiredRevision) {
            return { ArtifactRequestStatus::StaleRevision, node.generation };
        }
        if (desiredRevision == node.desiredRevision && node.state != ArtifactReadiness::Missing &&
            node.state != ArtifactReadiness::Failed && node.state != ArtifactReadiness::Cancelled) {
            if (requestFingerprint != node.requestFingerprint ||
                requirements != node.requirements || input.Type() != node.input.Type()) {
                return { ArtifactRequestStatus::ConflictingRevision, node.generation };
            }
            return { ArtifactRequestStatus::AlreadyDesired, node.generation };
        }
        m_impl->RemoveWaiterEdges(node);
        node.desiredRevision = desiredRevision;
        node.requestFingerprint = requestFingerprint;
        node.requirements = std::move(requirements);
        node.input = std::move(input);
        node.error.clear();
        // A newer desired revision does not cancel the executing CPU/GPU build.
        // Its completion remains a valid immutable intermediate version, and the
        // successor is queued after that execution slot is ingested.
        if (!node.buildInFlight && node.state != ArtifactReadiness::UploadSubmitted) {
            node.gpuSubmissions.reset();
        }
        node.retryAt.reset();
        m_impl->InstallWaiterEdges(node);
        ++m_impl->stats.requests;
        const auto cycle = m_impl->DetectCycle(node);
        if (!cycle.empty()) {
            m_impl->FailCycle(cycle);
        } else {
            std::unordered_set<ArtifactKey, ArtifactKey::Hasher> visited;
            m_impl->SupersedeDependents(key, visited);
            m_impl->QueueNode(node);
        }
        result = { ArtifactRequestStatus::Accepted, node.generation };
    }
    m_impl->ScheduleDrain();
    return result;
}

ArtifactRequestResult AsyncStateGraph::RequestExpressions(ArtifactKey key, std::uint64_t desiredRevision,
    std::vector<DependencyExpression> dependencies, ArtifactPayload input,
    std::uint64_t requestFingerprint) {
    std::vector<ArtifactRequirement> requirements;
    std::uint32_t nextAlternativeGroup = 1;
    for (const auto& expression : dependencies) {
        std::visit([&](const auto& value) {
            using Expression = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Expression, Require>) {
                requirements.push_back({ value.requirement.key, value.requirement.minimumRevision,
                    value.requirement.requiredReadiness, DependencyPolicy::AllOf, 0 });
            } else if constexpr (std::is_same_v<Expression, Optional>) {
                requirements.push_back({ value.requirement.key, value.requirement.minimumRevision,
                    value.requirement.requiredReadiness, DependencyPolicy::Optional, 0 });
            } else {
                const auto policy = std::is_same_v<Expression, FirstReady>
                    ? DependencyPolicy::FallbackAllowed : DependencyPolicy::AnyOf;
                const auto group = nextAlternativeGroup++;
                for (const auto& alternative : value.alternatives) {
                    requirements.push_back({ alternative.key, alternative.minimumRevision,
                        alternative.requiredReadiness, policy, group });
                }
            }
        }, expression);
    }
    return Request(key, desiredRevision, std::move(requirements), std::move(input),
        requestFingerprint);
}

bool AsyncStateGraph::Invalidate(ArtifactKey key, std::uint64_t desiredRevision) {
    std::vector<ArtifactRequirement> requirements;
    ArtifactPayload input;
    bool foundNode = false;
    std::uint64_t requestFingerprint = 0;
    {
        std::lock_guard lock(m_impl->mutex);
        const auto found = m_impl->nodes.find(key);
        foundNode = found != m_impl->nodes.end();
        if (foundNode) {
            requirements = found->second.requirements;
            input = found->second.input;
            requestFingerprint = found->second.requestFingerprint;
        }
        ++m_impl->stats.invalidations;
    }
    return static_cast<bool>(Request(key, desiredRevision,
        foundNode ? std::move(requirements) : std::vector<ArtifactRequirement>{}, std::move(input),
        requestFingerprint));
}

void AsyncStateGraph::Cancel(ArtifactKey key) {
    std::lock_guard lock(m_impl->mutex);
    const auto found = m_impl->nodes.find(key);
    if (found == m_impl->nodes.end()) return;
    ++found->second.generation;
    found->second.desired = false;
    found->second.retryAt.reset();
    if (found->second.state == ArtifactReadiness::UploadSubmitted &&
        found->second.waitingGpuSubmissions &&
        m_impl->stats.gpuWaiting) --m_impl->stats.gpuWaiting;
    if (found->second.waitingGpuSubmissions) (void)found->second.waitingGpuSubmissions->Cancel();
    found->second.waitingGpuSubmissions.reset();
    found->second.gpuSubmissions.reset();
    m_impl->SetState(found->second, ArtifactReadiness::Cancelled);
    ++m_impl->stats.cancelled;
}

void AsyncStateGraph::Release(ArtifactKey key) {
    std::lock_guard lock(m_impl->mutex);
    const auto found = m_impl->nodes.find(key);
    if (found == m_impl->nodes.end()) return;
    auto& node = found->second;
    m_impl->RemoveWaiterEdges(node);
    ++node.generation;
    node.desired = false;
    node.retryAt.reset();
    if (node.state == ArtifactReadiness::UploadSubmitted && node.waitingGpuSubmissions &&
        m_impl->stats.gpuWaiting) --m_impl->stats.gpuWaiting;
    if (node.waitingGpuSubmissions) (void)node.waitingGpuSubmissions->Cancel();
    node.waitingGpuSubmissions.reset();
    node.gpuSubmissions.reset();
    m_impl->SetState(node, ArtifactReadiness::Cancelled);
    ++m_impl->stats.cancelled;

    const auto dependents = m_impl->waiters.find(key);
    if (!node.buildInFlight && (dependents == m_impl->waiters.end() || dependents->second.empty())) {
        m_impl->nodes.erase(found);
    }
}

void AsyncStateGraph::MarkPublished(ArtifactKey key, std::uint64_t revision) {
    std::lock_guard lock(m_impl->mutex);
    const auto found = m_impl->nodes.find(key);
    if (found != m_impl->nodes.end() && found->second.producedRevision == revision &&
        (found->second.state == ArtifactReadiness::UploadSubmitted ||
         found->second.state == ArtifactReadiness::GpuReady ||
         found->second.state == ArtifactReadiness::Published)) {
        found->second.published = true;
        if (found->second.state == ArtifactReadiness::GpuReady) {
            m_impl->SetState(found->second, ArtifactReadiness::Published);
        }
        m_impl->WakeWaiters(key);
    }
}

void AsyncStateGraph::PumpGpuCompletions() {
    struct PendingCompletion {
        ArtifactKey key;
        std::uint64_t generation = 0;
        std::shared_ptr<const GpuSubmissionSet> token;
    };
    std::vector<PendingCompletion> pending;
    {
        std::lock_guard lock(m_impl->mutex);
        pending.reserve(m_impl->nodes.size());
        for (const auto& [key, node] : m_impl->nodes) {
            if (node.state == ArtifactReadiness::UploadSubmitted && node.waitingGpuSubmissions) {
                pending.push_back({ key, node.generation, node.waitingGpuSubmissions });
            }
        }
    }
    std::vector<PendingCompletion> completed;
    completed.reserve(pending.size());
    for (auto& item : pending) {
        if (item.token && item.token->Complete()) completed.push_back(std::move(item));
    }

    std::vector<ArtifactSnapshot> ready;
    std::vector<std::function<void(const ArtifactSnapshot&)>> callbacks;
    {
        std::lock_guard lock(m_impl->mutex);
        for (const auto& item : completed) {
            const auto found = m_impl->nodes.find(item.key);
            if (found == m_impl->nodes.end()) continue;
            auto& node = found->second;
            if (node.generation != item.generation ||
                node.state != ArtifactReadiness::UploadSubmitted ||
                node.waitingGpuSubmissions != item.token) continue;
            if (node.uploadSubmittedAt != std::chrono::steady_clock::time_point{}) {
                m_impl->stats.gpuWaitMicros += static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - node.uploadSubmittedAt).count());
            }
            node.waitingGpuSubmissions.reset();
            m_impl->SetState(node, node.published ? ArtifactReadiness::Published
                                                 : ArtifactReadiness::GpuReady);
            if (m_impl->stats.gpuWaiting) --m_impl->stats.gpuWaiting;
            ready.push_back(m_impl->MakeSnapshot(node));
            m_impl->WakeWaiters(node.key);
            if (node.desiredRevision > node.producedRevision) m_impl->QueueNode(node);
        }
        m_impl->stats.gpuWaiting = 0;
        for (const auto& [_, node] : m_impl->nodes) {
            if (node.state == ArtifactReadiness::UploadSubmitted && node.waitingGpuSubmissions) {
                ++m_impl->stats.gpuWaiting;
            }
        }
        callbacks.reserve(m_impl->readyCallbacks.size());
        for (const auto& [_, callback] : m_impl->readyCallbacks) callbacks.push_back(callback);
    }
    for (const auto& callback : callbacks) {
        if (callback) for (const auto& snapshot : ready) callback(snapshot);
    }
    m_impl->ScheduleDrain();
}

void AsyncStateGraph::SetReadyCallback(std::function<void(const ArtifactSnapshot&)> callback) {
    std::lock_guard lock(m_impl->mutex);
    if (callback) m_impl->readyCallbacks.insert_or_assign(0, std::move(callback));
    else m_impl->readyCallbacks.erase(0);
}

std::uint64_t AsyncStateGraph::AddReadyCallback(
    std::function<void(const ArtifactSnapshot&)> callback) {
    if (!callback) return 0;
    std::lock_guard lock(m_impl->mutex);
    const auto subscription = ++m_impl->nextReadyCallback;
    m_impl->readyCallbacks.emplace(subscription, std::move(callback));
    return subscription;
}

void AsyncStateGraph::RemoveReadyCallback(std::uint64_t subscription) {
    if (subscription == 0) return;
    std::lock_guard lock(m_impl->mutex);
    m_impl->readyCallbacks.erase(subscription);
}

ArtifactSnapshot AsyncStateGraph::Snapshot(ArtifactKey key) const {
    std::lock_guard lock(m_impl->mutex);
    const auto found = m_impl->nodes.find(key);
    return found == m_impl->nodes.end() ? ArtifactSnapshot{ key } : m_impl->MakeSnapshot(found->second);
}

ArtifactDiagnostic AsyncStateGraph::Diagnose(ArtifactKey key) const {
    std::lock_guard lock(m_impl->mutex);
    ArtifactDiagnostic result;
    const auto found = m_impl->nodes.find(key);
    if (found == m_impl->nodes.end()) { result.artifact.key = key; return result; }
    const auto& node = found->second;
    result.artifact = m_impl->MakeSnapshot(node);
    result.desiredRevision = node.desiredRevision;
    result.error = node.error;
    result.stateAge = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - node.stateSince);
    for (const auto& requirement : node.requirements) {
        if (!m_impl->RequirementSatisfied(requirement) && requirement.policy != DependencyPolicy::Optional)
            result.blockers.push_back(requirement);
    }
    std::unordered_set<ArtifactKey, ArtifactKey::Hasher> visited;
    m_impl->AppendBlockerChain(key, visited, result.blockerChain);
    return result;
}

AsyncStateGraphStats AsyncStateGraph::Stats() const {
    std::lock_guard lock(m_impl->mutex);
    auto result = m_impl->stats;
    result.stateCounts.fill(0);
    for (const auto& [_, node] : m_impl->nodes) {
        const auto index = static_cast<std::size_t>(node.state);
        if (index < result.stateCounts.size()) ++result.stateCounts[index];
    }
    return result;
}

std::uint64_t AsyncStateGraph::Outstanding(ArtifactKind kind) const {
    std::lock_guard lock(m_impl->mutex);
    return static_cast<std::uint64_t>(std::ranges::count_if(
        m_impl->nodes, [&](const auto& entry) {
            const auto& node = entry.second;
            if (node.key.kind != kind || !node.desired) return false;
            return node.state != ArtifactReadiness::GpuReady &&
                node.state != ArtifactReadiness::Published &&
                node.state != ArtifactReadiness::Superseded &&
                node.state != ArtifactReadiness::Cancelled &&
                node.state != ArtifactReadiness::Failed;
        }));
}

void AsyncStateGraph::WaitIdle() const { if (m_impl) m_impl->scope.Wait(); }

void AsyncStateGraph::Shutdown() {
    if (!m_impl || m_impl->shuttingDown.exchange(true)) return;
    // Waiting is mandatory, but a task failure already captured at the
    // scheduler boundary must not unwind renderer teardown.
    try {
        m_impl->scope.CancelAndWait();
    } catch (const std::exception& exception) {
        spdlog::error("AsyncStateGraph shutdown observed task failure: {}", exception.what());
    } catch (...) {
        spdlog::error("AsyncStateGraph shutdown observed unknown task failure");
    }
    std::lock_guard lock(m_impl->mutex);
    for (auto& [_, node] : m_impl->nodes) {
        if (node.waitingGpuSubmissions) (void)node.waitingGpuSubmissions->Cancel();
    }
    for (auto& completion : m_impl->completions) {
        if (completion.result.gpuSubmissions) (void)completion.result.gpuSubmissions->Cancel();
    }
    m_impl->pending.clear();
    m_impl->completions.clear();
    ArtifactKey discardedSignal;
    while (m_impl->gpuSignals.try_pop(discardedSignal)) {}
    m_impl->readyCallbacks.clear();
}

} // namespace br::render
