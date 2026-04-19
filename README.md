# Exon

A package manager for C++. Inspired by Cargo.

## Supported Platforms

| Platform | Architecture |
|----------|-------------|
| macOS | ARM64 (Apple Silicon) |
| Linux | x86_64 |
| Linux | aarch64 |
| Windows | x86_64 (MSVC) |

## Installation

### Script

```sh
curl -fsSL https://raw.githubusercontent.com/misut/exon/main/install.sh | sh
```

### mise

```sh
mise install "vfox:misut/mise-exon@latest"
mise use "vfox:misut/mise-exon@latest"
```

<details>
<summary>Build from source</summary>

Requires LLVM with libc++ modules: [Homebrew LLVM](https://formulae.brew.sh/formula/llvm) (macOS) or [LLVM 20+](https://apt.llvm.org/) (Linux). On **Windows**, `intron install` uses this repo's `.intron.toml` to provision MSVC, CMake, and Ninja.

```sh
# bootstrap (uses FetchContent for tomlcpp)
cmake -B build -G Ninja
cmake --build build --target exon

# self-host
./build/exon build
```

On macOS, use `cmake --build build --target exon --parallel 1` for the bootstrap build. Self-hosted `exon build`, `exon check`, and `exon test` serialize the build automatically on macOS when `import std;` is enabled. If you change `src/build.cppm`, `src/toolchain.cppm`, or `exon.toml` in the `exon` repo, rebuild `build/exon` before self-hosting again. If `build/` still points at an older source tree, remove it and rerun the bootstrap commands.

On Windows, a regular PowerShell session is enough:

```powershell
intron install
Invoke-Expression ((intron env) -join "`n")
cmake -B build -G Ninja
cmake --build build --target exon
.\build\exon.exe build
```

A Visual Studio Developer Command Prompt also works, but it is no longer required when `intron` is available.
</details>

## Quick Start

```sh
exon init
# edit exon.toml and src/main.cpp
exon run
```

```
created exon.toml (bin)
configuring...
building...
build succeeded: .exon/debug/hello
running hello...

hello, world!
```

## Commands

| Command | Description |
|---------|-------------|
| `exon init [--lib\|--workspace] [name]` | Create a new package or workspace |
| `exon new --lib\|--bin <name>` | Create a new workspace member from the workspace root |
| `exon info` | Show package information |
| `exon build [--release] [--target <t>] [--member a,b] [--exclude x,y] [--output human\|wrapped\|raw]` | Build the project or selected workspace members |
| `exon check [--release] [--target <t>] [--member a,b] [--exclude x,y]` | Check syntax without linking |
| `exon run [--release] [--target <t>] [--member <name>]` | Build and run |
| `exon debug [--release] [--debugger auto\|lldb\|gdb\|devenv\|cdb\|<path>] [--member <name>] [--exclude x,y] [-- <args...>]` | Build and open the selected native executable in a native debugger |
| `exon test [--release] [--target <t>] [--member a,b] [--exclude x,y] [--timeout <sec>] [--output human\|wrapped\|raw] [--show-output failed\|all\|none]` | Build and run tests |
| `exon clean [--member a,b] [--exclude x,y]` | Remove build artifacts |
| `exon add [--dev] <pkg> <ver>` | Add a git dependency |
| `exon add [--dev] --path <name> <path>` | Add a local path dependency |
| `exon add [--dev] --workspace <name>` | Add a workspace member dependency |
| `exon add [--dev] --vcpkg <name> <ver> [--features a,b]` | Add a vcpkg dependency |
| `exon add [--dev] --git <repo> --version <v> --subdir <dir>` | Add a git subdir dependency |
| `exon remove <pkg>` | Remove a dependency |
| `exon update [--member a,b] [--exclude x,y]` | Update dependencies |
| `exon sync [--member a,b] [--exclude x,y]` | Sync CMakeLists.txt with exon.toml |
| `exon fmt` | Format source files |

### Native debugging

`exon debug` is a native-only convenience launcher for host debuggers. It supports host executables only and does not support `--target wasm32-wasi` yet.

```sh
exon debug [--release] [--debugger auto|lldb|gdb|devenv|cdb|<path>] [--member <name>] [--exclude x,y] [-- <args...>]
```

```sh
exon debug
exon debug -- --port 8080
exon debug --debugger gdb -- input.txt
exon debug --debugger devenv.com
exon debug --debugger C:\Debuggers\cdb.exe -- input.txt
```

`--debugger` accepts `auto`, a built-in debugger name, or a path/command whose basename classifies as one of these supported debugger families:

- `lldb*`
- `gdb*`
- `devenv`, `devenv.exe`, `devenv.com`
- `cdb`, `cdb.exe`

Examples of accepted custom values include `lldb-18`, `gdb-14`, `devenv.com`, and `C:\Debuggers\cdb.exe`. Values outside those families, including `vsjitdebugger`, are rejected.

`auto` picks a debugger by host platform:

- macOS: `lldb`, then `gdb`
- Linux: `gdb`, then `lldb`
- Windows: `devenv`, then `cdb`, then `lldb`, then `gdb`

Debugger invocation follows the debugger's native CLI syntax:

- Visual Studio: `devenv /debugexe <executable> <args...>`
- CDB: `cdb <executable> <args...>`
- LLDB: `lldb -- <executable> <args...>`
- GDB: `gdb --args <executable> <args...>`

Windows-specific notes:

- `devenv` launches through the full Visual Studio IDE. `cdb` is the command-line debugger from the Windows debugger toolchain, which fits Build Tools / Debugging Tools-style environments better.
- `devenv` and `cdb` are only supported on Windows hosts.
- `devenv /debugexe` can misinterpret program arguments that start with `/` as Visual Studio switches. `exon debug --debugger devenv -- /flag` fails early with a targeted error.
- `exon debug --debugger auto -- /flag` skips `devenv` and continues with `cdb`, then `lldb`, then `gdb`.
- `exon debug --help` is not a separate command-specific help entry today; use the top-level `exon` usage text instead.

## exon.toml

```toml
[package]
name = "my-app"
version = "0.1.0"
description = "A sample project"
authors = ["misut"]
license = "MIT"
type = "bin"                              # "bin" or "lib"
standard = 23
build-system = "exon"                     # "exon" (default) or "cmake"
platforms = [                             # supported platforms (optional)
    { os = "linux" },
    { os = "macos", arch = "aarch64" },
    { os = "windows", arch = "x86_64" },
]

[dependencies]
"github.com/user/repo" = "0.1.0"          # git
member = { git = "github.com/user/monorepo", version = "0.1.0", subdir = "member" }

[dependencies.find]
Threads = "Threads::Threads"              # find_package()

[dependencies.path]
shared = "../shared"                       # local path

[dependencies.workspace]
core = true                                # workspace sibling

[dependencies.vcpkg]
fmt = "11.0.0"                             # vcpkg manifest

[dev-dependencies]
"github.com/user/testlib" = "0.1.0"       # test-only

[defines]
MY_FLAG = "value"

[defines.debug]
DEBUG_MODE = "1"

[build]                                    # raw compile/link flags
cxxflags = ["-Wall", "-Wextra"]

[build.debug]
cxxflags = ["-g", "-fsanitize=address"]
ldflags  = ["-fsanitize=address"]

[sync]
cmake-in-root = true                      # generate portable CMakeLists.txt
```

### Build system

The `build-system` field in `[package]` declares who manages the package's build definition.

- `"exon"` (default): exon scans `src/` for `.cpp`/`.cppm` files and generates cmake targets.
- `"cmake"`: the package provides its own `CMakeLists.txt`. Exon skips source scanning and uses `add_subdirectory()` directly. Useful for header-only wrappers or packages with custom cmake logic.

```toml
# header-only library with hand-written CMakeLists.txt
[package]
name = "metal-cpp"
type = "lib"
build-system = "cmake"
```

### Sync

The `[sync]` section controls what `exon sync` outputs.

- `cmake-in-root` (default `true`): generate a portable `CMakeLists.txt` at the project root for IDE and raw cmake consumers. Set to `false` for exon-only projects. The internal `.exon/CMakeLists.txt` is always generated regardless of this setting.

```toml
[sync]
cmake-in-root = false    # exon-only project, no root CMakeLists.txt needed
```

### Workspace root manifests

A workspace root declares members in `[workspace]` and may provide shared defaults for members:

```toml
[workspace]
members = ["core", "util", "app"]

[workspace.package]
version = "0.1.0"
authors = ["misut"]
license = "MIT"
standard = 23
build-system = "exon"

[workspace.build]
cxxflags = ["-Wall"]

[workspace.build.debug]
cxxflags = ["-fsanitize=address"]
ldflags = ["-fsanitize=address"]
```

`[workspace.package]` only fills missing member package fields; an explicit member value wins. `[workspace.build]`, `[workspace.build.debug]`, and `[workspace.build.release]` prepend shared flags to each selected member's own build flags.

Workspace roots are not runnable packages themselves. From the root:

- `exon build`, `exon check`, `exon test`, `exon sync`, `exon clean`, and `exon update` accept `--member a,b` and `--exclude x,y`
- `exon run --member <name>` runs a member package
- root builds use a unified graph under `.exon/workspace/<profile>` (or `.exon/workspace/<target>/<profile>` for cross-target builds)
- member execution order follows workspace dependency order, not the declaration order in `members = [...]`

Use `exon init --workspace` to create the root, then `exon new --lib <name>` or `exon new --bin <name>` from the root to add members.

### Build flags

The `[build]` section forwards raw flags to the compiler and linker. Profile-specific subsections (`[build.debug]`, `[build.release]`) merge on top of the base. Two environment variables append at the end so CI can inject flags without editing `exon.toml`:

```sh
EXON_CXXFLAGS="-fsanitize=address,undefined" exon test
EXON_LDFLAGS="-fsanitize=address,undefined" exon test
```

Common uses: sanitizers (`-fsanitize=address`), coverage (`--coverage`), warnings, profile-guided optimization. For `[defines]`-style preprocessor macros, prefer `[defines]` instead — exon escapes them per-target.

### Windows ASan

On Windows, declare ASan in `[build]` or `[target.'cfg(os = "windows")'.build]`:

```toml
[target.'cfg(os = "windows")'.build]
cxxflags = ["/fsanitize=address"]
ldflags = ["/fsanitize=address"]
```

When these flags are present, exon now copies `clang_rt.asan_dynamic-x86_64.dll` next to each built executable and test binary. This makes direct execution from `.exon/debug/` work without manually editing `PATH`.

`exon build` and `exon test` default to `human` output. `human` keeps the console focused on stage headers and final summaries, `wrapped` adds the same headers while still showing the underlying CMake/Ninja/test output, and `raw` keeps exon wrapping to a minimum.

`exon test --show-output failed` (default) only shows captured stdout/stderr for failing or timed-out test binaries. Use `--show-output all` to always print captured output, or `--show-output none` to suppress it entirely.

`exon test --timeout <sec>` is also available for long-running or hung tests. On Windows, timeout uses a native process runner that terminates the full child process tree instead of leaving stale `cmake`, `ninja`, or test processes behind.

### Windows Toolchain Troubleshooting

If a fresh Windows native configure fails with a CMake modules error such as:

```text
compiler does not provide a way to discover the import graph dependencies
```

check which compiler your current shell exported before assuming the repository is misconfigured.

```powershell
Invoke-Expression ((intron env) -join "`n")
Write-Host "CC=$env:CC"
Write-Host "CXX=$env:CXX"
where.exe cl.exe
where.exe clang-cl.exe
```

This often explains why local Windows and GitHub Actions Windows CI differ:

- the repository or CI may expect the MSVC `cl.exe` path
- your local `intron env` may have selected `clang-cl.exe` instead
- that mixed setup can surface CMake C++ modules or `import std` errors that do not appear in a `cl.exe`-based CI job

If the project expects MSVC, rerun with `cl` selected explicitly:

```powershell
$env:CC = "cl"
$env:CXX = "cl"
exon build
```

If `clang-cl` is intentional, verify that your current CMake and toolchain combination supports Windows C++ modules for the project you are building. With `intron 0.19.1`, Windows shells can still end up with an MSVC developer environment plus `CC` / `CXX` set to `clang-cl.exe`; newer `intron` releases prefer `cl.exe` when Windows MSVC is configured.

## Dependencies

Exon supports five kinds of dependencies, all with `[dev-dependencies.*]` variants that are only pulled in for `exon test`.

### Git dependencies

Fetched from GitHub (or any git remote) and built from source.

```toml
[dependencies]
"github.com/misut/tomlcpp" = "0.2.0"
```

The version becomes a git tag (`v0.2.0`). The short name (`tomlcpp`) becomes the CMake target.

```sh
exon add github.com/misut/tomlcpp 0.2.0
exon add --dev github.com/user/testlib 0.1.0
```

**Depending on a package inside a remote monorepo**: use inline-table syntax with `subdir`. The TOML key becomes the CMake target name.

```toml
[dependencies]
refl = { git = "github.com/misut/txn", version = "0.1.0", subdir = "refl" }
txn  = { git = "github.com/misut/txn", version = "0.2.0", subdir = "txn"  }
```

```sh
exon add --git github.com/misut/txn --version 0.1.0 --subdir refl
```

Exon clones the repo at tag `v{version}`, then uses `<repo>/<subdir>/CMakeLists.txt` (typically generated by `exon sync` on the upstream side) as the dep's build definition. Multiple subdirs sharing a `(repo, version)` share one clone. The TOML key must match the CMake target produced by the subdir's CMakeLists (for sync-managed repos, this is the member's `package.name`).

### find_package dependencies

Link against system-installed packages or anything `find_package()` can locate (conan/vcpkg-installed packages work too). The map key is the `find_package` name; the value is the imported target(s) to link. Space-separated values link multiple targets.

```toml
[dependencies.find]
Threads = "Threads::Threads"
ZLIB = "ZLIB::ZLIB"
fmt = "fmt::fmt fmt::fmt-header-only"

[dev-dependencies.find]
GTest = "GTest::gtest_main"
```

Generates:
```cmake
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)
find_package(fmt REQUIRED)

