export module toolchain;
import std;

export namespace toolchain {

// 추후 intron 연동 시 이 모듈만 수정하면 됨
struct Toolchain {
    std::string cmake;
    std::string ninja;
    std::string cxx_compiler;
};

namespace detail {

std::string find_in_path(std::string_view name) {
    auto path_env = std::getenv("PATH");
    if (!path_env) return std::string{name};

    auto path_str = std::string_view{path_env};
    std::size_t pos = 0;
    while (pos < path_str.size()) {
        auto sep = path_str.find(':', pos);
        auto dir = path_str.substr(pos, sep == std::string_view::npos ? sep : sep - pos);
        auto full = std::filesystem::path{dir} / name;
        if (std::filesystem::exists(full)) {
            return full.string();
        }
        if (sep == std::string_view::npos) break;
        pos = sep + 1;
    }
    return std::string{name};
}

} // namespace detail

Toolchain detect() {
    // TODO: intron 연동 시 intron에서 경로를 가져오도록 변경
    // 현재는 PATH에서 cmake, ninja를 찾아 절대 경로로 resolve
    Toolchain tc;
    tc.cmake = detail::find_in_path("cmake");
    tc.ninja = detail::find_in_path("ninja");
    return tc;
}

} // namespace toolchain
