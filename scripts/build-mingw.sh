#!/usr/bin/env bash
# Cross-compile call_assist.exe from Linux with MinGW-w64.
#
#   sudo apt install mingw-w64 cmake
#   scripts/build-mingw.sh [--no-whisper]
#
# Set MINGW_SYSROOT if your toolchain is not under /usr/x86_64-w64-mingw32.
set -euo pipefail
cd "$(dirname "$0")/.."

WHISPER=ON
[[ "${1:-}" == "--no-whisper" ]] && WHISPER=OFF

BUILD=build-mingw
cmake -S . -B "$BUILD" \
      -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
      -DCMAKE_BUILD_TYPE=Release \
      -DCALLASSIST_WITH_WHISPER=$WHISPER \
      ${MINGW_SYSROOT:+-DMINGW_SYSROOT="$MINGW_SYSROOT"}

cmake --build "$BUILD" -j"$(nproc)"
echo
echo "built: $BUILD/call_assist.exe"
