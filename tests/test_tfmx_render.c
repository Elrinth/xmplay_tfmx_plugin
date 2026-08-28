/*
 * Host-side detect / open / duration / render / seek tests for xmp-tfmx.
 */
#include "tfmx_player.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

static int g_fail;

static unsigned char *slurp(const char *path, size_t *out_len)
{
  FILE *f;
  unsigned char *buf;
  long sz;
  *out_len = 0;
  f = fopen(path, "rb");
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
  sz = ftell(f);
  if (sz < 1) { fclose(f); return NULL; }
  rewind(f);
  buf = (unsigned char *)malloc((size_t)sz);
  if (!buf) { fclose(f); return NULL; }
  if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
    free(buf); fclose(f); return NULL;
  }
  fclose(f);
  *out_len = (size_t)sz;
  return buf;
}

static void rms_peak(const float *s, int frames, double *rms, double *peak)
{
  double acc = 0.0, pk = 0.0;
  int i, n = frames * 2;
  for (i = 0; i < n; ++i) {
    double v = s[i];
    acc += v * v;
    if (v < 0) v = -v;
    if (v > pk) pk = v;
  }
  *rms = n > 0 ? sqrt(acc / n) : 0.0;
  *peak = pk;
}

static int render_sec(tfmx_player *p, double sec, double *rms, double *peak)
{
  int rate = tfmx_player_rate(p);
  int need = (int)(sec * rate + 0.5);
  int got = 0;
  float *buf;
  if (need < 64) need = 64;
  buf = (float *)calloc((size_t)need * 2, sizeof(float));
  if (!buf) return -1;
  while (got < need) {
    int n = tfmx_player_process(p, buf + got * 2, (need - got) * 2);
    if (n <= 0) break;
    got += n / 2;
  }
  rms_peak(buf, got, rms, peak);
  free(buf);
  return got;
}

