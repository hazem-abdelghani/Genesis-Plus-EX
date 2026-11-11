/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  dbgcpu.c -- Tools > Debug > CPU Debugger: stops the 68000 of the game
 *  at a breakpoint (an instruction reached, or an address read or written),
 *  shows the instructions around the one it stopped at (disassembled), the
 *  registers and the calls that led there, and steps one instruction at a
 *  time (into a call, over it, or out of it).
 *
 *  It uses the CPU hook of the Genesis Plus GX core (HOOK_CPU, which the
 *  Makefile defines; the core itself is not changed). The hook is only set
 *  while this window is open. While the game is stopped the program waits
 *  inside the hook, answering the debug windows (the Memory Viewer and the
 *  others keep showing the game as it is), and the main window is locked.
 *
 *  A 32X game (which PicoDrive runs) is debugged the same way with its three
 *  processors: the 68000 and the Master and Slave SH-2s. The PicoDrive library
 *  calls the debugger before every instruction (gpex_set_cpu_hook); there are
 *  Execute breakpoints only, no Read or Write ones, and the SH-2s are shown
 *  with the library's own disassembler.
 *
 *  Limits: the main 68000 only in the other games (not the Mega CD's second
 *  one, which a read or write breakpoint may also catch), and not the Z80,
 *  which neither core hooks. The idea comes from the Genesis Plus GX debugger
 *  project (smd_dgx); this code is written for Win32.
 ****************************************************************************/

#include <windows.h>
#include <commctrl.h>

#include "shared.h"
#include "cpuhook.h"
#include "gui.h"
#include "theme.h"
#include "resource.h"
#include "netplay.h"
#include "picocore.h"

#define DC_CLASS   "GPEXCpuDebugger"
#define DC_TIMER   1
#define DC_TICK_MS 100
#define DC_MAXBP   64
#define DC_STACK   128

#define IDC_DC_CONT  101
#define IDC_DC_BREAK 102
#define IDC_DC_INTO  103
#define IDC_DC_OVER  104
#define IDC_DC_OUT   105
#define IDC_DC_TO    106
#define IDC_DC_ADDR  107
#define IDC_DC_GO    108
#define IDC_DC_ADD   109
#define IDC_DC_DEL   110
#define IDC_DC_TOG   111
#define IDC_DC_CPU   112
#define IDC_DC_BTNS  10

enum { M_RUN, M_STEP, M_RUNTO, M_OUT };
enum { BP_E = 1, BP_R = 2, BP_W = 4 };

typedef struct { unsigned addr, end; int type, enabled, cpu; } bp_t;
typedef struct { unsigned ret, target; } frame_t;

static bp_t dc_bp[DC_MAXBP];
static int dc_nbp, dc_selbp = -1;
static frame_t dc_stk[3][DC_STACK];              /* the calls of each processor (0: the 68000, 1: the master SH-2, 2: the slave SH-2) */
static int dc_dep[3];
static unsigned dc_pcs[3];                       /* the instruction each processor stopped at (or last ran) */
static int dc_cur, dc_stopcpu, dc_stepcpu;       /* the processor shown, the one that stopped, the one that steps */
static int dc_32x;                               /* a 32X game: PicoDrive runs it */
static volatile int dc_np;                       /* netplay is on (looked at every tick of the timer, not at every instruction) */
static int dc_rte_wait;                          /* Step Out met a return from an exception: the game stops after this many more instructions */
static unsigned char *dc_mem[3];                 /* its ROM, 68000 RAM and SH-2 RAM (as the library keeps them) */
static unsigned dc_memn[3];
#define dc_stack dc_stk[dc_cur]
#define dc_depth dc_dep[dc_cur]
#define dc_pc    dc_pcs[dc_cur]
static int dc_mode, dc_runto, dc_rundepth;       /* how the game runs until it stops again */
static volatile int dc_pause_req;                /* the Break button was pressed */
static int dc_broken;                            /* the game is stopped inside the hook */
static char dc_reason[120] = "Running";

static HWND dc_win, dc_btn[IDC_DC_BTNS], dc_addr, dc_cpucb;
static HFONT dc_mono, dc_ui;
static int dc_cw, dc_lh;
static int dc_bw[6];                              /* the widths of the buttons of the strip (the longest translation sets them) */
static UINT_PTR dc_look;
static unsigned dc_top;                           /* the first address of the disassembly */
static unsigned dc_sel;                           /* the instruction picked in it */
static int dc_have_sel;

static int S(int px)
{
  return dbg_scale(px);
}

/****************************************************************************
 * The bus (no side effects: only what the CPU reads from memory directly)
 ****************************************************************************/

/* A word of one of the 32X game's memories (the pairs of bytes are swapped in them). */
static unsigned dc_memw(int k, unsigned off)
{
  off &= ~1u;
  return (dc_mem[k] && off + 1 < dc_memn[k]) ? (((unsigned)dc_mem[k][off + 1] << 8) | dc_mem[k][off]) : 0;
}

/* A word the 68000 reads. */
static unsigned dc_rd16(unsigned a)
{
  cpu_memory_map *m;

  if (dc_32x)
  {
    a &= 0xFFFFFF;
    if (a < 0x400000) return dc_memw(0, a);
    if (a >= 0x880000 && a < 0x900000) return dc_memw(0, a - 0x880000);      /* (the first 512 KB of the cartridge, as the 32X shows them) */
    if (a >= 0x900000 && a < 0xA00000)                                       /* the bank of the cartridge the game picked */
    {
      unsigned bank;

      return pico_regs(4, &bank, 1) > 0 ? dc_memw(0, (bank << 20) + (a - 0x900000)) : 0;
    }
    if (a >= 0xE00000) return dc_memw(1, a & 0xFFFF);
    return 0;
  }
  m = &m68k.memory_map[(a >> 16) & 0xFF];
  if (m->read16) return 0;
  return *(uint16 *)(m->base + (a & 0xFFFE));
}

/* A word an SH-2 reads (the cartridge window and its RAM; the top bits only say whether the cache is used). */
static unsigned dc_rd_sh2(unsigned a)
{
  a &= 0x1FFFFFFF;
  if (a >= 0x02000000 && a < 0x02400000) return dc_memw(0, a - 0x02000000);
  if (a >= 0x06000000 && a < 0x06040000) return dc_memw(2, a - 0x06000000);
  return 0;
}

/* What an SH-2 instruction does to the calls: 1 = it calls (BSR, BSRF, JSR; the return is 4 bytes on, after the delay slot), -1 = RTS, 2 = RTE, 3 = JMP @Rn, 0 = none of them. */
static int dc_sh2_call_kind(unsigned op)
{
  if ((op & 0xF000) == 0xB000 || (op & 0xF0FF) == 0x0003 || (op & 0xF0FF) == 0x400B) return 1;
  if (op == 0x000B) return -1;
  if (op == 0x002B) return 2;                              /* RTE */
  if ((op & 0xF0FF) == 0x402B) return 3;                   /* JMP @Rn (it may be a return) */
  return 0;
}

static int dc_same(int cpu, unsigned a, unsigned b)
{
  return cpu ? ((a & 0x1FFFFFFF) == (b & 0x1FFFFFFF)) : a == b;
}

/****************************************************************************
 * The 68000 disassembler
 ****************************************************************************/

static unsigned d_pc;
static const char *const d_cc[16] = { "t", "f", "hi", "ls", "cc", "cs", "ne", "eq", "vc", "vs", "pl", "mi", "ge", "lt", "gt", "le" };
static const char d_sz[3] = { 'b', 'w', 'l' };

static unsigned d_fetch(void)
{
  unsigned v = dc_rd16(d_pc);

  d_pc += 2;
  return v;
}

static void d_signed(char *o, int v)
{
  if (v < 0) wsprintfA(o, "-$%X", -v); else wsprintfA(o, "$%X", v);
}

/* An effective address; size 0, 1, 2 for a byte, word or long (only an immediate needs it). */
static void d_ea(char *o, int mode, int reg, int size)
{
  unsigned e, base;
  char t[24];

  switch (mode)
  {
    case 0: wsprintfA(o, "d%d", reg); break;
    case 1: wsprintfA(o, "a%d", reg); break;
    case 2: wsprintfA(o, "(a%d)", reg); break;
    case 3: wsprintfA(o, "(a%d)+", reg); break;
    case 4: wsprintfA(o, "-(a%d)", reg); break;
    case 5: d_signed(t, (short)d_fetch()); wsprintfA(o, "%s(a%d)", t, reg); break;
    case 6:
      e = d_fetch();
      d_signed(t, (signed char)(e & 0xFF));
      wsprintfA(o, "%s(a%d,%c%d.%c)", t, reg, (e & 0x8000) ? 'a' : 'd', (int)((e >> 12) & 7), (e & 0x800) ? 'l' : 'w');
      break;
    default:
      switch (reg)
      {
        case 0: wsprintfA(o, "$%X.w", d_fetch()); break;
        case 1: e = d_fetch(); wsprintfA(o, "$%X.l", (e << 16) | d_fetch()); break;
        case 2: base = d_pc; wsprintfA(o, "$%X(pc)", (unsigned)(base + (short)d_fetch())); break;
        case 3:
          base = d_pc;
          e = d_fetch();
          wsprintfA(o, "$%X(pc,%c%d.%c)", (unsigned)(base + (signed char)(e & 0xFF)), (e & 0x8000) ? 'a' : 'd', (int)((e >> 12) & 7), (e & 0x800) ? 'l' : 'w');
          break;
        case 4:
          if (size == 2) { e = d_fetch(); wsprintfA(o, "#$%X", (e << 16) | d_fetch()); }
          else { e = d_fetch(); wsprintfA(o, "#$%X", size == 0 ? (e & 0xFF) : e); }
          break;
        default: wsprintfA(o, "?"); break;
      }
  }
}

