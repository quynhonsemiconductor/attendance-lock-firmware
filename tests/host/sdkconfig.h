// Host-build stand-in for the ESP sdkconfig: only the Kconfig symbols the
// host-tested components read. Values mirror the Kconfig defaults; CI's
// sdkconfig asserts cover the device side.
#pragma once

#define CONFIG_SYNC_INTERVAL_MIN 30
