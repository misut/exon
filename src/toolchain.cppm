export module toolchain;
import std;
import cppx.env;
import cppx.platform;

export namespace toolchain {

using OS = cppx::platform::OS;
using Arch = cppx::platform::Arch;
using Platform = cppx::platform::Platform;

// platform-specific executable suffix (".exe" on Windows, empty otherwise).
// Delegated to cppx::env::EXE_SUFFIX so the constant lives in one place.
inline constexpr std::string_view exe_suffix = cppx::env::EXE_SUFFIX;

// wrap a path/command in double quotes if it contains whitespace, so that
// `std::system()` can invoke binaries at paths like "C:\Program Files\...".
inline std::string shell_quote(std::string_view s) {
    return cppx::env::shell_quote(s);
}

inline auto parse_os(std::string_view value) -> OS {
    return cppx::platform::parse_os(value);
}

inline auto parse_arch(std::string_view value) -> Arch {
    return cppx::platform::parse_arch(value);
}

inline auto parse_platform(std::string_view value) -> Platform {
    return cppx::platform::parse_platform(value);
}

inline auto make_platform(std::string_view os = {}, std::string_view arch = {}) -> Platform {
    return Platform{parse_os(os), parse_arch(arch)};
}

inline auto platform_has_os(Platform const& platform) -> bool {
    return platform.os != OS::Unknown;
}

inline auto platform_has_arch(Platform const& platform) -> bool {
    return platform.arch != Arch::Unknown;
}

inline auto platform_os_name(Platform const& platform) -> std::string_view {
    return platform_has_os(platform)
        ? cppx::platform::os_name(platform.os)
        : std::string_view{};
}

inline auto platform_arch_name(Platform const& platform) -> std::string_view {
    return platform_has_arch(platform)
        ? cppx::platform::arch_name(platform.arch)
        : std::string_view{};
}

inline auto detect_host_platform() -> Platform {
    return cppx::platform::host();
}

enum class CompilerKind {
    unknown,
    msvc_cl,
    clang_cl,
    clang,
    other,
};

inline auto compiler_kind_name(CompilerKind kind) -> std::string_view {
    switch (kind) {
    case CompilerKind::unknown:
        return "unknown";
    case CompilerKind::msvc_cl:
        return "msvc-cl";
    case CompilerKind::clang_cl:
        return "clang-cl";
    case CompilerKind::clang:
        return "clang";
    case CompilerKind::other:
        return "other";
    }
    return "unknown";
}

struct Toolchain {
    std::string cmake;
    std::string ninja;
    std::string cxx_compiler;
    std::string env_cc;
    std::string env_cxx;
    std::string stdlib_modules_json; // libc++.modules.json path (import std support)
    std::string lib_dir;             // libc++ library path (for linker)
    std::string sysroot;             // macOS SDK path
    CompilerKind compiler_kind = CompilerKind::unknown;
    bool has_clang_config = false;   // if clang config exists, linker flags are unnecessary
    bool needs_stdlib_flag = false;  // if -stdlib=libc++ is needed (Linux)
    bool is_msvc = false;            // compiler is MSVC cl.exe (Windows)
    bool compiler_from_environment = false;
    bool has_msvc_developer_env = false;
    bool native_import_std = false;  // compiler has native `import std;` (no modules json needed)
};

struct WasmToolchain {
    std::string triple;          // e.g. "wasm32-wasi"
    std::string sdk_path;        // wasi-sdk root directory
    std::string cmake_toolchain; // absolute path to wasi-sdk.cmake
    std::string modules_json;    // absolute path to libc++.modules.json
    std::string scan_deps;       // host clang-scan-deps (wasi-sdk lacks it)
};

struct AndroidToolchain {
    std::string triple;          // e.g. "aarch64-linux-android"
    std::string ndk_path;        // android-ndk root directory
    std::string cmake_toolchain; // absolute path to android.toolchain.cmake
    std::string modules_json;    // absolute path to libc++.modules.json
    std::string abi;             // CMake ANDROID_ABI (e.g. "arm64-v8a")
    std::string platform;        // CMake ANDROID_PLATFORM (e.g. "android-33")
    std::string scan_deps;       // host clang-scan-deps
};

inline auto platform_from_target(std::string_view triple) -> std::optional<Platform> {
    return cppx::platform::platform_from_target_triple(triple);
}

} // namespace toolchain
