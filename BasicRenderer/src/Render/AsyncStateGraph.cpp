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

} // namespace

std::size_t ArtifactKey::Hasher::operator()(const ArtifactKey& key) const noexcept {
    auto value = static_cast<std::size_t>(key.kind);
    value ^= std::hash<std::uint64_t>{}(key.primaryID) + 0x9e3779b9u + (value << 6u) + (value >> 2u);
    value ^= std::hash<std::uint64_t>{}(key.variantID) + 0x9e3779b9u + (value << 6u) + (value >> 2u);
    return value;
}

ArtifactBuildResult ArtifactBuildResult::Ready(ArtifactPayload payload,
    std::shared_ptr<const GpuDependencyToken> gpuDependency) {
    ArtifactBuildResult result;
    result.outcome = Outcome::Ready;
    result.payload = std::move(payload);
    result.gpuDependency = std::move(gpuDependency);
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

std::shared_ptr<const GpuDependencyToken> MakeGpuDependencyToken(
    const std::shared_ptr<org::TrackedUploadTicket>& ticket) {
    if (!ticket) return {};
    auto token = std::make_shared<GpuDependencyToken>();
    {
        std::lock_guard lock(ticket->timelineMutex);
        token->timelineOwner = ticket->timelineOwner;
        token->value = ticket->timelineValue;
    }
    token->isComplete = [ticket] { return ticket->Complete(); };
    token->currentTimelineOwner = [ticket] {
        std::lock_guard lock(ticket->timelineMutex);
        return ticket->timelineOwner;
    };
    token->currentValue = [ticket] {
        std::lock_guard lock(ticket->timelineMutex);
        return ticket->timelineValue;
    };
    token->subscribe = [ticket](std::function<void()> callback) {
        ticket->SetChangeCallback(std::move(callback));
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
        std::shared_ptr<const GpuDependencyToken> gpuDependency;
        std::optional<std::chrono::steady_clock::time_point> retryAt;
        std::string error;
        std::chrono::steady_clock::time_point stateSince = std::chrono::steady_clock::now();
        bool buildInFlight = false;
        std::chrono::steady_clock::time_point queuedAt{};
        std::chrono::steady_clock::time_point buildStartedAt{};
        std::chrono::steady_clock::time_point uploadSubmittedAt{};
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
    std::function<void(const ArtifactSnapshot&)> readyCallback;
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
        return { node.key, node.producedRevision, node.generation, node.state, node.payload };
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
                if (requirement.policy == DependencyPolicy::Optional) continue;
                if (FindPath(requirement.key, target, visited, path)) return true;
            }
        }
        path.pop_back();
        return false;
    }

    std::vector<ArtifactKey> DetectCycle(const Node& node) const {
        for (const auto& requirement : node.requirements) {
            if (requirement.policy == DependencyPolicy::Optional) continue;
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
            ++found->second.generation;
            found->second.buildInFlight = false;
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
        if (node.state == ArtifactReadiness::UploadSubmitted && node.gpuDependency) {
            output += std::format(" gpu-value={}", node.gpuDependency->TimelineValue());
			const auto detail = node.gpuDependency->Describe();
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
        if (node.buildInFlight || node.state == ArtifactReadiness::Queued ||
            node.state == ArtifactReadiness::UploadSubmitted) return;
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
            if (auto node = nodes.find(waiter); node != nodes.end()) QueueNode(node->second);
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
            if (node.gpuDependency) {
                (void)node.gpuDependency->Cancel();
                if (node.state == ArtifactReadiness::UploadSubmitted && stats.gpuWaiting) {
                    --stats.gpuWaiting;
                }
                node.gpuDependency.reset();
            }
            SetState(node, ArtifactReadiness::Superseded);
            if (!node.buildInFlight) QueueNode(node);
            SupersedeDependents(dependentKey, visited);
        }
    }

    bool DependenciesStillMatch(const Completion& completion) const {
        for (const auto& dependency : completion.dependencies) {
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
        if (node.generation != completion.generation || node.desiredRevision != completion.revision ||
            !DependenciesStillMatch(completion)) {
            ++stats.staleCompletions;
            node.buildInFlight = false;
            QueueNode(node);
            return;
        }
        node.buildInFlight = false;
        ++stats.buildsCompleted;
        if (node.buildStartedAt != std::chrono::steady_clock::time_point{}) {
            stats.buildMicros += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - node.buildStartedAt).count());
        }
        auto& result = completion.result;
        switch (result.outcome) {
        case ArtifactBuildResult::Outcome::Ready:
            node.payload = std::move(result.payload);
            node.resolvedDependencies = completion.dependencies;
            node.checkpoint = {};
            node.retryAt.reset();
            node.producedRevision = node.desiredRevision;
            node.gpuDependency = std::move(result.gpuDependency);
            if (node.gpuDependency && !node.gpuDependency->Complete()) {
                SetState(node, ArtifactReadiness::UploadSubmitted);
                node.uploadSubmittedAt = std::chrono::steady_clock::now();
                ++stats.gpuWaiting;
                if (node.gpuDependency->subscribe) {
                    auto weak = weak_from_this();
                    const auto key = node.key;
                    node.gpuDependency->subscribe([weak, key] {
                        if (auto self = weak.lock()) {
                            {
                                self->gpuSignals.push(key);
                            }
                            self->ScheduleDrain();
                        }
                    });
                }
            } else {
                node.gpuDependency.reset();
                SetState(node, ArtifactReadiness::GpuReady);
                ready.push_back(MakeSnapshot(node));
                WakeWaiters(node.key);
            }
            break;
        case ArtifactBuildResult::Outcome::NeedsDependencies: {
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
            node.checkpoint = std::move(result.checkpoint);
            node.retryAt = std::chrono::steady_clock::now() + result.retryDelay;
            SetState(node, ArtifactReadiness::Blocked);
            ++stats.retries;
            break;
        case ArtifactBuildResult::Outcome::Cancelled:
            SetState(node, ArtifactReadiness::Cancelled);
            ++stats.cancelled;
            break;
        case ArtifactBuildResult::Outcome::Failed:
            node.error = std::move(result.error);
            SetState(node, ArtifactReadiness::Failed);
            ++stats.failed;
            break;
        }
    }

    void Drain() {
        struct PendingGpuSignal {
            ArtifactKey key;
            std::uint64_t generation = 0;
            std::shared_ptr<const GpuDependencyToken> token;
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
                    found->second.gpuDependency) {
                    signalledGpu.push_back({ key, found->second.generation, found->second.gpuDependency });
                }
            }
        }
        // Complete() may synchronously notify ticket subscribers. Never call
        // it while holding the graph mutex because the subscriber queues a
        // graph signal and must acquire that same mutex.
        std::vector<PendingGpuSignal> completedGpu;
        completedGpu.reserve(signalledGpu.size());
        for (auto& signal : signalledGpu) {
            if (signal.token && signal.token->Complete()) completedGpu.push_back(std::move(signal));
        }
        std::vector<std::pair<ArtifactProducerRegistration, ArtifactBuildContext>> builds;
        std::vector<ArtifactSnapshot> ready;
        std::function<void(const ArtifactSnapshot&)> callback;
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
                    node.gpuDependency != signal.token) continue;
                if (node.uploadSubmittedAt != std::chrono::steady_clock::time_point{}) {
                    stats.gpuWaitMicros += static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            now - node.uploadSubmittedAt).count());
                }
                node.gpuDependency.reset();
                SetState(node, ArtifactReadiness::GpuReady);
                ready.push_back(MakeSnapshot(node));
                WakeWaiters(node.key);
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
                ApplyCompletion(std::move(completion), ready);
            }
            while (!pending.empty() && transitions++ < maxTransitions &&
                std::chrono::steady_clock::now() - started < maxDuration) {
                const auto key = pending.front();
                pending.pop_front();
                auto found = nodes.find(key);
                if (found == nodes.end()) continue;
                auto& node = found->second;
                if (node.state != ArtifactReadiness::Queued || node.buildInFlight) continue;
                const auto producer = producers.find(node.key.kind);
                if (producer == producers.end() || !producer->second.producer) {
                    node.error = "no producer registered";
                    SetState(node, ArtifactReadiness::Failed);
                    ++stats.failed;
                    continue;
                }
                node.buildInFlight = true;
                node.buildStartedAt = std::chrono::steady_clock::now();
                if (node.queuedAt != std::chrono::steady_clock::time_point{}) {
                    stats.queueWaitMicros += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                        node.buildStartedAt - node.queuedAt).count());
                }
                SetState(node, ArtifactReadiness::Preparing);
                ++stats.buildsStarted;
                ArtifactBuildContext context{ node.key, node.desiredRevision, node.generation,
                    DependencySnapshots(node), node.input, node.checkpoint, {} };
                builds.emplace_back(producer->second, std::move(context));
            }
            callback = readyCallback;
            hasImmediateWork = !pending.empty() || !completions.empty() || !gpuSignals.empty();
            const auto afterWork = std::chrono::steady_clock::now();
            for (const auto& [_, node] : nodes) {
                if (!node.retryAt) continue;
                const auto remaining = *node.retryAt > afterWork
                    ? *node.retryAt - afterWork
                    : std::chrono::steady_clock::duration::zero();
                retryDelay = retryDelay ? (std::min)(*retryDelay, remaining) : remaining;
            }
            drainScheduled.store(false, std::memory_order_release);
        }
        for (auto& [registration, context] : builds) SubmitBuild(registration, std::move(context));
        if (callback) for (const auto& snapshot : ready) callback(snapshot);
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
        auto& node = m_impl->nodes[key];
        node.key = key;
        if (desiredRevision < node.desiredRevision) {
            return { ArtifactRequestStatus::StaleRevision, node.generation };
        }
        if (desiredRevision == node.desiredRevision && node.state != ArtifactReadiness::Missing &&
            node.state != ArtifactReadiness::Failed && node.state != ArtifactReadiness::Cancelled) {
            if (requestFingerprint != 0 && node.requestFingerprint != 0 &&
                requestFingerprint != node.requestFingerprint) {
                return { ArtifactRequestStatus::ConflictingRevision, node.generation };
            }
            return { ArtifactRequestStatus::AlreadyDesired, node.generation };
        }
        m_impl->RemoveWaiterEdges(node);
        node.desiredRevision = desiredRevision;
        ++node.generation;
        node.requestFingerprint = requestFingerprint;
        node.requirements = std::move(requirements);
        node.input = std::move(input);
        node.error.clear();
        if (node.state == ArtifactReadiness::UploadSubmitted) {
			if (node.gpuDependency && m_impl->stats.gpuWaiting) --m_impl->stats.gpuWaiting;
			// The old upload may still complete, but this node now represents a newer
			// generation.  Leaving it in UploadSubmitted after dropping the token
			// makes QueueNode reject the successor forever.
			m_impl->SetState(node, ArtifactReadiness::Superseded);
		}
        if (node.gpuDependency) (void)node.gpuDependency->Cancel();
        node.gpuDependency.reset();
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

