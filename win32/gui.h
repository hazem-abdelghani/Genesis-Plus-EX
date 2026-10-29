/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  gui.h -- declarations shared between the frontend modules. The emulator
 *           core never sees this file.
 ****************************************************************************/

#ifndef _GUI_H_
#define _GUI_H_

#include <windows.h>

#define APP_NAME    "Genesis Plus EX"
#define APP_NAME_W  L"Genesis Plus EX"
#define APP_CLASS   "GenesisPlusEXWindow"
#define APP_VERSION "3.0.3"
#ifdef _WIN64
#define APP_BITS    "64-bit"
#else
#define APP_BITS    "32-bit"
#endif

/* Largest frame the core can produce, and the pixel format it renders in. */
#define FRAME_MAX_W 720
#define FRAME_MAX_H 576
#define FRAME_BPP   2

/****************************************************************************
 * main.c
 ****************************************************************************/

extern HWND      g_hwnd;
extern HWND      g_status;
extern HFONT     gui_get_ui_font(void);
extern HINSTANCE g_inst;

extern int emu_running;   /* a ROM is loaded */
extern const char *emu_rom_filename(void);
extern const char *emu_rom_path(void);
extern int emu_paused;    /* emulation is suspended */

extern int  emu_load_rom(const char *path);
extern int  emu_peek_rom(const char *path);
extern void emu_close_rom(void);
extern double emu_frame_rate(void);      /* real frame rate of the emulated console */
extern const char *emu_rom_base(void);   /* file name of the loaded game, no folder or extension */
extern void emu_reset(int hard);
extern void emu_save_state(int slot);
extern void emu_load_state(int slot);
extern void emu_undo_load_state(void);   /* back to the game as it was before the last load */
extern int  emu_can_undo_load(void);
extern void emu_undo_save_state(void);   /* put the slot back as it was before the last save */
extern int  emu_can_undo_save(void);
extern void gui_snprintf(char *out, int out_len, const char *fmt, ...);   /* never overruns out */
extern void emu_apply_region(void);

/* Per-game data files (battery saves, save states, cheats) live in
   <kind>\<Console>\<game name> [<ROM id>]<ext>, e.g.
   states\MegaDrive\Sonic The Hedgehog [A1B2C3D4].gp0
   The console folder groups them; the 8-digit id (a hash of the game's data)
   keeps two different games with the same file name apart.
   emu_data_path(): for_read=0 gives the path a file should be written to;
   for_read=1 gives the path of the existing file to read -- the new name if
   there is one, else the same game under another console folder, else the
   old flat name from earlier versions, else the new name. */
extern void emu_data_path(const char *kind, const char *ext, int for_read, char *out, int out_len);
extern void emu_ensure_data_dir(const char *kind);
extern void emu_console_file_path(const char *kind, const char *ext, char *out, int out_len);   /* <kind>\\<Console>\\<ROM name><ext>; makes the folders */
extern void state_delete(int slot);
extern void emu_apply_audio_settings(void);

/* Writes to the status bar's message pane. */
extern void gui_status(const char *fmt, ...);
extern void gui_status_persistent(const char *fmt, ...);
extern void gui_status_slot(const char *text);

/* Shows a short overlay message on the video output for a couple of seconds. */
extern void gui_notify(const char *fmt, ...);
extern int  gui_port_index(int port);   /* which device (0..9, as in the Port A / B lists) is plugged into controller port 0 / 1 */
extern void waveout_set_enhance(int bass, int normalize, int overdrive);   /* Bass Boost, Even Out Loudness and Overdrive */
extern void waveout_meter(int *l, int *r);                  /* peak levels since the last call, for the meter */
extern void gui_diag_start(void);                  /* log the frame rate and sound queue for a few seconds (when logging is on) */
extern void gui_pad_back(const char *name);        /* a pad is connected again: resumes a game paused by gui_pad_lost */
extern void gui_pad_lost(const char *name);        /* Player 1's pad went away: pauses when asked to */
extern void taskbar_progress(HWND hwnd, int mode, int pct);   /* green bar on the taskbar button: 0 none, 1 pct, 2 moving */
extern void gui_log(const char *fmt, ...);          /* one line in genesis-plus-ex.log, when logging is on */
extern double video_display_aspect(void);          /* width over height the picture is drawn in, 0 = fills the window */

