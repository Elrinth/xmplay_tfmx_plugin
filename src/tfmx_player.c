/*
 * TFMX (Hülsbeck) player wrapper around libtfmxaudiodecoder.
 */
#include "tfmx_player.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#ifdef _WIN32
# include <windows.h>
# include <direct.h>
# include <io.h>
# define MKDIR(p) _mkdir(p)
#else
# include <unistd.h>
# include <sys/stat.h>
# include <sys/types.h>
# define MKDIR(p) mkdir(p, 0700)
#endif

#include "tfmxaudiodecoder.h"

#define MIX_PREC   16
#define MIX_CHAN   2
#define MIX_ZERO   0
#define MIX_PAN    75
#define SCRATCH    16384

struct tfmx_player {
  void *dec;
  int rate;
  int songs;
  int song;
  int voices;
  int ended;
  int pos_ms;
  int duration_ms[TFMX_MAX_SONGS];
  int samples_acc; /* leftover samples toward next ms */
  char title[TFMX_STR];
  char artist[TFMX_STR];
  char name[TFMX_STR];
  char format_id[64];
  char format_name[128];
  char temp_dir[TFMX_PATH];
  char temp_tfx[TFMX_PATH];
  char temp_sam[TFMX_PATH];
  unsigned char scratch[SCRATCH];
};

