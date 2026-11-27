/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *  Copyright (c) 2026 Hazem Abdelghani
 *
 *  extras.c -- small Windows-integration helpers: file type registration,
 *  game shortcuts, manuals.
 ****************************************************************************/

#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <zlib.h>
#include <commctrl.h>
#include "theme.h"

#include "shared.h"
#include "gui.h"
#include "resource.h"

/****************************************************************************
 * Game shortcuts
 ****************************************************************************/

/* The game's name for a file name: the file name without its extension. */
static void game_label(const char *rom_path, char *out, int out_len)
{
  const char *name = strrchr(rom_path, '\\');
  char *dot;
  int i;

  name = name ? name + 1 : rom_path;
  lstrcpynA(out, name, out_len);
  dot = strrchr(out, '.');
  if (dot) *dot = '\0';
  for (i = 0; out[i]; i++)
    if (strchr("\\/:*?\"<>|", out[i])) out[i] = '_';   /* not allowed in a file name */
}

/* A shortcut to this game: on the desktop (start_menu = 0) or in the Start menu's Programs folder. */
int extras_make_shortcut(const char *rom_path, int start_menu)
{
  char exe[MAX_PATH], dir[MAX_PATH], label[160], lnk[MAX_PATH + 200], args[GUI_PATH_LEN + 4], *slash;
  wchar_t wlnk[MAX_PATH + 200];
  IShellLinkA *psl = NULL;
  IPersistFile *ppf = NULL;
  HRESULT hr, hr_init;
  int ok = 0;

  if (!GetModuleFileNameA(NULL, exe, sizeof(exe))) return 0;
  if (FAILED(SHGetFolderPathA(NULL, start_menu ? CSIDL_PROGRAMS : CSIDL_DESKTOPDIRECTORY, NULL, 0, dir))) return 0;

  game_label(rom_path, label, sizeof(label));
  wsprintfA(lnk, "%s\\%s.lnk", dir, label);
  wsprintfA(args, "--fullscreen \"%s\"", rom_path);
  MultiByteToWideChar(CP_ACP, 0, lnk, -1, wlnk, (int)(sizeof(wlnk) / sizeof(wlnk[0])));

  hr_init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkA, (void **)&psl);
  if (SUCCEEDED(hr) && psl)
  {
    char wdir[MAX_PATH];
    lstrcpynA(wdir, exe, sizeof(wdir));
    slash = strrchr(wdir, '\\');
    if (slash) *slash = '\0';

    IShellLinkA_SetPath(psl, exe);
    IShellLinkA_SetArguments(psl, args);
    IShellLinkA_SetWorkingDirectory(psl, wdir);
    IShellLinkA_SetIconLocation(psl, exe, 0);
    IShellLinkA_SetDescription(psl, label);
    if (SUCCEEDED(IShellLinkA_QueryInterface(psl, &IID_IPersistFile, (void **)&ppf)) && ppf)
    {
      ok = SUCCEEDED(IPersistFile_Save(ppf, wlnk, TRUE));
      IPersistFile_Release(ppf);
    }
    IShellLinkA_Release(psl);
  }
  if (SUCCEEDED(hr_init)) CoUninitialize();
  return ok;
}

/****************************************************************************
 * Shader Parameters window: the loaded shader preset's own parameters, changed while it runs
 ****************************************************************************/

#include "shader_chain.h"

static int sp_sel;

/* How many steps the slider has for this setting: the thumb jumps from one value of the preset's step to the next
   (1, 2, 3 ...). 0 when the setting has no sensible step: then it slides smoothly. */
static int sp_steps(const SHADER_PARAM *q)
{
  if (q->step > 0 && q->max > q->min)
  {
    int n = (int)((q->max - q->min) / q->step + 0.5f);
    if (n >= 1 && n <= 1000) return n;
  }
  return 0;
}

