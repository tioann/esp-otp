# Reproducible ESP-IDF build environment for esp-otp (ESP32-C3).
#
# The official Espressif image already contains the IDF toolchain, the
# RISC-V cross compiler and a matching Python venv. We pin a release so
# builds are reproducible; bump this tag deliberately.
#
# Usage (see scripts/build.sh and docker-compose.yml):
#   docker build -t esp-otp-builder .
#   docker run --rm -v "$PWD":/project esp-otp-builder idf.py build
FROM espressif/idf:release-v5.5

# Build for the C3 by default; overridable on the command line.
ENV IDF_TARGET=esp32c3

WORKDIR /project

# The base image's entrypoint sources export.sh so that idf.py, cmake,
# ninja and the toolchain are all on PATH for any command we pass.
