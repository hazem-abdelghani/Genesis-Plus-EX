/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  picocore.h -- 32X games. Genesis Plus GX has no 32X, so these run on
 *  PicoDrive, which is built as a libretro core (picodrive_libretro.dll, from
 *  the picodrive folder) and loaded only when a 32X game is opened. Everything
 *  else (window, picture filters and shaders, sound output, controls, states,
 *  recording) is the program's own.
 ****************************************************************************/

#ifndef PICOCORE_H
#define PICOCORE_H

#include <stddef.h>

/* 1 when the ROM that Genesis Plus GX just loaded is a 32X game (the "SEGA 32X" header). */
int  pico_rom_is_32x(void);

/* Starts the game in PicoDrive. Returns 1 on success; on failure 0, and err (if given) says why. */
int  pico_load(const char *path, char *err, int err_len);

/* Stops it and unloads the library. */
void pico_unload(void);

/* 1 while a 32X game is the one that is running. */
int  pico_active(void);
int  pico_restart(void);               /* starts the game again from power-on (netplay) */
void pico_state_log_pieces(void);              /* the pieces of the state, one line each in the log (netplay start) */
unsigned long long pico_state_hash(void);     /* a fingerprint of the machine, to compare the two PCs of a netplay game */

/* Runs one frame (reads the controls, draws into the program's own frame buffer) and returns how many sound
   frames (stereo pairs) it made, written to out. */
int  pico_run_frame(short *out, int max_frames);

void pico_reset(void);

/* The frames per second of the game (60 or 50). */
double pico_fps(void);

/* Save states: the size a state needs at most, writing one, reading one. */
/* The region or the low-pass filter setting changed: the library is told at the next frame (and its frame rate is asked again). */
void pico_settings_changed(void);
int  pico_take_rate_change(void);  /* 1 once after the library changed its sample rate: the sound output follows */
int  pico_sample_rate(void);      /* the rate the running 32X game was started with (0: none) */

/* VGM recording of a 32X game: cb gets the writes to the FM chip (kind 1: a = port 0..3, d = data), the PSG (2: d) and the PWM
   (3: a = register 0..4, d = value), with the place in the frame as 0..65535. NULL stops it. Returns 0 when it cannot. */
int pico_sound_log(void (*cb)(int kind, unsigned a, unsigned d, unsigned frac));

/* The memory the cheats work on while a 32X game runs: what 0 is the cartridge ROM, 1 the 68000's work RAM (NULL when there is
   none, or the library has no such call). */
unsigned char *pico_memory(int what, unsigned *size);

size_t pico_state_max(void);
void   pico_state_quiet(int on);       /* 1: saving and loading states is not written to the log (rewind does it at every frame) */
int    pico_state_save(unsigned char *buf, size_t cap);          /* the length written, 0 on failure */
int    pico_state_load(const unsigned char *buf, size_t len);    /* 1 when it was loaded */

/* The game's battery-backed memory, if it has any. */
unsigned char *pico_save_ram(size_t *len);

#endif
