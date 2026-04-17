export module fetch;
import std;
import manifest;
import toolchain;
import lock;

export namespace fetch {

struct FetchedDep {
    std::string key;  // dep key (e.g. "github.com/user/repo")
    std::string name; // canonical package / target name from exon.toml
    std::string package_name; // preserved canonical package name for compatibility
    std::string version;
    std::string commit;         // exact git commit hash (empty for path deps)
    std::filesystem::path path; // cached source path
    std::string subdir;         // non-empty for git+subdir deps
    bool is_dev = false;        // dev-dependency (test-only)
    bool is_path = false;       // local path / workspace dep (no git, no lock)
    std::vector<std::string> features; // consumer-selected features (empty = all)
    bool default_features = true;
    std::vector<std::string> aliases; // compatibility aliases requested by manifests
    std::vector<std::string> dependency_names; // canonical child package names
};

struct FetchResult {
    std::vector<FetchedDep> deps;
    lock::LockFile lock_file;
};

enum class RootKind {
    git,
    featured_git,
    subdir,
    path,
    workspace,
};

struct FetchRoot {
    RootKind kind;
    std::string key;
    std::string value;
    std::vector<std::string> features;
    bool default_features = true;
    bool is_dev = false;
};

struct FetchRequest {
    manifest::Manifest manifest;
    std::filesystem::path project_root;
    std::filesystem::path lock_path;
    bool include_dev = false;
    std::optional<toolchain::Platform> platform = std::nullopt;
};

struct FetchPlan {
    std::filesystem::path project_root;
    std::filesystem::path lock_path;
    std::optional<toolchain::Platform> platform = std::nullopt;
    bool include_dev = false;
    std::vector<FetchRoot> roots;
};

bool has_dependencies(manifest::Manifest const& m, bool include_dev = false) {
    bool has_any = !m.dependencies.empty() || !m.path_deps.empty() || !m.workspace_deps.empty() ||
                   !m.subdir_deps.empty() || !m.featured_deps.empty();
    bool has_dev = include_dev && (!m.dev_dependencies.empty() || !m.dev_path_deps.empty() ||
                                   !m.dev_workspace_deps.empty() || !m.dev_subdir_deps.empty() ||
                                   !m.dev_featured_deps.empty());
    return has_any || has_dev;
}

FetchPlan plan(FetchRequest const& request) {
    FetchPlan out{
        .project_root = request.project_root,
        .lock_path = request.lock_path,
        .platform = request.platform,
        .include_dev = request.include_dev,
    };

    auto append_roots = [&](auto const& deps, RootKind kind, bool is_dev) {
        for (auto const& [key, value] : deps) {
            FetchRoot root{
                .kind = kind,
                .key = key,
                .is_dev = is_dev,
            };
            if constexpr (std::same_as<std::decay_t<decltype(value)>, std::string>) {
                root.value = value;
            } else if constexpr (std::same_as<std::decay_t<decltype(value)>, manifest::GitFeatureDep>) {
                root.value = value.version;
                root.features = value.features;
                root.default_features = value.default_features;
            } else if constexpr (std::same_as<std::decay_t<decltype(value)>, manifest::GitSubdirDep>) {
                root.value = std::format("{}|{}|{}", value.repo, value.version, value.subdir);
            }
            out.roots.push_back(std::move(root));
        }
    };

    append_roots(request.manifest.dependencies, RootKind::git, false);
    append_roots(request.manifest.featured_deps, RootKind::featured_git, false);
    append_roots(request.manifest.subdir_deps, RootKind::subdir, false);
    append_roots(request.manifest.path_deps, RootKind::path, false);
    for (auto const& name : request.manifest.workspace_deps) {
        out.roots.push_back({
            .kind = RootKind::workspace,
            .key = name,
            .is_dev = false,
        });
    }

    if (request.include_dev) {
        append_roots(request.manifest.dev_dependencies, RootKind::git, true);
        append_roots(request.manifest.dev_featured_deps, RootKind::featured_git, true);
        append_roots(request.manifest.dev_subdir_deps, RootKind::subdir, true);
        append_roots(request.manifest.dev_path_deps, RootKind::path, true);
        for (auto const& name : request.manifest.dev_workspace_deps) {
            out.roots.push_back({
                .kind = RootKind::workspace,
                .key = name,
                .is_dev = true,
            });
        }
    }

    return out;
}

void mark_dev_dependencies(std::vector<FetchedDep>& deps, std::size_t from) {
    for (auto i = from; i < deps.size(); ++i)
        deps[i].is_dev = true;
}

void annotate_selected_features(FetchResult& result, manifest::Manifest const& m,
                                bool include_dev = false) {
    auto annotate = [&](std::map<std::string, manifest::GitFeatureDep> const& fdeps) {
        for (auto const& [key, fdep] : fdeps) {
            for (auto& dep : result.deps) {
                if (dep.key == key && dep.version == fdep.version) {
                    // union features additively
                    for (auto const& f : fdep.features) {
                        if (std::ranges::find(dep.features, f) == dep.features.end())
                            dep.features.push_back(f);
                    }
                    dep.default_features = dep.default_features && fdep.default_features;
                    break;
                }
            }
        }
    };
    annotate(m.featured_deps);
    if (include_dev)
        annotate(m.dev_featured_deps);

    // sync features into lock file entries
    for (auto const& dep : result.deps) {
        if (!dep.features.empty() && !dep.is_path) {
            for (auto& pkg : result.lock_file.packages) {
                if (pkg.name == dep.key && pkg.version == dep.version) {
                    pkg.features = dep.features;
                    break;
                }
            }
        }
    }
}

} // namespace fetch
