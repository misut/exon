# Exon

A package manager for C++. Inspired by Cargo.

## Build

Requires [Homebrew LLVM](https://formulae.brew.sh/formula/llvm) for `import std;` support.

### Bootstrap

```sh
brew install llvm
git clone --recursive git@github.com:misut/exon.git
cd exon
LLVM_PREFIX=/opt/homebrew/opt/llvm
cmake -B build -G Ninja \
  -DCMAKE_CXX_COMPILER="$LLVM_PREFIX/bin/clang++" \
  -DCMAKE_CXX_STDLIB_MODULES_JSON="$LLVM_PREFIX/lib/c++/libc++.modules.json"
cmake --build build
```

### Self-hosting

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
- **Build profiles** — debug (default) and release (`--release`)
- **Git-based registry** — fetches packages from GitHub repositories
- **Self-hosting** — exon builds itself with `exon build`

## License

MIT
