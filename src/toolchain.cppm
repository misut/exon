export module toolchain;
import std;

export namespace toolchain {

// platform-specific executable suffix (".exe" on Windows, empty otherwise)
#if defined(_WIN32)
constexpr std::string_view exe_suffix = ".exe";
#else
constexpr std::string_view exe_suffix = "";
#endif

// wrap a path/command in double quotes if it contains whitespace, so that
// `std::system()` can invoke binaries at paths like "C:\Program Files\...".
std::string shell_quote(std::string_view s) {
    if (s.find_first_of(" \t") == std::string_view::npos)
        return std::string{s};
    return std::format("\"{}\"", s);
}

// cross-platform home directory (HOME, falling back to USERPROFILE on Windows)
std::filesystem::path home_dir() {
    if (auto const* h = std::getenv("HOME"); h && *h)
        return std::filesystem::path{h};
#if defined(_WIN32)
    if (auto const* up = std::getenv("USERPROFILE"); up && *up)
        return std::filesystem::path{up};
#endif
    return {};
}

struct Platform {
    std::string os;   // "linux", "macos", "windows"
    std::string arch; // "x86_64", "aarch64"

    // match: empty field = wildcard (matches any value)
    bool matches(Platform const& target) const {
        if (!os.empty() && !target.os.empty() && os != target.os)
            return false;
        if (!arch.empty() && !target.arch.empty() && arch != target.arch)
            return false;
        return true;
    }

