import std;
import toml;
import manifest;
import build;

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::println("usage: exon <command>");
        return 1;
    }

    std::string_view command{argv[1]};

    if (command == "init") {
        auto file = std::ofstream("exon.toml");
        if (!file) {
            std::println(std::cerr, "error: failed to create exon.toml");
            return 1;
        }
        file << "[package]\n";
        file << "name = \"\"\n";
        file << "version = \"0.1.0\"\n";
        file << "description = \"\"\n";
        file << "authors = []\n";
        file << "license = \"\"\n";
        file << "standard = 23\n";
        file << "\n";
        file << "[dependencies]\n";
        std::println("created exon.toml");
    } else if (command == "info") {
        try {
            auto m = manifest::load("exon.toml");
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
        } catch (toml::ParseError const& e) {
            std::println(std::cerr, "error: {}", e.what());
            return 1;
        } catch (std::exception const& e) {
            std::println(std::cerr, "error: {}", e.what());
            return 1;
        }
    } else if (command == "build") {
        try {
            auto m = manifest::load("exon.toml");
            if (m.name.empty()) {
                std::println(std::cerr, "error: package name is required in exon.toml");
                return 1;
            }
            return build::run(m);
        } catch (toml::ParseError const& e) {
            std::println(std::cerr, "error: {}", e.what());
            return 1;
        } catch (std::exception const& e) {
            std::println(std::cerr, "error: {}", e.what());
            return 1;
        }
    } else {
        std::println(std::cerr, "unknown command: {}", command);
        return 1;
    }

    return 0;
}
