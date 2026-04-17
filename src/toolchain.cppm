export module toolchain;
import std;
import cppx.env;

export namespace toolchain {

// platform-specific executable suffix (".exe" on Windows, empty otherwise).
// Delegated to cppx::env::EXE_SUFFIX so the constant lives in one place.
inline constexpr std::string_view exe_suffix = cppx::env::EXE_SUFFIX;

// wrap a path/command in double quotes if it contains whitespace, so that
// `std::system()` can invoke binaries at paths like "C:\Program Files\...".
inline std::string shell_quote(std::string_view s) {
    return cppx::env::shell_quote(s);
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

struct WasmToolchain {
    std::string triple;          // e.g. "wasm32-wasi"
    std::string sdk_path;        // wasi-sdk root directory
    std::string cmake_toolchain; // absolute path to wasi-sdk.cmake
    std::string modules_json;    // absolute path to libc++.modules.json
    std::string scan_deps;       // host clang-scan-deps (wasi-sdk lacks it)
};

std::optional<Platform> platform_from_target(std::string_view triple) {
    if (triple.starts_with("wasm32-wasi"))
        return Platform{.os = "wasi", .arch = "wasm32"};
    return std::nullopt;
}

} // namespace toolchain
