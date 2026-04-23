import std;
import build;
import build.system;
import commands;
import core;
import fetch;
import fetch.system;
import manifest;
import manifest.system;
import reporting;
import reporting.system;
import toolchain;

#if defined(_WIN32)
extern "C" int _putenv(char const* envstring);
#else
extern "C" int setenv(char const* name, char const* value, int overwrite);
extern "C" int unsetenv(char const* name);
#endif

int failures = 0;
std::filesystem::path self_executable_path;

void check(bool cond, std::string_view msg) {
    if (!cond) {
        std::println(std::cerr, "  FAIL: {}", msg);
        ++failures;
    }
}

struct TmpProject {
    std::filesystem::path root;

    TmpProject() {
        root = std::filesystem::temp_directory_path() / "exon_test_regressions";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "src");
        std::filesystem::create_directories(root / "tests");
    }

    ~TmpProject() { std::filesystem::remove_all(root); }

    void write(std::string const& rel_path, std::string const& content) {
        auto path = root / rel_path;
        std::filesystem::create_directories(path.parent_path());
        auto file = std::ofstream{path, std::ios::binary};
        file << content;
    }
};

std::optional<std::string> read_if_exists(std::filesystem::path const& path) {
    if (!std::filesystem::exists(path))
        return std::nullopt;
    auto file = std::ifstream{path};
    auto text = std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n'))
        text.pop_back();
    return text;
}

std::vector<std::string> read_lines(std::filesystem::path const& path) {
    auto file = std::ifstream{path};
    auto lines = std::vector<std::string>{};
    for (std::string line; std::getline(file, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (!line.empty())
            lines.push_back(std::move(line));
    }
    return lines;
}
void set_env_var(std::string_view name, std::string_view value) {
#if defined(_WIN32)
    auto assignment = std::format("{}={}", name, value);
    _putenv(assignment.c_str());
#else
    setenv(name.data(), std::string{value}.c_str(), 1);
#endif
}

void unset_env_var(std::string_view name) {
#if defined(_WIN32)
    auto assignment = std::format("{}=", name);
    _putenv(assignment.c_str());
#else
    unsetenv(name.data());
#endif
}

struct EnvVarGuard {
    std::string name;
    std::optional<std::string> original;

    explicit EnvVarGuard(std::string name_)
        : name(std::move(name_)) {
        if (auto value = std::getenv(name.c_str()); value)
            original = value;
    }

    ~EnvVarGuard() {
        if (original)
            set_env_var(name, *original);
        else
            unset_env_var(name);
    }
};

std::string portable_intron_config() {
    return R"([toolchain]
cmake = "4.3.1-test"
ninja = "1.13.2-test"

[toolchain.macos]
llvm = "22.1.2-macos-test"

[toolchain.linux]
llvm = "22.1.2-linux-test"

[toolchain.windows]
msvc = "2022-test"
)";
}

std::string legacy_windows_intron_config() {
    return R"([toolchain]
llvm = "22.1.2-legacy-test"
cmake = "4.3.1-test"
ninja = "1.13.2-test"
)";
}

std::string explicit_windows_llvm_config() {
    return R"([toolchain]
llvm = "22.1.2-legacy-test"
cmake = "4.3.1-test"
ninja = "1.13.2-test"

[toolchain.windows]
llvm = "22.1.2-windows-test"
)";
}

int maybe_run_fake_intron(int argc, char* argv[]) {
    auto exe_name = std::filesystem::path{argv[0]}.stem().string();
    if (exe_name != "intron")
        return -1;

    auto args = std::ostringstream{};
    for (int i = 1; i < argc; ++i) {
        if (i > 1)
            args << ' ';
        args << argv[i];
    }

    {
        auto file = std::ofstream{"intron-args.txt", std::ios::app};
        file << args.str() << '\n';
    }
    {
        auto file = std::ofstream{"intron-cwd.txt", std::ios::app};
        file << std::filesystem::current_path().string() << '\n';
    }
    return 0;
}

struct CwdGuard {
    std::filesystem::path previous;

    explicit CwdGuard(std::filesystem::path const& next)
        : previous(std::filesystem::current_path()) {
        std::filesystem::current_path(next);
    }

    ~CwdGuard() { std::filesystem::current_path(previous); }
};

int run_command(auto fn, std::vector<std::string> const& args) {
    auto argv = std::vector<char*>{};
    argv.reserve(args.size());
    for (auto const& arg : args)
        argv.push_back(const_cast<char*>(arg.c_str()));
    return fn(static_cast<int>(argv.size()), argv.data());
}

core::ProcessSpec shell_spec(std::filesystem::path const& cwd, std::string body,
                             std::optional<std::chrono::milliseconds> timeout = {}) {
#if defined(_WIN32)
    return {
        .program = "cmd",
        .args = {"/c", std::move(body)},
        .cwd = cwd,
        .timeout = timeout,
    };
#else
    return {
        .program = "sh",
        .args = {"-c", std::move(body)},
        .cwd = cwd,
        .timeout = timeout,
    };
#endif
}

build::BuildRequest make_mock_request(std::filesystem::path const& root,
                                      std::string_view name,
                                      bool configured = false) {
    std::filesystem::create_directories(root / ".exon" / "debug");
    build::BuildRequest request;
    request.project = {
        .root = root,
        .exon_dir = root / ".exon",
        .build_dir = root / ".exon" / "debug",
        .profile = "debug",
        .target = {},
        .is_wasm = false,
    };
    request.manifest.name = std::string{name};
    request.configured = configured;
    return request;
}

reporting::ProcessResult run_self_fixture(std::string_view fixture_name) {
    auto spec = core::ProcessSpec{
        .program = self_executable_path.string(),
        .args = {"--fixture", std::string{fixture_name}},
        .cwd = std::filesystem::current_path(),
    };
    return reporting::system::run_process(spec, reporting::StreamMode::capture);
}

