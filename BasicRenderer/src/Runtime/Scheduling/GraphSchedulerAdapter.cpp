#include "Runtime/Scheduling/GraphSchedulerAdapter.h"
#include <BasicRenderer/Streaming/TaskScheduler.h>

namespace br {
namespace {
class GraphSchedulerAdapter final : public org::async::GraphScheduler {
    struct Identity {};
    struct ScopeState final : org::async::TaskScope {
        std::shared_ptr<const Identity> owner;
        br::TaskScope scope;
        ScopeState(std::shared_ptr<const Identity> ownerIn, br::TaskScope scopeIn)
            : owner(ownerIn), scope(std::move(scopeIn)) {}
        bool StopRequested() const noexcept override { return scope.StopRequested(); }
        void Cancel() noexcept override { scope.Cancel(); }
        void Wait() const override { scope.Wait(); }
    };
    TaskSchedulerManager& m_scheduler;
    // The token survives the adapter if a scope is retained. Comparing adapter
    // addresses would permit ABA when another adapter reuses the same address.
    const std::shared_ptr<const Identity> m_identity = std::make_shared<Identity>();
    std::mutex m_traceMutex;
    void* m_traceContext = nullptr;
    org::async::TaskTraceCallback m_traceCallback = nullptr;

    std::shared_ptr<ScopeState> Resolve(const org::async::Scope& scope, org::async::TaskClass cls) const {
        if (cls.lane >= static_cast<unsigned>(TaskLane::Count) ||
            cls.domain >= static_cast<unsigned>(TaskDomain::Count)) return {};
        auto state = std::dynamic_pointer_cast<ScopeState>(scope);
        return state && state->owner == m_identity ? state : nullptr;
    }
    static auto Wrap(Task task) {
        return [task = std::move(task)](const br::TaskContext& context) {
            task(org::async::TaskContext{[context] { return context.StopRequested(); }});
        };
    }
    static void Trace(void* opaque, const br::TaskTraceEvent& event) noexcept {
        auto& self = *static_cast<GraphSchedulerAdapter*>(opaque);
        static_assert(static_cast<unsigned>(br::TaskTraceEventID::Resubmitted) ==
            static_cast<unsigned>(org::async::TaskTraceEventID::Resubmitted));
        org::async::TaskTraceEvent out;
        out.event = static_cast<org::async::TaskTraceEventID>(event.event);
        out.metadata = {event.metadata.taskKind, event.metadata.correlationID, event.metadata.admissionKey,
            event.metadata.workClass, event.metadata.schedulingReason, event.metadata.admissionGroup};
        out.taskID = event.taskID;
        out.queueWaitMicros = event.queueWaitMicros;
        out.executionMicros = event.executionMicros;
        out.queuedDepth = event.queuedDepth;
        out.activeCount = event.activeCount;
        out.domain = static_cast<unsigned>(event.domain);
        out.lane = static_cast<unsigned>(event.lane);
        out.outcome = event.outcome;
        self.m_traceCallback(self.m_traceContext, out);
    }
public:
    explicit GraphSchedulerAdapter(TaskSchedulerManager& scheduler) : m_scheduler(scheduler) {}
    ~GraphSchedulerAdapter() override { RemoveTaskTraceSink(m_traceContext); }
    org::async::Scope CreateScope(std::string_view name) override {
        return std::make_shared<ScopeState>(m_identity, m_scheduler.CreateScope(name));
    }
    bool Dispatch(const org::async::Scope& scope, org::async::TaskClass cls,
        org::async::TaskDispatch dispatch, std::string_view name, Task task,
        org::async::TaskTraceMetadata trace) override {
        const auto state = Resolve(scope, cls);
        if (!state || !task) return false;
        const br::TaskTraceMetadata metadata{trace.taskKind, trace.correlationID, trace.admissionKey,
            trace.workClass, trace.schedulingReason, trace.admissionGroup};
        auto body = Wrap(std::move(task));
        if (dispatch == org::async::TaskDispatch::Controlled)
            return m_scheduler.Submit(state->scope, static_cast<TaskLane>(cls.lane),
                static_cast<TaskDomain>(cls.domain), name, std::move(body), metadata);
        return m_scheduler.SubmitCpu(state->scope, static_cast<TaskLane>(cls.lane),
            static_cast<TaskDomain>(cls.domain), name, std::move(body), metadata);
    }
    bool DispatchAfter(const org::async::Scope& scope, std::chrono::steady_clock::duration delay,
        org::async::TaskClass cls, std::string_view name, Task task) override {
        const auto state = Resolve(scope, cls);
        return state && task && m_scheduler.ScheduleAfter(state->scope, delay,
            static_cast<TaskLane>(cls.lane), static_cast<TaskDomain>(cls.domain), name, Wrap(std::move(task)));
    }
    bool InstallTaskTraceSink(void* context, org::async::TaskTraceCallback callback) noexcept override {
        std::lock_guard lock(m_traceMutex);
        if (!context || !callback || m_traceContext) return false;
        m_traceContext = context;
        m_traceCallback = callback;
        if (m_scheduler.InstallTaskTraceSink(this, &Trace)) return true;
        m_traceContext = nullptr;
        m_traceCallback = nullptr;
        return false;
    }
    void RemoveTaskTraceSink(void* context) noexcept override {
        std::lock_guard lock(m_traceMutex);
        if (!context || context != m_traceContext) return;
        m_scheduler.RemoveTaskTraceSink(this);
        m_traceContext = nullptr;
        m_traceCallback = nullptr;
    }
};
}

std::shared_ptr<org::async::GraphScheduler> MakeGraphSchedulerAdapter(TaskSchedulerManager& scheduler) {
    return std::make_shared<GraphSchedulerAdapter>(scheduler);
}
} // namespace br
