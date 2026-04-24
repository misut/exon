import std;
import fetch;
import fetch.system;
import lock;
import lock.system;
import manifest;
import manifest.system;

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

manifest::Manifest load_project_manifest(fs::path const& project_root) {
    return manifest::system::load((project_root / "exon.toml").string());
}

fetch::FetchResult fetch_project(fs::path const& project_root, bool include_dev = false) {
    auto manifest = load_project_manifest(project_root);
    return fetch::system::fetch_all(project_root, manifest, project_root / "exon.lock",
                                    include_dev);
}

// test: resolve path deps relative to project_root
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

    try {
        auto result = fetch_project(ws.root / "app");

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

    try {
        auto result = fetch_project(ws.root / "app");

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

    try {
        auto result = fetch_project(ws.root / "app");

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

    try {
        auto app_root = ws.root / "app";
        auto m = load_project_manifest(app_root);
        auto lock_path = app_root / "exon.lock";

        // without dev: empty
        auto r1 = fetch::system::fetch_all(app_root, m, lock_path, false);
        check(r1.deps.empty(), "dev_path: empty without include_dev");

        // with dev: 1 dep
        auto r2 = fetch::system::fetch_all(app_root, m, lock_path, true);
        check(r2.deps.size() == 1, "dev_path: 1 dep with include_dev");
        if (!r2.deps.empty())
            check(r2.deps[0].is_dev, "dev_path: is_dev=true");
    } catch (std::exception const& e) {
        std::println(std::cerr, "  exception: {}", e.what());
        ++failures;
    }
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
        auto full = std::format("git -C \"{}\" {} {}", repo.generic_string(), cmd, null_redirect);
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

    try {
        auto result = fetch_project(ws.root / "app");

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
        if (locked) {
            check(locked->subdir == "refl", "subdir fetch: lock subdir field");
            check(locked->package == "refl", "subdir fetch: canonical package stored");
        }

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
        auto full = std::format("git -C \"{}\" {} {}", repo.generic_string(), cmd, null_redirect);
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

    try {
        auto result = fetch_project(ws.root / "app");

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
        auto full = std::format("git -C \"{}\" {} {}", repo.generic_string(), cmd, null_redirect);
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

    bool threw = false;
    try {
        fetch_project(ws.root / "app");
    } catch (...) {
        threw = true;
    }
    check(threw, "subdir missing: throws");
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

    bool threw = false;
    try {
        fetch_project(ws.root / "app");
    } catch (...) {
        threw = true;
    }
    check(threw, "missing ws member: throws");
}

// helper: create a local git repo under a parent dir so that
// extract_repo_name (last path component) returns the desired name
struct TmpGitRepo {
    fs::path root;
    fs::path parent;
    bool owns_parent;

    // create at parent_dir/name so extract_repo_name returns "name"
    TmpGitRepo(fs::path const& parent_dir, std::string const& name)
        : parent(parent_dir), owns_parent(false) {
        root = parent_dir / name;
        fs::remove_all(root);
        fs::create_directories(root / "src");
    }
    ~TmpGitRepo() { fs::remove_all(root); }

    void write(std::string const& rel, std::string const& content) {
        auto p = root / rel;
        fs::create_directories(p.parent_path());
        auto f = std::ofstream{p};
        f << content;
    }

    int git(std::string const& cmd) const {
        auto full = std::format("git -C \"{}\" {} {}", root.generic_string(), cmd, null_redirect);
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

// test: path dep with a transitive git dependency
void test_fetch_path_with_git_dep() {
    TmpWorkspace ws{"exon_test_fetch_path_git"};
    auto cache = ws.root / "cache";
    setenv("EXON_CACHE_DIR", cache.string().c_str(), 1);

    // create a git repo; last path component = package name for extract_repo_name
    auto repos = ws.root / "repos";
    fs::create_directories(repos);
    TmpGitRepo leaf_repo{repos, "leaf"};
    leaf_repo.write("exon.toml", R"([package]
name = "leaf"
version = "0.1.0"
type = "lib"
standard = 23
)");
    leaf_repo.write("src/leaf.cppm", "export module leaf;");
    leaf_repo.init_and_tag("v0.1.0");

    // path dep that depends on the git repo
    ws.write("middle/exon.toml", std::format(R"([package]
name = "middle"
version = "0.1.0"
type = "lib"
standard = 23

[dependencies]
"{}" = "0.1.0"
)", leaf_repo.key()));
    ws.write("middle/src/middle.cppm", "export module middle;");

    // app that depends on middle via path
    ws.write("app/exon.toml", R"([package]
name = "app"
version = "0.1.0"

[dependencies.path]
middle = "../middle"
)");
    ws.write("app/src/main.cpp", "int main() {}");

    try {
        auto result = fetch_project(ws.root / "app");

        check(result.deps.size() == 2, "path->git: 2 deps");
        if (result.deps.size() == 2) {
            // topological: git dep (leaf) first, then path dep (middle)
            check(result.deps[0].name == "leaf", "path->git: leaf first");
            check(!result.deps[0].is_path, "path->git: leaf is git dep");
            check(result.deps[1].name == "middle", "path->git: middle second");
            check(result.deps[1].is_path, "path->git: middle is path dep");
        }
    } catch (std::exception const& e) {
        std::println(std::cerr, "  exception: {}", e.what());
        ++failures;
    }
    unsetenv("EXON_CACHE_DIR");
}

// test: git dep -> git dep transitive resolution
void test_fetch_git_transitive() {
    TmpWorkspace ws{"exon_test_fetch_git_trans"};
    auto cache = ws.root / "cache";
    setenv("EXON_CACHE_DIR", cache.string().c_str(), 1);

    // leaf git repo (no deps); last path component = package name
    auto repos = ws.root / "repos";
    fs::create_directories(repos);
    TmpGitRepo leaf_repo{repos, "leaf"};
    leaf_repo.write("exon.toml", R"([package]
name = "leaf"
version = "0.1.0"
type = "lib"
standard = 23
)");
    leaf_repo.write("src/leaf.cppm", "export module leaf;");
    leaf_repo.init_and_tag("v0.1.0");

    // middle git repo depends on leaf
    TmpGitRepo middle_repo{repos, "middle"};
    middle_repo.write("exon.toml", std::format(R"([package]
name = "middle"
version = "0.1.0"
type = "lib"
standard = 23

[dependencies]
"{}" = "0.1.0"
)", leaf_repo.key()));
    middle_repo.write("src/middle.cppm", "export module middle;");
    middle_repo.init_and_tag("v0.1.0");

    // app depends on middle via git
    ws.write("app/exon.toml", std::format(R"([package]
name = "app"
version = "0.1.0"

[dependencies]
"{}" = "0.1.0"
)", middle_repo.key()));
    ws.write("app/src/main.cpp", "int main() {}");

    try {
        auto result = fetch_project(ws.root / "app");

        check(result.deps.size() == 2, "git->git: 2 deps");
        if (result.deps.size() == 2) {
            // topological: leaf first, middle second
            check(result.deps[0].name == "leaf", "git->git: leaf first");
            check(result.deps[1].name == "middle", "git->git: middle second");
            check(!result.deps[0].commit.empty(), "git->git: leaf has commit");
            check(!result.deps[1].commit.empty(), "git->git: middle has commit");
        }

        // lock file should have both entries
        check(result.lock_file.find(leaf_repo.key(), "0.1.0") != nullptr,
              "git->git: leaf in lock file");
        check(result.lock_file.find(middle_repo.key(), "0.1.0") != nullptr,
              "git->git: middle in lock file");
    } catch (std::exception const& e) {
        std::println(std::cerr, "  exception: {}", e.what());
        ++failures;
    }
    unsetenv("EXON_CACHE_DIR");
}

// test: path dep with a transitive subdir dep
void test_fetch_path_with_subdir_dep() {
    TmpWorkspace ws{"exon_test_fetch_path_subdir"};
    auto cache = ws.root / "cache";
    setenv("EXON_CACHE_DIR", cache.string().c_str(), 1);

    // git repo with a workspace member "refl"
    auto repos = ws.root / "repos";
    fs::create_directories(repos);
    TmpGitRepo repo{repos, "subrepo"};
    repo.write("exon.toml", "[workspace]\nmembers = [\"refl\"]\n");
    repo.write("refl/exon.toml", R"([package]
name = "refl"
version = "0.1.0"
type = "lib"
standard = 23
)");
    repo.write("refl/src/refl.cppm", "export module refl;");
    repo.write("refl/CMakeLists.txt", "add_library(refl INTERFACE)\n");
    repo.init_and_tag("v0.1.0");

    // path dep that depends on the repo via subdir
    ws.write("middle/exon.toml", std::format(R"([package]
name = "middle"
version = "0.1.0"
type = "lib"
standard = 23

[dependencies]
refl = {{ git = "{}", version = "0.1.0", subdir = "refl" }}
)", repo.key()));
    ws.write("middle/src/middle.cppm", "export module middle;");

    // app depends on middle via path
    ws.write("app/exon.toml", R"([package]
name = "app"
version = "0.1.0"

[dependencies.path]
middle = "../middle"
)");
    ws.write("app/src/main.cpp", "int main() {}");

    try {
        auto result = fetch_project(ws.root / "app");

        check(result.deps.size() == 2, "path->subdir: refl + middle resolved");
        if (result.deps.size() == 2) {
            check(result.deps[0].name == "refl", "path->subdir: refl first");
            check(result.deps[1].name == "middle", "path->subdir: middle second");
            check(result.deps[1].dependency_names.size() == 1,
                  "path->subdir: middle has one child");
            check(result.deps[1].dependency_names[0] == "refl",
                  "path->subdir: middle links canonical refl target");
        }
    } catch (std::exception const& e) {
        std::println(std::cerr, "  exception: {}", e.what());
        ++failures;
    }
    unsetenv("EXON_CACHE_DIR");
}

// test: path dep with featured dep
void test_fetch_path_with_featured_dep() {
    TmpWorkspace ws{"exon_test_fetch_path_feat"};
    auto cache = ws.root / "cache";
    setenv("EXON_CACHE_DIR", cache.string().c_str(), 1);

    // git repo with features
    auto repos = ws.root / "repos";
    fs::create_directories(repos);
    TmpGitRepo feat_repo{repos, "featlib"};
    feat_repo.write("exon.toml", R"([package]
name = "featlib"
version = "0.1.0"
type = "lib"
standard = 23

[features]
x = ["x"]
)");
    feat_repo.write("src/featlib.cppm", "export module featlib;");
    feat_repo.write("src/x.cppm", "export module featlib.x;");
    feat_repo.init_and_tag("v0.1.0");

    // path dep with featured dep
    ws.write("middle/exon.toml", std::format(R"([package]
name = "middle"
version = "0.1.0"
type = "lib"
standard = 23

[dependencies]
"{}" = {{ version = "0.1.0", features = ["x"] }}
)", feat_repo.key()));
    ws.write("middle/src/middle.cppm", "export module middle;");

    // app depends on middle via path
    ws.write("app/exon.toml", R"([package]
name = "app"
version = "0.1.0"

[dependencies.path]
middle = "../middle"
)");
    ws.write("app/src/main.cpp", "int main() {}");

    try {
        auto result = fetch_project(ws.root / "app");

        check(result.deps.size() == 2, "path->featured: featlib + middle resolved");
        if (result.deps.size() == 2) {
            check(result.deps[0].name == "featlib", "path->featured: featlib first");
            check(result.deps[0].features.size() == 1, "path->featured: selected feature kept");
            check(result.deps[0].features[0] == "x", "path->featured: selected feature name");
            check(result.deps[1].name == "middle", "path->featured: middle second");
            check(result.deps[1].dependency_names.size() == 1,
                  "path->featured: middle has one child");
            check(result.deps[1].dependency_names[0] == "featlib",
                  "path->featured: middle links canonical featlib target");
        }
    } catch (std::exception const& e) {
        std::println(std::cerr, "  exception: {}", e.what());
        ++failures;
    }
    unsetenv("EXON_CACHE_DIR");
}

void test_fetch_duplicate_feature_requests_are_additive() {
    TmpWorkspace ws{"exon_test_fetch_feature_union"};
    auto cache = ws.root / "cache";
    setenv("EXON_CACHE_DIR", cache.string().c_str(), 1);

    auto repos = ws.root / "repos";
    fs::create_directories(repos);
    TmpGitRepo feat_repo{repos, "featlib"};
    feat_repo.write("exon.toml", R"([package]
name = "featlib"
version = "0.1.0"
type = "lib"
standard = 23

[features]
x = ["x"]
y = ["y"]
)");
    feat_repo.write("src/featlib.cppm", "export module featlib;");
    feat_repo.write("src/x.cppm", "export module featlib.x;");
    feat_repo.write("src/y.cppm", "export module featlib.y;");
    feat_repo.init_and_tag("v0.1.0");

    ws.write("middle/exon.toml", std::format(R"([package]
name = "middle"
version = "0.1.0"
type = "lib"
standard = 23

[dependencies]
"{}" = {{ version = "0.1.0", features = ["x"] }}
)", feat_repo.key()));
    ws.write("middle/src/middle.cppm", "export module middle;");

    ws.write("app/exon.toml", std::format(R"([package]
name = "app"
version = "0.1.0"

[dependencies]
"{}" = {{ version = "0.1.0", features = ["y"] }}

[dependencies.path]
middle = "../middle"
)", feat_repo.key()));
    ws.write("app/src/main.cpp", "int main() {}");

    try {
        auto result = fetch_project(ws.root / "app");

        auto it = std::ranges::find_if(result.deps, [](auto const& dep) {
            return dep.name == "featlib";
        });
        check(it != result.deps.end(), "feature union: featlib resolved");
        if (it != result.deps.end()) {
            check(std::ranges::find(it->features, "x") != it->features.end(),
                  "feature union: transitive feature kept");
            check(std::ranges::find(it->features, "y") != it->features.end(),
                  "feature union: root feature kept");
        }

        auto const* locked = result.lock_file.find(feat_repo.key(), "0.1.0");
        check(locked != nullptr, "feature union: lock entry exists");
        if (locked) {
            check(std::ranges::find(locked->features, "x") != locked->features.end(),
                  "feature union: lock keeps transitive feature");
            check(std::ranges::find(locked->features, "y") != locked->features.end(),
                  "feature union: lock keeps root feature");
        }
    } catch (std::exception const& e) {
        std::println(std::cerr, "  exception: {}", e.what());
        ++failures;
    }
    unsetenv("EXON_CACHE_DIR");
}

void test_fetch_root_direct_git_overrides_transitive_version() {
    TmpWorkspace ws{"exon_test_fetch_root_git_override"};
    auto cache = ws.root / "cache";
    setenv("EXON_CACHE_DIR", cache.string().c_str(), 1);

    auto repos = ws.root / "repos";
    fs::create_directories(repos);

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
    cppx_repo.write("src/cppx.cppm", "export module cppx;\nexport namespace cppx { inline constexpr int version = 120; }\n");
    cppx_repo.commit_and_tag("v1.2.0");

    TmpGitRepo txn_repo{repos, "txn"};
    txn_repo.write("exon.toml", std::format(R"([package]
name = "txn"
version = "0.6.1"
type = "lib"
standard = 23

[dependencies]
"{}" = "=1.0.3"
)", cppx_repo.key()));
    txn_repo.write("src/txn.cppm", "export module txn;");
    txn_repo.init_and_tag("v0.6.1");

    ws.write("app/exon.toml", std::format(R"([package]
name = "app"
version = "0.1.0"

[dependencies]
"{}" = "0.6.1"
"{}" = "1.2.0"
)", txn_repo.key(), cppx_repo.key()));
    ws.write("app/src/main.cpp", "int main() { return 0; }\n");

    try {
        auto app_root = ws.root / "app";
        lock::LockFile stale;
        stale.packages = {{
            .name = cppx_repo.key(),
            .package = "cppx",
            .version = "1.0.3",
            .commit = "stale",
        }};
        lock::system::save(stale, (app_root / "exon.lock").string());

        auto result = fetch_project(app_root);

        check(result.deps.size() == 2, "root git override: cppx + txn selected");
        if (result.deps.size() == 2) {
            check(result.deps[0].name == "cppx", "root git override: cppx first");
            check(result.deps[0].version == "1.2.0", "root git override: direct version kept");
            check(result.deps[1].name == "txn", "root git override: txn second");
            check(result.deps[1].dependency_names.size() == 1,
                  "root git override: txn has one child");
            check(result.deps[1].dependency_names[0] == "cppx",
                  "root git override: txn links selected cppx");
        }

        check(result.lock_file.find(cppx_repo.key(), "1.2.0") != nullptr,
              "root git override: selected cppx in lock");
        check(result.lock_file.find(cppx_repo.key(), "1.0.3") == nullptr,
              "root git override: stale transitive cppx pruned");
        check(result.lock_file.find(txn_repo.key(), "0.6.1") != nullptr,
              "root git override: txn in lock");
    } catch (std::exception const& e) {
        std::println(std::cerr, "  exception: {}", e.what());
        ++failures;
    }
    unsetenv("EXON_CACHE_DIR");
}

void test_fetch_no_lock_selects_latest_compatible() {
    TmpWorkspace ws{"exon_test_fetch_latest_compatible"};
    auto cache = ws.root / "cache";
    setenv("EXON_CACHE_DIR", cache.string().c_str(), 1);

    auto repos = ws.root / "repos";
    fs::create_directories(repos);

    TmpGitRepo lib_repo{repos, "lib"};
    lib_repo.write("exon.toml", R"([package]
name = "lib"
version = "1.0.0"
type = "lib"
standard = 23
)");
    lib_repo.write("src/lib.cppm", "export module lib;");
    lib_repo.init_and_tag("v1.0.0");
    lib_repo.write("exon.toml", R"([package]
name = "lib"
version = "1.2.0"
type = "lib"
standard = 23
)");
    lib_repo.commit_and_tag("v1.2.0");

    ws.write("app/exon.toml", std::format(R"([package]
name = "app"
version = "0.1.0"

[dependencies]
"{}" = "1.0.0"
)", lib_repo.key()));
    ws.write("app/src/main.cpp", "int main() { return 0; }\n");

    try {
        auto result = fetch_project(ws.root / "app");

        check(result.deps.size() == 1, "latest compatible: one dep");
        if (!result.deps.empty())
            check(result.deps[0].version == "1.2.0",
                  "latest compatible: bare requirement selects newest compatible");
        check(result.lock_file.find(lib_repo.key(), "1.2.0") != nullptr,
              "latest compatible: selected version locked");
    } catch (std::exception const& e) {
        std::println(std::cerr, "  exception: {}", e.what());
        ++failures;
    }
    unsetenv("EXON_CACHE_DIR");
}

void test_fetch_lock_reuse_and_update_controls() {
    TmpWorkspace ws{"exon_test_fetch_update_controls"};
    auto cache = ws.root / "cache";
    setenv("EXON_CACHE_DIR", cache.string().c_str(), 1);

    auto repos = ws.root / "repos";
    fs::create_directories(repos);

    TmpGitRepo lib_repo{repos, "lib"};
    lib_repo.write("exon.toml", R"([package]
name = "lib"
version = "1.0.0"
type = "lib"
standard = 23
)");
    lib_repo.write("src/lib.cppm", "export module lib;");
    lib_repo.init_and_tag("v1.0.0");

    ws.write("app/exon.toml", std::format(R"([package]
name = "app"
version = "0.1.0"

[dependencies]
"{}" = "1.0.0"
)", lib_repo.key()));
    ws.write("app/src/main.cpp", "int main() { return 0; }\n");

    try {
        auto app_root = ws.root / "app";
        auto first = fetch_project(app_root);
        check(!first.deps.empty() && first.deps[0].version == "1.0.0",
              "lock reuse: initial lock selects 1.0.0");

        lib_repo.write("exon.toml", R"([package]
name = "lib"
version = "1.1.0"
type = "lib"
standard = 23
)");
        lib_repo.commit_and_tag("v1.1.0");

        auto second = fetch_project(app_root);
        check(!second.deps.empty() && second.deps[0].version == "1.0.0",
              "lock reuse: compatible locked version is reused");

        auto manifest = load_project_manifest(app_root);
        auto updated = fetch::system::fetch_all({
            .manifest = manifest,
            .project_root = app_root,
            .lock_path = app_root / "exon.lock",
            .update_all = true,
        });
        check(!updated.deps.empty() && updated.deps[0].version == "1.1.0",
              "update all: newest compatible version selected");

        lib_repo.write("exon.toml", R"([package]
name = "lib"
version = "1.2.0"
type = "lib"
standard = 23
)");
        lib_repo.commit_and_tag("v1.2.0");

        auto dry_run = fetch::system::fetch_all({
            .manifest = manifest,
            .project_root = app_root,
            .lock_path = app_root / "exon.lock",
            .update_all = true,
            .dry_run = true,
        });
        check(!dry_run.deps.empty() && dry_run.deps[0].version == "1.2.0",
              "dry run: computes newest compatible version");
        auto lock_after_dry_run = lock::system::load((app_root / "exon.lock").string());
        check(lock_after_dry_run.find(lib_repo.key(), "1.1.0") != nullptr,
              "dry run: existing lockfile remains unchanged");

        auto precise = fetch::system::fetch_all({
            .manifest = manifest,
            .project_root = app_root,
            .lock_path = app_root / "exon.lock",
            .update_packages = {lib_repo.key()},
            .precise_version = std::string{"1.0.0"},
        });
        check(!precise.deps.empty() && precise.deps[0].version == "1.0.0",
              "precise update: requested compatible version selected");
    } catch (std::exception const& e) {
        std::println(std::cerr, "  exception: {}", e.what());
        ++failures;
    }
    unsetenv("EXON_CACHE_DIR");
}

void test_fetch_root_path_overrides_transitive_git() {
    TmpWorkspace ws{"exon_test_fetch_root_path_override"};
    auto cache = ws.root / "cache";
    setenv("EXON_CACHE_DIR", cache.string().c_str(), 1);

    auto repos = ws.root / "repos";
    fs::create_directories(repos);

    TmpGitRepo cppx_repo{repos, "cppx"};
    cppx_repo.write("exon.toml", R"([package]
name = "cppx"
version = "1.0.3"
type = "lib"
standard = 23
)");
    cppx_repo.write("src/cppx.cppm", "export module cppx;");
    cppx_repo.init_and_tag("v1.0.3");

    ws.write("ui/exon.toml", std::format(R"([package]
name = "ui"
version = "0.1.0"
type = "lib"
standard = 23

[dependencies]
"{}" = "=1.0.3"
)", cppx_repo.key()));
    ws.write("ui/src/ui.cppm", "export module ui;");

    ws.write("cppx-local/exon.toml", R"([package]
name = "cppx"
version = "9.9.9"
type = "lib"
standard = 23
)");
    ws.write("cppx-local/src/cppx.cppm", "export module cppx;");

    ws.write("app/exon.toml", R"([package]
name = "app"
version = "0.1.0"

[dependencies.path]
ui = "../ui"
cppx = "../cppx-local"
)");
    ws.write("app/src/main.cpp", "int main() { return 0; }\n");

    try {
        auto result = fetch_project(ws.root / "app");

        check(result.deps.size() == 2, "root path override: cppx + ui selected");
        if (result.deps.size() == 2) {
            check(result.deps[0].name == "cppx", "root path override: cppx first");
            check(result.deps[0].is_path, "root path override: cppx selected from path");
            check(result.deps[1].name == "ui", "root path override: ui second");
            check(result.deps[1].dependency_names.size() == 1,
                  "root path override: ui has one child");
            check(result.deps[1].dependency_names[0] == "cppx",
                  "root path override: ui links selected cppx");
        }

        check(result.lock_file.packages.empty(),
              "root path override: unselected git candidate omitted from lock");
    } catch (std::exception const& e) {
        std::println(std::cerr, "  exception: {}", e.what());
        ++failures;
    }
    unsetenv("EXON_CACHE_DIR");
}

void test_fetch_conflicting_transitive_versions_fail() {
    TmpWorkspace ws{"exon_test_fetch_transitive_conflict"};
    auto cache = ws.root / "cache";
    setenv("EXON_CACHE_DIR", cache.string().c_str(), 1);

    auto repos = ws.root / "repos";
    fs::create_directories(repos);

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

    ws.write("left/exon.toml", std::format(R"([package]
name = "left"
version = "0.1.0"
type = "lib"
standard = 23

[dependencies]
"{}" = "=1.0.3"
)", cppx_repo.key()));
    ws.write("left/src/left.cppm", "export module left;");

    ws.write("right/exon.toml", std::format(R"([package]
name = "right"
version = "0.1.0"
type = "lib"
standard = 23

[dependencies]
"{}" = "=1.2.0"
)", cppx_repo.key()));
    ws.write("right/src/right.cppm", "export module right;");

    ws.write("app/exon.toml", R"([package]
name = "app"
version = "0.1.0"

[dependencies.path]
left = "../left"
right = "../right"
)");
    ws.write("app/src/main.cpp", "int main() { return 0; }\n");

    bool threw = false;
    try {
        (void)fetch_project(ws.root / "app");
    } catch (...) {
        threw = true;
    }
    check(threw, "transitive conflict: conflicting versions fail");
    unsetenv("EXON_CACHE_DIR");
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

    run("test_fetch_path_dep", test_fetch_path_dep);
    run("test_fetch_workspace_dep", test_fetch_workspace_dep);
    run("test_fetch_transitive_path", test_fetch_transitive_path);
    run("test_fetch_dev_path_dep", test_fetch_dev_path_dep);
    run("test_fetch_missing_workspace_member", test_fetch_missing_workspace_member);
    run("test_fetch_subdir_dep", test_fetch_subdir_dep);
    run("test_fetch_subdir_dedup_clone", test_fetch_subdir_dedup_clone);
    run("test_fetch_subdir_missing", test_fetch_subdir_missing);
    run("test_fetch_path_with_git_dep", test_fetch_path_with_git_dep);
    run("test_fetch_git_transitive", test_fetch_git_transitive);
    run("test_fetch_path_with_subdir_dep", test_fetch_path_with_subdir_dep);
    run("test_fetch_path_with_featured_dep", test_fetch_path_with_featured_dep);
    run("test_fetch_duplicate_feature_requests_are_additive",
        test_fetch_duplicate_feature_requests_are_additive);
    run("test_fetch_root_direct_git_overrides_transitive_version",
        test_fetch_root_direct_git_overrides_transitive_version);
    run("test_fetch_no_lock_selects_latest_compatible",
        test_fetch_no_lock_selects_latest_compatible);
    run("test_fetch_lock_reuse_and_update_controls",
        test_fetch_lock_reuse_and_update_controls);
    run("test_fetch_root_path_overrides_transitive_git",
        test_fetch_root_path_overrides_transitive_git);
    run("test_fetch_conflicting_transitive_versions_fail",
        test_fetch_conflicting_transitive_versions_fail);

    if (failures > 0) {
        std::println("{} test(s) failed", failures);
        return 1;
    }
    std::println("test_fetch: all passed");
    return 0;
}
