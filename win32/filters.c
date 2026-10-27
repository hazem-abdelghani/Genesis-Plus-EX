/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  filters.c -- built-in ("native") render filters.
 *
 *  These replace the old Kega Fusion .rpi plugin loader. An .rpi file is a
 *  32-bit DLL, which a 64-bit process cannot load; a filter that is simply
 *  part of the executable has no such limit, and cannot crash the emulator
 *  the way a third-party DLL could.
 *
 *  Provenance, because it matters for licensing: every filter in THIS file is
 *  written from the published algorithm descriptions, not derived from any
 *  other project's source code -- with one exception, xBRZ, which is Zenju's
 *  GPLv3 code in xbrz/ and is only driven from here (see xbrz/README-xbrz.txt
 *  before distributing a binary that contains it; build with XBRZ=0 to leave
 *  it out).
 *
 *    Scale2x / Scale3x  AdvMAME's pixel-art scalers (Andrea Mazzoleni)
 *    Eagle 2x           the classic Eagle rules
 *    Smooth 2x / 4x     an edge-directed corner smoother in the spirit of
 *                       xBR (which is what it is *not*: it is a compact,
 *                       independently written implementation of the same
 *                       idea, so its output resembles xBR/hq2x but is not
 *                       identical to either)
 *    Scanlines / CRT / LCD Grid / Glow Scanlines   plain arithmetic, nothing borrowed
 *    Sharp Bilinear     nearest pixels with a short blend along each seam
 *    Lanczos, Catmull-Rom, B-Spline, Mitchell 3x / 4x   separable resampling, from the textbook formulas
 *    HQ2x - HQ4x, xBR 2x - 4x   the real hqx (Maxim Stepin) and xBR (Hyllian), from the
 *                       libxbr-standalone library in xbr/ (LGPL)
 *    LQ2x               the rules of RetroArch's lq2x filter
 *    ScaleFX 3x         Sp00kyFox's ScaleFX (MIT), ported from its five shader passes
 *    CRT Geom 3x / 4x  cgwg's and DOLLS' CRT-Geom (GPL 2 or later), ported from its shader
 *    2xSaI              Kreed's published rules for "Scale and Interpolate"
 *    Super Eagle, Super 2xSaI   Kreed's published rules, written from their descriptions
 *    xBRZ 2x - 6x       the real xBRZ by Zenju (xbrz/), optional (GPEX_XBRZ)
 *
 *  Design notes
 *
 *  - Input and output are both RGB565, matching the core's framebuffer and
 *    the DIB the frontend presents from, so no format conversion is needed.
 *  - The edge-detecting filters read up to two pixels past every edge of the
 *    frame. Rather than bounds-check every read, the frame is first copied
 *    into a scratch buffer with a two-pixel border made by repeating the
 *    outermost pixels; the inner loops then index freely.
 *  - Similarity is measured in a luma-weighted YUV-like space, computed once
 *    per pixel per frame into a parallel array, rather than once per
 *    comparison.
 *  - Scratch buffers are allocated lazily, only grow, and are freed by
 *    filter_shutdown().
 ****************************************************************************/

#include "filters.h"
#include "xbr/libxbr.h"

#ifdef GPEX_XBRZ
#include "xbrz_glue.h"
#endif

#include <math.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef uint16_t px_t;

#define PAD 2

/* Address of row n of a destination with a byte pitch. */
#define ROW(base, pitch, n) ((px_t *)((uint8_t *)(base) + (size_t)(n) * (size_t)(pitch)))

/****************************************************************************
 * Scratch memory
 ****************************************************************************/

typedef struct
{
  void  *p;
  size_t cap;
} scratch_t;

static scratch_t sc_pad1, sc_ycc1, sc_pad2, sc_ycc2;
static scratch_t sc_xin, sc_xout;                  /* the 32-bit frames of the xBR and HQx filters */
static xbr_data *xbr_table;                        /* ... and their color table */
static scratch_t sc_sfx0, sc_sfx1, sc_sfx2, sc_sfx3;   /* the passes of ScaleFX */

/* Set when a filter could not get its scratch memory and so wrote nothing;
   filter_apply() then reports that instead of claiming success. */
static int alloc_failed;

static void *scratch_get(scratch_t *s, size_t bytes)
{
  if (bytes > s->cap)
  {
    free(s->p);
    s->p   = malloc(bytes);
    s->cap = s->p ? bytes : 0;
    if (!s->p) alloc_failed = 1;
  }
  return s->p;
}

/* Gives back what the xBR, HQx, ScaleFX and CRT Geom filters keep (up to 46 MB for the largest). */
static void cg_release(void);

static void libxbr_release(void)
{
  cg_release();
  free(sc_sfx0.p); sc_sfx0.p = NULL; sc_sfx0.cap = 0;
  free(sc_sfx1.p); sc_sfx1.p = NULL; sc_sfx1.cap = 0;
  free(sc_sfx2.p); sc_sfx2.p = NULL; sc_sfx2.cap = 0;
  free(sc_sfx3.p); sc_sfx3.p = NULL; sc_sfx3.cap = 0;
  free(sc_xin.p);  sc_xin.p = NULL;  sc_xin.cap = 0;
  free(sc_xout.p); sc_xout.p = NULL; sc_xout.cap = 0;
  free(xbr_table); xbr_table = NULL;
}

void filter_shutdown(void)
{
  libxbr_release();
#ifdef GPEX_XBRZ
  xbrz_glue_release();
#endif
  free(sc_pad1.p); sc_pad1.p = NULL; sc_pad1.cap = 0;
  free(sc_ycc1.p); sc_ycc1.p = NULL; sc_ycc1.cap = 0;
  free(sc_pad2.p); sc_pad2.p = NULL; sc_pad2.cap = 0;
  free(sc_ycc2.p); sc_ycc2.p = NULL; sc_ycc2.cap = 0;
}

/****************************************************************************
 * Work shared between processors
 *
 * The heavy filters (CRT Geom) cut their output rows into bands and give one band to each processor (at most four). Where there
 * is only one processor, or no thread API, everything runs in the calling thread.
 ****************************************************************************/

typedef void (*rows_fn)(void *ctx, int from, int to);

#ifdef _WIN32
typedef struct { rows_fn fn; void *ctx; int from, to; } rows_task_t;

static DWORD WINAPI rows_thread(LPVOID p)
{
  rows_task_t *t = (rows_task_t *)p;

  t->fn(t->ctx, t->from, t->to);
  return 0;
}
#endif

static void run_rows(rows_fn fn, void *ctx, int rows)
{
#ifdef _WIN32
  static int cpus;
  rows_task_t task[4];
  HANDLE      th[4];
  int         i, n, started = 0;

  if (!cpus)
  {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    cpus = si.dwNumberOfProcessors < 1 ? 1 : si.dwNumberOfProcessors > 4 ? 4 : (int)si.dwNumberOfProcessors;
  }
  n = rows < 64 ? 1 : cpus;
  for (i = 0; i < n; i++)
  {
    task[i].fn = fn; task[i].ctx = ctx; task[i].from = rows * i / n; task[i].to = rows * (i + 1) / n;
  }
  for (i = 1; i < n; i++)                                    /* the first band is done here, the others in threads */
  {
    th[started] = CreateThread(NULL, 0, rows_thread, &task[i], 0, NULL);
    if (th[started]) started++; else fn(ctx, task[i].from, task[i].to);
  }
  fn(ctx, task[0].from, task[0].to);
  if (started)
  {
    WaitForMultipleObjects((DWORD)started, th, TRUE, INFINITE);
    for (i = 0; i < started; i++) CloseHandle(th[i]);
  }
#else
  fn(ctx, 0, rows);
#endif
}

/****************************************************************************
 * Padded planes
 *
 * A plane is a (w + 2*PAD) x (h + 2*PAD) buffer. Callers get a pointer to
 * pixel (0,0) and a stride, and may read anything in [-PAD, w+PAD-1] x
 * [-PAD, h+PAD-1].
 ****************************************************************************/

static px_t *plane_alloc(scratch_t *s, int w, int h, int *stride)
{
  size_t st = (size_t)w + 2 * PAD;
  px_t  *b  = (px_t *)scratch_get(s, st * ((size_t)h + 2 * PAD) * sizeof(px_t));

  if (!b) return NULL;
  *stride = (int)st;
  return b + PAD * st + PAD;
}

/* Fills the border by repeating the outermost pixels. */
static void plane_extend(px_t *org, int stride, int w, int h)
{
  int x, y;

  for (y = 0; y < h; y++)
  {
    px_t *row = org + (ptrdiff_t)y * stride;
    for (x = 1; x <= PAD; x++)
    {
      row[-x]        = row[0];
      row[w - 1 + x] = row[w - 1];
    }
  }

  for (y = 1; y <= PAD; y++)
  {
    memcpy(org - (ptrdiff_t)y * stride - PAD,
           org - PAD, (size_t)stride * sizeof(px_t));
    memcpy(org + (ptrdiff_t)(h - 1 + y) * stride - PAD,
           org + (ptrdiff_t)(h - 1) * stride - PAD, (size_t)stride * sizeof(px_t));
  }
}

static px_t *plane_from_src(scratch_t *s, const px_t *src, int pitch,
                            int w, int h, int *stride)
{
  px_t *org = plane_alloc(s, w, h, stride);
  int   y;

  if (!org) return NULL;

  for (y = 0; y < h; y++)
  {
    memcpy(org + (ptrdiff_t)y * (*stride),
           (const uint8_t *)src + (size_t)y * (size_t)pitch,
           (size_t)w * sizeof(px_t));
  }

  plane_extend(org, *stride, w, h);
  return org;
}

/****************************************************************************
 * Color helpers
 ****************************************************************************/

/* Blend a toward b. alpha is 0..32 (32 = all b). Works on all three RGB565
   fields at once by spreading them apart with a mask so the multiply of one
   field cannot bleed into the next. */
static inline px_t mix(px_t a, px_t b, unsigned alpha)
{
  uint32_t x = ((uint32_t)a | ((uint32_t)a << 16)) & 0x07E0F81Fu;
  uint32_t y = ((uint32_t)b | ((uint32_t)b << 16)) & 0x07E0F81Fu;

  x += ((y - x) * alpha) >> 5;
  x &= 0x07E0F81Fu;
  return (px_t)(x | (x >> 16));
}

/* Luma plus two scaled color-difference channels. Compact enough that a
   whole frame of them stays cache-friendly. */
typedef struct
{
  uint8_t y;
  int8_t  u, v, pad;
} ycc_t;

static inline void to_ycc(px_t c, ycc_t *o)
{
  int r = (c >> 11) & 31;
  int g = (c >> 5) & 63;
  int b = c & 31;
  int y;

  r = (r << 3) | (r >> 2);
  g = (g << 2) | (g >> 4);
  b = (b << 3) | (b >> 2);

  y    = (77 * r + 150 * g + 29 * b) >> 8;
  o->y = (uint8_t)y;
  o->u = (int8_t)((b - y) >> 1);
  o->v = (int8_t)((r - y) >> 1);
  o->pad = 0;
}

/* Builds the color-space plane for a padded pixel plane (same layout). */
static ycc_t *ycc_from_plane(scratch_t *s, const px_t *org, int stride, int w, int h)
{
  size_t  total = (size_t)stride * ((size_t)h + 2 * PAD);
  ycc_t  *base  = (ycc_t *)scratch_get(s, total * sizeof(ycc_t));
  const px_t *src = org - (ptrdiff_t)PAD * stride - PAD;
  size_t  i;

  (void)w;
  if (!base) return NULL;

  for (i = 0; i < total; i++) to_ycc(src[i], &base[i]);

  return base + (ptrdiff_t)PAD * stride + PAD;
}

/* Perceptual-ish distance between two pixels. Luma counts most, which is
   what makes an edge look like an edge to a person. Roughly: two different
   colors from the Mega Drive's 512-color palette are always > EQ_T apart,
   while smooth-gradient noise stays under it. */
static inline int cdist(const ycc_t *a, const ycc_t *b)
{
  int dy = (int)a->y - (int)b->y;
  int du = (int)a->u - (int)b->u;
  int dv = (int)a->v - (int)b->v;

  if (dy < 0) dy = -dy;
  if (du < 0) du = -du;
  if (dv < 0) dv = -dv;

  return 4 * dy + 2 * du + 2 * dv;
}

#define EQ_T 24   /* at or below this, two colors count as "the same" */

/****************************************************************************
 * Scale2x / Scale3x  (AdvMAME)
 ****************************************************************************/

static void f_scale2x(const px_t *src, int sp, int w, int h, px_t *dst, int dp)
{
  int stride, x, y;
  const px_t *P = plane_from_src(&sc_pad1, src, sp, w, h, &stride);

  if (!P) return;

  for (y = 0; y < h; y++)
  {
    const px_t *r  = P + (ptrdiff_t)y * stride;
    px_t       *o0 = ROW(dst, dp, 2 * y);
    px_t       *o1 = ROW(dst, dp, 2 * y + 1);

    for (x = 0; x < w; x++)
    {
      px_t B = r[x - stride], D = r[x - 1], E = r[x], F = r[x + 1], H = r[x + stride];
      px_t e0 = E, e1 = E, e2 = E, e3 = E;

      if (B != H && D != F)
      {
        e0 = (D == B) ? D : E;
        e1 = (B == F) ? F : E;
        e2 = (D == H) ? D : E;
        e3 = (H == F) ? F : E;
      }

      o0[2 * x] = e0; o0[2 * x + 1] = e1;
      o1[2 * x] = e2; o1[2 * x + 1] = e3;
    }
  }
}

static void f_scale3x(const px_t *src, int sp, int w, int h, px_t *dst, int dp)
{
  int stride, x, y;
  const px_t *P = plane_from_src(&sc_pad1, src, sp, w, h, &stride);

  if (!P) return;

  for (y = 0; y < h; y++)
  {
    const px_t *r  = P + (ptrdiff_t)y * stride;
    px_t       *o0 = ROW(dst, dp, 3 * y);
    px_t       *o1 = ROW(dst, dp, 3 * y + 1);
    px_t       *o2 = ROW(dst, dp, 3 * y + 2);

    for (x = 0; x < w; x++)
    {
      px_t A = r[x - stride - 1], B = r[x - stride], C = r[x - stride + 1];
      px_t D = r[x - 1],          E = r[x],          F = r[x + 1];
      px_t G = r[x + stride - 1], H = r[x + stride], I = r[x + stride + 1];
      px_t e0 = E, e1 = E, e2 = E, e3 = E, e4 = E, e5 = E, e6 = E, e7 = E, e8 = E;

      if (B != H && D != F)
      {
        e0 = (D == B) ? D : E;
        e1 = ((D == B && E != C) || (B == F && E != A)) ? B : E;
        e2 = (B == F) ? F : E;
        e3 = ((D == B && E != G) || (D == H && E != A)) ? D : E;
        e5 = ((B == F && E != I) || (H == F && E != C)) ? F : E;
        e6 = (D == H) ? D : E;
        e7 = ((D == H && E != I) || (H == F && E != G)) ? H : E;
        e8 = (H == F) ? F : E;
      }

      o0[3 * x] = e0; o0[3 * x + 1] = e1; o0[3 * x + 2] = e2;
      o1[3 * x] = e3; o1[3 * x + 1] = e4; o1[3 * x + 2] = e5;
      o2[3 * x] = e6; o2[3 * x + 1] = e7; o2[3 * x + 2] = e8;
    }
  }
}

