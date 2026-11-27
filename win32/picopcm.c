/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  picopcm.c -- the voices of Sega Pico games.
 *
 *  The Pico has a speech chip (a uPD7759 in slave mode: ADPCM nibbles fed through a 64 byte FIFO at $800010, a control
 *  register at $800012, and an interrupt when the FIFO runs low). The Genesis Plus GX core leaves it out ("ADPCM (TODO)"), so a
 *  Pico game plays its music but not its voices. This file is the chip, made from the ADPCM part of PicoDrive (pico/pico/xpcm.c
 *  and the register part of pico/pico/memory.c, by notaz and irixxxx; the ADPCM steps were derived from MAME's upd7759 driver,
 *  MAME license), put on to the core from outside: the register handlers at $800000 are replaced here, the chip is run every
 *  8 lines of the picture (render_line is wrapped, see deint.c) and its sound is added to the frame's sound after the core made it.
 *  The core is not changed.
 ****************************************************************************/

#include <math.h>
#include <string.h>
#include "shared.h"
#include "gui.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_SQRT2
#define M_SQRT2 1.41421356237309504880
#endif

#define Limit(val, max, min) ((val) > (max) ? (max) : (val) < (min) ? (min) : (val))

#define ADPCM_CLOCK         (1280000 / 4)
#define FIFO_IRQ_THRESHOLD  16
#define FIFO_SIZE           64
#define MIX_MAX             4096

static const int step_deltas[16][16] =
{
  { 0,  0,  1,  2,  3,   5,   7,  10,  0,   0,  -1,  -2,  -3,   -5,   -7,  -10 },
  { 0,  1,  2,  3,  4,   6,   8,  13,  0,  -1,  -2,  -3,  -4,   -6,   -8,  -13 },
  { 0,  1,  2,  4,  5,   7,  10,  15,  0,  -1,  -2,  -4,  -5,   -7,  -10,  -15 },
  { 0,  1,  3,  4,  6,   9,  13,  19,  0,  -1,  -3,  -4,  -6,   -9,  -13,  -19 },
  { 0,  2,  3,  5,  8,  11,  15,  23,  0,  -2,  -3,  -5,  -8,  -11,  -15,  -23 },
  { 0,  2,  4,  7, 10,  14,  19,  29,  0,  -2,  -4,  -7, -10,  -14,  -19,  -29 },
  { 0,  3,  5,  8, 12,  16,  22,  33,  0,  -3,  -5,  -8, -12,  -16,  -22,  -33 },
  { 1,  4,  7, 10, 15,  20,  29,  43, -1,  -4,  -7, -10, -15,  -20,  -29,  -43 },
  { 1,  4,  8, 13, 18,  25,  35,  53, -1,  -4,  -8, -13, -18,  -25,  -35,  -53 },
  { 1,  6, 10, 16, 22,  31,  43,  64, -1,  -6, -10, -16, -22,  -31,  -43,  -64 },
  { 2,  7, 12, 19, 27,  37,  51,  76, -2,  -7, -12, -19, -27,  -37,  -51,  -76 },
  { 2,  9, 16, 24, 34,  46,  64,  96, -2,  -9, -16, -24, -34,  -46,  -64,  -96 },
  { 3, 11, 19, 29, 41,  57,  79, 117, -3, -11, -19, -29, -41,  -57,  -79, -117 },
  { 4, 13, 24, 36, 50,  69,  96, 143, -4, -13, -24, -36, -50,  -69,  -96, -143 },
  { 4, 16, 29, 44, 62,  85, 118, 175, -4, -16, -29, -44, -62,  -85, -118, -175 },
  { 6, 20, 36, 54, 76, 104, 144, 214, -6, -20, -36, -54, -76, -104, -144, -214 },
};
static const int state_deltas[16] = { -1, -1, 0, 0, 1, 2, 2, 3, -1, -1, 0, 0, 1, 2, 2, 3 };

