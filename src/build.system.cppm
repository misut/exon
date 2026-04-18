module;
#include <cstdio>

export module build.system;
import std;
import build;
import core;
import cppx.env.system;
import cppx.fs;
import cppx.fs.system;
import cppx.process;
import cppx.process.system;
import fetch;
import fetch.system;
import manifest;
import manifest.system;
import reporting;
import reporting.system;
import toml;
import toolchain;
import toolchain.system;
import vcpkg.system;

export namespace build::system {

bool sync_root_cmake(std::filesystem::path const& project_root,
                     manifest::Manifest const& m,
                     std::vector<fetch::FetchedDep> const& deps);
int run(std::filesystem::path const& project_root, manifest::Manifest const& m,
        bool release, std::string_view target);
int run(std::filesystem::path const& project_root, manifest::Manifest const& m,
        bool release, std::string_view target, std::string_view output_mode_text);
int run(std::filesystem::path const& project_root, manifest::Manifest const& m,
        bool release, std::string_view target, reporting::Options const& options);
int run(std::filesystem::path const& project_root, manifest::Manifest const& m,
        manifest::Manifest const& portable_manifest, bool release, std::string_view target);
int run(std::filesystem::path const& project_root, manifest::Manifest const& m,
        manifest::Manifest const& portable_manifest, bool release, std::string_view target,
        std::string_view output_mode_text);
int run(std::filesystem::path const& project_root, manifest::Manifest const& m,
        manifest::Manifest const& portable_manifest, bool release, std::string_view target,
        reporting::Options const& options);
int run_check(std::filesystem::path const& project_root,
              manifest::Manifest const& m, bool release,
              std::string_view target);
int run_check(std::filesystem::path const& project_root,
              manifest::Manifest const& m, manifest::Manifest const& portable_manifest,
              bool release, std::string_view target);
int run_test(std::filesystem::path const& project_root,
             manifest::Manifest const& m, bool release,
             std::string_view target, std::string_view filter,
             std::optional<std::chrono::milliseconds> timeout);
int run_test(std::filesystem::path const& project_root,
             manifest::Manifest const& m, bool release,
             std::string_view target, std::string_view filter,
             std::optional<std::chrono::milliseconds> timeout,
             std::string_view output_mode_text,
             std::string_view show_output_text);
int run_test(std::filesystem::path const& project_root,
             manifest::Manifest const& m, bool release,
             std::string_view target, std::string_view filter,
             std::optional<std::chrono::milliseconds> timeout,
             reporting::Options const& options);
int run_test(std::filesystem::path const& project_root,
             manifest::Manifest const& m, manifest::Manifest const& portable_manifest,
             bool release, std::string_view target, std::string_view filter,
             std::optional<std::chrono::milliseconds> timeout);
int run_test(std::filesystem::path const& project_root,
             manifest::Manifest const& m, manifest::Manifest const& portable_manifest,
             bool release, std::string_view target, std::string_view filter,
             std::optional<std::chrono::milliseconds> timeout,
             std::string_view output_mode_text,
             std::string_view show_output_text);
int run_test(std::filesystem::path const& project_root,
             manifest::Manifest const& m, manifest::Manifest const& portable_manifest,
             bool release, std::string_view target, std::string_view filter,
             std::optional<std::chrono::milliseconds> timeout,
             reporting::Options const& options);

namespace detail {

void write_stream(FILE* stream, std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stream);
}

void write_line(FILE* stream, std::string_view text = {}) {
    write_stream(stream, text);
    write_stream(stream, "\n");
}

template <class... Args>
void write_formatted_line(FILE* stream, std::format_string<Args...> fmt, Args&&... args) {
    write_line(stream, std::format(fmt, std::forward<Args>(args)...));
}

[[noreturn]] void throw_process_error(cppx::process::process_error error,
                                      core::ProcessSpec const& spec) {
    throw std::runtime_error(std::format(
        "failed to run '{}' ({})", spec.program, cppx::process::to_string(error)));
}

[[noreturn]] void throw_fs_error(cppx::fs::fs_error error,
                                 std::filesystem::path const& path,
                                 std::string_view action) {
    throw std::runtime_error(std::format(
        "failed to {} '{}' ({})", action, path.string(), cppx::fs::to_string(error)));
}

core::ProcessResult run_process(core::ProcessSpec const& spec) {
    auto result = cppx::process::system::run(spec);
    if (!result)
        throw_process_error(result.error(), spec);
    return *result;
}

bool write_if_changed(core::FileWrite const& write, bool print_success_message = true) {
    auto result = cppx::fs::system::write_if_changed(write.text);
    if (!result)
        throw_fs_error(result.error(), write.text.path, "write");
    if (print_success_message && *result && !write.success_message.empty())
        std::println("{}", write.success_message);
    return *result;
}

bool apply_writes(std::vector<core::FileWrite> const& writes, bool print_success_messages = true) {
    bool changed = false;
    for (auto const& write : writes)
        changed = write_if_changed(write, print_success_messages) || changed;
    return changed;
}

std::optional<std::string_view> host_toolchain_section() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return std::nullopt;
#endif
}

