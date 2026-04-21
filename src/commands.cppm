export module commands;
import std;
import cli;
import core;
import cppx.fs;
import cppx.fs.system;
import toml;
import manifest;
import manifest.system;
import build;
import build.system;
import debug;
import debug.system;
import fetch;
import fetch.system;
import lock;
import lock.system;
import reporting;
import templates;
import toolchain;
import toolchain.system;

export namespace commands {

#ifndef EXON_PKG_VERSION
#define EXON_PKG_VERSION "dev"
#endif
constexpr auto version = EXON_PKG_VERSION;

int command_error(std::string_view msg) {
    std::println(std::cerr, "error: {}", msg);
    return 1;
}

int command_error(std::string_view msg, std::string_view hint) {
    std::println(std::cerr, "error: {}", msg);
    std::println(std::cerr, "hint: {}", hint);
    return 1;
}

std::string usage_text() {
    return cli::usage("exon", {
        cli::Section{"commands", {
            {"init [--lib|--workspace] [name]",    "create a new package or workspace"},
            {"new --lib|--bin <name>",             "create a new workspace member"},
            {"info",                               "show package information"},
            {"build [--release] [--target <t>] [--member a,b] [--exclude x,y] "
             "[--output human|wrapped|raw]",
                                                   "build the project"},
            {"check [--release] [--target <t>] [--member a,b] [--exclude x,y]",
                                                   "check syntax without linking"},
            {"run [--release] [--target <t>] [--member <name>] [args]",
                                                   "build and run the project"},
            {"debug [--release] [--debugger auto|lldb|gdb|devenv|cdb|<path>] "
             "[--member <name>] [--exclude x,y] [-- <args...>]",
                                                   "build and open the selected native executable in a native debugger"},
            {"test [--release] [--target <t>] [--member a,b] [--exclude x,y] [--timeout <sec>] "
             "[--output human|wrapped|raw] [--show-output failed|all|none]",
                                                   "build and run tests"},
            {"clean [--member a,b] [--exclude x,y]","remove build artifacts"},
            {"add [--dev] <pkg> <ver>",            "add a git dependency"},
            {"add [--dev] --path <name> <path>",   "add a local path dependency"},
            {"add [--dev] --workspace <name>",     "add a workspace member dependency"},
            {"add [--dev] --vcpkg <name> <ver> [--features a,b,c]",
                                                   "add a vcpkg dependency"},
            {"add [--dev] --git <repo> --version <ver> --subdir <dir> [--name <n>]",
                                                   "add a git dep pointing to a subdirectory"},
            {"remove <pkg>",                       "remove a dependency"},
            {"update [--member a,b] [--exclude x,y]",
                                                   "update dependencies to latest compatible versions"},
            {"sync [--member a,b] [--exclude x,y]","sync CMakeLists.txt with exon.toml"},
            {"fmt",                                "format source files with clang-format"},
            {"version",                            "show exon version"},
        }},
        cli::Section{"targets", {
            {"wasm32-wasi", "WebAssembly (requires wasi-sdk via intron or WASI_SDK_PATH)"},
            {"aarch64-linux-android", "Android arm64 (requires android-ndk via intron or ANDROID_NDK_HOME)"},
        }},
    });
}

manifest::Manifest load_manifest(std::filesystem::path const& project_root) {
    auto manifest_path = project_root / "exon.toml";
    if (!std::filesystem::exists(manifest_path)) {
        throw std::runtime_error("exon.toml not found. run 'exon init' first");
    }
    return manifest::system::load(manifest_path.string());
}

manifest::Manifest load_manifest() {
    return load_manifest(std::filesystem::current_path());
}

toolchain::Platform effective_platform(std::string_view target);
manifest::Manifest resolve_manifest(manifest::Manifest m, std::string_view target = {});
bool check_platform(manifest::Manifest const& m, std::string_view target = {});
std::string read_text(std::filesystem::path const& path);
void write_text(std::filesystem::path const& path, std::string const& content);

struct WorkspaceMember {
    std::string name;
    std::filesystem::path path;
    manifest::Manifest raw_manifest;
    manifest::Manifest resolved_manifest;
    std::set<std::string> workspace_dep_names;
    std::set<std::string> dev_workspace_dep_names;
    bool runnable = false;
};

struct WorkspaceSelection {
    std::vector<WorkspaceMember> members;
    bool explicitly_selected = false;
};

std::vector<cli::ArgDef> workspace_select_defs(std::vector<cli::ArgDef> defs = {}) {
    defs.push_back(cli::ListOption{"--member"});
    defs.push_back(cli::ListOption{"--exclude"});
    return defs;
}

std::filesystem::path workspace_build_root(std::filesystem::path const& workspace_root,
                                           bool release, std::string_view target = {}) {
    auto profile = release ? "release" : "debug";
    auto base = workspace_root / ".exon" / "workspace";
    if (!target.empty())
        base /= target;
    return base / profile;
}

std::string sanitize_workspace_name(std::string_view name) {
    auto value = std::string{name};
    if (value.empty())
        value = "workspace";
    for (auto& ch : value) {
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'))
            ch = '_';
    }
    if (!std::isalpha(static_cast<unsigned char>(value.front())) && value.front() != '_')
        value.insert(value.begin(), '_');
    return value;
}

std::string quote_cmake_path(std::filesystem::path const& path) {
    return std::format("\"{}\"", path.generic_string());
}

std::string workspace_display_name(WorkspaceMember const& member,
                                   std::filesystem::path const& workspace_root) {
    auto rel = std::filesystem::relative(member.path, workspace_root);
    return std::format("{} ({})", member.name, rel.generic_string());
}

bool manifest_has_any_dependencies(manifest::Manifest const& m) {
    return !m.dependencies.empty() || !m.path_deps.empty() || !m.workspace_deps.empty() ||
           !m.vcpkg_deps.empty() || !m.subdir_deps.empty() || !m.featured_deps.empty() ||
           !m.cmake_deps.empty() || !m.dev_dependencies.empty() || !m.dev_path_deps.empty() ||
           !m.dev_workspace_deps.empty() || !m.dev_vcpkg_deps.empty() ||
           !m.dev_subdir_deps.empty() || !m.dev_featured_deps.empty() ||
           !m.dev_cmake_deps.empty() || !m.find_deps.empty() || !m.dev_find_deps.empty();
}

WorkspaceMember load_workspace_member(std::filesystem::path const& workspace_root,
                                      manifest::Manifest const& workspace_manifest,
                                      std::filesystem::path const& member_path,
                                      std::string_view target) {
    auto raw_member = manifest::apply_workspace_defaults(load_manifest(member_path), workspace_manifest);
    if (!check_platform(raw_member, target))
        throw std::runtime_error(std::format(
            "workspace member '{}' does not support target '{}'",
            raw_member.name.empty() ? member_path.filename().string() : raw_member.name,
            target.empty() ? toolchain::detect_host_platform().to_string() : std::string{target}));
    auto resolved_member = resolve_manifest(raw_member, target);
    auto workspace_dep_names = resolved_member.workspace_deps;
    auto dev_workspace_dep_names = resolved_member.dev_workspace_deps;
    auto runnable = resolved_member.type != "lib";
    return {
        .name = resolved_member.name,
        .path = member_path,
        .raw_manifest = std::move(raw_member),
        .resolved_manifest = std::move(resolved_member),
        .workspace_dep_names = std::move(workspace_dep_names),
        .dev_workspace_dep_names = std::move(dev_workspace_dep_names),
        .runnable = runnable,
    };
}

std::vector<WorkspaceMember> load_workspace_members(std::filesystem::path const& workspace_root,
                                                    manifest::Manifest const& workspace_manifest,
                                                    std::string_view target) {
    auto members = std::vector<WorkspaceMember>{};
    auto seen_names = std::set<std::string>{};
    for (auto const& member : workspace_manifest.workspace_members) {
        auto member_path = workspace_root / member;
        if (!std::filesystem::exists(member_path / "exon.toml"))
            throw std::runtime_error(std::format("{} has no exon.toml", member));
        auto info = load_workspace_member(workspace_root, workspace_manifest, member_path, target);
        if (info.name.empty())
            throw std::runtime_error(std::format(
                "workspace member '{}' is missing [package].name", member_path.string()));
        if (!seen_names.insert(info.name).second)
            throw std::runtime_error(std::format(
                "duplicate workspace member package name '{}'", info.name));
        members.push_back(std::move(info));
    }
    return members;
}

WorkspaceSelection select_workspace_members(std::filesystem::path const& workspace_root,
                                            manifest::Manifest const& workspace_manifest,
                                            std::string_view target,
                                            std::vector<std::string> const& requested_members,
                                            std::vector<std::string> const& excluded_members,
                                            bool include_dependencies = false,
                                            bool include_dev_workspace = false) {
    auto members = load_workspace_members(workspace_root, workspace_manifest, target);
    auto by_name = std::map<std::string, WorkspaceMember const*>{};
    for (auto const& member : members)
        by_name.emplace(member.name, &member);

    auto excluded = std::set<std::string>{excluded_members.begin(), excluded_members.end()};
    for (auto const& name : excluded) {
        if (!by_name.contains(name))
            throw std::runtime_error(std::format("workspace member '{}' not found", name));
    }

    auto requested = std::vector<std::string>{};
    if (!requested_members.empty()) {
        for (auto const& name : requested_members) {
            if (!by_name.contains(name))
                throw std::runtime_error(std::format("workspace member '{}' not found", name));
            requested.push_back(name);
        }
    } else {
        for (auto const& member : members) {
            if (!excluded.contains(member.name))
                requested.push_back(member.name);
        }
    }

    auto selected_names = std::set<std::string>{};
    auto add_with_dependencies = [&](this auto const& self, std::string const& name) -> void {
        if (excluded.contains(name))
            throw std::runtime_error(std::format(
                "workspace member '{}' is excluded but required by the current selection", name));
        if (!selected_names.insert(name).second)
            return;
        auto const& member = *by_name.at(name);
        for (auto const& dep : member.workspace_dep_names)
            self(dep);
        if (include_dev_workspace) {
            for (auto const& dep : member.dev_workspace_dep_names)
                self(dep);
        }
    };

    if (include_dependencies) {
        for (auto const& name : requested)
            add_with_dependencies(name);
    } else {
        selected_names.insert(requested.begin(), requested.end());
    }

    for (auto const& name : excluded)
        selected_names.erase(name);
    if (selected_names.empty())
        throw std::runtime_error("no workspace members selected");

    auto ordered_names = std::vector<std::string>{};
    auto states = std::map<std::string, int>{};
    auto visit = [&](this auto const& self, std::string const& name) -> void {
        auto state = states[name];
        if (state == 2)
            return;
        if (state == 1)
            throw std::runtime_error(std::format(
                "workspace dependency cycle detected at '{}'", name));
        states[name] = 1;
        auto const& member = *by_name.at(name);
        for (auto const& dep : member.workspace_dep_names) {
            if (selected_names.contains(dep))
                self(dep);
        }
        if (include_dev_workspace) {
            for (auto const& dep : member.dev_workspace_dep_names) {
                if (selected_names.contains(dep))
                    self(dep);
            }
        }
        states[name] = 2;
        ordered_names.push_back(name);
    };
    for (auto const& name : requested) {
        if (selected_names.contains(name))
            visit(name);
    }
    for (auto const& name : selected_names) {
        if (!states.contains(name))
            visit(name);
    }

    WorkspaceSelection selection{
        .explicitly_selected = !requested_members.empty(),
    };
    for (auto const& name : ordered_names) {
        auto it = std::ranges::find_if(members, [&](WorkspaceMember const& member) {
            return member.name == name;
        });
        if (it != members.end())
            selection.members.push_back(*it);
    }
    return selection;
}

int run_selected_workspace_members(std::filesystem::path const& workspace_root,
                                   WorkspaceSelection const& selection,
                                   std::function<int(WorkspaceMember const&)> fn) {
    for (auto const& member : selection.members) {
        std::println("--- {} ---", workspace_display_name(member, workspace_root));
        auto rc = fn(member);
        if (rc != 0)
            return rc;
    }
    return 0;
}

void update_workspace_members_list(std::filesystem::path const& workspace_root,
                                   std::string_view member_name) {
    auto manifest_path = workspace_root / "exon.toml";
    auto content = read_text(manifest_path);
    auto workspace_manifest = load_manifest(workspace_root);
    auto member_text = std::string{member_name};
    if (std::ranges::find(workspace_manifest.workspace_members, member_text) !=
        workspace_manifest.workspace_members.end()) {
        return;
    }

    auto marker = std::string{"members = ["};
    auto pos = content.find(marker);
    if (pos == std::string::npos) {
        if (!content.empty() && content.back() != '\n')
            content += '\n';
        content += std::format("\n[workspace]\nmembers = [\"{}\"]\n", member_name);
        write_text(manifest_path, content);
        return;
    }

    auto insert_pos = content.find(']', pos);
    if (insert_pos == std::string::npos)
        throw std::runtime_error("workspace members list is malformed");
    auto before = content.substr(pos, insert_pos - pos);
    auto needs_separator = before.find('"') != std::string::npos;
    content.insert(insert_pos, std::format("{}\"{}\"", needs_separator ? ", " : "", member_name));
    write_text(manifest_path, content);
}

void create_package_files(std::filesystem::path const& target_dir, std::string const& name,
                          bool is_lib) {
    auto manifest_path = target_dir / "exon.toml";
    if (std::filesystem::exists(manifest_path))
        throw std::runtime_error(std::format("{} already exists", manifest_path.string()));

    std::filesystem::create_directories(target_dir / "src");

    auto tmpl = std::string{is_lib ? templates::exon_toml_lib : templates::exon_toml_bin};
    auto name_pos = tmpl.find("name = \"\"");
    if (name_pos != std::string::npos)
        tmpl.replace(name_pos, 9, std::format("name = \"{}\"", name));
    write_text(manifest_path, tmpl);

    auto source_path = target_dir / "src" / (is_lib ? std::format("{}.cppm", name) : "main.cpp");
    auto source = is_lib
        ? std::format("export module {};\nimport std;\n\n", name)
        : "import std;\n\nint main() {\n    std::println(\"hello, world!\");\n    return 0;\n}\n";
    write_text(source_path, source);
}

std::string generate_workspace_root_cmake(std::filesystem::path const& workspace_root,
                                          std::filesystem::path const& cmake_root,
                                          std::vector<WorkspaceMember> const& members) {
    auto out = std::ostringstream{};
    auto standard = members.empty() ? 23 : 0;
    for (auto const& member : members)
        standard = std::max(standard, member.resolved_manifest.standard);
    auto project_name = sanitize_workspace_name(workspace_root.filename().string());
    auto import_std = standard >= 23;

    out << "# Generated by exon. Do not edit manually.\n\n";
    if (import_std) {
        out << "cmake_minimum_required(VERSION 3.30)\n\n";
        out << std::format("set(CMAKE_CXX_STANDARD {})\n", standard);
        out << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n";
        out << "set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "
               "\"451f2fe2-a8a2-47c3-bc32-94786d8fc91b\")\n";
        out << "set(CMAKE_CXX_MODULE_STD ON)\n";
        out << std::format("project({} LANGUAGES CXX)\n\n", project_name);
    } else {
        out << "cmake_minimum_required(VERSION 3.28)\n";
        out << std::format("project({} LANGUAGES CXX)\n\n", project_name);
        out << std::format("set(CMAKE_CXX_STANDARD {})\n", standard);
        out << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n\n";
    }
    out << "set(CMAKE_EXPORT_COMPILE_COMMANDS ON)\n\n";
    out << "if(MSVC)\n";
    out << "    add_compile_options(/W4 /wd4996 /utf-8)\n";
    out << "    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)\n";
    out << "endif()\n\n";
    out << "set(EXON_ENABLE_TEST_TARGETS OFF)\n\n";
    for (auto const& member : members) {
        auto rel = std::filesystem::relative(member.path, cmake_root).generic_string();
        out << std::format("add_subdirectory({} ${{CMAKE_BINARY_DIR}}/members/{})\n",
                           quote_cmake_path(rel),
                           sanitize_workspace_name(member.name));
    }
    return out.str();
}

manifest::Manifest workspace_build_manifest(std::filesystem::path const& workspace_root,
                                            std::vector<WorkspaceMember> const& members) {
    auto manifest = manifest::Manifest{};
    manifest.name = sanitize_workspace_name(workspace_root.filename().string());
    manifest.type = "lib";
    manifest.standard = members.empty() ? 23 : 0;
    manifest.has_standard = true;
    for (auto const& member : members)
        manifest.standard = std::max(manifest.standard, member.resolved_manifest.standard);
    return manifest;
}

reporting::Options parse_reporting_options(std::string_view output_mode_text = {},
                                           std::string_view show_output_text = {}) {
    return {
        .output = build::system::detail::parse_output_mode_text(output_mode_text),
        .show_output = build::system::detail::parse_show_output_text(show_output_text),
    };
}

int sync_workspace_member_cmake(WorkspaceMember const& member,
                                std::optional<toolchain::Platform> platform = std::nullopt,
                                bool all_targets = false) {
    auto fetch_manifest = all_targets
        ? manifest::resolve_all_targets(member.raw_manifest)
        : member.resolved_manifest;
    auto fetch_result = fetch::system::fetch_all({
        .manifest = fetch_manifest,
        .project_root = member.path,
        .lock_path = member.path / "exon.lock",
        .platform = all_targets ? std::nullopt : platform,
    });
    write_text(member.path / "CMakeLists.txt",
               build::generate_portable_cmake(member.raw_manifest, member.path, fetch_result.deps));
    std::println("synced CMakeLists.txt");
    return 0;
}

int sync_workspace_root_cmake(std::filesystem::path const& workspace_root,
                              manifest::Manifest const& workspace_manifest,
                              WorkspaceSelection const& selection) {
    if (!workspace_manifest.sync_cmake_in_root)
        return 0;
    write_text(workspace_root / "CMakeLists.txt",
               generate_workspace_root_cmake(workspace_root, workspace_root, selection.members));
    std::println("synced CMakeLists.txt");
    return 0;
}

core::FileWrite workspace_member_cmake_write(WorkspaceMember const& member,
                                             toolchain::Platform const& platform) {
    auto fetch_result = fetch::system::fetch_all({
        .manifest = member.resolved_manifest,
        .project_root = member.path,
        .lock_path = member.path / "exon.lock",
        .platform = platform,
    });
    return {
        .text = {
            .path = member.path / "CMakeLists.txt",
            .content = build::generate_portable_cmake(member.raw_manifest, member.path,
                                                      fetch_result.deps),
        },
        .success_message = "synced CMakeLists.txt",
    };
}

std::optional<core::FileWrite> workspace_root_cmake_write(
    std::filesystem::path const& workspace_root,
    manifest::Manifest const& workspace_manifest,
    WorkspaceSelection const& selection) {
    if (!workspace_manifest.sync_cmake_in_root)
        return std::nullopt;
    return core::FileWrite{
        .text = {
            .path = workspace_root / "CMakeLists.txt",
            .content = generate_workspace_root_cmake(workspace_root, workspace_root,
                                                     selection.members),
        },
        .success_message = "synced CMakeLists.txt",
    };
}

struct WorkspaceBuildExecution {
    build::BuildRequest request;
    build::BuildPlan plan;
    std::filesystem::path success_path;
    std::string success_label;
};

WorkspaceBuildExecution prepare_workspace_build_execution(
    std::filesystem::path const& workspace_root,
    manifest::Manifest const& workspace_manifest,
    WorkspaceSelection const& selection,
    bool release, std::string_view target) {
    auto platform = target.empty()
        ? toolchain::detect_host_platform()
        : *toolchain::platform_from_target(target);

    auto cmake_root = workspace_root / ".exon";
    std::filesystem::create_directories(cmake_root);

    auto synthetic_manifest = workspace_build_manifest(workspace_root, selection.members);
    auto tc = toolchain::system::detect();
    std::string wasm_toolchain_file;
    std::string android_toolchain_file;
    std::string android_abi;
    std::string android_platform;
    bool is_wasm = !target.empty() && target.starts_with("wasm32");
    bool is_android = !target.empty() && target.ends_with("-linux-android");
    if (is_wasm) {
        auto wasm_tc = toolchain::system::detect_wasm(target);
        wasm_toolchain_file = wasm_tc.cmake_toolchain;
        tc.stdlib_modules_json = wasm_tc.modules_json;
        tc.cxx_compiler = wasm_tc.scan_deps;
        tc.sysroot.clear();
        tc.lib_dir.clear();
        tc.has_clang_config = false;
        tc.needs_stdlib_flag = false;
    } else if (is_android) {
        auto android_tc = toolchain::system::detect_android(target);
        android_toolchain_file = android_tc.cmake_toolchain;
        android_abi = android_tc.abi;
        android_platform = android_tc.platform;
        tc.stdlib_modules_json = android_tc.modules_json;
        tc.cxx_compiler = android_tc.scan_deps;
        tc.sysroot.clear();
        tc.lib_dir.clear();
        tc.has_clang_config = false;
        tc.needs_stdlib_flag = false;
    }

    auto build_dir = workspace_build_root(workspace_root, release, target);
    build::BuildRequest request{
        .project = {
            .root = workspace_root,
            .exon_dir = cmake_root,
            .build_dir = build_dir,
            .profile = release ? "release" : "debug",
            .target = std::string{target},
            .is_wasm = !target.empty(),
        },
        .manifest = synthetic_manifest,
        .toolchain = tc,
        .release = release,
        .configured = std::filesystem::exists(build_dir / "build.ninja"),
        .wasm_toolchain_file = wasm_toolchain_file,
        .android_toolchain_file = android_toolchain_file,
        .android_abi = android_abi,
        .android_platform = android_platform,
    };

    build::BuildPlan plan{
        .project = request.project,
        .configured = request.configured,
    };
    for (auto const& member : selection.members)
        plan.writes.push_back(workspace_member_cmake_write(member, platform));
    if (auto root_write = workspace_root_cmake_write(workspace_root, workspace_manifest, selection))
        plan.writes.push_back(*root_write);
    plan.writes.push_back({
        .text = {
            .path = cmake_root / "CMakeLists.txt",
            .content = generate_workspace_root_cmake(workspace_root, cmake_root,
                                                     selection.members),
        },
    });

    auto configure_spec = build::configure_command(
        tc, synthetic_manifest, build_dir, cmake_root, release, {}, {},
        wasm_toolchain_file, android_toolchain_file, android_abi, android_platform, false);
    configure_spec.cwd = workspace_root;
    plan.configure_steps.push_back({
        .spec = std::move(configure_spec),
        .label = "configuring workspace...",
    });

    auto build_spec = build::build_command(tc, synthetic_manifest, build_dir, {}, target);
    build_spec.cwd = workspace_root;
    plan.build_steps.push_back({
        .spec = std::move(build_spec),
        .label = "building workspace...",
    });

    return {
        .request = std::move(request),
        .plan = std::move(plan),
        .success_path = build_dir,
        .success_label = "build dir",
    };
}

int run_workspace_build(std::filesystem::path const& workspace_root,
                        manifest::Manifest const& workspace_manifest,
                        WorkspaceSelection const& selection,
                        bool release, std::string_view target,
                        reporting::Options const& options) {
    if (options.output != reporting::OutputMode::raw) {
        auto started = std::chrono::steady_clock::now();
        auto project = core::ProjectContext{
            .root = workspace_root,
            .exon_dir = workspace_root / ".exon",
            .build_dir = workspace_build_root(workspace_root, release, target),
            .profile = release ? "release" : "debug",
            .target = std::string{target},
            .is_wasm = !target.empty(),
        };
        auto workspace_name = workspace_build_manifest(workspace_root, selection.members).name;
        build::system::detail::print_header("build", workspace_name, project);
        build::system::detail::print_stage("resolve");
        try {
            auto execution = prepare_workspace_build_execution(
                workspace_root, workspace_manifest, selection, release, target);
            if (options.output == reporting::OutputMode::human) {
                return build::system::detail::run_build_human(
                    execution.request, execution.plan, "build", started,
                    execution.success_path, execution.success_label);
            }
            return build::system::detail::run_build_wrapped(
                execution.request, execution.plan, "build", started,
                execution.success_path, execution.success_label);
        } catch (...) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            build::system::detail::print_failure_summary("build", "resolve", project, elapsed);
            throw;
        }
    }

