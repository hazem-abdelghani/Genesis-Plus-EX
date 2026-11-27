/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *  Copyright (c) 2026 Hazem Abdelghani
 *
 *  raudio.c -- keeps the sound chain unbroken for Run-Ahead (Settings > Emulation).
 *
 *  Run-Ahead runs the game a frame ahead and then loads a saved state to go back. The core's state does not hold the sound chain: the blip buffers
 *  (what the sound output is built from) and the last FM level that the next frame's first FM change is measured from. A load also starts with
 *  system_reset(), which clears them. Played as it is, the sound jumps at every frame.
 *
 *  So, for a load that Run-Ahead does, system_reset() (vgm.c) leaves the sound chain alone, and this file puts the chain back to what it was
 *  right after the real frame (the blip buffers are copied; the last FM level is a private value of the core, so what the FM chip has sent to the
 *  blip buffer is counted here, by wrapping the calls: the difference between the frame from ahead and the real frame is sent as one more step).
 *  The core is not changed: these are link-time wrappers (see --wrap in the Makefile).
 ****************************************************************************/

#include <string.h>
#include "shared.h"
#include "blip_buf.h"

extern void __real_blip_add_delta(blip_t *, unsigned int, int, int);
extern void __real_blip_add_delta_fast(blip_t *, unsigned int, int, int);
extern void __real_psg_end_frame(unsigned int);
extern int  __real_sound_update(unsigned int);
extern void __real_sound_reset(void);

int ra_audio_keep;                       /* 1 while Run-Ahead loads its state: system_reset() (vgm.c) must not clear the sound chain */

static int in_update, in_psg;            /* inside sound_update() / psg_end_frame() */
static int fm_level[2];                  /* what the FM chip has sent to the blip buffer: the core's own "last FM level" (always equal to it) */

void __wrap_blip_add_delta(blip_t *b, unsigned int t, int l, int r)
{
  if (in_update && !in_psg && b == snd.blips[0]) { fm_level[0] += l; fm_level[1] += r; }      /* (the FM chip's changes are sent from sound_update, after the PSG's) */
  __real_blip_add_delta(b, t, l, r);
}

void __wrap_blip_add_delta_fast(blip_t *b, unsigned int t, int l, int r)
{
  if (in_update && !in_psg && b == snd.blips[0]) { fm_level[0] += l; fm_level[1] += r; }
  __real_blip_add_delta_fast(b, t, l, r);
}

void __wrap_psg_end_frame(unsigned int cycles) { in_psg++; __real_psg_end_frame(cycles); in_psg--; }

int __wrap_sound_update(unsigned int cycles)
{
  int n;
  in_update++;
  n = __real_sound_update(cycles);
  in_update--;
  return n;
}

void __wrap_sound_reset(void)
{
  fm_level[0] = fm_level[1] = 0;         /* (the core puts its last FM level to 0 here) */
  __real_sound_reset();
}

/* The layout of the core's blip_t (core/sound/blip_buf.c, stereo build: it is private there). */
typedef struct { unsigned long long factor, offset; int size; int integrator[2]; int *buffer[2]; } blipx_t;
#define BLIP_EXTRA 18                    /* buf_extra of blip_buf.c */
#define BLIP_MAX   16384                 /* samples we have room for in one blip buffer */

static struct
{
  int have;
  unsigned long long offset;
  int integrator[2];
  int fm[2];
  int n;
  int *buf[2];
} snap[3];                               /* the FM and PSG chain, and for a Mega CD game the PCM chip's and the disc music's (their own "last level" is in the saved state) */
static int fm_after[2];                  /* the FM level after the frame from ahead */

static int blip_ok(blipx_t *b)
{
  return b && b->size > 64 && b->size < BLIP_MAX && b->factor && b->buffer[0] && b->buffer[1];
}

/* Can the chain be kept on this game and this build? (The layout above is checked against what the buffers say.) The cartridge's own sound
   (blip 3: the MD+ games' tracks) keeps the plain reset: its last level is not part of a state. */
int ra_audio_usable(void)
{
  int i;

  if (!snd.blips[0] || snd.blips[3]) return 0;
  for (i = 0; i < 3; i++) if (snd.blips[i] && !blip_ok((blipx_t *)snd.blips[i])) return 0;
  return 1;
}

/* After the real frame's sound was taken: the chain as the saved state belongs to it. */
void ra_audio_snapshot(void)
{
  int i;

  for (i = 0; i < 3; i++)
  {
    blipx_t *b = (blipx_t *)snd.blips[i];
    int total;

    snap[i].have = 0;
    if (!blip_ok(b)) continue;
    total = b->size + BLIP_EXTRA;
    if (snap[i].n < total)
    {
      free(snap[i].buf[0]); free(snap[i].buf[1]);
      snap[i].buf[0] = (int *)malloc((size_t)total * sizeof(int));
      snap[i].buf[1] = (int *)malloc((size_t)total * sizeof(int));
      if (!snap[i].buf[0] || !snap[i].buf[1]) { snap[i].n = 0; continue; }
      snap[i].n = total;
    }
    snap[i].offset = b->offset;
    snap[i].integrator[0] = b->integrator[0]; snap[i].integrator[1] = b->integrator[1];
    memcpy(snap[i].buf[0], b->buffer[0], (size_t)total * sizeof(int));
    memcpy(snap[i].buf[1], b->buffer[1], (size_t)total * sizeof(int));
    if (i == 0) { snap[0].fm[0] = fm_level[0]; snap[0].fm[1] = fm_level[1]; }
    snap[i].have = 1;
  }
}

/* After the last frame from ahead's sound was taken. */
void ra_audio_ahead_done(void) { fm_after[0] = fm_level[0]; fm_after[1] = fm_level[1]; }

/* After the load: the chain goes back to the one after the real frame. */
void ra_audio_restore(void)
{
  int i, dl, dr;

  for (i = 0; i < 3; i++)
  {
    blipx_t *b = (blipx_t *)snd.blips[i];
    int total;

    if (!snap[i].have || !blip_ok(b)) continue;
    total = b->size + BLIP_EXTRA;
    if (total != snap[i].n) continue;
    b->offset = snap[i].offset;
    b->integrator[0] = snap[i].integrator[0]; b->integrator[1] = snap[i].integrator[1];
    memcpy(b->buffer[0], snap[i].buf[0], (size_t)total * sizeof(int));
    memcpy(b->buffer[1], snap[i].buf[1], (size_t)total * sizeof(int));
  }
  /* The core measures the next FM change from its own last FM level, which is still the one after the frame from ahead (it is not in a state); the
     chain was put back to the level after the real frame. The difference goes in as one step at the start of the next frame. (The PCM chip's and the
     disc music's last levels are in the saved state: nothing to add for them.) */
  dl = fm_after[0] - snap[0].fm[0]; dr = fm_after[1] - snap[0].fm[1];
  if (snap[0].have && (dl || dr))
  {
    if (config.hq_fm) __real_blip_add_delta(snd.blips[0], 0, dl, dr);
    else              __real_blip_add_delta_fast(snd.blips[0], 0, dl, dr);
  }
}
