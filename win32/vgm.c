/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  vgm.c -- logs what the sound chips are told to play to a .vgm file, which
 *  players that support the format can play back. The writes are caught by
 *  replacing the core's FM write function and wrapping the PSG one at link time
 *  (see the Makefile); the core itself is untouched.
 ****************************************************************************/

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shared.h"
#include "gui.h"

extern void __real_psg_write(unsigned int clocks, unsigned int data);
extern void __real_system_init(void);
extern void __real_system_reset(void);

static FILE *vgm;
static unsigned char *buf;
static size_t len, cap;
static unsigned total_samples, frame_pos, spf;
static unsigned frame_cycles;
static void (*real_fm)(unsigned int, unsigned int, unsigned int);
static int fm_is_opll;
static unsigned char fm_reg[2];

/* A copy of what the game has told the sound chips, kept so that when the FM chip emulation is switched while a game runs
   (the new chip starts blank, and the game's music driver will not send its instrument set-up again) the registers can be
   written into it and the music carries on. Cleared when a game starts or resets. (After a state is loaded the copy only
   holds what was written since, which is still more than a blank chip.) */
static unsigned char ym_reg[2][256];
static unsigned char ym_set[2][256];
static unsigned char ym_keyon[8];            /* slot mask last keyed for each channel (28h: channel 0-2, 4-6) */
static unsigned char fm_latch[2];
static int psg_latch;
static unsigned psg_tone[3];
static unsigned char psg_vol[4], psg_noise;

static int fm_chip_is_opll(void)
{
  return ((system_hw & SYSTEM_PBC) != SYSTEM_MD) && (system_hw != SYSTEM_MCD);
}

static void shadow_clear(void)
{
  int i;
  memset(ym_reg, 0, sizeof(ym_reg));
  memset(ym_set, 0, sizeof(ym_set));
  memset(ym_keyon, 0, sizeof(ym_keyon));
  memset(fm_latch, 0, sizeof(fm_latch));
  psg_latch = 0;
  for (i = 0; i < 3; i++) psg_tone[i] = 0;
  for (i = 0; i < 4; i++) psg_vol[i] = 0xF;
  psg_noise = 0;
}

static void shadow_fm(unsigned int address, unsigned int data)
{
  int port = (address >> 1) & 1;

  if (!(address & 1)) { fm_latch[port] = (unsigned char)data; return; }
  if (fm_chip_is_opll()) { ym_reg[0][fm_latch[0] & 0x3F] = (unsigned char)data; ym_set[0][fm_latch[0] & 0x3F] = 1; return; }
  if (port == 0 && fm_latch[0] == 0x28)
  {
    int ch = data & 7;
    if (ch != 3 && ch != 7) ym_keyon[ch] = (unsigned char)(data >> 4);
    return;
  }
  ym_reg[port][fm_latch[port]] = (unsigned char)data;
  ym_set[port][fm_latch[port]] = 1;
}

static void shadow_psg(unsigned int data)
{
  if (data & 0x80)
  {
    psg_latch = (data >> 4) & 7;
    if (psg_latch < 6 && !(psg_latch & 1)) psg_tone[psg_latch >> 1] = (psg_tone[psg_latch >> 1] & 0x3F0) | (data & 0x0F);
    else if (psg_latch == 6) psg_noise = (unsigned char)(data & 7);
    else psg_vol[psg_latch >> 1] = (unsigned char)(data & 0x0F);
  }
  else if (psg_latch < 6 && !(psg_latch & 1)) psg_tone[psg_latch >> 1] = (psg_tone[psg_latch >> 1] & 0x00F) | ((data & 0x3F) << 4);
  else if (psg_latch == 6) psg_noise = (unsigned char)(data & 7);
  else psg_vol[psg_latch >> 1] = (unsigned char)(data & 0x0F);
}

static void put(const void *p, size_t n)
{
  if (len + n > cap)
  {
    unsigned char *g;
    cap = cap ? cap * 2 : 65536;
    while (cap < len + n) cap *= 2;
    g = (unsigned char *)realloc(buf, cap);
    if (!g) return;
    buf = g;
  }
  memcpy(buf + len, p, n);
  len += n;
}

