import std;
import build;
import manifest;
import fetch;
import toolchain;

int failures = 0;

void check(bool cond, std::string_view msg) {
    if (!cond) {
        std::println(std::cerr, "  FAIL: {}", msg);
        ++failures;
    }
}

// create a minimal project structure in tmpdir
struct TmpProject {
    std::filesystem::path root;

    TmpProject() {
        root = std::filesystem::temp_directory_path() / "exon_test_build";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "src");
    }

    ~TmpProject() { std::filesystem::remove_all(root); }

    void write(std::string const& rel_path, std::string const& content) {
        auto path = root / rel_path;
        std::filesystem::create_directories(path.parent_path());
        auto f = std::ofstream{path};
        f << content;
    }
};

toolchain::Toolchain make_tc(bool with_modules = true) {
    toolchain::Toolchain tc;
    tc.cmake = "cmake";
    tc.ninja = "ninja";
    tc.cxx_compiler = "/usr/bin/clang++";
    if (with_modules)
        tc.stdlib_modules_json = "/fake/libc++.modules.json";
    return tc;
}

void test_basic_bin() {
    TmpProject proj;
    proj.write("src/main.cpp", "int main() {}");

    manifest::Manifest m;
    m.name = "hello";
    m.version = "1.0.0";
    m.type = "bin";
    m.standard = 23;

    auto cmake = build::generate_cmake(m, proj.root, {}, make_tc());

    check(cmake.contains("project(hello LANGUAGES CXX)"), "project name");
    check(cmake.contains("add_executable(hello)"), "add_executable");
    check(cmake.contains("main.cpp"), "main.cpp source");
    check(cmake.contains("CMAKE_CXX_STANDARD 23"), "standard 23");
    check(cmake.contains("EXON_PKG_NAME=\"hello\""), "built-in define name");
    check(cmake.contains("EXON_PKG_VERSION=\"1.0.0\""), "built-in define version");
}

void test_lib_with_modules() {
    TmpProject proj;
    proj.write("src/mylib.cppm", "export module mylib;");

    manifest::Manifest m;
    m.name = "mylib";
    m.version = "0.1.0";
    m.type = "lib";
    m.standard = 23;

    auto cmake = build::generate_cmake(m, proj.root, {}, make_tc());

    check(cmake.contains("add_library(mylib ALIAS mylib-modules)"), "lib alias");
    check(cmake.contains("mylib.cppm"), "module source");
    check(!cmake.contains("add_executable"), "no executable for lib");
}

void test_with_dependencies() {
    TmpProject proj;
    proj.write("src/main.cpp", "int main() {}");

    // create a fake dependency with a cppm file
    auto dep_path = std::filesystem::temp_directory_path() / "exon_test_dep";
    std::filesystem::remove_all(dep_path);
    std::filesystem::create_directories(dep_path / "src");
    {
        auto f = std::ofstream{dep_path / "src" / "dep.cppm"};
        f << "export module dep;";
    }

    manifest::Manifest m;
    m.name = "app";
    m.version = "1.0.0";
    m.type = "bin";
    m.standard = 23;

    std::vector<fetch::FetchedDep> deps = {{
        .key = "github.com/user/dep",
        .name = "dep",
        .version = "0.1.0",
        .commit = "abc123",
        .path = dep_path,
    }};

    auto cmake = build::generate_cmake(m, proj.root, deps, make_tc());

    check(cmake.contains("add_library(dep)"), "dep library");
    check(cmake.contains("dep.cppm"), "dep module source");

    std::filesystem::remove_all(dep_path);
}

void test_dev_deps_excluded_from_main() {
    TmpProject proj;
    proj.write("src/main.cpp", "int main() {}");
    proj.write("src/app.cppm", "export module app;");

    auto dep_path = std::filesystem::temp_directory_path() / "exon_test_devdep";
    std::filesystem::remove_all(dep_path);
    std::filesystem::create_directories(dep_path / "src");
    {
        auto f = std::ofstream{dep_path / "src" / "testlib.cppm"};
        f << "export module testlib;";
    }

    manifest::Manifest m;
    m.name = "app";
    m.version = "1.0.0";
    m.type = "bin";
    m.standard = 23;

    std::vector<fetch::FetchedDep> deps = {{
        .key = "github.com/user/testlib",
        .name = "testlib",
        .version = "0.1.0",
        .commit = "abc123",
        .path = dep_path,
        .is_dev = true,
    }};

    // without tests: dev dep should NOT be built at all
    auto cmake = build::generate_cmake(m, proj.root, deps, make_tc(), false);
    check(!cmake.contains("add_library(testlib)"), "dev dep not built without tests");
    check(!cmake.contains("testlib"), "no testlib reference without tests");

    // with tests: dev dep should be built and linked to test targets
    proj.write("tests/test_app.cpp", "int main() {}");
    auto cmake_test = build::generate_cmake(m, proj.root, deps, make_tc(), true);
    check(cmake_test.contains("add_library(testlib)"), "dev dep built with tests");
    // modules lib should NOT link to dev dep
    auto modules_link_pos = cmake_test.find("target_link_libraries(app-modules");
    check(modules_link_pos == std::string::npos, "modules should not link dev dep");
    // test target should link dev dep
    check(cmake_test.contains("target_link_libraries(test-test_app PRIVATE"), "test links");

    std::filesystem::remove_all(dep_path);
}

