/*
 * PicoDrive
 * (C) notaz, 2009,2010,2013
 * (C) irixxxx, 2019-2023
 *
 * This work is licensed under the terms of MAME license.
 * See COPYING file in the top-level directory.
 */
#include "../pico_int.h"

static struct {
  int cycles;
  unsigned mult;
  int ptr;
  int irq_reload;
  int doing_fifo;
  int silent;
  int irq_timer;
  int irq_state;
  short current[2];
} pwm;

enum { PWM_IRQ_LOCKED, PWM_IRQ_STOPPED, PWM_IRQ_LOW, PWM_IRQ_HIGH };

/* Genesis Plus EX: where in the frame (0..65535) a moment of the 68000 clock is, for the VGM recording. The clock counts on from
 * frame to frame; the frame began at m68c_frame_start. */
static inline unsigned gpex_pwm_frac(int m68k_cycles)
{
  return gpex_frac(m68k_cycles - (int)Pico.t.m68c_frame_start, GPEX_LINES * 488);
}

/* Genesis Plus EX: removing the start-up pop.
 *
 * A game often switches the PWM on with the value 0 in its FIFO (zero duty, the lowest output level of the converted
 * range) and keeps it there until it has music. The output filter in mix.c (a DC filter) then sees a step of about
 * -32768 and answers with a loud thump that takes a few frames to die away, and again a smaller one when the music starts
 * at another level. While the machine is new (the first DECLICK_SECONDS after power on or a reset, not after loading a
 * state) a jump from one PWM sample to the next of more than DECLICK_STEP, after the output had been flat for at least
 * DECLICK_FLAT samples (so it is not an edge of a tone), is taken as a change of the level and not as sound: the step is
 * taken out and the level comes back over a second or so, far slower than the output filter works, so it is not heard. */
#define DECLICK_STEP    12000
#define DECLICK_FLAT    48
#define DECLICK_SECONDS 10
#define DECLICK_MIXSTEP 20000   /* the same for the sum of everything that is played, which is the last thing before the output filter: no music jumps that far in one sample, so it does not wait for stillness */

struct declick {
  int step;                     /* the size of a jump that is taken as a change of the level */
  int need;                     /* how many still samples it needs before that */
  int prev[2];                  /* the previous sample of each channel */
  int flat[2];                  /* how many samples in a row it has hardly moved */
  int off[2];                   /* the level that has been taken out, times 256 */
};

static int declick_left;        /* output samples left in the start-up window */
static struct declick dk_pwm = { DECLICK_STEP, DECLICK_FLAT }, dk_mix = { DECLICK_MIXSTEP, 0 };

static void declick_init(struct declick *dk)
{
  dk->prev[0] = dk->prev[1] = 0;
  dk->flat[0] = dk->flat[1] = DECLICK_FLAT;       /* everything has been still since power on */
  dk->off[0] = dk->off[1] = 0;
}

void p32x_pwm_declick_reset(void)
{
  declick_left = DECLICK_SECONDS * (PicoIn.sndRate > 0 ? PicoIn.sndRate : 44100);
  declick_init(&dk_pwm);
  declick_init(&dk_mix);
}

static inline int declick(struct declick *dk, int ch, int s)
{
  if (declick_left > 0 || dk->off[ch])
  {
    int d = s - dk->prev[ch];

    dk->prev[ch] = s;
    if (declick_left > 0 && (d > dk->step || d < -dk->step)) {
      if (dk->flat[ch] >= dk->need)
        dk->off[ch] += d * 256;
    }
    if (d > -64 && d < 64) { if (dk->flat[ch] < DECLICK_FLAT) dk->flat[ch]++; }
    else dk->flat[ch] = 0;
    dk->off[ch] -= dk->off[ch] >> 16;       /* the level comes back over a second or so: far slower than the output filter works, so it is not heard */
    return s - (dk->off[ch] >> 8);
  }
  return s;
}

/* the same on the sum of everything that is played (FM, PSG, PWM), run on a block of the mix buffer before it is limited
 * and filtered; the PWM one above is the first line, this one catches a step that got past it */
void p32x_pwm_declick_mix(s32 *buf32, int length, int stereo)
{
  int i;

  if (declick_left <= 0 && !dk_mix.off[0] && !dk_mix.off[1])
    return;
  for (i = 0; i < length; i++) {
    if (stereo) {
      buf32[i * 2    ] = declick(&dk_mix, 0, buf32[i * 2    ]);
      buf32[i * 2 + 1] = declick(&dk_mix, 1, buf32[i * 2 + 1]);
    } else
      buf32[i] = declick(&dk_mix, 0, buf32[i]);
  }
  if (declick_left > 0)
    declick_left -= length;           /* the start-up window of the pop remover runs in output samples */
}

