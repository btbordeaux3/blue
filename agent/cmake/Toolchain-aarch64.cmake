# Cross toolchain for an ARM64 target (e.g. the product's handheld SoC).
# Usage:
#   cmake -S agent -B build-arm64 -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-aarch64.cmake
# Note: cross-building monitor.c requires a sysroot with libnm + glib for the
# target. Building ONLY the core (espcore) + tests needs no sysroot:
# the pure-C parts compile cleanly for any arch with a freestanding-ish libc.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CROSS_PREFIX aarch64-linux-gnu)

set(CMAKE_C_COMPILER   ${CROSS_PREFIX}-gcc)
set(CMAKE_C_COMPILER_TARGET aarch64-linux-gnu)

# Point at your BSP sysroot or leave empty to build only the no-dependency
# core by passing -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
set(CMAKE_SYSROOT "$ENV{SYSROOT}" CACHE PATH "Target sysroot")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)