static void sp_show(HWND dlg)
{
  SHADER_PARAM *q = shader_chain_param(sp_sel);
  char t[32];
  int n;

  if (!q) return;
  n = sp_steps(q);
  SendDlgItemMessage(dlg, IDC_SP_SLIDER, TBM_SETRANGE, TRUE, MAKELPARAM(0, n ? n : 1000));
  SendDlgItemMessage(dlg, IDC_SP_SLIDER, TBM_SETPAGESIZE, 0, n ? (n > 10 ? n / 10 : 1) : 25);
  SendDlgItemMessage(dlg, IDC_SP_SLIDER, TBM_SETLINESIZE, 0, 1);
  SendDlgItemMessage(dlg, IDC_SP_SLIDER, TBM_SETPOS, TRUE,
                     q->max > q->min ? (n ? (LPARAM)((q->cur - q->min) / q->step + 0.5f)
                                          : (LPARAM)((q->cur - q->min) * 1000.0f / (q->max - q->min) + 0.5f)) : 0);
  snprintf(t, sizeof(t), "%g", q->cur);
  SetDlgItemTextA(dlg, IDC_SP_VALUE, t);
}

/* What a setting is called in the list: its description, or its own name when the description has no letters or digits in it (some presets leave it empty). */
static const char *sp_label(const SHADER_PARAM *q, char *buf, int len)
{
  const char *p;

  for (p = q->desc; *p; p++) if (isalnum((unsigned char)*p) || (unsigned char)*p >= 0x80) return q->desc;
  wsprintfA(buf, "[%s]", q->name);
  (void)len;
  return buf;
}

static int sp_filling;
static float *sp_saved;          /* the values the settings had when the window opened: Cancel puts them back */
static int sp_saved_n;

