export module fetch;
import std;
import manifest;
import toolchain;

export namespace fetch {

struct FetchedDep {
    std::string name;       // 패키지 이름 (repo name)
    std::string version;
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

} // namespace detail

FetchedDep fetch_one(std::string const& dep_key, std::string const& version) {
    auto cache_root = detail::cache_dir();
    auto tag = detail::to_git_tag(version);
    auto dep_cache = cache_root / dep_key / tag;

    if (std::filesystem::exists(dep_cache)) {
        std::println("  cached: {} {}", dep_key, tag);
        return {
            .name = detail::extract_repo_name(dep_key),
            .version = version,
            .path = dep_cache,
        };
    }

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

    return {
        .name = detail::extract_repo_name(dep_key),
        .version = version,
        .path = dep_cache,
    };
}

std::vector<FetchedDep> fetch_all(manifest::Manifest const& m) {
    if (m.dependencies.empty()) return {};

    std::println("fetching dependencies...");
    std::vector<FetchedDep> deps;
    for (auto const& [key, version] : m.dependencies) {
        deps.push_back(fetch_one(key, version));
    }
    return deps;
}

} // namespace fetch
