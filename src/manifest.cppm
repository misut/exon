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

struct GitFeatureDep {
    std::string version;
    std::vector<std::string> features;
    bool default_features = true;
};

struct CmakeDep {
    std::string git;         // Git repository URL
    std::string tag;         // Git tag, branch, or commit hash
    std::string targets;     // Whitespace-separated CMake target names to link
    std::map<std::string, std::string> options; // set(KEY VALUE) before FetchContent
    bool shallow = true;
};

// evaluate a cfg() predicate string against a platform
// supported forms:
//   cfg(os = "linux")
//   cfg(os = "linux", arch = "x86_64")   — AND
//   cfg(not(os = "windows"))
// shared predicate parsing utilities

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.remove_suffix(1);
    return s;
}

struct PredicateField { std::string_view key; std::string_view value; };

struct ParsedPredicate {
    bool negated = false;
    std::vector<PredicateField> fields;
};

ParsedPredicate parse_predicate(std::string_view pred) {
    pred = trim(pred);
    if (!pred.starts_with("cfg(") || !pred.ends_with(")"))
        return {};
    auto inner = trim(pred.substr(4, pred.size() - 5));

    ParsedPredicate result;
    if (inner.starts_with("not(") && inner.ends_with(")")) {
        result.negated = true;
        inner = trim(inner.substr(4, inner.size() - 5));
    }

    std::size_t start = 0;
    bool in_quotes = false;
    for (std::size_t i = 0; i <= inner.size(); ++i) {
        if (i < inner.size() && inner[i] == '"')
            in_quotes = !in_quotes;
        if (i == inner.size() || (inner[i] == ',' && !in_quotes)) {
            auto pair = trim(inner.substr(start, i - start));
            auto eq = pair.find('=');
            if (eq != std::string_view::npos) {
                auto key = trim(pair.substr(0, eq));
                auto val_part = trim(pair.substr(eq + 1));
                if (val_part.size() >= 2 && val_part.front() == '"' && val_part.back() == '"')
                    result.fields.push_back({key, val_part.substr(1, val_part.size() - 2)});
            }
            start = i + 1;
        }
    }
    return result;
}

bool eval_predicate(std::string_view pred, toolchain::Platform const& target) {
    auto parsed = parse_predicate(pred);
    if (parsed.fields.empty())
        throw std::runtime_error(std::format("invalid predicate: '{}'", pred));

    bool match = true;
    for (auto const& f : parsed.fields) {
        if (f.key == "os" && toolchain::platform_os_name(target) != f.value) {
            match = false;
            break;
        }
        if (f.key == "arch" && toolchain::platform_arch_name(target) != f.value) {
            match = false;
            break;
        }
    }
    return parsed.negated ? !match : match;
}

struct Build {
    std::vector<std::string> cxxflags;
    std::vector<std::string> ldflags;
};

struct WorkspacePackageDefaults {
    std::string version;
    std::vector<std::string> authors;
    std::string license;
    std::string type;
    int standard = 23;
    std::vector<toolchain::Platform> platforms;
    std::string build_system;
    bool has_version = false;
    bool has_authors = false;
    bool has_license = false;
    bool has_type = false;
    bool has_standard = false;
    bool has_platforms = false;
    bool has_build_system = false;
};

struct WorkspaceDefaults {
    WorkspacePackageDefaults package;
    Build build;
    Build build_debug;
    Build build_release;
};

struct TargetSection {
    std::string predicate;
    std::map<std::string, std::string> dependencies;
    std::map<std::string, std::string> find_deps;
    std::map<std::string, std::string> path_deps;
    std::set<std::string> workspace_deps;
    std::map<std::string, VcpkgDep> vcpkg_deps;
    std::map<std::string, GitSubdirDep> subdir_deps;
    std::map<std::string, GitFeatureDep> featured_deps;
    std::map<std::string, std::string> dev_dependencies;
    std::map<std::string, std::string> dev_find_deps;
    std::map<std::string, std::string> dev_path_deps;
    std::set<std::string> dev_workspace_deps;
    std::map<std::string, VcpkgDep> dev_vcpkg_deps;
    std::map<std::string, GitSubdirDep> dev_subdir_deps;
    std::map<std::string, GitFeatureDep> dev_featured_deps;
    std::map<std::string, CmakeDep> cmake_deps;             // [dependencies.cmake]
    std::map<std::string, CmakeDep> dev_cmake_deps;          // [dev-dependencies.cmake]
    std::map<std::string, std::string> defines;
    std::map<std::string, std::string> defines_debug;
    std::map<std::string, std::string> defines_release;
    Build build;          // [target.'cfg(...)'.build]
    Build build_debug;    // [target.'cfg(...)'.build.debug]
    Build build_release;  // [target.'cfg(...)'.build.release]
};

