#pragma once

#include <rhi.h>

namespace br::telemetry::nvperf {

bool CaptureRequestedByEnvironment();
bool ServicePendingGpuOperations();
void LogStartupProbe(rhi::Backend backend, rhi::Device device, rhi::Queue graphicsQueue);
void BeginFrameCapture(rhi::Backend backend, rhi::Device device, rhi::Queue graphicsQueue, uint64_t frameNumber);
void EndFrameCapture(rhi::Backend backend, rhi::Queue graphicsQueue, uint64_t frameNumber);
void BeginPassRange(rhi::Backend backend, rhi::CommandList commandList, rhi::Queue queue, const char* queueName, const char* passName);
void EndPassRange(rhi::Backend backend, rhi::CommandList commandList, rhi::Queue queue);

} // namespace br::telemetry::nvperf