/****************************************************************************
 * Eagle 2x
 ****************************************************************************/

static void f_eagle2x(const px_t *src, int sp, int w, int h, px_t *dst, int dp)
{
  int stride, x, y;
  const px_t *P = plane_from_src(&sc_pad1, src, sp, w, h, &stride);

  if (!P) return;

  for (y = 0; y < h; y++)
  {
    const px_t *r  = P + (ptrdiff_t)y * stride;
    px_t       *o0 = ROW(dst, dp, 2 * y);
    px_t       *o1 = ROW(dst, dp, 2 * y + 1);

    for (x = 0; x < w; x++)
    {
      px_t S = r[x - stride - 1], T = r[x - stride], U = r[x - stride + 1];
      px_t V = r[x - 1],          C = r[x],          W = r[x + 1];
      px_t X = r[x + stride - 1], Y = r[x + stride], Z = r[x + stride + 1];
      px_t e0 = C, e1 = C, e2 = C, e3 = C;

      if (V == S && S == T) e0 = S;
      if (T == U && U == W) e1 = U;
      if (V == X && X == Y) e2 = X;
      if (W == Z && Z == Y) e3 = Z;

      o0[2 * x] = e0; o0[2 * x + 1] = e1;
      o1[2 * x] = e2; o1[2 * x + 1] = e3;
    }
  }
}

/****************************************************************************
 * Smooth 2x / 4x -- edge-directed corner smoothing
 *
 * Every source pixel becomes a 2x2 block that starts out as four copies of
 * the pixel. Then each of the four corners is examined on its own:
 *
 *      B
 *    D E F        (for the bottom-right corner; the other three are the
 *      H I         same picture reflected)
 *
 * If E differs from both F and H, the corner between them might be a cut
 * across a diagonal edge. Two costs are compared, each the summed color
 * change along one diagonal direction through the neighborhood:
 *
 *   anti  -- along the F-H diagonal (and the diagonals either side of it)
 *   main  -- along the E-I diagonal (and the ones either side of it)
 *
 * Small cost = the color holds steady in that direction = an edge runs that
 * way. If the anti-diagonal is the steadier one, F and H belong to the same
 * feature and E's corner is cut off, so that sub-pixel is blended toward
 * whichever of F/H is closer to E. A cleanly matched pair (F ~ H) is blended
 * harder than a merely plausible one. A corner is only cut if E itself
 * continues away from it (E matches the pixel behind it on at least one
 * side). Everything else stays untouched, which is what keeps flat areas,
 * dithering, single-pixel details and 1-pixel lines from smearing or eroding.
 *
 * The same 12-pixel neighborhood is used at every corner by reflecting the
 * two step vectors u (toward F) and v (toward H).
 ****************************************************************************/

/* Blend strengths, out of 32, for the corner sub-pixel:
     SMOOTH_CLEAN  F and H match each other, so the corner is a clean cut and
                   the sub-pixel takes the neighbor's color outright;
     SMOOTH_SOFT   F and H differ, so it is only a plausible edge and gets a
                   partial blend.
   Tuned by eye on pixel-art test scenes (round outlines, 1-pixel diagonals,
   sprites, text) at both 2x and through the 4x cascade. Softer values (16/8)
   leave the 2x result barely smoother than a plain resize, and fall apart at
   4x: the second pass sees the first pass's softened stair-steps as broken
   and turns a 1-pixel line into a chain of dots. */
#define SMOOTH_CLEAN 32
#define SMOOTH_SOFT  12

static inline px_t smooth_corner(const px_t *p, const ycc_t *q, int u, int v)
{
  const ycc_t *qE = q;
  const ycc_t *qF = q + u;
  const ycc_t *qH = q + v;
  const ycc_t *qI = q + u + v;
  px_t         E  = p[0];
  int dEF, dEH, anti, main_, dFH;

  /* Exact match with a neighbor is by far the commonest case; settle it
     without computing any distances. (Same answer as the test below.) */
  if (E == p[u] || E == p[v]) return E;

  dEF = cdist(qE, qF);
  dEH = cdist(qE, qH);

  /* E already matches one of the two neighbors: nothing to cut. */
  if (dEF <= EQ_T || dEH <= EQ_T) return E;

  /* E must continue away from this corner, i.e. match the pixel behind it on
     one side. Without this, an isolated pixel (an eye, a dot of a letter, a
     bright star) counts as a corner of its surroundings on all four sides and
     gets eroded, and a 1-pixel line loses its thickness. */
  if (cdist(qE, q - u) > EQ_T && cdist(qE, q - v) > EQ_T) return E;

  anti = cdist(qE, q + u - v) + cdist(qE, q + v - u)
       + cdist(qI, q + 2 * u) + cdist(qI, q + 2 * v)
       + 4 * cdist(qH, qF);

  main_ = cdist(qH, q - u) + cdist(qH, q + u + 2 * v)
        + cdist(qF, q + 2 * u + v) + cdist(qF, q - v)
        + 4 * cdist(qE, qI);

  if (anti >= main_) return E;

  dFH = cdist(qF, qH);

  return mix(E, (dEF <= dEH) ? p[u] : p[v], (dFH <= EQ_T) ? SMOOTH_CLEAN : SMOOTH_SOFT);
}

/* One 2x pass. p/q are padded planes (origin pointers) with stride ps;
   out is written directly, os pixels per output row. */
static void smooth2x_pass(const px_t *P, const ycc_t *Q, int ps, int w, int h,
                          px_t *out, int os)
{
  int x, y;

  for (y = 0; y < h; y++)
  {
    const px_t  *p  = P + (ptrdiff_t)y * ps;
    const ycc_t *q  = Q + (ptrdiff_t)y * ps;
    px_t        *o0 = out + (ptrdiff_t)(2 * y) * os;
    px_t        *o1 = o0 + os;

    for (x = 0; x < w; x++)
    {
      px_t E = p[x];

      /* All four neighbors identical to E: every corner test would bail out
         immediately, so skip them. Most of a game frame is like this. */
      if (E == p[x - 1] && E == p[x + 1] && E == p[x - ps] && E == p[x + ps])
      {
        o0[2 * x] = E; o0[2 * x + 1] = E;
        o1[2 * x] = E; o1[2 * x + 1] = E;
        continue;
      }

      o0[2 * x]     = smooth_corner(p + x, q + x, -1, -ps);   /* top-left     */
      o0[2 * x + 1] = smooth_corner(p + x, q + x,  1, -ps);   /* top-right    */
      o1[2 * x]     = smooth_corner(p + x, q + x, -1,  ps);   /* bottom-left  */
      o1[2 * x + 1] = smooth_corner(p + x, q + x,  1,  ps);   /* bottom-right */
    }
  }
}

static void f_smooth2x(const px_t *src, int sp, int w, int h, px_t *dst, int dp)
{
  int    stride;
  px_t  *P = plane_from_src(&sc_pad1, src, sp, w, h, &stride);
  ycc_t *Q = P ? ycc_from_plane(&sc_ycc1, P, stride, w, h) : NULL;

  if (!P || !Q) return;
  smooth2x_pass(P, Q, stride, w, h, dst, dp / (int)sizeof(px_t));
}

/* Two passes, each 2x. The first pass's output goes straight into the
   padded plane the second pass reads from. */
static void f_smooth4x(const px_t *src, int sp, int w, int h, px_t *dst, int dp)
{
  int    s1, s2;
  px_t  *P1 = plane_from_src(&sc_pad1, src, sp, w, h, &s1);
  ycc_t *Q1 = P1 ? ycc_from_plane(&sc_ycc1, P1, s1, w, h) : NULL;
  px_t  *P2;
  ycc_t *Q2;

  if (!P1 || !Q1) return;

  P2 = plane_alloc(&sc_pad2, 2 * w, 2 * h, &s2);
  if (!P2) return;

  smooth2x_pass(P1, Q1, s1, w, h, P2, s2);
  plane_extend(P2, s2, 2 * w, 2 * h);

  Q2 = ycc_from_plane(&sc_ycc2, P2, s2, 2 * w, 2 * h);
  if (!Q2) return;

  smooth2x_pass(P2, Q2, s2, 2 * w, 2 * h, dst, dp / (int)sizeof(px_t));
}

/****************************************************************************
 * Scanlines 2x
 *
 * Doubles every pixel sideways; the second of each pair of rows is dimmed.
 ****************************************************************************/

/* c * 5/8, done on all three fields at once. */
static inline px_t dim58(px_t c)
{
  return (px_t)(((c >> 1) & 0x7BEF) + ((c >> 3) & 0x18E3));
}

static void f_scan2x(const px_t *src, int sp, int w, int h, px_t *dst, int dp)
{
  int x, y;

  for (y = 0; y < h; y++)
  {
    const px_t *r  = (const px_t *)((const uint8_t *)src + (size_t)y * (size_t)sp);
    px_t       *o0 = ROW(dst, dp, 2 * y);
    px_t       *o1 = ROW(dst, dp, 2 * y + 1);

    for (x = 0; x < w; x++)
    {
      px_t c = r[x];
      px_t d = dim58(c);

      o0[2 * x] = c; o0[2 * x + 1] = c;
      o1[2 * x] = d; o1[2 * x + 1] = d;
    }
  }
}

/****************************************************************************
 * CRT 3x -- RGB aperture mask with scanlines
 *
 * Every pixel becomes a 3x3 block. The three columns are the red, green and
 * blue phosphor stripes: each stripe carries its own channel at full
 * strength and the other two at a fraction, the way a real mask leaks a
 * little. The three rows follow a beam profile: a bright middle with dimmer
 * top and bottom. A gain above 1 makes up for the light the mask throws
 * away; the three constants below give roughly 80% of the source's
 * brightness with the mask still clearly visible. (An earlier, harsher set --
 * leak 0.30, gain 1.45 -- measured 58% and turned blue skies navy, because
 * bright stripes clip at full scale while dim ones do not.)
 ****************************************************************************/

#define CRT_LEAK  0.45    /* other-channel level inside a stripe   */
#define CRT_GAIN  1.60    /* overall brightness make-up            */

static const double crt_row_w[3] = { 0.75, 1.00, 0.75 };

/* [row][stripe][channel][8-bit input] -> value already in its 565 field. */
typedef struct
{
  uint16_t t[3][3][3][256];
  int      ready;
} crt_lut_t;

static crt_lut_t crt_main, crt_grille, crt_shadow;

static void crt_build(crt_lut_t *l, const double *row_w, double leak, double gain)
{
  int r, c, ch, v;

  for (r = 0; r < 3; r++)
  for (c = 0; c < 3; c++)
  for (ch = 0; ch < 3; ch++)
  for (v = 0; v < 256; v++)
  {
    double f = v * row_w[r] * ((ch == c) ? 1.0 : leak) * gain;
    int    o = (int)(f + 0.5);

    if (o > 255) o = 255;

    l->t[r][c][ch][v] = (uint16_t)((ch == 0) ? ((o >> 3) << 11)
                                  : (ch == 1) ? ((o >> 2) << 5)
                                  :              (o >> 3));
  }

  l->ready = 1;
}

/* stagger 0: the same three stripes on every row (a mask of vertical stripes); 1: every second source row is moved by one
   stripe (a mask of staggered triads). */
static void crt_run(const px_t *src, int sp, int w, int h, px_t *dst, int dp, const crt_lut_t *l, int stagger)
{
  int x, y, r, c;

  for (y = 0; y < h; y++)
  {
    const px_t *row = (const px_t *)((const uint8_t *)src + (size_t)y * (size_t)sp);
    px_t       *o[3];
    int         shift = stagger ? (y & 1) : 0;

    for (r = 0; r < 3; r++) o[r] = ROW(dst, dp, 3 * y + r);

    for (x = 0; x < w; x++)
    {
      px_t px = row[x];
      int  r5 = (px >> 11) & 31;
      int  g6 = (px >> 5) & 63;
      int  b5 = px & 31;
      int  r8 = (r5 << 3) | (r5 >> 2);
      int  g8 = (g6 << 2) | (g6 >> 4);
      int  b8 = (b5 << 3) | (b5 >> 2);

      for (r = 0; r < 3; r++)
      for (c = 0; c < 3; c++)
      {
        int sc = (c + shift) % 3;
        o[r][3 * x + c] = (px_t)(l->t[r][sc][0][r8] |
                                 l->t[r][sc][1][g8] |
                                 l->t[r][sc][2][b8]);
      }
    }
  }
}

static void f_crt3x(const px_t *src, int sp, int w, int h, px_t *dst, int dp)
{
  if (!crt_main.ready) crt_build(&crt_main, crt_row_w, CRT_LEAK, CRT_GAIN);
  crt_run(src, sp, w, h, dst, dp, &crt_main, 0);
}

/* CRT Aperture Grille 3x: the stripes of a Trinitron-type tube, which has no mask across the picture, so the rows are
   hardly dimmed and the stripes lean less into each other. */
static const double grille_row_w[3] = { 0.88, 1.00, 0.88 };

static void f_crt_grille3x(const px_t *src, int sp, int w, int h, px_t *dst, int dp)
{
  if (!crt_grille.ready) crt_build(&crt_grille, grille_row_w, 0.30, 1.50);
  crt_run(src, sp, w, h, dst, dp, &crt_grille, 0);
}

/* CRT Shadow Mask 3x: dots instead of stripes. The triads of a shadow mask sit staggered from one row to the next, and
   the rows between them are darker. */
static const double shadow_row_w[3] = { 0.62, 1.00, 0.62 };

static void f_crt_shadow3x(const px_t *src, int sp, int w, int h, px_t *dst, int dp)
{
  if (!crt_shadow.ready) crt_build(&crt_shadow, shadow_row_w, 0.28, 1.75);
  crt_run(src, sp, w, h, dst, dp, &crt_shadow, 1);
}

