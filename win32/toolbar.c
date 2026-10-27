/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  toolbar.c -- flat icon buttons for the ROM browser's toolbar.
 *
 *  The Refresh and Reset icons follow the Lucide "RefreshCw" and "RotateCcw"
 *  shapes (lucide.dev, ISC license).
 *
 *  Each icon is drawn four times larger into a mask and reduced, which gives
 *  smooth edges, then blended in the theme's text color (gray when the
 *  button is disabled).
 ****************************************************************************/

#include <windows.h>
#include <commctrl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "shared.h"
#include "gui.h"
#include "theme.h"
#include "toolbar.h"

#define SS  4                    /* supersampling */
#define PI_ 3.14159265358979

static unsigned char *g_mask[TI_N];
static int g_maskSz;
static HWND g_tip;

typedef struct { HDC dc; unsigned *bits; int big; double u; } Canvas;

#define PX(v) ((int)((v) * c->u + 0.5))

static HPEN stroke(Canvas *c, double w)
{
  LOGBRUSH lb;
  lb.lbStyle = BS_SOLID; lb.lbColor = RGB(255, 255, 255); lb.lbHatch = 0;
  return ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND,
                      (DWORD)(w * c->u + 0.5), &lb, 0, NULL);
}

static void poly(Canvas *c, const double *xy, int n, int filled, double w)
{
  POINT p[40];
  int i;
  HPEN pen = stroke(c, w), op;
  HBRUSH br = (HBRUSH)GetStockObject(filled ? WHITE_BRUSH : NULL_BRUSH), ob;

  for (i = 0; i < n && i < 40; i++) { p[i].x = PX(xy[2 * i]); p[i].y = PX(xy[2 * i + 1]); }
  op = (HPEN)SelectObject(c->dc, pen); ob = (HBRUSH)SelectObject(c->dc, br);
  Polygon(c->dc, p, n);
  SelectObject(c->dc, op); SelectObject(c->dc, ob); DeleteObject(pen);
}

static void polyline(Canvas *c, const double *xy, int n, double w)
{
  POINT p[16];
  int i;
  HPEN pen = stroke(c, w), op = (HPEN)SelectObject(c->dc, pen);

  for (i = 0; i < n && i < 16; i++) { p[i].x = PX(xy[2 * i]); p[i].y = PX(xy[2 * i + 1]); }
  Polyline(c->dc, p, n);
  SelectObject(c->dc, op); DeleteObject(pen);
}

static void line(Canvas *c, double x0, double y0, double x1, double y1, double w)
{
  double xy[4];
  xy[0] = x0; xy[1] = y0; xy[2] = x1; xy[3] = y1;
  polyline(c, xy, 2, w);
}

static void ellipse(Canvas *c, double cx, double cy, double r, int filled, double w)
{
  HPEN pen = stroke(c, w), op;
  HBRUSH br = (HBRUSH)GetStockObject(filled ? WHITE_BRUSH : NULL_BRUSH), ob;

  op = (HPEN)SelectObject(c->dc, pen); ob = (HBRUSH)SelectObject(c->dc, br);
  Ellipse(c->dc, PX(cx - r), PX(cy - r), PX(cx + r), PX(cy + r));
  SelectObject(c->dc, op); SelectObject(c->dc, ob); DeleteObject(pen);
}

static void roundrect(Canvas *c, double x0, double y0, double x1, double y1,
                      double rad, int filled, double w)
{
  HPEN pen = stroke(c, w), op;
  HBRUSH br = (HBRUSH)GetStockObject(filled ? WHITE_BRUSH : NULL_BRUSH), ob;

  op = (HPEN)SelectObject(c->dc, pen); ob = (HBRUSH)SelectObject(c->dc, br);
  RoundRect(c->dc, PX(x0), PX(y0), PX(x1), PX(y1), PX(rad * 2), PX(rad * 2));
  SelectObject(c->dc, op); SelectObject(c->dc, ob); DeleteObject(pen);
}

