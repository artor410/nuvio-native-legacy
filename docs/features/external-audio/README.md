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
| App wiring (`src/video.c`): start, offset, failure note | Done, cross-compiled and linked into the ARM app; never run |
| Audio sheet rows (`src/faixas.c`) | Done, cross-compiled and linked into the ARM app; never drawn or driven |
| Behavior on a TV | **Not validated** |

The feature is off unless the flag below is set. With it off nothing new is
reachable: `dts_playback_start` is `dts_playback_start_ext` with no external
audio, and an engine nobody configures keeps the default role and read-ahead.
See [PLAN.md](PLAN.md) for the plan.

## Turning it on

Off by default. Either of these, read when a playback starts:

- the file `/tmp/nuvio-ext-audio` on the TV (`touch /tmp/nuvio-ext-audio` over
  SSH; same convention as `/tmp/nuvio-dts-liberar`; gone after a reboot), or
- `NUVIO_EXT_AUDIO=1` in the app's environment.

There is no Settings option yet: adding one touches the settings sync tables,
four of their tests and every language file, and is better done once the path
is proven on a TV. It also needs the DTS fallback to be available on the TV
(`[dts] fallback available` in the log), a film or episode (not live TV), and
another direct HTTP source in the title's source list.

## Using it

Open the audio sheet during playback. After the source's own tracks:

- **Audio from another source...**: OK lists the other sources already
  resolved for the title (torrent-only sources are left out). OK on one starts
  it; Back returns to the tracks. The track taken from that source is the first
  one in the preferred audio language (Settings), else its first audio track.
- **External audio: on**: OK turns it off and returns to the source's audio.
- **Audio offset**: LEFT/RIGHT, 50 ms a press, 100 ms while the key repeats.
  Applied 0.7 s after the last press (it reloads the pipeline).

If the TV player was the one playing, choosing a source opens the fed pipeline
(a short reload). Choosing one of the source's own tracks leaves external
audio. A failure of the other source shows a notice and the film goes on with
its own audio. These texts are English literals, not yet in the app's
translation tables.

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
| Anything else (TrueHD, 5.1 AAC, HE-AAC, Opus, FLAC...) | Refused on webOS; the session uses A's audio |

The last row is a limit of the webOS FFmpeg build (`dca` is its only audio
decoder) and of what the Starfish adapter declares to the TV. The engine itself
converts whatever the linked FFmpeg can decode, so adding decoders to the SDK
image would widen it; that rebuild is not part of this change.