enum { RESET, START, HDR, COUNT };       /* portstate */

static struct
{
  int   samplepos;       /* leftover duration for the current sample, Q16 (host samples) */
  int   sample;          /* current sample */
  short state;           /* ADPCM decoder state */
  short samplegain;      /* programmable gain */
  char  startpin, irqenable, portstate;
  short silence;         /* silence blocks still to be played */
  short rate, nibbles;   /* ADPCM nibbles still to be played */
  unsigned char highlow, cache;
  char  filter;
  int   x[3], y[3];      /* filter history */
} xp;

static struct { int a[2], gain; } filters[4];
static int  filter_on;                     /* the filter in use is filters[xp.filter] (0 = none) */
static int  stepsamples;                   /* host sound rate / chip clock, Q16 */
static int  host_rate;                     /* the rate the filters and the step were made for */

static unsigned char fifo[FIFO_SIZE + 4];
static unsigned char *fifo_ptr = fifo;
static int  fifo_bytes;
static unsigned r12;

static int  attached;                      /* the register handlers are in (a Sega Pico game is running) */
static short mix[MIX_MAX];                 /* the voice of this frame, mono */
static int  mixn;                          /* how many samples of it are made */

/* The original handlers of $800000 - $80FFFF, for the registers that are not the speech chip's. */
static unsigned int (*orig_read8)(unsigned int);
static unsigned int (*orig_read16)(unsigned int);

#define QB 16
#define FP(f) (int)((f) * (1 << QB))

static void filter_coeff(int idx, int cutoff, int rate)
{
  double a, axa, gain;

  if (cutoff >= rate / 2) { memset(&filters[idx], 0, sizeof(filters[idx])); return; }
  a = 1.0 / tan(M_PI * cutoff / rate);
  axa = a * a;
  gain = 1.0 / (1.0 + M_SQRT2 * a + axa);
  filters[idx].gain = FP(gain);
  filters[idx].a[0] = FP(2.0 * (axa - 1.0) * gain);
  filters[idx].a[1] = FP(-(1.0 - M_SQRT2 * a + axa) * gain);
}

static void set_filter(int index)
{
  if (index != xp.filter) { memset(xp.x, 0, sizeof(xp.x)); memset(xp.y, 0, sizeof(xp.y)); }
  xp.filter = (char)index;
  filter_on = (index > 0 && index < 4 && filters[index].a[0] != 0);
}

static int apply_filter(int sample)
{
  const int *a;
  int gain;

  if (!filter_on) return sample;
  a = filters[(int)xp.filter].a;
  gain = filters[(int)xp.filter].gain;
  xp.x[0] = xp.x[1]; xp.x[1] = xp.x[2];
  xp.x[2] = sample * gain;
  xp.y[0] = xp.y[1]; xp.y[1] = xp.y[2];
  xp.y[2] = (xp.x[0] + 2 * xp.x[1] + xp.x[2] + xp.y[0] * a[1] + xp.y[1] * a[0]) >> QB;
  return xp.y[2];
}

/* The filters and the step are made for the sound rate in use. */
static void rerate(void)
{
  int rate = snd.sample_rate > 0 ? (int)snd.sample_rate : 44100;
  int next = (int)(((long long)rate << 16) / ADPCM_CLOCK);

  if (rate == host_rate) return;
  host_rate = rate;
  if (stepsamples != next) { memset(xp.x, 0, sizeof(xp.x)); memset(xp.y, 0, sizeof(xp.y)); }
  stepsamples = next;
  filter_coeff(1, 6000 / 2, rate);
  filter_coeff(2, 9000 / 2, rate);
  filter_coeff(3, 15000 / 2, rate);
  filter_on = 0;
  { int f = xp.filter; xp.filter = -1; set_filter(f); }
}

static void ppcm_reset_n(int pin)
{
  if (!pin) { xp.portstate = RESET; xp.sample = xp.samplepos = xp.state = 0; xp.nibbles = xp.silence = 0; }
  else if (xp.portstate == RESET) xp.portstate = START;
}

