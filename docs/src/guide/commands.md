# Commands

A quick reference of every exon subcommand.

## Project lifecycle

- `exon init [name]` — create a new package in the current directory or
  at `./name`
- `exon new <name>` — create a new package at `./name` (always in a
  sibling directory, unlike `init`)
- `exon info` — print resolved manifest and dependency tree

## Build

- `exon build [--release] [--target <t>]` — fetch, configure, build
- `exon check [--target <t>]` — parse and type-check only (no linking)
- `exon run [--release] [-- args...]` — build then run the first
  runnable target, forwarding trailing args
- `exon debug` — run under the debugger configured in the toolchain
- `exon test [--filter f]` — build and run all test targets
- `exon clean` — remove `.exon/` build output

## Dependencies

- `exon add <pkg> <version>` — add a git dep to `[dependencies]`
- `exon add --path <name> <path>` — add a path dep to
  `[dependencies.path]`
- `exon add --workspace <name>` — add a workspace dep to
  `[dependencies.workspace]`
- `exon add --dev ...` — write under `[dev-dependencies*]` instead
- `exon remove <name>` — drop the dep entry from the manifest
- `exon update [--member m[,n,...]]` — refresh the lockfile

## Generation

- `exon sync` — regenerate `CMakeLists.txt` from the manifest.
  Invoked automatically by `build` / `check` / `test`; run manually
  when only the manifest changes.
- `exon fmt` — format C++23 sources using the configured formatter

## Version

- `exon version` — print the running exon version