/* The list as the Search box asks (every word in the label, any case); the setting being edited stays picked when it is still there. */
static void sp_fill(HWND dlg)
{
  HWND list = GetDlgItem(dlg, IDC_SP_LIST);
  char q[128], label[160], lower[200], lname[80];
  char *words[8], *p;
  int nw = 0, i, n = shader_chain_param_count(), shown = 0, pick = -1;

  GetDlgItemTextA(dlg, IDC_SP_SEARCH, q, sizeof(q));
  CharLowerA(q);
  for (p = strtok(q, " "); p && nw < 8; p = strtok(NULL, " ")) words[nw++] = p;

  sp_filling = 1;
  SendMessage(list, WM_SETREDRAW, FALSE, 0);
  SendMessage(list, LVM_DELETEALLITEMS, 0, 0);
  for (i = 0; i < n; i++)
  {
    const SHADER_PARAM *prm = shader_chain_param(i);
    const char *txt = sp_label(prm, label, sizeof(label));
    int k, ok = 1;
    LVITEMA it;

    lstrcpynA(lower, txt, sizeof(lower));
    CharLowerA(lower);
    lstrcpynA(lname, prm->name, sizeof(lname));
    CharLowerA(lname);
    for (k = 0; k < nw && ok; k++) if (!strstr(lower, words[k]) && !strstr(lname, words[k])) ok = 0;
    if (!ok) continue;
    ZeroMemory(&it, sizeof(it));
    it.mask = LVIF_TEXT | LVIF_PARAM; it.iItem = shown; it.pszText = (char *)txt; it.lParam = i;
    SendMessageA(list, LVM_INSERTITEMA, 0, (LPARAM)&it);
    if (i == sp_sel) pick = shown;
    shown++;
  }
  SendMessage(list, WM_SETREDRAW, TRUE, 0);
  if (pick < 0 && shown > 0) pick = 0;
  if (pick >= 0)
  {
    ListView_SetItemState(list, pick, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(list, pick, FALSE);
  }
  sp_filling = 0;
  if (pick >= 0) { LVITEMA it; ZeroMemory(&it, sizeof(it)); it.mask = LVIF_PARAM; it.iItem = pick; SendMessageA(list, LVM_GETITEMA, 0, (LPARAM)&it); sp_sel = (int)it.lParam; sp_show(dlg); }
  InvalidateRect(list, NULL, TRUE);
}

static INT_PTR CALLBACK shaderparams_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
    case WM_INITDIALOG:
    {
      HWND list = GetDlgItem(dlg, IDC_SP_LIST);
      LVCOLUMNA col;
      RECT lr;

      sp_sel = 0;
      sp_saved_n = shader_chain_param_count();
      sp_saved = (float *)malloc(sizeof(float) * (size_t)(sp_saved_n > 0 ? sp_saved_n : 1));
      if (sp_saved) { int i; for (i = 0; i < sp_saved_n; i++) sp_saved[i] = shader_chain_param(i)->cur; }
      ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT);
      theme_apply_to_window(dlg);
      GetClientRect(list, &lr);
      ZeroMemory(&col, sizeof(col));
      col.mask = LVCF_WIDTH; col.cx = lr.right - GetSystemMetrics(SM_CXVSCROLL) - 2;        /* (room for the bar up and down: no bar sideways) */
      SendMessageA(list, LVM_INSERTCOLUMNA, 0, (LPARAM)&col);
      sp_fill(dlg);
      return TRUE;
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORLISTBOX:
    {
      HBRUSH br = theme_ctlcolor_ctl((HDC)wp, (HWND)lp);
      if (br) return (LRESULT)br;
      break;
    }
    case WM_NOTIFY:
    {
      NMHDR *h = (NMHDR *)lp;
      if (h->idFrom == IDC_SP_LIST && h->code == LVN_ITEMCHANGED && !sp_filling)
      {
        NMLISTVIEW *nm = (NMLISTVIEW *)lp;
        if ((nm->uChanged & LVIF_STATE) && (nm->uNewState & LVIS_SELECTED) && !(nm->uOldState & LVIS_SELECTED) && nm->lParam >= 0 && nm->lParam < shader_chain_param_count())
        {
          sp_sel = (int)nm->lParam;
          sp_show(dlg);
        }
      }
      return TRUE;
    }
    case WM_HSCROLL:
    {
      SHADER_PARAM *q = shader_chain_param(sp_sel);
      if (q && (HWND)lp == GetDlgItem(dlg, IDC_SP_SLIDER))
      {
        int pos = (int)SendDlgItemMessage(dlg, IDC_SP_SLIDER, TBM_GETPOS, 0, 0);
        int n = sp_steps(q);
        float v = n ? q->min + (float)pos * q->step : q->min + (q->max - q->min) * pos / 1000.0f;
        char t[32];
        v = (float)((int)(v * 10000.0f + (v < 0 ? -0.5f : 0.5f))) / 10000.0f;
        shader_chain_param_set(sp_sel, v);
        video_picture_refresh();                       /* (the picture is drawn again with the new value: a paused game, or one waiting behind this window, shows it too) */
        snprintf(t, sizeof(t), "%g", q->cur);
        SetDlgItemTextA(dlg, IDC_SP_VALUE, t);
      }
      return TRUE;
    }
    case WM_COMMAND:
      switch (LOWORD(wp))
      {
        case IDC_SP_SEARCH:
          if (HIWORD(wp) == EN_CHANGE) sp_fill(dlg);
          return TRUE;
        case IDC_SP_RESET:
        {
          SHADER_PARAM *q = shader_chain_param(sp_sel);
          if (q) { shader_chain_param_set(sp_sel, q->init); video_picture_refresh(); sp_show(dlg); }
          return TRUE;
        }
        case IDC_SP_RESETALL:
        {
          int i;
          for (i = 0; i < shader_chain_param_count(); i++) shader_chain_param_set(i, shader_chain_param(i)->init);
          video_picture_refresh();
          sp_show(dlg);
          return TRUE;
        }
        case IDOK:
          if (GetFocus() == GetDlgItem(dlg, IDC_SP_SEARCH)) { SetFocus(GetDlgItem(dlg, IDC_SP_LIST)); return TRUE; }       /* (Enter in the Search box is not OK) */
          free(sp_saved); sp_saved = NULL;
          EndDialog(dlg, 1);
          return TRUE;
        case IDCANCEL:
          if (sp_saved)                                                      /* the settings go back to what they were when the window opened */
          {
            int i;
            for (i = 0; i < sp_saved_n && i < shader_chain_param_count(); i++) shader_chain_param_set(i, sp_saved[i]);
            video_picture_refresh();
            free(sp_saved); sp_saved = NULL;
          }
          EndDialog(dlg, 0);
          return TRUE;
      }
      break;
  }
  return FALSE;
}

