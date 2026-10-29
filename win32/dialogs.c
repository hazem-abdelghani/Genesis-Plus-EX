/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  dialogs.c -- the modal dialogs reachable from the menu.
 ****************************************************************************/

#include <windows.h>
#include <mmsystem.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <commctrl.h>
#include <richedit.h>
#include <shellapi.h>
#include <commdlg.h>
#define COBJMACROS
#include <shobjidl.h>

#include <uxtheme.h>
#include <zlib.h>

#include "shared.h"
#include "gui.h"
#include "picocore.h"
#include "resource.h"
#include "theme.h"
#include "netplay.h"
#include "toolbar.h"
#include "eggs.h"
#include "stb_image.h"

/****************************************************************************
 * Tooltips for the dialogs: one tooltip window per dialog, hiding itself
 * after 30 seconds. A tip goes on a control by id, and/or on the plain text
 * label (a static with that text) next to it.
 ****************************************************************************/

#define TipDef GuiTip
#define dlg_add_tips gui_dialog_tips

static void dlg_add_tip(HWND tt, HWND dlg, HWND ctl, const char *text)
{
  TOOLINFOA ti;

  if (!ctl || !text) return;
  memset(&ti, 0, sizeof(ti));
  ti.cbSize = sizeof(ti);
  ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
  ti.hwnd = dlg;
  ti.uId = (UINT_PTR)ctl;
  ti.lpszText = (LPSTR)text;
  SendMessageA(tt, TTM_ADDTOOLA, 0, (LPARAM)&ti);
}

void gui_dialog_tips(HWND dlg, const GuiTip *defs, int n)
{
  HWND tt = CreateWindowExA(WS_EX_TOPMOST, TOOLTIPS_CLASSA, NULL, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                            0, 0, 0, 0, dlg, NULL, GetModuleHandle(NULL), NULL);
  int i;

  if (!tt) return;
  SendMessage(tt, TTM_SETMAXTIPWIDTH, 0, TBS(320));
  SendMessage(tt, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);
  SendMessage(tt, TTM_SETDELAYTIME, TTDT_INITIAL, 400);
  SendMessage(tt, WM_SETFONT, SendMessage(dlg, WM_GETFONT, 0, 0), FALSE);

  for (i = 0; i < n; i++)
  {
    if (defs[i].id) dlg_add_tip(tt, dlg, GetDlgItem(dlg, defs[i].id), defs[i].tip);
    if (defs[i].label)
    {
      HWND c;
      for (c = GetWindow(dlg, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
      {
        char cls[16], txt[96];
        GetClassNameA(c, cls, sizeof(cls));
        if (lstrcmpiA(cls, "Static") != 0) continue;
        GetWindowTextA(c, txt, sizeof(txt));
        { char *a, *o; for (a = o = txt; *a; a++) if (*a != '&') *o++ = *a; *o = 0; }       /* ("&Name:" is "Name:" here) */
        if (lstrcmpA(txt, defs[i].label) != 0) continue;
        SetWindowLongPtr(c, GWL_STYLE, GetWindowLongPtr(c, GWL_STYLE) | SS_NOTIFY);   /* so it gets the mouse */
        dlg_add_tip(tt, dlg, c, defs[i].tip);
      }
    }
  }
}

/****************************************************************************
 * Input configuration
 *
 * Assigning works by listening rather than by asking the user to pick from a
 * list: choose a button, press what you want it to be. The same listening
 * pass covers the keyboard and the gamepad, so there is no separate mode to
 * switch into for controllers.
 ****************************************************************************/

/* Listening state machine */
enum { LISTEN_OFF = 0, LISTEN_WAIT_RELEASE, LISTEN_ACTIVE };

static struct
{
  int       player;     /* the tab showing */
  int       sel;        /* the mapping being assigned */
  int       test;       /* Test Keys is ticked: the keyboard only lights spots, it does not operate the window */
  unsigned  live;       /* bit per pad button: its key or gamepad button is held right now */
  HBITMAP   picture;    /* the controller, scaled to pic */
  HFONT     font;       /* the dialog font, 21% larger */
  RECT      pic;        /* where it is drawn, in dialog client coordinates */
  struct { RECT rc; UINT fmt; char text[32]; } label[16];   /* the names beside the buttons: painted, not controls, so they never cover the picture */
  int       labels;
  struct { t_pad_map map; uint8 padtype; int deadzone; } other[2];   /* each player's working copy while its tab is not showing */
  t_pad_map map;        /* working copy of the showing player, committed on Save */
  uint8     padtype;
  int       deadzone;   /* working copy, 0-100% */
  int       listen;
  int       listen_target;  /* which row listening is for, fixed at start */
  DWORD     listen_deadline;  /* GetTickCount() value; listening cancels itself past this */
  char      notice[300];        /* what the last assignment displaced or collides with, shown as the hint */
  int       listen_shown_secs;  /* countdown value the hint currently shows, -1 forces a redraw */
  int       shown_dev;      /* the device the list shows: -1 also while the saved pad is not connected */
  int       dev_shadow;     /* the saved device is not in the list: the list shows Keyboard Only, the saved one is kept until another is picked */
  int       auto_advance;   /* set by Auto Assign only; Assign/double-click leave it off */
  HWND      dlg;
  int       ports;          /* the Extras tab is showing */
  int       port_open[2];   /* the port devices when the window opened: Cancel puts them back, they apply at once */
} inp;

/* Shared by every dialog with a slider-plus-percentage-readout control
   (Audio Levels and Latency, and this one) -- sets the slider's range and
   starting position and writes the matching text into its paired readout,
   all in one call instead of five repeated at every call site. */
static void audio_set_slider(HWND dlg, int id, int text_id, int lo, int hi,
                             int value, const char *suffix)
{
  HWND bar = GetDlgItem(dlg, id);
  char buf[32];

  SendMessage(bar, TBM_SETRANGE, TRUE, MAKELPARAM(lo, hi));
  {
    /* About eight ticks, spaced so that the last one is the end of the range (a step that does not divide it leaves two ticks side by side there). */
    int step = (hi - lo) / 8;
    if (step < 1) step = 1;
    while (step > 1 && (hi - lo) % step) step--;
    SendMessage(bar, TBM_SETTICFREQ, (WPARAM)step, 0);
  }
  SendMessage(bar, TBM_SETPOS, TRUE, (LPARAM)value);

  wsprintfA(buf, "%d%s", value, suffix);
  SetDlgItemTextA(dlg, text_id, buf);
}

static int input_list_count(void)
{
  return PAD_KEYS;
}

static const char *input_list_entry_name(int sel)
{
  return input_button_label(sel);
}

/* The Large interface draws this window 21% bigger than the other dialogs; the normal UI draws it at the dialogs' own size. */
#define IS(v) (gui.large_ui ? TBS(v) * 121 / 100 : DPIS(v))     /* larger UI keeps the old size; the normal one is smaller */

/* The credits, as the About window and the first page of the User Guide show them. */
#define CREDITS_TEXT "2026 - Genesis Plus EX by Hazem Abdelghani\r\n" \
                     "2007-2026 - Genesis Plus GX by Eke-Eke\r\n" \
                     "1998-2003 - Genesis Plus by Charles MacDonald\r\n" \
                     "2004-2026 - PicoDrive (32X) by Dave, notaz, irixxxx and others\r\n"

#define INPUT_HINT_BASE ""     /* (the explanation is in the tooltips; this line shows messages: profiles, assigning keys, test mode) */
#define INPUT_HINT_TEST "Test mode: press any key or pad button and its spot lights. Keys do nothing else here. Click Test Keys again to leave."
#define INPUT_HINT_IDLE (inp.test ? INPUT_HINT_TEST : INPUT_HINT_BASE)

/* Each mapping is a button on the controller picture's sides, with its name
   written beside it; the button shows what it is set to. */
static void input_fill_list(HWND dlg)
{
  int i, count = input_list_count();

  for (i = 0; i < count; i++)
  {
    char line[160];
    int key, button, dev;

    key = inp.map.key[i]; button = inp.map.button[i]; dev = inp.shown_dev;

    if (inp.listen != LISTEN_OFF && i == inp.listen_target)
    {
      {
        int ms = (int)(inp.listen_deadline - GetTickCount());
        wsprintfA(line, "[Waiting %ds]", ms > 0 ? (ms + 999) / 1000 : 0);
      }
    }
    else
      /* Only what the chosen device uses: the key for "Keyboard Only", the pad button for a gamepad. */
      lstrcpyA(line, dev < 0 ? input_key_name(key) : input_pad_button_name(dev, button));

    /* On an XInput pad the left stick is folded into the D-pad: a direction on the D-pad is both. */
    if (dev >= 0 && dev < GUI_INPUT_XPAD_COUNT && i >= PAD_UP && i <= PAD_RIGHT &&
        !(inp.listen != LISTEN_OFF && i == inp.listen_target) && button == (1 << i))
    {
      static const char *const dir[4] = { "Up", "Down", "Left", "Right" };
      wsprintfA(line, "D-Pad %s / LS %s", dir[i], dir[i]);
    }
    SetDlgItemTextA(dlg, IDC_INPUT_LABEL_BASE + i, line);
    if (((0x3F0u >> i) & 1u))
      CheckDlgButton(dlg, IDC_INPUT_TURBO_BASE + i, (inp.map.turbo & (1u << i)) ? BST_CHECKED : BST_UNCHECKED);
  }
}

/* Where each button sits on the picture, in thousandths of its width/height. */
static const short input_hotspot[PAD_KEYS][2] =
{
  { 247, 326 }, { 247, 524 }, { 198, 425 }, { 298, 427 },   /* Up Down Left Right */
  { 669, 595 }, { 760, 498 }, { 858, 439 },                 /* A B C */
  { 635, 394 }, { 712, 318 }, { 796, 280 },                 /* X Y Z */
  { 485, 380 }, { 754, 73 }                                 /* Start, Mode (on the top edge) -- placed by hand on the 560 x 288 picture */
};

static RECT input_pic_rc(void)
{
  return inp.pic;
}

static void input_redraw_picture(HWND dlg)
{
  RECT rc = input_pic_rc();
  InflateRect(&rc, IS(14), IS(14));
  InvalidateRect(dlg, &rc, FALSE);      /* no erase: the paint fills its own background, which avoids flicker */
}

/* The controller, scaled once to the size it is shown at. It is stored
   compressed and premultiplied (pad.bin), so it blends onto either theme. */
static HBITMAP input_make_picture(int dw, int dh)
{
  HRSRC res = FindResourceA(NULL, MAKEINTRESOURCEA(IDR_PADIMG), (LPCSTR)RT_RCDATA);
  const unsigned char *src;
  unsigned char *raw, *dst;
  BITMAPINFO bi;
  HBITMAP bmp = NULL;
  int sw, sh, x, y;
  uLongf rawlen;

  if (!res) return NULL;
  src = (const unsigned char *)LockResource(LoadResource(NULL, res));
  if (!src) return NULL;
  sw = ((const int *)src)[0];
  sh = ((const int *)src)[1];
  rawlen = (uLongf)sw * sh * 4;
  raw = (unsigned char *)malloc(rawlen);
  if (!raw) return NULL;
  if (uncompress(raw, &rawlen, src + 8, SizeofResource(NULL, res) - 8) != Z_OK) { free(raw); return NULL; }

  ZeroMemory(&bi, sizeof(bi));
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = dw;
  bi.bmiHeader.biHeight = -dh;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&dst, NULL, 0);
  if (bmp)
  {
    for (y = 0; y < dh; y++)
    {
      int y0 = y * sh / dh, y1 = ((y + 1) * sh + dh - 1) / dh;
      if (y1 <= y0) y1 = y0 + 1;
      if (y1 > sh) y1 = sh;
      for (x = 0; x < dw; x++)
      {
        int x0 = x * sw / dw, x1 = ((x + 1) * sw + dw - 1) / dw;
        int sx, sy, n = 0, c[4] = { 0, 0, 0, 0 };
        if (x1 <= x0) x1 = x0 + 1;
        if (x1 > sw) x1 = sw;
        for (sy = y0; sy < y1; sy++)
          for (sx = x0; sx < x1; sx++)
          {
            const unsigned char *p = raw + ((size_t)sy * sw + sx) * 4;
            c[0] += p[0]; c[1] += p[1]; c[2] += p[2]; c[3] += p[3];
            n++;
          }
        dst[((size_t)y * dw + x) * 4 + 0] = (unsigned char)(c[0] / n);
        dst[((size_t)y * dw + x) * 4 + 1] = (unsigned char)(c[1] / n);
        dst[((size_t)y * dw + x) * 4 + 2] = (unsigned char)(c[2] / n);
        dst[((size_t)y * dw + x) * 4 + 3] = (unsigned char)(c[3] / n);
      }
    }
  }
  free(raw);
  return bmp;
}

/* The Combos tab's frames, in the dialog's coordinates (set by cb_build, drawn by input_paint). */
static RECT cb_frame[2];
static const char *const cb_frame_title[2] = { "Combos", "Controller Ports" };
static RECT pad_frame[3];                  /* the pad tabs' frames */
static const char *const pad_frame_title[3] = { "D-Pad", "System", "Buttons" };
static RECT hint_rc[2];                    /* where the message line sits: [0] on the pad tabs, [1] on the Extras tab */

/* The ring pictures: ring_gif.gif (an animation, shown on the button being assigned) and ring_png.png (a button
   held down), decoded from the program once and scaled to the ring's height; kept until the size changes. */
#define RING_MAXFRAMES 16
static HBITMAP ring_bmp[RING_MAXFRAMES + 1];       /* [0] the dot ring, [1..] the animation's frames */
static int ring_bw[RING_MAXFRAMES + 1], ring_for;
static unsigned char *ring_gif;                    /* all frames of the animation, RGBA */
static int ring_gw, ring_gh, ring_gz, ring_delay[RING_MAXFRAMES];

static const unsigned char *ring_res(int id, int *len)
{
  HRSRC res = FindResourceA(NULL, MAKEINTRESOURCEA(id), (LPCSTR)RT_RCDATA);
  const unsigned char *src;
  if (!res) return NULL;
  src = (const unsigned char *)LockResource(LoadResource(NULL, res));
  *len = (int)SizeofResource(NULL, res);
  return src;
}

static void ring_load_gif(void)
{
  int len, *delays = NULL, ch, i;
  const unsigned char *src;

  if (ring_gif || ring_gz < 0) return;
  ring_gz = -1;                                    /* tried: not again if it fails */
  src = ring_res(IDR_RINGBLINK, &len);
  if (!src) return;
  ring_gif = stbi_load_gif_from_memory(src, len, &delays, &ring_gw, &ring_gh, &ring_gz, &ch, 4);
  if (!ring_gif) { ring_gz = -1; return; }
  if (ring_gz > RING_MAXFRAMES) ring_gz = RING_MAXFRAMES;
  /* The picture's frames cover only part of it; the rest comes out solid black (the left and top bars). Black in every
     frame is not part of the ring, so it is made clear. */
  {
    size_t px, n = (size_t)ring_gw * ring_gh;
    for (px = 0; px < n; px++)
    {
      int f, black = 1;
      for (f = 0; f < ring_gz && black; f++)
      {
        const unsigned char *q = ring_gif + (n * f + px) * 4;
        if (q[0] || q[1] || q[2] || q[3] != 255) black = 0;
      }
      if (black) for (f = 0; f < ring_gz; f++) ring_gif[(n * f + px) * 4 + 3] = 0;
    }
  }
  for (i = 0; i < ring_gz; i++)
    ring_delay[i] = delays && delays[i] >= 2 ? delays[i] * 10 : 120;      /* the GIF counts in hundredths of a second; a frame with no time set lasts 0.12 s (a browser would use 0.1) */
  if (delays) stbi_image_free(delays);
}

/* One picture (img: sw x sh RGBA, straight alpha) scaled with a box filter to dh high, premultiplied BGRA. */
HBITMAP gui_scale_rgba(const unsigned char *img, int sw, int sh, int dh, int *pw)
{
  BITMAPINFO bi;
  HBITMAP bmp;
  unsigned char *dst;
  int dw = (int)(((long long)dh * sw + sh / 2) / sh), x, y;

  if (dw < 1) dw = 1;
  ZeroMemory(&bi, sizeof(bi));
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = dw;
  bi.bmiHeader.biHeight = -dh;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&dst, NULL, 0);
  if (!bmp) return NULL;
  for (y = 0; y < dh; y++)
  {
    int y0 = y * sh / dh, y1 = ((y + 1) * sh + dh - 1) / dh;
    if (y1 <= y0) y1 = y0 + 1;
    if (y1 > sh) y1 = sh;
    for (x = 0; x < dw; x++)
    {
      int x0 = x * sw / dw, x1 = ((x + 1) * sw + dw - 1) / dw;
      int sx, sy, n = 0, c[4] = { 0, 0, 0, 0 };
      unsigned char *o = dst + ((size_t)y * dw + x) * 4;
      if (x1 <= x0) x1 = x0 + 1;
      if (x1 > sw) x1 = sw;
      for (sy = y0; sy < y1; sy++)
        for (sx = x0; sx < x1; sx++)
        {
          const unsigned char *p = img + ((size_t)sy * sw + sx) * 4;
          c[0] += p[2] * p[3]; c[1] += p[1] * p[3]; c[2] += p[0] * p[3]; c[3] += p[3];
          n++;
        }
      o[0] = (unsigned char)(c[0] / (255 * n));
      o[1] = (unsigned char)(c[1] / (255 * n));
      o[2] = (unsigned char)(c[2] / (255 * n));
      o[3] = (unsigned char)(c[3] / n);
    }
  }
  *pw = dw;
  return bmp;
}

/* k: 0 = the plain ring, 1.. = a frame of the animation. */
static void ring_draw(HDC dc, int k, int cx, int cy, int h)
{
  BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
  HDC mem;
  HGDIOBJ old;

  if (ring_for != h)
  {
    int j;
    for (j = 0; j <= RING_MAXFRAMES; j++) { if (ring_bmp[j]) DeleteObject(ring_bmp[j]); ring_bmp[j] = NULL; }
    ring_for = h;
  }
  if (!ring_bmp[k])
  {
    if (k == 0)
    {
      int len, sw, sh, ch;
      const unsigned char *src = ring_res(IDR_RING, &len);
      unsigned char *img = src ? stbi_load_from_memory(src, len, &sw, &sh, &ch, 4) : NULL;
      if (img) { ring_bmp[0] = gui_scale_rgba(img, sw, sh, h, &ring_bw[0]); stbi_image_free(img); }
    }
    else
    {
      ring_load_gif();
      if (ring_gif && k <= ring_gz)
        ring_bmp[k] = gui_scale_rgba(ring_gif + (size_t)(k - 1) * ring_gw * ring_gh * 4, ring_gw, ring_gh, h, &ring_bw[k]);
    }
  }
  if (!ring_bmp[k]) return;
  mem = CreateCompatibleDC(dc);
  old = SelectObject(mem, ring_bmp[k]);
  AlphaBlend(dc, cx - ring_bw[k] / 2, cy - h / 2, ring_bw[k], h, mem, 0, 0, ring_bw[k], h, bf);
  SelectObject(mem, old);
  DeleteDC(mem);
}

/* Which frame of the animation is showing now (1-based), and how long it has left. */
static int ring_frame(DWORD now, int *left)
{
  int i, total = 0;
  DWORD t;
  ring_load_gif();
  if (!ring_gif || ring_gz < 1) { if (left) *left = 500; return 1; }
  for (i = 0; i < ring_gz; i++) total += ring_delay[i];
  t = now % (DWORD)total;
  for (i = 0; i < ring_gz; i++)
  {
    if (t < (DWORD)ring_delay[i]) { if (left) *left = ring_delay[i] - (int)t; return i + 1; }
    t -= (DWORD)ring_delay[i];
  }
  return 1;
}

static void input_paint(HWND dlg)
{
  PAINTSTRUCT ps;
  HDC pdc = BeginPaint(dlg, &ps), dc = pdc;
  RECT rc = input_pic_rc();
  int bw = ps.rcPaint.right - ps.rcPaint.left, bh = ps.rcPaint.bottom - ps.rcPaint.top;
  HDC bdc = NULL;
  HBITMAP bbmp = NULL;
  HGDIOBJ bold = NULL;

  /* Everything is drawn into a bitmap and put on the window in one go, so the animated ring does not flicker. */
  if (bw > 0 && bh > 0 && (bdc = CreateCompatibleDC(pdc)) != NULL)
  {
    bbmp = CreateCompatibleBitmap(pdc, bw, bh);
    if (bbmp)
    {
      HBRUSH br;
      bold = SelectObject(bdc, bbmp);
      SetViewportOrgEx(bdc, -ps.rcPaint.left, -ps.rcPaint.top, NULL);
      br = (HBRUSH)SendMessage(dlg, WM_CTLCOLORDLG, (WPARAM)bdc, (LPARAM)dlg);
      FillRect(bdc, &ps.rcPaint, br ? br : GetSysColorBrush(COLOR_BTNFACE));
      dc = bdc;
    }
    else { DeleteDC(bdc); bdc = NULL; }
  }

  if (inp.ports)       /* the Extras tab: titled frames, no picture */
  {
    int k;
    for (k = 0; k < 2; k++) settings_draw_frame(dc, dlg, &cb_frame[k], cb_frame_title[k]);
    goto done;
  }
  {
    int k;
    for (k = 0; k < 3; k++) settings_draw_frame(dc, dlg, &pad_frame[k], pad_frame_title[k]);
  }

  if (inp.picture)
  {
    HDC mem = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mem, inp.picture);
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    AlphaBlend(dc, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
               mem, 0, 0, rc.right - rc.left, rc.bottom - rc.top, bf);
    SelectObject(mem, old);
    DeleteDC(mem);
  }

  /* A ring marks the button being assigned (an animation), and a ring with a dot lights for every
     mapped button or key that is held down right now. */
  {
    int i;
    for (i = 0; i < PAD_KEYS; i++)
    {
      int cx = rc.left + (rc.right - rc.left) * input_hotspot[i][0] / 1000;
      int cy = rc.top  + (rc.bottom - rc.top) * input_hotspot[i][1] / 1000;
      int r = IS(11) > (rc.right - rc.left) / 26 ? IS(11) : (rc.right - rc.left) / 26;      /* a ring that grows with the picture */

      if (inp.listen != LISTEN_OFF && i == inp.listen_target)
        ring_draw(dc, ring_frame(GetTickCount(), NULL), cx, cy, r * 2);      /* the animation plays while it waits */
      else if (inp.live & (1u << i))
        ring_draw(dc, 0, cx, cy, r * 2);
    }
  }

  /* The names beside the buttons, drawn straight on the window over the picture's edge. */
  {
    int i;
    HGDIOBJ of = SelectObject(dc, (HFONT)SendMessage(dlg, WM_GETFONT, 0, 0));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, theme_is_dark() ? THEME_DARK_TEXT : GetSysColor(COLOR_WINDOWTEXT));
    for (i = 0; i < inp.labels; i++)
      DrawTextA(dc, inp.label[i].text, -1, &inp.label[i].rc, inp.label[i].fmt | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SelectObject(dc, of);
  }

done:
  if (bdc)
  {
    SetViewportOrgEx(bdc, 0, 0, NULL);
    BitBlt(pdc, ps.rcPaint.left, ps.rcPaint.top, bw, bh, bdc, 0, 0, SRCCOPY);
    SelectObject(bdc, bold);
    DeleteObject(bbmp);
    DeleteDC(bdc);
  }
  EndPaint(dlg, &ps);
}

/* Rebuilds the "Press the key or gamepad button..." hint with however many
   seconds are left before listening times out -- called every tick rather
   than once, but only actually touches the control (SetDlgItemTextA) when
   the displayed number changes, so this isn't repainting 60+ times a
   second for no visible difference. */
static void input_update_listen_hint(HWND dlg)
{
  char hint[160];
  int remaining_ms = (int)(inp.listen_deadline - GetTickCount());
  int secs = (remaining_ms > 0) ? (remaining_ms + 999) / 1000 : 0;

  if (secs == inp.listen_shown_secs) return;
  inp.listen_shown_secs = secs;

  wsprintfA(hint, "Press the key or gamepad button for \"%s\".%s Esc or a right click cancels.",
            input_list_entry_name(inp.listen_target),
            inp.auto_advance ? " Then moves to the next button." : "");
  SetDlgItemTextA(dlg, IDC_INPUT_HINT, hint);
  {
    char label[32];
    wsprintfA(label, "[Waiting %ds]", secs);
    SetDlgItemTextA(dlg, IDC_INPUT_LABEL_BASE + inp.listen_target, label);
  }
}

static void input_set_listening(HWND dlg, int on)
{
  static const int controls[] =
  {
    IDC_INPUT_AUTOASSIGN, IDC_INPUT_DEFAULTS, IDC_INPUT_PADHK, IDC_INPUT_REFRESH, IDC_INPUT_DEVICE, IDC_INPUT_PADTYPE, IDOK, IDCANCEL
  };
  int i;

  /* Focus moves onto the hint text before anything is disabled: disabling the
     focused control lets Windows pass focus (and a key press still in
     flight) on to whatever is next, which started a second assignment. */
  if (on) SetFocus(GetDlgItem(dlg, IDC_INPUT_HINT));

  for (i = 0; i < (int)(sizeof(controls) / sizeof(controls[0])); i++)
  {
    HWND ctl = GetDlgItem(dlg, controls[i]);
    EnableWindow(ctl, on ? FALSE : TRUE);
    RedrawWindow(ctl, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
  }
  for (i = 0; i < input_list_count(); i++)
  {
    HWND ctl = GetDlgItem(dlg, IDC_INPUT_LABEL_BASE + i);
    HWND tk = GetDlgItem(dlg, IDC_INPUT_TURBO_BASE + i);       /* the Turbo tick, where there is one */
    if (tk) EnableWindow(tk, on ? FALSE : TRUE);
    EnableWindow(ctl, on ? FALSE : TRUE);
    RedrawWindow(ctl, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
  }

  /* The button that waits is drawn in the color of a selected row (see WM_DRAWITEM), then back to a normal button. */
  for (i = 0; i < input_list_count(); i++)
  {
    HWND b = GetDlgItem(dlg, IDC_INPUT_LABEL_BASE + i);
    LONG_PTR st = b ? GetWindowLongPtr(b, GWL_STYLE) : 0;
    int want = on && i == inp.sel;
    if (!b) continue;
    if (want != ((st & BS_TYPEMASK) == BS_OWNERDRAW))
    {
      SetWindowLongPtr(b, GWL_STYLE, (st & ~(LONG_PTR)BS_TYPEMASK) | (want ? BS_OWNERDRAW : (BS_PUSHBUTTON | BS_MULTILINE)));
      SetWindowPos(b, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
      InvalidateRect(b, NULL, TRUE);
    }
  }

  /* The blue default-button border tracks focus history, not enabled state,
     so it is handed back to the dialog's real default. */
  SendMessage(dlg, DM_SETDEFID, (WPARAM)IDOK, 0);

  if (on)
  {
    inp.listen = LISTEN_WAIT_RELEASE;
    inp.listen_target = inp.sel;
    inp.listen_deadline = GetTickCount() + 6000;
    inp.listen_shown_secs = -1;   /* forces input_update_listen_hint() to draw immediately below */

    input_update_listen_hint(dlg);
    SetTimer(dlg, 1, 15, NULL);
  }
  else
  {
    inp.listen = LISTEN_OFF;
    KillTimer(dlg, 1);
    SetDlgItemTextA(dlg, IDC_INPUT_HINT, INPUT_HINT_IDLE);
  }
  input_fill_list(dlg);
  input_redraw_picture(dlg);
}

/* After a successful assignment, either moves on to the next button and
   re-arms listening automatically (Auto Assign), or just stops there
   (a click on one button) -- inp.auto_advance, set by whichever one
   actually started this listen, decides which. Reuses
   input_set_listening(dlg, 1) as-is when advancing, including its
   "wait for the current key/button to be released first" safety --
   without that, whatever's still physically held from the assignment
   just made could get immediately captured again as the next one. */
/* A key or pad button was just given to pad button sel. Takes it off any other button of this player (and of the
   other player, for keys, or a pad on the same device), and writes down what happened. A key that is also a shortcut,
   or a pad button that is also a gamepad hotkey, is only mentioned. */
/* Appends to the notice without ever running past its end (a key can sit on several buttons). */
static void notice_add(char *n, const char *fmt, ...)
{
  char tmp[200];
  va_list ap;

  va_start(ap, fmt);
  wvsprintfA(tmp, fmt, ap);
  va_end(ap);
  lstrcpynA(n + lstrlenA(n), tmp, (int)sizeof(inp.notice) - lstrlenA(n));
}

static void input_resolve_conflicts(int sel, int is_key, int value)
{
  char *n = inp.notice;
  int j, o = 1 - inp.player;
  const char *other;

  wsprintfA(n, "%s is now on %s.", is_key ? input_key_name(value) : input_pad_button_name(inp.shown_dev, value), input_list_entry_name(sel));

  for (j = 0; j < PAD_KEYS; j++)
  {
    if (j == sel) continue;
    if (is_key ? inp.map.key[j] == value : inp.map.button[j] == value)
    {
      if (is_key) inp.map.key[j] = 0; else inp.map.button[j] = 0;
      notice_add(n, " It was on %s: removed there.", input_list_entry_name(j));
    }
  }
  for (j = 0; j < PAD_KEYS && (is_key || inp.other[o].map.device == inp.map.device); j++)
  {
    if (is_key ? inp.other[o].map.key[j] == value : inp.other[o].map.button[j] == value)
    {
      if (is_key) inp.other[o].map.key[j] = 0; else inp.other[o].map.button[j] = 0;
      notice_add(n, " It was on Player %d %s: removed there.", o + 1, input_list_entry_name(j));
    }
  }
  if (is_key && (other = hotkey_owner_of_key(value)) != NULL)
    notice_add(n, " Note: it is also the shortcut \"%s\" (Input > Hotkeys).", other);
  if (!is_key && inp.map.device >= 0 && inp.map.device < GUI_INPUT_XPAD_COUNT && (other = padhk_owner_of_button(value)) != NULL)
    notice_add(n, " Note: it is also the gamepad hotkey \"%s\" (Controls > Gamepad Hotkeys).", other);
}

/* The order Auto Assign goes through the buttons in: Up Down Left Right, Start, Mode, X Y Z, A B C. */
static const int input_auto_order[PAD_KEYS] = { PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT, PAD_START, PAD_MODE, PAD_X, PAD_Y, PAD_Z, PAD_A, PAD_B, PAD_C };

static int input_auto_next(int sel)
{
  int k;
  for (k = 0; k + 1 < PAD_KEYS; k++) if (input_auto_order[k] == sel) return input_auto_order[k + 1];
  return -1;
}

static void input_advance_or_stop(HWND dlg, int sel)
{
  int next = inp.auto_advance ? input_auto_next(sel) : -1;
  if (next >= 0)
  {
    inp.sel = next;
    input_set_listening(dlg, 0);
    input_set_listening(dlg, 1);
  }
  else
  {
    input_set_listening(dlg, 0);
    if (inp.notice[0]) SetDlgItemTextA(dlg, IDC_INPUT_HINT, inp.notice);
  }
}

static void input_clear(HWND dlg, int sel)
{
  if (sel < 0 || sel >= input_list_count()) return;
  inp.map.key[sel] = 0; inp.map.button[sel] = 0;
  input_fill_list(dlg);
}

static DWORD wait_cancel_tick;       /* when a right click last ended a wait: its menu (on release) is not shown */

static void input_capture_tick(HWND dlg)
{
  int sel = inp.listen_target;
  int vk, button;

  if (sel < 0 || sel >= input_list_count())
  {
    input_set_listening(dlg, 0);
    return;
  }

  /* GetAsyncKeyState below (both directly for Escape, and inside
     input_capture_key()/input_capture_gamepad()) queries key/button
     state system-wide, with no idea which window is focused -- without
     this check, switching away to a different application entirely
     while a button was mid-assignment would still have whatever's
     typed there captured and assigned here, in the background. */
  if (GetForegroundWindow() != dlg) { input_set_listening(dlg, 0); return; }      /* the window lost focus: the wait ends instead of staying stuck */

  input_update_listen_hint(dlg);

  /* No key or gamepad button in 6 seconds -- most likely nothing was ever
     going to arrive (wrong device selected, a controller that isn't
     actually connected), so this backs out to the same ungreyed state Esc
     gives rather than leaving the dialog stuck waiting indefinitely. */
  if ((int)(GetTickCount() - inp.listen_deadline) >= 0)
  {
    if (inp.listen == LISTEN_WAIT_RELEASE && inp.shown_dev >= 0)       /* for the log: what the pad kept reporting as held */
    {
      int held = input_capture_gamepad(inp.shown_dev);
      gui_log("Controls: gave up waiting for the pad to be released (held: %s)", held ? input_pad_button_name(inp.shown_dev, held) : "nothing");
    }
    input_set_listening(dlg, 0);
    return;
  }

  if (inp.listen == LISTEN_WAIT_RELEASE)
  {
    /* Do not capture the very keypress that opened the prompt. */
    if (!input_any_input_down(inp.shown_dev)) inp.listen = LISTEN_ACTIVE;
    return;
  }

  if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) || (GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_LBUTTON : VK_RBUTTON) & 0x8000))      /* Esc or a right click */
  {
    wait_cancel_tick = GetTickCount();
    input_set_listening(dlg, 0);
    return;
  }

  vk = inp.shown_dev < 0 ? input_capture_key() : 0;      /* a key for "Keyboard Only", a pad button for a gamepad */
  if (vk)
  {
    inp.map.key[sel] = vk;
    input_resolve_conflicts(sel, 1, vk);
    input_fill_list(dlg);
    input_advance_or_stop(dlg, sel);
    return;
  }

  button = inp.shown_dev >= 0 ? input_capture_gamepad(inp.shown_dev) : 0;
  if (button)
  {
    inp.map.button[sel] = button;
    input_resolve_conflicts(sel, 0, button);
    input_fill_list(dlg);
    input_advance_or_stop(dlg, sel);
  }
}

/* Runs continuously while the dialog is open and idle (a separate, slower
   timer than input_capture_tick()'s -- that one only exists to service an
   active "listening" capture). Lets someone tell two same-named XInput pads
   apart -- "Gamepad 1"/"Gamepad 2" carry no identifying information of their
   own, since which physical pad Windows hands which XInput slot depends on
   connection order, not anything this app controls -- without going through
   a full Assign. Whichever pad is currently selected in the device combo
   just needs a button pressed on it; nothing gets recorded anywhere. */
static void input_update_identify_hint(HWND dlg)
{
  /* The resting hint is all the text needs now: whichever pad is selected
     shows its presses live on the picture (input_update_live). */
  if (inp.listen != LISTEN_OFF) return;
  SetDlgItemTextA(dlg, IDC_INPUT_HINT, INPUT_HINT_IDLE);
}

#define COMBO_NONE (-99)

/* The device chosen in the list: -1 keyboard only, an XInput slot, or a DirectInput joystick (COMBO_NONE: none). */
static int input_combo_device(HWND dlg)
{
  HWND c = GetDlgItem(dlg, IDC_INPUT_DEVICE);
  int sel = (int)SendMessage(c, CB_GETCURSEL, 0, 0);
  return sel == CB_ERR ? COMBO_NONE : (int)SendMessage(c, CB_GETITEMDATA, (WPARAM)sel, 0);
}

static void input_add_device(HWND c, const char *name, int device)
{
  int idx = (int)SendMessageA(c, CB_ADDSTRING, 0, (LPARAM)name);
  if (idx >= 0) SendMessage(c, CB_SETITEMDATA, (WPARAM)idx, (LPARAM)device);
}

/* Lights the spot of every mapped button whose key or gamepad button is held
   down: polled on a short timer, and only repainted when something changed. */
static void input_update_live(HWND dlg)
{
  unsigned now = 0;
  int i, dev = input_combo_device(dlg);

  if (dev == COMBO_NONE) dev = -1;

  if (inp.listen == LISTEN_OFF && GetForegroundWindow() == dlg)
  {
    for (i = 0; i < PAD_KEYS; i++)
    {
      if (inp.map.key[i] && (GetAsyncKeyState(inp.map.key[i]) & 0x8000)) now |= 1u << i;
      else if (dev >= 0 && inp.map.button[i] && input_pad_button_down(dev, inp.map.button[i])) now |= 1u << i;
    }
  }

  if (now != inp.live)
  {
    inp.live = now;
    input_redraw_picture(dlg);
  }
}

static void input_load_controls(HWND dlg)
{
  HWND dev = GetDlgItem(dlg, IDC_INPUT_DEVICE);
  HWND type = GetDlgItem(dlg, IDC_INPUT_PADTYPE);
  int i, joy_count;

  /* "Keyboard Only", then one entry per XInput slot, then one per attached
     DirectInput joystick -- contiguous with t_pad_map.device's own numbering
     (-1, 0.., GUI_INPUT_XPAD_COUNT..), so the combo's selected index and the
     device value it maps to are always exactly one apart, list position and
     device range alike. */
  SendMessage(dev, CB_RESETCONTENT, 0, 0);
  input_add_device(dev, "Keyboard Only", -1);
  for (i = 0; i < GUI_INPUT_XPAD_COUNT; i++)
  {
    char buf[96];
    const char *nm;
    int j, same = 0;
    /* Only a pad that is plugged in (or the one this player is set to, so a saved choice is not lost). */
    if (!gui_input_xpad_connected(i)) continue;
    nm = gui_input_xpad_name(i);
    if (!nm[0]) wsprintfA(buf, "Gamepad %d", i + 1);
    else
    {
      /* The pad's own name; two pads of one kind are told apart by their slot number. */
      for (j = 0; j < GUI_INPUT_XPAD_COUNT; j++)
        if (j != i && !lstrcmpiA(gui_input_xpad_name(j), nm)) same = 1;
      if (same) wsprintfA(buf, "%s (%d)", nm, i + 1);
      else lstrcpynA(buf, nm, sizeof(buf));
    }
    input_add_device(dev, buf, i);
  }
  joy_count = gui_input_joystick_count();
  for (i = 0; i < joy_count; i++)
  {
    input_add_device(dev, gui_input_joystick_name(i), GUI_INPUT_XPAD_COUNT + i);
  }

  /* A joystick's product name easily runs longer than "Gamepad 4" ever did
     -- widens the dropdown (not the closed combo box itself, which stays
     whatever width the .rc template gives it) so a longer one isn't
     truncated the moment the list opens. */
  if (joy_count) SendMessage(dev, CB_SETDROPPEDWIDTH, 220, 0);
  {
    int k, n2 = (int)SendMessage(dev, CB_GETCOUNT, 0, 0), pick = -1;
    for (k = 0; k < n2; k++)
      if ((int)SendMessage(dev, CB_GETITEMDATA, (WPARAM)k, 0) == inp.map.device) { pick = k; break; }
    /* A saved pad that is not connected is not in the list: Keyboard Only is shown, and the saved device is
       kept until another one is picked. */
    inp.dev_shadow = (pick < 0);
    inp.shown_dev = inp.dev_shadow ? -1 : inp.map.device;
    SendMessage(dev, CB_SETCURSEL, (WPARAM)(pick < 0 ? 0 : pick), 0);
  }

  SendMessage(type, CB_RESETCONTENT, 0, 0);
  SendMessageA(type, CB_ADDSTRING, 0, (LPARAM)"Match the Game");
  SendMessageA(type, CB_ADDSTRING, 0, (LPARAM)"3-Button Pad");
  SendMessageA(type, CB_ADDSTRING, 0, (LPARAM)"6-Button Pad");
  SendMessageA(type, CB_ADDSTRING, 0, (LPARAM)"2-Button Pad");

  switch (inp.padtype)
  {
    case DEVICE_PAD3B: SendMessage(type, CB_SETCURSEL, 1, 0); break;
    case DEVICE_PAD6B: SendMessage(type, CB_SETCURSEL, 2, 0); break;
    case DEVICE_PAD2B: SendMessage(type, CB_SETCURSEL, 3, 0); break;
    default:           SendMessage(type, CB_SETCURSEL, 0, 0); break;
  }

  audio_set_slider(dlg, IDC_INPUT_DEADZONE, IDC_INPUT_DEADZONE_TEXT, 0, 100, inp.deadzone, "%");
}