extern void gui_update_menu(void);
extern void gui_resize_to_scale(int scale);
extern void apply_always_on_top(void);

/****************************************************************************
 * video.c
 ****************************************************************************/

extern int   video_init(void);
extern void  video_shutdown(void);
extern void  video_frame(void);          /* blit the current core bitmap */
extern void  video_deinterlace_reset(void);  /* the picture changed under it (a state was loaded, a rewind step): start its history again */
extern void  video_deinterlace(void);    /* Interlaced Mode > Double Field: removes the comb teeth of a moving object from the frame just made */
extern void  video_repaint(HDC hdc);     /* redraw from WM_PAINT */
extern void  video_set_ntsc(int mode);   /* 0 off, 1 composite, 2 s-video, 3 rgb */
extern void  video_viewport_changed(void);
extern int   gui_pico_unsupported(int id);     /* 1: does nothing for the 32X game that is running (grayed out) */
extern void  video_pico_frame(const void *src, int w, int h, int pitch);   /* a 32X game's picture (RGB565) into the frame buffer */
extern void  video_force_redraw(void);
extern void  video_set_renderer(int renderer);
extern void  video_set_vsync_allowed(int allowed);   /* false: skip this frame's VSync wait */
extern void  video_invalidate(void);
extern void  video_preferred_size(int scale, int *w, int *h);
extern void browser_display_name(const char *name, char *out, int out_len);   /* a game's name as the list shows it (View > Game List > Clean Game Names) */
extern int   video_screenshot(char *path_out, int path_len);
extern void video_wait_screenshots(void);                      /* at exit: lets a screenshot just taken be finished */
extern int   video_save_thumbnail(const char *path);
extern HBITMAP video_load_thumbnail(const char *path);
extern void  state_path(int slot, char *out, int out_len);
extern void  thumb_path(int slot, char *out, int out_len);
/* "hh:mm:ss  DD/MM/YYYY" (local time) of a state file's last save; 0 if it doesn't exist. */
extern int   state_file_time(const char *path, char *out, int out_len);
extern void  video_show_notice(const char *text, int ms);
extern void  video_osd_font_changed(void);     /* Font Size under Settings > OSD */
extern int   rewind_set_memory(int mb);                      /* the history's size; 0 = not enough memory */
extern int  extras_make_shortcut(const char *rom_path, int start_menu);
extern void emu_insert_disc(void);                    /* File > Insert Disc... */
extern void emu_save_state_file(const char *path);     /* a state in a file of the player's choice */
extern void emu_load_state_file(const char *path);
extern void browser_density_changed(void);
#define WM_APP_PICOSCAN (WM_APP + 43)       /* browser.c: the thread that looks for Sega Pico games has an answer */
extern void browser_pico_done(LPARAM result);
extern int   rewind_status(void);                            /* frames of history left */
extern void  video_show_rewind(int permille, int tenths);   /* the rewind timeline bar, shown for a moment */
extern void  video_get_output_rect(RECT *dest, int *src_w, int *src_h);
extern void  video_set_fullscreen(int on);
extern void  video_report_fps(int fps);

/* Built-in render filters (filters.c). Filters are addressed by index into a
   fixed table; -1 means "no filter". */
extern int   video_filter_count(void);
extern const char *video_filter_name(int index);
extern const char *video_filter_group(int index);                 /* the sub-menu of Render Filter it is listed in, or NULL */
extern int   video_filter_current(void);                   /* index, or -1 */
extern void  video_set_filter(int index);
extern int   video_set_filter_by_name(const char *name);   /* 1 if it matched */

/****************************************************************************
 * audio.c
 ****************************************************************************/

