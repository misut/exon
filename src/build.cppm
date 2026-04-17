module;
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

export module build;
import std;
import core;
import manifest;
import manifest.system;
import toolchain;
import fetch;

export namespace build {

struct BuildRequest {
    core::ProjectContext project;
    manifest::Manifest manifest;
    toolchain::Toolchain toolchain;
    std::vector<fetch::FetchedDep> deps;
    bool release = false;
    bool with_tests = false;
    bool configured = false;
    bool any_cmake_deps = false;
    std::string wasm_toolchain_file;
    std::filesystem::path vcpkg_toolchain;
    std::vector<std::string> build_targets;
    std::vector<std::string> test_names;
    std::string filter;
    std::string wasm_runtime;
    std::optional<std::chrono::milliseconds> timeout = std::nullopt;
};

struct BuildPlan {
    core::ProjectContext project;
    std::vector<core::FileWrite> writes;
    std::vector<core::ProcessSpec> configure_steps;
    std::vector<core::ProcessSpec> build_steps;
    std::vector<core::ProcessSpec> run_steps;
    bool configured = false;
    std::string success_message;
    std::vector<std::string> test_names;
    std::string wasm_runtime;
    std::optional<std::chrono::milliseconds> timeout = std::nullopt;
};

// Read EXON_CXXFLAGS / EXON_LDFLAGS environment variables. These are an
// exon-only escape hatch — they pass through `cmake -DCMAKE_CXX_FLAGS=...`
// at configure time and do NOT propagate to raw cmake / add_subdirectory
// consumers. Declarative `[build]` and `[target.X.build]` flags now live
// in the generated CMakeLists.txt as target_compile_options/target_link_options
// so they DO propagate to those consumers.
std::string user_cxxflags() {
    if (auto const* env = std::getenv("EXON_CXXFLAGS"); env && *env)
        return std::string{env};
    return {};
}

std::string user_ldflags() {
    if (auto const* env = std::getenv("EXON_LDFLAGS"); env && *env)
        return std::string{env};
    return {};
}

namespace detail {

struct CommandResult {
    int exit_code = 0;
    bool timed_out = false;
};

std::optional<std::filesystem::path> current_executable_path() {
    std::error_code ec;
#if defined(_WIN32)
    std::wstring buffer(32768, L'\0');
    auto len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (len == 0 || len == buffer.size())
        return std::nullopt;
    buffer.resize(len);
    auto path = std::filesystem::path{buffer};
#elif defined(__APPLE__)
    uint32_t size = 0;
    std::array<char, 1> probe{};
    _NSGetExecutablePath(probe.data(), &size);
    if (size == 0)
        return std::nullopt;
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0)
        return std::nullopt;
    auto path = std::filesystem::path{buffer.c_str()};
#else
    auto path = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec)
        return std::nullopt;
#endif
    auto canon = std::filesystem::weakly_canonical(path, ec);
    if (!ec)
        return canon;
    return path;
}

bool is_exon_self_host_project(manifest::Manifest const& m,
                               std::filesystem::path const& project_root) {
    if (m.name != "exon")
        return false;
    auto required = {
        project_root / "exon.toml",
        project_root / "CMakeLists.txt",
        project_root / "src" / "build.cppm",
        project_root / "src" / "toolchain.cppm",
        project_root / "tests" / "test_build.cpp",
    };
    return std::ranges::all_of(required, [](std::filesystem::path const& path) {
        return std::filesystem::exists(path);
    });
}

std::array<std::filesystem::path, 3> self_host_bootstrap_inputs(
    std::filesystem::path const& project_root) {
    return {
        project_root / "exon.toml",
        project_root / "src" / "build.cppm",
        project_root / "src" / "toolchain.cppm",
    };
}

std::string display_path(std::filesystem::path const& path,
                         std::filesystem::path const& project_root) {
    std::error_code ec;
    auto rel = std::filesystem::relative(path, project_root, ec);
    if (!ec && !rel.empty()) {
        auto rel_str = rel.generic_string();
        if (rel_str != "." && !rel_str.starts_with("../"))
            return rel_str;
    }
    return path.generic_string();
}

bool build_has_asan_flag(manifest::Build const& b) {
    auto has_asan = [](std::vector<std::string> const& flags) {
        return std::ranges::any_of(flags, [](std::string const& flag) {
            return flag.contains("/fsanitize=address") ||
                   flag.contains("-fsanitize=address");
        });
    };
    return has_asan(b.cxxflags) || has_asan(b.ldflags);
}

bool build_has_windows_asan(manifest::Manifest const& m) {
    if (build_has_asan_flag(m.build) || build_has_asan_flag(m.build_debug) ||
        build_has_asan_flag(m.build_release)) {
        return true;
    }

    toolchain::Platform windows_x64{.os = "windows", .arch = "x86_64"};
    for (auto const& ts : m.target_sections) {
        if (!manifest::eval_predicate(ts.predicate, windows_x64))
            continue;
        if (build_has_asan_flag(ts.build) || build_has_asan_flag(ts.build_debug) ||
            build_has_asan_flag(ts.build_release)) {
            return true;
        }
    }
    return false;
}

void emit_windows_asan_runtime_support(std::ostringstream& out,
                                       manifest::Manifest const& m) {
    if (!build_has_windows_asan(m))
        return;

    out << R"(function(exon_copy_windows_asan_runtime target)
    if(NOT WIN32)
        return()
    endif()

    get_target_property(_exon_target_type ${target} TYPE)
    if(NOT _exon_target_type STREQUAL "EXECUTABLE")
        return()
    endif()

    get_filename_component(_exon_compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(_exon_asan_runtime "")
    set(_exon_candidates
        "${_exon_compiler_dir}/clang_rt.asan_dynamic-x86_64.dll"
        "$ENV{VCToolsInstallDir}/bin/Hostx64/x64/clang_rt.asan_dynamic-x86_64.dll"
    )

    foreach(_exon_candidate IN LISTS _exon_candidates)
        if(_exon_candidate AND EXISTS "${_exon_candidate}")
            set(_exon_asan_runtime "${_exon_candidate}")
            break()
        endif()
    endforeach()

    if(NOT _exon_asan_runtime)
        message(FATAL_ERROR
            "Windows ASan runtime not found: clang_rt.asan_dynamic-x86_64.dll. "
            "Run from a Visual Studio developer environment or install the MSVC ASan runtime.")
    endif()

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_exon_asan_runtime}"
            "$<TARGET_FILE_DIR:${target}>/clang_rt.asan_dynamic-x86_64.dll")
endfunction()

)";
}

#if defined(_WIN32)
std::wstring widen_command(std::string_view command) {
    if (command.empty())
        return {};
    auto size = MultiByteToWideChar(CP_ACP, 0, command.data(),
                                    static_cast<int>(command.size()), nullptr, 0);
    if (size <= 0)
        throw std::runtime_error("failed to convert command line to UTF-16");
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_ACP, 0, command.data(), static_cast<int>(command.size()),
                        wide.data(), size);
    return wide;
}

CommandResult run_command_windows(std::string_view command,
                                  std::optional<std::chrono::milliseconds> timeout = {}) {
    auto wide = widen_command(command);
    std::vector<wchar_t> mutable_cmd(wide.begin(), wide.end());
    mutable_cmd.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info,
                                     sizeof(info))) {
            CloseHandle(job);
            job = nullptr;
        }
    }

    auto old_error_mode =
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    auto restore_error_mode = [&] { SetErrorMode(old_error_mode); };

    if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                        &si, &pi)) {
        auto error = GetLastError();
        if (job != nullptr)
            CloseHandle(job);
        restore_error_mode();
        throw std::runtime_error(
            std::format("failed to start process (GetLastError={})", error));
    }

    if (job != nullptr && !AssignProcessToJobObject(job, pi.hProcess)) {
        CloseHandle(job);
        job = nullptr;
    }

    auto wait_ms = timeout
        ? static_cast<DWORD>(std::min<std::chrono::milliseconds::rep>(
              timeout->count(), std::numeric_limits<DWORD>::max()))
        : INFINITE;
    auto wait_result = WaitForSingleObject(pi.hProcess, wait_ms);

    CommandResult result;
    if (wait_result == WAIT_TIMEOUT) {
        result.timed_out = true;
        if (job != nullptr) {
            TerminateJobObject(job, 124);
        } else {
            TerminateProcess(pi.hProcess, 124);
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        result.exit_code = 124;
    } else if (wait_result != WAIT_OBJECT_0) {
        auto error = GetLastError();
        if (job != nullptr)
            CloseHandle(job);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        restore_error_mode();
        throw std::runtime_error(
            std::format("failed while waiting for process (GetLastError={})", error));
    } else {
        DWORD exit_code = 0;
        if (!GetExitCodeProcess(pi.hProcess, &exit_code)) {
            auto error = GetLastError();
            if (job != nullptr)
                CloseHandle(job);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            restore_error_mode();
            throw std::runtime_error(
                std::format("failed to read process exit code (GetLastError={})", error));
        }
        result.exit_code = static_cast<int>(exit_code);
    }

    if (job != nullptr)
        CloseHandle(job);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    restore_error_mode();
    return result;
}
#endif

