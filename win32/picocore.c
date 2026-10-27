/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  picocore.c -- 32X games, run by PicoDrive (picodrive_libretro.dll).
 *
 *  The library follows the libretro interface: the program hands it the game, calls retro_run() once per frame, and
 *  receives the picture and the sound through callbacks. The picture is copied into the same frame buffer the
 *  Genesis Plus GX core draws into (video_pico_frame), and the sound into the buffer run_one_frame() already plays,
 *  so everything after that (render filters, shaders, the NTSC-free scaling, screenshots, recording, volume,
 *  stereo width) works for 32X games the way it does for the others.
 *
 *  Genesis Plus GX still loads the game first, as a plain Mega Drive game: that gives the rest of the program the
 *  header, the size and the CRC it already knows how to show. It never runs a frame of it.
 ****************************************************************************/

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "shared.h"
#include "gui.h"
#include "picocore.h"
#include "netplay.h"
#include "picodrive/platform/libretro/libretro-common/include/libretro.h"

/* The libretro functions of the library. */
static struct
{
  HMODULE dll;
  void   (*set_environment)(retro_environment_t);
  void   (*set_video_refresh)(retro_video_refresh_t);
  void   (*set_audio_sample)(retro_audio_sample_t);
  void   (*set_audio_sample_batch)(retro_audio_sample_batch_t);
  void   (*set_input_poll)(retro_input_poll_t);
  void   (*set_input_state)(retro_input_state_t);
  void   (*init)(void);
  void   (*deinit)(void);
  void   (*set_controller_port_device)(unsigned, unsigned);
  void   (*reset)(void);
  void   (*run)(void);
  size_t (*serialize_size)(void);
  bool   (*serialize)(void *, size_t);
  bool   (*unserialize)(const void *, size_t);
  bool   (*load_game)(const struct retro_game_info *);
  void   (*unload_game)(void);
  void   (*get_system_av_info)(struct retro_system_av_info *);
  void  *(*get_memory_data)(unsigned);
  size_t (*get_memory_size)(unsigned);
  void   (*set_mute)(int, int);             /* gpex_set_mute (optional) */
  void   (*set_pwm)(int);                   /* gpex_set_pwm: the level of the 32X PWM sound in percent, 0 = muted (optional) */
  void   (*set_levels)(int, int);           /* gpex_set_levels: the FM and PSG levels in percent (optional) */
  void   (*set_sound_log)(void (*)(int, unsigned, unsigned, unsigned));    /* gpex_set_sound_log (optional) */
  void  *(*memory)(int, size_t *);          /* gpex_memory: only in the library built with this program (optional) */
} rt;

static int    loaded;              /* a game is running in the library */
static int    vars_dirty;          /* a setting the library reads (region, low-pass filter) was changed: it is told at the next frame */
static int    av_pending;          /* ... and the frame rate is asked again after that frame (a region of the other kind) */
static int    state_quiet;         /* saving and loading states is not written to the log (rewind) */
static size_t state_len_cache;     /* the length of the state saved last */
static double core_fps = 60.0;
static short *audio_out;           /* where this frame's sound goes */
static int    audio_cap, audio_got;
static char   sys_dir[GUI_PATH_LEN];
static int    sys_dir_ready;
static int    frames_logged;
static int    core_lines;

static int    no_input;            /* the start-up frames run without any button pressed (the same on every PC: netplay) */
static char   last_path[GUI_PATH_LEN];   /* the game that is running, to start it again (netplay) */
static char   keep_path[GUI_PATH_LEN];   /* a copy of it, when it was a temporary file (the unpacked game of a .7z) */
static int    restarting;
static int    started_32x;         /* the library has said "32X startup": the game has switched the 32X on */
static PVOID  veh_handle;

/* If the library itself crashes, say where in the log (the program's own crash report may not get the chance). Only
   a note is made; the exception is passed on. */
