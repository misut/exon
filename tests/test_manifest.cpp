import std;
import toml;
import manifest;

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

void test_basic_manifest() {
    auto input = R"(
[package]
name = "hello"
version = "1.0.0"
description = "A test project"
authors = ["alice", "bob"]
license = "MIT"
type = "bin"
standard = 23

[dependencies]
"github.com/user/repo" = "0.1.0"
)";

    auto table = toml::parse(input);
    auto m = manifest::from_toml(table);

    check(m.name == "hello", "name");
    check(m.version == "1.0.0", "version");
    check(m.description == "A test project", "description");
    check(m.authors.size() == 2, "authors count");
    check(m.authors[0] == "alice", "authors[0]");
    check(m.authors[1] == "bob", "authors[1]");
    check(m.license == "MIT", "license");
    check(m.type == "bin", "type");
    check(m.standard == 23, "standard");
    check(m.dependencies.size() == 1, "deps count");
    check(m.dependencies.contains("github.com/user/repo"), "dep key");
    check(m.dependencies.at("github.com/user/repo") == "0.1.0", "dep version");
}

void test_minimal_manifest() {
    auto input = R"(
[package]
name = "minimal"
version = "0.1.0"
)";

    auto table = toml::parse(input);
    auto m = manifest::from_toml(table);

    check(m.name == "minimal", "minimal name");
    check(m.version == "0.1.0", "minimal version");
    check(m.type == "bin", "default type is bin");
    check(m.standard == 23, "default standard is 23");
    check(m.dependencies.empty(), "no deps");
}

void test_lib_type() {
    auto input = R"(
[package]
name = "mylib"
version = "0.1.0"
type = "lib"
standard = 20
)";

    auto table = toml::parse(input);
    auto m = manifest::from_toml(table);

    check(m.type == "lib", "lib type");
    check(m.standard == 20, "standard 20");
}

void test_workspace() {
    auto input = R"(
[workspace]
members = ["packages/app", "packages/lib"]
)";

    auto table = toml::parse(input);
    auto m = manifest::from_toml(table);

    check(manifest::is_workspace(m), "is workspace");
    check(m.workspace_members.size() == 2, "workspace members count");
    check(m.workspace_members[0] == "packages/app", "workspace member[0]");
    check(m.workspace_members[1] == "packages/lib", "workspace member[1]");
}

void test_non_workspace() {
    auto input = R"(
[package]
name = "app"
version = "0.1.0"
)";

    auto table = toml::parse(input);
    auto m = manifest::from_toml(table);

    check(!manifest::is_workspace(m), "not a workspace");
    check(m.workspace_members.empty(), "no workspace members");
}

void test_dev_dependencies() {
    auto input = R"(
[package]
name = "app"
version = "1.0.0"

[dependencies]
"github.com/user/lib" = "0.1.0"

[dev-dependencies]
"github.com/user/testlib" = "0.2.0"
)";

    auto table = toml::parse(input);
    auto m = manifest::from_toml(table);

    check(m.dependencies.size() == 1, "dev-deps: 1 regular dep");
    check(m.dev_dependencies.size() == 1, "dev-deps: 1 dev dep");
    check(m.dev_dependencies.contains("github.com/user/testlib"), "dev-deps: key");
    check(m.dev_dependencies.at("github.com/user/testlib") == "0.2.0", "dev-deps: version");
}

void test_defines() {
    auto input = R"(
[package]
name = "app"
version = "1.0.0"

[defines]
FEATURE_X = "1"
APP_NAME = "myapp"

[defines.debug]
DEBUG_MODE = "1"

[defines.release]
NDEBUG = "1"
)";

    auto table = toml::parse(input);
    auto m = manifest::from_toml(table);

    check(m.defines.size() == 2, "defines: 2 entries");
    check(m.defines.at("FEATURE_X") == "1", "defines: FEATURE_X");
    check(m.defines.at("APP_NAME") == "myapp", "defines: APP_NAME");
    check(m.defines_debug.size() == 1, "defines.debug: 1 entry");
    check(m.defines_debug.at("DEBUG_MODE") == "1", "defines.debug: DEBUG_MODE");
    check(m.defines_release.size() == 1, "defines.release: 1 entry");
    check(m.defines_release.at("NDEBUG") == "1", "defines.release: NDEBUG");
}

