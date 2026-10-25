#pragma once
// Time source (NTP + server Date fallback; §2.4/§2.8), designed for deep
// sleep: the {epoch, uptime-at-set} base lives in RTC memory, which survives
// deep sleep (and the RTC-backed esp_timer keeps counting across it) but not
// power loss — a power cut simply means the next sync session re-syncs.
// Events never carry a mutable timestamp: the reader task buffers the
// monotonic uptime at capture, and device_ts is DERIVED at upload time from
// this base — the ADR's "same-boot rewrite" with no ring rewriting.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Monotonic uptime in microseconds since power-on (continuous across deep
// sleep on the ESP32 — the RTC timer that backs esp_timer keeps running).
int64_t clock_uptime_us(void);

// Mounts the persisted clock state from RTC memory: the current base, the
// previous power-segment's base and the boot id. Returns false only when
// the port cannot even be read (treated as "no clock" — the door still
// works, §2.4).
bool clock_init(void);

// Identifies this power-on segment (random at power-on; unchanged across
// deep sleep AND soft restarts, gone on power loss). Events carry it so
// clock_stamp knows which base applies — a record from before a soft
// restart must derive from that segment's base, not the new one.
uint32_t clock_boot_id(void);

// True once any time source (SNTP or a server Date header) has set the base
// in this power-on cycle. Drives time_synced on events and the no-clock
// validity rule (permanent cards open, windowed cards denied, §2.4).
bool clock_synced(void);

// Server time has landed (epoch seconds, valid "now"): sets the base.
// Call from the sync session — once after SNTP converged, or per response
// when the Date header was parsed and the clock is still unsynced.
void clock_on_time_source(uint64_t epoch_now);

// Derives the device timestamp for an event captured at uptime_us during
// the power-on segment boot_id:
// - current segment and a synced base: epoch = base + (uptime - base_uptime)
// - previous segment (before a soft restart) with a kept base: same, from
//   the kept base — the pre-restart events stay correctly stamped
// - otherwise (no clock, foreign segment): seconds since boot counted from
//   the Unix epoch (a stable, monotonic stand-in), *synced_out = false.
// Never blocks.
void clock_stamp(uint32_t boot_id, int64_t uptime_us, uint64_t *epoch_out, bool *synced_out);

// Parses an HTTP Date header value ("Wed, 23 Oct 2026 08:01:02 GMT", the
// IMF-fixdate format servers must send) into epoch seconds. Host-safe,
// libc-free, rejects garbage (returns false and does not touch the clock).
bool clock_parse_http_date(const char *s, uint64_t *epoch_out);

// Renders epoch seconds as contract device_ts ("2026-10-23T08:01:02Z",
// always UTC, out needs ≥ 21 bytes).
void clock_iso8601(uint64_t epoch, char *out, size_t cap);

// Bounded SNTP session: start before the sync window, stop before the
// radio goes off (no long-lived connections, deep-sleep design). Servers
// come from Kconfig (default time.cloudflare.com).
void clock_sntp_start(void);
void clock_sntp_stop(void);

#ifdef __cplusplus
}
#endif
