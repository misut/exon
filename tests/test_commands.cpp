import std;
import commands;
import fetch;
import manifest;
import manifest.system;

int failures = 0;

void check(bool cond, std::string_view msg) {
    if (!cond) {
        std::println(std::cerr, "  FAIL: {}", msg);
        ++failures;
    }
}

struct TmpDir {
    std::filesystem::path root;

    TmpDir(std::string_view name) {
        root = std::filesystem::temp_directory_path() / std::string{name};
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
    }

    ~TmpDir() { std::filesystem::remove_all(root); }

    void write(std::string const& rel_path, std::string const& content) {
        auto path = root / rel_path;
        std::filesystem::create_directories(path.parent_path());
        auto file = std::ofstream{path};
        file << content;
    }
};

struct CwdGuard {
    std::filesystem::path previous;

    explicit CwdGuard(std::filesystem::path const& next)
        : previous(std::filesystem::current_path()) {
        std::filesystem::current_path(next);
    }

    ~CwdGuard() { std::filesystem::current_path(previous); }
};

int run_command(auto fn, std::vector<std::string> const& args) {
    auto argv = std::vector<char*>{};
    argv.reserve(args.size());
    for (auto const& arg : args)
        argv.push_back(const_cast<char*>(arg.c_str()));
    return fn(static_cast<int>(argv.size()), argv.data());
}

