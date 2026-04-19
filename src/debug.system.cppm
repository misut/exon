export module debug.system;
import std;
import cppx.env.system;
import debug;
import toolchain;

export namespace debug::system {

using FindOnPath = std::function<std::optional<std::string>(std::string_view)>;

namespace detail {

std::string join_names(std::vector<debug::DebuggerKind> const& candidates) {
    auto joined = std::string{};
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (i > 0)
            joined += ", ";
        joined += debug::debugger_kind_name(candidates[i]);
    }
    return joined;
}

bool is_path_like(std::string_view value) {
    auto path = std::filesystem::path{std::string{value}};
    return path.is_absolute() || path.has_parent_path();
}

FindOnPath path_finder() {
    return [](std::string_view name) -> std::optional<std::string> {
        if (auto found = cppx::env::system::find_in_path(name))
            return found->string();
        return std::nullopt;
    };
}

} // namespace detail

inline auto resolve_debugger(std::string_view requested,
                             toolchain::Platform const& host,
                             FindOnPath const& find_on_path)
    -> std::expected<debug::DebuggerProgram, std::string> {
    auto request = requested.empty() ? std::string_view{"auto"} : requested;
    if (request == "auto") {
        auto candidates = debug::auto_debugger_candidates(host);
        for (auto kind : candidates) {
            auto name = debug::debugger_kind_name(kind);
            if (auto found = find_on_path(name)) {
                return debug::DebuggerProgram{
                    .program = *found,
                    .kind = kind,
                };
            }
        }
        return std::unexpected{std::format(
            "no supported debugger found on PATH (tried: {})",
            detail::join_names(candidates))};
    }

    if (request == "lldb" || request == "gdb") {
        auto kind = *debug::classify_debugger_program(request);
        if (auto found = find_on_path(request)) {
            return debug::DebuggerProgram{
                .program = *found,
                .kind = kind,
            };
        }
        return std::unexpected{std::format("{} not found on PATH", request)};
    }

    auto kind = debug::classify_debugger_program(request);
    if (!kind) {
        return std::unexpected{std::format(
            "unsupported debugger '{}': expected auto, lldb, gdb, or a path/command "
            "whose name starts with lldb or gdb",
            request)};
    }

    if (detail::is_path_like(request)) {
        auto path = std::filesystem::path{std::string{request}};
        if (!std::filesystem::exists(path)) {
            return std::unexpected{
                std::format("debugger not found: {}", path.string())};
        }
        return debug::DebuggerProgram{
            .program = path.string(),
            .kind = *kind,
        };
    }

    if (auto found = find_on_path(request)) {
        return debug::DebuggerProgram{
            .program = *found,
            .kind = *kind,
        };
    }

    return std::unexpected{std::format("{} not found on PATH", request)};
}

inline auto resolve_debugger(std::string_view requested,
                             toolchain::Platform const& host = toolchain::detect_host_platform())
    -> std::expected<debug::DebuggerProgram, std::string> {
    return resolve_debugger(requested, host, detail::path_finder());
}

} // namespace debug::system
