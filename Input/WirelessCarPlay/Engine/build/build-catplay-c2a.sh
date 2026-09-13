#!/usr/bin/env bash
# Build the owner-provided CatPlay C2A executable only in a disposable checkout.
# This helper is a local-evaluation boundary: it never patches the read-only
# reference trees under Reference/, never copies CatPlay into the C++ process,
# and never deploys an artifact.
set -euo pipefail

TARGET=${TARGET:-aarch64-unknown-linux-gnu}
RUSTUP_TOOLCHAIN=${RUSTUP_TOOLCHAIN:-1.88.0}
CARGO_BIN=${CARGO:-cargo}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
PATCH_ROOT="$ROOT/catplay-patch"
BASE_PATCH="$PATCH_ROOT/catplay-c2a-cp-native.patch"
INPUT_ONLY_PATCH="$PATCH_ROOT/catplay-c2a-input-only.patch"
INPUT_API_PATCH="$PATCH_ROOT/catplay-c2a-input-api.patch"
# Resolve with symlinks when the tree exists, so the prefix test below compares the
# same canonical form that realpath -e produces for CATPLAY_ROOT.
REFERENCE_PARENT=$(realpath -e "$ROOT/../../../Reference" 2>/dev/null || realpath -m "$ROOT/../../../Reference")

if [ -z "${CATPLAY_ROOT:-}" ]; then
  echo "CATPLAY_ROOT must name a disposable CatPlay checkout; refusing the reference tree." >&2
  exit 2
fi
CATPLAY_ROOT=$(realpath -e "$CATPLAY_ROOT")
# Refuse any checkout that lives inside the read-only Reference/ tree. This is a
# prefix test on the fully resolved path, so renaming a reference clone or adding
# a new one cannot bypass it. Reference/CatPlaySource is the outer repository's
# own tracked working tree, so patching it would dirty the main checkout instead
# of a disposable copy.
case "$CATPLAY_ROOT/" in
  "$REFERENCE_PARENT"/*)
    echo "refusing to patch the read-only reference tree: $REFERENCE_PARENT" >&2
    echo "CATPLAY_ROOT must be a disposable checkout outside Reference/" >&2
    exit 2
    ;;
esac
if [ ! -f "$CATPLAY_ROOT/Cargo.toml" ]; then
  echo "CatPlay source not found: $CATPLAY_ROOT" >&2
  exit 2
fi
for patch_file in "$BASE_PATCH" "$INPUT_ONLY_PATCH" "$INPUT_API_PATCH"; do
  [ -f "$patch_file" ] || { echo "missing required external patch: $patch_file" >&2; exit 2; }
done

# The owner-provided checkout uses CRLF in parts of the Rust tree. Normalizing a
# disposable copy before patching keeps both external layers deterministic.
find "$CATPLAY_ROOT" -type f \( -name '*.rs' -o -name 'Cargo.toml' \) -exec sed -i 's/\r$//' {} +

apply_once() {
  local patch_file=$1
  local label=$2
  if patch -d "$CATPLAY_ROOT" -p1 --batch --forward --fuzz=0 --dry-run < "$patch_file" >/dev/null 2>&1; then
    patch -d "$CATPLAY_ROOT" -p1 --batch --forward --fuzz=0 < "$patch_file"
    echo "applied $label"
    return
  fi
  if patch -d "$CATPLAY_ROOT" -p1 --batch --reverse --fuzz=0 --dry-run < "$patch_file" >/dev/null 2>&1; then
    echo "$label already applied"
    return
  fi
  echo "$label does not apply cleanly to $CATPLAY_ROOT" >&2
  exit 3
}

# A completed third overlay changes contexts of the first two layers. Verify
# its reverse applicability before testing the older layers on a reused tree.
if patch -d "$CATPLAY_ROOT" -p1 --batch --reverse --fuzz=0 --dry-run < "$INPUT_API_PATCH" >/dev/null 2>&1; then
  echo "catplay input API overlay already applied (building existing evaluation tree)"
else
  apply_once "$BASE_PATCH" "catplay base integration patch"
  apply_once "$INPUT_ONLY_PATCH" "catplay input-only overlay"
  apply_once "$INPUT_API_PATCH" "catplay input API overlay"
fi

if command -v rustup >/dev/null 2>&1; then
  CARGO_CMD=(rustup run "$RUSTUP_TOOLCHAIN" "$CARGO_BIN")
else
  CARGO_CMD=("$CARGO_BIN")
fi

cd "$CATPLAY_ROOT"
# Disable the default UI and allocator for the smallest local-evaluation path.
# Honour CARGO_TARGET_DIR so a verified isolated Bookworm cache can be reused.
BUILD_TARGET_DIR=${CARGO_TARGET_DIR:-"$CATPLAY_ROOT/target"}
"${CARGO_CMD[@]}" build --release --target "$TARGET" -p catplay_c2a --no-default-features
bin="$BUILD_TARGET_DIR/$TARGET/release/catplay_c2a"
[ -f "$bin" ] || { echo "CatPlay C2A artifact missing: $bin" >&2; exit 4; }

strict_bookworm_gate() {
  local readelf=${READELF:-aarch64-linux-gnu-readelf}
  local max_glibc version header
  command -v "$readelf" >/dev/null 2>&1 || { echo "missing AArch64 readelf: $readelf" >&2; return 1; }
  header=$($readelf -h "$bin")
  grep -Fq 'Machine:                           AArch64' <<<"$header"
  grep -Eq 'Type: +DYN \(Position-Independent Executable file\)' <<<"$header"
  version=$($readelf --version-info "$bin" | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -Vu | tail -n1 || true)
  [ -n "$version" ] || { echo "no GLIBC version found in $bin" >&2; return 1; }
  max_glibc=${version#GLIBC_}
  [ "$(printf '%s\n%s\n' "$max_glibc" "${MAX_GLIBC:-2.36}" | sort -V | head -n1)" = "$max_glibc" ] || {
    echo "maximum GLIBC $max_glibc exceeds ${MAX_GLIBC:-2.36}" >&2
    return 1
  }
  printf 'strict Bookworm gate: machine=AArch64 PIE max_glibc=%s\n' "$max_glibc"
}

if [ "${STRICT_BOOKWORM:-0}" = 1 ]; then
  strict_bookworm_gate
fi

ls -l "$bin"
command -v file >/dev/null 2>&1 && file "$bin" || true
echo "Built external CatPlay artifact: $(cd "$(dirname "$bin")" && pwd)/$(basename "$bin")"
echo "Local evaluation only; do not deploy, distribute, publish, or link this CatPlay artifact until its license is confirmed." >&2
