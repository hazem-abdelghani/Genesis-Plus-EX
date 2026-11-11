/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  ramsearch.c -- Tools > Debug > RAM Search: finds the addresses of the game's
 *  RAM whose values behave in a way you describe (they went up, they are the
 *  same as before, they changed by 4 ...), narrowing the list down each time you
 *  search again, and sends the ones you pick to the RAM Watch. It follows the
 *  RAM Search of Gens (sizes of 1, 2 and 4 bytes, signed, unsigned and hex
 *  values, comparisons with the previous value, a value, an address or the
 *  number of changes, Undo, Eliminate, Autosearch). Tools > Cheat Search is the
 *  simpler search that makes a cheat.
 *
 *  The idea of such a window comes from the Genesis Plus GX debugger project
 *  (smd_dgx) and from Gens; this code is written for Win32.
 ****************************************************************************/

#include <windows.h>
#include <commctrl.h>

#include "shared.h"
#include "gui.h"
#include "theme.h"
#include "resource.h"
#include "picocore.h"

#define RS_CLASS  "GPEXRamSearch"
#define RS_TIMER  1
#define RS_MAXMEM 0x40000

enum
{
  IDC_LIST = 101, IDC_MEM, IDC_COUNT,
  IDC_S1 = 110, IDC_S2, IDC_S4, IDC_MIS,
  IDC_TSIGN = 120, IDC_TUNS, IDC_THEX,
  IDC_CPREV = 130, IDC_CVAL, IDC_CADDR, IDC_CCHG, IDC_EVAL, IDC_EADDR, IDC_ECHG,
  IDC_OLT = 140, IDC_OGT, IDC_OLE, IDC_OGE, IDC_OEQ, IDC_ONE, IDC_ODIFF, IDC_OMOD, IDC_EDIFF, IDC_EMOD,
  IDC_SEARCH = 160, IDC_RESET, IDC_CLEARCH, IDC_UNDO, IDC_ELIM, IDC_ADDW, IDC_AUTO
};

typedef struct
{
  unsigned char active[RS_MAXMEM];
  unsigned prev[RS_MAXMEM];
  unsigned short chg[RS_MAXMEM];
} snapshot_t;

static snapshot_t rs_now, rs_undo;
static int rs_have_undo;
static int *rs_res, rs_nres;             /* the addresses still in the list (offsets in the memory) */
static int rs_kind, rs_size_n;           /* the memory searched, and its size in bytes */
static HWND rs_win, rs_list;
static HFONT rs_ui, rs_mono;
static UINT_PTR rs_look;
static char rs_game[GUI_PATH_LEN];
static int rs_kinds[3], rs_nkinds;

static int S(int px)
{
  return dbg_scale(px);
}

static HWND C(int id)
{
  return GetDlgItem(rs_win, id);
}

