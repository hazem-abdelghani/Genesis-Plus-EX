/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  debugger.c -- the Debugger window
 *
 *  One window for all the debug tools, as in gpgx_debugger: the CPU Debugger (breakpoints, stepping, disassembly, registers) on top
 *  and, on tabs under it, the Memory Viewer, VDP Viewer, Sound Viewer, CPU Viewer, RAM Search and RAM Watch. Each of those is the
 *  window of its own module (memview.c, debugwin.c, ramsearch.c, ramwatch.c, dbgcpu.c) made as a child of this one: while a pane
 *  is made, gui_debug_host is this window, and the module makes its window a child of it instead of a window of its own.
 *  The bar between the top and the tabs can be dragged.
 ****************************************************************************/

#include <windows.h>
#include <commctrl.h>
#include "shared.h"
#include "gui.h"
#include "theme.h"
#include "netplay.h"
#include "picocore.h"

#define DB_CLASS "GPEXDebugger"
#define ID_TABS  1
#define ID_VIEW  2
#define DB_BAR   S(30)               /* the height of the bar with the View button (the CPU Debugger starts under it) */
/* The commands of the View menu. */
#define DBM_MEMORY   101
#define DBM_SEARCH   102
#define DBM_WATCH    103
#define DBM_VDP0     200          /* + the view of the VDP Viewer */
#define DBM_SND0     300          /* + the view of the Sound Viewer */
#define DBM_CPUV0    400          /* + the view of the CPU Viewer */

HWND gui_debug_host;
HWND gui_debug_last;

static HWND db_win, db_tab, db_view;
static HWND db_pane[DBGPANE_COUNT];
static int db_split;               /* the height of the top part, in pixels (0: not set yet) */
static int db_drag;
static HWND db_lastfocus;          /* the control that had the focus when the window was left */
static int db_sel;                 /* the tab shown (a DBGPANE_ number below DBGPANE_CPUDBG) */

static const char *const db_tab_names[DBGPANE_CPUDBG] = { "Memory", "VDP", "Sound", "CPU", "RAM Search", "RAM Watch" };

static int S(int px)
{
  return dbg_scale(px);
}

void gui_debug_close(HWND h)
{
  if (db_win && h && GetParent(h) == db_win) PostMessage(db_win, WM_CLOSE, 0, 0);
  else DestroyWindow(h);
}

/* A game with a 68000 (the CPU Debugger's own conditions): Mega Drive, Mega CD, 32X. */
static int db_cpu_ok(void)
{
  if (netplay_active()) return 0;
  return pico_active() || (system_hw & SYSTEM_PBC) == SYSTEM_MD || system_hw == SYSTEM_MCD;
}

static HWND db_make_pane(int p)
{
  if (!db_win) return NULL;
  if (db_pane[p] && IsWindow(db_pane[p])) return db_pane[p];
  gui_debug_host = db_win;
  gui_debug_last = NULL;
  switch (p)
  {
    case DBGPANE_MEMORY:  dlg_memory_viewer(db_win); break;
    case DBGPANE_VDP:     dlg_vdp_viewer(db_win); break;
    case DBGPANE_SOUND:   dlg_sound_viewer(db_win); break;
    case DBGPANE_CPUVIEW: dlg_cpu_viewer(db_win); break;
    case DBGPANE_SEARCH:  ramsearch_open(db_win); break;
    case DBGPANE_WATCH:   ramwatch_open(db_win); break;
    case DBGPANE_CPUDBG:  dlg_cpu_debugger(db_win); break;
  }
  gui_debug_host = NULL;
  db_pane[p] = gui_debug_last;
  gui_debug_last = NULL;
  return db_pane[p];
}

static void db_layout(void)
{
  RECT rc;
  int w, h, gap = S(6), bar = DB_BAR, top = 0, y0, th, i;
  HWND cpu;

  if (!db_win || !db_tab) return;
  GetClientRect(db_win, &rc);
  w = rc.right;
  h = rc.bottom;
  if (w < 1 || h < 1) return;

  if (db_view) MoveWindow(db_view, S(6), S(3), S(70), bar - S(6), TRUE);          /* the View button */
  cpu = (db_pane[DBGPANE_CPUDBG] && IsWindow(db_pane[DBGPANE_CPUDBG])) ? db_pane[DBGPANE_CPUDBG] : NULL;
  if (cpu)
  {
    if (db_split <= 0) db_split = (h - bar) * 55 / 100;
    if (db_split > h - bar - S(180)) db_split = h - bar - S(180);
    if (db_split < S(180)) db_split = S(180);
    top = db_split;
    MoveWindow(cpu, 0, bar, w, top, TRUE);
    ShowWindow(cpu, SW_SHOW);
  }
  y0 = bar + (cpu ? top + gap : 0);
  th = settings_tab_height();                                                       /* the strip is the program's own (dark and Modern like the Settings window) */
  MoveWindow(db_tab, 0, y0, w, th, TRUE);
  for (i = 0; i < DBGPANE_CPUDBG; i++)
  {
    HWND p = db_pane[i];

    if (!p || !IsWindow(p)) continue;
    if (i == db_sel)
    {
      MoveWindow(p, 0, y0 + th, w, h - y0 - th > 0 ? h - y0 - th : 1, TRUE);
      ShowWindow(p, SW_SHOW);
    }
    else ShowWindow(p, SW_HIDE);
  }
}

