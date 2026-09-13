#!/usr/bin/env bash
set -euo pipefail
ROOT=/mnt/d/littlethings/CarPlay/zero2w/Input/WirelessCarPlay/Engine
SRC="$ROOT/../../Reference/catplay"
PATCHROOT="$ROOT/catplay-patch"
BASE=/opt/zero2w-catplay-epoch-base
WORK=/opt/zero2w-catplay-epoch-work
OUT="$PATCHROOT/catplay-c2a-input-only.patch"
rm -rf "$BASE" "$WORK"
cp -a "$SRC" "$BASE"
find "$BASE" -type f \( -name '*.rs' -o -name 'Cargo.toml' \) -exec sed -i 's/\r$//' {} +
patch -d "$BASE" -p1 --batch --fuzz=0 < "$PATCHROOT/catplay-c2a-cp-native.patch"
cp -a "$BASE" "$WORK"
patch -d "$WORK" -p1 --batch --fuzz=0 < "$OUT"
# The checked-in overlay is the desired complete delta; regenerate it from the
# normalized base after proving a fuzz-0 application.
files=(
 c2a/catplay_c2a/src/config.rs
 c2a/catplay_c2a/src/mfi_manager.rs
 c2a/catplay_c2a/src/lib.rs
 c2a/catplay_c2a/src/main.rs
 c2a/catplay_c2a/src/main_init.rs
 c2a/catplay_c2a/src/telemetry.rs
 c2a/catplay_c2a/src/media_ipc.rs
 c2a/catplay_c2a/src/input_only.rs
 c2a/catplay_c2a/src/preview_rx.rs
)
tmp="$OUT.tmp"
: > "$tmp"
for f in "${files[@]}"; do
  if test -e "$BASE/$f"; then
    diff -u --label "a/$f" "$BASE/$f" --label "b/$f" "$WORK/$f" >> "$tmp" || rc=$?
  else
    diff -u --label /dev/null /dev/null --label "b/$f" "$WORK/$f" >> "$tmp" || rc=$?
  fi
  if test "${rc:-0}" -gt 1; then exit "$rc"; fi
  unset rc
done
python3 - "$tmp" <<'PY'
import sys
p=sys.argv[1]
data=open(p,'rb').read()
if b'\0' in data or b'\r' in data: raise SystemExit('overlay contains NUL/CR')
PY
mv "$tmp" "$OUT"
printf 'overlay_sha256='; sha256sum "$OUT" | awk '{print $1}'
printf 'overlay_bytes='; wc -c < "$OUT"
