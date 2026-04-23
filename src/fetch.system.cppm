export module fetch.system;
import std;
import fetch;
import manifest;
import manifest.system;
import reporting;
import reporting.system;
import toolchain;
import toolchain.system;
import lock;
import lock.system;

export namespace fetch::system {

namespace detail {

enum class DependencyKind {
    git,
    subdir,
    path,
};

struct DependencySpec {
    DependencyKind kind = DependencyKind::git;
    std::string key;             // repo key for git/subdir, unused for path
    std::string requested_name;  // explicit alias key from the manifest, if any
    std::string version;
    std::string subdir;
    std::filesystem::path path;
    std::vector<std::string> features;
    bool default_features = true;
    bool is_dev = false;
    bool is_root_direct = false;
    std::string requester = "<root>";
};

struct Candidate {
    std::string source_id;
    DependencyKind kind = DependencyKind::git;
    std::string key;
    std::string version;
    std::string commit;
    std::filesystem::path path;
    std::string subdir;
    manifest::Manifest manifest;
    std::string package_name;
    bool is_path = false;
    bool has_regular_request = false;
    bool has_dev_request = false;
    bool has_root_regular = false;
    bool has_root_dev = false;
    bool processed_regular_children = false;
    bool processed_dev_children = false;
    std::vector<std::string> features;
    bool default_features = true;
    std::vector<std::string> aliases;
    std::vector<std::string> dependency_names;
};

struct DiscoveryState {
    lock::LockFile const& existing_lock;
    std::vector<Candidate> candidates;
    std::map<std::string, std::size_t> source_to_index;
    std::optional<toolchain::Platform> platform;
};

std::filesystem::path cache_dir() {
    if (auto const* override_dir = std::getenv("EXON_CACHE_DIR"); override_dir && *override_dir)
        return std::filesystem::path{override_dir};
    auto home = toolchain::system::home_dir();
    if (home.empty())
        throw std::runtime_error("HOME (or USERPROFILE on Windows) not set");
    return home / ".exon" / "cache";
}

// strip leading "/" and Windows drive colon from absolute-path keys so they
// compose safely as directories under cache_dir ("C:/x" -> "C_/x").
std::string cache_safe_key(std::string const& dep_key) {
    std::string k = dep_key;
    while (!k.empty() && k.front() == '/')
        k.erase(k.begin());
    if (k.size() >= 2 && k[1] == ':')
        k[1] = '_';
    return k;
}

// "github.com/user/repo" -> "https://github.com/user/repo.git"
// "/path/to/local/repo" -> "file:///path/to/local/repo"
// "C:/path/to/local/repo" -> "file:///C:/path/to/local/repo" (Windows)
std::string to_git_url(std::string const& dep_key) {
    if (dep_key.starts_with("/"))
        return std::format("file://{}", dep_key);
    if (dep_key.size() >= 2 && dep_key[1] == ':')
        return std::format("file:///{}", dep_key);
    return std::format("https://{}.git", dep_key);
}

std::string to_git_tag(std::string const& version) {
    if (version.starts_with("v"))
        return version;
    return std::format("v{}", version);
}

std::string get_git_commit(std::filesystem::path const& repo_path) {
    auto head_path = repo_path / ".git" / "HEAD";
    auto file = std::ifstream(head_path);
    if (!file)
        return "";

    std::string line;
    std::getline(file, line);

    if (line.starts_with("ref: ")) {
        auto ref = line.substr(5);
        auto ref_path = repo_path / ".git" / ref;
        auto ref_file = std::ifstream(ref_path);
        if (!ref_file)
            return "";
        std::getline(ref_file, line);
    }

    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' '))
        line.pop_back();
    return line;
}

std::string short_path(std::filesystem::path const& path) {
    std::error_code ec;
    auto canon = std::filesystem::weakly_canonical(path, ec);
    if (!ec)
        return canon.generic_string();
    return path.lexically_normal().generic_string();
}

void add_unique(std::vector<std::string>& items, std::string const& value) {
    if (value.empty())
        return;
    if (std::ranges::find(items, value) == items.end())
        items.push_back(value);
}

