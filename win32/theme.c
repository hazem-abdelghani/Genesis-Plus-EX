/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  theme.c -- see theme.h for the overall approach and the caveats
 *  around the undocumented uxtheme.dll entry points this leans on.
 ****************************************************************************/

#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <dwmapi.h>

#include "shared.h"
#include "gui.h"
#include "resource.h"
#include "theme.h"
#include "toolbar.h"

/* Not in every SDK's dwmapi.h yet -- just an integer constant, safe to
   supply if missing. 20 is the value used since Windows 10 20H1; 19 was
   used briefly before that in early 1809-era builds. Both are set below;
   whichever one the running Windows version doesn't recognize is simply
   ignored (DwmSetWindowAttribute fails harmlessly for it). */
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#define DWMWA_USE_IMMERSIVE_DARK_MODE_PRE20H1 19

/* uxtheme.dll ordinals -- these have no exported names, so GetProcAddress
   only works by number. Stable since Windows 10 1809, per the
   "win32-darkmode" reverse-engineering project; see theme.h for what
   happens if a future Windows update ever changes them. */
typedef enum { AppMode_Default, AppMode_AllowDark, AppMode_ForceDark, AppMode_ForceLight, AppMode_Max } PreferredAppMode;

typedef void             (WINAPI *fn_RefreshImmersiveColorPolicyState)(void);
typedef BOOL              (WINAPI *fn_AllowDarkModeForWindow)(HWND, BOOL);
typedef PreferredAppMode (WINAPI *fn_SetPreferredAppMode)(PreferredAppMode);
typedef void             (WINAPI *fn_FlushMenuThemes)(void);

static fn_RefreshImmersiveColorPolicyState pRefreshImmersiveColorPolicyState;
static fn_AllowDarkModeForWindow           pAllowDarkModeForWindow;
static fn_SetPreferredAppMode              pSetPreferredAppMode;
static fn_FlushMenuThemes                  pFlushMenuThemes;

static int system_dark;   /* last-read system preference, kept fresh via
                              theme_handle_settingchange() */

static int read_system_dark(void)
{
  HKEY key;
  DWORD value = 1, size = sizeof(value);

  /* Missing key/value (older Windows, or a very locked-down install)
     defaults to light -- matches what Windows itself falls back to. */
  if (RegOpenKeyExA(HKEY_CURRENT_USER,
        "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        0, KEY_READ, &key) == ERROR_SUCCESS)
  {
    DWORD type;
    if (RegQueryValueExA(key, "AppsUseLightTheme", NULL, &type, (BYTE *)&value, &size) != ERROR_SUCCESS)
      value = 1;
    RegCloseKey(key);
  }
  return value == 0;   /* 0 = dark, 1 = light */
}

void theme_init(void)
{
  HMODULE hux;

  system_dark = read_system_dark();

  hux = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!hux) return;   /* no dark mode support on this Windows version */

  pSetPreferredAppMode              = (fn_SetPreferredAppMode)(void *)GetProcAddress(hux, MAKEINTRESOURCEA(135));
  pAllowDarkModeForWindow           = (fn_AllowDarkModeForWindow)(void *)GetProcAddress(hux, MAKEINTRESOURCEA(133));
  pFlushMenuThemes                  = (fn_FlushMenuThemes)(void *)GetProcAddress(hux, MAKEINTRESOURCEA(136));
  pRefreshImmersiveColorPolicyState = (fn_RefreshImmersiveColorPolicyState)(void *)GetProcAddress(hux, MAKEINTRESOURCEA(104));

  if (pSetPreferredAppMode)
  {
    pSetPreferredAppMode(theme_is_dark() ? AppMode_ForceDark : AppMode_ForceLight);
    if (pRefreshImmersiveColorPolicyState) pRefreshImmersiveColorPolicyState();
  }
}

void theme_set_mode(int mode)
{
  gui.theme_mode = mode;
}

int theme_get_mode(void)
{
  return gui.theme_mode;
}

int theme_is_dark(void)
{
  if (gui.theme_mode == THEME_LIGHT) return 0;
  if (gui.theme_mode == THEME_DARK)  return 1;
  return system_dark;
}

/* A more direct fix than SB_SETBKCOLOR, which turned out not to
   actually change the rendered background despite being the documented
   mechanism for this -- painting it directly here bypasses whatever
   about that message chain was overriding the color. Text color is
   still handled separately by theme_statusbar_customdraw(). */
/* A disabled plain-text (STATIC) control: DefWindowProc's own painting for
   the disabled state overlays a highlight-colored copy of the text offset by
   one pixel on top of whatever color WM_CTLCOLORSTATIC provided (a classic-
   Windows "engraved" look, meant for a light background) -- confirmed by
   direct testing, on a dark background that highlight copy shows up as a
   pale ghost/halo around the text rather than blending in. Painting the
   control ourselves for just that one state, in a single flat color, avoids
   it entirely; the enabled state is untouched and still goes through the
   normal WM_CTLCOLORSTATIC path. */
static LRESULT CALLBACK static_text_subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                                   UINT_PTR subclass_id, DWORD_PTR ref_data)
{
  (void)subclass_id;
  (void)ref_data;

  if (msg == WM_PAINT && theme_is_dark() && !IsWindowEnabled(hwnd) &&
      ((GetWindowLongPtr(hwnd, GWL_STYLE) & SS_TYPEMASK) <= SS_RIGHT ||       /* text only: an icon or a picture is left to the control */
       (GetWindowLongPtr(hwnd, GWL_STYLE) & SS_TYPEMASK) == SS_SIMPLE ||
       (GetWindowLongPtr(hwnd, GWL_STYLE) & SS_TYPEMASK) == SS_LEFTNOWORDWRAP))
  {
    PAINTSTRUCT ps;
    HDC hdc;
    RECT r;
    char text[256];
    HFONT font, old_font;
    HBRUSH br;
    LONG style;
    UINT align = DT_LEFT;

    hdc = BeginPaint(hwnd, &ps);

    GetClientRect(hwnd, &r);
    br = CreateSolidBrush(THEME_DARK_BG);   /* the plain dialog background this control sits on */
    FillRect(hdc, &r, br);
    DeleteObject(br);

    style = GetWindowLongPtr(hwnd, GWL_STYLE);
    if ((style & SS_TYPEMASK) == SS_CENTER)     align = DT_CENTER;
    else if ((style & SS_TYPEMASK) == SS_RIGHT) align = DT_RIGHT;

    font = (HFONT)SendMessage(hwnd, WM_GETFONT, 0, 0);
    old_font = (HFONT)SelectObject(hdc, font ? font : GetStockObject(DEFAULT_GUI_FONT));

    GetWindowTextA(hwnd, text, sizeof(text));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, THEME_DARK_MUTED);   /* flat disabled gray, the same one used elsewhere for disabled menu text */
    {
      RECT calc = r;
      TEXTMETRICA tm;
      GetTextMetricsA(hdc, &tm);
      UINT hide = (SendMessage(hwnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEACCEL) ? DT_HIDEPREFIX : 0;    /* the underline of a & shows only once Alt is pressed */
      DrawTextA(hdc, text, -1, &calc, align | hide | DT_WORDBREAK | DT_CALCRECT);
      if (calc.bottom - calc.top > tm.tmHeight * 3 / 2)        /* more than one line: wrapped, from the top, like the control itself */
        DrawTextA(hdc, text, -1, &r, align | hide | DT_WORDBREAK);
      else
        DrawTextA(hdc, text, -1, &r, align | hide | DT_VCENTER | DT_SINGLELINE);
    }

    SelectObject(hdc, old_font);
    EndPaint(hwnd, &ps);
    return 0;
  }

  if (msg == WM_ENABLE)
  {
    /* Repaint right away rather than waiting for the next unrelated
       invalidate -- the control's look needs to change the moment its
       enabled state does (e.g. switching the Audio Filter dropdown). */
    InvalidateRect(hwnd, NULL, TRUE);
  }

  if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, static_text_subclass_proc, subclass_id);

  return DefSubclassProc(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK statusbar_subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                                 UINT_PTR subclass_id, DWORD_PTR ref_data)
{
  (void)subclass_id;
  (void)ref_data;

  if (msg == WM_ERASEBKGND && theme_is_dark())
  {
    return 1;   /* suppress -- WM_PAINT below draws the background itself */
  }

  if (msg == WM_PAINT && theme_is_dark())
  {
    PAINTSTRUCT ps;
    HDC hdc;
    RECT r;
    int parts[16];
    int n_parts;
    int i, left;
    HBRUSH br;
    HFONT font, old_font;

    hdc = BeginPaint(hwnd, &ps);

    GetClientRect(hwnd, &r);
    br = CreateSolidBrush(THEME_DARK_BG);
    FillRect(hdc, &r, br);
    DeleteObject(br);

    n_parts = (int)SendMessage(hwnd, SB_GETPARTS, 0, 0);
    if (n_parts > 16) n_parts = 16;
    if (n_parts < 1) n_parts = 1;
    SendMessage(hwnd, SB_GETPARTS, (WPARAM)n_parts, (LPARAM)parts);

    font = (HFONT)SendMessage(hwnd, WM_GETFONT, 0, 0);
    old_font = font ? (HFONT)SelectObject(hdc, font) : NULL;

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, THEME_DARK_TEXT);

    left = 0;
    for (i = 0; i < n_parts; i++)
    {
      char text[256];
      RECT part_rect;
      int right = parts[i];

      text[0] = '\0';
      SendMessageA(hwnd, SB_GETTEXTA, (WPARAM)i, (LPARAM)text);

      part_rect.left   = left + 4;
      part_rect.top    = r.top;
      part_rect.right  = (right == -1) ? r.right : right;
      part_rect.bottom = r.bottom;

      DrawTextA(hdc, text, -1, &part_rect, DT_VCENTER | DT_SINGLELINE | DT_LEFT);

      left = right;
    }

    if (old_font) SelectObject(hdc, old_font);

    EndPaint(hwnd, &ps);
    return 0;
  }

  return DefSubclassProc(hwnd, msg, wp, lp);
}

/* A ListView's column-header row is a child window, and it sends its
   custom-draw notifications to ITS parent -- the ListView itself. The ListView
   does not pass them on to its own parent, so the WM_NOTIFY handlers in the
   main window and in dialogs never see them and could not recolor the header
   text. Subclassing the ListView is what lets us receive them where they
   actually arrive; the color itself is still set by theme_header_customdraw(). */
static LRESULT CALLBACK listview_header_subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                                      UINT_PTR id, DWORD_PTR ref)
{
  (void)ref;

  if (msg == WM_NOTIFY)
  {
    LRESULT r = theme_header_customdraw((NMHDR *)lp);
    if (r != -1) return r;
  }
  else if (msg == WM_NCDESTROY)
  {
    RemoveWindowSubclass(hwnd, listview_header_subclass_proc, id);
  }

  return DefSubclassProc(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK header_paint_subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref);
static void draw_header_cell(HDC dc, HWND header, int idx, const RECT *prc);

static LRESULT CALLBACK listview_grid_subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
  LRESULT r;

  (void)ref;
  if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, listview_grid_subclass_proc, id);
  r = DefSubclassProc(hwnd, msg, wp, lp);

  if (msg == WM_PAINT && ListView_GetView(hwnd) == LV_VIEW_DETAILS)
  {
    HDC dc = GetDC(hwnd);

    if (dc)
    {
      RECT cr;
      HWND header = ListView_GetHeader(hwnd);
      int hh = 0, i, n, cols;
      HPEN pen = CreatePen(PS_SOLID, 1, theme_is_dark() ? RGB(0x3A, 0x3A, 0x3A) : RGB(0xDD, 0xDD, 0xDD));
      HGDIOBJ op = SelectObject(dc, pen);

      GetClientRect(hwnd, &cr);
      if (header && IsWindowVisible(header))
      {
        RECT hr;
        GetWindowRect(header, &hr);
        hh = hr.bottom - hr.top;
      }
      IntersectClipRect(dc, 0, hh, cr.right, cr.bottom);

      cols = header ? Header_GetItemCount(header) : 0;
      for (i = 0; i + 1 < cols; i++)             /* between the columns */
      {
        RECT ir;
        if (Header_GetItemRect(header, i, &ir))
        {
          MapWindowPoints(header, hwnd, (POINT *)&ir, 2);
          MoveToEx(dc, ir.right - 1, hh, NULL);
          LineTo(dc, ir.right - 1, cr.bottom);
        }
      }

      if (gui.browser_gridlines)                 /* under the rows */
      {
        int top = ListView_GetTopIndex(hwnd), per = ListView_GetCountPerPage(hwnd);
        n = ListView_GetItemCount(hwnd);
        for (i = top; i < n && i <= top + per + 1; i++)
        {
          RECT ir;
          if (ListView_GetItemRect(hwnd, i, &ir, LVIR_BOUNDS))
          {
            MoveToEx(dc, 0, ir.bottom - 1, NULL);
            LineTo(dc, cr.right, ir.bottom - 1);
          }
        }
      }

      SelectObject(dc, op);
      DeleteObject(pen);
      ReleaseDC(hwnd, dc);
    }
  }
  return r;
}

/* The drop-down list of a combo box is made as wide as its longest entry, so no entry is cut short in the list. */
void theme_fit_dropdown(HWND combo)
{
  int n = (int)SendMessage(combo, CB_GETCOUNT, 0, 0), i, widest = 0, cur;
  HDC dc;
  HGDIOBJ of;
  RECT rc;

  if (n <= 0) return;
  dc = GetDC(combo);
  of = SelectObject(dc, (HFONT)SendMessage(combo, WM_GETFONT, 0, 0));
  for (i = 0; i < n; i++)
  {
    char text[256];
    SIZE sz;
    if (SendMessageA(combo, CB_GETLBTEXT, (WPARAM)i, (LPARAM)text) > 0 && GetTextExtentPoint32A(dc, text, lstrlenA(text), &sz) && sz.cx > widest)
      widest = sz.cx;
  }
  SelectObject(dc, of);
  ReleaseDC(combo, dc);
  GetWindowRect(combo, &rc);
  cur = rc.right - rc.left;
  widest += GetSystemMetrics(SM_CXVSCROLL) + 16;
  if (widest > cur) SendMessage(combo, CB_SETDROPPEDWIDTH, (WPARAM)widest, 0);
}

static int is_combobox(HWND w)
{
  char c[24];
  return GetClassNameA(w, c, sizeof(c)) && !lstrcmpiA(c, "ComboBox");
}

static void modern_button_hook(HWND hwnd);     /* the Modern style: flat rounded buttons, check boxes and radio buttons */
static void modern_combo_hook(HWND hwnd);      /* ... and drop-down lists */