/****************************************************************************
 * Sharp 4x -- plain integer pixel replication
 *
 * Not a smoother at all: it hands the display stage a big, perfectly crisp
 * image so that the final (non-integer) resize to the window has 4x more
 * samples to work with. With Smooth Scaling on, that is the well-known
 * "sharp bilinear" look: crisp pixel edges without shimmering when the
 * window is not an exact multiple of the game's resolution.
 ****************************************************************************/

static void f_sharp4x(const px_t *src, int sp, int w, int h, px_t *dst, int dp)
{
  int x, y;

  for (y = 0; y < h; y++)
  {
    const px_t *r  = (const px_t *)((const uint8_t *)src + (size_t)y * (size_t)sp);
    px_t       *o0 = ROW(dst, dp, 4 * y);

    for (x = 0; x < w; x++)
    {
      px_t c = r[x];
      o0[4 * x] = c; o0[4 * x + 1] = c; o0[4 * x + 2] = c; o0[4 * x + 3] = c;
    }

    memcpy(ROW(dst, dp, 4 * y + 1), o0, (size_t)w * 4 * sizeof(px_t));
    memcpy(ROW(dst, dp, 4 * y + 2), o0, (size_t)w * 4 * sizeof(px_t));
    memcpy(ROW(dst, dp, 4 * y + 3), o0, (size_t)w * 4 * sizeof(px_t));
  }
}

/****************************************************************************
 * Scanlines 3x / 4x, Glow Scanlines 3x
 *
 * Every pixel becomes a block N wide and N tall. The rows follow a beam
 * profile: bright in the middle, dimmer at the top and bottom, so the picture
 * is made of lines the way a CRT draws it. "Glow" lets the beam widen with
 * the brightness of the pixel (a bright pixel hardly dims between the lines)
 * and bleeds a little of the neighboring pixels sideways.
 ****************************************************************************/

/* c * f / 32 (f = 0..32), on all three fields at once. */
static inline px_t scale32(px_t c, unsigned f)
{
  uint32_t x = ((uint32_t)c | ((uint32_t)c << 16)) & 0x07E0F81Fu;

  x = (x * f) >> 5;
  x &= 0x07E0F81Fu;
  return (px_t)(x | (x >> 16));
}

static const uint8_t beam3[3] = { 22, 32, 22 };
static const uint8_t beam4[4] = { 20, 32, 32, 20 };

static void scanlines_n(const px_t *src, int sp, int w, int h, px_t *dst, int dp, int n, const uint8_t *beam)
{
  int x, y, k;

  for (y = 0; y < h; y++)
  {
    const px_t *r  = (const px_t *)((const uint8_t *)src + (size_t)y * (size_t)sp);
    for (k = 0; k < n; k++)
    {
      px_t *o = ROW(dst, dp, n * y + k);
      for (x = 0; x < w; x++)
      {
        px_t c = beam[k] == 32 ? r[x] : scale32(r[x], beam[k]);
        int  i;
        for (i = 0; i < n; i++) o[n * x + i] = c;
      }
    }
  }
}

static void f_scan3x(const px_t *src, int sp, int w, int h, px_t *dst, int dp) { scanlines_n(src, sp, w, h, dst, dp, 3, beam3); }
static void f_scan4x(const px_t *src, int sp, int w, int h, px_t *dst, int dp) { scanlines_n(src, sp, w, h, dst, dp, 4, beam4); }

static void f_glow3x(const px_t *src, int sp, int w, int h, px_t *dst, int dp)
{
  int x, y;

  for (y = 0; y < h; y++)
  {
    const px_t *r  = (const px_t *)((const uint8_t *)src + (size_t)y * (size_t)sp);
    px_t       *o0 = ROW(dst, dp, 3 * y), *o1 = ROW(dst, dp, 3 * y + 1), *o2 = ROW(dst, dp, 3 * y + 2);

    for (x = 0; x < w; x++)
    {
      px_t c = r[x];
      px_t l = x > 0 ? r[x - 1] : c, rt = x < w - 1 ? r[x + 1] : c;
      px_t p0 = mix(c, l, 7), p2 = mix(c, rt, 7);
      unsigned lum = (((c >> 11) & 31) * 154 + ((c >> 5) & 63) * 150 + (c & 31) * 58) >> 8;     /* 0..~63 */
      unsigned dim = 12 + lum * 15 / 63;                                                        /* 12..27 of 32: a bright pixel keeps its beam */

      o1[3 * x] = p0; o1[3 * x + 1] = c; o1[3 * x + 2] = p2;
      o0[3 * x] = scale32(p0, dim); o0[3 * x + 1] = scale32(c, dim); o0[3 * x + 2] = scale32(p2, dim);
      o2[3 * x] = o0[3 * x]; o2[3 * x + 1] = o0[3 * x + 1]; o2[3 * x + 2] = o0[3 * x + 2];
    }
  }
}

/****************************************************************************
 * LCD Grid 3x / 4x
 *
 * The pixel grid of a handheld screen: each pixel is a bright square with a
 * dark line between it and its neighbors (one output pixel wide, 2x2 body at
 * 3x and 3x3 at 4x). The ghosting of such a screen is Video > Visual Effects >
 * LCD Ghosting, which works with any filter.
 ****************************************************************************/

static void lcd_n(const px_t *src, int sp, int w, int h, px_t *dst, int dp, int n)
{
  int x, y, k, i;

  for (y = 0; y < h; y++)
  {
    const px_t *r = (const px_t *)((const uint8_t *)src + (size_t)y * (size_t)sp);

    for (k = 0; k < n; k++)
    {
      px_t *o = ROW(dst, dp, n * y + k);
      for (x = 0; x < w; x++)
      {
        px_t c = r[x];
        for (i = 0; i < n; i++)
        {
          int gap_x = (i == n - 1), gap_y = (k == n - 1);
          o[n * x + i] = (gap_x && gap_y) ? scale32(c, 8) : (gap_x || gap_y) ? scale32(c, 13) : c;
        }
      }
    }
  }
}

static void f_lcd3x(const px_t *src, int sp, int w, int h, px_t *dst, int dp) { lcd_n(src, sp, w, h, dst, dp, 3); }
static void f_lcd4x(const px_t *src, int sp, int w, int h, px_t *dst, int dp) { lcd_n(src, sp, w, h, dst, dp, 4); }

/****************************************************************************
 * Sharp Bilinear 3x / 4x
 *
 * Like Sharp 4x, but the seam between two source pixels is a short blend
 * instead of a hard step, so edges look clean and not stair-stepped when the
 * result is shrunk to the window. Only the output pixels along a seam are
 * touched; the inside of a pixel keeps its exact color.
 ****************************************************************************/

/* For each output position inside a source pixel: which neighbor it leans
   toward (-1 before, 0 none, +1 after) and by how much (of 32). */
typedef struct { int8_t side; uint8_t a; } sb_t;

static const sb_t sb3[3] = { { -1, 10 }, { 0, 0 }, { 1, 10 } };
static const sb_t sb4[4] = { { -1, 12 }, { 0, 0 }, { 0, 0 }, { 1, 12 } };

static void sharpbil_n(const px_t *src, int sp, int w, int h, px_t *dst, int dp, int n, const sb_t *sb)
{
  int stride, x, y, k, i;
  const px_t *P = plane_from_src(&sc_pad1, src, sp, w, h, &stride);

  if (!P) return;

  for (y = 0; y < h; y++)
  for (k = 0; k < n; k++)
  {
    const px_t *r0 = P + (ptrdiff_t)y * stride;
    const px_t *r1 = P + (ptrdiff_t)(y + sb[k].side) * stride;
    px_t       *o  = ROW(dst, dp, n * y + k);

    for (x = 0; x < w; x++)
    for (i = 0; i < n; i++)
    {
      int sx = sb[i].side;
      px_t top = sb[i].a ? mix(r0[x], r0[x + sx], sb[i].a) : r0[x];
      px_t bot = sb[i].a ? mix(r1[x], r1[x + sx], sb[i].a) : r1[x];
      o[n * x + i] = sb[k].a ? mix(top, bot, sb[k].a) : top;
    }
  }
}

static void f_sharpbil3x(const px_t *src, int sp, int w, int h, px_t *dst, int dp) { sharpbil_n(src, sp, w, h, dst, dp, 3, sb3); }
static void f_sharpbil4x(const px_t *src, int sp, int w, int h, px_t *dst, int dp) { sharpbil_n(src, sp, w, h, dst, dp, 4, sb4); }

/****************************************************************************
 * Lanczos, Catmull-Rom, B-Spline and Mitchell, 3x / 4x
 *
 * Real resampling filters (four taps each way) instead of copying pixels: a
 * smooth, non-pixelated picture with clean edges. Lanczos (two lobes) and
 * Catmull-Rom are sharp and can overshoot a little next to a hard edge; the
 * B-Spline is the softest and never overshoots; Mitchell is in between. The
 * result is clamped. Done in two passes, across and then down.
 ****************************************************************************/

typedef struct { int off; int w[4]; } lz_t;       /* first tap is source pixel (x + off - 1); weights sum to 4096 */

enum { LZ_LANCZOS, LZ_CATMULL, LZ_BSPLINE, LZ_MITCHELL, LZ_KINDS };

static lz_t lz_tab[LZ_KINDS][2][4];                 /* [kind][3x or 4x][output pixel] */
static int  lz_ready[LZ_KINDS];

/* The Mitchell-Netravali family of cubics (B and C choose the one: Catmull-Rom 0 and 1/2, B-Spline 1 and 0, Mitchell 1/3 and 1/3). */
static double lz_bc(double x, double B, double C)
{
  if (x < 0) x = -x;
  if (x < 1.0) return ((12 - 9 * B - 6 * C) * x * x * x + (-18 + 12 * B + 6 * C) * x * x + (6 - 2 * B)) / 6.0;
  if (x < 2.0) return ((-B - 6 * C) * x * x * x + (6 * B + 30 * C) * x * x + (-12 * B - 48 * C) * x + (8 * B + 24 * C)) / 6.0;
  return 0.0;
}

static double lz_kernel(int kind, double x)
{
  const double pi = 3.14159265358979323846;

  switch (kind)
  {
    case LZ_CATMULL:  return lz_bc(x, 0.0, 0.5);
    case LZ_BSPLINE:  return lz_bc(x, 1.0, 0.0);
    case LZ_MITCHELL: return lz_bc(x, 1.0 / 3.0, 1.0 / 3.0);
    default: break;
  }
  if (x < 0) x = -x;
  if (x < 1e-9) return 1.0;
  if (x >= 2.0) return 0.0;
  return (2.0 * sin(pi * x) * sin(pi * x / 2.0)) / (pi * pi * x * x);
}

static void lz_build_one(lz_t *t, int n, int kind)
{
  int k, j;

  for (k = 0; k < n; k++)
  {
    double d = (k + 0.5) / n - 0.5;                   /* the output pixel's place relative to the source pixel's center */
    double fr = d >= 0 ? d : d + 1.0, wt[4], sum = 0;
    int    acc = 0, big = 0;

    t[k].off = d >= 0 ? 0 : -1;
    wt[0] = lz_kernel(kind, fr + 1.0); wt[1] = lz_kernel(kind, fr); wt[2] = lz_kernel(kind, 1.0 - fr); wt[3] = lz_kernel(kind, 2.0 - fr);
    for (j = 0; j < 4; j++) sum += wt[j];
    for (j = 0; j < 4; j++)
    {
      t[k].w[j] = (int)(wt[j] / sum * 4096.0 + (wt[j] >= 0 ? 0.5 : -0.5));
      acc += t[k].w[j];
      if (t[k].w[j] > t[k].w[big]) big = j;
    }
    t[k].w[big] += 4096 - acc;                        /* exactly 4096, so flat color stays flat */
  }
}

static void lanczos_n(const px_t *src, int sp, int w, int h, px_t *dst, int dp, int n, const lz_t *tab)
{
  int stride, x, y, k, j;
  const px_t *P = plane_from_src(&sc_pad1, src, sp, w, h, &stride);
  int16_t *H;                                          /* rows -2 .. h+1, across done, 3 channels */
  size_t   rowlen = (size_t)w * n * 3;
  int16_t  chan[(FILTER_MAX_W + 2 * PAD) * 3];         /* one source row as 8-bit r, g, b */

  if (!P) return;
  H = (int16_t *)scratch_get(&sc_ycc2, (size_t)(h + 4) * rowlen * sizeof(int16_t));
  if (!H) return;

  for (y = -2; y <= h + 1; y++)
  {
    const px_t *r = P + (ptrdiff_t)y * stride;
    int16_t    *o = H + (size_t)(y + 2) * rowlen;
    int16_t    *cp = chan + PAD * 3;                    /* pixel 0 */

    for (x = -PAD; x < w + PAD; x++)
    {
      px_t c = r[x];
      int  r5 = (c >> 11) & 31, g6 = (c >> 5) & 63, b5 = c & 31;
      cp[x * 3]     = (int16_t)((r5 << 3) | (r5 >> 2));
      cp[x * 3 + 1] = (int16_t)((g6 << 2) | (g6 >> 4));
      cp[x * 3 + 2] = (int16_t)((b5 << 3) | (b5 >> 2));
    }

    for (x = 0; x < w; x++)
    for (k = 0; k < n; k++)
    {
      const int16_t *t = cp + (x + tab[k].off - 1) * 3;
      int ch;

      for (ch = 0; ch < 3; ch++)
      {
        int v = (tab[k].w[0] * t[ch] + tab[k].w[1] * t[3 + ch] + tab[k].w[2] * t[6 + ch] + tab[k].w[3] * t[9 + ch]) >> 12;
        o[((size_t)x * n + k) * 3 + ch] = (int16_t)(v < -64 ? -64 : v > 320 ? 320 : v);
      }
    }
  }

  for (y = 0; y < h; y++)
  for (k = 0; k < n; k++)
  {
    px_t *o = ROW(dst, dp, n * y + k);
    const int16_t *t0 = H + (size_t)(y + tab[k].off + 1) * rowlen, *t1 = t0 + rowlen, *t2 = t1 + rowlen, *t3 = t2 + rowlen;
    int w0 = tab[k].w[0], w1 = tab[k].w[1], w2 = tab[k].w[2], w3 = tab[k].w[3];
    size_t i, m = (size_t)w * n;

    for (i = 0; i < m; i++)
    {
      int rr = (w0 * t0[i * 3]     + w1 * t1[i * 3]     + w2 * t2[i * 3]     + w3 * t3[i * 3])     >> 12;
      int gg = (w0 * t0[i * 3 + 1] + w1 * t1[i * 3 + 1] + w2 * t2[i * 3 + 1] + w3 * t3[i * 3 + 1]) >> 12;
      int bb = (w0 * t0[i * 3 + 2] + w1 * t1[i * 3 + 2] + w2 * t2[i * 3 + 2] + w3 * t3[i * 3 + 2]) >> 12;
      rr = rr < 0 ? 0 : rr > 255 ? 255 : rr;
      gg = gg < 0 ? 0 : gg > 255 ? 255 : gg;
      bb = bb < 0 ? 0 : bb > 255 ? 255 : bb;
      o[i] = (px_t)(((rr >> 3) << 11) | ((gg >> 2) << 5) | (bb >> 3));
    }
  }
  (void)j;
}

