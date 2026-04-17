export module vcpkg.system;
import std;
import manifest;
import toolchain.system;
import vcpkg;

export namespace vcpkg::system {

// write vcpkg.json to disk (creates parent dir if needed)
void write_manifest(manifest::Manifest const& m, std::filesystem::path const& out_path) {
    std::filesystem::create_directories(out_path.parent_path());
    auto content = vcpkg::render_manifest(m);
    auto file = std::ofstream(out_path);
    if (!file)
        throw std::runtime_error(std::format("failed to write {}", out_path.string()));
    file << content;
}

namespace detail {

bool looks_like_vcpkg_root(std::filesystem::path const& p) {
    return std::filesystem::exists(p / "scripts" / "buildsystems" / "vcpkg.cmake");
}

} // namespace detail

bool looks_like_vcpkg_root(std::filesystem::path const& p) {
    return detail::looks_like_vcpkg_root(p);
}

// discover VCPKG_ROOT via env vars or common locations; returns empty path on failure
std::filesystem::path find_root() {
    // 1. explicit env vars
    for (auto const* var : {"VCPKG_ROOT", "VCPKG_INSTALLATION_ROOT"}) {
        auto const* val = std::getenv(var);
        if (val && *val) {
            auto p = std::filesystem::path{val};
            if (looks_like_vcpkg_root(p))
                return p;
        }
    }

    // 2. common install locations
    std::vector<std::filesystem::path> candidates = {
        "/opt/vcpkg",
        "/opt/homebrew/share/vcpkg",
        "/usr/local/share/vcpkg",
    };
    if (auto home = toolchain::system::home_dir(); !home.empty()) {
        candidates.push_back(home / "vcpkg");
        candidates.push_back(home / ".vcpkg");
    }
    for (auto const& c : candidates) {
        if (looks_like_vcpkg_root(c))
            return c;
    }

    return {};
}

// find root or throw with helpful message
std::filesystem::path require_root() {
    auto r = find_root();
    if (r.empty())
        throw std::runtime_error(
            "vcpkg not found. Set VCPKG_ROOT or install vcpkg at a standard location "
            "(e.g. /opt/vcpkg, ~/vcpkg, or via 'brew install vcpkg').");
    return r;
}

} // namespace vcpkg::system
