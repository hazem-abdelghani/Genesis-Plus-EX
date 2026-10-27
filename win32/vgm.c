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
#include "picocore.h"
#include "ym3438.h"
#include "ym2612.h"

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
static unsigned ev_count[4];               /* how many writes of each kind a 32X recording got (for the log) */
static unsigned pwm_last[2];                /* 32X PWM: the last left and right values logged (the chip holds a value: a repeat is left out) */
static int pwm_last_set[2];
static int vgm_32x;                         /* recording a 32X game: the library tells the writes (pico_event) */

/* A copy of what the game has told the sound chips, kept so that when the FM chip emulation is switched while a game runs
   (the new chip starts blank, and the game's music driver will not send its instrument set-up again) the registers can be
   written into it and the music carries on. Cleared when a game starts or resets. (After a state is loaded the copy only
   holds what was written since, which is still more than a blank chip; a recording that starts reads the FM chip back, below.) */
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
      {
        int q = (r >= 0xA0 && r < 0xB0) ? 0x14F - r : r;       /* a frequency's high part is written before its low part (the chip latches it) */
        if (ym_set[port][q]) { rs_fm((unsigned)(port * 2), (unsigned)q); rs_fm((unsigned)(port * 2 + 1), ym_reg[port][q]); }
      }
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

/* A 32X game: the library tells what is written to the FM chip, the PSG and the PWM, and where in the frame (0..65535). */
static void pico_event(int kind, unsigned a, unsigned d, unsigned frac)
{
  if (!vgm) return;
  if (kind >= 1 && kind <= 3) ev_count[kind]++;
  at_cycles(frac);
  if (kind == 1)
  {
    int port = (int)((a >> 1) & 1);

    if (!(a & 1)) fm_reg[port] = (unsigned char)d;
    else { put8(0x52 + port); put8(fm_reg[port]); put8((int)d); }
  }
  else if (kind == 2) { put8(0x50); put8((int)(d & 0xFF)); }
  else if (kind == 3)
  {
    if ((a != 2 && a != 3) || !pwm_last_set[a - 2] || pwm_last[a - 2] != d)
    {
      put8(0xB2); put8((int)((a << 4) | ((d >> 8) & 0x0F))); put8((int)(d & 0xFF));       /* PWM, register a, 12 bits */
      if (a == 2 || a == 3) { pwm_last[a - 2] = d; pwm_last_set[a - 2] = 1; }
    }
  }
}

int vgm_active(void) { return vgm != NULL; }

/* The Nuked FM chip (the default for a Mega Drive game) keeps every register it was given in its own state, which a loaded state
   restores without any write: the copy kept above does not know it. The chip's state is read from what the core saves (the chip itself
   is private to the core), and the copy is made from it. */
typedef char nuked_state_fits[(sizeof(ym3438_t) + 1024 <= 8192) ? 1 : -1];       /* (the buffer below holds the chip's state and what the core saves with it) */

