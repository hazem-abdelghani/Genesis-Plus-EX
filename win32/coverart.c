/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  coverart.c -- see coverart.h. Decoding is stb_image (public domain,
 *  single header, bundled in this directory) rather than GDI+/WIC: both
 *  of those are COM/C++-flavored APIs that are painful to drive cleanly
 *  from plain C under MinGW, where this whole frontend already lives.
 ****************************************************************************/

#include <windows.h>
#include <stdio.h>
#include <math.h>
#include <zlib.h>

#include "shared.h"
#include "gui.h"
#include "coverart.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#include "stb_image.h"

/* Tried in this order against the same base name; whichever exists
   first wins. PNG first since that's what the feature was asked for
   and what coverart_set writes by default. */
static const char *cover_extensions[] = { ".png", ".jpg", ".jpeg", ".bmp", ".gif" };
#define COVER_EXT_COUNT (int)(sizeof(cover_extensions) / sizeof(cover_extensions[0]))

/* Same base-name extraction main.c's split_basename does (strip
   directory and extension), kept as its own small copy here rather
   than exposing that static function across files for one caller. */
static void base_name_only(const char *path, char *out, int out_len)
{
  const char *slash, *dot;
  const char *name;
  int len;

  slash = strrchr(path, '\\');
  name = slash ? slash + 1 : path;

  dot = strrchr(name, '.');
  len = dot ? (int)(dot - name) : (int)lstrlenA(name);
  if (len >= out_len) len = out_len - 1;

  memcpy(out, name, (size_t)len);
  out[len] = '\0';
}

/* The covers folder, looked up once: osd_path() hands out a few shared buffers that the other thread's calls would
   overwrite, and the covers are read from a thread of their own (coverart_prepare() is called before it starts). */
static char covers_cache[GUI_PATH_LEN];

static const char *covers_base(void)
{
  if (!covers_cache[0]) lstrcpynA(covers_cache, osd_path("covers"), sizeof(covers_cache));
  return covers_cache;
}

static void thumb_init(void);
void coverart_prepare(void) { covers_base(); thumb_init(); }

static void covers_dir(char *out, int out_len)
{
  lstrcpynA(out, covers_base(), out_len);
}

/* Folder name for a console, from the browser's console label. Matches the
   folder names used for saves, states and cheats. */
static const char *console_dir_name(const char *console)
{
  if (!console) return "Other";
  if (!lstrcmpiA(console, "Mega Drive / Genesis"))  return "MegaDrive";
  if (!lstrcmpiA(console, "Master System"))         return "MasterSystem";
  if (!lstrcmpiA(console, "Game Gear"))             return "GameGear";
  if (!lstrcmpiA(console, "SG-1000"))               return "SG1000";
  if (!lstrcmpiA(console, "Mega CD / Sega CD"))     return "MegaCD";
  return "Other";
}

/* covers\<Console> */
static void console_covers_dir(const char *console, char *out, int out_len)
{
  char tmp[GUI_PATH_LEN * 2];

  wsprintfA(tmp, "%s\\%s", covers_base(), console_dir_name(console));
  lstrcpynA(out, tmp, out_len);
}

static void build_candidate(const char *dir, const char *base, int ext_index, char *out, int out_len)
{
  char tmp[GUI_PATH_LEN * 2];

  wsprintfA(tmp, "%s\\%s%s", dir, base, cover_extensions[ext_index]);
  lstrcpynA(out, tmp, out_len);
}

/* Cache values: -2 unchecked, -1 none, 0..4 = extension found in the console's
   folder, COVER_FLAT + 0..4 = extension found in the flat covers folder that
   earlier versions used. */
#define COVER_FLAT 16

int coverart_find_cached(const char *rom_full_path, const char *console,
                         signed char *cache, char *found_path, int out_len)
{
  char cdir[GUI_PATH_LEN], fdir[GUI_PATH_LEN];
  char base[160];
  int i;

  if (*cache == -1) return 0;   /* already known: no cover on disk for this ROM */

  console_covers_dir(console, cdir, sizeof(cdir));
  covers_dir(fdir, sizeof(fdir));
  base_name_only(rom_full_path, base, sizeof(base));

  if (*cache >= COVER_FLAT)
  {
    build_candidate(fdir, base, *cache - COVER_FLAT, found_path, out_len);
    return 1;
  }
  if (*cache >= 0)
  {
    build_candidate(cdir, base, *cache, found_path, out_len);
    return 1;
  }

  /* COVERART_UNCHECKED: look in the console's folder first, then in the flat
     folder, and remember the answer either way. */
  for (i = 0; i < COVER_EXT_COUNT; i++)
  {
    char candidate[GUI_PATH_LEN];
    build_candidate(cdir, base, i, candidate, sizeof(candidate));
    if (GetFileAttributesA(candidate) != INVALID_FILE_ATTRIBUTES)
    {
      *cache = (signed char)i;
      lstrcpynA(found_path, candidate, out_len);
      return 1;
    }
  }
  for (i = 0; i < COVER_EXT_COUNT; i++)
  {
    char candidate[GUI_PATH_LEN];
    build_candidate(fdir, base, i, candidate, sizeof(candidate));
    if (GetFileAttributesA(candidate) != INVALID_FILE_ATTRIBUTES)
    {
      *cache = (signed char)(COVER_FLAT + i);
      lstrcpynA(found_path, candidate, out_len);
      return 1;
    }
  }

  *cache = -1;
  return 0;
}