void test_find_deps() {
    auto input = R"(
[package]
name = "app"
version = "1.0.0"

[dependencies]
"github.com/user/lib" = "0.1.0"

[dependencies.find]
Threads = "Threads::Threads"
ZLIB = "ZLIB::ZLIB"

[dev-dependencies]
"github.com/user/testlib" = "0.2.0"

[dev-dependencies.find]
GTest = "GTest::gtest_main"
)";

    auto table = toml::parse(input);
    auto m = manifest::from_toml(table);

    check(m.dependencies.size() == 1, "find: 1 regular git dep");
    check(m.dependencies.contains("github.com/user/lib"), "find: git dep key");
    check(m.find_deps.size() == 2, "find: 2 find deps");
    check(m.find_deps.at("Threads") == "Threads::Threads", "find: Threads target");
    check(m.find_deps.at("ZLIB") == "ZLIB::ZLIB", "find: ZLIB target");
    check(m.dev_dependencies.size() == 1, "find: 1 regular dev dep");
    check(m.dev_find_deps.size() == 1, "find: 1 dev find dep");
    check(m.dev_find_deps.at("GTest") == "GTest::gtest_main", "find: GTest dev target");
}

void test_find_deps_only() {
    auto input = R"(
[package]
name = "app"
version = "1.0.0"

[dependencies.find]
Threads = "Threads::Threads"
)";

    auto table = toml::parse(input);
    auto m = manifest::from_toml(table);

    check(m.dependencies.empty(), "find-only: no git deps");
    check(m.find_deps.size() == 1, "find-only: 1 find dep");
    check(m.find_deps.at("Threads") == "Threads::Threads", "find-only: Threads");
}

void test_path_deps() {
    auto input = R"(
[package]
name = "app"
version = "1.0.0"

[dependencies]
"github.com/user/lib" = "0.1.0"

[dependencies.path]
my-lib = "../my-lib"
shared = "packages/shared"

[dependencies.workspace]
core = true
utils = true

[dev-dependencies.path]
testlib = "../testlib"

[dev-dependencies.workspace]
mocks = true
)";

    auto table = toml::parse(input);
    auto m = manifest::from_toml(table);

    check(m.dependencies.size() == 1, "path: 1 git dep");
    check(m.path_deps.size() == 2, "path: 2 path deps");
    check(m.path_deps.at("my-lib") == "../my-lib", "path: my-lib path");
    check(m.path_deps.at("shared") == "packages/shared", "path: shared path");
    check(m.workspace_deps.size() == 2, "path: 2 workspace deps");
    check(m.workspace_deps.contains("core"), "path: workspace core");
    check(m.workspace_deps.contains("utils"), "path: workspace utils");
    check(m.dev_path_deps.size() == 1, "path: 1 dev path dep");
    check(m.dev_path_deps.at("testlib") == "../testlib", "path: dev testlib");
    check(m.dev_workspace_deps.size() == 1, "path: 1 dev workspace dep");
    check(m.dev_workspace_deps.contains("mocks"), "path: dev workspace mocks");
}

void test_workspace_deps_false_ignored() {
    auto input = R"(
[package]
name = "app"
version = "1.0.0"

[dependencies.workspace]
yes = true
no = false
)";

    auto table = toml::parse(input);
    auto m = manifest::from_toml(table);

    check(m.workspace_deps.size() == 1, "ws: only true entries");
    check(m.workspace_deps.contains("yes"), "ws: true kept");
    check(!m.workspace_deps.contains("no"), "ws: false skipped");
}

void test_find_workspace_root() {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "exon_test_ws_root";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "packages" / "app");

    // workspace root
    {
        auto f = std::ofstream{tmp / "exon.toml"};
        f << "[workspace]\nmembers = [\"packages/app\"]\n";
    }
    // member
    {
        auto f = std::ofstream{tmp / "packages" / "app" / "exon.toml"};
        f << "[package]\nname = \"app\"\nversion = \"0.1.0\"\n";
    }

    auto root = manifest::find_workspace_root(tmp / "packages" / "app");
    check(root.has_value(), "find_workspace_root: found");
    if (root)
        check(fs::weakly_canonical(*root) == fs::weakly_canonical(tmp),
              "find_workspace_root: correct root");

    // not in a workspace
    auto other = fs::temp_directory_path() / "exon_test_no_ws";
    fs::remove_all(other);
    fs::create_directories(other);
    auto no_root = manifest::find_workspace_root(other);
    check(!no_root.has_value(), "find_workspace_root: no ws");

    fs::remove_all(tmp);
    fs::remove_all(other);
}

