# Provisioning & eFuse plan (prod profile)

Everything here is run by hand, offline, with `espsecure` / `espefuse` from an
ESP-IDF v6.0.x shell — **never in CI**. eFuse writes are IRREVERSIBLE.

Commands use the esptool v5 hyphenated forms
(`espsecure generate-signing-key`, `espefuse burn-key-digest`, ...).

This document contains no hostnames, URLs, tokens or keys; the base URL and
the terminal token are provisioned at runtime over the serial console
(UART0, inside the enclosure).

## eFuse key block plan

| Block | Purpose |
|---|---|
| `BLOCK_KEY0` | flash encryption key (AES-128; generated ON-CHIP at first encrypted boot) |
| `BLOCK_KEY1` | Secure Boot v2 signing-key digest (`SECURE_BOOT_DIGEST0`) |
| `BLOCK_KEY2` | NVS HMAC key (generated on-chip by `nvs_sec_provider`; `CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID=2`) |
| `BLOCK_KEY3` | Secure Boot v2 backup signing-key digest 1 (`SECURE_BOOT_DIGEST1`) |
| `BLOCK_KEY4` | Secure Boot v2 backup signing-key digest 2 (`SECURE_BOOT_DIGEST2`) |
| `BLOCK_KEY5` | spare |

Burn the backup digests UP FRONT: unused secure-boot digest slots are
REVOKED at first boot (`CONFIG_SECURE_BOOT_ALLOW_UNUSED_DIGEST_SLOTS=n`,
changeable only in insecure mode) — KEY3/KEY4 cannot be "kept for later".

AES-128 flash encryption is deliberate: AES-256 needs two key blocks and
would collide with `BLOCK_KEY2` (`CONFIG_SECURE_FLASH_ENCRYPTION_AES128=y`,
asserted in CI).

## One-time flow (per production terminal)

1. **Generate the signing key offline** - on a machine that never touches CI
   or the network:

   ```sh
   espsecure generate-signing-key --version 2 --scheme rsa3072 secure_boot_key.pem
   espsecure generate-signing-key --version 2 --scheme rsa3072 backup_key1.pem
   espsecure generate-signing-key --version 2 --scheme rsa3072 backup_key2.pem
   ```

   Store the backup keys OFFLINE, in a different place from the primary —
   a revoked primary is recoverable through a backup digest.

2. **Burn the Secure Boot digest** (of the signing key) - this also revokes
   JTAG, because `CONFIG_SECURE_BOOT_ALLOW_JTAG` stays unset:

   ```sh
   espefuse burn-key-digest BLOCK_KEY1 secure_boot_key.pem SECURE_BOOT_DIGEST0
   espefuse burn-key-digest BLOCK_KEY3 backup_key1.pem SECURE_BOOT_DIGEST1
   espefuse burn-key-digest BLOCK_KEY4 backup_key2.pem SECURE_BOOT_DIGEST2
   ```

3. **Disable USB-Serial-JTAG** (permanent; from here on the console is UART0
   only). Burn BEFORE the first prod boot:

   ```sh
   espefuse burn-efuse DIS_USB_SERIAL_JTAG 1
   ```

4. **Sign and flash the PLAINTEXT images.** The build does not sign
   (`CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n` - signing never happens on a
   networked machine):

   ```sh
   espsecure sign-data --version 2 --keyfile secure_boot_key.pem \
       --output signed-bootloader.bin build-prod/bootloader/bootloader.bin
   espsecure sign-data --version 2 --keyfile secure_boot_key.pem \
       --output signed-attendance-lock.bin build-prod/attendance_lock.bin
   # NEVER hard-code addresses — read them from build-prod/flash_args of the
   # exact build being flashed (they move whenever the partition table
   # moves). The bootloader and app entries are REPLACED with the signed
   # files:
   #
   #   grep -E '^0x' build-prod/flash_args
   #   # →  0x0       bootloader/bootloader.bin
   #   #    0x<pt>      partition_table/partition-table.bin
   #   #    0x<otadata> ota_data_initial.bin
   #   #    0x<app>     attendance_lock.bin
   #
   #   esptool --chip esp32s3 --port <UART0> write-flash \
   #       0x0         signed-bootloader.bin \
   #       0x<pt>      build-prod/partition_table/partition-table.bin \
   #       0x<otadata> build-prod/ota_data_initial.bin \
   #       0x<app>     signed-attendance-lock.bin
   ```

   Flash the plaintext images while flash encryption is still OFF.

5. **First boot does the rest on-chip** - no key file ever exists on the host
   except the signing key:
   - the bootloader generates the flash-encryption key in `BLOCK_KEY0`
     (AES-128) and encrypts the flash in place — **the first boot is SLOW**,
     it encrypts the 4 MB event_ring partition in place; let it finish;
   - `nvs_sec_provider` generates the NVS HMAC key in `BLOCK_KEY2`.

## Verification

After first boot, confirm:

- `espefuse summary` - `DIS_USB_SERIAL_JTAG=1`, `SECURE_BOOT_DIGEST0` in
  `BLOCK_KEY1`, `SPI_BOOT_CRYPT_CNT` set (S3 name; ESP32 called it
  `FLASH_CRYPT_CNT`), HMAC key present in `BLOCK_KEY2`
- the boot log shows Secure Boot v2 enabled and flash encryption in RELEASE
  mode
- CI's prod assert (resolved `sdkconfig`) matched the profile before any of
  this: `CONFIG_SECURE_BOOT_V2_ENABLED`,
  `CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE`,
  `CONFIG_SECURE_FLASH_ENCRYPTION_AES128`,
  `CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE`,
  `CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID=2`, `CONFIG_ESP_TASK_WDT_PANIC=y`, and
  `CONFIG_SECURE_BOOT_ALLOW_JTAG` unset.

## Runtime provisioning

Over the serial console (UART0, inside the enclosure): set the base URL and
the terminal token. The console can set them and print status; the firmware
never reads the token back, and has no unlock, card or test-output commands.