static int busy_n(void) { return xp.portstate <= START; }

#define do_sample(nibble) \
{ \
  xp.sample += step_deltas[xp.state][nibble]; \
  xp.state += state_deltas[nibble]; \
  xp.state = (xp.state < 0 ? 0 : xp.state > 15 ? 15 : xp.state); \
}

#define write_sample(buffer, length) \
{ \
  while (xp.samplepos > 0 && length > 0) { \
    int val = Limit(xp.samplegain * xp.sample, 16383, -16384); \
    xp.samplepos -= 1 << 16; \
    length--; \
    *buffer++ += (short)Limit(apply_filter(val), 32767, -32768); \
  } \
}

/* Makes `length` samples of the voice into buffer (added to what is there); raises the interrupt when the FIFO has run low. */
static void ppcm_update(short *buffer, int length)
{
  unsigned char *src = fifo;
  unsigned char *lim = fifo_ptr;
  int srcval, irq = 0;

  write_sample(buffer, length);

  while (length > 0 && src < lim)
  {
    if (xp.silence > 0)
    {
      xp.silence--;
      xp.sample = 0;
      xp.samplepos += stepsamples * 256;
    }
    else if (xp.nibbles > 0)
    {
      xp.nibbles--;
      if (xp.highlow) xp.cache = *src++;
      else            xp.cache <<= 4;
      xp.highlow = !xp.highlow;
      do_sample((xp.cache & 0xf0) >> 4);
      xp.samplepos += stepsamples * xp.rate;
    }
    else switch (xp.portstate)
    {
      case RESET:
        xp.sample = 0;
        xp.samplepos += length << 16;
        break;
      case START:
        if (xp.startpin)
        {
          if (*src) xp.portstate++;
          else      src++;                          /* 0x00 bytes at the start of a stream are dropped */
        }
        else { xp.sample = 0; xp.samplepos += length << 16; }
        break;
      case HDR:
        srcval = *src++;
        xp.nibbles = xp.silence = xp.rate = 0;
        xp.highlow = 1;
        if (srcval == 0)                              /* terminator */
        {
          if (lim - src == 1) src++;                  /* (a left over odd byte: Minna de Odorou) */
          xp.portstate = START;
        }
        else switch (srcval >> 6)
        {
          case 0: xp.silence = (short)((srcval & 0x3f) + 1); break;
          case 1: xp.rate = (short)((srcval & 0x3f) + 1); xp.nibbles = 256; break;
          case 2: xp.rate = (short)((srcval & 0x3f) + 1); xp.portstate = COUNT; break;
          case 3: break;
        }
        break;
      case COUNT:
        xp.nibbles = (short)(*src++ + 1);
        xp.portstate = HDR;
        break;
    }
    write_sample(buffer, length);
  }

  if (src < lim && src != fifo)
  {
    int di = (int)(lim - src);
    memmove(fifo, src, (size_t)di);
    fifo_ptr = fifo + di;
    if (!irq && di < FIFO_IRQ_THRESHOLD) irq = xp.irqenable;
    fifo_bytes = di;
  }
  else if (src == lim && src != fifo)
  {
    fifo_ptr = fifo;
    if (!irq) irq = xp.irqenable;
    fifo_bytes = 0;
  }

  if (irq && ((m68k.int_level >> 8) < (unsigned)irq)) m68k_set_irq((unsigned)irq);      /* the FIFO wants more */

  if (buffer && length > 0)                           /* underflow: the last sample is held, to avoid a click */
  {
    int val = Limit(xp.samplegain * xp.sample, 16383, -16384);
    while (length--) *buffer++ += (short)Limit(apply_filter(val), 32767, -32768);
  }
}

/* The voice made so far this frame is brought to `target` samples. */
static void advance(int target)
{
  int n;

  if (target > MIX_MAX) target = MIX_MAX;
  n = target - mixn;
  if (n <= 0) return;
  ppcm_update(mix + mixn, n);
  mixn = target;
}

