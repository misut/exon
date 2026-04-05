# Workspaces

A workspace is a collection of packages that share a single root and can reference each other. Use it when one logical project is split across multiple packages (a library plus its binary, a core plus plugins, etc.).

## Layout

```
my-monorepo/
├── exon.toml               # workspace root
├── core/
│   ├── exon.toml           # member package (lib)
│   └── src/
│       └── core.cppm
└── app/
    ├── exon.toml           # member package (bin)
    └── src/
        └── main.cpp
```

## Root manifest

The workspace root has a `[workspace]` section listing member directories (relative to the root). It does not define its own package:

```toml
# my-monorepo/exon.toml
[workspace]
members = ["core", "app"]
```

Running `exon build`, `exon check`, `exon test`, `exon sync`, or `exon clean` in the workspace root iterates over every member in order.

## Member packages

Each member has a normal `exon.toml` with a `[package]` section:

```toml
# my-monorepo/core/exon.toml
[package]
name = "core"
version = "0.1.0"
type = "lib"
standard = 23
```

```toml
# my-monorepo/app/exon.toml
[package]
name = "app"
version = "0.1.0"
type = "bin"
standard = 23

[dependencies.workspace]
core = true
```

`core = true` tells exon: *find a workspace member whose `package.name` is `core` and link it*. Exon walks up from `app/` looking for an `exon.toml` with `[workspace]`, then matches `core` against each member's `package.name` field. Member directory names are irrelevant — only `package.name` matches.

## Sharing a module across members

With the workspace dep in place, `app/src/main.cpp` can just `import core;`:

```cpp
// my-monorepo/core/src/core.cppm
export module core;
import std;

export namespace core {
int magic_number() { return 42; }
std::string greeting() { return "hello from core"; }
}
```

```cpp
// my-monorepo/app/src/main.cpp
import std;
import core;

int main() {
    std::println("magic: {}", core::magic_number());
    std::println("{}", core::greeting());
    return 0;
}
```

## Building

Build everything from the workspace root:

```sh
cd my-monorepo
exon build
```

```
--- core ---
configuring...
building...
build succeeded: .exon/debug/core
--- app ---
fetching dependencies...
  path: core -> /abs/path/to/my-monorepo/core
configuring...
building...
build succeeded: .exon/debug/app
```

Or build a single member directly:

```sh
cd my-monorepo/app
exon run
```

```
fetching dependencies...
  path: core -> /abs/path/to/my-monorepo/core
configuring...
building...
build succeeded: .exon/debug/app
running app...

magic: 42
hello from core
```

## CLI

Add a workspace dep from inside a member:

```sh
cd my-monorepo/app
exon add --workspace core                 # [dependencies.workspace] core = true
exon add --dev --workspace mocks          # [dev-dependencies.workspace] mocks = true
```

Exon validates that the name matches an existing member before writing the entry — typos fail fast.

Add a path dep (works outside workspaces too):

```sh
exon add --path shared ../../vendor/shared
```

Remove any dep (git, find, path, or workspace) by name:

```sh
exon remove core
```

## Workspace vs. path deps

Both end up calling `add_subdirectory()` under the hood. Prefer **workspace** when both packages live in the same monorepo — it's resilient to directory moves and uses the package name that end-users actually see. Prefer **path** for references outside the workspace (vendored libraries, sibling repositories, etc.).

```toml
[dependencies.workspace]
core = true                      # "same workspace, look it up by name"

[dependencies.path]
shared = "../../vendor/shared"   # "explicit directory, anywhere on disk"
```

## Generated CMake

When exon syncs `CMakeLists.txt` for `app`, it emits:

```cmake
# my-monorepo/app/CMakeLists.txt
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../core ${CMAKE_BINARY_DIR}/_deps/core-build)

add_executable(app)
target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)
target_link_libraries(app PRIVATE core)
```

The explicit binary_dir is required because the source path is outside the project's own source tree. `exon sync` regenerates this file whenever `exon.toml` changes, so editing the TOML is enough — you never hand-edit the CMakeLists.