extern int  waveout_open(int sample_rate);
extern void waveout_close(void);
extern void waveout_submit(const short *samples, int frames);
extern int  waveout_pending(void);       /* buffers still queued on the device */
extern int  waveout_broken(void);        /* device closed or refusing data */
extern int  waveout_stalled(void);       /* queued but nothing finishing (about 1/s while true) */
extern void waveout_flush(void);
extern void waveout_set_volume(int percent);
extern int  waveout_rate(void);
extern void waveout_set_width(int percent);                 /* stereo width, 100 = unchanged */
extern int  waveout_list_devices(char names[][96], int max);
extern unsigned video_bar_color(void);                      /* 0xRRGGBB of the bars around the picture */
extern void video_adjust_changed(void);
extern void video_restore_display_mode(void);
extern int  video_fullscreen_modes(int *ws, int *hs, int max);
extern int  gui_slider_range(int id, int *lo, int *hi, int *def);   /* a Picture Adjustments slider: 1 if id is one */
extern int  gui_slider_get(int id);
extern void gui_slider_set(int id, int v);
extern void gui_slider_commit(void);          /* saves the sliders (not done at each step of a drag) */
extern size_t settings_guide_text(char *out, size_t cap);   /* the Settings window as text for the User Guide (every tab and option); returns the length (out may be NULL) */
extern int  gui_dyn_list(int base, char names[][96], int max, int *cur);   /* items of a drop-down filled at run time (sound device, fullscreen resolution) */
extern int  gui_setgroup_state(int id);                     /* settings-window choices: -1 not one, else 1 selected / 0 not */
extern int  gui_setgroup_set(int id);                       /* applies it; 1 if id was one */

/****************************************************************************
 * input.c
 ****************************************************************************/

/* t_pad_map.device: -1 keyboard only, 0..GUI_INPUT_XPAD_COUNT-1 an XInput
   pad slot, GUI_INPUT_XPAD_COUNT and up a DirectInput joystick (only those
   the pad's own product name -- t_pad_map.joy_name -- can still be matched
   to are actually usable; see gui_input_resolve_device()). */
#define GUI_INPUT_XPAD_COUNT 4
#define GUI_INPUT_DI_MAX     8

extern void gui_input_init(void);
extern void gui_input_shutdown(void);
extern void gui_combos_release(void);          /* lets go of every Hold combo that is switched on */
extern void gui_input_set_focus(int focused);
extern int  gui_input_fast_forward(void);
extern int  gui_input_rewind(void);
extern void theme_paint_menu_line(HWND hwnd, HDC dc);
extern void hotkey_menu_keys(int cmd, char *out, int len);
extern void dlg_cheat_search(HWND parent);        /* Tools > Cheat Search: a window that stays open while the game runs */
extern int  cheat_search_message(MSG *msg);       /* keyboard handling for it; 1 = used the message */
extern void cheat_search_close(void);             /* when the game stops */
extern void cheat_search_invalidate(void);        /* reset / state load: the search starts again */
extern void cheat_search_enable(int on);          /* off while a modal window is open */
extern HWND cheat_search_hwnd(void);
extern int  gui_input_gun_cursor(void);          /* 1 = crosshair over the picture with a light gun, else 0 */
extern int  gui_input_ff_latched(void);          /* Emulation > Fast Forward is ticked */
extern int  gui_input_rewind_latched(void);
extern void gui_input_toggle_ff(void);
extern void gui_input_toggle_rewind(void);
extern void gui_input_clear_latches(void);
extern const char *gui_input_xpad_name(int slot);   /* "" when not known */
extern void gui_input_devices_changed(void);        /* a USB / Bluetooth device came or went */
extern int  gui_input_xpad_connected(int slot);    /* an XInput pad is plugged into this slot */
extern int  gui_input_toggle_turbo(void);       /* gamepad hotkey: Turbo buttons on / off (returns on = 1); the buttons keep their Turbo setting */
extern int  gui_input_frame_advance(void);
extern uint8 gui_user_ports[2];       /* the devices of the two controller ports as the user chose them */
extern int  input_press_logged;       /* log only: the first button press of the game was seen */
extern int  gui_input_p2_slot(void);   /* -1 unless both ports have a plain control pad */
extern int  gui_input_save_slot_shortcut(void);
extern int  gui_input_load_slot_shortcut(void);