static void db_select(int p)
{
  if (p < 0 || p >= DBGPANE_CPUDBG) return;
  db_sel = p;
  if (db_tab) { SetWindowLongPtr(db_tab, GWLP_USERDATA, p); InvalidateRect(db_tab, NULL, FALSE); }
  db_make_pane(p);
  db_layout();
  if (db_pane[p] && IsWindow(db_pane[p])) SetFocus(db_pane[p]);
}

/* The View button is drawn here, not by the system or the theme's button painter: whatever those draw for a button that has the focus
   (an outline in a light color) must never show on it. It is a plain rounded button in the colors of the theme. */
static void db_draw_view(const DRAWITEMSTRUCT *di)
{
  HDC dc = di->hDC;
  RECT rc = di->rcItem;
  int dark = theme_is_dark(), pushed = (di->itemState & ODS_SELECTED) != 0;
  COLORREF fill = dark ? (pushed ? theme_dk(RGB(44, 46, 52)) : theme_dk(RGB(54, 57, 66))) : (pushed ? RGB(222, 228, 240) : RGB(251, 251, 252));
  COLORREF edge = dark ? theme_dk(RGB(82, 86, 98)) : RGB(196, 200, 210);
  COLORREF text = dark ? THEME_DARK_TEXT : RGB(27, 27, 27);
  HBRUSH bg = dark ? theme_ctlcolor(dc) : GetSysColorBrush(COLOR_BTNFACE), fb = CreateSolidBrush(fill), ob;
  HPEN pen = CreatePen(PS_SOLID, 1, edge), op;
  HFONT of = (HFONT)SelectObject(dc, gui_get_ui_font());
  char txt[32];

  FillRect(dc, &rc, bg);
  ob = (HBRUSH)SelectObject(dc, fb);
  op = (HPEN)SelectObject(dc, pen);
  RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, S(8), S(8));
  SelectObject(dc, ob);
  SelectObject(dc, op);
  GetWindowTextA(di->hwndItem, txt, sizeof(txt));
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, text);
  DrawTextA(dc, txt, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  SelectObject(dc, of);
  DeleteObject(fb);
  DeleteObject(pen);
}

/* The View button: a menu to show a part of the Debugger, or one view of the VDP, Sound or CPU Viewer. */
static void db_view_menu(void)
{
  static const int pane_of[3] = { DBGPANE_VDP, DBGPANE_SOUND, DBGPANE_CPUVIEW };
  static const int base_of[3] = { DBM_VDP0, DBM_SND0, DBM_CPUV0 };
  static const char *const name_of[3] = { "VDP", "Sound", "CPU" };
  HMENU menu = CreatePopupMenu(), subs[3];
  RECT r;
  int i, k, n, cur, cmd;

  AppendMenuA(menu, MF_STRING, DBM_MEMORY, "Memory");
  if (db_sel == DBGPANE_MEMORY) CheckMenuRadioItem(menu, DBM_MEMORY, DBM_MEMORY, DBM_MEMORY, MF_BYCOMMAND);       /* (one choice of several: a dot, not a check mark) */
  AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
  for (i = 0; i < 3; i++)
  {
    subs[i] = CreatePopupMenu();
    n = debugwin_view_count(i);
    cur = debugwin_view_current(i);
    for (k = 0; k < n; k++)
    {
      AppendMenuA(subs[i], MF_STRING, (UINT_PTR)(base_of[i] + k), debugwin_view_name(i, k));
      if (k == cur && db_sel == pane_of[i]) CheckMenuRadioItem(subs[i], (UINT)(base_of[i] + k), (UINT)(base_of[i] + k), (UINT)(base_of[i] + k), MF_BYCOMMAND);
    }
    AppendMenuA(menu, MF_POPUP, (UINT_PTR)subs[i], name_of[i]);
  }
  AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
  AppendMenuA(menu, MF_STRING, DBM_SEARCH, "RAM Search");
  AppendMenuA(menu, MF_STRING, DBM_WATCH, "RAM Watch");
  if (db_sel == DBGPANE_SEARCH) CheckMenuRadioItem(menu, DBM_SEARCH, DBM_SEARCH, DBM_SEARCH, MF_BYCOMMAND);
  if (db_sel == DBGPANE_WATCH) CheckMenuRadioItem(menu, DBM_WATCH, DBM_WATCH, DBM_WATCH, MF_BYCOMMAND);
  if (theme_menus_owner()) theme_ownerdraw_menu(menu);
  GetWindowRect(db_view, &r);
  cmd = gui_popup(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom);
  DestroyMenu(menu);
  if (cmd == DBM_MEMORY) db_select(DBGPANE_MEMORY);
  else if (cmd == DBM_SEARCH) db_select(DBGPANE_SEARCH);
  else if (cmd == DBM_WATCH) db_select(DBGPANE_WATCH);
  else
    for (i = 0; i < 3; i++)
      if (cmd >= base_of[i] && cmd < base_of[i] + 100) { db_select(pane_of[i]); debugwin_view_select(i, cmd - base_of[i]); }
}

