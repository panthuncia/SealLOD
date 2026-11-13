#pragma once

// Named JSON commands: what the control pipe (SamplingControlServer.h) and C# scenarios (ctx.Command) call. An
// application registers each command once; every caller then reaches the same handler. Single-threaded: register,
// invoke and serve on the application's main thread.

#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <BasicRenderer/Diagnostics/SamplingControlServer.h>

namespace br::diagnostics {

struct ControlRequest {
    std::string_view command;       // the name it was invoked by (handlers registered under several names use it)
    const nlohmann::json& args;     // the request document: "command", "request_id" and the command's own fields
};

class ControlCommandRegistry {
public:
    // Returns the reply's fields; throws std::exception to fail the request with that message.
    using Handler = std::function<nlohmann::json(const ControlRequest&)>;

    // Registers (or replaces) a command.
    void Register(std::string name, Handler handler) { m_handlers[std::move(name)] = std::move(handler); }

    void Unregister(const std::string& name) { m_handlers.erase(name); }

    bool Contains(std::string_view name) const { return m_handlers.find(name) != m_handlers.end(); }

    std::vector<std::string> Names() const
    {
        std::vector<std::string> names;
        names.reserve(m_handlers.size());
        for (const auto& [name, handler] : m_handlers) names.push_back(name);
        return names;
    }

    // Runs request["command"]. The reply is the handler's fields plus "ok": true, or {"ok": false, "error": ...} when
    // the command is unknown or its handler throws. "request_id" is echoed when the request has one.
    nlohmann::json Invoke(const nlohmann::json& request) const
    {
        nlohmann::json reply = nlohmann::json::object();
        try {
            if (const auto id = request.find("request_id"); id != request.end()) reply["request_id"] = *id;
            const std::string command = request.value("command", "");
            const auto found = m_handlers.find(command);
            if (found == m_handlers.end()) throw std::runtime_error("unknown command '" + command + "'");
            nlohmann::json fields = found->second(ControlRequest{ command, request });
            if (fields.is_object()) {
                for (auto& [key, value] : fields.items()) reply[key] = std::move(value);
            } else if (!fields.is_null()) {
                reply["result"] = std::move(fields);
            }
            reply["ok"] = true;
        } catch (const std::exception& exception) {
            reply["ok"] = false;
            reply["error"] = exception.what();
        }
        return reply;
    }

    // Runs a command with these arguments (an object; may be null).
    nlohmann::json Invoke(std::string_view command, const nlohmann::json& args) const
    {
        nlohmann::json request = args.is_object() ? args : nlohmann::json::object();
        request["command"] = std::string(command);
        return Invoke(request);
    }

    // Answers every request the pipe has queued.
    void Serve(br::telemetry::sampling::ControlServer& server) const
    {
        for (const auto& request : server.DrainRequests()) {
            request->response.set_value(Invoke(request->document));
        }
    }

private:
    std::map<std::string, Handler, std::less<>> m_handlers;
};

} // namespace br::diagnostics
