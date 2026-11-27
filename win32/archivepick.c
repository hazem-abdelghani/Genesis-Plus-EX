/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *  Copyright (c) 2026 Hazem Abdelghani
 *
 *  archivepick.c -- the window that lists the games of a .zip or .7z archive that holds more than one, and asks which to play
 *  (main.c: pick_archive_game). A modal window like the other small ones of the program: a search box, the list, Play and Cancel.
 ****************************************************************************/

#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <string.h>

#include "shared.h"
#include "gui.h"
#include "theme.h"
#include "resource.h"
#include "i18n.h"

#define AP_CLASS  "GPEXArchivePick"
#define AP_FILTER 301
#define AP_LIST   302
#define AP_LABEL  303

typedef struct
{
  const char *const *names;
  const unsigned *sizes;
  int n;
  int *shown;                 /* the entry (index into names) of each row */
  int nshown;
  int result, done;
  HWND dlg, label, filter, list;
} appick_t;

static int AS(int px)
{
  return dbg_scale(px);
}

static int ap_contains(const char *text, const char *word)
{
  size_t n = strlen(word), i, m = strlen(text);

  if (!n) return 1;
  for (i = 0; i + n <= m; i++)
    if (!_strnicmp(text + i, word, n)) return 1;
  return 0;
}

