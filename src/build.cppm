export module build;
import std;
import manifest;
import toolchain;
import fetch;

export namespace build {

namespace detail {

std::vector<std::string> collect_sources(std::filesystem::path const& src_dir) {
    std::vector<std::string> sources;
    if (!std::filesystem::exists(src_dir)) return sources;
    for (auto const& entry : std::filesystem::directory_iterator(src_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".cpp") {
            sources.push_back(std::filesystem::canonical(entry.path()).string());
        }
    }
    std::ranges::sort(sources);
    return sources;
}

} // namespace detail

void generate_cmake(manifest::Manifest const& m,
                    std::filesystem::path const& project_root,
                    std::filesystem::path const& output_dir,
                    std::vector<fetch::FetchedDep> const& deps) {
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

    // 의존성을 static library로 빌드
    for (auto const& dep : deps) {
        auto dep_src = dep.path / "src";
        auto dep_sources = detail::collect_sources(dep_src);

        if (dep_sources.empty()) {
            // CMakeLists.txt가 있으면 add_subdirectory 사용
            auto dep_cmake = dep.path / "CMakeLists.txt";
            if (std::filesystem::exists(dep_cmake)) {
                file << std::format("add_subdirectory({} {})\n\n",
                    std::filesystem::canonical(dep.path).string(),
                    dep.name);
                continue;
            }
            throw std::runtime_error(std::format(
                "dependency '{}' has no source files in src/ and no CMakeLists.txt", dep.name));
        }

        file << std::format("add_library({}", dep.name);
        for (auto const& src : dep_sources) {
            file << std::format("\n    {}", src);
        }
        file << "\n)\n";
        file << std::format("target_include_directories({} PUBLIC {})\n\n",
            dep.name, std::filesystem::canonical(dep.path / "include").string());
    }

    // 메인 프로젝트 소스
    auto src_dir = project_root / "src";
    auto sources = detail::collect_sources(src_dir);
    if (sources.empty()) {
        throw std::runtime_error("no .cpp files found in src/");
    }

    file << std::format("add_executable({}", m.name);
    for (auto const& src : sources) {
        file << std::format("\n    {}", src);
    }
    file << "\n)\n";

    // 의존성 링크
    if (!deps.empty()) {
        file << std::format("target_link_libraries({} PRIVATE", m.name);
        for (auto const& dep : deps) {
            file << std::format("\n    {}", dep.name);
        }
        file << "\n)\n";
    }
}

int run(manifest::Manifest const& m) {
    auto project_root = std::filesystem::current_path();
    auto exon_dir = project_root / ".exon";
    auto build_dir = exon_dir / "build";

    auto tc = toolchain::detect();

    // 의존성 패칭 + lock 파일
    auto lock_path = (project_root / "exon.lock").string();
    auto fetch_result = fetch::fetch_all(m, lock_path);

    generate_cmake(m, project_root, exon_dir, fetch_result.deps);

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