std::string source_identity(DependencySpec const& spec) {
    switch (spec.kind) {
    case DependencyKind::git:
        return std::format("git|{}|{}", spec.key, spec.version);
    case DependencyKind::subdir:
        return std::format("subdir|{}|{}|{}", spec.key, spec.version, spec.subdir);
    case DependencyKind::path:
        return std::format("path|{}", short_path(spec.path));
    }
    throw std::runtime_error("unknown dependency kind");
}

std::string describe_source(Candidate const& candidate) {
    switch (candidate.kind) {
    case DependencyKind::git:
        return std::format("git {} {}", candidate.key, candidate.version);
    case DependencyKind::subdir:
        return std::format("git {} {} [{}]", candidate.key, candidate.version, candidate.subdir);
    case DependencyKind::path:
        return std::format("path {}", candidate.path.generic_string());
    }
    throw std::runtime_error("unknown dependency kind");
}

manifest::Manifest load_dependency_manifest(std::filesystem::path const& dep_toml,
                                            std::optional<toolchain::Platform> const& platform) {
    auto dep_m = manifest::system::load(dep_toml.string());
    if (platform)
        dep_m = manifest::resolve_for_platform(std::move(dep_m), *platform);
    else
        dep_m = manifest::resolve_all_targets(std::move(dep_m));
    if (dep_m.name.empty()) {
        throw std::runtime_error(std::format(
            "dependency at {} is missing [package].name", dep_toml.parent_path().string()));
    }
    return dep_m;
}

struct MaterializedGitSource {
    std::filesystem::path path;
    std::string commit;
};

MaterializedGitSource materialize_git_source(std::string const& repo, std::string const& version,
                                             std::string const& expected_commit = {}) {
    auto cache_root = cache_dir();
    auto tag = to_git_tag(version);
    auto dep_cache = cache_root / cache_safe_key(repo) / tag;

    if (std::filesystem::exists(dep_cache)) {
        auto current_commit = get_git_commit(dep_cache);
        if (expected_commit.empty() || current_commit == expected_commit) {
            if (!expected_commit.empty())
                std::println("  locked: {} {} ({})", repo, tag, current_commit.substr(0, 8));
            else
                std::println("  cached: {} {} ({})", repo, tag, current_commit.substr(0, 8));
            return {.path = dep_cache, .commit = current_commit};
        }
        std::filesystem::remove_all(dep_cache);
    }

    std::filesystem::create_directories(dep_cache);
    auto git_url = to_git_url(repo);
    auto cmd = std::format("git clone --depth 1 --branch {} {} {} 2>&1", tag, git_url,
                           toolchain::shell_quote(dep_cache.string()));
    std::println("  fetching: {} {}...", repo, tag);
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::filesystem::remove_all(dep_cache);
        throw std::runtime_error(std::format("failed to fetch {} {}", repo, tag));
    }
    return {.path = dep_cache, .commit = get_git_commit(dep_cache)};
}

