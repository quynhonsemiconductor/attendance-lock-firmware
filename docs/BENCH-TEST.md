# Bench test — hardware bring-up checklist

> **Tóm tắt (VN):** Tài liệu này dành cho Vinh để chạy thử thiết bị trên bàn
> (bench): danh sách linh kiện, cách nối dây theo chân Kconfig mặc định, cách
> build/nạp firmware (ESP-IDF v6.0.3, profile `dev`), và bảng kiểm tra từng
> bước (đọc thẻ 4 byte, từ chối thẻ lạ, mở/khoá cửa, pin yếu, mất nguồn,
> ngắt đầu đọc, đồng bộ Wi-Fi). Ghi kết quả vào cột **Pass?**, chụp ảnh bảng
> đã điền, lưu log bằng `idf.py monitor` rồi gửi lại kèm số dòng điện đo
> được và SHA firmware.

## 1. Parts

| Part | Notes |
|---|---|
| ESP32-S3 **N16** devkit | 16 MB flash is required (two 3.2 MB OTA slots + 4 MB event ring do not fit smaller parts) |
| RC522 RFID module ×2 (start with 1) | 3.3 V, SPI. Single-reader bench: wire the *outside* reader (reader 0) only |
| Servo (bolt) | Bench PSU powered, signal from GPIO4; never from the devkit 5 V pin |
| Buzzer | active buzzer, GPIO5 |
| Bi-colour LED (green + red) | green GPIO16, red GPIO47 (or two LEDs) |
| Adjustable bench PSU | 4.5–6.5 V, current-limited, replaces the battery pack |
| USB power meter or multimeter | sleep current + servo current |
| Test cards | at least one 4-byte-UID card and one 7-byte-UID card (phone NFC app or another reader shows the UID length) |
| 100 kΩ + 10 kΩ resistors | battery divider (11:1) |

## 2. Wiring (Kconfig defaults)

| Signal | GPIO | Node |
|---|---|---|
| SPI SCK | 12 | RC522 SCK |
| SPI MISO | 13 | RC522 MISO |
| SPI MOSI | 11 | RC522 MOSI |
| Reader 0 (outside) CS | 10 | RC522 SDA/SS |
| Reader 0 (outside) RST | 17 | RC522 RST (power-down between polls) |
| Reader 1 (inside) CS | 9 | leave open on a single-reader bench |
| Reader 1 (inside) RST | 21 | leave open on a single-reader bench |
| Servo signal | 4 | 50 Hz, 1000 µs locked / 2000 µs released |
| LED green | 16 | |
| LED red | 47 | |
| Buzzer | 5 | |
| Battery divider tap | 1 | ADC1 channel 0; pack+ → 100 k → tap → 10 k → GND |

Both RC522 modules share one SPI bus (hand-driven CS). The servo and the
divider both reference the PSU/battery rail, not the devkit 3V3. Common
ground between PSU, servo and devkit is mandatory.

Expected single-reader log noise: `reader 1 offline at boot — probing every
5000 ms` every ~5 s — the unconnected second reader probing on its recovery
cadence. That is normal, not a fault.

## 3. Flash steps

Match the CI-pinned toolchain: **ESP-IDF v6.0.3**. Any v6.0.x is fine on the
bench; do not use v6.1 (beta).

```sh
idf.py set-target esp32s3
idf.py -B build-dev -DSDKCONFIG="build-dev/sdkconfig" \
       -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.dev" build
idf.py -B build-dev -DSDKCONFIG="build-dev/sdkconfig" flash monitor
```

The `dev` profile has **no** hardening (no secure boot, no flash encryption)
— bench only. Do not flash `sdkconfig.defaults.prod` on a devkit without the
eFuse plan in [PROVISIONING.md](PROVISIONING.md); a prod image on an
un-burned devkit bricks itself at first boot.

Logs: `idf.py monitor` (Ctrl+] to exit). Save the whole session — send the
full log back, not excerpts.

## 4. Provisioning note (sync rows only)

The serial provisioning console is still a stub (next PR), but the sync task
already reads four NVS keys from namespace `prov`: `url`, `token`,
`wifi_ssid`, `wifi_pass`. To exercise the Wi-Fi rows now, write them into the
`nvs` partition with `nvs_partition_gen` + `parttool.py` (ESP-IDF tools):

