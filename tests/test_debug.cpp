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

std::filesystem::path make_temp_debugger_path(std::string_view filename) {
    auto base = std::filesystem::temp_directory_path() / "exon-debug-tests";
    std::filesystem::create_directories(base);
    auto path = base / std::string{filename};
    auto out = std::ofstream{path};
    out << "stub";
    out.close();
    return path;
}

void cleanup_temp_debugger_path(std::filesystem::path const& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

void test_classifier_accepts_windows_debuggers() {
    check(debug::classify_debugger_program("devenv") == debug::DebuggerKind::devenv,
          "classifier: devenv");
    check(debug::classify_debugger_program("devenv.exe") == debug::DebuggerKind::devenv,
          "classifier: devenv.exe");
    check(debug::classify_debugger_program("devenv.com") == debug::DebuggerKind::devenv,
          "classifier: devenv.com");
    check(debug::classify_debugger_program("cdb") == debug::DebuggerKind::cdb,
          "classifier: cdb");
    check(debug::classify_debugger_program("cdb.exe") == debug::DebuggerKind::cdb,
          "classifier: cdb.exe");
}

void test_devenv_launch_spec() {
    auto spec = debug::debugger_launch_spec(
        {.program = "C:/VS/devenv.exe", .kind = debug::DebuggerKind::devenv},
        "C:/tmp/app.exe",
        {"--port", "8080"});
    check(spec.program == "C:/VS/devenv.exe", "devenv spec: program preserved");
    check(spec.args.size() == 4, "devenv spec: arg count");
    check(spec.args[0] == "/debugexe", "devenv spec: /debugexe emitted");
    check(spec.args[1] == "C:/tmp/app.exe", "devenv spec: executable emitted");
    check(spec.args[2] == "--port", "devenv spec: first program arg emitted");
    check(spec.args[3] == "8080", "devenv spec: second program arg emitted");
}

void test_cdb_launch_spec() {
    auto spec = debug::debugger_launch_spec(
        {.program = "C:/Debuggers/cdb.exe", .kind = debug::DebuggerKind::cdb},
        "C:/tmp/app.exe",
        {"input.txt"});
    check(spec.program == "C:/Debuggers/cdb.exe", "cdb spec: program preserved");
    check(spec.args.size() == 2, "cdb spec: arg count");
    check(spec.args[0] == "C:/tmp/app.exe", "cdb spec: executable emitted");
    check(spec.args[1] == "input.txt", "cdb spec: program arg emitted");
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

void test_auto_debugger_prefers_devenv_on_windows() {
    auto probed = std::vector<std::string>{};
    auto resolved = debug::system::resolve_debugger(
        "auto",
        toolchain::make_platform("windows", "x86_64"),
        [&](std::string_view name) -> std::optional<std::string> {
            probed.push_back(std::string{name});
            if (name == "devenv")
                return "C:/VS/devenv.com";
            return std::nullopt;
        });

    check(resolved.has_value(), "windows auto resolve succeeds");
    check(probed.size() == 1 && probed[0] == "devenv", "windows auto probes devenv first");
    check(resolved && resolved->kind == debug::DebuggerKind::devenv,
          "windows auto resolves devenv");
}

void test_auto_debugger_can_skip_devenv() {
    auto probed = std::vector<std::string>{};
    auto resolved = debug::system::resolve_debugger(
        "auto",
        toolchain::make_platform("windows", "x86_64"),
        [&](std::string_view name) -> std::optional<std::string> {
            probed.push_back(std::string{name});
            if (name == "cdb")
                return "C:/Debuggers/cdb.exe";
            return std::nullopt;
        },
        {debug::DebuggerKind::devenv});

    check(resolved.has_value(), "windows auto resolve with skip succeeds");
    check(probed.size() == 1 && probed[0] == "cdb", "windows auto skip probes cdb first");
    check(resolved && resolved->kind == debug::DebuggerKind::cdb,
          "windows auto skip resolves cdb");
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

void test_explicit_windows_debugger_rejected_on_non_windows() {
    auto devenv_resolved = debug::system::resolve_debugger(
        "devenv",
        toolchain::make_platform("linux", "x86_64"),
        [](std::string_view) -> std::optional<std::string> {
            return "/usr/bin/devenv";
        });
    check(!devenv_resolved.has_value(), "devenv rejected on non-Windows");
    check(!devenv_resolved.has_value() && devenv_resolved.error().contains("only supported on Windows"),
          "devenv non-Windows error explains host restriction");

    auto cdb_resolved = debug::system::resolve_debugger(
        "cdb",
        toolchain::make_platform("macos", "aarch64"),
        [](std::string_view) -> std::optional<std::string> {
            return "/usr/bin/cdb";
        });
    check(!cdb_resolved.has_value(), "cdb rejected on non-Windows");
    check(!cdb_resolved.has_value() && cdb_resolved.error().contains("only supported on Windows"),
          "cdb non-Windows error explains host restriction");
}

void test_path_like_windows_debugger_resolves() {
    auto devenv_path = make_temp_debugger_path("devenv.exe");
    auto devenv_resolved = debug::system::resolve_debugger(
        devenv_path.string(),
        toolchain::make_platform("windows", "x86_64"),
        [](std::string_view) -> std::optional<std::string> {
            return std::nullopt;
        });
    check(devenv_resolved.has_value(), "path-like devenv resolves");
    check(devenv_resolved && devenv_resolved->kind == debug::DebuggerKind::devenv,
          "path-like devenv kind preserved");
    cleanup_temp_debugger_path(devenv_path);

    auto cdb_path = make_temp_debugger_path("cdb.exe");
    auto cdb_resolved = debug::system::resolve_debugger(
        cdb_path.string(),
        toolchain::make_platform("windows", "x86_64"),
        [](std::string_view) -> std::optional<std::string> {
            return std::nullopt;
        });
    check(cdb_resolved.has_value(), "path-like cdb resolves");
    check(cdb_resolved && cdb_resolved->kind == debug::DebuggerKind::cdb,
          "path-like cdb kind preserved");
    cleanup_temp_debugger_path(cdb_path);
}

void test_windows_auto_error_mentions_install_hint() {
    auto resolved = debug::system::resolve_debugger(
        "auto",
        toolchain::make_platform("windows", "x86_64"),
        [](std::string_view) -> std::optional<std::string> {
            return std::nullopt;
        });
    check(!resolved.has_value(), "windows auto missing debugger reports failure");
    check(!resolved.has_value() && resolved.error().contains("install Visual Studio"),
          "windows auto missing debugger mentions install hint");
}

void test_devenv_program_args_guard() {
    auto args = std::vector<std::string>{"/flag", "value"};
    check(debug::has_devenv_unsafe_program_args(args),
          "devenv guard detects slash-prefixed args");
}

void test_cmd_debug_rejects_explicit_devenv_with_slash_args() {
    auto rc = run_cmd_debug({"exon", "debug", "--debugger", "devenv", "--", "/flag"});
    check(rc == 1, "cmd_debug rejects explicit devenv slash args");
}

void test_cmd_debug_rejects_target_option() {
    auto rc = run_cmd_debug({"exon", "debug", "--target", "wasm32-wasi"});
    check(rc == 1, "cmd_debug rejects non-native target");
}

int main() {
    std::println("test_debug:");

    test_classifier_accepts_windows_debuggers();
    test_devenv_launch_spec();
    test_cdb_launch_spec();
    test_lldb_launch_spec();
    test_gdb_launch_spec();
    test_auto_debugger_prefers_lldb_on_macos();
    test_auto_debugger_prefers_gdb_on_linux();
    test_auto_debugger_prefers_devenv_on_windows();
    test_auto_debugger_can_skip_devenv();
    test_explicit_debugger_not_found_reports_error();
    test_custom_debugger_name_must_look_like_lldb_or_gdb();
    test_explicit_windows_debugger_rejected_on_non_windows();
    test_path_like_windows_debugger_resolves();
    test_windows_auto_error_mentions_install_hint();
    test_devenv_program_args_guard();
    test_cmd_debug_rejects_explicit_devenv_with_slash_args();
    test_cmd_debug_rejects_target_option();

    if (failures > 0) {
        std::println("test_debug: {} FAILED", failures);
        return 1;
    }
    std::println("test_debug: all passed");
    return 0;
}
