/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  patches.c -- ROM patches (.ips, .bps) applied in memory when a game loads,
 *  and the header checksum fixer.
 ****************************************************************************/

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "shared.h"
#include "gui.h"
#ifdef USE_LIBCHDR
#include "sevenzip.h"
#endif

extern int load_archive(char *filename, unsigned char *buffer, int maxsize, char *extension);

#define ROM_CAP (16 * 1024 * 1024)

/* Reads a whole small file; the caller frees it. */
static unsigned char *read_file(const char *path, int *size)
{
  FILE *fp = fopen(path, "rb");
  long len;
  unsigned char *buf;

  if (!fp) return NULL;
  fseek(fp, 0, SEEK_END);
  len = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  if (len <= 0 || len > ROM_CAP) { fclose(fp); return NULL; }
  buf = (unsigned char *)malloc((size_t)len);
  if (buf && fread(buf, 1, (size_t)len, fp) != (size_t)len) { free(buf); buf = NULL; }
  fclose(fp);
  if (buf) *size = (int)len;
  return buf;
}

/* Reads a patch: a plain .ips / .bps file, or the first file of a .zip or .7z archive. The caller frees it. */
static unsigned char *read_patch(const char *path, int *size)
{
  const char *dot = strrchr(path, '.');

  if (dot && !lstrcmpiA(dot, ".zip"))
  {
    char ext[16] = "";
    unsigned char *buf = (unsigned char *)malloc(ROM_CAP);
    int n;
    if (!buf) return NULL;
    n = load_archive((char *)path, buf, ROM_CAP, ext);
    if (n <= 0) { free(buf); return NULL; }
    *size = n;
    return buf;
  }
#ifdef USE_LIBCHDR
  if (dot && !lstrcmpiA(dot, ".7z"))
  {
    sevenzip_t *sz = sevenzip_open(path);
    unsigned char *buf = NULL;
    int i, count, index = -1;
    unsigned int len;

    if (!sz) return NULL;
    count = sevenzip_count(sz);
    for (i = 0; i < count; i++) if (!sevenzip_entry_is_dir(sz, i)) { index = i; break; }
    if (index >= 0)
    {
      len = sevenzip_entry_size(sz, index);
      if (len > 0 && len <= (unsigned)ROM_CAP)
      {
        buf = (unsigned char *)malloc(len);
        if (buf && sevenzip_extract(sz, index, buf, (int)len) != (int)len) { free(buf); buf = NULL; }
        else if (buf) *size = (int)len;
      }
    }
    sevenzip_close(sz);
    return buf;
  }
#endif
  return read_file(path, size);
}

/* IPS: "PATCH", records (offset 3 bytes, size 2 bytes, data; size 0 = a run: length 2 bytes, value), "EOF" and
   optionally a 3-byte length to cut the ROM to. */
static int ips_apply(unsigned char *rom, int *size, int cap, const unsigned char *p, int plen)
{
  int pos = 5;

  if (plen < 8 || memcmp(p, "PATCH", 5)) return 0;
  while (pos + 3 <= plen)
  {
    unsigned off, len;

    if (!memcmp(p + pos, "EOF", 3))
    {
      if (pos + 6 == plen)
      {
        unsigned cut = ((unsigned)p[pos + 3] << 16) | ((unsigned)p[pos + 4] << 8) | p[pos + 5];
        if ((int)cut < *size) *size = (int)cut;
      }
      return 1;
    }
    if (pos + 5 > plen) return 0;
    off = ((unsigned)p[pos] << 16) | ((unsigned)p[pos + 1] << 8) | p[pos + 2];
    len = ((unsigned)p[pos + 3] << 8) | p[pos + 4];
    pos += 5;
    if (len == 0)
    {
      unsigned run;
      if (pos + 3 > plen) return 0;
      run = ((unsigned)p[pos] << 8) | p[pos + 1];
      if ((int)(off + run) > cap) return 0;
      if ((int)(off + run) > *size) { memset(rom + *size, 0, (size_t)(off + run - (unsigned)*size)); *size = (int)(off + run); }
      memset(rom + off, p[pos + 2], run);
      pos += 3;
    }
    else
    {
      if (pos + (int)len > plen || (int)(off + len) > cap) return 0;
      if ((int)(off + len) > *size) { memset(rom + *size, 0, (size_t)(off + len - (unsigned)*size)); *size = (int)(off + len); }
      memcpy(rom + off, p + pos, len);
      pos += (int)len;
    }
  }
  return 0;
}

static unsigned long bps_num(const unsigned char *p, int *pos, int end)
{
  unsigned long data = 0, shift = 1;

  while (*pos < end)
  {
    unsigned char x = p[(*pos)++];
    data += (x & 0x7f) * shift;
    if (x & 0x80) break;
    shift <<= 7;
    data += shift;
  }
  return data;
}

