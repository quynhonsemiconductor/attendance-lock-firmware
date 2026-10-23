cmake_minimum_required(VERSION 3.16)

# ESP-IDF v6.0.x (ADR §2.8 — v6.1 is beta and NOT supported here; the CI pins
# the espressif/idf container by digest). Set IDF_PATH in your shell or pass
# -DIDF_PATH when invoking cmake via idf.py.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(attendance_lock)
