export module commands;
import std;
import cli;
import core;
import cppx.fs;
import cppx.fs.system;
import cppx.terminal;
import toml;
import manifest;
import manifest.system;
import build;
import build.system;
import debug;
import debug.system;
import fetch;
import fetch.system;
import lock;
import lock.system;
import reporting;
import reporting.system;
import templates;
import toolchain;
import toolchain.system;

namespace terminal = cppx::terminal;

export namespace commands {

#ifndef EXON_PKG_VERSION
#define EXON_PKG_VERSION "dev"
#endif
constexpr auto version = EXON_PKG_VERSION;

int command_error(std::string_view msg) {
    return reporting::emit(reporting::Diagnostic{.message = std::string{msg}},
                           reporting::system::stderr_is_tty());
}

int command_error(std::string_view msg, std::string_view hint) {
    return reporting::emit(
        reporting::Diagnostic{
            .message = std::string{msg},
            .hints = {std::string{hint}},
        },
        reporting::system::stderr_is_tty());
}

std::string usage_text() {
    return cli::usage(
        "exon",
        {
            cli::Section{
                "commands",
                {
                    {"init [--lib|--workspace] [name]", "create a new package or workspace"},
                    {"new --lib|--bin <name>", "create a new workspace member"},
                    {"info", "show package information"},
                    {"build [--release] [--target <t>] [--member a,b] [--exclude x,y] "
                     "[--output human|json|wrapped|raw] [--color auto|always|never] "
                     "[--progress auto|always|never] [--unicode auto|always|never] "
                     "[--hyperlinks auto|always|never]",
                     "build the project"},
                    {"dist [--release] [--target <t>] [--output-dir <dir>] [--version <v>] "
                     "[--output human|json|wrapped|raw]",
                     "build and package a release-compatible archive"},
                    {"status [--output human|json]",
                     "inspect project, toolchain, and terminal status"},
                    {"doctor [--output human|json]", "alias for status"},
                    {"check [--release] [--target <t>] [--member a,b] [--exclude x,y]",
                     "check syntax without linking"},
                    {"run [--release] [--target <t>] [--member <name>] [args]",
                     "build and run the project"},
                    {"debug [--release] [--debugger auto|lldb|gdb|devenv|cdb|<path>] "
                     "[--member <name>] [--exclude x,y] [-- <args...>]",
                     "build and open the selected native executable in a native debugger"},
                    {"test [--release] [--target <t>] [--member a,b] [--exclude x,y] [--timeout "
                     "<sec>] "
                     "[--output human|json|wrapped|raw] [--show-output failed|all|none] "
                     "[--color auto|always|never] [--progress auto|always|never] "
                     "[--unicode auto|always|never] [--hyperlinks auto|always|never]",
                     "build and run tests"},
                    {"clean [--member a,b] [--exclude x,y]", "remove build artifacts"},
                    {"add [--dev] <pkg> <ver> [--features a,b] [--no-default-features]",
                     "add a git dependency"},
                    {"add [--dev] --path <name> <path>", "add a local path dependency"},
                    {"add [--dev] --workspace <name>", "add a workspace member dependency"},
                    {"add [--dev] --vcpkg <name> <ver> [--features a,b,c]",
                     "add a vcpkg dependency"},
                    {"add [--dev] --cmake <name> --repo <url> --tag <tag> --targets <targets> "
                     "[--option K=V] [--shallow false]",
                     "add a raw CMake dependency"},
                    {"add [--dev] --git <repo> --version <ver> --subdir <dir> [--name <n>]",
                     "add a git dep pointing to a subdirectory"},
                    {"remove <pkg>", "remove a dependency"},
                    {"outdated [pkg...] [--member a,b] [--exclude x,y] [--output human|json]",
                     "check git dependencies for newer versions"},
                    {"update [pkg...] [--dry-run] [--precise <version>] [--member a,b] [--exclude "
                     "x,y]",
                     "update lockfile entries to latest compatible versions"},
                    {"tree [--member a,b] [--exclude x,y] [--dev] [--features] [--output "
                     "human|json]",
                     "show the resolved dependency graph"},
                    {"why <pkg> [--member a,b] [--exclude x,y] [--dev] [--output human|json]",
                     "show why a package is in the dependency graph"},
                    {"sync [--member a,b] [--exclude x,y]", "sync CMakeLists.txt with exon.toml"},
                    {"fmt", "format source files with clang-format"},
                    {"version", "show exon version"},
                }},
            cli::Section{
                "targets",
                {
                    {"wasm32-wasi", "WebAssembly (requires wasi-sdk via intron or WASI_SDK_PATH)"},
                    {"aarch64-linux-android",
                     "Android arm64 (requires android-ndk via intron or ANDROID_NDK_HOME)"},
                }},
        });
}

std::span<std::string_view const> command_names() {
    static constexpr auto names = std::array<std::string_view, 21>{
        "init",     "new",    "info",  "build", "dist",  "status", "doctor",
        "check",    "run",    "debug", "test",  "clean", "add",    "remove",
        "outdated", "update", "sync",  "tree",  "why",   "fmt",    "version",
    };
    return std::span{names};
}

std::size_t edit_distance(std::string_view lhs, std::string_view rhs) {
    auto previous = std::vector<std::size_t>(rhs.size() + 1);
    auto current = std::vector<std::size_t>(rhs.size() + 1);
    std::iota(previous.begin(), previous.end(), std::size_t{0});

    for (std::size_t i = 0; i < lhs.size(); ++i) {
        current[0] = i + 1;
        for (std::size_t j = 0; j < rhs.size(); ++j) {
            auto substitution = previous[j] + (lhs[i] == rhs[j] ? 0 : 1);
            current[j + 1] = std::min({
                previous[j + 1] + 1,
                current[j] + 1,
                substitution,
            });
        }
        std::swap(previous, current);
    }
    return previous.back();
}

std::optional<std::string_view> suggest_command(std::string_view command) {
    std::optional<std::string_view> best;
    auto best_distance = std::numeric_limits<std::size_t>::max();
    for (auto name : command_names()) {
        auto distance = edit_distance(command, name);
        if (distance < best_distance) {
            best = name;
            best_distance = distance;
        }
    }
    if (best && best_distance <= 2)
        return best;
    return std::nullopt;
}

int unknown_command(std::string_view command) {
    auto diag = reporting::Diagnostic{
        .message = std::format("unknown command: {}", command),
    };
    if (auto suggestion = suggest_command(command))
        diag.hints.push_back(std::format("did you mean '{}'?", *suggestion));
    return reporting::emit(diag, reporting::system::stderr_is_tty());
}

void print_status(terminal::StatusKind status, std::string_view message) {
    std::println("{} {}", terminal::status_cell(status, reporting::system::stdout_is_tty()),
                 message);
}

manifest::Manifest load_manifest(std::filesystem::path const& project_root) {
    auto manifest_path = project_root / "exon.toml";
    if (!std::filesystem::exists(manifest_path)) {
        throw std::runtime_error("exon.toml not found. run 'exon init' first");
    }
    return manifest::system::load(manifest_path.string());
}

manifest::Manifest load_manifest() {
    return load_manifest(std::filesystem::current_path());
}

toolchain::Platform effective_platform(std::string_view target);
manifest::Manifest resolve_manifest(manifest::Manifest m, std::string_view target = {});
bool check_platform(manifest::Manifest const& m, std::string_view target = {});
std::string read_text(std::filesystem::path const& path);
void write_text(std::filesystem::path const& path, std::string const& content);

struct WorkspaceMember {
    std::string name;
    std::filesystem::path path;
    manifest::Manifest raw_manifest;
    manifest::Manifest resolved_manifest;
    std::set<std::string> workspace_dep_names;
    std::set<std::string> dev_workspace_dep_names;
    bool runnable = false;
};

struct WorkspaceSelection {
    std::vector<WorkspaceMember> members;
    bool explicitly_selected = false;
};

std::vector<cli::ArgDef> workspace_select_defs(std::vector<cli::ArgDef> defs = {}) {
    defs.push_back(cli::ListOption{"--member"});
    defs.push_back(cli::ListOption{"--exclude"});
    return defs;
}

std::vector<cli::ArgDef> reporting_defs(std::vector<cli::ArgDef> defs = {},
                                        bool include_show_output = false) {
    defs.push_back(cli::Option{"--output"});
    if (include_show_output)
        defs.push_back(cli::Option{"--show-output"});
    defs.push_back(cli::Option{"--color"});
    defs.push_back(cli::Option{"--progress"});
    defs.push_back(cli::Option{"--unicode"});
    defs.push_back(cli::Option{"--hyperlinks"});
    return defs;
}

std::filesystem::path workspace_build_root(std::filesystem::path const& workspace_root,
                                           bool release, std::string_view target = {}) {
    auto profile = release ? "release" : "debug";
    auto base = workspace_root / ".exon" / "workspace";
    if (!target.empty())
        base /= target;
    return base / profile;
}

std::string sanitize_workspace_name(std::string_view name) {
    auto value = std::string{name};
    if (value.empty())
        value = "workspace";
    for (auto& ch : value) {
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'))
            ch = '_';
    }
    if (!std::isalpha(static_cast<unsigned char>(value.front())) && value.front() != '_')
        value.insert(value.begin(), '_');
    return value;
}

std::string quote_cmake_path(std::filesystem::path const& path) {
    return std::format("\"{}\"", path.generic_string());
}

std::string workspace_display_name(WorkspaceMember const& member,
                                   std::filesystem::path const& workspace_root) {
    auto rel = std::filesystem::relative(member.path, workspace_root);
    return std::format("{} ({})", member.name, rel.generic_string());
}

bool manifest_has_any_dependencies(manifest::Manifest const& m) {
    return !m.dependencies.empty() || !m.path_deps.empty() || !m.workspace_deps.empty() ||
           !m.vcpkg_deps.empty() || !m.subdir_deps.empty() || !m.featured_deps.empty() ||
           !m.cmake_deps.empty() || !m.dev_dependencies.empty() || !m.dev_path_deps.empty() ||
           !m.dev_workspace_deps.empty() || !m.dev_vcpkg_deps.empty() ||
           !m.dev_subdir_deps.empty() || !m.dev_featured_deps.empty() ||
           !m.dev_cmake_deps.empty() || !m.find_deps.empty() || !m.dev_find_deps.empty();
}

bool manifest_has_git_dependencies(manifest::Manifest const& m, bool include_dev = false) {
    auto regular = !m.dependencies.empty() || !m.subdir_deps.empty() || !m.featured_deps.empty();
    auto dev = include_dev && (!m.dev_dependencies.empty() || !m.dev_subdir_deps.empty() ||
                               !m.dev_featured_deps.empty());
    return regular || dev;
}

lock::LockedDep const* find_locked_by_name(lock::LockFile const& lf, std::string const& name) {
    for (auto const& dep : lf.packages) {
        if (dep.name == name)
            return &dep;
    }
    return nullptr;
}

std::string display_version(std::string const& version) {
    return version.empty() ? "-" : version;
}

void print_dependency_statuses(std::vector<fetch::DependencyUpdateStatus> const& statuses) {
    if (statuses.empty()) {
        print_status(terminal::StatusKind::skip, "no matching dependencies");
        return;
    }
    std::println("{:<28} {:<12} {:<12} {:<12} {:<18} {}", "dependency", "current", "compatible",
                 "latest", "status", "note");
    for (auto const& status : statuses) {
        auto note = status.reason;
        if (status.is_dev)
            note = note.empty() ? "dev" : std::format("dev; {}", note);
        std::println("{:<28} {:<12} {:<12} {:<12} {:<18} {}", status.key,
                     display_version(status.current_version),
                     display_version(status.latest_compatible_version),
                     display_version(status.latest_version), status.status, note);
    }
}

void emit_dependency_status_json(std::vector<fetch::DependencyUpdateStatus> const& statuses) {
    for (auto const& status : statuses) {
        reporting::emit_json_event(
            "dependency-status",
            {
                reporting::json_string("key", status.key),
                reporting::json_string("package", status.package_name),
                reporting::json_string("requirement", status.requirement),
                reporting::json_string("current", status.current_version),
                reporting::json_string("latest_compatible", status.latest_compatible_version),
                reporting::json_string("latest", status.latest_version),
                reporting::json_string("status", status.status),
                reporting::json_string("reason", status.reason),
                reporting::json_bool("dev", status.is_dev),
            });
    }
    reporting::emit_json_event(
        "summary",
        {
            reporting::json_number("dependencies", static_cast<long long>(statuses.size())),
        });
}

std::vector<fetch::DependencyUpdateStatus>
dependency_statuses_for_project(std::filesystem::path const& project_root,
                                manifest::Manifest const& manifest,
                                std::vector<std::string> const& filters) {
    return fetch::system::inspect_dependencies(
        {
            .manifest = manifest,
            .project_root = project_root,
            .lock_path = project_root / "exon.lock",
            .include_dev = true,
        },
        filters);
}

void print_update_diff(lock::LockFile const& before, lock::LockFile const& after, bool dry_run) {
    auto changed = 0;
    for (auto const& dep : after.packages) {
        auto old = find_locked_by_name(before, dep.name);
        if (!old) {
            ++changed;
            std::println("{} {} {}",
                         terminal::status_cell(dry_run ? terminal::StatusKind::skip
                                                       : terminal::StatusKind::ok,
                                               reporting::system::stdout_is_tty()),
                         dry_run ? "would lock" : "locked",
                         std::format("{} {}", dep.name, dep.version));
            continue;
        }
        if (old->version != dep.version || old->commit != dep.commit) {
            ++changed;
            std::println("{} {} {} {} -> {}",
                         terminal::status_cell(dry_run ? terminal::StatusKind::skip
                                                       : terminal::StatusKind::ok,
                                               reporting::system::stdout_is_tty()),
                         dry_run ? "would update" : "updated", dep.name, old->version, dep.version);
        }
    }
    if (changed == 0)
        print_status(terminal::StatusKind::ok,
                     dry_run ? "lockfile already up to date" : "dependencies already up to date");
}

struct DependencyGraphNode {
    std::string name;
    std::string key;
    std::string version;
    bool is_dev = false;
    bool is_path = false;
    std::vector<std::string> features;
    bool default_features = true;
    std::vector<std::string> aliases;
    std::vector<std::string> dependencies;
};

struct DependencyGraph {
    std::string root_name;
    std::map<std::string, DependencyGraphNode> nodes;
    std::vector<std::string> root_dependencies;
};

void add_unique_string(std::vector<std::string>& values, std::string const& value) {
    if (value.empty())
        return;
    if (std::ranges::find(values, value) == values.end())
        values.push_back(value);
}

std::string package_basename(std::string_view key) {
    auto slash = key.find_last_of('/');
    if (slash == std::string_view::npos)
        return std::string{key};
    return std::string{key.substr(slash + 1)};
}

bool fetched_dep_matches_key(fetch::FetchedDep const& dep, std::string const& key) {
    if (dep.key == key || dep.name == key || dep.package_name == key)
        return true;
    if (std::ranges::find(dep.aliases, key) != dep.aliases.end())
        return true;
    return dep.package_name == package_basename(key);
}

std::optional<std::string> graph_name_for_key(fetch::FetchResult const& result,
                                              std::string const& key) {
    for (auto const& dep : result.deps) {
        if (fetched_dep_matches_key(dep, key))
            return dep.package_name;
    }
    return std::nullopt;
}

void add_graph_root_dep(DependencyGraph& graph, fetch::FetchResult const& result,
                        std::string const& key) {
    if (auto name = graph_name_for_key(result, key))
        add_unique_string(graph.root_dependencies, *name);
}

DependencyGraph build_dependency_graph(std::string root_name, manifest::Manifest const& m,
                                       fetch::FetchResult const& result, bool include_dev) {
    DependencyGraph graph{.root_name = std::move(root_name)};
    if (graph.root_name.empty())
        graph.root_name = m.name.empty() ? "package" : m.name;

    for (auto const& dep : result.deps) {
        auto node = DependencyGraphNode{
            .name = dep.package_name,
            .key = dep.key,
            .version = dep.version,
            .is_dev = dep.is_dev,
            .is_path = dep.is_path,
            .features = dep.features,
            .default_features = dep.default_features,
            .aliases = dep.aliases,
            .dependencies = dep.dependency_names,
        };
        graph.nodes[node.name] = std::move(node);
    }

    for (auto const& [key, _] : m.dependencies)
        add_graph_root_dep(graph, result, key);
    for (auto const& [key, _] : m.featured_deps)
        add_graph_root_dep(graph, result, key);
    for (auto const& [key, _] : m.subdir_deps)
        add_graph_root_dep(graph, result, key);
    for (auto const& [key, _] : m.path_deps)
        add_graph_root_dep(graph, result, key);
    for (auto const& key : m.workspace_deps)
        add_graph_root_dep(graph, result, key);

    if (include_dev) {
        for (auto const& [key, _] : m.dev_dependencies)
            add_graph_root_dep(graph, result, key);
        for (auto const& [key, _] : m.dev_featured_deps)
            add_graph_root_dep(graph, result, key);
        for (auto const& [key, _] : m.dev_subdir_deps)
            add_graph_root_dep(graph, result, key);
        for (auto const& [key, _] : m.dev_path_deps)
            add_graph_root_dep(graph, result, key);
        for (auto const& key : m.dev_workspace_deps)
            add_graph_root_dep(graph, result, key);
    }

    return graph;
}

std::string join_strings(std::vector<std::string> const& values, std::string_view separator = ",") {
    auto out = std::string{};
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0)
            out += separator;
        out += values[i];
    }
    return out;
}

