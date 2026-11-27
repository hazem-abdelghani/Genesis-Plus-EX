/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  memview.c -- Tools > Memory Viewer: a hex view of the memory of the
 *  game that runs (68000 RAM, Z80 RAM, VRAM, CRAM, VSRAM, the ROM, and the
 *  memories of a Mega CD or 32X game). The RAM can be edited by typing.
 *
 *  It is a window of its own that stays open while the game runs, and it
 *  reads the core's memory directly (nothing in the core is changed).
 *  The idea of a memory view that follows the running game, with the bytes
 *  that just changed marked, comes from the memory view of the Genesis Plus
 *  GX debugger project (smd_dgx); this code is written for Win32.
 ****************************************************************************/

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>

#include "shared.h"
#include "gui.h"
#include "theme.h"
#include "resource.h"
#include "picocore.h"
#include "netplay.h"
#include "toolbar.h"            /* gui_dpi */

#define MV_CLASS      "GPEXMemoryViewer"
#define MV_TIMER      1
#define MV_TICK_MS    100
#define MV_MAXROWS    256       /* the most rows kept for marking changes */
#define MV_AGE        6         /* how many ticks a changed byte stays marked */
#define IDC_MV_REGION 101
#define IDC_MV_ADDR   102
#define IDC_MV_GO     103
#define IDC_MV_INFO   104

static HWND mv_win, mv_combo, mv_addr, mv_go, mv_info, mv_tt;
static HFONT mv_mono, mv_ui;
static UINT_PTR mv_look;                /* what the window was laid out for (see dbg_look) */
static int mv_cw, mv_lh;                /* a character of the hex font: width, row height */
static WNDPROC mv_addr_proc;

static int mv_kinds[12], mv_nkinds, mv_region;
static char mv_game[GUI_PATH_LEN];      /* the game the region list was made for */
static int mv_top;                      /* the first row shown */
static int mv_anchor = -1;              /* where a selection of several bytes started (the other end is mv_sel) */
static int mv_sel = -1, mv_nib;         /* the selected byte; mv_nib = 1 once the first digit of a new value was typed */
static int mv_pending;
static uint8 mv_prev[MV_MAXROWS * 16], mv_age[MV_MAXROWS * 16];
static int mv_prev_top = -1, mv_prev_kind = -1;
static char mv_info_text[96];

static int S(int px)
{
  return dbg_scale(px);
}

/****************************************************************************
 * The memories
 ****************************************************************************/

static int mv_range(int *lo, int *hi);

static int mv_md(void)
{
  return pico_active() || (system_hw & SYSTEM_PBC) == SYSTEM_MD;
}

/* Where a memory is now: its bytes, how many, whether the pairs of bytes are swapped (the 16-bit consoles keep them so),
   and whether it can be written. NULL when the game has no such memory. */
static uint8 *mv_mem(int kind, uint32 *size, int *swap, int *writable)
{
  uint8 *p = NULL;
  unsigned n = 0;

  *swap = mv_md() ? 1 : 0;
  *writable = 0;

  switch (kind)
  {
    case MV_WRAM:
      p = work_ram; n = mv_md() ? 0x10000 : 0x2000; *writable = 1; break;
    case MV_ZRAM:
      p = zram; n = 0x2000; *swap = 0; *writable = 1; break;
    case MV_VRAM:
      *writable = 1;
      if (pico_active()) { p = pico_memory(3, &n); break; }
      p = vram; n = mv_md() ? 0x10000 : 0x4000; break;
    case MV_CRAM:
      *writable = 1;
      if (pico_active()) { p = pico_memory(4, &n); break; }
      p = cram; n = mv_md() ? 0x80 : 0x40; *swap = mv_md() ? 1 : 0; break;
    case MV_VSRAM:
      *writable = 1;
      if (pico_active()) { p = pico_memory(5, &n); if (n > 0x50) n = 0x50; break; }
      p = vsram; n = 0x50; break;
    case MV_ROM:
      p = cart.rom; n = cart.romsize; *writable = 1; break;
    case MV_PRG:
      p = scd.prg_ram; n = 0x80000; *writable = 1; break;
    case MV_WORD:
      if (scd.regs[0x03 >> 1].byte.l & 0x04) { p = m68k.memory_map[scd.cartridge.boot + 0x20].base; n = 0x20000; }
      else { p = scd.word_ram_2M; n = 0x40000; }
      *writable = 1; break;
    case MV_PWRAM:
      p = pico_memory(1, &n); *writable = 1; break;
    case MV_PROM:
      p = pico_memory(0, &n); *writable = 1; break;
    case MV_PSDRAM:
      p = pico_memory(2, &n); *writable = 1; break;
    case MV_R68K: case MV_RZ80:
    {
      static uint8 regbuf[96];
      unsigned v[24];
      int count = dbg_cpu_regs(kind == MV_R68K ? 0 : 1, v), i, o = 0;

      if (count < 0) break;
      *swap = 0;
      if (kind == MV_R68K)
      {
        for (i = 0; i < 18; i++) { regbuf[o++] = (uint8)(v[i] >> 24); regbuf[o++] = (uint8)(v[i] >> 16); regbuf[o++] = (uint8)(v[i] >> 8); regbuf[o++] = (uint8)v[i]; }
        *writable = !pico_active();
      }
      else
      {
        for (i = 0; i < 12; i++) { regbuf[o++] = (uint8)(v[i] >> 8); regbuf[o++] = (uint8)v[i]; }
        for (; i < 18; i++) regbuf[o++] = (uint8)v[i];
      }
      p = regbuf; n = (unsigned)o;
      break;
    }
    case MV_RVDP:
      *swap = 0;
      if (pico_active()) { p = pico_memory(6, &n); }
      else { p = reg; n = 0x20; }
      *writable = 1;
      break;
  }

  *size = p ? n : 0;
  return p;
}

static uint32 mv_base(int kind)
{
  switch (kind)
  {
    case MV_WRAM:   return mv_md() ? 0xFF0000 : 0xC000;
    case MV_ZRAM:   return 0xA00000;
    case MV_WORD:   return 0x200000;
    case MV_PWRAM:  return 0xFF0000;
    case MV_PSDRAM: return 0x06000000;
  }
  return 0;
}

static const char *mv_name(int kind)
{
  switch (kind)
  {
    case MV_WRAM:   return mv_md() ? "68000 RAM" : "RAM";
    case MV_ZRAM:   return "Z80 RAM";
    case MV_VRAM:   return "VRAM";
    case MV_CRAM:   return "CRAM (Colors)";
    case MV_VSRAM:  return "VSRAM (Scroll)";
    case MV_ROM:    return "ROM";
    case MV_PRG:    return "Mega CD PRG-RAM";
    case MV_WORD:   return "Mega CD Word RAM";
    case MV_PWRAM:  return "68000 RAM";
    case MV_PROM:   return "ROM";
    case MV_PSDRAM: return "32X SDRAM (SH-2)";
    case MV_R68K:   return "68000 Registers";
    case MV_RZ80:   return "Z80 Registers";
    case MV_RVDP:   return "VDP Registers";
  }
  return "";
}