static void arc(Canvas *c, double cx, double cy, double r, double a0, double a1, double w)
{
  HPEN pen = stroke(c, w), op = (HPEN)SelectObject(c->dc, pen);
  Arc(c->dc, PX(cx - r), PX(cy - r), PX(cx + r), PX(cy + r),
      PX(cx + r * cos(a0)), PX(cy + r * sin(a0)), PX(cx + r * cos(a1)), PX(cy + r * sin(a1)));
  SelectObject(c->dc, op); DeleteObject(pen);
}

/* The icons, on a 24 x 24 grid. */
static void draw_icon(Canvas *c, int icon)
{
  double p[16];
  int i;

  switch (icon)
  {
    case TI_OPEN:
      p[0] = 3;  p[1] = 6.5; p[2] = 9.5; p[3] = 6.5; p[4] = 11.5; p[5] = 9;
      p[6] = 21; p[7] = 9;   p[8] = 21;  p[9] = 19;  p[10] = 3;   p[11] = 19;
      poly(c, p, 6, 0, 1.9);
      break;

    case TI_REFRESH:   /* Lucide "RefreshCw": two arcs with arrowheads, clockwise */
    {
      double d = PI_ / 180.0;
      arc(c, 12, 12, 9, 317.1 * d, 180 * d, 2.0);
      line(c, 18.58, 5.87, 21, 8, 2.0);
      p[0] = 21; p[1] = 3; p[2] = 21; p[3] = 8; p[4] = 16; p[5] = 8;
      polyline(c, p, 3, 2.0);
      arc(c, 12, 12, 9, 137.1 * d, 0, 2.0);
      line(c, 5.42, 18.13, 3, 16, 2.0);
      p[0] = 8; p[1] = 16; p[2] = 3; p[3] = 16; p[4] = 3; p[5] = 21;
      polyline(c, p, 3, 2.0);
      break;
    }

    case TI_RESET:     /* Lucide "RotateCcw": one counterclockwise arc with an arrowhead */
    {
      double d = PI_ / 180.0;
      arc(c, 12, 12, 9, 180 * d, -137.1 * d, 2.0);
      line(c, 5.42, 5.87, 3, 8, 2.0);
      p[0] = 3; p[1] = 3; p[2] = 3; p[3] = 8; p[4] = 8; p[5] = 8;
      polyline(c, p, 3, 2.0);
      break;
    }

    case TI_PLAY:
      p[0] = 7.5; p[1] = 4.5; p[2] = 19.5; p[3] = 12; p[4] = 7.5; p[5] = 19.5;
      poly(c, p, 3, 1, 1.4);
      break;

    case TI_FULLSCREEN:
    {
      static const double k[4][6] = { {4, 9.5, 4, 4, 9.5, 4}, {14.5, 4, 20, 4, 20, 9.5},
                                      {20, 14.5, 20, 20, 14.5, 20}, {9.5, 20, 4, 20, 4, 14.5} };
      for (i = 0; i < 4; i++) polyline(c, k[i], 3, 2.2);
      break;
    }

    case TI_SETTINGS:   /* a gear: eight teeth around a ring */
    {
      double g[64];
      int t, k = 0;

      for (t = 0; t < 8; t++)
      {
        double base = t * 45.0, off[4] = { -10, -6.5, 6.5, 10 }, rad[4] = { 7.4, 10.2, 10.2, 7.4 };
        int j;
        for (j = 0; j < 4; j++)
        {
          double a = (base + off[j] - 90.0) * PI_ / 180.0;
          g[k++] = 12 + rad[j] * cos(a);
          g[k++] = 12 + rad[j] * sin(a);
        }
      }
      poly(c, g, 32, 0, 1.8);
      ellipse(c, 12, 12, 3.4, 0, 1.8);
      break;
    }

    case TI_STOP:
      roundrect(c, 6, 6, 18, 18, 1.2, 1, 1.0);
      break;

    case TI_PAUSE:
      roundrect(c, 6.5, 5, 10.5, 19, 0.8, 1, 0.8);
      roundrect(c, 13.5, 5, 17.5, 19, 0.8, 1, 0.8);
      break;

    case TI_SCREENSHOT:   /* a camera */
      p[0] = 8.5; p[1] = 6.5; p[2] = 10; p[3] = 4.5; p[4] = 14; p[5] = 4.5; p[6] = 15.5; p[7] = 6.5;
      polyline(c, p, 4, 1.8);
      roundrect(c, 2.5, 6.5, 21.5, 19.5, 2.2, 0, 2.0);
      ellipse(c, 12, 13, 3.6, 0, 1.9);
      break;

    case TI_VIDEO:   /* a monitor */
      roundrect(c, 2.5, 4.5, 21.5, 16, 1.8, 0, 2.0);
      line(c, 12, 16, 12, 19.5, 2.0);
      line(c, 7.5, 19.5, 16.5, 19.5, 2.0);
      break;

    case TI_AUDIO:   /* a speaker with sound waves */
    {
      static const double body[12] = { 3.5, 9.5, 7.5, 9.5, 12.5, 5, 12.5, 19, 7.5, 14.5, 3.5, 14.5 };
      static const double w1[6] = { 15.5, 9.2, 17, 12, 15.5, 14.8 };
      static const double w2[6] = { 18.3, 6.2, 20.8, 12, 18.3, 17.8 };
      poly(c, body, 6, 0, 1.8);
      polyline(c, w1, 3, 1.8);
      polyline(c, w2, 3, 1.8);
      break;
    }

    case TI_FAV:     /* a star, outlined */
    case TI_FAVON:   /* a star, filled: the Favorites Only switch while it is on */
    {
      double s[20];
      for (i = 0; i < 10; i++)
      {
        double a = (-90 + i * 36) * PI_ / 180, r = (i & 1) ? 4.6 : 10.2;
        s[2 * i] = 12 + r * cos(a);
        s[2 * i + 1] = 12.9 + r * sin(a);
      }
      poly(c, s, 10, icon == TI_FAVON, icon == TI_FAVON ? 0.8 : 1.9);
      break;
    }

    case TI_SEARCH:   /* a magnifying glass */
    {
      double d = PI_ / 180.0;
      arc(c, 10, 10, 6.5, 0, 180 * d, 2.0);
      arc(c, 10, 10, 6.5, 180 * d, 360 * d, 2.0);
      line(c, 15, 15, 21, 21, 2.2);
      break;
    }

    case TI_CONTROLS:   /* a game pad */
      roundrect(c, 2.5, 6.5, 21.5, 17.5, 5.2, 0, 2.0);
      line(c, 5.6, 12, 10.4, 12, 1.9);
      line(c, 8, 9.6, 8, 14.4, 1.9);
      ellipse(c, 15.4, 10.6, 1.3, 1, 0.3);
      ellipse(c, 18.0, 13.4, 1.3, 1, 0.3);
      break;
  }
}