static void bounded_copy(char *dst, size_t cap, const char *src)
{
  size_t n;
  if (!dst || cap == 0)
    return;
  if (!src) { dst[0] = '\0'; return; }
  n = strlen(src);
  if (n >= cap) n = cap - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

static int is_huelsbeck(void *dec)
{
  const char *name;
  const char *id;
  if (!dec)
    return 0;
  name = tfmxdec_format_name(dec);
  id = tfmxdec_format_id(dec);
  if (name && strstr(name, "Huelsbeck"))
    return 1;
  if (name && (strstr(name, "Hippel") || strstr(name, "Future Composer") ||
               strstr(name, "FC14") || strstr(name, "FC13") ||
               strstr(name, "MCMD") || strstr(name, "SMOD")))
    return 0;
  if (id && strcmp(id, "DNS") == 0)
    return 0;
  if (id && strcmp(id, "TFMX") == 0 && name && strstr(name, "Huelsbeck"))
    return 1;
  return 0;
}

int tfmx_probe(const unsigned char *data, size_t len)
{
  void *dec;
  int ok;
  if (!data || len < 5)
    return 0;
  dec = tfmxdec_new();
  if (!dec)
    return 0;
  ok = tfmxdec_detect(dec, (void *)data, (uint32_t)len);
  if (ok)
    ok = is_huelsbeck(dec);
  tfmxdec_delete(dec);
  return ok ? 1 : 0;
}

static void apply_mixer(void *dec)
{
  tfmxdec_mixer_init(dec, TFMX_RATE, MIX_PREC, MIX_CHAN, MIX_ZERO, MIX_PAN);
  tfmxdec_set_loop_mode(dec, 0);
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

/* When the library reports 0 ms, render until song_end or 2s silence (cap 10 min). */
static int measure_if_zero(void *dec)
{
  unsigned char buf[4096];
  int total_ms = 0;
  int silent_ms = 0;
  int heard = 0;
  int frames, chunk_ms, peak;

  apply_mixer(dec);
  tfmxdec_reinit(dec, -1);
  apply_mixer(dec);

  while (total_ms < TFMX_DETECT_CAP_MS) {
    memset(buf, 0, sizeof buf);
    tfmxdec_buffer_fill(dec, buf, (uint32_t)sizeof buf);
    frames = (int)(sizeof buf / 4);
    chunk_ms = (frames * 1000) / TFMX_RATE;
    if (chunk_ms < 1) chunk_ms = 1;
    peak = pcm_peak16((const int16_t *)buf, frames * 2);
    if (peak >= 24) {
      heard = 1;
      silent_ms = 0;
    } else {
      silent_ms += chunk_ms;
    }
    total_ms += chunk_ms;
    if (tfmxdec_song_end(dec))
      break;
    if (heard && silent_ms >= TFMX_SILENCE_MS) {
      total_ms -= silent_ms;
      if (total_ms < 1) total_ms = 1;
      break;
    }
  }
  if (total_ms < 1)
    total_ms = 1;
  tfmxdec_reinit(dec, -1);
  apply_mixer(dec);
  return total_ms;
}

static int song_duration(void *dec)
{
  uint32_t d = tfmxdec_duration(dec);
  if (d > 0)
    return (int)d;
  return measure_if_zero(dec);
}

static void fill_meta(void *dec, char *title, char *artist, char *name,
                      char *fid, char *fname)
{
  const char *t, *a, *n, *id, *fn;
  t = tfmxdec_get_title(dec);
  a = tfmxdec_get_artist(dec);
  n = tfmxdec_get_name(dec);
  id = tfmxdec_format_id(dec);
  fn = tfmxdec_format_name(dec);
  if (!t || !t[0]) t = n;
  bounded_copy(title, TFMX_STR, t);
  bounded_copy(artist, TFMX_STR, a);
  bounded_copy(name, TFMX_STR, n);
  bounded_copy(fid, 64, id);
  bounded_copy(fname, 128, fn);
}

static int write_file(const char *path, const unsigned char *data, size_t len)
{
  FILE *f;
  size_t w;
  f = fopen(path, "wb");
  if (!f)
    return 0;
  w = fwrite(data, 1, len, f);
  fclose(f);
  return w == len;
}

static void make_temp_pair(tfmx_player *p,
                           const unsigned char *mdat, size_t mdat_len,
                           const unsigned char *smpl, size_t smpl_len)
{
#ifdef _WIN32
  char base[MAX_PATH];
  DWORD n;
  n = GetTempPathA((DWORD)sizeof base, base);
  if (n == 0 || n >= sizeof base)
    return;
  snprintf(p->temp_dir, sizeof p->temp_dir, "%sxmp-tfmx-%u", base, (unsigned)GetCurrentProcessId());
#else
  snprintf(p->temp_dir, sizeof p->temp_dir, "/tmp/xmp-tfmx-%d", (int)getpid());
#endif
  MKDIR(p->temp_dir);
  snprintf(p->temp_tfx, sizeof p->temp_tfx, "%s/mod.tfx", p->temp_dir);
  snprintf(p->temp_sam, sizeof p->temp_sam, "%s/mod.sam", p->temp_dir);
  if (!write_file(p->temp_tfx, mdat, mdat_len)) {
    p->temp_dir[0] = '\0';
    return;
  }
  if (smpl && smpl_len) {
    if (!write_file(p->temp_sam, smpl, smpl_len)) {
      p->temp_dir[0] = '\0';
      return;
    }
  }
}

static void cleanup_temp(tfmx_player *p)
{
  if (!p || !p->temp_dir[0])
    return;
  if (p->temp_tfx[0]) remove(p->temp_tfx);
  if (p->temp_sam[0]) remove(p->temp_sam);
#ifdef _WIN32
  _rmdir(p->temp_dir);
#else
  rmdir(p->temp_dir);
#endif
  p->temp_dir[0] = '\0';
}

static int init_decoder(void *dec, const char *path,
                        const unsigned char *mdat, size_t mdat_len)
{
  int ok;
  if (path && path[0])
    tfmxdec_set_path(dec, path);
  tfmxdec_set_loop_mode(dec, 0);
  ok = 0;
  if (mdat && mdat_len)
    ok = tfmxdec_init(dec, (void *)mdat, (uint32_t)mdat_len, 0);
  if (!ok && path && path[0])
    ok = tfmxdec_load(dec, path, 0);
  if (!ok)
    return 0;
  if (!is_huelsbeck(dec))
    return 0;
  apply_mixer(dec);
  return 1;
}

static void snapshot_durations(void *dec, int *out, int *nsongs)
{
  int n, i, cur;
  n = tfmxdec_songs(dec);
  if (n < 1) n = 1;
  if (n > TFMX_MAX_SONGS) n = TFMX_MAX_SONGS;
  *nsongs = n;
  cur = 0;
  for (i = 0; i < n; ++i) {
    if (i != cur) {
      if (!tfmxdec_reinit(dec, i)) {
        out[i] = 0;
        continue;
      }
      apply_mixer(dec);
      cur = i;
    }
    out[i] = song_duration(dec);
  }
  if (cur != 0) {
    tfmxdec_reinit(dec, 0);
    apply_mixer(dec);
  }
}

tfmx_player *tfmx_player_open(const char *path,
                              const unsigned char *mdat, size_t mdat_len,
                              const unsigned char *smpl, size_t smpl_len)
{
  tfmx_player *p;
  const char *use_path = path;
  int i;

  p = (tfmx_player *)calloc(1, sizeof *p);
  if (!p)
    return NULL;
  p->rate = TFMX_RATE;
  p->dec = tfmxdec_new();
  if (!p->dec) {
    free(p);
    return NULL;
  }

  if (smpl && smpl_len && mdat && mdat_len) {
    make_temp_pair(p, mdat, mdat_len, smpl, smpl_len);
    if (p->temp_tfx[0])
      use_path = p->temp_tfx;
  }

  if (!init_decoder(p->dec, use_path, mdat, mdat_len)) {
    /* last resort: temp tfx only (library finds sibling .sam) */
    if (use_path != path && path && path[0]) {
      if (init_decoder(p->dec, path, mdat, mdat_len))
        goto ok;
    }
    tfmxdec_delete(p->dec);
    cleanup_temp(p);
    free(p);
    return NULL;
  }

ok:
  snapshot_durations(p->dec, p->duration_ms, &p->songs);
  p->song = 0;
  p->voices = tfmxdec_voices(p->dec);
  p->ended = 0;
  p->pos_ms = 0;
  fill_meta(p->dec, p->title, p->artist, p->name, p->format_id, p->format_name);
  if (!p->title[0] && p->name[0])
    bounded_copy(p->title, sizeof p->title, p->name);
  (void)i;
  return p;
}

void tfmx_player_close(tfmx_player *p)
{
  if (!p)
    return;
  if (p->dec)
    tfmxdec_delete(p->dec);
  cleanup_temp(p);
  free(p);
}

int tfmx_player_songs(const tfmx_player *p) { return p ? p->songs : 0; }
int tfmx_player_song(const tfmx_player *p) { return p ? p->song : 0; }
int tfmx_player_rate(const tfmx_player *p) { return p ? p->rate : TFMX_RATE; }
int tfmx_player_voices(const tfmx_player *p) { return p ? p->voices : 0; }
int tfmx_player_ended(const tfmx_player *p) { return p ? p->ended : 1; }
int tfmx_player_position_ms(const tfmx_player *p) { return p ? p->pos_ms : 0; }

int tfmx_player_duration_ms(const tfmx_player *p, int song0)
{
  if (!p || song0 < 0 || song0 >= p->songs)
    return 0;
  return p->duration_ms[song0];
}

int tfmx_player_total_duration_ms(const tfmx_player *p)
{
  int i, t = 0;
  if (!p)
    return 0;
  for (i = 0; i < p->songs; ++i)
    t += p->duration_ms[i];
  return t;
}

int tfmx_player_set_song(tfmx_player *p, int song0)
{
  if (!p || !p->dec)
    return -1;
  if (song0 < 0 || song0 >= p->songs)
    return -1;
  if (!tfmxdec_reinit(p->dec, song0))
    return -1;
  apply_mixer(p->dec);
  p->song = song0;
  p->ended = 0;
  p->pos_ms = 0;
  p->samples_acc = 0;
  p->duration_ms[song0] = song_duration(p->dec);
  fill_meta(p->dec, p->title, p->artist, p->name, p->format_id, p->format_name);
  return 0;
}

int tfmx_player_seek_ms(tfmx_player *p, int ms)
{
  int cap;
  if (!p || !p->dec)
    return -1;
  if (ms < 0)
    ms = 0;
  cap = p->duration_ms[p->song];
  if (cap > 0 && ms > cap)
    ms = cap;
  tfmxdec_seek(p->dec, ms);
  p->pos_ms = ms;
  p->samples_acc = 0;
  p->ended = 0;
  return p->pos_ms;
}

int tfmx_player_process(tfmx_player *p, float *stereo, int count)
{
  int frames, bytes, i, nsamp, cap;
  const int16_t *s;

  if (!p || !p->dec || !stereo || p->ended)
    return 0;
  frames = count / 2;
  if (frames <= 0)
    return 0;
  bytes = frames * 4;
  if (bytes > SCRATCH)
    bytes = SCRATCH;
  frames = bytes / 4;
  memset(p->scratch, 0, (size_t)bytes);
  tfmxdec_buffer_fill(p->dec, p->scratch, (uint32_t)bytes);
  nsamp = frames * 2;
  s = (const int16_t *)p->scratch;
  for (i = 0; i < nsamp; ++i)
    stereo[i] = (float)s[i] * (1.0f / 32768.0f);

  p->samples_acc += frames;
  while (p->samples_acc >= p->rate / 1000 && p->rate >= 1000) {
    /* accumulate whole milliseconds */
    int step = p->rate / 1000;
    int nms = p->samples_acc / step;
    p->pos_ms += nms;
    p->samples_acc -= nms * step;
  }
  /* leftover-safe: also bump from frames if rate not divisible — keep simple */
  cap = p->duration_ms[p->song];
  if (tfmxdec_song_end(p->dec))
    p->ended = 1;
  if (cap > 0 && p->pos_ms >= cap)
    p->ended = 1;
  return frames * 2;
}

const char *tfmx_player_title(const tfmx_player *p) { return p ? p->title : ""; }
const char *tfmx_player_artist(const tfmx_player *p) { return p ? p->artist : ""; }
const char *tfmx_player_name(const tfmx_player *p) { return p ? p->name : ""; }
const char *tfmx_player_format_id(const tfmx_player *p) { return p ? p->format_id : ""; }
const char *tfmx_player_format_name(const tfmx_player *p) { return p ? p->format_name : ""; }

int tfmx_analyze(const char *path,
                 const unsigned char *mdat, size_t mdat_len,
                 const unsigned char *smpl, size_t smpl_len,
                 tfmx_info *out)
{
  tfmx_player *p;
  int i;
  if (!out)
    return -1;
  memset(out, 0, sizeof *out);
  p = tfmx_player_open(path, mdat, mdat_len, smpl, smpl_len);
  if (!p)
    return -1;
  out->songs = p->songs;
  out->voices = p->voices;
  for (i = 0; i < p->songs && i < TFMX_MAX_SONGS; ++i)
    out->duration_ms[i] = p->duration_ms[i];
  bounded_copy(out->title, sizeof out->title, p->title);
  bounded_copy(out->artist, sizeof out->artist, p->artist);
  bounded_copy(out->name, sizeof out->name, p->name);
  bounded_copy(out->format_id, sizeof out->format_id, p->format_id);
  bounded_copy(out->format_name, sizeof out->format_name, p->format_name);
  tfmx_player_close(p);
  return 0;
}

