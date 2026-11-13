using System;
using System.IO;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.Json.Serialization;
using System.Threading.Tasks;
using BasicRenderer.Scripting.Interop;

namespace BasicRenderer.Scripting;

/// <summary>A control command answered <c>"ok": false</c>.</summary>
public sealed class CommandFailedException(string command, string error) : Exception($"{command}: {error}")
{
    public string Command { get; } = command;
}

/// <summary>
/// The host's control commands: the same ones the control pipe takes (TESTING.md lists them: lens.set,
/// pathtracing.set, upscaler.get, ...). Arguments are an object whose properties are the command's fields:
/// <c>ctx.Commands.Invoke("pathtracing.set", new { enabled = true })</c>.
/// </summary>
public sealed class CommandsApi
{
    private static readonly JsonSerializerOptions s_options = new() { DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull };
    private readonly ScenarioContext _ctx;

    internal CommandsApi(ScenarioContext ctx) => _ctx = ctx;

    /// <summary>Runs a command and returns its reply. Throws <see cref="CommandFailedException"/> when it fails.</summary>
    public JsonElement Invoke(string command, object? args = null)
    {
        var reply = InvokeRaw(command, args);
        if (!reply.TryGetProperty("ok", out var ok) || !ok.GetBoolean()) {
            var error = reply.TryGetProperty("error", out var e) ? e.ToString() : reply.ToString();
            throw new CommandFailedException(command, error);
        }
        return reply;
    }

    /// <summary>Runs a command; false (with the reply) when it fails.</summary>
    public bool TryInvoke(string command, object? args, out JsonElement reply)
    {
        reply = InvokeRaw(command, args);
        return reply.TryGetProperty("ok", out var ok) && ok.GetBoolean();
    }

    /// <summary>
    /// Runs a command that answers with a job (capture.hdr, pso.recompile, ...) and polls job.status once a frame until
    /// the job is published. Returns the final job object; throws when the job fails or the timeout passes.
    /// </summary>
    public async Task<JsonElement> RunJob(string command, object? args = null, TimeSpan? timeout = null)
    {
        var reply = Invoke(command, args);
        if (!reply.TryGetProperty("job_id", out var id)) throw new CommandFailedException(command, "the reply has no job_id");
        ulong jobId = id.GetUInt64();
        double deadline = _ctx.Time + (timeout ?? TimeSpan.FromSeconds(300)).TotalSeconds;
        for (;;) {
            await _ctx.NextFrame();
            var status = Invoke("job.status", new { job_id = jobId });
            var job = status.GetProperty("job");
            var state = job.TryGetProperty("state", out var s) ? s.GetString() : null;
            if (state == "published") return job.Clone();
            if (state == "failed") {
                var error = job.TryGetProperty("error", out var e) ? e.ToString() : "failed";
                throw new CommandFailedException(command, $"job {jobId} failed: {error}");
            }
            if (_ctx.Time >= deadline) throw new TimeoutException($"{command}: job {jobId} still '{state}' after {(timeout ?? TimeSpan.FromSeconds(300)).TotalSeconds:0} s");
        }
    }

    private JsonElement InvokeRaw(string command, object? args)
    {
        var request = args == null ? new JsonObject() : JsonSerializer.SerializeToNode(args, s_options) as JsonObject
            ?? throw new ArgumentException("command arguments must be an object (an anonymous type, a dictionary, ...)", nameof(args));
        request["command"] = command;
        var replyText = Native.Api.InvokeCommand(request.ToJsonString());
        using var document = JsonDocument.Parse(replyText);
        return document.RootElement.Clone();
    }
}

/// <summary>Frame captures, through the host's capture commands.</summary>
public sealed class CaptureApi
{
    private readonly ScenarioContext _ctx;

    internal CaptureApi(ScenarioContext ctx) => _ctx = ctx;

    /// <summary>
    /// Writes one frame's linear radiance as an RGBA32F DDS (capture.hdr, TESTING.md). <paramref name="samples"/> waits
    /// for that many path-traced samples (needs accumulation on); <paramref name="target"/> is "accumulation" or "hdr".
    /// A relative path is relative to the host's working directory (the demo's folder), as on the control pipe.
    /// Returns the finished job.
    /// </summary>
    public Task<JsonElement> Hdr(string path, int samples = 0, string? target = null, TimeSpan? timeout = null) =>
        _ctx.Commands.RunJob("capture.hdr", new { path = Path.GetFullPath(path), spp = samples, target }, timeout);
}