/* The Genesis Plus GX core keeps a color of the palette in 9 bits (BBBGGGRRR); the game writes it as 16 (BBB0GGG0RRR0), and so it is shown. */
static int mv_core_cram(int kind)
{
  return kind == MV_CRAM && !pico_active() && mv_md();
}

/* One byte at an offset of the memory; -1 outside it. */
static int mv_get(int kind, int off)
{
  uint32 size;
  int swap, w;
  uint8 *p = mv_mem(kind, &size, &swap, &w);

  if (!p || off < 0 || (uint32)off >= size) return -1;
  if (mv_core_cram(kind))
  {
    unsigned c = (unsigned)p[off & ~1] | ((unsigned)p[(off & ~1) + 1] << 8);
    unsigned bus = ((c & 0x1C0) << 3) | ((c & 0x38) << 2) | ((c & 7) << 1);

    return (off & 1) ? (int)(bus & 0xFF) : (int)(bus >> 8);
  }
  return p[swap ? (off ^ 1) : off];
}

/* Marks the tiles around a VRAM byte as changed, so the video chip draws them again (as the core does when the game writes to VRAM). */
static void mv_mark_dirty(unsigned index)
{
  int name = (int)((index >> 5) & 0x7FF);

  if (bg_name_dirty[name] == 0) bg_name_list[bg_list_index++] = (uint16)name;
  bg_name_dirty[name] |= (uint8)(1 << ((index >> 2) & 7));
}

/* A byte of the video chip's memories, changed the way the chip would have written it (so its caches of the sprite table, the tiles
   and the palette follow). */
static void mv_vdp_poke(int kind, int off, int value)
{
  if (!mv_md())                                  /* Master System, Game Gear, SG-1000: bytes (the palette in words of two) */
  {
    if (kind == MV_VRAM)
    {
      unsigned index = (unsigned)off & 0x3FFF;

      if (vram[index] != value)
      {
        vram[index] = (uint8)value;
        if (system_hw >= SYSTEM_MARKIII) mv_mark_dirty(index);
      }
    }
    else if (kind == MV_CRAM)
    {
      int index = (off >> 1) & 0x1F;
      uint16 w;

      cram[off & 0x3F] = (uint8)value;
      w = *(uint16 *)&cram[index << 1];
      color_update_m4(index, w);
      if (index == (0x10 | (reg[7] & 0x0F))) color_update_m4(0x40, w);
    }
    return;
  }

  if (kind == MV_VRAM)
  {
    unsigned index = (unsigned)off & 0xFFFE;
    uint16 *p = (uint16 *)&vram[index];
    uint16 data = *p;
    unsigned sat_mask = (reg[12] & 1) ? 0xFC00 : 0xFE00, sat_addr = (reg[12] & 1) ? 0x03FF : 0x01FF;

    data = (off & 1) ? (uint16)((data & 0xFF00) | (value & 0xFF)) : (uint16)((data & 0x00FF) | ((value & 0xFF) << 8));
    if ((index & sat_mask) == satb) *(uint16 *)&sat[index & sat_addr] = data;
    if (data != *p)
    {
      *p = data;
      mv_mark_dirty(index);
    }
  }
  else if (kind == MV_CRAM)
  {
    int index = (off >> 1) & 0x3F;
    uint16 *p = (uint16 *)&cram[index << 1];
    unsigned c = *p, bus = ((c & 0x1C0) << 3) | ((c & 0x38) << 2) | ((c & 7) << 1), data;

    bus = (off & 1) ? (bus & 0xFF00) | (unsigned)(value & 0xFF) : (bus & 0x00FF) | (unsigned)((value & 0xFF) << 8);
    data = ((bus & 0xE00) >> 3) | ((bus & 0x0E0) >> 2) | ((bus & 0x00E) >> 1);
    if (data != *p)
    {
      *p = (uint16)data;
      if (index & 0x0F) color_update_m5(index, data);
      if (index == (reg[7] & 0x3F)) color_update_m5(0x00, data);
    }
  }
  else if (kind == MV_VSRAM)
  {
    uint16 *p = (uint16 *)&vsram[off & 0x7E];
    uint16 data = *p;

    data = (off & 1) ? (uint16)((data & 0xFF00) | (value & 0xFF)) : (uint16)((data & 0x00FF) | ((value & 0xFF) << 8));
    *p = data & 0x7FF;
  }
}

static int mv_can_edit(void)
{
  uint32 size;
  int swap, w;

  if (mv_nkinds == 0 || netplay_active()) return 0;       /* (a change on one PC of a netplay game would put the two out of step) */
  return mv_mem(mv_kinds[mv_region], &size, &swap, &w) && w;
}

static void mv_put(int kind, int off, int value)
{
  uint32 size;
  int swap, w;
  uint8 *p = mv_mem(kind, &size, &swap, &w);

  if (!p || !w || off < 0 || (uint32)off >= size) return;
  if (kind == MV_RVDP) { dbg_vdp_reg_set(off & 0x1F, value); return; }
  if (kind == MV_R68K)                                   /* a byte of a register: the value is made again and set */
  {
    int idx = off / 4, sh = 24 - (off & 3) * 8;
    unsigned v[24], nv;

    if (dbg_cpu_regs(0, v) < 0 || idx >= 18) return;
    nv = (v[idx] & ~(0xFFu << sh)) | ((unsigned)value << sh);
    m68k_set_reg(idx < 16 ? (m68k_register_t)(M68K_REG_D0 + idx) : idx == 16 ? M68K_REG_PC : M68K_REG_SR, nv);
    return;
  }
  if (kind == MV_VRAM || kind == MV_CRAM || kind == MV_VSRAM)
  {
    if (pico_active()) pico_poke(kind == MV_VRAM ? 3 : kind == MV_CRAM ? 4 : 5, (unsigned)off, value);
    else mv_vdp_poke(kind, off, value);
    return;
  }
  p[swap ? (off ^ 1) : off] = (uint8)value;
}

static uint32 mv_size(void)
{
  uint32 size;
  int swap, w;

  if (mv_nkinds == 0) return 0;
  mv_mem(mv_kinds[mv_region], &size, &swap, &w);
  return size;
}

static int mv_digits(void)
{
  uint32 last = mv_base(mv_kinds[mv_region]) + mv_size();
  int d = 4;

  while (d < 8 && (last >> (4 * d))) d++;
  return d;
}