static BOOL CALLBACK apply_to_child(HWND hwnd, LPARAM lparam)
{
  int dark = (int)lparam;
  char class_name[64];
  int skip_generic_theme;
  LONG_PTR ex_style;

  GetClassNameA(hwnd, class_name, sizeof(class_name));
  skip_generic_theme = (lstrcmpiA(class_name, "msctls_statusbar32") == 0);

  /* A plain list box keeps its own painting (WM_CTLCOLORLISTBOX) but its scroll bars follow the theme like every other control's. */
  if (lstrcmpiA(class_name, "ListBox") == 0) SetWindowTheme(hwnd, dark ? L"DarkMode_Explorer" : NULL, NULL);

  if (lstrcmpiA(class_name, "Button") == 0) modern_button_hook(hwnd);
  if (lstrcmpiA(class_name, "ComboBox") == 0) modern_combo_hook(hwnd);

  if (lstrcmpiA(class_name, "Static") == 0)
  {
    /* Plain LTEXT labels: see static_text_subclass_proc for what this
       fixes (a disabled label's own default painting, not anything
       WM_CTLCOLORSTATIC controls). Always subclassed; the fix only
       actually changes anything when the control is later disabled
       while dark mode is active. */
    SetWindowSubclass(hwnd, static_text_subclass_proc, 3, 0);
  }

  /* WS_EX_CLIENTEDGE draws a fixed, light-gray sunken 3D border that's
     part of the control's own non-client rendering -- separate from
     its interior (which WM_CTLCOLOREDIT/LISTBOX already colors
     correctly) and from anything SetWindowTheme touches, so it stayed
     its normal light color regardless of theme, showing as a thin but
     very visible pale sliver around the search box and ROM list. */
  ex_style = GetWindowLongPtrA(hwnd, GWL_EXSTYLE);
  if ((dark && (ex_style & WS_EX_CLIENTEDGE)) ||
      (!dark && !(ex_style & WS_EX_CLIENTEDGE) &&
       (lstrcmpiA(class_name, "Edit") == 0 || lstrcmpiA(class_name, "SysListView32") == 0) &&
       !(gui.ui_style && lstrcmpiA(class_name, "Edit") == 0 && GetParent(hwnd) && is_combobox(GetParent(hwnd)))))
  {
    RECT r;

    SetWindowLongPtrA(hwnd, GWL_EXSTYLE, dark ? (ex_style & ~WS_EX_CLIENTEDGE) : (ex_style | WS_EX_CLIENTEDGE));

    /* SWP_FRAMECHANGED alone, with no actual size change, wasn't
       enough to force the visual update at startup time -- the stale
       border only got redrawn after a genuine resize (e.g. maximize
       then restore). Nudging the width by a real pixel and back is
       the same technique already proven for the same kind of stale
       non-client-border problem on the ListView header elsewhere in
       this file, forcing a real geometry recalculation rather than
       just marking one as pending. */
    GetWindowRect(hwnd, &r);
    SetWindowPos(hwnd, NULL, 0, 0, (r.right - r.left) + 1, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    SetWindowPos(hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE | RDW_FRAME);
  }

  if (pAllowDarkModeForWindow) pAllowDarkModeForWindow(hwnd, dark ? TRUE : FALSE);

  if (lstrcmpiA(class_name, "SysHeader32") == 0)
  {
    /* A ListView's column-header row is its own child window, visited
       here independently of the SysListView32 branch below that also
       tries to theme it -- handled here instead, explicitly, so it
       always ends up with the theme a list header actually wants
       regardless of which of the two visits happens last. */
    RECT r;
    SetWindowTheme(hwnd, dark ? L"ItemsView" : NULL, NULL);
    SetWindowSubclass(hwnd, header_paint_subclass_proc, 5, 0);
    /* Column-divider lines are drawn as part of this control's own
       frame, not just its interior -- a plain invalidate left stale
       dark divider lines behind when switching back to light, so this
       forces a real geometry change (nudging the width by a pixel and
       back) to guarantee a full repaint, the same technique used
       elsewhere in this app for controls whose visual state got stuck
       through an ordinary invalidate. */
    GetWindowRect(hwnd, &r);
    SetWindowPos(hwnd, NULL, 0, 0, (r.right - r.left) + 1, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
    return TRUE;
  }

  if (lstrcmpiA(class_name, "ComboBox") == 0)
  {
    COMBOBOXINFO cbi;

    SetWindowTheme(hwnd, dark ? L"DarkMode_CFD" : NULL, NULL);
    theme_fit_dropdown(hwnd);

    /* The drop-down list is a window of its own; without this it stays white. */
    cbi.cbSize = sizeof(cbi);
    if (GetComboBoxInfo(hwnd, &cbi) && cbi.hwndList)
      SetWindowTheme(cbi.hwndList, dark ? L"DarkMode_Explorer" : NULL, NULL);
  }
  else if (!skip_generic_theme && !lstrcmpiA(class_name, "Edit") && !dark && GetParent(hwnd))
  {
    /* The edit box inside a drop-down you can type in (the Profile box): the plain look, without the accent line Windows draws under a focused themed edit. */
    char pcls[24];
    GetClassNameA(GetParent(hwnd), pcls, sizeof(pcls));
    SetWindowTheme(hwnd, !lstrcmpiA(pcls, "ComboBox") ? L"" : NULL, !lstrcmpiA(pcls, "ComboBox") ? L"" : NULL);
  }
  else if (!skip_generic_theme)
  {
    /* Edit controls are included here (unlike ListBox and the status
       bar) specifically so their scrollbar renders dark --
       WM_CTLCOLOREDIT already handles the text/background correctly
       regardless of this call, but the scrollbar arrows/thumb/track
       are separate and only themed through this. */
    SetWindowTheme(hwnd, dark ? L"DarkMode_Explorer" : NULL, NULL);
  }

  if (lstrcmpiA(class_name, "SysListView32") == 0)
  {
    /* SetWindowTheme("DarkMode_Explorer") above only reaches this
       control's scrollbar and border -- the actual row/cell colors are
       separate and need setting explicitly. The header is handled in
       its own branch above, not here. */
    RECT r;
    /* Lines between the columns (always) and rows (View > Game List > Show Grid Lines) of every list but the
       game browser, which draws its own: painted over the list by grid_subclass_proc, in both
       themes (the built-in grid is too bright in dark mode). */
    SendMessage(hwnd, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_GRIDLINES, 0);
    if (GetDlgCtrlID(hwnd) != IDC_BROWSER_LIST) SetWindowSubclass(hwnd, listview_grid_subclass_proc, 6, 0);
    COLORREF bg_color = GetDlgCtrlID(hwnd) == IDC_BROWSER_LIST ? RGB(0x24, 0x24, 0x24)   /* the ROM browser: darker, #242424 */
                                                                : RGB(0x2D, 0x2D, 0x2D);  /* every other list: #2D2D2D */
    ListView_SetBkColor(hwnd, dark ? bg_color : RGB(255, 255, 255));
    ListView_SetTextColor(hwnd, dark ? THEME_DARK_TEXT : RGB(0, 0, 0));
    ListView_SetTextBkColor(hwnd, dark ? bg_color : RGB(255, 255, 255));

    /* So this list's header text can be recolored; see the note above. */
    SetWindowSubclass(hwnd, listview_header_subclass_proc, 2, 0);

    /* Same reasoning and technique as the header's own redraw above --
       a plain invalidate wasn't enough to clear stale column-divider
       rendering left over from before a theme switch. */
    GetWindowRect(hwnd, &r);
    SetWindowPos(hwnd, NULL, 0, 0, (r.right - r.left) + 1, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
  }
  else if (lstrcmpiA(class_name, "msctls_statusbar32") == 0)
  {
    /* SB_SETBKCOLOR is documented to have no effect at all while visual
       styles are active for this control -- unlike everywhere else in
       this file, NULL here would keep the OS's own themed (light)
       background and silently ignore the color set below. Empty
       strings specifically turn styling off for this control, which is
       what actually lets SB_SETBKCOLOR take effect. */
    SetWindowTheme(hwnd, dark ? L"" : NULL, dark ? L"" : NULL);
    SendMessage(hwnd, SB_SETBKCOLOR, 0, dark ? THEME_DARK_BG : (LPARAM)CLR_DEFAULT);
    SetWindowSubclass(hwnd, statusbar_subclass_proc, 1, 0);
    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE);
  }

  InvalidateRect(hwnd, NULL, TRUE);
  return TRUE;
}

/* A checkbox or radio button is only as wide as its box and its words, so clicking in the empty
   space beside it does not toggle it. Only ever narrows a control. */
static BOOL CALLBACK fit_check_width(HWND c, LPARAM lp)
{
  char cls[16], text[256];
  LONG style;
  RECT rc, tr;
  HDC dc;
  HGDIOBJ of;
  HFONT font;
  HWND parent;
  int type, w, dpi = 96;

  (void)lp;
  if (!GetClassNameA(c, cls, sizeof(cls)) || lstrcmpiA(cls, "Button")) return TRUE;
  style = GetWindowLong(c, GWL_STYLE);
  type = style & BS_TYPEMASK;
  if (type != BS_CHECKBOX && type != BS_AUTOCHECKBOX && type != BS_RADIOBUTTON && type != BS_AUTORADIOBUTTON &&
      type != BS_3STATE && type != BS_AUTO3STATE) return TRUE;
  if (style & (BS_PUSHLIKE | BS_MULTILINE | BS_LEFTTEXT | BS_BITMAP | BS_ICON)) return TRUE;
  if (!GetWindowTextA(c, text, sizeof(text)) || !text[0]) return TRUE;

  font = (HFONT)SendMessage(c, WM_GETFONT, 0, 0);
  dc = GetDC(c);
  if (!dc) return TRUE;
  of = SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
  SetRectEmpty(&tr);
  DrawTextA(dc, text, -1, &tr, DT_CALCRECT | DT_SINGLELINE | DT_LEFT);
  SelectObject(dc, of);
  dpi = GetDeviceCaps(dc, LOGPIXELSX);
  ReleaseDC(c, dc);

  if (gui.ui_style)
  {
    /* Modern: the box (16 px) and the words are measured with the font now in use, and the control is made exactly that wide,
       wider than before if need be, but never into a control beside it or past the edge of its window. */
    int need = MulDiv(16 + 8, dpi > 0 ? dpi : 96, 96) + (tr.right - tr.left) + MulDiv(8, dpi > 0 ? dpi : 96, 96);
    RECT cr, pr, sr;
    HWND sib;
    int limit;

    parent = GetParent(c);
    if (!parent || !GetWindowRect(c, &cr)) return TRUE;
    GetClientRect(parent, &pr);
    MapWindowPoints(NULL, parent, (POINT *)&cr, 2);
    limit = pr.right - 4;
    for (sib = GetWindow(parent, GW_CHILD); sib; sib = GetWindow(sib, GW_HWNDNEXT))
    {
      if (sib == c || !IsWindowVisible(sib) || !GetWindowRect(sib, &sr)) continue;
      MapWindowPoints(NULL, parent, (POINT *)&sr, 2);
      if (sr.top < cr.bottom && sr.bottom > cr.top && sr.left >= cr.left + MulDiv(24, dpi > 0 ? dpi : 96, 96) && sr.left - 6 < limit)
        limit = sr.left - 6;
    }
    w = need < limit - cr.left ? need : limit - cr.left;
    if (w < MulDiv(24, dpi > 0 ? dpi : 96, 96)) return TRUE;
    if (w != cr.right - cr.left) SetWindowPos(c, NULL, cr.left, cr.top, w, cr.bottom - cr.top, SWP_NOZORDER | SWP_NOACTIVATE);
    return TRUE;
  }

  /* Classic: the same box and gap the check box is drawn with (see mod_paint_check), so the words are never cut short. */
  w = MulDiv(16 + 8, dpi > 0 ? dpi : 96, 96) + (tr.right - tr.left) + MulDiv(8, dpi > 0 ? dpi : 96, 96);
  GetWindowRect(c, &rc);
  if (w >= rc.right - rc.left) return TRUE;
  parent = GetParent(c);
  MapWindowPoints(NULL, parent, (POINT *)&rc, 2);
  SetWindowPos(c, NULL, rc.left, rc.top, w, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOACTIVATE);
  return TRUE;
}

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#define DWMWA_BORDER_COLOR             34
#define DWMWA_CAPTION_COLOR            35
#define DWMWA_TEXT_COLOR               36
#endif
#define DWM_COLOR_DEFAULT_VALUE        0xFFFFFFFFu     /* DWMWA_COLOR_DEFAULT: the system's own color */

static struct { LONG height, weight; BYTE italic; HFONT font; } style_cache[32];     /* by size, weight and slant: a bold label stays bold */
static int style_cache_n;

/* Segoe UI Variable where the system has it (Windows 11), Segoe UI elsewhere. */
static const char *modern_face(void)
{
  static const char *face;
  if (!face)
  {
    HDC dc = CreateCompatibleDC(NULL);
    LOGFONTA lf;
    HFONT f;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -12;
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpyA(lf.lfFaceName, "Segoe UI Variable Text");
    f = CreateFontIndirectA(&lf);
    face = "Segoe UI";
    if (dc && f)
    {
      char got[LF_FACESIZE];
      HGDIOBJ old = SelectObject(dc, f);
      got[0] = '\0';
      GetTextFaceA(dc, sizeof(got), got);
      if (!lstrcmpiA(got, "Segoe UI Variable Text")) face = "Segoe UI Variable Text";
      SelectObject(dc, old);
    }
    if (f) DeleteObject(f);
    if (dc) DeleteDC(dc);
  }
  return face;
}

/* The Modern style's text size: 8.5 pt where the Classic style has the system font (8 pt, which is really 11 pixels, 8.25 pt): 3% more, as
   fonts are made in whole pixels. */
static LONG modern_size(LONG height)
{
  return (height * 103 + (height < 0 ? -50 : 50)) / 100;
}

/* A window's font in the chosen style: the Classic style keeps the font it has; Modern has Segoe UI at its own size. */
HFONT theme_style_font(HFONT base)
{
  LOGFONTA lf;
  HFONT f;
  LONG base_h;
  int i;

  if (!gui.ui_style || !base || !GetObjectA(base, sizeof(lf), &lf)) return base;
  if (lf.lfCharSet == SYMBOL_CHARSET || !lstrcmpiA(lf.lfFaceName, "Segoe UI Symbol") || !lstrcmpiA(lf.lfFaceName, "Segoe MDL2 Assets") ||
      !lstrcmpiA(lf.lfFaceName, "Segoe Fluent Icons"))
    return base;                                           /* a font of pictures stays what it is */
  for (i = 0; i < style_cache_n; i++)
    if (style_cache[i].height == lf.lfHeight && style_cache[i].weight == lf.lfWeight && style_cache[i].italic == lf.lfItalic) return style_cache[i].font;

  if (style_cache_n >= 32) return base;                    /* (every font made is kept: none is made once the table is full) */
  base_h = lf.lfHeight;
  lf.lfQuality = CLEARTYPE_QUALITY;
  lf.lfCharSet = DEFAULT_CHARSET;
  lstrcpynA(lf.lfFaceName, modern_face(), LF_FACESIZE);
  lf.lfHeight = modern_size(lf.lfHeight);
  lf.lfWidth = 0;                                          /* (a width kept from the font it came from would squeeze the letters) */
  f = CreateFontIndirectA(&lf);
  if (!f) return base;
  style_cache[style_cache_n].height = base_h;                    /* (the table is looked up by the height of the font it was made from) */
  style_cache[style_cache_n].weight = lf.lfWeight; style_cache[style_cache_n].italic = lf.lfItalic;
  style_cache[style_cache_n].font = f; style_cache_n++;
  return f;
}

/* The font of the menus the program draws: the Large interface uses its own big font; otherwise the system menu font's size and weight (the
   Modern style changes the face and makes it 8.5 pt, as in the windows). */
HFONT theme_menu_font(void)
{
  static HFONT f;
  static int for_style = -1;

  if (gui.large_ui) return gui_get_ui_font();
  if (!f || for_style != gui.ui_style)
  {
    NONCLIENTMETRICSA ncm;

    memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
    {
      if (gui.ui_style)
      {
        ncm.lfMenuFont.lfQuality = CLEARTYPE_QUALITY;
        lstrcpynA(ncm.lfMenuFont.lfFaceName, modern_face(), LF_FACESIZE);
        ncm.lfMenuFont.lfHeight = (ncm.lfMenuFont.lfHeight * 17 + (ncm.lfMenuFont.lfHeight < 0 ? -9 : 9)) / 18;      /* (the system's 9 pt menu font) */
      }
      if (f) DeleteObject(f);
      f = CreateFontIndirectA(&ncm.lfMenuFont);
      for_style = gui.ui_style;
    }
  }
  return f ? f : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

/****************************************************************************
 * The Modern style: flat, rounded buttons, check boxes and radio buttons
 *
 * Every ordinary button of every window is painted here when the Modern style is on (a subclass, put on or taken off when
 * the style is applied). Owner-drawn buttons, picture buttons, group boxes and push-like check boxes keep their own drawing.
 ****************************************************************************/

static COLORREF modern_blue(void)      /* the Modern style's color when no accent color is chosen */
{
  return theme_is_dark() ? RGB(0x4C, 0x9A, 0xFF) : RGB(0x00, 0x67, 0xC0);
}

static COLORREF mod_accent(void)
{
  if (gui.accent > 0) return theme_accent_bg();
  return modern_blue();
}

COLORREF theme_modern_accent(void) { return mod_accent(); }

static COLORREF mod_text(int dark, int disabled)
{
  if (disabled) return dark ? THEME_DARK_MUTED : RGB(160, 160, 165);
  return dark ? THEME_DARK_TEXT : RGB(27, 27, 27);
}

static void mod_fill_parent(HWND h, HDC dc, const RECT *rc)
{
  HWND p = GetParent(h);
  HBRUSH b = p ? (HBRUSH)SendMessage(p, WM_CTLCOLORBTN, (WPARAM)dc, (LPARAM)h) : NULL;
  FillRect(dc, rc, b ? b : GetSysColorBrush(COLOR_BTNFACE));
}

static int mod_dpi_px(HDC dc, int px)
{
  int dpi = GetDeviceCaps(dc, LOGPIXELSX);
  return MulDiv(px, dpi > 0 ? dpi : 96, 96);
}

static void mod_draw_text(HWND h, HDC dc, RECT *tr, UINT flags, COLORREF color)
{
  wchar_t text[256];
  HFONT f = (HFONT)SendMessage(h, WM_GETFONT, 0, 0);
  HGDIOBJ old = f ? SelectObject(dc, f) : NULL;

  text[0] = L'\0';
  GetWindowTextW(h, text, 256);            /* wide: a button's text can be a symbol (the Refresh arrow) */
  if (SendMessage(h, WM_QUERYUISTATE, 0, 0) & UISF_HIDEACCEL) flags |= DT_HIDEPREFIX;
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, color);
  if (flags & DT_WORDBREAK)
  {
    /* DT_VCENTER only works for one line: a caption that may wrap is centered here, from the height it takes. */
    RECT cr = *tr;
    int th, boxh = tr->bottom - tr->top;
    DrawTextW(dc, text, -1, &cr, DT_CALCRECT | DT_WORDBREAK | (flags & (DT_CENTER | DT_RIGHT | DT_NOPREFIX | DT_HIDEPREFIX)));
    th = cr.bottom - cr.top;
    if (th < boxh) { tr->top += (boxh - th) / 2; tr->bottom = tr->top + th; }
    flags &= ~(UINT)DT_VCENTER;
  }
  DrawTextW(dc, text, -1, tr, flags);
  if (old) SelectObject(dc, old);
}

/* Drawing at 4 times the size and shrinking the result (HALFTONE) gives smooth edges: plain GDI shapes have jagged ones. */
#define MOD_SS 4
typedef struct { HDC dc; HBITMAP bmp; HGDIOBJ old; int w, h; } mod_ss;

static int mod_ss_begin(mod_ss *s, HDC dc, int w, int h, COLORREF bg)
{
  BITMAPINFO bi;
  void *bits = NULL;
  RECT all;
  HBRUSH b;

  memset(s, 0, sizeof(*s));
  if (w < 1 || h < 1) return 0;
  s->w = w; s->h = h;
  s->dc = CreateCompatibleDC(dc);
  if (!s->dc) return 0;
  memset(&bi, 0, sizeof(bi));
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = w * MOD_SS;
  bi.bmiHeader.biHeight = -h * MOD_SS;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  s->bmp = CreateDIBSection(s->dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
  if (!s->bmp) { DeleteDC(s->dc); s->dc = NULL; return 0; }
  s->old = SelectObject(s->dc, s->bmp);
  all.left = 0; all.top = 0; all.right = w * MOD_SS; all.bottom = h * MOD_SS;
  b = CreateSolidBrush(bg);
  FillRect(s->dc, &all, b);
  DeleteObject(b);
  return 1;
}

static void mod_ss_end(mod_ss *s, HDC dc, int x, int y)
{
  if (!s->dc) return;
  SetStretchBltMode(dc, HALFTONE);
  SetBrushOrgEx(dc, 0, 0, NULL);
  StretchBlt(dc, x, y, s->w, s->h, s->dc, 0, 0, s->w * MOD_SS, s->h * MOD_SS, SRCCOPY);
  SelectObject(s->dc, s->old);
  DeleteObject(s->bmp);
  DeleteDC(s->dc);
  s->dc = NULL;
}

/* A rounded rectangle in 4x coordinates (x, y, w, h in pixels of the final picture, with a corner radius). */
static void mod_ss_round(mod_ss *s, double x, double y, double w, double h, double radius, COLORREF fill, COLORREF line, double line_w)
{
  HBRUSH br = fill == CLR_INVALID ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(fill);
  HPEN pen = line == CLR_INVALID ? (HPEN)GetStockObject(NULL_PEN) : CreatePen(PS_SOLID, (int)(line_w * MOD_SS + 0.5), line);
  HGDIOBJ ob = SelectObject(s->dc, br), op = SelectObject(s->dc, pen);
  int half = line == CLR_INVALID ? 0 : (int)(line_w * MOD_SS / 2);

  RoundRect(s->dc, (int)(x * MOD_SS) + half, (int)(y * MOD_SS) + half, (int)((x + w) * MOD_SS) - half, (int)((y + h) * MOD_SS) - half,
            (int)(radius * 2 * MOD_SS), (int)(radius * 2 * MOD_SS));
  SelectObject(s->dc, ob);
  SelectObject(s->dc, op);
  if (fill != CLR_INVALID) DeleteObject(br);
  if (line != CLR_INVALID) DeleteObject(pen);
}

/* The colors of a Modern push button: fill, text and the thin line round it. */
static void mod_push_colors(int dark, int hot, int pushed, int dis, int def, COLORREF *fill, COLORREF *text, COLORREF *edge)
{
  if (def && !dis)
  {
    COLORREF a = mod_accent();
    int lum = (GetRValue(a) * 30 + GetGValue(a) * 59 + GetBValue(a) * 11) / 100;
    int d = pushed ? -22 : (hot ? 14 : 0);
    int rr = GetRValue(a) + d, gg = GetGValue(a) + d, bb = GetBValue(a) + d;
    *fill = RGB(rr < 0 ? 0 : rr > 255 ? 255 : rr, gg < 0 ? 0 : gg > 255 ? 255 : gg, bb < 0 ? 0 : bb > 255 ? 255 : bb);
    *text = lum > 150 ? RGB(10, 10, 10) : RGB(255, 255, 255);
    *edge = CLR_INVALID;
  }
  else if (dark)
  {
    *fill = dis ? RGB(42, 44, 50) : pushed ? RGB(44, 46, 52) : hot ? RGB(68, 72, 84) : RGB(54, 57, 66);
    *edge = dis ? RGB(58, 60, 66) : hot ? RGB(104, 110, 126) : RGB(82, 86, 98);
    *text = mod_text(1, dis);
  }
  else
  {
    *fill = dis ? RGB(246, 246, 247) : pushed ? RGB(222, 228, 240) : hot ? RGB(238, 243, 251) : RGB(251, 251, 252);
    *edge = dis ? RGB(226, 226, 230) : hot ? RGB(150, 178, 224) : RGB(196, 200, 210);
    *text = mod_text(0, dis);
  }
}

static void mod_paint_push(HWND h, HDC dc, const RECT *rc)
{
  LONG st = GetWindowLong(h, GWL_STYLE);
  int dark = theme_is_dark();
  int bst = (int)SendMessage(h, BM_GETSTATE, 0, 0);
  int pushed = (bst & BST_PUSHED) != 0, focus = (bst & BST_FOCUS) != 0;
  int hot = GetPropA(h, "gpx_hot") != NULL, dis = !IsWindowEnabled(h);
  int def = (st & BS_TYPEMASK) == BS_DEFPUSHBUTTON;
  COLORREF fill, text, edge, bg;
  RECT tr;
  mod_ss ss;
  int w = rc->right - rc->left, hh = rc->bottom - rc->top;
  double rad = (double)mod_dpi_px(dc, 5);

  mod_push_colors(dark, hot, pushed, dis, def, &fill, &text, &edge);

  mod_fill_parent(h, dc, rc);
  bg = GetPixel(dc, rc->left, rc->top);
  if (bg == CLR_INVALID) bg = dark ? THEME_DARK_BG : GetSysColor(COLOR_BTNFACE);

  if (mod_ss_begin(&ss, dc, w, hh, bg))
  {
    if (focus && !dis) mod_ss_round(&ss, 0.5, 0.5, w - 1, hh - 1, rad, fill, mod_accent(), 2.0);
    else               mod_ss_round(&ss, 0.5, 0.5, w - 1, hh - 1, rad, fill, edge, 1.0);
    mod_ss_end(&ss, dc, rc->left, rc->top);
  }

  tr = *rc;
  InflateRect(&tr, -mod_dpi_px(dc, 4), 0);
  {
    UINT align = ((st & BS_CENTER) == BS_LEFT) ? DT_LEFT : ((st & BS_CENTER) == BS_RIGHT) ? DT_RIGHT : DT_CENTER;
    if (!(st & BS_MULTILINE))
    {
      /* A caption wider than the button starts at its left edge and ends in "...", instead of being cut off on both sides. */
      wchar_t wt[256];
      RECT mr = {0, 0, 0, 0};
      HFONT f = (HFONT)SendMessage(h, WM_GETFONT, 0, 0);
      HGDIOBJ old = f ? SelectObject(dc, f) : NULL;
      GetWindowTextW(h, wt, 256);
      DrawTextW(dc, wt, -1, &mr, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
      if (old) SelectObject(dc, old);
      if (mr.right - mr.left > tr.right - tr.left) align = DT_LEFT;
    }
    mod_draw_text(h, dc, &tr, align | DT_VCENTER | ((st & BS_MULTILINE) ? DT_WORDBREAK : DT_SINGLELINE | DT_END_ELLIPSIS), text);
  }
}

static void mod_paint_check(HWND h, HDC dc, const RECT *rc)
{
  LONG st = GetWindowLong(h, GWL_STYLE);
  int type = st & BS_TYPEMASK;
  int radio = (type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON);
  int dark = theme_is_dark();
  int check = (int)SendMessage(h, BM_GETCHECK, 0, 0);
  int hot = GetPropA(h, "gpx_hot") != NULL, dis = !IsWindowEnabled(h);
  int focus = ((int)SendMessage(h, BM_GETSTATE, 0, 0) & BST_FOCUS) != 0;
  int sz = mod_dpi_px(dc, 16);
  double lw = 1.5 * sz / 16.0;
  RECT box, tr;
  COLORREF accent = mod_accent(), edge = dark ? (hot ? RGB(190, 194, 205) : RGB(140, 144, 155)) : (hot ? RGB(60, 60, 66) : RGB(105, 105, 112)), bg;
  mod_ss ss;

  if (dis) { accent = dark ? RGB(70, 76, 90) : RGB(180, 190, 205); edge = dark ? RGB(80, 82, 90) : RGB(190, 190, 196); }
  else if (focus && !check) edge = accent;                       /* the keyboard focus: the box's outline in the accent color */

  mod_fill_parent(h, dc, rc);
  bg = GetPixel(dc, rc->left, rc->top);
  if (bg == CLR_INVALID) bg = dark ? THEME_DARK_BG : GetSysColor(COLOR_BTNFACE);

  box.left = rc->left + 1;
  box.top = rc->top + (rc->bottom - rc->top - sz) / 2;
  box.right = box.left + sz;
  box.bottom = box.top + sz;

  if (mod_ss_begin(&ss, dc, sz, sz, bg))
  {
    if (radio)
    {
      mod_ss_round(&ss, 0, 0, sz, sz, sz / 2.0, CLR_INVALID, check ? accent : edge, lw);       /* a full circle */
      if (check)
      {
        double in = sz * 0.28;
        mod_ss_round(&ss, in, in, sz - 2 * in, sz - 2 * in, (sz - 2 * in) / 2.0, accent, CLR_INVALID, 0);
      }
    }
    else
    {
      double rad = sz * 0.25;
      if (check) mod_ss_round(&ss, 0, 0, sz, sz, rad, accent, CLR_INVALID, 0);
      else       mod_ss_round(&ss, 0, 0, sz, sz, rad, CLR_INVALID, edge, lw);

      if (check == BST_CHECKED)
      {
        LOGBRUSH lb;
        HPEN pen;
        HGDIOBJ op;
        lb.lbStyle = BS_SOLID; lb.lbColor = RGB(255, 255, 255); lb.lbHatch = 0;
        pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, (DWORD)(sz * 0.14 * MOD_SS + 0.5), &lb, 0, NULL);
        op = SelectObject(ss.dc, pen);
        MoveToEx(ss.dc, (int)(sz * 0.27 * MOD_SS), (int)(sz * 0.53 * MOD_SS), NULL);
        LineTo(ss.dc, (int)(sz * 0.44 * MOD_SS), (int)(sz * 0.69 * MOD_SS));
        LineTo(ss.dc, (int)(sz * 0.74 * MOD_SS), (int)(sz * 0.33 * MOD_SS));
        SelectObject(ss.dc, op);
        DeleteObject(pen);
      }
      else if (check == BST_INDETERMINATE)
      {
        mod_ss_round(&ss, sz * 0.27, sz * 0.5 - sz * 0.07, sz * 0.46, sz * 0.14, sz * 0.07, RGB(255, 255, 255), CLR_INVALID, 0);
      }
    }
    mod_ss_end(&ss, dc, box.left, box.top);
  }

  tr = *rc;
  tr.left = box.right + mod_dpi_px(dc, 8);
  mod_draw_text(h, dc, &tr, DT_LEFT | DT_VCENTER | ((st & BS_MULTILINE) ? DT_WORDBREAK : DT_SINGLELINE | DT_END_ELLIPSIS), mod_text(dark, dis));

}

/* A tick box in the look of the Modern check boxes (rounded, smooth edges), drawn at x, y, sz pixels square on a background of
   the color bg; the Cheats window uses it for the rows and the header. */
void theme_draw_tickbox(HDC dc, int x, int y, int sz, int checked, COLORREF bg)
{
  int dark = theme_is_dark();
  COLORREF accent = mod_accent(), edge = dark ? RGB(140, 144, 155) : RGB(105, 105, 112);
  double lw = 1.5 * sz / 16.0, rad = sz * 0.25;
  mod_ss ss;

  if (!mod_ss_begin(&ss, dc, sz, sz, bg)) return;
  if (checked)
  {
    LOGBRUSH lb;
    HPEN pen;
    HGDIOBJ op;

    mod_ss_round(&ss, 0, 0, sz, sz, rad, accent, CLR_INVALID, 0);
    lb.lbStyle = BS_SOLID; lb.lbColor = RGB(255, 255, 255); lb.lbHatch = 0;
    pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, (DWORD)(sz * 0.14 * MOD_SS + 0.5), &lb, 0, NULL);
    op = SelectObject(ss.dc, pen);
    MoveToEx(ss.dc, (int)(sz * 0.27 * MOD_SS), (int)(sz * 0.53 * MOD_SS), NULL);
    LineTo(ss.dc, (int)(sz * 0.44 * MOD_SS), (int)(sz * 0.69 * MOD_SS));
    LineTo(ss.dc, (int)(sz * 0.74 * MOD_SS), (int)(sz * 0.33 * MOD_SS));
    SelectObject(ss.dc, op);
    DeleteObject(pen);
  }
  else mod_ss_round(&ss, 0, 0, sz, sz, rad, CLR_INVALID, edge, lw);
  mod_ss_end(&ss, dc, x, y);
}

static LRESULT CALLBACK modern_button_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref)
{
  (void)id; (void)ref;
  {
    /* A button can be changed to owner-drawn while it has this painting (the pad button that waits for a key): then it is
       drawn by its window, not here. */
    int type = GetWindowLong(h, GWL_STYLE) & BS_TYPEMASK;
    if (type == BS_OWNERDRAW || type == BS_GROUPBOX || type == BS_USERBUTTON) return DefSubclassProc(h, m, w, l);
  }
  switch (m)
  {
    case WM_ERASEBKGND:
      return 1;

    case WM_PAINT:
    case WM_PRINTCLIENT:
    {
      PAINTSTRUCT ps;
      HDC dc = (m == WM_PAINT) ? BeginPaint(h, &ps) : (HDC)w;
      RECT rc;
      int type = GetWindowLong(h, GWL_STYLE) & BS_TYPEMASK;

      GetClientRect(h, &rc);
      {
        /* Drawn off screen and copied over in one go: no flicker. */
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = mem ? CreateCompatibleBitmap(dc, rc.right, rc.bottom) : NULL;
        HGDIOBJ old = bmp ? SelectObject(mem, bmp) : NULL;
        HDC to = bmp ? mem : dc;

        if (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON) mod_paint_push(h, to, &rc);
        else mod_paint_check(h, to, &rc);
        if (bmp) { BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY); SelectObject(mem, old); DeleteObject(bmp); }
        if (mem) DeleteDC(mem);
      }
      if (m == WM_PAINT) EndPaint(h, &ps);
      return 0;
    }

    case WM_MOUSEMOVE:
      if (!GetPropA(h, "gpx_hot"))
      {
        TRACKMOUSEEVENT te;
        te.cbSize = sizeof(te); te.dwFlags = TME_LEAVE; te.hwndTrack = h; te.dwHoverTime = 0;
        TrackMouseEvent(&te);
        SetPropA(h, "gpx_hot", (HANDLE)1);
        InvalidateRect(h, NULL, FALSE);
      }
      break;

    case WM_MOUSELEAVE:
      RemovePropA(h, "gpx_hot");
      InvalidateRect(h, NULL, FALSE);
      break;

    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
    case WM_SETTEXT:
    case WM_UPDATEUISTATE:                  /* Alt shows the underline of a &: the system draws it itself, off its own place, until this repaint covers it */
    case BM_SETCHECK:
    case BM_SETSTATE:
    case WM_LBUTTONUP:
    case WM_KEYUP:
    {
      LRESULT r = DefSubclassProc(h, m, w, l);
      InvalidateRect(h, NULL, FALSE);
      return r;
    }

    case WM_NCDESTROY:
      RemovePropA(h, "gpx_hot");
      RemoveWindowSubclass(h, modern_button_proc, 7);
      break;
  }
  return DefSubclassProc(h, m, w, l);
}