/* How many samples of the frame's voice are due when the picture is at the end of `line`. */
static int target_for_line(int line)
{
  double fps = emu_frame_rate();

  if (fps < 1.0 || lines_per_frame < 100) return mixn;
  return (int)((double)snd.sample_rate / fps * (double)(line + 1) / (double)lines_per_frame);
}

/* ---- the registers ($800010: data / room in the FIFO, $800012: control / status) ---- */

static unsigned int reg_read16(unsigned int a)
{
  unsigned int d;

  if ((a & 0x1e) == 0x10) return (fifo_bytes > 0x3f) ? 0 : (unsigned)(0x3f - fifo_bytes);
  d = (fifo_bytes | !busy_n()) ? 0 : 0x8000;
  return d | (r12 & 0x7fff);
}

static unsigned int pico_r8(unsigned int a)
{
  if ((a & 0x1e) == 0x10 || (a & 0x1e) == 0x12)
  {
    unsigned int d = reg_read16(a);
    if (!(a & 1)) d >>= 8;
    return d & 0xff;
  }
  return orig_read8 ? orig_read8(a) : 0;
}

static unsigned int pico_r16(unsigned int a)
{
  if ((a & 0x1e) == 0x10 || (a & 0x1e) == 0x12) return reg_read16(a);
  return orig_read16 ? orig_read16(a) : 0;
}

static void pico_w8(unsigned int a, unsigned int d) { (void)a; (void)d; }

static void pico_w16(unsigned int a, unsigned int d)
{
  a &= 0xffff;
  if (a == 0x0010)
  {
    fifo_bytes += 2;
    if (fifo_ptr < fifo + FIFO_SIZE) { *fifo_ptr++ = (unsigned char)(d >> 8); *fifo_ptr++ = (unsigned char)d; }
    else if (fifo_ptr == fifo + FIFO_SIZE) fifo_ptr++;                           /* overflow: what comes is lost */
  }
  else if (a == 0x0012)
  {
    r12 = d & 0xffff;
    xp.samplegain = (short)((8 - (d & 7)) * 4);
    set_filter((int)((d & 0xc0) >> 6));
    xp.irqenable = (d & 0x4000) ? 3 : 0;
    if (d & 0x8000)                                   /* a 1 here resets the chip */
    {
      advance(target_for_line((int)v_counter));       /* (the chip is brought up to now first) */
      ppcm_reset_n(0);
      fifo_ptr = fifo;
      fifo_bytes = 0;
      ppcm_reset_n(1);
    }
  }
}

static void reset_chip(void)
{
  memset(&xp, 0, sizeof(xp));
  memset(fifo, 0, sizeof(fifo));
  fifo_ptr = fifo;
  fifo_bytes = 0;
  r12 = 0;
  mixn = 0;
  host_rate = 0;
  xp.startpin = 1;
  xp.samplegain = 32;                                 /* gain 8 */
  ppcm_reset_n(1);
  rerate();
}

/* After the core has set up a game (system_init): the register handlers of a Sega Pico game are put in front of the core's. */
void picopcm_attach(void)
{
  attached = 0;
  if (system_hw != SYSTEM_PICO) return;
  orig_read8  = m68k.memory_map[0x80].read8;
  orig_read16 = m68k.memory_map[0x80].read16;
  m68k.memory_map[0x80].read8   = pico_r8;
  m68k.memory_map[0x80].read16  = pico_r16;
  m68k.memory_map[0x80].write8  = pico_w8;
  m68k.memory_map[0x80].write16 = pico_w16;
  reset_chip();
  attached = 1;
}

/* The chip's state goes after the core's in a save state: its settings, the FIFO and the decoder, then their size and "PPCM" at the very end
   (a state from before has none: loading it leaves the chip as it is). Returns the bytes added. */
