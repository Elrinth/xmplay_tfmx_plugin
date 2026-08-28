/*
 * Host-side detect / open / duration / render / seek tests for xmp-tfmx 1.0.3.
 * A file is one playlist item. Process must not return 0 before the cap.
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

static int ieq(const char *a, const char *b)
{
  if (!a || !b) return 0;
  while (*a && *b) {
    unsigned char ca = (unsigned char)*a++;
    unsigned char cb = (unsigned char)*b++;
    if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
    if (ca != cb) return 0;
  }
  return *a == 0 && *b == 0;
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

static int check_title_not_mod(tfmx_player *p, const char *expect_stem)
{
  const char *title = tfmx_player_title(p);
  const char *name = tfmx_player_name(p);
  printf("title='%s' name='%s'\n", title, name);
  if (ieq(title, "mod") || ieq(name, "mod")) {
    fprintf(stderr, "FAIL: title/name is 'mod' (temp-file leak)\n");
    g_fail++;
    return -1;
  }
  if (ieq(title, "(Empty)") || ieq(name, "(Empty)")) {
    fprintf(stderr, "FAIL: title/name is '(Empty)'\n");
    g_fail++;
    return -1;
  }
  if (expect_stem && !ieq(title, expect_stem) && !ieq(name, expect_stem)) {
    fprintf(stderr, "FAIL: expected stem '%s' in title or name\n", expect_stem);
    g_fail++;
    return -1;
  }
  return 0;
}

/* 576-frame XMPlay-sized chunks. Any Process==0 before want_sec is a fail
 * unless pos_ms has already reached the advertised duration. */
