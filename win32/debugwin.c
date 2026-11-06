/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  debugwin.c -- Tools > VDP Viewer, Sound Viewer and CPU Viewer: windows of
 *  their own that stay open while the game runs and show what the video chip,
 *  the sound chips and the processors hold now (the registers, the palette,
 *  the tiles, the sprites and the planes; the FM channels and the PSG; the
 *  68000 and Z80 registers).
 *
 *  All three share one window: a View choice at the top, and below it either
 *  lines of text or a picture (scrollable, with a Zoom). The core is read
 *  directly (nothing in it is changed). The idea of these views comes from the
 *  Genesis Plus GX debugger project (smd_dgx); this code is written for Win32.
 ****************************************************************************/

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>

#include "shared.h"
#include "gui.h"
#include "theme.h"
#include "resource.h"
#include "netplay.h"
#include "picocore.h"
#include "toolbar.h"            /* gui_dpi */

#define DV_CLASS     "GPEXDebugView"
#define DV_TIMER     1
#define DV_TICK_MS   250
#define DV_MAXLINES  200
#define DV_LINELEN   160
#define DV_MAXPIX    (1024 * 1024)
#define IDC_DV_VIEW  101
#define IDC_DV_OPT   102
#define IDC_DV_ZOOM  103
#define IDC_DV_BTN   110        /* .. 113 */

typedef struct
{
  const char *name;
  int image;                         /* 1 = a picture, 0 = lines of text */
  const char *const *opts;           /* a second choice for this view (NULL: none) */
  int nopts;
  int zoom;                          /* the zoom a picture starts with (1-4) */
  const char *const *btns;           /* buttons for this view (NULL: none) */
  int nbtns;
  int custom;                        /* 1 = the view draws itself (def->custom), no scrolling */
} dv_view;

typedef struct
{
  const char *title;
  const dv_view *views;
  int nviews;
  int (*text)(int view, int opt, char lines[][DV_LINELEN], int max);                                 /* returns the number of lines */
  int (*image)(int view, int opt, uint32 *px, int *w, int *h, char *msg, int msglen);                /* 1 = a picture; 0 = msg tells why not */
  void (*action)(HWND owner, int view, int btn, char lines[][DV_LINELEN], int nlines);               /* a button of the view was clicked */
  void (*custom)(HDC dc, const RECT *area, int view, int opt);                                       /* draws a view with custom = 1 */
} dv_def;

typedef struct
{
  const dv_def *def;
  UINT_PTR look;                     /* what the window was laid out for (see dbg_look) */
  HWND hwnd, view_cb, opt_cb, zoom_cb, btn[4], tt;
  int view, opt, zoom[16];
  int sel_line;                      /* the line of a text view that was clicked (-1: none) */
  int hover_x, hover_y, lock_x, lock_y;   /* the tile of a plane under the pointer, and the one locked by a click (-1: none) */
  int info_x;                        /* where the note in the strip starts */
  int pan_x, pan_y, panning;         /* a drag with the middle button */
  char info[220];
  int top, left;                     /* the scroll position: lines, or pixels of the picture */
  char (*lines)[DV_LINELEN];
  char (*prev)[DV_LINELEN];
  int nlines, nprev;
  uint32 *px, *prevpx;
  int iw, ih, pw, ph;
  char msg[160];
} dv_win;

static HFONT dv_mono, dv_ui;
static int dv_cw, dv_lh;

static int S(int px)
{
  return dbg_scale(px);
}

static void dv_fonts(void)
{
  HDC dc;
  TEXTMETRICA tm;
  HFONT old;

  if (dv_mono == dbg_mono_font() && dv_ui == dbg_ui_font() && dv_cw) return;
  dv_mono = dbg_mono_font();
  dv_ui = dbg_ui_font();
  dc = GetDC(NULL);
  old = (HFONT)SelectObject(dc, dv_mono);
  GetTextMetricsA(dc, &tm);
  SelectObject(dc, old);
  ReleaseDC(NULL, dc);
  dv_cw = tm.tmAveCharWidth > 0 ? tm.tmAveCharWidth : S(8);
  dv_lh = tm.tmHeight + S(2);
}

static void dv_colors(COLORREF *bg, COLORREF *fg, COLORREF *strip)
{
  HDC probe = GetDC(NULL);
  HBRUSH b = theme_ctlcolor(probe);

  if (b) { *bg = GetBkColor(probe); *fg = GetTextColor(probe); *strip = *bg; }
  else { *bg = GetSysColor(COLOR_WINDOW); *fg = GetSysColor(COLOR_WINDOWTEXT); *strip = GetSysColor(COLOR_BTNFACE); }
  ReleaseDC(NULL, probe);
}

static int dv_strip_h(void)
{
  return S(38);
}

/****************************************************************************
 * What the VDP holds
 ****************************************************************************/

/* The video chip's memories and registers: the core's, or (a 32X game) those of PicoDrive's library. */
static const uint8 *dv_vram, *dv_cram, *dv_reg;

static int vdp_source(void)
{
  unsigned n;

  if (pico_active())
  {
    dv_vram = pico_memory(3, &n);
    dv_cram = pico_memory(4, &n);
    dv_reg = pico_memory(6, &n);
    return dv_vram && dv_cram && dv_reg;
  }
  dv_vram = vram; dv_cram = cram; dv_reg = reg;
  return 1;
}

static int dv_pal(void)
{
  return pico_active() ? pico_fps() < 55.0 : vdp_pal;
}

/* Where the chip keeps its tables, from its registers (the same way for both kinds of game). */
static unsigned vdp_ntab(void) { return (unsigned)(dv_reg[2] & 0x38) << 10; }
static unsigned vdp_ntbb(void) { return (unsigned)(dv_reg[4] & 0x07) << 13; }
static unsigned vdp_ntwb(void) { return (unsigned)(dv_reg[3] & ((dv_reg[12] & 0x81) ? 0x3C : 0x3E)) << 10; }
static unsigned vdp_satb(void) { return (unsigned)(dv_reg[5] & ((dv_reg[12] & 0x81) ? 0x7E : 0x7F)) << 9; }
static unsigned vdp_hscb(void) { return (unsigned)(dv_reg[13] & 0x3F) << 10; }

static int vdp_plane_tw, vdp_plane_th;          /* the plane drawn last: its size in tiles, and where its table is */
static unsigned vdp_plane_base;
static char vdp_note[200];                      /* a line about the picture drawn last (shown in the strip) */

static int vdp_md5(char *msg, int len)
{
  if (!vdp_source())
  {
    gui_snprintf(msg, len, "The 32X library has no video chip view: use the Plugins folder of the newest zip.");
    return 0;
  }
  if (!pico_active() && (system_hw & SYSTEM_PBC) != SYSTEM_MD)
  {
    gui_snprintf(msg, len, "This view is for Mega Drive games (the video chip of the other consoles works differently).");
    return 0;
  }
  if (!(dv_reg[1] & 4))
  {
    gui_snprintf(msg, len, "The game has the video chip in its old Master System mode now.");
    return 0;
  }
  return 1;
}

/* One byte of the video memory, in the order the game sees (the core keeps every pair of bytes swapped). */
static int vram_b(unsigned a)
{
  return dv_vram[(a & 0xFFFF) ^ 1];
}

static unsigned vram_w(unsigned a)
{
  return ((unsigned)vram_b(a) << 8) | (unsigned)vram_b(a + 1);
}

static uint32 md_color(int i)
{
  unsigned c = (unsigned)dv_cram[(i & 63) * 2] | ((unsigned)dv_cram[(i & 63) * 2 + 1] << 8);
  unsigned r, g, b;

  if (pico_active()) { r = (c >> 1) & 7; g = (c >> 5) & 7; b = (c >> 9) & 7; }      /* PicoDrive keeps the colors as the game writes them (BBB0GGG0RRR0) */
  else { r = c & 7; g = (c >> 3) & 7; b = (c >> 6) & 7; }                           /* the core keeps them in 9 bits (BBBGGGRRR) */

  return ((r * 255 / 7) << 16) | ((g * 255 / 7) << 8) | (b * 255 / 7);
}

static int vdp_text(int view, int opt, char lines[][DV_LINELEN], int max)
{
  int n = 0, i;
  char msg[160];

  if (view == 0)
  {
    static const char *const names[24] = {
      "Mode 1", "Mode 2", "Plane A Address", "Window Address", "Plane B Address", "Sprite Address", "(Unused)", "Backdrop Color",
      "(Unused)", "(Unused)", "H Interrupt Counter", "Mode 3", "Mode 4", "H Scroll Address", "(Unused)", "Auto Increment",
      "Plane Size", "Window Horizontal", "Window Vertical", "DMA Length Low", "DMA Length High", "DMA Source Low", "DMA Source Mid", "DMA Source High" };

    if (!vdp_source() || (!pico_active() && (system_hw & SYSTEM_PBC) != SYSTEM_MD))
    {
      vdp_md5(msg, sizeof(msg));
      gui_snprintf(lines[n++], DV_LINELEN, "%s", msg);
      return n;
    }
    for (i = 0; i < 24 && n < max; i++)
      gui_snprintf(lines[n++], DV_LINELEN, "%02X  %-20s %02X", i, names[i], dv_reg[i]);
    if (n < max) gui_snprintf(lines[n++], DV_LINELEN, " ");
    if (n < max) gui_snprintf(lines[n++], DV_LINELEN, "Display %s, %s mode, %s, %s",
                              (dv_reg[1] & 0x40) ? "on" : "off", (dv_reg[12] & 0x81) ? "H40" : "H32", (dv_reg[1] & 8) ? "V30" : "V28",
                              dv_pal() ? "PAL" : "NTSC");
    if (n < max) gui_snprintf(lines[n++], DV_LINELEN, "Interrupts: V %s, H %s;  DMA %s;  Interlace %s;  Shadow / Highlight %s",
                              (dv_reg[1] & 0x20) ? "on" : "off", (dv_reg[0] & 0x10) ? "on" : "off", (dv_reg[1] & 0x10) ? "on" : "off",
                              ((dv_reg[12] >> 1) & 3) == 3 ? "double" : ((dv_reg[12] >> 1) & 3) == 1 ? "on" : "off", (dv_reg[12] & 8) ? "on" : "off");
    if (n < max) gui_snprintf(lines[n++], DV_LINELEN, "Plane A %04X, Plane B %04X, Window %04X, Sprites %04X, H scroll %04X",
                              vdp_ntab(), vdp_ntbb(), vdp_ntwb(), vdp_satb(), vdp_hscb());
    return n;
  }

  /* Sprites */
  if (!vdp_md5(msg, sizeof(msg))) { gui_snprintf(lines[n++], DV_LINELEN, "%s", msg); return n; }
  {
    int count = (dv_reg[12] & 0x81) ? 80 : 64, idx = 0, seen = 0;

    gui_snprintf(lines[n++], DV_LINELEN, opt ? "Sprite Table at %04X, %d sprites, in the order of the table." : "Sprite Table at %04X, up to %d sprites, in the order the chip draws them (the link chain).", vdp_satb(), count);
    gui_snprintf(lines[n++], DV_LINELEN, "No.  X     Y     Size  Tile  Pal  Pri  Flip  Link");
    while (seen < count && n < max)
    {
      unsigned a = (vdp_satb() + (unsigned)idx * 8) & 0xFFFF;
      unsigned w0 = vram_w(a), w1 = vram_w(a + 2), w2 = vram_w(a + 4), w3 = vram_w(a + 6);
      int link = (int)(w1 & 0x7F);

      gui_snprintf(lines[n++], DV_LINELEN, "%2d   %-4d  %-4d  %dx%d   %03X   %d    %d    %c%c    %d",
                   idx, (int)(w3 & 0x1FF) - 128, (int)(w0 & 0x3FF) - 128, (int)((w1 >> 10) & 3) + 1, (int)((w1 >> 8) & 3) + 1,
                   (unsigned)(w2 & 0x7FF), (int)((w2 >> 13) & 3), (int)((w2 >> 15) & 1), (w2 & 0x800) ? 'H' : '-', (w2 & 0x1000) ? 'V' : '-', link);
      seen++;
      if (opt) { if (seen >= count) break; idx = seen; continue; }
      if (!link || link >= count) break;
      idx = link;
    }
  }
  return n;
}

static int vdp_src_ram;                         /* the Tiles view reads the 68000's RAM instead of VRAM (to see tile data before it is sent to the video chip) */

static int vdp_tile_byte(unsigned a)
{
  return vdp_src_ram ? dbgmem_get(pico_active() ? MV_PWRAM : MV_WRAM, (int)(a & 0xFFFF)) : vram_b(a);
}

