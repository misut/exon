export module commands;
import std;
import cli;
import toml;
import manifest;
import build;
import fetch;
import lock;
import templates;
import toolchain;

export namespace commands {

#ifndef EXON_PKG_VERSION
#define EXON_PKG_VERSION "dev"
#endif
constexpr auto version = EXON_PKG_VERSION;

std::string usage_text() {
    return cli::usage("exon", {
        cli::Section{"commands", {
            {"init [--lib] [name]",                "create a new exon.toml"},
            {"info",                               "show package information"},
            {"build [--release] [--target <t>]",   "build the project"},
            {"check [--release] [--target <t>]",   "check syntax without linking"},
            {"run [--release] [--target <t>] [args]", "build and run the project"},
            {"test [--release] [--target <t>]",    "build and run tests"},
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

manifest::Manifest load_manifest() {
    if (!std::filesystem::exists("exon.toml")) {
        throw std::runtime_error("exon.toml not found. run 'exon init' first");
    }
    return manifest::load("exon.toml");
}

// run a function in each workspace member directory
int run_for_workspace(manifest::Manifest const& m,
                      std::function<int(std::filesystem::path const&)> fn) {
    auto root = std::filesystem::current_path();
    for (auto const& member : m.workspace_members) {
        auto member_path = root / member;
        if (!std::filesystem::exists(member_path / "exon.toml")) {
            std::println(std::cerr, "error: {} has no exon.toml", member);
            return 1;
        }
        std::println("--- {} ---", member);
        std::filesystem::current_path(member_path);
        int rc = fn(member_path);
        std::filesystem::current_path(root);
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
            if (!p.os.empty() && !p.arch.empty())
                std::println(std::cerr, "    {{ os = \"{}\", arch = \"{}\" }}", p.os, p.arch);
            else if (!p.os.empty())
                std::println(std::cerr, "    {{ os = \"{}\" }}", p.os);
            else
                std::println(std::cerr, "    {{ arch = \"{}\" }}", p.arch);
        }
        return false;
    }
    return true;
}

std::vector<cli::ArgDef> const build_defs = {
    cli::Flag{"--release"},
    cli::Option{"--target"},
};

std::string read_file(std::string_view path) {
    auto file = std::ifstream(std::string{path}, std::ios::binary);
    if (!file) return {};
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
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
        return cli::error(std::format("unexpected argument '{}'", pos[1]));
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
    auto file = std::ofstream(manifest_path);
    if (!file) {
        std::println(std::cerr, "error: failed to create {}", manifest_path.string());
        return 1;
    }
    auto tmpl = std::string{is_lib ? templates::exon_toml_lib : templates::exon_toml_bin};
    // Fill in the package name (template has `name = ""`).
    auto name_pos = tmpl.find("name = \"\"");
    if (name_pos != std::string::npos)
        tmpl.replace(name_pos, 9, std::format("name = \"{}\"", name));
    file << tmpl;
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
                if (!p.os.empty() && !p.arch.empty())
                    std::println("  {{ os = \"{}\", arch = \"{}\" }}", p.os, p.arch);
                else if (!p.os.empty())
                    std::println("  {{ os = \"{}\" }}", p.os);
                else
                    std::println("  {{ arch = \"{}\" }}", p.arch);
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

using BuildFn = int(*)(manifest::Manifest const&, bool, std::string_view);

int run_build_command(int argc, char* argv[], BuildFn fn) {
    try {
        auto args = cli::parse(argc, argv, 2, build_defs);
        auto release = args.has("--release");
        auto target = std::string{args.get("--target")};
        auto m = load_manifest();
        if (!check_platform(m, target))
            return 1;
        m = resolve_manifest(std::move(m), target);
        if (manifest::is_workspace(m)) {
            return run_for_workspace(m, [&](auto const&) {
                auto member_m = manifest::load("exon.toml");
                if (!check_platform(member_m, target))
                    return 1;
                member_m = resolve_manifest(std::move(member_m), target);
                return fn(member_m, release, target);
            });
        }
        if (m.name.empty())
            return cli::error("package name is required in exon.toml");
        return fn(m, release, target);
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
}

int cmd_build(int argc, char* argv[]) {
    return run_build_command(argc, argv, build::run);
}

int cmd_check(int argc, char* argv[]) {
    return run_build_command(argc, argv, build::run_check);
}

int cmd_run(int argc, char* argv[]) {
    try {
        auto args = cli::parse(argc, argv, 2, build_defs);
        auto release = args.has("--release");
        auto target = std::string{args.get("--target")};
        auto m = load_manifest();
        if (!check_platform(m, target))
            return 1;
        m = resolve_manifest(std::move(m), target);
        if (m.name.empty())
            return cli::error("package name is required in exon.toml");
        if (m.type == "lib")
            return cli::error("cannot run a library package");
        bool is_wasm = !target.empty();
        int rc = build::run(m, release, target);
        if (rc != 0)
            return rc;

        auto profile = release ? "release" : "debug";
        std::string run_cmd;
        if (is_wasm) {
            auto wasm_runtime = toolchain::detect_wasm_runtime();
            if (wasm_runtime.empty())
                throw std::runtime_error(
                    "wasmtime not found on PATH (install: https://wasmtime.dev)");
            auto wasm_file = std::filesystem::current_path() / ".exon" / target / profile /
                             m.name;
            run_cmd = std::format("{} {}", toolchain::shell_quote(wasm_runtime),
                                  toolchain::shell_quote(wasm_file.string()));
        } else {
            auto exe = std::filesystem::current_path() / ".exon" / profile /
                       (m.name + std::string{toolchain::exe_suffix});
            run_cmd = toolchain::shell_quote(exe.string());
        }
        for (auto const& a : args.positional()) {
            run_cmd += std::format(" {}", a);
        }
        std::println("running {}...\n", m.name);
        return std::system(run_cmd.c_str());
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
}

int cmd_test(int argc, char* argv[]) {
    return run_build_command(argc, argv, build::run_test);
}

int cmd_clean() {
    try {
        if (std::filesystem::exists("exon.toml")) {
            auto m = load_manifest();
            if (manifest::is_workspace(m)) {
                return run_for_workspace(m, [](auto const&) {
                    auto dir = std::filesystem::current_path() / ".exon";
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
    auto exon_dir = std::filesystem::current_path() / ".exon";
    if (std::filesystem::exists(exon_dir)) {
        std::filesystem::remove_all(exon_dir);
        std::println("cleaned .exon/");
    } else {
        std::println("nothing to clean");
    }
    return 0;
}

// insert `line` into [section] block; create section at EOF if missing
void insert_into_section(std::string& content, std::string const& section,
                         std::string const& line) {
    auto header = std::format("[{}]", section);
    auto pos = content.find(header);
    if (pos == std::string::npos) {
        if (!content.empty() && content.back() != '\n')
            content += '\n';
        content += std::format("\n{}\n{}", header, line);
        return;
    }
    // verify this is an exact section header (preceded by start/newline, followed by newline)
    // simple check: header should start at column 0
    bool at_line_start = (pos == 0) || (content[pos - 1] == '\n');
    if (!at_line_start) {
        // header substring found mid-line; append as new section
        if (!content.empty() && content.back() != '\n')
            content += '\n';
        content += std::format("\n{}\n{}", header, line);
        return;
    }
    auto insert_pos = content.find('\n', pos);
    if (insert_pos != std::string::npos)
        content.insert(insert_pos + 1, line);
    else
        content += std::format("\n{}", line);
}

bool dep_exists(manifest::Manifest const& m, std::string const& name) {
    return m.dependencies.contains(name) || m.dev_dependencies.contains(name) ||
           m.path_deps.contains(name) || m.dev_path_deps.contains(name) ||
           m.workspace_deps.contains(name) || m.dev_workspace_deps.contains(name) ||
           m.vcpkg_deps.contains(name) || m.dev_vcpkg_deps.contains(name) ||
           m.subdir_deps.contains(name) || m.dev_subdir_deps.contains(name);
}

int cmd_add(int argc, char* argv[]) {
    auto args = cli::parse(argc, argv, 2, {
        cli::Flag{"--dev"}, cli::Flag{"--path"}, cli::Flag{"--workspace"}, cli::Flag{"--vcpkg"},
        cli::Option{"--git"}, cli::Option{"--subdir"}, cli::Option{"--version"},
        cli::Option{"--name"}, cli::ListOption{"--features"},
    });
    bool dev = args.has("--dev");
    bool is_path = args.has("--path");
    bool is_workspace_dep = args.has("--workspace");
    bool is_vcpkg = args.has("--vcpkg");
    bool is_git_subdir = !args.get("--git").empty();
    auto features = args.get_list("--features");
    auto git_repo = std::string{args.get("--git")};
    auto git_version = std::string{args.get("--version")};
    auto git_subdir = std::string{args.get("--subdir")};
    auto git_name = std::string{args.get("--name")};
    auto& positional = args.positional();

    if (!features.empty() && !is_vcpkg)
        return cli::error("--features is only valid with --vcpkg");

    int exclusive_count = int(is_path) + int(is_workspace_dep) + int(is_vcpkg) + int(is_git_subdir);
    if (exclusive_count > 1)
        return cli::error("--path, --workspace, --vcpkg, --git are mutually exclusive");

    if (!std::filesystem::exists("exon.toml")) {
        std::println(std::cerr, "error: exon.toml not found. run 'exon init' first");
        return 1;
    }

    auto content = read_file("exon.toml");
    auto m = manifest::load("exon.toml");

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
        auto target_dir = std::filesystem::current_path() / value;
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
        auto ws_root = manifest::find_workspace_root(std::filesystem::current_path());
        if (!ws_root) {
            std::println(std::cerr, "error: no workspace root found");
            return 1;
        }
        auto ws_m = manifest::load((*ws_root / "exon.toml").string());
        if (!manifest::resolve_workspace_member(*ws_root, ws_m, name)) {
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
                         "usage: exon add [--dev] <package> <version>\n"
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
        dep_line = std::format("\"{}\" = \"{}\"\n", name, value);
        display = std::format("{} {} = \"{}\"",
                              dev ? "dev-dependency" : "dependency", name, value);
    }

    // use the base name for duplicate check (for git deps, key is the full URL path)
    auto dup_key = name;
    if (dep_exists(m, dup_key)) {
        std::println(std::cerr, "error: '{}' is already a dependency", dup_key);
        return 1;
    }

    insert_into_section(content, section, dep_line);

    auto file = std::ofstream("exon.toml");
    file << content;
    std::println("added {}", display);
    return 0;
}

int cmd_remove(int argc, char* argv[]) {
    if (argc < 3) {
        std::println(std::cerr, "usage: exon remove <package>");
        return 1;
    }
    auto pkg = std::string{argv[2]};

    if (!std::filesystem::exists("exon.toml")) {
        std::println(std::cerr, "error: exon.toml not found. run 'exon init' first");
        return 1;
    }

    auto m = manifest::load("exon.toml");
    if (!dep_exists(m, pkg)) {
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

    auto content = read_file("exon.toml");
    // try quoted form first (git deps), then bare key (path/workspace deps)
    std::vector<std::string> candidates = {std::format("\"{}\"", pkg), std::format("{} =", pkg)};
    for (auto const& search : candidates) {
        auto pos = content.find(search);
        while (pos != std::string::npos) {
            // verify match starts at line start (after optional whitespace)
            auto line_start = content.rfind('\n', pos);
            line_start = (line_start == std::string::npos) ? 0 : line_start + 1;
            auto prefix = content.substr(line_start, pos - line_start);
            bool only_ws = std::all_of(prefix.begin(), prefix.end(),
                                        [](char c) { return c == ' ' || c == '\t'; });
            if (only_ws) {
                auto line_end = content.find('\n', pos);
                if (line_end != std::string::npos)
                    line_end += 1;
                else
                    line_end = content.size();
                content.erase(line_start, line_end - line_start);
                goto done;
            }
            pos = content.find(search, pos + 1);
        }
    }
done:

    // Drop subsection headers that have no entries after removing the dep,
    // e.g. `[dependencies.vcpkg]` that became empty. Leave the top-level
    // `[dependencies]` and `[package]` alone.
    {
        auto cleaned = std::string{};
        cleaned.reserve(content.size());
        std::size_t i = 0;
        while (i < content.size()) {
            auto line_end = content.find('\n', i);
            auto next = (line_end == std::string::npos) ? content.size() : line_end + 1;
            auto line = std::string_view{content}.substr(i, next - i);
            auto trimmed = line;
            while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r' ||
                                         trimmed.back() == ' ' || trimmed.back() == '\t'))
                trimmed.remove_suffix(1);
            while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t'))
                trimmed.remove_prefix(1);

            // Only strip subsection headers ([x.y] or [[x.y]]), never top-level ones.
            bool is_subsection = trimmed.starts_with("[") &&
                                 trimmed.ends_with("]") &&
                                 trimmed.find('.') != std::string_view::npos;
            if (is_subsection) {
                // Look ahead: is the next non-blank line another section header, or EOF?
                std::size_t j = next;
                bool empty_section = true;
                while (j < content.size()) {
                    auto je = content.find('\n', j);
                    auto jnext = (je == std::string::npos) ? content.size() : je + 1;
                    auto peek = std::string_view{content}.substr(j, jnext - j);
                    auto pt = peek;
                    while (!pt.empty() && (pt.back() == '\n' || pt.back() == '\r' ||
                                            pt.back() == ' ' || pt.back() == '\t'))
                        pt.remove_suffix(1);
                    while (!pt.empty() && (pt.front() == ' ' || pt.front() == '\t'))
                        pt.remove_prefix(1);
                    if (pt.empty() || pt.starts_with("#")) { j = jnext; continue; }
                    if (pt.starts_with("[")) break; // hit next section header
                    empty_section = false;
                    break;
                }
                if (empty_section) {
                    // drop header line; also drop a single trailing blank line if present
                    i = next;
                    if (i < content.size() && content[i] == '\n') ++i;
                    continue;
                }
            }
            cleaned.append(line);
            i = next;
        }
        content = std::move(cleaned);
    }

    auto file = std::ofstream("exon.toml");
    file << content;
    std::println("removed {}", pkg);

    auto lock_path = std::filesystem::current_path() / "exon.lock";
    if (std::filesystem::exists(lock_path)) {
        auto lf = lock::load(lock_path.string());
        auto& pkgs = lf.packages;
        std::erase_if(pkgs, [&](auto const& p) { return p.name == lock_name_to_erase; });
        lock::save(lf, lock_path.string());
    }
    return 0;
}

int cmd_update() {
    try {
        if (!std::filesystem::exists("exon.toml")) {
            std::println(std::cerr, "error: exon.toml not found. run 'exon init' first");
            return 1;
        }

        auto lock_path = std::filesystem::current_path() / "exon.lock";
        if (std::filesystem::exists(lock_path)) {
            std::filesystem::remove(lock_path);
        }

        auto m = load_manifest();
        if (m.dependencies.empty()) {
            std::println("no dependencies to update");
            return 0;
        }

        return build::run(m);
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
}

int cmd_sync() {
    try {
        auto raw_m = load_manifest();
        // for sync: merge ALL target sections for fetching, but keep raw for CMake generation
        auto fetch_m = manifest::resolve_all_targets(raw_m);
        if (manifest::is_workspace(raw_m)) {
            return run_for_workspace(raw_m, [](auto const&) {
                auto raw_member = manifest::load("exon.toml");
                auto fetch_member = manifest::resolve_all_targets(raw_member);
                auto lock_path = (std::filesystem::current_path() / "exon.lock").string();
                auto fetch_result = fetch::fetch_all(fetch_member, lock_path);
                build::sync_root_cmake(raw_member, fetch_result.deps);
                return 0;
            });
        }
        auto lock_path = (std::filesystem::current_path() / "exon.lock").string();
        auto fetch_result = fetch::fetch_all(fetch_m, lock_path);
        build::sync_root_cmake(raw_m, fetch_result.deps);
        return 0;
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
}

int cmd_fmt() {
    auto src_dir = std::filesystem::current_path() / "src";
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
    auto cmd = std::string{"clang-format -i"};
    for (auto const& f : files) {
        cmd += std::format(" {}", f);
    }

    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::println(std::cerr, "error: clang-format failed (is it installed?)");
        return 1;
    }
    std::println("formatted {} file(s)", files.size());
    return 0;
}

} // namespace commands