void test_with_tests() {
    TmpProject proj;
    proj.write("src/main.cpp", "int main() {}");
    proj.write("src/app.cppm", "export module app;");
    proj.write("tests/test_app.cpp", "int main() {}");

    manifest::Manifest m;
    m.name = "app";
    m.version = "1.0.0";
    m.type = "bin";
    m.standard = 23;

    auto cmake = build::generate_cmake(m, proj.root, {}, make_tc(), true);

    check(cmake.contains("add_executable(test-test_app)"), "test target");
    check(cmake.contains("test_app.cpp"), "test source");
    check(cmake.contains("target_link_libraries(test-test_app PRIVATE"), "test links");
}

void test_defines() {
    TmpProject proj;
    proj.write("src/main.cpp", "int main() {}");

    manifest::Manifest m;
    m.name = "app";
    m.version = "2.0.0";
    m.type = "bin";
    m.standard = 23;
    m.defines = {{"FEATURE_X", "1"}, {"APP_MODE", "production"}};
    m.defines_debug = {{"DEBUG_LOG", "1"}};
    m.defines_release = {{"NDEBUG", "1"}};

    // debug build
    auto cmake_debug = build::generate_cmake(m, proj.root, {}, make_tc(), false, false);
    check(cmake_debug.contains("FEATURE_X=\"1\""), "custom define FEATURE_X");
    check(cmake_debug.contains("APP_MODE=\"production\""), "custom define APP_MODE");
    check(cmake_debug.contains("DEBUG_LOG=\"1\""), "debug define");
    check(!cmake_debug.contains("NDEBUG"), "no release define in debug");

    // release build
    auto cmake_release = build::generate_cmake(m, proj.root, {}, make_tc(), false, true);
    check(cmake_release.contains("NDEBUG=\"1\""), "release define");
    check(!cmake_release.contains("DEBUG_LOG"), "no debug define in release");
}

void test_find_deps_bin() {
    TmpProject proj;
    proj.write("src/main.cpp", "int main() {}");

    manifest::Manifest m;
    m.name = "app";
    m.version = "1.0.0";
    m.type = "bin";
    m.standard = 23;
    m.find_deps = {{"Threads", "Threads::Threads"}, {"ZLIB", "ZLIB::ZLIB"}};

    auto cmake = build::generate_cmake(m, proj.root, {}, make_tc());

    check(cmake.contains("find_package(Threads REQUIRED)"), "find_package Threads");
    check(cmake.contains("find_package(ZLIB REQUIRED)"), "find_package ZLIB");
    // no modules, no git deps → targets linked on main executable
    check(cmake.contains("target_link_libraries(app PRIVATE"), "app links");
    check(cmake.contains("Threads::Threads"), "Threads target linked");
    check(cmake.contains("ZLIB::ZLIB"), "ZLIB target linked");
}

void test_find_deps_with_modules() {
    TmpProject proj;
    proj.write("src/main.cpp", "int main() {}");
    proj.write("src/app.cppm", "export module app;");

    manifest::Manifest m;
    m.name = "app";
    m.version = "1.0.0";
    m.type = "bin";
    m.standard = 23;
    m.find_deps = {{"Threads", "Threads::Threads"}};

    auto cmake = build::generate_cmake(m, proj.root, {}, make_tc());

    check(cmake.contains("find_package(Threads REQUIRED)"), "find_package Threads");
    // with modules: find_target links PUBLIC on modules lib (transitively applied to app)
    check(cmake.contains("target_link_libraries(app-modules PUBLIC"), "modules links");
    check(cmake.contains("Threads::Threads"), "Threads target linked");
}