static LRESULT CALLBACK db_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
  switch (m)
  {
    case WM_CREATE:
    {
      db_win = h;
      settings_register_classes();
      db_tab = CreateWindowExA(0, "GPSettingsTabs", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS, 0, 0, 10, 10, h, (HMENU)(INT_PTR)ID_TABS, g_inst, NULL);
      if (db_tab) { settings_set_tabs(db_tab, db_tab_names, DBGPANE_CPUDBG); SetWindowLongPtr(db_tab, GWLP_USERDATA, 0); }
      db_view = CreateWindowExA(0, "BUTTON", "View", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 10, 10, h, (HMENU)(INT_PTR)ID_VIEW, g_inst, NULL);
      if (db_view) SendMessage(db_view, WM_SETFONT, (WPARAM)gui_get_ui_font(), TRUE);
      theme_apply_to_window(h);
      return 0;
    }

    case WM_ACTIVATE:                                   /* (remember where the focus was: it is given back on return) */
      if (LOWORD(wp) == WA_INACTIVE) { HWND f = GetFocus(); db_lastfocus = (f && f != h && IsChild(h, f)) ? f : NULL; }
      return 0;

    case WM_SETFOCUS:                                   /* the focus goes on to what had it, else the part shown, never stays on the View button */
      if (db_lastfocus && IsWindow(db_lastfocus) && IsChild(h, db_lastfocus) && db_lastfocus != db_view) { SetFocus(db_lastfocus); return 0; }
      if (db_sel >= 0 && db_sel < DBGPANE_CPUDBG && db_pane[db_sel] && IsWindow(db_pane[db_sel])) SetFocus(db_pane[db_sel]);
      return 0;

    case WM_GETFONT:                                    /* the tab strip draws in its parent's font */
      return (LRESULT)gui_get_ui_font();

    case WM_COMMAND:
      if (LOWORD(wp) == ID_TABS && HIWORD(wp) == GP_TABS_CHANGED) db_select((int)GetWindowLongPtr(db_tab, GWLP_USERDATA));
      else if (LOWORD(wp) == ID_VIEW && HIWORD(wp) == BN_CLICKED) { db_view_menu(); SetFocus(h); }   /* (the button keeps no focus outline) */
      return 0;

    case WM_MEASUREITEM:                                /* the owner-drawn items of the View menu */
      if (theme_measure_menu_ownerdraw(lp)) return TRUE;
      break;

    case WM_DRAWITEM:
      if (((DRAWITEMSTRUCT *)lp)->CtlType == ODT_BUTTON && ((DRAWITEMSTRUCT *)lp)->CtlID == ID_VIEW) { db_draw_view((DRAWITEMSTRUCT *)lp); return TRUE; }
      if (theme_draw_menu_item(lp)) return TRUE;
      break;

    case WM_SIZE:
      if (wp != SIZE_MINIMIZED) db_layout();
      return 0;

    case WM_GETMINMAXINFO:
    {
      MINMAXINFO *mi = (MINMAXINFO *)lp;

      mi->ptMinTrackSize.x = S(720);
      mi->ptMinTrackSize.y = S(520);
      return 0;
    }

    case WM_ERASEBKGND:
    {
      RECT rc;
      HBRUSH b = theme_ctlcolor((HDC)wp);

      GetClientRect(h, &rc);
      FillRect((HDC)wp, &rc, b ? b : GetSysColorBrush(COLOR_BTNFACE));
      return 1;
    }

    case WM_SETCURSOR:
    {
      POINT p;
      HWND cpu = db_pane[DBGPANE_CPUDBG];

      GetCursorPos(&p);
      ScreenToClient(h, &p);
      if (LOWORD(lp) == HTCLIENT && cpu && IsWindow(cpu) && p.y >= DB_BAR + db_split && p.y < DB_BAR + db_split + S(6)) { SetCursor(LoadCursor(NULL, IDC_SIZENS)); return TRUE; }
      break;
    }

    case WM_LBUTTONDOWN:
    {
      int y = (short)HIWORD(lp);
      HWND cpu = db_pane[DBGPANE_CPUDBG];

      if (cpu && IsWindow(cpu) && y >= DB_BAR + db_split && y < DB_BAR + db_split + S(6)) { db_drag = 1; SetCapture(h); }
      return 0;
    }

    case WM_MOUSEMOVE:
      if (db_drag)
      {
        db_split = (short)HIWORD(lp) - DB_BAR - S(3);
        db_layout();
      }
      return 0;

    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
      if (db_drag) { db_drag = 0; if (GetCapture() == h) ReleaseCapture(); }
      return 0;

    case WM_CLOSE:
      dbgcpu_release();                                   /* (a game stopped at a breakpoint goes on; the breakpoints stay) */
      DestroyWindow(h);
      return 0;

    case WM_SYSCOMMAND:
      if ((wp & 0xFFF0) == SC_CLOSE) { SendMessage(h, WM_CLOSE, 0, 0); return 0; }
      if ((wp & 0xFFF0) == SC_KEYMENU)                    /* Alt or F10 (and Alt+V) open the View menu */
      {
        if (lp == 0 || lp == 'v' || lp == 'V') { db_view_menu(); SetFocus(h); }
        return 0;
      }
      break;

    case WM_DESTROY:
    {
      int i;

      for (i = 0; i < DBGPANE_COUNT; i++) db_pane[i] = NULL;
      db_win = NULL;
      db_tab = NULL;
      db_view = NULL;
      db_lastfocus = NULL;
      db_drag = 0;
      return 0;
    }
  }
  return DefWindowProc(h, m, wp, lp);
}

