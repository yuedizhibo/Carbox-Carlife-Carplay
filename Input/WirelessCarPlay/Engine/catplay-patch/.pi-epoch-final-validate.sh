#!/usr/bin/env bash
set -uo pipefail
source /root/.cargo/env
ROOT=/mnt/d/littlethings/CarPlay/zero2w/Input/WirelessCarPlay/Engine
WORK=/opt/zero2w-catplay-epoch-work
LOG="$ROOT/catplay-patch/epoch-final-validation.log"
HOST=/opt/zero2w-catplay-epoch-final-host
ARM_TARGET=/opt/zero2w-catplay-epoch-final-arm
status=0
cd "$WORK" || status=$?
if [ "$status" -eq 0 ]; then
  CARGO_TARGET_DIR="$HOST" rustup run 1.88.0 cargo test -p catplay_c2a --no-default-features || status=$?
fi
# Count library and binary test groups from this foreground run's log.
printf 'TEST_COUNT='; awk '/^running [0-9]+ tests$/ {n += $2} END {print n + 0}' "$LOG"
printf 'TEST_EXIT=%s\n' "$status"
if [ "$status" -eq 0 ]; then
  CATPLAY_ROOT="$WORK" CARGO_TARGET_DIR="$ARM_TARGET" STRICT_BOOKWORM=1 TARGET=aarch64-unknown-linux-gnu "$ROOT/build/build-catplay-c2a.sh" || status=$?
fi
if [ "$status" -eq 0 ]; then
  artifact="$ARM_TARGET/aarch64-unknown-linux-gnu/release/catplay_c2a"
  printf 'ARTIFACT_SHA256='; sha256sum "$artifact" | awk '{print $1}'
  printf 'MAX_GLIBC='; aarch64-linux-gnu-readelf --version-info "$artifact" | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -Vu | tail -n1
fi
printf 'EXIT=%s\n' "$status"
exit "$status"
