#!/usr/bin/env bash
# u-boot v2024.01 的 pylibfdt 与 swig 4.3 冲突 -> 先看清依赖来源
set -uo pipefail
UB="$HOME/opi/src/01-soc-h616-h618-boot-kernel/u-boot-orangepi-v2024.01"
cd "$UB" || exit 1
echo "=== dts/Kconfig 1-30 行 ==="
sed -n "1,30p" dts/Kconfig
echo "=== select PYLIBFDT 出现处 ==="
grep -rn "select PYLIBFDT" --include=Kconfig . | head
echo "=== .config 相关项 ==="
grep -E "^(CONFIG_(PYLIBFDT|OF_UPSTREAM|BINMAN|OF_SEPARATE|FIT)|# CONFIG_(PYLIBFDT|OF_UPSTREAM|BINMAN))" .config | head -12