static void input_read_controls(HWND dlg)
{
  int dev = input_combo_device(dlg);
  int type = (int)SendMessage(GetDlgItem(dlg, IDC_INPUT_PADTYPE), CB_GETCURSEL, 0, 0);

  /* CB_ERR here doesn't mean the user picked nothing -- the combo always has
     at least "Keyboard Only", so a real choice never comes back this way.
     It means input_load_controls()'s own CB_SETCURSEL(device+1) failed
     because inp.map.device pointed past the currently-populated list (a
     saved DirectInput mapping whose joystick isn't attached right now, so
     it couldn't be resolved to a valid combo item -- see
     gui_input_resolve_device()). Leaving inp.map.device/joy_name untouched
     in that case preserves the saved assignment; overwriting it with -1
     would silently downgrade the player to keyboard-only just because the
     dialog happened to be opened (for an unrelated key rebind, say) while
     their joystick was unplugged. */
  if (dev != COMBO_NONE && !inp.dev_shadow)
  {
    inp.map.device = dev;
    inp.shown_dev = dev;

    /* Remembered so a saved mapping can find the same physical joystick
       again even if it re-enumerates at a different index next time (see
       gui_input_resolve_device()). Meaningless for keyboard/XInput, so
       cleared there instead of left stale from whatever DirectInput device
       used to be picked. */
    if (inp.map.device >= GUI_INPUT_XPAD_COUNT)
      lstrcpynA(inp.map.joy_name, gui_input_joystick_name(inp.map.device - GUI_INPUT_XPAD_COUNT), sizeof(inp.map.joy_name));
    else
      inp.map.joy_name[0] = '\0';
  }

  switch (type)
  {
    case 1:  inp.padtype = DEVICE_PAD3B; break;
    case 2:  inp.padtype = DEVICE_PAD6B; break;
    case 3:  inp.padtype = DEVICE_PAD2B; break;
    default: inp.padtype = DEVICE_PAD2B | DEVICE_PAD3B | DEVICE_PAD6B; break;
  }

  inp.deadzone = (int)SendMessage(GetDlgItem(dlg, IDC_INPUT_DEADZONE), TBM_GETPOS, 0, 0);
}

/* The text of a name beside a button: "Up:", "A:" ... */
static const char *input_label_text(int i)
{
  static char buf[PAD_KEYS][32];
  wsprintfA(buf[i], "%s:", input_list_entry_name(i));
  return buf[i];
}

static void input_add_label(const char *text, int x, int y, int w, int h, UINT fmt)
{
  if (inp.labels >= 16) return;
  SetRect(&inp.label[inp.labels].rc, x, y, x + w, y + h);
  inp.label[inp.labels].fmt = fmt;
  lstrcpynA(inp.label[inp.labels].text, text, sizeof(inp.label[inp.labels].text));
  inp.labels++;
}

static HWND input_make(HWND dlg, const char *cls, const char *text, DWORD style,
                       int x, int y, int w, int h, int id)
{
  HWND c = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, dlg,
                           (HMENU)(INT_PTR)id, GetModuleHandle(NULL), NULL);
  SendMessage(c, WM_SETFONT, (WPARAM)(inp.font ? inp.font : (HFONT)SendMessage(dlg, WM_GETFONT, 0, 0)), TRUE);
  return c;
}


/****************************************************************************
 * Controls > Combos tab: one key or pad button presses several game buttons
 *
 * Stored as text in gui.combo_text ("Q = A+B;Pad:LB = B+C"), which input.c reads. Here it is a list of the
 * combos, a bind button (click it, then press the key or pad button to use) and a tick box per game button,
 * so nothing has to be typed. The working copy is written to gui.combo_text when Controls is saved.
 ****************************************************************************/

/* A combo as the window holds it: its trigger and the steps in the order they were clicked -- an action (one of the game buttons,
   perhaps Held for some time) or a pause before the next action. Saved as text: "Pad:Y = Down:H1000+P500+A". */
typedef struct { int pause, idx, hold, ms; } CBSTEP;     /* pause: a pause of ms; else the action idx (index into cb_game), Held (hold) for ms (0: until you let go of it) */
typedef struct { char trig[12]; int n; CBSTEP s[24]; } COMBOITEM;
static const int cb_holds[]  = { 250, 500, 1000, 1500, 2000, 3000, 5000, 10000 };       /* the Hold menu, after "Off" and "Until I Let Go" */
static const int cb_pauses[] = { 70, 150, 250, 500, 1000, 2000 };                        /* the Pause menu, after "None" */
#define CB_NHOLDS  ((int)(sizeof(cb_holds) / sizeof(cb_holds[0])))
#define CB_NPAUSES ((int)(sizeof(cb_pauses) / sizeof(cb_pauses[0])))
/* The Combos tab's controls outside the 1620-1639 run: Hold, Pause and How It Works (IDs 1649 to 1653). */
#define CB_EXTRA_ID(id) ((id) >= IDC_CB_HOLDTIME && (id) <= IDC_CB_HELP)
static int input_start_page;                 /* which tab Controls opens on: 3 = Combos */
static COMBOITEM cb_items[12];
static int       cb_n;
static int       cb_sel = -1;                /* the combo being made or changed (a row of the list), -1 when none */
static struct { int listen, wait, secs; DWORD deadline; } cbl;       /* secs: the countdown the trigger button shows */

static const char *const cb_game[12] = { "A", "B", "C", "X", "Y", "Z", "Start", "Mode", "Up", "Down", "Left", "Right" };

/* XInput pad buttons a combo can be bound to, with the names input.c reads. */
static const struct { int mask; const char *trig; } cb_pads[] =
{
  { 0x1000, "Pad:A" }, { 0x2000, "Pad:B" }, { 0x4000, "Pad:X" }, { 0x8000, "Pad:Y" },
  { 0x0100, "Pad:LB" }, { 0x0200, "Pad:RB" }, { 0x00010000, "Pad:LT" }, { 0x00020000, "Pad:RT" },
  { 0x0020, "Pad:Back" }, { 0x0010, "Pad:Start" },
  { 0x0001, "Pad:Up" }, { 0x0002, "Pad:Down" }, { 0x0004, "Pad:Left" }, { 0x0008, "Pad:Right" }
};
#define CB_NPADS ((int)(sizeof(cb_pads) / sizeof(cb_pads[0])))

/* The name a key or pad button is saved under; 0 when a combo cannot use that key. */
static int cb_key_trig(int vk, char *out)
{
  if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { out[0] = (char)vk; out[1] = '\0'; return 1; }
  if (vk >= VK_F1 && vk <= VK_F12) { wsprintfA(out, "F%d", vk - VK_F1 + 1); return 1; }
  if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { wsprintfA(out, "Num%d", vk - VK_NUMPAD0); return 1; }
  if (vk == VK_SPACE) { lstrcpyA(out, "Space"); return 1; }
  if (vk == VK_UP) { lstrcpyA(out, "Up"); return 1; }
  if (vk == VK_DOWN) { lstrcpyA(out, "Down"); return 1; }
  if (vk == VK_LEFT) { lstrcpyA(out, "Left"); return 1; }
  if (vk == VK_RIGHT) { lstrcpyA(out, "Right"); return 1; }
  return 0;
}

static int cb_trig_ok(const char *t)
{
  char tmp[12];
  int i, vk;

  for (i = 0; i < CB_NPADS; i++) if (!lstrcmpiA(cb_pads[i].trig, t)) return 1;
  for (vk = 1; vk < 256; vk++) if (cb_key_trig(vk, tmp) && !lstrcmpiA(tmp, t)) return 1;
  return 0;
}

static void cb_label(const char *trig, char *out)
{
  if (!strncmp(trig, "Pad:", 4)) wsprintfA(out, "Gamepad %s", trig + 4);
  else wsprintfA(out, "Key %s", trig);
}

/* "1.0", "0.5", "0.25": a time in ms as seconds. */
static void cb_secs(int ms, char *out)
{
  if (ms % 100 == 0) wsprintfA(out, "%d.%d", ms / 1000, ms % 1000 / 100);
  else wsprintfA(out, "%d.%02d", ms / 1000, ms % 1000 / 10);
}

static int cb_find_action(const COMBOITEM *c, int idx)
{
  int i;
  for (i = 0; i < c->n; i++) if (!c->s[i].pause && c->s[i].idx == idx) return i;
  return -1;
}

static int cb_actions(const COMBOITEM *c)
{
  int i, n = 0;
  for (i = 0; i < c->n; i++) if (!c->s[i].pause) n++;
  return n;
}

/* The steps as text: for the list ("Down(Hold 1.0s)+Pause 0.5s+A") or for saving ("Down:H1000+P500+A"). */
static void cb_steps_text(const COMBOITEM *c, int readable, char *out, int len)
{
  char tmp[640], one[64], t[16];
  int i;

  tmp[0] = '\0';
  for (i = 0; i < c->n; i++)
  {
    const CBSTEP *p = &c->s[i];
    if (i) lstrcatA(tmp, "+");
    if (p->pause)
    {
      if (readable) { cb_secs(p->ms, t); wsprintfA(one, "Pause %ss", t); }
      else wsprintfA(one, "P%d", p->ms);
    }
    else if (!p->hold) lstrcpyA(one, cb_game[p->idx]);
    else if (readable)
    {
      if (p->ms) { cb_secs(p->ms, t); wsprintfA(one, "%s(Hold %ss)", cb_game[p->idx], t); }
      else wsprintfA(one, "%s(Hold)", cb_game[p->idx]);
    }
    else if (p->ms) wsprintfA(one, "%s:H%d", cb_game[p->idx], p->ms);
    else wsprintfA(one, "%s:Hold", cb_game[p->idx]);
    lstrcatA(tmp, one);
  }
  lstrcpynA(out, tmp, len);
}

static void cb_parse(const char *text)
{
  char buf[512], *line, *next, *q;
  int k = 0, j;

  lstrcpynA(buf, text, sizeof(buf));
  for (q = buf; *q; q++) if (*q == '\r' || *q == '\n') *q = ';';
  ZeroMemory(cb_items, sizeof(cb_items));
  cb_n = 12;                                                    /* the list always has its 12 rows; the unused ones are empty */
  for (line = buf; line && k < 12; line = next)
  {
    char *eq, *t;
    COMBOITEM c;
    int row = k++;                                              /* an empty part of the text is an empty row, so rows keep their place */

    next = strchr(line, ';');
    if (next) *next++ = '\0';
    eq = strchr(line, '=');
    if (!eq) continue;
    *eq++ = '\0';
    while (*line == ' ') line++;
    { char *e = line + lstrlenA(line); while (e > line && e[-1] == ' ') *--e = '\0'; }
    if (!cb_trig_ok(line)) continue;
    for (j = 0; j < row && lstrcmpiA(cb_items[j].trig, line); j++) { }
    if (j < row) continue;                                      /* a trigger another row has already: ignored */
    ZeroMemory(&c, sizeof(c));
    lstrcpynA(c.trig, line, sizeof(c.trig));
    for (t = strtok(eq, "+ "); t && c.n < 24; t = strtok(NULL, "+ "))
    {
      char *colon;
      int i;

      if ((t[0] == 'P' || t[0] == 'p') && t[1] >= '0' && t[1] <= '9')
      {
        if (c.n && !c.s[c.n - 1].pause) { c.s[c.n].pause = 1; c.s[c.n].ms = atoi(t + 1); c.n++; }
        continue;
      }
      colon = strchr(t, ':');
      if (colon) *colon++ = '\0';
      for (i = 0; i < 12; i++)
        if (!lstrcmpiA(t, cb_game[i]) && cb_find_action(&c, i) < 0)
        {
          CBSTEP *p = &c.s[c.n++];
          p->idx = i;
          if (colon && (colon[0] == 'H' || colon[0] == 'h'))
          {
            p->hold = 1;
            if (colon[1] >= '0' && colon[1] <= '9') p->ms = atoi(colon + 1);
          }
          break;
        }
    }
    cb_items[row] = c;                                          /* (a row with a trigger and no actions yet is kept too) */
  }
}

static int cb_serialize(char *out, int len)
{
  char tmp[1600], m[640];
  int i;

  tmp[0] = '\0';
  for (i = 0; i < cb_n; i++)
  {
    if (i) lstrcatA(tmp, ";");                                  /* an empty row is an empty part, so rows keep their place */
    if (!cb_items[i].trig[0]) continue;
    cb_steps_text(&cb_items[i], 0, m, sizeof(m));
    if (lstrlenA(tmp) + lstrlenA(m) + 20 >= (int)sizeof(tmp)) return 0;
    lstrcatA(tmp, cb_items[i].trig); lstrcatA(tmp, " = "); lstrcatA(tmp, m);
  }
  for (i = lstrlenA(tmp); i > 0 && tmp[i - 1] == ';'; i--) tmp[i - 1] = '\0';
  if ((int)lstrlenA(tmp) >= len) return 0;
  lstrcpynA(out, tmp, len);
  return 1;
}

/* The widths of the list's first two columns: the row number and the trigger (the actions take the rest). */
#define CB_COL_NUM  IS(32)
#define CB_COL_TRIG IS(104)

static int cb_filling;          /* the list is being changed by the code: its selection messages are not the user's */

static void cb_fill_list(HWND dlg, int select)
{
  HWND l = GetDlgItem(dlg, IDC_CB_LIST);
  int i;

  cb_filling = 1;
  if ((int)SendMessage(l, LVM_GETITEMCOUNT, 0, 0) != cb_n)
  {
    SendMessage(l, LVM_DELETEALLITEMS, 0, 0);
    for (i = 0; i < cb_n; i++)
    {
      LVITEMA it;
      char num[8];

      wsprintfA(num, "%d", i + 1);
      ZeroMemory(&it, sizeof(it));
      it.mask = LVIF_TEXT;
      it.iItem = i;
      it.pszText = num;
      SendMessageA(l, LVM_INSERTITEMA, 0, (LPARAM)&it);
    }
  }
  for (i = 0; i < cb_n; i++)
  {
    char lab[40], m[640];

    if (cb_items[i].trig[0]) cb_label(cb_items[i].trig, lab); else lab[0] = '\0';      /* an empty row has only its number */
    cb_steps_text(&cb_items[i], 1, m, sizeof(m));
    ListView_SetItemText(l, i, 1, lab);
    ListView_SetItemText(l, i, 2, m);
  }
  if (select >= 0 && select < cb_n) ListView_SetItemState(l, select, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
  cb_filling = 0;
}

/* The combo being made or changed, or -1. */
static int cb_work(void)
{
  return (cb_sel >= 0 && cb_sel < cb_n) ? cb_sel : -1;
}

/* With a row picked the Trigger button works, and with a trigger the action buttons; Hold, Pause and Remove Last need an
   action in the combo; Delete Combo needs a row with something in it. */
static void cb_update_buttons(HWND dlg)
{
  int i, at = cb_work(), acts = (at >= 0) ? cb_actions(&cb_items[at]) : 0;

  EnableWindow(GetDlgItem(dlg, IDC_CB_BIND), at >= 0);
  for (i = 0; i < 12; i++) EnableWindow(GetDlgItem(dlg, IDC_CB_G0 + i), at >= 0 && cb_items[at].trig[0] != 0);
  EnableWindow(GetDlgItem(dlg, IDC_CB_HOLDTIME), acts > 0);
  EnableWindow(GetDlgItem(dlg, IDC_CB_PAUSE), acts > 0);
  EnableWindow(GetDlgItem(dlg, IDC_CB_UNDO), acts > 0);
  EnableWindow(GetDlgItem(dlg, IDC_CB_DEL), at >= 0 && (cb_items[at].trig[0] != 0 || acts > 0));          /* a blank row has nothing to delete */
}

static void cb_update_bind(HWND dlg)
{
  char t[48];
  if (cbl.listen) wsprintfA(t, "[Waiting %ds]", cbl.secs);
  else if (cb_work() >= 0 && cb_items[cb_work()].trig[0]) cb_label(cb_items[cb_work()].trig, t);
  else lstrcpyA(t, cb_work() >= 0 ? "Click here, then press a key or pad button" : "Trigger");
  SetDlgItemTextA(dlg, IDC_CB_BIND, t);
  cb_update_buttons(dlg);
}

static void cb_show(HWND dlg, int idx)
{
  if (idx < 0 || idx >= cb_n) return;
  cb_sel = idx;
  cb_fill_list(dlg, idx);                                       /* the row being worked on shows a + at its end */
  cb_update_bind(dlg);
}

static void cb_say(HWND dlg, const char *text);

static void cb_listen(HWND dlg, int on)
{
  if (on == cbl.listen) return;
  cbl.listen = on;
  if (on) { cbl.wait = 1; cbl.secs = 6; cbl.deadline = GetTickCount() + 6000; SetTimer(dlg, 3, 30, NULL); }
  else KillTimer(dlg, 3);
  cb_update_bind(dlg);
}

static int cb_any_down(void)
{
  int s;
  if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) return 1;
  if (input_capture_key()) return 1;
  for (s = 0; s < GUI_INPUT_XPAD_COUNT; s++) if (input_capture_gamepad(s)) return 1;
  return 0;
}

/* Called every 30 ms while the bind button waits for a key or pad button. */
static void cb_tick(HWND dlg)
{
  char trig[12];
  int vk, s, i, found = 0;

  if (!cbl.listen) return;
  if (GetForegroundWindow() != dlg) { cb_listen(dlg, 0); return; }
  if ((int)(GetTickCount() - cbl.deadline) >= 0) { cb_listen(dlg, 0); return; }
  {
    int ms = (int)(cbl.deadline - GetTickCount()), secs = ms > 0 ? (ms + 999) / 1000 : 0;
    if (secs != cbl.secs) { cbl.secs = secs; cb_update_bind(dlg); }
  }
  if (cbl.wait) { if (!cb_any_down()) cbl.wait = 0; return; }       /* not the press that opened the prompt */
  if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) || (GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_LBUTTON : VK_RBUTTON) & 0x8000)) { wait_cancel_tick = GetTickCount(); cb_listen(dlg, 0); return; }

  vk = input_capture_key();
  if (vk)
  {
    if (!cb_key_trig(vk, trig))
    {
      SetDlgItemTextA(dlg, IDC_CB_BIND, "That key cannot be used - press another");
      cbl.wait = 1;
      return;
    }
    found = 1;
  }
  for (s = 0; s < GUI_INPUT_XPAD_COUNT && !found; s++)
  {
    int m = input_capture_gamepad(s);
    if (!m) continue;
    for (i = 0; i < CB_NPADS; i++) if (cb_pads[i].mask == m) { lstrcpyA(trig, cb_pads[i].trig); found = 1; break; }
    if (!found) { SetDlgItemTextA(dlg, IDC_CB_BIND, "That button cannot be used - press another"); cbl.wait = 1; return; }
  }
  if (!found) return;

  cb_listen(dlg, 0);
  if (cb_work() < 0) return;
  for (i = 0; i < cb_n; i++)
    if (i != cb_sel && !lstrcmpiA(cb_items[i].trig, trig))
    {
      char msg[96];
      wsprintfA(msg, "Row %d already uses that trigger. Choose another.", i + 1);
      cb_say(dlg, msg);
      cb_update_bind(dlg);
      return;
    }
  lstrcpynA(cb_items[cb_sel].trig, trig, sizeof(cb_items[0].trig));
  cb_say(dlg, "");
  cb_fill_list(dlg, cb_sel);
  cb_update_bind(dlg);
}

/* Text for the message line under the combos while the window is open. */
static void cb_say(HWND dlg, const char *text)
{
  SetDlgItemTextA(dlg, IDC_INPUT_HINT, text);
}

/* Puts the working combo (the one on the trigger) in the list again and keeps it selected. */
static int cb_changed(HWND dlg, int at)
{
  char test[512];
  if (!cb_serialize(test, sizeof(test)) || (int)lstrlenA(test) >= (int)sizeof(((t_gui_config *)0)->combo_text))
    return 0;
  cb_fill_list(dlg, at);
  cb_update_buttons(dlg);
  return 1;
}

/* A click on an action button: the action goes onto the end of the combo on the trigger (the combo is made on the first one). */
static void cb_add_action(HWND dlg, int idx)
{
  int at = cb_work();
  COMBOITEM keep[12];
  int keep_n = cb_n;

  if (at < 0) { cb_say(dlg, "Click a row in the list first."); return; }
  if (!cb_items[at].trig[0]) { cb_say(dlg, "Click the Trigger button first and press the key or pad button to use."); return; }
  memcpy(keep, cb_items, sizeof(keep));
  if (cb_find_action(&cb_items[at], idx) >= 0) { cb_say(dlg, "That button is already in this combo."); return; }
  if (cb_items[at].n >= 24) { cb_say(dlg, "This combo is full."); return; }
  cb_items[at].s[cb_items[at].n].pause = 0;
  cb_items[at].s[cb_items[at].n].idx = idx;
  cb_items[at].s[cb_items[at].n].hold = 0;
  cb_items[at].s[cb_items[at].n].ms = 0;
  cb_items[at].n++;
  if (!cb_changed(dlg, at))
  {
    memcpy(cb_items, keep, sizeof(keep)); cb_n = keep_n;
    cb_fill_list(dlg, cb_work());
    cb_say(dlg, "The saved combos list is full. Remove an action or a combo to make room.");
    return;
  }
  cb_say(dlg, "");
}

/* The Hold button's menu: how long the action just added stays down. */
static void cb_hold_menu(HWND dlg)
{
  int at = cb_work(), i, last = -1, cmd;
  COMBOITEM keep1;
  HMENU m;
  RECT br;
  char t[24], name[48];

  if (at < 0) return;
  keep1 = cb_items[at];
  for (i = cb_items[at].n - 1; i >= 0; i--) if (!cb_items[at].s[i].pause) { last = i; break; }
  if (last < 0) return;

  m = CreatePopupMenu();
  AppendMenuA(m, MF_STRING, 1, "Off (No Hold)");
  AppendMenuA(m, MF_STRING, 2, "Until I Let Go");
  for (i = 0; i < CB_NHOLDS; i++) { cb_secs(cb_holds[i], t); wsprintfA(name, "%s s", t); AppendMenuA(m, MF_STRING, 10 + i, name); }
  { int cur = !cb_items[at].s[last].hold ? 1 : (cb_items[at].s[last].ms ? 0 : 2);
    for (i = 0; i < CB_NHOLDS; i++) if (cb_items[at].s[last].hold && cb_items[at].s[last].ms == cb_holds[i]) cur = 10 + i;
    if (cur) CheckMenuRadioItem(m, 1, 10 + CB_NHOLDS, (UINT)cur, MF_BYCOMMAND); }
  GetWindowRect(GetDlgItem(dlg, IDC_CB_HOLDTIME), &br);
  cmd = (int)gui_popup(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, br.left, br.bottom);
  DestroyMenu(m);
  if (cmd == 1) { cb_items[at].s[last].hold = 0; cb_items[at].s[last].ms = 0; }
  else if (cmd == 2) { cb_items[at].s[last].hold = 1; cb_items[at].s[last].ms = 0; }
  else if (cmd >= 10 && cmd < 10 + CB_NHOLDS) { cb_items[at].s[last].hold = 1; cb_items[at].s[last].ms = cb_holds[cmd - 10]; }
  else return;
  if (!cb_changed(dlg, at)) { cb_items[at] = keep1; cb_fill_list(dlg, at); cb_say(dlg, "The saved combos list is full. Remove an action or a combo to make room."); }
}

/* The Pause button's menu: a pause after the action just added (before the next one). */
static void cb_pause_menu(HWND dlg)
{
  int at = cb_work(), i, cmd, n, cur = 1;
  COMBOITEM keep1;
  HMENU m;
  RECT br;
  char t[24], name[48];

  if (at < 0 || !cb_actions(&cb_items[at])) return;
  keep1 = cb_items[at];
  n = cb_items[at].n;
  m = CreatePopupMenu();
  AppendMenuA(m, MF_STRING, 1, "None");
  for (i = 0; i < CB_NPAUSES; i++) { cb_secs(cb_pauses[i], t); wsprintfA(name, "%s s", t); AppendMenuA(m, MF_STRING, 10 + i, name); }
  if (cb_items[at].s[n - 1].pause)
    for (i = 0; i < CB_NPAUSES; i++) if (cb_items[at].s[n - 1].ms == cb_pauses[i]) cur = 10 + i;
  CheckMenuRadioItem(m, 1, 10 + CB_NPAUSES, (UINT)cur, MF_BYCOMMAND);
  GetWindowRect(GetDlgItem(dlg, IDC_CB_PAUSE), &br);
  cmd = (int)gui_popup(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, br.left, br.bottom);
  DestroyMenu(m);
  if (cmd == 1) { if (cb_items[at].s[n - 1].pause) cb_items[at].n--; }
  else if (cmd >= 10 && cmd < 10 + CB_NPAUSES)
  {
    if (!cb_items[at].s[n - 1].pause)
    {
      if (n >= 24) { cb_say(dlg, "This combo is full."); return; }
      cb_items[at].s[n].pause = 1;
      cb_items[at].n++;
      n++;
    }
    cb_items[at].s[n - 1].ms = cb_pauses[cmd - 10];
  }
  else return;
  if (!cb_changed(dlg, at)) { cb_items[at] = keep1; cb_fill_list(dlg, at); cb_say(dlg, "The saved combos list is full. Remove an action or a combo to make room."); }
}

static int cb_command(HWND dlg, int id, int code)
{
  switch (id)
  {
    case IDC_CB_HELP:
      if (code == BN_CLICKED)
        MessageBoxA(dlg,
          "1.  Click a row in the list (an empty one for a new combo).\n"
          "2.  Click the Trigger button, then press the key or gamepad button that should set the combo off.\n"
          "3.  Click the game buttons the combo should press, one after the other. Each click adds a button to the row.\n\n"
          "Hold: opens a menu that makes the button you just added Held. It stays down by itself after the trigger is pressed, for the time you choose, or until you press that button yourself.\n"
          "Pause: opens a menu to wait before the next button starts.\n"
          "Remove Last takes the last button off the row. Delete Combo empties the row.\n\n"
          "Example, for a Sonic spin dash: Trigger = Gamepad Y, then click Down, Hold > 1 s, then click A. The row reads\n"
          "Gamepad Y -> Down(Hold 1.0s)+A\n"
          "Pressing Y holds Down for a second and taps A a moment after it.",
          "Combos - How It Works", MB_OK | MB_ICONINFORMATION);
      return 1;
    case IDC_CB_BIND:
      if (code == BN_CLICKED) cb_listen(dlg, !cbl.listen);
      return 1;
    case IDC_CB_HOLDTIME:
      if (code == BN_CLICKED) cb_hold_menu(dlg);
      return 1;
    case IDC_CB_PAUSE:
      if (code == BN_CLICKED) cb_pause_menu(dlg);
      return 1;
    case IDC_CB_UNDO:                                           /* Remove Last: the last action (and the pause before it) */
      if (code == BN_CLICKED)
      {
        int at = cb_work();
        if (at < 0) return 1;
        while (cb_items[at].n && cb_items[at].s[cb_items[at].n - 1].pause) cb_items[at].n--;
        if (cb_items[at].n) cb_items[at].n--;
        while (cb_items[at].n && cb_items[at].s[cb_items[at].n - 1].pause) cb_items[at].n--;
        cb_changed(dlg, at);                                    /* (an empty row stays: it has its trigger, ready for new actions) */
      }
      return 1;
    case IDC_CB_DEL:
    {
      int at = cb_work(), sel = at;
      if (code == BN_CLICKED)
      {
        if (sel < 0 || sel >= cb_n) return 1;
        ZeroMemory(&cb_items[sel], sizeof(cb_items[sel]));      /* the row stays, empty */
        cb_say(dlg, "");
        cb_fill_list(dlg, sel);
        cb_update_bind(dlg);
      }
      return 1;
    }
    case IDC_CB_G0: case IDC_CB_G0 + 1: case IDC_CB_G0 + 2: case IDC_CB_G0 + 3: case IDC_CB_G0 + 4: case IDC_CB_G0 + 5:
    case IDC_CB_G0 + 6: case IDC_CB_G0 + 7: case IDC_CB_G0 + 8: case IDC_CB_G0 + 9: case IDC_CB_G0 + 10: case IDC_CB_G0 + 11:
      if (code == BN_CLICKED) cb_add_action(dlg, id - IDC_CB_G0);
      return 1;
  }
  return 0;
}

/* The Extras tab's Combos frame: the trigger and the action buttons on the left, Hold / Pause / Remove Last / Delete in the
   middle, the saved combos (number, trigger, actions) on the right (cy: where the frame starts, cw: the window's width). Hidden
   until that tab is chosen. */
static void cb_build(HWND dlg, int M, int cy, int cw)
{
  /* Where each game button sits in the grid (column, row), in groups: the D-pad, then Start and Mode, then X Y Z, then
     A B C. Indexes follow cb_game: A B C X Y Z Start Mode Up Down Left Right. */
  static const int pos[12][2] =
  {
    { 0, 3 }, { 1, 3 }, { 2, 3 },            /* A B C */
    { 0, 2 }, { 1, 2 }, { 2, 2 },            /* X Y Z */
    { 0, 1 }, { 1, 1 },                      /* Start, Mode */
    { 0, 0 }, { 1, 0 }, { 2, 0 }, { 3, 0 }   /* Up, Down, Left, Right */
  };
  int fx = M + IS(14), lw = IS(252), midw = IS(100), gap = IS(14), mx, rx, rw, i;
  HWND h;

  mx = fx + lw + gap;
  rx = mx + midw + gap;
  rw = cw - M - IS(14) - rx;

  SetRect(&cb_frame[0], M, cy, cw - M, cy + IS(188));                /* the one frame round it all */

  h = input_make(dlg, "BUTTON", "", WS_TABSTOP | BS_PUSHBUTTON, fx, cy + IS(18), lw, IS(26), IDC_CB_BIND);
  ShowWindow(h, SW_HIDE);
  for (i = 0; i < 12; i++)
  {
    h = input_make(dlg, "BUTTON", cb_game[i], WS_TABSTOP | BS_PUSHBUTTON,
                   fx + pos[i][0] * IS(62), cy + IS(52) + pos[i][1] * IS(25), IS(58), IS(22), IDC_CB_G0 + i);
    ShowWindow(h, SW_HIDE);
  }
  h = input_make(dlg, "BUTTON", "Hold", WS_TABSTOP | BS_OWNERDRAW, mx, cy + IS(18), midw, IS(26), IDC_CB_HOLDTIME);      /* drawn with an arrow: a menu opens */
  ShowWindow(h, SW_HIDE);
  h = input_make(dlg, "BUTTON", "Pause", WS_TABSTOP | BS_OWNERDRAW, mx, cy + IS(50), midw, IS(26), IDC_CB_PAUSE);
  ShowWindow(h, SW_HIDE);
  h = input_make(dlg, "BUTTON", "Remove Last", WS_TABSTOP | BS_PUSHBUTTON, mx, cy + IS(82), midw, IS(26), IDC_CB_UNDO);
  ShowWindow(h, SW_HIDE);
  h = input_make(dlg, "BUTTON", "Delete Combo", WS_TABSTOP | BS_PUSHBUTTON, mx, cy + IS(114), midw, IS(26), IDC_CB_DEL);
  ShowWindow(h, SW_HIDE);
  h = input_make(dlg, "BUTTON", "How It Works", WS_TABSTOP | BS_PUSHBUTTON, rx + rw - IS(96), cy + IS(9), IS(96), IS(22), IDC_CB_HELP);
  ShowWindow(h, SW_HIDE);
  h = input_make(dlg, "SysListView32", "", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, rx, cy + IS(38), rw, IS(140), IDC_CB_LIST);
  SendMessage(h, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT);
  {
    static const char *const titles[3] = { "#", "Trigger", "Actions" };
    int widths[3], c;

    widths[0] = CB_COL_NUM; widths[1] = CB_COL_TRIG; widths[2] = rw - CB_COL_NUM - CB_COL_TRIG - GetSystemMetrics(SM_CXVSCROLL) - 4;
    for (c = 0; c < 3; c++)
    {
      LVCOLUMNA col;

      ZeroMemory(&col, sizeof(col));
      col.mask = LVCF_TEXT | LVCF_WIDTH;
      col.pszText = (char *)titles[c];
      col.cx = widths[c];
      SendMessageA(h, LVM_INSERTCOLUMNA, c, (LPARAM)&col);
    }
  }
  gui_list_header_menu(h);
  ShowWindow(h, SW_HIDE);
  cb_update_bind(dlg);
}


/* The Ports tab: what is plugged into the console's two controller ports. */
static const char *const port_a_names[] = { "Nothing Connected", "Control Pad", "Mouse", "XE-1AP", "Activator",
                                            "Light Phaser", "Paddle Control", "Sports Pad", "Graphic Board", "Team Player",
                                            "EA 4-Way Play", "Master Tap", "Smash Controller" };
static const char *const port_b_names[] = { "Nothing Connected", "Control Pad", "Mouse", "Menacer", "Justifier",
                                            "XE-1AP", "Activator", "Light Phaser", "Paddle Control", "Team Player",
                                            "EA 4-Way Play", "Master Tap", "Smash Controller" };

/* The port devices apply the moment they are picked; Cancel gives them back as they were. */
static void input_ports_revert(void)
{
  int p;
  for (p = 0; p < 2; p++)
    if (gui_port_index(p) != inp.port_open[p])
      SendMessage(g_hwnd, WM_COMMAND, MAKEWPARAM((p ? IDM_INPUT_PORTB_BASE : IDM_INPUT_PORTA_BASE) + inp.port_open[p], 0), 0);
}

static void input_ports_fill(HWND dlg)
{
  int k;
  HWND a = GetDlgItem(dlg, IDC_INPUT_PORTA), b = GetDlgItem(dlg, IDC_INPUT_PORTB);

  SendMessage(a, CB_RESETCONTENT, 0, 0);
  SendMessage(b, CB_RESETCONTENT, 0, 0);
  for (k = 0; k < 13; k++)
  {
    SendMessageA(a, CB_ADDSTRING, 0, (LPARAM)port_a_names[k]);
    SendMessageA(b, CB_ADDSTRING, 0, (LPARAM)port_b_names[k]);
  }
  SendMessage(a, CB_SETCURSEL, (WPARAM)gui_port_index(0), 0);
  SendMessage(b, CB_SETCURSEL, (WPARAM)gui_port_index(1), 0);
}

/* Shows the Ports tab (on) or brings the pad pages back (off): every other control is hidden and remembered. */
static void input_ports_view(HWND dlg, int page)     /* 0 the pad, 1 Extras (the ports and the combos) */
{
  static HWND saved[160];
  static unsigned char vis[160];
  static int n;
  HWND c;
  int i;

  if (page == inp.ports) return;
  if (cbl.listen) cb_listen(dlg, 0);

  if (inp.ports == 0)
  {
    n = 0;
    for (c = GetWindow(dlg, GW_CHILD); c && n < 160; c = GetWindow(c, GW_HWNDNEXT))
    {
      int id = GetDlgCtrlID(c);
      if (id == IDC_INPUT_TAB || id == IDC_INPUT_HINT || id == IDC_INPUT_DEFAULTS || (id >= IDC_INPUT_PROFILES && id <= IDC_INPUT_PROFNEW) || id == IDOK || id == IDCANCEL || (id >= IDC_INPUT_PORTA_LABEL && id <= IDC_INPUT_PORTS_HINT) ||
          (id >= IDC_INPUT_CB_FIRST && id <= IDC_INPUT_CB_LAST) || CB_EXTRA_ID(id)) continue;
      saved[n] = c;
      vis[n++] = (unsigned char)IsWindowVisible(c);
      ShowWindow(c, SW_HIDE);
    }
  }
  else
  {
    for (i = IDC_INPUT_PORTA_LABEL; i <= IDC_INPUT_PORTS_HINT; i++) ShowWindow(GetDlgItem(dlg, i), SW_HIDE);
    for (i = IDC_INPUT_CB_FIRST; i <= IDC_INPUT_CB_LAST; i++) ShowWindow(GetDlgItem(dlg, i), SW_HIDE);
    for (i = IDC_CB_HOLDTIME; i <= IDC_CB_HELP; i++) ShowWindow(GetDlgItem(dlg, i), SW_HIDE);
  }

  inp.ports = page;
  SetDlgItemTextA(dlg, IDC_INPUT_HINT, page ? "" : INPUT_HINT_IDLE);
  {
    /* the message line: under the Profile row on the pad tabs, beside the OK button on the Extras one */
    const RECT *r = &hint_rc[page ? 1 : 0];
    MoveWindow(GetDlgItem(dlg, IDC_INPUT_HINT), r->left, r->top, r->right - r->left, r->bottom - r->top, TRUE);
  }
  if (page)
  {
    for (i = IDC_INPUT_PORTA_LABEL; i <= IDC_INPUT_PORTS_HINT; i++) ShowWindow(GetDlgItem(dlg, i), SW_SHOW);
    for (i = IDC_INPUT_CB_FIRST; i <= IDC_INPUT_CB_LAST; i++) ShowWindow(GetDlgItem(dlg, i), SW_SHOW);
    for (i = IDC_CB_HOLDTIME; i <= IDC_CB_HELP; i++) ShowWindow(GetDlgItem(dlg, i), SW_SHOW);
    input_ports_fill(dlg);
    if (cb_n) cb_show(dlg, 0); else cb_fill_list(dlg, -1);
  }
  else
  {
    for (i = 0; i < n; i++) if (vis[i]) ShowWindow(saved[i], SW_SHOWNA);
  }
  InvalidateRect(dlg, NULL, TRUE);
}

/****************************************************************************
 * Controls profiles: every key and button of both players, the turbo rate and the combos, saved under a name in
 * <exe>\profiles\<name>.cfg and loaded back from the Profiles... menu of the Controls window.
 ****************************************************************************/

#define PROF_MAX 40

static void prof_dir(char *out)
{
  lstrcpynA(out, osd_path("Profiles"), GUI_PATH_LEN);
  CreateDirectoryA(out, NULL);
}