Candidate materialize_candidate(DependencySpec const& spec, DiscoveryState const& state) {
    Candidate candidate;
    candidate.source_id = source_identity(spec);
    candidate.kind = spec.kind;

    std::filesystem::path dep_path;
    std::string commit;

    switch (spec.kind) {
    case DependencyKind::git: {
        auto locked = state.existing_lock.find(spec.key, spec.version);
        auto source = materialize_git_source(spec.key, spec.version, locked ? locked->commit : "");
        dep_path = source.path;
        commit = std::move(source.commit);
        candidate.key = spec.key;
        candidate.version = spec.version;
        break;
    }
    case DependencyKind::subdir: {
        auto lock_name = std::format("{}#{}", spec.key, spec.subdir);
        auto locked = state.existing_lock.find(lock_name, spec.version);
        auto source = materialize_git_source(spec.key, spec.version, locked ? locked->commit : "");
        dep_path = source.path / spec.subdir;
        if (!std::filesystem::exists(dep_path)) {
            throw std::runtime_error(std::format(
                "git dep '{}' ({}@{}): subdir '{}' not found in repo",
                spec.requested_name.empty() ? spec.key : spec.requested_name,
                spec.key, spec.version, spec.subdir));
        }
        commit = std::move(source.commit);
        candidate.key = spec.key;
        candidate.version = spec.version;
        candidate.subdir = spec.subdir;
        break;
    }
    case DependencyKind::path:
        {
            std::error_code ec;
            dep_path = std::filesystem::weakly_canonical(spec.path, ec);
            if (ec)
                dep_path = spec.path.lexically_normal();
        }
        candidate.is_path = true;
        break;
    }

    auto dep_toml = dep_path / "exon.toml";
    if (!std::filesystem::exists(dep_toml)) {
        throw std::runtime_error(std::format(
            "dependency '{}' has no exon.toml at {}",
            spec.requested_name.empty() ? dep_path.filename().string() : spec.requested_name,
            dep_path.string()));
    }

    candidate.path = dep_path;
    candidate.commit = std::move(commit);
    candidate.manifest = load_dependency_manifest(dep_toml, state.platform);
    candidate.package_name = candidate.manifest.name;
    if (candidate.is_path)
        candidate.key = candidate.package_name;
    return candidate;
}

void merge_request(Candidate& candidate, DependencySpec const& spec) {
    if (!spec.requested_name.empty() && spec.requested_name != candidate.package_name)
        add_unique(candidate.aliases, spec.requested_name);

    if (spec.is_dev) {
        candidate.has_dev_request = true;
        if (spec.is_root_direct)
            candidate.has_root_dev = true;
    } else {
        candidate.has_regular_request = true;
        if (spec.is_root_direct)
            candidate.has_root_regular = true;
    }

    for (auto const& feature : spec.features)
        add_unique(candidate.features, feature);
    candidate.default_features = candidate.default_features && spec.default_features;
}

std::vector<DependencySpec> collect_child_specs(Candidate const& candidate, bool is_dev) {
    std::vector<DependencySpec> specs;

    auto add_spec = [&](DependencySpec spec) {
        spec.is_dev = is_dev;
        spec.requester = candidate.package_name;
        specs.push_back(std::move(spec));
    };

    for (auto const& [repo, version] : candidate.manifest.dependencies) {
        add_spec({
            .kind = DependencyKind::git,
            .key = repo,
            .version = version,
        });
    }

    for (auto const& [repo, fdep] : candidate.manifest.featured_deps) {
        add_spec({
            .kind = DependencyKind::git,
            .key = repo,
            .version = fdep.version,
            .features = fdep.features,
            .default_features = fdep.default_features,
        });
    }

    for (auto const& [name, sdep] : candidate.manifest.subdir_deps) {
        add_spec({
            .kind = DependencyKind::subdir,
            .key = sdep.repo,
            .requested_name = name,
            .version = sdep.version,
            .subdir = sdep.subdir,
        });
    }

    for (auto const& [name, rel] : candidate.manifest.path_deps) {
        add_spec({
            .kind = DependencyKind::path,
            .requested_name = name,
            .path = candidate.path / rel,
        });
    }

    if (!candidate.manifest.workspace_deps.empty()) {
        auto ws_root = manifest::system::find_workspace_root(candidate.path);
        if (!ws_root) {
            throw std::runtime_error(std::format(
                "dependency '{}' uses [dependencies.workspace] but no workspace root found",
                candidate.package_name));
        }
        auto ws_m = manifest::system::load((*ws_root / "exon.toml").string());
        for (auto const& ws_name : candidate.manifest.workspace_deps) {
            auto member_path = manifest::system::resolve_workspace_member(*ws_root, ws_m, ws_name);
            if (!member_path) {
                throw std::runtime_error(
                    std::format("workspace member '{}' not found in workspace", ws_name));
            }
            add_spec({
                .kind = DependencyKind::path,
                .requested_name = ws_name,
                .path = *member_path,
            });
        }
    }

    return specs;
}

