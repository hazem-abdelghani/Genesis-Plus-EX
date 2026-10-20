/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
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

static INT_PTR CALLBACK shaderparams_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
    case WM_INITDIALOG:
    {
      int i, n = shader_chain_param_count();
      for (i = 0; i < n; i++) SendDlgItemMessageA(dlg, IDC_SP_LIST, CB_ADDSTRING, 0, (LPARAM)shader_chain_param(i)->desc);
      sp_sel = 0;
      SendDlgItemMessage(dlg, IDC_SP_LIST, CB_SETCURSEL, 0, 0);
      theme_apply_to_window(dlg);
      sp_show(dlg);
      return TRUE;
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORLISTBOX:
    {
      HBRUSH br = theme_ctlcolor((HDC)wp);
      if (br) return (LRESULT)br;
      break;
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
        snprintf(t, sizeof(t), "%g", q->cur);
        SetDlgItemTextA(dlg, IDC_SP_VALUE, t);
      }
      return TRUE;
    }
    case WM_COMMAND:
      switch (LOWORD(wp))
      {
        case IDC_SP_LIST:
          if (HIWORD(wp) == CBN_SELCHANGE) { sp_sel = (int)SendDlgItemMessage(dlg, IDC_SP_LIST, CB_GETCURSEL, 0, 0); sp_show(dlg); }
          return TRUE;
        case IDC_SP_RESET:
        {
          SHADER_PARAM *q = shader_chain_param(sp_sel);
          if (q) { shader_chain_param_set(sp_sel, q->init); sp_show(dlg); }
          return TRUE;
        }
        case IDC_SP_RESETALL:
        {
          int i;
          for (i = 0; i < shader_chain_param_count(); i++) shader_chain_param_set(i, shader_chain_param(i)->init);
          sp_show(dlg);
          return TRUE;
        }
        case IDOK:
        case IDCANCEL:
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
 * Taskbar progress
 ****************************************************************************/

/* The green bar on the program's taskbar button: mode 0 = none, 1 = a bar filled pct percent, 2 = a moving bar (no end
   in sight). Only talks to Windows when the state changes, and does so on a thread of its own: the call goes to the
   desktop shell, and when that is slow to answer (it took seconds when a recording started or ended) the game must
   not wait for it. */
static HWND          tb_hwnd;
static volatile LONG tb_mode = -1, tb_pct;
static HANDLE        tb_event, tb_thread;

static DWORD WINAPI taskbar_thread(LPVOID arg)
{
  ITaskbarList3 *tbl = NULL;
  int last_mode = -1, last_pct = -1;
  (void)arg;

  CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  if (CoCreateInstance(&CLSID_TaskbarList, NULL, CLSCTX_INPROC_SERVER, &IID_ITaskbarList3, (void **)&tbl) == S_OK && tbl)
  {
    if (tbl->lpVtbl->HrInit(tbl) != S_OK) { tbl->lpVtbl->Release(tbl); tbl = NULL; }
  }
  else tbl = NULL;

  for (;;)
  {
    int mode, pct;
    WaitForSingleObject(tb_event, INFINITE);
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

void taskbar_progress(HWND hwnd, int mode, int pct)
{
  static int last_mode = -1, last_pct = -1;

  if (!hwnd) return;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  if (mode == last_mode && (mode != 1 || pct == last_pct)) return;
  last_mode = mode;
  last_pct = pct;

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
