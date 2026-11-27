This directory holds an unmodified subset of the 7-Zip / LZMA SDK ("C" folder),
version 24.05, by Igor Pavlov: https://github.com/ip7z/7zip

The SDK is public domain (see the header comment of every file here); no
attribution is legally required, but this note documents where the code
came from and that it hasn't been altered, for anyone maintaining this port.

Only the .7z-container-specific files live here: 7zArcIn.c (archive header
parsing), 7zDec.c (folder decoding), 7zBuf.c/7zCrc.c/7zCrcOpt.c/7zFile.c/
7zStream.c (small helpers), Bcj2.c (one of the coder methods 7zDec.c can
dispatch to), and Lzma2Dec.c (LZMA2 framing over the LZMA decoder). The raw
LZMA decoder itself (LzmaDec.c), CpuArch.c and their headers are NOT
duplicated here -- this port already builds those, at the same SDK version,
for CHD (Sega CD compressed disc image) support in
core/cd_hw/libchdr/deps/lzma-24.05, and linking a second copy would redefine
the same LzmaDec_*/CpuArch_* symbols. Makefile.win32 points this directory's
#include "LzmaDec.h" / "CpuArch.h" / "Alloc.h" at that existing include path
instead. This is also why 7z archive support only builds with CHD=1 (the
Makefile default).

Nothing needed to *create* a .7z archive is included. The Delta and branch (BCJ x86, ARM, ARM64, PPC, IA64, SPARC, RISC-V) filters are decoded
too: their code comes from the same 24.05 SDK that CHD support builds (Bra.c, Bra86.c, BraIA64.c, Delta.c in
core/cd_hw/libchdr/deps/lzma-24.05/src). 7-Zip chooses the Delta filter by itself for WAV files, so an archive of audio (an MD+ game)
needs it. PPMd support is left off the way upstream already supports turning it off: Z7_PPMD_SUPPORT is simply never defined,
so Ppmd7.h is never included and no PPMd code is compiled in.

See win32/sevenzip.c for the small wrapper this port actually calls, and its
own tiny malloc/free-based ISzAlloc -- the SDK's own g_Alloc (Alloc.c, also
not duplicated here) adds large-page and aligned-allocation paths this
read-only use has no need for.