static void put_tile(uint32 *px, int stride, int x, int y, unsigned tile, int pal, int hf, int vf)
{
  int r, c;

  for (r = 0; r < 8; r++)
    for (c = 0; c < 8; c++)
    {
      int sy = vf ? 7 - r : r, sx = hf ? 7 - c : c;
      int b = vdp_tile_byte(tile * 32 + (unsigned)sy * 4 + (unsigned)(sx >> 1));
      int idx = (sx & 1) ? (b & 15) : (b >> 4);

      px[(y + r) * stride + x + c] = md_color(pal * 16 + idx);
    }
}

/* The attributes of sprite n of the table: where it is, how big, its tile, palette and flips. */
typedef struct { int x, y, wt, ht, link; unsigned tile; int pal, pri, hf, vf; } vdp_sprite_t;

static void vdp_read_sprite(int n, vdp_sprite_t *s)
{
  unsigned a = (vdp_satb() + (unsigned)n * 8) & 0xFFFF;
  unsigned w0 = vram_w(a), w1 = vram_w(a + 2), w2 = vram_w(a + 4), w3 = vram_w(a + 6);

  s->x = (int)(w3 & 0x1FF); s->y = (int)(w0 & 0x3FF);
  s->wt = (int)((w1 >> 10) & 3) + 1; s->ht = (int)((w1 >> 8) & 3) + 1; s->link = (int)(w1 & 0x7F);
  s->tile = w2 & 0x7FF; s->pal = (int)((w2 >> 13) & 3); s->pri = (int)((w2 >> 15) & 1); s->hf = (w2 & 0x800) != 0; s->vf = (w2 & 0x1000) != 0;
}

/* Draws a sprite (its tiles go down the columns, first column first) at x, y of an image of width stride and height rows; a color 0 pixel is left alone. */
static void vdp_draw_sprite(uint32 *px, int stride, int rows, int x0, int y0, const vdp_sprite_t *s)
{
  int c, r, tx, ty;

  for (c = 0; c < s->wt; c++)
    for (r = 0; r < s->ht; r++)
    {
      int cc = s->hf ? s->wt - 1 - c : c, rr = s->vf ? s->ht - 1 - r : r;
      unsigned tile = (s->tile + (unsigned)(cc * s->ht + rr)) & 0x7FF;

      for (ty = 0; ty < 8; ty++)
        for (tx = 0; tx < 8; tx++)
        {
          int sy = s->vf ? 7 - ty : ty, sx = s->hf ? 7 - tx : tx;
          int b = vram_b(tile * 32 + (unsigned)sy * 4 + (unsigned)(sx >> 1)), idx = (sx & 1) ? (b & 15) : (b >> 4);
          int dx = x0 + c * 8 + tx, dy = y0 + r * 8 + ty;

          if (idx && dx >= 0 && dx < stride && dy >= 0 && dy < rows) px[dy * stride + dx] = md_color(s->pal * 16 + idx);
        }
    }
}

static char vdp_sprite_names[80][12];
static const char *vdp_sprite_opts[80];

static int vdp_image(int view, int opt, uint32 *px, int *w, int *h, char *msg, int msglen)
{
  int i, x, y;

  if (!vdp_md5(msg, msglen)) return 0;

  if (view == 7)                                       /* the sprite layer: every sprite where it is, the first of the chain on top; the screen is framed */
  {
    int chain[80], n = 0, idx = 0, count = (dv_reg[12] & 0x81) ? 80 : 64;
    int sw = (dv_reg[12] & 0x81) ? 320 : 256, sh = (dv_reg[1] & 8) ? 240 : 224;
    uint32 bgc = md_color(dv_reg[7] & 0x3F), frame = 0x808080;

    *w = 512; *h = 512;
    for (i = 0; i < 512 * 512; i++) px[i] = bgc;
    while (n < count)
    {
      vdp_sprite_t s;

      chain[n++] = idx;
      vdp_read_sprite(idx, &s);
      if (!s.link || s.link >= count) break;
      idx = s.link;
    }
    for (i = n - 1; i >= 0; i--)
    {
      vdp_sprite_t s;

      vdp_read_sprite(chain[i], &s);
      vdp_draw_sprite(px, 512, 512, s.x, s.y & 0x1FF, &s);
    }
    for (x = 128; x < 128 + sw; x++) { px[128 * 512 + x] = frame; px[(128 + sh - 1) * 512 + x] = frame; }
    for (y = 128; y < 128 + sh; y++) { px[y * 512 + 128] = frame; px[y * 512 + 128 + sw - 1] = frame; }
    gui_snprintf(vdp_note, sizeof(vdp_note), "%d sprites in the chain. The gray frame is the screen (%d x %d).", n, sw, sh);
    return 1;
  }
  if (view == 8)                                       /* one sprite, large, with its palette line under it */
  {
    vdp_sprite_t s;

    vdp_read_sprite(opt < 80 ? opt : 0, &s);
    *w = 128; *h = 4 * 8 + 12;
    for (i = 0; i < *w * *h; i++) px[i] = 0x202020;
    vdp_draw_sprite(px, *w, *h, 0, 0, &s);
    for (x = 0; x < 128; x++)
      for (y = 4 * 8 + 2; y < 4 * 8 + 10; y++) px[y * 128 + x] = md_color(s.pal * 16 + x / 8);
    gui_snprintf(vdp_note, sizeof(vdp_note), "Sprite %d: %d x %d tiles at %d, %d; tile %03X, palette %d%s%s%s", opt, s.wt, s.ht, s.x - 128, (s.y & 0x3FF) - 128, s.tile, s.pal,
                 s.pri ? ", priority" : "", s.hf ? ", H flip" : "", s.vf ? ", V flip" : "");
    return 1;
  }
  if (view == 9)                                       /* the horizontal scroll of every line (red: Plane A, green: Plane B) */
  {
    int lines = (dv_reg[1] & 8) ? 240 : 224, mode = dv_reg[11] & 3;

    *w = lines; *h = 256;
    for (i = 0; i < *w * *h; i++) px[i] = 0x181818;
    for (y = 0; y < 256; y += 16) for (x = 0; x < lines; x++) px[y * lines + x] = 0x303030;
    for (x = 0; x < lines; x += 8) for (y = 0; y < 256; y++) if (!(y & 1)) px[y * lines + x] = 0x282828;
    for (x = 0; x < lines; x++)
    {
      unsigned e = vdp_hscb() + (mode == 3 ? (unsigned)x * 4 : mode == 0 ? 0u : (unsigned)(x & ~7) * 4);
      int va = (int)(vram_w(e) & 0x3FF), vb = (int)(vram_w(e + 2) & 0x3FF);

      px[(255 - (va >> 2)) * lines + x] = 0xFF6060;
      px[(255 - (vb >> 2)) * lines + x] = 0x60FF60;
    }
    gui_snprintf(vdp_note, sizeof(vdp_note), "H scroll, %s (the table is at %04X). Plane A red, Plane B green; the height is 0 to 1023.",
                 mode == 0 ? "the whole screen" : mode == 3 ? "every line" : "every 8 lines", vdp_hscb());
    return 1;
  }
  if (view == 10)                                      /* the vertical scroll of every column (Plane A above, Plane B below) */
  {
    int cols = (dv_reg[11] & 4) ? 20 : 1, c, bw = 320 / cols;

    *w = 320; *h = 2 * 128 + 4;
    for (i = 0; i < *w * *h; i++) px[i] = 0x181818;
    for (c = 0; c < cols; c++)
      for (i = 0; i < 2; i++)
      {
        int idx = (cols == 1 ? 0 : c * 2) + i, v = ((dbgmem_get(MV_VSRAM, idx * 2) << 8) | dbgmem_get(MV_VSRAM, idx * 2 + 1)) & 0x3FF, bh = v >> 3;

        for (y = 0; y < bh; y++)
          for (x = c * bw + 1; x < (c + 1) * bw - 1; x++)
            px[(i * 130 + 127 - y) * 320 + x] = i ? 0x60FF60 : 0xFF6060;
      }
    gui_snprintf(vdp_note, sizeof(vdp_note), "V scroll, %s. Plane A red (above), Plane B green (below); the height is 0 to 1023.", cols == 1 ? "the whole screen" : "every 2 columns of tiles (20 of them)");
    return 1;
  }

  if (view == 1)                                       /* the palette: four lines of sixteen */
  {
    *w = 16 * 16; *h = 4 * 16;
    for (y = 0; y < *h; y++)
      for (x = 0; x < *w; x++) px[y * *w + x] = md_color((y / 16) * 16 + x / 16);
    gui_snprintf(vdp_note, sizeof(vdp_note), "The 64 colors of the 4 palette lines. Point at one for its value, double-click to change it.");
    return 1;
  }
  if (view == 2)                                       /* the tiles of the video memory */
  {
    *w = 32 * 8; *h = 64 * 8;
    vdp_src_ram = opt >= 4;
    for (i = 0; i < 0x800; i++) put_tile(px, *w, (i % 32) * 8, (i / 32) * 8, (unsigned)i, opt & 3, 0, 0);
    vdp_src_ram = 0;
    gui_snprintf(vdp_note, sizeof(vdp_note), "All 2048 tiles of %s, 32 to a row. Point at one for its details, click to lock it.", opt >= 4 ? "the 68000's RAM (as if it were VRAM)" : "VRAM");
    return 1;
  }
  {                                                    /* a plane: Plane A, Plane B or the Window */
    unsigned base = view == 4 ? vdp_ntab() : view == 5 ? vdp_ntbb() : vdp_ntwb();
    static const int dim[4] = { 32, 64, 32, 128 };
    int tw = dim[dv_reg[16] & 3], th = dim[(dv_reg[16] >> 4) & 3], tx, ty;

    if (view == 6) { tw = (dv_reg[12] & 0x81) ? 64 : 32; th = 32; }
    *w = tw * 8; *h = th * 8;
    vdp_plane_tw = tw; vdp_plane_th = th; vdp_plane_base = base;
    gui_snprintf(vdp_note, sizeof(vdp_note), "%s: %d x %d tiles at %04X. Point at a tile for its details, click to lock.", view == 4 ? "Plane A" : view == 5 ? "Plane B" : "Window", tw, th, base);
    for (ty = 0; ty < th; ty++)
      for (tx = 0; tx < tw; tx++)
      {
        unsigned e = vram_w(base + (unsigned)(ty * tw + tx) * 2);

        put_tile(px, *w, tx * 8, ty * 8, e & 0x7FF, (int)((e >> 13) & 3), (e & 0x800) != 0, (e & 0x1000) != 0);
      }
    return 1;
  }
}

static void vdp_rgb_to_cram(int r, int g, int b, unsigned char *out)
{
  unsigned w = (unsigned)(((b >> 5) & 7) << 9) | (unsigned)(((g >> 5) & 7) << 5) | (unsigned)(((r >> 5) & 7) << 1);

  out[0] = (unsigned char)(w >> 8);
  out[1] = (unsigned char)(w & 0xFF);
}

/****************************************************************************
 * The video register editor (modal)
 ****************************************************************************/

static int dv_sel_line = -1;                    /* the line of the text view that was clicked: a register of the Registers view */

/* What a bit of a mode register means (NULL: not a bit of a mode). */
static const char *vdp_bit_name(int r, int b)
{
  static const char *const r0[8] = { NULL, "HV counter stop", "Palette select (leave on)", NULL, "H interrupt enable", NULL, NULL, NULL };
  static const char *const r1[8] = { NULL, NULL, "Mega Drive mode (leave on)", "V30 (240 lines, PAL)", "DMA enable", "V interrupt enable", "Display enable", "128 KB VRAM mode" };
  static const char *const r11[8] = { "H scroll mode, low bit", "H scroll mode, high bit", "V scroll by 2 columns", "External interrupt enable", NULL, NULL, NULL, NULL };
  static const char *const r12[8] = { "H40 (320 pixels)", "Interlace, low bit", "Interlace, high bit", "Shadow / highlight", NULL, NULL, NULL, "H40 (320 pixels), with bit 0" };

  return r == 0 ? r0[b] : r == 1 ? r1[b] : r == 11 ? r11[b] : r == 12 ? r12[b] : NULL;
}

typedef struct
{
  int reg, value, done, ok, busy;
  HWND dlg, combo, edit, check[8], owner;
} regedit_t;

static void vdp_reg_load_names(regedit_t *e)
{
  int b;
  char t[80];

  for (b = 0; b < 8; b++)
  {
    const char *n = vdp_bit_name(e->reg, 7 - b);

    if (n) gui_snprintf(t, sizeof(t), "Bit %d: %s", 7 - b, n); else gui_snprintf(t, sizeof(t), "Bit %d", 7 - b);
    SetWindowTextA(e->check[b], t);
  }
}

