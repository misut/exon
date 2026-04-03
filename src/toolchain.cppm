export module toolchain;
import std;

export namespace toolchain {

// 추후 intron 연동 시 이 모듈만 수정하면 됨
struct Toolchain {
    std::string cmake;
    std::string ninja;
    std::string cxx_compiler;
    std::string stdlib_modules_json; // libc++.modules.json 경로 (import std 지원)
    std::string lib_dir;             // libc++ 라이브러리 경로 (링커용)
    std::string sysroot;             // macOS SDK 경로
};

namespace detail {

std::string find_in_path(std::string_view name) {
    auto path_env = std::getenv("PATH");
    if (!path_env)
        return std::string{name};

    auto path_str = std::string_view{path_env};
    std::size_t pos = 0;
    while (pos < path_str.size()) {
        auto sep = path_str.find(':', pos);
        auto dir = path_str.substr(pos, sep == std::string_view::npos ? sep : sep - pos);
        auto full = std::filesystem::path{dir} / name;
        if (std::filesystem::exists(full)) {
            return std::filesystem::canonical(full).string();
        }
        if (sep == std::string_view::npos)
            break;
        pos = sep + 1;
    }
    return std::string{name};
}

// PATH에서 찾은 clang++ 위치 기준으로 libc++.modules.json 탐색
// Homebrew: <root>/lib/c++/libc++.modules.json
// LLVM 공식: <root>/lib/libc++.modules.json
void detect_clang(Toolchain& tc) {
    auto clangpp = find_in_path("clang++");
    if (clangpp == "clang++")
        return; // PATH에 없음

    auto bin_dir = std::filesystem::path{clangpp}.parent_path();
    auto root = bin_dir.parent_path();

    tc.cxx_compiler = clangpp;

    // modules json 탐색 (여러 경로)
    auto candidates = {
        root / "lib" / "c++" / "libc++.modules.json", // Homebrew
        root / "lib" / "libc++.modules.json",          // LLVM 공식 (intron)
    };
    for (auto const& path : candidates) {
        if (std::filesystem::exists(path)) {
            tc.stdlib_modules_json = std::filesystem::canonical(path).string();
            tc.lib_dir = std::filesystem::canonical(path.parent_path()).string();
            break;
        }
    }
}

} // namespace detail

Toolchain detect() {
    Toolchain tc;
    tc.cmake = detail::find_in_path("cmake");
    tc.ninja = detail::find_in_path("ninja");
    detail::detect_clang(tc);

    // macOS sysroot 감지
    #if defined(__APPLE__)
    auto xcrun = detail::find_in_path("xcrun");
    if (xcrun != "xcrun") {
        // xcrun --show-sdk-path를 읽을 수 없으므로 알려진 경로 탐색
        auto candidates = {
            std::filesystem::path{"/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk"},
            std::filesystem::path{"/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk"},
        };
        for (auto const& p : candidates) {
            if (std::filesystem::exists(p)) {
                tc.sysroot = p.string();
                break;
            }
        }
    }
    #endif

    return tc;
}

} // namespace toolchain
