/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  hotkeys.c -- the keyboard shortcuts: their defaults, the table Windows
 *  reads them from (rebuilt when they change), the text shown next to menu
 *  items, and the Input > Hotkeys window where they are changed.
 *
 *  A shortcut is stored as one int: the virtual-key code, plus 0x10000 for
 *  Ctrl, 0x20000 for Shift and 0x40000 for Alt. 0 means none.
 ****************************************************************************/

#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <stdio.h>
#include <string.h>

#include "shared.h"
#include "gui.h"
#include "resource.h"
#include "theme.h"
#include "toolbar.h"

#define K_CTRL  0x10000
#define K_SHIFT 0x20000
#define K_ALT   0x40000
#define K_VK(c) ((c) & 0xFFFF)

#define HK_FRAMEADV 2599   /* not a menu command: read by the input code while paused */

typedef struct
{
  const char *name;
  int cmd;
  int def;
} hk_t;

static const hk_t hk[] =
{
  { "Keyboard Shortcuts",             IDM_HELP_SHORTCUTS,          VK_F1 },
  { "Open ROM",                       IDM_FILE_OPEN,               K_CTRL | 'O' },
  { "ROMs Directory",                 IDM_FILE_BROWSER,            K_CTRL | 'R' },
  { "Pause and Resume",               IDM_EMU_PAUSE,               VK_F2 },
  { "Pause and Resume (second key)",  IDM_EMU_PAUSE,               VK_PAUSE },
  { "Stop",                           IDM_EMU_STOP,                VK_F3 },
  { "Reset",                          IDM_EMU_RESET,               VK_F4 },
  { "Hard Reset",                     IDM_EMU_HARDRESET,           K_CTRL | VK_F4 },
  { "Load State (current slot)",      IDM_ACCEL_QUICKLOAD,         VK_F8 },
  { "Load State from Slot 0",          IDM_FILE_LOADSLOT_BASE + 0,  '0' },
  { "Load State from Slot 1",          IDM_FILE_LOADSLOT_BASE + 1,  '1' },
  { "Load State from Slot 2",          IDM_FILE_LOADSLOT_BASE + 2,  '2' },
  { "Load State from Slot 3",          IDM_FILE_LOADSLOT_BASE + 3,  '3' },
  { "Load State from Slot 4",          IDM_FILE_LOADSLOT_BASE + 4,  '4' },
  { "Load State from Slot 5",          IDM_FILE_LOADSLOT_BASE + 5,  '5' },
  { "Load State from Slot 6",          IDM_FILE_LOADSLOT_BASE + 6,  '6' },
  { "Load State from Slot 7",          IDM_FILE_LOADSLOT_BASE + 7,  '7' },
  { "Load State from Slot 8",          IDM_FILE_LOADSLOT_BASE + 8,  '8' },
  { "Load State from Slot 9",          IDM_FILE_LOADSLOT_BASE + 9,  '9' },
  { "Undo Load State",                IDM_EMU_UNDOLOAD,            K_CTRL | VK_F8 },
  { "Save State (current slot)",      IDM_ACCEL_QUICKSAVE,         VK_F5 },
  { "Save State to Slot 0",            IDM_FILE_SAVESLOT_BASE + 0,  K_SHIFT | '0' },
  { "Save State to Slot 1",            IDM_FILE_SAVESLOT_BASE + 1,  K_SHIFT | '1' },
  { "Save State to Slot 2",            IDM_FILE_SAVESLOT_BASE + 2,  K_SHIFT | '2' },
  { "Save State to Slot 3",            IDM_FILE_SAVESLOT_BASE + 3,  K_SHIFT | '3' },
  { "Save State to Slot 4",            IDM_FILE_SAVESLOT_BASE + 4,  K_SHIFT | '4' },
  { "Save State to Slot 5",            IDM_FILE_SAVESLOT_BASE + 5,  K_SHIFT | '5' },
  { "Save State to Slot 6",            IDM_FILE_SAVESLOT_BASE + 6,  K_SHIFT | '6' },
  { "Save State to Slot 7",            IDM_FILE_SAVESLOT_BASE + 7,  K_SHIFT | '7' },
  { "Save State to Slot 8",            IDM_FILE_SAVESLOT_BASE + 8,  K_SHIFT | '8' },
  { "Save State to Slot 9",            IDM_FILE_SAVESLOT_BASE + 9,  K_SHIFT | '9' },
  { "Undo Save State",                IDM_EMU_UNDOSAVE,            K_CTRL | VK_F5 },
  { "Previous Slot",                  IDM_ACCEL_SLOT_PREV,         VK_F6 },
  { "Next Slot",                      IDM_ACCEL_SLOT_NEXT,         VK_F7 },
  { "Advance One Frame While Paused", HK_FRAMEADV,                 VK_OEM_5 },
  { "Fast Forward (hold)",            HK_FASTFWD,                  VK_TAB },
  { "Pause Menu",                     IDM_INPUT_PAUSEMENU,         0 },
  { "Game Speed: Faster",             IDM_EMU_SPEEDUP,             K_CTRL | VK_OEM_PLUS },
  { "Game Speed: Slower",             IDM_EMU_SPEEDDOWN,           K_CTRL | VK_OEM_MINUS },
  { "Game Speed: Normal",             IDM_EMU_SPEEDRESET,          K_CTRL | '0' },
  { "Rewind (hold)",                  HK_REWIND,                   VK_BACK },
  { "Show / Hide the Search Bar",     IDM_VIEW_FINDSEARCH,         K_CTRL | 'F' },
  { "Fullscreen",                     IDM_VIDEO_FULLSCREEN,        K_ALT | VK_RETURN },
  { "Leave Fullscreen",               IDM_ACCEL_LEAVE_FULLSCREEN,  VK_ESCAPE },
  { "Mute",                           IDM_AUDIO_ENABLE,            K_CTRL | 'M' },
  { "Mute FM Sound",                 IDM_AUDIO_MUTE_FM,           K_CTRL | 'N' },
  { "Mute PSG Sound",                IDM_AUDIO_MUTE_PSG,          K_CTRL | 'B' },
  { "Console Model: Default",         IDM_SET_MODEL_BASE + 0,      K_CTRL | K_SHIFT | '3' },
  { "Console Model: Genesis Model 1", IDM_SET_MODEL_BASE + 1,      K_CTRL | K_SHIFT | '1' },
  { "Console Model: Genesis Model 2", IDM_SET_MODEL_BASE + 2,      K_CTRL | K_SHIFT | '2' },
  { "Cheats",                         IDM_EMU_CHEATS,              K_CTRL | 'C' },
  { "Record Animated GIF",            IDM_FILE_REC_GIF,            K_CTRL | VK_F12 },
  { "Record Video + Audio",           IDM_FILE_REC_VIDEO,          K_SHIFT | VK_F11 },
  { "Record Audio Only",              IDM_FILE_REC_AUDIO,          K_CTRL | VK_F11 },
  { "Stop Recording",                 IDM_FILE_REC_STOP,           K_ALT | VK_F11 },
  { "Save a Screenshot",              IDM_FILE_SCREENSHOT,         VK_F12 },
};
#define HK_N ((int)(sizeof(hk) / sizeof(hk[0])))
typedef char hotkey_table_fits_gui_hotkey[(sizeof(hk) / sizeof(hk[0])) <= 96 ? 1 : -1];   /* gui.hotkey[] holds 96 */