    auto execution = prepare_workspace_build_execution(
        workspace_root, workspace_manifest, selection, release, target);
    auto changed = build::system::detail::apply_writes(execution.plan.writes);
    if (changed || !execution.plan.configured) {
        auto rc = build::system::detail::run_configure_steps(execution.plan.configure_steps,
                                                             execution.request);
        if (rc != 0)
            return rc;
    }

    auto rc = build::system::detail::run_steps(execution.plan.build_steps);
    if (rc != 0)
        return rc;

    auto rel = std::filesystem::relative(execution.success_path, workspace_root).generic_string();
    std::println("build succeeded: {}", rel);
    return 0;
}

toolchain::Platform effective_platform(std::string_view target) {
    if (!target.empty()) {
        auto p = toolchain::platform_from_target(target);
        if (!p)
            throw std::runtime_error(std::format("unknown target '{}'", target));
        return *p;
    }
    return toolchain::detect_host_platform();
}

manifest::Manifest resolve_manifest(manifest::Manifest m, std::string_view target) {
    return manifest::resolve_for_platform(std::move(m), effective_platform(target));
}

bool check_platform(manifest::Manifest const& m, std::string_view target) {
    auto plat = effective_platform(target);
    if (!manifest::supports_platform(m, plat)) {
        std::println(std::cerr,
                     "error: platform ({}) is not supported by this package",
                     plat.to_string());
        std::println(std::cerr, "  supported platforms:");
        for (auto const& p : m.platforms) {
            if (toolchain::platform_has_os(p) && toolchain::platform_has_arch(p))
                std::println(std::cerr, "    {{ os = \"{}\", arch = \"{}\" }}",
                             toolchain::platform_os_name(p),
                             toolchain::platform_arch_name(p));
            else if (toolchain::platform_has_os(p))
                std::println(std::cerr, "    {{ os = \"{}\" }}",
                             toolchain::platform_os_name(p));
            else
                std::println(std::cerr, "    {{ arch = \"{}\" }}",
                             toolchain::platform_arch_name(p));
        }
        return false;
    }
    return true;
}