target_link_libraries(my-app-modules PUBLIC
    Threads::Threads
    ZLIB::ZLIB
    fmt::fmt
    fmt::fmt-header-only
)
```

### Path dependencies

Depend on any local directory containing an `exon.toml`. Paths are relative to the declaring manifest.

```toml
[dependencies.path]
shared = "../shared"
helpers = "../../vendor/helpers"
```

```sh
exon add --path shared ../shared
exon add --dev --path testlib ../testlib
```

Transitive path deps are resolved relative to the dep's own directory, so nested monorepos compose naturally. Path deps are not written to `exon.lock` (no version to pin).

### Workspace dependencies

Reference a sibling package in the same workspace by its `package.name`. Exon walks up from the current directory to find a workspace root (an `exon.toml` with `[workspace]`) and resolves the name against member manifests.

```toml
[dependencies.workspace]
core = true
utils = true
```

```sh
exon add --workspace core
```

See [docs/workspace.md](docs/workspace.md) for a full monorepo walkthrough, or open
[`examples/workspace/`](examples/workspace/) for a checked-in runnable workspace that
demonstrates shared defaults, workspace member dependencies, and root
`exon run --member <name>`.

### vcpkg dependencies

Install packages through [vcpkg](https://vcpkg.io) in manifest mode. Exon generates `.exon/vcpkg.json` and passes `CMAKE_TOOLCHAIN_FILE` to CMake, which installs packages automatically at configure time.

```toml
[dependencies.vcpkg]
fmt = "11.0.0"                                            # pinned via version>=
zlib = "*"                                                # baseline version
boost-asio = { version = "*", features = ["ssl"] }        # select vcpkg features
opencv = { features = ["contrib", "cuda"] }               # version omitted = "*"