int hotkeys_count(void) { return HK_N; }
const char *hotkey_name(int i) { return hk[i].name; }
int hotkey_default(int i) { return hk[i].def; }

/* The setting's name in the ini file: the command number, and which of its keys this is. */
void hotkey_ini_key(int i, char *out, int len)
{
  int n = 0, j;
  for (j = 0; j < i; j++) if (hk[j].cmd == hk[i].cmd) n++;
  wsprintfA(out, "cmd%d_%d", hk[i].cmd, n);
  (void)len;
}

/* The key (without Ctrl/Shift/Alt) of "Advance One Frame While Paused". */
int hotkey_frame_advance_vk(void)
{
  int i;
  for (i = 0; i < HK_N; i++)
    if (hk[i].cmd == HK_FRAMEADV) return K_VK(gui.hotkey[i]);
  return 0;
}

/* The key held for Fast Forward or Rewind (modifiers do not count). */
int hotkey_hold_vk(int cmd)
{
  int i;
  for (i = 0; i < HK_N; i++)
    if (hk[i].cmd == cmd) return K_VK(gui.hotkey[i]);
  return 0;
}

/* The shortcut that is this key on its own (no Ctrl / Shift / Alt), or NULL. */
const char *hotkey_owner_of_key(int vk)
{
  int i;
  for (i = 0; i < HK_N; i++)
    if (gui.hotkey[i] == vk) return hk[i].name;
  return NULL;
}

/* "Ctrl+Shift+R", "F5", "Backspace" ... */
void hotkey_text(int code, char *out, int len)
{
  char key[64];
  int vk = K_VK(code);

  out[0] = '\0';
  if (!vk) return;

  if (code & K_CTRL)  lstrcatA(out, "Ctrl+");
  if (code & K_SHIFT) lstrcatA(out, "Shift+");
  if (code & K_ALT)   lstrcatA(out, "Alt+");

  key[0] = '\0';
  if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { key[0] = (char)vk; key[1] = '\0'; }
  else if (vk >= VK_F1 && vk <= VK_F24) wsprintfA(key, "F%d", vk - VK_F1 + 1);
  else switch (vk)
  {
    case VK_RETURN: lstrcpyA(key, "Enter"); break;
    case VK_ESCAPE: lstrcpyA(key, "Esc"); break;
    case VK_TAB:    lstrcpyA(key, "Tab"); break;
    case VK_SPACE:  lstrcpyA(key, "Space"); break;
    case VK_BACK:   lstrcpyA(key, "Backspace"); break;
    case VK_DELETE: lstrcpyA(key, "Delete"); break;
    case VK_INSERT: lstrcpyA(key, "Insert"); break;
    case VK_HOME:   lstrcpyA(key, "Home"); break;
    case VK_END:    lstrcpyA(key, "End"); break;
    case VK_PRIOR:  lstrcpyA(key, "Page Up"); break;
    case VK_NEXT:   lstrcpyA(key, "Page Down"); break;
    case VK_UP:     lstrcpyA(key, "Up"); break;
    case VK_DOWN:   lstrcpyA(key, "Down"); break;
    case VK_LEFT:   lstrcpyA(key, "Left"); break;
    case VK_RIGHT:  lstrcpyA(key, "Right"); break;
    case VK_PAUSE:  lstrcpyA(key, "Pause"); break;
    case VK_OEM_5:  lstrcpyA(key, "\\"); break;
    case VK_OEM_PERIOD: lstrcpyA(key, "."); break;
    case VK_OEM_COMMA:  lstrcpyA(key, ","); break;
    case VK_OEM_2:  lstrcpyA(key, "/"); break;
    default:
    {
      UINT sc = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
      if (!GetKeyNameTextA((LONG)(sc << 16), key, sizeof(key))) wsprintfA(key, "Key %d", vk);
    }
  }
  lstrcatA(out, key);
  (void)len;
}