int normalize_unix_exit_status(int status) {
    if (status == -1)
        return 1;
    if ((status & 0x7f) == 0)
        return (status >> 8) & 0xff;
    if ((status & 0x7f) != 0x7f)
        return 128 + (status & 0x7f);
    return status;
}

CommandResult run_command(std::string_view command,
                          std::optional<std::chrono::milliseconds> timeout = {}) {
#if defined(_WIN32)
    return run_command_windows(command, timeout);
#else
    (void)timeout;
    auto rc = std::system(std::string{command}.c_str());
    return CommandResult{.exit_code = normalize_unix_exit_status(rc)};
#endif
}

struct SourceFiles {
    std::vector<std::string> cpp;  // .cpp files
    std::vector<std::string> cppm; // .cppm module files
};

SourceFiles collect_sources(std::filesystem::path const& src_dir) {
    SourceFiles sf;
    if (!std::filesystem::exists(src_dir))
        return sf;
    for (auto const& entry : std::filesystem::directory_iterator(src_dir)) {
        if (!entry.is_regular_file())
            continue;
        auto ext = entry.path().extension();
        auto path = std::filesystem::canonical(entry.path()).generic_string();
        if (ext == ".cpp")
            sf.cpp.push_back(path);
        else if (ext == ".cppm")
            sf.cppm.push_back(path);
    }
    std::ranges::sort(sf.cpp);
    std::ranges::sort(sf.cppm);
    return sf;
}

struct BuildFlags {
    std::string cxx_flags;
    std::string linker_flags;
    std::string standard_libraries; // static libs appended after objects (for Linux)
};

bool use_system_macos_runtime(toolchain::Toolchain const& tc) {
    return !tc.is_msvc && !tc.needs_stdlib_flag && !tc.sysroot.empty();
}

BuildFlags resolve_flags(toolchain::Toolchain const& tc, manifest::Manifest const& m,
                         bool release) {
    BuildFlags flags;

    // MSVC has no libc++ linker flags; rely on CMake defaults.
    if (tc.is_msvc)
        return flags;

    if (tc.stdlib_modules_json.empty() || m.standard < 23)
        return flags;

    // On macOS, keep using the system libc++/libc++abi at runtime. The
    // toolchain's stdlib modules are still used for `import std;`, but
    // injecting the LLVM runtime path makes the release binary crash at
    // startup with libc++abi initialization failures. We still add the
    // standard library names explicitly because clean CMake configures do not
    // reliably link libc++/libc++abi when using the intron clang toolchain.
    if (use_system_macos_runtime(tc)) {
        flags.linker_flags = "-lc++ -lc++abi";
        return flags;
    }

    auto libcxx_a = std::filesystem::path{tc.lib_dir} / "libc++.a";
    auto libcxxabi_a = std::filesystem::path{tc.lib_dir} / "libc++abi.a";
    bool has_static_libs = !tc.lib_dir.empty() && std::filesystem::exists(libcxx_a) &&
                           std::filesystem::exists(libcxxabi_a);

    if (release && has_static_libs && tc.needs_stdlib_flag) {
        // Linux release: statically link libc++ for portable binaries that
        // run without intron / system libc++. Must come BEFORE the
        // has_clang_config branch — intron ≥0.16.2 generates a clang config
        // file with -lc++, which would otherwise force the dynamic-link
        // path and produce a libc++.so.1-dependent binary.
        flags.cxx_flags = "--no-default-config -stdlib=libc++";
        flags.linker_flags = "--no-default-config -nostdlib++ -stdlib=libc++";
        flags.standard_libraries = std::format("{} {}", libcxx_a.string(), libcxxabi_a.string());
    } else if (!tc.has_clang_config && !tc.lib_dir.empty()) {
        // debug or fallback: dynamic libc++ from lib_dir
        if (tc.needs_stdlib_flag)
            flags.cxx_flags = "-stdlib=libc++";
        flags.linker_flags = std::format("-L{0} -Wl,-rpath,{0} -lc++ -lc++abi", tc.lib_dir);
        if (tc.needs_stdlib_flag)
            flags.linker_flags += " -stdlib=libc++";
    } else if (tc.needs_stdlib_flag) {
        flags.cxx_flags = "-stdlib=libc++";
    }

    return flags;
}

std::string configure_cmd(toolchain::Toolchain const& tc, manifest::Manifest const& m,
                          std::filesystem::path const& build_dir,
                          std::filesystem::path const& source_dir, bool release,
                          std::string_view vcpkg_toolchain = {},
                          std::filesystem::path const& vcpkg_manifest_dir = {},
                          std::string_view wasm_toolchain = {},
                          bool any_cmake_deps = false) {
    auto build_type = release ? "Release" : "Debug";
    auto cmd = std::format("{} -B {} -S {} -G Ninja -DCMAKE_BUILD_TYPE={}",
                           toolchain::shell_quote(tc.cmake),
                           toolchain::shell_quote(build_dir.string()),
                           toolchain::shell_quote(source_dir.string()), build_type);
    if (!tc.ninja.empty())
        cmd += std::format(" -DCMAKE_MAKE_PROGRAM={}", toolchain::shell_quote(tc.ninja));

    if (!wasm_toolchain.empty()) {
        // WASM cross-compilation: toolchain file handles compiler, sysroot, flags.
        // User .cppm modules and `import std;` both work via host clang-scan-deps
        // and the wasi-sdk libc++.modules.json (passed via stdlib_modules_json).
        cmd += std::format(" -DCMAKE_TOOLCHAIN_FILE={}", wasm_toolchain);
        // wasi-sdk lacks clang-scan-deps; use the host LLVM's copy
        if (!tc.cxx_compiler.empty())
            cmd += std::format(" -DCMAKE_CXX_COMPILER_CLANG_SCAN_DEPS={}",
                               toolchain::shell_quote(tc.cxx_compiler));
        // import std; on wasi-sdk pulls in libc++ headers including csetjmp
        // (needs setjmp/longjmp lowering, enabled by -mllvm -wasm-enable-sjlj)
        // and csignal (needs -D_WASI_EMULATED_SIGNAL). Without these flags
        // the std module fails to compile on bare wasm32-wasip1.
        // -fno-exceptions stays so user code remains exception-free.
        if (!tc.stdlib_modules_json.empty() && m.standard >= 23)
            cmd += std::format(" -DCMAKE_CXX_STDLIB_MODULES_JSON={}",
                               tc.stdlib_modules_json);
        std::string wasm_cxx =
            "-fno-exceptions -D_LIBCPP_NO_EXCEPTIONS"
            " -mllvm -wasm-enable-sjlj -D_WASI_EMULATED_SIGNAL";
        auto wasm_extra_cxx = user_cxxflags();
        if (!wasm_extra_cxx.empty()) {
            wasm_cxx += ' ';
            wasm_cxx += wasm_extra_cxx;
        }
        cmd += std::format(" \"-DCMAKE_CXX_FLAGS={}\"", wasm_cxx);
        // Link against wasi-emulated-signal so std::signal stubs resolve.
        std::string wasm_ld = "-lwasi-emulated-signal";
        auto wasm_extra_ld = user_ldflags();
        if (!wasm_extra_ld.empty()) {
            wasm_ld += ' ';
            wasm_ld += wasm_extra_ld;
        }
        cmd += std::format(" \"-DCMAKE_EXE_LINKER_FLAGS={}\"", wasm_ld);
        // WASI: let dlmalloc/sbrk manage heap growth via memory.grow.
        // Do NOT use --initial-heap or --initial-memory; these corrupt the
        // allocator's heap metadata and cause abort on the first allocation.
        return cmd;
    }

    if (!tc.cxx_compiler.empty())
        cmd += std::format(" -DCMAKE_CXX_COMPILER={}", toolchain::shell_quote(tc.cxx_compiler));
    if (!tc.sysroot.empty())
        cmd += std::format(" -DCMAKE_OSX_SYSROOT={}", tc.sysroot);
    if (!tc.stdlib_modules_json.empty() && m.standard >= 23)
        cmd += std::format(" -DCMAKE_CXX_STDLIB_MODULES_JSON={}", tc.stdlib_modules_json);
    if (!vcpkg_toolchain.empty()) {
        cmd += std::format(" -DCMAKE_TOOLCHAIN_FILE={}", vcpkg_toolchain);
        cmd += std::format(" -DVCPKG_MANIFEST_DIR={}", vcpkg_manifest_dir.string());
    }

    auto flags = resolve_flags(tc, m, release);
    auto extra_cxx = user_cxxflags();
    auto extra_ld = user_ldflags();

    auto combine = [](std::string_view a, std::string_view b) {
        if (a.empty()) return std::string{b};
        if (b.empty()) return std::string{a};
        return std::format("{} {}", a, b);
    };

    auto cxx = combine(flags.cxx_flags, extra_cxx);
    auto ld = combine(flags.linker_flags, extra_ld);

    // When cmake deps are present (e.g. Dawn) and the toolchain has a
    // clang config that resolves -lc++ to the system libc++, the
    // system libc++ may lack newer C++23 symbols. Add -L and -rpath to
    // the toolchain's lib dir so the linker finds the matching version,
    // plus -lc++abi (the config doesn't include it but Dawn needs it).
    if (any_cmake_deps && tc.has_clang_config && !tc.lib_dir.empty() &&
        !use_system_macos_runtime(tc)) {
        ld = combine(ld, std::format("-L{0} -Wl,-rpath,{0} -lc++abi", tc.lib_dir));
    }

    if (!cxx.empty())
        cmd += std::format(" -DCMAKE_CXX_FLAGS=\"{}\"", cxx);
    if (!ld.empty())
        cmd += std::format(" -DCMAKE_EXE_LINKER_FLAGS=\"{}\"", ld);
    if (!flags.standard_libraries.empty())
        cmd += std::format(" -DCMAKE_CXX_STANDARD_LIBRARIES=\"{}\"", flags.standard_libraries);

    return cmd;
}

