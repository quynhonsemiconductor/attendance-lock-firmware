#pragma once
// Minimal MFRC522 driver over SPI: UID collection only (REQA + anticoll +
// select) — no MIFARE authentication, the attendance integration uses UIDs
// only (§2.5). Device-only (ESP-IDF SPI master). Register sequence ported
// from the bench-proven prototype firmware (PR#1).
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int spi_host;      // SPIx host id
    int cs_gpio;       // SDA/CS pin of this reader
    int rst_gpio;      // RST (NRSTPD) pin — hard reset + power-down between polls
} rc522_conf_t;

// Brings the bus up once (the second reader shares it; one spi device,
// hand-driven CS). Returns false on SPI setup failure.
bool rc522_bus_init(int spi_host, int sck, int miso, int mosi);

// Boot bring-up: pins, hard reset, init, health check. Does NOT power down
// — pair with rc522_power_down(). False when the reader fails its health
// check: that reader is dead but the other keeps working.
bool rc522_init(const rc522_conf_t *conf);

// The prototype's health check: VersionReg readable and TPrescaler still
// holding the init value (0xA9). 0x00/0xFF = nothing on the bus.
bool rc522_healthy(const rc522_conf_t *conf);

// One poll: wake from hard power-down (self-resets the chip — the init
// re-runs), REQIDL, anticoll, select, halt. True with a 4-byte UID when a
// card answered; false otherwise (also "no card"). 7/10-byte cascade UIDs
// are rejected (skipped) until CL2 exists.
bool rc522_wake_poll(const rc522_conf_t *conf, uint8_t *uid, uint8_t *uid_len);

// Hard power-down between polls (NRSTPD low; battery/wake decisions
// 2026-10-25).
void rc522_power_down(const rc522_conf_t *conf);