std::string dependency_node_label(DependencyGraphNode const& node, bool show_features = false) {
    auto label = node.name;
    if (!node.version.empty() && !node.is_path)
        label += std::format(" {}", node.version);
    if (node.is_path)
        label += " (path)";
    if (node.is_dev)
        label += " (dev)";
    if (show_features && (!node.features.empty() || !node.default_features)) {
        auto parts = std::vector<std::string>{};
        if (!node.features.empty())
            parts.push_back(std::format("features: {}", join_strings(node.features, ",")));
        if (!node.default_features)
            parts.push_back("default-features=false");
        label += std::format(" [{}]", join_strings(parts, "; "));
    }
    return label;
}

std::string dependency_label(DependencyGraph const& graph, std::string const& name,
                             bool show_features = false) {
    if (auto it = graph.nodes.find(name); it != graph.nodes.end())
        return dependency_node_label(it->second, show_features);
    return name;
}

void print_dependency_tree_node(DependencyGraph const& graph, std::string const& name,
                                std::string const& prefix, bool last, std::set<std::string>& seen,
                                bool show_features) {
    auto it = graph.nodes.find(name);
    auto label = it == graph.nodes.end() ? name : dependency_node_label(it->second, show_features);
    std::print("{}{}{}", prefix, last ? "`- " : "+- ", label);
    if (!seen.insert(name).second) {
        std::println(" (*)");
        return;
    }
    std::println("");
    if (it == graph.nodes.end())
        return;
    auto next_prefix = prefix + (last ? "   " : "|  ");
    for (std::size_t i = 0; i < it->second.dependencies.size(); ++i) {
        print_dependency_tree_node(graph, it->second.dependencies[i], next_prefix,
                                   i + 1 == it->second.dependencies.size(), seen, show_features);
    }
}

void print_dependency_tree(DependencyGraph const& graph, bool show_features) {
    std::println("{}", graph.root_name);
    auto seen = std::set<std::string>{};
    for (std::size_t i = 0; i < graph.root_dependencies.size(); ++i) {
        print_dependency_tree_node(graph, graph.root_dependencies[i], "",
                                   i + 1 == graph.root_dependencies.size(), seen, show_features);
    }
}

std::string json_string_array(std::vector<std::string> const& values) {
    auto out = std::string{"["};
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0)
            out += ",";
        out += std::format("\"{}\"", reporting::json_escape(values[i]));
    }
    out += "]";
    return out;
}

void emit_dependency_tree_json(DependencyGraph const& graph) {
    reporting::emit_json_event("dependency-tree-root",
                               {
                                   reporting::json_string("name", graph.root_name),
                                   reporting::JsonField{
                                       .key = "dependencies",
                                       .value = json_string_array(graph.root_dependencies),
                                       .quote = false,
                                   },
                               });
    for (auto const& [_, node] : graph.nodes) {
        reporting::emit_json_event(
            "dependency-tree-node",
            {
                reporting::json_string("name", node.name),
                reporting::json_string("key", node.key),
                reporting::json_string("version", node.version),
                reporting::json_bool("dev", node.is_dev),
                reporting::json_bool("path", node.is_path),
                reporting::json_bool("default_features", node.default_features),
                reporting::JsonField{
                    .key = "features",
                    .value = json_string_array(node.features),
                    .quote = false,
                },
                reporting::JsonField{
                    .key = "dependencies",
                    .value = json_string_array(node.dependencies),
                    .quote = false,
                },
            });
    }
    reporting::emit_json_event(
        "summary",
        {
            reporting::json_number("dependencies", static_cast<long long>(graph.nodes.size())),
        });
}

bool dependency_node_matches(DependencyGraphNode const& node, std::string const& target) {
    if (node.name == target || node.key == target)
        return true;
    return std::ranges::find(node.aliases, target) != node.aliases.end();
}

void collect_dependency_paths(DependencyGraph const& graph, std::string const& name,
                              std::string const& target, std::vector<std::string>& current,
                              std::set<std::string>& visiting,
                              std::vector<std::vector<std::string>>& paths) {
    auto it = graph.nodes.find(name);
    if (it == graph.nodes.end())
        return;
    current.push_back(name);
    if (dependency_node_matches(it->second, target))
        paths.push_back(current);
    if (visiting.insert(name).second) {
        for (auto const& child : it->second.dependencies)
            collect_dependency_paths(graph, child, target, current, visiting, paths);
        visiting.erase(name);
    }
    current.pop_back();
}

std::vector<std::vector<std::string>> dependency_paths(DependencyGraph const& graph,
                                                       std::string const& target) {
    auto paths = std::vector<std::vector<std::string>>{};
    for (auto const& root_dep : graph.root_dependencies) {
        auto current = std::vector<std::string>{graph.root_name};
        auto visiting = std::set<std::string>{};
        collect_dependency_paths(graph, root_dep, target, current, visiting, paths);
    }
    return paths;
}

void print_dependency_paths(DependencyGraph const& graph,
                            std::vector<std::vector<std::string>> const& paths) {
    for (auto const& path : paths) {
        auto parts = std::vector<std::string>{};
        for (std::size_t i = 0; i < path.size(); ++i) {
            if (i == 0)
                parts.push_back(path[i]);
            else
                parts.push_back(dependency_label(graph, path[i]));
        }
        std::println("{}", join_strings(parts, " -> "));
    }
}

void emit_dependency_paths_json(std::string const& target,
                                std::vector<std::vector<std::string>> const& paths) {
    for (auto const& path : paths) {
        reporting::emit_json_event("dependency-path", {
                                                          reporting::json_string("target", target),
                                                          reporting::JsonField{
                                                              .key = "path",
                                                              .value = json_string_array(path),
                                                              .quote = false,
                                                          },
                                                      });
    }
    reporting::emit_json_event(
        "summary", {
                       reporting::json_number("paths", static_cast<long long>(paths.size())),
                   });
}

WorkspaceMember load_workspace_member(std::filesystem::path const& workspace_root,
                                      manifest::Manifest const& workspace_manifest,
                                      std::filesystem::path const& member_path,
                                      std::string_view target) {
    auto raw_member =
        manifest::apply_workspace_defaults(load_manifest(member_path), workspace_manifest);
    if (!check_platform(raw_member, target))
        throw std::runtime_error(std::format(
            "workspace member '{}' does not support target '{}'",
            raw_member.name.empty() ? member_path.filename().string() : raw_member.name,
            target.empty() ? toolchain::detect_host_platform().to_string() : std::string{target}));
    auto resolved_member = resolve_manifest(raw_member, target);
    auto workspace_dep_names = resolved_member.workspace_deps;
    auto dev_workspace_dep_names = resolved_member.dev_workspace_deps;
    auto runnable = resolved_member.type != "lib";
    return {
        .name = resolved_member.name,
        .path = member_path,
        .raw_manifest = std::move(raw_member),
        .resolved_manifest = std::move(resolved_member),
        .workspace_dep_names = std::move(workspace_dep_names),
        .dev_workspace_dep_names = std::move(dev_workspace_dep_names),
        .runnable = runnable,
    };
}

std::vector<WorkspaceMember> load_workspace_members(std::filesystem::path const& workspace_root,
                                                    manifest::Manifest const& workspace_manifest,
                                                    std::string_view target) {
    auto members = std::vector<WorkspaceMember>{};
    auto seen_names = std::set<std::string>{};
    for (auto const& member : workspace_manifest.workspace_members) {
        auto member_path = workspace_root / member;
        if (!std::filesystem::exists(member_path / "exon.toml"))
            throw std::runtime_error(std::format("{} has no exon.toml", member));
        auto info = load_workspace_member(workspace_root, workspace_manifest, member_path, target);
        if (info.name.empty())
            throw std::runtime_error(std::format("workspace member '{}' is missing [package].name",
                                                 member_path.string()));
        if (!seen_names.insert(info.name).second)
            throw std::runtime_error(
                std::format("duplicate workspace member package name '{}'", info.name));
        members.push_back(std::move(info));
    }
    return members;
}

WorkspaceSelection select_workspace_members(std::filesystem::path const& workspace_root,
                                            manifest::Manifest const& workspace_manifest,
                                            std::string_view target,
                                            std::vector<std::string> const& requested_members,
                                            std::vector<std::string> const& excluded_members,
                                            bool include_dependencies = false,
                                            bool include_dev_workspace = false) {
    auto members = load_workspace_members(workspace_root, workspace_manifest, target);
    auto by_name = std::map<std::string, WorkspaceMember const*>{};
    for (auto const& member : members)
        by_name.emplace(member.name, &member);

    auto excluded = std::set<std::string>{excluded_members.begin(), excluded_members.end()};
    for (auto const& name : excluded) {
        if (!by_name.contains(name))
            throw std::runtime_error(std::format("workspace member '{}' not found", name));
    }

    auto requested = std::vector<std::string>{};
    if (!requested_members.empty()) {
        for (auto const& name : requested_members) {
            if (!by_name.contains(name))
                throw std::runtime_error(std::format("workspace member '{}' not found", name));
            requested.push_back(name);
        }
    } else {
        for (auto const& member : members) {
            if (!excluded.contains(member.name))
                requested.push_back(member.name);
        }
    }

    auto selected_names = std::set<std::string>{};
    auto add_with_dependencies = [&](this auto const& self, std::string const& name) -> void {
        if (excluded.contains(name))
            throw std::runtime_error(std::format(
                "workspace member '{}' is excluded but required by the current selection", name));
        if (!selected_names.insert(name).second)
            return;
        auto const& member = *by_name.at(name);
        for (auto const& dep : member.workspace_dep_names)
            self(dep);
        if (include_dev_workspace) {
            for (auto const& dep : member.dev_workspace_dep_names)
                self(dep);
        }
    };

    if (include_dependencies) {
        for (auto const& name : requested)
            add_with_dependencies(name);
    } else {
        selected_names.insert(requested.begin(), requested.end());
    }

    for (auto const& name : excluded)
        selected_names.erase(name);
    if (selected_names.empty())
        throw std::runtime_error("no workspace members selected");

    auto ordered_names = std::vector<std::string>{};
    auto states = std::map<std::string, int>{};
    auto visit = [&](this auto const& self, std::string const& name) -> void {
        auto state = states[name];
        if (state == 2)
            return;
        if (state == 1)
            throw std::runtime_error(
                std::format("workspace dependency cycle detected at '{}'", name));
        states[name] = 1;
        auto const& member = *by_name.at(name);
        for (auto const& dep : member.workspace_dep_names) {
            if (selected_names.contains(dep))
                self(dep);
        }
        if (include_dev_workspace) {
            for (auto const& dep : member.dev_workspace_dep_names) {
                if (selected_names.contains(dep))
                    self(dep);
            }
        }
        states[name] = 2;
        ordered_names.push_back(name);
    };
    for (auto const& name : requested) {
        if (selected_names.contains(name))
            visit(name);
    }
    for (auto const& name : selected_names) {
        if (!states.contains(name))
            visit(name);
    }

    WorkspaceSelection selection{
        .explicitly_selected = !requested_members.empty(),
    };
    for (auto const& name : ordered_names) {
        auto it = std::ranges::find_if(
            members, [&](WorkspaceMember const& member) { return member.name == name; });
        if (it != members.end())
            selection.members.push_back(*it);
    }
    return selection;
}

int run_selected_workspace_members(std::filesystem::path const& workspace_root,
                                   WorkspaceSelection const& selection,
                                   std::function<int(WorkspaceMember const&)> fn) {
    for (auto const& member : selection.members) {
        auto guard = reporting::ScopedStageContext{workspace_display_name(member, workspace_root)};
        auto rc = fn(member);
        if (rc != 0)
            return rc;
    }
    return 0;
}

void update_workspace_members_list(std::filesystem::path const& workspace_root,
                                   std::string_view member_name) {
    auto manifest_path = workspace_root / "exon.toml";
    auto content = read_text(manifest_path);
    auto workspace_manifest = load_manifest(workspace_root);
    auto member_text = std::string{member_name};
    if (std::ranges::find(workspace_manifest.workspace_members, member_text) !=
        workspace_manifest.workspace_members.end()) {
        return;
    }

    auto marker = std::string{"members = ["};
    auto pos = content.find(marker);
    if (pos == std::string::npos) {
        if (!content.empty() && content.back() != '\n')
            content += '\n';
        content += std::format("\n[workspace]\nmembers = [\"{}\"]\n", member_name);
        write_text(manifest_path, content);
        return;
    }

    auto insert_pos = content.find(']', pos);
    if (insert_pos == std::string::npos)
        throw std::runtime_error("workspace members list is malformed");
    auto before = content.substr(pos, insert_pos - pos);
    auto needs_separator = before.find('"') != std::string::npos;
    content.insert(insert_pos, std::format("{}\"{}\"", needs_separator ? ", " : "", member_name));
    write_text(manifest_path, content);
}

void create_package_files(std::filesystem::path const& target_dir, std::string const& name,
                          bool is_lib) {
    auto manifest_path = target_dir / "exon.toml";
    if (std::filesystem::exists(manifest_path))
        throw std::runtime_error(std::format("{} already exists", manifest_path.string()));

    std::filesystem::create_directories(target_dir / "src");

    auto tmpl = std::string{is_lib ? templates::exon_toml_lib : templates::exon_toml_bin};
    auto name_pos = tmpl.find("name = \"\"");
    if (name_pos != std::string::npos)
        tmpl.replace(name_pos, 9, std::format("name = \"{}\"", name));
    write_text(manifest_path, tmpl);

    auto source_path = target_dir / "src" / (is_lib ? std::format("{}.cppm", name) : "main.cpp");
    auto source = is_lib ? std::format("export module {};\nimport std;\n\n", name)
                         : "import std;\n\nint main() {\n    std::println(\"hello, world!\");\n    "
                           "return 0;\n}\n";
    write_text(source_path, source);
}

std::string generate_workspace_root_cmake(std::filesystem::path const& workspace_root,
                                          std::filesystem::path const& cmake_root,
                                          std::vector<WorkspaceMember> const& members) {
    auto out = std::ostringstream{};
    auto standard = members.empty() ? 23 : 0;
    for (auto const& member : members)
        standard = std::max(standard, member.resolved_manifest.standard);
    auto project_name = sanitize_workspace_name(workspace_root.filename().string());
    auto import_std = standard >= 23;

    out << "# Generated by exon. Do not edit manually.\n\n";
    if (import_std) {
        out << "cmake_minimum_required(VERSION 3.30)\n\n";
        out << std::format("set(CMAKE_CXX_STANDARD {})\n", standard);
        out << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n";
        out << "set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "
               "\"451f2fe2-a8a2-47c3-bc32-94786d8fc91b\")\n";
        out << "set(CMAKE_CXX_MODULE_STD ON)\n";
        out << std::format("project({} LANGUAGES CXX)\n\n", project_name);
    } else {
        out << "cmake_minimum_required(VERSION 3.28)\n";
        out << std::format("project({} LANGUAGES CXX)\n\n", project_name);
        out << std::format("set(CMAKE_CXX_STANDARD {})\n", standard);
        out << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n\n";
    }
    out << "set(CMAKE_EXPORT_COMPILE_COMMANDS ON)\n\n";
    out << "if(MSVC)\n";
    out << "    add_compile_options(/W4 /wd4996 /utf-8)\n";
    out << "    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)\n";
    out << "endif()\n\n";
    out << "set(EXON_ENABLE_TEST_TARGETS OFF)\n\n";
    for (auto const& member : members) {
        auto rel = std::filesystem::relative(member.path, cmake_root).generic_string();
        out << std::format("add_subdirectory({} ${{CMAKE_BINARY_DIR}}/members/{})\n",
                           quote_cmake_path(rel), sanitize_workspace_name(member.name));
    }
    return out.str();
}

manifest::Manifest workspace_build_manifest(std::filesystem::path const& workspace_root,
                                            std::vector<WorkspaceMember> const& members) {
    auto manifest = manifest::Manifest{};
    manifest.name = sanitize_workspace_name(workspace_root.filename().string());
    manifest.type = "lib";
    manifest.standard = members.empty() ? 23 : 0;
    manifest.has_standard = true;
    for (auto const& member : members)
        manifest.standard = std::max(manifest.standard, member.resolved_manifest.standard);
    return manifest;
}

std::optional<reporting::CapabilitySetting> parse_capability_option(std::string_view name,
                                                                    std::string_view value) {
    if (value.empty())
        return std::nullopt;
    auto parsed = reporting::parse_capability_setting(value);
    if (!parsed) {
        throw std::runtime_error(
            std::format("invalid {} '{}': expected auto, always, or never", name, value));
    }
    return parsed;
}

