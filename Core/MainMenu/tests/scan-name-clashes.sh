#!/bin/bash
# 只读扫描：冻结头新增的名字在 Core/ Input/ Output/ 里是否还有别的定义
cd /mnt/d/littlethings/CarPlay/zero2w || exit 1
NAMES="MediaInfo DisplayConfig InteractionState AudioState VehicleState TelephonyState LinkState AudioCodec AudioRole Maneuver Point kMaxTextLong kMaxLyrics kMaxArtwork kMaxMultiTouch kMaxContacts kMaxSessionList SessionSnapshot SessionCore InputAdapter ControlEvent AudioChunk VideoFrame SourceState SessionEvent SelectionMode InputSourceKind VideoEncoding"
echo "=== 扫描范围：Core/ Input/ Output/（*.hpp *.h *.cpp *.cxx），已排除 Reference/ Linux/ Temp/ ==="
echo
for n in $NAMES; do
  hits=$(grep -rnE --include='*.hpp' --include='*.h' --include='*.cpp' --include='*.cxx' \
      "(struct|class|enum[[:space:]]+class|enum|constexpr|using)[[:space:]]+${n}\b" Core Input Output 2>/dev/null)
  if [ -n "$hits" ]; then
    cnt=$(printf '%s\n' "$hits" | wc -l)
    if [ "$cnt" -gt 1 ]; then
      echo "!!! $n : $cnt 处（可能撞名）"
    else
      echo "    $n : $cnt 处"
    fi
    printf '%s\n' "$hits" | sed 's/^/        /'
  else
    echo "    $n : 0 处"
  fi
done
echo
echo "=== 单独确认 ControlEvent 内嵌 Point 与其它 Point ==="
grep -rnE --include='*.hpp' --include='*.h' --include='*.cpp' "\bPoint\b" Core Input Output 2>/dev/null | sed 's/^/    /' | head -20
echo
echo "=== 单独确认 mvp 命名空间里所有 struct/class/enum 定义（去重，看有无重复名）==="
grep -rhoE --include='*.hpp' --include='*.h' "(struct|class|enum class|enum)[[:space:]]+[A-Za-z_][A-Za-z0-9_]*" Core Input Output 2>/dev/null \
  | awk '{print $NF}' | sort | uniq -c | sort -rn | awk '$1>1' | sed 's/^/    /'