void extras_shader_params(HWND parent)
{
  if (!shader_chain_param_count()) { MessageBoxA(parent, "The loaded shader has no settings of its own (or no shader is loaded).", APP_NAME, MB_OK | MB_ICONINFORMATION); return; }
  gui_dialog_box(gui.large_ui ? IDD_SHADERPARAMS_LARGE : IDD_SHADERPARAMS, parent, shaderparams_proc);
}

/****************************************************************************
 * Shader Preset Browser: the .slangp presets of the Shaders folder in a list; the one picked is tried on the running game
 ****************************************************************************/

typedef struct { char rel[MAX_PATH]; char full[GUI_PATH_LEN]; } sb_item;

#define SB_MAX 6000

static sb_item *sb_all;
static int      sb_n;
static int      sb_shown[SB_MAX];       /* the entries the Search box lets through (indexes into sb_all) */
static char     sb_applied[GUI_PATH_LEN]; /* the preset in use (kept at every pick) */
static char     sb_initial[GUI_PATH_LEN]; /* the preset when the window opened: Cancel puts it back */
static char     sb_cur[GUI_PATH_LEN];   /* the preset loaded now ("" none) */
static int      sb_ok;                  /* what is loaded now loaded without an error */
static int      sb_loading;             /* a pick waits for the timer */

static void sb_scan(const char *dir, const char *rel, int depth)
{
  WIN32_FIND_DATAA fd;
  char pat[GUI_PATH_LEN];
  HANDLE h;

  if (depth > 8 || sb_n >= SB_MAX) return;
  wsprintfA(pat, "%s\\*", dir);
  h = FindFirstFileA(pat, &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do
  {
    char full[GUI_PATH_LEN], r[MAX_PATH];

    if (!lstrcmpA(fd.cFileName, ".") || !lstrcmpA(fd.cFileName, "..")) continue;
    if (lstrlenA(dir) + 1 + lstrlenA(fd.cFileName) >= GUI_PATH_LEN || lstrlenA(rel) + 1 + lstrlenA(fd.cFileName) >= MAX_PATH) continue;       /* (a path too long for the buffers is left out) */
    wsprintfA(full, "%s\\%s", dir, fd.cFileName);
    wsprintfA(r, "%s%s%s", rel, rel[0] ? "\\" : "", fd.cFileName);
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    {
      if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) sb_scan(full, r, depth + 1);
    }
    else
    {
      int n = lstrlenA(fd.cFileName);
      if (n > 7 && !lstrcmpiA(fd.cFileName + n - 7, ".slangp") && sb_n < SB_MAX)
      {
        lstrcpynA(sb_all[sb_n].full, full, sizeof(sb_all[sb_n].full));
        r[lstrlenA(r) - 7] = '\0';                          /* (without .slangp) */
        lstrcpynA(sb_all[sb_n].rel, r, sizeof(sb_all[sb_n].rel));
        sb_n++;
      }
    }
  } while (FindNextFileA(h, &fd));
  FindClose(h);
}

static int sb_cmp(const void *a, const void *b) { return lstrcmpiA(((const sb_item *)a)->rel, ((const sb_item *)b)->rel); }

/* A preset as the person knows it: the path under the Shaders folder without .slangp, or the whole path for one from elsewhere ("None" for no preset). */
static void sb_name(const char *full, char *out, int len)
{
  const char *root = osd_path("Shaders");
  int n = lstrlenA(root);

  if (!full[0]) { lstrcpynA(out, "None", len); return; }
  if (!_strnicmp(full, root, n) && full[n] == '\\') lstrcpynA(out, full + n + 1, len); else lstrcpynA(out, full, len);
  n = lstrlenA(out);
  if (n > 7 && !lstrcmpiA(out + n - 7, ".slangp")) out[n - 7] = '\0';
}

