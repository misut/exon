import std;
import build;
import core;
import manifest;
import fetch;
import toolchain;

#if defined(_WIN32)
extern "C" int _putenv(char const* envstring);
#else
extern "C" int setenv(char const* name, char const* value, int overwrite);
extern "C" int unsetenv(char const* name);
#endif

#if defined(_WIN32)
// Disable Windows crash dialogs so failures surface as exit codes instead of blocking UI.
extern "C" unsigned int __stdcall SetErrorMode(unsigned int);
extern "C" int _set_abort_behavior(unsigned int, unsigned int);
static int _crash_suppression = []() {
    SetErrorMode(0x0001u | 0x0002u);
    _set_abort_behavior(0, 0x1u | 0x4u);
    return 0;
}();
#endif

int failures = 0;

void check(bool cond, std::string_view msg) {
    if (!cond) {
        std::println(std::cerr, "  FAIL: {}", msg);
        ++failures;
    }
}

std::string command_text(core::ProcessSpec const& spec) {
    auto text = spec.program;
    for (auto const& arg : spec.args) {
        text += ' ';
        text += arg;
    }
    return text;
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

toolchain::Toolchain make_macos_tc(bool with_modules = true) {
    auto tc = make_tc(with_modules);
    tc.sysroot = "/fake/MacOSX.sdk";
    tc.lib_dir = "/fake/lib";
    tc.has_clang_config = true;
    return tc;
}

toolchain::Toolchain make_linux_tc(bool with_modules = true) {
    auto tc = make_tc(with_modules);
    tc.lib_dir = "/fake/lib";
    tc.has_clang_config = true;
    tc.needs_stdlib_flag = true;
    return tc;
}

void write_fake_exon_repo(TmpProject& proj) {
    proj.write("exon.toml", R"([package]
name = "exon"
version = "0.1.0"
type = "bin"
standard = 23
)");
    proj.write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.30)\n");
    proj.write("src/build.cppm", "export module build;\n");
    proj.write("src/toolchain.cppm", "export module toolchain;\n");
    proj.write("tests/test_build.cpp", "int main() { return 0; }\n");
    proj.write("build/exon", "");
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
    // defines are emitted via set_source_files_properties COMPILE_DEFINITIONS
    // where embedded quotes must be CMake-escaped (backslash-quote)
    check(cmake.contains("EXON_PKG_NAME=\\\"hello\\\""), "built-in define name");
    check(cmake.contains("EXON_PKG_VERSION=\\\"1.0.0\\\""), "built-in define version");
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

void test_git_dependency_features_filter_modules() {
    TmpProject proj;
    proj.write("src/main.cpp", "int main() {}\n");

    auto dep_path = std::filesystem::temp_directory_path() / "exon_test_featured_dep";
    std::filesystem::remove_all(dep_path);
    std::filesystem::create_directories(dep_path / "src");
    {
        auto f = std::ofstream{dep_path / "exon.toml"};
        f << R"([package]
name = "featlib"
version = "0.1.0"
type = "lib"
standard = 23

[features]
default = ["core"]
json = ["json"]
extra = ["json", "extra"]
)";
    }
    {
        auto f = std::ofstream{dep_path / "src" / "core.cppm"};
        f << "export module featlib.core;";
    }
    {
        auto f = std::ofstream{dep_path / "src" / "json.cppm"};
        f << "export module featlib.json;";
    }
    {
        auto f = std::ofstream{dep_path / "src" / "extra.cppm"};
        f << "export module featlib.extra;";
    }
    {
        auto f = std::ofstream{dep_path / "src" / "internal.cppm"};
        f << "export module featlib.internal;";
    }

    manifest::Manifest m;
    m.name = "app";
    m.version = "1.0.0";
    m.type = "bin";
    m.standard = 23;

    std::vector<fetch::FetchedDep> default_deps = {{
        .key = "github.com/user/featlib",
        .name = "featlib",
        .package_name = "featlib",
        .version = "0.1.0",
        .commit = "abc123",
        .path = dep_path,
    }};
    auto default_cmake = build::generate_cmake(m, proj.root, default_deps, make_tc());
    check(default_cmake.contains("core.cppm"), "git features: default module included");
    check(!default_cmake.contains("json.cppm"), "git features: unselected feature excluded");
    check(!default_cmake.contains("internal.cppm"), "git features: unlisted module excluded");

    auto selected_deps = default_deps;
    selected_deps[0].features = {"extra"};
    selected_deps[0].default_features = false;
    auto selected_cmake = build::generate_cmake(m, proj.root, selected_deps, make_tc());
    check(!selected_cmake.contains("core.cppm"),
          "git features: default module disabled");
    check(selected_cmake.contains("json.cppm"),
          "git features: nested feature module included");
    check(selected_cmake.contains("extra.cppm"),
          "git features: selected module included");
    check(!selected_cmake.contains("internal.cppm"),
          "git features: unlisted module still excluded");

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

void test_configure_command_macos_uses_system_runtime() {
    TmpProject proj;

    manifest::Manifest m;
    m.name = "app";
    m.version = "1.0.0";
    m.type = "bin";
    m.standard = 23;

    auto cmd =
        build::configure_command(make_macos_tc(), m, proj.root / "build", proj.root, true,
                                 {}, {}, {}, {}, {}, {}, {}, {}, true);
    auto text = command_text(cmd);

    check(text.contains("-DCMAKE_OSX_SYSROOT=/fake/MacOSX.sdk"), "macos: sysroot emitted");
    check(text.contains("-DCMAKE_CXX_STDLIB_MODULES_JSON=/fake/libc++.modules.json"),
          "macos: stdlib modules emitted");
    check(!text.contains("--no-default-config"), "macos: no no-default-config");
    check(!text.contains("-DCMAKE_CXX_FLAGS"), "macos: no manual cxx flags");
    check(text.contains("-DCMAKE_EXE_LINKER_FLAGS=-lc++ -lc++abi"),
          "macos: explicit system libc++ link");
    check(!text.contains("-L/fake/lib"), "macos: no llvm lib search path");
    check(!text.contains("-Wl,-rpath,/fake/lib"), "macos: no llvm runtime rpath");
}

void test_configure_command_linux_cmake_deps_keep_toolchain_runtime() {
    TmpProject proj;

    manifest::Manifest m;
    m.name = "app";
    m.version = "1.0.0";
    m.type = "bin";
    m.standard = 23;

    auto cmd =
        build::configure_command(make_linux_tc(), m, proj.root / "build", proj.root, false,
                                 {}, {}, {}, {}, {}, {}, {}, {}, true);
    auto text = command_text(cmd);

    check(text.contains("-DCMAKE_CXX_FLAGS=-stdlib=libc++"),
          "linux: compile uses libc++");
    check(text.contains("-DCMAKE_EXE_LINKER_FLAGS=-L/fake/lib -Wl,-rpath,/fake/lib -lc++abi"),
          "linux: cmake deps keep toolchain runtime");
}

void test_build_command_macos_import_std_serialized() {
    manifest::Manifest m;
    m.name = "app";
    m.version = "1.0.0";
    m.type = "bin";
    m.standard = 23;

    auto cmd = build::build_command(make_macos_tc(), m, "/tmp/exon-build");
    auto text = command_text(cmd);

    check(text.contains("cmake --build /tmp/exon-build"), "build cmd: base build");
    check(text.contains("--parallel 1"), "build cmd: macos import std serialized");
}

void test_build_command_macos_no_std_modules_keeps_default_parallelism() {
    manifest::Manifest m;
    m.name = "app";
    m.version = "1.0.0";
    m.type = "bin";
    m.standard = 23;

    auto cmd = build::build_command(make_macos_tc(false), m, "/tmp/exon-build");
    auto text = command_text(cmd);

    check(!text.contains("--parallel 1"), "build cmd: no std modules keeps default parallelism");
}

void test_build_command_wasm_keeps_default_parallelism() {
    manifest::Manifest m;
    m.name = "app";
    m.version = "1.0.0";
    m.type = "bin";
    m.standard = 23;

    auto cmd = build::build_command(make_macos_tc(), m, "/tmp/exon-build", "app", "wasm32-wasi");
    auto text = command_text(cmd);

    check(text.contains("--target app"), "build cmd: target emitted");
    check(!text.contains("--parallel 1"), "build cmd: wasm keeps default parallelism");
}

void test_plan_build_emits_write_and_steps() {
    TmpProject proj;
    proj.write("src/main.cpp", "int main() { return 0; }\n");

    manifest::Manifest m;
    m.name = "app";
    m.version = "1.0.0";
    m.type = "bin";
    m.standard = 23;

    build::BuildRequest request{
        .project = build::project_context(proj.root, true),
        .manifest = m,
        .toolchain = make_tc(),
        .release = true,
    };

    auto plan = build::plan_build(request);

    check(std::ranges::any_of(plan.writes, [&](auto const& write) {
              return write.text.path == request.project.exon_dir / "CMakeLists.txt";
          }),
          "plan build: emits exon cmake write");
    check(std::ranges::any_of(plan.writes, [&](auto const& write) {
              return write.text.path == proj.root / "CMakeLists.txt";
          }),
          "plan build: emits root cmake sync when enabled");
    check(plan.configure_steps.size() == 1, "plan build: emits configure step");
    check(plan.configure_steps.front().spec.cwd == proj.root, "plan build: configure cwd");
    check(plan.build_steps.size() == 1, "plan build: emits build step");
    check(plan.build_steps.front().spec.program == "cmake",
          "plan build: build command program emitted");
    check(std::ranges::find(plan.build_steps.front().spec.args, "--build") !=
              plan.build_steps.front().spec.args.end(),
          "plan build: build command emitted");
    check(plan.success_message == "build succeeded: .exon/release/app",
          "plan build: success message matches profile");
}

void test_plan_build_uses_host_manifest_for_exon_and_raw_manifest_for_root_sync() {
    TmpProject proj;
    proj.write("src/main.cpp", "int main() { return 0; }\n");

    manifest::Manifest raw_m;
    raw_m.name = "app";
    raw_m.version = "1.0.0";
    raw_m.type = "bin";
    raw_m.standard = 23;

    manifest::TargetSection ts_linux;
    ts_linux.predicate = "cfg(os = \"linux\")";
    ts_linux.build.cxxflags = {"-fsanitize=address,undefined"};
    raw_m.target_sections.push_back(std::move(ts_linux));

    manifest::TargetSection ts_windows;
    ts_windows.predicate = "cfg(os = \"windows\")";
    ts_windows.build.cxxflags = {"/fsanitize=address"};
    raw_m.target_sections.push_back(std::move(ts_windows));

    auto resolved_m = manifest::resolve_for_platform(
        raw_m, toolchain::make_platform("windows", "x86_64"));

    build::BuildRequest request{
        .project = build::project_context(proj.root),
        .manifest = resolved_m,
        .portable_manifest = raw_m,
        .toolchain = make_tc(),
        .portable_deps = {},
    };

    auto plan = build::plan_build(request);

    auto write_content = [&](std::filesystem::path const& path) -> std::string {
        auto it = std::ranges::find_if(plan.writes, [&](auto const& write) {
            return write.text.path == path;
        });
        if (it == plan.writes.end())
            return {};
        return it->text.content;
    };

    auto exon_cmake = write_content(request.project.exon_dir / "CMakeLists.txt");
    auto root_cmake = write_content(proj.root / "CMakeLists.txt");

    check(exon_cmake.contains("/fsanitize=address"),
          "plan build: exon cmake keeps host-resolved windows flag");
    check(!exon_cmake.contains("CMAKE_SYSTEM_NAME STREQUAL \"Linux\""),
          "plan build: exon cmake skips portable linux block");
    check(root_cmake.contains("if(CMAKE_SYSTEM_NAME STREQUAL \"Linux\")"),
          "plan build: root sync keeps linux conditional block");
    check(root_cmake.contains("if(WIN32)"),
          "plan build: root sync keeps windows conditional block");
    check(root_cmake.contains("-fsanitize=address,undefined"),
          "plan build: root sync keeps linux flag");
    check(root_cmake.contains("/fsanitize=address"),
          "plan build: root sync keeps windows flag");
    check(!root_cmake.contains("target_compile_options(app PRIVATE\n    /fsanitize=address\n)"),
          "plan build: root sync does not flatten host windows flag");
}

void test_plan_test_emits_filtered_targets_and_run_steps() {
    TmpProject proj;
    proj.write("src/main.cpp", "int main() { return 0; }\n");
    proj.write("tests/test_alpha.cpp", "int main() { return 0; }\n");
    proj.write("tests/test_beta.cpp", "int main() { return 0; }\n");

    manifest::Manifest m;
    m.name = "app";
    m.version = "1.0.0";
    m.type = "bin";
    m.standard = 23;

    build::BuildRequest request{
        .project = build::project_context(proj.root),
        .manifest = m,
        .toolchain = make_tc(),
        .with_tests = true,
        .build_targets = {"test-alpha"},
        .test_names = {"test-alpha"},
        .timeout = std::chrono::milliseconds{1500},
    };

    auto plan = build::plan_test(request);

    check(std::ranges::any_of(plan.writes, [&](auto const& write) {
              return write.text.path == request.project.exon_dir / "CMakeLists.txt";
          }),
          "plan test: emits exon cmake write");
    check(std::ranges::any_of(plan.writes, [&](auto const& write) {
              return write.text.path == proj.root / "CMakeLists.txt";
          }),
          "plan test: emits root cmake sync when enabled");
    check(plan.build_steps.size() == 1, "plan test: emits filtered build target");
    check(std::ranges::find(plan.build_steps.front().spec.args, "--target") !=
              plan.build_steps.front().spec.args.end(),
          "plan test: target flag emitted");
    check(std::ranges::find(plan.build_steps.front().spec.args, "test-alpha") !=
              plan.build_steps.front().spec.args.end(),
          "plan test: build target is preserved");
    check(plan.run_steps.size() == 1, "plan test: emits one run step");
    check(plan.run_steps.front().label == "test-alpha", "plan test: label matches test name");
    check(plan.run_steps.front().spec.timeout == std::chrono::milliseconds{1500},
          "plan test: timeout propagates");
}

void test_stale_self_host_bootstrap_message_for_macos_exon_repo() {
    TmpProject proj;
    write_fake_exon_repo(proj);

    auto executable = proj.root / "build" / "exon";
    auto newest_input = proj.root / "src" / "build.cppm";
    auto older_input = proj.root / "src" / "toolchain.cppm";
    auto manifest_path = proj.root / "exon.toml";
    auto base = std::filesystem::file_time_type::clock::now();

    std::filesystem::last_write_time(executable, base - std::chrono::hours{4});
    std::filesystem::last_write_time(newest_input, base - std::chrono::hours{1});
    std::filesystem::last_write_time(older_input, base - std::chrono::hours{2});
    std::filesystem::last_write_time(manifest_path, base - std::chrono::hours{3});

    manifest::Manifest m;
    m.name = "exon";

    auto message = build::stale_self_host_bootstrap_message(
        m, proj.root, executable, toolchain::make_platform("macos", "aarch64"));

    check(message.has_value(), "stale bootstrap: macOS exon repo is guarded");
    check(message && message->contains("src/build.cppm"),
          "stale bootstrap: newest input called out");
    check(message && message->contains("cmake --build build --target exon --parallel 1"),
          "stale bootstrap: rebuild command is documented");
}

void test_stale_self_host_bootstrap_message_skips_fresh_or_non_macos_cases() {
    TmpProject proj;
    write_fake_exon_repo(proj);

    auto executable = proj.root / "build" / "exon";
    auto base = std::filesystem::file_time_type::clock::now();

    std::filesystem::last_write_time(executable, base);
    std::filesystem::last_write_time(proj.root / "src" / "build.cppm", base - std::chrono::hours{1});
    std::filesystem::last_write_time(proj.root / "src" / "toolchain.cppm",
                                     base - std::chrono::hours{2});
    std::filesystem::last_write_time(proj.root / "exon.toml", base - std::chrono::hours{3});

    manifest::Manifest exon_repo;
    exon_repo.name = "exon";
    auto fresh_message = build::stale_self_host_bootstrap_message(
        exon_repo, proj.root, executable, toolchain::make_platform("macos", "aarch64"));
    check(!fresh_message.has_value(), "stale bootstrap: fresh bootstrap is allowed");

    std::filesystem::last_write_time(proj.root / "src" / "build.cppm", base + std::chrono::hours{1});
    auto linux_message = build::stale_self_host_bootstrap_message(
        exon_repo, proj.root, executable, toolchain::make_platform("linux", "x86_64"));
    check(!linux_message.has_value(), "stale bootstrap: non-macOS host is ignored");

    manifest::Manifest app_repo;
    app_repo.name = "app";
    auto app_message = build::stale_self_host_bootstrap_message(
        app_repo, proj.root, executable, toolchain::make_platform("macos", "aarch64"));
    check(!app_message.has_value(), "stale bootstrap: non-exon project is ignored");
}

void test_user_cxxflags_env_only() {
    // After v0.21.0, user_cxxflags / user_ldflags read ONLY the
    // EXON_CXXFLAGS / EXON_LDFLAGS env vars. Declarative `[build]`
    // flags now live in the generated CMakeLists.txt as
    // target_compile_options, so they propagate to raw cmake users
    // and add_subdirectory consumers — see test_generate_cmake_*
    // for the emission-side coverage.

    // Default state (no env var) → empty
#if defined(_WIN32)
    _putenv("EXON_CXXFLAGS=");
    _putenv("EXON_LDFLAGS=");
#else
    unsetenv("EXON_CXXFLAGS");
    unsetenv("EXON_LDFLAGS");
#endif
    check(build::user_cxxflags().empty(), "user_cxxflags: empty when env unset");
    check(build::user_ldflags().empty(), "user_ldflags: empty when env unset");

    // Set env vars → returned verbatim
#if defined(_WIN32)
    _putenv("EXON_CXXFLAGS=-fsanitize=address -g");
    _putenv("EXON_LDFLAGS=-fsanitize=address");
#else
    setenv("EXON_CXXFLAGS", "-fsanitize=address -g", 1);
    setenv("EXON_LDFLAGS", "-fsanitize=address", 1);
#endif
    check(build::user_cxxflags() == "-fsanitize=address -g",
          "user_cxxflags: returns env var contents");
    check(build::user_ldflags() == "-fsanitize=address",
          "user_ldflags: returns env var contents");

    // Cleanup
#if defined(_WIN32)
    _putenv("EXON_CXXFLAGS=");
    _putenv("EXON_LDFLAGS=");
#else
    unsetenv("EXON_CXXFLAGS");
    unsetenv("EXON_LDFLAGS");
#endif
}

void test_generate_cmake_base_build_flags() {
    // Base [build] cxxflags/ldflags should appear as
    // target_compile_options/target_link_options on the lib target.
    manifest::Manifest m;
    m.name = "myproj";
    m.version = "0.1.0";
    m.standard = 23;
    m.type = "lib";
    m.build.cxxflags = {"-Wall", "-Wextra"};
    m.build.ldflags = {"-Wl,--gc-sections"};

    auto temp = std::filesystem::temp_directory_path() / "exon_test_base_build";
    std::filesystem::create_directories(temp / "src");
    {
        auto f = std::ofstream(temp / "src" / "myproj.cppm");
        f << "export module myproj;\n";
    }
    try {
        toolchain::Toolchain tc;
        tc.native_import_std = true;
        auto cmake = build::generate_portable_cmake(m, temp, {});

        check(cmake.contains("if(PROJECT_IS_TOP_LEVEL)"),
              "base build flags wrapped in PROJECT_IS_TOP_LEVEL");
        check(cmake.contains("target_compile_options(myproj-modules PRIVATE"),
              "base build flags emit target_compile_options");
        check(cmake.contains("-Wall"), "base cxxflag -Wall present");
        check(cmake.contains("-Wextra"), "base cxxflag -Wextra present");
        check(cmake.contains("target_link_options(myproj-modules PRIVATE"),
              "base build flags emit target_link_options");
        check(cmake.contains("-Wl,--gc-sections"), "base ldflag present");
    } catch (...) {
        std::filesystem::remove_all(temp);
        throw;
    }
    std::filesystem::remove_all(temp);
}

void test_generate_cmake_bin_modules_build_flags_apply_to_exec() {
    TmpProject proj;
    proj.write("src/main.cpp", "int main() { return 0; }\n");
    proj.write("src/app.cppm", "export module app;\n");

    manifest::Manifest m;
    m.name = "app";
    m.version = "0.1.0";
    m.standard = 23;
    m.type = "bin";
    m.build.cxxflags = {"-Wall"};
    m.build.ldflags = {"-Wl,--gc-sections"};

    auto cmake = build::generate_cmake(m, proj.root, {}, make_tc());

    check(cmake.contains("target_compile_options(app-modules PRIVATE"),
          "modules lib gets compile flags");
    check(cmake.contains("target_compile_options(app PRIVATE"),
          "bin target gets compile flags");
    check(cmake.contains("target_link_options(app-modules PRIVATE"),
          "modules lib gets link flags");
    check(cmake.contains("target_link_options(app PRIVATE"),
          "bin target gets link flags");
}

void test_generate_portable_cmake_bin_modules_build_flags_apply_to_exec() {
    manifest::Manifest m;
    m.name = "app";
    m.version = "0.1.0";
    m.standard = 23;
    m.type = "bin";
    m.build.cxxflags = {"-Wall"};
    m.build.ldflags = {"-Wl,--gc-sections"};

    auto temp = std::filesystem::temp_directory_path() / "exon_test_bin_modules_build";
    std::filesystem::create_directories(temp / "src");
    {
        auto f = std::ofstream(temp / "src" / "main.cpp");
        f << "int main() { return 0; }\n";
    }
    {
        auto f = std::ofstream(temp / "src" / "app.cppm");
        f << "export module app;\n";
    }
    try {
        auto cmake = build::generate_portable_cmake(m, temp, {});

        check(cmake.contains("target_compile_options(app-modules PRIVATE"),
              "portable modules lib gets compile flags");
        check(cmake.contains("target_compile_options(app PRIVATE"),
              "portable bin target gets compile flags");
        check(cmake.contains("target_link_options(app-modules PRIVATE"),
              "portable modules lib gets link flags");
        check(cmake.contains("target_link_options(app PRIVATE"),
              "portable bin target gets link flags");
    } catch (...) {
        std::filesystem::remove_all(temp);
        throw;
    }
    std::filesystem::remove_all(temp);
}

void test_generate_cmake_subdir_dep_aliases_upstream_target() {
    TmpProject proj;
    proj.write("src/main.cpp", "int main() { return 0; }\n");

    auto dep_path = std::filesystem::temp_directory_path() / "exon_test_subdir_alias";
    std::filesystem::remove_all(dep_path);
    std::filesystem::create_directories(dep_path);
    {
        auto f = std::ofstream{dep_path / "CMakeLists.txt"};
        f << "add_library(member INTERFACE)\n";
    }

    manifest::Manifest m;
    m.name = "app";
    m.version = "0.1.0";
    m.standard = 23;
    m.type = "bin";

    std::vector<fetch::FetchedDep> deps = {{
        .key = "github.com/user/repo",
        .name = "member",
        .package_name = "member",
        .version = "0.1.0",
        .commit = "abc123",
        .path = dep_path,
        .subdir = "member",
        .aliases = {"gitmember"},
    }};

    auto cmake = build::generate_cmake(m, proj.root, deps, make_tc());

    check(cmake.contains("add_subdirectory("), "subdir alias: add_subdirectory emitted");
    check(cmake.contains("add_library(gitmember INTERFACE)"),
          "subdir alias: wrapper target emitted");
    check(cmake.contains("target_link_libraries(gitmember INTERFACE member)"),
          "subdir alias: wrapper links upstream target");

    std::filesystem::remove_all(dep_path);
}

void test_generate_portable_cmake_subdir_dep_aliases_upstream_target() {
    auto temp = std::filesystem::temp_directory_path() / "exon_test_portable_subdir_alias";
    std::filesystem::remove_all(temp);
    std::filesystem::create_directories(temp / "src");
    {
        auto f = std::ofstream{temp / "src" / "main.cpp"};
        f << "int main() { return 0; }\n";
    }

    manifest::Manifest m;
    m.name = "app";
    m.version = "0.1.0";
    m.standard = 23;
    m.type = "bin";

    std::vector<fetch::FetchedDep> deps = {{
        .key = "github.com/user/repo",
        .name = "member",
        .package_name = "member",
        .version = "0.1.0",
        .commit = "abc123",
        .path = temp / "_unused",
        .subdir = "member",
        .aliases = {"gitmember"},
    }};

    auto cmake = build::generate_portable_cmake(m, temp, deps);

    check(cmake.contains("FetchContent_Declare(member"),
          "portable subdir alias: FetchContent emitted for canonical target");
    check(cmake.contains("SOURCE_SUBDIR member"),
          "portable subdir alias: SOURCE_SUBDIR emitted");
    check(cmake.contains("add_library(gitmember INTERFACE)"),
          "portable subdir alias: wrapper target emitted");
    check(cmake.contains("target_link_libraries(gitmember INTERFACE member)"),
          "portable subdir alias: wrapper links upstream target");

    std::filesystem::remove_all(temp);
}

void test_generate_portable_cmake_guards_existing_dependency_targets() {
    auto temp = std::filesystem::temp_directory_path() / "exon_test_portable_target_guards";
    std::filesystem::remove_all(temp);
    std::filesystem::create_directories(temp / "src");
    std::filesystem::create_directories(temp / ".." / "core");
    {
        auto f = std::ofstream{temp / "src" / "main.cpp"};
        f << "int main() { return 0; }\n";
    }

    manifest::Manifest m;
    m.name = "app";
    m.version = "0.1.0";
    m.standard = 23;
    m.type = "bin";

    std::vector<fetch::FetchedDep> deps = {
        {
            .key = "core",
            .name = "core",
            .package_name = "core",
            .path = temp.parent_path() / "core",
            .is_path = true,
        },
        {
            .key = "github.com/user/repo",
            .name = "dep",
            .package_name = "dep",
            .version = "0.1.0",
            .commit = "abc123",
            .path = temp / "_unused",
        },
    };

    auto cmake = build::generate_portable_cmake(m, temp, deps);

    check(cmake.contains("if(NOT TARGET dep)\n    FetchContent_Declare(dep"),
          "portable guards: git dependency wrapped in target guard");
    check(cmake.contains("if(NOT TARGET core)\n    add_subdirectory("),
          "portable guards: path dependency wrapped in target guard");

    std::filesystem::remove_all(temp);
}

void test_generate_cmake_target_section_build() {
    // Per-target build flags should appear inside an if(<cmake-cond>)
    // block emitted from the existing target_sections loop.
    manifest::Manifest m;
    m.name = "myproj";
    m.version = "0.1.0";
    m.standard = 23;
    m.type = "lib";
    m.build.cxxflags = {"-Wall"};

    manifest::TargetSection ts_linux;
    ts_linux.predicate = "cfg(os = \"linux\")";
    ts_linux.build.cxxflags = {"-fsanitize=address,undefined"};
    ts_linux.build.ldflags = {"-fsanitize=address,undefined"};
    m.target_sections.push_back(std::move(ts_linux));

    manifest::TargetSection ts_windows;
    ts_windows.predicate = "cfg(os = \"windows\")";
    ts_windows.build.cxxflags = {"/fsanitize=address"};
    m.target_sections.push_back(std::move(ts_windows));

    auto temp = std::filesystem::temp_directory_path() / "exon_test_target_build";
    std::filesystem::create_directories(temp / "src");
    {
        auto f = std::ofstream(temp / "src" / "myproj.cppm");
        f << "export module myproj;\n";
    }
    try {
        auto cmake = build::generate_portable_cmake(m, temp, {});

        // Outer top-level guard wraps everything
        check(cmake.contains("if(PROJECT_IS_TOP_LEVEL)"),
              "outer PROJECT_IS_TOP_LEVEL guard present");

        // Base flag still emits at the top of the guarded block
        check(cmake.contains("-Wall"), "base -Wall present");

        // Linux block
        check(cmake.contains("if(CMAKE_SYSTEM_NAME STREQUAL \"Linux\")"),
              "Linux if() block present");
        check(cmake.contains("-fsanitize=address,undefined"),
              "Linux sanitizer cxxflag present");

        // Windows block
        check(cmake.contains("if(WIN32)"),
              "Windows if() block present");
        check(cmake.contains("/fsanitize=address"),
              "Windows ASan cxxflag present");
        check(cmake.contains("function(exon_copy_windows_asan_runtime target)"),
              "Windows ASan runtime helper emitted");
        check(cmake.contains("clang_rt.asan_dynamic-x86_64.dll"),
              "Windows ASan runtime dll referenced");

        // Verify nesting order: PROJECT_IS_TOP_LEVEL must come before the
        // first per-target if() block. Otherwise the per-target sections
        // would emit unguarded.
        auto top_pos = cmake.find("if(PROJECT_IS_TOP_LEVEL)");
        auto linux_pos = cmake.find("if(CMAKE_SYSTEM_NAME STREQUAL \"Linux\")");
        check(top_pos != std::string::npos && linux_pos != std::string::npos &&
                  top_pos < linux_pos,
              "PROJECT_IS_TOP_LEVEL guard precedes per-target if() blocks");
    } catch (...) {
        std::filesystem::remove_all(temp);
        throw;
    }
    std::filesystem::remove_all(temp);
}

void test_generate_cmake_windows_asan_runtime_copy_for_exec_and_tests() {
    TmpProject proj;
    proj.write("src/main.cpp", "int main() { return 0; }\n");
    proj.write("tests/test_app.cpp", "int main() { return 0; }\n");

    manifest::Manifest m;
    m.name = "app";
    m.version = "0.1.0";
    m.standard = 23;
    m.type = "bin";
    m.build.cxxflags = {"/fsanitize=address"};

    auto cmake = build::generate_cmake(m, proj.root, {}, make_tc(), true);

    check(cmake.contains("function(exon_copy_windows_asan_runtime target)"),
          "generate_cmake emits Windows ASan runtime helper");
    check(cmake.contains("exon_copy_windows_asan_runtime(app)"),
          "main executable gets Windows ASan runtime copy");
    check(cmake.contains("exon_copy_windows_asan_runtime(test-test_app)"),
          "test executable gets Windows ASan runtime copy");
    check(cmake.contains("copy_if_different"),
          "Windows ASan runtime copy command emitted");
}

void test_generate_cmake_target_section_build_profiles() {
    // [target.X.build.debug] / [target.X.build.release] should emit
    // generator expressions inside the if() block.
    manifest::Manifest m;
    m.name = "myproj";
    m.version = "0.1.0";
    m.standard = 23;
    m.type = "lib";

    manifest::TargetSection ts;
    ts.predicate = "cfg(os = \"linux\")";
    ts.build.cxxflags = {"-fsanitize=address"};
    ts.build_debug.cxxflags = {"-O0"};
    ts.build_release.cxxflags = {"-O3"};
    m.target_sections.push_back(std::move(ts));

    auto temp = std::filesystem::temp_directory_path() / "exon_test_target_profiles";
    std::filesystem::create_directories(temp / "src");
    {
        auto f = std::ofstream(temp / "src" / "myproj.cppm");
        f << "export module myproj;\n";
    }
    try {
        auto cmake = build::generate_portable_cmake(m, temp, {});
        check(cmake.contains("if(PROJECT_IS_TOP_LEVEL)"),
              "profile flags wrapped in PROJECT_IS_TOP_LEVEL");
        check(cmake.contains("$<$<CONFIG:Debug>:-O0>"),
              "debug profile uses CONFIG:Debug genex");
        check(cmake.contains("$<$<CONFIG:Release>:-O3>"),
              "release profile uses CONFIG:Release genex");
    } catch (...) {
        std::filesystem::remove_all(temp);
        throw;
    }
    std::filesystem::remove_all(temp);
}

void test_generate_cmake_no_build_flags_skipped() {
    // Backwards compat: a manifest without [build] or [target.X.build]
    // should not emit any target_compile_options block.
    manifest::Manifest m;
    m.name = "myproj";
    m.version = "0.1.0";
    m.standard = 23;
    m.type = "lib";

    auto temp = std::filesystem::temp_directory_path() / "exon_test_no_build";
    std::filesystem::create_directories(temp / "src");
    {
        auto f = std::ofstream(temp / "src" / "myproj.cppm");
        f << "export module myproj;\n";
    }
    try {
        auto cmake = build::generate_portable_cmake(m, temp, {});
        check(!cmake.contains("target_compile_options(myproj-modules PRIVATE"),
              "no target_compile_options when no build flags");
        check(!cmake.contains("target_link_options"),
              "no target_link_options when no build flags");
        check(!cmake.contains("if(PROJECT_IS_TOP_LEVEL)"),
              "no PROJECT_IS_TOP_LEVEL guard when there are no build flags");
        check(!cmake.contains("exon_copy_windows_asan_runtime"),
              "no Windows ASan helper when ASan is not enabled");
    } catch (...) {
        std::filesystem::remove_all(temp);
        throw;
    }
    std::filesystem::remove_all(temp);
}

// test: cmake_deps from a git dep are collected at root level and linked transitively
void test_transitive_cmake_deps_from_git() {
    TmpProject proj;
    proj.write("src/main.cpp", "int main() {}");

    auto dep_path = std::filesystem::temp_directory_path() / "exon_test_trans_cmake_git";
    std::filesystem::remove_all(dep_path);
    std::filesystem::create_directories(dep_path / "src");
    {
        auto f = std::ofstream{dep_path / "src" / "renderer.cppm"};
        f << "export module renderer;";
    }
    {
        auto f = std::ofstream{dep_path / "exon.toml"};
        f << R"([package]
name = "renderer"
version = "0.1.0"
type = "lib"
standard = 23

[dependencies.cmake.glfw]
git = "https://github.com/glfw/glfw.git"
tag = "3.4"
targets = "glfw"
)";
    }

    manifest::Manifest m;
    m.name = "app";
    m.version = "1.0.0";
    m.type = "bin";
    m.standard = 23;

    std::vector<fetch::FetchedDep> deps = {{
        .key = "github.com/user/renderer",
        .name = "renderer",
        .version = "0.1.0",
        .commit = "abc123",
        .path = dep_path,
    }};

    auto cmake = build::generate_cmake(m, proj.root, deps, make_tc());

    check(cmake.contains("FetchContent_Declare(glfw"),
          "trans cmake git: glfw FetchContent emitted");
    check(cmake.contains("target_link_libraries(renderer PUBLIC"),
          "trans cmake git: renderer links transitively");
    check(cmake.contains("glfw"),
          "trans cmake git: glfw target linked");

    std::filesystem::remove_all(dep_path);
}