/* Puts the Modern painting on an ordinary button, or takes it off again (Classic style). */
static void modern_button_hook(HWND hwnd)
{
  LONG st = GetWindowLong(hwnd, GWL_STYLE);
  int type = st & BS_TYPEMASK;
  int ours = (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON || type == BS_CHECKBOX || type == BS_AUTOCHECKBOX ||
              type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON || type == BS_3STATE || type == BS_AUTO3STATE) &&
             !(st & (BS_PUSHLIKE | BS_BITMAP | BS_ICON | BS_LEFTTEXT));      /* (text on the left of the box: left to the system) */
  DWORD_PTR dummy;

  /* Classic: the check boxes and radio buttons are drawn here too, flat and in the accent color (the system's cannot take it). */
  if (ours && (gui.ui_style || (type != BS_PUSHBUTTON && type != BS_DEFPUSHBUTTON))) SetWindowSubclass(hwnd, modern_button_proc, 7, 0);
  else if (GetWindowSubclass(hwnd, modern_button_proc, 7, &dummy)) { RemoveWindowSubclass(hwnd, modern_button_proc, 7); InvalidateRect(hwnd, NULL, TRUE); }
}

/****************************************************************************
 * The Modern style: drop-down lists (the closed box and the list that opens)
 ****************************************************************************/

/* The text of row i of a combo box or a list box, of any length (the caller frees it), or NULL. */
static wchar_t *mod_row_text(HWND h, UINT get_len, UINT get_text, int i)
{
  LRESULT n = SendMessageW(h, get_len, (WPARAM)i, 0);
  wchar_t *t;

  if (n == CB_ERR || n < 0 || n > 8192) return NULL;
  t = (wchar_t *)calloc((size_t)n + 2, sizeof(wchar_t));
  if (!t) return NULL;
  SendMessageW(h, get_text, (WPARAM)i, (LPARAM)t);
  return t;
}

static LRESULT CALLBACK modern_combo_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref)
{
  (void)id; (void)ref;
  switch (m)
  {
    case WM_ERASEBKGND:
      return 1;

    case WM_PAINT:
    case WM_PRINTCLIENT:
    {
      PAINTSTRUCT ps;
      HDC dc = (m == WM_PAINT) ? BeginPaint(h, &ps) : (HDC)w;
      RECT rc, tr;
      HDC mem;
      HBITMAP bmp;
      HGDIOBJ old;
      HDC to;
      int dark = theme_is_dark(), dis = !IsWindowEnabled(h);
      int hot = GetPropA(h, "gpx_hot") != NULL, focus = (GetFocus() == h) || (GetFocus() && IsChild(h, GetFocus())) || SendMessage(h, CB_GETDROPPEDSTATE, 0, 0);
      COLORREF fill, edge, text, bg;
      mod_ss ss;
      int rw, rh, sel, editable = ((GetWindowLong(h, GWL_STYLE) & 3) == CBS_DROPDOWN), have_ci;
      COMBOBOXINFO ci;
      HFONT f;

      ci.cbSize = sizeof(ci);
      have_ci = GetComboBoxInfo(h, &ci);
      GetClientRect(h, &rc);
      rw = rc.right; rh = rc.bottom;
      mem = CreateCompatibleDC(dc);
      bmp = mem ? CreateCompatibleBitmap(dc, rw, rh) : NULL;
      old = bmp ? SelectObject(mem, bmp) : NULL;
      to = bmp ? mem : dc;

      fill = dark ? (dis ? RGB(38, 40, 46) : hot ? RGB(54, 58, 68) : RGB(46, 49, 57)) : (dis ? RGB(246, 246, 247) : hot ? RGB(244, 247, 252) : RGB(255, 255, 255));
      edge = dark ? (focus ? mod_accent() : hot ? RGB(104, 110, 126) : RGB(82, 86, 98)) : (focus ? mod_accent() : hot ? RGB(150, 178, 224) : RGB(196, 200, 210));
      text = mod_text(dark, dis);
      if (editable && have_ci && ci.hwndItem && !dis)
      {
        /* A box you can type in is filled with the color of its own edit box, so the two are one. */
        HBRUSH eb = (HBRUSH)SendMessage(GetParent(h), WM_CTLCOLOREDIT, (WPARAM)to, (LPARAM)ci.hwndItem);
        LOGBRUSH elb;
        if (eb && GetObject(eb, sizeof(elb), &elb) && elb.lbStyle == BS_SOLID) fill = elb.lbColor;
      }

      mod_fill_parent(h, to, &rc);
      bg = GetPixel(to, 0, 0);
      if (bg == CLR_INVALID) bg = dark ? THEME_DARK_BG : GetSysColor(COLOR_BTNFACE);
      if (mod_ss_begin(&ss, to, rw, rh, bg))
      {
        double rad = (double)mod_dpi_px(to, 5);
        mod_ss_round(&ss, 0.5, 0.5, rw - 1, rh - 1, rad, fill, edge, focus ? 2.0 : 1.0);
        {
          /* the chevron on the right */
          LOGBRUSH lb;
          HPEN pen;
          HGDIOBJ op;
          double cx = rw - mod_dpi_px(to, 14), cy = rh / 2.0, d = mod_dpi_px(to, 3) + 1;
          if (have_ci && ci.rcButton.right > ci.rcButton.left) { cx = (ci.rcButton.left + ci.rcButton.right) / 2.0; cy = (ci.rcButton.top + ci.rcButton.bottom) / 2.0; }      /* in the middle of the button */
          lb.lbStyle = BS_SOLID; lb.lbColor = dis ? (dark ? RGB(90, 92, 100) : RGB(190, 190, 196)) : (dark ? RGB(200, 204, 214) : RGB(80, 84, 94)); lb.lbHatch = 0;
          pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, (DWORD)(1.6 * MOD_SS + 0.5), &lb, 0, NULL);
          op = SelectObject(ss.dc, pen);
          MoveToEx(ss.dc, (int)((cx - d) * MOD_SS), (int)((cy - d / 2) * MOD_SS), NULL);
          LineTo(ss.dc, (int)(cx * MOD_SS), (int)((cy + d / 2) * MOD_SS));
          LineTo(ss.dc, (int)((cx + d) * MOD_SS), (int)((cy - d / 2) * MOD_SS));
          SelectObject(ss.dc, op);
          DeleteObject(pen);
        }
        mod_ss_end(&ss, to, 0, 0);
      }

      tr = rc;
      tr.left += mod_dpi_px(to, 4);
      tr.right -= mod_dpi_px(to, 26);
      InflateRect(&tr, 0, -2);
      sel = (int)SendMessage(h, CB_GETCURSEL, 0, 0);
      f = (HFONT)SendMessage(h, WM_GETFONT, 0, 0);
      {
        LONG cst = GetWindowLong(h, GWL_STYLE);
        HGDIOBJ of = f ? SelectObject(to, f) : NULL;
        SetBkMode(to, TRANSPARENT);
        SetTextColor(to, text);
        if (cst & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE))
        {
          /* A list that draws its own rows (the color lists): the chosen one is drawn by its window, on the box. */
          DRAWITEMSTRUCT di;
          memset(&di, 0, sizeof(di));
          di.CtlType = ODT_COMBOBOX;
          di.CtlID = (UINT)GetDlgCtrlID(h);
          di.itemID = (UINT)sel;
          di.itemAction = ODA_DRAWENTIRE;
          di.itemState = ODS_COMBOBOXEDIT | (dis ? ODS_DISABLED : 0);
          di.hwndItem = h;
          di.hDC = to;
          di.rcItem = tr;
          if (sel >= 0) SendMessage(GetParent(h), WM_DRAWITEM, (WPARAM)di.CtlID, (LPARAM)&di);
        }
        else if ((cst & 3) == CBS_DROPDOWNLIST)
        {
          wchar_t *item = sel >= 0 ? mod_row_text(h, CB_GETLBTEXTLEN, CB_GETLBTEXT, sel) : NULL;
          tr.left += mod_dpi_px(to, 6);
          if (item) { DrawTextW(to, item, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX); free(item); }
        }
        if (of) SelectObject(to, of);
      }

      if (bmp)
      {
        /* A combo you can type in: its edit box is a window of its own on top; it is left out of the copy. */
        int saved = SaveDC(dc);
        if (editable && have_ci && ci.hwndItem)
        {
          RECT er;
          GetWindowRect(ci.hwndItem, &er);
          MapWindowPoints(NULL, h, (POINT *)&er, 2);
          if (ci.rcButton.right > ci.rcButton.left && er.right > ci.rcButton.left) er.right = ci.rcButton.left;      /* never over the arrow */
          ExcludeClipRect(dc, er.left, er.top, er.right, er.bottom);
        }
        BitBlt(dc, 0, 0, rw, rh, mem, 0, 0, SRCCOPY);
        RestoreDC(dc, saved);
        SelectObject(mem, old); DeleteObject(bmp);
      }
      if (mem) DeleteDC(mem);
      if (m == WM_PAINT) EndPaint(h, &ps);
      return 0;
    }

    case WM_MOUSEMOVE:
      if (!GetPropA(h, "gpx_hot"))
      {
        TRACKMOUSEEVENT te;
        te.cbSize = sizeof(te); te.dwFlags = TME_LEAVE; te.hwndTrack = h; te.dwHoverTime = 0;
        TrackMouseEvent(&te);
        SetPropA(h, "gpx_hot", (HANDLE)1);
        InvalidateRect(h, NULL, FALSE);
      }
      break;

    case WM_MOUSELEAVE:
      RemovePropA(h, "gpx_hot");
      InvalidateRect(h, NULL, FALSE);
      break;

    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
    case CB_SETCURSEL:
    case CB_SHOWDROPDOWN:
    {
      LRESULT r = DefSubclassProc(h, m, w, l);
      InvalidateRect(h, NULL, FALSE);
      return r;
    }

    case WM_NCDESTROY:
      RemovePropA(h, "gpx_hot");
      RemoveWindowSubclass(h, modern_combo_proc, 10);
      break;
  }
  return DefSubclassProc(h, m, w, l);
}

/* The list that drops down: rows with room around the words, the chosen row on a soft rounded plate. */
static LRESULT CALLBACK modern_list_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref)
{
  (void)id; (void)ref;
  switch (m)
  {
    case WM_ERASEBKGND:
      return 1;

    case WM_PAINT:
    case WM_PRINTCLIENT:
    {
      PAINTSTRUCT ps;
      HDC dc = (m == WM_PAINT) ? BeginPaint(h, &ps) : (HDC)w;
      RECT rc;
      HDC mem;
      HBITMAP bmp;
      HGDIOBJ old;
      HDC to;
      int dark = theme_is_dark(), n, top, sel, i;
      COLORREF bgc = dark ? RGB(40, 43, 50) : RGB(255, 255, 255);
      HBRUSH bb;
      HFONT f = (HFONT)SendMessage(h, WM_GETFONT, 0, 0);

      GetClientRect(h, &rc);
      mem = CreateCompatibleDC(dc);
      bmp = mem ? CreateCompatibleBitmap(dc, rc.right, rc.bottom) : NULL;
      old = bmp ? SelectObject(mem, bmp) : NULL;
      to = bmp ? mem : dc;

      bb = CreateSolidBrush(bgc);
      FillRect(to, &rc, bb);
      DeleteObject(bb);

      {
        HGDIOBJ of = f ? SelectObject(to, f) : NULL;
        n = (int)SendMessage(h, LB_GETCOUNT, 0, 0);
        top = (int)SendMessage(h, LB_GETTOPINDEX, 0, 0);
        sel = (int)SendMessage(h, LB_GETCURSEL, 0, 0);
        SetBkMode(to, TRANSPARENT);
        for (i = top; i >= 0 && i < n; i++)
        {
          RECT ir, pr;
          wchar_t *item;
          if (SendMessage(h, LB_GETITEMRECT, (WPARAM)i, (LPARAM)&ir) == LB_ERR) break;
          if (ir.top >= rc.bottom) break;
          item = mod_row_text(h, LB_GETTEXTLEN, LB_GETTEXT, i);
          if (i == sel)
          {
            HBRUSH hb = CreateSolidBrush(dark ? RGB(66, 74, 96) : RGB(222, 232, 248));
            HGDIOBJ ob = SelectObject(to, hb), op = SelectObject(to, GetStockObject(NULL_PEN));
            int rr = mod_dpi_px(to, 4);
            RoundRect(to, ir.left + rr, ir.top + 1, ir.right - rr, ir.bottom - 1, rr * 2, rr * 2);
            SelectObject(to, ob); SelectObject(to, op); DeleteObject(hb);
          }
          pr = ir;
          pr.left += mod_dpi_px(to, 12);
          SetTextColor(to, dark ? THEME_DARK_TEXT : RGB(27, 27, 27));
          if (item) { DrawTextW(to, item, -1, &pr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX); free(item); }
        }
        if (of) SelectObject(to, of);
      }

      if (bmp) { BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY); SelectObject(mem, old); DeleteObject(bmp); }
      if (mem) DeleteDC(mem);
      if (m == WM_PAINT) EndPaint(h, &ps);
      return 0;
    }

    /* The system draws the row that is picked up or let go of straight onto the list, in its own look (left-aligned words, a blue
       highlight), without a paint message. The whole list is painted again right after, so only the Modern drawing is left. */
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_KEYDOWN:
    case WM_MOUSEWHEEL:
    case WM_VSCROLL:
    case WM_TIMER:
    case LB_SETCURSEL:
    case LB_SETTOPINDEX:
    {
      LRESULT r = DefSubclassProc(h, m, w, l);
      if (IsWindowVisible(h)) RedrawWindow(h, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
      return r;
    }

    case WM_NCDESTROY:
      RemoveWindowSubclass(h, modern_list_proc, 11);
      break;
  }
  return DefSubclassProc(h, m, w, l);
}

