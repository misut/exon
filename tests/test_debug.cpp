import std;
import commands;
import core;
import debug;
import debug.system;
import toolchain;

int failures = 0;

void check(bool cond, std::string_view msg) {
    if (!cond) {
        std::println(std::cerr, "  FAIL: {}", msg);
        ++failures;
    }
}

int run_cmd_debug(std::vector<std::string> const& args) {
    auto argv_storage = args;
    auto argv = std::vector<char*>{};
    for (auto& arg : argv_storage)
        argv.push_back(arg.data());
    return commands::cmd_debug(static_cast<int>(argv.size()), argv.data());
}

void test_lldb_launch_spec() {
    auto spec = debug::debugger_launch_spec(
        {.program = "/usr/bin/lldb", .kind = debug::DebuggerKind::lldb},
        "/tmp/app",
        {"--port", "8080"},
        "/tmp");
    check(spec.program == "/usr/bin/lldb", "lldb spec: program preserved");
    check(spec.cwd == std::filesystem::path{"/tmp"}, "lldb spec: cwd preserved");
    check(spec.args.size() == 4, "lldb spec: arg count");
    check(spec.args[0] == "--", "lldb spec: separator emitted");
    check(spec.args[1] == "/tmp/app", "lldb spec: executable emitted");
    check(spec.args[2] == "--port", "lldb spec: first program arg emitted");
    check(spec.args[3] == "8080", "lldb spec: second program arg emitted");
}

void test_gdb_launch_spec() {
    auto spec = debug::debugger_launch_spec(
        {.program = "/usr/bin/gdb", .kind = debug::DebuggerKind::gdb},
        "/tmp/app",
        {"input.txt"});
    check(spec.program == "/usr/bin/gdb", "gdb spec: program preserved");
    check(spec.args.size() == 3, "gdb spec: arg count");
    check(spec.args[0] == "--args", "gdb spec: --args emitted");
    check(spec.args[1] == "/tmp/app", "gdb spec: executable emitted");
    check(spec.args[2] == "input.txt", "gdb spec: program arg emitted");
}

void test_auto_debugger_prefers_lldb_on_macos() {
    auto probed = std::vector<std::string>{};
    auto resolved = debug::system::resolve_debugger(
        "auto",
        toolchain::make_platform("macos", "aarch64"),
        [&](std::string_view name) -> std::optional<std::string> {
            probed.push_back(std::string{name});
            if (name == "lldb")
                return "/usr/bin/lldb";
            return std::nullopt;
        });

    check(resolved.has_value(), "macOS auto resolve succeeds");
    check(probed.size() == 1 && probed[0] == "lldb", "macOS auto probes lldb first");
    check(resolved && resolved->kind == debug::DebuggerKind::lldb,
          "macOS auto resolves lldb");
}

void test_auto_debugger_prefers_gdb_on_linux() {
    auto probed = std::vector<std::string>{};
    auto resolved = debug::system::resolve_debugger(
        "auto",
        toolchain::make_platform("linux", "x86_64"),
        [&](std::string_view name) -> std::optional<std::string> {
            probed.push_back(std::string{name});
            if (name == "gdb")
                return "/usr/bin/gdb";
            return std::nullopt;
        });

    check(resolved.has_value(), "linux auto resolve succeeds");
    check(probed.size() == 1 && probed[0] == "gdb", "linux auto probes gdb first");
    check(resolved && resolved->kind == debug::DebuggerKind::gdb,
          "linux auto resolves gdb");
}

void test_explicit_debugger_not_found_reports_error() {
    auto resolved = debug::system::resolve_debugger(
        "gdb",
        toolchain::make_platform("macos", "aarch64"),
        [](std::string_view) -> std::optional<std::string> {
            return std::nullopt;
        });
    check(!resolved.has_value(), "missing explicit debugger reports failure");
    check(!resolved.has_value() && resolved.error().contains("gdb not found on PATH"),
          "missing explicit debugger mentions PATH");
}

void test_custom_debugger_name_must_look_like_lldb_or_gdb() {
    auto resolved = debug::system::resolve_debugger(
        "custom-debugger",
        toolchain::make_platform("macos", "aarch64"),
        [](std::string_view) -> std::optional<std::string> {
            return std::nullopt;
        });
    check(!resolved.has_value(), "unknown custom debugger rejected");
    check(!resolved.has_value() && resolved.error().contains("unsupported debugger"),
          "unknown custom debugger explains valid forms");
}

void test_cmd_debug_rejects_target_option() {
    auto rc = run_cmd_debug({"exon", "debug", "--target", "wasm32-wasi"});
    check(rc == 1, "cmd_debug rejects non-native target");
}

int main() {
    std::println("test_debug:");

    test_lldb_launch_spec();
    test_gdb_launch_spec();
    test_auto_debugger_prefers_lldb_on_macos();
    test_auto_debugger_prefers_gdb_on_linux();
    test_explicit_debugger_not_found_reports_error();
    test_custom_debugger_name_must_look_like_lldb_or_gdb();
    test_cmd_debug_rejects_target_option();

    if (failures > 0) {
        std::println("test_debug: {} FAILED", failures);
        return 1;
    }
    std::println("test_debug: all passed");
    return 0;
}
