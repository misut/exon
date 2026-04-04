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
mise install exon@0.6.0
mise use exon@0.6.0
```

### Build from source

Requires LLVM with libc++ modules: [Homebrew LLVM](https://formulae.brew.sh/formula/llvm) (macOS) or [LLVM 20+](https://apt.llvm.org/) (Linux).

```sh
# bootstrap
git clone https://github.com/misut/tomlcpp.git /tmp/tomlcpp
cmake -B build -S .github/cmake -G Ninja -DTOMLCPP_DIR=/tmp/tomlcpp
cmake --build build

# self-host
./build/exon build
```

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
| `exon run [--release]` | Build and run |
| `exon test [--release]` | Build and run tests |
| `exon clean` | Remove build artifacts |
| `exon add <pkg> <ver>` | Add a dependency |
| `exon remove <pkg>` | Remove a dependency |
| `exon update` | Update dependencies |
| `exon fmt` | Format source files |

## exon.toml

```toml
[package]
name = "my-app"
version = "0.1.0"
description = "A sample project"
authors = ["misut"]
license = "MIT"
type = "bin"
standard = 23

[dependencies]
"github.com/user/repo" = "0.1.0"
```

## Features

- **C++23 `import std;`** — automatically detected when `standard >= 23` and clang with libc++ modules is available
- **C++20 modules** — `.cppm` files in `src/` are handled as module sources
- **Transitive dependencies** — recursive resolution with cycle detection
- **Lock file** — `exon.lock` for reproducible builds
- **Incremental builds** — CMake configuration is cached and skipped when unchanged
- **Build profiles** — debug (default) and release (`--release`)
- **Git-based registry** — fetches packages from GitHub repositories
- **Self-hosting** — exon builds itself with `exon build`
- **Cross-platform** — macOS (ARM64) and Linux (x86_64, aarch64)

## License

MIT