[dependencies.find]
fmt = "fmt::fmt"                           # link target (required)
ZLIB = "ZLIB::ZLIB"

[dev-dependencies.vcpkg]
gtest = "*"

[dev-dependencies.find]
GTest = "GTest::gtest_main"
```

```sh
exon add --vcpkg fmt 11.0.0
exon add --vcpkg boost-asio '*' --features ssl
exon add --dev --vcpkg gtest '*'
```

Install (`[dependencies.vcpkg]`) and link (`[dependencies.find]`) are separate because vcpkg package names and CMake `find_package` names often differ (vcpkg `zlib` ↔ `find_package(ZLIB)`). Use inline table form (`{ version = "...", features = [...] }`) to enable optional vcpkg features. Exon requires `VCPKG_ROOT` or a standard install path (e.g. `/opt/vcpkg`, `~/vcpkg`, GitHub Actions `VCPKG_INSTALLATION_ROOT`); if vcpkg cannot be located, the build fails with a clear error.

## Platform targeting

Declare which platforms your package supports with `platforms` in `[package]`. Each entry is an inline table with `os` and/or `arch` fields; omitting a field acts as a wildcard.

```toml
[package]
name = "mylib"
version = "1.0.0"
platforms = [
    { os = "linux" },                        # all Linux architectures
    { os = "macos", arch = "aarch64" },       # macOS ARM64 only
    { os = "windows", arch = "x86_64" },      # Windows x64 only
]
```

Known values: `os` = `linux`, `macos`, `windows`; `arch` = `x86_64`, `aarch64`.

If the host platform does not match any entry, `exon build`/`run`/`test`/`check` fail early with a clear error. Omitting `platforms` entirely means the package supports all platforms (backward compatible with existing manifests).

### Platform-conditional dependencies and defines

Use `[target.'cfg(...)']` sections to declare dependencies or defines that only apply on certain platforms. The predicate syntax supports `os`, `arch`, comma-separated AND, and `not()`.

```toml
# Linux-only: link io_uring
[target.'cfg(os = "linux")'.dependencies.find]
LibUring = "LibUring::LibUring"

