export module debug;
import std;
import core;
import toolchain;

export namespace debug {

enum class DebuggerKind {
    lldb,
    gdb,
};

inline auto debugger_kind_name(DebuggerKind kind) -> std::string_view {
    switch (kind) {
    case DebuggerKind::lldb:
        return "lldb";
    case DebuggerKind::gdb:
        return "gdb";
    }
    return "lldb";
}

struct DebuggerProgram {
    std::string program;
    DebuggerKind kind = DebuggerKind::lldb;
};

namespace detail {

std::string lowercase_ascii(std::string_view value) {
    auto text = std::string{value};
    std::ranges::transform(text, text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

} // namespace detail

inline auto classify_debugger_program(std::string_view program)
    -> std::optional<DebuggerKind> {
    auto name = detail::lowercase_ascii(
        std::filesystem::path{std::string{program}}.filename().string());
    if (name.starts_with("lldb"))
        return DebuggerKind::lldb;
    if (name.starts_with("gdb"))
        return DebuggerKind::gdb;
    return std::nullopt;
}

inline auto auto_debugger_candidates(toolchain::Platform const& host)
    -> std::vector<DebuggerKind> {
    switch (host.os) {
    case toolchain::OS::MacOS:
        return {DebuggerKind::lldb, DebuggerKind::gdb};
    case toolchain::OS::Linux:
        return {DebuggerKind::gdb, DebuggerKind::lldb};
    case toolchain::OS::Windows:
        return {DebuggerKind::lldb, DebuggerKind::gdb};
    default:
        return {DebuggerKind::lldb, DebuggerKind::gdb};
    }
}

inline auto debugger_launch_spec(DebuggerProgram const& debugger,
                                 std::filesystem::path const& executable,
                                 std::vector<std::string> const& program_args,
                                 std::filesystem::path const& cwd = {})
    -> core::ProcessSpec {
    auto spec = core::ProcessSpec{
        .program = debugger.program,
        .cwd = cwd,
    };
    if (debugger.kind == DebuggerKind::lldb)
        spec.args.push_back("--");
    else
        spec.args.push_back("--args");
    spec.args.push_back(executable.string());
    spec.args.insert(spec.args.end(), program_args.begin(), program_args.end());
    return spec;
}

} // namespace debug
