#!/usr/bin/env bash
# ============================================================
# 构建 Orange Pi Zero 2W 极简内核
#   VARIANT=minimal  用 configs/zero2w-minimal_defconfig (板载=y 内建, 其余保留 =m)
#   VARIANT=tiny     minimal 基础上再 MODULES=n (全部内建, 最省内存/最快启动)
# 产物: ~/opi/out-min/kernel/{Image,dtb,modules,System.map,kernel.config}
# 日志: ~/opi/out-min/build.log
# ============================================================
set -euo pipefail
SRC="${SRC:-$HOME/opi/src}"
WIN="${WIN:-/mnt/d/littlethings/CarPlay/zero2w/Linux/zero2wsource}"
KER="$SRC/01-soc-h616-h618-boot-kernel/linux-orangepi-6.1-sun50iw9"
VARIANT="${VARIANT:-minimal}"
OUT="${OUT:-$HOME/opi/out-${VARIANT}}"
CC="${CC:-aarch64-linux-gnu-}"
J="${J:-$(nproc)}"
KCFLAGS="${KCFLAGS:--Wno-error=int-conversion -Wno-error=implicit-function-declaration -Wno-error=implicit-int -Wno-error=incompatible-pointer-types -Wno-error=discarded-qualifiers}"
DEF="$WIN/configs/zero2w-minimal_defconfig"
if [ -f "$WIN/configs/zero2w-${VARIANT}_defconfig" ]; then DEF="$WIN/configs/zero2w-${VARIANT}_defconfig"; fi
[[ -d $KER ]] || { echo "找不到内核树 $KER"; exit 1; }
[[ -f $DEF ]] || { echo "找不到 $DEF, 先跑 wsl-10-minimize-config.sh"; exit 1; }
cd "$KER"; mkdir -p "$OUT"
cnt() { grep -cE "$1" .config || true; }

publish_kernel_bundle() {
  local krel module_archive module_manifest source_revision
  krel=$(make ARCH=arm64 CROSS_COMPILE="$CC" -s kernelrelease)
  [ "$VARIANT" = minimal ] && [ "$krel" = 6.1.31-opi-min ] || {
    echo "unexpected kernel release for published bundle: $krel" >&2; return 1;
  }
  [ -d "$K/modules/lib/modules/$krel" ] || { echo "modules_install did not install $krel" >&2; return 1; }
  [ "$(find "$K/modules/lib/modules/$krel" -type f -name '*.ko*' | wc -l)" -gt 0 ] || {
    echo "installed module payload is empty" >&2; return 1;
  }
  # modules_install adds build/source links to the build host. They are not usable
  # on the release rootfs and would make the runtime payload host-dependent.
  find "$K/modules/lib/modules/$krel" -maxdepth 1 -type l \( -name build -o -name source \) -delete
  ! find "$K/modules/lib/modules/$krel" -type l -print -quit | grep -q . || {
    echo "module payload contains unexpected symlinks" >&2; return 1;
  }

  module_archive="modules-$krel.tar.gz"
  module_manifest="modules-$krel.sha256"
  rm -f "$K/$module_archive" "$K/$module_manifest" "$K/artifact-manifest.sha256" "$K/provenance.txt"
  (
    cd "$K/modules"
    find "lib/modules/$krel" -type f -print0 | sort -z | xargs -0 sha256sum
  ) > "$K/$module_manifest"
  tar --sort=name --mtime='UTC 1970-01-01' --owner=0 --group=0 --numeric-owner \
    -C "$K/modules" -cf - "lib/modules/$krel" | gzip -n > "$K/$module_archive"
  source_revision=$(git -C "$KER" rev-parse HEAD 2>/dev/null || printf 'unavailable')
  (
    cd "$K"
    sha256sum Image System.map kernel.config dtb/sun50i-h618-orangepi-zero2w.dtb \
      "$module_archive" "$module_manifest"
  ) > "$K/artifact-manifest.sha256"
  {
    printf 'format=zero2w-kernel-bundle-v1\n'
    printf 'kernel_release=%s\n' "$krel"
    printf 'source_tree_revision=%s\n' "$source_revision"
    printf 'manifest_sha256=%s\n' "$(sha256sum "$K/artifact-manifest.sha256" | awk '{print $1}')"
  } > "$K/provenance.txt"
  ( cd "$K" && sha256sum -c artifact-manifest.sha256 )
  ( cd "$K/modules" && sha256sum -c "../$module_manifest" )
  echo "published ABI-bound module bundle: $K/$module_archive"
}