struct Manifest {
    std::string name;
    std::string version;
    std::string description;
    std::vector<std::string> authors;
    std::string license;
    std::string type = "bin"; // "bin" or "lib"
    int standard = 23;
    bool has_version = false;
    bool has_authors = false;
    bool has_license = false;
    bool has_type = false;
    bool has_standard = false;
    bool has_platforms = false;
    bool has_build_system = false;
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
    std::map<std::string, GitFeatureDep> featured_deps;  // [dependencies] with features
    std::map<std::string, GitFeatureDep> dev_featured_deps; // [dev-dependencies] with features
    std::map<std::string, CmakeDep> cmake_deps;              // [dependencies.cmake]
    std::map<std::string, CmakeDep> dev_cmake_deps;           // [dev-dependencies.cmake]
    std::map<std::string, std::vector<std::string>> features; // [features]
    std::vector<toolchain::Platform> platforms;          // [package].platforms (empty = all)
    std::vector<TargetSection> target_sections;          // [target.'cfg(...)'.* ]
    std::map<std::string, std::string> defines;         // [defines]
    std::map<std::string, std::string> defines_debug;   // [defines.debug]
    std::map<std::string, std::string> defines_release;  // [defines.release]
    Build build;                                         // [build]
    Build build_debug;                                   // [build.debug]
    Build build_release;                                 // [build.release]
    bool has_workspace = false;
    std::vector<std::string> workspace_members; // workspace member paths
    std::optional<WorkspaceDefaults> workspace_defaults;
    std::string build_system;                    // [package] build-system = "exon"|"cmake"
    bool sync_cmake_in_root = true;              // [sync] cmake-in-root = true
};

