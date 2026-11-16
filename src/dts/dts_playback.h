#ifndef NV_DTS_PLAYBACK_H
#define NV_DTS_PLAYBACK_H
#include "dts_media.h"
#include "dts_subtitles.h"
#include "dts_engine.h"
typedef struct DtsPlayback DtsPlayback;
typedef struct {
  int prepared, failed, eof, revision, native_loaded;
  DtsMediaInfo info;
  char media_id[64], error[160];
  DtsBufferInfo buffer;
  double buffered_until;
  /* External audio: ext_audio while the fed audio comes from the other
   * source. ext_fallback/ext_error after that source failed and the session
   * returned to its own audio; cleared by the next dts_playback_ext_audio. */
  int ext_audio, ext_fallback, ext_offset_ms;
  char ext_error[96];
} DtsPlaybackStatus;
/* Audio from another source (docs/features/external-audio): the video of the
 * session URL plays with one audio track of `url`. Fed timestamps are the
 * track's plus offset_ms, so a positive offset delays the audio; a seek to T
 * puts the session source at T and this one at T - offset (never below 0).
 * stream_index < 0 picks the first audio track matching `language` (optional),
 * else the first audio track. Strings are copied. */
typedef struct {
  const char *url, *headers, *language;
  int stream_index, offset_ms;
} DtsExtAudio;
#define DTS_EXT_OFFSET_MAX_MS 30000
/* All expensive work and native Feed calls are serialized on one worker.
 * Callers own a session; close joins it before freeing callback context. */
int dts_playback_enabled(void);
/* Error text of a Dolby Vision session whose source could not keep up. */
#define DTS_PLAYBACK_STARVED "Source too slow for the Dolby Vision path"
/* Main thread, right before dts_playback_start: the next session is a Dolby
 * Vision one (AC-3/E-AC-3 passthrough, larger direct ranges, starvation
 * fallback). Consumed by that start. */
void dts_playback_next_dv(int enabled);
/* Main thread: snapshot the P7/recovery opt-in for the next session. */
void dts_playback_next_p7(int enabled);
DtsPlayback *dts_playback_start(const char *url, const char *headers,
                               int audio_stream, double position,
                               int paused, const char *window, int webos_major,
                               const DtsTrack *selected, int ordinal, int count);
/* Same session with the audio of another source from the start; ext NULL is
 * dts_playback_start. A failure of that source never fails the session: the
 * worker returns to the session source's own audio (status.ext_fallback). */
DtsPlayback *dts_playback_start_ext(const char *url, const char *headers,
                                   int audio_stream, double position,
                                   int paused, const char *window, int webos_major,
                                   const DtsTrack *selected, int ordinal, int count,
                                   const DtsExtAudio *ext);
/* In session: switch to the other source, or back to the session's own audio
 * (NULL or empty url). Both, and an offset change, recreate the native
 * pipeline at the current position, like a seek. dts_playback_audio also
 * returns to the session's own audio. */
void dts_playback_ext_audio(DtsPlayback *p, const DtsExtAudio *ext);
void dts_playback_ext_offset(DtsPlayback *p, int offset_ms);
void dts_playback_status(DtsPlayback *p, DtsPlaybackStatus *out);
int dts_playback_event(DtsPlayback *p, char *json, size_t n);
void dts_playback_pause(DtsPlayback *p, int paused);
void dts_playback_volume(DtsPlayback *p, int percent);
void dts_playback_seek(DtsPlayback *p, double seconds);
void dts_playback_audio(DtsPlayback *p, int stream);
/* -1 off; -2-ordinal restores a native subtitle; >=0 exact source stream. */
void dts_playback_subtitle(DtsPlayback *p, int stream);
int dts_playback_subtitle_text(DtsPlayback *p, double seconds, char *out, size_t n);
/* Copies changed RGBA into caller-owned storage. A clearing cue has no pixels.
 * previous_revision avoids uploading/copying the same bitmap each frame. */
int dts_playback_subtitle_bitmap(DtsPlayback *p, double seconds,
                                 unsigned previous_revision, DtsSubtitle *out,
                                 unsigned *revision);
void dts_playback_close(DtsPlayback *p);
#endif