std::string read_text(std::filesystem::path const& path) {
    auto file = std::ifstream{path};
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

void test_apply_workspace_defaults_for_member_package_and_build() {
    auto workspace = manifest::parse(R"(
[workspace]
members = ["core"]

[workspace.package]
version = "1.2.3"
authors = ["misut"]
license = "MIT"
standard = 26
build-system = "cmake"

[workspace.build]
cxxflags = ["-Wall"]

[workspace.build.debug]
cxxflags = ["-fsanitize=address"]
)");

    auto member = manifest::parse(R"(
[package]
name = "core"
type = "lib"

[build]
cxxflags = ["-Wextra"]
)");

    auto applied = manifest::apply_workspace_defaults(member, workspace);
    check(applied.version == "1.2.3", "workspace defaults: version filled");
    check(applied.authors.size() == 1 && applied.authors[0] == "misut",
          "workspace defaults: authors filled");
    check(applied.license == "MIT", "workspace defaults: license filled");
    check(applied.standard == 26, "workspace defaults: standard filled");
    check(applied.build_system == "cmake", "workspace defaults: build-system filled");
    check(applied.build.cxxflags.size() == 2, "workspace defaults: build flags merged");
    check(applied.build.cxxflags[0] == "-Wall", "workspace defaults: workspace flag first");
    check(applied.build.cxxflags[1] == "-Wextra", "workspace defaults: member flag kept");
    check(applied.build_debug.cxxflags.size() == 1 &&
              applied.build_debug.cxxflags[0] == "-fsanitize=address",
          "workspace defaults: debug build flags inherited");

    auto explicit_member = manifest::parse(R"(
[package]
name = "app"
version = "9.9.9"
license = "Apache-2.0"
standard = 23
build-system = "exon"
)");
    auto explicit_applied = manifest::apply_workspace_defaults(explicit_member, workspace);
    check(explicit_applied.version == "9.9.9", "workspace defaults: explicit version wins");
    check(explicit_applied.license == "Apache-2.0", "workspace defaults: explicit license wins");
    check(explicit_applied.standard == 23, "workspace defaults: explicit standard wins");
    check(explicit_applied.build_system == "exon",
          "workspace defaults: explicit build-system wins");
}

void test_select_workspace_members_orders_dependency_closure() {
    TmpDir tmp{"exon_test_workspace_selection"};
    tmp.write("exon.toml", R"(
[workspace]
members = ["app", "util", "core"]

[workspace.package]
version = "0.1.0"
license = "MIT"

[workspace.build]
cxxflags = ["-Wall"]
)");
    tmp.write("core/exon.toml", R"(
[package]
name = "core"
type = "lib"
)");
    tmp.write("util/exon.toml", R"(
[package]
name = "util"
type = "lib"

[dependencies.workspace]
core = true
)");
    tmp.write("app/exon.toml", R"(
[package]
name = "app"
type = "bin"

[dependencies.workspace]
util = true
core = true
)");

    auto workspace_manifest = manifest::system::load((tmp.root / "exon.toml").string());
    auto all = commands::select_workspace_members(tmp.root, workspace_manifest, {}, {}, {}, false, false);
    check(all.members.size() == 3, "workspace selection: all members loaded");
    check(all.members[0].name == "core", "workspace selection: core ordered first");
    check(all.members[1].name == "util", "workspace selection: util ordered second");
    check(all.members[2].name == "app", "workspace selection: app ordered third");
    check(all.members[0].raw_manifest.version == "0.1.0",
          "workspace selection: workspace defaults applied to members");
    check(all.members[0].raw_manifest.build.cxxflags.size() == 1 &&
              all.members[0].raw_manifest.build.cxxflags[0] == "-Wall",
          "workspace selection: workspace build defaults applied");

    auto app_only = commands::select_workspace_members(
        tmp.root, workspace_manifest, {}, {"app"}, {}, true, false);
    check(app_only.members.size() == 3, "workspace selection: dependency closure includes libs");
    check(app_only.members[0].name == "core", "workspace closure: core ordered first");
    check(app_only.members[1].name == "util", "workspace closure: util ordered second");
    check(app_only.members[2].name == "app", "workspace closure: app ordered third");

    bool threw = false;
    try {
        (void)commands::select_workspace_members(
            tmp.root, workspace_manifest, {}, {"app"}, {"core"}, true, false);
    } catch (std::runtime_error const&) {
        threw = true;
    }
    check(threw, "workspace selection: excluding required dependency fails");
}

void test_cmd_init_workspace_and_cmd_new_updates_members() {
    TmpDir tmp{"exon_test_init_new_workspace"};
    {
        CwdGuard cwd{tmp.root};
        check(run_command(commands::cmd_init, {"exon", "init", "--workspace"}) == 0,
              "commands: init workspace succeeds");
        check(std::filesystem::exists(tmp.root / "exon.toml"),
              "commands: workspace manifest created");

        check(run_command(commands::cmd_new, {"exon", "new", "--lib", "core"}) == 0,
              "commands: new lib succeeds");
        check(run_command(commands::cmd_new, {"exon", "new", "--bin", "app"}) == 0,
              "commands: new bin succeeds");
    }

    auto text = read_text(tmp.root / "exon.toml");
    check(text.contains("members = [\"core\", \"app\"]"),
          "commands: workspace members appended in order");
    check(std::filesystem::exists(tmp.root / "core" / "src" / "core.cppm"),
          "commands: new lib creates module source");
    check(std::filesystem::exists(tmp.root / "app" / "src" / "main.cpp"),
          "commands: new bin creates main.cpp");
}

void test_generate_workspace_root_cmake_uses_single_member_entries() {
    TmpDir tmp{"exon_test_workspace_root_cmake"};
    tmp.write("exon.toml", R"(
[workspace]
members = ["app", "core"]
)");
    tmp.write("core/exon.toml", R"(
[package]
name = "core"
version = "0.1.0"
type = "lib"
)");
    tmp.write("app/exon.toml", R"(
[package]
name = "app"
version = "0.1.0"
type = "bin"

[dependencies.workspace]
core = true
)");

    auto workspace_manifest = manifest::system::load((tmp.root / "exon.toml").string());
    auto selection = commands::select_workspace_members(
        tmp.root, workspace_manifest, {}, {"app"}, {}, true, false);
    auto cmake = commands::generate_workspace_root_cmake(tmp.root, tmp.root, selection.members);

    check(cmake.contains("project(exon_test_workspace_root_cmake LANGUAGES CXX)"),
          "workspace root cmake: project emitted");
    check(cmake.contains("set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD"),
          "workspace root cmake: import std enabled");
    check(cmake.contains("add_subdirectory(\"core\" ${CMAKE_BINARY_DIR}/members/core)"),
          "workspace root cmake: core added once");
    check(cmake.contains("add_subdirectory(\"app\" ${CMAKE_BINARY_DIR}/members/app)"),
          "workspace root cmake: app added once");
}

void test_dependency_graph_paths_and_dedupe() {
    auto m = manifest::Manifest{};
    m.name = "app";
    m.path_deps.emplace("middle", "../middle");
    m.path_deps.emplace("leaf", "../leaf");

    auto result = fetch::FetchResult{};
    result.deps.push_back(fetch::FetchedDep{
        .key = "leaf",
        .name = "leaf",
        .package_name = "leaf",
        .path = std::filesystem::path{"/tmp/leaf"},
        .is_path = true,
    });
    result.deps.push_back(fetch::FetchedDep{
        .key = "middle",
        .name = "middle",
        .package_name = "middle",
        .path = std::filesystem::path{"/tmp/middle"},
        .is_path = true,
        .dependency_names = {"leaf"},
    });

    auto graph = commands::build_dependency_graph("app", m, result, false);
    check(graph.root_name == "app", "dependency graph: root name kept");
    check(graph.root_dependencies.size() == 2,
          "dependency graph: direct dependencies deduped");
    check(graph.nodes.size() == 2, "dependency graph: nodes captured");
    check(graph.nodes.at("middle").dependencies.size() == 1 &&
              graph.nodes.at("middle").dependencies[0] == "leaf",
          "dependency graph: transitive edge captured");

    auto paths = commands::dependency_paths(graph, "leaf");
    check(paths.size() == 2, "dependency graph: why finds direct and transitive paths");
    auto has_direct = std::ranges::any_of(paths, [](auto const& path) {
        return path.size() == 2 && path[0] == "app" && path[1] == "leaf";
    });
    auto has_transitive = std::ranges::any_of(paths, [](auto const& path) {
        return path.size() == 3 && path[0] == "app" && path[1] == "middle" &&
               path[2] == "leaf";
    });
    check(has_direct, "dependency graph: direct why path preserved");
    check(has_transitive, "dependency graph: transitive why path preserved");
}

void test_cmd_run_rejects_android_before_build() {
    TmpDir tmp{"exon_test_run_android_early_error"};
    tmp.write("exon.toml", R"([package]
name = "app"
version = "0.1.0"
type = "bin"
standard = 23
)");
    tmp.write("src/main.cpp", "int main() { return 0; }\n");

    CwdGuard cwd{tmp.root};
    auto rc = run_command(commands::cmd_run,
                          {"exon", "run", "--target", "aarch64-linux-android"});

    check(rc == 1, "cmd run android: rejected");
    check(!std::filesystem::exists(tmp.root / ".exon"),
          "cmd run android: rejected before build output");
}

int main() {
    test_apply_workspace_defaults_for_member_package_and_build();
    test_select_workspace_members_orders_dependency_closure();
    test_cmd_init_workspace_and_cmd_new_updates_members();
    test_generate_workspace_root_cmake_uses_single_member_entries();
    test_dependency_graph_paths_and_dedupe();
    test_cmd_run_rejects_android_before_build();

    if (failures > 0) {
        std::println("{} test(s) failed", failures);
        return 1;
    }

    std::println("test_commands: all passed");
    return 0;
}
