import std;
import fetch;
import manifest;
import toml;

int failures = 0;

void check(bool cond, std::string_view msg) {
    if (!cond) {
        std::println(std::cerr, "  FAIL: {}", msg);
        ++failures;
    }
}

namespace fs = std::filesystem;

struct TmpWorkspace {
    fs::path root;

    TmpWorkspace(std::string const& name) {
        root = fs::temp_directory_path() / name;
        fs::remove_all(root);
        fs::create_directories(root);
    }
    ~TmpWorkspace() { fs::remove_all(root); }

    void write(std::string const& rel, std::string const& content) {
        auto p = root / rel;
        fs::create_directories(p.parent_path());
        auto f = std::ofstream{p};
        f << content;
    }
};

// test: resolve path deps relative to cwd
void test_fetch_path_dep() {
    TmpWorkspace ws{"exon_test_fetch_path"};
    ws.write("app/exon.toml", R"(
[package]
name = "app"
version = "0.1.0"

[dependencies.path]
mylib = "../mylib"
)");
    ws.write("app/src/main.cpp", "int main() {}");
    ws.write("mylib/exon.toml", R"(
[package]
name = "mylib"
version = "0.1.0"
type = "lib"
)");
    ws.write("mylib/src/mylib.cppm", "export module mylib;");

    auto saved_cwd = fs::current_path();
    fs::current_path(ws.root / "app");

    try {
        auto m = manifest::load("exon.toml");
        auto lock_path = (ws.root / "app" / "exon.lock").string();
        auto result = fetch::fetch_all(m, lock_path);

        check(result.deps.size() == 1, "fetch_path: 1 dep");
        if (!result.deps.empty()) {
            auto& d = result.deps[0];
            check(d.is_path, "fetch_path: is_path=true");
            check(d.name == "mylib", "fetch_path: name");
            check(fs::weakly_canonical(d.path) == fs::weakly_canonical(ws.root / "mylib"),
                  "fetch_path: correct path");
        }
    } catch (std::exception const& e) {
        std::println(std::cerr, "  exception: {}", e.what());
        ++failures;
    }

    fs::current_path(saved_cwd);
}

// test: workspace dep resolves via workspace root
void test_fetch_workspace_dep() {
    TmpWorkspace ws{"exon_test_fetch_ws"};
    ws.write("exon.toml", R"(
[workspace]
members = ["core", "app"]
)");
    ws.write("core/exon.toml", R"(
[package]
name = "core"
version = "0.1.0"
type = "lib"
)");
    ws.write("core/src/core.cppm", "export module core;");
    ws.write("app/exon.toml", R"(
[package]
name = "app"
version = "0.1.0"

[dependencies.workspace]
core = true
)");
    ws.write("app/src/main.cpp", "int main() {}");

    auto saved_cwd = fs::current_path();
    fs::current_path(ws.root / "app");

    try {
        auto m = manifest::load("exon.toml");
        auto lock_path = (ws.root / "app" / "exon.lock").string();
        auto result = fetch::fetch_all(m, lock_path);

        check(result.deps.size() == 1, "fetch_ws: 1 dep");
        if (!result.deps.empty()) {
            auto& d = result.deps[0];
            check(d.is_path, "fetch_ws: is_path=true");
            check(d.name == "core", "fetch_ws: name=core");
            check(fs::weakly_canonical(d.path) == fs::weakly_canonical(ws.root / "core"),
                  "fetch_ws: path=workspace/core");
        }
    } catch (std::exception const& e) {
        std::println(std::cerr, "  exception: {}", e.what());
        ++failures;
    }

    fs::current_path(saved_cwd);
}

