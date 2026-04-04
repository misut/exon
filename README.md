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
mise install exon@0.5.0
mise use exon@0.5.0
```

### Build from source

Requires [Homebrew LLVM](https://formulae.brew.sh/formula/llvm) (macOS) or [LLVM 20+](https://apt.llvm.org/) (Linux) for `import std;` support.

**macOS:**
```sh
brew install llvm ninja
LLVM_PREFIX=$(brew --prefix llvm)
cmake -B build -G Ninja \
  -DCMAKE_CXX_COMPILER="$LLVM_PREFIX/bin/clang++" \
  -DCMAKE_CXX_STDLIB_MODULES_JSON="$LLVM_PREFIX/lib/c++/libc++.modules.json" \
  -DCMAKE_EXE_LINKER_FLAGS="-L$LLVM_PREFIX/lib/c++ -lc++ -lc++abi -L$LLVM_PREFIX/lib/unwind -lunwind"
cmake --build build
```

**Linux:**
```sh
wget -qO- https://apt.llvm.org/llvm.sh | sudo bash -s -- 20
sudo apt-get install -y libc++-20-dev libc++abi-20-dev ninja-build
pip install cmake --break-system-packages
cmake -B build -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++-20 \
  -DCMAKE_CXX_STDLIB_MODULES_JSON=/usr/lib/llvm-20/lib/libc++.modules.json \
  -DCMAKE_CXX_FLAGS="-stdlib=libc++" \
  -DCMAKE_EXE_LINKER_FLAGS="-stdlib=libc++ -lc++abi"
cmake --build build
```

Once bootstrapped, exon can build itself:

```sh
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