std::vector<std::string> split_targets(std::string_view s) {
    std::vector<std::string> result;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
        std::size_t start = i;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
        if (start < i)
            result.emplace_back(s.substr(start, i - start));
    }
    return result;
}

void emit_cmake_preamble(std::ostringstream& out, manifest::Manifest const& m,
                          bool import_std) {
    if (import_std) {
        out << "cmake_minimum_required(VERSION 3.30)\n\n";
        out << std::format("set(CMAKE_CXX_STANDARD {})\n", m.standard);
        out << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n";
        out << "set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD \"451f2fe2-a8a2-47c3-bc32-94786d8fc91b\")\n";
        out << "set(CMAKE_CXX_MODULE_STD ON)\n";
        out << std::format("project({} LANGUAGES CXX)\n\n", m.name);
    } else {
        out << "cmake_minimum_required(VERSION 3.28)\n";
        out << std::format("project({} LANGUAGES CXX)\n\n", m.name);
        out << std::format("set(CMAKE_CXX_STANDARD {})\n", m.standard);
        out << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n\n";
    }
}

void emit_msvc_options(std::ostringstream& out) {
    out << "if(MSVC)\n";
    out << "    add_compile_options(/W4 /wd4996 /utf-8)\n";
    out << "    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)\n";
    out << "endif()\n\n";
}

// Emit FetchContent blocks for [dependencies.cmake] entries. These are
// raw CMake projects (like Dawn) that need custom options + targets.
// Emitted BEFORE regular FetchContent deps so their targets are available.
void emit_cmake_deps(std::ostringstream& out, manifest::Manifest const& m, bool with_dev) {
    bool any = !m.cmake_deps.empty() || (with_dev && !m.dev_cmake_deps.empty());
    if (!any) return;
    out << "include(FetchContent)\n";
    auto emit = [&](std::string const& name, manifest::CmakeDep const& dep) {
        for (auto const& [k, v] : dep.options)
            out << std::format("set({} {})\n", k, v);
        out << std::format("FetchContent_Declare({}\n", name);
        out << std::format("    GIT_REPOSITORY {}\n", dep.git);
        out << std::format("    GIT_TAG {}\n", dep.tag);
        if (dep.shallow)
            out << "    GIT_SHALLOW ON\n";
        // Skip submodule update — projects like Dawn use their own
        // dependency fetching (DAWN_FETCH_DEPENDENCIES) instead.
        out << "    GIT_SUBMODULES \"\"\n";
        out << "    EXCLUDE_FROM_ALL\n";
        out << ")\n";
        out << std::format("FetchContent_MakeAvailable({})\n\n", name);
    };
    for (auto const& [name, dep] : m.cmake_deps)
        emit(name, dep);
    if (with_dev) {
        for (auto const& [name, dep] : m.dev_cmake_deps)
            emit(name, dep);
    }
}

// Collect all cmake dep targets for target_link_libraries.
std::string collect_cmake_targets(manifest::Manifest const& m, bool dev) {
    std::string targets;
    auto add = [&](manifest::CmakeDep const& dep) {
        for (auto const& t : split_targets(dep.targets)) {
            if (!targets.empty()) targets += "\n    ";
            targets += t;
        }
    };
    for (auto const& [_, dep] : m.cmake_deps) add(dep);
    if (dev)
        for (auto const& [_, dep] : m.dev_cmake_deps) add(dep);
    return targets;
}

void emit_find_packages(std::ostringstream& out, manifest::Manifest const& m, bool with_dev) {
    for (auto const& [pkg, _] : m.find_deps)
        out << std::format("find_package({} REQUIRED)\n", pkg);
    if (with_dev) {
        for (auto const& [pkg, _] : m.dev_find_deps)
            out << std::format("find_package({} REQUIRED)\n", pkg);
    }
    if (!m.find_deps.empty() || (with_dev && !m.dev_find_deps.empty()))
        out << "\n";
}

std::string collect_find_targets(manifest::Manifest const& m, bool dev) {
    std::string targets;
    for (auto const& [_, tgt] : m.find_deps) {
        for (auto const& t : detail::split_targets(tgt)) {
            if (!targets.empty()) targets += "\n    ";
            targets += t;
        }
    }
    if (dev) {
        for (auto const& [_, tgt] : m.dev_find_deps) {
            for (auto const& t : detail::split_targets(tgt)) {
                if (!targets.empty()) targets += "\n    ";
                targets += t;
            }
        }
    }
    return targets;
}

} // namespace detail

std::string configure_command(toolchain::Toolchain const& tc, manifest::Manifest const& m,
                              std::filesystem::path const& build_dir,
                              std::filesystem::path const& source_dir, bool release,
                              std::string_view vcpkg_toolchain = {},
                              std::filesystem::path const& vcpkg_manifest_dir = {},
                              std::string_view wasm_toolchain = {},
                              bool any_cmake_deps = false) {
    return detail::configure_cmd(tc, m, build_dir, source_dir, release, vcpkg_toolchain,
                                 vcpkg_manifest_dir, wasm_toolchain, any_cmake_deps);
}

