#!/usr/bin/env bash
for d in /opt/zero2w-catplay-build-input-base /opt/zero2w-catplay-final-clean-apply /opt/zero2w-catplay-build; do
  echo "===$d"
  if test -f "$d/c2a/catplay_c2a/src/media_ipc.rs"; then
    grep -n "next_session\|test_begin_session\|fn begin_session" "$d/c2a/catplay_c2a/src/media_ipc.rs" | head -12
  else
    echo missing
  fi
done