static void shadow_from_nuked(void)
{
  static unsigned char sbuf[8192];
  static ym3438_t chip;
  int s, c, r;

  if (!config.ym3438 || (system_hw & SYSTEM_PBC) != SYSTEM_MD) return;
  memset(sbuf, 0, sizeof(sbuf));
  sound_context_save(sbuf);
  if (!sbuf[0]) return;                                  /* (the first byte: the Nuked chip is the one in use) */
  memcpy(&chip, sbuf + 1, sizeof(chip));

  for (s = 0; s < 24; s++)
  {
    int i = s % 12;                                      /* 0-2 and 6-8: channels 1-3 (port 0), 3-5 and 9-11: channels 4-6 (port 1) */
    int port = ((i % 6) >= 3) ? 1 : 0;
    int off = (i % 3) + ((i >= 6) ? 4 : 0) + ((s >= 12) ? 8 : 0);       /* the operator's place among the registers: channel, column of the operators */
    int mul = chip.multi[s];

    ym_reg[port][0x30 + off] = (unsigned char)((chip.dt[s] << 4) | ((mul <= 1) ? 0 : (mul >> 1)));
    ym_reg[port][0x40 + off] = chip.tl[s];
    ym_reg[port][0x50 + off] = (unsigned char)((chip.ks[s] << 6) | chip.ar[s]);
    ym_reg[port][0x60 + off] = (unsigned char)((chip.am[s] << 7) | chip.dr[s]);
    ym_reg[port][0x70 + off] = chip.sr[s];
    ym_reg[port][0x80 + off] = (unsigned char)(((chip.sl[s] & 0x0F) << 4) | (chip.rr[s] & 0x0F));
    ym_reg[port][0x90 + off] = chip.ssg_eg[s];
    for (r = 0x30; r < 0xA0; r += 0x10) ym_set[port][r + off] = 1;
  }
  for (c = 0; c < 6; c++)
  {
    int port = c / 3, cc = c % 3;

    ym_reg[port][0xA0 + cc] = (unsigned char)(chip.fnum[c] & 0xFF);
    ym_reg[port][0xA4 + cc] = (unsigned char)((chip.block[c] << 3) | (chip.fnum[c] >> 8));
    ym_reg[port][0xB0 + cc] = (unsigned char)((chip.fb[c] << 3) | chip.connect[c]);
    ym_reg[port][0xB4 + cc] = (unsigned char)((chip.pan_l[c] << 7) | (chip.pan_r[c] << 6) | (chip.ams[c] << 4) | chip.pms[c]);
    ym_set[port][0xA0 + cc] = ym_set[port][0xA4 + cc] = ym_set[port][0xB0 + cc] = ym_set[port][0xB4 + cc] = 1;
    if (c < 3)                                           /* the three frequencies of channel 3's special mode */
    {
      ym_reg[0][0xA8 + cc] = (unsigned char)(chip.fnum_3ch[c] & 0xFF);
      ym_reg[0][0xAC + cc] = (unsigned char)((chip.block_3ch[c] << 3) | (chip.fnum_3ch[c] >> 8));
      ym_set[0][0xA8 + cc] = ym_set[0][0xAC + cc] = 1;
    }
    ym_keyon[c < 3 ? c : c + 1] = (unsigned char)(chip.mode_kon[c] | (chip.mode_kon[c + 12] << 1) | (chip.mode_kon[c + 6] << 2) | (chip.mode_kon[c + 18] << 3));
  }
  ym_reg[0][0x22] = (unsigned char)((chip.lfo_en ? 8 : 0) | (chip.lfo_freq & 7));
  ym_reg[0][0x27] = (unsigned char)(chip.mode_ch3 << 6);
  ym_reg[0][0x2B] = (unsigned char)(chip.dacen << 7);
  ym_set[0][0x22] = ym_set[0][0x27] = ym_set[0][0x2B] = 1;
}

/* The MAME FM chips (YM2612 and YM3438 of the options) keep their state in a structure private to the core, which the core writes out as it is
   (followed by the detune table row of each operator). Its layout is copied here, and a recording that starts checks that what the core
   writes is exactly as long as the copy before using it: a core that changed would leave the registers as they were. */
typedef struct
{
  int *DT; unsigned char KSR; unsigned int ar, d1r, d2r, rr; unsigned char ksr; unsigned int mul;
  unsigned int phase; int Incr;
  unsigned char state; unsigned int tl; int volume; unsigned int sl; unsigned int vol_out;
  unsigned char eg_sh_ar, eg_sel_ar, eg_sh_d1r, eg_sel_d1r, eg_sh_d2r, eg_sel_d2r, eg_sh_rr, eg_sel_rr;
  unsigned char ssg, ssgn, key; unsigned int AMmask;
} mame_slot;

typedef struct
{
  mame_slot SLOT[4];
  unsigned char ALGO, FB; int op1_out[2];
  int *connect1, *connect3, *connect2, *connect4, *mem_connect; int mem_value;
  int pms; unsigned char ams;
  unsigned int fc; unsigned char kcode; unsigned int block_fnum;
} mame_channel;

typedef struct
{
  struct { unsigned short address; unsigned char status; unsigned int mode; unsigned char fn_h; int TA, TAL, TAC, TB, TBL, TBC; int dt_tab[8][32]; } ST;
  struct { unsigned int fc[3]; unsigned char fn_h; unsigned char kcode[3]; unsigned int block_fnum[3]; unsigned char key_csm; } SL3;
  unsigned int pan[12];
  unsigned int eg_cnt, eg_timer;
  unsigned char lfo_cnt; unsigned int lfo_timer, lfo_timer_overflow, LFO_AM, LFO_PM;
} mame_opn;

typedef struct { mame_channel CH[6]; unsigned char dacen; int dacout; mame_opn OPN; } mame_chip;

typedef char mame_state_fits[(sizeof(mame_chip) + 64 <= 8192) ? 1 : -1];