static void put8(int v) { unsigned char c = (unsigned char)v; put(&c, 1); }

static void wait_to(unsigned pos)
{
  if (pos > spf) pos = spf;
  while (pos > frame_pos)
  {
    unsigned n = pos - frame_pos;
    if (n > 65535) n = 65535;
    put8(0x61); put8((int)(n & 0xFF)); put8((int)(n >> 8));
    frame_pos += n;
    total_samples += n;
  }
}

static void at_cycles(unsigned cycles)
{
  wait_to(frame_cycles ? (unsigned)(((unsigned long long)cycles * spf) / frame_cycles) : 0);
}

static void hook_fm(unsigned int cycles, unsigned int address, unsigned int data)
{
  shadow_fm(address, data);
  if (vgm)
  {
    int port = (address >> 1) & 1;

    at_cycles(cycles);
    if (!(address & 1)) fm_reg[port] = (unsigned char)data;
    else if (fm_is_opll) { put8(0x51); put8(fm_reg[0]); put8((int)data); }
    else { put8(0x52 + port); put8(fm_reg[port]); put8((int)data); }
  }
  real_fm(cycles, address, data);
}

void __wrap_psg_write(unsigned int clocks, unsigned int data)
{
  shadow_psg(data);
  if (vgm)
  {
    at_cycles(clocks);
    put8(0x50); put8((int)data);
  }
  __real_psg_write(clocks, data);
}

/* The write hook stays in place for good (it feeds the copy above); the core puts its own function back whenever the
   sound chips are set up again, so this is called after that and every frame. */
void vgm_attach(void)
{
  if (fm_write && fm_write != hook_fm) { real_fm = fm_write; fm_write = hook_fm; }
}

/* A game starts, or is reset: the chips are blank, and so is the copy. */
void __wrap_system_init(void)  { __real_system_init();  shadow_clear(); vgm_attach(); }
void __wrap_system_reset(void) { __real_system_reset(); shadow_clear(); vgm_attach(); }

/* Writes the remembered registers into the freshly set-up FM chip and PSG.
   The chips take one write at a time (the Nuked FM chip loses a write that arrives before the previous one was processed),
   so the writes are spaced out in time through a frame that is then thrown away: the chips are clocked between the writes
   and every counter is back at the start of a frame afterwards. */
static unsigned rs_cyc, rs_step;

static void rs_fm(unsigned address, unsigned data)  { real_fm(rs_cyc, address, data); rs_cyc += rs_step; }
static void rs_psg(unsigned data)                  { __real_psg_write(rs_cyc, data); rs_cyc += rs_step / 4; }

int vgm_restore_sound(void)
{
  int port, r, ch, i, writes = 0;
  unsigned frame = (unsigned)lines_per_frame * 3420u;
  static short discard[5000 * 2];

  vgm_attach();
  if (!real_fm) return 0;

  for (port = 0; port < 2; port++)
    for (r = 0; r < 256; r++) if (ym_set[port][r]) writes += 2;
  for (ch = 0; ch < 8; ch++) if (ym_keyon[ch]) writes += 2;
  writes += 2 * 3 + 4 + 8;                              /* the PSG */
  rs_step = frame / 2 / (unsigned)(writes + 1);         /* half a frame is plenty; the rest is left for the chips to settle */
  if (rs_step > 1500) rs_step = 1500;
  if (rs_step < 400) rs_step = 400;
  rs_cyc = 0;

  if (fm_chip_is_opll())
  {
    for (r = 0; r < 0x40; r++)
      if (ym_set[0][r]) { rs_fm(0, (unsigned)r); rs_fm(1, ym_reg[0][r]); }
  }
  else
  {
    /* channel and operator registers first, then the LFO, timers and DAC, then the keys */
    for (port = 0; port < 2; port++)
      for (r = 0x30; r < 0xC0; r++)
        if (ym_set[port][r]) { rs_fm((unsigned)(port * 2), (unsigned)r); rs_fm((unsigned)(port * 2 + 1), ym_reg[port][r]); }
    for (r = 0x22; r < 0x30; r++)
      if (ym_set[0][r] && r != 0x27) { rs_fm(0, (unsigned)r); rs_fm(1, ym_reg[0][r]); }
    if (ym_set[0][0x27]) { rs_fm(0, 0x27); rs_fm(1, ym_reg[0][0x27] & 0xCF); }          /* the channel 3 mode and the timers (a music driver may keep its tempo on them), without their reset strobes */
    for (ch = 0; ch < 8; ch++)
      if (ch != 3 && ch != 7 && ym_keyon[ch]) { rs_fm(0, 0x28); rs_fm(1, (unsigned)(ym_keyon[ch] << 4) | (unsigned)ch); }
  }

  for (i = 0; i < 3; i++)
  {
    rs_psg(0x80 | (unsigned)((i * 2) << 4) | (psg_tone[i] & 0x0F));
    rs_psg((psg_tone[i] >> 4) & 0x3F);
    rs_psg(0x80 | (unsigned)((i * 2 + 1) << 4) | psg_vol[i]);
  }
  rs_psg(0x80 | (6 << 4) | psg_noise);
  rs_psg(0x80 | (7 << 4) | psg_vol[3]);

  /* the frame the writes were spaced through: run the chips to its end and discard what they made */
  sound_update(frame);
  audio_update(discard);
  return 1;
}

