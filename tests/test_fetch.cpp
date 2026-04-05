import std;
import fetch;
import manifest;
import toml;

#if defined(_WIN32)
extern "C" int _putenv_s(char const* name, char const* value);
static int setenv(char const* name, char const* value, int /*overwrite*/) {
    return _putenv_s(name, value);
}
static int unsetenv(char const* name) {
    return _putenv_s(name, "");
}
constexpr auto null_redirect = ">NUL 2>&1";
extern "C" unsigned int __stdcall SetErrorMode(unsigned int);
extern "C" int _set_abort_behavior(unsigned int, unsigned int);
static int _crash_suppression = []() {
    SetErrorMode(0x0001u | 0x0002u);
    _set_abort_behavior(0, 0x1u | 0x4u);
    return 0;
}();
#else
extern "C" int setenv(char const* name, char const* value, int overwrite);
extern "C" int unsetenv(char const* name);
constexpr auto null_redirect = ">/dev/null 2>&1";
#endif

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

// test: fetch a git+subdir dep from a local bare repo via file:// URL
void test_fetch_subdir_dep() {
    TmpWorkspace ws{"exon_test_fetch_subdir"};

    // set EXON_CACHE_DIR to an isolated cache under the tmp workspace
    auto cache = ws.root / "cache";
    setenv("EXON_CACHE_DIR", cache.string().c_str(), 1);

    // create a fake workspace repo to be cloned: workspace root + `refl/` member
    auto repo = ws.root / "fake-repo";
    fs::create_directories(repo / "refl" / "src");
    {
        auto f = std::ofstream{repo / "exon.toml"};
        f << "[workspace]\nmembers = [\"refl\"]\n";
    }
    {
        auto f = std::ofstream{repo / "refl" / "exon.toml"};
        f << "[package]\nname = \"refl\"\nversion = \"0.1.0\"\ntype = \"lib\"\n";
    }
    {
        auto f = std::ofstream{repo / "refl" / "src" / "refl.cppm"};
        f << "export module refl;\n";
    }
    {
        auto f = std::ofstream{repo / "refl" / "CMakeLists.txt"};
        f << "add_library(refl INTERFACE)\n";
    }

    // init + commit + tag
    auto git = [&](std::string const& cmd) {
        auto full = std::format("cd \"{}\" && git {} {}", repo.generic_string(), cmd, null_redirect);
        return std::system(full.c_str());
    };
    git("init -q");
    git("config user.email test@example.com");
    git("config user.name Test");
    git("add .");
    git("commit -q -m init");
    git("tag v0.1.0");

    // consumer project in its own dir
    fs::create_directories(ws.root / "app" / "src");
    {
        auto f = std::ofstream{ws.root / "app" / "exon.toml"};
        f << std::format(R"(
[package]
name = "app"
version = "0.1.0"

[dependencies]
refl = {{ git = "{}", version = "0.1.0", subdir = "refl" }}
)", repo.generic_string());
    }

    auto saved_cwd = fs::current_path();
    fs::current_path(ws.root / "app");

    try {
        auto m = manifest::load("exon.toml");
        auto lock_path = (ws.root / "app" / "exon.lock").string();
        auto result = fetch::fetch_all(m, lock_path);

        check(result.deps.size() == 1, "subdir fetch: 1 dep");
        if (!result.deps.empty()) {
            auto& d = result.deps[0];
            check(d.name == "refl", "subdir fetch: name");
            check(d.subdir == "refl", "subdir fetch: subdir");
            check(!d.is_path, "subdir fetch: not is_path");
            check(!d.commit.empty(), "subdir fetch: has commit");
            check(d.path.filename() == "refl", "subdir fetch: path ends in subdir");
            check(fs::exists(d.path / "exon.toml"), "subdir fetch: subdir has exon.toml");
        }

        // lock entry should use composite name
        auto const* locked = result.lock_file.find(
            std::format("{}#{}", repo.generic_string(), "refl"), "0.1.0");
        check(locked != nullptr, "subdir fetch: composite-name lock entry exists");
        if (locked)
            check(locked->subdir == "refl", "subdir fetch: lock subdir field");

        // verify clone happened under EXON_CACHE_DIR (not under HOME/.exon/cache)
        // absolute-path keys have their leading "/" and Windows drive colon stripped
        // so the cache path composes cleanly as a directory
        auto repo_key = repo.generic_string();
        while (!repo_key.empty() && repo_key.front() == '/')
            repo_key.erase(repo_key.begin());
        if (repo_key.size() >= 2 && repo_key[1] == ':')
            repo_key[1] = '_';
        auto expected_clone = cache / repo_key / "v0.1.0";
        check(fs::exists(expected_clone / "exon.toml"),
              "subdir fetch: clone landed under EXON_CACHE_DIR");
    } catch (std::exception const& e) {
        std::println(std::cerr, "  exception: {}", e.what());
        ++failures;
    }

    fs::current_path(saved_cwd);
    unsetenv("EXON_CACHE_DIR");
}

