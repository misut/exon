export module build;
import std;
import toml;
import manifest;
import toolchain;
import fetch;
import vcpkg;

export namespace build {

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
                          std::filesystem::path const& vcpkg_manifest_dir = {}) {
    auto build_type = release ? "Release" : "Debug";
    auto cmd = std::format("{} -B {} -S {} -G Ninja -DCMAKE_BUILD_TYPE={}",
                           toolchain::shell_quote(tc.cmake),
                           toolchain::shell_quote(build_dir.string()),
                           toolchain::shell_quote(source_dir.string()), build_type);
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
    if (!flags.cxx_flags.empty())
        cmd += std::format(" -DCMAKE_CXX_FLAGS=\"{}\"", flags.cxx_flags);
    if (!flags.linker_flags.empty())
        cmd += std::format(" -DCMAKE_EXE_LINKER_FLAGS=\"{}\"", flags.linker_flags);
    if (!flags.standard_libraries.empty())
        cmd += std::format(" -DCMAKE_CXX_STANDARD_LIBRARIES=\"{}\"", flags.standard_libraries);

    return cmd;
}

std::string read_file(std::filesystem::path const& path) {
    auto file = std::ifstream(path);
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

} // namespace detail

std::string generate_cmake(manifest::Manifest const& m, std::filesystem::path const& project_root,
                           std::vector<fetch::FetchedDep> const& deps,
                           toolchain::Toolchain const& tc, bool with_tests = false,
                           bool release = false) {
    std::ostringstream out;

    bool import_std = (m.standard >= 23 &&
                       (!tc.stdlib_modules_json.empty() || tc.native_import_std));

    if (import_std) {
        out << "cmake_minimum_required(VERSION 3.30)\n\n";
        out << std::format("set(CMAKE_CXX_STANDARD {})\n", m.standard);
        out << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n";
        out << "set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD \"451f2fe2-a8a2-47c3-bc32-94786d8fc91b\")\n";
        out << "set(CMAKE_CXX_MODULE_STD ON)\n";
        out << std::format("project({} LANGUAGES CXX)\n\n", m.name);
    } else {
        out << "cmake_minimum_required(VERSION 3.20)\n";
        out << std::format("project({} LANGUAGES CXX)\n\n", m.name);
        out << std::format("set(CMAKE_CXX_STANDARD {})\n", m.standard);
        out << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n\n";
    }

    // MSVC-specific compile options (keep warnings informative but non-fatal)
    if (tc.is_msvc) {
        out << "if(MSVC)\n";
        out << "    add_compile_options(/W4 /wd4996 /utf-8)\n";
        out << "    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)\n";
        out << "endif()\n\n";
    }

    // separate deps into regular and dev
    std::vector<fetch::FetchedDep const*> regular_deps, dev_deps;
    for (auto const& dep : deps) {
        if (dep.is_dev)
            dev_deps.push_back(&dep);
        else
            regular_deps.push_back(&dep);
    }

    // split a space-separated target list
    auto split_targets = [](std::string_view s) {
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
    };

    // emit find_package() calls
    for (auto const& [pkg, _] : m.find_deps)
        out << std::format("find_package({} REQUIRED)\n", pkg);
    if (with_tests) {
        for (auto const& [pkg, _] : m.dev_find_deps)
            out << std::format("find_package({} REQUIRED)\n", pkg);
    }
    if (!m.find_deps.empty() || (with_tests && !m.dev_find_deps.empty()))
        out << "\n";

    // collect find_package imported targets for linkage
    std::vector<std::string> find_targets, dev_find_targets;
    for (auto const& [_, tgts] : m.find_deps)
        for (auto& t : split_targets(tgts))
            find_targets.push_back(std::move(t));
    for (auto const& [_, tgts] : m.dev_find_deps)
        for (auto& t : split_targets(tgts))
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

    // compile definitions (applied to modules library so .cppm files can use them)
    {
        auto def_target = has_modules ? modules_lib : std::string{m.name};
        auto& profile_defs = release ? m.defines_release : m.defines_debug;

        out << std::format("\ntarget_compile_definitions({} PUBLIC\n", def_target);
        out << std::format("    EXON_PKG_NAME=\"{}\"\n", m.name);
        out << std::format("    EXON_PKG_VERSION=\"{}\"\n", m.version);
        for (auto const& [key, val] : m.defines)
            out << std::format("    {}=\"{}\"\n", key, val);
        for (auto const& [key, val] : profile_defs)
            out << std::format("    {}=\"{}\"\n", key, val);
        out << ")\n";
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

    auto file = std::ofstream(cmake_path);
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
    if (import_std) {
        out << "cmake_minimum_required(VERSION 3.30)\n\n";
        out << std::format("set(CMAKE_CXX_STANDARD {})\n", m.standard);
        out << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n";
        out << "set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD \"451f2fe2-a8a2-47c3-bc32-94786d8fc91b\")\n";
        out << "set(CMAKE_CXX_MODULE_STD ON)\n";
        out << std::format("project({} LANGUAGES CXX)\n\n", m.name);
    } else {
        out << "cmake_minimum_required(VERSION 3.20)\n";
        out << std::format("project({} LANGUAGES CXX)\n\n", m.name);
        out << std::format("set(CMAKE_CXX_STANDARD {})\n", m.standard);
        out << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n\n";
    }

    // MSVC-specific compile options (no-op on clang/gcc)
    out << "if(MSVC)\n";
    out << "    add_compile_options(/W4 /wd4996 /utf-8)\n";
    out << "    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)\n";
    out << "endif()\n\n";

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

    // split a space-separated target list
    auto split_targets = [](std::string_view s) {
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
    };

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
        for (auto& t : split_targets(tgts))
            p_find.push_back(std::move(t));
    for (auto const& [_, tgts] : m.dev_find_deps)
        for (auto& t : split_targets(tgts))
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
    {
        bool is_alias = (m.type == "lib" && has_modules && sf.cpp.empty());
        auto def_target = is_alias ? modules_lib : std::string{m.name};

        out << std::format("\ntarget_compile_definitions({} PUBLIC\n", def_target);
        out << std::format("    EXON_PKG_NAME=\"{}\"\n", m.name);
        out << std::format("    EXON_PKG_VERSION=\"{}\"\n", m.version);
        for (auto const& [key, val] : m.defines)
            out << std::format("    {}=\"{}\"\n", key, val);
        out << ")\n";
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

    auto file = std::ofstream(cmake_path);
    if (!file)
        throw std::runtime_error(std::format("failed to create {}", cmake_path.string()));
    file << content;
    std::println("synced CMakeLists.txt");
    return true;
}

int run(manifest::Manifest const& m, bool release = false) {
    detail::ensure_intron_tools();

    auto project_root = std::filesystem::current_path();
    auto exon_dir = project_root / ".exon";
    auto profile = release ? "release" : "debug";
    auto build_dir = exon_dir / profile;

    auto tc = toolchain::detect();

    auto lock_path = (project_root / "exon.lock").string();
    auto fetch_result = fetch::fetch_all(m, lock_path);
    auto vcpkg_toolchain = detail::setup_vcpkg(m, exon_dir);

    auto content = generate_cmake(m, project_root, fetch_result.deps, tc, false, release);
    bool changed = sync_cmake(content, exon_dir);
    sync_root_cmake(m, fetch_result.deps);
    bool configured = std::filesystem::exists(build_dir / "build.ninja");

    if (changed || !configured) {
        std::println("configuring...");
        int rc = std::system(detail::configure_cmd(tc, m, build_dir, exon_dir, release,
                                                    vcpkg_toolchain.string(), exon_dir).c_str());
        if (rc != 0)
            return rc;
    }

    auto build_cmd = std::format("{} --build {}", toolchain::shell_quote(tc.cmake),
                                 toolchain::shell_quote(build_dir.string()));
    std::println("building...");
    int rc = std::system(build_cmd.c_str());
    if (rc != 0)
        return rc;

    std::println("build succeeded: .exon/{}/{}", profile, m.name);
    return 0;
}

int run_check(manifest::Manifest const& m, bool release = false) {
    detail::ensure_intron_tools();

    auto project_root = std::filesystem::current_path();
    auto exon_dir = project_root / ".exon";
    auto profile = release ? "release" : "debug";
    auto build_dir = exon_dir / profile;

    auto tc = toolchain::detect();

    auto lock_path = (project_root / "exon.lock").string();
    auto fetch_result = fetch::fetch_all(m, lock_path);
    auto vcpkg_toolchain = detail::setup_vcpkg(m, exon_dir);

    auto content = generate_cmake(m, project_root, fetch_result.deps, tc, false, release);
    bool changed = sync_cmake(content, exon_dir);
    sync_root_cmake(m, fetch_result.deps);
    bool configured = std::filesystem::exists(build_dir / "build.ninja");

    if (changed || !configured) {
        std::println("configuring...");
        int rc = std::system(detail::configure_cmd(tc, m, build_dir, exon_dir, release,
                                                    vcpkg_toolchain.string(), exon_dir).c_str());
        if (rc != 0)
            return rc;
    }

    auto target = std::format("{}-modules", m.name);
    auto build_cmd = std::format("{} --build {} --target {}", toolchain::shell_quote(tc.cmake),
                                 toolchain::shell_quote(build_dir.string()), target);
    std::println("checking...");
    int rc = std::system(build_cmd.c_str());
    if (rc != 0)
        return rc;

    std::println("check succeeded");
    return 0;
}

int run_test(manifest::Manifest const& m, bool release = false) {
    detail::ensure_intron_tools();

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
    auto build_dir = exon_dir / profile;

    auto tc = toolchain::detect();

    auto lock_path = (project_root / "exon.lock").string();
    auto fetch_result = fetch::fetch_all(m, lock_path, true);
    auto vcpkg_toolchain = detail::setup_vcpkg(m, exon_dir);

    auto content = generate_cmake(m, project_root, fetch_result.deps, tc, true, release);
    bool changed = sync_cmake(content, exon_dir);
    sync_root_cmake(m, fetch_result.deps);
    bool configured = std::filesystem::exists(build_dir / "build.ninja");

    if (changed || !configured) {
        std::println("configuring...");
        int rc = std::system(detail::configure_cmd(tc, m, build_dir, exon_dir, release,
                                                    vcpkg_toolchain.string(), exon_dir).c_str());
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
    int passed = 0;
    int failed = 0;
    for (auto const& name : test_names) {
        auto exe = build_dir / (name + std::string{toolchain::exe_suffix});
        int rc = std::system(toolchain::shell_quote(exe.string()).c_str());
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