// test: cmake_deps from a path dep are collected at root level
void test_transitive_cmake_deps_from_path() {
    TmpProject proj;
    proj.write("src/main.cpp", "int main() {}");

    auto dep_path = std::filesystem::temp_directory_path() / "exon_test_trans_cmake_path";
    std::filesystem::remove_all(dep_path);
    std::filesystem::create_directories(dep_path / "src");
    {
        auto f = std::ofstream{dep_path / "src" / "ui.cppm"};
        f << "export module ui;";
    }
    {
        auto f = std::ofstream{dep_path / "exon.toml"};
        f << R"([package]
name = "ui"
version = "0.1.0"
type = "lib"
standard = 23

[dependencies.cmake.glfw]
git = "https://github.com/glfw/glfw.git"
tag = "3.4"
targets = "glfw"
)";
    }

    manifest::Manifest m;
    m.name = "app";
    m.version = "1.0.0";
    m.type = "bin";
    m.standard = 23;

    std::vector<fetch::FetchedDep> deps = {{
        .key = "ui",
        .name = "ui",
        .path = dep_path,
        .is_path = true,
    }};

    auto cmake = build::generate_cmake(m, proj.root, deps, make_tc());

    check(cmake.contains("FetchContent_Declare(glfw"),
          "trans cmake path: glfw FetchContent emitted");
    check(cmake.contains("target_link_libraries(ui PUBLIC"),
          "trans cmake path: ui links transitively");

    std::filesystem::remove_all(dep_path);
}

