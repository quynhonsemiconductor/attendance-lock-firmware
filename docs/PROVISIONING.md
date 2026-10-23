# Provisioning & eFuse plan (prod profile)

Everything here is run by hand, offline, with `espefuse` from an ESP-IDF
v6.0.x shell — **never in CI**. eFuse writes are IRREVERSIBLE.

This document contains no hostnames, URLs, tokens or keys; the base URL and
the terminal token are provisioned at runtime over the serial console
(UART0, inside the enclosure).

## eFuse key block plan

| Block | Purpose |
|---|---|
| `BLOCK_KEY0` | flash encryption key (generated on-device at first encrypted boot) |
| `BLOCK_KEY1` | Secure Boot v2 signing-key digest(es) |
| `BLOCK_KEY2` | NVS HMAC key (`CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID=2`) |
| `BLOCK_KEY3+` | unused, kept free |

## One-time preparation (per production terminal)

1. **Sign the app image offline** (Secure Boot v2, `CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n`
   on purpose — signing never happens on a networked machine):

   ```sh
   espsecure.py generate_signing_key --version 2 secure_boot_key.pem   # offline, once
   espsecure.py sign_data --version 2 --keyfile secure_boot_key.pem \
       --output signed-bootloader.bin build-prod/bootloader/bootloader.bin
   espsecure.py sign_data --version 2 --keyfile secure_boot_key.pem \
       --output signed-attendance-lock.bin build-prod/attendance_lock.bin
   ```

2. **Burn the Secure Boot v2 digest** (digest of `secure_boot_key.pem` into
   `BLOCK_KEY1`, revokes JTAG because `CONFIG_SECURE_BOOT_ALLOW_JTAG` is unset):

   ```sh
   espsecure.py digest_rsa_public_key --keyfile secure_boot_key.pem  # -> digest
   espefuse.py --port <port> burn-key block_key1 <digest> SECURE_BOOT_DIGEST0
   ```

3. **USB-Serial-JTAG**: there is no Kconfig switch for it. Burn the eFuse
   BEFORE the first prod boot (after this, only UART0 remains as console):

   ```sh
   espefuse.py --port <port> burn-efuse DIS_USB_SERIAL_JTAG 1
   ```

4. **Flash encryption** (release mode): generate + burn into `BLOCK_KEY0`,
   then enable:

   ```sh
   espefuse.py --port <port> generate_key --keyfile fe_key.bin BLOCK_KEY0
   espefuse.py --port <port> burn_efuse FLASH_CRYPT_CNT
   ```

5. **NVS HMAC key** (`BLOCK_KEY2`, used by `CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC`):

   ```sh
   espefuse.py --port <port> generate_key --keyfile nvs_hmac_key.bin BLOCK_KEY2 HMAC_UP
   ```

6. Flash the signed bootloader + signed app + partition table (`idf.py flash`
   is fine for the DEV profile only; prod uses `esptool.py write_flash` with
   the signed artifacts).

7. **Provision at runtime** over the serial console: set the base URL and the
   terminal token. The console can set them and print status; the firmware
   never reads the token back, and has no unlock, card or test-output
   commands.

## Verification

After first boot, confirm:

- `espefuse.py --port <port> summary` — `DIS_USB_SERIAL_JTAG=1`,
  `SECURE_BOOT_DIGEST0` in `BLOCK_KEY1`, `FLASH_CRYPT_CNT` set, HMAC key in
  `BLOCK_KEY2`
- the boot log shows Secure Boot v2 enabled and flash encryption in RELEASE
  mode
- CI's prod assert (resolved `sdkconfig`) matched the profile before any of
  this: `CONFIG_SECURE_BOOT_V2_ENABLED`, `CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE`,
  `CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE`, `CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID=2`,
  and `CONFIG_SECURE_BOOT_ALLOW_JTAG` unset.
