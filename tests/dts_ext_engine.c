/* External audio (docs/features/external-audio): the real engine in its two
 * new roles, over the local-file transport that models strict 206 IO.
 * Usage: dts_ext_engine movie.mkv dub.mkv outdir [unsupported.mkv] */
#include "../src/dts/dts_engine.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char *HEADERS="X-Fixture: yes";
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
  assert(!strcmp(headers,HEADERS));
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
static void check(DtsEngine *e,int result) { if(result<0) { fprintf(stderr,"%s\n",dts_engine_error(e)); exit(1); } }
/* Drains the engine. Every frame must be of `kind`; audio payload goes to
 * outdir/name so the script can compare it with the track's exact bytes. */
static int drain(DtsEngine *e,int kind,int stream,const char *outdir,const char *name) {
  char path[2048]; FILE *out=NULL; DtsFrame f; int r,count=0; int64_t last=INT64_MIN;
  if(name) { snprintf(path,sizeof path,"%s/%s",outdir,name); out=fopen(path,"wb"); assert(out); }
  while((r=dts_engine_next(e,&f))>0) {
    assert(f.kind==kind && f.size>0);
    if(kind==DTS_AUDIO) {
      assert(f.stream_index==stream && f.pts_ns>=last); last=f.pts_ns;
      if(out) assert(fwrite(f.data,1,f.size,out)==f.size);
    }
    count++;
  }
  check(e,r);
  if(out) fclose(out);
  return count;
}
int main(int argc,char **argv) {
  assert(argc==4 || argc==5);
  const char *movie=argv[1],*dub=argv[2],*outdir=argv[3];
  const DtsMediaInfo *info; DtsBufferInfo buffer; DtsFrame f; int frames;
  DtsEngine *e=dts_engine_create(); assert(e && dts_engine_available());

  /* The movie's only audio is AC-3 and this is not a Dolby Vision session: in
   * the default role nothing can be fed, which is why the roles exist. */
  assert(dts_engine_open(e,movie,HEADERS,-1,0,0)<0);
  assert(strstr(dts_engine_error(e),"track or video absent"));

  /* Video-only: opens without an audio track and never delivers one. */
  dts_engine_role(e,DTS_ENGINE_VIDEO_ONLY);
  check(e,dts_engine_open(e,movie,HEADERS,-1,0,0));
  info=dts_engine_info(e);
  assert(info->video_stream==0 && info->audio_stream==-1 && !info->audio_codec[0]);
  assert(!strcmp(info->video_codec,"h264") && info->duration_ns>0);
  assert(drain(e,DTS_VIDEO,-1,outdir,NULL)==100); /* 4 s at 25 fps. */
  /* Default read-ahead of an engine nobody configured: 32 + 1 ranges of 1 MiB. */
  dts_engine_buffer_info(e,&buffer); assert(buffer.capacity_bytes==33*1048576LL);
  dts_engine_destroy(e);

  /* Audio-only: the first audio track, E-AC-3 untouched; no video, no subtitle. */
  e=dts_engine_create(); assert(e);
  dts_engine_role(e,DTS_ENGINE_AUDIO_ONLY); dts_engine_ahead_mib(e,6);
  check(e,dts_engine_open(e,dub,HEADERS,-1,0,0));
  info=dts_engine_info(e);
  assert(info->audio_stream==1 && info->video_stream==-1);
  assert(!strcmp(info->audio_codec,"eac3") && info->channels==6 && info->sample_rate==48000);
  assert(info->n_tracks==5 && !strcmp(info->tracks[2].language,"por"));
  frames=drain(e,DTS_AUDIO,1,outdir,"eac3.bin"); printf("eac3 frames: %d\n",frames);
  assert(frames>=124 && frames<=127); /* 4 s of 32 ms frames; encoders pad differently. */
  /* Its own read-ahead: 6 + 1 ranges of 1 MiB, not the 33 MiB default. */
  dts_engine_buffer_info(e,&buffer); assert(buffer.capacity_bytes==7*1048576LL);

  /* An exact track: AC-3, untouched. */
  check(e,dts_engine_open(e,dub,HEADERS,2,0,0));
  info=dts_engine_info(e);
  assert(info->audio_stream==2 && !strcmp(info->audio_codec,"ac3") && info->channels==6);
  frames=drain(e,DTS_AUDIO,2,outdir,"ac3.bin"); printf("ac3 frames: %d\n",frames);
  assert(frames>=124 && frames<=127);

  /* Stereo 48 kHz AAC-LC is the one AAC the native pipeline declares: untouched. */
  check(e,dts_engine_open(e,dub,HEADERS,3,0,0));
  info=dts_engine_info(e);
  assert(info->audio_stream==3 && !strcmp(info->audio_codec,"aac") && info->channels==2 && info->sample_rate==48000);
  frames=drain(e,DTS_AUDIO,3,outdir,"aac.bin"); printf("aac frames: %d\n",frames);
  assert(frames>=186 && frames<=190); /* 4 s of 21.3 ms frames. */

  /* Seek: the first packet delivered is the one that contains the target. */
  check(e,dts_engine_open(e,dub,HEADERS,2,0,0));
  check(e,dts_engine_seek(e,2.0));
  assert(dts_engine_next(e,&f)==1 && f.kind==DTS_AUDIO && f.stream_index==2);
  assert(f.pts_ns<=INT64_C(2000000000) && f.pts_ns+f.duration_ns>INT64_C(2000000000));
  /* Opening at a position is the same seek. */
  check(e,dts_engine_open(e,dub,HEADERS,2,0,3.0));
  assert(dts_engine_next(e,&f)==1 && f.kind==DTS_AUDIO);
  assert(f.pts_ns<=INT64_C(3000000000) && f.pts_ns+f.duration_ns>INT64_C(3000000000));

  /* A video stream is not an audio track; neither is a missing index. */
  assert(dts_engine_open(e,dub,HEADERS,0,0,0)<0);
  assert(strstr(dts_engine_error(e),"External audio track absent"));
  assert(dts_engine_open(e,dub,HEADERS,9,0,0)<0);

  if(argc==5) {
    /* No decoder for it in this build and no way to declare it: refused, so
     * the worker can return to the movie's own audio. */
    assert(dts_engine_open(e,argv[4],HEADERS,-1,0,0)<0);
    assert(strstr(dts_engine_error(e),"External audio codec unsupported"));
  }
  dts_engine_destroy(e);
  puts("External audio engine roles: video-only, audio-only passthrough (E-AC-3, AC-3, AAC-LC), seek, own read-ahead passed.");
  return 0;
}
