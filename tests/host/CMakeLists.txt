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

add_executable(host_tests
  test_sample.c
  # under test: component sources that are host-safe are added here directly
  ../../components/store/store.c)
target_include_directories(host_tests PRIVATE ../../components/store/include)
target_link_libraries(host_tests PRIVATE unity)

enable_testing()
add_test(NAME sample COMMAND host_tests)
