export module build;
import std;
import manifest;
import toolchain;
import fetch;

export namespace build {

namespace detail {

struct SourceFiles {
    std::vector<std::string> cpp;  // .cpp files
    std::vector<std::string> cppm; // .cppm module files
};

SourceFiles collect_sources(std::filesystem::path const& src_dir) {
    SourceFiles sf;
    if (!std::filesystem::exists(src_dir))
        return sf;
    for (auto const& entry : std::filesystem::directory_iterator(src_dir)) {
        if (!entry.is_regular_file())
            continue;
        auto ext = entry.path().extension();
        auto path = std::filesystem::canonical(entry.path()).string();
        if (ext == ".cpp")
            sf.cpp.push_back(path);
        else if (ext == ".cppm")
            sf.cppm.push_back(path);
    }
    std::ranges::sort(sf.cpp);
    std::ranges::sort(sf.cppm);
    return sf;
}

} // namespace detail

void generate_cmake(manifest::Manifest const& m, std::filesystem::path const& project_root,
                    std::filesystem::path const& output_dir,
                    std::vector<fetch::FetchedDep> const& deps, toolchain::Toolchain const& tc) {
    std::filesystem::create_directories(output_dir);

    auto cmake_path = output_dir / "CMakeLists.txt";
    auto file = std::ofstream(cmake_path);
    if (!file) {
        throw std::runtime_error(std::format("failed to create {}", cmake_path.string()));
    }

    bool import_std = (m.standard >= 23 && !tc.stdlib_modules_json.empty());

    if (import_std) {
        file << "cmake_minimum_required(VERSION 3.30)\n\n";
        file << std::format("set(CMAKE_CXX_STANDARD {})\n", m.standard);
        file << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n";
        file << "set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD \"451f2fe2-a8a2-47c3-bc32-94786d8fc91b\")\n";
        file << "set(CMAKE_CXX_MODULE_STD ON)\n";
        file << "add_compile_definitions(_LIBCPP_DISABLE_AVAILABILITY)\n\n";
        file << std::format("project({} LANGUAGES CXX)\n\n", m.name);
    } else {
        file << "cmake_minimum_required(VERSION 3.20)\n";
        file << std::format("project({} LANGUAGES CXX)\n\n", m.name);
        file << std::format("set(CMAKE_CXX_STANDARD {})\n", m.standard);
        file << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n\n";
    }

    // 의존성을 static library로 빌드
    for (auto const& dep : deps) {
        auto dep_src = dep.path / "src";
        auto dep_sf = detail::collect_sources(dep_src);

        if (dep_sf.cpp.empty() && dep_sf.cppm.empty()) {
            // CMakeLists.txt가 있으면 add_subdirectory 사용
            auto dep_cmake = dep.path / "CMakeLists.txt";
            if (std::filesystem::exists(dep_cmake)) {
                file << std::format("add_subdirectory({} {})\n\n",
                                    std::filesystem::canonical(dep.path).string(), dep.name);
                continue;
            }
            throw std::runtime_error(std::format(
                "dependency '{}' has no source files in src/ and no CMakeLists.txt", dep.name));
        }

        file << std::format("add_library({})\n", dep.name);
        if (!dep_sf.cpp.empty()) {
            file << std::format("target_sources({} PRIVATE", dep.name);
            for (auto const& src : dep_sf.cpp)
                file << std::format("\n    {}", src);
            file << "\n)\n";
        }
        if (!dep_sf.cppm.empty()) {
            file << std::format("target_sources({}\n    PUBLIC FILE_SET CXX_MODULES BASE_DIRS / FILES",
                                dep.name);
            for (auto const& src : dep_sf.cppm)
                file << std::format("\n    {}", src);
            file << "\n)\n";
        }

        auto include_dir = dep.path / "include";
        if (std::filesystem::exists(include_dir)) {
            file << std::format("target_include_directories({} PUBLIC {})\n", dep.name,
                                std::filesystem::canonical(include_dir).string());
        }

        // transitive: dep의 exon.toml에서 하위 의존성 읽어 링크
        auto dep_manifest_path = dep.path / "exon.toml";
        if (std::filesystem::exists(dep_manifest_path)) {
            auto dep_m = manifest::load(dep_manifest_path.string());
            if (!dep_m.dependencies.empty()) {
                file << std::format("target_link_libraries({} PUBLIC", dep.name);
                for (auto const& [sub_key, sub_ver] : dep_m.dependencies) {
                    auto sub_name = sub_key.substr(sub_key.rfind('/') + 1);
                    file << std::format("\n    {}", sub_name);
                }
                file << "\n)\n";
            }
        }
        file << "\n";
    }

    // 메인 프로젝트 소스
    auto src_dir = project_root / "src";
    auto sf = detail::collect_sources(src_dir);
    if (sf.cpp.empty() && sf.cppm.empty()) {
        throw std::runtime_error("no source files found in src/");
    }

    if (m.type == "lib") {
        file << std::format("add_library({})\n", m.name);
    } else {
        file << std::format("add_executable({})\n", m.name);
    }
    if (!sf.cpp.empty()) {
        file << std::format("target_sources({} PRIVATE", m.name);
        for (auto const& src : sf.cpp)
            file << std::format("\n    {}", src);
        file << "\n)\n";
    }
    if (!sf.cppm.empty()) {
        file << std::format("target_sources({}\n    PUBLIC FILE_SET CXX_MODULES BASE_DIRS / FILES", m.name);
        for (auto const& src : sf.cppm)
            file << std::format("\n    {}", src);
        file << "\n)\n";
    }

    // 라이브러리인 경우 include 디렉토리 공개
    if (m.type == "lib") {
        auto include_dir = project_root / "include";
        if (std::filesystem::exists(include_dir)) {
            file << std::format("target_include_directories({} PUBLIC {})\n", m.name,
                                std::filesystem::canonical(include_dir).string());
        }
    }

    // 의존성 링크
    if (!deps.empty()) {
        auto link_type = (m.type == "lib") ? "PUBLIC" : "PRIVATE";
        file << std::format("target_link_libraries({} {}", m.name, link_type);
        for (auto const& dep : deps) {
            file << std::format("\n    {}", dep.name);
        }
        file << "\n)\n";
    }
}

int run(manifest::Manifest const& m, bool release = false) {
    auto project_root = std::filesystem::current_path();
    auto exon_dir = project_root / ".exon";
    auto profile = release ? "release" : "debug";
    auto build_dir = exon_dir / profile;

    auto tc = toolchain::detect();

    // 의존성 패칭 + lock 파일
    auto lock_path = (project_root / "exon.lock").string();
    auto fetch_result = fetch::fetch_all(m, lock_path);

    generate_cmake(m, project_root, exon_dir, fetch_result.deps, tc);

    auto build_type = release ? "Release" : "Debug";
    auto configure_cmd = std::format("{} -B {} -S {} -G Ninja -DCMAKE_BUILD_TYPE={}", tc.cmake,
                                     build_dir.string(), exon_dir.string(), build_type);
    if (!tc.cxx_compiler.empty()) {
        configure_cmd += std::format(" -DCMAKE_CXX_COMPILER={}", tc.cxx_compiler);
    }
    if (!tc.stdlib_modules_json.empty() && m.standard >= 23) {
        configure_cmd += std::format(" -DCMAKE_CXX_STDLIB_MODULES_JSON={}", tc.stdlib_modules_json);
        auto llvm_root = std::filesystem::path{tc.cxx_compiler}.parent_path().parent_path();
        auto lib_cxx = llvm_root / "lib" / "c++";
        auto lib_unwind = llvm_root / "lib" / "unwind";
        configure_cmd += std::format(
            " -DCMAKE_EXE_LINKER_FLAGS=\"-L{} -lc++ -L{} -lunwind\"",
            lib_cxx.string(), lib_unwind.string());
    }

    std::println("configuring...");
    int rc = std::system(configure_cmd.c_str());
    if (rc != 0)
        return rc;

    auto build_cmd = std::format("{} --build {}", tc.cmake, build_dir.string());
    std::println("building...");
    rc = std::system(build_cmd.c_str());
    if (rc != 0)
        return rc;

    std::println("build succeeded: .exon/{}/{}", profile, m.name);
    return 0;
}

} // namespace build