static LONG CALLBACK core_exception(EXCEPTION_POINTERS *ep)
{
  DWORD code = ep->ExceptionRecord->ExceptionCode;
  HMODULE hm = NULL;
  char mod[MAX_PATH] = "?";
  ULONG_PTR a = (ULONG_PTR)ep->ExceptionRecord->ExceptionAddress;

  if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
      code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_IN_PAGE_ERROR && code != EXCEPTION_PRIV_INSTRUCTION &&
      code != EXCEPTION_ARRAY_BOUNDS_EXCEEDED && code != EXCEPTION_INT_OVERFLOW)
    return EXCEPTION_CONTINUE_SEARCH;

  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &hm) && hm)
    GetModuleFileNameA(hm, mod, sizeof(mod));

  if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR)
    gui_log("32X core exception %08lX at %s + %08lX (%s of %08lX%08lX)", (unsigned long)code, mod, (unsigned long)(a - (ULONG_PTR)hm),
            ep->ExceptionRecord->ExceptionInformation[0] == 0 ? "read" : ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "write" : "run",
            (unsigned long)((unsigned long long)ep->ExceptionRecord->ExceptionInformation[1] >> 32), (unsigned long)(ep->ExceptionRecord->ExceptionInformation[1] & 0xFFFFFFFFUL));
  else
    gui_log("32X core exception %08lX at %s + %08lX", (unsigned long)code, mod, (unsigned long)(a - (ULONG_PTR)hm));
  return EXCEPTION_CONTINUE_SEARCH;
}

/* The Mega Drive header of a 32X game says "SEGA 32X" where a plain Mega Drive game says "SEGA MEGA DRIVE" or
   "SEGA GENESIS" (Genesis Plus GX has just read it into rominfo). */
int pico_rom_is_32x(void)
{
  return !strncmp(rominfo.consoletype, "SEGA 32X", 8);
}

/* ---- the callbacks of the library ---- */

static void RETRO_CALLCONV core_log(enum retro_log_level level, const char *fmt, ...)
{
  char line[400];
  va_list ap;

  (void)level;
  va_start(ap, fmt);
  _vsnprintf(line, sizeof(line) - 1, fmt, ap);
  va_end(ap);
  line[sizeof(line) - 1] = '\0';
  {
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
  }
  if (strstr(line, "32X startup")) started_32x = 1;

  /* The library can say the same thing every frame (an unknown SH-2 opcode, say): after a few hundred lines it is not
     written any more, or every line would be a disk write. */
  if (line[0] && core_lines < 300)
  {
    gui_log("32X core: %s", line);
    if (++core_lines == 300) gui_log("32X core: (more messages of the library are not written)");
  }
}

static void RETRO_CALLCONV core_video(const void *data, unsigned width, unsigned height, size_t pitch)
{
  if (data && data != RETRO_HW_FRAME_BUFFER_VALID) video_pico_frame(data, (int)width, (int)height, (int)pitch);
}

static void RETRO_CALLCONV core_audio_sample(int16_t left, int16_t right)
{
  if (audio_out && audio_got < audio_cap)
  {
    audio_out[2 * audio_got]     = left;
    audio_out[2 * audio_got + 1] = right;
    audio_got++;
  }
}

static size_t RETRO_CALLCONV core_audio_batch(const int16_t *data, size_t frames)
{
  if (audio_out && data)
  {
    size_t n = frames;
    if ((int)n > audio_cap - audio_got) n = (size_t)(audio_cap - audio_got);
    if ((int)n > 0)
    {
      memcpy(audio_out + 2 * audio_got, data, n * 2 * sizeof(int16_t));
      audio_got += (int)n;
    }
  }
  return frames;
}

static void RETRO_CALLCONV core_input_poll(void)
{
}

/* The controls: whatever the program's own input code decided this frame (keyboard, gamepad, turbo, combos), turned
   into the buttons of a libretro pad. The program's A B C are PicoDrive's Y B A, X Y Z are L X R. */
