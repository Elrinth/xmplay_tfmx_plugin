/*
 * Diagnose every TFMX song slot (raw decoder + tfmx_player / XMPlay Process).
 */
#include "tfmx_player.h"
#include "tfmxaudiodecoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define RATE     44100
#define CHUNK    576          /* XMPlay-ish frames */
#define FLOATS   (CHUNK * 2)
#define SCAN_MS  30000       /* 3 min scan cap for audio stats */
#define END_SCAN 20000       /* 2 min to find first song_end */

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

static int pcm_peak16(const int16_t *s, int nsamp)
{
  int i, peak = 0;
  for (i = 0; i < nsamp; ++i) {
    int v = s[i];
    if (v < 0) v = -v;
    if (v > peak) peak = v;
  }
  return peak;
}

static int first_song_end_ms(void *dec, int loop, int cap_ms)
{
  unsigned char buf[CHUNK * 4];
  int total = 0;
  tfmxdec_reinit(dec, -1);
  tfmxdec_mixer_init(dec, RATE, 16, 2, 0, 75);
  tfmxdec_set_loop_mode(dec, loop);
  while (total < cap_ms) {
    memset(buf, 0, sizeof buf);
    tfmxdec_buffer_fill(dec, buf, (uint32_t)sizeof buf);
    total += (CHUNK * 1000) / RATE;
    if (tfmxdec_song_end(dec))
      return total;
  }
  return -1;
}

static void scan_audio(void *dec, int loop, int cap_ms,
                       int *first_peak, int *last_peak, int *heard_ms,
                       int *peak_max, int *process0_ms)
{
  unsigned char buf[CHUNK * 4];
  int total = 0;
  *first_peak = -1;
  *last_peak = -1;
  *heard_ms = 0;
  *peak_max = 0;
  *process0_ms = -1;
  tfmxdec_reinit(dec, -1);
  tfmxdec_mixer_init(dec, RATE, 16, 2, 0, 75);
  tfmxdec_set_loop_mode(dec, loop);
  while (total < cap_ms) {
    int peak, chunk_ms;
    memset(buf, 0, sizeof buf);
    tfmxdec_buffer_fill(dec, buf, (uint32_t)sizeof buf);
    peak = pcm_peak16((const int16_t *)buf, CHUNK * 2);
    chunk_ms = (CHUNK * 1000) / RATE;
    if (chunk_ms < 1) chunk_ms = 1;
    if (peak > *peak_max) *peak_max = peak;
    if (peak >= 24) {
      if (*first_peak < 0) *first_peak = total;
      *last_peak = total;
      *heard_ms += chunk_ms;
    }
    total += chunk_ms;
    if (*process0_ms < 0 && tfmxdec_song_end(dec) && loop == 0)
      *process0_ms = total;
  }
}

static void diag_raw(const char *label, const char *tfx,
                     const unsigned char *mdat, size_t mlen)
{
  void *dec;
  int n, i, ok;
  int durs[32];
  int sum = 0;

  printf("\n======== RAW DECODER: %s ========\n", label);
  dec = tfmxdec_new();
  if (!dec) { printf("tfmxdec_new failed\n"); return; }
  tfmxdec_set_path(dec, tfx);
  tfmxdec_set_loop_mode(dec, 1);
  ok = tfmxdec_init(dec, (void *)mdat, (uint32_t)mlen, 0);
  printf("init=%d format='%s' id='%s'\n", ok,
         tfmxdec_format_name(dec) ? tfmxdec_format_name(dec) : "",
         tfmxdec_format_id(dec) ? tfmxdec_format_id(dec) : "");
  printf("title='%s' artist='%s' name='%s'\n",
         tfmxdec_get_title(dec) ? tfmxdec_get_title(dec) : "",
         tfmxdec_get_artist(dec) ? tfmxdec_get_artist(dec) : "",
         tfmxdec_get_name(dec) ? tfmxdec_get_name(dec) : "");
  if (!ok) { tfmxdec_delete(dec); return; }
  n = tfmxdec_songs(dec);
  printf("tfmxdec_songs=%d voices=%d\n", n, tfmxdec_voices(dec));

  memset(durs, 0, sizeof durs);
  for (i = 0; i < n && i < 32; ++i) {
    uint32_t lib_dur;
    int end0, end1;
    int fp, lp, heard, pmax, p0;
    if (i > 0) {
      if (!tfmxdec_reinit(dec, i)) {
        printf("song %d: reinit FAILED\n", i);
        durs[i] = 0;
        continue;
      }
    }
    tfmxdec_mixer_init(dec, RATE, 16, 2, 0, 75);
    lib_dur = tfmxdec_duration(dec);
    durs[i] = (int)lib_dur;
    sum += (int)lib_dur;
    printf("\n--- song %d ---\n", i);
    printf("  lib_duration_ms=%u (%.1fs)\n", lib_dur, lib_dur / 1000.0);
    end0 = first_song_end_ms(dec, 0, END_SCAN);
    end1 = first_song_end_ms(dec, 1, END_SCAN);
    printf("  first song_end loop0=%d ms  loop1=%d ms\n", end0, end1);
    scan_audio(dec, 1, SCAN_MS, &fp, &lp, &heard, &pmax, &p0);
    printf("  audio loop1 (scan %ds): first_peak>=24 @ %d ms, last @ %d ms, heard_ms=%d, peak_max=%d\n",
           SCAN_MS / 1000, fp, lp, heard, pmax);
    {
      int sustain = (fp >= 0 && lp >= 0) ? (lp - fp) : -1;
      int keep = (fp >= 0 && sustain >= 2000) || (heard >= 2000);
      printf("  sustain_span=%d ms  KEEP_AS_REAL=%s\n", sustain, keep ? "YES" : "NO (SFX/empty)");
    }
  }
  printf("\nRAW duration table: ");
  for (i = 0; i < n && i < 32; ++i)
    printf("%d%s", durs[i], i + 1 < n ? ", " : "");
  printf("\nRAW sum=%d ms (%.1fs = %d:%02d)\n",
         sum, sum / 1000.0, sum / 60000, (sum / 1000) % 60);
  tfmxdec_delete(dec);
}

