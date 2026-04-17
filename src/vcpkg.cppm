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
    std::map<std::string, manifest::VcpkgDep> all;
    for (auto const& [k, v] : m.dev_vcpkg_deps)
        all[k] = v;
    for (auto const& [k, v] : m.vcpkg_deps)
        all[k] = v; // overwrites dev if same key

    bool first = true;
    for (auto const& [pkg, dep] : all) {
        if (!first)
            out << ",";
        out << "\n    ";
        bool has_version = !(dep.version.empty() || dep.version == "*");
        bool has_features = !dep.features.empty();
        if (!has_version && !has_features) {
            out << json_escape(pkg);
        } else {
            out << "{\"name\": " << json_escape(pkg);
            if (has_version)
                out << ", \"version>=\": " << json_escape(dep.version);
            if (has_features) {
                out << ", \"features\": [";
                for (std::size_t i = 0; i < dep.features.size(); ++i) {
                    if (i > 0)
                        out << ", ";
                    out << json_escape(dep.features[i]);
                }
                out << "]";
            }
            out << "}";
        }
        first = false;
    }
    if (!all.empty())
        out << "\n  ";
    out << "]\n";
    out << "}\n";
    return out.str();
}

} // namespace vcpkg