static void vdp_reg_show(regedit_t *e)
{
  char t[8];
  int b;

  e->busy = 1;
  for (b = 0; b < 8; b++) SendMessage(e->check[b], BM_SETCHECK, (e->value >> (7 - b)) & 1 ? BST_CHECKED : BST_UNCHECKED, 0);
  gui_snprintf(t, sizeof(t), "%02X", e->value & 0xFF);
  SetWindowTextA(e->edit, t);
  e->busy = 0;
}

static LRESULT CALLBACK vdp_reg_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
  regedit_t *e = (regedit_t *)GetWindowLongPtr(h, GWLP_USERDATA);

  if (m == WM_NCCREATE) { e = (regedit_t *)((CREATESTRUCTA *)lp)->lpCreateParams; SetWindowLongPtr(h, GWLP_USERDATA, (LONG_PTR)e); }
  if (!e) return DefWindowProc(h, m, wp, lp);

  switch (m)
  {
    case WM_COMMAND:
      if (LOWORD(wp) == IDOK) { e->ok = 1; e->done = 1; return 0; }
      if (LOWORD(wp) == IDCANCEL) { e->done = 1; return 0; }
      if (LOWORD(wp) == 301 && HIWORD(wp) == CBN_SELCHANGE)
      {
        int k = (int)SendMessage(e->combo, CB_GETCURSEL, 0, 0);

        if (k >= 0) { e->reg = k; e->value = dv_reg[k]; vdp_reg_load_names(e); vdp_reg_show(e); }
        return 0;
      }
      if (LOWORD(wp) >= 310 && LOWORD(wp) < 318 && HIWORD(wp) == BN_CLICKED && !e->busy)
      {
        int b, v = 0;

        for (b = 0; b < 8; b++) if (SendMessage(e->check[b], BM_GETCHECK, 0, 0) == BST_CHECKED) v |= 0x80 >> b;
        e->value = v;
        vdp_reg_show(e);
        return 0;
      }
      if (LOWORD(wp) == 302 && HIWORD(wp) == EN_CHANGE && !e->busy)
      {
        char t[16];
        int b;

        GetWindowTextA(e->edit, t, sizeof(t));
        e->value = (int)(strtoul(t, NULL, 16) & 0xFF);
        e->busy = 1;
        for (b = 0; b < 8; b++) SendMessage(e->check[b], BM_SETCHECK, (e->value >> (7 - b)) & 1 ? BST_CHECKED : BST_UNCHECKED, 0);
        e->busy = 0;
        return 0;
      }
      break;
    case WM_CLOSE:
      e->done = 1;
      return 0;
    case WM_ERASEBKGND:                                           /* the window's own background: dark in the dark style */
    {
      HBRUSH bg = theme_ctlcolor((HDC)wp);
      RECT cr;

      if (!bg) break;
      GetClientRect(h, &cr);
      FillRect((HDC)wp, &cr, bg);
      return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG:
    {
      HBRUSH b = theme_ctlcolor_ctl((HDC)wp, (HWND)lp);

      if (b) return (LRESULT)b;
      if (m == WM_CTLCOLORSTATIC || m == WM_CTLCOLORDLG) return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
      break;
    }
  }
  return DefWindowProc(h, m, wp, lp);
}

static HWND vdp_reg_ctl(HWND p, const char *cls, const char *text, DWORD style, DWORD ex, int x, int y, int w, int h, int id)
{
  HWND c = CreateWindowExA(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), p, (HMENU)(INT_PTR)id, g_inst, NULL);

  if (c) SendMessage(c, WM_SETFONT, (WPARAM)dv_ui, TRUE);
  return c;
}

/* Sets a register of the video chip the way a game does. */
void dbg_vdp_reg_set(int r, int v)
{
  if (pico_active()) { if (!pico_vdp_reg(r, v)) gui_notify("The 32X library cannot set a video register: use the Plugins folder of the newest zip"); }
  else vdp_68k_ctrl_w((unsigned)(0x8000 | (r << 8) | (v & 0xFF)));
}

static void vdp_reg_edit(HWND owner, int reg)
{
  static int registered;
  static const char *const rnames[24] = {
    "00  Mode 1", "01  Mode 2", "02  Plane A Address", "03  Window Address", "04  Plane B Address", "05  Sprite Address", "06  (Unused)", "07  Backdrop Color",
    "08  (Unused)", "09  (Unused)", "0A  H Interrupt Counter", "0B  Mode 3", "0C  Mode 4", "0D  H Scroll Address", "0E  (Unused)", "0F  Auto Increment",
    "10  Plane Size", "11  Window Horizontal", "12  Window Vertical", "13  DMA Length Low", "14  DMA Length High", "15  DMA Source Low", "16  DMA Source Mid", "17  DMA Source High" };
  regedit_t e;
  MSG msg;
  RECT r, pr;
  DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
  char msgbuf[160];
  int i;

  if (!vdp_md5(msgbuf, sizeof(msgbuf))) { gui_notify("%s", msgbuf); return; }
  if (netplay_active()) { gui_notify("The video chip cannot be changed during netplay"); return; }
  if (!registered)
  {
    WNDCLASSA wc;

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = vdp_reg_proc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "GPEXVdpRegEdit";
    if (!RegisterClassA(&wc)) return;
    registered = 1;
  }
  ZeroMemory(&e, sizeof(e));
  e.reg = reg >= 0 && reg < 24 ? reg : 0;
  e.value = dv_reg[e.reg];
  e.owner = owner;
  r.left = 0; r.top = 0; r.right = S(330); r.bottom = S(330);
  AdjustWindowRectEx(&r, style, FALSE, 0);
  e.dlg = CreateWindowExA(0, "GPEXVdpRegEdit", "Edit Video Register", style, 0, 0, r.right - r.left, r.bottom - r.top, GetAncestor(owner, GA_ROOT), NULL, g_inst, &e);
  if (!e.dlg) return;
  vdp_reg_ctl(e.dlg, "STATIC", "Register:", SS_LEFT, 0, 12, 16, 70, 16, 0);
  e.combo = vdp_reg_ctl(e.dlg, "COMBOBOX", "", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 86, 12, 230, 300, 301);
  for (i = 0; i < 24; i++) SendMessageA(e.combo, CB_ADDSTRING, 0, (LPARAM)rnames[i]);
  SendMessage(e.combo, CB_SETCURSEL, (WPARAM)e.reg, 0);
  vdp_reg_ctl(e.dlg, "STATIC", "Value (hex):", SS_LEFT, 0, 12, 48, 70, 16, 0);
  e.edit = vdp_reg_ctl(e.dlg, "EDIT", "", WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, 86, 45, 60, 22, 302);
  SendMessage(e.edit, EM_SETLIMITTEXT, 2, 0);
  for (i = 0; i < 8; i++) e.check[i] = vdp_reg_ctl(e.dlg, "BUTTON", "", WS_TABSTOP | BS_AUTOCHECKBOX, 0, 16, 80 + i * 24, 300, 20, 310 + i);
  vdp_reg_ctl(e.dlg, "BUTTON", "OK", WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 150, 290, 80, 26, IDOK);
  vdp_reg_ctl(e.dlg, "BUTTON", "Cancel", WS_TABSTOP | BS_PUSHBUTTON, 0, 238, 290, 80, 26, IDCANCEL);
  vdp_reg_load_names(&e);
  vdp_reg_show(&e);
  GetWindowRect(owner, &pr);
  SetWindowPos(e.dlg, NULL, pr.left + ((pr.right - pr.left) - (r.right - r.left)) / 2, pr.top + ((pr.bottom - pr.top) - (r.bottom - r.top)) / 2,
               r.right - r.left, r.bottom - r.top, SWP_NOZORDER);
  theme_apply_to_window(e.dlg);
  {
    static const GuiTip tips[] =
    {
      { 301, NULL, "Which register of the video chip to change." },
      { 302, NULL, "The register's value in hex (two digits). The boxes below follow it." },
      { 310, NULL, "Bit 7 of the value." }, { 311, NULL, "Bit 6 of the value." }, { 312, NULL, "Bit 5 of the value." }, { 313, NULL, "Bit 4 of the value." },
      { 314, NULL, "Bit 3 of the value." }, { 315, NULL, "Bit 2 of the value." }, { 316, NULL, "Bit 1 of the value." }, { 317, NULL, "Bit 0 of the value." },
      { IDOK, NULL, "Write the value to the register, the way a game would." },
      { IDCANCEL, NULL, "Close without changing anything." }
    };

    gui_dialog_tips(e.dlg, tips, (int)(sizeof(tips) / sizeof(tips[0])));
  }
  gui_window_icon(e.dlg);

  EnableWindow(GetAncestor(owner, GA_ROOT), FALSE);
  ShowWindow(e.dlg, SW_SHOW);
  SetFocus(e.edit);
  SendMessage(e.edit, EM_SETSEL, 0, -1);
  while (!e.done && GetMessage(&msg, NULL, 0, 0))
  {
    if (msg.message == WM_KEYDOWN && (msg.wParam == VK_RETURN || msg.wParam == VK_ESCAPE) && IsChild(e.dlg, msg.hwnd))
    {
      char cls[16];

      GetClassNameA(msg.hwnd, cls, sizeof(cls));
      if (lstrcmpiA(cls, "BUTTON") && lstrcmpiA(cls, "COMBOBOX")) { SendMessage(e.dlg, WM_COMMAND, msg.wParam == VK_RETURN ? IDOK : IDCANCEL, 0); continue; }
    }
    if (!IsDialogMessage(e.dlg, &msg)) { TranslateMessage(&msg); DispatchMessage(&msg); }
  }
  if (msg.message == WM_QUIT && !e.done) PostQuitMessage((int)msg.wParam);
  EnableWindow(GetAncestor(owner, GA_ROOT), TRUE);
  DestroyWindow(e.dlg);
  SetForegroundWindow(GetAncestor(owner, GA_ROOT));
  if (e.ok) dbg_vdp_reg_set(e.reg, e.value);
}

