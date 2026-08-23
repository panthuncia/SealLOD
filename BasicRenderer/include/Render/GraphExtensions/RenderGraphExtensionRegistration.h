#pragma once

#include <functional>
#include <memory>
#include <string>

#include <OpenRenderGraph/OpenRenderGraph.h>

#include "Render/BuiltinResources.h"
#include "Render/RenderContext.h"
#include "RenderPasses/Base/ComputePass.h"
#include "RenderPasses/Base/CopyPass.h"
#include "RenderPasses/Base/RenderPass.h"

using RenderGraphExtensionFactory = std::function<std::unique_ptr<RenderGraph::IRenderGraphExtension>()>;