reporting::Options parse_reporting_options(std::string_view output_mode_text = {},
                                           std::string_view show_output_text = {},
                                           std::string_view color_text = {},
                                           std::string_view progress_text = {},
                                           std::string_view unicode_text = {},
                                           std::string_view hyperlinks_text = {}) {
    return {
        .output = build::system::detail::parse_output_mode_text(output_mode_text),
        .show_output = build::system::detail::parse_show_output_text(show_output_text),
        .color = parse_capability_option("--color", color_text),
        .progress = parse_capability_option("--progress", progress_text),
        .unicode = parse_capability_option("--unicode", unicode_text),
        .hyperlinks = parse_capability_option("--hyperlinks", hyperlinks_text),
    };
}

reporting::Options parse_reporting_options(cli::Args const& args,
                                           bool include_show_output = false) {
    return parse_reporting_options(
        args.get("--output"), include_show_output ? args.get("--show-output") : std::string_view{},
        args.get("--color"), args.get("--progress"), args.get("--unicode"),
        args.get("--hyperlinks"));
}

int sync_workspace_member_cmake(WorkspaceMember const& member,
                                std::optional<toolchain::Platform> platform = std::nullopt,
                                bool all_targets = false) {
    auto fetch_manifest =
        all_targets ? manifest::resolve_all_targets(member.raw_manifest) : member.resolved_manifest;
    auto fetch_result = fetch::system::fetch_all({
        .manifest = fetch_manifest,
        .project_root = member.path,
        .lock_path = member.path / "exon.lock",
        .platform = all_targets ? std::nullopt : platform,
    });
    write_text(member.path / "CMakeLists.txt",
               build::generate_portable_cmake(member.raw_manifest, member.path, fetch_result.deps));
    print_status(terminal::StatusKind::ok, "synced CMakeLists.txt");
    return 0;
}

int sync_workspace_root_cmake(std::filesystem::path const& workspace_root,
                              manifest::Manifest const& workspace_manifest,
                              WorkspaceSelection const& selection) {
    if (!workspace_manifest.sync_cmake_in_root)
        return 0;
    write_text(workspace_root / "CMakeLists.txt",
               generate_workspace_root_cmake(workspace_root, workspace_root, selection.members));
    print_status(terminal::StatusKind::ok, "synced CMakeLists.txt");
    return 0;
}

core::FileWrite workspace_member_cmake_write(WorkspaceMember const& member,
                                             toolchain::Platform const& platform) {
    auto fetch_result = fetch::system::fetch_all({
        .manifest = member.resolved_manifest,
        .project_root = member.path,
        .lock_path = member.path / "exon.lock",
        .platform = platform,
    });
    return {
        .text =
            {
                .path = member.path / "CMakeLists.txt",
                .content = build::generate_portable_cmake(member.raw_manifest, member.path,
                                                          fetch_result.deps),
            },
        .success_message = "synced CMakeLists.txt",
    };
}

std::optional<core::FileWrite>
workspace_root_cmake_write(std::filesystem::path const& workspace_root,
                           manifest::Manifest const& workspace_manifest,
                           WorkspaceSelection const& selection) {
    if (!workspace_manifest.sync_cmake_in_root)
        return std::nullopt;
    return core::FileWrite{
        .text =
            {
                .path = workspace_root / "CMakeLists.txt",
                .content = generate_workspace_root_cmake(workspace_root, workspace_root,
                                                         selection.members),
            },
        .success_message = "synced CMakeLists.txt",
    };
}

struct WorkspaceBuildExecution {
    build::BuildRequest request;
    build::BuildPlan plan;
    std::filesystem::path success_path;
    std::string success_label;
};

WorkspaceBuildExecution prepare_workspace_build_execution(
    std::filesystem::path const& workspace_root, manifest::Manifest const& workspace_manifest,
    WorkspaceSelection const& selection, bool release, std::string_view target) {
    auto platform = target.empty() ? toolchain::detect_host_platform()
                                   : *toolchain::platform_from_target(target);

    auto cmake_root = workspace_root / ".exon";
    std::filesystem::create_directories(cmake_root);

    auto synthetic_manifest = workspace_build_manifest(workspace_root, selection.members);
    auto tc = toolchain::system::detect();
    std::string wasm_toolchain_file;
    std::string android_toolchain_file;
    std::string android_abi;
    std::string android_platform;
    std::string android_clang_target;
    std::string android_sysroot;
    bool is_wasm = !target.empty() && target.starts_with("wasm32");
    bool is_android = !target.empty() && target.ends_with("-linux-android");
    if (is_wasm) {
        auto wasm_tc = toolchain::system::detect_wasm(target);
        wasm_toolchain_file = wasm_tc.cmake_toolchain;
        tc.stdlib_modules_json = wasm_tc.modules_json;
        tc.cxx_compiler = wasm_tc.scan_deps;
        tc.sysroot.clear();
        tc.lib_dir.clear();
        tc.has_clang_config = false;
        tc.needs_stdlib_flag = false;
    } else if (is_android) {
        auto android_tc = toolchain::system::detect_android(target);
        android_toolchain_file = android_tc.cmake_toolchain;
        android_abi = android_tc.abi;
        android_platform = android_tc.platform;
        android_clang_target = android_tc.clang_target;
        android_sysroot = android_tc.sysroot;
        tc.stdlib_modules_json = android_tc.modules_json;
        tc.cxx_compiler = android_tc.scan_deps;
        tc.sysroot.clear();
        tc.lib_dir.clear();
        tc.has_clang_config = false;
        tc.needs_stdlib_flag = false;
    }

    auto build_dir = workspace_build_root(workspace_root, release, target);
    build::BuildRequest request{
        .project =
            {
                .root = workspace_root,
                .exon_dir = cmake_root,
                .build_dir = build_dir,
                .profile = release ? "release" : "debug",
                .target = std::string{target},
                .is_wasm = !target.empty(),
            },
        .manifest = synthetic_manifest,
        .toolchain = tc,
        .release = release,
        .configured = std::filesystem::exists(build_dir / "build.ninja"),
        .wasm_toolchain_file = wasm_toolchain_file,
        .android_toolchain_file = android_toolchain_file,
        .android_abi = android_abi,
        .android_platform = android_platform,
        .android_clang_target = android_clang_target,
        .android_sysroot = android_sysroot,
    };

    build::BuildPlan plan{
        .project = request.project,
        .configured = request.configured,
    };
    for (auto const& member : selection.members)
        plan.writes.push_back(workspace_member_cmake_write(member, platform));
    if (auto root_write = workspace_root_cmake_write(workspace_root, workspace_manifest, selection))
        plan.writes.push_back(*root_write);
    plan.writes.push_back({
        .text =
            {
                .path = cmake_root / "CMakeLists.txt",
                .content =
                    generate_workspace_root_cmake(workspace_root, cmake_root, selection.members),
            },
    });

    auto configure_spec =
        build::configure_command(tc, synthetic_manifest, build_dir, cmake_root, release, {}, {},
                                 wasm_toolchain_file, android_toolchain_file, android_abi,
                                 android_platform, android_clang_target, android_sysroot, false);
    configure_spec.cwd = workspace_root;
    plan.configure_steps.push_back({
        .spec = std::move(configure_spec),
        .label = "configuring workspace...",
    });

    auto build_spec = build::build_command(tc, synthetic_manifest, build_dir, {}, target);
    build_spec.cwd = workspace_root;
    plan.build_steps.push_back({
        .spec = std::move(build_spec),
        .label = "building workspace...",
    });

    return {
        .request = std::move(request),
        .plan = std::move(plan),
        .success_path = build_dir,
        .success_label = "build dir",
    };
}

int run_workspace_build(std::filesystem::path const& workspace_root,
                        manifest::Manifest const& workspace_manifest,
                        WorkspaceSelection const& selection, bool release, std::string_view target,
                        reporting::Options const& options) {
    auto scoped_options = reporting::ScopedOptionsContext{options};
    if (options.output != reporting::OutputMode::raw) {
        auto started = std::chrono::steady_clock::now();
        auto project = core::ProjectContext{
            .root = workspace_root,
            .exon_dir = workspace_root / ".exon",
            .build_dir = workspace_build_root(workspace_root, release, target),
            .profile = release ? "release" : "debug",
            .target = std::string{target},
            .is_wasm = !target.empty(),
        };
        auto workspace_name = workspace_build_manifest(workspace_root, selection.members).name;
        if (options.output == reporting::OutputMode::json)
            build::system::detail::emit_json_stage("build", "resolve", "started", project);
        else
            build::system::detail::print_header("build", workspace_name, project);
        if (options.output == reporting::OutputMode::human)
            build::system::detail::print_human_stage("resolve", 1, 5);
        else if (options.output == reporting::OutputMode::wrapped)
            build::system::detail::print_stage("resolve");
        try {
            auto execution = prepare_workspace_build_execution(workspace_root, workspace_manifest,
                                                               selection, release, target);
            if (options.output == reporting::OutputMode::human) {
                return build::system::detail::run_build_human(
                    execution.request, execution.plan, "build", started, execution.success_path,
                    execution.success_label);
            }
            if (options.output == reporting::OutputMode::json) {
                return build::system::detail::run_build_json(
                    execution.request, execution.plan, "build", started, execution.success_path,
                    execution.success_label);
            }
            return build::system::detail::run_build_wrapped(
                execution.request, execution.plan, "build", started, execution.success_path,
                execution.success_label);
        } catch (...) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            if (options.output == reporting::OutputMode::json) {
                build::system::detail::emit_json_stage("build", "resolve", "failed", project);
                build::system::detail::emit_json_summary("build", false, project, started);
            } else {
                build::system::detail::print_failure_summary("build", "resolve", project, elapsed);
            }
            throw;
        }
    }

    auto execution = prepare_workspace_build_execution(workspace_root, workspace_manifest,
                                                       selection, release, target);
    auto changed = build::system::detail::apply_writes(execution.plan.writes);
    if (changed || !execution.plan.configured) {
        auto rc = build::system::detail::run_configure_steps(execution.plan.configure_steps,
                                                             execution.request);
        if (rc != 0)
            return rc;
    }

    auto rc = build::system::detail::run_steps(execution.plan.build_steps);
    if (rc != 0)
        return rc;

    auto rel = std::filesystem::relative(execution.success_path, workspace_root).generic_string();
    std::println("build succeeded: {}", rel);
    return 0;
}

toolchain::Platform effective_platform(std::string_view target) {
    if (!target.empty()) {
        auto p = toolchain::platform_from_target(target);
        if (!p)
            throw std::runtime_error(std::format("unknown target '{}'", target));
        return *p;
    }
    return toolchain::detect_host_platform();
}

manifest::Manifest resolve_manifest(manifest::Manifest m, std::string_view target) {
    return manifest::resolve_for_platform(std::move(m), effective_platform(target));
}

bool check_platform(manifest::Manifest const& m, std::string_view target) {
    auto plat = effective_platform(target);
    if (!manifest::supports_platform(m, plat)) {
        std::println(std::cerr, "error: platform ({}) is not supported by this package",
                     plat.to_string());
        std::println(std::cerr, "  supported platforms:");
        for (auto const& p : m.platforms) {
            if (toolchain::platform_has_os(p) && toolchain::platform_has_arch(p))
                std::println(std::cerr, "    {{ os = \"{}\", arch = \"{}\" }}",
                             toolchain::platform_os_name(p), toolchain::platform_arch_name(p));
            else if (toolchain::platform_has_os(p))
                std::println(std::cerr, "    {{ os = \"{}\" }}", toolchain::platform_os_name(p));
            else
                std::println(std::cerr, "    {{ arch = \"{}\" }}",
                             toolchain::platform_arch_name(p));
        }
        return false;
    }
    return true;
}

std::vector<cli::ArgDef> const build_defs = {
    cli::Flag{"--release"},
    cli::Option{"--target"},
};

std::string dist_version_label(std::string_view requested_version, manifest::Manifest const& m) {
    auto value = std::string{requested_version};
    if (value.empty())
        value = m.version.empty() ? "dev" : m.version;
    if (value.empty())
        value = "dev";
    if (value == "dev" || value.starts_with('v'))
        return value;
    return "v" + value;
}

std::string dist_platform_label(toolchain::Platform const& platform) {
    auto os = toolchain::platform_os_name(platform);
    auto arch = toolchain::platform_arch_name(platform);
    if (os == "macos" && !arch.empty())
        return std::format("{}-apple-darwin", arch);
    if (os == "linux" && !arch.empty())
        return std::format("{}-linux-gnu", arch);
    if (os == "windows" && !arch.empty())
        return std::format("{}-pc-windows-msvc", arch);
    return platform.to_string();
}

std::string dist_platform_label(std::string_view target) {
    if (!target.empty())
        return std::string{target};
    return dist_platform_label(toolchain::detect_host_platform());
}

bool dist_platform_is_windows(std::string_view platform) {
    return platform.find("windows") != std::string_view::npos || platform.ends_with("-msvc");
}

std::string_view dist_archive_extension(std::string_view platform) {
    return dist_platform_is_windows(platform) ? ".zip" : ".tar.gz";
}

std::string dist_executable_filename(std::string_view package_name, std::string_view platform) {
    auto filename = std::string{package_name};
    if (dist_platform_is_windows(platform) && !filename.ends_with(".exe"))
        filename += ".exe";
    return filename;
}

std::string dist_archive_filename(std::string_view package_name, std::string_view version,
                                  std::string_view platform) {
    return std::format("{}-{}-{}{}", package_name, version, platform,
                       dist_archive_extension(platform));
}

