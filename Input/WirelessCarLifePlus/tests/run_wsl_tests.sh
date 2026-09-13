#!/usr/bin/env bash
# 在 WSL/Linux 上跑 CarLife+ 车机端的完整测试：编译 -> 单元测试 -> 生成 1080p 码流
# -> 无线端到端（手机模拟器 + 车机端 + 真 H.264 解码 + 出图快照）
# -> 有线/adb 转发拓扑端到端 -> 鉴权失败负向路径
set -uo pipefail
cd "$(dirname "$0")/.."
BUILD=build
OUT=out
rm -rf "$OUT/snaps-wireless" "$OUT/snaps-wired"
mkdir -p "$OUT"
fail=0

step() { printf '\n=========== %s ===========\n' "$*"; }
want() {  # want <file> <pattern> <label>
  if grep -qF -- "$2" "$1"; then echo "PASS  $3"
  else echo "FAIL  $3  (在 $1 里找不到: $2)"; fail=1; fi
}
forbid() {
  if grep -qF -- "$2" "$1"; then echo "FAIL  $3  (不该出现: $2)"; fail=1; else echo "PASS  $3"; fi
}

step "1/5 构建"
# 本模块自己的构建目录（不是 Temp/build-root，避免与其他车道并发构建互相覆盖）。
# 注意：BUILD_WIRELESS_CARLIFE_PLUS 是【根工程】的选项；本目录单独构建时用不到它
# （CMake 会警告 “Manually-specified variables were not used”），是正常的。
cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  >/dev/null || { echo "FAIL cmake"; exit 1; }
cmake --build "$BUILD" -j"$(nproc 2>/dev/null || echo 4)" 2>&1 | tail -25 || { echo "FAIL build"; exit 1; }
test -x "$BUILD/carlife-hu" || { echo "FAIL 没产出 carlife-hu"; exit 1; }
test -x "$BUILD/unit-wire" || { echo "FAIL 没产出 unit-wire"; exit 1; }
echo "PASS 构建完成"

step "2/6 协议层单元测试"
"./$BUILD/unit-wire" || fail=1

step "2c/6 全量接口单测（本轮新增：编解码 + 内容加密往返）"
test -x "$BUILD/unit-features" || { echo "FAIL 没产出 unit-features"; exit 1; }
"./$BUILD/unit-features" > "$OUT/unit-features.log" 2>&1 || fail=1
tail -1 "$OUT/unit-features.log" | sed 's/^/  /'
# 逐项确认关键项真的被覆盖了（不是只看总数）
for k in "MEDIA_INFO 封面 bytes 逐字节一致" \
         "NAV_NEXT_TURN_INFO(Kotlin 版)" \
         "NAV_NEXT_TURN_INFO(C++ 库版) field6 按 varint 读成 time=42" \
         "TOUCH_ACTION_3 action 是大端 4 字节" \
         "BT_HFP_REQUEST 拨号解析一致" \
         "VEHICLE_CONTROL repeated sint32 zigzag 负数正确" \
         "内容加密：RSA 包装的 AES 密钥能被 HU 私钥解出" \
         "内容加密：解密后与原文逐字节一致（含中文）" \
         "内容加密：错误密钥解出的载荷判失败（不静默通过）" \
         "加密后帧头长度自动改成 32（32=20 补齐到 16 的倍数）" \
         "硬键表逐行 == ServiceTypes.kt:358-408" \
         "up 必须是 23(0x17=MOVE_UP)" \
         "down 必须是 24(0x18)" \
         "back 必须是 14(0x0E=BACK)" \
         "NAV_NEXT_TURN_INFO 的 action 原值透传"; do
  want "$OUT/unit-features.log" "$k" "全量接口单测：$k"
done

