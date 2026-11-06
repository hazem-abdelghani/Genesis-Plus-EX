/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  i18n.h -- translations. The program's own text is English; a language is a
 *  UTF-8 file in the Languages folder (name.po, the gettext format that Poedit
 *  and other editors open): each "msgid" is the English text exactly as the
 *  program has it and "msgstr" is the text to show. tr() finds it; nothing that
 *  is not in the file is changed. Most of the program's text goes through it
 *  without being marked: this header gives the Windows calls that take text
 *  (menus, windows, messages, tooltips, painted labels) a version that looks
 *  the text up first, and a hook does the same for every dialog.
 ****************************************************************************/

#ifndef GPEX_I18N_H
#define GPEX_I18N_H

#include <windows.h>

extern int i18n_on;                                /* a language other than English is loaded */

const char *tr(const char *s);                     /* the translation of s, or s */
const char *i18n_menu_title(HMENU sub);            /* the English title a translated submenu had (NULL: none was kept) */
const char *i18n_untr(const char *s);              /* the English text a translation was made from (or s) */
char *i18n_block(const char *text);                /* a long text (many lines) translated line by line: malloc'd, free() it */
void i18n_init(const char *language);              /* loads Languages\<language>.po ("" or "English": none) and starts the dialog hook */
int  i18n_list(char names[][48], int max);         /* the languages found in the Languages folder */
void i18n_language_name(const char *file, char *out, int len);   /* the name a language gives itself (for example Francais), for the menu: never translated */
int  i18n_text_ratio(HWND w);                      /* percent: how much wider the text of its controls would need the window's layout to be (100: fits) */
void i18n_menu(HMENU menu);                        /* translates a menu and its submenus */
void i18n_language_menu(HMENU menu, const char *current);   /* fills the Options > Language submenu (the one holding IDM_OPT_LANGUAGE_BASE) */
int  i18n_language_from_command(int cmd, char *name, int len);   /* the language a Language menu command picks (0: not one) */

#ifndef IDM_OPT_LANGUAGE_BASE
#define IDM_OPT_LANGUAGE_BASE 4000                 /* .. 4031: English, then the files found */
#endif

/* Calls that take text, with a version that translates it first (not in i18n.c itself). */
#ifndef GPEX_I18N_IMPL
int WINAPIV i18n_wsprintfA(LPSTR out, LPCSTR fmt, ...);
HWND WINAPI i18n_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param);
BOOL WINAPI i18n_SetWindowTextA(HWND wnd, LPCSTR text);
BOOL WINAPI i18n_SetMenuItemInfoA(HMENU menu, UINT item, BOOL by_pos, LPCMENUITEMINFOA mi);
BOOL WINAPI i18n_GetMenuItemInfoA(HMENU menu, UINT item, BOOL by_pos, LPMENUITEMINFOA mi);
int WINAPI i18n_GetMenuStringA(HMENU menu, UINT item, LPSTR out, int cap, UINT flags);
BOOL WINAPI i18n_SetDlgItemTextA(HWND dlg, int id, LPCSTR text);
LRESULT WINAPI i18n_SendDlgItemMessageA(HWND dlg, int id, UINT msg, WPARAM wp, LPARAM lp);
BOOL WINAPI i18n_AppendMenuA(HMENU menu, UINT flags, UINT_PTR id, LPCSTR item);
BOOL WINAPI i18n_InsertMenuA(HMENU menu, UINT pos, UINT flags, UINT_PTR id, LPCSTR item);
BOOL WINAPI i18n_ModifyMenuA(HMENU menu, UINT pos, UINT flags, UINT_PTR id, LPCSTR item);
LRESULT WINAPI i18n_SendMessageA(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
BOOL WINAPI i18n_TextOutA(HDC dc, int x, int y, LPCSTR s, int n);
int  WINAPI i18n_DrawTextA(HDC dc, LPCSTR s, int n, LPRECT rc, UINT fmt);
BOOL WINAPI i18n_GetTextExtentPoint32A(HDC dc, LPCSTR s, int n, LPSIZE size);
#define wsprintfA        i18n_wsprintfA
#define CreateWindowExA  i18n_CreateWindowExA
#define SetWindowTextA   i18n_SetWindowTextA
#define SetMenuItemInfoA i18n_SetMenuItemInfoA
#define GetMenuItemInfoA i18n_GetMenuItemInfoA
#define GetMenuStringA   i18n_GetMenuStringA
#define SetDlgItemTextA  i18n_SetDlgItemTextA
#define SendDlgItemMessageA i18n_SendDlgItemMessageA
#define AppendMenuA      i18n_AppendMenuA
#define InsertMenuA      i18n_InsertMenuA
#define ModifyMenuA      i18n_ModifyMenuA
#define SendMessageA     i18n_SendMessageA
#define TextOutA         i18n_TextOutA
#define DrawTextA        i18n_DrawTextA
#define GetTextExtentPoint32A i18n_GetTextExtentPoint32A
#endif

#endif
