RUN-AHEAD TESTS (Linux)
=======================

sh run_tests.sh      needs gcc, zlib and python3 (about a minute to build); nothing here changes the core (core/ and sdl/).

What it checks
  Run-Ahead runs the game a frame ahead and then goes back with a saved state. The sound that plays is the frame from ahead, so it has to be the
  sound a normal run plays one frame later, sample for sample (win32/raudio.c keeps the sound chain unbroken across the state loads).
  core_test.c does the same steps as win32/main.c on small test ROMs (made by make_test_roms.py) and writes the sound of every frame;
  compare.py checks the two runs against each other (a few units of difference are allowed: 8 of 32767).

  - the PSG, the FM chip (YM2612 and the Nuked chip, both FM quality settings, High-Quality PSG on and off);
  - the low-pass filter and the 3-band EQ (Settings > Audio: they keep their state across the loads);
  - a sample played by hand in a tight 68000 loop (the SEGA chime kind: it depends on the 68000's timing);
  - a Game Gear (Z80) game with the sound written from the interrupt, from a loop and by polling the vertical blank;
  - the Mega CD's PCM chip (pcm_test.c drives the chip directly: no disc or BIOS needed), and that this test fails without the chain restore.
  It ends with a table from the input lag test ROM (make_input_lag_rom.py): the frame in which the screen flashes after a button press, without
  Run-Ahead and with 1 Frame.

Not covered: the disc music (CD-DA) chain of the Mega CD (it needs a disc), the 32X (PicoDrive runs in its own library), 2 Frames.
