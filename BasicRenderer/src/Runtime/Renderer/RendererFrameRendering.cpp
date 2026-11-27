#include <BasicRenderer/Renderer.h>
#include "OpenRenderGraph/GraphInstance.h"
#include "OpenRenderGraph/GraphHost.h"
#include "Resources/DynamicResource.h"
#include <BasicRenderer/Streaming/TaskScheduler.h>
#include <BasicRenderer/Diagnostics/NvPerfIntegration.h>
#include <BasicRenderer/Extensions/RenderContext.h>
#include <BasicTelemetry/Tracy.h>
#include <spdlog/spdlog.h>
#include <rhi_debug.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <stdexcept>
#include <stacktrace>
#include <sstream>
#include <thread>
#include <iomanip>
#include <typeinfo>
#include <chrono>
#include "Runtime/Device/DeviceManager.h"
#include "Runtime/Settings/SettingsManager.h"
#include "BasicRenderer/Streaming/TaskScheduler.h"
#include "Runtime/Settings/RendererSettingsHelpers.h"
#include "Resources/ExternalTextureResource.h"
#include "Runtime/IO/ReadbackManager.h"
#include "Runtime/IO/ReadbackCaptureService.h"
#include "OpenRenderGraph/ExternalClient.h"
#include "Runtime/Frame/RendererFrameInputs.h"
#include "Runtime/GraphIntegration/FrameBuild.h"
#include "Diagnostics/Telemetry/FrameTaskGraphTelemetry.h"
#include "Scene/ECS/RendererECSManager.h"
#include "VirtualGeometry/GeometryStorage/MeshManager.h"
#include "Materials/MaterialManager.h"
#include "BasicRenderer/Assets/MaterialTextureStreaming.h"
#include "VirtualGeometry/RayTracing/CLodRayTracingSystem.h"
#include "Utilities/Utilities.h"
#include "OpenRenderGraph/OpenRenderGraph.h"

namespace {
std::string RendererExceptionStacktraceString()
{
#if defined(__cpp_lib_stacktrace) && (__cpp_lib_stacktrace >= 202011L)
    try {
        std::ostringstream output;
        output << std::stacktrace::current();
        return output.str();
    } catch (...) {
        return "(stacktrace capture failed)";
    }
#else
    return "(no <stacktrace> support in this build)";
#endif
}

std::filesystem::path MakeRendererExceptionPath(uint64_t frameNumber, const char* stageName)
{
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
    (void)localtime_s(std::addressof(localTime), std::addressof(time));

    std::ostringstream name;
    name << "RendererException-"
         << std::put_time(std::addressof(localTime), "%Y%m%d-%H%M%S")
         << "-frame" << frameNumber
         << "-" << stageName
         << "-tid" << GetCurrentThreadId()
         << ".txt";

    std::error_code ec;
    std::filesystem::create_directories("crashes", ec);
    return std::filesystem::current_path() / "crashes" / name.str();
}

void WriteRendererExceptionNote(
    const char* stageName,
    uint64_t frameNumber,
    uint8_t frameIndex,
    uint64_t frameFenceValue,
    const std::exception& ex)
{
    const auto path = MakeRendererExceptionPath(frameNumber, stageName);
    std::ofstream report(path, std::ios::trunc);
    if (!report) {
        return;
    }

    std::ostringstream threadId;
    threadId << std::this_thread::get_id();
    report << "Renderer exception note\n";
    report << "stage='" << stageName << "'\n";
    report << "frame=" << frameNumber << "\n";
    report << "frame_index=" << static_cast<unsigned>(frameIndex) << "\n";
    report << "frame_fence_value=" << frameFenceValue << "\n";
    report << "thread_id=" << threadId.str() << "\n";
    report << "win32_thread_id=" << GetCurrentThreadId() << "\n";
    report << "exception_type='" << typeid(ex).name() << "'\n";
    report << "exception_what='" << ex.what() << "'\n\n";
    report << "Catch-site stacktrace:\n" << RendererExceptionStacktraceString() << "\n";
}
}

namespace {
}

void SyncOpenRenderGraphSettings(uint8_t numFramesInFlight) {
    auto& sm = SettingsManager::GetInstance();
    org::runtime::OpenRenderGraphSettings orgSettings{};
    orgSettings.numFramesInFlight = numFramesInFlight;
    orgSettings.collectPassStatistics = sm.getSettingGetter<bool>("collectPassStatistics")();
    orgSettings.collectPipelineStatistics = sm.getSettingGetter<bool>("collectPipelineStatistics")();
    orgSettings.renderGraphCompileDumpEnabled = sm.getSettingGetter<bool>("renderGraphCompileDumpEnabled")();
    orgSettings.renderGraphVramDumpEnabled = sm.getSettingGetter<bool>("renderGraphVramDumpEnabled")();
    orgSettings.renderGraphBatchTraceEnabled = sm.getSettingGetter<bool>("renderGraphBatchTraceEnabled")();
    orgSettings.readOnlyUniformTransitionElisionEnabled = true;
    orgSettings.autoAliasMode = static_cast<uint8_t>(sm.getSettingGetter<org::AutoAliasMode>("autoAliasMode")());
    orgSettings.autoAliasPackingStrategy = static_cast<uint8_t>(sm.getSettingGetter<org::AutoAliasPackingStrategy>("autoAliasPackingStrategy")());
    orgSettings.autoAliasEnableLogging = sm.getSettingGetter<bool>("autoAliasEnableLogging")();
    orgSettings.autoAliasPoolRetireIdleFrames = sm.getSettingGetter<uint32_t>("autoAliasPoolRetireIdleFrames")();
    orgSettings.autoAliasPoolGrowthHeadroom = sm.getSettingGetter<float>("autoAliasPoolGrowthHeadroom")();
    // ORG's diagnostic logs, under BasicRenderer's established environment switches.
    static const uint32_t diagnosticLogs = [] {
        using br::runtime::renderer_settings::ReadTruthyEnvironmentFlag;
        using org::runtime::DiagnosticLog;
        uint32_t logs = 0;
        if (ReadTruthyEnvironmentFlag("SARP_MEMORY_INTROSPECTION_LOG"))
            logs |= static_cast<uint32_t>(DiagnosticLog::MemoryIntrospection);
        if (ReadTruthyEnvironmentFlag("SARP_UPLOAD_TELEMETRY_LOG") || ReadTruthyEnvironmentFlag("SARP_TEXTURE_STREAMING_TRANSITION_LOG"))
            logs |= static_cast<uint32_t>(DiagnosticLog::UploadTelemetry);
        if (ReadTruthyEnvironmentFlag("SARP_GPU_LIFETIME_TRACE"))
            logs |= static_cast<uint32_t>(DiagnosticLog::KeepAliveLifetime);
        return logs;
    }();
    orgSettings.diagnosticLogs = diagnosticLogs;
    org::runtime::SetOpenRenderGraphSettings(orgSettings);
}