static const lz_t *lz_get(int kind, int n)
{
  if (!lz_ready[kind]) { lz_build_one(lz_tab[kind][0], 3, kind); lz_build_one(lz_tab[kind][1], 4, kind); lz_ready[kind] = 1; }
  return lz_tab[kind][n - 3];
}

#define LZ_FILTER(NAME, KIND, N) \
  static void NAME(const px_t *src, int sp, int w, int h, px_t *dst, int dp) { lanczos_n(src, sp, w, h, dst, dp, N, lz_get(KIND, N)); }

LZ_FILTER(f_lanczos3x, LZ_LANCZOS, 3)
LZ_FILTER(f_lanczos4x, LZ_LANCZOS, 4)
LZ_FILTER(f_catmull3x, LZ_CATMULL, 3)
LZ_FILTER(f_catmull4x, LZ_CATMULL, 4)
LZ_FILTER(f_bspline3x, LZ_BSPLINE, 3)
LZ_FILTER(f_bspline4x, LZ_BSPLINE, 4)
LZ_FILTER(f_mitchell3x, LZ_MITCHELL, 3)
LZ_FILTER(f_mitchell4x, LZ_MITCHELL, 4)

/****************************************************************************
 * xBR 2x / 3x / 4x and HQ2x / HQ3x / HQ4x
 *
 * Hyllian's xBR and Maxim Stepin's hqx, from the libxbr-standalone library (xbr/, LGPL 2.1 or later, see xbr/README-libxbr.txt).
 * Both work on 32-bit pixels, so each frame is made into 0x00RRGGBB pixels, scaled, and made into RGB565 again. The library's
 * table of how different two colors look has an entry for each of the 65536 RGB565 colors (it is built when first needed).
 ****************************************************************************/

static int xbr_table_ready(void)
{
  if (!xbr_table)
  {
    xbr_table = (xbr_data *)malloc(sizeof(xbr_data));
    if (!xbr_table) { alloc_failed = 1; return 0; }
    xbr_init_data(xbr_table);
  }
  return 1;
}

static void libxbr_run(void (*fn)(const xbr_params *), int n, const px_t *src, int sp, int w, int h, px_t *dst, int dp)
{
  uint32_t *in, *out;
  xbr_params prm;
  int x, y;

  if (!xbr_table_ready()) return;
  in  = (uint32_t *)scratch_get(&sc_xin,  (size_t)w * (size_t)h * 4);
  out = (uint32_t *)scratch_get(&sc_xout, (size_t)w * (size_t)h * (size_t)(n * n) * 4);
  if (!in || !out) return;

  for (y = 0; y < h; y++)
  {
    const px_t *r = (const px_t *)((const uint8_t *)src + (size_t)y * (size_t)sp);
    uint32_t   *o = in + (size_t)y * (size_t)w;

    for (x = 0; x < w; x++)
    {
      unsigned c = r[x], r5 = (c >> 11) & 31, g6 = (c >> 5) & 63, b5 = c & 31;
      o[x] = (((r5 << 3) | (r5 >> 2)) << 16) | (((g6 << 2) | (g6 >> 4)) << 8) | ((b5 << 3) | (b5 >> 2));
    }
  }

  prm.input = (const uint8_t *)in;
  prm.output = (uint8_t *)out;
  prm.inWidth = w; prm.inHeight = h;
  prm.inPitch = w * 4; prm.outPitch = w * n * 4;
  prm.data = xbr_table;
  fn(&prm);

  for (y = 0; y < h * n; y++)
  {
    const uint32_t *r = out + (size_t)y * (size_t)(w * n);
    px_t           *o = ROW(dst, dp, y);

    for (x = 0; x < w * n; x++)
      o[x] = (px_t)(((r[x] >> 8) & 0xF800) | ((r[x] >> 5) & 0x07E0) | ((r[x] >> 3) & 0x001F));
  }
}

#define LIBXBR_FILTER(NAME, N, FN) \
  static void NAME(const px_t *src, int sp, int w, int h, px_t *dst, int dp) { libxbr_run(FN, N, src, sp, w, h, dst, dp); }

LIBXBR_FILTER(f_xbr2x, 2, xbr_filter_xbr2x)
LIBXBR_FILTER(f_xbr3x, 3, xbr_filter_xbr3x)
LIBXBR_FILTER(f_xbr4x, 4, xbr_filter_xbr4x)
LIBXBR_FILTER(f_hq2x, 2, xbr_filter_hq2x)
LIBXBR_FILTER(f_hq3x, 3, xbr_filter_hq3x)
LIBXBR_FILTER(f_hq4x, 4, xbr_filter_hq4x)

/****************************************************************************
 * ScaleFX 3x
 *
 * Sp00kyFox's ScaleFX (MIT license), ported from its five RetroArch shader passes (libretro/glsl-shaders, scalefx/) to plain C:
 * edges of pixel art are found and followed up to level 6, with smooth changes between slopes, and the picture only ever
 * holds colors that were in the original. The passes keep their shader names and layout:
 *
 *   pass 0  how different a pixel is from its neighbors (a color distance, as floats)
 *   pass 1  corner strength from those distances
 *   pass 2  which corners, horizontal and vertical edges and orientations are real (packed in 4 bits for each of four values)
 *   pass 3  which neighbor each of the eight sub-pixels around the middle of a pixel is to be taken from
 *   pass 4  the 3x3 output, each sub-pixel copied from the pixel pass 3 chose
 *
 * The shader's 8-bit intermediate pictures are not needed here: its packed values are small whole numbers, so they are kept as
 * such. Outside the frame, the nearest edge pixel is used (as the other filters do).
 ****************************************************************************/

typedef struct { float v[4]; } f4_t;                 /* x, y, z, w */
typedef struct { unsigned char c[4], h[4], v[4], o[4]; } sfx_flags_t;   /* the four bits of each of the four values of a pass 2 pixel */
typedef struct { unsigned char crn[4], mid[4]; } sfx_sub_t;             /* pass 3's choice: 0 = E, 1 = D, 2 = D0, 3 = F, 4 = F0, 5 = B, 6 = B0, 7 = H, 8 = H0 */

static float sfx_dist(const float *A, const float *B)
{
  float r = 0.5f * (A[0] + B[0]);
  float d0 = A[0] - B[0], d1 = A[1] - B[1], d2 = A[2] - B[2];
  float c0 = (2.0f + r) * d0, c1 = 4.0f * d1, c2 = (3.0f - r) * d2;

  return sqrtf(c0 * d0 + c1 * d1 + c2 * d2) / 3.0f;
}

static float sfx_str(float d, float ax, float ay, float bx, float by)
{
  const float clr = 0.5f;
  float diff = ax - ay;
  float mn_a = ax < bx ? ax : bx, mn_b = ay < by ? ay : by;
  float wght1 = (clr - d > 0.0f ? clr - d : 0.0f) / clr;
  float wght2 = (1.0f - d) + (mn_a + ax > mn_b + ay ? diff : -diff);

  wght2 = wght2 < 0.0f ? 0.0f : wght2 > 1.0f ? 1.0f : wght2;
  return (wght1 * wght2) * (ax * ay);                /* (SFX_SAA is 1: the strength is always kept) */
}

/* The shader's helpers on one value: GE and LE are strict, LEQ and GEQ are not (as its #defines have them). */
static inline float sfx_ge(float x, float y) { return x > y ? 1.0f : 0.0f; }
static inline float sfx_le(float x, float y) { return x < y ? 1.0f : 0.0f; }
static inline float sfx_leq(float x, float y) { return x <= y ? 1.0f : 0.0f; }
static inline float sfx_min1(float x) { return x < 1.0f ? x : 1.0f; }

#define SFXAT(buf, W, H, X, Y) ((buf) + ((size_t)((Y) < 0 ? 0 : (Y) >= (H) ? (H) - 1 : (Y)) * (size_t)(W) + (size_t)((X) < 0 ? 0 : (X) >= (W) ? (W) - 1 : (X))))

/* Dominance at junctions: dom(x, y, z, w) of four vec3s given by their components. */
static void sfx_dom(float *out, const float x[3], const float y[3], const float z[3], const float w[3])
{
  out[0] = 2.0f * x[1] - (x[0] + x[2]);
  out[1] = 2.0f * y[1] - (y[0] + y[2]);
  out[2] = 2.0f * z[1] - (z[0] + z[2]);
  out[3] = 2.0f * w[1] - (w[0] + w[2]);
}

/* The majority vote for the ambiguous junctions of one corner: min(GE(jD, 0) * (LEQ(jD.yzwx, 0) * LEQ(jD.wxyz, 0) + GE(jD + jD.zwxy, jD.yzwx + jD.wxyz)), 1) */
static void sfx_vote(float *j, const float *d)
{
  int i;

  for (i = 0; i < 4; i++)
  {
    float yzwx = d[(i + 1) & 3], wxyz = d[(i + 3) & 3], zwxy = d[(i + 2) & 3];

    j[i] = sfx_min1(sfx_ge(d[i], 0.0f) * (sfx_leq(yzwx, 0.0f) * sfx_leq(wxyz, 0.0f) + sfx_ge(d[i] + zwxy, yzwx + wxyz)));
  }
}

static float sfx_clear(float c0, float c1, float a0, float a1, float b0, float b1)
{
  float m1 = a0 < a1 ? a0 : a1, m2 = b0 < b1 ? b0 : b1;
  float m3 = a0 < b1 ? a0 : b1, m4 = b0 < a1 ? b0 : a1;

  return (c0 >= (m1 > m2 ? m1 : m2)) && (c1 >= (m3 > m4 ? m3 : m4)) ? 1.0f : 0.0f;
}

/* (the expressions of the shader's last pass are kept as written there, with its && and || precedence) */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wparentheses"
#endif

