#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""CarLife+ 手机端 (MD) 模拟器 —— 在 WSL/Linux 上驱动 carlife-hu 做端到端测试。

协议完全按参考实现复刻（见 ../..//SPEC.md）：
  通道/端口  : carlife-sdk/.../receiver/transport/wirless/WirlessConnector.kt:20-26
  帧头       : carlife-sdk/.../internal/protocol/CarLifeMessage.kt + BitConverter.kt（全大端）
  鉴权/建会  : carlife-sdk/.../receiver/protocol/v1/ConnectionEstablishHandler.kt
两种拓扑：
  --role server （无线：车机主动连手机的 7240/8240/...，本脚本扮演手机监听）
  --role client （有线/adb 转发：本脚本连车机的 7200/8200/...）
"""
import argparse
import base64
import math
import os
import socket
import struct
import sys
import threading
import time
import uuid

CMD, VIDEO, MEDIA, TTS, VR, TOUCH, UPDATE = 1, 2, 3, 4, 5, 6, 7
MD_PORTS = {CMD: 7240, VIDEO: 8240, MEDIA: 9240, TTS: 9241, VR: 9242, TOUCH: 9340, UPDATE: 9440}
HU_PORTS = {CMD: 7200, VIDEO: 8200, MEDIA: 9200, TTS: 9201, VR: 9202, TOUCH: 9300, UPDATE: 9400}

# 蓝牙引导消息（InstantConnectionSetup.kt:427-434）
WIRELESS_INFO_REQUEST = 0x00100001
WIRELESS_INFO_RESPONSE = 0x00108002
WIRELESS_TARGET_INFO_REQUEST = 0x00100004
WIRELESS_TARGET_INFO_RESPONSE = 0x00108005
WIRELESS_REQUEST_IP = 0x00108006
WIRELESS_RESPONSE_IP = 0x00100007
WIRELESS_MD_STATUS = 0x00100008
WIRELESS_HU_STATUS = 0x00108009
SHORT_HDR_CHANNELS = (CMD, TOUCH)

HU_PROTOCOL_VERSION = 0x00018001
PROTOCOL_VERSION_MATCH_STATUS = 0x00010002
HU_INFO = 0x00018003
MD_INFO = 0x00010004
STATISTIC_INFO = 0x00018027
VIDEO_ENCODER_INIT = 0x00018007
VIDEO_ENCODER_INIT_DONE = 0x00010008
VIDEO_ENCODER_START = 0x00018009
VIDEO_ENCODER_FRAME_RATE_CHANGE = 0x0001800C
MD_VIDEO_ENCODER_REQ = 0x0001000F
MODULE_STATUS = 0x00010026
GO_TO_DESKTOP = 0x00010021
HU_AUTH_REQUEST = 0x00018048
MD_AUTH_RESPONSE = 0x00010049
HU_AUTH_RESULT = 0x0001804A
MD_AUTH_RESULT = 0x0001004B
MD_AUTH_RESULT_RESPONSE = 0x0001804C
MD_FEATURE_CONFIG_REQUEST = 0x00010051
HU_FEATURE_CONFIG_RESPONSE = 0x00018052
MD_EXIT = 0x00010059

# ── 本轮「全量接口」新增的消息 ID（与 include/carlife/service_types.h 一一对应）──
# ID 来源：ServiceTypes.kt + 官方 C++ 库 CTranRecvPackageProcess.h 交叉核对。
GEAR_INFO = 0x00010029
NAV_NEXT_TURN_INFO = 0x00010030
NAV_ASSISTANT_GUIDE = 0x00010047      # C++ 库版本（Kotlin 版是 0x00018047，两个都发一遍）
NAV_ASSISTANT_GUIDE_HU = 0x00018047
MEDIA_INFO = 0x00010035
MEDIA_PROGRESS_BAR = 0x00010036
CAR_DATA_SUBSCRIBE = 0x00010031
CAR_DATA_SUBSCRIBE_DONE = 0x00010032
CAR_DATA_START_REQ = 0x00010033
CAR_DATA_STOP_REQ = 0x00010034
CARLIFE_DATA_REQ = 0x00010043
CARLIFE_DATA_SUBSCRIBE = 0x00018043
CARLIFE_DATA_SUBSCRIBE_DONE = 0x00010044
CARLIFE_DATA_SUBSCRIBE_DONE_RSP = 0x00018044
TEL_STATE_INCOMING = 0x00010014
TEL_STATE_OUTGOING = 0x00010015
TEL_STATE_IDLE = 0x00010016
TEL_STATE_INCALLING = 0x00010017
MIC_RECORD_WAKEUP_START = 0x00010022
MIC_RECORD_END = 0x00010023
MIC_RECORD_RECOG_START = 0x00010024
TOUCH_PAD_DOWN = 0x0001005A
TOUCH_PAD_MOVE = 0x0001005B
TOUCH_PAD_UP = 0x0001005C
TOUCH_PAD_PINCH = 0x0001005D
BT_HFP_REQUEST = 0x00010040
BT_HFP_CONNECTION = 0x00018042
BT_HFP_RESPONSE = 0x0001804E
BT_HFP_STATUS_REQUEST = 0x0001004F
BT_HFP_STATUS_RESPONSE = 0x00018050
BT_HFP_CALL_STATUS_COVER = 0x00010058
VEHICLE_CONTROL = 0x0001006F
VEHICLE_CONTROL_INFO = 0x00018061
HU_VOICE_CONTROL = 0x00010066
BOX_ACTIVE = 0x00016001
HU_ACTIVE = 0x00016002
TIME_SYNC = 0x00010060
MD_RSA_PUBLIC_KEY_REQUEST = 0x0001006A
HU_RSA_PUBLIC_KEY_RESPONSE = 0x0001806B
MD_AES_KEY_SEND_REQUEST = 0x0001006C
HU_AES_REC_RESPONSE = 0x0001806D
MD_ENCRYPT_READY = 0x0001006E
MD_ENCRYPT_READY_DONE = 0x0001806F
MD_CARLIFE_DATA_REQ = 0x00010043
VIDEO_ENCODER_FRAME_RATE_CHANGE_DONE = 0x0001000D
# UPDATE 通道：文件传输（G 类）
SEND_START = 0x00070001
SENDING_DATA = 0x00070002
SEND_FINISH = 0x00070003
SEND_STOP = 0x00070004
STOP_RECEIVE = 0x00070005
DATA_MD_TRANSFER_START = 0x00070007
DATA_MD_TRANSFER_SEND = 0x00070008
DATA_MD_TRANSFER_END = 0x00070009
DATA_HU_UPDATE_START = 0x0007800A
DATA_HU_UPDATE_END = 0x0007800B
# 车机下行（手机侧应该收到）
CAR_VELOCITY = 0x0001800F
CAR_GPS = 0x00018010
CAR_GYROSCOPE = 0x00018011
CAR_ACCELERATION = 0x00018012
CAR_OIL = 0x00018013
CAR_GEAR = 0x00018029
TOUCH_ACTION_3 = 0x0006800E
TOUCH_CAR_HARD_KEY_CODE = 0x00068008

# CarLife 硬键表（ServiceTypes.kt:358-408）——不是 Android 的 ADB keycode。
KEYCODE = {
    "HOME": 0x01, "PHONE_CALL": 0x02, "PHONE_END": 0x03, "HFP": 0x05,
    "SELECTOR_NEXT": 0x06, "SELECTOR_PREVIOUS": 0x07, "SETTING": 0x08,
    "MEDIA": 0x09, "NAV": 0x0B, "BACK": 0x0E, "MUTE": 0x13, "OK": 0x14,
    "MOVE_LEFT": 0x15, "MOVE_RIGHT": 0x16, "MOVE_UP": 0x17, "MOVE_DOWN": 0x18,
    "TEL": 0x1D, "MAIN": 0x1E, "MEDIA_START": 0x1F, "VR_START": 0x21,
    "VR_STOP": 0x22,
}
_REV_KEYCODE = {v: k for k, v in KEYCODE.items()}

# K4 硬键自检的期望值列表（由 --expect-keys 填充）。
# 用全局而非参数透传：handle_sideband 是闭包，但它在 keycode 分支里需要按顺序消费。
KEY_EXPECT = {"list": []}


def f_bytes(field, b):
    return f_str(field, b)


def f_double(field, v):
    return tag(field, 1) + struct.pack("<d", float(v))


def f_float(field, v):
    return tag(field, 5) + struct.pack("<f", float(v))


def f_sint32(field, v):
    zz = (int(v) << 1) ^ (int(v) >> 31)
    return tag(field, 0) + varint(zz & 0xFFFFFFFF)


def f_bool(field, v):
    return f_int(field, 1 if v else 0)


def parse_touch_action_3(payload):
    """MSG_TOUCH_ACTION_3：action(4B BE) + [pointerId(1B) x(2B BE) y(2B BE)]*。"""
    if len(payload) < 4 or (len(payload) - 4) % 5 != 0:
        return None, []
    action = struct.unpack(">i", payload[:4])[0]
    pts = []
    for i in range(4, len(payload), 5):
        pid = payload[i]
        x = struct.unpack(">h", payload[i + 1:i + 3])[0]
        y = struct.unpack(">h", payload[i + 3:i + 5])[0]
        pts.append((pid, x, y))
    return action, pts
SCREEN_ON = 0x00010018
TIME_SYNC = 0x00010060
VIDEO_DATA = 0x00020001
VIDEO_HEARTBEAT = 0x00020002
MEDIA_INIT = 0x00030001
MEDIA_DATA = 0x00030006
# ---- A 档实现验证用（见 Temp/audit/A-IMPLEMENTATION-REPORT.md）----
# 全部取自本仓 Input/WirelessCarLifePlus/include/carlife/service_types.h
MEDIA_PROGRESS_BAR = 0x00010036        # A2：CarlifeMediaProgressBar{required int32 progressBar=1}
NAV_TTS_INIT = 0x00040001              # A1：CarlifeTTSInit{sampleRate,channelConfig,sampleFormat}
NAV_TTS_DATA = 0x00040003              # A1：TTS PCM 数据
NAV_TTS_END = 0x00040002               # A1：TTS 结束（车机应恢复媒体音量）
DATA_MD_TRANSFER_START = 0x00070007    # A3：UPDATE 通道
DATA_MD_TRANSFER_SEND = 0x00070008
DATA_MD_TRANSFER_END = 0x00070009
TOUCH_ACTION_DOWN = 0x00068002
TOUCH_ACTION_UP = 0x00068003
TOUCH_ACTION_MOVE = 0x00068004
TOUCH_CAR_HARD_KEY_CODE = 0x00068008

T0 = time.time()


def log(msg):
    sys.stdout.write("SIM  %6.3f  %s\n" % (time.time() - T0, msg))
    sys.stdout.flush()


# ------------------------------------------------------------------ protobuf
def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def tag(field, wire):
    return varint((field << 3) | wire)


def f_int(field, v):
    return tag(field, 0) + varint(int(v) & ((1 << 64) - 1))


def f_str(field, s):
    b = s.encode("utf-8") if isinstance(s, str) else s
    return tag(field, 2) + varint(len(b)) + b


def f_msg(field, payload):
    return tag(field, 2) + varint(len(payload)) + payload


def pb_fields(buf):
    """把 protobuf 解成 [(field, wire, value)]；value 对 varint 是 int，对 LEN 是 bytes。"""
    res = []
    i = 0
    n = len(buf)
    while i < n:
        key = 0
        shift = 0
        while True:
            if i >= n:
                return res
            b = buf[i]
            i += 1
            key |= (b & 0x7F) << shift
            if not (b & 0x80):
                break
            shift += 7
        field, wire = key >> 3, key & 7
        if wire == 0:
            v = 0
            shift = 0
            while True:
                if i >= n:
                    return res
                b = buf[i]
                i += 1
                v |= (b & 0x7F) << shift
                if not (b & 0x80):
                    break
                shift += 7
            if v >= (1 << 63):
                v -= 1 << 64
            res.append((field, wire, v))
        elif wire == 2:
            ln = 0
            shift = 0
            while True:
                if i >= n:
                    return res
                b = buf[i]
                i += 1
                ln |= (b & 0x7F) << shift
                if not (b & 0x80):
                    break
                shift += 7
            res.append((field, wire, buf[i:i + ln]))
            i += ln
        elif wire == 5:
            res.append((field, wire, buf[i:i + 4]))
            i += 4
        elif wire == 1:
            res.append((field, wire, buf[i:i + 8]))
            i += 8
        else:
            return res
    return res


def first(buf, field, default=None):
    for f, _w, v in pb_fields(buf):
        if f == field:
            return v
    return default


def strs(buf):
    out = {}
    for f, w, v in pb_fields(buf):
        if w == 2:
            try:
                out[f] = v.decode("utf-8")
            except UnicodeDecodeError:
                out[f] = repr(v[:16])
    return out


# ------------------------------------------------------------------ 帧
def encode_frame(channel, service_type, payload=b"", ts=0):
    if channel in SHORT_HDR_CHANNELS:
        if len(payload) > 0xFFFF:
            raise ValueError("CMD/TOUCH 通道长度域只有 16 位")
        head = struct.pack(">H", len(payload)) + b"\x00\x00" + struct.pack(">I", service_type)
    else:
        head = struct.pack(">III", len(payload), ts & 0xFFFFFFFF, service_type)
    return head + payload


def header_len(channel):
    return 8 if channel in SHORT_HDR_CHANNELS else 12


class Conn:
    """一个通道的连接 + 发送锁。"""

    # 内容加密（AUDIT 3.1 / 5.4）：照拄 EncryptionTool.kt 的语义——
    #   * encrypt 只加密 commandSize 之后的 payload，并修正 payloadSize；
    #   * 解密失败把 serviceType 加入 decryptExcludes，之后不再尝试（按明文处理）。
    # 这两个都是【类级】状态：加密一旦就绪就是整条连接（所有通道）的事。
    CRYPTO = {"key": None, "ready": False, "excludes": set()}

    @classmethod
    def _encrypt(cls, stype, payload):
        if not cls.CRYPTO["ready"] or not payload:
            return payload
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
        pad = 16 - (len(payload) % 16)
        data = payload + bytes([pad]) * pad
        enc = Cipher(algorithms.AES(cls.CRYPTO["key"]), modes.ECB()).encryptor()
        return enc.update(data) + enc.finalize()

    @classmethod
    def _decrypt(cls, stype, payload):
        if not cls.CRYPTO["ready"] or not payload:
            return payload
        if stype in cls.CRYPTO["excludes"]:
            return payload
        if len(payload) % 16 != 0:
            cls.CRYPTO["excludes"].add(stype)
            log("[R] 解密失败(非块对齐)，0x%08X 加入 decryptExcludes" % stype)
            return payload
        try:
            from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
            dec = Cipher(algorithms.AES(cls.CRYPTO["key"]), modes.ECB()).decryptor()
            plain = dec.update(payload) + dec.finalize()
            pad = plain[-1]
            if pad < 1 or pad > 16 or plain[-pad:] != bytes([pad]) * pad:
                raise ValueError("bad padding")
            return plain[:-pad]
        except Exception as exc:  # 任何失败都按参考实现处理：加入例外，不报错退出
            cls.CRYPTO["excludes"].add(stype)
            log("[R] 解密失败(%s)，0x%08X 加入 decryptExcludes" % (exc, stype))
            return payload

    def __init__(self, channel, sock):
        self.channel = channel
        self.sock = sock
        self.lock = threading.Lock()
        self.alive = True

    def send(self, service_type, payload=b"", ts=None):
        if not self.alive:
            return False
        payload = Conn._encrypt(service_type, payload)
        if ts is None:
            ts = int(time.time() * 1000) & 0xFFFFFFFF
        data = encode_frame(self.channel, service_type, payload, ts)
        try:
            with self.lock:
                self.sock.sendall(data)
            return True
        except OSError as exc:
            log("send failed on channel %d: %s" % (self.channel, exc))
            self.alive = False
            return False

    def read_frame(self):
        """返回 (service_type, ts, payload)；None 表示连接关闭。"""
        n = header_len(self.channel)
        head = self._read_exactly(n)
        if head is None:
            return None
        if self.channel in SHORT_HDR_CHANNELS:
            ln = struct.unpack(">H", head[0:2])[0]
            stype = struct.unpack(">I", head[4:8])[0]
            ts = 0
        else:
            ln, ts, stype = struct.unpack(">III", head[0:12])
        body = self._read_exactly(ln) if ln else b""
        if body is None:
            return None
        # 入站解密（照拄 EncryptionTool.decrypt：失败就加入例外，按明文继续）
        body = Conn._decrypt(stype, body)
        return stype, ts, body

    def _read_exactly(self, n):
        buf = bytearray()
        while len(buf) < n:
            try:
                chunk = self.sock.recv(min(65536, n - len(buf)))
            except OSError:
                self.alive = False
                return None
            if not chunk:
                self.alive = False
                return None
            buf += chunk
        return bytes(buf)


def serve_md_ports(host, timeout, include_update):
    conns = {}
    listeners = {}
    order = [CMD, VIDEO, MEDIA, TTS, VR, TOUCH] + ([UPDATE] if include_update else [])
    for ch in order:
        ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        ls.bind((host, MD_PORTS[ch]))
        ls.listen(2)
        ls.settimeout(timeout)
        listeners[ch] = ls
    deadline = time.time() + timeout
    for ch in order:
        left = max(1.0, deadline - time.time())
        listeners[ch].settimeout(left)
        try:
            s, addr = listeners[ch].accept()
        except socket.timeout:
            log("等待车机连入 :%d 超时" % MD_PORTS[ch])
            break
        s.settimeout(None)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        conns[ch] = Conn(ch, s)
        log("车机连入 channel=%d from %s" % (ch, addr[0]))
        listeners[ch].close()
    for ch, ls in listeners.items():
        try:
            ls.close()
        except OSError:
            pass
    return conns


def connect_hu_ports(host, timeout, include_update):
    conns = {}
    order = [CMD, VIDEO, MEDIA, TTS, VR, TOUCH] + ([UPDATE] if include_update else [])
    for ch in order:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(3.0)
        try:
            s.connect((host, HU_PORTS[ch]))
        except OSError as exc:
            log("连车机 :%d 失败: %s" % (HU_PORTS[ch], exc))
            s.close()
            continue
        s.settimeout(None)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        conns[ch] = Conn(ch, s)
        log("已连车机 channel=%d :%d" % (ch, HU_PORTS[ch]))
    return conns


# ------------------------------------------------------------------ H.264
def split_access_units(data):
    """把 Annex-B 流切成 access unit（SPS/PPS 与后面的 IDR 打包在一起）。"""
    starts = []
    i = 0
    n = len(data)
    while i + 3 <= n:
        if data[i] == 0 and data[i + 1] == 0 and data[i + 2] == 1:
            sc = 3
        elif i + 4 <= n and data[i] == 0 and data[i + 1] == 0 and data[i + 2] == 0 and data[i + 3] == 1:
            sc = 4
        else:
            i += 1
            continue
        nal_at = i + sc
        if nal_at >= n:
            break
        starts.append((i, nal_at, sc))
        i = nal_at + 1
    aus = []
    pending = b""
    for idx, (sc_at, nal_at, sc) in enumerate(starts):
        end = starts[idx + 1][0] if idx + 1 < len(starts) else n
        nal_type = data[nal_at] & 0x1F
        chunk = data[sc_at:end]
        if nal_type in (1, 5):
            aus.append(pending + chunk)
            pending = b""
        elif nal_type in (6, 7, 8, 9, 12, 13, 14):
            pending += chunk
        else:
            pending += chunk
    if pending:
        aus.append(pending)
    return aus


def bt_encode(stype, payload=b""):
    return struct.pack(">H", len(payload)) + b"\x00\x00" + struct.pack(">I", stype) + payload


def bt_read(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return bytes(buf)


def bt_recv_msg(sock):
    head = bt_read(sock, 8)
    if head is None:
        return None
    ln = struct.unpack(">H", head[0:2])[0]
    stype = struct.unpack(">I", head[4:8])[0]
    body = bt_read(sock, ln) if ln else b""
    if body is None:
        return None
    return stype, body


def bt_bringup_as_phone(path, report_ip, timeout=15.0):
    """手机端：连上车机的蓝牙链路（桥接用 AF_UNIX），跑 4 步把手机 IP 交出去。"""
    deadline = time.time() + timeout
    s = None
    while time.time() < deadline:
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(path)
            break
        except OSError:
            s.close()
            s = None
            time.sleep(0.2)
    if s is None:
        log("FAIL 连不上车机的蓝牙链路 %s" % path)
        return False
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVTIMEO, struct.pack("@ll", int(timeout), 0))
    log("蓝牙链路已连上车机 (%s)" % path)
    s.sendall(bt_encode(WIRELESS_INFO_REQUEST, f_int(1, 1) + f_int(2, 1)))
    got = bt_recv_msg(s)
    if not got or got[0] != WIRELESS_INFO_RESPONSE:
        log("FAIL 没收到 WIRELESS_INFO_RESPONSE: %r" % (got,))
        return False
    log("收到车机 WIRELESS_INFO_RESPONSE type=%s freq=%s" % (first(got[1], 1), first(got[1], 2)))
    s.sendall(bt_encode(WIRELESS_TARGET_INFO_REQUEST))
    got = bt_recv_msg(s)
    if not got or got[0] != WIRELESS_TARGET_INFO_RESPONSE:
        log("FAIL 没收到 WIRELESS_TARGET_INFO_RESPONSE: %r" % (got,))
        return False
    name = first(got[1], 1, b"")
    log("收到车机 WIRELESS_TARGET_INFO_RESPONSE wifiDeviceName=%s"
        % (name.decode("utf-8", "replace") if isinstance(name, bytes) else name))
    got = bt_recv_msg(s)
    if not got or got[0] != WIRELESS_REQUEST_IP:
        log("FAIL 没收到 WIRELESS_REQUEST_IP: %r" % (got,))
        return False
    log("收到 WIRELESS_REQUEST_IP，回报手机地址 %s" % report_ip)
    s.sendall(bt_encode(WIRELESS_RESPONSE_IP, f_str(1, report_ip)))
    s.sendall(bt_encode(WIRELESS_MD_STATUS, f_int(1, 1)))
    got = bt_recv_msg(s)
    if got and got[0] == WIRELESS_HU_STATUS:
        log("收到车机 WIRELESS_HU_STATUS status=%s" % first(got[1], 1))
    s.close()
    return True


def dev_digest(secret_and_seed):
    h = 1469598103934665603
    for b in secret_and_seed.encode("utf-8"):
        h ^= b
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return "sim-%016x" % h


def pcm_sine(ms=100, rate=44100, ch=2, freq=440.0):
    count = int(rate * ms / 1000)
    out = bytearray()
    for i in range(count):
        v = int(9000 * math.sin(2 * math.pi * freq * i / rate))
        for _ in range(ch):
            out += struct.pack("<h", v)
    return bytes(out)


# ------------------------------------------------------------------ 主流程
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--role", choices=["server", "client"], default="server",
                    help="server=无线(车机连过来) client=有线转发(我们连车机)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--listen-host", default="127.0.0.1")
    ap.add_argument("--accept-timeout", type=float, default=30.0)
    ap.add_argument("--video", required=True, help="Annex-B H.264 裸流文件")
    ap.add_argument("--fps", type=int, default=30)
    ap.add_argument("--seconds", type=float, default=8.0)
    ap.add_argument("--expect-size", default="1920x1080", help="期望车机协商的分辨率")
    ap.add_argument("--auth-secret", default="zero2w-shared-secret")
    ap.add_argument("--brand", default="Xiaomi")
    ap.add_argument("--model", default="2304FPN6DC")
    ap.add_argument("--sdk", default="34")
    ap.add_argument("--carlife-version", default="8.4.2")
    ap.add_argument("--audio", type=int, default=1, help="1=附带 Media 通道 PCM")
    ap.add_argument("--no-update-channel", action="store_true")
    # A 档实现验证：会话建立后一次性发送的刺激
    ap.add_argument("--send-progress", type=int, default=None,
                    help="A2：发 MEDIA_PROGRESS_BAR{progressBar=N}，验证进度转发")
    ap.add_argument("--send-tts", action="store_true",
                    help="A1：发 NAV_TTS_INIT/DATA/END，验证导航压低媒体音")
    ap.add_argument("--send-update", action="store_true",
                    help="A3：发 UPDATE 通道 START/SEND/END，验证通道消费")
    ap.add_argument("--bt-connect", default="", help="以手机端身份连车机的蓝牙链路(AF_UNIX 桥接)并跑引导")
    ap.add_argument("--report-ip", default="127.0.0.1", help="引导阶段上报给车机的手机 IP")
    # ── 本轮「全量接口」的验证开关 ──
    # --send-all 一条命令把所有项都发一遍；也可单独开某一项定位问题。
    ap.add_argument("--send-all", action="store_true",
                    help="发遍本轮所有新接口（元数据/导航/车况/电话/触控/加密/激活…）")
    ap.add_argument("--send-media-info", action="store_true", help="B：MEDIA_INFO（含封面 bytes）")
    ap.add_argument("--send-navi", action="store_true", help="C：NAV_NEXT_TURN_INFO + 辅助引导")
    ap.add_argument("--send-cardata", action="store_true", help="D：车辆数据订阅（收车机上报的车况）")
    ap.add_argument("--send-gear", action="store_true", help="D：手机上报名义档位（GEAR_INFO）")
    ap.add_argument("--send-telephony", action="store_true", help="F：TEL_STATE_* + HFP 状态覆盖")
    ap.add_argument("--send-hfp", action="store_true", help="F：BT_HFP_REQUEST（拨号/DTMF/静音）")
    ap.add_argument("--send-multitouch", action="store_true", help="H：TOUCH_PAD_* + 接收 ACTION_3")
    ap.add_argument("--send-voice", action="store_true", help="O：HU_VOICE_CONTROL + MIC_RECORD_*")
    ap.add_argument("--send-vehicle-control", action="store_true", help="O：VEHICLE_CONTROL（车控）")
    ap.add_argument("--send-activation", action="store_true", help="R：BOX_ACTIVE（激活）")
    ap.add_argument("--send-encryption", action="store_true",
                    help="R：RSA 公钥→AES 密钥→ENCRYPT_READY 全套，之后载荷走 AES")
    ap.add_argument("--send-filetransfer", action="store_true", help="Q：文件传输 SEND_* 系列")
    ap.add_argument("--send-carlife-data", action="store_true", help="D：CARLIFE_DATA_REQ（订阅表）")
    ap.add_argument("--send-frame-rate", type=int, default=None,
                    help="T：请求帧率（车机会回 FRAME_RATE_CHANGE，我们回 DONE）")
    ap.add_argument("--send-timesync", action="store_true", help="S：TIME_SYNC")
    ap.add_argument("--expect-keys", default="",
                    help="K4 自检：按【接收顺序】期望的硬键 CarLife keycode（十进制，逗号分隔）。"
                         "例：--expect-keys 23,24,14  （up/down/back，来自 ServiceTypes.kt:380/381/371）。"
                         "与实际不符则打印 FAIL 并以非 0 退出。")
    args = ap.parse_args()

    data = open(args.video, "rb").read()
    aus = split_access_units(data)
    if not aus:
        log("H.264 文件里没有可用的帧")
        return 2
    log("加载 %s: %d 字节, %d 个 access unit" % (os.path.basename(args.video), len(data), len(aus)))

    include_update = not args.no_update_channel
    if args.bt_connect:
        if not bt_bringup_as_phone(args.bt_connect, args.report_ip):
            return 6
        if args.role != "server":
            log("蓝牙引导后车机会主动连过来，自动切到 --role server")
            args.role = "server"
    if args.role == "server":
        conns = serve_md_ports(args.listen_host, args.accept_timeout, include_update)
    else:
        conns = connect_hu_ports(args.host, args.accept_timeout, include_update)
    if CMD not in conns:
        log("CMD 通道没建立，退出")
        return 2

    inbox = []
    inbox_lock = threading.Lock()
    stop = threading.Event()

    def reader(ch):
        c = conns.get(ch)
        if not c:
            return
        while not stop.is_set():
            item = c.read_frame()
            if item is None:
                with inbox_lock:
                    inbox.append((ch, None, 0, b""))
                return
            stype, ts, payload = item
            with inbox_lock:
                inbox.append((ch, stype, ts, payload))

    threads = [threading.Thread(target=reader, args=(ch,), daemon=True) for ch in conns]
    for t in threads:
        t.start()

    def pop(timeout=1.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            with inbox_lock:
                if inbox:
                    return inbox.pop(0)
            time.sleep(0.005)
        return None

    stats = {"heartbeats": 0, "touch": 0, "touch3": 0, "keys": 0, "active": 0,
             "key_fail": 0, "sent_frames": 0, "sent_audio": 0}
    # K4：把 --expect-keys 解析成按顺序的期望值列表
    KEY_EXPECT["list"] = [int(x) for x in args.expect_keys.split(",") if x.strip()]

    pending = {}

    def next_event(timeout=0.2):
        """取一条消息：CMD 的按 serviceType 缓存进 pending（不丢消息），其它通道交给 sideband。"""
        item = pop(timeout)
        if item is None:
            return False
        ch, stype, ts, payload = item
        if ch is None or stype is None:
            pending["_closed"] = True
            return True
        if ch == CMD:
            pending.setdefault(stype, []).append(payload)
        else:
            handle_sideband(ch, stype, payload)
        return True

    def take(stype):
        if pending.get(stype):
            return pending[stype].pop(0)
        return None

    def wait_cmd(stype, timeout=8.0, what=None):
        deadline = time.time() + timeout
        while True:
            got = take(stype)
            if got is not None:
                return got
            if pending.get("_closed"):
                raise SystemExit("connection closed while waiting for CMD %s" % (what or hex(stype)))
            if time.time() > deadline:
                raise SystemExit("timeout waiting for CMD %s" % (what or hex(stype)))
            next_event(0.1)

    def handle_sideband(ch, stype, payload):
        if ch == VIDEO and stype == VIDEO_HEARTBEAT:
            stats["heartbeats"] += 1
            if stats["heartbeats"] % 20 == 1:
                log("收到车机视频通道心跳 x%d" % stats["heartbeats"])
            return
        # ── 车机下行：本轮新增的全部消息（每一项都要能在日志里看到）──
        if stype == TOUCH_ACTION_3:
            action, pts = parse_touch_action_3(payload)
            stats["touch3"] += 1
            log("[H] 车机下发多点触控 action=%s 点数=%d 触点=%s" % (action, len(pts), pts))
            return
        if stype == TOUCH_CAR_HARD_KEY_CODE:
            code = first(payload, 1, 0)
            stats["keys"] += 1
            # K4 自检：不是“回显收到什么”，而是拿【参考表里的期望值】对。
            # 期望值由调用方从 ServiceTypes.kt:358-408 抄下来传进 --expect-keys，
            # 所以这是一次真正独立于我们 C++ 实现的核对。
            exp = KEY_EXPECT["list"]
            idx = stats["keys"] - 1
            if idx < len(exp):
                want = exp[idx]
                if code == want:
                    log("[K4] PASS 硬键 #%d keycode=%s == 参考表期望 %s" % (idx + 1, code, want))
                else:
                    stats["key_fail"] += 1
                    log("[K4] FAIL 硬键 #%d keycode=%s != 参考表期望 %s （参考表见 ServiceTypes.kt:358-408）"
                        % (idx + 1, code, want))
            else:
                log("[K4] 硬键 keycode=%s (%s)（未给期望值，未校验）"
                    % (code, _REV_KEYCODE.get(code, "?")))
            return
        if stype == CAR_VELOCITY:
            log("[D] 车机上报车速 speed=%s" % first(payload, 1, 0))
            return
        if stype == CAR_GEAR:
            log("[D] 车机上报档位 gear=%s" % first(payload, 1, 0))
            return
        if stype == CAR_OIL:
            log("[D] 车机上报油量 level=%s range=%s" % (first(payload, 1, 0), first(payload, 2, 0)))
            return
        if stype == CAR_GPS:
            log("[D] 车机上报 GPS lat=%s lon=%s speed=%s heading=%s sats=%s"
                % (first(payload, 3, 0), first(payload, 4, 0), first(payload, 6, 0),
                   first(payload, 7, 0), first(payload, 18, 0)))
            return
        if stype == CAR_GYROSCOPE:
            log("[D] 车机上报陀螺仪 type=%s" % first(payload, 1, 0))
            return
        if stype == CAR_ACCELERATION:
            log("[D] 车机上报加速度")
            return
        if stype == HU_ACTIVE:
            statue = first(payload, 1, b"")
            if isinstance(statue, bytes):
                statue = statue.decode("utf-8", "replace")
            stats["active"] += 1
            log("[R] 车机应答激活 statue=%s" % statue)
            return
        if stype == BT_HFP_RESPONSE:
            log("[F] 车机 HFP 应答 status=%s cmd=%s" % (first(payload, 1, 0), first(payload, 2, 0)))
            return
        if stype == BT_HFP_STATUS_RESPONSE:
            log("[F] 车机 HFP 状态应答 status=%s type=%s" % (first(payload, 1, 0), first(payload, 2, 0)))
            return
        if stype in (CARLIFE_DATA_SUBSCRIBE, CARLIFE_DATA_SUBSCRIBE_DONE_RSP):
            log("[D] 车机回应数据订阅 0x%08X" % stype)
            return
        if stype == VEHICLE_CONTROL_INFO:
            log("[N] 车机上报车控能力表")
            return
        if stype == VIDEO_ENCODER_FRAME_RATE_CHANGE:
            conns[CMD].send(VIDEO_ENCODER_FRAME_RATE_CHANGE_DONE, f_int(1, first(payload, 1, 30)))
            log("[T] 车机请求改帧率 frameRate=%s -> 已回 DONE" % first(payload, 1, 30))
            return
        if ch == TOUCH:
            stats["touch"] += 1
            x = first(payload, 1)
            y = first(payload, 2)
            # 措辞保持“车机下发触控”，run_wsl_tests.sh 的既有断言依赖它
            log("车机下发触控 type=0x%08X (%s,%s)" % (stype, x, y))
            return

    # 1. 车机协议版本
    payload = wait_cmd(HU_PROTOCOL_VERSION)
    major = first(payload, 1, 0)
    minor = first(payload, 2, 0)
    log("收到 HU_PROTOCOL_VERSION major=%s minor=%s" % (major, minor))
    conns[CMD].send(PROTOCOL_VERSION_MATCH_STATUS, f_int(1, 1) + f_int(2, 2))

    # 2. 手机设备信息
    conns[CMD].send(MD_INFO, f_str(1, "android") + f_str(4, args.brand) + f_str(14, args.model)
                    + f_str(16, "SIMSERIAL01") + f_str(20, args.sdk) + f_int(21, int(args.sdk))
                    + f_str(19, "14") + f_str(24, args.carlife_version))
    log("已发 MD_INFO brand=%s model=%s sdk=%s carlife=%s"
        % (args.brand, args.model, args.sdk, args.carlife_version))

    # 3. 车机设备信息
    payload = wait_cmd(HU_INFO)
    hu = strs(payload)
    log("收到 HU_INFO os=%s brand=%s model=%s bt=%s carlifeVersion=%s"
        % (hu.get(1), hu.get(4), hu.get(14), hu.get(23), hu.get(24)))

    # 4. 鉴权（车机问，手机答；seed = brand+model+sdk+connectTime）
    payload = wait_cmd(HU_AUTH_REQUEST)
    random_value = first(payload, 1, b"").decode() if isinstance(first(payload, 1), bytes) else ""
    log("收到 HU_AUTH_REQUEST randomValue=\"%s\"" % random_value)
    parts = random_value.split(";")
    connect_time = parts[1] if len(parts) > 1 else ""
    seed = args.brand + args.model + args.sdk + connect_time
    encrypt_value = dev_digest(args.auth_secret + "|" + seed)
    conns[CMD].send(MD_AUTH_RESPONSE, f_str(1, encrypt_value))
    log("已发 MD_AUTH_RESPONSE encryptValue=%s" % encrypt_value)

    payload = wait_cmd(HU_AUTH_RESULT)
    auth_ok = bool(first(payload, 1, 0))
    log("收到 HU_AUTH_RESULT authenResult=%s" % auth_ok)
    if not auth_ok:
        log("车机判定鉴权失败 —— 保持连接 9 秒，验证协议规定的 5 秒主动断开")
        t_end = time.time() + 9.0
        t0 = time.time()
        while time.time() < t_end:
            next_event(0.2)
            if pending.get("_closed"):
                log("车机在 %.1fs 时主动断开（协议规定 5 秒）" % (time.time() - t0))
                return 0
        log("FAIL 车机没有按协议在 5 秒内主动断开")
        return 5

    conns[CMD].send(MD_AUTH_RESULT, f_int(1, 1))
    log("已发 MD_AUTH_RESULT true")
    wait_cmd(MD_AUTH_RESULT_RESPONSE)
    log("会话建立 (双向鉴权完成)")

    # 5/6. 能力协商与视频协商（车机侧两条消息先后顺序不固定，两条都收齐才继续）
    conns[CMD].send(MD_FEATURE_CONFIG_REQUEST,
                    f_int(1, 2) + f_msg(2, f_str(1, "CONTENT_ENCRYPTION") + f_int(2, 0))
                    + f_msg(2, f_str(1, "MULTI_TOUCH") + f_int(2, 1)))
    feats = None
    init_payload = None
    deadline = time.time() + 8.0
    while time.time() < deadline and (feats is None or init_payload is None):
        if feats is None:
            payload = take(HU_FEATURE_CONFIG_RESPONSE)
            if payload is not None:
                feats = {}
                for f, _w, v in pb_fields(payload):
                    if f == 2:
                        k = first(v, 1, b"")
                        feats[k.decode("utf-8", "replace") if isinstance(k, bytes) else "?"] = first(v, 2)
                nm = first(payload, 4, b"")
                mc = first(payload, 5, b"")
                log("收到 HU_FEATURE_CONFIG_RESPONSE cnt=%s btName=%s btMAC=%s 能力=%s"
                    % (first(payload, 1),
                       nm.decode("utf-8", "replace") if isinstance(nm, bytes) else nm,
                       mc.decode("utf-8", "replace") if isinstance(mc, bytes) else mc, feats))
        if init_payload is None:
            init_payload = take(VIDEO_ENCODER_INIT)
        if feats is None or init_payload is None:
            next_event(0.1)
    if init_payload is None:
        log("FAIL 始终没收到 VIDEO_ENCODER_INIT")
        return 4
    w = first(init_payload, 1, 0)
    h = first(init_payload, 2, 0)
    fr = first(init_payload, 3, 0)
    log("收到 VIDEO_ENCODER_INIT %dx%d@%d" % (w, h, fr))
    expect_w, expect_h = args.expect_size.lower().split("x")
    if (int(w), int(h)) != (int(expect_w), int(expect_h)):
        log("FAIL 分辨率与期望 %s 不符" % args.expect_size)
        return 3
    conns[CMD].send(VIDEO_ENCODER_INIT_DONE, b"")
    log("已发 VIDEO_ENCODER_INIT_DONE")
    wait_cmd(VIDEO_ENCODER_START)
    log("车机发来 VIDEO_ENCODER_START，开始推流")

    # ---- R：内容加密协商（AUDIT 3.1 / 5.4）----
    # 严格照拄 FeaturesHandler.kt 的四步：
    #   ① 手机要公钥        → 车机回 HU_RSA_PUBLIC_KEY_RESPONSE(Base64 X.509/SPKI)
    #   ② 手机生成 AES key → RSA/ECB/PKCS1Padding 加密后走 MD_AES_KEY_SEND_REQUEST
    #   ③ 车机回 HU_AES_REC_RESPONSE（空载荷）
    #   ④ 手机发 MD_ENCRYPT_READY → 车机回 MD_ENCRYPT_READY_DONE 并置位
    if args.send_encryption or args.send_all:
        conns[CMD].send(MD_RSA_PUBLIC_KEY_REQUEST, b"")
        log("[R] 已发 MD_RSA_PUBLIC_KEY_REQUEST")
        pub = wait_cmd(HU_RSA_PUBLIC_KEY_RESPONSE, timeout=8.0)  # 超时会带着原因退出
        pub_b64 = first(pub, 1, b"")
        if isinstance(pub_b64, bytes):
            pub_b64 = pub_b64.decode("utf-8", "replace")
        log("[R] 收到 HU_RSA_PUBLIC_KEY_RESPONSE（公钥 %d 字符）" % len(pub_b64))
        from cryptography.hazmat.primitives import serialization
        from cryptography.hazmat.primitives.asymmetric import padding
        pubkey = serialization.load_der_public_key(base64.b64decode(pub_b64))
        # 照拄 AesEncryptor.withRandomKey：UUID 前 16 字符（可打印 ASCII）。
        import uuid
        aes_key = uuid.uuid4().hex[:16].encode()
        sealed = pubkey.encrypt(aes_key, padding.PKCS1v15())
        conns[CMD].send(MD_AES_KEY_SEND_REQUEST, f_str(1, base64.b64encode(sealed)))
        log("[R] 已发 MD_AES_KEY_SEND_REQUEST（RSA 密文 %d B）" % len(sealed))
        wait_cmd(HU_AES_REC_RESPONSE, timeout=8.0)
        log("[R] 收到 HU_AES_REC_RESPONSE（空载荷）")
        Conn.CRYPTO["key"] = aes_key
        conns[CMD].send(MD_ENCRYPT_READY, b"")
        log("[R] 已发 MD_ENCRYPT_READY（AES-128 密钥就绪）")
        # 这一步之后，所有载荷都走 AES-ECB/PKCS5（Conn._encrypt/_decrypt）
        Conn.CRYPTO["ready"] = True
        got = wait_cmd(MD_ENCRYPT_READY_DONE, timeout=8.0)
        log("[R] 内容加密已启用；收到 MD_ENCRYPT_READY_DONE=%s" % (got is not None))

    # 7. 推流线程
    def streamer():
        period = 1.0 / max(1, args.fps)
        audio = pcm_sine(100)
        i = 0
        next_t = time.time()
        while not stop.is_set():
            au = aus[i % len(aus)]
            if conns[VIDEO].send(VIDEO_DATA, au):
                stats["sent_frames"] += 1
            else:
                return
            if args.audio and MEDIA in conns:
                if i == 5:
                    conns[MEDIA].send(MEDIA_INIT, f_int(1, 44100) + f_int(2, 12) + f_int(3, 2))
                    log("已发 MEDIA_INIT 44100Hz stereo PCM16")
                if i % 3 == 0:
                    conns[MEDIA].send(MEDIA_DATA, audio[:3528])
                    stats["sent_audio"] += 1
            i += 1
            next_t += period
            delay = next_t - time.time()
            if delay > 0:
                time.sleep(delay)
            elif delay < -0.5:
                next_t = time.time()

    th = threading.Thread(target=streamer, daemon=True)
    th.start()

    # ---- A..U 档验证刺激（一次性；每项只在传了对应开关时发送）----
    def send_a_batch():
        time.sleep(0.4)
        all_on = args.send_all
        if args.send_progress is not None or all_on:
            conns[CMD].send(MEDIA_PROGRESS_BAR, f_int(1, int(args.send_progress or 42)))
            log("[A2] 已发 MEDIA_PROGRESS_BAR progress=%s" % (args.send_progress or 42))
        if args.send_update or all_on:
            if UPDATE in conns:
                conns[UPDATE].send(DATA_MD_TRANSFER_START, b"")
                conns[UPDATE].send(DATA_MD_TRANSFER_SEND, bytes(range(32)))
                conns[UPDATE].send(DATA_MD_TRANSFER_END, b"")
                log("[A3] 已发 UPDATE 通道 START/SEND(32B)/END")
        if args.send_tts or all_on:
            if TTS in conns:
                # CarlifeTTSInit{sampleRate=44100, channelConfig=12(STEREO), sampleFormat=2(PCM16)}
                conns[TTS].send(NAV_TTS_INIT, f_int(1, 44100) + f_int(2, 12) + f_int(3, 2))
                log("[A1] 已发 NAV_TTS_INIT 44100Hz stereo PCM16")
                tone = pcm_sine(100)
                for _k in range(3):
                    conns[TTS].send(NAV_TTS_DATA, tone[:3528])
                    time.sleep(0.1)
                log("[A1] 已发 3xNAV_TTS_DATA（此刻媒体音应被压低）")
                time.sleep(0.6)
                conns[TTS].send(NAV_TTS_END, b"")
                log("[A1] 已发 NAV_TTS_END（媒体音应恢复）")

        # A：媒体元数据 + 封面（AUDIT 3.7 / 5.1 #6）
        if args.send_media_info or all_on:
            # 一张最小的合法 JPEG（1x1，约 300 B）：只为证明“原字节搬运”，不做解码
            cover = bytes.fromhex(
                "ffd8ffe000104a46494600010100000100010000ffdb004300" + "08" * 64
                + "ffc0000b080001000101011100ffc40014000100000000000000000000000000000009"
                + "ffda0008010100003f00d2cf20ffd9")
            mi = (f_str(1, "com.xiaomi.music") + f_str(2, "CarLife 测试曲目")
                  + f_str(3, "测试歌手") + f_str(4, "测试专辑")
                  + f_bytes(5, cover) + f_int(6, 215000) + f_int(7, 3)
                  + f_str(8, "track-0001") + f_int(9, 1))
            conns[CMD].send(MEDIA_INFO, mi)
            log("[A] 已发 MEDIA_INFO song=测试曲目 封面=%dB" % len(cover))

        # C：导航逐向（action/nextTurn/roadName/total/remain）
        if args.send_navi or all_on:
            # 带 turnIconData（field 6 = bytes）—— 注意官方两个版本冲突：
            # Kotlin SDK 是 bytes turnIconData，官方 C++ 库是 int32 time。
            # 这里两个都发：先发 bytes 版，再发一个 varint 版，验证 wire 层双兼容。
            icon = b"turn-left"
            nu = (f_int(1, 3) + f_int(2, 12) + f_str(3, "中关村大街")
                  + f_int(4, 12500) + f_int(5, 800) + f_bytes(6, icon))
            conns[CMD].send(NAV_NEXT_TURN_INFO, nu)
            log("[C] 已发 NAV_NEXT_TURN_INFO action=3 nextTurn=12 road=中关村大街 remain=800m "
                "icon=%dB(Kotlin 版 field6=bytes)" % len(icon))
            # 同一个消息的 C++ 库版：field6 是 int32 time=90
            nu2 = (f_int(1, 7) + f_int(2, 13) + f_str(3, "知春路")
                   + f_int(4, 9000) + f_int(5, 300) + f_int(6, 90))
            conns[CMD].send(NAV_NEXT_TURN_INFO, nu2)
            log("[C] 已发 NAV_NEXT_TURN_INFO action=7 time=90s(C++ 版本 field6=int32)")
            ag = f_int(1, 1) + f_int(2, 2) + f_int(3, 5) + f_int(4, 12500) + f_int(5, 800) + f_int(6, 60)
            conns[CMD].send(NAV_ASSISTANT_GUIDE, ag)
            conns[CMD].send(NAV_ASSISTANT_GUIDE_HU, ag)
            log("[C] 已发 NAV_ASSISTANT_GUIDE（两个 ID 都发：0x00010047/0x00018047）")

        # D：车况订阅（让车机开始上报速度/GPS/档位/油量/陀螺仪/加速度）
        if args.send_cardata or all_on:
            conns[CMD].send(CAR_DATA_SUBSCRIBE, b"")
            log("[D] 已发 CAR_DATA_SUBSCRIBE（期望车机回 CAR_VELOCITY/CAR_GPS/…）")
            conns[CMD].send(CARLIFE_DATA_REQ, b"")
            log("[D] 已发 CARLIFE_DATA_REQ（期望回 CARLIFE_DATA_SUBSCRIBE）")
        if args.send_gear or all_on:
            conns[CMD].send(GEAR_INFO, f_int(1, 4))
            log("[D] 已发 GEAR_INFO gear=4(D)")

        # F：电话状态 + HFP
        if args.send_telephony or all_on:
            conns[CMD].send(TEL_STATE_INCOMING, b"")
            log("[F] 已发 TEL_STATE_INCOMING")
            conns[CMD].send(BT_HFP_CALL_STATUS_COVER,
                            f_int(1, 1) + f_str(2, "13800138000") + f_str(3, "张三"))
            log("[F] 已发 BT_HFP_CALL_STATUS_COVER state=1 num=13800138000 name=张三")
            time.sleep(0.2)
            conns[CMD].send(TEL_STATE_INCALLING, b"")
            log("[F] 已发 TEL_STATE_INCALLING")
        if args.send_hfp or all_on:
            conns[CMD].send(BT_HFP_REQUEST, f_int(1, 1) + f_str(2, "13800138000"))
            log("[F] 已发 BT_HFP_REQUEST START_CALL num=13800138000")
            time.sleep(0.2)
            conns[CMD].send(BT_HFP_REQUEST, f_int(1, 5) + f_int(3, 53))
            log("[F] 已发 BT_HFP_REQUEST DTMF code=53('5')")
            time.sleep(0.2)
            conns[CMD].send(BT_HFP_REQUEST, f_int(1, 6))
            log("[F] 已发 BT_HFP_REQUEST MUTE_MIC")
            time.sleep(0.2)
            conns[CMD].send(BT_HFP_STATUS_REQUEST, f_int(1, 1))
            log("[F] 已发 BT_HFP_STATUS_REQUEST type=1(MIC_STATUS)")

        # H/J：触摸板 + 手势 + 多点触控
        if args.send_multitouch or all_on:
            conns[CMD].send(TOUCH_PAD_DOWN, f_int(1, 1000))
            log("[I] 已发 TOUCH_PAD_DOWN")
            time.sleep(0.1)
            conns[CMD].send(TOUCH_PAD_MOVE, f_int(1, 1010) + f_int(2, 12) + f_int(3, -7))
            log("[I] 已发 TOUCH_PAD_MOVE delta=(12,-7)")
            time.sleep(0.1)
            conns[CMD].send(TOUCH_PAD_PINCH, f_float(1, 1.25))
            log("[J] 已发 TOUCH_PAD_PINCH scale=1.25")
            time.sleep(0.1)
            conns[CMD].send(TOUCH_PAD_UP, f_int(1, 1100))
            log("[I] 已发 TOUCH_PAD_UP")
            # 反向：用 ACTION_3 给车机发一个双指事件
            act3 = struct.pack(">i", 2) + bytes([0]) + struct.pack(">hh", 100, 200) \
                + bytes([1]) + struct.pack(">hh", 300, 400)
            conns[TOUCH].send(TOUCH_ACTION_3, act3)
            log("[H] 已向车机发 TOUCH_ACTION_3 action=2 双指 (100,200)/(300,400)")

        # O：语音控制 + 麦克风录音控制 + 车控
        if args.send_voice or all_on:
            conns[CMD].send(HU_VOICE_CONTROL, f_int(1, 1) + f_int(2, 0))
            log("[O] 已发 HU_VOICE_CONTROL command=1")
            for st, nm in ((MIC_RECORD_WAKEUP_START, "WAKEUP_START"),
                           (MIC_RECORD_RECOG_START, "RECOG_START"),
                           (MIC_RECORD_END, "END")):
                conns[CMD].send(st, b"")
                log("[M] 已发 MIC_RECORD_%s" % nm)
                time.sleep(0.1)
        if args.send_vehicle_control or all_on:
            vc = (f_int(1, 1) + f_int(2, 1001) + f_bool(3, True) + f_int(5, 2)
                  + f_int(6, 1) + f_int(6, 2) + f_int(7, 1) + f_sint32(9, 24))
            conns[CMD].send(VEHICLE_CONTROL, vc)
            log("[N] 已发 VEHICLE_CONTROL type=1 id=1001 area=2 int32=[24]")

        # R：激活
        if args.send_activation or all_on:
            conns[CMD].send(BOX_ACTIVE, f_str(1, "38:F7:FF:38:57:90") + f_int(2, 0) + f_int(3, 12345))
            log("[R] 已发 BOX_ACTIVE")

        # Q：文件传输（G 类的五条）
        if args.send_filetransfer or all_on:
            if UPDATE in conns:
                conns[UPDATE].send(SEND_START, f_int(1, 4096) + f_int(2, 1))
                for _k in range(4):
                    conns[UPDATE].send(SENDING_DATA, bytes(256))
                conns[UPDATE].send(SEND_FINISH, b"")
                log("[Q] 已发文件传输 SEND_START/SENDING_DATA x4/SEND_FINISH")

        # S：时间同步
        if args.send_timesync or all_on:
            conns[CMD].send(TIME_SYNC, f_int(1, int(time.time())))
            log("[S] 已发 TIME_SYNC")

    threading.Thread(target=send_a_batch, daemon=True).start()

    # 8. 主循环：处理车机下行消息，直到时间到
    deadline = time.time() + args.seconds
    got_first_frame_ack = False
    while time.time() < deadline:
        item = pop(0.2)
        if item is None:
            if conns[CMD].sock.fileno() < 0:
                break
            continue
        ch, stype, ts, payload = item
        if ch is None or stype is None:
            log("车机关闭了连接")
            break
        handle_sideband(ch, stype, payload)
        if not got_first_frame_ack and stats["sent_frames"] > 3:
            got_first_frame_ack = True
            log("已推送 %d 帧" % stats["sent_frames"])

    stop.set()
    time.sleep(0.2)
    log("汇总: 发送视频帧=%d 音频包=%d 收到心跳=%d 触控=%d 触控3=%d 硬键=%d 激活应答=%d"
        % (stats["sent_frames"], stats["sent_audio"], stats["heartbeats"], stats["touch"],
           stats["touch3"], stats["keys"], stats["active"]))
    # K4：硬键自检必须全部通过，否则本进程以非 0 退出（让测试脚本能抓到）
    if KEY_EXPECT["list"]:
        exp_n = len(KEY_EXPECT["list"])
        if stats["keys"] < exp_n:
            log("FAIL 硬键自检：期望 %d 个，实际只收到 %d 个" % (exp_n, stats["keys"]))
            stats["key_fail"] += 1
        elif stats["key_fail"] == 0:
            log("PASS 硬键自检：%d 个 keycode 全部等于参考表期望值" % exp_n)
        if stats["key_fail"]:
            log("FAIL 硬键自检：%d 项不符（参考表 ServiceTypes.kt:358-408）" % stats["key_fail"])
            return 7
    for c in conns.values():
        try:
            c.sock.close()
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())