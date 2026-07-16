#include "Telemetry/NvPerfIntegration.h"

#include <atomic>

#include <spdlog/spdlog.h>

#if BASICRENDERER_ENABLE_NVPERF
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nvperf_d3d12_host.h>
#include <nvperf_d3d12_target.h>
#include <nvperf_host.h>
#include <nvperf_target.h>
#include <nvperf_vulkan_host.h>
#include <nvperf_vulkan_target.h>
#include <rhi_interop_dx12.h>
#include <rhi_interop_vulkan.h>
#include <wrl/client.h>
#endif

namespace br::telemetry::nvperf {
namespace {

std::atomic<bool> g_streamingSuppressed{ false };

#if BASICRENDERER_ENABLE_NVPERF
bool InitializeNvPerf();

const char* StatusName(NVPA_Status status)
{
    const char* statusName = nullptr;
    const char* comment = nullptr;
    NVPW_NVPAStatusToString(status, &statusName, &comment);
    return statusName ? statusName : "NVPA_STATUS_UNKNOWN";
}

bool LogIfFailed(NVPA_Status status, const char* operation)
{
    if (status == NVPA_STATUS_SUCCESS) {
        return false;
    }

    spdlog::warn("NVPerf: {} failed with {}", operation, StatusName(status));
    return true;
}

bool IsTruthyEnv(const char* name)
{
    char* value = nullptr;
    size_t valueSize = 0;
    if (_dupenv_s(&value, &valueSize, name) != 0 || value == nullptr) {
        return false;
    }

    std::string text(value);
    std::free(value);
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text == "1" || text == "true" || text == "yes" || text == "on";
}

std::string ReadEnvString(const char* name)
{
    char* value = nullptr;
    size_t valueSize = 0;
    if (_dupenv_s(&value, &valueSize, name) != 0 || value == nullptr) {
        return {};
    }

    std::string result(value);
    std::free(value);
    return result;
}

std::string CsvEscape(std::string_view text)
{
    bool needsQuotes = false;
    for (char ch : text) {
        if (ch == '"' || ch == ',' || ch == '\n' || ch == '\r') {
            needsQuotes = true;
            break;
        }
    }
    if (!needsQuotes) {
        return std::string(text);
    }

    std::string escaped;
    escaped.reserve(text.size() + 2);
    escaped.push_back('"');
    for (char ch : text) {
        if (ch == '"') {
            escaped.push_back('"');
        }
        escaped.push_back(ch);
    }
    escaped.push_back('"');
    return escaped;
}

struct MetricSpec {
    std::string name;
    uint8_t metricType = NVPW_METRIC_TYPE_COUNTER;
    uint8_t rollupOp = NVPW_ROLLUP_OP_SUM;
    uint16_t submetric = NVPW_SUBMETRIC_NONE;
    std::string outputName;
    std::string id;
    std::string unit;
    bool required = true;
};

struct SelectedMetric {
    MetricSpec spec{};
    NVPW_MetricEvalRequest request{};
};

const std::array<MetricSpec, 50> kDefaultMetrics{ {
    { "gpu__time_duration", NVPW_METRIC_TYPE_COUNTER, NVPW_ROLLUP_OP_SUM, NVPW_SUBMETRIC_NONE },
    { "sm__throughput", NVPW_METRIC_TYPE_THROUGHPUT, NVPW_ROLLUP_OP_AVG, NVPW_SUBMETRIC_PCT_OF_PEAK_SUSTAINED_ELAPSED },
    { "smsp__throughput", NVPW_METRIC_TYPE_THROUGHPUT, NVPW_ROLLUP_OP_AVG, NVPW_SUBMETRIC_PCT_OF_PEAK_SUSTAINED_ELAPSED },
    { "smsp__inst_executed", NVPW_METRIC_TYPE_COUNTER, NVPW_ROLLUP_OP_SUM, NVPW_SUBMETRIC_NONE },
    { "smsp__inst_executed_pipe_alu", NVPW_METRIC_TYPE_COUNTER, NVPW_ROLLUP_OP_SUM, NVPW_SUBMETRIC_NONE },
    { "smsp__inst_executed_pipe_fma", NVPW_METRIC_TYPE_COUNTER, NVPW_ROLLUP_OP_SUM, NVPW_SUBMETRIC_NONE },
    { "smsp__inst_executed_pipe_lsu", NVPW_METRIC_TYPE_COUNTER, NVPW_ROLLUP_OP_SUM, NVPW_SUBMETRIC_NONE },
    { "smsp__inst_executed_pipe_tex", NVPW_METRIC_TYPE_COUNTER, NVPW_ROLLUP_OP_SUM, NVPW_SUBMETRIC_NONE },
    { "smsp__sass_thread_inst_executed_op_fadd_pred_on", NVPW_METRIC_TYPE_COUNTER, NVPW_ROLLUP_OP_SUM, NVPW_SUBMETRIC_NONE },
    { "smsp__sass_thread_inst_executed_op_ffma_pred_on", NVPW_METRIC_TYPE_COUNTER, NVPW_ROLLUP_OP_SUM, NVPW_SUBMETRIC_NONE },
    { "smsp__sass_thread_inst_executed_op_fmul_pred_on", NVPW_METRIC_TYPE_COUNTER, NVPW_ROLLUP_OP_SUM, NVPW_SUBMETRIC_NONE },
    { "smsp__sass_thread_inst_executed_op_integer_pred_on", NVPW_METRIC_TYPE_COUNTER, NVPW_ROLLUP_OP_SUM, NVPW_SUBMETRIC_NONE },
    { "smsp__sass_thread_inst_executed_op_memory_pred_on", NVPW_METRIC_TYPE_COUNTER, NVPW_ROLLUP_OP_SUM, NVPW_SUBMETRIC_NONE },
    { "smsp__warps_active", NVPW_METRIC_TYPE_RATIO, NVPW_ROLLUP_OP_AVG, NVPW_SUBMETRIC_RATIO },
    { "smsp__warps_eligible", NVPW_METRIC_TYPE_RATIO, NVPW_ROLLUP_OP_AVG, NVPW_SUBMETRIC_RATIO },
    { "smsp__warp_issue_stalled_barrier_per_warp_active", NVPW_METRIC_TYPE_RATIO, NVPW_ROLLUP_OP_AVG, NVPW_SUBMETRIC_RATIO },
    { "smsp__warp_issue_stalled_branch_resolving_per_warp_active", NVPW_METRIC_TYPE_RATIO, NVPW_ROLLUP_OP_AVG, NVPW_SUBMETRIC_RATIO },
    { "smsp__warp_issue_stalled_dispatch_stall_per_warp_active", NVPW_METRIC_TYPE_RATIO, NVPW_ROLLUP_OP_AVG, NVPW_SUBMETRIC_RATIO },
    { "smsp__warp_issue_stalled_imc_miss_per_warp_active", NVPW_METRIC_TYPE_RATIO, NVPW_ROLLUP_OP_AVG, NVPW_SUBMETRIC_RATIO },
    { "smsp__warp_issue_stalled_lg_throttle_per_warp_active", NVPW_METRIC_TYPE_RATIO, NVPW_ROLLUP_OP_AVG, NVPW_SUBMETRIC_RATIO },
    { "smsp__warp_issue_stalled_long_scoreboard_per_warp_active", NVPW_METRIC_TYPE_RATIO, NVPW_ROLLUP_OP_AVG, NVPW_SUBMETRIC_RATIO },
    { "smsp__warp_issue_stalled_math_pipe_throttle_per_warp_active", NVPW_METRIC_TYPE_RATIO, NVPW_ROLLUP_OP_AVG, NVPW_SUBMETRIC_RATIO },
    { "smsp__warp_issue_stalled_membar_per_warp_active", NVPW_METRIC_TYPE_RATIO, NVPW_ROLLUP_OP_AVG, NVPW_SUBMETRIC_RATIO },
    { "smsp__warp_issue_stalled_mio_throttle_per_warp_active", NVPW_METRIC_TYPE_RATIO, NVPW_ROLLUP_OP_AVG, NVPW_SUBMETRIC_RATIO },
    { "smsp__warp_issue_stalled_no_instruction_per_warp_active", NVPW_METRIC_TYPE_RATIO, NVPW_ROLLUP_OP_AVG, NVPW_SUBMETRIC_RATIO },
    { "smsp__warp_issue_stalled_not_selected_per_warp_active", NVPW_METRIC_TYPE_RATIO, NVPW_ROLLUP_OP_AVG, NVPW_SUBMETRIC_RATIO },
    { "smsp__warp_issue_stalled_short_scoreboard_per_warp_active", NVPW_METRIC_TYPE_RATIO, NVPW_ROLLUP_OP_AVG, NVPW_SUBMETRIC_RATIO },
    { "smsp__warp_issue_stalled_tex_throttle_per_warp_active", NVPW_METRIC_TYPE_RATIO, NVPW_ROLLUP_OP_AVG, NVPW_SUBMETRIC_RATIO },
    { "smsp__warp_issue_stalled_wait_per_warp_active", NVPW_METRIC_TYPE_RATIO, NVPW_ROLLUP_OP_AVG, NVPW_SUBMETRIC_RATIO },
    { "l1tex__throughput", NVPW_METRIC_TYPE_THROUGHPUT, NVPW_ROLLUP_OP_AVG, NVPW_SUBMETRIC_PCT_OF_PEAK_SUSTAINED_ELAPSED },
    { "lts__throughput", NVPW_METRIC_TYPE_THROUGHPUT, NVPW_ROLLUP_OP_AVG, NVPW_SUBMETRIC_PCT_OF_PEAK_SUSTAINED_ELAPSED },
    { "dram__throughput", NVPW_METRIC_TYPE_THROUGHPUT, NVPW_ROLLUP_OP_AVG, NVPW_SUBMETRIC_PCT_OF_PEAK_SUSTAINED_ELAPSED },
    { "tensor__throughput", NVPW_METRIC_TYPE_THROUGHPUT, NVPW_ROLLUP_OP_AVG, NVPW_SUBMETRIC_PCT_OF_PEAK_SUSTAINED_ELAPSED },
    { "rtcore__throughput", NVPW_METRIC_TYPE_THROUGHPUT, NVPW_ROLLUP_OP_AVG, NVPW_SUBMETRIC_PCT_OF_PEAK_SUSTAINED_ELAPSED },
    { "tpc__warps_launched", NVPW_METRIC_TYPE_COUNTER, NVPW_ROLLUP_OP_SUM, NVPW_SUBMETRIC_NONE },
    { "tpc__warps_launched_shader_cs", NVPW_METRIC_TYPE_COUNTER, NVPW_ROLLUP_OP_SUM, NVPW_SUBMETRIC_NONE },
    { "tpc__warps_launched_shader_ps", NVPW_METRIC_TYPE_COUNTER, NVPW_ROLLUP_OP_SUM, NVPW_SUBMETRIC_NONE },
    { "tpc__warp_launch_cycles_stalled_shader_cs_reason_barrier_allocation", NVPW_METRIC_TYPE_COUNTER, NVPW_ROLLUP_OP_SUM, NVPW_SUBMETRIC_PCT_OF_PEAK_SUSTAINED_ELAPSED, "tpc__warp_launch_stalled_cs_barrier_allocation_pct" },
    { "tpc__warp_launch_cycles_stalled_shader_cs_reason_cta_allocation", NVPW_METRIC_TYPE_COUNTER, NVPW_ROLLUP_OP_SUM, NVPW_SUBMETRIC_PCT_OF_PEAK_SUSTAINED_ELAPSED, "tpc__warp_launch_stalled_cs_cta_allocation_pct" },
    { "tpc__warp_launch_cycles_stalled_shader_cs_reason_register_allocation", NVPW_METRIC_TYPE_COUNTER, NVPW_ROLLUP_OP_SUM, NVPW_SUBMETRIC_PCT_OF_PEAK_SUSTAINED_ELAPSED, "tpc__warp_launch_stalled_cs_register_allocation_pct" },
    { "tpc__warp_launch_cycles_stalled_shader_cs_reason_shmem_allocation", NVPW_METRIC_TYPE_COUNTER, NVPW_ROLLUP_OP_SUM, NVPW_SUBMETRIC_PCT_OF_PEAK_SUSTAINED_ELAPSED, "tpc__warp_launch_stalled_cs_shmem_allocation_pct" },
    { "tpc__warp_launch_cycles_stalled_shader_cs_reason_warp_allocation", NVPW_METRIC_TYPE_COUNTER, NVPW_ROLLUP_OP_SUM, NVPW_SUBMETRIC_PCT_OF_PEAK_SUSTAINED_ELAPSED, "tpc__warp_launch_stalled_cs_warp_allocation_pct" },
    { "tpc__warp_launch_cycles_stalled_shader_ps", NVPW_METRIC_TYPE_COUNTER, NVPW_ROLLUP_OP_SUM, NVPW_SUBMETRIC_PCT_OF_PEAK_SUSTAINED_ELAPSED, "tpc__warp_launch_stalled_ps_pct" },
    { "tpc__warp_launch_cycles_stalled_shader_ps_reason_ooo_warp_completion", NVPW_METRIC_TYPE_COUNTER, NVPW_ROLLUP_OP_SUM, NVPW_SUBMETRIC_PCT_OF_PEAK_SUSTAINED_ELAPSED, "tpc__warp_launch_stalled_ps_ooo_warp_completion_pct" },
    { "tpc__warp_launch_cycles_stalled_shader_ps_reason_register_allocation", NVPW_METRIC_TYPE_COUNTER, NVPW_ROLLUP_OP_SUM, NVPW_SUBMETRIC_PCT_OF_PEAK_SUSTAINED_ELAPSED, "tpc__warp_launch_stalled_ps_register_allocation_pct" },
    { "tpc__warp_launch_cycles_stalled_shader_ps_reason_warp_allocation", NVPW_METRIC_TYPE_COUNTER, NVPW_ROLLUP_OP_SUM, NVPW_SUBMETRIC_PCT_OF_PEAK_SUSTAINED_ELAPSED, "tpc__warp_launch_stalled_ps_warp_allocation_pct" },
    { "tpc__warps_inactive_sm_active_realtime", NVPW_METRIC_TYPE_COUNTER, NVPW_ROLLUP_OP_SUM, NVPW_SUBMETRIC_PCT_OF_PEAK_SUSTAINED_ELAPSED, "tpc__idle_warp_slots_on_active_sms_pct" },
    { "tpc__average_registers_per_thread_shader_cs", NVPW_METRIC_TYPE_RATIO, NVPW_ROLLUP_OP_AVG, NVPW_SUBMETRIC_RATIO },
    { "tpc__average_registers_per_thread_shader_ps", NVPW_METRIC_TYPE_RATIO, NVPW_ROLLUP_OP_AVG, NVPW_SUBMETRIC_RATIO },
    { "tpc__average_sharedmem_bytes_per_cta", NVPW_METRIC_TYPE_RATIO, NVPW_ROLLUP_OP_AVG, NVPW_SUBMETRIC_RATIO },
} };

struct QueueCapture {
    rhi::Queue queue{};
    ID3D12CommandQueue* nativeQueue = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Fence> syncFence;
    HANDLE syncEvent = nullptr;
    uint64_t syncFenceValue = 0;
    std::string queueName;
    std::vector<uint8_t> counterDataImage;
    std::vector<uint8_t> counterDataScratch;
    bool sessionActive = false;
    bool passActive = false;
    bool allPassesSubmitted = false;
    bool decoded = false;
    size_t nextPassIndex = 0;
    uint16_t targetNestingLevel = 1;
};

struct D3D12PassProfiler {
    bool envChecked = false;
    bool requested = false;
    bool programmaticConfigured = false;
    bool armed = false;
    bool initialized = false;
    bool failed = false;
    bool finished = false;
    bool csvHeaderWritten = false;
    uint64_t captureStartFrame = 0;
    uint64_t captureEndFrame = 0;
    uint64_t sampleId = 0;
    std::string chipName;
    std::string controllerQueueName = "Graphics";
    ID3D12Device* nativeDevice = nullptr;
    std::filesystem::path csvPath;
    std::vector<MetricSpec> requestedMetrics;
    std::vector<MetricRequest> unsupportedMetrics;
    std::vector<PassFilter> passFilters;
    std::vector<SelectedMetric> metrics;
    std::vector<uint8_t> configImage;
    std::vector<uint8_t> counterDataPrefix;
    size_t configPassCount = 0;
    size_t traceBufferSize = 0;
    size_t maxRangesPerPass = 512;
    size_t captureStartDelayFrames = 120;
    size_t traceBufferCount = 0;
    uint32_t syncTimeoutMs = 10000;
    uint64_t droppedRanges = 0;
    uint64_t droppedTraceBytes = 0;
    std::string error;
    std::optional<CaptureResult> result;
    std::mutex mutex;
    std::unordered_map<ID3D12CommandQueue*, QueueCapture> queues;
    std::unordered_map<ID3D12GraphicsCommandList*, uint32_t> activeCommandListRanges;
};

D3D12PassProfiler& Profiler()
{
    static D3D12PassProfiler profiler;
    return profiler;
}

size_t ReadEnvSizeT(const char* name, size_t fallback)
{
    const std::string text = ReadEnvString(name);
    if (text.empty()) {
        return fallback;
    }

    char* end = nullptr;
    const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
    if (end == text.c_str()) {
        spdlog::warn("NVPerf: could not parse {}='{}'; using {}", name, text, fallback);
        return fallback;
    }
    return static_cast<size_t>(value);
}

void ReleaseQueueSyncObjects(QueueCapture& queueCapture)
{
    queueCapture.syncFence.Reset();
    queueCapture.syncFenceValue = 0;
    if (queueCapture.syncEvent) {
        CloseHandle(queueCapture.syncEvent);
        queueCapture.syncEvent = nullptr;
    }
}

bool EnsureQueueSyncObjects(D3D12PassProfiler& profiler, QueueCapture& queueCapture)
{
    if (queueCapture.syncFence && queueCapture.syncEvent) {
        return true;
    }

    if (!profiler.nativeDevice) {
        spdlog::warn("NVPerf: cannot create profiler synchronization fence without a D3D12 device");
        return false;
    }

    HRESULT hr = profiler.nativeDevice->CreateFence(
        0,
        D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS(queueCapture.syncFence.GetAddressOf()));
    if (FAILED(hr)) {
        spdlog::warn("NVPerf: failed to create profiler synchronization fence hr=0x{:08x}", static_cast<unsigned int>(hr));
        return false;
    }

    queueCapture.syncEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!queueCapture.syncEvent) {
        spdlog::warn("NVPerf: failed to create profiler synchronization event error={}", GetLastError());
        queueCapture.syncFence.Reset();
        return false;
    }