/* The alpha mask of an icon: sz x sz bytes. */
static unsigned char *icon_mask(int icon, int sz)
{
  Canvas c;
  BITMAPINFO bi;
  HBITMAP bmp, old;
  unsigned char *m;
  int x, y, sx, sy;

  memset(&c, 0, sizeof(c));
  c.big = sz * SS;
  c.u = (double)c.big / 24.0;

  memset(&bi, 0, sizeof(bi));
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = c.big;
  bi.bmiHeader.biHeight = -c.big;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;

  c.dc = CreateCompatibleDC(NULL);
  bmp = CreateDIBSection(c.dc, &bi, DIB_RGB_COLORS, (void **)&c.bits, NULL, 0);
  if (!bmp) { DeleteDC(c.dc); return NULL; }

  old = (HBITMAP)SelectObject(c.dc, bmp);
  memset(c.bits, 0, (size_t)c.big * c.big * 4);
  draw_icon(&c, icon);
  GdiFlush();

  m = (unsigned char *)malloc((size_t)sz * sz);
  if (m)
  {
    for (y = 0; y < sz; y++)
      for (x = 0; x < sz; x++)
      {
        int sum = 0;
        for (sy = 0; sy < SS; sy++)
          for (sx = 0; sx < SS; sx++)
            sum += c.bits[(y * SS + sy) * c.big + x * SS + sx] & 0xFF;
        m[y * sz + x] = (unsigned char)(sum / (SS * SS));
      }
  }

  SelectObject(c.dc, old);
  DeleteObject(bmp);
  DeleteDC(c.dc);
  return m;
}

