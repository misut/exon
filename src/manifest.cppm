export module manifest;
import std;
import toml;
import toolchain;

export namespace manifest {

struct VcpkgDep {
    std::string version;                // "" or "*" means baseline
    std::vector<std::string> features;  // optional vcpkg features
};

struct GitSubdirDep {
    std::string repo;     // "github.com/misut/txn"
    std::string version;  // "0.1.0"
    std::string subdir;   // "refl" (required, non-empty)
};

struct Manifest {
    std::string name;
    std::string version;
    std::string description;
    std::vector<std::string> authors;
    std::string license;
    std::string type = "bin"; // "bin" or "lib"
    int standard = 23;
    std::map<std::string, std::string> dependencies;
    std::map<std::string, std::string> dev_dependencies; // [dev-dependencies]
    std::map<std::string, std::string> find_deps;        // [dependencies.find]
    std::map<std::string, std::string> dev_find_deps;    // [dev-dependencies.find]
    std::map<std::string, std::string> path_deps;        // [dependencies.path]
    std::map<std::string, std::string> dev_path_deps;    // [dev-dependencies.path]
    std::set<std::string> workspace_deps;                // [dependencies.workspace]
    std::set<std::string> dev_workspace_deps;            // [dev-dependencies.workspace]
    std::map<std::string, VcpkgDep> vcpkg_deps;          // [dependencies.vcpkg]
    std::map<std::string, VcpkgDep> dev_vcpkg_deps;      // [dev-dependencies.vcpkg]
    std::map<std::string, GitSubdirDep> subdir_deps;     // [dependencies] inline-table form
    std::map<std::string, GitSubdirDep> dev_subdir_deps; // [dev-dependencies] inline-table form
    std::map<std::string, std::string> defines;         // [defines]
    std::map<std::string, std::string> defines_debug;   // [defines.debug]
    std::map<std::string, std::string> defines_release;  // [defines.release]
    std::vector<std::string> workspace_members; // workspace member paths
};

bool is_workspace(Manifest const& m) { return !m.workspace_members.empty(); }

Manifest from_toml(toml::Table const& table) {
    Manifest m;

    if (table.contains("package")) {
        auto const& pkg = table.at("package").as_table();

        if (pkg.contains("name"))
            m.name = pkg.at("name").as_string();
        if (pkg.contains("version"))
            m.version = pkg.at("version").as_string();
        if (pkg.contains("description"))
            m.description = pkg.at("description").as_string();
        if (pkg.contains("license"))
            m.license = pkg.at("license").as_string();
        if (pkg.contains("type"))
            m.type = pkg.at("type").as_string();
        if (pkg.contains("standard"))
            m.standard = static_cast<int>(pkg.at("standard").as_integer());
        if (pkg.contains("authors")) {
            for (auto const& a : pkg.at("authors").as_array()) {
                m.authors.push_back(a.as_string());
            }
        }
    }

    if (table.contains("workspace")) {
        auto const& ws = table.at("workspace").as_table();
        if (ws.contains("members")) {
            for (auto const& member : ws.at("members").as_array()) {
                m.workspace_members.push_back(member.as_string());
            }
        }
    }

    auto parse_deps_section = [](toml::Table const& deps,
                                  std::map<std::string, std::string>& string_deps,
                                  std::map<std::string, std::string>& find_deps,
                                  std::map<std::string, std::string>& path_deps,
                                  std::set<std::string>& workspace_deps,
                                  std::map<std::string, VcpkgDep>& vcpkg_deps,
                                  std::map<std::string, GitSubdirDep>& subdir_deps) {
        for (auto const& [key, val] : deps) {
            if (val.is_string()) {
                string_deps.emplace(key, val.as_string());
            } else if (val.is_table() && key == "find") {
                for (auto const& [k, v] : val.as_table()) {
                    if (v.is_string())
                        find_deps.emplace(k, v.as_string());
                }
            } else if (val.is_table() && key == "path") {
                for (auto const& [k, v] : val.as_table()) {
                    if (v.is_string())
                        path_deps.emplace(k, v.as_string());
                }
            } else if (val.is_table() && key == "workspace") {
                for (auto const& [k, v] : val.as_table()) {
                    if (v.is_bool() && v.as_bool())
                        workspace_deps.insert(k);
                }
            } else if (val.is_table() && key == "vcpkg") {
                for (auto const& [k, v] : val.as_table()) {
                    VcpkgDep dep;
                    if (v.is_string()) {
                        dep.version = v.as_string();
                    } else if (v.is_table()) {
                        auto const& t = v.as_table();
                        if (t.contains("version") && t.at("version").is_string())
                            dep.version = t.at("version").as_string();
                        if (t.contains("features") && t.at("features").is_array()) {
                            for (auto const& f : t.at("features").as_array()) {
                                if (f.is_string())
                                    dep.features.push_back(f.as_string());
                            }
                        }
                    } else {
                        continue;
                    }
                    vcpkg_deps.emplace(k, std::move(dep));
                }
            } else if (val.is_table()) {
                // inline-table git dep: name = { git = "...", version = "...", subdir = "..." }
                auto const& t = val.as_table();
                GitSubdirDep dep;
                if (!t.contains("git") || !t.at("git").is_string())
                    throw std::runtime_error(std::format(
                        "dependency '{}': missing required 'git' field", key));
                dep.repo = t.at("git").as_string();
                if (!t.contains("version") || !t.at("version").is_string())
                    throw std::runtime_error(std::format(
                        "dependency '{}': missing required 'version' field", key));
                dep.version = t.at("version").as_string();
                if (!t.contains("subdir") || !t.at("subdir").is_string())
                    throw std::runtime_error(std::format(
                        "dependency '{}': missing required 'subdir' field "
                        "(use string form for no-subdir git deps)", key));
                dep.subdir = t.at("subdir").as_string();
                if (dep.subdir.empty())
                    throw std::runtime_error(std::format(
                        "dependency '{}': 'subdir' must be non-empty", key));
                subdir_deps.emplace(key, std::move(dep));
            }
        }
    };

    if (table.contains("dependencies")) {
        parse_deps_section(table.at("dependencies").as_table(), m.dependencies, m.find_deps,
                           m.path_deps, m.workspace_deps, m.vcpkg_deps, m.subdir_deps);
    }

    if (table.contains("dev-dependencies")) {
        parse_deps_section(table.at("dev-dependencies").as_table(), m.dev_dependencies,
                           m.dev_find_deps, m.dev_path_deps, m.dev_workspace_deps, m.dev_vcpkg_deps,
                           m.dev_subdir_deps);
    }

    if (table.contains("defines")) {
        auto const& defs = table.at("defines").as_table();
        for (auto const& [key, val] : defs) {
            if (val.is_string()) {
                m.defines.emplace(key, val.as_string());
            } else if (val.is_table()) {
                auto const& sub = val.as_table();
                auto& target = (key == "debug") ? m.defines_debug : m.defines_release;
                for (auto const& [k, v] : sub) {
                    target.emplace(k, v.as_string());
                }
            }
        }
    }

    return m;
}

Manifest load(std::string_view path) {
    auto table = toml::parse_file(path);
    return from_toml(table);
}

// walk up from `start` looking for a workspace exon.toml; stop at HOME or filesystem root
std::optional<std::filesystem::path> find_workspace_root(std::filesystem::path start) {
    auto home = toolchain::home_dir();
    start = std::filesystem::weakly_canonical(start);
    while (true) {
        auto toml_path = start / "exon.toml";
        if (std::filesystem::exists(toml_path)) {
            try {
                auto m = load(toml_path.string());
                if (is_workspace(m))
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
    std::filesystem::path const& workspace_root, Manifest const& ws_manifest,
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

} // namespace manifest