step "2d/6 端到端：CarLife 全量接口 -> CarLifeInputAdapter -> SessionCore"
# 需要 carlife_input + feature-probe 两个目标（它们要 Core 同时能编译）
if [ -x "$BUILD/feature-probe" ]; then
  # K4 关键：期望值从参考表 ServiceTypes.kt:358-408 抄来，不是我们自己的产物：
  #   up → KEYCODE_MOVE_UP(23) :380；down → MOVE_DOWN(24) :381；back → BACK(14) :371
  # 顺序敏感：TOUCH_CAR_HARD_KEY_CODE 线上只有 keycode 数字、没有名字，
  # 所以用 --inject-keys 与 --expect-keys 约定同一个顺序，mdsim 对不上就打 FAIL。
  # 控件先行（--inject-controls）也会发出硬键，所以期望序列要把它们排在前：
  #   旋钮 Right → MOVE_RIGHT(22) :379；语音 → VR_START(33) :390；DTMF '5' → NUMBER_5(40) :397
  python3 tools/mdsim/mdsim.py --role server --listen-host 127.0.0.1 \
    --video "$OUT/clip1080.h264" --fps 30 --seconds 40 --send-all --send-encryption \
    --expect-keys 22,33,40,23,24,14 \
    > "$OUT/sim-features.log" 2>&1 &
  SIMF=$!
  sleep 1
  timeout 60 "$BUILD/feature-probe" --phone-ip 127.0.0.1 --seconds 12 \
    --inject-controls --inject-keys up,down,back --vehicle-demo \
    > "$OUT/probe-features.log" 2>&1
  wait "$SIMF"; SIMFRC=$?
  echo "  mdsim 退出码=$SIMFRC（硬键自检不过就是 7）"
  if [ "$SIMFRC" -ne 0 ]; then fail=1; fi

  # ── K4 硬键：拿参考表期望值判定，不是回显 ──
  want "$OUT/sim-features.log" "[K4] PASS 硬键 #4 keycode=23 == 参考表期望 23" \
       "K4 硬键 up == KEYCODE_MOVE_UP=23（参考表 :380）"
  want "$OUT/sim-features.log" "[K4] PASS 硬键 #5 keycode=24 == 参考表期望 24" \
       "K4 硬键 down == KEYCODE_MOVE_DOWN=24（参考表 :381）"
  want "$OUT/sim-features.log" "[K4] PASS 硬键 #6 keycode=14 == 参考表期望 14" \
       "K4 硬键 back == KEYCODE_BACK=14（参考表 :371）"
  want "$OUT/sim-features.log" "[K4] PASS 硬键 #1 keycode=22 == 参考表期望 22" \
       "K4 旋钮右 == KEYCODE_MOVE_RIGHT=22（参考表 :379）"
  want "$OUT/sim-features.log" "[K4] PASS 硬键 #2 keycode=33 == 参考表期望 33" \
       "K4 语音键 == KEYCODE_VR_START=33（参考表 :390）"
  want "$OUT/sim-features.log" "[K4] PASS 硬键 #3 keycode=40 == 参考表期望 40" \
       "K4 DTMF '5' == KEYCODE_NUMBER_5=40（参考表 :397）"
  want "$OUT/sim-features.log" "PASS 硬键自检：6 个 keycode 全部等于参考表期望值" \
       "K4 硬键自检整体通过（ 6 项）"
  forbid "$OUT/sim-features.log" "[K4] FAIL" "K4 不得出现任何 FAIL"

  # ── 逐项断言（车机侧解析 + 回写 SessionCore 的快照字段）──
  want "$OUT/probe-features.log" "MEDIA_INFO song=" "A 元数据：收到并解析 MEDIA_INFO"
  want "$OUT/probe-features.log" "has_artwork=1" "B 封面：写进了 SessionCore"
  want "$OUT/probe-features.log" "NAV_NEXT_TURN action=" "C 导航逐向：解析"
  want "$OUT/probe-features.log" "core.nav.valid=1" "C 导航逐向：写进了 SessionCore.nav"
  want "$OUT/probe-features.log" "maneuver_code=7" "C maneuver_code 保留原始 action 值（无码表，不编表）"
  want "$OUT/probe-features.log" "time_remaining_s=90" "C field6 的 C++ 版（int32 time）被解析"
  want "$OUT/probe-features.log" "icon=9B" "C field6 的 Kotlin 版（bytes turnIconData）被解析"
  want "$OUT/probe-features.log" "CAR_DATA_SUBSCRIBE" "D 车况：处理订阅请求"
  want "$OUT/sim-features.log" "车机上报车速" "D 车况：真的上报给手机"
  want "$OUT/probe-features.log" "HFP 请求" "F 电话：处理 HFP 命令"
  want "$OUT/probe-features.log" "TOUCH_ACTION_3 action=" "H 多点触控：反向事件解析"
  want "$OUT/sim-features.log" "车机下发多点触控" "H 多点触控：多指下发给手机"
  want "$OUT/probe-features.log" "手机触摸板" "I 触摸板事件"
  want "$OUT/probe-features.log" "pinch scale=" "J 手势（pinch）"
  want "$OUT/probe-features.log" "麦克风录音控制" "M MIC_RECORD_*"
  want "$OUT/probe-features.log" "车控 type=" "N 车控"
  want "$OUT/probe-features.log" "语音控制 command=" "O 语音控制"
  want "$OUT/probe-features.log" "文件传输" "Q 文件传输"
  want "$OUT/probe-features.log" "激活请求" "R 激活请求 + 应答"
  want "$OUT/probe-features.log" "内容加密状态=3" "R 内容加密走到 Ready"
  want "$OUT/sim-features.log" "收到 HU_RSA_PUBLIC_KEY_RESPONSE" "R 内容加密：公钥已下发"
  want "$OUT/sim-features.log" "收到 HU_AES_REC_RESPONSE" "R 内容加密：AES 密钥已被车机解出"
  want "$OUT/sim-features.log" "收到 MD_ENCRYPT_READY_DONE" "R 内容加密：READY_DONE 已回"
  want "$OUT/probe-features.log" "时间同步" "S TIME_SYNC"