int coverart_find(const char *rom_full_path, const char *console, char *found_path, int out_len)
{
  signed char throwaway = COVERART_UNCHECKED;
  return coverart_find_cached(rom_full_path, console, &throwaway, found_path, out_len);
}

/* The look of the grid covers (Settings > General): the frame shape and rounded corners. */
static int style_shape, style_round;

void coverart_set_style(int shape, int round)
{
  style_shape = shape < 0 || shape > 2 ? 0 : shape;
  style_round = round ? 1 : 0;
}

/* Where the frame of a cover sits inside its size x size tile (a box is portrait, a wide frame landscape). */
void coverart_frame_rect(int size, int *fx, int *fy, int *fw, int *fh)
{
  *fw = size; *fh = size;
  if (style_shape == 1) *fw = size * 3 / 4;
  else if (style_shape == 2) *fh = size * 3 / 4;
  *fx = (size - *fw) / 2;
  *fy = (size - *fh) / 2;
}

/* Rounds the corners of the rectangle x,y,w,h in a size x size top-down BGRA bitmap (premultiplied alpha). */
void coverart_round_corners(unsigned char *bits, int size, int x0, int y0, int w, int h)
{
  int r = (w < h ? w : h) / 8, cx, cy, i, j;

  if (!style_round || r < 2) return;
  for (cy = 0; cy < 2; cy++)
    for (cx = 0; cx < 2; cx++)
      for (j = 0; j < r; j++)
        for (i = 0; i < r; i++)
        {
          int px = cx ? x0 + w - 1 - i : x0 + i;
          int py = cy ? y0 + h - 1 - j : y0 + j;
          double dx = (r - 0.5) - i, dy = (r - 0.5) - j, d = sqrt(dx * dx + dy * dy), cov = r - d + 0.5;
          unsigned char *p;
          if (cov >= 1.0) continue;
          if (cov < 0.0) cov = 0.0;
          if (px < 0 || py < 0 || px >= size || py >= size) continue;
          p = bits + ((size_t)py * size + px) * 4;
          p[0] = (unsigned char)(p[0] * cov); p[1] = (unsigned char)(p[1] * cov);
          p[2] = (unsigned char)(p[2] * cov); p[3] = (unsigned char)(p[3] * cov);
        }
}

/* Scales the part cx,cy,cw,ch of an RGBA picture (stb_image's, straight alpha) into the rectangle dx,dy,dw,dh of a
   size x size premultiplied BGRA bitmap. Shrinking averages every source pixel under a destination pixel (a box
   filter: no shimmer or jagged edges, which nearest-neighbour gave on a large cover); enlarging blends the two nearest
   (bilinear). The colour is premultiplied by its alpha *before* averaging and stays that way: the bitmap is drawn
   with AlphaBlend, which expects it, and a transparent PNG no longer gets a bright or dark fringe along its edges. */
#define COVER_MAX_TAPS 256

/* The source pixels that make up destination pixel i along one axis: first source index, how many, their weights. */
static void axis_taps(int i, double off, double len, int dst_n, int src_n, int *first, int *count, float *w)
{
  double scale = len / dst_n, sum = 0.0;
  int k, n;

  if (scale >= 1.0)
  {
    double a = off + i * scale, b = a + scale;
    int k0 = (int)floor(a), k1 = (int)ceil(b) - 1, step = 1;

    if (k0 < 0) k0 = 0;
    if (k1 >= src_n) k1 = src_n - 1;
    if (k1 < k0) k1 = k0;
    if ((k1 - k0) / step + 1 > COVER_MAX_TAPS) step = (k1 - k0) / (COVER_MAX_TAPS - 1) + 1;     /* a huge picture: every few pixels */
    n = 0;
    for (k = k0; k <= k1 && n < COVER_MAX_TAPS; k += step, n++)
    {
      double lo = k > a ? k : a, hi = (k + 1) < b ? (k + 1) : b, ov = hi - lo;
      w[n] = (float)(ov > 0.0 ? ov : 0.0);
      sum += w[n];
    }
    *first = k0;
    *count = n;
    if (sum <= 0.0) { w[0] = 1.0f; sum = 1.0; *count = 1; }
  }
  else
  {
    double c = off + (i + 0.5) * scale - 0.5, f;
    int k0 = (int)floor(c);

    f = c - k0;
    if (k0 < 0) { k0 = 0; f = 0.0; }
    if (k0 >= src_n - 1) { k0 = src_n - 1; f = 0.0; }
    *first = k0;
    *count = (f > 0.0) ? 2 : 1;
    w[0] = (float)(1.0 - f);
    w[1] = (float)f;
    sum = (f > 0.0) ? 1.0 : w[0];
  }
  for (k = 0; k < *count; k++) w[k] = (float)(w[k] / sum);
}

