/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  ramwatch.c -- Tools > Debugger > RAM Watch tab: a list of addresses whose values
 *  are shown while the game runs (lives, health, a timer ...), with a note
 *  for each. It is a window of its own that stays open.
 *
 *  The list is kept only while the program runs; it can be saved, opened and appended in the .wch format of Gens, whose
 *  RAM Watch this follows: one line for a watch with the address, the size
 *  (b, w or d), the type (s, u or h) and the note; a separator is a line of
 *  its own. The idea of such a window comes from the Genesis Plus GX debugger
 *  project (smd_dgx) and from Gens; this code is written for Win32.
 ****************************************************************************/

#include <windows.h>
#include <commctrl.h>

#include "shared.h"
#include "gui.h"
#include "theme.h"
#include "resource.h"
#include "netplay.h"
#include "picocore.h"
#include "toolbar.h"            /* gui_dpi */

#define RW_CLASS      "GPEXRamWatch"
#define RW_EDIT_CLASS "GPEXWatchEdit"
#define RW_TIMER      1
#define RW_TICK_MS    100
#define RW_MAX        512
#define RW_AGE        6

#define IDC_RW_LIST    101
#define IDC_RW_NEWLIST 110
#define IDC_RW_OPEN    111
#define IDC_RW_SAVE    112
#define IDC_RW_APPEND  113
#define IDC_RW_NEW     120
#define IDC_RW_EDIT    121
#define IDC_RW_REMOVE  122
#define IDC_RW_DUP     123
#define IDC_RW_SEP     124
#define IDC_RW_UP      125
#define IDC_RW_DOWN    126
#define IDC_RW_BTNS    11

typedef struct
{
  unsigned addr;
  char size;                 /* 'b', 'w', 'd'; 'S' = a separator */
  char type;                 /* 's' signed, 'u' unsigned, 'h' hex */
  char note[96];
  unsigned cur;
  int valid;                 /* the address is in a memory the game has */
  int age;                   /* ticks left of marking the value as changed */
} watch_t;

static watch_t rw_list[RW_MAX];
static int rw_count;
static char rw_game[GUI_PATH_LEN];      /* the game the list belongs to */
static HWND rw_win, rw_lv, rw_btn[IDC_RW_BTNS];
static HFONT rw_ui, rw_mono;
static UINT_PTR rw_look;                /* what the window was laid out for (see dbg_look) */
static const int rw_btn_ids[IDC_RW_BTNS] = { IDC_RW_NEWLIST, IDC_RW_OPEN, IDC_RW_SAVE, IDC_RW_APPEND, IDC_RW_NEW, IDC_RW_EDIT, IDC_RW_REMOVE, IDC_RW_DUP,
                                             IDC_RW_SEP, IDC_RW_UP, IDC_RW_DOWN };
static const char *const rw_btn_text[IDC_RW_BTNS] = { "New List", "Open...", "Save...", "Append...", "New...", "Edit...", "Remove", "Duplicate",
                                                      "Separator", "Move Up", "Move Down" };

static int S(int px)
{
  return dbg_scale(px);
}

/****************************************************************************
 * The memory
 ****************************************************************************/

static int rw_md(void)
{
  return pico_active() || (system_hw & SYSTEM_PBC) == SYSTEM_MD;
}

/* An address of the 68000's bus (or the Z80's, in an 8-bit console) is one of the memories of the Memory Viewer: which one, and where in it. */
static int rw_map(unsigned a, int *kind, int *off)
{
  if (pico_active())
  {
    if (a < 0x400000) { *kind = MV_PROM; *off = (int)a; }
    else if (a >= 0x06000000 && a < 0x06040000) { *kind = MV_PSDRAM; *off = (int)(a - 0x06000000); }
    else if (a >= 0xE00000 && a <= 0xFFFFFF) { *kind = MV_PWRAM; *off = (int)(a & 0xFFFF); }
    else return 0;
    return 1;
  }
  if ((system_hw & SYSTEM_PBC) == SYSTEM_MD)
  {
    if (a < 0x400000) { *kind = MV_ROM; *off = (int)a; }
    else if (a >= 0xA00000 && a < 0xA02000) { *kind = MV_ZRAM; *off = (int)(a - 0xA00000); }
    else if (a >= 0xE00000 && a <= 0xFFFFFF) { *kind = MV_WRAM; *off = (int)(a & 0xFFFF); }
    else return 0;
    return 1;
  }
  if (a >= 0xC000 && a <= 0xFFFF) { *kind = MV_WRAM; *off = (int)(a & 0x1FFF); return 1; }
  return 0;
}

static int rw_nbytes(const watch_t *w)
{
  return w->size == 'd' ? 4 : w->size == 'w' ? 2 : 1;
}

