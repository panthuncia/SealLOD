#pragma once

// The settings frontend for automation: settings.list, settings.get and settings.set, on any ControlCommandRegistry (the
// control pipe, C# scenarios). Values travel as JSON: bools, numbers (an enum as its underlying integer), strings, and lists
// (vectors, and x/y points such as renderResolution). Callback settings are listed but cannot be read or written.
//
//   settings.list {"match": "clod"}               -> {"settings": [{"name", "type", "value"}, ...]}   (match: a substring,
//                                                                                                     case-insensitive)
//   settings.get  {"name": n} | {"names": [...]}  -> {"value": v} | {"values": {n: v, ...}}
//   settings.set  {"name": n, "value": v} | {"values": {n: v, ...}}
//                                                 -> {"value": v (single form), "values": {n: v read back},
//                                                     "changed": {every setting whose value changed, dependents included}}
//
// A write that does not change the value runs no observers. Writes take effect as each observer decides; graph changes are
// answered at the renderer's next consolidation point (Renderer::DescribeGraphRevisions shows them).

#include <string>

#include <nlohmann/json.hpp>

#include <BasicRenderer/Diagnostics/ControlCommandRegistry.h>
#include <BasicRenderer/Extensions/SettingAccess.h>

namespace br::diagnostics {

nlohmann::json SettingValueToJson(const br::extensions::SettingValue& value);
// Throws std::runtime_error for JSON that has no SettingValue form (objects, mixed lists, null).
br::extensions::SettingValue SettingValueFromJson(const nlohmann::json& value);

void RegisterSettingsCommands(ControlCommandRegistry& commands);

} // namespace br::diagnostics
