export module fetch;
import std;
import manifest;
import toolchain;
import lock;

export namespace fetch {

struct FetchedDep {
    std::string key;        // dep key (e.g. "github.com/user/repo")
    std::string name;       // 패키지 이름 (repo name)
    std::string version;
    std::string commit;     // exact git commit hash
    std::filesystem::path path;  // 캐시된 소스 경로
};

namespace detail {

std::filesystem::path cache_dir() {
    auto home = std::getenv("HOME");
    if (!home) throw std::runtime_error("HOME environment variable not set");
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
    if (last_slash == std::string::npos) return dep_key;
    return dep_key.substr(last_slash + 1);
}

// version 이 "v" 접두사가 없으면 붙임
std::string to_git_tag(std::string const& version) {
    if (version.starts_with("v")) return version;
    return std::format("v{}", version);
}

std::string get_git_commit(std::filesystem::path const& repo_path) {
    // .git/HEAD → ref: refs/heads/main → .git/refs/heads/main → commit hash
    // detached HEAD (shallow clone) → .git/HEAD contains hash directly
    auto head_path = repo_path / ".git" / "HEAD";
    auto file = std::ifstream(head_path);
    if (!file) return "";

    std::string line;
    std::getline(file, line);

    if (line.starts_with("ref: ")) {
        auto ref = line.substr(5);
        auto ref_path = repo_path / ".git" / ref;
        auto ref_file = std::ifstream(ref_path);
        if (!ref_file) return "";
        std::getline(ref_file, line);
    }

    // trim whitespace
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) {
        line.pop_back();
    }
    return line;
}

} // namespace detail

FetchedDep fetch_one(std::string const& dep_key, std::string const& version,
                     lock::LockFile const& lf) {
    auto cache_root = detail::cache_dir();
    auto tag = detail::to_git_tag(version);
    auto dep_cache = cache_root / dep_key / tag;

    auto locked = lf.find(dep_key, version);

    // 캐시가 존재하고 lock 파일의 커밋과 일치하면 사용
    if (std::filesystem::exists(dep_cache) && locked) {
        auto current_commit = detail::get_git_commit(dep_cache);
        if (current_commit == locked->commit) {
            std::println("  locked: {} {} ({})", dep_key, tag, current_commit.substr(0, 8));
            return {
                .key = dep_key,
                .name = detail::extract_repo_name(dep_key),
                .version = version,
                .commit = current_commit,
                .path = dep_cache,
            };
        }
    }

    // 캐시가 있지만 lock이 없으면 기존 캐시 사용
    if (std::filesystem::exists(dep_cache)) {
        auto commit = detail::get_git_commit(dep_cache);
        std::println("  cached: {} {} ({})", dep_key, tag, commit.substr(0, 8));
        return {
            .key = dep_key,
            .name = detail::extract_repo_name(dep_key),
            .version = version,
            .commit = commit,
            .path = dep_cache,
        };
    }

    // 새로 clone
    std::filesystem::create_directories(dep_cache);

    auto git_url = detail::to_git_url(dep_key);
    auto cmd = std::format("git clone --depth 1 --branch {} {} {} 2>&1",
        tag, git_url, dep_cache.string());

    std::println("  fetching: {} {}...", dep_key, tag);
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::filesystem::remove_all(dep_cache);
        throw std::runtime_error(std::format("failed to fetch {} {}", dep_key, tag));
    }

    auto commit = detail::get_git_commit(dep_cache);
    return {
        .key = dep_key,
        .name = detail::extract_repo_name(dep_key),
        .version = version,
        .commit = commit,
        .path = dep_cache,
    };
}

struct FetchResult {
    std::vector<FetchedDep> deps;
    lock::LockFile lock_file;
};

FetchResult fetch_all(manifest::Manifest const& m, std::string_view lock_path) {
    FetchResult result;

    if (m.dependencies.empty()) return result;

    result.lock_file = lock::load(lock_path);

    std::println("fetching dependencies...");
    for (auto const& [key, version] : m.dependencies) {
        auto dep = fetch_one(key, version, result.lock_file);
        result.lock_file.add_or_update({
            .name = dep.key,
            .version = dep.version,
            .commit = dep.commit,
        });
        result.deps.push_back(std::move(dep));
    }

    lock::save(result.lock_file, lock_path);
    return result;
}

} // namespace fetch