/* The table Windows reads the shortcuts from. The caller owns it (DestroyAcceleratorTable). */
HACCEL hotkeys_build_accel(void)
{
  ACCEL a[64];
  int i, n = 0;

  for (i = 0; i < HK_N && n < 64; i++)
  {
    int c = gui.hotkey[i];
    if (!K_VK(c) || hk[i].cmd == HK_FRAMEADV || hk[i].cmd == HK_FASTFWD || hk[i].cmd == HK_REWIND) continue;   /* read directly, not accelerators */
    a[n].fVirt = (BYTE)(FVIRTKEY | ((c & K_CTRL) ? FCONTROL : 0) | ((c & K_SHIFT) ? FSHIFT : 0) | ((c & K_ALT) ? FALT : 0));
    a[n].key = (WORD)K_VK(c);
    a[n].cmd = (WORD)hk[i].cmd;
    n++;
  }
  return n ? CreateAcceleratorTableA(a, n) : NULL;
}

/* The shortcut(s) of a menu command, joined with " / ", or "" if it has none. */
void hotkey_menu_keys(int cmd, char *out, int len)
{
  int j;
  out[0] = '\0';
  for (j = 0; j < HK_N; j++)
  {
    char one[64];
    if (hk[j].cmd != cmd) continue;
    hotkey_text(gui.hotkey[j], one, sizeof(one));
    if (!one[0] || lstrlenA(out) + lstrlenA(one) + 4 >= len) continue;
    if (out[0]) lstrcatA(out, " / ");
    lstrcatA(out, one);
  }
}

/* Rewrites the "\tShortcut" part of the menu items to match the current shortcuts. */
static void set_item_text(HMENU menu, UINT id, const char *text)
{
  MENUITEMINFOA mii;

  memset(&mii, 0, sizeof(mii));
  mii.cbSize = sizeof(mii);
  mii.fMask  = MIIM_FTYPE | MIIM_DATA;
  if (!GetMenuItemInfoA(menu, id, FALSE, &mii)) return;

  if (mii.fType & MFT_OWNERDRAW)
  {
    char *stored = (char *)malloc((size_t)lstrlenA(text) + 1);
    if (!stored) return;
    lstrcpyA(stored, text);
    if (mii.dwItemData) free((void *)mii.dwItemData);
    mii.fMask      = MIIM_DATA;
    mii.dwItemData = (ULONG_PTR)stored;
  }
  else
  {
    mii.fMask      = MIIM_STRING;
    mii.dwTypeData = (LPSTR)text;
  }
  SetMenuItemInfoA(menu, id, FALSE, &mii);
}

static int get_item_text(HMENU menu, UINT id, char *out, int len)
{
  MENUITEMINFOA mii;

  memset(&mii, 0, sizeof(mii));
  mii.cbSize = sizeof(mii);
  mii.fMask  = MIIM_FTYPE | MIIM_DATA | MIIM_STRING;
  mii.dwTypeData = out;
  mii.cch = (UINT)(len - 1);
  out[0] = '\0';
  if (!GetMenuItemInfoA(menu, id, FALSE, &mii)) return 0;
  if ((mii.fType & MFT_OWNERDRAW) && mii.dwItemData) lstrcpynA(out, (const char *)mii.dwItemData, len);
  return 1;
}

static void relabel(HMENU menu)
{
  int i, n = GetMenuItemCount(menu);

  for (i = 0; i < n; i++)
  {
    HMENU sub = GetSubMenu(menu, i);
    if (sub) { relabel(sub); continue; }
    {
      UINT id = GetMenuItemID(menu, i);
      char text[160], keys[96] = "", one[64];
      int j, found = 0;
      char *tab;

      if (id == 0 || id == (UINT)-1) continue;
      for (j = 0; j < HK_N; j++)
      {
        if (hk[j].cmd != (int)id) continue;
        found = 1;
        hotkey_text(gui.hotkey[j], one, sizeof(one));
        if (one[0])
        {
          if (keys[0]) lstrcatA(keys, " / ");
          lstrcatA(keys, one);
        }
      }
      if (!found) continue;
      if (!get_item_text(menu, id, text, sizeof(text))) continue;
      tab = strchr(text, '\t');
      if (tab) *tab = '\0';
      if (keys[0]) { lstrcatA(text, "\t"); lstrcatA(text, keys); }
      set_item_text(menu, id, text);
    }
  }
}

void hotkeys_apply_menu(void)
{
  HMENU m = gui_main_menu();
  if (m) relabel(m);
  DrawMenuBar(g_hwnd);
  browser_toolbar_tips();
}

