#!/bin/bash
cd /mnt/d/littlethings/CarPlay/zero2w || exit 1
echo "=== 1) 新增常量在 session_core.hpp 之外是否还有定义/重名 ==="
for n in kMaxTextLong kMaxLyrics kMaxArtwork kMaxMultiTouch kMaxContacts kMaxSessionList; do
  c=$(grep -rnE --include='*.hpp' --include='*.h' --include='*.cpp' --include='*.cxx' \
      "(constexpr|#define|enum|using)[^;]*[^A-Za-z0-9_]${n}\b" Core Input Output 2>/dev/null \
      | grep -v 'MainMenu/include/core/session_core.hpp' | wc -l)
  printf '  %-18s 非 session_core 定义数=%s\n' "$n" "$c"
done
echo
echo "=== 2) kMaxAudioBytes / kMaxVideoBytes / kMaxControls 是否唯一 ==="
for n in kMaxAudioBytes kMaxVideoBytes kMaxControls kMaxText kMaxSources; do
  hits=$(grep -rnE --include='*.hpp' --include='*.h' --include='*.cpp' "constexpr[^;]*${n}\b" Core Input Output 2>/dev/null | sed 's/^/        /')
  printf '  %-16s\n' "$n"; printf '%s\n' "$hits"
done
echo
echo "=== 3) catplay_media_ext.hpp 的触点点结构名（确认不撞 mvp::ControlEvent::Point）==="
sed -n '28,50p' Input/WirelessCarPlay/include/wirelesscarplay/catplay_media_ext.hpp | cat -n | sed 's/^/    /'