static void d_regs(char *o, unsigned mask, int reversed)
{
  int i, n = 0, run;

  o[0] = '\0';
  for (i = 0; i < 16; i++)
  {
    int bit = reversed ? 15 - i : i;

    if (mask & (1u << bit))
    {
      int j = i;
      char t[16];

      run = 0;
      while (j + 1 < 16 && (j + 1) / 8 == i / 8 && (mask & (1u << (reversed ? 15 - (j + 1) : j + 1)))) { j++; run = 1; }
      if (n) lstrcatA(o, "/");
      if (run) wsprintfA(t, "%c%d-%c%d", i < 8 ? 'd' : 'a', i & 7, i < 8 ? 'd' : 'a', j & 7);
      else wsprintfA(t, "%c%d", i < 8 ? 'd' : 'a', i & 7);
      lstrcatA(o, t);
      n++;
      i = j;
    }
  }
}

static void d_out(char *out, const char *mn, const char *ops)
{
  char t[96];

  if (ops && ops[0]) wsprintfA(t, "%-8s%s", mn, ops); else lstrcpyA(t, mn);
  lstrcpynA(out, t, 60);
}

/* Disassembles the instruction at pc: the text goes to out, the length in bytes is returned. */
static int dis68k(unsigned pc, char *out)
{
  unsigned op, ext;
  int mode, reg, size, dn;
  char a[40], b[40], mn[16], ops[96], t[24];
  static const char *const shifts[4] = { "as", "ls", "rox", "ro" };
  static const char *const imm_ops[8] = { "ori", "andi", "subi", "addi", "?", "eori", "cmpi", "?" };

  d_pc = pc;
  op = d_fetch();
  mode = (op >> 3) & 7; reg = op & 7; dn = (op >> 9) & 7; size = (op >> 6) & 3;
  mn[0] = ops[0] = '\0';

  switch (op >> 12)
  {
    case 0:
      if ((op & 0x0138) == 0x0108)                                   /* movep */
      {
        int dsp = (short)d_fetch();

        d_signed(t, dsp);
        wsprintfA(mn, "movep.%c", (op & 0x40) ? 'l' : 'w');
        if (op & 0x80) wsprintfA(ops, "d%d,%s(a%d)", dn, t, reg); else wsprintfA(ops, "%s(a%d),d%d", t, reg, dn);
      }
      else if (op & 0x0100)                                          /* a bit instruction with the bit number in a register */
      {
        static const char *const bits[4] = { "btst", "bchg", "bclr", "bset" };

        d_ea(b, mode, reg, 0);
        lstrcpyA(mn, bits[size]);
        wsprintfA(ops, "d%d,%s", dn, b);
      }
      else if (dn == 4)                                              /* a bit instruction with a fixed number */
      {
        static const char *const bits[4] = { "btst", "bchg", "bclr", "bset" };

        ext = d_fetch() & 0xFF;
        d_ea(b, mode, reg, 0);
        lstrcpyA(mn, bits[size]);
        wsprintfA(ops, "#%u,%s", ext, b);
      }
      else if (size != 3 && dn != 7 && dn != 4)
      {
        if ((dn == 0 || dn == 1 || dn == 5) && mode == 7 && reg == 4 && size < 2)         /* to the CCR or the SR */
        {
          ext = d_fetch();
          wsprintfA(mn, "%s", imm_ops[dn]);
          wsprintfA(ops, "#$%X,%s", size == 0 ? (ext & 0xFF) : ext, size == 0 ? "ccr" : "sr");
        }
        else
        {
          if (size == 2) { ext = d_fetch(); ext = (ext << 16) | d_fetch(); } else { ext = d_fetch(); if (size == 0) ext &= 0xFF; }
          d_ea(b, mode, reg, size);
          wsprintfA(mn, "%s.%c", imm_ops[dn], d_sz[size]);
          wsprintfA(ops, "#$%X,%s", ext, b);
        }
      }
      else lstrcpyA(mn, "dc.w");
      break;

    case 1: case 2: case 3:                                          /* move */
    {
      int sz = (op >> 12) == 1 ? 0 : (op >> 12) == 3 ? 1 : 2, dmode = (op >> 6) & 7;

      d_ea(a, mode, reg, sz);
      d_ea(b, dmode, dn, sz);
      wsprintfA(mn, "%s.%c", dmode == 1 ? "movea" : "move", d_sz[sz]);
      wsprintfA(ops, "%s,%s", a, b);
      break;
    }

    case 4:
      if (op == 0x4AFC) lstrcpyA(mn, "illegal");
      else if (op == 0x4E70) lstrcpyA(mn, "reset");
      else if (op == 0x4E71) lstrcpyA(mn, "nop");
      else if (op == 0x4E72) { wsprintfA(ops, "#$%X", d_fetch()); lstrcpyA(mn, "stop"); }
      else if (op == 0x4E73) lstrcpyA(mn, "rte");
      else if (op == 0x4E75) lstrcpyA(mn, "rts");
      else if (op == 0x4E76) lstrcpyA(mn, "trapv");
      else if (op == 0x4E77) lstrcpyA(mn, "rtr");
      else if ((op & 0xFFF0) == 0x4E40) { lstrcpyA(mn, "trap"); wsprintfA(ops, "#%u", op & 15); }
      else if ((op & 0xFFF8) == 0x4E50) { d_signed(t, (short)d_fetch()); lstrcpyA(mn, "link"); wsprintfA(ops, "a%d,#%s", reg, t); }
      else if ((op & 0xFFF8) == 0x4E58) { lstrcpyA(mn, "unlk"); wsprintfA(ops, "a%d", reg); }
      else if ((op & 0xFFF8) == 0x4E60) { lstrcpyA(mn, "move"); wsprintfA(ops, "a%d,usp", reg); }
      else if ((op & 0xFFF8) == 0x4E68) { lstrcpyA(mn, "move"); wsprintfA(ops, "usp,a%d", reg); }
      else if ((op & 0xFFC0) == 0x4E80) { d_ea(a, mode, reg, 2); lstrcpyA(mn, "jsr"); lstrcpyA(ops, a); }
      else if ((op & 0xFFC0) == 0x4EC0) { d_ea(a, mode, reg, 2); lstrcpyA(mn, "jmp"); lstrcpyA(ops, a); }
      else if ((op & 0xF1C0) == 0x41C0) { d_ea(a, mode, reg, 2); lstrcpyA(mn, "lea"); wsprintfA(ops, "%s,a%d", a, dn); }
      else if ((op & 0xF1C0) == 0x4180) { d_ea(a, mode, reg, 1); lstrcpyA(mn, "chk"); wsprintfA(ops, "%s,d%d", a, dn); }
      else if ((op & 0xFFF8) == 0x4840) { lstrcpyA(mn, "swap"); wsprintfA(ops, "d%d", reg); }
      else if ((op & 0xFFC0) == 0x4840) { d_ea(a, mode, reg, 2); lstrcpyA(mn, "pea"); lstrcpyA(ops, a); }
      else if ((op & 0xFFB8) == 0x4880) { lstrcpyA(mn, (op & 0x40) ? "ext.l" : "ext.w"); wsprintfA(ops, "d%d", reg); }
      else if ((op & 0xFB80) == 0x4880)                              /* movem */
      {
        unsigned mask = d_fetch();
        int to_reg = (op & 0x0400) != 0;

        d_ea(a, mode, reg, 2);
        d_regs(b, mask, !to_reg && mode == 4);
        wsprintfA(mn, "movem.%c", (op & 0x40) ? 'l' : 'w');
        if (to_reg) wsprintfA(ops, "%s,%s", a, b); else wsprintfA(ops, "%s,%s", b, a);
      }
      else if ((op & 0xFFC0) == 0x4800) { d_ea(a, mode, reg, 0); lstrcpyA(mn, "nbcd"); lstrcpyA(ops, a); }
      else if ((op & 0xFFC0) == 0x40C0) { d_ea(a, mode, reg, 1); lstrcpyA(mn, "move"); wsprintfA(ops, "sr,%s", a); }
      else if ((op & 0xFFC0) == 0x44C0) { d_ea(a, mode, reg, 1); lstrcpyA(mn, "move"); wsprintfA(ops, "%s,ccr", a); }
      else if ((op & 0xFFC0) == 0x46C0) { d_ea(a, mode, reg, 1); lstrcpyA(mn, "move"); wsprintfA(ops, "%s,sr", a); }
      else if ((op & 0xFFC0) == 0x4AC0) { d_ea(a, mode, reg, 0); lstrcpyA(mn, "tas"); lstrcpyA(ops, a); }
      else if (size != 3 && ((op & 0x0F00) == 0x0000 || (op & 0x0F00) == 0x0200 || (op & 0x0F00) == 0x0400 || (op & 0x0F00) == 0x0600 || (op & 0x0F00) == 0x0A00))
      {
        static const char *const names[16] = { "negx", NULL, "clr", NULL, "neg", NULL, "not", NULL, NULL, NULL, "tst", NULL, NULL, NULL, NULL, NULL };

        d_ea(a, mode, reg, size);
        wsprintfA(mn, "%s.%c", names[(op >> 8) & 15], d_sz[size]);
        lstrcpyA(ops, a);
      }
      else lstrcpyA(mn, "dc.w");
      break;

    case 5:
      if (size == 3)
      {
        if (mode == 1)                                               /* dbcc */
        {
          unsigned base = d_pc;

          wsprintfA(mn, "db%s", d_cc[(op >> 8) & 15]);
          wsprintfA(ops, "d%d,$%X", reg, (unsigned)(base + (short)d_fetch()));
        }
        else { d_ea(a, mode, reg, 0); wsprintfA(mn, "s%s", d_cc[(op >> 8) & 15]); lstrcpyA(ops, a); }
      }
      else
      {
        d_ea(a, mode, reg, size);
        wsprintfA(mn, "%s.%c", (op & 0x100) ? "subq" : "addq", d_sz[size]);
        wsprintfA(ops, "#%d,%s", dn ? dn : 8, a);
      }
      break;

    case 6:
    {
      unsigned base = d_pc;
      int disp = (signed char)(op & 0xFF);
      int cc = (op >> 8) & 15;

      if ((op & 0xFF) == 0) disp = (short)d_fetch();
      wsprintfA(mn, "%s%s", cc == 0 ? "bra" : cc == 1 ? "bsr" : "b", cc > 1 ? d_cc[cc] : "");
      wsprintfA(ops, "$%X", (unsigned)(base + disp));
      break;
    }

    case 7:
      lstrcpyA(mn, "moveq");
      wsprintfA(ops, "#%d,d%d", (signed char)(op & 0xFF), dn);
      break;

    case 8: case 12:
      if (size == 3)
      {
        d_ea(a, mode, reg, 1);
        wsprintfA(mn, "%s.w", (op >> 12) == 8 ? ((op & 0x100) ? "divs" : "divu") : ((op & 0x100) ? "muls" : "mulu"));
        wsprintfA(ops, "%s,d%d", a, dn);
      }
      else if ((op & 0x01F0) == 0x0100)                              /* sbcd, abcd */
      {
        lstrcpyA(mn, (op >> 12) == 8 ? "sbcd" : "abcd");
        if (op & 8) wsprintfA(ops, "-(a%d),-(a%d)", reg, dn); else wsprintfA(ops, "d%d,d%d", reg, dn);
      }
      else if ((op >> 12) == 12 && (op & 0x0130) == 0x0100 && (((op >> 3) & 0x1F) == 8 || ((op >> 3) & 0x1F) == 9 || ((op >> 3) & 0x1F) == 17))   /* exg */
      {
        int k = (op >> 3) & 0x1F;

        lstrcpyA(mn, "exg");
        wsprintfA(ops, "%c%d,%c%d", k == 9 ? 'a' : 'd', dn, k == 8 ? 'd' : 'a', reg);
      }
      else
      {
        d_ea(a, mode, reg, size);
        wsprintfA(mn, "%s.%c", (op >> 12) == 8 ? "or" : "and", d_sz[size]);
        if (op & 0x100) wsprintfA(ops, "d%d,%s", dn, a); else wsprintfA(ops, "%s,d%d", a, dn);
      }
      break;

    case 9: case 13:
    {
      const char *base = (op >> 12) == 9 ? "sub" : "add";

      if (size == 3)                                                 /* suba, adda */
      {
        d_ea(a, mode, reg, (op & 0x100) ? 2 : 1);
        wsprintfA(mn, "%sa.%c", base, (op & 0x100) ? 'l' : 'w');
        wsprintfA(ops, "%s,a%d", a, dn);
      }
      else if ((op & 0x0130) == 0x0100)                              /* subx, addx */
      {
        wsprintfA(mn, "%sx.%c", base, d_sz[size]);
        if (op & 8) wsprintfA(ops, "-(a%d),-(a%d)", reg, dn); else wsprintfA(ops, "d%d,d%d", reg, dn);
      }
      else
      {
        d_ea(a, mode, reg, size);
        wsprintfA(mn, "%s.%c", base, d_sz[size]);
        if (op & 0x100) wsprintfA(ops, "d%d,%s", dn, a); else wsprintfA(ops, "%s,d%d", a, dn);
      }
      break;
    }

    case 11:
      if (size == 3)
      {
        d_ea(a, mode, reg, (op & 0x100) ? 2 : 1);
        wsprintfA(mn, "cmpa.%c", (op & 0x100) ? 'l' : 'w');
        wsprintfA(ops, "%s,a%d", a, dn);
      }
      else if ((op & 0x0138) == 0x0108) { wsprintfA(mn, "cmpm.%c", d_sz[size]); wsprintfA(ops, "(a%d)+,(a%d)+", reg, dn); }
      else if (op & 0x100) { d_ea(a, mode, reg, size); wsprintfA(mn, "eor.%c", d_sz[size]); wsprintfA(ops, "d%d,%s", dn, a); }
      else { d_ea(a, mode, reg, size); wsprintfA(mn, "cmp.%c", d_sz[size]); wsprintfA(ops, "%s,d%d", a, dn); }
      break;

    case 14:
      if (size == 3)                                                 /* a shift of memory by one */
      {
        d_ea(a, mode, reg, 1);
        wsprintfA(mn, "%s%c.w", shifts[(op >> 9) & 3], (op & 0x100) ? 'l' : 'r');
        lstrcpyA(ops, a);
      }
      else
      {
        wsprintfA(mn, "%s%c.%c", shifts[(op >> 3) & 3], (op & 0x100) ? 'l' : 'r', d_sz[size]);
        if (op & 0x20) wsprintfA(ops, "d%d,d%d", dn, reg); else wsprintfA(ops, "#%d,d%d", dn ? dn : 8, reg);
      }
      break;

    case 10: wsprintfA(mn, "line-a"); wsprintfA(ops, "#$%X", op & 0xFFF); break;
    default: wsprintfA(mn, "line-f"); wsprintfA(ops, "#$%X", op & 0xFFF); break;
  }

  if (!mn[0]) lstrcpyA(mn, "dc.w");
  if (!lstrcmpA(mn, "dc.w")) { wsprintfA(ops, "$%04X", op); d_pc = pc + 2; }
  d_out(out, mn, ops);
  return (int)(d_pc - pc);
}