bool is_workspace(Manifest const& m) { return m.has_workspace; }

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

    auto parse_platforms = [](toml::Array const& arr,
                              std::vector<toolchain::Platform>& out) {
        if (arr.empty())
            throw std::runtime_error(
                "[package].platforms cannot be empty; remove the field to support all platforms");
        for (auto const& entry : arr) {
            if (!entry.is_table())
                throw std::runtime_error(
                    "[package].platforms entries must be inline tables like { os = \"linux\" }");
            toolchain::Platform p{};
            auto const& t = entry.as_table();
            if (t.contains("os")) {
                auto const os = toolchain::parse_os(t.at("os").as_string());
                if (os == toolchain::OS::Unknown)
                    throw std::runtime_error(std::format(
                        "unknown os '{}' in [package].platforms; known: linux, macos, windows, wasi, android",
                        t.at("os").as_string()));
                p.os = os;
            }
            if (t.contains("arch")) {
                auto const arch = toolchain::parse_arch(t.at("arch").as_string());
                if (arch == toolchain::Arch::Unknown)
                    throw std::runtime_error(std::format(
                        "unknown arch '{}' in [package].platforms; known: x86_64, aarch64, wasm32",
                        t.at("arch").as_string()));
                p.arch = arch;
            }
            if (!toolchain::platform_has_os(p) && !toolchain::platform_has_arch(p))
                throw std::runtime_error(
                    "[package].platforms entry must have at least 'os' or 'arch'");
            out.push_back(std::move(p));
        }
    };

    auto parse_build_table = [](toml::Table const& bt, Build& out) {
        if (bt.contains("cxxflags")) {
            auto const& arr = bt.at("cxxflags").as_array();
            for (auto const& v : arr)
                out.cxxflags.push_back(v.as_string());
        }
        if (bt.contains("ldflags")) {
            auto const& arr = bt.at("ldflags").as_array();
            for (auto const& v : arr)
                out.ldflags.push_back(v.as_string());
        }
    };

    if (table.contains("package")) {
        auto const& pkg = table.at("package").as_table();

        if (pkg.contains("name"))
            m.name = pkg.at("name").as_string();
        if (pkg.contains("version")) {
            m.version = pkg.at("version").as_string();
            m.has_version = true;
        }
        if (pkg.contains("description"))
            m.description = pkg.at("description").as_string();
        if (pkg.contains("license")) {
            m.license = pkg.at("license").as_string();
            m.has_license = true;
        }
        if (pkg.contains("type")) {
            m.type = pkg.at("type").as_string();
            m.has_type = true;
        }
        if (pkg.contains("build-system")) {
            m.build_system = pkg.at("build-system").as_string();
            if (m.build_system != "exon" && m.build_system != "cmake")
                throw std::runtime_error(
                    std::format("build-system must be \"exon\" or \"cmake\", got \"{}\"",
                                m.build_system));
            m.has_build_system = true;
        }
        if (pkg.contains("standard")) {
            m.standard = static_cast<int>(pkg.at("standard").as_integer());
            m.has_standard = true;
        }
        if (pkg.contains("authors")) {
            for (auto const& a : pkg.at("authors").as_array()) {
                m.authors.push_back(a.as_string());
            }
            m.has_authors = true;
        }
        if (pkg.contains("platforms")) {
            parse_platforms(pkg.at("platforms").as_array(), m.platforms);
            m.has_platforms = true;
        }
    }

    if (table.contains("workspace")) {
        m.has_workspace = true;
        auto const& ws = table.at("workspace").as_table();
        if (ws.contains("members")) {
            for (auto const& member : ws.at("members").as_array()) {
                m.workspace_members.push_back(member.as_string());
            }
        }
        if (ws.contains("package") && ws.at("package").is_table()) {
            WorkspaceDefaults defaults;
            auto const& pkg = ws.at("package").as_table();
            if (pkg.contains("version")) {
                defaults.package.version = pkg.at("version").as_string();
                defaults.package.has_version = true;
            }
            if (pkg.contains("authors")) {
                for (auto const& a : pkg.at("authors").as_array())
                    defaults.package.authors.push_back(a.as_string());
                defaults.package.has_authors = true;
            }
            if (pkg.contains("license")) {
                defaults.package.license = pkg.at("license").as_string();
                defaults.package.has_license = true;
            }
            if (pkg.contains("type")) {
                defaults.package.type = pkg.at("type").as_string();
                defaults.package.has_type = true;
            }
            if (pkg.contains("standard")) {
                defaults.package.standard = static_cast<int>(pkg.at("standard").as_integer());
                defaults.package.has_standard = true;
            }
            if (pkg.contains("platforms")) {
                parse_platforms(pkg.at("platforms").as_array(), defaults.package.platforms);
                defaults.package.has_platforms = true;
            }
            if (pkg.contains("build-system")) {
                defaults.package.build_system = pkg.at("build-system").as_string();
                if (defaults.package.build_system != "exon" &&
                    defaults.package.build_system != "cmake") {
                    throw std::runtime_error(std::format(
                        "workspace build-system must be \"exon\" or \"cmake\", got \"{}\"",
                        defaults.package.build_system));
                }
                defaults.package.has_build_system = true;
            }
            m.workspace_defaults = std::move(defaults);
        }
        if (ws.contains("build") && ws.at("build").is_table()) {
            if (!m.workspace_defaults)
                m.workspace_defaults.emplace();
            auto const& bt = ws.at("build").as_table();
            parse_build_table(bt, m.workspace_defaults->build);
            if (bt.contains("debug") && bt.at("debug").is_table())
                parse_build_table(bt.at("debug").as_table(), m.workspace_defaults->build_debug);
            if (bt.contains("release") && bt.at("release").is_table())
                parse_build_table(bt.at("release").as_table(), m.workspace_defaults->build_release);
        }
    }

    auto parse_deps_section = [](toml::Table const& deps,
                                  std::map<std::string, std::string>& string_deps,
                                  std::map<std::string, std::string>& find_deps,
                                  std::map<std::string, std::string>& path_deps,
                                  std::set<std::string>& workspace_deps,
                                  std::map<std::string, VcpkgDep>& vcpkg_deps,
                                  std::map<std::string, GitSubdirDep>& subdir_deps,
                                  std::map<std::string, GitFeatureDep>& featured_deps,
                                  std::map<std::string, CmakeDep>& cmake_deps) {
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
            } else if (val.is_table() && key == "cmake") {
                for (auto const& [k, v] : val.as_table()) {
                    if (!v.is_table()) continue;
                    auto const& t = v.as_table();
                    CmakeDep dep;
                    if (!t.contains("git") || !t.at("git").is_string())
                        throw std::runtime_error(std::format(
                            "cmake dependency '{}': missing required 'git' field", k));
                    dep.git = t.at("git").as_string();
                    if (!t.contains("tag") || !t.at("tag").is_string())
                        throw std::runtime_error(std::format(
                            "cmake dependency '{}': missing required 'tag' field", k));
                    dep.tag = t.at("tag").as_string();
                    if (!t.contains("targets") || !t.at("targets").is_string())
                        throw std::runtime_error(std::format(
                            "cmake dependency '{}': missing required 'targets' field", k));
                    dep.targets = t.at("targets").as_string();
                    if (t.contains("shallow") && t.at("shallow").is_bool())
                        dep.shallow = t.at("shallow").as_bool();
                    if (t.contains("options") && t.at("options").is_table()) {
                        for (auto const& [ok, ov] : t.at("options").as_table()) {
                            if (ov.is_string())
                                dep.options.emplace(ok, ov.as_string());
                        }
                    }
                    cmake_deps.emplace(k, std::move(dep));
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
            } else if (val.is_table() && !val.as_table().contains("git") &&
                       !val.as_table().contains("subdir")) {
                // inline-table git dep with features: key = { version = "...", features = [...] }
                auto const& t = val.as_table();
                if (!t.contains("version") || !t.at("version").is_string())
                    throw std::runtime_error(std::format(
                        "dependency '{}': missing required 'version' field", key));
                GitFeatureDep dep;
                dep.version = t.at("version").as_string();
                if (t.contains("features") && t.at("features").is_array()) {
                    for (auto const& f : t.at("features").as_array()) {
                        if (f.is_string())
                            dep.features.push_back(f.as_string());
                    }
                }
                if (t.contains("default-features") && t.at("default-features").is_bool())
                    dep.default_features = t.at("default-features").as_bool();
                featured_deps.emplace(key, std::move(dep));
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
                           m.path_deps, m.workspace_deps, m.vcpkg_deps, m.subdir_deps,
                           m.featured_deps, m.cmake_deps);
    }

    if (table.contains("dev-dependencies")) {
        parse_deps_section(table.at("dev-dependencies").as_table(), m.dev_dependencies,
                           m.dev_find_deps, m.dev_path_deps, m.dev_workspace_deps, m.dev_vcpkg_deps,
                           m.dev_subdir_deps, m.dev_featured_deps, m.dev_cmake_deps);
    }

    if (table.contains("features")) {
        auto const& feat = table.at("features").as_table();
        for (auto const& [key, val] : feat) {
            if (!val.is_array())
                throw std::runtime_error(std::format(
                    "[features].{} must be an array of strings", key));
            std::vector<std::string> entries;
            for (auto const& f : val.as_array()) {
                if (f.is_string())
                    entries.push_back(f.as_string());
            }
            m.features.emplace(key, std::move(entries));
        }
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

    if (table.contains("build")) {
        auto const& bt = table.at("build").as_table();
        parse_build_table(bt, m.build);
        if (bt.contains("debug") && bt.at("debug").is_table())
            parse_build_table(bt.at("debug").as_table(), m.build_debug);
        if (bt.contains("release") && bt.at("release").is_table())
            parse_build_table(bt.at("release").as_table(), m.build_release);
    }

    if (table.contains("sync") && table.at("sync").is_table()) {
        auto const& sync = table.at("sync").as_table();
        if (sync.contains("cmake-in-root") && sync.at("cmake-in-root").is_bool())
            m.sync_cmake_in_root = sync.at("cmake-in-root").as_bool();
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
                                   ts.workspace_deps, ts.vcpkg_deps, ts.subdir_deps,
                                   ts.featured_deps, ts.cmake_deps);
            }
            if (pred_table.contains("dev-dependencies")) {
                parse_deps_section(pred_table.at("dev-dependencies").as_table(),
                                   ts.dev_dependencies, ts.dev_find_deps, ts.dev_path_deps,
                                   ts.dev_workspace_deps, ts.dev_vcpkg_deps, ts.dev_subdir_deps,
                                   ts.dev_featured_deps, ts.dev_cmake_deps);
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
            if (pred_table.contains("build")) {
                auto const& bt = pred_table.at("build").as_table();
                parse_build_table(bt, ts.build);
                if (bt.contains("debug") && bt.at("debug").is_table())
                    parse_build_table(bt.at("debug").as_table(), ts.build_debug);
                if (bt.contains("release") && bt.at("release").is_table())
                    parse_build_table(bt.at("release").as_table(), ts.build_release);
            }
            m.target_sections.push_back(std::move(ts));
        }
    }

    return m;
}

