/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  main.c -- window, menu handling, ROM lifecycle and the emulation loop.
 *
 *  Emulation runs on the UI thread from a PeekMessage loop rather than on a
 *  worker thread. That trades a little responsiveness while a menu is open
 *  (Windows runs its own modal loop there, which we handle explicitly) for
 *  having no locking anywhere around the framebuffer or the core's state.
 ****************************************************************************/

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <commdlg.h>
#include <mmsystem.h>

#include "shared.h"
#include "blip_buf.h"
#include "gui.h"
#include "resource.h"
#include "theme.h"
#include "recorder.h"
#include "netplay.h"
#include "sevenzip.h"
#include "toolbar.h"
#include "eggs.h"
#include "coverart.h"
#include "d3d11_video.h"
#include "shader_chain.h"
#include "picocore.h"
#include "eq.h"

/* Required by main.h / the core (sdl/error.c reads log_error). */
int log_error = 0;
int debug_on  = 0;

HWND      g_hwnd;
HWND      g_status;
HINSTANCE g_inst;

int emu_running;
int emu_paused;

int gui_dpi = 96;         /* the display scale of the main window's monitor (96 = 100%) */

/* The system's display scale, for the start (the window's own is known once it is made, and changes when it moves to another monitor). */
static void dpi_init(void)
{
  typedef UINT (WINAPI *GetDpiForSystem_t)(void);
  GetDpiForSystem_t get_dpi = (GetDpiForSystem_t)GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForSystem");

  if (get_dpi) gui_dpi = (int)get_dpi();
  else
  {
    HDC dc = GetDC(NULL);
    if (dc) { gui_dpi = GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(NULL, dc); }
  }
  if (gui_dpi < 96) gui_dpi = 96;
}

/* Shared UI font -- the normal system default at 100%; at another display scale, or (when gui.large_ui is on) a version of it made
   bigger (the height times the scale, times 1.5 for the Large interface), applied uniformly rather than guessing a different amount per control.
   One is kept for the scale it was made for (a font that was in use before the scale changed is left alone: controls may still hold it). */
static HFONT ui_font_base(void)
{
  static HFONT scaled_font;
  static int scaled_dpi;
  HFONT stock = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

  if (!gui.large_ui && gui_dpi == 96) return stock;

  if (!scaled_font || scaled_dpi != gui_dpi)
  {
    LOGFONTA lf;
    HFONT f;
    GetObjectA(stock, sizeof(lf), &lf);
    /* lfHeight is conventionally negative (character height, not cell
       height) -- scaling while preserving sign keeps this correct
       regardless of which convention the stock font happens to use. */
    if (gui.large_ui) lf.lfHeight = (LONG)(lf.lfHeight * 1.5);
    lf.lfHeight = MulDiv(lf.lfHeight, gui_dpi, 96);
    f = CreateFontIndirectA(&lf);
    if (!f) return stock;
    scaled_font = f;
    scaled_dpi = gui_dpi;
  }

  return scaled_font;
}

/* The program's text font: the system one at the style's size (Classic 9 pt; Modern in Segoe UI, a little bigger). */
HFONT gui_get_ui_font(void)
{
  return theme_style_font(ui_font_base());
}

static BOOL CALLBACK dpi_font_child(HWND c, LPARAM unused)
{
  (void)unused;
  RemovePropA(c, "gpx_basefont");                                  /* (the font it kept was made for the old scale) */
  SendMessage(c, WM_SETFONT, (WPARAM)ui_font_base(), FALSE);
  return TRUE;
}

/* The window moved to a monitor with another display scale: the text and the sizes the program works out follow it. */
static void dpi_changed(HWND hwnd, int dpi)
{
  if (dpi < 96 || dpi == gui_dpi) return;
  gui_dpi = dpi;
  EnumChildWindows(hwnd, dpi_font_child, 0);
  theme_apply_to_window(hwnd);                                     /* the style's font and size on top of it */
  browser_density_changed();
  browser_bars_changed();
}

/* Set only when emu_paused was switched on by losing focus, not by the
   person pressing pause themselves -- distinguishes "resume automatically
   when focus comes back" from "leave a deliberate pause alone". */
static int auto_paused_by_focus;
static int paused_by_pad;          /* the game was paused because Player 1's pad went away: it carries on when a pad is back */
static int pm_open, pm_was_paused, pm_sel;      /* the in-game pause menu */
static const char *const pm_items[] = { "Resume", "Save State", "Load State", "Reset", "Quit to the List" };
static int speed_pct = 100;          /* Game Speed: 25 - 400 percent (not saved) */
static int model_current(void);
static int fast_osd_state;           /* the Fast Forward on-screen message last shown (1 = fast) */
static int auto_paused_by_minimize;

static HACCEL g_accel;
static HMENU  g_menu;

/* The menu bar, even while it is detached in fullscreen (settings.c reads the
   options' state from it). */
HMENU gui_main_menu(void) { return g_menu; }

static char rom_path[GUI_PATH_LEN];

/*
 * Filename without directory or extension. Deliberately much shorter than a
 * full path: it gets formatted into window titles, status text and save-file
 * names, and wvsprintf has a hard 1024-byte output limit.
 */
static char rom_base[128];

static int16 soundframe[4096];

static int   in_modal_loop;
static int   menu_popups_open;      /* drop-down menus open now (the game waits only while one is) */
static int   menu_loop_active;      /* a menu is open (the loop of the menu bar or of a drop-down is running) */
static int   menu_rebuild_pending;  /* the Style changed while a menu was open: the menu bar is made again when the menu closes */
static int   menu_keyboard_mode;   /* the menu bar was opened with Alt or F10 and is still open: losing the focus to it is not leaving the program */
static double diag_draw;        /* the slowest drawing of a picture in the last second (logged after a change, with Enable Log) */
static char  vgm_name[160];     /* the file name of the VGM log being recorded */
static int   skip_autosave_once;   /* set by the exit question when "Resume this game next time" is unticked: the game is closed without being kept for Auto Resume */
static int   frames_this_second;
static DWORD fps_tick;
static LARGE_INTEGER perf_freq;
static double next_frame_time;
static double next_rewind_time;
static int rewind_bar_ref;       /* frames of history there were when this rewind began: a full bar */
static int    rewind_hit_limit_notified;

/* Mega CD backup RAM header, used to tell formatted RAM from blank RAM. */
static uint8 brm_format[0x40] =
{
  0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x00,0x00,0x00,0x00,0x40,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x53,0x45,0x47,0x41,0x5f,0x43,0x44,0x5f,0x52,0x4f,0x4d,0x00,0x01,0x00,0x00,0x00,
  0x52,0x41,0x4d,0x5f,0x43,0x41,0x52,0x54,0x52,0x49,0x44,0x47,0x45,0x5f,0x5f,0x5f
};

static const uint16 vc_table[4][2] =
{
  /* NTSC, PAL */
  {0xDA , 0xF2},  /* Mode 4 (192 lines) */
  {0xEA , 0x102}, /* Mode 5 (224 lines) */
  {0xDA , 0xF2},  /* Mode 4 (192 lines) */
  {0x106, 0x10A}  /* Mode 5 (240 lines) */
};

/****************************************************************************
 * Paths
 ****************************************************************************/

/* wsprintf with an output size: wsprintfA has none and would write past a
   path buffer when the folders are deep or the names long. */
void gui_snprintf(char *out, int out_len, const char *fmt, ...)
{
  char buf[1024];        /* wvsprintf's own limit */
  va_list ap;

  va_start(ap, fmt);
  wvsprintfA(buf, fmt, ap);
  va_end(ap);
  lstrcpynA(out, buf, out_len);
}

char *osd_path(const char *filename)
{
  /*
   * Rotating buffers: the core occasionally has one path in flight while
   * building the next, and a single static buffer would corrupt both.
   */
  static char slots[4][GUI_PATH_LEN];
  static int  next;
  static char base[GUI_PATH_LEN];
  static int  base_ready;

  char *out = slots[next];
  next = (next + 1) & 3;

  if (!base_ready)
  {
    char *slash;
    DWORD len = GetModuleFileNameA(NULL, base, sizeof(base) - 1);

    if (len == 0 || len >= sizeof(base) - 1)
    {
      base[0] = '.';
      base[1] = '\0';
    }
    else
    {
      slash = strrchr(base, '\\');
      if (slash) *slash = '\0';
      else lstrcpyA(base, ".");
    }
    base_ready = 1;
  }

  gui_snprintf(out, GUI_PATH_LEN, "%s\\%s", base, filename);
  return out;
}

static void ensure_dir(const char *name)
{
  CreateDirectoryA(osd_path(name), NULL);
}

static void split_basename(const char *path, char *out, int out_len)
{
  const char *start = path;
  const char *slash = strrchr(path, '\\');
  const char *fwd   = strrchr(path, '/');
  const char *dot;
  int n;

  if (fwd > slash) slash = fwd;
  if (slash) start = slash + 1;

  dot = strrchr(start, '.');
  n = dot ? (int)(dot - start) : (int)strlen(start);
  if (n >= out_len) n = out_len - 1;

  memcpy(out, start, (size_t)n);
  out[n] = '\0';
}

/****************************************************************************
 * Status bar and notices
 ****************************************************************************/

static char status_persistent[128];
static void record_stop(void);   /* defined with the frame loop below */

/* Netplay: game data saved during a session belongs to a made-up start state
   (fresh backup RAM on both sides), so it is never written over the player's
   real saves. Cleared when a game is loaded or hard-reset from the real saves. */
static int np_no_save;
static int skip_frames_left;     /* Skip Intro: frames still to run at full speed */
static int backup_exists(int k);
static int np_rom_sha_ready;
static unsigned char np_rom_sha[32];

/* Pane 0 while something is going on that the person should always see:
   "[Netplay: ...] [REC 00:12] Running <game>". */
static void status_apply_prefixes(void)
{
  char buf[400];

  if (!g_status) return;
  buf[0] = '\0';

  if (netplay_state() != NP_IDLE)
  {
    lstrcatA(buf, "[");
    lstrcatA(buf, netplay_status_text());
    lstrcatA(buf, "] ");
  }
  if (recorder_active())
  {
    unsigned s = recorder_seconds();
    char t[40];

    if (s >= 3600) wsprintfA(t, "[REC %u:%02u:%02u] ", s / 3600, (s / 60) % 60, s % 60);
    else           wsprintfA(t, "[REC %02u:%02u] ", s / 60, s % 60);
    lstrcatA(buf, t);
  }
  lstrcatA(buf, status_persistent);
  SendMessageA(g_status, SB_SETTEXTA, 0, (LPARAM)buf);
}
static DWORD status_transient_until;

/* The baseline message pane 0 should show once any transient message
   above it clears -- "Running <game>" or the browsing prompt. */
void gui_status_persistent(const char *fmt, ...)
{
  char buf[1024];        /* wvsprintf's own limit */
  va_list ap;

  va_start(ap, fmt);
  wvsprintfA(buf, fmt, ap);
  va_end(ap);
  lstrcpynA(status_persistent, buf, sizeof(status_persistent));   /* never overruns */

  status_transient_until = 0;
  if (g_status) SendMessageA(g_status, SB_SETTEXTA, 0, (LPARAM)status_persistent);
}

void gui_status(const char *fmt, ...)
{
  char buf[1024];
  va_list ap;

  va_start(ap, fmt);
  wvsprintfA(buf, fmt, ap);
  va_end(ap);

  /* Transient -- reverts to the persistent baseline on its own after a
     few seconds instead of sitting there until something else happens
     to overwrite it, which was the actual bug: there was no revert path
     at all, so whatever the last one-off message was (e.g. "Audio
     settings saved") just stayed there indefinitely. */
  status_transient_until = GetTickCount() + 4000;
  if (g_status) SendMessageA(g_status, SB_SETTEXTA, 0, (LPARAM)buf);
}

static void status_set_fps(int fps);

void gui_pad_lost(const char *name)
{
  gui_log("Player 1's controller went away: %s", name);
  if (!gui.pause_pad || !emu_running || emu_paused || netplay_active()) return;
  emu_paused = 1;
  paused_by_pad = 1;
  auto_paused_by_focus = 0;
  waveout_flush();
  next_frame_time = 0.0;
  gui_update_menu();
  status_set_fps(0);
  gui_notify("Paused: Player 1's controller was disconnected (%s)", name);
}

/* A pad is connected again: a game paused only because Player 1's pad went away carries on. */
void gui_pad_back(const char *name)
{
  if (!paused_by_pad) return;
  paused_by_pad = 0;
  if (!gui.pause_pad || !emu_running || !emu_paused) return;
  gui_diag_start();
  emu_paused = 0;
  next_frame_time = 0.0;
  gui_update_menu();
  gui_notify("Resumed: a controller is connected again (%s)", name && name[0] ? name : "Gamepad");
}

static int notify_count;       /* messages shown so far: a command that showed none gets a generic one */

void gui_notify(const char *fmt, ...)
{
  notify_count++;
  char full[1024];       /* wvsprintf's own limit */
  char buf[256];
  va_list ap;

  va_start(ap, fmt);
  wvsprintfA(full, fmt, ap);
  va_end(ap);
  lstrcpynA(buf, full, sizeof(buf));   /* long names are cut, never overrun */
  gui_log("%s", buf);

  if (gui.osd && emu_running)           /* no game running: the message is for the status bar only (there is no picture to draw it on) */
  {
    char shown[256];
    int i, start = 1;
    /* Title Case: "saved to slot 3" is shown as "Saved to Slot 3" (a, an, the, to, of, in, on, at, by, for, from, and,
       or, as stay small unless they start the message). */
    for (i = 0; buf[i] && i < (int)sizeof(shown) - 1; i++)
    {
      int cap = start && buf[i] >= 'a' && buf[i] <= 'z';
      if (cap && i > 0)
      {
        static const char *const small[] = { "a", "an", "the", "to", "of", "in", "on", "at", "by", "for", "from", "and", "or", "as" };
        int k, n = 0;
        while (buf[i + n] && buf[i + n] != ' ' && buf[i + n] != ')' && buf[i + n] != ',' && buf[i + n] != ':') n++;
        for (k = 0; k < (int)(sizeof(small) / sizeof(small[0])); k++)
          if ((int)lstrlenA(small[k]) == n && !strncmp(buf + i, small[k], (size_t)n)) { cap = 0; break; }
      }
      shown[i] = cap ? (char)(buf[i] - 32) : buf[i];
      start = (buf[i] == ' ' || buf[i] == '(');
    }
    shown[i] = '\0';
    if (i > 56)       /* a long file name: the middle is left out so the message stays within the picture */
    {
      memmove(shown + 27, shown + i - 28, 29);   /* the last 28 characters and the end mark */
      shown[24] = '.'; shown[25] = '.'; shown[26] = '.';
    }
    video_show_notice(shown, (gui.osd_time >= 0 && gui.osd_time < 7 ? gui.osd_time : 2) * 1000 + 1000);
  }
  gui_status("%s", buf);
}

static void status_set_slot(void)
{
  char buf[32];
  wsprintfA(buf, "Slot %d", gui.state_slot);
  if (g_status) SendMessageA(g_status, SB_SETTEXTA, 1, (LPARAM)buf);
}

/* Lets the browser panel show the game count in the same pane Slot N
   normally occupies -- the slot number means nothing without a ROM
   running anyway, so the two never need the space at the same time. */
static void layout_status(void);
static int status_slot_w;       /* what the game count needs */

/* The width of a text in the status bar's own font. */
static int status_px(const char *text)
{
  HDC dc;
  HFONT f, old = NULL;
  SIZE sz = { 0, 0 };

  if (!g_status) return 0;
  dc = GetDC(g_status);
  if (!dc) return 0;
  f = (HFONT)SendMessage(g_status, WM_GETFONT, 0, 0);
  if (f) old = (HFONT)SelectObject(dc, f);
  GetTextExtentPoint32A(dc, text, lstrlenA(text), &sz);
  if (old) SelectObject(dc, old);
  ReleaseDC(g_status, dc);
  return sz.cx;
}

void gui_status_slot(const char *text)
{
  int w;

  if (!g_status) return;
  w = status_px(text) + DPIS(24);              /* the pane grows to fit the text, whatever the screen's scaling: it was cut off */
  if (w != status_slot_w) { status_slot_w = w; layout_status(); }
  SendMessageA(g_status, SB_SETTEXTA, 1, (LPARAM)text);
}

static int status_last_fps;

static void status_set_fps(int fps)
{
  static char last_buf[32], last_speed[16];
  char buf[32], speed[16];

  status_last_fps = fps;

  speed[0] = '\0';
  if (emu_running && !emu_paused)
  {
    double nominal = emu_frame_rate();      /* the speed as a percentage of the console's own */
    wsprintfA(buf, "%d FPS", fps);
    wsprintfA(speed, "%d%%", nominal > 0 ? (int)(fps * 100.0 / nominal + 0.5) : 100);
  }
  else lstrcpyA(buf, emu_running ? "Paused" : "");

  if (lstrcmpA(buf, last_buf) != 0)
  {
    lstrcpyA(last_buf, buf);
    if (g_status) SendMessageA(g_status, SB_SETTEXTA, 2, (LPARAM)buf);
  }
  if (lstrcmpA(speed, last_speed) != 0)
  {
    lstrcpyA(last_speed, speed);
    if (g_status) SendMessageA(g_status, SB_SETTEXTA, 3, (LPARAM)speed);
  }
}

/*
 * Remembers where the window is so it can reopen there next time. Skipped
 * during fullscreen (that geometry is a full-monitor rect, not a window
 * position) and while minimized (GetWindowRect would just capture the
 * taskbar-parked position, overwriting the last real one for no reason).
 */
static void capture_window_geometry(void)
{
  if (gui.fullscreen) return;
  if (IsIconic(g_hwnd)) return;

  gui.window_maximized = IsZoomed(g_hwnd) ? 1 : 0;

  if (!gui.window_maximized)
  {
    RECT r;
    GetWindowRect(g_hwnd, &r);
    gui.window_x = r.left;
    gui.window_y = r.top;
  }
}

static void layout_status(void)
{
  RECT rc;
  int parts[4];

  if (!g_status) return;

  if (gui.large_ui) SendMessage(g_status, SB_SETMINHEIGHT, DPIS(30), 0);     /* (follows the display scale when the window moves) */
  SendMessage(g_status, WM_SIZE, 0, 0);
  GetClientRect(g_hwnd, &rc);

  /* Middle pane widened from its original "Slot N"-only sizing -- it now
     also carries the browser panel's game count, which runs noticeably
     longer ("15 of 934 games", "999+ games found (list capped)"). */
  {
    int w3 = status_px("9999%") + DPIS(24);       /* the speed, in a pane of its own */
    int w2 = status_px("9999 FPS") + DPIS(24);    /* the frame rate */
    int w1 = status_px("Slot 0") + DPIS(24);      /* the slot, or the game count */
    if (w3 < DPIS(60)) w3 = DPIS(60);
    if (w2 < DPIS(70)) w2 = DPIS(70);
    if (w1 < DPIS(150)) w1 = DPIS(150);
    if (status_slot_w > w1) w1 = status_slot_w;
    parts[2] = rc.right - w3;
    parts[1] = parts[2] - w2;
    parts[0] = parts[1] - w1;
  }
  parts[3] = -1;
  if (parts[0] < 0) parts[0] = 0;
  if (parts[1] < parts[0]) parts[1] = parts[0];
  if (parts[2] < parts[1]) parts[2] = parts[1];

  SendMessage(g_status, SB_SETPARTS, 4, (LPARAM)parts);
}

/* Same area video.c's present() renders into -- client area minus the
   status bar, when it's visible. The browser panel occupies exactly this
   region so nothing black shows through around its edges. */
static void get_content_rect(RECT *out)
{
  GetClientRect(g_hwnd, out);
  out->top += browser_toolbar_height();   /* the toolbar is above the picture / the browser */

  if (g_status && IsWindowVisible(g_status))
  {
    RECT sb;
    GetWindowRect(g_status, &sb);
    out->bottom -= (sb.bottom - sb.top);
  }
  if (out->bottom < out->top) out->bottom = out->top;
}

/* Lets browser.c (which can't see the static get_content_rect above)
   trigger a relayout of its own controls -- needed after picking a folder
   from the "Choose ROM Directory" button, which switches the panel from that
   button to the normal search+list view without a window resize to
   otherwise trigger it. */
void browser_relayout(void)
{
  RECT content;
  get_content_rect(&content);
  browser_panel_layout(&content);
}

/****************************************************************************
 * Window title
 ****************************************************************************/

static void update_title(void)
{
  WCHAR title[600], name[120];
  const char *hdr = NULL;

  if (emu_running)
  {
    const char *n = (rominfo.international[0] != 0x20 && rominfo.international[0]) ? rominfo.international : rominfo.domestic;
    if (n && n[0] && n[0] != 0x20) hdr = n;
  }

  name[0] = 0;
  if (hdr)
  {
    char trimmed[64];
    int i;
    lstrcpynA(trimmed, hdr, sizeof(trimmed));
    for (i = lstrlenA(trimmed) - 1; i >= 0 && trimmed[i] == ' '; i--) trimmed[i] = '\0';
    /* the header is Shift-JIS: Japanese titles show as they are written */
    if (trimmed[0]) MultiByteToWideChar(932, 0, trimmed, -1, name, 120);
  }
  if (emu_running && !name[0]) MultiByteToWideChar(CP_ACP, 0, rom_base, -1, name, 120);

  if (emu_running) wsprintfW(title, L"%s - " APP_NAME_W, name);
  else lstrcpyW(title, APP_NAME_W);
  SetWindowTextW(g_hwnd, title);
}

/****************************************************************************
 * Menu state
 ****************************************************************************/

static int cover_size_index(void);

static void check_radio(HMENU menu, int base, int count, int selected)
{
  CheckMenuRadioItem(menu, base, base + count - 1, base + selected, MF_BYCOMMAND);
}

/* Empties a menu. With the Large interface the items are owner-drawn and each carries a
   malloc'd copy of its text in itemData (see theme.c), which has to be freed
   here or every rebuild leaks it. */
static void clear_menu(HMENU menu)
{
  while (GetMenuItemCount(menu) > 0)
  {
    MENUITEMINFOA mii;

    memset(&mii, 0, sizeof(mii));
    mii.cbSize = sizeof(mii);
    mii.fMask  = MIIM_FTYPE | MIIM_DATA;
    {
      HMENU sub = GetSubMenu(menu, 0);
      if (sub) clear_menu(sub);                     /* its own texts first (the Large interface keeps one for every item) */
    }
    if (GetMenuItemInfoA(menu, 0, TRUE, &mii) && (mii.fType & MFT_OWNERDRAW) && mii.dwItemData)
      { theme_menu_forget(mii.dwItemData); free((void *)mii.dwItemData); }     /* (its picture must not stay for the next text at that address) */
    DeleteMenu(menu, 0, MF_BYPOSITION);
  }
}

static void rebuild_recent_menu(void)
{
  HMENU file_menu = GetSubMenu(g_menu, 0);
  /* The File menu's only submenu is Recent Files; found by that, not by a position that moves. */
  HMENU recent = NULL;
  int i, added = 0;

  for (i = 0; file_menu && i < GetMenuItemCount(file_menu) && !recent; i++)
    recent = GetSubMenu(file_menu, i);

  if (!recent) return;

  clear_menu(recent);

  for (i = 0; i < GUI_RECENT_MAX; i++)
  {
    char label[GUI_PATH_LEN + 64];
    char name[GUI_PATH_LEN];
    const char *console;

    if (!gui.recent[i][0]) continue;

    split_basename(gui.recent[i], name, sizeof(name));
    { char full[GUI_PATH_LEN], shown[GUI_PATH_LEN]; wsprintfA(full, "%s.x", name); browser_display_name(full, shown, sizeof(shown)); lstrcpyA(name, shown); }      /* Show Clean Names applies here too */

    /* "Sonic The Hedgehog        Mega Drive" (the console on the right): the console's short name, i.e. the
       part before " / " ("Mega Drive / Genesis" -> "Mega Drive"). */
    console = browser_console_for_path(gui.recent[i]);
    if (console && console[0])
    {
      char shortname[48];
      char *slash;

      lstrcpynA(shortname, console, sizeof(shortname));
      slash = strstr(shortname, " / ");
      if (slash) *slash = '\0';
      wsprintfA(label, "%s%d.%s%s\t%s", added + 1 < 10 ? "&" : "", added + 1, added + 1 < 10 ? "     " : "   ", name, shortname);      /* after the tab: the right-hand column, where shortcuts go */
    }
    else
    {
      wsprintfA(label, "%s%d.%s%s", added + 1 < 10 ? "&" : "", added + 1, added + 1 < 10 ? "     " : "   ", name);
    }
    AppendMenuA(recent, MF_STRING, (UINT_PTR)(IDM_FILE_RECENT_BASE + i), label);
    added++;
  }

  if (!added)
  {
    AppendMenuA(recent, MF_STRING | MF_GRAYED, (UINT_PTR)IDM_FILE_RECENT_BASE, "(Nothing Yet)");
  }
  else
  {
    AppendMenuA(recent, MF_SEPARATOR, 0, NULL);
    AppendMenuA(recent, MF_STRING, (UINT_PTR)IDM_FILE_RECENT_CLEAR, "&Clear Recent Files");
  }

  if (theme_menus_owner()) theme_ownerdraw_menu(recent);

  DrawMenuBar(g_hwnd);
}

/* Room reserved for filter entries: IDM_VIDEO_FILTER_BASE .. +47. The menu
   command for entry i is IDM_VIDEO_FILTER_BASE + i, where i is the filter's
   index in filters.c's table. */
#define FILTER_MENU_MAX 48

/* Finds the "Video" top-level menu by what's actually in it (a known,
   always-direct-child item) rather than by position -- a hardcoded
   position here is exactly the same fragility find_render_filter_menu
   already works around one level down, and broke the same way when
   the View menu was inserted before Video, shifting its position. */
static HMENU find_video_menu(void)
{
  int i, count = GetMenuItemCount(g_menu);

  for (i = 0; i < count; i++)
  {
    HMENU sub = GetSubMenu(g_menu, i);
    if (sub && GetMenuState(sub, IDM_VIDEO_FULLSCREEN, MF_BYCOMMAND) != (UINT)-1) return sub;
  }
  return NULL;
}

/* Finds "Render Filter" by what's actually in it (its first item's command
   ID never changes) rather than by position -- a hardcoded position here
   broke silently the last time an item got added above it in the Video
   menu, and rebuilt the wrong submenu's contents without any error. */
static HMENU find_render_filter_menu(HMENU video_menu)
{
  int i, count = GetMenuItemCount(video_menu);

  for (i = 0; i < count; i++)
  {
    HMENU sub = GetSubMenu(video_menu, i);
    if (sub && GetMenuItemID(sub, 0) == (UINT)IDM_VIDEO_FILTER_NONE) return sub;
  }
  return NULL;
}

static void rebuild_filter_menu(void)
{
  HMENU video_menu = find_video_menu();
  HMENU filters = video_menu ? find_render_filter_menu(video_menu) : NULL;
  int current, count, i;

  if (!filters) return;

  clear_menu(filters);

  current = video_filter_current();
  count   = video_filter_count();
  if (count > FILTER_MENU_MAX) count = FILTER_MENU_MAX;

  AppendMenuA(filters, MF_STRING, (UINT_PTR)IDM_VIDEO_FILTER_NONE, "None");

  if (count > 0)
  {
    /* Sorted by name (the table in filters.c is), the families in a sub-menu of their own; a filter with no family of its
       own is listed directly. */
    HMENU groups[24];
    const char *gname[24];
    int ng = 0, k;

    AppendMenuA(filters, MF_SEPARATOR, 0, NULL);
    for (i = 0; i < count; i++)
    {
      const char *g = video_filter_group(i);
      HMENU target = filters;

      if (g)
      {
        for (k = 0; k < ng; k++) if (!lstrcmpA(gname[k], g)) break;
        if (k == ng && ng < 24)
        {
          groups[ng] = CreatePopupMenu();
          gname[ng] = g;
          AppendMenuA(filters, MF_POPUP | MF_STRING, (UINT_PTR)groups[ng], g);
          ng++;
        }
        if (k < ng) target = groups[k];
      }
      AppendMenuA(target, MF_STRING, (UINT_PTR)(IDM_VIDEO_FILTER_BASE + i), video_filter_name(i));
    }

    for (k = 0; k < ng; k++)
      if (current >= 0 && current < count)
        CheckMenuRadioItem(groups[k], (UINT)(IDM_VIDEO_FILTER_BASE + current), (UINT)(IDM_VIDEO_FILTER_BASE + current), (UINT)(IDM_VIDEO_FILTER_BASE + current), MF_BYCOMMAND);      /* (a dot, not a check mark: one filter is on) */
  }

  if (current < 0 || current >= count)
  {
    CheckMenuRadioItem(filters, IDM_VIDEO_FILTER_NONE, IDM_VIDEO_FILTER_NONE, IDM_VIDEO_FILTER_NONE, MF_BYCOMMAND);
  }
  else
  {
    CheckMenuRadioItem(filters, (UINT)(IDM_VIDEO_FILTER_BASE + current), (UINT)(IDM_VIDEO_FILTER_BASE + current), (UINT)(IDM_VIDEO_FILTER_BASE + current), MF_BYCOMMAND);
  }

  if (theme_menus_owner()) theme_ownerdraw_menu(filters);

  DrawMenuBar(g_hwnd);
}

static int system_menu_index(void)
{
  switch (config.system)
  {
    case SYSTEM_SG:           return 1;
    case SYSTEM_SGII:         return 2;
    case SYSTEM_SGII_RAM_EXT: return 3;
    case SYSTEM_MARKIII:      return 4;
    case SYSTEM_SMS:          return 5;
    case SYSTEM_SMS2:         return 6;
    case SYSTEM_GG:           return 7;
    case SYSTEM_MD:           return 8;
    default:                  return 0;
  }
}

#define PORT_CHOICES 13

/* What each choice of the Port A / Port B lists plugs in. 4-Way Play uses both ports at once. */
static const int port_table_a[PORT_CHOICES] = { NO_SYSTEM, SYSTEM_GAMEPAD, SYSTEM_MOUSE, SYSTEM_XE_1AP,
                                                SYSTEM_ACTIVATOR, SYSTEM_LIGHTPHASER, SYSTEM_PADDLE,
                                                SYSTEM_SPORTSPAD, SYSTEM_GRAPHIC_BOARD, SYSTEM_TEAMPLAYER,
                                                SYSTEM_WAYPLAY, SYSTEM_MASTERTAP, SYSTEM_SMASH };
static const int port_table_b[PORT_CHOICES] = { NO_SYSTEM, SYSTEM_GAMEPAD, SYSTEM_MOUSE, SYSTEM_MENACER,
                                                SYSTEM_JUSTIFIER, SYSTEM_XE_1AP, SYSTEM_ACTIVATOR,
                                                SYSTEM_LIGHTPHASER, SYSTEM_PADDLE, SYSTEM_TEAMPLAYER,
                                                SYSTEM_WAYPLAY, SYSTEM_MASTERTAP, SYSTEM_SMASH };

static int port_menu_index(int port)
{
  const int *table = port ? port_table_b : port_table_a;
  int i;

  for (i = 0; i < PORT_CHOICES; i++)
  {
    if (table[i] == input.system[port]) return i;
  }
  return 1;
}

static int port_menu_value(int port, int index)
{
  if (index < 0 || index >= PORT_CHOICES) return SYSTEM_GAMEPAD;
  return port ? port_table_b[index] : port_table_a[index];
}

/* A device is chosen for a port. 4-Way Play takes both ports; leaving it gives the other port a pad again. */
static void port_choose(int port, int value)
{
  int other = 1 - port;

  if (value == SYSTEM_WAYPLAY) input.system[0] = input.system[1] = SYSTEM_WAYPLAY;
  else
  {
    if (input.system[other] == SYSTEM_WAYPLAY) input.system[other] = SYSTEM_GAMEPAD;
    input.system[port] = (uint8)value;
  }
  /* remembered: only what was chosen here (what a game sets for itself on the other port is not) */
  if (value == SYSTEM_WAYPLAY) gui_user_ports[0] = gui_user_ports[1] = SYSTEM_WAYPLAY;
  else
  {
    if (gui_user_ports[other] == SYSTEM_WAYPLAY) gui_user_ports[other] = SYSTEM_GAMEPAD;
    gui_user_ports[port] = (uint8)value;
  }
}

/* A game is about to load: the ports go back to the user's choice (the last game may have put its own device there, such as
   a light gun). The core then applies what it knows about the new game. */
static void ports_for_new_game(void)
{
  input.system[0] = gui_user_ports[0];
  input.system[1] = gui_user_ports[1];
}

/* A Mega Drive or Mega CD game cannot be played with the Master System's Light Phaser, Paddle, Sports Pad or Master Tap (they stay
   chosen for the games that use them): a pad goes there for this game, as the console expects. */
static void ports_fit_system(void)
{
  int port;

  if (system_hw != SYSTEM_MCD && (system_hw & SYSTEM_PBC) != SYSTEM_MD) return;
  for (port = 0; port < 2; port++)
  {
    int d = input.system[port];
    if (d == SYSTEM_LIGHTPHASER || d == SYSTEM_PADDLE || d == SYSTEM_SPORTSPAD || d == SYSTEM_MASTERTAP)
    {
      gui_log("Input: port %c had a device for the Master System (%d); a Control Pad is used for this game", port ? 'B' : 'A', d);
      input.system[port] = SYSTEM_GAMEPAD;
    }
  }
}

/* Finds the "Select State Slot" submenu by the first command ID it holds, rather
   than by position or by which top-level menu it lives in, so moving it
   between menus (it used to be under File, now Emulation) doesn't break
   this. */
/* Which of the three slot lists (Select / Load from / Save to) this menu is: the command it starts with, or 0. */
static int slot_menu_base(HMENU m)
{
  UINT first;
  if (!m || GetMenuItemCount(m) < 1) return 0;
  first = GetMenuItemID(m, 0);
  if (first == (UINT)IDM_FILE_SLOT_BASE || first == (UINT)IDM_FILE_LOADSLOT_BASE || first == (UINT)IDM_FILE_SAVESLOT_BASE) return (int)first;
  return 0;
}

static HMENU find_slot_menu_in(HMENU menu, int base)
{
  int i, count = GetMenuItemCount(menu);

  for (i = 0; i < count; i++)
  {
    HMENU sub = GetSubMenu(menu, i), found;
    if (!sub) continue;
    if (slot_menu_base(sub) == base) return sub;
    found = find_slot_menu_in(sub, base);
    if (found) return found;
  }
  return NULL;
}

static HMENU find_slot_menu(int base)
{
  return find_slot_menu_in(g_menu, base);
}

/* Sets an item's text. With large_ui the menu is owner-drawn and paints
   from the string stored in itemData (see theme.c), so that has to be
   replaced too or the old text would keep showing. */
static void menu_set_text(HMENU menu, UINT id, const char *text)
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
    theme_menu_text_moved(mii.dwItemData, (ULONG_PTR)stored);       /* (its picture goes with it) */
    if (mii.dwItemData) free((void *)mii.dwItemData);
    mii.fMask      = MIIM_FTYPE | MIIM_DATA;                          /* (the type again: the menu measures its items anew for the longer text) */
    mii.dwItemData = (ULONG_PTR)stored;
  }
  else
  {
    mii.fMask      = MIIM_STRING;
    mii.dwTypeData = (LPSTR)text;
  }
  SetMenuItemInfoA(menu, id, FALSE, &mii);
}

/* "Slot 0 - hh:mm:ss  DD/MM/YYYY" for each slot that holds a save. */
static void update_slot_menu_labels(HMENU slot_menu, int base)
{
  int i;

  if (!slot_menu) return;

  for (i = 0; i < GUI_SLOT_MAX; i++)
  {
    char path[GUI_PATH_LEN], stamp[32], label[64];

    if (!emu_running)
    {
      wsprintfA(label, "Slot &%d", i);
    }
    else
    {
      state_path(i, path, sizeof(path));
      if (state_file_time(path, stamp, sizeof(stamp)))
        wsprintfA(label, "Slot &%d - %s", i, stamp);
      else
        wsprintfA(label, "Slot &%d - Empty", i);
    }
    {
      char keys[64];
      hotkey_menu_keys(base + i, keys, sizeof(keys));
      if (keys[0] && lstrlenA(label) + lstrlenA(keys) + 2 < (int)sizeof(label)) { lstrcatA(label, "\t"); lstrcatA(label, keys); }
    }
    menu_set_text(slot_menu, base + i, label);
    if (base == IDM_FILE_LOADSLOT_BASE)      /* nothing to load from an empty slot */
    {
      int has = 0;
      if (emu_running) { state_path(i, path, sizeof(path)); has = state_file_time(path, stamp, sizeof(stamp)); }
      EnableMenuItem(slot_menu, base + i, MF_BYCOMMAND | (has ? MF_ENABLED : (MF_GRAYED | MF_DISABLED)));
    }
    else if (base == IDM_FILE_SAVESLOT_BASE)
      EnableMenuItem(slot_menu, base + i, MF_BYCOMMAND | (emu_running ? MF_ENABLED : (MF_GRAYED | MF_DISABLED)));
  }
}

int gui_game_running(void) { return emu_running; }

/* A submenu whose items are all grayed is grayed itself (Record, with no game running). Returns 1 when every item of `menu` is. */
static int menu_autogray(HMENU menu)
{
  int i, n = GetMenuItemCount(menu), items = 0, usable = 0;

  for (i = 0; i < n; i++)
  {
    MENUITEMINFOA mii;
    HMENU sub = GetSubMenu(menu, i);

    ZeroMemory(&mii, sizeof(mii));
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_FTYPE | MIIM_STATE;
    if (!GetMenuItemInfoA(menu, (UINT)i, TRUE, &mii) || (mii.fType & MFT_SEPARATOR)) continue;
    items++;
    if (sub)
    {
      int all = menu_autogray(sub);
      EnableMenuItem(menu, (UINT)i, MF_BYPOSITION | (all ? (MF_GRAYED | MF_DISABLED) : MF_ENABLED));
      if (!all) usable++;
    }
    else if (!(mii.fState & (MFS_GRAYED | MFS_DISABLED))) usable++;
  }
  return items > 0 && usable == 0;
}

