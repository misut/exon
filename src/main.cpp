import std;

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
        std::println("created exon.toml");
    } else {
        std::println(std::cerr, "unknown command: {}", command);
        return 1;
    }

    return 0;
}