/* The line over the buttons (which preset is in use) and whether Parameters has something to open. */
static void sb_state(HWND dlg)
{
  char a[GUI_PATH_LEN], t[GUI_PATH_LEN + 64];

  sb_name(sb_applied, a, sizeof(a));
  wsprintfA(t, "In use: %s", a);
  SetDlgItemTextA(dlg, IDC_SB_NOW, t);
  EnableWindow(GetDlgItem(dlg, IDC_SB_PARAMS), sb_ok && sb_cur[0] && shader_chain_param_count() > 0);
}

static void sb_info(HWND dlg, const char *fmt, const char *a)
{
  char t[700];
  wsprintfA(t, fmt, a);
  SetDlgItemTextA(dlg, IDC_SB_INFO, t);
}

/* The text of an entry of the list: its name, and a mark on the preset in use. */
static void sb_text(int idx, char *out, int len)
{
  lstrcpynA(out, sb_all[idx].rel, len);
  if (sb_applied[0] && !lstrcmpiA(sb_all[idx].full, sb_applied)) lstrcatA(out, "      (in use)");
}

/* The list as the Search box asks (every word has to be in the name, any case); the loaded preset stays picked. */
static void sb_fill(HWND dlg)
{
  HWND list = GetDlgItem(dlg, IDC_SB_LIST);
  char q[128], lower[MAX_PATH];
  char *words[8];
  int nw = 0, i, shown = 0, pick = -1;
  char *p;

  GetDlgItemTextA(dlg, IDC_SB_SEARCH, q, sizeof(q));
  CharLowerA(q);
  for (p = strtok(q, " "); p && nw < 8; p = strtok(NULL, " ")) words[nw++] = p;

  SendMessage(list, WM_SETREDRAW, FALSE, 0);
  SendMessage(list, LVM_DELETEALLITEMS, 0, 0);
  for (i = 0; i < sb_n; i++)
  {
    int k, ok = 1;
    lstrcpynA(lower, sb_all[i].rel, sizeof(lower));
    CharLowerA(lower);
    for (k = 0; k < nw && ok; k++) if (!strstr(lower, words[k])) ok = 0;
    if (!ok) continue;
    {
      LVITEMA it;
      char text[MAX_PATH + 16];
      ZeroMemory(&it, sizeof(it));
      sb_text(i, text, sizeof(text));
      it.mask = LVIF_TEXT; it.iItem = shown; it.pszText = text;
      SendMessageA(list, LVM_INSERTITEMA, 0, (LPARAM)&it);
    }
    sb_shown[shown] = i;
    if (sb_cur[0] && !lstrcmpiA(sb_all[i].full, sb_cur)) pick = shown;
    shown++;
  }
  SendMessage(list, WM_SETREDRAW, TRUE, 0);
  if (pick >= 0)
  {
    sb_loading = -1;                                              /* (the pick of the loaded preset is not a new one) */
    ListView_SetItemState(list, pick, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(list, pick, FALSE);
    sb_loading = 0;
  }
  InvalidateRect(list, NULL, TRUE);
}

/* Keeps the preset that is loaded: saved, the one in use, marked in the list (only the text of the two entries changes: the list is not made again,
   which left the picked row blank until the pointer moved over it). */
static void sb_apply(HWND dlg)
{
  HWND list = GetDlgItem(dlg, IDC_SB_LIST);
  char old[GUI_PATH_LEN];
  int k, shown = ListView_GetItemCount(list);

  lstrcpynA(old, sb_applied, sizeof(old));
  gui_shader_commit();
  lstrcpynA(sb_applied, sb_cur, sizeof(sb_applied));
  for (k = 0; k < shown && k < SB_MAX; k++)
  {
    const sb_item *it = &sb_all[sb_shown[k]];
    if ((old[0] && !lstrcmpiA(it->full, old)) || (sb_applied[0] && !lstrcmpiA(it->full, sb_applied)))
    {
      char text[MAX_PATH + 16];
      sb_text(sb_shown[k], text, sizeof(text));
      ListView_SetItemText(list, k, 0, text);
    }
  }
  InvalidateRect(list, NULL, TRUE);
  if (sb_cur[0])
  {
    const char *slash = strrchr(sb_cur, '\\');
    char name[GUI_PATH_LEN], *dot;
    lstrcpynA(name, slash ? slash + 1 : sb_cur, sizeof(name));
    dot = strrchr(name, '.');
    if (dot) *dot = '\0';
    gui_notify("Shader: %s", name);
  }
  else gui_notify("Shader: None");
  sb_state(dlg);
  video_picture_refresh();                         /* (a paused game shows the new shader at once) */
}

/* Loads the picked preset (after the short wait that lets the arrow keys run down the list without a load at every step). */
static void sb_load_pick(HWND dlg)
{
  HWND list = GetDlgItem(dlg, IDC_SB_LIST);
  int sel = ListView_GetNextItem(list, -1, LVNI_SELECTED);
  char err[400];

  sb_loading = 0;
  if (sel < 0 || sel >= SB_MAX) return;
  {
    const sb_item *it = &sb_all[sb_shown[sel]];
    HCURSOR old = SetCursor(LoadCursorA(NULL, IDC_WAIT));
    sb_ok = gui_set_shader_quiet(it->full, err, sizeof(err));
    SetCursor(old);
    if (sb_ok)
    {
      lstrcpynA(sb_cur, it->full, sizeof(sb_cur));
      SetDlgItemTextA(dlg, IDC_SB_INFO, "");
    }
    else
    {
      sb_info(dlg, "Could not load it: %s", err);
      if (gui_set_shader_quiet(sb_applied, NULL, 0)) { lstrcpynA(sb_cur, sb_applied, sizeof(sb_cur)); sb_ok = 1; }       /* (the preset in use is back on the game, as the "In use" line says) */
      else sb_cur[0] = '\0';
    }
  }
  sb_state(dlg);
  if (sb_ok && lstrcmpiA(sb_cur, sb_applied)) sb_apply(dlg);          /* (a pick is kept at once) */
}

static INT_PTR CALLBACK shaderbrowser_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
    case WM_INITDIALOG:
    {
      HWND list = GetDlgItem(dlg, IDC_SB_LIST);
      LVCOLUMNA col;
      RECT lr;

      sb_n = 0; sb_loading = 0; sb_ok = 1;
      lstrcpynA(sb_applied, gui.shader_preset_path, sizeof(sb_applied));
      lstrcpynA(sb_initial, sb_applied, sizeof(sb_initial));
      lstrcpynA(sb_cur, sb_applied, sizeof(sb_cur));
      sb_all = (sb_item *)calloc(SB_MAX, sizeof(sb_item));
      if (sb_all) sb_scan(osd_path("Shaders"), "", 0);
      if (sb_n > 1) qsort(sb_all, (size_t)sb_n, sizeof(sb_item), sb_cmp);

      ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT);
      theme_apply_to_window(dlg);
      GetClientRect(list, &lr);
      ZeroMemory(&col, sizeof(col));
      col.mask = LVCF_WIDTH; col.cx = lr.right;
      SendMessageA(list, LVM_INSERTCOLUMNA, 0, (LPARAM)&col);
      sb_fill(dlg);
      if (sb_n == 0)
      {
        sb_info(dlg, "No presets (.slangp files) found in %s\nPut the slang-shaders folders there.", osd_path("Shaders"));
      }
      else
      {
        char t[GUI_PATH_LEN + 60];
        wsprintfA(t, "%d presets in %s", sb_n, osd_path("Shaders"));
        SetDlgItemTextA(dlg, IDC_SB_INFO, t);
      }
      sb_state(dlg);
      {
        static const GuiTip tips[] =
        {
          { IDC_SB_SEARCH, "Search:", "Type part of a preset's name. Several words narrow the list: every word has to be in the name." },
          { IDC_SB_LIST, NULL, "The presets (.slangp) of the Shaders folder and the folders below it. The one you pick is used on the running game at once." },
          { IDC_SB_FOLDER, NULL, "Open the Shaders folder in Explorer, to add presets." },
          { IDOK, NULL, "Close the window and keep the preset in use." },
          { IDC_SB_PARAMS, NULL, "Change the picked preset's own settings while it runs." },
          { IDC_SB_NONE, NULL, "Switch shaders off." },
          { IDCANCEL, NULL, "Close the window and put back the preset that was in use when it opened." },
        };
        gui_dialog_tips(dlg, tips, (int)(sizeof(tips) / sizeof(tips[0])));
      }
      return TRUE;
    }
    case WM_DESTROY:
      KillTimer(dlg, 1);
      free(sb_all); sb_all = NULL; sb_n = 0;
      return FALSE;
    case WM_CTLCOLORDLG:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORLISTBOX:
    {
      HBRUSH br = theme_ctlcolor_ctl((HDC)wp, (HWND)lp);
      if (br) return (LRESULT)br;
      break;
    }
    case WM_NOTIFY:
    {
      NMHDR *h = (NMHDR *)lp;
      if (h->idFrom == IDC_SB_LIST)
      {
        NMLISTVIEW *nm = (NMLISTVIEW *)lp;
        if (h->code == LVN_ITEMCHANGED && (nm->uChanged & LVIF_STATE) && (nm->uNewState & LVIS_SELECTED) && !(nm->uOldState & LVIS_SELECTED) && sb_loading >= 0)
        {
          sb_loading = 1;
          SetTimer(dlg, 1, 250, NULL);
        }
        else if (h->code == NM_DBLCLK && ListView_GetNextItem(h->hwndFrom, -1, LVNI_SELECTED) >= 0)
          PostMessage(dlg, WM_COMMAND, IDOK, 0);
      }
      return TRUE;
    }
    case WM_TIMER:
      if (wp == 1) { KillTimer(dlg, 1); sb_load_pick(dlg); }
      return TRUE;
    case WM_COMMAND:
      switch (LOWORD(wp))
      {
        case IDC_SB_SEARCH:
          if (HIWORD(wp) == EN_CHANGE) sb_fill(dlg);
          return TRUE;
        case IDC_SB_NONE:
          KillTimer(dlg, 1); sb_loading = 0;
          gui_set_shader_quiet(NULL, NULL, 0);
          sb_cur[0] = '\0'; sb_ok = 1;
          SetDlgItemTextA(dlg, IDC_SB_INFO, "");
          sb_loading = -1;
          ListView_SetItemState(GetDlgItem(dlg, IDC_SB_LIST), -1, 0, LVIS_SELECTED);
          sb_loading = 0;
          sb_state(dlg);
          if (sb_applied[0]) sb_apply(dlg);
          return TRUE;
        case IDC_SB_PARAMS:
          if (sb_loading) { KillTimer(dlg, 1); sb_load_pick(dlg); }
          if (shader_chain_param_count()) extras_shader_params(dlg);
          return TRUE;
        case IDC_SB_FOLDER:
          ShellExecuteA(dlg, "open", osd_path("Shaders"), NULL, NULL, SW_SHOWNORMAL);
          return TRUE;
        case IDOK:
          if (GetFocus() == GetDlgItem(dlg, IDC_SB_SEARCH)) { SetFocus(GetDlgItem(dlg, IDC_SB_LIST)); return TRUE; }       /* (Enter in the Search box is not OK) */
          if (sb_loading) { KillTimer(dlg, 1); sb_load_pick(dlg); }
          if (!sb_ok) { gui_set_shader_quiet(sb_applied, NULL, 0); gui_shader_commit(); }       /* (the last pick did not load: the preset in use before it comes back) */
          EndDialog(dlg, 1);
          return TRUE;
        case IDCANCEL:
          KillTimer(dlg, 1); sb_loading = 0;
          if (lstrcmpiA(sb_applied, sb_initial) || !sb_ok)             /* the preset that was there comes back (and is saved again) */
          {
            gui_set_shader_quiet(sb_initial, NULL, 0);
            gui_shader_commit();
          }
          EndDialog(dlg, 0);
          return TRUE;
      }
      break;
  }
  return FALSE;
}