void merge_intron_tools(std::map<std::string, std::string>& resolved,
                        toml::Table const& tools,
                        bool skip_common_windows_llvm = false) {
    for (auto const& [tool, value] : tools) {
        if (!value.is_string())
            continue;
#if defined(_WIN32)
        if (skip_common_windows_llvm && tool == "llvm")
            continue;
#else
        (void)skip_common_windows_llvm;
#endif
        resolved[tool] = value.as_string();
    }
}

std::map<std::string, std::string> resolve_intron_tools(std::filesystem::path const& intron_toml) {
    auto table = toml::parse_file(intron_toml.string());
    if (!table.contains("toolchain") || !table.at("toolchain").is_table())
        return {};

    auto resolved = std::map<std::string, std::string>{};
    auto const& root = table.at("toolchain").as_table();
    merge_intron_tools(resolved, root, true);

    if (auto host = host_toolchain_section()) {
        auto host_key = std::string{*host};
        if (root.contains(host_key) && root.at(host_key).is_table())
            merge_intron_tools(resolved, root.at(host_key).as_table());
    }

    return resolved;
}

void ensure_intron_tools(std::filesystem::path const& project_root) {
    auto intron_toml = project_root / ".intron.toml";
    if (!std::filesystem::exists(intron_toml))
        return;

    auto intron = cppx::env::system::find_in_path("intron");
    if (!intron)
        return;

    auto home = toolchain::system::home_dir();
    if (home.empty())
        return;
    auto intron_root = home / ".intron" / "toolchains";

    auto tools = resolve_intron_tools(intron_toml);
    for (auto const& [tool, version] : tools) {
        auto tool_path = intron_root / tool / version;
        if (std::filesystem::exists(tool_path))
            continue;

        std::println("installing {} {}...", tool, version);
        core::ProcessSpec spec{
            .program = intron->string(),
            .args = {"install", tool, version},
            .cwd = project_root,
        };

        try {
            auto result = run_process(spec);
            if (result.exit_code != 0) {
                std::println(std::cerr,
                             "warning: 'intron install {} {}' exited with code {} in {}",
                             tool, version, result.exit_code, project_root.string());
            }
        } catch (std::exception const& e) {
            std::println(std::cerr,
                         "warning: failed to run 'intron install {} {}' in {}: {}",
                         tool, version, project_root.string(), e.what());
        }
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

build::BuildRequest do_prepare_request(
    std::filesystem::path const& project_root,
    manifest::Manifest const& build_manifest,
    manifest::Manifest const* portable_manifest,
    bool release,
    std::string_view target,
    bool with_tests,
    std::string_view filter,
    std::optional<std::chrono::milliseconds> timeout) {
    build::ensure_fresh_self_host_bootstrap(build_manifest, project_root);
    ensure_intron_tools(project_root);

    bool is_wasm = !target.empty();
    if (is_wasm)
        validate_wasm_dependencies(build_manifest);

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
        .manifest = build_manifest,
        .project_root = project_root,
        .lock_path = project_root / "exon.lock",
        .include_dev = with_tests,
        .platform = platform,
    });
    auto vcpkg_toolchain = is_wasm ? std::filesystem::path{}
                                   : setup_vcpkg(build_manifest, project.exon_dir);
    auto any_cmake_deps = has_any_cmake_deps(build_manifest, fetch_result.deps, platform);

    build::BuildRequest request{
        .project = project,
        .manifest = build_manifest,
        .portable_manifest = portable_manifest ? std::optional{*portable_manifest}
                                               : std::nullopt,
        .toolchain = std::move(tc),
        .deps = std::move(fetch_result.deps),
        .portable_deps = {},
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

    if (portable_manifest && portable_manifest->sync_cmake_in_root) {
        auto portable_fetch_manifest = manifest::resolve_all_targets(*portable_manifest);
        auto portable_fetch_result = fetch::system::fetch_all({
            .manifest = std::move(portable_fetch_manifest),
            .project_root = project_root,
            .lock_path = project_root / "exon.lock",
            .include_dev = with_tests,
        });
        request.portable_deps = std::move(portable_fetch_result.deps);
    }

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
        request.build_targets.push_back(std::format("{}-modules", build_manifest.name));
    } else {
        request.build_targets.push_back(build_manifest.name);
    }

    return request;
}