// test: git dep's own git dependencies are linked transitively via target_link_libraries
void test_transitive_git_deps_linked() {
    TmpProject proj;
    proj.write("src/main.cpp", "int main() {}");

    // dep A depends on dep B
    auto dep_a_path = std::filesystem::temp_directory_path() / "exon_test_trans_git_a";
    auto dep_b_path = std::filesystem::temp_directory_path() / "exon_test_trans_git_b";
    std::filesystem::remove_all(dep_a_path);
    std::filesystem::remove_all(dep_b_path);
    std::filesystem::create_directories(dep_a_path / "src");
    std::filesystem::create_directories(dep_b_path / "src");
    {
        auto f = std::ofstream{dep_a_path / "src" / "A.cppm"};
        f << "export module A;";
    }
    {
        auto f = std::ofstream{dep_a_path / "exon.toml"};
        f << R"([package]
name = "A"
version = "0.1.0"
type = "lib"
standard = 23

[dependencies]
"github.com/user/B" = "0.1.0"
)";
    }
    {
        auto f = std::ofstream{dep_b_path / "src" / "B.cppm"};
        f << "export module B;";
    }

    manifest::Manifest m;
    m.name = "app";
    m.version = "1.0.0";
    m.type = "bin";
    m.standard = 23;

    // topological order: B first, then A
    std::vector<fetch::FetchedDep> deps = {
        {.key = "github.com/user/B", .name = "B", .version = "0.1.0",
         .commit = "bbb", .path = dep_b_path},
        {.key = "github.com/user/A", .name = "A", .version = "0.1.0",
         .commit = "aaa", .path = dep_a_path, .dependency_names = {"B"}},
    };

    auto cmake = build::generate_cmake(m, proj.root, deps, make_tc());

    check(cmake.contains("add_library(B)"), "trans git link: B built");
    check(cmake.contains("add_library(A)"), "trans git link: A built");
    check(cmake.contains("target_link_libraries(A PUBLIC\n    B\n)"),
          "trans git link: A links B");

    std::filesystem::remove_all(dep_a_path);
    std::filesystem::remove_all(dep_b_path);
}