bool AsyncStateGraph::Invalidate(ArtifactKey key, std::uint64_t desiredRevision) {
    std::vector<ArtifactRequirement> requirements;
    ArtifactPayload input;
    bool foundNode = false;
    {
        std::lock_guard lock(m_impl->mutex);
        const auto found = m_impl->nodes.find(key);
        foundNode = found != m_impl->nodes.end();
        if (foundNode) {
            requirements = found->second.requirements;
            input = found->second.input;
        }
        ++m_impl->stats.invalidations;
    }
    return static_cast<bool>(Request(key, desiredRevision,
        foundNode ? std::move(requirements) : std::vector<ArtifactRequirement>{}, std::move(input)));
}

void AsyncStateGraph::Cancel(ArtifactKey key) {
    std::lock_guard lock(m_impl->mutex);
    const auto found = m_impl->nodes.find(key);
    if (found == m_impl->nodes.end()) return;
    ++found->second.generation;
    found->second.buildInFlight = false;
    found->second.retryAt.reset();
    if (found->second.state == ArtifactReadiness::UploadSubmitted && found->second.gpuDependency &&
        m_impl->stats.gpuWaiting) --m_impl->stats.gpuWaiting;
    if (found->second.gpuDependency) (void)found->second.gpuDependency->Cancel();
    found->second.gpuDependency.reset();
    m_impl->SetState(found->second, ArtifactReadiness::Cancelled);
    ++m_impl->stats.cancelled;
}