std::uint32_t zip_crc32(std::span<char const> bytes) {
    auto crc = std::uint32_t{0xffffffffU};
    for (auto byte : bytes) {
        crc ^= static_cast<unsigned char>(byte);
        for (auto bit = 0; bit != 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return crc ^ 0xffffffffU;
}

void write_le16(std::ostream& out, std::uint16_t value) {
    out.put(static_cast<char>(value & 0xffU));
    out.put(static_cast<char>((value >> 8U) & 0xffU));
}

void write_le32(std::ostream& out, std::uint32_t value) {
    write_le16(out, static_cast<std::uint16_t>(value & 0xffffU));
    write_le16(out, static_cast<std::uint16_t>((value >> 16U) & 0xffffU));
}

std::uint32_t checked_zip32(std::uintmax_t value, std::string_view label) {
    if (value > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error{std::format("{} exceeds ZIP32 limit", label)};
    return static_cast<std::uint32_t>(value);
}

std::uint32_t zip_position(std::ostream& out, std::string_view label) {
    auto position = out.tellp();
    if (position < std::ostream::pos_type{0})
        throw std::runtime_error{std::format("failed to query ZIP {}", label)};
    return checked_zip32(static_cast<std::uintmax_t>(position), label);
}

void write_stored_zip_archive(std::filesystem::path const& source,
                              std::filesystem::path const& archive,
                              std::string_view entry_name) {
    if (entry_name.empty() || entry_name.size() > std::numeric_limits<std::uint16_t>::max())
        throw std::runtime_error{"invalid ZIP entry name"};

    auto size = checked_zip32(std::filesystem::file_size(source), "entry size");
    auto data = std::vector<char>(size);
    auto input = std::ifstream{source, std::ios::binary};
    if (!input)
        throw std::runtime_error{std::format("failed to open {}", source.string())};
    if (!data.empty())
        input.read(data.data(), static_cast<std::streamsize>(data.size()));
    if (input.gcount() != static_cast<std::streamsize>(data.size()))
        throw std::runtime_error{std::format("failed to read {}", source.string())};

    auto output = std::ofstream{archive, std::ios::binary | std::ios::trunc};
    if (!output)
        throw std::runtime_error{std::format("failed to create {}", archive.string())};

    auto const name_length = static_cast<std::uint16_t>(entry_name.size());
    auto const crc = zip_crc32(data);
    auto const dos_time = std::uint16_t{0};
    auto const dos_date = std::uint16_t{0x0021}; // 1980-01-01, the ZIP minimum date.

    auto local_header_offset = zip_position(output, "local header offset");
    write_le32(output, 0x04034b50U);
    write_le16(output, 20);
    write_le16(output, 0);
    write_le16(output, 0);
    write_le16(output, dos_time);
    write_le16(output, dos_date);
    write_le32(output, crc);
    write_le32(output, size);
    write_le32(output, size);
    write_le16(output, name_length);
    write_le16(output, 0);
    output.write(entry_name.data(), static_cast<std::streamsize>(entry_name.size()));
    if (!data.empty())
        output.write(data.data(), static_cast<std::streamsize>(data.size()));

    auto central_directory_offset = zip_position(output, "central directory offset");
    write_le32(output, 0x02014b50U);
    write_le16(output, 20);
    write_le16(output, 20);
    write_le16(output, 0);
    write_le16(output, 0);
    write_le16(output, dos_time);
    write_le16(output, dos_date);
    write_le32(output, crc);
    write_le32(output, size);
    write_le32(output, size);
    write_le16(output, name_length);
    write_le16(output, 0);
    write_le16(output, 0);
    write_le16(output, 0);
    write_le16(output, 0);
    write_le32(output, 0);
    write_le32(output, local_header_offset);
    output.write(entry_name.data(), static_cast<std::streamsize>(entry_name.size()));

    auto central_directory_end = zip_position(output, "central directory end");
    auto central_directory_size =
        checked_zip32(central_directory_end - central_directory_offset, "central directory size");
    write_le32(output, 0x06054b50U);
    write_le16(output, 0);
    write_le16(output, 0);
    write_le16(output, 1);
    write_le16(output, 1);
    write_le32(output, central_directory_size);
    write_le32(output, central_directory_offset);
    write_le16(output, 0);

    if (!output)
        throw std::runtime_error{std::format("failed to write {}", archive.string())};
}

core::ProcessSpec dist_archive_command(std::string cmake, std::filesystem::path const& build_dir,
                                       std::filesystem::path const& archive,
                                       std::string_view executable_filename,
                                       std::string_view platform) {
    if (dist_platform_is_windows(platform))
        throw std::invalid_argument{"Windows dist archives are written as ZIP directly"};
    return core::ProcessSpec{
        .program = std::move(cmake),
        .args =
            {
                "-E",
                "tar",
                "czf",
                archive.string(),
                std::string{executable_filename},
            },
        .cwd = build_dir,
    };
}

struct RunnableCommandTarget {
    std::filesystem::path root;
    manifest::Manifest raw_manifest;
    manifest::Manifest resolved_manifest;
};

std::expected<RunnableCommandTarget, std::string>
resolve_runnable_command_target(std::filesystem::path const& project_root,
                                manifest::Manifest const& raw_m, std::string_view target,
                                std::vector<std::string> const& requested_members,
                                std::vector<std::string> const& excluded_members) {
    auto m = resolve_manifest(raw_m, target);
    auto member_root = project_root;
    auto raw_target_manifest = raw_m;
    auto target_manifest = m;
    if (manifest::is_workspace(raw_m)) {
        auto selection = select_workspace_members(project_root, raw_m, target, requested_members,
                                                  excluded_members, false, false);
        auto runnable = std::vector<WorkspaceMember>{};
        for (auto const& member : selection.members) {
            if (member.runnable)
                runnable.push_back(member);
        }
        if (requested_members.empty()) {
            if (runnable.size() != 1) {
                return std::unexpected{"workspace execution requires exactly one runnable member; "
                                       "pass --member <name>"};
            }
        } else {
            if (selection.members.size() != 1) {
                return std::unexpected{"--member must select exactly one workspace member"};
            }
            if (!selection.members.front().runnable)
                return std::unexpected{"cannot run a library package"};
            runnable = {selection.members.front()};
        }
        auto const& member = runnable.front();
        member_root = member.path;
        raw_target_manifest = member.raw_manifest;
        target_manifest = member.resolved_manifest;
    } else if (!requested_members.empty() || !excluded_members.empty()) {
        return std::unexpected{"--member and --exclude require a workspace root"};
    }

    if (target_manifest.name.empty())
        return std::unexpected{"package name is required in exon.toml"};
    if (target_manifest.type == "lib")
        return std::unexpected{"cannot run a library package"};

    return RunnableCommandTarget{
        .root = member_root,
        .raw_manifest = raw_target_manifest,
        .resolved_manifest = target_manifest,
    };
}

std::string read_text(std::filesystem::path const& path) {
    auto text = cppx::fs::system::read_text(path);
    if (!text)
        throw std::runtime_error(std::format("failed to read {} ({})", path.string(),
                                             cppx::fs::to_string(text.error())));
    return *text;
}

void write_text(std::filesystem::path const& path, std::string const& content) {
    auto result = cppx::fs::system::write_if_changed({
        .path = path,
        .content = content,
        .skip_if_unchanged = false,
    });
    if (!result) {
        throw std::runtime_error(std::format("failed to write {} ({})", path.string(),
                                             cppx::fs::to_string(result.error())));
    }
}

std::optional<std::chrono::milliseconds> parse_timeout(std::string_view value) {
    if (value.empty())
        return std::nullopt;

    long long seconds = 0;
    auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), seconds);
    if (ec != std::errc{} || ptr != value.data() + value.size() || seconds <= 0) {
        throw std::runtime_error(
            std::format("invalid --timeout '{}': expected a positive integer in seconds", value));
    }

    constexpr auto max_ms = std::numeric_limits<std::chrono::milliseconds::rep>::max();
    if (seconds > max_ms / 1000) {
        throw std::runtime_error(std::format("invalid --timeout '{}': value is too large", value));
    }
    return std::chrono::milliseconds{seconds * 1000};
}

int cmd_version() {
    std::println("exon {}", version);
    return 0;
}

int cmd_init(int argc, char* argv[]) {
    auto args = cli::parse(argc, argv, 2,
                           {
                               cli::Flag{"--lib"},
                               cli::Flag{"--workspace"},
                           });
    bool is_lib = args.has("--lib");
    bool is_workspace = args.has("--workspace");
    if (is_lib && is_workspace)
        return command_error("--lib and --workspace are mutually exclusive");
    auto& pos = args.positional();
    if (pos.size() > 1)
        return command_error(std::format("unexpected argument '{}'", pos[1]));
    auto name = pos.empty() ? std::string{} : pos[0];

    auto target_dir = std::filesystem::current_path();
    if (!name.empty()) {
        target_dir /= name;
        std::error_code ec;
        std::filesystem::create_directories(target_dir, ec);
        if (ec) {
            std::println(std::cerr, "error: failed to create directory '{}': {}",
                         target_dir.string(), ec.message());
            return 1;
        }
    } else {
        // Default package name to the current directory name.
        name = target_dir.filename().string();
    }

    auto manifest_path = target_dir / "exon.toml";
    if (std::filesystem::exists(manifest_path)) {
        std::println(std::cerr, "error: {} already exists", manifest_path.string());
        return 1;
    }
    try {
        if (is_workspace) {
            write_text(manifest_path, "[workspace]\nmembers = []\n");
        } else {
            create_package_files(target_dir, name, is_lib);
        }
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
    print_status(terminal::StatusKind::ok,
                 std::format("created {} ({})", manifest_path.string(),
                             is_workspace ? "workspace" : (is_lib ? "lib" : "bin")));
    return 0;
}

int cmd_new(int argc, char* argv[]) {
    try {
        auto args = cli::parse(argc, argv, 2,
                               {
                                   cli::Flag{"--lib"},
                                   cli::Flag{"--bin"},
                               });
        bool is_lib = args.has("--lib");
        bool is_bin = args.has("--bin");
        if (is_lib == is_bin)
            return command_error("choose exactly one of --lib or --bin");

        auto const& positional = args.positional();
        if (positional.size() != 1)
            return command_error("usage: exon new --lib|--bin <name>");

        auto workspace_root = std::filesystem::current_path();
        auto workspace_manifest = load_manifest(workspace_root);
        if (!manifest::is_workspace(workspace_manifest))
            return command_error("exon new must run from a workspace root");

        auto member_name = positional[0];
        auto target_dir = workspace_root / member_name;
        if (std::filesystem::exists(target_dir))
            return command_error(std::format("{} already exists", target_dir.string()));

        create_package_files(target_dir, member_name, is_lib);
        update_workspace_members_list(workspace_root, member_name);
        print_status(terminal::StatusKind::ok,
                     std::format("created {} ({})", (target_dir / "exon.toml").string(),
                                 is_lib ? "lib" : "bin"));
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
    return 0;
}

int cmd_info() {
    try {
        auto project_root = std::filesystem::current_path();
        auto m = load_manifest(project_root);
        if (!manifest::is_workspace(m)) {
            if (auto ws_root = manifest::system::find_workspace_root(project_root);
                ws_root && *ws_root != project_root) {
                auto workspace_manifest = load_manifest(*ws_root);
                m = manifest::apply_workspace_defaults(std::move(m), workspace_manifest);
            }
        }
        std::println("{}", terminal::section("package", reporting::system::stdout_is_tty()));
        std::println("{}", terminal::key_value("name", m.name));
        std::println("{}", terminal::key_value("version", m.version));
        if (!m.description.empty())
            std::println("{}", terminal::key_value("description", m.description));
        if (!m.authors.empty()) {
            std::string authors;
            for (std::size_t i = 0; i < m.authors.size(); ++i) {
                if (i > 0)
                    authors += ", ";
                authors += m.authors[i];
            }
            std::println("{}", terminal::key_value("authors", authors));
        }
        if (!m.license.empty())
            std::println("{}", terminal::key_value("license", m.license));
        std::println("{}", terminal::key_value("type", m.type));
        std::println("{}", terminal::key_value("standard", std::format("C++{}", m.standard)));
        auto host = toolchain::detect_host_platform();
        std::println("{}", terminal::key_value("host", host.to_string()));
        if (!m.platforms.empty()) {
            std::println("platforms:");
            for (auto const& p : m.platforms) {
                if (toolchain::platform_has_os(p) && toolchain::platform_has_arch(p))
                    std::println("  {{ os = \"{}\", arch = \"{}\" }}",
                                 toolchain::platform_os_name(p), toolchain::platform_arch_name(p));
                else if (toolchain::platform_has_os(p))
                    std::println("  {{ os = \"{}\" }}", toolchain::platform_os_name(p));
                else
                    std::println("  {{ arch = \"{}\" }}", toolchain::platform_arch_name(p));
            }
        }
        if (!m.dependencies.empty()) {
            std::println("dependencies:");
            for (auto const& [name, ver] : m.dependencies) {
                std::println("  {} = \"{}\"", name, ver);
            }
        }
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
    return 0;
}

std::string first_line(std::string_view text) {
    while (!text.empty() && (text.front() == '\n' || text.front() == '\r'))
        text.remove_prefix(1);
    auto end = text.find_first_of("\r\n");
    if (end == std::string_view::npos)
        return std::string{text};
    return std::string{text.substr(0, end)};
}

std::string tool_version_line(std::string program, std::vector<std::string> args = {"--version"}) {
    if (program.empty())
        return "unavailable";
    try {
        auto result = reporting::system::run_process(
            core::ProcessSpec{.program = std::move(program), .args = std::move(args)},
            reporting::StreamMode::capture);
        if (result.exit_code != 0)
            return std::format("unavailable (exit {})", result.exit_code);
        auto line =
            first_line(!result.stdout_text.empty() ? result.stdout_text : result.stderr_text);
        return line.empty() ? "available" : line;
    } catch (std::exception const& e) {
        return std::format("unavailable ({})", e.what());
    }
}

std::string mise_tool_version_line(std::string_view tool,
                                   std::vector<std::string> args = {"--version"}) {
    try {
        auto result = reporting::system::run_process(
            core::ProcessSpec{
                .program = "mise",
                .args = {"which", std::string{tool}},
            },
            reporting::StreamMode::capture);
        if (result.exit_code != 0)
            return std::format("unavailable (mise which exit {})", result.exit_code);
        auto path =
            first_line(!result.stdout_text.empty() ? result.stdout_text : result.stderr_text);
        if (path.empty())
            return "unavailable (mise which returned no path)";
        return tool_version_line(std::move(path), std::move(args));
    } catch (std::exception const& e) {
        return std::format("unavailable (mise which failed: {})", e.what());
    }
}

std::size_t manifest_dependency_count(manifest::Manifest const& m) {
    return m.dependencies.size() + m.path_deps.size() + m.workspace_deps.size() +
           m.vcpkg_deps.size() + m.subdir_deps.size() + m.featured_deps.size() +
           m.cmake_deps.size() + m.dev_dependencies.size() + m.dev_path_deps.size() +
           m.dev_workspace_deps.size() + m.dev_vcpkg_deps.size() + m.dev_subdir_deps.size() +
           m.dev_featured_deps.size() + m.dev_cmake_deps.size() + m.find_deps.size() +
           m.dev_find_deps.size();
}

struct JsonValue {
    enum class Kind {
        null,
        boolean,
        number,
        string,
        array,
        object,
    };

    Kind kind = Kind::null;
    bool bool_value = false;
    std::string text;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object;
};

void append_utf8(std::string& out, unsigned int codepoint) {
    if (codepoint <= 0x7f) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ff) {
        out.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0xffff) {
        out.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
}

int hex_value(char ch) {
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

class JsonParser {
  public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    JsonValue parse() {
        skip_ws();
        auto value = parse_value();
        skip_ws();
        if (pos_ != text_.size())
            throw std::runtime_error("unexpected trailing JSON content");
        return value;
    }

  private:
    void skip_ws() {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' ||
                                       text_[pos_] == '\r' || text_[pos_] == '\n')) {
            ++pos_;
        }
    }

    char peek() const {
        if (pos_ >= text_.size())
            throw std::runtime_error("unexpected end of JSON");
        return text_[pos_];
    }

    bool consume(char ch) {
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == ch) {
            ++pos_;
            return true;
        }
        return false;
    }

    void expect(char ch) {
        skip_ws();
        if (pos_ >= text_.size() || text_[pos_] != ch)
            throw std::runtime_error(std::format("expected '{}'", ch));
        ++pos_;
    }

    void expect_literal(std::string_view literal) {
        if (text_.substr(pos_, literal.size()) != literal)
            throw std::runtime_error(std::format("expected '{}'", literal));
        pos_ += literal.size();
    }

    JsonValue parse_value() {
        skip_ws();
        switch (peek()) {
        case 'n':
            expect_literal("null");
            return JsonValue{.kind = JsonValue::Kind::null};
        case 't':
            expect_literal("true");
            return JsonValue{.kind = JsonValue::Kind::boolean, .bool_value = true};
        case 'f':
            expect_literal("false");
            return JsonValue{.kind = JsonValue::Kind::boolean, .bool_value = false};
        case '"':
            return JsonValue{.kind = JsonValue::Kind::string, .text = parse_string()};
        case '[':
            return parse_array();
        case '{':
            return parse_object();
        default:
            return parse_number();
        }
    }

    std::string parse_string() {
        expect('"');
        auto out = std::string{};
        while (pos_ < text_.size()) {
            auto ch = text_[pos_++];
            if (ch == '"')
                return out;
            if (static_cast<unsigned char>(ch) < 0x20)
                throw std::runtime_error("unescaped control character in JSON string");
            if (ch != '\\') {
                out.push_back(ch);
                continue;
            }
            if (pos_ >= text_.size())
                throw std::runtime_error("unterminated JSON escape");
            auto esc = text_[pos_++];
            switch (esc) {
            case '"':
            case '\\':
            case '/':
                out.push_back(esc);
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                if (pos_ + 4 > text_.size())
                    throw std::runtime_error("short JSON unicode escape");
                unsigned int codepoint = 0;
                for (int i = 0; i < 4; ++i) {
                    auto digit = hex_value(text_[pos_++]);
                    if (digit < 0)
                        throw std::runtime_error("invalid JSON unicode escape");
                    codepoint = (codepoint << 4) | static_cast<unsigned int>(digit);
                }
                append_utf8(out, codepoint);
                break;
            }
            default:
                throw std::runtime_error("invalid JSON escape");
            }
        }
        throw std::runtime_error("unterminated JSON string");
    }

    JsonValue parse_number() {
        auto start = pos_;
        if (pos_ < text_.size() && text_[pos_] == '-')
            ++pos_;
        auto digits_start = pos_;
        while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_])))
            ++pos_;
        if (digits_start == pos_)
            throw std::runtime_error("expected JSON value");
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            auto fraction_start = pos_;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_])))
                ++pos_;
            if (fraction_start == pos_)
                throw std::runtime_error("invalid JSON number");
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-'))
                ++pos_;
            auto exponent_start = pos_;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_])))
                ++pos_;
            if (exponent_start == pos_)
                throw std::runtime_error("invalid JSON exponent");
        }
        return JsonValue{.kind = JsonValue::Kind::number,
                         .text = std::string{text_.substr(start, pos_ - start)}};
    }

    JsonValue parse_array() {
        expect('[');
        auto value = JsonValue{.kind = JsonValue::Kind::array};
        if (consume(']'))
            return value;
        while (true) {
            value.array.push_back(parse_value());
            if (consume(']'))
                return value;
            expect(',');
        }
    }

    JsonValue parse_object() {
        expect('{');
        auto value = JsonValue{.kind = JsonValue::Kind::object};
        if (consume('}'))
            return value;
        while (true) {
            skip_ws();
            if (peek() != '"')
                throw std::runtime_error("expected JSON object key");
            auto key = parse_string();
            expect(':');
            value.object.push_back({std::move(key), parse_value()});
            if (consume('}'))
                return value;
            expect(',');
        }
    }

    std::string_view text_;
    std::size_t pos_ = 0;
};

JsonValue parse_json_value(std::string_view text) {
    return JsonParser{text}.parse();
}

std::string serialize_json(JsonValue const& value);

std::string serialize_json_object(std::vector<std::pair<std::string, JsonValue>> const& object) {
    auto out = std::string{"{"};
    bool first = true;
    for (auto const& [key, value] : object) {
        if (!first)
            out.push_back(',');
        first = false;
        out += std::format("\"{}\":{}", reporting::json_escape(key), serialize_json(value));
    }
    out.push_back('}');
    return out;
}

std::string serialize_json(JsonValue const& value) {
    switch (value.kind) {
    case JsonValue::Kind::null:
        return "null";
    case JsonValue::Kind::boolean:
        return value.bool_value ? "true" : "false";
    case JsonValue::Kind::number:
        return value.text;
    case JsonValue::Kind::string:
        return std::format("\"{}\"", reporting::json_escape(value.text));
    case JsonValue::Kind::array: {
        auto out = std::string{"["};
        for (std::size_t i = 0; i < value.array.size(); ++i) {
            if (i > 0)
                out.push_back(',');
            out += serialize_json(value.array[i]);
        }
        out.push_back(']');
        return out;
    }
    case JsonValue::Kind::object:
        return serialize_json_object(value.object);
    }
    return "null";
}

