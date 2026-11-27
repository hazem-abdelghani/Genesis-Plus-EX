/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *  Copyright (c) 2026 Hazem Abdelghani
 *
 *  scrollfit.c -- a window whose controls do not fit (a high screen resolution, a large text size or scaling, a small screen) gets scroll
 *  bars, as a way out: gui_fit_window() makes a window no bigger than the screen's work area and, for a window of a fixed size, lets its
 *  client area scroll (vertical and horizontal) when controls lie outside it. Nothing changes for a window that fits.
 *
 *  A window of a fixed size lays its controls out once, so they are moved with the scroll bars (ScrollWindowEx). A window that can be
 *  resized lays itself out again at every size, so it is only kept inside the work area.
 ****************************************************************************/

#include <windows.h>
#include <commctrl.h>

#include "shared.h"
#include <uxtheme.h>

#include "gui.h"
#include "theme.h"

#define SF_ID     0x5C01          /* the subclass id and the timer id */
#define SF_TICK   250             /* ms: the controls are measured again (a tab was changed, a control was shown) and the control with the focus is brought into view */
#define SF_SLACK  3               /* px: a control this far outside is not worth a scroll bar */

typedef struct
{
  int x, y;                       /* how far the content is scrolled */
  int cw, ch;                     /* the size of the content (at 0, 0) */
  int vis_v, vis_h;               /* the scroll bars that are shown */
  int busy, logged;
  int area, min_h;                /* a part of a window (a page of Settings), not a window: its own painting is told how far it is scrolled, and it is repainted whole */
  int last_cw, last_ch, last_w, last_h;
  HWND last_focus;                /* the control that was last brought into view: it is done once for each control that takes the focus */
} sf_t;

