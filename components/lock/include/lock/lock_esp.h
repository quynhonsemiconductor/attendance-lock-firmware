#pragma once
// Device-side wiring (declared here so lock.c stays backend-agnostic).
#include <stdint.h>

void lock_esp_init(void);
void lock_esp_relay_write(int level);
void lock_esp_servo_write(uint32_t pulse_us);
void lock_esp_servo_release_after(uint32_t move_ms);