static unsigned pad_mask(int port)
{
  int slot = port & 1;
  unsigned p;
  unsigned m = 0;

  if (slot == 1) { int p2 = gui_input_p2_slot(); if (p2 >= 0) slot = p2; }       /* Player 2 is in the program's slot 4 (port B) when the game was set up that way */
  p = (unsigned)input.pad[slot];

  if (p & INPUT_UP)    m |= 1u << RETRO_DEVICE_ID_JOYPAD_UP;
  if (p & INPUT_DOWN)  m |= 1u << RETRO_DEVICE_ID_JOYPAD_DOWN;
  if (p & INPUT_LEFT)  m |= 1u << RETRO_DEVICE_ID_JOYPAD_LEFT;
  if (p & INPUT_RIGHT) m |= 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
  if (p & INPUT_A)     m |= 1u << RETRO_DEVICE_ID_JOYPAD_Y;
  if (p & INPUT_B)     m |= 1u << RETRO_DEVICE_ID_JOYPAD_B;
  if (p & INPUT_C)     m |= 1u << RETRO_DEVICE_ID_JOYPAD_A;
  if (p & INPUT_X)     m |= 1u << RETRO_DEVICE_ID_JOYPAD_L;
  if (p & INPUT_Y)     m |= 1u << RETRO_DEVICE_ID_JOYPAD_X;
  if (p & INPUT_Z)     m |= 1u << RETRO_DEVICE_ID_JOYPAD_R;
  if (p & INPUT_START) m |= 1u << RETRO_DEVICE_ID_JOYPAD_START;
  if (p & INPUT_MODE)  m |= 1u << RETRO_DEVICE_ID_JOYPAD_SELECT;
  return m;
}

static int16_t RETRO_CALLCONV core_input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
  if (no_input) return 0;
  if (port > 1 || index != 0 || (device & 0xff) != RETRO_DEVICE_JOYPAD) return 0;
  if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)pad_mask((int)port);
  if (id > 15) return 0;
  return (int16_t)((pad_mask((int)port) >> id) & 1u);
}

static int run_rate;                        /* the sample rate the library makes its sound at now */
static int rate_changed;                    /* it changed (the setting was changed while the game runs): the sound output has to follow */

int pico_sample_rate(void) { return run_rate; }
int pico_take_rate_change(void) { int r = rate_changed; rate_changed = 0; return r; }

static const char *sample_rate_text(void)
{
  static char text[16];
  wsprintfA(text, "%d", gui.sample_rate > 0 ? gui.sample_rate : 44100);
  return text;
}