// The render thread's frame-graph time (M4 step 5): each host epoch's ExecuteFrame and the whole frame's (with the seal), as means
// over 600 frames in the log. index SIZE_MAX: the frame's total, which closes the frame.
void Renderer::RecordFrameGraphExecution(size_t index, double milliseconds, double waitMs, double updateMs, double executeMs) {
    auto& timing = m_frameGraphExecutionTiming;
    if (index != SIZE_MAX) {
        if (timing.epochMs.size() <= index) timing.epochMs.resize(index + 1, 0.0);
        timing.epochMs[index] += milliseconds;
        timing.waitMs += waitMs;
        timing.updateMs += updateMs;
        timing.executeMs += executeMs;
        const auto& hosts = br::render::FrameHostEpochs();
        const auto& last = m_frameHost->LastFrameTimings();
        {
            if (timing.stageMs.size() <= index) timing.stageMs.resize(index + 1, {});
            const auto& e = last.execute;
            const auto& u = last.update;
            const double parts[FrameGraphExecutionTiming::kStages] = {last.buildUs, last.waitUs, last.releaseUs + last.beforePrepareUs,
                u.passUpdatesUs, u.beforeCompileUs, u.prepareUs, e.retireUs, e.admissionUs, e.acquireUs, e.recordUs, e.submitUs, e.commitUs,
                last.executeUs - (e.retireUs + e.admissionUs + e.acquireUs + e.recordUs + e.submitUs + e.commitUs), last.signalUs};
            auto& stages = timing.stageMs[index];
            for (size_t stage = 0; stage < FrameGraphExecutionTiming::kStages; ++stage) stages[stage] += parts[stage] / 1000.0;
            if (last.probed) {
                if (timing.probes.size() <= index) timing.probes.resize(index + 1);
                auto& probe = timing.probes[index];
                const auto& p = last.probe;
                probe.runs += p.runs;
                probe.firstRuns += p.firstRuns;
                probe.publication += p.publication;
                probe.publicationBound += p.publicationBound;
                probe.bindings += p.bindings;
                probe.perFrame += p.perFrame;
                probe.versions += p.versions;
                probe.backings += p.backings;
                probe.placement += p.placement;
                probe.any += p.any;
                probe.anyByIdentity += p.anyByIdentity;
                for (const auto& [name, count] : p.names) probe.names[name] += count;
            }
            // Kept recordings (M6 step 3): what the epoch's run did with its kept copy, and why it recorded.
            // With the submission thread the run's outcome comes from the thread's report (RecordSubmittedExecutions).
            if (last.kept != org::RenderGraph::KeptRunOutcome::None) AddKeptStats(index, index < hosts.size() ? hosts[index] : 0u);
        }
        timing.frameEpochs += fmt::format("{}{}: {:.2f} (build {:.2f}, wait {:.2f}, update {:.2f}, execute {:.2f})",
            timing.frameEpochs.empty() ? "" : ", ", index < hosts.size() ? hosts[index] : 0u, milliseconds, last.buildUs / 1000.0, waitMs,
            updateMs, executeMs);
        // A long execute: where it went (diagnostic, M5 step 8).
        if (executeMs > 8.0) {
            const auto& e = last.execute;
            timing.frameEpochs += fmt::format(" [execute: retire {:.2f}, admission {:.2f}, acquire {:.2f}, record {:.2f}, submit {:.2f}, "
                "commit {:.2f}; {} batches]", e.retireUs / 1000.0, e.admissionUs / 1000.0, e.acquireUs / 1000.0, e.recordUs / 1000.0,
                e.submitUs / 1000.0, e.commitUs / 1000.0, e.batches);
        }
        return;
    }
    FinishFrameGraphExecution(milliseconds);
}

void Renderer::AddKeptStats(size_t index, uint32_t host) {
    auto& timing = m_frameGraphExecutionTiming;
    {
        {
            {
                if (timing.kept.size() <= index) timing.kept.resize(index + 1);
                const auto stats = m_frameHost->TakeKeptStats(host);
                for (auto* into : { &timing.kept[index], &m_keptRecordingTotals[host] }) {
                    into->runs += stats.runs;
                    into->reused += stats.reused;
                    into->kept += stats.kept;
                    into->unkeepable += stats.unkeepable;
                    for (const auto& [reason, count] : stats.reasons)
                        if (into->reasons.size() < 64 || into->reasons.contains(reason)) into->reasons[reason] += count;
                    into->audited += stats.audited;
                    into->auditCompared += stats.auditCompared;
                    into->auditUncomparable += stats.auditUncomparable;
                    into->auditMismatches += stats.auditMismatches;
                    into->stagedReused += stats.stagedReused;
                    into->stagedRecorded += stats.stagedRecorded;
                    into->partial += stats.partial;
                    into->batchesReused += stats.batchesReused;
                    into->batchesRecorded += stats.batchesRecorded;
                    for (const auto& [reason, count] : stats.batchReasons)
                        if (into->batchReasons.size() < 64 || into->batchReasons.contains(reason)) into->batchReasons[reason] += count;
                    for (const auto& [name, count] : stats.mismatches)
                        if (into->mismatches.size() < 64 || into->mismatches.contains(name)) into->mismatches[name] += count;
                }
            }
        }
    }
}

void Renderer::RecordSubmittedExecutions() {
    if (!m_frameHost || !m_frameHost->ExecutionsOnSubmissionThread()) return;
    auto& timing = m_frameGraphExecutionTiming;
    const auto& hosts = br::render::FrameHostEpochs();
    for (const auto& run : m_frameHost->TakeSubmittedExecutions()) {
        const auto position = static_cast<size_t>(std::find(hosts.begin(), hosts.end(), run.epoch) - hosts.begin());
        if (position >= hosts.size()) continue;  // the upload epoch's
        if (timing.threadStageMs.size() <= position) {
            timing.threadStageMs.resize(position + 1, {});
            timing.threadRuns.resize(position + 1, 0);
        }
        const auto& e = run.execute;
        const double parts[FrameGraphExecutionTiming::kThreadStages] = {e.retireUs, e.admissionUs, e.acquireUs, e.recordUs, e.submitUs,
            e.commitUs, run.executeUs - (e.retireUs + e.admissionUs + e.acquireUs + e.recordUs + e.submitUs + e.commitUs), run.signalUs};
        auto& stages = timing.threadStageMs[position];
        for (size_t stage = 0; stage < FrameGraphExecutionTiming::kThreadStages; ++stage) stages[stage] += parts[stage] / 1000.0;
        ++timing.threadRuns[position];
        if (run.failed) ++timing.threadFailed;
        if (run.kept != org::RenderGraph::KeptRunOutcome::None) AddKeptStats(position, run.epoch);
        if (run.executeUs > 8000.0)
            spdlog::info("Renderer: host epoch {} of host frame {} took {:.2f} ms on the submission thread [retire {:.2f}, admission {:.2f}, "
                "acquire {:.2f}, record {:.2f}, submit {:.2f}, commit {:.2f}; {} batches]", run.epoch, run.hostFrame, run.executeUs / 1000.0,
                e.retireUs / 1000.0, e.admissionUs / 1000.0, e.acquireUs / 1000.0, e.recordUs / 1000.0, e.submitUs / 1000.0, e.commitUs / 1000.0,
                e.batches);
    }
}