static int prof_list(char names[][64], int max)
{
  char dir[GUI_PATH_LEN], pat[GUI_PATH_LEN + 8];
  WIN32_FIND_DATAA fd;
  HANDLE h;
  int n = 0;

  prof_dir(dir);
  wsprintfA(pat, "%s\\*.cfg", dir);
  h = FindFirstFileA(pat, &fd);
  if (h == INVALID_HANDLE_VALUE) return 0;
  do
  {
    char *dot;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    if (n >= max) break;
    lstrcpynA(names[n], fd.cFileName, 64);
    dot = strrchr(names[n], '.');
    if (dot) *dot = '\0';
    n++;
  } while (FindNextFileA(h, &fd));
  FindClose(h);
  return n;
}

static void prof_path(const char *name, char *out)
{
  char dir[GUI_PATH_LEN];
  prof_dir(dir);
  wsprintfA(out, "%s\\%s.cfg", dir, name);
}

static void prof_add_ints(char *out, int len, const char *key, const int *v, int n)
{
  char t[24];
  int i;
  lstrcatA(out, key); lstrcatA(out, "=");
  for (i = 0; i < n; i++) { wsprintfA(t, "%s%d", i ? "," : "", v[i]); lstrcatA(out, t); }
  lstrcatA(out, "\n");
  (void)len;
}

/* The text of a profile: what the Controls window holds now (also how the window tells that it was changed). */
static void prof_build(HWND dlg, char *out, int len)
{
  char t[96], k[16];
  int p;

  input_read_controls(dlg);
  inp.other[inp.player].map = inp.map;
  inp.other[inp.player].padtype = inp.padtype;
  inp.other[inp.player].deadzone = inp.deadzone;

  lstrcpyA(out, "# Genesis Plus EX controls profile\n");
  wsprintfA(t, "turbo_speed=%d\n", (int)SendDlgItemMessage(dlg, IDC_INPUT_TURBORATE, CB_GETCURSEL, 0, 0));
  lstrcatA(out, t);
  for (p = 0; p < 2; p++)
  {
    wsprintfA(t, "p%d.padtype=%d\np%d.deadzone=%d\np%d.turbo=%u\n", p, (int)inp.other[p].padtype, p, inp.other[p].deadzone, p, inp.other[p].map.turbo);
    lstrcatA(out, t);
    wsprintfA(k, "p%d.key", p);    prof_add_ints(out, len, k, inp.other[p].map.key, PAD_KEYS);
    wsprintfA(k, "p%d.button", p); prof_add_ints(out, len, k, inp.other[p].map.button, PAD_KEYS);
  }
  { char c[512]; if (!cb_serialize(c, sizeof(c))) c[0] = '\0'; lstrcatA(out, "combos="); lstrcatA(out, c); lstrcatA(out, "\n"); }
}

static unsigned prof_sig(HWND dlg)
{
  char buf[2048];
  unsigned h = 2166136261u;
  const char *s;
  buf[0] = '\0';
  prof_build(dlg, buf, sizeof(buf));
  for (s = buf; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
  return h;
}

static unsigned prof_clean_sig;       /* what the window held when the profile was loaded or saved */
static char prof_active[64];          /* the profile last loaded or saved, "" = none */

static int prof_exists(const char *name);
static int prof_any;                  /* a profile is saved (kept up to date by prof_refresh) */

/* The name in the Profile box, without the characters a file name cannot hold. */
static void prof_text(HWND dlg, char *out, int len)
{
  char raw[96], *s, *o;
  GetDlgItemTextA(dlg, IDC_INPUT_PROFILES, raw, sizeof(raw));
  for (s = o = raw; *s; s++) if (!strchr("\\/:*?\"<>|", *s)) *o++ = *s;
  *o = '\0';
  while (o > raw && o[-1] == ' ') *--o = '\0';
  for (s = raw; *s == ' '; s++) { }
  lstrcpynA(out, s, len);
  while (out[0] && out[lstrlenA(out) - 1] == '.') out[lstrlenA(out) - 1] = '\0';     /* Windows drops trailing dots */
  {
    /* CON, PRN, AUX, NUL, COM1-9 and LPT1-9 are devices, not files: saving "CON" appeared to work and wrote nothing. */
    static const char *const dev[] = { "CON", "PRN", "AUX", "NUL" };
    int i, bad = 0, n = lstrlenA(out);
    for (i = 0; i < 4; i++) if (!lstrcmpiA(out, dev[i])) bad = 1;
    if (n == 4 && (!_strnicmp(out, "COM", 3) || !_strnicmp(out, "LPT", 3)) && out[3] >= '1' && out[3] <= '9') bad = 1;
    if (bad && n + 1 < len) { out[n] = '_'; out[n + 1] = '\0'; }
  }
}

static void prof_enable(HWND dlg, int id, int on)
{
  HWND c = GetDlgItem(dlg, id);
  if (c && (IsWindowEnabled(c) != 0) != (on != 0)) EnableWindow(c, on ? TRUE : FALSE);
}

/* Load is for a saved profile (or Default), Delete for a saved profile, Save for any name but Default; a profile that is loaded
   and unchanged has nothing to save. `name` is the text to judge (NULL = what the box holds now). */
static void prof_update_buttons(HWND dlg, const char *name)
{
  char t[64];
  int exists, same, dirty = 0, any;

  if (name) lstrcpynA(t, name, sizeof(t)); else prof_text(dlg, t, sizeof(t));
  exists = t[0] && prof_exists(t);
  any = prof_any;       /* with no saved profile there is nothing to load or delete */
  same = prof_active[0] && !lstrcmpiA(t, prof_active);
  if (same) dirty = prof_sig(dlg) != prof_clean_sig;

  prof_enable(dlg, IDC_INPUT_PROFLOAD, any && exists);
  prof_enable(dlg, IDC_INPUT_PROFNEW, t[0] && !exists);                       /* a name nobody has yet */
  prof_enable(dlg, IDC_INPUT_PROFDELETE, exists);
  /* Save: the loaded profile after a change, or the loaded profile under the new name typed in the box (a rename) */
  prof_enable(dlg, IDC_INPUT_PROFSAVE, prof_active[0] && ((same && dirty) || (!same && t[0] && !exists)));
}

static void prof_update_save(HWND dlg) { prof_update_buttons(dlg, NULL); }

static int prof_save(HWND dlg, const char *name)
{
  char path[GUI_PATH_LEN], buf[2048];
  FILE *fp;

  buf[0] = '\0';
  prof_build(dlg, buf, sizeof(buf));
  prof_path(name, path);
  fp = fopen(path, "w");
  if (!fp) return 0;
  fputs(buf, fp);
  fclose(fp);
  prof_clean_sig = prof_sig(dlg);
  return 1;
}

static void prof_read_ints(const char *s, int *v, int n)
{
  int i;
  for (i = 0; i < n && *s; i++)
  {
    v[i] = (int)strtol(s, (char **)&s, 10);
    if (*s == ',') s++;
  }
}

static int prof_load(HWND dlg, const char *name)
{
  char path[GUI_PATH_LEN], line[600], combos[512];
  FILE *fp;
  int have_combos = 0, p;

  prof_path(name, path);
  fp = fopen(path, "r");
  if (!fp) return 0;
  combos[0] = '\0';
  input_read_controls(dlg);
  inp.other[inp.player].map = inp.map;
  inp.other[inp.player].padtype = inp.padtype;
  inp.other[inp.player].deadzone = inp.deadzone;

  while (fgets(line, sizeof(line), fp))
  {
    char *v = strchr(line, '='), *e;
    if (line[0] == '#' || !v) continue;
    *v++ = '\0';
    for (e = v + lstrlenA(v); e > v && (e[-1] == '\n' || e[-1] == '\r'); ) *--e = '\0';
    if (!lstrcmpA(line, "turbo_speed")) SendDlgItemMessage(dlg, IDC_INPUT_TURBORATE, CB_SETCURSEL, (WPARAM)(atoi(v) < 0 || atoi(v) > 4 ? 2 : atoi(v)), 0);
    else if (!lstrcmpA(line, "turbo_rate")) SendDlgItemMessage(dlg, IDC_INPUT_TURBORATE, CB_SETCURSEL, (WPARAM)(atoi(v) == 3 ? 4 : atoi(v) & 3), 0);      /* (a profile saved with four speeds) */
    else if (!lstrcmpA(line, "combos")) { lstrcpynA(combos, v, sizeof(combos)); have_combos = 1; }
    else if (line[0] == 'p' && (line[1] == '0' || line[1] == '1') && line[2] == '.')
    {
      p = line[1] - '0';
      if (!lstrcmpA(line + 3, "padtype")) inp.other[p].padtype = (uint8)atoi(v);
      else if (!lstrcmpA(line + 3, "deadzone")) inp.other[p].deadzone = atoi(v) < 0 ? 0 : (atoi(v) > 100 ? 100 : atoi(v));
      else if (!lstrcmpA(line + 3, "turbo")) inp.other[p].map.turbo = (unsigned)strtoul(v, NULL, 10);
      else if (!lstrcmpA(line + 3, "key")) prof_read_ints(v, inp.other[p].map.key, PAD_KEYS);
      else if (!lstrcmpA(line + 3, "button")) prof_read_ints(v, inp.other[p].map.button, PAD_KEYS);
    }
  }
  fclose(fp);

  /* the showing player's working copy follows (its device stays as chosen in the window) */
  {
    int dev = inp.map.device;
    char joy[64];
    lstrcpynA(joy, inp.map.joy_name, sizeof(joy));
    inp.map = inp.other[inp.player].map;
    inp.map.device = dev;
    lstrcpynA(inp.map.joy_name, joy, sizeof(inp.map.joy_name));
    inp.padtype = inp.other[inp.player].padtype;
    inp.deadzone = inp.other[inp.player].deadzone;
    inp.other[inp.player].map = inp.map;
  }
  input_load_controls(dlg);
  input_fill_list(dlg);
  if (have_combos)
  {
    cb_parse(combos);
    cb_sel = -1;
    cb_fill_list(dlg, cb_n ? 0 : -1);
    if (cb_n) cb_show(dlg, 0); else cb_update_bind(dlg);
  }
  prof_clean_sig = prof_sig(dlg);
  return 1;
}

/* No combos (the default). */
static void input_restore_combos(HWND dlg)
{
  ZeroMemory(cb_items, sizeof(cb_items));
  cb_n = 12;
  cb_sel = -1;
  cb_show(dlg, 0);              /* the first row is picked again, as when the tab opens (with none picked the buttons would be grayed) */
}

/* Puts the keys, buttons, pad types, deadzones, turbo rate and combos back to their defaults: of both players (and the turbo
   rate and the combos), or of one player only. */
static void input_restore_defaults(HWND dlg, int which)      /* which: -1 both players and the combos, 0 or 1 only that player */
{
  /* set_config_defaults() resets everything: the frontend settings, the emulation settings (config) and the port devices
     (input.system). Only the controls are wanted here, so the rest is put back. */
  t_gui_config saved_gui = gui;
  t_config     saved_cfg = config;
  uint8        saved_sys[2];
  int p;

  saved_sys[0] = input.system[0];
  saved_sys[1] = input.system[1];

  set_config_defaults();
  for (p = 0; p < 2; p++)
  {
    int dev = inp.other[p].map.device;
    char joy[64];
    if (which >= 0 && p != which) continue;
    lstrcpynA(joy, inp.other[p].map.joy_name, sizeof(joy));
    inp.other[p].map = gui.pad[p];
    inp.other[p].map.device = dev;                      /* which gamepad each player uses is not part of it */
    lstrcpynA(inp.other[p].map.joy_name, joy, sizeof(inp.other[p].map.joy_name));
    inp.other[p].padtype = config.input[p].padtype;
    inp.other[p].deadzone = gui.deadzone[p];
  }
  if (which < 0 || which == inp.player)         /* the tab that is showing follows */
  {
    int dev = inp.map.device;
    char joy[64];
    lstrcpynA(joy, inp.map.joy_name, sizeof(joy));
    inp.map = inp.other[inp.player].map;
    inp.map.device = dev;
    lstrcpynA(inp.map.joy_name, joy, sizeof(inp.map.joy_name));
    inp.padtype = inp.other[inp.player].padtype;
    inp.deadzone = inp.other[inp.player].deadzone;
  }

  gui = saved_gui;
  config = saved_cfg;
  input.system[0] = saved_sys[0];
  input.system[1] = saved_sys[1];
  if (which < 0 || which == inp.player)
  {
    input_load_controls(dlg);
    input_fill_list(dlg);
  }
  SendDlgItemMessage(dlg, IDC_INPUT_TURBORATE, CB_SETCURSEL, 2, 0);   /* Turbo Rate back to Medium (it is on the tab of each player) */
  if (which < 0) input_restore_combos(dlg);
}

/* Fills the Profile drop-down and puts `select` (or nothing) in its box. */
static void prof_refresh(HWND dlg, const char *select)
{
  char names[PROF_MAX][64];
  int n = prof_list(names, PROF_MAX), i;
  HWND c = GetDlgItem(dlg, IDC_INPUT_PROFILES);

  prof_any = n > 0;
  SendMessage(c, CB_RESETCONTENT, 0, 0);
  for (i = 0; i < n; i++) SendMessageA(c, CB_ADDSTRING, 0, (LPARAM)names[i]);      /* only the saved profiles: the list is empty when there are none */
  SetWindowTextA(c, select && select[0] ? select : "");
  lstrcpynA(prof_active, select ? select : "", sizeof(prof_active));
  prof_update_buttons(dlg, NULL);
}

static int prof_exists(const char *name)
{
  char path[GUI_PATH_LEN];
  prof_path(name, path);
  return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

static void prof_command(HWND dlg, int id, int code)
{
  char name[64];

  if (id == IDC_INPUT_PROFILES)
  {
    if (code == CBN_EDITCHANGE) prof_update_buttons(dlg, NULL);
    else if (code == CBN_SELCHANGE)
    {
      int sel = (int)SendDlgItemMessage(dlg, IDC_INPUT_PROFILES, CB_GETCURSEL, 0, 0);
      name[0] = '\0';
      if (sel >= 0) SendDlgItemMessageA(dlg, IDC_INPUT_PROFILES, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)name);
      prof_update_buttons(dlg, name);      /* (the box itself still shows the old text at this moment) */
    }
    return;
  }
  if (code != BN_CLICKED) return;
  prof_text(dlg, name, sizeof(name));

  if (id == IDC_INPUT_PROFLOAD)
  {
    if (prof_exists(name) && prof_load(dlg, name))
    {
      char msg[128];
      lstrcpynA(prof_active, name, sizeof(prof_active));
      wsprintfA(msg, "Profile \"%s\" loaded. Press OK to keep it.", name);
      SetDlgItemTextA(dlg, IDC_INPUT_HINT, msg);
      prof_update_buttons(dlg, NULL);
    }
    else SetDlgItemTextA(dlg, IDC_INPUT_HINT, "Could not read that profile.");
  }
  else if (id == IDC_INPUT_PROFNEW && name[0] && !prof_exists(name))
  {
    /* a new profile from what the window holds now, under the name typed in the box */
    SetDlgItemTextA(dlg, IDC_INPUT_HINT, prof_save(dlg, name) ? "New profile saved." : "Could not save the profile.");
    prof_refresh(dlg, name);
  }
  else if (id == IDC_INPUT_PROFSAVE && prof_active[0])
  {
    /* The loaded profile is saved; with another name in the box it is renamed to it (after asking). */
    if (lstrcmpiA(name, prof_active))
    {
      char q[256], from[GUI_PATH_LEN], to[GUI_PATH_LEN];
      if (!name[0] || prof_exists(name)) return;
      wsprintfA(q, "Rename \"%s\" to \"%s\" and save it?", prof_active, name);
      if (MessageBoxA(dlg, q, "Controls Profile", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
      prof_path(prof_active, from);
      prof_path(name, to);
      if (!MoveFileA(from, to)) { SetDlgItemTextA(dlg, IDC_INPUT_HINT, "Could not rename the profile."); return; }
    }
    SetDlgItemTextA(dlg, IDC_INPUT_HINT, prof_save(dlg, name) ? "Profile saved." : "Could not save the profile.");
    prof_refresh(dlg, name);
  }
  else if (id == IDC_INPUT_PROFDELETE && name[0] && prof_exists(name))
  {
    char path[GUI_PATH_LEN];
    if (MessageBoxA(dlg, "Delete this profile?", "Controls Profile", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    prof_path(name, path);
    DeleteFileA(path);
    SetDlgItemTextA(dlg, IDC_INPUT_HINT, "Profile deleted.");
    prof_refresh(dlg, NULL);
  }
}

/* With no saved profile there is nothing to drop down: the arrow and the keys that open the list do nothing. */
static LRESULT CALLBACK prof_combo_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref)
{
  (void)id; (void)ref;
  if (!prof_any)
  {
    if (m == WM_LBUTTONDOWN || m == WM_LBUTTONDBLCLK)
    {
      RECT rc;
      GetClientRect(h, &rc);
      if ((short)LOWORD(l) > rc.right - IS(26)) return 0;        /* the arrow */
    }
    else if (m == CB_SHOWDROPDOWN && w) return 0;
    else if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN) && (w == VK_F4 || (w == VK_DOWN && (GetKeyState(VK_MENU) & 0x8000)))) return 0;
  }
  if (m == WM_NCDESTROY) RemoveWindowSubclass(h, prof_combo_proc, 78);
  return DefSubclassProc(h, m, w, l);
}

/* Enter in the Profile box saves (or, when there is nothing to save, loads) instead of pressing OK. */
static LRESULT CALLBACK prof_edit_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref)
{
  HWND dlg = (HWND)ref;
  (void)id;
  if (m == WM_GETDLGCODE)
  {
    MSG *msg = (MSG *)l;
    if (msg && msg->message == WM_KEYDOWN && msg->wParam == VK_RETURN) return DLGC_WANTMESSAGE | DLGC_WANTALLKEYS;
  }
  else if (m == WM_KEYDOWN && w == VK_RETURN)
  {
    int btn = IsWindowEnabled(GetDlgItem(dlg, IDC_INPUT_PROFSAVE)) ? IDC_INPUT_PROFSAVE : IDC_INPUT_PROFLOAD;
    if (IsWindowEnabled(GetDlgItem(dlg, btn))) PostMessage(dlg, WM_COMMAND, MAKEWPARAM(btn, BN_CLICKED), (LPARAM)GetDlgItem(dlg, btn));
    return 0;
  }
  else if (m == WM_CHAR && w == VK_RETURN) return 0;
  else if (m == WM_NCDESTROY) RemoveWindowSubclass(h, prof_edit_proc, 77);
  return DefSubclassProc(h, m, w, l);
}

/* One mapping: its name (painted) to the left of the button that shows what it is set to. */
static void input_cell(HWND dlg, int id, int x, int y, int lw, int bw, int bh)
{
  input_add_label(input_label_text(id), x - IS(10), y + (bh - IS(16)) / 2, lw - IS(5) + IS(10), IS(16), DT_RIGHT);       /* (room to the left too: "Down:" must not be cut) */
  input_make(dlg, "BUTTON", "", WS_TABSTOP | BS_PUSHBUTTON | BS_MULTILINE, x + lw, y, bw, bh, IDC_INPUT_LABEL_BASE + id);
}

/* The whole window is built here: a Mega Drive pad laid out in titled frames (the D-pad with its deadzone, Start and Mode,
   the six buttons with their Turbo ticks) around the controller picture, and an Extras tab for the ports and the combos. */
static void input_build(HWND dlg)
{
  static const char *const player_tabs[] = { "Player 1", "Player 2", "Extras" };
  int M = IS(14), th = settings_tab_height() * (gui.large_ui ? 121 : 100) / 100;
  int LW = IS(264), PW = gui.large_ui ? IS(250) : 360, RW = IS(330), gap = IS(10), FH = IS(252);
  int cw = M + LW + gap + PW + gap + RW + M;
  int bh = IS(28);
  int y0 = th + M, yT = y0 + IS(42), y, picw, pich, picx, picy, rx;
  RECT rc, wr;
  HWND tab;
  DWORD style = (DWORD)GetWindowLongPtr(dlg, GWL_STYLE), ex = (DWORD)GetWindowLongPtr(dlg, GWL_EXSTYLE);

  if (!inp.font)
  {
    LOGFONTA lf;
    if (GetObjectA((HFONT)SendMessage(dlg, WM_GETFONT, 0, 0), sizeof(lf), &lf))
    {
      /* The Large interface already makes the dialog font bigger; scaling it again looked heavy, so it is used as is. */
      if (!gui.large_ui) lf.lfHeight = lf.lfHeight * 110 / 100;
      lf.lfWeight = FW_NORMAL;
      lf.lfWidth = 0;
      inp.font = CreateFontIndirectA(&lf);
      if (inp.font) SendMessage(dlg, WM_SETFONT, (WPARAM)inp.font, FALSE);
    }
  }
  inp.labels = 0;
  settings_register_classes();
  tab = input_make(dlg, "GPSettingsTabs", "", WS_TABSTOP, 0, 0, cw, th, IDC_INPUT_TAB);
  settings_set_tabs(tab, player_tabs, 3);
  SetWindowLongPtr(tab, GWLP_USERDATA, inp.player);

  /* The Extras tab: the ports (here) and the combos (cb_build); shown only on that tab. */
  {
    HWND h;
    SetRect(&cb_frame[1], M, y0 + IS(8), M + LW + gap + PW, y0 + IS(84));
    h = input_make(dlg, "STATIC", "Port A Device:", SS_LEFT, M + IS(14), y0 + IS(30), IS(98), IS(16), IDC_INPUT_PORTA_LABEL);        ShowWindow(h, SW_HIDE);
    h = input_make(dlg, "COMBOBOX", "", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP, M + IS(114), y0 + IS(26), IS(250), IS(240), IDC_INPUT_PORTA); ShowWindow(h, SW_HIDE);
    h = input_make(dlg, "STATIC", "Port B Device:", SS_LEFT, M + IS(14), y0 + IS(58), IS(98), IS(16), IDC_INPUT_PORTB_LABEL);       ShowWindow(h, SW_HIDE);
    h = input_make(dlg, "COMBOBOX", "", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP, M + IS(114), y0 + IS(54), IS(250), IS(240), IDC_INPUT_PORTB); ShowWindow(h, SW_HIDE);
    h = input_make(dlg, "STATIC", "", SS_LEFT, 0, 0, 0, 0, IDC_INPUT_PORTS_HINT);
    ShowWindow(h, SW_HIDE);
  }
  cb_build(dlg, M, y0 + IS(92), cw);

  input_make(dlg, "STATIC", "Input Devices:", SS_LEFT, M, y0 + IS(4), IS(88), IS(16), IDC_INPUT_DEVICELABEL);
  input_make(dlg, "COMBOBOX", "", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP,
             M + IS(92), y0, IS(230), IS(220), IDC_INPUT_DEVICE);
  {
    /* A refresh arrow instead of a word. */
    static HFONT sym;
    HWND rb = input_make(dlg, "BUTTON", "", WS_TABSTOP | BS_PUSHBUTTON, M + IS(92) + IS(238), y0 - IS(1), IS(32), IS(24), IDC_INPUT_REFRESH);
    if (!sym) sym = CreateFontW(-IS(16), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI Symbol");
    SetWindowTextW(rb, L"\x21BB");
    if (sym) SendMessage(rb, WM_SETFONT, (WPARAM)sym, TRUE);
    {
      /* level with the Input Devices drop-down, same height */
      RECT cr;
      POINT p0;
      HWND combo = GetDlgItem(dlg, IDC_INPUT_DEVICE);
      if (combo && GetWindowRect(combo, &cr))
      {
        p0.x = cr.left; p0.y = cr.top;
        ScreenToClient(dlg, &p0);
        MoveWindow(rb, M + IS(92) + IS(238), p0.y, IS(32), cr.bottom - cr.top, TRUE);
      }
    }
  }
  /* The right column starts with the Controller Model and the Profile (box and buttons), as wide as the frame under them. */
  rx = M + LW + gap + PW + gap;
  input_make(dlg, "STATIC", "Controller Model:", SS_LEFT, rx, y0 + IS(4), IS(112), IS(16), IDC_INPUT_PADTYPE_LABEL);
  input_make(dlg, "COMBOBOX", "", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP,
             rx + IS(114), y0, RW - IS(114), IS(160), IDC_INPUT_PADTYPE);
  {
    int pbw = (RW - 3 * IS(6)) / 4, pgap = IS(6);
    input_make(dlg, "STATIC", "Profile:", SS_LEFT, rx, y0 + IS(34), IS(52), IS(18), IDC_INPUT_PROFLABEL);
    input_make(dlg, "COMBOBOX", "", CBS_DROPDOWN | CBS_AUTOHSCROLL | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP, rx + IS(56), y0 + IS(30), RW - IS(56), IS(220), IDC_INPUT_PROFILES);
    input_make(dlg, "BUTTON", "&Load", WS_TABSTOP | BS_PUSHBUTTON, rx, y0 + IS(60), pbw, IS(24), IDC_INPUT_PROFLOAD);
    input_make(dlg, "BUTTON", "&New", WS_TABSTOP | BS_PUSHBUTTON, rx + pbw + pgap, y0 + IS(60), pbw, IS(24), IDC_INPUT_PROFNEW);
    input_make(dlg, "BUTTON", "&Save", WS_TABSTOP | BS_PUSHBUTTON, rx + 2 * (pbw + pgap), y0 + IS(60), pbw, IS(24), IDC_INPUT_PROFSAVE);
    input_make(dlg, "BUTTON", "&Delete", WS_TABSTOP | BS_PUSHBUTTON, rx + 3 * (pbw + pgap), y0 + IS(60), pbw, IS(24), IDC_INPUT_PROFDELETE);
    prof_refresh(dlg, NULL);
    {
      COMBOBOXINFO cbi;
      cbi.cbSize = sizeof(cbi);
      if (GetComboBoxInfo(GetDlgItem(dlg, IDC_INPUT_PROFILES), &cbi) && cbi.hwndItem) SetWindowSubclass(cbi.hwndItem, prof_edit_proc, 77, (DWORD_PTR)dlg);
      SetWindowSubclass(GetDlgItem(dlg, IDC_INPUT_PROFILES), prof_combo_proc, 78, 0);
    }
  }

  /* Left: the D-pad (a diamond) with the stick's deadzone, and Start and Mode. */
  SetRect(&pad_frame[0], M, yT, M + LW, yT + IS(182));
  SetRect(&pad_frame[1], M, yT + IS(190), M + LW, yT + FH);
  {
    int dlw = IS(34), dbw = IS(86), cwid = dlw + dbw, ty = yT + IS(26);       /* the four buttons are alike; wide enough for "D-Pad Down /" on the first of two lines */
    int dbh = IS(40), dpitch = IS(42);      /* these four are a little taller: "D-Pad Right / LS Right" is two lines */
    int midud = M + (LW - cwid) / 2;        /* Up and Down in the middle, over each other */
    input_cell(dlg, PAD_UP,    midud, ty, dlw, dbw, dbh);
    input_cell(dlg, PAD_LEFT,  M + IS(8), ty + dpitch, dlw, dbw, dbh);
    input_cell(dlg, PAD_RIGHT, M + LW - IS(8) - cwid, ty + dpitch, dlw, dbw, dbh);
    input_cell(dlg, PAD_DOWN,  midud, ty + 2 * dpitch, dlw, dbw, dbh);

    y = ty + 2 * dpitch + dbh + IS(4);
    input_make(dlg, "STATIC", "Deadzone:", SS_LEFT, M + IS(12), y + IS(4), IS(68), IS(16), IDC_INPUT_DEADZONE_LABEL);
    input_make(dlg, "msctls_trackbar32", "", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, M + IS(80), y, IS(122), IS(24), IDC_INPUT_DEADZONE);
    input_make(dlg, "STATIC", "", SS_LEFT, M + IS(206), y + IS(4), IS(42), IS(16), IDC_INPUT_DEADZONE_TEXT);

    ty = yT + IS(190) + IS(26);
    input_cell(dlg, PAD_START, M + IS(12), ty, IS(40), IS(76), bh);
    input_cell(dlg, PAD_MODE,  M + LW - IS(12) - IS(116), ty, IS(40), IS(76), bh);
  }

  /* Right: X Y Z over A B C, each with its Turbo tick under it, and the turbo rate. */
  SetRect(&pad_frame[2], rx, y0 + IS(92), rx + RW, yT + FH);
  {
    static const int face[2][3] = { { PAD_X, PAD_Y, PAD_Z }, { PAD_A, PAD_B, PAD_C } };
    int r, c;
    for (r = 0; r < 2; r++)
      for (c = 0; c < 3; c++)
      {
        int id = face[r][c], x = rx + IS(12) + c * IS(104), by = y0 + IS(92) + IS(26) + r * IS(62);
        input_cell(dlg, id, x, by, IS(16), IS(80), bh);
        input_make(dlg, "BUTTON", "Turbo", WS_TABSTOP | BS_AUTOCHECKBOX, x + IS(16) + (IS(80) - IS(68)) / 2, by + bh + IS(4), IS(68), IS(20), IDC_INPUT_TURBO_BASE + id);   /* centered under its button */
      }
    y = y0 + IS(92) + IS(26) + 2 * IS(62) + IS(4);
    input_make(dlg, "STATIC", "Turbo Rate:", SS_LEFT, rx + IS(12), y + IS(4), IS(72), IS(16), IDC_INPUT_TURBORATE_LABEL);
    input_make(dlg, "COMBOBOX", "", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP, rx + IS(88), y, IS(110), IS(160), IDC_INPUT_TURBORATE);
    {
      HWND tr = GetDlgItem(dlg, IDC_INPUT_TURBORATE);
      SendMessageA(tr, CB_ADDSTRING, 0, (LPARAM)"Slower");
      SendMessageA(tr, CB_ADDSTRING, 0, (LPARAM)"Slow");
      SendMessageA(tr, CB_ADDSTRING, 0, (LPARAM)"Medium");
      SendMessageA(tr, CB_ADDSTRING, 0, (LPARAM)"Fast");
      SendMessageA(tr, CB_ADDSTRING, 0, (LPARAM)"Faster");
      SendMessage(tr, CB_SETCURSEL, (WPARAM)(gui.turbo_rate < 0 || gui.turbo_rate > 4 ? 2 : gui.turbo_rate), 0);
    }
  }

  /* The controller picture, between the two columns. */
  picw = PW;
  pich = picw * 206 / 400;
  picx = M + LW + gap;
  picy = yT + (FH - pich) / 2;
  SetRect(&inp.pic, picx, picy, picx + picw, picy + pich);
  if (inp.picture) DeleteObject(inp.picture);
  inp.picture = input_make_picture(picw, pich);

  y = yT + FH + IS(12);

  /* The message line (profile messages, "press a key for ...", test mode): under the Profile row on the pad tabs, in the
     row of the OK button on the Extras tab, where the pad tabs' buttons are not. */
  SetRect(&hint_rc[0], M, y, cw - M, y + IS(18));
  input_make(dlg, "STATIC", INPUT_HINT_IDLE, SS_LEFT | SS_ENDELLIPSIS, M, y, cw - 2 * M, IS(18), IDC_INPUT_HINT);

  y += IS(26);
  SetRect(&hint_rc[1], M + IS(164), y + IS(3), cw - M - IS(190) - IS(132), y + IS(21));
  {
    input_make(dlg, "BUTTON", "Restore Defaults", WS_TABSTOP | BS_OWNERDRAW, M, y, IS(140), IS(24), IDC_INPUT_DEFAULTS);        /* drawn with an arrow: a list opens */
  }
  input_make(dlg, "BUTTON", "A&uto Assign...", WS_TABSTOP | BS_PUSHBUTTON, M + IS(164), y, IS(120), IS(24), IDC_INPUT_AUTOASSIGN);
  input_make(dlg, "BUTTON", "&Test Keys", WS_TABSTOP | BS_AUTOCHECKBOX, M + IS(164) + IS(298), y + IS(2), IS(100), IS(20), IDC_INPUT_TEST);
  input_make(dlg, "BUTTON", "Gamepad &Hotkeys...", WS_TABSTOP | BS_PUSHBUTTON, M + IS(164) + IS(128), y, IS(160), IS(24), IDC_INPUT_PADHK);
  input_make(dlg, "BUTTON", "OK", WS_TABSTOP | BS_DEFPUSHBUTTON, cw - M - IS(168), y, IS(80), IS(24), IDOK);
  input_make(dlg, "BUTTON", "Cancel", WS_TABSTOP | BS_PUSHBUTTON, cw - M - IS(80), y, IS(80), IS(24), IDCANCEL);

  {
    static const char *const how = " Click it, then press the key or gamepad button you want. Right-click to set or clear it. A B C X Y Z have a Turbo tick beside them.";
    static char map_tip[PAD_KEYS][200];
    static const char *const what[PAD_KEYS] =
    {
      "D-Pad Up.", "D-Pad Down.", "D-Pad Left.", "D-Pad Right.", "Button A.", "Button B.", "Button C.",
      "Button X (6-button pads).", "Button Y (6-button pads).", "Button Z (6-button pads).",
      "The Start button.", "The Mode button (6-button pads)."
    };
    static const TipDef tips[] =
    {
      { IDC_INPUT_TAB,      NULL, "Switch between Player 1 and Player 2." },
      { IDC_INPUT_DEVICE,   "Input Devices:", "Which device controls this player: the keyboard only, a gamepad, or a joystick." },
      { IDC_INPUT_PADTYPE,  "Controller Model:", "Which Mega Drive pad the game sees for this player: Match the Game (the default), or force a 3-Button, 6-Button (adds X, Y, Z and Mode) or 2-Button pad. Only change it if a game misbehaves with the default." },
      { IDC_INPUT_DEADZONE, "Deadzone:", "How far a thumbstick has to move off-center before it registers at all." },
      { IDC_INPUT_REFRESH, NULL, "Refresh: look for gamepads and joysticks again (after plugging one in or out)." },
      { IDC_INPUT_TURBORATE, "Turbo Rate:", "How fast a Turbo button fires while it is held (set for A, B, C, X, Y or Z with a right-click). Slower presses it 7.5 times a second, Slow 10, Medium 15 (the default), Fast 22.5 and Faster 30 (the most a game can see at 60 frames a second)." },
      { IDC_INPUT_DEFAULTS, NULL, "Choose Player 1, Player 2 or Extras Tab: only that one goes back to its defaults (OK keeps the change)." },
      { IDC_INPUT_AUTOASSIGN, NULL, "Go through the pad buttons one after another, pressing a key or button for each." },
      { IDC_INPUT_TEST, NULL, "Try your keys: while ticked, the keyboard no longer moves around or presses buttons in this window, so you can press Enter, Tab, Esc and the arrows and see the pad spots light." },
      { IDC_INPUT_PADHK, NULL, "Choose which gamepad button runs each shortcut: fast forward, rewind, save and load state, previous and next slot." },
      { IDC_INPUT_PORTA, "Port A Device:", "What is plugged into the console's first controller port: a pad, mouse, light gun, Team Player and so on. Netplay needs a plain Control Pad on both ports." },
      { IDC_INPUT_PORTB, "Port B Device:", "What is plugged into the console's second controller port: a pad, mouse, light gun, Team Player and so on. Netplay needs a plain Control Pad on both ports." },
      { IDC_INPUT_PROFILES, "Profile:", "A saved set of controls: both players' keys and buttons, pad types, deadzones, turbo buttons and rate, and the combos (not the ports). Pick a saved profile from the list, or type a name for a new one. Restore Defaults puts the program's own controls back." },
      { IDC_INPUT_PROFLOAD, NULL, "Load the profile named in the box into this window (OK keeps it). Grayed when no profile is saved." },
      { IDC_INPUT_PROFNEW, NULL, "Make a new profile from this window's controls, under the name typed in the box." },
      { IDC_INPUT_PROFSAVE, NULL, "Save this window's controls into the loaded profile. If you typed another name in the box, it renames the loaded profile to it (it asks)." },
      { IDC_INPUT_PROFDELETE, NULL, "Delete the profile named in the box (it asks first)." },
      { IDC_CB_HELP,        NULL, "A short explanation of how to make a combo, with an example." },
      { IDC_CB_BIND,        "Trigger:", "The key or gamepad button that sets the combo off. Click here, then press it (Esc or a right click cancels). A letter or digit, F1-F12, Num0-Num9, Space, an arrow key or an XInput pad button can be used." },
      { IDC_CB_HOLDTIME,    "Hold:", "Click it after an action button to make that action Held: it stays down by itself once the trigger has been pressed, for the time you choose in the menu (or until you press that button yourself). Shown as Down(Hold 1.0s)." },
      { IDC_CB_PAUSE,       "Pause:", "Click it between two action buttons to wait before the next action starts, for the time you choose in the menu. Shown as Pause 0.5s. Without one, the action after a Held one starts 70 ms later (Down, then A)." },
      { IDC_CB_UNDO,        NULL, "Take the last action (and the pause before it) off the end of the combo." },
      { IDC_CB_DEL,         NULL, "Delete the combo selected in the list (the row being worked on)." },
      { IDC_CB_LIST,        "Saved Combos:", "The combos set up so far: the row number, the trigger and the actions. Pick one to add to it or to change it." },
      { IDOK,               NULL, "Save the controls of both players." },
      { IDCANCEL,           NULL, "Close without saving (the port devices go back as they were)." },
    };
    enum { NFIXED = (int)(sizeof(tips) / sizeof(tips[0])) };
    TipDef all[NFIXED + PAD_KEYS + 6 + 12];
    int k;

    memcpy(all, tips, sizeof(tips));
    for (k = 0; k < PAD_KEYS; k++)
    {
      wsprintfA(map_tip[k], "%s%s", what[k], how);
      all[NFIXED + k].id = IDC_INPUT_LABEL_BASE + k;
      all[NFIXED + k].label = input_label_text(k);
      all[NFIXED + k].tip = map_tip[k];
    }
    {
      static const int turbo_btn[6] = { PAD_A, PAD_B, PAD_C, PAD_X, PAD_Y, PAD_Z };
      static char turbo_tip[6][120];
      for (k = 0; k < 6; k++)
      {
        wsprintfA(turbo_tip[k], "Turbo for button %s: while it is held, the button fires on and off repeatedly (Turbo Rate sets how fast).", input_list_entry_name(turbo_btn[k]));
        all[NFIXED + PAD_KEYS + k].id = IDC_INPUT_TURBO_BASE + turbo_btn[k];
        all[NFIXED + PAD_KEYS + k].label = "Turbo:";
        all[NFIXED + PAD_KEYS + k].tip = turbo_tip[k];
      }
    }
    {
      static char act_tip[12][200];
      for (k = 0; k < 12; k++)
      {
        wsprintfA(act_tip[k], "%s: adds it to the combo on the trigger above (the combo is made on the first click). Then Hold and Pause shape how it is pressed.", cb_game[k]);
        all[NFIXED + PAD_KEYS + 6 + k].id = IDC_CB_G0 + k;
        all[NFIXED + PAD_KEYS + 6 + k].label = "Action:";
        all[NFIXED + PAD_KEYS + 6 + k].tip = act_tip[k];
      }
    }
    dlg_add_tips(dlg, all, NFIXED + PAD_KEYS + 6 + 12);
  }

  /* Size the window to that client area and center it on its owner. */
  SetRect(&rc, 0, 0, cw, y + IS(24) + M);
  AdjustWindowRectEx(&rc, style, FALSE, ex);
  {
    HWND owner = GetWindow(dlg, GW_OWNER);
    int w = rc.right - rc.left, h = rc.bottom - rc.top, x, yy;
    if (!owner || !IsWindowVisible(owner)) owner = GetDesktopWindow();
    GetWindowRect(owner, &wr);
    x = wr.left + ((wr.right - wr.left) - w) / 2;
    yy = wr.top + ((wr.bottom - wr.top) - h) / 2;
    if (x < 0) x = 0;
    if (yy < 0) yy = 0;
    SetWindowPos(dlg, NULL, x, yy, w, h, SWP_NOZORDER);
  }
}

/* In test mode every key press is taken before the dialog sees it, so Enter does not Save, Esc
   does not Cancel and Tab does not move the focus. The pad spots light from the keys' real state
   (GetAsyncKeyState), which this does not touch. */
static HHOOK input_hook;

static LRESULT CALLBACK input_key_hook(int code, WPARAM wp, LPARAM lp)
{
  if (code >= 0 && inp.test && inp.dlg && GetActiveWindow() == inp.dlg) return 1;
  return CallNextHookEx(input_hook, code, wp, lp);
}

/* The refresh button, level with and as tall as the Input Devices drop-down beside it (measured once the window is fully built and themed). */
static void input_align_refresh(HWND dlg)
{
  static const int pairs[][2] = { { IDC_INPUT_DEVICELABEL, IDC_INPUT_DEVICE }, { IDC_INPUT_PADTYPE_LABEL, IDC_INPUT_PADTYPE }, { IDC_INPUT_PROFLABEL, IDC_INPUT_PROFILES },
                                  { IDC_INPUT_TURBORATE_LABEL, IDC_INPUT_TURBORATE }, { IDC_INPUT_PORTA_LABEL, IDC_INPUT_PORTA }, { IDC_INPUT_PORTB_LABEL, IDC_INPUT_PORTB } };
  HWND combo = GetDlgItem(dlg, IDC_INPUT_DEVICE), rb = GetDlgItem(dlg, IDC_INPUT_REFRESH);
  RECT cr, br;
  POINT p0;
  int h, itemh, k;

  /* Each label is as tall as its drop-down, its text centered in that height (the drop-downs are taller in the Large interface). */
  for (k = 0; k < (int)(sizeof(pairs) / sizeof(pairs[0])); k++)
  {
    HWND lab = GetDlgItem(dlg, pairs[k][0]), box = GetDlgItem(dlg, pairs[k][1]);
    RECT lr;
    POINT lp;

    if (!lab || !box || !GetWindowRect(box, &cr) || !GetWindowRect(lab, &lr)) continue;
    lp.x = lr.left; lp.y = cr.top;
    ScreenToClient(dlg, &lp);
    SetWindowLongPtr(lab, GWL_STYLE, GetWindowLongPtr(lab, GWL_STYLE) | SS_CENTERIMAGE);
    MoveWindow(lab, lp.x, lp.y, lr.right - lr.left, cr.bottom - cr.top, TRUE);
  }

  if (!combo || !rb || !GetWindowRect(combo, &cr) || !GetWindowRect(rb, &br)) return;
  itemh = (int)SendMessage(combo, CB_GETITEMHEIGHT, (WPARAM)-1, 0);
  h = cr.bottom - cr.top;
  if (itemh > 0 && itemh + 2 * GetSystemMetrics(SM_CYEDGE) > h) h = itemh + 2 * GetSystemMetrics(SM_CYEDGE);
  p0.x = br.left; p0.y = cr.top;
  ScreenToClient(dlg, &p0);
  /* A themed push button paints a little inside its window, a drop-down paints right to its edge: the button window is
     grown by 2 px on each side so the two look the same height. */
  MoveWindow(rb, p0.x, p0.y - 2, br.right - br.left, h + 4, TRUE);
}

static INT_PTR CALLBACK input_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
    case WM_NOTIFY:
    {
      const NMHDR *nh = (const NMHDR *)lp;

      if (nh->idFrom == IDC_CB_LIST && !cb_filling)
      {
        if (nh->code == LVN_ITEMCHANGED && (((const NMLISTVIEW *)lp)->uChanged & LVIF_STATE) &&
            (((const NMLISTVIEW *)lp)->uNewState & LVIS_SELECTED) && !(((const NMLISTVIEW *)lp)->uOldState & LVIS_SELECTED))
          cb_show(dlg, ((const NMLISTVIEW *)lp)->iItem);
        else if (nh->code == NM_CLICK)
        {
          /* A click below the last row would leave nothing chosen while a combo is being worked on. */
          LVHITTESTINFO hit;

          ZeroMemory(&hit, sizeof(hit));
          GetCursorPos(&hit.pt);
          ScreenToClient(nh->hwndFrom, &hit.pt);
          if (ListView_HitTest(nh->hwndFrom, &hit) < 0 && cb_work() >= 0) cb_fill_list(dlg, cb_work());
        }
      }
      return FALSE;
    }

    case WM_DRAWITEM:
    {
      DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT *)lp;
      if (di->CtlType == ODT_BUTTON && (di->CtlID == (UINT)IDC_INPUT_DEFAULTS || di->CtlID == (UINT)IDC_CB_HOLDTIME || di->CtlID == (UINT)IDC_CB_PAUSE)) { theme_draw_dropdown_button(di); return TRUE; }
      if (di->CtlType == ODT_BUTTON && di->CtlID >= (UINT)IDC_INPUT_LABEL_BASE && di->CtlID < (UINT)(IDC_INPUT_LABEL_BASE + PAD_KEYS))
      {
        /* the button that is waiting for a key: the color of a selected row, in both themes */
        int dark = theme_is_dark();
        COLORREF bg = theme_accent_bg();
        COLORREF fg = dark ? RGB(255, 255, 255) : GetSysColor(COLOR_HIGHLIGHTTEXT);
        HBRUSH bb = CreateSolidBrush(bg), eb = CreateSolidBrush(theme_accent_edge());
        RECT r = di->rcItem;
        char text[64];

        FillRect(di->hDC, &r, bb);
        FrameRect(di->hDC, &r, eb);
        GetWindowTextA(di->hwndItem, text, sizeof(text));
        SetBkMode(di->hDC, TRANSPARENT);
        SetTextColor(di->hDC, fg);
        DrawTextA(di->hDC, text, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
        DeleteObject(bb); DeleteObject(eb);
        return TRUE;
      }
      return FALSE;
    }

    case WM_INITDIALOG:
    {
      inp.dlg = dlg;
      input_hook = SetWindowsHookExA(WH_KEYBOARD, input_key_hook, NULL, GetCurrentThreadId());
      SetWindowTextA(dlg, "Controls");
      input_build(dlg);
      input_load_controls(dlg);
      input_fill_list(dlg);
      input_update_identify_hint(dlg);
      if (input_start_page == 3)
      {
        SetWindowLongPtr(GetDlgItem(dlg, IDC_INPUT_TAB), GWLP_USERDATA, 2);
        InvalidateRect(GetDlgItem(dlg, IDC_INPUT_TAB), NULL, TRUE);
        input_ports_view(dlg, 1);
      }
      SetTimer(dlg, 2, 30, NULL);   /* keeps the live presses on the picture current while the dialog is open */

      /* Explicit, not left to default tab-order focus: a stray Return still
         on its way from the menu would otherwise press a mapping button and
         start listening. The tab strip takes focus instead. */
      SetFocus(GetDlgItem(dlg, IDC_INPUT_TAB));
      SendMessage(dlg, DM_SETDEFID, (WPARAM)IDOK, 0);

      /* Applied last, after focus is already settled -- SWP_FRAMECHANGED
         inside this can trigger synchronous frame recalculation, which
         risks pumping other pending messages before focus was parked. */
      theme_apply_to_window(dlg);
      input_align_refresh(dlg);
      if (pico_active())                         /* a 32X game: PicoDrive uses six-button pads on both ports, whatever is chosen here */
      {
        static const int ids[] = { IDC_INPUT_PORTA, IDC_INPUT_PORTA_LABEL, IDC_INPUT_PORTB, IDC_INPUT_PORTB_LABEL, IDC_INPUT_PADTYPE, IDC_INPUT_PADTYPE_LABEL };
        int k;
        for (k = 0; k < (int)(sizeof(ids) / sizeof(ids[0])); k++) EnableWindow(GetDlgItem(dlg, ids[k]), FALSE);
      }
      return FALSE;
    }

    case WM_PAINT:
      input_paint(dlg);
      return TRUE;

    case WM_DESTROY:
      if (input_hook) { UnhookWindowsHookEx(input_hook); input_hook = NULL; }
      inp.test = 0;
      if (inp.picture) { DeleteObject(inp.picture); inp.picture = NULL; }
      if (inp.font) { DeleteObject(inp.font); inp.font = NULL; }
      return FALSE;

    case WM_TIMER:
      if (wp == 1)      { if (inp.listen != LISTEN_OFF) input_capture_tick(dlg); }
      else if (wp == 2)
      {
        static int blink_phase;
        int ph = ring_frame(GetTickCount(), NULL);
        if (inp.listen != LISTEN_OFF && ph != blink_phase) { blink_phase = ph; input_redraw_picture(dlg); }
        input_update_live(dlg);
      }
      else if (wp == 3) { cb_tick(dlg); }
      if (wp == 2 && inp.listen == LISTEN_OFF)
      {
        static int tick;
        if (++tick % 10 == 0) prof_update_save(dlg);          /* it looks at the disk: a third of a second is often enough */
      }
      return TRUE;

    case WM_HSCROLL:
    {
      HWND bar = (HWND)lp;

      if (GetDlgCtrlID(bar) == IDC_INPUT_DEADZONE)
      {
        char buf[16];
        inp.deadzone = (int)SendMessage(bar, TBM_GETPOS, 0, 0);
        wsprintfA(buf, "%d%%", inp.deadzone);
        SetDlgItemTextA(dlg, IDC_INPUT_DEADZONE_TEXT, buf);
      }
      return TRUE;
    }

    case WM_LBUTTONDOWN:
    {
      /* A click on a button's spot on the picture starts assigning it. */
      RECT rc = input_pic_rc();
      int mx = (short)LOWORD(lp), my = (short)HIWORD(lp), i, best = -1, bestd = IS(16) * IS(16);
      POINT pt;
      pt.x = mx; pt.y = my;
      if (inp.listen != LISTEN_OFF || inp.ports || !PtInRect(&rc, pt)) return FALSE;
      for (i = 0; i < PAD_KEYS; i++)
      {
        int dx = mx - (rc.left + (rc.right - rc.left) * input_hotspot[i][0] / 1000);
        int dy = my - (rc.top  + (rc.bottom - rc.top) * input_hotspot[i][1] / 1000);
        if (dx * dx + dy * dy < bestd) { bestd = dx * dx + dy * dy; best = i; }
      }
      if (best >= 0)
      {
        input_read_controls(dlg);
        inp.sel = best;
        inp.auto_advance = 0;
        input_set_listening(dlg, 1);
      }
      return TRUE;
    }

    case WM_CONTEXTMENU:
    {
      int id = GetDlgCtrlID((HWND)wp);
      if (inp.listen != LISTEN_OFF || cbl.listen || GetTickCount() - wait_cancel_tick < 600) return TRUE;
      if (id >= IDC_INPUT_LABEL_BASE && id < IDC_INPUT_LABEL_BASE + input_list_count() && inp.listen == LISTEN_OFF)
      {
        int b = id - IDC_INPUT_LABEL_BASE;
        HMENU m = CreatePopupMenu();
        POINT pt;
        int cmd;
        GetCursorPos(&pt);
        if (lp != (LPARAM)-1) { pt.x = (short)LOWORD(lp); pt.y = (short)HIWORD(lp); }
        AppendMenuA(m, MF_STRING, 1, "Set");
        AppendMenuA(m, MF_STRING, 2, "Clear");
        cmd = (int)gui_popup(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y);
        DestroyMenu(m);
        if (cmd == 1)
        {
          input_read_controls(dlg);
          inp.sel = b;
          inp.auto_advance = 0;
          input_set_listening(dlg, 1);
        }
        else if (cmd == 2) input_clear(dlg, b);
      }
      return TRUE;
    }

    case WM_CTLCOLORDLG:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    {
      HBRUSH br = theme_ctlcolor((HDC)wp);
      if (br) return (LRESULT)br;
      break;
    }

    case WM_COMMAND:
    {
      int id = LOWORD(wp);

      if (((id >= IDC_INPUT_CB_FIRST && id <= IDC_INPUT_CB_LAST) || CB_EXTRA_ID(id)) && cb_command(dlg, id, HIWORD(wp))) return TRUE;
      if (id >= IDC_INPUT_PROFILES && id <= IDC_INPUT_PROFNEW) { if (inp.listen == LISTEN_OFF && !cbl.listen) prof_command(dlg, id, HIWORD(wp)); return TRUE; }

      if (id >= IDC_INPUT_TURBO_BASE && id < IDC_INPUT_TURBO_BASE + PAD_KEYS)
      {
        if (HIWORD(wp) == BN_CLICKED)
        {
          unsigned bit = 1u << (id - IDC_INPUT_TURBO_BASE);
          if (IsDlgButtonChecked(dlg, id) == BST_CHECKED) inp.map.turbo |= bit; else inp.map.turbo &= ~bit;
        }
        return TRUE;
      }

      if (id >= IDC_INPUT_LABEL_BASE && id < IDC_INPUT_LABEL_BASE + input_list_count())
      {
        if (HIWORD(wp) == BN_CLICKED && inp.listen == LISTEN_OFF)
        {
          input_read_controls(dlg);
          inp.sel = id - IDC_INPUT_LABEL_BASE;
          inp.auto_advance = 0;
          input_set_listening(dlg, 1);
        }
        return TRUE;
      }

      switch (id)
      {
        case IDC_INPUT_TAB:
          if (HIWORD(wp) == GP_TABS_CHANGED)
          {
            int to = (int)GetWindowLongPtr((HWND)lp, GWLP_USERDATA);
            if (to == 2)
            {
              if (!inp.ports)
              {
                if (inp.listen != LISTEN_OFF) input_set_listening(dlg, 0);
                input_read_controls(dlg);
              }
              input_ports_view(dlg, 1);
            }
            else if (inp.ports) input_ports_view(dlg, 0);
            if (to != 2 && to != inp.player && (to == 0 || to == 1))
            {
              if (inp.listen != LISTEN_OFF) input_set_listening(dlg, 0);
              input_read_controls(dlg);
              inp.other[inp.player].map = inp.map;
              inp.other[inp.player].padtype = inp.padtype;
              inp.other[inp.player].deadzone = inp.deadzone;
              inp.player = to;
              inp.map = inp.other[to].map;
              inp.padtype = inp.other[to].padtype;
              inp.deadzone = inp.other[to].deadzone;
              input_load_controls(dlg);
              input_fill_list(dlg);
              input_update_identify_hint(dlg);
            }
          }
          return TRUE;

        case IDC_INPUT_PORTA:
        case IDC_INPUT_PORTB:
          if (HIWORD(wp) == CBN_SELCHANGE)
          {
            int p = (LOWORD(wp) == IDC_INPUT_PORTA) ? 0 : 1;
            int sel = (int)SendDlgItemMessage(dlg, LOWORD(wp), CB_GETCURSEL, 0, 0);
            if (sel >= 0) SendMessage(g_hwnd, WM_COMMAND, MAKEWPARAM((p ? IDM_INPUT_PORTB_BASE : IDM_INPUT_PORTA_BASE) + sel, 0), 0);
            SendDlgItemMessage(dlg, LOWORD(wp), CB_SETCURSEL, (WPARAM)gui_port_index(p), 0);     /* what was really accepted */
          }
          return TRUE;

        case IDC_INPUT_DEVICE:
          /* Otherwise the identify hint only catches up on its next 100ms
             tick -- rare to notice, but free to fix. */
          if (HIWORD(wp) == CBN_SELCHANGE)
          {
            int before = inp.shown_dev;

            inp.dev_shadow = 0;             /* a device was picked: it is now the one */
            input_update_identify_hint(dlg);
            input_read_controls(dlg);       /* the buttons show what the new device uses */
            if (inp.shown_dev >= 0 && before != inp.shown_dev)
            {
              /* Buttons that are still the old device's own defaults follow the new device (XInput's masks would put a
                 joystick's D-pad on its face buttons); ones that were set by hand stay. */
              int was[PAD_KEYS];

              gui_input_default_buttons(before, was);
              if (!memcmp(inp.map.button, was, sizeof(was))) gui_input_default_buttons(inp.shown_dev, inp.map.button);
            }
            input_fill_list(dlg);
          }
          return TRUE;

        case IDC_INPUT_TEST:
          if (HIWORD(wp) == BN_CLICKED)
          {
            inp.test = (SendDlgItemMessage(dlg, IDC_INPUT_TEST, BM_GETCHECK, 0, 0) == BST_CHECKED);
            if (inp.listen == LISTEN_OFF) SetDlgItemTextA(dlg, IDC_INPUT_HINT, INPUT_HINT_IDLE);
          }
          return TRUE;

        case IDC_INPUT_REFRESH:
          if (HIWORD(wp) == BN_CLICKED && inp.listen == LISTEN_OFF)
          {
            input_read_controls(dlg);            /* keep what is picked now */
            gui_input_refresh_joysticks();       /* DirectInput joysticks again; XInput slots are asked when the list is built */
            input_load_controls(dlg);
            input_fill_list(dlg);
          }
          return TRUE;

        case IDC_INPUT_PADHK:
          if (HIWORD(wp) == BN_CLICKED && inp.listen == LISTEN_OFF) dlg_pad_hotkeys(dlg, inp.player == 0 ? inp.shown_dev : inp.other[0].map.device);
          return TRUE;

        case IDC_INPUT_DEFAULTS:
          if (HIWORD(wp) == BN_CLICKED)
          {
            /* A drop-down: which player (or the combos) goes back to the defaults. */
            HMENU m = CreatePopupMenu();
            RECT br;
            int cmd;

            AppendMenuA(m, MF_STRING, 1, "Player 1");
            AppendMenuA(m, MF_STRING, 2, "Player 2");
            AppendMenuA(m, MF_STRING, 3, "Extras Tab");
            GetWindowRect(GetDlgItem(dlg, IDC_INPUT_DEFAULTS), &br);
            cmd = (int)gui_popup(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, br.left, br.bottom);      /* below the button */
            DestroyMenu(m);
            if (cmd >= 1 && cmd <= 3 &&
                MessageBoxA(dlg, cmd == 3 ? "Put the Extras tab back to its defaults: remove all combos and set both controller ports to a Control Pad?"
                                          : (cmd == 1 ? "Put the keys, buttons, pad type, deadzone, turbo ticks and turbo rate of Player 1 back to their defaults?"
                                                      : "Put the keys, buttons, pad type, deadzone, turbo ticks and turbo rate of Player 2 back to their defaults?"),
                            "Restore Defaults", MB_YESNO | MB_ICONQUESTION) == IDYES)
            {
              prof_active[0] = '\0';
              if (cmd == 3)
              {
                input_restore_combos(dlg);
                SendMessage(g_hwnd, WM_COMMAND, MAKEWPARAM(IDM_INPUT_PORTA_BASE + 1, 0), 0);       /* a Control Pad in each port */
                SendMessage(g_hwnd, WM_COMMAND, MAKEWPARAM(IDM_INPUT_PORTB_BASE + 1, 0), 0);
                input_ports_fill(dlg);
              }
              else input_restore_defaults(dlg, cmd - 1);
              SetDlgItemTextA(dlg, IDC_INPUT_HINT, cmd == 3 ? "Combos removed and the ports set to a Control Pad."
                                                             : "Controls set to their defaults. Press OK to keep them.");
              prof_update_buttons(dlg, NULL);
            }
          }
          return TRUE;

        case IDC_INPUT_AUTOASSIGN:
          input_read_controls(dlg);
          inp.sel = input_auto_order[0];
          inp.auto_advance = 1;
          input_set_listening(dlg, 1);
          return TRUE;

        case IDOK:
        {
          int p;
          input_read_controls(dlg);
          inp.other[inp.player].map = inp.map;
          inp.other[inp.player].padtype = inp.padtype;
          inp.other[inp.player].deadzone = inp.deadzone;
          gui.turbo_rate = (int)SendDlgItemMessage(dlg, IDC_INPUT_TURBORATE, CB_GETCURSEL, 0, 0);
          cb_serialize(gui.combo_text, sizeof(gui.combo_text));
          for (p = 0; p < 2; p++)
          {
            gui.pad[p] = inp.other[p].map;
            config.input[p].padtype = inp.other[p].padtype;
            gui.deadzone[p] = inp.other[p].deadzone;
          }
          EndDialog(dlg, IDOK);
          return TRUE;
        }

        case IDCANCEL:
          if (inp.listen != LISTEN_OFF) { input_set_listening(dlg, 0); return TRUE; }
          if (cbl.listen) { cb_listen(dlg, 0); return TRUE; }
          input_ports_revert();
          EndDialog(dlg, IDCANCEL);
          return TRUE;
      }
      return FALSE;
    }

    case WM_CLOSE:
      input_ports_revert();
      EndDialog(dlg, IDCANCEL);
      return TRUE;
  }

  return FALSE;
}

