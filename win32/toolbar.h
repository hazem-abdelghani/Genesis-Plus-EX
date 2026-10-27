/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  toolbar.h -- the flat, icon-only buttons of the ROM browser's toolbar.
 *  The icons are drawn in code (no picture files), in the theme's text
 *  color, so they follow the light and dark themes.
 ****************************************************************************/

#ifndef _TOOLBAR_H_
#define _TOOLBAR_H_

enum { TI_OPEN, TI_REFRESH, TI_PLAY, TI_STOP, TI_RESET, TI_PAUSE, TI_FULLSCREEN,
       TI_SCREENSHOT, TI_SETTINGS, TI_VIDEO, TI_AUDIO, TI_CONTROLS, TI_FAV, TI_FAVON, TI_SEARCH, TI_N };

/* The display scale of the window the program is on (96 = 100%), kept up to date when the window moves to another monitor. */
extern int gui_dpi;
#define DPIS(v) MulDiv((v), gui_dpi, 96)

/* Sizes follow the Interface Size setting and the display scale. */
#define TBS(v) DPIS(gui.large_ui ? (((v) * 1175 + 500) / 1000) : (v))

/* A flat button showing `icon`; `tip` is its tooltip (must stay valid). */
HWND tb_button(HWND parent, int id, int icon, const char *tip);

/* The names under the icons (View > Toolbars > Show Toolbar Labels) and the icon size (Toolbar Icon Size) set how big the
   buttons and the bar are: the toolbar is laid out from these. */
void tb_set_label(HWND button, const char *label);     /* label must stay valid */
int  tb_button_h(void);
int  tb_button_w(HWND button);
int  tb_bar_h_rows(int rows);      /* the bar's height for that many rows of buttons */

/* A thin vertical divider between groups of buttons. */
HWND tb_separator(HWND parent);

/* The pressed look of a switch (e.g. the current view). */
void tb_set_checked(HWND button, int on);
void tb_set_tip(HWND button, const char *tip);

/* From WM_DRAWITEM: 1 when the item was one of the toolbar's. */
int tb_draw(const DRAWITEMSTRUCT *di);

/* Paints one icon (alpha-blended, in `color`) with its top-left at x, y, sz pixels square -- for
   the star in the game list. */
void tb_paint_icon(HDC dc, int x, int y, int sz, int icon, COLORREF color);

/* The color of an icon when Colorful Icons is on (a bright shade on the dark theme, a deeper one on the light). */
COLORREF tb_icon_color(int icon);

/* The face color the toolbar, search bar and filter bar sit on. */
COLORREF tb_face_color(void);

#endif