// test: same (repo, version) with two different subdirs shares one clone
void test_fetch_subdir_dedup_clone() {
    TmpWorkspace ws{"exon_test_fetch_subdir_dedup"};
    auto cache = ws.root / "cache";
    setenv("EXON_CACHE_DIR", cache.string().c_str(), 1);

    auto repo = ws.root / "fake-repo";
    fs::create_directories(repo / "a" / "src");
    fs::create_directories(repo / "b" / "src");
    {
        auto f = std::ofstream{repo / "exon.toml"};
        f << "[workspace]\nmembers = [\"a\", \"b\"]\n";
    }
    for (auto const* name : {"a", "b"}) {
        auto f = std::ofstream{repo / name / "exon.toml"};
        f << std::format("[package]\nname = \"{}\"\nversion = \"0.1.0\"\ntype = \"lib\"\n", name);
    }
    for (auto const* name : {"a", "b"}) {
        auto f = std::ofstream{repo / name / "CMakeLists.txt"};
        f << std::format("add_library({} INTERFACE)\n", name);
    }
    auto git = [&](std::string const& cmd) {
        auto full = std::format("cd \"{}\" && git {} {}", repo.generic_string(), cmd, null_redirect);
        return std::system(full.c_str());
    };
    git("init -q");
    git("config user.email test@example.com");
    git("config user.name Test");
    git("add .");
    git("commit -q -m init");
    git("tag v0.1.0");

    fs::create_directories(ws.root / "app" / "src");
    {
        auto f = std::ofstream{ws.root / "app" / "exon.toml"};
        f << std::format(R"(
[package]
name = "app"
version = "0.1.0"

[dependencies]
a = {{ git = "{0}", version = "0.1.0", subdir = "a" }}
b = {{ git = "{0}", version = "0.1.0", subdir = "b" }}
)", repo.generic_string());
    }

    auto saved_cwd = fs::current_path();
    fs::current_path(ws.root / "app");

    try {
        auto m = manifest::load("exon.toml");
        auto lock_path = (ws.root / "app" / "exon.lock").string();
        auto result = fetch::fetch_all(m, lock_path);

        check(result.deps.size() == 2, "subdir dedup: 2 deps");

        // only one clone directory (leading "/" and drive colon stripped from absolute-path keys)
        auto repo_key = repo.generic_string();
        while (!repo_key.empty() && repo_key.front() == '/')
            repo_key.erase(repo_key.begin());
        if (repo_key.size() >= 2 && repo_key[1] == ':')
            repo_key[1] = '_';
        auto clone_root = cache / repo_key / "v0.1.0";
        check(fs::exists(clone_root / "a" / "exon.toml"), "subdir dedup: a present in clone");
        check(fs::exists(clone_root / "b" / "exon.toml"), "subdir dedup: b present in clone");

        // both deps point into the same clone
        auto parent_a = result.deps[0].path.parent_path();
        auto parent_b = result.deps[1].path.parent_path();
        check(parent_a == parent_b, "subdir dedup: both paths share parent clone");
    } catch (std::exception const& e) {
        std::println(std::cerr, "  exception: {}", e.what());
        ++failures;
    }

    fs::current_path(saved_cwd);
    unsetenv("EXON_CACHE_DIR");
}

// test: subdir that doesn't exist in the cloned repo throws
void test_fetch_subdir_missing() {
    TmpWorkspace ws{"exon_test_fetch_subdir_missing"};
    auto cache = ws.root / "cache";
    setenv("EXON_CACHE_DIR", cache.string().c_str(), 1);

    auto repo = ws.root / "fake-repo";
    fs::create_directories(repo);
    {
        auto f = std::ofstream{repo / "exon.toml"};
        f << "[package]\nname = \"x\"\nversion = \"0.1.0\"\n";
    }
    auto git = [&](std::string const& cmd) {
        auto full = std::format("cd \"{}\" && git {} {}", repo.generic_string(), cmd, null_redirect);
        return std::system(full.c_str());
    };
    git("init -q");
    git("config user.email test@example.com");
    git("config user.name Test");
    git("add .");
    git("commit -q -m init");
    git("tag v0.1.0");

    fs::create_directories(ws.root / "app" / "src");
    {
        auto f = std::ofstream{ws.root / "app" / "exon.toml"};
        f << std::format(R"(
[package]
name = "app"
version = "0.1.0"

[dependencies]
nope = {{ git = "{}", version = "0.1.0", subdir = "does-not-exist" }}
)", repo.generic_string());
    }

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
    check(threw, "subdir missing: throws");

    fs::current_path(saved_cwd);
    unsetenv("EXON_CACHE_DIR");
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
    test_fetch_subdir_dep();
    test_fetch_subdir_dedup_clone();
    test_fetch_subdir_missing();

    if (failures > 0) {
        std::println("{} test(s) failed", failures);
        return 1;
    }
    std::println("test_fetch: all passed");
    return 0;
}