static void shadow_from_mame(void)
{
  static unsigned char sbuf[8192];
  static mame_chip chip;
  static const unsigned char lfo_steps[8] = { 108, 77, 71, 67, 62, 44, 8, 5 };       /* (lfo_samples_per_step) */
  static const unsigned char ams_shift[4] = { 8, 3, 1, 0 };
  int c, s, i, n;

  if (config.ym3438 || (system_hw & SYSTEM_PBC) != SYSTEM_MD) return;
  memset(sbuf, 0, sizeof(sbuf));
  n = YM2612SaveContext(sbuf);
  if (n != (int)(sizeof(chip) + 2 * 24)) return;                               /* (not the layout copied above; the core counts each of the 24 detune bytes twice) */
  memcpy(&chip, sbuf, sizeof(chip));

  for (c = 0; c < 6; c++)
  {
    const mame_channel *ch = &chip.CH[c];
    int port = c / 3, cc = c % 3, bf = (int)ch->block_fnum;

    for (s = 0; s < 4; s++)                                                    /* s: the operator's column among the registers (0, 4, 8, 12) */
    {
      const mame_slot *sl = &ch->SLOT[s];
      int o = (s << 2) + cc, dt = sbuf[sizeof(chip) + 2 * (c * 4 + s)] & 7;       /* (the core leaves a gap after each detune byte) */
      int mul = (int)sl->mul, ar = (int)sl->ar, d1r = (int)sl->d1r, d2r = (int)sl->d2r, rr = (int)sl->rr, sli = 0;

      while (sli < 15 && (int)sl->sl != sli * 32) sli++;                       /* sl_table: 3 dB steps, the last one 93 dB */
      ym_reg[port][0x30 + o] = (unsigned char)((dt << 4) | ((mul <= 1) ? 0 : ((mul >> 1) & 0x0F)));
      ym_reg[port][0x40 + o] = (unsigned char)((sl->tl >> 3) & 0x7F);
      ym_reg[port][0x50 + o] = (unsigned char)(((3 - sl->KSR) << 6) | (ar < 32 ? 0 : (((ar - 32) >> 1) & 0x1F)));
      ym_reg[port][0x60 + o] = (unsigned char)((sl->AMmask ? 0x80 : 0) | (d1r < 32 ? 0 : (((d1r - 32) >> 1) & 0x1F)));
      ym_reg[port][0x70 + o] = (unsigned char)(d2r < 32 ? 0 : (((d2r - 32) >> 1) & 0x1F));
      ym_reg[port][0x80 + o] = (unsigned char)((sli << 4) | (rr < 34 ? 0 : (((rr - 34) >> 2) & 0x0F)));
      ym_reg[port][0x90 + o] = (unsigned char)(sl->ssg & 0x0F);
      for (i = 0x30; i < 0xA0; i += 0x10) ym_set[port][i + o] = 1;
    }
    for (i = 0; i < 3 && ams_shift[i] != ch->ams; i++) ;
    ym_reg[port][0xA0 + cc] = (unsigned char)(bf & 0xFF);
    ym_reg[port][0xA4 + cc] = (unsigned char)(((bf >> 11) << 3) | ((bf >> 8) & 7));
    ym_reg[port][0xB0 + cc] = (unsigned char)(ch->ALGO | (((10 - ch->FB) & 7) << 3));
    ym_reg[port][0xB4 + cc] = (unsigned char)((chip.OPN.pan[c * 2] ? 0x80 : 0) | (chip.OPN.pan[c * 2 + 1] ? 0x40 : 0) | (i << 4) | ((ch->pms >> 5) & 7));
    ym_set[port][0xA0 + cc] = ym_set[port][0xA4 + cc] = ym_set[port][0xB0 + cc] = ym_set[port][0xB4 + cc] = 1;
    if (c < 3)                                                                 /* the three frequencies of channel 3's special mode */
    {
      int b3 = (int)chip.OPN.SL3.block_fnum[c];
      ym_reg[0][0xA8 + cc] = (unsigned char)(b3 & 0xFF);
      ym_reg[0][0xAC + cc] = (unsigned char)(((b3 >> 11) << 3) | ((b3 >> 8) & 7));
      ym_set[0][0xA8 + cc] = ym_set[0][0xAC + cc] = 1;
    }
    ym_keyon[c < 3 ? c : c + 1] = (unsigned char)((ch->SLOT[0].key ? 1 : 0) | (ch->SLOT[2].key ? 2 : 0) | (ch->SLOT[1].key ? 4 : 0) | (ch->SLOT[3].key ? 8 : 0));
  }
  ym_reg[0][0x22] = 0;
  if (chip.OPN.lfo_timer_overflow)
    for (i = 0; i < 8; i++)
      if (chip.OPN.lfo_timer_overflow == lfo_steps[i]) ym_reg[0][0x22] = (unsigned char)(8 | i);
  ym_reg[0][0x27] = (unsigned char)(chip.OPN.ST.mode & 0xC0);
  ym_reg[0][0x2B] = (unsigned char)(chip.dacen ? 0x80 : 0);
  ym_set[0][0x22] = ym_set[0][0x27] = ym_set[0][0x2B] = 1;
}