/* What an instruction does to the calls: 1 = it calls (JSR, BSR), -1 = it returns (RTS, RTR), 0 = neither. */
static int dc_call_kind(unsigned op, unsigned pc, int *len)
{
  char t[64];

  if ((op & 0xFFC0) == 0x4E80 || (op & 0xFF00) == 0x6100) { *len = dis68k(pc, t); return 1; }
  if (op == 0x4E75 || op == 0x4E77) return -1;           /* (not RTE: an interrupt does not put a call on the list, so the end of its handler takes none off) */
  return 0;
}

/* An instruction of the processor shown: its text goes to out, its length in bytes is returned. */
static int dc_dis(unsigned a, char *out)
{
  char t[80];

  if (dc_cur == 0) return dis68k(a, out);
  pico_dasm_sh2(t, a, dc_rd_sh2(a));
  lstrcpynA(out, t, 60);
  return 2;
}

/****************************************************************************
 * Stopping the game
 ****************************************************************************/

static void dc_refresh(int follow);

static void dc_set_buttons(void)
{
  int i;

  if (!dc_win) return;
  for (i = 0; i < 6; i++)
    EnableWindow(dc_btn[i], i == 1 ? !dc_broken : dc_broken);      /* Break while it runs; the others while it is stopped */
}

/* The game is stopped here: this waits, answering the program's windows, until a button of the debugger lets it go on. */
static void dc_stop(const char *reason)
{
  MSG msg;

  if (!dc_win) return;
  dc_broken = 1;
  dc_pause_req = 0;
  dc_mode = M_RUN;
  dc_cur = dc_stopcpu;
  dc_rte_wait = 0;
  if (dc_cpucb) SendMessage(dc_cpucb, CB_SETCURSEL, (WPARAM)dc_cur, 0);
  lstrcpynA(dc_reason, reason, sizeof(dc_reason));
  dc_set_buttons();
  dc_refresh(1);
  SetForegroundWindow(GetAncestor(dc_win, GA_ROOT));
  EnableWindow(g_hwnd, FALSE);
  while (dc_broken)
  {
    if (GetMessage(&msg, NULL, 0, 0) <= 0)
    {
      if (msg.message == WM_QUIT) PostQuitMessage((int)msg.wParam);        /* the program is closing: the game goes on, and ends */
      dc_broken = 0;
      break;
    }
    if (msg.hwnd == g_hwnd && (msg.message == WM_CLOSE || (msg.message == WM_SYSCOMMAND && (msg.wParam & 0xFFF0) == SC_CLOSE))) continue;
    if (memview_message(&msg) || debugwin_message(&msg)) continue;
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
  EnableWindow(g_hwnd, TRUE);
  lstrcpynA(dc_reason, "Running", sizeof(dc_reason));
  dc_set_buttons();
  emu_timing_resync();
}

static int dc_match(const bp_t *b, unsigned a, int width, int type)
{
  return b->enabled && (b->type & type) && a + (unsigned)width > b->addr && a <= b->end;
}

static void dc_hook(hook_type_t type, int width, unsigned address, unsigned value)
{
  int i;

  (void)value;
  if (!dc_win || dc_broken || dc_np) return;

  if (type == HOOK_M68K_E)
  {
    unsigned op;
    int kind, len = 0;

    if (address != m68k.pc) return;                          /* the Mega CD's second 68000 */
    dc_pc = address;
    dc_stopcpu = 0;
    op = dc_rd16(address);

    if (dc_pause_req) dc_stop("Stopped (Break)");
    else if (dc_rte_wait && --dc_rte_wait == 0) dc_stop("Step out (return from exception)");
    else if (dc_mode == M_STEP) dc_stop("Step");
    else if (dc_mode == M_RUNTO && address == (unsigned)dc_runto && dc_depth <= dc_rundepth) dc_stop("Run to cursor");
    else if (dc_mode == M_OUT && dc_depth < dc_rundepth) dc_stop("Step out");
    else
      for (i = 0; i < dc_nbp; i++)
        if (dc_match(&dc_bp[i], address, 2, BP_E) && address == dc_bp[i].addr) { char t[64]; wsprintfA(t, "Breakpoint at $%X", address); dc_stop(t); break; }

    if (op == 0x4E73 && dc_mode == M_OUT && dc_depth <= dc_rundepth) dc_rte_wait = 1;     /* RTE: the handler ends, so Step Out stops just after */
    kind = dc_call_kind(op, address, &len);                  /* (what the instruction does to the call stack, once it runs) */
    if (kind == 1 && dc_depth < DC_STACK)
    {
      dc_stack[dc_depth].ret = address + (unsigned)len;
      dc_stack[dc_depth].target = 0;
      dc_depth++;
    }
    else if (kind == -1 && dc_depth > 0) dc_depth--;
    return;
  }

  if (type == HOOK_M68K_R || type == HOOK_M68K_W)
  {
    for (i = 0; i < dc_nbp; i++)
      if (dc_match(&dc_bp[i], address, width, type == HOOK_M68K_R ? BP_R : BP_W))
      {
        char t[96];

        wsprintfA(t, "%s of $%X (%d byte%s) at $%X", type == HOOK_M68K_R ? "Read" : "Write", address, width, width == 1 ? "" : "s", dc_pc);
        dc_stop(t);
        return;
      }
  }
}

/* A 32X game: PicoDrive calls this before every instruction of the 68000 (cpu 0) and the SH-2s (1, 2). */
static void dc_pico_hook(int cpu, unsigned pc)
{
  int i, kind, len = 4;
  const char *why = NULL;
  char t[80];
  static const char *const names[3] = { "68000", "Master SH-2", "Slave SH-2" };

  if (cpu < 0 || cpu > 2) return;
  dc_pcs[cpu] = pc;
  if (!dc_win || dc_broken || dc_np) return;

  if (dc_pause_req) why = "Stopped (Break)";
  else if (dc_rte_wait && cpu == dc_stepcpu && --dc_rte_wait == 0) why = "Step out (return from exception)";
  else if (dc_mode == M_STEP && cpu == dc_stepcpu) why = "Step";
  else if (dc_mode == M_RUNTO && cpu == dc_stepcpu && dc_same(cpu, pc, (unsigned)dc_runto) && dc_dep[cpu] <= dc_rundepth) why = "Run to cursor";
  else if (dc_mode == M_OUT && cpu == dc_stepcpu && dc_dep[cpu] < dc_rundepth) why = "Step out";
  else
    for (i = 0; i < dc_nbp; i++)
      if (dc_bp[i].enabled && (dc_bp[i].type & BP_E) && dc_bp[i].cpu == cpu && dc_same(cpu, dc_bp[i].addr, pc))
      {
        wsprintfA(t, "%s: breakpoint at $%X", names[cpu], pc);
        why = t;
        break;
      }
  if (why) { dc_stopcpu = cpu; dc_stop(why); }

  if (cpu == 0)
  {
    unsigned op = dc_rd16(pc);

    kind = dc_call_kind(op, pc, &len);                             /* (what the instruction does to the call stack, once it runs) */
    if (op == 0x4E73 && dc_mode == M_OUT && cpu == dc_stepcpu && dc_dep[0] <= dc_rundepth) dc_rte_wait = 1;
  }
  else
  {
    unsigned op = dc_rd_sh2(pc);

    kind = dc_sh2_call_kind(op);
    if (kind == 2)                                                 /* RTE: no call was put on the list for the exception, so there is none to take off */
    {
      if (dc_mode == M_OUT && cpu == dc_stepcpu && dc_dep[cpu] <= dc_rundepth) dc_rte_wait = 2;       /* (and a delay slot) */
      kind = 0;
    }
    else if (kind == -1 || kind == 3)                              /* RTS, or JMP @Rn: the calls up to the one this goes back to are over */
    {
      unsigned rg[24], dest;
      int k;

      if (pico_regs(cpu + 1, rg, 24) > 0)
      {
        dest = kind == -1 ? rg[17] : rg[(op >> 8) & 15];
        for (k = dc_dep[cpu] - 1; k >= 0; k--)
          if (dc_same(1, dc_stk[cpu][k].ret, dest)) { dc_dep[cpu] = k; break; }
        if (k < 0 && kind == -1 && dc_dep[cpu] > 0) dc_dep[cpu]--;
      }
      kind = 0;
    }
  }
  if (kind == 1 && dc_dep[cpu] < DC_STACK)
  {
    dc_stk[cpu][dc_dep[cpu]].ret = pc + (unsigned)len;
    dc_stk[cpu][dc_dep[cpu]].target = 0;
    dc_dep[cpu]++;
  }
  else if (kind == -1 && dc_dep[cpu] > 0) dc_dep[cpu]--;
}

static void dc_go(int mode)
{
  int cpu = dc_stopcpu, len = 4, kind;
  unsigned pc = dc_pcs[cpu];

  dc_mode = M_RUN;
  dc_rte_wait = 0;
  dc_stepcpu = cpu;
  if (mode == IDC_DC_INTO) dc_mode = M_STEP;
  else if (mode == IDC_DC_OVER)
  {
    kind = cpu ? dc_sh2_call_kind(dc_rd_sh2(pc)) : dc_call_kind(dc_rd16(pc), pc, &len);
    if (kind == 1) { dc_mode = M_RUNTO; dc_runto = (int)(pc + (unsigned)len); dc_rundepth = dc_dep[cpu]; }
    else dc_mode = M_STEP;
  }
  else if (mode == IDC_DC_OUT) { if (dc_dep[cpu] > 0) { dc_mode = M_OUT; dc_rundepth = dc_dep[cpu]; } }
  else if (mode == IDC_DC_TO && dc_have_sel) { dc_mode = M_RUNTO; dc_stepcpu = dc_cur; dc_runto = (int)dc_sel; dc_rundepth = 1 << 20; }
  dc_broken = 0;
}

/****************************************************************************
 * The window
 ****************************************************************************/

static void dc_fonts(void)
{
  HDC dc;
  TEXTMETRICA tm;
  HFONT old;

  if (dc_mono == dbg_mono_font() && dc_ui == dbg_ui_font() && dc_cw) return;
  dc_mono = dbg_mono_font();
  dc_ui = dbg_ui_font();
  dc = GetDC(NULL);
  old = (HFONT)SelectObject(dc, dc_mono);
  GetTextMetricsA(dc, &tm);
  SelectObject(dc, old);
  ReleaseDC(NULL, dc);
  dc_cw = tm.tmAveCharWidth > 0 ? tm.tmAveCharWidth : S(8);
  dc_lh = tm.tmHeight + S(2);
}

static int dc_strip_h(void)
{
  return S(40);
}

/* The size of the parts of the window. */
static void dc_parts(const RECT *c, RECT *dis, RECT *regs, RECT *stack, RECT *bps)
{
  int split = c->right * 55 / 100, top = dc_strip_h(), h = c->bottom - top;

  dis->left = 0; dis->top = top + dc_lh;      /* (a title line, "Disassembly", above the code) */ dis->right = split; dis->bottom = c->bottom - S(34);
  regs->left = split + 1; regs->top = top; regs->right = c->right; regs->bottom = top + h * 46 / 100;
  stack->left = split + 1; stack->top = regs->bottom; stack->right = c->right; stack->bottom = top + h * 72 / 100;
  bps->left = split + 1; bps->top = stack->bottom; bps->right = c->right; bps->bottom = c->bottom - S(34);
}

static void dc_layout(void)
{
  RECT c;
  int i, x = S(8), y = (dc_strip_h() - S(26)) / 2, bw = S(96), bh = S(26);
  static const char *const names[6] = { "Continue", "Break", "Step Into", "Step Over", "Step Out", "Run to Cursor" };

  HDC mdc;
  HGDIOBJ mold;

  if (!dc_win) return;
  GetClientRect(dc_win, &c);
  mdc = GetDC(dc_win);
  mold = SelectObject(mdc, dc_ui);
  for (i = 0; i < 6; i++)
  {
    int w = i == 5 ? S(120) : bw;
    SIZE tsz;

    if (GetTextExtentPoint32A(mdc, tr(names[i]), lstrlenA(tr(names[i])), &tsz) && tsz.cx + S(24) > w) w = tsz.cx + S(24);      /* (a translation may be longer) */
    dc_bw[i] = w;

    SetWindowTextA(dc_btn[i], names[i]);
    SendMessage(dc_btn[i], WM_SETFONT, (WPARAM)dc_ui, TRUE);
    SetWindowPos(dc_btn[i], NULL, x, y, w, bh, SWP_NOZORDER);
    x += w + S(6);
  }
  SelectObject(mdc, mold);
  ReleaseDC(dc_win, mdc);
  y = c.bottom - S(30);
  SendMessage(dc_addr, WM_SETFONT, (WPARAM)dc_ui, TRUE);
  SetWindowPos(dc_addr, NULL, S(8), y, S(100), S(24), SWP_NOZORDER);
  for (i = 6; i < IDC_DC_BTNS; i++) SendMessage(dc_btn[i], WM_SETFONT, (WPARAM)dc_ui, TRUE);
  SetWindowPos(dc_btn[6], NULL, S(114), y, S(44), S(24), SWP_NOZORDER);                  /* Go */
  if (dc_cpucb)
  {
    SendMessage(dc_cpucb, WM_SETFONT, (WPARAM)dc_ui, TRUE);
    SetWindowPos(dc_cpucb, NULL, S(166), y, S(150), S(200), SWP_NOZORDER);
  }
  SetWindowPos(dc_btn[7], NULL, c.right * 55 / 100 + S(8), y, S(80), S(24), SWP_NOZORDER);       /* Add... */
  SetWindowPos(dc_btn[8], NULL, c.right * 55 / 100 + S(92), y, S(80), S(24), SWP_NOZORDER);      /* Remove */
  SetWindowPos(dc_btn[9], NULL, c.right * 55 / 100 + S(176), y, S(100), S(24), SWP_NOZORDER);    /* Enable / Disable */
}

/* Where the disassembly starts so that the instruction at pc is a few lines down: tries starts a little before it and keeps the one the instructions
   after which land exactly on pc. */
static unsigned dc_start_before(unsigned pc, int lines)
{
  int back;
  char t[64];

  if (dc_cur) return pc - 8;                                  /* (the SH-2's instructions are all two bytes) */
  for (back = lines * 6; back >= 2; back -= 2)
  {
    unsigned a = pc - (unsigned)back, p = a;
    int n = 0;

    while (p < pc && n < 64) { p += (unsigned)dc_dis(p, t); n++; }
    if (p == pc && n >= lines / 2) return a;
  }
  return pc;
}

static void dc_refresh(int follow)
{
  if (!dc_win) return;
  if (follow && dc_broken) dc_top = dc_start_before(dc_pc, 4);
  InvalidateRect(dc_win, NULL, FALSE);
}

static void dc_paint(HDC target, const RECT *c)
{
  HDC dc = CreateCompatibleDC(target);
  HBITMAP bmp = CreateCompatibleBitmap(target, c->right, c->bottom), oldbmp;
  HFONT oldfont;
  HBRUSH br;
  RECT dis, regs, stack, bps, r;
  COLORREF bg, fg, dim, strip, accent = theme_accent_bg();
  HDC probe = GetDC(NULL);
  HBRUSH tb = theme_ctlcolor(probe);
  int i, y, lines, n;
  char line[160], text[80];
  unsigned a;

  if (tb) { bg = GetBkColor(probe); fg = GetTextColor(probe); strip = bg; }
  else { bg = GetSysColor(COLOR_WINDOW); fg = GetSysColor(COLOR_WINDOWTEXT); strip = GetSysColor(COLOR_BTNFACE); }
  ReleaseDC(NULL, probe);
  dim = RGB((GetRValue(bg) + GetRValue(fg)) / 2, (GetGValue(bg) + GetGValue(fg)) / 2, (GetBValue(bg) + GetBValue(fg)) / 2);

  oldbmp = (HBITMAP)SelectObject(dc, bmp);
  oldfont = (HFONT)SelectObject(dc, dc_mono);
  SetBkMode(dc, TRANSPARENT);
  br = CreateSolidBrush(bg);
  FillRect(dc, c, br);
  DeleteObject(br);
  br = CreateSolidBrush(strip);
  r = *c; r.bottom = dc_strip_h();
  FillRect(dc, &r, br);
  r = *c; r.top = c->bottom - S(34);
  FillRect(dc, &r, br);
  DeleteObject(br);

  dc_parts(c, &dis, &regs, &stack, &bps);

  /* the strip: what the game is doing */
  {
    int bx = S(8) + S(14), bi;

    for (bi = 0; bi < 6; bi++) bx += (dc_bw[bi] ? dc_bw[bi] : S(96)) + S(6);
    RECT sr;

    sr.left = bx; sr.top = 0; sr.right = c->right - S(8); sr.bottom = dc_strip_h();
    SetTextColor(dc, dc_broken ? accent : fg);
    SelectObject(dc, dc_ui);
    DrawTextA(dc, dc_reason, -1, &sr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, dc_mono);
  }

  /* the disassembly */
  SetTextColor(dc, dim);
  TextOutA(dc, dis.left + S(8), dis.top - dc_lh + S(2), "Disassembly", 11);
  lines = (dis.bottom - dis.top) / dc_lh;
  a = dc_top;
  for (i = 0; i < lines; i++)
  {
    int len = dc_dis(a, text), isbp = 0, k;
    char bytes[40] = "";

    y = dis.top + i * dc_lh;
    for (k = 0; k < dc_nbp; k++) if ((dc_bp[k].type & BP_E) && dc_bp[k].cpu == dc_cur && dc_bp[k].addr == a) isbp = dc_bp[k].enabled ? 1 : 2;
    for (k = 0; k < len / 2 && k < 4; k++) { char w[8]; wsprintfA(w, "%04X ", dc_cur ? dc_rd_sh2(a + (unsigned)k * 2) : dc_rd16(a + (unsigned)k * 2)); lstrcatA(bytes, w); }
    if (a == dc_pc && dc_broken)
    {
      br = CreateSolidBrush(accent);
      r.left = dis.left; r.right = dis.right; r.top = y; r.bottom = y + dc_lh;
      FillRect(dc, &r, br);
      DeleteObject(br);
    }
    else if (dc_have_sel && a == dc_sel)
    {
      br = CreateSolidBrush(RGB((GetRValue(bg) + GetRValue(accent)) / 2, (GetGValue(bg) + GetGValue(accent)) / 2, (GetBValue(bg) + GetBValue(accent)) / 2));
      r.left = dis.left; r.right = dis.right; r.top = y; r.bottom = y + dc_lh;
      FillRect(dc, &r, br);
      DeleteObject(br);
    }
    if (isbp)
    {
      HBRUSH rb = CreateSolidBrush(isbp == 1 ? RGB(220, 40, 40) : RGB(150, 150, 150)), ob = (HBRUSH)SelectObject(dc, rb);
      HPEN np = (HPEN)SelectObject(dc, GetStockObject(NULL_PEN));

      Ellipse(dc, S(6), y + S(2), S(6) + dc_lh - S(4), y + dc_lh - S(2));
      SelectObject(dc, ob); SelectObject(dc, np); DeleteObject(rb);
    }
    if (dc_cur) wsprintfA(line, "%08X  %-8s%s", a, bytes, text); else wsprintfA(line, "%06X  %-20s%s", a, bytes, text);
    SetTextColor(dc, (a == dc_pc && dc_broken) ? RGB(255, 255, 255) : fg);
    TextOutA(dc, S(8) + dc_lh, y + S(1), line, lstrlenA(line));
    a += (unsigned)len;
  }

  /* the registers */
  y = regs.top + S(2);
  SetTextColor(dc, dim);
  TextOutA(dc, regs.left + S(8), y, "Registers", 9);
  y += dc_lh;
  SetTextColor(dc, fg);
  if (dc_32x)
  {
    unsigned rg[24];
    int have = pico_regs(dc_cur ? dc_cur + 1 : 0, rg, 24) > 0;

    if (!have) memset(rg, 0, sizeof(rg));
    if (dc_cur == 0)
    {
      for (i = 0; i < 8; i++)
      {
        wsprintfA(line, "D%d %08X   A%d %08X", i, rg[i], i, rg[8 + i]);
        TextOutA(dc, regs.left + S(8), y, line, lstrlenA(line));
        y += dc_lh;
      }
      wsprintfA(line, "PC %08X   SR %04X %c%c%c%c%c", dc_pcs[0], rg[17], (rg[17] & 16) ? 'X' : '-', (rg[17] & 8) ? 'N' : '-', (rg[17] & 4) ? 'Z' : '-', (rg[17] & 2) ? 'V' : '-', (rg[17] & 1) ? 'C' : '-');
      TextOutA(dc, regs.left + S(8), y, line, lstrlenA(line));
    }
    else
    {
      for (i = 0; i < 8; i++)
      {
        wsprintfA(line, "R%-2d %08X   R%-2d %08X", i, rg[i], i + 8, rg[8 + i]);
        TextOutA(dc, regs.left + S(8), y, line, lstrlenA(line));
        y += dc_lh;
      }
      wsprintfA(line, "PC  %08X   PR  %08X", dc_pcs[dc_cur], rg[17]);
      TextOutA(dc, regs.left + S(8), y, line, lstrlenA(line));
      y += dc_lh;
      wsprintfA(line, "SR  %04X       GBR %08X", rg[18], rg[19]);
      TextOutA(dc, regs.left + S(8), y, line, lstrlenA(line));
      y += dc_lh;
      wsprintfA(line, "VBR %08X   MAC %08X:%08X", rg[20], rg[21], rg[22]);
      TextOutA(dc, regs.left + S(8), y, line, lstrlenA(line));
    }
  }
  else
  {
    unsigned sr;

    for (i = 0; i < 8; i++)
    {
      wsprintfA(line, "D%d %08X   A%d %08X", i, m68k_get_reg((m68k_register_t)(M68K_REG_D0 + i)), i, m68k_get_reg((m68k_register_t)(M68K_REG_A0 + i)));
      TextOutA(dc, regs.left + S(8), y, line, lstrlenA(line));
      y += dc_lh;
    }
    sr = m68k_get_reg(M68K_REG_SR);

    wsprintfA(line, "PC %08X   SR %04X %c%c%c%c%c", m68k_get_reg(M68K_REG_PC), sr, (sr & 16) ? 'X' : '-', (sr & 8) ? 'N' : '-', (sr & 4) ? 'Z' : '-', (sr & 2) ? 'V' : '-', (sr & 1) ? 'C' : '-');
    TextOutA(dc, regs.left + S(8), y, line, lstrlenA(line));
  }

  /* the calls that led here */
  y = stack.top + S(2);
  SetTextColor(dc, dim);
  TextOutA(dc, stack.left + S(8), y, "Calls (most recent first)", 25);
  y += dc_lh;
  SetTextColor(dc, fg);
  n = (stack.bottom - y) / dc_lh;
  for (i = 0; i < n && i < dc_depth; i++)
  {
    wsprintfA(line, dc_cur ? "return to %08X" : "return to %06X", dc_stack[dc_depth - 1 - i].ret);
    TextOutA(dc, stack.left + S(8), y, line, lstrlenA(line));
    y += dc_lh;
  }

  /* the breakpoints */
  y = bps.top + S(2);
  SetTextColor(dc, dim);
  TextOutA(dc, bps.left + S(8), y, "Breakpoints (double-click a line of the code to set one)", 56);
  y += dc_lh;
  for (i = 0; i < dc_nbp && y + dc_lh <= bps.bottom; i++)
  {
    char ty[16] = "";

    if (dc_32x) lstrcatA(ty, dc_bp[i].cpu == 0 ? "68K " : dc_bp[i].cpu == 1 ? "MSH " : "SSH ");
    if (dc_bp[i].type & BP_E) lstrcatA(ty, "X");
    if (dc_bp[i].type & BP_R) lstrcatA(ty, "R");
    if (dc_bp[i].type & BP_W) lstrcatA(ty, "W");
    if (i == dc_selbp)
    {
      br = CreateSolidBrush(accent);
      r.left = bps.left; r.right = bps.right; r.top = y; r.bottom = y + dc_lh;
      FillRect(dc, &r, br);
      DeleteObject(br);
      SetTextColor(dc, RGB(255, 255, 255));
    }
    else SetTextColor(dc, dc_bp[i].enabled ? fg : dim);
    if (dc_bp[i].end > dc_bp[i].addr) wsprintfA(line, dc_bp[i].cpu ? "%c %-8s %08X - %08X" : "%c %-8s %06X - %06X", dc_bp[i].enabled ? '*' : ' ', ty, dc_bp[i].addr, dc_bp[i].end);
    else wsprintfA(line, dc_bp[i].cpu ? "%c %-8s %08X" : "%c %-8s %06X", dc_bp[i].enabled ? '*' : ' ', ty, dc_bp[i].addr);
    TextOutA(dc, bps.left + S(8), y, line, lstrlenA(line));
    y += dc_lh;
  }

  /* the lines between the parts */
  br = CreateSolidBrush(dim);
  r.left = dis.right; r.right = dis.right + 1; r.top = regs.top; r.bottom = bps.bottom;
  FillRect(dc, &r, br);
  r.left = stack.left; r.right = stack.right; r.top = stack.top; r.bottom = stack.top + 1;
  FillRect(dc, &r, br);
  r.top = bps.top; r.bottom = bps.top + 1;
  FillRect(dc, &r, br);
  r.left = 0; r.right = c->right; r.top = dc_strip_h() - 1; r.bottom = dc_strip_h();
  FillRect(dc, &r, br);
  DeleteObject(br);

  BitBlt(target, 0, 0, c->right, c->bottom, dc, 0, 0, SRCCOPY);
  SelectObject(dc, oldfont);
  SelectObject(dc, oldbmp);
  DeleteObject(bmp);
  DeleteDC(dc);
}

/* The instruction at a point of the disassembly, or 0. */
static int dc_line_at(int x, int y, unsigned *addr)
{
  RECT c, dis, regs, stack, bps;
  int row;
  unsigned a;
  char t[64];

  GetClientRect(dc_win, &c);
  dc_parts(&c, &dis, &regs, &stack, &bps);
  if (x < dis.left || x >= dis.right || y < dis.top || y >= dis.bottom) return 0;
  row = (y - dis.top) / dc_lh;
  a = dc_top;
  while (row-- > 0) a += (unsigned)dc_dis(a, t);
  *addr = a;
  return 1;
}

static void dc_toggle_bp(unsigned a)
{
  int i;

  for (i = 0; i < dc_nbp; i++)
    if (dc_bp[i].type == BP_E && dc_bp[i].cpu == dc_cur && dc_bp[i].addr == a)
    {
      memmove(&dc_bp[i], &dc_bp[i + 1], (size_t)(dc_nbp - i - 1) * sizeof(bp_t));
      dc_nbp--;
      return;
    }
  if (dc_nbp < DC_MAXBP)
  {
    dc_bp[dc_nbp].addr = a; dc_bp[dc_nbp].end = a; dc_bp[dc_nbp].type = BP_E; dc_bp[dc_nbp].enabled = 1; dc_bp[dc_nbp].cpu = dc_cur;
    dc_nbp++;
  }
}

/* A modal window for a new breakpoint: an address (or a range), and what stops the game there. */
typedef struct { unsigned addr, end; int type, cpu, done, ok; HWND dlg, a, b, x, r, w, cb; } bpedit_t;

static LRESULT CALLBACK dc_bp_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
  bpedit_t *e = (bpedit_t *)GetWindowLongPtr(h, GWLP_USERDATA);

  if (m == WM_NCCREATE) { e = (bpedit_t *)((CREATESTRUCTA *)lp)->lpCreateParams; SetWindowLongPtr(h, GWLP_USERDATA, (LONG_PTR)e); }
  if (!e) return DefWindowProc(h, m, wp, lp);
  switch (m)
  {
    case WM_COMMAND:
      if (LOWORD(wp) == IDOK)
      {
        char t[24];

        GetWindowTextA(e->a, t, sizeof(t));
        if (!t[0]) { gui_notify("Type the address in hex"); return 0; }
        e->cpu = e->cb ? (int)SendMessage(e->cb, CB_GETCURSEL, 0, 0) : 0;
        if (e->cpu < 0) e->cpu = 0;
        e->addr = (unsigned)strtoul(t, NULL, 16) & (e->cpu ? 0xFFFFFFFFu : 0xFFFFFFu);
        GetWindowTextA(e->b, t, sizeof(t));
        e->end = t[0] ? ((unsigned)strtoul(t, NULL, 16) & (e->cpu ? 0xFFFFFFFFu : 0xFFFFFFu)) : e->addr;
        if (e->end < e->addr) e->end = e->addr;
        e->type = e->cb ? BP_E : ((SendMessage(e->x, BM_GETCHECK, 0, 0) == BST_CHECKED ? BP_E : 0) | (SendMessage(e->r, BM_GETCHECK, 0, 0) == BST_CHECKED ? BP_R : 0) |
                  (SendMessage(e->w, BM_GETCHECK, 0, 0) == BST_CHECKED ? BP_W : 0));
        if (!e->type) { gui_notify("Tick what should stop the game: Execute, Read or Write"); return 0; }
        e->ok = 1; e->done = 1;
        return 0;
      }
      if (LOWORD(wp) == IDCANCEL) { e->done = 1; return 0; }
      break;
    case WM_CLOSE:
      e->done = 1;
      return 0;
    case WM_ERASEBKGND:                                           /* the window's own background: dark in the dark style */
    {
      HBRUSH bg = theme_ctlcolor((HDC)wp);
      RECT cr;

      if (!bg) break;
      GetClientRect(h, &cr);
      FillRect((HDC)wp, &cr, bg);
      return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG:
    {
      HBRUSH b = theme_ctlcolor_ctl((HDC)wp, (HWND)lp);

      if (b) return (LRESULT)b;
      if (m == WM_CTLCOLORSTATIC || m == WM_CTLCOLORDLG) return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
      break;
    }
  }
  return DefWindowProc(h, m, wp, lp);
}

static HWND dc_ctl(HWND p, const char *cls, const char *text, DWORD style, DWORD ex, int x, int y, int w, int h, int id)
{
  HWND c = CreateWindowExA(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), p, (HMENU)(INT_PTR)id, g_inst, NULL);

  if (c) SendMessage(c, WM_SETFONT, (WPARAM)dc_ui, TRUE);
  return c;
}

