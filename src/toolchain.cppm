export module toolchain;
import std;
import cppx.env;

#if defined(_WIN32)
extern "C" int __cdecl _putenv_s(char const*, char const*);
#endif

export namespace toolchain {

// platform-specific executable suffix (".exe" on Windows, empty otherwise).
// Delegated to cppx::env::EXE_SUFFIX so the constant lives in one place.
inline constexpr std::string_view exe_suffix = cppx::env::EXE_SUFFIX;

// wrap a path/command in double quotes if it contains whitespace, so that
// `std::system()` can invoke binaries at paths like "C:\Program Files\...".
inline std::string shell_quote(std::string_view s) {
    return cppx::env::shell_quote(s);
}

// cross-platform home directory (HOME, falling back to USERPROFILE on Windows).
// Returns an empty path on failure for backwards compatibility with callers
// that use `.empty()` to detect "not set".
inline std::filesystem::path home_dir() {
    return cppx::env::home_dir().value_or(std::filesystem::path{});
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

// Re-exports of cppx::env constants for in-namespace use.
inline constexpr char path_separator = cppx::env::PATH_SEPARATOR;
inline constexpr std::string_view exe_suffix = cppx::env::EXE_SUFFIX;

// Search PATH for `name`. Returns the canonical full path on success, or
// the bare `name` (acts as fallback for std::system) on failure — preserves
// the previous semantics so callers can fall through to the system PATH.
inline std::string find_in_path(std::string_view name) {
    if (auto found = cppx::env::find_in_path(name))
        return found->string();
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

// detect MSVC cl.exe (Windows). If the developer environment is already
// loaded (vcvars64.bat), use it directly.  Otherwise, locate Visual Studio
// via vswhere.exe, run vcvars64.bat, capture the resulting environment
// variables, and apply them to the current process so that CMake and Ninja
// can find cl.exe, INCLUDE, LIB, etc.
void detect_msvc(Toolchain& tc) {
#if defined(_WIN32)
    // fast path: developer environment already loaded
    auto cl = find_in_path("cl.exe");
    if (cl != "cl.exe") {
        auto include = std::getenv("INCLUDE");
        if (include && *include) {
            tc.is_msvc = true;
            tc.native_import_std = true;
            return;
        }
    }

    // locate Visual Studio via vswhere.exe
    auto vswhere = std::filesystem::path{
        "C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"};
    if (!std::filesystem::exists(vswhere))
        return;

    auto tmp_vs = std::filesystem::temp_directory_path() / "exon_vswhere.txt";
    auto vs_cmd = std::format("cmd /c \"\"{}\" -latest -property installationPath > \"{}\"\"",
                              vswhere.string(), tmp_vs.string());
    std::system(vs_cmd.c_str());

    auto vs_file = std::ifstream{tmp_vs};
    std::string vs_path;
    std::getline(vs_file, vs_path);
    vs_file.close();
    std::filesystem::remove(tmp_vs);

    while (!vs_path.empty() && (vs_path.back() == '\r' || vs_path.back() == '\n'))
        vs_path.pop_back();
    if (vs_path.empty())
        return;

    auto vcvars = std::filesystem::path{vs_path} / "VC" / "Auxiliary" / "Build" / "vcvars64.bat";
    if (!std::filesystem::exists(vcvars))
        return;

    // run vcvars64.bat and capture resulting environment
    auto tmp_env = std::filesystem::temp_directory_path() / "exon_msvc_env.txt";
    auto env_cmd = std::format("cmd /c \"\"{}\" && set > \"{}\"\"",
                               vcvars.string(), tmp_env.string());
    if (std::system(env_cmd.c_str()) != 0) {
        std::filesystem::remove(tmp_env);
        return;
    }

    auto env_file = std::ifstream{tmp_env};
    std::string line;
    while (std::getline(env_file, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        auto eq = line.find('=');
        if (eq == std::string::npos || eq == 0)
            continue;
        auto key = line.substr(0, eq);
        auto value = line.substr(eq + 1);
        _putenv_s(key.c_str(), value.c_str());
    }
    env_file.close();
    std::filesystem::remove(tmp_env);

    tc.is_msvc = true;
    tc.native_import_std = true;
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

struct WasmToolchain {
    std::string triple;          // e.g. "wasm32-wasi"
    std::string sdk_path;        // wasi-sdk root directory
    std::string cmake_toolchain; // absolute path to wasi-sdk.cmake
    std::string modules_json;    // absolute path to libc++.modules.json
    std::string scan_deps;       // host clang-scan-deps (wasi-sdk lacks it)
};

WasmToolchain detect_wasm(std::string_view triple) {
    if (triple != "wasm32-wasi")
        throw std::runtime_error(
            std::format("unsupported WASM target '{}' (supported: wasm32-wasi)", triple));

    // find wasi-sdk: intron → WASI_SDK_PATH env
    auto sdk = detail::find_intron_latest("wasi-sdk");
    if (sdk.empty()) {
        if (auto const* env = std::getenv("WASI_SDK_PATH"); env && *env)
            sdk = env;
    }
    if (sdk.empty())
        throw std::runtime_error(
            "wasi-sdk not found (install: intron install wasi-sdk <version>, "
            "or set WASI_SDK_PATH)");

    auto sdk_path = std::filesystem::path{sdk};

    // use wasip1 toolchain (wasm32-wasi is deprecated in clang 22+)
    auto toolchain_file = sdk_path / "share" / "cmake" / "wasi-sdk-p1.cmake";
    if (!std::filesystem::exists(toolchain_file)) {
        // fallback to legacy wasi-sdk.cmake
        toolchain_file = sdk_path / "share" / "cmake" / "wasi-sdk.cmake";
    }
    if (!std::filesystem::exists(toolchain_file))
        throw std::runtime_error(
            std::format("wasi-sdk toolchain file not found: {}", toolchain_file.string()));

    auto modules_json = sdk_path / "share" / "wasi-sysroot" / "lib" / "wasm32-wasip1" /
                         "libc++.modules.json";

    // wasi-sdk lacks clang-scan-deps; use the host LLVM's copy for module scanning
    std::string scan_deps;
    auto intron_llvm = detail::find_intron_latest("llvm");
    if (!intron_llvm.empty()) {
        auto p = std::filesystem::path{intron_llvm} / "bin" / "clang-scan-deps";
        if (detail::exists_bin(p))
            scan_deps = detail::canonical_bin(p).string();
    }
    if (scan_deps.empty()) {
        auto p = detail::find_in_path("clang-scan-deps");
        if (p != "clang-scan-deps")
            scan_deps = p;
    }

    WasmToolchain wt;
    wt.triple = triple;
    wt.sdk_path = sdk_path.string();
    wt.cmake_toolchain = std::filesystem::canonical(toolchain_file).string();
    if (std::filesystem::exists(modules_json))
        wt.modules_json = std::filesystem::canonical(modules_json).string();
    wt.scan_deps = std::move(scan_deps);
    return wt;
}

std::string detect_wasm_runtime() {
    auto rt = detail::find_in_path("wasmtime");
    if (rt != "wasmtime")
        return rt;
    return {};
}

std::optional<Platform> platform_from_target(std::string_view triple) {
    if (triple.starts_with("wasm32-wasi"))
        return Platform{.os = "wasi", .arch = "wasm32"};
    return std::nullopt;
}

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

    // ninja: intron → PATH (verify the binary actually runs to catch arch mismatches)
    auto intron_ninja = detail::find_intron_latest("ninja");
    if (!intron_ninja.empty()) {
        auto bin = std::filesystem::path{intron_ninja} / "ninja";
        if (detail::exists_bin(bin)) {
            auto candidate = detail::canonical_bin(bin).string();
            auto verify = std::format("{} --version", shell_quote(candidate));
#if defined(_WIN32)
            if (std::system((verify + " > NUL 2>&1").c_str()) == 0)
#else
            if (std::system((verify + " > /dev/null 2>&1").c_str()) == 0)
#endif
                tc.ninja = std::move(candidate);
        }
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
