# Bench prototype — KHONG dung cho cua that

Thu muc nay la **prototype ban thu nghiem** (Arduino IDE, ESP32-S3 + 2x RC522): dung de
thu phan cung, do xung servo, thu the RFID. **Khong co cai dat tai cong tu that.**

- Khong bao mat production: web Basic Auth, khong TLS, khong secure boot.
- Firmware san xuat theo ADR v2 (ESP-IDF v6.0.x) se **viet lai hoan toan**,
  khong port tu code nay.
- Thay doi o day khong anh huong firmware production.

Lap trinh nhanh: `arduino-cli compile --fqbn esp32:esp32:esp32s3 prototype/rfid_door_arduino`
(sau khi copy `secrets.example.h` -> `secrets.h` va dien thong tin that).