void test_generate_cmake_uses_canonical_dependency_graph() {
    TmpProject proj;
    proj.write("src/main.cpp", "int main() { return 0; }\n");

    auto txn_path = std::filesystem::temp_directory_path() / "exon_test_canonical_txn";
    auto cppx_path = std::filesystem::temp_directory_path() / "exon_test_canonical_cppx";
    std::filesystem::remove_all(txn_path);
    std::filesystem::remove_all(cppx_path);
    std::filesystem::create_directories(txn_path / "src");
    std::filesystem::create_directories(cppx_path / "src");
    {
        auto f = std::ofstream{txn_path / "src" / "txn.cppm"};
        f << "export module txn;";
    }
    {
        auto f = std::ofstream{txn_path / "exon.toml"};
        f << R"([package]
name = "txn"
version = "0.6.1"
type = "lib"
standard = 23
)";
    }
    {
        auto f = std::ofstream{cppx_path / "src" / "cppx.cppm"};
        f << "export module cppx;";
    }
    {
        auto f = std::ofstream{cppx_path / "exon.toml"};
        f << R"([package]
name = "cppx"
version = "1.2.0"
type = "lib"
standard = 23
)";
    }

    manifest::Manifest m;
    m.name = "app";
    m.version = "1.0.0";
    m.type = "bin";
    m.standard = 23;

    std::vector<fetch::FetchedDep> deps = {
        {.key = "github.com/misut/cppx", .name = "cppx", .package_name = "cppx",
         .version = "1.2.0", .commit = "cppx120", .path = cppx_path},
        {.key = "github.com/misut/txn", .name = "txn", .package_name = "txn",
         .version = "0.6.1", .commit = "txn061", .path = txn_path,
         .dependency_names = {"cppx"}},
    };

    auto cmake = build::generate_cmake(m, proj.root, deps, make_tc());

    auto first = cmake.find("add_library(cppx)");
    check(first != std::string::npos, "canonical graph: cppx target emitted");
    check(cmake.find("add_library(cppx)", first + 1) == std::string::npos,
          "canonical graph: cppx target emitted once");
    check(cmake.contains("target_link_libraries(txn PUBLIC\n    cppx\n)"),
          "canonical graph: txn links canonical cppx target");

    std::filesystem::remove_all(txn_path);
    std::filesystem::remove_all(cppx_path);
}