void Renderer::FinishFrameGraphExecution(double milliseconds) {
    auto& timing = m_frameGraphExecutionTiming;
    RecordSubmittedExecutions();
    timing.totalMs += milliseconds;
    timing.worstMs = (std::max)(timing.worstMs, milliseconds);
    // A slow frame, or one of the two frames after an attach (diagnostic): where its render-thread time went, with the
    // consolidation point before it.
    const bool afterAttach = m_framesSinceFrameGraphAttach < 2;
    if (m_framesSinceFrameGraphAttach != UINT32_MAX) ++m_framesSinceFrameGraphAttach;
    // The frames around a new frame placement version (a trim, the settled layout): the one it was published in, and the first that
    // places from it, where every graph re-places (diagnostic, M5 step 7).
    if (const auto placement = m_frameHost->CurrentFramePlacementId(); placement != m_lastFramePlacementId) {
        if (m_lastFramePlacementId) m_framesSinceFramePlacementChange = 0;
        m_lastFramePlacementId = placement;
    }
    if (m_framesSinceFramePlacementChange < 2)
        spdlog::info("Renderer: frame {} {} frame placement {}: consolidation {:.2f} ms, frame graph {:.2f} ms; per host epoch {}", m_totalFramesRendered,
            m_framesSinceFramePlacementChange == 0 ? "published" : "first on", m_lastFramePlacementId, timing.consolidationMs, milliseconds,
            timing.frameEpochs);
    if (m_framesSinceFramePlacementChange != UINT32_MAX) ++m_framesSinceFramePlacementChange;
    if (milliseconds + timing.consolidationMs > 16.0 || afterAttach)
        spdlog::info("Renderer: {} {}: consolidation {:.2f} ms, frame graph {:.2f} ms; per host epoch {}",
            afterAttach ? (m_framesSinceFrameGraphAttach == 1 ? "first frame after an attach," : "second frame after an attach,") : "slow frame",
            m_totalFramesRendered, timing.consolidationMs, milliseconds, timing.frameEpochs);
    timing.frameEpochs.clear();
    timing.consolidationMs = 0.0;
    if (++timing.frames < 600) return;
    std::string epochs;
    const auto& hosts = br::render::FrameHostEpochs();
    for (size_t epoch = 0; epoch < timing.epochMs.size(); ++epoch)
        epochs += fmt::format("{}{}: {:.3f}", epochs.empty() ? "" : ", ", epoch < hosts.size() ? hosts[epoch] : 0u, timing.epochMs[epoch] / timing.frames);
    auto& sets = m_frameSetCheck;
    spdlog::info("Renderer: frame graph ExecuteFrame over {} frames: mean {:.3f} ms per frame (worst {:.2f}; pacing wait {:.3f}, "
        "update {:.3f}, execute {:.3f}); per host epoch {}; build sets: {} frames checked, {} mixed, {} unknown", timing.frames,
        timing.totalMs / timing.frames, timing.worstMs, timing.waitMs / timing.frames, timing.updateMs / timing.frames,
        timing.executeMs / timing.frames, epochs, sets.frames, sets.mixed, sets.unknown);
    // Per epoch by stage (M6 step 2): build, pacing wait, releases and inputs, pass updates, before compile, preparation, then
    // Execute's retire, admission, acquire, record, submit, commit, its rest, and the completion points.
    static constexpr const char* kStageNames[FrameGraphExecutionTiming::kStages] = {"build", "wait", "inputs", "updates", "beforeCompile",
        "prepare", "retire", "admission", "acquire", "record", "submit", "commit", "executeRest", "signal"};
    for (size_t epoch = 0; epoch < timing.stageMs.size(); ++epoch) {
        std::string stages;
        for (size_t stage = 0; stage < FrameGraphExecutionTiming::kStages; ++stage)
            stages += fmt::format("{}{} {:.3f}", stages.empty() ? "" : ", ", kStageNames[stage], timing.stageMs[epoch][stage] / timing.frames);
        spdlog::info("Renderer: frame graph stages over {} frames, host epoch {}: {}", timing.frames, epoch < hosts.size() ? hosts[epoch] : 0u, stages);
    }
    // The currency probe: of the runs compared with the epoch's previous one, how many changed what a kept recording is keyed on.
    for (size_t epoch = 0; epoch < timing.probes.size(); ++epoch) {
        const auto& probe = timing.probes[epoch];
        std::vector<std::pair<uint32_t, std::string>> names;
        for (const auto& [name, count] : probe.names) names.emplace_back(count, name);
        std::sort(names.begin(), names.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::string top;
        for (size_t i = 0; i < names.size() && i < 12; ++i) top += fmt::format("{}{} {}", top.empty() ? "" : ", ", names[i].second, names[i].first);
        spdlog::info("Renderer: currency probe over {} frames, host epoch {}: runs {} (first {}), stale {} (by identity {}); publication {} "
            "(in a bound slot {}), bindings {}, per-frame {}, versions {}, backings {}, placement {}; changed: {}", timing.frames,
            epoch < hosts.size() ? hosts[epoch] : 0u, probe.runs, probe.firstRuns, probe.any, probe.anyByIdentity, probe.publication,
            probe.publicationBound, probe.bindings, probe.perFrame, probe.versions, probe.backings, probe.placement, top.empty() ? "none" : top);
    }
    // Kept recordings (M6 step 3, BASICRENDERER_KEPT_RECORDINGS): runs that submitted their copy's kept recording, runs that recorded
    // and kept it, runs whose recording cannot be kept, and why each recorded.
    for (size_t epoch = 0; epoch < timing.kept.size(); ++epoch) {
        const auto& kept = timing.kept[epoch];
        if (!kept.runs) continue;
        std::vector<std::pair<uint64_t, std::string>> reasons;
        for (const auto& [reason, count] : kept.reasons) reasons.emplace_back(count, reason);
        std::sort(reasons.begin(), reasons.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::string top;
        for (size_t i = 0; i < reasons.size() && i < 6; ++i) top += fmt::format("{}{} x{}", top.empty() ? "" : "; ", reasons[i].second, reasons[i].first);
        spdlog::info("Renderer: kept recordings over {} frames, host epoch {}: runs {}, reused {}, partly reused {}, recorded and kept {}, not "
            "keepable {}; staged recordings reused {}, recorded again {}; why recorded: {}", timing.frames, epoch < hosts.size() ? hosts[epoch] : 0u,
            kept.runs, kept.reused, kept.partial, kept.kept, kept.unkeepable, kept.stagedReused, kept.stagedRecorded, top.empty() ? "none" : top);
        // Batch reuse (M6 step 7): the batches submitted from kept recordings and recorded, and why the recorded ones recorded.
        if (kept.batchesReused || kept.batchesRecorded) {
            std::vector<std::pair<uint64_t, std::string>> batchReasons;
            for (const auto& [reason, count] : kept.batchReasons) batchReasons.emplace_back(count, reason);
            std::sort(batchReasons.begin(), batchReasons.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            std::string batches;
            for (size_t i = 0; i < batchReasons.size() && i < 12; ++i)
                batches += fmt::format("{}{} x{}", batches.empty() ? "" : "; ", batchReasons[i].second, batchReasons[i].first);
            spdlog::info("Renderer: kept batches over {} frames, host epoch {}: reused {}, recorded {}; why recorded: {}", timing.frames,
                epoch < hosts.size() ? hosts[epoch] : 0u, kept.batchesReused, kept.batchesRecorded, batches.empty() ? "none" : batches);
        }
        // The revision audit (M6 step 4): what the audited reuses compared, and the passes whose revision missed an input.
        if (kept.audited) {
            std::string names;
            for (const auto& [name, count] : kept.mismatches) names += fmt::format("{}{} x{}", names.empty() ? "" : "; ", name, count);
            spdlog::info("Renderer: kept recording audit over {} frames, host epoch {}: audited {}, passes compared {}, not comparable {}, "
                "mismatches {}{}{}", timing.frames, epoch < hosts.size() ? hosts[epoch] : 0u, kept.audited, kept.auditCompared,
                kept.auditUncomparable, kept.auditMismatches, names.empty() ? "" : ": ", names);
        }
    }
    // The submission thread (M7 step 5): each epoch's execution there by stage, and the thread's own counts.
    static constexpr const char* kThreadStageNames[FrameGraphExecutionTiming::kThreadStages] = {"retire", "admission", "acquire", "record",
        "submit", "commit", "executeRest", "signal"};
    for (size_t epoch = 0; epoch < timing.threadStageMs.size(); ++epoch) {
        std::string stages;
        double total = 0.0;
        for (size_t stage = 0; stage < FrameGraphExecutionTiming::kThreadStages; ++stage) {
            stages += fmt::format("{}{} {:.3f}", stages.empty() ? "" : ", ", kThreadStageNames[stage], timing.threadStageMs[epoch][stage] / timing.frames);
            total += timing.threadStageMs[epoch][stage];
        }
        spdlog::info("Renderer: frame graph on the submission thread over {} frames, host epoch {}: total {:.3f}, {}; runs {}", timing.frames,
            epoch < hosts.size() ? hosts[epoch] : 0u, total / timing.frames, stages, timing.threadRuns[epoch]);
    }
    if (m_frameHost->SubmissionThreadOn()) {
        const auto thread = m_frameHost->TakeSubmitThreadStats();
        const auto describe = [](const auto& byWhy) {
            std::string text;
            for (const auto& [why, counts] : byWhy)
                text += fmt::format("{}{} x{} ({:.2f} ms)", text.empty() ? "" : "; ", why, counts.first, counts.second / 1000.0);
            return text.empty() ? std::string("none") : text;
        };
        spdlog::info("Renderer: submission thread over {} frames: busy {:.3f} ms per frame, {} executions ({} failed, {} reported failed), "
            "peak depth {}, external submissions queued {}; drains {} ({:.3f} ms per frame): {}; epoch waits {} ({:.3f} ms per frame): {}; "
            "backpressure waits {} ({:.2f} ms)", timing.frames, thread.busyUs / 1000.0 / timing.frames, thread.executions,
            thread.failedExecutions, timing.threadFailed, thread.peakDepth, thread.externalQueued, thread.drains,
            thread.drainUs / 1000.0 / timing.frames, describe(thread.drainsByCaller), thread.epochWaits, thread.epochWaitUs / 1000.0 / timing.frames,
            describe(thread.epochWaitsByReason), thread.backpressureWaits, thread.backpressureUs / 1000.0);
    }
    if (timing.renderThreadFrames) {
        const double frames = timing.renderThreadFrames;
        spdlog::info("Renderer: render thread over {} frames: Update + Render {:.3f} ms per frame, of which WaitForFrame {:.3f}, Present {:.3f}, "
            "present lead wait {:.3f} ({} waits); the rest {:.3f}", timing.renderThreadFrames, timing.renderThreadFrameMs / frames,
            timing.waitForFrameMs / frames, timing.inlinePresentMs / frames, timing.presentLeadWaitMs / frames, timing.presentLeadWaits,
            (timing.renderThreadFrameMs - timing.waitForFrameMs - timing.inlinePresentMs - timing.presentLeadWaitMs) / frames);
    }
    timing = {};
    sets.frames = sets.mixed = sets.unknown = 0;
}

void Renderer::RecordAttachedBuildSet(uint64_t serial, const std::vector<uint32_t>& hosts) {
    if (!m_frameHost || hosts.empty()) return;
    auto& check = m_frameSetCheck;
    for (const auto host : hosts) {
        auto& generations = check.serialOfGeneration[host];
        generations[m_frameHost->BuildGeneration(host)] = serial;
        // The latest few are enough: an epoch runs its newest attached build.
        while (generations.size() > 8) generations.erase(generations.begin());
    }
    check.setOf[serial] = hosts;
    // Forget sets no epoch can run any more (older than every epoch's oldest kept generation).
    uint64_t oldest = UINT64_MAX;
    for (const auto& [host, generations] : check.serialOfGeneration)
        if (!generations.empty()) oldest = (std::min)(oldest, generations.begin()->second);
    while (!check.setOf.empty() && check.setOf.begin()->first < oldest) check.setOf.erase(check.setOf.begin());
}

bool Renderer::CheckFrameBuildSet() {
    auto& check = m_frameSetCheck;
    ++check.frames;
    ++check.totalFrames;
    const auto& hosts = br::render::FrameHostEpochs();
    // Each epoch's running build, from the host's own generation of the graph it runs.
    std::array<uint64_t, 8> serials{};
    bool unknown = false;
    for (size_t index = 0; index < hosts.size() && index < serials.size(); ++index) {
        const auto found = check.serialOfGeneration.find(hosts[index]);
        const auto generation = m_frameHost->BuildGeneration(hosts[index]);
        const auto serial = found != check.serialOfGeneration.end() ? found->second.find(generation) : decltype(found->second.end()){};
        if (found == check.serialOfGeneration.end() || serial == found->second.end()) {
            unknown = true;
            continue;
        }
        serials[index] = serial->second;
    }
    if (unknown) {
        ++check.unknown;
        ++check.totalUnknown;
    }
    // Every epoch a running build attached together with this one runs that build or a later one.
    for (size_t index = 0; index < hosts.size() && index < serials.size(); ++index) {
        if (!serials[index]) continue;
        const auto set = check.setOf.find(serials[index]);
        if (set == check.setOf.end()) continue;
        for (const auto other : set->second) {
            const auto position = std::find(hosts.begin(), hosts.end(), other) - hosts.begin();
            if (position >= static_cast<std::ptrdiff_t>(serials.size()) || !serials[position] || serials[position] >= serials[index]) continue;
            ++check.mixed;
            ++check.totalMixed;
            spdlog::error("Renderer: mixed frame {}: host epoch {} runs build {}, which attached host epoch {} with it, but that epoch runs "
                "the older build {}", m_totalFramesRendered, hosts[index], serials[index], other, serials[position]);
            return false;
        }
    }
    return true;
}

void Renderer::ExecuteFrameGraph(const org::UpdateExecutionContext& frameContext) {
    BT_ZONE_SCOPE("Renderer::ExecuteFrameGraph");
    m_frameExecuted = false;

    const auto runCapturedStage = [this](const char* stageName, auto&& stageFn) {
        const auto stageStart = std::chrono::steady_clock::now();
        stageFn();
        const auto stageEnd = std::chrono::steady_clock::now();
        RecordFrameTaskStage(stageName, br::telemetry::CpuTaskDomain::MainThread, stageStart, stageEnd);
    };

    auto deltaTime = m_frameTimer.tick();
    if (m_deterministicSamplingMode) {
        deltaTime = 0.0f;
    }
    if (!IsSceneReadyForFrame()) {
        return;
    }

    if (!currentRenderGraph) {
        return;
    }

    // Update may intentionally apply publication backpressure while the first
    // complete immutable manifest is being produced.
    if (!m_frameInputs) {
        return;
    }

    if (!m_swapChainReady) {
        if (!m_loggedSwapChainNotReady) {
            spdlog::critical(
                "Renderer: skipping render because swapchain/backbuffers are not ready frame={} frameIndex={}",
                m_totalFramesRendered,
                static_cast<unsigned>(m_frameIndex));
            m_loggedSwapChainNotReady = true;
        }
        return;
    }

    const bool renderGraphBatchTraceEnabled = SettingsManager::GetInstance().getSettingGetter<bool>("renderGraphBatchTraceEnabled")();

    // Vulkan does not guarantee round-robin swapchain acquisition.  Re-read the
    // acquired image at the last responsible point and bind the graph's dynamic
    // backbuffer to that exact image.  Using the CPU frame slot here can record
    // transitions for a presentable image that was not acquired.
    // With the submission thread (M7 step 5) the present runs there and reads the image itself: the frame slot advances round
    // robin, and no graph pass binds the backbuffer.
    const uint8_t renderedFrameIndex = m_swapChain && !m_submissionThread
        ? static_cast<uint8_t>(m_swapChain->CurrentImageIndex())
        : m_frameIndex;
    if (renderedFrameIndex != m_frameIndex) {
        spdlog::warn(
            "Renderer: acquired swapchain image changed before render (frame slot={} acquired={}); resynchronizing",
            static_cast<unsigned>(m_frameIndex),
            static_cast<unsigned>(renderedFrameIndex));
        m_frameIndex = renderedFrameIndex;
    }
    if (m_dynamicBackbuffer && renderedFrameIndex < m_backbufferResources.size()) {
        m_dynamicBackbuffer->SetResource(m_backbufferResources[renderedFrameIndex]);
    }

    auto& world = RendererECSManager::GetInstance().GetWorld();
	const Components::DrawStats& drawStats = world.get<Components::DrawStats>();
    // The attached graphs' sizes (M5 step 4).
    const auto attachedResolution = AttachedResolution();
    auto renderRes = attachedResolution.render;
    auto outputRes = attachedResolution.output;

    auto& deviceManager = DeviceManager::GetInstance();

    {
        BT_ZONE_SCOPE("Renderer::Render::PrepareRenderContext");
        runCapturedStage("PrepareRenderContext", [&]() {
			if (!currentRenderGraph || !m_orgRuntime->Descriptors().get()) {
				throw std::runtime_error("Renderer: descriptor service unavailable while preparing render context");
			}
			auto* descriptorService = m_orgRuntime->Descriptors().get();
            m_context.hasPrimaryCamera = false;
            m_context.primaryViewID = 0;
			m_context.textureDescriptorHeap = descriptorService->GetSRVDescriptorHeap();
			m_context.samplerDescriptorHeap = descriptorService->GetSamplerDescriptorHeap();
            m_context.rtvHeap = rtvHeap.Get();
            m_context.rtvDescriptorSize = rtvDescriptorSize;
            m_context.dsvDescriptorSize = dsvDescriptorSize;
            m_context.swapchainImageIndex = renderedFrameIndex;
            m_context.frameIndex = renderedFrameIndex;
            m_context.frameNumber = m_totalFramesRendered;
            m_context.frameFenceValue = m_currentFrameFenceValue;
            m_context.renderResolution = { renderRes.x, renderRes.y };
            m_context.outputResolution = { outputRes.x, outputRes.y };
            m_context.clodRayTracingSupported = deviceManager.GetCLodRayTracingSupported();
            m_context.rayTracedReflectionsEnabled = m_rayTracedReflections && m_context.clodRayTracingSupported;
            {
                BT_ZONE_SCOPE("Renderer::Render::PrepareRenderContext::MaterialStreamingStats");
                m_context.materialTextureStreamingStats = m_pMaterialManager
                    ? m_pMaterialManager->GetMaterialTextureStreamingStats()
                    : MaterialTextureStreamingStats{};
            }
            m_context.environmentWork = m_environmentWorkServices;
            m_context.clodRayTracingSystem = m_clodRayTracingSystem;

            if (m_clodRayTracingSystem && m_clodRayTracingSystem->IsInitialized() && m_pMeshManager) {
                // Reads back the RT cache's statistics and micromap requests, and records new page slabs.
                BT_ZONE_SCOPE("Renderer::Render::PrepareRenderContext::CLodRayTracing");
                // The scene passes recorded at execution (CLodRayTracingSceneOmmPass, CLodRayTracingSceneEndPass) write the frame's
                // readback slice by the system's frame count. With the submission thread the last frame's may still be queued there:
                // they go through before the count advances. Normally they have, and this costs nothing.
                if (m_submissionThread)
                    for (const auto host : br::render::FrameHostEpochs()) m_frameHost->WaitForEpochExecution(host, "CLod ray tracing frame advance");
                std::scoped_lock rayTracingLock(m_clodRayTracingSystem->FrameOperationMutex());
                m_clodRayTracingSystem->AdvanceFrame();
                m_clodRayTracingSystem->BeginFrame(*m_pMeshManager);
            }
            m_context.drawStats = drawStats;
            m_context.deltaTime = deltaTime;
            {
                BT_ZONE_SCOPE("Renderer::Render::PrepareRenderContext::SceneOverlapStatus");
                m_context.sceneOverlapStatus = GetSceneOverlapStatus();
            }

            BT_ZONE_SCOPE("Renderer::Render::PrepareRenderContext::PrimaryCamera");
            auto primaryCamera = GetValidatedPrimaryRenderCamera(false);
            if (primaryCamera) {
                m_context.hasPrimaryCamera = true;
                m_context.primaryViewID = primaryCamera.get<Components::RenderViewRef>().viewID;
                m_context.primaryCamera = primaryCamera.get<Components::Camera>();
                if (auto depthMap = primaryCamera.try_get<Components::DepthMap>()) {
                    m_context.primaryDepthMap = *depthMap;
                }
            }

            unsigned int globalPSOFlags = 0;
            if (m_imageBasedLighting) {
                globalPSOFlags |= PSOFlags::PSO_IMAGE_BASED_LIGHTING;
            }
            if (m_clusteredLighting) {
                globalPSOFlags |= PSOFlags::PSO_CLUSTERED_LIGHTING;
            }
            if (m_screenSpaceReflections || m_context.rayTracedReflectionsEnabled) {
                globalPSOFlags |= PSOFlags::PSO_SCREENSPACE_REFLECTIONS;
            }
            m_context.globalPSOFlags = globalPSOFlags;
        });

    }

    auto graphicsQueue = deviceManager.GetGraphicsQueue();
    auto computeQueue = deviceManager.GetComputeQueue();

    SyncOpenRenderGraphSettings(m_numFramesInFlight);

    {
        BT_ZONE_SCOPE("Renderer::Render::CLodVisibilityTelemetry");
        MaybeRequestCLodVisibilityTelemetry();
        MaybeRequestCLodVirtualShadowTelemetry();
        MaybeRequestCLodVirtualShadowFrameTelemetry();
    }
    runCapturedStage("RenderGraphExecute", [&]() {
        BT_ZONE_SCOPE("Renderer::Render::RenderGraphExecute");
        if (renderGraphBatchTraceEnabled) {
            ProbeGraphicsCommandListCreation(deviceManager.GetDevice(), "before RenderGraph::Execute");
            spdlog::info("Renderer: frame {} entering RenderGraph::Execute", m_totalFramesRendered);
        }
        const rhi::Backend activeBackend = deviceManager.GetBackend();
        try {
            br::telemetry::nvperf::BeginFrameCapture(
                activeBackend,
                deviceManager.GetDevice(),
                graphicsQueue,
                computeQueue,
                m_totalFramesRendered);
#if BASICRENDERER_ENABLE_NVPERF
            // NVPerf's per-pass ranges. The persistent executor records them from the graph's callbacks and never
            // reads the execution context's hooks, so they are set on the graph.
            m_frameHost->SetGpuPassRangeCallbacks(
                [activeBackend](rhi::CommandList commandList, rhi::Queue queue, const char* queueName, const char* passName) {
                    br::telemetry::nvperf::BeginPassRange(activeBackend, commandList, queue, queueName, passName);
                },
                [activeBackend](rhi::CommandList commandList, rhi::Queue queue) {
                    br::telemetry::nvperf::EndPassRange(activeBackend, commandList, queue);
                });
#endif
            // The frame graph: the host prepares its passes for this frame's inputs and submits them before returning.
            // m_frameInputs stays alive until the frame's GPU work completes.
            if (m_useGraphInstance) {
                auto prepared = m_frameInstance->Prepare(frameContext);
                org::PassExecutionContext passExecutionContext{};
                passExecutionContext.device = deviceManager.GetDevice();
                passExecutionContext.frameIndex = m_context.frameIndex;
                passExecutionContext.executionSlot = renderedFrameIndex;
                passExecutionContext.frameFenceValue = m_context.frameFenceValue;
                passExecutionContext.deltaTime = m_context.deltaTime;
                passExecutionContext.ownedHostData = m_frameInputs;
                passExecutionContext.hostData = passExecutionContext.ownedHostData.get();
                prepared.Submit(passExecutionContext);
            } else {
            // Each frame host epoch's graph, in frame order (M4 step 6: Visibility, Shading, Post, Overlay). Each graph's
            // after-execution work (CLod streaming's readback and async uploads, the texture streaming feedback: Shading's)
            // runs after its own execution, in the last graph that writes its sources.
            const auto executeBegin = std::chrono::steady_clock::now();
            const auto& hosts = br::render::FrameHostEpochs();
            // The frame's own hook (its diagnostics requests) runs once, in the first epoch's preparation.
            org::UpdateExecutionContext laterEpochs = frameContext;
            laterEpochs.beforeCompileFrame = {};
            (void)CheckFrameBuildSet();
            // The currency probe (diagnostic, M6 step 2): off unless BASICRENDERER_CURRENCY_PROBE is set.
            static const bool currencyProbe = br::runtime::renderer_settings::ReadTruthyEnvironmentFlag("BASICRENDERER_CURRENCY_PROBE");
            m_frameHost->SetCurrencyProbe(currencyProbe);
            for (size_t index = 0; index < hosts.size(); ++index) {
                const auto epochBegin = std::chrono::steady_clock::now();
                m_frameHost->ExecuteFrame(index == 0 ? frameContext : laterEpochs, {}, hosts[index], m_frameInputs);
                const auto& timings = m_frameHost->LastFrameTimings();
                RecordFrameGraphExecution(index, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - epochBegin).count(),
                    timings.waitUs / 1000.0, timings.updateUs / 1000.0, timings.executeUs / 1000.0);
            }
            // The frame boundary: everything this frame submitted is on the device queues. Runs and the runtime's
            // retirement wait only for runs a seal came after.
            m_frameHost->SealSubmitted();
            RecordFrameGraphExecution(SIZE_MAX, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - executeBegin).count());
            }
            // Texture readbacks and after-graph captures copy what the frame wrote, so they are submitted after the frame's last
            // execution, through the last graph's client.
            auto* lastGraph = m_useGraphInstance ? currentRenderGraph : PresentationGraph();
            if (m_pReadbackManager && lastGraph) {
                const auto graphicsSlot = lastGraph->GetQueueRegistry().FindGraphicsSlot();
                m_pReadbackManager->Submit(lastGraph->GetExternalClient(), org::ToUnderlying(graphicsSlot));
            }
            if (m_readbackCaptures && lastGraph) {
                const auto graphicsSlot = lastGraph->GetQueueRegistry().FindGraphicsSlot();
                m_readbackCaptures->SubmitAfterGraph(lastGraph->GetExternalClient(), org::ToUnderlying(graphicsSlot));
            }
            static const std::filesystem::path submittedCameraTracePath = [] {
                wchar_t* value = nullptr;
                size_t length = 0;
                _wdupenv_s(&value, &length, L"SARP_SUBMITTED_CAMERA_TELEMETRY_PATH");
                std::filesystem::path result = value && value[0] ? value : L"";
                std::free(value);
                return result;
            }();
            if (!submittedCameraTracePath.empty()) {
                const auto submittedData = currentRenderGraph->GetLastSubmittedFrameData();
                const auto* submitted = submittedData ? submittedData->Get<RenderContext>() : nullptr;
                const auto views = submitted ? submitted->viewFamily : nullptr;
                const auto primary = views ? std::find_if(views->views.begin(), views->views.end(),
                    [](const auto& view) { return view.primary; }) : decltype(views->views.begin()){};
                if (submitted && views && primary != views->views.end()) {
                    const auto previousInverse = DirectX::XMMatrixInverse(nullptr, primary->cameraInfo.prevView);
                    std::error_code fileError;
                    if (submittedCameraTracePath.has_parent_path())
                        std::filesystem::create_directories(submittedCameraTracePath.parent_path(), fileError);
                    const bool writeHeader = !std::filesystem::exists(submittedCameraTracePath, fileError) ||
                        std::filesystem::file_size(submittedCameraTracePath, fileError) == 0u;
                    std::ofstream output(submittedCameraTracePath, std::ios::app);
                    if (writeHeader)
                        output << "present_call,owned_frame,preparation_slot,view_snapshot_revision,x,y,z,prev_x,prev_y,prev_z,context_x,context_y,context_z,jitter_x,jitter_y\n";
                    output << m_totalFramesRendered << ',' << submitted->frameNumber << ','
                        << submitted->frameSlot << ',' << views->revision << ','
                        << primary->cameraInfo.positionWorldSpace.x << ','
                        << primary->cameraInfo.positionWorldSpace.y << ','
                        << primary->cameraInfo.positionWorldSpace.z << ','
                        << DirectX::XMVectorGetX(previousInverse.r[3]) << ','
                        << DirectX::XMVectorGetY(previousInverse.r[3]) << ','
                        << DirectX::XMVectorGetZ(previousInverse.r[3]) << ','
                        << submitted->primaryCamera.info.positionWorldSpace.x << ','
                        << submitted->primaryCamera.info.positionWorldSpace.y << ','
                        << submitted->primaryCamera.info.positionWorldSpace.z << ','
                        << submitted->primaryCamera.jitterPixelSpace.x << ','
                        << submitted->primaryCamera.jitterPixelSpace.y << '\n';
                }
            }
            if (renderGraphBatchTraceEnabled) {
                spdlog::info("Renderer: frame {} completed RenderGraph::Execute", m_totalFramesRendered);
            }
        }
        catch (const std::exception& ex) {
            spdlog::critical("Renderer: frame {} RenderGraph::Execute threw: {}", m_totalFramesRendered, ex.what());
            WriteRendererExceptionNote(
                "RenderGraphExecute",
                m_totalFramesRendered,
                renderedFrameIndex,
                m_currentFrameFenceValue,
                ex);
            spdlog::apply_all([](const std::shared_ptr<spdlog::logger>& logger) {
                logger->flush();
            });
            throw;
        }
    });

    // What Render presents: this frame's slot-owned presentation color and the swapchain image acquired for it.
    m_executedPresentationSlot = static_cast<uint8_t>(frameContext.preparationSlot);
    m_executedFrameIndex = renderedFrameIndex;
    m_frameExecuted = true;
}

void Renderer::SubmitUploadsWithoutFrame() {
    BT_ZONE_SCOPE("Renderer::SubmitUploadsWithoutFrame");
    if (!m_frameHost || m_useGraphInstance) return;
    auto* uploads = m_orgRuntime ? m_orgRuntime->Uploads().get() : nullptr;
    if (!uploads || !uploads->HasPendingUploads()) return;
    try {
        m_frameHost->ExecuteFrame(nullptr, {}, kUploadEpoch);
        m_frameHost->SealSubmitted();
    } catch (const std::exception& error) {
        spdlog::error("Renderer: submitting uploads without a frame failed: {}", error.what());
    }
}

void Renderer::Render() {
    BT_ZONE_SCOPE("Renderer::Render");

    const auto runCapturedStage = [this](const char* stageName, auto&& stageFn) {
        const auto stageStart = std::chrono::steady_clock::now();
        stageFn();
        const auto stageEnd = std::chrono::steady_clock::now();
        RecordFrameTaskStage(stageName, br::telemetry::CpuTaskDomain::MainThread, stageStart, stageEnd);
    };
    // Update submitted the frame graph (ExecuteFrameGraph). Without a submitted frame there is nothing to present.
    if (!m_frameExecuted || !currentRenderGraph) {
        return;
    }
    // The graph that presents: the last host epoch's, which has PresentationReadyPass (M4 step 5).
    org::RenderGraph* presentationGraph = m_useGraphInstance ? currentRenderGraph : PresentationGraph();
    if (!presentationGraph) {
        return;
    }
    m_frameExecuted = false;
    const bool renderGraphBatchTraceEnabled = SettingsManager::GetInstance().getSettingGetter<bool>("renderGraphBatchTraceEnabled")();
    const uint8_t renderedFrameIndex = m_executedFrameIndex;
    auto& deviceManager = DeviceManager::GetInstance();
    auto graphicsQueue = deviceManager.GetGraphicsQueue();

    if (m_submissionThread) {
        // The tail, Present and the frame fence's signal go behind the frame's executions on the submission thread.
        EnqueuePresent(presentationGraph);
    } else {
    if (!presentationGraph->GetLastPresentDependency()) {
        basic_telemetry::AddCounter("ORG.PresentationTail.DeferredForRecording");
        static auto lastDeferredLog = std::chrono::steady_clock::time_point{};
        if (const auto now = std::chrono::steady_clock::now(); now - lastDeferredLog > std::chrono::seconds(2)) {
            lastDeferredLog = now;
            spdlog::warn("Renderer: frame {} has no present dependency; presentation deferred", m_totalFramesRendered);
        }
        br::telemetry::nvperf::EndFrameCapture(
            deviceManager.GetBackend(), graphicsQueue, m_totalFramesRendered);
        PublishFrameTaskGraphCapture();
        FrameMark;
        return;
    }

    // Acquire/bind the swapchain image only after scene recording and FIFO
    // submission selected the logical frame. PresentationColor is slot-owned
    // and was left in CopySource by PresentationReadyPass.
    const auto sourceSlot = static_cast<size_t>(m_executedPresentationSlot);
    if (sourceSlot >= m_presentationColorResources.size()
        || renderedFrameIndex >= m_backbufferResources.size()
        || renderedFrameIndex >= m_commandAllocators.size()
        || renderedFrameIndex >= m_commandLists.size()) {
        throw std::runtime_error("Presentation tail has incomplete slot ownership");
    }
    auto presentationSource = m_presentationColorResources[sourceSlot];
    auto currentBackbufferResource = m_backbufferResources[renderedFrameIndex];
    if (!presentationSource || !currentBackbufferResource
        || !presentationSource->HasValidBackingResource()
        || !currentBackbufferResource->HasHandle()) {
        throw std::runtime_error("Presentation tail has invalid resource bindings");
    }
    SubmitPresentationTail(*presentationGraph, presentationSource, currentBackbufferResource);
    basic_telemetry::AddCounter("ORG.PresentationTail.Submitted");

    // Present the frame
    rhi::Result presentResult = rhi::Result::Ok;
    runCapturedStage("Present", [&]() {
        BT_ZONE_SCOPE("Renderer::Render::Present");
        if (renderGraphBatchTraceEnabled) {
            spdlog::info("Renderer: frame {} calling Present for slot {}", m_totalFramesRendered, renderedFrameIndex);
        }
        // The tail was submitted on the presentation queue immediately before
        // this call, so queue order covers the copy and Present transition.
        const auto presentBegin = std::chrono::steady_clock::now();
        presentResult = m_swapChain->Present(!m_allowTearing);
        m_lastPresentReturned = std::chrono::steady_clock::now();
        m_lastPresentMs = std::chrono::duration<double, std::milli>(m_lastPresentReturned - presentBegin).count();
        m_frameGraphExecutionTiming.inlinePresentMs += m_lastPresentMs;
        // Where the render thread waits for the display (diagnostic): its frame-start wait for the GPU and Present, per 600 frames.
        static double presentSum = 0.0, waitSum = 0.0;
        static uint32_t frames = 0;
        presentSum += m_lastPresentMs;
        waitSum += m_lastWaitForFrameMs;
        if (++frames == 600) {
            spdlog::info("Renderer: over 600 frames the render thread waited {:.2f} ms per frame for the GPU at the frame's start "
                "(WaitForFrame) and {:.2f} ms in Present", waitSum / frames, presentSum / frames);
            presentSum = waitSum = 0.0;
            frames = 0;
        }
    });
	if (presentResult == rhi::Result::ModeChanged) HandlePresentModeChange(m_swapchainGeneration);
	if (presentResult != rhi::Result::Ok && presentResult != rhi::Result::ModeChanged) {
		spdlog::error("Renderer: swapchain presentation failed with {}", rhi::ResultName(presentResult));
		return;
	}

    runCapturedStage("SignalFence", [&]() {
        BT_ZONE_SCOPE("Renderer::Render::SignalFence");
        // The renderer's slot: WaitForFrame waits on it before the slot's resources are written again.
        SignalFence(graphicsQueue, m_executedPresentationSlot);
        // The capture copies this frame recorded complete with its fence.
        if (m_readbackCaptures) m_readbackCaptures->NoteFrameSubmitted(m_frameFence.Get(), m_currentFrameFenceValue);
        br::telemetry::nvperf::EndFrameCapture(deviceManager.GetBackend(), graphicsQueue, m_totalFramesRendered);
    });
    }

    AdvanceFrameIndex();

    runCapturedStage("ReadbackRequests", [&]() {
        if (m_readbackCaptures) {
            BT_ZONE_SCOPE("Renderer::Render::ReadbackRequests");
            m_readbackCaptures->ProcessCompleted();
        }
        if (m_pReadbackManager) {
            m_pReadbackManager->ProcessReadbackRequests(); // Save images to disk if requested
        }
    });
    PublishFrameTaskGraphCapture();
    FrameMark;
    // The render thread's whole frame (M7 step 5): from Update's start to here.
    auto& timing = m_frameGraphExecutionTiming;
    if (m_updateBegin != std::chrono::steady_clock::time_point{}) {
        timing.renderThreadFrameMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_updateBegin).count();
        timing.waitForFrameMs += m_lastWaitForFrameMs;
        ++timing.renderThreadFrames;
    }
}

