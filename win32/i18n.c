/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  i18n.c -- translations from Languages\*.po (see i18n.h).
 ****************************************************************************/

#define GPEX_I18N_IMPL
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "i18n.h"

extern char *osd_path(const char *filename);
BOOL WINAPI i18n_SetMenuItemInfoA(HMENU menu, UINT item, BOOL by_pos, LPCMENUITEMINFOA mi);
BOOL WINAPI i18n_GetMenuItemInfoA(HMENU menu, UINT item, BOOL by_pos, LPMENUITEMINFOA mi);
int WINAPI i18n_GetMenuStringA(HMENU menu, UINT item, LPSTR out, int cap, UINT flags);

int i18n_on;

typedef struct { char *key, *val; } entry_t;

static entry_t *tab;                    /* open-addressing hash table, a power of two long */
static size_t tab_size, tab_count;
static HHOOK dlg_hook;

static unsigned hash_str(const char *s)
{
  unsigned h = 2166136261u;

  while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
  return h;
}

static const char *find(const char *s)
{
  size_t i;

  if (!tab_size) return NULL;
  for (i = hash_str(s) & (tab_size - 1); tab[i].key; i = (i + 1) & (tab_size - 1))
    if (!strcmp(tab[i].key, s)) return tab[i].val;
  return NULL;
}

static void put(char *key, char *val);

static void grow(void)
{
  entry_t *old = tab;
  size_t old_size = tab_size, i;

  tab_size = tab_size ? tab_size * 2 : 1024;
  tab = (entry_t *)calloc(tab_size, sizeof(entry_t));
  tab_count = 0;
  for (i = 0; i < old_size; i++) if (old[i].key) put(old[i].key, old[i].val);
  free(old);
}

/* Takes over the two strings (a later entry for the same text replaces an earlier one). */
static void put(char *key, char *val)
{
  size_t i;

  if (!tab_size || (tab_count + 1) * 2 > tab_size) grow();
  for (i = hash_str(key) & (tab_size - 1); tab[i].key; i = (i + 1) & (tab_size - 1))
    if (!strcmp(tab[i].key, key)) { free(tab[i].val); free(key); tab[i].val = val; return; }
  tab[i].key = key;
  tab[i].val = val;
  tab_count++;
}

/* The other way: the English text a translation came from (the program looks things up by their English names, for instance the
   picture of a menu), or the text itself when it is not a translation. Searched in the table; it is short enough and asked rarely. */
const char *i18n_untr(const char *s)
{
  size_t i;

  if (!i18n_on || !s || !s[0]) return s;
  for (i = 0; i < tab_size; i++)
    if (tab[i].key && !strcmp(tab[i].val, s)) return tab[i].key;
  return s;
}

const char *tr(const char *s)
{
  const char *t;

  if (!i18n_on || !s || !s[0]) return s;
  t = find(s);
  return t ? t : s;
}

/* ---- the .po file ------------------------------------------------------ */

typedef struct { char *p; size_t n, cap; } sb_t;

static void sb_add(sb_t *b, const char *s, size_t n)
{
  if (b->n + n + 1 > b->cap)
  {
    b->cap = (b->n + n + 1) * 2 + 64;
    char *np = (char *)realloc(b->p, b->cap);
    if (!np) return;                              /* (out of memory: the text is left as it was) */
    b->p = np;
  }
  memcpy(b->p + b->n, s, n);
  b->n += n;
  b->p[b->n] = '\0';
}

/* The text of a quoted string in a .po line (starting at the quote), with its escapes. */
static void unquote(const char *s, sb_t *out)
{
  if (*s != '"') return;
  for (s++; *s && *s != '"'; s++)
  {
    char c = *s;

    if (c == '\\' && s[1])
    {
      s++;
      switch (*s)
      {
        case 'n': c = '\n'; break;
        case 'r': c = '\r'; break;
        case 't': c = '\t'; break;
        default: c = *s; break;
      }
    }
    sb_add(out, &c, 1);
  }
}