void test_generate_cmake_duplicate_canonical_targets_fail_fast() {
    TmpProject proj;
    proj.write("src/main.cpp", "int main() { return 0; }\n");

    auto dep_a_path = std::filesystem::temp_directory_path() / "exon_test_dup_cppx_a";
    auto dep_b_path = std::filesystem::temp_directory_path() / "exon_test_dup_cppx_b";
    std::filesystem::remove_all(dep_a_path);
    std::filesystem::remove_all(dep_b_path);
    std::filesystem::create_directories(dep_a_path / "src");
    std::filesystem::create_directories(dep_b_path / "src");
    {
        auto f = std::ofstream{dep_a_path / "src" / "cppx.cppm"};
        f << "export module cppx;";
    }
    {
        auto f = std::ofstream{dep_b_path / "src" / "cppx.cppm"};
        f << "export module cppx;";
    }
    {
        auto f = std::ofstream{dep_a_path / "exon.toml"};
        f << R"([package]
name = "cppx"
version = "1.0.3"
type = "lib"
standard = 23
)";
    }
    {
        auto f = std::ofstream{dep_b_path / "exon.toml"};
        f << R"([package]
name = "cppx"
version = "1.2.0"
type = "lib"
standard = 23
)";
    }

    manifest::Manifest m;
    m.name = "app";
    m.version = "1.0.0";
    m.type = "bin";
    m.standard = 23;

    std::vector<fetch::FetchedDep> deps = {
        {.key = "github.com/misut/cppx", .name = "cppx", .package_name = "cppx",
         .version = "1.0.3", .commit = "cppx103", .path = dep_a_path},
        {.key = "github.com/misut/cppx", .name = "cppx", .package_name = "cppx",
         .version = "1.2.0", .commit = "cppx120", .path = dep_b_path},
    };

    bool threw = false;
    try {
        (void)build::generate_cmake(m, proj.root, deps, make_tc());
    } catch (...) {
        threw = true;
    }
    check(threw, "canonical graph: duplicate canonical deps fail fast");

    std::filesystem::remove_all(dep_a_path);
    std::filesystem::remove_all(dep_b_path);
}