/* The list of memories this game has. */
static void mv_build(void)
{
  uint32 size;
  int swap, w, i, keep = mv_nkinds ? mv_kinds[mv_region] : -1;

  mv_nkinds = 0;
  if (pico_active())
  {
    mv_kinds[mv_nkinds++] = MV_PWRAM;
    mv_kinds[mv_nkinds++] = MV_PROM;
    if (mv_mem(MV_PSDRAM, &size, &swap, &w)) mv_kinds[mv_nkinds++] = MV_PSDRAM;
    if (mv_mem(MV_VRAM, &size, &swap, &w))
    {
      mv_kinds[mv_nkinds++] = MV_VRAM;
      mv_kinds[mv_nkinds++] = MV_CRAM;
      mv_kinds[mv_nkinds++] = MV_VSRAM;
    }
    mv_kinds[mv_nkinds++] = MV_R68K;
    mv_kinds[mv_nkinds++] = MV_RZ80;
    mv_kinds[mv_nkinds++] = MV_RVDP;
  }
  else if ((system_hw & SYSTEM_PBC) == SYSTEM_MD)
  {
    mv_kinds[mv_nkinds++] = MV_WRAM;
    mv_kinds[mv_nkinds++] = MV_ZRAM;
    mv_kinds[mv_nkinds++] = MV_VRAM;
    mv_kinds[mv_nkinds++] = MV_CRAM;
    mv_kinds[mv_nkinds++] = MV_VSRAM;
    mv_kinds[mv_nkinds++] = MV_ROM;
    if (system_hw == SYSTEM_MCD && !scd.cartridge.boot)
    {
      mv_kinds[mv_nkinds++] = MV_PRG;
      mv_kinds[mv_nkinds++] = MV_WORD;
    }
    mv_kinds[mv_nkinds++] = MV_R68K;
    mv_kinds[mv_nkinds++] = MV_RZ80;
    mv_kinds[mv_nkinds++] = MV_RVDP;
  }
  else
  {
    mv_kinds[mv_nkinds++] = MV_WRAM;
    mv_kinds[mv_nkinds++] = MV_VRAM;
    mv_kinds[mv_nkinds++] = MV_CRAM;
    mv_kinds[mv_nkinds++] = MV_ROM;
    mv_kinds[mv_nkinds++] = MV_RZ80;
    mv_kinds[mv_nkinds++] = MV_RVDP;
  }

  mv_region = 0;
  for (i = 0; i < mv_nkinds; i++) if (mv_kinds[i] == keep) mv_region = i;

  SendMessage(mv_combo, CB_RESETCONTENT, 0, 0);
  for (i = 0; i < mv_nkinds; i++) SendMessageA(mv_combo, CB_ADDSTRING, 0, (LPARAM)mv_name(mv_kinds[i]));
  SendMessage(mv_combo, CB_SETCURSEL, (WPARAM)mv_region, 0);
  lstrcpynA(mv_game, emu_rom_path(), sizeof(mv_game));
  mv_top = 0;
  mv_sel = -1;
  mv_nib = 0;
  mv_prev_top = -1;
}

/****************************************************************************
 * Look
 ****************************************************************************/

static void mv_colors(COLORREF *bg, COLORREF *fg, COLORREF *dim, COLORREF *chg, COLORREF *strip)
{
  HDC probe = GetDC(NULL);
  HBRUSH b = theme_ctlcolor(probe);

  if (b)
  {
    *bg = GetBkColor(probe);
    *fg = GetTextColor(probe);
    *chg = RGB(255, 110, 110);
    *strip = *bg;
  }
  else
  {
    *bg = GetSysColor(COLOR_WINDOW);
    *fg = GetSysColor(COLOR_WINDOWTEXT);
    *chg = RGB(200, 0, 0);
    *strip = GetSysColor(COLOR_BTNFACE);
  }
  ReleaseDC(NULL, probe);
  *dim = RGB((GetRValue(*bg) + GetRValue(*fg)) / 2, (GetGValue(*bg) + GetGValue(*fg)) / 2, (GetBValue(*bg) + GetBValue(*fg)) / 2);
}

static void mv_fonts(void)
{
  HDC dc;
  TEXTMETRICA tm;
  HFONT old;

  if (mv_mono == dbg_mono_font() && mv_ui == dbg_ui_font() && mv_cw) return;
  mv_mono = dbg_mono_font();
  mv_ui = dbg_ui_font();
  dc = GetDC(NULL);
  old = (HFONT)SelectObject(dc, mv_mono);
  GetTextMetricsA(dc, &tm);
  SelectObject(dc, old);
  ReleaseDC(NULL, dc);
  mv_cw = tm.tmAveCharWidth > 0 ? tm.tmAveCharWidth : S(8);
  mv_lh = tm.tmHeight + S(3);
}

static int mv_strip_h(void)
{
  return S(38);
}

static void mv_layout(void)
{
  RECT rc;
  int h = mv_strip_h(), y, x, hh;
  HWND c[5];
  int i;

  if (!mv_win) return;
  GetClientRect(mv_win, &rc);
  hh = S(24);
  y = (h - hh) / 2;
  x = S(8);

  c[0] = mv_combo; c[1] = mv_addr; c[2] = mv_go; c[3] = mv_info; c[4] = NULL;
  for (i = 0; i < 4; i++) SendMessage(c[i], WM_SETFONT, (WPARAM)mv_ui, TRUE);

  SetWindowPos(mv_combo, NULL, x, y, S(190), S(300), SWP_NOZORDER);
  x += S(190) + S(12);
  SetWindowPos(mv_addr, NULL, x, y, S(96), hh, SWP_NOZORDER);
  x += S(96) + S(6);
  SetWindowPos(mv_go, NULL, x, y, S(44), hh, SWP_NOZORDER);
  x += S(44) + S(14);
  SetWindowPos(mv_info, NULL, x, y, rc.right - x - S(8) > S(40) ? rc.right - x - S(8) : S(40), hh, SWP_NOZORDER);
}

static int mv_rows_visible(void)
{
  RECT rc;
  int n;

  GetClientRect(mv_win, &rc);
  n = (rc.bottom - mv_strip_h()) / mv_lh;
  if (n < 1) n = 1;
  if (n > MV_MAXROWS) n = MV_MAXROWS;
  return n;
}

static int mv_rows_total(void)
{
  return (int)((mv_size() + 15) / 16);
}

static void mv_scrollbar(void)
{
  SCROLLINFO si;
  int total = mv_rows_total(), page = mv_rows_visible(), max_top = total > page ? total - page : 0;

  if (mv_top > max_top) mv_top = max_top;
  if (mv_top < 0) mv_top = 0;
  ZeroMemory(&si, sizeof(si));
  si.cbSize = sizeof(si);
  si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
  si.nMin = 0;
  si.nMax = total > 0 ? total - 1 : 0;
  si.nPage = (UINT)page;
  si.nPos = mv_top;
  SetScrollInfo(mv_win, SB_VERT, &si, TRUE);
}

static void mv_pane_invalidate(void)
{
  RECT rc;

  GetClientRect(mv_win, &rc);
  rc.top = mv_strip_h();
  InvalidateRect(mv_win, &rc, FALSE);
}

static void mv_set_top(int top)
{
  mv_top = top;
  mv_scrollbar();
  mv_prev_top = -1;
  mv_pane_invalidate();
}

static void mv_show_sel(void)
{
  int row, page = mv_rows_visible();

  if (mv_sel < 0) return;
  row = mv_sel / 16;
  if (row < mv_top) mv_set_top(row);
  else if (row >= mv_top + page) mv_set_top(row - page + 1);
}