static void vdp_action(HWND owner, int view, int btn, char lines[][DV_LINELEN], int nlines)
{
  char msg[160];
  int i;

  if (view == 0 && btn == 0) { vdp_reg_edit(owner, dv_sel_line); return; }
  if (!vdp_md5(msg, sizeof(msg))) { gui_notify("%s", msg); return; }

  if (view == 1 && btn == 0)                          /* Dump Pal: the 64 colors as the game writes them (big-endian words) */
  {
    unsigned char b[0x80];

    for (i = 0; i < 0x80; i++) b[i] = (unsigned char)dbgmem_get(MV_CRAM, i);
    if (dbg_save_file(owner, "Dump Palette", "pal.bin", "Binary files (*.bin)|*.bin|All files (*.*)|*.*", b, sizeof(b))) gui_notify("Palette dumped");
  }
  else if (view == 1 && btn == 1)                     /* Load Pal */
  {
    size_t n = 0;
    unsigned char *b = dbg_open_file(owner, "Load Palette", "Binary files (*.bin)|*.bin|All files (*.*)|*.*", &n);

    if (!b) return;
    if (n < 0x80) gui_notify("A palette file is 128 bytes");
    else { for (i = 0; i < 0x80; i++) dbgmem_put(MV_CRAM, i, b[i]); gui_notify("Palette loaded"); }
    free(b);
  }
  else if (view == 1 && btn == 2)                     /* YY-CHR Pal: 256 colors of three bytes, the first 64 are the game's */
  {
    static unsigned char b[768];

    memset(b, 0, sizeof(b));
    for (i = 0; i < 64; i++)
    {
      uint32 c = md_color(i);

      b[i * 3] = (unsigned char)(c >> 16); b[i * 3 + 1] = (unsigned char)(c >> 8); b[i * 3 + 2] = (unsigned char)c;
    }
    if (dbg_save_file(owner, "Dump YY-CHR Palette", "pal.pal", "YY-CHR palette (*.pal)|*.pal|All files (*.*)|*.*", b, sizeof(b))) gui_notify("YY-CHR palette dumped");
  }
  else if (view == 1 && btn == 3)                     /* Gray && Rainbow: lines 0-2 become an ascending gray, a descending gray and a rainbow (line 3 stays) */
  {
    unsigned char b[96];

    for (i = 0; i < 16; i++) { int v = (i & 7) << 5; vdp_rgb_to_cram(v, v, v, b + i * 2); }
    for (i = 0; i < 16; i++) { int v = ((15 - i) & 7) << 5; vdp_rgb_to_cram(v, v, v, b + 32 + i * 2); }
    for (i = 0; i < 16; i++)
    {
      int sector = (i * 6) / 16, f = (int)(((i * 6) % 16) * 255 / 16), p = 255 * 15 / 100, q = 255 * (255 - f * 85 / 100) / 255, t = 255 * (255 - (255 - f) * 85 / 100) / 255, r, g, bl;

      switch (sector)
      {
        case 0: r = 255; g = t; bl = p; break;
        case 1: r = q; g = 255; bl = p; break;
        case 2: r = p; g = 255; bl = t; break;
        case 3: r = p; g = q; bl = 255; break;
        case 4: r = t; g = p; bl = 255; break;
        default: r = 255; g = p; bl = q; break;
      }
      vdp_rgb_to_cram(r, g, bl, b + 64 + i * 2);
    }
    for (i = 0; i < 96; i++) dbgmem_put(MV_CRAM, i, b[i]);
    gui_notify("Gray and rainbow palette set");
  }
  else if (view == 2 && btn == 0)                     /* Dump VRAM */
  {
    static unsigned char b[0x10000];

    for (i = 0; i < 0x10000; i++) b[i] = (unsigned char)dbgmem_get(MV_VRAM, i);
    if (dbg_save_file(owner, "Dump VRAM", "vram.bin", "Binary files (*.bin)|*.bin|All files (*.*)|*.*", b, sizeof(b))) gui_notify("VRAM dumped");
  }
  else if (view == 2 && btn == 1)                     /* Load VRAM */
  {
    size_t n = 0;
    unsigned char *b = dbg_open_file(owner, "Load VRAM", "Binary files (*.bin)|*.bin|All files (*.*)|*.*", &n);

    if (!b) return;
    if (n < 0x10000) gui_notify("A VRAM file is 65536 bytes");
    else { for (i = 0; i < 0x10000; i++) dbgmem_put(MV_VRAM, i, b[i]); gui_notify("VRAM loaded"); }
    free(b);
  }
  else if (view == 3 && btn == 0)                     /* Dump (the sprite list) */
  {
    char *t = (char *)malloc((size_t)nlines * DV_LINELEN + 16), *o = t;

    if (!t) return;
    for (i = 0; i < nlines; i++) { int l = lstrlenA(lines[i]); memcpy(o, lines[i], (size_t)l); o += l; *o++ = '\r'; *o++ = '\n'; }
    if (dbg_save_file(owner, "Dump Sprite List", "sprites.txt", "Text files (*.txt)|*.txt|All files (*.*)|*.*", t, (size_t)(o - t))) gui_notify("Sprite list dumped");
    free(t);
  }
}

static const char *const vdp_pal_btns[4] = { "Dump Pal", "Load Pal", "YY-CHR Pal", "Gray && Rainbow" };
static const char *const vdp_vram_btns[2] = { "Dump VRAM", "Load VRAM" };
static const char *const vdp_spr_btns[1] = { "Dump" };
static const char *const vdp_tile_opts[8] = { "VRAM, Palette 0", "VRAM, Palette 1", "VRAM, Palette 2", "VRAM, Palette 3", "RAM, Palette 0", "RAM, Palette 1", "RAM, Palette 2", "RAM, Palette 3" };

static const char *const vdp_order_opts[2] = { "Link Chain", "Table Order" };
static const char *const vdp_reg_btns[1] = { "Edit Register..." };

static const dv_view vdp_views[11] = {
  { "Registers", 0, NULL, 0, 1, vdp_reg_btns, 1 },
  { "Palette", 1, NULL, 0, 2, vdp_pal_btns, 4 },
  { "Tiles", 1, vdp_tile_opts, 8, 2, vdp_vram_btns, 2 },
  { "Sprites", 0, vdp_order_opts, 2, 1, vdp_spr_btns, 1 },
  { "Plane A", 1, NULL, 0, 1 },
  { "Plane B", 1, NULL, 0, 1 },
  { "Window", 1, NULL, 0, 1 },
  { "Sprite Layer", 1, NULL, 0, 1 },
  { "Sprite Preview", 1, vdp_sprite_opts, 80, 6 },
  { "H Scroll", 1, NULL, 0, 2 },
  { "V Scroll", 1, NULL, 0, 2 }
};

static const dv_def vdp_def = { "VDP Viewer", vdp_views, 11, vdp_text, vdp_image, vdp_action };

/****************************************************************************
 * What the sound chips hold
 ****************************************************************************/

static int pwm_text(char lines[][DV_LINELEN], int max)
{
  int p[8], n = 0;
  unsigned ctl, cycle;
  static const char *const out_mode[4] = { "off", "left", "right", "(undefined)" };

  (void)max;
  if (!pico_active())
  {
    gui_snprintf(lines[n++], DV_LINELEN, "The PWM sound chip is the 32X's: this game has none.");
    return n;
  }
  if (!pico_pwm_state(p))
  {
    gui_snprintf(lines[n++], DV_LINELEN, "The 32X library has no PWM view: use the Plugins folder of the newest zip.");
    return n;
  }
  ctl = (unsigned)p[0];
  cycle = (unsigned)p[1];
  gui_snprintf(lines[n++], DV_LINELEN, "32X PWM (pulse width modulation) sound chip");
  gui_snprintf(lines[n++], DV_LINELEN, "Control %04X:  left channel plays %s,  right channel plays %s,  interrupt every %d samples (now every %d)",
               ctl & 0xFFFF, out_mode[ctl & 3], out_mode[(ctl >> 2) & 3], p[7], p[6]);
  gui_snprintf(lines[n++], DV_LINELEN, "Cycle %u:  %s", cycle, cycle ? "" : "the chip is stopped");
  if (cycle)
    gui_snprintf(lines[n++], DV_LINELEN, "Sample rate  %d Hz  (%d Hz clock divided by the cycle)", (int)((dv_pal() ? 22801467.0 : 23011361.0) / cycle),
                 (int)(dv_pal() ? 22801467.0 : 23011361.0));
  gui_snprintf(lines[n++], DV_LINELEN, " ");
  gui_snprintf(lines[n++], DV_LINELEN, "Last sample played:  left %6d   right %6d", p[2], p[3]);
  gui_snprintf(lines[n++], DV_LINELEN, "Waiting in the FIFOs (of 3):  left %d   right %d", p[4], p[5]);
  return n;
}

static int snd_text(int view, int opt, char lines[][DV_LINELEN], int max)
{
  static unsigned char ym[2][256], keyon[8], vol[4], noise;
  static unsigned tone[3];
  int n = 0, c, o, chip, i;
  double clk;

  if (view == 2) return pwm_text(lines, max);
  (void)opt;
  if (pico_active())
  {
    if (!pico_sound_state(ym, keyon, tone, vol, &noise))
    {
      gui_snprintf(lines[n++], DV_LINELEN, "The 32X library has no sound chip view: use the Plugins folder of the newest zip.");
      return n;
    }
    chip = 1;
  }
  else chip = vgm_sound_snapshot(ym, keyon, tone, vol, &noise);
  clk = dv_pal() ? 3546895.0 : 3579545.0;

  if (view == 1)
  {
    gui_snprintf(lines[n++], DV_LINELEN, "PSG (SN76489)");
    for (i = 0; i < 3 && n < max; i++)
      gui_snprintf(lines[n++], DV_LINELEN, "Tone %d   period %-4u  %5d Hz   volume %2d of 15", i + 1, tone[i],
                   tone[i] ? (int)(clk / (32.0 * tone[i])) : 0, 15 - vol[i]);
    if (n < max) gui_snprintf(lines[n++], DV_LINELEN, "Noise    %s, rate %s   volume %2d of 15", (noise & 4) ? "white" : "periodic",
                              (noise & 3) == 3 ? "tone 3" : (noise & 3) == 0 ? "high" : (noise & 3) == 1 ? "medium" : "low", 15 - vol[3]);
    return n;
  }

  if (chip == 0)
  {
    gui_snprintf(lines[n++], DV_LINELEN, "This game has no FM chip.");
    return n;
  }
  if (chip == 2)
  {
    gui_snprintf(lines[n++], DV_LINELEN, "FM unit (YM2413), registers 00 to 3F");
    for (i = 0; i < 0x40 && n < max; i += 8)
      gui_snprintf(lines[n++], DV_LINELEN, "%02X:  %02X %02X %02X %02X %02X %02X %02X %02X", i, ym[0][i], ym[0][i + 1], ym[0][i + 2], ym[0][i + 3],
                   ym[0][i + 4], ym[0][i + 5], ym[0][i + 6], ym[0][i + 7]);
    return n;
  }

  gui_snprintf(lines[n++], DV_LINELEN, "FM (YM2612 / YM3438)    DAC %s,  LFO %s %d,  Channel 3 mode %d,  Timer A %03X, Timer B %02X",
               (ym[0][0x2B] & 0x80) ? "on" : "off", (ym[0][0x22] & 8) ? "on" : "off", ym[0][0x22] & 7, (ym[0][0x27] >> 6) & 3,
               (unsigned)(((ym[0][0x24] << 2) | (ym[0][0x25] & 3)) & 0x3FF), ym[0][0x26]);
  for (c = 0; c < 6 && n + 5 <= max; c++)
  {
    int port = c / 3, cc = c % 3, fn, blk, alg, fb, pan, ams, pms, on;
    static const int opoff[4] = { 0, 8, 4, 12 };         /* operator 1, 2, 3, 4: where their registers sit */

    fn = ym[port][0xA0 + cc] | ((ym[port][0xA4 + cc] & 7) << 8);
    blk = (ym[port][0xA4 + cc] >> 3) & 7;
    alg = ym[port][0xB0 + cc] & 7;
    fb = (ym[port][0xB0 + cc] >> 3) & 7;
    pan = ym[port][0xB4 + cc] >> 6;
    ams = (ym[port][0xB4 + cc] >> 4) & 3;
    pms = ym[port][0xB4 + cc] & 7;
    on = keyon[c < 3 ? c : c + 1];
    gui_snprintf(lines[n++], DV_LINELEN, "Channel %d  keys %c%c%c%c   F-number %03X  block %d  %6d Hz   algorithm %d  feedback %d  pan %s  AMS %d  PMS %d",
                 c + 1, (on & 1) ? '1' : '-', (on & 2) ? '2' : '-', (on & 4) ? '3' : '-', (on & 8) ? '4' : '-', fn, blk,
                 (int)(fn * (dv_pal() ? 7600489.0 : 7670454.0) / 144.0 / (double)(1 << (21 - blk))), alg, fb,
                 pan == 3 ? "L+R" : pan == 2 ? "L" : pan == 1 ? "R" : "off", ams, pms);
    for (o = 0; o < 4; o++)
    {
      int off = cc + opoff[o];

      gui_snprintf(lines[n++], DV_LINELEN, "    Op%d  level %3d  multiple %2d  detune %d  attack %2d  decay %2d  sustain rate %2d  sustain level %2d  release %2d",
                   o + 1, ym[port][0x40 + off] & 0x7F, ym[port][0x30 + off] & 15, (ym[port][0x30 + off] >> 4) & 7, ym[port][0x50 + off] & 31,
                   ym[port][0x60 + off] & 31, ym[port][0x70 + off] & 31, ym[port][0x80 + off] >> 4, ym[port][0x80 + off] & 15);
    }
  }
  return n;
}

