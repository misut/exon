module;
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

export module build.system;
import std;
import toml;
import build;
import core;
import fetch;
import fetch.system;
import manifest;
import manifest.system;
import toolchain;
import toolchain.system;
import vcpkg.system;

export namespace build::system {

bool sync_root_cmake(std::filesystem::path const& project_root,
                     manifest::Manifest const& m,
                     std::vector<fetch::FetchedDep> const& deps);
int run(std::filesystem::path const& project_root, manifest::Manifest const& m,
        bool release, std::string_view target);
int run_check(std::filesystem::path const& project_root,
              manifest::Manifest const& m, bool release,
              std::string_view target);
int run_test(std::filesystem::path const& project_root,
             manifest::Manifest const& m, bool release,
             std::string_view target, std::string_view filter,
             std::optional<std::chrono::milliseconds> timeout);

namespace detail {

struct CurrentPathGuard {
    std::filesystem::path previous;
    bool active = false;

    explicit CurrentPathGuard(std::filesystem::path const& next) {
        if (next.empty())
            return;
        previous = std::filesystem::current_path();
        std::filesystem::current_path(next);
        active = true;
    }

    ~CurrentPathGuard() {
        if (active)
            std::filesystem::current_path(previous);
    }
};

struct CommandResult {
    int exit_code = 0;
    bool timed_out = false;
};

#if defined(_WIN32)
std::wstring widen_command(std::string_view command) {
    if (command.empty())
        return {};
    auto size = MultiByteToWideChar(CP_ACP, 0, command.data(),
                                    static_cast<int>(command.size()), nullptr, 0);
    if (size <= 0)
        throw std::runtime_error("failed to convert command line to UTF-16");
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_ACP, 0, command.data(), static_cast<int>(command.size()),
                        wide.data(), size);
    return wide;
}

CommandResult run_command_windows(std::string_view command,
                                  std::optional<std::chrono::milliseconds> timeout = {}) {
    auto wide = widen_command(command);
    std::vector<wchar_t> mutable_cmd(wide.begin(), wide.end());
    mutable_cmd.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info,
                                     sizeof(info))) {
            CloseHandle(job);
            job = nullptr;
        }
    }

    auto old_error_mode =
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    auto restore_error_mode = [&] { SetErrorMode(old_error_mode); };

    if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                        &si, &pi)) {
        auto error = GetLastError();
        if (job != nullptr)
            CloseHandle(job);
        restore_error_mode();
        throw std::runtime_error(
            std::format("failed to start process (GetLastError={})", error));
    }

    if (job != nullptr && !AssignProcessToJobObject(job, pi.hProcess)) {
        CloseHandle(job);
        job = nullptr;
    }

    auto wait_ms = timeout
        ? static_cast<DWORD>(std::min<std::chrono::milliseconds::rep>(
              timeout->count(), std::numeric_limits<DWORD>::max()))
        : INFINITE;
    auto wait_result = WaitForSingleObject(pi.hProcess, wait_ms);

    CommandResult result;
    if (wait_result == WAIT_TIMEOUT) {
        result.timed_out = true;
        if (job != nullptr) {
            TerminateJobObject(job, 124);
        } else {
            TerminateProcess(pi.hProcess, 124);
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        result.exit_code = 124;
    } else if (wait_result != WAIT_OBJECT_0) {
        auto error = GetLastError();
        if (job != nullptr)
            CloseHandle(job);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        restore_error_mode();
        throw std::runtime_error(
            std::format("failed while waiting for process (GetLastError={})", error));
    } else {
        DWORD exit_code = 0;
        if (!GetExitCodeProcess(pi.hProcess, &exit_code)) {
            auto error = GetLastError();
            if (job != nullptr)
                CloseHandle(job);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            restore_error_mode();
            throw std::runtime_error(
                std::format("failed to read process exit code (GetLastError={})", error));
        }
        result.exit_code = static_cast<int>(exit_code);
    }

    if (job != nullptr)
        CloseHandle(job);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    restore_error_mode();
    return result;
}
#endif

int normalize_unix_exit_status(int status) {
    if (status == -1)
        return 1;
    if ((status & 0x7f) == 0)
        return (status >> 8) & 0xff;
    if ((status & 0x7f) != 0x7f)
        return 128 + (status & 0x7f);
    return status;
}

