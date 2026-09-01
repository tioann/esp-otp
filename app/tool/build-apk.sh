#!/usr/bin/env bash
# Build the esp-otp Android APK inside the Flutter container.
#
# The android/ platform folder is generated here (kept out of git) so we never
# hand-maintain Gradle; our permission/service overlay is layered on top every
# build. Run via: docker compose run --rm app-build   (see docker-compose.yml).
set -euo pipefail

cd "$(dirname "$0")/.."   # -> app/
MODE="${1:-debug}"        # debug (default) or release

# Flutter shells out to git for versioning; the mounted repo is owned by the
# host user, so mark it safe for the container's root.
git config --global --add safe.directory '*' || true
export PUB_CACHE="${PUB_CACHE:-/tmp/.pub-cache}"

# Scaffold android/ once. We create a throwaway project and copy only its
# android/ so our lib/ and pubspec.yaml are never touched.
if [ ! -d android ]; then
  echo ">> scaffolding android/ ..."
  tmp="$(mktemp -d)"
  flutter create --platforms=android --org com.espotp --project-name esp_otp \
      "$tmp/esp_otp" >/dev/null
  cp -a "$tmp/esp_otp/android" ./android
  rm -rf "$tmp"
fi

# Layer the permission + foreground-service manifest overlay (idempotent).
echo ">> applying android overlay ..."
cp -a android_overlay/. android/

# Pin the NDK to the version the plugins want and that the image pre-installs, so
# Gradle never downloads a different one (the scaffold defaults to an older NDK).
GRADLE_APP=android/app/build.gradle.kts
NDK_VER=27.0.12077973
if grep -q 'ndkVersion' "$GRADLE_APP"; then
  # Newer scaffolds emit `ndkVersion = flutter.ndkVersion` (an older NDK). Force
  # our pinned, pre-installed version so Gradle never downloads a second NDK.
  echo ">> repinning ndkVersion in $GRADLE_APP -> $NDK_VER ..."
  sed -i "s|ndkVersion = .*|ndkVersion = \"$NDK_VER\"|" "$GRADLE_APP"
else
  echo ">> pinning ndkVersion in $GRADLE_APP -> $NDK_VER ..."
  sed -i "0,/android {/s//android {\n    ndkVersion = \"$NDK_VER\"/" "$GRADLE_APP"
fi

echo ">> flutter pub get ..."
flutter pub get

echo ">> flutter test ..."
flutter test || echo "!! tests reported failures (continuing to build)"

echo ">> flutter build apk --$MODE ..."
flutter build apk "--$MODE"

echo ""
echo "APK ready:  app/build/app/outputs/flutter-apk/app-$MODE.apk"