/* One FM channel with the envelope of each of its four operators (a sketch: the heights and the widths follow the rates, not exact times). */
static void snd_envelope(HDC dc, const RECT *a, int ar, int dr, int sr, int sl, int rr, int tl, COLORREF fg, COLORREF dim)
{
  int w = a->right - a->left, h = a->bottom - a->top;
  double xa, xd, xs, xr, tot, y_peak, y_sus, x = 0;
  HPEN pen = CreatePen(PS_SOLID, 2, fg), grid = CreatePen(PS_DOT, 1, dim), old;
  POINT pt[8];
  int n = 0;

  if (w < 8 || h < 8) return;
  y_peak = tl / 127.0;                                         /* 0 = full level (top), 1 = silent (bottom) */
  y_sus = y_peak + (1.0 - y_peak) * (sl == 15 ? 1.0 : sl / 15.0);
  xa = ar == 0 ? 0 : (32 - ar) * 4.0;
  xd = dr == 0 ? 0 : (32 - dr) * 6.0;
  xs = sr == 0 ? 60 : (32 - sr) * 8.0;
  xr = (32 - (2 * rr + 1)) * 6.0;
  tot = xa + xd + xs + xr + 20;
  #define PX(v) (a->left + (int)((v) / tot * w))
  #define PY(v) (a->top + (int)((v) * (h - 2)) + 1)
  old = (HPEN)SelectObject(dc, grid);
  MoveToEx(dc, a->left, PY(1.0), NULL); LineTo(dc, a->right, PY(1.0));
  MoveToEx(dc, a->left, PY(0.0), NULL); LineTo(dc, a->right, PY(0.0));
  SelectObject(dc, pen);
  if (ar == 0) { pt[n].x = PX(0); pt[n].y = PY(1.0); n++; pt[n].x = PX(tot); pt[n].y = PY(1.0); n++; }           /* no attack: it never rises */
  else
  {
    pt[n].x = PX(0); pt[n].y = PY(1.0); n++;
    x += xa; pt[n].x = PX(x); pt[n].y = PY(y_peak); n++;
    x += xd; pt[n].x = PX(x); pt[n].y = PY(dr == 0 ? y_peak : y_sus); n++;
    if (sr == 0) { x += xs; pt[n].x = PX(x); pt[n].y = PY(dr == 0 ? y_peak : y_sus); n++; }
    else { x += xs; pt[n].x = PX(x); pt[n].y = PY(1.0); n++; }
    {
      double ykey = (sr == 0) ? (dr == 0 ? y_peak : y_sus) : 1.0;

      pt[n].x = PX(x); pt[n].y = PY(ykey); n++;                                                                  /* the key goes off here, and the release starts */
      x += xr; pt[n].x = PX(x); pt[n].y = PY(1.0); n++;
    }
  }
  Polyline(dc, pt, n);
  SelectObject(dc, old);
  DeleteObject(pen);
  DeleteObject(grid);
  #undef PX
  #undef PY
}

static void snd_custom(HDC dc, const RECT *area, int view, int opt)
{
  static unsigned char ym[2][256], keyon[8], vol[4], noise;
  static unsigned tone[3];
  COLORREF bg, fg, strip, dim;
  char t[200];
  int chip, c = opt, port, cc, o, y, pw, ph, i, dlh = dv_lh;
  static const int opoff[4] = { 0, 8, 4, 12 };

  (void)view;
  dv_colors(&bg, &fg, &strip);
  dim = RGB((GetRValue(bg) + GetRValue(fg)) / 2, (GetGValue(bg) + GetGValue(fg)) / 2, (GetBValue(bg) + GetBValue(fg)) / 2);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, fg);
  if (pico_active())
  {
    if (!pico_sound_state(ym, keyon, tone, vol, &noise)) { TextOutA(dc, area->left + S(8), area->top + S(8), "The 32X library has no sound chip view: use the Plugins folder of the newest zip.", 79); return; }
    chip = 1;
  }
  else chip = vgm_sound_snapshot(ym, keyon, tone, vol, &noise);
  if (chip != 1) { lstrcpyA(t, "This game has no YM2612 / YM3438 FM chip."); TextOutA(dc, area->left + S(8), area->top + S(8), t, lstrlenA(t)); return; }

  port = c / 3; cc = c % 3;
  {
    int fn = ym[port][0xA0 + cc] | ((ym[port][0xA4 + cc] & 7) << 8), blk = (ym[port][0xA4 + cc] >> 3) & 7, on = keyon[c < 3 ? c : c + 1];
    int alg = ym[port][0xB0 + cc] & 7, fb = (ym[port][0xB0 + cc] >> 3) & 7, pan = ym[port][0xB4 + cc] >> 6;

    y = area->top + S(4);
    gui_snprintf(t, sizeof(t), "Channel %d   keys %c%c%c%c   F-number %03X  block %d  %d Hz   algorithm %d  feedback %d  pan %s  AMS %d  PMS %d", c + 1,
                 (on & 1) ? '1' : '-', (on & 2) ? '2' : '-', (on & 4) ? '3' : '-', (on & 8) ? '4' : '-', fn, blk,
                 (int)(fn * (dv_pal() ? 7600489.0 : 7670454.0) / 144.0 / (double)(1 << (21 - blk))), alg, fb, pan == 3 ? "L+R" : pan == 2 ? "L" : pan == 1 ? "R" : "off",
                 (ym[port][0xB4 + cc] >> 4) & 3, ym[port][0xB4 + cc] & 7);
    TextOutA(dc, area->left + S(8), y, t, lstrlenA(t));
    y += dlh;
    gui_snprintf(t, sizeof(t), "Chip: DAC %s, LFO %s %d, Timer A %s %d, Timer B %s %d", (ym[0][0x2B] & 0x80) ? "on" : "off", (ym[0][0x22] & 8) ? "on" : "off", ym[0][0x22] & 7,
                 (ym[0][0x27] & 4) ? "(interrupt on)" : "(interrupt off)", ((ym[0][0x24] << 2) | (ym[0][0x25] & 3)) & 0x3FF, (ym[0][0x27] & 8) ? "(interrupt on)" : "(interrupt off)", ym[0][0x26]);
    TextOutA(dc, area->left + S(8), y, t, lstrlenA(t));
    y += dlh;
    if (c == 2 && ((ym[0][0x27] >> 6) & 3))                                      /* channel 3 with a frequency of its own for each operator */
    {
      gui_snprintf(t, sizeof(t), "Channel 3 special mode: operator frequencies (F-number, block): op1 %03X,%d  op2 %03X,%d  op3 %03X,%d  op4 %03X,%d",
                   ym[0][0xA9] | ((ym[0][0xAD] & 7) << 8), (ym[0][0xAD] >> 3) & 7, ym[0][0xAA] | ((ym[0][0xAE] & 7) << 8), (ym[0][0xAE] >> 3) & 7,
                   ym[0][0xA8] | ((ym[0][0xAC] & 7) << 8), (ym[0][0xAC] >> 3) & 7, ym[0][0xA2] | ((ym[0][0xA6] & 7) << 8), (ym[0][0xA6] >> 3) & 7);
      TextOutA(dc, area->left + S(8), y, t, lstrlenA(t));
      y += dlh;
    }
  }
  y += S(4);
  pw = (area->right - area->left - S(24)) / 2;
  ph = (area->bottom - y - S(12)) / 2;
  if (pw < S(120) || ph < dlh * 3) return;
  for (o = 0; o < 4; o++)
  {
    int off = cc + opoff[o], px = area->left + S(8) + (o & 1) * (pw + S(8)), py = y + (o >> 1) * (ph + S(4));
    int dt = (ym[port][0x30 + off] >> 4) & 7, mul = ym[port][0x30 + off] & 15, tl = ym[port][0x40 + off] & 0x7F, ks = ym[port][0x50 + off] >> 6, ar = ym[port][0x50 + off] & 31;
    int am = ym[port][0x60 + off] >> 7, dr = ym[port][0x60 + off] & 31, sr = ym[port][0x70 + off] & 31, sl = ym[port][0x80 + off] >> 4, rr = ym[port][0x80 + off] & 15, ssg = ym[port][0x90 + off] & 15;
    RECT g;

    gui_snprintf(t, sizeof(t), "Operator %d:  level %d  multiple %d  detune %d  key scale %d  AM %s  SSG-EG %X", o + 1, tl, mul, dt, ks, am ? "on" : "off", ssg);
    TextOutA(dc, px, py, t, lstrlenA(t));
    gui_snprintf(t, sizeof(t), "attack %d  decay %d  sustain rate %d  sustain level %d  release %d", ar, dr, sr, sl, rr);
    TextOutA(dc, px, py + dlh, t, lstrlenA(t));
    g.left = px; g.right = px + pw; g.top = py + 2 * dlh + S(2); g.bottom = py + ph;
    SetTextColor(dc, dim);
    FrameRect(dc, &g, (HBRUSH)GetStockObject(GRAY_BRUSH));
    SetTextColor(dc, fg);
    g.left += S(2); g.right -= S(2); g.top += S(2); g.bottom -= S(2);
    snd_envelope(dc, &g, ar, dr, sr, sl, rr, tl, (keyon[c < 3 ? c : c + 1] >> o) & 1 ? RGB(0, 190, 80) : fg, dim);
  }
  (void)i;
}

static const char *const snd_chan_opts[6] = { "Channel 1", "Channel 2", "Channel 3", "Channel 4", "Channel 5", "Channel 6" };

static const dv_view snd_views[4] = {
  { "FM Chip", 0, NULL, 0, 1 },
  { "PSG", 0, NULL, 0, 1 },
  { "PWM (32X)", 0, NULL, 0, 1 },
  { "FM Channel", 0, snd_chan_opts, 6, 1, NULL, 0, 1 }
};

static const dv_def snd_def = { "Sound Viewer", snd_views, 4, snd_text, NULL, NULL, snd_custom };

/****************************************************************************
 * What the processors hold
 ****************************************************************************/

/* The registers of a processor: 0 = the 68000 (D0-D7, A0-A7, PC, SR), 1 = the Z80 (PC, SP, AF, BC, DE, HL, IX, IY, AF', BC', DE', HL', R, I, IM, IFF1, IFF2,
   halted), 2 and 3 = the SH-2s of a 32X game (R0-R15, PC, PR, SR, GBR, VBR, MACH, MACL). Returns how many values, or -1 when the game has none. */
int dbg_cpu_regs(int which, unsigned *v)
{
  int i;

  if (pico_active())
  {
    int n = pico_regs(which, v, 24);

    if (which == 1 && n == 18)          /* the library's Z80 keeps the flags in the high byte of its AF */
    {
      v[2] = ((v[2] & 0xFF) << 8) | ((v[2] >> 8) & 0xFF);
      v[8] = ((v[8] & 0xFF) << 8) | ((v[8] >> 8) & 0xFF);
    }
    return n;
  }
  if (which == 0)
  {
    if ((system_hw & SYSTEM_PBC) != SYSTEM_MD && system_hw != SYSTEM_MCD) return -1;
    for (i = 0; i < 16; i++) v[i] = m68k_get_reg((m68k_register_t)(M68K_REG_D0 + i));
    v[16] = m68k_get_reg(M68K_REG_PC);
    v[17] = m68k_get_reg(M68K_REG_SR);
    return 18;
  }
  if (which == 1)
  {
    v[0] = Z80.pc.w.l; v[1] = Z80.sp.w.l; v[2] = Z80.af.w.l; v[3] = Z80.bc.w.l; v[4] = Z80.de.w.l; v[5] = Z80.hl.w.l;
    v[6] = Z80.ix.w.l; v[7] = Z80.iy.w.l; v[8] = Z80.af2.w.l; v[9] = Z80.bc2.w.l; v[10] = Z80.de2.w.l; v[11] = Z80.hl2.w.l;
    v[12] = (unsigned)((Z80.r & 127) | (Z80.r2 & 128)); v[13] = Z80.i; v[14] = Z80.im; v[15] = Z80.iff1; v[16] = Z80.iff2; v[17] = Z80.halt;
    return 18;
  }
  return -1;
}