/* An address in `digits` hex digits (wsprintf has no variable width). */
static void mv_addr_text(char *out, unsigned value, int digits)
{
  int i;

  for (i = 0; i < digits; i++) out[i] = "0123456789ABCDEF"[(value >> (4 * (digits - 1 - i))) & 15];
  out[digits] = '\0';
}

static void mv_update_info(void)
{
  char t[96], a[16];
  int kind, b, b2, digits;

  if (mv_nkinds == 0) return;
  kind = mv_kinds[mv_region];
  digits = mv_digits();
  if (mv_sel < 0)
    gui_snprintf(t, sizeof(t), "%s", mv_can_edit() ? "Click a byte, then type hex digits to change it." : "Read only.");
  else
  {
    b = mv_get(kind, mv_sel);
    b2 = mv_get(kind, mv_sel + 1);
    mv_addr_text(a, (unsigned)(mv_base(kind) + mv_sel), digits);
    if (b < 0) gui_snprintf(t, sizeof(t), " ");
    else if (b2 < 0) gui_snprintf(t, sizeof(t), "%s: %02X (%d)", a, b, b);
    else gui_snprintf(t, sizeof(t), "%s: %02X (%d)   Word %04X%s", a, b, b,
                      mv_md() ? (b << 8) | b2 : (b2 << 8) | b, mv_can_edit() ? "" : "   Read only");
  }
  if (lstrcmpA(t, mv_info_text))
  {
    lstrcpynA(mv_info_text, t, sizeof(mv_info_text));
    SetWindowTextA(mv_info, t);
  }
}

/****************************************************************************
 * Painting
 ****************************************************************************/

static int mv_hexcol(int i, int digits)
{
  return digits + 2 + i * 3 + (i >= 8 ? 1 : 0);
}

static int mv_asciicol(int i, int digits)
{
  return digits + 2 + 49 + 2 + i;
}

static void mv_paint(HDC target, const RECT *client)
{
  COLORREF bg, fg, dim, chg, strip;
  int w = client->right, h = client->bottom, top = mv_strip_h();
  HDC dc = CreateCompatibleDC(target);
  HBITMAP bmp = CreateCompatibleBitmap(target, w, h), oldbmp;
  HFONT oldfont;
  HBRUSH br;
  RECT r;
  int kind, digits, rows, row, i, x0 = S(8), rlo = 0, rhi = -1, has_sel = mv_range(&rlo, &rhi);
  COLORREF accent = theme_accent_bg();

  if (mv_nkinds == 0) { DeleteDC(dc); DeleteObject(bmp); return; }
  kind = mv_kinds[mv_region];
  digits = mv_digits();
  rows = mv_rows_visible() + 1;
  mv_colors(&bg, &fg, &dim, &chg, &strip);

  oldbmp = (HBITMAP)SelectObject(dc, bmp);
  oldfont = (HFONT)SelectObject(dc, mv_mono);
  SetBkMode(dc, TRANSPARENT);

  br = CreateSolidBrush(bg);
  r = *client;
  r.top = top;
  FillRect(dc, &r, br);
  DeleteObject(br);
  br = CreateSolidBrush(strip);
  r.top = 0; r.bottom = top;
  FillRect(dc, &r, br);
  DeleteObject(br);

  for (row = 0; row < rows; row++)
  {
    int y = top + row * mv_lh, base = (mv_top + row) * 16;
    char s[16];

    if (base >= (int)mv_size()) break;
    SetTextColor(dc, dim);
    mv_addr_text(s, (unsigned)(mv_base(kind) + base), digits);
    s[digits] = ':';
    TextOutA(dc, x0, y + S(1), s, digits + 1);

    for (i = 0; i < 16; i++)
    {
      int off = base + i, v = mv_get(kind, off), idx = row * 16 + i, selected;
      char hx[4], ch;
      int hxl = x0 + mv_hexcol(i, digits) * mv_cw, axl = x0 + mv_asciicol(i, digits) * mv_cw;
      COLORREF col;

      if (v < 0) break;
      if (idx < MV_MAXROWS * 16 && mv_age[idx]) col = chg; else col = fg;
      ch = (v >= 32 && v < 127) ? (char)v : '.';
      selected = has_sel && off >= rlo && off <= rhi;

      if (selected)
      {
        RECT sr;
        HBRUSH sb = CreateSolidBrush(accent);

        sr.left = hxl - 1; sr.top = y; sr.right = hxl + 2 * mv_cw + 1; sr.bottom = y + mv_lh;
        FillRect(dc, &sr, sb);
        sr.left = axl; sr.right = axl + mv_cw;
        FillRect(dc, &sr, sb);
        DeleteObject(sb);
        col = RGB(255, 255, 255);
      }
      if (off == mv_sel && mv_nib) wsprintfA(hx, "%X_", mv_pending);
      else wsprintfA(hx, "%02X", v);
      SetTextColor(dc, col);
      TextOutA(dc, hxl, y + S(1), hx, 2);
      TextOutA(dc, axl, y + S(1), &ch, 1);
    }
  }

  br = CreateSolidBrush(dim);
  r.left = 0; r.right = w; r.top = top - 1; r.bottom = top;
  FillRect(dc, &r, br);
  DeleteObject(br);

  BitBlt(target, 0, 0, w, h, dc, 0, 0, SRCCOPY);
  SelectObject(dc, oldfont);
  SelectObject(dc, oldbmp);
  DeleteObject(bmp);
  DeleteDC(dc);
}

/****************************************************************************
 * Behavior
 ****************************************************************************/

/* Every tick: the bytes shown are compared with the last time, and a changed one stays marked for a moment. */
static void mv_tick(void)
{
  int kind, rows, row, i, dirty = 0;

  if (mv_nkinds == 0) return;
  if (lstrcmpA(mv_game, emu_rom_path())) { mv_build(); mv_scrollbar(); mv_layout(); mv_pane_invalidate(); return; }

  kind = mv_kinds[mv_region];
  rows = mv_rows_visible() + 1;
  if (rows > MV_MAXROWS) rows = MV_MAXROWS;
  if (mv_prev_top != mv_top || mv_prev_kind != kind)
  {
    ZeroMemory(mv_age, sizeof(mv_age));
    mv_prev_top = mv_top;
    mv_prev_kind = kind;
    for (i = 0; i < rows * 16; i++) { int v = mv_get(kind, mv_top * 16 + i); mv_prev[i] = (uint8)(v < 0 ? 0 : v); }
    dirty = 1;
  }
  else
  {
    for (row = 0; row < rows; row++)
      for (i = 0; i < 16; i++)
      {
        int idx = row * 16 + i, v = mv_get(kind, (mv_top + row) * 16 + i);

        if (v < 0) v = 0;
        if (v != mv_prev[idx]) { mv_prev[idx] = (uint8)v; mv_age[idx] = MV_AGE; dirty = 1; }
        else if (mv_age[idx]) { mv_age[idx]--; dirty = 1; }
      }
  }
  if (dirty) mv_pane_invalidate();
  mv_update_info();
}

