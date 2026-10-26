// Host port: RAM-persisted base, controllable uptime, injectable wall clock,
// no-op SNTP. Test helpers in clock_port_host.h.
#include "clock_port_host.h"

#include "clock_port.h"

#include <string.h>
#include <time.h>

static struct {
    bool valid;           // the persisted state is physically present
    bool deep_sleep_wake; // true = resumed from deep sleep (uptime continued)
    uint8_t blob[sizeof(clock_state_t)];
    size_t len;
} s_state;

static uint64_t injected_epoch = 0;    // "system clock" the tests control
static uint64_t rng_state = 0xDEADBEEFu; // separate: random must not touch the clock
int64_t clock_host_override_us = INT64_MIN; // "uptime" the tests control

void clock_port_log(const char *msg)
{
    (void)msg;
}

int64_t clock_port_uptime_us(void)
{
    if (clock_host_override_us != INT64_MIN) {
        return clock_host_override_us;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

clock_port_res_t clock_port_state_load(void *out, size_t cap, size_t *len)
{
    if (!s_state.valid) {
        return CLOCK_PORT_ABSENT; // never written = power-on state
    }
    if (s_state.len > cap) {
        return CLOCK_PORT_ERROR;
    }
    memcpy(out, s_state.blob, s_state.len);
    *len = s_state.len;
    return CLOCK_PORT_FOUND;
}

bool clock_port_deep_sleep_wake(void)
{
    return s_state.valid && s_state.deep_sleep_wake;
}

bool clock_port_state_store(const void *buf, size_t len)
{
    if (len > sizeof(s_state.blob)) {
        return false;
    }
    memcpy(s_state.blob, buf, len);
    s_state.len = len;
    s_state.valid = true;
    return true;
}

uint64_t clock_port_now_epoch(void)
{
    return injected_epoch;
}

void clock_port_random_bytes(void *out, size_t len)
{
    uint8_t *q = out;
    for (size_t i = 0; i < len; i++) {
        rng_state ^= rng_state << 13;
        rng_state ^= rng_state >> 7;
        rng_state ^= rng_state << 17;
        q[i] = (uint8_t)(rng_state >> 24);
    }
}

void clock_port_sntp_start(void)
{
}

void clock_port_sntp_stop(void)
{
}

// --- Test helpers ---

void clock_host_power_loss(void)
{
    memset(&s_state, 0, sizeof(s_state));
}

void clock_host_reboot_after_deep_sleep(void)
{
    s_state.valid = true; // blob already there; nothing re-stores it
    s_state.deep_sleep_wake = true;
}

void clock_host_soft_restart(void)
{
    s_state.valid = true;
    s_state.deep_sleep_wake = false;
}

void clock_host_set_epoch(uint64_t epoch)
{
    injected_epoch = epoch;
}

void clock_host_set_uptime_us(int64_t us)
{
    clock_host_override_us = us;
}