/* What does nothing for a 32X game (PicoDrive runs it, not the Genesis Plus GX core): these are grayed out in the menus and in
   Settings while one is running, and refused when a shortcut asks for them. */
/* The sample rate the running game's sound is made at: a 32X game keeps the rate it was started with (a new choice waits for
   the next game), the others follow the setting at once. */
static int run_rate_now(void)
{
  return pico_active() && pico_sample_rate() > 0 ? pico_sample_rate() : gui.sample_rate;
}

int gui_pico_unsupported(int id)
{
  if (!pico_active()) return 0;

  /* (Force VDP Mode, Console, Borders, Interlaced Mode and High-Quality PSG stay available: they are kept for the games of the other systems.) */
  if (id >= IDM_EMU_LOCKON_BASE && id < IDM_EMU_LOCKON_BASE + 4) return 1;
  if (id == IDM_EMU_BIOS || id == IDM_EMU_ADDRERROR || id == IDM_EMU_CDLATENCY) return 1;
  if (id >= IDM_SET_FMUNIT_BASE && id < IDM_SET_FMUNIT_BASE + 3) return 1;
  if (id >= IDM_SET_OPLL_BASE && id < IDM_SET_OPLL_BASE + 2) return 1;
  if (id == IDM_VIDEO_SMSBORDER || id == IDM_VIDEO_GGEXTRA) return 1;
  if (id >= IDM_INPUT_PORTA_BASE && id < IDM_INPUT_PORTA_BASE + PORT_CHOICES) return 1;
  if (id >= IDM_INPUT_PORTB_BASE && id < IDM_INPUT_PORTB_BASE + PORT_CHOICES) return 1;
  if (id == IDM_INPUT_GUNCURSOR || id == IDM_INPUT_INVERTMOUSE) return 1;
  if (id >= IDM_SET_MOUSESENS_BASE && id < IDM_SET_MOUSESENS_BASE + 5) return 1;
  if (id >= IDM_SET_GUNX_BASE && id < IDM_SET_GUNX_BASE + 7) return 1;
  if (id >= IDM_SET_GUNY_BASE && id < IDM_SET_GUNY_BASE + 7) return 1;
  return 0;
}

/* The option only a 32X game uses (Mute PWM Sound): it means nothing while a game of another system runs. With no game open it stays
   available. */
int gui_pico_only(int id)
{
  if (!emu_running || pico_active()) return 0;
  return id == IDM_AUDIO_MUTE_PWM;
}