else
  echo "SKIP  端到端：$BUILD 里没有 feature-probe（需要 carlife_input 目标可构建）"
fi

step "2b/6 车机侧 WiFi 热点计划（hostapd/dnsmasq 配置生成）"
"./$BUILD/carlife-hu" --print-ap-plan --wifi-ap wlan0 --ssid zero2w-CarLife --psk carlife12345 --wifi-band 5 \
  > "$OUT/ap-plan-5g.txt" 2>&1
"./$BUILD/carlife-hu" --print-ap-plan --wifi-ap wlan0 --wifi-band 2 > "$OUT/ap-plan-2g.txt" 2>&1
want "$OUT/ap-plan-5g.txt" "hw_mode=a" "5GHz 用 hw_mode=a"
want "$OUT/ap-plan-5g.txt" "ssid=zero2w-CarLife" "SSID 写入 hostapd.conf"
want "$OUT/ap-plan-5g.txt" "wpa_passphrase=carlife12345" "WPA2 口令写入"
want "$OUT/ap-plan-5g.txt" "dhcp-range=192.168.43.1,192.168.43.10" "dnsmasq 分配地址段"
want "$OUT/ap-plan-2g.txt" "hw_mode=g" "2.4GHz 用 hw_mode=g"

step "3/6 生成 1920x1080 测试码流"
ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i "testsrc2=size=1920x1080:rate=30:duration=6" \
  -vf "drawtext=text='CARLIFE+ HU 1920x1080':fontcolor=white:fontsize=72:x=(w-tw)/2:y=(h-th)/2" \
  -c:v libx264 -profile:v baseline -level 3.1 -pix_fmt yuv420p -g 30 -f h264 "$OUT/clip1080.h264" \
  || { echo "FAIL ffmpeg 生成码流"; exit 1; }
ls -l "$OUT/clip1080.h264"

step "4/6 无线端到端（车机主动连手机 7240/8240/9240/9241/9242/9340）"
python3 tools/mdsim/mdsim.py --role server --listen-host 127.0.0.1 \
  --video "$OUT/clip1080.h264" --fps 30 --seconds 12 --expect-size 1920x1080 \
  > "$OUT/sim-wireless.log" 2>&1 &
SIM=$!
sleep 0.8
"./$BUILD/carlife-hu" --phone-ip 127.0.0.1 --width 1920 --height 1080 --fps 30 \
  --seconds 12 --min-frames 40 --auth dev --auth-secret zero2w-shared-secret \
  --snapshot-dir "$OUT/snaps-wireless" --snapshot-every 2 --inject-touch 500,500 \
  --bt-name CarLife-HU --bt-mac 00:11:22:33:44:55 > "$OUT/hu-wireless.log" 2>&1