void merge_section_into(Manifest& m, TargetSection const& ts) {
    for (auto const& [k, v] : ts.dependencies) m.dependencies.emplace(k, v);
    for (auto const& [k, v] : ts.find_deps) m.find_deps.emplace(k, v);
    for (auto const& [k, v] : ts.path_deps) m.path_deps.emplace(k, v);
    for (auto const& ws : ts.workspace_deps) m.workspace_deps.insert(ws);
    for (auto const& [k, v] : ts.vcpkg_deps) m.vcpkg_deps.emplace(k, v);
    for (auto const& [k, v] : ts.subdir_deps) m.subdir_deps.emplace(k, v);
    for (auto const& [k, v] : ts.featured_deps) m.featured_deps.emplace(k, v);
    for (auto const& [k, v] : ts.cmake_deps) m.cmake_deps.emplace(k, v);
    for (auto const& [k, v] : ts.dev_dependencies) m.dev_dependencies.emplace(k, v);
    for (auto const& [k, v] : ts.dev_find_deps) m.dev_find_deps.emplace(k, v);
    for (auto const& [k, v] : ts.dev_path_deps) m.dev_path_deps.emplace(k, v);
    for (auto const& ws : ts.dev_workspace_deps) m.dev_workspace_deps.insert(ws);
    for (auto const& [k, v] : ts.dev_vcpkg_deps) m.dev_vcpkg_deps.emplace(k, v);
    for (auto const& [k, v] : ts.dev_subdir_deps) m.dev_subdir_deps.emplace(k, v);
    for (auto const& [k, v] : ts.dev_featured_deps) m.dev_featured_deps.emplace(k, v);
    for (auto const& [k, v] : ts.dev_cmake_deps) m.dev_cmake_deps.emplace(k, v);
    for (auto const& [k, v] : ts.defines) m.defines.emplace(k, v);
    for (auto const& [k, v] : ts.defines_debug) m.defines_debug.emplace(k, v);
    for (auto const& [k, v] : ts.defines_release) m.defines_release.emplace(k, v);
    // Build flags append (not emplace) so multiple matching sections can stack.
    auto append = [](auto& dst, auto const& src) {
        dst.insert(dst.end(), src.begin(), src.end());
    };
    append(m.build.cxxflags,         ts.build.cxxflags);
    append(m.build.ldflags,          ts.build.ldflags);
    append(m.build_debug.cxxflags,   ts.build_debug.cxxflags);
    append(m.build_debug.ldflags,    ts.build_debug.ldflags);
    append(m.build_release.cxxflags, ts.build_release.cxxflags);
    append(m.build_release.ldflags,  ts.build_release.ldflags);
}

