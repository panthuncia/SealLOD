using System;
using System.Collections.Generic;
using BasicRenderer.Scripting.Interop;

namespace BasicRenderer.Scripting;

/// <summary>
/// The renderer's settings by name (the ones the menu shows: "enableBloom", "pathTracing", ...). Only bool, int, uint,
/// byte and float settings are reachable. What a scenario changes is put back when it ends, unless
/// <see cref="KeepChanges"/> is set.
/// </summary>
public sealed class SettingsApi
{
    private readonly Dictionary<string, (int Type, object Value)> _originals = new();

    internal SettingsApi() { }

    /// <summary>Leave the scenario's changes in place when it ends.</summary>
    public bool KeepChanges { get; set; }

    public bool GetBool(string name) => (bool)Native.Api.GetSetting(name, SettingTypes.Bool);
    public float GetFloat(string name) => (float)Native.Api.GetSetting(name, SettingTypes.Float);
    public int GetInt(string name) => (int)Native.Api.GetSetting(name, SettingTypes.Int32);
    public uint GetUInt(string name) => (uint)Native.Api.GetSetting(name, SettingTypes.UInt32);
    public byte GetByte(string name) => (byte)(uint)Native.Api.GetSetting(name, SettingTypes.UInt8);

    public void Set(string name, bool value) => Write(name, SettingTypes.Bool, value);
    public void Set(string name, float value) => Write(name, SettingTypes.Float, value);
    public void Set(string name, double value) => Write(name, SettingTypes.Float, (float)value);
    public void Set(string name, uint value) => WriteInteger(name, value, SettingTypes.UInt32, SettingTypes.UInt8, SettingTypes.Int32);
    /// <summary>An integer setting of whichever width the setting has (int, uint or byte).</summary>
    public void Set(string name, int value) => WriteInteger(name, value, SettingTypes.Int32, SettingTypes.UInt32, SettingTypes.UInt8);

    /// <summary>Reads or writes a setting whatever its type: <c>ctx.Settings["enableBloom"] = false;</c></summary>
    public object this[string name]
    {
        get
        {
            foreach (var type in new[] { SettingTypes.Bool, SettingTypes.Float, SettingTypes.UInt32, SettingTypes.Int32, SettingTypes.UInt8 }) {
                try {
                    return Native.Api.GetSetting(name, type);
                } catch (ScriptApiException e) when (e.Status == AbiConstants.TypeMismatch) {
                }
            }
            throw new ScriptApiException(AbiConstants.TypeMismatch, $"reading setting '{name}'", "its type is not one scripts can use");
        }
        set
        {
            switch (value) {
            case bool b: Set(name, b); break;
            case float f: Set(name, f); break;
            case double d: Set(name, d); break;
            case int i: Set(name, i); break;
            case uint u: Set(name, u); break;
            case byte by: Set(name, (uint)by); break;
            default: throw new ArgumentException($"setting '{name}': a {value?.GetType().Name ?? "null"} value is not bool, int, uint or float");
            }
        }
    }

    private void WriteInteger(string name, object value, params int[] types)
    {
        foreach (var type in types) {
            try {
                Write(name, type, value);
                return;
            } catch (ScriptApiException e) when (e.Status == AbiConstants.TypeMismatch && type != types[^1]) {
            }
        }
    }

    private void Write(string name, int type, object value)
    {
        if (!_originals.ContainsKey(name)) {
            // Read first: it also checks the type, so a mismatch records nothing.
            var original = Native.Api.GetSetting(name, type);
            _originals[name] = (type, original);
        }
        Native.Api.SetSetting(name, type, value);
    }

    /// <summary>Puts back what the scenario changed. Called when it ends.</summary>
    internal void Restore()
    {
        if (KeepChanges || !Native.IsAttached) return;
        foreach (var (name, (type, value)) in _originals) {
            try {
                Native.Api.SetSetting(name, type, value);
            } catch (ScriptApiException e) {
                Native.Log(NativeLogLevel.Warn, $"could not restore setting '{name}': {e.Message}");
            }
        }
        _originals.Clear();
    }
}
