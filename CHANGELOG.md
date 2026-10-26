# Changelog

## 0.1.0 (2026-10-26)


### Features

* **clock:** rtc-backed time base with sntp and date-header fallback ([#12](https://github.com/quynhonsemiconductor/attendance-lock-firmware/issues/12)) ([2ac3bc8](https://github.com/quynhonsemiconductor/attendance-lock-firmware/commit/2ac3bc8dddb602de77db383cd86ff6778447eed9))
* **door,main:** door FSM task — one owner, queue-fed, battery-telemetry events ([#17](https://github.com/quynhonsemiconductor/attendance-lock-firmware/issues/17)) ([925cc01](https://github.com/quynhonsemiconductor/attendance-lock-firmware/commit/925cc0171fd271b466d45a18e7879fd3df18426d))
* **reader,lock,power:** drivers — rc522 from the prototype, lock choice, calibrated power ([#16](https://github.com/quynhonsemiconductor/attendance-lock-firmware/issues/16)) ([7ab71df](https://github.com/quynhonsemiconductor/attendance-lock-firmware/commit/7ab71dfa546942db53464338f0203f9cb8b54ddc))
* **rfid-door:** firmware khoa cua RFID 2 dau doc RC522 tren ESP32-S3 ([#1](https://github.com/quynhonsemiconductor/attendance-lock-firmware/issues/1)) ([9f769f4](https://github.com/quynhonsemiconductor/attendance-lock-firmware/commit/9f769f4869b8117bbc733daf467b89b5a5817425))
* scaffold the esp-idf v6 firmware per adr v2 (work item 6) ([#4](https://github.com/quynhonsemiconductor/attendance-lock-firmware/issues/4)) ([7b212a3](https://github.com/quynhonsemiconductor/attendance-lock-firmware/commit/7b212a3e851ef4f630caddcf462b9acf405f3100))
* **store:** nvs allow-list cache and encrypted-partition event ring ([#10](https://github.com/quynhonsemiconductor/attendance-lock-firmware/issues/10)) ([ef824c8](https://github.com/quynhonsemiconductor/attendance-lock-firmware/commit/ef824c88643001674f7ffa86b024f18017ddd7f4))
* **sync:** bounded sync sessions over the terminal api ([#14](https://github.com/quynhonsemiconductor/attendance-lock-firmware/issues/14)) ([e7159e0](https://github.com/quynhonsemiconductor/attendance-lock-firmware/commit/e7159e02f3fbb16475c7e08002a95929aa21db3b))


### Bug Fixes

* **prototype:** park the bench prototype and remove default credentials and remote unlock ([#3](https://github.com/quynhonsemiconductor/attendance-lock-firmware/issues/3)) ([7516e0d](https://github.com/quynhonsemiconductor/attendance-lock-firmware/commit/7516e0da3c9183f986242d899a4025138b6fa091))
* **reader,main,docs:** crc error check, boot battery cache, bench checklist ([#18](https://github.com/quynhonsemiconductor/attendance-lock-firmware/issues/18)) ([afafd70](https://github.com/quynhonsemiconductor/attendance-lock-firmware/commit/afafd70dfce9ec5bc978ff1533a2bf966674937c))

## Changelog