Manifest resolve_for_platform(Manifest m, toolchain::Platform const& target) {
    for (auto const& ts : m.target_sections) {
        if (eval_predicate(ts.predicate, target))
            merge_section_into(m, ts);
    }
    m.target_sections.clear();
    return m;
}

Manifest resolve_all_targets(Manifest m) {
    for (auto const& ts : m.target_sections)
        merge_section_into(m, ts);
    return m;
}

void prepend_build_flags(Build& dst, Build const& defaults) {
    auto prepend = [](std::vector<std::string>& values,
                      std::vector<std::string> const& defaults_values) {
        if (defaults_values.empty())
            return;
        auto merged = defaults_values;
        merged.insert(merged.end(), values.begin(), values.end());
        values = std::move(merged);
    };
    prepend(dst.cxxflags, defaults.cxxflags);
    prepend(dst.ldflags, defaults.ldflags);
}

Manifest apply_workspace_defaults(Manifest member, Manifest const& workspace) {
    if (!workspace.workspace_defaults)
        return member;

    auto const& defaults = *workspace.workspace_defaults;
    if (!member.has_version && defaults.package.has_version)
        member.version = defaults.package.version;
    if (!member.has_authors && defaults.package.has_authors)
        member.authors = defaults.package.authors;
    if (!member.has_license && defaults.package.has_license)
        member.license = defaults.package.license;
    if (!member.has_type && defaults.package.has_type)
        member.type = defaults.package.type;
    if (!member.has_standard && defaults.package.has_standard)
        member.standard = defaults.package.standard;
    if (!member.has_platforms && defaults.package.has_platforms)
        member.platforms = defaults.package.platforms;
    if (!member.has_build_system && defaults.package.has_build_system)
        member.build_system = defaults.package.build_system;

    prepend_build_flags(member.build, defaults.build);
    prepend_build_flags(member.build_debug, defaults.build_debug);
    prepend_build_flags(member.build_release, defaults.build_release);
    return member;
}