void AsyncStateGraph::MarkPublished(ArtifactKey key, std::uint64_t revision) {
    std::lock_guard lock(m_impl->mutex);
    const auto found = m_impl->nodes.find(key);
    if (found != m_impl->nodes.end() && found->second.producedRevision == revision &&
        found->second.state == ArtifactReadiness::GpuReady) {
        m_impl->SetState(found->second, ArtifactReadiness::Published);
        m_impl->WakeWaiters(key);
    }
}

void AsyncStateGraph::PumpGpuCompletions() {
    struct PendingCompletion {
        ArtifactKey key;
        std::uint64_t generation = 0;
        std::shared_ptr<const GpuDependencyToken> token;
    };
    std::vector<PendingCompletion> pending;
    {
        std::lock_guard lock(m_impl->mutex);
        pending.reserve(m_impl->nodes.size());
        for (const auto& [key, node] : m_impl->nodes) {
            if (node.state == ArtifactReadiness::UploadSubmitted && node.gpuDependency) {
                pending.push_back({ key, node.generation, node.gpuDependency });
            }
        }
    }
    std::vector<PendingCompletion> completed;
    completed.reserve(pending.size());
    for (auto& item : pending) {
        if (item.token && item.token->Complete()) completed.push_back(std::move(item));
    }

    std::vector<ArtifactSnapshot> ready;
    std::function<void(const ArtifactSnapshot&)> callback;
    {
        std::lock_guard lock(m_impl->mutex);
        for (const auto& item : completed) {
            const auto found = m_impl->nodes.find(item.key);
            if (found == m_impl->nodes.end()) continue;
            auto& node = found->second;
            if (node.generation != item.generation ||
                node.state != ArtifactReadiness::UploadSubmitted ||
                node.gpuDependency != item.token) continue;
            if (node.uploadSubmittedAt != std::chrono::steady_clock::time_point{}) {
                m_impl->stats.gpuWaitMicros += static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - node.uploadSubmittedAt).count());
            }
            node.gpuDependency.reset();
            m_impl->SetState(node, ArtifactReadiness::GpuReady);
            if (m_impl->stats.gpuWaiting) --m_impl->stats.gpuWaiting;
            ready.push_back(m_impl->MakeSnapshot(node));
            m_impl->WakeWaiters(node.key);
        }
        m_impl->stats.gpuWaiting = 0;
        for (const auto& [_, node] : m_impl->nodes) {
            if (node.state == ArtifactReadiness::UploadSubmitted && node.gpuDependency) {
                ++m_impl->stats.gpuWaiting;
            }
        }
        callback = m_impl->readyCallback;
    }
    if (callback) for (const auto& snapshot : ready) callback(snapshot);
    m_impl->ScheduleDrain();
}

void AsyncStateGraph::SetReadyCallback(std::function<void(const ArtifactSnapshot&)> callback) {
    std::lock_guard lock(m_impl->mutex);
    m_impl->readyCallback = std::move(callback);
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
        if (node.gpuDependency) (void)node.gpuDependency->Cancel();
    }
    for (auto& completion : m_impl->completions) {
        if (completion.result.gpuDependency) (void)completion.result.gpuDependency->Cancel();
    }
    m_impl->pending.clear();
    m_impl->completions.clear();
    ArtifactKey discardedSignal;
    while (m_impl->gpuSignals.try_pop(discardedSignal)) {}
    m_impl->readyCallback = {};
}

} // namespace br::render
