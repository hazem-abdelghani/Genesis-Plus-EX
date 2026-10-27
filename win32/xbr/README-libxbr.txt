xBR and HQ2x / HQ3x / HQ4x render filters
=========================================

This folder holds the "libxbr-standalone" library by Treeki
(https://github.com/Treeki/libxbr-standalone), which merges two pixel art scalers:

  xBR     by Hyllian (Hyllian/Jararaca), through FFmpeg's libavfilter/vf_xbr.c
          Copyright (c) 2011, 2012 Hyllian/Jararaca, (c) 2014 Arwa Arif
  hqx     by Maxim Stepin (2003) and Cameron Zemek (2010), from the hqx project
          (https://code.google.com/p/hqx/)

Licensed under the GNU Lesser General Public License, version 2.1 or later (see the
headers of the files). The library works on 32-bit pixels; Genesis Plus EX converts
each RGB565 frame to 32 bits for it and back.

Changes made for Genesis Plus EX:
  - the header is called libxbr.h (the original name clashes with filters.h);
  - the color table that ranks how different two colors look has 65536 entries,
    one for each RGB565 color, instead of 16.7 million (64 MB): xbr_key() makes the
    index, and xbr_init_data() fills the table with the same formula as before.
