# Workspaces

A workspace is a collection of packages that share a single root and can reference each other. Use it when one logical project is split across multiple packages (a library plus its binary, a core plus plugins, etc.).

## Scaffolding

Create a workspace root, then add members from the root:

```sh
mkdir my-monorepo
cd my-monorepo

exon init --workspace
exon new --lib core
exon new --lib util
exon new --bin app
```

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
members = ["core", "util", "app"]

[workspace.package]
version = "0.1.0"
authors = ["misut"]
license = "MIT"
standard = 23

[workspace.build]
cxxflags = ["-Wall"]

[workspace.build.debug]
cxxflags = ["-fsanitize=address"]
ldflags = ["-fsanitize=address"]
```

`[workspace.package]` provides defaults for members. A member's explicit `[package]` field wins; missing fields inherit from the root.

`[workspace.build]`, `[workspace.build.debug]`, and `[workspace.build.release]` prepend shared compile and link flags to each member's own build flags.

## Member packages

Each member has a normal `exon.toml` with a `[package]` section:

```toml
# my-monorepo/core/exon.toml
[package]
name = "core"
type = "lib"
```

```toml
# my-monorepo/util/exon.toml
[package]
name = "util"
type = "lib"

[dependencies.workspace]
core = true
```

```toml
# my-monorepo/app/exon.toml
[package]
name = "app"
type = "bin"

[dependencies.workspace]
util = true
```

`util = true` tells exon: find a workspace member whose `package.name` is `util` and link it. Exon walks up from `app/` looking for an `exon.toml` with `[workspace]`, then matches `util` against each member's `package.name`. Member directory names are irrelevant — only `package.name` matches.

## Sharing a module across members

With the workspace deps in place, `app/src/main.cpp` can just `import util;`:

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
// my-monorepo/util/src/util.cppm
export module util;
import std;
import core;

export namespace util {
std::string banner() {
    return std::format("{} ({})", core::greeting(), core::magic_number());
}
}
```

```cpp
// my-monorepo/app/src/main.cpp
import std;
import util;

int main() {
    std::println("{}", util::banner());
    return 0;
}
```

## Root commands

From the workspace root:

- `exon build`, `exon check`, `exon test`, `exon sync`, `exon clean`, and `exon update` operate on the selected member set.
- Members run in dependency order based on `[dependencies.workspace]`, not declaration order in `members = [...]`.
- `--member a,b` narrows the selection.
- `--exclude x,y` removes members from the selection.
- `exon run --member <name>` runs a workspace member from the root. Without `--member`, exon requires exactly one runnable member in the current selection.

## Building

Build the whole workspace from the root:

```sh
cd my-monorepo
exon build
```

```
configuring workspace...
building workspace...
build succeeded: .exon/workspace/debug
```

Workspace builds generate one shared CMake graph under `.exon/workspace/<profile>` (or `.exon/workspace/<target>/<profile>` for cross-target builds). Shared workspace libraries are compiled once and reused by downstream members.

Build only one app plus the workspace libraries it depends on:

```sh
exon build --member app
```

Run one app from the root:

```sh
exon run --member app
```

You can still run a member directly from its own directory:

```sh
cd my-monorepo/app
exon run
```

```
fetching dependencies...
fetching dependencies...
configuring...
building...
build succeeded: .exon/debug/app
running app...

hello from core (42)
```

## Runnable example in this repo

This repository includes a checked-in workspace example at `examples/workspace/`:

- `libs/foundation` exports shared helpers and package defaults
- `libs/greeting` depends on `foundation`
- `apps/hello` depends on `greeting`, so the build graph is `hello -> greeting -> foundation`
- `apps/report` depends directly on `foundation`
- `apps/inspect` depends on both `greeting` and `foundation`

From `examples/workspace/`:

```sh
exon sync
exon build
exon run --member hello
```

Run `exon run --release --member hello` to see the workspace root's
`[workspace.build.release]` flags change the profile label printed by `hello`.

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

Update part of the workspace:

```sh
exon update --member util,app
```

Exclude one member from a root-wide test run:

```sh
exon test --exclude app
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

`exon sync` from the workspace root does two things:

- regenerate each selected member's root `CMakeLists.txt`
- regenerate the workspace root `CMakeLists.txt` aggregator when `[sync] cmake-in-root = true` on the workspace root

The generated workspace root file looks like this:

```cmake
# my-monorepo/CMakeLists.txt
cmake_minimum_required(VERSION 3.30)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
set(CMAKE_CXX_MODULE_STD ON)
project(my_monorepo LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(EXON_ENABLE_TEST_TARGETS OFF)

add_subdirectory("core" ${CMAKE_BINARY_DIR}/members/core)
add_subdirectory("util" ${CMAKE_BINARY_DIR}/members/util)
add_subdirectory("app" ${CMAKE_BINARY_DIR}/members/app)
```

Each member still gets its own portable root `CMakeLists.txt`. For example, `app/CMakeLists.txt` can contain:

```cmake
# my-monorepo/app/CMakeLists.txt
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../util ${CMAKE_BINARY_DIR}/_deps/util-build)

add_executable(app)
target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)
target_link_libraries(app PRIVATE util)
```

The explicit binary dir is required because the source path is outside the member's own source tree. `exon sync` regenerates these files whenever `exon.toml` changes, so editing the TOML is enough — do not hand-edit the generated `CMakeLists.txt` files.