/* The value at a watch's address (big-endian on the Mega Drive, little-endian on the 8-bit consoles); 0 and *ok = 0 when it is not in memory. */
static unsigned rw_read(const watch_t *w, int *ok)
{
  int n = rw_nbytes(w), i;
  unsigned v = 0;

  *ok = 1;
  for (i = 0; i < n; i++)
  {
    int kind, off, b;

    if (!rw_map(w->addr + (unsigned)i, &kind, &off) || (b = dbgmem_get(kind, off)) < 0) { *ok = 0; return 0; }
    if (rw_md()) v = (v << 8) | (unsigned)b; else v |= (unsigned)b << (8 * i);
  }
  return v;
}

static int rw_write(const watch_t *w, unsigned v)
{
  int n = rw_nbytes(w), i;

  for (i = 0; i < n; i++)
  {
    int kind, off;
    unsigned b = rw_md() ? (v >> (8 * (n - 1 - i))) & 0xFF : (v >> (8 * i)) & 0xFF;

    if (!rw_map(w->addr + (unsigned)i, &kind, &off)) return 0;
    dbgmem_put(kind, off, (int)b);
  }
  return 1;
}

static void rw_value_text(const watch_t *w, char *out, int len)
{
  int n = rw_nbytes(w);

  if (w->size == 'S') { out[0] = '\0'; return; }
  if (!w->valid) { gui_snprintf(out, len, "??"); return; }
  if (w->type == 'h') gui_snprintf(out, len, n == 1 ? "%02X" : n == 2 ? "%04X" : "%08X", n == 1 ? (w->cur & 0xFF) : n == 2 ? (w->cur & 0xFFFF) : w->cur);
  else if (w->type == 's') gui_snprintf(out, len, "%d", n == 1 ? (int)(signed char)w->cur : n == 2 ? (int)(short)w->cur : (int)w->cur);
  else gui_snprintf(out, len, "%u", n == 1 ? (w->cur & 0xFF) : n == 2 ? (w->cur & 0xFFFF) : w->cur);
}

/****************************************************************************
 * The list
 ****************************************************************************/

static void rw_addr_text(const watch_t *w, char *out, int len)
{
  if (w->size == 'S') { out[0] = '\0'; return; }
  gui_snprintf(out, len, "%08X", w->addr);
}

static void rw_set_row(int i)
{
  char t[40];

  rw_addr_text(&rw_list[i], t, sizeof(t));
  ListView_SetItemText(rw_lv, i, 0, t);
  rw_value_text(&rw_list[i], t, sizeof(t));
  ListView_SetItemText(rw_lv, i, 1, t);
  ListView_SetItemText(rw_lv, i, 2, rw_list[i].size == 'S' ? (char *)"----------------------------" : rw_list[i].note);
}

