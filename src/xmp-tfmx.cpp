/*
 * xmp-tfmx — native XMPlay input plugin for Chris Hülsbeck TFMX.
 *
 * Not a Winamp in_tfmx.dll wrapper (Per Lindén ~1998, TMFXPlug).
 * Classic XMPlay is 32-bit only. DllMain only DisableThreadLibraryCalls.
 */
#if defined(__GNUC__)
#define XMPIN_GetInterface XMPIN_GetInterface_Declared
#endif
#include "xmpin.h"
#if defined(__GNUC__)
#undef XMPIN_GetInterface
#endif

#include "tfmx_player.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define PLUGIN_NAME    "TFMX"
#define PLUGIN_VERSION "1.0.2"
#define PLUGIN_XMPVER  1000000
#define MAX_MODULE_BYTES ((size_t)16u * 1024u * 1024u)
#define MAX_SMPL_BYTES   ((size_t)32u * 1024u * 1024u)
#define INFO_WRITE_MAX   32766

static XMPFUNC_IN   *xmpfin;
static XMPFUNC_MISC *xmpfmisc;
static XMPFUNC_FILE *xmpffile;

static tfmx_player *g_play;
static char         g_name_hint[512];

#ifdef _WIN32
static HINSTANCE g_hinst;
#endif

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

static void sanitize_line(char *s)
{
  if (!s) return;
  for (; *s; ++s)
    if (*s == '\t' || *s == '\r' || *s == '\n')
      *s = ' ';
}

static void write_kv(char **cursor, char *end, const char *name, const char *value)
{
  size_t nl, vl, need;
  if (!cursor || !*cursor || !end || !name || !value || !value[0])
    return;
  nl = strlen(name);
  vl = strlen(value);
  need = nl + 1 + vl + 1;
  if (*cursor + need >= end)
    return;
  memcpy(*cursor, name, nl); *cursor += nl;
  **cursor = '\t'; *cursor += 1;
  memcpy(*cursor, value, vl); *cursor += vl;
  **cursor = '\r'; *cursor += 1;
  **cursor = '\0';
}

static void *xmp_alloc(DWORD n)
{
  if (!xmpfmisc || !xmpfmisc->Alloc || n == 0)
    return NULL;
  return xmpfmisc->Alloc(n);
}

static void remember_hint(const char *filename)
{
  size_t n;
  /* Keep a previous hint if this call is memory-only (no path). */
  if (!filename || !filename[0])
    return;
  n = strlen(filename);
  if (n >= sizeof g_name_hint)
    n = sizeof g_name_hint - 1;
  memcpy(g_name_hint, filename, n);
  g_name_hint[n] = '\0';
}

static const char *resolve_path(const char *filename)
{
  if (filename && filename[0])
    return filename;
  if (g_name_hint[0])
    return g_name_hint;
  return NULL;
}

static int slurp_xmpfile_max(XMPFILE file, unsigned char **out, size_t *out_len, size_t max_bytes)
{
  DWORD type, sz, got, pos;
  unsigned char *buf;
  if (out) *out = NULL;
  if (out_len) *out_len = 0;
  if (!file || !out || !out_len || !xmpffile || !xmpffile->Read)
    return 0;
  type = xmpffile->GetType(file);
  if (type == XMPFILE_TYPE_MEMORY) {
    const void *mem;
    if (!xmpffile->GetMemory || !xmpffile->GetSize)
      return 0;
    mem = xmpffile->GetMemory(file);
    sz = xmpffile->GetSize(file);
    if (!mem || sz < 4 || (size_t)sz > max_bytes)
      return 0;
    buf = (unsigned char *)malloc(sz);
    if (!buf) return 0;
    memcpy(buf, mem, sz);
    *out = buf;
    *out_len = sz;
    return 1;
  }
  sz = xmpffile->GetSize ? xmpffile->GetSize(file) : 0;
  pos = xmpffile->Tell ? xmpffile->Tell(file) : 0;
  if (xmpffile->Seek)
    xmpffile->Seek(file, 0);
  if (sz > 0) {
    if (sz < 4 || (size_t)sz > max_bytes) {
      if (xmpffile->Seek) xmpffile->Seek(file, pos);
      return 0;
    }
    buf = (unsigned char *)malloc(sz);
    if (!buf) {
      if (xmpffile->Seek) xmpffile->Seek(file, pos);
      return 0;
    }
    got = xmpffile->Read(file, buf, sz);
    if (xmpffile->Seek) xmpffile->Seek(file, pos);
    if (got < 4) { free(buf); return 0; }
    *out = buf;
    *out_len = got;
    return 1;
  }
  {
    size_t cap = 64 * 1024, total = 0;
    buf = (unsigned char *)malloc(cap);
    if (!buf) return 0;
    for (;;) {
      DWORD chunk;
      if (total == cap) {
        size_t ncap = cap * 2;
        unsigned char *nb;
        if (ncap > max_bytes) ncap = max_bytes;
        if (ncap <= cap) { free(buf); return 0; }
        nb = (unsigned char *)realloc(buf, ncap);
        if (!nb) { free(buf); return 0; }
        buf = nb;
        cap = ncap;
      }
      chunk = xmpffile->Read(file, buf + total, (DWORD)(cap - total));
      if (chunk == 0) break;
      total += chunk;
      if (total >= max_bytes) break;
    }
    if (xmpffile->Seek) xmpffile->Seek(file, pos);
    if (total < 4) { free(buf); return 0; }
    *out = buf;
    *out_len = total;
    return 1;
  }
}

