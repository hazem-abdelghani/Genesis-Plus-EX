/* Genesis Plus EX: the Master System FM unit (YM2413) is not part of this build.
   PicoDrive is only used here for 32X games, and the Master System is run by Genesis Plus GX; the emu2413 library
   (a git submodule of PicoDrive that is not in the source this was made from) is replaced by this empty stand-in. */
#ifndef EMU2413_STUB_H
#define EMU2413_STUB_H

#include <stdint.h>

typedef struct OPLL { uint32_t clk, rate; } OPLL;

static inline OPLL *OPLL_new(uint32_t clk, uint32_t rate)
{
	static OPLL one;
	one.clk = clk;
	one.rate = rate;
	return &one;
}
static inline void OPLL_delete(OPLL *o) { (void)o; }
static inline void OPLL_reset(OPLL *o) { (void)o; }
static inline void OPLL_setChipType(OPLL *o, uint8_t t) { (void)o; (void)t; }
static inline void OPLL_setRate(OPLL *o, uint32_t rate) { o->rate = rate; }
static inline int16_t OPLL_calc(OPLL *o) { (void)o; return 0; }
static inline void OPLL_writeIO(OPLL *o, uint32_t reg, uint8_t val) { (void)o; (void)reg; (void)val; }

#endif
