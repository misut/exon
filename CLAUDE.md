# Exon

A package manager for C++, inspired by Rust's Cargo. MIT License (misut).

## Tools

Managed via [mise](https://mise.jdx.dev/). See `mise.toml`.

```sh
mise install
```

## Build

exon manages its own CMakeLists.txt. Bootstrap with `.github/cmake/`, then use `exon build`.

```sh
# bootstrap (first build only)
git clone https://github.com/misut/tomlcpp.git /tmp/tomlcpp
cmake -B build -S .github/cmake -G Ninja -DTOMLCPP_DIR=/tmp/tomlcpp
cmake --build build

# self-host (normal development)
./build/exon build
./build/exon test
```

## Code Style

- Use C++20 `import` instead of `#include`. For build speed and modern features.
- No external libraries. Implement everything from scratch.

## Manifest

- Format: TOML (`exon.toml`)
- Parser: implemented from scratch (no external libraries)

### Compile Definitions

Built-in defines are automatically passed to all targets:
- `EXON_PKG_NAME` — package name from `[package]`
- `EXON_PKG_VERSION` — package version from `[package]`

User-defined compile definitions via `[defines]` section:
```toml
[defines]
MY_FLAG = "value"

[defines.debug]
DEBUG_MODE = "1"

[defines.release]
NDEBUG = "1"
```

### CMakeLists.txt Sync

`exon sync` generates a portable `CMakeLists.txt` in the project root, allowing plain cmake builds without exon. Uses relative paths and FetchContent for dependencies. Also auto-synced on `exon build`, `check`, and `test`.

## Related Projects

- **intron**: Toolchain manager (like rust-toolchain). https://github.com/misut/intron

## Package Registry

- Uses GitHub repositories as package sources (Go modules style)
- Self-hosted registry service may be developed later

## Conventions

- Commits: [Conventional Commits](https://www.conventionalcommits.org/) (`feat:`, `fix:`, `chore:`, etc.)
- No Co-Authored-By in commits.
- Branches: prefixed with `feat/`, `fix/`, `chore/`, etc. Squash and merge after CI passes.

## Repository

- Remote: `git@github.com:misut/exon.git`
- Default branch: `main`