static int cpu_text(int view, int opt, char lines[][DV_LINELEN], int max)
{
  unsigned v[24];
  int n = 0, i, count;

  (void)opt; (void)max;
  count = dbg_cpu_regs(view, v);
  if (count < 0)
  {
    gui_snprintf(lines[n++], DV_LINELEN, view >= 2 ? "Only a 32X game has SH-2 processors." :
                 pico_active() ? "The 32X library has no processor view: use the Plugins folder of the newest zip." :
                 view == 0 ? "This game has no 68000 processor." : "No processor.");
    return n;
  }

  if (view == 0)
  {
    unsigned sr = v[17];

    for (i = 0; i < 8; i++) gui_snprintf(lines[n++], DV_LINELEN, "D%d  %08X      A%d  %08X", i, v[i], i, v[8 + i]);
    gui_snprintf(lines[n++], DV_LINELEN, " ");
    gui_snprintf(lines[n++], DV_LINELEN, "PC  %08X      SR  %04X   %c%c%c%c%c   interrupt level %u   %s mode", v[16], sr, (sr & 16) ? 'X' : '-', (sr & 8) ? 'N' : '-',
                 (sr & 4) ? 'Z' : '-', (sr & 2) ? 'V' : '-', (sr & 1) ? 'C' : '-', (sr >> 8) & 7, (sr & 0x2000) ? "supervisor" : "user");
    if (!pico_active())
      gui_snprintf(lines[n++], DV_LINELEN, "USP %08X      ISP %08X", m68k_get_reg(M68K_REG_USP), m68k_get_reg(M68K_REG_ISP));
  }
  else if (view == 1)
  {
    unsigned f = v[2] & 0xFF;

    gui_snprintf(lines[n++], DV_LINELEN, "AF  %04X   BC  %04X   DE  %04X   HL  %04X", v[2], v[3], v[4], v[5]);
    gui_snprintf(lines[n++], DV_LINELEN, "AF' %04X   BC' %04X   DE' %04X   HL' %04X", v[8], v[9], v[10], v[11]);
    gui_snprintf(lines[n++], DV_LINELEN, "IX  %04X   IY  %04X   SP  %04X   PC  %04X", v[6], v[7], v[1], v[0]);
    gui_snprintf(lines[n++], DV_LINELEN, " ");
    gui_snprintf(lines[n++], DV_LINELEN, "Flags %c%c%c%c%c%c%c%c   I %02X   R %02X   interrupt mode %u   IFF1 %u  IFF2 %u   %s",
                 (f & 0x80) ? 'S' : '-', (f & 0x40) ? 'Z' : '-', (f & 0x20) ? 'Y' : '-', (f & 0x10) ? 'H' : '-', (f & 8) ? 'X' : '-',
                 (f & 4) ? 'P' : '-', (f & 2) ? 'N' : '-', (f & 1) ? 'C' : '-', v[13], v[12], v[14], v[15], v[16], v[17] ? "halted" : "running");
  }
  else
  {
    unsigned sr = v[18];

    for (i = 0; i < 16; i += 2) gui_snprintf(lines[n++], DV_LINELEN, "R%-2d %08X      R%-2d %08X", i, v[i], i + 1, v[i + 1]);
    gui_snprintf(lines[n++], DV_LINELEN, " ");
    gui_snprintf(lines[n++], DV_LINELEN, "PC  %08X      PR  %08X", v[16], v[17]);
    gui_snprintf(lines[n++], DV_LINELEN, "SR  %08X      %c%c%c%c   interrupt mask %u", sr, (sr & 1) ? 'T' : '-', (sr & 2) ? 'S' : '-', (sr & 0x100) ? 'Q' : '-',
                 (sr & 0x200) ? 'M' : '-', (sr >> 4) & 15);
    gui_snprintf(lines[n++], DV_LINELEN, "GBR %08X      VBR %08X", v[19], v[20]);
    gui_snprintf(lines[n++], DV_LINELEN, "MACH %08X     MACL %08X", v[21], v[22]);
  }
  return n;
}

static const dv_view cpu_views[4] = {
  { "68000", 0, NULL, 0, 1 },
  { "Z80", 0, NULL, 0, 1 },
  { "Master SH-2 (32X)", 0, NULL, 0, 1 },
  { "Slave SH-2 (32X)", 0, NULL, 0, 1 }
};

static const dv_def cpu_def = { "CPU Viewer", cpu_views, 4, cpu_text, NULL, NULL };

/****************************************************************************
 * The window
 ****************************************************************************/

static dv_win dv_wins[3];
static const dv_def *const dv_defs[3] = { &vdp_def, &snd_def, &cpu_def };

static const dv_view *dv_cur(const dv_win *w)
{
  return &w->def->views[w->view];
}

static void dv_update_info(dv_win *w);
static void vdp_color_edit(HWND owner, int idx);

/* The lines or the picture the view shows now; 1 = different from what was drawn. */
static int dv_refresh(dv_win *w)
{
  const dv_view *v = dv_cur(w);
  int changed = 0;

  if (!w->lines) w->lines = (char (*)[DV_LINELEN])calloc(DV_MAXLINES, DV_LINELEN);
  if (!w->prev) w->prev = (char (*)[DV_LINELEN])calloc(DV_MAXLINES, DV_LINELEN);
  if (!w->px) w->px = (uint32 *)calloc(DV_MAXPIX, 4);
  if (!w->prevpx) w->prevpx = (uint32 *)calloc(DV_MAXPIX, 4);
  if (!w->lines || !w->prev || !w->px || !w->prevpx) return 0;

  if (v->custom)
  {
    w->nlines = 0; w->iw = w->ih = 0;
    dv_update_info(w);
    return 1;
  }
  if (v->image && w->def->image)
  {
    int iw = 0, ih = 0, ok;

    w->msg[0] = '\0';
    ok = w->def->image(w->view, w->opt, w->px, &iw, &ih, w->msg, sizeof(w->msg));
    if (!ok) { iw = ih = 0; }
    if (iw != w->pw || ih != w->ph || memcmp(w->px, w->prevpx, (size_t)iw * (size_t)ih * 4)) changed = 1;
    w->iw = iw; w->ih = ih;
    w->nlines = 0;
    if (!ok)
    {
      lstrcpynA(w->lines[0], w->msg, DV_LINELEN);
      w->nlines = 1;
      if (w->nprev != 1 || lstrcmpA(w->lines[0], w->prev[0])) changed = 1;
    }
    memcpy(w->prevpx, w->px, (size_t)iw * (size_t)ih * 4);
    w->pw = iw; w->ph = ih;
    w->nprev = w->nlines;
    if (w->nlines) memcpy(w->prev[0], w->lines[0], DV_LINELEN);
  }
  else
  {
    w->nlines = w->def->text(w->view, w->opt, w->lines, DV_MAXLINES);
    if (w->nlines != w->nprev || memcmp(w->lines, w->prev, (size_t)w->nlines * DV_LINELEN)) changed = 1;
    memcpy(w->prev, w->lines, (size_t)w->nlines * DV_LINELEN);
    w->nprev = w->nlines;
    w->iw = w->ih = 0;
  }
  dv_update_info(w);
  return changed;
}

static void dv_content_size(dv_win *w, int *cw, int *ch)
{
  const dv_view *v = dv_cur(w);

  if (v->custom) { *cw = 0; *ch = 0; return; }
  if (v->image && w->iw > 0)
  {
    *cw = w->iw * w->zoom[w->view];
    *ch = w->ih * w->zoom[w->view];
  }
  else
  {
    int i, widest = 0;

    for (i = 0; i < w->nlines; i++) { int l = lstrlenA(w->lines[i]); if (l > widest) widest = l; }
    *cw = widest * dv_cw + S(16);
    *ch = w->nlines * dv_lh + S(8);
  }
}

static void dv_scrollbars(dv_win *w)
{
  RECT rc;
  SCROLLINFO si;
  int cw, ch, vw, vh;

  GetClientRect(w->hwnd, &rc);
  vw = rc.right;
  vh = rc.bottom - dv_strip_h();
  if (vh < 1) vh = 1;
  dv_content_size(w, &cw, &ch);

  if (w->top > ch - vh) w->top = ch - vh;
  if (w->top < 0) w->top = 0;
  if (w->left > cw - vw) w->left = cw - vw;
  if (w->left < 0) w->left = 0;

  ZeroMemory(&si, sizeof(si));
  si.cbSize = sizeof(si);
  si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
  si.nMin = 0; si.nMax = ch > 0 ? ch - 1 : 0; si.nPage = (UINT)vh; si.nPos = w->top;
  SetScrollInfo(w->hwnd, SB_VERT, &si, TRUE);
  si.nMax = cw > 0 ? cw - 1 : 0; si.nPage = (UINT)vw; si.nPos = w->left;
  SetScrollInfo(w->hwnd, SB_HORZ, &si, TRUE);
}

static void dv_invalidate(dv_win *w)
{
  RECT rc;

  GetClientRect(w->hwnd, &rc);
  rc.top = dv_strip_h();
  InvalidateRect(w->hwnd, &rc, FALSE);
}

/* What a button of a view does (the names are the buttons' own). */
static const char *dv_btn_tip(const char *n)
{
  if (!lstrcmpA(n, "Dump Pal")) return "Save the 64 colors as the game wrote them to a file.";
  if (!lstrcmpA(n, "Load Pal")) return "Put the colors of a file into the palette (they change at once; the game may write them again).";
  if (!lstrcmpA(n, "YY-CHR Pal")) return "Save the palette as a .pal file for the YY-CHR tile editor.";
  if (!lstrcmpA(n, "Gray && Rainbow")) return "Fill the palette with grays and a rainbow, to see which tiles use which colors. The game may write its own colors again.";
  if (!lstrcmpA(n, "Dump VRAM")) return "Save the whole video memory to a file.";
  if (!lstrcmpA(n, "Load VRAM")) return "Put the bytes of a file into the video memory.";
  if (!lstrcmpA(n, "Dump")) return "Save the list of sprites as a text file.";
  if (!lstrcmpA(n, "Edit Register...")) return "Change the register picked in the list (or the line clicked) the way a game would.";
  return NULL;
}

/* What the choices in the strip do, for the view shown. */
static void dv_tips(dv_win *w, const dv_view *v)
{
  int c;
  const char *opt = NULL;

  if (!w->tt) return;
  dbg_tip(w->tt, w->hwnd, w->view_cb, "Pick what to look at.");
  dbg_tip(w->tt, w->hwnd, w->zoom_cb, "How big the picture is drawn (1x to 8x).");
  if (!lstrcmpA(v->name, "Tiles")) opt = "Which memory the tiles are read from, and which palette line colors them.";
  else if (!lstrcmpA(v->name, "Sprites")) opt = "The order of the list: along the sprite links (the order they are drawn in) or as the table holds them.";
  else if (!lstrcmpA(v->name, "Sprite Preview")) opt = "Which sprite to show.";
  else if (!lstrcmpA(v->name, "FM Channel")) opt = "Which of the FM chip's channels to show.";
  if (opt) dbg_tip(w->tt, w->hwnd, w->opt_cb, opt);
  for (c = 0; c < v->nbtns && c < 4; c++)
    if (dv_btn_tip(v->btns[c])) dbg_tip(w->tt, w->hwnd, w->btn[c], dv_btn_tip(v->btns[c]));
}

static void dv_layout(dv_win *w)
{
  RECT rc;
  int x = S(8), y, hh = S(24), c;
  HWND cs[3];
  const dv_view *v = dv_cur(w);

  GetClientRect(w->hwnd, &rc);
  y = (dv_strip_h() - hh) / 2;
  cs[0] = w->view_cb; cs[1] = w->opt_cb; cs[2] = w->zoom_cb;
  for (c = 0; c < 3; c++) SendMessage(cs[c], WM_SETFONT, (WPARAM)dv_ui, TRUE);

  SetWindowPos(w->view_cb, NULL, x, y, S(150), S(300), SWP_NOZORDER);
  x += S(150) + S(10);
  ShowWindow(w->opt_cb, v->nopts ? SW_SHOW : SW_HIDE);
  if (v->nopts) { SetWindowPos(w->opt_cb, NULL, x, y, S(110), S(300), SWP_NOZORDER); x += S(110) + S(10); }
  ShowWindow(w->zoom_cb, v->image ? SW_SHOW : SW_HIDE);
  if (v->image) { SetWindowPos(w->zoom_cb, NULL, x, y, S(70), S(300), SWP_NOZORDER); x += S(70) + S(14); }
  for (c = 0; c < 4; c++)
  {
    if (c < v->nbtns)
    {
      int bw = S(26) + (int)lstrlenA(tr(v->btns[c])) * S(6);

      SetWindowTextA(w->btn[c], v->btns[c]);
      SendMessage(w->btn[c], WM_SETFONT, (WPARAM)dv_ui, TRUE);
      SetWindowPos(w->btn[c], NULL, x, y, bw, hh, SWP_NOZORDER);
      ShowWindow(w->btn[c], SW_SHOW);
      x += bw + S(6);
    }
    else ShowWindow(w->btn[c], SW_HIDE);
  }
  dv_tips(w, v);
  w->info_x = x + S(8);
}

static void dv_select_view(dv_win *w, int view)
{
  const dv_view *v;
  int i;

  w->view = view;
  v = dv_cur(w);
  w->top = w->left = 0;
  w->sel_line = -1;
  w->hover_x = w->hover_y = w->lock_x = w->lock_y = -1;
  w->info[0] = '\0';
  w->opt = 0;
  w->pw = w->ph = 0;
  SendMessage(w->opt_cb, CB_RESETCONTENT, 0, 0);
  for (i = 0; i < v->nopts; i++) SendMessageA(w->opt_cb, CB_ADDSTRING, 0, (LPARAM)v->opts[i]);
  if (v->nopts) SendMessage(w->opt_cb, CB_SETCURSEL, 0, 0);
  SendMessage(w->zoom_cb, CB_SETCURSEL, (WPARAM)(w->zoom[view] - 1), 0);
  dv_layout(w);
  dv_refresh(w);
  dv_scrollbars(w);
  dv_invalidate(w);
}