void test_resolve_workspace_member() {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "exon_test_resolve_ws";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "pkg-a");
    fs::create_directories(tmp / "pkg-b");

    manifest::Manifest ws;
    ws.workspace_members = {"pkg-a", "pkg-b"};

    // member a: package name "alpha"
    {
        auto f = std::ofstream{tmp / "pkg-a" / "exon.toml"};
        f << "[package]\nname = \"alpha\"\nversion = \"0.1.0\"\n";
    }
    // member b: package name "beta"
    {
        auto f = std::ofstream{tmp / "pkg-b" / "exon.toml"};
        f << "[package]\nname = \"beta\"\nversion = \"0.1.0\"\n";
    }

    auto alpha = manifest::resolve_workspace_member(tmp, ws, "alpha");
    check(alpha.has_value() && alpha->filename() == "pkg-a", "resolve: alpha -> pkg-a");

    auto beta = manifest::resolve_workspace_member(tmp, ws, "beta");
    check(beta.has_value() && beta->filename() == "pkg-b", "resolve: beta -> pkg-b");

    auto missing = manifest::resolve_workspace_member(tmp, ws, "gamma");
    check(!missing.has_value(), "resolve: missing returns nullopt");

    fs::remove_all(tmp);
}

void test_vcpkg_deps() {
    auto input = R"(
[package]
name = "app"
version = "1.0.0"

[dependencies.vcpkg]
fmt = "11.0.0"
zlib = "*"

[dev-dependencies.vcpkg]
gtest = "*"
benchmark = "1.9.0"
)";

    auto table = toml::parse(input);
    auto m = manifest::from_toml(table);

    check(m.vcpkg_deps.size() == 2, "vcpkg: 2 regular deps");
    check(m.vcpkg_deps.at("fmt").version == "11.0.0", "vcpkg: fmt version");
    check(m.vcpkg_deps.at("fmt").features.empty(), "vcpkg: fmt no features");
    check(m.vcpkg_deps.at("zlib").version == "*", "vcpkg: zlib wildcard");
    check(m.dev_vcpkg_deps.size() == 2, "vcpkg: 2 dev deps");
    check(m.dev_vcpkg_deps.at("gtest").version == "*", "vcpkg: dev gtest");
    check(m.dev_vcpkg_deps.at("benchmark").version == "1.9.0", "vcpkg: dev benchmark version");
}

void test_vcpkg_deps_with_features() {
    auto input = R"(
[package]
name = "app"
version = "1.0.0"

[dependencies.vcpkg]
zlib = "*"
fmt = { version = "11.0.0", features = ["xchar"] }
boost-asio = { version = "*", features = ["ssl"] }
opencv = { features = ["contrib", "cuda"] }
empty = { version = "1.0.0", features = [] }
)";

    auto table = toml::parse(input);
    auto m = manifest::from_toml(table);

    check(m.vcpkg_deps.size() == 5, "vcpkg features: 5 deps");
    check(m.vcpkg_deps.at("zlib").version == "*", "vcpkg features: zlib string form");
    check(m.vcpkg_deps.at("zlib").features.empty(), "vcpkg features: zlib no features");
    check(m.vcpkg_deps.at("fmt").version == "11.0.0", "vcpkg features: fmt version");
    check(m.vcpkg_deps.at("fmt").features.size() == 1, "vcpkg features: fmt one feature");
    check(m.vcpkg_deps.at("fmt").features[0] == "xchar", "vcpkg features: fmt feature name");
    check(m.vcpkg_deps.at("boost-asio").version == "*", "vcpkg features: boost-asio version");
    check(m.vcpkg_deps.at("boost-asio").features.size() == 1, "vcpkg features: boost-asio features");
    check(m.vcpkg_deps.at("opencv").version.empty(), "vcpkg features: opencv version omitted");
    check(m.vcpkg_deps.at("opencv").features.size() == 2, "vcpkg features: opencv 2 features");
    check(m.vcpkg_deps.at("opencv").features[0] == "contrib", "vcpkg features: opencv[0]");
    check(m.vcpkg_deps.at("opencv").features[1] == "cuda", "vcpkg features: opencv[1]");
    check(m.vcpkg_deps.at("empty").version == "1.0.0", "vcpkg features: empty features ok");
    check(m.vcpkg_deps.at("empty").features.empty(), "vcpkg features: empty array");
}

