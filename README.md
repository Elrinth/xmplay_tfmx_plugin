# xmp-tfmx 1.0.3

Native **32-bit** XMPlay input plugin for Chris Hülsbeck **TFMX**
(The Final Musicsystem eXtended).

Engine: **libtfmxaudiodecoder** by Michael Schwendt, compiled in statically.
No extra DLLs.

**This is not** Winamp `in_tfmx.dll` (Per Lindén ~1998, product TMFXPlug).
That plugin has no playhead, no real length, and often rejects modern
`.tfx` + `.sam` rips — especially tiny `TFMX-SONG` modules that do not
carry a 512-byte text block or classic `mdat.` / `smpl.` names.

Intended home: `Elrinth/xmplay_tfmx_plugin`.

v1.0 is **Hülsbeck TFMX only**. It does **not** claim Hippel / Future
Composer `.fc` (those conflict with other XMPlay plugins).

## Install

Copy `xmp-tfmx.dll` next to `xmplay.exe` (or into XMPlay's plugin folder)
and restart XMPlay. The DLL carries a Windows VERSIONINFO resource
(FILEVERSION 1.0.3.0, PLUGIN_XMPVER 1000300) so XMPlay can include it
in update notifications. Classic XMPlay is **32-bit only** — this DLL
is PE32 i386. A 64-bit build will not load.

XMPlay's *Supported file types* list shows **TFMX** with extensions
`tfx/tfm/mdat/tfmx`.

## Formats

| Extension | Notes |
|-----------|--------|
| `.tfx`    | Music data; looks for sibling `.sam` |
| `.tfm` `.tfmx` | Single-file / editor names |
| `.mdat`   | Amiga-style music data; sibling `.smpl` |
| `mdat.*`  | Amiga prefix; sibling `smpl.*` / `SMPL.*` |

Sample sidecars: `.sam` / `.smpl` / `smpl.` / `SMPL.` (and `smpl.set` via
the engine). CheckFile only inspects the music-data file (no sidecar
required to add to the playlist). Open must find the sidecar — or a
merged single-file format (TFMXPAK / TFHD / TFMX-MOD).

## Length + seek

- **One playlist item per file.** `GetFileInfo` returns a single length;
  `GetSubSongs` returns 1 (never 8). There is no fake NSF split and no
  Shift+arrow tracks. The title is never `name - 1/8`.
- Real song-table slots (sustained audio) play **back-to-back** as one
  stream. Length is the sum of those slots (e.g. 6:46 when eight library
  slots have audio). A file with one slot stays one stream of that slot.
- Playlist / file-info length comes from `tfmxdec_duration()` per slot
  when that matches heard audio (dry-run to the first loop / song-end).
- If the engine reports 0 or a one-note-short time, that slot is measured
  with loop mode on until 2 seconds of silence, capped at **10 minutes**.
- Loop-end / `song_end` is not treated as EOF. Process does not return 0
  until the playhead reaches the advertised total. Decoder errors become
  silence until that cap — XMPlay is never handed an early end-of-file.
- Playhead is seekable (`SetLength(seconds, TRUE)`, granularity 1 ms);
  seek maps into the slot chain.
- Playback uses loop mode so the first pattern loop does not stop a slot.
- Open uses the real `.tfx` path (`tfmxdec_set_path`) so the library
  finds the sibling `.sam` next to it — same as qmmp-tfmx. A temp pair
  is only written for memory-only opens. Engine is Chris/Hülsbeck
  `TFMXDecoder`, not Jochen/Hippel.


The displayed title is the module title when it is non-empty and not
`(Empty)`; otherwise the original filename stem (never a temp `mod.tfx`).

## Why `in_tfmx.dll` rejects some `.tfx` files

Classic TMFXPlug (~1998) typically expects:

- a full ~512-byte text/comment block after `TFMX-SONG `
- uncompressed layout (track table at `0x800`)
- Amiga `mdat.` + `smpl.` names rather than `.tfx` + `.sam`

A compressed 1008-byte `TFMX-SONG` with `00 01 00 00 08 00` (compress
flag + hint) and a sidecar `.sam` is valid Hülsbeck TFMX. This plugin
loads it via libtfmxaudiodecoder (`tfmxdec_set_path` so the sibling
`.sam` is found).

## Build

```
git clone --depth 1 https://github.com/mschwendt/libtfmxaudiodecoder \
  third_party/libtfmxaudiodecoder
# Config.h is shipped under third_party/.../src/Config.h
/usr/bin/make          # host tests + i686 DLL
/usr/bin/make dll
/usr/bin/make test
```

Needs `g++` and `i686-w64-mingw32-g++` / `windres`. The DLL is linked
`-static -static-libgcc -static-libstdc++`.

Do not commit copyrighted `.tfx` / `.sam` rips. Host tests look for
`tests/samples/user-song.tfx` + `.sam` (and `user-mod.tfx` + `.sam`
when present) locally.

## Credits

- TFMX — Chris Hülsbeck
- libtfmxaudiodecoder — Michael Schwendt
- XMPlay plugin SDK — un4seen / Ian Luck

License: GPLv2 or later (libtfmxaudiodecoder is GPLv2+).
