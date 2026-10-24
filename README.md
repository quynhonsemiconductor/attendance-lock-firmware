# attendance-lock-firmware

Production firmware for the door terminal (ESP32-S3): RFID readers in/out,
fail-secure strike, allow-list cache synced from opshub, buffered event
upload. Written against the attendance-device ADR v2 — this repo is the
ESP-IDF v6.0.x rewrite, **not** a port of the Arduino prototype.

> The bench prototype lives in [`prototype/`](prototype/) once PR #1's
> follow-ups land. It is a hardware bring-up aid only; nothing in it ships.

## Layout

| Path | What |
|---|---|
| `main/` | app entry; boots locked, fail-secure order |
| `components/reader` | card readers (in/out lanes) |
| `components/lock` | fail-secure strike output |
| `components/store` | allow-list cache + event ring (encrypted NVS / encrypted data partition) |
| `components/sync` | allow-list poll + event batch upload (60 s poll) |
| `components/clock` | NTP + server `Date` fallback |
| `components/provision` | serial provisioning: base URL + token, write-only |
| `tests/host/` | host unit tests (Unity, plain cmake): store ring + cache covered |
| `partitions.csv` | two OTA app slots + encrypted event-ring partition |

The base URL and the terminal token are **provisioned at runtime** over the
serial console (inside the enclosure). Nothing is baked into the image; there
are no hostnames, URLs or keys in this repository.

## Build

Requires ESP-IDF **v6.0.x** (v6.1 is beta and unsupported). The same build runs
in CI pinned to the `espressif/idf` container by tag + digest (v6.0.x line).

```sh
. $IDF_PATH/export.sh
idf.py set-target esp32s3
idf.py -B build-dev        -DSDKCONFIG=build-dev/sdkconfig        -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.dev" build
```

Profiles: `dev` (no hardening, bench only) and `prod` (Secure Boot v2, flash
encryption RELEASE mode, secure ROM download mode, HMAC-based NVS encryption
with the key in eFuse — see `sdkconfig.defaults.prod` and
`docs/PROVISIONING.md` for the irreversible eFuse plan). JTAG is revoked by
Secure Boot v2 (`CONFIG_SECURE_BOOT_ALLOW_JTAG` stays unset, asserted in CI);
USB-Serial-JTAG is burned off with `espefuse` before the first prod boot.

**Always build each profile into its own `-B build-<profile>` directory with
the explicit `SDKCONFIG`** — a shared build dir lets one profile inherit the
other's values. Prod signing is manual and offline (`espsecure.py`); the
repository's build does not sign.

Requires the ESP32-S3 **N16 module (16 MB flash)** — the partition table
(two 3.2 MB OTA slots + a 4 MB event ring) does not fit smaller parts.

## Flash (dev bench)

```sh
idf.py -p <port> flash monitor
```

**Never flash the `prod` profile casually: it burns eFuses, and that is
irreversible.** Burning happens only on production hardware, with the signing
and encryption keys kept offline (manual step, work item 6/8).

## Tests

```sh
cmake -S tests/host -B build-host && cmake --build build-host && ctest --test-dir build-host
```

Deviation, on purpose for now: host tests run on the native target, not the
IDF linux target. Work item 7 moves NVS-touching code under test onto the IDF
linux target so the encrypted-storage code paths are exercised with the real
component stack.

CI (`build-dev`, `build-prod`, `host-tests`) runs all of it on every PR.
