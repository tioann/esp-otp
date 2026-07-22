#!/usr/bin/env bash
# Build (and flash/monitor) esp-otp inside the pinned ESP-IDF container.
#
# Examples:
#   scripts/build.sh                 # idf.py build
#   scripts/build.sh set-target esp32c3 build
#   scripts/build.sh menuconfig
#   scripts/build.sh fullclean
#   scripts/build.sh -p /dev/ttyACM0 flash monitor   # needs the serial port
#
# When a port is passed with `-p <port>`, it is mounted into the container so
# flash/monitor can reach the device; a TTY is allocated when the shell is
# interactive so `monitor` works.
set -euo pipefail

IMAGE=esp-otp-builder
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Build the image once (cached afterwards).
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  docker build -t "$IMAGE" "$ROOT"
fi

# Default action is a plain build.
if [ "$#" -eq 0 ]; then
  set -- build
fi

# Pull the serial port out of the args (idf.py's `-p <port>` / `--port <port>`)
# so we can hand that device to the container.
PORT=""
args=("$@")
for ((i = 0; i < ${#args[@]}; i++)); do
  case "${args[i]}" in
    -p|--port) PORT="${args[i+1]:-}" ;;
    -p=*|--port=*) PORT="${args[i]#*=}" ;;
  esac
done

DOCKER_ARGS=(--rm)
# Allocate a TTY only in an interactive shell (so CI / piped use still works).
if [ -t 0 ] && [ -t 1 ]; then
  DOCKER_ARGS+=(-it)
fi
if [ -n "$PORT" ]; then
  if [ -e "$PORT" ]; then
    # The ESP32-C3's native USB-Serial/JTAG re-enumerates on every reset, so a
    # single `--device "$PORT"` mapping goes stale mid-flash (and the node can
    # even change, ttyACM0 -> ttyACM1). Give the container live access to /dev
    # so it always sees the current node.
    DOCKER_ARGS+=(--privileged -v /dev:/dev)
    # ModemManager probes ttyACM* the moment they appear and will grab the port
    # mid-flash, dropping the connection. Warn if it's running.
    if command -v systemctl >/dev/null 2>&1 \
       && [ "$(systemctl is-active ModemManager 2>/dev/null)" = active ]; then
      echo "build.sh: warning: ModemManager is active and may grab '$PORT'" \
           "during flash. If flashing drops mid-write, run:" >&2
      echo "  sudo systemctl stop ModemManager" >&2
    fi
  else
    echo "build.sh: warning: serial port '$PORT' not found on host" >&2
  fi
fi

exec docker run "${DOCKER_ARGS[@]}" -v "$ROOT":/project -w /project "$IMAGE" idf.py "$@"
