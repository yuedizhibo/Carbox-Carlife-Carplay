#!/usr/bin/env python3
"""
Apple Authentication 3.0 Coprocessor helper for Linux/i2c-tools.

Default wiring used here:
    /dev/i2c-1
    7-bit I2C address: 0x10

The chip may NACK the first transaction while waking from sleep, so all
register accesses retry automatically.

Examples:
    python3 auth3.py info
    python3 auth3.py selftest
    python3 auth3.py auth --random
    python3 auth3.py auth --challenge 000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f
    python3 auth3.py auth --random --json

Use as a Python module:
    from auth3 import Auth3
    dev = Auth3(bus=1)
    response = dev.authenticate(bytes.fromhex("00" * 32))
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from dataclasses import dataclass


class Auth3Error(RuntimeError):
    pass


@dataclass
class Auth3Info:
    device_version: int
    authentication_revision: int
    protocol_major: int
    protocol_minor: int
    device_id: bytes
    self_test: int

    def as_dict(self):
        return {
            "device_version": f"0x{self.device_version:02x}",
            "authentication_revision": f"0x{self.authentication_revision:02x}",
            "protocol_major": f"0x{self.protocol_major:02x}",
            "protocol_minor": f"0x{self.protocol_minor:02x}",
            "device_id": self.device_id.hex(),
            "self_test": f"0x{self.self_test:02x}",
            "certificate_present": bool(self.self_test & 0x80),
            "private_key_present": bool(self.self_test & 0x40),
        }


class Auth3:
    # Register map used by the authentication flow.
    REG_DEVICE_VERSION = 0x00
    REG_AUTH_REVISION = 0x01
    REG_PROTOCOL_MAJOR = 0x02
    REG_PROTOCOL_MINOR = 0x03
    REG_DEVICE_ID = 0x04
    REG_ERROR_CODE = 0x05

    REG_AUTH_CONTROL_STATUS = 0x10
    REG_RESPONSE_LENGTH = 0x11
    REG_RESPONSE_DATA = 0x12

    REG_CHALLENGE_LENGTH = 0x20
    REG_CHALLENGE_DATA = 0x21

    REG_CERT_LENGTH = 0x30
    REG_SELF_TEST = 0x40
    REG_CERT_SERIAL = 0x4E
    REG_SLEEP = 0x60

    def __init__(
        self,
        bus: int = 1,
        address: int = 0x10,
        retries: int = 5,
        retry_delay: float = 0.002,
    ):
        self.bus = int(bus)
        self.address = int(address)
        self.retries = int(retries)
        self.retry_delay = float(retry_delay)

        dev = f"/dev/i2c-{self.bus}"
        if not os.path.exists(dev):
            raise Auth3Error(f"{dev} 不存在")
        if not self._command_exists("i2ctransfer"):
            raise Auth3Error("找不到 i2ctransfer，请先安装 i2c-tools")

    @staticmethod
    def _command_exists(name: str) -> bool:
        from shutil import which
        return which(name) is not None

    def _run_transfer(self, *parts: str) -> str:
        cmd = ["i2ctransfer", "-y", str(self.bus), *parts]
        p = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if p.returncode != 0:
            msg = p.stderr.strip() or p.stdout.strip() or "I2C transfer failed"
            raise Auth3Error(msg)
        return p.stdout.strip()

    @staticmethod
    def _parse_i2ctransfer_bytes(text: str) -> bytes:
        text = text.strip()
        if not text:
            return b""
        out = bytearray()
        for token in text.split():
            if token.lower().startswith("0x"):
                token = token[2:]
            out.append(int(token, 16))
        return bytes(out)

    def _select_register(self, reg: int) -> None:
        self._run_transfer(f"w1@0x{self.address:02x}", f"0x{reg:02x}")

    def read_register(self, reg: int, length: int, delay: float = 0.002) -> bytes:
        """
        Select register with a write transaction/STOP, then perform a read
        transaction, matching the device transaction format.
        """
        last_error = None
        for _ in range(self.retries):
            try:
                self._select_register(reg)
                time.sleep(delay)
                out = self._run_transfer(f"r{length}@0x{self.address:02x}")
                data = self._parse_i2ctransfer_bytes(out)
                if len(data) != length:
                    raise Auth3Error(
                        f"寄存器 0x{reg:02x} 期望 {length} bytes，实际 {len(data)} bytes"
                    )
                return data
            except Auth3Error as e:
                last_error = e
                # A first NACK may just be waking the chip.
                time.sleep(self.retry_delay)
        raise Auth3Error(f"读取寄存器 0x{reg:02x} 失败: {last_error}")

    def write_register(self, reg: int, data: bytes = b"") -> None:
        last_error = None
        payload = [f"0x{reg:02x}"] + [f"0x{x:02x}" for x in data]
        desc = f"w{1 + len(data)}@0x{self.address:02x}"

        for _ in range(self.retries):
            try:
                self._run_transfer(desc, *payload)
                return
            except Auth3Error as e:
                last_error = e
                # Spec allows retry after a NACK; 2 ms also covers wake init.
                time.sleep(self.retry_delay)
        raise Auth3Error(f"写寄存器 0x{reg:02x} 失败: {last_error}")

    def wake(self) -> None:
        """
        Cause I2C activity and then verify the device is awake.
        """
        try:
            self._select_register(self.REG_DEVICE_VERSION)
        except Auth3Error:
            pass
        time.sleep(self.retry_delay)

        version = self.read_register(self.REG_DEVICE_VERSION, 1)[0]
        if version != 0x07:
            raise Auth3Error(f"唤醒后 Device Version 异常: 0x{version:02x}")

    def get_info(self) -> Auth3Info:
        return Auth3Info(
            device_version=self.read_register(self.REG_DEVICE_VERSION, 1)[0],
            authentication_revision=self.read_register(self.REG_AUTH_REVISION, 1)[0],
            protocol_major=self.read_register(self.REG_PROTOCOL_MAJOR, 1)[0],
            protocol_minor=self.read_register(self.REG_PROTOCOL_MINOR, 1)[0],
            device_id=self.read_register(self.REG_DEVICE_ID, 4),
            self_test=self.read_register(self.REG_SELF_TEST, 1)[0],
        )

    def get_certificate_serial_raw(self) -> bytes:
        return self.read_register(self.REG_CERT_SERIAL, 32)

    def get_certificate_serial(self) -> str:
        raw = self.get_certificate_serial_raw()
        try:
            return raw.decode("ascii")
        except UnicodeDecodeError:
            return raw.hex()

    def get_certificate_length(self) -> int:
        # Selecting any block-3 register requires waiting at least tCERT
        # before the read. The documented maximum tCERT is 10 ms.
        raw = self.read_register(self.REG_CERT_LENGTH, 2, delay=0.012)
        # On this tested device, uint16 fields are returned MSB first
        # (e.g. 00 20 -> 32, 00 40 -> 64).
        return int.from_bytes(raw, "big")

    def get_error_code(self) -> int:
        # Reading this register clears it.
        return self.read_register(self.REG_ERROR_CODE, 1)[0]

    def authenticate(
        self,
        challenge: bytes,
        timeout: float = 0.8,
        poll_interval: float = 0.02,
        verify_challenge: bool = True,
    ) -> bytes:
        """
        Run one 32-byte challenge-response operation and return 64-byte response.
        """
        if len(challenge) != 32:
            raise ValueError("challenge 必须正好是 32 bytes")

        self.wake()

        # 1) Write challenge.
        self.write_register(self.REG_CHALLENGE_DATA, challenge)

        # 2) Confirm chip accepted a 32-byte challenge.
        challenge_len = int.from_bytes(
            self.read_register(self.REG_CHALLENGE_LENGTH, 2),
            "big",
        )
        if challenge_len != 32:
            raise Auth3Error(f"Challenge Data Length 异常: {challenge_len}")

        # Optional integrity check over the I2C path.
        if verify_challenge:
            readback = self.read_register(self.REG_CHALLENGE_DATA, 32)
            if readback != challenge:
                raise Auth3Error("Challenge 回读不一致")

        # 3) Start challenge-response computation.
        self.write_register(self.REG_AUTH_CONTROL_STATUS, b"\x01")

        # 4) Poll status until success/error/timeout.
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            time.sleep(poll_interval)
            status = self.read_register(self.REG_AUTH_CONTROL_STATUS, 1)[0]
            error_set = (status >> 7) & 0x01
            procedure_result = (status >> 4) & 0x07

            if error_set:
                code = self.get_error_code()
                descriptions = {
                    0x00: "No error",
                    0x02: "Invalid register specified or register is read-only",
                    0x05: "Sequence error / command out of sequence",
                    0x06: "Internal process error during challenge response generation",
                }
                desc = descriptions.get(code, "Reserved/unknown error")
                raise Auth3Error(f"认证失败: Error Code 0x{code:02x} ({desc})")

            if procedure_result == 1:
                break
        else:
            raise Auth3Error("认证计算超时")

        # 5) Verify and read 64-byte response.
        response_len = int.from_bytes(
            self.read_register(self.REG_RESPONSE_LENGTH, 2),
            "big",
        )
        if response_len != 64:
            raise Auth3Error(f"Challenge Response Length 异常: {response_len}")

        response = self.read_register(self.REG_RESPONSE_DATA, 64)
        if response == b"\x00" * 64:
            raise Auth3Error("Challenge Response 全为 0x00")
        return response

    def sleep(self) -> None:
        self.write_register(self.REG_SLEEP, b"\xEE")


def make_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Apple Authentication 3.0 Coprocessor command-line helper"
    )
    p.add_argument("--bus", type=int, default=1, help="I2C bus number, default: 1")
    p.add_argument(
        "--addr",
        type=lambda x: int(x, 0),
        default=0x10,
        help="7-bit I2C address, default: 0x10",
    )
    p.add_argument("--json", action="store_true", help="JSON output")

    sub = p.add_subparsers(dest="command", required=True)

    sub.add_parser("info", help="Read chip identity and self-test info")
    sub.add_parser("selftest", help="Check certificate/private-key self-test bits")
    sub.add_parser("serial", help="Read certificate serial number")
    sub.add_parser("certlen", help="Read accessory certificate length")
    sub.add_parser("sleep", help="Force chip into sleep mode")

    a = sub.add_parser("auth", help="Run 32-byte challenge-response authentication")
    g = a.add_mutually_exclusive_group(required=True)
    g.add_argument("--challenge", help="32-byte challenge as 64 hex characters")
    g.add_argument("--random", action="store_true", help="Generate random 32-byte challenge")
    a.add_argument(
        "--no-verify-challenge",
        action="store_true",
        help="Skip 32-byte challenge readback check",
    )
    return p


def main() -> int:
    args = make_parser().parse_args()

    try:
        dev = Auth3(bus=args.bus, address=args.addr)

        if args.command == "info":
            info = dev.get_info().as_dict()
            info["certificate_serial"] = dev.get_certificate_serial()
            info["certificate_length"] = dev.get_certificate_length()

            if args.json:
                print(json.dumps(info, ensure_ascii=False))
            else:
                for k, v in info.items():
                    print(f"{k}: {v}")
            return 0

        if args.command == "selftest":
            value = dev.read_register(dev.REG_SELF_TEST, 1)[0]
            result = {
                "raw": f"0x{value:02x}",
                "certificate_present": bool(value & 0x80),
                "private_key_present": bool(value & 0x40),
                "ok": (value & 0xC0) == 0xC0,
            }
            if args.json:
                print(json.dumps(result, ensure_ascii=False))
            else:
                print(f"Self-Test: 0x{value:02x}")
                print(f"X.509 Certificate: {'PASS' if value & 0x80 else 'FAIL'}")
                print(f"Private Key: {'PASS' if value & 0x40 else 'FAIL'}")
            return 0 if result["ok"] else 2

        if args.command == "serial":
            serial = dev.get_certificate_serial()
            if args.json:
                print(json.dumps({"certificate_serial": serial}, ensure_ascii=False))
            else:
                print(serial)
            return 0

        if args.command == "certlen":
            n = dev.get_certificate_length()
            if args.json:
                print(json.dumps({"certificate_length": n}))
            else:
                print(n)
            return 0

        if args.command == "sleep":
            dev.sleep()
            if args.json:
                print(json.dumps({"ok": True, "state": "sleep"}))
            else:
                print("OK")
            return 0

        if args.command == "auth":
            if args.random:
                challenge = os.urandom(32)
            else:
                h = args.challenge.strip().replace(" ", "").replace(":", "")
                if len(h) != 64:
                    raise ValueError("--challenge 必须是 64 个十六进制字符（32 bytes）")
                try:
                    challenge = bytes.fromhex(h)
                except ValueError as e:
                    raise ValueError("--challenge 包含无效十六进制字符") from e

            response = dev.authenticate(
                challenge,
                verify_challenge=not args.no_verify_challenge,
            )

            result = {
                "ok": True,
                "challenge": challenge.hex(),
                "response": response.hex(),
                "response_length": len(response),
            }

            if args.json:
                print(json.dumps(result))
            else:
                print(f"challenge={challenge.hex()}")
                print(f"response={response.hex()}")
            return 0

        raise Auth3Error("未知命令")

    except (Auth3Error, ValueError) as e:
        if getattr(args, "json", False):
            print(json.dumps({"ok": False, "error": str(e)}, ensure_ascii=False))
        else:
            print(f"ERROR: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