int vgm_active(void) { return vgm != NULL; }

int vgm_start(const char *path)
{
  if (!emu_running || vgm) return 0;
  vgm = fopen(path, "wb");
  if (!vgm) return 0;
  len = 0;
  total_samples = frame_pos = 0;
  spf = vdp_pal ? 882 : 735;
  frame_cycles = (unsigned)lines_per_frame * 3420u;
  fm_is_opll = ((system_hw & SYSTEM_PBC) != SYSTEM_MD) && (system_hw != SYSTEM_MCD);
  memset(fm_reg, 0, sizeof(fm_reg));
  { unsigned char hdr[0x40] = { 0 }; put(hdr, sizeof(hdr)); }
  if (fm_write != hook_fm) { real_fm = fm_write; fm_write = hook_fm; }
  return 1;
}

/* After each emulated frame: the rest of the frame passes silently, and the FM hook is put back if the core replaced it. */
void vgm_frame_end(void)
{
  vgm_attach();
  if (!vgm) return;
  wait_to(spf);
  frame_pos = 0;
  if (fm_write != hook_fm) { real_fm = fm_write; fm_write = hook_fm; }
  frame_cycles = (unsigned)lines_per_frame * 3420u;
  spf = vdp_pal ? 882 : 735;
}

void vgm_stop(void)
{
  unsigned char *h;
  unsigned v;

  if (!vgm) return;
  put8(0x66);
  h = buf;
  memcpy(h, "Vgm ", 4);
  v = (unsigned)len - 4;                memcpy(h + 0x04, &v, 4);
  v = 0x161;                            memcpy(h + 0x08, &v, 4);
  v = vdp_pal ? 3546893u : 3579545u;    memcpy(h + 0x0C, &v, 4);                 /* SN76489 clock */
  if (fm_is_opll) { v = 3579545u; memcpy(h + 0x10, &v, 4); }                      /* YM2413 clock */
  memcpy(h + 0x18, &total_samples, 4);
  v = vdp_pal ? 50 : 60;                memcpy(h + 0x24, &v, 4);                 /* rate */
  { unsigned short fb = 0x0009; h[0x28] = (unsigned char)fb; h[0x29] = (unsigned char)(fb >> 8); h[0x2A] = 16; }
  if (!fm_is_opll) { v = vdp_pal ? 7600489u : 7670453u; memcpy(h + 0x2C, &v, 4); } /* YM2612 clock */
  v = 0x0C;                             memcpy(h + 0x34, &v, 4);                 /* data starts at 0x40 */
  fwrite(buf, 1, len, vgm);
  fclose(vgm);
  vgm = NULL;
  free(buf);
  buf = NULL;
  len = cap = 0;
  gui_notify("Sound log saved");
}