static void diag_player(const char *label, const char *tfx,
                        const unsigned char *mdat, size_t mlen,
                        const unsigned char *smpl, size_t slen)
{
  tfmx_player *p;
  int n, i, sum = 0;
  float buf[FLOATS];

  printf("\n======== TFMX_PLAYER / XMPlay Process: %s ========\n", label);
  p = tfmx_player_open(tfx, mdat, mlen, smpl, slen);
  if (!p) {
    printf("tfmx_player_open FAILED\n");
    return;
  }
  n = tfmx_player_songs(p);
  printf("songs=%d title='%s' name='%s' artist='%s'\n",
         n, tfmx_player_title(p), tfmx_player_name(p), tfmx_player_artist(p));
  printf("format='%s' voices=%d\n", tfmx_player_format_name(p), tfmx_player_voices(p));
  printf("player durations: ");
  for (i = 0; i < n; ++i) {
    int d = tfmx_player_duration_ms(p, i);
    sum += d;
    printf("%d%s", d, i + 1 < n ? ", " : "");
  }
  printf("\nplayer sum=%d ms (%.1fs = %d:%02d)\n",
         sum, sum / 1000.0, sum / 60000, (sum / 1000) % 60);

  for (i = 0; i < n; ++i) {
    int got, frames = 0, heard_frames = 0;
    int first_ret0 = -1, first_peak = -1, last_peak = -1;
    int ended_ms = -1, pos_at_0 = -1;
    int cap, peak_max = 0;
    int want_frames = (int)((long)RATE * 8); /* 8s */

    if (tfmx_player_set_song(p, i) != 0) {
      printf("song %d: set_song FAILED\n", i);
      continue;
    }
    cap = tfmx_player_duration_ms(p, i);
    printf("\n--- player song %d  duration_after_set=%d title='%s' name='%s' ---\n",
           i, cap, tfmx_player_title(p), tfmx_player_name(p));

    while (frames < want_frames) {
      int k, peak16 = 0;
      got = tfmx_player_process(p, buf, FLOATS);
      if (got <= 0) {
        if (first_ret0 < 0) {
          first_ret0 = (frames * 1000) / RATE;
          pos_at_0 = tfmx_player_position_ms(p);
        }
        break;
      }
      for (k = 0; k < got; ++k) {
        float v = buf[k];
        int iv;
        if (v < 0) v = -v;
        iv = (int)(v * 32768.0f);
        if (iv > peak16) peak16 = iv;
      }
      if (peak16 > peak_max) peak_max = peak16;
      if (peak16 >= 24) {
        if (first_peak < 0) first_peak = (frames * 1000) / RATE;
        last_peak = (frames * 1000) / RATE;
        heard_frames += got / 2;
      }
      frames += got / 2;
      if (tfmx_player_ended(p) && ended_ms < 0)
        ended_ms = tfmx_player_position_ms(p);
    }
    printf("  Process 8s sim (576-frame chunks): frames=%d (~%dms) first_ret0=%d ms pos_at_0=%d ended_flag@%d\n",
           frames, (frames * 1000) / RATE, first_ret0, pos_at_0, ended_ms);
    printf("  first_peak@%d last_peak@%d heard_ms=%d peak_max=%d pos_ms=%d ended=%d\n",
           first_peak, last_peak, (heard_frames * 1000) / RATE, peak_max,
           tfmx_player_position_ms(p), tfmx_player_ended(p));
    if (first_ret0 >= 0 && first_ret0 < 500)
      printf("  *** Process returned 0 in first 500ms ***\n");
    if (cap > 0 && first_ret0 >= 0 && first_ret0 + 50 < cap && first_ret0 < 2000)
      printf("  *** Process ended far before advertised duration (%d vs %d) ***\n",
             first_ret0, cap);
  }
  tfmx_player_close(p);
}

static void run_pair(const char *label, const char *tfx, const char *sam)
{
  unsigned char *mdat, *smpl;
  size_t mlen, slen;
  printf("\n################ %s ################\n", label);
  mdat = slurp(tfx, &mlen);
  smpl = slurp(sam, &slen);
  printf("tfx=%s (%zu B)  sam=%s (%zu B)\n", tfx, mlen, sam, slen);
  if (!mdat) { printf("cannot read tfx\n"); return; }
  printf("probe=%d\n", tfmx_probe(mdat, mlen));
  diag_raw(label, tfx, mdat, mlen);
  diag_player(label, tfx, mdat, mlen, smpl, slen);
  free(mdat);
  free(smpl);
}

int main(void)
{
  run_pair("user-mod",
           "tests/samples/user-mod.tfx",
           "tests/samples/user-mod.sam");
  run_pair("user-song",
           "tests/samples/user-song.tfx",
           "tests/samples/user-song.sam");
  return 0;
}