static void f_scalefx3x(const px_t *src, int sp, int w, int h, px_t *dst, int dp)
{
  size_t n = (size_t)w * (size_t)h;
  f4_t *m = (f4_t *)scratch_get(&sc_sfx0, n * sizeof(f4_t));                    /* pass 0 */
  f4_t *s = (f4_t *)scratch_get(&sc_sfx1, n * sizeof(f4_t));                    /* pass 1 */
  sfx_flags_t *fl = (sfx_flags_t *)scratch_get(&sc_sfx2, n * sizeof(sfx_flags_t));   /* pass 2 */
  sfx_sub_t *sub = (sfx_sub_t *)scratch_get(&sc_sfx3, n * sizeof(sfx_sub_t));   /* pass 3 */
  float (*rgb)[3];
  int x, y, i;

  if (!m || !s || !fl || !sub) return;
  rgb = (float (*)[3])malloc(n * sizeof(float[3]));
  if (!rgb) { alloc_failed = 1; return; }

  for (y = 0; y < h; y++)
  {
    const px_t *r = (const px_t *)((const uint8_t *)src + (size_t)y * (size_t)sp);

    for (x = 0; x < w; x++)
    {
      unsigned c = r[x], r5 = (c >> 11) & 31, g6 = (c >> 5) & 63, b5 = c & 31;
      float *o = rgb[(size_t)y * (size_t)w + (size_t)x];

      o[0] = (float)((r5 << 3) | (r5 >> 2)) / 255.0f;
      o[1] = (float)((g6 << 2) | (g6 >> 4)) / 255.0f;
      o[2] = (float)((b5 << 3) | (b5 >> 2)) / 255.0f;
    }
  }

  /* pass 0: the distances of a pixel E to A (up left), B (up), C (up right) and F (right) */
  for (y = 0; y < h; y++)
  for (x = 0; x < w; x++)
  {
    const float *E = rgb[(size_t)y * (size_t)w + (size_t)x];
    f4_t *o = m + (size_t)y * (size_t)w + (size_t)x;

    o->v[0] = sfx_dist(E, SFXAT((float (*)[3])rgb, w, h, x - 1, y - 1)[0]);
    o->v[1] = sfx_dist(E, SFXAT((float (*)[3])rgb, w, h, x, y - 1)[0]);
    o->v[2] = sfx_dist(E, SFXAT((float (*)[3])rgb, w, h, x + 1, y - 1)[0]);
    o->v[3] = sfx_dist(E, SFXAT((float (*)[3])rgb, w, h, x + 1, y)[0]);
  }

  /* pass 1: the strength of the four corners of a pixel */
  for (y = 0; y < h; y++)
  for (x = 0; x < w; x++)
  {
    const f4_t *A = SFXAT(m, w, h, x - 1, y - 1), *B = SFXAT(m, w, h, x, y - 1);
    const f4_t *D = SFXAT(m, w, h, x - 1, y), *E = SFXAT(m, w, h, x, y), *F = SFXAT(m, w, h, x + 1, y);
    const f4_t *G = SFXAT(m, w, h, x - 1, y + 1), *H = SFXAT(m, w, h, x, y + 1), *I = SFXAT(m, w, h, x + 1, y + 1);
    f4_t *o = s + (size_t)y * (size_t)w + (size_t)x;

    o->v[0] = sfx_str(D->v[2], D->v[3], E->v[1], A->v[3], D->v[1]);
    o->v[1] = sfx_str(F->v[0], E->v[3], E->v[1], B->v[3], F->v[1]);
    o->v[2] = sfx_str(H->v[2], E->v[3], H->v[1], H->v[3], I->v[1]);
    o->v[3] = sfx_str(H->v[0], D->v[3], H->v[1], G->v[3], G->v[1]);
  }

  /* pass 2: which corners and edges are real */
  for (y = 0; y < h; y++)
  for (x = 0; x < w; x++)
  {
    const f4_t *A = SFXAT(m, w, h, x - 1, y - 1), *B = SFXAT(m, w, h, x, y - 1);
    const f4_t *D = SFXAT(m, w, h, x - 1, y), *E = SFXAT(m, w, h, x, y), *H = SFXAT(m, w, h, x, y + 1);
    const f4_t *F = SFXAT(m, w, h, x + 1, y), *G = SFXAT(m, w, h, x - 1, y + 1), *I = SFXAT(m, w, h, x + 1, y + 1);
    const f4_t *As = SFXAT(s, w, h, x - 1, y - 1), *Bs = SFXAT(s, w, h, x, y - 1), *Cs = SFXAT(s, w, h, x + 1, y - 1);
    const f4_t *Ds = SFXAT(s, w, h, x - 1, y), *Es = SFXAT(s, w, h, x, y), *Fs = SFXAT(s, w, h, x + 1, y);
    const f4_t *Gs = SFXAT(s, w, h, x - 1, y + 1), *Hs = SFXAT(s, w, h, x, y + 1), *Is = SFXAT(s, w, h, x + 1, y + 1);
    float jSx[4], jSy[4], jSz[4], jSw[4], jDx[4], jDy[4], jDz[4], jDw[4], jx[4], jy[4], jz[4], jw[4], res[4], clr[4], hh[4], vv[4];
    sfx_flags_t *ofl = fl + (size_t)y * (size_t)w + (size_t)x;


    jSx[0] = As->v[2]; jSx[1] = Bs->v[3]; jSx[2] = Es->v[0]; jSx[3] = Ds->v[1];
    jSy[0] = Bs->v[2]; jSy[1] = Cs->v[3]; jSy[2] = Fs->v[0]; jSy[3] = Es->v[1];
    jSz[0] = Es->v[2]; jSz[1] = Fs->v[3]; jSz[2] = Is->v[0]; jSz[3] = Hs->v[1];
    jSw[0] = Ds->v[2]; jSw[1] = Es->v[3]; jSw[2] = Hs->v[0]; jSw[3] = Gs->v[1];

#define V3(P, a, b, c) do { (P)[0] = (a); (P)[1] = (b); (P)[2] = (c); } while (0)
    {
      float p0[3], p1[3], p2[3], p3[3];
      /* dom(As.yzw, Bs.zwx, Es.wxy, Ds.xyz) */
      V3(p0, As->v[1], As->v[2], As->v[3]); V3(p1, Bs->v[2], Bs->v[3], Bs->v[0]); V3(p2, Es->v[3], Es->v[0], Es->v[1]); V3(p3, Ds->v[0], Ds->v[1], Ds->v[2]);
      sfx_dom(jDx, p0, p1, p2, p3);
      /* dom(Bs.yzw, Cs.zwx, Fs.wxy, Es.xyz) */
      V3(p0, Bs->v[1], Bs->v[2], Bs->v[3]); V3(p1, Cs->v[2], Cs->v[3], Cs->v[0]); V3(p2, Fs->v[3], Fs->v[0], Fs->v[1]); V3(p3, Es->v[0], Es->v[1], Es->v[2]);
      sfx_dom(jDy, p0, p1, p2, p3);
      /* dom(Es.yzw, Fs.zwx, Is.wxy, Hs.xyz) */
      V3(p0, Es->v[1], Es->v[2], Es->v[3]); V3(p1, Fs->v[2], Fs->v[3], Fs->v[0]); V3(p2, Is->v[3], Is->v[0], Is->v[1]); V3(p3, Hs->v[0], Hs->v[1], Hs->v[2]);
      sfx_dom(jDz, p0, p1, p2, p3);
      /* dom(Ds.yzw, Es.zwx, Hs.wxy, Gs.xyz) */
      V3(p0, Ds->v[1], Ds->v[2], Ds->v[3]); V3(p1, Es->v[2], Es->v[3], Es->v[0]); V3(p2, Hs->v[3], Hs->v[0], Hs->v[1]); V3(p3, Gs->v[0], Gs->v[1], Gs->v[2]);
      sfx_dom(jDw, p0, p1, p2, p3);
    }

    sfx_vote(jx, jDx); sfx_vote(jy, jDy); sfx_vote(jz, jDz); sfx_vote(jw, jDw);

    res[0] = sfx_min1(jx[2] + (1 - jx[1]) * (1 - jx[3]) * sfx_ge(jSx[2], 0.0f) * (jx[0] + sfx_ge(jSx[0] + jSx[2], jSx[1] + jSx[3])));
    res[1] = sfx_min1(jy[3] + (1 - jy[2]) * (1 - jy[0]) * sfx_ge(jSy[3], 0.0f) * (jy[1] + sfx_ge(jSy[1] + jSy[3], jSy[0] + jSy[2])));
    res[2] = sfx_min1(jz[0] + (1 - jz[3]) * (1 - jz[1]) * sfx_ge(jSz[0], 0.0f) * (jz[2] + sfx_ge(jSz[0] + jSz[2], jSz[1] + jSz[3])));
    res[3] = sfx_min1(jw[1] + (1 - jw[0]) * (1 - jw[2]) * sfx_ge(jSw[1], 0.0f) * (jw[3] + sfx_ge(jSw[1] + jSw[3], jSw[0] + jSw[2])));

    /* single pixel and end of line detection: res = min(res * (vec4(jx.z, jy.w, jz.x, jw.y) + NOT(res.wxyz * res.yzwx)), 1) */
    {
      float jj[4], r0[4];

      jj[0] = jx[2]; jj[1] = jy[3]; jj[2] = jz[0]; jj[3] = jw[1];
      for (i = 0; i < 4; i++) r0[i] = res[i];
      for (i = 0; i < 4; i++) res[i] = sfx_min1(r0[i] * (jj[i] + (1.0f - r0[(i + 3) & 3] * r0[(i + 1) & 3])));
    }

    clr[0] = sfx_clear(D->v[2], E->v[0], D->v[3], E->v[1], A->v[3], D->v[1]);
    clr[1] = sfx_clear(F->v[0], E->v[2], E->v[3], E->v[1], B->v[3], F->v[1]);
    clr[2] = sfx_clear(H->v[2], I->v[0], E->v[3], H->v[1], H->v[3], I->v[1]);
    clr[3] = sfx_clear(H->v[0], G->v[2], D->v[3], H->v[1], G->v[3], G->v[1]);

    hh[0] = A->v[3] < D->v[3] ? A->v[3] : D->v[3]; hh[1] = B->v[3] < E->v[3] ? B->v[3] : E->v[3];
    hh[2] = H->v[3] < E->v[3] ? H->v[3] : E->v[3]; hh[3] = G->v[3] < D->v[3] ? G->v[3] : D->v[3];
    vv[0] = D->v[1] < E->v[1] ? D->v[1] : E->v[1]; vv[1] = F->v[1] < E->v[1] ? F->v[1] : E->v[1];
    vv[2] = I->v[1] < H->v[1] ? I->v[1] : H->v[1]; vv[3] = G->v[1] < H->v[1] ? G->v[1] : H->v[1];

    {
      float hs[4], vs[4];

      hs[0] = hh[0] + D->v[3]; hs[1] = hh[1] + E->v[3]; hs[2] = hh[2] + E->v[3]; hs[3] = hh[3] + D->v[3];
      vs[0] = vv[0] + E->v[1]; vs[1] = vv[1] + E->v[1]; vs[2] = vv[2] + H->v[1]; vs[3] = vv[3] + H->v[1];
      for (i = 0; i < 4; i++)
      {
        float hori = sfx_le(hh[i], vv[i]) * clr[i], vert = sfx_ge(hh[i], vv[i]) * clr[i];

        ofl->c[i] = (unsigned char)(res[i] > 0.5f);
        ofl->h[i] = (unsigned char)(hori > 0.5f);
        ofl->v[i] = (unsigned char)(vert > 0.5f);
        ofl->o[i] = (unsigned char)(sfx_ge(hs[i], vs[i]) > 0.5f);
      }
    }
  }
#undef V3

  /* pass 3: where each sub-pixel comes from */
  for (y = 0; y < h; y++)
  for (x = 0; x < w; x++)
  {
    const sfx_flags_t *E = SFXAT(fl, w, h, x, y);
    const sfx_flags_t *D = SFXAT(fl, w, h, x - 1, y), *D0 = SFXAT(fl, w, h, x - 2, y), *D1 = SFXAT(fl, w, h, x - 3, y);
    const sfx_flags_t *F = SFXAT(fl, w, h, x + 1, y), *F0 = SFXAT(fl, w, h, x + 2, y), *F1 = SFXAT(fl, w, h, x + 3, y);
    const sfx_flags_t *B = SFXAT(fl, w, h, x, y - 1), *B0 = SFXAT(fl, w, h, x, y - 2), *B1 = SFXAT(fl, w, h, x, y - 3);
    const sfx_flags_t *H = SFXAT(fl, w, h, x, y + 1), *H0 = SFXAT(fl, w, h, x, y + 2), *H1 = SFXAT(fl, w, h, x, y + 3);
    const unsigned char *Ec = E->c, *Eh = E->h, *Ev = E->v, *Eo = E->o;
    const unsigned char *Dc = D->c, *Dh = D->h, *Do = D->o, *D0c = D0->c, *D0h = D0->h, *D1h = D1->h;
    const unsigned char *Fc = F->c, *Fh = F->h, *Fo = F->o, *F0c = F0->c, *F0h = F0->h, *F1h = F1->h;
    const unsigned char *Bc = B->c, *Bv = B->v, *Bo = B->o, *B0c = B0->c, *B0v = B0->v, *B1v = B1->v;
    const unsigned char *Hc = H->c, *Hv = H->v, *Ho = H->o, *H0c = H0->c, *H0v = H0->v, *H1v = H1->v;
    const int scn = 1;                                                   /* SFX_SCN */
    int lvl1[4], lvl2[4][2], lvl3[4][2], lvl4[4][2], lvl5[4][2], lvl6[4][2];
    sfx_sub_t *o = sub + (size_t)y * (size_t)w + (size_t)x;

    /* the corners and middles are 0 = x, 1 = y, 2 = z, 3 = w, as in the shader */
    lvl1[0] = Ec[0] && (Dc[2] || Bc[2] || scn);
    lvl1[1] = Ec[1] && (Fc[3] || Bc[3] || scn);
    lvl1[2] = Ec[2] && (Fc[0] || Hc[0] || scn);
    lvl1[3] = Ec[3] && (Dc[1] || Hc[1] || scn);

    lvl2[0][0] = (Ec[0] && Eh[1]) && Dc[2];  lvl2[0][1] = (Ec[1] && Eh[0]) && Fc[3];
    lvl2[1][0] = (Ec[1] && Ev[2]) && Bc[3];  lvl2[1][1] = (Ec[2] && Ev[1]) && Hc[0];
    lvl2[2][0] = (Ec[3] && Eh[2]) && Dc[1];  lvl2[2][1] = (Ec[2] && Eh[3]) && Fc[0];
    lvl2[3][0] = (Ec[0] && Ev[3]) && Bc[2];  lvl2[3][1] = (Ec[3] && Ev[0]) && Hc[1];

    lvl3[0][0] = lvl2[0][1] && (Dh[1] && Dh[0]) && Fh[2];  lvl3[0][1] = lvl2[3][1] && (Bv[3] && Bv[0]) && Hv[2];
    lvl3[1][0] = lvl2[0][0] && (Fh[0] && Fh[1]) && Dh[3];  lvl3[1][1] = lvl2[1][1] && (Bv[2] && Bv[1]) && Hv[3];
    lvl3[2][0] = lvl2[2][0] && (Fh[3] && Fh[2]) && Dh[0];  lvl3[2][1] = lvl2[1][0] && (Hv[1] && Hv[2]) && Bv[0];
    lvl3[3][0] = lvl2[2][1] && (Dh[2] && Dh[3]) && Fh[1];  lvl3[3][1] = lvl2[3][0] && (Hv[0] && Hv[3]) && Bv[1];

    lvl4[0][0] = (Dc[0] && Dh[1] && Eh[0] && Eh[1] && Fh[0] && Fh[1]) && (D0c[2] && D0h[3]);
    lvl4[0][1] = (Bc[0] && Bv[3] && Ev[0] && Ev[3] && Hv[0] && Hv[3]) && (B0c[2] && B0v[1]);
    lvl4[1][0] = (Fc[1] && Fh[0] && Eh[1] && Eh[0] && Dh[1] && Dh[0]) && (F0c[3] && F0h[2]);
    lvl4[1][1] = (Bc[1] && Bv[2] && Ev[1] && Ev[2] && Hv[1] && Hv[2]) && (B0c[3] && B0v[0]);
    lvl4[2][0] = (Fc[2] && Fh[3] && Eh[2] && Eh[3] && Dh[2] && Dh[3]) && (F0c[0] && F0h[1]);
    lvl4[2][1] = (Hc[2] && Hv[1] && Ev[2] && Ev[1] && Bv[2] && Bv[1]) && (H0c[0] && H0v[3]);
    lvl4[3][0] = (Dc[3] && Dh[2] && Eh[3] && Eh[2] && Fh[3] && Fh[2]) && (D0c[1] && D0h[0]);
    lvl4[3][1] = (Hc[3] && Hv[0] && Ev[3] && Ev[0] && Bv[3] && Bv[0]) && (H0c[1] && H0v[2]);

    lvl5[0][0] = lvl4[0][0] && (F0h[0] && F0h[1]) && (D1h[2] && D1h[3]);  lvl5[0][1] = lvl4[1][0] && (D0h[1] && D0h[0]) && (F1h[3] && F1h[2]);
    lvl5[1][0] = lvl4[1][1] && (H0v[1] && H0v[2]) && (B1v[3] && B1v[0]);  lvl5[1][1] = lvl4[2][1] && (B0v[2] && B0v[1]) && (H1v[0] && H1v[3]);
    lvl5[2][0] = lvl4[3][0] && (F0h[3] && F0h[2]) && (D1h[1] && D1h[0]);  lvl5[2][1] = lvl4[2][0] && (D0h[2] && D0h[3]) && (F1h[0] && F1h[1]);
    lvl5[3][0] = lvl4[0][1] && (H0v[0] && H0v[3]) && (B1v[2] && B1v[1]);  lvl5[3][1] = lvl4[3][1] && (B0v[3] && B0v[0]) && (H1v[1] && H1v[2]);

    lvl6[0][0] = lvl5[0][1] && (D1h[1] && D1h[0]);  lvl6[0][1] = lvl5[3][1] && (B1v[3] && B1v[0]);
    lvl6[1][0] = lvl5[0][0] && (F1h[0] && F1h[1]);  lvl6[1][1] = lvl5[1][1] && (B1v[2] && B1v[1]);
    lvl6[2][0] = lvl5[2][0] && (F1h[3] && F1h[2]);  lvl6[2][1] = lvl5[1][0] && (H1v[1] && H1v[2]);
    lvl6[3][0] = lvl5[2][1] && (D1h[2] && D1h[3]);  lvl6[3][1] = lvl5[3][0] && (H1v[0] && H1v[3]);

    /* sub-pixels: 0 = E, 1 = D, 2 = D0, 3 = F, 4 = F0, 5 = B, 6 = B0, 7 = H, 8 = H0 */
    o->crn[0] = (lvl1[0] && Eo[0] || lvl3[0][0] && Eo[1] || lvl4[0][0] && Do[0] || lvl6[0][0] && Fo[1]) ? 5 : (lvl1[0] || lvl3[0][1] && !Eo[3] || lvl4[0][1] && !Bo[0] || lvl6[0][1] && !Ho[3]) ? 1 : lvl3[0][0] ? 3 : lvl3[0][1] ? 7 : lvl4[0][0] ? 2 : lvl4[0][1] ? 6 : lvl6[0][0] ? 4 : lvl6[0][1] ? 8 : 0;
    o->crn[1] = (lvl1[1] && Eo[1] || lvl3[1][0] && Eo[0] || lvl4[1][0] && Fo[1] || lvl6[1][0] && Do[0]) ? 5 : (lvl1[1] || lvl3[1][1] && !Eo[2] || lvl4[1][1] && !Bo[1] || lvl6[1][1] && !Ho[2]) ? 3 : lvl3[1][0] ? 1 : lvl3[1][1] ? 7 : lvl4[1][0] ? 4 : lvl4[1][1] ? 6 : lvl6[1][0] ? 2 : lvl6[1][1] ? 8 : 0;
    o->crn[2] = (lvl1[2] && Eo[2] || lvl3[2][0] && Eo[3] || lvl4[2][0] && Fo[2] || lvl6[2][0] && Do[3]) ? 7 : (lvl1[2] || lvl3[2][1] && !Eo[1] || lvl4[2][1] && !Ho[2] || lvl6[2][1] && !Bo[1]) ? 3 : lvl3[2][0] ? 1 : lvl3[2][1] ? 5 : lvl4[2][0] ? 4 : lvl4[2][1] ? 8 : lvl6[2][0] ? 2 : lvl6[2][1] ? 6 : 0;
    o->crn[3] = (lvl1[3] && Eo[3] || lvl3[3][0] && Eo[2] || lvl4[3][0] && Do[3] || lvl6[3][0] && Fo[2]) ? 7 : (lvl1[3] || lvl3[3][1] && !Eo[0] || lvl4[3][1] && !Ho[3] || lvl6[3][1] && !Bo[0]) ? 1 : lvl3[3][0] ? 3 : lvl3[3][1] ? 5 : lvl4[3][0] ? 2 : lvl4[3][1] ? 8 : lvl6[3][0] ? 4 : lvl6[3][1] ? 6 : 0;

    o->mid[0] = (lvl2[0][0] &&  Eo[0] || lvl2[0][1] &&  Eo[1] || lvl5[0][0] &&  Do[0] || lvl5[0][1] &&  Fo[1]) ? 5 : lvl2[0][0] ? 1 : lvl2[0][1] ? 3 : lvl5[0][0] ? 2 : lvl5[0][1] ? 4 : (Ec[0] && Dc[2] && Ec[1] && Fc[3]) ? (Eo[0] ? (Eo[1] ? 5 : 3) : 1) : 0;
    o->mid[1] = (lvl2[1][0] && !Eo[1] || lvl2[1][1] && !Eo[2] || lvl5[1][0] && !Bo[1] || lvl5[1][1] && !Ho[2]) ? 3 : lvl2[1][0] ? 5 : lvl2[1][1] ? 7 : lvl5[1][0] ? 6 : lvl5[1][1] ? 8 : (Ec[1] && Bc[3] && Ec[2] && Hc[0]) ? (!Eo[1] ? (!Eo[2] ? 3 : 7) : 5) : 0;
    o->mid[2] = (lvl2[2][0] &&  Eo[3] || lvl2[2][1] &&  Eo[2] || lvl5[2][0] &&  Do[3] || lvl5[2][1] &&  Fo[2]) ? 7 : lvl2[2][0] ? 1 : lvl2[2][1] ? 3 : lvl5[2][0] ? 2 : lvl5[2][1] ? 4 : (Ec[2] && Fc[0] && Ec[3] && Dc[1]) ? (Eo[2] ? (Eo[3] ? 7 : 1) : 3) : 0;
    o->mid[3] = (lvl2[3][0] && !Eo[0] || lvl2[3][1] && !Eo[3] || lvl5[3][0] && !Bo[0] || lvl5[3][1] && !Ho[3]) ? 1 : lvl2[3][0] ? 5 : lvl2[3][1] ? 7 : lvl5[3][0] ? 6 : lvl5[3][1] ? 8 : (Ec[3] && Hc[1] && Ec[0] && Bc[2]) ? (!Eo[3] ? (!Eo[0] ? 1 : 5) : 7) : 0;
  }

  /* pass 4: the 3x3 block of each pixel */
  for (y = 0; y < h; y++)
  for (x = 0; x < w; x++)
  {
    const sfx_sub_t *o = sub + (size_t)y * (size_t)w + (size_t)x;
    int fy, fx;

    for (fy = 0; fy < 3; fy++)
    for (fx = 0; fx < 3; fx++)
    {
      static const signed char offx[9] = { 0, -1, -2, 1, 2, 0, 0, 0, 0 }, offy[9] = { 0, 0, 0, 0, 0, -1, -2, 1, 2 };
      int spx = fy == 0 ? (fx == 0 ? o->crn[0] : fx == 1 ? o->mid[0] : o->crn[1])
                        : fy == 1 ? (fx == 0 ? o->mid[3] : fx == 1 ? 0 : o->mid[1])
                                  : (fx == 0 ? o->crn[3] : fx == 1 ? o->mid[2] : o->crn[2]);
      int sx = x + offx[spx], sy = y + offy[spx];

      sx = sx < 0 ? 0 : sx >= w ? w - 1 : sx;
      sy = sy < 0 ? 0 : sy >= h ? h - 1 : sy;
      ROW(dst, dp, 3 * y + fy)[3 * x + fx] = *(const px_t *)((const uint8_t *)src + (size_t)sy * (size_t)sp + (size_t)sx * sizeof(px_t));
    }
  }

  free(rgb);
}

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/****************************************************************************
 * CRT Geom 3x / 4x
 *
 * cgwg's and DOLLS' CRT-Geom (GPL 2 or later), ported from its RetroArch shader (libretro/glsl-shaders, crt/crt-geom.glsl) with
 * the shader's own default settings: a curved glass screen (distance 1.6, radius 2.0, rounded corners), Lanczos sampling along
 * the line, a scanline beam that is wider for bright colors (sampled three times for each output row), a dot mask of 0.3, and
 * the gamma of the CRT (2.4) and of the monitor (2.2).
 *
 * The shader does all this for every pixel of the picture with a few dozen exp, pow and sin calls; the same work here would be
 * far too slow, so what only depends on the size is made once (where each output pixel falls on the source picture, and the
 * corner mask), and the functions of one or two numbers are looked up in tables: the Lanczos weights by the place along the
 * line, the beam by the place between two lines and the brightness, and the monitor's gamma. The picture is the same up to the
 * rounding of those tables.
 ****************************************************************************/

