#!/usr/bin/env bash
set -euo pipefail
# WSL non-login execution does not load the Rust toolchain path.
source /root/.cargo/env
ROOT=/mnt/d/littlethings/CarPlay/zero2w/Input/WirelessCarPlay/Engine
SRC="$ROOT/../../Reference/catplay"
PATCHROOT="$ROOT/catplay-patch"
CLEAN=/opt/zero2w-catplay-epoch-clean
HOST_TARGET=/opt/zero2w-catplay-epoch-target-host
ARM_TARGET=/opt/zero2w-catplay-epoch-target-arm
rm -rf "$CLEAN" "$HOST_TARGET" "$ARM_TARGET"
cp -a "$SRC" "$CLEAN"
find "$CLEAN" -type f \( -name '*.rs' -o -name 'Cargo.toml' \) -exec sed -i 's/\r$//' {} +
patch -d "$CLEAN" -p1 --batch --fuzz=0 < "$PATCHROOT/catplay-c2a-cp-native.patch"
patch -d "$CLEAN" -p1 --batch --fuzz=0 < "$PATCHROOT/catplay-c2a-input-only.patch"
printf 'clean_apply=PASS\n'
cd "$CLEAN"
CARGO_TARGET_DIR="$HOST_TARGET" rustup run 1.88.0 cargo test -p catplay_c2a --no-default-features
CARGO_TARGET_DIR="$HOST_TARGET" rustup run 1.88.0 cargo build -p catplay_c2a --no-default-features
"$HOST_TARGET/debug/catplay_c2a" --cp-capabilities-json
CATPLAY_ROOT="$CLEAN" CARGO_TARGET_DIR="$ARM_TARGET" STRICT_BOOKWORM=1 TARGET=aarch64-unknown-linux-gnu "$ROOT/build/build-catplay-c2a.sh"
ARM="$ARM_TARGET/aarch64-unknown-linux-gnu/release/catplay_c2a"
printf 'arm_sha256='; sha256sum "$ARM" | awk '{print $1}'
printf 'arm_glibc_max='; aarch64-linux-gnu-readelf --version-info "$ARM" | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -Vu | tail -n1
printf 'overlay_sha256='; sha256sum "$PATCHROOT/catplay-c2a-input-only.patch" | awk '{print $1}'
printf 'overlay_bytes='; wc -c < "$PATCHROOT/catplay-c2a-input-only.patch"
python3 - "$PATCHROOT/catplay-c2a-input-only.patch" <<'PY'
import sys
p=sys.argv[1]
d=open(p,'rb').read()
assert b'\0' not in d and b'\r' not in d
print('overlay_text_safety=PASS')
PY