static void input_open(HWND parent, int player, int page);

void dlg_input(HWND parent, int player)
{
  input_open(parent, player, 0);
}

/* Controls, opened on the Combos tab. */
void dlg_input_combos(HWND parent)
{
  input_open(parent, 0, 3);
}


static void input_open(HWND parent, int player, int page)
{
  if (player < 0 || player > 1) return;
  input_start_page = page;
  ZeroMemory(&cbl, sizeof(cbl));
  cb_sel = -1;
  cb_parse(gui.combo_text);

  /* Picks up any DirectInput joystick plugged in since the app started (or
     since this dialog was last opened) before the device list below is
     built from it -- and, as part of that same refresh, re-resolves both
     players' device mappings by name against the new list (see
     gui_input_refresh_joysticks()), so inp.map below already has the right
     one without this needing to redo that itself. */
  gui_input_refresh_joysticks();

  ZeroMemory(&inp, sizeof(inp));
  inp.player  = player;
  inp.port_open[0] = gui_port_index(0);
  inp.port_open[1] = gui_port_index(1);
  {
    int p;
    for (p = 0; p < 2; p++)
    {
      inp.other[p].map = gui.pad[p];
      inp.other[p].padtype = config.input[p].padtype;
      inp.other[p].deadzone = gui.deadzone[p];
    }
  }
  inp.map     = gui.pad[player];
  inp.padtype = config.input[player].padtype;
  inp.deadzone = gui.deadzone[player];

  if (gui_dialog_box(gui.large_ui ? IDD_INPUT_LARGE : IDD_INPUT, parent, input_proc) == IDOK)
  {
    config_save();
    gui_status("Controls saved");
  }
}

/****************************************************************************
 * Audio levels and latency
 ****************************************************************************/

static struct
{
  int volume;
  int fm;
  int psg;
  int cdda;
  int pwm;                    /* the 32X PWM level: the CD Audio slider is this one while a 32X game runs */
  int latency;
} aud;

/* The values the window opened with, put back on Cancel (the sliders act at once, so the sound can be judged while sliding). */
static struct { int volume, fm, psg, cdda, pwm, latency; } aud_orig;

static void audio_apply_live(void)
{
  gui.volume = aud.volume;
  waveout_set_volume(aud.volume);
  config.fm_preamp = (int16)aud.fm;
  config.psg_preamp = (int16)aud.psg;
  config.cdda_volume = (int16)aud.cdda;
  gui.pwm_level = aud.pwm;
  gui.latency = aud.latency;
  if (emu_running)      /* the PSG only picks its level up when asked */
    psg_config(0, (unsigned)config.psg_preamp, ((system_hw & SYSTEM_PBC) == SYSTEM_MD) ? 0xff : io_reg[6]);
}

static void audio_revert_live(void)
{
  aud.volume = aud_orig.volume; aud.fm = aud_orig.fm; aud.psg = aud_orig.psg;
  aud.cdda = aud_orig.cdda; aud.pwm = aud_orig.pwm; aud.latency = aud_orig.latency;
  audio_apply_live();
}

static void audio_refresh_labels(HWND dlg)
{
  char buf[32];

  wsprintfA(buf, "%d%%", aud.volume);
  SetDlgItemTextA(dlg, IDC_AUDIO_VOLUME_TEXT, buf);
  wsprintfA(buf, "%d%%", aud.fm);
  SetDlgItemTextA(dlg, IDC_AUDIO_FM_TEXT, buf);
  wsprintfA(buf, "%d%%", aud.psg);
  SetDlgItemTextA(dlg, IDC_AUDIO_PSG_TEXT, buf);
  wsprintfA(buf, "%d%%", aud.cdda);
  SetDlgItemTextA(dlg, IDC_AUDIO_CDDA_TEXT, buf);
  wsprintfA(buf, "%d%%", aud.pwm);
  SetDlgItemTextA(dlg, IDC_AUDIO_PWM_TEXT, buf);
  wsprintfA(buf, "%d", aud.latency);
  SetDlgItemTextA(dlg, IDC_AUDIO_LATENCY_TEXT, buf);
}

static void audio_load_controls(HWND dlg)
{
  audio_set_slider(dlg, IDC_AUDIO_VOLUME,  IDC_AUDIO_VOLUME_TEXT,  0, 100, aud.volume,  "%");
  audio_set_slider(dlg, IDC_AUDIO_FM,      IDC_AUDIO_FM_TEXT,      0, 200, aud.fm,      "%");
  audio_set_slider(dlg, IDC_AUDIO_PSG,     IDC_AUDIO_PSG_TEXT,     0, 200, aud.psg,     "%");
  audio_set_slider(dlg, IDC_AUDIO_CDDA,    IDC_AUDIO_CDDA_TEXT,    0, 200, aud.cdda, "%");
  audio_set_slider(dlg, IDC_AUDIO_PWM,     IDC_AUDIO_PWM_TEXT,     0, 200, aud.pwm,  "%");
  audio_set_slider(dlg, IDC_AUDIO_LATENCY, IDC_AUDIO_LATENCY_TEXT, 2, 8,   aud.latency, "");
}

/* Audio filter, equalizer and Sega CD PCM volume (the right-hand side of the window). */

static struct
{
  int filter;                 /* 0 off, 1 low-pass, 2 3-band EQ */
  int lp;                     /* low-pass strength, % */
  int lg, mg, hg;             /* EQ gains, % */
  int lowf, highf;            /* EQ band edges, Hz */
  int pcm;
} adv;

/* The settings the window opened with; filter, strength, EQ bands and PCM volume act while sliding, Cancel puts these back. */
static struct { int filter, lp, lg, mg, hg, lowf, highf, pcm; } adv_orig;

static void adv_apply_live(int lowf, int highf)
{
  config.filter = (uint8)adv.filter;
  config.lp_range = (uint32)((adv.lp * 65536) / 100);
  config.lg = (int16)adv.lg;
  config.mg = (int16)adv.mg;
  config.hg = (int16)adv.hg;
  config.pcm_volume = (int16)adv.pcm;
  if (lowf >= 20 && lowf <= 2000 && highf >= 1000 && highf <= 20000 && lowf < highf)
  {
    config.low_freq = (int16)lowf;
    config.high_freq = (int16)highf;
  }
  if (emu_running) audio_set_equalizer();
  if (pico_active()) pico_settings_changed();       /* a 32X game: the library has its own low-pass filter */
}

static void adv_revert_live(void)
{
  adv.filter = adv_orig.filter; adv.lp = adv_orig.lp; adv.lg = adv_orig.lg; adv.mg = adv_orig.mg;
  adv.hg = adv_orig.hg; adv.pcm = adv_orig.pcm;
  adv_apply_live(adv_orig.lowf, adv_orig.highf);
}

static void adv_enable_by_filter(HWND dlg)
{
  int lp = (adv.filter == 1), eq = (adv.filter == 2);

  EnableWindow(GetDlgItem(dlg, IDC_ADV_LP), lp);
  EnableWindow(GetDlgItem(dlg, IDC_ADV_LP_TEXT), lp);
  EnableWindow(GetDlgItem(dlg, IDC_ADV_LABEL_LP), lp);
  EnableWindow(GetDlgItem(dlg, IDC_ADV_LG), eq);
  EnableWindow(GetDlgItem(dlg, IDC_ADV_MG), eq);
  EnableWindow(GetDlgItem(dlg, IDC_ADV_HG), eq);
  EnableWindow(GetDlgItem(dlg, IDC_ADV_LG_TEXT), eq);
  EnableWindow(GetDlgItem(dlg, IDC_ADV_MG_TEXT), eq);
  EnableWindow(GetDlgItem(dlg, IDC_ADV_HG_TEXT), eq);
  EnableWindow(GetDlgItem(dlg, IDC_ADV_LOWF), eq);
  EnableWindow(GetDlgItem(dlg, IDC_ADV_HIGHF), eq);
  EnableWindow(GetDlgItem(dlg, IDC_ADV_LABEL_LG), eq);
  EnableWindow(GetDlgItem(dlg, IDC_ADV_LABEL_MG), eq);
  EnableWindow(GetDlgItem(dlg, IDC_ADV_LABEL_HG), eq);
  EnableWindow(GetDlgItem(dlg, IDC_ADV_LABEL_LOWF), eq);
  EnableWindow(GetDlgItem(dlg, IDC_ADV_LABEL_HIGHF), eq);
}

static void adv_load_controls(HWND dlg)
{
  HWND c;

  c = GetDlgItem(dlg, IDC_ADV_FILTER);
  SendMessage(c, CB_RESETCONTENT, 0, 0);
  SendMessageA(c, CB_ADDSTRING, 0, (LPARAM)"Off");
  SendMessageA(c, CB_ADDSTRING, 0, (LPARAM)"Low-Pass");
  SendMessageA(c, CB_ADDSTRING, 0, (LPARAM)"3-Band EQ");
  SendMessage(c, CB_SETCURSEL, (WPARAM)adv.filter, 0);

  audio_set_slider(dlg, IDC_ADV_LP,  IDC_ADV_LP_TEXT,  0, 99,  adv.lp, "%");
  audio_set_slider(dlg, IDC_ADV_LG,  IDC_ADV_LG_TEXT,  0, 200, adv.lg, "%");
  audio_set_slider(dlg, IDC_ADV_MG,  IDC_ADV_MG_TEXT,  0, 200, adv.mg, "%");
  audio_set_slider(dlg, IDC_ADV_HG,  IDC_ADV_HG_TEXT,  0, 200, adv.hg, "%");
  audio_set_slider(dlg, IDC_ADV_PCM, IDC_ADV_PCM_TEXT, 0, 200, adv.pcm, "%");

  SetDlgItemInt(dlg, IDC_ADV_LOWF, (UINT)adv.lowf, FALSE);
  SetDlgItemInt(dlg, IDC_ADV_HIGHF, (UINT)adv.highf, FALSE);

  adv_enable_by_filter(dlg);
}


static void adv_read_config(void)
{
  adv.filter = config.filter > 2 ? 0 : config.filter;
  adv.lp     = (int)((config.lp_range * 100u + 32768u) / 65536u);
  adv.lg     = config.lg;
  adv.mg     = config.mg;
  adv.hg     = config.hg;
  adv.lowf   = config.low_freq;
  adv.highf  = config.high_freq;
  adv.pcm    = config.pcm_volume;
  adv_orig.filter = adv.filter; adv_orig.lp = adv.lp; adv_orig.lg = adv.lg; adv_orig.mg = adv.mg;
  adv_orig.hg = adv.hg; adv_orig.lowf = adv.lowf; adv_orig.highf = adv.highf; adv_orig.pcm = adv.pcm;
}

static void adv_current_freqs(HWND dlg, int *lowf, int *highf)
{
  *lowf = (int)GetDlgItemInt(dlg, IDC_ADV_LOWF, NULL, FALSE);
  *highf = (int)GetDlgItemInt(dlg, IDC_ADV_HIGHF, NULL, FALSE);
}

static int meter_shown_l, meter_shown_r;     /* what the Live Level meter shows now (it falls back slowly) */

static INT_PTR CALLBACK audio_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
    case WM_INITDIALOG:
    {
      static const TipDef tips[] =
      {
        { IDC_AUDIO_VOLUME,  "Master Volume",   "Overall loudness of the sound." },
        { IDC_AUDIO_FM,      "FM Preamp",       "Level of the FM sound channels (the Genesis's main music chip)." },
        { IDC_AUDIO_PSG,     "PSG Preamp",      "Level of the PSG sound channels (the square-wave chip)." },
        { IDC_AUDIO_LATENCY, "Buffered Frames", "How many frames of sound are buffered. Fewer means less delay, but more chance of crackle on a slow PC." },
        { IDC_ADV_FILTER, "Audio Filter:",       "Off, a Low-Pass filter, or a 3-Band equalizer." },
        { IDC_ADV_LP,     "Low-Pass Strength",   "How strongly the high frequencies are rolled off (Low-Pass filter)." },
        { IDC_ADV_LG,     "EQ Low Gain",         "Level of the low band (3-Band equalizer)." },
        { IDC_ADV_MG,     "EQ Mid Gain",         "Level of the middle band (3-Band equalizer)." },
        { IDC_ADV_HG,     "EQ High Gain",        "Level of the high band (3-Band equalizer)." },
        { IDC_ADV_LOWF,   "EQ Low Frequency (Hz):",  "Where the low band ends and the middle band begins, in Hz." },
        { IDC_ADV_HIGHF,  "EQ High Frequency (Hz):", "Where the middle band ends and the high band begins, in Hz." },
        { IDC_ADV_PCM,    "Sega CD PCM Volume",  "Volume of the Sega CD's PCM sound chip." },
        { IDC_AUDIO_DEFAULTS, NULL,             "Put these levels and filter settings back to their defaults." },
        { IDOK,              NULL,              "Save these settings." },
        { IDCANCEL,          NULL,              "Close without saving." },
      };
      theme_apply_to_window(dlg);
      audio_load_controls(dlg);
      adv_load_controls(dlg);
      dlg_add_tips(dlg, tips, (int)(sizeof(tips) / sizeof(tips[0])));
      {
        static const GuiTip cd_tip[] =
        {
          { IDC_AUDIO_CDDA, "CD Audio", "Volume of the Sega CD music tracks." },
          { IDC_AUDIO_PWM, "PWM Preamp", "Level of the 32X PWM sound (most of a 32X game's music and effects). Mute PWM Sound is in Audio > Mute." },
        };
        dlg_add_tips(dlg, cd_tip, 2);
      }
      if (emu_running && !pico_active())                /* only a 32X game has PWM sound: the level does nothing for the others */
      {
        EnableWindow(GetDlgItem(dlg, IDC_AUDIO_PWM), FALSE);
        EnableWindow(GetDlgItem(dlg, IDC_AUDIO_PWM_TEXT), FALSE);
        EnableWindow(GetDlgItem(dlg, IDC_AUDIO_PWM_LABEL), FALSE);
      }
      if (emu_running && (pico_active() || system_hw != SYSTEM_MCD))    /* only a Sega CD game has the CD music and the PCM chip: those two levels do nothing for the others */
      {
        EnableWindow(GetDlgItem(dlg, IDC_AUDIO_CDDA), FALSE);
        EnableWindow(GetDlgItem(dlg, IDC_AUDIO_CDDA_TEXT), FALSE);
        EnableWindow(GetDlgItem(dlg, IDC_AUDIO_CDDA_LABEL), FALSE);
        EnableWindow(GetDlgItem(dlg, IDC_ADV_PCM), FALSE);
        EnableWindow(GetDlgItem(dlg, IDC_ADV_PCM_TEXT), FALSE);
        EnableWindow(GetDlgItem(dlg, IDC_ADV_PCM_LABEL), FALSE);
      }
      SendDlgItemMessage(dlg, IDC_AUDIO_METER_L, PBM_SETRANGE32, 0, 100);
      SendDlgItemMessage(dlg, IDC_AUDIO_METER_R, PBM_SETRANGE32, 0, 100);
      meter_shown_l = meter_shown_r = 0;
      { int l, r; waveout_meter(&l, &r); }          /* what was heard before the window opened is not shown */
      SetTimer(dlg, 61, 50, NULL);
      return TRUE;
    }

    case WM_TIMER:
      if (wp == 61)
      {
        int l, r;
        int shown_l = meter_shown_l, shown_r = meter_shown_r;
        waveout_meter(&l, &r);
        if (!emu_running) l = r = 0;                      /* no game: nothing is playing */
        l = (int)(sqrt((double)l / 32767.0) * 100.0);     /* a ear-friendly scale: quiet sound still shows */
        r = (int)(sqrt((double)r / 32767.0) * 100.0);
        shown_l = l > shown_l ? l : (shown_l > 4 ? shown_l - 4 : 0);   /* falls back slowly */
        shown_r = r > shown_r ? r : (shown_r > 4 ? shown_r - 4 : 0);
        SendDlgItemMessage(dlg, IDC_AUDIO_METER_L, PBM_SETPOS, (WPARAM)shown_l, 0);
        SendDlgItemMessage(dlg, IDC_AUDIO_METER_R, PBM_SETPOS, (WPARAM)shown_r, 0);
        meter_shown_l = shown_l; meter_shown_r = shown_r;
        return TRUE;
      }
      return FALSE;

    case WM_DESTROY:
      KillTimer(dlg, 61);
      return FALSE;

    case WM_CTLCOLORDLG:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    {
      HBRUSH br = theme_ctlcolor((HDC)wp);
      if (br) return (LRESULT)br;
      break;
    }

    case WM_HSCROLL:
    {
      HWND bar = (HWND)lp;
      int pos = (int)SendMessage(bar, TBM_GETPOS, 0, 0);
      int id = GetDlgCtrlID(bar);

      if (id == IDC_ADV_LP || id == IDC_ADV_LG || id == IDC_ADV_MG || id == IDC_ADV_HG || id == IDC_ADV_PCM)
      {
        char tbuf[16];
        int text = 0, lowf, highf;
        switch (id)
        {
          case IDC_ADV_LP:  adv.lp  = pos; text = IDC_ADV_LP_TEXT;  break;
          case IDC_ADV_LG:  adv.lg  = pos; text = IDC_ADV_LG_TEXT;  break;
          case IDC_ADV_MG:  adv.mg  = pos; text = IDC_ADV_MG_TEXT;  break;
          case IDC_ADV_HG:  adv.hg  = pos; text = IDC_ADV_HG_TEXT;  break;
          default:          adv.pcm = pos; text = IDC_ADV_PCM_TEXT; break;
        }
        wsprintfA(tbuf, "%d%%", pos);
        SetDlgItemTextA(dlg, text, tbuf);
        adv_current_freqs(dlg, &lowf, &highf);
        adv_apply_live(lowf, highf);
        return TRUE;
      }

      switch (id)
      {
        case IDC_AUDIO_VOLUME:  aud.volume  = pos; break;
        case IDC_AUDIO_FM:      aud.fm      = pos; break;
        case IDC_AUDIO_PSG:     aud.psg     = pos; break;
        case IDC_AUDIO_CDDA:    aud.cdda = pos; break;
        case IDC_AUDIO_PWM:     aud.pwm = pos; break;
        case IDC_AUDIO_LATENCY: aud.latency = pos; break;
        default: break;
      }

      audio_refresh_labels(dlg);
      audio_apply_live();            /* heard while sliding; Cancel puts the old values back */
      return TRUE;
    }

    case WM_COMMAND:
      switch (LOWORD(wp))
      {
        case IDC_ADV_FILTER:
          if (HIWORD(wp) == CBN_SELCHANGE)
          {
            int lowf, highf;
            adv.filter = (int)SendDlgItemMessage(dlg, IDC_ADV_FILTER, CB_GETCURSEL, 0, 0);
            if (adv.filter < 0) adv.filter = 0;
            adv_enable_by_filter(dlg);
            adv_current_freqs(dlg, &lowf, &highf);
            adv_apply_live(lowf, highf);
          }
          return TRUE;

        case IDC_ADV_LOWF:
        case IDC_ADV_HIGHF:
          if (HIWORD(wp) == EN_CHANGE)
          {
            int lowf, highf;
            adv_current_freqs(dlg, &lowf, &highf);
            adv_apply_live(lowf, highf);
          }
          return TRUE;

        case IDC_AUDIO_DEFAULTS:
          adv.filter = 0; adv.lp = 60;
          adv.lg = adv.mg = adv.hg = 100;
          adv.lowf = 200; adv.highf = 8000;
          adv.pcm = 100;
          adv_load_controls(dlg);
          adv_apply_live(adv.lowf, adv.highf);
          aud.volume  = 100;
          aud.fm      = 100;
          aud.psg     = 150;
          aud.cdda    = 100;
          aud.pwm     = 100;
          aud.latency = 4;
          audio_load_controls(dlg);
          audio_apply_live();
          return TRUE;

        case IDOK:
        {
          int lowf, highf;
          adv_current_freqs(dlg, &lowf, &highf);
          if (lowf < 20 || lowf > 2000 || highf < 1000 || highf > 20000 || lowf >= highf)
          {
            MessageBoxA(dlg, "The EQ bands need a low frequency of 20-2000 Hz, a high frequency of\n"
                             "1000-20000 Hz, and the low one below the high one.",
                        APP_NAME, MB_OK | MB_ICONINFORMATION);
            return TRUE;
          }
          adv.lowf = lowf;
          adv.highf = highf;
          EndDialog(dlg, IDOK);
          return TRUE;
        }

        case IDCANCEL:
          audio_revert_live();              /* undo the live preview */
          adv_revert_live();
          EndDialog(dlg, IDCANCEL);
          return TRUE;
      }
      return FALSE;

    case WM_CLOSE:
      audio_revert_live();
      adv_revert_live();
      EndDialog(dlg, IDCANCEL);
      return TRUE;
  }

  return FALSE;
}

