/*
 * TFMX (Hülsbeck) player wrapper around libtfmxaudiodecoder.
 *
 * One playlist item per file. Real song-table slots (sustained audio)
 * are chained back-to-back into a single seekable length. One-note /
 * empty / SFX slots are dropped, not NSF tracks.
 *
 * Process never returns 0 until pos_ms >= total duration. song_end is
 * ignored for stopping; decoder errors become silence until the cap.
 *
 * Init matches qmmp-tfmx / libtfmxaudiodecoder: set_path(real .tfx),
 * end_shorts(1, 10), mixer_init, then init from slurped bytes. The
 * library finds the sibling .sam from that real path (Chris/TFMXDecoder
 * — Hülsbeck only; Jochen/TFMX.cpp is Hippel and is not used).
 * A temp pair is written only for memory-only opens (no usable path)
 * when both blobs are already in hand. Stem is never hardcoded "mod".
 */
#include "tfmx_player.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <ctype.h>

#ifdef _WIN32
# include <windows.h>
# include <direct.h>
# include <io.h>
# define MKDIR(p) _mkdir(p)
# define PATH_SEP '\\'
#else
# include <unistd.h>
# include <sys/stat.h>
# include <sys/types.h>
# define MKDIR(p) mkdir(p, 0700)
# define PATH_SEP '/'
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
  int songs;                 /* exposed count: always 1 when open */
  int song;                  /* always 0 */
  int n_slots;               /* real library slots in the chain */
  int slot_of[TFMX_MAX_SONGS];
  int slot_dur[TFMX_MAX_SONGS];
  int slot_start[TFMX_MAX_SONGS];
  int cur_slot;
  int slot_ready;
  int voices;
  int ended;
  int pos_ms;
  int duration_ms;           /* sum of slot_dur — the one advertised length */
  int samples_acc;
  char title[TFMX_STR];
  char artist[TFMX_STR];
  char name[TFMX_STR];
  char stem[TFMX_STR];       /* original filename stem (display + temp) */
  char file_stem[64];        /* sanitized for temp filenames */
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

static int is_blank_title(const char *s)
{
  if (!s)
    return 1;
  while (*s == ' ' || *s == '\t')
    s++;
  if (!s[0])
    return 1;
  if (strncmp(s, "(Empty)", 7) == 0) {
    const char *p = s + 7;
    while (*p == ' ')
      p++;
    if (!*p)
      return 1;
  }
  return 0;
}

static void path_stem(const char *path, char *out, size_t cap)
{
  const char *base;
  const char *slash;
  const char *bslash;
  char tmp[TFMX_PATH];
  char *dot;

  if (!path || !path[0]) {
    bounded_copy(out, cap, "tfmx");
    return;
  }
  slash = strrchr(path, '/');
  bslash = strrchr(path, '\\');
  base = path;
  if (slash && slash + 1 > base)
    base = slash + 1;
  if (bslash && bslash + 1 > base)
    base = bslash + 1;
  bounded_copy(tmp, sizeof tmp, base);
  dot = strrchr(tmp, '.');
  if (dot && dot != tmp)
    *dot = '\0';
  if (!tmp[0])
    bounded_copy(out, cap, "tfmx");
  else
    bounded_copy(out, cap, tmp);
}

