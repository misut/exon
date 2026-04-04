export module toolchain;
import std;

export namespace toolchain {

struct Toolchain {
    std::string cmake;
    std::string ninja;
    std::string cxx_compiler;
    std::string stdlib_modules_json; // libc++.modules.json path (import std support)
    std::string lib_dir;             // libc++ library path (for linker)
    std::string sysroot;             // macOS SDK path
    bool has_clang_config = false;   // if clang config exists, linker flags are unnecessary
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

std::filesystem::path intron_root() {
    auto home = std::getenv("HOME");
    if (!home)
        return {};
    return std::filesystem::path{home} / ".intron" / "toolchains";
}

// find the highest version in intron toolchains directory
std::string find_intron_latest(std::string_view tool) {
    auto root = intron_root() / tool;
    if (!std::filesystem::exists(root))
        return {};

    std::string latest;
    for (auto const& entry : std::filesystem::directory_iterator(root)) {
        if (!entry.is_directory())
            continue;
        auto ver = entry.path().filename().string();
        if (latest.empty() || ver > latest)
            latest = ver;
    }
    if (latest.empty())
        return {};
    return (root / latest).string();
}

// detect clang++ from intron, fall back to PATH
void detect_clang(Toolchain& tc) {
    // 1. intron LLVM
    auto intron_llvm = find_intron_latest("llvm");
    if (!intron_llvm.empty()) {
        auto clangpp = std::filesystem::path{intron_llvm} / "bin" / "clang++";
        if (std::filesystem::exists(clangpp)) {
            tc.cxx_compiler = std::filesystem::canonical(clangpp).string();
        }
    }

    // 2. PATH fallback
    if (tc.cxx_compiler.empty()) {
        auto clangpp = find_in_path("clang++");
        if (clangpp != "clang++")
            tc.cxx_compiler = clangpp;
    }

    if (tc.cxx_compiler.empty())
        return;

    auto root = std::filesystem::path{tc.cxx_compiler}.parent_path().parent_path();

    // if clang config has -lc++, clang handles linker flags
    auto etc_dir = root / "etc" / "clang";
    if (std::filesystem::exists(etc_dir)) {
        for (auto const& entry : std::filesystem::directory_iterator(etc_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".cfg") {
                auto f = std::ifstream(entry.path());
                auto content = std::string{std::istreambuf_iterator<char>{f},
                                           std::istreambuf_iterator<char>{}};
                if (content.find("-lc++") != std::string::npos) {
                    tc.has_clang_config = true;
                    break;
                }
            }
        }
    }

    // search for modules json
    auto candidates = {
        root / "lib" / "c++" / "libc++.modules.json", // Homebrew
        root / "lib" / "libc++.modules.json",          // LLVM official (intron)
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

    // cmake: intron → PATH
    auto intron_cmake = detail::find_intron_latest("cmake");
    if (!intron_cmake.empty()) {
        auto bin = std::filesystem::path{intron_cmake} / "bin" / "cmake";
        if (std::filesystem::exists(bin))
            tc.cmake = std::filesystem::canonical(bin).string();
    }
    if (tc.cmake.empty())
        tc.cmake = detail::find_in_path("cmake");

    // ninja: intron → PATH
    auto intron_ninja = detail::find_intron_latest("ninja");
    if (!intron_ninja.empty()) {
        auto bin = std::filesystem::path{intron_ninja} / "ninja";
        if (std::filesystem::exists(bin))
            tc.ninja = std::filesystem::canonical(bin).string();
    }
    if (tc.ninja.empty())
        tc.ninja = detail::find_in_path("ninja");

    // clang: intron → PATH
    detail::detect_clang(tc);

    // detect macOS sysroot
#if defined(__APPLE__)
    auto candidates = {
        std::filesystem::path{
            "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk"},
        std::filesystem::path{"/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk"},
    };
    for (auto const& p : candidates) {
        if (std::filesystem::exists(p)) {
            tc.sysroot = p.string();
            break;
        }
    }
#endif

    return tc;
}

} // namespace toolchain