void Renderer::SubmitPresentationTail(org::RenderGraph& presentationGraph, const std::shared_ptr<org::PixelBuffer>& presentationSource,
    const std::shared_ptr<org::ExternalTextureResource>& backbuffer) {
    // The copy into the backbuffer goes through the frame graph's external client: the runtime orders it after the
    // frame's write of PresentationColor (its wait and transition), and keeps PresentationColor from its next writer
    // until the copy is done. The backbuffer is not a graph resource; its own barriers leave it ready to present, on
    // the queue Present uses.
    {
        BT_ZONE_SCOPE("Renderer::Render::PresentationTail");
        const auto graphicsSlot = static_cast<uint32_t>(
            static_cast<uint8_t>(presentationGraph.GetQueueRegistry().FindGraphicsSlot()));
        const org::ResourceState copySource{rhi::ResourceAccessType::CopySource,
            rhi::AccessToLayout(rhi::ResourceAccessType::CopySource, true),
            rhi::RenderSyncFromAccess(rhi::ResourceAccessType::CopySource)};
        const std::array<org::ExternalClient::Access, 1> accesses{{{presentationSource.get(), {}, copySource}}};
        presentationGraph.GetExternalClient().Submit(graphicsSlot, accesses,
            [&](rhi::CommandList& list) {
                rhi::TextureSubresourceRange presentationRange{};
                presentationRange.mipCount = 1;
                presentationRange.layerCount = 1;
                rhi::TextureBarrier backbufferToCopy{};
                backbufferToCopy.texture = backbuffer->GetHandle();
                backbufferToCopy.range = presentationRange;
                backbufferToCopy.beforeSync = rhi::ResourceSyncState::All;
                backbufferToCopy.afterSync = rhi::ResourceSyncState::Copy;
                backbufferToCopy.beforeAccess = rhi::ResourceAccessType::Present;
                backbufferToCopy.afterAccess = rhi::ResourceAccessType::CopyDest;
                backbufferToCopy.beforeLayout = rhi::ResourceLayout::Present;
                backbufferToCopy.afterLayout = rhi::ResourceLayout::CopyDest;
                rhi::BarrierBatch beforePresentationCopy{};
                beforePresentationCopy.textures = {&backbufferToCopy, 1};
                list.Barriers(beforePresentationCopy);

                rhi::TextureCopyRegion sourceRegion{};
                sourceRegion.texture = presentationSource->GetAPIResource().GetHandle();
                rhi::TextureCopyRegion destinationRegion{};
                destinationRegion.texture = backbuffer->GetHandle();
                // A backbuffer of another size than the presentation surface (a swapchain the RHI made again at present, before
                // the graphs for its size attach): the copy takes the part both have.
                if (backbuffer->GetWidth() != presentationSource->GetWidth()
                    || backbuffer->GetHeight() != presentationSource->GetHeight()) {
                    sourceRegion.width = destinationRegion.width = (std::min)(backbuffer->GetWidth(), presentationSource->GetWidth());
                    sourceRegion.height = destinationRegion.height = (std::min)(backbuffer->GetHeight(), presentationSource->GetHeight());
                }
                list.CopyTextureRegion(destinationRegion, sourceRegion);

                auto backbufferToPresent = backbufferToCopy;
                backbufferToPresent.beforeSync = rhi::ResourceSyncState::Copy;
                backbufferToPresent.afterSync = rhi::ResourceSyncState::All;
                backbufferToPresent.beforeAccess = rhi::ResourceAccessType::CopyDest;
                backbufferToPresent.afterAccess = rhi::ResourceAccessType::Present;
                backbufferToPresent.beforeLayout = rhi::ResourceLayout::CopyDest;
                backbufferToPresent.afterLayout = rhi::ResourceLayout::Present;
                rhi::BarrierBatch afterPresentationCopy{};
                afterPresentationCopy.textures = {&backbufferToPresent, 1};
                list.Barriers(afterPresentationCopy);
            }, {}, presentationSource);
    }
}