static int check_process_no_ret0(tfmx_player *p, double want_sec, const char *label)
{
  float chunk[576 * 2];
  int rate = tfmx_player_rate(p);
  int want = (int)(want_sec * rate + 0.5);
  int got = 0, heard = 0, ret0 = 0;
  int n, i, peak16;
  int cap = tfmx_player_duration_ms(p, 0);
  int start_pos = tfmx_player_position_ms(p);

  while (got < want) {
    n = tfmx_player_process(p, chunk, (int)(sizeof chunk / sizeof chunk[0]));
    if (n <= 0) {
      ret0 = 1;
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
  printf("  %s: frames=%d heard=%d pos=%d->%d ended=%d ret0=%d cap=%d\n",
         label, got, heard, start_pos, tfmx_player_position_ms(p),
         tfmx_player_ended(p), ret0, cap);
  if (ret0) {
    int pos = tfmx_player_position_ms(p);
    if (cap > 0 && pos >= cap - 50) {
      printf("  (ret0 at cap — OK)\n");
    } else {
      fprintf(stderr, "FAIL: Process returned 0 during %s (pos=%d cap=%d)\n",
              label, pos, cap);
      g_fail++;
      return -1;
    }
  }
  if (got < (int)(0.5 * rate) && want >= rate) {
    fprintf(stderr, "FAIL: %s rendered too little (frames=%d)\n", label, got);
    g_fail++;
    return -1;
  }
  if (heard < rate / 8 && want >= rate) {
    fprintf(stderr, "FAIL: %s not enough audio (heard=%d)\n", label, heard);
    g_fail++;
    return -1;
  }
  return 0;
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
  printf("GetFileInfo equiv: n=%d duration0=%d title='%s' name='%s' fmt='%s'\n",
         inf.songs, inf.duration_ms[0], inf.title, inf.name, inf.format_name);
  if (inf.songs != 1) {
    fprintf(stderr, "FAIL: user-song GetFileInfo n=%d want 1\n", inf.songs);
    g_fail++;
  }
  if (inf.duration_ms[0] < 41500 || inf.duration_ms[0] > 42500) {
    fprintf(stderr, "FAIL: user-song duration %d not ~42020 ms\n", inf.duration_ms[0]);
    g_fail++;
  }

  p = tfmx_player_open(tfx, mdat, mlen, smpl, slen);
  if (!p)
    p = tfmx_player_open(tfx, mdat, mlen, NULL, 0);
  if (!p) {
    fprintf(stderr, "FAIL: open user-song\n");
    g_fail++;
    free(mdat);
    free(smpl);
    return -1;
  }

  dur = tfmx_player_duration_ms(p, 0);
  printf("open: songs_exposed=%d voices=%d duration_ms=%d\n",
         tfmx_player_songs(p), tfmx_player_voices(p), dur);
  check_title_not_mod(p, "user-song");
  if (tfmx_player_songs(p) != 1) {
    fprintf(stderr, "FAIL: user-song songs=%d want 1\n", tfmx_player_songs(p));
    g_fail++;
  }
  if (dur < 41500 || dur > 42500) {
    fprintf(stderr, "FAIL: user-song duration_ms=%d want ~42020\n", dur);
    g_fail++;
  }

  frames = render_sec(p, 2.0, &rms, &peak);
  printf("render 2s: frames=%d rms=%.6f peak=%.6f\n", frames, rms, peak);
  if (frames < 1000 || rms < 1e-6) {
    fprintf(stderr, "FAIL: first 2s silent or short (rms=%g)\n", rms);
    g_fail++;
  }

  tfmx_player_seek_ms(p, 0);
  check_process_no_ret0(p, 10.0, "user-song first 10s");

  seek_to = dur > 4000 ? dur / 2 : 1000;
  pos = tfmx_player_seek_ms(p, seek_to);
  printf("seek to %d -> %d\n", seek_to, pos);
  frames = render_sec(p, 1.0, &rms, &peak);
  printf("render after seek: frames=%d rms=%.6f peak=%.6f\n", frames, rms, peak);
  if (frames < 200 || rms < 1e-6) {
    fprintf(stderr, "FAIL: post-seek silent (rms=%g)\n", rms);
    g_fail++;
  }

  tfmx_player_close(p);
  free(mdat);
  free(smpl);
  return 0;
}

static int test_user_mod(void)
{
  const char *tfx = "tests/samples/user-mod.tfx";
  const char *sam = "tests/samples/user-mod.sam";
  unsigned char *mdat, *smpl;
  size_t mlen, slen;
  tfmx_player *p;
  tfmx_info inf;
  double rms, peak;
  int dur, pos, frames;
  const int expect_ms = 406240;

  printf("==== user-mod.tfx ====\n");
  mdat = slurp(tfx, &mlen);
  if (!mdat) {
    printf("SKIP: no %s\n", tfx);
    return 0;
  }
  smpl = slurp(sam, &slen);
  if (!smpl) {
    printf("SKIP: no %s\n", sam);
    free(mdat);
    return 0;
  }
  printf("mdat bytes: %zu  sam bytes: %zu  probe=%d\n",
         mlen, slen, tfmx_probe(mdat, mlen));

  if (tfmx_analyze(tfx, mdat, mlen, smpl, slen, &inf) != 0) {
    fprintf(stderr, "FAIL: analyze user-mod\n");
    g_fail++;
    free(mdat);
    free(smpl);
    return -1;
  }
  printf("GetFileInfo equiv: n=%d duration0=%d title='%s' name='%s'\n",
         inf.songs, inf.duration_ms[0], inf.title, inf.name);
  if (inf.songs != 1) {
    fprintf(stderr, "FAIL: user-mod GetFileInfo n=%d want 1 (no NSF split)\n", inf.songs);
    g_fail++;
  }
  if (inf.duration_ms[0] < expect_ms - 500 || inf.duration_ms[0] > expect_ms + 500) {
    fprintf(stderr, "FAIL: user-mod duration %d want %d\n", inf.duration_ms[0], expect_ms);
    g_fail++;
  }
  if (ieq(inf.title, "mod") || ieq(inf.name, "mod")) {
    fprintf(stderr, "FAIL: analyze title/name is 'mod'\n");
    g_fail++;
  }

  p = tfmx_player_open(tfx, mdat, mlen, smpl, slen);
  if (!p) {
    fprintf(stderr, "FAIL: open user-mod\n");
    g_fail++;
    free(mdat);
    free(smpl);
    return -1;
  }
  dur = tfmx_player_duration_ms(p, 0);
  printf("open: songs_exposed=%d duration_ms=%d (%d:%02d)\n",
         tfmx_player_songs(p), dur, dur / 60000, (dur / 1000) % 60);
  check_title_not_mod(p, "user-mod");
  if (tfmx_player_songs(p) != 1) {
    fprintf(stderr, "FAIL: user-mod songs_exposed=%d want 1\n", tfmx_player_songs(p));
    g_fail++;
  }
  if (dur < expect_ms - 500 || dur > expect_ms + 500) {
    fprintf(stderr, "FAIL: user-mod duration_ms=%d want %d\n", dur, expect_ms);
    g_fail++;
  }

  /* First 15s of the chained stream — no Process 0. */
  tfmx_player_seek_ms(p, 0);
  check_process_no_ret0(p, 15.0, "user-mod first 15s");

  /* Seek 3:00 (slot 3 of the chain) */
  pos = tfmx_player_seek_ms(p, 3 * 60 * 1000);
  printf("seek 3:00 -> %d\n", pos);
  frames = render_sec(p, 2.0, &rms, &peak);
  printf("  after 3:00: frames=%d rms=%.6f peak=%.6f pos=%d\n",
         frames, rms, peak, tfmx_player_position_ms(p));
  if (frames < 200 || rms < 1e-6) {
    fprintf(stderr, "FAIL: silent after seek 3:00 (rms=%g)\n", rms);
    g_fail++;
  }
  check_process_no_ret0(p, 3.0, "user-mod after 3:00");

  /* Seek 6:00 (slot 6 of the chain) */
  pos = tfmx_player_seek_ms(p, 6 * 60 * 1000);
  printf("seek 6:00 -> %d\n", pos);
  frames = render_sec(p, 2.0, &rms, &peak);
  printf("  after 6:00: frames=%d rms=%.6f peak=%.6f pos=%d\n",
         frames, rms, peak, tfmx_player_position_ms(p));
  if (frames < 200 || rms < 1e-6) {
    fprintf(stderr, "FAIL: silent after seek 6:00 (rms=%g)\n", rms);
    g_fail++;
  }
  check_process_no_ret0(p, 3.0, "user-mod after 6:00");

  /* Cross first slot boundary (~44.8s) without ret0. */
  pos = tfmx_player_seek_ms(p, 44000);
  printf("seek 44.0s -> %d\n", pos);
  check_process_no_ret0(p, 3.0, "user-mod slot-boundary 44s");

  tfmx_player_close(p);
  free(mdat);
  free(smpl);
  return 0;
}

int main(void)
{
  test_user_song();
  test_user_mod();
  if (g_fail) {
    fprintf(stderr, "FAILED %d check(s)\n", g_fail);
    return 1;
  }
  printf("OK\n");
  return 0;
}