    return true;
}

bool SynchronizeQueueAfterProfilerPass(D3D12PassProfiler& profiler, QueueCapture& queueCapture)
{
    if (!EnsureQueueSyncObjects(profiler, queueCapture)) {
        return false;
    }

    const uint64_t fenceValue = ++queueCapture.syncFenceValue;
    HRESULT hr = queueCapture.nativeQueue->Signal(queueCapture.syncFence.Get(), fenceValue);
    if (FAILED(hr)) {
        spdlog::warn(
            "NVPerf: failed to signal profiler synchronization fence for queue '{}' value={} hr=0x{:08x}",
            queueCapture.queueName,
            fenceValue,
            static_cast<unsigned int>(hr));
        return false;
    }

    if (queueCapture.syncFence->GetCompletedValue() >= fenceValue) {
        return true;
    }

    hr = queueCapture.syncFence->SetEventOnCompletion(fenceValue, queueCapture.syncEvent);
    if (FAILED(hr)) {
        spdlog::warn(
            "NVPerf: failed to arm profiler synchronization fence for queue '{}' value={} hr=0x{:08x}",
            queueCapture.queueName,
            fenceValue,
            static_cast<unsigned int>(hr));
        return false;
    }

    const DWORD waitResult = WaitForSingleObject(queueCapture.syncEvent, profiler.syncTimeoutMs);
    if (waitResult != WAIT_OBJECT_0) {
        spdlog::warn(
            "NVPerf: profiler synchronization timed out for queue '{}' value={} completed={} timeoutMs={} waitResult={}",
            queueCapture.queueName,
            fenceValue,
            queueCapture.syncFence->GetCompletedValue(),
            profiler.syncTimeoutMs,
            waitResult);
        return false;
    }

    spdlog::debug(
        "NVPerf: synchronized profiler pass for queue '{}' value={}",
        queueCapture.queueName,
        fenceValue);
    return true;
}

void ServicePendingGpuOperationsForQueue(QueueCapture& queueCapture, const char* reason, uint32_t numOperations, uint32_t timeoutMs)
{
    NVPW_D3D12_Queue_ServicePendingGpuOperations_Params service{ NVPW_D3D12_Queue_ServicePendingGpuOperations_Params_STRUCT_SIZE };
    service.pCommandQueue = queueCapture.nativeQueue;
    service.numOperations = numOperations;
    service.timeout = timeoutMs;
    const NVPA_Status status = NVPW_D3D12_Queue_ServicePendingGpuOperations(&service);
    if (status != NVPA_STATUS_SUCCESS) {
        spdlog::debug(
            "NVPerf: service pending GPU operations for queue '{}' reason='{}' returned {}",
            queueCapture.queueName,
            reason,
            StatusName(status));
        return;
    }
    if (service.timeoutExpired) {
        spdlog::debug(
            "NVPerf: service pending GPU operations for queue '{}' reason='{}' timed out without blocking",
            queueCapture.queueName,
            reason);
    }
}

std::optional<size_t> FindMetricIndex(NVPW_MetricsEvaluator* evaluator, const MetricSpec& spec)
{
    NVPW_MetricsEvaluator_GetMetricNames_Params names{ NVPW_MetricsEvaluator_GetMetricNames_Params_STRUCT_SIZE };
    names.pMetricsEvaluator = evaluator;
    names.metricType = spec.metricType;
    if (LogIfFailed(NVPW_MetricsEvaluator_GetMetricNames(&names), "NVPW_MetricsEvaluator_GetMetricNames")) {
        return std::nullopt;
    }

    for (size_t i = 0; i < names.numMetrics; ++i) {
        const char* metricName = names.pMetricNames + names.pMetricNameBeginIndices[i];
        if (metricName && spec.name == metricName) {
            return i;
        }
    }
    return std::nullopt;
}

bool BuildD3D12ProfilerConfig(D3D12PassProfiler& profiler, const char* chipName, const uint8_t* counterAvailability, size_t counterAvailabilitySize)
{
    profiler.metrics.clear();
    profiler.unsupportedMetrics.clear();
    profiler.configImage.clear();
    profiler.counterDataPrefix.clear();

    NVPW_D3D12_MetricsEvaluator_CalculateScratchBufferSize_Params scratchParams{ NVPW_D3D12_MetricsEvaluator_CalculateScratchBufferSize_Params_STRUCT_SIZE };
    scratchParams.pChipName = chipName;
    if (LogIfFailed(NVPW_D3D12_MetricsEvaluator_CalculateScratchBufferSize(&scratchParams), "NVPW_D3D12_MetricsEvaluator_CalculateScratchBufferSize")) {
        return false;
    }

    std::vector<uint8_t> evaluatorScratch(scratchParams.scratchBufferSize);
    NVPW_D3D12_MetricsEvaluator_Initialize_Params evalInit{ NVPW_D3D12_MetricsEvaluator_Initialize_Params_STRUCT_SIZE };
    evalInit.pScratchBuffer = evaluatorScratch.data();
    evalInit.scratchBufferSize = evaluatorScratch.size();
    evalInit.pChipName = chipName;
    if (LogIfFailed(NVPW_D3D12_MetricsEvaluator_Initialize(&evalInit), "NVPW_D3D12_MetricsEvaluator_Initialize")) {
        return false;
    }
    NVPW_MetricsEvaluator* evaluator = evalInit.pMetricsEvaluator;

    std::vector<NVPW_RawCounterRequest> rawCounterRequests;
    std::unordered_set<std::string> rawCounterNames;

    const auto& requestedMetrics = profiler.requestedMetrics.empty()
        ? std::vector<MetricSpec>(kDefaultMetrics.begin(), kDefaultMetrics.end())
        : profiler.requestedMetrics;
    for (const MetricSpec& spec : requestedMetrics) {
        const auto metricIndex = FindMetricIndex(evaluator, spec);
        if (!metricIndex) {
            spdlog::warn("NVPerf: metric '{}' is unavailable on chip '{}'", spec.name, chipName);
            profiler.unsupportedMetrics.push_back({
                spec.id.empty() ? spec.name : spec.id,
                spec.name,
                spec.outputName,
                spec.unit,
                spec.metricType,
                spec.rollupOp,
                spec.submetric,
                spec.required
            });
            continue;
        }

        SelectedMetric metric{};
        metric.spec = spec;
        metric.request.metricIndex = *metricIndex;
        metric.request.metricType = spec.metricType;
        metric.request.rollupOp = spec.rollupOp;
        metric.request.submetric = spec.submetric;

        NVPW_MetricsEvaluator_GetMetricRawDependencies_Params deps{ NVPW_MetricsEvaluator_GetMetricRawDependencies_Params_STRUCT_SIZE };
        deps.pMetricsEvaluator = evaluator;
        deps.pMetricEvalRequests = &metric.request;
        deps.numMetricEvalRequests = 1;
        deps.metricEvalRequestStructSize = NVPW_MetricEvalRequest_STRUCT_SIZE;
        deps.metricEvalRequestStrideSize = sizeof(NVPW_MetricEvalRequest);
        if (LogIfFailed(NVPW_MetricsEvaluator_GetMetricRawDependencies(&deps), "NVPW_MetricsEvaluator_GetMetricRawDependencies(count)")) {
            continue;
        }

        std::vector<const char*> dependencies(deps.numRawDependencies);
        deps.ppRawDependencies = dependencies.data();
        deps.numRawDependencies = dependencies.size();
        if (LogIfFailed(NVPW_MetricsEvaluator_GetMetricRawDependencies(&deps), "NVPW_MetricsEvaluator_GetMetricRawDependencies(values)")) {
            continue;
        }

        for (const char* dependency : dependencies) {
            if (!dependency || !rawCounterNames.insert(dependency).second) {
                continue;
            }
        }

        profiler.metrics.push_back(metric);
    }

    NVPW_MetricsEvaluator_Destroy_Params evalDestroy{ NVPW_MetricsEvaluator_Destroy_Params_STRUCT_SIZE };
    evalDestroy.pMetricsEvaluator = evaluator;
    (void)NVPW_MetricsEvaluator_Destroy(&evalDestroy);

    if (profiler.metrics.empty() || rawCounterRequests.empty()) {
        rawCounterRequests.reserve(rawCounterNames.size());
        for (const std::string& rawCounterName : rawCounterNames) {
            NVPW_RawCounterRequest request{};
            request.pRawCounterName = rawCounterName.c_str();
            request.domain = NVPW_RAW_COUNTER_DOMAIN_INVALID;
            request.keepInstances = false;
            rawCounterRequests.push_back(request);
        }
    }

    if (profiler.metrics.empty() || rawCounterRequests.empty()) {
        spdlog::warn("NVPerf: no requested pass metrics could be configured");
        return false;
    }

    NVPW_D3D12_RawCounterConfig_Create_Params rawConfigCreate{ NVPW_D3D12_RawCounterConfig_Create_Params_STRUCT_SIZE };
    rawConfigCreate.pChipName = chipName;
    rawConfigCreate.activityKind = NVPA_ACTIVITY_KIND_PROFILER;
    if (LogIfFailed(NVPW_D3D12_RawCounterConfig_Create(&rawConfigCreate), "NVPW_D3D12_RawCounterConfig_Create")) {
        return false;
    }
    NVPW_RawCounterConfig* rawConfig = rawConfigCreate.pRawCounterConfig;

    NVPW_RawCounterConfig_SetCounterAvailability_Params availability{ NVPW_RawCounterConfig_SetCounterAvailability_Params_STRUCT_SIZE };
    availability.pRawCounterConfig = rawConfig;
    availability.pCounterAvailabilityImage = counterAvailability;
    if (counterAvailability && counterAvailabilitySize > 0) {
        (void)NVPW_RawCounterConfig_SetCounterAvailability(&availability);
    }

    NVPW_RawCounterConfig_GetAllAvailableRawCounterDomains_Params domainCount{ NVPW_RawCounterConfig_GetAllAvailableRawCounterDomains_Params_STRUCT_SIZE };
    domainCount.pRawCounterConfig = rawConfig;
    if (LogIfFailed(NVPW_RawCounterConfig_GetAllAvailableRawCounterDomains(&domainCount), "NVPW_RawCounterConfig_GetAllAvailableRawCounterDomains(count)")) {
        NVPW_RawCounterConfig_Destroy_Params destroy{ NVPW_RawCounterConfig_Destroy_Params_STRUCT_SIZE };
        destroy.pRawCounterConfig = rawConfig;
        (void)NVPW_RawCounterConfig_Destroy(&destroy);
        return false;
    }
    std::vector<NVPW_RawCounterDomain> domains(domainCount.numAvailableDomains);
    NVPW_RawCounterConfig_GetAllAvailableRawCounterDomains_Params domainValues{ NVPW_RawCounterConfig_GetAllAvailableRawCounterDomains_Params_STRUCT_SIZE };
    domainValues.pRawCounterConfig = rawConfig;
    domainValues.numAvailableDomains = domains.size();
    domainValues.pAvailableDomains = domains.data();
    if (LogIfFailed(NVPW_RawCounterConfig_GetAllAvailableRawCounterDomains(&domainValues), "NVPW_RawCounterConfig_GetAllAvailableRawCounterDomains(values)")) {
        NVPW_RawCounterConfig_Destroy_Params destroy{ NVPW_RawCounterConfig_Destroy_Params_STRUCT_SIZE };
        destroy.pRawCounterConfig = rawConfig;
        (void)NVPW_RawCounterConfig_Destroy(&destroy);
        return false;
    }
    domains.resize(domainValues.numAvailableDomains);
    NVPW_RawCounterConfig_BeginPassGroup_Params beginPassGroup{ NVPW_RawCounterConfig_BeginPassGroup_Params_STRUCT_SIZE };
    beginPassGroup.pRawCounterConfig = rawConfig;
    beginPassGroup.numDomains = domains.size();
    beginPassGroup.pDomains = domains.data();
    if (LogIfFailed(NVPW_RawCounterConfig_BeginPassGroup(&beginPassGroup), "NVPW_RawCounterConfig_BeginPassGroup")) {
        NVPW_RawCounterConfig_Destroy_Params destroy{ NVPW_RawCounterConfig_Destroy_Params_STRUCT_SIZE };
        destroy.pRawCounterConfig = rawConfig;
        (void)NVPW_RawCounterConfig_Destroy(&destroy);
        return false;
    }

    NVPW_RawCounterConfig_AddRawCounters_Params addRaw{ NVPW_RawCounterConfig_AddRawCounters_Params_STRUCT_SIZE };
    addRaw.pRawCounterConfig = rawConfig;
    addRaw.rawCounterRequestStructSize = NVPW_RAW_COUNTER_REQUEST_STRUCT_SIZE;
    addRaw.numRawCounterRequests = rawCounterRequests.size();
    addRaw.pRawCounterRequests = rawCounterRequests.data();
    if (LogIfFailed(NVPW_RawCounterConfig_AddRawCounters(&addRaw), "NVPW_RawCounterConfig_AddRawCounters")) {
        NVPW_RawCounterConfig_Destroy_Params destroy{ NVPW_RawCounterConfig_Destroy_Params_STRUCT_SIZE };
        destroy.pRawCounterConfig = rawConfig;
        (void)NVPW_RawCounterConfig_Destroy(&destroy);
        return false;
    }

    NVPW_RawCounterConfig_EndPassGroup_Params endPassGroup{ NVPW_RawCounterConfig_EndPassGroup_Params_STRUCT_SIZE };
    endPassGroup.pRawCounterConfig = rawConfig;
    endPassGroup.numDomains = domains.size();
    endPassGroup.pDomains = domains.data();
    if (LogIfFailed(NVPW_RawCounterConfig_EndPassGroup(&endPassGroup), "NVPW_RawCounterConfig_EndPassGroup")) {
        NVPW_RawCounterConfig_Destroy_Params destroy{ NVPW_RawCounterConfig_Destroy_Params_STRUCT_SIZE };
        destroy.pRawCounterConfig = rawConfig;
        (void)NVPW_RawCounterConfig_Destroy(&destroy);
        return false;
    }

    NVPW_RawCounterConfig_GenerateConfigImage_Params generate{ NVPW_RawCounterConfig_GenerateConfigImage_Params_STRUCT_SIZE };
    generate.pRawCounterConfig = rawConfig;
    if (LogIfFailed(NVPW_RawCounterConfig_GenerateConfigImage(&generate), "NVPW_RawCounterConfig_GenerateConfigImage")) {
        NVPW_RawCounterConfig_Destroy_Params destroy{ NVPW_RawCounterConfig_Destroy_Params_STRUCT_SIZE };
        destroy.pRawCounterConfig = rawConfig;
        (void)NVPW_RawCounterConfig_Destroy(&destroy);
        return false;
    }

    NVPW_RawCounterConfig_GetConfigImage_Params configImage{ NVPW_RawCounterConfig_GetConfigImage_Params_STRUCT_SIZE };
    configImage.pRawCounterConfig = rawConfig;
    if (LogIfFailed(NVPW_RawCounterConfig_GetConfigImage(&configImage), "NVPW_RawCounterConfig_GetConfigImage(size)")) {
        NVPW_RawCounterConfig_Destroy_Params destroy{ NVPW_RawCounterConfig_Destroy_Params_STRUCT_SIZE };
        destroy.pRawCounterConfig = rawConfig;
        (void)NVPW_RawCounterConfig_Destroy(&destroy);
        return false;
    }
    profiler.configImage.resize(configImage.bytesCopied);
    configImage.bytesAllocated = profiler.configImage.size();
    configImage.pBuffer = profiler.configImage.data();
    if (LogIfFailed(NVPW_RawCounterConfig_GetConfigImage(&configImage), "NVPW_RawCounterConfig_GetConfigImage(data)")) {
        NVPW_RawCounterConfig_Destroy_Params destroy{ NVPW_RawCounterConfig_Destroy_Params_STRUCT_SIZE };
        destroy.pRawCounterConfig = rawConfig;
        (void)NVPW_RawCounterConfig_Destroy(&destroy);
        return false;
    }

    NVPW_RawCounterConfig_GetNumPasses_Params passes{ NVPW_RawCounterConfig_GetNumPasses_Params_STRUCT_SIZE };
    passes.pRawCounterConfig = rawConfig;
    if (!LogIfFailed(NVPW_RawCounterConfig_GetNumPasses(&passes), "NVPW_RawCounterConfig_GetNumPasses")) {
        profiler.configPassCount = passes.numPasses;
    }

    NVPW_CounterDataBuilder_Create_Params builderCreate{ NVPW_CounterDataBuilder_Create_Params_STRUCT_SIZE };
    builderCreate.pChipName = chipName;
    if (LogIfFailed(NVPW_CounterDataBuilder_Create(&builderCreate), "NVPW_CounterDataBuilder_Create")) {
        NVPW_RawCounterConfig_Destroy_Params destroy{ NVPW_RawCounterConfig_Destroy_Params_STRUCT_SIZE };
        destroy.pRawCounterConfig = rawConfig;
        (void)NVPW_RawCounterConfig_Destroy(&destroy);
        return false;
    }
    NVPA_CounterDataBuilder* builder = builderCreate.pCounterDataBuilder;

    NVPW_CounterDataBuilder_AddRawCounters_Params builderAdd{ NVPW_CounterDataBuilder_AddRawCounters_Params_STRUCT_SIZE };
    builderAdd.pCounterDataBuilder = builder;
    builderAdd.rawCounterRequestStructSize = NVPW_RAW_COUNTER_REQUEST_STRUCT_SIZE;
    builderAdd.numRawCounterRequests = rawCounterRequests.size();
    builderAdd.pRawCounterRequests = rawCounterRequests.data();
    if (LogIfFailed(NVPW_CounterDataBuilder_AddRawCounters(&builderAdd), "NVPW_CounterDataBuilder_AddRawCounters")) {
        NVPW_CounterDataBuilder_Destroy_Params destroyBuilder{ NVPW_CounterDataBuilder_Destroy_Params_STRUCT_SIZE };
        destroyBuilder.pCounterDataBuilder = builder;
        (void)NVPW_CounterDataBuilder_Destroy(&destroyBuilder);
        NVPW_RawCounterConfig_Destroy_Params destroy{ NVPW_RawCounterConfig_Destroy_Params_STRUCT_SIZE };
        destroy.pRawCounterConfig = rawConfig;
        (void)NVPW_RawCounterConfig_Destroy(&destroy);
        return false;
    }

    NVPW_CounterDataBuilder_GetCounterDataPrefix_Params prefix{ NVPW_CounterDataBuilder_GetCounterDataPrefix_Params_STRUCT_SIZE };
    prefix.pCounterDataBuilder = builder;
    if (LogIfFailed(NVPW_CounterDataBuilder_GetCounterDataPrefix(&prefix), "NVPW_CounterDataBuilder_GetCounterDataPrefix(size)")) {
        return false;
    }
    profiler.counterDataPrefix.resize(prefix.bytesCopied);
    prefix.bytesAllocated = profiler.counterDataPrefix.size();
    prefix.pBuffer = profiler.counterDataPrefix.data();
    if (LogIfFailed(NVPW_CounterDataBuilder_GetCounterDataPrefix(&prefix), "NVPW_CounterDataBuilder_GetCounterDataPrefix(data)")) {
        return false;
    }

    NVPW_CounterDataBuilder_Destroy_Params destroyBuilder{ NVPW_CounterDataBuilder_Destroy_Params_STRUCT_SIZE };
    destroyBuilder.pCounterDataBuilder = builder;
    (void)NVPW_CounterDataBuilder_Destroy(&destroyBuilder);
    NVPW_RawCounterConfig_Destroy_Params destroy{ NVPW_RawCounterConfig_Destroy_Params_STRUCT_SIZE };
    destroy.pRawCounterConfig = rawConfig;
    (void)NVPW_RawCounterConfig_Destroy(&destroy);

    NVPW_D3D12_Profiler_CalcTraceBufferSize_Params traceSize{ NVPW_D3D12_Profiler_CalcTraceBufferSize_Params_STRUCT_SIZE };
    traceSize.maxRangesPerPass = profiler.maxRangesPerPass;
    traceSize.avgRangeNameLength = 96;
    if (LogIfFailed(NVPW_D3D12_Profiler_CalcTraceBufferSize(&traceSize), "NVPW_D3D12_Profiler_CalcTraceBufferSize")) {
        return false;
    }
    profiler.traceBufferSize = traceSize.traceBufferSize;

    spdlog::info(
        "NVPerf: pass capture configured chip='{}' metrics={} rawCounters={} configPasses={} maxRangesPerPass={}",
        chipName,
        profiler.metrics.size(),
        rawCounterRequests.size(),
        profiler.configPassCount,
        profiler.maxRangesPerPass);
    return true;
}

bool InitializeQueueCapture(D3D12PassProfiler& profiler, QueueCapture& queueCapture)
{
    NVPW_D3D12_Profiler_CounterDataImageOptions options{ NVPW_D3D12_Profiler_CounterDataImageOptions_STRUCT_SIZE };
    options.pCounterDataPrefix = profiler.counterDataPrefix.data();
    options.counterDataPrefixSize = profiler.counterDataPrefix.size();
    options.maxNumRanges = static_cast<uint32_t>(profiler.maxRangesPerPass);
    options.maxNumRangeTreeNodes = static_cast<uint32_t>(profiler.maxRangesPerPass);
    options.maxRangeNameLength = 128;

    NVPW_D3D12_Profiler_CounterDataImage_CalculateSize_Params imageSize{ NVPW_D3D12_Profiler_CounterDataImage_CalculateSize_Params_STRUCT_SIZE };
    imageSize.counterDataImageOptionsSize = NVPW_D3D12_Profiler_CounterDataImageOptions_STRUCT_SIZE;
    imageSize.pOptions = &options;
    if (LogIfFailed(NVPW_D3D12_Profiler_CounterDataImage_CalculateSize(&imageSize), "NVPW_D3D12_Profiler_CounterDataImage_CalculateSize")) {
        return false;
    }

    queueCapture.counterDataImage.assign(imageSize.counterDataImageSize, 0);
    NVPW_D3D12_Profiler_CounterDataImage_Initialize_Params imageInit{ NVPW_D3D12_Profiler_CounterDataImage_Initialize_Params_STRUCT_SIZE };
    imageInit.counterDataImageOptionsSize = NVPW_D3D12_Profiler_CounterDataImageOptions_STRUCT_SIZE;
    imageInit.pOptions = &options;
    imageInit.counterDataImageSize = queueCapture.counterDataImage.size();
    imageInit.pCounterDataImage = queueCapture.counterDataImage.data();
    if (LogIfFailed(NVPW_D3D12_Profiler_CounterDataImage_Initialize(&imageInit), "NVPW_D3D12_Profiler_CounterDataImage_Initialize")) {
        return false;
    }

    NVPW_D3D12_Profiler_CounterDataImage_CalculateScratchBufferSize_Params scratchSize{ NVPW_D3D12_Profiler_CounterDataImage_CalculateScratchBufferSize_Params_STRUCT_SIZE };
    scratchSize.counterDataImageSize = queueCapture.counterDataImage.size();
    scratchSize.pCounterDataImage = queueCapture.counterDataImage.data();
    if (LogIfFailed(NVPW_D3D12_Profiler_CounterDataImage_CalculateScratchBufferSize(&scratchSize), "NVPW_D3D12_Profiler_CounterDataImage_CalculateScratchBufferSize")) {
        return false;
    }

    queueCapture.counterDataScratch.assign(scratchSize.counterDataScratchBufferSize, 0);
    NVPW_D3D12_Profiler_CounterDataImage_InitializeScratchBuffer_Params scratchInit{ NVPW_D3D12_Profiler_CounterDataImage_InitializeScratchBuffer_Params_STRUCT_SIZE };
    scratchInit.counterDataImageSize = queueCapture.counterDataImage.size();
    scratchInit.pCounterDataImage = queueCapture.counterDataImage.data();
    scratchInit.counterDataScratchBufferSize = queueCapture.counterDataScratch.size();
    scratchInit.pCounterDataScratchBuffer = queueCapture.counterDataScratch.data();
    if (LogIfFailed(NVPW_D3D12_Profiler_CounterDataImage_InitializeScratchBuffer(&scratchInit), "NVPW_D3D12_Profiler_CounterDataImage_InitializeScratchBuffer")) {
        return false;
    }

    NVPW_D3D12_Profiler_Queue_BeginSession_Params beginSession{ NVPW_D3D12_Profiler_Queue_BeginSession_Params_STRUCT_SIZE };
    beginSession.pCommandQueue = queueCapture.nativeQueue;
    beginSession.numTraceBuffers = profiler.traceBufferCount;
    beginSession.traceBufferSize = profiler.traceBufferSize;
    beginSession.maxRangesPerPass = profiler.maxRangesPerPass;
    beginSession.maxLaunchesPerPass = 0;
    if (LogIfFailed(NVPW_D3D12_Profiler_Queue_BeginSession(&beginSession), "NVPW_D3D12_Profiler_Queue_BeginSession")) {
        return false;
    }

    queueCapture.sessionActive = true;
    spdlog::info("NVPerf: pass capture session began for queue '{}'", queueCapture.queueName);
    return true;
}

bool EnsureQueuePassActive(D3D12PassProfiler& profiler, QueueCapture& queueCapture)
{
    if (queueCapture.allPassesSubmitted || queueCapture.decoded) {
        return false;
    }

    if (!queueCapture.sessionActive && !InitializeQueueCapture(profiler, queueCapture)) {
        profiler.failed = true;
        return false;
    }

    if (queueCapture.passActive) {
        return true;
    }

    NVPW_D3D12_Profiler_Queue_SetConfig_Params setConfig{ NVPW_D3D12_Profiler_Queue_SetConfig_Params_STRUCT_SIZE };
    setConfig.pCommandQueue = queueCapture.nativeQueue;
    setConfig.pConfig = profiler.configImage.data();
    setConfig.configSize = profiler.configImage.size();
    setConfig.minNestingLevel = 1;
    setConfig.numNestingLevels = 1;
    setConfig.passIndex = queueCapture.nextPassIndex;
    setConfig.targetNestingLevel = queueCapture.targetNestingLevel;
    spdlog::info(
        "NVPerf: queue '{}' setting profiler config passIndex={} targetNestingLevel={}",
        queueCapture.queueName,
        setConfig.passIndex,
        setConfig.targetNestingLevel);
    if (LogIfFailed(NVPW_D3D12_Profiler_Queue_SetConfig(&setConfig), "NVPW_D3D12_Profiler_Queue_SetConfig")) {
        profiler.failed = true;
        return false;
    }

    NVPW_D3D12_Profiler_Queue_BeginPass_Params beginPass{ NVPW_D3D12_Profiler_Queue_BeginPass_Params_STRUCT_SIZE };
    beginPass.pCommandQueue = queueCapture.nativeQueue;
    spdlog::info("NVPerf: queue '{}' beginning profiler pass", queueCapture.queueName);
    if (LogIfFailed(NVPW_D3D12_Profiler_Queue_BeginPass(&beginPass), "NVPW_D3D12_Profiler_Queue_BeginPass")) {
        profiler.failed = true;
        return false;
    }

    queueCapture.passActive = true;
    return true;
}

std::string GetRangeName(const std::vector<uint8_t>& counterDataImage, size_t rangeIndex)
{
    NVPW_Profiler_CounterData_GetRangeDescriptions_Params desc{ NVPW_Profiler_CounterData_GetRangeDescriptions_Params_STRUCT_SIZE };
    desc.pCounterDataImage = counterDataImage.data();
    desc.rangeIndex = rangeIndex;
    if (LogIfFailed(NVPW_Profiler_CounterData_GetRangeDescriptions(&desc), "NVPW_Profiler_CounterData_GetRangeDescriptions(count)")) {
        return {};
    }
    std::vector<const char*> descriptions(desc.numDescriptions);
    desc.ppDescriptions = descriptions.data();
    desc.numDescriptions = descriptions.size();
    if (LogIfFailed(NVPW_Profiler_CounterData_GetRangeDescriptions(&desc), "NVPW_Profiler_CounterData_GetRangeDescriptions(values)")) {
        return {};
    }
    if (descriptions.empty() || descriptions.back() == nullptr) {
        return {};
    }
    return descriptions.back();
}

bool AppendQueueCsvRows(D3D12PassProfiler& profiler, const QueueCapture& queueCapture)
{
    NVPW_CounterData_GetNumRanges_Params numRanges{ NVPW_CounterData_GetNumRanges_Params_STRUCT_SIZE };
    numRanges.pCounterDataImage = queueCapture.counterDataImage.data();
    if (LogIfFailed(NVPW_CounterData_GetNumRanges(&numRanges), "NVPW_CounterData_GetNumRanges")) {
        return false;
    }

    if (numRanges.numRanges == 0) {
        spdlog::warn("NVPerf: no ranges were collected for queue '{}'", queueCapture.queueName);
        return true;
    }

    NVPW_D3D12_MetricsEvaluator_CalculateScratchBufferSize_Params scratchParams{ NVPW_D3D12_MetricsEvaluator_CalculateScratchBufferSize_Params_STRUCT_SIZE };
    scratchParams.pChipName = profiler.chipName.c_str();
    if (LogIfFailed(NVPW_D3D12_MetricsEvaluator_CalculateScratchBufferSize(&scratchParams), "NVPW_D3D12_MetricsEvaluator_CalculateScratchBufferSize(csv)")) {
        return false;
    }
    std::vector<uint8_t> scratch(scratchParams.scratchBufferSize);

    NVPW_D3D12_MetricsEvaluator_Initialize_Params evalInit{ NVPW_D3D12_MetricsEvaluator_Initialize_Params_STRUCT_SIZE };
    evalInit.pScratchBuffer = scratch.data();
    evalInit.scratchBufferSize = scratch.size();
    evalInit.pCounterDataImage = queueCapture.counterDataImage.data();
    evalInit.counterDataImageSize = queueCapture.counterDataImage.size();
    if (LogIfFailed(NVPW_D3D12_MetricsEvaluator_Initialize(&evalInit), "NVPW_D3D12_MetricsEvaluator_Initialize(csv)")) {
        return false;
    }
    NVPW_MetricsEvaluator* evaluator = evalInit.pMetricsEvaluator;

    std::ofstream out;
    if (!profiler.csvPath.empty()) {
        const std::filesystem::path parentPath = profiler.csvPath.parent_path();
        if (!parentPath.empty()) {
            std::filesystem::create_directories(parentPath);
        }
        const bool writeHeader = !std::filesystem::exists(profiler.csvPath) || std::filesystem::file_size(profiler.csvPath) == 0;
        out.open(profiler.csvPath, std::ios::app);
        if (!out) {
            spdlog::warn("NVPerf: failed to open CSV '{}'", profiler.csvPath.string());
            return false;
        }
        if (writeHeader) {
            out << "capture_start_frame,queue,range_index,pass_name";
            for (const auto& metric : profiler.metrics) {
                out << ',' << CsvEscape(metric.spec.outputName.empty() ? metric.spec.name : metric.spec.outputName);
            }
            out << '\n';
        }
    }

    std::vector<NVPW_MetricEvalRequest> requests;
    requests.reserve(profiler.metrics.size());
    for (const auto& metric : profiler.metrics) {
        requests.push_back(metric.request);
    }
    std::vector<double> values(requests.size(), 0.0);

    std::unordered_map<std::string, uint32_t> occurrences;
    for (size_t rangeIndex = 0; rangeIndex < numRanges.numRanges; ++rangeIndex) {
        std::fill(values.begin(), values.end(), 0.0);
        NVPW_MetricsEvaluator_EvaluateToGpuValues_Params eval{ NVPW_MetricsEvaluator_EvaluateToGpuValues_Params_STRUCT_SIZE };
        eval.pMetricsEvaluator = evaluator;
        eval.pMetricEvalRequests = requests.data();
        eval.numMetricEvalRequests = requests.size();
        eval.metricEvalRequestStructSize = NVPW_MetricEvalRequest_STRUCT_SIZE;
        eval.metricEvalRequestStrideSize = sizeof(NVPW_MetricEvalRequest);
        eval.pCounterDataImage = queueCapture.counterDataImage.data();
        eval.counterDataImageSize = queueCapture.counterDataImage.size();
        eval.rangeIndex = rangeIndex;
        eval.pMetricValues = values.data();
        if (LogIfFailed(NVPW_MetricsEvaluator_EvaluateToGpuValues(&eval), "NVPW_MetricsEvaluator_EvaluateToGpuValues")) {
            continue;
        }

        const std::string rangeName = GetRangeName(queueCapture.counterDataImage, rangeIndex);
        const std::string resolvedRangeName = rangeName.empty() ? "<unnamed>" : rangeName;
        const std::string occurrenceKey = queueCapture.queueName + "\n" + resolvedRangeName;
        RangeResult result;
        result.queue = queueCapture.queueName;
        result.passName = resolvedRangeName;
        result.occurrence = occurrences[occurrenceKey]++;
        result.rangeIndex = static_cast<uint32_t>(rangeIndex);
        result.values = values;
        if (!profiler.result) {
            profiler.result.emplace();
        }
        profiler.result->ranges.push_back(std::move(result));

        if (out) {
            out << profiler.captureStartFrame
                << ',' << CsvEscape(queueCapture.queueName)
                << ',' << rangeIndex
                << ',' << CsvEscape(resolvedRangeName);
            for (double value : values) {
                out << ',' << value;
            }
            out << '\n';
        }
    }

    NVPW_MetricsEvaluator_Destroy_Params destroy{ NVPW_MetricsEvaluator_Destroy_Params_STRUCT_SIZE };
    destroy.pMetricsEvaluator = evaluator;
    (void)NVPW_MetricsEvaluator_Destroy(&destroy);

    spdlog::info(
        "NVPerf: wrote {} pass metric rows for queue '{}' to '{}'",
        numRanges.numRanges,
        queueCapture.queueName,
        profiler.csvPath.empty() ? "<structured result>" : profiler.csvPath.string());
    return true;
}

bool DecodeQueueCapture(D3D12PassProfiler& profiler, QueueCapture& queueCapture)
{
    size_t decodeIterations = 0;
    for (;;) {
        NVPW_D3D12_Profiler_Queue_DecodeCounters_Params decode{ NVPW_D3D12_Profiler_Queue_DecodeCounters_Params_STRUCT_SIZE };
        decode.pCommandQueue = queueCapture.nativeQueue;
        decode.counterDataImageSize = queueCapture.counterDataImage.size();
        decode.pCounterDataImage = queueCapture.counterDataImage.data();
        decode.counterDataScratchBufferSize = queueCapture.counterDataScratch.size();
        decode.pCounterDataScratchBuffer = queueCapture.counterDataScratch.data();
        if (LogIfFailed(NVPW_D3D12_Profiler_Queue_DecodeCounters(&decode), "NVPW_D3D12_Profiler_Queue_DecodeCounters")) {
            return false;
        }

        if (decode.numRangesDropped > 0 || decode.numTraceBytesDropped > 0) {
            profiler.droppedRanges += decode.numRangesDropped;
            profiler.droppedTraceBytes += decode.numTraceBytesDropped;
            spdlog::warn(
                "NVPerf: queue '{}' decode dropped ranges={} traceBytes={}",
                queueCapture.queueName,
                decode.numRangesDropped,
                decode.numTraceBytesDropped);
        }
        if (decode.allPassesCollected) {
            break;
        }
        if (decode.onePassCollected) {
            spdlog::debug(
                "NVPerf: queue '{}' decoded profiler pass {}",
                queueCapture.queueName,
                decode.passIndexDecoded);
        }
        else {
            ServicePendingGpuOperationsForQueue(queueCapture, "Decode", 16, 10);
        }
        if (++decodeIterations > 2048) {
            spdlog::warn("NVPerf: queue '{}' decode exceeded iteration guard", queueCapture.queueName);
            return false;
        }
    }

    if (!AppendQueueCsvRows(profiler, queueCapture)) {
        return false;
    }
    queueCapture.decoded = true;
    return true;
}

bool PrepareProfilerIfRequested(rhi::Backend backend, rhi::Device device, rhi::Queue graphicsQueue, uint64_t frameNumber)
{
    auto& profiler = Profiler();
    if (!profiler.envChecked) {
        profiler.envChecked = true;
        if (!profiler.programmaticConfigured) {
            profiler.requested = IsTruthyEnv("BASICRENDERER_NVPERF_CAPTURE");
            profiler.armed = profiler.requested;
        }
        std::string queueOverride = ReadEnvString("BASICRENDERER_NVPERF_QUEUE");
        std::transform(queueOverride.begin(), queueOverride.end(), queueOverride.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (queueOverride == "compute" || queueOverride == "async_compute" || queueOverride == "async-compute") {
            profiler.controllerQueueName = "Compute";
        }
        else if (!queueOverride.empty() && queueOverride != "graphics" && queueOverride != "direct") {
            spdlog::warn("NVPerf: unrecognized BASICRENDERER_NVPERF_QUEUE='{}'; using Graphics", queueOverride);
        }
        profiler.captureStartDelayFrames = ReadEnvSizeT("BASICRENDERER_NVPERF_START_FRAME", profiler.captureStartDelayFrames);
        profiler.traceBufferCount = ReadEnvSizeT("BASICRENDERER_NVPERF_TRACE_BUFFERS", 5);
        profiler.syncTimeoutMs = static_cast<uint32_t>(ReadEnvSizeT("BASICRENDERER_NVPERF_SYNC_TIMEOUT_MS", profiler.syncTimeoutMs));
        const std::string csvOverride = ReadEnvString("BASICRENDERER_NVPERF_CSV");
        profiler.csvPath = csvOverride.empty()
            ? std::filesystem::path("logs") / "nvperf_pass_metrics.csv"
            : std::filesystem::path(csvOverride);
    }

    if (!profiler.requested || !profiler.armed || profiler.failed || profiler.finished) {
        return false;
    }
    if (!profiler.programmaticConfigured && frameNumber < profiler.captureStartDelayFrames) {
        return false;
    }
    if (backend != rhi::Backend::D3D12) {
        if (!profiler.failed) {
            spdlog::warn("NVPerf: pass metric capture is currently implemented for D3D12 only");
        }
        profiler.failed = true;
        return false;
    }
    if (profiler.initialized) {
        return true;
    }
    if (!InitializeNvPerf()) {
        profiler.failed = true;
        return false;
    }

    ID3D12Device* nativeDevice = rhi::dx12::get_device(device);
    ID3D12CommandQueue* nativeGraphicsQueue = rhi::dx12::get_queue(graphicsQueue);
    if (!nativeDevice || !nativeGraphicsQueue) {
        spdlog::warn("NVPerf: pass metric capture could not get native D3D12 device/queue");
        profiler.failed = true;
        return false;
    }
    profiler.nativeDevice = nativeDevice;

    NVPW_D3D12_LoadDriver_Params loadParams{ NVPW_D3D12_LoadDriver_Params_STRUCT_SIZE };
    if (LogIfFailed(NVPW_D3D12_LoadDriver(&loadParams), "NVPW_D3D12_LoadDriver(pass capture)")) {
        profiler.failed = true;
        return false;
    }

    NVPW_D3D12_Device_GetDeviceIndex_Params indexParams{ NVPW_D3D12_Device_GetDeviceIndex_Params_STRUCT_SIZE };
    indexParams.pDevice = nativeDevice;
    if (LogIfFailed(NVPW_D3D12_Device_GetDeviceIndex(&indexParams), "NVPW_D3D12_Device_GetDeviceIndex(pass capture)")) {
        profiler.failed = true;
        return false;
    }

    NVPW_Device_GetNames_Params namesParams{ NVPW_Device_GetNames_Params_STRUCT_SIZE };
    namesParams.deviceIndex = indexParams.deviceIndex;
    if (LogIfFailed(NVPW_Device_GetNames(&namesParams), "NVPW_Device_GetNames(pass capture)") || !namesParams.pChipName) {
        profiler.failed = true;
        return false;
    }
    profiler.chipName = namesParams.pChipName;

    NVPW_D3D12_Profiler_Queue_GetCounterAvailability_Params availabilitySize{ NVPW_D3D12_Profiler_Queue_GetCounterAvailability_Params_STRUCT_SIZE };
    availabilitySize.pCommandQueue = nativeGraphicsQueue;
    if (LogIfFailed(NVPW_D3D12_Profiler_Queue_GetCounterAvailability(&availabilitySize), "NVPW_D3D12_Profiler_Queue_GetCounterAvailability(size)")) {
        profiler.failed = true;
        return false;
    }
    std::vector<uint8_t> counterAvailability(availabilitySize.counterAvailabilityImageSize);
    const uint8_t* counterAvailabilityImage = nullptr;
    size_t counterAvailabilityImageSize = 0;
    NVPW_D3D12_Profiler_Queue_GetCounterAvailability_Params availability{ NVPW_D3D12_Profiler_Queue_GetCounterAvailability_Params_STRUCT_SIZE };
    availability.pCommandQueue = nativeGraphicsQueue;
    availability.counterAvailabilityImageSize = counterAvailability.size();
    availability.pCounterAvailabilityImage = counterAvailability.data();
    const NVPA_Status availabilityStatus = NVPW_D3D12_Profiler_Queue_GetCounterAvailability(&availability);
    if (availabilityStatus == NVPA_STATUS_SUCCESS) {
        counterAvailabilityImage = counterAvailability.data();
        counterAvailabilityImageSize = availability.counterAvailabilityImageSize;
    }
    else {
        spdlog::warn(
            "NVPerf: counter availability image query failed with {}; continuing without availability filtering",
            StatusName(availabilityStatus));
    }

    if (!BuildD3D12ProfilerConfig(profiler, profiler.chipName.c_str(), counterAvailabilityImage, counterAvailabilityImageSize)) {
        profiler.failed = true;
        profiler.error = "failed to build NVPerf metric configuration";
        return false;
    }
    for (const auto& unsupported : profiler.unsupportedMetrics) {
        if (unsupported.required) {
            profiler.failed = true;
            profiler.error = "required NVPerf metric is unavailable: " + unsupported.name;
            return false;
        }
    }

    profiler.captureStartFrame = frameNumber;
    profiler.traceBufferCount = std::max({ profiler.traceBufferCount, profiler.configPassCount, size_t{ 5 } });
    profiler.initialized = true;
    spdlog::info(
        "NVPerf: pass metric capture armed at frame {} queue='{}' traceBuffers={} output='{}'",
        frameNumber,
        profiler.controllerQueueName,
        profiler.traceBufferCount,
        profiler.csvPath.string());
    return true;
}

bool InitializeNvPerf()
{
    static bool initialized = false;
    static bool available = false;

    if (initialized) {
        return available;
    }
    initialized = true;

    NVPW_InitializeHost_Params hostParams{ NVPW_InitializeHost_Params_STRUCT_SIZE };
    const NVPA_Status hostStatus = NVPW_InitializeHost(&hostParams);
    if (LogIfFailed(hostStatus, "NVPW_InitializeHost")) {
        return false;
    }

    NVPW_InitializeTarget_Params targetParams{ NVPW_InitializeTarget_Params_STRUCT_SIZE };
    const NVPA_Status targetStatus = NVPW_InitializeTarget(&targetParams);
    if (LogIfFailed(targetStatus, "NVPW_InitializeTarget")) {
        return false;
    }

    available = true;
    spdlog::info("NVPerf: host and target libraries initialized");
    return true;
}

void LogHostMetricSmokeTest(rhi::Backend backend)
{
    NVPW_GetSupportedChipNames_Params chipParams{ NVPW_GetSupportedChipNames_Params_STRUCT_SIZE };
    if (LogIfFailed(NVPW_GetSupportedChipNames(&chipParams), "NVPW_GetSupportedChipNames")) {
        return;
    }

    const char* firstChip = chipParams.numChipNames > 0 && chipParams.ppChipNames ? chipParams.ppChipNames[0] : nullptr;
    spdlog::info(
        "NVPerf: supported virtual chip count={} firstChip='{}'",
        chipParams.numChipNames,
        firstChip ? firstChip : "<none>");

    if (!firstChip) {
        return;
    }

    size_t scratchBufferSize = 0;
    if (backend == rhi::Backend::Vulkan) {
        NVPW_VK_MetricsEvaluator_CalculateScratchBufferSize_Params scratchParams{ NVPW_VK_MetricsEvaluator_CalculateScratchBufferSize_Params_STRUCT_SIZE };
        scratchParams.pChipName = firstChip;
        if (LogIfFailed(NVPW_VK_MetricsEvaluator_CalculateScratchBufferSize(&scratchParams), "NVPW_VK_MetricsEvaluator_CalculateScratchBufferSize")) {
            return;
        }
        scratchBufferSize = scratchParams.scratchBufferSize;
    }
    else {
        NVPW_D3D12_MetricsEvaluator_CalculateScratchBufferSize_Params scratchParams{ NVPW_D3D12_MetricsEvaluator_CalculateScratchBufferSize_Params_STRUCT_SIZE };
        scratchParams.pChipName = firstChip;
        if (LogIfFailed(NVPW_D3D12_MetricsEvaluator_CalculateScratchBufferSize(&scratchParams), "NVPW_D3D12_MetricsEvaluator_CalculateScratchBufferSize")) {
            return;
        }
        scratchBufferSize = scratchParams.scratchBufferSize;
    }

    std::vector<uint8_t> scratch(scratchBufferSize);
    NVPW_MetricsEvaluator* evaluator = nullptr;
    if (backend == rhi::Backend::Vulkan) {
        NVPW_VK_MetricsEvaluator_Initialize_Params initParams{ NVPW_VK_MetricsEvaluator_Initialize_Params_STRUCT_SIZE };
        initParams.pScratchBuffer = scratch.data();
        initParams.scratchBufferSize = scratch.size();
        initParams.pChipName = firstChip;
        if (LogIfFailed(NVPW_VK_MetricsEvaluator_Initialize(&initParams), "NVPW_VK_MetricsEvaluator_Initialize")) {
            return;
        }
        evaluator = initParams.pMetricsEvaluator;
    }
    else {
        NVPW_D3D12_MetricsEvaluator_Initialize_Params initParams{ NVPW_D3D12_MetricsEvaluator_Initialize_Params_STRUCT_SIZE };
        initParams.pScratchBuffer = scratch.data();
        initParams.scratchBufferSize = scratch.size();
        initParams.pChipName = firstChip;
        if (LogIfFailed(NVPW_D3D12_MetricsEvaluator_Initialize(&initParams), "NVPW_D3D12_MetricsEvaluator_Initialize")) {
            return;
        }
        evaluator = initParams.pMetricsEvaluator;
    }

    NVPW_MetricsEvaluator_GetMetricNames_Params metricParams{ NVPW_MetricsEvaluator_GetMetricNames_Params_STRUCT_SIZE };
    metricParams.pMetricsEvaluator = evaluator;
    if (!LogIfFailed(NVPW_MetricsEvaluator_GetMetricNames(&metricParams), "NVPW_MetricsEvaluator_GetMetricNames")) {
        const char* firstMetric = metricParams.numMetrics > 0 && metricParams.pMetricNames && metricParams.pMetricNameBeginIndices
            ? metricParams.pMetricNames + metricParams.pMetricNameBeginIndices[0]
            : nullptr;
        spdlog::info(
            "NVPerf: metrics evaluator ready for backend={} metricCount={} firstMetric='{}'",
            backend == rhi::Backend::Vulkan ? "Vulkan" : "D3D12",
            metricParams.numMetrics,
            firstMetric ? firstMetric : "<none>");
    }

    NVPW_MetricsEvaluator_Destroy_Params destroyParams{ NVPW_MetricsEvaluator_Destroy_Params_STRUCT_SIZE };
    destroyParams.pMetricsEvaluator = evaluator;
    (void)NVPW_MetricsEvaluator_Destroy(&destroyParams);
}

void LogD3D12DeviceProbe(rhi::Device device, rhi::Queue graphicsQueue)
{
    ID3D12Device* nativeDevice = rhi::dx12::get_device(device);
    ID3D12CommandQueue* nativeQueue = rhi::dx12::get_queue(graphicsQueue);
    if (!nativeDevice || !nativeQueue) {
        spdlog::warn("NVPerf: D3D12 native device or graphics queue is unavailable");
        return;
    }

    NVPW_D3D12_LoadDriver_Params loadParams{ NVPW_D3D12_LoadDriver_Params_STRUCT_SIZE };
    if (LogIfFailed(NVPW_D3D12_LoadDriver(&loadParams), "NVPW_D3D12_LoadDriver")) {
        return;
    }

    NVPW_D3D12_Device_GetDeviceIndex_Params indexParams{ NVPW_D3D12_Device_GetDeviceIndex_Params_STRUCT_SIZE };
    indexParams.pDevice = nativeDevice;
    if (LogIfFailed(NVPW_D3D12_Device_GetDeviceIndex(&indexParams), "NVPW_D3D12_Device_GetDeviceIndex")) {
        return;
    }

    NVPW_Device_GetNames_Params namesParams{ NVPW_Device_GetNames_Params_STRUCT_SIZE };
    namesParams.deviceIndex = indexParams.deviceIndex;
    const NVPA_Status namesStatus = NVPW_Device_GetNames(&namesParams);

    NVPW_D3D12_Profiler_IsGpuSupported_Params supportParams{ NVPW_D3D12_Profiler_IsGpuSupported_Params_STRUCT_SIZE };
    supportParams.deviceIndex = indexParams.deviceIndex;
    const NVPA_Status supportStatus = NVPW_D3D12_Profiler_IsGpuSupported(&supportParams);

    NVPW_D3D12_Profiler_Queue_GetCounterAvailability_Params availabilityParams{ NVPW_D3D12_Profiler_Queue_GetCounterAvailability_Params_STRUCT_SIZE };
    availabilityParams.pCommandQueue = nativeQueue;
    const NVPA_Status availabilityStatus = NVPW_D3D12_Profiler_Queue_GetCounterAvailability(&availabilityParams);

    spdlog::info(
        "NVPerf: D3D12 probe deviceIndex={} device='{}' chip='{}' namesStatus={} profilerSupported={} supportStatus={} arch={} sli={} cmp={} wsl={} sku={} counterAvailabilityStatus={} counterAvailabilityBytes={}",
        indexParams.deviceIndex,
        namesStatus == NVPA_STATUS_SUCCESS && namesParams.pDeviceName ? namesParams.pDeviceName : "<unknown>",
        namesStatus == NVPA_STATUS_SUCCESS && namesParams.pChipName ? namesParams.pChipName : "<unknown>",
        StatusName(namesStatus),
        supportStatus == NVPA_STATUS_SUCCESS && supportParams.isSupported,
        StatusName(supportStatus),
        static_cast<uint32_t>(supportParams.gpuArchitectureSupportLevel),
        static_cast<uint32_t>(supportParams.sliSupportLevel),
        static_cast<uint32_t>(supportParams.cmpSupportLevel),
        static_cast<uint32_t>(supportParams.wslSupportLevel),
        static_cast<uint32_t>(supportParams.skuSupportLevel),
        StatusName(availabilityStatus),
        availabilityParams.counterAvailabilityImageSize);
}

void LogVulkanDeviceProbe(rhi::Device device, rhi::Queue graphicsQueue)
{
#if BASICRHI_HAS_VULKAN_HEADERS
    VkInstance instance = rhi::vulkan::get_instance(device);
    VkPhysicalDevice physicalDevice = rhi::vulkan::get_physical_device(device);
    VkDevice nativeDevice = rhi::vulkan::get_device(device);
    VkQueue nativeQueue = rhi::vulkan::get_queue(graphicsQueue);
    auto getDeviceProcAddr = rhi::vulkan::get_device_proc_addr();
    if (!instance || !physicalDevice || !nativeDevice || !nativeQueue || !getDeviceProcAddr) {
        spdlog::warn("NVPerf: Vulkan native handles are unavailable");
        return;
    }

    NVPW_VK_LoadDriver_Params loadParams{ NVPW_VK_LoadDriver_Params_STRUCT_SIZE };
    loadParams.instance = instance;
    if (LogIfFailed(NVPW_VK_LoadDriver(&loadParams), "NVPW_VK_LoadDriver")) {
        return;
    }

    NVPW_VK_Device_GetDeviceIndex_Params indexParams{ NVPW_VK_Device_GetDeviceIndex_Params_STRUCT_SIZE };
    indexParams.instance = instance;
    indexParams.physicalDevice = physicalDevice;
    indexParams.device = nativeDevice;
    indexParams.pfnGetDeviceProcAddr = reinterpret_cast<void*>(getDeviceProcAddr);
    if (LogIfFailed(NVPW_VK_Device_GetDeviceIndex(&indexParams), "NVPW_VK_Device_GetDeviceIndex")) {
        return;
    }

    NVPW_Device_GetNames_Params namesParams{ NVPW_Device_GetNames_Params_STRUCT_SIZE };
    namesParams.deviceIndex = indexParams.deviceIndex;
    const NVPA_Status namesStatus = NVPW_Device_GetNames(&namesParams);

    NVPW_VK_Profiler_IsGpuSupported_Params supportParams{ NVPW_VK_Profiler_IsGpuSupported_Params_STRUCT_SIZE };
    supportParams.deviceIndex = indexParams.deviceIndex;
    const NVPA_Status supportStatus = NVPW_VK_Profiler_IsGpuSupported(&supportParams);

    NVPW_VK_Profiler_Queue_GetCounterAvailability_Params availabilityParams{ NVPW_VK_Profiler_Queue_GetCounterAvailability_Params_STRUCT_SIZE };
    availabilityParams.instance = instance;
    availabilityParams.physicalDevice = physicalDevice;
    availabilityParams.device = nativeDevice;
    availabilityParams.queue = nativeQueue;
    availabilityParams.pfnGetDeviceProcAddr = reinterpret_cast<void*>(getDeviceProcAddr);
    const NVPA_Status availabilityStatus = NVPW_VK_Profiler_Queue_GetCounterAvailability(&availabilityParams);

    spdlog::info(
        "NVPerf: Vulkan probe deviceIndex={} device='{}' chip='{}' namesStatus={} profilerSupported={} supportStatus={} arch={} sli={} cmp={} wsl={} sku={} counterAvailabilityStatus={} counterAvailabilityBytes={}",
        indexParams.deviceIndex,
        namesStatus == NVPA_STATUS_SUCCESS && namesParams.pDeviceName ? namesParams.pDeviceName : "<unknown>",
        namesStatus == NVPA_STATUS_SUCCESS && namesParams.pChipName ? namesParams.pChipName : "<unknown>",
        StatusName(namesStatus),
        supportStatus == NVPA_STATUS_SUCCESS && supportParams.isSupported,
        StatusName(supportStatus),
        static_cast<uint32_t>(supportParams.gpuArchitectureSupportLevel),
        static_cast<uint32_t>(supportParams.sliSupportLevel),
        static_cast<uint32_t>(supportParams.cmpSupportLevel),
        static_cast<uint32_t>(supportParams.wslSupportLevel),
        static_cast<uint32_t>(supportParams.skuSupportLevel),
        StatusName(availabilityStatus),
        availabilityParams.counterAvailabilityImageSize);
#else
    (void)device;
    (void)graphicsQueue;
    spdlog::warn("NVPerf: Vulkan probe is unavailable because Vulkan headers are not enabled");
#endif
}
#endif

} // namespace

void LogStartupProbe(rhi::Backend backend, rhi::Device device, rhi::Queue graphicsQueue)
{
#if BASICRENDERER_ENABLE_NVPERF
    if (!InitializeNvPerf()) {
        return;
    }

    LogHostMetricSmokeTest(backend);

    switch (backend) {
    case rhi::Backend::D3D12:
        LogD3D12DeviceProbe(device, graphicsQueue);
        break;
    case rhi::Backend::Vulkan:
        LogVulkanDeviceProbe(device, graphicsQueue);
        break;
    default:
        spdlog::warn("NVPerf: skipping live device probe for unsupported backend {}", static_cast<uint32_t>(backend));
        break;
    }
#else
    (void)backend;
    (void)device;
    (void)graphicsQueue;
#endif
}

bool ConfigureCapture(const CaptureConfiguration& configuration, std::string& error)
{
#if BASICRENDERER_ENABLE_NVPERF
    auto& profiler = Profiler();
    std::lock_guard<std::mutex> lock(profiler.mutex);
    const bool sessionActive = std::ranges::any_of(profiler.queues, [](const auto& entry) {
        return entry.second.sessionActive || entry.second.passActive;
    });
    if (profiler.armed || sessionActive) {
        error = "NVPerf capture is active";
        return false;
    }
    if (configuration.metrics.empty()) {
        error = "at least one NVPerf metric is required";
        return false;
    }

    profiler.programmaticConfigured = true;
    profiler.envChecked = true;
    profiler.requested = false;
    profiler.armed = false;
    profiler.initialized = false;
    profiler.failed = false;
    profiler.finished = false;
    profiler.result.reset();
    profiler.requestedMetrics.clear();
    profiler.passFilters = configuration.passes;
    profiler.controllerQueueName = configuration.controllerQueue.empty() ? "Graphics" : configuration.controllerQueue;
    profiler.syncTimeoutMs = configuration.syncTimeoutMs;
    profiler.csvPath.clear();
    for (const auto& metric : configuration.metrics) {
        if (metric.name.empty()) {
            error = "NVPerf metric name cannot be empty";
            return false;
        }
        profiler.requestedMetrics.push_back({
            metric.name,
            metric.metricType,
            metric.rollupOp,
            metric.submetric,
            metric.outputName,
            metric.id,
            metric.unit,
            metric.required
        });
    }
    return true;
#else
    (void)configuration;
    error = "BasicRenderer was built without NVPerf support";
    return false;
#endif
}

bool ArmCapture(uint64_t sampleId, std::string& error)
{
#if BASICRENDERER_ENABLE_NVPERF
    auto& profiler = Profiler();
    std::lock_guard<std::mutex> lock(profiler.mutex);
    if (!profiler.programmaticConfigured) {
        error = "NVPerf capture has not been configured";
        return false;
    }
    if (profiler.armed && !profiler.finished && !profiler.failed) {
        error = "NVPerf capture is already armed";
        return false;
    }
    for (auto& [_, queueCapture] : profiler.queues) {
        ReleaseQueueSyncObjects(queueCapture);
    }
    profiler.queues.clear();
    profiler.activeCommandListRanges.clear();
    profiler.requested = true;
    profiler.armed = true;
    profiler.initialized = false;
    profiler.failed = false;
    profiler.finished = false;
    profiler.sampleId = sampleId;
    profiler.captureStartFrame = 0;
    profiler.captureEndFrame = 0;
    profiler.droppedRanges = 0;
    profiler.droppedTraceBytes = 0;
    profiler.error.clear();
    profiler.result.emplace();
    profiler.result->sampleId = sampleId;
    return true;
#else
    (void)sampleId;
    error = "BasicRenderer was built without NVPerf support";
    return false;
#endif
}

bool CaptureConfigured()
{
#if BASICRENDERER_ENABLE_NVPERF
    auto& profiler = Profiler();
    std::lock_guard<std::mutex> lock(profiler.mutex);
    return profiler.programmaticConfigured;
#else
    return false;
#endif
}

bool CaptureComplete()
{
#if BASICRENDERER_ENABLE_NVPERF
    auto& profiler = Profiler();
    std::lock_guard<std::mutex> lock(profiler.mutex);
    return profiler.armed && (profiler.finished || profiler.failed);
#else
    return false;
#endif
}

size_t ScheduledPassCount()
{
#if BASICRENDERER_ENABLE_NVPERF
    auto& profiler = Profiler();
    std::lock_guard<std::mutex> lock(profiler.mutex);
    return profiler.configPassCount;
#else
    return 0;
#endif
}

std::optional<CaptureResult> TakeCaptureResult()
{
#if BASICRENDERER_ENABLE_NVPERF
    auto& profiler = Profiler();
    std::lock_guard<std::mutex> lock(profiler.mutex);
    if (!profiler.armed || (!profiler.finished && !profiler.failed)) {
        return std::nullopt;
    }
    if (!profiler.result) {
        profiler.result.emplace();
    }
    profiler.result->sampleId = profiler.sampleId;
    profiler.result->startFrame = profiler.captureStartFrame;
    profiler.result->endFrame = profiler.captureEndFrame;
    profiler.result->scheduledPasses = profiler.configPassCount;
    profiler.result->chipName = profiler.chipName;
    profiler.result->droppedRanges = profiler.droppedRanges;
    profiler.result->droppedTraceBytes = profiler.droppedTraceBytes;
    if (profiler.failed) {
        profiler.result->success = false;
        profiler.result->error = profiler.error.empty() ? "NVPerf capture failed" : profiler.error;
        profiler.result->unsupportedMetrics = profiler.unsupportedMetrics;
    }
    auto result = std::move(profiler.result);
    profiler.result.reset();
    profiler.armed = false;
    profiler.requested = false;
    return result;
#else
    return std::nullopt;
#endif
}

void ResetCaptureConfiguration()
{
#if BASICRENDERER_ENABLE_NVPERF
    auto& profiler = Profiler();
    std::lock_guard<std::mutex> lock(profiler.mutex);
    for (auto& [_, queueCapture] : profiler.queues) {
        ReleaseQueueSyncObjects(queueCapture);
    }
    profiler.queues.clear();
    profiler.activeCommandListRanges.clear();
    profiler.requestedMetrics.clear();
    profiler.passFilters.clear();
    profiler.programmaticConfigured = false;
    profiler.requested = false;
    profiler.armed = false;
    profiler.initialized = false;
    profiler.failed = false;
    profiler.finished = false;
    profiler.result.reset();
#endif
    g_streamingSuppressed.store(false, std::memory_order_release);
}

void SetStreamingSuppressed(bool suppressed)
{
    g_streamingSuppressed.store(suppressed, std::memory_order_release);
}

bool StreamingSuppressed()
{
    return g_streamingSuppressed.load(std::memory_order_acquire);
}

bool CaptureRequestedByEnvironment()
{
#if BASICRENDERER_ENABLE_NVPERF
    return IsTruthyEnv("BASICRENDERER_NVPERF_CAPTURE");
#else
    return false;
#endif
}

bool CaptureActive()
{
#if BASICRENDERER_ENABLE_NVPERF
    auto& profiler = Profiler();
    std::lock_guard<std::mutex> lock(profiler.mutex);
    if (!profiler.initialized || profiler.failed || profiler.finished) {
        return false;
    }

    for (const auto& [nativeQueue, queueCapture] : profiler.queues) {
        (void)nativeQueue;
        if (queueCapture.sessionActive || queueCapture.passActive) {
            return true;
        }
    }
    return false;
#else
    return false;
#endif
}

bool ServicePendingGpuOperations()
{
#if BASICRENDERER_ENABLE_NVPERF
    auto& profiler = Profiler();
    std::lock_guard<std::mutex> lock(profiler.mutex);
    if (!profiler.initialized || profiler.failed || profiler.finished) {
        return false;
    }

    bool serviced = false;
    for (auto& [nativeQueue, queueCapture] : profiler.queues) {
        (void)nativeQueue;
        if (!queueCapture.sessionActive) {
            continue;
        }
        ServicePendingGpuOperationsForQueue(queueCapture, "FrameWait", 16, 0);
        serviced = true;
    }
    return serviced;
#else
    return false;
#endif
}

#if BASICRENDERER_ENABLE_NVPERF
bool PassIsSelected(const D3D12PassProfiler& profiler, std::string_view queueName, std::string_view passName)
{
    if (profiler.passFilters.empty()) {
        return true;
    }
    return std::ranges::any_of(profiler.passFilters, [&](const PassFilter& filter) {
        return filter.name == passName && (filter.queue.empty() || filter.queue == queueName);
    });
}
#endif

void BeginFrameCapture(rhi::Backend backend, rhi::Device device, rhi::Queue graphicsQueue, uint64_t frameNumber)
{
#if BASICRENDERER_ENABLE_NVPERF
    auto& profiler = Profiler();
    std::lock_guard<std::mutex> lock(profiler.mutex);
    (void)PrepareProfilerIfRequested(backend, device, graphicsQueue, frameNumber);
#else
    (void)backend;
    (void)device;
    (void)graphicsQueue;
    (void)frameNumber;
#endif
}

void EndFrameCapture(rhi::Backend backend, rhi::Queue graphicsQueue, uint64_t frameNumber)
{
#if BASICRENDERER_ENABLE_NVPERF
    (void)backend;
    (void)graphicsQueue;
    (void)frameNumber;

    auto& profiler = Profiler();
    std::lock_guard<std::mutex> lock(profiler.mutex);
    if (!profiler.initialized || profiler.failed || profiler.finished) {
        return;
    }

    bool anyActive = false;
    bool allSubmitted = !profiler.queues.empty();
    for (auto& [nativeQueue, queueCapture] : profiler.queues) {
        (void)nativeQueue;
        anyActive = anyActive || queueCapture.sessionActive;
        if (queueCapture.passActive) {
            NVPW_D3D12_Profiler_Queue_EndPass_Params endPass{ NVPW_D3D12_Profiler_Queue_EndPass_Params_STRUCT_SIZE };
            endPass.pCommandQueue = queueCapture.nativeQueue;
            if (LogIfFailed(NVPW_D3D12_Profiler_Queue_EndPass(&endPass), "NVPW_D3D12_Profiler_Queue_EndPass")) {
                profiler.failed = true;
                return;
            }
            queueCapture.nextPassIndex = endPass.passIndex;
            queueCapture.targetNestingLevel = endPass.targetNestingLevel;
            queueCapture.allPassesSubmitted = endPass.allPassesSubmitted != 0;
            queueCapture.passActive = false;
            spdlog::info(
                "NVPerf: queue '{}' ended profiler pass nextPassIndex={} targetNestingLevel={} allPassesSubmitted={}",
                queueCapture.queueName,
                queueCapture.nextPassIndex,
                queueCapture.targetNestingLevel,
                queueCapture.allPassesSubmitted);
            ServicePendingGpuOperationsForQueue(queueCapture, "EndPass", 16, 0);
        }
        allSubmitted = allSubmitted && queueCapture.allPassesSubmitted;
    }

    if (!anyActive || !allSubmitted) {
        return;
    }

    bool allDecoded = true;
    for (auto& [nativeQueue, queueCapture] : profiler.queues) {
        (void)nativeQueue;
        if (!queueCapture.sessionActive) {
            continue;
        }

        const bool decoded = DecodeQueueCapture(profiler, queueCapture);
        if (!decoded) {
            allDecoded = false;
        }

        NVPW_D3D12_Profiler_Queue_EndSession_Params endSession{ NVPW_D3D12_Profiler_Queue_EndSession_Params_STRUCT_SIZE };
        endSession.pCommandQueue = queueCapture.nativeQueue;
        endSession.timeout = profiler.syncTimeoutMs;
        if (LogIfFailed(NVPW_D3D12_Profiler_Queue_EndSession(&endSession), "NVPW_D3D12_Profiler_Queue_EndSession")) {
            profiler.failed = true;
            return;
        }
        if (endSession.timeoutExpired) {
            spdlog::warn(
                "NVPerf: end session timed out for queue '{}' timeoutMs={}",
                queueCapture.queueName,
                profiler.syncTimeoutMs);
            profiler.failed = true;
            return;
        }
        queueCapture.sessionActive = false;
        ReleaseQueueSyncObjects(queueCapture);
    }

    profiler.finished = allDecoded;
    if (profiler.finished) {
        profiler.captureEndFrame = frameNumber;
        if (!profiler.result) {
            profiler.result.emplace();
        }
        profiler.result->sampleId = profiler.sampleId;
        profiler.result->startFrame = profiler.captureStartFrame;
        profiler.result->endFrame = frameNumber;
        profiler.result->scheduledPasses = profiler.configPassCount;
        profiler.result->droppedRanges = profiler.droppedRanges;
        profiler.result->droppedTraceBytes = profiler.droppedTraceBytes;
        profiler.result->success = profiler.droppedRanges == 0 && profiler.droppedTraceBytes == 0;
        profiler.result->error = profiler.result->success ? std::string{} : "NVPerf dropped counter data";
        profiler.result->chipName = profiler.chipName;
        profiler.result->unsupportedMetrics = profiler.unsupportedMetrics;
        profiler.result->metrics.clear();
        for (const auto& metric : profiler.metrics) {
            profiler.result->metrics.push_back({
                metric.spec.id.empty() ? metric.spec.name : metric.spec.id,
                metric.spec.name,
                metric.spec.outputName,
                metric.spec.unit,
                metric.spec.metricType,
                metric.spec.rollupOp,
                metric.spec.submetric,
                metric.spec.required
            });
        }
        spdlog::info(
            "NVPerf: pass metric capture complete startFrame={} endFrame={} output='{}'",
            profiler.captureStartFrame,
            frameNumber,
            profiler.csvPath.string());
    }
#else
    (void)backend;
    (void)graphicsQueue;
    (void)frameNumber;
#endif
}

void BeginPassRange(rhi::Backend backend, rhi::CommandList commandList, rhi::Queue queue, const char* queueName, const char* passName)
{
#if BASICRENDERER_ENABLE_NVPERF
    auto& profiler = Profiler();
    std::lock_guard<std::mutex> lock(profiler.mutex);
    if (backend != rhi::Backend::D3D12 || !profiler.initialized || profiler.failed || profiler.finished) {
        return;
    }

    const char* resolvedQueueName = queueName && queueName[0] != '\0' ? queueName : "Unknown";
    if (std::strcmp(resolvedQueueName, profiler.controllerQueueName.c_str()) != 0) {
        return;
    }
    const char* resolvedPassName = passName && passName[0] != '\0' ? passName : "<unnamed>";
    if (!PassIsSelected(profiler, resolvedQueueName, resolvedPassName)) {
        return;
    }

    ID3D12CommandQueue* nativeQueue = rhi::dx12::get_queue(queue);
    ID3D12GraphicsCommandList* nativeCommandList = rhi::dx12::get_cmd_list(commandList);
    if (!nativeQueue || !nativeCommandList) {
        return;
    }

    auto& queueCapture = profiler.queues[nativeQueue];
    if (!queueCapture.nativeQueue) {
        queueCapture.queue = queue;
        queueCapture.nativeQueue = nativeQueue;
        queueCapture.queueName = resolvedQueueName;
    }

    if (!EnsureQueuePassActive(profiler, queueCapture)) {
        return;
    }

    NVPW_D3D12_Profiler_CommandList_PushRange_Params push{ NVPW_D3D12_Profiler_CommandList_PushRange_Params_STRUCT_SIZE };
    push.pCommandList = nativeCommandList;
    push.pRangeName = resolvedPassName;
    push.rangeNameLength = std::strlen(push.pRangeName);
    if (!LogIfFailed(NVPW_D3D12_Profiler_CommandList_PushRange(&push), "NVPW_D3D12_Profiler_CommandList_PushRange")) {
        ++profiler.activeCommandListRanges[nativeCommandList];
    }
#else
    (void)backend;
    (void)commandList;
    (void)queue;
    (void)queueName;
    (void)passName;
#endif
}

void EndPassRange(rhi::Backend backend, rhi::CommandList commandList, rhi::Queue queue)
{
#if BASICRENDERER_ENABLE_NVPERF
    (void)queue;
    auto& profiler = Profiler();
    std::lock_guard<std::mutex> lock(profiler.mutex);
    if (backend != rhi::Backend::D3D12 || !profiler.initialized || profiler.failed || profiler.finished) {
        return;
    }

    ID3D12GraphicsCommandList* nativeCommandList = rhi::dx12::get_cmd_list(commandList);
    if (!nativeCommandList) {
        return;
    }

    auto rangeIt = profiler.activeCommandListRanges.find(nativeCommandList);
    if (rangeIt == profiler.activeCommandListRanges.end() || rangeIt->second == 0) {
        return;
    }

    NVPW_D3D12_Profiler_CommandList_PopRange_Params pop{ NVPW_D3D12_Profiler_CommandList_PopRange_Params_STRUCT_SIZE };
    pop.pCommandList = nativeCommandList;
    if (!LogIfFailed(NVPW_D3D12_Profiler_CommandList_PopRange(&pop), "NVPW_D3D12_Profiler_CommandList_PopRange")) {
        --rangeIt->second;
        if (rangeIt->second == 0) {
            profiler.activeCommandListRanges.erase(rangeIt);
        }
    }
#else
    (void)backend;
    (void)commandList;
    (void)queue;
#endif
}

} // namespace br::telemetry::nvperf
