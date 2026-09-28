#pragma once

#include <BasicRenderer/Streaming/ArtifactTypes.h>
#include "Runtime/StateGraph/GraphDiagnostics.h"
#include <ORGModuleServices/Async/GraphTrace.h>

namespace br::render::graph_detail {

using GraphTraceSession = org::async::GraphTraceSession<ArtifactKind,
    AsyncStateGraphTraceEventID, kArtifactKindCount, AsyncStateGraphTraceReport>;

} // namespace br::render::graph_detail