build::BuildRequest prepare_request(std::filesystem::path const& project_root,
                                    manifest::Manifest const& m,
                                    bool release,
                                    std::string_view target,
                                    bool with_tests,
                                    std::string_view filter,
                                    std::optional<std::chrono::milliseconds> timeout) {
    return do_prepare_request(project_root, m, nullptr, release, target, with_tests, filter,
                              timeout);
}

build::BuildRequest prepare_request(std::filesystem::path const& project_root,
                                    manifest::Manifest const& m,
                                    manifest::Manifest const& portable_manifest,
                                    bool release,
                                    std::string_view target,
                                    bool with_tests,
                                    std::string_view filter,
                                    std::optional<std::chrono::milliseconds> timeout) {
    return do_prepare_request(project_root, m, &portable_manifest, release, target, with_tests,
                              filter, timeout);
}

std::string profile_label(core::ProjectContext const& project) {
    return project.profile;
}

std::string target_label(core::ProjectContext const& project) {
    return project.target.empty() ? "native" : project.target;
}

std::string display_path(std::filesystem::path const& path,
                         std::filesystem::path const& root) {
    std::error_code ec;
    auto relative = std::filesystem::relative(path, root, ec);
    if (!ec && !relative.empty()) {
        auto relative_text = relative.generic_string();
        if (relative_text != "." && !relative_text.starts_with("../"))
            return relative_text;
    }
    return path.generic_string();
}

void print_header(std::string_view verb, std::string_view name,
                  core::ProjectContext const& project) {
    write_formatted_line(stdout, "exon: {} {} ({})", verb, name, profile_label(project));
    write_formatted_line(stdout, "  target    {}", target_label(project));
    write_formatted_line(stdout, "  build dir {}", display_path(project.build_dir, project.root));
    write_line(stdout);
}

void print_stage(std::string_view stage) {
    write_formatted_line(stdout, "==> {}", stage);
}

void print_failure_summary(std::string_view verb, std::string_view stage,
                           core::ProjectContext const& project,
                           std::chrono::milliseconds elapsed) {
    write_line(stdout);
    write_formatted_line(stdout, "exon: {} failed", verb);
    write_formatted_line(stdout, "  phase     {}", stage);
    write_formatted_line(stdout, "  profile   {}", profile_label(project));
    write_formatted_line(stdout, "  target    {}", target_label(project));
    write_formatted_line(stdout, "  build dir {}", display_path(project.build_dir, project.root));
    write_formatted_line(stdout, "  elapsed   {}", reporting::format_duration(elapsed));
}