void p32x_pwm_ctl_changed(void)
{
  int control = Pico32x.regs[0x30 / 2];
  int cycles = Pico32x.regs[0x32 / 2];
  int pwm_irq_opt = PicoIn.opt & POPT_PWM_IRQ_OPT;

  cycles = (cycles - 1) & 0x0fff;
  pwm.cycles = cycles;

  // supposedly we should stop FIFO when xMd is 0,
  // but mars test disagrees
  pwm.mult = 0;
  if ((control & 0x0f) != 0)
    pwm.mult = (0x10000<<8) / (cycles+1);

  pwm.irq_timer = (control & 0x0f00) >> 8;
  pwm.irq_timer = ((pwm.irq_timer - 1) & 0x0f) + 1;
  pwm.irq_reload = pwm.irq_timer;
  pwm.irq_state = pwm_irq_opt ? PWM_IRQ_STOPPED: PWM_IRQ_LOCKED;

  if (Pico32x.pwm_irq_cnt <= 0)
    Pico32x.pwm_irq_cnt = pwm.irq_reload;
}

static void do_pwm_irq(SH2 *sh2, unsigned int m68k_cycles)
{
  p32x_trigger_irq(NULL, m68k_cycles, P32XI_PWM);

  if (Pico32x.regs[0x30 / 2] & P32XP_RTP) {
    p32x_event_schedule(m68k_cycles, P32X_EVENT_PWM, pwm.cycles / 3 + 1);
    // note: might recurse
    p32x_dreq1_trigger();
  }
}

static int convert_sample(unsigned int v)
{
  if (v > pwm.cycles)
    v = pwm.cycles;
  return (v * pwm.mult >> 8) - 0x10000/2;
}

#define consume_fifo(sh2, m68k_cycles) { \
  int cycles_diff = ((m68k_cycles) * 3) - Pico32x.pwm_cycle_p; \
  if (cycles_diff >= pwm.cycles) \
    consume_fifo_do(sh2, m68k_cycles, cycles_diff); \
}

static void consume_fifo_do(SH2 *sh2, unsigned int m68k_cycles,
  int sh2_cycles_diff)
{
  struct Pico32xMem *mem = Pico32xMem;
  unsigned short *fifo_l = mem->pwm_fifo[0];
  unsigned short *fifo_r = mem->pwm_fifo[1];
  int sum = 0;
  unsigned int tick = Pico32x.pwm_cycle_p;      /* where the chip is, in 32X cycles (for the VGM recording) */

  if (pwm.cycles == 0 || pwm.doing_fifo)
    return;

  elprintf(EL_PWM, "pwm: %u: consume %d/%d, %d,%d ptr %d",
    m68k_cycles, sh2_cycles_diff, sh2_cycles_diff / pwm.cycles,
    Pico32x.pwm_p[0], Pico32x.pwm_p[1], pwm.ptr);

  // this is for recursion from dreq1 writes
  pwm.doing_fifo = 1;

  while (sh2_cycles_diff >= pwm.cycles)
  {
    sh2_cycles_diff -= pwm.cycles;
    tick += pwm.cycles;

    if (Pico32x.pwm_p[0] > 0) {
      mem->pwm_index[0] = (mem->pwm_index[0]+1) % 4;
      Pico32x.pwm_p[0]--;
      pwm.current[0] = convert_sample(fifo_l[mem->pwm_index[0]]);
      sum |= (u16)pwm.current[0];
      /* Genesis Plus EX: the VGM recording gets what the chip plays, at the time it plays it (the games write in bursts) */
      if (gpex_snd_log)
        gpex_snd_log(3, 2, (fifo_l[mem->pwm_index[0]] + 1) & 0x0fff, gpex_pwm_frac((int)(tick / 3)));
    }
    if (Pico32x.pwm_p[1] > 0) {
      mem->pwm_index[1] = (mem->pwm_index[1]+1) % 4;
      Pico32x.pwm_p[1]--;
      pwm.current[1] = convert_sample(fifo_r[mem->pwm_index[1]]);
      sum |= (u16)pwm.current[1];
      if (gpex_snd_log)
        gpex_snd_log(3, 3, (fifo_r[mem->pwm_index[1]] + 1) & 0x0fff, gpex_pwm_frac((int)(tick / 3)));
    }

    mem->pwm[pwm.ptr * 2    ] = pwm.current[0];
    mem->pwm[pwm.ptr * 2 + 1] = pwm.current[1];
    pwm.ptr = (pwm.ptr + 1) & (PWM_BUFF_LEN - 1);

    if (--Pico32x.pwm_irq_cnt <= 0) {
      Pico32x.pwm_irq_cnt = pwm.irq_reload;
      do_pwm_irq(sh2, m68k_cycles);
    } else if (Pico32x.pwm_p[1] == 0 && pwm.irq_state >= PWM_IRQ_LOW) {
      // buffer underrun. Reduce reload rate if above programmed setting.
      if (pwm.irq_reload > pwm.irq_timer)
        pwm.irq_reload--;
      pwm.irq_state = PWM_IRQ_LOW;
    }
  }
  Pico32x.pwm_cycle_p = m68k_cycles * 3 - sh2_cycles_diff;
  pwm.doing_fifo = 0;
  if (sum != 0)
    pwm.silent = 0;
}