static int resample_rgba(const unsigned char *src, int src_w, int src_h, int cx, int cy, int cw, int ch,
                         unsigned char *bits, int size, int dx, int dy, int dw, int dh, int straight)
{
  float *mid, wts[COVER_MAX_TAPS];
  int x, y, first, count, k, row_lo, row_hi;

  if (cw < 1 || ch < 1 || dw < 1 || dh < 1) return 0;
  row_lo = cy; row_hi = cy + ch - 1;
  if (row_lo < 0) row_lo = 0;
  if (row_hi >= src_h) row_hi = src_h - 1;
  mid = (float *)malloc(sizeof(float) * 4 * (size_t)dw * (size_t)(row_hi - row_lo + 1));
  if (!mid) return 0;

  /* across: every needed source row down to dw columns */
  for (x = 0; x < dw; x++)
  {
    axis_taps(x, cx, cw, dw, src_w, &first, &count, wts);
    for (y = row_lo; y <= row_hi; y++)
    {
      const unsigned char *row = src + (size_t)y * src_w * 4;
      float acc[4] = { 0, 0, 0, 0 }, *o = mid + ((size_t)(y - row_lo) * dw + x) * 4;
      for (k = 0; k < count; k++)
      {
        const unsigned char *p = row + (size_t)(first + k) * 4;
        float a = p[3] * (1.0f / 255.0f), wk = wts[k];
        acc[0] += p[0] * a * wk; acc[1] += p[1] * a * wk; acc[2] += p[2] * a * wk; acc[3] += a * wk;
      }
      o[0] = acc[0]; o[1] = acc[1]; o[2] = acc[2]; o[3] = acc[3];
    }
  }

  /* down: the rows, to dh */
  for (y = 0; y < dh; y++)
  {
    unsigned char *dst = bits + ((size_t)(dy + y) * size + dx) * 4;
    axis_taps(y, cy, ch, dh, src_h, &first, &count, wts);
    for (x = 0; x < dw; x++)
    {
      float acc[4] = { 0, 0, 0, 0 };
      for (k = 0; k < count; k++)
      {
        int r = first + k;
        const float *p;
        if (r < row_lo) r = row_lo;
        if (r > row_hi) r = row_hi;
        p = mid + ((size_t)(r - row_lo) * dw + x) * 4;
        acc[0] += p[0] * wts[k]; acc[1] += p[1] * wts[k]; acc[2] += p[2] * wts[k]; acc[3] += p[3] * wts[k];
      }
      if (straight)          /* RGBA, colour not multiplied by alpha: the shrunk copy kept for later scaling (see cover_source) */
      {
        float al = acc[3] > 0.0001f ? acc[3] : 0.0001f;
        int r = (int)(acc[0] / al + 0.5f), g = (int)(acc[1] / al + 0.5f), b = (int)(acc[2] / al + 0.5f), a = (int)(acc[3] * 255.0f + 0.5f);
        dst[x * 4 + 0] = (unsigned char)(r > 255 ? 255 : r); dst[x * 4 + 1] = (unsigned char)(g > 255 ? 255 : g);
        dst[x * 4 + 2] = (unsigned char)(b > 255 ? 255 : b); dst[x * 4 + 3] = (unsigned char)(a > 255 ? 255 : a);
      }
      else
      {
        int b = (int)(acc[2] + 0.5f), g = (int)(acc[1] + 0.5f), r = (int)(acc[0] + 0.5f), a = (int)(acc[3] * 255.0f + 0.5f);
        if (a > 255) a = 255;
        if (b > a) b = a;      /* premultiplied: a colour never exceeds its alpha */
        if (g > a) g = a;
        if (r > a) r = a;
        dst[x * 4 + 0] = (unsigned char)b; dst[x * 4 + 1] = (unsigned char)g;
        dst[x * 4 + 2] = (unsigned char)r; dst[x * 4 + 3] = (unsigned char)a;
      }
    }
  }
  free(mid);
  return 1;
}

/* Decoding a cover (a PNG of a thousand pixels takes tens of milliseconds) is most of the cost of showing it. The decoded
   picture is kept, shrunk to at most 512 pixels, for the next time the same game is drawn at another size (the grid
   size changed, the shape, the corners): then only the scaling is done again. Used from the cover thread only.
   The returned pixels belong to the cache: straight RGBA, valid until the next call. */
#define SRC_CACHE   48
#define SRC_MAX_DIM 512

static struct { char path[GUI_PATH_LEN]; FILETIME mt; int w, h; unsigned char *px; DWORD stamp; } src_cache[SRC_CACHE];
static DWORD src_tick;