/* Puts the Modern painting on a drop-down list box (not on owner-drawn ones, nor on one you can type in), or takes it off. */
static void modern_combo_hook(HWND hwnd)
{
  LONG st = GetWindowLong(hwnd, GWL_STYLE);
  COMBOBOXINFO cbi;
  DWORD_PTR dummy;
  int type = st & 3;
  int ours = type == CBS_DROPDOWNLIST || type == CBS_DROPDOWN;
  int plain = type == CBS_DROPDOWNLIST && !(st & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE));     /* the open list is restyled only for these */

  cbi.cbSize = sizeof(cbi);
  if (gui.ui_style && ours)
  {
    SetWindowSubclass(hwnd, modern_combo_proc, 10, 0);
    if (plain && GetComboBoxInfo(hwnd, &cbi) && cbi.hwndList) SetWindowSubclass(cbi.hwndList, modern_list_proc, 11, 0);
    if (type == CBS_DROPDOWN && GetComboBoxInfo(hwnd, &cbi) && cbi.hwndItem)
    {
      LONG_PTR ex = GetWindowLongPtrA(cbi.hwndItem, GWL_EXSTYLE);
      if (ex & WS_EX_CLIENTEDGE)       /* the box you type in: no sunken frame of its own, the rounded one is the combo's */
      {
        SetWindowLongPtrA(cbi.hwndItem, GWL_EXSTYLE, ex & ~(LONG_PTR)WS_EX_CLIENTEDGE);
        SetWindowPos(cbi.hwndItem, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
      }
    }
  }
  else if (GetWindowSubclass(hwnd, modern_combo_proc, 10, &dummy))
  {
    RemoveWindowSubclass(hwnd, modern_combo_proc, 10);
    if (GetComboBoxInfo(hwnd, &cbi) && cbi.hwndList) RemoveWindowSubclass(cbi.hwndList, modern_list_proc, 11);
    InvalidateRect(hwnd, NULL, TRUE);
  }
}


/* Every control gets the style's font (its first font is kept in a property, so the other style can start from it again). */
static BOOL CALLBACK style_font_child(HWND c, LPARAM unused)
{
  HFONT cur = (HFONT)SendMessage(c, WM_GETFONT, 0, 0);
  HFONT base = (HFONT)GetPropA(c, "gpx_basefont");
  HFONT want;
  (void)unused;

  if (!cur) return TRUE;
  want = theme_style_font(base ? base : cur);
  if (want != cur)
  {
    if (!base) SetPropA(c, "gpx_basefont", (HANDLE)cur);
    SendMessage(c, WM_SETFONT, (WPARAM)want, FALSE);
  }
  return TRUE;
}

/* The Modern style's window frame: rounded corners (square in fullscreen), a title bar in the colors of the theme and no border line (Windows 11;
   older Windows ignores the attributes). The Classic style gives the system's own values back. */
static void apply_frame_style(HWND hwnd, int dark)
{
  DWORD corner = gui.ui_style ? 2 : 0;                                   /* DWMWCP_ROUND, DWMWCP_DEFAULT */
  COLORREF caption = gui.ui_style ? (dark ? THEME_DARK_BG : RGB(0xF3, 0xF3, 0xF3)) : (COLORREF)DWM_COLOR_DEFAULT_VALUE;
  COLORREF text    = gui.ui_style ? (dark ? THEME_DARK_TEXT : RGB(0x1B, 0x1B, 0x1B)) : (COLORREF)DWM_COLOR_DEFAULT_VALUE;
  COLORREF border  = gui.ui_style ? 0xFFFFFFFEu /* DWMWA_COLOR_NONE: no frame line around a window */ : (COLORREF)DWM_COLOR_DEFAULT_VALUE;

  if (hwnd == g_hwnd && gui.fullscreen)
  {
    corner = 1;                                                          /* DWMWCP_DONOTROUND: the picture fills the whole screen */
    border = 0xFFFFFFFEu;                                                /* no frame line round it in either style */
  }
  DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
  DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
  DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &text, sizeof(text));
  DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
}

void theme_refresh_frame(HWND hwnd)
{
  apply_frame_style(hwnd, theme_is_dark());
}

/* The rows of a drop-down list that draws its own rows (every drop-down of the program does, so that the chosen row takes the accent color):
   called with the owner-draw messages; 0 when the message is not about a combo box. */
int theme_combo_measure(MEASUREITEMSTRUCT *mi)
{
  if (mi->CtlType != ODT_COMBOBOX) return 0;
  mi->itemHeight = (UINT)TBS(18);
  return 1;
}

int theme_combo_draw(const DRAWITEMSTRUCT *di)
{
  int dark, sel, off;
  COLORREF bg, fg;
  HBRUSH br;
  RECT r;
  char text[256] = "", *t = text;
  int len;

  if (di->CtlType != ODT_COMBOBOX) return 0;
  if (di->itemID == (UINT)-1) return 1;
  dark = theme_is_dark();
  sel = (di->itemState & ODS_SELECTED) && !(di->itemState & ODS_COMBOBOXEDIT);
  off = (di->itemState & ODS_DISABLED) != 0;
  bg = sel ? theme_accent_bg() : (dark ? RGB(0x2D, 0x2D, 0x2D) : GetSysColor(COLOR_WINDOW));
  fg = off ? GetSysColor(COLOR_GRAYTEXT) : (sel ? (dark || gui.ui_style ? RGB(255, 255, 255) : GetSysColor(COLOR_HIGHLIGHTTEXT)) : (dark ? RGB(0xE0, 0xE0, 0xE0) : GetSysColor(COLOR_WINDOWTEXT)));
  r = di->rcItem;
  if (!(gui.ui_style && (di->itemState & ODS_COMBOBOXEDIT)))      /* (Modern: the rounded box behind the chosen one is already drawn) */
  {
    br = CreateSolidBrush(bg);
    FillRect(di->hDC, &r, br);
    DeleteObject(br);
  }
  len = (int)SendMessageA(di->hwndItem, CB_GETLBTEXTLEN, di->itemID, 0);       /* (CB_GETLBTEXT copies the whole text: the buffer must hold it) */
  if (len >= (int)sizeof(text)) t = (char *)malloc((size_t)len + 1);
  if (!t) t = text;
  else if (len >= 0) SendMessageA(di->hwndItem, CB_GETLBTEXT, di->itemID, (LPARAM)t);
  if (t == text && len >= (int)sizeof(text)) text[0] = '\0';                   /* (no memory: nothing is drawn) */
  SetBkMode(di->hDC, TRANSPARENT);
  SetTextColor(di->hDC, fg);
  r.left += TBS(6);
  DrawTextA(di->hDC, t, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
  if (t != text) free(t);
  if ((di->itemState & ODS_FOCUS) && !(di->itemState & ODS_COMBOBOXEDIT) && !sel) { RECT fr = di->rcItem; DrawFocusRect(di->hDC, &fr); }
  return 1;
}

static int is_trackbar(HWND h)
{
  char cls[32];
  return GetClassNameA(h, cls, sizeof(cls)) && !lstrcmpiA(cls, "msctls_trackbar32");
}

/* A color a part lighter (pct > 0) or darker (pct < 0). */
static COLORREF shade_color(COLORREF c, int pct)
{
  int r = GetRValue(c), g = GetGValue(c), b = GetBValue(c);

  if (pct >= 0) { r += (255 - r) * pct / 100; g += (255 - g) * pct / 100; b += (255 - b) * pct / 100; }
  else          { r += r * pct / 100; g += g * pct / 100; b += b * pct / 100; }
  return RGB(r, g, b);
}

/* The slider's knob in the accent color (Windows' own is its own blue, and black while it is held down): lighter in the dark theme and
   darker in the light one while the pointer is on it or it is held. `prev` is what was asked for so far. Returns what the window's
   answer to the custom-draw message is. */
LRESULT theme_trackbar_customdraw(NMHDR *hdr, LRESULT prev)
{
  NMCUSTOMDRAW *nm = (NMCUSTOMDRAW *)hdr;
  int off = !IsWindowEnabled(hdr->hwndFrom);

  if (nm->dwDrawStage == CDDS_PREPAINT) return prev | CDRF_NOTIFYITEMDRAW;
  if (nm->dwDrawStage == CDDS_ITEMPREPAINT && nm->dwItemSpec == TBCD_CHANNEL && off)
  {
    HBRUSH cb = CreateSolidBrush(theme_is_dark() ? RGB(0x44, 0x44, 0x44) : RGB(0xD0, 0xD0, 0xD0));      /* a slider that is switched off: a dim rail */
    FillRect(nm->hdc, &nm->rc, cb);
    DeleteObject(cb);
    return CDRF_SKIPDEFAULT;
  }
  if (nm->dwDrawStage == CDDS_ITEMPREPAINT && nm->dwItemSpec == TBCD_THUMB)
  {
    LONG st = GetWindowLong(hdr->hwndFrom, GWL_STYLE);
    int held = (nm->uItemState & CDIS_SELECTED) != 0, hot = (nm->uItemState & CDIS_HOT) != 0, dark = theme_is_dark();
    COLORREF base = mod_accent(), fill = off ? (dark ? RGB(0x5A, 0x5A, 0x5A) : RGB(0xB8, 0xB8, 0xB8))      /* (a slider that is switched off has a gray knob) */
                                             : held ? shade_color(base, dark ? 40 : -30) : hot ? shade_color(base, dark ? 20 : -12) : base;
    RECT rc = nm->rc;
    POINT pts[5];
    int mid = (rc.left + rc.right) / 2, tip = (rc.right - rc.left) / 2;
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, fill);
    HGDIOBJ ob = SelectObject(nm->hdc, br), op = SelectObject(nm->hdc, pen);

    rc.right--; rc.bottom--;
    if (st & TBS_TOP) { pts[0].x = rc.left; pts[0].y = rc.bottom; pts[1].x = rc.right; pts[1].y = rc.bottom; pts[2].x = rc.right; pts[2].y = rc.top + tip; pts[3].x = mid; pts[3].y = rc.top; pts[4].x = rc.left; pts[4].y = rc.top + tip; }
    else { pts[0].x = rc.left; pts[0].y = rc.top; pts[1].x = rc.right; pts[1].y = rc.top; pts[2].x = rc.right; pts[2].y = rc.bottom - tip; pts[3].x = mid; pts[3].y = rc.bottom; pts[4].x = rc.left; pts[4].y = rc.bottom - tip; }
    Polygon(nm->hdc, pts, 5);
    SelectObject(nm->hdc, ob); SelectObject(nm->hdc, op);
    DeleteObject(br); DeleteObject(pen);
    return CDRF_SKIPDEFAULT;
  }
  return prev;
}

/* A window with drop-downs or lists: the window's own code gets the owner-draw messages first (a list with pictures draws itself); the
   rows of the others are drawn here. */
static LRESULT CALLBACK combo_rows_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref)
{
  (void)id; (void)ref;
  if (m == WM_NCDESTROY) RemoveWindowSubclass(h, combo_rows_proc, 12);
  else if (m == WM_DRAWITEM && ((const DRAWITEMSTRUCT *)l)->CtlType == ODT_COMBOBOX)
  {
    LRESULT r = DefSubclassProc(h, m, w, l);
    return r ? r : (theme_combo_draw((const DRAWITEMSTRUCT *)l) ? TRUE : FALSE);
  }
  else if (m == WM_MEASUREITEM && ((const MEASUREITEMSTRUCT *)l)->CtlType == ODT_COMBOBOX)
  {
    LRESULT r = DefSubclassProc(h, m, w, l);
    return r ? r : (theme_combo_measure((MEASUREITEMSTRUCT *)l) ? TRUE : FALSE);
  }
  else if (m == WM_NOTIFY && ((const NMHDR *)l)->code == NM_CUSTOMDRAW && ((const NMHDR *)l)->idFrom != IDC_BROWSER_LIST &&
           ((gui.accent || gui.ui_style) || is_trackbar(((const NMHDR *)l)->hwndFrom)))
  {
    /* The lists of the other windows: the chosen row is drawn in the accent color, as in the game list (Windows would use its own
       color). The window's own code goes first (the Cheats list draws its boxes), and what it asks for is kept. */
    NMLVCUSTOMDRAW *cd = (NMLVCUSTOMDRAW *)l;
    char cls[32];
    LRESULT r;

    GetClassNameA(cd->nmcd.hdr.hwndFrom, cls, sizeof(cls));
    r = DefSubclassProc(h, m, w, l);
    if (!lstrcmpiA(cls, "msctls_trackbar32")) return theme_trackbar_customdraw(&cd->nmcd.hdr, r);
    if (lstrcmpiA(cls, "SysListView32") != 0) return r;
    if (!(gui.accent || gui.ui_style)) return r;
    if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return r | CDRF_NOTIFYITEMDRAW;
    if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) return r | CDRF_NOTIFYSUBITEMDRAW;
    if (cd->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM) &&
        (ListView_GetItemState(cd->nmcd.hdr.hwndFrom, (int)cd->nmcd.dwItemSpec, LVIS_SELECTED) & LVIS_SELECTED))
    {
      HWND list = cd->nmcd.hdr.hwndFrom;
      RECT cell, client;
      HBRUSH br;
      int last = (int)SendMessage(ListView_GetHeader(list), HDM_GETITEMCOUNT, 0, 0) - 1;

      cell.top = cd->iSubItem; cell.left = LVIR_BOUNDS;
      if (SendMessageA(list, LVM_GETSUBITEMRECT, cd->nmcd.dwItemSpec, (LPARAM)&cell))
      {
        if (cd->iSubItem == 0) cell.right = cell.left + ListView_GetColumnWidth(list, 0);     /* (the first cell's rectangle is the whole row's) */
        if (cd->iSubItem >= last) { GetClientRect(list, &client); if (client.right > cell.right) cell.right = client.right; }
        br = CreateSolidBrush(theme_accent_bg());
        FillRect(cd->nmcd.hdc, &cell, br);
        DeleteObject(br);
      }
      cd->clrTextBk = theme_accent_bg();
      cd->clrText = RGB(255, 255, 255);
      cd->nmcd.uItemState &= ~(UINT)(CDIS_SELECTED | CDIS_FOCUS);
      return r | CDRF_NEWFONT;
    }
    return r;
  }
  return DefSubclassProc(h, m, w, l);
}

void theme_apply_to_window(HWND hwnd)
{
  int dark = theme_is_dark();

  SetWindowSubclass(hwnd, combo_rows_proc, 12, 0);
  BOOL dark_bool = dark ? TRUE : FALSE;

  if (pSetPreferredAppMode)
  {
    pSetPreferredAppMode(dark ? AppMode_ForceDark : AppMode_ForceLight);
    if (pRefreshImmersiveColorPolicyState) pRefreshImmersiveColorPolicyState();
  }

  if (pAllowDarkModeForWindow) pAllowDarkModeForWindow(hwnd, dark_bool);
  SetWindowTheme(hwnd, dark ? L"DarkMode_Explorer" : NULL, NULL);

  /* Title bar -- both attribute numbers tried, see the #define comment
     above; DwmSetWindowAttribute simply fails (harmlessly) for whichever
     one this Windows version doesn't recognize. */
  DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark_bool, sizeof(dark_bool));
  DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE_PRE20H1, &dark_bool, sizeof(dark_bool));

  apply_frame_style(hwnd, dark);
  EnumChildWindows(hwnd, style_font_child, 0);

  EnumChildWindows(hwnd, fit_check_width, 0);
  EnumChildWindows(hwnd, apply_to_child, (LPARAM)dark);

  /* Popup menus cache their own theme state; without this, a menu
     opened after switching modes can still show the old one until the
     app restarts. */
  if (pFlushMenuThemes) pFlushMenuThemes();

  /* SWP_FRAMECHANGED forces the non-client area (title bar, borders) to
     actually repaint with the DWM attribute change just made -- without
     it, the change can take effect only on the next unrelated resize. */
  SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
  DrawMenuBar(hwnd);
  InvalidateRect(hwnd, NULL, TRUE);
}

int theme_handle_settingchange(HWND hwnd)
{
  int new_dark = read_system_dark();

  if (new_dark == system_dark) return 0;
  system_dark = new_dark;

  if (gui.theme_mode != THEME_AUTO) return 0;   /* manual override -- system change doesn't matter */

  theme_apply_to_window(hwnd);
  return 1;
}

HBRUSH theme_ctlcolor(HDC hdc)
{
  static HBRUSH dark_brush;

  if (!theme_is_dark()) return NULL;

  if (!dark_brush) dark_brush = CreateSolidBrush(THEME_DARK_BG);

  SetTextColor(hdc, THEME_DARK_TEXT);
  SetBkColor(hdc, THEME_DARK_BG);
  return dark_brush;
}

/* Same as theme_ctlcolor, but with an explicit background color instead
   of the app-wide default -- for the handful of controls that need a
   slightly different shade to stand out from their surroundings
   (search box, cheat code/name fields, certain list controls). Caller
   passes a distinct id per color so each gets its own cached brush
   rather than fighting over one shared static. */
HBRUSH theme_ctlcolor_custom(HDC hdc, COLORREF bg, int cache_id)
{
  static HBRUSH cached[4];

  if (!theme_is_dark()) return NULL;
  if (cache_id < 0 || cache_id >= 4) return theme_ctlcolor(hdc);

  if (!cached[cache_id]) cached[cache_id] = CreateSolidBrush(bg);

  SetTextColor(hdc, THEME_DARK_TEXT);
  SetBkColor(hdc, bg);
  return cached[cache_id];
}

LRESULT theme_statusbar_customdraw(NMHDR *hdr, HWND status_bar)
{
  NMCUSTOMDRAW *cd;

  if (!hdr || hdr->hwndFrom != status_bar || hdr->code != NM_CUSTOMDRAW) return -1;
  if (!theme_is_dark()) return -1;

  cd = (NMCUSTOMDRAW *)hdr;

  /* The header strip is #191919 in dark mode: the items are drawn here, and what is left of the
     strip to the right of the last one is filled after they are. */
  if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;

  if (cd->dwDrawStage == CDDS_ITEMPREPAINT)
  {
    draw_header_cell(cd->hdc, hdr->hwndFrom, (int)cd->dwItemSpec, &cd->rc);
    return CDRF_SKIPDEFAULT;
  }

  if (cd->dwDrawStage == CDDS_POSTPAINT)
  {
    RECT client, last;
    int n = (int)SendMessage(hdr->hwndFrom, HDM_GETITEMCOUNT, 0, 0);
    HBRUSH br = CreateSolidBrush(RGB(0x19, 0x19, 0x19));

    GetClientRect(hdr->hwndFrom, &client);
    if (n > 0 && SendMessage(hdr->hwndFrom, HDM_GETITEMRECT, (WPARAM)(n - 1), (LPARAM)&last)) client.left = last.right;
    FillRect(cd->hdc, &client, br);
    DeleteObject(br);
    return CDRF_DODEFAULT;
  }

  return CDRF_DODEFAULT;
}

/* One header cell in dark mode: #191919 with light text, the sort arrow and a thin divider. */
static void draw_header_cell(HDC dc, HWND header, int idx, const RECT *prc)
{
    HDITEMA hi;
    char text[128];
    RECT r = *prc, tr;
    HBRUSH br = CreateSolidBrush(RGB(0x19, 0x19, 0x19));
    HPEN pen, op;
    UINT fmt;

    FillRect(dc, &r, br);
    DeleteObject(br);

    /* A column that is switched off is a header cell of no width: nothing of it is drawn (its
       truncated text used to leave a stray colored speck at the end of the header row). */
    if (r.right - r.left < 14) return;

    memset(&hi, 0, sizeof(hi));
    text[0] = '\0';
    hi.mask = HDI_TEXT | HDI_FORMAT | HDI_IMAGE;
    hi.pszText = text;
    hi.cchTextMax = (int)sizeof(text);
    SendMessageA(header, HDM_GETITEMA, (WPARAM)idx, (LPARAM)&hi);

    tr = r;
    tr.left += 6;
    if (hi.fmt & HDF_IMAGE)                            /* a picture at the left of the title (the tick box of the Cheats window) */
    {
      HIMAGELIST il = (HIMAGELIST)SendMessageA(header, HDM_GETIMAGELIST, 0, 0);
      int iw = 0, ih = 0;

      if (il && ImageList_GetIconSize(il, &iw, &ih))
      {
        ImageList_Draw(il, hi.iImage, dc, tr.left, r.top + ((r.bottom - r.top) - ih) / 2, ILD_TRANSPARENT);
        tr.left += iw + 6;
      }
    }
    tr.right -= 6;
    if (hi.fmt & (HDF_SORTUP | HDF_SORTDOWN)) tr.right -= 14;
    fmt = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS;
    fmt |= (hi.fmt & HDF_JUSTIFYMASK) == HDF_CENTER ? DT_CENTER : (hi.fmt & HDF_JUSTIFYMASK) == HDF_RIGHT ? DT_RIGHT : DT_LEFT;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, THEME_DARK_TEXT);
    DrawTextA(dc, text, -1, &tr, (UINT)fmt);

    if (hi.fmt & (HDF_SORTUP | HDF_SORTDOWN))        /* the sort arrow */
    {
      int cx = r.right - 12, cy = (r.top + r.bottom) / 2;
      POINT p[3];
      HBRUSH ab = CreateSolidBrush(RGB(200, 200, 200));
      HGDIOBJ ob = SelectObject(dc, ab), opn = SelectObject(dc, GetStockObject(NULL_PEN));
      if (hi.fmt & HDF_SORTUP) { p[0].x = cx - 4; p[0].y = cy + 2; p[1].x = cx + 4; p[1].y = cy + 2; p[2].x = cx; p[2].y = cy - 2; }
      else                     { p[0].x = cx - 4; p[0].y = cy - 2; p[1].x = cx + 4; p[1].y = cy - 2; p[2].x = cx; p[2].y = cy + 2; }
      Polygon(dc, p, 3);
      SelectObject(dc, opn);
      SelectObject(dc, ob);
      DeleteObject(ab);
    }

    pen = CreatePen(PS_SOLID, 1, RGB(0x55, 0x55, 0x55));       /* divider on the right edge */
    op = SelectObject(dc, pen);
    MoveToEx(dc, r.right - 1, r.top + 3, NULL);
    LineTo(dc, r.right - 1, r.bottom - 3);
    SelectObject(dc, op);
    DeleteObject(pen);
}