static int checked(int id)
{
  return SendMessage(C(id), BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static int rs_md(void)
{
  return pico_active() || (system_hw & SYSTEM_PBC) == SYSTEM_MD;
}

static unsigned rs_base(void)
{
  return rs_kind == MV_PSDRAM ? 0x06000000u : rs_kind == MV_ZRAM ? 0xA00000u : rs_md() ? 0xFF0000u : 0xC000u;
}

static int rs_nbytes(void)
{
  return checked(IDC_S4) ? 4 : checked(IDC_S2) ? 2 : 1;
}

static int rs_type(void)               /* 0 signed, 1 unsigned, 2 hex */
{
  return checked(IDC_THEX) ? 2 : checked(IDC_TUNS) ? 1 : 0;
}

/* A value of the memory at an offset (big-endian on the Mega Drive, little-endian on the 8-bit consoles); 0 outside it. */
static unsigned rs_read(int off, int n)
{
  unsigned v = 0;
  int i;

  for (i = 0; i < n; i++)
  {
    int b = dbgmem_get(rs_kind, off + i);

    if (b < 0) b = 0;
    if (rs_md()) v = (v << 8) | (unsigned)b; else v |= (unsigned)b << (8 * i);
  }
  return v;
}

/* The value as the comparison sees it: signed or unsigned, of the size searched. */
static long long rs_val(unsigned v, int n, int type)
{
  if (type == 0) return n == 1 ? (long long)(signed char)v : n == 2 ? (long long)(short)v : (long long)(int)v;
  return n == 1 ? (long long)(v & 0xFF) : n == 2 ? (long long)(v & 0xFFFF) : (long long)v;
}

static void rs_text(unsigned v, int n, int type, char *out, int len)
{
  if (type == 2) gui_snprintf(out, len, n == 1 ? "%02X" : n == 2 ? "%04X" : "%08X", n == 1 ? (v & 0xFF) : n == 2 ? (v & 0xFFFF) : v);
  else if (type == 0) gui_snprintf(out, len, "%d", (int)rs_val(v, n, 0));
  else gui_snprintf(out, len, "%u", (unsigned)rs_val(v, n, 1));
}

static long long rs_parse(int id, int n, int type)
{
  char t[40];
  const char *s;
  int neg = 0, hex = (type == 2);
  unsigned long long v;

  GetWindowTextA(C(id), t, sizeof(t));
  s = t;
  if (*s == '-') { neg = 1; s++; }
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { hex = 1; s += 2; }
  v = strtoull(s, NULL, hex ? 16 : 10);
  if (neg) v = (unsigned long long)(-(long long)v);
  return type == 0 ? rs_val((unsigned)v, n, 0) : rs_val((unsigned)v, n, 1);
}

static void rs_status(void)
{
  char t[80];

  gui_snprintf(t, sizeof(t), "%d result%s", rs_nres, rs_nres == 1 ? "" : "s");
  SetWindowTextA(C(IDC_COUNT), t);
}

static void rs_rebuild_results(void)
{
  int i;

  rs_nres = 0;
  for (i = 0; i < rs_size_n; i++) if (rs_now.active[i]) rs_res[rs_nres++] = i;
  ListView_SetItemCountEx(rs_list, rs_nres, LVSICF_NOINVALIDATEALL);
  InvalidateRect(rs_list, NULL, TRUE);
  rs_status();
}

/* Every address can be the start of a value; unless Misaligned is ticked a word or long starts on an even address. */
static int rs_ok_addr(int off, int n)
{
  if (off + n > rs_size_n) return 0;
  if (n > 1 && !checked(IDC_MIS) && (off & 1)) return 0;
  return 1;
}

static void rs_save_undo(void)
{
  memcpy(&rs_undo, &rs_now, sizeof(rs_now));
  rs_have_undo = 1;
  EnableWindow(C(IDC_UNDO), TRUE);
}

static void rs_reset(void)
{
  int i, n = rs_nbytes();

  rs_save_undo();
  memset(&rs_now, 0, sizeof(rs_now));
  for (i = 0; i < rs_size_n; i++)
    if (rs_ok_addr(i, n)) { rs_now.active[i] = 1; rs_now.prev[i] = rs_read(i, n); }
  rs_rebuild_results();
}

static void rs_search(void)
{
  int n = rs_nbytes(), type = rs_type(), i, cmp_to, op;
  long long rhs_fixed = 0, x = 0, nn = 0;
  int addr_off = 0;

  rs_save_undo();
  cmp_to = checked(IDC_CPREV) ? 0 : checked(IDC_CVAL) ? 1 : checked(IDC_CADDR) ? 2 : 3;
  op = checked(IDC_OLT) ? 0 : checked(IDC_OGT) ? 1 : checked(IDC_OLE) ? 2 : checked(IDC_OGE) ? 3 : checked(IDC_OEQ) ? 4 : checked(IDC_ONE) ? 5 : checked(IDC_ODIFF) ? 6 : 7;
  if (cmp_to == 1) rhs_fixed = rs_parse(IDC_EVAL, n, type);
  if (cmp_to == 3) rhs_fixed = rs_parse(IDC_ECHG, 2, 1);
  if (cmp_to == 2)
  {
    char t[40];
    unsigned a;

    GetWindowTextA(C(IDC_EADDR), t, sizeof(t));
    a = (unsigned)strtoul(t, NULL, 16);
    addr_off = (int)(a >= rs_base() ? a - rs_base() : a);
  }
  if (op == 6) nn = rs_parse(IDC_EDIFF, n, 1);
  if (op == 7) { nn = rs_parse(IDC_EMOD, n, 1); x = (cmp_to == 1 || cmp_to == 3) ? rhs_fixed : 0; }

  for (i = 0; i < rs_size_n; i++)
  {
    unsigned curu;
    long long cur, rhs;
    int keep = 0;

    if (!rs_now.active[i]) continue;
    curu = rs_read(i, n);
    cur = rs_val(curu, n, type);
    if (cmp_to == 0) rhs = rs_val(rs_now.prev[i], n, type);
    else if (cmp_to == 1 || cmp_to == 3) rhs = (cmp_to == 3) ? (long long)rs_now.chg[i] : rhs_fixed;
    else rhs = rs_val(rs_read(addr_off, n), n, type);
    if (cmp_to == 3) { cur = (long long)rs_now.chg[i]; rhs = rhs_fixed; }

    switch (op)
    {
      case 0: keep = cur < rhs; break;
      case 1: keep = cur > rhs; break;
      case 2: keep = cur <= rhs; break;
      case 3: keep = cur >= rhs; break;
      case 4: keep = cur == rhs; break;
      case 5: keep = cur != rhs; break;
      case 6: { long long d = cur - rhs; keep = (d < 0 ? -d : d) == nn; break; }
      default: keep = nn > 0 && (cur % nn) == x; break;
    }
    if (!keep) { rs_now.active[i] = 0; continue; }
    if (curu != rs_now.prev[i] && rs_now.chg[i] < 0xFFFF) rs_now.chg[i]++;
    rs_now.prev[i] = curu;
  }
  rs_rebuild_results();
}

static void rs_clear_changes(void)
{
  rs_save_undo();
  memset(rs_now.chg, 0, sizeof(rs_now.chg));
  InvalidateRect(rs_list, NULL, TRUE);
}

static void rs_undo_do(void)
{
  if (!rs_have_undo) return;
  memcpy(&rs_now, &rs_undo, sizeof(rs_now));
  rs_have_undo = 0;
  EnableWindow(C(IDC_UNDO), FALSE);
  rs_rebuild_results();
}

static void rs_eliminate(void)
{
  int i = -1;

  rs_save_undo();
  while ((i = ListView_GetNextItem(rs_list, i, LVNI_SELECTED)) >= 0)
    if (i < rs_nres) rs_now.active[rs_res[i]] = 0;
  rs_rebuild_results();
}

static void rs_add_watch(void)
{
  int i = -1, n = rs_nbytes(), count = 0;

  while ((i = ListView_GetNextItem(rs_list, i, LVNI_SELECTED)) >= 0 && count < 64)
    if (i < rs_nres) { ramwatch_add(rs_base() + (unsigned)rs_res[i], n, rs_type()); count++; }
  if (!count) gui_notify("Pick results in the list first");
}

static void rs_fill_memory(void)
{
  int i;

  rs_nkinds = 0;
  rs_kinds[rs_nkinds++] = pico_active() ? MV_PWRAM : MV_WRAM;
  if ((system_hw & SYSTEM_PBC) == SYSTEM_MD && !pico_active()) rs_kinds[rs_nkinds++] = MV_ZRAM;
  if (pico_active() && dbgmem_size(MV_PSDRAM) > 0) rs_kinds[rs_nkinds++] = MV_PSDRAM;
  SendMessage(C(IDC_MEM), CB_RESETCONTENT, 0, 0);
  SendMessageA(C(IDC_MEM), CB_ADDSTRING, 0, (LPARAM)(rs_md() ? "68000 RAM" : "RAM"));
  for (i = 1; i < rs_nkinds; i++)
    SendMessageA(C(IDC_MEM), CB_ADDSTRING, 0, (LPARAM)(rs_kinds[i] == MV_PSDRAM ? "32X SDRAM (SH-2)" : "Z80 RAM"));
  SendMessage(C(IDC_MEM), CB_SETCURSEL, 0, 0);
  rs_kind = rs_kinds[0];
  rs_size_n = dbgmem_size(rs_kind);
  if (rs_size_n > RS_MAXMEM) rs_size_n = RS_MAXMEM;
  lstrcpynA(rs_game, emu_rom_path(), sizeof(rs_game));
}

/****************************************************************************
 * The window
 ****************************************************************************/

static HWND rs_ctl(const char *cls, const char *text, DWORD style, DWORD ex, int id)
{
  HWND c = CreateWindowExA(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, rs_win, (HMENU)(INT_PTR)id, g_inst, NULL);

  if (c) SendMessage(c, WM_SETFONT, (WPARAM)rs_ui, TRUE);
  return c;
}

static void rs_place(int id, int x, int y, int w, int h)
{
  SetWindowPos(C(id), NULL, x, y, w, h, SWP_NOZORDER);
}

static void rs_layout(void)
{
  RECT c;
  int px, pw, y, rh = S(22), g = S(4);

  if (!rs_win) return;
  rs_ui = dbg_ui_font();
  rs_mono = dbg_mono_font();
  GetClientRect(rs_win, &c);
  pw = S(236);
  px = c.right - pw - S(8);
  SendMessage(rs_list, WM_SETFONT, (WPARAM)rs_mono, TRUE);
  SetWindowPos(rs_list, NULL, S(8), S(8), px - S(16), c.bottom - S(40), SWP_NOZORDER);
  rs_place(IDC_COUNT, S(8), c.bottom - S(28), px - S(16), S(20));
  {
    int ids[60], n = 0, i;

    ids[n++] = IDC_MEM; ids[n++] = IDC_S1; ids[n++] = IDC_S2; ids[n++] = IDC_S4; ids[n++] = IDC_MIS; ids[n++] = IDC_TSIGN; ids[n++] = IDC_TUNS; ids[n++] = IDC_THEX;
    ids[n++] = IDC_CPREV; ids[n++] = IDC_CVAL; ids[n++] = IDC_EVAL; ids[n++] = IDC_CADDR; ids[n++] = IDC_EADDR; ids[n++] = IDC_CCHG; ids[n++] = IDC_ECHG;
    ids[n++] = IDC_OLT; ids[n++] = IDC_OGT; ids[n++] = IDC_OLE; ids[n++] = IDC_OGE; ids[n++] = IDC_OEQ; ids[n++] = IDC_ONE; ids[n++] = IDC_ODIFF; ids[n++] = IDC_EDIFF;
    ids[n++] = IDC_OMOD; ids[n++] = IDC_EMOD; ids[n++] = IDC_SEARCH; ids[n++] = IDC_RESET; ids[n++] = IDC_CLEARCH; ids[n++] = IDC_UNDO; ids[n++] = IDC_ELIM; ids[n++] = IDC_ADDW; ids[n++] = IDC_AUTO;
    for (i = 0; i < n; i++) SendMessage(C(ids[i]), WM_SETFONT, (WPARAM)rs_ui, TRUE);
  }
  y = S(8);
  rs_place(IDC_MEM, px, y, pw, S(200)); y += rh + g;
  rs_place(IDC_S1, px, y, S(70), rh); rs_place(IDC_S2, px + S(72), y, S(70), rh); rs_place(IDC_S4, px + S(144), y, S(90), rh); y += rh;
  rs_place(IDC_MIS, px, y, pw, rh); y += rh + g;
  rs_place(IDC_TSIGN, px, y, S(76), rh); rs_place(IDC_TUNS, px + S(78), y, S(86), rh); rs_place(IDC_THEX, px + S(168), y, S(66), rh); y += rh + g * 2;
  rs_place(IDC_CPREV, px, y, pw, rh); y += rh;
  rs_place(IDC_CVAL, px, y, S(124), rh); rs_place(IDC_EVAL, px + S(128), y, S(106), rh); y += rh;
  rs_place(IDC_CADDR, px, y, S(124), rh); rs_place(IDC_EADDR, px + S(128), y, S(106), rh); y += rh;
  rs_place(IDC_CCHG, px, y, S(124), rh); rs_place(IDC_ECHG, px + S(128), y, S(106), rh); y += rh + g * 2;
  rs_place(IDC_OLT, px, y, S(76), rh); rs_place(IDC_OGT, px + S(78), y, S(76), rh); y += rh;
  rs_place(IDC_OLE, px, y, S(76), rh); rs_place(IDC_OGE, px + S(78), y, S(76), rh); y += rh;
  rs_place(IDC_OEQ, px, y, S(76), rh); rs_place(IDC_ONE, px + S(78), y, S(76), rh); y += rh;
  rs_place(IDC_ODIFF, px, y, S(124), rh); rs_place(IDC_EDIFF, px + S(128), y, S(106), rh); y += rh;
  rs_place(IDC_OMOD, px, y, S(124), rh); rs_place(IDC_EMOD, px + S(128), y, S(106), rh); y += rh + g * 2;
  rs_place(IDC_SEARCH, px, y, S(114), S(26)); rs_place(IDC_RESET, px + S(120), y, S(114), S(26)); y += S(30);
  rs_place(IDC_CLEARCH, px, y, pw, S(26)); y += S(30);
  rs_place(IDC_UNDO, px, y, S(114), S(26)); rs_place(IDC_ELIM, px + S(120), y, S(114), S(26)); y += S(30);
  rs_place(IDC_ADDW, px, y, pw, S(26)); y += S(30);
  rs_place(IDC_AUTO, px, y, pw, rh);
  ListView_SetColumnWidth(rs_list, 0, S(100));
  ListView_SetColumnWidth(rs_list, 1, S(110));
  ListView_SetColumnWidth(rs_list, 2, S(110));
  ListView_SetColumnWidth(rs_list, 3, S(70));
}

static void rs_getdispinfo(NMLVDISPINFOA *di)
{
  int row = di->item.iItem;

  if (row < 0 || row >= rs_nres || !(di->item.mask & LVIF_TEXT)) return;
  {
    int off = rs_res[row], n = rs_nbytes(), type = rs_type();
    char t[40];

    t[0] = '\0';
    switch (di->item.iSubItem)
    {
      case 0: gui_snprintf(t, sizeof(t), "%08X", rs_base() + (unsigned)off); break;
      case 1: rs_text(rs_read(off, n), n, type, t, sizeof(t)); break;
      case 2: rs_text(rs_now.prev[off], n, type, t, sizeof(t)); break;
      case 3: gui_snprintf(t, sizeof(t), "%u", (unsigned)rs_now.chg[off]); break;
    }
    lstrcpynA(di->item.pszText, t, di->item.cchTextMax);
  }
}

static LRESULT CALLBACK rs_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
  switch (m)
  {
    case WM_CREATE:
    {
      LVCOLUMNA col;
      static const char *const heads[4] = { "Address", "Value", "Previous", "Changes" };
      int i;

      rs_win = h;
      rs_ui = dbg_ui_font();
      rs_mono = dbg_mono_font();
      rs_look = dbg_look();
      rs_list = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA, 0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_LIST, g_inst, NULL);
      ListView_SetExtendedListViewStyle(rs_list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
      ZeroMemory(&col, sizeof(col));
      col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
      col.fmt = LVCFMT_LEFT; col.cx = S(100);
      for (i = 0; i < 4; i++) { col.pszText = (char *)heads[i]; SendMessageA(rs_list, LVM_INSERTCOLUMNA, (WPARAM)i, (LPARAM)&col); }
      rs_ctl("STATIC", "", SS_LEFT | SS_NOPREFIX, 0, IDC_COUNT);
      rs_ctl("COMBOBOX", "", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, IDC_MEM);
      rs_ctl("BUTTON", "1 Byte", WS_GROUP | WS_TABSTOP | BS_AUTORADIOBUTTON, 0, IDC_S1);
      rs_ctl("BUTTON", "2 Bytes", BS_AUTORADIOBUTTON, 0, IDC_S2);
      rs_ctl("BUTTON", "4 Bytes", BS_AUTORADIOBUTTON, 0, IDC_S4);
      rs_ctl("BUTTON", "Misaligned values", WS_TABSTOP | BS_AUTOCHECKBOX, 0, IDC_MIS);
      rs_ctl("BUTTON", "Signed", WS_GROUP | WS_TABSTOP | BS_AUTORADIOBUTTON, 0, IDC_TSIGN);
      rs_ctl("BUTTON", "Unsigned", BS_AUTORADIOBUTTON, 0, IDC_TUNS);
      rs_ctl("BUTTON", "Hex", BS_AUTORADIOBUTTON, 0, IDC_THEX);
      rs_ctl("BUTTON", "Previous Value", WS_GROUP | WS_TABSTOP | BS_AUTORADIOBUTTON, 0, IDC_CPREV);
      rs_ctl("BUTTON", "Specific Value", BS_AUTORADIOBUTTON, 0, IDC_CVAL);
      rs_ctl("EDIT", "0", WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, IDC_EVAL);
      rs_ctl("BUTTON", "Specific Address", BS_AUTORADIOBUTTON, 0, IDC_CADDR);
      rs_ctl("EDIT", "0", WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, IDC_EADDR);
      rs_ctl("BUTTON", "Number of Changes", BS_AUTORADIOBUTTON, 0, IDC_CCHG);
      rs_ctl("EDIT", "0", WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, IDC_ECHG);
      rs_ctl("BUTTON", "<  Less", WS_GROUP | WS_TABSTOP | BS_AUTORADIOBUTTON, 0, IDC_OLT);
      rs_ctl("BUTTON", ">  Greater", BS_AUTORADIOBUTTON, 0, IDC_OGT);
      rs_ctl("BUTTON", "<=  Or Same", BS_AUTORADIOBUTTON, 0, IDC_OLE);
      rs_ctl("BUTTON", ">=  Or Same", BS_AUTORADIOBUTTON, 0, IDC_OGE);
      rs_ctl("BUTTON", "=  Equal", BS_AUTORADIOBUTTON, 0, IDC_OEQ);
      rs_ctl("BUTTON", "!=  Not Equal", BS_AUTORADIOBUTTON, 0, IDC_ONE);
      rs_ctl("BUTTON", "Different By", BS_AUTORADIOBUTTON, 0, IDC_ODIFF);
      rs_ctl("EDIT", "1", WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, IDC_EDIFF);
      rs_ctl("BUTTON", "Modulo", BS_AUTORADIOBUTTON, 0, IDC_OMOD);
      rs_ctl("EDIT", "2", WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, IDC_EMOD);
      rs_ctl("BUTTON", "Search", WS_TABSTOP | BS_DEFPUSHBUTTON, 0, IDC_SEARCH);
      rs_ctl("BUTTON", "Reset", WS_TABSTOP | BS_PUSHBUTTON, 0, IDC_RESET);
      rs_ctl("BUTTON", "Clear Change Counts", WS_TABSTOP | BS_PUSHBUTTON, 0, IDC_CLEARCH);
      rs_ctl("BUTTON", "Undo", WS_TABSTOP | BS_PUSHBUTTON, 0, IDC_UNDO);
      rs_ctl("BUTTON", "Eliminate", WS_TABSTOP | BS_PUSHBUTTON, 0, IDC_ELIM);
      rs_ctl("BUTTON", "Add Watch", WS_TABSTOP | BS_PUSHBUTTON, 0, IDC_ADDW);
      rs_ctl("BUTTON", "Autosearch", WS_TABSTOP | BS_AUTOCHECKBOX, 0, IDC_AUTO);
      {
        static const GuiTip tips[] =
        {
          { IDC_LIST, NULL, "The addresses still in the search, with their value now, the value at the last search and how often it changed. Select some to add them to the RAM Watch or to drop them." },
          { IDC_MEM, NULL, "Which memory to search." },
          { IDC_COUNT, NULL, "How many addresses are left." },
          { IDC_S1, NULL, "Look at values of one byte." }, { IDC_S2, NULL, "Look at values of two bytes (a word)." }, { IDC_S4, NULL, "Look at values of four bytes (a long)." },
          { IDC_MIS, NULL, "Also look at odd addresses for words and longs (the 68000 cannot use them, so leave this off unless a game stores such values)." },
          { IDC_TSIGN, NULL, "Compare the values as signed numbers." }, { IDC_TUNS, NULL, "Compare the values as unsigned numbers." }, { IDC_THEX, NULL, "Show the values in hex (compared as unsigned)." },
          { IDC_CPREV, NULL, "Compare each value with what it was at the last search." },
          { IDC_CVAL, NULL, "Compare each value with the number typed beside it." }, { IDC_EVAL, NULL, "The number to compare with." },
          { IDC_CADDR, NULL, "Keep only the address typed beside it." }, { IDC_EADDR, NULL, "The address, in hex." },
          { IDC_CCHG, NULL, "Compare how many times each value has changed with the number typed beside it." }, { IDC_ECHG, NULL, "The number of changes." },
          { IDC_OLT, NULL, "Keep values that are less than what they are compared with." }, { IDC_OGT, NULL, "Keep values that are greater." },
          { IDC_OLE, NULL, "Keep values that are less than or the same." }, { IDC_OGE, NULL, "Keep values that are greater than or the same." },
          { IDC_OEQ, NULL, "Keep values that are equal." }, { IDC_ONE, NULL, "Keep values that are not equal." },
          { IDC_ODIFF, NULL, "Keep values that differ from the previous one by exactly the number typed beside it." }, { IDC_EDIFF, NULL, "The difference." },
          { IDC_OMOD, NULL, "Keep values that leave no remainder when divided by the number typed beside it." }, { IDC_EMOD, NULL, "The divisor." },
          { IDC_SEARCH, NULL, "Narrow the list with the comparison chosen. Press it again and again as the game changes." },
          { IDC_RESET, NULL, "Start over with every address of the memory." },
          { IDC_CLEARCH, NULL, "Set the change counts back to zero." },
          { IDC_UNDO, NULL, "Take back the last search." },
          { IDC_ELIM, NULL, "Drop the picked addresses from the list." },
          { IDC_ADDW, NULL, "Send the picked addresses to the RAM Watch." },
          { IDC_AUTO, NULL, "Search at every moment while the game runs." }
        };

        gui_dialog_tips(h, tips, (int)(sizeof(tips) / sizeof(tips[0])));
      }
      SendMessage(C(IDC_S1), BM_SETCHECK, BST_CHECKED, 0);
      SendMessage(C(IDC_TUNS), BM_SETCHECK, BST_CHECKED, 0);
      SendMessage(C(IDC_CPREV), BM_SETCHECK, BST_CHECKED, 0);
      SendMessage(C(IDC_OEQ), BM_SETCHECK, BST_CHECKED, 0);
      EnableWindow(C(IDC_UNDO), FALSE);
      theme_apply_to_window(h);
      {
        HDC probe = GetDC(NULL);
        HBRUSH b = theme_ctlcolor(probe);
        COLORREF bg = b ? GetBkColor(probe) : GetSysColor(COLOR_WINDOW), fg = b ? GetTextColor(probe) : GetSysColor(COLOR_WINDOWTEXT);

        ReleaseDC(NULL, probe);
        ListView_SetBkColor(rs_list, bg); ListView_SetTextBkColor(rs_list, bg); ListView_SetTextColor(rs_list, fg);
      }
      rs_fill_memory();
      rs_res = (int *)malloc(sizeof(int) * RS_MAXMEM);
      rs_layout();
      rs_reset();
      SetTimer(h, RS_TIMER, 150, NULL);
      return 0;
    }

    case WM_SIZE:
      rs_layout();
      return 0;

    case WM_GETMINMAXINFO:
    {
      MINMAXINFO *mi = (MINMAXINFO *)lp;

      mi->ptMinTrackSize.x = S(660);
      mi->ptMinTrackSize.y = S(640);
      return 0;
    }

    case WM_ERASEBKGND:
    {
      RECT rc;
      HDC probe = GetDC(NULL);
      HBRUSH b = theme_ctlcolor(probe);

      GetClientRect(h, &rc);
      FillRect((HDC)wp, &rc, b ? b : GetSysColorBrush(COLOR_BTNFACE));
      ReleaseDC(NULL, probe);
      return 1;
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

    case WM_TIMER:
      if (wp == RS_TIMER)
      {
        if (dbg_look() != rs_look) { rs_look = dbg_look(); rs_layout(); }
        if (lstrcmpA(rs_game, emu_rom_path())) { rs_fill_memory(); rs_reset(); }
        else if (checked(IDC_AUTO)) rs_search();
        else InvalidateRect(rs_list, NULL, FALSE);
      }
      return 0;

    case WM_NOTIFY:
    {
      NMHDR *hdr = (NMHDR *)lp;
      LRESULT hr = theme_header_customdraw(hdr);

      if (hr != -1) return hr;
      if (hdr->idFrom == IDC_LIST && hdr->code == LVN_GETDISPINFOA) rs_getdispinfo((NMLVDISPINFOA *)lp);
      break;
    }

    case WM_COMMAND:
      switch (LOWORD(wp))
      {
        case IDC_SEARCH:  rs_search(); return 0;
        case IDC_RESET:   rs_reset(); return 0;
        case IDC_CLEARCH: rs_clear_changes(); return 0;
        case IDC_UNDO:    rs_undo_do(); return 0;
        case IDC_ELIM:    rs_eliminate(); return 0;
        case IDC_ADDW:    rs_add_watch(); return 0;
        case IDC_MEM:
          if (HIWORD(wp) == CBN_SELCHANGE)
          {
            int k = (int)SendMessage(C(IDC_MEM), CB_GETCURSEL, 0, 0);

            if (k >= 0 && k < rs_nkinds) { rs_kind = rs_kinds[k]; rs_size_n = dbgmem_size(rs_kind); if (rs_size_n > RS_MAXMEM) rs_size_n = RS_MAXMEM; rs_reset(); }
          }
          return 0;
        case IDC_S1: case IDC_S2: case IDC_S4: case IDC_MIS:
          if (HIWORD(wp) == BN_CLICKED) rs_reset();                           /* a different size: a new search */
          return 0;
        case IDC_TSIGN: case IDC_TUNS: case IDC_THEX:
          if (HIWORD(wp) == BN_CLICKED) InvalidateRect(rs_list, NULL, TRUE);
          return 0;
      }
      break;

    case WM_KEYDOWN:
      if (wp == VK_ESCAPE) gui_debug_close(h);
      return 0;

    case WM_CLOSE:
      DestroyWindow(h);
      return 0;

    case WM_DESTROY:
      KillTimer(h, RS_TIMER);
      free(rs_res);
      rs_res = NULL;
      rs_nres = 0;
      rs_win = NULL;
      return 0;
  }

  return DefWindowProc(h, m, wp, lp);
}

/****************************************************************************
 * The rest of the program
 ****************************************************************************/

void ramsearch_open(HWND parent)
{
  static int registered;

  if (rs_win)
  {
    HWND top = GetAncestor(rs_win, GA_ROOT);

    if (IsIconic(top)) ShowWindow(top, SW_RESTORE);
    SetForegroundWindow(top);
    return;
  }
  if (!registered)
  {
    WNDCLASSA wc;

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = rs_proc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = RS_CLASS; i18n_keep_ltr(RS_CLASS);
    if (!RegisterClassA(&wc)) return;
    registered = 1;
  }
  if (gui_debug_host)                                       /* a pane of the Debugger window */
  {
    CreateWindowExA(0, RS_CLASS, "RAM Search", WS_CHILD | WS_CLIPCHILDREN, 0, 0, S(760), S(660), gui_debug_host, NULL, g_inst, NULL);
    gui_debug_last = rs_win;
    return;
  }
  CreateWindowExA(0, RS_CLASS, "RAM Search", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, S(760), S(660), parent, NULL, g_inst, NULL);
  if (rs_win)
  {
    gui_center_window(rs_win);
    gui_window_icon(rs_win);
    ShowWindow(rs_win, SW_SHOW);
  }
}

int ramsearch_message(MSG *msg)
{
  if (!rs_win || !msg->hwnd) return 0;
  if (msg->hwnd != rs_win && !IsChild(rs_win, msg->hwnd)) return 0;
  if (IsDialogMessage(rs_win, msg)) return 1;
  TranslateMessage(msg);
  DispatchMessage(msg);
  return 1;
}

void ramsearch_close(void)
{
  if (rs_win) DestroyWindow(rs_win);
}

void ramsearch_enable(int on)
{
  if (rs_win) EnableWindow(rs_win, on ? TRUE : FALSE);
}