/* "Name<TAB>Shortcut" lines for the Keyboard Shortcuts window. */
void hotkeys_describe(char *out, int len)
{
  int i;
  out[0] = '\0';
  for (i = 0; i < HK_N; i++)
  {
    char one[64], line[160];
    int base = hk[i].cmd == IDM_FILE_LOADSLOT_BASE ? IDM_FILE_LOADSLOT_BASE : hk[i].cmd == IDM_FILE_SAVESLOT_BASE ? IDM_FILE_SAVESLOT_BASE : 0;

    /* The ten slot rows (Load State from Slot 0 ... 9) are one line when they are consecutive digits. */
    if (base && i + 9 < HK_N)
    {
      int k, same = 1, mods = gui.hotkey[i] & ~0xFFFF;
      for (k = 0; k < 10; k++)
        if (hk[i + k].cmd != base + k || gui.hotkey[i + k] != (mods | ('0' + k))) same = 0;
      if (same)
      {
        hotkey_text(gui.hotkey[i], one, sizeof(one));
        one[lstrlenA(one) - 1] = '\0';          /* "Shift+0" -> "Shift+" */
        wsprintfA(line, "%s\t%s0-9\r\n", base == IDM_FILE_LOADSLOT_BASE ? "Load State from Slot 0-9" : "Save State to Slot 0-9", one);
        if (lstrlenA(out) + lstrlenA(line) < len - 1) lstrcatA(out, line);
        i += 9;
        continue;
      }
    }
    {
      int j, again = 0;                  /* a second key for the same action is part of its first row */
      for (j = 0; j < i; j++) if (hk[j].cmd == hk[i].cmd) again = 1;
      if (again) continue;
    }
    {
      char keys[96];
      hotkey_menu_keys(hk[i].cmd, keys, sizeof(keys));
      lstrcpynA(one, keys, sizeof(one));
    }
    if (!one[0]) continue;
    wsprintfA(line, "%s\t%s\r\n", hk[i].name, one);
    if (lstrlenA(out) + lstrlenA(line) < len - 1) lstrcatA(out, line);
  }
}

/****************************************************************************
 * The Hotkeys window
 ****************************************************************************/

static struct
{
  int work[96];          /* the shortcuts being edited */
  int capture;           /* the row being assigned, or -1 */
  int got;               /* a key press arrived from the hook, to finish in the dialog */
  HWND dlg;
  HHOOK hook;
} hkd;

static void hkd_fill(HWND dlg)
{
  HWND list = GetDlgItem(dlg, IDC_HK_LIST);
  int i;

  /* The rows are made once; after that only the key texts are rewritten, so the list keeps its scroll position and selection. */
  int fresh = ((int)SendMessage(list, LVM_GETITEMCOUNT, 0, 0) != HK_N);

  SendMessage(list, WM_SETREDRAW, FALSE, 0);
  if (fresh) SendMessage(list, LVM_DELETEALLITEMS, 0, 0);
  for (i = 0; i < HK_N; i++)
  {
    LVITEMA it;
    char text[64];

    memset(&it, 0, sizeof(it));
    it.mask = LVIF_TEXT;
    it.iItem = i;
    it.pszText = (LPSTR)hk[i].name;
    if (fresh) SendMessageA(list, LVM_INSERTITEMA, 0, (LPARAM)&it);

    if (hkd.capture == i) lstrcpyA(text, "Press a key ...");
    else hotkey_text(hkd.work[i], text, sizeof(text));
    it.iSubItem = 1;
    it.pszText = text;
    SendMessageA(list, LVM_SETITEMTEXTA, (WPARAM)i, (LPARAM)&it);
  }
  SendMessage(list, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(list, NULL, FALSE);
}

static LRESULT CALLBACK hkd_hook(int code, WPARAM wp, LPARAM lp)
{
  if (code >= 0 && hkd.capture >= 0 && GetActiveWindow() == hkd.dlg)
  {
    if (!(lp & 0x80000000))   /* a key going down */
    {
      int vk = (int)wp;
      if (vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU && vk != VK_LWIN && vk != VK_RWIN &&
          vk != VK_LSHIFT && vk != VK_RSHIFT && vk != VK_LCONTROL && vk != VK_RCONTROL && vk != VK_LMENU && vk != VK_RMENU)
      {
        int c = vk;
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) c |= K_CTRL;
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000)   c |= K_SHIFT;
        if (GetAsyncKeyState(VK_MENU) & 0x8000)    c |= K_ALT;
        hkd.got = c;
        PostMessage(hkd.dlg, WM_APP + 2, 0, 0);
      }
    }
    return 1;   /* every key is taken while a shortcut is being assigned */
  }
  return CallNextHookEx(hkd.hook, code, wp, lp);
}

static void hkd_set_hint(HWND dlg, const char *text)
{
  SetDlgItemTextA(dlg, IDC_HK_HINT, text);
}

static void hkd_begin(HWND dlg, int row)
{
  if (row < 0 || row >= HK_N) return;
  hkd.capture = row;
  hkd_fill(dlg);
  hkd_set_hint(dlg, "Press the new shortcut (any key, alone or with Ctrl, Shift or Alt). Click Assign again to cancel.");
  SetTimer(dlg, 7, 8000, NULL);
}

static void hkd_end(HWND dlg)
{
  KillTimer(dlg, 7);
  hkd.capture = -1;
  hkd_fill(dlg);
}

