#pragma once
// Two RC522 readers on one SPI bus (outside = IN, inside = OUT), polled —
// the RC522 sits in hard power-down (RST low) between polls (battery/wake
// decisions 2026-10-25). The wake strategy is behind Kconfig so a button or
// an LPCD reader can be swapped in without a rewrite.

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

// Starts the poll task. on_card runs in the reader task's context — it must
// not block (it pushes to the ring and drives the lock, both non-blocking).
void reader_start(void (*on_card)(const reader_card_t *card));

// Should this tap be processed? Host-testable tap logic: the same card on
// the same reader within debounce_ms is a still-held card (one event per
// tap), and a card seen on the OTHER reader within cross_read_ms is the
// same physical pass being read twice (prototype's cross-read suppression).
// state must be zeroed before first use.
bool reader_accept(uint8_t *state, size_t state_cap, const reader_card_t *card, uint32_t now_ms,
                   uint32_t debounce_ms, uint32_t cross_read_ms);

#ifdef __cplusplus
}
#endif