# Windows-only: vcpkg package
[target.'cfg(os = "windows")'.dependencies.vcpkg]
wil = "*"

# All except Windows
[target.'cfg(not(os = "windows"))'.dependencies.vcpkg]
libuv = "*"

# Platform-specific defines
[target.'cfg(os = "linux")'.defines]
IO_BACKEND = "io_uring"

[target.'cfg(os = "macos")'.defines]
IO_BACKEND = "kqueue"
```

All dependency subsections (`find`, `vcpkg`, `path`, `workspace`, inline-table git) and `defines` / `defines.debug` / `defines.release` are supported inside `[target.'cfg(...)']`. Non-matching sections are skipped entirely — their dependencies are not fetched.

## WebAssembly (WASM)

Build for WebAssembly using `--target wasm32-wasi`. Requires [wasi-sdk](https://github.com/WebAssembly/wasi-sdk) via intron or `WASI_SDK_PATH`.

```sh
# install wasi-sdk
intron install wasi-sdk 32
intron default wasi-sdk 32

# build
exon build --target wasm32-wasi

# run (requires wasmtime on PATH)
exon run --target wasm32-wasi
```

Output is placed in `.exon/wasm32-wasi/{debug,release}/`.

**Limitations:**

- `import std;` is not available (`#include` individual headers instead). User-defined `.cppm` modules work normally.
- C++ exceptions are disabled (`-fno-exceptions`).
- `vcpkg` and `find_package` dependencies are not supported for WASM targets.

