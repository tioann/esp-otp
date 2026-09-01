# Reproducible Flutter/Android build environment for the esp-otp companion app.
#
# The Cirrus Labs image bundles the Flutter SDK, the Android SDK/NDK and an
# accepted licence set. We pin a release so APKs are reproducible; bump the tag
# deliberately.
#
# Usage (see docker-compose.yml at the repo root):
#   docker compose run --rm app-build            # build a debug APK
#   docker compose run --rm app-build release    # build a release APK
FROM ghcr.io/cirruslabs/flutter:3.29.3

# Keep pub's cache inside the container (the project dir is a mounted volume).
ENV PUB_CACHE=/tmp/.pub-cache

# Pre-install the Android SDK components the build needs so `docker compose run`
# does no network fetches at build time (reproducible, offline-friendly). The
# NDK is required by a plugin's Gradle config even though we ship no native code.
RUN yes | sdkmanager --licenses >/dev/null 2>&1 || true \
 && sdkmanager \
      "platform-tools" \
      "platforms;android-34" \
      "build-tools;34.0.0" \
      "ndk;27.0.12077973" \
      "cmake;3.22.1" >/dev/null

WORKDIR /project/app

ENTRYPOINT ["bash", "tool/build-apk.sh"]