void Renderer::HandlePresentModeChange(uint64_t generationAtPresent) {
	// The client extent in the window's own DPI awareness, as its WM_SIZE reports it: the thread may be DPI aware (ImGui's
	// EnableDpiAwareness) while a DPI-unaware window (the SARP host's) gets logical sizes, which a query in the thread's
	// awareness would scale to physical pixels (M5 step 8).
	RECT clientRect{};
	bool clientKnown = false;
	if (m_hwnd) {
		const DPI_AWARENESS_CONTEXT previous = SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(m_hwnd));
		clientKnown = GetClientRect(m_hwnd, &clientRect) != FALSE;
		if (previous) SetThreadDpiAwarenessContext(previous);
	}
	if (clientKnown) {
		const UINT clientWidth = static_cast<UINT>((std::max)(clientRect.right - clientRect.left, 0L));
		const UINT clientHeight = static_cast<UINT>((std::max)(clientRect.bottom - clientRect.top, 0L));
		if (clientWidth != 0 && clientHeight != 0) {
			spdlog::info(
				"Renderer: presentation reported mode change; the swapchain is made again for client extent {}x{} at the next "
				"consolidation point",
				clientWidth,
				clientHeight);
			// A resolution request as WM_SIZE's (M5 step 4), made even at the same size. The frame was submitted: it is
			// signalled and the frame index advances as for any other.
			m_modeChangeSwapchainGeneration = generationAtPresent;
			m_frameRevisions.RequestOutputSize(clientWidth, clientHeight, "presentation mode change", true);
			// The RHI made the swapchain again (Vulkan: out of date, at the surface's extent, which is the client's): its
			// images are new. The backbuffer wrappers take them now, and the presentation copy clips to the smaller size until
			// the graphs built for the window's size attach (M5 step 8).
			bool remade = false;
			for (UINT n = 0; n < m_numFramesInFlight && n < renderTargets.size(); ++n) {
				const auto image = m_swapChain->Image(n);
				if (image.index != renderTargets[n].index || image.generation != renderTargets[n].generation) remade = true;
			}
			if (remade) {
				uint32_t width = clientWidth, height = clientHeight;
				(void)m_swapChain->Extent(width, height);
				SettingsManager::GetInstance().getSettingSetter<DirectX::XMUINT2>("outputResolution")({ width, height });
				CreateRTVs();
				++m_swapchainGeneration;
				++m_resolutionStats.swapchainResizes;
				spdlog::info("Renderer: the swapchain was made again at present ({}x{}); the backbuffers follow", width, height);
			}
		}
		else {
			spdlog::warn("Renderer: presentation reported mode change but no non-zero client extent is available");
		}
	}
	else {
		spdlog::warn("Renderer: presentation reported mode change but no non-zero client extent is available");
	}
}

