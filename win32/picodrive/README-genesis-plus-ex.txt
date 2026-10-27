PicoDrive in Genesis Plus EX
============================

This folder is PicoDrive (https://github.com/notaz/picodrive; see AUTHORS and COPYING) as it is used by
Genesis Plus EX to run Sega 32X games, which Genesis Plus GX cannot do. It is built as a libretro core,
picodrive_libretro.dll, with the Makefile.win32 of the folder above (make -f Makefile.win32, or only the DLL:
make -f Makefile.win32 picodrive). The program (picocore.c) loads it only when a 32X game is opened.

What is different from the PicoDrive source this was taken from (the changes COPYING asks to be named):

  * Only what a Windows libretro build needs was kept: pico/, cpu/ (without Cyclone and DrZ80, which are
    ARM-only), zlib/, unzip/, platform/common, platform/libretro (libretro.c, the core options and
    libretro-common) and the makefiles. The other platforms were left out.

  * Four parts that PicoDrive pulls in as git submodules were not in that source, and none of them is needed
    to run 32X games, so they are replaced or left out:
      - emu2413 (the Master System FM unit): pico/sound/emu2413/emu2413.h and pico/sound/ym2413.c are empty
        stand-ins, so the YM2413 makes no sound in this build (Genesis Plus GX runs the Master System).
      - libchdr (CHD discs, Sega CD): the build is made with use_libchdr=0.
      - dr_libs (MP3 for Sega CD music): Makefile uses platform/common/mp3_dummy.o instead of mp3_drmp3.o.
      - libpicofe: platform/libretro/libretro.c defines the one macro it took from it, PXMAKE.

  * The SH-2 dynamic recompiler is off in the default build (use_sh2drc=0); the interpreter is used. Build with
    PICODRC=1 to turn it on.

Sega CD, Master System and Pico games are not played through this build: Genesis Plus GX plays those.

  * pico/32x/pwm.c: a loud thump at the start of 32X games (the PWM is switched on with the lowest output level, a step
    of about -32768 that the output filter in mix.c answers with a thump) is taken out: during the first ten seconds
    after power on or a reset, a jump of more than 12000 between two PWM samples after the output had been flat for
    48 samples is taken as a change of level, not as sound. See the comment at the top of the file. A call to
    p32x_pwm_declick_reset() was added to PicoPower32x() in pico/32x/32x.c. The same is done a second time on the sum
    of everything that is played (a jump of more than 20000 in one sample): p32x_pwm_declick_mix() is called from
    PsndRender() in pico/sound/sound.c, declared in pico/pico_int.h.


Sound levels: gpex_set_levels(fm_pct, psg_pct) scales the YM2612 (with its DAC) and the PSG output in percent (100 = unchanged). The program
calls it every frame with the FM and PSG levels of Levels and Latency (the PSG's 150% of the program is 100% here). pico/sound/sound.c
and sn76496.c apply them; PWM is not touched.
