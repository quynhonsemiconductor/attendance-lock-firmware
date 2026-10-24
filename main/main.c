// app_main — scaffold: boots the components and stays locked (fail-secure,
// ADR §2.8). Firmware v1 wires the real loop; the component stubs below are
// already the call shape it will keep.
#include "clock/clock.h"
#include "lock/lock.h"
#include "provision/provision.h"
#include "reader/reader.h"
#include "store/store.h"
#include "sync/sync.h"

void app_main(void)
{
    // Fail-secure order: the lock comes up FIRST, before anything that could
    // block or crash. A hung boot or a watchdog reset lands here again —
    // locked (§2.8: boots locked).
    lock_init();
    lock_engage();

    provision_init();
    // Mount the event ring and the allow-list cache before anything reads
    // them; on failure the door still works (locked) and sync re-syncs.
    store_init();
    clock_init();
    reader_init();
    sync_init();

    // Scaffold: nothing else runs yet. Firmware v1 (work item 7) adds the
    // poll/buffer/upload tasks and the serial provisioning console loop.
    // No busy loop: app_main returns and the idle tasks feed the watchdog.
}
