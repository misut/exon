export module build;
import std;
import toml;
import manifest;
import toolchain;
import fetch;
import vcpkg;

export namespace build {

// Join user-supplied compile/link flags from [build] sections + env var.
// Order: manifest [build] (always) → manifest [build.debug|release] (profile) → env var.
// Env var goes last so it overrides (CMake processes flags left-to-right).
std::string join_user_flags(std::vector<std::string> const& base,
                            std::vector<std::string> const& profile,
                            char const* env_name) {
    std::string out;
    auto append = [&](std::string_view flag) {
        if (flag.empty()) return;
        if (!out.empty()) out += ' ';
        out += flag;
    };
    for (auto const& f : base) append(f);
    for (auto const& f : profile) append(f);
    if (auto const* env = std::getenv(env_name); env && *env)
        append(env);
    return out;
}

std::string user_cxxflags(manifest::Manifest const& m, bool release) {
    auto const& profile = release ? m.build_release.cxxflags : m.build_debug.cxxflags;
    return join_user_flags(m.build.cxxflags, profile, "EXON_CXXFLAGS");
}

std::string user_ldflags(manifest::Manifest const& m, bool release) {
    auto const& profile = release ? m.build_release.ldflags : m.build_debug.ldflags;
    return join_user_flags(m.build.ldflags, profile, "EXON_LDFLAGS");
}

namespace detail {

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

BuildFlags resolve_flags(toolchain::Toolchain const& tc, manifest::Manifest const& m,
                         bool release) {
    BuildFlags flags;

    // MSVC has no libc++ linker flags; rely on CMake defaults.
    if (tc.is_msvc)
        return flags;

    if (tc.stdlib_modules_json.empty() || m.standard < 23)
        return flags;

    auto libcxx_a = std::filesystem::path{tc.lib_dir} / "libc++.a";
    auto libcxxabi_a = std::filesystem::path{tc.lib_dir} / "libc++abi.a";
    bool has_static_libs = !tc.lib_dir.empty() && std::filesystem::exists(libcxx_a) &&
                           std::filesystem::exists(libcxxabi_a);

    if (release && tc.has_clang_config) {
        // intron LLVM release: bypass clang config to avoid rpath to toolchain dir.
        // uses system libc++ (/usr/lib/) on macOS instead.
        flags.cxx_flags = "--no-default-config -stdlib=libc++";
        flags.linker_flags = "--no-default-config -lc++ -lc++abi";
        if (!tc.sysroot.empty()) {
            auto sysroot = std::format(" --sysroot={}", tc.sysroot);
            flags.cxx_flags += sysroot;
            flags.linker_flags += sysroot;
        }
    } else if (release && has_static_libs && tc.needs_stdlib_flag) {
        // Linux release: statically link libc++ for portable binaries
        flags.cxx_flags = "-stdlib=libc++";
        flags.linker_flags = "-nostdlib++ -stdlib=libc++";
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
                          std::string_view wasm_toolchain = {}) {
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
        auto wasm_extra_cxx = user_cxxflags(m, release);
        if (!wasm_extra_cxx.empty()) {
            wasm_cxx += ' ';
            wasm_cxx += wasm_extra_cxx;
        }
        cmd += std::format(" \"-DCMAKE_CXX_FLAGS={}\"", wasm_cxx);
        // Link against wasi-emulated-signal so std::signal stubs resolve.
        std::string wasm_ld = "-lwasi-emulated-signal";
        auto wasm_extra_ld = user_ldflags(m, release);
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
    auto extra_cxx = user_cxxflags(m, release);
    auto extra_ld = user_ldflags(m, release);

    auto combine = [](std::string_view a, std::string_view b) {
        if (a.empty()) return std::string{b};
        if (b.empty()) return std::string{a};
        return std::format("{} {}", a, b);
    };

    auto cxx = combine(flags.cxx_flags, extra_cxx);
    auto ld = combine(flags.linker_flags, extra_ld);

    if (!cxx.empty())
        cmd += std::format(" -DCMAKE_CXX_FLAGS=\"{}\"", cxx);
    if (!ld.empty())
        cmd += std::format(" -DCMAKE_EXE_LINKER_FLAGS=\"{}\"", ld);
    if (!flags.standard_libraries.empty())
        cmd += std::format(" -DCMAKE_CXX_STANDARD_LIBRARIES=\"{}\"", flags.standard_libraries);

    return cmd;
}

std::string read_file(std::filesystem::path const& path) {
    auto file = std::ifstream(path, std::ios::binary);
    if (!file)
        return {};
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

void ensure_intron_tools() {
    if (!std::filesystem::exists(".intron.toml"))
        return;

    // check if intron is available
#if defined(_WIN32)
    constexpr auto null_redirect = "intron help > NUL 2>&1";
#else
    constexpr auto null_redirect = "intron help > /dev/null 2>&1";
#endif
    if (std::system(null_redirect) != 0)
        return;

    auto table = toml::parse_file(".intron.toml");
    if (!table.contains("toolchain"))
        return;

    auto home = toolchain::home_dir();
    if (home.empty())
        return;
    auto intron_root = home / ".intron" / "toolchains";

    auto const& tools = table.at("toolchain").as_table();
    for (auto const& [tool, ver_val] : tools) {
        // Windows uses MSVC's native `import std;` — llvm/libc++ is not needed
        // and the clang+llvm Windows archive is ~800MB compressed, slow to install.
#if defined(_WIN32)
        if (tool == "llvm")
            continue;
#endif
        auto version = ver_val.as_string();
        auto tool_path = intron_root / tool / version;
        if (std::filesystem::exists(tool_path))
            continue;
        std::println("installing {} {}...", tool, version);
        auto cmd = std::format("intron install {} {}", tool, version);
        if (std::system(cmd.c_str()) != 0)
            std::println(std::cerr, "warning: failed to install {} {}", tool, version);
    }
}

// set up vcpkg.json + return toolchain file path, or empty path if no vcpkg deps
std::filesystem::path setup_vcpkg(manifest::Manifest const& m,
                                   std::filesystem::path const& exon_dir) {
    if (m.vcpkg_deps.empty() && m.dev_vcpkg_deps.empty())
        return {};
    auto root = vcpkg::require_root();
    vcpkg::write_manifest(m, exon_dir / "vcpkg.json");
    return root / "scripts" / "buildsystems" / "vcpkg.cmake";
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

std::string generate_cmake(manifest::Manifest const& m, std::filesystem::path const& project_root,
                           std::vector<fetch::FetchedDep> const& deps,
                           toolchain::Toolchain const& tc, bool with_tests = false,
                           bool release = false) {
    std::ostringstream out;

    bool import_std = (m.standard >= 23 &&
                       (!tc.stdlib_modules_json.empty() || tc.native_import_std));

    detail::emit_cmake_preamble(out, m, import_std);
    if (tc.is_msvc)
        detail::emit_msvc_options(out);

    // separate deps into regular and dev
    std::vector<fetch::FetchedDep const*> regular_deps, dev_deps;
    for (auto const& dep : deps) {
        if (dep.is_dev)
            dev_deps.push_back(&dep);
        else
            regular_deps.push_back(&dep);
    }

    detail::emit_find_packages(out, m, with_tests);
    if (with_tests) {
        for (auto const& [pkg, _] : m.dev_find_deps)
            out << std::format("find_package({} REQUIRED)\n", pkg);
    }
    if (!m.find_deps.empty() || (with_tests && !m.dev_find_deps.empty()))
        out << "\n";

    // collect find_package imported targets for linkage
    std::vector<std::string> find_targets, dev_find_targets;
    for (auto const& [_, tgts] : m.find_deps)
        for (auto& t : detail::split_targets(tgts))
            find_targets.push_back(std::move(t));
    for (auto const& [_, tgts] : m.dev_find_deps)
        for (auto& t : detail::split_targets(tgts))
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

        auto dep_src = dep.path / "src";
        auto dep_sf = detail::collect_sources(dep_src);

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

        auto dep_manifest_path = dep.path / "exon.toml";
        if (std::filesystem::exists(dep_manifest_path)) {
            auto dep_m = manifest::load(dep_manifest_path.string());
            if (!dep_m.dependencies.empty()) {
                out << std::format("target_link_libraries({} PUBLIC", dep.name);
                for (auto const& [sub_key, sub_ver] : dep_m.dependencies) {
                    auto sub_name = sub_key.substr(sub_key.rfind('/') + 1);
                    out << std::format("\n    {}", sub_name);
                }
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
        }
    }

    return out.str();
}

// compare generated content with existing lock file, write only if changed
bool sync_cmake(std::string const& content, std::filesystem::path const& output_dir) {
    std::filesystem::create_directories(output_dir);
    auto cmake_path = output_dir / "CMakeLists.txt";

    auto existing = detail::read_file(cmake_path);
    if (existing == content)
        return false;

    auto file = std::ofstream(cmake_path, std::ios::binary);
    if (!file)
        throw std::runtime_error(std::format("failed to create {}", cmake_path.string()));
    file << content;
    return true;
}

std::string generate_portable_cmake(manifest::Manifest const& m,
                                    std::vector<fetch::FetchedDep> const& deps) {
    std::ostringstream out;
    out << "# Generated by exon. Do not edit manually.\n\n";

    bool import_std = (m.standard >= 23);
    detail::emit_cmake_preamble(out, m, import_std);
    detail::emit_msvc_options(out);

    auto root = std::filesystem::current_path();

    // separate deps (git vs path, regular vs dev)
    std::vector<fetch::FetchedDep const*> p_git_regular, p_git_dev;
    std::vector<fetch::FetchedDep const*> p_path_regular, p_path_dev;
    for (auto const& dep : deps) {
        auto& bucket = dep.is_dev
                           ? (dep.is_path ? p_path_dev : p_git_dev)
                           : (dep.is_path ? p_path_regular : p_git_regular);
        bucket.push_back(&dep);
    }

    // emit find_package() calls
    for (auto const& [pkg, _] : m.find_deps)
        out << std::format("find_package({} REQUIRED)\n", pkg);
    for (auto const& [pkg, _] : m.dev_find_deps)
        out << std::format("find_package({} REQUIRED)\n", pkg);
    if (!m.find_deps.empty() || !m.dev_find_deps.empty())
        out << "\n";

    // collect imported targets
    std::vector<std::string> p_find, p_dev_find;
    for (auto const& [_, tgts] : m.find_deps)
        for (auto& t : detail::split_targets(tgts))
            p_find.push_back(std::move(t));
    for (auto const& [_, tgts] : m.dev_find_deps)
        for (auto& t : detail::split_targets(tgts))
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
    auto src_dir = std::filesystem::current_path() / "src";
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

    // platform-conditional sections (target.'cfg(...)' blocks)
    auto link_target = has_modules ? modules_lib : std::string{m.name};
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
        }
    }

    return out.str();
}

bool sync_root_cmake(manifest::Manifest const& m, std::vector<fetch::FetchedDep> const& deps) {
    auto content = generate_portable_cmake(m, deps);
    auto cmake_path = std::filesystem::current_path() / "CMakeLists.txt";

    auto existing = detail::read_file(cmake_path);
    if (existing == content)
        return false;

    auto file = std::ofstream(cmake_path, std::ios::binary);
    if (!file)
        throw std::runtime_error(std::format("failed to create {}", cmake_path.string()));
    file << content;
    std::println("synced CMakeLists.txt");
    return true;
}

int run(manifest::Manifest const& m, bool release = false, std::string_view target = {}) {
    detail::ensure_intron_tools();

    bool is_wasm = !target.empty();

    // reject incompatible dependency types for WASM targets
    if (is_wasm) {
        if (!m.vcpkg_deps.empty() || !m.dev_vcpkg_deps.empty())
            throw std::runtime_error("vcpkg dependencies are not supported for WASM targets");
        if (!m.find_deps.empty() || !m.dev_find_deps.empty())
            throw std::runtime_error(
                "find_package dependencies are not supported for WASM targets; "
                "use git or path dependencies instead");
    }

    auto project_root = std::filesystem::current_path();
    auto exon_dir = project_root / ".exon";
    auto profile = release ? "release" : "debug";
    auto build_dir = is_wasm ? (exon_dir / target / profile) : (exon_dir / profile);

    auto tc = toolchain::detect();

    std::string wasm_toolchain_file;
    if (is_wasm) {
        auto wasm_tc = toolchain::detect_wasm(target);
        wasm_toolchain_file = wasm_tc.cmake_toolchain;
        // import std; on wasm32-wasip1 needs -mllvm -wasm-enable-sjlj and
        // -D_WASI_EMULATED_SIGNAL because libc++'s csetjmp/csignal headers
        // include setjmp.h/signal.h which #error on bare wasi-sdk-32. Those
        // flags are injected by configure_cmd alongside -fno-exceptions.
        tc.stdlib_modules_json = wasm_tc.modules_json;
        tc.cxx_compiler = wasm_tc.scan_deps; // repurpose for host clang-scan-deps
        tc.sysroot.clear();
        tc.lib_dir.clear();
        tc.has_clang_config = false;
        tc.needs_stdlib_flag = false;
    }

    auto lock_path = (project_root / "exon.lock").string();
    auto fetch_result = fetch::fetch_all(m, lock_path);
    auto vcpkg_toolchain = is_wasm ? std::filesystem::path{} : detail::setup_vcpkg(m, exon_dir);

    auto content = generate_cmake(m, project_root, fetch_result.deps, tc, false, release);
    bool changed = sync_cmake(content, exon_dir);
    sync_root_cmake(m, fetch_result.deps);
    bool configured = std::filesystem::exists(build_dir / "build.ninja");

    if (changed || !configured) {
        std::println("configuring...");
        int rc = std::system(detail::configure_cmd(tc, m, build_dir, exon_dir, release,
                                                    vcpkg_toolchain.string(), exon_dir,
                                                    wasm_toolchain_file).c_str());
        if (rc != 0)
            return rc;
    }

    auto build_cmd = std::format("{} --build {}", toolchain::shell_quote(tc.cmake),
                                 toolchain::shell_quote(build_dir.string()));
    std::println("building...");
    int rc = std::system(build_cmd.c_str());
    if (rc != 0)
        return rc;

    if (is_wasm)
        std::println("build succeeded: .exon/{}/{}/{}", target, profile, m.name);
    else
        std::println("build succeeded: .exon/{}/{}", profile, m.name);
    return 0;
}

int run_check(manifest::Manifest const& m, bool release = false, std::string_view target = {}) {
    detail::ensure_intron_tools();

    bool is_wasm = !target.empty();

    if (is_wasm) {
        if (!m.vcpkg_deps.empty() || !m.dev_vcpkg_deps.empty())
            throw std::runtime_error("vcpkg dependencies are not supported for WASM targets");
        if (!m.find_deps.empty() || !m.dev_find_deps.empty())
            throw std::runtime_error(
                "find_package dependencies are not supported for WASM targets; "
                "use git or path dependencies instead");
    }

    auto project_root = std::filesystem::current_path();
    auto exon_dir = project_root / ".exon";
    auto profile = release ? "release" : "debug";
    auto build_dir = is_wasm ? (exon_dir / target / profile) : (exon_dir / profile);

    auto tc = toolchain::detect();

    std::string wasm_toolchain_file;
    if (is_wasm) {
        auto wasm_tc = toolchain::detect_wasm(target);
        wasm_toolchain_file = wasm_tc.cmake_toolchain;
        // import std; on wasm32-wasip1 needs sjlj + emulated signal flags
        // (injected by configure_cmd alongside -fno-exceptions).
        tc.stdlib_modules_json = wasm_tc.modules_json;
        tc.cxx_compiler = wasm_tc.scan_deps; // repurpose for host clang-scan-deps
        tc.sysroot.clear();
        tc.lib_dir.clear();
        tc.has_clang_config = false;
        tc.needs_stdlib_flag = false;
    }

    auto lock_path = (project_root / "exon.lock").string();
    auto fetch_result = fetch::fetch_all(m, lock_path);
    auto vcpkg_toolchain = is_wasm ? std::filesystem::path{} : detail::setup_vcpkg(m, exon_dir);

    auto content = generate_cmake(m, project_root, fetch_result.deps, tc, false, release);
    bool changed = sync_cmake(content, exon_dir);
    sync_root_cmake(m, fetch_result.deps);
    bool configured = std::filesystem::exists(build_dir / "build.ninja");

    if (changed || !configured) {
        std::println("configuring...");
        int rc = std::system(detail::configure_cmd(tc, m, build_dir, exon_dir, release,
                                                    vcpkg_toolchain.string(), exon_dir,
                                                    wasm_toolchain_file).c_str());
        if (rc != 0)
            return rc;
    }

    // Prefer building just the modules library (skips linking). If the
    // project has no .cppm files there is no <name>-modules target, so
    // fall back to building the main target.
    auto src_dir = project_root / "src";
    auto src_sf = detail::collect_sources(src_dir);
    auto check_target = src_sf.cppm.empty()
        ? std::string{m.name}
        : std::format("{}-modules", m.name);
    auto build_cmd = std::format("{} --build {} --target {}", toolchain::shell_quote(tc.cmake),
                                 toolchain::shell_quote(build_dir.string()), check_target);
    std::println("checking...");
    int rc = std::system(build_cmd.c_str());
    if (rc != 0)
        return rc;

    std::println("check succeeded");
    return 0;
}

int run_test(manifest::Manifest const& m, bool release = false, std::string_view target = {}) {
    detail::ensure_intron_tools();

    bool is_wasm = !target.empty();

    if (is_wasm) {
        if (!m.vcpkg_deps.empty() || !m.dev_vcpkg_deps.empty())
            throw std::runtime_error("vcpkg dependencies are not supported for WASM targets");
        if (!m.find_deps.empty() || !m.dev_find_deps.empty())
            throw std::runtime_error(
                "find_package dependencies are not supported for WASM targets; "
                "use git or path dependencies instead");
    }

    auto project_root = std::filesystem::current_path();
    auto tests_dir = project_root / "tests";

    if (!std::filesystem::exists(tests_dir)) {
        std::println(std::cerr, "error: tests/ directory not found");
        return 1;
    }

    auto test_sf = detail::collect_sources(tests_dir);
    if (test_sf.cpp.empty()) {
        std::println(std::cerr, "error: no test files found in tests/");
        return 1;
    }

    auto exon_dir = project_root / ".exon";
    auto profile = release ? "release" : "debug";
    auto build_dir = is_wasm ? (exon_dir / target / profile) : (exon_dir / profile);

    auto tc = toolchain::detect();

    std::string wasm_toolchain_file;
    std::string wasm_runtime;
    if (is_wasm) {
        auto wasm_tc = toolchain::detect_wasm(target);
        wasm_toolchain_file = wasm_tc.cmake_toolchain;
        // import std; on wasm32-wasip1 needs sjlj + emulated signal flags
        // (injected by configure_cmd alongside -fno-exceptions).
        tc.stdlib_modules_json = wasm_tc.modules_json;
        tc.cxx_compiler = wasm_tc.scan_deps; // repurpose for host clang-scan-deps
        tc.sysroot.clear();
        tc.lib_dir.clear();
        tc.has_clang_config = false;
        tc.needs_stdlib_flag = false;

        wasm_runtime = toolchain::detect_wasm_runtime();
        if (wasm_runtime.empty())
            throw std::runtime_error(
                "wasmtime not found on PATH (install: https://wasmtime.dev)");
    }

    auto lock_path = (project_root / "exon.lock").string();
    auto fetch_result = fetch::fetch_all(m, lock_path, true);
    auto vcpkg_toolchain = is_wasm ? std::filesystem::path{} : detail::setup_vcpkg(m, exon_dir);

    auto content = generate_cmake(m, project_root, fetch_result.deps, tc, true, release);
    bool changed = sync_cmake(content, exon_dir);
    sync_root_cmake(m, fetch_result.deps);
    bool configured = std::filesystem::exists(build_dir / "build.ninja");

    if (changed || !configured) {
        std::println("configuring...");
        int rc = std::system(detail::configure_cmd(tc, m, build_dir, exon_dir, release,
                                                    vcpkg_toolchain.string(), exon_dir,
                                                    wasm_toolchain_file).c_str());
        if (rc != 0)
            return rc;
    }

    // collect test names
    std::vector<std::string> test_names;
    for (auto const& test_cpp : test_sf.cpp) {
        test_names.push_back(
            std::format("test-{}", std::filesystem::path{test_cpp}.stem().string()));
    }

    // build
    std::println("building tests...");
    for (auto const& name : test_names) {
        auto build_cmd = std::format("{} --build {} --target {}", toolchain::shell_quote(tc.cmake),
                                     toolchain::shell_quote(build_dir.string()), name);
        int rc = std::system(build_cmd.c_str());
        if (rc != 0)
            return rc;
    }

    // run
    std::println("running tests...\n");
    auto exe_suffix = std::string{is_wasm ? "" : toolchain::exe_suffix};
    int passed = 0;
    int failed = 0;
    for (auto const& name : test_names) {
        auto exe = build_dir / (name + exe_suffix);
        std::string run_cmd;
        if (is_wasm)
            // -W exceptions=y enables the WebAssembly exception-handling
            // proposal (Wasmtime ≥37). Required because import std; uses
            // wasm-sjlj which lowers to try_table/throw/throw_ref opcodes.
            run_cmd = std::format("{} -W exceptions=y {}",
                                  toolchain::shell_quote(wasm_runtime),
                                  toolchain::shell_quote(exe.string()));
        else
            run_cmd = toolchain::shell_quote(exe.string());
        int rc = std::system(run_cmd.c_str());
        if (rc == 0) {
            std::println("  {} ... ok", name);
            ++passed;
        } else {
            std::println("  {} ... FAILED", name);
            ++failed;
        }
    }

    std::println("");
    if (failed > 0) {
        std::println("{} passed, {} failed", passed, failed);
        return 1;
    }
    std::println("all {} tests passed", passed);
    return 0;
}

} // namespace build