static void mv_go_to(void)
{
  char t[32];
  unsigned long a;
  char *end;
  int kind = mv_kinds[mv_region];

  GetWindowTextA(mv_addr, t, sizeof(t));
  a = strtoul(t, &end, 16);
  if (end == t) return;
  if (a >= mv_base(kind) && a - mv_base(kind) < mv_size()) a -= mv_base(kind);
  if (a >= mv_size()) { gui_notify("That address is outside %s", mv_name(kind)); return; }
  mv_sel = (int)a;
  mv_nib = 0;
  mv_set_top((int)(a / 16) - mv_rows_visible() / 2);
  mv_update_info();
  SetFocus(mv_win);
}

static void mv_select_ex(int off, int extend)
{
  if (off < 0) off = 0;
  if ((uint32)off >= mv_size()) off = (int)mv_size() - 1;
  if (!extend || mv_anchor < 0) mv_anchor = off;
  mv_sel = off;
  mv_nib = 0;
  mv_show_sel();
  mv_pane_invalidate();
  mv_update_info();
}

static void mv_select(int off)
{
  mv_select_ex(off, 0);
}

/* The byte under a point of the window, or -1. */
static int mv_hit(int x, int y)
{
  int digits, row, i, cx, kind;

  if (mv_nkinds == 0 || y < mv_strip_h()) return -1;
  kind = mv_kinds[mv_region];
  digits = mv_digits();
  row = (y - mv_strip_h()) / mv_lh;
  cx = (x - S(8)) / mv_cw;
  for (i = 0; i < 16; i++)
  {
    int hc = mv_hexcol(i, digits);

    if ((cx >= hc && cx <= hc + 1) || cx == mv_asciicol(i, digits))
    {
      int off = (mv_top + row) * 16 + i;

      return mv_get(kind, off) >= 0 ? off : -1;
    }
  }
  return -1;
}

/* The bytes selected: from lo to hi (one byte when nothing more was selected). */
static int mv_range(int *lo, int *hi)
{
  if (mv_sel < 0) return 0;
  *lo = mv_anchor >= 0 && mv_anchor < mv_sel ? mv_anchor : mv_sel;
  *hi = mv_anchor >= 0 && mv_anchor > mv_sel ? mv_anchor : mv_sel;
  return 1;
}

static void mv_type(int digit)
{
  if (mv_sel < 0 || !mv_can_edit()) return;
  if (!mv_nib)
  {
    mv_pending = digit;
    mv_nib = 1;
  }
  else
  {
    mv_put(mv_kinds[mv_region], mv_sel, (mv_pending << 4) | digit);
    mv_nib = 0;
    mv_select(mv_sel + 1);
    return;
  }
  mv_pane_invalidate();
}

/* The bytes of the selection as text: 0 = hex bytes with spaces (16 to a line), 1 = hex digits only, 2 = characters. */
static char *mv_selection_text(int how)
{
  int lo, hi, i, n, kind = mv_kinds[mv_region];
  char *t, *o;

  if (!mv_range(&lo, &hi)) return NULL;
  if (hi - lo > 0xFFFFF) hi = lo + 0xFFFFF;
  n = hi - lo + 1;
  t = (char *)malloc((size_t)n * 3 + (size_t)n / 16 + 16);
  if (!t) return NULL;
  o = t;
  for (i = 0; i < n; i++)
  {
    int v = mv_get(kind, lo + i);

    if (v < 0) break;
    if (how == 2) *o++ = (v >= 32 && v < 127) ? (char)v : '.';
    else
    {
      *o++ = "0123456789ABCDEF"[v >> 4];
      *o++ = "0123456789ABCDEF"[v & 15];
      if (how == 0) *o++ = ((i & 15) == 15 && i + 1 < n) ? '\n' : ' ';
    }
  }
  if (how == 0 && o > t && o[-1] == ' ') o--;
  *o = '\0';
  return t;
}

static void mv_clip_set(const char *text)
{
  size_t n = strlen(text) + 1;
  HGLOBAL g;

  if (!OpenClipboard(mv_win)) return;
  EmptyClipboard();
  g = GlobalAlloc(GMEM_MOVEABLE, n);
  if (g)
  {
    memcpy(GlobalLock(g), text, n);
    GlobalUnlock(g);
    if (!SetClipboardData(CF_TEXT, g)) GlobalFree(g);
  }
  CloseClipboard();
}

static void mv_copy(int how)
{
  char *t = (how == 3) ? NULL : mv_selection_text(how);
  char a[16];
  int lo, hi;

  if (how == 3)                                          /* the address of the first byte */
  {
    if (!mv_range(&lo, &hi)) return;
    mv_addr_text(a, (unsigned)(mv_base(mv_kinds[mv_region]) + lo), mv_digits());
    mv_clip_set(a);
    return;
  }
  if (!t) return;
  mv_clip_set(t);
  free(t);
}

/* Puts the text of the clipboard into the memory from the selection on: how 1 = as hex digits, 2 = as characters, 0 = hex digits when that is all the
   text is made of, else characters. */
static void mv_paste(int how)
{
  HANDLE g;
  const char *t;
  int lo, hi, n = 0, i, kind;

  if (!mv_range(&lo, &hi)) return;
  if (!mv_can_edit()) { gui_notify("This memory cannot be changed now"); return; }
  if (!OpenClipboard(mv_win)) return;
  g = GetClipboardData(CF_TEXT);
  t = g ? (const char *)GlobalLock(g) : NULL;
  if (t)
  {
    kind = mv_kinds[mv_region];
    if (how == 0)
    {
      how = 1;
      for (i = 0; t[i]; i++)
        if (!((t[i] >= '0' && t[i] <= '9') || (t[i] >= 'a' && t[i] <= 'f') || (t[i] >= 'A' && t[i] <= 'F') || t[i] == ' ' || t[i] == '\r' || t[i] == '\n' || t[i] == '\t'))
          { how = 2; break; }
    }
    if (how == 2)
    {
      for (i = 0; t[i] && (uint32)(lo + n) < mv_size(); i++)
        if (t[i] != '\r' && t[i] != '\n') mv_put(kind, lo + n++, (unsigned char)t[i]);
    }
    else
    {
      int hi_nib = -1;

      for (i = 0; t[i] && (uint32)(lo + n) < mv_size(); i++)
      {
        int d = (t[i] >= '0' && t[i] <= '9') ? t[i] - '0' : (t[i] >= 'a' && t[i] <= 'f') ? t[i] - 'a' + 10 : (t[i] >= 'A' && t[i] <= 'F') ? t[i] - 'A' + 10 : -1;

        if (d < 0) continue;
        if (hi_nib < 0) hi_nib = d;
        else { mv_put(kind, lo + n++, (hi_nib << 4) | d); hi_nib = -1; }
      }
    }
    GlobalUnlock(g);
  }
  CloseClipboard();
  if (n > 0)
  {
    mv_prev_top = -1;
    mv_pane_invalidate();
    gui_notify("%d byte%s pasted", n, n == 1 ? "" : "s");
  }
}

