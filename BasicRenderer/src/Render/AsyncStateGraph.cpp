#include "Render/AsyncStateGraph.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <format>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

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
    return token;
}

struct AsyncStateGraph::Impl : std::enable_shared_from_this<Impl> {
    struct Node {
        ArtifactKey key;
        std::uint64_t desiredRevision = 0;
        std::uint64_t producedRevision = 0;
        std::uint64_t generation = 0;
        ArtifactReadiness state = ArtifactReadiness::Missing;
        ArtifactPayload payload;
        ArtifactPayload checkpoint;
        std::vector<ArtifactRequirement> requirements;
        std::shared_ptr<const GpuDependencyToken> gpuDependency;
        std::optional<std::chrono::steady_clock::time_point> retryAt;
        std::string error;
        std::chrono::steady_clock::time_point stateSince = std::chrono::steady_clock::now();
        bool buildInFlight = false;
    };

    struct Completion {
        ArtifactKey key;
        std::uint64_t revision = 0;
        std::uint64_t generation = 0;
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
        bool hasAny = false;
        bool anySatisfied = false;
        for (const auto& requirement : node.requirements) {
            const bool satisfied = RequirementSatisfied(requirement);
            switch (requirement.policy) {
            case DependencyPolicy::Optional: break;
            case DependencyPolicy::AnyOf: hasAny = true; anySatisfied |= satisfied; break;
            case DependencyPolicy::AllOf:
            case DependencyPolicy::FallbackAllowed:
                if (!satisfied) return false;
                break;
            }
        }
        return !hasAny || anySatisfied;
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
            output += std::format(" gpu-value={}", node.gpuDependency->value);
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
        for (const auto& requirement : node.requirements) {
            const auto found = nodes.find(requirement.key);
            if (found != nodes.end() && RequirementSatisfied(requirement)) result.push_back(MakeSnapshot(found->second));
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
        const bool submitted = scheduler.Submit(scope, registration.lane, registration.domain,
            registration.taskName.empty() ? "AsyncStateGraph::Build" : registration.taskName,
            [weak, registration, context = std::move(context), key, revision, generation](const TaskContext& cancellation) mutable {
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
                    self->completions.push_back({ key, revision, generation, std::move(result) });
                }
                self->ScheduleDrain();
            });
        if (!submitted) {
            auto found = nodes.find(key);
            if (found != nodes.end()) {
                found->second.buildInFlight = false;
                found->second.error = "scheduler rejected producer";
                SetState(found->second, ArtifactReadiness::Failed);
                ++stats.failed;
            }
        }
    }