JsonValue const* json_object_get(JsonValue const& object, std::string_view key) {
    if (object.kind != JsonValue::Kind::object)
        return nullptr;
    for (auto const& [member_key, member_value] : object.object) {
        if (member_key == key)
            return &member_value;
    }
    return nullptr;
}

std::string json_string_or(JsonValue const* value, std::string_view fallback = {}) {
    if (value == nullptr || value->kind == JsonValue::Kind::null)
        return std::string{fallback};
    if (value->kind == JsonValue::Kind::string || value->kind == JsonValue::Kind::number)
        return value->text;
    if (value->kind == JsonValue::Kind::boolean)
        return value->bool_value ? "true" : "false";
    return std::string{fallback};
}

bool json_bool_or(JsonValue const* value, bool fallback = false) {
    if (value != nullptr && value->kind == JsonValue::Kind::boolean)
        return value->bool_value;
    return fallback;
}

std::string json_array_of_strings(std::vector<std::string> const& values) {
    auto out = std::string{"["};
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0)
            out.push_back(',');
        out += std::format("\"{}\"", reporting::json_escape(values[i]));
    }
    out.push_back(']');
    return out;
}

struct IntronToolDiagnostic {
    std::string name;
    std::string version;
    bool installed = false;
    bool system = false;
    std::string path;
    std::vector<std::pair<std::string, std::string>> binaries;
};

struct IntronStatusDiagnostic {
    bool command_available = false;
    bool ok = false;
    int exit_code = -1;
    std::string source = "intron";
    std::string error;
    std::string stdout_text;
    std::string stderr_text;
    std::optional<JsonValue> raw_status;
    std::string version;
    std::string platform_name;
    std::string platform_triple;
    bool project_config_found = false;
    std::string project_config_path;
    bool environment_available = false;
    std::string environment_error;
    std::vector<std::string> path_entries;
    std::string network_backend;
    std::string msvc_status;
    std::vector<IntronToolDiagnostic> tools;
    std::vector<std::string> diagnostics;
    std::vector<std::string> hints;
};

struct ToolchainDiagnostic {
    bool available = false;
    std::string cmake;
    std::string ninja;
    std::string compiler;
    std::string compiler_kind;
    std::string error;
};

struct ToolchainMismatch {
    std::string tool;
    std::string expected;
    std::string actual;
};

bool text_contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

std::string first_non_empty_line(std::string_view first, std::string_view second = {}) {
    auto line = first_line(first);
    if (!line.empty())
        return line;
    return first_line(second);
}

void add_intron_failure_hints(IntronStatusDiagnostic& status,
                              std::filesystem::path const& project_root,
                              std::string_view combined_output) {
    if (text_contains(combined_output, "not trusted") ||
        text_contains(combined_output, "mise trust")) {
        status.hints.push_back(std::format("run: mise trust {}", project_root.generic_string()));
    }
    if (text_contains(combined_output, "unknown command") ||
        text_contains(combined_output, "unrecognized") ||
        text_contains(combined_output, "invalid command")) {
        status.hints.push_back("upgrade intron to v0.22.0 or newer");
    }
    if (status.hints.empty()) {
        status.hints.push_back("run: intron doctor --output human");
        status.hints.push_back("rerun exon with --output wrapped if a build command failed");
    }
}

std::vector<std::string> diagnostics_from_json_array(JsonValue const* diagnostics) {
    auto out = std::vector<std::string>{};
    if (diagnostics == nullptr || diagnostics->kind != JsonValue::Kind::array)
        return out;
    for (auto const& item : diagnostics->array) {
        if (item.kind == JsonValue::Kind::string) {
            out.push_back(item.text);
        } else if (item.kind == JsonValue::Kind::object) {
            auto message = json_string_or(json_object_get(item, "message"));
            if (message.empty())
                message = json_string_or(json_object_get(item, "summary"));
            if (message.empty())
                message = serialize_json(item);
            out.push_back(std::move(message));
        } else {
            out.push_back(serialize_json(item));
        }
    }
    return out;
}

void populate_intron_status(IntronStatusDiagnostic& status, JsonValue const& root) {
    status.raw_status = root;
    status.ok = true;
    status.version = json_string_or(json_object_get(root, "version"));

    if (auto platform = json_object_get(root, "platform")) {
        status.platform_name = json_string_or(json_object_get(*platform, "name"));
        status.platform_triple = json_string_or(json_object_get(*platform, "triple"));
    }
    if (auto project_config = json_object_get(root, "project_config")) {
        status.project_config_found = json_bool_or(json_object_get(*project_config, "found"));
        status.project_config_path = json_string_or(json_object_get(*project_config, "path"));
    }
    if (auto environment = json_object_get(root, "environment")) {
        status.environment_available = json_bool_or(json_object_get(*environment, "available"));
        status.environment_error = json_string_or(json_object_get(*environment, "error"));
        if (auto path_entries = json_object_get(*environment, "path_entries");
            path_entries && path_entries->kind == JsonValue::Kind::array) {
            for (auto const& entry : path_entries->array) {
                auto text = json_string_or(&entry);
                if (!text.empty())
                    status.path_entries.push_back(std::move(text));
            }
        }
    }
    if (auto network = json_object_get(root, "network"))
        status.network_backend = json_string_or(json_object_get(*network, "selected_backend"));
    if (auto msvc = json_object_get(root, "msvc"))
        status.msvc_status = json_string_or(json_object_get(*msvc, "status"));
    status.diagnostics = diagnostics_from_json_array(json_object_get(root, "diagnostics"));

    if (auto tools = json_object_get(root, "tools");
        tools && tools->kind == JsonValue::Kind::object) {
        for (auto const& [name, value] : tools->object) {
            auto tool = IntronToolDiagnostic{
                .name = name,
                .version = json_string_or(json_object_get(value, "version")),
                .installed = json_bool_or(json_object_get(value, "installed")),
                .system = json_bool_or(json_object_get(value, "system")),
                .path = json_string_or(json_object_get(value, "path")),
            };
            if (auto binaries = json_object_get(value, "binaries");
                binaries && binaries->kind == JsonValue::Kind::object) {
                for (auto const& [binary_name, binary_value] : binaries->object) {
                    auto binary_path = json_string_or(&binary_value);
                    if (!binary_path.empty())
                        tool.binaries.push_back({binary_name, std::move(binary_path)});
                }
            }
            status.tools.push_back(std::move(tool));
        }
    }

    if (!status.environment_available)
        status.hints.push_back("run: intron doctor --output human");
    if (std::ranges::any_of(status.tools,
                            [](IntronToolDiagnostic const& tool) { return !tool.installed; })) {
        status.hints.push_back("run: intron install");
    }
}

IntronStatusDiagnostic intron_status_from_process(std::string source,
                                                  reporting::ProcessResult result,
                                                  std::filesystem::path const& project_root) {
    auto status = IntronStatusDiagnostic{
        .command_available = true,
        .exit_code = result.exit_code,
        .source = std::move(source),
        .stdout_text = std::move(result.stdout_text),
        .stderr_text = std::move(result.stderr_text),
    };
    if (result.exit_code != 0) {
        status.error = first_non_empty_line(status.stderr_text, status.stdout_text);
        if (status.error.empty())
            status.error = std::format("intron status exited with code {}", result.exit_code);
        add_intron_failure_hints(status, project_root,
                                 status.stdout_text + "\n" + status.stderr_text);
        return status;
    }

    try {
        auto parsed = parse_json_value(status.stdout_text);
        if (parsed.kind != JsonValue::Kind::object)
            throw std::runtime_error("expected top-level JSON object");
        populate_intron_status(status, parsed);
    } catch (std::exception const& e) {
        status.ok = false;
        status.error = std::format("failed to parse intron status JSON: {}", e.what());
        status.hints.push_back("run: intron status --output json");
    }
    return status;
}

std::optional<std::string> resolve_intron_from_mise(std::filesystem::path const& project_root,
                                                    IntronStatusDiagnostic& status) {
    try {
        auto result = reporting::system::run_process(
            core::ProcessSpec{
                .program = "mise",
                .args = {"which", "intron"},
                .cwd = project_root,
            },
            reporting::StreamMode::capture);
        if (result.exit_code == 0) {
            auto path = first_non_empty_line(result.stdout_text, result.stderr_text);
            if (!path.empty())
                return path;
        }

        status.error = first_non_empty_line(result.stderr_text, result.stdout_text);
        if (status.error.empty())
            status.error = std::format("mise which intron exited with code {}", result.exit_code);
        add_intron_failure_hints(status, project_root,
                                 result.stdout_text + "\n" + result.stderr_text);
    } catch (std::exception const& e) {
        status.error = std::format("failed to resolve intron with mise: {}", e.what());
        status.hints.push_back("install intron or run from a trusted mise project");
    }
    return std::nullopt;
}

std::optional<std::string>
intron_version_from_mise_toml(std::filesystem::path const& project_root) {
    auto path = project_root / "mise.toml";
    if (!std::filesystem::exists(path))
        return std::nullopt;
    auto text = read_text(path);
    auto key = std::string_view{"\"vfox:misut/mise-intron\""};
    auto pos = text.find(key);
    if (pos == std::string::npos)
        return std::nullopt;
    auto eq = text.find('=', pos + key.size());
    if (eq == std::string::npos)
        return std::nullopt;
    auto first_quote = text.find('"', eq);
    if (first_quote == std::string::npos)
        return std::nullopt;
    auto second_quote = text.find('"', first_quote + 1);
    if (second_quote == std::string::npos || second_quote == first_quote + 1)
        return std::nullopt;
    return text.substr(first_quote + 1, second_quote - first_quote - 1);
}

std::optional<std::string>
resolve_intron_from_known_paths(std::filesystem::path const& project_root,
                                IntronStatusDiagnostic& status) {
    auto home = toolchain::system::home_dir();
    if (home.empty())
        return std::nullopt;

    auto candidates = std::vector<std::filesystem::path>{
        home / ".intron" / "bin" / "intron",
    };
    if (auto version = intron_version_from_mise_toml(project_root)) {
        candidates.push_back(home / ".local" / "share" / "mise" / "installs" /
                             "vfox-misut-mise-intron" / *version / "intron");
    }

    for (auto const& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec) && !ec)
            return candidate.string();
    }

    if (status.hints.empty()) {
        status.hints.push_back("run: mise exec -- intron status --output json");
        status.hints.push_back("ensure intron is on PATH before running exon doctor");
    }
    return std::nullopt;
}

IntronStatusDiagnostic collect_intron_status(std::filesystem::path const& project_root) {
    try {
        auto result = reporting::system::run_process(
            core::ProcessSpec{
                .program = "intron",
                .args = {"status", "--output", "json"},
                .cwd = project_root,
            },
            reporting::StreamMode::capture);
        if (result.exit_code == 127) {
            auto direct_status = intron_status_from_process("intron", result, project_root);
            if (auto intron = resolve_intron_from_known_paths(project_root, direct_status)) {
                auto fallback_result = reporting::system::run_process(
                    core::ProcessSpec{
                        .program = *intron,
                        .args = {"status", "--output", "json"},
                        .cwd = project_root,
                    },
                    reporting::StreamMode::capture);
                return intron_status_from_process(*intron, std::move(fallback_result),
                                                  project_root);
            }
            return direct_status;
        }
        return intron_status_from_process("intron", std::move(result), project_root);
    } catch (std::exception const& direct_error) {
        auto status = IntronStatusDiagnostic{
            .source = "intron",
            .error = std::format("failed to run intron: {}", direct_error.what()),
        };
        auto intron = resolve_intron_from_mise(project_root, status);
        if (!intron)
            intron = resolve_intron_from_known_paths(project_root, status);
        if (intron) {
            try {
                auto result = reporting::system::run_process(
                    core::ProcessSpec{
                        .program = *intron,
                        .args = {"status", "--output", "json"},
                        .cwd = project_root,
                    },
                    reporting::StreamMode::capture);
                return intron_status_from_process(*intron, std::move(result), project_root);
            } catch (std::exception const& e) {
                status.error = std::format("failed to run '{}': {}", *intron, e.what());
                status.hints.push_back("run: mise exec -- intron status --output json");
            }
        }
        if (status.hints.empty())
            status.hints.push_back("run: mise exec -- intron status --output json");
        return status;
    }
}

ToolchainDiagnostic detect_toolchain_status() {
    try {
        auto tc = toolchain::system::detect();
        return ToolchainDiagnostic{
            .available = true,
            .cmake = tc.cmake,
            .ninja = tc.ninja,
            .compiler = tc.cxx_compiler,
            .compiler_kind = std::string{toolchain::compiler_kind_name(tc.compiler_kind)},
        };
    } catch (std::exception const& e) {
        return ToolchainDiagnostic{.error = e.what()};
    }
}

std::string intron_tool_binary(IntronStatusDiagnostic const& intron, std::string_view tool_name,
                               std::string_view binary_name) {
    for (auto const& tool : intron.tools) {
        if (tool.name != tool_name)
            continue;
        for (auto const& [name, path] : tool.binaries) {
            if (name == binary_name)
                return path;
        }
    }
    return {};
}

std::vector<ToolchainMismatch> toolchain_mismatches(ToolchainDiagnostic const& toolchain,
                                                    IntronStatusDiagnostic const& intron) {
    auto mismatches = std::vector<ToolchainMismatch>{};
    if (!toolchain.available || !intron.ok)
        return mismatches;

    auto compare = [&](std::string_view tool, std::string expected, std::string actual) {
        if (!expected.empty() && !actual.empty() && expected != actual) {
            mismatches.push_back(ToolchainMismatch{
                .tool = std::string{tool},
                .expected = std::move(expected),
                .actual = std::move(actual),
            });
        }
    };

    compare("cmake", intron_tool_binary(intron, "cmake", "cmake"), toolchain.cmake);
    compare("ninja", intron_tool_binary(intron, "ninja", "ninja"), toolchain.ninja);
    compare("compiler", intron_tool_binary(intron, "llvm", "cxx"), toolchain.compiler);
    return mismatches;
}

std::string mismatches_json(std::vector<ToolchainMismatch> const& mismatches) {
    auto out = std::string{"["};
    for (std::size_t i = 0; i < mismatches.size(); ++i) {
        if (i > 0)
            out.push_back(',');
        auto const& mismatch = mismatches[i];
        out += std::format("{{\"tool\":\"{}\",\"expected\":\"{}\",\"actual\":\"{}\"}}",
                           reporting::json_escape(mismatch.tool),
                           reporting::json_escape(mismatch.expected),
                           reporting::json_escape(mismatch.actual));
    }
    out.push_back(']');
    return out;
}

std::string toolchain_json(ToolchainDiagnostic const& toolchain,
                           IntronStatusDiagnostic const& intron) {
    if (!toolchain.available) {
        return std::format("{{\"available\":false,\"error\":\"{}\",\"hints\":[\"run: exon "
                           "doctor\",\"run: intron status --output human\"]}}",
                           reporting::json_escape(toolchain.error));
    }
    auto mismatches = toolchain_mismatches(toolchain, intron);
    return std::format("{{\"available\":true,\"matches_intron\":{},\"mismatches\":{},"
                       "\"cmake\":\"{}\",\"ninja\":\"{}\",\"compiler\":\"{}\","
                       "\"compiler_kind\":\"{}\"}}",
                       mismatches.empty() ? "true" : "false", mismatches_json(mismatches),
                       reporting::json_escape(toolchain.cmake),
                       reporting::json_escape(toolchain.ninja),
                       reporting::json_escape(toolchain.compiler),
                       reporting::json_escape(toolchain.compiler_kind));
}

std::string intron_json(IntronStatusDiagnostic const& status) {
    auto out = std::format("{{\"available\":{},\"ok\":{},\"exit_code\":{},\"source\":\"{}\"",
                           status.command_available ? "true" : "false",
                           status.ok ? "true" : "false", static_cast<long long>(status.exit_code),
                           reporting::json_escape(status.source));
    if (!status.error.empty())
        out += std::format(",\"error\":\"{}\"", reporting::json_escape(status.error));
    if (!status.stdout_text.empty() && !status.ok)
        out += std::format(",\"stdout\":\"{}\"", reporting::json_escape(status.stdout_text));
    if (!status.stderr_text.empty() && !status.ok)
        out += std::format(",\"stderr\":\"{}\"", reporting::json_escape(status.stderr_text));
    if (!status.hints.empty())
        out += std::format(",\"hints\":{}", json_array_of_strings(status.hints));

    if (status.raw_status && status.raw_status->kind == JsonValue::Kind::object) {
        for (auto const& [key, value] : status.raw_status->object) {
            out += std::format(",\"{}\":{}", reporting::json_escape(key), serialize_json(value));
        }
    }
    out.push_back('}');
    return out;
}

