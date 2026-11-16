# External audio (webOS) — plan

Video of source A with one audio track of source B, without re-encoding the
video, through the same hardware path as the DTS fallback / Dolby Vision in MKV.

## What the code says (checked before planning)

- The session is started in `src/video.c` (`iniciarDts`), not in `player.c`.
- The audio selector is `src/faixas.c`. `src/audioescolha.c` only holds the
  automatic-choice rule (no network, no screen); the menu entry goes in
  `faixas.c`.
- `dts_engine_open` requires video AND a feedable audio track in the same file.
  Two sources need two new engine roles: video-only (A) and audio-only (B).
- The Starfish adapter (`src/dts/adapter/starfish.cpp`) only accepts in Load:
  AC-3 / E-AC-3 (1 to 8 channels) or **stereo 48 kHz** AAC (LC, raw).
- The webOS FFmpeg build (`tools/build-dts-ffmpeg.sh`) only has the `dca`
  decoder. TrueHD, 5.1 AAC, Opus etc. in B cannot be converted without changing
  the SDK. Out of the MVP (see "Decisions").
- `dts_engine_set_ahead_mib` is global. B's engine gets a setting of its own.

## Files

| File | Change |
|---|---|
| `src/dts/dts_engine.h/.c` | `dts_engine_role` (AV / video-only / audio-only) and per-engine `dts_engine_ahead_mib`. Defaults unchanged. |
| `src/dts/dts_playback.h/.c` | `DtsExtAudio`, `dts_playback_start_ext`, `dts_playback_ext_audio`, `dts_playback_ext_offset`; second engine on the single worker; offset; seek of both sources; fallback to A's audio; state in `DtsPlaybackStatus`. |
| `src/video.h/.c` | `video_ext_audio*`: on/off, offset, fallback notice. |
| `src/faixas.c` | "Audio from another source..." row in the audio sheet, list of the other resolved sources, offset row (50 ms steps). |
| Flag | `/tmp/nuvio-ext-audio` or `NUVIO_EXT_AUDIO=1`, read in `video.c`. A Settings option modeled on `AJ_DV7` was the first idea; it touches the sync tables, four settings tests and 30 language files, so it waits for the path to be proven on a TV. |
| `.github/workflows/external-audio.yml` | Runs the host tests of this branch. |
| `tests/dts_ext_audio.c/.sh` | Real worker, fake engines: audio comes from B, offset, seek, fallback. |
| `tests/dts_ext_engine.c/.sh` | Real engine with small synthetic MKVs (ffmpeg): roles, AC-3 passthrough, seek, unsupported codec. |
| `docs/features/external-audio/README.md` | Requirements, limits, how to test on the TV. |

## Decisions

1. **Offset convention**: `pts_out = pts_B + offset_ms`. Positive delays the
   audio. Seek to T: A at T, B at `max(0, T - offset)`.
2. **Codec of B**: AC-3/E-AC-3 pass through; stereo 48 kHz AAC-LC passes through
   (the only AAC the adapter declares); DTS uses the existing conversion (AC3
   5.1 with `dts_engine_set_ac3`, else stereo AAC). Any other codec is a failure
   of B, and the session returns to A's audio.
3. **Failure of B** (open, read, timestamp discontinuity, slow source, format
   refused by the native Load): the worker recreates the pipeline at the
   presented position with A's audio and publishes `ext_fallback` + `ext_error`
   in the status; `video.c` shows a notice. If A has no feedable track (TrueHD
   only, for instance) the session fails as it does today and the native player
   takes over.
4. **Memory**: B uses 1 MiB ranges with 6 of read-ahead (about 7 MiB), without
   touching A's prefetch.
5. **Changing the offset recreates the pipeline** (like a seek). The UI waits
   for the person to stop pressing before applying.
6. Choosing one of A's own tracks turns external audio off.

## Risks

- B is read whole (video included): MKV interleaves, so the audio cannot be
  downloaded alone. B's bitrate adds to A's.
- A stuck read of B holds the worker until the HTTP timeout; the 6 s lead
  covers the short cases.
- The offset is constant: different cuts or 23.976 vs 25 fps drift.
- How the Starfish pipeline behaves with audio from another origin can only be
  validated on the TV.

## Not touched

Samsung/.tpk (`dts_remux.c`, `dts_hls_tpk.c`, `video_tpk.c`, `video_tizen.c`),
VIDAA, Android, JS plugins, the Starfish adapter, the FFmpeg configuration and
the behavior of any session without external audio.

## Tests

Host, fake engines (`tests/dts_ext_audio.sh`): (a) the fed audio comes from B
and A's own is never fed; (b) timestamps carry the offset; (c) a seek
repositions A and B; (d) a failure of B at open and mid-stream returns to A's
audio without failing the session. Host, real FFmpeg
(`tests/dts_ext_engine.sh`): the same points at the engine level. On the TV:
only the owner can validate.