// test: vcpkg_deps from a dependency are NOT propagated to root
void test_transitive_vcpkg_deps_not_propagated() {
    TmpProject proj;
    proj.write("src/main.cpp", "int main() {}");

    auto dep_path = std::filesystem::temp_directory_path() / "exon_test_trans_pkgdep";
    std::filesystem::remove_all(dep_path);
    std::filesystem::create_directories(dep_path / "src");
    {
        auto f = std::ofstream{dep_path / "src" / "dep.cppm"};
        f << "export module dep;";
    }
    {
        auto f = std::ofstream{dep_path / "exon.toml"};
        f << R"([package]
name = "dep"
version = "0.1.0"
type = "lib"
standard = 23

[dependencies.vcpkg]
fmt = "11.0.0"
)";
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
        .commit = "abc",
        .path = dep_path,
    }};

    auto cmake = build::generate_cmake(m, proj.root, deps, make_tc());

    // vcpkg deps from sub-dependency should NOT appear as find_package or
    // FetchContent in generated cmake (vcpkg setup happens outside generate_cmake)
    check(!cmake.contains("find_package(fmt"), "trans vcpkg: no find_package(fmt) in root cmake");
    check(!cmake.contains("FetchContent_Declare(fmt"),
          "trans vcpkg: no FetchContent(fmt) in root cmake");

    std::filesystem::remove_all(dep_path);
}

