#!/usr/bin/env bash
# 收尾: 压缩产物，验证 ABI-bound modules bundle，并同步到 Windows。
set -euo pipefail
WIN="${WIN:-/mnt/d/littlethings/CarPlay/zero2w/Linux/zero2wsource}"
KREL=6.1.31-opi-min

verify_minimal_bundle() {
  local k=$1 manifest_hash
  [ -f "$k/Image" ] && [ -f "$k/System.map" ] && [ -f "$k/kernel.config" ] && \
    [ -f "$k/dtb/sun50i-h618-orangepi-zero2w.dtb" ] || { echo "missing kernel artifact in $k" >&2; return 1; }
  [ -f "$k/modules-$KREL.tar.gz" ] && [ -f "$k/modules-$KREL.sha256" ] && \
    [ -f "$k/artifact-manifest.sha256" ] && [ -f "$k/provenance.txt" ] || {
      echo "missing ABI-bound module bundle in $k; rebuild minimal with wsl-11" >&2; return 1;
    }
  grep -qx 'format=zero2w-kernel-bundle-v1' "$k/provenance.txt"
  grep -qx "kernel_release=$KREL" "$k/provenance.txt"
  manifest_hash=$(sha256sum "$k/artifact-manifest.sha256" | awk '{print $1}')
  grep -qx "manifest_sha256=$manifest_hash" "$k/provenance.txt"
  ( cd "$k" && sha256sum -c artifact-manifest.sha256 )
}

for V in minimal tiny; do
  K="$HOME/opi/out-$V/kernel"
  [[ -f $K/Image ]] || { echo "跳过 $V (无 Image)"; continue; }
  cd "$K"
  gzip -9 -c Image > Image.gz
  echo "=== $V ==="
  printf "  Image    : %'d 字节 (%.1f MB)\n" "$(stat -c %s Image)" "$(echo "$(stat -c %s Image)/1048576" | bc -l)"
  printf "  Image.gz : %'d 字节 (%.1f MB)\n" "$(stat -c %s Image.gz)" "$(echo "$(stat -c %s Image.gz)/1048576" | bc -l)"
  echo "  驱动符号计数 (System.map, 证明 built-in):"
  for pat in brcmf stmmac sunxi_gmac gmac cedrus sun50iw9 audcodec aw8738 ac200 axp20x mv64xxx sun4i_spi sunxi_musb musb btmuart hci_uart sun8i_ths sunxi_sid gpio_sunxi leds_gpio; do
    n=$(grep -cw "$pat" System.map 2>/dev/null || true)
    n2=$(grep -c "$pat" System.map 2>/dev/null || true)
    [ "$n2" -gt 0 ] && printf "    %-16s %4s\n" "$pat" "$n2"
  done
  D="$WIN/build-out-$V/kernel"
  mkdir -p "$D/dtb"
  cp -f Image Image.gz System.map kernel.config "$D/"
  cp -f dtb/*.dtb "$D/dtb/"
  if [ "$V" = minimal ]; then
    verify_minimal_bundle "$K"
    cp -f "modules-$KREL.tar.gz" "modules-$KREL.sha256" artifact-manifest.sha256 provenance.txt "$D/"
    verify_minimal_bundle "$D"
    echo "  ABI-bound modules bundle 已同步 -> $D/modules-$KREL.tar.gz"
  fi
  echo "  已同步 -> Windows build-out-$V/kernel"
done
cp -f "$HOME/opi/out-tiny/kernel/kernel.config" "$WIN/configs/zero2w-tiny.config" 2>/dev/null || true
echo FINALIZE_DONE