/* Writes the selection (or, with nothing but one byte selected, the whole memory) to a file. */
static void mv_dump(void)
{
  int lo, hi, kind = mv_kinds[mv_region], i, n;
  unsigned char *b;
  char def[48];

  if (!mv_range(&lo, &hi)) { lo = 0; hi = (int)mv_size() - 1; }
  if (hi == lo) { lo = 0; hi = (int)mv_size() - 1; }
  n = hi - lo + 1;
  if (n <= 0) return;
  b = (unsigned char *)malloc((size_t)n);
  if (!b) return;
  for (i = 0; i < n; i++) { int v = mv_get(kind, lo + i); b[i] = (unsigned char)(v < 0 ? 0 : v); }
  gui_snprintf(def, sizeof(def), "%s.bin", mv_name(kind));
  for (i = 0; def[i]; i++) if (def[i] == ' ' || def[i] == '(' || def[i] == ')') def[i] = '_';
  if (dbg_save_file(mv_win, "Dump to File", def, "Binary files (*.bin)|*.bin|All files (*.*)|*.*", b, (size_t)n))
    gui_notify("%d bytes written", n);
  free(b);
}

#define MV_CMD_COPY      1
#define MV_CMD_COPYNUM   2
#define MV_CMD_COPYCHR   3
#define MV_CMD_COPYADDR  4
#define MV_CMD_PASTE     5
#define MV_CMD_PASTENUM  6
#define MV_CMD_PASTECHR  7
#define MV_CMD_DUMP      8
#define MV_CMD_GOTO      9

static void mv_menu(int x, int y)
{
  HMENU m = CreatePopupMenu();
  UINT can = mv_sel >= 0 ? MF_STRING : (MF_STRING | MF_GRAYED), pcan = (mv_sel >= 0 && mv_can_edit()) ? MF_STRING : (MF_STRING | MF_GRAYED);
  int cmd;

  AppendMenuA(m, can, MV_CMD_COPY, "Copy\tCtrl+C");
  AppendMenuA(m, can, MV_CMD_COPYNUM, "Copy Numbers");
  AppendMenuA(m, can, MV_CMD_COPYCHR, "Copy Characters");
  AppendMenuA(m, can, MV_CMD_COPYADDR, "Copy Address");
  AppendMenuA(m, MF_SEPARATOR, 0, NULL);
  AppendMenuA(m, pcan, MV_CMD_PASTE, "Paste\tCtrl+V");
  AppendMenuA(m, pcan, MV_CMD_PASTENUM, "Paste to Numbers");
  AppendMenuA(m, pcan, MV_CMD_PASTECHR, "Paste to Characters");
  AppendMenuA(m, MF_SEPARATOR, 0, NULL);
  AppendMenuA(m, MF_STRING, MV_CMD_DUMP, "Dump to File...");
  AppendMenuA(m, MF_STRING, MV_CMD_GOTO, "Go To...");
  if (theme_menus_owner()) theme_ownerdraw_menu(m);
  cmd = gui_popup(m, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON, x, y);
  DestroyMenu(m);
  switch (cmd)
  {
    case MV_CMD_COPY:     mv_copy(0); break;
    case MV_CMD_COPYNUM:  mv_copy(1); break;
    case MV_CMD_COPYCHR:  mv_copy(2); break;
    case MV_CMD_COPYADDR: mv_copy(3); break;
    case MV_CMD_PASTE:    mv_paste(0); break;
    case MV_CMD_PASTENUM: mv_paste(1); break;
    case MV_CMD_PASTECHR: mv_paste(2); break;
    case MV_CMD_DUMP:     mv_dump(); break;
    case MV_CMD_GOTO:     SetFocus(mv_addr); SendMessage(mv_addr, EM_SETSEL, 0, -1); break;
  }
}

static LRESULT CALLBACK mv_addr_sub(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
  if (m == WM_KEYDOWN && wp == VK_RETURN) { mv_go_to(); return 0; }
  if (m == WM_CHAR && wp == '\r') return 0;
  return CallWindowProc(mv_addr_proc, h, m, wp, lp);
}