static int p32x_pwm_schedule_(SH2 *sh2, unsigned int m68k_now)
{
  unsigned int pwm_now = m68k_now * 3;
  int cycles_diff_sh2;

  if (pwm.cycles == 0)
    return 0;

  cycles_diff_sh2 = pwm_now - Pico32x.pwm_cycle_p;
  if (cycles_diff_sh2 >= pwm.cycles)
    consume_fifo_do(sh2, m68k_now, cycles_diff_sh2);

  if (!((Pico32x.sh2irq_mask[0] | Pico32x.sh2irq_mask[1]) & 1))
    return 0; // masked by everyone

  cycles_diff_sh2 = pwm_now - Pico32x.pwm_cycle_p;
  return (Pico32x.pwm_irq_cnt * pwm.cycles
           - cycles_diff_sh2) / 3 + 1;
}

void p32x_pwm_schedule(unsigned int m68k_now)
{
  int after = p32x_pwm_schedule_(NULL, m68k_now);
  if (after != 0)
    p32x_event_schedule(m68k_now, P32X_EVENT_PWM, after);
}

void p32x_pwm_schedule_sh2(SH2 *sh2)
{
  int after = p32x_pwm_schedule_(sh2, sh2_cycles_done_m68k(sh2));
  if (after != 0)
    p32x_event_schedule_sh2(sh2, P32X_EVENT_PWM, after);
}

void p32x_pwm_sync_to_sh2(SH2 *sh2)
{
  int m68k_cycles = sh2_cycles_done_m68k(sh2);
  consume_fifo(sh2, m68k_cycles);
}

void p32x_pwm_irq_event(unsigned int m68k_now)
{
  p32x_pwm_schedule(m68k_now);
}

unsigned int p32x_pwm_read16(u32 a, SH2 *sh2, unsigned int m68k_cycles)
{
  unsigned int d = 0;

  consume_fifo(sh2, m68k_cycles);

  a &= 0x0e;
  switch (a/2) {
    case 0/2: // control
    case 2/2: // cycle
      d = Pico32x.regs[(0x30 + a) / 2];
      break;

    case 4/2: // L ch
      if (Pico32x.pwm_p[0] == 3)
        d |= P32XP_FULL;
      else if (Pico32x.pwm_p[0] == 0)
        d |= P32XP_EMPTY;
      break;

    case 6/2: // R ch
    case 8/2: // MONO
      if (Pico32x.pwm_p[1] == 3)
        d |= P32XP_FULL;
      else if (Pico32x.pwm_p[1] == 0)
        d |= P32XP_EMPTY;
      break;
  }

  elprintf(EL_PWM, "pwm: %u: r16 %02x %04x (p %d %d)",
    m68k_cycles, a, d, Pico32x.pwm_p[0], Pico32x.pwm_p[1]);
  return d;
}

