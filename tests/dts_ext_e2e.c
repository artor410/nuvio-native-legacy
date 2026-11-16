/* External audio end to end on the host (docs/features/external-audio): the
 * real playback worker and the real engines over synthetic MKVs. Only the
 * native pipeline is faked, and what it is fed is what gets checked.
 * Usage: dts_ext_e2e movie.mkv dub.mkv outdir */
#include "../src/dts/dts_playback.h"
#include "../src/dts/dts_engine.h"
#include "../src/dts/dts_pipeline.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define MOVIE_HEADERS "X-Fixture: movie"
#define DUB_HEADERS "X-Fixture: dub"
/* Local-file transport that models strict 206 IO, as in tests/dts_engine.c.
 * Each source must be read with its own headers, never the other's. */
char *rede_range_headers(const char *raw,int public_only) {
  (void)public_only;return strdup(raw?raw:"");
}
int rede_fluxo64(const char *url,const char *headers,int64_t start,
                 int (*sink)(void *,const char *,size_t),void *opaque,
                 int64_t *total,int *status,volatile int *cancel,
                 char *final,int *cross,long stall_ms,int64_t *bytes) {
  (void)url;(void)headers;(void)start;(void)sink;(void)opaque;
  (void)total;(void)status;(void)cancel;(void)final;(void)cross;
  (void)stall_ms;(void)bytes;
  assert(!"external audio must not enter the Samsung streaming reader");
  return -1;
}
char *rede_baixar_trecho64_cab(const char *url,const char *headers,int64_t start,int64_t end,long *size,int64_t *total,int *status,volatile int *cancelled) {
  assert(!strcmp(headers,strstr(url,"movie.mkv")?MOVIE_HEADERS:DUB_HEADERS));
  assert(start>=0 && end>=start && end-start<4*1024*1024);
  *size=0; *status=0; *total=-1;
  if(*cancelled) return NULL;
  FILE *f=fopen(url,"rb"); if(!f) return NULL;
  assert(!fseek(f,0,SEEK_END)); *total=ftell(f);
  if(start>=*total) { *status=416; fclose(f); return NULL; }
  assert(!fseek(f,(long)start,SEEK_SET));
  long n=(long)(end-start+1); if(n>*total-start)n=(long)(*total-start);
  char *data=malloc(n); assert(data); *size=(long)fread(data,1,n,f); fclose(f); *status=206; return data;
}
char *rede_baixar_trecho64_final(const char *url,const char *headers,int64_t start,int64_t end,long *size,int64_t *total,int *status,volatile int *cancelled,char *final,int *cross) {
  if(final) final[0]=0;
  if(cross) *cross=0;
  return rede_baixar_trecho64_cab(url,headers,start,end,size,total,status,cancelled);
}
char *rede_baixar_trecho64_vigiado(const char *u,const char *h,int64_t s,int64_t e,long *n,
 int64_t *t,int *status,volatile int *c,char *f,int *x,volatile int *urgent,unsigned idle,int *stalled) {
 (void)urgent;(void)idle;if(stalled)*stalled=0;
 return rede_baixar_trecho64_final(u,h,s,e,n,t,status,c,f,x);
}
/* No subtitle is selected in these sessions. */
struct DtsSubtitles { int unused; };
DtsSubtitles *dts_subtitles_create(const void *params) { (void)params;return calloc(1,sizeof(DtsSubtitles)); }
int dts_subtitles_decode(DtsSubtitles *s,const DtsFrame *f,DtsSubtitle *out) { (void)s;(void)f;(void)out;return 0; }
void dts_subtitles_flush(DtsSubtitles *s) { (void)s; }
void dts_subtitles_destroy(DtsSubtitles *s) { free(s); }
/* The fake native pipeline: remembers each Load and keeps the audio it is fed. */
static pthread_mutex_t fixture=PTHREAD_MUTEX_INITIALIZER;
static int loads, videos, audios, eoses;
static int64_t first_audio, first_video, last_video;
static char load_audio[16];
static int load_channels;
static FILE *capture;
struct DtsPipeline { void (*event)(void *,const char *); void *user; int completed; };
int dts_pipeline_available(int major) { (void)major;return 1; }
DtsPipeline *dts_pipeline_create(const char *a,const char *w,int m,void (*cb)(void *,const char *),void *u) {
  (void)a;(void)w;(void)m;DtsPipeline *p=calloc(1,sizeof *p);p->event=cb;p->user=u;return p;
}
int dts_pipeline_load(DtsPipeline *p,const DtsMediaInfo *m,double t) {
  (void)p;(void)t;
  pthread_mutex_lock(&fixture);
  loads++;videos=audios=0;first_audio=first_video=last_video=-1;
  snprintf(load_audio,sizeof load_audio,"%s",m->audio_codec);load_channels=m->channels;
  pthread_mutex_unlock(&fixture);
  return 1;
}
int dts_pipeline_feed(DtsPipeline *p,const DtsFrame *f) {
  pthread_mutex_lock(&fixture);
  if(f->kind==DTS_AUDIO) {
    if(first_audio<0) first_audio=f->pts_ns;
    audios++;
    if(capture) assert(fwrite(f->data,1,f->size,capture)==f->size);
  } else {
    assert(f->kind==DTS_VIDEO);
    if(first_video<0) first_video=f->pts_ns;
    last_video=f->pts_ns;videos++;
  }
  pthread_mutex_unlock(&fixture);
  if(!p->completed) { p->completed=1;p->event(p->user,"{\"loadCompleted\":{}}"); }
  return 1;
}
int dts_pipeline_play(DtsPipeline *p) { (void)p;return 1; }
int dts_pipeline_pause(DtsPipeline *p) { (void)p;return 1; }
int dts_pipeline_flush(DtsPipeline *p,double t) { (void)p;(void)t;return 1; }
int dts_pipeline_eos(DtsPipeline *p) { (void)p;pthread_mutex_lock(&fixture);eoses++;pthread_mutex_unlock(&fixture);return 1; }
int dts_pipeline_volume(DtsPipeline *p,int pct) { (void)p;(void)pct;return 1; }
void dts_pipeline_destroy(DtsPipeline *p) { free(p); }
const char *dts_pipeline_media_id(DtsPipeline *p) { (void)p;return "fixture-media"; }
const char *dts_pipeline_error(DtsPipeline *p) { (void)p;return "fixture pipeline failure"; }