static bool RETRO_CALLCONV core_environment(unsigned cmd, void *data)
{
  switch (cmd)
  {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
      return data && *(enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;

    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
      if (!data) return false;
      if (!sys_dir_ready)
      {
        lstrcpynA(sys_dir, osd_path("BIOS"), sizeof(sys_dir));
        sys_dir_ready = 1;
      }
      *(const char **)data = sys_dir;
      return true;

    case RETRO_ENVIRONMENT_GET_VARIABLE:
    {
      struct retro_variable *v = (struct retro_variable *)data;
      if (!v || !v->key) return false;
      if (!strcmp(v->key, "picodrive_sound_rate")) { v->value = sample_rate_text(); return true; }   /* the rate the sound output runs at */
      if (!strcmp(v->key, "picodrive_region"))               /* Settings > System > Region */
      {
        static const char *const names[5] = { "Auto", "US", "Europe", "Japan NTSC", "Japan PAL" };
        if (config.vdp_mode == 1) v->value = "US";            /* Force VDP Mode: NTSC (60 Hz) ... */
        else if (config.vdp_mode == 2) v->value = "Europe";   /* ... or PAL (50 Hz) */
        else v->value = names[config.region_detect < 5 ? config.region_detect : 0];
        return true;
      }
      if (!strcmp(v->key, "picodrive_fmchip"))               /* Settings > Audio > FM Chip: the YM3438 kinds (and the Console Model 2 preset) */
      {
        int ym3438 = config.ym3438 ? !gui.nuked_ym2612 : (config.ym2612 != 0);
        v->value = ym3438 ? "ym3438" : "ym2612";
        return true;
      }
      if (!strcmp(v->key, "picodrive_fm_filter"))            /* Settings > Audio > High-Quality FM Synthesis */
      {
        v->value = config.hq_fm ? "on" : "off";
        return true;
      }
      if (!strcmp(v->key, "picodrive_audio_filter"))         /* Settings > Audio > Low-Pass Filter */
      {
        v->value = config.filter == 1 ? "low-pass" : "disabled";
        return true;
      }
      if (!strcmp(v->key, "picodrive_lowpass_range"))
      {
        static char pct[8];
        wsprintfA(pct, "%d", (int)((config.lp_range * 100u + 32768u) / 65536u));
        v->value = pct;
        return true;
      }
      v->value = NULL;
      return false;                          /* everything else: the library's own defaults */
    }

    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
      if (data) *(bool *)data = vars_dirty ? true : false;
      vars_dirty = 0;
      return true;

    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
      if (!data) return false;
      ((struct retro_log_callback *)data)->log = core_log;
      return true;

    case RETRO_ENVIRONMENT_SET_MESSAGE:
    {
      const struct retro_message *m = (const struct retro_message *)data;
      if (m && m->msg && m->msg[0]) gui_log("32X core message: %s", m->msg);
      return true;
    }

    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
    case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY:
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE:
    case RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE:
    case RETRO_ENVIRONMENT_SET_VARIABLES:
      return true;

    default:
      return false;                          /* nothing else is offered */
  }
}

/* ---- loading ---- */

static void module_dir(char *out, int len)
{
  char *slash;

  GetModuleFileNameA(NULL, out, (DWORD)len);
  slash = strrchr(out, '\\');
  if (slash) slash[1] = '\0'; else out[0] = '\0';
}

/* Why resolve() found no usable library: 1 no such file in any of the places, 2 a file that Windows would not load (the wrong kind, damaged,
   blocked), 3 a library that is not a PicoDrive one (the functions it must have are missing). */
static int resolve_why;
static char resolve_file[GUI_PATH_LEN];
static DWORD resolve_error;

static int resolve(void)
{
  char path[GUI_PATH_LEN];

  resolve_why = 1;
  resolve_file[0] = '\0';
  resolve_error = 0;

  /* In the Plugins folder next to the program, then next to the program itself (how it used to be); under the name of its
     own size first (so the 32-bit and the 64-bit library can share a folder), then the plain name. */
  {
    static const char *const names[] = { sizeof(void *) == 8 ? "picodrive_libretro-64.dll" : "picodrive_libretro-32.dll", "picodrive_libretro.dll" };
    static const char *const dirs[] = { "Plugins\\", "" };
    int i, d;

    for (d = 0; d < 2 && !rt.dll; d++)
    for (i = 0; i < 2 && !rt.dll; i++)
    {
      module_dir(path, sizeof(path));
      lstrcatA(path, dirs[d]);
      lstrcatA(path, names[i]);
      if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;
      rt.dll = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
      if (!rt.dll && !resolve_error) { resolve_why = 2; resolve_error = GetLastError(); lstrcpynA(resolve_file, path, sizeof(resolve_file)); }
    }
  }
  if (!rt.dll) return 0;
  lstrcpynA(resolve_file, path, sizeof(resolve_file));
  resolve_why = 3;

#define SYM(field, name) do { *(FARPROC *)&rt.field = GetProcAddress(rt.dll, name); if (!rt.field) return 0; } while (0)
  SYM(set_environment,           "retro_set_environment");
  SYM(set_video_refresh,         "retro_set_video_refresh");
  SYM(set_audio_sample,          "retro_set_audio_sample");
  SYM(set_audio_sample_batch,    "retro_set_audio_sample_batch");
  SYM(set_input_poll,            "retro_set_input_poll");
  SYM(set_input_state,           "retro_set_input_state");
  SYM(init,                      "retro_init");
  SYM(deinit,                    "retro_deinit");
  SYM(set_controller_port_device, "retro_set_controller_port_device");
  SYM(reset,                     "retro_reset");
  SYM(run,                       "retro_run");
  SYM(serialize_size,            "retro_serialize_size");
  SYM(serialize,                 "retro_serialize");
  SYM(unserialize,               "retro_unserialize");
  SYM(load_game,                 "retro_load_game");
  SYM(unload_game,               "retro_unload_game");
  SYM(get_system_av_info,        "retro_get_system_av_info");
  SYM(get_memory_data,           "retro_get_memory_data");
  SYM(get_memory_size,           "retro_get_memory_size");
  *(FARPROC *)&rt.set_mute = GetProcAddress(rt.dll, "gpex_set_mute");
  *(FARPROC *)&rt.set_levels = GetProcAddress(rt.dll, "gpex_set_levels");
  *(FARPROC *)&rt.set_pwm = GetProcAddress(rt.dll, "gpex_set_pwm");
  *(FARPROC *)&rt.set_sound_log = GetProcAddress(rt.dll, "gpex_set_sound_log");
  *(FARPROC *)&rt.memory = GetProcAddress(rt.dll, "gpex_memory");       /* optional: without it there are no cheats */
#undef SYM
  gui_log("32X library: %s (%s)", path, rt.set_sound_log && rt.memory && rt.set_mute && rt.set_levels ? "complete" : "OLD: it lacks parts this program needs; use the one from the newest zip");
  return 1;
}

static void fail(char *err, int len, const char *text)
{
  if (err && len > 0) lstrcpynA(err, text, len);
}

/* What to tell the player when the PicoDrive library cannot be used: what is wrong and what to do about it. */
static void resolve_failure_text(char *err, int len)
{
  const char *want = sizeof(void *) == 8 ? "picodrive_libretro-64.dll" : "picodrive_libretro-32.dll";
  const char *file = strrchr(resolve_file, '\\');
  char text[900], folder[GUI_PATH_LEN];

  module_dir(folder, sizeof(folder));
  lstrcatA(folder, "Plugins");
  file = file ? file + 1 : resolve_file;
  if (resolve_why == 2)
    wsprintfA(text, "The 32X emulator library (%s) could not be loaded: Windows error %lu%s.\n\n"
                    "It may be the wrong kind (this is the %s-bit program: it needs %s), damaged, or blocked by Windows or a security program "
                    "(right-click the file, choose Properties and tick Unblock if it is there). Copy it again from the zip the program came in, "
                    "into the Plugins folder:\n%s",
              file, (unsigned long)resolve_error, resolve_error == 193 ? " (not a library for this kind of program)" : "",
              sizeof(void *) == 8 ? "64" : "32", want, folder);
  else if (resolve_why == 3)
    wsprintfA(text, "The file %s in the Plugins folder is not a library this program can use (it lacks parts of the PicoDrive interface).\n\n"
                    "Copy %s again from the zip the program came in, into the Plugins folder:\n%s", file, want, folder);
  else
    wsprintfA(text, "32X games are run by PicoDrive, and its library was not found.\n\n"
                    "Put %s (it is in the zip the program came in) in the Plugins folder:\n%s\n\nThen open the game again.", want, folder);
  fail(err, len, text);
}

int pico_load(const char *path, char *err, int err_len)
{
  struct retro_game_info info;
  struct retro_system_av_info av;

  pico_unload();
  fail(err, err_len, "");
  run_rate = gui.sample_rate > 0 ? gui.sample_rate : 44100;

  if (!resolve())
  {
    if (rt.dll) { FreeLibrary(rt.dll); rt.dll = NULL; }
    resolve_failure_text(err, err_len);
    return 0;
  }

  if (!veh_handle) veh_handle = AddVectoredExceptionHandler(1, core_exception);

  rt.set_environment(core_environment);
  rt.set_video_refresh(core_video);
  rt.set_audio_sample(core_audio_sample);
  rt.set_audio_sample_batch(core_audio_batch);
  rt.set_input_poll(core_input_poll);
  rt.set_input_state(core_input_state);
  rt.init();

  rt.set_controller_port_device(0, RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 1));    /* six-button pads */
  rt.set_controller_port_device(1, RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 1));

  memset(&info, 0, sizeof(info));
  info.path = path;

  if (!rt.load_game(&info))
  {
    rt.deinit();
    FreeLibrary(rt.dll);
    memset(&rt, 0, sizeof(rt));
    fail(err, err_len, "PicoDrive could not start this game.");
    return 0;
  }

  memset(&av, 0, sizeof(av));
  rt.get_system_av_info(&av);
  core_fps = av.timing.fps > 20.0 ? av.timing.fps : 60.0;
  loaded = 1;
  state_len_cache = 0;
  if (!restarting)
  {
    char tmp[GUI_PATH_LEN];
    DWORD n = GetTempPathA(sizeof(tmp), tmp);

    if (keep_path[0]) { DeleteFileA(keep_path); keep_path[0] = '\0'; }
    lstrcpynA(last_path, path, sizeof(last_path));
    /* A game unpacked into the temporary folder is deleted by the caller once it is loaded: a copy is kept for a restart. */
    if (n > 0 && n < sizeof(tmp) && !_strnicmp(path, tmp, n))
    {
      lstrcpynA(keep_path, tmp, sizeof(keep_path));
      lstrcatA(keep_path, "gpex32x-restart.tmp");
      if (CopyFileA(path, keep_path, FALSE)) lstrcpynA(last_path, keep_path, sizeof(last_path));
      else keep_path[0] = '\0';
    }
  }
  frames_logged = 0;
  core_lines = 0;
  gui_log("32X game loaded in PicoDrive (%d fps, sample rate %d, state size up to %u bytes)", (int)(core_fps + 0.5), run_rate,
          (unsigned)rt.serialize_size());

  /* A few frames are run before anything else (a saved state, the controls) is applied: until the game has switched the
     32X on (at most half a second), and one more. A state of a running 32X game loaded into a machine that has not
     started its 32X yet made the program end; this way the machine is in the state it is in when it runs a game. The
     sound of these frames is thrown away. */
  {
    static short scrap[4096];
    int i;

    started_32x = 0;
    audio_out = scrap;
    audio_cap = 2048;
    no_input = 1;
    for (i = 0; i < 30 && (i < 2 || !started_32x); i++)
    {
      audio_got = 0;
      rt.run();
    }
    audio_got = 0;
    rt.run();
    no_input = 0;
    audio_out = NULL;
    gui_log("32X: %s after %d frames", started_32x ? "the 32X is on" : "the 32X is not on yet", i + 1);
  }
  return 1;
}