CommandResult run_command(std::string_view command,
                          std::optional<std::chrono::milliseconds> timeout = {}) {
#if defined(_WIN32)
    return run_command_windows(command, timeout);
#else
    (void)timeout;
    auto rc = std::system(std::string{command}.c_str());
    return CommandResult{.exit_code = normalize_unix_exit_status(rc)};
#endif
}

CommandResult run_process(core::ProcessSpec const& spec) {
    CurrentPathGuard guard{spec.cwd};
    return run_command(spec.command, spec.timeout);
}

std::string read_file(std::filesystem::path const& path) {
    auto file = std::ifstream(path, std::ios::binary);
    if (!file)
        return {};
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

bool write_if_changed(core::FileWrite const& write) {
    std::filesystem::create_directories(write.path.parent_path());
    auto existing = read_file(write.path);
    if (write.skip_if_unchanged && existing == write.content)
        return false;

    auto file = std::ofstream(write.path, std::ios::binary);
    if (!file)
        throw std::runtime_error(std::format("failed to write {}", write.path.string()));
    file << write.content;
    if (!write.success_message.empty())
        std::println("{}", write.success_message);
    return true;
}

bool apply_writes(std::vector<core::FileWrite> const& writes) {
    bool changed = false;
    for (auto const& write : writes)
        changed = write_if_changed(write) || changed;
    return changed;
}

void ensure_intron_tools(std::filesystem::path const& project_root) {
    auto intron_toml = project_root / ".intron.toml";
    if (!std::filesystem::exists(intron_toml))
        return;

#if defined(_WIN32)
    constexpr auto null_redirect = "intron help > NUL 2>&1";
#else
    constexpr auto null_redirect = "intron help > /dev/null 2>&1";
#endif
    if (std::system(null_redirect) != 0)
        return;

    auto table = toml::parse_file(intron_toml.string());
    if (!table.contains("toolchain"))
        return;

    auto home = toolchain::system::home_dir();
    if (home.empty())
        return;
    auto intron_root = home / ".intron" / "toolchains";

    auto const& tools = table.at("toolchain").as_table();
    for (auto const& [tool, ver_val] : tools) {
#if defined(_WIN32)
        if (tool == "llvm")
            continue;
#endif
        auto version = ver_val.as_string();
        auto tool_path = intron_root / tool / version;
        if (std::filesystem::exists(tool_path))
            continue;
        std::println("installing {} {}...", tool, version);
        auto cmd = std::format("intron install {} {}", tool, version);
        if (std::system(cmd.c_str()) != 0)
            std::println(std::cerr, "warning: failed to install {} {}", tool, version);
    }
}

std::filesystem::path setup_vcpkg(manifest::Manifest const& m,
                                  std::filesystem::path const& exon_dir) {
    if (m.vcpkg_deps.empty() && m.dev_vcpkg_deps.empty())
        return {};
    auto root = vcpkg::system::require_root();
    vcpkg::system::write_manifest(m, exon_dir / "vcpkg.json");
    return root / "scripts" / "buildsystems" / "vcpkg.cmake";
}

struct SourceFiles {
    std::vector<std::string> cpp;
    std::vector<std::string> cppm;
};

SourceFiles collect_sources(std::filesystem::path const& src_dir) {
    SourceFiles sf;
    if (!std::filesystem::exists(src_dir))
        return sf;
    for (auto const& entry : std::filesystem::directory_iterator(src_dir)) {
        if (!entry.is_regular_file())
            continue;
        auto ext = entry.path().extension();
        auto path = std::filesystem::canonical(entry.path()).generic_string();
        if (ext == ".cpp")
            sf.cpp.push_back(path);
        else if (ext == ".cppm")
            sf.cppm.push_back(path);
    }
    std::ranges::sort(sf.cpp);
    std::ranges::sort(sf.cppm);
    return sf;
}

void validate_wasm_dependencies(manifest::Manifest const& m) {
    if (!m.vcpkg_deps.empty() || !m.dev_vcpkg_deps.empty())
        throw std::runtime_error("vcpkg dependencies are not supported for WASM targets");
    if (!m.find_deps.empty() || !m.dev_find_deps.empty())
        throw std::runtime_error(
            "find_package dependencies are not supported for WASM targets; "
            "use git or path dependencies instead");
}

bool has_any_cmake_deps(manifest::Manifest const& m,
                        std::vector<fetch::FetchedDep> const& deps,
                        toolchain::Platform const& platform) {
    if (!m.cmake_deps.empty())
        return true;

    for (auto const& dep : deps) {
        if (!dep.subdir.empty())
            continue;
        auto dep_manifest_path = dep.path / "exon.toml";
        if (!std::filesystem::exists(dep_manifest_path))
            continue;
        auto dep_manifest = manifest::system::load(dep_manifest_path.string());
        dep_manifest = manifest::resolve_for_platform(std::move(dep_manifest), platform);
        if (!dep_manifest.cmake_deps.empty())
            return true;
    }

    return false;
}

build::BuildRequest prepare_request(std::filesystem::path const& project_root,
                                    manifest::Manifest const& m,
                                    bool release,
                                    std::string_view target,
                                    bool with_tests,
                                    std::string_view filter,
                                    std::optional<std::chrono::milliseconds> timeout) {
    build::ensure_fresh_self_host_bootstrap(m, project_root);
    ensure_intron_tools(project_root);

    bool is_wasm = !target.empty();
    if (is_wasm)
        validate_wasm_dependencies(m);

    auto project = build::project_context(project_root, release, target);
    auto tc = toolchain::system::detect();

    std::string wasm_toolchain_file;
    std::string wasm_runtime;
    if (is_wasm) {
        auto wasm_tc = toolchain::system::detect_wasm(target);
        wasm_toolchain_file = wasm_tc.cmake_toolchain;
        tc.stdlib_modules_json = wasm_tc.modules_json;
        tc.cxx_compiler = wasm_tc.scan_deps;
        tc.sysroot.clear();
        tc.lib_dir.clear();
        tc.has_clang_config = false;
        tc.needs_stdlib_flag = false;

        if (with_tests) {
            wasm_runtime = toolchain::system::detect_wasm_runtime();
            if (wasm_runtime.empty())
                throw std::runtime_error(
                    "wasmtime not found on PATH (install: https://wasmtime.dev)");
        }
    }

    auto platform = target.empty()
        ? toolchain::detect_host_platform()
        : *toolchain::platform_from_target(target);
    auto fetch_result = fetch::system::fetch_all({
        .manifest = m,
        .project_root = project_root,
        .lock_path = project_root / "exon.lock",
        .include_dev = with_tests,
        .platform = platform,
    });
    auto vcpkg_toolchain = is_wasm ? std::filesystem::path{}
                                   : setup_vcpkg(m, project.exon_dir);
    auto any_cmake_deps = has_any_cmake_deps(m, fetch_result.deps, platform);

    build::BuildRequest request{
        .project = project,
        .manifest = m,
        .toolchain = std::move(tc),
        .deps = std::move(fetch_result.deps),
        .release = release,
        .with_tests = with_tests,
        .configured = std::filesystem::exists(project.build_dir / "build.ninja"),
        .any_cmake_deps = any_cmake_deps,
        .wasm_toolchain_file = std::move(wasm_toolchain_file),
        .vcpkg_toolchain = std::move(vcpkg_toolchain),
        .filter = std::string{filter},
        .wasm_runtime = std::move(wasm_runtime),
        .timeout = timeout,
    };

    auto src_files = collect_sources(project_root / "src");
    auto tests_dir = project_root / "tests";
    if (with_tests) {
        if (!std::filesystem::exists(tests_dir))
            throw std::runtime_error("tests/ directory not found");

        auto test_files = collect_sources(tests_dir);
        if (test_files.cpp.empty())
            throw std::runtime_error("no test files found in tests/");

        for (auto const& test_cpp : test_files.cpp) {
            auto name = std::format("test-{}", std::filesystem::path{test_cpp}.stem().string());
            if (filter.empty() || name.contains(filter)) {
                request.test_names.push_back(name);
                request.build_targets.push_back(std::move(name));
            }
        }

        if (request.test_names.empty())
            throw std::runtime_error(
                std::format("no tests matched filter '{}'", std::string{filter}));
    } else if (!src_files.cppm.empty()) {
        request.build_targets.push_back(std::format("{}-modules", m.name));
    } else {
        request.build_targets.push_back(m.name);
    }

    return request;
}

int run_steps(std::vector<core::ProcessSpec> const& steps) {
    for (auto const& step : steps) {
        if (!step.label.empty())
            std::println("{}", step.label);
        auto result = run_process(step);
        if (result.exit_code != 0)
            return result.exit_code;
    }
    return 0;
}

int run_tests(std::vector<core::ProcessSpec> const& steps) {
    std::println("running tests...\n");
    int passed = 0;
    int failed = 0;
    for (auto const& step : steps) {
        auto result = run_process(step);
        auto const& name = step.label;
        if (result.timed_out) {
            std::println("  {} ... TIMEOUT", name);
            ++failed;
        } else if (result.exit_code == 0) {
            std::println("  {} ... ok", name);
            ++passed;
        } else {
            std::println("  {} ... FAILED", name);
            ++failed;
        }
    }

    std::println("");
    if (failed > 0) {
        std::println("{} passed, {} failed", passed, failed);
        return 1;
    }
    std::println("all {} tests passed", passed);
    return 0;
}

} // namespace detail

