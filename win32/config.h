/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  config.h -- emulation settings (t_config, read directly by the core) and
 *              frontend settings (t_gui_config, used only by the GUI).
 *
 *  t_config must keep the same fields the core expects; add frontend options
 *  to t_gui_config instead.
 ****************************************************************************/

#ifndef _CONFIG_H_
#define _CONFIG_H_

#include "types.h"

/****************************************************************************
 * Emulation settings -- shared with the core
 ****************************************************************************/

typedef struct
{
  uint8 padtype;
} t_input_config;

typedef struct
{
  uint8 hq_fm;
  uint8 filter;
  uint8 hq_psg;
  uint8 ym2612;
  uint8 ym2413;
  uint8 ym3438;
  uint8 opll;
  uint8 cd_latency;
  int16 psg_preamp;
  int16 fm_preamp;
  int16 cdda_volume;
  int16 pcm_volume;
  uint32 lp_range;
  int16 low_freq;
  int16 high_freq;
  int16 lg;
  int16 mg;
  int16 hg;
  uint8 mono;
  uint8 system;
  uint8 region_detect;
  uint8 vdp_mode;
  uint8 master_clock;
  uint8 force_dtack;
  uint8 addr_error;
  uint8 bios;
  uint8 lock_on;
  uint8 add_on;
  uint8 hot_swap;
  uint8 invert_mouse;
  uint8 gun_cursor[2];
  uint8 overscan;
  uint8 gg_extra;
  uint8 ntsc;
  uint8 lcd;
  uint8 render;
  uint8 enhanced_vscroll;
  uint8 enhanced_vscroll_limit;
  t_input_config input[MAX_INPUTS];
} t_config;

/****************************************************************************
 * Frontend settings -- GUI only
 ****************************************************************************/

#define GUI_PATH_LEN    512
#define GUI_RECENT_MAX  10
#define GUI_SLOT_MAX    10

/* Indices into t_pad_map.key[] and t_pad_map.button[] */
enum
{
  PAD_UP = 0,
  PAD_DOWN,
  PAD_LEFT,
  PAD_RIGHT,
  PAD_A,
  PAD_B,
  PAD_C,
  PAD_X,
  PAD_Y,
  PAD_Z,
  PAD_START,
  PAD_MODE,
  PAD_KEYS
};

typedef struct
{
  int key[PAD_KEYS];    /* Windows virtual-key code per button */
  int button[PAD_KEYS]; /* button/POV/stick mask per button (namespace depends
                            on device: XInput below GUI_INPUT_XPAD_COUNT,
                            DirectInput at or above it), 0 = unassigned */
  unsigned turbo;       /* bit per PAD_* index: that button fires on and off while held (A B C X Y Z) */
  int device;           /* -1 keyboard only, else an XInput slot or a
                            DirectInput joystick -- see GUI_INPUT_XPAD_COUNT
                            in gui.h */
  char joy_name[64];     /* DirectInput product name device was last set to,
                            empty for keyboard/XInput -- lets a saved mapping
                            find the same physical joystick again even if it
                            re-enumerates at a different index */
} t_pad_map;

/* video.aspect */
enum { ASPECT_PAR = 0, ASPECT_SQUARE, ASPECT_43, ASPECT_STRETCH };   /* ASPECT_PAR: the real console's pixel shape */

/* gui.renderer */
enum { RENDERER_GDI = 0, RENDERER_D3D9 = 1, RENDERER_D3D11 = 2 };