/* The whole header row painted here in dark mode (rather than relying on the control's own
   themed painting, which kept its color): background, every cell, and the empty part at the end. */
static LRESULT CALLBACK header_paint_subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                                   UINT_PTR id, DWORD_PTR ref)
{
  (void)ref;

  if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, header_paint_subclass_proc, id);

  if (theme_is_dark())
  {
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT || msg == WM_PRINTCLIENT)
    {
      PAINTSTRUCT ps;
      RECT client;
      HDC dc = msg == WM_PAINT ? BeginPaint(hwnd, &ps) : (HDC)wp;
      HFONT f = (HFONT)SendMessage(hwnd, WM_GETFONT, 0, 0);
      HGDIOBJ of = f ? SelectObject(dc, f) : NULL;
      HBRUSH br = CreateSolidBrush(RGB(0x19, 0x19, 0x19));
      int i, cnt = (int)SendMessage(hwnd, HDM_GETITEMCOUNT, 0, 0);

      GetClientRect(hwnd, &client);
      FillRect(dc, &client, br);
      DeleteObject(br);
      for (i = 0; i < cnt; i++)
      {
        RECT r;
        if (SendMessage(hwnd, HDM_GETITEMRECT, (WPARAM)i, (LPARAM)&r)) draw_header_cell(dc, hwnd, i, &r);
      }
      if (of) SelectObject(dc, of);
      if (msg == WM_PAINT) EndPaint(hwnd, &ps);
      return 0;
    }
  }

  /* A repaint only covers what Windows marks as changed, so a column that is shown, hidden or resized
     could leave a speck of the old header behind (seen at the end of the row with a column switched
     off): the whole row is repainted after anything that changes it. */
  {
    LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);

    if (theme_is_dark() &&
        (msg == WM_SIZE || msg == WM_WINDOWPOSCHANGED || msg == HDM_SETITEMA || msg == HDM_SETITEMW ||
         msg == HDM_INSERTITEMA || msg == HDM_INSERTITEMW || msg == HDM_DELETEITEM || msg == HDM_SETORDERARRAY ||
         msg == HDM_LAYOUT))
      InvalidateRect(hwnd, NULL, FALSE);
    return r;
  }
}

LRESULT theme_header_customdraw(NMHDR *hdr)
{
  NMCUSTOMDRAW *cd;
  char class_name[64];

  if (!hdr || hdr->code != NM_CUSTOMDRAW) return -1;

  GetClassNameA(hdr->hwndFrom, class_name, sizeof(class_name));
  if (lstrcmpiA(class_name, "SysHeader32") != 0) return -1;

  if (!theme_is_dark()) return -1;

  cd = (NMCUSTOMDRAW *)hdr;

  if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;

  if (cd->dwDrawStage == CDDS_ITEMPREPAINT)
  {
    SetTextColor(cd->hdc, THEME_DARK_TEXT);

    /* CDRF_NEWFONT, not CDRF_DODEFAULT: it tells the control that the colors
       set on the DC are meant to be used. With DODEFAULT, a header themed with
       "ItemsView" (set above) drew its own black text instead. */
    return CDRF_NEWFONT;
  }

  return CDRF_DODEFAULT;
}

/* ==========================================================================
 * Menu bar strip custom-draw (dark mode only)
 * ==========================================================================
 * Everything above (ForceDark/ForceLight, FlushMenuThemes) covers popup
 * dropdowns -- it does not reach the menu *bar* strip itself, which
 * Windows always paints on its own with no documented way to recolor
 * it. The only way in is to intercept the two undocumented messages it
 * sends when that strip needs painting. Both the messages and the
 * structs they carry are unnamed by any public SDK header; the layout
 * here matches what's been a stable, widely-used reverse-engineering
 * result since Windows 10 (the "win32-darkmode" project on GitHub uses
 * the same one). The trailing arrays in UAHMENUITEM are deliberately
 * oversized past what's actually read (iPosition only) as a margin of
 * safety against the exact tail layout being slightly off.
 * A future Windows update changing this would simply make these two
 * handlers stop firing or paint nothing -- theme_is_dark() still gates
 * both, so light mode is entirely unaffected either way. */

typedef struct
{
  HMENU hmenu;
  HDC   hdc;
  DWORD dwFlags;
} UAHMENU;

typedef struct
{
  int   iPosition;
  DWORD rgsizeBar[4];
  DWORD rgsizePopup[8];
} UAHMENUITEM;

typedef struct
{
  DRAWITEMSTRUCT dis;
  UAHMENU um;
  UAHMENUITEM umi;
} UAHDRAWMENUITEM;

int theme_draw_menu_bar(HWND hwnd, LPARAM lp)
{
  UAHMENU *menu = (UAHMENU *)lp;
  MENUBARINFO mbi;
  RECT rc_window, rect;
  HBRUSH br;

  if (!theme_is_dark()) return 0;
  if (!menu || !menu->hdc) return 0;

  mbi.cbSize = sizeof(mbi);
  if (!GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi)) return 0;

  GetWindowRect(hwnd, &rc_window);

  rect = mbi.rcBar;
  OffsetRect(&rect, -rc_window.left, -rc_window.top);
  rect.right = rc_window.right - rc_window.left;   /* full window width, not just client -- rcBar/client.right both stop short */
  rect.top -= 2;   /* covers a light edge Windows paints in the caption seam just above the bar */

  SelectClipRgn(menu->hdc, NULL);   /* ensure nothing restricts the fill below to less than rect */

  br = CreateSolidBrush(THEME_DARK_MENUBAR);
  FillRect(menu->hdc, &rect, br);
  DeleteObject(br);
  theme_paint_menu_line(hwnd, menu->hdc);

  return 1;
}

/* The 1-pixel line Windows draws between the menu bar and the window's client area is white
   whatever the theme; painted gray here (in window coordinates, so with a window DC). */
void theme_paint_menu_line(HWND hwnd, HDC dc)
{
  RECT rc_client, rc_window, line;
  HBRUSH br;
  HDC own = NULL;

  if (!theme_is_dark() || !GetMenu(hwnd)) return;
  if (!dc) { own = GetWindowDC(hwnd); dc = own; }
  if (!dc) return;

  GetClientRect(hwnd, &rc_client);
  MapWindowPoints(hwnd, NULL, (POINT *)&rc_client, 2);
  GetWindowRect(hwnd, &rc_window);
  OffsetRect(&rc_client, -rc_window.left, -rc_window.top);
  line = rc_client;
  line.bottom = line.top;
  line.top--;
  br = CreateSolidBrush(RGB(0x55, 0x55, 0x55));       /* a gray line, not white and not gone */
  FillRect(dc, &line, br);
  DeleteObject(br);
  if (own) ReleaseDC(hwnd, own);
}

static int bar_forced_hot = -2;       /* set while theme_paint_menubar_selected() paints */

void theme_paint_menubar_selected(HWND hwnd, int sel)
{
  bar_forced_hot = sel;
  theme_paint_menubar(hwnd, 1);
  bar_forced_hot = -2;
}

void theme_paint_menubar(HWND hwnd, int force)
{
  static int last_hot = -2;
  HMENU menu = GetMenu(hwnd);
  MENUBARINFO mbi;
  RECT wr, rect;
  POINT pt;
  HDC dc;
  HBRUSH br;
  HFONT font;
  HGDIOBJ old_font;
  int i, n, hot = -1;

  if (!menu || !theme_is_dark()) { last_hot = -2; return; }
  theme_paint_menu_line(hwnd, NULL);

  mbi.cbSize = sizeof(mbi);
  if (!GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi)) return;
  GetWindowRect(hwnd, &wr);
  GetCursorPos(&pt);
  n = GetMenuItemCount(menu);

  for (i = 0; i < n; i++)
  {
    RECT ir;
    if (GetMenuItemRect(hwnd, menu, (UINT)i, &ir) && PtInRect(&ir, pt)) { hot = i; break; }
  }
  if (bar_forced_hot != -2) hot = bar_forced_hot;
  if (!force && hot == last_hot) return;
  last_hot = hot;

  dc = GetWindowDC(hwnd);
  if (!dc) return;

  rect = mbi.rcBar;
  OffsetRect(&rect, -wr.left, -wr.top);
  rect.right = wr.right - wr.left;
  br = CreateSolidBrush(THEME_DARK_MENUBAR);
  FillRect(dc, &rect, br);
  DeleteObject(br);

  if (theme_menus_owner()) font = theme_menu_font();
  else
  {
    static HFONT menu_font;
    if (!menu_font)
    {
      NONCLIENTMETRICSA ncm;
      memset(&ncm, 0, sizeof(ncm));
      ncm.cbSize = sizeof(ncm);
      if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
        menu_font = CreateFontIndirectA(&ncm.lfMenuFont);
    }
    font = menu_font;
  }
  old_font = font ? SelectObject(dc, font) : NULL;
  SetBkMode(dc, TRANSPARENT);

  for (i = 0; i < n; i++)
  {
    RECT ir;
    char text[128];
    MENUITEMINFOA mii;
    UINT st = GetMenuState(menu, (UINT)i, MF_BYPOSITION);

    if (!GetMenuItemRect(hwnd, menu, (UINT)i, &ir)) continue;
    OffsetRect(&ir, -wr.left, -wr.top);

    memset(&mii, 0, sizeof(mii));
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_STRING | MIIM_FTYPE | MIIM_DATA;
    mii.dwTypeData = text;
    mii.cch = sizeof(text) - 1;
    text[0] = 0;
    GetMenuItemInfoA(menu, (UINT)i, TRUE, &mii);
    if ((mii.fType & MFT_OWNERDRAW) && mii.dwItemData) lstrcpynA(text, (const char *)mii.dwItemData, sizeof(text));

    br = CreateSolidBrush(i == hot ? THEME_DARK_HOT : THEME_DARK_MENUBAR);
    FillRect(dc, &ir, br);
    DeleteObject(br);
    SetTextColor(dc, (st != (UINT)-1 && (st & (MF_GRAYED | MF_DISABLED))) ? THEME_DARK_MUTED : THEME_DARK_TEXT);
    DrawTextA(dc, text, -1, &ir, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }

  if (old_font) SelectObject(dc, old_font);
  ReleaseDC(hwnd, dc);
}

int theme_draw_menu_item(LPARAM lp)
{
  UAHDRAWMENUITEM *item = (UAHDRAWMENUITEM *)lp;
  char text[256];
  MENUITEMINFOA mii;
  RECT rect;
  HBRUSH br;
  int selected, disabled;
  int old_mode;
  int use_big_font = 0;
  HGDIOBJ old_font = NULL;
  COLORREF old_text;

  if (!theme_is_dark()) return 0;
  if (!item) return 0;

  memset(&mii, 0, sizeof(mii));
  mii.cbSize = sizeof(mii);
  mii.fMask = MIIM_STRING | MIIM_STATE;
  mii.dwTypeData = text;
  mii.cch = sizeof(text) - 1;
  text[0] = 0;
  GetMenuItemInfoA(item->um.hmenu, item->umi.iPosition, TRUE, &mii);

  /* With the Large interface the bar titles are owner-drawn: an owner-drawn item has no
     text of its own to ask for (that read comes back empty, which painted the
     titles as blanks until the mouse was over them). Its text is the copy kept
     in itemData, drawn here in the larger font. */
  {
    MENUITEMINFOA od;

    memset(&od, 0, sizeof(od));
    od.cbSize = sizeof(od);
    od.fMask = MIIM_FTYPE | MIIM_DATA;
    if (GetMenuItemInfoA(item->um.hmenu, item->umi.iPosition, TRUE, &od) &&
        (od.fType & MFT_OWNERDRAW) && od.dwItemData)
    {
      lstrcpynA(text, (const char *)od.dwItemData, sizeof(text));
      use_big_font = 1;
    }
  }

  rect = item->dis.rcItem;
  selected = (item->dis.itemState & (ODS_HOTLIGHT | ODS_SELECTED)) != 0;
  disabled = (item->dis.itemState & ODS_DISABLED) != 0;

  br = CreateSolidBrush(selected ? (gui.ui_style ? theme_accent_bg() : THEME_DARK_HOT) : THEME_DARK_MENUBAR);
  FillRect(item->um.hdc, &rect, br);
  DeleteObject(br);

  old_mode = SetBkMode(item->um.hdc, TRANSPARENT);
  old_text = SetTextColor(item->um.hdc, disabled ? THEME_DARK_MUTED : THEME_DARK_TEXT);

  if (use_big_font) old_font = SelectObject(item->um.hdc, theme_menu_font());
  DrawTextA(item->um.hdc, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | ((item->dis.itemState & ODS_NOACCEL) ? DT_HIDEPREFIX : 0));
  if (use_big_font) SelectObject(item->um.hdc, old_font);

  SetTextColor(item->um.hdc, old_text);
  SetBkMode(item->um.hdc, old_mode);

  return 1;
}

/****************************************************************************
 * Owner-drawn menu items for large_ui
 *
 * A completely different, much older and more conservative mechanism
 * than the WM_UAHDRAWMENU family above -- MF_OWNERDRAW has been part of
 * documented Win32 since Windows 3.1, driven by the standard
 * WM_MEASUREITEM/WM_DRAWITEM messages, not an undocumented, guessed
 * struct layout. Used here (rather than the UAH approach) specifically
 * because an earlier attempt at resizing the menu via WM_UAHMEASUREMENUITEM
 * broke the menu entirely on real Windows -- this trades a little visual
 * conformity (Windows draws less of the chrome around each item for us)
 * for something that can't silently corrupt the menu's own internal
 * layout data the way writing to a guessed struct could.
 ****************************************************************************/

/* Item text is stored here at conversion time (not re-queried at
   measure/draw time) because popup items -- "Select State Slot", "Theme", and
   so on -- have no usable command ID to look themselves up by; only
   leaf command items do. Storing the string once, up front, works
   uniformly for both. Never freed -- one-time setup for the app's
   lifetime, same as the cached large font. */
/* The Style the menu bar in use was made for: a Style changed while a menu is open takes effect when the bar is made again, and what
   is open is still drawn the way it was made. -1 until the first bar is made. */
static int menus_made_style = -1;

/* The menus are drawn by the program (and so use its font) with the Large interface or the Modern style; otherwise the system draws them. */
int theme_menus_owner(void)
{
  return gui.large_ui || (menus_made_style < 0 ? gui.ui_style : menus_made_style);
}

static void convert_item_to_ownerdraw(HMENU menu, int pos)
{
  MENUITEMINFOA mii;
  char text[128];
  char *stored;

  memset(&mii, 0, sizeof(mii));
  mii.cbSize = sizeof(mii);
  mii.fMask = MIIM_STRING | MIIM_FTYPE;
  mii.dwTypeData = text;
  mii.cch = sizeof(text) - 1;
  text[0] = '\0';

  if (!GetMenuItemInfoA(menu, (UINT)pos, TRUE, &mii)) return;
  if (mii.fType & MFT_OWNERDRAW) return;                 /* already done */

  if (mii.fType & MFT_SEPARATOR)
  {
    mii.fMask = MIIM_FTYPE | MIIM_DATA;
    mii.fType |= MFT_OWNERDRAW;
    mii.dwItemData = 0;   /* NULL itemData marks this a separator at draw time */
    SetMenuItemInfoA(menu, (UINT)pos, TRUE, &mii);
    return;
  }

  stored = (char *)malloc((size_t)lstrlenA(text) + 1);
  if (!stored) return;
  lstrcpyA(stored, text);

  mii.fMask = MIIM_FTYPE | MIIM_DATA | MIIM_BITMAP;      /* (a picture in the menu's own bitmap slot would make Windows size the menu to it) */
  mii.fType |= MFT_OWNERDRAW;
  mii.dwItemData = (ULONG_PTR)stored;
  mii.hbmpItem = NULL;
  SetMenuItemInfoA(menu, (UINT)pos, TRUE, &mii);
}

/* Recurses into every submenu (Select State Slot, Theme, Aspect Ratio, and so
   on) so the whole menu tree is owner-drawn consistently, not just the
   top level. Call once, at startup, only when large_ui is on. */
static void menu_frame_hook_install(void);

void theme_ownerdraw_menu(HMENU menu)
{
  int count, i;

  if (!menu) return;
  menu_frame_hook_install();
  count = GetMenuItemCount(menu);
  if (count < 0) return;

  for (i = 0; i < count; i++)
  {
    HMENU submenu = GetSubMenu(menu, i);

    convert_item_to_ownerdraw(menu, i);

    if (submenu) theme_ownerdraw_menu(submenu);
  }
}

static int menu_item_position(HMENU menu, ULONG_PTR item_data);

static HMENU bar_menu;                  /* the menu bar, known before it is attached to the window */

void theme_set_menu_bar(HMENU menu) { bar_menu = menu; menus_made_style = gui.ui_style; }

static void own_icon_set(ULONG_PTR key, HBITMAP b);

/* Gives the system back its own drawing of a menu (the Modern font in the menus was switched off). */
void theme_menu_forget(ULONG_PTR key)
{
  if (key) own_icon_set(key, NULL);
}

void theme_unconvert_menu(HMENU menu)
{
  int count, i;

  if (!menu) return;
  count = GetMenuItemCount(menu);
  for (i = 0; i < count; i++)
  {
    HMENU sub = GetSubMenu(menu, i);
    MENUITEMINFOA mii;

    if (sub) theme_unconvert_menu(sub);
    memset(&mii, 0, sizeof(mii));
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_FTYPE | MIIM_DATA;
    if (!GetMenuItemInfoA(menu, (UINT)i, TRUE, &mii) || !(mii.fType & MFT_OWNERDRAW)) continue;
    if (!mii.dwItemData)
    {
      mii.fMask = MIIM_FTYPE;
      mii.fType = (mii.fType & ~MFT_OWNERDRAW) | MFT_SEPARATOR;
    }
    else
    {
      char *text = (char *)mii.dwItemData;
      mii.fMask = MIIM_FTYPE | MIIM_STRING | MIIM_DATA;
      mii.fType &= ~MFT_OWNERDRAW;
      mii.dwTypeData = text;
      mii.dwItemData = 0;
      SetMenuItemInfoA(menu, (UINT)i, TRUE, &mii);
      own_icon_set((ULONG_PTR)text, NULL);
      free(text);
      continue;
    }
    SetMenuItemInfoA(menu, (UINT)i, TRUE, &mii);
  }
}