/* A recording that starts while a game plays begins with the state of the chips (a game loads its instruments once, when a song
   starts): the copy of what was written to them, as the writes of the first moment of the file. */
static void dump_state(void)
{
  int port, r, ch, i;

  if (fm_is_opll)
  {
    for (r = 0; r < 0x40; r++)
      if (ym_set[0][r]) { put8(0x51); put8(r); put8(ym_reg[0][r]); }
  }
  else
  {
    for (port = 0; port < 2; port++)
      for (r = 0x30; r < 0xC0; r++)
      {
        int q = (r >= 0xA0 && r < 0xB0) ? 0x14F - r : r;       /* a frequency's high part is written before its low part (the chip latches it) */
        if (ym_set[port][q]) { put8(0x52 + port); put8(q); put8(ym_reg[port][q]); }
      }
    for (r = 0x22; r < 0x30; r++)
      if (ym_set[0][r] && r != 0x27) { put8(0x52); put8(r); put8(ym_reg[0][r]); }
    if (ym_set[0][0x27]) { put8(0x52); put8(0x27); put8(ym_reg[0][0x27] & 0xCF); }          /* without the timers' reset strobes */
    for (ch = 0; ch < 8; ch++)
      if (ch != 3 && ch != 7 && ym_keyon[ch]) { put8(0x52); put8(0x28); put8((ym_keyon[ch] << 4) | ch); }
  }
  for (i = 0; i < 3; i++)
  {
    put8(0x50); put8(0x80 | ((i * 2) << 4) | (int)(psg_tone[i] & 0x0F));
    put8(0x50); put8((int)((psg_tone[i] >> 4) & 0x3F));
    put8(0x50); put8(0x80 | ((i * 2 + 1) << 4) | psg_vol[i]);
  }
  put8(0x50); put8(0x80 | (6 << 4) | psg_noise);
  put8(0x50); put8(0x80 | (7 << 4) | psg_vol[3]);
}

int vgm_start(const char *path)
{
  if (!emu_running || vgm) return 0;
  vgm = fopen(path, "wb");
  if (!vgm) return 0;
  len = 0;
  total_samples = frame_pos = 0;
  vgm_32x = pico_active();
  if (vgm_32x)
  {
    spf = pico_fps() < 55.0 ? 882 : 735;
    frame_cycles = 65536u;
    fm_is_opll = 0;
    memset(fm_reg, 0, sizeof(fm_reg));
    { unsigned char hdr[0x80] = { 0 }; put(hdr, sizeof(hdr)); }
    memset(ev_count, 0, sizeof(ev_count));
    pwm_last_set[0] = pwm_last_set[1] = 0;
    gui_log("VGM recording of a 32X game started (%s)", path);
    if (!pico_sound_log(pico_event)) { fclose(vgm); vgm = NULL; remove(path); free(buf); buf = NULL; len = cap = 0; vgm_32x = 0; return 0; }
    return 1;
  }
  spf = vdp_pal ? 882 : 735;
  frame_cycles = (unsigned)lines_per_frame * 3420u;
  fm_is_opll = ((system_hw & SYSTEM_PBC) != SYSTEM_MD) && (system_hw != SYSTEM_MCD);
  memset(fm_reg, 0, sizeof(fm_reg));
  { unsigned char hdr[0x40] = { 0 }; put(hdr, sizeof(hdr)); }
  if (fm_write != hook_fm) { real_fm = fm_write; fm_write = hook_fm; }
  shadow_from_nuked();
  shadow_from_mame();
  dump_state();
  return 1;
}

