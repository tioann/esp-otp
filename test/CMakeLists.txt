# Standalone test application: runs the components' Unity test cases on-target
# under QEMU (no hardware, no display needed). Build/run via scripts/test-qemu.sh.
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")

# Scope the build to the test runner and its dependency closure (unity, totp,
# protocol and what those pull in). This deliberately excludes blecon/NimBLE:
# the suites cover pure logic (TOTP, base32, protocol framing), and the BLE
# stack now unconditionally needs a controller, which QEMU's test target lacks.
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp-otp-tests)
