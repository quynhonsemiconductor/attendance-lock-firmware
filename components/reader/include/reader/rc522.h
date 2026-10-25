#pragma once
// Minimal RC522 driver over SPI: UID collection only (REQA/anticoll) — no
// MIFARE authentication, the attendance integration uses UIDs only (§2.5).
// Device-only (ESP-IDF SPI master).
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int spi_host;      // SPIx host id
    int cs_gpio;       // SDA/CS pin of this reader
    int rst_gpio;      // RST (NRSTPD) pin — hard reset + power-down between polls
} rc522_conf_t;

// Brings the bus up once (the second reader shares it). Returns false on
// SPI setup failure.
bool rc522_bus_init(int spi_host, int sck, int miso, int mosi);

// Hard reset (RST low→high), soft reset, antenna on. False when the
// VersionReg does not report a known MFRC522 — that reader is dead but the
// other keeps working.
bool rc522_init(const rc522_conf_t *conf);

// One poll: wake, REQA, anticoll. Returns true with the UID (4/7/10 bytes)
// when a card answered; false otherwise (also the "no card" case).
bool rc522_poll(const rc522_conf_t *conf, uint8_t *uid, uint8_t *uid_len);

// Hard power-down between polls (RST low; battery decisions 2026-10-25).
void rc522_power_down(const rc522_conf_t *conf);
