export module toolchain.system;
import std;
import cppx.env.system;
import toolchain;

#if defined(_WIN32)
extern "C" int __cdecl _putenv_s(char const*, char const*);
#endif

export namespace toolchain::system {

inline std::filesystem::path home_dir() {
    return cppx::env::system::home_dir().value_or(std::filesystem::path{});
}

namespace detail {

inline std::string find_in_path(std::string_view name) {
    if (auto found = cppx::env::system::find_in_path(name))
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

std::string env_value(std::string_view name) {
    auto key = std::string{name};
    if (auto const* value = std::getenv(key.c_str()); value && *value)
        return value;
    return {};
}

std::string lowercase_ascii(std::string_view value) {
    auto text = std::string{value};
    std::ranges::transform(text, text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

toolchain::CompilerKind classify_compiler(std::string_view candidate) {
    if (candidate.empty())
        return toolchain::CompilerKind::unknown;

    auto name = lowercase_ascii(std::filesystem::path{std::string{candidate}}.filename().string());
    if (name == "clang-cl" || name == "clang-cl.exe")
        return toolchain::CompilerKind::clang_cl;
    if (name == "cl" || name == "cl.exe")
        return toolchain::CompilerKind::msvc_cl;
    if (name == "clang++" || name == "clang++.exe" ||
        name == "clang" || name == "clang.exe")
        return toolchain::CompilerKind::clang;
    return toolchain::CompilerKind::other;
}

void capture_compiler_environment(toolchain::Toolchain& tc) {
    tc.env_cc = env_value("CC");
    tc.env_cxx = env_value("CXX");
    auto const& env_compiler = !tc.env_cxx.empty() ? tc.env_cxx : tc.env_cc;
    if (!env_compiler.empty()) {
        tc.compiler_from_environment = true;
        tc.compiler_kind = classify_compiler(env_compiler);
    }
}

void finalize_compiler_provenance(toolchain::Toolchain& tc) {
    if (tc.compiler_from_environment)
        return;
    if (!tc.cxx_compiler.empty()) {
        tc.compiler_kind = classify_compiler(tc.cxx_compiler);
        return;
    }
    if (tc.is_msvc)
        tc.compiler_kind = toolchain::CompilerKind::msvc_cl;
}

// detect MSVC cl.exe (Windows). If the developer environment is already
// loaded (vcvars64.bat), use it directly. Otherwise, locate Visual Studio
// via vswhere.exe, run vcvars64.bat, capture the resulting environment
// variables, and apply them to the current process so that CMake and Ninja
// can find cl.exe, INCLUDE, LIB, etc.
void detect_msvc(toolchain::Toolchain& tc) {
#if defined(_WIN32)
    // fast path: developer environment already loaded
    auto cl = find_in_path("cl.exe");
    if (cl != "cl.exe") {
        auto include = std::getenv("INCLUDE");
        if (include && *include) {
            tc.has_msvc_developer_env = true;
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

    tc.has_msvc_developer_env = true;
    tc.is_msvc = true;
    tc.native_import_std = true;
#else
    (void)tc;
#endif
}

// detect clang++ from intron, fall back to system LLVM, then PATH
void detect_clang(toolchain::Toolchain& tc) {
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

toolchain::WasmToolchain detect_wasm(std::string_view triple) {
    if (triple != "wasm32-wasi")
        throw std::runtime_error(
            std::format("unsupported WASM target '{}' (supported: wasm32-wasi)", triple));

    // find wasi-sdk: intron -> WASI_SDK_PATH env
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

    toolchain::WasmToolchain wt;
    wt.triple = triple;
    wt.sdk_path = sdk_path.string();
    wt.cmake_toolchain = std::filesystem::canonical(toolchain_file).string();
    if (std::filesystem::exists(modules_json))
        wt.modules_json = std::filesystem::canonical(modules_json).string();
    wt.scan_deps = std::move(scan_deps);
    return wt;
}

toolchain::AndroidToolchain detect_android(std::string_view triple) {
    // supported ABI map
    std::string abi;
    if (triple == "aarch64-linux-android") {
        abi = "arm64-v8a";
    } else {
        throw std::runtime_error(std::format(
            "unsupported Android target '{}' (supported: aarch64-linux-android)", triple));
    }

    // find android-ndk: intron -> ANDROID_NDK_HOME -> ANDROID_NDK_ROOT
    auto ndk = detail::find_intron_latest("android-ndk");
    if (ndk.empty()) {
        if (auto const* env = std::getenv("ANDROID_NDK_HOME"); env && *env)
            ndk = env;
    }
    if (ndk.empty()) {
        if (auto const* env = std::getenv("ANDROID_NDK_ROOT"); env && *env)
            ndk = env;
    }
    if (ndk.empty())
        throw std::runtime_error(
            "android-ndk not found (install: intron install android-ndk <version>, "
            "or set ANDROID_NDK_HOME / ANDROID_NDK_ROOT)");

    auto ndk_path = std::filesystem::path{ndk};

    auto toolchain_file = ndk_path / "build" / "cmake" / "android.toolchain.cmake";
    if (!std::filesystem::exists(toolchain_file))
        throw std::runtime_error(std::format(
            "android-ndk toolchain file not found: {}", toolchain_file.string()));

    // NDK's prebuilt LLVM host folder contains libc++.modules.json and the scan-deps binary.
    // Google ships a single macOS host (darwin-x86_64, universal), linux-x86_64, and windows-x86_64.
    auto prebuilt_root = ndk_path / "toolchains" / "llvm" / "prebuilt";
    std::filesystem::path host_root;
    if (std::filesystem::exists(prebuilt_root)) {
        for (auto const& entry : std::filesystem::directory_iterator(prebuilt_root)) {
            if (entry.is_directory()) {
                host_root = entry.path();
                break;
            }
        }
    }
    if (host_root.empty())
        throw std::runtime_error(std::format(
            "android-ndk prebuilt host toolchain not found under {}", prebuilt_root.string()));

    auto modules_json = host_root / "lib" / "libc++.modules.json";

    // NDK ships its own clang-scan-deps; prefer it over host fallback for consistency
    std::string scan_deps;
    auto ndk_scan = host_root / "bin" / "clang-scan-deps";
    if (detail::exists_bin(ndk_scan))
        scan_deps = detail::canonical_bin(ndk_scan).string();
    if (scan_deps.empty()) {
        auto intron_llvm = detail::find_intron_latest("llvm");
        if (!intron_llvm.empty()) {
            auto p = std::filesystem::path{intron_llvm} / "bin" / "clang-scan-deps";
            if (detail::exists_bin(p))
                scan_deps = detail::canonical_bin(p).string();
        }
    }

    // sysroot lives alongside the host toolchain; we inject it into CMAKE_CXX_FLAGS
    // so CMake's stdlib detection probe (which ignores the Android toolchain file's
    // --target/--sysroot injection) can find <version> and correctly identify libc++.
    auto sysroot = host_root / "sysroot";

    constexpr int android_api_level = 33;

    // clang's canonical Android target triple for use with --target=
    // (NDK r29+ uses `aarch64-none-linux-android<api>` as the effective triple).
    auto const arch = std::string_view{triple}.substr(0, std::string_view{triple}.find('-'));
    auto clang_target = std::format("{}-none-linux-android{}", arch, android_api_level);

    toolchain::AndroidToolchain at;
    at.triple = std::string{triple};
    at.clang_target = std::move(clang_target);
    at.ndk_path = ndk_path.string();
    if (std::filesystem::exists(sysroot))
        at.sysroot = std::filesystem::canonical(sysroot).string();
    at.cmake_toolchain = std::filesystem::canonical(toolchain_file).string();
    if (std::filesystem::exists(modules_json))
        at.modules_json = std::filesystem::canonical(modules_json).string();
    at.abi = std::move(abi);
    at.platform = std::format("android-{}", android_api_level);
    at.api_level = android_api_level;
    at.scan_deps = std::move(scan_deps);
    return at;
}

std::string detect_wasm_runtime() {
    auto rt = detail::find_in_path("wasmtime");
    if (rt != "wasmtime")
        return rt;
    return {};
}

toolchain::Toolchain detect() {
    toolchain::Toolchain tc;
    detail::capture_compiler_environment(tc);

    // cmake: intron -> PATH
    auto intron_cmake = detail::find_intron_latest("cmake");
    if (!intron_cmake.empty()) {
        auto bin = std::filesystem::path{intron_cmake} / "bin" / "cmake";
        if (detail::exists_bin(bin))
            tc.cmake = detail::canonical_bin(bin).string();
    }
    if (tc.cmake.empty())
        tc.cmake = detail::find_in_path("cmake");

    // ninja: intron -> PATH (verify the binary actually runs to catch arch mismatches)
    auto intron_ninja = detail::find_intron_latest("ninja");
    if (!intron_ninja.empty()) {
        auto bin = std::filesystem::path{intron_ninja} / "ninja";
        if (detail::exists_bin(bin)) {
            auto candidate = detail::canonical_bin(bin).string();
            auto verify = std::format("{} --version", toolchain::shell_quote(candidate));
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

    // compiler: MSVC (Windows dev env) -> clang (intron -> system LLVM -> PATH)
    detail::detect_msvc(tc);
    if (!tc.is_msvc)
        detail::detect_clang(tc);
    detail::finalize_compiler_provenance(tc);

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

} // namespace toolchain::system