static INT_PTR CALLBACK hotkeys_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
    case WM_INITDIALOG:
    {
      HWND list = GetDlgItem(dlg, IDC_HK_LIST);
      LVCOLUMNA col;
      RECT rc;
      static const GuiTip tips[] =
      {
        { IDC_HK_LIST, NULL, "Every action that has a keyboard shortcut. Select one and click Assign, or double-click it." },
        { IDC_HK_ASSIGN, NULL, "Press the key (or Ctrl/Shift/Alt and a key) you want for the selected action." },
        { IDC_HK_CLEAR, NULL, "Take the shortcut off the selected action." },
        { IDC_HK_DEFAULTS, NULL, "Put every shortcut back to its original key." },
        { IDOK, NULL, "Save the shortcuts. Menu labels update at once." },
        { IDCANCEL, NULL, "Close without saving." },
      };
      int i;

      hkd.dlg = dlg;
      hkd.capture = -1;
      hkd.got = 0;
      for (i = 0; i < HK_N; i++) hkd.work[i] = gui.hotkey[i];
      hkd.hook = SetWindowsHookExA(WH_KEYBOARD, hkd_hook, NULL, GetCurrentThreadId());

      SendMessage(list, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT);
      GetClientRect(list, &rc);
      memset(&col, 0, sizeof(col));
      col.mask = LVCF_TEXT | LVCF_WIDTH;
      col.pszText = "Action";
      col.cx = (rc.right - rc.left) * 62 / 100;
      SendMessageA(list, LVM_INSERTCOLUMNA, 0, (LPARAM)&col);
      col.pszText = "Shortcut";
      col.cx = (rc.right - rc.left) * 34 / 100;
      SendMessageA(list, LVM_INSERTCOLUMNA, 1, (LPARAM)&col);
      hkd_fill(dlg);
      ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);

      theme_apply_to_window(dlg);
      {
        HWND header = ListView_GetHeader(list);
        if (header && theme_is_dark()) { SetWindowTheme(header, L"ItemsView", NULL); InvalidateRect(header, NULL, TRUE); }
      }
      hkd_set_hint(dlg, "Select an action and click Assign (or double-click it), then press the key you want.");
      gui_dialog_tips(dlg, tips, (int)(sizeof(tips) / sizeof(tips[0])));
      return TRUE;
    }

    case WM_DESTROY:
      if (hkd.hook) { UnhookWindowsHookEx(hkd.hook); hkd.hook = NULL; }
      hkd.capture = -1;
      return FALSE;

    case WM_APP + 2:   /* a key arrived while assigning */
    {
      int row = hkd.capture, c = hkd.got, j;
      char other[160] = "", name[64], buf[320];

      if (row < 0) return TRUE;
      for (j = 0; j < HK_N; j++)
        if (j != row && hkd.work[j] == c && hk[j].cmd != hk[row].cmd)
        {
          int answer;
          char key[64], ask[400];

          hotkey_text(c, key, sizeof(key));
          hkd_end(dlg);                   /* the key is no longer being captured while the question is up */
          wsprintfA(ask, "%s is already the shortcut for \"%s\".\n\n"
                         "Yes: swap, so \"%s\" gets %s and \"%s\" gets the shortcut \"%s\" had.\n"
                         "No: give %s to \"%s\" and leave \"%s\" without a shortcut.\n"
                         "Cancel: keep things as they were.",
                    key, hk[j].name, hk[row].name, key, hk[j].name, hk[row].name, key, hk[row].name, hk[j].name);
          answer = MessageBoxA(dlg, ask, "Shortcut Already Used", MB_YESNOCANCEL | MB_ICONQUESTION);
          if (answer == IDCANCEL) { hkd_set_hint(dlg, "Cancelled: the shortcut was not changed."); return TRUE; }
          hkd.work[j] = (answer == IDYES) ? hkd.work[row] : 0;
          wsprintfA(other, " (\"%s\" %s)", hk[j].name, answer == IDYES ? "got the old shortcut" : "has none now");
          break;
        }
      hkd.work[row] = c;
      hotkey_text(c, name, sizeof(name));
      hkd_end(dlg);
      wsprintfA(buf, "%s is now %s%s.", hk[row].name, name, other);
      hkd_set_hint(dlg, buf);
      return TRUE;
    }

    case WM_TIMER:
      if (wp == 7) { hkd_end(dlg); hkd_set_hint(dlg, "No key was pressed."); }
      return TRUE;

    case WM_NOTIFY:
    {
      NMHDR *h = (NMHDR *)lp;
      if (h->idFrom == IDC_HK_LIST && h->code == NM_DBLCLK)
      {
        int sel = ListView_GetNextItem(GetDlgItem(dlg, IDC_HK_LIST), -1, LVNI_SELECTED);
        if (hkd.capture < 0) hkd_begin(dlg, sel);
        return TRUE;
      }
      return FALSE;
    }

    case WM_CTLCOLORDLG:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    {
      HBRUSH br = theme_ctlcolor((HDC)wp);
      if (br) return (LRESULT)br;
      break;
    }

    case WM_COMMAND:
    {
      HWND list = GetDlgItem(dlg, IDC_HK_LIST);
      int sel = ListView_GetNextItem(list, -1, LVNI_SELECTED);

      switch (LOWORD(wp))
      {
        case IDC_HK_ASSIGN:
          if (hkd.capture >= 0) { hkd_end(dlg); hkd_set_hint(dlg, "Cancelled."); }
          else hkd_begin(dlg, sel);
          return TRUE;

        case IDC_HK_CLEAR:
          if (hkd.capture >= 0) hkd_end(dlg);
          if (sel >= 0) { hkd.work[sel] = 0; hkd_fill(dlg); ListView_SetItemState(list, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED); }
          return TRUE;

        case IDC_HK_DEFAULTS:
        {
          int i;
          if (hkd.capture >= 0) hkd_end(dlg);
          for (i = 0; i < HK_N; i++) hkd.work[i] = hk[i].def;
          hkd_fill(dlg);
          hkd_set_hint(dlg, "Every shortcut is back to its original key (not saved until you click Save).");
          return TRUE;
        }

        case IDOK:
        {
          int i;
          if (hkd.capture >= 0) { hkd_end(dlg); return TRUE; }
          for (i = 0; i < HK_N; i++) gui.hotkey[i] = hkd.work[i];
          config_save();
          gui_hotkeys_changed();
          EndDialog(dlg, IDOK);
          return TRUE;
        }

        case IDCANCEL:
          if (hkd.capture >= 0) { hkd_end(dlg); return TRUE; }
          EndDialog(dlg, IDCANCEL);
          return TRUE;
      }
      return FALSE;
    }

    case WM_CLOSE:
      EndDialog(dlg, IDCANCEL);
      return TRUE;
  }
  return FALSE;
}

