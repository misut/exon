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

struct TestRunSummary {
    int collected = 0;
    int passed = 0;
    int failed = 0;
    int timed_out = 0;
};

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
             manifest::Manifest const& m, bool release,
             std::string_view target, std::string_view filter,
             std::optional<std::chrono::milliseconds> timeout,
             reporting::Options const& options,
             TestRunSummary& summary);
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
int run_test(std::filesystem::path const& project_root,
             manifest::Manifest const& m, manifest::Manifest const& portable_manifest,
             bool release, std::string_view target, std::string_view filter,
             std::optional<std::chrono::milliseconds> timeout,
             reporting::Options const& options,
             TestRunSummary& summary);

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

bool path_uses_modules(std::filesystem::path const& root) {
    if (!std::filesystem::exists(root))
        return false;
    return !collect_sources(root).cppm.empty();
}

bool dep_uses_modules(fetch::FetchedDep const& dep) {
    auto source_root = dep.path;
    if (!dep.subdir.empty())
        source_root /= dep.subdir;
    source_root /= "src";
    return path_uses_modules(source_root);
}

bool request_uses_cpp_modules(std::filesystem::path const& project_root,
                              std::vector<fetch::FetchedDep> const& deps,
                              bool with_tests) {
    if (path_uses_modules(project_root / "src"))
        return true;
    if (with_tests && path_uses_modules(project_root / "tests"))
        return true;
    return std::ranges::any_of(deps, [](fetch::FetchedDep const& dep) {
        return dep_uses_modules(dep);
    });
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

    bool is_cross = !target.empty();
    bool is_wasm = is_cross && target.starts_with("wasm32");
    bool is_android = is_cross && target.ends_with("-linux-android");
    if (is_wasm)
        validate_wasm_dependencies(build_manifest);

    auto project = build::project_context(project_root, release, target);
    auto tc = toolchain::system::detect();

    std::string wasm_toolchain_file;
    std::string android_toolchain_file;
    std::string android_abi;
    std::string android_platform;
    std::string android_clang_target;
    std::string android_sysroot;
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
    } else if (is_android) {
        auto android_tc = toolchain::system::detect_android(target);
        android_toolchain_file = android_tc.cmake_toolchain;
        android_abi = android_tc.abi;
        android_platform = android_tc.platform;
        android_clang_target = android_tc.clang_target;
        android_sysroot = android_tc.sysroot;
        tc.stdlib_modules_json = android_tc.modules_json;
        tc.cxx_compiler = android_tc.scan_deps;
        tc.sysroot.clear();
        tc.lib_dir.clear();
        tc.has_clang_config = false;
        tc.needs_stdlib_flag = false;
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
    auto module_aware = request_uses_cpp_modules(project_root, fetch_result.deps, with_tests);

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
        .module_aware = module_aware,
        .configured = std::filesystem::exists(project.build_dir / "build.ninja"),
        .any_cmake_deps = any_cmake_deps,
        .wasm_toolchain_file = std::move(wasm_toolchain_file),
        .android_toolchain_file = std::move(android_toolchain_file),
        .android_abi = std::move(android_abi),
        .android_platform = std::move(android_platform),
        .android_clang_target = std::move(android_clang_target),
        .android_sysroot = std::move(android_sysroot),
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

std::string quote_command_arg(std::string_view arg) {
    if (arg.empty())
        return "\"\"";

    auto needs_quotes = false;
    for (auto ch : arg) {
        if (std::isspace(static_cast<unsigned char>(ch)) || ch == '"') {
            needs_quotes = true;
            break;
        }
    }
    if (!needs_quotes)
        return std::string{arg};

    auto escaped = std::string{};
    escaped.reserve(arg.size());
    for (auto ch : arg) {
        if (ch == '"')
            escaped += "\\\"";
        else
            escaped.push_back(ch);
    }
    return std::format("\"{}\"", escaped);
}

std::string command_text(core::ProcessSpec const& spec) {
    auto text = quote_command_arg(spec.program);
    for (auto const& arg : spec.args) {
        text.push_back(' ');
        text += quote_command_arg(arg);
    }
    return text;
}

std::string tail_excerpt(std::string_view text, std::size_t max_chars = 2000) {
    if (text.empty() || text.size() <= max_chars)
        return std::string{text};

    auto start = text.size() - max_chars;
    if (auto line_start = text.find('\n', start); line_start != std::string_view::npos &&
        line_start + 1 < text.size()) {
        start = line_start + 1;
    }
    return std::format("...\n{}", text.substr(start));
}

void print_output_block(std::string_view heading, std::string_view text) {
    if (text.empty())
        return;
    write_line(stdout);
    write_formatted_line(stdout, "---- {} ----", heading);
    write_stream(stdout, text);
    if (!text.ends_with('\n'))
        write_line(stdout);
}

void print_header(std::string_view verb, std::string_view name,
                  core::ProjectContext const& project) {
    write_formatted_line(stdout, "exon: {} {} ({})", verb, name, profile_label(project));
    write_formatted_line(stdout, "  target    {}", target_label(project));
    write_formatted_line(stdout, "  build dir {}", display_path(project.build_dir, project.root));
    write_line(stdout);
}

void print_stage(std::string_view stage) {
    if (reporting::current_stage_context.empty()) {
        write_formatted_line(stdout, "==> {}", stage);
        return;
    }
    write_formatted_line(stdout, "==> [{}] {}", reporting::current_stage_context,
                         stage);
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
                         std::chrono::milliseconds elapsed,
                         std::string_view location_label) {
    write_line(stdout);
    write_formatted_line(stdout, "exon: {} succeeded", verb);
    write_formatted_line(stdout, "  {:<9} {}", location_label,
                         display_path(artifact, project.root));
    write_formatted_line(stdout, "  elapsed   {}", reporting::format_duration(elapsed));
}

void print_build_success(std::string_view verb, std::filesystem::path const& artifact,
                         core::ProjectContext const& project,
                         std::chrono::milliseconds elapsed) {
    print_build_success(verb, artifact, project, elapsed, "artifact");
}

void print_build_finish(std::filesystem::path const& location,
                        core::ProjectContext const& project,
                        std::chrono::milliseconds elapsed,
                        std::string_view location_label) {
    print_stage("finish");
    write_formatted_line(stdout, "  {:<9} {}", location_label,
                         display_path(location, project.root));
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

void assign_test_run_summary(TestRunSummary& summary, int collected, int passed, int failed,
                             int timed_out) {
    summary.collected = collected;
    summary.passed = passed;
    summary.failed = failed;
    summary.timed_out = timed_out;
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

struct StepFailure {
    core::ProcessStep step;
    reporting::ProcessResult result;
};

struct TestRunRecord {
    std::string name;
    reporting::ProcessResult result;
};

reporting::ProcessResult run_step(core::ProcessStep const& step, reporting::StreamMode mode,
                                  std::string_view ninja_status_format = {},
                                  reporting::system::OutputObserver observer = {});
void maybe_print_windows_native_toolchain_warning(build::BuildRequest const& request);
void maybe_print_windows_native_toolchain_failure_hint(build::BuildRequest const& request,
                                                       reporting::ProcessResult const& result);

struct NinjaProgressUpdate {
    int finished = 0;
    int total = 0;
    int percent = 0;
};

constexpr auto wrapped_ninja_status_format = std::string_view{"[%f/%t] "};
constexpr auto human_ninja_status_format = std::string_view{"[%f/%t %p%%] "};

std::optional<int> parse_decimal(std::string_view text, std::size_t& pos) {
    if (pos >= text.size() ||
        !std::isdigit(static_cast<unsigned char>(text[pos]))) {
        return std::nullopt;
    }

    int value = 0;
    while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) {
        value = value * 10 + (text[pos] - '0');
        ++pos;
    }
    return value;
}

std::optional<NinjaProgressUpdate> parse_ninja_progress_prefix(std::string_view line) {
    auto pos = std::size_t{0};
    if (pos >= line.size() || line[pos] != '[')
        return std::nullopt;
    ++pos;

    auto finished = parse_decimal(line, pos);
    if (!finished || pos >= line.size() || line[pos] != '/')
        return std::nullopt;
    ++pos;

    auto total = parse_decimal(line, pos);
    if (!total || *total <= 0 || pos >= line.size() || line[pos] != ' ')
        return std::nullopt;
    ++pos;

    auto percent = parse_decimal(line, pos);
    if (!percent || *percent < 0 || *percent > 100 ||
        pos >= line.size() || line[pos] != '%') {
        return std::nullopt;
    }
    ++pos;

    if (pos >= line.size() || line[pos] != ']')
        return std::nullopt;

    return NinjaProgressUpdate{
        .finished = *finished,
        .total = *total,
        .percent = *percent,
    };
}

std::string_view strip_ninja_progress_prefix_view(std::string_view line) {
    if (!parse_ninja_progress_prefix(line))
        return line;

    auto closing = line.find(']');
    if (closing == std::string_view::npos)
        return line;

    auto rest = line.substr(closing + 1);
    if (!rest.empty() && rest.front() == ' ')
        rest.remove_prefix(1);
    return rest;
}

std::string strip_ninja_progress_prefix(std::string_view line) {
    return std::string{strip_ninja_progress_prefix_view(line)};
}

bool has_visible_text(std::string_view text) {
    return std::ranges::any_of(text, [](unsigned char ch) {
        return !std::isspace(ch);
    });
}

void append_sanitized_progress_line(std::string& out, std::string_view line, bool append_newline) {
    auto stripped = strip_ninja_progress_prefix_view(line);
    if (!has_visible_text(stripped))
        return;

    out.append(stripped);
    if (append_newline)
        out.push_back('\n');
}

std::string sanitize_ninja_progress_output(std::string_view text) {
    auto out = std::string{};
    auto current = std::string{};
    current.reserve(text.size());

    for (auto ch : text) {
        if (ch == '\n' || ch == '\r') {
            append_sanitized_progress_line(out, current, true);
            current.clear();
            continue;
        }
        current.push_back(ch);
    }

    append_sanitized_progress_line(out, current, false);
    return out;
}

class NinjaProgressTracker {
public:
    void observe(std::string_view chunk, bool stderr_stream) {
        if (stderr_stream || chunk.empty())
            return;

        auto lock = std::lock_guard{mutex_};
        for (auto ch : chunk) {
            if (ch == '\n' || ch == '\r') {
                consume_pending_locked();
                continue;
            }
            pending_.push_back(ch);
        }
    }

    void finish() {
        auto lock = std::lock_guard{mutex_};
        consume_pending_locked();
    }

    std::optional<NinjaProgressUpdate> snapshot() const {
        auto lock = std::lock_guard{mutex_};
        return latest_;
    }

private:
    void consume_pending_locked() {
        if (pending_.empty())
            return;
        if (auto parsed = parse_ninja_progress_prefix(pending_))
            latest_ = *parsed;
        pending_.clear();
    }

    mutable std::mutex mutex_;
    std::string pending_;
    std::optional<NinjaProgressUpdate> latest_;
};

reporting::ProgressSource as_progress_source(NinjaProgressTracker& tracker) {
    return reporting::ProgressSource{
        .poll = [&tracker]() -> std::optional<reporting::ProgressSnapshot> {
            auto snap = tracker.snapshot();
            if (!snap)
                return std::nullopt;
            return reporting::ProgressSnapshot{
                .done = snap->finished,
                .total = snap->total,
                .percent = snap->percent,
                .label = {},
            };
        },
    };
}

struct TestProgressCounter {
    std::atomic<int> done{0};
    int total{0};
};

reporting::ProgressSource as_progress_source(TestProgressCounter const& counter) {
    return reporting::ProgressSource{
        .poll = [&counter]() -> std::optional<reporting::ProgressSnapshot> {
            auto done = counter.done.load(std::memory_order_relaxed);
            auto total = counter.total;
            if (total <= 0)
                return reporting::ProgressSnapshot{.label = "tests"};
            auto percent = static_cast<int>(
                (static_cast<std::int64_t>(done) * 100) / total);
            return reporting::ProgressSnapshot{
                .done = done,
                .total = total,
                .percent = percent,
                .label = "tests",
            };
        },
    };
}

std::string format_test_status_cell(reporting::ProcessResult const& result,
                                    bool color_enabled) {
    auto padded = std::format("{:<7}", reporting::test_status(result));
    auto failing = result.timed_out || result.exit_code != 0;
    if (!failing || !color_enabled)
        return padded;
    return std::format("{}{}{}", reporting::ansi::red, padded,
                       reporting::ansi::reset);
}

void print_failure_excerpt(core::ProcessStep const& step,
                           reporting::ProcessResult const& result) {
    auto stderr_excerpt = tail_excerpt(sanitize_ninja_progress_output(result.stderr_text));
    auto stdout_excerpt = tail_excerpt(sanitize_ninja_progress_output(result.stdout_text));
    if (stderr_excerpt.empty() && stdout_excerpt.empty())
        return;

    write_line(stdout);
    write_line(stdout, "Captured output excerpt:");
    write_formatted_line(stdout, "  command   {}", command_text(step.spec));
    print_output_block("stderr excerpt", stderr_excerpt);
    print_output_block("stdout excerpt", stdout_excerpt);
}

void print_wrapped_rerun_hint() {
    write_line(stdout);
    write_line(stdout, "hint: rerun with --output wrapped to show full tool output");
}

std::optional<StepFailure> run_steps_human(std::vector<core::ProcessStep> const& steps,
                                           std::string_view stage,
                                           bool use_ninja_status = false) {
    print_stage(stage);
    for (auto const& step : steps) {
        auto observer = reporting::system::OutputObserver{};
        auto renderer = std::unique_ptr<reporting::system::LiveProgressRenderer>{};
        auto tracker = NinjaProgressTracker{};
        auto enable_live_progress = false;
        if (use_ninja_status) {
            renderer = reporting::system::make_live_progress_renderer();
            enable_live_progress = renderer && renderer->active();
            if (enable_live_progress) {
                observer = [&tracker](std::string_view chunk, bool stderr_stream) {
                    tracker.observe(chunk, stderr_stream);
                };
                renderer->start(as_progress_source(tracker));
            }
        }

        auto result = run_step(step, reporting::StreamMode::capture,
                               enable_live_progress ? human_ninja_status_format
                                                    : std::string_view{},
                               std::move(observer));

        if (enable_live_progress) {
            tracker.finish();
            renderer->refresh();
            renderer->stop();
        }

        if (result.exit_code != 0)
            return StepFailure{.step = step, .result = std::move(result)};
    }
    return std::nullopt;
}

std::optional<StepFailure> run_configure_steps_human(std::vector<core::ProcessStep> const& steps,
                                                     build::BuildRequest const& request) {
    print_stage("configure");
    maybe_print_windows_native_toolchain_warning(request);
    for (auto const& step : steps) {
        auto result = run_step(step, reporting::StreamMode::capture);
        if (result.exit_code != 0)
            return StepFailure{.step = step, .result = std::move(result)};
    }
    return std::nullopt;
}

int finish_human_failure(std::string_view verb, std::string_view stage,
                         build::BuildRequest const& request,
                         core::ProcessStep const& step,
                         reporting::ProcessResult const& result,
                         std::chrono::steady_clock::time_point started) {
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    print_failure_summary(verb, stage, request.project, elapsed);
    print_failure_excerpt(step, result);
    maybe_print_windows_native_toolchain_failure_hint(request, result);
    print_wrapped_rerun_hint();
    return result.exit_code;
}

void print_human_test_outputs(std::vector<TestRunRecord> const& records,
                              reporting::ShowOutput show_output) {
    auto selected = std::vector<TestRunRecord const*>{};
    for (auto const& record : records) {
        auto failed = record.result.timed_out || record.result.exit_code != 0;
        if (!reporting::should_show_output(show_output, failed))
            continue;
        if (record.result.stdout_text.empty() && record.result.stderr_text.empty())
            continue;
        selected.push_back(&record);
    }
    if (selected.empty())
        return;

    write_line(stdout);
    write_line(stdout, show_output == reporting::ShowOutput::all ? "Output:" : "Failures:");
    for (auto const* record : selected) {
        write_line(stdout);
        write_formatted_line(stdout, "{} {}", reporting::test_status(record->result), record->name);
        if (!record->result.stdout_text.empty()) {
            write_formatted_line(stdout, "---- output: {} (stdout) ----", record->name);
            write_stream(stdout, record->result.stdout_text);
            if (!record->result.stdout_text.ends_with('\n'))
                write_line(stdout);
        }
        if (!record->result.stderr_text.empty()) {
            write_formatted_line(stdout, "---- output: {} (stderr) ----", record->name);
            write_stream(stdout, record->result.stderr_text);
            if (!record->result.stderr_text.ends_with('\n'))
                write_line(stdout);
        }
    }
}

bool is_windows_native_build(build::BuildRequest const& request) {
#if defined(_WIN32)
    return request.project.target.empty() && !request.project.is_wasm;
#else
    (void)request;
    return false;
#endif
}

bool is_windows_native_clang_cl_msvc_mix(build::BuildRequest const& request) {
    return is_windows_native_build(request) &&
           request.toolchain.compiler_from_environment &&
           request.toolchain.has_msvc_developer_env &&
           request.toolchain.compiler_kind == toolchain::CompilerKind::clang_cl;
}

bool has_windows_native_module_failure_signature(reporting::ProcessResult const& result) {
    auto combined = result.stdout_text;
    if (!combined.empty() && !combined.ends_with('\n'))
        combined.push_back('\n');
    combined += result.stderr_text;
    return combined.contains(
               "compiler does not provide a way to discover the import graph dependencies") ||
           combined.contains("CMAKE_CXX_SCAN_FOR_MODULES");
}

bool should_warn_for_windows_native_toolchain(build::BuildRequest const& request) {
    return request.module_aware && is_windows_native_clang_cl_msvc_mix(request);
}

bool should_explain_windows_native_toolchain_failure(build::BuildRequest const& request,
                                                     reporting::ProcessResult const& result) {
    return is_windows_native_clang_cl_msvc_mix(request) &&
           has_windows_native_module_failure_signature(result);
}

std::string windows_native_toolchain_preflight_message() {
    return std::string{
        "note: detected a Windows native build using clang-cl from CC/CXX inside an MSVC developer environment\n"
        "note: this combination can fail during CMake configure for C++ modules or import std on Windows\n"
    };
}

std::string windows_native_toolchain_failure_message() {
    return std::string{
        "CMake reported that this compiler cannot discover C++ module import dependencies for this project.\n"
        "This usually means the environment selected clang-cl, while the repository or CI expects the MSVC cl.exe path.\n"
        "\n"
        "next steps:\n"
        "  1. inspect .intron.toml and run intron env to see which compiler was injected\n"
        "  2. confirm CC/CXX point to clang-cl or cl\n"
        "  3. if this repository expects MSVC, rerun with a cl-based environment\n"
        "     for example: $env:CC='cl'; $env:CXX='cl'; exon build\n"
        "  4. if clang-cl is intentional, verify that the current CMake/toolchain combination supports Windows C++ modules\n"
    };
}

void print_multiline_note(FILE* stream, std::string_view text) {
    write_line(stream);
    write_stream(stream, text);
    if (!text.empty() && !text.ends_with('\n'))
        write_line(stream);
}

void maybe_print_windows_native_toolchain_warning(build::BuildRequest const& request) {
    if (!should_warn_for_windows_native_toolchain(request))
        return;
    print_multiline_note(stdout, windows_native_toolchain_preflight_message());
}

void maybe_print_windows_native_toolchain_failure_hint(build::BuildRequest const& request,
                                                       reporting::ProcessResult const& result) {
    if (!should_explain_windows_native_toolchain_failure(request, result))
        return;
    print_multiline_note(stdout, windows_native_toolchain_failure_message());
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
                                  std::string_view ninja_status_format,
                                  reporting::system::OutputObserver observer) {
    auto ninja_status = std::optional<EnvVarGuard>{};
    if (!ninja_status_format.empty())
        ninja_status.emplace("NINJA_STATUS", std::string{ninja_status_format});
    return reporting::system::run_process(step.spec, mode, std::move(observer));
}

reporting::OutputMode parse_output_mode_text(std::string_view value) {
    auto parsed = value.empty() ? std::optional{reporting::OutputMode::human}
                                : reporting::parse_output_mode(value);
    if (!parsed) {
        throw std::runtime_error(
            std::format("invalid --output '{}': expected human, raw, or wrapped", value));
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
        auto result = run_step(step, reporting::StreamMode::tee,
                               use_ninja_status ? wrapped_ninja_status_format
                                                : std::string_view{});
        if (result.exit_code != 0)
            return result.exit_code;
    }
    return 0;
}

int run_configure_steps(std::vector<core::ProcessStep> const& steps,
                        build::BuildRequest const& request) {
    maybe_print_windows_native_toolchain_warning(request);
    return run_steps(steps);
}

int run_configure_steps_wrapped(std::vector<core::ProcessStep> const& steps,
                                build::BuildRequest const& request) {
    print_stage("configure");
    maybe_print_windows_native_toolchain_warning(request);
    for (auto const& step : steps) {
        auto result = run_step(step, reporting::StreamMode::tee);
        if (result.exit_code != 0) {
            maybe_print_windows_native_toolchain_failure_hint(request, result);
            return result.exit_code;
        }
    }
    return 0;
}

int run_tests(std::vector<core::ProcessStep> const& steps, TestRunSummary& summary) {
    std::println("running tests...\n");
    auto color_enabled = reporting::system::stdout_is_tty();
    int passed = 0;
    int failed = 0;
    int timed_out = 0;
    for (auto const& step : steps) {
        auto result = run_process(step.spec);
        auto const& name = step.label;
        if (result.timed_out) {
            std::println("  {} ... {}", name,
                         reporting::colorize(reporting::ansi::red, "TIMEOUT",
                                             color_enabled));
            ++timed_out;
        } else if (result.exit_code == 0) {
            std::println("  {} ... ok", name);
            ++passed;
        } else {
            std::println("  {} ... {}", name,
                         reporting::colorize(reporting::ansi::red, "FAILED",
                                             color_enabled));
            ++failed;
        }
    }

    assign_test_run_summary(summary, static_cast<int>(steps.size()), passed, failed, timed_out);
    std::println("");
    if (failed > 0 || timed_out > 0) {
        std::println("{} passed, {} failed", passed, failed + timed_out);
        return 1;
    }
    std::println("all {} tests passed", passed);
    return 0;
}

int run_tests_wrapped(std::vector<core::ProcessStep> const& steps,
                      reporting::ShowOutput show_output,
                      TestRunSummary& summary) {
    print_stage("run");
    auto color_enabled = reporting::system::stdout_is_tty();
    int passed = 0;
    int failed = 0;
    int timed_out = 0;
    for (auto const& step : steps) {
        auto result = run_step(step, reporting::StreamMode::capture);
        auto status = reporting::test_status(result);
        auto failing = result.timed_out || result.exit_code != 0;
        write_formatted_line(
            stdout, "  {} ... {}", step.label,
            failing ? reporting::colorize(reporting::ansi::red, status,
                                          color_enabled)
                    : std::string{status});
        print_captured_output(step.label, result, show_output);
        if (result.timed_out) {
            ++timed_out;
        } else if (result.exit_code == 0) {
            ++passed;
        } else {
            ++failed;
        }
    }
    assign_test_run_summary(summary, static_cast<int>(steps.size()), passed, failed, timed_out);
    return (failed == 0 && timed_out == 0) ? 0 : 1;
}

int run_build_wrapped(build::BuildRequest const& request, build::BuildPlan const& plan,
                      std::string_view verb,
                      std::chrono::steady_clock::time_point started,
                      std::optional<std::filesystem::path> success_path = std::nullopt,
                      std::string_view success_label = "artifact") {
    print_stage("sync");
    auto changed = apply_writes(plan.writes, false);
    write_formatted_line(stdout, "  {}",
                         changed ? "generated build files" : "build files up to date");

    if (changed || !plan.configured) {
        auto rc = run_configure_steps_wrapped(plan.configure_steps, request);
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

    auto location = success_path.value_or(request.project.build_dir / request.manifest.name);
    if (!success_path && !request.project.is_wasm)
        location += toolchain::exe_suffix;
    print_build_success(verb, location, request.project, elapsed, success_label);
    return 0;
}

int run_build_human(build::BuildRequest const& request, build::BuildPlan const& plan,
                    std::string_view verb,
                    std::chrono::steady_clock::time_point started,
                    std::optional<std::filesystem::path> success_path = std::nullopt,
                    std::string_view success_label = "artifact") {
    print_stage("sync");
    auto changed = apply_writes(plan.writes, false);
    write_formatted_line(stdout, "  {}",
                         changed ? "generated build files" : "build files up to date");

    if (changed || !plan.configured) {
        if (auto failure = run_configure_steps_human(plan.configure_steps, request))
            return finish_human_failure(verb, "configure", request, failure->step,
                                        failure->result, started);
    } else {
        print_stage("configure");
        write_line(stdout, "  cached");
    }

    if (auto failure = run_steps_human(plan.build_steps, "build", true))
        return finish_human_failure(verb, "build", request, failure->step,
                                    failure->result, started);

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    auto location = success_path.value_or(request.project.build_dir / request.manifest.name);
    if (!success_path && !request.project.is_wasm)
        location += toolchain::exe_suffix;
    print_build_finish(location, request.project, elapsed, success_label);
    return 0;
}

int run_test_wrapped(build::BuildRequest const& request, build::BuildPlan const& plan,
                     reporting::Options const& options,
                     std::chrono::steady_clock::time_point started,
                     TestRunSummary& summary) {
    print_stage("sync");
    auto changed = apply_writes(plan.writes, false);
    write_formatted_line(stdout, "  {}", changed ? "generated build files" : "build files up to date");

    if (changed || !plan.configured) {
        auto rc = run_configure_steps_wrapped(plan.configure_steps, request);
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

    write_formatted_line(stdout, "  collected {} test binaries", plan.run_steps.size());
    auto run_rc = run_tests_wrapped(plan.run_steps, options.show_output, summary);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    print_test_summary(summary.collected, summary.passed, summary.failed, summary.timed_out,
                       request.project, elapsed);
    return run_rc;
}

int run_test_human(build::BuildRequest const& request, build::BuildPlan const& plan,
                   reporting::Options const& options,
                   std::chrono::steady_clock::time_point started,
                   TestRunSummary& summary) {
    print_stage("sync");
    auto changed = apply_writes(plan.writes, false);
    write_formatted_line(stdout, "  {}",
                         changed ? "generated build files" : "build files up to date");

    if (changed || !plan.configured) {
        if (auto failure = run_configure_steps_human(plan.configure_steps, request))
            return finish_human_failure("test", "configure", request, failure->step,
                                        failure->result, started);
    } else {
        print_stage("configure");
        write_line(stdout, "  cached");
    }

    if (auto failure = run_steps_human(plan.build_steps, "build", true))
        return finish_human_failure("test", "build", request, failure->step,
                                    failure->result, started);

    auto records = std::vector<TestRunRecord>{};
    int passed = 0;
    int failed = 0;
    int timed_out = 0;

    print_stage("run");
    write_formatted_line(stdout, "  collected {} test binaries", plan.run_steps.size());

    auto color_enabled = reporting::system::stdout_is_tty();
    auto counter = TestProgressCounter{};
    counter.total = static_cast<int>(plan.run_steps.size());
    auto renderer = reporting::system::make_live_progress_renderer();
    auto live = renderer && renderer->active();
    if (live)
        renderer->start(as_progress_source(counter));

    for (auto const& step : plan.run_steps) {
        auto result = run_step(step, reporting::StreamMode::capture);
        if (live)
            renderer->stop();
        write_formatted_line(stdout, "  {} {}",
                             format_test_status_cell(result, color_enabled),
                             step.label);
        if (result.timed_out) {
            ++timed_out;
        } else if (result.exit_code == 0) {
            ++passed;
        } else {
            ++failed;
        }
        records.push_back({
            .name = step.label,
            .result = std::move(result),
        });
        counter.done.fetch_add(1, std::memory_order_relaxed);
        if (live)
            renderer->start(as_progress_source(counter));
    }

    if (live)
        renderer->stop();

    print_human_test_outputs(records, options.show_output);
    assign_test_run_summary(summary, static_cast<int>(plan.run_steps.size()), passed, failed,
                            timed_out);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    print_test_summary(summary.collected, summary.passed, summary.failed, summary.timed_out,
                       request.project, elapsed);
    return (failed == 0 && timed_out == 0) ? 0 : 1;
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
            if (options.output == reporting::OutputMode::human)
                return detail::run_build_human(request, plan, "build", started);
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
        auto rc = detail::run_configure_steps(plan.configure_steps, request);
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
            if (options.output == reporting::OutputMode::human)
                return detail::run_build_human(request, plan, "build", started);
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
        auto rc = detail::run_configure_steps(plan.configure_steps, request);
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
        auto rc = detail::run_configure_steps(plan.configure_steps, request);
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
        auto rc = detail::run_configure_steps(plan.configure_steps, request);
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
    TestRunSummary summary;
    return run_test(project_root, m, release, target, filter, timeout, options, summary);
}

inline int run_test(std::filesystem::path const& project_root,
                    manifest::Manifest const& m, bool release,
                    std::string_view target, std::string_view filter,
                    std::optional<std::chrono::milliseconds> timeout,
                    reporting::Options const& options,
                    TestRunSummary& summary) {
    if (options.output != reporting::OutputMode::raw) {
        auto started = std::chrono::steady_clock::now();
        auto project = build::project_context(project_root, release, target);
        detail::print_header("test", m.name, project);
        detail::print_stage("resolve");
        try {
            auto request = detail::prepare_request(project_root, m, release, target, true, filter,
                                                   timeout);
            auto plan = build::plan_test(request);
            if (options.output == reporting::OutputMode::human)
                return detail::run_test_human(request, plan, options, started, summary);
            return detail::run_test_wrapped(request, plan, options, started, summary);
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
        auto rc = detail::run_configure_steps(plan.configure_steps, request);
        if (rc != 0)
            return rc;
    }

    auto rc = detail::run_steps(plan.build_steps);
    if (rc != 0)
        return rc;

    return detail::run_tests(plan.run_steps, summary);
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
    TestRunSummary summary;
    return run_test(project_root, m, portable_manifest, release, target, filter, timeout,
                    options, summary);
}

inline int run_test(std::filesystem::path const& project_root,
                    manifest::Manifest const& m,
                    manifest::Manifest const& portable_manifest, bool release,
                    std::string_view target, std::string_view filter,
                    std::optional<std::chrono::milliseconds> timeout,
                    reporting::Options const& options,
                    TestRunSummary& summary) {
    if (options.output != reporting::OutputMode::raw) {
        auto started = std::chrono::steady_clock::now();
        auto project = build::project_context(project_root, release, target);
        detail::print_header("test", m.name, project);
        detail::print_stage("resolve");
        try {
            auto request = detail::prepare_request(project_root, m, portable_manifest, release,
                                                   target, true, filter, timeout);
            auto plan = build::plan_test(request);
            if (options.output == reporting::OutputMode::human)
                return detail::run_test_human(request, plan, options, started, summary);
            return detail::run_test_wrapped(request, plan, options, started, summary);
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
        auto rc = detail::run_configure_steps(plan.configure_steps, request);
        if (rc != 0)
            return rc;
    }

    auto rc = detail::run_steps(plan.build_steps);
    if (rc != 0)
        return rc;

    return detail::run_tests(plan.run_steps, summary);
}

} // namespace build::system