/* DirectInput joysticks currently attached (XInput-capable pads excluded --
   those are only ever addressed as one of the fixed XInput slots above).
   Re-scanned by gui_input_refresh_joysticks(), which the config dialog calls
   every time it opens so newly plugged devices show up without a restart. */
extern void        gui_input_refresh_joysticks(void);
extern int         gui_input_joystick_count(void);
extern const char *gui_input_joystick_name(int index);      /* 0-based DI index */
extern void        gui_input_default_buttons(int device, int *button);   /* the pad buttons a mapping starts with on that device (PAD_KEYS values) */

/* Best current device index for a mapping saved/opened with this device
   value and (if it was a DirectInput one) this product name: re-matches by
   name against the devices currently enumerated so replugging a joystick
   into a different port, or in a different order across two runs, doesn't
   silently detach it from whichever player it was assigned to. Falls back
   to the saved index verbatim (harmless -- just reads as disconnected) if
   no device with that name is currently attached. avoid_device is a device
   value this call must never return (pass the other player's already-
   resolved device, or -1) -- otherwise two identically-named controllers
   could both resolve to the same physical one. */
extern int  gui_input_resolve_device(int device, const char *joy_name, int avoid_device);

extern int  input_capture_key(void);
extern int  input_capture_gamepad(int device);
extern int  gui_input_menu_pad(void);                  /* pause menu: 1 up, 2 down, 4 choose, 8 back (new presses) */
extern void video_show_menu(const char *const *items, int n, int sel);   /* the pause menu over the picture; n = 0 removes it */
extern int  gui_input_pad_hotkey(void);                  /* Back + button: the menu command, or 0 */
extern int  input_pad_button_down(int device, int mask);   /* is that assigned button held right now */
extern int  input_any_input_down(int device);

extern const char *input_key_name(int vk);
extern const char *input_pad_button_name(int device, int mask);
extern const char *input_button_label(int index);

/****************************************************************************
 * cheats.c
 ****************************************************************************/

#define CHEAT_MAX 1000

extern int  cheats_import_file(const char *path);   /* a .cht file (RetroArch or plain lines) added to the list; count, or -1 */
extern void cheats_apply(void);
extern unsigned long emu_rom_crc32(void);       /* CRC32 of the running game's ROM as a database lists it; 0 = none */
extern int  cheats_wordram_1m(void);
extern void extras_shader_params(HWND parent);
extern int  vgm_active(void);
extern int  vgm_start(const char *path);
extern void vgm_stop(void);
extern void vgm_frame_end(void);
extern void vgm_attach(void);          /* keeps the sound-write copy fed (see vgm.c) */
extern int  vgm_restore_sound(void);   /* after the sound chips were set up anew: writes back what the game had set; 0 when it cannot */
extern void movie_stop(void);
extern int  movie_record(const char *path);
extern int  movie_play(const char *path, char *err, int err_len);
extern void movie_frame(void);
extern int  patches_make_copy(const char *rom_path, const char *load_path, char *out, int out_len, char *what, int what_len);
extern void browser_capture_columns(void);
extern int  browser_type_ahead(MSG *msg);   /* a letter typed elsewhere goes to the game list (type to find a game) */
extern void browser_cover_style_changed(void);
extern void browser_covers_shutdown(void);          /* at exit: stops the cover threads */
extern void browser_refilter(void);
extern void browser_layout_changed(void);   /* the A-Z strip was switched on or off */
extern void dlg_rom_folders(HWND parent);
extern void cheats_suspend(void);
extern void cheats_ram_update(void);
extern void ROMCheatUpdate(void);          /* called by the core on rebanking */

extern int  cheats_add(const char *code, const char *desc);
extern void cheats_remove(int index);
extern void cheats_remove_all(void);
extern void cheats_set_enabled(int index, int on);