/* The tile of a plane under a point of the window (the VDP Viewer's Plane A, Plane B and Window views only). */
static int dv_tile_at(dv_win *w, int x, int y, int *tx, int *ty)
{
  int z, ix, iy;

  if (w->def != &vdp_def || !(w->view == 1 || w->view == 2 || (w->view >= 4 && w->view <= 6)) || w->iw <= 0 || y < dv_strip_h()) return 0;
  z = w->zoom[w->view];
  ix = (x + w->left) / z;
  iy = (y - dv_strip_h() + w->top) / z;
  if (ix < 0 || iy < 0 || ix >= w->iw || iy >= w->ih) return 0;
  *tx = ix / (w->view == 1 ? 16 : 8);
  *ty = iy / (w->view == 1 ? 16 : 8);
  return 1;
}

static void dv_tile_info(dv_win *w, int tx, int ty, int locked, char *out, int len)
{
  unsigned addr, e;

  if (w->view == 1)                                   /* a color of the palette */
  {
    int idx = ty * 16 + tx;
    uint32 c = md_color(idx);

    gui_snprintf(out, len, "%sColor %d (line %d, entry %d): $%02X%02X, RGB (%d, %d, %d). Double-click to change it.", locked ? "Locked: " : "", idx, ty, tx,
                 dbgmem_get(MV_CRAM, idx * 2), dbgmem_get(MV_CRAM, idx * 2 + 1), (int)((c >> 16) & 255), (int)((c >> 8) & 255), (int)(c & 255));
    return;
  }
  if (w->view == 2)                                   /* a tile of the video memory */
  {
    unsigned t = (unsigned)(ty * 32 + tx);

    gui_snprintf(out, len, "%sTile %03X at address %04X (%s)", locked ? "Locked: " : "", t, t * 32, w->opt >= 4 ? "68000 RAM" : "VRAM");
    return;
  }
  addr = (vdp_plane_base + (unsigned)(ty * vdp_plane_tw + tx) * 2) & 0xFFFF;
  e = vram_w(addr);

  gui_snprintf(out, len, "%s%s tile (%d, %d) at %04X: entry %04X, tile %03X, palette %d%s%s%s", locked ? "Locked: " : "", w->view == 4 ? "Plane A" : w->view == 5 ? "Plane B" : "Window",
               tx, ty, addr, e, e & 0x7FF, (int)((e >> 13) & 3), (e & 0x8000) ? ", priority" : "", (e & 0x800) ? ", H flip" : "", (e & 0x1000) ? ", V flip" : "");
}

/* The note in the strip: the tile pointed at or locked, or what the picture is. */
static void dv_update_info(dv_win *w)
{
  char t[sizeof(w->info)];

  t[0] = '\0';
  if (w->def == &vdp_def && dv_cur(w)->image)
  {
    int pickable = w->view == 1 || w->view == 2 || (w->view >= 4 && w->view <= 6);

    if (pickable && w->iw > 0 && w->hover_x >= 0) dv_tile_info(w, w->hover_x, w->hover_y, 0, t, sizeof(t));
    else if (pickable && w->iw > 0 && w->lock_x >= 0) dv_tile_info(w, w->lock_x, w->lock_y, 1, t, sizeof(t));
    else if (w->iw > 0) lstrcpynA(t, vdp_note, sizeof(t));
  }
  if (lstrcmpA(t, w->info))
  {
    RECT rc;

    lstrcpynA(w->info, t, sizeof(w->info));
    GetClientRect(w->hwnd, &rc);
    rc.bottom = dv_strip_h();
    InvalidateRect(w->hwnd, &rc, FALSE);
  }
}

static void dv_paint(dv_win *w, HDC target, const RECT *client)
{
  COLORREF bg, fg, strip;
  int cw = client->right, ch = client->bottom, top = dv_strip_h();
  HDC dc = CreateCompatibleDC(target);
  HBITMAP bmp = CreateCompatibleBitmap(target, cw, ch), oldbmp;
  HFONT oldfont;
  HBRUSH br;
  RECT r;
  const dv_view *v = dv_cur(w);

  dv_colors(&bg, &fg, &strip);
  oldbmp = (HBITMAP)SelectObject(dc, bmp);
  oldfont = (HFONT)SelectObject(dc, dv_mono);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, fg);

  br = CreateSolidBrush(bg);
  r = *client; r.top = top;
  FillRect(dc, &r, br);
  DeleteObject(br);
  br = CreateSolidBrush(strip);
  r.top = 0; r.bottom = top;
  FillRect(dc, &r, br);
  DeleteObject(br);

  IntersectClipRect(dc, 0, top, cw, ch);
  if (v->custom && w->def->custom)
  {
    RECT ca;

    ca.left = 0; ca.top = top; ca.right = cw; ca.bottom = ch;
    w->def->custom(dc, &ca, w->view, w->opt);
  }
  else if (v->image && w->iw > 0 && w->px)
  {
    BITMAPINFO bi;
    int z = w->zoom[w->view];

    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w->iw;
    bi.bmiHeader.biHeight = -w->ih;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, -w->left, top - w->top, w->iw * z, w->ih * z, 0, 0, w->iw, w->ih, w->px, &bi, DIB_RGB_COLORS, SRCCOPY);
    if (w->def == &vdp_def && (w->view == 1 || w->view == 2 || (w->view >= 4 && w->view <= 6)))
    {
      int pass, cell = w->view == 1 ? 16 : 8;

      SelectObject(dc, GetStockObject(NULL_BRUSH));
      for (pass = 0; pass < 2; pass++)
      {
        int tx = pass ? w->lock_x : w->hover_x, ty = pass ? w->lock_y : w->hover_y;
        HPEN pen, old;

        if (tx < 0) continue;
        pen = pass ? CreatePen(PS_SOLID, 2, theme_accent_bg()) : CreatePen(PS_DOT, 1, RGB(255, 255, 255));
        old = (HPEN)SelectObject(dc, pen);
        if (!pass) { HPEN dark = CreatePen(PS_SOLID, 1, RGB(0, 0, 0)); HPEN o2 = (HPEN)SelectObject(dc, dark);
                     Rectangle(dc, -w->left + tx * cell * z, top - w->top + ty * cell * z, -w->left + (tx + 1) * cell * z + 1, top - w->top + (ty + 1) * cell * z + 1);
                     SelectObject(dc, o2); DeleteObject(dark); }
        Rectangle(dc, -w->left + tx * cell * z, top - w->top + ty * cell * z, -w->left + (tx + 1) * cell * z + 1, top - w->top + (ty + 1) * cell * z + 1);
        SelectObject(dc, old);
        DeleteObject(pen);
      }
    }
  }
  else
  {
    int i, first = w->top / dv_lh;

    for (i = first; i < w->nlines; i++)
    {
      int y = top + S(4) + i * dv_lh - w->top;

      if (y > ch) break;
      if (i == w->sel_line)
      {
        RECT sr;
        HBRUSH sb = CreateSolidBrush(theme_accent_bg());

        sr.left = 0; sr.right = cw; sr.top = y - S(1); sr.bottom = y + dv_lh - S(1);
        FillRect(dc, &sr, sb);
        DeleteObject(sb);
        SetTextColor(dc, RGB(255, 255, 255));
        TextOutA(dc, S(8) - w->left, y, w->lines[i], lstrlenA(w->lines[i]));
        SetTextColor(dc, fg);
        continue;
      }
      TextOutA(dc, S(8) - w->left, y, w->lines[i], lstrlenA(w->lines[i]));
    }
  }
  SelectClipRgn(dc, NULL);

  br = CreateSolidBrush(RGB((GetRValue(bg) + GetRValue(fg)) / 2, (GetGValue(bg) + GetGValue(fg)) / 2, (GetBValue(bg) + GetBValue(fg)) / 2));
  r.left = 0; r.right = cw; r.top = top - 1; r.bottom = top;
  FillRect(dc, &r, br);
  DeleteObject(br);

  if (w->info[0] && w->info_x < cw)
  {
    RECT ir;

    int ptx = w->hover_x >= 0 ? w->hover_x : w->lock_x, pty = w->hover_x >= 0 ? w->hover_y : w->lock_y;

    ir.left = w->info_x; ir.top = 0; ir.right = cw - S(8); ir.bottom = top;
    if (w->def == &vdp_def && (w->view == 2 || (w->view >= 4 && w->view <= 6)) && ptx >= 0 && w->px && ptx * 8 + 8 <= w->iw && pty * 8 + 8 <= w->ih)
    {
      int pr, pc, ps = (top - S(6)) / 8;                   /* a zoomed copy of the tile pointed at, at the end of the strip */
      int px0 = cw - S(8) - ps * 8, py0 = S(3);

      for (pr = 0; pr < 8; pr++)
        for (pc = 0; pc < 8; pc++)
        {
          uint32 c = w->px[(pty * 8 + pr) * w->iw + ptx * 8 + pc];
          HBRUSH pb = CreateSolidBrush(RGB((c >> 16) & 255, (c >> 8) & 255, c & 255));
          RECT cell;

          cell.left = px0 + pc * ps; cell.top = py0 + pr * ps; cell.right = cell.left + ps; cell.bottom = cell.top + ps;
          FillRect(dc, &cell, pb);
          DeleteObject(pb);
        }
      ir.right = px0 - S(8);
    }
    SelectObject(dc, dv_ui);
    SetTextColor(dc, fg);
    DrawTextA(dc, w->info, -1, &ir, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
  }

  BitBlt(target, 0, 0, cw, ch, dc, 0, 0, SRCCOPY);
  SelectObject(dc, oldfont);
  SelectObject(dc, oldbmp);
  DeleteObject(bmp);
  DeleteDC(dc);
}