std::optional<std::string> stale_self_host_bootstrap_message(
    manifest::Manifest const& m, std::filesystem::path const& project_root,
    std::filesystem::path const& executable_path, toolchain::Platform const& host) {
    if (host.os != "macos" || executable_path.empty() ||
        !detail::is_exon_self_host_project(m, project_root)) {
        return std::nullopt;
    }

    std::error_code ec;
    if (!std::filesystem::exists(executable_path, ec) || ec)
        return std::nullopt;
    auto executable_time = std::filesystem::last_write_time(executable_path, ec);
    if (ec)
        return std::nullopt;

    std::filesystem::path newest_input;
    std::filesystem::file_time_type newest_input_time;
    bool has_newer_input = false;
    for (auto const& input : detail::self_host_bootstrap_inputs(project_root)) {
        auto input_time = std::filesystem::last_write_time(input, ec);
        if (ec) {
            ec.clear();
            continue;
        }
        if (input_time > executable_time && (!has_newer_input || input_time > newest_input_time)) {
            newest_input = input;
            newest_input_time = input_time;
            has_newer_input = true;
        }
    }
    if (!has_newer_input)
        return std::nullopt;

    auto executable_display = detail::display_path(executable_path, project_root);
    auto newest_input_display = detail::display_path(newest_input, project_root);
    return std::format(
        "stale bootstrap executable '{}': '{}' is newer. Rebuild the bootstrap binary before "
        "running `exon build`, `exon check`, `exon run`, or `exon test` in the exon repo.\n"
        "  cmake -S . -B build -G Ninja\n"
        "  cmake --build build --target exon --parallel 1\n"
        "If build/ points at an older source tree, remove it first.",
        executable_display, newest_input_display);
}

void ensure_fresh_self_host_bootstrap(manifest::Manifest const& m,
                                      std::filesystem::path const& project_root) {
    auto executable_path = detail::current_executable_path();
    if (!executable_path)
        return;
    if (auto message = stale_self_host_bootstrap_message(m, project_root, *executable_path,
                                                         toolchain::detect_host_platform())) {
        throw std::runtime_error(*message);
    }
}

bool needs_serial_cxx_modules_build(toolchain::Toolchain const& tc, manifest::Manifest const& m,
                                    std::string_view target = {}) {
    return target.empty() && !tc.is_msvc && !tc.sysroot.empty() &&
           !tc.stdlib_modules_json.empty() && m.standard >= 23;
}

std::string build_command(toolchain::Toolchain const& tc, manifest::Manifest const& m,
                          std::filesystem::path const& build_dir,
                          std::string_view cmake_target = {},
                          std::string_view target = {}) {
    auto cmd = std::format("{} --build {}", toolchain::shell_quote(tc.cmake),
                           toolchain::shell_quote(build_dir.string()));
    if (!cmake_target.empty())
        cmd += std::format(" --target {}", cmake_target);
    if (needs_serial_cxx_modules_build(tc, m, target))
        cmd += " --parallel 1";
    return cmd;
}

core::ProjectContext project_context(std::filesystem::path const& project_root,
                                     bool release = false,
                                     std::string_view target = {}) {
    auto profile = release ? "release" : "debug";
    auto is_wasm = !target.empty();
    auto exon_dir = project_root / ".exon";
    auto build_dir = is_wasm
        ? (exon_dir / target / profile)
        : (exon_dir / profile);

    return {
        .root = project_root,
        .exon_dir = exon_dir,
        .build_dir = build_dir,
        .profile = profile,
        .target = std::string{target},
        .is_wasm = is_wasm,
    };
}