static void ap_fill(appick_t *a)
{
  char word[128];
  int i;

  GetWindowTextA(a->filter, word, sizeof(word));
  SendMessage(a->list, WM_SETREDRAW, FALSE, 0);
  SendMessage(a->list, LVM_DELETEALLITEMS, 0, 0);
  a->nshown = 0;
  for (i = 0; i < a->n; i++)
  {
    LVITEMA it;
    char size[32];

    if (!ap_contains(a->names[i], word)) continue;
    ZeroMemory(&it, sizeof(it));
    it.mask = LVIF_TEXT;
    it.iItem = a->nshown;
    it.pszText = (char *)a->names[i];
    SendMessageA(a->list, LVM_INSERTITEMA, 0, (LPARAM)&it);
    wsprintfA(size, "%u KB", (a->sizes[i] + 1023) / 1024);
    it.iSubItem = 1;
    it.pszText = size;
    SendMessageA(a->list, LVM_SETITEMTEXTA, (WPARAM)a->nshown, (LPARAM)&it);
    a->shown[a->nshown++] = i;
  }
  if (a->nshown > 0)
  {
    LVITEMA it;

    ZeroMemory(&it, sizeof(it));
    it.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    it.state = LVIS_SELECTED | LVIS_FOCUSED;
    SendMessageA(a->list, LVM_SETITEMSTATE, 0, (LPARAM)&it);
  }
  SendMessage(a->list, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(a->list, NULL, TRUE);
}

static void ap_accept(appick_t *a)
{
  int row = (int)SendMessage(a->list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);

  if (row < 0 || row >= a->nshown) { gui_notify("Pick a game from the list"); return; }
  a->result = a->shown[row];
  a->done = 1;
}

static LRESULT CALLBACK ap_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
  appick_t *a = (appick_t *)GetWindowLongPtr(h, GWLP_USERDATA);

  if (m == WM_NCCREATE) { a = (appick_t *)((CREATESTRUCTA *)lp)->lpCreateParams; SetWindowLongPtr(h, GWLP_USERDATA, (LONG_PTR)a); }
  if (!a) return DefWindowProc(h, m, wp, lp);
  switch (m)
  {
    case WM_COMMAND:
      if (LOWORD(wp) == IDOK) { ap_accept(a); return 0; }
      if (LOWORD(wp) == IDCANCEL) { a->done = 1; return 0; }
      if (LOWORD(wp) == AP_FILTER && HIWORD(wp) == EN_CHANGE) { ap_fill(a); return 0; }
      break;
    case WM_NOTIFY:
      if (((NMHDR *)lp)->idFrom == AP_LIST && ((NMHDR *)lp)->code == NM_DBLCLK) { ap_accept(a); return 0; }
      break;
    case WM_CLOSE:
      a->done = 1;
      return 0;
    case WM_ERASEBKGND:
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

static HWND ap_ctl(HWND p, const char *cls, const char *text, DWORD style, DWORD ex, int x, int y, int w, int h, int id)
{
  HWND c = CreateWindowExA(ex, cls, text, WS_CHILD | WS_VISIBLE | style, AS(x), AS(y), AS(w), AS(h), p, (HMENU)(INT_PTR)id, g_inst, NULL);

  if (c) SendMessage(c, WM_SETFONT, (WPARAM)dbg_ui_font(), TRUE);
  return c;
}

/* Asks which of the n games of an archive to play. Returns the number of the game (0..n-1), or -1 when the window is closed. */
int archive_pick_window(HWND owner, const char *archive, const char *const *names, const unsigned *sizes, int n)
{
  static int registered;
  appick_t a;
  MSG msg = { 0 };
  RECT r, pr;
  DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
  char text[GUI_PATH_LEN + 120];
  const char *base = strrchr(archive, '\\');

  if (n < 1) return -1;
  if (!registered)
  {
    WNDCLASSA wc;

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = ap_proc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = AP_CLASS;
    if (!RegisterClassA(&wc)) return -1;
    i18n_keep_ltr(AP_CLASS);
    registered = 1;
  }
  ZeroMemory(&a, sizeof(a));
  a.names = names; a.sizes = sizes; a.n = n; a.result = -1;
  a.shown = (int *)malloc((size_t)n * sizeof(int));
  if (!a.shown) return -1;
  r.left = 0; r.top = 0; r.right = AS(560); r.bottom = AS(400);
  AdjustWindowRectEx(&r, style, FALSE, 0);
  a.dlg = CreateWindowExA(0, AP_CLASS, "Choose a Game", style, 0, 0, r.right - r.left, r.bottom - r.top, owner, NULL, g_inst, &a);
  if (!a.dlg) { free(a.shown); return -1; }
  wsprintfA(text, "%s holds %d games. Pick the one to play:", base ? base + 1 : archive, n);
  a.label = ap_ctl(a.dlg, "STATIC", text, SS_LEFT | SS_ENDELLIPSIS, 0, 12, 12, 536, 18, AP_LABEL);
  ap_ctl(a.dlg, "STATIC", "Search:", SS_LEFT, 0, 12, 42, 60, 16, 0);
  a.filter = ap_ctl(a.dlg, "EDIT", "", WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, 74, 38, 474, 22, AP_FILTER);
  a.list = ap_ctl(a.dlg, "SysListView32", "", WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 0, 12, 70, 536, 274, AP_LIST);
  SendMessage(a.list, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT);
  {
    LVCOLUMNA col;

    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = "Name";
    col.cx = AS(536) - AS(90) - GetSystemMetrics(SM_CXVSCROLL) - 4;
    SendMessageA(a.list, LVM_INSERTCOLUMNA, 0, (LPARAM)&col);
    col.pszText = "Size";
    col.cx = AS(90);
    SendMessageA(a.list, LVM_INSERTCOLUMNA, 1, (LPARAM)&col);
    gui_list_header_menu(a.list);                  /* (right-click on the titles: Size All Columns to Fit) */
  }
  ap_ctl(a.dlg, "BUTTON", "Play", WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 360, 356, 90, 28, IDOK);
  ap_ctl(a.dlg, "BUTTON", "Cancel", WS_TABSTOP | BS_PUSHBUTTON, 0, 458, 356, 90, 28, IDCANCEL);
  GetWindowRect(owner, &pr);
  SetWindowPos(a.dlg, NULL, pr.left + ((pr.right - pr.left) - (r.right - r.left)) / 2, pr.top + ((pr.bottom - pr.top) - (r.bottom - r.top)) / 2,
               r.right - r.left, r.bottom - r.top, SWP_NOZORDER);
  gui_fit_window(a.dlg);                           /* (a window bigger than the screen is made smaller, and scrolls) */
  {
    RECT wr, wa;
    MONITORINFO mi;

    mi.cbSize = sizeof(mi);
    if (GetWindowRect(a.dlg, &wr) && GetMonitorInfo(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &mi))
    {
      int x = wr.left, y = wr.top;

      wa = mi.rcWork;
      if (x + (wr.right - wr.left) > wa.right) x = wa.right - (wr.right - wr.left);
      if (y + (wr.bottom - wr.top) > wa.bottom) y = wa.bottom - (wr.bottom - wr.top);
      if (x < wa.left) x = wa.left;
      if (y < wa.top) y = wa.top;
      if (x != wr.left || y != wr.top) SetWindowPos(a.dlg, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
  }
  theme_apply_to_window(a.dlg);
  {
    static const GuiTip tips[] =
    {
      { AP_FILTER, NULL, "Type part of a name to show only the games that have it." },
      { AP_LIST, NULL, "The games in the archive. Double-click one to play it." },
      { IDOK, NULL, "Play the picked game." },
      { IDCANCEL, NULL, "Close without starting a game." }
    };

    gui_dialog_tips(a.dlg, tips, (int)(sizeof(tips) / sizeof(tips[0])));
  }
  gui_window_icon(a.dlg);
  ap_fill(&a);

  EnableWindow(owner, FALSE);
  ShowWindow(a.dlg, SW_SHOW);
  SetFocus(a.filter);
  while (!a.done && GetMessage(&msg, NULL, 0, 0))
  {
    if (msg.message == WM_KEYDOWN && IsChild(a.dlg, msg.hwnd))
    {
      if (msg.wParam == VK_ESCAPE) { SendMessage(a.dlg, WM_COMMAND, IDCANCEL, 0); continue; }
      if (msg.wParam == VK_RETURN) { SendMessage(a.dlg, WM_COMMAND, IDOK, 0); continue; }
      if ((msg.wParam == VK_DOWN || msg.wParam == VK_UP) && msg.hwnd == a.filter)      /* (the arrow keys of the filter box move in the list) */
      {
        int row = (int)SendMessage(a.list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);

        row += msg.wParam == VK_DOWN ? 1 : -1;
        if (row >= 0 && row < a.nshown)
        {
          LVITEMA it;

          ZeroMemory(&it, sizeof(it));
          it.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
          it.state = LVIS_SELECTED | LVIS_FOCUSED;
          SendMessageA(a.list, LVM_SETITEMSTATE, (WPARAM)row, (LPARAM)&it);
          SendMessage(a.list, LVM_ENSUREVISIBLE, (WPARAM)row, FALSE);
        }
        continue;
      }
    }
    if (!IsDialogMessage(a.dlg, &msg)) { TranslateMessage(&msg); DispatchMessage(&msg); }
  }
  if (msg.message == WM_QUIT && !a.done) PostQuitMessage((int)msg.wParam);
  EnableWindow(owner, TRUE);
  DestroyWindow(a.dlg);
  SetForegroundWindow(owner);
  free(a.shown);
  return a.result;
}