std::size_t discover_candidate(DependencySpec const& spec, DiscoveryState& state) {
    auto source_id = source_identity(spec);
    auto existing = state.source_to_index.find(source_id);
    if (existing == state.source_to_index.end()) {
        auto candidate = materialize_candidate(spec, state);
        merge_request(candidate, spec);
        candidate.processed_regular_children = !spec.is_dev;
        candidate.processed_dev_children = spec.is_dev;

        auto index = state.candidates.size();
        state.source_to_index.emplace(source_id, index);
        state.candidates.push_back(std::move(candidate));

        auto child_specs = collect_child_specs(state.candidates[index], spec.is_dev);
        for (auto const& child_spec : child_specs) {
            auto child_index = discover_candidate(child_spec, state);
            add_unique(state.candidates[index].dependency_names,
                       state.candidates[child_index].package_name);
        }
        return index;
    }

    auto index = existing->second;
    auto& candidate = state.candidates[index];
    auto needs_children = spec.is_dev
        ? !candidate.processed_dev_children
        : !candidate.processed_regular_children;
    merge_request(candidate, spec);

    if (needs_children) {
        if (spec.is_dev)
            candidate.processed_dev_children = true;
        else
            candidate.processed_regular_children = true;

        auto child_specs = collect_child_specs(candidate, spec.is_dev);
        for (auto const& child_spec : child_specs) {
            auto child_index = discover_candidate(child_spec, state);
            add_unique(candidate.dependency_names, state.candidates[child_index].package_name);
        }
    }

    return index;
}

std::vector<std::size_t> distinct_source_indices(std::vector<std::size_t> const& indices,
                                                 std::vector<Candidate> const& candidates,
                                                 bool root_only) {
    std::vector<std::size_t> distinct;
    std::set<std::string> seen;
    for (auto const index : indices) {
        auto const& candidate = candidates[index];
        auto is_root = candidate.has_root_regular || candidate.has_root_dev;
        if (root_only && !is_root)
            continue;
        if (seen.insert(candidate.source_id).second)
            distinct.push_back(index);
    }
    return distinct;
}

[[noreturn]] void throw_conflict(std::string const& package_name,
                                 std::vector<std::size_t> const& indices,
                                 std::vector<Candidate> const& candidates,
                                 bool root_conflict) {
    std::ostringstream msg;
    if (root_conflict)
        msg << "conflicting direct dependency overrides for package '" << package_name << "':";
    else
        msg << "conflicting transitive dependency versions for package '" << package_name << "':";

    for (auto const index : indices) {
        auto const& candidate = candidates[index];
        msg << "\n  - " << describe_source(candidate);
    }
    throw std::runtime_error(msg.str());
}

std::map<std::string, std::size_t> select_candidates(std::vector<Candidate> const& candidates) {
    std::map<std::string, std::vector<std::size_t>> by_package;
    for (std::size_t i = 0; i < candidates.size(); ++i)
        by_package[candidates[i].package_name].push_back(i);

    std::map<std::string, std::size_t> selected;
    for (auto const& [package_name, indices] : by_package) {
        auto root_direct = distinct_source_indices(indices, candidates, true);
        if (!root_direct.empty()) {
            if (root_direct.size() > 1)
                throw_conflict(package_name, root_direct, candidates, true);
            selected.emplace(package_name, root_direct.front());
            continue;
        }

        auto all_distinct = distinct_source_indices(indices, candidates, false);
        if (all_distinct.size() > 1)
            throw_conflict(package_name, all_distinct, candidates, false);
        selected.emplace(package_name, all_distinct.front());
    }
    return selected;
}

void topo_visit(std::string const& package_name,
                std::map<std::string, std::size_t> const& selected,
                std::vector<Candidate> const& candidates,
                std::map<std::string, int>& marks,
                std::vector<std::string>& order) {
    auto& mark = marks[package_name];
    if (mark == 2)
        return;
    if (mark == 1)
        throw std::runtime_error(std::format("cyclic dependency detected at '{}'", package_name));

    mark = 1;
    auto const& candidate = candidates[selected.at(package_name)];
    for (auto const& child_name : candidate.dependency_names) {
        if (selected.contains(child_name))
            topo_visit(child_name, selected, candidates, marks, order);
    }
    mark = 2;
    order.push_back(package_name);
}

