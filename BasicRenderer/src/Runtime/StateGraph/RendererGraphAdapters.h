#pragma once

#include "Runtime/StateGraph/AsyncStateGraph.h"

namespace br::render {
// versionedBuffers: advanced whenever the graph retires versions (none: not advanced).
AsyncStateGraphHostHooks MakeRendererGraphHooks(std::shared_ptr<VersionedBufferRuntime> versionedBuffers = {});
std::shared_ptr<graph_detail::GraphTraceSession> MakeRendererGraphTrace(AsyncStateGraphTraceConfig config);
}
