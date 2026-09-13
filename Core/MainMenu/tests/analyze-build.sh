#!/bin/bash
# 归因分析：全量构建日志里，失败分别属于哪个模块
cd /mnt/d/littlethings/CarPlay/zero2w || exit 1
L=Temp/full-build.txt
echo "=== 1) 成功构建的 target ==="
grep -a -oE 'Built target [A-Za-z0-9_-]+' "$L" | sort -u | sed 's/^/    /'
echo
echo "=== 2) 失败/中止的模块（CMakeFiles/<mod>.dir） ==="
grep -a -oE 'CMakeFiles/[A-Za-z0-9_-]+\.dir' "$L" | sort -u | sed 's/^/    /'
echo
echo "=== 3) 报错文件按目录归类 ==="
grep -a -oE '^[^ ]*\.(cpp|hpp|h):[0-9]+:[0-9]+: error' "$L" \
  | sed 's/:.*//' | sed 's|^/mnt/d/littlethings/CarPlay/zero2w/||' \
  | awk -F/ '{if(NF>=2)print $1"/"$2; else print $0}' | sort | uniq -c | sort -rn | sed 's/^/    /'
echo
echo "=== 4) 是否还有 DisplayConfig 撞名（应为 0）==="
grep -a -c 'redefinition of' "$L" | sed 's/^/    redefinition 次数: /'
grep -a -n 'DisplayConfig' "$L" | head -5 | sed 's/^/    /' || echo "    （日志中无 DisplayConfig 相关错误）"
echo
echo "=== 5) Core/MainMenu 是否有任何错误（应为 0）==="
grep -a -E 'Core/MainMenu' "$L" | grep -a -E 'error|warning' | head -5 | sed 's/^/    /' || echo "    0 处"
echo
echo "=== 6) 全部错误首行（去重，看还剩几类）==="
grep -a -oE 'error: .*' "$L" | sed 's/error: //' | sort -u | head -20 | sed 's/^/    /'
