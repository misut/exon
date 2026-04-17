import std;
import build;
import build.system;
import core;
import fetch;
import fetch.system;
import manifest;
import manifest.system;
import toolchain;

int failures = 0;

void check(bool cond, std::string_view msg) {
    if (!cond) {
        std::println(std::cerr, "  FAIL: {}", msg);
        ++failures;
    }
}

struct TmpProject {
    std::filesystem::path root;

    TmpProject() {
        root = std::filesystem::temp_directory_path() / "exon_test_regressions";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "src");
        std::filesystem::create_directories(root / "tests");
    }

    ~TmpProject() { std::filesystem::remove_all(root); }

    void write(std::string const& rel_path, std::string const& content) {
        auto path = root / rel_path;
        std::filesystem::create_directories(path.parent_path());
        auto file = std::ofstream{path};
        file << content;
    }
};

struct TmpGitRepo {
    std::filesystem::path root;

    TmpGitRepo(std::filesystem::path const& parent_dir, std::string const& name) {
        root = parent_dir / name;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "src");
    }

    ~TmpGitRepo() { std::filesystem::remove_all(root); }

    void write(std::string const& rel_path, std::string const& content) {
        auto path = root / rel_path;
        std::filesystem::create_directories(path.parent_path());
        auto file = std::ofstream{path};
        file << content;
    }

    int git(std::string const& cmd) const {
#if defined(_WIN32)
        auto full = std::format("git -C \"{}\" {} >NUL 2>&1", root.generic_string(), cmd);
#else
        auto full = std::format("git -C \"{}\" {} >/dev/null 2>&1", root.generic_string(), cmd);
#endif
        return std::system(full.c_str());
    }

    void commit_and_tag(std::string const& tag) {
        git("add .");
        git(std::format("commit -q -m {}", tag));
        git(std::format("tag {}", tag));
    }

    void init_and_tag(std::string const& tag) {
        git("init -q");
        git("config user.email test@example.com");
        git("config user.name Test");
        commit_and_tag(tag);
    }

    std::string key() const { return root.generic_string(); }
};

toolchain::Toolchain make_tc() {
    toolchain::Toolchain tc;
    tc.cmake = "cmake";
    tc.ninja = "ninja";
    tc.cxx_compiler = "/usr/bin/clang++";
    tc.stdlib_modules_json = "/fake/libc++.modules.json";
    return tc;
}

#if !defined(_WIN32)
void test_portable_windows_asan_helper_configures_on_non_windows() {
    TmpProject proj;
    proj.write("src/main.cpp", "int main() { return 0; }\n");
    proj.write("tests/test_app.cpp", "int main() { return 0; }\n");

    manifest::Manifest m;
    m.name = "app";
    m.version = "0.1.0";
    m.standard = 20;
    m.type = "bin";
    m.build.cxxflags = {"/fsanitize=address"};

    try {
        auto cmake = build::generate_portable_cmake(m, proj.root, {});
        check(cmake.contains("function(exon_copy_windows_asan_runtime target)"),
              "portable CMake emits Windows ASan helper");
        check(cmake.contains("if(NOT WIN32)"),
              "portable CMake helper short-circuits on non-Windows");

        {
            auto file = std::ofstream{proj.root / "CMakeLists.txt"};
            file << cmake;
        }

        auto build_dir = proj.root / "build";
        core::ProcessSpec spec{
            .program = "cmake",
            .args = {"-S", proj.root.string(), "-B", build_dir.string()},
            .cwd = proj.root,
        };
        auto rc = build::system::run_process(spec);
        check(rc == 0, "portable Windows ASan helper configures on non-Windows");
    } catch (...) {
        throw;
    }
}
#endif

void test_run_process_returns_child_exit_code() {
#if defined(_WIN32)
    core::ProcessSpec spec{
        .program = "cmd",
        .args = {"/c", "exit 7"},
    };
#else
    core::ProcessSpec spec{
        .program = "sh",
        .args = {"-c", "exit 7"},
    };
#endif
    auto rc = build::system::run_process(spec);
    check(rc == 7, "run_process returns child exit code");
}

void test_system_run_process_honors_cwd() {
    TmpProject proj;
    proj.write("marker.txt", "ok\n");

#if defined(_WIN32)
    core::ProcessSpec spec{
        .program = "cmd",
        .args = {"/c", "if exist marker.txt (exit 0) else (exit 1)"},
        .cwd = proj.root,
    };
#else
    core::ProcessSpec spec{
        .program = "sh",
        .args = {"-c", "test -f marker.txt"},
        .cwd = proj.root,
    };
#endif

    auto rc = build::system::run_process(spec);
    check(rc == 0, "build.system::run_process honors ProcessSpec.cwd");
}

void test_fetch_and_generate_cmake_root_override_fixture() {
    TmpProject proj;
    proj.write("exon.toml", "");
    auto repos = proj.root / "repos";
    std::filesystem::create_directories(repos);

    TmpGitRepo cppx_repo{repos, "cppx"};
    cppx_repo.write("exon.toml", R"([package]
name = "cppx"
version = "1.0.3"
type = "lib"
standard = 23
)");
    cppx_repo.write("src/cppx.cppm", "export module cppx;");
    cppx_repo.init_and_tag("v1.0.3");
    cppx_repo.write("exon.toml", R"([package]
name = "cppx"
version = "1.2.0"
type = "lib"
standard = 23
)");
    cppx_repo.commit_and_tag("v1.2.0");

    TmpGitRepo txn_repo{repos, "txn"};
    txn_repo.write("exon.toml", std::format(R"([package]
name = "txn"
version = "0.6.1"
type = "lib"
standard = 23

[dependencies]
"{}" = "1.0.3"
)", cppx_repo.key()));
    txn_repo.write("src/txn.cppm", "export module txn;");
    txn_repo.init_and_tag("v0.6.1");

    proj.write("exon.toml", std::format(R"([package]
name = "app"
version = "0.1.0"
type = "bin"
standard = 23

[dependencies]
"{}" = "0.6.1"
"{}" = "1.2.0"
)", txn_repo.key(), cppx_repo.key()));
    proj.write("src/main.cpp", "int main() { return 0; }\n");

    auto app_manifest = manifest::system::load((proj.root / "exon.toml").string());
    auto fetch_result = fetch::system::fetch_all(
        proj.root, app_manifest, proj.root / "exon.lock", false);
    auto cmake = build::generate_cmake(app_manifest, proj.root, fetch_result.deps, make_tc());

    auto first = cmake.find("add_library(cppx)");
    check(first != std::string::npos, "override fixture: cppx target emitted");
    check(cmake.find("add_library(cppx)", first + 1) == std::string::npos,
          "override fixture: cppx target emitted once");
    check(cmake.contains("target_link_libraries(txn PUBLIC\n    cppx\n)"),
          "override fixture: txn links canonical cppx");
}

int main() {
#if !defined(_WIN32)
    test_portable_windows_asan_helper_configures_on_non_windows();
#endif
    test_run_process_returns_child_exit_code();
    test_system_run_process_honors_cwd();
    test_fetch_and_generate_cmake_root_override_fixture();

    if (failures > 0) {
        std::println("{} test(s) failed", failures);
        return 1;
    }

    std::println("test_regressions: all passed");
    return 0;
}