void pico_unload(void)
{
  if (!restarting && keep_path[0]) { DeleteFileA(keep_path); keep_path[0] = '\0'; }
  if (loaded)
  {
    gui_log("32X: closing the game");
    rt.unload_game();
    rt.deinit();
    loaded = 0;
    gui_log("32X: closed");
  }
  if (rt.dll)
  {
    FreeLibrary(rt.dll);
    memset(&rt, 0, sizeof(rt));
  }
  if (veh_handle) { RemoveVectoredExceptionHandler(veh_handle); veh_handle = NULL; }
  audio_out = NULL;
}

int pico_active(void)
{
  return loaded;
}

/* The game starts again as if the program had just been started (netplay: both PCs begin from the same state): the library is
   let go and loaded again, so nothing of what was played before is left in it. 0 when that did not work (the game is then gone). */
int pico_restart(void)
{
  char path[GUI_PATH_LEN], err[200];
  int ok;

  if (!loaded || !last_path[0]) return 0;
  lstrcpynA(path, last_path, sizeof(path));
  restarting = 1;
  ok = pico_load(path, err, sizeof(err));
  restarting = 0;
  return ok;
}

/* For a "did not start out in the same state" report: with Enable Log on, the state is written to the log in pieces (one line for
   each chunk of the library's state), so the logs of the two PCs show which piece differs. */