static void dc_add_bp(void)
{
  static int registered;
  bpedit_t e;
  MSG msg;
  RECT r, pr;
  DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
  char t[24];

  if (dc_nbp >= DC_MAXBP) { gui_notify("That is the most breakpoints"); return; }
  if (!registered)
  {
    WNDCLASSA wc;

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = dc_bp_proc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "GPEXBreakpointEdit"; i18n_keep_ltr("GPEXBreakpointEdit");
    if (!RegisterClassA(&wc)) return;
    registered = 1;
  }
  ZeroMemory(&e, sizeof(e));
  r.left = 0; r.top = 0; r.right = S(300); r.bottom = S(176);
  AdjustWindowRectEx(&r, style, FALSE, 0);
  e.dlg = CreateWindowExA(0, "GPEXBreakpointEdit", "New Breakpoint", style, 0, 0, r.right - r.left, r.bottom - r.top, GetAncestor(dc_win, GA_ROOT), NULL, g_inst, &e);
  if (!e.dlg) return;
  dc_ctl(e.dlg, "STATIC", "Address (hex):", SS_LEFT, 0, 12, 16, 100, 16, 0);
  wsprintfA(t, "%X", dc_have_sel ? dc_sel : dc_pc);
  e.a = dc_ctl(e.dlg, "EDIT", t, WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, 120, 12, 100, 22, 201);
  dc_ctl(e.dlg, "STATIC", "To (a range, hex):", SS_LEFT, 0, 12, 46, 104, 16, 0);
  e.b = dc_ctl(e.dlg, "EDIT", "", WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, 120, 42, 100, 22, 202);
  if (dc_32x)                                                  /* which processor (the instruction reached is all it can stop at) */
  {
    dc_ctl(e.dlg, "STATIC", "Processor:", SS_LEFT, 0, 12, 82, 100, 16, 0);
    e.cb = dc_ctl(e.dlg, "COMBOBOX", "", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 0, 120, 78, 150, 120, 206);
    SendMessageA(e.cb, CB_ADDSTRING, 0, (LPARAM)"68000");
    SendMessageA(e.cb, CB_ADDSTRING, 0, (LPARAM)"Master SH-2");
    SendMessageA(e.cb, CB_ADDSTRING, 0, (LPARAM)"Slave SH-2");
    SendMessage(e.cb, CB_SETCURSEL, (WPARAM)dc_cur, 0);
  }
  else
  {
    e.x = dc_ctl(e.dlg, "BUTTON", "Execute", WS_TABSTOP | BS_AUTOCHECKBOX, 0, 16, 78, 80, 20, 203);
    e.r = dc_ctl(e.dlg, "BUTTON", "Read", WS_TABSTOP | BS_AUTOCHECKBOX, 0, 104, 78, 70, 20, 204);
    e.w = dc_ctl(e.dlg, "BUTTON", "Write", WS_TABSTOP | BS_AUTOCHECKBOX, 0, 182, 78, 70, 20, 205);
    SendMessage(e.x, BM_SETCHECK, BST_CHECKED, 0);
  }
  dc_ctl(e.dlg, "BUTTON", "OK", WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 120, 132, 80, 26, IDOK);
  dc_ctl(e.dlg, "BUTTON", "Cancel", WS_TABSTOP | BS_PUSHBUTTON, 0, 208, 132, 80, 26, IDCANCEL);
  GetWindowRect(dc_win, &pr);
  SetWindowPos(e.dlg, NULL, pr.left + ((pr.right - pr.left) - (r.right - r.left)) / 2, pr.top + ((pr.bottom - pr.top) - (r.bottom - r.top)) / 2,
               r.right - r.left, r.bottom - r.top, SWP_NOZORDER);
  theme_apply_to_window(e.dlg);
  {
    static const GuiTip tips[] =
    {
      { 201, NULL, "The address, in hex. For an instruction, the address of its first byte." },
      { 202, NULL, "The end of a range, in hex (leave empty for one address)." },
      { 203, NULL, "Stop the game when the processor reaches the instruction." },
      { 204, NULL, "Stop the game when the 68000 reads from the address." },
      { 205, NULL, "Stop the game when the 68000 writes to the address." },
      { 206, NULL, "Which processor the breakpoint is for." },
      { IDOK, NULL, "Add the breakpoint." },
      { IDCANCEL, NULL, "Close without adding anything." }
    };

    gui_dialog_tips(e.dlg, tips, (int)(sizeof(tips) / sizeof(tips[0])));
  }
  gui_window_icon(e.dlg);

  EnableWindow(GetAncestor(dc_win, GA_ROOT), FALSE);
  ShowWindow(e.dlg, SW_SHOW);
  SetFocus(e.a);
  SendMessage(e.a, EM_SETSEL, 0, -1);
  while (!e.done && GetMessage(&msg, NULL, 0, 0))
  {
    if (msg.message == WM_KEYDOWN && (msg.wParam == VK_RETURN || msg.wParam == VK_ESCAPE) && IsChild(e.dlg, msg.hwnd))
    {
      char cls[16];

      GetClassNameA(msg.hwnd, cls, sizeof(cls));
      if (lstrcmpiA(cls, "BUTTON")) { SendMessage(e.dlg, WM_COMMAND, msg.wParam == VK_RETURN ? IDOK : IDCANCEL, 0); continue; }
    }
    if (!IsDialogMessage(e.dlg, &msg)) { TranslateMessage(&msg); DispatchMessage(&msg); }
  }
  if (msg.message == WM_QUIT && !e.done) PostQuitMessage((int)msg.wParam);
  EnableWindow(GetAncestor(dc_win, GA_ROOT), TRUE);
  DestroyWindow(e.dlg);
  SetForegroundWindow(GetAncestor(dc_win, GA_ROOT));
  if (e.ok)
  {
    dc_bp[dc_nbp].addr = e.addr; dc_bp[dc_nbp].end = e.end; dc_bp[dc_nbp].type = e.type; dc_bp[dc_nbp].enabled = 1; dc_bp[dc_nbp].cpu = e.cpu;
    dc_selbp = dc_nbp++;
    InvalidateRect(dc_win, NULL, FALSE);
  }
}