void dlg_hotkeys(HWND parent)
{
  gui_dialog_box(gui.large_ui ? IDD_HOTKEYS_LARGE : IDD_HOTKEYS, parent, hotkeys_proc);
}


/****************************************************************************
 * Gamepad hotkeys: press a button on Player 1's XInput pad to run an action.
 * Which button runs which action is set here (Controls > Gamepad Hotkeys...).
 ****************************************************************************/

typedef struct { const char *name; int cmd; int def; } ph_t;

/* The pad button masks are XInput's own. Fast Forward and Rewind are held buttons (cmd 0); the rest
   run their command once per press. None needs Back held: the defaults use the right stick, which
   no game button uses. */
static const ph_t ph[] =
{
  { "Fast Forward (hold)",        0,                    0x0100 },       /* Left Bumper */
  { "Rewind (hold)",              0,                    0x00010000 },   /* LT */
  { "Save State (current slot)",  IDM_ACCEL_QUICKSAVE,  0x00040000 },   /* right stick up */
  { "Load State (current slot)",  IDM_ACCEL_QUICKLOAD,  0x00080000 },   /* right stick down */
  { "Previous Slot",              IDM_ACCEL_SLOT_PREV,  0x00100000 },   /* right stick left */
  { "Next Slot",                  IDM_ACCEL_SLOT_NEXT,  0x00200000 },   /* right stick right */
  { "Toggle Turbo", IDM_INPUT_TURBO_ALL,  0x0080 },       /* right stick button */
  { "Pause and Resume",           IDM_EMU_PAUSE,        0 },
  { "Reset",                      IDM_EMU_RESET,        0 },
  { "Save a Screenshot",          IDM_FILE_SCREENSHOT,  0 },
  { "Fullscreen",                 IDM_VIDEO_FULLSCREEN, 0 },
  { "Mute",                       IDM_AUDIO_ENABLE,     0 },
  { "Record Video + Audio",       IDM_FILE_REC_VIDEO,   0 },
  { "Game Speed: Faster",         IDM_EMU_SPEEDUP,      0 },
  { "Game Speed: Slower",         IDM_EMU_SPEEDDOWN,    0 },
  { "Pause Menu",                 IDM_INPUT_PAUSEMENU,  0 },
};
#define PH_N ((int)(sizeof(ph) / sizeof(ph[0])))

int padhk_count(void) { return PH_N; }
int padhk_cmd(int i) { return ph[i].cmd; }
int padhk_default(int i) { return ph[i].def; }
void padhk_ini_key(int i, char *out)
{
  if (i == PADHK_FASTFWD)      lstrcpyA(out, "pad_fast_forward");
  else if (i == PADHK_REWIND)  lstrcpyA(out, "pad_rewind");
  else                         wsprintfA(out, "padhk_cmd%d", ph[i].cmd);
}
/* Player 1 on a DirectInput joystick: its buttons have their own numbers, kept apart from XInput's. */
static int *padhk_array_for(int dev)
{
  return dev >= GUI_INPUT_XPAD_COUNT ? gui.padhk_di : gui.padhk;
}

static int *padhk_array(void)
{
  return padhk_array_for(gui.pad[0].device);
}

/* The Gamepad Hotkeys window works on the pad the Controls window shows for Player 1 (not saved yet). */
static int phd_dev = -1;

int padhk_mask(int i) { return padhk_array()[i]; }

/* The gamepad hotkey on this pad button, or NULL. */
const char *padhk_owner_of_button(int mask)
{
  int i;
  const int *a = padhk_array();
  if (!mask) return NULL;
  for (i = 0; i < PH_N; i++)
    if (a[i] == mask) return ph[i].name;
  return NULL;
}

int padhk_is_hold(int i) { return i == PADHK_FASTFWD || i == PADHK_REWIND; }

