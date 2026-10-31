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

**Before the first sync test, verify the battery reading.** Tap an
allow-listed card and read `battery=` in the `tap r=0 ...` log line: it must
read **4500–6500 mV** for the 4.5–6.5 V pack/PSU. The firmware treats a
reading outside the plausibility window (Kconfig `POWER_BATTERY_MIN_MV` /
`POWER_BATTERY_MAX_MV`, default 3000–9000 mV) as a **sensor fault**: it logs
`battery reading implausible (<mv> mV): divider wiring?`, shows the red fault
LED, omits `battery_mv` from the sync envelope and sends
`"battery_fault":true` instead, and denies releases (fail-secure). A reading
near 33,9xx mV means the divider tap is not wired (GPIO1 floating) — fix the
wiring (§2) before testing sync.

Moving the terminal to another server (re-provisioning): register on the
new server first, then on the console set the **token before the url**. A
sync session that runs between the two writes fails 401 and keeps its queue
(events are retried with backoff, nothing is lost), and finishing with the
url means the new server's first contact already carries the matching
token — its terminal health starts clean instead of logging bad-token
attempts.

### Reading the sync log

Every session ends with one INFO line, success included, so the cadence is
visible without the server log:

```
sync: session ok|failed (<stage>: <detail>) allow-list=<200|304|status|none> events=<acked>/<sent> misreads=<n> next in <s> s (backoff <n>/5)
```

| Line | Meaning | Fix |
|---|---|---|
| `session ok allow-list=200 events=0/0 misreads=0 next in 3600 s (backoff 0/5)` | Clean session: allow-list fetched, empty-ring heartbeat sent, reader UID reads all clean. | None. |
| `misreads=5` | The reader rejected 5 UID reads since the last session (a BCC mismatch, or the confirm-read saw two different UIDs). Rejected reads are NOT taps and send no event; the warning `uid read rejected (bcc|confirm)` is rate-limited to one per 10 s. | A few under noise is normal; a stream means a loose SPI/antenna wire (MISO/CS), a card too far away, or two cards in the field. Check the §2 wiring. |
| `allow-list=304` | Cached list unchanged (the ETag matched). | None. |
| `allow-list=none` | The GET never reached an HTTP status (transport failed). | Read the `(<stage>: ...)` part. |
| `events=3/3` | Server acked 3 of the 3 distinct events sent this session. | None. |
| `events=0/5` | 5 events were attempted, none acked; the batch stays queued. | Read the stage/detail; nothing is lost. |
| `session failed (wifi: radio did not connect)` | SSID/password wrong, AP down, or the AP is 5 GHz-only. | `set wifi ssid`, `set wifi pass`; check the AP. |
| `session failed (ip: associated, no IP (DHCP))` | Associated but no DHCP address within 15 s. | Check the AP's DHCP pool / client isolation. |
| `session ok (sntp: no time source)` | No SNTP and no usable Date header; events carry 1970 + `time_synced:false` (correct per contract). | Open UDP 123 or check the server's Date header. Not a sync failure. |
| `session failed (dns: cannot resolve or connect)` | URL typo, host down, or a blocked port — ESP-IDF reports DNS and TCP-connect failures alike. | `set url <scheme://host[:port]>`; reach the host from a laptop first. |
| `session failed (dns: ..., code 0x...)` | Same failure with the platform error code. | As above; quote the code when asking for help. |
| `session failed (tls: TLS handshake failed, code 0x...)` | HTTPS on the wrong port, an untrusted certificate, or a clock that makes the cert not-yet-valid. | Confirm `https://` and the port/CA; the first session after a cold boot may need SNTP before TLS can validate dates. |
| `session failed (http 401: terminal token refused: check `set token`)` | Token wrong, rotated, or the terminal was deleted. | Re-register the terminal in opshub (§7) and `set token`. |
| `session failed (http 422: server rejected the envelope)` | The server refused the JSON envelope; the warning just above quotes the first 120 bytes of its body. | Fix the named field. A bad battery reading now arrives as `battery_fault`, so a 422 here is a different field. |
| `session failed (http 500: unexpected status)` | Server-side error. | Check the opshub log; the ring keeps the events. |
| `session failed (config: unprovisioned — run `set url` and `set token`)` | NVS has no URL/token. | §4. |
| `session failed (config: no Wi-Fi credentials ...)` | NVS has no SSID/password. | §4. |
| `next in 30 s (backoff 3/5)` | Retry cadence: 30 s doubling per consecutive failure, capped at 8 min plus 0–25 % jitter; n is the consecutive-failure count, shown capped at 5. | None — it returns to `backoff 0/5` and the normal interval after one clean session. |

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
| 14 | Wi-Fi sync (opshub develop) | Provision the 4 NVS keys with the console (§4), wait one sync interval (default 60 min — lower `SYNC_INTERVAL_MIN` for the bench) | Session log: Wi-Fi up, SNTP, allow-list fetch (200 or 304), events uploaded, and one `sync: session ...` summary line per session (§4 "Reading the sync log"); server sees `battery_mv` + `low_battery` on the envelope EVERY session (heartbeat with an empty ring) | |
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
