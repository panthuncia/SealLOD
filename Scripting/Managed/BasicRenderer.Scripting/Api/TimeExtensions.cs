using System;

namespace BasicRenderer.Scripting;

/// <summary>Readable durations: <c>3.Seconds()</c>, <c>250.Milliseconds()</c>.</summary>
public static class TimeExtensions
{
    public static TimeSpan Seconds(this double value) => TimeSpan.FromSeconds(value);
    public static TimeSpan Seconds(this float value) => TimeSpan.FromSeconds(value);
    public static TimeSpan Seconds(this int value) => TimeSpan.FromSeconds(value);
    public static TimeSpan Milliseconds(this double value) => TimeSpan.FromMilliseconds(value);
    public static TimeSpan Milliseconds(this int value) => TimeSpan.FromMilliseconds(value);
}