extern int  cheats_active(void);
extern void cheats_hold_for_boot(void);         /* a game starts or is reset: ROM codes wait for the first button press (the game checks its ROM first)  */
extern unsigned cheats_fingerprint(void);    /* the cheats that are on, as one number (0: none) */
extern int  cheats_effective(void);        /* 0 while Enable Cheats is off */
extern void cheats_set_all_enabled(int on);
extern int  cheats_is_valid(int index);

/* Accent colors: 0 the default, 1..GUI_ACCENT_HIDDEN-1 the listed ones, GUI_ACCENT_HIDDEN the one found by an easter egg (eggs.c). */
#define GUI_ACCENT_HIDDEN 19
extern void cheats_load_for_rom(const char *rom_base);
extern void cheats_save_for_rom(const char *rom_base);
extern void cheats_sync_with_disk(const char *rom_base);
extern void cheats_sync_current(void);

/* Saves against whatever ROM is loaded; implemented in main.c. */
extern void cheats_save_current(void);

extern void dlg_cheats(HWND parent);

/****************************************************************************
 * browser.c
 ****************************************************************************/

extern void browser_panel_create(HWND parent);
extern void browser_panel_show(int show);
extern int  browser_panel_visible(void);
extern int  browser_has_selection(void);
extern void browser_show_rom_info(void);   /* File > ROM Information on the game picked in the list */
extern void browser_edit_cheats(void);      /* Tools > Cheats on the game picked in the list (no game running) */
extern void browser_panel_layout(const RECT *area);
extern void browser_grid_zoom(int delta);
extern void browser_relayout(void);   /* implemented in main.c */
extern int  browser_panel_handle_command(WORD id, WORD notify_code);
extern int  browser_panel_handle_notify(NMHDR *hdr);
extern int  browser_panel_handle_return(void);
extern int  browser_panel_handle_contextmenu(HWND target, int x, int y);
extern HBRUSH browser_panel_ctlcolor(HWND ctrl, HDC hdc);
extern void browser_panel_change_folder(void);
extern void browser_apply_view_mode(void);
extern void browser_bars_changed(void);        /* toolbar / search bar / filter bar switched on or off */
extern void browser_focus_search(void);        /* Ctrl+F */
extern int  browser_search_shown(void);
extern void browser_toolbar_tips(void);          /* toolbar tooltips with the current shortcuts */
extern void browser_toggle_searchbar(void);
extern int  browser_toolbar_height_at(int width);   /* the same, for a window `width` wide */
extern int  browser_toolbar_height(void);      /* 0 when the toolbar is hidden or fullscreen */
extern void browser_toolbar_layout(void);      /* place / hide the toolbar (hidden in fullscreen) */
extern void browser_toolbar_update(void);      /* pressed look of the toolbar's switches */
extern int  browser_column_hidden(int col);
extern void browser_fullscreen_toolbar(int show);    /* fullscreen: the toolbar over the picture while the mouse is at the top */
extern int  browser_fullscreen_toolbar_shown_height(void);
extern void browser_fullscreen_toolbar_refresh(void);   /* paints the fullscreen toolbar again (it was sometimes left half drawn) */
extern void browser_toggle_recent(void);           /* View > Game List > Show Recently Played */
extern void browser_refresh_names(void);
extern void browser_hide_search_now(void);        /* Ctrl+F: hide the search bar for this run only */
extern void browser_rescan_now(void);              /* search the ROM directory again */
extern void browser_redraw(void);
extern void browser_genre_for(const char *path, char *out, int out_len);
extern void browser_toggle_favorites_only(void);    /* the toolbar star / View > Game List > Show Favorites Only */
extern int  browser_favorites_only(void);
extern LRESULT browser_list_customdraw(NMHDR *hdr); /* -1 when it is not the game list */
extern void browser_toggle_column(int col);

/****************************************************************************
 * rewind.c
 ****************************************************************************/

extern void rewind_init(void);
extern void rewind_reset(void);
extern void rewind_capture_tick(const int16 *audio_samples, int audio_frames);
extern void rewind_enable(int on);   /* allocates / frees the history (about 200 MB) */
extern int  rewind_ready(void);      /* the history exists (enable succeeded) */
extern int  rewind_step(void);
extern const int16 *rewind_get_audio(int *out_frames);

