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

std::string configure_cmd(toolchain::Toolchain const& tc, manifest::Manifest const& m,
                          std::filesystem::path const& build_dir,
                          std::filesystem::path const& source_dir, bool release) {
    auto build_type = release ? "Release" : "Debug";
    auto cmd = std::format("{} -B {} -S {} -G Ninja -DCMAKE_BUILD_TYPE={}", tc.cmake,
                           build_dir.string(), source_dir.string(), build_type);
    if (!tc.cxx_compiler.empty())
        cmd += std::format(" -DCMAKE_CXX_COMPILER={}", tc.cxx_compiler);
    if (!tc.sysroot.empty())
        cmd += std::format(" -DCMAKE_OSX_SYSROOT={}", tc.sysroot);
    if (tc.needs_stdlib_flag)
        cmd += " -DCMAKE_CXX_FLAGS=\"-stdlib=libc++\"";
    if (!tc.stdlib_modules_json.empty() && m.standard >= 23) {
        cmd += std::format(" -DCMAKE_CXX_STDLIB_MODULES_JSON={}", tc.stdlib_modules_json);
        if (!tc.has_clang_config && !tc.lib_dir.empty()) {
            auto linker_flags = std::format("-L{0} -Wl,-rpath,{0} -lc++ -lc++abi", tc.lib_dir);
            if (tc.needs_stdlib_flag)
                linker_flags += " -stdlib=libc++";
            cmd += std::format(" -DCMAKE_EXE_LINKER_FLAGS=\"{}\"", linker_flags);
        }
    }
    return cmd;
}

} // namespace detail

void generate_cmake(manifest::Manifest const& m, std::filesystem::path const& project_root,
                    std::filesystem::path const& output_dir,
                    std::vector<fetch::FetchedDep> const& deps, toolchain::Toolchain const& tc,
                    bool with_tests = false) {
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
        file << std::format("project({} LANGUAGES CXX)\n\n", m.name);
    } else {
        file << "cmake_minimum_required(VERSION 3.20)\n";
        file << std::format("project({} LANGUAGES CXX)\n\n", m.name);
        file << std::format("set(CMAKE_CXX_STANDARD {})\n", m.standard);
        file << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n\n";
    }

    // build dependencies as static libraries
    for (auto const& dep : deps) {
        auto dep_src = dep.path / "src";
        auto dep_sf = detail::collect_sources(dep_src);

        if (dep_sf.cpp.empty() && dep_sf.cppm.empty()) {
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
            file << std::format(
                "target_sources({}\n    PUBLIC FILE_SET CXX_MODULES BASE_DIRS / FILES", dep.name);
            for (auto const& src : dep_sf.cppm)
                file << std::format("\n    {}", src);
            file << "\n)\n";
        }

        auto include_dir = dep.path / "include";
        if (std::filesystem::exists(include_dir)) {
            file << std::format("target_include_directories({} PUBLIC {})\n", dep.name,
                                std::filesystem::canonical(include_dir).string());
        }

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

    // main project sources
    auto src_dir = project_root / "src";
    auto sf = detail::collect_sources(src_dir);
    if (sf.cpp.empty() && sf.cppm.empty()) {
        throw std::runtime_error("no source files found in src/");
    }

    // create shared module library if cppm files exist (shared between main and tests)
    auto modules_lib = std::format("{}-modules", m.name);
    bool has_modules = !sf.cppm.empty();

    if (has_modules) {
        file << std::format("add_library({})\n", modules_lib);
        file << std::format(
            "target_sources({}\n    PUBLIC FILE_SET CXX_MODULES BASE_DIRS / FILES", modules_lib);
        for (auto const& src : sf.cppm)
            file << std::format("\n    {}", src);
        file << "\n)\n";

        if (!deps.empty()) {
            file << std::format("target_link_libraries({} PUBLIC", modules_lib);
            for (auto const& dep : deps)
                file << std::format("\n    {}", dep.name);
            file << "\n)\n";
        }

        if (m.type == "lib") {
            auto include_dir = project_root / "include";
            if (std::filesystem::exists(include_dir)) {
                file << std::format("target_include_directories({} PUBLIC {})\n", modules_lib,
                                    std::filesystem::canonical(include_dir).string());
            }
        }
        file << "\n";
    }

    // メインタゲット
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

    if (has_modules) {
        auto link_type = (m.type == "lib") ? "PUBLIC" : "PRIVATE";
        file << std::format("target_link_libraries({} {} {})\n", m.name, link_type, modules_lib);
    } else if (!deps.empty()) {
        auto link_type = (m.type == "lib") ? "PUBLIC" : "PRIVATE";
        file << std::format("target_link_libraries({} {}", m.name, link_type);
        for (auto const& dep : deps)
            file << std::format("\n    {}", dep.name);
        file << "\n)\n";
    }

    // テストタゲット
    if (with_tests) {
        auto tests_dir = project_root / "tests";
        auto test_sf = detail::collect_sources(tests_dir);

        for (auto const& test_cpp : test_sf.cpp) {
            auto test_stem = std::filesystem::path{test_cpp}.stem().string();
            auto test_name = std::format("test-{}", test_stem);

            file << std::format("\nadd_executable({})\n", test_name);
            file << std::format("target_sources({} PRIVATE\n    {}\n)\n", test_name, test_cpp);

            if (has_modules) {
                file << std::format("target_link_libraries({} PRIVATE {})\n", test_name,
                                    modules_lib);
            } else if (!deps.empty()) {
                file << std::format("target_link_libraries({} PRIVATE", test_name);
                for (auto const& dep : deps)
                    file << std::format("\n    {}", dep.name);
                file << "\n)\n";
            }
        }
    }
}

