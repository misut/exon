# Workspace Example

This checked-in workspace is a runnable example of exon's Gradle-lite workspace flow.

It demonstrates:

- root `[workspace] members`
- shared `[workspace.package]`
- shared `[workspace.build]`, `[workspace.build.debug]`, and `[workspace.build.release]`
- member-level `[dependencies.workspace]`
- root `exon run --member <name>`

## Layout

```text
examples/workspace/
├── exon.toml
├── libs/
│   ├── foundation/
│   └── greeting/
└── apps/
    ├── hello/
    ├── inspect/
    └── report/
```

## Dependency graph

```text
foundation
├── greeting
│   └── hello
├── report
└── inspect
    └── greeting
```

`hello` is the representative runnable target: it depends on `greeting`, which depends on `foundation`, so root builds must respect the workspace DAG instead of the declaration order in `members = [...]`.

## Run it

From this directory:

```sh
exon sync
exon build
exon run --member hello
exon run --release --member hello
```

The debug and release runs print different profile labels because the workspace root injects profile-specific compiler flags through `[workspace.build.debug]` and `[workspace.build.release]`.

When you are working on the `exon` repository itself, use your current-source binary instead of a stale installed one. From the repo root, that is typically `./.exon/debug/exon`.