static LRESULT CALLBACK mv_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
  switch (m)
  {
    case WM_CREATE:
      mv_win = h;
      mv_fonts();
      mv_look = dbg_look();
      mv_combo = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                 0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_MV_REGION, g_inst, NULL);
      mv_addr = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_MV_ADDR, g_inst, NULL);
      mv_go = CreateWindowExA(0, "BUTTON", "Go", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                              0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_MV_GO, g_inst, NULL);
      mv_info = CreateWindowExA(0, "STATIC", "", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX,
                                0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_MV_INFO, g_inst, NULL);
      mv_addr_proc = (WNDPROC)SetWindowLongPtr(mv_addr, GWLP_WNDPROC, (LONG_PTR)mv_addr_sub);
      mv_build();
      mv_layout();
      mv_scrollbar();
      theme_apply_to_window(h);
      mv_tt = dbg_tips_new(h);
      dbg_tip(mv_tt, h, mv_combo, "Which memory is shown: the 68000 RAM, Z80 RAM, video memories, ROM and, in some games, more.");
      dbg_tip(mv_tt, h, mv_addr, "Type an address in hex and press Enter to jump to it.");
      dbg_tip(mv_tt, h, mv_go, "Jump to the address typed beside it.");
      SetTimer(h, MV_TIMER, MV_TICK_MS, NULL);
      return 0;

    case WM_SIZE:
      mv_layout();
      mv_scrollbar();
      InvalidateRect(h, NULL, FALSE);
      return 0;

    case WM_GETMINMAXINFO:
    {
      MINMAXINFO *mi = (MINMAXINFO *)lp;
      mi->ptMinTrackSize.x = S(520);
      mi->ptMinTrackSize.y = S(220);
      return 0;
    }

    case WM_DPICHANGED:
    {
      const RECT *r = (const RECT *)lp;

      mv_fonts();
      mv_layout();
      mv_scrollbar();
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
      mv_paint(dc, &rc);
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
      if (wp == MV_TIMER && IsWindowVisible(h))
      {
        if (dbg_look() != mv_look)           /* the text size, the style or the scale changed (the Large interface was switched on or off ...) */
        {
          mv_look = dbg_look();
          mv_fonts();
          mv_layout();
          mv_scrollbar();
          InvalidateRect(h, NULL, FALSE);
        }
        mv_tick();
      }
      return 0;

    case WM_VSCROLL:
    {
      SCROLLINFO si;
      int top = mv_top, page = mv_rows_visible();

      ZeroMemory(&si, sizeof(si));
      si.cbSize = sizeof(si);
      si.fMask = SIF_ALL;
      GetScrollInfo(h, SB_VERT, &si);
      switch (LOWORD(wp))
      {
        case SB_LINEUP:        top -= 1; break;
        case SB_LINEDOWN:      top += 1; break;
        case SB_PAGEUP:        top -= page - 1; break;
        case SB_PAGEDOWN:      top += page - 1; break;
        case SB_TOP:           top = 0; break;
        case SB_BOTTOM:        top = mv_rows_total(); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: top = si.nTrackPos; break;
      }
      mv_set_top(top);
      return 0;
    }

    case WM_MOUSEWHEEL:
      mv_set_top(mv_top - (GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA) * 3);
      return 0;

    case WM_LBUTTONDOWN:
    {
      int off = mv_hit((short)LOWORD(lp), (short)HIWORD(lp));

      SetFocus(h);
      if (off >= 0) { mv_select_ex(off, (wp & MK_SHIFT) != 0); SetCapture(h); }
      return 0;
    }

    case WM_MOUSEMOVE:
      if ((wp & MK_LBUTTON) && GetCapture() == h)
      {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp), off;

        if (y < mv_strip_h()) mv_set_top(mv_top - 1);                 /* dragging above or below the rows scrolls */
        else if (y > mv_strip_h() + mv_rows_visible() * mv_lh) mv_set_top(mv_top + 1);
        off = mv_hit(x, y < mv_strip_h() ? mv_strip_h() : y);
        if (off >= 0 && off != mv_sel) mv_select_ex(off, 1);
      }
      return 0;

    case WM_LBUTTONUP:
      if (GetCapture() == h) ReleaseCapture();
      return 0;

    case WM_CONTEXTMENU:
    {
      POINT pt;
      int off, lo, hi;

      if (mv_nkinds == 0) break;
      pt.x = (short)LOWORD(lp); pt.y = (short)HIWORD(lp);
      if (lp == -1) { pt.x = 0; pt.y = 0; ClientToScreen(h, &pt); pt.x += S(60); pt.y += mv_strip_h() + S(20); }
      {
        POINT cp = pt;

        ScreenToClient(h, &cp);
        off = mv_hit(cp.x, cp.y);
      }
      if (off >= 0 && !(mv_range(&lo, &hi) && off >= lo && off <= hi)) mv_select(off);
      mv_menu(pt.x, pt.y);
      return 0;
    }

    case WM_GETDLGCODE:
      return DLGC_WANTARROWS | DLGC_WANTCHARS;

    case WM_KEYDOWN:
    {
      int page = mv_rows_visible();

      if (mv_nkinds == 0) break;
      if (mv_sel < 0 && wp != VK_NEXT && wp != VK_PRIOR && wp != VK_HOME && wp != VK_END)
        { if (wp == VK_LEFT || wp == VK_RIGHT || wp == VK_UP || wp == VK_DOWN) mv_select(mv_top * 16); return 0; }
      switch (wp)
      {
        case VK_LEFT:  mv_select_ex(mv_sel - 1, GetKeyState(VK_SHIFT) < 0); break;
        case VK_RIGHT: mv_select_ex(mv_sel + 1, GetKeyState(VK_SHIFT) < 0); break;
        case VK_UP:    mv_select_ex(mv_sel - 16, GetKeyState(VK_SHIFT) < 0); break;
        case VK_DOWN:  mv_select_ex(mv_sel + 16, GetKeyState(VK_SHIFT) < 0); break;
        case 'C': if (GetKeyState(VK_CONTROL) < 0) mv_copy(0); break;
        case 'V': if (GetKeyState(VK_CONTROL) < 0) mv_paste(0); break;
        case VK_PRIOR: mv_set_top(mv_top - (page - 1)); break;
        case VK_NEXT:  mv_set_top(mv_top + (page - 1)); break;
        case VK_HOME:  mv_set_top(0); break;
        case VK_END:   mv_set_top(mv_rows_total()); break;
        case VK_ESCAPE:
          if (mv_nib) { mv_nib = 0; mv_pane_invalidate(); }
          else gui_debug_close(h);
          break;
      }
      return 0;
    }

    case WM_CHAR:
      if ((wp >= '0' && wp <= '9') || (wp >= 'a' && wp <= 'f') || (wp >= 'A' && wp <= 'F'))
        mv_type(wp <= '9' ? (int)(wp - '0') : (int)((wp | 0x20) - 'a' + 10));
      return 0;

    case WM_COMMAND:
      switch (LOWORD(wp))
      {
        case IDC_MV_REGION:
          if (HIWORD(wp) == CBN_SELCHANGE)
          {
            int k = (int)SendMessage(mv_combo, CB_GETCURSEL, 0, 0);

            if (k >= 0 && k < mv_nkinds)
            {
              mv_region = k;
              mv_top = 0;
              mv_sel = -1;
              mv_nib = 0;
              mv_prev_top = -1;
              mv_scrollbar();
              mv_pane_invalidate();
              mv_update_info();
              SetFocus(h);
            }
          }
          return 0;
        case IDC_MV_GO:
          mv_go_to();
          return 0;
      }
      break;

    case WM_CLOSE:
      DestroyWindow(h);
      return 0;

    case WM_DESTROY:
      KillTimer(h, MV_TIMER);
      mv_win = NULL;
      mv_nkinds = 0;
      mv_info_text[0] = '\0';
      return 0;
  }

  return DefWindowProc(h, m, wp, lp);
}

/****************************************************************************
 * The rest of the program
 ****************************************************************************/

void dlg_memory_viewer(HWND parent)
{
  static int registered;

  if (mv_win)
  {
    HWND top = GetAncestor(mv_win, GA_ROOT);

    if (IsIconic(top)) ShowWindow(top, SW_RESTORE);
    SetForegroundWindow(top);
    return;
  }

  if (!registered)
  {
    WNDCLASSA wc;

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = mv_proc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = MV_CLASS; i18n_keep_ltr(MV_CLASS);
    if (!RegisterClassA(&wc)) return;
    registered = 1;
  }

  mv_fonts();
  if (gui_debug_host)                                       /* a pane of the Debugger window */
  {
    CreateWindowExA(0, MV_CLASS, "Memory Viewer", WS_CHILD | WS_VSCROLL | WS_CLIPCHILDREN, 0, 0, 80 * mv_cw + S(40), mv_strip_h() + 24 * mv_lh + S(40), gui_debug_host, NULL, g_inst, NULL);
    gui_debug_last = mv_win;
    return;
  }
  CreateWindowExA(0, MV_CLASS, "Memory Viewer", WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_CLIPCHILDREN,
                  CW_USEDEFAULT, CW_USEDEFAULT, 80 * mv_cw + S(40), mv_strip_h() + 24 * mv_lh + S(40), parent, NULL, g_inst, NULL);
  if (mv_win)
  {
    gui_center_window(mv_win);
    gui_window_icon(mv_win);
    ShowWindow(mv_win, SW_SHOW);
  }
}

int memview_message(MSG *msg)
{
  if (!mv_win || !msg->hwnd) return 0;
  if (msg->hwnd != mv_win && !IsChild(mv_win, msg->hwnd)) return 0;
  if (msg->message == WM_KEYDOWN && msg->wParam == VK_TAB)
  {
    HWND next = GetNextDlgTabItem(mv_win, GetFocus() == mv_win ? NULL : GetFocus(), GetKeyState(VK_SHIFT) < 0);

    if (next) SetFocus(next);
    return 1;
  }
  TranslateMessage(msg);
  DispatchMessage(msg);
  return 1;
}

