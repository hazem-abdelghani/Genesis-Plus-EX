/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  picobook.c -- the "Storyware" window: the picture of the page of a Sega Pico storybook that is open.
 *
 *  A Sega Pico game comes with a book; the console knows which page is open and the game follows it. The program turns the
 *  page with the keys Page Up and Page Down (see IDM_EMU_PICO_PAGEPREV / NEXT in main.c) and this window shows a picture of that page,
 *  when there is one: Bezels\Pico\<game>\<game>_1.png, <game>_2.png ... (_0 is the closed book; .png, .jpg or .bmp). The
 *  pictures are the person's own (scans of the book); none comes with the program.
 *
 *  The window is a window of its own (a taskbar button of its own), so it can be put beside the game or on another screen. It
 *  does not take the focus from the game, and Page Up / Page Down turn the page in it too. Being in this window is being in
 *  the program: the keys of the pad (the Pico's D-pad) and the mouse (the pen pad) reach the game from it.
 ****************************************************************************/

#include <windows.h>
#include <stdio.h>

#include "shared.h"
#include "gui.h"
#include "resource.h"
#include "toolbar.h"            /* DPIS */
#include "theme.h"
#include "stb_image.h"

#define PB_CLASS "GPEXPicoBook"

static HWND    pb_win;
static int     pb_page;
static HBITMAP pb_bmp;          /* the picture of the page (32 bit, top-down), or NULL: there is none */
static int     pb_w, pb_h;
static char    pb_dir[GUI_PATH_LEN];      /* where the pictures of this game are looked for */
static int     pb_class_done;
static RECT    pb_rect;         /* where the window was last (kept while the program runs) */
static int     pb_rect_ok;
static int     pb_user_sized;   /* the window was resized by hand (or maximized): turning the page keeps its size for the rest of this run */
static int     pb_auto_w, pb_auto_h;      /* the size the program gave it to fit a picture */

static void pb_free(void)
{
  if (pb_bmp) { DeleteObject(pb_bmp); pb_bmp = NULL; }
  pb_w = pb_h = 0;
}

/* Loads the picture of a page: <game>_<N>.png, .jpg, .jpeg or .bmp. Alpha is put over the dark background of the window. */
static void pb_load(int page)
{
  static const char *const ext[] = { "png", "jpg", "jpeg", "bmp" };
  char path[GUI_PATH_LEN * 2];
  const char *name = emu_rom_base();
  int k;

  pb_free();
  lstrcpynA(path, osd_path("Images\\Pico-Storyware"), sizeof(path));
  wsprintfA(pb_dir, "%s\\%s", path, name);      /* the game's own folder (the pictures may also lie loose in Images\Pico-Storyware) */

  for (k = 0; k < 4 && !pb_bmp; k++)
  {
    int iw = 0, ih = 0, ch = 0;
    unsigned char *img;

    wsprintfA(path, "%s\\%s_%d.%s", pb_dir, name, page, ext[k]);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
    {
      wsprintfA(path, "%s\\%s_%d.%s", osd_path("Images\\Pico-Storyware"), name, page, ext[k]);        /* loose in Images\Pico-Storyware */
      if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;
    }
    img = stbi_load(path, &iw, &ih, &ch, 4);
    if (!img) continue;
    if (iw > 0 && ih > 0 && iw <= 8192 && ih <= 8192)
    {
      BITMAPINFO bi;
      void *bits = NULL;
      HBITMAP bmp;

      ZeroMemory(&bi, sizeof(bi));
      bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
      bi.bmiHeader.biWidth = iw;
      bi.bmiHeader.biHeight = -ih;                    /* top-down */
      bi.bmiHeader.biPlanes = 1;
      bi.bmiHeader.biBitCount = 32;
      bi.bmiHeader.biCompression = BI_RGB;
      bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
      if (bmp && bits)
      {
        size_t n = (size_t)iw * (size_t)ih, i;
        unsigned char *d = (unsigned char *)bits;

        for (i = 0; i < n; i++)
        {
          const unsigned char *s = img + i * 4;
          int a = s[3];
          d[i * 4 + 0] = (unsigned char)((s[2] * a + 32 * (255 - a)) / 255);        /* over a dark gray, in BGR */
          d[i * 4 + 1] = (unsigned char)((s[1] * a + 32 * (255 - a)) / 255);
          d[i * 4 + 2] = (unsigned char)((s[0] * a + 32 * (255 - a)) / 255);
          d[i * 4 + 3] = 255;
        }
        pb_bmp = bmp; pb_w = iw; pb_h = ih;
      }
      else if (bmp) DeleteObject(bmp);
    }
    stbi_image_free(img);
  }
}

static void pb_title(void)
{
  char t[GUI_PATH_LEN];

  if (!pb_win) return;
  if (pb_page) wsprintfA(t, "Storyware - %s - Page %d", emu_rom_base(), pb_page);
  else wsprintfA(t, "Storyware - %s - closed", emu_rom_base());
  SetWindowTextA(pb_win, t);
}

/* Where the page is drawn in the client area: all of it, its shape kept and centered (the whole client area when there is no picture). */
static void pb_fit(const RECT *rc, RECT *d)
{
  int cw = rc->right - rc->left, ch = rc->bottom - rc->top, w = cw, h = ch;

  if (pb_bmp && pb_w > 0 && pb_h > 0 && cw > 0 && ch > 0)
  {
    h = (int)((long long)pb_h * cw / pb_w);
    if (h > ch) { h = ch; w = (int)((long long)pb_w * ch / pb_h); }
  }
  d->left = rc->left + (cw - w) / 2;
  d->top = rc->top + (ch - h) / 2;
  d->right = d->left + w;
  d->bottom = d->top + h;
}

static void pb_paint(HWND hwnd)
{
  PAINTSTRUCT ps;
  HDC hdc = BeginPaint(hwnd, &ps);
  RECT rc;
  HBRUSH bg = CreateSolidBrush(RGB(32, 32, 32));

  GetClientRect(hwnd, &rc);
  FillRect(hdc, &rc, bg);
  DeleteObject(bg);

  if (pb_bmp && pb_w > 0 && pb_h > 0 && rc.right > 8 && rc.bottom > 8)
  {
    HDC mem = CreateCompatibleDC(hdc);
    HGDIOBJ old = SelectObject(mem, pb_bmp);
    RECT d;

    pb_fit(&rc, &d);                                                          /* the whole page, its shape kept */
    SetStretchBltMode(hdc, HALFTONE);
    SetBrushOrgEx(hdc, 0, 0, NULL);
    StretchBlt(hdc, d.left, d.top, d.right - d.left, d.bottom - d.top, mem, 0, 0, pb_w, pb_h, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
  }
  else
  {
    char msg[GUI_PATH_LEN * 2];
    const char *name = emu_rom_base();
    RECT tr = rc;

    wsprintfA(msg, "No picture for %s.\n\nPut the pictures of the book in a folder named like the game, each named like the game with the page number after it (%s_1.png, %s_2.png ...; _0 for the closed book; PNG, JPG or BMP):\n\n%s\n\nPage Up and Page Down turn the page.",
              pb_page ? "this page" : "the closed book", name, name, pb_dir);
    InflateRect(&tr, -DPIS(16), -DPIS(16));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(210, 210, 210));
    SelectObject(hdc, GetStockObject(DEFAULT_GUI_FONT));
    DrawTextA(hdc, msg, -1, &tr, DT_WORDBREAK | DT_NOPREFIX);
  }
  EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK pb_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
    case WM_PAINT:
      pb_paint(hwnd);
      return 0;

    case WM_ERASEBKGND:
      return 1;

    case WM_SIZE:
    {
      static int was_iconic;

      if (wp == SIZE_MINIMIZED) was_iconic = 1;
      else if (was_iconic)                           /* brought back (from the taskbar): the program's window comes back with it */
      {
        was_iconic = 0;
        if (g_hwnd && IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
      }
      if (wp == SIZE_MAXIMIZED) pb_user_sized = 1;
      InvalidateRect(hwnd, NULL, FALSE);
      return 0;
    }

    case WM_KEYDOWN:
      /* Page Up (next) and Page Down (previous) turn the page, as in the main window. The arrow keys are not used here: they are the
         Pico's D-pad, read from the keyboard wherever the focus is in the program. */
      if (wp == VK_PRIOR) PostMessage(g_hwnd, WM_COMMAND, IDM_EMU_PICO_PAGENEXT, 0);
      else if (wp == VK_NEXT) PostMessage(g_hwnd, WM_COMMAND, IDM_EMU_PICO_PAGEPREV, 0);
      else if (wp == VK_ESCAPE) DestroyWindow(hwnd);
      return 0;

    case WM_ACTIVATE:
      /* The main window is told when it is left for this one (and keeps the game going), but not when this one is left: so the
         input is given back to the game here, and when another program is gone to the main window does what it does for it. */
      if (LOWORD(wp) != WA_INACTIVE) gui_input_set_focus(1);
      else if ((HWND)lp != g_hwnd) SendMessage(g_hwnd, WM_ACTIVATE, WA_INACTIVE, lp);
      return 0;

    case WM_EXITSIZEMOVE:
    {
      RECT r;                                   /* a size that is not the one given to fit the picture was chosen by hand: it is kept */
      if (GetWindowRect(hwnd, &r) && (r.right - r.left != pb_auto_w || r.bottom - r.top != pb_auto_h)) pb_user_sized = 1;
      return 0;
    }

    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;

    case WM_DESTROY:
      if (GetWindowRect(hwnd, &pb_rect)) pb_rect_ok = 1;
      pb_win = NULL;
      pb_free();
      return 0;
  }
  return DefWindowProcA(hwnd, msg, wp, lp);
}

/* The window as big as the picture (its size in pixels; smaller, with the shape kept, when it would not fit the screen). */
static void pb_fit_window(void)
{
  MONITORINFO mi;
  RECT wr, cr;
  DWORD style, ex;
  int cw, ch, maxw, maxh;

  if (!pb_win || !pb_bmp || pb_w < 1 || pb_h < 1 || pb_user_sized || IsZoomed(pb_win) || IsIconic(pb_win)) return;
  mi.cbSize = sizeof(mi);
  if (!GetWindowRect(pb_win, &wr) || !GetMonitorInfo(MonitorFromWindow(pb_win, MONITOR_DEFAULTTONEAREST), &mi)) return;
  style = (DWORD)GetWindowLongPtr(pb_win, GWL_STYLE);
  ex = (DWORD)GetWindowLongPtr(pb_win, GWL_EXSTYLE);

  cr.left = cr.top = 0; cr.right = 100; cr.bottom = 100;
  AdjustWindowRectEx(&cr, style, FALSE, ex);
  maxw = (mi.rcWork.right - mi.rcWork.left) - (cr.right - cr.left - 100);       /* the room for the picture itself */
  maxh = (mi.rcWork.bottom - mi.rcWork.top) - (cr.bottom - cr.top - 100);

  cw = pb_w; ch = pb_h;
  if (cw > maxw) { ch = (int)((long long)ch * maxw / cw); cw = maxw; }
  if (ch > maxh) { cw = (int)((long long)cw * maxh / ch); ch = maxh; }
  if (cw < 64) cw = 64;
  if (ch < 64) ch = 64;

  cr.left = cr.top = 0; cr.right = cw; cr.bottom = ch;
  AdjustWindowRectEx(&cr, style, FALSE, ex);
  SetWindowPos(pb_win, NULL, wr.left, wr.top, cr.right - cr.left, cr.bottom - cr.top, SWP_NOZORDER | SWP_NOACTIVATE);
  pb_auto_w = cr.right - cr.left; pb_auto_h = cr.bottom - cr.top;
  if (GetWindowRect(pb_win, &wr))                              /* grown past the screen: pulled back in */
  {
    int x = wr.left, y = wr.top;
    if (wr.right > mi.rcWork.right) x -= wr.right - mi.rcWork.right;
    if (wr.bottom > mi.rcWork.bottom) y -= wr.bottom - mi.rcWork.bottom;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y < mi.rcWork.top) y = mi.rcWork.top;
    if (x != wr.left || y != wr.top) SetWindowPos(pb_win, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
}

/* The Sega Pico's pen pad: the pen is where the mouse is over the picture of the page (x 0x03C - 0x17C, y 0x1FC - 0x2F7: the
   numbers the console reads) and the left and right mouse buttons are the pen and the red button, when the mouse is in this window.
   Returns 0 when the mouse is not over this window (the pen is then where the mouse is over the game's picture). */
int picobook_pen(int *x, int *y, int *pen, int *red)
{
  POINT pt;
  RECT cr, d;
  int inside, dw, dh;

  if (!pb_win || !GetCursorPos(&pt) || WindowFromPoint(pt) != pb_win) return 0;
  ScreenToClient(pb_win, &pt);
  GetClientRect(pb_win, &cr);
  pb_fit(&cr, &d);
  dw = d.right - d.left; dh = d.bottom - d.top;
  if (dw < 1 || dh < 1) return 0;
  inside = PtInRect(&d, pt) ? 1 : 0;
  if (pt.x < d.left) pt.x = d.left; else if (pt.x >= d.right) pt.x = d.right - 1;
  if (pt.y < d.top) pt.y = d.top; else if (pt.y >= d.bottom) pt.y = d.bottom - 1;
  *x = 0x03C + (pt.x - d.left) * (0x17C - 0x03C + 1) / dw;
  *y = 0x1FC + (pt.y - d.top) * (0x2F7 - 0x1FC + 1) / dh;
  *pen = inside && (GetAsyncKeyState(VK_LBUTTON) & 0x8000);
  *red = inside && (GetAsyncKeyState(VK_RBUTTON) & 0x8000);
  return 1;
}

/* Shows the window (opening it when it is not open) with the picture of this page of the open game. */
void picobook_show(int page)
{
  pb_page = page;
  pb_load(page);

  if (!pb_win)
  {
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = DPIS(520), h = DPIS(700);

    if (!pb_class_done)
    {
      WNDCLASSA wc;
      ZeroMemory(&wc, sizeof(wc));
      wc.lpfnWndProc = pb_proc;
      wc.hInstance = GetModuleHandle(NULL);
      wc.hCursor = LoadCursor(NULL, IDC_CROSS);          /* the pen */
      wc.hIcon = LoadIconA(GetModuleHandle(NULL), MAKEINTRESOURCEA(IDI_APPICON));
      wc.lpszClassName = PB_CLASS;
      if (!RegisterClassA(&wc)) return;
      pb_class_done = 1;
    }
    if (pb_rect_ok)
    {
      x = pb_rect.left; y = pb_rect.top; w = pb_rect.right - pb_rect.left; h = pb_rect.bottom - pb_rect.top;
    }
    else
    {
      RECT mr;                                   /* the first time: beside the program's window, when there is room */
      MONITORINFO mi;

      mi.cbSize = sizeof(mi);
      if (g_hwnd && GetWindowRect(g_hwnd, &mr) && GetMonitorInfo(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi) &&
          mr.right + w <= mi.rcWork.right)
      {
        x = mr.right; y = mr.top;
        if (y + h > mi.rcWork.bottom) h = mi.rcWork.bottom - y;
      }
    }
    pb_win = CreateWindowExA(0, PB_CLASS, "Storyware", WS_OVERLAPPEDWINDOW, x, y, w, h, NULL, NULL, GetModuleHandle(NULL), NULL);
    if (!pb_win) return;
    theme_apply_to_window(pb_win);                     /* the title bar and frame in the program's theme */
    ShowWindow(pb_win, SW_SHOWNOACTIVATE);             /* the game keeps the focus */
  }
  pb_fit_window();
  pb_title();
  InvalidateRect(pb_win, NULL, FALSE);
}

/* A Sega Pico game has just been loaded: the closed book (page 0) is shown when there is a picture for it (the window is not opened
   with only its "no picture" text). */
void picobook_show_closed(void)
{
  if (pb_win) { picobook_show(0); return; }
  pb_load(0);
  if (!pb_bmp) return;
  picobook_show(0);
}

/* Closes the window (the game was stopped). */
void picobook_close(void)
{
  if (pb_win) DestroyWindow(pb_win);
}

/* The theme or the style was changed: the window's frame follows. */
void picobook_retheme(void)
{
  if (pb_win) { theme_apply_to_window(pb_win); RedrawWindow(pb_win, NULL, NULL, RDW_INVALIDATE | RDW_FRAME); }
}

/* The program's window was minimized or brought back (from the taskbar or by a click): this window goes with it. One that was
   minimized by hand is left alone. */
void picobook_main_minimized(int minimized)
{
  static int hid_by_main;

  if (!pb_win) { hid_by_main = 0; return; }
  if (minimized)
  {
    if (!IsIconic(pb_win)) { ShowWindow(pb_win, SW_SHOWMINNOACTIVE); hid_by_main = 1; }
  }
  else if (hid_by_main)
  {
    hid_by_main = 0;
    if (IsIconic(pb_win)) ShowWindow(pb_win, SW_SHOWNOACTIVATE);        /* (back as it was; the game's window keeps the focus) */
  }
}

/* 1 when the window is the one in front (the program's input is then the game's, as in the main window). */
int picobook_active(void)
{
  return pb_win && GetForegroundWindow() == pb_win;
}

/* 1 when h is the window (so the main message loop leaves its keys to it). */
int picobook_owns(HWND h)
{
  return h && pb_win == h;
}
