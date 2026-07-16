#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <rhi.h>

namespace br::telemetry::nvperf {

struct MetricRequest {
    std::string id;
    std::string name;
    std::string outputName;
    std::string unit;
    uint8_t metricType = 0;
    uint8_t rollupOp = 0;
    uint16_t submetric = 0;
    bool required = true;
};

struct PassFilter {
    std::string name;
    std::string queue;
};

struct CaptureConfiguration {
    std::vector<MetricRequest> metrics;
    std::vector<PassFilter> passes;
    std::string controllerQueue = "Graphics";
    uint32_t syncTimeoutMs = 10000;
};

struct RangeResult {
    std::string queue;
    std::string passName;
    uint32_t occurrence = 0;
    uint32_t rangeIndex = 0;
    std::vector<double> values;
};

struct CaptureResult {
    uint64_t sampleId = 0;
    uint64_t startFrame = 0;
    uint64_t endFrame = 0;
    size_t scheduledPasses = 0;
    uint64_t droppedRanges = 0;
    uint64_t droppedTraceBytes = 0;
    bool success = false;
    std::string error;
    std::string chipName;
    std::vector<MetricRequest> metrics;
    std::vector<MetricRequest> unsupportedMetrics;
    std::vector<RangeResult> ranges;
};

bool ConfigureCapture(const CaptureConfiguration& configuration, std::string& error);
bool ArmCapture(uint64_t sampleId, std::string& error);
bool CaptureConfigured();
bool CaptureComplete();
size_t ScheduledPassCount();
std::optional<CaptureResult> TakeCaptureResult();
void ResetCaptureConfiguration();
void SetStreamingSuppressed(bool suppressed);
bool StreamingSuppressed();

bool CaptureRequestedByEnvironment();
bool CaptureActive();
bool ServicePendingGpuOperations();
void LogStartupProbe(rhi::Backend backend, rhi::Device device, rhi::Queue graphicsQueue);
void BeginFrameCapture(rhi::Backend backend, rhi::Device device, rhi::Queue graphicsQueue, uint64_t frameNumber);
void EndFrameCapture(rhi::Backend backend, rhi::Queue graphicsQueue, uint64_t frameNumber);
void BeginPassRange(rhi::Backend backend, rhi::CommandList commandList, rhi::Queue queue, const char* queueName, const char* passName);
void EndPassRange(rhi::Backend backend, rhi::CommandList commandList, rhi::Queue queue);

} // namespace br::telemetry::nvperf