/****************************************************************************
 * Picture Adjustments: Contrast, Saturation, Gamma and Hue
 *
 * Like the Audio Levels and Latency window: the sliders act while they are dragged, Cancel puts the old values back and OK
 * keeps them (and writes them to the settings file).
 ****************************************************************************/

static const struct { int slider, text, id; } padj_rows[4] =
{
  { IDC_PADJ_CONTRAST, IDC_PADJ_CONTRAST_TEXT, IDM_SET_CONTRAST_BASE },
  { IDC_PADJ_SAT,      IDC_PADJ_SAT_TEXT,      IDM_SET_SATUR_BASE },
  { IDC_PADJ_GAMMA,    IDC_PADJ_GAMMA_TEXT,    IDM_SET_GAMMA_BASE },
  { IDC_PADJ_HUE,      IDC_PADJ_HUE_TEXT,      IDM_SET_HUE_BASE },
};
static int padj_orig[4];                          /* the values when the window opened */

/* A slider's value as its readout shows it: 100%, 1.10 for the gamma, 15 degrees for the hue. */
static void padj_show(HWND dlg, int k)
{
  int v = gui_slider_get(padj_rows[k].id);
  char t[16];

  if (padj_rows[k].id == IDM_SET_GAMMA_BASE) { wsprintfA(t, "%d.%02d", v / 100, v % 100); SetDlgItemTextA(dlg, padj_rows[k].text, t); }
  else if (padj_rows[k].id == IDM_SET_HUE_BASE)
  {
    WCHAR wt[16];
    wsprintfW(wt, L"%d\x00B0", v);
    SetDlgItemTextW(dlg, padj_rows[k].text, wt);
  }
  else { wsprintfA(t, "%d%%", v); SetDlgItemTextA(dlg, padj_rows[k].text, t); }
}

/* The sliders and readouts from the values the program holds now. */
static void padj_load(HWND dlg)
{
  int k;

  for (k = 0; k < 4; k++)
  {
    HWND bar = GetDlgItem(dlg, padj_rows[k].slider);
    int lo = 0, hi = 100, def = 0, span, step;

    gui_slider_range(padj_rows[k].id, &lo, &hi, &def);
    SendMessage(bar, TBM_SETRANGE, TRUE, MAKELPARAM(lo / 10, hi / 10));        /* one notch is 10: the knob jumps from value to value */
    SendMessage(bar, TBM_SETLINESIZE, 0, 1);
    SendMessage(bar, TBM_SETPAGESIZE, 0, 1);
    span = hi / 10 - lo / 10; step = span / 8;                                  /* about eight ticks, the last one at the end of the range */
    if (step < 1) step = 1;
    while (step > 1 && span % step) step--;
    SendMessage(bar, TBM_SETTICFREQ, (WPARAM)step, 0);
    SendMessage(bar, TBM_SETPOS, TRUE, (LPARAM)((gui_slider_get(padj_rows[k].id) >= 0 ? gui_slider_get(padj_rows[k].id) + 5 : gui_slider_get(padj_rows[k].id) - 5) / 10));
    padj_show(dlg, k);
  }
}

static INT_PTR CALLBACK padj_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
    case WM_INITDIALOG:
    {
      static const GuiTip tips[] =
      {
        { IDC_PADJ_CONTRAST, "Contrast",   "How strongly light and dark parts of the picture differ." },
        { IDC_PADJ_SAT,      "Saturation", "How vivid the colors are; 0% is black and white." },
        { IDC_PADJ_GAMMA,    "Gamma",      "Lightens (above 1) or darkens (below 1) the mid-tones." },
        { IDC_PADJ_HUE,      "Hue",        "Turns all the colors around the color wheel." },
        { IDC_PADJ_DEFAULTS, NULL,         "Put Contrast, Saturation, Gamma and Hue back to normal." },
        { IDOK,              NULL,         "Keep these settings." },
        { IDCANCEL,          NULL,         "Close and put the old values back." },
      };
      int k;

      for (k = 0; k < 4; k++) padj_orig[k] = gui_slider_get(padj_rows[k].id);
      theme_apply_to_window(dlg);
      padj_load(dlg);
      dlg_add_tips(dlg, tips, (int)(sizeof(tips) / sizeof(tips[0])));
      return TRUE;
    }

    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    {
      HBRUSH br = theme_ctlcolor((HDC)wp);
      if (br) return (LRESULT)br;
      break;
    }

    case WM_HSCROLL:
    {
      int k, id = GetDlgCtrlID((HWND)lp);

      for (k = 0; k < 4; k++)
        if (padj_rows[k].slider == id)
        {
          gui_slider_set(padj_rows[k].id, (int)SendMessage((HWND)lp, TBM_GETPOS, 0, 0) * 10);       /* seen while sliding */
          padj_show(dlg, k);
        }
      return TRUE;
    }

    case WM_COMMAND:
      switch (LOWORD(wp))
      {
        case IDC_PADJ_DEFAULTS:
        {
          int k;

          for (k = 0; k < 4; k++)
          {
            int lo, hi, def;
            if (gui_slider_range(padj_rows[k].id, &lo, &hi, &def)) gui_slider_set(padj_rows[k].id, def);
          }
          padj_load(dlg);
          return TRUE;
        }

        case IDOK:
          gui_slider_commit();
          EndDialog(dlg, IDOK);
          return TRUE;

        case IDCANCEL:
        {
          int k;

          for (k = 0; k < 4; k++) gui_slider_set(padj_rows[k].id, padj_orig[k]);
          EndDialog(dlg, IDCANCEL);
          return TRUE;
        }
      }
      break;

    case WM_CLOSE:
      SendMessage(dlg, WM_COMMAND, IDCANCEL, 0);
      return TRUE;
  }
  return FALSE;
}

void dlg_picture_adjust(HWND parent)
{
  gui_dialog_box(gui.large_ui ? IDD_PICTUREADJ_LARGE : IDD_PICTUREADJ, parent, padj_proc);
}

void dlg_audio(HWND parent)
{
  aud.volume  = gui.volume;
  aud.fm      = config.fm_preamp;
  aud.psg     = config.psg_preamp;
  aud.cdda    = config.cdda_volume;
  aud.pwm     = gui.pwm_level;
  aud.latency = gui.latency;
  aud_orig.volume = aud.volume; aud_orig.fm = aud.fm; aud_orig.psg = aud.psg;
  aud_orig.cdda = aud.cdda; aud_orig.pwm = aud.pwm; aud_orig.latency = aud.latency;
  adv_read_config();

  if (gui_dialog_box(gui.large_ui ? IDD_AUDIO_LARGE : IDD_AUDIO, parent, audio_proc) == IDOK)
  {
    config.filter    = (uint8)adv.filter;
    config.lp_range  = (uint32)((adv.lp * 65536) / 100);
    config.lg        = (int16)adv.lg;
    config.mg        = (int16)adv.mg;
    config.hg        = (int16)adv.hg;
    config.low_freq  = (int16)adv.lowf;
    config.high_freq = (int16)adv.highf;
    config.pcm_volume = (int16)adv.pcm;
    if (emu_running) audio_set_equalizer();
    if (pico_active()) pico_settings_changed();

    gui.volume         = aud.volume;
    config.fm_preamp   = (int16)aud.fm;
    config.psg_preamp  = (int16)aud.psg;
    config.cdda_volume = (int16)aud.cdda;
    gui.pwm_level      = aud.pwm;
    gui.latency        = aud.latency;

    waveout_set_volume(gui.volume);
    emu_apply_audio_settings();
    config_save();
    gui_update_menu();
    gui_status("Audio settings saved");
  }
}

/****************************************************************************
 * Keyboard shortcuts
 ****************************************************************************/

static INT_PTR CALLBACK shortcuts_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
  (void)lp;

  switch (msg)
  {
    case WM_INITDIALOG:
    {
      HWND list = GetDlgItem(dlg, IDC_SHORTCUTS_TEXT);
      static const GuiTip tips[] =
      {
        { IDC_SHORTCUTS_TEXT, NULL, "Every keyboard shortcut. Change them in Input > Hotkeys." },
        { IDOK, NULL, "Close this window." },
      };
      gui_dialog_tips(dlg, tips, (int)(sizeof(tips) / sizeof(tips[0])));
      LVCOLUMNA col;
      LVITEMA it;
      RECT rc;
      char keys[4096], *line;
      int row = 0;

      SendMessage(list, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT);
      GetClientRect(list, &rc);
      memset(&col, 0, sizeof(col));
      col.mask = LVCF_TEXT | LVCF_WIDTH;
      col.pszText = "Action";
      col.cx = (rc.right - rc.left) * 42 / 100;
      SendMessageA(list, LVM_INSERTCOLUMNA, 0, (LPARAM)&col);
      col.pszText = "Shortcut";
      col.cx = (rc.right - rc.left) * 38 / 100;
      SendMessageA(list, LVM_INSERTCOLUMNA, 1, (LPARAM)&col);

      /* hotkeys_describe gives "Action<TAB>Shortcut" lines. */
      hotkeys_describe(keys, sizeof(keys));
      for (line = strtok(keys, "\r\n"); line; line = strtok(NULL, "\r\n"))
      {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = '\0';
        memset(&it, 0, sizeof(it));
        it.mask = LVIF_TEXT;
        it.iItem = row;
        it.pszText = line;
        SendMessageA(list, LVM_INSERTITEMA, 0, (LPARAM)&it);
        it.iSubItem = 1;
        it.pszText = tab + 1;
        SendMessageA(list, LVM_SETITEMTEXTA, (WPARAM)row, (LPARAM)&it);
        row++;
      }

      theme_apply_to_window(dlg);
      {
        HWND header = ListView_GetHeader(list);
        if (header && theme_is_dark()) { SetWindowTheme(header, L"ItemsView", NULL); InvalidateRect(header, NULL, TRUE); }
      }
      SetDlgItemTextA(dlg, IDC_SHORTCUTS_HINT,
        "Change any of these under Input > Hotkeys. Player 1 keyboard defaults: arrows, Z X C = A B C, A S D = X Y Z, Enter = Start, Right Shift = Mode (Input > Controls). A connected gamepad works without setup.");
      return TRUE;
    }

    case WM_CTLCOLORDLG:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    {
      HBRUSH br = theme_ctlcolor((HDC)wp);
      if (br) return (LRESULT)br;
      break;
    }

    case WM_COMMAND:
      if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)
      {
        EndDialog(dlg, LOWORD(wp));
        return TRUE;
      }
      return FALSE;

    case WM_CLOSE:
      EndDialog(dlg, IDCANCEL);
      return TRUE;
  }

  return FALSE;
}

void dlg_shortcuts(HWND parent)
{
  gui_dialog_box(gui.large_ui ? IDD_SHORTCUTS_LARGE : IDD_SHORTCUTS, parent, shortcuts_proc);
}

static const char user_guide_text[] =
  "Every menu, and the Settings, Controls, Cheats and Netplay windows, in one place. Keyboard shortcuts are in brackets (the original keys; Input > Hotkeys changes them).\r\n"
  "\r\n"
  "FILE\r\n"
  "  Open ROM... (Ctrl+O):\r\n"
  "   Pick a game file to play: Mega Drive / Genesis (and Sega Pico, which uses the same file types), Master System, Game Gear, SG-1000, Mega CD, 32X (run by PicoDrive: needs picodrive_libretro.dll in the Plugins folder next to the program; some settings of the Genesis Plus GX core do not apply to those), or a .zip / .7z / .gz that holds one.\r\n"
  "\r\n"
  "  ROM Directory... (Ctrl+R):\r\n"
  "   Shows the game browser for a folder of games. Double-click a game to play it, right-click for more choices (play with a saved state, cheats, cover image, clear the recently played list: it is grayed when there is nothing to clear); right-clicking empty space shows the same menu with the choices for a game grayed; with more than one ROM folder set, Open ROM Directory opens a list of them. Folders inside it are searched too (Settings > General > Scan ROM Subfolders, on by default).\r\n"
  "\r\n"
  "  Recent Files:\r\n"
  "   Your last games, each with its console on the right. If a game's file is gone, the program says so and asks whether to remove it from this list (and from Recently Played); the same question comes from the Recently Played group.\r\n"
  "\r\n"
  "  Insert Disc...:\r\n"
  "   A Mega CD command to swap the disc by hand, for games that do not ask for it themselves: pick the next disc's .cue or .chd file.\r\n"
  "\r\n"
  "  Apply Patch...:\r\n"
  "   Pick an .ips or .bps patch file (or a .zip or .7z that holds one) and it is applied in memory to the running game (the item is gray while no game runs), which is then started again from the patched copy: for translations and hacks. A .bps patch made for a different game is refused. The game file itself is not changed.\r\n"
  "\r\n"
  "  ROM Information...:\r\n"
  "   The game's name, console, genre, whether it is a favorite, file size and location, the CRC32, the cartridge header (console name, type, titles, product code, region, copyright, supported controllers, save RAM), and the ROM size and checksum. Right-clicking the column titles offers Size All Columns to Fit.\r\n"
  "\r\n"
  "  Open Genesis Plus EX Folder:\r\n"
  "   Opens the program folder: settings, saves, states, screenshots, recordings, cheats, covers and BIOS files.\r\n"
  "\r\n"
  "  Exit:\r\n"
  "   Quits.\r\n"
  "\r\n"
  "EMULATION\r\n"
  "  Pause (F2 / Pause), Stop (F3):\r\n"
  "   Freeze the game, or end it.\r\n"
  "\r\n"
  "  Reset (F4), Hard Reset (Ctrl+F4):\r\n"
  "   Reset is the console's reset button. Hard Reset power-cycles the console (saved game data is kept).\r\n"
  "\r\n"
  "  Load State:\r\n"
  "   Load State from Selected Slot (F8) restores the exact moment saved in the selected slot. Load State from Slot (keys 0-9) loads any of the slots 0-9 directly, whichever is selected (empty slots are grayed). Undo Load State (Ctrl+F8) puts the game back as it was just before the last load; one level, cleared when the game is closed.\r\n"
  "\r\n"
  "  Save State:\r\n"
  "   Save State to Selected Slot (F5) saves the exact moment into the selected slot. Save State to Slot (Shift+0 - Shift+9) saves into any of the slots 0-9 directly. Undo Save State puts the slot back as it was before the last save (or empties it if it was empty); one level, cleared when the game is closed.\r\n"
  "\r\n"
  "  Select State Slot:\r\n"
  "   Choose slot 0-9 for F5 / F8 (F6 / F7 = previous / next). Each entry shows when the slot was last saved.\r\n"
  "\r\n"
  "  Load State from File..., Save State to File...:\r\n"
  "   Load a state from, or save the state to, any file you choose (extension .gpstate), for example to send it to a friend or keep it outside the slots.\r\n"
  "\r\n"
  "  Manage States...:\r\n"
  "   Thumbnails of all ten slots: left-click loads one, right-click saves to it, saves it to a file (Save This Slot As..., not for an empty slot) or deletes it (not for an empty slot); a double-click on an empty slot saves to it.\r\n"
  "\r\n"
  "  Enable Rewind (Settings > Options; Backspace, Hold):\r\n"
  "   Keeps a history of recent play so holding Backspace runs it backwards. Uses 256 MB by default and some CPU while on; switch it off if you never rewind. Not available during netplay.\r\n"
  "\r\n"
  "  Rewind History Size (Settings > Options):\r\n"
  "   How much memory the Rewind history may use: 128, 256 (the default), 512 or 768 MB. More memory lets you go further back (how far depends on the game: busy scenes take more room); changing it starts the history again. The largest sizes may not be available in the 32-bit program.\r\n"
  "\r\n"
  "  Rewind Speed (Settings > Options):\r\n"
  "   How fast Rewind runs backwards: Slow (half the speed), Normal (the default), Fast (twice) or Very Fast (four times). The faster ones go through the history sooner.\r\n"
  "\r\n"
  "  Pause When Inactive:\r\n"
  "   Pauses the game while another window has focus.\r\n"
  "\r\n"
  "  Mute When Inactive:\r\n"
  "   Silences the game while another window has focus; it keeps running (Pause When Inactive stops it). In Settings > General.\r\n"
  "\r\n"
  "  Hide Mouse Pointer:\r\n"
  "   Hides the pointer after it has been still for 2, 3, 5 or 10 seconds, over a running game in a window and in fullscreen; moving the mouse shows it again (Settings > Input).\r\n"
  "\r\n"
  "  Keep Only One Instance:\r\n"
  "   Off by default. When on, starting the program again (or opening a game with it) brings the running copy to the front and opens the game there, instead of starting a second copy that would share and overwrite the same settings and saves. Takes effect the next time the program starts; a launch from a frontend (--exit-on-close) is not affected. In Settings > General.\r\n"
  "\r\n"
  "  Keep the PC Awake While Playing:\r\n"
  "   Stops Windows from sleeping, dimming the screen or starting the screen saver while a game is running, and lets it do so again when the game stops. In Settings > General.\r\n"
  "\r\n"
  "  Confirm Exit While Playing:\r\n"
  "   Asks what to do when you close the program while a game is running (on by default): Stop Emulation (back to the game list, the program stays), Exit or Cancel. In Settings > General.\r\n"
  "\r\n"
  "  Game Speed (Ctrl++, Ctrl+-, Ctrl+0):\r\n"
  "   Steps the speed of the game (gray while no game runs) through 25, 50, 75, 100, 125, 150, 200, 300 and 400% and shows it on screen; Ctrl+0 goes back to 100 %. The sound follows the speed: below 100% it is stretched and plays lower, above 100% short slices play at the normal pitch (a little choppy). The speed returns to 100% when the game is closed, and it is not available during netplay.\r\n"
  "\r\n"
  "  Auto Resume:\r\n"
  "   Saves the game when you close it (or quit) and, the next time you open that game, asks whether to carry on from there. The question has a \"Do not ask again\" box that remembers your answer; the Restore Dialog button next to Auto Resume in Settings brings it back. It uses a state of its own, so your ten save slots are untouched. On by default; when you close a running game the exit question has a \"Resume this game next time\" box.\r\n"
  "\r\n"
  "  Fast Forward (Tab), Rewind (Backspace):\r\n"
  "   Ticked items that keep the game running fast, or backwards, until you untick them (Rewind needs Enable Rewind in Settings > Options). The keys themselves work while held; they can be changed under Input > Hotkeys. Only one of Fast Forward and Rewind is on at a time. For slow motion use Game Speed below 100 %.\r\n"
  "\r\n"
  "VIEW\r\n"
  "  View Mode:\r\n"
  "   List View or Grid View: how the game browser shows your games.\r\n"
  "\r\n"
  "  List Columns:\r\n"
  "   A Sega Pico game (a Mega Drive file whose header says SEGA PICO) is listed as its own console, Sega Pico. Choose which columns the list view shows: Favorite (a star: click it to mark a game as a favorite) and Console (Name is always shown), plus Genre, Region (read from tags such as (USA) in the file name), File Size and File Type (zip, bin, chd and so on), which are hidden at first and shown after Console. Drag a column title to move the column, and drag the edge between titles to change its width; the order and the widths you leave are remembered. Click a column's title to sort by it; Shift+click more titles to sort by them as well, in that order (for example Console, then Genre, then name); Shift+click one again to flip it, while a plain click starts over (View > Game List > Show Recently Played shows what you played last). Right-clicking the column header offers the same choices, and also Size All Columns to Fit, which gives every shown column the width its contents need. The choices are grayed out while the grid view is shown.\r\n"
  "\r\n"
  "  Theme:\r\n"
  "   System (follow Windows), Light or Dark.\r\n"
  "\r\n"
  "  Style (Settings > General):\r\n"
  "   Classic (the look the program has always had) or Modern: flat, rounded buttons, check boxes, radio buttons and drop-down lists (the box and the list that opens), tabs with an accent line under the chosen one, softer toolbar buttons, the chosen game or row in the accent color, rounded window corners and a title bar and border in the color of the theme (Windows 11), and Segoe UI text, also in the menus (changed at once in the windows that are open, both ways). It is also in View > Style, and goes with Theme: any Style can be Light or Dark.\r\n"
  "\r\n"
  "  Interface Size (Settings > General):\r\n"
  "   Normal or Large menus and dialogs. Changing it restarts the program.\r\n"
  "\r\n"
  "  Show Toolbar Labels (View > Toolbars):\r\n"
  "   Writes the name of each toolbar button (Open, Play, Settings ...) under its icon; switch it off for icons only.\r\n"
  "\r\n"
  "  Show Toolbar, Show Search Bar (Ctrl+F), Show Filter Bar:\r\n"
  "   Switch the rows above the game list on or off. The toolbar has buttons for open ROM, refresh, favorites only, search (the same as Ctrl+F), play, pause, stop, reset, switch fullscreen, screenshot, settings, the Video and Audio settings tabs and the player controls; a star button next to Refresh shows only your favorites. The toolbar stays above the game while it runs in a window; in fullscreen it slides in at the top edge of the screen together with the menu bar (the picture makes room for it; View > Toolbars > Toolbar in Fullscreen switches that on, and it is off by default). The search bar narrows the list as you type (Ctrl+F jumps to it; Close or Show Search Bar hides it, and Show Search Bar brings it back). Search words narrow by one field: region:usa, console:mega cd, genre:action, type:chd, fav:yes; anything else you type is looked for in the name (for example sonic region:usa). The filter bar narrows it by console, genre (looked up from the game's title in a table built into the program; a game that is not in it is listed as Unknown) and region (read from tags such as (USA) in the file name). Hiding the filter bar clears its choices, so the list shows every game again. Show Status Bar (View > Toolbars, on by default) shows or hides the bar along the bottom. Toolbar Icon Size (Small, Normal, the default, or Large; also in View > Toolbars > Toolbar Size) and Show Toolbar Labels (the name of each button under its icon, on by default) are in Settings > General. When the window is too narrow for all the buttons, the ones that do not fit go on a second row (a third if needed), like a menu bar, and the bar grows taller; they return to one row when the window is wide enough again.\r\n"
  "\r\n"
  "  Toolbar in Fullscreen:\r\n"
  "   In fullscreen, shows the toolbar together with the menu bar when the mouse touches the top edge of the screen.\r\n"
  "\r\n"
  "  Show Favorites Only:\r\n"
  "   Mark a game as a favorite by clicking its star in the Favorite column (the right-click menu has Add to Favorites too). Show Favorites Only, or the star on the toolbar, hides every other game. Sorting by the Favorite column puts your favorites first.\r\n"
  "\r\n"
  "  Show Recently Played:\r\n"
  "   Shows the last few games you opened as a group at the top of the list, above all the others (they are in the full list too). On by default; choose it again to switch it off.\r\n"
  "\r\n"
  "  Clean Game Names:\r\n"
  "   The game list shows \"Sonic The Hedgehog\" instead of \"Sonic The Hedgehog (USA, Europe) [!].md\". Only the list (and File > Recent Files) changes, not the files. On by default.\r\n"
  "\r\n"
  "  Show Grid Lines:\r\n"
  "   Thin horizontal lines between the rows of the lists; the lines between columns are always shown.\r\n"
  "\r\n"
  "  Group by Console:\r\n"
  "   Splits the list into sections, one per console (Genesis, Master System, Mega CD ...), each with a heading row in the same style as Recently Played. The grid view has the same headings (Recently Played, All Games, Favorites and one per console): each starts a new row of covers. The empty space above the first heading shows the Genesis Plus EX logo.\r\n"
  "\r\n"
  "  Show A-Z Strip:\r\n"
  "   A strip of letters beside the list: click a letter, or drag along the strip, to jump to the games starting with it (the letter under the pointer grows) (# is for names starting with a digit or a symbol); letters with no game are dimmed. \r\n"
  "\r\n"
  "  List Spacing:\r\n"
  "   Compact, Normal or Expanded spacing between the rows of the game list (grayed while a game is running). \r\n"
  "\r\n"
  "  Always on Top:\r\n"
  "   Keeps the window above every other window (also in Settings > General).\r\n"
  "\r\n"
  "  Reset Window Size:\r\n"
  "   Puts the window back at the size chosen under Video > Window Size, in the middle of the screen.\r\n"
  "\r\n"
  "VIDEO\r\n"
  "  Window Size:\r\n"
  "   1x to 6x (3x by default).\r\n"
  "\r\n"
  "  Fullscreen (Alt+Enter), Start ROM in Fullscreen:\r\n"
  "   Esc also toggles fullscreen. In fullscreen, move the mouse to the top edge of the screen to show the menu bar; it hides again when the mouse moves away. The mouse cursor stays visible in fullscreen and hides itself after a few idle seconds. Closing the ROM leaves fullscreen and returns to the selected window size.\r\n"
  "\r\n"
  "  Render Filter:\r\n"
  "   Pixel-art scalers and CRT looks (Scale2x, xBR, HQ2x, xBRZ, Catmull-Rom, Scanlines, CRT...) applied before the picture is shown.\r\n"
  "\r\n"
  "  Shaders:\r\n"
  "   Runs a librashader (.slangp) shader chain on top of the picture, Direct3D 11 only, and 64-bit exe only (the 32-bit exe has no Shaders menu); librashader.dll ships in the Plugins folder next to the exe (see the README).\r\n"
  "\r\n"
  "  Visual Effects:\r\n"
  "   Smooth Scaling (soft edges when enlarged), Integer Scaling (enlarges the picture by whole multiples only, 1x, 2x, 3x ..., so every pixel is the same size; it is centered, with borders, when the window is between two sizes, and follows the Aspect Ratio setting for the shape of the pixels), Brighten (a gentle brightness lift), LCD Ghosting (bright pixels fade slowly, like a handheld screen).\r\n"
  "\r\n"
  "  Picture Adjustments...:\r\n"
  "   A window with Contrast, Saturation, Gamma and Hue, each a slider that acts while you drag it (Restore Defaults puts them back to normal, Cancel puts back what they were when the window opened, OK keeps them), alongside Brighten (Settings > Video; the window is also a button there). They work with every renderer.\r\n"
  "\r\n"
  "  Flicker Blending, Dither Smoothing:\r\n"
  "   Flicker Blending joins the two frames of an object that a game shows every other frame to fake transparency, so it looks solid. Dither Smoothing softens the checkerboard patterns many games use to fake extra colors (Settings > Video).\r\n"
  "\r\n"
  "  Bezel Image...:\r\n"
  "   Draws an image (a PNG whose middle is see-through: a TV, an arcade cabinet) over the picture as a frame; Remove Bezel goes back to plain bars. The picker starts in the bezels folder next to the program (made for you). The bezel is drawn in fullscreen only (Settings > Video, in the Bezel and Border frame).\r\n"
  "\r\n"
  "  Shader Parameters...:\r\n"
  "   Adjusts the loaded shader preset's own parameters while it runs, with a slider that jumps from one value to the next (1, 2, 3 ...) as the preset defines them, without editing its file; the values are remembered for that preset (Video > Shaders, and Settings > Video in the shader box under the preset file; grayed until a shader that has settings is loaded).\r\n"
  "\r\n"
  "  Skip Render Filter with Shaders:\r\n"
  "   While a shader runs, the Render Filter is not applied first, so the shader gets the game's own picture instead of one the filter already enlarged. Shaders that scale by whole multiples or place a bezel work out their sizes from the picture they receive, so one that looks too small or wrong with a Render Filter on may look right with this ticked. Off by default; has no effect without a shader or a Render Filter (Video > Shaders, below Shader Parameters, and Settings > Video, in the shader box beside it; Direct3D 11 only).\r\n"
  "\r\n"
  "  Border Color:\r\n"
  "   The color of the bars around the picture when the window or screen is a different shape: black, dark gray, gray, white, navy, dark green or dark red (Settings > Video).\r\n"
  "\r\n"
  "  Fullscreen Mode:\r\n"
  "   Whether fullscreen is a Borderless Window or Exclusive Fullscreen: the screen is switched to the Fullscreen Resolution for the game and put back afterwards (Video menu, or Settings > Video).\r\n"
  "\r\n"
  "  Fullscreen Resolution:\r\n"
  "   The screen size used when Fullscreen Mode is Exclusive Fullscreen: a drop-down of the sizes your monitor offers (Desktop Resolution changes nothing). Under Window Size in Settings > Video.\r\n"
  "\r\n"
  "  Scanlines:\r\n"
  "   Dark lines between rows: Off, 25, 50, 75 or 100%.\r\n"
  "\r\n"
  "  NTSC Filter:\r\n"
  "   Composite, S-Video or RGB television look. For a 32X game it is the program's own version of the look (the picture keeps its size), not the Genesis Plus GX core's.\r\n"
  "\r\n"
  "  Aspect Ratio:\r\n"
  "   Original Console Pixels (the default: the Mega Drive's own pixels, 8:7 wide in the 256-pixel mode and 32:35 in the 320-pixel one, which makes the picture about 1.31 times as wide as it is high; other consoles use square pixels), 1:1 (Square Pixels), 4:3 (Standard, as on a CRT television) or 16:9 (Widescreen: stretches the picture to fill a widescreen window).\r\n"
  "\r\n"
  "  Limit Redraws:\r\n"
  "   Draws the picture at most 30, 40, 50 or 60 times a second while the game itself still runs at full speed. Helps a weak PC or a very fast monitor; Off draws every frame (Settings > Video).\r\n"
  "\r\n"
  "  Borders:\r\n"
  "   Hides or shows the overscan borders (top/bottom, left/right).\r\n"
  "\r\n"
  "  Interlaced Mode:\r\n"
  "   Single Field (the default) or Double Field, for games that use the 448-line interlaced mode. Single Field shows the same half of the picture every frame, so it stays steady. Double Field shows both fields at once; where something moves, the older field's lines are rebuilt from the new ones and both lines of each pair are drawn partly alike, so there are no comb teeth and the picture hardly shakes (a moving part is a little softer than a still one). With Original Console Pixels or Square Pixels the picture is drawn at the shape of the 224-line one, not tall.\r\n"
  "\r\n"
  "  Frameskip:\r\n"
  "   Skips drawing some frames on a slow PC (Off, Auto, or Manual by audio-buffer level). Not used during netplay or recording.\r\n"
  "\r\n"
  "  VSync:\r\n"
  "   Waits for the screen's refresh after each frame so motion is even. Works with any renderer; it is skipped for a frame whenever the sound buffer runs low, so it never causes crackle. Off by default.\r\n"
  "\r\n"
  "  Renderer:\r\n"
  "   GDI draws with the CPU; Direct3D 9 and Direct3D 11 use the graphics card instead (each falls back to GDI automatically if it isn't available). Direct3D 11 is what a shader tool such as librashader needs.\r\n"
  "\r\n"
  "AUDIO\r\n"
  "  Mute (Ctrl+M):\r\n"
  "   Silences the sound; untick to hear it again.\r\n"
  "\r\n"
  "  Mute FM Sound (Ctrl+N), Mute PSG Sound (Ctrl+B), Mute PWM Sound (Ctrl+P):\r\n"
  "   Silence the FM chip (the main music), the PSG chip (the square-wave sound) or, in a 32X game, the PWM sound (most of its music and effects) on its own, without touching the saved levels. The level of the PWM sound is the PWM Preamp slider of Levels and Latency.\r\n"
  "\r\n"
  "  Levels and Latency...:\r\n"
  "   Master volume, FM and PSG levels (they work for 32X games too), CD audio (not for 32X), how many frames of sound are buffered, and on the right the audio filter (Off, Low-Pass or a 3-Band EQ) with its settings and the Sega CD PCM volume. The sliders act while you drag them (Cancel puts the old levels back), and a live Level meter at the bottom (left and right) shows what is playing (it stays empty when no game is running); it moves while the game keeps running behind the window.\r\n"
  "\r\n"
  "  High-Quality FM Synthesis, Master System FM Unit and Chip (Settings > Audio):\r\n"
  "   Band-limited FM, whether the Master System's FM unit is used (auto, off, on), and which chip emulates it.\r\n"
  "\r\n"
  "  FM Chip:\r\n"
  "   Which Genesis FM sound chip is emulated: YM2612 or YM3438, the exact Nuked versions (default is YM2612 Nuked) or MAME. A 32X game (PicoDrive) has only the two chip types: the YM3438 kinds play as a YM3438, the others as a YM2612.\r\n"
  "\r\n"
  "  Audio Backend:\r\n"
  "   Which Windows sound system plays the game's sound: Windows Classic, XAudio2, WASAPI (Shared; the default) or WASAPI (Exclusive). The newer ones can have lower delay and cope better when the PC is busy. Exclusive gives the game the sound device for itself, for the lowest delay: no other program can play sound on that device while the game runs, and the device has to accept 16-bit stereo at the chosen Sample Rate. If one cannot be opened the game falls back (Exclusive to Shared, then to Windows Classic). Sound Device applies to all of them (Settings > Audio). Takes effect at once.\r\n"
  "\r\n"
  "  Console Model:\r\n"
  "   A sound preset in the Audio menu (Console Model, with the shortcuts Ctrl+Shift+3, 1 and 2): Default puts the program's own sound set-up back (YM2612 Nuked, no low-pass filter); Genesis Model 1 is the early, duller console (the YM2612 chip with its low-pass filter), Genesis Model 2 the later, cleaner one (the YM3438 chip, no filter). They set FM Chip and Low-Pass Filter; changing those yourself leaves none ticked.\r\n"
  "\r\n"
  "  Sample Rate:\r\n"
  "   22050 Hz (lighter and duller, for a slow PC), 44100 or 48000 Hz.\r\n"
  "\r\n"
  "  3-Band EQ (Settings > Audio, Tone):\r\n"
  "   Switches the equalizer on or off like the Low-Pass Filter tick (only one of the two can be on); its gains and crossover frequencies are in Levels and Latency.\r\n"
  "\r\n"
  "  High-Quality PSG Resampling, Low-Pass Filter, 3-Band EQ, Mono Output (Audio > Tone and Effects, which also holds Stereo Width, Bass Boost, Overdrive and Even Out Loudness):\r\n"
  "   Cleaner PSG sound, a soft high-frequency roll-off, and mono instead of stereo.\r\n"
  "\r\n"
  "  Stereo Width:\r\n"
  "   Narrow (50%), Normal, Wide (150%) or Extra Wide (200%): pulls the left and right sound toward the middle or spreads them apart (Settings > Audio).\r\n"
  "\r\n"
  "  Bass Boost:\r\n"
  "   Off, Light, Medium or Strong: lifts the low notes so the music is fuller on laptop speakers and small headphones (Settings > Audio).\r\n"
  "\r\n"
  "  Overdrive:\r\n"
  "   Pushes the sound into a soft limiter: louder, fuller and a little gritty, like a console turned up. Off by default (Settings > Audio).\r\n"
  "\r\n"
  "  Even Out Loudness:\r\n"
  "   Lowers very loud games and lifts quiet ones so they sit at a similar level, without clipping. Off by default (Settings > Audio).\r\n"
  "\r\n"
  "  Sound Device...:\r\n"
  "   A drop-down of your sound devices: which speakers or headphones the game plays through; Windows Default follows the Windows setting (Settings > Audio).\r\n"
  "\r\n"
  "INPUT\r\n"
  "  Controls...:\r\n"
  "   One window with a tab for Player 1, one for Player 2 and an Extras tab (what is plugged into the console's two controller ports, and the combos). The pad tabs lay the buttons out like a Mega Drive pad in titled frames (D-Pad with its deadzone, System, Buttons with their Turbo ticks) around a picture of the pad. Click a button, then press the key or gamepad button you want (Esc or a right click cancels the wait; right-click a button to set or clear it; clicking its spot on the picture works too). Tick Turbo beside A, B, C, X, Y or Z: held down, the button then fires repeatedly (Turbo Rate sets how fast). A key or button can only be on one pad button: assigning it takes it off the one that had it (also on the other player, for keys), and a line under the pad says what was removed, and when the key is also a shortcut or the pad button also a gamepad hotkey. Auto Assign goes through them all in turn. Pressing a mapped key or gamepad button lights its spot on the picture, so you can check a pad; tick Test Keys to try keys such as Enter, Tab and Esc without them operating the window. You can also set the gamepad thumbstick's deadzone (how far it has to move off-center before it registers at all). The right stick counts as four more buttons (RS Up, Down, Left, Right) that can be assigned like any other; this works for XInput pads and, where the pad reports the stick on its Rx / Ry or Z / Rz axes, for DirectInput ones. Gamepad Hotkeys... opens a window to choose which pad button runs each shortcut, pressed on its own (no Back needed): Fast Forward and Rewind (held), Toggle Turbo, Save State and Load State (current slot), and Previous and Next Slot. By default the right stick does the last four (up = save, down = load, left = previous slot, right = next slot) and LB / LT are Fast Forward / Rewind; the right stick button (RSB) switches Turbo on or off (the buttons keep their Turbo setting). A button used there also reaches the game, so the right stick is a good choice. Gamepad hotkeys use the Xbox-style (XInput) pad of Player 1 only. Player 1 defaults: arrows, Z X C = A B C, A S D = X Y Z, Enter = Start, Right Shift = Mode. Restore Defaults (bottom left) opens a list: Player 1, Player 2 or Extras Tab (the combos removed and both ports back to a Control Pad); only that one goes back to its defaults (a player's also resets the Turbo Rate).\r\n"
  "\r\n"
  "  Hotkeys...:\r\n"
  "   Every keyboard shortcut in one list: select an action, click Assign (or double-click it) and press the key you want, alone or with Ctrl, Shift or Alt. A key can only be on one action: the old one loses it. Clear removes a shortcut and Restore Defaults brings the original keys back. The menu labels and the Keyboard Shortcuts window follow your choices. Fast Forward and Rewind (the keys you hold, Tab and Backspace by default) are in the list too; their gamepad buttons are under Controls > Gamepad Hotkeys.\r\n"
  "\r\n"
  "  Port A / Port B Device:\r\n"
  "   What is plugged in: Control Pad, Mouse, a light gun (Light Phaser, Menacer or Justifier, depending on the port), XE-1AP analog joystick, Activator, Paddle Control, Sports Pad, Graphic Board (Pico's drawing tablet), a 4-player Team Player adapter, EA 4-Way Play (4 players; it takes both ports), a Master Tap (the Master System's 4-player adapter) or the Smash Controller; not every device is on both ports. Found in Input > Controls, on the Extras tab (no longer in the Input menu or Settings).\r\n"
  "\r\n"
  "  Enable Background Input:\r\n"
  "   Keeps reading the controller while another window has focus.\r\n"
  "\r\n"
  "  Show Light Gun Crosshair:\r\n"
  "   Shows a crosshair instead of the arrow over the picture while a light gun is plugged in (off by default; Settings > Input only, not in the Input menu).\r\n"
  "\r\n"
  "  Invert Mouse Y Axis:\r\n"
  "   Flips the up/down movement of the Sega Mouse (Settings > Input only, not in the Input menu).\r\n"
  "\r\n"
  "  Combo Buttons...:\r\n"
  "   Makes one key or pad button press several game buttons together. It is on the Extras tab of Controls (Input > Controls...), in one Combos frame. The Saved Combos list always shows 12 numbered rows (number, trigger, actions). Click a row (an empty one for a new combo), then click the Trigger button and press the key or gamepad button to use (a letter or digit, F1-F12, Num0-Num9, Space, an arrow key, or a pad button: A B X Y LB RB LT RT Back Start or the D-pad on XInput pads). Then click the game buttons you want in the Actions grid (Up Down Left Right, Start Mode, X Y Z, A B C): each click adds that button to the row, for example Gamepad Y -> Down+A. The Hold button opens a menu that makes the action you just added Held: after the trigger is pressed it stays down by itself, for the time chosen (0.25 to 10 seconds) or until you press that button yourself, shown as Down(Hold 1.0s); Off (No Hold) is the default, the action is then only pressed while the trigger is. The Pause button opens a menu to wait before the next action starts (Pause 0.5s); without one, the action after a Held one starts 70 ms later (Down, then A). Remove Last takes the last action off the row, Delete Combo empties the selected row (up to 12 combos). If every action is Held until you let go, the trigger works as a switch: press it to hold them, press it again to let go. Held actions are also let go when the window loses focus or a game is opened or closed. The How It Works button, at the right end of the list's title row, explains all this with an example. Saved with Controls. They work for Player 1.\r\n"
  "\r\n"
  "  Profile (Controls window):\r\n"
  "   In the Controls window, a Profile box with Load, Save and Delete: a profile stores both players' keys and buttons, pad types, deadzones, the turbo rate and the combos (not the ports; in the profiles folder next to the program). The Profile box is a drop-down you can also type in: pick a saved profile or type a name, then use Load (loads it into the window, and OK keeps it), New (a new profile from the window, under the name typed in the box), Save (saves the window into the loaded profile; with another name typed in the box it renames the loaded profile, after asking) or Delete. Restore Defaults (next to OK) puts both players' keys, buttons, deadzones, turbo rate and combos back to their defaults; Load and Delete are grayed while no profile is saved.\r\n"
  "\r\n"
  "  Mouse Speed:\r\n"
  "   How fast the Sega Mouse moves: 50 to 200%.\r\n"
  "\r\n"
  "  Pause When the Pad Disconnects:\r\n"
  "   Pauses the game with a message when Player 1's gamepad is unplugged or its battery runs out, instead of running on without input. Off by default (Settings > Input).\r\n"
  "\r\n"
  "  Gun Aim Offset X / Y:\r\n"
  "   Shifts where the light gun hits by up to 12 pixels, if shots land beside or above where you point.\r\n"
  "\r\n"
  "  Confine Mouse to the Picture:\r\n"
  "   While a Sega Mouse or light gun game runs, keeps the pointer inside the picture. Pausing the game, opening a menu or dialog, or Alt+Tab lets it go.\r\n"
  "\r\n"
  "  Gamepad Hotkeys:\r\n"
  "   Under Controls, besides fast forward, rewind and the states, these can each be put on a gamepad button (none by default): Pause, Reset, Screenshot, Fullscreen, Mute, Record Video + Audio, Game Speed Faster and Slower, and the Pause Menu. A box in the same window makes them need a held Hold Button (the last row), so the pad's buttons are free in the game when it is not held. The Pause Menu pauses the game and shows a small menu over the picture: Resume, Save State, Load State, Reset and Quit to the List. Move with the D-pad (or the arrow keys), choose with the pad\'s A or Start (or Enter), and go back with B (or Esc).\r\n"
  "\r\n"
  "TOOLS\r\n"
  "  Cheats... (Ctrl+C):\r\n"
  "   With no game running, this opens the cheats of the game picked in the list, without starting it. Add Game Genie, Action Replay or raw codes (also a work RAM byte, aaaaaa:dd), or load a .cht file. Click #, Code, Description or Status to sort the list; the # is the number a cheat got when it was added and stays with it. Right-clicking the column titles offers Size All Columns to Fit. The Status column says Active, Off, Cheats are off, Game not running or Code not valid for this system. ROM codes wait for the first button press after a game starts or is reset (at least two seconds, at most twenty), because a game checks its ROM first. In netplay both PCs need the same cheats on. Convert... shows a code as Game Genie, Action Replay or raw. When other files in the game's console folder under Cheats have a name starting like the game's (\"Sonic The Hedgehog (Action Replay).cht\" beside \"Sonic The Hedgehog (USA).cht\"), the Similar Cheat Files drop-down appears under the buttons to load their cheats (when the list already has cheats it offers Load, which replaces the list, or Insert, which merges the file's cheats into it); it is not shown otherwise. A file in Cheats\\library named after the game's CRC32 (12345678.cht) is loaded by itself for a game that has no cheats of its own.\r\n"
  "\r\n"
  "  Cheat Search...:\r\n"
  "   Finds where a value (lives, health, ammo; or with Size 2 bytes a score or money, saved as two cheats) is kept in memory, so you can make a cheat without a code (a Mega CD game can also search the CD's own memory, Word RAM in either of its modes: pick it under Memory). A reset or a state load starts the search again; closing the window keeps it until the game stops. Working it through: First Scan, change the value in the game, choose Compare (Equal to Value, Changed, Unchanged, Increased or Decreased) and Next Scan, and repeat until a few addresses are left; select one, type the value to hold in Set to and click Add Cheat. It becomes an ordinary RAM cheat in the Cheats list.\r\n"
  "\r\n"
  "  Record Input..., Play Input Recording..., Stop Input Recording / Playback:\r\n"
  "   Record Input saves the state of the game at that moment and every button you press after it into a .gpm file (named after the game and the time, in the Input folder inside the recordings folder, like the other recordings); Play Input Recording loads that state and presses the same buttons again, exactly: for speedruns and bug reports. It must be played with the same game. Stop ends either one; closing the game does too. Not during netplay. These are in Tools > Record.\r\n"
  "\r\n"
  "  Record VGM File:\r\n"
  "   Records what the sound chips play into a .vgm file that music players supporting the format can play. Like the other recordings it goes in the recordings folder (in its VGM folder, in the console's folder), named after the game and the time, and Stop Recording (Alt+F11) ends it (the item is grayed while logging). Shift+F10 starts it. A recording that starts while a game plays begins with the state of the sound chips, so the instruments are there even if the song started earlier (the FM chip is read back, whichever FM Chip is chosen, so this holds after a state was loaded too). At the end every held note is released and the PSG turned down, so the file does not end on a ringing tone. A 32X game's PWM sound is recorded too; only some players can play it (the libvgm-based ones). It is in Tools > Record, and grayed out until a game is loaded.\r\n"
  "\r\n"
  "  Netplay... / Stop Netplay:\r\n"
  "   Two-player game over a local network: one PC hosts, the other joins. Both need the same ROM, the same program file and the same settings. The status bar shows the ping to the other player and whether a different delay would suit it better. A lost connection is given a minute to come back. The Host box can open the router's port for you (UPnP, off until you tick it).\r\n"
  "\r\n"
  "  Record:\r\n"
  "   Video + Audio (Shift+F11) makes an .mp4, Audio Only (Ctrl+F11) a .wav. Files go in the recordings folder, each kind in a folder of its own (Video, Audio, GIF, VGM, Input) and in it a folder for the console. How a video recording looks is set in Settings > Options > Video Recording: its size (1x to 4x the console's own picture, 2x by default), its quality (automatic, or 4 to 32 Mbit/s) and its shape (the console's own, stretched to 4:3, or stretched to 16:9). Record Animated GIF makes a picture-only .gif (20 pictures a second, up to 6 seconds, then it stops by itself) with Ctrl+F12. Stop Recording (Alt+F11) ends it, a Record VGM File log too.\r\n"
  "\r\n"
  "  Save Screenshot (F12):\r\n"
  "   Saves a PNG named after the game and the time.\r\n"
  "\r\n"
  "  Screenshot Output:\r\n"
  "   Corrected (the default): the picture stretched to the 4:3 shape of a TV, 640x480. Raw: the console's own picture at its own size (for example 320x224), not stretched. Final: like Corrected, but with the Render Filter applied (the same as Corrected when no filter is on). Settings > Options only (no longer in the Tools menu).\r\n"
  "\r\n"
  "  Screenshot Size:\r\n"
  "   1x to 4x: how many times larger than the output's own size the screenshot is (Corrected 640x480, Raw the console's own size, Final the filter's size). Raw is enlarged in whole pixels so it stays sharp. Settings > Options only.\r\n"
  "\r\n"
  "OPTIONS\r\n"
  "  Settings...:\r\n"
  "   The options of the menus, and some more, on tabs (General, Video, Audio, Picture, Input, Options and OSD). Hover over an option for a short description (it hides after 30 seconds). Restore Defaults (bottom left) opens a list: All Tabs puts every setting on all tabs back to its default, keeping your controls, ROM folder and cheats; This Tab does the same for the tab you are looking at only. The window grows taller when a tab needs more room. OK keeps what you changed; Cancel (also the X and Esc) puts every setting back as it was when the window opened. Changes show at once while the window is open, so you can see their effect; the buttons that open another window or act on files (ROM Directories, Bezel Image, Clear Cover Cache, Restore Dialog) work immediately and are not undone by Cancel. A change takes effect at once, exactly as if you had used the menu; the windows that have their own settings open from a button.\r\n"
  "\r\n"
  "  Region:\r\n"
  "   Which region the console reports to the game: detect from the ROM, USA, Europe or Japan.\r\n"
  "\r\n"
  "  Force VDP Mode:\r\n"
  "   Runs the video timing at 60 Hz (NTSC) or 50 Hz (PAL), whatever the region says.\r\n"
  "\r\n"
  "  Console:\r\n"
  "   Detects the console from the game, or forces a specific model.\r\n"
  "\r\n"
  "  Lock-On Cartridge:\r\n"
  "   Attaches a Game Genie, Action Replay or Sonic & Knuckles to the game. Settings > Options only (not in the Options menu).\r\n"
  "\r\n"
  "  Boot from BIOS When Available:\r\n"
  "   Runs the console's start-up ROM when a BIOS file is present (see below). Settings > Options only (not in the Options menu).\r\n"
  "\r\n"
  "  Emulate Address Error Exceptions:\r\n"
  "   Hardware-accurate crashes on bad memory access; turn off only for a few hacked games. Settings > Options only (not in the Options menu).\r\n"
  "\r\n"
  "  Emulate Mega CD Disc Latency:\r\n"
  "   Makes the Mega CD drive seek slowly like the real one (some games need it); off, the default, makes loading instant. Settings > Options only (not in the Options menu).\r\n"
  "\r\n"
  "  Show Extended Game Gear Screen:\r\n"
  "   Shows the whole Game Gear picture instead of only the part its screen displayed. Settings > Options only (not in the Options menu).\r\n"
  "\r\n"
  "  Show Master System Side Borders:\r\n"
  "   Off by default, which crops the 8-pixel columns at each side; tick to show them. Settings > Options only (not in the Options menu).\r\n"
  "\r\n"
  "  Show Frame Rate (Ctrl+T):\r\n"
  "   Frames per second on screen. (Settings > OSD.) Ctrl+T switches it on and off, and it can be put on a pad button in Gamepad Hotkeys.\r\n"
  "\r\n"
  "  Messages for Changed Options:\r\n"
  "   On by default: changing an option shows its name and new value (needs Show Messages) (Settings > OSD).\r\n"
  "\r\n"
  "  Message Position:\r\n"
  "   Which corner of the picture messages appear in: top left, top right, bottom left (the default) or bottom right (Settings > OSD).\r\n"
  "\r\n"
  "  Message Time:\r\n"
  "   1, 2, 3 (the default), 4, 5, 6 or 7 seconds: how long each message stays on screen (Settings > OSD). Messages Shown (1 to 7, default 3) sets how many of the latest messages stay together, the newest nearest the corner and older ones stacked beside it; each fades out when its time is up.\r\n"
  "\r\n"
  "  Font Color:\r\n"
  "   White (the default), Yellow, Green, Cyan, Orange, Pink, Red or Black: the color of messages, the frame rate and the pause menu title (Settings > OSD). The outline turns light for dark colors, so the text stays readable on any picture.\r\n"
  "\r\n"
  "  Font Size:\r\n"
  "   Small, Normal, Large or Extra Large: the size of the text of messages, the frame rate and the pause menu (Settings > OSD).\r\n"
  "\r\n"
  "  Frame Rate Position:\r\n"
  "   Which corner of the picture the frame rate appears in: top left, top right (the default), bottom left or bottom right (Settings > OSD).\r\n"
  "\r\n"
  "  Backup State:\r\n"
  "   Off, or every 1, 2, 5 or 10 minutes while a game runs: a state is saved quietly into three rolling backups. Restore one with Load State > Load Backup State. Not used during netplay (Settings > Options).\r\n"
  "\r\n"
  "  Skip Intro on Launch:\r\n"
  "   Off, or 5, 10, 15, 20, 25 or 30 seconds: when a game starts, its first seconds run at full speed to get past logos and intros. A press of a game button ends the skip early. Not used when resuming a game or during netplay (Settings > Options). Off by default.\r\n"
  "\r\n"
  "  Enable Log:\r\n"
  "   Writes genesis-plus-ex.log in the Logs folder next to the program: the program version, Windows and the main settings, games loaded and closed, resets, saved and loaded states, the cheat codes read and when the ROM ones go in, messages, errors, netplay events and crashes. Off by default; send it along with a bug report (Settings > Options).\r\n"
  "\r\n"
  "  Keep a Cover Cache:\r\n"
  "   On by default. Each finished cover is saved (small and compressed) in one file, Covers\\thumbs.dat, so the next time the program starts the grid covers appear almost at once instead of being read and scaled again. A cover you change, or a new cover size or frame, is made again by itself; the first run, and a new size, still take the usual time. Clear Cover Cache (next to it in Settings > General) deletes the saved covers. The file holds roughly 20 to 60 KB per game and size, and is emptied by itself when it passes 300 MB.\r\n"
  "\r\n"
  "  Enable Crash Report:\r\n"
  "   If the program crashes it writes genesis-plus-ex-crash.txt and genesis-plus-ex-crash.dmp in the Logs folder next to the program, whether or not Enable Log is on (only the last crash is kept). Off by default; switch it on with Enable Crash Report in Settings > Options. Send both with a bug report.\r\n"
  "\r\n"
  "  Show Messages:\r\n"
  "   Short messages over the picture, such as \"Saved to slot 3\" or the name of a screenshot. Changing an option, from a menu or from Settings, shows its name and new value too (\"Smooth Scaling: On\", \"Scanlines: 50%\"). Untick to hide them (the status bar still shows them). (Settings > OSD.)\r\n"
  "\r\n"
  "HELP\r\n"
  "  Keyboard Shortcuts (F1):\r\n"
  "   The list of hotkeys.\r\n"
  "\r\n"
  "  User Guide:\r\n"
  "   This page. Pick a section on the left to show just that part. The window can be made bigger or maximized.\r\n"
  "\r\n"
  "  About Genesis Plus EX:\r\n"
  "   Version and credits.\r\n"
  "\r\n";