static void make_file_stem(const char *stem, char *out, size_t cap)
{
  size_t i, j = 0;
  if (!stem)
    stem = "";
  for (i = 0; stem[i] && j + 1 < cap && j + 1 < 64; ++i) {
    unsigned char c = (unsigned char)stem[i];
    if (isalnum(c) || c == '.' || c == '_' || c == '-')
      out[j++] = (char)c;
    else if (c == ' ' || c == '+')
      out[j++] = '_';
  }
  out[j] = '\0';
  if (!out[0] || strcmp(out, ".") == 0 || strcmp(out, "..") == 0)
    bounded_copy(out, cap, "tfmx");
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
  /* Loop-end is not EOF. Play through to the measured duration. */
  tfmxdec_set_loop_mode(dec, 1);
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

/* Render with loop_mode=1 until 2s of silence (cap 10 min). */
static int measure_until_silence(void *dec)
{
  unsigned char buf[4096];
  int total_ms = 0;
  int silent_ms = 0;
  int heard = 0;
  int last_peak = -1;
  int frames, chunk_ms, peak;

  apply_mixer(dec);
  tfmxdec_reinit(dec, -1);
  apply_mixer(dec);
  tfmxdec_set_loop_mode(dec, 1);

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
      last_peak = total_ms;
    } else {
      silent_ms += chunk_ms;
    }
    total_ms += chunk_ms;
    if (heard && silent_ms >= TFMX_SILENCE_MS) {
      total_ms = last_peak + TFMX_TAIL_MS;
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

/*
 * Classify one decoder slot and pick a play length.
 * Real tune: last peak>=24 at least ~2s after first, or heard_ms >= 2s.
 * SFX / one-note / empty: dropped (real=0).
 */
static void classify_slot(void *dec, int *real, int *duration_ms,
                          int *first_peak, int *last_peak, int *heard_ms)
{
  unsigned char buf[4096];
  int total_ms = 0;
  int lib_dur;
  int frames, chunk_ms, peak;
  int fp = -1, lp = -1, heard = 0;

  *real = 0;
  *duration_ms = 0;
  *first_peak = -1;
  *last_peak = -1;
  *heard_ms = 0;

  apply_mixer(dec);
  tfmxdec_set_loop_mode(dec, 1);
  lib_dur = (int)tfmxdec_duration(dec);

  while (total_ms < TFMX_CLASSIFY_MS) {
    memset(buf, 0, sizeof buf);
    tfmxdec_buffer_fill(dec, buf, (uint32_t)sizeof buf);
    frames = (int)(sizeof buf / 4);
    chunk_ms = (frames * 1000) / TFMX_RATE;
    if (chunk_ms < 1) chunk_ms = 1;
    peak = pcm_peak16((const int16_t *)buf, frames * 2);
    if (peak >= 24) {
      if (fp < 0) fp = total_ms;
      lp = total_ms;
      heard += chunk_ms;
    }
    total_ms += chunk_ms;
  }

  *first_peak = fp;
  *last_peak = lp;
  *heard_ms = heard;

  {
    int sustain = (fp >= 0 && lp >= 0) ? (lp - fp) : 0;
    if (!((fp >= 0 && sustain >= TFMX_SUSTAIN_MS) || heard >= TFMX_HEARD_MIN_MS))
      return; /* SFX / empty */
  }
  *real = 1;

  /* Library duration is a dry-run to first song_end (one loop). Trust it
   * when it is not one-note-short and we already heard sustained audio. */
  if (lib_dur >= TFMX_TINY_MS)
    *duration_ms = lib_dur;
  else
    *duration_ms = measure_until_silence(dec);
}

static void apply_display_names(tfmx_player *p)
{
  const char *t, *a, *n, *id, *fn;
  if (!p || !p->dec)
    return;
  t = tfmxdec_get_title(p->dec);
  a = tfmxdec_get_artist(p->dec);
  n = tfmxdec_get_name(p->dec);
  id = tfmxdec_format_id(p->dec);
  fn = tfmxdec_format_name(p->dec);
  bounded_copy(p->artist, sizeof p->artist, a);
  bounded_copy(p->format_id, sizeof p->format_id, id);
  bounded_copy(p->format_name, sizeof p->format_name, fn);

  /* Name is the original stem, never the temp basename ("mod"). */
  if (p->stem[0])
    bounded_copy(p->name, sizeof p->name, p->stem);
  else if (n && n[0] && strcmp(n, "mod") != 0)
    bounded_copy(p->name, sizeof p->name, n);
  else
    bounded_copy(p->name, sizeof p->name, "tfmx");

  if (!is_blank_title(t))
    bounded_copy(p->title, sizeof p->title, t);
  else
    bounded_copy(p->title, sizeof p->title, p->name);
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
  const char *stem = p->file_stem[0] ? p->file_stem : "tfmx";
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
  snprintf(p->temp_tfx, sizeof p->temp_tfx, "%s%c%s.tfx", p->temp_dir, PATH_SEP, stem);
  snprintf(p->temp_sam, sizeof p->temp_sam, "%s%c%s.sam", p->temp_dir, PATH_SEP, stem);
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
  /* qmmp-tfmx order: set_path(real file), end_shorts, mixer, then init. */
  if (path && path[0])
    tfmxdec_set_path(dec, path);
  tfmxdec_end_shorts(dec, 1, 10);
  apply_mixer(dec);
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

static int switch_to_slot(tfmx_player *p, int idx, int local_ms)
{
  if (!p || idx < 0 || idx >= p->n_slots)
    return 0;
  p->cur_slot = idx;
  if (!p->dec)
    return 0;
  if (!tfmxdec_reinit(p->dec, p->slot_of[idx]))
    return 0;
  apply_mixer(p->dec);
  if (local_ms > 0)
    tfmxdec_seek(p->dec, local_ms);
  return 1;
}

/* Map pos_ms onto the slot chain. Retry reinit if the last one failed. */
static void ensure_slot_for_pos(tfmx_player *p)
{
  int i, want, local, end;
  if (!p || p->n_slots < 1)
    return;
  want = 0;
  for (i = 0; i < p->n_slots; ++i) {
    end = (i + 1 < p->n_slots) ? p->slot_start[i + 1] : p->duration_ms;
    if (p->pos_ms >= p->slot_start[i] && (p->pos_ms < end || i == p->n_slots - 1))
      want = i;
  }
  local = p->pos_ms - p->slot_start[want];
  if (local < 0) local = 0;
  if (want != p->cur_slot || !p->slot_ready) {
    p->slot_ready = switch_to_slot(p, want, local);
    p->cur_slot = want;
  }
}

/* Keep only slots with sustained audio. Chain them as one stream. */
static void snapshot_real_songs(tfmx_player *p)
{
  int n, i, exposed = 0;
  int cur = 0;

  n = tfmxdec_songs(p->dec);
  if (n < 1) n = 1;
  if (n > TFMX_MAX_SONGS) n = TFMX_MAX_SONGS;

  for (i = 0; i < n; ++i) {
    int real = 0, dur = 0, fp = 0, lp = 0, heard = 0;
    if (i != cur) {
      if (!tfmxdec_reinit(p->dec, i))
        continue;
      apply_mixer(p->dec);
      cur = i;
    }
    classify_slot(p->dec, &real, &dur, &fp, &lp, &heard);
    cur = i; /* classify may reinit current */
    if (!real || dur < 1)
      continue;
    p->slot_of[exposed] = i;
    p->slot_dur[exposed] = dur;
    exposed++;
  }

  if (exposed == 0) {
    /* Last resort: still expose decoder song 0 so Open does not fail. */
    if (cur != 0) {
      tfmxdec_reinit(p->dec, 0);
      apply_mixer(p->dec);
    }
    p->slot_of[0] = 0;
    p->slot_dur[0] = (int)tfmxdec_duration(p->dec);
    if (p->slot_dur[0] < TFMX_TINY_MS)
      p->slot_dur[0] = measure_until_silence(p->dec);
    if (p->slot_dur[0] < 1)
      p->slot_dur[0] = 1;
    exposed = 1;
  }

  p->n_slots = exposed;
  p->songs = 1;
  p->song = 0;
  p->duration_ms = 0;
  for (i = 0; i < exposed; ++i) {
    p->slot_start[i] = p->duration_ms;
    p->duration_ms += p->slot_dur[i];
  }
  p->cur_slot = 0;
  p->slot_ready = 0;
}

tfmx_player *tfmx_player_open(const char *path,
                              const unsigned char *mdat, size_t mdat_len,
                              const unsigned char *smpl, size_t smpl_len)
{
  tfmx_player *p;
  const char *use_path = path;

  p = (tfmx_player *)calloc(1, sizeof *p);
  if (!p)
    return NULL;
  p->rate = TFMX_RATE;
  path_stem(path, p->stem, sizeof p->stem);
  make_file_stem(p->stem, p->file_stem, sizeof p->file_stem);

  p->dec = tfmxdec_new();
  if (!p->dec) {
    free(p);
    return NULL;
  }

  /* Real on-disk .tfx: always set_path to THAT file so .sam is the sibling.
   * Temp pair only when there is no usable directory (memory-only) and
   * we already have both blobs. */
  if (path && path[0]) {
    use_path = path;
  } else if (smpl && smpl_len && mdat && mdat_len) {
    make_temp_pair(p, mdat, mdat_len, smpl, smpl_len);
    if (p->temp_tfx[0])
      use_path = p->temp_tfx;
  }

  if (!init_decoder(p->dec, use_path, mdat, mdat_len)) {
    /* Real-path set_path is first. If that missed a shared Set.sam
     * and the host already slurped those bytes, try a temp pair. */
    if (path && path[0] && smpl && smpl_len && mdat && mdat_len && !p->temp_tfx[0]) {
      make_temp_pair(p, mdat, mdat_len, smpl, smpl_len);
      if (p->temp_tfx[0]) {
        tfmxdec_delete(p->dec);
        p->dec = tfmxdec_new();
        if (p->dec && init_decoder(p->dec, p->temp_tfx, mdat, mdat_len))
          goto opened;
      }
    }
    if (p->dec)
      tfmxdec_delete(p->dec);
    cleanup_temp(p);
    free(p);
    return NULL;
  }
opened:
  snapshot_real_songs(p);
  p->slot_ready = switch_to_slot(p, 0, 0);
  p->voices = tfmxdec_voices(p->dec);
  p->ended = 0;
  p->pos_ms = 0;
  p->samples_acc = 0;
  apply_display_names(p);
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
  if (!p || song0 != 0)
    return 0;
  return p->duration_ms;
}

int tfmx_player_total_duration_ms(const tfmx_player *p)
{
  return p ? p->duration_ms : 0;
}

int tfmx_player_set_song(tfmx_player *p, int song0)
{
  if (!p)
    return -1;
  if (song0 != 0)
    return -1;
  p->pos_ms = 0;
  p->samples_acc = 0;
  p->ended = 0;
  p->slot_ready = 0;
  p->cur_slot = 0;
  p->slot_ready = switch_to_slot(p, 0, 0);
  apply_display_names(p);
  return 0;
}

int tfmx_player_seek_ms(tfmx_player *p, int ms)
{
  if (!p)
    return -1;
  if (ms < 0)
    ms = 0;
  if (p->duration_ms > 0 && ms > p->duration_ms)
    ms = p->duration_ms;
  p->pos_ms = ms;
  p->samples_acc = 0;
  p->ended = (p->duration_ms > 0 && ms >= p->duration_ms) ? 1 : 0;
  p->slot_ready = 0;
  if (!p->ended)
    ensure_slot_for_pos(p);
  return p->pos_ms;
}

static void advance_pos(tfmx_player *p, int frames)
{
  if (!p || p->rate < 1000)
    return;
  p->samples_acc += frames;
  while (p->samples_acc >= p->rate / 1000) {
    int step = p->rate / 1000;
    int nms = p->samples_acc / step;
    p->pos_ms += nms;
    p->samples_acc -= nms * step;
  }
}

int tfmx_player_process(tfmx_player *p, float *stereo, int count)
{
  int want_frames, written = 0;

  if (!p || !stereo)
    return 0;
  want_frames = count / 2;
  if (want_frames <= 0)
    return 0;

  if (p->ended || (p->duration_ms > 0 && p->pos_ms >= p->duration_ms)) {
    p->ended = 1;
    return 0;
  }

  /* song_end is ignored for stopping. Decoder errors become silence. */
  while (written < want_frames) {
    int remain_ms, remain_slot, chunk, i, nsamp, max_fr;
    const int16_t *s;

    if (p->duration_ms > 0 && p->pos_ms >= p->duration_ms) {
      p->ended = 1;
      break;
    }

    ensure_slot_for_pos(p);

    remain_ms = (p->duration_ms > 0) ? (p->duration_ms - p->pos_ms) : 0x7fffffff;
    if (remain_ms < 1) {
      p->ended = 1;
      break;
    }
    remain_slot = remain_ms;
    if (p->cur_slot + 1 < p->n_slots) {
      int slot_end = p->slot_start[p->cur_slot + 1];
      if (slot_end - p->pos_ms < remain_slot && slot_end > p->pos_ms)
        remain_slot = slot_end - p->pos_ms;
    }

    chunk = want_frames - written;
    if (chunk * 4 > SCRATCH)
      chunk = SCRATCH / 4;
    max_fr = (int)(((int64_t)remain_slot * (int64_t)p->rate + 999) / 1000);
    if (max_fr < 1) max_fr = 1;
    if (chunk > max_fr) chunk = max_fr;

    memset(p->scratch, 0, (size_t)chunk * 4);
    if (p->dec && p->slot_ready)
      tfmxdec_buffer_fill(p->dec, p->scratch, (uint32_t)(chunk * 4));
    /* else: silence until the cap / next slot */

    nsamp = chunk * 2;
    s = (const int16_t *)p->scratch;
    for (i = 0; i < nsamp; ++i)
      stereo[written * 2 + i] = (float)s[i] * (1.0f / 32768.0f);

    advance_pos(p, chunk);
    written += chunk;

    /* Crossing a slot boundary: force reinit of the next slot next loop. */
    if (p->cur_slot + 1 < p->n_slots &&
        p->pos_ms >= p->slot_start[p->cur_slot + 1])
      p->slot_ready = 0;
  }

  if (p->duration_ms > 0 && p->pos_ms >= p->duration_ms)
    p->ended = 1;

  if (written <= 0 && !p->ended) {
    /* Never hand the host an early EOF. One silent stereo frame. */
    stereo[0] = 0.0f;
    stereo[1] = 0.0f;
    advance_pos(p, 1);
    written = 1;
  }
  return written * 2;
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
  if (!out)
    return -1;
  memset(out, 0, sizeof *out);
  p = tfmx_player_open(path, mdat, mdat_len, smpl, smpl_len);
  if (!p)
    return -1;
  out->songs = 1;
  out->voices = p->voices;
  out->duration_ms[0] = p->duration_ms;
  bounded_copy(out->title, sizeof out->title, p->title);
  bounded_copy(out->artist, sizeof out->artist, p->artist);
  bounded_copy(out->name, sizeof out->name, p->name);
  bounded_copy(out->format_id, sizeof out->format_id, p->format_id);
  bounded_copy(out->format_name, sizeof out->format_name, p->format_name);
  tfmx_player_close(p);
  return 0;
}