    void ApplyCompletion(Completion completion, std::vector<ArtifactSnapshot>& ready) {
        const auto found = nodes.find(completion.key);
        if (found == nodes.end()) return;
        auto& node = found->second;
        if (node.generation != completion.generation || node.desiredRevision != completion.revision) {
            ++stats.staleCompletions;
            node.buildInFlight = false;
            QueueNode(node);
            return;
        }
        node.buildInFlight = false;
        ++stats.buildsCompleted;
        auto& result = completion.result;
        switch (result.outcome) {
        case ArtifactBuildResult::Outcome::Ready:
            node.payload = std::move(result.payload);
            node.checkpoint = {};
            node.retryAt.reset();
            node.producedRevision = node.desiredRevision;
            node.gpuDependency = std::move(result.gpuDependency);
            if (node.gpuDependency && !node.gpuDependency->Complete()) {
                SetState(node, ArtifactReadiness::UploadSubmitted);
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
        constexpr std::size_t maxTransitions = 128;
        constexpr auto maxDuration = std::chrono::milliseconds(2);
        const auto started = std::chrono::steady_clock::now();
        std::size_t transitions = 0;
        std::vector<std::pair<ArtifactProducerRegistration, ArtifactBuildContext>> builds;
        std::vector<ArtifactSnapshot> ready;
        std::function<void(const ArtifactSnapshot&)> callback;
        std::optional<std::chrono::steady_clock::duration> retryDelay;
        bool hasImmediateWork = false;
        {
            std::lock_guard lock(mutex);
            const auto now = std::chrono::steady_clock::now();
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
                SetState(node, ArtifactReadiness::Preparing);
                ++stats.buildsStarted;
                ArtifactBuildContext context{ node.key, node.desiredRevision, node.generation,
                    DependencySnapshots(node), node.checkpoint, {} };
                builds.emplace_back(producer->second, std::move(context));
            }
            callback = readyCallback;
            hasImmediateWork = !pending.empty() || !completions.empty();
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

bool AsyncStateGraph::Request(ArtifactKey key, std::uint64_t desiredRevision,
    std::vector<ArtifactRequirement> requirements) {
    if (m_impl->shuttingDown.load(std::memory_order_acquire)) return false;
    {
        std::lock_guard lock(m_impl->mutex);
        auto& node = m_impl->nodes[key];
        node.key = key;
        if (desiredRevision < node.desiredRevision) return false;
        if (desiredRevision == node.desiredRevision && node.state != ArtifactReadiness::Missing &&
            node.state != ArtifactReadiness::Failed && node.state != ArtifactReadiness::Cancelled) return true;
        m_impl->RemoveWaiterEdges(node);
        node.desiredRevision = desiredRevision;
        ++node.generation;
        node.requirements = std::move(requirements);
        node.error.clear();
        node.gpuDependency.reset();
        node.retryAt.reset();
        m_impl->InstallWaiterEdges(node);
        ++m_impl->stats.requests;
        const auto cycle = m_impl->DetectCycle(node);
        if (!cycle.empty()) {
            m_impl->FailCycle(cycle);
        } else {
            m_impl->QueueNode(node);
        }
    }
    m_impl->ScheduleDrain();
    return true;
}

bool AsyncStateGraph::Invalidate(ArtifactKey key, std::uint64_t desiredRevision) {
    std::vector<ArtifactRequirement> requirements;
    bool foundNode = false;
    {
        std::lock_guard lock(m_impl->mutex);
        const auto found = m_impl->nodes.find(key);
        foundNode = found != m_impl->nodes.end();
        if (foundNode) requirements = found->second.requirements;
        ++m_impl->stats.invalidations;
    }
    return Request(key, desiredRevision, foundNode ? std::move(requirements) : std::vector<ArtifactRequirement>{});
}

void AsyncStateGraph::Cancel(ArtifactKey key) {
    std::lock_guard lock(m_impl->mutex);
    const auto found = m_impl->nodes.find(key);
    if (found == m_impl->nodes.end()) return;
    ++found->second.generation;
    found->second.buildInFlight = false;
    found->second.retryAt.reset();
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
    std::vector<ArtifactSnapshot> ready;
    std::function<void(const ArtifactSnapshot&)> callback;
    {
        std::lock_guard lock(m_impl->mutex);
        std::uint64_t waiting = 0;
        for (auto& [_, node] : m_impl->nodes) {
            if (node.state != ArtifactReadiness::UploadSubmitted || !node.gpuDependency) continue;
            if (!node.gpuDependency->Complete()) { ++waiting; continue; }
            node.gpuDependency.reset();
            m_impl->SetState(node, ArtifactReadiness::GpuReady);
            ready.push_back(m_impl->MakeSnapshot(node));
            m_impl->WakeWaiters(node.key);
        }
        m_impl->stats.gpuWaiting = waiting;
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
    return m_impl->stats;
}

void AsyncStateGraph::WaitIdle() const { if (m_impl) m_impl->scope.Wait(); }

void AsyncStateGraph::Shutdown() {
    if (!m_impl || m_impl->shuttingDown.exchange(true)) return;
    m_impl->scope.CancelAndWait();
    std::lock_guard lock(m_impl->mutex);
    m_impl->pending.clear();
    m_impl->completions.clear();
    m_impl->readyCallback = {};
}

} // namespace br::render