std::string generate_cmake(manifest::Manifest const& m, std::filesystem::path const& project_root,
                           std::vector<fetch::FetchedDep> const& deps,
                           toolchain::Toolchain const& tc, bool with_tests = false,
                           bool release = false, std::string_view target = {}) {
    std::ostringstream out;

    bool import_std = (m.standard >= 23 &&
                       (!tc.stdlib_modules_json.empty() || tc.native_import_std));

    detail::emit_cmake_preamble(out, m, import_std);
    if (tc.is_msvc)
        detail::emit_msvc_options(out);
    detail::emit_windows_asan_runtime_support(out, m);

    // separate deps into regular and dev
    std::vector<fetch::FetchedDep const*> regular_deps, dev_deps;
    for (auto const& dep : deps) {
        if (dep.is_dev)
            dev_deps.push_back(&dep);
        else
            regular_deps.push_back(&dep);
    }

    // Pre-scan path deps: resolve their manifests for the current platform
    // so we can collect cmake deps and reuse them in emit_dep.
    auto platform = target.empty()
        ? toolchain::detect_host_platform()
        : *toolchain::platform_from_target(target);

    std::map<std::string, manifest::CmakeDep> all_cmake_deps = m.cmake_deps;
    std::map<std::string, manifest::CmakeDep> all_dev_cmake_deps =
        with_tests ? m.dev_cmake_deps : std::map<std::string, manifest::CmakeDep>{};
    std::map<std::string, manifest::Manifest> resolved_dep_manifests;

    for (auto const& dep : deps) {
        if (!dep.subdir.empty()) continue;
        auto dep_manifest_path = dep.path / "exon.toml";
        if (!std::filesystem::exists(dep_manifest_path)) continue;
        auto dep_m = manifest::system::load(dep_manifest_path.string());
        dep_m = manifest::resolve_for_platform(std::move(dep_m), platform);
        for (auto const& [k, v] : dep_m.cmake_deps)
            all_cmake_deps.emplace(k, v);
        if (with_tests)
            for (auto const& [k, v] : dep_m.dev_cmake_deps)
                all_dev_cmake_deps.emplace(k, v);
        resolved_dep_manifests.emplace(dep.path.string(), std::move(dep_m));
    }

    // [dependencies.cmake] — raw CMake projects (e.g. Dawn). Includes
    // cmake deps from path dependencies so their targets are available.
    {
        bool any = !all_cmake_deps.empty() || (with_tests && !all_dev_cmake_deps.empty());
        if (any) {
            out << "include(FetchContent)\n";
            auto emit_one = [&](std::string const& name, manifest::CmakeDep const& cd) {
                for (auto const& [k, v] : cd.options)
                    out << std::format("set({} {})\n", k, v);
                out << std::format("FetchContent_Declare({}\n", name);
                out << std::format("    GIT_REPOSITORY {}\n", cd.git);
                out << std::format("    GIT_TAG {}\n", cd.tag);
                if (cd.shallow)
                    out << "    GIT_SHALLOW ON\n";
                out << "    GIT_SUBMODULES \"\"\n";
                out << "    EXCLUDE_FROM_ALL\n";
                out << ")\n";
                out << std::format("FetchContent_MakeAvailable({})\n\n", name);
            };
            for (auto const& [name, cd] : all_cmake_deps)
                emit_one(name, cd);
            if (with_tests)
                for (auto const& [name, cd] : all_dev_cmake_deps)
                    emit_one(name, cd);
        }
    }

    detail::emit_find_packages(out, m, with_tests);
    if (with_tests) {
        for (auto const& [pkg, _] : m.dev_find_deps)
            out << std::format("find_package({} REQUIRED)\n", pkg);
    }
    if (!m.find_deps.empty() || (with_tests && !m.dev_find_deps.empty()))
        out << "\n";

    // collect find_package + cmake imported targets for linkage (top-level only;
    // path dep cmake targets flow transitively through PUBLIC linkage)
    std::vector<std::string> find_targets, dev_find_targets;
    for (auto const& [_, tgts] : m.find_deps)
        for (auto& t : detail::split_targets(tgts))
            find_targets.push_back(std::move(t));
    for (auto const& [_, dep] : m.cmake_deps)
        for (auto& t : detail::split_targets(dep.targets))
            find_targets.push_back(std::move(t));
    for (auto const& [_, tgts] : m.dev_find_deps)
        for (auto& t : detail::split_targets(tgts))
            dev_find_targets.push_back(std::move(t));
    for (auto const& [_, dep] : m.dev_cmake_deps)
        for (auto& t : detail::split_targets(dep.targets))
            dev_find_targets.push_back(std::move(t));

    // emit dependency as static library
    auto emit_dep = [&](fetch::FetchedDep const& dep) {
        // git+subdir deps: always use add_subdirectory on the member's own CMakeLists.txt
        if (!dep.subdir.empty()) {
            auto dep_cmake = dep.path / "CMakeLists.txt";
            if (!std::filesystem::exists(dep_cmake))
                throw std::runtime_error(std::format(
                    "git dep '{}': {}/CMakeLists.txt not found (run `exon sync` in the upstream "
                    "repo, or add a CMakeLists.txt to the subdir)", dep.name, dep.subdir));
            out << std::format("add_subdirectory({} {})\n\n",
                               std::filesystem::canonical(dep.path).generic_string(), dep.name);
            return;
        }

        // build-system = "cmake": use existing CMakeLists.txt directly
        {
            auto cached_it = resolved_dep_manifests.find(dep.path.string());
            if (cached_it != resolved_dep_manifests.end() &&
                cached_it->second.build_system == "cmake") {
                auto dep_cmake = dep.path / "CMakeLists.txt";
                if (!std::filesystem::exists(dep_cmake))
                    throw std::runtime_error(std::format(
                        "dependency '{}': build-system = \"cmake\" but no CMakeLists.txt found",
                        dep.name));
                out << std::format("add_subdirectory({} {})\n\n",
                    std::filesystem::canonical(dep.path).generic_string(), dep.name);
                return;
            }
        }

        auto dep_src = dep.path / "src";
        auto dep_sf = detail::collect_sources(dep_src);

        // Use pre-scanned resolved manifest for feature filtering and linking
        auto cached_it = resolved_dep_manifests.find(dep.path.string());

        // filter .cppm files by features if consumer selected specific features
        if (!dep.features.empty() && cached_it != resolved_dep_manifests.end()) {
            auto const& dep_m = cached_it->second;
            if (!dep_m.features.empty()) {
                auto modules = manifest::resolve_features(
                    dep_m.features, dep.features, dep.default_features);
                std::erase_if(dep_sf.cppm, [&](std::string const& path) {
                    auto stem = std::filesystem::path{path}.stem().string();
                    return !modules.contains(stem);
                });
            }
        }

        if (dep_sf.cpp.empty() && dep_sf.cppm.empty()) {
            auto dep_cmake = dep.path / "CMakeLists.txt";
            if (std::filesystem::exists(dep_cmake)) {
                out << std::format("add_subdirectory({} {})\n\n",
                                   std::filesystem::canonical(dep.path).generic_string(), dep.name);
                return;
            }
            throw std::runtime_error(std::format(
                "dependency '{}' has no source files in src/ and no CMakeLists.txt", dep.name));
        }

        out << std::format("add_library({})\n", dep.name);
        if (!dep_sf.cpp.empty()) {
            out << std::format("target_sources({} PRIVATE", dep.name);
            for (auto const& src : dep_sf.cpp)
                out << std::format("\n    {}", src);
            out << "\n)\n";
        }
        if (!dep_sf.cppm.empty()) {
            out << std::format(
                "target_sources({}\n    PUBLIC FILE_SET CXX_MODULES BASE_DIRS {} FILES", dep.name,
                std::filesystem::canonical(dep.path).generic_string());
            for (auto const& src : dep_sf.cppm)
                out << std::format("\n    {}", src);
            out << "\n)\n";
        }

        auto include_dir = dep.path / "include";
        if (std::filesystem::exists(include_dir)) {
            out << std::format("target_include_directories({} PUBLIC {})\n", dep.name,
                               std::filesystem::canonical(include_dir).generic_string());
        }

        // Link transitive deps: git deps + featured deps + cmake dep targets + path deps
        if (cached_it != resolved_dep_manifests.end()) {
            auto const& dep_m = cached_it->second;
            std::vector<std::string> link_targets;
            for (auto const& [sub_key, sub_ver] : dep_m.dependencies) {
                link_targets.push_back(sub_key.substr(sub_key.rfind('/') + 1));
            }
            for (auto const& [sub_key, sub_fdep] : dep_m.featured_deps) {
                link_targets.push_back(sub_key.substr(sub_key.rfind('/') + 1));
            }
            for (auto const& [_, cmake_dep] : dep_m.cmake_deps) {
                for (auto& t : detail::split_targets(cmake_dep.targets))
                    link_targets.push_back(std::move(t));
            }
            for (auto const& [sub_name, _] : dep_m.path_deps) {
                link_targets.push_back(sub_name);
            }
            if (!link_targets.empty()) {
                out << std::format("target_link_libraries({} PUBLIC", dep.name);
                for (auto const& t : link_targets)
                    out << std::format("\n    {}", t);
                out << "\n)\n";
            }
        }
        out << "\n";
    };

    // build regular dependencies
    for (auto const* dep : regular_deps)
        emit_dep(*dep);

    // build dev dependencies (test-only)
    if (with_tests) {
        for (auto const* dep : dev_deps)
            emit_dep(*dep);
    }

    // main project sources
    auto src_dir = project_root / "src";
    auto sf = detail::collect_sources(src_dir);
    if (sf.cpp.empty() && sf.cppm.empty()) {
        throw std::runtime_error("no source files found in src/");
    }

    // create shared module library if cppm files exist (shared between main and tests)
    auto modules_lib = std::format("{}-modules", m.name);
    bool has_modules = !sf.cppm.empty();

    if (has_modules) {
        // Diamond dependency guard: when multiple consumers
        // FetchContent_MakeAvailable the same library, CMake re-enters
        // this file. The early return prevents a duplicate target error.
        if (m.type == "lib")
            out << std::format("if(TARGET {})\n    return()\nendif()\n\n", m.name);
        out << std::format("add_library({})\n", modules_lib);
        out << std::format(
            "target_sources({}\n    PUBLIC FILE_SET CXX_MODULES BASE_DIRS {} FILES", modules_lib,
            std::filesystem::canonical(project_root).generic_string());
        for (auto const& src : sf.cppm)
            out << std::format("\n    {}", src);
        out << "\n)\n";

        if (!regular_deps.empty() || !find_targets.empty()) {
            out << std::format("target_link_libraries({} PUBLIC", modules_lib);
            for (auto const& dep : regular_deps)
                out << std::format("\n    {}", dep->name);
            for (auto const& t : find_targets)
                out << std::format("\n    {}", t);
            out << "\n)\n";
        }

        if (m.type == "lib") {
            auto include_dir = project_root / "include";
            if (std::filesystem::exists(include_dir)) {
                out << std::format("target_include_directories({} PUBLIC {})\n", modules_lib,
                                   std::filesystem::canonical(include_dir).generic_string());
            }
        }
        out << "\n";
    }

    // main target
    // lib with only .cppm files: alias the modules library
    if (m.type == "lib" && has_modules && sf.cpp.empty()) {
        out << std::format("add_library({} ALIAS {})\n", m.name, modules_lib);
    } else {
        if (m.type == "lib") {
            out << std::format("add_library({})\n", m.name);
        } else {
            out << std::format("add_executable({})\n", m.name);
        }
        if (!sf.cpp.empty()) {
            out << std::format("target_sources({} PRIVATE", m.name);
            for (auto const& src : sf.cpp)
                out << std::format("\n    {}", src);
            out << "\n)\n";
        }

        if (has_modules) {
            auto link_type = (m.type == "lib") ? "PUBLIC" : "PRIVATE";
            out << std::format("target_link_libraries({} {} {})\n", m.name, link_type, modules_lib);
        } else if (!regular_deps.empty() || !find_targets.empty()) {
            auto link_type = (m.type == "lib") ? "PUBLIC" : "PRIVATE";
            out << std::format("target_link_libraries({} {}", m.name, link_type);
            for (auto const& dep : regular_deps)
                out << std::format("\n    {}", dep->name);
            for (auto const& t : find_targets)
                out << std::format("\n    {}", t);
            out << "\n)\n";
        }
    }
    if (m.type != "lib" && detail::build_has_windows_asan(m))
        out << std::format("exon_copy_windows_asan_runtime({})\n", m.name);

    // compile definitions
    // EXON_PKG_NAME/VERSION are set per-source so our value wins over any
    // same-named macro inherited from a dependency's compile definitions
    // (every exon package exposes these macros). CMake applies inherited
    // target definitions after the target's own, so a target-level define
    // would be overridden.
    {
        auto& profile_defs = release ? m.defines_release : m.defines_debug;

        if (!sf.cpp.empty() || !sf.cppm.empty()) {
            out << "\nset_source_files_properties(\n";
            for (auto const& src : sf.cppm)
                out << std::format("    {}\n", src);
            for (auto const& src : sf.cpp)
                out << std::format("    {}\n", src);
            out << "    PROPERTIES COMPILE_DEFINITIONS\n";
            out << std::format("    \"EXON_PKG_NAME=\\\"{}\\\";EXON_PKG_VERSION=\\\"{}\\\"\"\n",
                               m.name, m.version);
            out << ")\n";
        }

        if (!m.defines.empty() || !profile_defs.empty()) {
            auto def_target = has_modules ? modules_lib : std::string{m.name};
            out << std::format("\ntarget_compile_definitions({} PUBLIC\n", def_target);
            for (auto const& [key, val] : m.defines)
                out << std::format("    {}=\"{}\"\n", key, val);
            for (auto const& [key, val] : profile_defs)
                out << std::format("    {}=\"{}\"\n", key, val);
            out << ")\n";
        }
    }

    // base [build] flags + per-target flags merged into m.build at resolve time.
    // PRIVATE so add_subdirectory consumers don't inherit via INTERFACE_COMPILE_OPTIONS.
    // Applied to every package-owned target plus every test executable so
    // sanitizer-instrumented libs and executables link cleanly with their tests.
    auto const& prof_b = release ? m.build_release : m.build_debug;
    auto emit_build_for = [&](std::string_view target) {
        if (!m.build.cxxflags.empty() || !prof_b.cxxflags.empty()) {
            out << std::format("\ntarget_compile_options({} PRIVATE", target);
            for (auto const& f : m.build.cxxflags)
                out << std::format("\n    {}", f);
            for (auto const& f : prof_b.cxxflags)
                out << std::format("\n    {}", f);
            out << "\n)\n";
        }
        if (!m.build.ldflags.empty() || !prof_b.ldflags.empty()) {
            out << std::format("\ntarget_link_options({} PRIVATE", target);
            for (auto const& f : m.build.ldflags)
                out << std::format("\n    {}", f);
            for (auto const& f : prof_b.ldflags)
                out << std::format("\n    {}", f);
            out << "\n)\n";
        }
    };
    {
        auto cxx_target = has_modules ? modules_lib : std::string{m.name};
        emit_build_for(cxx_target);
        if (has_modules && m.type != "lib")
            emit_build_for(m.name);
    }

    // test targets
    if (with_tests) {
        auto tests_dir = project_root / "tests";
        auto test_sf = detail::collect_sources(tests_dir);

        for (auto const& test_cpp : test_sf.cpp) {
            auto test_stem = std::filesystem::path{test_cpp}.stem().string();
            auto test_name = std::format("test-{}", test_stem);

            out << std::format("\nadd_executable({})\n", test_name);
            out << std::format("target_sources({} PRIVATE\n    {}\n)\n", test_name, test_cpp);

            if (has_modules || !dev_deps.empty() || !dev_find_targets.empty()) {
                out << std::format("target_link_libraries({} PRIVATE", test_name);
                if (has_modules)
                    out << std::format("\n    {}", modules_lib);
                for (auto const& dep : dev_deps)
                    out << std::format("\n    {}", dep->name);
                for (auto const& t : dev_find_targets)
                    out << std::format("\n    {}", t);
                out << "\n)\n";
            } else if (!regular_deps.empty() || !find_targets.empty()) {
                out << std::format("target_link_libraries({} PRIVATE", test_name);
                for (auto const& dep : regular_deps)
                    out << std::format("\n    {}", dep->name);
                for (auto const& t : find_targets)
                    out << std::format("\n    {}", t);
                out << "\n)\n";
            }

            emit_build_for(test_name);
            if (detail::build_has_windows_asan(m))
                out << std::format("exon_copy_windows_asan_runtime({})\n", test_name);
        }
    }

    return out.str();
}