void print_build_success(std::string_view verb, std::filesystem::path const& artifact,
                         core::ProjectContext const& project,
                         std::chrono::milliseconds elapsed) {
    write_line(stdout);
    write_formatted_line(stdout, "exon: {} succeeded", verb);
    write_formatted_line(stdout, "  artifact  {}", display_path(artifact, project.root));
    write_formatted_line(stdout, "  elapsed   {}", reporting::format_duration(elapsed));
}

void print_test_summary(int collected, int passed, int failed, int timed_out,
                        core::ProjectContext const& project,
                        std::chrono::milliseconds elapsed) {
    write_line(stdout);
    write_formatted_line(stdout, "exon: test {}",
                         (failed == 0 && timed_out == 0) ? "succeeded" : "failed");
    write_formatted_line(stdout, "  collected {}", collected);
    write_formatted_line(stdout, "  passed    {}", passed);
    write_formatted_line(stdout, "  failed    {}", failed);
    write_formatted_line(stdout, "  timed out {}", timed_out);
    write_formatted_line(stdout, "  target    {}", target_label(project));
    write_formatted_line(stdout, "  elapsed   {}", reporting::format_duration(elapsed));
}

void print_captured_output(std::string_view name, reporting::ProcessResult const& result,
                           reporting::ShowOutput show_output) {
    auto failed = result.timed_out || result.exit_code != 0;
    if (!reporting::should_show_output(show_output, failed))
        return;

    if (!result.stdout_text.empty()) {
        write_line(stdout);
        write_formatted_line(stdout, "---- output: {} (stdout) ----", name);
        write_stream(stdout, result.stdout_text);
        if (!result.stdout_text.ends_with('\n'))
            write_line(stdout);
    }
    if (!result.stderr_text.empty()) {
        write_line(stdout);
        write_formatted_line(stdout, "---- output: {} (stderr) ----", name);
        write_stream(stderr, result.stderr_text);
        if (!result.stderr_text.ends_with('\n'))
            write_line(stderr);
    }
}

#if defined(_WIN32)
extern "C" int _putenv(char const* envstring);
#else
extern "C" int setenv(char const* name, char const* value, int overwrite);
extern "C" int unsetenv(char const* name);
#endif

struct EnvVarGuard {
    std::string name;
    std::optional<std::string> original;

    EnvVarGuard(std::string name_, std::string value)
        : name(std::move(name_)) {
        if (auto* current = std::getenv(name.c_str()); current)
            original = current;
#if defined(_WIN32)
        auto assignment = std::format("{}={}", name, value);
        _putenv(assignment.c_str());
#else
        setenv(name.c_str(), value.c_str(), 1);
#endif
    }

    ~EnvVarGuard() {
        if (original) {
#if defined(_WIN32)
            auto assignment = std::format("{}={}", name, *original);
            _putenv(assignment.c_str());
#else
            setenv(name.c_str(), original->c_str(), 1);
#endif
            return;
        }
#if defined(_WIN32)
        auto assignment = std::format("{}=", name);
        _putenv(assignment.c_str());
#else
        unsetenv(name.c_str());
#endif
    }
};

reporting::ProcessResult run_step(core::ProcessStep const& step, reporting::StreamMode mode,
                                  bool use_ninja_status = false) {
    auto ninja_status = std::optional<EnvVarGuard>{};
    if (use_ninja_status)
        ninja_status.emplace("NINJA_STATUS", "[%f/%t] ");
    return reporting::system::run_process(step.spec, mode);
}