static unsigned char *mask_for(int icon, int sz)
{
  if (sz != g_maskSz)
  {
    int i;
    for (i = 0; i < TI_N; i++) { free(g_mask[i]); g_mask[i] = NULL; }
    g_maskSz = sz;
  }
  if (!g_mask[icon]) g_mask[icon] = icon_mask(icon, sz);
  return g_mask[icon];
}

void tb_paint_icon(HDC dc, int x, int y, int sz, int icon, COLORREF color)
{
  unsigned char *mk = mask_for(icon, sz);
  BITMAPINFO bi;
  unsigned *bits = NULL;
  HDC mdc;
  HBITMAP bmp, old;
  int i, r = GetRValue(color), g = GetGValue(color), b = GetBValue(color);
  BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };

  if (!mk) return;
  mdc = CreateCompatibleDC(dc);
  memset(&bi, 0, sizeof(bi));
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = sz;
  bi.bmiHeader.biHeight = -sz;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  bmp = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
  if (bmp)
  {
    old = (HBITMAP)SelectObject(mdc, bmp);
    for (i = 0; i < sz * sz; i++)   /* premultiplied, as AlphaBlend wants */
    {
      unsigned a = mk[i];
      bits[i] = (a << 24) | ((unsigned)(r * a / 255) << 16) | ((unsigned)(g * a / 255) << 8) | (unsigned)(b * a / 255);
    }
    AlphaBlend(dc, x, y, sz, sz, mdc, 0, 0, sz, sz, bf);
    SelectObject(mdc, old);
    DeleteObject(bmp);
  }
  DeleteDC(mdc);
}

/* Sizes: the icon is 20, 26 or 32 units; a label adds a line under it. */
static int tb_icon_px(void)
{
  static const int px[3] = { 20, 26, 32 };
  int s = gui.toolbar_size;
  return TBS(px[s < 0 || s > 2 ? 0 : s]);
}

/* The line under the icon: room for the font in use (The Large interface has a taller one, whose descenders - the g of Settings - were cut). */
static int tb_label_h(void)
{
  int h = TBS(15);
  HDC dc;
  TEXTMETRICA tm;
  HGDIOBJ of;

  if (!gui.toolbar_labels) return 0;
  dc = GetDC(NULL);
  if (dc)
  {
    of = SelectObject(dc, gui_get_ui_font());
    if (GetTextMetricsA(dc, &tm) && tm.tmHeight + 3 > h) h = tm.tmHeight + 3;
    SelectObject(dc, of);
    ReleaseDC(NULL, dc);
  }
  return h;
}

int tb_button_h(void) { return tb_icon_px() + TBS(12) + tb_label_h(); }

/* The bar's height for that many rows of buttons (the buttons wrap onto more rows in a narrow window, like a menu bar). */
int tb_bar_h_rows(int rows)
{
  if (rows < 1) rows = 1;
  return rows * tb_button_h() + (rows - 1) * TBS(2) + TBS(8);
}

void tb_set_label(HWND b, const char *label) { if (b) SetPropA(b, "tblabel", (HANDLE)label); }