static const char *padhk_button_name(int mask)
{
  switch (mask)
  {
    case 0x0001: return "D-Pad Up";
    case 0x0002: return "D-Pad Down";
    case 0x0004: return "D-Pad Left";
    case 0x0008: return "D-Pad Right";
    case 0x0010: return "Start";
    case 0x0020: return "Back";
    case 0x0040: return "LSB";
    case 0x0080: return "RSB";
    case 0x0100: return "LB";
    case 0x0200: return "RB";
    case 0x1000: return "A";
    case 0x2000: return "B";
    case 0x4000: return "X";
    case 0x8000: return "Y";
    case 0x00010000: return "LT";
    case 0x00020000: return "RT";
    case 0x00040000: return "RS Up";
    case 0x00080000: return "RS Down";
    case 0x00100000: return "RS Left";
    case 0x00200000: return "RS Right";
  }
  return "";
}

static struct
{
  int work[24];
  int capture;       /* the row being assigned, or -1 */
  int phase;         /* 0 = waiting for every button to be let go, 1 = listening */
  DWORD deadline;
} phd;

/* The name of a pad button for Player 1's pad, whichever kind it is. */
static const char *phd_button_name(int mask)
{
  return phd_dev >= GUI_INPUT_XPAD_COUNT ? input_pad_button_name(phd_dev, mask) : padhk_button_name(mask);
}