void test_find_deps_multi_targets() {
    TmpProject proj;
    proj.write("src/main.cpp", "int main() {}");

    manifest::Manifest m;
    m.name = "app";
    m.version = "1.0.0";
    m.type = "bin";
    m.standard = 23;
    // one find_package can expose multiple targets
    m.find_deps = {{"fmt", "fmt::fmt fmt::fmt-header-only"}};

    auto cmake = build::generate_cmake(m, proj.root, {}, make_tc());

    check(cmake.contains("find_package(fmt REQUIRED)"), "find_package fmt");
    check(cmake.contains("fmt::fmt"), "fmt::fmt target");
    check(cmake.contains("fmt::fmt-header-only"), "fmt::fmt-header-only target");
}

void test_dev_find_deps() {
    TmpProject proj;
    proj.write("src/main.cpp", "int main() {}");
    proj.write("src/app.cppm", "export module app;");
    proj.write("tests/test_app.cpp", "int main() {}");

    manifest::Manifest m;
    m.name = "app";
    m.version = "1.0.0";
    m.type = "bin";
    m.standard = 23;
    m.dev_find_deps = {{"GTest", "GTest::gtest_main"}};

    // without tests: dev find_package should NOT be emitted
    auto cmake = build::generate_cmake(m, proj.root, {}, make_tc(), false);
    check(!cmake.contains("find_package(GTest"), "no dev find_package without tests");
    check(!cmake.contains("GTest::gtest_main"), "no dev target without tests");

    // with tests: dev find_package emitted, linked to test target only
    auto cmake_test = build::generate_cmake(m, proj.root, {}, make_tc(), true);
    check(cmake_test.contains("find_package(GTest REQUIRED)"), "dev find_package emitted");
    // modules should NOT link dev target
    auto modules_link_pos = cmake_test.find("target_link_libraries(app-modules");
    check(modules_link_pos == std::string::npos, "modules don't link dev find target");
    // test target links dev target
    check(cmake_test.contains("target_link_libraries(test-test_app PRIVATE"), "test links");
    check(cmake_test.contains("GTest::gtest_main"), "test links dev target");
}

void test_path_dep_in_generate_cmake() {
    TmpProject proj;
    proj.write("src/main.cpp", "int main() {}");

    // create a path dep directory
    auto dep_path = std::filesystem::temp_directory_path() / "exon_test_pathdep";
    std::filesystem::remove_all(dep_path);
    std::filesystem::create_directories(dep_path / "src");
    {
        auto f = std::ofstream{dep_path / "src" / "mylib.cppm"};
        f << "export module mylib;";
    }

    manifest::Manifest m;
    m.name = "app";
    m.version = "1.0.0";
    m.type = "bin";
    m.standard = 23;

    std::vector<fetch::FetchedDep> deps = {{
        .key = "mylib",
        .name = "mylib",
        .version = "",
        .commit = "",
        .path = dep_path,
        .is_dev = false,
        .is_path = true,
    }};

    auto cmake = build::generate_cmake(m, proj.root, deps, make_tc());

    // path deps reuse the existing add_library code path
    check(cmake.contains("add_library(mylib)"), "path dep: add_library emitted");
    check(cmake.contains("mylib.cppm"), "path dep: source file included");

    std::filesystem::remove_all(dep_path);
}

void test_no_import_std_below_23() {
    TmpProject proj;
    proj.write("src/main.cpp", "int main() {}");

    manifest::Manifest m;
    m.name = "legacy";
    m.version = "0.1.0";
    m.type = "bin";
    m.standard = 20;

    auto cmake = build::generate_cmake(m, proj.root, {}, make_tc());

    check(!cmake.contains("CMAKE_CXX_MODULE_STD"), "no module std for C++20");
    check(!cmake.contains("EXPERIMENTAL"), "no experimental for C++20");
    check(cmake.contains("CMAKE_CXX_STANDARD 20"), "standard 20");
}

int main() {
    test_basic_bin();
    test_lib_with_modules();
    test_with_dependencies();
    test_dev_deps_excluded_from_main();
    test_with_tests();
    test_defines();
    test_find_deps_bin();
    test_find_deps_with_modules();
    test_find_deps_multi_targets();
    test_dev_find_deps();
    test_path_dep_in_generate_cmake();
    test_no_import_std_below_23();

    if (failures > 0) {
        std::println("{} test(s) failed", failures);
        return 1;
    }
    std::println("test_build: all passed");
    return 0;
}