int tb_button_w(HWND b)
{
  int w = tb_icon_px() + TBS(16);
  const char *t = b ? (const char *)GetPropA(b, "tblabel") : NULL;

  if (gui.toolbar_labels && t)
  {
    HDC dc = GetDC(NULL);
    SIZE sz;
    HGDIOBJ of = dc ? SelectObject(dc, gui_get_ui_font()) : NULL;
    if (dc && GetTextExtentPoint32A(dc, t, lstrlenA(t), &sz) && sz.cx + TBS(14) > w) w = sz.cx + TBS(14);
    if (dc) { SelectObject(dc, of); ReleaseDC(NULL, dc); }
  }
  return w;
}

/* Theme colors. */
COLORREF tb_icon_color(int icon)
{
  int dark = theme_is_dark();

  switch (icon)
  {
    case TI_OPEN:       return dark ? RGB(0xF5, 0xB8, 0x3D) : RGB(0xB9, 0x78, 0x00);
    case TI_REFRESH:
    case TI_PLAY:       return dark ? RGB(0x4C, 0xC7, 0x64) : RGB(0x1E, 0x8A, 0x3A);
    case TI_STOP:       return dark ? RGB(0xFF, 0x6B, 0x6B) : RGB(0xC6, 0x28, 0x28);
    case TI_RESET:
    case TI_PAUSE:      return dark ? RGB(0xFF, 0xA9, 0x4D) : RGB(0xC4, 0x6A, 0x00);
    case TI_FULLSCREEN:
    case TI_VIDEO:      return dark ? RGB(0x38, 0xBD, 0xF8) : RGB(0x03, 0x69, 0xA1);
    case TI_SCREENSHOT: return dark ? RGB(0xFF, 0x5C, 0x7A) : RGB(0xC0, 0x26, 0x4A);
    case TI_SETTINGS:   return dark ? RGB(0x2D, 0xD4, 0xBF) : RGB(0x0F, 0x76, 0x6E);
    case TI_AUDIO:      return dark ? RGB(0xF4, 0x72, 0xB6) : RGB(0xB0, 0x30, 0x6E);
    case TI_CONTROLS:   return dark ? RGB(0xA3, 0xE6, 0x35) : RGB(0x5C, 0x8A, 0x00);
    case TI_FAV:
    case TI_FAVON:      return dark ? RGB(0xFA, 0xCC, 0x15) : RGB(0xA1, 0x62, 0x07);
    case TI_SEARCH:     return dark ? RGB(0x4D, 0xA3, 0xFF) : RGB(0x0B, 0x62, 0xC4);
  }
  return dark ? RGB(0x7A, 0xA2, 0xF7) : RGB(0x3B, 0x63, 0xC9);
}

COLORREF tb_face_color(void)
{
  return theme_is_dark() ? RGB(32, 32, 32) : GetSysColor(COLOR_BTNFACE);
}
static COLORREF text_color(void) { return theme_is_dark() ? RGB(240, 240, 240) : GetSysColor(COLOR_BTNTEXT); }
static COLORREF gray_color(void) { return theme_is_dark() ? RGB(120, 120, 120) : GetSysColor(COLOR_GRAYTEXT); }

/****************************************************************************
 * The buttons
 ****************************************************************************/

static LRESULT CALLBACK tb_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref)
{
  (void)id; (void)ref;

  switch (m)
  {
    case WM_MOUSEMOVE:
      if (!GetPropA(h, "tbhot"))
      {
        TRACKMOUSEEVENT te;
        te.cbSize = sizeof(te); te.dwFlags = TME_LEAVE; te.hwndTrack = h; te.dwHoverTime = 0;
        TrackMouseEvent(&te);
        SetPropA(h, "tbhot", (HANDLE)1);
        InvalidateRect(h, NULL, FALSE);
      }
      break;

    case WM_MOUSELEAVE:
      SetPropA(h, "tbhot", (HANDLE)0);
      InvalidateRect(h, NULL, FALSE);
      break;

    case WM_ENABLE:
      InvalidateRect(h, NULL, FALSE);
      break;

    case WM_NCDESTROY:
      RemovePropA(h, "tbico"); RemovePropA(h, "tbhot"); RemovePropA(h, "tbchk"); RemovePropA(h, "tblabel");
      RemoveWindowSubclass(h, tb_proc, 99);
      break;
  }
  return DefSubclassProc(h, m, w, l);
}