reporting::OutputMode parse_output_mode_text(std::string_view value) {
    auto parsed = value.empty() ? std::optional{reporting::OutputMode::wrapped}
                                : reporting::parse_output_mode(value);
    if (!parsed) {
        throw std::runtime_error(
            std::format("invalid --output '{}': expected raw or wrapped", value));
    }
    return *parsed;
}

reporting::ShowOutput parse_show_output_text(std::string_view value) {
    if (value.empty())
        return reporting::ShowOutput::failed;
    auto parsed = reporting::parse_show_output(value);
    if (!parsed) {
        throw std::runtime_error(
            std::format("invalid --show-output '{}': expected failed, all, or none", value));
    }
    return *parsed;
}

int run_steps(std::vector<core::ProcessStep> const& steps) {
    for (auto const& step : steps) {
        if (!step.label.empty())
            std::println("{}", step.label);
        auto result = run_process(step.spec);
        if (result.exit_code != 0)
            return result.exit_code;
    }
    return 0;
}

int run_steps_wrapped(std::vector<core::ProcessStep> const& steps, std::string_view stage,
                      bool use_ninja_status = false) {
    print_stage(stage);
    for (auto const& step : steps) {
        auto result = run_step(step, reporting::StreamMode::tee, use_ninja_status);
        if (result.exit_code != 0)
            return result.exit_code;
    }
    return 0;
}

int run_tests(std::vector<core::ProcessStep> const& steps) {
    std::println("running tests...\n");
    int passed = 0;
    int failed = 0;
    for (auto const& step : steps) {
        auto result = run_process(step.spec);
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

int run_tests_wrapped(std::vector<core::ProcessStep> const& steps,
                      reporting::ShowOutput show_output,
                      int& passed, int& failed, int& timed_out) {
    print_stage("run");
    for (auto const& step : steps) {
        auto result = run_step(step, reporting::StreamMode::capture);
        write_formatted_line(stdout, "  {} ... {}", step.label, reporting::test_status(result));
        print_captured_output(step.label, result, show_output);
        if (result.timed_out) {
            ++timed_out;
        } else if (result.exit_code == 0) {
            ++passed;
        } else {
            ++failed;
        }
    }
    return (failed == 0 && timed_out == 0) ? 0 : 1;
}

int run_build_wrapped(build::BuildRequest const& request, build::BuildPlan const& plan,
                      std::string_view verb,
                      std::chrono::steady_clock::time_point started) {
    print_stage("sync");
    auto changed = apply_writes(plan.writes, false);
    write_formatted_line(stdout, "  {}", changed ? "generated build files" : "build files up to date");

    if (changed || !plan.configured) {
        auto rc = run_steps_wrapped(plan.configure_steps, "configure");
        if (rc != 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            print_failure_summary(verb, "configure", request.project, elapsed);
            return rc;
        }
    } else {
        print_stage("configure");
        write_line(stdout, "  cached: existing build.ninja");
    }

    auto rc = run_steps_wrapped(plan.build_steps, "build", true);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    if (rc != 0) {
        print_failure_summary(verb, "build", request.project, elapsed);
        return rc;
    }

    auto artifact = request.project.build_dir / request.manifest.name;
    if (!request.project.is_wasm)
        artifact += toolchain::exe_suffix;
    print_build_success(verb, artifact, request.project, elapsed);
    return 0;
}

int run_test_wrapped(build::BuildRequest const& request, build::BuildPlan const& plan,
                     reporting::Options const& options,
                     std::chrono::steady_clock::time_point started) {
    print_stage("sync");
    auto changed = apply_writes(plan.writes, false);
    write_formatted_line(stdout, "  {}", changed ? "generated build files" : "build files up to date");

    if (changed || !plan.configured) {
        auto rc = run_steps_wrapped(plan.configure_steps, "configure");
        if (rc != 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            print_failure_summary("test", "configure", request.project, elapsed);
            return rc;
        }
    } else {
        print_stage("configure");
        write_line(stdout, "  cached: existing build.ninja");
    }

    auto build_rc = run_steps_wrapped(plan.build_steps, "build", true);
    if (build_rc != 0) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        print_failure_summary("test", "build", request.project, elapsed);
        return build_rc;
    }

    int passed = 0;
    int failed = 0;
    int timed_out = 0;
    write_formatted_line(stdout, "  collected {} test binaries", plan.run_steps.size());
    auto run_rc = run_tests_wrapped(plan.run_steps, options.show_output, passed, failed,
                                    timed_out);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    print_test_summary(static_cast<int>(plan.run_steps.size()), passed, failed, timed_out,
                       request.project, elapsed);
    return run_rc;
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
        .text = {
            .path = project_root / "CMakeLists.txt",
            .content = build::generate_portable_cmake(m, project_root, deps),
        },
        .success_message = "synced CMakeLists.txt",
    });
}