void test_subdir_deps() {
    auto input = R"(
[package]
name = "app"
version = "1.0.0"

[dependencies]
"github.com/user/lib" = "0.1.0"
refl = { git = "github.com/misut/txn", version = "0.1.0", subdir = "refl" }
txn  = { git = "github.com/misut/txn", version = "0.2.0", subdir = "txn"  }

[dev-dependencies]
helpers = { git = "github.com/misut/tools", version = "1.0.0", subdir = "helpers" }
)";

    auto table = toml::parse(input);
    auto m = manifest::from_toml(table);

    // string-form git dep still parsed
    check(m.dependencies.size() == 1, "subdir: string git dep still parsed");
    check(m.dependencies.at("github.com/user/lib") == "0.1.0", "subdir: string dep version");

    // inline-table subdir deps
    check(m.subdir_deps.size() == 2, "subdir: 2 regular subdir deps");
    check(m.subdir_deps.at("refl").repo == "github.com/misut/txn", "subdir: refl repo");
    check(m.subdir_deps.at("refl").version == "0.1.0", "subdir: refl version");
    check(m.subdir_deps.at("refl").subdir == "refl", "subdir: refl subdir");
    check(m.subdir_deps.at("txn").version == "0.2.0", "subdir: txn version");
    check(m.subdir_deps.at("txn").subdir == "txn", "subdir: txn subdir");

    // dev-dependencies variant
    check(m.dev_subdir_deps.size() == 1, "subdir: 1 dev subdir dep");
    check(m.dev_subdir_deps.at("helpers").repo == "github.com/misut/tools",
          "subdir: dev helpers repo");
    check(m.dev_subdir_deps.at("helpers").subdir == "helpers", "subdir: dev helpers subdir");
}

void test_subdir_deps_missing_fields() {
    auto parse_throws = [](char const* input) {
        try {
            auto table = toml::parse(input);
            (void)manifest::from_toml(table);
            return false;
        } catch (std::runtime_error const&) {
            return true;
        }
    };

    // missing git
    check(parse_throws(R"(
[package]
name = "app"
version = "1.0.0"
[dependencies]
refl = { version = "0.1.0", subdir = "refl" }
)"), "subdir: missing git throws");

    // missing version
    check(parse_throws(R"(
[package]
name = "app"
version = "1.0.0"
[dependencies]
refl = { git = "github.com/misut/txn", subdir = "refl" }
)"), "subdir: missing version throws");

    // missing subdir
    check(parse_throws(R"(
[package]
name = "app"
version = "1.0.0"
[dependencies]
refl = { git = "github.com/misut/txn", version = "0.1.0" }
)"), "subdir: missing subdir throws");

    // empty subdir
    check(parse_throws(R"(
[package]
name = "app"
version = "1.0.0"
[dependencies]
refl = { git = "github.com/misut/txn", version = "0.1.0", subdir = "" }
)"), "subdir: empty subdir throws");
}

void test_no_dev_deps() {
    auto input = R"(
[package]
name = "simple"
version = "0.1.0"
)";

    auto table = toml::parse(input);
    auto m = manifest::from_toml(table);

    check(m.dev_dependencies.empty(), "no dev-deps by default");
    check(m.find_deps.empty(), "no find-deps by default");
    check(m.dev_find_deps.empty(), "no dev find-deps by default");
    check(m.path_deps.empty(), "no path-deps by default");
    check(m.dev_path_deps.empty(), "no dev path-deps by default");
    check(m.workspace_deps.empty(), "no workspace-deps by default");
    check(m.dev_workspace_deps.empty(), "no dev workspace-deps by default");
    check(m.vcpkg_deps.empty(), "no vcpkg-deps by default");
    check(m.dev_vcpkg_deps.empty(), "no dev vcpkg-deps by default");
    check(m.subdir_deps.empty(), "no subdir-deps by default");
    check(m.dev_subdir_deps.empty(), "no dev subdir-deps by default");
    check(m.defines.empty(), "no defines by default");
    check(m.defines_debug.empty(), "no debug defines by default");
    check(m.defines_release.empty(), "no release defines by default");
}

int main() {
    test_basic_manifest();
    test_minimal_manifest();
    test_lib_type();
    test_workspace();
    test_non_workspace();
    test_dev_dependencies();
    test_find_deps();
    test_find_deps_only();
    test_path_deps();
    test_workspace_deps_false_ignored();
    test_find_workspace_root();
    test_resolve_workspace_member();
    test_vcpkg_deps();
    test_vcpkg_deps_with_features();
    test_subdir_deps();
    test_subdir_deps_missing_fields();
    test_defines();
    test_no_dev_deps();

    if (failures > 0) {
        std::println("{} test(s) failed", failures);
        return 1;
    }
    std::println("test_manifest: all passed");
    return 0;
}