HWND tb_button(HWND parent, int id, int icon, const char *tip)
{
  HWND b = CreateWindowExA(0, "BUTTON", "", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
                           0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, g_inst, NULL);
  if (!b) return NULL;

  SetPropA(b, "tbico", (HANDLE)(INT_PTR)(icon + 1));
  SetWindowSubclass(b, tb_proc, 99, 0);

  if (tip)
  {
    TOOLINFOA ti;

    if (!g_tip)
      g_tip = CreateWindowExA(WS_EX_TOPMOST, TOOLTIPS_CLASSA, NULL,
                              WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                              0, 0, 0, 0, parent, NULL, g_inst, NULL);
    if (g_tip)
    {
      memset(&ti, 0, sizeof(ti));
      ti.cbSize = sizeof(ti);
      ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
      ti.hwnd = parent;
      ti.uId = (UINT_PTR)b;
      ti.lpszText = (LPSTR)tip;
      SendMessageA(g_tip, TTM_ADDTOOLA, 0, (LPARAM)&ti);
    }
  }
  return b;
}

/* Changes the tooltip text of a toolbar button (the shortcut in it follows the Hotkeys settings). */
void tb_set_tip(HWND b, const char *tip)
{
  TOOLINFOA ti;

  if (!g_tip || !b || !tip) return;
  memset(&ti, 0, sizeof(ti));
  ti.cbSize = sizeof(ti);
  ti.hwnd = GetParent(b);
  ti.uId = (UINT_PTR)b;
  ti.uFlags = TTF_IDISHWND;
  ti.lpszText = (LPSTR)tip;
  SendMessageA(g_tip, TTM_UPDATETIPTEXTA, 0, (LPARAM)&ti);
}

HWND tb_separator(HWND parent)
{
  HWND s = CreateWindowExA(0, "STATIC", "", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
                           0, 0, 4, 4, parent, NULL, g_inst, NULL);
  if (s) SetPropA(s, "tbsep", (HANDLE)1);
  return s;
}

void tb_set_checked(HWND b, int on)
{
  if (!b) return;
  if ((GetPropA(b, "tbchk") != NULL) == (on != 0)) return;
  SetPropA(b, "tbchk", (HANDLE)(INT_PTR)(on ? 1 : 0));
  InvalidateRect(b, NULL, FALSE);
}