static unsigned char *cover_source(const char *path, int *w, int *h)
{
  WIN32_FILE_ATTRIBUTE_DATA fa;
  int i, slot = -1, oldest = 0;
  unsigned char *pixels, *master;
  int sw, sh, ch;

  if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) return NULL;
  for (i = 0; i < SRC_CACHE; i++)
  {
    if (src_cache[i].px && !lstrcmpiA(src_cache[i].path, path) && !CompareFileTime(&src_cache[i].mt, &fa.ftLastWriteTime))
    {
      src_cache[i].stamp = ++src_tick;
      *w = src_cache[i].w; *h = src_cache[i].h;
      return src_cache[i].px;
    }
    if (!src_cache[i].px) { if (slot < 0) slot = i; }
    else if (src_cache[i].stamp < src_cache[oldest].stamp || !src_cache[oldest].px) oldest = i;
  }

  pixels = stbi_load(path, &sw, &sh, &ch, 4);
  if (!pixels) return NULL;
  master = pixels;
  if (sw > SRC_MAX_DIM || sh > SRC_MAX_DIM)
  {
    int mw = sw >= sh ? SRC_MAX_DIM : (int)((long long)SRC_MAX_DIM * sw / sh), mh = sw >= sh ? (int)((long long)SRC_MAX_DIM * sh / sw) : SRC_MAX_DIM;
    unsigned char *small;

    if (mw < 1) mw = 1;
    if (mh < 1) mh = 1;
    small = (unsigned char *)malloc((size_t)mw * mh * 4);
    if (small && resample_rgba(pixels, sw, sh, 0, 0, sw, sh, small, mw, 0, 0, mw, mh, 1))
    {
      stbi_image_free(pixels);
      master = small;
      sw = mw; sh = mh;
    }
    else free(small);                 /* could not shrink: the full picture is kept as it is */
  }

  if (slot < 0)
  {
    slot = oldest;
    if (src_cache[slot].px) { free(src_cache[slot].px); src_cache[slot].px = NULL; }
  }
  lstrcpynA(src_cache[slot].path, path, sizeof(src_cache[slot].path));
  src_cache[slot].mt = fa.ftLastWriteTime;
  src_cache[slot].w = sw; src_cache[slot].h = sh;
  src_cache[slot].px = master;
  src_cache[slot].stamp = ++src_tick;
  *w = sw; *h = sh;
  return master;
}


/* The thumbnail cache (Settings > General > Keep a Cover Cache): a finished cover, scaled and framed for one size and
   style, is kept compressed in ONE file, covers\thumbs.dat, and read back the next time instead of decoding and scaling
   the original. The file is a list of records; each says which picture it was made from (its size and modified time) and
   for what (tile size, frame, corners), and anything that does not match is made again from the original. The index of the
   records is read when the first cover is asked for. A new version of a record is added at the end (the old one stays as
   unused space); a half-written end after a crash is cut off. The file is wiped when it passes THUMB_CAP.
   Used from the cover thread, and Clear Cover Cache from the main one: thumb_lock covers both. */
#define THUMB_MAGIC 0x32424D54u      /* "TMB2" */
#define THUMB_CAP   (300u * 1024u * 1024u)
#define THUMB_MAXREC (16u * 1024u * 1024u)

typedef struct { unsigned magic, ver; } THUMBFILE;
typedef struct
{
  unsigned long long key;            /* a hash of the picture's path, the size and the style */
  unsigned size, style;              /* the tile size, and shape | round << 4 */
  unsigned src_lo, src_hi;           /* the original's size */
  unsigned mt_lo, mt_hi;             /* the original's modified time */
  unsigned rawlen, clen;             /* the picture's bytes, and what they take compressed (the bytes follow) */
} THUMBREC;

typedef struct { THUMBREC r; unsigned long long off; } THUMBIDX;      /* off: where the record starts; clen 0 = empty slot */

static volatile int thumb_on = 1;
static CRITICAL_SECTION thumb_lock;
static int thumb_lock_ready;
static HANDLE thumb_file = INVALID_HANDLE_VALUE;
static int thumb_state;              /* 0 not tried, 1 open, -1 not usable (another copy has it, no disk access) */
static THUMBIDX *thumb_ix;
static unsigned thumb_cap, thumb_count;
static unsigned long long thumb_end;
static unsigned long long thumb_dead;      /* bytes of records that a newer one replaced */

void coverart_set_cache(int on) { thumb_on = on ? 1 : 0; }

static void thumbs_dat_path(char *out, int n)
{
  char tmp[GUI_PATH_LEN + 16];
  wsprintfA(tmp, "%s\\thumbs.dat", covers_base());
  lstrcpynA(out, tmp, n);
}

static THUMBIDX *thumb_find(unsigned long long key)
{
  unsigned i;
  if (!thumb_cap) return NULL;
  for (i = (unsigned)key & (thumb_cap - 1); thumb_ix[i].r.clen; i = (i + 1) & (thumb_cap - 1))
    if (thumb_ix[i].r.key == key) return &thumb_ix[i];
  return NULL;
}

