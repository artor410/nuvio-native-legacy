# External audio on webOS

Play the video of one source (A, for example 4K Dolby Vision with English audio
only) with one audio track of another source (B, for example a 1080p dub). The
video is never re-encoded: A's compressed video and B's audio are fed as
separate streams into LG's hardware pipeline, the same path as the
[DTS fallback and Dolby Vision in MKV](../dts/README.md).

## Status

| Layer | State |
|---|---|
| Engine (`src/dts/dts_engine.*`): video-only and audio-only roles, per-engine read-ahead | Done, host-tested |
| Playback worker (`src/dts/dts_playback.*`): second engine, offset, seek, fallback | Done, host-tested |
| App wiring (`src/video.c`), audio sheet row (`src/faixas.c`), on/off flag | **Not written yet** |
| Behavior on a TV | **Not validated** |

Nothing calls the new entry points yet, so the feature is off and existing
sessions are unchanged: `dts_playback_start` is `dts_playback_start_ext` with
no external audio, and an engine nobody configures keeps the default role and
read-ahead. See [PLANO.md](PLANO.md) for the plan and the remaining steps.

## Source requirements

- **A (video)**: unencrypted MKV or MP4 film/episode, H.264/HEVC/VP9/AV1 as the
  TV decodes it. For Dolby Vision: single-layer HEVC profile 5 or 8 (profile 7
  only with the existing experimental option). A does not need a feedable audio
  track while external audio is active, but needs one for the fallback below.
- **B (audio)**: unencrypted MKV or MP4 with the wanted track as:

| Track in B | What happens |
|---|---|
| AC-3, E-AC-3 | Passed through untouched (preferred) |
| AAC-LC, stereo, 48 kHz | Passed through untouched |
| DTS | Existing conversion: AC3 5.1 with `dts_engine_set_ac3(1)`, else stereo AAC |
| Anything else (TrueHD, 5.1 AAC, HE-AAC, Opus, FLAC...) | Refused; the session uses A's audio |

The last row is a limit of the webOS FFmpeg build (`dca` is its only audio
decoder) and of what the Starfish adapter declares to the TV. Adding decoders
means rebuilding the SDK image and is not part of this change.

The track of B is an exact stream index, or the first audio track matching a
language, or the first audio track.

## Offset

`pts_out = pts_B + offset_ms`: a positive offset delays the audio. A seek to T
puts A at T and B at `T - offset`, never below 0. The range is ±30 s
(`DTS_EXT_OFFSET_MAX_MS`). Changing the offset recreates the native pipeline at
the current position, like a seek.

## When B fails

Failing to open, a read error, a timestamp jump in B (more than 0.5 s back or
5 s forward), B too slow for two 5 s windows, a CPU overrun while converting B,
or the native Load refusing B's audio format: the worker reloads at the
presented position with A's own audio. The session does not fail;
`DtsPlaybackStatus.ext_fallback` and `ext_error` say what happened (no URL or
header is ever in that text). If A has no track the path can feed (TrueHD only,
for instance), the session then fails as any DTS/DV session does today and the
native player takes over.

B ending before A is not a failure: the video goes on without audio.

## Limits

- **Constant offset only.** Different cuts, or 23.976 vs 25 fps masters, drift;
  there is no speed correction and no automatic offset.
- **B is downloaded whole**, video included: containers interleave, so the
  audio cannot be fetched alone. B's bitrate adds to A's.
- **Memory**: B reads 1 MiB ranges with 6 of read-ahead (about 7 MiB of
  payload) regardless of Settings > Buffer size, which keeps applying to A
  only. Two demuxers and two sets of four HTTP workers run during the session.
- **Stereo** when B's DTS falls to AAC.
- A read of B that hangs holds the single worker until the HTTP layer gives
  up; the six seconds of lead cover short stalls only.
- Subtitles come from A. B's subtitles and video are never delivered.
- Live TV, DRM and webOS 1/2 are excluded, as on the DTS path.

## API

```c
DtsExtAudio ext = { .url = b_url, .headers = b_headers, .language = "pt",
                    .stream_index = -1, .offset_ms = 0 };
p = dts_playback_start_ext(a_url, a_headers, ..., &ext);  /* from the start */
dts_playback_ext_audio(p, &ext);   /* in session; NULL returns to A's audio */
dts_playback_ext_offset(p, 150);   /* audio 150 ms later */
```

`dts_playback_audio` (one of A's own tracks) also leaves external audio.

## Tests

```sh
bash tests/dts_ext_audio.sh     # worker, fake engines; needs only cc
CC=gcc NUVIO_DTS_HOST=1 NUVIO_DTS_ROOT=/tmp/nuvio-dts-host \
  NUVIO_DTS_BUILD=/tmp/nuvio-dts-host-build sh tools/build-dts-ffmpeg.sh
bash tests/dts_ext_engine.sh    # real engine, synthetic MKVs; needs the ffmpeg CLI
```

`dts_ext_audio` checks, against the real worker: the fed audio is B's and A's
own is never fed; timestamps carry the offset (delay, advance, clamp at zero);
a seek repositions both sources with one reload; B failing at open, mid-stream,
on a timestamp jump or at the native Load returns to A's audio without failing
the session; on/off and offset changes in session; track choice in B; B or A
ending first.

`dts_ext_engine` checks, against the real demuxer: A opens video-only with no
feedable audio track and delivers only video; B opens audio-only and delivers
only the chosen track, byte-identical to the file for E-AC-3, AC-3 and AAC-LC;
seek lands on the packet containing the target; B's read-ahead is its own; an
undecodable codec is refused.

### What was actually run

Both fixtures were run on Windows with MSVC and a local pthread shim, the
engine one against FFmpeg 7 shared libraries, because the development machine
had no Linux toolchain. All scenarios passed, as did the 52 existing
`tests/dts_playback.c` scenarios. **The `.sh` scripts themselves, the
`gcc -Wall -Wextra -Werror` build, the sanitizer runs and the pinned static
FFmpeg 7.1.5 were not run**, and neither was the "undecodable codec" case (it
depends on the minimal build). The CI workflow lists both scripts.

Host tests do not establish playback on any TV. Whether the Starfish pipeline
accepts audio whose timeline comes from another file, how A/V sync holds over a
film, and the cost of a second download on a 1-2 GB TV can only be measured on
the device.

## Testing on a TV

Not possible until the app wiring exists. Once it does: build and install as in
the [main build recipe](../../../README.md#building) (`tools/arm.sh`, or
`ares-install` / Homebrew Channel with the built `.ipk`), play A, choose B from
the audio sheet, then check start, seek, pause, offset steps, a deliberately
dead B link, and sync at the start and end of a film. Reports should include
model, firmware, the codecs of both sources and the `[ext-audio]` / `[dts]`
log lines.
