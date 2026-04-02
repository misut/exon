import std;
import toml;
import manifest;
import build;
import templates;

namespace {

constexpr auto usage_text = R"(usage: exon <command> [args]

commands:
    init          create a new exon.toml
    info          show package information
    build         build the project
    run [args]    build and run the project
    clean         remove build artifacts
    add <pkg> <ver>  add a dependency
)";

manifest::Manifest load_manifest() {
    if (!std::filesystem::exists("exon.toml")) {
        throw std::runtime_error("exon.toml not found. run 'exon init' first");
    }
    return manifest::load("exon.toml");
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::print("{}", usage_text);
        return 1;
    }

    std::string_view command{argv[1]};

    if (command == "init") {
        if (std::filesystem::exists("exon.toml")) {
            std::println(std::cerr, "error: exon.toml already exists");
            return 1;
        }
        auto file = std::ofstream("exon.toml");
        if (!file) {
            std::println(std::cerr, "error: failed to create exon.toml");
            return 1;
        }
        file << templates::exon_toml;
        std::println("created exon.toml");
    } else if (command == "info") {
        try {
            auto m = load_manifest();
            std::println("name: {}", m.name);
            std::println("version: {}", m.version);
            if (!m.description.empty())
                std::println("description: {}", m.description);
            if (!m.authors.empty()) {
                std::print("authors: ");
                for (std::size_t i = 0; i < m.authors.size(); ++i) {
                    if (i > 0) std::print(", ");
                    std::print("{}", m.authors[i]);
                }
                std::println("");
            }
            if (!m.license.empty())
                std::println("license: {}", m.license);
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
    } else if (command == "build") {
        try {
            auto m = load_manifest();
            if (m.name.empty()) {
                std::println(std::cerr, "error: package name is required in exon.toml");
                return 1;
            }
            return build::run(m);
        } catch (std::exception const& e) {
            std::println(std::cerr, "error: {}", e.what());
            return 1;
        }
    } else if (command == "run") {
        try {
            auto m = load_manifest();
            if (m.name.empty()) {
                std::println(std::cerr, "error: package name is required in exon.toml");
                return 1;
            }
            int rc = build::run(m);
            if (rc != 0) return rc;

            auto exe = std::filesystem::current_path() / ".exon" / "build" / m.name;
            auto run_cmd = exe.string();
            for (int i = 2; i < argc; ++i) {
                run_cmd += std::format(" {}", argv[i]);
            }
            std::println("running {}...\n", m.name);
            return std::system(run_cmd.c_str());
        } catch (std::exception const& e) {
            std::println(std::cerr, "error: {}", e.what());
            return 1;
        }
    } else if (command == "clean") {
        auto exon_dir = std::filesystem::current_path() / ".exon";
        if (std::filesystem::exists(exon_dir)) {
            std::filesystem::remove_all(exon_dir);
            std::println("cleaned .exon/");
        } else {
            std::println("nothing to clean");
        }
    } else if (command == "add") {
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

        // exon.toml 읽기
        auto content = std::string{};
        {
            auto file = std::ifstream("exon.toml");
            content = std::string{
                std::istreambuf_iterator<char>{file},
                std::istreambuf_iterator<char>{}
            };
        }

        // 이미 존재하는지 확인
        auto m = manifest::load("exon.toml");
        if (m.dependencies.contains(pkg)) {
            std::println(std::cerr, "error: '{}' is already a dependency", pkg);
            return 1;
        }

        // [dependencies] 섹션 끝에 추가
        auto dep_line = std::format("\"{}\" = \"{}\"\n", pkg, ver);
        auto deps_pos = content.find("[dependencies]");
        if (deps_pos == std::string::npos) {
            content += "\n[dependencies]\n" + dep_line;
        } else {
            // [dependencies] 다음 줄 끝에 추가
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
    } else {
        std::println(std::cerr, "unknown command: {}", command);
        std::print(std::cerr, "{}", usage_text);
        return 1;
    }

    return 0;
}
