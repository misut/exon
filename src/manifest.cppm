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

// evaluate a cfg() predicate string against a platform
// supported forms:
//   cfg(os = "linux")
//   cfg(os = "linux", arch = "x86_64")   — AND
//   cfg(not(os = "windows"))
bool eval_predicate(std::string_view pred, toolchain::Platform const& target) {
    auto trim = [](std::string_view s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
            s.remove_prefix(1);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
            s.remove_suffix(1);
        return s;
    };
    pred = trim(pred);

    if (!pred.starts_with("cfg(") || !pred.ends_with(")"))
        throw std::runtime_error(std::format("invalid predicate: '{}'", pred));
    auto inner = trim(pred.substr(4, pred.size() - 5));

    // cfg(not(...))
    if (inner.starts_with("not(") && inner.ends_with(")")) {
        auto not_inner = inner.substr(4, inner.size() - 5);
        auto synthetic = std::format("cfg({})", not_inner);
        return !eval_predicate(synthetic, target);
    }

    // split inner by commas that are outside quotes, then check each key="value"
    auto eval_pair = [&](std::string_view pair) {
        pair = trim(pair);
        auto eq = pair.find('=');
        if (eq == std::string_view::npos)
            throw std::runtime_error(std::format("invalid predicate: expected '=' in '{}'", pred));
        auto key = trim(pair.substr(0, eq));
        auto val_part = trim(pair.substr(eq + 1));
        if (val_part.size() < 2 || val_part.front() != '"' || val_part.back() != '"')
            throw std::runtime_error(
                std::format("invalid predicate: expected quoted value in '{}'", pred));
        auto value = val_part.substr(1, val_part.size() - 2);

        if (key == "os")
            return target.os == value;
        if (key == "arch")
            return target.arch == value;
        throw std::runtime_error(
            std::format("invalid predicate: unknown key '{}' in '{}'", key, pred));
    };

    // split by commas (outside quotes)
    std::size_t start = 0;
    bool in_quotes = false;
    for (std::size_t i = 0; i <= inner.size(); ++i) {
        if (i < inner.size() && inner[i] == '"')
            in_quotes = !in_quotes;
        if (i == inner.size() || (inner[i] == ',' && !in_quotes)) {
            if (!eval_pair(inner.substr(start, i - start)))
                return false;
            start = i + 1;
        }
    }
    return true;
}

struct TargetSection {
    std::string predicate;
    std::map<std::string, std::string> dependencies;
    std::map<std::string, std::string> find_deps;
    std::map<std::string, std::string> path_deps;
    std::set<std::string> workspace_deps;
    std::map<std::string, VcpkgDep> vcpkg_deps;
    std::map<std::string, GitSubdirDep> subdir_deps;
    std::map<std::string, std::string> dev_dependencies;
    std::map<std::string, std::string> dev_find_deps;
    std::map<std::string, std::string> dev_path_deps;
    std::set<std::string> dev_workspace_deps;
    std::map<std::string, VcpkgDep> dev_vcpkg_deps;
    std::map<std::string, GitSubdirDep> dev_subdir_deps;
    std::map<std::string, std::string> defines;
    std::map<std::string, std::string> defines_debug;
    std::map<std::string, std::string> defines_release;
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
    std::vector<toolchain::Platform> platforms;          // [package].platforms (empty = all)
    std::vector<TargetSection> target_sections;          // [target.'cfg(...)'.* ]
    std::map<std::string, std::string> defines;         // [defines]
    std::map<std::string, std::string> defines_debug;   // [defines.debug]
    std::map<std::string, std::string> defines_release;  // [defines.release]
    std::vector<std::string> workspace_members; // workspace member paths
};

bool is_workspace(Manifest const& m) { return !m.workspace_members.empty(); }