static void load_po(char *data)
{
  sb_t id = { 0 }, str = { 0 };
  int field = 0, fuzzy = 0;
  char *line, *next;

  if ((unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF) data += 3;

#define FLUSH() do { \
    if (id.n && str.n && !fuzzy) put(_strdup(id.p), _strdup(str.p)); \
    id.n = str.n = 0; if (id.p) id.p[0] = 0; if (str.p) str.p[0] = 0; field = 0; fuzzy = 0; } while (0)

  for (line = data; line && *line; line = next)
  {
    char *e;

    next = strchr(line, '\n');
    if (next) *next++ = '\0';
    e = line + strlen(line);
    while (e > line && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = '\0';
    while (*line == ' ' || *line == '\t') line++;

    if (line[0] == '#')                                            /* a comment starts the next entry: the one before is complete */
    {
      if (field == 2) FLUSH();
      if (line[1] == ',' && strstr(line, "fuzzy")) fuzzy = 1;
      continue;
    }
    if (!strncmp(line, "msgid ", 6) || !strncmp(line, "msgid\t", 6))
    {
      if (field == 2) FLUSH();
      field = 1;
      id.n = 0;
      unquote(line + 6 + strspn(line + 6, " \t"), &id);
    }
    else if (!strncmp(line, "msgstr ", 7) || !strncmp(line, "msgstr\t", 7))
    {
      field = 2;
      str.n = 0;
      unquote(line + 7 + strspn(line + 7, " \t"), &str);
    }
    else if (line[0] == '"') unquote(line, field == 1 ? &id : field == 2 ? &str : &id);
  }
  if (field == 2) FLUSH();
#undef FLUSH
  free(id.p);
  free(str.p);
}

static void clear_table(void)
{
  size_t i;

  for (i = 0; i < tab_size; i++) if (tab && tab[i].key) { free(tab[i].key); free(tab[i].val); }
  free(tab);
  tab = NULL;
  tab_size = tab_count = 0;
}

/* ---- text the program shows --------------------------------------------- */

/* Translates a menu item's text: the part before a tab (the shortcut stays as it is). */
static void menu_text(const char *in, char *out, size_t len)
{
  const char *tab_at = strchr(in, '\t');
  char head[256];
  const char *t;

  lstrcpynA(head, in, tab_at && tab_at - in < (int)sizeof(head) ? (int)(tab_at - in) + 1 : (int)sizeof(head));      /* (never more than head holds) */
  t = tr(head);
  if (tab_at) _snprintf(out, len, "%s%s", t, tab_at);
  else lstrcpynA(out, t, (int)len);
  out[len - 1] = '\0';
}

/* The English title of each submenu, kept when the menu is translated: the program finds the pictures of the menus by those names. */
static struct { HMENU sub; char title[96]; } sub_titles[512];
static int sub_titles_n;

static void remember_title(HMENU sub, const char *english)
{
  int i;

  for (i = 0; i < sub_titles_n; i++) if (sub_titles[i].sub == sub) break;
  if (i == sub_titles_n)
  {
    if (sub_titles_n == (int)(sizeof(sub_titles) / sizeof(sub_titles[0])))        /* full: the menus that no longer exist go (the live ones stay) */
    {
      int k, o = 0;

      for (k = 0; k < sub_titles_n; k++) if (IsMenu(sub_titles[k].sub)) sub_titles[o++] = sub_titles[k];
      sub_titles_n = o;
      i = o < (int)(sizeof(sub_titles) / sizeof(sub_titles[0])) ? o : 0;
      if (i == o) sub_titles_n++;
    }
    else sub_titles_n++;
  }
  sub_titles[i].sub = sub;
  lstrcpynA(sub_titles[i].title, english, sizeof(sub_titles[i].title));
}

const char *i18n_menu_title(HMENU sub)
{
  int i;

  for (i = 0; i < sub_titles_n; i++) if (sub_titles[i].sub == sub) return IsMenu(sub) ? sub_titles[i].title : NULL;
  return NULL;
}

void i18n_menu(HMENU menu)
{
  int i, n;

  if (!i18n_on || !menu) return;
  n = GetMenuItemCount(menu);
  for (i = 0; i < n; i++)
  {
    MENUITEMINFOA mi;
    char text[256], out[300];

    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    mi.fMask = MIIM_FTYPE | MIIM_SUBMENU;
    if (!GetMenuItemInfoA(menu, (UINT)i, TRUE, &mi)) continue;
    if (mi.hSubMenu) i18n_menu(mi.hSubMenu);
    if (mi.fType & (MFT_SEPARATOR | MFT_OWNERDRAW | MFT_BITMAP)) continue;
    i18n_GetMenuStringA(menu, (UINT)i, text, sizeof(text), MF_BYPOSITION);
    if (mi.hSubMenu) remember_title(mi.hSubMenu, text);
    menu_text(text, out, sizeof(out));
    if (strcmp(text, out))
    {
      memset(&mi, 0, sizeof(mi));
      mi.cbSize = sizeof(mi);
      mi.fMask = MIIM_STRING;
      mi.dwTypeData = out;
      i18n_SetMenuItemInfoA(menu, (UINT)i, TRUE, &mi);              /* (the Unicode call, with the text converted from UTF-8 here) */
    }
  }
}

/* A long text, a line at a time (the end of each line is kept). */
char *i18n_block(const char *text)
{
  sb_t out = { 0 };
  const char *p = text;

  if (!text) return NULL;
  while (*p)
  {
    const char *eol = strchr(p, '\n');
    size_t len = eol ? (size_t)(eol - p) : strlen(p), body = len;
    char line[1024];
    const char *t = NULL;

    if (body && p[body - 1] == '\r') body--;
    if (body > 0 && body < sizeof(line))
    {
      memcpy(line, p, body);
      line[body] = '\0';
      t = find(line);
    }
    if (t) sb_add(&out, t, strlen(t)); else sb_add(&out, p, body);
    sb_add(&out, p + body, len - body);
    if (eol) { sb_add(&out, "\n", 1); p = eol + 1; } else break;
  }
  if (!out.p) out.p = _strdup("");
  return out.p;
}

/* ---- fitting the controls to the translated text ------------------------

   The windows are laid out for English. A translation is often longer, and a button, a check box, a label or a drop-down then
   shows only part of its text. After a window is laid out (WM_INITDIALOG, WM_SIZE, shown) every control whose text is wider than
   it is made wider, as far as the room to its right allows (the next control, or the edge of the window); a dialog grows when a
   control would not fit in it; a label that still does not fit ends in "...". The list of a drop-down is as wide as its widest text. */

static int in_fit;

static int text_px(HDC dc, const char *t)
{
  char plain[512];
  WCHAR w[512];
  SIZE sz;
  int i, o = 0, n;

  for (i = 0; t[i] && o < (int)sizeof(plain) - 1; i++)           /* the key letter's & is not drawn (&& is one &) */
  {
    if (t[i] == '&' && t[i + 1] == '&') { plain[o++] = '&'; i++; }
    else if (t[i] != '&') plain[o++] = t[i];
  }
  plain[o] = '\0';
  n = MultiByteToWideChar(CP_UTF8, 0, plain, -1, w, 512);
  if (n <= 1) return 0;
  return GetTextExtentPoint32W(dc, w, n - 1, &sz) ? sz.cx : 0;
}

static int is_standard_class(const char *cls)
{
  return !lstrcmpiA(cls, "Button") || !lstrcmpiA(cls, "Static") || !lstrcmpiA(cls, "Edit") || !lstrcmpiA(cls, "ComboBox") || !lstrcmpiA(cls, "ListBox") ||
         !strncmp(cls, "SysListView", 11) || !strncmp(cls, "msctls", 6) || !strncmp(cls, "SysTab", 6) || !strncmp(cls, "SysHeader", 9) ||
         !strncmp(cls, "RichEdit", 8) || !strncmp(cls, "tooltips", 8) || !strncmp(cls, "ComboLBox", 9) || !strncmp(cls, "SysTreeView", 11);
}

static int fit_bottom_out;

static int wrap_px(HDC dc, const char *t, int width)
{
  WCHAR w[1024];
  RECT r;
  int n = MultiByteToWideChar(CP_UTF8, 0, t, -1, w, 1024);

  if (n <= 1) return 0;
  r.left = r.top = r.bottom = 0;
  r.right = width;
  r.right = width > 8 ? width - 4 : width;                            /* (a little narrower than the control, to be on the safe side) */
  DrawTextW(dc, w, n - 1, &r, DT_CALCRECT | DT_WORDBREAK);
  { TEXTMETRICW tm; if (GetTextMetricsW(dc, &tm)) r.bottom += tm.tmHeight / 2; }
  return r.bottom;
}

static void fit_children(HWND parent, int *out_right, int allow_push)
{
  HWND list[256];
  RECT rc[256], pc;
  int orig[256];
  int n = 0, i, j, dpi;
  HWND c;
  HDC sdc = GetDC(NULL);

  dpi = sdc ? GetDeviceCaps(sdc, LOGPIXELSX) : 96;
  if (sdc) ReleaseDC(NULL, sdc);
  GetClientRect(parent, &pc);
  for (c = GetWindow(parent, GW_CHILD); c && n < 256; c = GetWindow(c, GW_HWNDNEXT))
  {
    if (!(GetWindowLongA(c, GWL_STYLE) & WS_VISIBLE)) continue;      /* (not IsWindowVisible: the window is not shown yet while it is laid out) */
    list[n] = c;
    GetWindowRect(c, &rc[n]);
    MapWindowPoints(NULL, parent, (POINT *)&rc[n], 2);
    orig[n] = rc[n].left;
    n++;
  }

  for (i = 0; i < n; i++)
  {
    char cls[48], text[512];
    LONG st = GetWindowLongA(list[i], GWL_STYLE);
    int need = 0, width = rc[i].right - rc[i].left, limit, newright, fixed_label = 0, wrap_h = 0;
    HDC dc;
    HFONT font, old;

    GetClassNameA(list[i], cls, sizeof(cls));
    if (!is_standard_class(cls))
    {
      if (GetWindow(list[i], GW_CHILD)) fit_children(list[i], NULL, 0);       /* a page of controls of the program's own */
      continue;
    }
    dc = GetDC(list[i]);
    if (!dc) continue;
    font = (HFONT)SendMessageA(list[i], WM_GETFONT, 0, 0);
    old = font ? (HFONT)SelectObject(dc, font) : NULL;

    if (!lstrcmpiA(cls, "Button"))
    {
      int type = st & BS_TYPEMASK;

      GetWindowTextA(list[i], text, sizeof(text));
      if (text[0] && type != BS_GROUPBOX)                              /* (a button the program draws itself has its text as the window text too) */
        need = text_px(dc, text) + ((type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON) ? 16 * dpi / 96 : 26 * dpi / 96);
    }
    else if (!lstrcmpiA(cls, "Static"))
    {
      if ((st & SS_TYPEMASK) == SS_LEFT || (st & SS_TYPEMASK) == SS_SIMPLE)
      {
        GetWindowTextA(list[i], text, sizeof(text));
        if (text[0] && !strchr(text, '\n') && rc[i].bottom - rc[i].top < 2 * 13 * dpi / 96) { need = text_px(dc, text) + 4 * dpi / 96; fixed_label = 1; }
      }
      if ((st & SS_TYPEMASK) <= SS_RIGHT && !(st & (SS_ENDELLIPSIS | SS_PATHELLIPSIS | SS_WORDELLIPSIS)) && rc[i].bottom - rc[i].top >= 2 * 13 * dpi / 96)
      {
        GetWindowTextA(list[i], text, sizeof(text));              /* a text that wraps over several lines: the height it needs */
        if (text[0]) wrap_h = wrap_px(dc, text, width) + 2 * dpi / 96;
      }
    }
    else if (!lstrcmpiA(cls, "ComboBox") && (st & 3) != CBS_SIMPLE)
    {
      int k, cnt = (int)SendMessageA(list[i], CB_GETCOUNT, 0, 0), widest = 0, sel = (int)SendMessageA(list[i], CB_GETCURSEL, 0, 0);

      if (!(st & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE)) || (st & CBS_HASSTRINGS))
      {
        for (k = 0; k < cnt && k < 200; k++)
        {
          int len = (int)SendMessageA(list[i], CB_GETLBTEXTLEN, (WPARAM)k, 0);

          if (len > 0 && len < 500 && SendMessageA(list[i], CB_GETLBTEXT, (WPARAM)k, (LPARAM)text) > 0)
          {
            int px = text_px(dc, text);

            if (px > widest) widest = px;
            if (k == sel) need = px + GetSystemMetrics(SM_CXVSCROLL) + 12 * dpi / 96;
          }
        }
        if (widest > 0)
        {
          int drop = widest + GetSystemMetrics(SM_CXVSCROLL) + 10 * dpi / 96;

          if (drop > width) SendMessageA(list[i], CB_SETDROPPEDWIDTH, (WPARAM)drop, 0);
        }
      }
    }
    if (old) SelectObject(dc, old);
    ReleaseDC(list[i], dc);

    if (wrap_h > rc[i].bottom - rc[i].top)
    {
      int extra = wrap_h - (rc[i].bottom - rc[i].top), oldb = rc[i].bottom;

      SetWindowPos(list[i], NULL, 0, 0, width, wrap_h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
      rc[i].bottom += extra;
      if (allow_push)                                                 /* what is below moves down (the dialog grows), the frame around it too */
        for (j = 0; j < n; j++)
        {
          char kc[16];

          GetClassNameA(list[j], kc, sizeof(kc));
          if (j != i && !lstrcmpiA(kc, "Button") && (GetWindowLongA(list[j], GWL_STYLE) & BS_TYPEMASK) == BS_GROUPBOX &&
              rc[j].left <= rc[i].left && rc[j].right >= rc[i].right && rc[j].top < rc[i].top && rc[j].bottom >= oldb)
          {
            rc[j].bottom += extra;
            SetWindowPos(list[j], NULL, 0, 0, rc[j].right - rc[j].left, rc[j].bottom - rc[j].top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
          }
        }
      if (allow_push)
        for (j = 0; j < n; j++)
          if (j != i && rc[j].top >= oldb - 2 * dpi / 96 && rc[j].left < rc[i].right && rc[j].right > rc[i].left)
          {
            SetWindowPos(list[j], NULL, rc[j].left, rc[j].top + extra, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            rc[j].top += extra;
            rc[j].bottom += extra;
          }
    }
    if (need > width)
    {
      limit = pc.right - 6 * dpi / 96;
      for (j = 0; j < n; j++)
        if (j != i && rc[j].left >= rc[i].right - 2 * dpi / 96 && rc[j].top < rc[i].bottom && rc[j].bottom > rc[i].top && rc[j].left - 4 < limit)
          limit = rc[j].left - 4 * dpi / 96;
      newright = rc[i].left + need;
      if (newright > limit) newright = limit;
      if (newright > rc[i].right)
      {
        SetWindowPos(list[i], NULL, 0, 0, newright - rc[i].left, rc[i].bottom - rc[i].top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        rc[i].right = newright;
        width = newright - rc[i].left;
      }
      if (allow_push && need > width && (!lstrcmpiA(cls, "Button") || fixed_label || !lstrcmpiA(cls, "ComboBox")))        /* a control boxed in by its neighbours: they move over, the dialog grows */
      {
        int extra = need - width;

        for (j = 0; j < n; j++)
          if (j != i && rc[j].left >= rc[i].right - 2 * dpi / 96 && rc[j].top < rc[i].bottom && rc[j].bottom > rc[i].top)
          {
            int oldr = rc[j].right, k;

            SetWindowPos(list[j], NULL, rc[j].left + extra, rc[j].top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            rc[j].left += extra;
            rc[j].right += extra;
            for (k = 0; k < n; k++)                                       /* the titled frame around it grows too */
            {
              char kc[16];

              GetClassNameA(list[k], kc, sizeof(kc));
              if (k != j && !lstrcmpiA(kc, "Button") && (GetWindowLongA(list[k], GWL_STYLE) & BS_TYPEMASK) == BS_GROUPBOX &&
                  rc[k].left <= rc[j].left && rc[k].right >= oldr && rc[k].top < rc[j].top && rc[k].bottom > rc[j].bottom &&
                  rc[j].right + (rc[k].right - oldr) > rc[k].right)
              {
                rc[k].right = rc[j].right + (rc[k].right - oldr);
                SetWindowPos(list[k], NULL, 0, 0, rc[k].right - rc[k].left, rc[k].bottom - rc[k].top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
              }
            }
          }
        SetWindowPos(list[i], NULL, 0, 0, need, rc[i].bottom - rc[i].top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        rc[i].right = rc[i].left + need;
        width = need;
      }
      if (fixed_label && need > width) SetWindowLongA(list[i], GWL_STYLE, GetWindowLongA(list[i], GWL_STYLE) | SS_ENDELLIPSIS);
    }
  }
  if (allow_push)           /* controls that were in a column (the sliders of a list of rows) stay in one: every one of them moves as far as the farthest */
  {
    int prev = -1000000, tol = 3 * dpi / 96;

    for (;;)
    {
      int key = 1 << 30, target = 0, k, m;

      for (k = 0; k < n; k++) if (orig[k] > prev && orig[k] < key) key = orig[k];
      if (key == 1 << 30) break;
      prev = key + tol;
      for (k = 0; k < n; k++) if (orig[k] >= key && orig[k] <= key + tol && rc[k].left - orig[k] > target) target = rc[k].left - orig[k];
      if (target <= 0) continue;
      for (k = 0; k < n; k++)
      {
        int d = target - (rc[k].left - orig[k]);
        char kc[16];

        if (orig[k] < key || orig[k] > key + tol || d <= 0) continue;
        GetClassNameA(list[k], kc, sizeof(kc));
        if (!lstrcmpiA(kc, "Button") && (GetWindowLongA(list[k], GWL_STYLE) & BS_TYPEMASK) == BS_GROUPBOX) continue;
        for (m = 0; m < n; m++)
        {
          if (m != k && !(rc[m].left >= rc[k].right - 2 * dpi / 96 && rc[m].top < rc[k].bottom && rc[m].bottom > rc[k].top)) continue;
          SetWindowPos(list[m], NULL, rc[m].left + d, rc[m].top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
          rc[m].left += d;
          rc[m].right += d;
        }
      }
    }
  }
  if (out_right)
  {
    int r = 0;

    for (i = 0; i < n; i++) if (rc[i].right > r) r = rc[i].right;
    *out_right = r;
  }
  {
    int b = 0;

    for (i = 0; i < n; i++) if (rc[i].bottom > b) b = rc[i].bottom;
    if (out_right) fit_bottom_out = b;
  }
}

/* How much wider than it is a window's text would have to be to show every text of its controls: 100 when all fit, more (at most 180) when
   some do not. For a window the program lays out by itself, which then scales its layout by this (the Controls window). */
int i18n_text_ratio(HWND w)
{
  HWND c;
  int best = 100, dpi;
  HDC sdc = GetDC(NULL);

  dpi = sdc ? GetDeviceCaps(sdc, LOGPIXELSX) : 96;
  if (sdc) ReleaseDC(NULL, sdc);
  if (!i18n_on) return 100;
  for (c = GetWindow(w, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
  {
    char cls[48], text[512];
    LONG st = GetWindowLongA(c, GWL_STYLE);
    RECT r;
    int width, need = 0;
    HDC dc;
    HFONT font, old;

    GetClassNameA(c, cls, sizeof(cls));
    if (!is_standard_class(cls) || !GetWindowRect(c, &r)) continue;
    width = r.right - r.left;
    if (width < 8) continue;
    if (lstrcmpiA(cls, "Button") && !(!lstrcmpiA(cls, "Static") && ((st & SS_TYPEMASK) == SS_LEFT || (st & SS_TYPEMASK) == SS_SIMPLE))) continue;
    GetWindowTextA(c, text, sizeof(text));
    if (!text[0] || strchr(text, '\n') || (!lstrcmpiA(cls, "Button") && (st & BS_TYPEMASK) == BS_GROUPBOX)) continue;
    dc = GetDC(c);
    if (!dc) continue;
    font = (HFONT)SendMessageA(c, WM_GETFONT, 0, 0);
    old = font ? (HFONT)SelectObject(dc, font) : NULL;
    need = text_px(dc, text);
    if (old) SelectObject(dc, old);
    ReleaseDC(c, dc);
    if (!lstrcmpiA(cls, "Static")) need += 4 * dpi / 96;
    else if ((st & BS_TYPEMASK) == BS_PUSHBUTTON || (st & BS_TYPEMASK) == BS_DEFPUSHBUTTON || (st & BS_TYPEMASK) == BS_OWNERDRAW) need += 18 * dpi / 96;
    else need += 24 * dpi / 96;
    if (need * 100 / width > best) best = need * 100 / width;
  }
  return best > 180 ? 180 : best;
}

static void fit_window(HWND w, int dialog)
{
  int right = 0;

  if (in_fit || !w) return;
  in_fit = 1;
  fit_children(w, &right, dialog);
  if (dialog)                                                       /* a dialog grows when a control sticks out of it */
  {
    RECT cr, wr;

    GetClientRect(w, &cr);
    GetWindowRect(w, &wr);
    if (fit_bottom_out > cr.bottom - 2)                          /* the buttons along the bottom edge go down with it */
    {
      HWND c;
      int grow = fit_bottom_out + 8 - cr.bottom;

      for (c = GetWindow(w, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
      {
        char kc[16];
        RECT r;

        GetClassNameA(c, kc, sizeof(kc));
        if (lstrcmpiA(kc, "Button") || (GetWindowLongA(c, GWL_STYLE) & BS_TYPEMASK) == BS_GROUPBOX || !(GetWindowLongA(c, GWL_STYLE) & WS_VISIBLE)) continue;
        GetWindowRect(c, &r);
        MapWindowPoints(NULL, w, (POINT *)&r, 2);
        if (r.bottom >= cr.bottom - 30) SetWindowPos(c, NULL, r.left, r.top + grow, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
      }
    }
    if (right > cr.right - 4 || fit_bottom_out > cr.bottom - 2)
      SetWindowPos(w, NULL, 0, 0, wr.right - wr.left + (right > cr.right - 4 ? right + 8 - cr.right : 0),
                   wr.bottom - wr.top + (fit_bottom_out > cr.bottom - 2 ? fit_bottom_out + 8 - cr.bottom : 0), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
  in_fit = 0;
}

/* A message box (Windows lays it out and sizes it by itself): only buttons and labels, with the ids of the standard buttons, the text and the icon. */
static int is_msgbox(HWND w)
{
  HWND c;
  int n = 0;

  for (c = GetWindow(w, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
  {
    int id = GetDlgCtrlID(c);

    if (!((id >= 1 && id <= 11) || id == 0xFFFF || id == 0x14 || id == 0)) return 0;
    n++;
  }
  return n > 0 && n <= 6;
}

static LRESULT CALLBACK dlg_hook_proc(int code, WPARAM wp, LPARAM lp)
{
  const CWPRETSTRUCT *r = (const CWPRETSTRUCT *)lp;

  if (code >= 0 && r->message == WM_INITDIALOG)
  {
    char cls[32], text[512];
    HWND c;

    GetWindowTextA(r->hwnd, text, sizeof(text));
    if (find(text)) SetWindowTextA(r->hwnd, find(text));
    for (c = GetWindow(r->hwnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
    {
      GetClassNameA(c, cls, sizeof(cls));
      if (lstrcmpiA(cls, "Button") && lstrcmpiA(cls, "Static")) continue;
      GetWindowTextA(c, text, sizeof(text));
      if (text[0] && find(text)) SetWindowTextA(c, find(text));
    }
    if (!is_msgbox(r->hwnd)) fit_window(r->hwnd, 1);
  }
  else if (code >= 0 && !in_fit && ((r->message == WM_SIZE && r->wParam != SIZE_MINIMIZED && (GetWindowLongA(r->hwnd, GWL_STYLE) & WS_CHILD)) || (r->message == WM_SHOWWINDOW && r->wParam)))      /* (a top-level window being resized lays itself out: only the pages inside a window are fitted again) */
  {
    char cls[32];

    GetClassNameA(r->hwnd, cls, sizeof(cls));
    if (!is_standard_class(cls) && GetWindow(r->hwnd, GW_CHILD)) { if (lstrcmpA(cls, "#32770") || !is_msgbox(r->hwnd)) fit_window(r->hwnd, !lstrcmpA(cls, "#32770")); }
  }
  return CallNextHookEx(dlg_hook, code, wp, lp);
}

/* ---- languages ------------------------------------------------------------ */

int i18n_list(char names[][48], int max)
{
  WIN32_FIND_DATAA fd;
  HANDLE h;
  char pat[MAX_PATH + 16];
  int n = 0;

  _snprintf(pat, sizeof(pat), "%s\\*.po", osd_path("Languages"));
  h = FindFirstFileA(pat, &fd);
  if (h == INVALID_HANDLE_VALUE) return 0;
  do
  {
    char *dot;

    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    lstrcpynA(names[n], fd.cFileName, 48);
    dot = strrchr(names[n], '.');
    if (dot) *dot = '\0';
    if (names[n][0] && n < max) n++;
  } while (n < max && FindNextFileA(h, &fd));
  FindClose(h);
  return n;
}

/* How a language names itself, from "Language-Name: Francais" in the header of its file (the name of the file when there is none): the menu
   shows it as it is, in its own language, never translated. */
void i18n_language_name(const char *file, char *out, int len)
{
  char path[MAX_PATH + 64], buf[4096];
  HANDLE f;
  DWORD got = 0;
  char *p, *e;

  lstrcpynA(out, file, len);
  _snprintf(path, sizeof(path), "%s\\%s.po", osd_path("Languages"), file);
  f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  if (f == INVALID_HANDLE_VALUE) return;
  if (!ReadFile(f, buf, sizeof(buf) - 1, &got, NULL)) got = 0;
  CloseHandle(f);
  buf[got] = '\0';
  p = strstr(buf, "Language-Name:");
  if (!p) return;
  p += 14;
  while (*p == ' ') p++;
  for (e = p; *e && *e != '\n' && *e != '\r' && *e != '"' && !(e[0] == '\\' && e[1] == 'n'); e++) ;
  if (e > p && e - p < len) { memcpy(out, p, (size_t)(e - p)); out[e - p] = '\0'; }
}

void i18n_init(const char *language)
{
  char path[MAX_PATH + 64];
  HANDLE f;
  DWORD size, got = 0;
  char *data;

  clear_table();
  i18n_on = 0;
  if (!language || !language[0] || !lstrcmpiA(language, "English")) return;
  _snprintf(path, sizeof(path), "%s\\%s.po", osd_path("Languages"), language);
  f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  if (f == INVALID_HANDLE_VALUE) return;
  size = GetFileSize(f, NULL);
  data = (char *)malloc((size_t)size + 1);
  if (data && ReadFile(f, data, size, &got, NULL))
  {
    data[got] = '\0';
    load_po(data);
  }
  free(data);
  CloseHandle(f);
  i18n_on = tab_count > 0;
  if (i18n_on && !dlg_hook) dlg_hook = SetWindowsHookExA(WH_CALLWNDPROCRET, dlg_hook_proc, NULL, GetCurrentThreadId());
}

static BOOL find_language_menu(HMENU m, HMENU *found)
{
  int i, n = GetMenuItemCount(m);

  for (i = 0; i < n; i++)
  {
    HMENU sub = GetSubMenu(m, i);

    if ((int)GetMenuItemID(m, i) == IDM_OPT_LANGUAGE_BASE) { *found = m; return TRUE; }
    if (sub && find_language_menu(sub, found)) return TRUE;
  }
  return FALSE;
}

void i18n_language_menu(HMENU menu, const char *current)
{
  HMENU lm = NULL;
  char names[31][48];
  int i, n;

  if (!menu || !find_language_menu(menu, &lm)) return;
  while (GetMenuItemCount(lm) > 0) DeleteMenu(lm, 0, MF_BYPOSITION);
  AppendMenuA(lm, MF_STRING, IDM_OPT_LANGUAGE_BASE, "English");
  n = i18n_list(names, 31);
  for (i = 0; i < n; i++)
  {
    char shown[48];

    i18n_language_name(names[i], shown, sizeof(shown));
    AppendMenuA(lm, MF_STRING, IDM_OPT_LANGUAGE_BASE + 1 + i, shown);
  }
  CheckMenuRadioItem(lm, IDM_OPT_LANGUAGE_BASE, IDM_OPT_LANGUAGE_BASE + 31, IDM_OPT_LANGUAGE_BASE, MF_BYCOMMAND);
  for (i = 0; i < n; i++)
    if (current && !lstrcmpiA(current, names[i]))
      CheckMenuRadioItem(lm, IDM_OPT_LANGUAGE_BASE, IDM_OPT_LANGUAGE_BASE + 31, IDM_OPT_LANGUAGE_BASE + 1 + i, MF_BYCOMMAND);
}

int i18n_language_from_command(int cmd, char *name, int len)
{
  char names[31][48];
  int n;

  if (cmd < IDM_OPT_LANGUAGE_BASE || cmd > IDM_OPT_LANGUAGE_BASE + 31) return 0;
  if (cmd == IDM_OPT_LANGUAGE_BASE) { lstrcpynA(name, "", len); return 1; }
  n = i18n_list(names, 31);
  if (cmd - IDM_OPT_LANGUAGE_BASE - 1 >= n) return 0;
  lstrcpynA(name, names[cmd - IDM_OPT_LANGUAGE_BASE - 1], len);
  return 1;
}

/* ---- the Windows calls --------------------------------------------------- */

static int is_text_class(HWND w)
{
  char cls[32];

  GetClassNameA(w, cls, sizeof(cls));
  return lstrcmpiA(cls, "Edit") && strncmp(cls, "RichEdit", 8) && lstrcmpiA(cls, "ComboBox");
}

static int is_class(HWND w, const char *cls)
{
  char c[48];

  return GetClassNameA(w, c, sizeof(c)) && !lstrcmpiA(c, cls);
}

LRESULT WINAPI i18n_SendMessageA(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
BOOL WINAPI i18n_SetWindowTextA(HWND wnd, LPCSTR text);

int WINAPIV i18n_wsprintfA(LPSTR out, LPCSTR fmt, ...)
{
  va_list ap;
  int n;

  va_start(ap, fmt);
  n = wvsprintfA(out, tr(fmt), ap);
  va_end(ap);
  return n;
}

HWND WINAPI i18n_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
  if (i18n_on && name && (ULONG_PTR)cls > 0xFFFF && lstrcmpiA(cls, "EDIT") && strncmp(cls, "RichEdit", 8)) name = tr(name);
  return CreateWindowExA(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
}

BOOL WINAPI i18n_SetDlgItemTextA(HWND dlg, int id, LPCSTR text)
{
  HWND c = GetDlgItem(dlg, id);

  return c ? i18n_SetWindowTextA(c, text) : SetDlgItemTextA(dlg, id, text);
}

LRESULT WINAPI i18n_SendDlgItemMessageA(HWND dlg, int id, UINT msg, WPARAM wp, LPARAM lp)
{
  HWND c = GetDlgItem(dlg, id);

  return c ? i18n_SendMessageA(c, msg, wp, lp) : 0;
}

BOOL WINAPI i18n_SetWindowTextA(HWND wnd, LPCSTR text)
{
  if (i18n_on && text && is_text_class(wnd))
  {
    if (strlen(text) > 300)
    {
      char *b = i18n_block(text);
      BOOL r = SetWindowTextA(wnd, b);

      free(b);
      return r;
    }
    text = tr(text);
  }
  return SetWindowTextA(wnd, text);
}

/* ---- menu text, always through the Unicode calls ------------------------------
   The program's text is UTF-8. The A versions of the menu calls convert with the code page of the process, which is not always UTF-8,
   and a conversion that does not match loses or changes characters: the menu text goes through the W versions with an explicit
   UTF-8 conversion instead. A text that is not valid UTF-8 (a name that came from Windows in the ANSI code page) is passed as it is. */

static int has_high(const char *s, int n);
static int real_ptr(const void *p);
static int u8_wide(const char *s, WCHAR *w, int cap)
{
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, cap);
}

/* UTF-16 to UTF-8 in a buffer of cap bytes: cut at a whole character when it is too short; the number of bytes without the end. */
static int utf8_copy(char *out, int cap, const WCHAR *w)
{
  char tmp[2200];
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, tmp, sizeof(tmp), NULL, NULL);

  if (cap < 1) return 0;
  if (n <= 0) { out[0] = '\0'; return 0; }
  if (n - 1 >= cap)
  {
    n = cap - 1;
    while (n > 0 && ((unsigned char)tmp[n] & 0xC0) == 0x80) n--;               /* not in the middle of a character */
    memcpy(out, tmp, (size_t)n);
    out[n] = '\0';
    return n;
  }
  memcpy(out, tmp, (size_t)n);
  return n - 1;
}

BOOL WINAPI i18n_SetMenuItemInfoA(HMENU menu, UINT item, BOOL by_pos, LPCMENUITEMINFOA mi)
{
  WCHAR w[700];

  if ((mi->fMask & MIIM_STRING) && mi->dwTypeData && has_high(mi->dwTypeData, -1) &&
      !((mi->fMask & MIIM_FTYPE) && (mi->fType & (MFT_OWNERDRAW | MFT_BITMAP | MFT_SEPARATOR))) && u8_wide(mi->dwTypeData, w, 700) > 0)
  {
    MENUITEMINFOW mw;

    memset(&mw, 0, sizeof(mw));
    mw.cbSize = sizeof(mw);
    mw.fMask = mi->fMask;
    mw.fType = mi->fType;
    mw.fState = mi->fState;
    mw.wID = mi->wID;
    mw.hSubMenu = mi->hSubMenu;
    mw.hbmpChecked = mi->hbmpChecked;
    mw.hbmpUnchecked = mi->hbmpUnchecked;
    mw.dwItemData = mi->dwItemData;
    mw.dwTypeData = w;
    mw.hbmpItem = mi->hbmpItem;
    return SetMenuItemInfoW(menu, item, by_pos, &mw);
  }
  return SetMenuItemInfoA(menu, item, by_pos, mi);
}

BOOL WINAPI i18n_GetMenuItemInfoA(HMENU menu, UINT item, BOOL by_pos, LPMENUITEMINFOA mi)
{
  if ((mi->fMask & MIIM_STRING) && mi->dwTypeData && mi->cch > 1)
  {
    WCHAR w[700];
    MENUITEMINFOW mw;
    BOOL r;

    memset(&mw, 0, sizeof(mw));
    mw.cbSize = sizeof(mw);
    mw.fMask = mi->fMask | MIIM_FTYPE;
    mw.dwTypeData = w;
    mw.cch = 699;
    w[0] = 0;
    r = GetMenuItemInfoW(menu, item, by_pos, &mw);
    if (!r) return FALSE;
    mi->fType = mw.fType;
    mi->fState = mw.fState;
    mi->wID = mw.wID;
    mi->hSubMenu = mw.hSubMenu;
    mi->hbmpChecked = mw.hbmpChecked;
    mi->hbmpUnchecked = mw.hbmpUnchecked;
    mi->dwItemData = mw.dwItemData;
    mi->hbmpItem = mw.hbmpItem;
    if (mw.fType & (MFT_OWNERDRAW | MFT_BITMAP | MFT_SEPARATOR)) { mi->dwTypeData[0] = '\0'; mi->cch = 0; }
    else mi->cch = (UINT)utf8_copy(mi->dwTypeData, (int)mi->cch, w);
    return TRUE;
  }
  return GetMenuItemInfoA(menu, item, by_pos, mi);
}

int WINAPI i18n_GetMenuStringA(HMENU menu, UINT item, LPSTR out, int cap, UINT flags)
{
  WCHAR w[700];
  int n;

  if (!out || cap < 2) return GetMenuStringA(menu, item, out, cap, flags);
  n = GetMenuStringW(menu, item, w, 699, flags);
  if (n <= 0) { out[0] = '\0'; return 0; }
  w[n] = 0;
  return utf8_copy(out, cap, w);
}

/* Text for a menu call: the W version when it has more than ASCII. */
static BOOL menu_call_w(int which, HMENU menu, UINT pos, UINT flags, UINT_PTR id, LPCSTR item)
{
  WCHAR w[700];

  if (u8_wide(item, w, 700) <= 0) return -1;
  if (which == 0) return AppendMenuW(menu, flags, id, w);
  if (which == 1) return InsertMenuW(menu, pos, flags, id, w);
  return ModifyMenuW(menu, pos, flags, id, w);
}

BOOL WINAPI i18n_AppendMenuA(HMENU menu, UINT flags, UINT_PTR id, LPCSTR item)
{
  char t[300];

  if (i18n_on && item && !(flags & (MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR))) { menu_text(item, t, sizeof(t)); item = t; }
  if (item && !(flags & (MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR)) && has_high(item, -1)) { BOOL r = menu_call_w(0, menu, 0, flags, id, item); if (r != (BOOL)-1) return r; }
  return AppendMenuA(menu, flags, id, item);
}

BOOL WINAPI i18n_InsertMenuA(HMENU menu, UINT pos, UINT flags, UINT_PTR id, LPCSTR item)
{
  char t[300];

  if (i18n_on && item && !(flags & (MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR))) { menu_text(item, t, sizeof(t)); item = t; }
  if (item && !(flags & (MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR)) && has_high(item, -1)) { BOOL r = menu_call_w(1, menu, pos, flags, id, item); if (r != (BOOL)-1) return r; }
  return InsertMenuA(menu, pos, flags, id, item);
}

BOOL WINAPI i18n_ModifyMenuA(HMENU menu, UINT pos, UINT flags, UINT_PTR id, LPCSTR item)
{
  char t[300];

  if (i18n_on && item && !(flags & (MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR))) { menu_text(item, t, sizeof(t)); item = t; }
  if (item && !(flags & (MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR)) && has_high(item, -1)) { BOOL r = menu_call_w(2, menu, pos, flags, id, item); if (r != (BOOL)-1) return r; }
  return ModifyMenuA(menu, pos, flags, id, item);
}

static int real_ptr(const void *p)
{
  return (ULONG_PTR)p > 0xFFFF && (LONG_PTR)p != -1;
}

LRESULT WINAPI i18n_SendMessageA(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
  if (msg == SB_SETTEXTA && is_class(wnd, "msctls_statusbar32") && real_ptr((const void *)lp) && has_high((const char *)lp, -1))      /* the status bar: UTF-8, whatever the code page of the process is */
  {
    WCHAR w[700];

    if (u8_wide(i18n_on ? tr((const char *)lp) : (const char *)lp, w, 700) > 0) return SendMessageW(wnd, SB_SETTEXTW, wp, (LPARAM)w);
  }
  if ((msg == LVM_INSERTITEMA || msg == LVM_SETITEMA || msg == LVM_SETITEMTEXTA) && real_ptr((const void *)lp) && is_class(wnd, "SysListView32"))
  {
    /* The rows of a list: a name with Chinese, Japanese or accented letters is UTF-8 here, and the list reads ANSI text with the code
       page of its font (Western), which turned it into other characters. Such a text goes in through the Unicode message. */
    const LVITEMA *src = (const LVITEMA *)lp;

    if ((msg == LVM_SETITEMTEXTA || (src->mask & LVIF_TEXT)) && real_ptr(src->pszText) && has_high(src->pszText, -1))
    {
      WCHAR w[700];
      LVITEMW li;

      if (u8_wide(i18n_on ? tr(src->pszText) : src->pszText, w, 700) > 0)
      {
        memcpy(&li, src, sizeof(li));
        li.pszText = w;
        return SendMessageW(wnd, msg == LVM_INSERTITEMA ? LVM_INSERTITEMW : msg == LVM_SETITEMA ? LVM_SETITEMW : LVM_SETITEMTEXTW, wp, (LPARAM)&li);
      }
    }
  }
  if (!i18n_on) return SendMessageA(wnd, msg, wp, lp);

  switch (msg)
  {
    case CB_ADDSTRING: case CB_INSERTSTRING: case CB_FINDSTRING: case CB_FINDSTRINGEXACT: case CB_SELECTSTRING:
    case LB_ADDSTRING: case LB_INSERTSTRING: case LB_FINDSTRING: case LB_FINDSTRINGEXACT: case LB_SELECTSTRING:
    {
      LONG st = GetWindowLongA(wnd, GWL_STYLE);
      int combo = msg >= 0x140 && msg < 0x160;                  /* the CB_ messages; the LB_ ones start at 0x180 */

      /* an owner-drawn list without strings holds data, not text */
      if (combo ? ((st & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE)) && !(st & CBS_HASSTRINGS)) :
                  ((st & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE)) && !(st & LBS_HASSTRINGS)))
        break;
      if (real_ptr((const void *)lp)) lp = (LPARAM)tr((const char *)lp);
      break;
    }
    case WM_SETTEXT:
      if (real_ptr((const void *)lp) && is_text_class(wnd)) lp = (LPARAM)tr((const char *)lp);
      break;
    case TTM_ADDTOOLA: case TTM_UPDATETIPTEXTA: case TTM_SETTOOLINFOA:
    {
      if (!is_class(wnd, "tooltips_class32")) break;           /* (the numbers are WM_USER + n: other windows use them too) */
      TTTOOLINFOA ti;
      const TTTOOLINFOA *src = (const TTTOOLINFOA *)lp;

      if (src && src->cbSize <= sizeof(ti) && real_ptr(src->lpszText))
      {
        memcpy(&ti, src, src->cbSize);
        ti.lpszText = (LPSTR)tr(src->lpszText);
        return SendMessageA(wnd, msg, wp, (LPARAM)&ti);
      }
      break;
    }
    case LVM_INSERTITEMA: case LVM_SETITEMA: case LVM_SETITEMTEXTA:      /* the rows of a list: the words in it are the program's, the names and numbers not found in the file stay as they are */
    {
      LVITEMA li;
      const LVITEMA *src = (const LVITEMA *)lp;

      if (src && (msg == LVM_SETITEMTEXTA || (src->mask & LVIF_TEXT)) && real_ptr(src->pszText))
      {
        li = *src;
        li.pszText = (LPSTR)tr(src->pszText);
        return SendMessageA(wnd, msg, wp, (LPARAM)&li);
      }
      break;
    }
    case TCM_INSERTITEMA: case TCM_SETITEMA:
    {
      TCITEMA it;
      const TCITEMA *src = (const TCITEMA *)lp;

      if (src && (src->mask & TCIF_TEXT) && real_ptr(src->pszText))
      {
        it = *src;
        it.pszText = (LPSTR)tr(src->pszText);
        return SendMessageA(wnd, msg, wp, (LPARAM)&it);
      }
      break;
    }
    case LVM_INSERTCOLUMNA: case LVM_SETCOLUMNA:
    {
      LVCOLUMNA col;
      const LVCOLUMNA *src = (const LVCOLUMNA *)lp;

      if (src && (src->mask & LVCF_TEXT) && real_ptr(src->pszText))
      {
        col = *src;
        col.pszText = (LPSTR)tr(src->pszText);
        return SendMessageA(wnd, msg, wp, (LPARAM)&col);
      }
      break;
    }
  }
  return SendMessageA(wnd, msg, wp, lp);
}

/* Text the program draws is UTF-8. The A versions of the drawing calls read it with the code page of the font in the DC, and the fonts
   Windows gives the menu bar and the status bar are not UTF-8 ones: an accent came out as two wrong characters. So anything
   that is not plain ASCII goes through the W versions. */
static int has_high(const char *s, int n)
{
  int i;

  for (i = 0; (n < 0 ? s[i] : i < n) && s[i]; i++) if ((unsigned char)s[i] >= 0x80) return 1;
  return 0;
}

/* The text as UTF-16 (n bytes, or -1 for a string): the number of characters; *out is malloc'd (free it), NULL if the text is not UTF-8. */
static int to_wide(const char *s, int n, WCHAR **out)
{
  int w = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, n, NULL, 0);

  *out = NULL;
  if (w <= 0) return 0;
  *out = (WCHAR *)malloc(((size_t)w + 1) * sizeof(WCHAR));
  if (!*out) return 0;
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, n, *out, w);
  (*out)[w] = 0;
  return n < 0 ? w - 1 : w;
}

BOOL WINAPI i18n_TextOutA(HDC dc, int x, int y, LPCSTR s, int n)
{
  if (i18n_on && s && n >= 4 && n < 200 && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')))
  {
    char t[200];
    const char *r;

    memcpy(t, s, (size_t)n);
    t[n] = '\0';
    r = find(t);
    if (r) { s = r; n = lstrlenA(r); }
  }
  if (s && has_high(s, n))
  {
    WCHAR *w;
    int len = to_wide(s, n, &w);

    if (w)
    {
      BOOL r = TextOutW(dc, x, y, w, len);

      free(w);
      return r;
    }
  }
  return TextOutA(dc, x, y, s, n);
}

int WINAPI i18n_DrawTextA(HDC dc, LPCSTR s, int n, LPRECT rc, UINT fmt)
{
  if (i18n_on && s && n >= -1 && n < 200)
  {
    char t[200];
    const char *r;
    int len = n < 0 ? lstrlenA(s) : n;

    if (len >= 2 && len < 200)
    {
      memcpy(t, s, (size_t)len);
      t[len] = '\0';
      r = find(t);
      if (r) { s = r; n = -1; }
    }
  }
  if (s && has_high(s, n))
  {
    WCHAR *w;
    int len = to_wide(s, n, &w);

    if (w)
    {
      int r = DrawTextW(dc, w, len, rc, fmt);

      free(w);
      return r;
    }
  }
  return DrawTextA(dc, s, n, rc, fmt);
}

BOOL WINAPI i18n_GetTextExtentPoint32A(HDC dc, LPCSTR s, int n, LPSIZE size)
{
  if (s && has_high(s, n))
  {
    WCHAR *w;
    int len = to_wide(s, n, &w);

    if (w)
    {
      BOOL r = GetTextExtentPoint32W(dc, w, len, size);

      free(w);
      return r;
    }
  }
  return GetTextExtentPoint32A(dc, s, n, size);
}