std::vector<cli::ArgDef> const build_defs = {
    cli::Flag{"--release"},
    cli::Option{"--target"},
};

struct RunnableCommandTarget {
    std::filesystem::path root;
    manifest::Manifest raw_manifest;
    manifest::Manifest resolved_manifest;
};

std::expected<RunnableCommandTarget, std::string>
resolve_runnable_command_target(std::filesystem::path const& project_root,
                                manifest::Manifest const& raw_m,
                                std::string_view target,
                                std::vector<std::string> const& requested_members,
                                std::vector<std::string> const& excluded_members) {
    auto m = resolve_manifest(raw_m, target);
    auto member_root = project_root;
    auto raw_target_manifest = raw_m;
    auto target_manifest = m;
    if (manifest::is_workspace(raw_m)) {
        auto selection = select_workspace_members(project_root, raw_m, target,
                                                  requested_members, excluded_members,
                                                  false, false);
        auto runnable = std::vector<WorkspaceMember>{};
        for (auto const& member : selection.members) {
            if (member.runnable)
                runnable.push_back(member);
        }
        if (requested_members.empty()) {
            if (runnable.size() != 1) {
                return std::unexpected{
                    "workspace execution requires exactly one runnable member; pass --member <name>"};
            }
        } else {
            if (selection.members.size() != 1) {
                return std::unexpected{
                    "--member must select exactly one workspace member"};
            }
            if (!selection.members.front().runnable)
                return std::unexpected{"cannot run a library package"};
            runnable = {selection.members.front()};
        }
        auto const& member = runnable.front();
        member_root = member.path;
        raw_target_manifest = member.raw_manifest;
        target_manifest = member.resolved_manifest;
    } else if (!requested_members.empty() || !excluded_members.empty()) {
        return std::unexpected{"--member and --exclude require a workspace root"};
    }

    if (target_manifest.name.empty())
        return std::unexpected{"package name is required in exon.toml"};
    if (target_manifest.type == "lib")
        return std::unexpected{"cannot run a library package"};

    return RunnableCommandTarget{
        .root = member_root,
        .raw_manifest = raw_target_manifest,
        .resolved_manifest = target_manifest,
    };
}