// test: transitive path deps
void test_fetch_transitive_path() {
    TmpWorkspace ws{"exon_test_fetch_trans"};
    ws.write("app/exon.toml", R"(
[package]
name = "app"
version = "0.1.0"

[dependencies.path]
middle = "../middle"
)");
    ws.write("app/src/main.cpp", "int main() {}");
    ws.write("middle/exon.toml", R"(
[package]
name = "middle"
version = "0.1.0"
type = "lib"

[dependencies.path]
leaf = "../leaf"
)");
    ws.write("middle/src/middle.cppm", "export module middle;");
    ws.write("leaf/exon.toml", R"(
[package]
name = "leaf"
version = "0.1.0"
type = "lib"
)");
    ws.write("leaf/src/leaf.cppm", "export module leaf;");

    auto saved_cwd = fs::current_path();
    fs::current_path(ws.root / "app");

    try {
        auto m = manifest::load("exon.toml");
        auto lock_path = (ws.root / "app" / "exon.lock").string();
        auto result = fetch::fetch_all(m, lock_path);

        check(result.deps.size() == 2, "transitive: 2 deps");
        // topological: leaf before middle
        if (result.deps.size() == 2) {
            check(result.deps[0].name == "leaf", "transitive: leaf first");
            check(result.deps[1].name == "middle", "transitive: middle second");
        }
    } catch (std::exception const& e) {
        std::println(std::cerr, "  exception: {}", e.what());
        ++failures;
    }

    fs::current_path(saved_cwd);
}

// test: dev-dependencies.path only included with include_dev=true
void test_fetch_dev_path_dep() {
    TmpWorkspace ws{"exon_test_fetch_dev_path"};
    ws.write("app/exon.toml", R"(
[package]
name = "app"
version = "0.1.0"

[dev-dependencies.path]
testlib = "../testlib"
)");
    ws.write("app/src/main.cpp", "int main() {}");
    ws.write("testlib/exon.toml", R"(
[package]
name = "testlib"
version = "0.1.0"
type = "lib"
)");
    ws.write("testlib/src/testlib.cppm", "export module testlib;");

    auto saved_cwd = fs::current_path();
    fs::current_path(ws.root / "app");

    try {
        auto m = manifest::load("exon.toml");
        auto lock_path = (ws.root / "app" / "exon.lock").string();

        // without dev: empty
        auto r1 = fetch::fetch_all(m, lock_path, false);
        check(r1.deps.empty(), "dev_path: empty without include_dev");

        // with dev: 1 dep
        auto r2 = fetch::fetch_all(m, lock_path, true);
        check(r2.deps.size() == 1, "dev_path: 1 dep with include_dev");
        if (!r2.deps.empty())
            check(r2.deps[0].is_dev, "dev_path: is_dev=true");
    } catch (std::exception const& e) {
        std::println(std::cerr, "  exception: {}", e.what());
        ++failures;
    }

    fs::current_path(saved_cwd);
}

// test: unknown workspace member errors
void test_fetch_missing_workspace_member() {
    TmpWorkspace ws{"exon_test_fetch_missing"};
    ws.write("exon.toml", R"(
[workspace]
members = ["app"]
)");
    ws.write("app/exon.toml", R"(
[package]
name = "app"
version = "0.1.0"

[dependencies.workspace]
missing = true
)");
    ws.write("app/src/main.cpp", "int main() {}");

    auto saved_cwd = fs::current_path();
    fs::current_path(ws.root / "app");

    bool threw = false;
    try {
        auto m = manifest::load("exon.toml");
        auto lock_path = (ws.root / "app" / "exon.lock").string();
        fetch::fetch_all(m, lock_path);
    } catch (std::exception const&) {
        threw = true;
    }
    check(threw, "missing ws member: throws");

    fs::current_path(saved_cwd);
}

int main() {
    test_fetch_path_dep();
    test_fetch_workspace_dep();
    test_fetch_transitive_path();
    test_fetch_dev_path_dep();
    test_fetch_missing_workspace_member();

    if (failures > 0) {
        std::println("{} test(s) failed", failures);
        return 1;
    }
    std::println("test_fetch: all passed");
    return 0;
}