static int thumb_index_put(const THUMBREC *r, unsigned long long off)
{
  THUMBIDX *e = thumb_find(r->key);
  unsigned i;

  if (e) thumb_dead += sizeof(THUMBREC) + e->r.clen;       /* the older version of this cover is now unused space */
  if (!e)
  {
    if ((thumb_count + 1) * 2 > thumb_cap)         /* at most half full: grow */
    {
      unsigned ncap = thumb_cap ? thumb_cap * 2 : 1024, k;
      THUMBIDX *nix = (THUMBIDX *)calloc(ncap, sizeof(THUMBIDX));
      if (!nix) return 0;
      for (k = 0; k < thumb_cap; k++)
        if (thumb_ix[k].r.clen)
        {
          for (i = (unsigned)thumb_ix[k].r.key & (ncap - 1); nix[i].r.clen; i = (i + 1) & (ncap - 1)) ;
          nix[i] = thumb_ix[k];
        }
      free(thumb_ix);
      thumb_ix = nix; thumb_cap = ncap;
    }
    for (i = (unsigned)r->key & (thumb_cap - 1); thumb_ix[i].r.clen; i = (i + 1) & (thumb_cap - 1)) ;
    e = &thumb_ix[i];
    thumb_count++;
  }
  e->r = *r;
  e->off = off;
  return 1;
}

static void thumb_forget(void)       /* the index in memory and the open file, not the file on disk */
{
  if (thumb_file != INVALID_HANDLE_VALUE) CloseHandle(thumb_file);
  thumb_file = INVALID_HANDLE_VALUE;
  free(thumb_ix);
  thumb_ix = NULL; thumb_cap = thumb_count = 0; thumb_end = 0; thumb_dead = 0;
  thumb_state = 0;
}


/* Writes the live records to a new file and puts it in place of the old one, so replaced covers stop taking room. Lock held. */
static void thumb_compact(void)
{
  char path[GUI_PATH_LEN], tmp[GUI_PATH_LEN + 8];
  THUMBFILE head;
  HANDLE out;
  unsigned long long *noff, at;
  unsigned i;
  int ok = 1;
  DWORD n;
  LARGE_INTEGER pos;

  if (thumb_file == INVALID_HANDLE_VALUE || !thumb_cap) return;
  noff = (unsigned long long *)calloc(thumb_cap, sizeof(*noff));
  if (!noff) return;
  thumbs_dat_path(path, sizeof(path));
  wsprintfA(tmp, "%s.tmp", path);
  out = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (out == INVALID_HANDLE_VALUE) { free(noff); return; }
  head.magic = THUMB_MAGIC; head.ver = 1;
  at = sizeof(head);
  if (!WriteFile(out, &head, sizeof(head), &n, NULL) || n != sizeof(head)) ok = 0;
  for (i = 0; ok && i < thumb_cap; i++)
  {
    unsigned long long len;
    unsigned char *buf;
    if (!thumb_ix[i].r.clen) continue;
    len = sizeof(THUMBREC) + thumb_ix[i].r.clen;
    buf = (unsigned char *)malloc((size_t)len);
    pos.QuadPart = (LONGLONG)thumb_ix[i].off;
    if (!buf || !SetFilePointerEx(thumb_file, pos, NULL, FILE_BEGIN) || !ReadFile(thumb_file, buf, (DWORD)len, &n, NULL) || n != (DWORD)len ||
        !WriteFile(out, buf, (DWORD)len, &n, NULL) || n != (DWORD)len) ok = 0;
    free(buf);
    noff[i] = at;
    at += len;
  }
  CloseHandle(out);
  if (!ok) { DeleteFileA(tmp); free(noff); return; }
  CloseHandle(thumb_file);                  /* it is not shared for deleting or replacing: closed for the swap, opened again after */
  thumb_file = INVALID_HANDLE_VALUE;
  if (MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING))
  {
    for (i = 0; i < thumb_cap; i++) if (thumb_ix[i].r.clen) thumb_ix[i].off = noff[i];
    thumb_end = at;
    thumb_dead = 0;
  }
  else DeleteFileA(tmp);
  free(noff);
  thumb_file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (thumb_file == INVALID_HANDLE_VALUE) { thumb_forget(); thumb_state = -1; }      /* could not be opened again: no cache for now */
}