bool dependency_exists(Manifest const& m, std::string const& name) {
    return m.dependencies.contains(name) || m.dev_dependencies.contains(name) ||
           m.path_deps.contains(name) || m.dev_path_deps.contains(name) ||
           m.workspace_deps.contains(name) || m.dev_workspace_deps.contains(name) ||
           m.vcpkg_deps.contains(name) || m.dev_vcpkg_deps.contains(name) ||
           m.subdir_deps.contains(name) || m.dev_subdir_deps.contains(name) ||
           m.featured_deps.contains(name) || m.dev_featured_deps.contains(name) ||
           m.cmake_deps.contains(name) || m.dev_cmake_deps.contains(name);
}

// insert `line` into [section] block; create section at EOF if missing
void insert_into_section(std::string& content, std::string const& section,
                         std::string const& line) {
    auto header = std::format("[{}]", section);
    auto pos = content.find(header);
    if (pos == std::string::npos) {
        if (!content.empty() && content.back() != '\n')
            content += '\n';
        content += std::format("\n{}\n{}", header, line);
        return;
    }
    // verify this is an exact section header (preceded by start/newline, followed by newline)
    // simple check: header should start at column 0
    bool at_line_start = (pos == 0) || (content[pos - 1] == '\n');
    if (!at_line_start) {
        // header substring found mid-line; append as new section
        if (!content.empty() && content.back() != '\n')
            content += '\n';
        content += std::format("\n{}\n{}", header, line);
        return;
    }
    auto insert_pos = content.find('\n', pos);
    if (insert_pos != std::string::npos)
        content.insert(insert_pos + 1, line);
    else
        content += std::format("\n{}", line);
}

bool remove_dependency_entry(std::string& content, std::string_view name) {
    std::vector<std::string> candidates = {
        std::format("\"{}\"", name),
        std::format("{} =", name),
    };

    for (auto const& search : candidates) {
        auto pos = content.find(search);
        while (pos != std::string::npos) {
            auto line_start = content.rfind('\n', pos);
            line_start = (line_start == std::string::npos) ? 0 : line_start + 1;
            auto prefix = content.substr(line_start, pos - line_start);
            auto only_ws = std::ranges::all_of(prefix, [](char c) {
                return c == ' ' || c == '\t';
            });
            if (only_ws) {
                auto line_end = content.find('\n', pos);
                if (line_end != std::string::npos)
                    ++line_end;
                else
                    line_end = content.size();
                content.erase(line_start, line_end - line_start);
                return true;
            }
            pos = content.find(search, pos + 1);
        }
    }

    return false;
}