void pico_state_log_pieces(void)
{
  static const char *const names[40] = { "invalid", "M68K", "RAM", "VRAM", "ZRAM", "CRAM", "VSRAM", "emu state", "VIDEO", "Z80", "PSG", "FM",
    "S68K", "PRG_RAM", "WORD_RAM", "PCM_RAM", "BRAM", "GATE ARRAY", "PCM", "CDC", "CDD", "SCD", "GFX", "MCD", "IO", "SMS",
    "MSH2", "MSH2 data", "MSH2 peri", "SSH2", "SSH2 data", "SSH2 peri", "32X regs", "M68K BIOS", "MSH2 BIOS", "SSH2 BIOS", "SDRAM", "DRAM", "32X palette", "32X events" };
  unsigned char *buf;
  size_t n, pos = 8;

  if (!loaded) return;
  n = rt.serialize_size();
  buf = n ? (unsigned char *)calloc(1, n + 64) : NULL;
  if (!buf) return;
  if (rt.serialize(buf, n))
  {
    while (pos + 5 <= n)
    {
      unsigned id = buf[pos];
      unsigned len = (unsigned)buf[pos + 1] | ((unsigned)buf[pos + 2] << 8) | ((unsigned)buf[pos + 3] << 16) | ((unsigned)buf[pos + 4] << 24);
      unsigned h = 2166136261u;
      size_t i;

      if (id == 0 || pos + 5 + len > n) break;
      for (i = 0; i < len; i++) { h ^= buf[pos + 5 + i]; h *= 16777619u; }
      gui_log("Netplay start: %s %08X (%u bytes)", id < 40 ? names[id] : "chunk", h, len);
      pos += 5 + len;
    }
  }
  free(buf);
}