// check if the given target platform is allowed by the manifest's platforms list
bool supports_platform(Manifest const& m, toolchain::Platform const& target) {
    if (m.platforms.empty())
        return true; // no platforms field → supports all
    for (auto const& p : m.platforms) {
        if (p.matches(target))
            return true;
    }
    return false;
}

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
        if (pkg.contains("platforms")) {
            auto const& arr = pkg.at("platforms").as_array();
            if (arr.empty())
                throw std::runtime_error(
                    "[package].platforms cannot be empty; remove the field to support all platforms");
            for (auto const& entry : arr) {
                if (!entry.is_table())
                    throw std::runtime_error(
                        "[package].platforms entries must be inline tables like { os = \"linux\" }");
                toolchain::Platform p;
                auto const& t = entry.as_table();
                if (t.contains("os")) {
                    p.os = t.at("os").as_string();
                    static constexpr auto known_os =
                        std::array{"linux", "macos", "windows"};
                    if (std::ranges::find(known_os, p.os) == known_os.end())
                        throw std::runtime_error(std::format(
                            "unknown os '{}' in [package].platforms; known: linux, macos, windows",
                            p.os));
                }
                if (t.contains("arch")) {
                    p.arch = t.at("arch").as_string();
                    static constexpr auto known_arch =
                        std::array{"x86_64", "aarch64"};
                    if (std::ranges::find(known_arch, p.arch) == known_arch.end())
                        throw std::runtime_error(std::format(
                            "unknown arch '{}' in [package].platforms; known: x86_64, aarch64",
                            p.arch));
                }
                if (p.os.empty() && p.arch.empty())
                    throw std::runtime_error(
                        "[package].platforms entry must have at least 'os' or 'arch'");
                m.platforms.push_back(std::move(p));
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

    // parse [target.'predicate'.dependencies/dev-dependencies/defines]
    if (table.contains("target")) {
        auto const& targets = table.at("target").as_table();
        for (auto const& [pred_key, pred_val] : targets) {
            if (!pred_val.is_table())
                continue;
            TargetSection ts;
            ts.predicate = pred_key;
            auto const& pred_table = pred_val.as_table();

            if (pred_table.contains("dependencies")) {
                parse_deps_section(pred_table.at("dependencies").as_table(),
                                   ts.dependencies, ts.find_deps, ts.path_deps,
                                   ts.workspace_deps, ts.vcpkg_deps, ts.subdir_deps);
            }
            if (pred_table.contains("dev-dependencies")) {
                parse_deps_section(pred_table.at("dev-dependencies").as_table(),
                                   ts.dev_dependencies, ts.dev_find_deps, ts.dev_path_deps,
                                   ts.dev_workspace_deps, ts.dev_vcpkg_deps, ts.dev_subdir_deps);
            }
            if (pred_table.contains("defines")) {
                auto const& defs = pred_table.at("defines").as_table();
                for (auto const& [key, val] : defs) {
                    if (val.is_string()) {
                        ts.defines.emplace(key, val.as_string());
                    } else if (val.is_table()) {
                        auto const& sub = val.as_table();
                        auto& tgt = (key == "debug") ? ts.defines_debug : ts.defines_release;
                        for (auto const& [k, v] : sub)
                            tgt.emplace(k, v.as_string());
                    }
                }
            }
            m.target_sections.push_back(std::move(ts));
        }
    }

    return m;
}

// resolve platform-conditional sections: evaluate predicates and merge matching
// sections into a copy of the manifest
Manifest resolve_for_platform(Manifest m, toolchain::Platform const& target) {
    for (auto const& ts : m.target_sections) {
        if (!eval_predicate(ts.predicate, target))
            continue;
        // merge deps
        for (auto const& [k, v] : ts.dependencies)
            m.dependencies.emplace(k, v);
        for (auto const& [k, v] : ts.find_deps)
            m.find_deps.emplace(k, v);
        for (auto const& [k, v] : ts.path_deps)
            m.path_deps.emplace(k, v);
        for (auto const& ws : ts.workspace_deps)
            m.workspace_deps.insert(ws);
        for (auto const& [k, v] : ts.vcpkg_deps)
            m.vcpkg_deps.emplace(k, v);
        for (auto const& [k, v] : ts.subdir_deps)
            m.subdir_deps.emplace(k, v);
        // merge dev-deps
        for (auto const& [k, v] : ts.dev_dependencies)
            m.dev_dependencies.emplace(k, v);
        for (auto const& [k, v] : ts.dev_find_deps)
            m.dev_find_deps.emplace(k, v);
        for (auto const& [k, v] : ts.dev_path_deps)
            m.dev_path_deps.emplace(k, v);
        for (auto const& ws : ts.dev_workspace_deps)
            m.dev_workspace_deps.insert(ws);
        for (auto const& [k, v] : ts.dev_vcpkg_deps)
            m.dev_vcpkg_deps.emplace(k, v);
        for (auto const& [k, v] : ts.dev_subdir_deps)
            m.dev_subdir_deps.emplace(k, v);
        // merge defines
        for (auto const& [k, v] : ts.defines)
            m.defines.emplace(k, v);
        for (auto const& [k, v] : ts.defines_debug)
            m.defines_debug.emplace(k, v);
        for (auto const& [k, v] : ts.defines_release)
            m.defines_release.emplace(k, v);
    }
    m.target_sections.clear(); // already resolved
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