void gui_update_menu(void)
{
  UINT rom_state = emu_running ? MF_ENABLED : (MF_GRAYED | MF_DISABLED);
  UINT browsing_state = emu_running ? (MF_GRAYED | MF_DISABLED) : MF_ENABLED;

  if (!g_menu) return;

  EnableMenuItem(g_menu, IDM_VIEW_LIST, MF_BYCOMMAND | browsing_state);
  EnableMenuItem(g_menu, IDM_VIEW_GRID, MF_BYCOMMAND | browsing_state);
  {
    /* The whole Game List menu is for the list, which is not showing while a game runs. */
    static const int ids[] = { IDM_VIEW_DENSITY_BASE, IDM_VIEW_DENSITY_BASE + 1, IDM_VIEW_DENSITY_BASE + 2, IDM_VIEW_COL_FAV,
      IDM_VIEW_COL_BASE, IDM_VIEW_COL_BASE + 1, IDM_VIEW_COL_EXTRA, IDM_VIEW_COL_EXTRA + 1, IDM_VIEW_COL_EXTRA + 2, IDM_VIEW_COL_EXTRA + 3,
      IDM_VIEW_FAVONLY, IDM_VIEW_SORT_RECENT, IDM_VIEW_CLEANNAMES, IDM_VIEW_GRIDLINES, IDM_VIEW_GROUPCONSOLE, IDM_VIEW_AZSTRIP };
    size_t gi;
    int k;
    for (gi = 0; gi < sizeof(ids) / sizeof(ids[0]); gi++) EnableMenuItem(g_menu, ids[gi], MF_BYCOMMAND | browsing_state);
    /* The grid's cover sizes: only with the grid view chosen (and no game running). */
    for (k = 0; k < 7; k++)
      EnableMenuItem(g_menu, IDM_SET_COVERSIZE_BASE + k, MF_BYCOMMAND | (gui.browser_grid_view ? browsing_state : (MF_GRAYED | MF_DISABLED)));
  }

  EnableMenuItem(g_menu, IDM_FILE_CLOSE, MF_BYCOMMAND | rom_state);
  EnableMenuItem(g_menu, IDM_EMU_STOP, MF_BYCOMMAND | rom_state);
  EnableMenuItem(g_menu, IDM_FILE_ROMINFO, MF_BYCOMMAND | ((!emu_running && browser_has_selection()) ? MF_ENABLED : rom_state));
  EnableMenuItem(g_menu, IDM_FILE_APPLYPATCH, MF_BYCOMMAND | rom_state);
  EnableMenuItem(g_menu, IDM_EMU_SPEEDDOWN, MF_BYCOMMAND | rom_state);       /* Game Speed: only while a game runs */
  EnableMenuItem(g_menu, IDM_EMU_SPEEDRESET, MF_BYCOMMAND | rom_state);
  EnableMenuItem(g_menu, IDM_EMU_SPEEDUP, MF_BYCOMMAND | rom_state);
  EnableMenuItem(g_menu, IDM_FILE_SAVESTATE, MF_BYCOMMAND | rom_state);
  EnableMenuItem(g_menu, IDM_FILE_LOADSTATE, MF_BYCOMMAND | rom_state);
  EnableMenuItem(g_menu, IDM_EMU_UNDOSAVE, MF_BYCOMMAND |
                 ((emu_running && emu_can_undo_save()) ? MF_ENABLED : (MF_GRAYED | MF_DISABLED)));
  EnableMenuItem(g_menu, IDM_EMU_UNDOLOAD, MF_BYCOMMAND |
                 ((emu_running && emu_can_undo_load()) ? MF_ENABLED : (MF_GRAYED | MF_DISABLED)));
  EnableMenuItem(g_menu, IDM_FILE_STATEMGR, MF_BYCOMMAND | rom_state);
  {
    int b;
    for (b = 0; b < 3; b++)
      EnableMenuItem(g_menu, IDM_FILE_LOADBACKUP_BASE + b, MF_BYCOMMAND | ((emu_running && !netplay_active() && backup_exists(b)) ? MF_ENABLED : (MF_GRAYED | MF_DISABLED)));
  }
  EnableMenuItem(g_menu, IDM_FILE_LOADSTATEFILE, MF_BYCOMMAND | rom_state);
  EnableMenuItem(g_menu, IDM_FILE_SAVESTATEFILE, MF_BYCOMMAND | rom_state);
  EnableMenuItem(g_menu, IDM_EMU_INSERTDISC, MF_BYCOMMAND | ((emu_running && system_hw == SYSTEM_MCD) ? MF_ENABLED : (MF_GRAYED | MF_DISABLED)));
  EnableMenuItem(g_menu, IDM_FILE_SCREENSHOT, MF_BYCOMMAND | rom_state);
  {
    int rec = recorder_active(), vgm = vgm_active();

    EnableMenuItem(g_menu, IDM_FILE_REC_VIDEO,
                   MF_BYCOMMAND | ((emu_running && !vgm && (rec == REC_NONE || rec == REC_VIDEO)) ? MF_ENABLED : (MF_GRAYED | MF_DISABLED)));
    EnableMenuItem(g_menu, IDM_FILE_REC_AUDIO,
                   MF_BYCOMMAND | ((emu_running && !vgm && (rec == REC_NONE || rec == REC_AUDIO)) ? MF_ENABLED : (MF_GRAYED | MF_DISABLED)));
    EnableMenuItem(g_menu, IDM_FILE_REC_GIF,
                   MF_BYCOMMAND | ((emu_running && !vgm && (rec == REC_NONE || rec == REC_GIF)) ? MF_ENABLED : (MF_GRAYED | MF_DISABLED)));
    EnableMenuItem(g_menu, IDM_TOOLS_VGM,
                   MF_BYCOMMAND | ((emu_running && !vgm && rec == REC_NONE) ? MF_ENABLED : (MF_GRAYED | MF_DISABLED)));      /* like the others it is started here and ended with Stop Recording */
    CheckMenuItem(g_menu, IDM_FILE_REC_GIF, MF_BYCOMMAND | (rec == REC_GIF ? MF_CHECKED : MF_UNCHECKED));
    EnableMenuItem(g_menu, IDM_FILE_REC_STOP,
                   MF_BYCOMMAND | ((rec != REC_NONE || vgm) ? MF_ENABLED : (MF_GRAYED | MF_DISABLED)));
    CheckMenuItem(g_menu, IDM_FILE_REC_VIDEO, MF_BYCOMMAND | (rec == REC_VIDEO ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(g_menu, IDM_FILE_REC_AUDIO, MF_BYCOMMAND | (rec == REC_AUDIO ? MF_CHECKED : MF_UNCHECKED));
  }
  EnableMenuItem(g_menu, IDM_TOOLS_NETPLAY,
                 MF_BYCOMMAND | ((emu_running && netplay_state() == NP_IDLE) ? MF_ENABLED : (MF_GRAYED | MF_DISABLED)));
  EnableMenuItem(g_menu, IDM_TOOLS_NETPLAY_STOP,
                 MF_BYCOMMAND | (netplay_state() != NP_IDLE ? MF_ENABLED : (MF_GRAYED | MF_DISABLED)));
  EnableMenuItem(g_menu, IDM_EMU_PAUSE, MF_BYCOMMAND | rom_state);
  EnableMenuItem(g_menu, IDM_EMU_RESET, MF_BYCOMMAND | rom_state);
  EnableMenuItem(g_menu, IDM_EMU_HARDRESET, MF_BYCOMMAND | rom_state);
  EnableMenuItem(g_menu, IDM_EMU_CHEATS, MF_BYCOMMAND | ((!emu_running && browser_has_selection()) ? MF_ENABLED : rom_state));
  EnableMenuItem(g_menu, IDM_EMU_CHEATSEARCH, MF_BYCOMMAND | rom_state);
  EnableMenuItem(g_menu, IDM_VIDEO_FULLSCREEN, MF_BYCOMMAND | rom_state);

  CheckMenuItem(g_menu, IDM_EMU_PAUSE,
                MF_BYCOMMAND | (emu_paused ? MF_CHECKED : MF_UNCHECKED));

  update_slot_menu_labels(find_slot_menu(IDM_FILE_SLOT_BASE), IDM_FILE_SLOT_BASE);
  update_slot_menu_labels(find_slot_menu(IDM_FILE_LOADSLOT_BASE), IDM_FILE_LOADSLOT_BASE);
  update_slot_menu_labels(find_slot_menu(IDM_FILE_SAVESLOT_BASE), IDM_FILE_SAVESLOT_BASE);
  check_radio(g_menu, IDM_FILE_SLOT_BASE, GUI_SLOT_MAX, gui.state_slot);
  check_radio(g_menu, IDM_EMU_REGION_BASE, 5, config.region_detect);
  check_radio(g_menu, IDM_EMU_VDPMODE_BASE, 3, config.vdp_mode);
  check_radio(g_menu, IDM_VIEW_DENSITY_BASE, 3, gui.row_density);
  check_radio(g_menu, IDM_SET_FSMODE_BASE, 2, gui.fs_exclusive);
  { int k; for (k = 0; k < 3; k++) EnableMenuItem(g_menu, IDM_VIEW_DENSITY_BASE + k, MF_BYCOMMAND | ((emu_running || gui.browser_grid_view) ? (MF_GRAYED | MF_DISABLED) : MF_ENABLED)); }
  check_radio(g_menu, IDM_EMU_REWINDMEM_BASE, 4, gui.rewind_memory);
  check_radio(g_menu, IDM_EMU_SYSTEM_BASE, 9, system_menu_index());
  check_radio(g_menu, IDM_EMU_LOCKON_BASE, 4, config.lock_on);

  CheckMenuItem(g_menu, IDM_EMU_BIOS,
                MF_BYCOMMAND | (config.bios ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_EMU_ADDRERROR,
                MF_BYCOMMAND | (config.addr_error ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_EMU_CDLATENCY,
                MF_BYCOMMAND | (config.cd_latency ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_INPUT_GUNCURSOR,
                MF_BYCOMMAND | (config.gun_cursor[0] ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_INPUT_INVERTMOUSE,
                MF_BYCOMMAND | (config.invert_mouse ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_EMU_PAUSE_UNFOCUSED,
                MF_BYCOMMAND | (gui.pause_on_focus_loss ? MF_CHECKED : MF_UNCHECKED));

  check_radio(g_menu, IDM_VIEW_LIST, 2, gui.browser_grid_view);
  check_radio(g_menu, IDM_SET_COVERSIZE_BASE, 7, cover_size_index());
  check_radio(g_menu, IDM_SET_TBSIZE_BASE, 3, gui.toolbar_size);
  {
    int c;
    for (c = 0; c < 2; c++)
      CheckMenuItem(g_menu, IDM_VIEW_COL_BASE + c,
                    MF_BYCOMMAND | ((c == 0 || !browser_column_hidden(c)) ? MF_CHECKED : MF_UNCHECKED));
    EnableMenuItem(g_menu, IDM_VIEW_COL_BASE, MF_BYCOMMAND | MF_GRAYED);   /* Name is always shown */
    CheckMenuItem(g_menu, IDM_VIEW_COL_FAV, MF_BYCOMMAND | (browser_column_hidden(2) ? MF_UNCHECKED : MF_CHECKED));   /* the star column is 2, Genre 3 */
    for (c = 0; c < 4; c++)
      CheckMenuItem(g_menu, IDM_VIEW_COL_EXTRA + c, MF_BYCOMMAND | (browser_column_hidden(3 + c) ? MF_UNCHECKED : MF_CHECKED));
    /* The columns only exist in the list view: gray while the grid is shown. */
    {
      static const int col_ids[] = { IDM_VIEW_COL_FAV, IDM_VIEW_COL_BASE + 1, IDM_VIEW_COL_EXTRA, IDM_VIEW_COL_EXTRA + 1, IDM_VIEW_COL_EXTRA + 2, IDM_VIEW_COL_EXTRA + 3 };
      int k;
      for (k = 0; k < 6; k++)
        EnableMenuItem(g_menu, col_ids[k], MF_BYCOMMAND | ((gui.browser_grid_view || emu_running) ? MF_GRAYED : MF_ENABLED));
    }
  }
  CheckMenuItem(g_menu, IDM_VIEW_SORT_RECENT, MF_BYCOMMAND | (gui.recent_mode ? MF_CHECKED : MF_UNCHECKED));
  EnableMenuItem(g_menu, IDM_TOOLS_MOVIE_REC, MF_BYCOMMAND | rom_state);
  EnableMenuItem(g_menu, IDM_TOOLS_MOVIE_PLAY, MF_BYCOMMAND | rom_state);
  EnableMenuItem(g_menu, IDM_TOOLS_MOVIE_STOP, MF_BYCOMMAND | rom_state);
  CheckMenuItem(g_menu, IDM_VIEW_GROUPCONSOLE, MF_BYCOMMAND | (gui.group_console ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_VIEW_AZSTRIP, MF_BYCOMMAND | (gui.az_strip ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_VIEW_GRIDLINES, MF_BYCOMMAND | (gui.browser_gridlines ? MF_CHECKED : MF_UNCHECKED));
  EnableMenuItem(g_menu, IDM_VIEW_GRIDLINES, MF_BYCOMMAND | ((gui.browser_grid_view || emu_running) ? (MF_GRAYED | MF_DISABLED) : MF_ENABLED));
  CheckMenuItem(g_menu, IDM_VIEW_CLEANNAMES, MF_BYCOMMAND | (gui.clean_names ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_VIEW_FAVONLY, MF_BYCOMMAND | (browser_favorites_only() ? MF_CHECKED : MF_UNCHECKED));
  EnableMenuItem(g_menu, IDM_VIEW_FAVONLY, MF_BYCOMMAND | (emu_running ? MF_GRAYED : MF_ENABLED));
  CheckMenuItem(g_menu, IDM_VIEW_TOOLBAR, MF_BYCOMMAND | (gui.hide_toolbar ? MF_UNCHECKED : MF_CHECKED));
  CheckMenuItem(g_menu, IDM_OPT_TBLABELS, MF_BYCOMMAND | (gui.toolbar_labels ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_VIEW_FSTOOLBAR, MF_BYCOMMAND | (gui.fullscreen_toolbar ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_VIEW_SEARCHBAR, MF_BYCOMMAND | (browser_search_shown() ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_VIEW_STATUSBAR, MF_BYCOMMAND | (gui.hide_statusbar ? MF_UNCHECKED : MF_CHECKED));
  CheckMenuItem(g_menu, IDM_VIEW_FILTERBAR, MF_BYCOMMAND | (gui.hide_filterbar ? MF_UNCHECKED : MF_CHECKED));
  /* The search and filter bars belong to the ROM browser: nothing to switch while a game runs. */
  EnableMenuItem(g_menu, IDM_VIEW_SEARCHBAR, MF_BYCOMMAND | (emu_running ? MF_GRAYED : MF_ENABLED));
  EnableMenuItem(g_menu, IDM_VIEW_FILTERBAR, MF_BYCOMMAND | (emu_running ? MF_GRAYED : MF_ENABLED));
  browser_toolbar_update();
  check_radio(g_menu, IDM_VIDEO_SCALE_BASE, 6, gui.scale - 1);
  check_radio(g_menu, IDM_VIDEO_ASPECT_BASE, 4, gui.aspect);
  check_radio(g_menu, IDM_VIDEO_NTSC_BASE, 4, config.ntsc);
  check_radio(g_menu, IDM_VIDEO_OVERSCAN_BASE, 4, config.overscan);

  CheckMenuItem(g_menu, IDM_VIDEO_FULLSCREEN,
                MF_BYCOMMAND | (gui.fullscreen ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_VIDEO_FULLSCREEN_START,
                MF_BYCOMMAND | (gui.fullscreen_on_load ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_VIDEO_ALWAYS_ON_TOP,
                MF_BYCOMMAND | (gui.always_on_top ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_VIDEO_SMOOTH,
                MF_BYCOMMAND | (gui.smooth ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_INPUT_BACKGROUND,
                MF_BYCOMMAND | (gui.background_input ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_VIDEO_FLICKER,
                MF_BYCOMMAND | (gui.flicker_blend ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_VIDEO_DITHER,
                MF_BYCOMMAND | (gui.dither_smooth ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_VIDEO_BRIGHTEN,
                MF_BYCOMMAND | (gui.brighten ? MF_CHECKED : MF_UNCHECKED));
  check_radio(g_menu, IDM_VIDEO_RENDERER_BASE, 3, gui.renderer);
  CheckMenuItem(g_menu, IDM_VIDEO_VSYNC,
                MF_BYCOMMAND | (gui.vsync ? MF_CHECKED : MF_UNCHECKED));
  check_radio(g_menu, IDM_VIDEO_SCANLINE_BASE, 5, gui.scanline_pct / 25);
  check_radio(g_menu, IDM_VIDEO_THEME_BASE, 3, theme_get_mode());
  check_radio(g_menu, IDM_SET_STYLE_BASE, 2, gui.ui_style);
  CheckMenuItem(g_menu, IDM_VIDEO_SHOWFPS,
                MF_BYCOMMAND | (gui.show_fps ? MF_CHECKED : MF_UNCHECKED));
  check_radio(g_menu, IDM_VIDEO_INTERLACE_BASE, 2, config.render ? 1 : 0);
  check_radio(g_menu, IDM_VIDEO_LCD_BASE, 4,
              config.lcd == 0 ? 0 : (config.lcd <= 128 ? 1 : (config.lcd <= 160 ? 2 : 3)));
  check_radio(g_menu, IDM_VIDEO_FRAMESKIP_BASE, 6, gui.frameskip);
  CheckMenuItem(g_menu, IDM_EMU_REWIND,
                MF_BYCOMMAND | (gui.rewind ? MF_CHECKED : MF_UNCHECKED));
  check_radio(g_menu, IDM_TOOLS_SHOT_BASE, 3, gui.shot_mode);
  CheckMenuItem(g_menu, IDM_VIDEO_SMSBORDER,
                MF_BYCOMMAND | (gui.sms_show_border ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_VIDEO_GGEXTRA,
                MF_BYCOMMAND | (config.gg_extra ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_VIDEO_SHADER_NONE,
                MF_BYCOMMAND | (shader_chain_active() ? MF_UNCHECKED : MF_CHECKED));
  EnableMenuItem(g_menu, IDM_VIDEO_SHADERPARAMS, MF_BYCOMMAND | (shader_chain_param_count() ? MF_ENABLED : (MF_GRAYED | MF_DISABLED)));
  CheckMenuItem(g_menu, IDM_VIDEO_SHADER_PRESET,
                MF_BYCOMMAND | (shader_chain_active() ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_VIDEO_SHADERNOFILTER, MF_BYCOMMAND | (gui.shader_nofilter ? MF_CHECKED : MF_UNCHECKED));
  EnableMenuItem(g_menu, IDM_VIDEO_SHADERNOFILTER, MF_BYCOMMAND | (gui.shader_preset_path[0] ? MF_ENABLED : (MF_GRAYED | MF_DISABLED)));

  CheckMenuItem(g_menu, IDM_AUDIO_ENABLE,
                /* Menu label is "Mute", so the checkmark means the opposite of
                   what it did as "Enable Sound" -- checked now means sound is
                   OFF. Same underlying gui.sound_enabled and the same click
                   handler (still a plain toggle either way), just presented
                   the other way round, so nothing else in the audio pipeline
                   needed to change. */
                MF_BYCOMMAND | (!gui.sound_enabled ? MF_CHECKED : MF_UNCHECKED));
  check_radio(g_menu, IDM_SET_STEREO_BASE, 4, gui.stereo_idx >= 0 && gui.stereo_idx < 4 ? gui.stereo_idx : 1);
  check_radio(g_menu, IDM_SET_BASS_BASE, 4, gui.bass >= 0 && gui.bass < 4 ? gui.bass : 0);
  CheckMenuItem(g_menu, IDM_OPT_OVERDRIVE, MF_BYCOMMAND | (gui.overdrive ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_OPT_NORMALIZE, MF_BYCOMMAND | (gui.normalize ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_AUDIO_MONO,
                MF_BYCOMMAND | (config.mono ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_EMU_FASTFWD, MF_BYCOMMAND | (gui_input_ff_latched() ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_EMU_REWINDNOW, MF_BYCOMMAND | (gui_input_rewind_latched() ? MF_CHECKED : MF_UNCHECKED));
  EnableMenuItem(g_menu, IDM_EMU_FASTFWD, MF_BYCOMMAND | ((emu_running && !netplay_active()) ? MF_ENABLED : (MF_GRAYED | MF_DISABLED)));
  EnableMenuItem(g_menu, IDM_EMU_REWINDNOW, MF_BYCOMMAND | ((emu_running && gui.rewind && !netplay_active()) ? MF_ENABLED : (MF_GRAYED | MF_DISABLED)));
  CheckMenuItem(g_menu, IDM_AUDIO_MUTE_FM, MF_BYCOMMAND | (gui.mute_fm ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_AUDIO_MUTE_PSG, MF_BYCOMMAND | (gui.mute_psg ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_AUDIO_MUTE_PWM, MF_BYCOMMAND | (gui.mute_pwm ? MF_CHECKED : MF_UNCHECKED));
  EnableMenuItem(g_menu, IDM_AUDIO_MUTE_PWM, MF_BYCOMMAND | (gui_pico_only(IDM_AUDIO_MUTE_PWM) ? (MF_GRAYED | MF_DISABLED) : MF_ENABLED));      /* (grayed only while a game of another system runs) */
  CheckMenuItem(g_menu, IDM_VIDEO_INTSCALE, MF_BYCOMMAND | (gui.integer_scale ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_EMU_AUTOSAVE, MF_BYCOMMAND | (gui.autosave ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_OPTIONS_OSD, MF_BYCOMMAND | (gui.osd ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_AUDIO_LOWPASS,
                MF_BYCOMMAND | (config.filter == 1 ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_AUDIO_EQ,
                MF_BYCOMMAND | (config.filter == 2 ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_AUDIO_HQPSG,
                MF_BYCOMMAND | (config.hq_psg ? MF_CHECKED : MF_UNCHECKED));

  {
    int mod = model_current(), k;
    for (k = 0; k < 3; k++) CheckMenuItem(g_menu, IDM_SET_MODEL_BASE + k, MF_BYCOMMAND | MF_UNCHECKED);
    if (mod >= 0) CheckMenuRadioItem(g_menu, IDM_SET_MODEL_BASE, IDM_SET_MODEL_BASE + 2, IDM_SET_MODEL_BASE + mod, MF_BYCOMMAND);
  }
  CheckMenuRadioItem(g_menu, IDM_EMU_SPEEDDOWN, IDM_EMU_SPEEDUP,
                     speed_pct < 100 ? IDM_EMU_SPEEDDOWN : (speed_pct > 100 ? IDM_EMU_SPEEDUP : IDM_EMU_SPEEDRESET), MF_BYCOMMAND);
  check_radio(g_menu, IDM_AUDIO_FMCORE_BASE, 5,
              config.ym3438 ? (gui.nuked_ym2612 ? 0 : 1) : ((config.ym2612 > 2) ? 4 : config.ym2612 + 2));
  check_radio(g_menu, IDM_AUDIO_RATE_BASE, 3, (gui.sample_rate == 22050) ? 0 : ((gui.sample_rate == 44100) ? 1 : 2));

  check_radio(g_menu, IDM_INPUT_PORTA_BASE, PORT_CHOICES, port_menu_index(0));
  check_radio(g_menu, IDM_INPUT_PORTB_BASE, PORT_CHOICES, port_menu_index(1));

  if (pico_active())                        /* a 32X game: what does nothing for it is grayed out */
  {
    int k;
    for (k = 1; k < 3800; k++)
      if (gui_pico_unsupported(k)) EnableMenuItem(g_menu, (UINT)k, MF_BYCOMMAND | MF_GRAYED | MF_DISABLED);
  }

  {
    int top, nt = GetMenuItemCount(g_menu);
    for (top = 0; top < nt; top++)
    {
      HMENU sub = GetSubMenu(g_menu, top);
      if (sub) menu_autogray(sub);          /* the menus themselves stay as they are */
    }
  }
  theme_menu_icon_marks(g_menu);
  status_set_slot();
}

int gui_port_index(int port)
{
  return port_menu_index(port);
}

/****************************************************************************
 * Window sizing
 ****************************************************************************/

/* In dark mode the top-level bar is themed by hand (Windows paints its own
   light-mode chrome around a plain dark background otherwise), and that
   theming goes through an undocumented per-item message (WM_UAHDRAWMENUITEM)
   that -- confirmed by direct testing -- Windows simply does not send for
   every item once the bar has wrapped to a second row: whichever titles land
   on the row Windows drops come up blank until something else (a click, a
   hover, a resize) forces a repaint of that one item, at which point it
   appears for good. This is not specific to the Large interface -- it reproduces in
   plain dark mode at a narrow window, the Large interface just makes it easier to hit
   because the bigger font needs more width. There is no fix for the message
   not being sent, so the window is kept wide enough that the bar never wraps
   in the first place, both when it is first created (create_main_window())
   and whenever it is resized to a scale afterward (gui_resize_to_scale(),
   which otherwise sizes purely from the game picture and knows nothing about
   the menu bar). Measured against the menu's plain text, before the Large interface's
   theme_ownerdraw_menu() converts the items: once converted, their text is no
   longer retrievable through GetMenuStringA(). Returns 0 outside dark mode,
   where the bar is never touched by hand and this does not apply. */
static int menu_bar_min_width(HMENU menu)
{
  HDC mhdc;
  HFONT mfont, mold;
  int needed = 0;
  int mcount, mi;

  if (!theme_is_dark() || !menu) return 0;

  mhdc  = GetDC(NULL);
  mfont = theme_menus_owner() ? gui_get_ui_font() : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
  mold  = (HFONT)SelectObject(mhdc, mfont);
  mcount = GetMenuItemCount(menu);

  for (mi = 0; mi < mcount; mi++)
  {
    char mtext[128];
    SIZE msz;

    if (GetMenuStringA(menu, (UINT)mi, mtext, sizeof(mtext), MF_BYPOSITION) > 0 &&
        GetTextExtentPoint32A(mhdc, mtext, lstrlenA(mtext), &msz))
    {
      needed += msz.cx + (theme_menus_owner() ? 44 : 40);   /* generous: the real bar's own
        inter-item and edge spacing runs well past itemWidth's own +20 formula,
        confirmed by direct testing (20 left 2 of 9 items still wrapped, 20+a
        flat +60 still left 1 of 9 wrapped) -- a window wider than strictly
        necessary costs nothing, so this errs well on the generous side rather
        than chasing the exact figure further. */
    }
  }

  needed += 40;   /* extra flat margin, on top of the generous per-item one above */

  SelectObject(mhdc, mold);
  ReleaseDC(NULL, mhdc);
  return needed;
}

void gui_resize_to_scale(int scale)
{
  RECT want;
  int w, h, w0, sb = 0;

  if (gui.fullscreen) return;

  video_preferred_size(scale, &w, &h);
  w0 = w;

  /* With no game running the ROM browser is what is shown: wide enough for
     its toolbar. */
  if (!gui.hide_toolbar && w < TBS(480)) w = TBS(480);

  /* The Large interface's toolbar is wide: the window is made wide enough for it to stay on one row (as far as the screen allows). */
  if (gui.large_ui && !gui.hide_toolbar)
  {
    int tw = browser_toolbar_one_row_width();
    MONITORINFO mi;

    mi.cbSize = sizeof(mi);
    if (GetMonitorInfo(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi) && tw > mi.rcWork.right - mi.rcWork.left - 16)
      tw = mi.rcWork.right - mi.rcWork.left - 16;
    if (w < tw) w = tw;
  }

  /* A game's picture keeps its shape: a window made wider for the toolbar is made taller by the same factor (as far as the screen allows). */
  if (emu_running && w > w0 && w0 > 0)
  {
    MONITORINFO mi;
    int th = (int)((long long)h * w / w0);

    mi.cbSize = sizeof(mi);
    if (GetMonitorInfo(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi) && th + browser_toolbar_height_at(w) + DPIS(160) <= mi.rcWork.bottom - mi.rcWork.top) h = th;
  }

  if (g_status && IsWindowVisible(g_status))
  {
    RECT r;
    GetWindowRect(g_status, &r);
    sb = r.bottom - r.top;
  }

  want.left = 0;
  want.top = 0;
  want.right = w;
  want.bottom = h + sb + browser_toolbar_height_at(w);          /* the toolbar grows with the window: its height at the width wanted */

  AdjustWindowRectEx(&want,
                     (DWORD)GetWindowLongPtr(g_hwnd, GWL_STYLE), TRUE,
                     (DWORD)GetWindowLongPtr(g_hwnd, GWL_EXSTYLE));

  {
    int mw = menu_bar_min_width(GetMenu(g_hwnd));
    if (mw > want.right - want.left) want.right = want.left + mw;
  }

  SetWindowPos(g_hwnd, NULL, 0, 0,
               want.right - want.left, want.bottom - want.top,
               SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

  /* The menu bar can be taller than the system's own (the Large interface's is 1.5 times) or wrap onto a second row, which
     AdjustWindowRectEx() does not know: what is left for the client area is then too small. Grow the window by what is missing. */
  {
    int pass, ch = h + sb + browser_toolbar_height_at(w);

    for (pass = 0; pass < 3; pass++)
    {
      RECT cr, wr;
      int dh, dw;

      GetClientRect(g_hwnd, &cr);
      dh = ch - cr.bottom;
      dw = w - cr.right;
      if (dh == 0 && dw <= 0) break;
      GetWindowRect(g_hwnd, &wr);
      SetWindowPos(g_hwnd, NULL, 0, 0, wr.right - wr.left + (dw > 0 ? dw : 0), wr.bottom - wr.top + dh,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
  }
}

/* Keeps the window above all others, or lets it behave normally again --
   called once at startup to restore the saved setting, and again whenever
   the person toggles it from the Video menu. */
void apply_always_on_top(void)
{
  SetWindowPos(g_hwnd, gui.always_on_top ? HWND_TOPMOST : HWND_NOTOPMOST,
               0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

/****************************************************************************
 * Save files
 ****************************************************************************/

/****************************************************************************
 * Per-game data files: <kind>\<Console>\<name>\<name><ext>
 ****************************************************************************/

static const char *rom_console = "";       /* folder name, "" if not known  */

static const char *const console_folders[] =
  { "MegaDrive", "MasterSystem", "GameGear", "SG1000", "MegaCD", "32X", "Pico", "Other" };

static const char *console_folder_name(void)
{
  if (system_hw == SYSTEM_MCD) return "MegaCD";
  if (pico_active()) return "32X";
  if (system_hw == SYSTEM_PICO) return "Pico";
  if ((system_hw & SYSTEM_PBC) == SYSTEM_MD) return "MegaDrive";
  if (system_hw == SYSTEM_GG || system_hw == SYSTEM_GGMS) return "GameGear";
  if (system_hw == SYSTEM_SMS || system_hw == SYSTEM_SMS2 || system_hw == SYSTEM_MARKIII) return "MasterSystem";
  if (system_hw == SYSTEM_SG || system_hw == SYSTEM_SGII || system_hw == SYSTEM_SGII_RAM_EXT) return "SG1000";
  return "Other";
}

/* Works out which console folder the game that was just loaded belongs to. */
static void identify_rom(void)
{
  rom_console = console_folder_name();
}

static int file_exists(const char *path)
{
  return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

/* The Mega CD RAM cartridge's file: Saves\MegaCD\cart.brm (it used to be in Saves itself, and is still read from there). */
static void cart_brm_path(char *out, int out_len, int for_read)
{
  char tmp[GUI_PATH_LEN * 2];

  wsprintfA(tmp, "%s\\MegaCD", osd_path("Saves"));
  CreateDirectoryA(tmp, NULL);
  wsprintfA(tmp, "%s\\MegaCD\\cart.brm", osd_path("Saves"));
  lstrcpynA(out, tmp, out_len);
  if (for_read && !file_exists(out))
  {
    char old[GUI_PATH_LEN * 2];
    wsprintfA(old, "%s\\cart.brm", osd_path("Saves"));
    if (file_exists(old)) lstrcpynA(out, old, out_len);
  }
}

/* Where a game's saves and states live: <kind>\<Console>\<Game name>\<Game name><ext>, one folder for each game
   inside its console's folder. */
static void data_path_new(const char *kind, const char *console, const char *ext, char *out, int out_len)
{
  char tmp[GUI_PATH_LEN * 2];

  if (rom_base[0])
    wsprintfA(tmp, "%s\\%s\\%s\\%s%s", osd_path(kind), console, rom_base, rom_base, ext);
  else
    wsprintfA(tmp, "%s\\%s%s", osd_path(kind), rom_base, ext);
  lstrcpynA(out, tmp, out_len);
}

void emu_data_path(const char *kind, const char *ext, int for_read, char *out, int out_len)
{
  char cand[GUI_PATH_LEN];
  size_t i;

  data_path_new(kind, rom_console[0] ? rom_console : "Other", ext, out, out_len);
  if (!for_read || file_exists(out)) return;

  /* The same game under another console folder (the Console option was
     changed after it was saved). */
  for (i = 0; i < sizeof(console_folders) / sizeof(console_folders[0]); i++)
  {
    if (!lstrcmpA(console_folders[i], rom_console)) continue;
    data_path_new(kind, console_folders[i], ext, cand, sizeof(cand));
    if (file_exists(cand)) { lstrcpynA(out, cand, out_len); return; }
  }
}

/* <exe>\<kind>\<Console>\<ROM name><ext>: named after the game alone, inside its console's folder (the cheat files).
   Makes the folders. */
void emu_console_file_path(const char *kind, const char *ext, char *out, int out_len)
{
  char tmp[GUI_PATH_LEN * 2];

  CreateDirectoryA(osd_path(kind), NULL);
  wsprintfA(tmp, "%s\\%s", osd_path(kind), rom_console[0] ? rom_console : "Other");
  CreateDirectoryA(tmp, NULL);
  wsprintfA(tmp, "%s\\%s\\%s%s", osd_path(kind), rom_console[0] ? rom_console : "Other", rom_base, ext);
  lstrcpynA(out, tmp, out_len);
}

/* A new recording's file: Recordings\<Kind>\<Console>\<game>_<date>_<time>.<ext> (the folders are made). */
static void recording_file_path(const char *kind, const char *ext, char *out, int out_len)
{
  char dir[GUI_PATH_LEN];
  SYSTEMTIME st;

  wsprintfA(dir, "Recordings\\%s", kind);
  ensure_dir("Recordings");
  ensure_dir(dir);
  wsprintfA(dir, "Recordings\\%s\\%s", kind, rom_console[0] ? rom_console : "Other");
  ensure_dir(dir);
  GetLocalTime(&st);
  gui_snprintf(out, out_len, "%s\\%s_%04d%02d%02d_%02d%02d%02d.%s", osd_path(dir), rom_base,
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, ext);
}

static int is_directory(const char *path);

/* Where Play Input Recording starts looking: the Input folder inside recordings, or recordings itself while that folder is not made yet
   (a folder is made when it is first used). */
static void input_recordings_dir(char *out, int out_len)
{
  lstrcpynA(out, osd_path("Recordings\\Input"), out_len);
  if (!is_directory(out)) lstrcpynA(out, osd_path("Recordings"), out_len);
}

void emu_ensure_data_dir(const char *kind)
{
  char tmp[GUI_PATH_LEN];

  CreateDirectoryA(osd_path(kind), NULL);
  wsprintfA(tmp, "%s\\%s", osd_path(kind), rom_console[0] ? rom_console : "Other");
  CreateDirectoryA(tmp, NULL);
  if (rom_base[0])                                /* and the game's own folder in it */
  {
    wsprintfA(tmp, "%s\\%s\\%s", osd_path(kind), rom_console[0] ? rom_console : "Other", rom_base);
    CreateDirectoryA(tmp, NULL);
  }
}

/* Writes a file so that a crash or power cut part-way through can never leave
   it half-written: the data goes to "<path>.tmp" first, is flushed, and then
   replaces the real file in one step. Returns 1 only if everything succeeded. */
static int write_file_atomic(const char *path, const void *data, size_t len)
{
  char tmp[GUI_PATH_LEN + 8];
  FILE *fp;
  int ok;

  wsprintfA(tmp, "%s.tmp", path);
  fp = fopen(tmp, "wb");
  if (!fp) return 0;

  ok = (len == 0) || (fwrite(data, 1, len, fp) == len);
  if (fflush(fp) != 0) ok = 0;
  if (fclose(fp) != 0) ok = 0;

  if (ok && !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) ok = 0;
  if (!ok) DeleteFileA(tmp);
  return ok;
}

/* Fingerprint of everything save_backup_ram() writes, so changes can be
   noticed cheaply (a few 64 KB passes) without rewriting files needlessly. */
static uint32 bram_hash_bytes(uint32 h, const uint8 *p, size_t n)
{
  while (n--) { h ^= *p++; h *= 16777619u; }
  return h;
}

static uint32 backup_ram_hash(void)
{
  uint32 h = 2166136261u;

  if (system_hw == SYSTEM_MCD)
  {
    h = bram_hash_bytes(h, scd.bram, 0x2000);
    if (scd.cartridge.id) h = bram_hash_bytes(h, scd.cartridge.area, (size_t)scd.cartridge.mask + 1);
  }
  if (pico_active())
  {
    size_t n;
    unsigned char *p = pico_save_ram(&n);
    if (p) h = bram_hash_bytes(h, p, n);
    return h;
  }
  if (sram.on) h = bram_hash_bytes(h, sram.sram, 0x10000);
  return h;
}

static uint32 bram_saved_hash;
static int    bram_hash_valid;      /* bram_saved_hash matches what is on disk */

static void save_backup_ram(void)
{
  char path[GUI_PATH_LEN];

  if (np_no_save) return;   /* see np_no_save */

  if (!emu_running) return;

  emu_ensure_data_dir("Saves");

  if (pico_active())                      /* a 32X game: the battery memory PicoDrive keeps, if the game has any */
  {
    size_t n;
    unsigned char *p = pico_save_ram(&n);
    if (p)
    {
      emu_data_path("Saves", ".srm", 0, path, sizeof(path));
      write_file_atomic(path, p, n);
    }
    bram_saved_hash = backup_ram_hash();
    bram_hash_valid = 1;
    return;
  }

  if (system_hw == SYSTEM_MCD)
  {
    if (!memcmp(scd.bram + 0x2000 - 0x20, brm_format + 0x20, 0x20))
    {
      emu_data_path("Saves", ".brm", 0, path, sizeof(path));
      write_file_atomic(path, scd.bram, 0x2000);
    }

    if (scd.cartridge.id &&
        !memcmp(scd.cartridge.area + scd.cartridge.mask + 1 - 0x20, brm_format + 0x20, 0x20))
    {
      cart_brm_path(path, (int)sizeof(path), 0);
      write_file_atomic(path, scd.cartridge.area, (size_t)scd.cartridge.mask + 1);
    }
  }

  if (sram.on)
  {
    emu_data_path("Saves", ".srm", 0, path, sizeof(path));
    write_file_atomic(path, sram.sram, 0x10000);
  }

  bram_saved_hash = backup_ram_hash();
  bram_hash_valid = 1;
}

/* Called about every 5 seconds while a game runs: if the game has changed its
   save data since the last write, write it now, so a crash, a power cut or
   Windows shutting down does not lose the progress. */
#define AUTOSAVE_INTERVAL_MS 5000

static void backup_ram_autosave(DWORD now)
{
  static DWORD last_check;

  if (now - last_check < AUTOSAVE_INTERVAL_MS) return;
  last_check = now;

  if (!emu_running || np_no_save) return;
  if (!bram_hash_valid || backup_ram_hash() != bram_saved_hash) save_backup_ram();
}

/* Reads a save file into memory. A file shorter than the memory is still used as far as it goes (the rest keeps what
   it had); the log says so, so a cut-off save is not a mystery. */
static void backup_read(const char *path, void *dst, size_t size, FILE *fp)
{
  size_t got = fread(dst, 1, size, fp);

  if (got != size) gui_log("The save file %s holds %u of %u bytes", path, (unsigned)got, (unsigned)size);
}

static void load_backup_ram(void)
{
  FILE *fp;
  char path[GUI_PATH_LEN];

  if (pico_active())
  {
    size_t n;
    unsigned char *p = pico_save_ram(&n);
    if (p)
    {
      emu_data_path("Saves", ".srm", 1, path, sizeof(path));
      fp = fopen(path, "rb");
      if (fp) { backup_read(path, p, n, fp); fclose(fp); }
    }
    bram_saved_hash = backup_ram_hash();
    bram_hash_valid = 1;
    return;
  }

  if (system_hw == SYSTEM_MCD)
  {
    emu_data_path("Saves", ".brm", 1, path, sizeof(path));
    fp = fopen(path, "rb");
    if (fp) { backup_read(path, scd.bram, 0x2000, fp); fclose(fp); }

    if (memcmp(scd.bram + 0x2000 - 0x20, brm_format + 0x20, 0x20))
    {
      memset(scd.bram, 0x00, 0x200);
      brm_format[0x10] = brm_format[0x12] = brm_format[0x14] = brm_format[0x16] = 0x00;
      brm_format[0x11] = brm_format[0x13] = brm_format[0x15] = brm_format[0x17] =
        (uint8)((sizeof(scd.bram) / 64) - 3);
      memcpy(scd.bram + 0x2000 - 0x40, brm_format, 0x40);
    }

    if (scd.cartridge.id)
    {
      cart_brm_path(path, (int)sizeof(path), 1);
      fp = fopen(path, "rb");
      if (fp) { backup_read(path, scd.cartridge.area, scd.cartridge.mask + 1, fp); fclose(fp); }

      if (memcmp(scd.cartridge.area + scd.cartridge.mask + 1 - 0x20, brm_format + 0x20, 0x20))
      {
        memset(scd.cartridge.area, 0x00, scd.cartridge.mask + 1);
        brm_format[0x10] = brm_format[0x12] = brm_format[0x14] = brm_format[0x16] =
          (uint8)((((scd.cartridge.mask + 1) / 64) - 3) >> 8);
        brm_format[0x11] = brm_format[0x13] = brm_format[0x15] = brm_format[0x17] =
          (uint8)((((scd.cartridge.mask + 1) / 64) - 3) & 0xff);
        memcpy(scd.cartridge.area + scd.cartridge.mask + 1 - sizeof(brm_format),
               brm_format, sizeof(brm_format));
      }
    }
  }

  if (sram.on)
  {
    emu_data_path("Saves", ".srm", 1, path, sizeof(path));
    fp = fopen(path, "rb");
    if (fp) { backup_read(path, sram.sram, 0x10000, fp); fclose(fp); }
  }

  /* What is in memory now is what is on disk (or the fresh default). */
  bram_saved_hash = backup_ram_hash();
  bram_hash_valid = 1;
}

/* Persists the cheat list against whatever ROM the list belongs to: the one
   that is running, or the one that was only inspected (emu_peek_rom, from the
   browser's Edit Cheats). rom_base is set in both cases and cleared when a
   running game is closed, and the list is always the one loaded for it. */
void cheats_save_current(void)
{
  if (rom_base[0]) cheats_save_for_rom(rom_base);
}

void cheats_sync_current(void)
{
  if (rom_base[0]) cheats_sync_with_disk(rom_base);
}

/****************************************************************************
 * Save states
 ****************************************************************************/

/* The existing state file for a slot (see emu_data_path()). */
void state_path(int slot, char *out, int out_len)
{
  char ext[16];

  wsprintfA(ext, ".gp%d", slot);
  emu_data_path("States", ext, 1, out, out_len);
}

void thumb_path(int slot, char *out, int out_len)
{
  char ext[24];

  wsprintfA(ext, ".gp%d.bmp", slot);
  emu_data_path("States", ext, 1, out, out_len);
}

/* Removes a slot's state and thumbnail, including any copy under an older
   name -- otherwise the old copy would show up in the slot again. */
void state_delete(int slot)
{
  char p[GUI_PATH_LEN];
  int i;

  for (i = 0; i < 8; i++)
  {
    state_path(slot, p, sizeof(p));
    if (!file_exists(p)) break;
    DeleteFileA(p);
  }
  for (i = 0; i < 8; i++)
  {
    thumb_path(slot, p, sizeof(p));
    if (!file_exists(p)) break;
    DeleteFileA(p);
  }
}

int state_file_time(const char *path, char *out, int out_len)
{
  WIN32_FILE_ATTRIBUTE_DATA fad;
  SYSTEMTIME utc, st;

  if (out_len > 0) out[0] = '\0';
  if (!path || !GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) return 0;

  FileTimeToSystemTime(&fad.ftLastWriteTime, &utc);
  if (!SystemTimeToTzSpecificLocalTime(NULL, &utc, &st)) st = utc;

  if (out_len >= 24)
    wsprintfA(out, "%02d:%02d:%02d  %02d/%02d/%04d",
              st.wHour, st.wMinute, st.wSecond, st.wDay, st.wMonth, st.wYear);
  return 1;
}

/* A state is the Genesis Plus GX core's, or, for a 32X game, PicoDrive's (which sizes its states itself). */
static size_t host_state_cap(void)
{
  return pico_active() ? pico_state_max() : (size_t)STATE_SIZE;
}

static int host_state_save(uint8 *buf)
{
  return pico_active() ? pico_state_save(buf, pico_state_max()) : state_save(buf);
}

static int host_state_load(uint8 *buf, size_t len)
{
  video_deinterlace_reset();       /* the picture is not the one after the last frame */
  return pico_active() ? pico_state_load(buf, len) : state_load(buf);
}

#define AUTO_SLOT 10   /* the "resume where you left off" state: not one of the ten slots of the menu */
static int resume_prompt_blocked;   /* set when the command line asked for a state of its own */
static char resume_marker[GUI_PATH_LEN];     /* see emu_load_rom: the file that says a resume point is being used */
static int  resume_marker_frames;

/* What a slot held before the last save, so that save can be undone. */
static uint8 *usave_state, *usave_thumb;
static size_t usave_state_len, usave_thumb_len;
static int    usave_slot = -1, usave_valid, usave_existed;

static uint8 *read_whole_file(const char *path, size_t *len)
{
  FILE *fp = fopen(path, "rb");
  long n;
  uint8 *buf = NULL;

  *len = 0;
  if (!fp) return NULL;
  if (fseek(fp, 0, SEEK_END) == 0 && (n = ftell(fp)) > 0 && fseek(fp, 0, SEEK_SET) == 0)
  {
    buf = (uint8 *)malloc((size_t)n);
    if (buf && fread(buf, 1, (size_t)n, fp) == (size_t)n) *len = (size_t)n;
    else { free(buf); buf = NULL; }
  }
  fclose(fp);
  return buf;
}

int emu_can_undo_save(void)
{
  return usave_valid;
}

void emu_undo_save_state(void)
{
  char sp[GUI_PATH_LEN], tp[GUI_PATH_LEN];

  if (!emu_running || !usave_valid) return;
  usave_valid = 0;
  state_path(usave_slot, sp, sizeof(sp));
  thumb_path(usave_slot, tp, sizeof(tp));

  if (usave_existed && usave_state)
  {
    write_file_atomic(sp, usave_state, usave_state_len);
    if (usave_thumb) write_file_atomic(tp, usave_thumb, usave_thumb_len);
    else DeleteFileA(tp);
  }
  else
  {
    DeleteFileA(sp);       /* the slot was empty before that save */
    DeleteFileA(tp);
  }
  gui_notify("Save undone (slot %d)", usave_slot);
}

void emu_save_state(int slot)
{
  char path[GUI_PATH_LEN];
  uint8 *buffer;
  int len;

  if (!emu_running) return;

  buffer = (uint8 *)malloc(host_state_cap());
  if (!buffer) { gui_notify("Not enough memory to save the state"); return; }

  len = host_state_save(buffer);

  emu_ensure_data_dir("States");
  {
    char ext[16];
    wsprintfA(ext, ".gp%d", slot);
    emu_data_path("States", ext, 0, path, sizeof(path));
  }

  /* Keep what the slot holds now, so this save can be undone. */
  if (len > 0 && slot != AUTO_SLOT)
  {
    char tp[GUI_PATH_LEN];
    free(usave_state); free(usave_thumb);
    usave_state = usave_thumb = NULL;
    state_path(slot, tp, sizeof(tp));
    usave_state = read_whole_file(tp, &usave_state_len);
    usave_existed = (usave_state != NULL);
    thumb_path(slot, tp, sizeof(tp));
    usave_thumb = read_whole_file(tp, &usave_thumb_len);
    usave_slot = slot;
    usave_valid = 0;
  }

  if (len > 0 && write_file_atomic(path, buffer, (size_t)len))
  {
    gui_log(slot == AUTO_SLOT ? "State saved: the resume point (%d bytes)" : "State saved: slot %d", slot == AUTO_SLOT ? len : slot);
    if (slot != AUTO_SLOT) usave_valid = 1;
    /* state_path() now finds the new file, so the thumbnail lands beside it. */
    thumb_path(slot, path, sizeof(path));
    video_save_thumbnail(path);
    if (slot != AUTO_SLOT) gui_notify("Saved to slot %d", slot);
  }
  else
  {
    if (slot != AUTO_SLOT) gui_notify("Could not write slot %d", slot);
  }

  free(buffer);
}

/* The game as it was just before the last successful state load. */
static uint8 *undo_state;
static size_t undo_cap, undo_len;
static int    undo_valid;

static int undo_buffer(void)
{
  size_t need = host_state_cap();

  if (!undo_state || undo_cap < need)
  {
    free(undo_state);
    undo_state = (uint8 *)malloc(need);
    undo_cap = undo_state ? need : 0;
  }
  return undo_state != NULL;
}

void emu_undo_load_state(void)
{
  if (!emu_running || !undo_valid || !undo_state) return;

  undo_valid = 0;
  if (host_state_load(undo_state, undo_len))
  {
    cheat_search_invalidate();
    emu_apply_sms_border();
    waveout_flush();
    cheats_apply();
    video_viewport_changed();
    gui_notify("Load undone");
  }
  else
  {
    gui_notify("Could not undo the load");
  }
}

int emu_can_undo_load(void)
{
  return undo_valid;
}

#define STATE_FILE (-2)   /* "slot" used for a state file picked by the player */

static void load_state_path(const char *path, int slot)
{
  uint8 *buffer;
  FILE *fp;
  size_t got;
  int have_undo = 0;
  int loaded = 0;

  if (!emu_running) return;

  fp = fopen(path, "rb");
  if (!fp)
  {
    if (slot == STATE_FILE) gui_notify("Could not open that state file");
    else if (slot != AUTO_SLOT) gui_notify("Slot %d is empty", slot);
    return;
  }

  buffer = (uint8 *)malloc(host_state_cap());
  if (!buffer) { fclose(fp); gui_notify("Not enough memory to load the state"); return; }

  /* state_save() writes only the bytes it actually used (bufferptr), which is
     always well under STATE_SIZE -- STATE_SIZE is just the worst-case (Sega CD)
     allocation, so a legitimate file is normally shorter than a full read.
     Zero the buffer first so a genuinely truncated file (disk full mid-write,
     an interrupted copy) leaves its unread tail as zeroes instead of feeding
     state_load() uninitialized heap memory; state_load() has no way to know
     how much fread() actually filled in and walks the whole STATE_SIZE region
     regardless (see core/state.h's load_param). */
  memset(buffer, 0, host_state_cap());

  got = fread(buffer, 1, host_state_cap(), fp);
  fclose(fp);

  if (got > 0)
  {
    /* Keep the game as it is now, so a load pressed by mistake can be undone. */
    if (undo_buffer())
    {
      int n = host_state_save(undo_state);
      if (n > 0) { undo_len = (size_t)n; have_undo = 1; }
    }

    loaded = host_state_load(buffer, got) ? 1 : 0;

    /* A 32X state that was refused can leave the machine half loaded: what it was before goes back. */
    if (!loaded && have_undo && pico_active())
      host_state_load(undo_state, undo_len);
  }

  if (loaded)
  {
    gui_log(slot == STATE_FILE ? "State loaded from a file" : slot == AUTO_SLOT ? "State loaded: the resume point" : "State loaded: slot %d", slot);
    cheat_search_invalidate();
    undo_valid = have_undo;
    emu_apply_sms_border();
    if (slot == STATE_FILE) gui_notify("State loaded from the file");
    else if (slot != AUTO_SLOT) gui_notify("Loaded slot %d", slot);
    waveout_flush();
    /* Banking was restored from the state, so 8-bit ROM patches now point at the
       wrong page. Rewrite them all. */
    cheats_apply();
    video_viewport_changed();
    gui_update_menu();
  }
  else if (slot == STATE_FILE)
  {
    gui_notify(got == 0 ? "That state file could not be read" : "That state file was saved by a different version or another game");
  }
  else
  {
    gui_notify(got == 0 ? "Slot %d could not be read" : "Slot %d was saved by a different version", slot);
  }

  free(buffer);
}

void emu_load_state(int slot)
{
  char path[GUI_PATH_LEN];

  if (!emu_running) return;
  state_path(slot, path, sizeof(path));
  load_state_path(path, slot);
}

void emu_load_state_file(const char *path)
{
  load_state_path(path, STATE_FILE);
}

void emu_save_state_file(const char *path)
{
  uint8 *buffer;
  int len;

  if (!emu_running) return;
  buffer = (uint8 *)malloc(host_state_cap());
  if (!buffer) { gui_notify("Not enough memory to save the state"); return; }
  len = host_state_save(buffer);
  if (len > 0 && write_file_atomic(path, buffer, (size_t)len)) gui_notify("State saved to the file");
  else gui_notify("Could not write the state file");
  free(buffer);
}

/* Backup States: while a game runs, a state is saved every few minutes (Settings > Options > Backup State) into three
   rolling files, newest first, so a crash or a mistake never costs much. */
static void backup_path(int k, char *out, int len)
{
  char ext[16];
  wsprintfA(ext, ".bk%d", k + 1);
  emu_data_path("States", ext, 1, out, len);
}

/* The menu asks often (every refresh of it): the disk is looked at again after a second, for another game, or after a
   backup has been written. */
static DWORD bk_stamp;
static char  bk_rom[GUI_PATH_LEN];
static int   bk_have[3];

static int backup_exists(int k)
{
  char p[GUI_PATH_LEN];
  DWORD now = GetTickCount();
  int i;

  if (!emu_running || k < 0 || k >= 3) return 0;
  if (!bk_stamp || now - bk_stamp > 1000 || lstrcmpA(bk_rom, rom_path))
  {
    for (i = 0; i < 3; i++)
    {
      backup_path(i, p, sizeof(p));
      bk_have[i] = GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
    }
    lstrcpynA(bk_rom, rom_path, sizeof(bk_rom));
    bk_stamp = now | 1;
  }
  return bk_have[k];
}

static void backup_state_save(void)
{
  char p1[GUI_PATH_LEN], p2[GUI_PATH_LEN];
  uint8 *buffer;
  int len, k;

  bk_stamp = 0;                 /* the menu looks at the disk again */

  buffer = (uint8 *)malloc(host_state_cap());
  if (!buffer) return;
  len = host_state_save(buffer);
  if (len > 0)
  {
    for (k = 2; k > 0; k--)                   /* older ones move down one place */
    {
      backup_path(k - 1, p1, sizeof(p1));
      backup_path(k, p2, sizeof(p2));
      MoveFileExA(p1, p2, MOVEFILE_REPLACE_EXISTING);
    }
    backup_path(0, p1, sizeof(p1));
    if (write_file_atomic(p1, buffer, (size_t)len)) gui_log("Backup state saved");
    else gui_log("Backup state could not be written");
  }
  free(buffer);
}

static void backup_state_tick(DWORD now)
{
  static const int mins[5] = { 0, 1, 2, 5, 10 };
  static DWORD last;
  int m = mins[gui.autosave_min >= 0 && gui.autosave_min < 5 ? gui.autosave_min : 0];

  if (m == 0 || !emu_running || emu_paused || netplay_active() || np_no_save || pm_open) { last = now; return; }
  if (now - last < (DWORD)m * 60000UL) return;
  last = now;
  backup_state_save();
}

static void backup_state_load(int k)
{
  char p[GUI_PATH_LEN];
  if (!emu_running || netplay_active()) return;
  backup_path(k, p, sizeof(p));
  load_state_path(p, STATE_FILE);
}

/****************************************************************************
 * Audio plumbing
 ****************************************************************************/

/* The Nuked core can behave as a YM2612 (early consoles: status readable only
   at the first port) or as a YM3438 (later ones: readable at any port). The
   core default is the YM3438 behavior. */
/* Master System only. Unless the side borders are shown, hides the 8-pixel side borders (the column games blank to
   hide scrolling) by cropping 8 pixels off each side of the picture, the same
   way the Game Gear's smaller screen is produced. The core resets this on every
   reset, so it is re-applied after each one. Not used while the Borders
   setting already shows the left/right overscan. */
void emu_apply_sms_border(void)
{
  if (!emu_running) return;
  if (system_hw != SYSTEM_MARKIII && system_hw != SYSTEM_SMS && system_hw != SYSTEM_SMS2) return;

  if (!gui.sms_show_border && !(config.overscan & 2)) bitmap.viewport.x = -8;
  else                                               bitmap.viewport.x = (config.overscan & 2) * 7;

  bitmap.viewport.changed = 3;
}

void emu_apply_nuked_type(void)
{
  OPN2_SetChipType(gui.nuked_ym2612 ? ym3438_mode_ym2612 : ym3438_mode_readmode);
}

/* Re-creates the FM chip after its type changed (Master System FM unit or
   chip, or the Genesis FM core). Swapping chips mid-game can't carry the
   old chip's registers over, so some instruments may stay quiet until the
   game re-sends them or is reset. */
void emu_apply_fm_settings(void)
{
  if (!emu_running) return;

  audio_init(run_rate_now(), 0);
  sound_init();
  sound_reset();
  audio_reset();
  vgm_restore_sound();
}

void emu_apply_audio_settings(void)
{
  if (!gui.sound_enabled)
  {
    waveout_close();
    return;
  }

  /* Only touch the device when the format actually changed -- reopening it
     on every ROM load produces an audible click for no reason. */
  if (waveout_rate() != run_rate_now())
  {
    waveout_close();

    if (!waveout_open(run_rate_now()))
    {
      gui.sound_enabled = 0;
      gui_status("No audio device available, sound is off");
      return;
    }
  }

  waveout_set_volume(gui.volume);

  if (emu_running)
  {
    /* Only audio_init() -- it's the one that actually depends on the
       sample rate (it rebuilds the blip buffers to match). sound_init()
       does not: reading its implementation directly shows its behavior
       depends purely on which FM core is configured (config.ym2612/
       config.ym3438), never on the sample rate. Calling it here anyway
       was unnecessarily wiping the FM/PSG chip's entire register/channel
       state on every Sample Rate or Enable Sound change -- silencing
       whatever instruments the running game had already set up and
       wasn't continuously re-sending, since nothing told the game itself
       anything had changed. That's what "missing instruments until a
       reset" was: not a timing bug, but real state being thrown away for
       a change that never needed to touch it. */
    audio_init(run_rate_now(), 0);
  }
}

/****************************************************************************
 * Region changes
 ****************************************************************************/

void emu_apply_region(void)
{
  if (!emu_running) return;

  get_region(0);

  if ((system_hw == SYSTEM_MCD) || ((system_hw & SYSTEM_SMS) && (config.bios & 1)))
  {
    /* Same order as a hard reset: system_init() starts the save memory from
       scratch, so keep what the game had written and put it back. */
    cheats_suspend();
    save_backup_ram();
    system_init();
    load_backup_ram();
    system_reset();
    emu_apply_sms_border();
    undo_valid = 0;   /* the state "Undo Load State" would restore no longer matches this machine */
  }
  else
  {
    if (system_hw == SYSTEM_MD)
    {
      io_reg[0x00] = (uint8)(0x20 | region_code | (config.bios & 1));
    }
    else
    {
      io_reg[0x00] = (uint8)(0x80 | (region_code >> 1));
    }

    if (vdp_pal)
    {
      status |= 1;
      lines_per_frame = 313;
    }
    else
    {
      status &= ~1;
      lines_per_frame = 262;
    }

    switch (bitmap.viewport.h)
    {
      case 192: vc_max = vc_table[0][vdp_pal]; break;
      case 224: vc_max = vc_table[1][vdp_pal]; break;
      case 240: vc_max = vc_table[3][vdp_pal]; break;
      default: break;
    }
  }

  /* The frame rate moved, so the audio timing has to be rebuilt. This has to
     come after lines_per_frame is updated above: the core derives the frame
     rate from it, and rebuilding first kept the old (60 Hz / 50 Hz) rate. */
  audio_init(snd.sample_rate, 0);

  cheats_hold_for_boot();
  cheats_apply();
  waveout_flush();
  video_viewport_changed();
}

/****************************************************************************
 * ROM lifecycle
 ****************************************************************************/

static void load_boot_rom(void)
{
  FILE *fp;

  system_bios = 0;
  memset(boot_rom, 0xFF, sizeof(boot_rom));

  fp = fopen(MD_BIOS, "rb");
  if (!fp) return;

  if (fread(boot_rom, 1, 0x800, fp) != 0x800) { gui_log("The boot ROM %s is shorter than 2 KB: not used", MD_BIOS); fclose(fp); memset(boot_rom, 0xFF, sizeof(boot_rom)); return; }
  fclose(fp);

  if (!memcmp((char *)(boot_rom + 0x120), "GENESIS OS", 10))
  {
    int i;

    system_bios = SYSTEM_MD;

    for (i = 0; i < 0x800; i += 2)
    {
      uint8 temp = boot_rom[i];
      boot_rom[i] = boot_rom[i + 1];
      boot_rom[i + 1] = temp;
    }

    for (i = 0x800; i < 0x10000; i++)
    {
      boot_rom[i] = boot_rom[i & 0x7ff];
    }
  }
}

/* The program and machine, and the settings that matter most: written when the log starts and when it is switched on, so a
   log sent with a bug report says what it came from. */
static void log_system_info(void)
{
  typedef LONG (WINAPI *rtl_version_fn)(OSVERSIONINFOW *);
  rtl_version_fn rtl = (rtl_version_fn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
  OSVERSIONINFOW vi;
  char exe[MAX_PATH];

  ZeroMemory(&vi, sizeof(vi));
  vi.dwOSVersionInfoSize = sizeof(vi);
  if (rtl) rtl(&vi);
  if (!GetModuleFileNameA(NULL, exe, sizeof(exe))) exe[0] = '\0';
  gui_log("Version %s, Windows %lu.%lu build %lu, %s", APP_VERSION, (unsigned long)vi.dwMajorVersion, (unsigned long)vi.dwMinorVersion, (unsigned long)vi.dwBuildNumber, exe);
  gui_log("Settings: renderer %d, %s style, theme %d, %s interface, display scale %d%%, sound rate %d", gui.renderer, gui.ui_style ? "Modern" : "Classic",
          gui.theme_mode, gui.large_ui ? "larger" : "normal", gui_dpi * 100 / 96, gui.sample_rate);
}

void emu_close_rom(void)
{
  gui_combos_release();
  if (!emu_running) return;

  gui_log("Game closed: %s", rom_base);

  cheat_search_close();
  fast_osd_state = 0;
  speed_pct = 100;
  movie_stop();
  vgm_stop();
  if (pm_open) { pm_open = 0; video_show_menu(NULL, 0, 0); }
  SetThreadExecutionState(ES_CONTINUOUS);   /* the PC may sleep again */

  /* "Auto Resume": the game as it is now, kept for the next time it is opened. */
  if (gui.autosave && netplay_state() == NP_IDLE)
  {
    if (skip_autosave_once) state_delete(AUTO_SLOT);            /* "Resume this game next time" was unticked: no old resume point is left either */
    else emu_save_state(AUTO_SLOT);
  }
  skip_autosave_once = 0;

  if (resume_marker[0]) { DeleteFileA(resume_marker); resume_marker[0] = '\0'; }       /* closed in the normal way */
  record_stop();
  netplay_disconnect(NULL);
  history_game_ended();

  cheats_save_current();
  cheats_suspend();          /* undo patches while cart.rom is still valid */
  cheats_remove_all();

  save_backup_ram();
  bram_hash_valid = 0;
  pico_unload();             /* a 32X game: the library is let go (its battery memory has just been written) */
  audio_shutdown();
  waveout_flush();

  emu_running = 0;
  emu_paused = 0;
  auto_paused_by_focus = 0;

  /* Closing from fullscreen: back to the windowed size first. */
  if (gui.fullscreen) video_set_fullscreen(0);
  undo_valid = 0;
  usave_valid = 0;
  gui_input_clear_latches();
  rewind_reset();
  rom_path[0] = '\0';
  rom_base[0] = '\0';
  rom_console = "";

  update_title();
  gui_update_menu();
  status_set_fps(0);                       /* no frame rate or speed once nothing runs */
  gui_status_persistent("Open a ROM to start, or drop one on this window");

  /* Back to the window size the Video > Window Size setting asks for, rather
     than leaving the window at whatever size the game's own picture (a
     different aspect ratio, NTSC filter width, or a manual drag) happened to
     leave it at -- video_preferred_size() already gives the plain 320x224
     base size once nothing is running, so this lands on the same size a
     fresh launch with no ROM would. A maximized window stays maximized. */
  if (!IsZoomed(g_hwnd)) gui_resize_to_scale(gui.scale);

  {
    RECT content;
    get_content_rect(&content);
    browser_panel_layout(&content);
    browser_panel_show(1);
  }

  InvalidateRect(g_hwnd, NULL, TRUE);
}

static int is_7z_path(const char *path)
{
  const char *dot = strrchr(path, '.');
  return dot && !lstrcmpiA(dot, ".7z");
}

/* Decompresses the first file in a .7z archive (matching sdl/fileio.c's
   load_archive(), which does the same for .zip: no entry-type filtering,
   the archive is trusted to hold exactly the one ROM it was made for) into a
   temp file named with THAT entry's own extension, not ".7z" -- load_rom()
   picks the system to emulate from the extension of the file it's handed
   (see core/loadrom.c), so the temp file needs to look like the real thing.
   Returns 1 and fills out_path on success; out_path is unchanged on failure.
   Only actually reads anything when built with CHD=1 (the default) -- see
   sevenzip.c and lzmasdk/README-lzmasdk.txt for why .7z support is tied to
   that flag; otherwise this just reports the archive as unreadable. */
static int extract_7z_rom(const char *archive_path, char *out_path, int out_len)
{
#ifdef USE_LIBCHDR
  sevenzip_t *sz;
  int i, count, index = -1;
  char inner_name[GUI_PATH_LEN];
  const char *inner_dot;
  char temp_dir[MAX_PATH];
  unsigned char *buffer;
  unsigned int size;
  int got, ok = 0;
  FILE *fp;

  sz = sevenzip_open(archive_path);
  if (!sz) return 0;

  count = sevenzip_count(sz);
  for (i = 0; i < count; i++)
  {
    if (!sevenzip_entry_is_dir(sz, i)) { index = i; break; }
  }
  if (index < 0) { sevenzip_close(sz); return 0; }

  sevenzip_entry_name(sz, index, inner_name, sizeof(inner_name));
  inner_dot = strrchr(inner_name, '.');
  if (!inner_dot || !inner_dot[1]) { sevenzip_close(sz); return 0; }

  size = sevenzip_entry_size(sz, index);
  if (size == 0 || size > (32u * 1024 * 1024))
  {
    /* Empty, or bigger than any ROM this core supports (MAXROMSIZE) --
       either way not worth allocating for. */
    sevenzip_close(sz);
    return 0;
  }

  buffer = (unsigned char *)malloc(size);
  if (!buffer) { sevenzip_close(sz); return 0; }

  got = sevenzip_extract(sz, index, buffer, (int)size);
  sevenzip_close(sz);
  if (got != (int)size) { free(buffer); return 0; }

  if (!GetTempPathA(sizeof(temp_dir), temp_dir)) { free(buffer); return 0; }
  wsprintfA(out_path, "%sgpex7z%lu%s", temp_dir, (unsigned long)GetCurrentProcessId(), inner_dot);   /* (per process: two copies do not share a file) */
  if ((int)lstrlenA(out_path) >= out_len) { free(buffer); return 0; }

  fp = fopen(out_path, "wb");
  if (fp)
  {
    ok = (fwrite(buffer, 1, size, fp) == size) && (fclose(fp) == 0);
    if (!ok) DeleteFileA(out_path);
  }

  free(buffer);
  return ok;
#else
  (void)archive_path; (void)out_path; (void)out_len;
  return 0;
#endif
}

/* "A game is running": Stop Emulation (back to the list), Exit (the program closes) or Cancel. Returns 1, 2 or 0. With Auto
   Resume on, a box offers to keep the game for the next time (*keep). */
static int exit_ask(HWND owner, int *keep)
{
  typedef HRESULT (WINAPI *TaskDialogIndirect_t)(const TASKDIALOGCONFIG *, int *, int *, BOOL *);
  static const TASKDIALOG_BUTTON buttons[] = { { 101, L"Stop Emulation" }, { 102, L"Exit" } };
  TaskDialogIndirect_t td = NULL;
  HMODULE cc = GetModuleHandleA("comctl32.dll");
  int answer = IDCANCEL;
  BOOL checked = TRUE;

  *keep = 1;
  if (cc) td = (TaskDialogIndirect_t)GetProcAddress(cc, "TaskDialogIndirect");
  if (td)
  {
    TASKDIALOGCONFIG cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cbSize = sizeof(cfg);
    cfg.hwndParent = owner;
    cfg.pszWindowTitle = L"Genesis Plus EX";
    cfg.pszMainIcon = TD_INFORMATION_ICON;
    if (gui.autosave && netplay_state() == NP_IDLE)
    {
      cfg.pszVerificationText = L"Resume this game next time";
      cfg.dwFlags |= TDF_VERIFICATION_FLAG_CHECKED;
    }
    cfg.pszMainInstruction = L"A game is running.";
    cfg.pszContent = L"Stop the emulation and go back to the game list, or exit the program?";
    cfg.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    cfg.cButtons = 2;
    cfg.pButtons = buttons;
    cfg.nDefaultButton = IDCANCEL;
    if (FAILED(td(&cfg, &answer, NULL, &checked))) answer = IDCANCEL;
    if (cfg.pszVerificationText) *keep = checked ? 1 : 0;
    return answer == 101 ? 1 : (answer == 102 ? 2 : 0);
  }
  /* Without task dialogs: Yes = Stop Emulation, No = Exit. */
  answer = MessageBoxA(owner, "A game is running.\n\nYes: Stop Emulation (back to the game list)\nNo: Exit\nCancel: carry on", APP_NAME,
                       MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON3);
  return answer == IDYES ? 1 : (answer == IDNO ? 2 : 0);
}

/* The Auto Resume question, with a "Do not ask again" box that remembers the answer. Returns 1 to resume. */
static int resume_ask(void)
{
  typedef HRESULT (WINAPI *TaskDialogIndirect_t)(const TASKDIALOGCONFIG *, int *, int *, BOOL *);
  static const TASKDIALOG_BUTTON buttons[] = { { IDYES, L"Resume" }, { IDNO, L"Start Fresh" } };
  TaskDialogIndirect_t td = NULL;
  HMODULE cc;
  int answer = IDNO, remember = 0;

  if (gui.resume_choice == 1) return 1;
  if (gui.resume_choice == 2) return 0;

  cc = GetModuleHandleA("comctl32.dll");
  if (cc) td = (TaskDialogIndirect_t)GetProcAddress(cc, "TaskDialogIndirect");
  if (td)
  {
    TASKDIALOGCONFIG cfg;
    BOOL checked = FALSE;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cbSize = sizeof(cfg);
    cfg.hwndParent = g_hwnd;
    cfg.pszWindowTitle = L"Genesis Plus EX";
    cfg.pszMainIcon = TD_INFORMATION_ICON;
    cfg.pszMainInstruction = L"Resume where you left off?";
    cfg.pszVerificationText = L"Do not ask again";
    cfg.cButtons = 2;
    cfg.pButtons = buttons;
    cfg.nDefaultButton = IDYES;
    if (SUCCEEDED(td(&cfg, &answer, NULL, &checked))) remember = checked ? 1 : 0;
    else answer = IDNO;
  }
  else
  {
    answer = MessageBoxA(g_hwnd, "Resume where you left off?", APP_NAME, MB_YESNO | MB_ICONQUESTION);
  }

  if (remember && (answer == IDYES || answer == IDNO))
  {
    gui.resume_choice = (answer == IDYES) ? 1 : 2;
    config_save();
  }
  return answer == IDYES;
}

static char patch_override[GUI_PATH_LEN];     /* a patched copy of the game to load instead of the file (File > Apply Patch) */

/* A game with a "SEGA 32X" header that Genesis Plus GX has just loaded: PicoDrive is started for it (the file may be
   the extracted copy of an archive, so this runs before that is deleted). Returns 0, after telling the player why, when
   it could not be started; 1 when it was started or the game is not a 32X one. */
static int start_32x_if_needed(const char *file)
{
  char err[1000], msg[GUI_PATH_LEN + 1100];

  if (!pico_rom_is_32x()) return 1;
  if (pico_load(file, err, sizeof(err))) return 1;

  wsprintfA(msg, "Could not start this 32X game.\n\n%s", err[0] ? err : "PicoDrive could not start it.");
  MessageBoxA(g_hwnd, msg, APP_NAME, MB_OK | MB_ICONWARNING);
  return 0;
}

int emu_load_rom(const char *path)
{
  gui_combos_release();
  char attempt[GUI_PATH_LEN];
  int was_fullscreen = 0;

  if (!path || !path[0]) return 0;

  lstrcpynA(attempt, path, sizeof(attempt));

  /* Checked before the current game is closed: a missing file, a folder, or a
     file that is not a game type must not end the game that is running. */
  {
    DWORD attr = GetFileAttributesA(attempt);

    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY))
    {
      char msg[GUI_PATH_LEN + 160];
      int i, in_recent = 0, in_played = history_last(attempt) != 0;
      for (i = 0; i < GUI_RECENT_MAX; i++)
        if (gui.recent[i][0] && !lstrcmpiA(gui.recent[i], attempt)) in_recent = 1;

      if (!in_recent && !in_played)
      {
        wsprintfA(msg, "Could not find:\n\n%s", attempt);
        MessageBoxA(g_hwnd, msg, APP_NAME, MB_OK | MB_ICONWARNING);
      }
      else
      {
        wsprintfA(msg, "Could not find:\n\n%s\n\nRemove it from the %s?", attempt,
                  in_recent && in_played ? "recent files and the recently played list" : in_recent ? "recent files" : "recently played list");
        if (MessageBoxA(g_hwnd, msg, APP_NAME, MB_YESNO | MB_ICONWARNING) == IDYES)
        {
          int k = 0;
          for (i = 0; i < GUI_RECENT_MAX; i++)
            if (gui.recent[i][0] && lstrcmpiA(gui.recent[i], attempt)) { if (k != i) lstrcpynA(gui.recent[k], gui.recent[i], GUI_PATH_LEN); k++; }
          for (; k < GUI_RECENT_MAX; k++) gui.recent[k][0] = '\0';
          if (in_played) history_remove(attempt);
          rebuild_recent_menu();
          config_save();
          browser_refilter();
        }
      }
      return 0;
    }

    if (!browser_console_for_path(attempt)) return 0;     /* not a game: nothing happens, and it is not remembered */
  }

  was_fullscreen = gui.fullscreen;      /* opening another game from fullscreen stays in fullscreen */
  emu_close_rom();

  {
    char load_path[GUI_PATH_LEN];
    int from_7z = is_7z_path(attempt);
    int loaded;

    lstrcpynA(load_path, attempt, sizeof(load_path));
    if (from_7z && !extract_7z_rom(attempt, load_path, sizeof(load_path)))
    {
      char msg[GUI_PATH_LEN + 96];
      wsprintfA(msg, "Could not read the .7z archive:\n\n%s", attempt);
      MessageBoxA(g_hwnd, msg, APP_NAME, MB_OK | MB_ICONWARNING);
      gui_status("Loading failed");
      return 0;
    }

    ports_for_new_game();
    if (patch_override[0])
    {
      loaded = load_rom(patch_override) ? 1 : 0;       /* File > Apply Patch: the patched copy */
      if (loaded && !start_32x_if_needed(patch_override)) loaded = -1;
      DeleteFileA(patch_override);
      patch_override[0] = '\0';
    }
    else
    {
      loaded = load_rom(load_path) ? 1 : 0;
      if (loaded && !start_32x_if_needed(load_path)) loaded = -1;
    }
    if (from_7z) DeleteFileA(load_path);

    if (loaded < 0)                    /* a 32X game that PicoDrive could not start (it has said why) */
    {
      gui_status("Loading failed");
      return 0;
    }

    if (!loaded)
    {
      char msg[GUI_PATH_LEN + 96];
      wsprintfA(msg, "Could not load:\n\n%s\n\n"
                     "The file may be missing, unreadable, or not a supported ROM.",
                attempt);
      MessageBoxA(g_hwnd, msg, APP_NAME, MB_OK | MB_ICONWARNING);
      gui_status("Loading failed");
      return 0;
    }
  }

  lstrcpynA(rom_path, attempt, sizeof(rom_path));
  split_basename(rom_path, rom_base, sizeof(rom_base));
  identify_rom();
  ports_fit_system();
  np_no_save = 0;
  np_rom_sha_ready = 0;

  audio_init(gui.sample_rate, 0);
  system_init();

  load_backup_ram();

  emu_running = 1;
  emu_paused = 0;
  browser_panel_show(0);
  rewind_reset();

  /* Set the audio format up before the reset so the chips come out of reset
     already matched to the output rate. */
  emu_apply_audio_settings();
  system_reset();
  emu_apply_sms_border();

  cheats_load_for_rom(rom_base);
  video_viewport_changed();

  input_press_logged = 0;
  gui_log("Input: port A device %d, port B device %d, devices %d %d %d %d %d %d %d %d", (int)input.system[0], (int)input.system[1],
          (int)input.dev[0], (int)input.dev[1], (int)input.dev[2], (int)input.dev[3], (int)input.dev[4], (int)input.dev[5], (int)input.dev[6], (int)input.dev[7]);
  gui_log("Game file: %s (%u KB)", rom_path, (unsigned)(cart.romsize / 1024));
  gui_log("Game loaded: %s (hardware %d, region %d, %s, FM chip %s, sample rate %d, renderer %d)", rom_base, (int)system_hw, (int)region_code,
          vdp_pal ? "PAL" : "NTSC", config.ym3438 ? "Nuked" : "YM2612", snd.sample_rate, gui.renderer);
  config_add_recent(rom_path);
  history_game_started(rom_path);
  config_save();

  rebuild_recent_menu();
  update_title();
  gui_update_menu();
  if (!IsZoomed(g_hwnd)) gui_resize_to_scale(gui.scale);   /* a maximized window stays maximized */
  gui_status_persistent("Running %s", rom_base);

  if ((gui.fullscreen_on_load || was_fullscreen) && !gui.fullscreen)
  {
    video_set_fullscreen(1);
    gui_update_menu();
  }

  next_frame_time = 0.0;

  int resumed = 0;

  if (gui.autosave && !resume_prompt_blocked && netplay_state() == NP_IDLE)
  {
    char sp[GUI_PATH_LEN];
    FILE *rf;

    state_path(AUTO_SLOT, sp, sizeof(sp));
    rf = fopen(sp, "rb");
    if (rf)
    {
      char mk[GUI_PATH_LEN];

      fclose(rf);
      /* While a resume point is in use a small marker file lies next to it (it goes after a couple of seconds of play). One
         still there at the next start means the program ended right after resuming: that resume point is dropped, so a bad
         one cannot keep the game from starting. */
      gui_snprintf(mk, (int)sizeof(mk), "%s.resuming", sp);
      if (file_exists(mk))
      {
        DeleteFileA(mk);
        state_delete(AUTO_SLOT);
        gui_log("The resume point of this game was dropped: the program ended right after using it last time");
        gui_notify("The saved resume point was dropped (the program ended right after using it last time)");
      }
      else if (resume_ask())
      {
        FILE *mf = fopen(mk, "wb");
        if (mf) { fputc('1', mf); fclose(mf); lstrcpynA(resume_marker, mk, sizeof(resume_marker)); resume_marker_frames = 0; }
        emu_load_state(AUTO_SLOT);
        resumed = 1;
      }
    }
  }

  /* Skip Intro on Launch: runs the first seconds at full speed (a press of a game button ends it). */
  if (!resumed && !resume_prompt_blocked && gui.skip_len > 0 && netplay_state() == NP_IDLE)
  {
    static const int secs[6] = { 5, 10, 15, 20, 25, 30 };
    skip_frames_left = (int)(secs[(gui.skip_len - 1) % 6] * emu_frame_rate());
    gui_notify("Skipping the intro (press a button to stop)");
  }
  return 1;
}

/* For ROM Information / Edit Cheats from the browser's context menu on
   a ROM that isn't the one currently running: populates rominfo and
   loads that ROM's cheat list for viewing/editing, without starting
   emulation -- no system_init/system_reset, no audio, emu_running
   stays 0, and the browser panel is left exactly as it was. This is
   only safe because nothing is running yet: load_rom() overwrites the
   same cart.rom buffer a live game would be using, so calling this
   while a *different* ROM is actually playing would corrupt it out
   from under it. Returns 0 without doing anything if a game is
   currently running -- callers fall back to emu_load_rom() then,
   which is the only safe option in that case. */
int emu_peek_rom(const char *path)
{
  char attempt[GUI_PATH_LEN];

  if (emu_running) return 0;
  if (!path || !path[0]) return 0;

  lstrcpynA(attempt, path, sizeof(attempt));

  emu_close_rom();   /* no-op given the emu_running guard above, but mirrors emu_load_rom's own pattern */

  {
    char load_path[GUI_PATH_LEN];
    int from_7z = is_7z_path(attempt);
    int loaded;

    lstrcpynA(load_path, attempt, sizeof(load_path));
    if (from_7z && !extract_7z_rom(attempt, load_path, sizeof(load_path))) return 0;

    ports_for_new_game();
    loaded = load_rom(load_path) ? 1 : 0;
    if (from_7z) DeleteFileA(load_path);
    if (!loaded) return 0;
  }

  lstrcpynA(rom_path, attempt, sizeof(rom_path));
  split_basename(rom_path, rom_base, sizeof(rom_base));
  identify_rom();
  ports_fit_system();

  cheats_load_for_rom(rom_base);

  return 1;
}

/* See osd.h's CD_TRAY_CALLBACK: core/cd_hw/cdd.c calls this on both CDD
   "Open Tray" (0x0D, requesting a disc) and "Close Tray" (0x0C) commands --
   only the open half asks the host to do anything, cdd.status == CD_OPEN
   at that point. The actual swap is a single cdd_load() call with no
   system_reset()/system_init() around it, exactly like a real console:
   only the drive's loaded image changes, not the running 68k/work RAM/
   backup RAM state, which is the whole point of a multi-disc game not
   losing progress across a disc change. */
void win32_cd_tray_callback(void)
{
  char dir[GUI_PATH_LEN];
  char path[GUI_PATH_LEN] = "";
  const char *slash;

  if (!emu_running || system_hw != SYSTEM_MCD || cdd.status != CD_OPEN) return;

  if (MessageBoxA(g_hwnd,
        "This game is asking for a different disc.\n\n"
        "Insert the next disc and continue?",
        APP_NAME, MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
    return;

  lstrcpynA(dir, rom_path, sizeof(dir));
  slash = dir;
  {
    const char *p;
    for (p = dir; *p; p++)
      if (*p == '\\' || *p == '/') slash = p;
  }
  if (slash > dir) ((char *)slash)[0] = '\0'; else dir[0] = '\0';

  if (!gui_pick_file(g_hwnd, GUI_PICK_ROM, "Insert Disc",
        "Mega CD / Sega CD\0*.cue;*.iso;*.chd\0"
        "All Files\0*.*\0\0",
        dir[0] ? dir : NULL, path, sizeof(path)))
    return;

  if (cdd_load(path, (char *)cart.rom) != 1)
  {
    MessageBoxA(g_hwnd, "Could not read that disc image.", APP_NAME, MB_OK | MB_ICONWARNING);
    return;
  }

  gui_status("Disc changed");
}

/* File > Insert Disc: swaps the Mega CD disc by hand, for games that do not ask for one themselves.
   The tray is treated as closed again afterwards, as when a game asks for the next disc. */
void emu_insert_disc(void)
{
  char dir[GUI_PATH_LEN];
  char path[GUI_PATH_LEN] = "";
  char *slash;

  if (!emu_running || system_hw != SYSTEM_MCD) return;

  lstrcpynA(dir, rom_path, sizeof(dir));
  slash = strrchr(dir, '\\');
  if (slash) *slash = '\0'; else dir[0] = '\0';

  if (!gui_pick_file(g_hwnd, GUI_PICK_ROM, "Insert Disc",
        "Mega CD / Sega CD\0*.cue;*.iso;*.chd\0All Files\0*.*\0\0", dir[0] ? dir : NULL, path, sizeof(path)))
    return;

  if (cdd_load(path, (char *)cart.rom) != 1)
  {
    MessageBoxA(g_hwnd, "Could not read that disc image.", APP_NAME, MB_OK | MB_ICONWARNING);
    return;
  }
  cdd.status = cdd.loaded ? CD_TOC : NO_DISC;
  cdd.index = cdd.lba = 0;
  gui_notify("Disc changed");
}

const char *emu_rom_filename(void)
{
  return rom_base;
}

const char *emu_rom_path(void)
{
  return rom_path;
}

void emu_reset(int hard)
{
  if (!emu_running) return;

  gui_log("Game %s", hard ? "hard reset" : "reset");

  if (pico_active())                    /* a 32X game: PicoDrive starts over (it has no difference between the two) */
  {
    cheats_suspend();                   /* the game must find its own ROM when it starts over */
    pico_reset();
    undo_valid = 0;
    cheats_hold_for_boot();
    cheats_apply();
    waveout_flush();
    gui_notify(hard ? "Hard reset" : "Reset");
    return;
  }

  cheat_search_invalidate();
  cheats_suspend();

  if (hard)
  {
    rewind_reset();      /* the machine starts over: earlier history no longer fits */
    save_backup_ram();
    system_init();
    load_backup_ram();
    np_no_save = 0;         /* backup RAM now matches the real save files again */
  }

  system_reset();
  undo_valid = 0;   /* the state "Undo Load State" would restore no longer matches this machine */

  emu_apply_sms_border();
  cheats_hold_for_boot();
  cheats_apply();
  waveout_flush();
  gui_notify(hard ? "Hard reset" : "Reset");
}

/****************************************************************************
 * Open dialog
 ****************************************************************************/

static int is_directory(const char *path)
{
  DWORD attr;

  if (!path || !path[0]) return 0;
  attr = GetFileAttributesA(path);
  return (attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

static void browse_for_rom(void)
{
  char file[GUI_PATH_LEN] = "";
  const char *start_dir = NULL;

  /* Always start in the ROM directory chosen for the browser, no matter
     where the last ROM was picked from. */
  if (is_directory(gui.rom_dir)) start_dir = gui.rom_dir;

  if (gui_pick_file(g_hwnd, GUI_PICK_ROM, "Open ROM",
        "All Supported Files\0*.zip;*.7z;*.gz;*.md;*.gen;*.bin;*.smd;*.mdx;*.sms;*.gg;*.sg;*.68k;*.32x;*.cue;*.iso;*.chd\0"
        "Mega Drive / Genesis / Sega Pico\0*.md;*.gen;*.bin;*.smd;*.mdx;*.68k\0"
        "32X\0*.32x\0"
        "Master System / Game Gear / SG-1000\0*.sms;*.gg;*.sg\0"
        "Mega CD / Sega CD\0*.cue;*.iso;*.chd\0"
        "Archives\0*.zip;*.7z;*.gz\0"
        "All Files\0*.*\0\0",
        start_dir, file, sizeof(file)))
  {
    emu_load_rom(file);
  }
}

/****************************************************************************
 * Emulation
 ****************************************************************************/

/****************************************************************************
 * Netplay glue -- everything netplay.c needs to know about the emulator
 ****************************************************************************/

static int np_preflight(char *err, int err_len)
{
  if (!emu_running)
  {
    lstrcpynA(err, "Load a game first.", err_len);
    return 0;
  }
  if (system_hw == SYSTEM_MCD)
  {
    lstrcpynA(err, "Netplay supports cartridge games only, not Sega CD.", err_len);
    return 0;
  }
  if (gui_input_p2_slot() < 0)
  {
    lstrcpynA(err, "Netplay needs a plain Control Pad on both ports (Input > Port A / Port B Device).", err_len);
    return 0;
  }
  return 1;
}

static void np_add(np_info *info, const char *name, unsigned value)
{
  if (info->n_settings < NP_MAX_SETTINGS)
  {
    info->name[info->n_settings] = name;
    info->value[info->n_settings] = value;
    info->n_settings++;
  }
}

static void np_collect(np_info *info)
{
  /* Anything here that differs between the two PCs changes what the game
     does, so the connection is refused (and the name shown) if it does. */
  if (!np_rom_sha_ready)
  {
    cheats_suspend();                       /* the ROM as it is in the file, not with a code in it */
    np_sha256(cart.rom, (size_t)cart.romsize, np_rom_sha);
    cheats_apply();
    np_rom_sha_ready = 1;
  }
  memcpy(info->rom_sha, np_rom_sha, 32);

  np_add(info, "Cheats (both PCs need the same ones switched on)", cheats_fingerprint());
  np_add(info, "32X Game",            (unsigned)pico_active());
  np_add(info, "Low-Pass Filter",     (unsigned)(config.filter == 1 ? config.lp_range : 0));
  np_add(info, "Console",             config.system);
  np_add(info, "Region",              config.region_detect);
  np_add(info, "Detected Region",     region_code);
  np_add(info, "TV Standard (PAL)",   vdp_pal);
  np_add(info, "System Hardware",     system_hw);
  np_add(info, "Force VDP Mode",      config.vdp_mode);
  np_add(info, "Interlaced Mode",     config.render);       /* Single Field keeps the interlace field flag still, which the game can read */
  np_add(info, "Master Clock",        config.master_clock);
  np_add(info, "DTACK",               config.force_dtack);
  np_add(info, "Address Errors",      config.addr_error);
  np_add(info, "Boot from BIOS",      config.bios);
  np_add(info, "Lock-on Cartridge",   config.lock_on);
  np_add(info, "Add-on",              config.add_on);
  np_add(info, "Hot Swap",            config.hot_swap);
  np_add(info, "FM Chip",             (unsigned)config.ym2612 | ((unsigned)config.ym3438 << 8) |
                                      ((unsigned)config.ym2413 << 16) | ((unsigned)config.opll << 24));
  np_add(info, "Nuked FM Chip Type",  (unsigned)gui.nuked_ym2612);
  np_add(info, "High-Quality PSG / FM", (unsigned)config.hq_psg | ((unsigned)config.hq_fm << 8));
  np_add(info, "Sample Rate",         (unsigned)snd.sample_rate);
  np_add(info, "Port A Device",       input.system[0]);
  np_add(info, "Port B Device",       input.system[1]);
  np_add(info, "Pad Type, Player 1",  config.input[0].padtype);
  np_add(info, "Pad Type, Player 2",  config.input[1].padtype);
}

/* The VDP's 4-entry write FIFO is not cleared by a reset (vdp_reset() leaves it alone), so it keeps what the game
   last wrote -- and it is part of the saved state, so two machines that were not at the same point in the game before
   netplay started begin with different fingerprints (found in a user's log: only the VDP piece differed). It is
   zeroed in a saved copy of the state, which is then loaded back. The layout is checked first (the registers are
   found where expected); if it does not match, nothing is touched. */
static void np_normalize_vdp_fifo(void)
{
  static uint8 buf[STATE_SIZE];
  int pos = 16, len, regs;

  if ((system_hw & SYSTEM_PBC) == SYSTEM_MD)
    pos += (int)(sizeof(work_ram) + sizeof(zram) + sizeof(zstate) + sizeof(zbank));
  else
    pos += 0x2000;
  pos += (int)sizeof(io_reg);

  memset(buf, 0, STATE_SIZE);
  len = state_save(buf);
  regs = pos + (int)(sizeof(sat) + sizeof(vram) + sizeof(cram) + sizeof(vsram));       /* then reg[], addr, addr_latch, code, pending, status, dmafill, fifo_idx, fifo[4] */
  if (regs + (int)sizeof(reg) + 2 + 2 + 1 + 1 + 2 + 4 + 4 + 8 > len || memcmp(buf + regs, reg, sizeof(reg))) return;
  memset(buf + regs + sizeof(reg) + 2 + 2 + 1 + 1 + 2 + 4 + 4, 0, 8);
  state_load(buf);
}

/* A reset of the 68000 sets only the stack pointer, the program counter and the status register: D0-D7, A0-A6 and the
   user stack pointer keep what the game had in them. They are part of the saved state, so two machines that were not at
   the same point in the game before netplay started began with different fingerprints (the joining PC's logs showed only
   the 68000 piece changing from run to run). They are zeroed in a saved copy of the state, which is loaded back; the
   layout is checked first (the program counter must be found where expected). */
static void np_normalize_m68k_regs(void)
{
  static uint8 buf[STATE_SIZE], scratch[STATE_SIZE];
  int pos = 16, len, cpu;
  uint32 pc;

  if ((system_hw & SYSTEM_PBC) != SYSTEM_MD) return;
  pos += (int)(sizeof(work_ram) + sizeof(zram) + sizeof(zstate) + sizeof(zbank) + sizeof(io_reg));
  pos += vdp_context_save(scratch);
  pos += sound_context_save(scratch);

  memset(buf, 0, STATE_SIZE);
  len = state_save(buf);
  cpu = pos;                                   /* D0-D7 (32 bytes), A0-A7 (32), PC (4), SR (2), USP (4), ISP (4) */
  pc = m68k_get_reg(M68K_REG_PC);
  if (cpu + 32 + 32 + 4 + 2 + 4 + 4 > len || memcmp(buf + cpu + 64, &pc, 4)) return;
  memset(buf + cpu, 0, 56);                    /* D0-D7, A0-A6 */
  memset(buf + cpu + 70, 0, 4);                /* USP (after PC at 64 and SR at 68) */
  state_load(buf);
}

/* The Nuked FM chip's 0..23 phase counter (ym3438_cycles in sound.c) is not reset by a reset, so it
   keeps whatever value the game had before netplay started, and the two machines then differ in
   that one byte. It is saved in the state, so it is put right by saving, zeroing it there and
   loading the state back. */
static void np_normalize_fm_phase(void)
{
  static uint8 buf[STATE_SIZE], scratch[STATE_SIZE];
  int pos = 16, len;

  if ((system_hw & SYSTEM_PBC) != SYSTEM_MD || !config.ym3438) return;

  pos += (int)(sizeof(work_ram) + sizeof(zram) + sizeof(zstate) + sizeof(zbank) + sizeof(io_reg));
  pos += vdp_context_save(scratch);
  /* sound section: config byte, chip, 24x2 shorts, 2 ints, then the phase counter */
  pos += 1 + (int)sizeof(ym3438_t) + 24 * 2 * 2 + 2 * 4;

  memset(buf, 0, STATE_SIZE);
  len = state_save(buf);
  if (pos + 4 > len) return;
  memset(buf + pos, 0, 4);
  state_load(buf);
}

/* For tracking down a "did not start out in the same state" report: with Enable Log on, the state each side starts from
   is written to the log in pieces (a piece that differs between the two PCs' logs is where the difference is). */
static unsigned np_piece_hash(const uint8 *p, int n)
{
  unsigned h = 2166136261u;
  while (n-- > 0) { h ^= *p++; h *= 16777619u; }
  return h;
}

static void np_log_start_state(void)
{
  static uint8 buf[STATE_SIZE], scratch[STATE_SIZE];
  int len, pos = 16, n, vdp;
  int (*keep_cb)(int) = Z80.irq_callback;
  const struct z80_irq_daisy_chain *keep_daisy = Z80.daisy;

  if (!gui.log_on) return;
  if (pico_active())
  {
    unsigned long long hh = pico_state_hash();
    gui_log("Netplay start: 32X game, state fingerprint %08X%08X", (unsigned)(hh >> 32), (unsigned)hh);
    pico_state_log_pieces();
    return;
  }
  Z80.irq_callback = NULL;
  Z80.daisy = NULL;
  memset(buf, 0, STATE_SIZE);
  len = state_save(buf);
  Z80.irq_callback = keep_cb;
  Z80.daisy = keep_daisy;

  gui_log("Netplay start: state %d bytes, hardware %02X, region %d, cycles %d", len, (unsigned)system_hw, (int)region_code, (int)mcycles_vdp);
  gui_log("Netplay start: header %08X", np_piece_hash(buf, 16));
  n = (int)sizeof(work_ram); gui_log("Netplay start: work_ram %08X", np_piece_hash(buf + pos, n)); pos += n;
  n = (int)sizeof(zram);     gui_log("Netplay start: zram %08X", np_piece_hash(buf + pos, n)); pos += n;
  n = (int)(sizeof(zstate) + sizeof(zbank)); gui_log("Netplay start: zstate+zbank %08X", np_piece_hash(buf + pos, n)); pos += n;
  n = (int)sizeof(io_reg);   gui_log("Netplay start: io_reg %08X", np_piece_hash(buf + pos, n)); pos += n;
  vdp = vdp_context_save(scratch);
  if (pos + vdp <= len) { gui_log("Netplay start: vdp %08X", np_piece_hash(buf + pos, vdp)); pos += vdp; }
  {
    int snd = sound_context_save(scratch);
    int cpu = 16 * 4 + 4 + 2 + 4 + 4 + (int)(sizeof(m68k.cycles) + sizeof(m68k.int_level) + sizeof(m68k.stopped));
    char map[0x40 * 2 + 1];
    int k;

    if ((system_hw & SYSTEM_PBC) != SYSTEM_MD) cpu = 0;
    if (pos + snd <= len) { gui_log("Netplay start: sound %08X (%d bytes)", np_piece_hash(buf + pos, snd), snd); pos += snd; }
    if (cpu && pos + cpu <= len) { gui_log("Netplay start: 68000 %08X (%d bytes)", np_piece_hash(buf + pos, cpu), cpu); pos += cpu; }
    if (pos + (int)sizeof(Z80_Regs) <= len) { gui_log("Netplay start: Z80 %08X (%d bytes)", np_piece_hash(buf + pos, (int)sizeof(Z80_Regs)), (int)sizeof(Z80_Regs)); pos += (int)sizeof(Z80_Regs); }
    if ((system_hw & SYSTEM_PBC) == SYSTEM_MD && pos + 0x40 <= len)
    {
      for (k = 0; k < 0x40; k++) wsprintfA(map + k * 2, "%02X", buf[pos + k]);
      gui_log("Netplay start: cartridge map %s", map);
      pos += 0x40;
    }
    if (pos < len) gui_log("Netplay start: cartridge rest %08X (%d bytes)", np_piece_hash(buf + pos, len - pos), len - pos);
  }
}

static void np_message(const char *text, int level);

static void np_begin(void)
{
  /* Both machines start from the same power-on state. The backup RAM comes
     out at its defaults (nothing is loaded from disk), and saving is blocked
     until a game is loaded or hard-reset normally. */
  cheats_suspend();
  if (pico_active())
  {
    /* A 32X game: PicoDrive is started again (a new copy of the library, so nothing of what was played is left in it), which
       puts both PCs at the same point: the start-up frames run with no button pressed. */
    if (!pico_restart())
    {
      np_message("The 32X game could not be started again for netplay.", 1);
      netplay_disconnect(NULL);
      return;
    }
  }
  else
  {
    system_init();
    system_reset();
    np_normalize_vdp_fifo();
    np_normalize_m68k_regs();
    np_normalize_fm_phase();
  }
  undo_valid = 0;   /* the state "Undo Load State" would restore no longer matches this machine */
  if (!pico_active()) emu_apply_sms_border();
  waveout_flush();
  emu_paused = 0;
  auto_paused_by_focus = 0;
  auto_paused_by_minimize = 0;
  np_no_save = 1;
  rewind_reset();
  next_frame_time = 0.0;
  cheats_hold_for_boot();
  cheats_apply();                           /* the cheats that are on (the same on both PCs: they were compared) */
  np_log_start_state();

  gui_update_menu();
  status_apply_prefixes();
}

static void np_end(void)
{
  gui_update_menu();
  status_apply_prefixes();
}

/* FNV-1a over the whole machine state. The buffer is cleared first so bytes the
   save code doesn't write (padding) hash the same on both sides. */
static unsigned long long np_state_hash(void)
{
  static uint8 *buf;
  unsigned long long h = 14695981039346656037ULL;
  int i, len;

  if (pico_active()) return pico_state_hash();
  if (!buf) buf = (uint8 *)malloc(STATE_SIZE);
  if (!buf) return 0;

  /* Z80.irq_callback and Z80.daisy are pointers, saved as they are. Each program
     starts at its own address (ASLR), so the two machines would never hash alike. */
  {
    int (*keep_cb)(int) = Z80.irq_callback;
    const struct z80_irq_daisy_chain *keep_daisy = Z80.daisy;
    Z80.irq_callback = NULL;
    Z80.daisy = NULL;
    memset(buf, 0, STATE_SIZE);
    len = state_save(buf);
    Z80.irq_callback = keep_cb;
    Z80.daisy = keep_daisy;
  }
  for (i = 0; i < len; i++)
  {
    h ^= buf[i];
    h *= 1099511628211ULL;
  }
  return h;
}

static const char *np_game_title(void)
{
  return rom_base;
}

/* Frames per second the emulated console really runs at: master clock over
   the length of a frame (59.92 Hz NTSC, 49.70 Hz PAL, or whatever Force VDP
   Mode / Force Master Clock make of it). snd.frame_rate is not usable for this:
   the GUI starts the audio with a frame rate of 0, which leaves it at 0. */
double emu_frame_rate(void)
{
  if (pico_active()) return pico_fps();
  if (lines_per_frame < 1 || system_clock < 1) return 60.0;
  return (double)system_clock / ((double)MCYCLES_PER_LINE * (double)lines_per_frame);
}

/* File name of the loaded game without folder or extension. */
const char *emu_rom_base(void)
{
  return rom_base;
}

/* Problems the person has to read are shown from emulation_step(), between
   frames. netplay code can report one from inside the core's frame (the input
   callback), and a modal dialog there would run the message loop -- repaints
   and all -- in the middle of an emulated frame. */
static char np_pending_msg[700];

static void np_message(const char *text, int level)
{
  if (level) lstrcpynA(np_pending_msg, text, sizeof(np_pending_msg));
  else       gui_notify("%s", text);
  status_apply_prefixes();
}

static const np_host_t np_iface =
{
  np_preflight, np_collect, np_begin, np_end, np_state_hash, np_game_title, np_message
};

/* Things that would change the game's state on one PC only. Blocked while a
   session is running. */
static int np_command_locked(int id)
{
  if (!netplay_active()) return 0;

  if (id == IDM_EMU_PAUSE || id == IDM_EMU_RESET || id == IDM_EMU_HARDRESET) return 1;
  if (id == IDM_FILE_SAVESTATE || id == IDM_FILE_LOADSTATE || id == IDM_FILE_STATEMGR || id == IDM_FILE_LOADSTATEFILE || id == IDM_FILE_SAVESTATEFILE) return 1;
  if (id == IDM_INPUT_PAUSEMENU) return 1;
  if (id == IDM_EMU_INSERTDISC || id == IDM_EMU_SPEEDUP || id == IDM_EMU_SPEEDDOWN || id == IDM_EMU_SPEEDRESET) return 1;
  if (id == IDM_EMU_UNDOLOAD || id == IDM_EMU_UNDOSAVE || id == IDM_EMU_FASTFWD || id == IDM_EMU_REWINDNOW) return 1;
  if (id >= IDM_FILE_LOADSLOT_BASE && id < IDM_FILE_LOADSLOT_BASE + GUI_SLOT_MAX) return 1;
  if (id >= IDM_FILE_LOADBACKUP_BASE && id < IDM_FILE_LOADBACKUP_BASE + 3) return 1;
  if (id >= IDM_FILE_SAVESLOT_BASE && id < IDM_FILE_SAVESLOT_BASE + GUI_SLOT_MAX) return 1;
  if (id >= IDM_ACCEL_QUICKSAVE && id <= IDM_ACCEL_SLOT_PREV) return 1;
  if (id >= IDM_FILE_SLOT_BASE && id < IDM_FILE_SLOT_BASE + GUI_SLOT_MAX) return 1;
  if (id == IDM_EMU_CHEATS || id == IDM_EMU_CHEATSEARCH) return 1;
  if (id >= IDM_EMU_REGION_BASE && id < IDM_EMU_REGION_BASE + 5) return 1;
  if (id >= IDM_EMU_VDPMODE_BASE && id < IDM_EMU_VDPMODE_BASE + 3) return 1;
  if (id >= IDM_EMU_SYSTEM_BASE && id < IDM_EMU_SYSTEM_BASE + 9) return 1;
  if (id >= IDM_EMU_LOCKON_BASE && id < IDM_EMU_LOCKON_BASE + 4) return 1;
  if (id == IDM_EMU_BIOS || id == IDM_EMU_ADDRERROR || id == IDM_EMU_CDLATENCY) return 1;
  if (id == IDM_AUDIO_HQPSG) return 1;       /* re-creates the PSG: would put the two games out of step */
  if (id >= IDM_AUDIO_FMCORE_BASE && id < IDM_AUDIO_FMCORE_BASE + 5) return 1;
  if (id >= IDM_SET_MODEL_BASE && id <= IDM_SET_MODEL_BASE + 2) return 1;
  if ((id >= IDM_SET_FMUNIT_BASE && id < IDM_SET_FMUNIT_BASE + 3) || (id >= IDM_SET_OPLL_BASE && id < IDM_SET_OPLL_BASE + 2)) return 1;   /* changes the FM chip type */
  if (id >= IDM_AUDIO_RATE_BASE && id < IDM_AUDIO_RATE_BASE + 3) return 1;
  if (id == IDM_INPUT_P1) return 1;
  if (id >= IDM_INPUT_PORTA_BASE && id < IDM_INPUT_PORTA_BASE + PORT_CHOICES) return 1;
  if (id >= IDM_INPUT_PORTB_BASE && id < IDM_INPUT_PORTB_BASE + PORT_CHOICES) return 1;
  return 0;
}

/****************************************************************************
 * Recording
 ****************************************************************************/

/* Something blocking just ran on the frame thread (finishing a recording, writing a screenshot): the frame clock and the
   fps count start again, so the pause is not made up for by a burst of frames or shown as a drop in fps. */
static void frame_clock_restart(void)
{
  next_frame_time = 0.0;
  frames_this_second = 0;
  fps_tick = GetTickCount();
  gui_diag_start();                       /* with Enable Log, the next seconds are logged: fps, slowest frame, sound queue */
}

static void record_stop_ex(int async)
{
  char path[GUI_PATH_LEN], err[256];
  int failed;

  if (!recorder_active()) return;

  failed = recorder_failed();
  lstrcpynA(path, recorder_path(), sizeof(path));
  lstrcpynA(err, recorder_error(), sizeof(err));

  if (async) recorder_stop_async(); else recorder_stop();
  frame_clock_restart();

  status_apply_prefixes();
  gui_update_menu();

  if (failed)
  {
    char msg[GUI_PATH_LEN + 384];
    wsprintfA(msg, "Recording stopped: %s\n\nWhat was recorded up to that point was kept in:\n%s", err, path);
    MessageBoxA(g_hwnd, msg, APP_NAME, MB_OK | MB_ICONWARNING);
  }
  else
  {
    gui_log("Recording file: %s", path);
    gui_notify("Recording saved");
  }
}

static void record_stop(void)
{
  record_stop_ex(0);
  recorder_stop();       /* also waits for a recording that was stopped in the background and is still being finished */
}

static void record_start(int mode)
{
  char path[GUI_PATH_LEN];

  if (!emu_running || recorder_active()) return;

  /* Each kind in a folder of its own inside recordings, and in it a folder for the console. */
  recording_file_path((mode == REC_VIDEO) ? "Video" : (mode == REC_GIF) ? "GIF" : "Audio",
                      (mode == REC_VIDEO) ? "mp4" : (mode == REC_GIF) ? "gif" : "wav", path, (int)sizeof(path));

  recorder_set_video_options(gui.rec_scale + 1, gui.rec_quality ? 2 << gui.rec_quality : 0, gui.rec_tv);
  if (!recorder_start(mode, path, (int)snd.sample_rate, emu_frame_rate()))
  {
    char msg[512];
    wsprintfA(msg, "Could not start recording:\n\n%s", recorder_error());
    MessageBoxA(g_hwnd, msg, APP_NAME, MB_OK | MB_ICONWARNING);
    return;
  }

  gui_update_menu();
  gui_notify("Recording %s", (mode == REC_VIDEO) ? "video + audio" : (mode == REC_GIF) ? "an animated GIF" : "audio only");
}

/* `record`: this is a normal-speed frame, so it belongs in a recording.
   Fast-forward and frame-advance frames are left out, which keeps the picture
   and sound continuous instead of leaving gaps or sped-up sections. */
static double timer_lag_frames;   /* how far behind schedule, in frames; only used when sound is off */

/* Waiting for the display's refresh (VSync) can take up to a whole refresh.
   Only allowed when there is room for that: at least two frames of sound queued
   (or, with sound off, the frame timer not running behind). */
static void set_vsync_allowed(void)
{
  int ok;

  if (gui.sound_enabled && !waveout_broken()) ok = (waveout_pending() >= 2);
  else                   ok = (timer_lag_frames < 0.5);
  video_set_vsync_allowed(ok);
}


static int skip_render;      /* run this frame without drawing it (frameskip) */


/* What the taskbar button shows: rewind history left, fast forward, or a recording under way. */
static void taskbar_update(int rewinding, int rewind_pct, int fast)
{
  int rec = recorder_active();

  if (rewinding)          taskbar_progress(g_hwnd, 1, rewind_pct);
  else if (fast)          taskbar_progress(g_hwnd, 2, 0);
  else if (rec == REC_GIF) { int p = (int)(recorder_seconds() * 100 / 6); taskbar_progress(g_hwnd, 1, p > 100 ? 100 : p); }
  else if (rec)           taskbar_progress(g_hwnd, 2, 0);
  else                    taskbar_progress(g_hwnd, 0, 0);
}

static double now_seconds(void);

static void run_one_frame(int present_video, int record)
{
  int samples;

  /* Mute FM / Mute PSG: the chip's level is taken to 0 for this frame only, so the saved levels stay as they were. */
  int keep_fm = config.fm_preamp, keep_psg = config.psg_preamp;
  if (gui.mute_fm)  config.fm_preamp  = 0;
  if (gui.mute_psg) config.psg_preamp = 0;

  /* The PSG only picks its level up when its stereo register is written, which games
     seldom do, so the level is pushed in here: every frame while muted, once on un-muting. */
  {
    static int psg_was_muted;
    if (!pico_active() && (gui.mute_psg || psg_was_muted))
      psg_config(0, gui.mute_psg ? 0 : (unsigned)keep_psg,
                 ((system_hw & SYSTEM_PBC) == SYSTEM_MD) ? 0xff : io_reg[6]);
    psg_was_muted = gui.mute_psg;
  }

  if (pico_active())
  {
    /* A 32X game: PicoDrive runs the frame and hands over the picture (into the frame buffer) and the sound. A change
       of the picture setting is dealt with first: clearing the buffer afterwards would blank the frame just drawn. */
    if (bitmap.viewport.changed & 1)
    {
      bitmap.viewport.changed &= ~1;
      video_viewport_changed();
    }
    cheats_ram_update();                    /* RAM cheats go back in each frame (the 68000's work RAM of the library) */
    samples = pico_run_frame(soundframe, (int)(sizeof(soundframe) / sizeof(soundframe[0]) / 2));
    if (pico_take_rate_change()) emu_apply_audio_settings();      /* Sample Rate was changed: the sound output takes the new rate */
    if (config.filter == 2 && samples > 0)  /* the 3-Band EQ (the library has none of its own): the same filter the Genesis Plus GX core uses */
    {
      static EQSTATE eq32[2];
      static int eq_sig;
      int sig = config.low_freq * 31 + config.high_freq * 17 + config.lg * 7 + config.mg * 13 + config.hg * 29 + pico_sample_rate(), j;

      if (sig != eq_sig)
      {
        init_3band_state(&eq32[0], config.low_freq, config.high_freq, pico_sample_rate());
        init_3band_state(&eq32[1], config.low_freq, config.high_freq, pico_sample_rate());
        eq32[0].lg = eq32[1].lg = (double)config.lg / 100.0;
        eq32[0].mg = eq32[1].mg = (double)config.mg / 100.0;
        eq32[0].hg = eq32[1].hg = (double)config.hg / 100.0;
        eq_sig = sig;
      }
      for (j = 0; j < 2 * samples; j++)
      {
        int v = (int)do_3band(&eq32[j & 1], soundframe[j]);
        soundframe[j] = (int16)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
      }
    }
    if (config.mono)                        /* Mono Output (the Genesis Plus GX core does this for its own games) */
    {
      int j;
      for (j = 0; j < samples; j++)
      {
        int m = (soundframe[2 * j] + soundframe[2 * j + 1]) / 2;
        soundframe[2 * j] = soundframe[2 * j + 1] = (int16)m;
      }
    }
    if (skip_render) present_video = 0;
  }
  else
  {
    /* RAM patches are overwritten by the game, so they go back in each frame. */
    cheats_ram_update();

    if (system_hw == SYSTEM_MCD)              system_frame_scd(skip_render);
    else if ((system_hw & SYSTEM_PBC) == SYSTEM_MD) system_frame_gen(skip_render);
    else                                      system_frame_sms(skip_render);

    /* Single Field: the same field every frame. The core draws the other one in turn (and the lines of one are half a line off the
       other's), so a picture of the two shakes, its text flickering between two looks. It flips the flag at the start of a frame:
       left set, the next frame is the even field again. */
    if (interlaced && !config.render && ((system_hw & SYSTEM_PBC) == SYSTEM_MD)) odd_frame = 1;

    if (skip_render) present_video = 0;
    else video_deinterlace();

    if (bitmap.viewport.changed & 1)
    {
      bitmap.viewport.changed &= ~1;
      video_viewport_changed();
    }

    /* audio_update must run every frame or the blip buffers overrun, even
       when the samples are then thrown away. */
    samples = audio_update(soundframe);
  }

  config.fm_preamp  = (int16)keep_fm;
  config.psg_preamp = (int16)keep_psg;

  /* Sound at every game speed (Emulation > Game Speed, Slow Motion). Faster than normal: only every Nth frame's sound is
     played, at its own pitch (so it is choppy). Slower: the sound is stretched to last longer, so it plays lower. */
  if (gui.sound_enabled && waveout_pending() < gui.latency && samples > 0)
  {
    double eff = netplay_active() ? 100.0 : (double)speed_pct;     /* the speed in percent */
    int play = 1;

    if (eff > 101.0)
    {
      static int phase;
      int n = (int)(eff / 100.0 + 0.5);
      play = (phase++ % n) == 0;
    }

    if (play)
    {
      static int16 stretched[2 * 4200], silent[2 * 4200];
      const int16 *src = soundframe;
      int n_out = samples;

      if (eff < 99.0 && samples <= 1024)
      {
        int j;
        n_out = (int)(samples * 100.0 / eff + 0.5);
        if (n_out > 4200) n_out = 4200;
        for (j = 0; j < n_out; j++)          /* linear interpolation between neighboring samples */
        {
          double pos = (double)j * (double)samples / (double)n_out;
          int a = (int)pos, b = (a + 1 < samples) ? a + 1 : a;
          double f = pos - (double)a;
          stretched[2 * j]     = (int16)(soundframe[2 * a]     + (soundframe[2 * b]     - soundframe[2 * a])     * f);
          stretched[2 * j + 1] = (int16)(soundframe[2 * a + 1] + (soundframe[2 * b + 1] - soundframe[2 * a + 1]) * f);
        }
        src = stretched;
      }

      /* "Mute When Inactive": silence of the same length keeps the timing the sound sets. */
      if (gui.mute_unfocused && GetActiveWindow() == NULL && n_out * 2 <= (int)(sizeof(silent) / sizeof(silent[0])))
      {
        memset(silent, 0, (size_t)n_out * 2 * sizeof(int16));
        src = silent;
      }

      while (n_out > 0)                      /* the device takes up to 2048 frames at a time */
      {
        int c = n_out > 2048 ? 2048 : n_out;
        waveout_submit(src, c);
        src += 2 * c;
        n_out -= c;
      }
    }
  }

  if (record && recorder_active())
  {
    recorder_capture(soundframe, samples);
    if (recorder_failed() || recorder_done()) record_stop_ex(1);
  }

  /* Limit Redraws: the game runs on, but the picture is only drawn this often. */
  if (present_video && gui.frame_cap > 0 && !recorder_active())
  {
    static double last_present;
    double now = now_seconds(), gap = 1.0 / (double)(20 + 10 * gui.frame_cap);
    if (now - last_present < gap * 0.92) present_video = 0;
    else last_present = now;
  }

  if (present_video)
  {
    double d0 = now_seconds(), d1;
    set_vsync_allowed();
    video_frame();
    d1 = now_seconds() - d0;
    if (d1 > diag_draw) diag_draw = d1;
  }

  frames_this_second++;

  if (resume_marker[0] && ++resume_marker_frames >= 120)      /* the resumed game has run for a while: the resume point is fine */
  {
    DeleteFileA(resume_marker);
    resume_marker[0] = '\0';
  }

  /* A frame that was skipped was never drawn, so its picture is stale. */
  vgm_frame_end();
  if (!skip_render) rewind_capture_tick(soundframe, samples);
}

static double now_seconds(void)
{
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return (double)c.QuadPart / (double)perf_freq.QuadPart;
}

static double diag_last, diag_gap, diag_work;   /* the slowest frame, and the longest wait between two frames, in the last second */
static DWORD diag_until;            /* log the frame rate and the sound queue each second until this time (after a controller change) */

void gui_diag_start(void)
{
  diag_until = GetTickCount() + 12000;
}

static void tick_fps(void)
{
  DWORD now = GetTickCount();

  if (now - fps_tick > 1500)      /* the clock stood still (a pause, a dragged window): a fresh second, not one stretched over the gap */
  {
    frames_this_second = 0;
    fps_tick = now;
    return;
  }

  if (now - fps_tick >= 1000)
  {
    int fps = (int)(((DWORD)frames_this_second * 1000) / (now - fps_tick));
    video_report_fps(fps);
    status_set_fps(fps);
    if (gui.log_on && (int)(diag_until - now) > 0)
      gui_log("After a controller change: %d fps, sound queue %d of %d, slowest frame %d ms (drawing %d ms), longest wait between frames %d ms, %s%s", fps,
              gui.sound_enabled ? waveout_pending() : -1, gui.latency, (int)(diag_work * 1000.0), (int)(diag_draw * 1000.0), (int)(diag_gap * 1000.0),
              emu_paused ? "paused" : "running", (gui.sound_enabled && waveout_broken()) ? ", sound device lost" : "");
    diag_work = diag_gap = diag_draw = 0.0;
    frames_this_second = 0;
    fps_tick = now;

    if (recorder_active() || netplay_state() != NP_IDLE) status_apply_prefixes();
  }
}

/* Input > Hotkeys changed a shortcut: the table Windows reads them from, and the menu labels. */
void gui_hotkeys_changed(void)
{
  HACCEL old = g_accel;
  g_accel = hotkeys_build_accel();
  if (old) DestroyAcceleratorTable(old);
  hotkeys_apply_menu();
}

/* Shows or hides the menu bar while fullscreen. Driven by the mouse: see
   tick_fullscreen_menu_hover() below. */
static void set_fullscreen_menu(int show)
{
  int showing = (GetMenu(g_hwnd) != NULL);

  if (show == showing) return;

  SetMenu(g_hwnd, show ? g_menu : NULL);
  if (show) theme_paint_menubar(g_hwnd, 1);

  if (!show)
  {
    /* Menu bar just got detached. Every way of asking Windows to redraw
       the non-client area on its own (DrawMenuBar()+InvalidateRect(),
       RedrawWindow() with RDW_FRAME, an actual resize-and-restore)
       failed to reliably clear it in testing -- painting directly over
       where it was doesn't depend on any of that, it's just an ordinary
       GDI fill like every other frame in this app already does. */
    RECT rc;
    HDC hdc = GetDC(NULL);

    if (hdc)
    {
      GetWindowRect(g_hwnd, &rc);
      rc.bottom = rc.top + GetSystemMetrics(SM_CYMENU);
      FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
      ReleaseDC(NULL, hdc);
    }
  }

  video_invalidate();
  InvalidateRect(g_hwnd, NULL, TRUE);
}

/* The mouse pointer hides after it has been still for a while (Settings > Video > Hide Mouse Pointer), in fullscreen
   and over a running game in a window. Hiding repeats ShowCursor(FALSE) because its internal counter has to go
   negative; any movement brings the pointer back. */
static void tick_cursor_hide(void)
{
  static const int ms[5] = { 0, 2000, 3000, 5000, 10000 };
  static POINT last;
  static DWORD last_move;
  static int hidden;
  POINT p;
  RECT rc;
  DWORD now = GetTickCount();
  int over_game = 0, limit = ms[gui.cursor_idle >= 0 && gui.cursor_idle < 5 ? gui.cursor_idle : 2];

  if (GetCursorPos(&p) && GetClientRect(g_hwnd, &rc))
  {
    POINT c = p;
    ScreenToClient(g_hwnd, &c);
    if (gui.fullscreen) over_game = 1;
    else if (emu_running && PtInRect(&rc, c) && GetForegroundWindow() == g_hwnd)
    {
      HWND w = WindowFromPoint(p);
      char cls[32] = "";
      if (w && w != g_hwnd) GetClassNameA(w, cls, sizeof(cls));
      over_game = (w == g_hwnd) || lstrcmpA(cls, "GPEXD3D11Surface") == 0;   /* the picture, not the status bar or toolbar */
    }
  }

  if (limit == 0 || !over_game || in_modal_loop || menu_loop_active || gui_cursor_depth > 0)    /* (a window that asked for the pointer is open) */
  {
    last_move = now;
    if (hidden) { while (ShowCursor(TRUE) < 0) { } hidden = 0; }
    last = p;
    return;
  }

  if (p.x != last.x || p.y != last.y)
  {
    last = p;
    last_move = now;
    if (hidden) { while (ShowCursor(TRUE) < 0) { } hidden = 0; }
    else if (ShowCursor(TRUE) < 1) { while (ShowCursor(TRUE) < 0) { } }      /* hidden by something else (a window's own count): moving shows it */
    else ShowCursor(FALSE);                                                   /* it was visible: the count goes back */
  }
  else if (!hidden && now - last_move >= (DWORD)limit)
  {
    while (ShowCursor(FALSE) >= 0) { }
    hidden = 1;
  }
}

/* Fullscreen: the menu bar appears when the cursor touches the top edge of
   the screen and goes away again once the cursor leaves the bar. The cursor
   itself stays visible, and hides after a few idle seconds (repeated
   ShowCursor(FALSE) calls, since its internal counter needs to go negative
   to actually hide); any movement brings it back. */
static void tick_fullscreen_menu_hover(void)
{
  POINT p;
  RECT rc;

  if (!gui.fullscreen) return;
  if (in_modal_loop || menu_loop_active) return;             /* a menu or dialog is open: the person is using the mouse */
  if (!GetCursorPos(&p) || !GetWindowRect(g_hwnd, &rc)) return;

  if (GetMenu(g_hwnd)) theme_paint_menubar(g_hwnd, 0);   /* the item under the mouse lights up */

  if (!GetMenu(g_hwnd))
  {
    if (p.y <= rc.top + 1 && p.x >= rc.left && p.x < rc.right)
    {
      set_fullscreen_menu(1);
      browser_fullscreen_toolbar(1);   /* the toolbar comes with the menu bar */
    }
  }
  else if (p.y > rc.top + GetSystemMetrics(SM_CYMENU) + browser_fullscreen_toolbar_shown_height() + 2 ||
           p.x < rc.left || p.x >= rc.right)
  {
    browser_fullscreen_toolbar(0);
    set_fullscreen_menu(0);
  }
  else
  {
    static DWORD last_refresh;
    DWORD now = GetTickCount();

    if (now - last_refresh > 400) { last_refresh = now; browser_fullscreen_toolbar_refresh(); }
  }
}

static void tick_status_expiry(void)
{
  DWORD now = GetTickCount();

  if (status_transient_until && now >= status_transient_until)
  {
    status_transient_until = 0;
    status_apply_prefixes();
  }
}

/* Frameskip. The sound card sets the pace, so "running behind" shows up as its
   queue running low: skipping the drawing of a frame (the emulation and the
   sound still run) gives the machine time to catch up. Auto skips when the
   queue is nearly empty; Manual when it falls below 25/33/50/75%. At most four
   frames in a row, so the picture keeps moving. Never during netplay (both
   sides must run identical frames) or while recording (it would repeat frames). */
static int frameskip_wanted(void)
{
  static const int threshold[6] = { 0, 25, 25, 33, 50, 75 };
  static int in_a_row;
  int behind;

  if (!gui.frameskip || netplay_active() || recorder_active())
  {
    in_a_row = 0;
    return 0;
  }

  if (gui.sound_enabled && !waveout_broken() && gui.latency > 0)
    behind = (waveout_pending() * 100 / gui.latency) < threshold[gui.frameskip];
  else
    behind = timer_lag_frames > 1.0;

  if (behind && in_a_row < 4)
  {
    in_a_row++;
    return 1;
  }
  in_a_row = 0;
  return 0;
}

/* Sound normally sets the pace. If the device disappears (headset unplugged,
   driver reset) it would otherwise either stall the game or let it run flat
   out, so the frame timer takes over until the device can be opened again.
   Returns 1 while the timer should be used. */
static int audio_pacing_lost(void)
{
  static DWORD last_try;
  DWORD now;

  if (!gui.sound_enabled || !waveout_broken()) return 0;

  now = GetTickCount();
  if (now - last_try > 2000)
  {
    last_try = now;
    gui_log("Sound: the device is lost, trying to open it again");
    waveout_close();
    if (waveout_open(run_rate_now()))
    {
      waveout_set_volume(gui.volume);
      return 0;
    }
  }
  return 1;
}

/* Buffers are queued but the device is not finishing any. */
static void audio_recover(void)
{
  static DWORD last;
  static int stage;
  DWORD now = GetTickCount();

  if (now - last > 10000) stage = 0;
  last = now;
  gui_log("Sound: the device stopped finishing buffers, recovering (step %d)", stage + 1);

  if (stage++ == 0)
  {
    waveout_flush();                       /* first try: just clear the queue */
  }
  else
  {
    waveout_close();                       /* then reopen the device */
    if (waveout_open(run_rate_now())) waveout_set_volume(gui.volume);
    else gui_status("The sound device is not responding; running without it for now");
  }
}

/* Rewind History Size: the memory the history may use. Returns 0 when it could not be had. */
static int rewind_apply_memory(void)
{
  static const int mb[4] = { 128, 256, 512, 768 };
  return rewind_set_memory(mb[gui.rewind_memory < 0 || gui.rewind_memory > 3 ? 1 : gui.rewind_memory]);
}

static void on_command(int id);
static void emulation_step(void);

/****************************************************************************
 * Frame pacer
 *
 * While a window of this program is open (Settings, Controls...) or being dragged, Windows runs that window's own message
 * loop, and the game only gets a turn when a message reaches the main window. A timer (WM_TIMER) is the lowest priority
 * message there is and ticks at the system clock's 15 ms at best, so the game stuttered. This thread posts an ordinary
 * message every 2 ms instead, which the loops dispatch ahead of timers, painting and mouse moves.
 ****************************************************************************/

#define WM_APP_PACE (WM_APP + 41)
#define WM_APP_FSBAR (WM_APP + 42)           /* fullscreen: paint the menu bar dark again */
static int fs_bar_sel = -1;              /* the title of the menu bar that is open (fullscreen) */

static HANDLE pace_ev, pace_thr;
static volatile LONG pace_quit, pace_pending;
static int pace_depth;

static DWORD WINAPI pace_proc(LPVOID arg)
{
  (void)arg;
  for (;;)
  {
    WaitForSingleObject(pace_ev, INFINITE);        /* signalled while a window is open */
    if (pace_quit) break;
    if (g_hwnd && !InterlockedExchange(&pace_pending, 1)) PostMessage(g_hwnd, WM_APP_PACE, 0, 0);
    Sleep(2);
  }
  return 0;
}

/* A window of this program that runs its own message loop opens (on) or closes (off). */
void gui_pace(int on)
{
  if (!pace_ev)
  {
    pace_ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (pace_ev) pace_thr = CreateThread(NULL, 0, pace_proc, NULL, 0, NULL);
    if (pace_thr) SetThreadPriority(pace_thr, THREAD_PRIORITY_ABOVE_NORMAL);
  }
  if (!pace_ev) return;
  if (on) { if (pace_depth++ == 0) SetEvent(pace_ev); }
  else if (pace_depth > 0 && --pace_depth == 0) ResetEvent(pace_ev);
}

static void pace_shutdown(void)
{
  if (!pace_thr) return;
  pace_quit = 1;
  SetEvent(pace_ev);
  WaitForSingleObject(pace_thr, 500);
  CloseHandle(pace_thr);
  CloseHandle(pace_ev);
  pace_thr = NULL;
  pace_ev = NULL;
}

/* The clock, power and scheduling the frame loop depends on. By default Windows wakes a waiting thread on a 15.6 ms
   tick (a 1 ms wait lasts up to 15 ms, so a frame comes late and the sound queue runs dry at the first hiccup), and
   recent versions let the system ignore a program's own request for a finer clock when its window is hidden or the
   machine is saving power. */
static void timing_boost(void)
{
  typedef struct { ULONG Version, ControlMask, StateMask; } PWR_THROTTLE;
  typedef BOOL (WINAPI *set_info_fn)(HANDLE, int, LPVOID, DWORD);
  typedef HANDLE (WINAPI *mm_char_fn)(LPCSTR, LPDWORD);
  HMODULE k = GetModuleHandleA("kernel32.dll"), av;
  set_info_fn set_info = k ? (set_info_fn)(void *)GetProcAddress(k, "SetProcessInformation") : NULL;

  timeBeginPeriod(1);
  if (set_info)
  {
    PWR_THROTTLE p;
    p.Version = 1;
    p.ControlMask = 0x1 | 0x4;       /* execution speed, ignore timer resolution */
    p.StateMask = 0;                 /* ... both left to this program, not throttled */
    set_info(GetCurrentProcess(), 4 /* ProcessPowerThrottling */, &p, sizeof(p));
  }
  SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

  av = LoadLibraryExA("avrt.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);       /* the multimedia scheduler: a game thread is kept on time */
  if (av)
  {
    mm_char_fn fn = (mm_char_fn)(void *)GetProcAddress(av, "AvSetMmThreadCharacteristicsA");
    DWORD idx = 0;
    if (fn) fn("Games", &idx);
  }
}

static int step_nested;

void gui_run_step(void)
{
  static int busy;
  if (busy || !emu_running) return;
  busy = 1;
  step_nested++;
  emulation_step();
  step_nested--;
  busy = 0;
}

/* Settings-window choices that have no menu item: a base command plus one per item,
   each backed by one number in gui. */
/* The menus are drawn by the program (with the Modern font) or by the system: after the Style changed. */
static void apply_menu_font(void)
{
  HMENU old = g_menu;
  int attached = GetMenu(g_hwnd) != NULL;               /* (not in fullscreen: the bar is detached then) */

  if (gui.large_ui) return;                              /* (The Large interface draws the menus itself, in either style) */

  /* A new menu is made, as at the start: changing the items of the old one in place left the sizes Windows had worked out for them. */
  g_menu = LoadMenuA(g_inst, MAKEINTRESOURCEA(IDR_MAINMENU));
  if (!g_menu) { g_menu = old; return; }
  theme_set_menu_bar(g_menu);
  if (theme_menus_owner()) theme_ownerdraw_menu(g_menu);
  theme_menu_icons(g_menu);
  hotkeys_apply_menu();
  if (attached) SetMenu(g_hwnd, g_menu);
  rebuild_recent_menu();
  rebuild_filter_menu();
  gui_update_menu();
  DrawMenuBar(g_hwnd);
  theme_unconvert_menu(old);                             /* (gives back the strings and pictures the owner-drawn items held) */
  DestroyMenu(old);
}

static const struct { int base, n; int *val; } setgroups[] =
{
  { IDM_SET_STEREO_BASE,    4, &gui.stereo_idx },
  { IDM_SET_MOUSESENS_BASE, 5, &gui.mouse_sens },
  { IDM_SET_GUNX_BASE,      7, &gui.gun_ox },
  { IDM_SET_GUNY_BASE,      7, &gui.gun_oy },
  { IDM_SET_BARCOLOR_BASE,  7, &gui.bar_color },
  { IDM_SET_FSMODE_BASE,    2, &gui.fs_exclusive },
  { IDM_SET_COVERSHAPE_BASE, 3, &gui.cover_shape },
  { IDM_SET_ACCENT_BASE,    GUI_ACCENT_HIDDEN + 1, &gui.accent },       /* the last is a hidden one (eggs.c) */
  { IDM_SET_TBSIZE_BASE,    3, &gui.toolbar_size },
  { IDM_SET_CURSORIDLE_BASE, 5, &gui.cursor_idle },
  { IDM_SET_FRAMECAP_BASE,  5, &gui.frame_cap },
  { IDM_SET_BASS_BASE,      4, &gui.bass },
  { IDM_SET_AUTOSAVE_BASE,  5, &gui.autosave_min },
  { IDM_SET_SKIPLEN_BASE,   7, &gui.skip_len },
  { IDM_SET_REWINDSPEED_BASE, 4, &gui.rewind_speed },
  { IDM_SET_OSDPOS_BASE,    4, &gui.osd_pos },
  { IDM_SET_OSDTIME_BASE,   7, &gui.osd_time },
  { IDM_SET_BACKEND_BASE,   4, &gui.audio_backend },
  { IDM_SET_OSDSIZE_BASE,   4, &gui.osd_size },
  { IDM_SET_OSDCOLOR_BASE,  8, &gui.osd_color },
  { IDM_SET_OSDLINES_BASE,  7, &gui.osd_lines },
  { IDM_SET_FPSPOS_BASE,    4, &gui.fps_pos },
  { IDM_SET_STYLE_BASE,     2, &gui.ui_style },
};

/* Console model presets (Audio menu): Default is the program's own sound set-up (YM2612 Nuked, no filter), Model 1 the duller early console (YM2612 and
   its low-pass filter), Model 2 the cleaner late one (YM3438, no filter). */
static int model_pick;

static int model_matches(int mdl)
{
  if (mdl == 2) return config.ym3438 && !gui.nuked_ym2612 && config.filter == 0;
  if (!config.ym3438 || !gui.nuked_ym2612) return 0;
  return mdl == 1 ? (config.filter == 1 && config.lp_range == 0x9999) : config.filter == 0;
}

static int model_current(void)
{
  int k;
  if (model_matches(model_pick)) return model_pick;
  for (k = 0; k < 3; k++) if (model_matches(k)) return k;
  return -1;
}

/* The Picture Adjustments sliders: the command id of each, the number it sets, and its range. */
static const struct { int id; int *val; int lo, hi, def; } sliders[] =
{
  { IDM_SET_CONTRAST_BASE, &gui.adj_contrast, 50, 150, 100 },
  { IDM_SET_SATUR_BASE,    &gui.adj_sat,       0, 200, 100 },
  { IDM_SET_GAMMA_BASE,    &gui.adj_gamma,    50, 150, 100 },
  { IDM_SET_HUE_BASE,      &gui.adj_hue,     -30,  30,   0 },
};

int gui_slider_range(int id, int *lo, int *hi, int *def)
{
  size_t k;
  for (k = 0; k < sizeof(sliders) / sizeof(sliders[0]); k++)
    if (sliders[k].id == id) { *lo = sliders[k].lo; *hi = sliders[k].hi; *def = sliders[k].def; return 1; }
  return 0;
}

int gui_slider_get(int id)
{
  size_t k;
  for (k = 0; k < sizeof(sliders) / sizeof(sliders[0]); k++) if (sliders[k].id == id) return *sliders[k].val;
  return 0;
}

void gui_slider_set(int id, int v)
{
  size_t k;
  for (k = 0; k < sizeof(sliders) / sizeof(sliders[0]); k++)
    if (sliders[k].id == id)
    {
      v = (v >= 0 ? v + 5 : v - 5) / 10 * 10;      /* in steps of 10: 1.10, 1.20 ... for Gamma */
      if (v < sliders[k].lo) v = sliders[k].lo;
      if (v > sliders[k].hi) v = sliders[k].hi;
      *sliders[k].val = v;
      video_adjust_changed();
    }
}

/* The sliders are saved when the thumb is let go (every step of a drag would write the file). */
void gui_slider_commit(void)
{
  config_save();
}

/* The entries of a drop-down filled at run time; returns how many, and which is chosen. */
int gui_dyn_list(int base, char names[][96], int max, int *cur)
{
  int n = 0, i;

  *cur = 0;
  if (base == IDM_SET_DEVICE_BASE)
  {
    char dev[16][96];
    int nd = waveout_list_devices(dev, 16);
    lstrcpyA(names[n++], "Windows Default");
    for (i = 0; i < nd && n < max; i++)
    {
      lstrcpynA(names[n], dev[i], 96);
      if (gui.audio_device[0] && !*cur && !strncmp(dev[i], gui.audio_device, strlen(gui.audio_device))) *cur = n;      /* (a name saved earlier may be cut short) */
      n++;
    }
  }
  else if (base == IDM_SET_FSRES_BASE)
  {
    int ws[60], hs[60], nm = video_fullscreen_modes(ws, hs, 59);
    lstrcpyA(names[n++], "Desktop Resolution");
    for (i = 0; i < nm && n < max; i++)
    {
      snprintf(names[n], 64, "%d x %d", ws[i], hs[i]);
      if (ws[i] == gui.fs_res_w && hs[i] == gui.fs_res_h) *cur = n;
      n++;
    }
  }
  return n;
}

/* The cover size choices of the grid view (Settings > General): pixels for Tiny to Extra Large (Medium is the default). Ctrl + wheel can
   set sizes in between; those show as the nearest choice. */
static const int cover_sizes[7] = { 48, 64, 96, 128, 176, 216, 256 };

static int cover_size_index(void)
{
  int i, best = 0;
  for (i = 1; i < 7; i++)
    if (abs(gui.browser_grid_size - cover_sizes[i]) < abs(gui.browser_grid_size - cover_sizes[best])) best = i;
  return best;
}

int gui_setgroup_state(int id)
{
  size_t k;

  /* choices that are no longer in a menu */
  if (id >= IDM_EMU_LOCKON_BASE && id < IDM_EMU_LOCKON_BASE + 4) return config.lock_on == id - IDM_EMU_LOCKON_BASE;
  if (id >= IDM_TOOLS_SHOT_BASE && id < IDM_TOOLS_SHOT_BASE + 3) return gui.shot_mode == id - IDM_TOOLS_SHOT_BASE;
  if (id >= IDM_TOOLS_SHOTSIZE_BASE && id < IDM_TOOLS_SHOTSIZE_BASE + 4) return gui.shot_size == id - IDM_TOOLS_SHOTSIZE_BASE;
  if (id >= IDM_SET_RECSIZE_BASE && id < IDM_SET_RECSIZE_BASE + 4) return gui.rec_scale == id - IDM_SET_RECSIZE_BASE;
  if (id >= IDM_SET_RECQUAL_BASE && id < IDM_SET_RECQUAL_BASE + 5) return gui.rec_quality == id - IDM_SET_RECQUAL_BASE;
  if (id >= IDM_SET_RECSHAPE_BASE && id < IDM_SET_RECSHAPE_BASE + 3) return gui.rec_tv == id - IDM_SET_RECSHAPE_BASE;
  if (id >= IDM_EMU_REWINDMEM_BASE && id < IDM_EMU_REWINDMEM_BASE + 4) return gui.rewind_memory == id - IDM_EMU_REWINDMEM_BASE;       /* the Rewind submenu is gone: Settings asks here */

  /* the FM chip and the sample rate are no longer in the Audio menu, so Settings asks here */
  if (id >= IDM_AUDIO_FMCORE_BASE && id < IDM_AUDIO_FMCORE_BASE + 5)
    return (config.ym3438 ? (gui.nuked_ym2612 ? 0 : 1) : ((config.ym2612 > 2) ? 4 : config.ym2612 + 2)) == id - IDM_AUDIO_FMCORE_BASE;
  if (id >= IDM_AUDIO_RATE_BASE && id < IDM_AUDIO_RATE_BASE + 3)
    return ((gui.sample_rate == 22050) ? 0 : ((gui.sample_rate == 44100) ? 1 : 2)) == id - IDM_AUDIO_RATE_BASE;
  if (id >= IDM_SET_UISIZE_BASE && id < IDM_SET_UISIZE_BASE + 2) return (gui.large_ui ? 1 : 0) == id - IDM_SET_UISIZE_BASE;
  if (id >= IDM_SET_FMUNIT_BASE && id < IDM_SET_FMUNIT_BASE + 3)
    return ((config.ym2413 & 2) ? 0 : ((config.ym2413 & 1) ? 2 : 1)) == id - IDM_SET_FMUNIT_BASE;
  if (id >= IDM_SET_OPLL_BASE && id < IDM_SET_OPLL_BASE + 2)
    return (config.opll ? 0 : 1) == id - IDM_SET_OPLL_BASE;
  if (id >= IDM_SET_COVERSIZE_BASE && id < IDM_SET_COVERSIZE_BASE + 7) return cover_size_index() == id - IDM_SET_COVERSIZE_BASE;
  for (k = 0; k < sizeof(setgroups) / sizeof(setgroups[0]); k++)
    if (id >= setgroups[k].base && id < setgroups[k].base + setgroups[k].n)
      return *setgroups[k].val == id - setgroups[k].base;
  return -1;
}

static void stereo_apply(void)
{
  static const int pct[4] = { 50, 100, 150, 200 };
  waveout_set_width(pct[gui.stereo_idx >= 0 && gui.stereo_idx < 4 ? gui.stereo_idx : 1]);
  waveout_set_enhance(gui.bass, gui.normalize, gui.overdrive);
}

static BOOL CALLBACK redraw_all_cb(HWND w, LPARAM lp)
{
  (void)lp;
  RedrawWindow(w, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN);      /* the program's windows, with the new accent color */
  return TRUE;
}

int gui_setgroup_set(int id)
{
  size_t k;

  if (id >= IDM_SET_COVERSIZE_BASE && id < IDM_SET_COVERSIZE_BASE + 7)
  {
    gui.browser_grid_size = cover_sizes[id - IDM_SET_COVERSIZE_BASE];
    browser_cover_style_changed();                    /* rebuilds the covers at the new size when the grid is showing */
    return 1;
  }

  for (k = 0; k < sizeof(setgroups) / sizeof(setgroups[0]); k++)
    if (id >= setgroups[k].base && id < setgroups[k].base + setgroups[k].n)
    {
      *setgroups[k].val = id - setgroups[k].base;
      if (setgroups[k].base == IDM_SET_STEREO_BASE) stereo_apply();
      else if (setgroups[k].base == IDM_SET_BACKEND_BASE)
      {
        if (gui.sound_enabled)
        {
          waveout_close();
          if (waveout_open(run_rate_now())) waveout_set_volume(gui.volume);
          else gui_status("That sound output is not available; sound is off");
        }
      }
      else if (setgroups[k].base == IDM_SET_BARCOLOR_BASE) video_adjust_changed();
      else if (setgroups[k].base == IDM_SET_OSDSIZE_BASE) { video_osd_font_changed(); video_invalidate(); }
      else if (setgroups[k].base == IDM_SET_OSDPOS_BASE || setgroups[k].base == IDM_SET_FPSPOS_BASE || setgroups[k].base == IDM_SET_OSDCOLOR_BASE || setgroups[k].base == IDM_SET_OSDLINES_BASE) video_invalidate();
      else if (setgroups[k].base == IDM_SET_COVERSHAPE_BASE) browser_cover_style_changed();
      else if (setgroups[k].base == IDM_SET_TBSIZE_BASE) browser_bars_changed();        /* the toolbar is laid out again at its new size */
      else if (setgroups[k].base == IDM_SET_ACCENT_BASE) EnumThreadWindows(GetCurrentThreadId(), redraw_all_cb, 0);
      else if (setgroups[k].base == IDM_SET_STYLE_BASE)
      {
        theme_apply_to_window(g_hwnd);                  /* the window frame, controls and fonts at once (Settings follows itself) */
        if (menu_loop_active) { menu_rebuild_pending = 1; EndMenu(); }     /* (the open menu belongs to the menu bar that is replaced: it closes, and the new one is made) */
        else apply_menu_font();
        RedrawWindow(g_hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
      }
      return 1;
    }
  return 0;
}

/* The in-game pause menu: opened with a pad button (Controls > Gamepad Hotkeys > Pause Menu) or its shortcut, moved with
   the D-pad / arrow keys, and left with B / Esc. The game is paused while it is up. */
static void pause_menu_hide(void)
{
  if (!pm_open) return;
  pm_open = 0;
  video_show_menu(NULL, 0, 0);
}

static void pause_menu_close(int resume)
{
  pause_menu_hide();
  emu_paused = resume ? 0 : pm_was_paused;
  gui_update_menu();
}

static void pause_menu_choose(void)
{
  int sel = pm_sel;

  pause_menu_close(1);
  switch (sel)
  {
    case 1: on_command(IDM_ACCEL_QUICKSAVE); break;
    case 2: on_command(IDM_ACCEL_QUICKLOAD); break;
    case 3: on_command(IDM_EMU_RESET); break;
    case 4: on_command(IDM_EMU_STOP); break;
    default: break;
  }
}

static void pause_menu_move(int d)
{
  pm_sel = (pm_sel + d + 5) % 5;
  video_show_menu(pm_items, 5, pm_sel);
}

/* A key press while the pause menu is up: 1 = used it. */
static int pause_menu_key(WPARAM vk)
{
  if (!pm_open) return 0;
  switch (vk)
  {
    case VK_UP:     pause_menu_move(-1); return 1;
    case VK_DOWN:   pause_menu_move(1); return 1;
    case VK_RETURN: pause_menu_choose(); return 1;
    case VK_ESCAPE: pause_menu_close(0); return 1;
  }
  return 0;
}

/* Keeps the pointer inside the picture while a mouse or light gun game has the focus. */
static int mouse_confined;

static void mouse_confine_update(void)
{
  int want = 0, i;

  if (gui.confine_mouse && emu_running && !emu_paused && GetForegroundWindow() == g_hwnd && !IsIconic(g_hwnd))
    for (i = 0; i < MAX_DEVICES; i++)
      if (input.dev[i] == DEVICE_MOUSE || input.dev[i] == DEVICE_LIGHTGUN) want = 1;

  if (want)
  {
    RECT d;
    POINT a, b;
    RECT sr;

    video_get_output_rect(&d, NULL, NULL);
    a.x = d.left; a.y = d.top; b.x = d.right; b.y = d.bottom;
    ClientToScreen(g_hwnd, &a);
    ClientToScreen(g_hwnd, &b);
    sr.left = a.x; sr.top = a.y; sr.right = b.x; sr.bottom = b.y;
    if (sr.right > sr.left && sr.bottom > sr.top) { ClipCursor(&sr); mouse_confined = 1; }
  }
  else if (mouse_confined)
  {
    ClipCursor(NULL);
    mouse_confined = 0;
  }
}

/* Waits for input or the next frame. From a window's own message loop (the game keeps running behind Settings, say) the loop
   is the caller's, so there is nothing to wait for here. */
static void step_wait(DWORD n, const HANDLE *h, DWORD ms)
{
  if (step_nested) return;
  MsgWaitForMultipleObjects(n, h, FALSE, ms, QS_ALLINPUT);
}

static void emulation_step(void)
{
  int fast, i, count;
  DWORD now_ms = GetTickCount();

  stereo_apply();
  mouse_confine_update();

  tick_status_expiry();
  tick_cursor_hide();
  tick_fullscreen_menu_hover();
  backup_ram_autosave(now_ms);
  backup_state_tick(now_ms);

  /* Keeps Windows from sleeping, dimming the screen or starting the screen saver while a game is open. */
  if (emu_running && gui.keep_awake)
  {
    static DWORD awake_tick;
    if (!awake_tick || now_ms - awake_tick > 5000)
    {
      awake_tick = now_ms ? now_ms : 1;
      SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
    }
  }

  /* Accept a joining player, read the socket, run the handshake. */
  netplay_poll();

  if (np_pending_msg[0])
  {
    char text[300];

    lstrcpynA(text, np_pending_msg, sizeof(text));
    np_pending_msg[0] = '\0';
    MessageBoxA(g_hwnd, text, APP_NAME, MB_OK | MB_ICONWARNING);
    next_frame_time = 0.0;
  }

  if (emu_running && !in_modal_loop && !netplay_active())
  {
    int slot;

    slot = gui_input_save_slot_shortcut();
    if (slot >= 0)
    {
      gui.state_slot = slot;
      emu_save_state(slot);      /* reports its own result */
      gui_update_menu();
    }

    slot = gui_input_load_slot_shortcut();
    if (slot >= 0)
    {
      gui.state_slot = slot;
      emu_load_state(slot);      /* reports its own result */
      gui_update_menu();
    }
  }

  /* Gamepad hotkeys: a press of the button set for a shortcut. */
  if (emu_running && !in_modal_loop && !netplay_active())
  {
    int c = gui_input_pad_hotkey();
    if (c) on_command(c);
    if (pm_open)
    {
      int e = gui_input_menu_pad();
      if (e & 1) pause_menu_move(-1);
      else if (e & 2) pause_menu_move(1);
      else if (e & 4) pause_menu_choose();
      else if (e & 8) pause_menu_close(0);
    }
  }

  /* A session can't pause: the other player's game keeps waiting on this one. */
  if (netplay_active() && emu_paused)
  {
    emu_paused = 0;
    gui_update_menu();
  }

  if (!emu_running || emu_paused || in_modal_loop)
  {
    taskbar_update(0, 0, 0);
    if (emu_running && emu_paused && !netplay_active() && gui_input_frame_advance())
    {
      run_one_frame(1, 0);
      return;
    }

    step_wait(0, NULL, 16);
    return;
  }

  if (gui.rewind && !netplay_active() && gui_input_rewind())
  {
    /* Matches normal playback's own cadence -- snapshots are now captured
       every single frame (see rewind.c), so popping one every 60th of a
       second reads as real-time-speed reverse motion instead of jumping. */
    double now = now_seconds();
    double period = (gui.rewind_speed == 0 ? 2.0 : 1.0) / 60.0;         /* slow: a step every other 60th of a second */
    int steps = gui.rewind_speed == 2 ? 2 : gui.rewind_speed == 3 ? 4 : 1;      /* fast: two steps at a time, very fast: four */
    int rewind_ok, k;

    if (next_rewind_time == 0.0 || now - next_rewind_time > 0.5)
    {
      next_rewind_time = now;
      rewind_bar_ref = 0;                 /* a new rewind: its bar starts full */
    }
    else if (now < next_rewind_time)
    {
      step_wait(0, NULL, 1);
      return;
    }
    next_rewind_time += period;

    rewind_ok = rewind_step();
    for (k = 1; rewind_ok && k < steps; k++) if (!rewind_step()) break;       /* (the last step's picture and sound are the ones shown) */
    if (rewind_ok)
    {
      int audio_frames;
      const int16 *audio;

      rewind_hit_limit_notified = 0;

      {
        int left = rewind_status();
        if (left > rewind_bar_ref) rewind_bar_ref = left;
        taskbar_update(1, rewind_bar_ref > 0 ? left * 100 / rewind_bar_ref : 0, 0);
        if (gui.osd) video_show_rewind(rewind_bar_ref > 0 ? left * 1000 / rewind_bar_ref : 0, left * 10 / (int)(emu_frame_rate() + 0.5));
      }

      /* rewind_step() already restored bitmap.data directly -- no
         forward frame needed to regenerate it, just present what's
         there. */
      video_frame();

      /* Reversed sample order, not forward playback -- this is what
         actually produces the characteristic reversed-sound rewind
         effect, matching the reversed picture. */
      audio = rewind_get_audio(&audio_frames);
      if (gui.sound_enabled && audio_frames > 0 && waveout_pending() < gui.latency)
      {
        waveout_submit(audio, audio_frames);
      }
    }
    else if (!rewind_hit_limit_notified)
    {
      rewind_hit_limit_notified = 1;
      gui_notify("Rewind limit reached");
    }

    next_frame_time = 0.0;
    tick_fps();
    return;
  }

  if (!emu_running || netplay_active()) skip_frames_left = 0;
  fast = netplay_active() ? 0 : (gui_input_fast_forward() || skip_frames_left > 0);
  taskbar_update(0, 0, fast);

  {
    /* Fast forward shows a notice when it starts and when it stops, whether it is the held key or the tick. */
    if (fast != fast_osd_state)
    {
      fast_osd_state = fast;
      gui_notify(fast ? "Fast forward" : "Normal speed");
    }
  }

  if (!fast)
  {
    /* Lockstep: wait for the other player's input for this frame *before*
       touching the frame timer, or every stall would push the schedule
       forward without a frame being run and the game would slow down. */
    if (netplay_active() && !netplay_frame_ready())
    {
      HANDLE h = netplay_wait_handle();

      if (h) step_wait(1, &h, 4);
      else   step_wait(0, NULL, 4);
      return;
    }

    if (gui.sound_enabled && !audio_pacing_lost() && (speed_pct == 100 || netplay_active()))
    {
      /* Let the sound card set the pace. */
      if (waveout_pending() >= gui.latency)
      {
        if (waveout_stalled()) audio_recover();
        step_wait(0, NULL, 1);
        return;
      }
    }
    else
    {
      double now = now_seconds();
      double period = (1.0 / emu_frame_rate()) * 100.0 / (netplay_active() ? 100 : speed_pct);

      if (next_frame_time == 0.0 || now - next_frame_time > 0.5)
      {
        next_frame_time = now;
      }
      else if (now < next_frame_time)
      {
        step_wait(0, NULL, 1);
        return;
      }
      timer_lag_frames = (now - next_frame_time) / period;
      next_frame_time += period;
    }

    if (netplay_active())
    {
      /* Readiness was checked above. */
      netplay_frame_begin();
      run_one_frame(1, 1);
      netplay_frame_end();
    }
    else
    {
      double t0 = now_seconds();

      if (diag_last > 0.0 && t0 - diag_last > diag_gap) diag_gap = t0 - diag_last;
      skip_render = frameskip_wanted();
      run_one_frame(1, 1);
      skip_render = 0;
      diag_last = now_seconds();
      if (diag_last - t0 > diag_work) diag_work = diag_last - t0;
    }
  }
  else
  {
    /* Draw one frame in every fast_forward_ratio to keep the window alive. */
    count = gui.fast_forward_ratio;
    for (i = 0; i < count; i++)
    {
      run_one_frame(i == count - 1, 0);
      if (skip_frames_left > 0)
      {
        /* A press of any game button ends the skip. */
        if (--skip_frames_left == 0 || (input.pad[0] & (INPUT_UP | INPUT_DOWN | INPUT_LEFT | INPUT_RIGHT | INPUT_A | INPUT_B | INPUT_C |
                                                         INPUT_START | INPUT_X | INPUT_Y | INPUT_Z | INPUT_MODE)))
          skip_frames_left = 0;
      }
    }
    next_frame_time = 0.0;
  }

  tick_fps();
}

/****************************************************************************
 * Commands
 ****************************************************************************/

/* Shaders need librashader.dll and the Direct3D 11 renderer; says so (once
   per attempt) when either is missing. Returns 1 when shaders can be used. */
int gui_shader_available(void)
{
  void *device;

  if (!shader_chain_available())
  {
    char text[MAX_PATH + 400];
    shader_chain_missing_text(text, (int)sizeof(text));
    MessageBoxA(g_hwnd, text, APP_NAME, MB_OK | MB_ICONWARNING);
    return 0;
  }
  if (gui.renderer != RENDERER_D3D11 || !d3d11_get_frame_resources(&device, NULL, NULL))
  {
    MessageBoxA(g_hwnd, "Shaders need the Direct3D 11 renderer.\n\n"
                        "Pick it under Video > Renderer first.",
                APP_NAME, MB_OK | MB_ICONWARNING);
    return 0;
  }
  return 1;
}

/* Switches to the shader preset at `path`, or turns shaders off when `path`
   is NULL or empty. Returns 1 on success. */
int gui_set_shader(const char *path)
{
  void *device;

  if (!path || !path[0])
  {
    shader_chain_unload();
    gui.shader_preset_path[0] = '\0';
    gui_update_menu();
    config_save();
    return 1;
  }

  if (!gui_shader_available()) return 0;
  if (!d3d11_get_frame_resources(&device, NULL, NULL)) return 0;

  if (shader_chain_load(device, path))
  {
    lstrcpynA(gui.shader_preset_path, path, sizeof(gui.shader_preset_path));
    gui_update_menu();
    config_save();
    return 1;
  }

  {
    char msg[GUI_PATH_LEN + 128];
    wsprintfA(msg, "Could not load that shader preset:\n\n%s", shader_chain_last_error());
    MessageBoxA(g_hwnd, msg, APP_NAME, MB_OK | MB_ICONWARNING);
  }
  return 0;
}

/* Set from the command line (see parse_command_line()). */
static int cli_fullscreen = -1;    /* -1: as set in the program */
static int cli_exit_on_close;
static int cli_state = -1;
static int cli_help;
static int cli_restarted;       /* --restarted: started by the Large interface's restart, so not handed to the copy that is closing */
static int restart_closing;     /* this copy is closing for that restart: no "stop the game?" question */
static int cli_mute, cli_paused, cli_scale = -1;
static char cli_state_file[GUI_PATH_LEN];

static void on_command_inner(int id);

/* The text of a menu item without its & and its shortcut. */
static void menu_item_text(HMENU menu, int pos_or_id, int by_pos, char *out, int len)
{
  char *p;
  out[0] = '\0';
  GetMenuStringA(menu, (UINT)pos_or_id, out, len, by_pos ? MF_BYPOSITION : MF_BYCOMMAND);
  if ((p = strchr(out, '\t')) != NULL) *p = '\0';
  for (p = out; *p; )
  {
    if (*p == '&') memmove(p, p + 1, strlen(p));     /* (a doubled && is rare enough to ignore) */
    else p++;
  }
}

/* The title of the popup that holds command `id` directly. */
static int menu_parent_title(HMENU menu, int id, char *out, int len)
{
  int i, n = GetMenuItemCount(menu);

  for (i = 0; i < n; i++)
  {
    HMENU sub = GetSubMenu(menu, i);
    int j, k;
    if (!sub) continue;
    k = GetMenuItemCount(sub);
    for (j = 0; j < k; j++)
      if ((int)GetMenuItemID(sub, j) == id) { menu_item_text(menu, i, 1, out, len); return 1; }
    if (menu_parent_title(sub, id, out, len)) return 1;
  }
  return 0;
}

/* Commands go through here: an option that changed and said nothing gets an on-screen message of its own
   ("Smooth Scaling: On", "Scanlines: 50%"). */
static void on_command(int id)
{
  UINT before = g_menu ? GetMenuState(g_menu, (UINT)id, MF_BYCOMMAND) : (UINT)-1;
  int shown = notify_count;

  on_command_inner(id);

  if (!gui.osd_opts || before == (UINT)-1 || notify_count != shown || !g_menu) return;
  {
    UINT after = GetMenuState(g_menu, (UINT)id, MF_BYCOMMAND);
    MENUITEMINFOA mi;
    char item[96], group[96];

    if (after == (UINT)-1 || !((before ^ after) & MF_CHECKED)) return;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    mi.fMask = MIIM_FTYPE;
    GetMenuItemInfoA(g_menu, (UINT)id, FALSE, &mi);
    menu_item_text(g_menu, id, 0, item, sizeof(item));

    if (mi.fType & MFT_RADIOCHECK)           /* one of a group: the group's name and the choice */
    {
      if ((after & MF_CHECKED) && menu_parent_title(g_menu, id, group, sizeof(group)))
        gui_notify("%s: %s", group, item);
    }
    else if (!lstrcmpiA(item, "None") || !lstrcmpiA(item, "Off") || !lstrcmpiA(item, "Auto") || !lstrcmpiA(item, "Default"))
    {
      /* a label that means nothing on its own: said with the name of the menu it is in */
      if ((after & MF_CHECKED) && menu_parent_title(g_menu, id, group, sizeof(group))) gui_notify("%s: %s", group, item);
    }
    else
      gui_notify("%s: %s", item, (after & MF_CHECKED) ? "On" : "Off");
  }
}

static void on_command_inner(int id)
{
  if (gui_pico_unsupported(id))
  {
    gui_notify("Not available for 32X games");
    return;
  }

  if (np_command_locked(id))
  {
    gui_notify("Not available during netplay");
    return;
  }

  if (mouse_confined) { ClipCursor(NULL); mouse_confined = 0; }   /* a menu or dialog is about to need the pointer */

  if (gui_setgroup_set(id))
  {
    gui_update_menu();
    config_save();
    return;
  }

  if (id == IDM_OPT_CONFINE)
  {
    gui.confine_mouse = !gui.confine_mouse;
    gui_update_menu();
    config_save();
    return;
  }

  if (id >= IDM_SET_DEVICE_BASE && id < IDM_SET_DEVICE_BASE + 40)
  {
    char names[16][96];
    int n = waveout_list_devices(names, 16), k = id - IDM_SET_DEVICE_BASE;

    lstrcpynA(gui.audio_device, (k >= 1 && k <= n) ? names[k - 1] : "", sizeof(gui.audio_device));
    if (gui.sound_enabled)
    {
      waveout_close();
      if (waveout_open(run_rate_now())) waveout_set_volume(gui.volume);
      else gui_status("That sound device is not available; sound is off");
    }
    config_save();
    return;
  }

  if (id >= IDM_SET_FSRES_BASE && id < IDM_SET_FSRES_BASE + 60)
  {
    int ws[60], hs[60], n = video_fullscreen_modes(ws, hs, 59), k = id - IDM_SET_FSRES_BASE;

    gui.fs_res_w = (k >= 1 && k <= n) ? ws[k - 1] : 0;
    gui.fs_res_h = (k >= 1 && k <= n) ? hs[k - 1] : 0;
    config_save();
    return;
  }

  /* Ranged commands first. */
  if (id >= IDM_FILE_RECENT_BASE && id < IDM_FILE_RECENT_BASE + GUI_RECENT_MAX)
  {
    int i = id - IDM_FILE_RECENT_BASE;
    if (gui.recent[i][0])
    {
      char path[GUI_PATH_LEN];
      lstrcpynA(path, gui.recent[i], sizeof(path));
      emu_load_rom(path);
    }
    return;
  }

  if (id >= IDM_FILE_LOADSLOT_BASE && id < IDM_FILE_LOADSLOT_BASE + GUI_SLOT_MAX)
  {
    emu_load_state(id - IDM_FILE_LOADSLOT_BASE);
    return;
  }

  if (id >= IDM_FILE_SAVESLOT_BASE && id < IDM_FILE_SAVESLOT_BASE + GUI_SLOT_MAX)
  {
    emu_save_state(id - IDM_FILE_SAVESLOT_BASE);
    gui_update_menu();
    return;
  }

  if (id >= IDM_FILE_SLOT_BASE && id < IDM_FILE_SLOT_BASE + GUI_SLOT_MAX)
  {
    gui.state_slot = id - IDM_FILE_SLOT_BASE;
    gui_update_menu();
    gui_notify("Slot %d selected", gui.state_slot);
    config_save();
    return;
  }

  /* Force VDP Mode: run the video timing as NTSC (60 Hz) or PAL (50 Hz) whatever
     the region says. Applied the same way as a region change. */
  if (id >= IDM_EMU_VDPMODE_BASE && id < IDM_EMU_VDPMODE_BASE + 3)
  {
    config.vdp_mode = (uint8)(id - IDM_EMU_VDPMODE_BASE);
    if (pico_active()) pico_settings_changed();       /* a 32X game: the library takes it as a region (NTSC = US, PAL = Europe) */
    else emu_apply_region();
    gui_update_menu();
    config_save();
    return;
  }

  if (id >= IDM_EMU_REGION_BASE && id < IDM_EMU_REGION_BASE + 5)
  {
    config.region_detect = (uint8)(id - IDM_EMU_REGION_BASE);
    if (pico_active()) pico_settings_changed();       /* a 32X game: the library reads the region itself */
    else emu_apply_region();
    gui_update_menu();
    config_save();
    return;
  }

  if (id >= IDM_EMU_SYSTEM_BASE && id < IDM_EMU_SYSTEM_BASE + 9)
  {
    static const uint8 systems[] =
    {
      0, SYSTEM_SG, SYSTEM_SGII, SYSTEM_SGII_RAM_EXT, SYSTEM_MARKIII,
      SYSTEM_SMS, SYSTEM_SMS2, SYSTEM_GG, SYSTEM_MD
    };
    config.system = systems[id - IDM_EMU_SYSTEM_BASE];
    gui_update_menu();
    config_save();
    gui_status("Console setting applies the next time a ROM is loaded");
    return;
  }

  if (id >= IDM_EMU_LOCKON_BASE && id < IDM_EMU_LOCKON_BASE + 4)
  {
    config.lock_on = (uint8)(id - IDM_EMU_LOCKON_BASE);
    gui_update_menu();
    config_save();
    gui_status("Lock-on cartridge applies the next time a ROM is loaded");
    return;
  }

  if (id >= IDM_VIDEO_SCALE_BASE && id < IDM_VIDEO_SCALE_BASE + 6)
  {
    gui.scale = id - IDM_VIDEO_SCALE_BASE + 1;
    if (gui.fullscreen) video_set_fullscreen(0);
    if (IsZoomed(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);   /* picking a size leaves the maximized state properly */
    gui_resize_to_scale(gui.scale);
    gui_update_menu();
    config_save();
    return;
  }

  if (id >= IDM_VIDEO_ASPECT_BASE && id < IDM_VIDEO_ASPECT_BASE + 4)
  {
    gui.aspect = id - IDM_VIDEO_ASPECT_BASE;
    video_viewport_changed();
    if (!IsZoomed(g_hwnd)) gui_resize_to_scale(gui.scale);   /* a maximized window stays maximized */
    gui_update_menu();
    config_save();
    return;
  }

  if (id >= IDM_VIDEO_NTSC_BASE && id < IDM_VIDEO_NTSC_BASE + 4)
  {
    video_set_ntsc(id - IDM_VIDEO_NTSC_BASE);
    gui_update_menu();
    rebuild_filter_menu();   /* NTSC and a render filter are mutually exclusive */
    config_save();
    return;
  }

  if (id >= IDM_VIDEO_FILTER_BASE && id < IDM_VIDEO_FILTER_BASE + FILTER_MENU_MAX)
  {
    int i = id - IDM_VIDEO_FILTER_BASE;
    if (i < video_filter_count())
    {
      video_set_filter(i);
      rebuild_filter_menu();
      config_save();
    }
    return;
  }

  if (id >= IDM_TOOLS_SHOTSIZE_BASE && id < IDM_TOOLS_SHOTSIZE_BASE + 4)
  {
    gui.shot_size = id - IDM_TOOLS_SHOTSIZE_BASE;
    config_save();
    return;
  }

  if (id >= IDM_SET_RECSIZE_BASE && id < IDM_SET_RECSIZE_BASE + 4)   { gui.rec_scale = id - IDM_SET_RECSIZE_BASE; config_save(); return; }
  if (id >= IDM_SET_RECQUAL_BASE && id < IDM_SET_RECQUAL_BASE + 5)   { gui.rec_quality = id - IDM_SET_RECQUAL_BASE; config_save(); return; }
  if (id >= IDM_SET_RECSHAPE_BASE && id < IDM_SET_RECSHAPE_BASE + 3) { gui.rec_tv = id - IDM_SET_RECSHAPE_BASE; config_save(); return; }

  if (id >= IDM_TOOLS_SHOT_BASE && id < IDM_TOOLS_SHOT_BASE + 3)
  {
    gui.shot_mode = id - IDM_TOOLS_SHOT_BASE;
    gui_update_menu();
    config_save();
    return;
  }

  /* Interlaced Mode: single field, or double field (both fields, twice the height). */
  if (id >= IDM_VIDEO_INTERLACE_BASE && id < IDM_VIDEO_INTERLACE_BASE + 2)
  {
    config.render = (uint8)(id - IDM_VIDEO_INTERLACE_BASE);
    if (emu_running && !pico_active()) { bitmap.viewport.changed = 3; video_viewport_changed(); }       /* (a 32X game has no such mode: it is kept for the other games) */
    gui_update_menu();
    config_save();
    return;
  }

  /* LCD ghosting: how much of a bright pixel lingers into the next frame. */
  if (id >= IDM_VIDEO_LCD_BASE && id < IDM_VIDEO_LCD_BASE + 4)
  {
    static const uint8 lcd_rate[4] = { 0, 128, 160, 192 };      /* Light was 96 (hardly to be seen), Heavy 224 (too much) */
    config.lcd = lcd_rate[id - IDM_VIDEO_LCD_BASE];
    gui_update_menu();
    config_save();
    return;
  }

  if (id >= IDM_VIDEO_FRAMESKIP_BASE && id < IDM_VIDEO_FRAMESKIP_BASE + 6)
  {
    gui.frameskip = id - IDM_VIDEO_FRAMESKIP_BASE;
    gui_update_menu();
    config_save();
    return;
  }

  if (id >= IDM_VIDEO_OVERSCAN_BASE && id < IDM_VIDEO_OVERSCAN_BASE + 4)
  {
    config.overscan = (uint8)(id - IDM_VIDEO_OVERSCAN_BASE);
    if (!pico_active())                                  /* (a 32X game has no borders to show: it is kept for the other games) */
    {
      emu_apply_sms_border();
      if (emu_running) bitmap.viewport.changed = 3;
      video_viewport_changed();
    }
    gui_update_menu();
    config_save();
    return;
  }

  if (id >= IDM_VIDEO_SCANLINE_BASE && id < IDM_VIDEO_SCANLINE_BASE + 5)
  {
    gui.scanline_pct = (id - IDM_VIDEO_SCANLINE_BASE) * 25;
    video_invalidate();
    gui_update_menu();
    config_save();
    return;
  }

  if (id >= IDM_VIDEO_THEME_BASE && id < IDM_VIDEO_THEME_BASE + 3)
  {
    theme_set_mode(id - IDM_VIDEO_THEME_BASE);
    theme_apply_to_window(g_hwnd);
    theme_menu_icons(g_menu);                    /* the pictures follow the text color of the theme */
    RedrawWindow(g_hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    gui_update_menu();
    config_save();
    return;
  }

  if (id >= IDM_VIEW_COL_BASE && id < IDM_VIEW_COL_BASE + 2)
  {
    browser_toggle_column(id - IDM_VIEW_COL_BASE);
    gui_update_menu();
    return;
  }

  if (id == IDM_VIEW_SEARCHBAR)
  {
    browser_toggle_searchbar();
    gui_update_menu();
    return;
  }

  if (id == IDM_VIEW_FSTOOLBAR)
  {
    gui.fullscreen_toolbar = !gui.fullscreen_toolbar;
    browser_fullscreen_toolbar(0);
    gui_update_menu();
    config_save();
    return;
  }

  if (id >= IDM_VIEW_COL_EXTRA && id < IDM_VIEW_COL_EXTRA + 4)
  {
    browser_toggle_column(3 + (id - IDM_VIEW_COL_EXTRA));
    gui_update_menu();
    return;
  }

  if (id == IDM_OPT_TBLABELS)
  {
    gui.toolbar_labels = !gui.toolbar_labels;
    browser_bars_changed();
    gui_update_menu();
    config_save();
    return;
  }

  if (id == IDM_OPT_COVERCACHE)
  {
    gui.cover_cache = !gui.cover_cache;
    coverart_set_cache(gui.cover_cache);
    gui_update_menu();
    config_save();
    return;
  }

  if (id == IDM_OPT_COVERCACHE_CLEAR)
  {
    char msg[96];
    int n = coverart_clear_cache();
    wsprintfA(msg, n ? "Removed %d saved cover%s." : "There were no saved covers.", n, n == 1 ? "" : "s");
    MessageBoxA(g_hwnd, msg, APP_NAME, MB_OK | MB_ICONINFORMATION);
    return;
  }

  if (id == IDM_OPT_CRASHREPORT)
  {
    gui.crash_report = !gui.crash_report;
    gui_update_menu();
    config_save();
    return;
  }

  if (id == IDM_VIEW_GROUPCONSOLE || id == IDM_VIEW_AZSTRIP || id == IDM_OPT_COVERROUND || id == IDM_OPT_COVERCAP || id == IDM_OPT_COVERCARD)
  {
    if (id == IDM_VIEW_GROUPCONSOLE) { gui.group_console = !gui.group_console; browser_refilter(); }
    else if (id == IDM_VIEW_AZSTRIP) { gui.az_strip = !gui.az_strip; browser_layout_changed(); }
    else if (id == IDM_OPT_COVERCAP) { gui.cover_caption = !gui.cover_caption; browser_cover_style_changed(); }
    else if (id == IDM_OPT_COVERCARD) { gui.cover_card = !gui.cover_card; browser_cover_style_changed(); }
    else { gui.cover_round = !gui.cover_round; browser_cover_style_changed(); }
    gui_update_menu();
    config_save();
    return;
  }

  if (id == IDM_FILE_ROMFOLDERS)
  {
    dlg_rom_folders(g_hwnd);
    return;
  }

  if (id == IDM_VIEW_SORT_RECENT)
  {
    browser_toggle_recent();
    gui_update_menu();
    return;
  }

  if (id == IDM_VIEW_COL_FAV)
  {
    browser_toggle_column(2);
    gui_update_menu();
    return;
  }

  if (id == IDM_VIEW_GRIDLINES)
  {
    gui.browser_gridlines = !gui.browser_gridlines;
    browser_redraw();
    gui_update_menu();
    config_save();
    return;
  }

  if (id >= IDM_VIEW_DENSITY_BASE && id < IDM_VIEW_DENSITY_BASE + 3)
  {
    gui.row_density = id - IDM_VIEW_DENSITY_BASE;
    browser_density_changed();
    gui_update_menu();
    config_save();
    return;
  }

  if (id == IDM_OPT_COLORICONS)
  {
    gui.color_icons = !gui.color_icons;
    theme_menu_icons(g_menu);                    /* the menu pictures again, in color or black and white */
    gui_update_menu();
    RedrawWindow(g_hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE | RDW_UPDATENOW);      /* the toolbar and the star of the list */
    config_save();
    return;
  }

  if (id == IDM_OPT_LOG || id == IDM_OPT_PAUSEPAD || id == IDM_OPT_NORMALIZE ||
      id == IDM_OPT_OVERDRIVE || id == IDM_OPT_OSDOPTS)
  {
    if (id == IDM_OPT_LOG) { gui.log_on = !gui.log_on; if (gui.log_on) { gui_log("Logging switched on"); log_system_info(); } }
    else if (id == IDM_OPT_PAUSEPAD) gui.pause_pad = !gui.pause_pad;
    else if (id == IDM_OPT_NORMALIZE) gui.normalize = !gui.normalize;
    else if (id == IDM_OPT_OVERDRIVE) gui.overdrive = !gui.overdrive;
    else if (id == IDM_OPT_OSDOPTS) gui.osd_opts = !gui.osd_opts;
    gui_update_menu();
    config_save();
    return;
  }

  if (id == IDM_OPT_CONFIRMEXIT || id == IDM_OPT_KEEPAWAKE || id == IDM_OPT_MUTEUNFOCUSED || id == IDM_OPT_SINGLEINSTANCE)
  {
    if (id == IDM_OPT_CONFIRMEXIT) gui.confirm_exit = !gui.confirm_exit;
    else if (id == IDM_OPT_SINGLEINSTANCE) gui.single_instance = !gui.single_instance;
    else if (id == IDM_OPT_KEEPAWAKE) { gui.keep_awake = !gui.keep_awake; if (!gui.keep_awake) SetThreadExecutionState(ES_CONTINUOUS); }
    else gui.mute_unfocused = !gui.mute_unfocused;
    gui_update_menu();
    config_save();
    return;
  }

  if (id == IDM_VIEW_SCANSUB)
  {
    gui.scan_subfolders = !gui.scan_subfolders;
    browser_rescan_now();
    config_save();
    return;
  }

  if (id == IDM_VIEW_CLEANNAMES)
  {
    gui.clean_names = !gui.clean_names;
    browser_refresh_names();
    rebuild_recent_menu();                            /* File > Recent Files follows it */
    gui_update_menu();
    config_save();
    return;
  }

  if (id == IDM_VIEW_FAVONLY)
  {
    browser_toggle_favorites_only();
    gui_update_menu();
    return;
  }

  if (id == IDM_VIEW_STATUSBAR)
  {
    RECT rc;
    gui.hide_statusbar = !gui.hide_statusbar;
    if (g_status && !gui.fullscreen) ShowWindow(g_status, gui.hide_statusbar ? SW_HIDE : SW_SHOW);
    GetClientRect(g_hwnd, &rc);
    SendMessage(g_hwnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right, rc.bottom));       /* the picture and the list take the room */
    InvalidateRect(g_hwnd, NULL, TRUE);
    gui_update_menu();
    config_save();
    return;
  }

  if (id == IDM_VIEW_TOOLBAR || id == IDM_VIEW_FILTERBAR)
  {
    if (id == IDM_VIEW_TOOLBAR)   gui.hide_toolbar   = !gui.hide_toolbar;
    if (id == IDM_VIEW_FILTERBAR) gui.hide_filterbar = !gui.hide_filterbar;
    browser_bars_changed();
    gui_update_menu();
    config_save();
    return;
  }

  if (id == IDM_OPTIONS_SETTINGS)
  {
    dlg_settings(g_hwnd);
    return;
  }

  if (id == IDM_OPTIONS_SETTINGS_VIDEO || id == IDM_OPTIONS_SETTINGS_AUDIO)
  {
    dlg_settings_tab(g_hwnd, id == IDM_OPTIONS_SETTINGS_VIDEO ? 1 : 2);   /* Video, Audio */
    return;
  }

  if (id == IDM_VIEW_FINDSEARCH)
  {
    if (!browser_panel_visible()) return;
    /* Ctrl+F never changes the saved View > Toolbars > Show Search Bar setting: the bar comes and goes for this run only. */
    if (browser_search_shown()) browser_hide_search_now();                    /* shown -> hidden */
    else browser_focus_search();                                              /* hidden -> shown, ready to type; goes away again when left empty */
    gui_update_menu();
    return;
  }

  if (id == IDM_VIEW_RESETWINDOW)
  {
    /* Back to the window size the Video > Window Size setting asks for. */
    if (gui.fullscreen) video_set_fullscreen(0);
    if (IsZoomed(g_hwnd) || IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
    gui_resize_to_scale(gui.scale);
    gui_center_window(g_hwnd);               /* and in the middle of the screen */
    gui_update_menu();
    return;
  }

  if (id == IDM_VIEW_LIST || id == IDM_VIEW_GRID)
  {
    gui.browser_grid_view = (id == IDM_VIEW_GRID);
    browser_apply_view_mode();
    gui_update_menu();
    config_save();
    return;
  }

  if (id >= IDM_SET_UISIZE_BASE && id < IDM_SET_UISIZE_BASE + 2)
  {
    if ((id - IDM_SET_UISIZE_BASE) != (gui.large_ui ? 1 : 0)) on_command(IDM_VIDEO_LARGE_UI);      /* the switch below, which asks to restart */
    return;
  }

  if (id == IDM_VIDEO_LARGE_UI)
  {
    gui.large_ui = !gui.large_ui;
    gui_update_menu();
    config_save();

    if (MessageBoxA(g_hwnd,
          "Genesis Plus EX needs to restart for this to take effect.\n\nRestart now?",
          "Interface Size", MB_YESNO | MB_ICONQUESTION) == IDYES)
    {
      char exe_path[GUI_PATH_LEN];
      char cmdline[GUI_PATH_LEN * 2 + 16];
      STARTUPINFOA si;
      PROCESS_INFORMATION pi;

      GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));

      /* Only hand the new instance a ROM if one is actually running. rom_path
         is also set by ROM Information / Edit Cheats on a game that was only
         inspected (emu_peek_rom), and that must not launch it. */
      if (emu_running && rom_path[0])
        wsprintfA(cmdline, "\"%s\" --restarted \"%s\"", exe_path, rom_path);
      else
        wsprintfA(cmdline, "\"%s\" --restarted", exe_path);

      ZeroMemory(&si, sizeof(si));
      si.cb = sizeof(si);
      ZeroMemory(&pi, sizeof(pi));

      /* Launch the replacement first, and only close this instance if
         that actually succeeded -- if CreateProcess failed for some
         reason, the setting is still saved (just applies next time the
         user starts the app some other way) rather than leaving them
         with no running instance at all. */
      if (CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
      {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        restart_closing = 1;
        PostMessage(g_hwnd, WM_CLOSE, 0, 0);
      }
    }
    return;
  }

  if (id >= IDM_SET_MODEL_BASE && id <= IDM_SET_MODEL_BASE + 2)
  {
    int mdl = id - IDM_SET_MODEL_BASE;
    static const char *const names[3] = { "Console Model: Default", "Console Model: Genesis Model 1", "Console Model: Genesis Model 2" };

    model_pick = mdl;
    on_command(IDM_AUDIO_FMCORE_BASE + (mdl == 2 ? 1 : 0));       /* YM2612 / YM3438 */
    config.filter = (uint8)(mdl == 1 ? 1 : 0);       /* only Model 1 has the low-pass filter */
    if (mdl == 1) config.lp_range = 0x9999;
    if (emu_running) audio_set_equalizer();
    if (pico_active()) pico_settings_changed();
    gui_update_menu();
    config_save();
    gui_notify("%s", names[mdl]);
    return;
  }

  if (id >= IDM_AUDIO_FMCORE_BASE && id < IDM_AUDIO_FMCORE_BASE + 5)
  {
    int choice = id - IDM_AUDIO_FMCORE_BASE;

    /* 0-1: Nuked (as a YM2612, as a YM3438). 2-4: MAME (YM2612 discrete,
       YM3438 ASIC, YM3438 enhanced). */
    config.ym3438 = (uint8)(choice <= 1);
    if (choice >= 2) config.ym2612 = (uint8)(choice - 2);
    if (choice <= 1) gui.nuked_ym2612 = (choice == 0);
    emu_apply_nuked_type();

    if (pico_active())                       /* a 32X game: PicoDrive has its own FM chip, told the type through its option */
    {
      pico_settings_changed();
      gui_update_menu();
      config_save();
      return;
    }

    if (emu_running)
    {
      /* Unlike Sample Rate, Enable Sound, HQPSG, or Region -- none of
         which actually need sound_init() at all, since its behavior
         depends purely on which FM core is configured, confirmed by
         reading it -- this menu is the one place that genuinely changes
         that configuration, so sound_init() actually has to run here.
         audio_init() then sound_reset() are the same fix as before:
         audio_init() rebuilds the blip buffers (skipping it left the OLD
         core's accumulator contents in place); sound_reset() resyncs
         fm_cycles_count/fm_cycles_start/fm_ptr with whatever ratio
         sound_init() just set up (skipping it crashed outright, since
         MAME's YM2612 and Nuked's YM3438 run at very different ratios).
         What this can't fix: MAME and Nuked are independent C
         implementations with no shared internal state, so switching
         between them can't carry over which instrument was playing on
         which channel -- the running game isn't told anything changed
         and won't re-send configuration it thinks is already in place.
         Some instruments may stay quiet until the game's own music driver
         naturally re-triggers them (a new area, a menu, etc.) or the game
         is reset. That part is inherent to swapping cores mid-run, not a
         bug this sequence can paper over. */
      audio_init(run_rate_now(), 0);
      sound_init();
      sound_reset();

      /* audio_reset() is what actually clears the blip buffers/low-pass
         filter state/re-syncs the equalizer -- see the longer note in
         emu_apply_audio_settings(). Missing it here was why some
         instruments stayed silent until an actual reset happened. */
      audio_reset();

      /* The new chip starts blank and the game will not send its instrument set-up again, so what the game had told
         the old chip and the PSG is written into the new one. */
      vgm_restore_sound();
    }
    gui_update_menu();
    config_save();
    return;
  }

  if (id >= IDM_SET_FMUNIT_BASE && id < IDM_SET_FMUNIT_BASE + 3)
  {
    int u = id - IDM_SET_FMUNIT_BASE;      /* 0 auto, 1 off, 2 on */
    config.ym2413 = (uint8)(u == 0 ? 2 : (u == 2 ? 1 : 0));
    emu_apply_fm_settings();
    gui_update_menu();
    config_save();
    return;
  }

  if (id >= IDM_SET_OPLL_BASE && id < IDM_SET_OPLL_BASE + 2)
  {
    config.opll = (uint8)(id == IDM_SET_OPLL_BASE ? 1 : 0);      /* Nuked first, MAME second */
    emu_apply_fm_settings();
    gui_update_menu();
    config_save();
    return;
  }

  if (id >= IDM_AUDIO_RATE_BASE && id < IDM_AUDIO_RATE_BASE + 3)
  {
    static const int rates[3] = { 22050, 44100, 48000 };
    gui.sample_rate = rates[id - IDM_AUDIO_RATE_BASE];
    if (pico_active()) pico_settings_changed();                   /* the library changes its rate; the sound output follows after the next frame */
    else emu_apply_audio_settings();
    gui_update_menu();
    config_save();
    return;
  }

  if (id >= IDM_INPUT_PORTA_BASE && id < IDM_INPUT_PORTA_BASE + PORT_CHOICES)
  {
    port_choose(0, port_menu_value(0, id - IDM_INPUT_PORTA_BASE));
    if (emu_running) io_init();
    gui_update_menu();
    config_save();
    return;
  }

  if (id >= IDM_INPUT_PORTB_BASE && id < IDM_INPUT_PORTB_BASE + PORT_CHOICES)
  {
    port_choose(1, port_menu_value(1, id - IDM_INPUT_PORTB_BASE));
    if (emu_running) io_init();
    gui_update_menu();
    config_save();
    return;
  }

  switch (id)
  {
    case IDM_FILE_OPEN:
      browse_for_rom();
      break;

    case IDM_FILE_BROWSER:
      browser_panel_change_folder();
      break;

    case IDM_FILE_ROMINFO:
      if (!emu_running && browser_has_selection()) browser_show_rom_info();
      else dlg_rom_info(g_hwnd);
      break;

    case IDM_FILE_STATEMGR:
      dlg_state_manager(g_hwnd);
      break;

    case IDM_FILE_CLOSE:
    case IDM_EMU_STOP:
      emu_close_rom();
      if (cli_exit_on_close) PostMessage(g_hwnd, WM_CLOSE, 0, 0);   /* back to the frontend */
      break;

    case IDM_FILE_RECENT_CLEAR:
    {
      int i;
      for (i = 0; i < GUI_RECENT_MAX; i++) gui.recent[i][0] = '\0';
      rebuild_recent_menu();
      config_save();
      break;
    }

    case IDM_FILE_SAVESTATE:
    case IDM_ACCEL_QUICKSAVE:
      emu_save_state(gui.state_slot);
      break;

    case IDM_FILE_LOADSTATE:
    case IDM_ACCEL_QUICKLOAD:
      emu_load_state(gui.state_slot);
      break;

    case IDM_FILE_LOADSTATEFILE:
    {
      char p[GUI_PATH_LEN];
      if (!emu_running) break;
      if (gui_pick_file(g_hwnd, GUI_PICK_STATE, "Load State from File",
                        "Genesis Plus EX State\0*.gpstate;*.gp0;*.gp1;*.gp2;*.gp3;*.gp4;*.gp5;*.gp6;*.gp7;*.gp8;*.gp9\0All Files\0*.*\0\0",
                        NULL, p, sizeof(p)))
        emu_load_state_file(p);
      break;
    }

    case IDM_FILE_SAVESTATEFILE:
    {
      char p[GUI_PATH_LEN], suggest[GUI_PATH_LEN];
      if (!emu_running) break;
      wsprintfA(suggest, "%s.gpstate", rom_base);
      if (gui_save_file(g_hwnd, "Save State to File", "Genesis Plus EX State\0*.gpstate\0All Files\0*.*\0\0",
                        "gpstate", suggest, p, sizeof(p)))
        emu_save_state_file(p);
      break;
    }

    case IDM_FILE_APPLYPATCH:
    {
      char target[GUI_PATH_LEN], patch[GUI_PATH_LEN], out[GUI_PATH_LEN], what[200];

      if (!emu_running) { gui_notify("Open a game first"); break; }
      lstrcpynA(target, rom_path, sizeof(target));
      if (!gui_pick_file(g_hwnd, GUI_PICK_STATE, "Apply a Patch", "Patches (.ips, .bps, .zip, .7z)\0*.ips;*.bps;*.zip;*.7z\0All Files\0*.*\0\0", osd_path("."), patch, sizeof(patch))) break;
      {
        /* A game in a .7z archive is unpacked to a temporary file first; the patched copy is what gets loaded. */
        char unpacked[GUI_PATH_LEN] = "";
        const char *source = target;
        int made;

        if (is_7z_path(target))
        {
          if (!extract_7z_rom(target, unpacked, sizeof(unpacked))) { MessageBoxA(g_hwnd, "The game in that .7z archive could not be read.", APP_NAME, MB_OK | MB_ICONWARNING); break; }
          source = unpacked;
        }
        made = patches_make_copy(patch, source, out, sizeof(out), what, sizeof(what));
        if (unpacked[0]) DeleteFileA(unpacked);
        if (!made) { MessageBoxA(g_hwnd, what, APP_NAME, MB_OK | MB_ICONWARNING); break; }
      }
      lstrcpynA(patch_override, out, sizeof(patch_override));
      emu_load_rom(target);
      patch_override[0] = '\0';
      gui_notify("%s", what);
      break;
    }

    case IDM_INPUT_PAUSEMENU:
      if (!emu_running || netplay_active()) break;
      if (pm_open) { pause_menu_close(0); break; }
      pm_was_paused = emu_paused;
      emu_paused = 1;
      pm_open = 1;
      pm_sel = 0;
      video_show_menu(pm_items, 5, 0);
      gui_update_menu();
      break;

    case IDM_VIDEO_SHADERNOFILTER:
      gui.shader_nofilter = !gui.shader_nofilter;
      video_invalidate();
      InvalidateRect(g_hwnd, NULL, TRUE);
      gui_update_menu();
      config_save();
      break;

    case IDM_VIDEO_SHADERPARAMS:
      extras_shader_params(g_hwnd);
      break;

    case IDM_TOOLS_VGM:
    {
      /* Started like the other recordings (a file named after the game and the time, in the VGM folder inside recordings) and ended
         with Stop Recording. */
      char f[GUI_PATH_LEN];
      if (!emu_running) { gui_notify("Open a game first"); break; }
      if (vgm_active() || recorder_active()) break;
      recording_file_path("VGM", "vgm", f, (int)sizeof(f));
      if (vgm_start(f))
      {
        const char *n = strrchr(f, '\\');
        lstrcpynA(vgm_name, n ? n + 1 : f, sizeof(vgm_name));
        gui_notify("Recording the sound chips (VGM)");
      }
      else MessageBoxA(g_hwnd, pico_active() ? "The file could not be created.\n\nFor a 32X game the library in the Plugins folder must be the one from the newest zip (an older picodrive_libretro DLL cannot record). Replace it and start the program again." : "The file could not be created.", APP_NAME, MB_OK | MB_ICONWARNING);
      gui_update_menu();
      break;
    }

    case IDM_TOOLS_MOVIE_REC:
    case IDM_TOOLS_MOVIE_PLAY:
    case IDM_TOOLS_MOVIE_STOP:
    {
      char f[GUI_PATH_LEN], err[120], start[GUI_PATH_LEN];

      if (id == IDM_TOOLS_MOVIE_STOP) { movie_stop(); break; }
      if (!emu_running) { gui_notify("Open a game first"); break; }
      if (id == IDM_TOOLS_MOVIE_REC)
      {
        /* Like the other recordings: a file named after the game and the time, in the Input folder inside recordings. */
        recording_file_path("Input", "gpm", f, (int)sizeof(f));
        if (!movie_record(f)) MessageBoxA(g_hwnd, "The recording could not be started.", APP_NAME, MB_OK | MB_ICONWARNING);
        else gui_notify("Recording the input");
      }
      else
      {
        input_recordings_dir(start, sizeof(start));
        if (gui_pick_file(g_hwnd, GUI_PICK_STATE, "Play Input Recording", "Input Recordings\0*.gpm\0\0", start, f, sizeof(f)) &&
            !movie_play(f, err, sizeof(err)))
          MessageBoxA(g_hwnd, err, APP_NAME, MB_OK | MB_ICONWARNING);
      }
      break;
    }

    case IDM_INPUT_COMBOS:
      dlg_input_combos(g_hwnd);
      break;

    case IDM_VIDEO_FLICKER: gui.flicker_blend = !gui.flicker_blend; video_adjust_changed(); gui_update_menu(); config_save(); break;
    case IDM_VIDEO_DITHER:  gui.dither_smooth = !gui.dither_smooth; video_adjust_changed(); gui_update_menu(); config_save(); break;
    case IDM_VIDEO_BEZEL:
    {
      char f[GUI_PATH_LEN];
      if (gui_pick_file(g_hwnd, GUI_PICK_COVER, "Choose a Bezel Image", "Images\0*.png;*.jpg;*.bmp\0All Files\0*.*\0\0", osd_path("Bezels"), f, sizeof(f)))
      {
        lstrcpynA(gui.bezel_path, f, sizeof(gui.bezel_path));
        video_adjust_changed();
        config_save();
      }
      break;
    }
    case IDM_VIDEO_BEZEL_OFF: gui.bezel_path[0] = '\0'; video_adjust_changed(); config_save(); break;

    case IDM_EMU_INSERTDISC:
      emu_insert_disc();
      break;

    case IDM_ACCEL_SLOT_NEXT:
      gui.state_slot = (gui.state_slot + 1) % GUI_SLOT_MAX;
      gui_update_menu();
      gui_notify("Slot %d selected", gui.state_slot);
      break;

    case IDM_ACCEL_SLOT_PREV:
      gui.state_slot = (gui.state_slot + GUI_SLOT_MAX - 1) % GUI_SLOT_MAX;
      gui_update_menu();
      gui_notify("Slot %d selected", gui.state_slot);
      break;

    case IDM_FILE_REC_VIDEO:
      if (recorder_active() == REC_VIDEO) record_stop_ex(1);
      else if (!recorder_active()) record_start(REC_VIDEO);
      break;

    case IDM_FILE_REC_GIF:
      if (recorder_active() == REC_GIF) record_stop_ex(1);
      else if (!recorder_active()) record_start(REC_GIF);
      break;

    case IDM_FILE_REC_AUDIO:
      if (recorder_active() == REC_AUDIO) record_stop_ex(1);
      else if (!recorder_active()) record_start(REC_AUDIO);
      break;

    case IDM_FILE_REC_STOP:
      if (vgm_active())                              /* the VGM log is a recording too */
      {
        vgm_stop();
        gui_log("Recording file: %s", vgm_name);
        gui_notify("Recording saved");
      }
      record_stop_ex(1);
      gui_update_menu();
      break;

    case IDM_TOOLS_NETPLAY:
      if (!emu_running) gui_notify("Load a game first");
      else if (netplay_state() != NP_IDLE) gui_notify("Netplay is already running");
      else dlg_netplay(g_hwnd);
      break;

    case IDM_TOOLS_NETPLAY_STOP:
      netplay_disconnect("Netplay stopped");
      break;

    case IDM_FILE_SCREENSHOT:
    {
      char path[GUI_PATH_LEN];
      char name[GUI_PATH_LEN];

      if (video_screenshot(path, sizeof(path)))
      {
        char *slash = strrchr(path, '\\');
        lstrcpynA(name, slash ? slash + 1 : path, sizeof(name));
        gui_notify("Saved %s", name);
      }
      else
      {
        gui_notify("Could not save the screenshot");
      }
      frame_clock_restart();
      break;
    }

    case IDM_INPUT_TURBO_ALL:
    {
      /* Turbo switched off and on again as a whole: which buttons have Turbo (Controls) is not touched. */
      int on = gui_input_toggle_turbo();
      if (on && !(gui.pad[0].turbo & 0x3F0u)) gui_notify("Turbo on (no button has Turbo set)");
      else gui_notify(on ? "Turbo on" : "Turbo off");
      break;
    }

    case IDM_EMU_FASTFWD:
      gui_input_toggle_ff();
      next_frame_time = 0.0;
      gui_update_menu();   /* the "Fast forward" notice comes from the frame loop, whichever way it was switched on ... */
      if (emu_paused)      /* ... which does not run while paused */
      {
        fast_osd_state = gui_input_fast_forward() ? 1 : 0;
        gui_notify(fast_osd_state ? "Fast forward" : "Normal speed");
      }
      break;

    case IDM_EMU_REWINDNOW:
      gui_input_toggle_rewind();
      next_rewind_time = 0.0;
      gui_notify(gui_input_rewind_latched() ? "Rewinding" : "Rewind off");
      gui_update_menu();
      break;

    case IDM_EMU_SPEEDUP:
    case IDM_EMU_SPEEDDOWN:
    case IDM_EMU_SPEEDRESET:
    {
      static const int steps[] = { 25, 50, 75, 100, 125, 150, 200, 300, 400 };
      int k, cur = 3, n = (int)(sizeof(steps) / sizeof(steps[0]));
      char t[16];

      if (netplay_active()) break;
      for (k = 0; k < n; k++) if (steps[k] <= speed_pct) cur = k;
      if (id == IDM_EMU_SPEEDUP && cur + 1 < n) cur++;
      else if (id == IDM_EMU_SPEEDDOWN && cur > 0) cur--;
      else if (id == IDM_EMU_SPEEDRESET) cur = 3;
      speed_pct = steps[cur];
      waveout_flush();
      next_frame_time = 0.0;
      snprintf(t, sizeof(t), "%d%%", speed_pct);
      gui_notify(speed_pct == 100 ? "Normal speed" : "Speed %s", t);
      gui_update_menu();
      break;
    }

    case IDM_INPUT_HOTKEYS:
      dlg_hotkeys(g_hwnd);
      break;

    case IDM_VIDEO_INTSCALE:
      gui.integer_scale = !gui.integer_scale;
      video_invalidate();
      InvalidateRect(g_hwnd, NULL, TRUE);
      gui_update_menu();
      config_save();
      break;

    case IDM_AUDIO_MUTE_FM:
      gui.mute_fm = !gui.mute_fm;
      gui_notify(gui.mute_fm ? "FM sound muted" : "FM sound unmuted");
      gui_update_menu();
      config_save();
      break;

    case IDM_AUDIO_MUTE_PWM:
      gui.mute_pwm = !gui.mute_pwm;
      gui_notify(gui.mute_pwm ? "PWM sound muted" : "PWM sound unmuted");
      gui_update_menu();
      config_save();
      break;

    case IDM_AUDIO_MUTE_PSG:
      gui.mute_psg = !gui.mute_psg;
      gui_notify(gui.mute_psg ? "PSG sound muted" : "PSG sound unmuted");
      gui_update_menu();
      config_save();
      break;

    case IDM_EMU_AUTOSAVE:
      gui.autosave = !gui.autosave;
      gui_update_menu();
      config_save();
      break;

    case IDM_OPTIONS_OSD:
      gui.osd = !gui.osd;
      gui_update_menu();
      config_save();
      break;

    case IDM_FILE_OPENDIR:
      /* The folder holding Genesis Plus EX.exe, genesis-plus-ex.ini, states, saves, cheats, ... */
      ShellExecuteA(g_hwnd, "open", osd_path(""), NULL, NULL, SW_SHOWNORMAL);
      break;

    case IDM_FILE_EXIT:
      PostMessage(g_hwnd, WM_CLOSE, 0, 0);
      break;

    case IDM_EMU_PAUSE:
      if (!emu_running) break;
      emu_paused = !emu_paused;
      auto_paused_by_focus = 0;
      paused_by_pad = 0;
      if (emu_paused) waveout_flush();
      next_frame_time = 0.0;
      gui_update_menu();
      status_set_fps(0);
      gui_notify(emu_paused ? "Paused" : "Resumed");
      break;

    case IDM_EMU_RESET:
      emu_reset(0);
      break;

    case IDM_EMU_HARDRESET:
      emu_reset(1);
      break;

    case IDM_EMU_CHEATS:
      if (!emu_running) { browser_edit_cheats(); break; }
      dlg_cheats(g_hwnd);
      break;

    case IDM_EMU_CHEATSEARCH:
      if (!emu_running) break;
      dlg_cheat_search(g_hwnd);
      break;

    case IDM_EMU_BIOS:
      /* 0 = cartridge only, 3 = BIOS enabled and booted first. */
      config.bios = (uint8)(config.bios ? 0 : 3);
      gui_update_menu();
      config_save();
      gui_status("BIOS setting applies the next time a ROM is loaded");
      break;

    case IDM_EMU_ADDRERROR:
      config.addr_error = (uint8)(!config.addr_error);
      gui_update_menu();
      config_save();
      break;

    case IDM_EMU_CDLATENCY:
      config.cd_latency = (uint8)(!config.cd_latency);
      gui_update_menu();
      config_save();
      break;

    case IDM_INPUT_GUNCURSOR:
      config.gun_cursor[0] = config.gun_cursor[1] = (uint8)(!config.gun_cursor[0]);
      gui_update_menu();
      config_save();
      break;

    case IDM_INPUT_INVERTMOUSE:
      config.invert_mouse = (uint8)(!config.invert_mouse);
      gui_update_menu();
      config_save();
      break;

    case IDM_FILE_LOADBACKUP_BASE + 0:
    case IDM_FILE_LOADBACKUP_BASE + 1:
    case IDM_FILE_LOADBACKUP_BASE + 2:
      backup_state_load(id - IDM_FILE_LOADBACKUP_BASE);
      gui_update_menu();
      break;

    case IDM_EMU_UNDOLOAD:
      emu_undo_load_state();
      gui_update_menu();
      break;

    case IDM_EMU_UNDOSAVE:
      emu_undo_save_state();
      gui_update_menu();
      break;

    case IDM_EMU_REWINDMEM_BASE + 0:
    case IDM_EMU_REWINDMEM_BASE + 1:
    case IDM_EMU_REWINDMEM_BASE + 2:
    case IDM_EMU_REWINDMEM_BASE + 3:
      gui.rewind_memory = id - IDM_EMU_REWINDMEM_BASE;
      if (!rewind_apply_memory() && gui.rewind)
      {
        gui.rewind = 0;
        if (gui_input_rewind_latched()) gui_input_toggle_rewind();
        gui_status("Not enough memory for that rewind history: rewind is off");
      }
      gui_update_menu();
      config_save();
      break;

    case IDM_EMU_REWIND:
      gui.rewind = !gui.rewind;
      rewind_enable(gui.rewind);
      if (gui.rewind && !rewind_ready())
      {
        gui.rewind = 0;
        gui_status("Not enough memory for the rewind history");
      }
      if (!gui.rewind && gui_input_rewind_latched()) gui_input_toggle_rewind();   /* nothing to rewind with */
      gui_update_menu();
      config_save();
      break;

    case IDM_EMU_PAUSE_UNFOCUSED:
      gui.pause_on_focus_loss = !gui.pause_on_focus_loss;
      gui_update_menu();
      config_save();
      break;

    case IDM_VIDEO_FULLSCREEN:
      if (!emu_running) break;
      video_set_fullscreen(!gui.fullscreen);
      gui_update_menu();
      config_save();
      break;

    case IDM_VIDEO_FULLSCREEN_START:
      gui.fullscreen_on_load = !gui.fullscreen_on_load;
      gui_update_menu();
      config_save();
      break;

    case IDM_VIDEO_ALWAYS_ON_TOP:
      gui.always_on_top = !gui.always_on_top;
      apply_always_on_top();
      gui_update_menu();
      config_save();
      break;

    case IDM_ACCEL_LEAVE_FULLSCREEN:
      /* Esc: same as Alt+Enter. */
      if (!emu_running) break;
      video_set_fullscreen(!gui.fullscreen);
      gui_update_menu();
      config_save();
      break;

    case IDM_VIDEO_SMOOTH:
      gui.smooth = !gui.smooth;
      video_viewport_changed();
      gui_update_menu();
      config_save();
      break;

    case IDM_INPUT_BACKGROUND:
      gui.background_input = !gui.background_input;
      gui_update_menu();
      config_save();
      break;

    case IDM_VIDEO_BRIGHTEN:
      gui.brighten = !gui.brighten;
      video_force_redraw();   /* repaint the current frame (also while paused) */
      gui_update_menu();
      config_save();
      break;

    case IDM_VIDEO_RENDERER_BASE + 0:
    case IDM_VIDEO_RENDERER_BASE + 1:
    case IDM_VIDEO_RENDERER_BASE + 2:
      gui.renderer = id - IDM_VIDEO_RENDERER_BASE;
      video_set_renderer(gui.renderer);
      video_viewport_changed();
      gui_update_menu();
      config_save();
      break;

    case IDM_VIDEO_VSYNC:
      gui.vsync = !gui.vsync;
      gui_update_menu();
      config_save();
      break;

    case IDM_VIDEO_FILTER_NONE:
      video_set_filter(-1);
      rebuild_filter_menu();
      config_save();
      break;

    case IDM_VIDEO_SHADER_NONE:
      gui_set_shader(NULL);
      gui_notify("Shader: None");
      break;

    case IDM_VIDEO_SHADER_PRESET:
    {
      char path[GUI_PATH_LEN] = "";

      /* shaders/ is created at startup (see WinMain) like every other data
         folder; passed here only as the picker's starting point, and only
         used the very first time -- after that Windows remembers whatever
         folder was last used for this specific picker (GUI_PICK_SHADER's
         own remembered-state slot) on its own. */
      if (!gui_shader_available()) break;

      if (gui_pick_file(g_hwnd, GUI_PICK_SHADER, "Open Shader Preset",
            "Slang Shader Presets\0*.slangp\0"
            "All Files\0*.*\0\0",
            osd_path("Shaders"), path, sizeof(path)))
      {
        char name[GUI_PATH_LEN], *dot;
        const char *slash;
        gui_set_shader(path);
        slash = strrchr(path, '\\');
        lstrcpynA(name, slash ? slash + 1 : path, sizeof(name));
        dot = strrchr(name, '.');
        if (dot) *dot = '\0';
        gui_notify("Shader: %s", name);
      }
      break;
    }

    case IDM_VIDEO_SHOWFPS:
      gui.show_fps = !gui.show_fps;
      gui_update_menu();
      config_save();
      break;

    case IDM_VIDEO_SMSBORDER:
      gui.sms_show_border = !gui.sms_show_border;
      emu_apply_sms_border();
      video_viewport_changed();
      gui_update_menu();
      config_save();
      break;

    case IDM_VIDEO_GGEXTRA:
      config.gg_extra = (uint8)(!config.gg_extra);
      if (emu_running) bitmap.viewport.changed = 3;
      video_viewport_changed();
      gui_update_menu();
      config_save();
      break;

    case IDM_AUDIO_ENABLE:
      gui.sound_enabled = !gui.sound_enabled;
      gui_notify(gui.sound_enabled ? "Sound unmuted" : "Sound muted");
      emu_apply_audio_settings();
      gui_update_menu();
      config_save();
      break;

    case IDM_AUDIO_SETTINGS:
      dlg_audio(g_hwnd);
      break;

    case IDM_VIDEO_PICTUREADJ:
      dlg_picture_adjust(g_hwnd);
      break;

    case IDM_AUDIO_MONO:
      config.mono = (uint8)(!config.mono);
      gui_update_menu();
      config_save();
      break;

    case IDM_AUDIO_HQFM:
      config.hq_fm = (uint8)(!config.hq_fm);
      if (pico_active()) pico_settings_changed();
      gui_update_menu();
      config_save();
      break;

    case IDM_AUDIO_LOWPASS:
      config.filter = (uint8)(config.filter == 1 ? 0 : 1);       /* from the 3-Band EQ it switches to the low-pass filter */
      if (pico_active()) pico_settings_changed();                /* a 32X game: the library has its own low-pass filter */
      else if (emu_running) audio_set_equalizer();
      gui_update_menu();
      config_save();
      break;

    case IDM_AUDIO_EQ:
      config.filter = (uint8)(config.filter == 2 ? 0 : 2);
      if (emu_running) audio_set_equalizer();
      if (pico_active()) pico_settings_changed();                /* the library's low-pass filter goes off with it */
      gui_update_menu();
      config_save();
      break;

    case IDM_AUDIO_HQPSG:
      config.hq_psg = (uint8)(!config.hq_psg);
      if (emu_running && !pico_active())
      {
        /* Direct psg_init() call, not sound_init() -- sound_init() always
           reinitializes the FM chip too as an inseparable part of the same
           call (confirmed by reading it: FM and PSG setup are one
           function, no way to ask for just one), which has nothing to do
           with this setting and was needlessly wiping FM instrument state
           on every toggle. psg_init() is independently exposed by the
           core for exactly this. Matches sound_init()'s own ternary for
           which PSG variant a given system uses. */
        psg_init((system_hw == SYSTEM_SG) ? PSG_DISCRETE : PSG_INTEGRATED);
      }
      gui_update_menu();
      config_save();
      break;

    case IDM_INPUT_P1:
      dlg_input(g_hwnd, 0);
      break;

    case IDM_HELP_SHORTCUTS:
      dlg_shortcuts(g_hwnd);
      break;

    case IDM_HELP_MENUGUIDE:
      dlg_menu_guide(g_hwnd);
      break;

    case IDM_HELP_ABOUT:
      dlg_about(g_hwnd);
      break;

    default:
      break;
  }
}

/****************************************************************************
 * Window procedure
 ****************************************************************************/

static BOOL CALLBACK repaint_menu_popup_cb(HWND w, LPARAM lp)
{
  char cls[16];
  (void)lp;
  if (IsWindowVisible(w) && GetClassNameA(w, cls, sizeof(cls)) && !lstrcmpA(cls, "#32768"))
  {
    theme_menu_frame(w);
    RedrawWindow(w, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
  }
  return TRUE;
}

/* View menu: its on/off choices (and the list / grid and spacing ones) stay open after a click, so several can be changed in
   a row; the menu closes when the mouse is clicked outside it or Esc is pressed. Windows closes a menu at every click, so while
   a menu is open a message hook (MSGF_MENU) catches the click on such an item, runs the command itself and keeps the click
   from reaching the menu. WM_MENUSELECT tells which item is highlighted. */
static int menu_sel_id = -1;
static HHOOK menu_hook;

static int menu_stays_open(int id)
{
  return id == IDM_VIEW_LIST || id == IDM_VIEW_GRID ||
         id == IDM_VIEW_TOOLBAR || id == IDM_OPT_TBLABELS || id == IDM_VIEW_FSTOOLBAR || id == IDM_VIEW_SEARCHBAR || id == IDM_VIEW_FILTERBAR || id == IDM_VIEW_STATUSBAR ||
         (id >= IDM_VIEW_DENSITY_BASE && id < IDM_VIEW_DENSITY_BASE + 3) ||
         id == IDM_VIEW_COL_FAV || (id >= IDM_VIEW_COL_BASE && id < IDM_VIEW_COL_BASE + 2) || (id >= IDM_VIEW_COL_EXTRA && id < IDM_VIEW_COL_EXTRA + 4) ||
         id == IDM_VIEW_FAVONLY || id == IDM_VIEW_SORT_RECENT || id == IDM_VIEW_CLEANNAMES || id == IDM_VIEW_GRIDLINES ||
         id == IDM_VIEW_GROUPCONSOLE || id == IDM_VIEW_AZSTRIP || (id >= IDM_SET_COVERSIZE_BASE && id < IDM_SET_COVERSIZE_BASE + 7) ||
         (id >= IDM_SET_TBSIZE_BASE && id < IDM_SET_TBSIZE_BASE + 3) ||
         (id >= IDM_SET_STYLE_BASE && id < IDM_SET_STYLE_BASE + 2) || (id >= IDM_VIDEO_THEME_BASE && id < IDM_VIDEO_THEME_BASE + 3);
}

/* Switches the check (or the radio dot) of a View menu choice in the menu that is open, before its command has run. */
static void menu_flip_mark(int id)
{
  static const struct { int base, n; } radios[] =
  {
    { IDM_VIEW_LIST, 2 }, { IDM_SET_COVERSIZE_BASE, 7 }, { IDM_VIEW_DENSITY_BASE, 3 }, { IDM_SET_TBSIZE_BASE, 3 },
    { IDM_SET_STYLE_BASE, 2 }, { IDM_VIDEO_THEME_BASE, 3 }
  };
  int i;

  for (i = 0; i < (int)(sizeof(radios) / sizeof(radios[0])); i++)
    if (id >= radios[i].base && id < radios[i].base + radios[i].n)
    {
      CheckMenuRadioItem(g_menu, radios[i].base, radios[i].base + radios[i].n - 1, id, MF_BYCOMMAND);
      return;
    }
  CheckMenuItem(g_menu, id, MF_BYCOMMAND | ((GetMenuState(g_menu, id, MF_BYCOMMAND) & MF_CHECKED) ? MF_UNCHECKED : MF_CHECKED));
}

static LRESULT CALLBACK menu_stay_hook(int code, WPARAM wp, LPARAM lp)
{
  if (code == MSGF_MENU && menu_sel_id >= 0 && menu_stays_open(menu_sel_id))
  {
    const MSG *m = (const MSG *)lp;

    if (m->message == WM_LBUTTONUP || (m->message == WM_KEYDOWN && (m->wParam == VK_RETURN || m->wParam == VK_SPACE)))
    {
      /* The command can take a while (the game list is rebuilt, the window laid out again). The mark in the open menu is
         switched first and drawn at once, so the click is answered straight away; gui_update_menu() then puts the real state. */
      menu_flip_mark(menu_sel_id);
      EnumThreadWindows(GetCurrentThreadId(), repaint_menu_popup_cb, 0);
      SendMessage(g_hwnd, WM_COMMAND, MAKEWPARAM(menu_sel_id, 0), 0);
      gui_update_menu();
      EnumThreadWindows(GetCurrentThreadId(), repaint_menu_popup_cb, 0);        /* the check marks in the open menu */
      SetTimer(g_hwnd, 9002, 30, NULL);                                          /* and again, once a layout the command started has settled */
      SetTimer(g_hwnd, 9003, 160, NULL);
      return 1;                                                                  /* the menu never sees the click */
    }
  }
  return CallNextHookEx(menu_hook, code, wp, lp);
}

/* A file dropped on the window: a game is opened; a cheat file or a state file goes to the game that runs. */
static void handle_dropped_file(const char *path)
{
  const char *dot = strrchr(path, '.');

  if (dot && (!lstrcmpiA(dot, ".cht") || !lstrcmpiA(dot, ".gpstate") ||
              (!_strnicmp(dot, ".gp", 3) && dot[3] >= '0' && dot[3] <= '9' && !dot[4])))
  {
    if (!emu_running)
    {
      gui_status("Open a game first, then drop the file on it");
      return;
    }
    if (!lstrcmpiA(dot, ".cht"))
    {
      int n = cheats_import_file(path);
      if (n > 0) { cheats_save_current(); gui_notify("%d cheat%s added", n, n == 1 ? "" : "s"); }
      else gui_notify(n < 0 ? "Could not read that cheat file" : "No cheats found in that file");
    }
    else emu_load_state_file(path);
    return;
  }

  emu_load_rom(path);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
    case WM_COMMAND:
    {
      WORD id = LOWORD(wp);
      WORD notify = HIWORD(wp);

      if (browser_panel_handle_command(id, notify)) return 0;

      if (notify == 0 || notify == 1) on_command(id);
      return 0;
    }

    case WM_SIZING:
      /* Dragging an edge: keep the picture's own shape, so there are no bars and no stretching. */
      if (gui.keep_aspect && emu_running && !gui.fullscreen && !IsZoomed(hwnd))
      {
        double ar = video_display_aspect();
        if (ar > 0.1)
        {
          RECT *r = (RECT *)lp, wr, cr;
          int extra_w, extra_h, aw, ah, sbh = 0;

          GetWindowRect(hwnd, &wr);
          GetClientRect(hwnd, &cr);
          if (g_status && IsWindowVisible(g_status)) { RECT sb; GetWindowRect(g_status, &sb); sbh = sb.bottom - sb.top; }
          extra_w = (wr.right - wr.left) - (cr.right - cr.left);
          extra_h = (wr.bottom - wr.top) - (cr.bottom - cr.top) + browser_toolbar_height() + sbh;

          aw = (r->right - r->left) - extra_w;
          ah = (r->bottom - r->top) - extra_h;
          if (aw < 64) aw = 64;
          if (ah < 64) ah = 64;

          if (wp == WMSZ_TOP || wp == WMSZ_BOTTOM) aw = (int)(ah * ar + 0.5);
          else ah = (int)(aw / ar + 0.5);

          if (wp == WMSZ_LEFT || wp == WMSZ_TOPLEFT || wp == WMSZ_BOTTOMLEFT) r->left = r->right - (aw + extra_w);
          else r->right = r->left + (aw + extra_w);
          if (wp == WMSZ_TOP || wp == WMSZ_TOPLEFT || wp == WMSZ_TOPRIGHT) r->top = r->bottom - (ah + extra_h);
          else r->bottom = r->top + (ah + extra_h);
          return TRUE;
        }
      }
      break;

    case WM_SETCURSOR:
      /* Over the picture with a light gun plugged in: crosshair or no pointer. */
      if (LOWORD(lp) == HTCLIENT)
      {
        int gc = gui_input_gun_cursor();
        if (gc) { SetCursor(gc == 1 ? LoadCursor(NULL, IDC_CROSS) : NULL); return TRUE; }
      }
      break;

    case WM_DEVICECHANGE:
      /* DBT_DEVNODES_CHANGED: something was plugged in or out. Wait a moment: a pad announces itself in several steps. */
      if (wp == 0x0007) SetTimer(hwnd, 9001, 1000, NULL);
      return TRUE;

    case WM_TIMER:
      if (wp == 9100)
      {
        /* A menu is open: the game keeps running behind it, unless "Pause When Inactive" is on (then it waits while a drop-down is
           open; the bar alone, opened with Alt or F10, does not stop it). */
        if (menu_loop_active && emu_running && !emu_paused &&
            (!gui.pause_on_focus_loss || (menu_keyboard_mode && menu_popups_open == 0)))
        {
          int rep;
          in_modal_loop = 0;
          for (rep = 0; rep < 2; rep++) gui_run_step();      /* (a timer tick comes about every 16 ms: two steps keep a 60 Hz game at speed) */
          in_modal_loop = 1;
        }
        return 0;
      }
      if (wp == 9004)
      {
        KillTimer(hwnd, 9004);
        if (menu_rebuild_pending) { menu_rebuild_pending = 0; apply_menu_font(); }
        return 0;
      }
      if (wp == 9003)
      {
        KillTimer(hwnd, 9003);
        gui_update_menu();
        EnumThreadWindows(GetCurrentThreadId(), repaint_menu_popup_cb, 0);
        return 0;
      }
      if (wp == 9002)
      {
        KillTimer(hwnd, 9002);
        EnumThreadWindows(GetCurrentThreadId(), repaint_menu_popup_cb, 0);
        return 0;
      }
      if (wp == 9101)
      {
        KillTimer(hwnd, 9101);
        if (gui.fullscreen && GetMenu(hwnd)) theme_paint_menubar_selected(hwnd, fs_bar_sel);
        return 0;
      }
      if (wp == 9001)
      {
        KillTimer(hwnd, 9001);
        if (!in_modal_loop) gui_input_devices_changed();   /* a window with its own device list (Controls) has its Refresh button */
        return 0;
      }
      break;

    case WM_NCPAINT:
    case WM_NCACTIVATE:
    {
      /* Fullscreen has no title bar and no frame: the only thing in the non-client area is the menu bar, and the system
         paints it light. Painting it dark afterwards left the light one on the screen for a moment (the white flash), so
         in fullscreen the system is not asked to paint it at all (WM_NCACTIVATE with lParam -1 means "do not repaint"). */
      LRESULT r;
      if (gui.fullscreen && GetMenu(hwnd) && theme_is_dark())
      {
        if (msg == WM_NCACTIVATE) r = DefWindowProc(hwnd, msg, wp, (LPARAM)-1);
        else r = 0;
        theme_paint_menubar(hwnd, 1);
        return r;
      }
      r = DefWindowProc(hwnd, msg, wp, lp);
      if (gui.fullscreen) theme_paint_menubar(hwnd, 1);
      else theme_paint_menu_line(hwnd, NULL);
      return r;
    }

    case WM_NCMOUSEMOVE:
    case WM_NCMOUSELEAVE:
    case 0x02A0:   /* WM_NCMOUSEHOVER */
    case WM_NCLBUTTONUP:
      if (gui.fullscreen && GetMenu(hwnd))
      {
        /* The system lights the item under the mouse white when the bar has no title bar above it (fullscreen). Letting it
           do so and painting the bar dark again afterwards showed as a flash, so the pointer moving over the bar (or
           leaving it) is not passed on at all: the bar is painted here, dark, with the title under the pointer lit. The
           bar is also painted at every tick of the hover timer, which covers a pointer that left without a message. */
        if (msg == WM_NCMOUSELEAVE || ((msg == WM_NCMOUSEMOVE || msg == 0x02A0) && wp == HTMENU))
        {
          theme_paint_menubar(hwnd, 0);
          return 0;
        }
        {
          LRESULT r = DefWindowProc(hwnd, msg, wp, lp);
          theme_paint_menubar(hwnd, 1);
          return r;
        }
      }
      break;

    case 0x0091:   /* WM_UAHDRAWMENU -- undocumented, no public name */
      if (theme_draw_menu_bar(hwnd, lp)) return 0;
      break;

    case 0x0092:   /* WM_UAHDRAWMENUITEM -- undocumented, no public name */
      if (theme_draw_menu_item(lp)) return 0;
      break;

    case WM_UNINITMENUPOPUP:
      if (menu_popups_open > 0) menu_popups_open--;
      break;

    case WM_INITMENUPOPUP:
    {
      if (!HIWORD(lp)) menu_popups_open++;
      /* A menu opened right after another program's menu was dismissed (a second copy of this program, say)
         could stay unpainted (white) until the mouse moved over it: repaint the popup a moment after it shows. */
      SetTimer(hwnd, 9002, 30, NULL);
      /* Refresh the slot timestamps each time the state slot menus opens. */
      int base = slot_menu_base((HMENU)wp);
      if (base)
      {
        update_slot_menu_labels((HMENU)wp, base);
        if (base == IDM_FILE_SLOT_BASE) check_radio((HMENU)wp, IDM_FILE_SLOT_BASE, GUI_SLOT_MAX, gui.state_slot);
        return 0;
      }
      break;
    }

    case WM_MEASUREITEM:
      if (theme_combo_measure((MEASUREITEMSTRUCT *)lp)) return TRUE;
      if (theme_measure_menu_ownerdraw(lp)) return TRUE;
      break;

    case WM_DRAWITEM:
      if (tb_draw((const DRAWITEMSTRUCT *)lp)) return TRUE;
      if (theme_combo_draw((const DRAWITEMSTRUCT *)lp)) return TRUE;
      if (theme_draw_menu_ownerdraw(lp)) return TRUE;
      break;

    case WM_NOTIFY:
    {
      LRESULT sb = theme_statusbar_customdraw((NMHDR *)lp, g_status);
      if (sb != -1) return sb;
      sb = theme_header_customdraw((NMHDR *)lp);
      if (sb != -1) return sb;
      sb = browser_list_customdraw((NMHDR *)lp);
      if (sb != -1) return sb;
      if (browser_panel_handle_notify((NMHDR *)lp)) return 0;
      return 0;
    }

    case WM_PAINT:
    {
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(hwnd, &ps);
      video_repaint(hdc);
      EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_ERASEBKGND:
      return 1;   /* video.c paints every pixel it owns */

    case WM_CTLCOLORBTN:       /* the corners around the filter bar's push buttons */
    case WM_CTLCOLORLISTBOX:   /* the drop-down lists of the filter bar */
    {
      HBRUSH br = theme_ctlcolor((HDC)wp);
      if (br) return (LRESULT)br;
      break;
    }

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    {
      HBRUSH br = browser_panel_ctlcolor((HWND)lp, (HDC)wp);
      if (br) return (LRESULT)br;
      break;
    }

    case WM_CONTEXTMENU:
      if (browser_panel_handle_contextmenu((HWND)wp,
            (lp == (LPARAM)-1) ? -1 : (int)(short)LOWORD(lp),
            (lp == (LPARAM)-1) ? -1 : (int)(short)HIWORD(lp))) return 0;
      break;

    case WM_SIZE:
      if (wp == SIZE_MINIMIZED)
      {
        if (emu_running && !emu_paused && !netplay_active())
        {
          emu_paused = 1;
          auto_paused_by_minimize = 1;
          waveout_flush();
          gui_update_menu();
          status_set_fps(0);
        }
        return 0;
      }

      if (auto_paused_by_minimize)
      {
        auto_paused_by_minimize = 0;
        if (emu_running && emu_paused)
        {
          emu_paused = 0;
          gui_update_menu();
        }
      }

      layout_status();
      browser_toolbar_layout();
      video_invalidate();
      capture_window_geometry();
      if (browser_panel_visible())
      {
        RECT content;
        get_content_rect(&content);
        browser_panel_layout(&content);
      }

      /* In dark mode the menu bar is painted by hand, and how many rows it
         wraps into changes with the window's width. A live resize does not
         reliably repaint the whole bar on its own -- confirmed by direct
         testing, a stray white patch from the bar's *previous* size and row
         count is left behind, since Windows only invalidates what its own
         (unaware of the hand-painted content) layout thinks changed. Forcing
         a full non-client repaint here, on every size change, clears it. */
      if (theme_is_dark())
      {
        RedrawWindow(hwnd, NULL, NULL, RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE | RDW_ALLCHILDREN);
      }
      return 0;

    case WM_MOVE:
      capture_window_geometry();
      return 0;

    case 0x02E0:   /* WM_DPICHANGED, not in every mingw headers version */
    {
      /* The manifest claims per-monitor awareness, so Windows expects us to
         resize ourselves when the window moves to a different-DPI display. */
      RECT *suggested = (RECT *)lp;

      dpi_changed(hwnd, LOWORD(wp));
      if (suggested && !gui.fullscreen)
      {
        SetWindowPos(hwnd, NULL,
                     suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
      }
      layout_status();
      video_invalidate();
      return 0;
    }

    case WM_GETMINMAXINFO:
    {
      MINMAXINFO *mmi = (MINMAXINFO *)lp;
      mmi->ptMinTrackSize.x = 256;
      mmi->ptMinTrackSize.y = 224;
      return 0;
    }

    case WM_ACTIVATE:
    {
      int active = (LOWORD(wp) != WA_INACTIVE);

      gui_input_set_focus(active);

      /* Clicking into the Cheat Search window is not leaving the program: the game keeps running. */
      if (!active && (HWND)lp != NULL && (HWND)lp == cheat_search_hwnd()) { next_frame_time = 0.0; return 0; }

      if (!active && menu_keyboard_mode) { next_frame_time = 0.0; return 0; }          /* (the menu bar taking the focus) */

      if (!active && gui.pause_on_focus_loss && emu_running && !emu_paused && !netplay_active())
      {
        emu_paused = 1;
        auto_paused_by_focus = 1;
        waveout_flush();
        gui_update_menu();
        status_set_fps(0);
      }
      else if (active && auto_paused_by_focus)
      {
        auto_paused_by_focus = 0;
        if (emu_running && emu_paused)
        {
          emu_paused = 0;
          gui_update_menu();
        }
      }
      next_frame_time = 0.0;
      return 0;
    }

    case WM_APP_FSBAR:
      if (gui.fullscreen && GetMenu(hwnd)) theme_paint_menubar_selected(hwnd, fs_bar_sel);
      return 0;

    case WM_APP_PICOSCAN:
      browser_pico_done(lp);
      return 0;

    case WM_APP_PACE:
      InterlockedExchange(&pace_pending, 0);
      if (pace_depth > 0) gui_run_step();      /* a window of this program is open: the game goes on behind it */
      return 0;

    case WM_MENUSELECT:
      if (gui.fullscreen && GetMenu(hwnd))
      {
        /* Fullscreen has no title bar, so the system paints the title of the menu that opens (and, as the pointer moves along
           the bar, the next one) white. Which title is open is known here: paint the bar dark again, with that one lit, right
           after the system has painted it (a posted message and a short timer, the system paints a moment after this one). */
        if ((HIWORD(wp) & MF_POPUP) && (HMENU)lp == GetMenu(hwnd)) fs_bar_sel = (int)LOWORD(wp);
        else if (HIWORD(wp) == 0xFFFF && !lp) fs_bar_sel = -1;
        PostMessage(hwnd, WM_APP_FSBAR, 0, 0);
        SetTimer(hwnd, 9101, 25, NULL);
      }
      /* Which command is highlighted in the open menu (-1: none, a submenu title or a separator). */
      if (HIWORD(wp) == 0xFFFF && !lp) menu_sel_id = -1;
      else if (HIWORD(wp) & (MF_POPUP | MF_SEPARATOR | MF_GRAYED | MF_DISABLED)) menu_sel_id = -1;
      else menu_sel_id = LOWORD(wp);
      break;

    case WM_ENTERMENULOOP:
    case WM_ENTERSIZEMOVE:
      in_modal_loop = 1;
      waveout_flush();
      if (msg == WM_ENTERMENULOOP) { menu_popups_open = 0; menu_loop_active = 1; SetTimer(hwnd, 9100, 8, NULL); }
      if (msg == WM_ENTERMENULOOP && !menu_hook)
      {
        menu_sel_id = -1;
        menu_hook = SetWindowsHookExA(WH_MSGFILTER, menu_stay_hook, NULL, GetCurrentThreadId());
      }
      return 0;

    case WM_EXITMENULOOP:
    case WM_EXITSIZEMOVE:
      if (msg == WM_EXITMENULOOP) { fs_bar_sel = -1; if (gui.fullscreen && GetMenu(hwnd)) PostMessage(hwnd, WM_APP_FSBAR, 0, 0); }
      if (msg == WM_EXITMENULOOP && menu_hook) { UnhookWindowsHookEx(menu_hook); menu_hook = NULL; menu_sel_id = -1; }
      if (msg == WM_EXITMENULOOP)
      {
        menu_keyboard_mode = 0;
        menu_popups_open = 0;
        menu_loop_active = 0;
        KillTimer(hwnd, 9100);
        if (menu_rebuild_pending) SetTimer(hwnd, 9004, 1, NULL);        /* (not inside the menu's own shutdown) */
        if (auto_paused_by_focus && GetForegroundWindow() == hwnd)       /* a pause the focus change made while the menu was open ends with it */
        {
          auto_paused_by_focus = 0;
          if (emu_running && emu_paused) { emu_paused = 0; gui_update_menu(); }
        }
      }
      in_modal_loop = 0;
      next_frame_time = 0.0;
      return 0;

    case WM_COPYDATA:
    {
      /* A second launch (Keep Only One Instance) hands over the game it was asked to open. */
      const COPYDATASTRUCT *cd = (const COPYDATASTRUCT *)lp;

      if (cd && cd->dwData == 0x47505831 && cd->lpData && cd->cbData > 1 && cd->cbData < GUI_PATH_LEN &&
          ((const char *)cd->lpData)[cd->cbData - 1] == '\0')
      {
        /* Only another copy of this very program may hand a game over (its window is in wp), and only a file that exists. */
        char path[GUI_PATH_LEN], mine[MAX_PATH], theirs[MAX_PATH];
        DWORD pid = 0, n = sizeof(theirs), attr;
        HANDLE ph;
        int same = 0;

        GetWindowThreadProcessId((HWND)wp, &pid);
        ph = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : NULL;
        if (ph)
        {
          same = GetModuleFileNameA(NULL, mine, sizeof(mine)) && QueryFullProcessImageNameA(ph, 0, theirs, &n) && !lstrcmpiA(mine, theirs);
          CloseHandle(ph);
        }
        lstrcpynA(path, (const char *)cd->lpData, sizeof(path));
        attr = GetFileAttributesA(path);
        if (!same || attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) return FALSE;
        handle_dropped_file(path);
        return TRUE;
      }
      return FALSE;
    }

    case WM_DROPFILES:
    {
      HDROP drop = (HDROP)wp;
      char path[GUI_PATH_LEN];

      if (DragQueryFileA(drop, 0, path, sizeof(path)))
      {
        handle_dropped_file(path);
        SetForegroundWindow(hwnd);
      }
      DragFinish(drop);
      return 0;
    }

    case WM_SYSCOMMAND:
      /* Do not let the screensaver interrupt a game. */
      if ((wp & 0xFFF0) == SC_SCREENSAVE || (wp & 0xFFF0) == SC_MONITORPOWER)
      {
        if (emu_running && !emu_paused) return 0;
      }

      /* F10 and Alt open the menu bar for the keyboard (SC_KEYMENU). That briefly takes the focus from the game window, which
         would trigger "Pause emulation when in background" and leave the game paused; while the bar is open the game waits
         anyway (the menu loop), so the focus change is not counted as leaving the program, and when the bar closes the game
         carries on. */
      if ((wp & 0xFFF0) == SC_KEYMENU && GetMenu(hwnd)) menu_keyboard_mode = 1;

      break;

    case WM_QUERYENDSESSION:
      return TRUE;

    case WM_ENDSESSION:
      /* Windows is shutting down or logging off and will end the process
         without a WM_CLOSE: write out everything worth keeping now. */
      if (wp)
      {
        save_backup_ram();
        cheats_save_current();
        config_save();
        config_save_wait();
      }
      return 0;

    case WM_CLOSE:
      if (emu_running && gui.confirm_exit && !cli_exit_on_close && !restart_closing)
      {
        int keep = 1, what = exit_ask(hwnd, &keep);
        if (what && !keep) skip_autosave_once = 1;
        if (what == 1) { emu_close_rom(); return 0; }       /* Stop Emulation: back to the game list, the program stays */
        if (what == 0) return 0;                            /* Cancel */
      }
      DestroyWindow(hwnd);
      return 0;

    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;

    case WM_SETTINGCHANGE:
      theme_handle_settingchange(hwnd);
      return 0;

    default:
      break;
  }

  return DefWindowProc(hwnd, msg, wp, lp);
}

/****************************************************************************
 * Startup
 ****************************************************************************/

static int create_main_window(void)
{
  int center_window_at_start = 0;
  POINT pt_origin = { 0, 0 };
  WNDCLASSEXA wc;
  RECT rc;
  int w, h;
  int x, y;

  ZeroMemory(&wc, sizeof(wc));
  wc.cbSize        = sizeof(wc);
  wc.style         = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
  wc.lpfnWndProc   = wnd_proc;
  wc.hInstance     = g_inst;
  wc.hIcon         = LoadIconA(g_inst, MAKEINTRESOURCEA(IDI_APPICON));
  wc.hIconSm       = wc.hIcon;
  wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
  wc.lpszClassName = APP_CLASS;

  if (!RegisterClassExA(&wc)) return 0;

  g_menu = LoadMenuA(g_inst, MAKEINTRESOURCEA(IDR_MAINMENU));

  video_preferred_size(gui.scale, &w, &h);
  rc.left = 0; rc.top = 0; rc.right = w; rc.bottom = h;
  AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, TRUE, 0);

  {
    int mw = menu_bar_min_width(g_menu);
    if (mw > rc.right - rc.left) rc.right = rc.left + mw;
  }

  theme_set_menu_bar(g_menu);
  if (theme_menus_owner()) theme_ownerdraw_menu(g_menu);
  theme_menu_icons(g_menu);                 /* the pictures (in the Large interface the owner-drawn items paint them themselves) */
  hotkeys_apply_menu();   /* the labels show the shortcuts as they are set now */

  /*
   * Use the saved position only if it still lands on a real monitor --
   * otherwise a since-unplugged second monitor would put the window
   * somewhere the person can never reach. MonitorFromRect returns NULL for
   * a rect that intersects nothing, which is exactly the check needed.
   */
  x = CW_USEDEFAULT;
  y = CW_USEDEFAULT;
  center_window_at_start = 1;      /* until a saved position turns out to be usable */

  if (gui.window_x > -32000 && gui.window_y > -32000)
  {
    RECT candidate;
    candidate.left   = gui.window_x;
    candidate.top    = gui.window_y;
    candidate.right  = gui.window_x + (rc.right - rc.left);
    candidate.bottom = gui.window_y + (rc.bottom - rc.top);

    if (MonitorFromRect(&candidate, MONITOR_DEFAULTTONULL) != NULL)
    {
      x = gui.window_x;
      y = gui.window_y;
      center_window_at_start = 0;
    }
  }

  g_hwnd = CreateWindowExA(0, APP_CLASS, APP_NAME, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                           x, y,
                           rc.right - rc.left, rc.bottom - rc.top,
                           NULL, g_menu, g_inst, NULL);
  if (!g_hwnd) return 0;

  g_status = CreateWindowExA(WS_EX_COMPOSITED, STATUSCLASSNAMEA, NULL,
                             WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                             0, 0, 0, 0, g_hwnd, NULL, g_inst, NULL);
  SendMessage(g_status, WM_SETFONT, (WPARAM)gui_get_ui_font(), TRUE);
  if (gui.hide_statusbar) ShowWindow(g_status, SW_HIDE);

  browser_panel_create(g_hwnd);

  g_accel = hotkeys_build_accel();

  DragAcceptFiles(g_hwnd, TRUE);
  layout_status();

  /* Skipped when about to restore maximized: WinMain will maximize right
     after this returns, which would just discard this resize. */
  if (!gui.window_maximized)
  {
    /* Now that the status bar exists its real height is known, so the
       client area can be sized to give exactly the requested scale. */
    gui_resize_to_scale(gui.scale);

    /* A big scale (3x by default) can be taller than a small screen: take it down until the
       window fits the work area. */
    {
      MONITORINFO fit;
      RECT wr;
      HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);

      fit.cbSize = sizeof(fit);
      while (gui.scale > 1 && GetMonitorInfo(mon, &fit) && GetWindowRect(g_hwnd, &wr) &&
             (wr.bottom - wr.top > fit.rcWork.bottom - fit.rcWork.top ||
              wr.right - wr.left > fit.rcWork.right - fit.rcWork.left))
      {
        gui.scale--;
        gui_resize_to_scale(gui.scale);
      }
    }
  }

  /* No saved position: open in the middle of the screen, not wherever Windows
     cascades a new window. Done after the final resize so the size is settled. */
  if (center_window_at_start && !gui.window_maximized)
  {
    RECT wr, area;
    MONITORINFO mi;
    HMONITOR mon = MonitorFromPoint(pt_origin, MONITOR_DEFAULTTOPRIMARY);

    mi.cbSize = sizeof(mi);
    if (GetWindowRect(g_hwnd, &wr) && GetMonitorInfo(mon, &mi))
    {
      int ww = wr.right - wr.left, wh = wr.bottom - wr.top;

      area = mi.rcWork;
      SetWindowPos(g_hwnd, NULL,
                   area.left + ((area.right - area.left) - ww) / 2,
                   area.top  + ((area.bottom - area.top) - wh) / 2,
                   0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
  }

  return 1;
}

/* Pulls the first command-line argument out, if there is one. */
/* Command line, for launching from a frontend (LaunchBox, Pegasus, Playnite,
   ES-DE, Steam ROM Manager and the like):

     "Genesis Plus EX.exe" [options] <rom>

   Options (a leading - or --, and unknown ones are ignored):
     -f, --fullscreen      start the game in fullscreen
     -w, --windowed        start the game in a window
     -x, --exit-on-close   quit the program when the game is closed
                           (Stop, Close ROM) -- hands control back to the frontend
     -s N, --state N       load save state slot N once the game has started
     -h, --help            show this list
   The ROM is the first argument that is not an option. */

static void parse_command_line(char *rom, int rom_len)
{
  LPWSTR *argv;
  int argc = 0, i;

  rom[0] = '\0';

  argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) return;

  for (i = 1; i < argc; i++)
  {
    const wchar_t *a = argv[i];

    if (a[0] == L'-' && a[1])
    {
      const wchar_t *o = a + (a[1] == L'-' ? 2 : 1);

      if (!lstrcmpiW(o, L"f") || !lstrcmpiW(o, L"fullscreen"))          cli_fullscreen = 1;
      else if (!lstrcmpiW(o, L"w") || !lstrcmpiW(o, L"windowed"))       cli_fullscreen = 0;
      else if (!lstrcmpiW(o, L"x") || !lstrcmpiW(o, L"exit-on-close") ||
               !lstrcmpiW(o, L"quit-on-close"))                         cli_exit_on_close = 1;
      else if (!lstrcmpiW(o, L"h") || !lstrcmpiW(o, L"help") || !lstrcmpiW(o, L"?")) cli_help = 1;
      else if (!lstrcmpiW(o, L"restarted"))                            cli_restarted = 1;
      else if (!lstrcmpiW(o, L"m") || !lstrcmpiW(o, L"mute"))          cli_mute = 1;
      else if (!lstrcmpiW(o, L"p") || !lstrcmpiW(o, L"paused"))         cli_paused = 1;
      else if (!lstrcmpiW(o, L"scale") && i + 1 < argc)
      {
        cli_scale = _wtoi(argv[++i]);
        if (cli_scale < 1 || cli_scale > 6) cli_scale = -1;
      }
      else if (!lstrcmpiW(o, L"state-file") && i + 1 < argc)
        WideCharToMultiByte(CP_ACP, 0, argv[++i], -1, cli_state_file, (int)sizeof(cli_state_file), NULL, NULL);
      else if ((!lstrcmpiW(o, L"s") || !lstrcmpiW(o, L"state")) && i + 1 < argc)
      {
        cli_state = _wtoi(argv[++i]);
        resume_prompt_blocked = 1;
        if (cli_state < 0 || cli_state >= GUI_SLOT_MAX) cli_state = -1;
      }
      continue;   /* anything else starting with - is not for us */
    }

    if (!rom[0])
      WideCharToMultiByte(CP_ACP, 0, a, -1, rom, rom_len, NULL, NULL);
  }

  LocalFree(argv);
}

/* Ctrl+C / X / V / A / Z, and the digits, with the focus in a text box are copy and typing, not a program shortcut
   (Ctrl+C is Cheats). */
static int is_text_edit_key(const MSG *m)
{
  char cls[16];
  HWND f;

  if (m->message == WM_KEYDOWN && m->wParam >= '0' && m->wParam <= '9' &&
      !(GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000))
  {
    /* Digits (the state slot shortcuts) are typing when a text box has the focus, and, with no game
       running, the list's type-ahead ("1943", "688 Attack Sub"): a state shortcut has nothing to do then. */
    if (!emu_running) return 1;
    f = GetFocus();
    return f && GetClassNameA(f, cls, sizeof(cls)) && !lstrcmpiA(cls, "Edit");
  }
  if (m->message != WM_KEYDOWN || !(GetKeyState(VK_CONTROL) & 0x8000)) return 0;
  if (m->wParam != 'C' && m->wParam != 'X' && m->wParam != 'V' && m->wParam != 'A' && m->wParam != 'Z') return 0;
  f = GetFocus();
  return f && GetClassNameA(f, cls, sizeof(cls)) && !lstrcmpiA(cls, "Edit");
}

/* A crash leaves a line in the log: what went wrong and where. */
/* A crash is always written down, with or without Enable Log: genesis-plus-ex-crash.txt (what happened and where, as module
   + offset) and genesis-plus-ex-crash.dmp (a small minidump that opens in Visual Studio or WinDbg). Only the last crash is
   kept. Nothing else is done here: the program is in an unknown state, so no save is attempted. */
static LONG WINAPI crash_logger(EXCEPTION_POINTERS *ep)
{
  static volatile LONG busy;
  char dir[MAX_PATH], txt[MAX_PATH + 32], dmp[MAX_PATH + 32], mod[MAX_PATH] = "?", line[600];
  ULONG_PTR a = (ULONG_PTR)ep->ExceptionRecord->ExceptionAddress;
  HMODULE hm = NULL;
  DWORD n, wrote;
  HANDLE f;
  char *slash;

  if (InterlockedExchange(&busy, 1)) return EXCEPTION_CONTINUE_SEARCH;       /* a crash while handling a crash */
  gui_restore_combo_animation();                                              /* the Windows setting a window had turned off */
  if (!gui.crash_report)                                                      /* Settings > Options > Write Crash Report */
  {
    gui_log("CRASH: exception %08lX", (unsigned long)ep->ExceptionRecord->ExceptionCode);
    return EXCEPTION_CONTINUE_SEARCH;
  }

  n = GetModuleFileNameA(NULL, dir, sizeof(dir));
  if (n == 0 || n >= sizeof(dir)) lstrcpyA(dir, ".\\");
  else if ((slash = strrchr(dir, '\\')) != NULL) slash[1] = '\0';
  {
    char logs[MAX_PATH + 8];
    wsprintfA(logs, "%sLogs", dir);
    CreateDirectoryA(logs, NULL);                                             /* the "Logs" folder next to the program */
  }
  wsprintfA(txt, "%sLogs\\genesis-plus-ex-crash.txt", dir);
  wsprintfA(dmp, "%sLogs\\genesis-plus-ex-crash.dmp", dir);

  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &hm) && hm)
    GetModuleFileNameA(hm, mod, sizeof(mod));
  {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wsprintfA(line, "%04d-%02d-%02d %02d:%02d:%02d  Genesis Plus EX crashed (%s)\r\nException %08lX at address %08lX%08lX\r\nModule %s + %08lX\r\n"
                    "Game running: %s\r\n",
              t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, sizeof(void *) == 8 ? "64-bit" : "32-bit",
              (unsigned long)ep->ExceptionRecord->ExceptionCode, (unsigned long)((unsigned long long)a >> 32), (unsigned long)(a & 0xFFFFFFFFUL),
              mod, (unsigned long)(a - (ULONG_PTR)hm), emu_running ? "yes" : "no");
  }
  f = CreateFileA(txt, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (f != INVALID_HANDLE_VALUE)
  {
    WriteFile(f, line, (DWORD)lstrlenA(line), &wrote, NULL);
    CloseHandle(f);
  }
  gui_log("CRASH: exception %08lX at address %08lX%08lX", (unsigned long)ep->ExceptionRecord->ExceptionCode,
          (unsigned long)((unsigned long long)a >> 32), (unsigned long)(a & 0xFFFFFFFFUL));

  /* The minidump: dbghelp.dll is looked for only now, so the program does not depend on it. */
  {
    typedef BOOL (WINAPI *dump_fn)(HANDLE, DWORD, HANDLE, int, void *, void *, void *);
    HMODULE dh = LoadLibraryExA("dbghelp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    dump_fn dump = dh ? (dump_fn)(void *)GetProcAddress(dh, "MiniDumpWriteDump") : NULL;

    if (dump)
    {
      /* MINIDUMP_EXCEPTION_INFORMATION is declared with 4-byte packing: on 64-bit the pointer sits right after the
         thread id, not at an 8-byte boundary. With the natural layout dbghelp read a wrong pointer and wrote nothing
         (a dump of 0 bytes). */
      #pragma pack(push, 4)
      struct { DWORD ThreadId; EXCEPTION_POINTERS *ExceptionPointers; BOOL ClientPointers; } mei;
      #pragma pack(pop)
      HANDLE df = CreateFileA(dmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

      mei.ThreadId = GetCurrentThreadId();
      mei.ExceptionPointers = ep;
      mei.ClientPointers = FALSE;
      if (df != INVALID_HANDLE_VALUE)
      {
        BOOL ok = dump(GetCurrentProcess(), GetCurrentProcessId(), df, 0 /* MiniDumpNormal */, &mei, NULL, NULL);
        DWORD err = ok ? 0 : GetLastError();

        CloseHandle(df);
        if (!ok)
        {
          char note[96];
          DeleteFileA(dmp);                                   /* no empty file left behind */
          wsprintfA(note, "Minidump not written (error %08lX)\r\n", (unsigned long)err);
          f = CreateFileA(txt, FILE_APPEND_DATA, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
          if (f != INVALID_HANDLE_VALUE) { WriteFile(f, note, (DWORD)lstrlenA(note), &wrote, NULL); CloseHandle(f); }
        }
      }
    }
    else
    {
      f = CreateFileA(txt, FILE_APPEND_DATA, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
      if (f != INVALID_HANDLE_VALUE) { WriteFile(f, "Minidump not written (dbghelp.dll not found)\r\n", 46, &wrote, NULL); CloseHandle(f); }
    }
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

/* Keep Only One Instance: one named mutex per program folder (a copy in another folder has its own settings and
   saves, so it may run alongside). Returns 1 when another copy is already running and has been handed the work. */
static int hand_over_to_running_copy(void)
{
  char exe[MAX_PATH], rom[GUI_PATH_LEN], name[64];
  unsigned h = 2166136261u;
  const char *s;
  HANDLE mx;
  HWND me;
  int tries;

  if (!gui.single_instance || !GetModuleFileNameA(NULL, exe, sizeof(exe))) return 0;
  parse_command_line(rom, sizeof(rom));
  if (cli_help || cli_exit_on_close) return 0;               /* a frontend wants its own process, and its exit */

  for (s = exe; *s; s++) { h ^= (unsigned char)(*s >= 'A' && *s <= 'Z' ? *s + 32 : *s); h *= 16777619u; }
  wsprintfA(name, "Local\\GenesisPlusEXGUI-%08X", h);
  mx = CreateMutexA(NULL, FALSE, name);                       /* kept for as long as the program runs */
  if (!mx || GetLastError() != ERROR_ALREADY_EXISTS || cli_restarted) return 0;   /* (a restart takes over the mutex from the copy that is closing) */

  /* A window of its own to tell the other copy who is calling (message-only: never shown). */
  me = CreateWindowExA(0, "STATIC", "", WS_POPUP, 0, 0, 0, 0, HWND_MESSAGE, NULL, g_inst, NULL);

  /* The other copy may still be starting up: look for its window for a few seconds. */
  for (tries = 0; tries < 30; tries++)
  {
    HWND w = NULL;

    while ((w = FindWindowExA(NULL, w, APP_CLASS, NULL)) != NULL)
    {
      DWORD pid = 0;
      HANDLE ph;
      char other[MAX_PATH];
      DWORD n = sizeof(other);
      int same = 0;

      GetWindowThreadProcessId(w, &pid);
      ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
      if (ph)
      {
        same = QueryFullProcessImageNameA(ph, 0, other, &n) && !lstrcmpiA(other, exe);
        CloseHandle(ph);
      }
      if (!same || pid == GetCurrentProcessId()) continue;

      if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
      if (rom[0])
      {
        COPYDATASTRUCT cd;
        cd.dwData = 0x47505831;
        cd.cbData = (DWORD)lstrlenA(rom) + 1;
        cd.lpData = rom;
        SendMessageTimeoutA(w, WM_COPYDATA, (WPARAM)me, (LPARAM)&cd, SMTO_ABORTIFHUNG, 5000, NULL);
      }
      AllowSetForegroundWindow(pid);
      SetForegroundWindow(w);
      return 1;
    }
    Sleep(100);
  }
  return 0;          /* no window turned up: carry on as a copy of our own */
}

static int folder_readonly;   /* the program's own folder cannot be written to (settings, saves and covers would be lost) */

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
  INITCOMMONCONTROLSEX icc;
  MSG msg;
  char startup_rom[GUI_PATH_LEN];

  (void)prev;
  (void)cmdline;

  g_inst = inst;
  dpi_init();

  QueryPerformanceFrequency(&perf_freq);
  timing_boost();
  if (perf_freq.QuadPart == 0) perf_freq.QuadPart = 1;

  icc.dwSize = sizeof(icc);
  icc.dwICC  = ICC_BAR_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_LINK_CLASS;
  InitCommonControlsEx(&icc);

  error_init();
  config_load();
  if (hand_over_to_running_copy()) return 0;
  {
    HANDLE t = CreateFileA(osd_path("gpex-write-test.tmp"), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (t == INVALID_HANDLE_VALUE) folder_readonly = 1;
    else CloseHandle(t);
  }
  SetUnhandledExceptionFilter(crash_logger);
  gui_log("Genesis Plus EX started (%s)", sizeof(void *) == 8 ? "64-bit" : "32-bit");
  log_system_info();
  history_load();
  netplay_init(&np_iface);
  emu_apply_nuked_type();
  gui.fullscreen = 0;   /* never restored across runs -- see the note near WinMain's end */

  ensure_dir("Saves");
  ensure_dir("States");
  ensure_dir("Screenshots");
  ensure_dir("BIOS");
  ensure_dir("Cheats");
  ensure_dir("Covers");
  ensure_dir("Recordings");
  ensure_dir("Bezels");
  ensure_dir("Plugins");
#ifdef _WIN64
  ensure_dir("Shaders");
#endif
  {
    /* The console folders of the cheats and the covers (files you put there yourself), made at once (not when a game of that console is first opened). */
    static const char *const per_console[] = { "Cheats", "Covers" };
    size_t k, c;

    for (k = 0; k < sizeof(per_console) / sizeof(per_console[0]); k++)
      for (c = 0; c < sizeof(console_folders) / sizeof(console_folders[0]) - 1; c++)       /* (not "Other": made when a game that is no known console needs it) */
      {
        char dir[GUI_PATH_LEN];
        wsprintfA(dir, "%s\\%s", osd_path(per_console[k]), console_folders[c]);
        CreateDirectoryA(dir, NULL);
      }
  }

  /* Before create_main_window(), not after -- that call loads and
     attaches the menu resource, and the app-wide dark mode preference
     needs to already be active for the menu *bar* itself (not just its
     dropdown popups, which pick it up regardless of timing) to render
     dark. */
  theme_init();

  if (!create_main_window())
  {
    MessageBoxA(NULL, "The main window could not be created.",
                APP_NAME, MB_OK | MB_ICONERROR);
    return 1;
  }

  theme_apply_to_window(g_hwnd);

  if (!video_init())
  {
    MessageBoxA(g_hwnd, "The video buffer could not be created.",
                APP_NAME, MB_OK | MB_ICONERROR);
    return 1;
  }

  /* Restore whichever render filter was active last session. A name that no
     longer matches anything (a filter that was removed or renamed, or a
     leftover .rpi filename) just leaves filtering off and clears the saved
     value. */
  if (gui.render_filter[0])
  {
    char saved[64];
    lstrcpynA(saved, gui.render_filter, sizeof(saved));
    video_set_filter_by_name(saved);
  }

  gui_input_init();
  rewind_init();
  rewind_apply_memory();
  rewind_enable(gui.rewind);
  load_boot_rom();

  if (gui.sound_enabled && !waveout_open(gui.sample_rate))
  {
    gui.sound_enabled = 0;
    gui_status("No audio device available, sound is off");
  }
  waveout_set_volume(gui.volume);

  rebuild_recent_menu();
  rebuild_filter_menu();
  gui_update_menu();
  update_title();

  ShowWindow(g_hwnd, gui.window_maximized ? SW_MAXIMIZE : show);
  UpdateWindow(g_hwnd);
  apply_always_on_top();
  if (folder_readonly)
  {
    char msg[GUI_PATH_LEN + 300];
    wsprintfA(msg,
      "The program cannot write to its own folder:\n%s\n\nYour settings, hotkeys, saves and covers would not be kept. "
      "Move the program to a folder you can write to, such as Documents or the Desktop (not Program Files), and start it from there.",
      osd_path(""));
    MessageBoxA(g_hwnd, msg, "Folder Not Writable", MB_OK | MB_ICONWARNING);
  }

  /* Confirmed by direct testing: any real resize (either direction)
     fixes a stale-paint artifact that's otherwise present from first
     launch -- so trigger the exact same layout/repaint path a genuine
     WM_SIZE runs, once, right here, rather than waiting for the user
     to happen to resize the window themselves. */
  SendMessage(g_hwnd, WM_SIZE, 0, 0);

  /* Confirmed by direct testing (the Large interface + a dark theme, a window narrow
     enough that the owner-drawn menu bar wraps to more than one row): some
     of the bar's own titles -- not popup items, the top-level titles
     themselves -- come up blank on the very first paint and stay that way
     until something (a click, a hover, or an actual resize) makes Windows
     lay the bar out again, at which point the missing titles appear for
     good. WM_SIZE above does not reach this: it is the non-client menu area,
     not the client area, that came up wrong. A same-size SWP_FRAMECHANGED
     asks for a repaint but not a new layout, and does not fix it either --
     what does is a real size change, so the window is grown by a pixel and
     immediately put back, forcing Windows to lay the bar out again at its
     real width rather than repaint whatever it already (incompletely) had. */
  if (gui.large_ui)
  {
    RECT wr;
    GetWindowRect(g_hwnd, &wr);
    SetWindowPos(g_hwnd, NULL, 0, 0, wr.right - wr.left + 1, wr.bottom - wr.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(g_hwnd, NULL, 0, 0, wr.right - wr.left, wr.bottom - wr.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }

  parse_command_line(startup_rom, sizeof(startup_rom));

  if (cli_help)
  {
    MessageBoxA(g_hwnd,
      "Genesis Plus EX [options] <rom>\n\n"
      "  -f, --fullscreen     start the game in fullscreen\n"
      "  -w, --windowed       start the game in a window\n"
      "  -x, --exit-on-close  quit when the game is closed (for frontends)\n"
      "  -s N, --state N      load save state slot N after starting\n"
      "  --state-file FILE    load a state file after starting\n"
      "  -m, --mute           start with the sound muted\n"
      "  -p, --paused         start the game paused\n"
      "  --scale N            window size, 1 to 6 times the picture\n"
      "  -h, --help           show this list",
      APP_NAME, MB_OK | MB_ICONINFORMATION);
  }

  if (startup_rom[0])
  {
    int keep_fs = gui.fullscreen_on_load;

    /* The command line decides fullscreen for this launch only. */
    if (cli_fullscreen >= 0) gui.fullscreen_on_load = cli_fullscreen;
    if (cli_mute) gui.sound_enabled = 0;
    if (cli_scale > 0) gui.scale = cli_scale;
    emu_load_rom(startup_rom);
    gui.fullscreen_on_load = keep_fs;

    if (emu_running && cli_state_file[0]) emu_load_state_file(cli_state_file);
    if (emu_running && cli_paused) { emu_paused = 1; gui_update_menu(); }

    if (emu_running && cli_state >= 0)
    {
      gui.state_slot = cli_state;
      SendMessage(g_hwnd, WM_COMMAND, MAKEWPARAM(IDM_FILE_LOADSTATE, 0), 0);
    }

    /* A frontend started this to play that game: if it did not load, there
       is nothing to stay open for. */
    if (!emu_running && cli_exit_on_close) PostMessage(g_hwnd, WM_CLOSE, 0, 0);
  }

  if (!emu_running)
  {
    RECT content;
    gui_status_persistent("Open a ROM to start, or drop one on this window");
    get_content_rect(&content);
    browser_panel_layout(&content);
    browser_panel_show(1);
  }

  /* Fullscreen deliberately does not carry over between runs -- always
     start windowed regardless of what gui.fullscreen was saved as. The
     field itself still gets tracked and saved during the session (menu
     checkmark state, various runtime guards elsewhere read it), it's only
     the startup restore that's skipped. */
  fps_tick = GetTickCount();
  eggs_startup();

  for (;;)
  {
    int quit = 0;

    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
    {
      if (msg.message == WM_QUIT) { quit = 1; break; }

      /* The Cheat Search window stays open while the game runs. */
      if (cheat_search_message(&msg)) continue;
      if (msg.message == WM_KEYDOWN) eggs_key(msg.wParam, msg.lParam);
      if (msg.message == WM_KEYDOWN && pause_menu_key(msg.wParam)) continue;

      if (browser_panel_visible())
      {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && browser_panel_handle_return())
          continue;

        browser_type_ahead(&msg);   /* a letter goes to the game list, wherever the focus was */

        /* Shortcuts first, ahead of the dialog-style key handling, so they work whichever control
           has the focus (Ctrl+F toggles the search bar, whatever it is set to under Input > Hotkeys). */
        if ((msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) && g_accel && !is_text_edit_key(&msg) &&
            TranslateAccelerator(g_hwnd, g_accel, &msg))
          continue;

        /* IsDialogMessage is documented as usable on any top-level window
           that manages child controls the way a dialog does, not only real
           dialog boxes -- this is what gives Tab/Shift+Tab navigation
           between the search box, list and buttons without hand-rolling
           focus management. Gated on the panel being visible so it can
           never intercept a keystroke meant for the emulator itself, which
           only reads input while this panel is hidden. */
        if (IsDialogMessage(g_hwnd, &msg)) continue;
      }

      int text_edit = is_text_edit_key(&msg);

      if (text_edit || !g_accel || !TranslateAccelerator(g_hwnd, g_accel, &msg))
      {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
      }
    }

    if (quit) break;

    emulation_step();
  }

  pace_shutdown();
  browser_covers_shutdown();
  emu_close_rom();
  netplay_shutdown();
  config_save();
  config_save_wait();

  waveout_close();
  gui_input_shutdown();
  video_wait_screenshots();
  video_shutdown();
  error_shutdown();

  return (int)msg.wParam;
}