wait "$SIM"; SIMRC=$?
echo "车机退出码=$?  手机模拟器退出码=$SIMRC"
want "$OUT/hu-wireless.log" "sent HU_PROTOCOL_VERSION" "车机发出首条协议版本消息"
want "$OUT/hu-wireless.log" "phone matchStatus=1" "手机接受协议版本"
want "$OUT/hu-wireless.log" "MD_INFO brand=Xiaomi" "解析手机 MD_INFO"
want "$OUT/hu-wireless.log" "sent HU_AUTH_REQUEST" "车机发起鉴权"
want "$OUT/hu-wireless.log" "mode=dev: expect=sim-" "鉴权校验分支被真实执行"
want "$OUT/hu-wireless.log" "phone reported MD_AUTH_RESULT authenResult=true" "双向鉴权完成"
want "$OUT/hu-wireless.log" "sent VIDEO_ENCODER_INIT 1920x1080@30" "默认协商分辨率 1920x1080@30"
want "$OUT/hu-wireless.log" "phone VIDEO_ENCODER_INIT_DONE -> sent VIDEO_ENCODER_START" "进入推流"
want "$OUT/hu-wireless.log" "first frame decoded" "解出第一帧并显示"
want "$OUT/hu-wireless.log" "RESULT: OK" "有画面输出"
want "$OUT/hu-wireless.log" "displayResolution=1920x1080" "显示分辨率 1920x1080"
want "$OUT/sim-wireless.log" "收到 VIDEO_ENCODER_INIT 1920x1080@30" "手机侧看到 1920x1080@30 协商"
want "$OUT/sim-wireless.log" "会话建立 (双向鉴权完成)" "手机侧会话建立"
want "$OUT/sim-wireless.log" "收到车机视频通道心跳" "车机按 1s 发视频通道心跳"
want "$OUT/sim-wireless.log" "HU_FEATURE_CONFIG_RESPONSE" "能力协商往返"
want "$OUT/hu-wireless.log" "注入触控" "触控回传：车机下发"
want "$OUT/sim-wireless.log" "车机下发触控" "触控回传：手机侧收到"

step "4b 快照出图校验（BMP 实际尺寸 + 非纯色）"
python3 - "$OUT" <<'PY' || fail=1
import os, struct, sys
out = sys.argv[1]
ok = False
for sub in ("snaps-wireless",):
    d = os.path.join(out, sub)
    if not os.path.isdir(d):
        print("FAIL 没有快照目录", d); continue
    files = sorted(f for f in os.listdir(d) if f.endswith(".bmp"))
    if not files:
        print("FAIL 目录里没有 bmp", d); continue
    p = os.path.join(d, files[-1])
    b = open(p, "rb").read()
    w, h = struct.unpack("<ii", b[18:26])
    body = b[54:]
    uniq = len(set(body[:200000][i:i+3] for i in range(0, min(len(body)-3, 200000), 7)))
    print("PASS 快照 %s  尺寸=%dx%d  文件=%d 字节  采样到的颜色种类=%d" % (os.path.basename(p), w, h, len(b), uniq))
    ok = (w == 1920 and h == 1080 and uniq > 8)
print("PASS 快照确实是 1920x1080 的真实画面" if ok else "FAIL 快照尺寸/内容不符合预期")
sys.exit(0 if ok else 1)
PY

step "5/6 有线(adb 转发)拓扑 + 鉴权失败负向路径"
"$BUILD/carlife-hu" --listen --seconds 10 --min-frames 30 --auth dev \
  --auth-secret zero2w-shared-secret --snapshot-dir "$OUT/snaps-wired" \
  > "$OUT/hu-wired.log" 2>&1 &
HUS=$!
sleep 0.8
python3 tools/mdsim/mdsim.py --role client --host 127.0.0.1 --video "$OUT/clip1080.h264" \
  --seconds 10 --expect-size 1920x1080 > "$OUT/sim-wired.log" 2>&1
wait "$HUS"
want "$OUT/hu-wired.log" "listening on HU ports 7200/8200/9200" "车机在 7200/8200/... 监听"
want "$OUT/hu-wired.log" "RESULT: OK" "有线拓扑也有画面"
want "$OUT/sim-wired.log" "已连车机 channel=1" "手机侧连入车机 CMD:7200"

