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
import fetch;
import fetch.system;
import lock;
import lock.system;
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
            {"init [--lib] [name]",                "create a new exon.toml"},
            {"info",                               "show package information"},
            {"build [--release] [--target <t>]",   "build the project"},
            {"check [--release] [--target <t>]",   "check syntax without linking"},
            {"run [--release] [--target <t>] [args]", "build and run the project"},
            {"test [--release] [--target <t>] [--timeout <sec>]", "build and run tests"},
            {"clean",                              "remove build artifacts"},
            {"add [--dev] <pkg> <ver>",            "add a git dependency"},
            {"add [--dev] --path <name> <path>",   "add a local path dependency"},
            {"add [--dev] --workspace <name>",     "add a workspace member dependency"},
            {"add [--dev] --vcpkg <name> <ver> [--features a,b,c]",
                                                   "add a vcpkg dependency"},
            {"add [--dev] --git <repo> --version <ver> --subdir <dir> [--name <n>]",
                                                   "add a git dep pointing to a subdirectory"},
            {"remove <pkg>",                       "remove a dependency"},
            {"update",                             "update dependencies to latest compatible versions"},
            {"sync",                               "sync CMakeLists.txt with exon.toml"},
            {"fmt",                                "format source files with clang-format"},
            {"version",                            "show exon version"},
        }},
        cli::Section{"targets", {
            {"wasm32-wasi", "WebAssembly (requires wasi-sdk via intron or WASI_SDK_PATH)"},
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

// run a function in each workspace member directory
int run_for_workspace(std::filesystem::path const& workspace_root,
                      manifest::Manifest const& m,
                      std::function<int(std::filesystem::path const&)> fn) {
    for (auto const& member : m.workspace_members) {
        auto member_path = workspace_root / member;
        if (!std::filesystem::exists(member_path / "exon.toml")) {
            std::println(std::cerr, "error: {} has no exon.toml", member);
            return 1;
        }
        std::println("--- {} ---", member);
        int rc = fn(member_path);
        if (rc != 0)
            return rc;
    }
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

manifest::Manifest resolve_manifest(manifest::Manifest m, std::string_view target = {}) {
    return manifest::resolve_for_platform(std::move(m), effective_platform(target));
}

bool check_platform(manifest::Manifest const& m, std::string_view target = {}) {
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
    auto args = cli::parse(argc, argv, 2, {cli::Flag{"--lib"}});
    bool is_lib = args.has("--lib");
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
    auto tmpl = std::string{is_lib ? templates::exon_toml_lib : templates::exon_toml_bin};
    // Fill in the package name (template has `name = ""`).
    auto name_pos = tmpl.find("name = \"\"");
    if (name_pos != std::string::npos)
        tmpl.replace(name_pos, 9, std::format("name = \"{}\"", name));
    try {
        write_text(manifest_path, tmpl);
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
    std::println("created {} ({})", manifest_path.string(), is_lib ? "lib" : "bin");
    return 0;
}

int cmd_info() {
    try {
        auto m = load_manifest();
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
                                  bool,
                                  std::string_view)>;

int run_build_command(int argc, char* argv[], BuildFn fn) {
    try {
        auto args = cli::parse(argc, argv, 2, build_defs);
        auto release = args.has("--release");
        auto target = std::string{args.get("--target")};
        auto project_root = std::filesystem::current_path();
        auto m = load_manifest(project_root);
        if (!check_platform(m, target))
            return 1;
        m = resolve_manifest(std::move(m), target);
        if (manifest::is_workspace(m)) {
            return run_for_workspace(project_root, m, [&](auto const& member_path) {
                auto member_m = load_manifest(member_path);
                if (!check_platform(member_m, target))
                    return 1;
                member_m = resolve_manifest(std::move(member_m), target);
                return fn(member_path, member_m, release, target);
            });
        }
        if (m.name.empty())
            return command_error("package name is required in exon.toml");
        return fn(project_root, m, release, target);
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
}

int cmd_build(int argc, char* argv[]) {
    return run_build_command(argc, argv,
                             [](std::filesystem::path const& project_root,
                                manifest::Manifest const& m,
                                bool release,
                                std::string_view target) {
                                 return build::system::run(project_root, m, release, target);
                             });
}

int cmd_check(int argc, char* argv[]) {
    return run_build_command(argc, argv,
                             [](std::filesystem::path const& project_root,
                                manifest::Manifest const& m,
                                bool release,
                                std::string_view target) {
                                 return build::system::run_check(project_root, m, release,
                                                                 target);
                             });
}

int cmd_run(int argc, char* argv[]) {
    try {
        auto args = cli::parse(argc, argv, 2, build_defs);
        auto release = args.has("--release");
        auto target = std::string{args.get("--target")};
        auto project_root = std::filesystem::current_path();
        auto m = load_manifest(project_root);
        if (!check_platform(m, target))
            return 1;
        m = resolve_manifest(std::move(m), target);
        if (m.name.empty())
            return command_error("package name is required in exon.toml");
        if (m.type == "lib")
            return command_error("cannot run a library package");
        bool is_wasm = !target.empty();
        int rc = build::system::run(project_root, m, release, target);
        if (rc != 0)
            return rc;

        auto project = build::project_context(project_root, release, target);
        core::ProcessSpec spec{
            .cwd = project_root,
        };
        if (is_wasm) {
            auto wasm_runtime = toolchain::system::detect_wasm_runtime();
            if (wasm_runtime.empty())
                throw std::runtime_error(
                    "wasmtime not found on PATH (install: https://wasmtime.dev)");
            auto wasm_file = project.build_dir / m.name;
            spec.program = wasm_runtime;
            spec.args.push_back(wasm_file.string());
        } else {
            auto exe = project.build_dir / (m.name + std::string{toolchain::exe_suffix});
            spec.program = exe.string();
        }
        for (auto const& a : args.positional())
            spec.args.push_back(a);
        std::println("running {}...\n", m.name);
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
        };
        auto args = cli::parse(argc, argv, 2, test_defs);
        auto release = args.has("--release");
        auto target = std::string{args.get("--target")};
        auto filter = std::string{args.get("--filter")};
        auto timeout = parse_timeout(args.get("--timeout"));
        auto project_root = std::filesystem::current_path();
        auto m = load_manifest(project_root);
        if (!check_platform(m, target))
            return 1;
        m = resolve_manifest(std::move(m), target);
        if (manifest::is_workspace(m)) {
            return run_for_workspace(project_root, m, [&](auto const& member_path) {
                auto member_m = load_manifest(member_path);
                if (!check_platform(member_m, target))
                    return 1;
                member_m = resolve_manifest(std::move(member_m), target);
                return build::system::run_test(member_path, member_m, release, target, filter,
                                               timeout);
            });
        }
        if (m.name.empty())
            return command_error("package name is required in exon.toml");
        return build::system::run_test(project_root, m, release, target, filter, timeout);
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
}

int cmd_clean() {
    auto project_root = std::filesystem::current_path();
    try {
        if (std::filesystem::exists(project_root / "exon.toml")) {
            auto m = load_manifest(project_root);
            if (manifest::is_workspace(m)) {
                return run_for_workspace(project_root, m, [](auto const& member_path) {
                    auto dir = member_path / ".exon";
                    if (std::filesystem::exists(dir)) {
                        std::filesystem::remove_all(dir);
                        std::println("cleaned .exon/");
                    }
                    return 0;
                });
            }
        }
    } catch (...) {
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

int cmd_update() {
    try {
        auto project_root = std::filesystem::current_path();
        auto manifest_path = project_root / "exon.toml";
        if (!std::filesystem::exists(manifest_path)) {
            std::println(std::cerr, "error: exon.toml not found. run 'exon init' first");
            return 1;
        }

        auto lock_path = project_root / "exon.lock";
        if (std::filesystem::exists(lock_path)) {
            std::filesystem::remove(lock_path);
        }

        auto m = load_manifest(project_root);
        if (m.dependencies.empty()) {
            std::println("no dependencies to update");
            return 0;
        }

        return build::system::run(project_root, m);
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
}

int cmd_sync() {
    try {
        auto project_root = std::filesystem::current_path();
        auto raw_m = load_manifest(project_root);
        // for sync: merge ALL target sections for fetching, but keep raw for CMake generation
        auto fetch_m = manifest::resolve_all_targets(raw_m);
        if (manifest::is_workspace(raw_m)) {
            return run_for_workspace(project_root, raw_m, [](auto const& member_path) {
                auto raw_member = load_manifest(member_path);
                auto fetch_member = manifest::resolve_all_targets(raw_member);
                auto lock_path = (member_path / "exon.lock").string();
                auto fetch_result = fetch::system::fetch_all({
                    .manifest = fetch_member,
                    .project_root = member_path,
                    .lock_path = std::filesystem::path{lock_path},
                });
                build::system::sync_root_cmake(member_path, raw_member, fetch_result.deps);
                return 0;
            });
        }
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