int theme_measure_menu_ownerdraw(LPARAM lp)
{
  MEASUREITEMSTRUCT *mis = (MEASUREITEMSTRUCT *)lp;
  HDC hdc;
  HFONT font, old_font;
  SIZE sz;
  char *text;
  int dpi;

  if (!theme_menus_owner()) return 0;
  if (!mis || mis->CtlType != ODT_MENU) return 0;

  text = (char *)mis->itemData;

  if (!text)
  {
    /* Separator -- fixed, small height regardless of font size. */
    mis->itemWidth = 8;
    mis->itemHeight = 6;
    return 1;
  }

  hdc = GetDC(NULL);
  font = theme_menu_font();
  old_font = (HFONT)SelectObject(hdc, font);
  {
    char plain[256], *o = plain;
    const char *p;

    for (p = text; *p && o < plain + sizeof(plain) - 1; p++)       /* the "&" of a mnemonic is not drawn: it is not part of the width */
    {
      if (*p == '&' && p[1]) { p++; if (*p != '&' ) { *o++ = *p; continue; } }
      *o++ = *p;
    }
    *o = '\0';
    GetTextExtentPoint32A(hdc, plain, lstrlenA(plain), &sz);
    if (!strncmp(plain, "Slot ", 5))                  /* a state slot: wide enough for a date too (the menu is measured before its dates are filled in) */
    {
      char wide[128] = "Slot 0 - 00:00:00  00/00/0000";
      const char *tab = strchr(plain, '\t');
      SIZE ws;

      if (tab) lstrcpynA(wide + lstrlenA(wide), tab, (int)sizeof(wide) - lstrlenA(wide));
      GetTextExtentPoint32A(hdc, wide, lstrlenA(wide), &ws);
      if (ws.cx > sz.cx) sz.cx = ws.cx;
    }
  }
  SelectObject(hdc, old_font);
  dpi = GetDeviceCaps(hdc, LOGPIXELSX);
  ReleaseDC(NULL, hdc);

  /* Margin beyond the raw text size for the selection highlight and
     checkmark/submenu-arrow gutter, the same way Windows' own default
     menu metrics include room beyond the text itself. */
  {
    int on_bar = menu_item_position(bar_menu ? bar_menu : GetMenu(g_hwnd), (ULONG_PTR)text) >= 0;

    if (gui.large_ui)
    {
      /* The Large interface: the same idea, with roomier rows; Windows adds about 15 pixels (at 100 %) of its own to a title on the bar. */
      mis->itemWidth  = on_bar ? (UINT)(sz.cx + 4 * dpi / 96) : (UINT)sz.cx + 36;     /* a dropdown item: room for the picture gutter and the arrow */
      mis->itemHeight = on_bar ? (UINT)(sz.cy + 12) : (UINT)(sz.cy * 1.25);                 /* (a title on the bar has room for the tails of g, p and y) */
    }
    else
    {
      /* Normal size (the Modern font in the menus): the spacing the system gives its own menus -- a few pixels around a title on the
         bar, and a dropdown row about as high as its text plus 5. */
      /* (On the bar Windows adds about 15 pixels of its own to an owner-drawn title -- seen in Windows 11 at 100 % -- so the title is
         made narrower by 2, which leaves the 13 pixels between two titles that the system menu has.) */
      mis->itemWidth  = on_bar ? (UINT)(sz.cx - 2 * dpi / 96 > 4 ? sz.cx - 2 * dpi / 96 : 4) : (UINT)sz.cx + 32;
      mis->itemHeight = (UINT)(sz.cy + 5);
    }
  }

  return 1;
}

/* An item's position in its menu, found by the string pointer stored as its
   data (unique to it -- there is no usable command ID for a popup item).
   Used both by menu_item_has_submenu() below and by the top-level bar's
   trailing-space fill, which needs to know whether an item is the last one
   on its own row. */
static int menu_item_position(HMENU menu, ULONG_PTR item_data)
{
  int count, i;

  if (!menu || !item_data) return -1;

  count = GetMenuItemCount(menu);
  for (i = 0; i < count; i++)
  {
    MENUITEMINFOA mii;

    memset(&mii, 0, sizeof(mii));
    mii.cbSize = sizeof(mii);
    mii.fMask  = MIIM_DATA;

    if (GetMenuItemInfoA(menu, (UINT)i, TRUE, &mii) && mii.dwItemData == item_data) return i;
  }
  return -1;
}

/* Popup items ("Region", "Theme", "Aspect Ratio", ...) have no usable command
   ID, but the string pointer stored as each item's data is unique to it, so
   the item is found in its menu by that. */
static int menu_item_has_submenu(HMENU menu, ULONG_PTR item_data)
{
  int count, i;

  if (!menu || !item_data) return 0;

  count = GetMenuItemCount(menu);
  for (i = 0; i < count; i++)
  {
    MENUITEMINFOA mii;

    memset(&mii, 0, sizeof(mii));
    mii.cbSize = sizeof(mii);
    mii.fMask  = MIIM_DATA | MIIM_SUBMENU;

    if (GetMenuItemInfoA(menu, (UINT)i, TRUE, &mii) && mii.dwItemData == item_data)
    {
      return mii.hSubMenu != NULL;
    }
  }

  return 0;
}

/* Windows draws a submenu's arrow itself, in black, right after an
   owner-drawn item has been painted -- which vanishes against a dark item.
   So in dark mode the arrow is drawn here in the text color, and its area is
   then clipped out of the DC so the system's black one cannot go on top.

   It is drawn to match the system's own arrow, measured from a light-mode
   screenshot at a 30 px item height: a staircase triangle 4 px wide and 7 px
   tall (column 0 spans 7 rows, then 5, 3 and 1), its tip 8 px in from the
   item's right edge and its middle about 2 px above the item's center. The
   size scales with the item height so it stays in proportion. */
static void draw_dark_submenu_arrow(HDC hdc, const RECT *r, int disabled)
{
  int h    = r->bottom - r->top;
  int half = (3 * h + 15) / 30;                     /* 3 at a 30 px item */
  int tip  = r->right - 8;                          /* x of the tip's column */
  int cy   = (r->top + r->bottom) / 2 - (h + 7) / 15;   /* 2 px above center at 30 px */
  int zone = (h > 24) ? h : 24;
  COLORREF color = disabled ? THEME_DARK_MUTED : THEME_DARK_TEXT;
  HBRUSH br = CreateSolidBrush(color);
  int i;

  if (half < 2) half = 2;

  for (i = 0; i <= half; i++)
  {
    int ext = half - i;
    RECT col;

    col.left   = tip - half + i;
    col.right  = col.left + 1;
    col.top    = cy - ext;
    col.bottom = cy + ext + 1;
    FillRect(hdc, &col, br);
  }

  DeleteObject(br);

  ExcludeClipRect(hdc, r->right - zone, r->top, r->right, r->bottom);
}

/* The pictures of the owner-drawn (Large interface) menu items, by the item's data: the menu's own bitmap slot makes Windows size the
   whole menu to the picture, so the items keep them here and paint them themselves. */
static struct { ULONG_PTR key; HBITMAP bmp; } own_icons[8192];
static int own_icon_n;

static HBITMAP own_icon_get(ULONG_PTR key)
{
  int i;
  for (i = 0; i < own_icon_n; i++) if (own_icons[i].key == key) return own_icons[i].bmp;
  return NULL;
}

/* An item's text was replaced (main.c, menu_set_text): its picture follows the new pointer. */
void theme_menu_text_moved(ULONG_PTR old_key, ULONG_PTR new_key)
{
  int i;
  for (i = 0; i < own_icon_n; i++) if (own_icons[i].key == old_key) { own_icons[i].key = new_key; return; }
}

static void own_icon_set(ULONG_PTR key, HBITMAP b)
{
  int i;
  for (i = 0; i < own_icon_n; i++) if (own_icons[i].key == key) { own_icons[i].bmp = b; return; }
  if (own_icon_n < (int)(sizeof(own_icons) / sizeof(own_icons[0]))) { own_icons[own_icon_n].key = key; own_icons[own_icon_n].bmp = b; own_icon_n++; }
}

/* The background of the menus the program draws in the light theme: what the system menus have on Windows 11 (white for the bar; a drop-down
   #F3F3F3, a little grayer than the system's #F9F9F9, as chosen), the system colors on an older Windows. */