inline bool sync_root_cmake(manifest::Manifest const& m,
                            std::vector<fetch::FetchedDep> const& deps) {
    return sync_root_cmake(std::filesystem::current_path(), m, deps);
}

inline bool sync_root_cmake(std::filesystem::path const& project_root,
                            manifest::Manifest const& m,
                            std::vector<fetch::FetchedDep> const& deps) {
    if (!m.sync_cmake_in_root)
        return false;
    return detail::write_if_changed({
        .path = project_root / "CMakeLists.txt",
        .content = build::generate_portable_cmake(m, project_root, deps),
        .success_message = "synced CMakeLists.txt",
    });
}

inline int run_process(std::string_view command,
                       std::optional<std::chrono::milliseconds> timeout = {}) {
    return detail::run_command(command, timeout).exit_code;
}

inline int run_process(core::ProcessSpec const& spec) {
    return detail::run_process(spec).exit_code;
}

inline int run(manifest::Manifest const& m, bool release = false, std::string_view target = {}) {
    return run(std::filesystem::current_path(), m, release, target);
}

inline int run(std::filesystem::path const& project_root, manifest::Manifest const& m,
               bool release = false, std::string_view target = {}) {
    auto request = detail::prepare_request(project_root, m, release, target, false, {}, {});
    auto plan = build::plan_build(request);
    auto changed = detail::apply_writes(plan.writes);
    if (changed || !plan.configured) {
        auto rc = detail::run_steps(plan.configure_steps);
        if (rc != 0)
            return rc;
    }

    auto rc = detail::run_steps(plan.build_steps);
    if (rc != 0)
        return rc;

    if (!plan.success_message.empty())
        std::println("{}", plan.success_message);
    return 0;
}