static void sf_measure(HWND h, const sf_t *s, int *right, int *bottom)
{
  HWND c;

  *right = *bottom = 0;
  for (c = GetWindow(h, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
  {
    RECT r;

    if (!(GetWindowLongA(c, GWL_STYLE) & WS_VISIBLE)) continue;
    if (!GetWindowRect(c, &r)) continue;
    MapWindowPoints(NULL, h, (POINT *)&r, 2);
    r.right += s->x;
    r.bottom += s->y;
    if (r.right > *right) *right = r.right;
    if (r.bottom > *bottom) *bottom = r.bottom;
  }
}

static void sf_scroll_to(HWND h, sf_t *s, int bar, int pos)
{
  SCROLLINFO si;
  int old = bar == SB_HORZ ? s->x : s->y, maxpos, delta;

  ZeroMemory(&si, sizeof(si));
  si.cbSize = sizeof(si);
  si.fMask = SIF_RANGE | SIF_PAGE;
  if (!GetScrollInfo(h, bar, &si)) return;
  maxpos = si.nMax - (int)si.nPage + 1;
  if (pos > maxpos) pos = maxpos;
  if (pos < 0) pos = 0;
  delta = old - pos;
  if (!delta) return;
  if (bar == SB_HORZ) s->x = pos; else s->y = pos;
  SetScrollPos(h, bar, pos, TRUE);
  SetPropA(h, "gpex_sx", (HANDLE)(INT_PTR)s->x);
  SetPropA(h, "gpex_sy", (HANDLE)(INT_PTR)s->y);
  ScrollWindowEx(h, bar == SB_HORZ ? delta : 0, bar == SB_VERT ? delta : 0, NULL, NULL, NULL, NULL, SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
  if (s->area) InvalidateRect(h, NULL, TRUE);
  /* Painted now, not left waiting: while the thumb is dragged (or the wheel turned) the next scroll message always comes before the lowest-priority WM_PAINT, so the
     controls stayed erased (a page of blanks and empty frames) until the scrolling stopped. */
  RedrawWindow(h, NULL, NULL, RDW_UPDATENOW | RDW_ALLCHILDREN);
}

/* Measures the controls again and shows or hides the scroll bars. */
static void sf_update(HWND h, sf_t *s)
{
  int pass;
  RECT cr;
  int right, bottom;

  if (s->busy) return;
  s->busy = 1;
  for (pass = 0; pass < 2; pass++)                          /* (a scroll bar takes room from the other direction) */
  {
    int needv, needh;
    SCROLLINFO si;

    GetClientRect(h, &cr);
    sf_measure(h, s, &right, &bottom);
    s->cw = right + 8;
    s->ch = bottom + 8;
    if (s->min_h > s->ch) { s->ch = s->min_h; bottom = s->ch - 8; }       /* (what the page paints itself, beyond its controls) */
    needv = bottom > cr.bottom + SF_SLACK;
    needh = right > cr.right + SF_SLACK;
    ZeroMemory(&si, sizeof(si));
    si.cbSize = sizeof(si);
    if (needv)
    {
      si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
      si.nMin = 0; si.nMax = s->ch - 1; si.nPage = (UINT)cr.bottom; si.nPos = s->y;
      SetScrollInfo(h, SB_VERT, &si, TRUE);
      s->vis_v = 1;
    }
    else if (s->vis_v)
    {
      if (s->y) { ScrollWindowEx(h, 0, s->y, NULL, NULL, NULL, NULL, SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE); s->y = 0; SetPropA(h, "gpex_sy", (HANDLE)0); if (s->area) InvalidateRect(h, NULL, TRUE); }
      ShowScrollBar(h, SB_VERT, FALSE);
      s->vis_v = 0;
    }
    if (needh)
    {
      si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
      si.nMin = 0; si.nMax = s->cw - 1; si.nPage = (UINT)cr.right; si.nPos = s->x;
      SetScrollInfo(h, SB_HORZ, &si, TRUE);
      s->vis_h = 1;
    }
    else if (s->vis_h)
    {
      if (s->x) { ScrollWindowEx(h, s->x, 0, NULL, NULL, NULL, NULL, SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE); s->x = 0; SetPropA(h, "gpex_sx", (HANDLE)0); if (s->area) InvalidateRect(h, NULL, TRUE); }
      ShowScrollBar(h, SB_HORZ, FALSE);
      s->vis_h = 0;
    }
  }
  GetClientRect(h, &cr);
  if ((s->vis_v || s->vis_h) && !s->logged)
  {
    s->logged = 1;
    gui_log("Window: controls reach %dx%d, the window's client area is %dx%d: scroll bars shown", s->cw, s->ch, (int)cr.right, (int)cr.bottom);
  }
  s->last_cw = s->cw; s->last_ch = s->ch; s->last_w = cr.right; s->last_h = cr.bottom;
  s->busy = 0;
}

static void sf_bar_msg(HWND h, sf_t *s, int bar, WPARAM wp)
{
  SCROLLINFO si;
  int pos, line = 20;

  ZeroMemory(&si, sizeof(si));
  si.cbSize = sizeof(si);
  si.fMask = SIF_ALL;
  if (!GetScrollInfo(h, bar, &si)) return;
  pos = si.nPos;
  switch (LOWORD(wp))
  {
    case SB_LINEUP:     pos -= line; break;
    case SB_LINEDOWN:   pos += line; break;
    case SB_PAGEUP:     pos -= (int)si.nPage - line; break;
    case SB_PAGEDOWN:   pos += (int)si.nPage - line; break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION: pos = HIWORD(wp); break;          /* (the colored themes' bar of our own sends the position this way too) */
    case SB_TOP:        pos = 0; break;
    case SB_BOTTOM:     pos = si.nMax; break;
    default: return;
  }
  sf_scroll_to(h, s, bar, pos);
}

/* The control with the focus is brought into view (the Tab key moves it to a control that is scrolled out). Once for each control that
   takes the focus: done at every tick it pulled the window back after a scroll, and a control wider than the window (the tabs of the Controls
   window) was scrolled right and then left again, over and over: the window wiggled. */
static void sf_show_focus(HWND h, sf_t *s)
{
  HWND f = GetFocus();
  RECT fr, cr;

  if (!f || f == h || !IsChild(h, f) || (!s->vis_v && !s->vis_h)) return;
  while (GetParent(f) && GetParent(f) != h) f = GetParent(f);       /* (the control of this window that holds it) */
  if (f == s->last_focus) return;
  s->last_focus = f;
  if (!GetWindowRect(f, &fr)) return;
  MapWindowPoints(NULL, h, (POINT *)&fr, 2);
  GetClientRect(h, &cr);
  if (s->vis_v)
  {
    if (fr.bottom - fr.top > cr.bottom) { if (fr.top < 0) sf_scroll_to(h, s, SB_VERT, s->y + fr.top - 8); }        /* (taller than the window: its top edge) */
    else if (fr.bottom > cr.bottom) sf_scroll_to(h, s, SB_VERT, s->y + (fr.bottom - cr.bottom) + 8);
    else if (fr.top < 0) sf_scroll_to(h, s, SB_VERT, s->y + fr.top - 8);
  }
  if (s->vis_h)
  {
    if (fr.right - fr.left > cr.right) { if (fr.left < 0) sf_scroll_to(h, s, SB_HORZ, s->x + fr.left - 8); }        /* (wider than the window: its left edge) */
    else if (fr.right > cr.right) sf_scroll_to(h, s, SB_HORZ, s->x + (fr.right - cr.right) + 8);
    else if (fr.left < 0) sf_scroll_to(h, s, SB_HORZ, s->x + fr.left - 8);
  }
}

static LRESULT CALLBACK sf_proc(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
  sf_t *s = (sf_t *)ref;
  LRESULT r;

  (void)id;
  switch (m)
  {
    case WM_SIZE:
      r = DefSubclassProc(h, m, wp, lp);
      if (wp != SIZE_MINIMIZED) sf_update(h, s);
      return r;
    case WM_VSCROLL:
      if (s->vis_v) { sf_bar_msg(h, s, SB_VERT, wp); return 0; }
      break;
    case WM_HSCROLL:
      if (s->vis_h) { sf_bar_msg(h, s, SB_HORZ, wp); return 0; }
      break;
    case WM_MOUSEWHEEL:
      if (s->vis_v)
      {
        sf_scroll_to(h, s, SB_VERT, s->y - (short)HIWORD(wp) * 60 / WHEEL_DELTA);
        return 0;
      }
      break;
    case WM_MOUSEHWHEEL:
      if (s->vis_h)
      {
        sf_scroll_to(h, s, SB_HORZ, s->x + (short)HIWORD(wp) * 60 / WHEEL_DELTA);
        return 0;
      }
      break;
    case WM_TIMER:
      if (wp == SF_ID)
      {
        RECT cr;
        int right, bottom;

        if (s->busy || IsIconic(h) || !IsWindowVisible(h)) return 0;
        if (s->area && IsIconic(GetAncestor(h, GA_ROOT))) return 0;
        GetClientRect(h, &cr);
        sf_measure(h, s, &right, &bottom);
        if (s->min_h > bottom + 8) bottom = s->min_h - 8;
        if (right + 8 != s->last_cw || bottom + 8 != s->last_ch || cr.right != s->last_w || cr.bottom != s->last_h) sf_update(h, s);
        sf_show_focus(h, s);
        return 0;
      }
      break;
    case WM_NCDESTROY:
      KillTimer(h, SF_ID);
      RemovePropA(h, "gpex_scrollarea");
      RemovePropA(h, "gpex_sx");
      RemovePropA(h, "gpex_sy");
      RemoveWindowSubclass(h, sf_proc, SF_ID);
      free(s);
      break;
  }
  return DefSubclassProc(h, m, wp, lp);
}

void gui_fit_window(HWND w)
{
  MONITORINFO mi;
  RECT r;
  DWORD style;
  int ww, hh, maxw, maxh;
  sf_t *s;
  DWORD_PTR dummy;

  if (!w || !IsWindow(w)) return;
  mi.cbSize = sizeof(mi);
  if (!GetWindowRect(w, &r) || !GetMonitorInfo(MonitorFromWindow(g_hwnd ? g_hwnd : w, MONITOR_DEFAULTTONEAREST), &mi)) return;
  maxw = mi.rcWork.right - mi.rcWork.left;
  maxh = mi.rcWork.bottom - mi.rcWork.top;
  ww = r.right - r.left;
  hh = r.bottom - r.top;
  if (ww > maxw || hh > maxh)
  {
    gui_log("Window %dx%d does not fit the screen's work area %dx%d: made smaller", ww, hh, maxw, maxh);
    SetWindowPos(w, NULL, 0, 0, ww > maxw ? maxw : ww, hh > maxh ? maxh : hh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
  style = (DWORD)GetWindowLongA(w, GWL_STYLE);
  if (style & (WS_THICKFRAME | WS_CHILD)) return;                      /* a window that can be resized lays itself out again */
  if (GetWindowSubclass(w, sf_proc, SF_ID, &dummy)) return;
  s = (sf_t *)calloc(1, sizeof(sf_t));
  if (!s) return;
  if (!SetWindowSubclass(w, sf_proc, SF_ID, (DWORD_PTR)s)) { free(s); return; }
  SetTimer(w, SF_ID, SF_TICK, NULL);
  sf_update(w, s);
}

/* A part of a window that scrolls on its own (a page of Settings, which holds more rows than the window has room for when the screen is small):
   its controls are moved with the scroll bars, and the page asks gui_scroll_x / gui_scroll_y how far it is scrolled when it paints
   itself. min_h: the height of what the page needs (its controls and what it draws), in its own coordinates. */
void gui_scroll_area(HWND area, int min_h)
{
  sf_t *s;
  DWORD_PTR dummy;

  if (!area || !IsWindow(area) || GetWindowSubclass(area, sf_proc, SF_ID, &dummy)) return;
  s = (sf_t *)calloc(1, sizeof(sf_t));
  if (!s) return;
  s->area = 1;
  s->min_h = min_h;
  if (theme_is_dark()) SetWindowTheme(area, L"DarkMode_Explorer", NULL);          /* (the plain dark theme: the system's own dark bar) */
  SetPropA(area, "gpex_scrollarea", (HANDLE)1);          /* (the colored themes lay their own scroll bar over it: theme.c) */
  if (!SetWindowSubclass(area, sf_proc, SF_ID, (DWORD_PTR)s)) { free(s); return; }
  SetTimer(area, SF_ID, SF_TICK, NULL);
  sf_update(area, s);
}

int gui_scroll_x(HWND w)
{
  return (int)(INT_PTR)GetPropA(w, "gpex_sx");
}

int gui_scroll_y(HWND w)
{
  return (int)(INT_PTR)GetPropA(w, "gpex_sy");
}
