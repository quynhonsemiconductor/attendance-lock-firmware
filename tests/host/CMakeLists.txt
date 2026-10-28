# Host unit tests — build on linux/macOS with plain cmake + gcc, no ESP-IDF.
# Unity is pinned to a release tarball so CI and laptops compile byte-identical.
# Device-only code needs the IDF build; everything testable on the host
# (parsers, ring math, seq/cursor logic) lives here (work item 7 grows this).
cmake_minimum_required(VERSION 3.16)
project(alf_host_tests C)

include(FetchContent)
FetchContent_Declare(unity
  URL https://github.com/ThrowTheSwitch/Unity/archive/refs/tags/v2.6.0.tar.gz
  URL_HASH SHA256=aa4c9fb1ae5fc5242f914c65f3557e817e40cb37f04a31e5ff76d1ab89dbf674)
FetchContent_MakeAvailable(unity)

# store: the component is platform-free; the host port (RAM flash + NVS) is
# linked in place of store_port_esp.c.
set(STORE_SOURCES
  ../../components/store/store.c
  ../../components/store/store_port_map.c
  ../../components/store/store_port_host.c)
include_directories(../../components/store) # store_port.h / store_port_host.h

set(CLOCK_SOURCES
  ../../components/clock/clock.c
  ../../components/clock/clock_port_host.c)
include_directories(../../components/clock) # clock_port.h / clock_port_host.h
include_directories(${CMAKE_CURRENT_SOURCE_DIR}) # host sdkconfig.h stand-in

add_executable(host_tests_ring
  test_store_ring.c
  ${STORE_SOURCES})
target_include_directories(host_tests_ring PRIVATE ../../components/store/include)
target_link_libraries(host_tests_ring PRIVATE unity)

add_executable(host_tests_alist
  test_store_alist.c
  ${STORE_SOURCES})
target_include_directories(host_tests_alist PRIVATE ../../components/store/include)
target_link_libraries(host_tests_alist PRIVATE unity)

add_executable(host_tests_clock
  test_clock.c
  ${CLOCK_SOURCES})
target_include_directories(host_tests_clock PRIVATE ../../components/clock/include)
target_link_libraries(host_tests_clock PRIVATE unity)

set(SYNC_SOURCES
  ../../components/sync/sync.c
  ../../components/sync/sync_json.c
  ../../components/sync/sync_port_host.c
  ${STORE_SOURCES}
  ${CLOCK_SOURCES})
include_directories(../../components/sync) # sync_port.h / sync_port_host.h

add_executable(host_tests_sync
  test_sync.c
  ${SYNC_SOURCES})
target_include_directories(host_tests_sync PRIVATE
  ../../components/sync/include
  ../../components/store/include
  ../../components/clock/include)
target_compile_definitions(host_tests_sync PRIVATE
  FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/contract-v1"
  SYNC_HOST_BUILD)
target_link_libraries(host_tests_sync PRIVATE unity)

enable_testing()
add_test(NAME store_ring COMMAND host_tests_ring)
add_test(NAME store_alist COMMAND host_tests_alist)
add_test(NAME clock COMMAND host_tests_clock)
add_test(NAME sync COMMAND host_tests_sync)
add_test(NAME reader COMMAND host_tests_reader)
add_test(NAME lock COMMAND host_tests_lock)
add_test(NAME power COMMAND host_tests_power)
add_test(NAME door COMMAND host_tests_door)

# reader: host-safe tap logic
add_executable(host_tests_reader
  test_reader_logic.c
  ../../components/reader/reader_logic.c)
target_include_directories(host_tests_reader PRIVATE ../../components/reader/include)
target_link_libraries(host_tests_reader PRIVATE unity)

# lock: host-safe servo math
add_executable(host_tests_lock
  test_lock.c
  ../../components/lock/lock_math.c)
target_include_directories(host_tests_lock PRIVATE ../../components/lock/include)
target_link_libraries(host_tests_lock PRIVATE unity)

# door_fsm: the door state machine + decision
add_executable(host_tests_door
  test_door_fsm.c
  ../../components/door_fsm/door_fsm.c
  ${STORE_SOURCES})
target_include_directories(host_tests_door PRIVATE
  ../../components/door_fsm/include
  ../../components/store/include)
target_link_libraries(host_tests_door PRIVATE unity)

# power: host-safe battery math
add_executable(host_tests_power
  test_power.c
  ../../components/power/power.c)
target_include_directories(host_tests_power PRIVATE ../../components/power/include)
target_link_libraries(host_tests_power PRIVATE unity)
