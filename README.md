# Exon

A package manager for C++. Inspired by Cargo.

## Supported Platforms

| Platform | Architecture |
|----------|-------------|
| macOS | ARM64 (Apple Silicon) |
| Linux | x86_64 |
| Linux | aarch64 |

## Installation

### Script

```sh
curl -fsSL https://raw.githubusercontent.com/misut/exon/main/install.sh | sh
```

### mise

```sh
mise plugin add exon https://github.com/misut/mise-exon.git
mise install exon@0.11.0
mise use exon@0.11.0
```

<details>
<summary>Build from source</summary>

Requires LLVM with libc++ modules: [Homebrew LLVM](https://formulae.brew.sh/formula/llvm) (macOS) or [LLVM 20+](https://apt.llvm.org/) (Linux).

```sh
# bootstrap
git clone https://github.com/misut/tomlcpp.git /tmp/tomlcpp
cmake -B build -S .github/cmake -G Ninja -DTOMLCPP_DIR=/tmp/tomlcpp
cmake --build build

# self-host
./build/exon build
```
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
| `exon init [--lib]` | Create a new project |
| `exon info` | Show package information |
| `exon build [--release]` | Build the project |
| `exon check [--release]` | Check syntax without linking |
| `exon run [--release]` | Build and run |
| `exon test [--release]` | Build and run tests |
| `exon clean` | Remove build artifacts |
| `exon add [--dev] <pkg> <ver>` | Add a git dependency |
| `exon add [--dev] --path <name> <path>` | Add a local path dependency |
| `exon add [--dev] --workspace <name>` | Add a workspace member dependency |
| `exon remove <pkg>` | Remove a dependency |
| `exon update` | Update dependencies |
| `exon sync` | Sync CMakeLists.txt with exon.toml |
| `exon fmt` | Format source files |

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

[dependencies]
"github.com/user/repo" = "0.1.0"          # git

[dependencies.find]
Threads = "Threads::Threads"              # find_package()

[dependencies.path]
shared = "../shared"                       # local path

[dependencies.workspace]
core = true                                # workspace sibling

[dev-dependencies]
"github.com/user/testlib" = "0.1.0"       # test-only

[defines]
MY_FLAG = "value"

[defines.debug]
DEBUG_MODE = "1"
```

## Dependencies

Exon supports four kinds of dependencies, all with `[dev-dependencies.*]` variants that are only pulled in for `exon test`.

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

See [docs/workspace.md](docs/workspace.md) for a full monorepo walkthrough.

## Features

- **C++23 `import std;`** — automatically detected when `standard >= 23` and clang with libc++ modules is available
- **C++20 modules** — `.cppm` files in `src/` are handled as module sources
- **Transitive dependencies** — recursive resolution with cycle detection
- **Lock file** — `exon.lock` for reproducible builds (git deps only)
- **Incremental builds** — CMake configuration is cached and skipped when unchanged
- **Build profiles** — debug (default) and release (`--release`, statically links libc++ for portable binaries)
- **Dev dependencies** — `[dev-dependencies.*]` for test-only packages, excluded from builds
- **Four dependency kinds** — git, find_package, local path, workspace sibling
- **Workspaces** — monorepos with `[workspace] members = [...]` and member-to-member references
- **Compile definitions** — built-in (`EXON_PKG_NAME`, `EXON_PKG_VERSION`) and user-defined via `[defines]`
- **CMakeLists.txt sync** — `exon sync` generates a portable CMakeLists.txt for plain cmake builds
- **Syntax check** — `exon check` compiles modules without linking for fast feedback
- **Self-hosting** — exon builds itself with `exon build`
- **Cross-platform** — macOS (ARM64) and Linux (x86_64, aarch64)

## License

MIT
