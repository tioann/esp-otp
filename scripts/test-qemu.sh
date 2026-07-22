#!/usr/bin/env bash
# Build and run the on-target Unity tests in QEMU (no hardware). Exits non-zero
# if any test fails, so it works in CI.
#
#   scripts/test-qemu.sh
set -euo pipefail

IMAGE=esp-otp-builder
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  docker build -t "$IMAGE" "$ROOT"
fi

out=$(docker run --rm -v "$ROOT":/project -w /project/test "$IMAGE" bash -lc '
  idf.py build >/dev/null 2>&1 || { echo BUILD_FAILED; exit 1; }
  timeout 90 idf.py qemu --qemu-extra-args "-no-reboot" 2>&1 || true
')

echo "$out" | sed -n '/Running .*\.\.\./,/Tests .*Failures/p'

# Score the run: require the summary line with zero failures.
summary=$(echo "$out" | grep -E "[0-9]+ Tests [0-9]+ Failures" | tail -1 || true)
echo "----"
echo "summary: ${summary:-<none>}"
if echo "$out" | grep -q "ESP_OTP_TESTS_DONE" && echo "$summary" | grep -qE " 0 Failures "; then
  echo "QEMU tests PASSED"
  exit 0
fi
echo "QEMU tests FAILED"
exit 1