The track of B is an exact stream index; or the first track in a wanted
language that the path can feed; or B's first track the path can feed. A B
whose first track is TrueHD and second is AC-3 therefore works.

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
bash tests/dts_ext_engine.sh    # real engine and end to end, synthetic MKVs; needs the ffmpeg CLI
```

`dts_ext_audio` checks, against the real worker: the fed audio is B's and A's
own is never fed; timestamps carry the offset (delay, advance, clamp at zero);
a seek repositions both sources with one reload; B failing at open, mid-stream,
on a timestamp jump or at the native Load returns to A's audio without failing
the session; on/off and offset changes in session; track choice in B; B or A
ending first.

`dts_ext_engine` checks, against the real demuxer: A opens video-only with no
feedable audio track and delivers only video; B opens audio-only and delivers
only the chosen track, with the exact bytes of the file for E-AC-3, AC-3 and
AAC-LC; seek lands on the packet containing the target; B's read-ahead is its
own; an undecodable codec (FLAC) is refused.

The same script then runs `tests/dts_ext_e2e.c`: the real worker over the real
engines and the same MKVs, with only the native pipeline faked. It checks what
that pipeline is fed: with external audio on, the other source's E-AC-3 and
nothing of the movie's AC-3; with a 250 ms offset, every audio timestamp 250 ms
later; after a seek to 2 s, the movie from the keyframe before it and the other
source's audio landing on 2 s; with the other source missing, the movie's own
AC-3 and a session that did not fail. Each source is read with its own headers.

One thing the engine fixture showed: the minimal FFmpeg build has no AC-3 parser, so
E-AC-3 and AAC packets arrive without a duration, and the passthrough rule that
already existed (drop a packet that ends at or before the start target) then
drops the packet sitting exactly on the target. At most the first packets of a
start or seek are lost (one E-AC-3 frame, two or three AAC frames in the
fixture); no later byte differs. The same happens today on the Dolby Vision
path and was left as it is.

### What was actually run

- **GitHub Actions on the fork** (`.github/workflows/external-audio.yml`,
  ubuntu-24.04, gcc `-Wall -Wextra -Werror`): the 52 existing playback worker
  scenarios and the 15 new ones, each also under ASan/UBSan; the existing
  engine fixtures, the new one and the end-to-end one against the pinned
  static FFmpeg 7.1.5; `tests/video_acb_lg.sh`, which compiles the luna branch
  of `src/video.c` and runs its existing checks (the linker drops the external
  audio functions there, so they are compiled but not link-checked); and a
  compile-only check of `src/faixas.c` with and without the webOS defines.
- **Locally** (Windows, MSVC with a pthread shim, FFmpeg 7 shared libraries):
  the same fixtures during development.

- **The ARM app itself** (`.github/workflows/webos-ipk.yml`): `tools/arm.sh
  --ipk --build` in the SDK image cross-compiles and links the whole app with
  this change and packages the `.ipk`, passing the script's own checks.

What that does **not** cover: no test calls `video_ext_audio_*` or drives the
audio sheet rows, so the app wiring and the UI are built but never executed;
and the engine fixture does not exercise B as DTS (the conversion path is the existing one, with a
decoder chosen by codec in the audio-only role).

Host tests do not establish playback on any TV. Whether the Starfish pipeline
accepts audio whose timeline comes from another file, how A/V sync holds over a
film, and the cost of a second download on a 1-2 GB TV can only be measured on
the device.

## Getting an .ipk

Either the [main build recipe](../../../README.md#building) on a computer
(`tools/arm.sh --ipk --build`, Docker or Podman), or the "Build webOS IPK"
workflow of the fork: it runs on pushes to this branch that touch code and
uploads the package as an artifact of the run.

- Server URLs and client ids are not in the source. The workflow takes them
  from the repository secret `NUVIO_LOCAL_PROPERTIES` (the contents of a
  `local.properties` file). Without the secret the package builds, but the app
  has no account server and stops at the login screen. With it, the values end
  up inside the binary of a downloadable artifact, as they do in any release.
- The package has the same app id and version as the released app, so
  installing it replaces the one on the TV.
- The CI package has no P2P engine (`tools/arm.sh` reports it): torrent-only
  sources do not play in it. Direct HTTP sources, which are what external
  audio uses, are unaffected.

## First run on a TV (LG OLED55A1PSA, webOS 6.5.3, 2026-11-17)

Installed by copying the CI package over the app, flag set, driven through the
app's own `/tmp/nuvio-key` hook. A was a 4K Dolby Vision profile 8.1 MKV (AC-3
tracks in Russian, Ukrainian and English) from a debrid service through its
add-on, played by the TV player.

- The audio sheet shows the new row, and the picker lists the 48 direct
  sources already resolved for the title.
- B = a FrostStream 720p dub: its server answered HTTP 526 (Cloudflare origin
  error). The worker reported it, and the app went back to the TV player at
  the same position.
- B = a debrid 1080p WEB-DL through the same add-on: every 1 MiB range followed
  the add-on redirect again and the add-on answered HTTP 429 for both sources.
  Fixed: both engines of an external audio session now reuse the final address.
- The reload after that failure hit a network error before its Load, and the
  reconnection restarted the film at 0 s. Fixed: a reload still waiting for its
  Load reconnects to its own target (verified on the TV: 755 s).
- Second attempt, with both fixes: the debrid CDN refused TCP connections from
  the TV (curl error 7) for B, for A and for the TV player alike. Probably a
  limit on parallel connections: the fed path opens four lanes per source,
  the TV player one. Not resolved; the feature has not yet played a frame with
  audio from another source on a TV.

Two things this run showed about use: the picker shows only the source name
(AIOStreams names are stylized and carry no language), so finding a dub is
guesswork; and with Settings > Dolby Vision in MKV off, a Dolby Vision file is
played by the TV player and external audio has to move it to the fed path.

## Testing on a TV

Install the `.ipk` (`ares-install`, or Homebrew Channel), set the flag, play A
and choose B from the audio sheet. Worth checking, in this
order: that picture and sound start at all; Dolby Vision still reported by the
TV; seek forward and back; pause and resume; offset steps; a deliberately dead
B link (the notice, then A's audio); turning it off; sync at the start and near
the end of a film; memory over a long session. Reports should include
model, firmware, the codecs of both sources and the `[ext-audio]` / `[dts]`
log lines.
