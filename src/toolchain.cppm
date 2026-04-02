export module toolchain;
import std;

export namespace toolchain {

// 추후 intron 연동 시 이 모듈만 수정하면 됨
struct Toolchain {
    std::string cmake;
    std::string ninja;
    std::string cxx_compiler;
    std::string stdlib_modules_json; // libc++.modules.json 경로 (import std 지원)
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
            return std::filesystem::canonical(full).string();
        }
        if (sep == std::string_view::npos) break;
        pos = sep + 1;
    }
    return std::string{name};
}

// PATH에서 찾은 clang++ 위치 기준으로 libc++.modules.json 탐색
// 패턴: <root>/bin/clang++ → <root>/lib/c++/libc++.modules.json
void detect_clang(Toolchain& tc) {
    auto clangpp = find_in_path("clang++");
    if (clangpp == "clang++") return; // PATH에 없음

    auto bin_dir = std::filesystem::path{clangpp}.parent_path();
    auto root = bin_dir.parent_path();

    tc.cxx_compiler = clangpp;

    auto modules_json = root / "lib" / "c++" / "libc++.modules.json";
    if (std::filesystem::exists(modules_json)) {
        tc.stdlib_modules_json = std::filesystem::canonical(modules_json).string();
    }
}

} // namespace detail

Toolchain detect() {
    // TODO: intron 연동 시 intron에서 경로를 가져오도록 변경
    Toolchain tc;
    tc.cmake = detail::find_in_path("cmake");
    tc.ninja = detail::find_in_path("ninja");
    detail::detect_clang(tc);
    return tc;
}

} // namespace toolchain
