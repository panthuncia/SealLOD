#pragma once

#include <cstdint>
#include <memory>

#include "BasicRenderer/Extensions/EnvironmentWorkService.h"

namespace org { class CompiledGraph; class Resource; class Runtime; }

namespace br::render {

// The graph environment processing runs in: HDRI conversion to a cubemap, prefiltering, and spherical harmonics. It
// is a graph of its own on the runtime (an org::CompiledGraph), so a new environment changes its declarations instead
// of the frame graph's. The frame graph reads its results through its own declarations of the same resources, and the
// runtime orders those reads after these writes.
//
// It executes on the render thread, ahead of the frame graph, while one of its queues has work.
class EnvironmentJobGraph {
public:
    EnvironmentJobGraph(std::shared_ptr<org::Runtime> runtime, EnvironmentWorkServices work,
        std::shared_ptr<org::Resource> environmentInfo, uint32_t framesInFlight);
    ~EnvironmentJobGraph();
    EnvironmentJobGraph(const EnvironmentJobGraph&) = delete;
    EnvironmentJobGraph& operator=(const EnvironmentJobGraph&) = delete;

    // One execution while a queue has work (a new declaration builds on a worker, so work can wait a few executions).
    // Otherwise only the finished executions retire.
    void Execute();

private:
    EnvironmentWorkServices m_work;
    std::shared_ptr<org::CompiledGraph> m_graph;
    uint64_t m_executions = 0;  // logged when it goes
};

} // namespace br::render
