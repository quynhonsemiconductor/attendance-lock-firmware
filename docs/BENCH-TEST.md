# Bench test — hardware bring-up checklist

> **Tóm tắt (VN):** Tài liệu này dành cho Vinh để chạy thử thiết bị trên bàn
> (bench): danh sách linh kiện, cách nối dây theo chân Kconfig mặc định, cách
> build/nạp firmware (ESP-IDF v6.0.3, profile `dev`), và bảng kiểm tra từng
> bước (đọc thẻ 4 byte, từ chối thẻ lạ, mở/khoá cửa, pin yếu — dưới 5.0 V
> thì báo động và từ chối mở cửa, mất nguồn,
> ngắt đầu đọc, đồng bộ Wi-Fi, cấp phát qua cổng serial và chạy thử đồng bộ
với opshub ngay trên laptop — mục 7). Ghi kết quả vào cột **Pass?**, chụp ảnh bảng
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
. $IDF_PATH/export.sh
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

## 4. Provisioning (sync rows)

Provision over the serial console (UART0) in `idf.py monitor` — type at the
`door> ` prompt. The four keys land in NVS namespace `prov`, exactly what the
sync task reads; the token and the Wi-Fi pass are write-only (`status`
reports set/unset, never their values):

```
door> set url https://example.vn
door> set token <terminal token>      # shown once when registering the terminal in opshub (§7)
door> set wifi ssid <ssid>
door> set wifi pass <pass>
door> status
```

`set url` accepts `http://` only in the dev build (no secure boot / flash
encryption, asserted in CI); the prod build rejects anything but `https://`.
The url must be a bare `scheme://host[:port]` — no path, query or fragment.
The console echoes `*` while a token or Wi-Fi pass is being typed, so the
secret never shows on the monitor or in the saved log.
Commands not listed above do not exist — there is no unlock, card or
test-output command. (Row 15 is read distance and does not need provisioning.)

Moving the terminal to another server (re-provisioning): register on the
new server first, then on the console set the **token before the url**. A
sync session that runs between the two writes fails 401 and keeps its queue
(events are retried with backoff, nothing is lost), and finishing with the
url means the new server's first contact already carries the matching
token — its terminal health starts clean instead of logging bad-token
attempts.

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
| 8 | Low-battery alert + release gate | Sweep PSU 5.2 → 5.0 → 4.9 V slowly; at 5.0 V and at 4.9 V tap an allow-listed card | At >= 5.0 V: tap unlocks normally (green + single beep). At < 5.0 V: log `battery low`, event `low_battery=1`, and the tap is DENIED (see row 9). Threshold is 5000 mV for both the alert and the release gate | |
| 9 | Release deny below 5.0 V | At 4.99 V and at 4.8 V tap an allow-listed card | DENY: red 2 s + double beep, stays locked, log `tap r=0 deny (low-battery) … battery=…mV`. Deny gate = cutoff 4800 + margin 200 = 5000 mV, same as the row 8 alert | |
| 10 | Boot-window deny | Set PSU to 4.9 V, power-cycle, tap during the first ~0.7 s (while the boot servo move runs) | DENY, not allow: the battery cache is primed before the boot move (this PR) | |
| 11 | Power cut mid-unlock | Cut PSU while the lock is released, restore | Boots LOCKED (step 1 behaviour). Optional: confirm the buffered tap events survive (event ring) | |
| 12 | Brownout backoff | Briefly dip the PSU so the board browns out ×2, then tap | Log `brownout resets: N, release blocked …ms after boot`; releases denied during the backoff window (doubling), taps still log | |
| 13 | Reader unplugged | Pull the RC522 CS/SPI jumper while running | Log `reader 0 fault (health check) — probing`, red LED, door stays locked; replug → `reader 0 back online` within ~5 s and taps work again | |
| 14 | Wi-Fi sync (opshub develop) | Provision the 4 NVS keys with the console (§4), wait one sync interval (default 60 min — lower `SYNC_INTERVAL_MIN` for the bench) | Session log: Wi-Fi up, SNTP, allow-list fetch (200 or 304), events uploaded; server sees `battery_mv` + `low_battery` on the envelope EVERY session (heartbeat with an empty ring) | |
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

## 7. Sync test against a local opshub

The whole loop (allow-list down, events up) against an opshub running on the
bench laptop — no develop environment needed. Dev profile only: it
provisions a plain-http base URL; against a deployed opshub always use
`https://`.

1. Run opshub locally (repo `opshub`, see its README "Local stack"):
   ```sh
   cp .env.example .env
   pnpm install
   docker compose -f docker-compose.dev.yml up -d --wait
   pnpm db:migrate
   pnpm start:dev        # API  -> http://localhost:3001
   pnpm dev:web          # SPA  -> http://localhost:5174
   ```
2. Sign in to the SPA (test login with a seeded demo user), open the **Door**
   page and **register a terminal** — the token is shown ONCE, copy it. Add
   a card with the UID of a test card (4-byte UID).
3. Find the laptop's LAN IP (`ifconfig` / `ip a`). The board must reach the
   API directly (not the SPA's dev proxy): provision `set url
   http://<laptop-ip>:3001` plus token and Wi-Fi (§4). Set
   `SYNC_INTERVAL_MIN = 1` (menuconfig → Sync) so the bench does not wait an
   hour between sessions.
4. Expected on the board (session log): Wi-Fi up, allow-list fetch **200**
   first session then **304** once the ETag matches, buffered taps uploaded.
5. Expected in opshub: the Door page lists the terminal as seen/healthy, and
   the taps appear as events — allow-listed taps as access events, unknown
   cards also under the unknown-cards view.
6. **Session soak (≥3 consecutive sessions at `SYNC_INTERVAL_MIN = 1`):**
   every session — not just the first — must log Wi-Fi up with an IP,
   the allow-list exchange (304 after the first) and the events POST (the
   envelope carries `battery_mv`/`low_battery` each session, empty ring
   included). Session 2+ getting no IP or a "malformed" 200 on every
   response means the netif/driver lifetime regression is back — the netif
   and the Wi-Fi driver are now created ONCE and only started/stopped per
   session. When the ring holds more than 100 events, one session drains it
   in back-to-back batches (≤100 each, 60 s budget) until the queue is
   empty.
