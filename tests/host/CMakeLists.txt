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
  ../../components/store/store_port_host.c)
include_directories(../../components/store) # store_port.h / store_port_host.h

set(CLOCK_SOURCES
  ../../components/clock/clock.c
  ../../components/clock/clock_port_host.c)
include_directories(../../components/clock) # clock_port.h / clock_port_host.h

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

enable_testing()
add_test(NAME store_ring COMMAND host_tests_ring)
add_test(NAME store_alist COMMAND host_tests_alist)
add_test(NAME clock COMMAND host_tests_clock)