static COLORREF menu_light_bg(int bar)
{
  static int win11 = -1;

  if (win11 < 0)
  {
    typedef LONG (WINAPI *rtl_t)(OSVERSIONINFOW *);
    OSVERSIONINFOW vi;
    rtl_t fn = (rtl_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");

    memset(&vi, 0, sizeof(vi));
    vi.dwOSVersionInfoSize = sizeof(vi);
    win11 = fn && fn(&vi) == 0 && vi.dwBuildNumber >= 22000;
  }
  if (win11) return bar ? RGB(0xFF, 0xFF, 0xFF) : RGB(0xF3, 0xF3, 0xF3);
  return GetSysColor(bar ? COLOR_MENUBAR : COLOR_MENU);
}

/* The icon font at a size, kept (one at a time): the check marks of the owner-drawn menus. NULL when no icon font is installed. */
static HFONT menu_check_font(int px)
{
  static HFONT f;
  static int for_px;

  if (px < 10) px = 10;
  if (f && for_px == px) return f;
  if (f) { DeleteObject(f); f = NULL; }
  /* Segoe UI Symbol has the heavy check mark (U+2714): it stays clear at menu size, unlike the thin one of the icon font. */
  f = CreateFontW(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Symbol");
  for_px = px;
  return f;
}

int theme_draw_menu_ownerdraw(LPARAM lp)
{
  DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lp;
  char *text;
  int selected, disabled, checked, dark;
  HBRUSH br;
  RECT r;

  if (!theme_menus_owner()) return 0;
  if (!dis || dis->CtlType != ODT_MENU) return 0;

  text     = (char *)dis->itemData;
  selected = (dis->itemState & ODS_SELECTED) != 0;
  disabled = (dis->itemState & ODS_DISABLED) != 0;
  checked  = (dis->itemState & ODS_CHECKED) != 0;
  dark     = theme_is_dark();
  r        = dis->rcItem;

  {
    int on_bar = (HMENU)dis->hwndItem == GetMenu(g_hwnd);
    COLORREF bgc = dark ? (on_bar ? THEME_DARK_MENUBAR : THEME_DARK_MENU) : menu_light_bg(on_bar);        /* (the same with the Large interface or not) */
    COLORREF fillc = !selected ? bgc
                   : (gui.ui_style && !disabled) ? theme_accent_bg()                                  /* the Modern style: the accent color (not on a grayed item: its text would not read) */
                   : dark ? THEME_DARK_HOT : (disabled ? RGB(0xE5, 0xE5, 0xE5) : GetSysColor(COLOR_HIGHLIGHT));

    br = CreateSolidBrush(fillc);
    if (selected && gui.ui_style && !on_bar)
    {
      /* The Modern style: the chosen item of a drop-down is a rounded patch (a title on the bar stays a plain rectangle), with a little room round it, drawn smooth. */
      mod_ss ss;
      int w = r.right - r.left, h = r.bottom - r.top;
      int ix = mod_dpi_px(dis->hDC, 4), iy = mod_dpi_px(dis->hDC, 2);

      if (mod_ss_begin(&ss, dis->hDC, w, h, bgc))
      {
        mod_ss_round(&ss, ix, iy, w - 2 * ix, h - 2 * iy, (double)mod_dpi_px(dis->hDC, 4), fillc, CLR_INVALID, 0);
        mod_ss_end(&ss, dis->hDC, r.left, r.top);
      }
      else FillRect(dis->hDC, &r, br);
    }
    else FillRect(dis->hDC, &r, br);
  }

  if ((HMENU)dis->hwndItem == GetMenu(g_hwnd))
  {
    /* Fill from this item's right edge to the window's right edge, but only
       when this item is the last one on its own row -- either the very last
       item in the menu, or immediately followed by an item that wrapped to a
       new row. Doing this for every item would be the bug: Windows repaints
       only the item whose state actually changed (e.g. just "Emulation" when
       its dropdown opens), not the whole bar, so filling rightward from any
       item that has another item sitting to its right would erase that
       neighbor, which is not due to repaint at this moment.

       This used to be decided by comparing GetMenuStringA()'s text for the
       last position against this item's own text -- but GetMenuStringA()
       does not return anything useful once an item has been converted to
       owner-draw (which every item here was, back at startup), so that
       comparison could never actually match and this fill silently never
       ran except where it happened not to be needed. Position, found the
       same way menu_item_has_submenu() finds it (by the item's unique
       itemData pointer), does not have that problem. */
    HMENU top_menu = (HMENU)dis->hwndItem;
    int count = GetMenuItemCount(top_menu);
    int pos = menu_item_position(top_menu, (ULONG_PTR)text);
    int is_last_on_row = 0;

    if (pos >= 0)
    {
      if (pos == count - 1)
      {
        is_last_on_row = 1;   /* the very last item in the menu */
      }
      else
      {
        RECT this_r, next_r;
        if (GetMenuItemRect(g_hwnd, top_menu, (UINT)pos, &this_r) &&
            GetMenuItemRect(g_hwnd, top_menu, (UINT)(pos + 1), &next_r) &&
            next_r.top != this_r.top)
        {
          is_last_on_row = 1;   /* the next item wrapped to a new row */
        }
      }
    }

    if (is_last_on_row)
    {
      RECT rc_window;
      HBRUSH bg_br = CreateSolidBrush(dark ? THEME_DARK_MENUBAR : menu_light_bg(1));   /* always the plain background, never the highlight color (with the Modern font in the menus the bar is one color to its end) */
      GetWindowRect(g_hwnd, &rc_window);
      {
        int full_width = rc_window.right - rc_window.left;
        if (r.right < full_width)
        {
          RECT trailing = r;
          trailing.left = r.right;
          trailing.right = full_width;   /* full window width, not just client -- same fix as theme_draw_menu_bar */
          /* A couple of pixels above this item's own top, not all the way up to
             row 0: reaching above this item's own row would erase whatever an
             earlier, unrelated row's items already painted. */
          trailing.top = (r.top > 2) ? (r.top - 2) : 0;
          FillRect(dis->hDC, &trailing, bg_br);
        }
      }
      DeleteObject(bg_br);
    }
  }

  DeleteObject(br);

  if (!text)   /* separator */
  {
    HPEN pen = CreatePen(PS_SOLID, 1, dark ? RGB(80, 80, 80) : GetSysColor(COLOR_3DSHADOW));
    HPEN old_pen = (HPEN)SelectObject(dis->hDC, pen);
    int mid = (r.top + r.bottom) / 2;

    MoveToEx(dis->hDC, r.left + 4, mid, NULL);
    LineTo(dis->hDC, r.right - 4, mid);
    SelectObject(dis->hDC, old_pen);
    DeleteObject(pen);
    return 1;
  }

  {
    HFONT font = theme_menu_font();
    HFONT old_font = (HFONT)SelectObject(dis->hDC, font);
    RECT text_rect = r;
    UINT hide_prefix = (dis->itemState & ODS_NOACCEL) ? DT_HIDEPREFIX : 0;      /* the mnemonic underlines show only when the system asks (Alt pressed) */

    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, dark
        ? (disabled ? THEME_DARK_MUTED : THEME_DARK_TEXT)
        : GetSysColor(disabled ? COLOR_GRAYTEXT : (selected ? COLOR_HIGHLIGHTTEXT : COLOR_MENUTEXT)));
    if (selected && gui.ui_style && !disabled) SetTextColor(dis->hDC, RGB(255, 255, 255));        /* (on the accent color) */

    /* Room for the checkmark gutter in a dropdown; the menu bar's own titles have
       none and are centered, or the text would be pushed out of its box. */
    if ((HMENU)dis->hwndItem == GetMenu(g_hwnd))
    {
      /* The system clips an owner-drawn title to its row; the tails of g, p and y are let out a little below it. */
      HRGN rg = CreateRectRgn(r.left, r.top, r.right, r.bottom + 6);
      SelectClipRgn(dis->hDC, rg);
      DeleteObject(rg);
      if (gui.large_ui)                      /* (the row the system gives a title is shorter than the big font: the text sits a little higher, so the tails of g, p and y fit) */
      {
        text_rect.top -= 2;
        text_rect.bottom -= 2;
      }
      DrawTextA(dis->hDC, text, -1, &text_rect, DT_VCENTER | DT_SINGLELINE | DT_CENTER | DT_NOCLIP | hide_prefix);
      SelectClipRgn(dis->hDC, NULL);
    }
    else
      text_rect.left += 24;

    if ((HMENU)dis->hwndItem != GetMenu(g_hwnd))
    {
      char *tab = strchr(text, '\t');
      if (tab)
      {
        RECT shortcut_rect = text_rect;
        *tab = '\0';
        DrawTextA(dis->hDC, text, -1, &text_rect, DT_VCENTER | DT_SINGLELINE | DT_LEFT | hide_prefix);
        shortcut_rect.right -= 8;
        /* A shortcut hint reads as a secondary detail next to the item's own
           label, not something that should compete with it for attention --
           confirmed by direct testing, drawing it in the same bright color as
           the label (as an enabled, checked item like Rewind otherwise would)
           looked like the two were equally important. Disabled rows keep
           their single, already-dim color throughout; this only affects
           enabled ones. */
        if (!disabled && !selected) SetTextColor(dis->hDC, dark ? RGB(150, 150, 150) : GetSysColor(COLOR_GRAYTEXT));       /* (the chosen item's own text color reads on its highlight: the dim one did not) */
        DrawTextA(dis->hDC, tab + 1, -1, &shortcut_rect, DT_VCENTER | DT_SINGLELINE | DT_RIGHT);
        *tab = '\t';   /* restore -- this string is reused every time the item repaints */
      }
      else
      {
        DrawTextA(dis->hDC, text, -1, &text_rect, DT_VCENTER | DT_SINGLELINE | DT_LEFT | hide_prefix);
      }
    }

    if ((HMENU)dis->hwndItem != GetMenu(g_hwnd))
    {
      /* The picture of the item (the same ones the other menus show, set as the item's bitmap), in the gutter before the text;
         with a picture the item's check mark is the box the picture is drawn on, so no tick is drawn. */
      HBITMAP pic = own_icon_get((ULONG_PTR)text);

      {
        {
          BITMAP bm;

          if (pic && GetObject(pic, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight > 0)
          {
            HDC mem = CreateCompatibleDC(dis->hDC);
            HGDIOBJ oldb = SelectObject(mem, pic);
            BLENDFUNCTION bf;
            int x = r.left + (24 - bm.bmWidth) / 2, y = r.top + ((r.bottom - r.top) - bm.bmHeight) / 2;

            if (x < r.left + 1) x = r.left + 1;
            bf.BlendOp = AC_SRC_OVER; bf.BlendFlags = 0; bf.SourceConstantAlpha = disabled ? 110 : 255; bf.AlphaFormat = AC_SRC_ALPHA;
            AlphaBlend(dis->hDC, x, y, bm.bmWidth, bm.bmHeight, mem, 0, 0, bm.bmWidth, bm.bmHeight, bf);
            SelectObject(mem, oldb);
            DeleteDC(mem);
            checked = 0;
          }
        }
      }
    }

    if (checked)
    {
      /* Hand-drawn rather than DrawFrameControl -- that draws a fixed,
         always-light background square behind the glyph regardless of
         theme, which stood out against a dark item background. This
         way nothing is drawn but the checkmark itself, in a color that
         matches the surrounding text. */
      int px = (r.bottom - r.top) * 5 / 8;                       /* the size of the tick follows the row */
      HFONT icons = menu_check_font(px);
      COLORREF tick = dark ? THEME_DARK_TEXT : GetSysColor(COLOR_MENUTEXT);

      MENUITEMINFOA rmi;
      int radio;

      if (selected && gui.ui_style) tick = RGB(255, 255, 255);
      ZeroMemory(&rmi, sizeof(rmi));
      rmi.cbSize = sizeof(rmi);
      rmi.fMask = MIIM_FTYPE;
      radio = GetMenuItemInfoA((HMENU)dis->hwndItem, dis->itemID, FALSE, &rmi) && (rmi.fType & MFT_RADIOCHECK);
      if (radio)
      {
        /* One choice of several (a radio item): a dot, not a check mark. */
        int rad = (r.bottom - r.top) / 7 + 1, cx = r.left + 12, cy = (r.top + r.bottom) / 2;
        HBRUSH dot = CreateSolidBrush(tick);
        HPEN dpen = CreatePen(PS_SOLID, 1, tick);
        HGDIOBJ ob = SelectObject(dis->hDC, dot), op = SelectObject(dis->hDC, dpen);

        Ellipse(dis->hDC, cx - rad, cy - rad, cx + rad + 1, cy + rad + 1);
        SelectObject(dis->hDC, ob); SelectObject(dis->hDC, op);
        DeleteObject(dot); DeleteObject(dpen);
      }
      else if (icons)
      {
        /* The heavy check mark of Segoe UI Symbol, drawn with smoothing (a pen draws it jagged). */
        RECT box = r;
        HGDIOBJ of = SelectObject(dis->hDC, icons);

        box.right = box.left + 24;
        SetTextColor(dis->hDC, tick);
        DrawTextW(dis->hDC, L"\x2714", 1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dis->hDC, of);
      }
      else
      {
        HPEN pen = CreatePen(PS_SOLID, 2, tick);
        HPEN old_pen = (HPEN)SelectObject(dis->hDC, pen);
        int cx = r.left + 12;
        int cy = (r.top + r.bottom) / 2;

        MoveToEx(dis->hDC, cx - 5, cy, NULL);
        LineTo(dis->hDC, cx - 2, cy + 4);
        LineTo(dis->hDC, cx + 5, cy - 5);

        SelectObject(dis->hDC, old_pen);
        DeleteObject(pen);
      }
    }

    /* Submenu arrow, dark mode only (the system's black one is right on a
       light menu). Never the menu bar itself, which has no arrows. */
    if (dark && (HMENU)dis->hwndItem != GetMenu(g_hwnd) &&
        menu_item_has_submenu((HMENU)dis->hwndItem, dis->itemData))
    {
      draw_dark_submenu_arrow(dis->hDC, &r, disabled);
    }

    SelectObject(dis->hDC, old_font);
  }

  return 1;
}

/****************************************************************************
 * Menu icons
 *
 * A small picture beside the text of each action in the menus, drawn from the icon font Windows ships (Segoe Fluent Icons
 * on Windows 11, Segoe MDL2 Assets on Windows 10). Each glyph is rendered once into a bitmap with its own alpha, in the
 * color of its kind of action (menu_icon_color), and handed to the menu item (MIIM_BITMAP); the system lays the menu out around it. Choices that
 * carry a check mark or a radio dot have none: the mark and a picture would share one place. Not used with the Large interface,
 * whose owner-drawn menus have no room for it, nor when neither font is installed.
 ****************************************************************************/

/* Not a font glyph: the picture of the toolbar's Reset button, so the menu item looks the same. */
#define MENU_ICON_TOOLBAR_RESET 0xFFF0
#define MENU_ICON_TOOLBAR_REFRESH 0xFFF1      /* (also used by browser.c for the right-click menu, as 0xFFF1) */

typedef struct { int id; unsigned short glyph; } menu_icon_id;
typedef struct { const char *title; unsigned short glyph; } menu_icon_title;

static const menu_icon_id menu_icon_ids[] =
{
  { IDM_FILE_OPEN, 0xE8E5 }, { IDM_FILE_BROWSER, 0xE838 }, { IDM_EMU_INSERTDISC, 0xE710 },
  { IDM_FILE_ROMINFO, 0xE946 }, { IDM_FILE_APPLYPATCH, 0xE90F }, { IDM_FILE_OPENDIR, 0xE8B7 }, { IDM_FILE_EXIT, 0xE7E8 },
  { IDM_EMU_PAUSE, 0xE769 }, { IDM_EMU_STOP, 0xE71A }, { IDM_EMU_RESET, MENU_ICON_TOOLBAR_RESET }, { IDM_EMU_HARDRESET, 0xE7E8 },
  { IDM_FILE_LOADSTATE, 0xE896 }, { IDM_FILE_LOADSTATEFILE, 0xE838 }, { IDM_EMU_UNDOLOAD, 0xE7A7 },
  { IDM_FILE_SAVESTATE, 0xE74E }, { IDM_FILE_SAVESTATEFILE, 0xE792 }, { IDM_EMU_UNDOSAVE, 0xE7A7 }, { IDM_FILE_STATEMGR, 0xE81C },
  { IDM_EMU_FASTFWD, 0xEB9D }, { IDM_EMU_REWINDNOW, 0xEB9E },
  { IDM_EMU_SPEEDDOWN, 0xE738 }, { IDM_EMU_SPEEDRESET, 0xE72C }, { IDM_EMU_SPEEDUP, 0xE710 },
  { IDM_VIEW_RESETWINDOW, 0xE923 },
  { IDM_VIDEO_FULLSCREEN, 0xE740 }, { IDM_VIDEO_SHADER_PRESET, 0xE8E5 }, { IDM_VIDEO_SHADERPARAMS, 0xE713 },
  { IDM_AUDIO_SETTINGS, 0xEA41 },
  { IDM_INPUT_P1, 0xE7FC }, { IDM_INPUT_HOTKEYS, 0xE765 },
  { IDM_EMU_CHEATS, 0xE8EC }, { IDM_EMU_CHEATSEARCH, 0xE721 }, { IDM_TOOLS_NETPLAY, 0xE968 }, { IDM_TOOLS_NETPLAY_STOP, 0xE711 },
  { IDM_FILE_REC_GIF, 0xE722 }, { IDM_FILE_REC_VIDEO, 0xE714 }, { IDM_FILE_REC_AUDIO, 0xE720 }, { IDM_TOOLS_VGM, 0xE8D6 }, { IDM_FILE_REC_STOP, 0xE71A },
  { IDM_TOOLS_MOVIE_REC, 0xE7FC }, { IDM_TOOLS_MOVIE_PLAY, 0xE768 }, { IDM_TOOLS_MOVIE_STOP, 0xE71A },
  { IDM_FILE_SCREENSHOT, 0xE722 },
  { IDM_OPTIONS_SETTINGS, 0xE713 },
  { IDM_VIDEO_FULLSCREEN_START, 0xE92D }, { IDM_VIDEO_PICTUREADJ, 0xE790 }, { IDM_INPUT_BACKGROUND, 0xE80C }, { IDM_EMU_PAUSE_UNFOCUSED, 0xF2D9 }, { IDM_EMU_AUTOSAVE, 0xF470 }, { IDM_VIDEO_ALWAYS_ON_TOP, 0xE718 }, { IDM_VIDEO_VSYNC, 0xEABB },
  { IDM_HELP_SHORTCUTS, 0xE765 }, { IDM_HELP_USERGUIDE, 0xE897 }, { IDM_HELP_ABOUT, 0xE946 },
};

static const menu_icon_title menu_icon_titles[] =
{
  { "Recent Files", 0xE81C }, { "Load State", 0xE896 }, { "Save State", 0xE74E }, { "Select State Slot", 0xE71D },
  { "Load State from Slot", 0xE71D }, { "Save State to Slot", 0xE71D }, { "Load Backup State", 0xE81C },
  { "Rewind", 0xEB9E }, { "Game Speed", 0xE916 },
  { "View Mode", 0xE8A9 }, { "Theme", 0xE790 }, { "Style", 0xE771 }, { "Toolbars", 0xE737 }, { "Game List", 0xE8FD },
  { "List Spacing", 0xE8CB }, { "List Columns", 0xE8FD },
  { "Window Size", 0xE73F }, { "Render Filter", 0xE71C }, { "Shaders", 0xEA99 }, { "Visual Effects", 0xE794 },
  { "Scanlines", 0xE700 }, { "NTSC Filter", 0xE7F4 }, { "Aspect Ratio", 0xE799 },
  { "Borders", 0xE739 }, { "Interlaced Mode", 0xE8CB }, { "Frameskip", 0xE84F }, { "Renderer", 0xF1BA },
  { "Mute", 0xE74F }, { "Console Model", 0xE967 }, { "Tone and Effects", 0xE767 }, 
  
  { "Record", 0xE714 },
  { "Region", 0xE774 }, { "Force VDP Mode", 0xE7F4 }, { "Console", 0xE967 },
};

static HBITMAP menu_icon_bmp[96];
static HBITMAP menu_icon_bmp_on[96];        /* the same picture on a colored box, for a choice that is switched on */
static unsigned short menu_icon_glyph[96];
static int menu_icon_n;

/* The icon font that is installed, or NULL. */
static HFONT menu_icon_font(int px)
{
  static const wchar_t *const faces[] = { L"Segoe Fluent Icons", L"Segoe MDL2 Assets" };
  int i;

  for (i = 0; i < 2; i++)
  {
    HFONT f = CreateFontW(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, faces[i]);
    if (f)
    {
      HDC dc = CreateCompatibleDC(NULL);
      wchar_t got[LF_FACESIZE] = L"";
      HGDIOBJ old = SelectObject(dc, f);
      GetTextFaceW(dc, LF_FACESIZE, got);
      SelectObject(dc, old);
      DeleteDC(dc);
      if (!lstrcmpiW(got, faces[i])) return f;
      DeleteObject(f);
    }
  }
  return NULL;
}

static HBITMAP menu_icon_make(HFONT font, unsigned short glyph, int sz, COLORREF fg)
{
  BITMAPINFO bi;
  unsigned char *bits;
  HBITMAP bmp;
  HDC dc;
  HGDIOBJ oldbmp, oldfont;
  RECT r;
  wchar_t ch[2];
  int i;

  ZeroMemory(&bi, sizeof(bi));
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = sz;
  bi.bmiHeader.biHeight = -sz;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  dc = CreateCompatibleDC(NULL);
  bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
  if (!bmp) { DeleteDC(dc); return NULL; }

  ZeroMemory(bits, (size_t)sz * sz * 4);
  oldbmp = SelectObject(dc, bmp);
  oldfont = SelectObject(dc, font);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(255, 255, 255));            /* white on black: the brightness becomes the alpha below */
  ch[0] = (wchar_t)glyph; ch[1] = 0;
  r.left = 0; r.top = 0; r.right = sz; r.bottom = sz;
  DrawTextW(dc, ch, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  GdiFlush();

  for (i = 0; i < sz * sz; i++)
  {
    unsigned char *p = bits + (size_t)i * 4;
    unsigned a = p[0] > p[1] ? p[0] : p[1];
    if (p[2] > a) a = p[2];
    p[0] = (unsigned char)(GetBValue(fg) * a / 255);   /* premultiplied, as menu bitmaps with alpha need */
    p[1] = (unsigned char)(GetGValue(fg) * a / 255);
    p[2] = (unsigned char)(GetRValue(fg) * a / 255);
    p[3] = (unsigned char)a;
  }
  SelectObject(dc, oldfont);
  SelectObject(dc, oldbmp);
  DeleteDC(dc);
  return bmp;
}

/* The toolbar's own picture (drawn in code there) as a menu bitmap: premultiplied, transparent around it. */
static HBITMAP menu_icon_make_toolbar(int icon, int sz, COLORREF fg)
{
  BITMAPINFO bi;
  unsigned char *bits;
  HBITMAP bmp;
  HDC dc;
  HGDIOBJ oldbmp;

  ZeroMemory(&bi, sizeof(bi));
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = sz;
  bi.bmiHeader.biHeight = -sz;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  dc = CreateCompatibleDC(NULL);
  bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
  if (!bmp) { DeleteDC(dc); return NULL; }
  ZeroMemory(bits, (size_t)sz * sz * 4);
  oldbmp = SelectObject(dc, bmp);
  tb_paint_icon(dc, 0, 0, sz, icon, fg);
  GdiFlush();
  SelectObject(dc, oldbmp);
  DeleteDC(dc);
  return bmp;
}

/* The color of a menu picture: one per kind of action, a bright shade on the dark theme and a deeper one on the light theme, so
   the pictures read as a set and each kind is told apart at a glance. */
static COLORREF menu_icon_color(unsigned short glyph, int dark)
{
  static const struct { unsigned short g; unsigned char d[3], l[3]; } tab[] =
  {
    /* files and folders: amber */
    { 0xE8E5, {0xF5,0xB8,0x3D}, {0xB9,0x78,0x00} }, { 0xE838, {0xF5,0xB8,0x3D}, {0xB9,0x78,0x00} }, { 0xE8B7, {0xF5,0xB8,0x3D}, {0xB9,0x78,0x00} },
    { 0xE8B9, {0xF5,0xB8,0x3D}, {0xB9,0x78,0x00} },
    /* states: save blue, load teal, history and slots indigo, undo orange */
    { 0xE74E, {0x4D,0xA3,0xFF}, {0x0B,0x62,0xC4} }, { 0xE792, {0x4D,0xA3,0xFF}, {0x0B,0x62,0xC4} },
    { 0xE896, {0x2E,0xC4,0xB6}, {0x0F,0x8F,0x85} },
    { 0xE71D, {0x7C,0x8C,0xFF}, {0x3F,0x4F,0xB8} }, { 0xE81C, {0x7C,0x8C,0xFF}, {0x3F,0x4F,0xB8} },
    { 0xE7A7, {0xFF,0x9F,0x43}, {0xC2,0x5E,0x00} },
    /* close, stop, power, delete: red */
    { 0xE711, {0xFF,0x6B,0x6B}, {0xC6,0x28,0x28} }, { 0xE71A, {0xFF,0x6B,0x6B}, {0xC6,0x28,0x28} }, { 0xE7E8, {0xFF,0x6B,0x6B}, {0xC6,0x28,0x28} },
    { 0xE74D, {0xFF,0x6B,0x6B}, {0xC6,0x28,0x28} }, { 0xE75C, {0xFF,0x6B,0x6B}, {0xC6,0x28,0x28} },
    /* pause, reset, slower: orange; play, add, refresh: green */
    { 0xE769, {0xFF,0xA9,0x4D}, {0xC4,0x6A,0x00} }, { MENU_ICON_TOOLBAR_RESET, {0xFF,0xA9,0x4D}, {0xC4,0x6A,0x00} }, { 0xE738, {0xFF,0xA9,0x4D}, {0xC4,0x6A,0x00} },
    { 0xE768, {0x4C,0xC7,0x64}, {0x1E,0x8A,0x3A} }, { 0xE710, {0x4C,0xC7,0x64}, {0x1E,0x8A,0x3A} }, { MENU_ICON_TOOLBAR_REFRESH, {0x4C,0xC7,0x64}, {0x1E,0x8A,0x3A} },
    { 0xE72C, {0x4C,0xC7,0x64}, {0x1E,0x8A,0x3A} },
    /* speed: purple */
    { 0xEB9D, {0xB3,0x88,0xFF}, {0x6A,0x3F,0xC4} }, { 0xEB9E, {0xB3,0x88,0xFF}, {0x6A,0x3F,0xC4} }, { 0xE916, {0xB3,0x88,0xFF}, {0x6A,0x3F,0xC4} },
    /* picture: sky blue */
    { 0xE740, {0x38,0xBD,0xF8}, {0x03,0x69,0xA1} }, { 0xE73F, {0x38,0xBD,0xF8}, {0x03,0x69,0xA1} }, { 0xE71C, {0x38,0xBD,0xF8}, {0x03,0x69,0xA1} },
    { 0xEA99, {0x38,0xBD,0xF8}, {0x03,0x69,0xA1} }, { 0xE794, {0x38,0xBD,0xF8}, {0x03,0x69,0xA1} }, { 0xE700, {0x38,0xBD,0xF8}, {0x03,0x69,0xA1} },
    { 0xE7F4, {0x38,0xBD,0xF8}, {0x03,0x69,0xA1} }, { 0xE799, {0x38,0xBD,0xF8}, {0x03,0x69,0xA1} }, { 0xE739, {0x38,0xBD,0xF8}, {0x03,0x69,0xA1} },
    { 0xE8CB, {0x38,0xBD,0xF8}, {0x03,0x69,0xA1} }, { 0xE84F, {0x38,0xBD,0xF8}, {0x03,0x69,0xA1} }, { 0xF1BA, {0x38,0xBD,0xF8}, {0x03,0x69,0xA1} },
    { 0xE8A9, {0x38,0xBD,0xF8}, {0x03,0x69,0xA1} }, { 0xE923, {0x38,0xBD,0xF8}, {0x03,0x69,0xA1} }, { 0xE737, {0x38,0xBD,0xF8}, {0x03,0x69,0xA1} },
    { 0xE8FD, {0x38,0xBD,0xF8}, {0x03,0x69,0xA1} },
    /* look of the program: violet */
    { 0xE790, {0xD1,0x8B,0xFF}, {0x8A,0x3F,0xC4} }, { 0xE771, {0xD1,0x8B,0xFF}, {0x8A,0x3F,0xC4} },
    /* sound: pink */
    { 0xEA41, {0xF4,0x72,0xB6}, {0xB0,0x30,0x6E} }, { 0xE74F, {0xF4,0x72,0xB6}, {0xB0,0x30,0x6E} }, { 0xE767, {0xF4,0x72,0xB6}, {0xB0,0x30,0x6E} },
    /* controls: lime */
    { 0xE7FC, {0xA3,0xE6,0x35}, {0x5C,0x8A,0x00} }, { 0xE765, {0xA3,0xE6,0x35}, {0x5C,0x8A,0x00} }, { 0xE80C, {0xA3,0xE6,0x35}, {0x5C,0x8A,0x00} },
    /* cheats and favorites: gold */
    { 0xE8EC, {0xFA,0xCC,0x15}, {0xA1,0x62,0x07} }, { 0xE721, {0xFA,0xCC,0x15}, {0xA1,0x62,0x07} }, { 0xE734, {0xFA,0xCC,0x15}, {0xA1,0x62,0x07} },
    /* network, region, info, help: blue */
    { 0xE968, {0x4D,0xA3,0xFF}, {0x0B,0x62,0xC4} }, { 0xE774, {0x4D,0xA3,0xFF}, {0x0B,0x62,0xC4} }, { 0xE946, {0x4D,0xA3,0xFF}, {0x0B,0x62,0xC4} },
    { 0xE897, {0x4D,0xA3,0xFF}, {0x0B,0x62,0xC4} }, { 0xE71B, {0x4D,0xA3,0xFF}, {0x0B,0x62,0xC4} },
    { 0xE967, {0x7C,0x8C,0xFF}, {0x3F,0x4F,0xB8} },
    /* recording: rose */
    { 0xE722, {0xFF,0x5C,0x7A}, {0xC0,0x26,0x4A} }, { 0xE714, {0xFF,0x5C,0x7A}, {0xC0,0x26,0x4A} }, { 0xE720, {0xFF,0x5C,0x7A}, {0xC0,0x26,0x4A} },
    { 0xE8D6, {0xFF,0x5C,0x7A}, {0xC0,0x26,0x4A} },
    /* settings: teal (not a gray, which would look like a disabled item) */
    { 0xE713, {0x2D,0xD4,0xBF}, {0x0F,0x76,0x6E} },
  };
  int i;

  for (i = 0; i < (int)(sizeof(tab) / sizeof(tab[0])); i++)
    if (tab[i].g == glyph)
      return dark ? RGB(tab[i].d[0], tab[i].d[1], tab[i].d[2]) : RGB(tab[i].l[0], tab[i].l[1], tab[i].l[2]);
  return dark ? RGB(0x7A, 0xA2, 0xF7) : RGB(0x3B, 0x63, 0xC9);          /* the rest: a soft blue */
}

static HBITMAP menu_icon_get(HFONT font, unsigned short glyph, int sz, COLORREF fg)
{
  int i;
  for (i = 0; i < menu_icon_n; i++) if (menu_icon_glyph[i] == glyph) return menu_icon_bmp[i];
  if (!font) return NULL;          /* only what is made already */
  if (menu_icon_n >= (int)(sizeof(menu_icon_bmp) / sizeof(menu_icon_bmp[0]))) return NULL;
  if (gui.color_icons) fg = menu_icon_color(glyph, theme_is_dark());          /* else the color of the text, as passed */
  menu_icon_bmp[menu_icon_n] = glyph == MENU_ICON_TOOLBAR_RESET ? menu_icon_make_toolbar(TI_RESET, sz, fg) :
                                             glyph == MENU_ICON_TOOLBAR_REFRESH ? menu_icon_make_toolbar(TI_REFRESH, sz, fg) : menu_icon_make(font, glyph, sz, fg);
  menu_icon_glyph[menu_icon_n] = glyph;
  return menu_icon_bmp[menu_icon_n++];
}

/* The picture of a choice that is switched on: the same picture on a box in the accent color (the check mark of the menu
   itself does not show beside a picture). Made from the plain one and kept with it. */
static HBITMAP menu_icon_checked(HBITMAP plain, int slot)
{
  DIBSECTION ds;
  BITMAPINFO bi;
  unsigned char *src, *dst;
  HBITMAP bmp;
  HDC dc;
  COLORREF c;
  int sz, x, y, r;

  if (slot < 0 || !plain) return plain;
  if (menu_icon_bmp_on[slot]) return menu_icon_bmp_on[slot];
  if (!GetObject(plain, sizeof(ds), &ds) || !ds.dsBm.bmBits || ds.dsBm.bmBitsPixel != 32) return plain;
  sz = ds.dsBm.bmWidth;
  if (ds.dsBm.bmHeight != sz) return plain;
  ZeroMemory(&bi, sizeof(bi));
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = sz;
  bi.bmiHeader.biHeight = -sz;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  dc = CreateCompatibleDC(NULL);
  bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&dst, NULL, 0);
  DeleteDC(dc);
  if (!bmp) return plain;
  src = (unsigned char *)ds.dsBm.bmBits;
  c = theme_accent_bg();
  r = sz * 3 / 16;
  for (y = 0; y < sz; y++)
    for (x = 0; x < sz; x++)
    {
      unsigned char *s = src + ((size_t)y * sz + x) * 4, *d = dst + ((size_t)y * sz + x) * 4;
      int dx = x < r ? r - x - 1 : (x >= sz - r ? x - (sz - r) : -1), dy = y < r ? r - y - 1 : (y >= sz - r ? y - (sz - r) : -1);
      int inside = 1, edge = (x == 0 || y == 0 || x == sz - 1 || y == sz - 1), ba, k;

      if (dx >= 0 && dy >= 0)                                  /* a corner: rounded */
      {
        int d2 = dx * dx + dy * dy;
        if (d2 > r * r) inside = 0;
        else if (d2 > (r - 1) * (r - 1)) edge = 1;
      }
      ba = !inside ? 0 : (edge ? 170 : 80);                    /* the box: a clear fill and a stronger edge */
      k = 255 - s[3];
      d[0] = (unsigned char)(s[0] + GetBValue(c) * ba / 255 * k / 255);
      d[1] = (unsigned char)(s[1] + GetGValue(c) * ba / 255 * k / 255);
      d[2] = (unsigned char)(s[2] + GetRValue(c) * ba / 255 * k / 255);
      d[3] = (unsigned char)(s[3] + ba * k / 255);
    }
  menu_icon_bmp_on[slot] = bmp;
  return bmp;
}

static int menu_icon_slot(unsigned short glyph)
{
  int i;
  for (i = 0; i < menu_icon_n; i++) if (menu_icon_glyph[i] == glyph) return i;
  return -1;
}

static int walk_no_icons;                  /* inside Render Filter, a list of state slots or Recent Files: its sub-menus and their items have no pictures */

static void menu_icons_walk(HMENU menu, HFONT font, int sz, COLORREF fg)
{
  int n = GetMenuItemCount(menu), i, k;

  for (i = 0; i < n; i++)
  {
    HMENU sub = GetSubMenu(menu, i);
    unsigned short glyph = 0;
    MENUITEMINFOA mii;
    ULONG_PTR own = 0;                                        /* The Large interface: the item's data, the key of its picture */

    ZeroMemory(&mii, sizeof(mii));
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_FTYPE | MIIM_DATA;
    GetMenuItemInfoA(menu, (UINT)i, TRUE, &mii);
    if ((mii.fType & MFT_OWNERDRAW) && mii.dwItemData) own = mii.dwItemData;

    if (sub)
    {
      char title[96], *a, *o;
      ZeroMemory(&mii, sizeof(mii));
      mii.cbSize = sizeof(mii);
      mii.fMask = MIIM_FTYPE | MIIM_DATA;
      title[0] = 0;
      GetMenuItemInfoA(menu, (UINT)i, TRUE, &mii);
      if ((mii.fType & MFT_OWNERDRAW) && mii.dwItemData)             /* The Large interface: the text was kept as the item's data */
        lstrcpynA(title, (const char *)mii.dwItemData, sizeof(title));
      else
      {
        ZeroMemory(&mii, sizeof(mii));
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_STRING;
        mii.dwTypeData = title;
        mii.cch = sizeof(title) - 1;
        GetMenuItemInfoA(menu, (UINT)i, TRUE, &mii);
      }
      for (a = o = title; *a; a++) if (*a != '&') *o++ = *a;        /* "&Load State" -> "Load State" */
      *o = 0;
      if (GetMenuItemID(menu, 0) != (UINT)IDM_VIDEO_FILTER_NONE)      /* the sub-menus of Render Filter (CRT, Scanlines ...) have no picture */
        for (k = 0; k < (int)(sizeof(menu_icon_titles) / sizeof(menu_icon_titles[0])); k++)
          if (!lstrcmpiA(title, menu_icon_titles[k].title)) { glyph = menu_icon_titles[k].glyph; break; }
      {
        int nf = !lstrcmpiA(title, "Render Filter") || (n > 0 && GetMenuItemID(menu, 0) == (UINT)IDM_VIDEO_FILTER_NONE) ||
                 !lstrcmpiA(title, "Select State Slot") || !lstrcmpiA(title, "Load State from Slot") || !lstrcmpiA(title, "Save State to Slot") || !lstrcmpiA(title, "Load Backup State") || !lstrcmpiA(title, "Recent Files");
        walk_no_icons += nf;
        menu_icons_walk(sub, font, sz, fg);
        walk_no_icons -= nf;
      }
    }
    else
    {
      int id = (int)GetMenuItemID(menu, i);
      for (k = 0; k < (int)(sizeof(menu_icon_ids) / sizeof(menu_icon_ids[0])); k++)
        if (menu_icon_ids[k].id == id) { glyph = menu_icon_ids[k].glyph; break; }
    }

    if (walk_no_icons)
    {
      glyph = 0;
      if (own) own_icon_set(own, NULL);
      else
      {
        MENUITEMINFOA bi;
        ZeroMemory(&bi, sizeof(bi));
        bi.cbSize = sizeof(bi);
        bi.fMask = MIIM_BITMAP;
        if (GetMenuItemInfoA(menu, (UINT)i, TRUE, &bi) && bi.hbmpItem)
        {
          bi.hbmpItem = NULL;
          SetMenuItemInfoA(menu, (UINT)i, TRUE, &bi);
        }
      }
    }
    if (glyph)
    {
      HBITMAP b = menu_icon_get(font, glyph, sz, fg);
      if (b && !sub && (GetMenuState(menu, (UINT)i, MF_BYPOSITION) & MF_CHECKED)) b = menu_icon_checked(b, menu_icon_slot(glyph));
      if (b && own)
      {
        if (menu != GetMenu(g_hwnd)) own_icon_set(own, b);
      }
      else if (b)
      {
        HBITMAP cur;
        ZeroMemory(&mii, sizeof(mii));
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_BITMAP;
        GetMenuItemInfoA(menu, (UINT)i, TRUE, &mii);
        cur = mii.hbmpItem;
        if (cur != b)                                          /* only when it is another picture */
        {
          mii.hbmpItem = b;
          SetMenuItemInfoA(menu, (UINT)i, TRUE, &mii);
        }
      }
    }
  }
}

/* Gives the menus their icons, or renews them after the theme changed (the pictures change to the shade that suits it). */
/* The frame of an open menu (the window class #32768) is drawn by Windows in white; in the dark theme with the Large interface it is painted
   over in the menu color. */
/* Paints the border of a menu window (the whole window's device context) in the menu color. */
static void menu_frame_fill(HWND hwnd, HDC dc)
{
  RECT wr, cr, e[4];
  HBRUSH br = CreateSolidBrush(THEME_DARK_MENU);
  int n, k;

  GetWindowRect(hwnd, &wr);
  OffsetRect(&wr, -wr.left, -wr.top);
  GetClientRect(hwnd, &cr);
  n = (wr.right - cr.right) / 2;
  if (n < 1) n = 1;
  e[0] = wr; e[0].bottom = n;
  e[1] = wr; e[1].top = wr.bottom - n;
  e[2] = wr; e[2].right = n;
  e[3] = wr; e[3].left = wr.right - n;
  for (k = 0; k < 4; k++) FillRect(dc, &e[k], br);
  DeleteObject(br);
}

/* A menu window whose items the program draws (a system-drawn one, such as the window menu or an edit box's, keeps its own look). */
static int popup_is_ownerdraw(HWND hwnd)
{
  HMENU m = (HMENU)SendMessage(hwnd, 0x01E1 /* MN_GETHMENU */, 0, 0);
  MENUITEMINFOA mii;

  if (!m || GetMenuItemCount(m) <= 0) return 0;
  memset(&mii, 0, sizeof(mii));
  mii.cbSize = sizeof(mii);
  mii.fMask = MIIM_FTYPE;
  return GetMenuItemInfoA(m, 0, TRUE, &mii) && (mii.fType & MFT_OWNERDRAW) != 0;
}

static LRESULT CALLBACK menu_frame_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
  (void)ref;
  if (msg == WM_NCDESTROY)
    RemoveWindowSubclass(hwnd, menu_frame_proc, id);
  else if (msg == WM_ERASEBKGND && theme_menus_owner() && !theme_is_dark() && popup_is_ownerdraw(hwnd))
  {
    RECT cr;                                          /* whatever the items do not cover is the same color as they are */
    HBRUSH bb = CreateSolidBrush(menu_light_bg(0));
    GetClientRect(hwnd, &cr);
    FillRect((HDC)wp, &cr, bb);
    DeleteObject(bb);
    return 1;
  }
  else if (msg == WM_WINDOWPOSCHANGED)
  {
    LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
    int corner = 2;                                   /* the system's own rounding again after every size change (a window region would round with jagged edges and shadow) */
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    return r;
  }
  else if (msg == WM_SHOWWINDOW && wp)
  {
    int corner = 2;                                   /* (again once it is shown: the system may set its own when the window appears) */
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
  }
  else if (msg == WM_NCPAINT && theme_menus_owner() && theme_is_dark())
  {
    HDC dc = GetWindowDC(hwnd);
    DefSubclassProc(hwnd, msg, wp, lp);
    menu_frame_fill(hwnd, dc);
    ReleaseDC(hwnd, dc);
    return 0;
  }
  else if (msg == WM_PRINT && (lp & PRF_NONCLIENT) && theme_menus_owner() && theme_is_dark())     /* the opening animation draws the menu this way */
  {
    LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
    menu_frame_fill(hwnd, (HDC)wp);
    return r;
  }
  return DefSubclassProc(hwnd, msg, wp, lp);
}

