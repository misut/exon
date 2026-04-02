export module manifest;
import std;
import toml;

export namespace manifest {

struct Manifest {
    std::string name;
    std::string version;
    std::string description;
    std::vector<std::string> authors;
    std::string license;
    int standard = 23;
    std::map<std::string, std::string> dependencies;
};

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
        if (pkg.contains("standard"))
            m.standard = static_cast<int>(pkg.at("standard").as_integer());
        if (pkg.contains("authors")) {
            for (auto const& a : pkg.at("authors").as_array()) {
                m.authors.push_back(a.as_string());
            }
        }
    }

    if (table.contains("dependencies")) {
        auto const& deps = table.at("dependencies").as_table();
        for (auto const& [key, val] : deps) {
            m.dependencies.emplace(key, val.as_string());
        }
    }

    return m;
}

Manifest load(std::string_view path) {
    auto table = toml::parse_file(path);
    return from_toml(table);
}

} // namespace manifest