inline int run_process(core::ProcessSpec const& spec) {
    return detail::run_process(spec).exit_code;
}

inline int run(manifest::Manifest const& m, bool release = false, std::string_view target = {}) {
    return run(std::filesystem::current_path(), m, release, target, reporting::Options{});
}

inline int run(std::filesystem::path const& project_root, manifest::Manifest const& m,
               bool release = false, std::string_view target = {}) {
    return run(project_root, m, release, target, reporting::Options{});
}

inline int run(std::filesystem::path const& project_root, manifest::Manifest const& m,
               bool release, std::string_view target, std::string_view output_mode_text) {
    reporting::Options options{
        .output = detail::parse_output_mode_text(output_mode_text),
    };
    return run(project_root, m, release, target, options);
}

inline int run(std::filesystem::path const& project_root, manifest::Manifest const& m,
               bool release, std::string_view target,
               reporting::Options const& options) {
    if (options.output != reporting::OutputMode::raw) {
        auto started = std::chrono::steady_clock::now();
        auto project = build::project_context(project_root, release, target);
        detail::print_header("build", m.name, project);
        detail::print_stage("resolve");
        try {
            auto request = detail::prepare_request(project_root, m, release, target, false, {},
                                                   {});
            auto plan = build::plan_build(request);
            return detail::run_build_wrapped(request, plan, "build", started);
        } catch (...) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            detail::print_failure_summary("build", "resolve", project, elapsed);
            throw;
        }
    }

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

inline int run(std::filesystem::path const& project_root, manifest::Manifest const& m,
               manifest::Manifest const& portable_manifest, bool release = false,
               std::string_view target = {}) {
    return run(project_root, m, portable_manifest, release, target, reporting::Options{});
}

inline int run(std::filesystem::path const& project_root, manifest::Manifest const& m,
               manifest::Manifest const& portable_manifest, bool release,
               std::string_view target, std::string_view output_mode_text) {
    reporting::Options options{
        .output = detail::parse_output_mode_text(output_mode_text),
    };
    return run(project_root, m, portable_manifest, release, target, options);
}

inline int run(std::filesystem::path const& project_root, manifest::Manifest const& m,
               manifest::Manifest const& portable_manifest, bool release,
               std::string_view target, reporting::Options const& options) {
    if (options.output != reporting::OutputMode::raw) {
        auto started = std::chrono::steady_clock::now();
        auto project = build::project_context(project_root, release, target);
        detail::print_header("build", m.name, project);
        detail::print_stage("resolve");
        try {
            auto request = detail::prepare_request(project_root, m, portable_manifest, release,
                                                   target, false, {}, {});
            auto plan = build::plan_build(request);
            return detail::run_build_wrapped(request, plan, "build", started);
        } catch (...) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            detail::print_failure_summary("build", "resolve", project, elapsed);
            throw;
        }
    }

    auto request = detail::prepare_request(project_root, m, portable_manifest, release, target,
                                           false, {}, {});
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

