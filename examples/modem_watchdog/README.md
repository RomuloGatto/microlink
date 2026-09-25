# Modem Watchdog + MicroLink subnet router

Target: classic ESP32/WROOM, no PSRAM.

GPIO27 is active-low:
- HIGH: relay released, NC closed, modem powered
- LOW: relay energized, NC open, modem power cut

The ESP must be powered from the 12V input before the relay.

Configure credentials and the LAN prefix with:

```bash
idf.py set-target esp32
idf.py menuconfig
```

Under **MicroLink V2 Configuration > Credentials**, set WiFi SSID/password,
Tailscale auth key, and device name.

Under **MicroLink V2 Configuration > Subnet Router**, set the LAN prefix to
advertise, for example `192.168.100.0/24`.

For the Novoagatto Headscale deployment, also open **Control Plane** and use:

- hostname: `headscale.novoagatto.com`
- TLS/443: enabled
- Noise public key: the 64 hex characters from
  `https://headscale.novoagatto.com/key?v=88`

On macOS the value can be extracted with:

```bash
curl -fsSL 'https://headscale.novoagatto.com/key?v=88' \
  | python3 -c 'import json,sys; print((json.load(sys.stdin).get("publicKey") or json.load(sys.stdin).get("legacyPublicKey")).removeprefix("mkey:"))'
```

If the one-liner fails because the endpoint response shape differs, print the
raw JSON and copy the value after `mkey:`.

Then:

```bash
idf.py build
idf.py -p /dev/cu.usbserial-... flash monitor
```

Once the node appears in Tailscale, approve the advertised route. The watchdog
UI listens on TCP/80 and can be reached over the node's Tailscale IP.

Before permanent deployment, add an external ~10k pull-up from relay IN to
3V3 so the active-low relay remains released during reset/boot ROM time.