static LRESULT CALLBACK dv_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
  dv_win *w = (dv_win *)GetWindowLongPtr(h, GWLP_USERDATA);
  int i;

  if (m == WM_NCCREATE)
  {
    w = (dv_win *)((CREATESTRUCTA *)lp)->lpCreateParams;
    SetWindowLongPtr(h, GWLP_USERDATA, (LONG_PTR)w);
    w->hwnd = h;
  }
  if (!w) return DefWindowProc(h, m, wp, lp);

  switch (m)
  {
    case WM_CREATE:
      dv_fonts();
      w->look = dbg_look();
      w->view_cb = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 10, 10, h,
                                   (HMENU)(INT_PTR)IDC_DV_VIEW, g_inst, NULL);
      w->opt_cb = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 10, 10, h,
                                  (HMENU)(INT_PTR)IDC_DV_OPT, g_inst, NULL);
      w->zoom_cb = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 10, 10, h,
                                   (HMENU)(INT_PTR)IDC_DV_ZOOM, g_inst, NULL);
      for (i = 0; i < 4; i++)
        w->btn[i] = CreateWindowExA(0, "BUTTON", "", WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10, h, (HMENU)(INT_PTR)(IDC_DV_BTN + i), g_inst, NULL);
      w->tt = dbg_tips_new(h);
      for (i = 0; i < w->def->nviews; i++) SendMessageA(w->view_cb, CB_ADDSTRING, 0, (LPARAM)w->def->views[i].name);
      for (i = 1; i <= 8; i++) { char t[8]; wsprintfA(t, "%dx", i); SendMessageA(w->zoom_cb, CB_ADDSTRING, 0, (LPARAM)t); }
      for (i = 0; i < 16; i++) w->zoom[i] = i < w->def->nviews ? w->def->views[i].zoom : 1;
      SendMessage(w->view_cb, CB_SETCURSEL, 0, 0);
      theme_apply_to_window(h);
      dv_select_view(w, 0);
      SetTimer(h, DV_TIMER, DV_TICK_MS, NULL);
      return 0;

    case WM_SIZE:
      dv_scrollbars(w);
      InvalidateRect(h, NULL, FALSE);
      return 0;

    case WM_GETMINMAXINFO:
    {
      MINMAXINFO *mi = (MINMAXINFO *)lp;

      mi->ptMinTrackSize.x = S(480);
      mi->ptMinTrackSize.y = S(220);
      return 0;
    }

    case WM_DPICHANGED:
    {
      const RECT *r = (const RECT *)lp;

      dv_fonts();
      dv_layout(w);
      dv_scrollbars(w);
      SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }

    case WM_ERASEBKGND:
      return 1;

    case WM_PAINT:
    {
      PAINTSTRUCT ps;
      RECT rc;
      HDC dc = BeginPaint(h, &ps);

      GetClientRect(h, &rc);
      dv_paint(w, dc, &rc);
      EndPaint(h, &ps);
      return 0;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
    {
      HBRUSH b = theme_ctlcolor_ctl((HDC)wp, (HWND)lp);

      if (b) return (LRESULT)b;
      break;
    }

    case WM_TIMER:
      if (wp == DV_TIMER && IsWindowVisible(h))
      {
        if (dbg_look() != w->look)           /* the text size, the style or the scale changed (the Large interface was switched on or off ...) */
        {
          w->look = dbg_look();
          dv_fonts();
          dv_layout(w);
          dv_scrollbars(w);
          InvalidateRect(h, NULL, FALSE);
        }
        if (emu_running && dv_refresh(w)) { dv_scrollbars(w); dv_invalidate(w); }
      }
      return 0;

    case WM_VSCROLL:
    case WM_HSCROLL:
    {
      SCROLLINFO si;
      int bar = (m == WM_VSCROLL) ? SB_VERT : SB_HORZ, pos, line = (m == WM_VSCROLL && !dv_cur(w)->image) ? dv_lh : S(16);

      ZeroMemory(&si, sizeof(si));
      si.cbSize = sizeof(si);
      si.fMask = SIF_ALL;
      GetScrollInfo(h, bar, &si);
      pos = si.nPos;
      switch (LOWORD(wp))
      {
        case SB_LINEUP:        pos -= line; break;
        case SB_LINEDOWN:      pos += line; break;
        case SB_PAGEUP:        pos -= (int)si.nPage; break;
        case SB_PAGEDOWN:      pos += (int)si.nPage; break;
        case SB_TOP:           pos = 0; break;
        case SB_BOTTOM:        pos = si.nMax; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = si.nTrackPos; break;
      }
      if (bar == SB_VERT) w->top = pos; else w->left = pos;
      dv_scrollbars(w);
      dv_invalidate(w);
      return 0;
    }

    case WM_MOUSEWHEEL:
      if ((GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) && dv_cur(w)->image)           /* Ctrl + wheel: zoom */
      {
        int z = w->zoom[w->view] + (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1);

        if (z < 1) z = 1;
        if (z > 8) z = 8;
        w->zoom[w->view] = z;
        SendMessage(w->zoom_cb, CB_SETCURSEL, (WPARAM)(z - 1), 0);
        dv_scrollbars(w);
        dv_invalidate(w);
        return 0;
      }
      w->top -= (GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA) * (dv_cur(w)->image ? S(48) : 3 * dv_lh);
      dv_scrollbars(w);
      dv_invalidate(w);
      return 0;

    case WM_MOUSEMOVE:
    {
      int x = (short)LOWORD(lp), y = (short)HIWORD(lp), tx = -1, ty = -1;

      if (w->panning)
      {
        w->left -= x - w->pan_x; w->top -= y - w->pan_y;
        w->pan_x = x; w->pan_y = y;
        dv_scrollbars(w);
        dv_invalidate(w);
        return 0;
      }
      if (!dv_tile_at(w, x, y, &tx, &ty)) { tx = ty = -1; }
      if (tx != w->hover_x || ty != w->hover_y)
      {
        TRACKMOUSEEVENT tme;

        w->hover_x = tx; w->hover_y = ty;
        ZeroMemory(&tme, sizeof(tme));
        tme.cbSize = sizeof(tme); tme.dwFlags = TME_LEAVE; tme.hwndTrack = h;
        TrackMouseEvent(&tme);
        dv_update_info(w);
        dv_invalidate(w);
      }
      return 0;
    }

    case WM_MOUSELEAVE:
      if (w->hover_x >= 0) { w->hover_x = w->hover_y = -1; dv_update_info(w); dv_invalidate(w); }
      return 0;

    case WM_MBUTTONDOWN:
      if (dv_cur(w)->image) { SetFocus(h); SetCapture(h); w->panning = 1; w->pan_x = (short)LOWORD(lp); w->pan_y = (short)HIWORD(lp); }
      return 0;

    case WM_MBUTTONUP:
      if (w->panning) { w->panning = 0; ReleaseCapture(); }
      return 0;

    case WM_LBUTTONDOWN:
    {
      int x = (short)LOWORD(lp), y = (short)HIWORD(lp), tx, ty;

      SetFocus(h);
      if (dv_tile_at(w, x, y, &tx, &ty))                   /* a click locks the tile (the same one again lets it go) */
      {
        if (w->lock_x == tx && w->lock_y == ty) w->lock_x = w->lock_y = -1; else { w->lock_x = tx; w->lock_y = ty; }
        dv_update_info(w);
        dv_invalidate(w);
      }
      else if (!dv_cur(w)->image && y >= dv_strip_h())       /* a line of a text view is picked (the Registers view: the register to edit) */
      {
        int line = (y - dv_strip_h() - S(4) + w->top) / dv_lh;

        w->sel_line = (line >= 0 && line < w->nlines) ? line : -1;
        if (w->def == &vdp_def) dv_sel_line = w->sel_line;
        dv_invalidate(w);
      }
      return 0;
    }

    case WM_LBUTTONDBLCLK:
    {
      int tx, ty;

      if (w->def == &vdp_def && w->view == 0 && w->sel_line >= 0 && w->sel_line < 24) vdp_reg_edit(h, w->sel_line);
      else if (w->def == &vdp_def && w->view == 1 && dv_tile_at(w, (short)LOWORD(lp), (short)HIWORD(lp), &tx, &ty)) vdp_color_edit(h, ty * 16 + tx);
      return 0;
    }

    case WM_COMMAND:
      if (LOWORD(wp) >= IDC_DV_BTN && LOWORD(wp) < IDC_DV_BTN + 4 && HIWORD(wp) == BN_CLICKED)
      {
        if (w->def->action) w->def->action(h, w->view, LOWORD(wp) - IDC_DV_BTN, w->lines, w->nlines);
        SetFocus(h);
        return 0;
      }
      if (HIWORD(wp) == CBN_SELCHANGE)
      {
        int k = (int)SendMessage((HWND)lp, CB_GETCURSEL, 0, 0);

        if (k < 0) return 0;
        if (LOWORD(wp) == IDC_DV_VIEW) dv_select_view(w, k);
        else if (LOWORD(wp) == IDC_DV_OPT) { w->opt = k; dv_refresh(w); dv_invalidate(w); }
        else if (LOWORD(wp) == IDC_DV_ZOOM) { w->zoom[w->view] = k + 1; dv_scrollbars(w); dv_invalidate(w); }
        SetFocus(h);
      }
      return 0;

    case WM_KEYDOWN:
      if (wp == VK_ESCAPE) gui_debug_close(h);
      return 0;

    case WM_CLOSE:
      DestroyWindow(h);
      return 0;

    case WM_DESTROY:
      KillTimer(h, DV_TIMER);
      free(w->lines); free(w->prev); free(w->px); free(w->prevpx);
      w->lines = NULL; w->prev = NULL; w->px = NULL; w->prevpx = NULL;
      w->nlines = w->nprev = 0;
      w->hwnd = NULL;
      return 0;
  }

  return DefWindowProc(h, m, wp, lp);
}

/* Changes a color of the palette with the color picker. */
static void vdp_color_edit(HWND owner, int idx)
{
  static COLORREF custom[16];
  CHOOSECOLORA cc;
  uint32 c = md_color(idx);
  unsigned bus;

  if (netplay_active()) { gui_notify("The palette cannot be changed during netplay"); return; }
  ZeroMemory(&cc, sizeof(cc));
  cc.lStructSize = sizeof(cc);
  cc.hwndOwner = owner;
  cc.rgbResult = RGB((c >> 16) & 255, (c >> 8) & 255, c & 255);
  cc.lpCustColors = custom;
  cc.Flags = CC_RGBINIT | CC_FULLOPEN;
  if (!ChooseColorA(&cc)) return;
  bus = (unsigned)(((GetBValue(cc.rgbResult) >> 5) & 7) << 9) | (unsigned)(((GetGValue(cc.rgbResult) >> 5) & 7) << 5) | (unsigned)(((GetRValue(cc.rgbResult) >> 5) & 7) << 1);
  dbgmem_put(MV_CRAM, idx * 2, (int)(bus >> 8));
  dbgmem_put(MV_CRAM, idx * 2 + 1, (int)(bus & 0xFF));
  gui_notify("Color %d changed (the video chip has 3 bits for each of red, green and blue)", idx);
}

static void vdp_init_names(void)
{
  int i;

  for (i = 0; i < 80; i++) { gui_snprintf(vdp_sprite_names[i], sizeof(vdp_sprite_names[i]), "Sprite %d", i); vdp_sprite_opts[i] = vdp_sprite_names[i]; }
}

static void dv_open(int which, HWND parent)
{
  static int registered;
  dv_win *w = &dv_wins[which];

  if (w->hwnd)
  {
    HWND top = GetAncestor(w->hwnd, GA_ROOT);

    if (IsIconic(top)) ShowWindow(top, SW_RESTORE);
    SetForegroundWindow(top);
    return;
  }
  if (!registered)
  {
    WNDCLASSA wc;

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = dv_proc;
    wc.style = CS_DBLCLKS;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = DV_CLASS;
    if (!RegisterClassA(&wc)) return;
    registered = 1;
  }

  ZeroMemory(w, sizeof(*w));
  vdp_init_names();
  w->def = dv_defs[which];
  w->sel_line = -1; w->hover_x = w->hover_y = w->lock_x = w->lock_y = -1;
  dv_fonts();
  if (gui_debug_host)                                       /* a pane of the Debugger window */
  {
    CreateWindowExA(0, DV_CLASS, w->def->title, WS_CHILD | WS_VSCROLL | WS_HSCROLL | WS_CLIPCHILDREN, 0, 0, S(760), dv_strip_h() + S(430), gui_debug_host, NULL, g_inst, w);
    gui_debug_last = w->hwnd;
    return;
  }
  CreateWindowExA(0, DV_CLASS, w->def->title, WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_HSCROLL | WS_CLIPCHILDREN,
                  CW_USEDEFAULT, CW_USEDEFAULT, S(760), dv_strip_h() + S(430), parent, NULL, g_inst, w);
  if (w->hwnd)
  {
    gui_center_window(w->hwnd);
    gui_window_icon(w->hwnd);
    ShowWindow(w->hwnd, SW_SHOW);
  }
}

void dlg_vdp_viewer(HWND parent)   { dv_open(0, parent); }
void dlg_sound_viewer(HWND parent) { dv_open(1, parent); }
void dlg_cpu_viewer(HWND parent)   { dv_open(2, parent); }

/* The views of the VDP, Sound and CPU viewers (which 0, 1, 2), for the Debugger window's View menu. */
int debugwin_view_count(int which)
{
  return which >= 0 && which < 3 ? dv_defs[which]->nviews : 0;
}

const char *debugwin_view_name(int which, int i)
{
  return which >= 0 && which < 3 && i >= 0 && i < dv_defs[which]->nviews ? dv_defs[which]->views[i].name : "";
}

int debugwin_view_current(int which)
{
  return which >= 0 && which < 3 && dv_wins[which].hwnd ? dv_wins[which].view : -1;
}

void debugwin_view_select(int which, int i)
{
  dv_win *w;

  if (which < 0 || which > 2) return;
  w = &dv_wins[which];
  if (!w->hwnd || i < 0 || i >= w->def->nviews) return;
  SendMessage(w->view_cb, CB_SETCURSEL, (WPARAM)i, 0);
  dv_select_view(w, i);
}

int debugwin_message(MSG *msg)
{
  int i;

  if (!msg->hwnd) return 0;
  if (ramwatch_message(msg)) return 1;
  if (dbgcpu_message(msg)) return 1;
  if (ramsearch_message(msg)) return 1;
  for (i = 0; i < 3; i++)
  {
    HWND h = dv_wins[i].hwnd;

    if (h && (msg->hwnd == h || IsChild(h, msg->hwnd)))
    {
      if (msg->message == WM_KEYDOWN && msg->wParam == VK_TAB)
      {
        HWND next = GetNextDlgTabItem(h, GetFocus() == h ? NULL : GetFocus(), GetKeyState(VK_SHIFT) < 0);

        if (next) SetFocus(next);
        return 1;
      }
      TranslateMessage(msg);
      DispatchMessage(msg);
      return 1;
    }
  }
  return 0;
}

void debugwin_close(void)
{
  int i;

  debugger_close();
  ramwatch_close();
  dbgcpu_close();
  ramsearch_close();
  for (i = 0; i < 3; i++) if (dv_wins[i].hwnd) DestroyWindow(dv_wins[i].hwnd);
}

void debugwin_enable(int on)
{
  int i;

  debugger_enable(on);
  ramwatch_enable(on);
  dbgcpu_enable(on);
  ramsearch_enable(on);
  for (i = 0; i < 3; i++) if (dv_wins[i].hwnd) EnableWindow(dv_wins[i].hwnd, on ? TRUE : FALSE);
}

int debugwin_owns(HWND h)
{
  int i;

  for (i = 0; i < 3; i++) if (h && dv_wins[i].hwnd == h) return 1;
  return debugger_owns(h);
}
