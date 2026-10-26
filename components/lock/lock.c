// Backend-neutral lock commands. The GPIO/LEDC wiring lives in lock_esp.c,
// the pure pulse math in lock_math.c (host-tested).
#include "lock/lock.h"
#include "lock/lock_esp.h"
#include "lock/lock_math.h"

#include "sdkconfig.h"

static uint32_t clamp_pulse(uint32_t us)
{
    return us; // the Kconfig values are the calibration (bench-set); no
               // silent clamping here — the servo driver clamps to the
               // LEDC range, the positions themselves are trusted config
}

void lock_init(void)
{
    lock_esp_init();
    lock_engage(); // fail-secure boot state (§2.8)
}

void lock_engage(void)
{
#if CONFIG_LOCK_BACKEND_SERVO
    lock_esp_servo_write(clamp_pulse(CONFIG_LOCK_SERVO_LOCK_US));
#if CONFIG_LOCK_SERVO_RELEASE
    lock_esp_servo_release_after(CONFIG_LOCK_SERVO_MOVE_MS);
#endif
#elif CONFIG_LOCK_BACKEND_RELAY
    lock_esp_relay_write(0);
#else
#error "pick a lock backend"
#endif
}

void lock_release(void)
{
#if CONFIG_LOCK_BACKEND_SERVO
    lock_esp_servo_write(clamp_pulse(CONFIG_LOCK_SERVO_UNLOCK_US));
#if CONFIG_LOCK_SERVO_RELEASE
    lock_esp_servo_release_after(CONFIG_LOCK_SERVO_MOVE_MS);
#endif
#elif CONFIG_LOCK_BACKEND_RELAY
    lock_esp_relay_write(1);
#else
#error "pick a lock backend"
#endif
}
