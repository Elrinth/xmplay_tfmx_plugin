/*
 * Host-testable TFMX (Hülsbeck) wrapper used by xmp-tfmx.
 * Engine: libtfmxaudiodecoder (GPLv2+).
 */
#ifndef TFMX_PLAYER_H
#define TFMX_PLAYER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TFMX_MAX_SONGS     32
#define TFMX_STR           256
#define TFMX_PATH          1024
#define TFMX_RATE          44100
#define TFMX_DETECT_CAP_MS (10 * 60 * 1000)
#define TFMX_SILENCE_MS    2000
#define TFMX_TINY_MS       500   /* ignore one-note / bogus duration */

typedef struct tfmx_info {
  int  songs;
  int  duration_ms[TFMX_MAX_SONGS];
  int  voices;
  char title[TFMX_STR];
  char artist[TFMX_STR];
  char name[TFMX_STR];
  char format_id[64];
  char format_name[128];
} tfmx_info;

/* 1 if Hülsbeck TFMX (not Hippel / Future Composer / DNS). */
int tfmx_probe(const unsigned char *data, size_t len);

/* Open, measure per-track length, close. Returns 0 on success. */
int tfmx_analyze(const char *path,
                 const unsigned char *mdat, size_t mdat_len,
                 const unsigned char *smpl, size_t smpl_len,
                 tfmx_info *out);

typedef struct tfmx_player tfmx_player;

/* path is the .tfx/.mdat path (used to find sibling .sam/.smpl).
 * smpl may be NULL if the sidecar is next to path on disk. */
tfmx_player *tfmx_player_open(const char *path,
                              const unsigned char *mdat, size_t mdat_len,
                              const unsigned char *smpl, size_t smpl_len);
void         tfmx_player_close(tfmx_player *p);

int    tfmx_player_songs(const tfmx_player *p);
int    tfmx_player_song(const tfmx_player *p); /* 0-based */
int    tfmx_player_set_song(tfmx_player *p, int song0);
int    tfmx_player_rate(const tfmx_player *p);
int    tfmx_player_voices(const tfmx_player *p);
int    tfmx_player_duration_ms(const tfmx_player *p, int song0);
int    tfmx_player_total_duration_ms(const tfmx_player *p);
int    tfmx_player_position_ms(const tfmx_player *p);
int    tfmx_player_seek_ms(tfmx_player *p, int ms);
int    tfmx_player_ended(const tfmx_player *p);

/* Decode stereo float samples. count = number of floats (L+R).
 * Returns floats written (0 = end). */
int    tfmx_player_process(tfmx_player *p, float *stereo, int count);

const char *tfmx_player_title(const tfmx_player *p);
const char *tfmx_player_artist(const tfmx_player *p);
const char *tfmx_player_name(const tfmx_player *p);
const char *tfmx_player_format_id(const tfmx_player *p);
const char *tfmx_player_format_name(const tfmx_player *p);

#ifdef __cplusplus
}
#endif
#endif
