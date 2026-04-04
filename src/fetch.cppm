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
    std::string commit;         // exact git commit hash
    std::filesystem::path path; // cached source path
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

FetchResult fetch_all(manifest::Manifest const& m, std::string_view lock_path) {
    FetchResult result;

    if (m.dependencies.empty())
        return result;

    result.lock_file = lock::load(lock_path);

    std::println("fetching dependencies...");
    std::set<std::string> visited;
    for (auto const& [key, version] : m.dependencies) {
        detail::fetch_recursive(key, version, result.lock_file, result.deps, visited);
    }

    lock::save(result.lock_file, lock_path);
    return result;
}

} // namespace fetch
