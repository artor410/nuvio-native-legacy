#define _POSIX_C_SOURCE 200809L
#include "dts/dts_playback.h"
#include "dts/dts_engine.h"
#include "dts/dts_pipeline.h"
#include <assert.h>
#include <math.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
/* Audio from another source (docs/features/external-audio). Real controller,
 * two fake engines and a fake native boundary: A is the movie, B the other
 * source. No HTTP, TV or FFmpeg. Packets carry who made them in data[0]. */
#define MOVIE "fixture://movie"
#define DUB "fixture://dub"
static pthread_mutex_t fixture = PTHREAD_MUTEX_INITIALIZER;
static int a_opens, a_seeks, a_role, b_opens, b_seeks, b_role, b_ahead, b_stream, b_destroyed;
static double a_target, b_target; /* Last open or seek position of each source. */
static int creates, destroys, loads, eoses;
static int audio_a, audio_b, videos; /* Accepted since the last native Load. */
static int total_b;
static int64_t first_audio_pts, last_video_pts;
static char load_audio[16];
static int load_channels;
static atomic_int b_fail_open, b_fail_next, b_jump_back, b_eof_at, a_eof_at, refuse_eac3;
static void (*native_event)(void *,const char *);
static void *native_user;
struct DtsEngine { DtsMediaInfo info; atomic_int cancelled; unsigned char packet[8]; int serial, b, role, ahead; double base; };
struct DtsPipeline { void (*event)(void *,const char *); void *user; int loaded, completed; };
struct DtsSubtitles { int unused; };
static void nap(void) { struct timespec t={0,1000000};nanosleep(&t,NULL); }
static void emit(const char *json) {
 void (*cb)(void *,const char *);void *u;
 pthread_mutex_lock(&fixture);cb=native_event;u=native_user;pthread_mutex_unlock(&fixture);
 assert(cb);cb(u,json);
}
int dts_engine_available(void) { return 1; }
void dts_engine_allow_p7(DtsEngine *e,int on) { (void)e;(void)on; }
void dts_engine_dv(DtsEngine *e,int on) { (void)e;(void)on; }
void dts_engine_role(DtsEngine *e,int role) { e->role=role; }
void dts_engine_ahead_mib(DtsEngine *e,int mib) { e->ahead=mib; }
void dts_engine_buffer_info(DtsEngine *e,DtsBufferInfo *out) { (void)e;memset(out,0,sizeof *out); }
DtsEngine *dts_engine_create(void) { return calloc(1,sizeof(DtsEngine)); }
static void track(DtsMediaInfo *m,int index,int kind,const char *codec,const char *language,int channels) {
 DtsTrack *t=&m->tracks[m->n_tracks++];
 *t=(DtsTrack){.stream_index=index,.kind=kind,.channels=channels};
 strcpy(t->codec,codec);strcpy(t->language,language);
}
int dts_engine_open(DtsEngine *e,const char *url,const char *headers,int stream,int core,double target) {
 (void)core;e->b=!strcmp(url,DUB);assert(e->b || !strcmp(url,MOVIE));
 memset(&e->info,0,sizeof e->info);e->base=target;e->serial=0;
 pthread_mutex_lock(&fixture);
 if(e->b) { b_opens++;b_role=e->role;b_ahead=e->ahead;b_stream=stream;b_target=target; }
 else { a_opens++;a_role=e->role;a_target=target; }
 pthread_mutex_unlock(&fixture);
 if(e->b) {
  /* The other source gets its own headers, never the movie's credentials. */
  assert(!strcmp(headers,"X-Dub: yes"));
  if(atomic_load(&b_fail_open)) return -1;
  e->info.video_stream=-1;e->info.audio_stream=stream<0?1:stream;
  strcpy(e->info.audio_codec,e->info.audio_stream==2?"ac3":"eac3");e->info.channels=6;e->info.sample_rate=48000;
  track(&e->info,0,DTS_VIDEO,"h264","",0);
  track(&e->info,1,DTS_AUDIO,"eac3","eng",6);
  track(&e->info,2,DTS_AUDIO,"ac3","por",6);
  return 0;
 }
 assert(!strcmp(headers,"X-Token: fixture"));
 strcpy(e->info.video_codec,"hevc");e->info.width=3840;e->info.height=2160;e->info.dovi_profile=8;
 e->info.video_stream=0;e->info.audio_stream=-1;
 if(e->role!=DTS_ENGINE_VIDEO_ONLY) {
  e->info.audio_stream=stream<0?1:stream;
  strcpy(e->info.audio_codec,"aac");e->info.channels=2;e->info.sample_rate=48000;
 }
 track(&e->info,0,DTS_VIDEO,"hevc","",0);
 track(&e->info,1,DTS_AUDIO,"dts","eng",6);
 return 0;
}
const DtsMediaInfo *dts_engine_info(DtsEngine *e) { return &e->info; }
const void *dts_engine_subtitle_parameters(DtsEngine *e,int stream) { (void)e;(void)stream;return NULL; }
int dts_engine_has_core(DtsEngine *e) { (void)e;return 0; }
DtsSubtitles *dts_subtitles_create(const void *params) { (void)params;return calloc(1,sizeof(DtsSubtitles)); }
int dts_subtitles_decode(DtsSubtitles *s,const DtsFrame *f,DtsSubtitle *out) { (void)s;(void)f;(void)out;return 0; }
void dts_subtitles_flush(DtsSubtitles *s) { (void)s; }
void dts_subtitles_destroy(DtsSubtitles *s) { free(s); }
int dts_engine_next(DtsEngine *e,DtsFrame *f) {
 if(atomic_load(&e->cancelled)) return -1;
 nap();e->serial++;
 int64_t base=(int64_t)llround(e->base*1e9);
 if(e->b) {
  int end=atomic_load(&b_eof_at);
  if(atomic_load(&b_fail_next)) return -1;
  if(end && e->serial>end) return 0;
  /* 32 ms frames, like AC-3 at 48 kHz. */
  e->packet[0]='B';
  *f=(DtsFrame){.kind=DTS_AUDIO,.stream_index=e->info.audio_stream,.data=e->packet,.size=4,
                .pts_ns=base+e->serial*INT64_C(32000000),.duration_ns=INT64_C(32000000)};
  f->dts_ns=f->pts_ns;
  if(atomic_load(&b_jump_back) && e->serial==6) f->pts_ns-=INT64_C(10000000000);
  return 1;
 }
 {
  int end=atomic_load(&a_eof_at);
  if(end && e->serial>end) return 0;
 }
 /* The movie keeps interleaving its own audio in every role: the controller,
  * not the engine, is what must keep it away from the pipeline here. */
 e->packet[0]=e->serial%2?'V':'A';
 *f=(DtsFrame){.kind=e->serial%2?DTS_VIDEO:DTS_AUDIO,.stream_index=e->serial%2?0:1,.data=e->packet,.size=4,
               .pts_ns=base+e->serial*INT64_C(20000000),.duration_ns=INT64_C(40000000)};
 f->dts_ns=f->pts_ns;
 return 1;
}
int dts_engine_seek(DtsEngine *e,double target) {
 pthread_mutex_lock(&fixture);
 if(e->b) { b_seeks++;b_target=target; } else { a_seeks++;a_target=target; }
 pthread_mutex_unlock(&fixture);
 e->base=target;e->serial=0;return 0;
}
void dts_engine_cancel(DtsEngine *e) { atomic_store(&e->cancelled,1); }
void dts_engine_destroy(DtsEngine *e) {
 if(!e) return;
 if(e->b) { pthread_mutex_lock(&fixture);b_destroyed++;pthread_mutex_unlock(&fixture); }
 free(e);
}
const char *dts_engine_error(DtsEngine *e) { return e->b?"fixture dub failure":"fixture movie failure"; }
int dts_pipeline_available(int major) { (void)major;return 1; }
DtsPipeline *dts_pipeline_create(const char *a,const char *w,int m,void (*cb)(void *,const char *),void *u) {
 (void)a;(void)w;(void)m;DtsPipeline *p=calloc(1,sizeof *p);p->event=cb;p->user=u;
 pthread_mutex_lock(&fixture);creates++;native_event=cb;native_user=u;pthread_mutex_unlock(&fixture);return p;
}
int dts_pipeline_load(DtsPipeline *p,const DtsMediaInfo *m,double t) {
 (void)t;
 /* The real adapter refuses audio it cannot declare; so can this one. */
 if(atomic_load(&refuse_eac3) && !strcmp(m->audio_codec,"eac3")) return 0;
 pthread_mutex_lock(&fixture);
 loads++;audio_a=audio_b=videos=0;first_audio_pts=-1;
 snprintf(load_audio,sizeof load_audio,"%s",m->audio_codec);load_channels=m->channels;
 pthread_mutex_unlock(&fixture);
 p->loaded=1;return 1;
}
int dts_pipeline_feed(DtsPipeline *p,const DtsFrame *f) {
 assert(p->loaded);
 pthread_mutex_lock(&fixture);
 if(f->kind==DTS_AUDIO) {
  if(f->data[0]=='B') { audio_b++;total_b++; } else { assert(f->data[0]=='A');audio_a++; }
  if(first_audio_pts<0) first_audio_pts=f->pts_ns;
 } else { assert(f->kind==DTS_VIDEO && f->data[0]=='V');videos++;last_video_pts=f->pts_ns; }
 pthread_mutex_unlock(&fixture);
 if(!p->completed) { p->completed=1;p->event(p->user,"{\"loadCompleted\":{}}"); }
 return 1;
}
int dts_pipeline_play(DtsPipeline *p) { (void)p;return 1; }
int dts_pipeline_pause(DtsPipeline *p) { (void)p;return 1; }
int dts_pipeline_flush(DtsPipeline *p,double t) { (void)p;(void)t;return 1; }
int dts_pipeline_eos(DtsPipeline *p) { (void)p;pthread_mutex_lock(&fixture);eoses++;pthread_mutex_unlock(&fixture);return 1; }
int dts_pipeline_volume(DtsPipeline *p,int pct) { (void)p;(void)pct;return 1; }
void dts_pipeline_destroy(DtsPipeline *p) {
 if(!p) return;
 pthread_mutex_lock(&fixture);destroys++;native_event=NULL;pthread_mutex_unlock(&fixture);free(p);
}
const char *dts_pipeline_media_id(DtsPipeline *p) { (void)p;return "fixture-media"; }
const char *dts_pipeline_error(DtsPipeline *p) { (void)p;return "fixture pipeline failure"; }
static int count(int *v) { int n;pthread_mutex_lock(&fixture);n=*v;pthread_mutex_unlock(&fixture);return n; }
static double seconds(double *v) { double n;pthread_mutex_lock(&fixture);n=*v;pthread_mutex_unlock(&fixture);return n; }
static int64_t stamp(int64_t *v) { int64_t n;pthread_mutex_lock(&fixture);n=*v;pthread_mutex_unlock(&fixture);return n; }
static void wait_count(int *v,int expected) {
 int n;for(n=0;n<3000 && count(v)<expected;n++)nap();
 if(count(v)<expected) { fprintf(stderr,"waited for %d, have %d\n",expected,count(v));assert(0); }
}
static DtsPlaybackStatus status(DtsPlayback *p) { DtsPlaybackStatus s;memset(&s,0,sizeof s);dts_playback_status(p,&s);return s; }
static int saw_event(DtsPlayback *p,const char *needle) {
 char event[2048];int saw=0;
 while(dts_playback_event(p,event,sizeof event)) saw|=strstr(event,needle)!=NULL;
 return saw;
}
static DtsExtAudio dub(int offset_ms,int stream,const char *language) {
 return (DtsExtAudio){.url=DUB,.headers="X-Dub: yes",.language=language,.stream_index=stream,.offset_ms=offset_ms};
}
static DtsPlayback *start(const DtsExtAudio *ext,double position) {
 DtsPlayback *p=dts_playback_start_ext(MOVIE,"X-Token: fixture",1,position,0,"window",0,NULL,0,0,ext);
 assert(p);return p;
}
int main(int argc,char **argv) {
 assert(argc==2);DtsPlayback *p;DtsPlaybackStatus s;DtsExtAudio ext;
 if(!strcmp(argv[1],"fromb")) {
  /* (a) The fed audio is the other source's; the movie's own never arrives. */
  ext=dub(0,-1,NULL);p=start(&ext,0);wait_count(&audio_b,150);wait_count(&videos,100);
  s=status(p);assert(s.prepared && !s.failed && s.ext_audio && !s.ext_fallback && !s.ext_error[0]);
  assert(count(&audio_a)==0);
  assert(a_role==DTS_ENGINE_VIDEO_ONLY && b_role==DTS_ENGINE_AUDIO_ONLY);
  assert(b_ahead>=4 && b_ahead<=8); /* Small read-ahead of its own. */
  assert(!strcmp(load_audio,"eac3") && load_channels==6);
  assert(!strcmp(s.info.video_codec,"hevc") && !strcmp(s.info.audio_codec,"eac3") && s.info.dovi_profile==8);
  dts_playback_close(p);assert(count(&b_destroyed)==1);
 } else if(!strcmp(argv[1],"offset") || !strcmp(argv[1],"advance") || !strcmp(argv[1],"clamp")) {
  /* (b) pts_out = pts + offset; the other source starts at T - offset, >= 0.
   * Its first packet sits 32 ms after its start position. */
  int advance=!strcmp(argv[1],"advance"),clamp=!strcmp(argv[1],"clamp");
  ext=dub(advance?-500:250,-1,NULL);p=start(&ext,clamp?0.125:10);wait_count(&audio_b,1);
  assert(seconds(&a_target)==(clamp?0.125:10));
  assert(seconds(&b_target)==(clamp?0:advance?10.5:9.75));
  assert(stamp(&first_audio_pts)==(clamp?INT64_C(282000000):INT64_C(10032000000)));
  s=status(p);assert(s.ext_audio && s.ext_offset_ms==(advance?-500:250));
  dts_playback_close(p);
 } else if(!strcmp(argv[1],"seek")) {
  /* (c) A seek repositions both sources and reloads the pipeline once. */
  ext=dub(250,-1,NULL);p=start(&ext,0);wait_count(&audio_b,3);
  dts_playback_seek(p,60);wait_count(&loads,2);wait_count(&audio_b,3);wait_count(&videos,3);
  assert(seconds(&a_target)==60 && seconds(&b_target)==59.75);
  assert(count(&a_opens)==1 && count(&b_opens)==1 && count(&a_seeks)==1 && count(&b_seeks)==1);
  assert(count(&creates)==2 && count(&audio_a)==0);
  assert(stamp(&first_audio_pts)==INT64_C(60032000000));
  assert(stamp(&last_video_pts)>=INT64_C(60000000000));
  s=status(p);assert(!s.failed && s.ext_audio && s.revision==2);
  dts_playback_close(p);
 } else if(!strcmp(argv[1],"openfail")) {
  /* (d) The other source does not open: the movie plays with its own audio. */
  atomic_store(&b_fail_open,1);ext=dub(0,-1,NULL);p=start(&ext,0);wait_count(&audio_a,3);wait_count(&videos,3);
  s=status(p);assert(s.prepared && !s.failed && !s.ext_audio && s.ext_fallback && strstr(s.ext_error,"dub failure"));
  assert(count(&total_b)==0 && a_role==DTS_ENGINE_AV && !strcmp(load_audio,"aac"));
  assert(count(&loads)==1 && count(&a_opens)==1);
  assert(saw_event(p,"ext-audio-fallback"));
  dts_playback_close(p);
 } else if(!strcmp(argv[1],"midfail") || !strcmp(argv[1],"discontinuity")) {
  /* (d) It fails mid-stream: same video, the movie's audio, at the native
   * presentation clock. The session does not fail. */
  int jump=!strcmp(argv[1],"discontinuity");
  if(jump) atomic_store(&b_jump_back,1);
  ext=dub(0,-1,NULL);p=start(&ext,0);wait_count(&audio_b,3);
  if(!jump) { emit("{\"currentTime\":{\"currentTime\":5000}}");atomic_store(&b_fail_next,1); }
  wait_count(&loads,2);wait_count(&audio_a,3);wait_count(&videos,3);
  s=status(p);assert(!s.failed && !s.ext_audio && s.ext_fallback);
  assert(strstr(s.ext_error,jump?"discontinuity":"dub failure"));
  assert(count(&audio_b)==0 && a_role==DTS_ENGINE_AV && !strcmp(load_audio,"aac"));
  if(!jump) assert(seconds(&a_target)==5);
  assert(count(&b_destroyed)==1 && saw_event(p,"ext-audio-fallback"));
  dts_playback_close(p);
 } else if(!strcmp(argv[1],"refused")) {
  /* (d) The native Load refuses the other source's audio format. */
  atomic_store(&refuse_eac3,1);ext=dub(0,-1,NULL);p=start(&ext,0);wait_count(&audio_a,3);
  s=status(p);assert(!s.failed && !s.ext_audio && s.ext_fallback && strstr(s.ext_error,"refused"));
  assert(count(&total_b)==0 && count(&loads)==1);
  dts_playback_close(p);
 } else if(!strcmp(argv[1],"toggle")) {
  /* In session: on, new offset, off. Each recreates the pipeline in place. */
  p=start(NULL,0);wait_count(&audio_a,3);s=status(p);assert(!s.ext_audio && a_role==DTS_ENGINE_AV);
  ext=dub(0,-1,NULL);dts_playback_ext_audio(p,&ext);wait_count(&loads,2);wait_count(&audio_b,3);
  s=status(p);assert(s.ext_audio && count(&audio_a)==0 && a_role==DTS_ENGINE_VIDEO_ONLY);
  emit("{\"currentTime\":{\"currentTime\":20000}}");
  dts_playback_ext_offset(p,500);wait_count(&loads,3);wait_count(&audio_b,1);
  assert(seconds(&a_target)==20 && seconds(&b_target)==19.5);
  assert(count(&b_opens)==1 && count(&b_seeks)==1); /* Same source: a seek, not a reopen. */
  assert(stamp(&first_audio_pts)==INT64_C(20032000000));
  s=status(p);assert(s.ext_audio && s.ext_offset_ms==500);
  dts_playback_ext_audio(p,NULL);wait_count(&loads,4);wait_count(&audio_a,3);
  s=status(p);assert(!s.failed && !s.ext_audio && !s.ext_fallback && count(&audio_b)==0 && a_role==DTS_ENGINE_AV);
  dts_playback_close(p);
 } else if(!strcmp(argv[1],"trackpick")) {
  /* Choosing one of the movie's own tracks leaves the other source. */
  ext=dub(0,-1,NULL);p=start(&ext,0);wait_count(&audio_b,3);
  dts_playback_audio(p,1);wait_count(&loads,2);wait_count(&audio_a,3);
  s=status(p);assert(!s.failed && !s.ext_audio && !s.ext_fallback && count(&audio_b)==0);
  dts_playback_close(p);
 } else if(!strcmp(argv[1],"language") || !strcmp(argv[1],"explicit")) {
  /* Track of the other source: an exact stream, or the first in a language. */
  ext=!strcmp(argv[1],"explicit")?dub(0,2,NULL):dub(0,-1,"pt");
  p=start(&ext,0);wait_count(&audio_b,3);
  s=status(p);assert(s.ext_audio && count(&b_stream)==2 && !strcmp(load_audio,"ac3"));
  dts_playback_close(p);
 } else if(!strcmp(argv[1],"beof")) {
  /* The other source is shorter: the video goes on, nothing fails. */
  atomic_store(&b_eof_at,10);ext=dub(0,-1,NULL);p=start(&ext,0);wait_count(&videos,120);
  s=status(p);assert(!s.failed && !s.eof && s.ext_audio && !s.ext_fallback);
  assert(count(&audio_b)==10 && count(&audio_a)==0 && count(&eoses)==0);
  dts_playback_close(p);
 } else if(!strcmp(argv[1],"aeof")) {
  /* The movie ends: end of stream, whatever the other source still has. */
  atomic_store(&a_eof_at,40);ext=dub(0,-1,NULL);p=start(&ext,0);wait_count(&eoses,1);
  s=status(p);assert(!s.failed && s.eof);
  dts_playback_close(p);
 } else assert(0);
 assert(count(&creates)==count(&destroys));printf("DTS external audio %s passed\n",argv[1]);return 0;
}