void p32x_pwm_write16(u32 a, unsigned int d, SH2 *sh2, unsigned int m68k_cycles)
{
  unsigned short *fifo;
  int idx;

  elprintf(EL_PWM, "pwm: %u: w16 %02x %04x (p %d %d)",
    m68k_cycles, a & 0x0e, d, Pico32x.pwm_p[0], Pico32x.pwm_p[1]);

  consume_fifo(sh2, m68k_cycles);

  if (gpex_snd_log && (a & 0x0e) <= 2)       /* Genesis Plus EX: the VGM recording (the control and cycle registers; the sample values are logged as the chip plays them) */
    gpex_snd_log(3, (a & 0x0e) / 2, d & 0x0fff, gpex_pwm_frac((int)m68k_cycles));

  a &= 0x0e;
  switch (a/2) {
    case 0/2: // control
      // avoiding pops..
      if ((Pico32x.regs[0x30 / 2] & 0x0f) == 0)
        Pico32xMem->pwm_fifo[0][0] = Pico32xMem->pwm_fifo[1][0] = 0;
      Pico32x.regs[0x30 / 2] = d;
      p32x_pwm_ctl_changed();
      Pico32x.pwm_irq_cnt = pwm.irq_reload; // ?
      break;
    case 2/2: // cycle
      Pico32x.regs[0x32 / 2] = d & 0x0fff;
      p32x_pwm_ctl_changed();
      break;
    case 8/2: // MONO
    case 6/2: // R ch
      fifo = Pico32xMem->pwm_fifo[1];
      idx = Pico32xMem->pwm_index[1];
      if (Pico32x.pwm_p[1] < 3) {
        if (Pico32x.pwm_p[1] == 2 && pwm.irq_state >= PWM_IRQ_STOPPED) {
          // buffer full. If there was no buffer underrun after last fill,
          // try increasing reload rate to reduce IRQs
          if (pwm.irq_reload < 3 && pwm.irq_state == PWM_IRQ_HIGH)
            pwm.irq_reload ++;
          pwm.irq_state = PWM_IRQ_HIGH;
        }
        Pico32x.pwm_p[1]++;
      } else {
        // buffer overflow. Some roms always fill the complete buffer even if
        // reload rate is set below max. Lock reload rate to programmed setting.
        pwm.irq_reload = pwm.irq_timer;
        pwm.irq_state = PWM_IRQ_LOCKED;
        idx = (idx+1) % 4;
        Pico32xMem->pwm_index[1] = idx;
      }
      fifo[(idx+Pico32x.pwm_p[1]) % 4] = (d - 1) & 0x0fff;
      if (a != 8) break; // fallthrough if MONO
    case 4/2: // L ch
      fifo = Pico32xMem->pwm_fifo[0];
      idx = Pico32xMem->pwm_index[0];
      if (Pico32x.pwm_p[0] < 3)
        Pico32x.pwm_p[0]++;
      else {
        idx = (idx+1) % 4;
        Pico32xMem->pwm_index[0] = idx;
      }
      fifo[(idx+Pico32x.pwm_p[0]) % 4] = (d - 1) & 0x0fff;
      break;
  }
}

void p32x_pwm_update(s32 *buf32, int length, int stereo)
{
  short *pwmb;
  int step;
  int p = 0;
  int xmd;

  consume_fifo(NULL, SekCyclesDone());

  xmd = Pico32x.regs[0x30 / 2] & 0x0f;
  if (xmd == 0 || xmd == 0x06 || xmd == 0x09 || xmd == 0x0f)
    goto out; // invalid?
  if (pwm.silent)
  {
    dk_pwm.flat[0] = dk_pwm.flat[1] = DECLICK_FLAT;       /* nothing was playing: still */
    dk_pwm.prev[0] = dk_pwm.prev[1] = 0;
    return;
  }

  step = (pwm.ptr << 16) / length;
  pwmb = Pico32xMem->pwm;

  if (stereo)
  {
    if (xmd == 0x05) {
      // normal
      while (length-- > 0) {
        *buf32++ += declick(&dk_pwm, 0, pwmb[0]);
        *buf32++ += declick(&dk_pwm, 1, pwmb[1]);

        p += step;
        pwmb += (p >> 16) * 2;
        p &= 0xffff;
      }
    }
    else if (xmd == 0x0a) {
      // channel swap
      while (length-- > 0) {
        *buf32++ += declick(&dk_pwm, 1, pwmb[1]);
        *buf32++ += declick(&dk_pwm, 0, pwmb[0]);

        p += step;
        pwmb += (p >> 16) * 2;
        p &= 0xffff;
      }
    }
    else {
      // mono - LMD, RMD specify dst
      int src_ch = (xmd & 0x06) ? 1 : 0;
      if (xmd & 0x06) // src is R
        pwmb++;
      if (xmd & 0x0c) // dst is R
        buf32++;
      while (length-- > 0) {
        *buf32 += declick(&dk_pwm, src_ch, *pwmb);

        p += step;
        pwmb += (p >> 16) * 2;
        p &= 0xffff;
        buf32 += 2;
      }
    }
  }
  else
  {
    // mostly unused
    while (length-- > 0) {
      *buf32++ += pwmb[0];

      p += step;
      pwmb += (p >> 16) * 2;
      p &= 0xffff;
    }
  }

  elprintf(EL_PWM, "pwm_update: pwm.ptr %d, len %d, step %04x, done %d",
    pwm.ptr, length, step, (pwmb - Pico32xMem->pwm) / 2);

out:
  pwm.ptr = 0;
  pwm.silent = pwm.current[0] == 0 && pwm.current[1] == 0;
}

void p32x_pwm_state_loaded(void)
{
  int cycles_diff_sh2;

  p32x_pwm_ctl_changed();

  // for old savestates
  cycles_diff_sh2 = Pico.t.m68c_cnt * 3 - Pico32x.pwm_cycle_p;
  if (cycles_diff_sh2 >= pwm.cycles || cycles_diff_sh2 < 0) {
    Pico32x.pwm_irq_cnt = pwm.irq_reload;
    Pico32x.pwm_cycle_p = Pico.t.m68c_cnt * 3;
    p32x_pwm_schedule(Pico.t.m68c_cnt);
  }
}

// vim:shiftwidth=2:ts=2:expandtab
