# cp-native installation

`install.sh` is plan-only unless passed `--apply`. It never enables the unit,
starts an AP, changes `wlan0`, or touches the AC200.

```sh
./install.sh /path/to/cp-native
./install.sh /path/to/cp-native --apply
```

The script accepts only an ARM64 Linux ELF. It runs `apt-get update` before
installing missing packages. Installation uses the **development** USB profile;
RNDIS must remain reachable at `192.168.77.2`.

After installation, over the USB SSH link only:

1. Set a lawful `CP_COUNTRY` in `/etc/zero2w/cp-native.env`.
2. Review `cp-native ap-plan --env-file /etc/zero2w/cp-native.env`.
3. Run `cp-native preflight` and `cp-native mfi-check`; both must preserve the
   bus-1-only refusal of AC200.
4. Development uses CatPlay `--cp-input-only` and preserves RNDIS. For an
   engine-only development session, use `cp-native run --no-ap`; it does not
   touch USB/UDC/configfs, the Wi-Fi radio, or the mDNS publisher, but still
   supervises the external engine telemetry socket.
5. Do not select `CP_USB_PROFILE=vehicle` without an explicitly confirmed
   alternate management path or offline deployment plan. Vehicle alone uses
   CatPlay `--cp-bridge` and owns the wired UDC role.
6. Do not set `CP_ENGINE=catplay` until the external engine answers the
   required bounded capability handshake, including `input_only: true`.
