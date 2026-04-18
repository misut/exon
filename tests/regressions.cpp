import std;
import build;
import build.system;
import core;
import fetch;
import fetch.system;
import manifest;
import manifest.system;
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
        auto file = std::ofstream{path};
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
    test_prepare_request_keeps_portable_root_sync_inputs_unresolved();
    test_fetch_and_generate_cmake_root_override_fixture();

    if (failures > 0) {
        std::println("{} test(s) failed", failures);
        return 1;
    }

    std::println("test_regressions: all passed");
    return 0;
}