void Renderer::EnqueuePresent(org::RenderGraph* presentationGraph) {
    BT_ZONE_SCOPE("Renderer::Render::EnqueuePresent");
    auto& shared = *m_presentShared;
    auto& timing = m_frameGraphExecutionTiming;
    // At most one present queued ahead of the submission thread (decision U3): the previous frame's closure must have run.
    // Normally it has, so this costs nothing. It also makes what that closure reported safe to read below.
    if (shared.passed.load(std::memory_order_acquire) < m_presentsQueued) {
        BT_ZONE_SCOPE("Renderer::Render::PresentLeadWait");
        const auto waitBegin = std::chrono::steady_clock::now();
        for (uint64_t passed = shared.passed.load(std::memory_order_acquire); passed < m_presentsQueued;
             passed = shared.passed.load(std::memory_order_acquire))
            shared.passed.wait(passed, std::memory_order_acquire);
        timing.presentLeadWaitMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitBegin).count();
        ++timing.presentLeadWaits;
    }
    // A resize the previous closure made (an attach's, m_resizeAtPresent): its numbers, and the size the swapchain has.
    if (shared.resized) {
        shared.resized = false;
        auto& stats = m_resolutionStats;
        stats.lastSwapchainWaitMs = shared.resizeWaitMs;
        stats.maxSwapchainWaitMs = (std::max)(stats.maxSwapchainWaitMs, stats.lastSwapchainWaitMs);
        stats.lastResizeBuffersMs = shared.resizeBuffersMs;
        stats.maxResizeBuffersMs = (std::max)(stats.maxResizeBuffersMs, stats.lastResizeBuffersMs);
        if (shared.resizeFailed) {
            spdlog::critical("Renderer: the swapchain could not be resized to {}x{} in the attach frame's present", shared.resizedWidth,
                shared.resizedHeight);
        } else {
            auto& settings = SettingsManager::GetInstance();
            const auto size = settings.getSettingGetter<DirectX::XMUINT2>("outputResolution")();
            if (size.x != shared.resizedWidth || size.y != shared.resizedHeight) {
                spdlog::info("Renderer: the swapchain is {}x{}, not the {}x{} asked for", shared.resizedWidth, shared.resizedHeight, size.x, size.y);
                settings.getSettingSetter<DirectX::XMUINT2>("outputResolution")({ shared.resizedWidth, shared.resizedHeight });
            }
        }
    }
    // A mode change the previous present reported: handled here, before the next present, with no closure in flight.
    if (shared.modeChanged.exchange(false, std::memory_order_acq_rel)) HandlePresentModeChange(shared.modeChangeGeneration);

    const auto sourceSlot = static_cast<size_t>(m_executedPresentationSlot);
    if (sourceSlot >= m_presentationColorResources.size() || sourceSlot >= m_frameFenceValues.size())
        throw std::runtime_error("Presentation tail has incomplete slot ownership");
    auto presentationSource = m_presentationColorResources[sourceSlot];
    if (!presentationSource || !presentationSource->HasValidBackingResource())
        throw std::runtime_error("Presentation tail has invalid resource bindings");
    // The slot's frame fence value is reserved here, so WaitForFrame and the captures know it before the closure signals it.
    // Closures run in order, so the values are signalled in increasing order.
    const uint64_t fenceValue = ++m_currentFrameFenceValue;
    m_frameFenceValues[sourceSlot] = fenceValue;
    const auto resize = std::exchange(m_pendingSwapchainResize, std::nullopt);
    if (resize) {
        ++m_swapchainGeneration;
        ++m_resolutionStats.swapchainResizes;
    }
    auto& deviceManager = DeviceManager::GetInstance();
    const bool trace = SettingsManager::GetInstance().getSettingGetter<bool>("renderGraphBatchTraceEnabled")();
    ++m_presentsQueued;
    m_frameHost->EnqueueOnSubmitThread([this, presentationGraph, presentationSource = std::move(presentationSource), fenceValue,
        vsync = !m_allowTearing, frameNumber = m_totalFramesRendered, generation = m_swapchainGeneration, resize, trace,
        graphicsQueue = deviceManager.GetGraphicsQueue(), backend = deviceManager.GetBackend()]() mutable {
        BT_ZONE_SCOPE("Renderer::PresentOnSubmissionThread");
        auto& shared = *m_presentShared;
        rhi::Result presentResult = rhi::Result::Ok;
        try {
            if (resize) ResizeSwapChainAtPresent(fenceValue, resize->width, resize->height, trace);
            // The presentation graph's execution ran just before this on the thread. Without a present dependency (it failed, or
            // recorded nothing to present) the frame is not presented.
            if (!presentationGraph->GetLastPresentDependency()) {
                basic_telemetry::AddCounter("ORG.PresentationTail.DeferredForRecording");
                static auto lastDeferredLog = std::chrono::steady_clock::time_point{};
                if (const auto now = std::chrono::steady_clock::now(); now - lastDeferredLog > std::chrono::seconds(2)) {
                    lastDeferredLog = now;
                    spdlog::warn("Renderer: frame {} has no present dependency; presentation deferred", frameNumber);
                }
            } else {
                // The image the swapchain gives now: every present is made on this thread, so it is the one this present uses.
                const auto image = m_swapChain->CurrentImageIndex();
                if (image >= m_backbufferResources.size()) throw std::runtime_error("Presentation tail has no backbuffer for the acquired image");
                const auto& backbuffer = m_backbufferResources[image];
                if (!backbuffer || !backbuffer->HasHandle()) throw std::runtime_error("Presentation tail has invalid resource bindings");
                if (trace) spdlog::info("Renderer: frame {} calling Present for image {} on the submission thread", frameNumber, image);
                {
                    BT_ZONE_SCOPE("Renderer::PresentationTail");
                    SubmitPresentationTail(*presentationGraph, presentationSource, backbuffer);
                }
                basic_telemetry::AddCounter("ORG.PresentationTail.Submitted");
                BT_ZONE_SCOPE("Renderer::Present");
                const auto presentBegin = std::chrono::steady_clock::now();
                presentResult = m_swapChain->Present(vsync);
                m_lastPresentReturned = std::chrono::steady_clock::now();
                m_lastPresentMs = std::chrono::duration<double, std::milli>(m_lastPresentReturned - presentBegin).count();
                static double presentSum = 0.0;
                static uint32_t presents = 0;
                presentSum += m_lastPresentMs;
                if (++presents == 600) {
                    spdlog::info("Renderer: over 600 frames the submission thread spent {:.2f} ms per frame in Present", presentSum / presents);
                    presentSum = 0.0;
                    presents = 0;
                }
            }
        } catch (const std::exception& error) {
            spdlog::error("Renderer: frame {}'s present on the submission thread failed: {}", frameNumber, error.what());
        }
        // The fence is signalled whatever happened above: WaitForFrame waits for it.
        if (rhi::Failed(graphicsQueue.Signal({ m_frameFence->GetHandle(), fenceValue })))
            spdlog::error("Renderer: signalling the frame fence value {} failed on the submission thread", fenceValue);
        br::telemetry::nvperf::EndFrameCapture(backend, graphicsQueue, frameNumber);
        if (presentResult == rhi::Result::ModeChanged) {
            shared.modeChangeGeneration = generation;
            shared.modeChanged.store(true, std::memory_order_release);
        } else if (presentResult != rhi::Result::Ok) {
            spdlog::error("Renderer: swapchain presentation failed with {}", rhi::ResultName(presentResult));
        }
        shared.passed.fetch_add(1, std::memory_order_release);
        shared.passed.notify_all();
    });
    // The capture copies this frame recorded complete with its fence.
    if (m_readbackCaptures) m_readbackCaptures->NoteFrameSubmitted(m_frameFence.Get(), fenceValue);
}