static LRESULT CALLBACK dc_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
  int i;

  switch (m)
  {
    case WM_CREATE:
      dc_win = h;
      dc_fonts();
      dc_look = dbg_look();
      for (i = 0; i < IDC_DC_BTNS; i++)
      {
        static const char *const names[IDC_DC_BTNS] = { "", "", "", "", "", "", "Go", "Add...", "Remove", "Enable / Disable" };

        dc_btn[i] = CreateWindowExA(0, "BUTTON", names[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10, h,
                                    (HMENU)(INT_PTR)(i < 6 ? IDC_DC_CONT + i : i == 6 ? IDC_DC_GO : i == 7 ? IDC_DC_ADD : i == 8 ? IDC_DC_DEL : IDC_DC_TOG), g_inst, NULL);
      }
      dc_addr = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_DC_ADDR, g_inst, NULL);
      dc_cpucb = NULL;
      dc_np = netplay_active();
      if (dc_32x)
      {
        unsigned rg[24];

        dc_cpucb = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_DC_CPU, g_inst, NULL);
        SendMessageA(dc_cpucb, CB_ADDSTRING, 0, (LPARAM)"68000");
        SendMessageA(dc_cpucb, CB_ADDSTRING, 0, (LPARAM)"Master SH-2");
        SendMessageA(dc_cpucb, CB_ADDSTRING, 0, (LPARAM)"Slave SH-2");
        SendMessage(dc_cpucb, CB_SETCURSEL, 0, 0);
        for (i = 0; i < 3; i++) if (pico_regs(i ? i + 1 : 0, rg, 24) > 0) dc_pcs[i] = rg[16];
      }
      theme_apply_to_window(h);
      {
        static const GuiTip tips[] =
        {
          { IDC_DC_CONT, NULL, "Let the game go on (F5)." },
          { IDC_DC_BREAK, NULL, "Stop the game at the next instruction." },
          { IDC_DC_INTO, NULL, "Run one instruction; go into a call (F7)." },
          { IDC_DC_OVER, NULL, "Run one instruction; a call is run as a whole (F8)." },
          { IDC_DC_OUT, NULL, "Run until the current call returns." },
          { IDC_DC_TO, NULL, "Run until the line picked in the code is reached (F4)." },
          { IDC_DC_ADDR, NULL, "An address in hex to show the code of. Press Enter to go." },
          { IDC_DC_GO, NULL, "Show the code at the address typed." },
          { IDC_DC_ADD, NULL, "Make a breakpoint: an address or a range, and what stops the game there." },
          { IDC_DC_DEL, NULL, "Remove the breakpoint picked in the list (Delete)." },
          { IDC_DC_TOG, NULL, "Turn the picked breakpoint off or on without removing it." },
          { IDC_DC_CPU, NULL, "Which processor's code, registers and calls are shown (a 32X game has three)." }
        };

        gui_dialog_tips(h, tips, (int)(sizeof(tips) / sizeof(tips[0])));
      }
      dc_layout();
      if (!dc_32x) dc_pc = m68k_get_reg(M68K_REG_PC);
      dc_top = dc_pc;
      dc_set_buttons();
      SetTimer(h, DC_TIMER, DC_TICK_MS, NULL);
      if (dc_32x) pico_cpu_hook(dc_pico_hook); else set_cpu_hook(dc_hook);
      return 0;

    case WM_SIZE:
      dc_layout();
      InvalidateRect(h, NULL, FALSE);
      return 0;

    case WM_GETMINMAXINFO:
    {
      MINMAXINFO *mi = (MINMAXINFO *)lp;

      mi->ptMinTrackSize.x = S(720);
      mi->ptMinTrackSize.y = S(420);
      return 0;
    }

    case WM_ERASEBKGND:
      return 1;

    case WM_PAINT:
    {
      PAINTSTRUCT ps;
      RECT rc;
      HDC dc = BeginPaint(h, &ps);

      GetClientRect(h, &rc);
      dc_paint(dc, &rc);
      EndPaint(h, &ps);
      return 0;
    }

    case WM_TIMER:
      if (wp == DC_TIMER)
      {
        if (dbg_look() != dc_look) { dc_look = dbg_look(); dc_fonts(); dc_layout(); InvalidateRect(h, NULL, FALSE); }
        dc_np = netplay_active();
        if (!dc_broken) { if (!dc_32x) dc_pc = m68k_get_reg(M68K_REG_PC); InvalidateRect(h, NULL, FALSE); }
      }
      return 0;

    case WM_LBUTTONDOWN:
    {
      unsigned a;
      RECT c, dis, regs, stack, bps;
      int x = (short)LOWORD(lp), y = (short)HIWORD(lp);

      SetFocus(h);
      if (dc_line_at(x, y, &a)) { dc_sel = a; dc_have_sel = 1; InvalidateRect(h, NULL, FALSE); return 0; }
      GetClientRect(h, &c);
      dc_parts(&c, &dis, &regs, &stack, &bps);
      if (x >= bps.left && x < bps.right && y >= bps.top + dc_lh && y < bps.bottom)
      {
        int k = (y - bps.top - dc_lh) / dc_lh;

        dc_selbp = k < dc_nbp ? k : -1;
        InvalidateRect(h, NULL, FALSE);
      }
      return 0;
    }

    case WM_LBUTTONDBLCLK:
    {
      unsigned a;

      if (dc_line_at((short)LOWORD(lp), (short)HIWORD(lp), &a)) { dc_toggle_bp(a); InvalidateRect(h, NULL, FALSE); }
      return 0;
    }

    case WM_MOUSEWHEEL:
    {
      char t[64];
      int steps = (GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA) * 3;

      while (steps > 0) { dc_top -= 2; steps--; }                          /* up: back by words (the instructions before are not known) */
      while (steps < 0) { dc_top += (unsigned)dc_dis(dc_top, t); steps++; }
      InvalidateRect(h, NULL, FALSE);
      return 0;
    }

    case WM_KEYDOWN:
      switch (wp)
      {
        case VK_F5: if (dc_broken) dc_go(IDC_DC_CONT); else dc_pause_req = 1; return 0;
        case VK_F7: if (dc_broken) dc_go(IDC_DC_INTO); return 0;
        case VK_F8: if (dc_broken) dc_go(IDC_DC_OVER); return 0;
        case VK_F9: if (dc_have_sel) { dc_toggle_bp(dc_sel); InvalidateRect(h, NULL, FALSE); } return 0;
        case VK_F4: if (dc_broken) dc_go(IDC_DC_TO); return 0;
        case VK_DELETE:
          if (dc_selbp >= 0 && dc_selbp < dc_nbp)
          {
            memmove(&dc_bp[dc_selbp], &dc_bp[dc_selbp + 1], (size_t)(dc_nbp - dc_selbp - 1) * sizeof(bp_t));
            dc_nbp--; dc_selbp = -1;
            InvalidateRect(h, NULL, FALSE);
          }
          return 0;
        case VK_ESCAPE: if (!dc_broken) gui_debug_close(h); return 0;
      }
      return 0;

    case WM_COMMAND:
      switch (LOWORD(wp))
      {
        case IDC_DC_CONT: case IDC_DC_INTO: case IDC_DC_OVER: case IDC_DC_OUT: case IDC_DC_TO:
          if (dc_broken) dc_go(LOWORD(wp));
          return 0;
        case IDC_DC_BREAK:
          dc_pause_req = 1;
          lstrcpynA(dc_reason, "Stopping at the next instruction...", sizeof(dc_reason));
          InvalidateRect(h, NULL, FALSE);
          return 0;
        case IDC_DC_GO:
        {
          char t[24];

          GetWindowTextA(dc_addr, t, sizeof(t));
          if (t[0]) { dc_top = (unsigned)strtoul(t, NULL, 16) & (dc_cur ? 0xFFFFFFFEu : 0xFFFFFEu); dc_sel = dc_top; dc_have_sel = 1; InvalidateRect(h, NULL, FALSE); }
          return 0;
        }
        case IDC_DC_CPU:
          if (HIWORD(wp) == CBN_SELCHANGE)
          {
            int k = (int)SendMessage(dc_cpucb, CB_GETCURSEL, 0, 0);

            if (k >= 0 && k < 3) { dc_cur = k; dc_have_sel = 0; dc_top = dc_start_before(dc_pc, 4); InvalidateRect(h, NULL, FALSE); }
          }
          return 0;
        case IDC_DC_ADD: dc_add_bp(); return 0;
        case IDC_DC_DEL:
          if (dc_selbp >= 0 && dc_selbp < dc_nbp)
          {
            memmove(&dc_bp[dc_selbp], &dc_bp[dc_selbp + 1], (size_t)(dc_nbp - dc_selbp - 1) * sizeof(bp_t));
            dc_nbp--; dc_selbp = -1;
            InvalidateRect(h, NULL, FALSE);
          }
          return 0;
        case IDC_DC_TOG:
          if (dc_selbp >= 0 && dc_selbp < dc_nbp) { dc_bp[dc_selbp].enabled = !dc_bp[dc_selbp].enabled; InvalidateRect(h, NULL, FALSE); }
          return 0;
      }
      break;

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN:
    {
      HBRUSH b = theme_ctlcolor_ctl((HDC)wp, (HWND)lp);

      if (b) return (LRESULT)b;
      break;
    }

    case WM_CLOSE:
      if (dc_broken) { gui_notify("Let the game go on first (Continue)"); return 0; }
      DestroyWindow(h);
      return 0;

    case WM_DESTROY:
      KillTimer(h, DC_TIMER);
      if (dc_32x) pico_cpu_hook(NULL); else set_cpu_hook(NULL);
      dc_broken = 0;
      dc_pause_req = 0;
      dc_mode = M_RUN;
      dc_win = NULL;
      dc_cpucb = NULL;
      return 0;
  }

  return DefWindowProc(h, m, wp, lp);
}

