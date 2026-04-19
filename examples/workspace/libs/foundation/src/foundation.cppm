export module foundation;
import std;

export namespace foundation {

std::string shared_defaults() {
    return "workspace.package -> version 0.1.0, license MIT, standard C++23";
}

std::string member_label(std::string_view member, std::string_view kind) {
    return std::format("{} ({})", member, kind);
}

std::string dependency_path(std::string_view member, std::string_view path) {
    return std::format("{} uses {}", member, path);
}

}