void theme_menu_frame(HWND popup)
{
  int corner = 2;                                     /* DWMWCP_ROUND: the menus have rounded corners, as the system's own do (Windows 11) */

  SetWindowSubclass(popup, menu_frame_proc, 1, 0);
  DwmSetWindowAttribute(popup, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
}

/* Every menu window (the class #32768) gets the frame painter as it is created, so the context menus have it too. */
static HHOOK menu_cbt_hook;

static LRESULT CALLBACK menu_cbt_proc(int code, WPARAM wp, LPARAM lp)
{
  if (code == HCBT_CREATEWND)
  {
    char cls[16];
    if (GetClassNameA((HWND)wp, cls, sizeof(cls)) && !lstrcmpA(cls, "#32768")) theme_menu_frame((HWND)wp);
  }
  return CallNextHookEx(menu_cbt_hook, code, wp, lp);
}

static void menu_frame_hook_install(void)
{
  if (!menu_cbt_hook) menu_cbt_hook = SetWindowsHookExA(WH_CBT, menu_cbt_proc, NULL, GetCurrentThreadId());
}

void theme_menu_icons(HMENU menu)
{
  HBITMAP old_bmp[96], old_on[96];
  int old_n = menu_icon_n, i, sz, dpi;
  HDC sdc;
  HFONT font;

  if (!menu) return;
  sdc = GetDC(NULL);
  dpi = sdc ? GetDeviceCaps(sdc, LOGPIXELSX) : 96;
  if (sdc) ReleaseDC(NULL, sdc);
  sz = MulDiv(16, dpi, 96);
  if (sz < 16) sz = 16;

  font = menu_icon_font(sz - 3);
  if (!font) return;

  for (i = 0; i < old_n; i++) { old_bmp[i] = menu_icon_bmp[i]; old_on[i] = menu_icon_bmp_on[i]; menu_icon_bmp_on[i] = NULL; }
  menu_icon_n = 0;                                          /* new pictures, in the color of the theme that is on now */
  menu_icons_walk(menu, font, sz, theme_is_dark() ? RGB(0xF0, 0xF0, 0xF0) : RGB(0x30, 0x30, 0x30));
  for (i = 0; i < old_n; i++) { if (old_bmp[i]) DeleteObject(old_bmp[i]); if (old_on[i]) DeleteObject(old_on[i]); }    /* the menu items no longer use these */
  DeleteObject(font);
}

/* The menu choices that are switched on show their picture on a box (see menu_icon_checked), the others the plain one: renewed
   whenever the check marks were set. Uses only the pictures that exist already. */
void theme_menu_icon_marks(HMENU menu)
{
  if (!menu || menu_icon_n == 0) return;
  menu_icons_walk(menu, NULL, 16, 0);
}

/* A picture for one item of a menu built on the spot (the right-click menu of a game): the glyph in the color of the theme.
   The bitmaps are kept in the same list as the menu bar's and live as long as the program (or until the theme changes). */
void theme_menu_icon(HMENU menu, UINT id, unsigned short glyph)
{
  HDC sdc;
  int dpi, sz;
  HFONT font;
  HBITMAP b;
  MENUITEMINFOA mii;

  if (!menu) return;
  sdc = GetDC(NULL);
  dpi = sdc ? GetDeviceCaps(sdc, LOGPIXELSX) : 96;
  if (sdc) ReleaseDC(NULL, sdc);
  sz = MulDiv(16, dpi, 96);
  if (sz < 16) sz = 16;
  font = menu_icon_font(sz - 3);
  if (!font) return;
  b = menu_icon_get(font, glyph, sz, theme_is_dark() ? RGB(0xF0, 0xF0, 0xF0) : RGB(0x30, 0x30, 0x30));
  DeleteObject(font);
  if (!b) return;
  ZeroMemory(&mii, sizeof(mii));
  mii.cbSize = sizeof(mii);
  mii.fMask = MIIM_FTYPE | MIIM_DATA;
  GetMenuItemInfoA(menu, id, FALSE, &mii);
  if ((mii.fType & MFT_OWNERDRAW) && mii.dwItemData)       /* The Large interface: the item paints the picture itself */
  {
    own_icon_set(mii.dwItemData, b);
    return;
  }
  ZeroMemory(&mii, sizeof(mii));
  mii.cbSize = sizeof(mii);
  mii.fMask = MIIM_BITMAP;
  mii.hbmpItem = b;
  SetMenuItemInfoA(menu, id, FALSE, &mii);
}

/* The accent color (Settings > General): the selected row of the program's own lists and the button that waits for a key.
   0 is the default (a blue in the dark theme, the color Windows uses in the light one); the others are fixed colors that
   read with white text in both themes. */
static const COLORREF accent_bg[GUI_ACCENT_HIDDEN + 1]   = { 0, RGB(0x7A, 0x52, 0x33), RGB(0xA8, 0x1F, 0x3A), RGB(0xA6, 0x7C, 0x00),
                                                          RGB(0x55, 0x55, 0x55), RGB(0x3A, 0x76, 0x3E), RGB(0x3F, 0x4F, 0xA8), RGB(0x6B, 0x8E, 0x23),
                                                          RGB(0xA0, 0x2F, 0x8F), RGB(0x1F, 0x3F, 0x8F), RGB(0x6B, 0x6B, 0x1F), RGB(0x94, 0x62, 0x30),
                                                          RGB(0x92, 0x45, 0x78), RGB(0x62, 0x4A, 0x92), RGB(0x90, 0x40, 0x40), RGB(0x1F, 0x7F, 0xB5),
                                                          RGB(0x2A, 0x7A, 0x7A), RGB(0x7B, 0x2F, 0xB8), RGB(0xB8, 0x98, 0x00), RGB(0x0E, 0x8F, 0x3F) };
static const COLORREF accent_edge[GUI_ACCENT_HIDDEN + 1] = { 0, RGB(0xB0, 0x84, 0x60), RGB(0xE0, 0x60, 0x78), RGB(0xE0, 0xB8, 0x3A),
                                                          RGB(0x80, 0x80, 0x80), RGB(0x68, 0xA8, 0x6C), RGB(0x7C, 0x8A, 0xD8), RGB(0xA4, 0xCC, 0x5A),
                                                          RGB(0xD8, 0x70, 0xC8), RGB(0x60, 0x80, 0xD8), RGB(0xB8, 0xB8, 0x58), RGB(0xC4, 0x90, 0x58),
                                                          RGB(0xC0, 0x72, 0xA5), RGB(0x90, 0x78, 0xC0), RGB(0xC0, 0x70, 0x70), RGB(0x5C, 0xB8, 0xE8),
                                                          RGB(0x55, 0xA8, 0xA8), RGB(0xB0, 0x80, 0xE8), RGB(0xF0, 0xD8, 0x40), RGB(0x58, 0xE8, 0x98) };

/* A push button that opens a list: its text and a down arrow after it, drawn here so the arrow is whole and big enough in any font
   (a glyph in the text came out cut off). Owner-drawn: the colors of the two themes are used. */
void theme_draw_dropdown_button(const DRAWITEMSTRUCT *di)
{
  int dark = theme_is_dark(), down = (di->itemState & ODS_SELECTED) != 0, dis = (di->itemState & ODS_DISABLED) != 0;
  POINT cur;
  int hot = 0;
  COLORREF face, edge, fg;
  RECT r = di->rcItem, tr;
  HBRUSH fb, eb;
  char text[80];
  SIZE sz;
  HGDIOBJ of;
  int aw = TBS(10), ah = TBS(6), gap = TBS(8), total, x0, cy;
  POINT tri[3];
  HBRUSH ab;
  HGDIOBJ ob, op;

  if (GetCursorPos(&cur)) { RECT wr; if (GetWindowRect(di->hwndItem, &wr) && PtInRect(&wr, cur)) hot = 1; }
  face = dark ? (down ? RGB(0x55, 0x55, 0x55) : (hot ? RGB(0x4A, 0x4A, 0x4A) : RGB(0x38, 0x38, 0x38)))
              : (down ? RGB(0xCC, 0xE4, 0xF7) : (hot ? RGB(0xE5, 0xF1, 0xFB) : RGB(0xE1, 0xE1, 0xE1)));
  edge = dark ? RGB(0x6B, 0x6B, 0x6B) : (hot ? RGB(0x00, 0x78, 0xD7) : RGB(0xAD, 0xAD, 0xAD));
  fg = dis ? (dark ? RGB(0x80, 0x80, 0x80) : RGB(0x80, 0x80, 0x80)) : (dark ? RGB(0xF0, 0xF0, 0xF0) : RGB(0, 0, 0));

  if (gui.ui_style)
  {
    /* Modern: the same flat rounded look as the other buttons. */
    COLORREF mf, mt, me, bg;
    mod_ss ss;
    int w = r.right - r.left, hh = r.bottom - r.top;
    mod_push_colors(dark, hot, down, dis, 0, &mf, &mt, &me);
    fg = mt;
    mod_fill_parent(di->hwndItem, di->hDC, &r);
    bg = GetPixel(di->hDC, r.left, r.top);
    if (bg == CLR_INVALID) bg = dark ? THEME_DARK_BG : GetSysColor(COLOR_BTNFACE);
    if (mod_ss_begin(&ss, di->hDC, w, hh, bg))
    {
      mod_ss_round(&ss, 0.5, 0.5, w - 1, hh - 1, (double)mod_dpi_px(di->hDC, 5), mf, (di->itemState & ODS_FOCUS) ? mod_accent() : me, (di->itemState & ODS_FOCUS) ? 2.0 : 1.0);
      mod_ss_end(&ss, di->hDC, r.left, r.top);
    }
  }
  else
  {
    /* The button itself is drawn by the visual style, so it looks like the other buttons (rounded in Windows 11, flat in 10). */
    HTHEME th = OpenThemeData(di->hwndItem, L"BUTTON");
    if (th)
    {
      DrawThemeBackground(th, di->hDC, 1 /* BP_PUSHBUTTON */, dis ? 4 : (down ? 3 : (hot ? 2 : 1)), &r, NULL);
      CloseThemeData(th);
    }
    else
    {
      fb = CreateSolidBrush(face);
      eb = CreateSolidBrush(edge);
      FillRect(di->hDC, &r, fb);
      FrameRect(di->hDC, &r, eb);
      DeleteObject(fb);
      DeleteObject(eb);
    }
  }

  GetWindowTextA(di->hwndItem, text, sizeof(text));
  of = SelectObject(di->hDC, (HFONT)SendMessage(di->hwndItem, WM_GETFONT, 0, 0));
  GetTextExtentPoint32A(di->hDC, text, lstrlenA(text), &sz);
  total = sz.cx + gap + aw;
  x0 = r.left + (r.right - r.left - total) / 2;
  cy = (r.top + r.bottom) / 2;

  SetBkMode(di->hDC, TRANSPARENT);
  SetTextColor(di->hDC, fg);
  tr.left = x0; tr.right = x0 + sz.cx; tr.top = r.top; tr.bottom = r.bottom;
  DrawTextA(di->hDC, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  SelectObject(di->hDC, of);

  tri[0].x = x0 + sz.cx + gap;          tri[0].y = cy - ah / 2;
  tri[1].x = x0 + sz.cx + gap + aw;     tri[1].y = cy - ah / 2;
  tri[2].x = x0 + sz.cx + gap + aw / 2; tri[2].y = cy - ah / 2 + ah;
  ab = CreateSolidBrush(fg);
  ob = SelectObject(di->hDC, ab);
  op = SelectObject(di->hDC, GetStockObject(NULL_PEN));
  Polygon(di->hDC, tri, 3);
  SelectObject(di->hDC, ob);
  SelectObject(di->hDC, op);
  DeleteObject(ab);

  if ((di->itemState & ODS_FOCUS) && !gui.ui_style)
  {
    RECT fr = r;
    InflateRect(&fr, -TBS(3), -TBS(3));
    DrawFocusRect(di->hDC, &fr);
  }
}

COLORREF theme_accent_bg(void)
{
  int a = gui.accent < 0 || gui.accent > GUI_ACCENT_HIDDEN ? 0 : gui.accent;
  if (a) return accent_bg[a];
  if (gui.ui_style) return modern_blue();          /* every chosen row of every list: the same color as the Modern style's other highlights */
  return theme_is_dark() ? RGB(0x40, 0x60, 0x90) : GetSysColor(COLOR_HIGHLIGHT);
}

/* The color shown beside a name in the Accent Color list. */
COLORREF theme_accent_swatch(int i)
{
  if (i <= 0 || i > GUI_ACCENT_HIDDEN) return gui.ui_style ? modern_blue() : (theme_is_dark() ? RGB(0x40, 0x60, 0x90) : GetSysColor(COLOR_HIGHLIGHT));
  return accent_bg[i];
}

COLORREF theme_accent_edge(void)
{
  int a = gui.accent < 0 || gui.accent > GUI_ACCENT_HIDDEN ? 0 : gui.accent;
  if (a) return accent_edge[a];
  if (gui.ui_style) return modern_blue();
  return theme_is_dark() ? RGB(0x70, 0x90, 0xC0) : GetSysColor(COLOR_HIGHLIGHT);
}