int run_build_output_fixture(std::string_view scenario) {
    TmpProject proj;
    auto request = make_mock_request(proj.root, "app", scenario == "build-cached");
    auto plan = build::BuildPlan{
        .project = request.project,
        .configured = request.configured,
    };

    auto write_path = proj.root / ".exon" / "CMakeLists.txt";
    if (scenario == "build-cached")
        proj.write(".exon/CMakeLists.txt", "generated-build-files\n");
    plan.writes.push_back({
        .text = {
            .path = write_path,
            .content = "generated-build-files\n",
        },
    });

#if defined(_WIN32)
    auto configure_ok = shell_spec(proj.root,
                                   "(echo configure-noisy-stdout & echo configure-noisy-stderr 1>&2 & exit 0)");
    auto build_ok = shell_spec(proj.root,
                               "(echo build-noisy-stdout & echo build-noisy-stderr 1>&2 & exit 0)");
    auto build_fail = shell_spec(
        proj.root,
        "(echo build-error-prefix & echo build-error-tail 1>&2 & exit 9)");
#else
    auto configure_ok = shell_spec(
        proj.root,
        "printf 'configure-noisy-stdout\\n'; printf 'configure-noisy-stderr\\n' 1>&2");
    auto build_ok = shell_spec(
        proj.root,
        "printf 'build-noisy-stdout\\n'; printf 'build-noisy-stderr\\n' 1>&2");
    auto build_fail = shell_spec(
        proj.root,
        "printf 'build-error-prefix\\n'; printf 'build-error-tail\\n' 1>&2; exit 9");
#endif

    if (scenario != "build-cached") {
        plan.configure_steps.push_back({
            .spec = std::move(configure_ok),
            .label = "configuring...",
        });
    }

    plan.build_steps.push_back({
        .spec = std::move(build_ok),
        .label = "building...",
    });
    if (scenario == "build-failure")
        plan.build_steps.back().spec = std::move(build_fail);

    return build::system::detail::run_build_human(
        request, plan, "build", std::chrono::steady_clock::now());
}

int run_test_output_fixture(reporting::ShowOutput show_output) {
    TmpProject proj;
    proj.write(".exon/CMakeLists.txt", "generated-build-files\n");
    auto request = make_mock_request(proj.root, "app", true);
    auto plan = build::BuildPlan{
        .project = request.project,
        .configured = true,
    };
    plan.writes.push_back({
        .text = {
            .path = proj.root / ".exon" / "CMakeLists.txt",
            .content = "generated-build-files\n",
        },
    });

#if defined(_WIN32)
    auto build_ok = shell_spec(proj.root, "(echo hidden-build-output & exit 0)");
    auto pass = shell_spec(proj.root, "(echo pass-output & exit 0)");
    auto fail = shell_spec(proj.root, "(echo fail-stdout & echo fail-stderr 1>&2 & exit 3)");
    auto timeout = shell_spec(proj.root, "ping -n 6 127.0.0.1 > nul",
                              std::chrono::milliseconds{50});
#else
    auto build_ok = shell_spec(proj.root, "printf 'hidden-build-output\\n'");
    auto pass = shell_spec(proj.root, "printf 'pass-output\\n'");
    auto fail = shell_spec(proj.root,
                           "printf 'fail-stdout\\n'; printf 'fail-stderr\\n' 1>&2; exit 3");
    auto timeout = shell_spec(proj.root, "sleep 1", std::chrono::milliseconds{50});
#endif

    plan.build_steps.push_back({
        .spec = std::move(build_ok),
        .label = "building tests...",
    });
    plan.run_steps.push_back({
        .spec = std::move(pass),
        .label = "test-pass",
    });
    plan.run_steps.push_back({
        .spec = std::move(fail),
        .label = "test-fail",
    });
    plan.run_steps.push_back({
        .spec = std::move(timeout),
        .label = "test-timeout",
    });

    build::system::TestRunSummary summary;
    return build::system::detail::run_test_human(
        request, plan, reporting::Options{.output = reporting::OutputMode::human,
                                          .show_output = show_output},
        std::chrono::steady_clock::now(), summary);
}

int run_wrapped_build_output_fixture() {
    TmpProject proj;
    auto request = make_mock_request(proj.root, "app", true);
    auto plan = build::BuildPlan{
        .project = request.project,
        .configured = true,
    };
    plan.writes.push_back({
        .text = {
            .path = proj.root / ".exon" / "CMakeLists.txt",
            .content = "generated-build-files\n",
        },
    });
#if defined(_WIN32)
    auto build_step = shell_spec(proj.root, "(echo wrapped-build-output & echo %NINJA_STATUS% & exit 0)");
#else
    auto build_step = shell_spec(proj.root, "printf 'wrapped-build-output\\n'; printf '%s\\n' \"$NINJA_STATUS\"");
#endif
    plan.build_steps.push_back({
        .spec = std::move(build_step),
        .label = "building...",
    });
    return build::system::detail::run_build_wrapped(
        request, plan, "build", std::chrono::steady_clock::now());
}

int run_wrapped_test_output_fixture() {
    TmpProject proj;
    proj.write(".exon/CMakeLists.txt", "generated-build-files\n");
    auto request = make_mock_request(proj.root, "app", true);
    auto plan = build::BuildPlan{
        .project = request.project,
        .configured = true,
    };
    plan.writes.push_back({
        .text = {
            .path = proj.root / ".exon" / "CMakeLists.txt",
            .content = "generated-build-files\n",
        },
    });
#if defined(_WIN32)
    auto pass = shell_spec(proj.root, "(echo wrapped-pass-output & exit 0)");
    auto fail = shell_spec(proj.root, "(echo wrapped-fail-output & exit 4)");
#else
    auto pass = shell_spec(proj.root, "printf 'wrapped-pass-output\\n'");
    auto fail = shell_spec(proj.root, "printf 'wrapped-fail-output\\n'; exit 4");
#endif
    plan.run_steps.push_back({.spec = std::move(pass), .label = "wrapped-pass"});
    plan.run_steps.push_back({.spec = std::move(fail), .label = "wrapped-fail"});
    build::system::TestRunSummary summary;
    return build::system::detail::run_test_wrapped(
        request, plan, reporting::Options{.output = reporting::OutputMode::wrapped,
                                          .show_output = reporting::ShowOutput::failed},
        std::chrono::steady_clock::now(), summary);
}

