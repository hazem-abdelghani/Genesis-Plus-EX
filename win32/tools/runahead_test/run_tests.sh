#!/bin/sh
# Run-Ahead checks on the core, on Linux: sh run_tests.sh  (needs gcc, zlib and python3; takes about a minute to build)
# Builds the core with the test programs, makes the test ROMs, and checks that the sound Run-Ahead plays is the sound a normal run plays one frame
# later, for the PSG, the FM chip (both FM quality settings and the Nuked chip), the filters, a sample played by hand, a Game Gear game, and the
# Mega CD's PCM chip. Then it shows the delay table of the input lag test ROM. Nothing here changes the core.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
W=${TMPDIR:-/tmp}/gpex_runahead_test
rm -rf "$W"; mkdir -p "$W"; cd "$W"
cp "$HERE"/*.py "$HERE"/*.c .
python3 make_test_roms.py >/dev/null
cat > osd.h <<'H'
#ifndef _OSD_H_
#define _OSD_H_
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include "main.h"
#include "config.h"
#include "error.h"
#include "unzip.h"
#include "fileio.h"
#define osd_input_update sdl_input_update
#define GG_ROM "./ggenie.bin"
#define AR_ROM "./areplay.bin"
#define SK_ROM "./sk.bin"
#define SK_UPMEM "./sk2chip.bin"
#define CD_BIOS_US "./bios_CD_U.bin"
#define CD_BIOS_EU "./bios_CD_E.bin"
#define CD_BIOS_JP "./bios_CD_J.bin"
#define MD_BIOS "./bios_MD.bin"
#define MS_BIOS_US "./bios_U.sms"
#define MS_BIOS_EU "./bios_E.sms"
#define MS_BIOS_JP "./bios_J.sms"
#define GG_BIOS "./bios.gg"
#endif
H
cat > main.h <<'H'
#ifndef _MAIN_H_
#define _MAIN_H_
#define MAX_INPUTS 8
extern int debug_on;
extern int log_error;
extern int sdl_input_update(void);
#endif
H
SRC=$(cd "$ROOT" && find core -name "*.c" | grep -v "tremor\|libchdr\|minimp3\|libFLAC\|zstd\|lzma\|7z\|cpuhook" | sed "s#^#$ROOT/#")
INC="-I$W -I$ROOT/core -I$ROOT/core/z80 -I$ROOT/core/m68k -I$ROOT/core/sound -I$ROOT/core/sound/minimp3 -I$ROOT/core/sound/tremor -I$ROOT/core/input_hw -I$ROOT/core/cart_hw -I$ROOT/core/cart_hw/svp -I$ROOT/core/cd_hw -I$ROOT/core/ntsc -I$ROOT/sdl"
DEF="-DLSB_FIRST -DUSE_16BPP_RENDERING -DHAVE_YM3438_CORE -DHAVE_OPLL_CORE -DM68K_OVERCLOCK_SHIFT=20 -DMAXROMSIZE=100663296"
WRAP="-Wl,--wrap=sound_reset -Wl,--wrap=blip_add_delta -Wl,--wrap=blip_add_delta_fast -Wl,--wrap=psg_end_frame -Wl,--wrap=sound_update"
COMMON="$ROOT/sdl/error.c $ROOT/sdl/fileio.c $ROOT/sdl/unzip.c $ROOT/sdl/config.c $ROOT/win32/raudio.c"
gcc -O1 -w -fcommon $DEF $INC $SRC $COMMON core_test.c $WRAP -Wl,--wrap=system_reset -o core_test -lz -lm
gcc -O1 -w -fcommon $DEF $INC $SRC $COMMON pcm_test.c $WRAP -o pcm_test -lz -lm
FAIL=0
check() {   # name, command that makes rec0/rec1
  if sh -c "$2" >/dev/null 2>&1 && python3 compare.py >/tmp/gpex_ra_line 2>&1; then echo "PASS  $1: $(cat /tmp/gpex_ra_line)"; else echo "FAIL  $1: $(cat /tmp/gpex_ra_line 2>/dev/null)"; FAIL=1; fi
}
for rom in test_snd.bin test_fm.bin test_both.bin test_vint.bin test_dac.bin test.gg test_loop.gg test_poll.gg; do
  check "$rom" "./core_test 0 $rom && ./core_test 1 $rom"
done
for opt in "HQ=1" "NUKED=1" "NUKED=1 HQ=1" "FILTER=1" "FILTER=2" "NUKED=1 FILTER=1" "NUKED=1 FILTER=2" "HQPSG=0"; do
  check "test_both.bin with $opt" "env $opt ./core_test 0 test_both.bin && env $opt ./core_test 1 test_both.bin"
  check "test_dac.bin with $opt" "env $opt ./core_test 0 test_dac.bin && env $opt ./core_test 1 test_dac.bin"
done
check "Mega CD PCM chip" "./pcm_test 0 && ./pcm_test 1"
if NORESTORE=1 sh -c "./pcm_test 0 && ./pcm_test 1" >/dev/null 2>&1 && ! python3 compare.py >/dev/null 2>&1; then echo "PASS  the PCM test fails without the chain restore (so it can fail)"; else echo "FAIL  the PCM test did not fail without the chain restore"; FAIL=1; fi
echo
echo "Input lag test ROM: frame in which the flash shows when A goes down at frame 20 (normal run / Run-Ahead 1 frame)"
for d in 0 1 2 3; do
  S=$(python3 -c "print(','.join(str(2+2*i) for i in range($d)) or '99')")
  N=$(PIX=1 PRESS=20 START_LIST=$S ./core_test 0 input_lag_test.md | awk '/frame/ && $3+0>=18 && $7!="0000" {print $3+0; exit}' | tr -d ':')
  R=$(PIX=1 PRESS=20 START_LIST=$S ./core_test 1 input_lag_test.md | awk '/frame/ && $3+0>=18 && $7!="0000" {print $3+0; exit}' | tr -d ':')
  echo "  ROM delay $d: $N / $R"
done
exit $FAIL
