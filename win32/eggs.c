/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  eggs.c -- hidden extras. Nothing here is in the menus, the Menu Guide or
 *  the README on purpose. Compiled in only with GPEX_EGGS (Makefile: EGGS=1).
 *
 *  HOW TO TRIGGER THEM
 *
 *  Credits          On the game list, type G P E X (the letters, one after the other).
 *                   A black window scrolls the credits; a click or any key closes it.
 *  SEGA sound       Help > About: click the program icon 10 times, each click within
 *                   1.5 seconds of the last (plays sega.wav).
 *  Ring sound       Grid view: click the logo above the first heading 10 times, each
 *                   within 1.5 seconds of the last (plays ring_sfx.wav). The clicks do
 *                   not select anything.
 *  Logo shake       Grid view: move the pointer onto the logo; it wobbles for a moment.
 *  Birthdays        Start the program on the day a console came out: 15 July (SG-1000),
 *                   20 October (Master System), 29 October (Mega Drive, Japan),
 *                   14 August (Genesis, USA), 6 October (Game Gear, Japan),
 *                   26 April (Game Gear, USA), 12 December (Mega CD), 15 October
 *                   (Sega CD). A message shows for 6 seconds. To test one, set the
 *                   PC clock to that day.
 *  Chaos Emerald    Settings > General: right click the Accent Color list 7 times, each
 *                   within 3 seconds of the last. A green "Chaos Emerald" accent is added
 *                   and selected; it stays in the list afterwards.
 *
 *  The floating message (toast) is a child of the main window, so it moves with it.
 ****************************************************************************/

#include "eggs.h"

#ifdef GPEX_EGGS

#include <mmsystem.h>
#include <commctrl.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "shared.h"
#include "gui.h"
#include "resource.h"
#include "theme.h"
#include "toolbar.h"

/* ---- a message that floats over the window, bottom centre (the OSD only shows over a running game) ---- */

static HWND toast_wnd;
static char toast_text[160];

static int toast_drop;                 /* 1: a little lower (the birthdays) */

static void toast_place(HWND w)
{
  /* From the size of the window itself, bottom centre, so it stays put when the game area changes as a game starts or stops. */
  RECT wr, tr;
  POINT p;
  GetWindowRect(g_hwnd, &wr);
  GetWindowRect(w, &tr);
  p.x = (wr.left + wr.right - (tr.right - tr.left)) / 2;
  p.y = wr.bottom - (tr.bottom - tr.top) - (toast_drop ? TBS(55) : TBS(95));
  ScreenToClient(g_hwnd, &p);
  SetWindowPos(w, HWND_TOP, p.x, p.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}

static LRESULT CALLBACK toast_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
    case WM_TIMER:
      if (wp == 2) { toast_place(w); return 0; }              /* follows the main window while it is up */
      DestroyWindow(w);
      return 0;
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT:
    {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(w, &ps);
      RECT rc;
      HBRUSH br = CreateSolidBrush(RGB(28, 28, 32));
      HPEN pen = CreatePen(PS_SOLID, 1, RGB(90, 150, 255));
      HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
      HFONT font = CreateFontA(-TBS(16), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI"), of;
      GetClientRect(w, &rc);
      RoundRect(dc, 0, 0, rc.right, rc.bottom, TBS(14), TBS(14));
      of = (HFONT)SelectObject(dc, font);
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, RGB(255, 255, 255));
      DrawTextA(dc, toast_text, -1, &rc, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
      SelectObject(dc, of); DeleteObject(font);
      SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(br); DeleteObject(pen);
      EndPaint(w, &ps);
      return 0;
    }
    case WM_DESTROY: if (w == toast_wnd) toast_wnd = NULL; return 0;
  }
  return DefWindowProcA(w, msg, wp, lp);
}

static void toast(int ms, const char *fmt, ...)
{
  static int registered;
  char buf[160];
  va_list ap;
  SIZE sz;
  HDC dc;
  HFONT font, of;
  int w, h;

  va_start(ap, fmt);
  wvsprintfA(buf, fmt, ap);
  va_end(ap);
  lstrcpynA(toast_text, buf, sizeof(toast_text));
  if (!g_hwnd) return;
  if (!registered)
  {
    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = toast_proc;
    wc.hInstance = g_inst;
    wc.lpszClassName = "GpexToast";
    RegisterClassA(&wc);
    registered = 1;
  }
  dc = GetDC(NULL);
  font = CreateFontA(-TBS(16), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
  of = (HFONT)SelectObject(dc, font);
  GetTextExtentPoint32A(dc, toast_text, lstrlenA(toast_text), &sz);
  SelectObject(dc, of); DeleteObject(font); ReleaseDC(NULL, dc);
  w = sz.cx + TBS(44); h = sz.cy + TBS(20);

  if (toast_wnd) DestroyWindow(toast_wnd);
  toast_wnd = CreateWindowExA(0, "GpexToast", "", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, w, h, g_hwnd, NULL, g_inst, NULL);   /* inside the main window, so it moves with it */
  if (!toast_wnd) return;
  {
    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, TBS(14), TBS(14));
    SetWindowRgn(toast_wnd, rgn, TRUE);
  }
  toast_place(toast_wnd);
  ShowWindow(toast_wnd, SW_SHOWNOACTIVATE);
  SetTimer(toast_wnd, 1, (UINT)ms, NULL);
  SetTimer(toast_wnd, 2, 100, NULL);
}