static void nap(void) { struct timespec t={0,2000000};nanosleep(&t,NULL); }
static int count(int *v) { int n;pthread_mutex_lock(&fixture);n=*v;pthread_mutex_unlock(&fixture);return n; }
static int64_t stamp(int64_t *v) { int64_t n;pthread_mutex_lock(&fixture);n=*v;pthread_mutex_unlock(&fixture);return n; }
/* The whole 4 s film fits the worker's lead: it is fed to its end, then EOS. */
static DtsPlaybackStatus wait_end(DtsPlayback *p,int ends) {
  DtsPlaybackStatus s;
  for(int i=0;i<10000;i++) {
    memset(&s,0,sizeof s);dts_playback_status(p,&s);
    if(s.failed || (s.eof && count(&eoses)>=ends)) return s;
    nap();
  }
  fprintf(stderr,"timed out: prepared=%d eof=%d error=%s ext=%s\n",s.prepared,s.eof,s.error,s.ext_error);
  assert(0);return s;
}
static void capture_to(const char *outdir,const char *name) {
  char path[2048];
  pthread_mutex_lock(&fixture);
  if(capture) fclose(capture);
  capture=NULL;
  if(name) { snprintf(path,sizeof path,"%s/%s",outdir,name);capture=fopen(path,"wb");assert(capture); }
  loads=eoses=0;
  pthread_mutex_unlock(&fixture);
}
int main(int argc,char **argv) {
  assert(argc==4);
  const char *movie=argv[1],*dub=argv[2],*outdir=argv[3];
  char missing[2048];
  DtsPlayback *p;DtsPlaybackStatus s;DtsExtAudio ext;int64_t plain,later;
  int plain_audios;
  snprintf(missing,sizeof missing,"%s/missing.mkv",outdir);

  /* (a) The movie's only audio is AC-3, which a plain session cannot even
   * feed. With the other source's E-AC-3 track it plays, and every audio
   * byte fed is that track's (the script compares the capture). */
  capture_to(outdir,"e2e-dub.bin");
  ext=(DtsExtAudio){.url=dub,.headers=DUB_HEADERS,.stream_index=1,.offset_ms=0};
  p=dts_playback_start_ext(movie,MOVIE_HEADERS,-1,0,0,"window",0,NULL,0,0,&ext);assert(p);
  s=wait_end(p,1);
  assert(!s.failed && s.ext_audio && !s.ext_fallback);
  assert(!strcmp(load_audio,"eac3") && load_channels==6 && !strcmp(s.info.video_codec,"h264"));
  assert(count(&loads)==1 && count(&videos)==100); /* 4 s at 25 fps, all of it. */
  plain=stamp(&first_audio);plain_audios=count(&audios);
  assert(plain>=0 && plain_audios>=120);
  dts_playback_close(p);

  /* (b) The same track 250 ms later: every timestamp moves by the offset. */
  capture_to(outdir,NULL);
  ext.offset_ms=250;
  p=dts_playback_start_ext(movie,MOVIE_HEADERS,-1,0,0,"window",0,NULL,0,0,&ext);assert(p);
  s=wait_end(p,1);
  later=stamp(&first_audio);
  assert(!s.failed && s.ext_audio && s.ext_offset_ms==250);
  assert(later==plain+INT64_C(250000000));
  /* The session ends with the movie: the delayed audio past its last frame
   * (250 ms, eight 32 ms packets) is never fed. */
  assert(count(&audios)<=plain_audios && count(&audios)>=plain_audios-9);

  /* (c) Seek to 2 s: one reload. The movie restarts at the keyframe at or
   * before 2 s (one per second here) and plays to its end; the other source
   * restarts at 1.75 s, so its audio lands on 2 s after the offset. */
  dts_playback_seek(p,2.0);
  s=wait_end(p,2);
  assert(!s.failed && s.ext_audio && count(&loads)==2);
  printf("after the seek: video %lld..%lld ns, first audio %lld ns\n",(long long)stamp(&first_video),
         (long long)stamp(&last_video),(long long)stamp(&first_audio));
  assert(stamp(&first_video)>=INT64_C(1000000000) && stamp(&first_video)<=INT64_C(2000000000));
  assert(stamp(&last_video)>=INT64_C(3900000000));
  /* The first packet is the one containing 1.75 s, or the next one when the
   * container gives no duration: within one 32 ms frame of 2 s either way. */
  assert(stamp(&first_audio)>=INT64_C(1968000000) && stamp(&first_audio)<=INT64_C(2040000000));
  dts_playback_close(p);

  /* (d) The other source does not exist: the session plays with the movie's
   * own AC-3 (a Dolby Vision path session can feed it) and does not fail. */
  capture_to(outdir,"e2e-movie.bin");
  ext=(DtsExtAudio){.url=missing,.headers=DUB_HEADERS,.stream_index=-1,.offset_ms=0};
  dts_playback_next_dv(1);
  p=dts_playback_start_ext(movie,MOVIE_HEADERS,-1,0,0,"window",0,NULL,0,0,&ext);assert(p);
  s=wait_end(p,1);
  assert(!s.failed && !s.ext_audio && s.ext_fallback && s.ext_error[0]);
  assert(!strstr(s.ext_error,"missing.mkv")); /* No path or URL in the note. */
  assert(!strcmp(load_audio,"ac3") && count(&loads)==1 && count(&videos)==100);
  dts_playback_close(p);
  capture_to(outdir,NULL);
  puts("External audio end to end: fed audio is the other source's, offset, seek of both sources and fallback passed.");
  return 0;
}
