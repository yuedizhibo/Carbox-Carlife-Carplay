#!/usr/bin/env bash
set -euo pipefail
while pgrep -f 'zero2w-catplay-epoch-clean.*cargo|zero2w-catplay-epoch-clean.*rustc' >/dev/null; do sleep 5; done
cp /mnt/d/littlethings/CarPlay/zero2w/Input/WirelessCarPlay/Engine/catplay-patch/epoch-validation.log /mnt/d/littlethings/CarPlay/zero2w/Input/WirelessCarPlay/Engine/catplay-patch/epoch-validation-final.log
