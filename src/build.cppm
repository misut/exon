export module build;
import std;
import manifest;
import toolchain;

export namespace build {

void generate_cmake(manifest::Manifest const& m, std::filesystem::path const& project_root,
                    std::filesystem::path const& output_dir) {
    std::filesystem::create_directories(output_dir);

    auto cmake_path = output_dir / "CMakeLists.txt";
    auto file = std::ofstream(cmake_path);
    if (!file) {
        throw std::runtime_error(std::format("failed to create {}", cmake_path.string()));
    }

    file << "cmake_minimum_required(VERSION 3.20)\n";
    file << std::format("project({} LANGUAGES CXX)\n\n", m.name);
    file << std::format("set(CMAKE_CXX_STANDARD {})\n", m.standard);
    file << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n\n";

    auto src_dir = project_root / "src";
    std::vector<std::string> sources;
    if (std::filesystem::exists(src_dir)) {
        for (auto const& entry : std::filesystem::directory_iterator(src_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".cpp") {
                sources.push_back(std::filesystem::canonical(entry.path()).string());
            }
        }
    }
    std::ranges::sort(sources);

    if (sources.empty()) {
        throw std::runtime_error("no .cpp files found in src/");
    }

    file << std::format("add_executable({}",  m.name);
    for (auto const& src : sources) {
        file << std::format("\n    {}", src);
    }
    file << "\n)\n";
}

int run(manifest::Manifest const& m) {
    auto project_root = std::filesystem::current_path();
    auto exon_dir = project_root / ".exon";
    auto build_dir = exon_dir / "build";

    auto tc = toolchain::detect();

    generate_cmake(m, project_root, exon_dir);

    auto configure_cmd = std::format("{} -B {} -S {} -G Ninja",
        tc.cmake, build_dir.string(), exon_dir.string());
    if (!tc.cxx_compiler.empty()) {
        configure_cmd += std::format(" -DCMAKE_CXX_COMPILER={}", tc.cxx_compiler);
    }

    std::println("configuring...");
    int rc = std::system(configure_cmd.c_str());
    if (rc != 0) return rc;

    auto build_cmd = std::format("{} --build {}", tc.cmake, build_dir.string());
    std::println("building...");
    rc = std::system(build_cmd.c_str());
    if (rc != 0) return rc;

    std::println("build succeeded: .exon/build/{}", m.name);
    return 0;
}

} // namespace build
