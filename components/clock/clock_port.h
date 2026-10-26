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

// The persisted clock state — ONE definition shared by clock.c (logic) and
// the ESP port (RTC layout). Field order/layout matters for the RTC blob.
typedef struct {
    uint32_t magic;        // BASE_MAGIC
    uint32_t boot_magic;   // BOOT_MAGIC (valid boot id inside)
    uint32_t cur_boot_id;  // this power segment's id
    uint8_t cur_valid;     // current segment has a synced base
    uint8_t prev_valid;    // previous segment's base kept across a soft restart
    uint32_t prev_boot_id; // the segment that ran before the restart
    uint64_t cur_epoch;
    int64_t cur_uptime_us;
    uint64_t prev_epoch;
    int64_t prev_uptime_us;
} clock_state_t;

// Logged when persisting the state fails (device: ESP_LOGE; host: silent).
void clock_port_log(const char *msg);

// Tri-state: a missing state is a normal state (power-on); a backend
// failure is not.
typedef enum {
    CLOCK_PORT_FOUND = 0,
    CLOCK_PORT_ABSENT,
    CLOCK_PORT_ERROR
} clock_port_res_t;

// The persisted clock state in RTC memory: survives deep sleep AND soft
// restarts (panic/watchdog/esp_restart), gone on power loss. Whether the
// state is USABLE depends on the wake kind (see below) — the port hands out
// the raw blob and the caller decides.
clock_port_res_t clock_port_state_load(void *out, size_t cap, size_t *len);
bool clock_port_state_store(const void *buf, size_t len);

// True only when the device resumed from deep sleep (the RTC-backed
// esp_timer kept counting, so stored uptime deltas stay valid). False after
// power-on and after panic/watchdog/esp_restart — there esp_timer restarted
// at 0 and every stored uptime delta is stale.
bool clock_port_deep_sleep_wake(void);

// Wall-clock epoch seconds from the system clock (set by SNTP behind the
// scenes). Used to notice SNTP convergence inside the bounded sync session;
// before any sync it returns something pre-2020 and the caller ignores it.
uint64_t clock_port_now_epoch(void);

// Random bytes (boot-id generation at power-on).
void clock_port_random_bytes(void *out, size_t len);

// Bounded SNTP session; the ESP port resolves the servers from Kconfig.
void clock_port_sntp_start(void);
void clock_port_sntp_stop(void);