inline int run_check(std::filesystem::path const& project_root,
                     manifest::Manifest const& m,
                     manifest::Manifest const& portable_manifest, bool release = false,
                     std::string_view target = {}) {
    auto request = detail::prepare_request(project_root, m, portable_manifest, release, target,
                                           false, {}, {});
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
    return run_test(std::filesystem::current_path(), m, release, target, filter, timeout,
                    reporting::Options{});
}

inline int run_test(std::filesystem::path const& project_root,
                    manifest::Manifest const& m, bool release = false,
                    std::string_view target = {}, std::string_view filter = {},
                    std::optional<std::chrono::milliseconds> timeout = {}) {
    return run_test(project_root, m, release, target, filter, timeout, reporting::Options{});
}

inline int run_test(std::filesystem::path const& project_root,
                    manifest::Manifest const& m, bool release,
                    std::string_view target, std::string_view filter,
                    std::optional<std::chrono::milliseconds> timeout,
                    std::string_view output_mode_text,
                    std::string_view show_output_text) {
    reporting::Options options{
        .output = detail::parse_output_mode_text(output_mode_text),
        .show_output = detail::parse_show_output_text(show_output_text),
    };
    return run_test(project_root, m, release, target, filter, timeout, options);
}

inline int run_test(std::filesystem::path const& project_root,
                    manifest::Manifest const& m, bool release,
                    std::string_view target, std::string_view filter,
                    std::optional<std::chrono::milliseconds> timeout,
                    reporting::Options const& options) {
    if (options.output != reporting::OutputMode::raw) {
        auto started = std::chrono::steady_clock::now();
        auto project = build::project_context(project_root, release, target);
        detail::print_header("test", m.name, project);
        detail::print_stage("resolve");
        try {
            auto request = detail::prepare_request(project_root, m, release, target, true, filter,
                                                   timeout);
            auto plan = build::plan_test(request);
            return detail::run_test_wrapped(request, plan, options, started);
        } catch (...) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            detail::print_failure_summary("test", "resolve", project, elapsed);
            throw;
        }
    }

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

inline int run_test(std::filesystem::path const& project_root,
                    manifest::Manifest const& m,
                    manifest::Manifest const& portable_manifest, bool release = false,
                    std::string_view target = {}, std::string_view filter = {},
                    std::optional<std::chrono::milliseconds> timeout = {}) {
    return run_test(project_root, m, portable_manifest, release, target, filter, timeout,
                    reporting::Options{});
}

inline int run_test(std::filesystem::path const& project_root,
                    manifest::Manifest const& m,
                    manifest::Manifest const& portable_manifest, bool release,
                    std::string_view target, std::string_view filter,
                    std::optional<std::chrono::milliseconds> timeout,
                    std::string_view output_mode_text,
                    std::string_view show_output_text) {
    reporting::Options options{
        .output = detail::parse_output_mode_text(output_mode_text),
        .show_output = detail::parse_show_output_text(show_output_text),
    };
    return run_test(project_root, m, portable_manifest, release, target, filter, timeout,
                    options);
}

inline int run_test(std::filesystem::path const& project_root,
                    manifest::Manifest const& m,
                    manifest::Manifest const& portable_manifest, bool release,
                    std::string_view target, std::string_view filter,
                    std::optional<std::chrono::milliseconds> timeout,
                    reporting::Options const& options) {
    if (options.output != reporting::OutputMode::raw) {
        auto started = std::chrono::steady_clock::now();
        auto project = build::project_context(project_root, release, target);
        detail::print_header("test", m.name, project);
        detail::print_stage("resolve");
        try {
            auto request = detail::prepare_request(project_root, m, portable_manifest, release,
                                                   target, true, filter, timeout);
            auto plan = build::plan_test(request);
            return detail::run_test_wrapped(request, plan, options, started);
        } catch (...) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            detail::print_failure_summary("test", "resolve", project, elapsed);
            throw;
        }
    }

    auto request = detail::prepare_request(project_root, m, portable_manifest, release, target,
                                           true, filter, timeout);
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
