export module manifest.system;
import std;
import manifest;
import toolchain.system;

export namespace manifest::system {

namespace detail {

std::string read_file(std::filesystem::path const& path) {
    auto file = std::ifstream(path, std::ios::binary);
    if (!file)
        throw std::runtime_error(std::format("failed to read {}", path.string()));
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

} // namespace detail

Manifest load(std::string_view path) {
    return manifest::parse(detail::read_file(std::filesystem::path{path}));
}

// walk up from `start` looking for a workspace exon.toml; stop at HOME or filesystem root
std::optional<std::filesystem::path> find_workspace_root(std::filesystem::path start) {
    auto home = toolchain::system::home_dir();
    start = std::filesystem::weakly_canonical(start);
    while (true) {
        auto toml_path = start / "exon.toml";
        if (std::filesystem::exists(toml_path)) {
            try {
                auto m = load(toml_path.string());
                if (manifest::is_workspace(m))
                    return start;
            } catch (...) {
                // unreadable manifest: ignore and keep walking up
            }
        }
        if (!home.empty() && start == home)
            return std::nullopt;
        auto parent = start.parent_path();
        if (parent == start)
            return std::nullopt;
        start = parent;
    }
}

// resolve a workspace dep name (matches member's package.name) to absolute path
std::optional<std::filesystem::path> resolve_workspace_member(
    std::filesystem::path const& workspace_root, manifest::Manifest const& ws_manifest,
    std::string_view name) {
    for (auto const& member : ws_manifest.workspace_members) {
        auto member_path = workspace_root / member;
        auto member_toml = member_path / "exon.toml";
        if (!std::filesystem::exists(member_toml))
            continue;
        try {
            auto mm = load(member_toml.string());
            if (mm.name == name)
                return member_path;
        } catch (...) {
            // skip malformed
        }
    }
    return std::nullopt;
}

} // namespace manifest::system
