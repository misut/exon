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

bool contains_kind(std::vector<debug::DebuggerKind> const& kinds,
                   debug::DebuggerKind kind) {
    return std::ranges::find(kinds, kind) != kinds.end();
}

std::string windows_auto_debugger_hint() {
    return "install Visual Studio for 'devenv' support or Debugging Tools for Windows "
           "for 'cdb', or pass --debugger <path>";
}

} // namespace detail

inline auto resolve_debugger(std::string_view requested,
                             toolchain::Platform const& host,
                             FindOnPath const& find_on_path,
                             std::vector<debug::DebuggerKind> const& skipped_auto_kinds = {})
    -> std::expected<debug::DebuggerProgram, std::string> {
    auto request = requested.empty() ? std::string_view{"auto"} : requested;
    if (request == "auto") {
        auto candidates = std::vector<debug::DebuggerKind>{};
        for (auto kind : debug::auto_debugger_candidates(host)) {
            if (!detail::contains_kind(skipped_auto_kinds, kind))
                candidates.push_back(kind);
        }
        for (auto kind : candidates) {
            auto name = debug::debugger_kind_name(kind);
            if (auto found = find_on_path(name)) {
                return debug::DebuggerProgram{
                    .program = *found,
                    .kind = kind,
                };
            }
        }
        auto message = std::format("no supported debugger found on PATH (tried: {})",
                                   detail::join_names(candidates));
        if (host.os == toolchain::OS::Windows)
            message += std::format("; {}", detail::windows_auto_debugger_hint());
        return std::unexpected{std::move(message)};
    }

    auto kind = debug::classify_debugger_program(request);
    if (!kind) {
        return std::unexpected{std::format(
            "unsupported debugger '{}': expected auto, lldb, gdb, devenv, cdb, or a "
            "path/command recognized as one of those debugger families",
            request)};
    }
    if (debug::debugger_is_windows_only(*kind) &&
        host.os != toolchain::OS::Windows) {
        return std::unexpected{std::format(
            "{} is only supported on Windows hosts", debug::debugger_kind_name(*kind))};
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
                             toolchain::Platform const& host,
                             std::vector<debug::DebuggerKind> const& skipped_auto_kinds)
    -> std::expected<debug::DebuggerProgram, std::string> {
    return resolve_debugger(requested, host, detail::path_finder(), skipped_auto_kinds);
}

inline auto resolve_debugger(std::string_view requested,
                             toolchain::Platform const& host = toolchain::detect_host_platform())
    -> std::expected<debug::DebuggerProgram, std::string> {
    return resolve_debugger(requested, host, detail::path_finder(), {});
}

} // namespace debug::system
