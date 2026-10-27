/* Genesis Plus EX: stand-in for PicoDrive's emu2413 wrapper (the Master System FM unit is not part of this build). */

#include "emu2413/emu2413.h"
#include "ym2413.h"

OPLL *opll = NULL;

void YM2413_regWrite(unsigned data) { (void)data; }
void YM2413_dataWrite(unsigned data) { (void)data; }

size_t ym2413_pack_state(void *buf_, size_t size) { (void)buf_; (void)size; return 0; }
void ym2413_unpack_state(const void *buf_, size_t size) { (void)buf_; (void)size; }