std::string read_text(std::filesystem::path const& path) {
    auto text = cppx::fs::system::read_text(path);
    if (!text)
        throw std::runtime_error(std::format(
            "failed to read {} ({})", path.string(), cppx::fs::to_string(text.error())));
    return *text;
}

void write_text(std::filesystem::path const& path, std::string const& content) {
    auto result = cppx::fs::system::write_if_changed({
        .path = path,
        .content = content,
        .skip_if_unchanged = false,
    });
    if (!result) {
        throw std::runtime_error(std::format(
            "failed to write {} ({})", path.string(), cppx::fs::to_string(result.error())));
    }
}

std::optional<std::chrono::milliseconds> parse_timeout(std::string_view value) {
    if (value.empty())
        return std::nullopt;

    long long seconds = 0;
    auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), seconds);
    if (ec != std::errc{} || ptr != value.data() + value.size() || seconds <= 0) {
        throw std::runtime_error(
            std::format("invalid --timeout '{}': expected a positive integer in seconds", value));
    }

    constexpr auto max_ms = std::numeric_limits<std::chrono::milliseconds::rep>::max();
    if (seconds > max_ms / 1000) {
        throw std::runtime_error(
            std::format("invalid --timeout '{}': value is too large", value));
    }
    return std::chrono::milliseconds{seconds * 1000};
}

int cmd_version() {
    std::println("exon {}", version);
    return 0;
}

