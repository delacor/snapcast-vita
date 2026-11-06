#!/bin/sh
set -eu
cd "$(dirname "$0")"
: "${VITASDK:?Set VITASDK to the installed Vita SDK}"
test_binary=$(mktemp /tmp/snapcast-vita-track-gap.XXXXXX)
trap 'rm -f "$test_binary"' EXIT HUP INT TERM
# SDK headers provide platform declarations; the test supplies clock/audio
# stubs. -idirafter keeps the host C library ahead of the SDK's ARM C library.
for test_source in track_gap.c config_storage.c; do
PATH="/usr/bin:/bin:$PATH" "${CC:-/usr/bin/cc}" -std=gnu11 -O1 -g -ffunction-sections -fdata-sections \
    -DSNAPVITA_FILE_LOGGING="${SNAPVITA_TEST_FILE_LOGGING:-0}" \
    -idirafter "$VITASDK/arm-vita-eabi/include" \
    -fsanitize=undefined -fsanitize-undefined-trap-on-error \
    "$test_source" ../src/time_sync.c ../src/json.c -Wl,--gc-sections -pthread -lm -o "$test_binary"
"$test_binary"
done
