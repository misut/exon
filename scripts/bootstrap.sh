#!/bin/sh
# Bootstrap the exon CMake build using the intron-pinned LLVM toolchain.
# Used by .github/workflows/ci.yml, .github/workflows/release.yml, and the
# README "Build from source" path so that local and CI invocations match.
set -eu

eval "$(intron env)"

LLVM_ROOT="$HOME/.intron/toolchains/llvm/22.1.2"
MODULES_JSON=$(find "$LLVM_ROOT" -name "libc++.modules.json" -print -quit)

CMAKE_ARGS="-Wno-dev -B build -G Ninja"
CMAKE_ARGS="$CMAKE_ARGS -DCMAKE_CXX_COMPILER=$LLVM_ROOT/bin/clang++"
if [ -n "$MODULES_JSON" ]; then
    CMAKE_ARGS="$CMAKE_ARGS -DCMAKE_CXX_STDLIB_MODULES_JSON=$MODULES_JSON"
fi

OS=$(uname -s)
case "$OS" in
    Darwin)
        # Mirror the self-host link flags from build::use_system_macos_runtime so
        # newer macOS SDKs (Xcode 26+) that no longer auto-link libc++abi succeed.
        CMAKE_ARGS="$CMAKE_ARGS -DCMAKE_OSX_SYSROOT=$(xcrun --show-sdk-path)"
        CMAKE_ARGS="$CMAKE_ARGS \"-DCMAKE_EXE_LINKER_FLAGS=-lc++ -lc++abi\""
        ;;
    Linux)
        LIBCXX_DIR=$(dirname "$(find "$LLVM_ROOT" -name "libc++.so" -print -quit)")
        CMAKE_ARGS="$CMAKE_ARGS -DCMAKE_CXX_FLAGS=-stdlib=libc++"
        CMAKE_ARGS="$CMAKE_ARGS \"-DCMAKE_EXE_LINKER_FLAGS=-stdlib=libc++ -lc++abi -Wl,-rpath,$LIBCXX_DIR\""
        ;;
esac

eval cmake $CMAKE_ARGS

# macOS serializes the bootstrap to work around the libc++ modules build race
# tracked in https://www.mail-archive.com/llvm-bugs@lists.llvm.org/msg86280.html
if [ "$OS" = "Darwin" ]; then
    cmake --build build --target exon --parallel 1
else
    cmake --build build --target exon
fi