#define CG_UBINS   1024                             /* Lanczos weights, by the fraction along the line */
#define CG_YBINS   256                              /* the beam, by the place between two lines ... */
#define CG_CBINS   256                              /* ... and by the brightness */
#define CG_GAMMA_N 4096                             /* the monitor's gamma on 0 .. 2 */

typedef struct { float x, y, c; } cg_geom_t;        /* where the output pixel falls on the source picture (0 .. 1) and how much of it shows */

static struct
{
  int      w, h, n;                                 /* the size the geometry is made for (0: none yet) */
  cg_geom_t *geom;
  float    lanczos[CG_UBINS][4];
  float    *beam1, *beam2;                          /* [CG_YBINS][CG_CBINS]: this line's and the next line's weights */
  float    gamma_out[CG_GAMMA_N + 1];
  float    lin[256];                                /* the CRT's gamma of an 8-bit value */
  unsigned frame;                                   /* counts frames, for the two fields of an interlaced picture */
} cg;

static void cg_release(void)
{
  free(cg.geom); cg.geom = NULL;
  free(cg.beam1); cg.beam1 = NULL;
  free(cg.beam2); cg.beam2 = NULL;
  cg.w = cg.h = cg.n = 0;
}

/* The screen's curvature, as in the shader's vertex part: a pixel's place on the glass to its place on the picture and back. */
static const double CG_D = 1.6, CG_R = 2.0, CG_PI = 3.14159265358979323846;
static double cg_sinx, cg_siny, cg_cosx, cg_cosy;

static double cg_intersect(double x, double y)
{
  double A = x * x + y * y + CG_D * CG_D;
  double B = 2.0 * (CG_R * (x * cg_sinx + y * cg_siny - CG_D * cg_cosx * cg_cosy) - CG_D * CG_D);
  double C = CG_D * CG_D + 2.0 * CG_R * CG_D * cg_cosx * cg_cosy;

  return (-B - sqrt(B * B - 4.0 * A * C)) / (2.0 * A);
}

static void cg_bkwtrans(double x, double y, double *ox, double *oy)
{
  double c = cg_intersect(x, y);
  double px = c * x + CG_R * cg_sinx, py = c * y + CG_R * cg_siny, tx, ty, pocx, pocy, A, B, C, a, ux, uy, r;

  px /= CG_R; py /= CG_R;
  tx = cg_sinx / cg_cosx; ty = cg_siny / cg_cosy;
  pocx = px / cg_cosx; pocy = py / cg_cosy;
  A = tx * tx + ty * ty + 1.0;
  B = -2.0 * (pocx * tx + pocy * ty);
  C = pocx * pocx + pocy * pocy - 1.0;
  a = (-B + sqrt(B * B - 4.0 * A * C)) / (2.0 * A);
  ux = (px - a * cg_sinx) / cg_cosx; uy = (py - a * cg_siny) / cg_cosy;
  r = CG_R * acos(a);
  r = fabs(r) > 1e-5 ? fabs(r) : 1e-5;
  *ox = ux * r / sin(r / CG_R);
  *oy = uy * r / sin(r / CG_R);
}

static void cg_fwtrans(double ux, double uy, double *ox, double *oy)
{
  double r = sqrt(ux * ux + uy * uy), s, xx, D;

  r = fabs(r) > 1e-5 ? fabs(r) : 1e-5;
  s = sin(r / CG_R) / r;
  ux *= s; uy *= s;
  xx = 1.0 - cos(r / CG_R);
  D = CG_D / CG_R + xx * cg_cosx * cg_cosy + ux * cg_sinx + uy * cg_siny;
  *ox = CG_D * (ux * cg_cosx - xx * cg_sinx) / D;
  *oy = CG_D * (uy * cg_cosy - xx * cg_siny) / D;
}

/* The beam of one scanline at a distance d from its center, for a color c (0 .. 1) in one channel (the shader's scanlineWeights, lum 0, weight 0.3). */
static double cg_beam(double d, double c)
{
  double wid = 2.0 + 2.0 * pow(c, 4.0), q = fabs(d) / 0.3 / sqrt(0.5 * wid);

  return 1.4 * exp(-pow(q, wid)) / (0.6 + 0.2 * wid);
}

static int cg_prepare(int w, int h, int n)
{
  int ox, oy, i, k, ow = w * n, oh = h * n;
  double sx, sy, sz, ax = 1.0, ay = 0.75, cxx, cyy, lox, loy, hix, hiy, tmp;
  double f = 1.0 / (double)n;                       /* the shader's InputSize.y / OutputSize.y: how far apart the three beam samples lie */

  if (cg.geom && cg.w == w && cg.h == h && cg.n == n) return 1;
  cg_release();

  /* the tables that do not depend on the size */
  for (i = 0; i < 256; i++) cg.lin[i] = (float)pow(i / 255.0, 2.4);
  for (i = 0; i <= CG_GAMMA_N; i++) cg.gamma_out[i] = (float)pow(i * 2.0 / CG_GAMMA_N, 1.0 / 2.2);
  for (i = 0; i < CG_UBINS; i++)
  {
    double u = (i + 0.5) / CG_UBINS, co[4], sum = 0;

    co[0] = 1.0 + u; co[1] = u; co[2] = 1.0 - u; co[3] = 2.0 - u;
    for (k = 0; k < 4; k++)
    {
      double p = CG_PI * co[k];

      p = fabs(p) > 1e-5 ? fabs(p) : 1e-5;
      co[k] = 2.0 * sin(p) * sin(p / 2.0) / (p * p);          /* Lanczos 2 */
      sum += co[k];
    }
    for (k = 0; k < 4; k++) cg.lanczos[i][k] = (float)(co[k] / sum);
  }
  cg.beam1 = (float *)malloc(sizeof(float) * CG_YBINS * CG_CBINS);
  cg.beam2 = (float *)malloc(sizeof(float) * CG_YBINS * CG_CBINS);
  cg.geom  = (cg_geom_t *)malloc(sizeof(cg_geom_t) * (size_t)ow * (size_t)oh);
  if (!cg.beam1 || !cg.beam2 || !cg.geom) { cg_release(); alloc_failed = 1; return 0; }

  for (i = 0; i < CG_YBINS; i++)
  for (k = 0; k < CG_CBINS; k++)
  {
    double u = (i + 0.5) / CG_YBINS, c = k / 255.0;

    /* three samples along the beam's width, as the shader's OVERSAMPLE does */
    cg.beam1[i * CG_CBINS + k] = (float)((cg_beam(u, c) + cg_beam(u + f / 3.0, c) + cg_beam(u - f / 3.0, c)) / 3.0);
    cg.beam2[i * CG_CBINS + k] = (float)((cg_beam(1.0 - u, c) + cg_beam(1.0 - (u + f / 3.0), c) + cg_beam(1.0 - (u - f / 3.0), c)) / 3.0);
  }

  /* the screen: no tilt (the shader adds 0.001 to the sines and cosines) */
  cg_sinx = sin(0.0) + 0.001; cg_siny = sin(0.0) + 0.001;
  cg_cosx = cos(0.0) + 0.001; cg_cosy = cos(0.0) + 0.001;
  cg_bkwtrans(-CG_R * cg_sinx / (1.0 + CG_R / CG_D * cg_cosx * cg_cosy), -CG_R * cg_siny / (1.0 + CG_R / CG_D * cg_cosx * cg_cosy), &cxx, &cyy);
  cg_fwtrans(-0.5 * ax, cyy, &lox, &tmp);
  cg_fwtrans(cxx, -0.5 * ay, &tmp, &loy);
  cg_fwtrans(+0.5 * ax, cyy, &hix, &tmp);
  cg_fwtrans(cxx, +0.5 * ay, &tmp, &hiy);
  lox /= ax; loy /= ay; hix /= ax; hiy /= ay;
  sx = (hix + lox) * ax * 0.5;
  sy = (hiy + loy) * ay * 0.5;
  sz = (hix - lox) > (hiy - loy) ? (hix - lox) : (hiy - loy);

  for (oy = 0; oy < oh; oy++)
  for (ox = 0; ox < ow; ox++)
  {
    cg_geom_t *g = cg.geom + (size_t)oy * (size_t)ow + (size_t)ox;
    double tx = (ox + 0.5) / ow * 1.0001, ty = (oy + 0.5) / oh * 1.0001;      /* the shader's TexCoord */
    double px = (tx - 0.5) * ax * sz + sx, py = (ty - 0.5) * ay * sz + sy, ux, uy, cx, cy, ex, ey, dist;

    cg_bkwtrans(px, py, &ux, &uy);
    ux = ux / ax + 0.5; uy = uy / ay + 0.5;
    g->x = (float)ux; g->y = (float)uy;

    /* the rounded corners of the glass */
    cx = ux < 1.0 - ux ? ux : 1.0 - ux; cy = uy < 1.0 - uy ? uy : 1.0 - uy;
    cx *= ax; cy *= ay;
    ex = 0.03 - (cx < 0.03 ? cx : 0.03); ey = 0.03 - (cy < 0.03 ? cy : 0.03);
    dist = sqrt(ex * ex + ey * ey);
    dist = (0.03 - dist) * 1000.0;
    dist = dist < 0.0 ? 0.0 : dist > 1.0 ? 1.0 : dist;
    g->c = (float)(dist * 1.0001);
    if (!(ux == ux) || !(uy == uy)) g->c = 0.0f;                              /* (nothing to show where the glass does not reach) */
  }

  cg.w = w; cg.h = h; cg.n = n;
  return 1;
}