/****************************************************************************
 * dialogs.c
 ****************************************************************************/

extern void dlg_about(HWND parent);
extern void emu_apply_fm_settings(void);      /* re-create the FM chip after its type changed */
extern void emu_apply_nuked_type(void);
extern void emu_apply_sms_border(void);
extern const char *browser_console_for_path(const char *full_path);   /* NULL if unknown */
extern void dlg_netplay(HWND parent);

/* Fullscreen hides the mouse cursor with a ShowCursor() counter, which would
   also hide it inside any dialog opened meanwhile (e.g. Ctrl+C for cheats).
   gui_dialog_box() shows it for the dialog's lifetime and puts it back. */
extern INT_PTR gui_dialog_box(int template_id, HWND parent, DLGPROC proc);
extern void dlg_input_combos(HWND parent);   /* Controls window opened on its Combos tab */
extern INT_PTR gui_modal(int template_id, HWND parent, DLGPROC proc, LPARAM lp);   /* centered; the game keeps running if Settings says so */
extern void gui_restore_combo_animation(void);   /* puts the drop-down animation setting back (crash handler) */
extern void gui_center_window(HWND w);
extern int  gui_game_running(void);        /* a game is running */
extern void gui_window_icon(HWND w);       /* the program's icon in the title bar of one of its windows */
extern HBITMAP gui_logo_bitmap(int dh, int *pw);   /* the program's logo, dh pixels high (the caller deletes it); *pw = its width */
extern HBITMAP gui_scale_rgba(const unsigned char *img, int sw, int sh, int dh, int *pw);   /* RGBA picture -> premultiplied BGRA bitmap dh high (box filter); *pw = its width */
extern int  gui_popup(HMENU menu, UINT flags, int x, int y);   /* a pop-up menu of one of the program's windows, drawn like the main menus; returns the command (TPM_RETURNCMD is added) */
extern int  gui_pico_only(int id);      /* 1 = an option that only 32X games use, and a game of another system is running: gray it */
extern int  gui_msgbox(HWND owner, const char *text, const char *caption, UINT flags);   /* MessageBox, centered over the program's window */
#define MessageBoxA gui_msgbox
extern void gui_pace(int on);                 /* a window with its own message loop opens (1) / closes (0): the game keeps being driven */
extern void gui_run_step(void);              /* one step of the main loop, from a modal window */
/* Asks for some text (multiline: a big box for pasting). Returns 1 for OK. */
extern int     gui_cursor_show(void);
extern int     gui_cursor_depth;         /* how many windows have asked for the pointer (no idle hiding meanwhile) */
extern void    gui_cursor_restore(int hidden_count);

/* Open-file picker (see dialogs.c). `which` selects which remembered-state
   slot the dialog uses so the pickers never share state. */
#define GUI_PICK_ROM    0
#define GUI_PICK_CHEAT  1
#define GUI_PICK_COVER  2
#define GUI_PICK_SHADER 3
#define GUI_PICK_STATE  4
#define GUI_PICK_COUNT  5
extern int gui_save_file(HWND owner, const char *title, const char *filter, const char *defext,
                         const char *suggest, char *out, int out_len);   /* a "Save as" window; 1 = a name was chosen */
extern int gui_pick_file(HWND owner, int which, const char *title, const char *filter,
                         const char *start_dir, char *out, int out_len);
extern void dlg_state_manager(HWND parent);
extern void dlg_input(HWND parent, int player);
extern void dlg_audio(HWND parent);
extern void dlg_picture_adjust(HWND parent);       /* Video > Picture Adjustments... */
extern void dlg_shortcuts(HWND parent);
extern void dlg_user_guide(HWND parent);
extern HMENU gui_main_menu(void);
extern int  gui_shader_available(void);          /* librashader + Direct3D 11 present (says what is missing) */
extern int  gui_set_shader(const char *path);    /* a .slangp preset, or NULL / "" for none */
/* Tooltips for a dialog: a tip on a control by id and/or on the plain text label (a static with that
   text) beside it; they show after a short delay and hide after 30 seconds. */
