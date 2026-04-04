export module commands;
import std;
import toml;
import manifest;
import build;
import lock;
import templates;

export namespace commands {

constexpr auto version = "0.3.0";

constexpr auto usage_text = R"(usage: exon <command> [args]

commands:
    init [--lib]           create a new exon.toml
    info                   show package information
    build [--release]      build the project
    run [--release] [args] build and run the project
    test [--release]       build and run tests
    clean                  remove build artifacts
    add <pkg> <ver>        add a dependency
    remove <pkg>           remove a dependency
    update                 update dependencies to latest compatible versions
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

int cmd_add(int argc, char* argv[]) {
    if (argc < 4) {
        std::println(std::cerr, "usage: exon add <package> <version>");
        return 1;
    }
    auto pkg = std::string{argv[2]};
    auto ver = std::string{argv[3]};

    if (!std::filesystem::exists("exon.toml")) {
        std::println(std::cerr, "error: exon.toml not found. run 'exon init' first");
        return 1;
    }

    auto content = read_file("exon.toml");
    auto m = manifest::load("exon.toml");
    if (m.dependencies.contains(pkg)) {
        std::println(std::cerr, "error: '{}' is already a dependency", pkg);
        return 1;
    }

    auto dep_line = std::format("\"{}\" = \"{}\"\n", pkg, ver);
    auto deps_pos = content.find("[dependencies]");
    if (deps_pos == std::string::npos) {
        content += "\n[dependencies]\n" + dep_line;
    } else {
        auto insert_pos = content.find('\n', deps_pos);
        if (insert_pos != std::string::npos) {
            content.insert(insert_pos + 1, dep_line);
        } else {
            content += "\n" + dep_line;
        }
    }

    auto file = std::ofstream("exon.toml");
    file << content;
    std::println("added {} = \"{}\"", pkg, ver);
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
    if (!m.dependencies.contains(pkg)) {
        std::println(std::cerr, "error: '{}' is not a dependency", pkg);
        return 1;
    }

    auto content = read_file("exon.toml");
    auto search = std::format("\"{}\"", pkg);
    auto pos = content.find(search);
    if (pos != std::string::npos) {
        auto line_start = content.rfind('\n', pos);
        line_start = (line_start == std::string::npos) ? 0 : line_start + 1;
        auto line_end = content.find('\n', pos);
        if (line_end != std::string::npos)
            line_end += 1;
        else
            line_end = content.size();
        content.erase(line_start, line_end - line_start);
    }

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