void print_intron_diagnostics(IntronStatusDiagnostic const& status) {
    auto color = reporting::system::stdout_is_tty();
    std::println("{}", terminal::section("intron", color));
    if (!status.ok) {
        std::println("{} {}", terminal::status_cell(terminal::StatusKind::fail, color),
                     "intron status unavailable");
        if (!status.error.empty())
            std::println("{}", terminal::key_value("error", status.error));
        for (auto const& hint : status.hints)
            std::println("  {}", terminal::hint_line(hint, color));
        return;
    }

    std::println("{} {}", terminal::status_cell(terminal::StatusKind::ok, color),
                 "intron status resolved");
    std::println("{}", terminal::key_value("version", status.version));
    auto platform = status.platform_name;
    if (!status.platform_triple.empty())
        platform += std::format(" ({})", status.platform_triple);
    if (!platform.empty())
        std::println("{}", terminal::key_value("platform", platform));
    std::println("{}", terminal::key_value("config", status.project_config_found
                                                         ? status.project_config_path
                                                         : "missing"));
    std::println("{}",
                 terminal::key_value("environment",
                                     status.environment_available ? "available" : "unavailable"));
    if (!status.environment_error.empty())
        std::println("{}", terminal::key_value("env error", status.environment_error));
    if (!status.network_backend.empty())
        std::println("{}", terminal::key_value("network", status.network_backend));
    if (!status.msvc_status.empty())
        std::println("{}", terminal::key_value("msvc", status.msvc_status));

    std::println("");
    std::println("{}", terminal::section("toolchain", color));
    for (auto const& tool : status.tools) {
        auto kind = tool.installed ? terminal::StatusKind::ok : terminal::StatusKind::fail;
        auto summary = std::format("{} {} {}", tool.name, tool.version,
                                   tool.installed ? "installed" : "missing");
        if (tool.system)
            summary += " (system)";
        std::println("{} {}", terminal::status_cell(kind, color), summary);
        for (auto const& [binary, path] : tool.binaries)
            std::println("{}", terminal::key_value(binary, path));
        if (!tool.installed)
            std::println("  {}", terminal::hint_line("run: intron install", color));
    }

    if (!status.diagnostics.empty()) {
        std::println("");
        std::println("{}", terminal::section("diagnostics", color));
        for (auto const& diagnostic : status.diagnostics)
            std::println("{} {}", terminal::status_cell(terminal::StatusKind::fail, color),
                         diagnostic);
    }
    for (auto const& hint : status.hints)
        std::println("  {}", terminal::hint_line(hint, color));
}

void print_exon_toolchain_status(ToolchainDiagnostic const& toolchain,
                                 IntronStatusDiagnostic const& intron) {
    auto color = reporting::system::stdout_is_tty();
    std::println("{}", terminal::section("exon detected tools", color));
    if (!toolchain.available) {
        std::println("{} {}", terminal::status_cell(terminal::StatusKind::fail, color),
                     "toolchain detection failed");
        std::println("{}", terminal::key_value("error", toolchain.error));
        std::println("  {}", terminal::hint_line("run: intron status --output human", color));
        return;
    }
    std::println("{}", terminal::key_value("cmake", tool_version_line(toolchain.cmake)));
    std::println("{}", terminal::key_value("ninja", tool_version_line(toolchain.ninja)));
    std::println("{}", terminal::key_value("compiler", tool_version_line(toolchain.compiler)));
    std::println("{}", terminal::key_value("compiler kind", toolchain.compiler_kind));

    auto mismatches = toolchain_mismatches(toolchain, intron);
    for (auto const& mismatch : mismatches) {
        std::println("{} {}", terminal::status_cell(terminal::StatusKind::fail, color),
                     std::format("{} differs from intron status", mismatch.tool));
        std::println("{}", terminal::key_value("expected", mismatch.expected));
        std::println("{}", terminal::key_value("actual", mismatch.actual));
    }
    if (!mismatches.empty()) {
        std::println("  {}", terminal::hint_line(
                                 "run intron status --output human, then ensure exon uses the same "
                                 "toolchain",
                                 color));
    }
}

void print_terminal_status(reporting::Options const& options) {
    auto color = reporting::resolve_capability(options.color, "EXON_COLOR");
    auto progress = reporting::resolve_capability(options.progress, "EXON_PROGRESS");
    auto unicode = reporting::resolve_capability(options.unicode, "EXON_UNICODE");
    auto hyperlinks = reporting::resolve_capability(options.hyperlinks, "EXON_HYPERLINKS");
    std::println("{}", terminal::section("terminal", reporting::system::stdout_is_tty()));
    std::println("{}", terminal::key_value("stdout tty",
                                           reporting::system::stdout_is_terminal() ? "yes" : "no"));
    std::println("{}", terminal::key_value(
                           "width", reporting::system::terminal_width() == 0
                                        ? std::string{"unknown"}
                                        : std::format("{}", reporting::system::terminal_width())));
    std::println("{}", terminal::key_value("color", reporting::to_string(color)));
    std::println("{}", terminal::key_value("progress", reporting::to_string(progress)));
    std::println("{}", terminal::key_value("unicode", reporting::to_string(unicode)));
    std::println("{}", terminal::key_value("hyperlinks", reporting::to_string(hyperlinks)));
}

