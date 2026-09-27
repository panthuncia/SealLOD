#pragma once

#include <functional>
#include <memory>
#include <OpenRenderGraph/OpenRenderGraph.h>

using RenderGraphExtensionFactory = std::function<std::unique_ptr<org::RenderGraph::IRenderGraphExtension>()>;
