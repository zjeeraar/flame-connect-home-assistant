# Dimplex Flame Connect → Home Assistant

Control a **Dimplex Vivente VVT75-EU** (and other Flame Connect fireplaces) from Home Assistant over Bluetooth.

There is no official local Home Assistant integration for the Flame Connect app. This project uses an **ESP32 running ESPHome** that pairs with the fireplace as a BLE client and exposes switches, numbers, and a select entity in Home Assistant.

> Based on [community work by gjongenelen](https://gist.github.com/gjongenelen/62d80815bd518841db55f64bb7ed4ec2), tested on a Dimplex Vivente VVT75-EU (BLE-only, no Wi-Fi/cloud).

## What works

| Feature | Status |
|---------|--------|
| Power on / off | Working |
| Heat on / off | Working |
| Temperature | Working (15–32 °C) |
| Flame intensity (F1–F6) | Working |
| Ambient lighting (AMB1–AMB7) | Working |
| Ambient 8 (custom theme) | Buttons / Flame Connect remote only |
| Volume | Working (0–6) |
| Sleep timer | Working (0–8 h, 0.5 h steps) |
| SmartSense | Working (Off / Min / Max) |

The fireplace accepts **one BLE client at a time**. While the Home Assistant ESP32 is connected, the Flame Connect app and the BLE remote cannot connect.

## What you need

1. **ESP32 dev board** (ESP32-WROOM or similar) with BLE client support
2. **Home Assistant** with ESPHome (add-on or standalone)
3. **Fireplace PIN** — 6-digit code on a sticker under the bottom front metal panel (same PIN the Flame Connect app uses)
4. **Fireplace BLE MAC address**

Place the ESP32 within about 10 m of the fireplace. This is a **direct BLE connection**, not a Bluetooth proxy.

A second ESP32 is optional and only used for diagnostic GATT dumps (`dimplex-ble-dump.yaml`). Do not run dump and production at the same time.

## Setup

### 1. Find your fireplace PIN

1. Lift the bottom front panel of the fireplace.
2. Find the QR code / sticker with a 6-digit PIN.
3. You need this for first-time BLE pairing.

### 2. Find your fireplace MAC address

**Option A — Flame Connect app (already paired):**
On iOS: Settings → Bluetooth → tap the (i) next to your fireplace.
On Android: use a BLE scanner such as **nRF Connect**.

**Option B — ESPHome scan:**
Flash `ble-scan.yaml` and watch the logs for nearby devices.

### 3. Configure secrets

```bash
cp secrets.yaml.example secrets.yaml
```

Edit `secrets.yaml` with your Wi-Fi credentials, fireplace MAC, and PIN.

Generate API encryption keys on first compile, or create them yourself:

```bash
esphome run dimplex-fireplace.yaml
```

### 4. Flash the ESP32

```bash
esphome run dimplex-fireplace.yaml
```

On first boot the ESP32 pairs with the fireplace using your PIN. Watch the logs for pairing and `Connected to device`.

### 5. Add to Home Assistant

Once the ESP32 is online, Home Assistant discovers it via the ESPHome integration.

Example entities (prefix depends on the ESPHome device name / HA area):

- `switch.dimplex_fireplace_fireplace` — on/off
- `switch.dimplex_fireplace_fireplace_heat` — heat
- `number.dimplex_fireplace_fireplace_temperature`
- `number.dimplex_fireplace_fireplace_flame` — F1–F6
- `number.dimplex_fireplace_fireplace_ambient` — AMB1–AMB7
- `number.dimplex_fireplace_fireplace_volume`
- `number.dimplex_fireplace_fireplace_timer`
- `select.dimplex_fireplace_fireplace_smartsense`

`homeassistant/dashboards/haard.yaml` is an example Lovelace dashboard. Rename the entity IDs to match your install.

## Diagnostic dump board

`dimplex-ble-dump.yaml` is a read-mostly GATT dumper for reverse-engineering. Flash it on a **second** ESP32 only:

1. Unplug / power off the production fireplace ESP32 (one BLE client).
2. Flash and connect the dump board.
3. Change one setting on the fireplace buttons.
4. Compare the dump logs.

Do not use the dump firmware as the Home Assistant controller.

## Troubleshooting

| Problem | Things to try |
|---------|---------------|
| Won’t pair | Confirm PIN. Power-cycle the fireplace. Disconnect the app and BLE remote. |
| Disconnects often | Move the ESP32 closer. Set `wifi.power_save_mode: none`. |
| App or remote stops working | Expected while the ESP32 is connected. Unplug the ESP32 to free BLE. |
| Ambient 8 does not change from HA | AMB1–7 apply over BLE; AMB8 is the custom theme and currently only follows the fireplace buttons / remote. |
| `No characteristic found` | Older Opti-myst models use a different protocol. See [Lammerink/Dimplex-Optimyst-BLE](https://github.com/Lammerink/Dimplex-Optimyst-BLE). |

## Protocol notes

The fireplace is a BLE GATT server. Pairing uses MITM bonding with the 6-digit PIN.

| UUID | Role |
|------|------|
| `0x1001` | Power / heat / temperature (6 bytes) |
| `0x1003` | Temperature set |
| `0x1017` / `0x1027` | Timer |
| `0x4010` | Volume (1 byte, 0–6) |
| `0x4013` | Ambient + flame. Set with `01 [AMB 1–7] [FIRE 1–6] 00 00 00 00` |

Power payloads (byte 0 of `0x1001`): `06` standby, `08` flame only, `02` heat on.

## Credits

- Protocol starting point: [gjongenelen](https://gist.github.com/gjongenelen/62d80815bd518841db55f64bb7ed4ec2)
- [Home Assistant community thread](https://community.home-assistant.io/t/is-it-possible-to-integrate-flame-connect-dimplex-fireplace-app-to-ha/626256)
