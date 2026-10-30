#pragma once

#include <functional>
#include <memory>
#include "Render/RenderGraph/RenderGraphExtension.h"
namespace org { class RenderGraph; }

using RenderGraphExtensionFactory = std::function<std::unique_ptr<org::IRenderGraphExtension>()>;