typedef struct { int id; const char *label; const char *tip; } GuiTip;
extern void gui_dialog_tips(HWND dlg, const GuiTip *defs, int n);
extern void gui_list_header_menu(HWND list);   /* right-clicking the list's column titles offers Size All Columns to Fit */

/* history.c: when each game was last played, how often and for how long */
extern void history_load(void);
extern void history_remove(const char *path);        /* right-click > Remove From Recently Played */
extern void history_clear(void);                  /* Clear Recently Played in the right-click menu */
extern void browser_clear_recent(void);
extern int  browser_toolbar_one_row_width(void);   /* the width at which the toolbar does not wrap */
extern COLORREF video_bar_color_of(int n);      /* the color of the n-th Border Color choice (Settings > Video) */
extern COLORREF video_osd_color_of(int n);      /* the color of the n-th Font Color choice (Settings > OSD) */
extern int  history_count(void);
extern const char *history_entry_path(int i);
extern unsigned long long history_entry_last(int i);
extern void history_restore(const char *path, unsigned long long last);   /* config.c reading the ini */
extern void config_history_load(void);
extern void history_save(void);
extern void history_game_started(const char *path);
extern void history_game_ended(void);
extern unsigned long long history_last(const char *path);   /* when the game was last opened (Unix time), 0 = never */

/* hotkeys.c: the keyboard shortcuts */
extern int  hotkeys_count(void);
extern const char *hotkey_owner_of_key(int vk);          /* the shortcut that is this plain key, or NULL */
extern int  padhk_mask(int i);                          /* the pad button of gamepad hotkey i for Player 1's pad (XInput or DirectInput numbers) */
extern const char *padhk_owner_of_button(int mask);     /* the gamepad hotkey on this pad button, or NULL */
extern int  hotkey_hold_vk(int cmd);   /* the key held for Fast Forward / Rewind */
#define HK_FASTFWD IDM_EMU_FASTFWD
#define HK_REWIND  IDM_EMU_REWINDNOW
#define PADHK_FASTFWD 0       /* gui.padhk[]: the first two are held buttons */
#define PADHK_REWIND  1
extern int  padhk_is_hold(int i);
extern void padhk_ini_key(int i, char *out);
extern int  padhk_count(void);          /* gamepad hotkeys (hold Back + a button) */
extern int  padhk_cmd(int i);
extern int  padhk_default(int i);
extern void dlg_pad_hotkeys(HWND parent, int dev);   /* Controls > Gamepad Hotkeys... */
extern const char *hotkey_name(int i);
extern int  hotkey_default(int i);
extern void hotkey_ini_key(int i, char *out, int len);
extern void hotkey_text(int code, char *out, int len);         /* "Ctrl+Shift+R" */
extern int  hotkey_frame_advance_vk(void);
extern HACCEL hotkeys_build_accel(void);
extern void hotkeys_apply_menu(void);                          /* menu labels follow the shortcuts */
extern void hotkeys_describe(char *out, int len);               /* "Action<TAB>Shortcut" lines */
extern void dlg_hotkeys(HWND parent);
extern void gui_hotkeys_changed(void);                          /* main.c: rebuild the table and the menu labels */

extern void dlg_settings(HWND parent);   /* settings.c */
extern void dlg_settings_tab(HWND parent, int tab);   /* the same, opened on tab 0 General, 1 Video, 2 Audio ... */
extern void settings_register_classes(void);
extern void settings_draw_frame(HDC dc, HWND dlg, const RECT *fr, const char *title);   /* a titled rounded frame, in the Settings style */
extern int  settings_tab_height(void);
extern void settings_set_tabs(HWND tab, const char *const *titles, int n);
#define GP_TABS_CHANGED 0x1000           /* WM_COMMAND code the tab strip sends; the selection is its GWLP_USERDATA */
extern void dlg_rom_info(HWND parent);

#endif /* _GUI_H_ */