void cleanup_empty_subsections(std::string& content) {
    auto cleaned = std::string{};
    cleaned.reserve(content.size());

    std::size_t i = 0;
    while (i < content.size()) {
        auto line_end = content.find('\n', i);
        auto next = (line_end == std::string::npos) ? content.size() : line_end + 1;
        auto line = std::string_view{content}.substr(i, next - i);
        auto trimmed = line;
        while (!trimmed.empty() &&
               (trimmed.back() == '\n' || trimmed.back() == '\r' ||
                trimmed.back() == ' ' || trimmed.back() == '\t')) {
            trimmed.remove_suffix(1);
        }
        while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t'))
            trimmed.remove_prefix(1);

        bool is_subsection = trimmed.starts_with("[") &&
                             trimmed.ends_with("]") &&
                             trimmed.find('.') != std::string_view::npos;
        if (is_subsection) {
            std::size_t j = next;
            bool empty_section = true;
            while (j < content.size()) {
                auto peek_end = content.find('\n', j);
                auto peek_next =
                    (peek_end == std::string::npos) ? content.size() : peek_end + 1;
                auto peek = std::string_view{content}.substr(j, peek_next - j);
                auto peek_trimmed = peek;
                while (!peek_trimmed.empty() &&
                       (peek_trimmed.back() == '\n' || peek_trimmed.back() == '\r' ||
                        peek_trimmed.back() == ' ' || peek_trimmed.back() == '\t')) {
                    peek_trimmed.remove_suffix(1);
                }
                while (!peek_trimmed.empty() &&
                       (peek_trimmed.front() == ' ' || peek_trimmed.front() == '\t')) {
                    peek_trimmed.remove_prefix(1);
                }
                if (peek_trimmed.empty() || peek_trimmed.starts_with("#")) {
                    j = peek_next;
                    continue;
                }
                if (peek_trimmed.starts_with("["))
                    break;
                empty_section = false;
                break;
            }

            if (empty_section) {
                i = next;
                if (i < content.size() && content[i] == '\n')
                    ++i;
                continue;
            }
        }

        cleaned.append(line);
        i = next;
    }

    content = std::move(cleaned);
}

// resolve consumer-selected features into a set of .cppm module basenames
// provider_features: the [features] table from the dep's exon.toml
// selected: features requested by the consumer
// use_defaults: whether to include the "default" feature set
std::set<std::string> resolve_features(
    std::map<std::string, std::vector<std::string>> const& provider_features,
    std::vector<std::string> const& selected,
    bool use_defaults) {

    // collect active feature names
    std::set<std::string> active;
    if (use_defaults && provider_features.contains("default"))
        active.insert("default");
    for (auto const& f : selected)
        active.insert(f);

    // validate that all requested features exist
    for (auto const& f : active) {
        if (f != "default" && !provider_features.contains(f))
            throw std::runtime_error(std::format("unknown feature '{}'", f));
    }

    // recursively expand features to module basenames
    std::set<std::string> modules;
    std::set<std::string> visited;

    auto expand = [&](this auto const& self, std::string const& name) -> void {
        if (visited.contains(name))
            return;
        visited.insert(name);
        auto it = provider_features.find(name);
        if (it == provider_features.end())
            return;
        for (auto const& entry : it->second) {
            if (entry != name && provider_features.contains(entry))
                self(entry); // entry is another feature name
            else
                modules.insert(entry); // entry is a module basename
        }
    };

    for (auto const& f : active)
        expand(f);

    if (modules.empty())
        throw std::runtime_error("no features enabled (resolved to zero modules)");

    return modules;
}

std::string predicate_to_cmake(std::string_view pred) {
    auto parsed = parse_predicate(pred);
    if (parsed.fields.empty())
        return "";

    auto field_to_cmake = [](std::string_view key, std::string_view value) -> std::string {
        if (key == "os") {
            if (value == "linux") return "CMAKE_SYSTEM_NAME STREQUAL \"Linux\"";
            if (value == "macos") return "CMAKE_SYSTEM_NAME STREQUAL \"Darwin\"";
            if (value == "windows") return "WIN32";
            if (value == "wasi") return "CMAKE_SYSTEM_NAME STREQUAL \"WASI\"";
            if (value == "android") return "CMAKE_SYSTEM_NAME STREQUAL \"Android\"";
        } else if (key == "arch") {
            if (value == "x86_64") return "CMAKE_SYSTEM_PROCESSOR MATCHES \"x86_64|AMD64\"";
            if (value == "aarch64") return "CMAKE_SYSTEM_PROCESSOR MATCHES \"aarch64|ARM64\"";
            if (value == "wasm32") return "CMAKE_SYSTEM_PROCESSOR STREQUAL \"wasm32\"";
        }
        return "";
    };

    std::vector<std::string> conditions;
    for (auto const& f : parsed.fields) {
        auto cmake = field_to_cmake(f.key, f.value);
        if (!cmake.empty())
            conditions.push_back(std::move(cmake));
    }

    if (conditions.empty()) return "";
    std::string result;
    for (std::size_t i = 0; i < conditions.size(); ++i) {
        if (i > 0) result += " AND ";
        result += conditions[i];
    }
    if (parsed.negated)
        return std::format("NOT ({})", result);
    return result;
}

Manifest parse(std::string_view content) {
    auto table = toml::parse(std::string{content});
    return from_toml(table);
}

} // namespace manifest