python3 tools/mdsim/mdsim.py --role server --listen-host 127.0.0.1 --video "$OUT/clip1080.h264" \
  --seconds 8 > "$OUT/sim-deny.log" 2>&1 &
SIMD=$!
sleep 0.8
"./$BUILD/carlife-hu" --phone-ip 127.0.0.1 --seconds 8 --auth deny --retry-seconds 5 > "$OUT/hu-deny.log" 2>&1
RC_DENY=$?
wait "$SIMD"
echo "负向用例车机退出码=$RC_DENY"
want "$OUT/hu-deny.log" "mode=deny" "负向：鉴权被判失败"
want "$OUT/hu-deny.log" "auth failed, closing per protocol" "负向：按协议 5 秒后主动断开"
want "$OUT/sim-deny.log" "authenResult=False" "负向：手机收到 false"
want "$OUT/hu-deny.log" "RESULT: NO_VIDEO" "负向：没有画面"

step "6/6 蓝牙引导 -> 完整无线链路（SPP 承载同一套 CMD 帧；无蓝牙硬件时用 AF_UNIX 桥接）"
BT_SOCK="/tmp/carlife-hu-bt.$$.sock"   # WSL 的 /mnt/d 是 DrvFs，AF_UNIX 只能放 /tmp
"./$BUILD/carlife-hu" --wifi-ap wlan0 --bt-local "$BT_SOCK" --bt-wifi-name zero2w-AP --bt-timeout 20 \
  --retry-seconds 12 --auth dev --auth-secret zero2w-shared-secret --bt-advertise \
  --seconds 12 --min-frames 30 --snapshot-dir "$OUT/snaps-bt" > "$OUT/hu-bt.log" 2>&1 &
HUB=$!
sleep 0.8
python3 tools/mdsim/mdsim.py --bt-connect "$BT_SOCK" --report-ip 127.0.0.1 --role server \
  --listen-host 127.0.0.1 --video "$OUT/clip1080.h264" --seconds 12 --expect-size 1920x1080 \
  > "$OUT/sim-bt.log" 2>&1
BTRC=$?
wait "$HUB"
want "$OUT/hu-bt.log" "桥接模式)监听" "车机建立蓝牙(SPP)链路监听"
want "$OUT/hu-bt.log" "应答 WIRELESS_INFO_RESPONSE" "应答手机的 WIRELESS_INFO_REQUEST"
want "$OUT/hu-bt.log" "应答 WIRELESS_TARGET_INFO_RESPONSE" "应答手机的 TARGET_INFO_REQUEST"
want "$OUT/hu-bt.log" "已发 WIRELESS_REQUEST_IP" "车机主动索要手机 IP"
want "$OUT/hu-bt.log" "手机 IP = 127.0.0.1" "从蓝牙链路拿到手机地址"
want "$OUT/hu-bt.log" "手机 MD_STATUS = 1" "处理手机的 MD_STATUS"
want "$OUT/hu-bt.log" "RESULT: OK" "蓝牙引导后完整无线链路出画面"
want "$OUT/sim-bt.log" "收到车机 WIRELESS_INFO_RESPONSE" "手机侧看到车机能力"
want "$OUT/sim-bt.log" "回报手机地址 127.0.0.1" "手机侧完成 IP 回报"
want "$OUT/hu-bt.log" "热点未启动（继续走已有的网络/桥接路径）" "无无线网卡时热点优雅降级，不阻塞后续引导"

printf '\n=========== 结果 ===========\n'
grep -h '^framesDecoded=\|^displayResolution=\|^videoBytes=\|^audioBytes=\|^heartbeatsSent=' "$OUT/hu-wireless.log" | sed 's/^/  无线: /'
grep -h '^framesDecoded=\|^displayResolution=' "$OUT/hu-wired.log" | sed 's/^/  有线: /'
grep -h "^framesDecoded=\|^displayResolution=" "$OUT/hu-bt.log" | sed 's/^/  蓝牙引导: /'
if [ "$fail" -eq 0 ]; then echo "ALL TESTS PASSED"; else echo "存在失败项 ($fail)"; fi
exit $fail