inline int run_check(manifest::Manifest const& m, bool release = false,
                     std::string_view target = {}) {
    return run_check(std::filesystem::current_path(), m, release, target);
}

inline int run_check(std::filesystem::path const& project_root,
                     manifest::Manifest const& m, bool release = false,
                     std::string_view target = {}) {
    auto request = detail::prepare_request(project_root, m, release, target, false, {}, {});
    auto plan = build::plan_check(request);
    auto changed = detail::apply_writes(plan.writes);
    if (changed || !plan.configured) {
        auto rc = detail::run_steps(plan.configure_steps);
        if (rc != 0)
            return rc;
    }

    auto rc = detail::run_steps(plan.build_steps);
    if (rc != 0)
        return rc;

    if (!plan.success_message.empty())
        std::println("{}", plan.success_message);
    return 0;
}

inline int run_test(manifest::Manifest const& m, bool release = false,
                    std::string_view target = {}, std::string_view filter = {},
                    std::optional<std::chrono::milliseconds> timeout = {}) {
    return run_test(std::filesystem::current_path(), m, release, target, filter, timeout);
}

inline int run_test(std::filesystem::path const& project_root,
                    manifest::Manifest const& m, bool release = false,
                    std::string_view target = {}, std::string_view filter = {},
                    std::optional<std::chrono::milliseconds> timeout = {}) {
    auto request = detail::prepare_request(project_root, m, release, target, true, filter,
                                           timeout);
    auto plan = build::plan_test(request);
    auto changed = detail::apply_writes(plan.writes);
    if (changed || !plan.configured) {
        auto rc = detail::run_steps(plan.configure_steps);
        if (rc != 0)
            return rc;
    }

    auto rc = detail::run_steps(plan.build_steps);
    if (rc != 0)
        return rc;

    return detail::run_tests(plan.run_steps);
}

} // namespace build::system
