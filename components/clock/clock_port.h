#pragma once
// Platform port for the clock component. clock.c is platform-free and runs
// unchanged in the host tests.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Monotonic microseconds since power-on. On the ESP32 this is esp_timer,
// which the RTC timer (still counting in deep sleep) re-seeds on wake —
// uptime is continuous across a deep-sleep cycle.
int64_t clock_port_uptime_us(void);

// The persisted {epoch, uptime-at-set} base: RTC memory on the ESP32
// (survives deep sleep, gone on power loss — exactly the wanted semantics).
typedef enum {
    CLOCK_PORT_FOUND = 0,
    CLOCK_PORT_ABSENT, // never written or power-lost: normal after a cut
    CLOCK_PORT_ERROR   // backend failure
} clock_port_res_t;

clock_port_res_t clock_port_state_load(void *out, size_t cap, size_t *len);
bool clock_port_state_store(const void *buf, size_t len);

// Wall-clock epoch seconds from the system clock (set by SNTP behind the
// scenes). Used to notice SNTP convergence inside the bounded sync session;
// before any sync it returns something pre-2020 and the caller ignores it.
uint64_t clock_port_now_epoch(void);

// Bounded SNTP session; the ESP port resolves the servers from Kconfig.
void clock_port_sntp_start(void);
void clock_port_sntp_stop(void);