```cpp
// WASM-compatible source (use #include, not import std)
#include <print>

int main() {
    std::println("hello, wasm!");
    return 0;
}
```

## Features

- **C++23 `import std;`** — automatically detected when `standard >= 23` and clang with libc++ modules is available
- **C++20 modules** — `.cppm` files in `src/` are handled as module sources
- **Transitive dependencies** — recursive resolution with cycle detection
- **Lock file** — `exon.lock` for reproducible builds (git deps only)
- **Incremental builds** — CMake configuration is cached and skipped when unchanged
- **Build profiles** — debug (default) and release (`--release`, statically links libc++ for portable binaries)
- **Dev dependencies** — `[dev-dependencies.*]` for test-only packages, excluded from builds
- **Five dependency kinds** — git, find_package, local path, workspace sibling, vcpkg
- **Workspaces** — monorepos with `[workspace] members = [...]`, shared `[workspace.package]` / `[workspace.build.*]` defaults, dependency-ordered root commands, and unified workspace builds
- **Compile definitions** — built-in (`EXON_PKG_NAME`, `EXON_PKG_VERSION`) and user-defined via `[defines]`
- **CMakeLists.txt sync** — `exon sync` generates a portable CMakeLists.txt for plain cmake builds (opt-out with `[sync] cmake-in-root = false`)
- **Custom cmake packages** — `build-system = "cmake"` delegates to a hand-written CMakeLists.txt instead of scanning `src/`
- **Syntax check** — `exon check` compiles modules without linking for fast feedback
- **Self-hosting** — exon builds itself with `exon build`
- **Cross-platform** — macOS (ARM64), Linux (x86_64, aarch64), and Windows (x86_64, MSVC)
- **WebAssembly** — `--target wasm32-wasi` cross-compiles to WASM via wasi-sdk
- **Platform targeting** — `platforms = [{ os = "linux" }, ...]` declares supported platforms; build fails early on unsupported hosts

## License

MIT