static int db_create(HWND parent)
{
  static int registered;

  if (!registered)
  {
    WNDCLASSA wc;

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = db_proc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = DB_CLASS; i18n_keep_ltr(DB_CLASS);
    if (!RegisterClassA(&wc)) return 0;
    registered = 1;
  }
  db_split = 0;
  db_sel = DBGPANE_MEMORY;
  (void)parent;                       /* no owner: the window has its own button on the taskbar and minimizes to it */
  CreateWindowExA(WS_EX_APPWINDOW, DB_CLASS, "Debugger", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, S(1040), S(760), NULL, NULL, g_inst, NULL);
  if (!db_win) return 0;
  if (db_cpu_ok()) db_make_pane(DBGPANE_CPUDBG);                  /* (nothing is said for a game without a 68000: the tabs are still there) */
  gui_center_window(db_win);
  gui_window_icon(db_win);
  return 1;
}

/* Opens the window (if it is not open) and shows the pane asked for. */
void debugger_open(HWND parent, int pane)
{
  int fresh = 0;

  if (!db_win)
  {
    if (!db_create(parent)) return;
    fresh = 1;
  }
  if (pane == DBGPANE_CPUDBG)
  {
    if (!db_pane[DBGPANE_CPUDBG] || !IsWindow(db_pane[DBGPANE_CPUDBG])) db_make_pane(DBGPANE_CPUDBG);      /* (says why when the game has no 68000) */
    if (fresh) db_select(DBGPANE_MEMORY);
    else db_layout();
  }
  else if (pane < 0) { if (fresh) db_select(DBGPANE_MEMORY); }          /* just the window (Tools > Debugger) */
  else db_select(pane);
  if (fresh) ShowWindow(db_win, SW_SHOW);
  else
  {
    if (IsIconic(db_win)) ShowWindow(db_win, SW_RESTORE);
    SetForegroundWindow(db_win);
  }
  if (pane == DBGPANE_CPUDBG && db_pane[DBGPANE_CPUDBG] && IsWindow(db_pane[DBGPANE_CPUDBG])) SetFocus(db_pane[DBGPANE_CPUDBG]);
}

/* A pane made without showing it (the watch list for "send to RAM Watch"). */
void debugger_ensure(int pane)
{
  if (!db_win) { debugger_open(g_hwnd, pane); return; }
  if (pane >= 0 && pane < DBGPANE_CPUDBG) { db_make_pane(pane); db_layout(); }
}

void debugger_close(void)
{
  if (db_win) { dbgcpu_close(); DestroyWindow(db_win); }
}

void debugger_enable(int on)
{
  if (db_win) EnableWindow(db_win, on ? TRUE : FALSE);
}

int debugger_owns(HWND h)
{
  return h && db_win == h;
}