/* One output row of CRT Geom (the rows are independent of each other). */
static void crt_geom_row(const float *lin, int w, int h, int ilfac_y, int ilvec, const cg_geom_t *g, px_t *out, int ow)
{
  static const float rmask[2][3] = { { 1.0f, 0.7f, 1.0f }, { 0.7f, 1.0f, 0.7f } };   /* the dot mask: even and odd output pixels (strength 0.3) */
  const float inv_il = 1.0f / (float)ilfac_y;
  int ox;

  for (ox = 0; ox < ow; ox++, g++)
  {
    float rx, ry, fx, fy, a[3], b[3];
    int ix, iy, row0, row1, ubin, ybin, ch;
    const float *cf, *r0, *r1;
    const float *mk = rmask[ox & 1];
    unsigned rgb[3];

    if (g->c <= 0.0f) { out[ox] = 0; continue; }

    rx = g->x * (float)w - 0.5f;
    ix = (int)rx - (rx < 0.0f); fx = rx - (float)ix;                    /* floor */
    ry = (g->y * (float)h - 0.5f + (float)ilvec) * inv_il;
    iy = (int)ry - (ry < 0.0f); fy = ry - (float)iy;
    row0 = iy * ilfac_y - ilvec; row1 = row0 + ilfac_y;
    row0 = row0 < 0 ? 0 : row0 >= h ? h - 1 : row0;
    row1 = row1 < 0 ? 0 : row1 >= h ? h - 1 : row1;
    ubin = (int)(fx * CG_UBINS); ubin = ubin < 0 ? 0 : ubin >= CG_UBINS ? CG_UBINS - 1 : ubin;
    ybin = (int)(fy * CG_YBINS); ybin = ybin < 0 ? 0 : ybin >= CG_YBINS ? CG_YBINS - 1 : ybin;
    cf = cg.lanczos[ubin];
    r0 = lin + (size_t)row0 * (size_t)w * 3;
    r1 = lin + (size_t)row1 * (size_t)w * 3;

    if (ix >= 1 && ix + 2 < w)                                          /* the four taps are side by side: 12 floats in a row */
    {
      const float *p = r0 + (size_t)(ix - 1) * 3, *q = r1 + (size_t)(ix - 1) * 3;

      for (ch = 0; ch < 3; ch++)
      {
        a[ch] = cf[0] * p[ch] + cf[1] * p[3 + ch] + cf[2] * p[6 + ch] + cf[3] * p[9 + ch];
        b[ch] = cf[0] * q[ch] + cf[1] * q[3 + ch] + cf[2] * q[6 + ch] + cf[3] * q[9 + ch];
      }
    }
    else                                                                /* at the left and right edge the nearest pixel is used */
    {
      int k, tx[4];

      for (k = 0; k < 4; k++) { tx[k] = ix - 1 + k; tx[k] = tx[k] < 0 ? 0 : tx[k] >= w ? w - 1 : tx[k]; }
      for (ch = 0; ch < 3; ch++)
      {
        a[ch] = cf[0] * r0[tx[0] * 3 + ch] + cf[1] * r0[tx[1] * 3 + ch] + cf[2] * r0[tx[2] * 3 + ch] + cf[3] * r0[tx[3] * 3 + ch];
        b[ch] = cf[0] * r1[tx[0] * 3 + ch] + cf[1] * r1[tx[1] * 3 + ch] + cf[2] * r1[tx[2] * 3 + ch] + cf[3] * r1[tx[3] * 3 + ch];
      }
    }

    for (ch = 0; ch < 3; ch++)
    {
      float v;
      int ka, kb, gi;

      a[ch] = a[ch] < 0.0f ? 0.0f : a[ch] > 1.0f ? 1.0f : a[ch];
      b[ch] = b[ch] < 0.0f ? 0.0f : b[ch] > 1.0f ? 1.0f : b[ch];
      ka = (int)(a[ch] * 255.0f + 0.5f); kb = (int)(b[ch] * 255.0f + 0.5f);
      v = (a[ch] * cg.beam1[ybin * CG_CBINS + ka] + b[ch] * cg.beam2[ybin * CG_CBINS + kb]) * g->c * mk[ch];
      gi = (int)(v * (CG_GAMMA_N / 2.0f) + 0.5f);
      gi = gi > CG_GAMMA_N ? CG_GAMMA_N : gi;
      v = cg.gamma_out[gi];
      v = v > 1.0f ? 1.0f : v;
      rgb[ch] = (unsigned)(v * 255.0f + 0.5f);
    }
    out[ox] = (px_t)(((rgb[0] >> 3) << 11) | ((rgb[1] >> 2) << 5) | (rgb[2] >> 3));
  }
}

typedef struct { const float *lin; int w, h, ilfac_y, ilvec, ow, oh; px_t *dst; int dp; } cg_job_t;

/* Output rows from `from` up to (not including) `to`. */
static void cg_rows(void *ctx, int from, int to)
{
  const cg_job_t *j = (const cg_job_t *)ctx;
  int oy;

  for (oy = from; oy < to; oy++)
    crt_geom_row(j->lin, j->w, j->h, j->ilfac_y, j->ilvec, cg.geom + (size_t)oy * (size_t)j->ow, ROW(j->dst, j->dp, oy), j->ow);
}

static void crt_geom_n(const px_t *src, int sp, int w, int h, px_t *dst, int dp, int n)
{
  int ow = w * n, oh = h * n, x, y, ilfac_y = h / 200 < 1 ? 1 : h / 200 > 2 ? 2 : h / 200, ilvec = 0;
  float *lin;                                      /* the picture in the CRT's gamma, 3 floats a pixel */

  if (!cg_prepare(w, h, n)) return;
  lin = (float *)scratch_get(&sc_ycc1, (size_t)w * (size_t)h * 3 * sizeof(float));
  if (!lin) return;

  if (ilfac_y == 2) ilvec = (int)(cg.frame & 1);   /* an interlaced picture shows its two fields in turn */
  cg.frame++;

  for (y = 0; y < h; y++)
  {
    const px_t *r = (const px_t *)((const uint8_t *)src + (size_t)y * (size_t)sp);

    for (x = 0; x < w; x++)
    {
      unsigned c = r[x], r5 = (c >> 11) & 31, g6 = (c >> 5) & 63, b5 = c & 31;
      float *o = lin + ((size_t)y * (size_t)w + (size_t)x) * 3;

      o[0] = cg.lin[(r5 << 3) | (r5 >> 2)];
      o[1] = cg.lin[(g6 << 2) | (g6 >> 4)];
      o[2] = cg.lin[(b5 << 3) | (b5 >> 2)];
    }
  }

  {
    cg_job_t job;

    job.lin = lin; job.w = w; job.h = h; job.ilfac_y = ilfac_y; job.ilvec = ilvec; job.ow = ow; job.oh = oh;
    job.dst = dst; job.dp = dp;
    run_rows(cg_rows, &job, oh);
  }
}

static void f_crtgeom3x(const px_t *src, int sp, int w, int h, px_t *dst, int dp) { crt_geom_n(src, sp, w, h, dst, dp, 3); }
static void f_crtgeom4x(const px_t *src, int sp, int w, int h, px_t *dst, int dp) { crt_geom_n(src, sp, w, h, dst, dp, 4); }

/****************************************************************************
 * LQ2x
 *
 * The small, fast cousin of hq2x, with the rules of RetroArch's lq2x filter: a pixel's corner is mixed with the neighbor
 * above or below it when that neighbor equals the one beside the pixel on the same side, and the pixels above and below
 * (and left and right) differ.
 ****************************************************************************/

static void f_lq2x(const px_t *src, int sp, int w, int h, px_t *dst, int dp)
{
  int stride, x, y;
  const px_t *P = plane_from_src(&sc_pad1, src, sp, w, h, &stride);

  if (!P) return;
  for (y = 0; y < h; y++)
  {
    px_t *o0 = ROW(dst, dp, 2 * y), *o1 = ROW(dst, dp, 2 * y + 1);

    for (x = 0; x < w; x++)
    {
      const px_t *c = P + (ptrdiff_t)y * stride + x;
      px_t C = c[0], A = c[-stride], E = c[stride], B = c[-1], D = c[1];
      px_t tl = C, tr = C, bl = C, br = C;

      if (A != E && B != D)
      {
        if (A == B) tl = mix(C, A, 16);
        if (A == D) tr = mix(C, A, 16);
        if (E == B) bl = mix(C, E, 16);
        if (E == D) br = mix(C, E, 16);
      }
      o0[2 * x] = tl; o0[2 * x + 1] = tr;
      o1[2 * x] = bl; o1[2 * x + 1] = br;
    }
  }
}

/****************************************************************************
 * 2xSaI
 *
 * Derek Liauw Kie Fa's "Scale and Interpolate" idea, written from the
 * published description of its rules. Each source pixel A becomes 2x2: A
 * itself, and three pixels that are A, a neighbor, or an average, chosen by
 * which of the nearby pixels match in a diagonal or a line:
 *
 *      I E F J
 *      G A B K        A is the pixel; the output is  A  p1
 *      H C D L                                       p2 p3
 *      M N O P
 ****************************************************************************/

static inline px_t avg2(px_t a, px_t b) { return mix(a, b, 16); }
static inline px_t avg4(px_t a, px_t b, px_t c, px_t d) { return avg2(avg2(a, b), avg2(c, d)); }

static int sai_r1(px_t a, px_t b, px_t c, px_t d)
{
  int x = 0, y = 0, r = 0;
  if (a == c) x++; else if (b == c) y++;
  if (a == d) x++; else if (b == d) y++;
  if (x <= 1) r++;
  if (y <= 1) r--;
  return r;
}

static int sai_r2(px_t a, px_t b, px_t c, px_t d)
{
  int x = 0, y = 0, r = 0;
  if (a == c) x++; else if (b == c) y++;
  if (a == d) x++; else if (b == d) y++;
  if (x <= 1) r--;
  if (y <= 1) r++;
  return r;
}

static void f_2xsai(const px_t *src, int sp, int w, int h, px_t *dst, int dp)
{
  int stride, x, y;
  const px_t *P = plane_from_src(&sc_pad1, src, sp, w, h, &stride);

  if (!P) return;

  for (y = 0; y < h; y++)
  {
    const px_t *r = P + (ptrdiff_t)y * stride;
    px_t       *o0 = ROW(dst, dp, 2 * y), *o1 = ROW(dst, dp, 2 * y + 1);

    for (x = 0; x < w; x++)
    {
      px_t I = r[x - stride - 1], E = r[x - stride], F = r[x - stride + 1], J = r[x - stride + 2];
      px_t G = r[x - 1],          A = r[x],          B = r[x + 1],          K = r[x + 2];
      px_t H = r[x + stride - 1], C = r[x + stride], D = r[x + stride + 1], L = r[x + stride + 2];
      px_t M = r[x + 2 * stride - 1], N = r[x + 2 * stride], O = r[x + 2 * stride + 1];
      px_t p, p1, p2;

      if (A == D && B != C)
      {
        p  = ((A == E && B == L) || (A == C && A == F && B != E && B == J)) ? A : avg2(A, B);
        p1 = ((A == G && C == O) || (A == B && A == H && G != C && C == M)) ? A : avg2(A, C);
        p2 = A;
      }
      else if (B == C && A != D)
      {
        p  = ((B == F && A == H) || (B == E && B == D && A != F && A == I)) ? B : avg2(A, B);
        p1 = ((C == H && A == F) || (C == G && C == D && A != H && A == I)) ? C : avg2(A, C);
        p2 = B;
      }
      else if (A == D && B == C)
      {
        if (A == B) { p = p1 = p2 = A; }
        else
        {
          int rr = sai_r1(A, B, G, E) + sai_r2(B, A, K, F) + sai_r2(B, A, H, N) + sai_r1(A, B, L, O);
          p = p1 = avg2(A, B);
          p2 = rr > 0 ? A : rr < 0 ? B : avg4(A, B, C, D);
        }
      }
      else
      {
        if (A == C && A == F && B != E && B == J) p = A;
        else if (B == E && B == D && A != F && A == I) p = B;
        else p = avg2(A, B);

        if (A == B && A == H && G != C && C == M) p1 = A;
        else if (C == G && C == D && A != H && A == I) p1 = C;
        else p1 = avg2(A, C);

        p2 = avg4(A, B, C, D);
      }

      o0[2 * x] = A; o0[2 * x + 1] = p;
      o1[2 * x] = p1; o1[2 * x + 1] = p2;
    }
  }
}

#ifdef GPEX_XBRZ
/****************************************************************************
 * xBRZ 2x - 6x
 *
 * The real xBRZ ("scale by rules") by Zenju, from the sources in xbrz/,
 * driven through xbrz_glue.cpp. It is the same algorithm the well-known
 * xBRZ render plugins were built from, at default settings, so the picture
 * is the one you would have got from those. Unlike everything above it works
 * on 32-bit pixels and needs a 64 MB table, which is why it lives behind the
 * glue and is only set up while one of these filters is selected.
 *
 * If xBRZ cannot run (out of memory) the frame falls back to plain integer
 * replication at the same size, so the output is always complete.
 ****************************************************************************/

