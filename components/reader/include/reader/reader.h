#pragma once
// Two RC522 readers on one SPI bus (outside = IN, inside = OUT), polled —
// the RC522 sits in hard power-down (RST low) between polls (battery/wake
// decisions 2026-10-25: tap-only, ~400 ms poll cadence). Between polls the
// task delays; tickless idle light-sleeps the chip unless the door task
// holds its pm lock (lock released / servo moving).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One observed card tap. reader: 0 = outside (in), 1 = inside (out).
typedef struct {
    uint8_t reader;
    uint8_t uid_len;
    uint8_t uid[10];
} reader_card_t;

// Bytes of scratch state reader_accept() needs (zero before first use).
size_t reader_state_size(void);

// Starts the poll task. Both callbacks run in the reader task's context —
// they must only post (the door task's queue), never block or touch the
// door state. on_fault fires when a reader that was healthy stops answering.
void reader_start(void (*on_card)(const reader_card_t *card),
                  void (*on_fault)(uint8_t reader));

// Should this tap be processed? Host-testable tap logic: the same card on
// the same reader within debounce_ms is a still-held card (one event per
// tap), and a card seen on the OTHER reader within cross_read_ms is the
// same physical pass being read twice (prototype's cross-read suppression).
// state must be zeroed before first use.
bool reader_accept(uint8_t *state, size_t state_cap, const reader_card_t *card, uint32_t now_ms,
                   uint32_t debounce_ms, uint32_t cross_read_ms);

// ISO 14443-3 BCC over the four CL1 UID bytes. Host-testable: a corrupted
// anticoll reply (loose SPI/antenna) is rejected before it can become a tap
// (bench 2026-10-31: ghost UIDs reached opshub as unknown cards).
bool reader_bcc_ok(const uint8_t uid[4], uint8_t bcc);

// Confirm-read: two INDEPENDENT reads of the card must return the same
// 4-byte UID. One corrupted pass is not a tap; the driver reads each card
// twice inside one poll (REQA then WUPA after the first HALT) and only the
// second, identical read reaches this check.
bool reader_uid_confirmed(const uint8_t *first, const uint8_t *second, uint8_t uid_len);

// UID reads rejected by the driver since boot (BCC mismatch, confirm-read
// mismatch). Monotonic; the sync port reports the per-session delta.
uint32_t reader_misread_count(void);

#ifdef __cplusplus
}
#endif