void memview_close(void)
{
  if (mv_win) DestroyWindow(mv_win);
}

void memview_enable(int on)
{
  if (mv_win) EnableWindow(mv_win, on ? TRUE : FALSE);
}

HWND memview_hwnd(void)
{
  return mv_win;
}

/****************************************************************************
 * What the other debug windows use
 ****************************************************************************/

/* The look of the debug windows: the text font of the program (so the Large interface, the Modern style and the display scale carry over to
   them) and sizes in proportion to it (a size in pixels is as it would be at the Classic style's normal text size). */
static HFONT dbg_base_font, dbg_mono_font_h;
static int dbg_tmh = 13;

static void dbg_measure(void)
{
  HFONT f = gui_get_ui_font();
  HDC dc;
  TEXTMETRICA tm;
  HFONT old;
  LOGFONTA lf;

  if (f == dbg_base_font && dbg_mono_font_h) return;
  dbg_base_font = f;
  dc = GetDC(NULL);
  old = (HFONT)SelectObject(dc, f);
  GetTextMetricsA(dc, &tm);
  SelectObject(dc, old);
  ReleaseDC(NULL, dc);
  dbg_tmh = tm.tmHeight > 8 ? tm.tmHeight : 13;
  if (GetObjectA(f, sizeof(lf), &lf))
  {
    lf.lfHeight = lf.lfHeight * 11 / 10;             /* the text of the hex and list areas is a little larger than the buttons' */
    lf.lfWidth = 0;
    lf.lfWeight = FW_NORMAL;
    lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpynA(lf.lfFaceName, "Consolas", LF_FACESIZE);
    dbg_mono_font_h = CreateFontIndirectA(&lf);       /* (a font that was in use before is left alone: a window may still hold it) */
  }
  if (!dbg_mono_font_h) dbg_mono_font_h = (HFONT)GetStockObject(ANSI_FIXED_FONT);
}

HFONT dbg_ui_font(void)
{
  dbg_measure();
  return dbg_base_font;
}

HFONT dbg_mono_font(void)
{
  dbg_measure();
  return dbg_mono_font_h;
}

int dbg_scale(int px)
{
  dbg_measure();
  return MulDiv(px, dbg_tmh, 13);
}

UINT_PTR dbg_look(void)
{
  dbg_measure();
  return (UINT_PTR)dbg_base_font;
}

/* Tooltips for the debug windows: one tooltip window per window; a tip goes on a control (text is copied; putting another on the
   same control changes it, for the buttons whose names depend on the view). */
HWND dbg_tips_new(HWND parent)
{
  HWND tt = CreateWindowExA(WS_EX_TOPMOST, TOOLTIPS_CLASSA, NULL, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0, parent, NULL, g_inst, NULL);

  if (!tt) return NULL;
  SendMessage(tt, TTM_SETMAXTIPWIDTH, 0, dbg_scale(320));
  SendMessage(tt, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);
  SendMessage(tt, TTM_SETDELAYTIME, TTDT_INITIAL, 400);
  SendMessage(tt, WM_SETFONT, (WPARAM)dbg_ui_font(), FALSE);
  return tt;
}

void dbg_tip(HWND tt, HWND parent, HWND ctl, const char *text)
{
  TOOLINFOA ti;

  if (!tt || !ctl || !text) return;
  memset(&ti, 0, sizeof(ti));
  ti.cbSize = sizeof(ti);
  ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
  ti.hwnd = parent;
  ti.uId = (UINT_PTR)ctl;
  if (SendMessageA(tt, TTM_GETTOOLINFOA, 0, (LPARAM)&ti))
  {
    ti.lpszText = (LPSTR)text;
    SendMessageA(tt, TTM_UPDATETIPTEXTA, 0, (LPARAM)&ti);
  }
  else
  {
    ti.lpszText = (LPSTR)text;
    SendMessageA(tt, TTM_ADDTOOLA, 0, (LPARAM)&ti);
  }
}

int dbgmem_get(int kind, int off)
{
  return mv_get(kind, off);
}

void dbgmem_put(int kind, int off, int value)
{
  mv_put(kind, off, value);
}

int dbgmem_size(int kind)
{
  uint32 size;
  int swap, w;

  return mv_mem(kind, &size, &swap, &w) ? (int)size : 0;
}

/* The filter of the file dialogs is written "Name|*.ext|Name|*.ext", and the dialogs want its parts apart by zero characters. */
static void dbg_filter(const char *in, char *out, int len)
{
  char raw[256];
  int i;

  for (i = 0; in[i] && i < (int)sizeof(raw) - 2; i++) raw[i] = in[i] == '|' ? '\0' : in[i];     /* "text|pattern|text|pattern" -> the Windows form */
  raw[i++] = '\0';
  raw[i] = '\0';
  i18n_filter(raw, out, (size_t)len);
}

int dbg_save_file(HWND owner, const char *title, const char *defname, const char *filter, const void *data, size_t len)
{
  OPENFILENAMEA ofn;
  char path[MAX_PATH], flt[256];
  FILE *f;
  int ok;

  lstrcpynA(path, defname, sizeof(path));
  dbg_filter(filter, flt, sizeof(flt));
  ZeroMemory(&ofn, sizeof(ofn));
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = owner;
  ofn.lpstrFilter = flt;
  ofn.lpstrFile = path;
  ofn.nMaxFile = sizeof(path);
  ofn.lpstrTitle = tr(title);
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
  if (!GetSaveFileNameA(&ofn)) return 0;
  f = fopen(path, "wb");
  if (!f) { gui_notify("Could not write that file"); return 0; }
  ok = fwrite(data, 1, len, f) == len;
  fclose(f);
  if (!ok) gui_notify("Could not write that file");
  return ok;
}

unsigned char *dbg_open_file(HWND owner, const char *title, const char *filter, size_t *len)
{
  OPENFILENAMEA ofn;
  char path[MAX_PATH] = "", flt[256];
  FILE *f;
  long n;
  unsigned char *b;

  dbg_filter(filter, flt, sizeof(flt));
  ZeroMemory(&ofn, sizeof(ofn));
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = owner;
  ofn.lpstrFilter = flt;
  ofn.lpstrFile = path;
  ofn.nMaxFile = sizeof(path);
  ofn.lpstrTitle = tr(title);
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
  if (!GetOpenFileNameA(&ofn)) return NULL;
  f = fopen(path, "rb");
  if (!f) { gui_notify("Could not open that file"); return NULL; }
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n <= 0 || n > 0x1000000) { fclose(f); gui_notify("That file is empty or too large"); return NULL; }
  b = (unsigned char *)malloc((size_t)n);
  if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); gui_notify("Could not read that file"); return NULL; }
  fclose(f);
  *len = (size_t)n;
  return b;
}