/* A fingerprint of the whole machine (FNV-1a over its state), for the two PCs of a netplay game to compare. */
unsigned long long pico_state_hash(void)
{
  static unsigned char *buf;
  static size_t cap;
  unsigned long long h = 14695981039346656037ULL;
  size_t i, n;

  if (!loaded) return 0;
  n = rt.serialize_size();
  if (n == 0) return 0;
  if (n + 64 > cap)
  {
    free(buf);
    buf = (unsigned char *)malloc(n + 64);
    cap = buf ? n + 64 : 0;
  }
  if (!buf) return 0;
  memset(buf, 0, n);
  if (!rt.serialize(buf, n)) return 0;
  for (i = 0; i < n; i++) { h ^= buf[i]; h *= 1099511628211ULL; }
  return h;
}

int pico_run_frame(short *out, int max_frames)
{
  if (!loaded) return 0;
  if (frames_logged < 3) gui_log("32X: frame %d is being run", ++frames_logged);

  audio_out = out;
  audio_cap = max_frames;
  audio_got = 0;

  osd_input_update();          /* what the keyboard and the pads say this frame (input.pad[]) */
  if (rt.set_mute)                                                  /* Mute FM, Mute PSG (not in netplay: a chip that is not run would put the two PCs out of step) */
  {
    int np = netplay_active();
    rt.set_mute(np ? 0 : gui.mute_fm, np ? 0 : gui.mute_psg);
  }
  if (rt.set_levels) rt.set_levels(config.fm_preamp, (config.psg_preamp * 100 + 75) / 150);        /* Levels and Latency: FM and PSG (the PSG's 150% is the library's own level) */
  if (rt.set_pwm) rt.set_pwm(gui.mute_pwm ? 0 : gui.pwm_level);                                 /* Mute PWM Sound, and the PWM level of Levels and Latency */
  rt.run();

  if (av_pending)              /* a region of the other kind (50 Hz and 60 Hz) changes the frame rate */
  {
    struct retro_system_av_info av;

    av_pending = 0;
    memset(&av, 0, sizeof(av));
    rt.get_system_av_info(&av);
    if (av.timing.sample_rate > 8000.0 && (int)(av.timing.sample_rate + 0.5) != run_rate)     /* Sample Rate was changed */
    {
      run_rate = (int)(av.timing.sample_rate + 0.5);
      rate_changed = 1;
      gui_log("32X: the sample rate is now %d", run_rate);
    }
    if (av.timing.fps > 20.0 && av.timing.fps != core_fps)
    {
      core_fps = av.timing.fps;
      gui_log("32X: the frame rate is now %d (the region was changed)", (int)(core_fps + 0.5));
    }
  }

  audio_out = NULL;

  return audio_got;
}

