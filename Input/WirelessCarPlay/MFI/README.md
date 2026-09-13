# MFi Auth 3.0 native utility

`auth3-native` is a C++20, fixed-buffer replacement for the command-line behavior of
[`auth3.py`](auth3.py).  The Python script is deliberately retained as a behavior reference
and rollback path.  The native tool uses no Python, `i2c-tools` subprocess, or third-party
runtime: it opens `/dev/i2c-N`, selects the slave with `ioctl(I2C_SLAVE)`, then uses `write` /
`read`.

## **Board safety: verify the current adapter mapping**

`--bus 1 --addr 0x10` remains the Python-compatible default, but adapter numbers are not stable
across overlays/images.  Current board evidence after enabling `overlays=pi-i2c1` and rebooting
is: `/dev/i2c-1` is the external PI7/PI8 controller (`i2c@5002400`), while `/dev/i2c-2` is the
internal AC200 (`i2c@5002c00`, sysfs `2-0010/name=ac200`).  Python `--bus 1 --addr 0x10 info`
succeeded (version `0x07`, revision `0x01`, protocol `3.0`, self-test `0xc0`, certificate length
`608`); `0x11` was absent.  This validates the mapping only, not native authentication.  The
AC200 and MFi coprocessor can both use `0x10`; bus/controller identity is therefore mandatory,
not merely an address check.

**Never scan or write the internal AC200 bus/address (currently `bus 2 @ 0x10`).**  The native
tool does not hard-code a bus number: before opening it rejects any target already represented in
`/sys/bus/i2c/devices/<bus>-<addr>`, checking `name` (including `ac200`), kernel `driver`, and
`of_node`.  This has no bypass, including for nominally read-only commands.  Always confirm
`i2cdetect -l` after a reboot and specify the verified external adapter with `--bus`.

## Wiring and board preparation

Use the 40-pin header's I2C1 pins: pin 3 **SDA / PI8**, pin 5 **SCL / PI7**, plus **3.3 V** and
**GND**.  Enable the `sun50i-h616-pi-i2c1` device-tree overlay, reboot, then inspect adapters:

```sh
i2cdetect -l
```

The resulting external adapter number is board-image dependent; use that number rather than
assuming it is 1.  On the currently observed mapping, do not use `i2cdetect` on internal bus 2
or scan/write `2@0x10`.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

The pure protocol tests run under WSL and other hosts.  The `/dev/i2c-*` executable is built
only when CMake targets Linux.  Set `-DBUILD_MFI_AUTH3_NATIVE=OFF` to omit this component.

## CLI correspondence

Supported Python-compatible commands are `info`, `selftest`, `serial`, `certlen`, `sleep`, and
`auth --random` or `auth --challenge HEX`; global options are `--bus`, `--addr`, and `--json`.
`auth --no-verify-challenge` is supported.  `--timeout-ms` (1..60000) is a strict native
addition; the default remains Python's 800 ms.  Bus is strict decimal (0..9999), address is a
strict 7-bit decimal or `0x` hexadecimal value, and a challenge is exactly 32 bytes / 64 hex
characters (spaces and colons are accepted as in Python).

```sh
# Example only after the external adapter is verified and is not represented by sysfs.
./Temp/build/Input/WirelessCarPlay/MFI/auth3-native --bus ACTUAL_EXTERNAL_BUS --addr 0x10 info
./Temp/build/Input/WirelessCarPlay/MFI/auth3-native --bus ACTUAL_EXTERNAL_BUS --addr 0x10 auth --random --json
```

Register reads intentionally remain two transactions: selector `write(reg)` followed by a
**STOP**, delay, then `read`.  They are not converted to repeated-start.  Accesses retry five
times after a NACK/wake delay; wake/version validation, `tCERT` 12 ms wait, big-endian lengths,
challenge readback, status/error polling, response length, and all-zero response rejection match
`auth3.py`.  During tAUTH, a status-register NACK is treated as busy through the overall timeout
(the observed Python flow NACKs after start); it becomes a normal timeout if completion never
arrives.

## Memory/RSS check

The protocol uses stack `std::array` buffers (maximum response 64 bytes) and has no unbounded
payload allocation.  On a host, safely measure only help (it performs no I2C access):

```sh
/usr/bin/time -f 'maxrss_kib=%M' ./Temp/build/Input/WirelessCarPlay/MFI/auth3-native --help
```

Repeat this measurement on the final H618 image.  Do not measure by contacting an internal
adapter.  The executable's RSS includes libc, dynamic loader, and C++ runtime in addition to
its small fixed protocol buffers.