static const char guide_bios_text[] =
  "BIOS AND FIRMWARE FILES\r\n"
  "Required or optional firmware files go in the program's \"BIOS\" folder (Open Genesis Plus EX Folder, then BIOS).\r\n"
  "\r\n"
  "Filename:\tDescription:\r\n"
  "\r\n"
  "- bios_MD.bin\tMega Drive TMSS start-up ROM - optional\r\n"
  "- bios_CD_E.bin\tMega CD Europe BIOS - required for Mega CD Europe games\r\n"
  "- bios_CD_U.bin\tSega CD USA BIOS - required for Sega CD USA games\r\n"
  "- bios_CD_J.bin\tMega CD Japan BIOS - required for Mega CD Japan games\r\n"
  "- bios_E.sms\tMaster System Europe BIOS - optional\r\n"
  "- bios_U.sms\tMaster System USA BIOS - optional\r\n"
  "- bios_J.sms\tMaster System Japan BIOS - optional\r\n"
  "- bios.gg\tGame Gear BIOS - optional\r\n"
  "- sk.bin\tSonic & Knuckles ROM (lock-on) - optional\r\n"
  "- sk2chip.bin\tSonic & Knuckles UPMEM ROM (lock-on) - optional\r\n"
  "- areplay.bin\tAction Replay ROM (lock-on) - optional\r\n"
  "- ggenie.bin\tGame Genie ROM (lock-on) - optional\r\n"
  "- 32X_G_BIOS.BIN\t32X Genesis-side BIOS - not needed, not used (32X games start without it)\r\n"
  "- 32X_M_BIOS.BIN\t32X Master SH-2 BIOS - not needed, not used\r\n"
  "- 32X_S_BIOS.BIN\t32X Slave SH-2 BIOS - not needed, not used\r\n";

/* One page of text, split at run time into its sections (a header is a line
   with no indent, in capitals; entries are the indented lines under it) so the
   guide can be browsed by section and filtered by a search word. */
#define GUIDE_MAX_SECTIONS 16

static const char guide_extra_text[] =
  "CONTROLS WINDOW\r\n"
  "  Where It Is:\r\n"
  "   Input > Controls... or the Controls button on the toolbar. It has a tab for Player 1, one for Player 2 and an Extras tab (Alt+1, Alt+2 and Alt+E open them).\r\n"
  "  Buttons of the Pad:\r\n"
  "   Click a button, then press the key or gamepad button you want. The same press works for keyboard and pad. Esc or a right click cancels. A key or button can only be on one pad button; giving it to a new one takes it from the old one.\r\n"
  "  Input Devices:\r\n"
  "   Which gamepad (or the keyboard) the window shows and reads; the arrow next to it looks for pads again. Xbox pads (and others that Windows shows as one) are read as XInput pads. PlayStation pads (DualShock 4, DualSense), Nintendo pads (Switch Pro Controller and the like) and other joysticks are read through DirectInput: when you pick one, the buttons start in a layout that fits it (the D-pad on its D-pad, the face buttons where the Mega Drive pad has them), and everything can be changed. A Joy-Con is a joystick of its own, so one at a time is used.\r\n"
  "  Controller Model:\r\n"
  "   Which Mega Drive pad the game sees for this player: Match the Game (the default), 3-Button, 6-Button or 2-Button.\r\n"
  "  Turbo and Turbo Rate:\r\n"
  "   The Turbo box beside A, B, C, X, Y and Z makes that button fire again and again while it is held. Turbo Rate sets how fast: Slower, Slow, Medium (the default), Fast or Faster.\r\n"
  "  Deadzone:\r\n"
  "   How far a stick has to move before it counts.\r\n"
  "  Profile:\r\n"
  "   Load, New, Save and Delete keep whole control setups (both players, pad types, deadzones, turbo rate and combos) in the Profiles folder.\r\n"
  "  Auto Assign...:\r\n"
  "   Goes through all the buttons in turn so you only press the keys.\r\n"
  "  Gamepad Hotkeys...:\r\n"
  "   Puts shortcuts (fast forward, rewind, states, pause, screenshot and more) on pad buttons; an optional hold button keeps the pad free for the game until it is held.\r\n"
  "  Test Keys:\r\n"
  "   Try your keys: while it is ticked the keyboard no longer moves around or presses buttons in this window, so you can press any key or pad button and see its spot light.\r\n"
  "  Extras Tab:\r\n"
  "   Port A Device and Port B Device choose what is plugged into each port (Control Pad, Mouse, Light Phaser, Menacer, Justifier, Paddle, Sports Pad, Team Player, Master Tap and more). Combo buttons let one key or pad button press several game buttons: pick a row, choose the Trigger, then click the game buttons to add; Hold keeps a button down for a time and Pause waits before the next one. Up to 12 combos; How It Works shows an example.\r\n"
  "  Restore Defaults:\r\n"
  "   Puts both players' keys, buttons, deadzones, turbo rate and combos back to their defaults. OK keeps what you changed; Cancel puts everything back.\r\n"
  "\r\n"
  "CHEATS WINDOW\r\n"
  "  Where It Is:\r\n"
  "   Tools > Cheats... (Ctrl+C), or Edit Cheats in a game's right-click menu in the list (no need to start the game).\r\n"
  "  Enable Cheats:\r\n"
  "   Turns every cheat on or off at once. It is off until you turn it on.\r\n"
  "  The List:\r\n"
  "   #  The number a cheat got when it was added; it stays with the cheat when the list is sorted.\r\n"
  "   Box  Tick it to switch that cheat on; the box in the column title ticks or unticks all of them.\r\n"
  "   Code  A Game Genie, Action Replay or raw code (the hint under the buttons shows what this console accepts).\r\n"
  "   Description  The name you gave it.\r\n"
  "   Status  Active, Off, Cheats are off, Game not running, or Code not valid for this system.\r\n"
  "   Click a title to sort by it (again to turn the order round); right-click the titles for Size All Columns to Fit.\r\n"
  "  Name and Code:\r\n"
  "   Type a name and a code, then Add Code. Select a cheat to edit it, then Save.\r\n"
  "  Remove and Remove All:\r\n"
  "   Take out the selected cheat, or every cheat (it asks first).\r\n"
  "  Load Cheat...:\r\n"
  "   Adds the cheats of a RetroArch .cht file to the list.\r\n"
  "  Export...:\r\n"
  "   Saves the list as a RetroArch .cht file (name, code and whether it is on), to keep, share or use in another program; Load Cheat reads it back.\r\n"
  "  Convert...:\r\n"
  "   Shows a code as Game Genie, Action Replay or raw.\r\n"
  "  Similar Cheat Files:\r\n"
  "   Other cheat files in the console's Cheats folder whose name starts like the game's. Pick one to load its cheats; when the list already has cheats you choose Load (replace the list) or Insert (merge them in).\r\n"
  "  Cheat Search:\r\n"
  "   Tools > Cheat Search... (a window of its own) finds where a value (lives, health, a score) is kept in memory, so you can make a cheat without a code.\r\n"
  "  Good to Know:\r\n"
  "   Right-clicking the column titles of the result list offers Size All Columns to Fit. ROM codes wait for the first button press after a game starts or resets (twenty seconds at most). In netplay both PCs need the same cheats on, and the window is locked during the session.\r\n"
  "\r\n"
  "NETPLAY WINDOW\r\n"
  "  Where It Is:\r\n"
  "   Tools > Netplay...\r\n"
  "  Before You Start:\r\n"
  "   Both PCs load the same ROM, use the same program and the same settings. Only cartridge games work (not Mega CD), and both ports need a Control Pad.\r\n"
  "  Host:\r\n"
  "   Pick a port (55455 by default), an input delay (1 to 4 frames) and, if you like, a session code, then press Host. The host is Player 1.\r\n"
  "  Join:\r\n"
  "   Press Find hosts and choose the host (or type its address), enter the code and press Join. The guest is Player 2.\r\n"
  "  During a Session:\r\n"
  "   Both games reset and start together. The status bar shows the ping. Pause, reset, states, cheats, rewind, fast forward and emulation settings are locked.\r\n"
  "  If Something Differs:\r\n"
  "   A setting that differs between the PCs refuses the connection and names it; the games are compared every second.\r\n"
  "  Open the Port on My Router Automatically (UPnP):\r\n"
  "   Lets friends outside your network join with your public address (UPnP). Otherwise forward the port yourself; Windows Firewall may ask to allow TCP 55455 and UDP 55456.\r\n";

static struct
{
  char *text;                          /* working copy, lines end in \n */
  int   count;                         /* sections, [0] = introduction */
  const char *name[GUIDE_MAX_SECTIONS];
  char *first[GUIDE_MAX_SECTIONS];     /* first line (the header) of each */
  char *end[GUIDE_MAX_SECTIONS];
} guide;

static int guide_is_header(const char *line)
{
  const char *p = line;
  int letters = 0;

  if (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\0' || *p == '\t') return 0;
  for (; *p && *p != '\r' && *p != '\n'; p++)
  {
    if (*p >= 'a' && *p <= 'z') return 0;
    if (*p >= 'A' && *p <= 'Z') letters++;
  }
  return letters >= 3;
}

/* The whole guide: the menus, then the windows (Settings is made from its own table of options, so it cannot fall behind). */
static void guide_parse(void)
{
  char *p;
  int s = 0;
  size_t cs = settings_guide_text(NULL, 0), need = sizeof(user_guide_text) + sizeof(guide_extra_text) + sizeof(guide_bios_text) + cs + 16;

  guide.text = (char *)malloc(need);
  if (!guide.text) return;
  lstrcpyA(guide.text, user_guide_text);
  lstrcatA(guide.text, "\r\n");
  settings_guide_text(guide.text + lstrlenA(guide.text), cs + 1);
  lstrcatA(guide.text, "\r\n");
  lstrcatA(guide.text, guide_extra_text);
  lstrcatA(guide.text, "\r\n");
  lstrcatA(guide.text, guide_bios_text);                       /* the firmware files come last */
  lstrcatA(guide.text, "\r\n");

  guide.name[0] = "Introduction";
  guide.first[0] = guide.text;

  for (p = guide.text; *p; )
  {
    char *eol = strchr(p, '\n');
    if (!eol) eol = p + lstrlenA(p) - 1;

    if (guide_is_header(p) && s + 1 < GUIDE_MAX_SECTIONS)
    {
      guide.end[s] = p;
      s++;
      guide.first[s] = p;
      guide.name[s] = p;               /* trimmed to the line when listed */
    }
    p = eol + 1;
  }
  guide.end[s] = p;
  guide.count = s + 1;
}

/* What each paragraph of the page looks like. */
enum { GP_H1, GP_H2, GP_PARA, GP_ROW, GP_LOGO, GP_CTITLE, GP_CENTER };
typedef struct { int start, len, type, label; } GuidePara;

/* The first page shows the logo in the middle, with the credits under it: the page keeps room for the logo at its top and a picture window
   on the text box covers that room (see guide_logo_place). */
static int guide_logo_on;                       /* the page shown is the first one */
#define GUIDE_LOGO_H TBS(96)                    /* the logo's height, in pixels */
#define GUIDE_LOGO_MARGIN TBS(12)               /* the room above and below it */
static HWND guide_logo_wnd;                     /* a picture window on the text box, over the room kept for the logo */
static int guide_logo_w;

static int guide_logo_dx;                       /* the logo's shake, in pixels (a hidden extra) */
static DWORD guide_logo_wob;                    /* when the pointer came onto the logo, 0 when it is not on it */

/* The logo's window covers all the room kept for the logo (so nothing of the line under it shows through, and a click on it selects no text). */
#define GUIDE_LOGO_ROOM (GUIDE_LOGO_H + 2 * GUIDE_LOGO_MARGIN)

/* Puts the logo's window where the room for it is now (the text may have scrolled or the box been resized); hides it when it is not in view. */
static void guide_logo_place(HWND rich)
{
  RECT cr;
  POINT pt;
  int y;

  if (!guide_logo_wnd) return;
  if (!guide_logo_on) { ShowWindow(guide_logo_wnd, SW_HIDE); return; }
  GetClientRect(rich, &cr);
  pt.x = pt.y = 0;
  SendMessage(rich, EM_GETSCROLLPOS, 0, (LPARAM)&pt);              /* how far the page is scrolled: the room for the logo is at its very top */
  y = -pt.y;
  if (y + GUIDE_LOGO_ROOM <= 0 || y >= cr.bottom) { ShowWindow(guide_logo_wnd, SW_HIDE); return; }
  {
    RECT cur;
    int x = (cr.right - guide_logo_w) / 2 + guide_logo_dx;
    GetWindowRect(guide_logo_wnd, &cur);
    MapWindowPoints(NULL, rich, (POINT *)&cur, 2);
    if (IsWindowVisible(guide_logo_wnd) && cur.left == x && cur.top == y) return;           /* already there */
    SetWindowPos(guide_logo_wnd, HWND_TOP, x, y, guide_logo_w, GUIDE_LOGO_ROOM, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    RedrawWindow(rich, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);      /* (the room may still show the other page: the box and the logo over it are painted at once) */
  }
}

/* The logo: the pointer on it shakes it for a moment, and seven clicks on it play the SEGA sound (hidden extras); the arrow stays the pointer,
   and a click selects nothing behind it. */
static LRESULT CALLBACK guide_logo_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
  (void)id; (void)ref;
  switch (msg)
  {
    case WM_SETCURSOR:
      SetCursor(LoadCursor(NULL, IDC_ARROW));
      return TRUE;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
      eggs_logo_click();
      return 0;

    case WM_MOUSEMOVE:
      if (!guide_logo_wob)
      {
        TRACKMOUSEEVENT te;
        te.cbSize = sizeof(te); te.dwFlags = TME_LEAVE; te.hwndTrack = h; te.dwHoverTime = 0;
        TrackMouseEvent(&te);
        guide_logo_wob = GetTickCount() | 1;
        SetTimer(h, 1, 16, NULL);
      }
      break;

    case WM_MOUSELEAVE:
      guide_logo_wob = 0;
      break;

    case WM_TIMER:
      if (wp == 1)
      {
        DWORD age = GetTickCount() - (guide_logo_wob ? guide_logo_wob : GetTickCount());
        int dx = guide_logo_wob ? eggs_wobble_dx(age, TBS(10)) : 0;

        if (dx != guide_logo_dx) { guide_logo_dx = dx; guide_logo_place(GetParent(h)); }
        if (!guide_logo_wob || age > 700) { KillTimer(h, 1); if (guide_logo_dx) { guide_logo_dx = 0; guide_logo_place(GetParent(h)); } }
        return 0;
      }
      break;

    case WM_NCDESTROY:
      RemoveWindowSubclass(h, guide_logo_proc, 1);
      break;
  }
  return DefSubclassProc(h, msg, wp, lp);
}

/* The window with the logo on the color of the page (a picture window cannot blend: the logo is laid over that color pixel by pixel first). */
static void guide_logo_make(HWND rich, COLORREF bg)
{
  HBITMAP logo, img;
  BITMAPINFO bi;
  unsigned char *src, *dst;
  void *dbits = NULL;
  int w = 0, x, y;
  HDC sc;

  logo = gui_logo_bitmap(GUIDE_LOGO_H, &w);
  if (!logo) { gui_log("User Guide: the logo could not be made"); return; }

  ZeroMemory(&bi, sizeof(bi));
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -GUIDE_LOGO_ROOM;                      /* top-down */
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  sc = GetDC(NULL);
  src = (unsigned char *)malloc((size_t)w * GUIDE_LOGO_ROOM * 4);
  img = CreateDIBSection(sc, &bi, DIB_RGB_COLORS, &dbits, NULL, 0);
  dst = (unsigned char *)dbits;
  bi.bmiHeader.biHeight = -GUIDE_LOGO_H;                         /* (the picture itself is read at its own height) */
  if (src && dst && GetDIBits(sc, logo, 0, GUIDE_LOGO_H, src, &bi, DIB_RGB_COLORS) == GUIDE_LOGO_H)
  {
    for (y = 0; y < GUIDE_LOGO_ROOM; y++)
      for (x = 0; x < w; x++)
      {
        int sy = y - GUIDE_LOGO_MARGIN;                              /* the room above and below the picture is the page's color */
        const unsigned char *p = src + ((size_t)(sy < 0 ? 0 : sy >= GUIDE_LOGO_H ? GUIDE_LOGO_H - 1 : sy) * w + x) * 4;      /* premultiplied B G R A */
        unsigned char *q = dst + ((size_t)y * w + x) * 4;
        int inv = (sy < 0 || sy >= GUIDE_LOGO_H) ? 255 : 255 - p[3];
        q[0] = (unsigned char)((inv == 255 ? 0 : p[0]) + GetBValue(bg) * inv / 255);
        q[1] = (unsigned char)((inv == 255 ? 0 : p[1]) + GetGValue(bg) * inv / 255);
        q[2] = (unsigned char)((inv == 255 ? 0 : p[2]) + GetRValue(bg) * inv / 255);
        q[3] = 255;
      }
    if (!guide_logo_wnd)
    {
      /* The text box paints over a window put on it unless it leaves its children alone. */
      SetWindowLong(rich, GWL_STYLE, GetWindowLong(rich, GWL_STYLE) | WS_CLIPCHILDREN);
      guide_logo_wnd = CreateWindowExA(0, "STATIC", "", WS_CHILD | SS_BITMAP | SS_NOTIFY, 0, 0, w, GUIDE_LOGO_ROOM, rich, NULL, g_inst, NULL);
      if (guide_logo_wnd) SetWindowSubclass(guide_logo_wnd, guide_logo_proc, 1, 0);
    }
    if (guide_logo_wnd)
    {
      HBITMAP old = (HBITMAP)SendMessage(guide_logo_wnd, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)img);
      if (old) DeleteObject(old);
      guide_logo_w = w;
      img = NULL;                                                    /* (the window has it now) */
    }
  }
  if (img) DeleteObject(img);
  free(src);
  ReleaseDC(NULL, sc);
  DeleteObject(logo);
}

static void guide_title_case(char *title)
{
  int k, up = 1;

  for (k = 0; title[k]; k++)
  {
    if (title[k] == ' ') up = 1;
    else if (up) up = 0;
    else if (title[k] >= 'A' && title[k] <= 'Z') title[k] = (char)(title[k] + 32);
  }
  if (!strncmp(title, "Bios", 4)) memcpy(title, "BIOS", 4);                 /* an acronym */
  { char *w = strstr(title, " And "); if (w) memcpy(w + 1, "and", 3); }     /* a small word */
}

/* Shows the chosen section (or all of them) in the text pane, as a formatted page: titles, items in bold, the text of each indented
   under it and the lists of options as two columns. */