#define STATE_MAGIC "PPCM"

static unsigned state_payload_size(void)
{
  return (unsigned)(sizeof(xp) + sizeof(fifo) + 3 * 4);
}

int picopcm_state_save(unsigned char *buf)
{
  unsigned char *p = buf;
  int v;
  unsigned size = state_payload_size();

  if (!attached) return 0;
  memcpy(p, &xp, sizeof(xp)); p += sizeof(xp);
  memcpy(p, fifo, sizeof(fifo)); p += sizeof(fifo);
  v = fifo_bytes;                 memcpy(p, &v, 4); p += 4;
  v = (int)(fifo_ptr - fifo);     memcpy(p, &v, 4); p += 4;
  { unsigned r = r12;             memcpy(p, &r, 4); p += 4; }
  memcpy(p, &size, 4); p += 4;
  memcpy(p, STATE_MAGIC, 4); p += 4;
  return (int)(p - buf);
}

void picopcm_state_load(const unsigned char *buf, size_t len)
{
  const unsigned char *p;
  unsigned size, r;
  int bytes, off, f;

  if (!attached || len < 8 || memcmp(buf + len - 4, STATE_MAGIC, 4)) return;
  memcpy(&size, buf + len - 8, 4);
  if (size != state_payload_size() || (size_t)size > len - 8) return;
  p = buf + len - 8 - size;
  memcpy(&xp, p, sizeof(xp)); p += sizeof(xp);
  memcpy(fifo, p, sizeof(fifo)); p += sizeof(fifo);
  memcpy(&bytes, p, 4); p += 4;
  memcpy(&off, p, 4); p += 4;
  memcpy(&r, p, 4);
  if (off < 0 || off > FIFO_SIZE + 1 || bytes < 0) { reset_chip(); return; }
  fifo_ptr = fifo + off;
  fifo_bytes = bytes;
  r12 = r;
  memset(mix, 0, sizeof(mix)); mixn = 0;
  host_rate = 0;                                      /* the filters and the step are made again for the sound rate in use */
  f = xp.filter; xp.filter = -1;
  rerate();                                           /* (it puts the filter in use back, with a clean history) */
  if (f >= 0 && f < 4) set_filter(f);
}

void picopcm_reset(void)
{
  if (attached) reset_chip();
}

/* From render_line: every 8 lines the chip is run up to there, so that the interrupt for more data comes at the pace of the sound. */
void picopcm_line(int line)
{
  if (!attached || (line & 7) != 7 || lines_per_frame < 100) return;
  if (!(fifo_bytes | !busy_n())) return;              /* nothing to play */
  rerate();
  advance(target_for_line(line));
}

/* After audio_update(): the rest of the frame's voice is made and added to the frame's sound (16 bit stereo, `frames` samples). */
void picopcm_mix(short *out, int frames)
{
  int i;

  if (!attached) return;
  if (!out || frames <= 0) { memset(mix, 0, sizeof(mix)); mixn = 0; return; }       /* (a frame with no sound: what was made is dropped, so the next frame starts clean) */
  rerate();
  if ((fifo_bytes | !busy_n()) || mixn > 0 || xp.sample != 0)
  {
    advance(frames);
    if (!(fifo_bytes | !busy_n())) xp.sample = 0;     /* the voice is over: the last level is let go (once) */
  }
  else { mixn = 0; return; }
  for (i = 0; i < frames && i < MIX_MAX; i++)
  {
    int l = out[2 * i] + mix[i], r = out[2 * i + 1] + mix[i];
    out[2 * i]     = (short)Limit(l, 32767, -32768);
    out[2 * i + 1] = (short)Limit(r, 32767, -32768);
  }
  {
    int used = mixn > frames ? mixn : frames;                       /* (all that was made is cleared, also what went past the frame) */
    memset(mix, 0, sizeof(mix[0]) * (size_t)(used < MIX_MAX ? used : MIX_MAX));
  }
  mixn = 0;
}