std::string generate_portable_cmake(manifest::Manifest const& m,
                                    std::filesystem::path const& project_root,
                                    std::vector<fetch::FetchedDep> const& deps) {
    std::ostringstream out;
    out << "# Generated by exon. Do not edit manually.\n\n";

    bool import_std = (m.standard >= 23);
    detail::emit_cmake_preamble(out, m, import_std);
    detail::emit_msvc_options(out);
    detail::emit_windows_asan_runtime_support(out, m);

    auto root = project_root;

    // separate deps (git vs path, regular vs dev)
    std::vector<fetch::FetchedDep const*> p_git_regular, p_git_dev;
    std::vector<fetch::FetchedDep const*> p_path_regular, p_path_dev;
    for (auto const& dep : deps) {
        auto& bucket = dep.is_dev
                           ? (dep.is_path ? p_path_dev : p_git_dev)
                           : (dep.is_path ? p_path_regular : p_git_regular);
        bucket.push_back(&dep);
    }

    // [dependencies.cmake] — raw CMake projects
    if (!m.cmake_deps.empty() || !m.dev_cmake_deps.empty()) {
        out << "include(FetchContent)\n";
        detail::emit_cmake_deps(out, m, true);
    }

    // emit find_package() calls
    for (auto const& [pkg, _] : m.find_deps)
        out << std::format("find_package({} REQUIRED)\n", pkg);
    for (auto const& [pkg, _] : m.dev_find_deps)
        out << std::format("find_package({} REQUIRED)\n", pkg);
    if (!m.find_deps.empty() || !m.dev_find_deps.empty())
        out << "\n";

    // collect imported targets (find + cmake)
    std::vector<std::string> p_find, p_dev_find;
    for (auto const& [_, tgts] : m.find_deps)
        for (auto& t : detail::split_targets(tgts))
            p_find.push_back(std::move(t));
    for (auto const& [_, dep] : m.cmake_deps)
        for (auto& t : detail::split_targets(dep.targets))
            p_find.push_back(std::move(t));
    for (auto const& [_, tgts] : m.dev_find_deps)
        for (auto& t : detail::split_targets(tgts))
            p_dev_find.push_back(std::move(t));
    for (auto const& [_, dep] : m.dev_cmake_deps)
        for (auto& t : detail::split_targets(dep.targets))
            p_dev_find.push_back(std::move(t));

    // git deps via FetchContent
    auto emit_fetch = [&](std::vector<fetch::FetchedDep const*> const& dep_list) {
        for (auto const* dep : dep_list) {
            auto git_url = std::format("https://{}.git", dep->key);
            auto tag = dep->version.starts_with("v") ? dep->version
                                                      : std::format("v{}", dep->version);
            out << std::format("FetchContent_Declare({}\n", dep->name);
            out << std::format("    GIT_REPOSITORY {}\n", git_url);
            out << std::format("    GIT_TAG {}\n", tag);
            out << "    GIT_SHALLOW ON\n";
            if (!dep->subdir.empty())
                out << std::format("    SOURCE_SUBDIR {}\n", dep->subdir);
            out << ")\n";
        }
        for (auto const* dep : dep_list)
            out << std::format("FetchContent_MakeAvailable({})\n", dep->name);
    };

    if (!p_git_regular.empty() || !p_git_dev.empty()) {
        out << "include(FetchContent)\n";
        emit_fetch(p_git_regular);
        if (!p_git_dev.empty()) {
            out << "\n# dev-dependencies (test-only)\n";
            emit_fetch(p_git_dev);
        }
        out << "\n";
    }

    // path deps via add_subdirectory (binary_dir required for paths outside source tree)
    auto emit_subdirs = [&](std::vector<fetch::FetchedDep const*> const& dep_list) {
        for (auto const* dep : dep_list) {
            auto rel = std::filesystem::relative(dep->path, root).generic_string();
            out << std::format("add_subdirectory(${{CMAKE_CURRENT_SOURCE_DIR}}/{} "
                               "${{CMAKE_BINARY_DIR}}/_deps/{}-build)\n",
                               rel, dep->name);
        }
    };

    if (!p_path_regular.empty() || !p_path_dev.empty()) {
        emit_subdirs(p_path_regular);
        if (!p_path_dev.empty()) {
            out << "\n# dev-dependencies (test-only)\n";
            emit_subdirs(p_path_dev);
        }
        out << "\n";
    }

    // combined lists for link emission (order: git first, then path)
    std::vector<fetch::FetchedDep const*> p_regular = p_git_regular;
    p_regular.insert(p_regular.end(), p_path_regular.begin(), p_path_regular.end());
    std::vector<fetch::FetchedDep const*> p_dev = p_git_dev;
    p_dev.insert(p_dev.end(), p_path_dev.begin(), p_path_dev.end());

    // project sources (relative paths)
    auto src_dir = root / "src";
    auto sf = detail::collect_sources(src_dir);
    if (sf.cpp.empty() && sf.cppm.empty())
        throw std::runtime_error("no source files found in src/");
    auto to_rel = [&](std::string const& abs) -> std::string {
        return std::format("${{CMAKE_CURRENT_SOURCE_DIR}}/{}",
            std::filesystem::relative(abs, root).generic_string());
    };

    auto modules_lib = std::format("{}-modules", m.name);
    bool has_modules = !sf.cppm.empty();

    if (has_modules) {
        if (m.type == "lib")
            out << std::format("if(TARGET {})\n    return()\nendif()\n\n", m.name);
        out << std::format("add_library({})\n", modules_lib);
        out << std::format(
            "target_sources({}\n    PUBLIC FILE_SET CXX_MODULES BASE_DIRS ${{CMAKE_CURRENT_SOURCE_DIR}} FILES",
            modules_lib);
        for (auto const& src : sf.cppm)
            out << std::format("\n    {}", to_rel(src));
        out << "\n)\n";

        if (!p_regular.empty() || !p_find.empty()) {
            out << std::format("target_link_libraries({} PUBLIC", modules_lib);
            for (auto const* dep : p_regular)
                out << std::format("\n    {}", dep->name);
            for (auto const& t : p_find)
                out << std::format("\n    {}", t);
            out << "\n)\n";
        }

        if (m.type == "lib") {
            auto include_dir = root / "include";
            if (std::filesystem::exists(include_dir))
                out << std::format("target_include_directories({} PUBLIC ${{CMAKE_CURRENT_SOURCE_DIR}}/include)\n",
                                   modules_lib);
        }
        out << "\n";
    }

    // main target
    if (m.type == "lib" && has_modules && sf.cpp.empty()) {
        out << std::format("add_library({} ALIAS {})\n", m.name, modules_lib);
    } else {
        if (m.type == "lib")
            out << std::format("add_library({})\n", m.name);
        else
            out << std::format("add_executable({})\n", m.name);

        if (!sf.cpp.empty()) {
            out << std::format("target_sources({} PRIVATE", m.name);
            for (auto const& src : sf.cpp)
                out << std::format("\n    {}", to_rel(src));
            out << "\n)\n";
        }

        if (has_modules) {
            auto link_type = (m.type == "lib") ? "PUBLIC" : "PRIVATE";
            out << std::format("target_link_libraries({} {} {})\n", m.name, link_type, modules_lib);
        } else if (!p_regular.empty() || !p_find.empty()) {
            auto link_type = (m.type == "lib") ? "PUBLIC" : "PRIVATE";
            out << std::format("target_link_libraries({} {}", m.name, link_type);
            for (auto const* dep : p_regular)
                out << std::format("\n    {}", dep->name);
            for (auto const& t : p_find)
                out << std::format("\n    {}", t);
            out << "\n)\n";
        }
    }
    if (m.type != "lib" && detail::build_has_windows_asan(m))
        out << std::format("exon_copy_windows_asan_runtime({})\n", m.name);

    // compile definitions
    // EXON_PKG_NAME/VERSION are set per-source so our value wins over any
    // same-named macro inherited from a dependency's compile definitions
    // (every exon package exposes these macros). CMake applies inherited
    // target definitions after the target's own, so a target-level define
    // would be overridden.
    {
        if (!sf.cpp.empty() || !sf.cppm.empty()) {
            out << "\nset_source_files_properties(\n";
            for (auto const& src : sf.cppm)
                out << std::format("    {}\n", to_rel(src));
            for (auto const& src : sf.cpp)
                out << std::format("    {}\n", to_rel(src));
            out << "    PROPERTIES COMPILE_DEFINITIONS\n";
            out << std::format("    \"EXON_PKG_NAME=\\\"{}\\\";EXON_PKG_VERSION=\\\"{}\\\"\"\n",
                               m.name, m.version);
            out << ")\n";
        }

        if (!m.defines.empty()) {
            bool is_alias = (m.type == "lib" && has_modules && sf.cpp.empty());
            auto def_target = is_alias ? modules_lib : std::string{m.name};
            out << std::format("\ntarget_compile_definitions({} PUBLIC\n", def_target);
            for (auto const& [key, val] : m.defines)
                out << std::format("    {}=\"{}\"\n", key, val);
            out << ")\n";
        }
    }

    // Build flag emission. PRIVATE so consumers via add_subdirectory don't
    // inherit them through INTERFACE_COMPILE_OPTIONS — each project decides
    // its own flags.
    //
    // Applied to every package-owned target plus every test executable: when a
    // lib or executable is built with sanitizers
    // (e.g. -fsanitize=address,undefined), the final linked targets must also
    // carry the corresponding link options so the runtime symbols get pulled
    // in. PRIVATE on the modules lib alone is not enough for bin packages.
    auto link_target = has_modules ? modules_lib : std::string{m.name};
    auto emit_build_options_for = [&](std::string_view target,
                                      manifest::Build const& b,
                                      manifest::Build const& bd,
                                      manifest::Build const& br,
                                      std::string_view indent) {
        bool any_cxx = !b.cxxflags.empty() || !bd.cxxflags.empty() || !br.cxxflags.empty();
        bool any_ld = !b.ldflags.empty() || !bd.ldflags.empty() || !br.ldflags.empty();
        if (any_cxx) {
            out << std::format("\n{}target_compile_options({} PRIVATE", indent, target);
            for (auto const& f : b.cxxflags)
                out << std::format("\n{}    {}", indent, f);
            for (auto const& f : bd.cxxflags)
                out << std::format("\n{}    $<$<CONFIG:Debug>:{}>", indent, f);
            for (auto const& f : br.cxxflags)
                out << std::format("\n{}    $<$<CONFIG:Release>:{}>", indent, f);
            out << std::format("\n{})\n", indent);
        }
        if (any_ld) {
            out << std::format("\n{}target_link_options({} PRIVATE", indent, target);
            for (auto const& f : b.ldflags)
                out << std::format("\n{}    {}", indent, f);
            for (auto const& f : bd.ldflags)
                out << std::format("\n{}    $<$<CONFIG:Debug>:{}>", indent, f);
            for (auto const& f : br.ldflags)
                out << std::format("\n{}    $<$<CONFIG:Release>:{}>", indent, f);
            out << std::format("\n{})\n", indent);
        }
    };
    auto ts_has_build = [](manifest::TargetSection const& ts) {
        return !ts.build.cxxflags.empty() || !ts.build.ldflags.empty() ||
               !ts.build_debug.cxxflags.empty() || !ts.build_debug.ldflags.empty() ||
               !ts.build_release.cxxflags.empty() || !ts.build_release.ldflags.empty();
    };

    // Whether the manifest has any declarative build flags worth emitting.
    bool const base_has_build =
        !m.build.cxxflags.empty() || !m.build.ldflags.empty() ||
        !m.build_debug.cxxflags.empty() || !m.build_debug.ldflags.empty() ||
        !m.build_release.cxxflags.empty() || !m.build_release.ldflags.empty();
    bool any_ts_has_build = false;
    for (auto const& ts : m.target_sections) {
        if (ts_has_build(ts) && !manifest::predicate_to_cmake(ts.predicate).empty()) {
            any_ts_has_build = true;
            break;
        }
    }
    bool const has_any_build = base_has_build || any_ts_has_build;

    // Wrap all declarative [build] / [target.cfg.build] flags in
    // if(PROJECT_IS_TOP_LEVEL) so they only apply when this project is the
    // top-level cmake project. When a project is consumed via FetchContent
    // or add_subdirectory, sanitizer-instrumented .o files would otherwise
    // be archived into the static library and force every downstream
    // consumer to also link the sanitizer runtimes. Matches Cargo's rule
    // that [build] flags apply only to the workspace member that declares
    // them, not to its dependencies.
    auto emit_all_build_for = [&](std::string_view target) {
        if (!has_any_build)
            return;
        out << "\nif(PROJECT_IS_TOP_LEVEL)\n";
        emit_build_options_for(target, m.build, m.build_debug, m.build_release, "    ");
        for (auto const& ts : m.target_sections) {
            if (!ts_has_build(ts))
                continue;
            auto cmake_cond = manifest::predicate_to_cmake(ts.predicate);
            if (cmake_cond.empty())
                continue;
            out << std::format("\n    if({})\n", cmake_cond);
            emit_build_options_for(target, ts.build, ts.build_debug, ts.build_release, "        ");
            out << "    endif()\n";
        }
        out << "endif()\n";
    };
    emit_all_build_for(link_target);
    if (has_modules && m.type != "lib")
        emit_all_build_for(m.name);

    // platform-conditional sections (target.'cfg(...)' blocks) for non-build
    // bits — find_package and defines, both lib-only.
    for (auto const& ts : m.target_sections) {
        auto cmake_cond = manifest::predicate_to_cmake(ts.predicate);
        if (cmake_cond.empty())
            continue;
        bool has_content = !ts.find_deps.empty() || !ts.dev_find_deps.empty() ||
                           !ts.defines.empty() || !ts.defines_debug.empty() ||
                           !ts.defines_release.empty();
        if (!has_content)
            continue;

        out << std::format("\nif({})\n", cmake_cond);
        for (auto const& [pkg, _] : ts.find_deps)
            out << std::format("    find_package({} REQUIRED)\n", pkg);
        for (auto const& [pkg, _] : ts.dev_find_deps)
            out << std::format("    find_package({} REQUIRED)\n", pkg);

        // collect conditional link targets
        std::vector<std::string> cond_find;
        for (auto const& [_, tgts] : ts.find_deps)
            for (auto& t : detail::split_targets(tgts))
                cond_find.push_back(std::move(t));
        if (!cond_find.empty()) {
            out << std::format("    target_link_libraries({} PUBLIC", link_target);
            for (auto const& t : cond_find)
                out << std::format("\n        {}", t);
            out << "\n    )\n";
        }

        if (!ts.defines.empty()) {
            out << std::format("    target_compile_definitions({} PUBLIC", link_target);
            for (auto const& [key, val] : ts.defines)
                out << std::format("\n        {}=\"{}\"", key, val);
            out << "\n    )\n";
        }
        out << "endif()\n";
    }

    // test targets
    auto tests_dir = root / "tests";
    if (std::filesystem::exists(tests_dir)) {
        auto test_sf = detail::collect_sources(tests_dir);
        for (auto const& test_cpp : test_sf.cpp) {
            auto test_stem = std::filesystem::path{test_cpp}.stem().string();
            auto test_name = std::format("test-{}", test_stem);

            out << std::format("\nadd_executable({})\n", test_name);
            out << std::format("target_sources({} PRIVATE\n    {}\n)\n", test_name, to_rel(test_cpp));

            if (has_modules || !p_dev.empty() || !p_dev_find.empty()) {
                out << std::format("target_link_libraries({} PRIVATE", test_name);
                if (has_modules)
                    out << std::format("\n    {}", modules_lib);
                for (auto const* dep : p_dev)
                    out << std::format("\n    {}", dep->name);
                for (auto const& t : p_dev_find)
                    out << std::format("\n    {}", t);
                out << "\n)\n";
            } else if (!p_regular.empty() || !p_find.empty()) {
                out << std::format("target_link_libraries({} PRIVATE", test_name);
                for (auto const* dep : p_regular)
                    out << std::format("\n    {}", dep->name);
                for (auto const& t : p_find)
                    out << std::format("\n    {}", t);
                out << "\n)\n";
            }

            // Apply same build flags (base + per-target if() blocks) to the
            // test executable. Required for sanitizer-instrumented libs whose
            // tests must also be instrumented to satisfy runtime symbol refs.
            emit_all_build_for(test_name);
            if (detail::build_has_windows_asan(m))
                out << std::format("exon_copy_windows_asan_runtime({})\n", test_name);
        }
    }

    return out.str();
}