void extras_shader_browser(HWND parent)
{
  gui_dialog_box(gui.large_ui ? IDD_SHADERBROWSER_LARGE : IDD_SHADERBROWSER, parent, shaderbrowser_proc);
}

/****************************************************************************
 * Taskbar progress
 ****************************************************************************/

/* The green bar on the program's taskbar button: mode 0 = none, 1 = a bar filled pct percent, 2 = a moving bar (no end
   in sight). Only talks to Windows when the state changes, and does so on a thread of its own: the call goes to the
   desktop shell, and when that is slow to answer (it took seconds when a recording started or ended) the game must
   not wait for it. */
static HWND          tb_hwnd;
static volatile LONG tb_mode = -1, tb_pct;
static HANDLE        tb_event, tb_thread;
static volatile LONG tb_again;          /* the taskbar button was made again (Explorer restarted): the thread gets a new taskbar object and says everything again */
static int           tp_last_mode = -1, tp_last_pct = -1;

static DWORD WINAPI taskbar_thread(LPVOID arg)
{
  ITaskbarList3 *tbl = NULL;
  int last_mode = -1, last_pct = -1;
  (void)arg;

  CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  for (;;)
  {
    int mode, pct;

    WaitForSingleObject(tb_event, INFINITE);
    if (InterlockedExchange(&tb_again, 0))                  /* (Explorer made the button again: the old object is no good, and the bar is said again) */
    {
      if (tbl) { tbl->lpVtbl->Release(tbl); tbl = NULL; }
      last_mode = last_pct = -1;
    }
    if (!tbl)
    {
      if (CoCreateInstance(&CLSID_TaskbarList, NULL, CLSCTX_INPROC_SERVER, &IID_ITaskbarList3, (void **)&tbl) == S_OK && tbl)
      {
        if (tbl->lpVtbl->HrInit(tbl) != S_OK) { tbl->lpVtbl->Release(tbl); tbl = NULL; }
      }
      else tbl = NULL;
    }
    mode = (int)tb_mode;
    pct = (int)tb_pct;
    if (!tbl) continue;
    if (mode == last_mode && (mode != 1 || pct == last_pct)) continue;
    last_mode = mode;
    last_pct = pct;

    if (mode == 0)      tbl->lpVtbl->SetProgressState(tbl, tb_hwnd, TBPF_NOPROGRESS);
    else if (mode == 2) tbl->lpVtbl->SetProgressState(tbl, tb_hwnd, TBPF_INDETERMINATE);
    else
    {
      tbl->lpVtbl->SetProgressState(tbl, tb_hwnd, TBPF_NORMAL);
      tbl->lpVtbl->SetProgressValue(tbl, tb_hwnd, (ULONGLONG)pct, 100);
    }
  }
  return 0;
}

/* The "TaskbarButtonCreated" message (the main window's): Explorer made the taskbar button again, so the progress on it is gone. */
void taskbar_button_created(void)
{
  tp_last_mode = tp_last_pct = -1;
  if (!tb_thread || !tb_event) return;
  InterlockedExchange(&tb_again, 1);
  SetEvent(tb_event);
}

void taskbar_progress(HWND hwnd, int mode, int pct)
{
  int last_mode = tp_last_mode, last_pct = tp_last_pct;

  if (!hwnd) return;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  if (mode == last_mode && (mode != 1 || pct == last_pct)) return;
  tp_last_mode = mode;
  tp_last_pct = pct;

  if (!tb_thread)
  {
    tb_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!tb_event) return;
    tb_thread = CreateThread(NULL, 0, taskbar_thread, NULL, 0, NULL);
    if (!tb_thread) return;
  }
  tb_hwnd = hwnd;
  tb_pct = pct;
  tb_mode = mode;
  SetEvent(tb_event);
}
