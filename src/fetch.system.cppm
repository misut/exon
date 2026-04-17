export module fetch.system;
import std;
import fetch;
import manifest;
import manifest.system;
import toolchain;
import toolchain.system;
import lock;
import lock.system;

export namespace fetch::system {

namespace detail {

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
    if (dep_key.starts_with("/")) {
        return std::format("file://{}", dep_key);
    }
    // Windows absolute path: drive letter like "C:/..." or "C:\\..."
    if (dep_key.size() >= 2 && dep_key[1] == ':') {
        return std::format("file:///{}", dep_key);
    }
    return std::format("https://{}.git", dep_key);
}

// "github.com/user/repo" -> "repo"
std::string extract_repo_name(std::string const& dep_key) {
    auto last_slash = dep_key.rfind('/');
    if (last_slash == std::string::npos)
        return dep_key;
    return dep_key.substr(last_slash + 1);
}

// prepend "v" prefix if missing
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

    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) {
        line.pop_back();
    }
    return line;
}

struct FetchContext {
    lock::LockFile& lf;
    std::vector<fetch::FetchedDep>& result;
    std::set<std::string>& visited;               // git: "key@version"
    std::set<std::filesystem::path>& visited_paths; // path/workspace: absolute paths
    std::optional<toolchain::Platform> platform;  // for resolving target sections in path deps
};

void fetch_recursive(std::string const& dep_key, std::string const& version, lock::LockFile& lf,
                     std::vector<fetch::FetchedDep>& result, std::set<std::string>& visited);

