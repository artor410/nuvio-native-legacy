#!/usr/bin/env bash
# External audio (docs/features/external-audio): the real playback worker
# against two fake engines and a fake native pipeline. Needs only a C compiler.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
flags=()
if [[ ${SANITIZE:-0} == 1 ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
cc "${flags[@]}" -std=c11 -D_GNU_SOURCE -DNV_DTS_WEBOS_RECOVERY -Wall -Wextra -Werror -Isrc \
  tests/dts_ext_audio.c src/dts/dts_playback.c src/js.c src/linguas.c -pthread -lm -o "$tmp/test"
for scenario in fromb offset advance clamp seek openfail midfail discontinuity refused toggle trackpick language explicit beof aeof; do
 "$tmp/test" "$scenario"
done