static void replicate(int n, const px_t *src, int sp, int w, int h, px_t *dst, int dp)
{
  int x, y, k, j;

  for (y = 0; y < h; y++)
  {
    const px_t *r  = (const px_t *)((const uint8_t *)src + (size_t)y * (size_t)sp);
    px_t       *o0 = ROW(dst, dp, n * y);

    for (x = 0; x < w; x++)
      for (k = 0; k < n; k++) o0[n * x + k] = r[x];

    for (j = 1; j < n; j++)
      memcpy(ROW(dst, dp, n * y + j), o0, (size_t)w * (size_t)n * sizeof(px_t));
  }
}

#define XBRZ_FILTER(N) \
  static void f_xbrz##N(const px_t *src, int sp, int w, int h, px_t *dst, int dp) \
  { \
    if (!xbrz_glue_scale(N, src, sp, w, h, dst, dp)) replicate(N, src, sp, w, h, dst, dp); \
  }

XBRZ_FILTER(2)
XBRZ_FILTER(3)
XBRZ_FILTER(4)
XBRZ_FILTER(5)
XBRZ_FILTER(6)
#endif /* GPEX_XBRZ */

/****************************************************************************
 * Super Eagle and Super 2xSaI
 *
 * Kreed's two follow-ups to Eagle and 2xSaI, written from the published
 * rules (the pixel names are the ones those descriptions use):
 *
 *      B0 B1 B2 B3
 *      4  5  6  S2        5 is the pixel; its 2x2 block is  p1a p1b
 *      1  2  3  S1                                         p2a p2b
 *      A0 A1 A2 A3
 *
 * Super Eagle looks at the pixels around 5, 6, 2 and 3 and keeps a diagonal
 * going where two corners agree; Super 2xSaI blends more, along lines that
 * run through the block. Both are a little softer than their older
 * versions. Neither is an exact copy of any other program's output.
 ****************************************************************************/

/* three parts a, one part b */
static inline px_t avg31(px_t a, px_t b) { return mix(a, b, 8); }

static void f_supereagle(const px_t *src, int sp, int w, int h, px_t *dst, int dp)
{
  int stride, x, y;
  const px_t *P = plane_from_src(&sc_pad1, src, sp, w, h, &stride);

  if (!P) return;

  for (y = 0; y < h; y++)
  {
    const px_t *r = P + (ptrdiff_t)y * stride;
    px_t       *o0 = ROW(dst, dp, 2 * y), *o1 = ROW(dst, dp, 2 * y + 1);

    for (x = 0; x < w; x++)
    {
      px_t B1 = r[x - stride],       B2 = r[x - stride + 1];
      px_t c4 = r[x - 1],            c5 = r[x],              c6 = r[x + 1],          S2 = r[x + 2];
      px_t c1 = r[x + stride - 1],   c2 = r[x + stride],     c3 = r[x + stride + 1], S1 = r[x + stride + 2];
      px_t A1 = r[x + 2 * stride],   A2 = r[x + 2 * stride + 1];
      px_t p1a, p1b, p2a, p2b;

      if (c2 == c6 && c5 != c3)
      {
        p1b = p2a = c2;
        if (c1 == c2 || c6 == B2) { p1a = avg2(c2, c5); p1a = avg2(c2, p1a); }
        else p1a = avg2(c5, c6);
        if (c6 == S2 || c2 == A1) { p2b = avg2(c2, c3); p2b = avg2(c2, p2b); }
        else p2b = avg2(c2, c3);
      }
      else if (c5 == c3 && c2 != c6)
      {
        p2b = p1a = c5;
        if (B1 == c5 || c3 == S1) { p1b = avg2(c5, c6); p1b = avg2(c5, p1b); }
        else p1b = avg2(c5, c6);
        if (c3 == A2 || c4 == c5) { p2a = avg2(c5, c2); p2a = avg2(c5, p2a); }
        else p2a = avg2(c2, c3);
      }
      else if (c5 == c3 && c2 == c6)
      {
        int rr = sai_r1(c6, c5, c1, A1) + sai_r1(c6, c5, c4, B1) + sai_r1(c6, c5, A2, S1) + sai_r1(c6, c5, B2, S2);
        if (rr > 0)      { p1b = p2a = c2; p1a = p2b = avg2(c5, c6); }
        else if (rr < 0) { p2b = p1a = c5; p1b = p2a = avg2(c5, c6); }
        else             { p2b = p1a = c5; p1b = p2a = c2; }
      }
      else
      {
        p2b = p1a = avg2(c2, c6);
        p2b = avg31(c3, p2b);
        p1a = avg31(c5, p1a);
        p2a = p1b = avg2(c5, c3);
        p2a = avg31(c2, p2a);
        p1b = avg31(c6, p1b);
      }

      o0[2 * x] = p1a; o0[2 * x + 1] = p1b;
      o1[2 * x] = p2a; o1[2 * x + 1] = p2b;
    }
  }
}

static void f_super2xsai(const px_t *src, int sp, int w, int h, px_t *dst, int dp)
{
  int stride, x, y;
  const px_t *P = plane_from_src(&sc_pad1, src, sp, w, h, &stride);

  if (!P) return;

  for (y = 0; y < h; y++)
  {
    const px_t *r = P + (ptrdiff_t)y * stride;
    px_t       *o0 = ROW(dst, dp, 2 * y), *o1 = ROW(dst, dp, 2 * y + 1);

    for (x = 0; x < w; x++)
    {
      px_t B0 = r[x - stride - 1], B1 = r[x - stride], B2 = r[x - stride + 1], B3 = r[x - stride + 2];
      px_t c4 = r[x - 1],          c5 = r[x],          c6 = r[x + 1],          S2 = r[x + 2];
      px_t c1 = r[x + stride - 1], c2 = r[x + stride], c3 = r[x + stride + 1], S1 = r[x + stride + 2];
      px_t A0 = r[x + 2 * stride - 1], A1 = r[x + 2 * stride], A2 = r[x + 2 * stride + 1], A3 = r[x + 2 * stride + 2];
      px_t p1a, p1b, p2a, p2b;

      if (c2 == c6 && c5 != c3) p2b = p1b = c2;
      else if (c5 == c3 && c2 != c6) p2b = p1b = c5;
      else if (c5 == c3 && c2 == c6)
      {
        int rr = sai_r1(c6, c5, c1, A1) + sai_r1(c6, c5, c4, B1) + sai_r1(c6, c5, A2, S1) + sai_r1(c6, c5, B2, S2);
        if (rr > 0)      p2b = p1b = c6;
        else if (rr < 0) p2b = p1b = c5;
        else             p2b = p1b = avg2(c5, c6);
      }
      else
      {
        if (c6 == c3 && c3 == A1 && c2 != A2 && c3 != A0) p2b = avg31(c3, c2);
        else if (c5 == c2 && c2 == A2 && A1 != c3 && c2 != A3) p2b = avg31(c2, c3);
        else p2b = avg2(c2, c3);

        if (c6 == c3 && c6 == B1 && c5 != B2 && c6 != B0) p1b = avg31(c6, c5);
        else if (c5 == c2 && c5 == B2 && B1 != c6 && c5 != B3) p1b = avg31(c5, c6);
        else p1b = avg2(c5, c6);
      }

      if (c5 == c3 && c2 != c6 && c4 == c5 && c5 != A2) p2a = avg2(c2, c5);
      else if (c5 == c1 && c6 == c5 && c4 != c2 && c5 != A0) p2a = avg2(c2, c5);
      else p2a = c2;

      if (c2 == c6 && c5 != c3 && c1 == c2 && c2 != B2) p1a = avg2(c2, c5);
      else if (c4 == c2 && c3 == c2 && c1 != c5 && c2 != B0) p1a = avg2(c2, c5);
      else p1a = c5;

      o0[2 * x] = p1a; o0[2 * x + 1] = p1b;
      o1[2 * x] = p2a; o1[2 * x + 1] = p2b;
    }
  }
}


/****************************************************************************
 * Table
 ****************************************************************************/

static const filter_def_t filters[] =
{
  /* The order here is the order in the menu. Names, not positions, are what the
     settings file stores. */
  /* Sorted by name (what the menu and the Settings list show); group is the sub-menu of the Render Filter menu. */
  { "2xSaI",                 2, f_2xsai, 0, "SaI and Eagle" },
  { "CRT 3x (RGB Mask)",     3, f_crt3x, 0, "CRT" },
  { "CRT Aperture Grille 3x", 3, f_crt_grille3x, 0, "CRT" },
  { "CRT Geom 3x",           3, f_crtgeom3x, 0, "CRT" },
  { "CRT Geom 4x",           4, f_crtgeom4x, 0, "CRT" },
  { "CRT Shadow Mask 3x",    3, f_crt_shadow3x, 0, "CRT" },
  { "Eagle 2x",              2, f_eagle2x, 0, "SaI and Eagle" },
  { "Glow Scanlines 3x",     3, f_glow3x, 0, "Scanlines" },
  { "HQ2x",                  2, f_hq2x, 0, "HQ" },
  { "HQ3x",                  3, f_hq3x, 0, "HQ" },
  { "HQ4x",                  4, f_hq4x, 0, "HQ" },
  { "B-Spline 3x",           3, f_bspline3x, 0, "Bicubic" },
  { "B-Spline 4x",           4, f_bspline4x, 0, "Bicubic" },
  { "Catmull-Rom 3x",        3, f_catmull3x, 0, "Bicubic" },
  { "Catmull-Rom 4x",        4, f_catmull4x, 0, "Bicubic" },
  { "Lanczos 3x",            3, f_lanczos3x, 0, "Lanczos" },
  { "Lanczos 4x",            4, f_lanczos4x, 0, "Lanczos" },
  { "LQ2x",                  2, f_lq2x, 0, "HQ" },
  { "Mitchell 3x",           3, f_mitchell3x, 0, "Bicubic" },
  { "Mitchell 4x",           4, f_mitchell4x, 0, "Bicubic" },
  { "LCD Grid 3x",           3, f_lcd3x, 0, "LCD Grid" },
  { "LCD Grid 4x",           4, f_lcd4x, 0, "LCD Grid" },
  { "Scale2x",               2, f_scale2x, 0, "Scale" },
  { "Scale3x",               3, f_scale3x, 0, "Scale" },
  { "ScaleFX 3x",            3, f_scalefx3x, 0, "Scale" },
  { "Scanlines 2x",          2, f_scan2x, 0, "Scanlines" },
  { "Scanlines 3x",          3, f_scan3x, 0, "Scanlines" },
  { "Scanlines 4x",          4, f_scan4x, 0, "Scanlines" },
  { "Sharp 4x",              4, f_sharp4x, 0, "Sharp" },
  { "Sharp Bilinear 3x",     3, f_sharpbil3x, 0, "Sharp" },
  { "Sharp Bilinear 4x",     4, f_sharpbil4x, 0, "Sharp" },
  { "Smooth 2x (xBR-Style)", 2, f_smooth2x, 0, "Smooth" },
  { "Smooth 4x (xBR-Style)", 4, f_smooth4x, 0, "Smooth" },
  { "Super 2xSaI",           2, f_super2xsai, 0, "SaI and Eagle" },
  { "Super Eagle",           2, f_supereagle, 0, "SaI and Eagle" },
  { "xBR 2x",                2, f_xbr2x, 0, "xBR" },
  { "xBR 3x",                3, f_xbr3x, 0, "xBR" },
  { "xBR 4x",                4, f_xbr4x, 0, "xBR" },

#ifdef GPEX_XBRZ
  { "xBRZ 2x",               2, f_xbrz2, FILTER_NEEDS_XBRZ, "xBRZ" },
  { "xBRZ 3x",               3, f_xbrz3, FILTER_NEEDS_XBRZ, "xBRZ" },
  { "xBRZ 4x",               4, f_xbrz4, FILTER_NEEDS_XBRZ, "xBRZ" },
  { "xBRZ 5x",               5, f_xbrz5, FILTER_NEEDS_XBRZ, "xBRZ" },
  { "xBRZ 6x",               6, f_xbrz6, FILTER_NEEDS_XBRZ, "xBRZ" },
#endif
};

#define FILTER_TOTAL ((int)(sizeof(filters) / sizeof(filters[0])))

int filter_count(void)
{
  return FILTER_TOTAL;
}

const filter_def_t *filter_get(int index)
{
  return (index >= 0 && index < FILTER_TOTAL) ? &filters[index] : NULL;
}

static char lower(char c)
{
  return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* Case-insensitive match on the filter name. A trailing ".rpi" is ignored so
   a config file written by the old plugin-based build still resolves when the
   plugin happened to share a name with a built-in ("Scale2x.rpi"). */
int filter_find(const char *name)
{
  size_t n;
  int    i;
  char   renamed[8];

  if (!name || !name[0]) return -1;

  /* The HQ filters were called "HQ-Style 2x" while they were an imitation of hqx: a saved choice of one still finds its HQ2x. */
  if (strncmp(name, "HQ-Style ", 9) == 0 && name[9] >= '2' && name[9] <= '4' && name[10] == 'x' && !name[11])
  {
    renamed[0] = 'H'; renamed[1] = 'Q'; renamed[2] = name[9]; renamed[3] = 'x'; renamed[4] = '\0';
    name = renamed;
  }

  n = strlen(name);
  if (n > 4 && name[n - 4] == '.' && lower(name[n - 3]) == 'r' &&
      lower(name[n - 2]) == 'p' && lower(name[n - 1]) == 'i')
  {
    n -= 4;
  }

  for (i = 0; i < FILTER_TOTAL; i++)
  {
    const char *f = filters[i].name;
    size_t      k;

    if (strlen(f) != n) continue;
    for (k = 0; k < n; k++)
    {
      if (lower(f[k]) != lower(name[k])) break;
    }
    if (k == n) return i;
  }

  return -1;
}

int filter_prepare(int index)
{
  const filter_def_t *f = filter_get(index);
  static int last = -2;

  if (index != last) { libxbr_release(); last = index; }              /* (another filter: what the one before kept is given back) */

#ifdef GPEX_XBRZ
  if (f && (f->flags & FILTER_NEEDS_XBRZ)) return xbrz_glue_prepare();

  /* Not an xBRZ filter (or no filter): give back the 64 MB it may have held. */
  xbrz_glue_release();
#endif

  return 1;
}

int filter_apply(int index, const uint16_t *src, int src_pitch, int w, int h,
                 uint16_t *dst, int dst_pitch)
{
  const filter_def_t *f = filter_get(index);

  if (!f || !src || !dst || w < 1 || h < 1) return 0;
  if (w > FILTER_MAX_W || h > FILTER_MAX_H) return 0;

  alloc_failed = 0;
  f->run(src, src_pitch, w, h, dst, dst_pitch);
  return alloc_failed ? 0 : f->scale;
}