```sh
# prov.csv: key,type,encoding,value rows for url/token/wifi_ssid/wifi_pass
python $IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py \
       generate prov.csv prov.bin 0x10000
parttool.py write_partition --partition-name nvs --input prov.bin
```

Or skip rows 14–15 until the provisioning console lands.

## 5. Bench checklist

Timings referenced: unlock hold 5 s, deny red 2 s, beeps 120 ms on/off,
servo move 700 ms, poll 400 ms.

| # | Step | How | Expected | Pass? |
|---|---|---|---|---|
| 1 | Boot locked | PSU on, watch servo + LED | Servo moves to the LOCKED position, LEDs off, `battery low at boot` only if under 5.0 V; no unlock | |
| 2 | Card read, 4-byte UID | Tap an allow-listed card flat on the reader | Green + single beep, unlock (servo to released), relock after 5 s; log `tap r=0 allow … battery=…mV` | |
| 3 | Unknown card deny | Tap a 4-byte-UID card NOT on the allow-list | Red 2 s + double beep, stays locked, log `tap r=0 deny (unknown-card)` | |
| 4 | Re-tap extends | Tap allow-listed card, tap again within 5 s | Window restarts from the second tap (relock ~5 s after it), no second beep storm | |
| 5 | 7-byte UID card | Tap the 7-byte-UID card | Skipped silently: log `7/10-byte UID (cascade) not supported — card skipped`, no feedback, no unlock. This is the current design (CL2 not implemented) | |
| 6 | Servo released after move | Listen/feel + current meter during unlock | Signal stops after the 700 ms move (Kconfig `LOCK_SERVO_RELEASE`): no hum, no holding current | |
| 7 | Sleep current | USB meter between taps, lock idle, no sync session | Light sleep between the 400 ms polls; record mA (and µA if the meter resolves it) | |
| 8 | Low-battery alert | Sweep PSU 5.2 → 4.9 V slowly | At < 5.0 V: log `battery low`, event `low_battery=1`; door still works (alert is report-only) | |
| 9 | Release deny below cutoff+margin | At 4.99–4.8 V tap an allow-listed card | DENY: red 2 s + double beep, stays locked, log `tap r=0 deny (low-battery) … battery=…mV` (deny gate = 4800 + 200 mV) | |
| 10 | Boot-window deny | Set PSU to 4.9 V, power-cycle, tap during the first ~0.7 s (while the boot servo move runs) | DENY, not allow: the battery cache is primed before the boot move (this PR) | |
| 11 | Power cut mid-unlock | Cut PSU while the lock is released, restore | Boots LOCKED (step 1 behaviour). Optional: confirm the buffered tap events survive (event ring) | |
| 12 | Brownout backoff | Briefly dip the PSU so the board browns out ×2, then tap | Log `brownout resets: N, release blocked …ms after boot`; releases denied during the backoff window (doubling), taps still log | |
| 13 | Reader unplugged | Pull the RC522 CS/SPI jumper while running | Log `reader 0 fault (health check) — probing`, red LED, door stays locked; replug → `reader 0 back online` within ~5 s and taps work again | |
| 14 | Wi-Fi sync (opshub develop) | Provision the 4 NVS keys (§4), wait one sync interval (default 60 min — lower `SYNC_INTERVAL_MIN` for the bench) | Session log: Wi-Fi up, SNTP, allow-list fetch (200 or 304), events uploaded; server sees `battery_mv` + `low_battery` fields on the events | |
| 15 | Read distance / placement | Move the card away/angle it | Record max stable distance (typically 0–3 cm) and any dead angles; RC522 antenna away from the servo wires | |

Row 5 note: 7-byte-UID cards are *rejected by design* in this build. The
result of interest is the log line — not an unlock.

## 6. What to send back

1. Photo of the filled-in table (or the file edited in place).
2. Full `idf.py monitor` logs, one per test session (at minimum rows 2, 3, 5,
   9, 10, 12, 13, 14).
3. Measured numbers: sleep current, servo-move current, read distance.
4. Firmware version you flashed: the `idf.py -B build-dev flash` banner +
   `git rev-parse HEAD` of the build (or `esptool.py image_info build-dev/*.bin`).