// clone (or use cached) a git repo at the given tag; returns (cache_path, commit)
struct EnsureCloneResult {
    std::filesystem::path path;
    std::string commit;
};
EnsureCloneResult ensure_git_clone(std::string const& repo, std::string const& version) {
    auto cache_root = cache_dir();
    auto tag = to_git_tag(version);
    auto dep_cache = cache_root / cache_safe_key(repo) / tag;

    if (std::filesystem::exists(dep_cache)) {
        auto commit = get_git_commit(dep_cache);
        return {.path = dep_cache, .commit = commit};
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

void fetch_path_recursive(std::filesystem::path const& abs_dir, std::string name,
                          FetchContext ctx) {
    auto canon = std::filesystem::weakly_canonical(abs_dir);
    if (ctx.visited_paths.contains(canon))
        return;
    ctx.visited_paths.insert(canon);

    auto dep_toml = canon / "exon.toml";
    if (!std::filesystem::exists(dep_toml))
        throw std::runtime_error(std::format("path dep '{}' has no exon.toml at {}", name,
                                             canon.string()));

    auto dep_m = manifest::system::load(dep_toml.string());
    if (ctx.platform)
        dep_m = manifest::resolve_for_platform(std::move(dep_m), *ctx.platform);
    else
        dep_m = manifest::resolve_all_targets(std::move(dep_m));

    // recurse into transitive deps FIRST (topological: deps come before this package)
    for (auto const& [sub_key, sub_version] : dep_m.dependencies)
        fetch_recursive(sub_key, sub_version, ctx.lf, ctx.result, ctx.visited);
    for (auto const& [sub_name, sub_rel] : dep_m.path_deps) {
        auto sub_abs = canon / sub_rel;
        fetch_path_recursive(sub_abs, sub_name, ctx);
    }
    if (!dep_m.workspace_deps.empty()) {
        auto ws_root = manifest::system::find_workspace_root(canon);
        if (!ws_root)
            throw std::runtime_error(std::format(
                "path dep '{}' uses [dependencies.workspace] but no workspace root found", name));
        auto ws_m = manifest::system::load((*ws_root / "exon.toml").string());
        for (auto const& ws_name : dep_m.workspace_deps) {
            auto member_path =
                manifest::system::resolve_workspace_member(*ws_root, ws_m, ws_name);
            if (!member_path)
                throw std::runtime_error(
                    std::format("workspace member '{}' not found in workspace", ws_name));
            fetch_path_recursive(*member_path, ws_name, ctx);
        }
    }

    // now add this dep (after its transitive deps)
    std::println("  path: {} -> {}", name, canon.string());
    fetch::FetchedDep d;
    d.key = name;
    d.name = dep_m.name.empty() ? name : dep_m.name;
    d.path = canon;
    d.is_path = true;
    ctx.result.push_back(std::move(d));
}

void fetch_subdir_recursive(std::string const& name, manifest::GitSubdirDep const& sdep,
                            FetchContext ctx) {
    auto visit_key = std::format("{}#{}@{}", sdep.repo, sdep.subdir, sdep.version);
    if (ctx.visited.contains(visit_key))
        return;
    ctx.visited.insert(visit_key);

    auto clone = ensure_git_clone(sdep.repo, sdep.version);
    auto subdir_path = clone.path / sdep.subdir;
    if (!std::filesystem::exists(subdir_path))
        throw std::runtime_error(std::format(
            "git dep '{}' ({}@{}): subdir '{}' not found in repo", name, sdep.repo, sdep.version,
            sdep.subdir));
    auto dep_toml = subdir_path / "exon.toml";
    if (!std::filesystem::exists(dep_toml))
        throw std::runtime_error(std::format(
            "git dep '{}': {}/exon.toml not found (not a valid exon package)", name, sdep.subdir));

    auto lock_name = std::format("{}#{}", sdep.repo, sdep.subdir);
    auto msg_tag = to_git_tag(sdep.version);
    std::println("  subdir: {} {} {} [{}] ({})", name, sdep.repo, msg_tag, sdep.subdir,
                 clone.commit.substr(0, 8));

    // recurse only into deps that need external resolution (git-based); path/workspace deps
    // within the cloned repo are handled by the member's own CMakeLists via relative paths.
    auto dep_m = manifest::system::load(dep_toml.string());
    for (auto const& [sub_key, sub_version] : dep_m.dependencies)
        fetch_recursive(sub_key, sub_version, ctx.lf, ctx.result, ctx.visited);
    for (auto const& [sub_name, sub_sdep] : dep_m.subdir_deps)
        fetch_subdir_recursive(sub_name, sub_sdep, ctx);
    // validate sibling workspace members exist in the clone (fail fast with a clear error)
    if (!dep_m.workspace_deps.empty()) {
        auto ws_root = manifest::system::find_workspace_root(subdir_path);
        if (!ws_root)
            throw std::runtime_error(std::format(
                "git dep '{}' uses [dependencies.workspace] but no workspace root found in repo",
                name));
        auto ws_m = manifest::system::load((*ws_root / "exon.toml").string());
        for (auto const& ws_name : dep_m.workspace_deps) {
            if (!manifest::system::resolve_workspace_member(*ws_root, ws_m, ws_name))
                throw std::runtime_error(std::format(
                    "git dep '{}' needs workspace sibling '{}', not found in repo", name,
                    ws_name));
        }
    }

    ctx.lf.add_or_update({
        .name = lock_name,
        .version = sdep.version,
        .commit = clone.commit,
        .subdir = sdep.subdir,
    });

    fetch::FetchedDep d;
    d.key = sdep.repo;
    d.name = name;
    d.version = sdep.version;
    d.commit = clone.commit;
    d.path = subdir_path;
    d.subdir = sdep.subdir;
    d.is_path = false;
    ctx.result.push_back(std::move(d));
}

void fetch_recursive(std::string const& dep_key, std::string const& version, lock::LockFile& lf,
                     std::vector<fetch::FetchedDep>& result, std::set<std::string>& visited) {
    // prevent circular dependencies and duplicates
    auto visit_key = dep_key + "@" + version;
    if (visited.contains(visit_key))
        return;
    visited.insert(visit_key);

    auto cache_root = cache_dir();
    auto tag = to_git_tag(version);
    auto dep_cache = cache_root / cache_safe_key(dep_key) / tag;

    auto locked = lf.find(dep_key, version);

    fetch::FetchedDep dep;
    dep.key = dep_key;
    dep.name = extract_repo_name(dep_key);
    dep.version = version;

    // use cache if it exists and matches the lock file commit
    if (std::filesystem::exists(dep_cache) && locked) {
        auto current_commit = get_git_commit(dep_cache);
        if (current_commit == locked->commit) {
            std::println("  locked: {} {} ({})", dep_key, tag, current_commit.substr(0, 8));
            dep.commit = current_commit;
            dep.path = dep_cache;
        }
    }

    // use existing cache if no lock entry
    if (dep.path.empty() && std::filesystem::exists(dep_cache)) {
        auto commit = get_git_commit(dep_cache);
        std::println("  cached: {} {} ({})", dep_key, tag, commit.substr(0, 8));
        dep.commit = commit;
        dep.path = dep_cache;
    }

    // fresh clone
    if (dep.path.empty()) {
        std::filesystem::create_directories(dep_cache);

        auto git_url = to_git_url(dep_key);
        auto cmd = std::format("git clone --depth 1 --branch {} {} {} 2>&1", tag, git_url,
                               toolchain::shell_quote(dep_cache.string()));

        std::println("  fetching: {} {}...", dep_key, tag);
        int rc = std::system(cmd.c_str());
        if (rc != 0) {
            std::filesystem::remove_all(dep_cache);
            throw std::runtime_error(std::format("failed to fetch {} {}", dep_key, tag));
        }

        dep.commit = get_git_commit(dep_cache);
        dep.path = dep_cache;
    }

    lf.add_or_update({
        .name = dep.key,
        .version = dep.version,
        .commit = dep.commit,
    });

    // transitive: recursively resolve if dep has exon.toml
    auto dep_manifest_path = dep.path / "exon.toml";
    if (std::filesystem::exists(dep_manifest_path)) {
        auto dep_manifest = manifest::system::load(dep_manifest_path.string());
        for (auto const& [sub_key, sub_version] : dep_manifest.dependencies) {
            fetch_recursive(sub_key, sub_version, lf, result, visited);
        }
    }

    // append dep at end (topological order: dependencies come first)
    result.push_back(std::move(dep));
}

manifest::GitSubdirDep decode_subdir_dep(std::string_view value) {
    auto first = value.find('|');
    auto second = value.find('|', first == std::string_view::npos ? first : first + 1);
    if (first == std::string_view::npos || second == std::string_view::npos)
        throw std::runtime_error("invalid subdir dependency plan entry");
    return {
        .repo = std::string{value.substr(0, first)},
        .version = std::string{value.substr(first + 1, second - first - 1)},
        .subdir = std::string{value.substr(second + 1)},
    };
}

} // namespace detail

fetch::FetchResult fetch_all(fetch::FetchRequest const& request) {
    fetch::FetchResult result;
    if (!fetch::has_dependencies(request.manifest, request.include_dev))
        return result;

    result.lock_file = lock::system::load(request.lock_path.string());

    std::println("fetching dependencies...");
    std::set<std::string> visited;
    std::set<std::filesystem::path> visited_paths;
    detail::FetchContext ctx{result.lock_file, result.deps, visited, visited_paths, request.platform};

    auto fetch_plan = fetch::plan(request);

    std::optional<std::filesystem::path> ws_root;
    std::optional<manifest::Manifest> ws_m;
    auto get_ws = [&]() {
        if (!ws_root) {
            ws_root = manifest::system::find_workspace_root(request.project_root);
            if (ws_root)
                ws_m = manifest::system::load((*ws_root / "exon.toml").string());
        }
        return ws_root && ws_m;
    };

    std::size_t dev_start = result.deps.size();
    bool saw_dev_root = false;
    for (auto const& root : fetch_plan.roots) {
        if (root.is_dev && !saw_dev_root) {
            dev_start = result.deps.size();
            saw_dev_root = true;
        }

        switch (root.kind) {
        case fetch::RootKind::git:
        case fetch::RootKind::featured_git:
            detail::fetch_recursive(root.key, root.value, result.lock_file, result.deps, visited);
            break;
        case fetch::RootKind::subdir:
            detail::fetch_subdir_recursive(root.key, detail::decode_subdir_dep(root.value), ctx);
            break;
        case fetch::RootKind::path:
            detail::fetch_path_recursive(request.project_root / root.value, root.key, ctx);
            break;
        case fetch::RootKind::workspace: {
            if (!get_ws()) {
                auto section = root.is_dev ? "[dev-dependencies.workspace]" : "[dependencies.workspace]";
                throw std::runtime_error(std::format("{} used but no workspace root found", section));
            }
            auto member_path =
                manifest::system::resolve_workspace_member(*ws_root, *ws_m, root.key);
            if (!member_path)
                throw std::runtime_error(
                    std::format("workspace member '{}' not found in workspace", root.key));
            detail::fetch_path_recursive(*member_path, root.key, ctx);
            break;
        }
        }
    }

    if (saw_dev_root)
        fetch::mark_dev_dependencies(result.deps, dev_start);

    fetch::annotate_selected_features(result, request.manifest, request.include_dev);

    lock::system::save(result.lock_file, request.lock_path.string());
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
