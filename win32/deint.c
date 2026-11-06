/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  deint.c -- Video > Visual Effects > Deinterlacing.
 *
 *  A game in the 448-line interlaced mode shows only one field of the picture in each frame: the lines of the other field are
 *  from the frame before, so anything that moves is drawn twice, a line apart, with comb teeth. With this switched on the
 *  graphics processor draws both fields from the same moment in every frame, so every frame is a whole picture of 448 lines
 *  and nothing is combed.
 *
 *  The core is not changed: render_line() is wrapped (see --wrap in the Makefile). Before the line is drawn as the core does,
 *  it is drawn once more as the other field, with the sprites of that field looked for.
 ****************************************************************************/

#include <windows.h>
#include "shared.h"
#include "gui.h"
#include "picocore.h"

extern void __real_render_line(int line);

void __wrap_render_line(int line)
{
  picopcm_line(line);                                       /* (a Sega Pico game's voices: run every 8 lines) */
  if (gui.deinterlace && interlaced && config.render && emu_running && !pico_active() &&
      (((system_hw & SYSTEM_PBC) == SYSTEM_MD) || system_hw == SYSTEM_MCD))
  {
    uint8 field = odd_frame;
    uint16 flags = status;                                  /* (the status bits the extra drawing may set: sprite overflow, collision) */

    odd_frame = field ^ 1;
    if (reg[1] & 0x40) parse_satb(line - 1);                /* the sprites on this line, as that field sees them */
    __real_render_line(line);                               /* the other field's line goes into its place of the picture */
    odd_frame = field;
    status = flags;
    if (reg[1] & 0x40) parse_satb(line - 1);                /* and the list of this field's own is made again */
  }
  __real_render_line(line);
}