static int thumb_open(void)          /* called with the lock held */
{
  char path[GUI_PATH_LEN];
  THUMBFILE head;
  THUMBREC r;
  LARGE_INTEGER size, pos;
  DWORD n;
  unsigned long long at;

  if (thumb_state) return thumb_state > 0;
  thumb_state = -1;
  thumbs_dat_path(path, sizeof(path));
  CreateDirectoryA(covers_base(), NULL);
  /* Shared for reading only: a second copy of the program cannot open it for writing and so goes without the cache. */
  thumb_file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (thumb_file == INVALID_HANDLE_VALUE) return 0;
  if (!GetFileSizeEx(thumb_file, &size)) { thumb_forget(); thumb_state = -1; return 0; }

  head.magic = 0; head.ver = 0;
  if (size.QuadPart >= (LONGLONG)sizeof(head))
  {
    pos.QuadPart = 0;
    SetFilePointerEx(thumb_file, pos, NULL, FILE_BEGIN);
    if (!ReadFile(thumb_file, &head, sizeof(head), &n, NULL) || n != sizeof(head)) head.magic = 0;
  }
  if (head.magic != THUMB_MAGIC || (unsigned long long)size.QuadPart > THUMB_CAP)      /* new, not ours, or grown too big: start again */
  {
    pos.QuadPart = 0;
    SetFilePointerEx(thumb_file, pos, NULL, FILE_BEGIN);
    SetEndOfFile(thumb_file);
    head.magic = THUMB_MAGIC; head.ver = 1;
    if (!WriteFile(thumb_file, &head, sizeof(head), &n, NULL) || n != sizeof(head)) { thumb_forget(); thumb_state = -1; return 0; }
    thumb_end = sizeof(head);
    thumb_state = 1;
    return 1;
  }

  /* Read the records' headers (skipping the pictures) to build the index. */
  at = sizeof(head);
  for (;;)
  {
    if (at + sizeof(r) > (unsigned long long)size.QuadPart) break;
    pos.QuadPart = (LONGLONG)at;
    SetFilePointerEx(thumb_file, pos, NULL, FILE_BEGIN);
    if (!ReadFile(thumb_file, &r, sizeof(r), &n, NULL) || n != sizeof(r)) break;
    if (!r.clen || r.clen > THUMB_MAXREC || !r.size || r.size > 1024 || r.rawlen != r.size * r.size * 4u) break;
    if (at + sizeof(r) + r.clen > (unsigned long long)size.QuadPart) break;       /* cut off by a crash */
    if (!thumb_index_put(&r, at)) break;
    at += sizeof(r) + r.clen;
  }
  if (at < (unsigned long long)size.QuadPart)       /* whatever follows the last good record is dropped */
  {
    pos.QuadPart = (LONGLONG)at;
    SetFilePointerEx(thumb_file, pos, NULL, FILE_BEGIN);
    SetEndOfFile(thumb_file);
  }
  thumb_end = at;
  thumb_state = 1;
  if (thumb_dead > (1u << 20) && thumb_dead * 4 > thumb_end) thumb_compact();      /* a quarter of the file is replaced covers */
  return thumb_state > 0;
}

static unsigned long long thumb_key(const char *path, int size, int style)
{
  unsigned long long h = 1469598103934665603ULL;
  const char *p;
  for (p = path; *p; p++)
  {
    char c = *p >= 'A' && *p <= 'Z' ? (char)(*p + 32) : (*p == '/' ? '\\' : *p);
    h = (h ^ (unsigned char)c) * 1099511628211ULL;
  }
  h = (h ^ (unsigned)size) * 1099511628211ULL;
  h = (h ^ (unsigned)style) * 1099511628211ULL;
  return h ? h : 1;
}

static int thumb_stat(const char *path, THUMBREC *h)
{
  WIN32_FILE_ATTRIBUTE_DATA fa;
  if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) return 0;
  h->src_lo = fa.nFileSizeLow; h->src_hi = fa.nFileSizeHigh;
  h->mt_lo = fa.ftLastWriteTime.dwLowDateTime; h->mt_hi = fa.ftLastWriteTime.dwHighDateTime;
  return 1;
}

static HBITMAP thumb_new(int size, unsigned char **bits)
{
  BITMAPINFO bi;
  ZeroMemory(&bi, sizeof(bi));
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = size;
  bi.bmiHeader.biHeight = -size;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  return CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)bits, NULL, 0);
}

static HBITMAP thumb_read(const char *path, int size, int style)
{
  THUMBREC want;
  THUMBIDX *e;
  LARGE_INTEGER pos;
  DWORD n;
  unsigned char *comp = NULL, *bits = NULL;
  HBITMAP bmp = NULL;
  uLongf rawlen;
  unsigned clen = 0;

  if (!thumb_lock_ready) return NULL;
  ZeroMemory(&want, sizeof(want));
  if (!thumb_stat(path, &want)) return NULL;
  want.key = thumb_key(path, size, style);

  EnterCriticalSection(&thumb_lock);
  if (thumb_open() && (e = thumb_find(want.key)) != NULL &&
      e->r.size == (unsigned)size && e->r.style == (unsigned)style &&
      e->r.src_lo == want.src_lo && e->r.src_hi == want.src_hi && e->r.mt_lo == want.mt_lo && e->r.mt_hi == want.mt_hi)
  {
    clen = e->r.clen;
    comp = (unsigned char *)malloc(clen);
    pos.QuadPart = (LONGLONG)(e->off + sizeof(THUMBREC));
    if (comp && (!SetFilePointerEx(thumb_file, pos, NULL, FILE_BEGIN) || !ReadFile(thumb_file, comp, clen, &n, NULL) || n != clen))
    { free(comp); comp = NULL; }
  }
  LeaveCriticalSection(&thumb_lock);
  if (!comp) return NULL;

  bmp = thumb_new(size, &bits);
  rawlen = (uLongf)((unsigned)size * (unsigned)size * 4u);
  if (!bmp || uncompress(bits, &rawlen, comp, clen) != Z_OK || rawlen != (uLongf)((unsigned)size * (unsigned)size * 4u))
  {
    if (bmp) DeleteObject(bmp);
    bmp = NULL;
  }
  free(comp);
  return bmp;
}