std::string generate_portable_cmake(manifest::Manifest const& m,
                                    std::vector<fetch::FetchedDep> const& deps) {
    return generate_portable_cmake(m, std::filesystem::current_path(), deps);
}

namespace detail {

BuildPlan base_plan(BuildRequest const& request, bool with_tests = false) {
    BuildPlan plan{
        .project = request.project,
        .configured = request.configured,
        .timeout = request.timeout,
    };

    plan.writes.push_back({
        .path = request.project.exon_dir / "CMakeLists.txt",
        .content = generate_cmake(request.manifest, request.project.root, request.deps,
                                  request.toolchain, with_tests, request.release,
                                  request.project.target),
    });

    if (request.manifest.sync_cmake_in_root) {
        plan.writes.push_back({
            .path = request.project.root / "CMakeLists.txt",
            .content = generate_portable_cmake(request.manifest, request.project.root,
                                               request.deps),
            .success_message = "synced CMakeLists.txt",
        });
    }

    plan.configure_steps.push_back({
        .cwd = request.project.root,
        .command = configure_command(request.toolchain, request.manifest,
                                     request.project.build_dir, request.project.exon_dir,
                                     request.release, request.vcpkg_toolchain.string(),
                                     request.project.exon_dir, request.wasm_toolchain_file,
                                     request.any_cmake_deps),
        .label = "configuring...",
    });

    return plan;
}

} // namespace detail