int tb_draw(const DRAWITEMSTRUCT *di)
{
  if (!di) return 0;

  if (di->CtlType == ODT_STATIC && GetPropA(di->hwndItem, "tbsep"))
  {
    RECT r = di->rcItem;
    int x = (r.left + r.right) / 2;
    int pad = (r.bottom - r.top) / 5;
    RECT l;
    HBRUSH face = CreateSolidBrush(tb_face_color()), ln = CreateSolidBrush(gray_color());

    l.left = x; l.right = x + 1; l.top = r.top + pad; l.bottom = r.bottom - pad;
    FillRect(di->hDC, &r, face);
    FillRect(di->hDC, &l, ln);
    DeleteObject(face); DeleteObject(ln);
    return 1;
  }

  if (di->CtlType == ODT_BUTTON && GetPropA(di->hwndItem, "tbico"))
  {
    int icon = (int)(INT_PTR)GetPropA(di->hwndItem, "tbico") - 1;
    int hot = GetPropA(di->hwndItem, "tbhot") != NULL;
    int chk = GetPropA(di->hwndItem, "tbchk") != NULL;
    int dis = (di->itemState & ODS_DISABLED) != 0;
    int down = (di->itemState & ODS_SELECTED) != 0;
    int dark = theme_is_dark();
    RECT r = di->rcItem;
    int sz = tb_icon_px(), lh = tb_label_h(), icon_y, x, y;
    COLORREF face = tb_face_color(), bg = face, fg = dis ? gray_color() : text_color();
    unsigned char *mk;
    HBRUSH hb;

    if (icon == TI_FAV && chk) icon = TI_FAVON;   /* the star fills in while Favorites Only is on */
    if (gui.color_icons && !dis) fg = tb_icon_color(icon);       /* Settings > General > Colorful Icons */

    if (!dis && (hot || down || chk))
    {
      if (gui.ui_style)      /* Modern: softer plates with a hint of blue */
      {
        if (down)      bg = dark ? RGB(70, 76, 92) : RGB(205, 214, 230);
        else if (hot)  bg = dark ? RGB(56, 60, 72) : RGB(224, 230, 241);
        else           bg = dark ? RGB(50, 58, 78) : RGB(214, 224, 242);
      }
      else if (down) bg = dark ? RGB(92, 92, 92) : RGB(196, 196, 196);
      else if (hot)  bg = dark ? RGB(68, 68, 68) : RGB(222, 222, 222);
      else           bg = dark ? RGB(62, 62, 62) : RGB(214, 214, 214);
    }

    hb = CreateSolidBrush(face);
    FillRect(di->hDC, &r, hb);
    DeleteObject(hb);

    if (bg != face)   /* a soft rounded plate under the icon */
    {
      HBRUSH pb = CreateSolidBrush(bg);
      HGDIOBJ ob = SelectObject(di->hDC, pb), op = SelectObject(di->hDC, GetStockObject(NULL_PEN));
      RoundRect(di->hDC, r.left + 1, r.top + 1, r.right - 1, r.bottom - 1, TBS(8), TBS(8));
      SelectObject(di->hDC, ob); SelectObject(di->hDC, op);
      DeleteObject(pb);
    }

    mk = mask_for(icon, sz);
    if (!mk) return 1;
    icon_y = r.top + (r.bottom - r.top - sz - lh) / 2;

    {
      BITMAPINFO bi;
      unsigned *bits = NULL;
      HDC mdc = CreateCompatibleDC(di->hDC);
      HBITMAP bmp, old;

      memset(&bi, 0, sizeof(bi));
      bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
      bi.bmiHeader.biWidth = sz;
      bi.bmiHeader.biHeight = -sz;
      bi.bmiHeader.biPlanes = 1;
      bi.bmiHeader.biBitCount = 32;
      bi.bmiHeader.biCompression = BI_RGB;

      bmp = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
      if (bmp)
      {
        int br = GetRValue(bg), bgc = GetGValue(bg), bb = GetBValue(bg);
        int fr = GetRValue(fg), fgc = GetGValue(fg), fb = GetBValue(fg);

        old = (HBITMAP)SelectObject(mdc, bmp);
        for (y = 0; y < sz; y++)
          for (x = 0; x < sz; x++)
          {
            int a = mk[y * sz + x];
            int rr = (br * (255 - a) + fr * a) / 255;
            int gg = (bgc * (255 - a) + fgc * a) / 255;
            int b2 = (bb * (255 - a) + fb * a) / 255;
            bits[y * sz + x] = ((unsigned)rr << 16) | ((unsigned)gg << 8) | (unsigned)b2;
          }
        BitBlt(di->hDC, r.left + (r.right - r.left - sz) / 2, icon_y,
               sz, sz, mdc, 0, 0, SRCCOPY);
        SelectObject(mdc, old);
        DeleteObject(bmp);
      }
      DeleteDC(mdc);
    }

    if (lh)      /* the name of the button, under its icon */
    {
      const char *t = (const char *)GetPropA(di->hwndItem, "tblabel");
      if (t)
      {
        RECT tr;
        HGDIOBJ of = SelectObject(di->hDC, gui_get_ui_font());
        tr.left = r.left; tr.right = r.right; tr.top = icon_y + sz + TBS(1); tr.bottom = tr.top + lh;
        SetBkMode(di->hDC, TRANSPARENT);
        SetTextColor(di->hDC, fg);
        DrawTextA(di->hDC, t, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(di->hDC, of);
      }
    }
    return 1;
  }

  return 0;
}