static void thumb_write(const char *path, int size, HBITMAP bmp, int style)
{
  THUMBREC h;
  DIBSECTION ds;
  uLongf clen;
  unsigned char *buf;
  LARGE_INTEGER pos;
  DWORD n;
  unsigned rawlen = (unsigned)size * (unsigned)size * 4u;

  if (!thumb_lock_ready) return;
  if (!GetObject(bmp, sizeof(ds), &ds) || !ds.dsBm.bmBits || ds.dsBm.bmWidth != size || ds.dsBm.bmHeight != size) return;
  ZeroMemory(&h, sizeof(h));
  if (!thumb_stat(path, &h)) return;
  h.key = thumb_key(path, size, style);
  h.size = (unsigned)size;
  h.style = (unsigned)style;
  h.rawlen = rawlen;
  clen = compressBound(rawlen);
  buf = (unsigned char *)malloc(sizeof(h) + clen);      /* the record's header and its picture, written in one go */
  if (!buf) return;
  if (compress2(buf + sizeof(h), &clen, (const Bytef *)ds.dsBm.bmBits, rawlen, 1) != Z_OK || clen == 0 || clen > THUMB_MAXREC) { free(buf); return; }
  h.clen = (unsigned)clen;
  memcpy(buf, &h, sizeof(h));

  EnterCriticalSection(&thumb_lock);
  if (thumb_open() && thumb_end + sizeof(h) + clen <= THUMB_CAP)      /* over the cap: no more until the next start wipes it */
  {
    pos.QuadPart = (LONGLONG)thumb_end;
    if (SetFilePointerEx(thumb_file, pos, NULL, FILE_BEGIN) &&
        WriteFile(thumb_file, buf, (DWORD)(sizeof(h) + clen), &n, NULL) && n == (DWORD)(sizeof(h) + clen))
    {
      if (thumb_index_put(&h, thumb_end)) { thumb_end += sizeof(h) + clen; if (thumb_dead > (16u << 20) && thumb_dead * 2 > thumb_end) thumb_compact(); }
      else { SetFilePointerEx(thumb_file, pos, NULL, FILE_BEGIN); SetEndOfFile(thumb_file); }
    }
    else { SetFilePointerEx(thumb_file, pos, NULL, FILE_BEGIN); SetEndOfFile(thumb_file); }      /* a failed write leaves no half record */
  }
  LeaveCriticalSection(&thumb_lock);
  free(buf);
}

/* Settings > General > Clear Cover Cache: deletes the file. Returns how many covers it held. */
int coverart_clear_cache(void)
{
  char path[GUI_PATH_LEN];
  int n;

  if (!thumb_lock_ready) return 0;
  EnterCriticalSection(&thumb_lock);
  thumb_open();                        /* to count what is in it */
  n = (int)thumb_count;
  thumb_forget();
  thumbs_dat_path(path, sizeof(path));
  DeleteFileA(path);
  LeaveCriticalSection(&thumb_lock);
  return n;
}

static void thumb_init(void)
{
  if (!thumb_lock_ready) { InitializeCriticalSection(&thumb_lock); thumb_lock_ready = 1; }
  thumb_on = gui.cover_cache ? 1 : 0;       /* the setting, from the start */
}

/* Letterbox fit into a size x size square (or fill of the cover frame, see coverart_set_style), smoothly scaled by
   resample_rgba() above. Padding is left fully transparent (alpha 0) so the control's own background shows through
   whatever the source image's own aspect ratio doesn't fill. */