static int test_user_song(void)
{
  const char *tfx = "tests/samples/user-song.tfx";
  const char *sam = "tests/samples/user-song.sam";
  unsigned char *mdat, *smpl;
  size_t mlen, slen;
  tfmx_player *p;
  tfmx_info inf;
  double rms, peak;
  int frames, det, dur, seek_to, pos;

  printf("==== user-song.tfx ====\n");
  mdat = slurp(tfx, &mlen);
  if (!mdat) {
    fprintf(stderr, "FAIL: cannot read %s\n", tfx);
    g_fail++;
    return -1;
  }
  printf("mdat bytes: %zu\n", mlen);
  det = tfmx_probe(mdat, mlen);
  printf("detect: %d\n", det);
  if (det != 1) {
    fprintf(stderr, "FAIL: detect user-song.tfx != 1\n");
    g_fail++;
    free(mdat);
    return -1;
  }

  smpl = slurp(sam, &slen);
  printf("sam bytes: %zu\n", slen);

  if (tfmx_analyze(tfx, mdat, mlen, NULL, 0, &inf) != 0) {
    fprintf(stderr, "FAIL: analyze via path (sidecar on disk)\n");
    g_fail++;
    free(mdat);
    free(smpl);
    return -1;
  }
  printf("analyze path: songs=%d duration0=%d title='%s' name='%s' fmt='%s'\n",
         inf.songs, inf.duration_ms[0], inf.title, inf.name, inf.format_name);
  if (inf.duration_ms[0] <= 0) {
    fprintf(stderr, "FAIL: duration <= 0\n");
    g_fail++;
  }
  if (inf.duration_ms[0] == 180000)
    printf("NOTE: duration is 180000 ms — accepted only if that is the real loop\n");

  p = tfmx_player_open(tfx, mdat, mlen, NULL, 0);
  if (!p) {
    fprintf(stderr, "FAIL: open with path (expect sibling .sam)\n");
    g_fail++;
    /* try with explicit smpl */
    p = tfmx_player_open(tfx, mdat, mlen, smpl, slen);
  }
  if (!p) {
    fprintf(stderr, "FAIL: open with explicit sidecar also failed\n");
    g_fail++;
    free(mdat);
    free(smpl);
    return -1;
  }

  dur = tfmx_player_duration_ms(p, 0);
  printf("open: title='%s' artist='%s' name='%s'\n",
         tfmx_player_title(p), tfmx_player_artist(p), tfmx_player_name(p));
  printf("format: %s (%s)\n", tfmx_player_format_id(p), tfmx_player_format_name(p));
  printf("songs=%d voices=%d duration_ms=%d\n",
         tfmx_player_songs(p), tfmx_player_voices(p), dur);
  if (dur <= 0) {
    fprintf(stderr, "FAIL: duration_ms <= 0\n");
    g_fail++;
  }

  frames = render_sec(p, 2.0, &rms, &peak);
  printf("render 2s: frames=%d rms=%.6f peak=%.6f\n", frames, rms, peak);
  if (frames < 1000 || rms < 1e-6) {
    fprintf(stderr, "FAIL: first 2s silent or short (rms=%g)\n", rms);
    g_fail++;
  }

  /* 10s of intermittent nonzero PCM; song_end in first 500ms must not stop. */
  {
    float chunk[44100 / 50 * 2]; /* ~20ms */
    int rate = tfmx_player_rate(p);
    int want = 10 * rate;
    int got = 0, heard = 0, early_end = 0;
    int n, i, peak16;
    tfmx_player_seek_ms(p, 0);
    while (got < want) {
      n = tfmx_player_process(p, chunk, (int)(sizeof chunk / sizeof chunk[0]));
      if (n <= 0) {
        if (tfmx_player_position_ms(p) < 500)
          early_end = 1;
        break;
      }
      peak16 = 0;
      for (i = 0; i < n; ++i) {
        float v = chunk[i];
        int iv;
        if (v < 0) v = -v;
        iv = (int)(v * 32768.0f);
        if (iv > peak16) peak16 = iv;
      }
      if (peak16 >= 24) heard += n / 2;
      got += n / 2;
    }
    printf("render 10s: frames=%d heard=%d pos=%d ended=%d early_end=%d\n",
           got, heard, tfmx_player_position_ms(p), tfmx_player_ended(p), early_end);
    if (early_end) {
      fprintf(stderr, "FAIL: song_end/process 0 in first 500ms\n");
      g_fail++;
    }
    if (got < 8 * rate) {
      fprintf(stderr, "FAIL: less than ~8s rendered (frames=%d)\n", got);
      g_fail++;
    }
    if (heard < rate) { /* at least ~1s of audible samples across 10s */
      fprintf(stderr, "FAIL: not enough intermittent audio in 10s (heard=%d)\n", heard);
      g_fail++;
    }
  }

  seek_to = dur > 4000 ? dur / 2 : 1000;
  pos = tfmx_player_seek_ms(p, seek_to);
  printf("seek to %d -> %d\n", seek_to, pos);
  if (pos < 0) {
    fprintf(stderr, "FAIL: seek failed\n");
    g_fail++;
  }
  frames = render_sec(p, 1.0, &rms, &peak);
  printf("render after seek: frames=%d rms=%.6f peak=%.6f\n", frames, rms, peak);
  if (frames < 200 || rms < 1e-6) {
    fprintf(stderr, "FAIL: post-seek silent (rms=%g)\n", rms);
    g_fail++;
  }

  if (tfmx_player_songs(p) > 1) {
    if (tfmx_player_set_song(p, 1) != 0) {
      fprintf(stderr, "FAIL: set_song 1\n");
      g_fail++;
    } else {
      frames = render_sec(p, 0.5, &rms, &peak);
      printf("track 2: duration=%d frames=%d rms=%.6f\n",
             tfmx_player_duration_ms(p, 1), frames, rms);
    }
  }

  tfmx_player_close(p);
  free(mdat);
  free(smpl);
  return 0;
}

int main(void)
{
  test_user_song();
  if (g_fail) {
    fprintf(stderr, "FAILED %d check(s)\n", g_fail);
    return 1;
  }
  printf("OK\n");
  return 0;
}