    std::string to_string() const {
        if (!os.empty() && !arch.empty())
            return std::format("{}-{}", os, arch);
        if (!os.empty())
            return os;
        if (!arch.empty())
            return arch;
        return "any";
    }
};

Platform detect_host_platform() {
    Platform p;
#if defined(__APPLE__)
    p.os = "macos";
#elif defined(__linux__)
    p.os = "linux";
#elif defined(_WIN32)
    p.os = "windows";
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
    p.arch = "aarch64";
#elif defined(__x86_64__) || defined(_M_X64)
    p.arch = "x86_64";
#endif
    return p;
}

struct Toolchain {
    std::string cmake;
    std::string ninja;
    std::string cxx_compiler;
    std::string stdlib_modules_json; // libc++.modules.json path (import std support)
    std::string lib_dir;             // libc++ library path (for linker)
    std::string sysroot;             // macOS SDK path
    bool has_clang_config = false;   // if clang config exists, linker flags are unnecessary
    bool needs_stdlib_flag = false;  // if -stdlib=libc++ is needed (Linux)
    bool is_msvc = false;            // compiler is MSVC cl.exe (Windows)
    bool native_import_std = false;  // compiler has native `import std;` (no modules json needed)
};

namespace detail {

#if defined(_WIN32)
constexpr char path_separator = ';';
constexpr std::string_view exe_suffix = ".exe";
#else
constexpr char path_separator = ':';
constexpr std::string_view exe_suffix = "";
#endif

std::string find_in_path(std::string_view name) {
    auto path_env = std::getenv("PATH");
    if (!path_env)
        return std::string{name};

    auto path_str = std::string_view{path_env};
    std::size_t pos = 0;
    while (pos < path_str.size()) {
        auto sep = path_str.find(path_separator, pos);
        auto dir = path_str.substr(pos, sep == std::string_view::npos ? sep : sep - pos);
        auto full = std::filesystem::path{dir} / name;
        if (std::filesystem::exists(full)) {
            return std::filesystem::canonical(full).string();
        }
        if constexpr (!exe_suffix.empty()) {
            auto full_exe = full;
            full_exe += exe_suffix;
            if (std::filesystem::exists(full_exe))
                return std::filesystem::canonical(full_exe).string();
        }
        if (sep == std::string_view::npos)
            break;
        pos = sep + 1;
    }
    return std::string{name};
}

std::filesystem::path intron_root() {
    auto home = home_dir();
    if (home.empty())
        return {};
    return home / ".intron" / "toolchains";
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

// find system LLVM root on Linux (e.g. /usr/lib/llvm-20)
std::string find_system_llvm() {
#if defined(__linux__)
    auto usr_lib = std::filesystem::path{"/usr/lib"};
    if (!std::filesystem::exists(usr_lib))
        return {};

    std::string latest;
    for (auto const& entry : std::filesystem::directory_iterator(usr_lib)) {
        if (!entry.is_directory())
            continue;
        auto name = entry.path().filename().string();
        if (name.starts_with("llvm-")) {
            if (latest.empty() || name > latest)
                latest = name;
        }
    }
    if (latest.empty())
        return {};
    return (usr_lib / latest).string();
#else
    return {};
#endif
}

// look for a binary with optional .exe suffix on Windows
bool exists_bin(std::filesystem::path const& p) {
    if (std::filesystem::exists(p))
        return true;
#if defined(_WIN32)
    auto with_exe = p;
    with_exe += ".exe";
    return std::filesystem::exists(with_exe);
#else
    return false;
#endif
}

std::filesystem::path canonical_bin(std::filesystem::path const& p) {
    if (std::filesystem::exists(p))
        return std::filesystem::canonical(p);
#if defined(_WIN32)
    auto with_exe = p;
    with_exe += ".exe";
    if (std::filesystem::exists(with_exe))
        return std::filesystem::canonical(with_exe);
#endif
    return p;
}

// detect MSVC cl.exe (Windows). Requires a Visual Studio developer environment
// (vcvars64.bat) so that cl.exe is on PATH and INCLUDE/LIB env vars are set.
void detect_msvc(Toolchain& tc) {
#if defined(_WIN32)
    auto cl = find_in_path("cl.exe");
    if (cl == "cl.exe")
        return; // not found
    auto include = std::getenv("INCLUDE");
    if (!include || !*include)
        return; // no developer env loaded
    tc.is_msvc = true;
    tc.native_import_std = true;
    // leave cxx_compiler empty: CMake auto-detects MSVC from the environment.
#else
    (void)tc;
#endif
}

// detect clang++ from intron, fall back to system LLVM, then PATH
void detect_clang(Toolchain& tc) {
    // 1. intron LLVM
    auto intron_llvm = find_intron_latest("llvm");
    if (!intron_llvm.empty()) {
        auto clangpp = std::filesystem::path{intron_llvm} / "bin" / "clang++";
        if (exists_bin(clangpp)) {
            tc.cxx_compiler = canonical_bin(clangpp).string();
        }
    }

    // 2. system LLVM on Linux (e.g. /usr/lib/llvm-20/bin/clang++)
    if (tc.cxx_compiler.empty()) {
        auto sys_llvm = find_system_llvm();
        if (!sys_llvm.empty()) {
            auto clangpp = std::filesystem::path{sys_llvm} / "bin" / "clang++";
            if (exists_bin(clangpp)) {
                tc.cxx_compiler = canonical_bin(clangpp).string();
            }
        }
    }

    // 3. PATH fallback
    if (tc.cxx_compiler.empty()) {
        auto clangpp = find_in_path("clang++");
        if (clangpp != "clang++")
            tc.cxx_compiler = clangpp;
    }

    if (tc.cxx_compiler.empty())
        return;

    auto root = std::filesystem::path{tc.cxx_compiler}.parent_path().parent_path();

#if defined(__linux__)
    // Linux clang always needs -stdlib=libc++ (defaults to libstdc++)
    tc.needs_stdlib_flag = true;
#endif

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
        root / "lib" / "c++" / "libc++.modules.json",                      // Homebrew
        root / "lib" / "libc++.modules.json",                               // LLVM official
        root / "lib" / "x86_64-unknown-linux-gnu" / "libc++.modules.json",  // Linux x86_64
        root / "lib" / "aarch64-unknown-linux-gnu" / "libc++.modules.json", // Linux ARM64
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
        if (detail::exists_bin(bin))
            tc.cmake = detail::canonical_bin(bin).string();
    }
    if (tc.cmake.empty())
        tc.cmake = detail::find_in_path("cmake");

    // ninja: intron → PATH
    auto intron_ninja = detail::find_intron_latest("ninja");
    if (!intron_ninja.empty()) {
        auto bin = std::filesystem::path{intron_ninja} / "ninja";
        if (detail::exists_bin(bin))
            tc.ninja = detail::canonical_bin(bin).string();
    }
    if (tc.ninja.empty())
        tc.ninja = detail::find_in_path("ninja");

    // compiler: MSVC (Windows dev env) → clang (intron → system LLVM → PATH)
    detail::detect_msvc(tc);
    if (!tc.is_msvc)
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