static void guide_show(HWND dlg)
{
  HWND rich = GetDlgItem(dlg, IDC_USERGUIDE_TEXT);
  int sel = (int)SendDlgItemMessage(dlg, IDC_USERGUIDE_LIST, LB_GETCURSEL, 0, 0);
  int s, n = 0, cap_p = 4096, len = 0, k, dark = theme_is_dark(), big = gui.large_ui;
  size_t cap = (size_t)lstrlenA(guide.text ? guide.text : "") + 800;           /* (the credits of the first page too) */
  int prelude = 0;                                               /* paragraphs the first page has above its text: the logo's room and the credits */
  char *out = (char *)malloc(cap);
  GuidePara *ps = (GuidePara *)malloc(sizeof(GuidePara) * (size_t)cap_p);
  COLORREF fg = dark ? THEME_DARK_TEXT : GetSysColor(COLOR_WINDOWTEXT);
  COLORREF accent = dark ? theme_accent_edge() : theme_accent_bg(), bg = dark ? RGB(0x24, 0x24, 0x24) : GetSysColor(COLOR_WINDOW);
  const int COL = 3300;                                         /* the width of the first column of a list, in twips */
  CHARFORMAT2A cf;
  PARAFORMAT2 pf;
  CHARRANGE cr;

  if (!out || !ps || !guide.text) { free(out); free(ps); return; }

  guide_logo_on = (sel == 0);
  if (guide_logo_on)
  {
    static const char head[] = "Genesis Plus EX v" APP_VERSION " (" APP_BITS ")";
    static const char credits[] = CREDITS_TEXT;
    const char *c = credits;

    guide_logo_make(rich, bg);

    ps[n].start = len; ps[n].len = 1; ps[n].type = GP_LOGO; ps[n].label = 0;                 /* one space: the line the logo's room belongs to */
    out[len++] = ' '; out[len++] = '\r'; n++;
    ps[n].start = len; ps[n].len = (int)sizeof(head) - 1; ps[n].type = GP_CTITLE; ps[n].label = 0;
    memcpy(out + len, head, sizeof(head) - 1); len += (int)sizeof(head) - 1; out[len++] = '\r'; n++;
    while (*c)
    {
      const char *e = strchr(c, '\r');
      int l = e ? (int)(e - c) : lstrlenA(c);
      ps[n].start = len; ps[n].len = l; ps[n].type = GP_CENTER; ps[n].label = 0;
      memcpy(out + len, c, (size_t)l); len += l; out[len++] = '\r'; n++;
      c += l;
      while (*c == '\r' || *c == '\n') c++;
    }
    prelude = n;
  }

  for (s = 0; s < guide.count; s++)
  {
    char *p;

    if (sel != s) continue;              /* row n of the list is section n (row 0 the introduction) */
    for (p = guide.first[s]; p < guide.end[s]; )
    {
      char *eol = strchr(p, '\n'), *q;
      int l, type, label = 0, first_sp;

      if (!eol || eol >= guide.end[s]) eol = guide.end[s] - 1;
      l = (int)(eol - p);
      if (l > 0 && p[l - 1] == '\r') l--;
      if (l <= 0) { p = eol + 1; continue; }
      if (n + 2 >= cap_p || (size_t)len + (size_t)l + 4 >= cap) break;

      for (first_sp = 0; first_sp < l && (p[first_sp] == ' ' || p[first_sp] == '-'); first_sp++) ;
      q = (char *)memchr(p, '\t', (size_t)l);
      if (q && !(p[0] == ' ' && p[1] == ' ' && p[2] != ' ' && p[l - 1] == ':'))     /* a line with a tab is a row of a list */
      {
        int a = (p[0] == '-' ? 2 : 0), b = (int)(q - p);
        while (a < b && (p[a] == ' ' || p[a] == '\t')) a++;
        memcpy(out + len, p + a, (size_t)(b - a));
        out[len + (b - a)] = '\t';
        memcpy(out + len + (b - a) + 1, q + 1, (size_t)(l - b - 1));
        label = b - a;
        ps[n].start = len; ps[n].len = (b - a) + 1 + (l - b - 1); ps[n].type = GP_ROW; ps[n].label = label;
      }
      else
      {
        int a = 0, e = l;
        if (guide_is_header(p)) type = GP_H1;
        else if (p[0] == ' ' && p[1] == ' ' && p[2] != ' ' && p[l - 1] == ':') type = GP_H2;           /* an item: two spaces, then its name and a colon */
        else type = GP_PARA;
        while (a < e && p[a] == ' ') a++;
        if (type == GP_H2) e--;                               /* the colon */
        memcpy(out + len, p + a, (size_t)(e - a));
        if (type == GP_H1) { out[len + (e - a)] = '\0'; guide_title_case(out + len); }
        ps[n].start = len; ps[n].len = e - a; ps[n].type = type; ps[n].label = 0;
      }
      len += ps[n].len;
      out[len++] = '\r';
      n++;
      p = eol + 1;
    }
  }
  if (len > 0) len--;                                         /* no paragraph mark after the last one */
  out[len] = '\0';

  SendMessage(rich, WM_SETREDRAW, FALSE, 0);
  SendMessage(rich, EM_SETBKGNDCOLOR, 0, (LPARAM)bg);
  SetWindowTextA(rich, out);

  ZeroMemory(&cf, sizeof(cf));
  cf.cbSize = sizeof(cf);
  cf.dwMask = CFM_COLOR | CFM_SIZE | CFM_FACE | CFM_BOLD;
  cf.crTextColor = fg;
  cf.yHeight = big ? 240 : 200;
  lstrcpyA(cf.szFaceName, "Segoe UI");
  SendMessage(rich, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);

  for (k = 0; k < n; k++)
  {
    int top = ps[k].type == GP_H1 && k == 0 ? 0 : 1;

    cr.cpMin = ps[k].start; cr.cpMax = ps[k].start + ps[k].len;
    SendMessage(rich, EM_EXSETSEL, 0, (LPARAM)&cr);

    ZeroMemory(&pf, sizeof(pf));
    pf.cbSize = sizeof(pf);
    pf.dwMask = PFM_STARTINDENT | PFM_OFFSET | PFM_SPACEBEFORE | PFM_SPACEAFTER | PFM_TABSTOPS;
    ZeroMemory(&cf, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_BOLD | CFM_COLOR | CFM_SIZE;
    cf.crTextColor = fg;
    cf.yHeight = big ? 240 : 200;
    switch (ps[k].type)
    {
      case GP_H1:
        pf.dySpaceBefore = top ? 520 : 0; pf.dySpaceAfter = 140;
        cf.dwEffects = CFE_BOLD; cf.crTextColor = accent; cf.yHeight = big ? 360 : 300;
        break;
      case GP_H2:
        pf.dySpaceBefore = 240; pf.dySpaceAfter = 20;
        cf.dwEffects = CFE_BOLD; cf.yHeight = big ? 260 : 220;
        break;
      case GP_ROW:
        pf.dxStartIndent = 200; pf.dxOffset = COL; pf.dySpaceBefore = 40; pf.dySpaceAfter = 40;           /* the name at 200, the text (and what it wraps to) at COL + 200 */
        pf.cTabCount = 1; pf.rgxTabs[0] = COL + 200;
        break;
      case GP_LOGO:
        pf.dwMask |= PFM_ALIGNMENT; pf.wAlignment = PFA_CENTER;
        pf.dySpaceBefore = (GUIDE_LOGO_H + 2 * GUIDE_LOGO_MARGIN) * 1440 / gui_dpi; pf.dySpaceAfter = 0;      /* the room the logo is drawn in */
        cf.yHeight = 20;
        break;
      case GP_CTITLE:
        pf.dwMask |= PFM_ALIGNMENT; pf.wAlignment = PFA_CENTER;
        pf.dySpaceBefore = 0; pf.dySpaceAfter = 120;
        cf.dwEffects = CFE_BOLD; cf.yHeight = big ? 340 : 280;
        break;
      case GP_CENTER:
        pf.dwMask |= PFM_ALIGNMENT; pf.wAlignment = PFA_CENTER;
        pf.dySpaceBefore = 0; pf.dySpaceAfter = 20;
        break;
      default:
        pf.dxStartIndent = 200; pf.dySpaceAfter = 80;
        if (prelude && k == prelude) pf.dySpaceBefore = 420;            /* the text of the first page, a little below the credits */
        break;
    }
    SendMessage(rich, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    SendMessage(rich, EM_SETPARAFORMAT, 0, (LPARAM)&pf);

    if (ps[k].type == GP_ROW)                                   /* the name at the start of a row of a list is bold */
    {
      cr.cpMax = ps[k].start + ps[k].label;
      SendMessage(rich, EM_EXSETSEL, 0, (LPARAM)&cr);
      ZeroMemory(&cf, sizeof(cf));
      cf.cbSize = sizeof(cf);
      cf.dwMask = CFM_BOLD;
      cf.dwEffects = CFE_BOLD;
      SendMessage(rich, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    }
  }

  cr.cpMin = cr.cpMax = 0;
  SendMessage(rich, EM_EXSETSEL, 0, (LPARAM)&cr);
  SendMessage(rich, EM_HIDESELECTION, TRUE, 0);
  SendMessage(rich, WM_SETREDRAW, TRUE, 0);
  SendMessage(rich, WM_VSCROLL, SB_TOP, 0);
  InvalidateRect(rich, NULL, TRUE);
  guide_logo_place(rich);
  free(out);
  free(ps);
}

/* The User Guide can be resized and maximized: the list stays as wide as it was and the page takes the rest. */
static RECT guide_rl, guide_rt, guide_rb;      /* the list, the page and the Close button, in client coordinates, as the window opened */
static int  guide_cw, guide_ch, guide_minw, guide_minh;

static void guide_place(HWND dlg)
{
  RECT cr;
  int dx, dy;

  GetClientRect(dlg, &cr);
  dx = cr.right - guide_cw; dy = cr.bottom - guide_ch;
  MoveWindow(GetDlgItem(dlg, IDC_USERGUIDE_LIST), guide_rl.left, guide_rl.top, guide_rl.right - guide_rl.left, guide_rl.bottom - guide_rl.top + dy, TRUE);
  MoveWindow(GetDlgItem(dlg, IDC_USERGUIDE_TEXT), guide_rt.left, guide_rt.top, guide_rt.right - guide_rt.left + dx, guide_rt.bottom - guide_rt.top + dy, TRUE);
  MoveWindow(GetDlgItem(dlg, IDOK), guide_rb.left + dx, guide_rb.top + dy, guide_rb.right - guide_rb.left, guide_rb.bottom - guide_rb.top, TRUE);
  RedrawWindow(dlg, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);      /* (the page, and the text on it, drawn again for its new width: bits of the old one stayed) */
}

static void guide_remember(HWND dlg)
{
  RECT cr, wr;
  int ids[3] = { IDC_USERGUIDE_LIST, IDC_USERGUIDE_TEXT, IDOK }, i;
  RECT *dst[3] = { &guide_rl, &guide_rt, &guide_rb };

  GetClientRect(dlg, &cr);
  guide_cw = cr.right; guide_ch = cr.bottom;
  for (i = 0; i < 3; i++)
  {
    GetWindowRect(GetDlgItem(dlg, ids[i]), dst[i]);
    MapWindowPoints(NULL, dlg, (POINT *)dst[i], 2);
  }
  GetWindowRect(dlg, &wr);
  guide_minw = wr.right - wr.left; guide_minh = wr.bottom - wr.top;
}

/* A pop-up menu of a window of the program: drawn the same way everywhere (owner-drawn, in the Large interface or Modern-font look, when the
   main menus are), by the main window, which is the one that draws the owner-drawn items. Returns the command chosen, or 0. */
int gui_popup(HMENU menu, UINT flags, int x, int y)
{
  int cur = gui_cursor_show(), cmd;

  if (theme_menus_owner()) theme_ownerdraw_menu(menu);
  cmd = (int)TrackPopupMenu(menu, flags | TPM_RETURNCMD, x, y, 0, g_hwnd, NULL);
  theme_unconvert_menu(menu);                       /* (gives back what the owner-drawn items held) */
  gui_cursor_restore(cur);
  return cmd;
}

/* Right click in the text of the User Guide: Copy (the selected text) and Select All. */
static LRESULT CALLBACK guide_text_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
  (void)id; (void)ref;
  if (msg == WM_CONTEXTMENU)
  {
    HMENU menu = CreatePopupMenu();
    CHARRANGE cr;
    POINT mp;
    int cmd, cur = gui_cursor_show();

    SendMessage(h, EM_EXGETSEL, 0, (LPARAM)&cr);
    mp.x = (short)LOWORD(lp); mp.y = (short)HIWORD(lp);
    if (lp == (LPARAM)-1) GetCursorPos(&mp);                    /* (from the keyboard: where the pointer is) */
    AppendMenuA(menu, MF_STRING | (cr.cpMax > cr.cpMin ? 0 : MF_GRAYED), 2101, "&Copy");
    AppendMenuA(menu, MF_STRING, 2102, "Select &All");
    if (theme_menus_owner()) theme_ownerdraw_menu(menu);
    cmd = (int)gui_popup(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, mp.x, mp.y);       /* (the main window draws the owner-drawn items) */
    DestroyMenu(menu);
    gui_cursor_restore(cur);
    if (cmd == 2101) SendMessage(h, WM_COPY, 0, 0);
    else if (cmd == 2102) { cr.cpMin = 0; cr.cpMax = -1; SendMessage(h, EM_EXSETSEL, 0, (LPARAM)&cr); }
    return 0;
  }
  if (msg == WM_VSCROLL || msg == WM_MOUSEWHEEL || msg == WM_SIZE || msg == WM_KEYDOWN || msg == WM_MOUSEMOVE || msg == WM_LBUTTONUP || msg == WM_PAINT)
  {
    LRESULT r = DefSubclassProc(h, msg, wp, lp);        /* (the text has moved: the logo goes with it) */
    guide_logo_place(h);
    if (msg == WM_PAINT && guide_logo_on && guide_logo_wnd && IsWindowVisible(guide_logo_wnd))
      RedrawWindow(guide_logo_wnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);       /* (the text box may have painted its page over the room: the logo is put back at once) */
    return r;
  }
  if (msg == WM_NCDESTROY) RemoveWindowSubclass(h, guide_text_proc, 1);
  return DefSubclassProc(h, msg, wp, lp);
}

static INT_PTR CALLBACK user_guide_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
    case WM_GETMINMAXINFO:
    {
      MINMAXINFO *mm = (MINMAXINFO *)lp;
      if (guide_minw > 0) { mm->ptMinTrackSize.x = guide_minw; mm->ptMinTrackSize.y = guide_minh; }
      return TRUE;
    }

    case WM_SIZE:
      if (wp != SIZE_MINIMIZED && guide_cw > 0) guide_place(dlg);
      return 0;

    case WM_INITDIALOG:
      theme_apply_to_window(dlg);            /* (the Modern style changes the frame: the sizes are taken after it) */
      guide_remember(dlg);
      {
        static const GuiTip tips[] =
        {
          { IDC_USERGUIDE_LIST, NULL, "Pick a menu or window to read about it." },
          { IDC_USERGUIDE_TEXT, NULL, "What each menu item and each window does." },
          { IDOK, NULL, "Close this window." },
        };
        gui_dialog_tips(dlg, tips, (int)(sizeof(tips) / sizeof(tips[0])));
      }
      {
        int s;
        HWND list = GetDlgItem(dlg, IDC_USERGUIDE_LIST);

        SendDlgItemMessage(dlg, IDC_USERGUIDE_TEXT, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(12, 12));
        SetWindowSubclass(GetDlgItem(dlg, IDC_USERGUIDE_TEXT), guide_text_proc, 1, 0);

        guide_parse();
        for (s = 0; s < guide.count; s++)
        {
          char title[64];
          int n = 0;
          const char *src = guide.name[s];

          for (; src[n] && src[n] != '\r' && src[n] != '\n' && n < 60; n++) title[n] = src[n];
          title[n] = '\0';
          guide_title_case(title);                       /* "BIOS AND FIRMWARE FILES" -> "BIOS and Firmware Files" */
          SendMessageA(list, LB_ADDSTRING, 0, (LPARAM)title);
        }
        SendMessage(list, LB_SETCURSEL, 0, 0);
        guide_show(dlg);
      }
      return TRUE;

    case WM_DESTROY:
      free(guide.text);
      guide.text = NULL;
      guide.count = 0;
      guide_logo_on = 0;
      if (guide_logo_wnd)
      {
        HBITMAP img = (HBITMAP)SendMessage(guide_logo_wnd, STM_GETIMAGE, IMAGE_BITMAP, 0);
        if (img) DeleteObject(img);                      /* (the window is destroyed with the text box; the picture is ours) */
        guide_logo_wnd = NULL;
      }
      return FALSE;

    case WM_MEASUREITEM:
    {
      MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT *)lp;
      if (mi->CtlID == IDC_USERGUIDE_LIST) { mi->itemHeight = (UINT)IS(17); return TRUE; }
      return FALSE;
    }

    case WM_DRAWITEM:
    {
      /* The list of sections is drawn here so its selection has the theme's colors (the system one turned white in dark mode). */
      DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT *)lp;
      if (di->CtlID == IDC_USERGUIDE_LIST && di->itemID != (UINT)-1)
      {
        int dark = theme_is_dark(), sel = (di->itemState & ODS_SELECTED) != 0;
        COLORREF bg = sel ? theme_accent_bg() : (dark ? RGB(0x2D, 0x2D, 0x2D) : GetSysColor(COLOR_WINDOW));
        COLORREF fg = sel ? (dark ? RGB(255, 255, 255) : GetSysColor(COLOR_HIGHLIGHTTEXT)) : (dark ? RGB(0xF0, 0xF0, 0xF0) : GetSysColor(COLOR_WINDOWTEXT));
        HBRUSH bb = CreateSolidBrush(bg);
        RECT r = di->rcItem;
        char text[96] = "";

        FillRect(di->hDC, &r, bb);
        DeleteObject(bb);
        SendMessageA(di->hwndItem, LB_GETTEXT, di->itemID, (LPARAM)text);
        SetBkMode(di->hDC, TRANSPARENT);
        SetTextColor(di->hDC, fg);
        r.left += IS(4);
        DrawTextA(di->hDC, text, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_NOCLIP);
        return TRUE;
      }
      return FALSE;
    }

    case WM_CTLCOLORDLG:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORLISTBOX:
    {
      /* The clickable list of sections is #2D2D2D in dark mode. */
      HBRUSH br = (GetDlgCtrlID((HWND)lp) == IDC_USERGUIDE_LIST)
                ? theme_ctlcolor_custom((HDC)wp, RGB(0x2D, 0x2D, 0x2D), 2)
                : theme_ctlcolor((HDC)wp);
      if (br) return (LRESULT)br;
      break;
    }

    case WM_COMMAND:
      if (LOWORD(wp) == IDC_USERGUIDE_LIST && HIWORD(wp) == LBN_SELCHANGE)
      {
        guide_show(dlg);
        return TRUE;
      }
      if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)
      {
        EndDialog(dlg, LOWORD(wp));
        return TRUE;
      }
      return FALSE;

    case WM_CLOSE:
      EndDialog(dlg, IDCANCEL);
      return TRUE;
  }

  (void)lp;
  return FALSE;
}

void dlg_user_guide(HWND parent)
{
  LoadLibraryA("Msftedit.dll");                  /* the text box of the page is a rich text box */
  gui_dialog_box(gui.large_ui ? IDD_USERGUIDE_LARGE : IDD_USERGUIDE, parent, user_guide_proc);
}

static const char *system_name(void)
{
  if (pico_active()) return "32X";
  if (system_hw == SYSTEM_MCD) return "Sega CD / Mega CD";
  if (system_hw == SYSTEM_PICO) return "Sega Pico";
  if ((system_hw & SYSTEM_PBC) == SYSTEM_MD) return "Genesis / Mega Drive";
  if (system_hw == SYSTEM_GG || system_hw == SYSTEM_GGMS) return "Game Gear";
  if (system_hw == SYSTEM_SMS || system_hw == SYSTEM_SMS2) return "Master System";
  if (system_hw == SYSTEM_SG || system_hw == SYSTEM_SGII || system_hw == SYSTEM_SGII_RAM_EXT) return "SG-1000";
  return "Unknown";
}

/* Text from the game's header is Shift-JIS (Japanese titles); file names are in the system code page. ROM Information keeps
   everything as UTF-8 so any title shows (and copies) correctly. */
static void to_utf8(UINT codepage, const char *in, char *out, int out_len)
{
  WCHAR w[600];
  int n = MultiByteToWideChar(codepage, 0, in, -1, w, 600);

  if (n <= 0 || !WideCharToMultiByte(CP_UTF8, 0, w, -1, out, out_len, NULL, NULL)) lstrcpynA(out, in, out_len);
}

static void copy_text_to_clipboard(HWND owner, const char *text)    /* text is UTF-8 */
{
  int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
  HGLOBAL h;

  if (n <= 0 || !OpenClipboard(owner)) return;
  EmptyClipboard();
  h = GlobalAlloc(GMEM_MOVEABLE, (size_t)n * sizeof(WCHAR));
  if (h)
  {
    WCHAR *p = (WCHAR *)GlobalLock(h);
    if (p) { MultiByteToWideChar(CP_UTF8, 0, text, -1, p, n); GlobalUnlock(h); SetClipboardData(CF_UNICODETEXT, h); }
    else GlobalFree(h);
  }
  CloseClipboard();
}

/* The full text of every ROM Information row, kept apart from the list: the list view cuts a long
   value (a long file path) short when it is read back, and copying must give the whole text. */
static char **ri_name, **ri_value;
static int    ri_count;

static void rominfo_free(void)
{
  int i;
  for (i = 0; i < ri_count; i++) { free(ri_name[i]); free(ri_value[i]); }
  free(ri_name); free(ri_value);
  ri_name = ri_value = NULL;
  ri_count = 0;
}

static void rominfo_add(const char *name, const char *value)
{
  char **n = (char **)realloc(ri_name, (size_t)(ri_count + 1) * sizeof(char *));
  char **v = (char **)realloc(ri_value, (size_t)(ri_count + 1) * sizeof(char *));
  if (n) ri_name = n;
  if (v) ri_value = v;
  if (!n || !v) return;
  ri_name[ri_count] = _strdup(name);
  ri_value[ri_count] = _strdup(value);
  ri_count++;
}

/* The whole table, one "Field<TAB>Value" line per row (just the field for a section title); the caller frees it. */
static char *rominfo_all_text(void)
{
  size_t need = 16;
  char *all, *p;
  int i;

  for (i = 0; i < ri_count; i++) need += strlen(ri_name[i]) + strlen(ri_value[i]) + 4;
  all = (char *)malloc(need);
  if (!all) return NULL;
  p = all;
  for (i = 0; i < ri_count; i++)
  {
    p += sprintf(p, ri_value[i][0] ? "%s\t%s\r\n" : "%s\r\n", ri_name[i], ri_value[i]);
  }
  *p = '\0';
  return all;
}

/* Right-clicking the title row of a list offers Size All Columns to Fit: every column gets the width
   its contents (or its title) need. */
static LRESULT CALLBACK header_menu_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
  if (msg == WM_CONTEXTMENU)
  {
    HWND list = (HWND)ref;
    HMENU menu = CreatePopupMenu();
    POINT pt;
    int cmd;

    pt.x = (short)LOWORD(lp); pt.y = (short)HIWORD(lp);
    if (lp == (LPARAM)-1) GetCursorPos(&pt);
    AppendMenuA(menu, MF_STRING, 2100, "Size All Columns to Fit");
    if (theme_menus_owner()) theme_ownerdraw_menu(menu);
    cmd = (int)gui_popup(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y);
    DestroyMenu(menu);
    if (cmd == 2100)
    {
      int k, n = (int)SendMessage(hwnd, HDM_GETITEMCOUNT, 0, 0);

      for (k = 0; k < n; k++)
      {
        int w1, w2;
        ListView_SetColumnWidth(list, k, LVSCW_AUTOSIZE);
        w1 = ListView_GetColumnWidth(list, k);
        ListView_SetColumnWidth(list, k, LVSCW_AUTOSIZE_USEHEADER);
        w2 = ListView_GetColumnWidth(list, k);
        ListView_SetColumnWidth(list, k, (w1 > w2 ? w1 : w2) + 8);
      }
    }
    return 0;
  }
  if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, header_menu_proc, id);
  return DefSubclassProc(hwnd, msg, wp, lp);
}

void gui_list_header_menu(HWND list)
{
  SetWindowSubclass(ListView_GetHeader(list), header_menu_proc, 1, (DWORD_PTR)list);
}

static INT_PTR CALLBACK rominfo_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
    case WM_CONTEXTMENU:
    {
      HWND list = GetDlgItem(dlg, IDC_ROMINFO_TEXT);
      int sel = ListView_GetNextItem(list, -1, LVNI_SELECTED);

      if ((HWND)wp == list)
      {
        POINT pt;
        HMENU menu;
        int cmd;

        pt.x = (short)LOWORD(lp);
        pt.y = (short)HIWORD(lp);
        if (pt.x == -1 && pt.y == -1) { RECT r; GetWindowRect(list, &r); pt.x = r.left + 20; pt.y = r.top + 20; }
        else
        {
          LVHITTESTINFO hit;
          POINT cp = pt;
          ScreenToClient(list, &cp);
          memset(&hit, 0, sizeof(hit));
          hit.pt = cp;
          if (ListView_HitTest(list, &hit) >= 0) { sel = hit.iItem; ListView_SetItemState(list, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED); }
        }
        menu = CreatePopupMenu();
        AppendMenuA(menu, MF_STRING | (sel >= 0 ? 0 : (UINT)MF_GRAYED), 1, "Copy");
        cmd = gui_popup(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y);
        DestroyMenu(menu);
        if (cmd == 1 && sel >= 0)
        {
          /* Just the value, without the field name (a section title copies its own text). */
          if (sel < ri_count) copy_text_to_clipboard(dlg, ri_value[sel][0] ? ri_value[sel] : ri_name[sel]);
        }
        return TRUE;
      }
      return FALSE;
    }

    case WM_TIMER:
      if (wp == 1)
      {
        KillTimer(dlg, 1);
        SetDlgItemTextA(dlg, IDC_ROMINFO_COPY, "Copy");
      }
      return TRUE;

    case WM_INITDIALOG:
      theme_apply_to_window(dlg);
      rominfo_free();
      {
        static const GuiTip tips[] =
        {
          { IDC_ROMINFO_TEXT, NULL, "Details of the running game. Right-click a row to copy its value." },
          { IDC_ROMINFO_COPY, NULL, "Copy the whole table to the clipboard." },
          { IDOK, NULL, "Close this window." },
        };
        gui_dialog_tips(dlg, tips, (int)(sizeof(tips) / sizeof(tips[0])));
      }
      {
        char text[4096];
        char domestic[200], international[200], product[14], romtype_str[4],
             country[18], copyright[100];
        char fname8[GUI_PATH_LEN * 3], path8[GUI_PATH_LEN * 3];
        uint32 rom_size;

        /* Copied out and trimmed -- the core's own fields are fixed-size
           and space-padded, not necessarily null-terminated at the point
           the real text ends. */
        lstrcpynA(domestic, rominfo.domestic, sizeof(domestic));
        lstrcpynA(international, rominfo.international, sizeof(international));
        lstrcpynA(product, rominfo.product, sizeof(product));
        lstrcpynA(romtype_str, rominfo.ROMType, sizeof(romtype_str));
        lstrcpynA(country, rominfo.country, sizeof(country));
        lstrcpynA(copyright, rominfo.copyright, sizeof(copyright));
        { char t[100]; lstrcpynA(t, domestic, 50); to_utf8(932, t, domestic, sizeof(domestic)); }
        { char t[100]; lstrcpynA(t, international, 50); to_utf8(932, t, international, sizeof(international)); }
        { char t[100]; lstrcpynA(t, copyright, 18); to_utf8(932, t, copyright, sizeof(copyright)); }
        to_utf8(CP_ACP, emu_rom_filename(), fname8, sizeof(fname8));

        rom_size = rominfo.romend - rominfo.romstart + 1;

        {
          char genre[40] = "", sizebuf[40] = "", console_hdr[20], devs[160] = "", ramtxt[40], crctxt[16], typetxt[32];
          static const char *const dev_names[15] = { "3-Button Pad", "6-Button Pad", "Keyboard", "Printer", "Control Ball",
            "Floppy Drive", "Activator", "Team Player", "MS Joypad", "RS232C Serial", "Tablet", "Paddle", "CD-ROM", "Mega Mouse", "Menacer" };
          int d;
          WIN32_FILE_ATTRIBUTE_DATA fa;
          const char *path = emu_rom_path();

          if (path[0])
          {
            browser_genre_for(path, genre, sizeof(genre));
            if (GetFileAttributesExA(path, GetFileExInfoStandard, &fa))
              wsprintfA(sizebuf, "%lu KB", (unsigned long)((fa.nFileSizeLow + 1023) / 1024));
          }

          lstrcpynA(console_hdr, rominfo.consoletype, sizeof(console_hdr));
          for (d = lstrlenA(console_hdr); d > 0 && console_hdr[d - 1] == ' '; d--) console_hdr[d - 1] = '\0';
          for (d = 0; d < 15; d++)
            if (rominfo.peripherals & (1u << d))
            {
              if (devs[0]) lstrcatA(devs, ", ");
              lstrcatA(devs, dev_names[d]);
            }
          if (!strncmp(rominfo.ROMType, "GM", 2))      lstrcpyA(typetxt, "Game");
          else if (!strncmp(rominfo.ROMType, "AL", 2)) lstrcpyA(typetxt, "Educational Program");
          else if (!strncmp(rominfo.ROMType, "BR", 2)) lstrcpyA(typetxt, "Boot ROM");
          else typetxt[0] = '\0';
          if (sram.detected && sram.end >= sram.start)
            wsprintfA(ramtxt, "Yes (%lu KB)", (unsigned long)((sram.end - sram.start + 1 + 1023) / 1024));
          else lstrcpyA(ramtxt, "No");
          wsprintfA(crctxt, "%08lX", emu_rom_crc32());

          to_utf8(CP_ACP, path, path8, sizeof(path8));
          snprintf(text, sizeof(text),      /* not wsprintf: that stops at 1024 bytes, and a long path would cut the last rows */
            "GAME\r\n"
            "Name:\t%s\r\n"
            "Console:\t%s\r\n"
            "Genre:\t%s\r\n"
            "Favorite:\t%s\r\n"
            "File Size:\t%s\r\n"
            "CRC32:\t%s\r\n"
            "Location:\t%s\r\n"
            "\r\n"
            "CARTRIDGE HEADER\r\n"
            "Console Name:\t%s\r\n"
            "Type:\t%s\r\n"
            "Domestic Title:\t%s\r\n"
            "International Title:\t%s\r\n"
            "Product Code:\t%s\r\n"
            "Region:\t%s\r\n"
            "Copyright:\t%s\r\n"
            "Controllers:\t%s\r\n"
            "Save RAM:\t%s\r\n"
            "\r\n"
            "ROM\r\n"
            "ROM Size:\t%lu KB\r\n"
            "Header Checksum:\t%04X\r\n"
            "Calculated Checksum:\t%04X %s\r\n",
            fname8[0] ? fname8 : "(unknown)",
            system_name(),
            genre[0] ? genre : "(unknown)",
            path[0] && config_fav_has(path) ? "Yes" : "No",
            sizebuf[0] ? sizebuf : "(unknown)",
            crctxt,
            path8[0] ? path8 : "(unknown)",
            console_hdr[0] ? console_hdr : "(none)",
            typetxt[0] ? typetxt : "(none)",
            domestic[0] ? domestic : "(none)",
            international[0] ? international : "(none)",
            product[0] ? product : "(none)",
            country[0] ? country : "(none)",
            copyright[0] ? copyright : "(none)",
            devs[0] ? devs : "(none)",
            ramtxt,
            (unsigned long)(rom_size / 1024),
            rominfo.checksum,
            rominfo.realchecksum,
            (rominfo.checksum == rominfo.realchecksum) ? "(match)" : "(mismatch)");
        }

        {
          /* "Field<TAB>Value" lines into a two-column list, as in the Keyboard Shortcuts window;
             a line with no tab is a section title. */
          HWND list = GetDlgItem(dlg, IDC_ROMINFO_TEXT);
          LVCOLUMNA col;
          RECT rc;
          char *line;
          int row = 0;

          SendMessage(list, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT);
          GetClientRect(list, &rc);
          memset(&col, 0, sizeof(col));
          col.mask = LVCF_TEXT | LVCF_WIDTH;
          col.pszText = "Field";
          col.cx = (rc.right - rc.left) * 32 / 100;
          SendMessageA(list, LVM_INSERTCOLUMNA, 0, (LPARAM)&col);
          col.pszText = "Value";
          col.cx = (rc.right - rc.left) * 64 / 100;
          SendMessageA(list, LVM_INSERTCOLUMNA, 1, (LPARAM)&col);
          gui_list_header_menu(list);

          for (line = strtok(text, "\r\n"); line; line = strtok(NULL, "\r\n"))
          {
            char *tab = strchr(line, '\t');
            const char *name = line, *value = "";
            char title[64];

            if (tab) { *tab = '\0'; value = tab + 1; }
            else
            {
              /* A section title: capitals become "Game", "Cartridge Header", "Rom" ... */
              int k, up = 1;
              lstrcpynA(title, line, sizeof(title));
              for (k = 0; title[k]; k++)
              {
                if (title[k] == ' ') up = 1;
                else if (up) up = 0;
                else if (title[k] >= 'A' && title[k] <= 'Z') title[k] = (char)(title[k] + 32);
              }
              if (!lstrcmpA(title, "Rom")) lstrcpyA(title, "ROM");
              name = title;
            }
            {
              LVITEMW wi;
              WCHAR wname[200], wvalue[1600];
              MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 200);
              MultiByteToWideChar(CP_UTF8, 0, value, -1, wvalue, 1600);
              memset(&wi, 0, sizeof(wi));
              wi.mask = LVIF_TEXT;
              wi.iItem = row;
              wi.pszText = wname;
              SendMessageW(list, LVM_INSERTITEMW, 0, (LPARAM)&wi);
              wi.iSubItem = 1;
              wi.pszText = wvalue;
              SendMessageW(list, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&wi);
            }
            rominfo_add(name, value);
            row++;
          }

          theme_apply_to_window(dlg);
          {
            HWND header = ListView_GetHeader(list);
            if (header && theme_is_dark()) { SetWindowTheme(header, L"ItemsView", NULL); InvalidateRect(header, NULL, TRUE); }
          }
        }
      }
      return TRUE;

    case WM_CTLCOLORDLG:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    {
      HBRUSH br = theme_ctlcolor((HDC)wp);
      if (br) return (LRESULT)br;
      break;
    }

    case WM_COMMAND:
      if (LOWORD(wp) == IDC_ROMINFO_COPY)
      {
        HWND list = GetDlgItem(dlg, IDC_ROMINFO_TEXT);
        char *all = rominfo_all_text();

        (void)list;
        if (all)
        {
          copy_text_to_clipboard(dlg, all);
          free(all);
          SetDlgItemTextA(dlg, IDC_ROMINFO_COPY, "Copied");
          SetTimer(dlg, 1, 1500, NULL);
        }
        return TRUE;
      }
      if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)
      {
        KillTimer(dlg, 1);
        EndDialog(dlg, LOWORD(wp));
        return TRUE;
      }
      return FALSE;

    case WM_DESTROY:
      rominfo_free();
      return FALSE;

    case WM_CLOSE:
      KillTimer(dlg, 1);
      EndDialog(dlg, IDCANCEL);
      return TRUE;
  }

  return FALSE;
}

void dlg_rom_info(HWND parent)
{
  if (!emu_running && !emu_rom_filename()[0]) return;
  gui_dialog_box(gui.large_ui ? IDD_ROMINFO_LARGE : IDD_ROMINFO, parent, rominfo_proc);
}

/****************************************************************************
 * About
 ****************************************************************************/

/* STM_SETICON hands the control a handle without taking ownership of it
   (it just returns whatever icon was there before) -- kept here so
   WM_DESTROY below can DestroyIcon() it, since this LoadImageA() call
   (explicit size, no LR_SHARED) returns a private handle this dialog is
   responsible for freeing itself. */
static HICON about_big_icon;

