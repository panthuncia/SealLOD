#pragma once

#include "Runtime/StateGraph/AsyncStateGraph.h"

namespace br::render {
AsyncStateGraphHostHooks MakeRendererGraphHooks();
std::shared_ptr<graph_detail::GraphTraceSession> MakeRendererGraphTrace(AsyncStateGraphTraceConfig config);
}
