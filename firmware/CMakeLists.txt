# Top-level ESP-IDF project file for Klingelbox — a local, cloud-free
# 433 MHz doorbell server on the ESP32-S3. "Die Klingel, lokal und ohne Cloud."
#
# Build with the official toolchain (note: the installed IDF python env is 3.12,
# while the system python3 may be newer — pin it before sourcing export.sh):
#   export IDF_PYTHON_ENV_PATH=~/.espressif/python_env/idf5.3_py3.12_env
#   source ~/esp/esp-idf/export.sh
#   idf.py set-target esp32s3
#   idf.py build flash monitor
#
# The radio layers live in components/ on purpose: `cc1101` (chip driver) and
# `rfpulse` (protocol-agnostic pulse capture/replay + decoder plugins) know
# nothing about doorbells and are reusable outside this project.
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

# Single source of truth for the version, stamped into
# esp_app_get_description()->version and reported by GET /api/system.
set(PROJECT_VER "0.2.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(klingelbox)