static int slurp_xmpfile(XMPFILE file, unsigned char **out, size_t *out_len)
{
  return slurp_xmpfile_max(file, out, out_len, MAX_MODULE_BYTES);
}

static XMPFILE open_if_needed(const char *filename, XMPFILE file, int *opened)
{
  *opened = 0;
  if (file) return file;
  if (!filename || !xmpffile || !xmpffile->Open)
    return NULL;
  file = xmpffile->Open(filename);
  if (file) *opened = 1;
  return file;
}

static void close_if_opened(XMPFILE file, int opened)
{
  if (opened && file && xmpffile && xmpffile->Close)
    xmpffile->Close(file);
}

static void unload_playback(void)
{
  if (g_play) {
    tfmx_player_close(g_play);
    g_play = NULL;
  }
  /* Keep g_name_hint so a later memory-only Open can still find the sidecar. */
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

static int starts_ieq(const char *s, const char *pfx)
{
  size_t n;
  if (!s || !pfx) return 0;
  n = strlen(pfx);
  {
    size_t i;
    for (i = 0; i < n; ++i) {
      unsigned char ca = (unsigned char)s[i];
      unsigned char cb = (unsigned char)pfx[i];
      if (!ca) return 0;
      if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
      if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
      if (ca != cb) return 0;
    }
  }
  return 1;
}

static void add_cand(char cands[][TFMX_PATH], int *n, int max, const char *s)
{
  int i;
  if (!s || !s[0] || *n >= max)
    return;
  for (i = 0; i < *n; ++i)
    if (strcmp(cands[i], s) == 0)
      return;
  bounded_copy(cands[*n], TFMX_PATH, s);
  *n += 1;
}

static int sidecar_candidates(const char *path, char cands[][TFMX_PATH], int max)
{
  char dir[TFMX_PATH], name[TFMX_PATH], stem[TFMX_PATH], buf[TFMX_PATH];
  const char *sep, *slash, *bslash;
  const char *exts[] = { ".sam", ".SAM", ".smpl", ".SMPL", NULL };
  int n = 0, i;

  if (!path || !path[0])
    return 0;
  slash = strrchr(path, '/');
  bslash = strrchr(path, '\\');
  sep = slash;
  if (bslash && (!sep || bslash > sep))
    sep = bslash;
  if (sep) {
    size_t dlen = (size_t)(sep - path + 1);
    if (dlen >= sizeof dir) dlen = sizeof dir - 1;
    memcpy(dir, path, dlen);
    dir[dlen] = '\0';
    bounded_copy(name, sizeof name, sep + 1);
  } else {
    dir[0] = '\0';
    bounded_copy(name, sizeof name, path);
  }
  bounded_copy(stem, sizeof stem, name);
  {
    char *dot = strrchr(stem, '.');
    if (dot && dot != stem)
      *dot = '\0';
  }
  for (i = 0; exts[i]; ++i) {
    snprintf(buf, sizeof buf, "%s%s%s", dir, stem, exts[i]);
    add_cand(cands, &n, max, buf);
  }
  snprintf(buf, sizeof buf, "%ssmpl.%s", dir, stem);
  add_cand(cands, &n, max, buf);
  snprintf(buf, sizeof buf, "%sSMPL.%s", dir, stem);
  add_cand(cands, &n, max, buf);
  snprintf(buf, sizeof buf, "%ssmpl.%s", dir, name);
  add_cand(cands, &n, max, buf);
  snprintf(buf, sizeof buf, "%sSMPL.%s", dir, name);
  add_cand(cands, &n, max, buf);
  if (starts_ieq(name, "mdat.")) {
    snprintf(buf, sizeof buf, "%ssmpl.%s", dir, name + 5);
    add_cand(cands, &n, max, buf);
    snprintf(buf, sizeof buf, "%sSMPL.%s", dir, name + 5);
    add_cand(cands, &n, max, buf);
  }
  (void)ieq;
  return n;
}

static int find_sidecar(const char *filename, unsigned char **out, size_t *out_len)
{
  char cands[16][TFMX_PATH];
  int n, i;
  *out = NULL;
  *out_len = 0;
  if (!filename || !xmpffile || !xmpffile->Open)
    return 0;
  n = sidecar_candidates(filename, cands, 16);
  for (i = 0; i < n; ++i) {
    XMPFILE f = xmpffile->Open(cands[i]);
    unsigned char *buf = NULL;
    size_t len = 0;
    if (!f)
      continue;
    if (slurp_xmpfile_max(f, &buf, &len, MAX_SMPL_BYTES) && buf && len >= 4) {
      if (xmpffile->Close)
        xmpffile->Close(f);
      *out = buf;
      *out_len = len;
      return 1;
    }
    free(buf);
    if (xmpffile->Close)
      xmpffile->Close(f);
  }
  return 0;
}

static void append_tag(char **p, char *end, const char *key, const char *val)
{
  size_t kl, vl;
  if (!p || !*p || !end || !key || !val || !val[0])
    return;
  kl = strlen(key);
  vl = strlen(val);
  if (*p + kl + 1 + vl + 1 + 1 >= end)
    return;
  memcpy(*p, key, kl); *p += kl;
  **p = '\0'; *p += 1;
  memcpy(*p, val, vl); *p += vl;
  **p = '\0'; *p += 1;
}

static char *finish_tags(char *stack, char *p, size_t stack_sz)
{
  char *end = stack + stack_sz;
  char *out;
  size_t n;
  if (p + 1 < end)
    *p++ = '\0';
  n = (size_t)(p - stack);
  out = (char *)xmp_alloc((DWORD)n);
  if (!out) return NULL;
  memcpy(out, stack, n);
  return out;
}

static char *build_tags_info(const tfmx_info *inf, int track0)
{
  char stack[8192];
  char *p = stack;
  char *end = stack + sizeof stack;
  char trk[16];
  (void)track0;
  if (!xmpfmisc) return NULL;
  append_tag(&p, end, "filetype", "TFMX");
  append_tag(&p, end, "title", inf->title[0] ? inf->title : inf->name);
  append_tag(&p, end, "artist", inf->artist);
  if (inf->songs > 1) {
    snprintf(trk, sizeof trk, "%d", track0 + 1);
    append_tag(&p, end, "track", trk);
  }
  return finish_tags(stack, p, sizeof stack);
}

static char *build_tags_play(tfmx_player *pl)
{
  char stack[8192];
  char *p = stack;
  char *end = stack + sizeof stack;
  char trk[16];
  if (!pl || !xmpfmisc) return NULL;
  append_tag(&p, end, "filetype", "TFMX");
  append_tag(&p, end, "title", tfmx_player_title(pl)[0] ? tfmx_player_title(pl) : tfmx_player_name(pl));
  append_tag(&p, end, "artist", tfmx_player_artist(pl));
  if (tfmx_player_songs(pl) > 1) {
    snprintf(trk, sizeof trk, "%d", tfmx_player_song(pl) + 1);
    append_tag(&p, end, "track", trk);
  }
  return finish_tags(stack, p, sizeof stack);
}

static void set_length_now(int play_ms)
{
  float sec;
  if (!xmpfin || !xmpfin->SetLength || play_ms <= 0)
    return;
  sec = (float)play_ms / 1000.0f;
  if (sec > 0.0f && sec < 86400.0f)
    xmpfin->SetLength(sec, TRUE);
}

static void WINAPI tfmx_About(HWND win)
{
  char buf[1600];
  snprintf(buf, sizeof buf,
    PLUGIN_NAME " " PLUGIN_VERSION "\r\n"
    "Native XMPlay input plugin for Chris Hülsbeck TFMX\r\n"
    "(The Final Musicsystem eXtended).\r\n\r\n"
    "This is NOT Winamp in_tfmx.dll (Per Lindén ~1998, TMFXPlug).\r\n"
    "That plugin has no playhead, no real length, and rejects some\r\n"
    "modern .tfx+.sam rips (tiny TFMX-SONG modules without a 512-byte\r\n"
    "text block / mdat+smpl names).\r\n\r\n"
    "This plugin:\r\n"
    "  - seekable playhead (tfmxdec_seek, 1 ms)\r\n"
    "  - real song length (library duration if it matches heard audio;\r\n"
    "    else last audible sample + tail / 10-minute cap)\r\n"
    "  - loop-end is not EOF — we play to the measured length\r\n"
    "  - SFX / one-note song-table slots are not NSF tracks\r\n"
    "  - .tfx + .sam (also .tfm/.mdat/.tfmx + .smpl, mdat./smpl.)\r\n"
    "  - NSF-style tracks (Shift+Left / Shift+Right)\r\n\r\n"
    "Engine: libtfmxaudiodecoder by Michael Schwendt.\r\n"
    "Hülsbeck TFMX only — not Hippel / Future Composer .fc.\r\n"
    "32-bit XMPlay only (PE32 i386).\r\n"
    "License: GPLv2+.");
#ifdef _WIN32
  MessageBoxA(win, buf, PLUGIN_NAME, MB_OK | MB_ICONINFORMATION);
#else
  (void)win;
  (void)buf;
#endif
}

static BOOL WINAPI tfmx_CheckFile(const char *filename, XMPFILE file)
{
  unsigned char *data = NULL;
  size_t len = 0;
  int opened = 0;
  int ok;

  remember_hint(filename);
  file = open_if_needed(filename, file, &opened);
  if (!file)
    return FALSE;
  if (!slurp_xmpfile(file, &data, &len)) {
    close_if_opened(file, opened);
    return FALSE;
  }
  close_if_opened(file, opened);
  ok = tfmx_probe(data, len);
  free(data);
  return ok ? TRUE : FALSE;
}

static tfmx_player *open_from(const char *filename, unsigned char *data, size_t len)
{
  unsigned char *smpl = NULL;
  size_t smpl_len = 0;
  tfmx_player *pl;
  const char *path = resolve_path(filename);
  /* Always try stem.sam / stem.smpl / smpl.stem / SMPL.stem via XMPlay. */
  if (path && path[0])
    find_sidecar(path, &smpl, &smpl_len);
  if (!smpl && g_name_hint[0] && (!path || strcmp(path, g_name_hint) != 0))
    find_sidecar(g_name_hint, &smpl, &smpl_len);
  pl = tfmx_player_open(path, data, len, smpl, smpl_len);
  free(smpl);
  return pl;
}

static DWORD WINAPI tfmx_GetFileInfo(const char *filename, XMPFILE file,
                                     float **length, char **tags)
{
  unsigned char *data = NULL;
  size_t len = 0;
  int opened = 0;
  tfmx_info inf;
  unsigned char *smpl = NULL;
  size_t smpl_len = 0;
  int n, i;

  if (length) *length = NULL;
  if (tags) *tags = NULL;

  remember_hint(filename);
  file = open_if_needed(filename, file, &opened);
  if (!file)
    return 0;
  if (!slurp_xmpfile(file, &data, &len)) {
    close_if_opened(file, opened);
    return 0;
  }
  close_if_opened(file, opened);

  {
    const char *path = resolve_path(filename);
    find_sidecar(path, &smpl, &smpl_len);
    if (!smpl && g_name_hint[0] && (!path || strcmp(path, g_name_hint) != 0))
      find_sidecar(g_name_hint, &smpl, &smpl_len);
    if (tfmx_analyze(path, data, len, smpl, smpl_len, &inf) != 0) {
      free(data);
      free(smpl);
      return 0;
    }
  }
  free(data);
  free(smpl);

  n = inf.songs > 0 ? inf.songs : 1;
  if (length) {
    float *lens = (float *)xmp_alloc((DWORD)(sizeof(float) * (unsigned)n));
    if (lens) {
      for (i = 0; i < n; ++i)
        lens[i] = (float)inf.duration_ms[i] / 1000.0f;
    }
    *length = lens;
  }
  if (tags)
    *tags = build_tags_info(&inf, 0);
  return (DWORD)n | XMPIN_INFO_NOSUBTAGS;
}

static DWORD WINAPI tfmx_Open(const char *filename, XMPFILE file)
{
  unsigned char *data = NULL;
  size_t len = 0;
  int opened = 0;

  unload_playback();
  remember_hint(filename);

  file = open_if_needed(filename, file, &opened);
  if (!file)
    return 0;
  if (!slurp_xmpfile(file, &data, &len)) {
    close_if_opened(file, opened);
    return 0;
  }
  close_if_opened(file, opened);

  g_play = open_from(filename, data, len);
  free(data);
  if (!g_play)
    return 0;
  set_length_now(tfmx_player_duration_ms(g_play, tfmx_player_song(g_play)));
  return 2; /* stereo */
}

static void WINAPI tfmx_Close(void)
{
  unload_playback();
}

static void WINAPI tfmx_SetFormat(XMPFORMAT *form)
{
  if (!form)
    return;
  if (!g_play) {
    form->rate = 0;
    form->chan = 0;
    form->res = 0;
    form->chanmask = 0;
    return;
  }
  form->rate = (DWORD)tfmx_player_rate(g_play);
  form->chan = 2;
  form->res = 4; /* float */
  form->chanmask = 0;
}

static char *WINAPI tfmx_GetTags(void)
{
  if (!g_play)
    return NULL;
  return build_tags_play(g_play);
}

static void WINAPI tfmx_GetInfoText(char *format, char *length)
{
  char tmp[256];
  int m, s, play;
  if (format) format[0] = '\0';
  if (length) length[0] = '\0';
  if (!g_play)
    return;
  if (format) {
    snprintf(tmp, sizeof tmp, "TFMX  %s", tfmx_player_format_name(g_play));
    sanitize_line(tmp);
    bounded_copy(format, 256, tmp);
  }
  if (length) {
    play = tfmx_player_duration_ms(g_play, tfmx_player_song(g_play));
    m = play / 60000;
    s = (play / 1000) % 60;
    if (tfmx_player_songs(g_play) > 1)
      snprintf(tmp, sizeof tmp, "%d:%02d  track %d/%d",
               m, s, tfmx_player_song(g_play) + 1, tfmx_player_songs(g_play));
    else
      snprintf(tmp, sizeof tmp, "%d:%02d", m, s);
    sanitize_line(tmp);
    bounded_copy(length, 256, tmp);
  }
}

static void WINAPI tfmx_GetGeneralInfo(char *buf)
{
  char local[4096];
  char *p, *end;
  char num[32];
  if (!buf) return;
  buf[0] = '\0';
  if (!g_play) return;
  p = local;
  end = local + sizeof local - 2;
  local[0] = '\0';
  write_kv(&p, end, "Title", tfmx_player_title(g_play));
  write_kv(&p, end, "Name", tfmx_player_name(g_play));
  write_kv(&p, end, "Artist", tfmx_player_artist(g_play));
  write_kv(&p, end, "Format", tfmx_player_format_name(g_play));
  write_kv(&p, end, "Format ID", tfmx_player_format_id(g_play));
  snprintf(num, sizeof num, "%d", tfmx_player_voices(g_play));
  write_kv(&p, end, "Voices", num);
  if (tfmx_player_songs(g_play) > 1) {
    snprintf(num, sizeof num, "%d", tfmx_player_songs(g_play));
    write_kv(&p, end, "Tracks", num);
    snprintf(num, sizeof num, "%d", tfmx_player_song(g_play) + 1);
    write_kv(&p, end, "Current track", num);
  }
  write_kv(&p, end, "Player", PLUGIN_NAME " " PLUGIN_VERSION);
  write_kv(&p, end, "Engine", "libtfmxaudiodecoder (Michael Schwendt)");
  write_kv(&p, end, "Note", "Native XMPlay TFMX — not in_tfmx.dll");
  bounded_copy(buf, INFO_WRITE_MAX, local);
}

static void WINAPI tfmx_GetMessage(char *buf)
{
  if (!buf) return;
  buf[0] = '\0';
}

static double WINAPI tfmx_GetGranularity(void)
{
  return 0.001;
}

static double WINAPI tfmx_SetPosition(DWORD pos)
{
  int sub;
  int ms;

  if (!g_play)
    return -1.0;

  if (pos == (DWORD)XMPIN_POS_LOOP || pos == (DWORD)XMPIN_POS_AUTOLOOP)
    return -2.0;

  if (pos & XMPIN_POS_SUBSONG) {
    sub = (int)(pos & 0xFFFFu);
    if (tfmx_player_set_song(g_play, sub) != 0)
      return -1.0;
    set_length_now(tfmx_player_duration_ms(g_play, sub));
    if (xmpfin && xmpfin->UpdateTitle)
      xmpfin->UpdateTitle(NULL);
    return 0.0;
  }

  ms = (int)pos;
  ms = tfmx_player_seek_ms(g_play, ms);
  if (ms < 0)
    return -1.0;
  return (double)ms / 1000.0;
}

static DWORD WINAPI tfmx_Process(float *buf, DWORD count)
{
  int got;
  if (!buf || !g_play)
    return 0;
  got = tfmx_player_process(g_play, buf, (int)count);
  if (got <= 0)
    return 0;
  return (DWORD)got;
}

static DWORD WINAPI tfmx_GetSubSongs(float *length)
{
  if (!g_play)
    return 0;
  if (length)
    *length = (float)tfmx_player_total_duration_ms(g_play) / 1000.0f;
  return (DWORD)tfmx_player_songs(g_play);
}

static const char g_exts[] = "TFMX\0tfx/tfm/mdat/tfmx";

static XMPIN g_xmpin = {
  0,
  PLUGIN_NAME " " PLUGIN_VERSION,
  g_exts,
  tfmx_About,
  NULL,
  tfmx_CheckFile,
  tfmx_GetFileInfo,
  tfmx_Open,
  tfmx_Close,
  NULL,
  tfmx_SetFormat,
  tfmx_GetTags,
  tfmx_GetInfoText,
  tfmx_GetGeneralInfo,
  tfmx_GetMessage,
  tfmx_SetPosition,
  tfmx_GetGranularity,
  NULL,
  tfmx_Process,
  NULL,
  NULL,
  tfmx_GetSubSongs,
  NULL,
  NULL,
  NULL,
  NULL, NULL, NULL, NULL, NULL, NULL, NULL,
  NULL,
  NULL,
  NULL
};

static XMPIN *WINAPI xmpin_get_interface_impl(DWORD face, InterfaceProc faceproc)
{
  if (face != XMPIN_FACE)
    return NULL;
  if (!faceproc)
    return NULL;
  xmpfin   = (XMPFUNC_IN *)faceproc(XMPFUNC_IN_FACE);
  xmpfmisc = (XMPFUNC_MISC *)faceproc(XMPFUNC_MISC_FACE);
  xmpffile = (XMPFUNC_FILE *)faceproc(XMPFUNC_FILE_FACE);
  if (!xmpfin || !xmpfmisc || !xmpffile)
    return NULL;
  if (!xmpfmisc->Alloc || !xmpffile->Read)
    return NULL;
  (void)PLUGIN_XMPVER;
  return &g_xmpin;
}

extern "C" {

BOOL WINAPI DllMain(HINSTANCE hDLL, DWORD reason, LPVOID reserved)
{
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
#ifdef _WIN32
    g_hinst = hDLL;
    DisableThreadLibraryCalls(hDLL);
#else
    (void)hDLL;
#endif
  }
  return TRUE;
}

#if defined(__GNUC__) && defined(_WIN32) && !defined(_WIN64)
XMPIN *WINAPI XMPIN_GetInterface_(DWORD face, InterfaceProc faceproc)
{
  return xmpin_get_interface_impl(face, faceproc);
}
#if __GNUC__ >= 8
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattribute-alias"
#endif
__attribute__((dllexport)) void XMPIN_GetInterface(void)
  __attribute__((alias("XMPIN_GetInterface_@8")));
#if __GNUC__ >= 8
#pragma GCC diagnostic pop
#endif
#else
__declspec(dllexport) XMPIN *WINAPI XMPIN_GetInterface(DWORD face, InterfaceProc faceproc)
{
  return xmpin_get_interface_impl(face, faceproc);
}
#endif

} /* extern "C" */