static void phd_fill(HWND dlg)
{
  HWND list = GetDlgItem(dlg, IDC_PH_LIST);
  int i, fresh = ((int)SendMessage(list, LVM_GETITEMCOUNT, 0, 0) != PH_N + 1);

  SendMessage(list, WM_SETREDRAW, FALSE, 0);
  for (i = 0; i <= PH_N; i++)      /* the last row is the Hold Button */
  {
    LVITEMA it;
    const char *text;

    memset(&it, 0, sizeof(it));
    it.mask = LVIF_TEXT;
    it.iItem = i;
    it.pszText = (LPSTR)(i < PH_N ? ph[i].name : "Hold Button (see the box below the list)");
    if (fresh) SendMessageA(list, LVM_INSERTITEMA, 0, (LPARAM)&it);
    text = (phd.capture == i) ? "Press a button ..." : (phd.work[i] ? phd_button_name(phd.work[i]) : "");
    it.iSubItem = 1;
    it.pszText = (LPSTR)text;
    SendMessageA(list, LVM_SETITEMTEXTA, (WPARAM)i, (LPARAM)&it);
  }
  SendMessage(list, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(list, NULL, FALSE);
}

static void phd_end(HWND dlg)
{
  KillTimer(dlg, 8);
  phd.capture = -1;
  phd_fill(dlg);
}

static void phd_begin(HWND dlg, int row)
{
  int dev = phd_dev;

  if (row < 0 || row > PH_N) return;
  if (dev < 0)
  {
    SetDlgItemTextA(dlg, IDC_PH_HINT, "Player 1 needs a gamepad: pick one under Input Devices in Controls first.");
    return;
  }
  phd.capture = row;
  phd.phase = 0;
  phd.deadline = GetTickCount() + 8000;
  phd_fill(dlg);
  SetDlgItemTextA(dlg, IDC_PH_HINT, "Press the gamepad button you want (the right stick works too). Click Assign again to cancel.");
  SetTimer(dlg, 8, 30, NULL);
}

static INT_PTR CALLBACK padhk_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
    case WM_INITDIALOG:
    {
      HWND list = GetDlgItem(dlg, IDC_PH_LIST);
      LVCOLUMNA col;
      RECT rc;
      static const GuiTip tips[] =
      {
        { IDC_PH_LIST, NULL, "Each action and the pad button that runs it, pressed on its own (Player 1's Xbox-style pad). Select one and click Assign, or double-click it." },
        { IDC_PH_ASSIGN, NULL, "Press the pad button you want for the selected action." },
        { IDC_PH_HOLD, NULL, "When ticked, a gamepad hotkey only runs while you also hold the Hold Button (the last row), so the buttons can still be used in the game on their own." },
        { IDC_PH_CLEAR, NULL, "Take the button off the selected action." },
        { IDC_PH_DEFAULTS, NULL, "Put every action back to its original button." },
        { IDOK, NULL, "Save these settings." },
        { IDCANCEL, NULL, "Close without saving." },
      };
      int i;

      phd.capture = -1;
      for (i = 0; i < PH_N; i++) phd.work[i] = padhk_array_for(phd_dev)[i];
      phd.work[PH_N] = gui.padhk_hold[phd_dev >= GUI_INPUT_XPAD_COUNT ? 1 : 0];
      CheckDlgButton(dlg, IDC_PH_HOLD, gui.padhk_hold_on ? BST_CHECKED : BST_UNCHECKED);

      SendMessage(list, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT);
      GetClientRect(list, &rc);
      memset(&col, 0, sizeof(col));
      col.mask = LVCF_TEXT | LVCF_WIDTH;
      col.pszText = "Action";
      col.cx = (rc.right - rc.left) * 55 / 100;
      SendMessageA(list, LVM_INSERTCOLUMNA, 0, (LPARAM)&col);
      col.pszText = "Gamepad Button";
      col.cx = (rc.right - rc.left) * 41 / 100;
      SendMessageA(list, LVM_INSERTCOLUMNA, 1, (LPARAM)&col);
      phd_fill(dlg);
      ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);

      theme_apply_to_window(dlg);
      {
        HWND header = ListView_GetHeader(list);
        if (header && theme_is_dark()) { SetWindowTheme(header, L"ItemsView", NULL); InvalidateRect(header, NULL, TRUE); }
      }
      SetDlgItemTextA(dlg, IDC_PH_HINT, "Select an action and click Assign (or double-click it), then press the pad button you want. A button used here also reaches the game, so the right stick is a good choice.");
      gui_dialog_tips(dlg, tips, (int)(sizeof(tips) / sizeof(tips[0])));
      return TRUE;
    }

    case WM_TIMER:
      if (wp == 8 && phd.capture >= 0)
      {
        int dev = phd_dev, m, j, row = phd.capture;

        if ((int)(GetTickCount() - phd.deadline) >= 0) { phd_end(dlg); SetDlgItemTextA(dlg, IDC_PH_HINT, "No button was pressed."); return TRUE; }
        if (GetForegroundWindow() != dlg) return TRUE;
        if (phd.phase == 0) { if (!input_any_input_down(dev)) phd.phase = 1; return TRUE; }
        m = input_capture_gamepad(dev);
        if (m)
        {
          char buf[360], other[100] = "";
          for (j = 0; j <= PH_N; j++)
            if (j != row && phd.work[j] == m) { phd.work[j] = 0; wsprintfA(other, " (it was on \"%s\")", j < PH_N ? ph[j].name : "the Hold Button"); }
          phd.work[row] = m;
          phd_end(dlg);
          wsprintfA(buf, "%s is now %s%s.", row < PH_N ? ph[row].name : "The Hold Button", phd_button_name(m), other);
          if (phd_dev >= 0)
            for (j = 0; j < PAD_KEYS; j++)
              if (gui.pad[0].button[j] == m)
              {
                lstrcatA(buf, " Note: it is also the Player 1 game button ");
                lstrcatA(buf, input_button_label(j));
                lstrcatA(buf, " (Controls), so it does both.");
                break;
              }
          SetDlgItemTextA(dlg, IDC_PH_HINT, buf);
        }
      }
      return TRUE;

    case WM_NOTIFY:
    {
      NMHDR *h = (NMHDR *)lp;
      if (h->idFrom == IDC_PH_LIST && h->code == NM_DBLCLK)
      {
        int sel = ListView_GetNextItem(GetDlgItem(dlg, IDC_PH_LIST), -1, LVNI_SELECTED);
        if (phd.capture < 0) phd_begin(dlg, sel);
        return TRUE;
      }
      return FALSE;
    }

    case WM_CTLCOLORDLG:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    {
      HBRUSH br = theme_ctlcolor((HDC)wp);
      if (br) return (LRESULT)br;
      break;
    }

    case WM_COMMAND:
    {
      HWND list = GetDlgItem(dlg, IDC_PH_LIST);
      int sel = ListView_GetNextItem(list, -1, LVNI_SELECTED);

      switch (LOWORD(wp))
      {
        case IDC_PH_ASSIGN:
          if (phd.capture >= 0) { phd_end(dlg); SetDlgItemTextA(dlg, IDC_PH_HINT, "Cancelled."); }
          else phd_begin(dlg, sel);
          return TRUE;

        case IDC_PH_CLEAR:
          if (phd.capture >= 0) phd_end(dlg);
          if (sel >= 0) { phd.work[sel] = 0; phd_fill(dlg); }
          return TRUE;

        case IDC_PH_DEFAULTS:
        {
          int i;
          if (phd.capture >= 0) phd_end(dlg);
          for (i = 0; i < PH_N; i++) phd.work[i] = phd_dev >= GUI_INPUT_XPAD_COUNT ? 0 : ph[i].def;   /* a joystick has no defaults */
          phd.work[PH_N] = 0;
          CheckDlgButton(dlg, IDC_PH_HOLD, BST_UNCHECKED);
          phd_fill(dlg);
          SetDlgItemTextA(dlg, IDC_PH_HINT, "Every action is back to its original button (not saved until you click Save).");
          return TRUE;
        }

        case IDOK:
        {
          int i;
          if (phd.capture >= 0) { phd_end(dlg); return TRUE; }
          for (i = 0; i < PH_N; i++) padhk_array_for(phd_dev)[i] = phd.work[i];
          gui.padhk_hold[phd_dev >= GUI_INPUT_XPAD_COUNT ? 1 : 0] = phd.work[PH_N];
          gui.padhk_hold_on = IsDlgButtonChecked(dlg, IDC_PH_HOLD) == BST_CHECKED;
          if (gui.padhk_hold_on && !phd.work[PH_N])
          {
            MessageBoxA(dlg, "Choose a Hold Button in the last row first, or untick the box.", "Gamepad Hotkeys", MB_OK | MB_ICONINFORMATION);
            return TRUE;
          }
          config_save();
          EndDialog(dlg, IDOK);
          return TRUE;
        }

        case IDCANCEL:
          if (phd.capture >= 0) { phd_end(dlg); return TRUE; }
          EndDialog(dlg, IDCANCEL);
          return TRUE;
      }
      return FALSE;
    }

    case WM_CLOSE:
      EndDialog(dlg, IDCANCEL);
      return TRUE;
  }
  return FALSE;
}

void dlg_pad_hotkeys(HWND parent, int dev)
{
  phd_dev = dev;
  gui_dialog_box(gui.large_ui ? IDD_PADHK_LARGE : IDD_PADHK, parent, padhk_proc);
}