/* After each emulated frame: the rest of the frame passes silently, and the FM hook is put back if the core replaced it. */
void vgm_frame_end(void)
{
  if (!vgm_32x) vgm_attach();
  if (!vgm) return;
  wait_to(spf);
  frame_pos = 0;
  if (vgm_32x) { spf = pico_fps() < 55.0 ? 882 : 735; return; }
  if (fm_write != hook_fm) { real_fm = fm_write; fm_write = hook_fm; }
  frame_cycles = (unsigned)lines_per_frame * 3420u;
  spf = vdp_pal ? 882 : 735;
}

/* The end of the file: a player that reaches it leaves the chips as the log left them, so a note still held rings on ("toooon"), and one
   that was released with a slow release still fades out over seconds. Every note is released, the operators are turned down to their
   quietest (an instant silence, whatever their release is) and the PSG is turned down first. */
static void silence_chips(void)
{
  int ch, i, port, o;

  if (fm_is_opll)
  {
    for (ch = 0; ch < 9; ch++)
      if (ym_set[0][0x20 + ch]) { put8(0x51); put8(0x20 + ch); put8(ym_reg[0][0x20 + ch] & ~0x10); }          /* the key bit off */
    if (ym_set[0][0x0E]) { put8(0x51); put8(0x0E); put8(ym_reg[0][0x0E] & 0xE0); }                           /* the drum keys off */
    for (ch = 0; ch < 9; ch++) { put8(0x51); put8(0x30 + ch); put8((ym_reg[0][0x30 + ch] & 0xF0) | 0x0F); }   /* volume 15: the quietest */
  }
  else
  {
    for (ch = 0; ch < 8; ch++)
      if (ch != 3 && ch != 7) { put8(0x52); put8(0x28); put8(ch); }                                          /* all four operators off */
    for (port = 0; port < 2; port++)                                                                         /* total level 127 on every operator */
      for (o = 0; o < 16; o++)
        if ((o & 3) != 3) { put8(0x52 + port); put8(0x40 + o); put8(0x7F); }
  }
  for (i = 0; i < 4; i++) { put8(0x50); put8(0x9F | (i << 5)); }                                             /* PSG volume 15 (off) on the four channels */
}

void vgm_stop(void)
{
  unsigned char *h;
  unsigned v;

  if (!vgm) return;
  if (vgm_32x)
  {
    pico_sound_log(NULL);
    gui_log("VGM recording of a 32X game ended: %u FM writes, %u PSG writes, %u PWM writes, %u samples (%u s)", ev_count[1], ev_count[2], ev_count[3],
            total_samples, total_samples / 44100u);
  }
  silence_chips();
  put8(0x66);
  h = buf;
  memcpy(h, "Vgm ", 4);
  v = (unsigned)len - 4;                memcpy(h + 0x04, &v, 4);
  v = vgm_32x ? 0x171 : 0x161;          memcpy(h + 0x08, &v, 4);                 /* 1.71 has the 32X PWM */
  v = (vgm_32x ? spf == 882 : vdp_pal) ? 3546893u : 3579545u;    memcpy(h + 0x0C, &v, 4);   /* SN76489 clock */
  if (fm_is_opll) { v = 3579545u; memcpy(h + 0x10, &v, 4); }                      /* YM2413 clock */
  memcpy(h + 0x18, &total_samples, 4);
  v = (vgm_32x ? spf == 882 : vdp_pal) ? 50 : 60;                memcpy(h + 0x24, &v, 4);   /* rate */
  { unsigned short fb = 0x0009; h[0x28] = (unsigned char)fb; h[0x29] = (unsigned char)(fb >> 8); h[0x2A] = 16; }
  if (!fm_is_opll) { v = (vgm_32x ? spf == 882 : vdp_pal) ? 7600489u : 7670453u; memcpy(h + 0x2C, &v, 4); } /* YM2612 clock */
  if (vgm_32x) { v = spf == 882 ? 22801467u : 23011361u; memcpy(h + 0x70, &v, 4); }       /* PWM clock */
  v = vgm_32x ? 0x4C : 0x0C;            memcpy(h + 0x34, &v, 4);                 /* data starts at 0x80 (0x40 for the others) */
  fwrite(buf, 1, len, vgm);
  fclose(vgm);
  vgm = NULL;
  vgm_32x = 0;
  free(buf);
  buf = NULL;
  len = cap = 0;
  gui_notify("Sound log saved");
}
