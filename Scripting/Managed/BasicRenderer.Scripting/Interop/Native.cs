using System;
using System.Runtime.InteropServices;
using System.Text;

namespace BasicRenderer.Scripting.Interop;

/// <summary>
/// The native API table and safe wrappers over it. Every call happens on the host's main thread, inside a tick or an
/// entry point. Without a table (managed unit tests) the wrappers fall back to the console and no-ops.
/// </summary>
internal static unsafe class Native
{
    private static BRScriptApiV1* s_api;

    public static bool IsAttached => s_api != null;

    public static void Attach(BRScriptApiV1* api) => s_api = api;

    public static void Detach() => s_api = null;

    /// <summary>Test hook: receives log lines when no native table is attached.</summary>
    public static Action<NativeLogLevel, string>? DetachedLogSink { get; set; }

    public static void Log(NativeLogLevel level, string message)
    {
        if (s_api == null) {
            if (DetachedLogSink != null) DetachedLogSink(level, message);
            else Console.Error.WriteLine($"[script:{level.ToString().ToLowerInvariant()}] {message}");
            return;
        }
        fixed (byte* text = Utf8(message)) {
            s_api->Log(s_api->Context, (int)level, text);
        }
    }

    public static void ReportState(ScenarioState state, string? resultJson)
    {
        if (s_api == null) return;
        fixed (byte* json = resultJson == null ? null : Utf8(resultJson)) {
            s_api->ReportState(s_api->Context, (int)state, json);
        }
    }

    public static void RequestExit(int exitCode)
    {
        if (s_api == null) return;
        s_api->RequestExit(s_api->Context, exitCode);
    }

    public static string LastError()
    {
        if (s_api == null) return string.Empty;
        int length = s_api->GetLastError(s_api->Context, null, 0);
        if (length <= 0) return string.Empty;
        var buffer = new byte[length + 1];
        fixed (byte* p = buffer) {
            s_api->GetLastError(s_api->Context, p, buffer.Length);
        }
        return Encoding.UTF8.GetString(buffer, 0, length);
    }

    /// <summary>A host extension table by name, or null. See BRScriptApiV1::get_extension.</summary>
    public static void* GetExtension(string name, uint minimumVersion)
    {
        if (s_api == null) return null;
        fixed (byte* text = Utf8(name)) {
            return s_api->GetExtension(s_api->Context, text, minimumVersion);
        }
    }

    /// <summary>Throws ScriptApiException for a failing status, with the host's last error.</summary>
    public static void Check(int status, string operation)
    {
        if (status >= AbiConstants.Ok) return;
        throw new ScriptApiException(status, operation, LastError());
    }

    /// <summary>NUL-terminated UTF-8, for passing to native code under <c>fixed</c>.</summary>
    public static byte[] Utf8(string text)
    {
        var bytes = new byte[Encoding.UTF8.GetByteCount(text) + 1];
        Encoding.UTF8.GetBytes(text, 0, text.Length, bytes, 0);
        return bytes;
    }

    public static string? FromUtf8(byte* text) => text == null ? null : Marshal.PtrToStringUTF8((IntPtr)text);
}

/// <summary>A native API entry failed. Status is one of the BR_SCRIPT_E_* codes.</summary>
public sealed class ScriptApiException(int status, string operation, string detail)
    : Exception(string.IsNullOrEmpty(detail) ? $"{operation} failed ({status})" : $"{operation} failed ({status}): {detail}")
{
    public int Status { get; } = status;
}
