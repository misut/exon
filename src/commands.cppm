export module commands;
import std;
import toml;
import manifest;
import build;
import fetch;
import lock;
import templates;

export namespace commands {

#ifndef EXON_PKG_VERSION
#define EXON_PKG_VERSION "dev"
#endif
constexpr auto version = EXON_PKG_VERSION;

constexpr auto usage_text = R"(usage: exon <command> [args]

commands:
    init [--lib]           create a new exon.toml
    info                   show package information
    build [--release]      build the project
    check [--release]      check syntax without linking
    run [--release] [args] build and run the project
    test [--release]       build and run tests
    clean                  remove build artifacts
    add [--dev] <pkg> <ver>            add a git dependency
    add [--dev] --path <name> <path>   add a local path dependency
    add [--dev] --workspace <name>     add a workspace member dependency
    add [--dev] --vcpkg <name> <ver> [--features a,b,c]
                                       add a vcpkg dependency
    remove <pkg>                       remove a dependency
    update                 update dependencies to latest compatible versions
    sync                   sync CMakeLists.txt with exon.toml
    fmt                    format source files with clang-format
    version                show exon version
)";

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

std::string read_file(std::string_view path) {
    auto file = std::ifstream(std::string{path});
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

int cmd_version() {
    std::println("exon {}", version);
    return 0;
}

int cmd_init(int argc, char* argv[]) {
    if (std::filesystem::exists("exon.toml")) {
        std::println(std::cerr, "error: exon.toml already exists");
        return 1;
    }
    auto file = std::ofstream("exon.toml");
    if (!file) {
        std::println(std::cerr, "error: failed to create exon.toml");
        return 1;
    }
    bool is_lib = (argc >= 3 && std::string_view{argv[2]} == "--lib");
    file << (is_lib ? templates::exon_toml_lib : templates::exon_toml_bin);
    std::println("created exon.toml ({})", is_lib ? "lib" : "bin");
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

int cmd_build(int argc, char* argv[]) {
    try {
        auto m = load_manifest();
        bool release = (argc >= 3 && std::string_view{argv[2]} == "--release");
        if (manifest::is_workspace(m)) {
            return run_for_workspace(m, [release](auto const&) {
                auto member_m = manifest::load("exon.toml");
                return build::run(member_m, release);
            });
        }
        if (m.name.empty()) {
            std::println(std::cerr, "error: package name is required in exon.toml");
            return 1;
        }
        return build::run(m, release);
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
}

int cmd_check(int argc, char* argv[]) {
    try {
        auto m = load_manifest();
        bool release = (argc >= 3 && std::string_view{argv[2]} == "--release");
        if (manifest::is_workspace(m)) {
            return run_for_workspace(m, [release](auto const&) {
                auto member_m = manifest::load("exon.toml");
                return build::run_check(member_m, release);
            });
        }
        if (m.name.empty()) {
            std::println(std::cerr, "error: package name is required in exon.toml");
            return 1;
        }
        return build::run_check(m, release);
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
}

int cmd_run(int argc, char* argv[]) {
    try {
        auto m = load_manifest();
        if (m.name.empty()) {
            std::println(std::cerr, "error: package name is required in exon.toml");
            return 1;
        }
        if (m.type == "lib") {
            std::println(std::cerr, "error: cannot run a library package");
            return 1;
        }
        bool release = false;
        int args_start = 2;
        if (argc >= 3 && std::string_view{argv[2]} == "--release") {
            release = true;
            args_start = 3;
        }
        int rc = build::run(m, release);
        if (rc != 0)
            return rc;

        auto profile = release ? "release" : "debug";
        auto exe = std::filesystem::current_path() / ".exon" / profile / m.name;
        auto run_cmd = exe.string();
        for (int i = args_start; i < argc; ++i) {
            run_cmd += std::format(" {}", argv[i]);
        }
        std::println("running {}...\n", m.name);
        return std::system(run_cmd.c_str());
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
}

int cmd_test(int argc, char* argv[]) {
    try {
        auto m = load_manifest();
        bool release = (argc >= 3 && std::string_view{argv[2]} == "--release");
        if (manifest::is_workspace(m)) {
            return run_for_workspace(m, [release](auto const&) {
                auto member_m = manifest::load("exon.toml");
                return build::run_test(member_m, release);
            });
        }
        if (m.name.empty()) {
            std::println(std::cerr, "error: package name is required in exon.toml");
            return 1;
        }
        return build::run_test(m, release);
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
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
           m.vcpkg_deps.contains(name) || m.dev_vcpkg_deps.contains(name);
}

int cmd_add(int argc, char* argv[]) {
    bool dev = false;
    bool is_path = false;
    bool is_workspace_dep = false;
    bool is_vcpkg = false;
    std::vector<std::string> features;
    std::vector<std::string> positional;
    for (int i = 2; i < argc; ++i) {
        std::string_view a{argv[i]};
        if (a == "--dev")
            dev = true;
        else if (a == "--path")
            is_path = true;
        else if (a == "--workspace")
            is_workspace_dep = true;
        else if (a == "--vcpkg")
            is_vcpkg = true;
        else if (a == "--features") {
            if (i + 1 >= argc) {
                std::println(std::cerr, "error: --features requires a comma-separated list");
                return 1;
            }
            std::string_view list{argv[i + 1]};
            std::size_t start = 0;
            while (start < list.size()) {
                auto comma = list.find(',', start);
                auto end = (comma == std::string_view::npos) ? list.size() : comma;
                auto tok = list.substr(start, end - start);
                while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.front())))
                    tok.remove_prefix(1);
                while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.back())))
                    tok.remove_suffix(1);
                if (!tok.empty())
                    features.emplace_back(tok);
                start = end + 1;
            }
            ++i;
        } else if (a.starts_with("--")) {
            std::println(std::cerr, "error: unknown flag '{}'", a);
            return 1;
        } else {
            positional.emplace_back(a);
        }
    }
    if (!features.empty() && !is_vcpkg) {
        std::println(std::cerr, "error: --features is only valid with --vcpkg");
        return 1;
    }

    int exclusive_count = int(is_path) + int(is_workspace_dep) + int(is_vcpkg);
    if (exclusive_count > 1) {
        std::println(std::cerr, "error: --path, --workspace, --vcpkg are mutually exclusive");
        return 1;
    }

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
                         "       exon add [--dev] --vcpkg <name> <version>");
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

    auto file = std::ofstream("exon.toml");
    file << content;
    std::println("removed {}", pkg);

    auto lock_path = std::filesystem::current_path() / "exon.lock";
    if (std::filesystem::exists(lock_path)) {
        auto lf = lock::load(lock_path.string());
        auto& pkgs = lf.packages;
        std::erase_if(pkgs, [&](auto const& p) { return p.name == pkg; });
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
        auto m = load_manifest();
        if (manifest::is_workspace(m)) {
            return run_for_workspace(m, [](auto const&) {
                auto member_m = manifest::load("exon.toml");
                auto lock_path = (std::filesystem::current_path() / "exon.lock").string();
                auto fetch_result = fetch::fetch_all(member_m, lock_path);
                build::sync_root_cmake(member_m, fetch_result.deps);
                return 0;
            });
        }
        auto lock_path = (std::filesystem::current_path() / "exon.lock").string();
        auto fetch_result = fetch::fetch_all(m, lock_path);
        build::sync_root_cmake(m, fetch_result.deps);
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