std::vector<std::string> topo_sort_selected(std::map<std::string, std::size_t> const& selected,
                                            std::vector<Candidate> const& candidates) {
    std::vector<std::string> order;
    std::map<std::string, int> marks;
    for (auto const& [package_name, _] : selected)
        topo_visit(package_name, selected, candidates, marks, order);
    return order;
}

lock::LockFile build_selected_lock(std::vector<fetch::FetchedDep> const& deps) {
    lock::LockFile lf;
    std::vector<lock::LockedDep> packages;
    for (auto const& dep : deps) {
        if (dep.is_path)
            continue;

        lock::LockedDep locked;
        locked.name = dep.subdir.empty() ? dep.key : std::format("{}#{}", dep.key, dep.subdir);
        locked.package = dep.package_name;
        locked.version = dep.version;
        locked.commit = dep.commit;
        locked.subdir = dep.subdir;
        locked.features = dep.features;
        packages.push_back(std::move(locked));
    }

    std::ranges::sort(packages, [](lock::LockedDep const& lhs, lock::LockedDep const& rhs) {
        return std::tie(lhs.name, lhs.version, lhs.package) <
               std::tie(rhs.name, rhs.version, rhs.package);
    });

    lf.packages = std::move(packages);
    return lf;
}

fetch::FetchedDep make_fetched_dep(Candidate const& candidate) {
    fetch::FetchedDep dep;
    dep.key = candidate.is_path ? candidate.package_name : candidate.key;
    dep.name = candidate.package_name;
    dep.package_name = candidate.package_name;
    dep.version = candidate.version;
    dep.commit = candidate.commit;
    dep.path = candidate.path;
    dep.subdir = candidate.subdir;
    dep.is_dev = !candidate.has_regular_request;
    dep.is_path = candidate.is_path;
    dep.features = candidate.features;
    dep.default_features = candidate.default_features;
    dep.aliases = candidate.aliases;
    dep.dependency_names = candidate.dependency_names;
    return dep;
}

DependencySpec decode_root(fetch::FetchRoot const& root, std::filesystem::path const& project_root,
                           std::optional<std::filesystem::path> const& ws_root,
                           std::optional<manifest::Manifest> const& ws_manifest) {
    DependencySpec spec;
    spec.is_dev = root.is_dev;
    spec.is_root_direct = true;

    switch (root.kind) {
    case fetch::RootKind::git:
    case fetch::RootKind::featured_git:
        spec.kind = DependencyKind::git;
        spec.key = root.key;
        spec.version = root.value;
        spec.features = root.features;
        spec.default_features = root.default_features;
        break;
    case fetch::RootKind::subdir: {
        auto first = root.value.find('|');
        auto second = root.value.find('|', first == std::string::npos ? first : first + 1);
        if (first == std::string::npos || second == std::string::npos)
            throw std::runtime_error("invalid subdir dependency plan entry");
        spec.kind = DependencyKind::subdir;
        spec.key = std::string{root.value.substr(0, first)};
        spec.requested_name = root.key;
        spec.version = std::string{root.value.substr(first + 1, second - first - 1)};
        spec.subdir = std::string{root.value.substr(second + 1)};
        break;
    }
    case fetch::RootKind::path:
        spec.kind = DependencyKind::path;
        spec.requested_name = root.key;
        spec.path = project_root / root.value;
        break;
    case fetch::RootKind::workspace: {
        if (!ws_root || !ws_manifest) {
            auto section = root.is_dev ? "[dev-dependencies.workspace]" : "[dependencies.workspace]";
            throw std::runtime_error(std::format("{} used but no workspace root found", section));
        }
        auto member_path =
            manifest::system::resolve_workspace_member(*ws_root, *ws_manifest, root.key);
        if (!member_path)
            throw std::runtime_error(
                std::format("workspace member '{}' not found in workspace", root.key));
        spec.kind = DependencyKind::path;
        spec.requested_name = root.key;
        spec.path = *member_path;
        break;
    }
    }

    return spec;
}

} // namespace detail