/* BPS: returns a new buffer (malloc) with the patched ROM, or NULL (wrong ROM, damaged patch). */
static unsigned char *bps_apply(const unsigned char *src, int src_size, int *out_size, const unsigned char *p, int plen)
{
  int pos = 4, end = plen - 12;
  unsigned long ssz, tsz, meta, src_rel = 0, tgt_rel = 0, out_pos = 0;
  unsigned char *out;
  unsigned long crc_src, crc_tgt;

  if (plen < 20 || memcmp(p, "BPS1", 4)) return NULL;
  crc_src = p[end] | (p[end + 1] << 8) | (p[end + 2] << 16) | ((unsigned long)p[end + 3] << 24);
  crc_tgt = p[end + 4] | (p[end + 5] << 8) | (p[end + 6] << 16) | ((unsigned long)p[end + 7] << 24);
  if (crc32(0L, src, (uInt)src_size) != crc_src) return NULL;     /* made for a different ROM */

  ssz = bps_num(p, &pos, end);
  tsz = bps_num(p, &pos, end);
  meta = bps_num(p, &pos, end);
  if ((int)ssz != src_size || tsz == 0 || tsz > ROM_CAP) return NULL;
  if (meta > (unsigned long)(end - pos)) return NULL;          /* the notes cannot be longer than the patch */
  pos += (int)meta;
  out = (unsigned char *)calloc(1, tsz);
  if (!out) return NULL;

  while (pos < end && out_pos < tsz)
  {
    unsigned long cmd = bps_num(p, &pos, end), len = (cmd >> 2) + 1;
    int mode = (int)(cmd & 3);

    if (out_pos + len > tsz) { free(out); return NULL; }
    switch (mode)
    {
      case 0:   /* the same bytes of the original ROM */
        if (out_pos + len > (unsigned long)src_size) { free(out); return NULL; }
        memcpy(out + out_pos, src + out_pos, len);
        out_pos += len;
        break;
      case 1:   /* new bytes, in the patch */
        if (pos + (int)len > end) { free(out); return NULL; }
        memcpy(out + out_pos, p + pos, len);
        pos += (int)len; out_pos += len;
        break;
      case 2:   /* bytes from somewhere else in the original ROM */
      {
        unsigned long d = bps_num(p, &pos, end);
        long off = (long)(d >> 1);
        src_rel = (unsigned long)((long)src_rel + ((d & 1) ? -off : off));
        if (src_rel > (unsigned long)src_size || len > (unsigned long)src_size - src_rel) { free(out); return NULL; }   /* no wrap-around */
        memcpy(out + out_pos, src + src_rel, len);
        src_rel += len; out_pos += len;
        break;
      }
      default:  /* bytes already written (may overlap, so one at a time) */
      {
        unsigned long d = bps_num(p, &pos, end);
        long off = (long)(d >> 1), i;
        tgt_rel = (unsigned long)((long)tgt_rel + ((d & 1) ? -off : off));
        if (tgt_rel >= out_pos) { free(out); return NULL; }
        for (i = 0; i < (long)len; i++) out[out_pos++] = out[tgt_rel++];
        break;
      }
    }
  }
  if (out_pos != tsz || crc32(0L, out, (uInt)tsz) != crc_tgt) { free(out); return NULL; }
  *out_size = (int)tsz;
  return out;
}

/* Applies the .ips / .bps patch `patch` to the game in `load_path` and writes the result to a temporary file, whose path
   goes in `out`. Returns 1 on success; otherwise 0 with the reason in `what`. */
int patches_make_copy(const char *patch, const char *load_path, char *out, int out_len, char *what, int what_len)
{
  char ext[16] = "", dir[GUI_PATH_LEN];
  unsigned char *rom, *pdata, *result = NULL;
  int size, psize = 0, ok = 0, out_size = 0;
  const char *dot = strrchr(load_path, '.');

  if (dot && (!lstrcmpiA(dot, ".cue") || !lstrcmpiA(dot, ".chd") || !lstrcmpiA(dot, ".iso")))
  { snprintf(what, (size_t)what_len, "Disc games cannot be patched."); return 0; }

  rom = (unsigned char *)malloc(ROM_CAP);
  if (!rom) return 0;
  if (!what || what_len <= 0) { static char sink[8]; what = sink; what_len = sizeof(sink); }
  size = load_archive((char *)load_path, rom, ROM_CAP, ext);
  pdata = read_patch(patch, &psize);
  if (size <= 0 || !pdata) { free(rom); free(pdata); snprintf(what, (size_t)what_len, "The game or the patch could not be read."); return 0; }

  if (psize > 4 && !memcmp(pdata, "BPS1", 4))
  {
    result = bps_apply(rom, size, &out_size, pdata, psize);
    ok = result != NULL;
  }
  else if (psize > 5 && !memcmp(pdata, "PATCH", 5))
  {
    ok = ips_apply(rom, &size, ROM_CAP, pdata, psize);
    result = rom;
    out_size = size;
  }
  free(pdata);

  if (ok)
  {
    FILE *fp;
    GetTempPathA(sizeof(dir), dir);
    snprintf(out, (size_t)out_len, "%sgpexpatch.%s", dir, ext[0] ? ext : "bin");
    fp = fopen(out, "wb");
    if (fp)
    {
      ok = fwrite(result, 1, (size_t)out_size, fp) == (size_t)out_size;
      fclose(fp);
    }
    else ok = 0;
  }
  if (what)
  {
    const char *n = strrchr(patch, '\\');
    snprintf(what, (size_t)what_len, ok ? "Patch applied: %s" : "The patch %s does not fit this game, so it was not applied.", n ? n + 1 : patch);
  }
  if (result && result != rom) free(result);
  free(rom);
  return ok;
}
