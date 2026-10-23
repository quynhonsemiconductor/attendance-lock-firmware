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
| `tests/host/` | host unit tests (Unity, plain cmake) |
| `partitions.csv` | two OTA app slots + encrypted event-ring partition |

The base URL and the terminal token are **provisioned at runtime** over the
serial console (inside the enclosure). Nothing is baked into the image; there
are no hostnames, URLs or keys in this repository.

## Build

Requires ESP-IDF **v6.0.x** (v6.1 is beta and unsupported). The same build runs
in CI pinned to the `espressif/idf` container by digest.

```sh
. $IDF_PATH/export.sh
idf.py set-target esp32s3
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.dev" build
```

Profiles: `dev` (no hardening, bench only) and `prod` (Secure Boot v2, flash
encryption release mode, ROM download secure, JTAG/USB-Serial-JTAG disabled,
HMAC-based NVS encryption — see `sdkconfig.defaults.prod`).

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

CI (`build-dev`, `build-prod`, `host-tests`) runs all of it on every PR.
