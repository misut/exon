import std;
import toml;
import manifest;

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

void test_no_dev_deps() {
    auto input = R"(
[package]
name = "simple"
version = "0.1.0"
)";

    auto table = toml::parse(input);
    auto m = manifest::from_toml(table);

    check(m.dev_dependencies.empty(), "no dev-deps by default");
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
    test_defines();
    test_no_dev_deps();

    if (failures > 0) {
        std::println("{} test(s) failed", failures);
        return 1;
    }
    std::println("test_manifest: all passed");
    return 0;
}
