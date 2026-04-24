# Getting started

## Install

```sh
curl -fsSL https://raw.githubusercontent.com/misut/exon/main/install.sh | sh
curl -fsSL https://raw.githubusercontent.com/misut/intron/main/install.sh | sh
```

The `exon` binary goes to `~/.exon/bin`; `intron` to `~/.intron/bin`.
Make sure both are on `$PATH` (the installers print the export lines
you need).

## Scaffold a package

```sh
exon init hello
cd hello
tree
```

```
hello/
├── exon.toml
└── src/
    └── main.cpp
```

`exon.toml` is the manifest:

```toml
[package]
name    = "hello"
version = "0.1.0"
type    = "bin"
standard = 23
```

## Build and run

```sh
exon build
exon run
```

`exon build` generates `CMakeLists.txt`, configures with Ninja, and
builds. Output goes to `.exon/debug/hello`. Re-running `exon build`
after editing sources is incremental.

## Add a dependency

```sh
exon add github.com/misut/phenotype 0.13.0
```

This writes the dep to `exon.toml`, pins the resolved commit in
`exon.lock`, and clones the repository under `~/.exon/cache`.

## Test

```sh
exon test
```

Any target declared in `exon.toml` under `[tests]` is built and run
once. Filter with `exon test --filter test_name`.

## Cross-compile

```sh
exon build --target wasm32-wasi
```

`intron` provides the `wasi-sdk` toolchain; set it up once with
`intron install wasi-sdk 32`. Output goes to
`.exon/wasm32-wasi/debug/hello`, and `wasmtime` can execute it
directly.
