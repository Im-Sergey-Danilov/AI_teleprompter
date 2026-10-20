#!/bin/sh
# Host-side tests for the portable core (JSON, resampler, VAD, WAV).
# Runs on Linux or macOS with any C compiler - no Windows headers involved.
#
# The sources are staged into a temp directory alongside a Win32-free util.h
# shim, because #include "util.h" always resolves against the including file's
# own directory first.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-cc}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

cp src/json.c src/json.h src/resample.c src/resample.h \
   src/vad.c src/vad.h src/wav.c src/wav.h \
   src/dotenv.c src/dotenv.h "$OUT/"
cp tests/shim/util.h tests/util_stub.c tests/test_core.c "$OUT/"

$CC -std=c11 -O1 -g -Wall -Wextra -Wno-unused-parameter \
    "$OUT/test_core.c" "$OUT/util_stub.c" \
    "$OUT/json.c" "$OUT/resample.c" "$OUT/vad.c" "$OUT/wav.c" \
    "$OUT/dotenv.c" \
    -I"$OUT" -lm -o "$OUT/test_core"
"$OUT/test_core"