/* ---- sounds kept inside the program (sega.wav, ring_sfx.wav), played without a thread ---- */

static void play_sfx(int id)
{
  HRSRC res = FindResourceA(NULL, MAKEINTRESOURCEA(id), (LPCSTR)RT_RCDATA);
  HGLOBAL g = res ? LoadResource(NULL, res) : NULL;
  const void *wav = g ? LockResource(g) : NULL;
  if (wav) PlaySoundA((LPCSTR)wav, NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

/* ---- "gpex" for the credits ---- */

static void credits_window(void);
static void CALLBACK wobble_tick(HWND w, UINT m, UINT_PTR id, DWORD t);

/* "gpex" typed on the game list opens the credits. */
void eggs_key(WPARAM vk, LPARAM lp)
{
  static char typed[8];

  if (lp & 0x40000000) return;                                                /* a key held down repeats: not another press */
  if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_CAPITAL || vk == VK_LSHIFT || vk == VK_RSHIFT) return;
  if (!browser_panel_visible() || (GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000)) { typed[0] = 0; return; }   /* only on the game list, never while a game runs */

  if (vk >= 'A' && vk <= 'Z')
  {
    int n = lstrlenA(typed);
    if (n >= (int)sizeof(typed) - 1) { memmove(typed, typed + 1, (size_t)n - 1); n--; }
    typed[n] = (char)vk; typed[n + 1] = 0;
    n++;
    if (n >= 4 && !memcmp(typed + n - 4, "GPEX", 4)) { typed[0] = 0; credits_window(); }
  }
  else typed[0] = 0;
}

/* ---- the birthdays ---- */

static const struct { int month, day; const char *text; } birthdays[] =
{
  {  7, 15, "Happy birthday, SG-1000! (Japan, 1983)" },
  { 10, 20, "Happy birthday, Master System! (Mark III, Japan, 1985)" },
  { 10, 29, "Happy birthday, Mega Drive! (Japan, 1988)" },
  {  8, 14, "Happy birthday, Genesis! (USA, 1989)" },
  { 10,  6, "Happy birthday, Game Gear! (Japan, 1990)" },
  { 12, 12, "Happy birthday, Mega CD! (Japan, 1991)" },
  {  4, 26, "Happy birthday, Game Gear! (USA, 1991)" },
  { 10, 15, "Happy birthday, Sega CD! (USA, 1992)" }
};

void eggs_startup(void)
{
  SYSTEMTIME t;
  int i;
  SetTimer(NULL, 0, 40, wobble_tick);
  GetLocalTime(&t);
  for (i = 0; i < (int)(sizeof(birthdays) / sizeof(birthdays[0])); i++)
    if (t.wMonth == birthdays[i].month && t.wDay == birthdays[i].day) { toast_drop = 1; toast(6000, "%s", birthdays[i].text); break; }
}

/* ---- the logo above the grid: ten clicks ---- */

void eggs_ring(void)
{
  static int clicks;
  static DWORD last;
  DWORD now = GetTickCount();

  if (now - last > 1500) clicks = 0;
  last = now;
  if (++clicks == 10) { clicks = 0; play_sfx(IDR_RING_SFX); }
}

/* ---- About: ten clicks on the icon ---- */

void eggs_about_click(HWND dlg, LPARAM lp)
{
  static int clicks;
  static DWORD last;
  RECT r;
  POINT p;
  DWORD now = GetTickCount();

  p.x = (short)LOWORD(lp); p.y = (short)HIWORD(lp);
  ClientToScreen(dlg, &p);
  if (!GetWindowRect(GetDlgItem(dlg, IDC_ABOUT_ICON), &r) || !PtInRect(&r, p)) { clicks = 0; return; }
  if (now - last > 1500) clicks = 0;
  last = now;
  if (++clicks == 10) { clicks = 0; play_sfx(IDR_SEGA_SFX); }
}

/* ---- the logo shakes when the pointer is on it ---- */

extern int  browser_logo_rect(RECT *scr);
extern void browser_logo_redraw(void);

static DWORD wob_start;
static int wob_hover, wob_dx;

int eggs_logo_dx(void) { return wob_dx; }

static void CALLBACK wobble_tick(HWND w, UINT m, UINT_PTR id, DWORD t)
{
  RECT r;
  POINT p;
  int hover = 0, dx = 0;
  (void)w; (void)m; (void)id; (void)t;

  if (browser_logo_rect(&r))
  {
    GetCursorPos(&p);
    hover = PtInRect(&r, p) && GetForegroundWindow() == g_hwnd;
    if (hover)
    {
      GUITHREADINFO gti;                      /* no wobble while a menu (the menu bar, a right-click menu) is open */
      gti.cbSize = sizeof(gti);
      if (GetGUIThreadInfo(0, &gti) && (gti.flags & GUI_INMENUMODE)) { hover = 0; wob_start = 0; }
    }
  }
  if (hover && !wob_hover) wob_start = GetTickCount();
  wob_hover = hover;
  if (wob_start)
  {
    DWORD age = GetTickCount() - wob_start;
    if (age > 700) wob_start = 0;
    else dx = (int)(TBS(10) * sin(age / 45.0) * (1.0 - age / 700.0));
  }
  if (dx != wob_dx || (wob_dx && !dx)) { wob_dx = dx; browser_logo_redraw(); }
}

/* ---- a hidden accent colour: seven right clicks on the Accent Color list ---- */

int eggs_accent_tap(void)
{
  static int taps;
  static DWORD last;
  DWORD now = GetTickCount();
  if (now - last > 3000) taps = 0;
  last = now;
  if (++taps < 7) return 0;
  taps = 0;
  return 1;
}

/* ---- the credits: a black window with the text scrolling up ---- */

static const char *const credit_lines[] =
{
  "GENESIS PLUS EX",
  "",
  "Windows front end",
  "Hazem Abdelghani",
  "",
  "Genesis Plus GX core",
  "Eke-Eke",
  "",
  "Original Genesis Plus",
  "Charles MacDonald",
  "",
  "Nuked OPN2 / OPLL",
  "Nuked",
  "",
  "xBRZ",
  "Zenju",
  "",
  "librashader",
  "SnowflakePowered",
  "",
  "Thanks for playing!",
  "",
  "(click or press a key)"
};
#define CREDIT_N ((int)(sizeof(credit_lines) / sizeof(credit_lines[0])))

static int credits_y;

static LRESULT CALLBACK credits_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
    case WM_CREATE: SetTimer(w, 1, 30, NULL); return 0;
    case WM_TIMER:
    {
      RECT rc; GetClientRect(w, &rc);
      credits_y -= 1;
      if (credits_y < -(CREDIT_N * 28 + 40)) credits_y = rc.bottom;
      InvalidateRect(w, NULL, FALSE);
      return 0;
    }
    case WM_PAINT:
    {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(w, &ps), mem = CreateCompatibleDC(dc);
      RECT rc, line;
      HBITMAP bmp, old;
      HFONT font, of;
      int i;
      GetClientRect(w, &rc);
      bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
      old = (HBITMAP)SelectObject(mem, bmp);
      FillRect(mem, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
      font = CreateFontA(-22, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
      of = (HFONT)SelectObject(mem, font);
      SetBkMode(mem, TRANSPARENT);
      for (i = 0; i < CREDIT_N; i++)
      {
        int y = credits_y + i * 28;
        line.left = 0; line.right = rc.right; line.top = y; line.bottom = y + 28;
        if (line.bottom < 0 || line.top > rc.bottom) continue;
        SetTextColor(mem, i == 0 ? RGB(80, 140, 255) : (credit_lines[i][0] && !(i % 3 == 0) ? RGB(255, 255, 255) : RGB(170, 170, 170)));
        DrawTextA(mem, credit_lines[i], -1, &line, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
      }
      SelectObject(mem, of); DeleteObject(font);
      BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
      SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
      EndPaint(w, &ps);
      return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_KEYDOWN: case WM_CLOSE:
      DestroyWindow(w);
      return 0;
    case WM_DESTROY: KillTimer(w, 1); return 0;
  }
  return DefWindowProcA(w, msg, wp, lp);
}

static void credits_window(void)
{
  static int registered;
  HWND w;
  RECT pr;
  int cw = 360, ch = 420;

  if (FindWindowA("GpexCredits", NULL)) return;
  if (!registered)
  {
    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = credits_proc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "GpexCredits";
    RegisterClassA(&wc);
    registered = 1;
  }
  GetWindowRect(g_hwnd, &pr);
  credits_y = ch;
  w = CreateWindowExA(WS_EX_TOOLWINDOW, "GpexCredits", "Credits", WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                      pr.left + (pr.right - pr.left - cw) / 2, pr.top + (pr.bottom - pr.top - ch) / 2, cw, ch, g_hwnd, NULL, g_inst, NULL);
  if (w) SetFocus(w);
}

#endif /* GPEX_EGGS */