void pico_reset(void)
{
  if (loaded)
  {
    rt.reset();
  }
}

double pico_fps(void)
{
  return core_fps;
}

/* The region or the low-pass filter was changed in the program: the library reads them again at its next frame. */
void pico_settings_changed(void)
{
  vars_dirty = 1;
  av_pending = 1;
}

/* The library tells every write to the FM chip, the PSG and the 32X PWM to cb (NULL: no more); see vgm.c. 0 when it cannot. */
int pico_sound_log(void (*cb)(int kind, unsigned a, unsigned d, unsigned frac))
{
  if (!loaded || !rt.set_sound_log) return 0;
  rt.set_sound_log(cb);
  return 1;
}

unsigned char *pico_memory(int what, unsigned *size)
{
  size_t n = 0;
  void *p = (loaded && rt.memory) ? rt.memory(what, &n) : NULL;

  if (size) *size = p ? (unsigned)n : 0;
  return (unsigned char *)p;
}

size_t pico_state_max(void)
{
  size_t n = loaded ? rt.serialize_size() : 0;
  return n + 4096;
}

/* Rewind saves and loads a state at every frame: that is not written to the log, and the size is asked of the library once
   (asking runs the whole state through once) and kept while it stays right. */
void pico_state_quiet(int on)
{
  state_quiet = on;
}

int pico_state_save(unsigned char *buf, size_t cap)
{
  size_t n;

  if (!loaded || !buf) return 0;
  n = state_quiet && state_len_cache ? state_len_cache : rt.serialize_size();
  if (n == 0 || n > cap) { gui_log("32X state: size %u does not fit %u", (unsigned)n, (unsigned)cap); return 0; }
  memset(buf, 0, n);           /* a state of a game that has not started the 32X is shorter than the largest one: the rest is zeros, not leftovers */
  if (!rt.serialize(buf, n))
  {
    state_len_cache = 0;       /* the size changed (the 32X was switched on): ask again next time */
    if (!state_quiet) gui_log("32X state: could not be written");
    return 0;
  }
  state_len_cache = n;
  if (!state_quiet) gui_log("32X state written (%u bytes)", (unsigned)n);
  return (int)n;
}

int pico_state_load(const unsigned char *buf, size_t len)
{
  int ok;

  if (!loaded || !buf || len == 0) return 0;
  if (!state_quiet) gui_log("32X state: loading %u bytes", (unsigned)len);
  ok = rt.unserialize(buf, len) ? 1 : 0;
  if (!state_quiet) gui_log("32X state: %s", ok ? "loaded" : "not accepted");
  return ok;
}

unsigned char *pico_save_ram(size_t *len)
{
  size_t n;
  void *p;

  if (len) *len = 0;
  if (!loaded) return NULL;
  n = rt.get_memory_size(RETRO_MEMORY_SAVE_RAM);
  p = rt.get_memory_data(RETRO_MEMORY_SAVE_RAM);
  if (!p || n == 0) return NULL;
  if (len) *len = n;
  return (unsigned char *)p;
}