struct FetchProgressCounter {
    std::atomic<int> done{0};
    std::atomic<int> total{0};
    std::atomic<bool> discovery_phase{true};
};

reporting::ProgressSource as_progress_source(FetchProgressCounter const& counter) {
    return reporting::ProgressSource{
        .poll = [&counter]() -> std::optional<reporting::ProgressSnapshot> {
            auto done = counter.done.load(std::memory_order_relaxed);
            auto total = counter.total.load(std::memory_order_relaxed);
            auto discovering = counter.discovery_phase.load(std::memory_order_relaxed);
            if (discovering)
                return reporting::ProgressSnapshot{.label = "discover"};
            if (total <= 0)
                return reporting::ProgressSnapshot{.label = "resolve"};
            auto percent = static_cast<int>(
                (static_cast<std::int64_t>(done) * 100) / total);
            return reporting::ProgressSnapshot{
                .done = done,
                .total = total,
                .percent = percent,
                .label = "resolve",
            };
        },
    };
}

fetch::FetchResult fetch_all(fetch::FetchRequest const& request) {
    fetch::FetchResult result;
    if (!fetch::has_dependencies(request.manifest, request.include_dev))
        return result;

    auto existing_lock = lock::system::load(request.lock_path.string());
    detail::DiscoveryState state{
        .existing_lock = existing_lock,
        .platform = request.platform,
    };

    if (reporting::current_stage_context.empty())
        std::println("==> fetch");
    else
        std::println("==> [{}] fetch", reporting::current_stage_context);
    auto counter = FetchProgressCounter{};
    auto renderer = reporting::system::make_live_progress_renderer();
    if (renderer && renderer->active())
        renderer->start(as_progress_source(counter));

    auto fetch_plan = fetch::plan(request);

    std::optional<std::filesystem::path> ws_root;
    std::optional<manifest::Manifest> ws_manifest;
    auto ensure_workspace = [&]() {
        if (!ws_root) {
            ws_root = manifest::system::find_workspace_root(request.project_root);
            if (ws_root)
                ws_manifest = manifest::system::load((*ws_root / "exon.toml").string());
        }
    };

    for (auto const& root : fetch_plan.roots) {
        if (root.kind == fetch::RootKind::workspace)
            ensure_workspace();
        auto spec = detail::decode_root(root, request.project_root, ws_root, ws_manifest);
        (void)detail::discover_candidate(spec, state);
    }

    auto selected = detail::select_candidates(state.candidates);
    auto order = detail::topo_sort_selected(selected, state.candidates);

    counter.total.store(static_cast<int>(order.size()), std::memory_order_relaxed);
    counter.discovery_phase.store(false, std::memory_order_relaxed);

    for (auto const& package_name : order) {
        auto const& candidate = state.candidates[selected.at(package_name)];
        result.deps.push_back(detail::make_fetched_dep(candidate));
        counter.done.fetch_add(1, std::memory_order_relaxed);
    }

    result.lock_file = detail::build_selected_lock(result.deps);
    lock::system::save(result.lock_file, request.lock_path.string());

    if (renderer)
        renderer->stop();
    return result;
}

fetch::FetchResult fetch_all(std::filesystem::path const& project_root,
                             manifest::Manifest const& m,
                             std::filesystem::path const& lock_path,
                             bool include_dev = false,
                             std::optional<toolchain::Platform> platform = std::nullopt) {
    return fetch_all({
        .manifest = m,
        .project_root = project_root,
        .lock_path = lock_path,
        .include_dev = include_dev,
        .platform = platform,
    });
}

fetch::FetchResult fetch_all(manifest::Manifest const& m, std::string_view lock_path,
                             bool include_dev = false,
                             std::optional<toolchain::Platform> platform = std::nullopt) {
    return fetch_all(std::filesystem::current_path(), m, std::filesystem::path{lock_path},
                     include_dev, platform);
}

} // namespace fetch::system
