import std;
import build;
import manifest;
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

    auto saved_cwd = std::filesystem::current_path();
    std::filesystem::current_path(proj.root);

    try {
        auto cmake = build::generate_portable_cmake(m, {});
        check(cmake.contains("function(exon_copy_windows_asan_runtime target)"),
              "portable CMake emits Windows ASan helper");
        check(cmake.contains("if(NOT WIN32)"),
              "portable CMake helper short-circuits on non-Windows");

        {
            auto file = std::ofstream{proj.root / "CMakeLists.txt"};
            file << cmake;
        }

        auto build_dir = proj.root / "build";
        auto cmd = std::format("cmake -S {} -B {}",
                               toolchain::shell_quote(proj.root.string()),
                               toolchain::shell_quote(build_dir.string()));
        auto rc = build::run_process(cmd);
        check(rc == 0, "portable Windows ASan helper configures on non-Windows");
    } catch (...) {
        std::filesystem::current_path(saved_cwd);
        throw;
    }

    std::filesystem::current_path(saved_cwd);
}
#endif

void test_run_process_returns_child_exit_code() {
#if defined(_WIN32)
    auto rc = build::run_process("cmd /c \"exit 7\"");
#else
    auto rc = build::run_process("sh -c 'exit 7'");
#endif
    check(rc == 7, "run_process returns child exit code");
}

int main() {
#if !defined(_WIN32)
    test_portable_windows_asan_helper_configures_on_non_windows();
#endif
    test_run_process_returns_child_exit_code();

    if (failures > 0) {
        std::println("{} test(s) failed", failures);
        return 1;
    }

    std::println("test_regressions: all passed");
    return 0;
}
