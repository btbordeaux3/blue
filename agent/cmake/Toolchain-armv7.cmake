# Cross toolchain for an ARMv7 (32-bit) target.
# Usage:
#   cmake -S agent -B build-armhf \
#       -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-armv7.cmake

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CROSS_PREFIX arm-linux-gnueabihf)

set(CMAKE_C_COMPILER   ${CROSS_PREFIX}-gcc)
set(CMAKE_C_COMPILER_TARGET arm-linux-gnueabihf)

set(CMAKE_SYSROOT "$ENV{SYSROOT}" CACHE PATH "Target sysroot")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)