BuildPlan plan_build(BuildRequest const& request) {
    auto plan = detail::base_plan(request, false);

    plan.build_steps.push_back({
        .cwd = request.project.root,
        .command = build_command(request.toolchain, request.manifest,
                                 request.project.build_dir, {}, request.project.target),
        .label = "building...",
    });

    if (request.project.is_wasm) {
        plan.success_message = std::format("build succeeded: .exon/{}/{}/{}",
                                           request.project.target, request.project.profile,
                                           request.manifest.name);
    } else {
        plan.success_message = std::format("build succeeded: .exon/{}/{}",
                                           request.project.profile, request.manifest.name);
    }

    return plan;
}

BuildPlan plan_check(BuildRequest const& request) {
    auto plan = detail::base_plan(request, false);
    auto build_target = request.build_targets.empty()
        ? std::string{request.manifest.name}
        : request.build_targets.front();

    plan.build_steps.push_back({
        .cwd = request.project.root,
        .command = build_command(request.toolchain, request.manifest,
                                 request.project.build_dir, build_target,
                                 request.project.target),
        .label = "checking...",
    });
    plan.success_message = "check succeeded";

    return plan;
}

BuildPlan plan_test(BuildRequest const& request) {
    auto plan = detail::base_plan(request, true);
    plan.test_names = request.test_names;
    plan.wasm_runtime = request.wasm_runtime;

    for (std::size_t i = 0; i < request.build_targets.size(); ++i) {
        plan.build_steps.push_back({
            .cwd = request.project.root,
            .command = build_command(request.toolchain, request.manifest,
                                     request.project.build_dir, request.build_targets[i],
                                     request.project.target),
            .label = i == 0 ? "building tests..." : "",
        });
    }

    auto exe_suffix = std::string{request.project.is_wasm ? "" : toolchain::exe_suffix};
    for (auto const& name : request.test_names) {
        auto exe = request.project.build_dir / (name + exe_suffix);
        std::string run_cmd;
        if (request.project.is_wasm) {
            run_cmd = std::format("{} -W exceptions=y {}",
                                  toolchain::shell_quote(request.wasm_runtime),
                                  toolchain::shell_quote(exe.string()));
        } else {
            run_cmd = toolchain::shell_quote(exe.string());
        }

        plan.run_steps.push_back({
            .cwd = request.project.root,
            .command = std::move(run_cmd),
            .timeout = request.timeout,
            .label = name,
        });
    }

    return plan;
}

int run_process(std::string_view command,
                std::optional<std::chrono::milliseconds> timeout = {}) {
    return detail::run_command(command, timeout).exit_code;
}

} // namespace build