// test: find_deps from a dependency are NOT propagated to root
void test_transitive_find_deps_not_propagated() {
    TmpProject proj;
    proj.write("src/main.cpp", "int main() {}");

    auto dep_path = std::filesystem::temp_directory_path() / "exon_test_trans_find";
    std::filesystem::remove_all(dep_path);
    std::filesystem::create_directories(dep_path / "src");
    {
        auto f = std::ofstream{dep_path / "src" / "dep.cppm"};
        f << "export module dep;";
    }
    {
        auto f = std::ofstream{dep_path / "exon.toml"};
        f << R"([package]
name = "dep"
version = "0.1.0"
type = "lib"
standard = 23

[dependencies.find]
Threads = "Threads::Threads"
)";
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
        .commit = "abc",
        .path = dep_path,
    }};

    auto cmake = build::generate_cmake(m, proj.root, deps, make_tc());

    // find_deps from sub-dependency should NOT appear as find_package in root
    check(!cmake.contains("find_package(Threads"),
          "trans find: no find_package(Threads) in root cmake");

    std::filesystem::remove_all(dep_path);
}

int main() {
    auto run = [](std::string_view name, auto&& fn) {
        try {
            fn();
        } catch (std::exception const& e) {
            std::println(std::cerr, "  exception in {}: {}", name, e.what());
            ++failures;
        } catch (...) {
            std::println(std::cerr, "  unknown exception in {}", name);
            ++failures;
        }
    };

    run("test_basic_bin", test_basic_bin);
    run("test_lib_with_modules", test_lib_with_modules);
    run("test_with_dependencies", test_with_dependencies);
    run("test_git_dependency_features_filter_modules",
        test_git_dependency_features_filter_modules);
    run("test_dev_deps_excluded_from_main", test_dev_deps_excluded_from_main);
    run("test_with_tests", test_with_tests);
    run("test_defines", test_defines);
    run("test_find_deps_bin", test_find_deps_bin);
    run("test_find_deps_with_modules", test_find_deps_with_modules);
    run("test_find_deps_multi_targets", test_find_deps_multi_targets);
    run("test_dev_find_deps", test_dev_find_deps);
    run("test_path_dep_in_generate_cmake", test_path_dep_in_generate_cmake);
    run("test_no_import_std_below_23", test_no_import_std_below_23);
    run("test_configure_command_macos_uses_system_runtime",
        test_configure_command_macos_uses_system_runtime);
    run("test_configure_command_linux_cmake_deps_keep_toolchain_runtime",
        test_configure_command_linux_cmake_deps_keep_toolchain_runtime);
    run("test_build_command_macos_import_std_serialized",
        test_build_command_macos_import_std_serialized);
    run("test_build_command_macos_no_std_modules_keeps_default_parallelism",
        test_build_command_macos_no_std_modules_keeps_default_parallelism);
    run("test_build_command_wasm_keeps_default_parallelism",
        test_build_command_wasm_keeps_default_parallelism);
    run("test_plan_build_emits_write_and_steps", test_plan_build_emits_write_and_steps);
    run("test_plan_build_uses_host_manifest_for_exon_and_raw_manifest_for_root_sync",
        test_plan_build_uses_host_manifest_for_exon_and_raw_manifest_for_root_sync);
    run("test_plan_test_emits_filtered_targets_and_run_steps",
        test_plan_test_emits_filtered_targets_and_run_steps);
    run("test_stale_self_host_bootstrap_message_for_macos_exon_repo",
        test_stale_self_host_bootstrap_message_for_macos_exon_repo);
    run("test_stale_self_host_bootstrap_message_skips_fresh_or_non_macos_cases",
        test_stale_self_host_bootstrap_message_skips_fresh_or_non_macos_cases);
    run("test_user_cxxflags_env_only", test_user_cxxflags_env_only);
    run("test_generate_cmake_base_build_flags", test_generate_cmake_base_build_flags);
    run("test_generate_cmake_bin_modules_build_flags_apply_to_exec",
        test_generate_cmake_bin_modules_build_flags_apply_to_exec);
    run("test_generate_portable_cmake_bin_modules_build_flags_apply_to_exec",
        test_generate_portable_cmake_bin_modules_build_flags_apply_to_exec);
    run("test_generate_cmake_subdir_dep_aliases_upstream_target",
        test_generate_cmake_subdir_dep_aliases_upstream_target);
    run("test_generate_portable_cmake_subdir_dep_aliases_upstream_target",
        test_generate_portable_cmake_subdir_dep_aliases_upstream_target);
    run("test_generate_portable_cmake_guards_existing_dependency_targets",
        test_generate_portable_cmake_guards_existing_dependency_targets);
    run("test_generate_cmake_target_section_build",
        test_generate_cmake_target_section_build);
    run("test_generate_cmake_windows_asan_runtime_copy_for_exec_and_tests",
        test_generate_cmake_windows_asan_runtime_copy_for_exec_and_tests);
    run("test_generate_cmake_target_section_build_profiles",
        test_generate_cmake_target_section_build_profiles);
    run("test_generate_cmake_no_build_flags_skipped",
        test_generate_cmake_no_build_flags_skipped);
    run("test_transitive_cmake_deps_from_git", test_transitive_cmake_deps_from_git);
    run("test_transitive_cmake_deps_from_path", test_transitive_cmake_deps_from_path);
    run("test_transitive_git_deps_linked", test_transitive_git_deps_linked);
    run("test_generate_cmake_uses_canonical_dependency_graph",
        test_generate_cmake_uses_canonical_dependency_graph);
    run("test_generate_cmake_duplicate_canonical_targets_fail_fast",
        test_generate_cmake_duplicate_canonical_targets_fail_fast);
    run("test_transitive_vcpkg_deps_not_propagated",
        test_transitive_vcpkg_deps_not_propagated);
    run("test_transitive_find_deps_not_propagated",
        test_transitive_find_deps_not_propagated);

    if (failures > 0) {
        std::println("{} test(s) failed", failures);
        return 1;
    }
    std::println("test_build: all passed");
    return 0;
}