static void rw_rebuild(int select)
{
  int i;

  SendMessage(rw_lv, WM_SETREDRAW, FALSE, 0);
  ListView_DeleteAllItems(rw_lv);
  for (i = 0; i < rw_count; i++)
  {
    LVITEMA it;

    ZeroMemory(&it, sizeof(it));
    it.mask = LVIF_TEXT;
    it.iItem = i;
    it.pszText = (char *)"";
    SendMessageA(rw_lv, LVM_INSERTITEMA, 0, (LPARAM)&it);
    rw_set_row(i);
  }
  if (select >= 0 && select < rw_count)
  {
    ListView_SetItemState(rw_lv, select, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(rw_lv, select, FALSE);
  }
  SendMessage(rw_lv, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(rw_lv, NULL, TRUE);
}

static int rw_selected(void)
{
  return ListView_GetNextItem(rw_lv, -1, LVNI_SELECTED);
}

/* Every tick: the values are read again, and the ones that changed stay marked for a moment. */
static void rw_tick(void)
{
  int i;

  for (i = 0; i < rw_count; i++)
  {
    watch_t *w = &rw_list[i];
    int ok;
    unsigned v;
    char t[40];

    if (w->size == 'S') continue;
    v = rw_read(w, &ok);
    if (ok != w->valid || (ok && v != w->cur))
    {
      if (ok && w->valid) w->age = RW_AGE;
      w->valid = ok;
      w->cur = v;
      rw_value_text(w, t, sizeof(t));
      ListView_SetItemText(rw_lv, i, 1, t);
    }
    else if (w->age)
    {
      w->age--;
      if (!w->age) InvalidateRect(rw_lv, NULL, FALSE);
    }
  }
}

/* Adds a watch at index (-1: at the end); the same watch twice is skipped. Returns 1 when it was added. */
static int rw_insert(const watch_t *w, int index)
{
  int i;

  if (rw_count >= RW_MAX) return 0;
  if (w->size != 'S')
    for (i = 0; i < rw_count; i++)
      if (rw_list[i].size != 'S' && rw_list[i].addr == w->addr && rw_list[i].size == w->size && rw_list[i].type == w->type) return 0;
  if (index < 0 || index > rw_count) index = rw_count;
  memmove(&rw_list[index + 1], &rw_list[index], (size_t)(rw_count - index) * sizeof(watch_t));
  rw_list[index] = *w;
  rw_count++;
  if (w->size != 'S') rw_list[index].cur = rw_read(&rw_list[index], &rw_list[index].valid);
  return 1;
}

static void rw_remove(int i)
{
  if (i < 0 || i >= rw_count) return;
  memmove(&rw_list[i], &rw_list[i + 1], (size_t)(rw_count - i - 1) * sizeof(watch_t));
  rw_count--;
}

/****************************************************************************
 * Files (the .wch format of Gens)
 ****************************************************************************/

static char *rw_text(size_t *len)
{
  char *t = (char *)malloc((size_t)rw_count * 160 + 64), *o = t;
  int i;

  if (!t) return NULL;
  o += wsprintfA(o, "%d\r\n%d\r\n", pico_active() ? 2 : system_hw == SYSTEM_MCD ? 1 : 0, rw_count);
  for (i = 0; i < rw_count; i++)
  {
    const watch_t *w = &rw_list[i];

    o += wsprintfA(o, "%05X\t%08X\t%c\t%c\t0\t%s\r\n", i, w->addr, w->size, w->type == 0 ? 's' : w->type,
                   w->size == 'S' ? "----------------------------" : w->note);
  }
  *len = (size_t)(o - t);
  return t;
}

static void rw_parse(const char *t, size_t len, int clear)
{
  const char *end = t + len, *p = t;
  int mode = 0, count = 0, read = 0, added = 0;
  char line[256];

#define NEXT_LINE() do { size_t n = 0; while (p < end && *p != '\n') { if (*p != '\r' && n < sizeof(line) - 1) line[n++] = *p; p++; } line[n] = '\0'; if (p < end) p++; } while (0)

  NEXT_LINE(); mode = atoi(line);
  NEXT_LINE(); count = atoi(line);
  if (clear) rw_count = 0;
  if (mode == 1 || mode == 2)
    if ((mode == 2) != (pico_active() != 0) || (mode == 1) != (system_hw == SYSTEM_MCD))
      gui_notify("These watches are for %s addresses; they may not fit this game", mode == 2 ? "32X" : "Mega CD");
  while (read < count && p < end)
  {
    char *f[6], *q = line;
    int nf = 0;
    watch_t w;

    NEXT_LINE();
    if (!line[0]) continue;
    read++;
    f[nf++] = q;
    while (*q && nf < 6) { if (*q == '\t') { *q = '\0'; f[nf++] = q + 1; } q++; }
    if (nf < 6) continue;
    memset(&w, 0, sizeof(w));
    w.addr = (unsigned)strtoul(f[1], NULL, 16) & 0xFFFFFF;
    w.size = f[2][0] == 'w' || f[2][0] == 'd' || f[2][0] == 'S' ? f[2][0] : 'b';
    w.type = f[3][0] == 'u' || f[3][0] == 'h' ? f[3][0] : 's';
    if (w.size != 'S') lstrcpynA(w.note, f[5], sizeof(w.note));
    added += rw_insert(&w, -1);
  }
#undef NEXT_LINE
  (void)added;
}

/* The list is the game's while it runs; a new game starts with an empty one. Lists are kept in files only when asked for (Save... and Open...). */
static void rw_autoload(void)
{
  lstrcpynA(rw_game, emu_rom_path(), sizeof(rw_game));
  rw_count = 0;
}

/****************************************************************************
 * The Edit Watch window (modal)
 ****************************************************************************/

typedef struct
{
  watch_t w;
  char set_value[24];
  int is_new, done, ok;
  HWND parent, dlg, addr, size, type, note, value;
} edit_t;

static void rw_edit_collect(edit_t *e)
{
  char t[64];
  int sz, ty;

  GetWindowTextA(e->addr, t, sizeof(t));
  if (!t[0]) { gui_notify("Type the address in hex"); return; }
  e->w.addr = (unsigned)strtoul(t[0] == '0' && (t[1] == 'x' || t[1] == 'X') ? t + 2 : t, NULL, 16) & 0xFFFFFF;
  sz = (int)SendMessage(e->size, CB_GETCURSEL, 0, 0);
  ty = (int)SendMessage(e->type, CB_GETCURSEL, 0, 0);
  e->w.size = sz == 2 ? 'd' : sz == 1 ? 'w' : 'b';
  e->w.type = ty == 2 ? 'h' : ty == 1 ? 'u' : 's';
  GetWindowTextA(e->note, e->w.note, sizeof(e->w.note));
  GetWindowTextA(e->value, e->set_value, sizeof(e->set_value));
  e->ok = 1;
  e->done = 1;
}

static LRESULT CALLBACK rw_edit_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
  edit_t *e = (edit_t *)GetWindowLongPtr(h, GWLP_USERDATA);

  if (m == WM_NCCREATE) { e = (edit_t *)((CREATESTRUCTA *)lp)->lpCreateParams; SetWindowLongPtr(h, GWLP_USERDATA, (LONG_PTR)e); }
  if (!e) return DefWindowProc(h, m, wp, lp);

  switch (m)
  {
    case WM_COMMAND:
      if (LOWORD(wp) == IDOK) { rw_edit_collect(e); return 0; }
      if (LOWORD(wp) == IDCANCEL) { e->done = 1; return 0; }
      break;
    case WM_CLOSE:
      e->done = 1;
      return 0;
    case WM_ERASEBKGND:                                           /* the window's own background: dark in the dark style */
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
    case WM_CTLCOLORLISTBOX:
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

static HWND rw_ctl(HWND p, const char *cls, const char *text, DWORD style, DWORD ex, int x, int y, int w, int h, int id)
{
  HWND c = CreateWindowExA(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), p, (HMENU)(INT_PTR)id, g_inst, NULL);

  if (c) SendMessage(c, WM_SETFONT, (WPARAM)rw_ui, TRUE);
  return c;
}

/* Edit Watch (also for a new one): the address, the size and type, a note, and a value to put into the memory. Returns 1 for OK. */
static int rw_edit_dialog(HWND parent, watch_t *w, int is_new, int *set, unsigned *set_value)
{
  static int registered;
  edit_t e;
  MSG msg;
  RECT pr, r;
  char t[64];
  DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;

  if (!registered)
  {
    WNDCLASSA wc;

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = rw_edit_proc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = RW_EDIT_CLASS; i18n_keep_ltr(RW_EDIT_CLASS);
    if (!RegisterClassA(&wc)) return 0;
    registered = 1;
  }

  ZeroMemory(&e, sizeof(e));
  e.w = *w;
  e.is_new = is_new;
  e.parent = parent;
  r.left = 0; r.top = 0; r.right = S(300); r.bottom = S(202);
  AdjustWindowRectEx(&r, style, FALSE, 0);
  e.dlg = CreateWindowExA(0, RW_EDIT_CLASS, is_new ? "New Watch" : "Edit Watch", style, 0, 0, r.right - r.left, r.bottom - r.top, GetAncestor(parent, GA_ROOT), NULL, g_inst, &e);
  if (!e.dlg) return 0;

  rw_ctl(e.dlg, "STATIC", "Address (hex):", SS_LEFT, 0, 12, 14, 90, 16, 0);
  gui_snprintf(t, sizeof(t), "%X", w->addr);
  e.addr = rw_ctl(e.dlg, "EDIT", is_new && !w->addr ? "" : t, WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, 108, 11, 100, 22, 201);
  rw_ctl(e.dlg, "STATIC", "Size:", SS_LEFT, 0, 12, 44, 90, 16, 0);
  e.size = rw_ctl(e.dlg, "COMBOBOX", "", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 108, 41, 100, 120, 202);
  SendMessageA(e.size, CB_ADDSTRING, 0, (LPARAM)"Byte");
  SendMessageA(e.size, CB_ADDSTRING, 0, (LPARAM)"Word (2 Bytes)");
  SendMessageA(e.size, CB_ADDSTRING, 0, (LPARAM)"Long (4 Bytes)");
  SendMessage(e.size, CB_SETCURSEL, w->size == 'd' ? 2 : w->size == 'w' ? 1 : 0, 0);
  rw_ctl(e.dlg, "STATIC", "Show as:", SS_LEFT, 0, 12, 74, 90, 16, 0);
  e.type = rw_ctl(e.dlg, "COMBOBOX", "", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 108, 71, 100, 120, 203);
  SendMessageA(e.type, CB_ADDSTRING, 0, (LPARAM)"Signed");
  SendMessageA(e.type, CB_ADDSTRING, 0, (LPARAM)"Unsigned");
  SendMessageA(e.type, CB_ADDSTRING, 0, (LPARAM)"Hex");
  SendMessage(e.type, CB_SETCURSEL, w->type == 'h' ? 2 : w->type == 'u' ? 1 : 0, 0);
  rw_ctl(e.dlg, "STATIC", "Note:", SS_LEFT, 0, 12, 104, 90, 16, 0);
  e.note = rw_ctl(e.dlg, "EDIT", w->note, WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, 108, 101, 180, 22, 204);
  SendMessage(e.note, EM_SETLIMITTEXT, sizeof(w->note) - 1, 0);
  rw_ctl(e.dlg, "STATIC", "Set value:", SS_LEFT, 0, 12, 134, 90, 16, 0);
  e.value = rw_ctl(e.dlg, "EDIT", "", WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, 108, 131, 100, 22, 205);
  rw_ctl(e.dlg, "BUTTON", "OK", WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 120, 164, 80, 26, IDOK);
  rw_ctl(e.dlg, "BUTTON", "Cancel", WS_TABSTOP | BS_PUSHBUTTON, 0, 208, 164, 80, 26, IDCANCEL);
  r.left = 0; r.top = 0; r.right = S(300); r.bottom = S(202);
  AdjustWindowRectEx(&r, style, FALSE, 0);
  GetWindowRect(parent, &pr);
  SetWindowPos(e.dlg, NULL, pr.left + ((pr.right - pr.left) - (r.right - r.left)) / 2, pr.top + ((pr.bottom - pr.top) - (r.bottom - r.top)) / 2,
               r.right - r.left, r.bottom - r.top, SWP_NOZORDER);
  theme_apply_to_window(e.dlg);
  {
    static const GuiTip tips[] =
    {
      { 201, NULL, "The address to watch, in hex (68000 addresses: RAM at E00000 to FFFFFF, Z80 RAM at A00000; a 32X game's SDRAM at 6000000)." },
      { 202, NULL, "How many bytes the value has." },
      { 203, NULL, "How the value is shown: signed or unsigned decimal, or hex." },
      { 204, NULL, "A note shown beside the watch." },
      { 205, NULL, "A value to write to the address when you press OK (leave empty to write nothing)." },
      { IDOK, NULL, "Keep the watch." },
      { IDCANCEL, NULL, "Close without changing anything." }
    };

    gui_dialog_tips(e.dlg, tips, (int)(sizeof(tips) / sizeof(tips[0])));
  }
  gui_window_icon(e.dlg);

  EnableWindow(GetAncestor(parent, GA_ROOT), FALSE);
  ShowWindow(e.dlg, SW_SHOW);
  SetFocus(e.addr);
  SendMessage(e.addr, EM_SETSEL, 0, -1);
  while (!e.done && GetMessage(&msg, NULL, 0, 0))
  {
    if (msg.message == WM_KEYDOWN && (msg.wParam == VK_RETURN || msg.wParam == VK_ESCAPE) && IsChild(e.dlg, msg.hwnd))
    {
      char cls[16];

      GetClassNameA(msg.hwnd, cls, sizeof(cls));
      if (lstrcmpiA(cls, "BUTTON") && lstrcmpiA(cls, "COMBOBOX")) { SendMessage(e.dlg, WM_COMMAND, msg.wParam == VK_RETURN ? IDOK : IDCANCEL, 0); continue; }
    }
    if (!IsDialogMessage(e.dlg, &msg)) { TranslateMessage(&msg); DispatchMessage(&msg); }
  }
  if (msg.message == WM_QUIT && !e.done) PostQuitMessage((int)msg.wParam);
  EnableWindow(GetAncestor(parent, GA_ROOT), TRUE);
  DestroyWindow(e.dlg);
  SetForegroundWindow(GetAncestor(parent, GA_ROOT));

  if (!e.ok) return 0;
  *w = e.w;
  *set = 0;
  if (e.set_value[0])
  {
    const char *s = e.set_value;
    int neg = 0, hex = (w->type == 'h');

    if (*s == '-') { neg = 1; s++; }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { hex = 1; s += 2; }
    *set_value = (unsigned)strtoul(s, NULL, hex ? 16 : 10);
    if (neg) *set_value = (unsigned)(-(int)*set_value);
    *set = 1;
  }
  return 1;
}

/****************************************************************************
 * The window
 ****************************************************************************/

static void rw_fonts(void)
{
  rw_ui = dbg_ui_font();
  rw_mono = dbg_mono_font();
}

static void rw_colors(void)
{
  HDC probe = GetDC(NULL);
  HBRUSH b = theme_ctlcolor(probe);
  COLORREF bg, fg;

  if (b) { bg = GetBkColor(probe); fg = GetTextColor(probe); }
  else { bg = GetSysColor(COLOR_WINDOW); fg = GetSysColor(COLOR_WINDOWTEXT); }
  ReleaseDC(NULL, probe);
  ListView_SetBkColor(rw_lv, bg);
  ListView_SetTextBkColor(rw_lv, bg);
  ListView_SetTextColor(rw_lv, fg);
}

static void rw_layout(void)
{
  RECT rc;
  int i, bw = S(104), bh = S(26), top = S(8), x, y, lw;

  if (!rw_win) return;
  GetClientRect(rw_win, &rc);
  x = S(8);
  for (i = 0; i < 4; i++)           /* the file buttons along the top */
  {
    SetWindowPos(rw_btn[i], NULL, x, top, bw, bh, SWP_NOZORDER);
    x += bw + S(6);
  }
  y = top + bh + S(8);
  lw = rc.right - bw - S(24);
  if (lw < S(100)) lw = S(100);
  SetWindowPos(rw_lv, NULL, S(8), y, lw, rc.bottom - y - S(8), SWP_NOZORDER);
  for (i = 4; i < IDC_RW_BTNS; i++)  /* the edit buttons down the right */
  {
    SetWindowPos(rw_btn[i], NULL, rc.right - bw - S(8), y + (i - 4) * (bh + S(6)), bw, bh, SWP_NOZORDER);
  }
  {
    int w3 = lw - S(160) - S(110) - GetSystemMetrics(SM_CXVSCROLL) - S(8);

    ListView_SetColumnWidth(rw_lv, 0, S(100));
    ListView_SetColumnWidth(rw_lv, 1, S(110));
    ListView_SetColumnWidth(rw_lv, 2, w3 > S(100) ? w3 + S(60) : S(160));
  }
}

static void rw_new_watch(int is_new)
{
  watch_t w;
  int set = 0, sel = rw_selected();
  unsigned val = 0;

  memset(&w, 0, sizeof(w));
  w.size = 'b'; w.type = 'h';
  if (!is_new)
  {
    if (sel < 0 || rw_list[sel].size == 'S') { gui_notify("Select a watch to edit it"); return; }
    w = rw_list[sel];
  }
  else if (sel >= 0 && rw_list[sel].size != 'S')
  {
    w.addr = rw_list[sel].addr;
    w.size = rw_list[sel].size;
    w.type = rw_list[sel].type;
  }
  w.note[0] = is_new ? '\0' : w.note[0];
  if (!rw_edit_dialog(rw_win, &w, is_new, &set, &val)) return;
  if (is_new)
  {
    if (!rw_insert(&w, sel >= 0 ? sel + 1 : -1)) { gui_notify("That watch is in the list already"); return; }
    sel = sel >= 0 ? sel + 1 : rw_count - 1;
  }
  else
  {
    rw_list[sel].addr = w.addr; rw_list[sel].size = w.size; rw_list[sel].type = w.type;
    lstrcpynA(rw_list[sel].note, w.note, sizeof(rw_list[sel].note));
    rw_list[sel].cur = rw_read(&rw_list[sel], &rw_list[sel].valid);
  }
  if (set)
  {
    if (netplay_active()) gui_notify("The memory cannot be changed during netplay");
    else if (!rw_write(&rw_list[sel], val)) gui_notify("That address is not in a memory that can be changed");
  }
  rw_rebuild(sel);
}

static void rw_file_open(int append)
{
  size_t n = 0;
  unsigned char *b = dbg_open_file(rw_win, append ? "Append Watches" : "Open Watches", "Gens watch lists (*.wch)|*.wch|All files (*.*)|*.*", &n);

  if (!b) return;
  rw_parse((const char *)b, n, !append);
  free(b);
  rw_rebuild(-1);
}

static void rw_file_save(void)
{
  size_t n;
  char *t = rw_text(&n);

  if (!t) return;
  if (dbg_save_file(rw_win, "Save Watches", "watches.wch", "Gens watch lists (*.wch)|*.wch|All files (*.*)|*.*", t, n)) gui_notify("Watches saved");
  free(t);
}

static void rw_move(int dir)
{
  int sel = rw_selected(), to = sel + dir;
  watch_t tmp;

  if (sel < 0 || to < 0 || to >= rw_count) return;
  tmp = rw_list[sel]; rw_list[sel] = rw_list[to]; rw_list[to] = tmp;
  rw_rebuild(to);
}

static LRESULT CALLBACK rw_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
  int i;

  switch (m)
  {
    case WM_CREATE:
    {
      LVCOLUMNA col;

      rw_win = h;
      rw_fonts();
      rw_look = dbg_look();
      rw_lv = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                              0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_RW_LIST, g_inst, NULL);
      ListView_SetExtendedListViewStyle(rw_lv, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
      SendMessage(rw_lv, WM_SETFONT, (WPARAM)rw_mono, TRUE);
      ZeroMemory(&col, sizeof(col));
      col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
      col.fmt = LVCFMT_LEFT; col.cx = S(100); col.pszText = (char *)"Address"; SendMessageA(rw_lv, LVM_INSERTCOLUMNA, 0, (LPARAM)&col);
      col.cx = S(110); col.pszText = (char *)"Value"; SendMessageA(rw_lv, LVM_INSERTCOLUMNA, 1, (LPARAM)&col);
      col.cx = S(220); col.pszText = (char *)"Note"; SendMessageA(rw_lv, LVM_INSERTCOLUMNA, 2, (LPARAM)&col);
      for (i = 0; i < IDC_RW_BTNS; i++)
      {
        rw_btn[i] = CreateWindowExA(0, "BUTTON", rw_btn_text[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10, h,
                                    (HMENU)(INT_PTR)rw_btn_ids[i], g_inst, NULL);
        SendMessage(rw_btn[i], WM_SETFONT, (WPARAM)rw_ui, TRUE);
      }
      theme_apply_to_window(h);
      {
        static const char *const tips[IDC_RW_BTNS] =
        {
          "Empty the list and start a new one.",
          "Load a list of watches from a .wch file (the format of Gens), replacing the one shown.",
          "Save the list to a .wch file.",
          "Add the watches of a .wch file to the end of the list.",
          "Add a watch: an address, its size and how to show it (and, if you like, a value to put there).",
          "Change the picked watch, or put a value into its address. Double-clicking a line does the same.",
          "Take the picked watch off the list.",
          "Make a copy of the picked watch under it.",
          "Add a blank line to keep groups of watches apart.",
          "Move the picked watch up one line.",
          "Move the picked watch down one line."
        };
        HWND tt = dbg_tips_new(h);

        for (i = 0; i < IDC_RW_BTNS; i++) dbg_tip(tt, h, rw_btn[i], tips[i]);
        dbg_tip(tt, h, rw_lv, "The watched addresses and their values. A value turns red for a moment when it changes.");
      }
      rw_colors();
      rw_autoload();
      rw_layout();
      rw_rebuild(-1);
      SetTimer(h, RW_TIMER, RW_TICK_MS, NULL);
      return 0;
    }

    case WM_SIZE:
      rw_layout();
      return 0;

    case WM_GETMINMAXINFO:
    {
      MINMAXINFO *mi = (MINMAXINFO *)lp;

      mi->ptMinTrackSize.x = S(440);
      mi->ptMinTrackSize.y = S(300);
      return 0;
    }

    case WM_DPICHANGED:
    {
      const RECT *r = (const RECT *)lp;

      rw_fonts();
      SendMessage(rw_lv, WM_SETFONT, (WPARAM)rw_mono, TRUE);
      for (i = 0; i < IDC_RW_BTNS; i++) SendMessage(rw_btn[i], WM_SETFONT, (WPARAM)rw_ui, TRUE);
      SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      rw_layout();
      return 0;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
    {
      HBRUSH b = theme_ctlcolor_ctl((HDC)wp, (HWND)lp);

      if (b) return (LRESULT)b;
      break;
    }

    case WM_ERASEBKGND:
    {
      RECT rc;
      HDC dc = (HDC)wp;
      HDC probe = GetDC(NULL);
      HBRUSH b = theme_ctlcolor(probe), own = NULL;

      GetClientRect(h, &rc);
      if (b) FillRect(dc, &rc, b); else { own = GetSysColorBrush(COLOR_BTNFACE); FillRect(dc, &rc, own); }
      ReleaseDC(NULL, probe);
      return 1;
    }

    case WM_TIMER:
      if (wp == RW_TIMER && IsWindowVisible(h))
      {
        if (dbg_look() != rw_look)           /* the text size, the style or the scale changed (the Large interface was switched on or off ...) */
        {
          rw_look = dbg_look();
          rw_fonts();
          SendMessage(rw_lv, WM_SETFONT, (WPARAM)rw_mono, TRUE);
          for (i = 0; i < IDC_RW_BTNS; i++) SendMessage(rw_btn[i], WM_SETFONT, (WPARAM)rw_ui, TRUE);
          rw_layout();
        }
        if (lstrcmpA(rw_game, emu_rom_path())) { rw_autoload(); rw_rebuild(-1); }
        rw_tick();
      }
      return 0;

    case WM_NOTIFY:
    {
      NMHDR *hdr = (NMHDR *)lp;
      LRESULT hr = theme_header_customdraw(hdr);

      if (hr != -1) return hr;
      if (hdr->idFrom == IDC_RW_LIST)
      {
        if (hdr->code == NM_DBLCLK) { int sel = rw_selected(); if (sel >= 0) rw_new_watch(0); return 0; }
        if (hdr->code == NM_CUSTOMDRAW)
        {
          NMLVCUSTOMDRAW *cd = (NMLVCUSTOMDRAW *)lp;

          if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
          if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) return CDRF_NOTIFYSUBITEMDRAW;
          if (cd->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM))
          {
            int row = (int)cd->nmcd.dwItemSpec;

            if (cd->iSubItem == 1 && row >= 0 && row < rw_count && rw_list[row].age) cd->clrText = theme_is_dark() ? RGB(255, 110, 110) : RGB(200, 0, 0);
            return CDRF_NEWFONT;
          }
        }
      }
      break;
    }

    case WM_COMMAND:
      switch (LOWORD(wp))
      {
        case IDC_RW_NEWLIST:
          if (rw_count && MessageBoxA(h, "Remove every watch from this list?", "RAM Watch", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
          rw_count = 0;
          rw_rebuild(-1);
          return 0;
        case IDC_RW_OPEN:   rw_file_open(0); return 0;
        case IDC_RW_APPEND: rw_file_open(1); return 0;
        case IDC_RW_SAVE:   rw_file_save(); return 0;
        case IDC_RW_NEW:    rw_new_watch(1); return 0;
        case IDC_RW_EDIT:   rw_new_watch(0); return 0;
        case IDC_RW_REMOVE:
        {
          int sel = rw_selected();

          if (sel < 0) { gui_notify("Select a watch to remove it"); return 0; }
          rw_remove(sel);
          rw_rebuild(sel < rw_count ? sel : rw_count - 1);
          return 0;
        }
        case IDC_RW_DUP:
        {
          int sel = rw_selected();

          if (sel < 0 || rw_list[sel].size == 'S') { gui_notify("Select a watch to duplicate it"); return 0; }
          {
            watch_t d = rw_list[sel];

            d.addr += (unsigned)rw_nbytes(&d);           /* the next one in memory: the same watch twice would be skipped */
            if (rw_insert(&d, sel + 1)) { rw_rebuild(sel + 1); }
          }
          return 0;
        }
        case IDC_RW_SEP:
        {
          watch_t s;
          int sel = rw_selected();

          memset(&s, 0, sizeof(s));
          s.size = 'S'; s.type = 'S';
          if (rw_insert(&s, sel >= 0 ? sel + 1 : -1)) { rw_rebuild(sel >= 0 ? sel + 1 : rw_count - 1); }
          return 0;
        }
        case IDC_RW_UP:   rw_move(-1); return 0;
        case IDC_RW_DOWN: rw_move(1); return 0;
      }
      break;

    case WM_KEYDOWN:
      if (wp == VK_ESCAPE) gui_debug_close(h);
      return 0;

    case WM_CLOSE:
      DestroyWindow(h);
      return 0;

    case WM_DESTROY:
      KillTimer(h, RW_TIMER);
      rw_win = NULL;
      return 0;
  }

  return DefWindowProc(h, m, wp, lp);
}

/****************************************************************************
 * The rest of the program
 ****************************************************************************/

void ramwatch_open(HWND parent)
{
  static int registered;

  if (rw_win)
  {
    HWND top = GetAncestor(rw_win, GA_ROOT);

    if (IsIconic(top)) ShowWindow(top, SW_RESTORE);
    SetForegroundWindow(top);
    return;
  }
  if (!registered)
  {
    WNDCLASSA wc;

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = rw_proc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = RW_CLASS; i18n_keep_ltr(RW_CLASS);
    if (!RegisterClassA(&wc)) return;
    registered = 1;
  }
  rw_fonts();
  if (gui_debug_host)                                       /* a pane of the Debugger window */
  {
    CreateWindowExA(0, RW_CLASS, "RAM Watch", WS_CHILD | WS_CLIPCHILDREN, 0, 0, S(620), S(430), gui_debug_host, NULL, g_inst, NULL);
    gui_debug_last = rw_win;
    return;
  }
  CreateWindowExA(0, RW_CLASS, "RAM Watch", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, S(620), S(430), parent, NULL, g_inst, NULL);
  if (rw_win)
  {
    gui_center_window(rw_win);
    gui_window_icon(rw_win);
    ShowWindow(rw_win, SW_SHOW);
  }
}

/* A watch from the RAM Search: the address (as the 68000 sees it), the size in bytes and the type (0 signed, 1 unsigned, 2 hex). */
void ramwatch_add(unsigned addr, int nbytes, int type)
{
  watch_t w;

  memset(&w, 0, sizeof(w));
  w.addr = addr & 0xFFFFFF;
  w.size = nbytes == 4 ? 'd' : nbytes == 2 ? 'w' : 'b';
  w.type = type == 0 ? 's' : type == 1 ? 'u' : 'h';
  if (!rw_win) debugger_ensure(DBGPANE_WATCH);
  if (!rw_win) return;
  if (rw_insert(&w, -1)) { rw_rebuild(rw_count - 1); }
}

int ramwatch_message(MSG *msg)
{
  if (!rw_win || !msg->hwnd) return 0;
  if (msg->hwnd != rw_win && !IsChild(rw_win, msg->hwnd)) return 0;
  if (IsDialogMessage(rw_win, msg)) return 1;
  TranslateMessage(msg);
  DispatchMessage(msg);
  return 1;
}

void ramwatch_close(void)
{
  if (rw_win) DestroyWindow(rw_win);
  rw_game[0] = '\0';
  rw_count = 0;
}

void ramwatch_enable(int on)
{
  if (rw_win) EnableWindow(rw_win, on ? TRUE : FALSE);
}