int cmd_init(int argc, char* argv[]) {
    auto args = cli::parse(argc, argv, 2, {
        cli::Flag{"--lib"},
        cli::Flag{"--workspace"},
    });
    bool is_lib = args.has("--lib");
    bool is_workspace = args.has("--workspace");
    if (is_lib && is_workspace)
        return command_error("--lib and --workspace are mutually exclusive");
    auto& pos = args.positional();
    if (pos.size() > 1)
        return command_error(std::format("unexpected argument '{}'", pos[1]));
    auto name = pos.empty() ? std::string{} : pos[0];

    auto target_dir = std::filesystem::current_path();
    if (!name.empty()) {
        target_dir /= name;
        std::error_code ec;
        std::filesystem::create_directories(target_dir, ec);
        if (ec) {
            std::println(std::cerr, "error: failed to create directory '{}': {}",
                         target_dir.string(), ec.message());
            return 1;
        }
    } else {
        // Default package name to the current directory name.
        name = target_dir.filename().string();
    }

    auto manifest_path = target_dir / "exon.toml";
    if (std::filesystem::exists(manifest_path)) {
        std::println(std::cerr, "error: {} already exists", manifest_path.string());
        return 1;
    }
    try {
        if (is_workspace) {
            write_text(manifest_path, "[workspace]\nmembers = []\n");
        } else {
            create_package_files(target_dir, name, is_lib);
        }
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
    std::println("created {} ({})", manifest_path.string(),
                 is_workspace ? "workspace" : (is_lib ? "lib" : "bin"));
    return 0;
}

int cmd_new(int argc, char* argv[]) {
    try {
        auto args = cli::parse(argc, argv, 2, {
            cli::Flag{"--lib"},
            cli::Flag{"--bin"},
        });
        bool is_lib = args.has("--lib");
        bool is_bin = args.has("--bin");
        if (is_lib == is_bin)
            return command_error("choose exactly one of --lib or --bin");

        auto const& positional = args.positional();
        if (positional.size() != 1)
            return command_error("usage: exon new --lib|--bin <name>");

        auto workspace_root = std::filesystem::current_path();
        auto workspace_manifest = load_manifest(workspace_root);
        if (!manifest::is_workspace(workspace_manifest))
            return command_error("exon new must run from a workspace root");

        auto member_name = positional[0];
        auto target_dir = workspace_root / member_name;
        if (std::filesystem::exists(target_dir))
            return command_error(std::format("{} already exists", target_dir.string()));

        create_package_files(target_dir, member_name, is_lib);
        update_workspace_members_list(workspace_root, member_name);
        std::println("created {} ({})", (target_dir / "exon.toml").string(),
                     is_lib ? "lib" : "bin");
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
    return 0;
}

int cmd_info() {
    try {
        auto project_root = std::filesystem::current_path();
        auto m = load_manifest(project_root);
        if (!manifest::is_workspace(m)) {
            if (auto ws_root = manifest::system::find_workspace_root(project_root); ws_root &&
                *ws_root != project_root) {
                auto workspace_manifest = load_manifest(*ws_root);
                m = manifest::apply_workspace_defaults(std::move(m), workspace_manifest);
            }
        }
        std::println("name: {}", m.name);
        std::println("version: {}", m.version);
        if (!m.description.empty())
            std::println("description: {}", m.description);
        if (!m.authors.empty()) {
            std::print("authors: ");
            for (std::size_t i = 0; i < m.authors.size(); ++i) {
                if (i > 0)
                    std::print(", ");
                std::print("{}", m.authors[i]);
            }
            std::println("");
        }
        if (!m.license.empty())
            std::println("license: {}", m.license);
        std::println("type: {}", m.type);
        std::println("standard: C++{}", m.standard);
        auto host = toolchain::detect_host_platform();
        std::println("host: {}", host.to_string());
        if (!m.platforms.empty()) {
            std::println("platforms:");
            for (auto const& p : m.platforms) {
                if (toolchain::platform_has_os(p) && toolchain::platform_has_arch(p))
                    std::println("  {{ os = \"{}\", arch = \"{}\" }}",
                                 toolchain::platform_os_name(p),
                                 toolchain::platform_arch_name(p));
                else if (toolchain::platform_has_os(p))
                    std::println("  {{ os = \"{}\" }}",
                                 toolchain::platform_os_name(p));
                else
                    std::println("  {{ arch = \"{}\" }}",
                                 toolchain::platform_arch_name(p));
            }
        }
        if (!m.dependencies.empty()) {
            std::println("dependencies:");
            for (auto const& [name, ver] : m.dependencies) {
                std::println("  {} = \"{}\"", name, ver);
            }
        }
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
    return 0;
}

using BuildFn = std::function<int(std::filesystem::path const&,
                                  manifest::Manifest const&,
                                  manifest::Manifest const&,
                                  bool,
                                  std::string_view)>;

int run_build_command(int argc, char* argv[], BuildFn fn) {
    try {
        auto args = cli::parse(argc, argv, 2, workspace_select_defs(build_defs));
        auto release = args.has("--release");
        auto target = std::string{args.get("--target")};
        auto requested_members = args.get_list("--member");
        auto excluded_members = args.get_list("--exclude");
        auto project_root = std::filesystem::current_path();
        auto raw_m = load_manifest(project_root);
        if (!check_platform(raw_m, target))
            return 1;
        auto m = resolve_manifest(raw_m, target);
        if (manifest::is_workspace(raw_m)) {
            auto selection = select_workspace_members(project_root, raw_m, target,
                                                      requested_members, excluded_members,
                                                      false, false);
            return run_selected_workspace_members(project_root, selection, [&](auto const& member) {
                return fn(member.path, member.raw_manifest, member.resolved_manifest, release, target);
            });
        }
        if (!requested_members.empty() || !excluded_members.empty())
            return command_error("--member and --exclude require a workspace root");
        if (m.name.empty())
            return command_error("package name is required in exon.toml");
        return fn(project_root, raw_m, m, release, target);
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
}

void print_workspace_human_member_header(std::filesystem::path const& workspace_root,
                                         WorkspaceMember const& member) {
    std::println("==> member {}", workspace_display_name(member, workspace_root));
}

struct WorkspaceTestAggregate {
    int members_run = 0;
    int members_passed = 0;
    int members_failed = 0;
    int binaries_run = 0;
};

void print_workspace_test_summary(WorkspaceTestAggregate const& aggregate,
                                  std::chrono::milliseconds elapsed) {
    std::println("");
    std::println("exon: workspace test {}",
                 aggregate.members_failed == 0 ? "succeeded" : "failed");
    std::println("  members   {} run, {} passed, {} failed",
                 aggregate.members_run, aggregate.members_passed, aggregate.members_failed);
    std::println("  binaries  {} run", aggregate.binaries_run);
    std::println("  elapsed   {}", reporting::format_duration(elapsed));
}

int run_workspace_test(std::filesystem::path const& workspace_root,
                       WorkspaceSelection const& selection,
                       bool release, std::string_view target,
                       std::string_view filter,
                       std::optional<std::chrono::milliseconds> timeout,
                       reporting::Options const& options) {
    auto started = std::chrono::steady_clock::now();
    WorkspaceTestAggregate aggregate;
    int last_rc = 0;

    for (auto const& member : selection.members) {
        if (options.output == reporting::OutputMode::human) {
            if (aggregate.members_run > 0)
                std::println("");
            print_workspace_human_member_header(workspace_root, member);
        } else {
            std::println("--- {} ---", workspace_display_name(member, workspace_root));
        }

        ++aggregate.members_run;
        build::system::TestRunSummary member_summary;
        auto rc = build::system::run_test(member.path, member.resolved_manifest,
                                          member.raw_manifest, release, target, filter,
                                          timeout, options, member_summary);
        aggregate.binaries_run += member_summary.collected;
        if (rc == 0) {
            ++aggregate.members_passed;
            continue;
        }

        ++aggregate.members_failed;
        last_rc = rc;
        break;
    }

    if (options.output == reporting::OutputMode::human) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        print_workspace_test_summary(aggregate, elapsed);
    }
    return last_rc;
}

int cmd_build(int argc, char* argv[]) {
    try {
        auto args = cli::parse(argc, argv, 2, workspace_select_defs({
            cli::Flag{"--release"},
            cli::Option{"--target"},
            cli::Option{"--output"},
        }));
        auto release = args.has("--release");
        auto target = std::string{args.get("--target")};
        auto options = parse_reporting_options(args.get("--output"));
        auto requested_members = args.get_list("--member");
        auto excluded_members = args.get_list("--exclude");
        auto project_root = std::filesystem::current_path();
        auto raw_m = load_manifest(project_root);
        if (!check_platform(raw_m, target))
            return 1;
        auto m = resolve_manifest(raw_m, target);
        if (manifest::is_workspace(raw_m)) {
            auto selection = select_workspace_members(project_root, raw_m, target,
                                                      requested_members, excluded_members,
                                                      true, false);
            return run_workspace_build(project_root, raw_m, selection, release, target, options);
        }
        if (!requested_members.empty() || !excluded_members.empty())
            return command_error("--member and --exclude require a workspace root");
        if (m.name.empty())
            return command_error("package name is required in exon.toml");
        return build::system::run(project_root, m, raw_m, release, target, options);
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
}

int cmd_check(int argc, char* argv[]) {
    return run_build_command(argc, argv,
                             [](std::filesystem::path const& project_root,
                                manifest::Manifest const& raw_m,
                                manifest::Manifest const& m,
                                bool release,
                                std::string_view target) {
                                 return build::system::run_check(project_root, m, raw_m,
                                                                 release, target);
                             });
}

int cmd_run(int argc, char* argv[]) {
    try {
        auto args = cli::parse(argc, argv, 2, workspace_select_defs(build_defs));
        auto release = args.has("--release");
        auto target = std::string{args.get("--target")};
        auto requested_members = args.get_list("--member");
        auto excluded_members = args.get_list("--exclude");
        auto project_root = std::filesystem::current_path();
        auto raw_m = load_manifest(project_root);
        if (!check_platform(raw_m, target))
            return 1;
        auto runnable_target = resolve_runnable_command_target(project_root, raw_m, target,
                                                               requested_members, excluded_members);
        if (!runnable_target)
            return command_error(runnable_target.error());
        bool is_wasm = !target.empty() && target.starts_with("wasm32");
        bool is_android = !target.empty() && target.ends_with("-linux-android");
        int rc = build::system::run(runnable_target->root,
                                    runnable_target->resolved_manifest,
                                    runnable_target->raw_manifest,
                                    release, target);
        if (rc != 0)
            return rc;

        if (is_android)
            throw std::runtime_error(
                "cannot `exon run` an Android target on the host; deploy the produced library "
                "to a device or emulator instead");

        auto project = build::project_context(runnable_target->root, release, target);
        core::ProcessSpec spec{
            .cwd = runnable_target->root,
        };
        if (is_wasm) {
            auto wasm_runtime = toolchain::system::detect_wasm_runtime();
            if (wasm_runtime.empty())
                throw std::runtime_error(
                    "wasmtime not found on PATH (install: https://wasmtime.dev)");
            auto wasm_file = project.build_dir / runnable_target->resolved_manifest.name;
            spec.program = wasm_runtime;
            spec.args.push_back(wasm_file.string());
        } else {
            auto exe = project.build_dir /
                (runnable_target->resolved_manifest.name + std::string{toolchain::exe_suffix});
            spec.program = exe.string();
        }
        for (auto const& a : args.positional())
            spec.args.push_back(a);
        std::println("running {}...\n", runnable_target->resolved_manifest.name);
        return build::system::run_process(spec);
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
}

int cmd_debug(int argc, char* argv[]) {
    try {
        auto debug_defs = workspace_select_defs({
            cli::Flag{"--release"},
            cli::Option{"--target"},
            cli::Option{"--debugger"},
        });
        auto args = cli::parse(argc, argv, 2, debug_defs);
        auto release = args.has("--release");
        auto target = std::string{args.get("--target")};
        if (!target.empty()) {
            return command_error(
                "exon debug currently supports native host executables only; --target is not supported yet");
        }

        auto program_args = std::vector<std::string>{};
        for (auto const& arg : args.positional())
            program_args.push_back(arg);

        auto debugger_request = std::string{args.get("--debugger")};
        if (debugger_request.empty())
            debugger_request = "auto";
        auto requested_kind = debug::classify_debugger_program(debugger_request);
        if (requested_kind == debug::DebuggerKind::devenv &&
            debug::has_devenv_unsafe_program_args(program_args)) {
            return command_error(
                "devenv cannot safely launch programs whose arguments start with '/'",
                "use --debugger cdb, lldb, gdb, or a different debugger path for this command");
        }

        auto requested_members = args.get_list("--member");
        auto excluded_members = args.get_list("--exclude");
        auto project_root = std::filesystem::current_path();
        auto raw_m = load_manifest(project_root);
        if (!check_platform(raw_m, {}))
            return 1;

        auto runnable_target = resolve_runnable_command_target(project_root, raw_m, {},
                                                               requested_members, excluded_members);
        if (!runnable_target)
            return command_error(runnable_target.error());

        auto host = toolchain::detect_host_platform();
        auto skipped_auto_kinds = std::vector<debug::DebuggerKind>{};
        auto skipped_devenv_for_args = debugger_request == "auto" &&
            debug::has_devenv_unsafe_program_args(program_args);
        if (skipped_devenv_for_args)
            skipped_auto_kinds.push_back(debug::DebuggerKind::devenv);
        auto debugger_program = skipped_devenv_for_args
            ? debug::system::resolve_debugger(debugger_request, host, skipped_auto_kinds)
            : debug::system::resolve_debugger(debugger_request, host);
        if (!debugger_program)
            return skipped_devenv_for_args
                ? command_error(
                    debugger_program.error(),
                    "devenv was skipped because Visual Studio may treat '/'-prefixed "
                    "program arguments as its own switches")
                : command_error(debugger_program.error());

        auto rc = build::system::run(runnable_target->root,
                                     runnable_target->resolved_manifest,
                                     runnable_target->raw_manifest,
                                     release, {});
        if (rc != 0)
            return rc;

        auto project = build::project_context(runnable_target->root, release, {});
        auto executable = project.build_dir /
            (runnable_target->resolved_manifest.name + std::string{toolchain::exe_suffix});
        auto spec = debug::debugger_launch_spec(*debugger_program, executable,
                                                program_args, runnable_target->root);
        std::println("starting {} for {}...\n",
                     debug::debugger_kind_name(debugger_program->kind),
                     runnable_target->resolved_manifest.name);
        return build::system::run_process(spec);
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
}

int cmd_test(int argc, char* argv[]) {
    try {
        std::vector<cli::ArgDef> const test_defs = {
            cli::Flag{"--release"},
            cli::Option{"--target"},
            cli::Option{"--filter"},
            cli::Option{"--timeout"},
            cli::Option{"--output"},
            cli::Option{"--show-output"},
        };
        auto args = cli::parse(argc, argv, 2, workspace_select_defs(test_defs));
        auto release = args.has("--release");
        auto target = std::string{args.get("--target")};
        auto filter = std::string{args.get("--filter")};
        auto timeout = parse_timeout(args.get("--timeout"));
        auto options = parse_reporting_options(args.get("--output"), args.get("--show-output"));
        auto requested_members = args.get_list("--member");
        auto excluded_members = args.get_list("--exclude");
        auto project_root = std::filesystem::current_path();
        auto raw_m = load_manifest(project_root);
        if (!check_platform(raw_m, target))
            return 1;
        auto m = resolve_manifest(raw_m, target);
        if (manifest::is_workspace(raw_m)) {
            auto selection = select_workspace_members(project_root, raw_m, target,
                                                      requested_members, excluded_members,
                                                      false, false);
            return run_workspace_test(project_root, selection, release, target, filter, timeout,
                                      options);
        }
        if (!requested_members.empty() || !excluded_members.empty())
            return command_error("--member and --exclude require a workspace root");
        if (m.name.empty())
            return command_error("package name is required in exon.toml");
        return build::system::run_test(project_root, m, raw_m, release, target, filter,
                                       timeout, options);
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
}

int cmd_clean(int argc, char* argv[]) {
    auto project_root = std::filesystem::current_path();
    try {
        auto args = cli::parse(argc, argv, 2, workspace_select_defs());
        if (std::filesystem::exists(project_root / "exon.toml")) {
            auto m = load_manifest(project_root);
            if (manifest::is_workspace(m)) {
                auto selection = select_workspace_members(project_root, m, {},
                                                          args.get_list("--member"),
                                                          args.get_list("--exclude"),
                                                          false, false);
                auto rc = run_selected_workspace_members(project_root, selection, [](auto const& member) {
                    auto dir = member.path / ".exon";
                    if (std::filesystem::exists(dir)) {
                        std::filesystem::remove_all(dir);
                        std::println("cleaned .exon/");
                    } else {
                        std::println("nothing to clean");
                    }
                    return 0;
                });
                if (rc != 0)
                    return rc;
                auto workspace_exon_dir = project_root / ".exon";
                if (std::filesystem::exists(workspace_exon_dir)) {
                    std::filesystem::remove_all(workspace_exon_dir);
                    std::println("cleaned workspace .exon/");
                }
                return 0;
            }
        }
        if (!args.get_list("--member").empty() || !args.get_list("--exclude").empty())
            return command_error("--member and --exclude require a workspace root");
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
    auto exon_dir = project_root / ".exon";
    if (std::filesystem::exists(exon_dir)) {
        std::filesystem::remove_all(exon_dir);
        std::println("cleaned .exon/");
    } else {
        std::println("nothing to clean");
    }
    return 0;
}

int cmd_add(int argc, char* argv[]) {
    try {
    auto args = cli::parse(argc, argv, 2, {
        cli::Flag{"--dev"}, cli::Flag{"--path"}, cli::Flag{"--workspace"}, cli::Flag{"--vcpkg"},
        cli::Flag{"--no-default-features"},
        cli::Option{"--git"}, cli::Option{"--subdir"}, cli::Option{"--version"},
        cli::Option{"--name"}, cli::ListOption{"--features"},
    });
    bool dev = args.has("--dev");
    bool is_path = args.has("--path");
    bool is_workspace_dep = args.has("--workspace");
    bool is_vcpkg = args.has("--vcpkg");
    bool is_git_subdir = !args.get("--git").empty();
    bool no_default_features = args.has("--no-default-features");
    auto features = args.get_list("--features");
    auto git_repo = std::string{args.get("--git")};
    auto git_version = std::string{args.get("--version")};
    auto git_subdir = std::string{args.get("--subdir")};
    auto git_name = std::string{args.get("--name")};
    auto& positional = args.positional();

    if (!features.empty() && (is_path || is_workspace_dep || is_git_subdir))
        return command_error("--features is not valid with --path, --workspace, or --git");
    if (no_default_features && features.empty())
        return command_error("--no-default-features requires --features");

    int exclusive_count = int(is_path) + int(is_workspace_dep) + int(is_vcpkg) + int(is_git_subdir);
    if (exclusive_count > 1)
        return command_error("--path, --workspace, --vcpkg, --git are mutually exclusive");

    if (!std::filesystem::exists("exon.toml")) {
        std::println(std::cerr, "error: exon.toml not found. run 'exon init' first");
        return 1;
    }

    auto project_root = std::filesystem::current_path();
    auto manifest_path = project_root / "exon.toml";
    auto content = read_text(manifest_path);
    auto m = manifest::system::load(manifest_path.string());

    std::string name, value;
    std::string section_prefix = dev ? "dev-dependencies" : "dependencies";
    std::string section;
    std::string dep_line;
    std::string display;

    if (is_path) {
        if (positional.size() < 2) {
            std::println(std::cerr, "usage: exon add [--dev] --path <name> <path>");
            return 1;
        }
        name = positional[0];
        value = positional[1];
        auto target_dir = project_root / value;
        if (!std::filesystem::exists(target_dir / "exon.toml")) {
            std::println(std::cerr, "error: no exon.toml found at {}", target_dir.string());
            return 1;
        }
        section = section_prefix + ".path";
        dep_line = std::format("{} = \"{}\"\n", name, value);
        display = std::format("path dep {} = \"{}\"", name, value);
    } else if (is_workspace_dep) {
        if (positional.size() < 1) {
            std::println(std::cerr, "usage: exon add [--dev] --workspace <name>");
            return 1;
        }
        name = positional[0];
        auto ws_root = manifest::system::find_workspace_root(project_root);
        if (!ws_root) {
            std::println(std::cerr, "error: no workspace root found");
            return 1;
        }
        auto ws_m = manifest::system::load((*ws_root / "exon.toml").string());
        if (!manifest::system::resolve_workspace_member(*ws_root, ws_m, name)) {
            std::println(std::cerr, "error: workspace member '{}' not found", name);
            return 1;
        }
        section = section_prefix + ".workspace";
        dep_line = std::format("{} = true\n", name);
        display = std::format("workspace dep {}", name);
    } else if (is_git_subdir) {
        if (git_repo.empty() || git_version.empty() || git_subdir.empty()) {
            std::println(std::cerr,
                         "usage: exon add [--dev] --git <repo> --version <ver> --subdir <dir> "
                         "[--name <name>]");
            return 1;
        }
        name = git_name.empty() ? git_subdir : git_name;
        section = section_prefix;
        dep_line = std::format(
            "{} = {{ git = \"{}\", version = \"{}\", subdir = \"{}\" }}\n",
            name, git_repo, git_version, git_subdir);
        display = std::format("git subdir dep {} = {{ git = \"{}\", version = \"{}\", "
                              "subdir = \"{}\" }}",
                              name, git_repo, git_version, git_subdir);
    } else if (is_vcpkg) {
        if (positional.size() < 2) {
            std::println(std::cerr,
                         "usage: exon add [--dev] --vcpkg <name> <version> [--features a,b,c]");
            return 1;
        }
        name = positional[0];
        value = positional[1];
        section = section_prefix + ".vcpkg";
        if (features.empty()) {
            dep_line = std::format("{} = \"{}\"\n", name, value);
            display = std::format("vcpkg dep {} = \"{}\"", name, value);
        } else {
            std::string feat_list;
            for (std::size_t fi = 0; fi < features.size(); ++fi) {
                if (fi > 0)
                    feat_list += ", ";
                feat_list += std::format("\"{}\"", features[fi]);
            }
            dep_line = std::format("{} = {{ version = \"{}\", features = [{}] }}\n",
                                    name, value, feat_list);
            display = std::format("vcpkg dep {} = {{ version = \"{}\", features = [{}] }}",
                                   name, value, feat_list);
        }
    } else {
        if (positional.size() < 2) {
            std::println(std::cerr,
                         "usage: exon add [--dev] <package> <version> [--features a,b --no-default-features]\n"
                         "       exon add [--dev] --path <name> <path>\n"
                         "       exon add [--dev] --workspace <name>\n"
                         "       exon add [--dev] --vcpkg <name> <version>\n"
                         "       exon add [--dev] --git <repo> --version <ver> --subdir <dir> "
                         "[--name <name>]");
            return 1;
        }
        name = positional[0];
        value = positional[1];
        section = section_prefix;
        if (!features.empty()) {
            std::string feat_list;
            for (std::size_t fi = 0; fi < features.size(); ++fi) {
                if (fi > 0)
                    feat_list += ", ";
                feat_list += std::format("\"{}\"", features[fi]);
            }
            if (no_default_features)
                dep_line = std::format(
                    "\"{}\" = {{ version = \"{}\", default-features = false, features = [{}] }}\n",
                    name, value, feat_list);
            else
                dep_line = std::format(
                    "\"{}\" = {{ version = \"{}\", features = [{}] }}\n",
                    name, value, feat_list);
            display = std::format("{} {} = {{ version = \"{}\", features = [{}] }}",
                                  dev ? "dev-dependency" : "dependency", name, value, feat_list);
        } else {
            dep_line = std::format("\"{}\" = \"{}\"\n", name, value);
            display = std::format("{} {} = \"{}\"",
                                  dev ? "dev-dependency" : "dependency", name, value);
        }
    }

    // use the base name for duplicate check (for git deps, key is the full URL path)
    auto dup_key = name;
    if (manifest::dependency_exists(m, dup_key)) {
        std::println(std::cerr, "error: '{}' is already a dependency", dup_key);
        return 1;
    }

    manifest::insert_into_section(content, section, dep_line);

    try {
        write_text(manifest_path, content);
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
    std::println("added {}", display);
    return 0;
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
}

int cmd_remove(int argc, char* argv[]) {
    try {
    if (argc < 3) {
        std::println(std::cerr, "usage: exon remove <package>");
        return 1;
    }
    auto pkg = std::string{argv[2]};

    auto project_root = std::filesystem::current_path();
    auto manifest_path = project_root / "exon.toml";
    if (!std::filesystem::exists(manifest_path)) {
        std::println(std::cerr, "error: exon.toml not found. run 'exon init' first");
        return 1;
    }

    auto m = manifest::system::load(manifest_path.string());
    if (!manifest::dependency_exists(m, pkg)) {
        std::println(std::cerr, "error: '{}' is not a dependency", pkg);
        return 1;
    }

    // if this is a subdir dep, compute the composite lock name to erase later
    std::string lock_name_to_erase = pkg;
    auto find_subdir = [&]() -> manifest::GitSubdirDep const* {
        if (auto it = m.subdir_deps.find(pkg); it != m.subdir_deps.end())
            return &it->second;
        if (auto it = m.dev_subdir_deps.find(pkg); it != m.dev_subdir_deps.end())
            return &it->second;
        return nullptr;
    };
    if (auto const* sdep = find_subdir())
        lock_name_to_erase = std::format("{}#{}", sdep->repo, sdep->subdir);

    auto content = read_text(manifest_path);
    manifest::remove_dependency_entry(content, pkg);
    manifest::cleanup_empty_subsections(content);

    try {
        write_text(manifest_path, content);
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
    std::println("removed {}", pkg);

    auto lock_path = project_root / "exon.lock";
    if (std::filesystem::exists(lock_path)) {
        auto lf = lock::system::load(lock_path.string());
        auto& pkgs = lf.packages;
        std::erase_if(pkgs, [&](auto const& p) { return p.name == lock_name_to_erase; });
        lock::system::save(lf, lock_path.string());
    }
    return 0;
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
}

int cmd_update(int argc, char* argv[]) {
    try {
        auto args = cli::parse(argc, argv, 2, workspace_select_defs());
        auto project_root = std::filesystem::current_path();
        auto manifest_path = project_root / "exon.toml";
        if (!std::filesystem::exists(manifest_path)) {
            std::println(std::cerr, "error: exon.toml not found. run 'exon init' first");
            return 1;
        }
        auto m = load_manifest(project_root);
        if (manifest::is_workspace(m)) {
            auto selection = select_workspace_members(project_root, m, {},
                                                      args.get_list("--member"),
                                                      args.get_list("--exclude"),
                                                      false, false);
            return run_selected_workspace_members(project_root, selection, [&](auto const& member) {
                auto lock_path = member.path / "exon.lock";
                if (std::filesystem::exists(lock_path))
                    std::filesystem::remove(lock_path);
                if (!manifest_has_any_dependencies(member.raw_manifest)) {
                    std::println("no dependencies to update");
                    return 0;
                }
                return build::system::run(member.path, member.resolved_manifest, member.raw_manifest);
            });
        }
        if (!args.get_list("--member").empty() || !args.get_list("--exclude").empty())
            return command_error("--member and --exclude require a workspace root");

        auto lock_path = project_root / "exon.lock";
        if (std::filesystem::exists(lock_path))
            std::filesystem::remove(lock_path);

        if (!manifest_has_any_dependencies(m)) {
            std::println("no dependencies to update");
            return 0;
        }

        return build::system::run(project_root, m);
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
}

int cmd_sync(int argc, char* argv[]) {
    try {
        auto args = cli::parse(argc, argv, 2, workspace_select_defs());
        auto project_root = std::filesystem::current_path();
        auto raw_m = load_manifest(project_root);
        // for sync: merge ALL target sections for fetching, but keep raw for CMake generation
        auto fetch_m = manifest::resolve_all_targets(raw_m);
        if (manifest::is_workspace(raw_m)) {
            auto selection = select_workspace_members(project_root, raw_m, {},
                                                      args.get_list("--member"),
                                                      args.get_list("--exclude"),
                                                      true, false);
            auto rc = run_selected_workspace_members(project_root, selection, [&](auto const& member) {
                return sync_workspace_member_cmake(member, std::nullopt, true);
            });
            if (rc != 0)
                return rc;
            return sync_workspace_root_cmake(project_root, raw_m, selection);
        }
        if (!args.get_list("--member").empty() || !args.get_list("--exclude").empty())
            return command_error("--member and --exclude require a workspace root");
        auto lock_path = (project_root / "exon.lock").string();
        auto fetch_result = fetch::system::fetch_all({
            .manifest = fetch_m,
            .project_root = project_root,
            .lock_path = std::filesystem::path{lock_path},
        });
        build::system::sync_root_cmake(project_root, raw_m, fetch_result.deps);
        return 0;
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
}

int cmd_fmt() {
    auto project_root = std::filesystem::current_path();
    auto src_dir = project_root / "src";
    if (!std::filesystem::exists(src_dir)) {
        std::println(std::cerr, "error: src/ directory not found");
        return 1;
    }

    std::vector<std::string> files;
    for (auto const& entry : std::filesystem::recursive_directory_iterator(src_dir)) {
        if (!entry.is_regular_file())
            continue;
        auto ext = entry.path().extension().string();
        if (ext == ".cpp" || ext == ".cppm" || ext == ".h" || ext == ".hpp") {
            files.push_back(entry.path().string());
        }
    }

    if (files.empty()) {
        std::println("no source files to format");
        return 0;
    }

    std::ranges::sort(files);
    core::ProcessSpec spec{
        .program = "clang-format",
        .args = {"-i"},
        .cwd = project_root,
    };
    spec.args.insert(spec.args.end(), files.begin(), files.end());

    int rc = build::system::run_process(spec);
    if (rc != 0) {
        std::println(std::cerr, "error: clang-format failed (is it installed?)");
        return 1;
    }
    std::println("formatted {} file(s)", files.size());
    return 0;
}

} // namespace commands
