export module fetch;
import std;
import manifest;
import toolchain;
import lock;

export namespace fetch {

struct FetchedDep {
    std::string key;  // dep key (e.g. "github.com/user/repo")
    std::string name; // package name (repo name)
    std::string version;
    std::string commit;         // exact git commit hash (empty for path deps)
    std::filesystem::path path; // cached source path
    bool is_dev = false;        // dev-dependency (test-only)
    bool is_path = false;       // local path / workspace dep (no git, no lock)
};

namespace detail {

std::filesystem::path cache_dir() {
    auto home = std::getenv("HOME");
    if (!home)
        throw std::runtime_error("HOME environment variable not set");
    return std::filesystem::path{home} / ".exon" / "cache";
}

// "github.com/user/repo" → "https://github.com/user/repo.git"
// "/path/to/local/repo" → "file:///path/to/local/repo"
std::string to_git_url(std::string const& dep_key) {
    if (dep_key.starts_with("/")) {
        return std::format("file://{}", dep_key);
    }
    return std::format("https://{}.git", dep_key);
}

// "github.com/user/repo" → "repo"
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
    std::vector<FetchedDep>& result;
    std::set<std::string>& visited;          // git: "key@version"
    std::set<std::filesystem::path>& visited_paths; // path/workspace: absolute paths
};

void fetch_recursive(std::string const& dep_key, std::string const& version, lock::LockFile& lf,
                     std::vector<FetchedDep>& result, std::set<std::string>& visited);

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

    auto dep_m = manifest::load(dep_toml.string());

    // recurse into transitive deps FIRST (topological: deps come before this package)
    for (auto const& [sub_key, sub_version] : dep_m.dependencies)
        fetch_recursive(sub_key, sub_version, ctx.lf, ctx.result, ctx.visited);
    for (auto const& [sub_name, sub_rel] : dep_m.path_deps) {
        auto sub_abs = canon / sub_rel;
        fetch_path_recursive(sub_abs, sub_name, ctx);
    }
    if (!dep_m.workspace_deps.empty()) {
        auto ws_root = manifest::find_workspace_root(canon);
        if (!ws_root)
            throw std::runtime_error(std::format(
                "path dep '{}' uses [dependencies.workspace] but no workspace root found", name));
        auto ws_m = manifest::load((*ws_root / "exon.toml").string());
        for (auto const& ws_name : dep_m.workspace_deps) {
            auto member_path = manifest::resolve_workspace_member(*ws_root, ws_m, ws_name);
            if (!member_path)
                throw std::runtime_error(
                    std::format("workspace member '{}' not found in workspace", ws_name));
            fetch_path_recursive(*member_path, ws_name, ctx);
        }
    }

    // now add this dep (after its transitive deps)
    std::println("  path: {} -> {}", name, canon.string());
    FetchedDep d;
    d.key = name;
    d.name = dep_m.name.empty() ? name : dep_m.name;
    d.path = canon;
    d.is_path = true;
    ctx.result.push_back(std::move(d));
}

void fetch_recursive(std::string const& dep_key, std::string const& version, lock::LockFile& lf,
                     std::vector<FetchedDep>& result, std::set<std::string>& visited) {
    // prevent circular dependencies and duplicates
    auto visit_key = dep_key + "@" + version;
    if (visited.contains(visit_key))
        return;
    visited.insert(visit_key);

    auto cache_root = cache_dir();
    auto tag = to_git_tag(version);
    auto dep_cache = cache_root / dep_key / tag;

    auto locked = lf.find(dep_key, version);

    FetchedDep dep;
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
                               dep_cache.string());

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
        auto dep_manifest = manifest::load(dep_manifest_path.string());
        for (auto const& [sub_key, sub_version] : dep_manifest.dependencies) {
            fetch_recursive(sub_key, sub_version, lf, result, visited);
        }
    }

    // append dep at end (topological order: dependencies come first)
    result.push_back(std::move(dep));
}

} // namespace detail

struct FetchResult {
    std::vector<FetchedDep> deps;
    lock::LockFile lock_file;
};

FetchResult fetch_all(manifest::Manifest const& m, std::string_view lock_path,
                      bool include_dev = false) {
    FetchResult result;

    bool has_any = !m.dependencies.empty() || !m.path_deps.empty() || !m.workspace_deps.empty();
    bool has_dev = include_dev && (!m.dev_dependencies.empty() || !m.dev_path_deps.empty() ||
                                    !m.dev_workspace_deps.empty());
    if (!has_any && !has_dev)
        return result;

    result.lock_file = lock::load(lock_path);

    std::println("fetching dependencies...");
    std::set<std::string> visited;
    std::set<std::filesystem::path> visited_paths;
    detail::FetchContext ctx{result.lock_file, result.deps, visited, visited_paths};

    auto cwd = std::filesystem::current_path();

    // resolve workspace once (lazily) if needed
    std::optional<std::filesystem::path> ws_root;
    std::optional<manifest::Manifest> ws_m;
    auto get_ws = [&]() {
        if (!ws_root) {
            ws_root = manifest::find_workspace_root(cwd);
            if (ws_root)
                ws_m = manifest::load((*ws_root / "exon.toml").string());
        }
        return ws_root && ws_m;
    };

    for (auto const& [key, version] : m.dependencies)
        detail::fetch_recursive(key, version, result.lock_file, result.deps, visited);
    for (auto const& [name, rel] : m.path_deps)
        detail::fetch_path_recursive(cwd / rel, name, ctx);
    for (auto const& ws_name : m.workspace_deps) {
        if (!get_ws())
            throw std::runtime_error("[dependencies.workspace] used but no workspace root found");
        auto member_path = manifest::resolve_workspace_member(*ws_root, *ws_m, ws_name);
        if (!member_path)
            throw std::runtime_error(
                std::format("workspace member '{}' not found in workspace", ws_name));
        detail::fetch_path_recursive(*member_path, ws_name, ctx);
    }

    if (include_dev) {
        auto dev_start = result.deps.size();
        for (auto const& [key, version] : m.dev_dependencies)
            detail::fetch_recursive(key, version, result.lock_file, result.deps, visited);
        for (auto const& [name, rel] : m.dev_path_deps)
            detail::fetch_path_recursive(cwd / rel, name, ctx);
        for (auto const& ws_name : m.dev_workspace_deps) {
            if (!get_ws())
                throw std::runtime_error(
                    "[dev-dependencies.workspace] used but no workspace root found");
            auto member_path = manifest::resolve_workspace_member(*ws_root, *ws_m, ws_name);
            if (!member_path)
                throw std::runtime_error(
                    std::format("workspace member '{}' not found in workspace", ws_name));
            detail::fetch_path_recursive(*member_path, ws_name, ctx);
        }
        for (auto i = dev_start; i < result.deps.size(); ++i)
            result.deps[i].is_dev = true;
    }

    lock::save(result.lock_file, lock_path);
    return result;
}

} // namespace fetch
