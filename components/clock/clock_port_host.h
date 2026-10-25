#pragma once
// Test-only hooks for the clock host port. Never included by clock.c or the
// firmware build.

#include <stdint.h>

// Simulate a power cut: the persisted base is gone (RTC memory resets).
void clock_host_power_loss(void);

// Set the "system clock" the port reports (SNTP convergence detection).
void clock_host_set_epoch(uint64_t epoch);

// Pin the monotonic uptime the port reports (INT64_MIN = use real time).
void clock_host_set_uptime_us(int64_t us);
