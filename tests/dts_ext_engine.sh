#!/bin/sh
# External audio (docs/features/external-audio): the real engine in its
# video-only and audio-only roles. Requires the ffmpeg CLI for transient
# fixtures (libx264, ac3, eac3, aac, flac encoders) and the minimal FFmpeg
# libraries built by tools/build-dts-ffmpeg.sh (NUVIO_DTS_HOST=1 CC=gcc).
export TMPDIR="${TMPDIR:-/tmp}"
TMPDIR="${TMPDIR%/}"
set -eu
cd "$(dirname "$0")/.."
PREFIX=${NUVIO_DTS_ROOT:-${TMPDIR:-/tmp}/nuvio-dts-host}
TMP=$(mktemp -d ${TMPDIR:-/tmp}/nuvio-ext-test.XXXXXXXX)
trap 'rm -rf "$TMP"' EXIT
${CC:-gcc} -std=gnu11 -Wall -Wextra -Werror -DNV_DTS_FFMPEG ${NV_TEST_DTS_RECOVERY_CFLAGS--DNV_DTS_WEBOS_RECOVERY} -I"$PREFIX/include" \
  src/dts/dts_engine.c tests/dts_ext_engine.c -o "$TMP/engine" \
  -L"$PREFIX/lib" -Wl,--start-group -lavformat -lavcodec -lswresample -lavutil -Wl,--end-group -lm -lpthread
TONE='aevalsrc=0.1*sin(2*PI*300*t)|0.1*sin(2*PI*500*t)|0.1*sin(2*PI*700*t)|0.1*sin(2*PI*100*t)|0.1*sin(2*PI*900*t)|0.1*sin(2*PI*1100*t):s=48000:d=4:c=5.1'
printf '1\n00:00:00,500 --> 00:00:01,500\nExternal audio fixture\n' > "$TMP/input.srt"
# The movie: video plus an AC-3 track, as a non-DV session cannot feed.
ffmpeg -hide_banner -loglevel error \
  -f lavfi -i 'color=size=64x64:rate=25:duration=4' -f lavfi -i "$TONE" \
  -map 0:v -map 1:a -c:v libx264 -bf 0 -g 25 -c:a ac3 -b:a 384k \
  -metadata:s:a:0 language=eng "$TMP/movie.mkv"
# The other source: its own video, E-AC-3, AC-3, stereo AAC-LC and a subtitle.
ffmpeg -hide_banner -loglevel error \
  -f lavfi -i 'color=size=64x64:rate=25:duration=4' -f lavfi -i "$TONE" -i "$TMP/input.srt" \
  -map 0:v -map 1:a -map 1:a -map 1:a -map 2:s -c:s srt -c:v libx264 -bf 0 -g 25 \
  -c:a:0 eac3 -b:a:0 384k -c:a:1 ac3 -b:a:1 384k -c:a:2 aac -ac:a:2 2 -b:a:2 128k \
  -metadata:s:a:0 language=eng -metadata:s:a:1 language=por -metadata:s:a:2 language=por "$TMP/dub.mkv"
# A codec the minimal build cannot decode and the pipeline cannot declare.
ffmpeg -hide_banner -loglevel error \
  -f lavfi -i 'color=size=64x64:rate=25:duration=4' -f lavfi -i "$TONE" \
  -map 0:v -map 1:a -c:v libx264 -bf 0 -g 25 -c:a flac "$TMP/flac.mkv"
"$TMP/engine" "$TMP/movie.mkv" "$TMP/dub.mkv" "$TMP" "$TMP/flac.mkv"
# Passthrough is exact: the delivered payload is the track's own bytes. The
# existing passthrough rule trims a packet that ends at or before the start
# target, and one whose duration the container does not give counts as such at
# time zero; so at most the first packet may be missing, never a later byte.
for track in 0:eac3 1:ac3 2:aac; do
  ffmpeg -hide_banner -loglevel error -i "$TMP/dub.mkv" -map "0:a:${track%%:*}" -c copy -f data "$TMP/${track##*:}.expected"
done
python3 - "$TMP" <<'PY'
import sys
for name in ('eac3','ac3','aac'):
    expected=open(f'{sys.argv[1]}/{name}.expected','rb').read()
    got=open(f'{sys.argv[1]}/{name}.bin','rb').read()
    missing=len(expected)-len(got)
    assert got and expected.endswith(got) and 0<=missing<=2048,(name,len(expected),len(got))
    print(f'{name}: {len(got)} bytes identical to the track, {missing} trimmed at the start')
PY
echo 'External audio: exact passthrough of E-AC-3, AC-3 and AAC-LC from the other source passed.'