static INT_PTR CALLBACK about_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
  (void)lp;

  switch (msg)
  {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:                       /* quick clicks arrive as double clicks */
      eggs_about_click(dlg, lp);
      return TRUE;

    case WM_INITDIALOG:
      theme_apply_to_window(dlg);
      {
        static const GuiTip tips[] = { { IDOK, NULL, "Close this window." } };
        gui_dialog_tips(dlg, tips, 1);
      }
      if (gui.large_ui)
      {
        about_big_icon = (HICON)LoadImageA(g_inst, MAKEINTRESOURCEA(IDI_APPICON),
                                            IMAGE_ICON, 64, 64, 0);
        if (about_big_icon) SendDlgItemMessage(dlg, IDC_ABOUT_ICON, STM_SETICON, (WPARAM)about_big_icon, 0);
      }
      SetDlgItemTextA(dlg, IDC_ABOUT_TEXT,
        "Genesis Plus EX v" APP_VERSION " (" APP_BITS ")\r\n"
        "\r\n"
        "A Windows emulator for Sega Mega Drive / Genesis, Master System, Game Gear, SG-1000, Mega CD, Pico and 32X, based on Genesis Plus GX and PicoDrive.\r\n"
        "\r\n"
        CREDITS_TEXT
        "\r\n"
        "Distributed under the licenses of Genesis Plus GX and PicoDrive: the source must accompany modified redistributions, and it may not be sold or used commercially.");

      SendDlgItemMessage(dlg, IDC_ABOUT_LINK1, WM_SETTEXT, 0,
        (LPARAM)"<a href=\"https://github.com/hazem-abdelghani/genesis-plus-ex\">Genesis Plus EX</a>");

      /* Laid out from the text itself: the link sits two empty lines under the last paragraph, the Close button
         a little below it, and the window is only as tall as that. */
      {
        HWND st = GetDlgItem(dlg, IDC_ABOUT_TEXT), lk = GetDlgItem(dlg, IDC_ABOUT_LINK1), ok = GetDlgItem(dlg, IDOK);
        RECT tr, lr, br, calc, cr, wr;
        HDC dc = GetDC(st);
        HFONT of = (HFONT)SelectObject(dc, (HFONT)SendMessage(st, WM_GETFONT, 0, 0));
        TEXTMETRICA tm;
        char text[1024];
        int lh, th, link_y, btn_y, margin;

        GetWindowRect(st, &tr); MapWindowPoints(NULL, dlg, (POINT *)&tr, 2);
        GetWindowRect(lk, &lr); MapWindowPoints(NULL, dlg, (POINT *)&lr, 2);
        GetWindowRect(ok, &br); MapWindowPoints(NULL, dlg, (POINT *)&br, 2);
        GetClientRect(dlg, &cr);
        GetTextMetricsA(dc, &tm);
        lh = tm.tmHeight;
        GetWindowTextA(st, text, sizeof(text));
        calc.left = 0; calc.top = 0; calc.right = tr.right - tr.left; calc.bottom = 0;
        DrawTextA(dc, text, -1, &calc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, of);
        ReleaseDC(st, dc);

        th = calc.bottom + 2;
        link_y = tr.top + th + 2 * lh;
        btn_y = link_y + (lr.bottom - lr.top) + 2 * lh;
        margin = cr.bottom - br.bottom;               /* the room under the button, as in the template */
        SetWindowPos(st, NULL, 0, 0, tr.right - tr.left, th, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(lk, NULL, lr.left, link_y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(ok, NULL, br.left, btn_y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        GetWindowRect(dlg, &wr);
        cr.bottom = btn_y + (br.bottom - br.top) + margin;
        AdjustWindowRectEx(&cr, (DWORD)GetWindowLongPtr(dlg, GWL_STYLE), FALSE, (DWORD)GetWindowLongPtr(dlg, GWL_EXSTYLE));
        SetWindowPos(dlg, NULL, 0, 0, wr.right - wr.left, cr.bottom - cr.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
      }
      SetFocus(GetDlgItem(dlg, IDOK));          /* not the first link, which would show as selected */
      return FALSE;

    case WM_NOTIFY:
    {
      NMHDR *hdr = (NMHDR *)lp;
      if ((hdr->code == NM_CLICK || hdr->code == NM_RETURN) &&
          hdr->idFrom == IDC_ABOUT_LINK1)
      {
        NMLINK *link = (NMLINK *)lp;
        char url[256];

        WideCharToMultiByte(CP_ACP, 0, link->item.szUrl, -1, url, sizeof(url), NULL, NULL);
        ShellExecuteA(dlg, "open", url, NULL, NULL, SW_SHOWNORMAL);
        return TRUE;
      }
      break;
    }

    case WM_CTLCOLORDLG:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    {
      HBRUSH br = theme_ctlcolor((HDC)wp);
      if (br) return (LRESULT)br;
      break;
    }

    case WM_COMMAND:
      if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)
      {
        EndDialog(dlg, LOWORD(wp));
        return TRUE;
      }
      return FALSE;

    case WM_CLOSE:
      EndDialog(dlg, IDCANCEL);
      return TRUE;

    case WM_DESTROY:
      if (about_big_icon) { DestroyIcon(about_big_icon); about_big_icon = NULL; }
      return TRUE;
  }

  return FALSE;
}

void dlg_about(HWND parent)
{
  gui_dialog_box(gui.large_ui ? IDD_ABOUT_LARGE : IDD_ABOUT, parent, about_proc);
}

/****************************************************************************
 * Mouse cursor while a dialog is open
 ****************************************************************************/

/* Makes sure the cursor is visible. Returns how many ShowCursor(TRUE) calls
   it took (always at least one), so gui_cursor_restore() can undo exactly
   that and leave the counter where fullscreen had put it. */
int gui_cursor_depth;                        /* windows that have asked for the pointer (the idle hiding waits while there are any) */

int gui_cursor_show(void)
{
  int calls = 1;

  gui_cursor_depth++;
  while (ShowCursor(TRUE) < 0 && calls < 64) calls++;
  return calls;
}

void gui_cursor_restore(int hidden_count)
{
  if (gui_cursor_depth > 0) gui_cursor_depth--;
  while (hidden_count-- > 0) ShowCursor(FALSE);
}

/* Windows opened by the program appear in the middle of the screen (the one the main window is on). */
void gui_center_window(HWND w)
{
  RECT r;
  MONITORINFO mi;
  HMONITOR mon = MonitorFromWindow(g_hwnd ? g_hwnd : w, MONITOR_DEFAULTTONEAREST);

  mi.cbSize = sizeof(mi);
  if (!GetWindowRect(w, &r) || !GetMonitorInfo(mon, &mi)) return;
  SetWindowPos(w, NULL, (mi.rcWork.left + mi.rcWork.right - (r.right - r.left)) / 2,
               (mi.rcWork.top + mi.rcWork.bottom - (r.bottom - r.top)) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* Question and message boxes (Yes / No, OK ...) open in the middle of the program's window, not of the screen. */
static HHOOK msgbox_hook;
static HWND msgbox_owner;                /* the window the box was opened from: it is centered over that (the Controls or Cheats window, say), not over the main window */

static LRESULT CALLBACK msgbox_center_proc(int code, WPARAM wp, LPARAM lp)
{
  HHOOK h = msgbox_hook;

  if (code == HCBT_ACTIVATE)
  {
    HWND box = (HWND)wp, ref = g_hwnd;
    HWND own = msgbox_owner ? GetAncestor(msgbox_owner, GA_ROOT) : NULL;
    RECT br, rr;
    char cls[16];

    GetClassNameA(box, cls, sizeof(cls));
    if (!lstrcmpA(cls, "#32770") && msgbox_hook)
    {
      if (own && own != g_hwnd && IsWindow(own) && IsWindowVisible(own) && !IsIconic(own)) ref = own;
      if (ref && IsWindowVisible(ref) && !IsIconic(ref) && GetWindowRect(box, &br) && GetWindowRect(ref, &rr))
      {
        int x = rr.left + ((rr.right - rr.left) - (br.right - br.left)) / 2;
        int y = rr.top + ((rr.bottom - rr.top) - (br.bottom - br.top)) / 2;
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfo(MonitorFromWindow(ref, MONITOR_DEFAULTTONEAREST), &mi))      /* kept on the screen */
        {
          if (x + (br.right - br.left) > mi.rcWork.right) x = mi.rcWork.right - (br.right - br.left);
          if (y + (br.bottom - br.top) > mi.rcWork.bottom) y = mi.rcWork.bottom - (br.bottom - br.top);
          if (x < mi.rcWork.left) x = mi.rcWork.left;
          if (y < mi.rcWork.top) y = mi.rcWork.top;
        }
        SetWindowPos(box, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
      }
      else gui_center_window(box);
      UnhookWindowsHookEx(msgbox_hook);
      msgbox_hook = NULL;
    }
  }
  return CallNextHookEx(h, code, wp, lp);
}

/* The window a question or a new window should open over: the one given, unless it is the main window while one of the program's
   own windows (Settings, Cheats ...) is open on top of it -- those open windows and questions on the main window's behalf, but the
   person is looking at the open window. */
static HWND gui_visible_top(HWND w)
{
  if (w && w == g_hwnd)
  {
    HWND lap = GetLastActivePopup(w);
    if (lap && lap != w && IsWindowVisible(lap) && !IsIconic(lap)) return lap;
  }
  return w;
}

#undef MessageBoxA
int gui_msgbox(HWND owner, const char *text, const char *caption, UINT flags)
{
  int r;
  HHOOK saved = msgbox_hook;
  HWND saved_owner = msgbox_owner;

  msgbox_owner = gui_visible_top(owner);
  msgbox_hook = SetWindowsHookExA(WH_CBT, msgbox_center_proc, NULL, GetCurrentThreadId());
  {
    /* Owned by the window that is on top of the program (Settings, Controls ... when one is open), so it always opens in front of
       it, never behind; and brought to the front of everything. */
    HWND top = owner, lap;
    if (top == g_hwnd) while ((lap = GetLastActivePopup(top)) != NULL && lap != top && IsWindowVisible(lap) && !IsIconic(lap)) top = lap;
    r = MessageBoxA(top, text, caption, flags | MB_SETFOREGROUND);
  }
  msgbox_owner = saved_owner;
  if (msgbox_hook) { UnhookWindowsHookEx(msgbox_hook); msgbox_hook = NULL; }
  msgbox_hook = saved;
  return r;
}

#define MessageBoxA gui_msgbox

/* The program's icon in the title bar of a window (Settings, Controls, Cheats ...); small dialogs (a name to type, a question) have none. */
void gui_window_icon(HWND w)
{
  static HICON small_icon, big_icon;
  LONG_PTR ex = GetWindowLongPtr(w, GWL_EXSTYLE);

  if (!small_icon) small_icon = (HICON)LoadImageA(g_inst, MAKEINTRESOURCEA(IDI_APPICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
  if (!big_icon) big_icon = LoadIconA(g_inst, MAKEINTRESOURCEA(IDI_APPICON));
  if (ex & WS_EX_DLGMODALFRAME)       /* a modal-frame window shows no icon, whatever it is given */
  {
    SetWindowLongPtr(w, GWL_EXSTYLE, ex & ~(LONG_PTR)WS_EX_DLGMODALFRAME);
    SetWindowPos(w, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
  }
  SendMessage(w, WM_SETICON, ICON_SMALL, (LPARAM)small_icon);
  SendMessage(w, WM_SETICON, ICON_BIG, (LPARAM)big_icon);
}

static int center_icon;               /* set by gui_dialog_box: the window about to open is one of the program's windows, not a small dialog */
static int center_icon_now;           /* the same, for the window gui_modal is opening right now */
static HHOOK center_hook;
static HWND center_parent;           /* the window a modal window was opened from, when that is not the main window */

static LRESULT CALLBACK center_proc(int code, WPARAM wp, LPARAM lp)
{
  HHOOK h = center_hook;

  if (code == HCBT_ACTIVATE)
  {
    char cls[16];
    GetClassNameA((HWND)wp, cls, sizeof(cls));
    if (!lstrcmpA(cls, "#32770") && center_hook)
    {
      HWND win = (HWND)wp, par = center_parent;
      RECT pr, wr;
      if (par && par != g_hwnd && IsWindow(par) && IsWindowVisible(par) && !IsIconic(par) && GetWindowRect(par, &pr) && GetWindowRect(win, &wr))
      {
        /* opened from another window (Controls, Cheats ...): in the middle of that one, kept on the screen */
        MONITORINFO mi;
        int x = pr.left + ((pr.right - pr.left) - (wr.right - wr.left)) / 2;
        int y = pr.top + ((pr.bottom - pr.top) - (wr.bottom - wr.top)) / 2;
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfo(MonitorFromWindow(par, MONITOR_DEFAULTTONEAREST), &mi))
        {
          if (x + (wr.right - wr.left) > mi.rcWork.right) x = mi.rcWork.right - (wr.right - wr.left);
          if (y + (wr.bottom - wr.top) > mi.rcWork.bottom) y = mi.rcWork.bottom - (wr.bottom - wr.top);
          if (x < mi.rcWork.left) x = mi.rcWork.left;
          if (y < mi.rcWork.top) y = mi.rcWork.top;
        }
        SetWindowPos(win, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
      }
      else gui_center_window(win);
      if (center_icon_now) gui_window_icon(win);
      UnhookWindowsHookEx(center_hook);
      center_hook = NULL;
    }
  }
  return CallNextHookEx(h, code, wp, lp);
}

static int combo_anim_depth, combo_anim_was;

/* For the crash handler: the drop-down animation is a Windows-wide setting that this program turns off while a window is open. */
void gui_restore_combo_animation(void)
{
  if (combo_anim_was) SystemParametersInfoA(SPI_SETCOMBOBOXANIMATION, 0, (PVOID)TRUE, 0);
  combo_anim_was = 0;
}

/* A modal window, centered; the game goes on behind it. */
INT_PTR gui_modal(int template_id, HWND parent, DLGPROC proc, LPARAM lp)
{
  int paced = 0;
  INT_PTR r;

  center_parent = gui_visible_top(parent);
  center_icon_now = center_icon;
  center_icon = 0;
  center_hook = SetWindowsHookExA(WH_CBT, center_proc, NULL, GetCurrentThreadId());
  if (emu_running) { gui_pace(1); paced = 1; }

  /* A drop-down list slides open with an animation; in this program's windows it opens at once instead.
     The Windows setting is put back when the window closes (windows opened from a window share one change). */
  if (combo_anim_depth++ == 0)
  {
    BOOL on = FALSE;
    combo_anim_was = (SystemParametersInfoA(SPI_GETCOMBOBOXANIMATION, 0, &on, 0) && on);
    if (combo_anim_was) SystemParametersInfoA(SPI_SETCOMBOBOXANIMATION, 0, (PVOID)FALSE, 0);
  }
  r = DialogBoxParamA(g_inst, MAKEINTRESOURCEA(template_id), parent, proc, lp);
  if (--combo_anim_depth == 0 && combo_anim_was)
  {
    SystemParametersInfoA(SPI_SETCOMBOBOXANIMATION, 0, (PVOID)TRUE, 0);
    combo_anim_was = 0;
  }
  if (paced) gui_pace(0);
  if (center_hook) { UnhookWindowsHookEx(center_hook); center_hook = NULL; }
  return r;
}

INT_PTR gui_dialog_box(int template_id, HWND parent, DLGPROC proc)
{
  INT_PTR r;
  int c = gui_cursor_show();

  cheat_search_enable(0);
  center_icon = 1;
  r = gui_modal(template_id, parent, proc, 0);
  cheat_search_enable(1);
  gui_cursor_restore(c);
  return r;
}

/****************************************************************************
 * Open-file picker
 *
 * Uses the Vista+ "common item" dialog instead of GetOpenFileName. The old
 * API lets Windows restore its own remembered state -- last folder, window
 * size / maximized, even a previous search -- and only treats the start
 * folder as a hint. Here the start folder is forced (SetFolder) and the
 * remembered state is cleared every time, per picker (each has its own
 * GUID so they can't affect one another).
 *
 * `filter` uses the same "Name\0pattern\0Name\0pattern\0\0" layout as
 * OPENFILENAME. Returns 1 and fills `out` with the chosen path, else 0.
 ****************************************************************************/

static const GUID pick_guids[GUI_PICK_COUNT] =
{
  { 0x6f1a5c10, 0x3b7e, 0x4c1a, { 0x9a, 0x21, 0x5d, 0x0e, 0x7b, 0x11, 0xa0, 0x01 } },  /* Open ROM     */
  { 0x6f1a5c10, 0x3b7e, 0x4c1a, { 0x9a, 0x21, 0x5d, 0x0e, 0x7b, 0x11, 0xa0, 0x02 } },  /* Cheat file   */
  { 0x6f1a5c10, 0x3b7e, 0x4c1a, { 0x9a, 0x21, 0x5d, 0x0e, 0x7b, 0x11, 0xa0, 0x03 } },  /* Cover        */
  { 0x6f1a5c10, 0x3b7e, 0x4c1a, { 0x9a, 0x21, 0x5d, 0x0e, 0x7b, 0x11, 0xa0, 0x04 } },  /* Shader preset */
  { 0x6f1a5c10, 0x3b7e, 0x4c1a, { 0x9a, 0x21, 0x5d, 0x0e, 0x7b, 0x11, 0xa0, 0x05 } }   /* State file   */
};

static int pick_file_legacy(HWND owner, const char *title, const char *filter,
                            const char *start_dir, char *out, int out_len)
{
  OPENFILENAMEA ofn;

  out[0] = '\0';
  ZeroMemory(&ofn, sizeof(ofn));
  ofn.lStructSize     = sizeof(ofn);
  ofn.hwndOwner       = owner;
  ofn.lpstrFile       = out;
  ofn.nMaxFile        = (DWORD)out_len;
  ofn.lpstrTitle      = title;
  ofn.lpstrFilter     = filter;
  ofn.nFilterIndex    = 1;
  ofn.lpstrInitialDir = (start_dir && start_dir[0]) ? start_dir : NULL;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY |
              OFN_EXPLORER | OFN_NOCHANGEDIR;

  return GetOpenFileNameA(&ofn) ? 1 : 0;
}

static int pick_file_impl(HWND owner, int which, const char *title, const char *filter,
                          const char *start_dir, char *out, int out_len)
{
  static wchar_t names[8][128], pats[8][256];
  COMDLG_FILTERSPEC specs[8];
  wchar_t wtitle[128], wpath[GUI_PATH_LEN];
  IFileOpenDialog *dlg = NULL;
  HRESULT hr, hr_init;
  const char *p;
  FILEOPENDIALOGOPTIONS opts = 0;
  int n = 0, ok = 0;

  if (!out || out_len <= 0) return 0;
  out[0] = '\0';
  if (which < 0 || which >= GUI_PICK_COUNT) which = 0;

  hr_init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                        &IID_IFileOpenDialog, (void **)&dlg);
  if (FAILED(hr) || !dlg)
  {
    if (SUCCEEDED(hr_init)) CoUninitialize();
    return pick_file_legacy(owner, title, filter, start_dir, out, out_len);
  }

  IFileOpenDialog_SetClientGuid(dlg, &pick_guids[which]);
  IFileOpenDialog_ClearClientData(dlg);

  MultiByteToWideChar(CP_ACP, 0, title, -1, wtitle, 128);
  IFileOpenDialog_SetTitle(dlg, wtitle);

  for (p = filter; p && *p && n < 8; n++)
  {
    MultiByteToWideChar(CP_ACP, 0, p, -1, names[n], 128);
    p += lstrlenA(p) + 1;
    MultiByteToWideChar(CP_ACP, 0, p, -1, pats[n], 256);
    p += lstrlenA(p) + 1;
    specs[n].pszName = names[n];
    specs[n].pszSpec = pats[n];
  }
  if (n > 0)
  {
    IFileOpenDialog_SetFileTypes(dlg, (UINT)n, specs);
    IFileOpenDialog_SetFileTypeIndex(dlg, 1);
  }

  IFileOpenDialog_GetOptions(dlg, &opts);
  IFileOpenDialog_SetOptions(dlg, opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST |
                                  FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);

  if (start_dir && start_dir[0])
  {
    IShellItem *folder = NULL;

    MultiByteToWideChar(CP_ACP, 0, start_dir, -1, wpath, GUI_PATH_LEN);
    if (SUCCEEDED(SHCreateItemFromParsingName(wpath, NULL, &IID_IShellItem, (void **)&folder)) && folder)
    {
      IFileOpenDialog_SetFolder(dlg, folder);
      IShellItem_Release(folder);
    }
  }

  hr = IFileOpenDialog_Show(dlg, owner);
  if (SUCCEEDED(hr))
  {
    IShellItem *item = NULL;

    if (SUCCEEDED(IFileOpenDialog_GetResult(dlg, &item)) && item)
    {
      PWSTR w = NULL;

      if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &w)) && w)
      {
        ok = WideCharToMultiByte(CP_ACP, 0, w, -1, out, out_len, NULL, NULL) > 0;
        CoTaskMemFree(w);
      }
      IShellItem_Release(item);
    }
  }

  IFileOpenDialog_Release(dlg);
  if (SUCCEEDED(hr_init)) CoUninitialize();

  if (!ok) out[0] = '\0';
  return ok;
}

/* A "Save as" window (the classic one: it only has to hand back a path). */
int gui_save_file(HWND owner, const char *title, const char *filter, const char *defext,
                  const char *suggest, char *out, int out_len)
{
  OPENFILENAMEA ofn;
  int c = gui_cursor_show(), ok;

  lstrcpynA(out, suggest ? suggest : "", out_len);
  ZeroMemory(&ofn, sizeof(ofn));
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = owner;
  ofn.lpstrFile = out;
  ofn.nMaxFile = (DWORD)out_len;
  ofn.lpstrTitle = title;
  ofn.lpstrFilter = filter;
  ofn.lpstrDefExt = defext;
  ofn.nFilterIndex = 1;
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER | OFN_NOCHANGEDIR;
  ok = GetSaveFileNameA(&ofn) ? 1 : 0;
  gui_cursor_restore(c);
  if (!ok) out[0] = '\0';
  return ok;
}

int gui_pick_file(HWND owner, int which, const char *title, const char *filter,
                  const char *start_dir, char *out, int out_len)
{
  int c = gui_cursor_show();
  int r = pick_file_impl(owner, which, title, filter, start_dir, out, out_len);

  gui_cursor_restore(c);
  return r;
}

/****************************************************************************
 * Netplay (LAN)
 ****************************************************************************/

static np_found np_hosts[8];
static int      np_host_count;

static void np_dlg_status(HWND dlg, const char *text)
{
  SetDlgItemTextA(dlg, IDC_NP_STATUS, text);
}

/* In dark mode the system draws a group box's title in black whatever color
   the dialog asks for, which vanishes against the dark background. So the
   group boxes of this dialog paint themselves: a thin gray frame and a light
   title. (Only the frame and title are drawn; everything inside is left to the
   controls on top.) */
static LRESULT CALLBACK np_groupbox_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                         UINT_PTR id, DWORD_PTR ref)
{
  (void)ref;

  if (msg == WM_ERASEBKGND) return 1;   /* the dialog has already painted the background */

  if (msg == WM_PAINT)
  {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    char text[128];
    HFONT font = (HFONT)SendMessage(hwnd, WM_GETFONT, 0, 0);
    HGDIOBJ old_font = SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
    SIZE sz;
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(100, 100, 100));
    HGDIOBJ old_pen = SelectObject(dc, pen);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    HBRUSH bg = CreateSolidBrush(THEME_DARK_BG);
    RECT patch;

    GetClientRect(hwnd, &rc);
    GetWindowTextA(hwnd, text, sizeof(text));
    GetTextExtentPoint32A(dc, text, lstrlenA(text), &sz);

    Rectangle(dc, rc.left, rc.top + sz.cy / 2, rc.right, rc.bottom);

    /* Clear the stretch of frame behind the title, then write the title. */
    patch.left = rc.left + 7; patch.right = rc.left + 13 + sz.cx;
    patch.top = rc.top;       patch.bottom = rc.top + sz.cy;
    FillRect(dc, &patch, bg);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, THEME_DARK_TEXT);
    TextOutA(dc, rc.left + 10, rc.top, text, lstrlenA(text));

    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_font);
    DeleteObject(pen);
    DeleteObject(bg);
    EndPaint(hwnd, &ps);
    return 0;
  }

  if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, np_groupbox_proc, id);

  return DefSubclassProc(hwnd, msg, wp, lp);
}

static BOOL CALLBACK np_untheme_groupbox(HWND child, LPARAM lp)
{
  char cls[16];

  (void)lp;
  if (GetClassNameA(child, cls, sizeof(cls)) && !lstrcmpiA(cls, "Button") &&
      (GetWindowLongPtr(child, GWL_STYLE) & BS_TYPEMASK) == BS_GROUPBOX)
  {
    SetWindowSubclass(child, np_groupbox_proc, 1, 0);
    InvalidateRect(child, NULL, TRUE);
  }
  return TRUE;
}

#define NP_JOIN_TIMER 77
static int  np_joining;              /* a Join is connecting in the background */
static char np_join_addr[64];

/* While a Join connects, the other buttons that start something are off. */
static void np_join_busy(HWND dlg, int on)
{
  static const int ids[] = { IDC_NP_JOIN, IDC_NP_HOST, IDC_NP_FIND };
  int i;
  np_joining = on;
  for (i = 0; i < 3; i++) EnableWindow(GetDlgItem(dlg, ids[i]), !on);
}

static INT_PTR CALLBACK netplay_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
    case WM_INITDIALOG:
    {
      char buf[160];
      HWND combo = GetDlgItem(dlg, IDC_NP_DELAY);
      int i;

      np_joining = 0;
      theme_apply_to_window(dlg);
      if (theme_is_dark()) EnumChildWindows(dlg, np_untheme_groupbox, 0);

      wsprintfA(buf, "%d", gui.np_port);
      SetDlgItemTextA(dlg, IDC_NP_HOST_PORT, buf);
      SetDlgItemTextA(dlg, IDC_NP_PORT, buf);
      SetDlgItemTextA(dlg, IDC_NP_ADDR, gui.np_addr);

      for (i = 1; i <= 4; i++)
      {
        wsprintfA(buf, "%d frame%s", i, i == 1 ? "" : "s");
        SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)buf);
      }
      SendMessage(combo, CB_SETCURSEL, (WPARAM)(gui.np_delay - 1), 0);

      CheckDlgButton(dlg, IDC_NP_UPNP, gui.np_upnp ? BST_CHECKED : BST_UNCHECKED);
      wsprintfA(buf, "Your address on this network: %s", netplay_local_addresses()[0] ? netplay_local_addresses() : "(unknown)");
      SetDlgItemTextA(dlg, IDC_NP_MYADDR, buf);
      np_dlg_status(dlg, "Both players must load the same ROM and use the same Genesis Plus EX.exe.");
      {
        static const GuiTip tips[] =
        {
          { IDC_NP_HOST_PORT, NULL, "The port this game listens on. The other player joins this port." },
          { IDC_NP_DELAY, "Input Delay:", "Frames of input delay: more is smoother on a slow network, fewer feels more immediate." },
          { IDC_NP_HOST_CODE, "Session Code (Optional):", "A code the other player has to type to join. Leave it empty to let anyone join." },
          { IDC_NP_UPNP, NULL, "Ask your router (UPnP) to forward the port while you host, so friends outside your home network can join. The port is closed again when you stop. Needs UPnP switched on in the router; if it is not, forward the port by hand." },
          { IDC_NP_HOST, NULL, "Wait for another player to join this game." },
          { IDC_NP_LIST, NULL, "Hosts found on your network. Pick one to fill in its address." },
          { IDC_NP_FIND, NULL, "Look for hosts on your local network." },
          { IDC_NP_ADDR, "Address:", "The address of the host to join." },
          { IDC_NP_PORT, NULL, "The port the host is listening on." },
          { IDC_NP_JOIN_CODE, "Code:", "The session code, if the host set one." },
          { IDC_NP_JOIN, NULL, "Join the host at this address." },
          { IDCANCEL, NULL, "Close this window." },
        };
        gui_dialog_tips(dlg, tips, (int)(sizeof(tips) / sizeof(tips[0])));
      }
      return TRUE;
    }

    case WM_MEASUREITEM:
    {
      MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT *)lp;
      if (mi->CtlID != IDC_NP_LIST) return FALSE;
      {
        HDC dc = GetDC(dlg);
        HFONT of = (HFONT)SelectObject(dc, (HFONT)SendMessage(dlg, WM_GETFONT, 0, 0));
        TEXTMETRICA tm;
        GetTextMetricsA(dc, &tm);
        SelectObject(dc, of);
        ReleaseDC(dlg, dc);
        mi->itemHeight = (UINT)(tm.tmHeight + 4);
      }
      return TRUE;
    }

    case WM_DRAWITEM:
    {
      /* The hosts found: a chosen row is the accent color in dark mode (a plain list box draws it white there). */
      const DRAWITEMSTRUCT *di = (const DRAWITEMSTRUCT *)lp;
      int dark = theme_is_dark(), sel;
      COLORREF bg, fg;
      HBRUSH b;
      char text[200];
      RECT r;

      if (di->CtlID != IDC_NP_LIST) return FALSE;
      if (di->itemID == (UINT)-1) return TRUE;
      sel = (di->itemState & ODS_SELECTED) != 0;
      if (dark) { bg = sel ? theme_accent_bg() : RGB(0x2D, 0x2D, 0x2D); fg = sel ? RGB(255, 255, 255) : THEME_DARK_TEXT; }
      else { bg = GetSysColor(sel ? COLOR_HIGHLIGHT : COLOR_WINDOW); fg = GetSysColor(sel ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT); }
      b = CreateSolidBrush(bg);
      FillRect(di->hDC, &di->rcItem, b);
      DeleteObject(b);
      text[0] = '\0';
      SendMessageA(di->hwndItem, LB_GETTEXT, di->itemID, (LPARAM)text);
      r = di->rcItem;
      r.left += 4;
      SetBkMode(di->hDC, TRANSPARENT);
      SetTextColor(di->hDC, fg);
      DrawTextA(di->hDC, text, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
      if (!dark && (di->itemState & ODS_FOCUS)) DrawFocusRect(di->hDC, &di->rcItem);
      return TRUE;
    }

    case WM_CTLCOLORDLG:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    {
      HBRUSH br;

      /* The list of hosts: #2D2D2D in dark mode (the same shade as the other lists). */
      if (GetDlgCtrlID((HWND)lp) == IDC_NP_LIST)
        br = theme_ctlcolor_custom((HDC)wp, RGB(0x2D, 0x2D, 0x2D), 2);
      else if (msg == WM_CTLCOLOREDIT)      /* the entry boxes stand out from the dialog, as in Cheats */
        br = theme_ctlcolor_custom((HDC)wp, RGB(0x38, 0x38, 0x38), 1);
      else
        br = theme_ctlcolor((HDC)wp);
      if (br) return (LRESULT)br;
      break;
    }

    case WM_COMMAND:
    {
      char err[300], code[64], addr[64], buf[32];
      int port, delay;

      switch (LOWORD(wp))
      {
        case IDC_NP_FIND:
        {
          int i;
          HWND list = GetDlgItem(dlg, IDC_NP_LIST);
          HCURSOR old = SetCursor(LoadCursorA(NULL, IDC_WAIT));

          np_dlg_status(dlg, "Looking for hosts on your network...");
          np_host_count = netplay_discover(np_hosts, 8);
          SetCursor(old);

          SendMessage(list, LB_RESETCONTENT, 0, 0);
          for (i = 0; i < np_host_count; i++)
          {
            char line[160];
            wsprintfA(line, "%s  -  %s  (%s)%s", np_hosts[i].name, np_hosts[i].game[0] ? np_hosts[i].game : "no game",
                      np_hosts[i].addr, np_hosts[i].needs_code ? "  [code needed]" : "");
            SendMessageA(list, LB_ADDSTRING, 0, (LPARAM)line);
          }
          if (np_host_count)
          {
            SendMessage(list, LB_SETCURSEL, 0, 0);
            SetDlgItemTextA(dlg, IDC_NP_ADDR, np_hosts[0].addr);
            wsprintfA(buf, "%u", (unsigned)np_hosts[0].port);
            SetDlgItemTextA(dlg, IDC_NP_PORT, buf);
            np_dlg_status(dlg, np_hosts[0].needs_code ? "Found a host. Enter its session code, then Join." : "Found a host. Press Join.");
          }
          else
          {
            np_dlg_status(dlg, "No hosts found. Type the host's address below, or ask them to Host first.");
          }
          return TRUE;
        }

        case IDC_NP_LIST:
          if (HIWORD(wp) == LBN_SELCHANGE)
          {
            int sel = (int)SendDlgItemMessage(dlg, IDC_NP_LIST, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < np_host_count)
            {
              SetDlgItemTextA(dlg, IDC_NP_ADDR, np_hosts[sel].addr);
              wsprintfA(buf, "%u", (unsigned)np_hosts[sel].port);
              SetDlgItemTextA(dlg, IDC_NP_PORT, buf);
            }
          }
          return TRUE;

        case IDC_NP_HOST:
          GetDlgItemTextA(dlg, IDC_NP_HOST_PORT, buf, sizeof(buf));
          port = atoi(buf);
          GetDlgItemTextA(dlg, IDC_NP_HOST_CODE, code, sizeof(code));
          delay = (int)SendDlgItemMessage(dlg, IDC_NP_DELAY, CB_GETCURSEL, 0, 0) + 1;
          if (delay < 1) delay = 2;
          gui.np_upnp = IsDlgButtonChecked(dlg, IDC_NP_UPNP) == BST_CHECKED;
          netplay_set_upnp(gui.np_upnp);

          if (!netplay_host(port, code, delay, err, sizeof(err)))
          {
            np_dlg_status(dlg, err);
            return TRUE;
          }
          gui.np_port = port;
          gui.np_delay = delay;
          config_save();
          gui_notify("Hosting: waiting for Player 2 (%s, port %d)", netplay_local_addresses()[0] ? netplay_local_addresses() : "this PC", port);
          EndDialog(dlg, IDOK);
          return TRUE;

        case IDC_NP_JOIN:
        {
          GetDlgItemTextA(dlg, IDC_NP_ADDR, addr, sizeof(addr));
          GetDlgItemTextA(dlg, IDC_NP_PORT, buf, sizeof(buf));
          port = atoi(buf);
          GetDlgItemTextA(dlg, IDC_NP_JOIN_CODE, code, sizeof(code));

          if (!netplay_join_start(addr, port, code, err, sizeof(err)))
          {
            np_dlg_status(dlg, err);
            return TRUE;
          }
          lstrcpynA(np_join_addr, addr, sizeof(np_join_addr));
          np_dlg_status(dlg, "Connecting... (Cancel stops it)");
          np_join_busy(dlg, 1);
          SetTimer(dlg, NP_JOIN_TIMER, 100, NULL);       /* the window stays alive while it connects */
          return TRUE;
        }

        case IDCANCEL:
          if (np_joining) { netplay_join_cancel(); KillTimer(dlg, NP_JOIN_TIMER); np_join_busy(dlg, 0); }
          EndDialog(dlg, IDCANCEL);
          return TRUE;
      }
      return FALSE;
    }

    case WM_TIMER:
      if (wp == NP_JOIN_TIMER && np_joining)
      {
        char err[300];
        int r = netplay_join_poll(err, sizeof(err));

        if (r == 0) return TRUE;
        KillTimer(dlg, NP_JOIN_TIMER);
        np_join_busy(dlg, 0);
        if (r < 0) { np_dlg_status(dlg, err); return TRUE; }
        lstrcpynA(gui.np_addr, np_join_addr, sizeof(gui.np_addr));
        config_save();
        EndDialog(dlg, IDOK);
      }
      return TRUE;

    case WM_CLOSE:
      if (np_joining) { netplay_join_cancel(); KillTimer(dlg, NP_JOIN_TIMER); np_join_busy(dlg, 0); }
      EndDialog(dlg, IDCANCEL);
      return TRUE;
  }

  return FALSE;
}

void dlg_netplay(HWND parent)
{
  gui_dialog_box(gui.large_ui ? IDD_NETPLAY_LARGE : IDD_NETPLAY, parent, netplay_proc);
}

/****************************************************************************
 * Manage Save States
 ****************************************************************************/

#define STATEMGR_SLOTS 10

static HBITMAP statemgr_bitmaps[STATEMGR_SLOTS];

static void statemgr_refresh_slot(HWND dlg, int slot)
{
  char state_p[GUI_PATH_LEN], thumb_p[GUI_PATH_LEN], label[96], stamp[32];
  HWND btn = GetDlgItem(dlg, IDC_STATEMGR_SLOT_BASE + slot);
  HWND lbl = GetDlgItem(dlg, IDC_STATEMGR_LABEL_BASE + slot);
  int exists;

  state_path(slot, state_p, sizeof(state_p));
  exists = GetFileAttributesA(state_p) != INVALID_FILE_ATTRIBUTES;

  if (statemgr_bitmaps[slot])
  {
    SendMessage(btn, BM_SETIMAGE, IMAGE_BITMAP, (LPARAM)NULL);
    DeleteObject(statemgr_bitmaps[slot]);
    statemgr_bitmaps[slot] = NULL;
  }

  if (exists)
  {
    thumb_path(slot, thumb_p, sizeof(thumb_p));
    statemgr_bitmaps[slot] = video_load_thumbnail(thumb_p);

    /* The saved picture is 128 x 96: stretch it to fill the (bigger) slot button. */
    if (statemgr_bitmaps[slot])
    {
      RECT br;
      GetClientRect(btn, &br);
      br.right -= 8; br.bottom -= 8;   /* the button's own border */
      if (br.right > 32 && br.bottom > 24)
      {
        HDC sdc = CreateCompatibleDC(NULL), ddc = CreateCompatibleDC(NULL), scr = GetDC(NULL);
        HBITMAP big = CreateCompatibleBitmap(scr, br.right, br.bottom);
        BITMAP bm;
        HGDIOBJ so = SelectObject(sdc, statemgr_bitmaps[slot]), dobj = SelectObject(ddc, big);

        GetObject(statemgr_bitmaps[slot], sizeof(bm), &bm);
        SetStretchBltMode(ddc, HALFTONE);
        SetBrushOrgEx(ddc, 0, 0, NULL);
        StretchBlt(ddc, 0, 0, br.right, br.bottom, sdc, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
        SelectObject(sdc, so);
        SelectObject(ddc, dobj);
        DeleteDC(sdc); DeleteDC(ddc); ReleaseDC(NULL, scr);
        DeleteObject(statemgr_bitmaps[slot]);
        statemgr_bitmaps[slot] = big;
      }
    }

    /* Three short lines -- "Slot N", time, date -- because the full
       "hh:mm:ss  DD/MM/YYYY" stamp is wider than a thumbnail column. */
    if (state_file_time(state_p, stamp, sizeof(stamp)) && lstrlenA(stamp) >= 20)
    {
      stamp[8] = '\0';
      wsprintfA(label, "Slot %d\n%s\n%s", slot, stamp, stamp + 10);
    }
    else
    {
      wsprintfA(label, "Slot %d", slot);
    }
  }
  else
  {
    wsprintfA(label, "Slot %d\nEmpty", slot);
  }

  /* Always enabled, so an empty slot can be right-clicked or double-clicked to save to it. A state without a thumbnail (an older
     save from before this dialog existed has no .bmp) still loads, just without a preview image. */
  SendMessage(btn, BM_SETIMAGE, IMAGE_BITMAP, (LPARAM)statemgr_bitmaps[slot]);
  SetWindowTextA(lbl, label);
}

static INT_PTR CALLBACK statemgr_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
    case WM_INITDIALOG:
    {
      int i;
      static GuiTip tips[STATEMGR_SLOTS + 1];
      theme_apply_to_window(dlg);
      for (i = 0; i < STATEMGR_SLOTS; i++)
      {
        statemgr_refresh_slot(dlg, i);
        tips[i].id = IDC_STATEMGR_SLOT_BASE + i;
        tips[i].label = NULL;
        tips[i].tip = "Left-click loads this slot (double-click an empty one to save to it); right-click saves to it, saves it to a file, or deletes it.";
      }
      tips[STATEMGR_SLOTS].id = IDCANCEL;
      tips[STATEMGR_SLOTS].label = NULL;
      tips[STATEMGR_SLOTS].tip = "Close this window.";
      gui_dialog_tips(dlg, tips, STATEMGR_SLOTS + 1);
      return TRUE;
    }

    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    {
      HBRUSH br = theme_ctlcolor((HDC)wp);
      if (br) return (LRESULT)br;
      break;
    }

    case WM_CONTEXTMENU:
    {
      HWND target = (HWND)wp;
      int id = GetDlgCtrlID(target);

      if (id >= IDC_STATEMGR_SLOT_BASE && id < IDC_STATEMGR_SLOT_BASE + STATEMGR_SLOTS)
      {
        int slot = id - IDC_STATEMGR_SLOT_BASE;
        char state_p[GUI_PATH_LEN];
        int exists;
        HMENU menu;
        int cmd;

        state_path(slot, state_p, sizeof(state_p));
        exists = GetFileAttributesA(state_p) != INVALID_FILE_ATTRIBUTES;

        menu = CreatePopupMenu();
        AppendMenuA(menu, MF_STRING, 1, "Save to This Slot");
        AppendMenuA(menu, MF_STRING | (exists ? 0 : MF_GRAYED), 3, "Save This Slot As...");
        AppendMenuA(menu, MF_STRING | (exists ? 0 : MF_GRAYED), 2, "Delete");

        /* Documented Windows workaround (MSDN, TrackPopupMenu remarks):
           without this, the popup can fail to dismiss correctly if this
           window isn't confirmed as the foreground one. */
        {
          /* lp is a signed screen position (negative on a monitor left of or
             above the main one), or -1 when opened from the keyboard: then
             open it at the middle of the slot's button. */
          int px = (int)(short)LOWORD(lp), py = (int)(short)HIWORD(lp);

          if (lp == (LPARAM)-1 || (px == -1 && py == -1))
          {
            RECT br;
            GetWindowRect(target, &br);
            px = (br.left + br.right) / 2;
            py = (br.top + br.bottom) / 2;
          }

          SetForegroundWindow(dlg);
          cmd = gui_popup(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, px, py);
        }
        PostMessage(dlg, WM_NULL, 0, 0);
        DestroyMenu(menu);

        if (cmd == 1)
        {
          emu_save_state(slot);
          statemgr_refresh_slot(dlg, slot);
        }
        else if (cmd == 3 && exists)                            /* the slot's state copied to a file of your choice (as Save State to File does for the game now) */
        {
          char dest[GUI_PATH_LEN], suggest[GUI_PATH_LEN], base[GUI_PATH_LEN];
          lstrcpynA(base, emu_rom_base(), sizeof(base));
          wsprintfA(suggest, "%s slot %d.gpstate", base, slot);
          if (gui_save_file(dlg, "Save This Slot As", "Genesis Plus EX State\0*.gpstate\0All Files\0*.*\0\0", "gpstate", suggest, dest, sizeof(dest)))
            gui_notify(CopyFileA(state_p, dest, FALSE) ? "Slot %d saved to the file" : "Could not write the state file", slot);
        }
        else if (cmd == 2 && exists)
        {
          state_delete(slot);
          statemgr_refresh_slot(dlg, slot);
        }
        return TRUE;
      }
      return FALSE;
    }

    case WM_COMMAND:
    {
      int id = LOWORD(wp);

      if (id >= IDC_STATEMGR_SLOT_BASE && id < IDC_STATEMGR_SLOT_BASE + STATEMGR_SLOTS &&
          (HIWORD(wp) == BN_CLICKED || HIWORD(wp) == BN_DOUBLECLICKED))
      {
        int slot = id - IDC_STATEMGR_SLOT_BASE;
        char state_p[GUI_PATH_LEN];

        state_path(slot, state_p, sizeof(state_p));
        if (GetFileAttributesA(state_p) != INVALID_FILE_ATTRIBUTES)
        {
          if (HIWORD(wp) == BN_CLICKED)
          {
            emu_load_state(slot);
            EndDialog(dlg, IDOK);
          }
        }
        else if (HIWORD(wp) == BN_DOUBLECLICKED)           /* a double-click on an empty slot saves to it */
        {
          emu_save_state(slot);
          statemgr_refresh_slot(dlg, slot);
        }
        return TRUE;
      }

      if (id == IDCANCEL)
      {
        EndDialog(dlg, IDCANCEL);
        return TRUE;
      }
      return FALSE;
    }

    case WM_DESTROY:
    {
      int i;
      for (i = 0; i < STATEMGR_SLOTS; i++)
      {
        if (statemgr_bitmaps[i])
        {
          DeleteObject(statemgr_bitmaps[i]);
          statemgr_bitmaps[i] = NULL;
        }
      }
      return FALSE;
    }

    case WM_CLOSE:
      EndDialog(dlg, IDCANCEL);
      return TRUE;
  }

  return FALSE;
}

void dlg_state_manager(HWND parent)
{
  if (!emu_running) return;
  gui_dialog_box(gui.large_ui ? IDD_STATEMGR_LARGE : IDD_STATEMGR, parent, statemgr_proc);
}
