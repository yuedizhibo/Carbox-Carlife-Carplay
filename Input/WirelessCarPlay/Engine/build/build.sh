#!/usr/bin/env bash
# Reproducible build for the Zero2W real Wireless-CarPlay sidecar.
#
#   ./build/build.sh test          host unit tests (cargo test --workspace)
#   ./build/build.sh fmt           formatting check (cargo fmt --check)
#   ./build/build.sh clippy        cargo clippy --workspace --all-targets -- -D warnings
#   ./build/build.sh check-arm64   cargo check for arm64 (no linker required)
#   ./build/build.sh arm64         static aarch64-unknown-linux-musl release binary
#   ./build/build.sh all           fmt + clippy + test + arm64
#
# CatPlay C2A is deliberately built outside this workspace to preserve its
# unresolved provenance boundary: see ./build/build-catplay-c2a.sh.
#
# The arm64 lane produces a fully static binary, which is what the target needs:
# the board runs Debian 12 with glibc 2.36, and a static musl binary has no libc
# coupling at all. It requires a linker driver that can target musl; clang from
# LLVM plus rustup's self-contained musl sysroot is verified to work.
#
# Toolchain overrides (needed when cargo lives on another OS side, e.g. running
# the Windows toolchain from WSL):
#   CARGO, RUSTC, CLANG, TARGET
set -euo pipefail

TARGET=${TARGET:-aarch64-unknown-linux-musl}
CARGO=${CARGO:-cargo}
RUSTC=${RUSTC:-rustc}
CLANG=${CLANG:-clang}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
cd "$ROOT"

lane=${1:-all}

arm64_flags() {
  local sysroot musl_root
  # tr strips the CR that a Windows-side rustc emits when driven from WSL/MSYS.
  sysroot=$("$RUSTC" --print sysroot | tr -d '\r')
  musl_root="$sysroot/lib/rustlib/$TARGET"
  # Kept in RUSTFLAGS rather than on the command line so no shell translates the
  # Windows-style sysroot path.
  export RUSTFLAGS="-C linker=$CLANG -C link-arg=--target=aarch64-linux-musl -C link-arg=--sysroot=$musl_root -C link-arg=-static"
  # WSL only forwards the variables listed in WSLENV to Windows processes. Without
  # this, a Windows-side cargo.exe never sees RUSTFLAGS, silently falls back to
  # `cc` (mingw) and fails on ELF-only linker options.
  case $CARGO in
    *.exe) export WSLENV="${WSLENV:+$WSLENV:}RUSTFLAGS" ;;
  esac
  echo "sysroot: $musl_root" >&2
  echo "linker:  $CLANG" >&2
}

report_artifact() {
  local bin="target/$TARGET/release/cp-native"
  if [ -f "$bin" ]; then
    ls -l "$bin" >&2
    if command -v file >/dev/null 2>&1; then file "$bin" >&2; fi
    echo "artifact: $ROOT/$bin" >&2
  else
    echo "artifact missing: $bin" >&2
    return 3
  fi
}

case "$lane" in
  fmt)
    "$CARGO" fmt --all --check
    ;;
  test)
    "$CARGO" test --workspace
    ;;
  clippy)
    "$CARGO" clippy --workspace --all-targets -- -D warnings
    ;;
  check-arm64)
    "$CARGO" check --workspace --all-targets --target "$TARGET"
    ;;
  arm64)
    arm64_flags
    "$CARGO" build --release --target "$TARGET" -p cp-native
    report_artifact
    ;;
  all)
    "$0" fmt
    "$0" clippy
    "$0" test
    "$0" arm64
    ;;
  *)
    echo "usage: $0 {fmt|test|clippy|check-arm64|arm64|all}" >&2
    exit 2
    ;;
esac