echo "==> [1/4] 配置: $(basename "$DEF")  (VARIANT=$VARIANT)"
cp "$DEF" arch/arm64/configs/zero2w_kernel_defconfig
make ARCH=arm64 CROSS_COMPILE="$CC" zero2w_kernel_defconfig >/dev/null
if [ "$VARIANT" = tiny ]; then
  echo "    tiny: 关闭 MODULES, 所有驱动内建"
  echo "    tiny: 先把所有 =m 关掉(MODULES=n 时 kconfig 会把 m 升成 y, 不先清会更大)"
  sed -i 's/^\(CONFIG_[A-Za-z0-9_]*\)=m$/# \1 is not set/' .config
  sed -i 's/^CONFIG_MODULES=y/# CONFIG_MODULES is not set/' .config
  printf '\nCONFIG_MODULES=n\n' >> .config
  make ARCH=arm64 CROSS_COMPILE="$CC" olddefconfig >/dev/null
fi
echo "    =y $(cnt '=y$')   =m $(cnt '=m$')   -Os=$(cnt 'CC_OPTIMIZE_FOR_SIZE=y')   CMA=$(grep -m1 '^CONFIG_CMA_SIZE_MBYTES=' .config || echo 未设)"

echo "==> [2/4] 编译 Image + dtbs + modules (-j$J)"
# 变体后缀: full=-zero2w, minimal=-opi-min, tiny=-opi-tiny (防止覆盖 /lib/modules 里的同名树)
SUF="-opi-min"
[ "$VARIANT" = tiny ] && SUF="-opi-tiny"
printf '\nCONFIG_LOCALVERSION="%s"\n' "$SUF" >> .config
make ARCH=arm64 CROSS_COMPILE="$CC" olddefconfig >/dev/null
echo "    内核版本后缀: $(grep -m1 "^CONFIG_LOCALVERSION=" .config)"
TGT="Image dtbs modules"
[ "$VARIANT" = tiny ] && TGT="Image dtbs"   # tiny 无模块, make modules 会直接报错
make ARCH=arm64 CROSS_COMPILE="$CC" KCFLAGS="$KCFLAGS" -j"$J" $TGT > "$OUT/build.log" 2>&1
echo "    编译完成, $(grep -cE '^(ERROR|error:)' "$OUT/build.log" || true) 个 error 关键字 (详见 $OUT/build.log)"

echo "==> [3/4] 收集产物"
K="$OUT/kernel"; rm -rf "$K"; mkdir -p "$K/dtb" "$K/modules"
cp arch/arm64/boot/Image "$K/"
cp arch/arm64/boot/dts/allwinner/sun50i-h618-orangepi-zero2w.dtb "$K/dtb/"
cp .config "$K/kernel.config"; cp System.map "$K/System.map"
if [ "$VARIANT" != tiny ]; then
  make ARCH=arm64 CROSS_COMPILE="$CC" INSTALL_MOD_PATH="$K/modules" modules_install >/dev/null
  publish_kernel_bundle
fi
NM=$(find "$K/modules" -name '*.ko*' | wc -l)
MSZ=$(du -sm "$K/modules" | cut -f1)

echo "==> [4/4] 与 full 配置对比"
FULL=$(stat -c %s "$HOME/opi/out/kernel/Image" 2>/dev/null || echo 0)
MIN=$(stat -c %s "$K/Image")
echo "Image  : $(numfmt --to=iec --suffix=B "$MIN") ($MIN 字节)"
if [ "$FULL" -gt 0 ]; then
  awk -v a="$FULL" -v b="$MIN" 'BEGIN{printf "         full=%s (%d 字节)  ->  小 %.1f%%\n", sprintf("%.1f MB", a/1048576), a, 100*(a-b)/a}'
fi
echo "模块   : $NM 个 .ko, ${MSZ} MB   (full=2907 个 / 132 MB)"
echo "内建驱动抽查 (System.map 里符号个数, >0 即真的编进了内核):"
for pat in brcmf sunxi_gmac sun50iw9 aw8738 ac200 cedrus sunxi_sid sun8i_ths mv64xxx musb_hdrc sun4i_spi sunxi_musb; do
  printf "         %-14s %s\n" "$pat" "$(grep -ci "$pat" System.map || true)"
done
if [ "$VARIANT" = tiny ]; then
  make ARCH=arm64 CROSS_COMPILE="$CC" savedefconfig >/dev/null
  cp defconfig "$WIN/configs/zero2w-tiny_defconfig"
  echo "    已输出 configs/zero2w-tiny_defconfig"
fi
rm -f arch/arm64/configs/zero2w_kernel_defconfig
echo "产物目录: $K"
echo "MINIMAL_BUILD_DONE $VARIANT"
