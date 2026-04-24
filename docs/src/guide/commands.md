# Commands

A quick reference of every exon subcommand.

## Project lifecycle

- `exon init [name]` — create a new package in the current directory or
  at `./name`
- `exon new <name>` — create a new package at `./name` (always in a
  sibling directory, unlike `init`)
- `exon info` — print resolved manifest and dependency tree

## Build

- `exon build [--release] [--target <t>] [--output human|json|wrapped|raw]`
  — fetch, configure, build
- `exon status [--output human|json]` — inspect the current package or
  workspace, detected tools, build cache, and terminal capability policy
- `exon doctor [--output human|json]` — alias for `status`
- `exon check [--target <t>]` — parse and type-check only (no linking)
- `exon run [--release] [-- args...]` — build then run the first
  runnable target, forwarding trailing args
- `exon debug` — run under the debugger configured in the toolchain
- `exon test [--filter f]` — build and run all test targets
- `exon clean` — remove `.exon/` build output

`build` and `test` default to `--output human`. Use `--output json`
for JSON Lines events (`stage`, `diagnostic`, `artifact`,
`test-result`, and `summary`). In an interactive terminal, human configure
and build progress refreshes recent CMake/Ninja output lines in place while
keeping full tool logs available through `--output wrapped`. Terminal
rendering can be controlled with `--color`, `--progress`, `--unicode`, and
`--hyperlinks`, each accepting `auto`, `always`, or `never`; the matching
environment variables are `EXON_COLOR`, `EXON_PROGRESS`, `EXON_UNICODE`, and
`EXON_HYPERLINKS`.

## Dependencies

- `exon add <pkg> <version>` — add a git dep to `[dependencies]`
- `exon add --path <name> <path>` — add a path dep to
  `[dependencies.path]`
- `exon add --workspace <name>` — add a workspace dep to
  `[dependencies.workspace]`
- `exon add --dev ...` — write under `[dev-dependencies*]` instead
- `exon remove <name>` — drop the dep entry from the manifest
- `exon outdated [pkg...] [--member m[,n,...]] [--output human|json]`
  — compare locked git dependencies with remote semver tags; non-git
  dependencies are reported as skipped
- `exon update [pkg...] [--dry-run] [--precise <version>] [--member m[,n,...]]`
  — refresh lockfile entries to the newest compatible git tag, or lock one
  package to a precise version that still satisfies its manifest requirement

## Generation

- `exon sync` — regenerate `CMakeLists.txt` from the manifest.
  Invoked automatically by `build` / `check` / `test`; run manually
  when only the manifest changes.
- `exon fmt` — format C++23 sources using the configured formatter

## Version

- `exon version` — print the running exon version