int cmd_status(int argc, char* argv[]) {
    try {
        auto args = cli::parse(argc, argv, 2, reporting_defs());
        auto options = parse_reporting_options(args);
        auto scoped_options = reporting::ScopedOptionsContext{options};
        auto project_root = std::filesystem::current_path();
        auto manifest_path = project_root / "exon.toml";
        auto manifest_exists = std::filesystem::exists(manifest_path);
        auto intron_status = collect_intron_status(project_root);
        auto toolchain_status = detect_toolchain_status();
        if (options.output == reporting::OutputMode::json) {
            auto fields = std::vector<reporting::JsonField>{
                reporting::json_string("root", project_root.generic_string()),
                reporting::json_bool("manifest", manifest_exists),
                reporting::json_string("exon_version", version),
                reporting::json_raw("intron", intron_json(intron_status)),
                reporting::json_raw("toolchain", toolchain_json(toolchain_status, intron_status)),
            };
            if (!manifest_exists) {
                fields.push_back(reporting::json_string("next", "exon init"));
                reporting::emit_json_event("status", fields);
                return 0;
            }

            auto m = load_manifest(project_root);
            auto kind = manifest::is_workspace(m) ? std::string{"workspace"} : m.type;
            fields.push_back(reporting::json_string("package", m.name));
            fields.push_back(reporting::json_string("kind", kind));
            fields.push_back(reporting::json_number(
                "workspace_members", static_cast<long long>(m.workspace_members.size())));
            fields.push_back(reporting::json_number(
                "dependencies", static_cast<long long>(manifest_dependency_count(m))));
            fields.push_back(
                reporting::json_bool("lock", std::filesystem::exists(project_root / "exon.lock")));
            auto project = build::project_context(project_root, false, {});
            fields.push_back(reporting::json_bool(
                "configured", std::filesystem::exists(project.build_dir / "build.ninja")));
            if (toolchain_status.available) {
                fields.push_back(reporting::json_string("cmake", toolchain_status.cmake));
                fields.push_back(reporting::json_string("ninja", toolchain_status.ninja));
                fields.push_back(reporting::json_string("compiler", toolchain_status.compiler));
                fields.push_back(
                    reporting::json_string("compiler_kind", toolchain_status.compiler_kind));
            } else {
                fields.push_back(reporting::json_string("toolchain_error", toolchain_status.error));
            }
            reporting::emit_json_event("status", fields);
            return 0;
        }

        std::println("{}", terminal::section("exon status", reporting::system::stdout_is_tty()));
        std::println("{}", terminal::key_value("root", project_root.generic_string()));
        std::println("{}", terminal::key_value("version", version));
        if (!manifest_exists) {
            std::println("{}", terminal::key_value("manifest", "missing"));
            std::println("{}", terminal::key_value("next", "exon init"));
            std::println("");
            print_intron_diagnostics(intron_status);
            std::println("");
            print_exon_toolchain_status(toolchain_status, intron_status);
            std::println("");
            print_terminal_status(options);
            return 0;
        }

        auto m = load_manifest(project_root);
        auto kind = manifest::is_workspace(m) ? std::string{"workspace"} : m.type;
        std::println("{}", terminal::key_value("manifest", "found"));
        std::println("{}", terminal::key_value("package", m.name));
        std::println("{}", terminal::key_value("kind", kind));
        if (manifest::is_workspace(m))
            std::println("{}", terminal::key_value("members",
                                                   std::format("{}", m.workspace_members.size())));
        std::println("{}",
                     terminal::key_value("deps", std::format("{}", manifest_dependency_count(m))));
        std::println("{}",
                     terminal::key_value("lock", std::filesystem::exists(project_root / "exon.lock")
                                                     ? "present"
                                                     : "missing"));
        auto project = build::project_context(project_root, false, {});
        std::println("{}", terminal::key_value("build dir", build::system::detail::display_path(
                                                                project.build_dir, project.root)));
        std::println(
            "{}", terminal::key_value(
                      "configured",
                      std::filesystem::exists(project.build_dir / "build.ninja") ? "yes" : "no"));

        std::println("");
        std::println("{}", terminal::key_value("mise", tool_version_line("mise")));
        std::println("");
        print_intron_diagnostics(intron_status);
        std::println("");
        print_exon_toolchain_status(toolchain_status, intron_status);

        std::println("");
        print_terminal_status(options);
        std::println("");
        std::println("{}",
                     terminal::key_value("next", intron_status.ok ? "exon build" : "exon doctor"));
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
    return 0;
}

using BuildFn = std::function<int(std::filesystem::path const&, manifest::Manifest const&,
                                  manifest::Manifest const&, bool, std::string_view)>;

int run_build_command(int argc, char* argv[], BuildFn fn) {
    try {
        auto args = cli::parse(argc, argv, 2, workspace_select_defs(build_defs));
        auto release = args.has("--release");
        auto target = std::string{args.get("--target")};
        auto requested_members = args.get_list("--member");
        auto excluded_members = args.get_list("--exclude");
        auto project_root = std::filesystem::current_path();
        auto raw_m = load_manifest(project_root);
        if (!check_platform(raw_m, target))
            return 1;
        auto m = resolve_manifest(raw_m, target);
        if (manifest::is_workspace(raw_m)) {
            auto selection = select_workspace_members(
                project_root, raw_m, target, requested_members, excluded_members, false, false);
            return run_selected_workspace_members(project_root, selection, [&](auto const& member) {
                return fn(member.path, member.raw_manifest, member.resolved_manifest, release,
                          target);
            });
        }
        if (!requested_members.empty() || !excluded_members.empty())
            return command_error("--member and --exclude require a workspace root");
        if (m.name.empty())
            return command_error("package name is required in exon.toml");
        return fn(project_root, raw_m, m, release, target);
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
}

void print_workspace_human_member_header(std::filesystem::path const& workspace_root,
                                         WorkspaceMember const& member) {
    print_status(terminal::StatusKind::run,
                 std::format("member {}", workspace_display_name(member, workspace_root)));
}

struct WorkspaceTestAggregate {
    int members_run = 0;
    int members_passed = 0;
    int members_failed = 0;
    int binaries_run = 0;
};

void print_workspace_test_summary(WorkspaceTestAggregate const& aggregate,
                                  std::chrono::milliseconds elapsed) {
    std::println("");
    std::println("exon: workspace test {}", aggregate.members_failed == 0 ? "succeeded" : "failed");
    std::println("  members   {} run, {} passed, {} failed", aggregate.members_run,
                 aggregate.members_passed, aggregate.members_failed);
    std::println("  binaries  {} run", aggregate.binaries_run);
    std::println("  elapsed   {}", reporting::format_duration(elapsed));
}

int run_workspace_test(std::filesystem::path const& workspace_root,
                       WorkspaceSelection const& selection, bool release, std::string_view target,
                       std::string_view filter, std::optional<std::chrono::milliseconds> timeout,
                       reporting::Options const& options) {
    auto scoped_options = reporting::ScopedOptionsContext{options};
    auto started = std::chrono::steady_clock::now();
    WorkspaceTestAggregate aggregate;
    int last_rc = 0;

    for (auto const& member : selection.members) {
        if (options.output == reporting::OutputMode::human) {
            if (aggregate.members_run > 0)
                std::println("");
            print_workspace_human_member_header(workspace_root, member);
        } else if (options.output == reporting::OutputMode::json) {
            reporting::emit_json_event(
                "workspace-member",
                {
                    reporting::json_string("name", member.name),
                    reporting::json_string(
                        "path",
                        std::filesystem::relative(member.path, workspace_root).generic_string()),
                    reporting::json_string("state", "started"),
                });
        } else {
            std::println("--- {} ---", workspace_display_name(member, workspace_root));
        }

        ++aggregate.members_run;
        build::system::TestRunSummary member_summary;
        auto rc =
            build::system::run_test(member.path, member.resolved_manifest, member.raw_manifest,
                                    release, target, filter, timeout, options, member_summary);
        aggregate.binaries_run += member_summary.collected;
        if (rc == 0) {
            ++aggregate.members_passed;
            if (options.output == reporting::OutputMode::json) {
                reporting::emit_json_event("workspace-member",
                                           {
                                               reporting::json_string("name", member.name),
                                               reporting::json_string("state", "passed"),
                                           });
            }
            continue;
        }

        ++aggregate.members_failed;
        if (options.output == reporting::OutputMode::json) {
            reporting::emit_json_event("workspace-member",
                                       {
                                           reporting::json_string("name", member.name),
                                           reporting::json_string("state", "failed"),
                                       });
        }
        last_rc = rc;
        break;
    }

    if (options.output == reporting::OutputMode::human) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        print_workspace_test_summary(aggregate, elapsed);
    } else if (options.output == reporting::OutputMode::json) {
        reporting::emit_json_event(
            "summary", {
                           reporting::json_string("verb", "workspace-test"),
                           reporting::json_bool("success", aggregate.members_failed == 0),
                           reporting::json_number("members_run", aggregate.members_run),
                           reporting::json_number("members_passed", aggregate.members_passed),
                           reporting::json_number("members_failed", aggregate.members_failed),
                           reporting::json_number("binaries_run", aggregate.binaries_run),
                           reporting::json_number(
                               "elapsed_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 std::chrono::steady_clock::now() - started)
                                                 .count()),
                       });
    }
    return last_rc;
}

int cmd_build(int argc, char* argv[]) {
    try {
        auto args = cli::parse(argc, argv, 2,
                               workspace_select_defs(reporting_defs({
                                   cli::Flag{"--release"},
                                   cli::Option{"--target"},
                               })));
        auto release = args.has("--release");
        auto target = std::string{args.get("--target")};
        auto options = parse_reporting_options(args);
        auto requested_members = args.get_list("--member");
        auto excluded_members = args.get_list("--exclude");
        auto project_root = std::filesystem::current_path();
        auto raw_m = load_manifest(project_root);
        if (!check_platform(raw_m, target))
            return 1;
        auto m = resolve_manifest(raw_m, target);
        if (manifest::is_workspace(raw_m)) {
            auto selection = select_workspace_members(
                project_root, raw_m, target, requested_members, excluded_members, true, false);
            return run_workspace_build(project_root, raw_m, selection, release, target, options);
        }
        if (!requested_members.empty() || !excluded_members.empty())
            return command_error("--member and --exclude require a workspace root");
        if (m.name.empty())
            return command_error("package name is required in exon.toml");
        return build::system::run(project_root, m, raw_m, release, target, options);
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
}

int cmd_dist(int argc, char* argv[]) {
    try {
        auto args = cli::parse(argc, argv, 2,
                               reporting_defs({
                                   cli::Flag{"--release"},
                                   cli::Option{"--target"},
                                   cli::Option{"--output-dir"},
                                   cli::Option{"--version"},
                               }));
        auto release = args.has("--release");
        auto target = std::string{args.get("--target")};
        auto options = parse_reporting_options(args);
        auto scoped_options = reporting::ScopedOptionsContext{options};
        auto project_root = std::filesystem::current_path();
        auto raw_m = load_manifest(project_root);
        if (manifest::is_workspace(raw_m)) {
            return command_error(
                "exon dist packages a single binary package",
                "run from a package root or package a workspace member from its own directory");
        }
        if (!check_platform(raw_m, target))
            return 1;
        auto m = resolve_manifest(raw_m, target);
        if (m.name.empty())
            return command_error("package name is required in exon.toml");
        if (m.type == "lib")
            return command_error("exon dist packages executable packages only");

        auto build_rc = build::system::run(project_root, m, raw_m, release, target, options);
        if (build_rc != 0)
            return build_rc;

        auto platform = dist_platform_label(target);
        auto executable_filename = dist_executable_filename(m.name, platform);
        auto project = build::project_context(project_root, release, target);
        auto executable = project.build_dir / executable_filename;
        if (!std::filesystem::exists(executable)) {
            return command_error(
                std::format("build artifact not found: {}", executable.string()),
                "rerun exon dist with the same --release and --target options used by the build");
        }

        auto output_dir_text = std::string{args.get("--output-dir")};
        auto output_dir =
            output_dir_text.empty() ? project_root : std::filesystem::path{output_dir_text};
        std::filesystem::create_directories(output_dir);

        auto archive_filename =
            dist_archive_filename(m.name, dist_version_label(args.get("--version"), m), platform);
        auto archive = std::filesystem::absolute(output_dir / archive_filename);
        std::error_code ec;
        std::filesystem::remove(archive, ec);

        auto started = std::chrono::steady_clock::now();
        auto result = reporting::ProcessResult{};
        auto archive_error = std::string{};
        if (dist_platform_is_windows(platform)) {
            try {
                write_stored_zip_archive(executable, archive, executable_filename);
            } catch (std::exception const& e) {
                result.exit_code = 1;
                archive_error = e.what();
            }
        } else {
            auto tc = toolchain::system::detect();
            auto spec = dist_archive_command(tc.cmake, project.build_dir, archive,
                                             executable_filename, platform);
            result =
                reporting::system::run_process(spec, reporting::stream_mode_for(options.output));
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        if (result.exit_code != 0) {
            if (options.output == reporting::OutputMode::json) {
                reporting::emit_json_event(
                    "diagnostic",
                    {
                        reporting::json_string("severity", "error"),
                        reporting::json_string("message", "archive command failed"),
                        reporting::json_number("exit_code", result.exit_code),
                        reporting::json_string(
                            "stderr_excerpt",
                            archive_error.empty() ? first_line(result.stderr_text)
                                                  : archive_error),
                    });
                reporting::emit_json_event(
                    "summary", {
                                   reporting::json_string("verb", "dist"),
                                   reporting::json_bool("success", false),
                                   reporting::json_number("elapsed_ms", elapsed.count()),
                               });
            } else {
                auto hint = archive_error.empty()
                                ? first_non_empty_line(result.stderr_text, result.stdout_text)
                                : archive_error;
                if (hint.empty())
                    hint = std::format("archive command exited with code {}", result.exit_code);
                command_error("failed to package archive", hint);
            }
            return result.exit_code == 0 ? 1 : result.exit_code;
        }

        if (options.output == reporting::OutputMode::json) {
            reporting::emit_json_event(
                "artifact",
                {
                    reporting::json_string("label", "archive"),
                    reporting::json_string(
                        "path", build::system::detail::display_path(archive, project_root)),
                    reporting::json_string("absolute_path", archive.generic_string()),
                    reporting::json_string("platform", platform),
                    reporting::json_string("format",
                                           dist_platform_is_windows(platform) ? "zip" : "tar.gz"),
                });
            reporting::emit_json_event("summary",
                                       {
                                           reporting::json_string("verb", "dist"),
                                           reporting::json_bool("success", true),
                                           reporting::json_string("profile", project.profile),
                                           reporting::json_string("target", platform),
                                           reporting::json_number("elapsed_ms", elapsed.count()),
                                       });
        } else {
            auto rel = build::system::detail::display_path(archive, project_root);
            if (options.output == reporting::OutputMode::raw)
                std::println("packaged: {}", rel);
            else
                print_status(terminal::StatusKind::ok, std::format("packaged {}", rel));
        }
        return 0;
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
}

int cmd_check(int argc, char* argv[]) {
    return run_build_command(
        argc, argv,
        [](std::filesystem::path const& project_root, manifest::Manifest const& raw_m,
           manifest::Manifest const& m, bool release, std::string_view target) {
            return build::system::run_check(project_root, m, raw_m, release, target);
        });
}

int cmd_run(int argc, char* argv[]) {
    try {
        auto args = cli::parse(argc, argv, 2, workspace_select_defs(build_defs));
        auto release = args.has("--release");
        auto target = std::string{args.get("--target")};
        auto requested_members = args.get_list("--member");
        auto excluded_members = args.get_list("--exclude");
        auto project_root = std::filesystem::current_path();
        auto raw_m = load_manifest(project_root);
        if (!check_platform(raw_m, target))
            return 1;
        auto runnable_target = resolve_runnable_command_target(project_root, raw_m, target,
                                                               requested_members, excluded_members);
        if (!runnable_target)
            return command_error(runnable_target.error());
        bool is_wasm = !target.empty() && target.starts_with("wasm32");
        bool is_android = !target.empty() && target.ends_with("-linux-android");
        if (is_android)
            return command_error(
                "cannot `exon run` an Android target on the host",
                "use `exon build --target aarch64-linux-android`, then deploy the artifact "
                "to a device or emulator");

        int rc = build::system::run(runnable_target->root, runnable_target->resolved_manifest,
                                    runnable_target->raw_manifest, release, target);
        if (rc != 0)
            return rc;

        auto project = build::project_context(runnable_target->root, release, target);
        core::ProcessSpec spec{
            .cwd = runnable_target->root,
        };
        if (is_wasm) {
            auto wasm_runtime = toolchain::system::detect_wasm_runtime();
            if (wasm_runtime.empty())
                throw std::runtime_error(
                    "wasmtime not found on PATH (install: https://wasmtime.dev)");
            auto wasm_file = project.build_dir / runnable_target->resolved_manifest.name;
            spec.program = wasm_runtime;
            spec.args.push_back(wasm_file.string());
        } else {
            auto exe = project.build_dir / (runnable_target->resolved_manifest.name +
                                            std::string{toolchain::exe_suffix});
            spec.program = exe.string();
        }
        for (auto const& a : args.positional())
            spec.args.push_back(a);
        std::println("running {}...\n", runnable_target->resolved_manifest.name);
        return build::system::run_process(spec);
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
}

int cmd_debug(int argc, char* argv[]) {
    try {
        auto debug_defs = workspace_select_defs({
            cli::Flag{"--release"},
            cli::Option{"--target"},
            cli::Option{"--debugger"},
        });
        auto args = cli::parse(argc, argv, 2, debug_defs);
        auto release = args.has("--release");
        auto target = std::string{args.get("--target")};
        if (!target.empty()) {
            return command_error("exon debug currently supports native host executables only; "
                                 "--target is not supported yet");
        }

        auto program_args = std::vector<std::string>{};
        for (auto const& arg : args.positional())
            program_args.push_back(arg);

        auto debugger_request = std::string{args.get("--debugger")};
        if (debugger_request.empty())
            debugger_request = "auto";
        auto requested_kind = debug::classify_debugger_program(debugger_request);
        if (requested_kind == debug::DebuggerKind::devenv &&
            debug::has_devenv_unsafe_program_args(program_args)) {
            return command_error(
                "devenv cannot safely launch programs whose arguments start with '/'",
                "use --debugger cdb, lldb, gdb, or a different debugger path for this command");
        }

        auto requested_members = args.get_list("--member");
        auto excluded_members = args.get_list("--exclude");
        auto project_root = std::filesystem::current_path();
        auto raw_m = load_manifest(project_root);
        if (!check_platform(raw_m, {}))
            return 1;

        auto runnable_target = resolve_runnable_command_target(project_root, raw_m, {},
                                                               requested_members, excluded_members);
        if (!runnable_target)
            return command_error(runnable_target.error());

        auto host = toolchain::detect_host_platform();
        auto skipped_auto_kinds = std::vector<debug::DebuggerKind>{};
        auto skipped_devenv_for_args =
            debugger_request == "auto" && debug::has_devenv_unsafe_program_args(program_args);
        if (skipped_devenv_for_args)
            skipped_auto_kinds.push_back(debug::DebuggerKind::devenv);
        auto debugger_program =
            skipped_devenv_for_args
                ? debug::system::resolve_debugger(debugger_request, host, skipped_auto_kinds)
                : debug::system::resolve_debugger(debugger_request, host);
        if (!debugger_program)
            return skipped_devenv_for_args
                       ? command_error(
                             debugger_program.error(),
                             "devenv was skipped because Visual Studio may treat '/'-prefixed "
                             "program arguments as its own switches")
                       : command_error(debugger_program.error());

        auto rc = build::system::run(runnable_target->root, runnable_target->resolved_manifest,
                                     runnable_target->raw_manifest, release, {});
        if (rc != 0)
            return rc;

        auto project = build::project_context(runnable_target->root, release, {});
        auto executable = project.build_dir / (runnable_target->resolved_manifest.name +
                                               std::string{toolchain::exe_suffix});
        auto spec = debug::debugger_launch_spec(*debugger_program, executable, program_args,
                                                runnable_target->root);
        std::println("starting {} for {}...\n", debug::debugger_kind_name(debugger_program->kind),
                     runnable_target->resolved_manifest.name);
        return build::system::run_process(spec);
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
}

int cmd_test(int argc, char* argv[]) {
    try {
        std::vector<cli::ArgDef> const test_defs = {
            cli::Flag{"--release"},
            cli::Option{"--target"},
            cli::Option{"--filter"},
            cli::Option{"--timeout"},
        };
        auto args =
            cli::parse(argc, argv, 2, workspace_select_defs(reporting_defs(test_defs, true)));
        auto release = args.has("--release");
        auto target = std::string{args.get("--target")};
        auto filter = std::string{args.get("--filter")};
        auto timeout = parse_timeout(args.get("--timeout"));
        auto options = parse_reporting_options(args, true);
        auto requested_members = args.get_list("--member");
        auto excluded_members = args.get_list("--exclude");
        auto project_root = std::filesystem::current_path();
        auto raw_m = load_manifest(project_root);
        if (!check_platform(raw_m, target))
            return 1;
        auto m = resolve_manifest(raw_m, target);
        if (manifest::is_workspace(raw_m)) {
            auto selection = select_workspace_members(
                project_root, raw_m, target, requested_members, excluded_members, false, false);
            return run_workspace_test(project_root, selection, release, target, filter, timeout,
                                      options);
        }
        if (!requested_members.empty() || !excluded_members.empty())
            return command_error("--member and --exclude require a workspace root");
        if (m.name.empty())
            return command_error("package name is required in exon.toml");
        return build::system::run_test(project_root, m, raw_m, release, target, filter, timeout,
                                       options);
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
}

int cmd_clean(int argc, char* argv[]) {
    auto project_root = std::filesystem::current_path();
    try {
        auto args = cli::parse(argc, argv, 2, workspace_select_defs());
        if (std::filesystem::exists(project_root / "exon.toml")) {
            auto m = load_manifest(project_root);
            if (manifest::is_workspace(m)) {
                auto selection =
                    select_workspace_members(project_root, m, {}, args.get_list("--member"),
                                             args.get_list("--exclude"), false, false);
                auto rc =
                    run_selected_workspace_members(project_root, selection, [](auto const& member) {
                        auto dir = member.path / ".exon";
                        if (std::filesystem::exists(dir)) {
                            std::filesystem::remove_all(dir);
                            print_status(terminal::StatusKind::ok, "cleaned .exon/");
                        } else {
                            print_status(terminal::StatusKind::skip, "nothing to clean");
                        }
                        return 0;
                    });
                if (rc != 0)
                    return rc;
                auto workspace_exon_dir = project_root / ".exon";
                if (std::filesystem::exists(workspace_exon_dir)) {
                    std::filesystem::remove_all(workspace_exon_dir);
                    print_status(terminal::StatusKind::ok, "cleaned workspace .exon/");
                }
                return 0;
            }
        }
        if (!args.get_list("--member").empty() || !args.get_list("--exclude").empty())
            return command_error("--member and --exclude require a workspace root");
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
    auto exon_dir = project_root / ".exon";
    if (std::filesystem::exists(exon_dir)) {
        std::filesystem::remove_all(exon_dir);
        print_status(terminal::StatusKind::ok, "cleaned .exon/");
    } else {
        print_status(terminal::StatusKind::skip, "nothing to clean");
    }
    return 0;
}

std::string toml_string_literal(std::string_view value) {
    std::string out = "\"";
    for (char c : value) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += c;
            break;
        }
    }
    out += '"';
    return out;
}

std::pair<std::string, std::string> parse_key_value_option(std::string_view value) {
    auto eq = value.find('=');
    if (eq == std::string_view::npos || eq == 0)
        throw std::runtime_error(std::format("invalid --option '{}'; expected K=V", value));
    return {std::string{value.substr(0, eq)}, std::string{value.substr(eq + 1)}};
}

int cmd_add(int argc, char* argv[]) {
    try {
        auto args = cli::parse(argc, argv, 2,
                               {
                                   cli::Flag{"--dev"},
                                   cli::Flag{"--path"},
                                   cli::Flag{"--workspace"},
                                   cli::Flag{"--vcpkg"},
                                   cli::Flag{"--cmake"},
                                   cli::Flag{"--no-default-features"},
                                   cli::Option{"--git"},
                                   cli::Option{"--subdir"},
                                   cli::Option{"--version"},
                                   cli::Option{"--name"},
                                   cli::Option{"--repo"},
                                   cli::Option{"--tag"},
                                   cli::Option{"--targets"},
                                   cli::Option{"--shallow"},
                                   cli::ListOption{"--features"},
                                   cli::ListOption{"--option"},
                               });
        bool dev = args.has("--dev");
        bool is_path = args.has("--path");
        bool is_workspace_dep = args.has("--workspace");
        bool is_vcpkg = args.has("--vcpkg");
        bool is_cmake = args.has("--cmake");
        bool is_git_subdir = !args.get("--git").empty();
        bool no_default_features = args.has("--no-default-features");
        auto features = args.get_list("--features");
        auto cmake_options = args.get_list("--option");
        auto git_repo = std::string{args.get("--git")};
        auto git_version = std::string{args.get("--version")};
        auto git_subdir = std::string{args.get("--subdir")};
        auto git_name = std::string{args.get("--name")};
        auto cmake_repo = std::string{args.get("--repo")};
        auto cmake_tag = std::string{args.get("--tag")};
        auto cmake_targets = std::string{args.get("--targets")};
        auto cmake_shallow = std::string{args.get("--shallow")};
        auto& positional = args.positional();

        if (!features.empty() && (is_path || is_workspace_dep || is_git_subdir || is_cmake))
            return command_error(
                "--features is not valid with --path, --workspace, --git, or --cmake");
        if (no_default_features && features.empty())
            return command_error("--no-default-features requires --features");
        if (!is_cmake && (!cmake_repo.empty() || !cmake_tag.empty() || !cmake_targets.empty() ||
                          !cmake_options.empty() || !cmake_shallow.empty())) {
            return command_error(
                "--repo, --tag, --targets, --option, and --shallow require --cmake");
        }

        int exclusive_count = int(is_path) + int(is_workspace_dep) + int(is_vcpkg) + int(is_cmake) +
                              int(is_git_subdir);
        if (exclusive_count > 1)
            return command_error(
                "--path, --workspace, --vcpkg, --cmake, --git are mutually exclusive");

        if (!std::filesystem::exists("exon.toml")) {
            std::println(std::cerr, "error: exon.toml not found. run 'exon init' first");
            return 1;
        }

        auto project_root = std::filesystem::current_path();
        auto manifest_path = project_root / "exon.toml";
        auto content = read_text(manifest_path);
        auto m = manifest::system::load(manifest_path.string());

        std::string name, value;
        std::string section_prefix = dev ? "dev-dependencies" : "dependencies";
        std::string section;
        std::string dep_line;
        std::string display;
        std::string options_section;
        std::string options_lines;

        if (is_path) {
            if (positional.size() < 2) {
                std::println(std::cerr, "usage: exon add [--dev] --path <name> <path>");
                return 1;
            }
            name = positional[0];
            value = positional[1];
            auto target_dir = project_root / value;
            if (!std::filesystem::exists(target_dir / "exon.toml")) {
                std::println(std::cerr, "error: no exon.toml found at {}", target_dir.string());
                return 1;
            }
            section = section_prefix + ".path";
            dep_line = std::format("{} = \"{}\"\n", name, value);
            display = std::format("path dep {} = \"{}\"", name, value);
        } else if (is_workspace_dep) {
            if (positional.size() < 1) {
                std::println(std::cerr, "usage: exon add [--dev] --workspace <name>");
                return 1;
            }
            name = positional[0];
            auto ws_root = manifest::system::find_workspace_root(project_root);
            if (!ws_root) {
                std::println(std::cerr, "error: no workspace root found");
                return 1;
            }
            auto ws_m = manifest::system::load((*ws_root / "exon.toml").string());
            if (!manifest::system::resolve_workspace_member(*ws_root, ws_m, name)) {
                std::println(std::cerr, "error: workspace member '{}' not found", name);
                return 1;
            }
            section = section_prefix + ".workspace";
            dep_line = std::format("{} = true\n", name);
            display = std::format("workspace dep {}", name);
        } else if (is_git_subdir) {
            if (git_repo.empty() || git_version.empty() || git_subdir.empty()) {
                std::println(std::cerr,
                             "usage: exon add [--dev] --git <repo> --version <ver> --subdir <dir> "
                             "[--name <name>]");
                return 1;
            }
            name = git_name.empty() ? git_subdir : git_name;
            section = section_prefix;
            dep_line = std::format("{} = {{ git = \"{}\", version = \"{}\", subdir = \"{}\" }}\n",
                                   name, git_repo, git_version, git_subdir);
            display = std::format("git subdir dep {} = {{ git = \"{}\", version = \"{}\", "
                                  "subdir = \"{}\" }}",
                                  name, git_repo, git_version, git_subdir);
        } else if (is_vcpkg) {
            if (positional.size() < 2) {
                std::println(std::cerr,
                             "usage: exon add [--dev] --vcpkg <name> <version> [--features a,b,c]");
                return 1;
            }
            name = positional[0];
            value = positional[1];
            section = section_prefix + ".vcpkg";
            if (features.empty()) {
                dep_line = std::format("{} = \"{}\"\n", name, value);
                display = std::format("vcpkg dep {} = \"{}\"", name, value);
            } else {
                std::string feat_list;
                for (std::size_t fi = 0; fi < features.size(); ++fi) {
                    if (fi > 0)
                        feat_list += ", ";
                    feat_list += std::format("\"{}\"", features[fi]);
                }
                dep_line = std::format("{} = {{ version = \"{}\", features = [{}] }}\n", name,
                                       value, feat_list);
                display = std::format("vcpkg dep {} = {{ version = \"{}\", features = [{}] }}",
                                      name, value, feat_list);
            }
        } else if (is_cmake) {
            if (positional.size() < 1 || cmake_repo.empty() || cmake_tag.empty() ||
                cmake_targets.empty()) {
                std::println(std::cerr,
                             "usage: exon add [--dev] --cmake <name> --repo <url> "
                             "--tag <tag> --targets <targets> [--option K=V] [--shallow false]");
                return 1;
            }
            if (!cmake_shallow.empty() && cmake_shallow != "true" && cmake_shallow != "false")
                return command_error("--shallow must be true or false");

            name = positional[0];
            section = section_prefix + ".cmake." + name;
            dep_line =
                std::format("git = {}\ntag = {}\ntargets = {}\n", toml_string_literal(cmake_repo),
                            toml_string_literal(cmake_tag), toml_string_literal(cmake_targets));
            if (cmake_shallow == "false")
                dep_line += "shallow = false\n";
            if (!cmake_options.empty()) {
                options_section = section + ".options";
                for (auto const& option : cmake_options) {
                    auto [key, option_value] = parse_key_value_option(option);
                    options_lines +=
                        std::format("{} = {}\n", key, toml_string_literal(option_value));
                }
            }
            display =
                std::format("cmake dep {} = {{ git = \"{}\", tag = \"{}\", targets = \"{}\" }}",
                            name, cmake_repo, cmake_tag, cmake_targets);
        } else {
            if (positional.size() < 2) {
                std::println(std::cerr,
                             "usage: exon add [--dev] <package> <version> [--features a,b "
                             "--no-default-features]\n"
                             "       exon add [--dev] --path <name> <path>\n"
                             "       exon add [--dev] --workspace <name>\n"
                             "       exon add [--dev] --vcpkg <name> <version>\n"
                             "       exon add [--dev] --cmake <name> --repo <url> "
                             "--tag <tag> --targets <targets> [--option K=V] [--shallow false]\n"
                             "       exon add [--dev] --git <repo> --version <ver> --subdir <dir> "
                             "[--name <name>]");
                return 1;
            }
            name = positional[0];
            value = positional[1];
            section = section_prefix;
            if (!features.empty()) {
                std::string feat_list;
                for (std::size_t fi = 0; fi < features.size(); ++fi) {
                    if (fi > 0)
                        feat_list += ", ";
                    feat_list += std::format("\"{}\"", features[fi]);
                }
                if (no_default_features)
                    dep_line = std::format("\"{}\" = {{ version = \"{}\", default-features = "
                                           "false, features = [{}] }}\n",
                                           name, value, feat_list);
                else
                    dep_line = std::format("\"{}\" = {{ version = \"{}\", features = [{}] }}\n",
                                           name, value, feat_list);
                display =
                    std::format("{} {} = {{ version = \"{}\", features = [{}] }}",
                                dev ? "dev-dependency" : "dependency", name, value, feat_list);
            } else {
                dep_line = std::format("\"{}\" = \"{}\"\n", name, value);
                display = std::format("{} {} = \"{}\"", dev ? "dev-dependency" : "dependency", name,
                                      value);
            }
        }

        // use the base name for duplicate check (for git deps, key is the full URL path)
        auto dup_key = name;
        if (manifest::dependency_exists(m, dup_key)) {
            std::println(std::cerr, "error: '{}' is already a dependency", dup_key);
            return 1;
        }

        manifest::insert_into_section(content, section, dep_line);
        if (!options_section.empty())
            manifest::insert_into_section(content, options_section, options_lines);

        try {
            write_text(manifest_path, content);
        } catch (std::exception const& e) {
            std::println(std::cerr, "error: {}", e.what());
            return 1;
        }
        print_status(terminal::StatusKind::ok, std::format("added {}", display));
        return 0;
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
}

int cmd_remove(int argc, char* argv[]) {
    try {
        if (argc < 3) {
            std::println(std::cerr, "usage: exon remove <package>");
            return 1;
        }
        auto pkg = std::string{argv[2]};

        auto project_root = std::filesystem::current_path();
        auto manifest_path = project_root / "exon.toml";
        if (!std::filesystem::exists(manifest_path)) {
            std::println(std::cerr, "error: exon.toml not found. run 'exon init' first");
            return 1;
        }

        auto m = manifest::system::load(manifest_path.string());
        if (!manifest::dependency_exists(m, pkg)) {
            std::println(std::cerr, "error: '{}' is not a dependency", pkg);
            return 1;
        }

        // if this is a subdir dep, compute the composite lock name to erase later
        std::string lock_name_to_erase = pkg;
        auto find_subdir = [&]() -> manifest::GitSubdirDep const* {
            if (auto it = m.subdir_deps.find(pkg); it != m.subdir_deps.end())
                return &it->second;
            if (auto it = m.dev_subdir_deps.find(pkg); it != m.dev_subdir_deps.end())
                return &it->second;
            return nullptr;
        };
        if (auto const* sdep = find_subdir())
            lock_name_to_erase = std::format("{}#{}", sdep->repo, sdep->subdir);

        auto content = read_text(manifest_path);
        manifest::remove_dependency_entry(content, pkg);
        manifest::cleanup_empty_subsections(content);

        try {
            write_text(manifest_path, content);
        } catch (std::exception const& e) {
            std::println(std::cerr, "error: {}", e.what());
            return 1;
        }
        print_status(terminal::StatusKind::ok, std::format("removed {}", pkg));

        auto lock_path = project_root / "exon.lock";
        if (std::filesystem::exists(lock_path)) {
            auto lf = lock::system::load(lock_path.string());
            auto& pkgs = lf.packages;
            std::erase_if(pkgs, [&](auto const& p) { return p.name == lock_name_to_erase; });
            lock::system::save(lf, lock_path.string());
        }
        return 0;
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
}

reporting::OutputMode parse_dependency_output_mode(std::string_view value) {
    if (value.empty() || value == "human")
        return reporting::OutputMode::human;
    if (value == "json")
        return reporting::OutputMode::json;
    throw std::runtime_error(std::format("invalid --output '{}': expected human or json", value));
}

int cmd_outdated(int argc, char* argv[]) {
    try {
        auto args = cli::parse(argc, argv, 2, workspace_select_defs({cli::Option{"--output"}}));
        auto project_root = std::filesystem::current_path();
        auto manifest_path = project_root / "exon.toml";
        if (!std::filesystem::exists(manifest_path)) {
            std::println(std::cerr, "error: exon.toml not found. run 'exon init' first");
            return 1;
        }
        auto output = parse_dependency_output_mode(args.get("--output"));
        auto scoped_options = reporting::ScopedOptionsContext{reporting::Options{.output = output}};
        auto filters = std::vector<std::string>{args.positional().begin(), args.positional().end()};
        auto raw_m = load_manifest(project_root);
        auto statuses = std::vector<fetch::DependencyUpdateStatus>{};
        if (manifest::is_workspace(raw_m)) {
            auto selection =
                select_workspace_members(project_root, raw_m, {}, args.get_list("--member"),
                                         args.get_list("--exclude"), false, false);
            for (auto const& member : selection.members) {
                auto member_statuses =
                    dependency_statuses_for_project(member.path, member.resolved_manifest, filters);
                statuses.insert(statuses.end(), std::make_move_iterator(member_statuses.begin()),
                                std::make_move_iterator(member_statuses.end()));
            }
        } else {
            if (!args.get_list("--member").empty() || !args.get_list("--exclude").empty())
                return command_error("--member and --exclude require a workspace root");
            statuses =
                dependency_statuses_for_project(project_root, resolve_manifest(raw_m), filters);
        }
        if (output == reporting::OutputMode::json)
            emit_dependency_status_json(statuses);
        else
            print_dependency_statuses(statuses);
        return 0;
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
}

int cmd_update(int argc, char* argv[]) {
    try {
        auto args =
            cli::parse(argc, argv, 2,
                       workspace_select_defs({cli::Flag{"--dry-run"}, cli::Option{"--precise"}}));
        auto project_root = std::filesystem::current_path();
        auto manifest_path = project_root / "exon.toml";
        if (!std::filesystem::exists(manifest_path)) {
            std::println(std::cerr, "error: exon.toml not found. run 'exon init' first");
            return 1;
        }

        auto filters = std::vector<std::string>{args.positional().begin(), args.positional().end()};
        auto precise = std::string{args.get("--precise")};
        if (!precise.empty() && filters.size() != 1)
            return command_error("--precise requires exactly one package argument");
        auto dry_run = args.has("--dry-run");

        auto update_project = [&](std::filesystem::path const& root,
                                  manifest::Manifest const& m) -> int {
            if (!manifest_has_git_dependencies(m, true)) {
                print_status(terminal::StatusKind::skip, "no git dependencies to update");
                return 0;
            }
            auto lock_path = root / "exon.lock";
            auto before = lock::system::load(lock_path.string());
            auto result = fetch::system::fetch_all({
                .manifest = m,
                .project_root = root,
                .lock_path = lock_path,
                .include_dev = true,
                .update_packages = filters,
                .precise_version = precise.empty() ? std::optional<std::string>{}
                                                   : std::optional<std::string>{precise},
                .update_all = filters.empty(),
                .dry_run = dry_run,
            });
            print_update_diff(before, result.lock_file, dry_run);
            return 0;
        };

        auto raw_m = load_manifest(project_root);
        if (manifest::is_workspace(raw_m)) {
            auto selection =
                select_workspace_members(project_root, raw_m, {}, args.get_list("--member"),
                                         args.get_list("--exclude"), false, false);
            return run_selected_workspace_members(project_root, selection, [&](auto const& member) {
                return update_project(member.path, member.resolved_manifest);
            });
        }
        if (!args.get_list("--member").empty() || !args.get_list("--exclude").empty())
            return command_error("--member and --exclude require a workspace root");
        return update_project(project_root, resolve_manifest(raw_m));
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
}

int cmd_tree(int argc, char* argv[]) {
    try {
        auto args = cli::parse(argc, argv, 2,
                               workspace_select_defs({cli::Flag{"--dev"}, cli::Flag{"--features"},
                                                      cli::Option{"--output"}}));
        auto project_root = std::filesystem::current_path();
        auto manifest_path = project_root / "exon.toml";
        if (!std::filesystem::exists(manifest_path)) {
            std::println(std::cerr, "error: exon.toml not found. run 'exon init' first");
            return 1;
        }
        if (!args.positional().empty())
            return command_error(std::format("unexpected argument '{}'", args.positional()[0]));

        auto output = parse_dependency_output_mode(args.get("--output"));
        auto include_dev = args.has("--dev");
        auto show_features = args.has("--features");
        auto scoped_options = reporting::ScopedOptionsContext{reporting::Options{.output = output}};

        auto print_project_tree = [&](std::filesystem::path const& root,
                                      manifest::Manifest const& m) -> int {
            auto result = fetch::system::fetch_all({
                .manifest = m,
                .project_root = root,
                .lock_path = root / "exon.lock",
                .include_dev = include_dev,
            });
            auto graph = build_dependency_graph(m.name, m, result, include_dev);
            if (output == reporting::OutputMode::json)
                emit_dependency_tree_json(graph);
            else
                print_dependency_tree(graph, show_features);
            return 0;
        };

        auto raw_m = load_manifest(project_root);
        if (manifest::is_workspace(raw_m)) {
            auto selection =
                select_workspace_members(project_root, raw_m, {}, args.get_list("--member"),
                                         args.get_list("--exclude"), false, include_dev);
            return run_selected_workspace_members(project_root, selection, [&](auto const& member) {
                return print_project_tree(member.path, member.resolved_manifest);
            });
        }
        if (!args.get_list("--member").empty() || !args.get_list("--exclude").empty())
            return command_error("--member and --exclude require a workspace root");
        return print_project_tree(project_root, resolve_manifest(raw_m));
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
}

int cmd_why(int argc, char* argv[]) {
    try {
        auto args = cli::parse(
            argc, argv, 2, workspace_select_defs({cli::Flag{"--dev"}, cli::Option{"--output"}}));
        auto project_root = std::filesystem::current_path();
        auto manifest_path = project_root / "exon.toml";
        if (!std::filesystem::exists(manifest_path)) {
            std::println(std::cerr, "error: exon.toml not found. run 'exon init' first");
            return 1;
        }
        auto const& positional = args.positional();
        if (positional.size() != 1)
            return command_error("usage: exon why <pkg>");

        auto target = positional[0];
        auto output = parse_dependency_output_mode(args.get("--output"));
        auto include_dev = args.has("--dev");
        auto scoped_options = reporting::ScopedOptionsContext{reporting::Options{.output = output}};

        auto explain_project = [&](std::filesystem::path const& root, manifest::Manifest const& m,
                                   bool fail_if_missing) -> std::pair<int, bool> {
            auto result = fetch::system::fetch_all({
                .manifest = m,
                .project_root = root,
                .lock_path = root / "exon.lock",
                .include_dev = include_dev,
            });
            auto graph = build_dependency_graph(m.name, m, result, include_dev);
            auto paths = dependency_paths(graph, target);
            if (paths.empty()) {
                if (fail_if_missing)
                    return {
                        command_error(std::format("'{}' is not in the dependency graph", target)),
                        false};
                return {0, false};
            }
            if (output == reporting::OutputMode::json)
                emit_dependency_paths_json(target, paths);
            else
                print_dependency_paths(graph, paths);
            return {0, true};
        };

        auto raw_m = load_manifest(project_root);
        if (manifest::is_workspace(raw_m)) {
            auto selection =
                select_workspace_members(project_root, raw_m, {}, args.get_list("--member"),
                                         args.get_list("--exclude"), false, include_dev);
            auto found = false;
            auto fail_if_missing = selection.members.size() == 1;
            for (auto const& member : selection.members) {
                auto guard =
                    reporting::ScopedStageContext{workspace_display_name(member, project_root)};
                auto [rc, member_found] =
                    explain_project(member.path, member.resolved_manifest, fail_if_missing);
                if (rc != 0)
                    return rc;
                found = found || member_found;
            }
            if (!found)
                return command_error(
                    std::format("'{}' is not in the selected dependency graphs", target));
            return 0;
        }
        if (!args.get_list("--member").empty() || !args.get_list("--exclude").empty())
            return command_error("--member and --exclude require a workspace root");
        return explain_project(project_root, resolve_manifest(raw_m), true).first;
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
}

int cmd_sync(int argc, char* argv[]) {
    try {
        auto args = cli::parse(argc, argv, 2, workspace_select_defs());
        auto project_root = std::filesystem::current_path();
        auto raw_m = load_manifest(project_root);
        // for sync: merge ALL target sections for fetching, but keep raw for CMake generation
        auto fetch_m = manifest::resolve_all_targets(raw_m);
        if (manifest::is_workspace(raw_m)) {
            auto selection =
                select_workspace_members(project_root, raw_m, {}, args.get_list("--member"),
                                         args.get_list("--exclude"), true, false);
            auto rc =
                run_selected_workspace_members(project_root, selection, [&](auto const& member) {
                    return sync_workspace_member_cmake(member, std::nullopt, true);
                });
            if (rc != 0)
                return rc;
            return sync_workspace_root_cmake(project_root, raw_m, selection);
        }
        if (!args.get_list("--member").empty() || !args.get_list("--exclude").empty())
            return command_error("--member and --exclude require a workspace root");
        auto lock_path = (project_root / "exon.lock").string();
        auto fetch_result = fetch::system::fetch_all({
            .manifest = fetch_m,
            .project_root = project_root,
            .lock_path = std::filesystem::path{lock_path},
        });
        build::system::sync_root_cmake(project_root, raw_m, fetch_result.deps);
        return 0;
    } catch (std::exception const& e) {
        std::println(std::cerr, "error: {}", e.what());
        return 1;
    }
}

int cmd_fmt() {
    auto project_root = std::filesystem::current_path();
    auto src_dir = project_root / "src";
    if (!std::filesystem::exists(src_dir)) {
        std::println(std::cerr, "error: src/ directory not found");
        return 1;
    }

    std::vector<std::string> files;
    for (auto const& entry : std::filesystem::recursive_directory_iterator(src_dir)) {
        if (!entry.is_regular_file())
            continue;
        auto ext = entry.path().extension().string();
        if (ext == ".cpp" || ext == ".cppm" || ext == ".h" || ext == ".hpp") {
            files.push_back(entry.path().string());
        }
    }

    if (files.empty()) {
        print_status(terminal::StatusKind::skip, "no source files to format");
        return 0;
    }

    std::ranges::sort(files);
    core::ProcessSpec spec{
        .program = "clang-format",
        .args = {"-i"},
        .cwd = project_root,
    };
    spec.args.insert(spec.args.end(), files.begin(), files.end());

    int rc = build::system::run_process(spec);
    if (rc != 0) {
        std::println(std::cerr, "error: clang-format failed (is it installed?)");
        return 1;
    }
    print_status(terminal::StatusKind::ok, std::format("formatted {} file(s)", files.size()));
    return 0;
}

} // namespace commands