void write_workspace_member(TmpProject& proj, std::string_view dir, std::string_view name,
                            std::vector<std::pair<std::string, std::string>> const& tests) {
    proj.write(std::format("{}/exon.toml", dir), std::format(R"(
[package]
name = "{}"
version = "0.1.0"
type = "bin"
standard = 23
)", name));
    proj.write(std::format("{}/src/main.cpp", dir), std::format(R"(import std;

int main() {{
    std::println("{} main");
    return 0;
}}
)", name));
    for (auto const& [file_name, content] : tests)
        proj.write(std::format("{}/tests/{}", dir, file_name), content);
}

void write_workspace_fixture(TmpProject& proj) {
    proj.write("exon.toml", R"(
[workspace]
members = ["app", "tool"]
)");
    write_workspace_member(
        proj, "app", "app",
        {
            {"first.cpp", R"(import std;

int main() {
    std::println("workspace app first");
    return 0;
}
)"},
            {"second.cpp", R"(import std;

int main() {
    std::println("workspace app second");
    return 0;
}
)"},
        });
    write_workspace_member(
        proj, "tool", "tool",
        {
            {"tool.cpp", R"(import std;

int main() {
    std::println("workspace tool");
    return 0;
}
)"},
        });
}

void write_workspace_failfast_fixture(TmpProject& proj) {
    proj.write("exon.toml", R"(
[workspace]
members = ["app", "broken", "after"]
)");
    write_workspace_member(
        proj, "app", "app",
        {
            {"first.cpp", R"(import std;

int main() {
    std::println("workspace app first");
    return 0;
}
)"},
            {"second.cpp", R"(import std;

int main() {
    std::println("workspace app second");
    return 0;
}
)"},
        });
    write_workspace_member(
        proj, "broken", "broken",
        {
            {"broken.cpp", R"(import std;

int main() {
    std::println("workspace broken");
    return 1;
}
)"},
        });
    write_workspace_member(
        proj, "after", "after",
        {
            {"after.cpp", R"(import std;

int main() {
    std::println("workspace after");
    return 0;
}
)"},
        });
}

int run_workspace_build_fixture() {
    TmpProject proj;
    write_workspace_fixture(proj);
    auto guard = CwdGuard{proj.root};
    return run_command(commands::cmd_build, {"exon", "build", "--output", "human"});
}

int run_workspace_test_fixture() {
    TmpProject proj;
    write_workspace_fixture(proj);
    auto guard = CwdGuard{proj.root};
    return run_command(commands::cmd_test, {"exon", "test", "--output", "human"});
}

int run_workspace_test_failfast_fixture() {
    TmpProject proj;
    write_workspace_failfast_fixture(proj);
    auto guard = CwdGuard{proj.root};
    return run_command(commands::cmd_test, {"exon", "test", "--output", "human"});
}

int maybe_run_output_fixture(int argc, char* argv[]) {
    if (argc < 3 || std::string_view{argv[1]} != "--fixture")
        return -1;

    auto fixture = std::string_view{argv[2]};
    if (fixture == "build-success")
        return run_build_output_fixture(fixture);
    if (fixture == "build-cached")
        return run_build_output_fixture(fixture);
    if (fixture == "build-failure")
        return run_build_output_fixture(fixture);
    if (fixture == "test-show-failed")
        return run_test_output_fixture(reporting::ShowOutput::failed);
    if (fixture == "test-show-all")
        return run_test_output_fixture(reporting::ShowOutput::all);
    if (fixture == "test-show-none")
        return run_test_output_fixture(reporting::ShowOutput::none);
    if (fixture == "wrapped-build")
        return run_wrapped_build_output_fixture();
    if (fixture == "wrapped-test")
        return run_wrapped_test_output_fixture();
    if (fixture == "workspace-build")
        return run_workspace_build_fixture();
    if (fixture == "workspace-test")
        return run_workspace_test_fixture();
    if (fixture == "workspace-test-failfast")
        return run_workspace_test_failfast_fixture();
    return 2;
}

void write_fake_intron(TmpProject& proj) {
    auto bin_dir = proj.root / "bin";
    std::filesystem::create_directories(bin_dir);

#if defined(_WIN32)
    auto fake_intron = bin_dir / "intron.exe";
#else
    auto fake_intron = bin_dir / "intron";
#endif

    std::filesystem::copy_file(self_executable_path, fake_intron,
                               std::filesystem::copy_options::overwrite_existing);

#if !defined(_WIN32)
    std::filesystem::permissions(
        fake_intron,
        std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write,
        std::filesystem::perm_options::add);
#endif
}

void prepend_fake_intron_to_path(TmpProject& proj) {
    auto fake_path = (proj.root / "bin").string();
    if (auto* current = std::getenv("PATH"); current && *current) {
#if defined(_WIN32)
        fake_path += ";";
#else
        fake_path += ":";
#endif
        fake_path += current;
    }
    set_env_var("PATH", fake_path);
}

void isolate_intron_home(TmpProject& proj) {
    set_env_var("USERPROFILE", proj.root.string());
    set_env_var("HOME", proj.root.string());
}

void check_same_cwd(std::vector<std::string> const& actual, std::filesystem::path const& expected,
                    std::string_view message) {
    auto expected_cwd = std::filesystem::weakly_canonical(expected);
    check(!actual.empty(), message);
    for (auto const& line : actual) {
        auto actual_cwd = std::filesystem::weakly_canonical(std::filesystem::path{line});
        check(actual_cwd == expected_cwd, message);
    }
}

void check_contains(std::vector<std::string> const& values, std::string_view needle,
                    std::string_view message) {
    check(std::ranges::find(values, std::string{needle}) != values.end(), message);
}

void check_not_contains(std::vector<std::string> const& values, std::string_view needle,
                        std::string_view message) {
    check(std::ranges::find(values, std::string{needle}) == values.end(), message);
}

void test_portable_intron_config_installs_host_tools_from_project_root() {
    TmpProject proj;
    proj.write(".intron.toml", portable_intron_config());
    write_fake_intron(proj);

    EnvVarGuard path_guard{"PATH"};
    EnvVarGuard home_guard{"USERPROFILE"};
    EnvVarGuard home_env_guard{"HOME"};
    prepend_fake_intron_to_path(proj);
    isolate_intron_home(proj);

    build::system::detail::ensure_intron_tools(proj.root);

    auto args = read_lines(proj.root / "intron-args.txt");
    auto cwd = read_lines(proj.root / "intron-cwd.txt");
    check_same_cwd(cwd, proj.root,
                   "portable intron config: every intron install runs in project root");
    check_contains(args, "install cmake 4.3.1-test",
                   "portable intron config: installs shared cmake");
    check_contains(args, "install ninja 1.13.2-test",
                   "portable intron config: installs shared ninja");
#if defined(_WIN32)
    check_contains(args, "install msvc 2022-test",
                   "portable intron config: installs windows msvc");
    check_not_contains(args, "install llvm 22.1.2-linux-test",
                       "portable intron config: skips linux llvm on windows");
    check_not_contains(args, "install llvm 22.1.2-macos-test",
                       "portable intron config: skips macos llvm on windows");
    check(args.size() == 3, "portable intron config: installs exactly three windows tools");
#elif defined(__APPLE__)
    check_contains(args, "install llvm 22.1.2-macos-test",
                   "portable intron config: installs macos llvm");
    check(args.size() == 3, "portable intron config: installs exactly three macos tools");
#elif defined(__linux__)
    check_contains(args, "install llvm 22.1.2-linux-test",
                   "portable intron config: installs linux llvm");
    check(args.size() == 3, "portable intron config: installs exactly three linux tools");
#endif
}

void test_portable_intron_config_skips_when_intron_missing() {
    TmpProject proj;
    proj.write(".intron.toml", portable_intron_config());
    std::filesystem::create_directories(proj.root / "empty-bin");

    EnvVarGuard path_guard{"PATH"};
    set_env_var("PATH", (proj.root / "empty-bin").string());

    build::system::detail::ensure_intron_tools(proj.root);

    check(!std::filesystem::exists(proj.root / "intron-args.txt"),
          "portable intron config: missing intron skips install");
    check(!std::filesystem::exists(proj.root / "intron-cwd.txt"),
          "portable intron config: missing intron leaves no marker");
}

#if defined(_WIN32)
void test_windows_legacy_common_llvm_is_skipped() {
    TmpProject proj;
    proj.write(".intron.toml", legacy_windows_intron_config());
    write_fake_intron(proj);

    EnvVarGuard path_guard{"PATH"};
    EnvVarGuard home_guard{"USERPROFILE"};
    EnvVarGuard home_env_guard{"HOME"};
    prepend_fake_intron_to_path(proj);
    isolate_intron_home(proj);

    build::system::detail::ensure_intron_tools(proj.root);

    auto args = read_lines(proj.root / "intron-args.txt");
    check_contains(args, "install cmake 4.3.1-test",
                   "windows legacy config: installs cmake");
    check_contains(args, "install ninja 1.13.2-test",
                   "windows legacy config: installs ninja");
    check_not_contains(args, "install llvm 22.1.2-legacy-test",
                       "windows legacy config: skips common llvm");
    check(args.size() == 2, "windows legacy config: installs exactly non-llvm tools");
}

void test_windows_explicit_llvm_override_is_installed() {
    TmpProject proj;
    proj.write(".intron.toml", explicit_windows_llvm_config());
    write_fake_intron(proj);

    EnvVarGuard path_guard{"PATH"};
    EnvVarGuard home_guard{"USERPROFILE"};
    EnvVarGuard home_env_guard{"HOME"};
    prepend_fake_intron_to_path(proj);
    isolate_intron_home(proj);

    build::system::detail::ensure_intron_tools(proj.root);

    auto args = read_lines(proj.root / "intron-args.txt");
    check_contains(args, "install cmake 4.3.1-test",
                   "windows override config: installs cmake");
    check_contains(args, "install ninja 1.13.2-test",
                   "windows override config: installs ninja");
    check_contains(args, "install llvm 22.1.2-windows-test",
                   "windows override config: installs explicit windows llvm");
    check_not_contains(args, "install llvm 22.1.2-legacy-test",
                       "windows override config: skips legacy common llvm");
    check(args.size() == 3, "windows override config: installs explicit host tools");
}
#endif
struct TmpGitRepo {
    std::filesystem::path root;

    TmpGitRepo(std::filesystem::path const& parent_dir, std::string const& name) {
        root = parent_dir / name;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "src");
    }

    ~TmpGitRepo() { std::filesystem::remove_all(root); }

    void write(std::string const& rel_path, std::string const& content) {
        auto path = root / rel_path;
        std::filesystem::create_directories(path.parent_path());
        auto file = std::ofstream{path};
        file << content;
    }

    int git(std::string const& cmd) const {
#if defined(_WIN32)
        auto full = std::format("git -C \"{}\" {} >NUL 2>&1", root.generic_string(), cmd);
#else
        auto full = std::format("git -C \"{}\" {} >/dev/null 2>&1", root.generic_string(), cmd);
#endif
        return std::system(full.c_str());
    }

    void commit_and_tag(std::string const& tag) {
        git("add .");
        git(std::format("commit -q -m {}", tag));
        git(std::format("tag {}", tag));
    }

    void init_and_tag(std::string const& tag) {
        git("init -q");
        git("config user.email test@example.com");
        git("config user.name Test");
        commit_and_tag(tag);
    }

    std::string key() const { return root.generic_string(); }
};

toolchain::Toolchain make_tc() {
    toolchain::Toolchain tc;
    tc.cmake = "cmake";
    tc.ninja = "ninja";
    tc.cxx_compiler = "/usr/bin/clang++";
    tc.stdlib_modules_json = "/fake/libc++.modules.json";
    return tc;
}

void test_prepare_request_keeps_portable_root_sync_inputs_unresolved() {
    TmpProject proj;
    proj.write("src/main.cpp", "int main() { return 0; }\n");
    proj.write("deps/winlib/exon.toml", R"([package]
name = "winlib"
version = "0.1.0"
type = "lib"
standard = 23
)");
    proj.write("deps/winlib/src/winlib.cppm", "export module winlib;\n");
    proj.write("deps/linuxlib/exon.toml", R"([package]
name = "linuxlib"
version = "0.1.0"
type = "lib"
standard = 23
)");
    proj.write("deps/linuxlib/src/linuxlib.cppm", "export module linuxlib;\n");
    proj.write("exon.toml", R"([package]
name = "app"
version = "0.1.0"
type = "bin"
standard = 23

[sync]
cmake-in-root = true

[target.'cfg(os = "windows")'.dependencies.path]
winlib = "deps/winlib"

[target.'cfg(os = "linux")'.dependencies.path]
linuxlib = "deps/linuxlib"

[target.'cfg(os = "windows")'.build]
cxxflags = ["/fsanitize=address"]

[target.'cfg(os = "linux")'.build]
cxxflags = ["-fsanitize=address,undefined"]
)");

    auto raw_m = manifest::system::load((proj.root / "exon.toml").string());
    auto resolved_m = manifest::resolve_for_platform(
        raw_m, toolchain::make_platform("windows", "x86_64"));

    auto request = build::system::detail::prepare_request(
        proj.root, resolved_m, raw_m, false, {}, false, {}, {});

    check(request.manifest.target_sections.empty(),
          "prepare_request: host manifest is flattened");
    check(request.manifest.path_deps.contains("winlib"),
          "prepare_request: host manifest keeps windows path dependency");
    check(!request.manifest.path_deps.contains("linuxlib"),
          "prepare_request: host manifest skips linux path dependency");
    check(request.manifest.build.cxxflags == std::vector<std::string>{"/fsanitize=address"},
          "prepare_request: host manifest keeps resolved windows build flags");

    check(request.portable_manifest.has_value(),
          "prepare_request: portable manifest is preserved for root sync");
    check(request.portable_manifest && request.portable_manifest->target_sections.size() == 2,
          "prepare_request: portable manifest keeps target sections");
    check(request.portable_manifest && request.portable_manifest->path_deps.empty(),
          "prepare_request: portable manifest remains unflattened");

    auto portable_dep_names = std::vector<std::string>{};
    for (auto const& dep : request.portable_deps)
        portable_dep_names.push_back(dep.name);
    check_contains(portable_dep_names, "winlib",
                   "prepare_request: portable deps include windows dependency");
    check_contains(portable_dep_names, "linuxlib",
                   "prepare_request: portable deps include linux dependency");
    check(request.portable_deps.size() == 2,
          "prepare_request: portable deps mirror all target sections");
}

#if !defined(_WIN32)
void test_portable_windows_asan_helper_configures_on_non_windows() {
    TmpProject proj;
    proj.write("src/main.cpp", "int main() { return 0; }\n");
    proj.write("tests/test_app.cpp", "int main() { return 0; }\n");

    manifest::Manifest m;
    m.name = "app";
    m.version = "0.1.0";
    m.standard = 20;
    m.type = "bin";
    m.build.cxxflags = {"/fsanitize=address"};

    try {
        auto cmake = build::generate_portable_cmake(m, proj.root, {});
        check(cmake.contains("function(exon_copy_windows_asan_runtime target)"),
              "portable CMake emits Windows ASan helper");
        check(cmake.contains("if(NOT WIN32)"),
              "portable CMake helper short-circuits on non-Windows");

        {
            auto file = std::ofstream{proj.root / "CMakeLists.txt"};
            file << cmake;
        }

        auto build_dir = proj.root / "build";
        core::ProcessSpec spec{
            .program = "cmake",
            .args = {"-S", proj.root.string(), "-B", build_dir.string()},
            .cwd = proj.root,
        };
        auto rc = build::system::run_process(spec);
        check(rc == 0, "portable Windows ASan helper configures on non-Windows");
    } catch (...) {
        throw;
    }
}
#endif

void test_run_process_returns_child_exit_code() {
#if defined(_WIN32)
    core::ProcessSpec spec{
        .program = "cmd",
        .args = {"/c", "exit 7"},
    };
#else
    core::ProcessSpec spec{
        .program = "sh",
        .args = {"-c", "exit 7"},
    };
#endif
    auto rc = build::system::run_process(spec);
    check(rc == 7, "run_process returns child exit code");
}

void test_system_run_process_honors_cwd() {
    TmpProject proj;
    proj.write("marker.txt", "ok\n");

#if defined(_WIN32)
    core::ProcessSpec spec{
        .program = "cmd",
        .args = {"/c", "if exist marker.txt (exit 0) else (exit 1)"},
        .cwd = proj.root,
    };
#else
    core::ProcessSpec spec{
        .program = "sh",
        .args = {"-c", "test -f marker.txt"},
        .cwd = proj.root,
    };
#endif

    auto rc = build::system::run_process(spec);
    check(rc == 0, "build.system::run_process honors ProcessSpec.cwd");
}

void test_reporting_capture_collects_stdout_and_stderr() {
#if defined(_WIN32)
    core::ProcessSpec spec{
        .program = "cmd",
        .args = {"/c", "(echo out & echo err 1>&2 & exit 0)"},
    };
#else
    core::ProcessSpec spec{
        .program = "sh",
        .args = {"-c", "printf 'out\\n'; printf 'err\\n' 1>&2"},
    };
#endif

    auto result = reporting::system::run_process(spec, reporting::StreamMode::capture);
    check(result.exit_code == 0, "capture: exit code");
    check(!result.timed_out, "capture: not timed out");
    check(result.stdout_text.contains("out"), "capture: stdout captured");
    check(result.stderr_text.contains("err"), "capture: stderr captured");
}

void test_reporting_capture_observer_receives_chunks() {
#if defined(_WIN32)
    core::ProcessSpec spec{
        .program = "cmd",
        .args = {"/c", "(echo observer-out & echo observer-err 1>&2 & exit 0)"},
    };
#else
    core::ProcessSpec spec{
        .program = "sh",
        .args = {"-c", "printf 'observer-out\\n'; printf 'observer-err\\n' 1>&2"},
    };
#endif

    auto observed_stdout = std::string{};
    auto observed_stderr = std::string{};
    auto result = reporting::system::run_process(
        spec, reporting::StreamMode::capture,
        [&](std::string_view chunk, bool stderr_stream) {
            if (stderr_stream)
                observed_stderr.append(chunk);
            else
                observed_stdout.append(chunk);
        });

    check(result.exit_code == 0, "observer: exit code");
    check(observed_stdout.contains("observer-out"), "observer: stdout chunk seen");
    check(observed_stderr.contains("observer-err"), "observer: stderr chunk seen");
    check(result.stdout_text == observed_stdout, "observer: stdout matches capture");
    check(result.stderr_text == observed_stderr, "observer: stderr matches capture");
}

void test_reporting_tee_collects_output() {
#if defined(_WIN32)
    core::ProcessSpec spec{
        .program = "cmd",
        .args = {"/c", "(echo tee-out & echo tee-err 1>&2 & exit 0)"},
    };
#else
    core::ProcessSpec spec{
        .program = "sh",
        .args = {"-c", "printf 'tee-out\\n'; printf 'tee-err\\n' 1>&2"},
    };
#endif

    auto result = reporting::system::run_process(spec, reporting::StreamMode::tee);
    check(result.exit_code == 0, "tee: exit code");
    check(result.stdout_text.contains("tee-out"), "tee: stdout captured");
    check(result.stderr_text.contains("tee-err"), "tee: stderr captured");
}

void test_reporting_timeout_marks_result() {
#if defined(_WIN32)
    core::ProcessSpec spec{
        .program = "cmd",
        .args = {"/c", "ping -n 6 127.0.0.1 > nul"},
        .timeout = std::chrono::milliseconds{50},
    };
#else
    core::ProcessSpec spec{
        .program = "sh",
        .args = {"-c", "sleep 1"},
        .timeout = std::chrono::milliseconds{50},
    };
#endif

    auto result = reporting::system::run_process(spec, reporting::StreamMode::capture);
    check(result.timed_out, "timeout: result flagged");
    check(result.exit_code == 124, "timeout: uses 124 exit code");
}

void test_ninja_progress_helpers() {
    auto progress = build::system::detail::parse_ninja_progress_prefix(
        "[12/56 21%] Building CXX object foo.o");
    check(progress.has_value(), "progress parser: prefix parsed");
    check(progress && progress->finished == 12, "progress parser: finished count");
    check(progress && progress->total == 56, "progress parser: total count");
    check(progress && progress->percent == 21, "progress parser: percent");
    check(!build::system::detail::parse_ninja_progress_prefix("Building CXX object foo.o"),
          "progress parser: non-progress line ignored");

    auto stripped = build::system::detail::strip_ninja_progress_prefix(
        "[12/56 21%] Building CXX object foo.o");
    check(stripped == "Building CXX object foo.o", "progress strip: prefix removed");

    auto sanitized = build::system::detail::sanitize_ninja_progress_output(
        "[1/4 25%] Building CXX object foo.o\n"
        "[2/4 50%] \n"
        "plain stdout\n");
    check(!sanitized.contains("[1/4 25%]"), "progress sanitize: progress prefix removed");
    check(sanitized.contains("Building CXX object foo.o"),
          "progress sanitize: line body preserved");
    check(sanitized.contains("plain stdout"),
          "progress sanitize: non-progress output preserved");
}

void test_live_progress_frame_format() {
    auto frame0 = reporting::system::format_progress_frame({
        .done = 12,
        .total = 56,
        .percent = 21,
    }, 0);
    auto frame1 = reporting::system::format_progress_frame({
        .done = 12,
        .total = 56,
        .percent = 21,
    }, 1);
    check(frame0 == "  [|] [12/56 21%]", "progress frame: first spinner frame");
    check(frame1 == "  [/] [12/56 21%]", "progress frame: second spinner frame");
}

void test_human_build_success_output() {
    auto result = run_self_fixture("build-success");
    check(result.exit_code == 0, "human build success: exit code");
    check(result.stdout_text.contains("==> sync"), "human build success: sync stage");
    check(result.stdout_text.contains("generated build files"),
          "human build success: sync summary");
    check(result.stdout_text.contains("==> configure"), "human build success: configure stage");
    check(!result.stdout_text.contains("configure-noisy-stdout"),
          "human build success: hides configure chatter");
    check(result.stdout_text.contains("==> build"), "human build success: build stage");
    check(!result.stdout_text.contains("build-noisy-stdout"),
          "human build success: hides build chatter");
    check(result.stdout_text.contains("==> finish"), "human build success: finish stage");
    check(result.stdout_text.contains("artifact"), "human build success: artifact reported");
    check(result.stdout_text.contains(".exon/debug/app"),
          "human build success: artifact path reported");
    check(result.stdout_text.contains("elapsed"), "human build success: elapsed reported");
}

void test_human_build_cached_output() {
    auto result = run_self_fixture("build-cached");
    check(result.exit_code == 0, "human build cached: exit code");
    check(result.stdout_text.contains("build files up to date"),
          "human build cached: sync says up to date");
    check(result.stdout_text.contains("==> configure"), "human build cached: configure stage");
    check(result.stdout_text.contains("cached"), "human build cached: cached configure");
    check(!result.stdout_text.contains("configure-noisy-stdout"),
          "human build cached: configure command skipped");
}

void test_human_build_failure_output() {
    auto result = run_self_fixture("build-failure");
    check(result.exit_code == 9, "human build failure: exit code");
    check(result.stdout_text.contains("exon: build failed"),
          "human build failure: summary printed");
    check(result.stdout_text.contains("phase     build"),
          "human build failure: build phase reported");
    check(result.stdout_text.contains("Captured output excerpt:"),
          "human build failure: excerpt heading");
    check(result.stdout_text.contains("stderr excerpt"),
          "human build failure: stderr excerpt printed");
    check(result.stdout_text.contains("build-error-tail"),
          "human build failure: failure output captured");
    check(result.stdout_text.contains("hint: rerun with --output wrapped"),
          "human build failure: wrapped rerun hint");
}

void test_human_test_failed_output_mode() {
    auto result = run_self_fixture("test-show-failed");
    check(result.exit_code == 1, "human test failed mode: exit code");
    check(result.stdout_text.contains("collected 3 test binaries"),
          "human test failed mode: collected count");
    check(result.stdout_text.contains("PASSED  test-pass"),
          "human test failed mode: pass status shown");
    check(result.stdout_text.contains("FAILED  test-fail"),
          "human test failed mode: fail status shown");
    check(result.stdout_text.contains("TIMEOUT test-timeout"),
          "human test failed mode: timeout status shown");
    check(result.stdout_text.contains("Failures:"),
          "human test failed mode: failures section shown");
    check(!result.stdout_text.contains("pass-output"),
          "human test failed mode: passing output hidden");
    check(result.stdout_text.contains("fail-stdout"),
          "human test failed mode: failing stdout shown");
    check(result.stdout_text.contains("fail-stderr"),
          "human test failed mode: failing stderr shown");
    check(result.stdout_text.contains("failed    1"),
          "human test failed mode: failed summary count");
    check(result.stdout_text.contains("timed out 1"),
          "human test failed mode: timeout summary count");
}

void test_human_test_all_output_mode() {
    auto result = run_self_fixture("test-show-all");
    check(result.exit_code == 1, "human test all mode: exit code");
    check(result.stdout_text.contains("Output:"), "human test all mode: output section shown");
    check(result.stdout_text.contains("pass-output"),
          "human test all mode: passing output shown");
    check(result.stdout_text.contains("fail-stdout"),
          "human test all mode: failing output shown");
}

void test_human_test_none_output_mode() {
    auto result = run_self_fixture("test-show-none");
    check(result.exit_code == 1, "human test none mode: exit code");
    check(!result.stdout_text.contains("Failures:"),
          "human test none mode: failures section hidden");
    check(!result.stdout_text.contains("Output:"),
          "human test none mode: output section hidden");
    check(!result.stdout_text.contains("pass-output"),
          "human test none mode: passing output hidden");
    check(!result.stdout_text.contains("fail-stdout"),
          "human test none mode: failing output hidden");
    check(result.stdout_text.contains("collected 3"),
          "human test none mode: summary preserved");
}

void test_wrapped_output_regressions() {
    auto build_result = run_self_fixture("wrapped-build");
    check(build_result.exit_code == 0, "wrapped build: exit code");
    check(build_result.stdout_text.contains("wrapped-build-output"),
          "wrapped build: command output preserved");
    check(build_result.stdout_text.contains("[%f/%t]"),
          "wrapped build: ninja status propagated");

    auto test_result = run_self_fixture("wrapped-test");
    check(test_result.exit_code == 1, "wrapped test: exit code");
    check(test_result.stdout_text.contains("wrapped-fail-output"),
          "wrapped test: failed output preserved");
    check(!test_result.stdout_text.contains("wrapped-pass-output"),
          "wrapped test: passing output stays hidden");
}

void test_workspace_human_output() {
    auto build_result = run_self_fixture("workspace-build");
    check(build_result.exit_code == 0, "workspace build human: exit code");
    check(build_result.stdout_text.contains("==> resolve"),
          "workspace build human: resolve stage");
    check(build_result.stdout_text.contains("==> sync"),
          "workspace build human: sync stage");
    check(build_result.stdout_text.contains("==> finish"),
          "workspace build human: finish stage");
    check(build_result.stdout_text.contains("build dir"),
          "workspace build human: build dir reported");

    auto test_result = run_self_fixture("workspace-test");
    check(test_result.exit_code == 0, "workspace test human: exit code");
    check(test_result.stdout_text.contains("==> member app (app)"),
          "workspace test human: member header");
    check(test_result.stdout_text.contains("==> member tool (tool)"),
          "workspace test human: second member header");
    check(test_result.stdout_text.contains("members   2 run, 2 passed, 0 failed"),
          "workspace test human: member aggregate summary");
    check(test_result.stdout_text.contains("binaries  3 run"),
          "workspace test human: binary aggregate summary");
    check(test_result.stdout_text.contains("collected 2 test binaries"),
          "workspace test human: per-member binary summary kept");
    check(test_result.stdout_text.contains("collected 1 test binaries"),
          "workspace test human: second member binary summary kept");
    check(test_result.stdout_text.contains("exon: workspace test succeeded"),
          "workspace test human: aggregate summary");
    check(!test_result.stdout_text.contains("exon: workspace test succeeded\n  collected "),
          "workspace test human: aggregate summary is no longer binary-looking");
}

void test_workspace_human_fail_fast_summary() {
    auto result = run_self_fixture("workspace-test-failfast");
    check(result.exit_code == 1, "workspace test fail-fast: exit code");
    check(result.stdout_text.contains("==> member app (app)"),
          "workspace test fail-fast: first member ran");
    check(result.stdout_text.contains("==> member broken (broken)"),
          "workspace test fail-fast: failing member ran");
    check(!result.stdout_text.contains("==> member after (after)"),
          "workspace test fail-fast: later member skipped after failure");
    check(result.stdout_text.contains("members   2 run, 1 passed, 1 failed"),
          "workspace test fail-fast: executed member summary");
    check(result.stdout_text.contains("binaries  3 run"),
          "workspace test fail-fast: executed binary summary");
    check(result.stdout_text.contains("exon: workspace test failed"),
          "workspace test fail-fast: failure footer");
}

void test_module_detection_includes_dependency_cppm_sources() {
    TmpProject proj;
    proj.write("src/main.cpp", "int main() { return 0; }\n");
    proj.write("deps/phenotype/src/phenotype.cppm", "export module phenotype;\n");

    fetch::FetchedDep dep;
    dep.name = "phenotype";
    dep.path = proj.root / "deps" / "phenotype";

    check(build::system::detail::request_uses_cpp_modules(proj.root, {dep}, false),
          "module detection: dependency cppm sources mark request as module-aware");
}

#if defined(_WIN32)
void test_windows_native_toolchain_diagnostic_predicates() {
    build::BuildRequest request;
    request.module_aware = true;
    request.project.root = std::filesystem::temp_directory_path() / "exon_test_windows_diag";
    request.toolchain.compiler_kind = toolchain::CompilerKind::clang_cl;
    request.toolchain.compiler_from_environment = true;
    request.toolchain.has_msvc_developer_env = true;

    reporting::ProcessResult failure;
    failure.exit_code = 1;
    failure.stderr_text =
        "The target named \"phenotype\" has C++ sources that may use modules, but the compiler does not provide a way to discover the import graph dependencies.\nUse the CMAKE_CXX_SCAN_FOR_MODULES variable to enable or disable scanning.\n";

    check(build::system::detail::should_warn_for_windows_native_toolchain(request),
          "windows diag: mixed clang-cl/msvc env warns before configure");
    check(build::system::detail::should_explain_windows_native_toolchain_failure(request, failure),
          "windows diag: mixed clang-cl/msvc env explains matching configure failure");

    auto preflight = build::system::detail::windows_native_toolchain_preflight_message();
    check(preflight.contains("clang-cl from CC/CXX"),
          "windows diag: preflight mentions clang-cl from CC/CXX");
    auto hint = build::system::detail::windows_native_toolchain_failure_message();
    check(hint.contains(".intron.toml"),
          "windows diag: failure hint mentions .intron.toml");
    check(hint.contains("$env:CC='cl'; $env:CXX='cl'; exon build"),
          "windows diag: failure hint shows cl rerun example");

    request.module_aware = false;
    check(!build::system::detail::should_warn_for_windows_native_toolchain(request),
          "windows diag: non-module request skips preflight warning");

    request.module_aware = true;
    request.project.target = "wasm32-wasi";
    request.project.is_wasm = true;
    check(!build::system::detail::should_warn_for_windows_native_toolchain(request),
          "windows diag: non-native target skips preflight warning");

    request.project.target.clear();
    request.project.is_wasm = false;
    request.toolchain.compiler_kind = toolchain::CompilerKind::msvc_cl;
    check(!build::system::detail::should_warn_for_windows_native_toolchain(request),
          "windows diag: cl environment skips preflight warning");

    request.toolchain.compiler_kind = toolchain::CompilerKind::clang_cl;
    request.toolchain.compiler_from_environment = false;
    check(!build::system::detail::should_warn_for_windows_native_toolchain(request),
          "windows diag: non-env clang-cl skips preflight warning");

    request.toolchain.compiler_from_environment = true;
    auto no_signature = reporting::ProcessResult{.exit_code = 1, .stderr_text = "plain failure"};
    check(!build::system::detail::should_explain_windows_native_toolchain_failure(
              request, no_signature),
          "windows diag: unrelated failures skip extra hint");
}
#endif

void test_fetch_and_generate_cmake_root_override_fixture() {
    TmpProject proj;
    proj.write("exon.toml", "");
    auto repos = proj.root / "repos";
    std::filesystem::create_directories(repos);

    TmpGitRepo cppx_repo{repos, "cppx"};
    cppx_repo.write("exon.toml", R"([package]
name = "cppx"
version = "1.0.3"
type = "lib"
standard = 23
)");
    cppx_repo.write("src/cppx.cppm", "export module cppx;");
    cppx_repo.init_and_tag("v1.0.3");
    cppx_repo.write("exon.toml", R"([package]
name = "cppx"
version = "1.2.0"
type = "lib"
standard = 23
)");
    cppx_repo.commit_and_tag("v1.2.0");

    TmpGitRepo txn_repo{repos, "txn"};
    txn_repo.write("exon.toml", std::format(R"([package]
name = "txn"
version = "0.6.1"
type = "lib"
standard = 23

[dependencies]
"{}" = "1.0.3"
)", cppx_repo.key()));
    txn_repo.write("src/txn.cppm", "export module txn;");
    txn_repo.init_and_tag("v0.6.1");

    proj.write("exon.toml", std::format(R"([package]
name = "app"
version = "0.1.0"
type = "bin"
standard = 23

[dependencies]
"{}" = "0.6.1"
"{}" = "1.2.0"
)", txn_repo.key(), cppx_repo.key()));
    proj.write("src/main.cpp", "int main() { return 0; }\n");

    auto app_manifest = manifest::system::load((proj.root / "exon.toml").string());
    auto fetch_result = fetch::system::fetch_all(
        proj.root, app_manifest, proj.root / "exon.lock", false);
    auto cmake = build::generate_cmake(app_manifest, proj.root, fetch_result.deps, make_tc());

    auto first = cmake.find("add_library(cppx)");
    check(first != std::string::npos, "override fixture: cppx target emitted");
    check(cmake.find("add_library(cppx)", first + 1) == std::string::npos,
          "override fixture: cppx target emitted once");
    check(cmake.contains("target_link_libraries(txn PUBLIC\n    cppx\n)"),
          "override fixture: txn links canonical cppx");
}

int main(int argc, char* argv[]) {
    if (auto rc = maybe_run_fake_intron(argc, argv); rc >= 0)
        return rc;
    if (auto rc = maybe_run_output_fixture(argc, argv); rc >= 0)
        return rc;

    self_executable_path = std::filesystem::weakly_canonical(std::filesystem::path{argv[0]});

#if !defined(_WIN32)
    test_portable_windows_asan_helper_configures_on_non_windows();
#endif
    test_portable_intron_config_installs_host_tools_from_project_root();
    test_portable_intron_config_skips_when_intron_missing();
#if defined(_WIN32)
    test_windows_legacy_common_llvm_is_skipped();
    test_windows_explicit_llvm_override_is_installed();
#endif
    test_run_process_returns_child_exit_code();
    test_system_run_process_honors_cwd();
    test_reporting_capture_collects_stdout_and_stderr();
    test_reporting_capture_observer_receives_chunks();
    test_reporting_tee_collects_output();
    test_reporting_timeout_marks_result();
    test_ninja_progress_helpers();
    test_live_progress_frame_format();
    test_human_build_success_output();
    test_human_build_cached_output();
    test_human_build_failure_output();
    test_human_test_failed_output_mode();
    test_human_test_all_output_mode();
    test_human_test_none_output_mode();
    test_wrapped_output_regressions();
    test_workspace_human_output();
    test_workspace_human_fail_fast_summary();
    test_module_detection_includes_dependency_cppm_sources();
#if defined(_WIN32)
    test_windows_native_toolchain_diagnostic_predicates();
#endif
    test_prepare_request_keeps_portable_root_sync_inputs_unresolved();
    test_fetch_and_generate_cmake_root_override_fixture();

    if (failures > 0) {
        std::println("{} test(s) failed", failures);
        return 1;
    }

    std::println("test_regressions: all passed");
    return 0;
}