typedef struct
{
  /* video */
  int scale;              /* 1-4, integer window scale at 320x224 */
  int aspect;             /* one of the ASPECT_* values */
  int smooth;             /* 1 = smooth scaling (HALFTONE), 0 = nearest */
  int scanline_pct;       /* 0/25/50/75/100 -- darkens alternating output rows */
  int nuked_ym2612;       /* Nuked FM core: 1 = behave as a YM2612, 0 = as a YM3438 */
  int shot_size;          /* screenshot size: 0..3 = 1x..4x of the output's own size */
  int rec_scale;          /* video recording size: 0..3 = 1x..4x of the picture's own size */
  int rec_quality;        /* video recording quality: 0 = chosen from the size, 1..4 = 4, 8, 16, 32 megabits a second */
  int rec_tv;             /* video recording shape: 0 = the picture's own, 1 = stretched to 4:3, 2 = stretched to 16:9 */
  int shot_mode;          /* screenshot output: 0 final, 1 corrected (640x480, the default), 2 raw */
  int frameskip;          /* 0 off, 1 auto, 2-5 manual (25/33/50/75% audio buffer) */
  int sms_show_border;    /* 1 = show the Master System's 8-pixel side borders (0 crops them) */
  int background_input;   /* 1 = controller input keeps working while the window is not focused */
  int brighten;           /* 1 = lift the picture's brightness (offsets scanline darkening) */

  /* netplay (LAN) -- last values used in the Netplay dialog */
  int  np_port;
  int  np_delay;          /* input delay in frames, 1-4 */
  char np_addr[64];       /* last host address joined */
  int browser_col_hidden; /* bitmask, one bit per ROM browser list column (Name, Console, Favorite) */
  int browser_col_extra;  /* bitmask of the extra columns that are shown: Genre, Last Played, Play Time, Plays */
  int browser_sort_column; /* which column the ROM browser list is sorted by */
  int browser_sort_ascending;
  int browser_sort_extra[2];     /* the 2nd and 3rd sort column (Shift+click a header), -1 = none */
  int browser_sort_extra_asc[2];
  int browser_col_pct[2]; /* user-adjustable column width, as a percent of the list's width */
  int renderer;            /* which output path to use: RENDERER_GDI, RENDERER_D3D9 or
                               RENDERER_D3D11 -- always falls back to GDI automatically
                               if the chosen one is unavailable or fails */
  int vsync;                /* wait for the compositor's refresh after each frame (Direct3D
                               or GDI). Off by default: the app is paced by audio, so this
                               only helps if the display refresh is close to the game's rate;
                               skipped automatically when the sound buffer is running low */
  int fullscreen;
  int theme_mode;          /* THEME_AUTO/LIGHT/DARK, see theme.h */
  int ui_style;            /* 0 = Classic, 1 = Modern (rounded window frame, Segoe UI Variable text; more of it in later versions) */
  int large_ui;            /* 0 = normal size, 1 = ~15-20% larger throughout */
  int cheats_enabled;      /* master toggle -- 0 disables every cheat regardless of each one's own checkbox */
  int cursor_idle, frame_cap, bass, log_on, pause_pad, keep_aspect, normalize, autosave_min, skip_len, np_upnp, overdrive, osd_opts, osd_pos, osd_time, osd_size, fps_pos, osd_color, osd_lines, rewind_speed;   /* see settings.c (rewind_speed: 0 slow, 1 normal, 2 fast, 3 very fast) */
  int hide_toolbar;        /* the ROM browser's toolbar / search bar / filter bar are hidden */
  int hide_searchbar;
  int hide_filterbar;
  int shader_nofilter;     /* the CPU Render Filter is skipped while a shader runs (the shader gets the game's own picture) */
  int hide_statusbar;      /* View > Toolbars > Show Status Bar is off */
  int browser_grid_view;   /* 0 = report/list view, 1 = icon/grid view with cover art */
  int browser_grid_size;   /* icon size in pixels, adjustable via Ctrl+wheel while in grid view */
  int fullscreen_on_load;  /* persists across runs, unlike fullscreen itself --
                               enters fullscreen automatically whenever a ROM loads */
  int show_fps;
  int turbo_rate;          /* 0-4: how fast Turbo buttons fire (Controls): 7.5, 10, 15, 22.5 or 30 presses a second */
  int confirm_exit;        /* ask before exiting while a game is running */
  int single_instance;     /* a second launch hands its game to the copy already running */
  int keep_awake;          /* no sleep, screen saver or screen off while a game runs */
  int mute_unfocused;      /* the sound is muted while another program has the focus */
  int browser_col_w[12];   /* pixel width of each fixed-width list column the person set, 0 = the default */
  int browser_col_order[12]; /* the left-to-right order of the list columns the person set; [0] = -1: the default */
  int flicker_blend, dither_smooth;   /* picture clean-ups: blend flickering objects, smooth dither, black frame insertion */
  char bezel_path[GUI_PATH_LEN];           /* an image drawn over the picture as a frame ("" = none) */
  char combo_text[512];                    /* combo buttons, one per line: "Q = A+B" */
  char shader_params[1024];                /* "name=value;" for the shader settings changed from the preset's own */
  char shader_params_for[GUI_PATH_LEN];    /* the preset they belong to */
  int padhk_hold_on;       /* gamepad hotkeys only work while the Hold Button is held */
  int padhk_hold[2];       /* that button: [0] XInput pad, [1] DirectInput joystick */
  int group_console;       /* 1 = the list is grouped into collapsible sections, one per console */
  int az_strip;            /* 1 = a letter strip beside the list jumps to games starting with that letter */
  int cover_shape;         /* grid covers: 0 square, 1 box (portrait), 2 wide */
  int toolbar_size;        /* toolbar icon size: 0 small, 1 normal (the default), 2 large */
  int toolbar_labels;      /* 1 = the name of each toolbar button is written under its icon */
  int cover_cache;         /* keep finished cover thumbnails on disk (covers\thumbs.dat) so they load fast the next time */
  int cover_card;          /* grid covers: a frame around the cover and its name */
  int color_icons;         /* 1: the menu icons, the toolbar and the star are in color, 0: in the color of the text */
  int crash_report;        /* write genesis-plus-ex-crash.txt and .dmp after a crash */
  int accent;              /* accent color: 0 default (blue in the dark theme, the Windows color in the light one), 1 to 13 a color, 14 a hidden one */
  int cover_round;         /* grid covers: 0 square corners, 1 rounded */
  int cover_caption;       /* grid covers: 0 name below the cover, 1 no name */
  char rom_extra[6][GUI_PATH_LEN];   /* more ROM folders */
  int rom_extra_n;
  char rom_exclude[6][GUI_PATH_LEN]; /* folders left out of the scan */
  int rom_exclude_n;
  int stereo_idx;          /* stereo width: 0 narrow, 1 normal, 2 wide, 3 extra wide */
  int mouse_sens;          /* mouse / light gun speed: 0..4 = 50, 75, 100, 150, 200 % */
  int gun_ox, gun_oy;      /* light gun aim offset, 0..6 = -12, -8, -4, 0, 4, 8, 12 pixels */
  int adj_contrast, adj_sat, adj_gamma, adj_hue;   /* picture adjustments: contrast 50-150 %, saturation 0-200 %, gamma 50-150 (/100), hue -30 to 30 degrees; 100, 100, 100, 0 = unchanged */
  int bar_color;           /* color of the bars around the picture (index into video.c's list) */
  int fs_res_w, fs_res_h;  /* exclusive fullscreen resolution; 0 = the desktop's own */
  int fs_exclusive;        /* 1 = fullscreen changes the display mode instead of a borderless window */
  int confine_mouse;       /* keep the pointer inside the picture while a mouse / light gun game runs */
  char audio_device[96];   /* name of the sound output device ("" = the Windows default) */
  int row_density;         /* 0 compact, 1 normal, 2 roomy rows in the game list */
  int browser_gridlines;   /* lines between the columns of the game list */
  int scan_subfolders;     /* the ROM directory is searched including its subfolders */
  char filter_console[40]; /* the filter bar choices, kept between runs ("" = all) */
  char filter_genre[28];
  int  filter_genre_on;    /* filter_genre is "" for an unknown genre, so it needs its own flag */
  char filter_region[24];
  int clean_names;         /* the game list shows names without the (...) and [...] tags */
  int recent_mode;         /* the last few games are grouped at the top of the list */
  int favorites_only;      /* the game list shows only the favorites */
  int hotkey[96];          /* the keyboard shortcuts: virtual key + 0x10000 Ctrl / 0x20000 Shift / 0x40000 Alt (hotkeys.c) */
  int padhk_di[24];        /* the same for a DirectInput pad as Player 1 (its own button numbers); 0 = none, nothing is set by default */
  int padhk[24];           /* the pad button (XInput mask) for each gamepad hotkey action, 0 = none; the first two are Fast Forward and Rewind (hotkeys.c) */
  int integer_scale;       /* the picture is enlarged by whole multiples only (sharp pixels) */
  int mute_fm;             /* silence the FM chip alone */
  int mute_psg;            /* silence the PSG chip alone */
  int mute_pwm;            /* silence the 32X PWM sound alone */
  int pwm_level;           /* the level of the 32X PWM sound in percent (100: as the library makes it) */
  int resume_choice;       /* the Auto Resume question: 0 = ask, 1 = always resume, 2 = never (set by "Do not ask again") */
  int autosave;            /* save the game when it is closed and offer to resume it next time */
  int osd;                 /* messages over the picture (saved, slot, screenshot ...) */
  int fullscreen_toolbar;  /* the toolbar slides in at the top edge of the screen in fullscreen, like the menu bar */

  /* window geometry, restored on the next launch */
  int window_x;            /* top-left of the window, INT_MIN = "not set" */
  int window_y;
  int window_maximized;

  /* Built-in CPU render filter (filters.c). Stored by name rather than by
     table position so adding or reordering filters never silently changes
     what an existing settings file selects. */
  char render_filter[64];   /* empty = none */

  /* audio */
  int sound_enabled;
  int volume;             /* 0-100, applied on the mixed output */
  int sample_rate;        /* 44100 or 48000 */
  int audio_backend;      /* 0 Windows Classic (waveOut), 1 XAudio2, 2 WASAPI shared (default), 3 WASAPI exclusive */
  int latency;            /* number of queued frame buffers, 2-8 */

  /* behavior */
  int rewind_memory;      /* 0..3: how much memory the Rewind history may use (128, 256, 512, 768 MB) */
  int rewind;             /* 1 = keep the history Backspace rewinds through (uses ~200 MB) */
  int pause_on_focus_loss;
  int always_on_top;
  int fast_forward_ratio; /* frames run per displayed frame in fast forward */
  int state_slot;         /* 0-9 */

  /* input */
  t_pad_map pad[2];
  int deadzone[2];        /* 0-100%, how far a thumbstick has to move off-center before it
                              registers at all -- per player, since two different controllers
                              (or the same model with worn-in sticks) can drift by different
                              amounts. Applied both to the stick-as-D-pad digital directions and
                              to the analog reading itself (for the few games/peripherals that
                              read the stick as an analog axis rather than a D-pad). */

  /* paths */
  char rom_dir[GUI_PATH_LEN];
  char recent[GUI_RECENT_MAX][GUI_PATH_LEN];
  char shader_preset_path[GUI_PATH_LEN];   /* .slangp shader (D3D11 only); "" = none */
} t_gui_config;

/* Global variables */
extern t_config config;
extern t_gui_config gui;

extern void set_config_defaults(void);
extern void config_load(void);
extern void config_save(void);
extern void config_save_wait(void);       /* waits until the settings file is written (the write runs on its own thread) */
extern void config_add_recent(const char *path);

/* Favorite games, kept by full path in genesis-plus-ex.ini ([favorites]). */
extern int  config_fav_has(const char *path);
extern int  config_fav_set(const char *path, int on);   /* 1 when it is now as asked (0: the list is full); does not save */

#endif /* _CONFIG_H_ */