int run(manifest::Manifest const& m, bool release = false) {
    auto project_root = std::filesystem::current_path();
    auto exon_dir = project_root / ".exon";
    auto profile = release ? "release" : "debug";
    auto build_dir = exon_dir / profile;

    auto tc = toolchain::detect();

    auto lock_path = (project_root / "exon.lock").string();
    auto fetch_result = fetch::fetch_all(m, lock_path);

    generate_cmake(m, project_root, exon_dir, fetch_result.deps, tc);

    std::println("configuring...");
    int rc = std::system(detail::configure_cmd(tc, m, build_dir, exon_dir, release).c_str());
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

int run_test(manifest::Manifest const& m, bool release = false) {
    auto project_root = std::filesystem::current_path();
    auto tests_dir = project_root / "tests";

    if (!std::filesystem::exists(tests_dir)) {
        std::println(std::cerr, "error: tests/ directory not found");
        return 1;
    }

    auto test_sf = detail::collect_sources(tests_dir);
    if (test_sf.cpp.empty()) {
        std::println(std::cerr, "error: no test files found in tests/");
        return 1;
    }

    auto exon_dir = project_root / ".exon";
    auto profile = release ? "release" : "debug";
    auto build_dir = exon_dir / profile;

    auto tc = toolchain::detect();

    auto lock_path = (project_root / "exon.lock").string();
    auto fetch_result = fetch::fetch_all(m, lock_path);

    generate_cmake(m, project_root, exon_dir, fetch_result.deps, tc, true);

    std::println("configuring...");
    int rc = std::system(detail::configure_cmd(tc, m, build_dir, exon_dir, release).c_str());
    if (rc != 0)
        return rc;

    // テスト名を収集
    std::vector<std::string> test_names;
    for (auto const& test_cpp : test_sf.cpp) {
        test_names.push_back(
            std::format("test-{}", std::filesystem::path{test_cpp}.stem().string()));
    }

    // ビルド
    std::println("building tests...");
    for (auto const& name : test_names) {
        auto build_cmd =
            std::format("{} --build {} --target {}", tc.cmake, build_dir.string(), name);
        rc = std::system(build_cmd.c_str());
        if (rc != 0)
            return rc;
    }

    // 実行
    std::println("running tests...\n");
    int passed = 0;
    int failed = 0;
    for (auto const& name : test_names) {
        auto exe = build_dir / name;
        rc = std::system(exe.string().c_str());
        if (rc == 0) {
            std::println("  {} ... ok", name);
            ++passed;
        } else {
            std::println("  {} ... FAILED", name);
            ++failed;
        }
    }

    std::println("");
    if (failed > 0) {
        std::println("{} passed, {} failed", passed, failed);
        return 1;
    }
    std::println("all {} tests passed", passed);
    return 0;
}

} // namespace build
