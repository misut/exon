export module vcpkg;
import std;
import manifest;

export namespace vcpkg {

// escape a string for JSON output (basic: ", \, control chars)
std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
                out += std::format("\\u{:04x}", static_cast<unsigned char>(c));
            else
                out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
}

// render vcpkg.json content from manifest (always includes dev deps)
std::string render_manifest(manifest::Manifest const& m) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"name\": " << json_escape(m.name) << ",\n";
    out << "  \"version-string\": " << json_escape(m.version.empty() ? "0.0.0" : m.version)
        << ",\n";
    out << "  \"dependencies\": [";

    // deduplicate: dev + regular, regular wins when key collides
    std::map<std::string, std::string> all;
    for (auto const& [k, v] : m.dev_vcpkg_deps)
        all[k] = v;
    for (auto const& [k, v] : m.vcpkg_deps)
        all[k] = v; // overwrites dev if same key

    bool first = true;
    for (auto const& [pkg, version] : all) {
        if (!first)
            out << ",";
        out << "\n    ";
        if (version == "*" || version.empty()) {
            out << json_escape(pkg);
        } else {
            out << "{\"name\": " << json_escape(pkg) << ", \"version>=\": "
                << json_escape(version) << "}";
        }
        first = false;
    }
    if (!all.empty())
        out << "\n  ";
    out << "]\n";
    out << "}\n";
    return out.str();
}

// write vcpkg.json to disk (creates parent dir if needed)
void write_manifest(manifest::Manifest const& m, std::filesystem::path const& out_path) {
    std::filesystem::create_directories(out_path.parent_path());
    auto content = render_manifest(m);
    auto file = std::ofstream(out_path);
    if (!file)
        throw std::runtime_error(std::format("failed to write {}", out_path.string()));
    file << content;
}

// verify a path looks like a vcpkg root (contains scripts/buildsystems/vcpkg.cmake)
bool looks_like_vcpkg_root(std::filesystem::path const& p) {
    return std::filesystem::exists(p / "scripts" / "buildsystems" / "vcpkg.cmake");
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
    if (auto const* home = std::getenv("HOME"); home && *home) {
        candidates.push_back(std::filesystem::path{home} / "vcpkg");
        candidates.push_back(std::filesystem::path{home} / ".vcpkg");
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

} // namespace vcpkg