/****************************************************************************
 * The rest of the program
 ****************************************************************************/

void dlg_cpu_debugger(HWND parent)
{
  static int registered;

  if (dc_win)
  {
    HWND top = GetAncestor(dc_win, GA_ROOT);

    if (IsIconic(top)) ShowWindow(top, SW_RESTORE);
    SetForegroundWindow(top);
    return;
  }
  if (pico_active())
  {
    if (!pico_cpu_hook(NULL))
    {
      gui_notify("This 32X library cannot be debugged: use the Plugins from the newest zip (a library built with the SH-2 recompiler, PICODRC=1, cannot be debugged either)");
      return;
    }
    dc_mem[0] = pico_memory(0, &dc_memn[0]);
    dc_mem[1] = pico_memory(1, &dc_memn[1]);
    dc_mem[2] = pico_memory(2, &dc_memn[2]);
    dc_32x = 1;
  }
  else if ((system_hw & SYSTEM_PBC) != SYSTEM_MD && system_hw != SYSTEM_MCD)
  {
    gui_notify("The CPU Debugger is for games with a 68000 processor");
    return;
  }
  if (netplay_active())
  {
    gui_notify("The CPU Debugger is not available during netplay");
    return;
  }
  if (!registered)
  {
    WNDCLASSA wc;

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = dc_proc;
    wc.style = CS_DBLCLKS;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = DC_CLASS; i18n_keep_ltr(DC_CLASS);
    if (!RegisterClassA(&wc)) return;
    registered = 1;
  }
  if (!pico_active()) dc_32x = 0;
  dc_fonts();
  memset(dc_dep, 0, sizeof(dc_dep));
  memset(dc_pcs, 0, sizeof(dc_pcs));
  dc_cur = dc_stopcpu = dc_stepcpu = 0;
  if (gui_debug_host)                                       /* a pane of the Debugger window */
  {
    CreateWindowExA(0, DC_CLASS, "CPU Debugger", WS_CHILD | WS_CLIPCHILDREN, 0, 0, S(860), S(560), gui_debug_host, NULL, g_inst, NULL);
    gui_debug_last = dc_win;
    return;
  }
  CreateWindowExA(0, DC_CLASS, "CPU Debugger", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, S(860), S(560), parent, NULL, g_inst, NULL);
  if (dc_win)
  {
    gui_center_window(dc_win);
    gui_window_icon(dc_win);
    ShowWindow(dc_win, SW_SHOW);
  }
}

int dbgcpu_message(MSG *msg)
{
  if (!dc_win || !msg->hwnd) return 0;
  if (msg->hwnd != dc_win && !IsChild(dc_win, msg->hwnd)) return 0;
  if (msg->message == WM_KEYDOWN && (msg->wParam == VK_RETURN) && msg->hwnd == dc_addr) { SendMessage(dc_win, WM_COMMAND, IDC_DC_GO, 0); return 1; }
  TranslateMessage(msg);
  DispatchMessage(msg);
  return 1;
}

void dbgcpu_close(void)
{
  if (dc_win)
  {
    dc_broken = 0;
    DestroyWindow(dc_win);
  }
  dc_nbp = 0;
  dc_selbp = -1;
  dc_have_sel = 0;
  memset(dc_dep, 0, sizeof(dc_dep));
  dc_cur = 0;
}

void dbgcpu_enable(int on)
{
  if (dc_win) EnableWindow(dc_win, on ? TRUE : FALSE);
}

/* Lets a game that is stopped at a breakpoint go on (the window is about to close); the breakpoints stay. */
void dbgcpu_release(void)
{
  dc_broken = 0;
}
