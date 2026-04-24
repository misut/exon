# Introduction

**exon** is a Cargo-inspired package manager for C++23. It drives the
daily loop — scaffold, fetch, build, test, release — for projects that
use C++23 modules (`import std;`) without asking the author to maintain
CMake by hand.

## Features

- C++23 modules — `import std;` everywhere; no headers
- Automatic `CMakeLists.txt` generation from the manifest
- Git-based dependency resolution with a lockfile for reproducible
  builds
- WebAssembly cross-compilation (`exon build --target wasm32-wasi`)
  using wasi-sdk
- Workspaces with shared build flags, path and workspace deps, and
  selective builds
- Hooks into the rest of the workspace toolchain: pairs with
  [intron](https://github.com/misut/intron) for compiler / linker
  / sysroot management and [mise](https://mise.jdx.dev/) for tool
  version pinning

## Quick start

```sh
# Install the toolchain (handled by mise + intron in practice)
curl -fsSL https://raw.githubusercontent.com/misut/exon/main/install.sh | sh
curl -fsSL https://raw.githubusercontent.com/misut/intron/main/install.sh | sh

# Create a project
exon init hello
cd hello

# Build and run
exon run

# Cross-compile to WebAssembly
exon build --target wasm32-wasi
```