static HBITMAP load_and_fit_raw(const char *path, int size)
{
  int src_w, src_h;
  unsigned char *pixels;
  HBITMAP hbmp;
  BITMAPINFO bmi;
  unsigned char *dib_bits;
  int fit_w, fit_h, off_x, off_y;

  pixels = cover_source(path, &src_w, &src_h);
  if (!pixels) return NULL;

  if (style_shape != 0)
  {
    /* A box or wide frame: the picture fills it (the overflow is cropped, evenly all round). */
    int fx, fy, fw, fh, cw, ch, cx0, cy0;
    BITMAPINFO bi;
    unsigned char *bits;
    HBITMAP hb;

    coverart_frame_rect(size, &fx, &fy, &fw, &fh);
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    hb = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    if (!hb) {  return NULL; }
    ZeroMemory(bits, (size_t)size * size * 4);
    if ((long)src_w * fh > (long)src_h * fw) { ch = src_h; cw = (int)((long)src_h * fw / fh); }
    else { cw = src_w; ch = (int)((long)src_w * fh / fw); }
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    cx0 = (src_w - cw) / 2;
    cy0 = (src_h - ch) / 2;
    resample_rgba(pixels, src_w, src_h, cx0, cy0, cw, ch, bits, size, fx, fy, fw, fh, 0);
    
    coverart_round_corners(bits, size, fx, fy, fw, fh);
    return hb;
  }

  if (src_w >= src_h)
  {
    fit_w = size;
    fit_h = (int)((long)size * src_h / src_w);
  }
  else
  {
    fit_h = size;
    fit_w = (int)((long)size * src_w / src_h);
  }
  if (fit_w < 1) fit_w = 1;
  if (fit_h < 1) fit_h = 1;
  off_x = (size - fit_w) / 2;
  off_y = (size - fit_h) / 2;

  ZeroMemory(&bmi, sizeof(bmi));
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = size;
  bmi.bmiHeader.biHeight = -size;   /* negative = top-down, matches how we fill it below */
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  hbmp = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, (void **)&dib_bits, NULL, 0);
  if (!hbmp)
  {
    
    return NULL;
  }

  ZeroMemory(dib_bits, (size_t)size * size * 4);   /* transparent letterbox bars */

  resample_rgba(pixels, src_w, src_h, 0, 0, src_w, src_h, dib_bits, size, off_x, off_y, fit_w, fit_h, 0);

  
  coverart_round_corners(dib_bits, size, off_x, off_y, fit_w, fit_h);
  return hbmp;
}

static HBITMAP load_and_fit(const char *path, int size)
{
  HBITMAP bmp;
  int style = style_shape | (style_round << 4);       /* the look this cover is made for; the main thread may change it meanwhile */
  if (!thumb_on) return load_and_fit_raw(path, size);
  bmp = thumb_read(path, size, style);
  if (bmp) return bmp;
  bmp = load_and_fit_raw(path, size);
  if (bmp && style == (style_shape | (style_round << 4))) thumb_write(path, size, bmp, style);      /* not kept when the look changed while it was made */
  return bmp;
}

HBITMAP coverart_load_cached(const char *rom_full_path, const char *console, int size, signed char *cache)
{
  char path[GUI_PATH_LEN];
  if (!coverart_find_cached(rom_full_path, console, cache, path, sizeof(path))) return NULL;
  return load_and_fit(path, size);
}

/* Index into cover_extensions of a file name's extension, or -1. */
static int cover_ext_index(const char *file)
{
  const char *dot = strrchr(file, '.');
  int i;

  if (!dot) return -1;
  for (i = 0; i < COVER_EXT_COUNT; i++)
  {
    if (!lstrcmpiA(dot, cover_extensions[i])) return i;
  }
  return -1;
}

int coverart_set(const char *rom_full_path, const char *console, const char *source_image_path)
{
  char dir[GUI_PATH_LEN];
  char base[160];
  char dest[GUI_PATH_LEN];
  int ext = cover_ext_index(source_image_path);
  int i;

  /* Only formats the finder looks for: anything else would be copied and then
     never shown. */
  if (ext < 0) return 0;

  CreateDirectoryA(covers_base(), NULL);
  console_covers_dir(console, dir, sizeof(dir));
  CreateDirectoryA(dir, NULL);

  base_name_only(rom_full_path, base, sizeof(base));
  build_candidate(dir, base, ext, dest, sizeof(dest));

  /* Choosing the cover that is already in place: nothing to do (and nothing
     to delete). */
  if (lstrcmpiA(source_image_path, dest) != 0)
  {
    /* Copy first; the old cover is only replaced once this has worked. */
    if (!CopyFileA(source_image_path, dest, FALSE)) return 0;
  }

  /* Never more than one cover per game in this folder. */
  for (i = 0; i < COVER_EXT_COUNT; i++)
  {
    char other[GUI_PATH_LEN];
    if (i == ext) continue;
    build_candidate(dir, base, i, other, sizeof(other));
    DeleteFileA(other);
  }
  return 1;
}

/* Deletes the game's cover(s) in its console folder and any in the flat folder
   from earlier versions (which would otherwise show through again). */
int coverart_remove(const char *rom_full_path, const char *console)
{
  char cdir[GUI_PATH_LEN], fdir[GUI_PATH_LEN], base[160];
  int pass, i, removed = 0;

  console_covers_dir(console, cdir, sizeof(cdir));
  covers_dir(fdir, sizeof(fdir));
  base_name_only(rom_full_path, base, sizeof(base));

  for (pass = 0; pass < 2; pass++)
  {
    for (i = 0; i < COVER_EXT_COUNT; i++)
    {
      char p[GUI_PATH_LEN];
      build_candidate(pass ? fdir : cdir, base, i, p, sizeof(p));
      if (DeleteFileA(p)) removed = 1;
    }
  }
  return removed;
}
