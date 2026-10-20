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

Nothing needed to *create* a .7z archive is included, and no compression
filters beyond what a plain LZMA/LZMA2/BCJ2-compressed archive needs (built
with -DZ7_NO_METHODS_FILTERS, which this port's Makefile passes, to leave out
the Delta/BCJ(x86)/ARM/PPC/IA64/SPARC/RISCV branch filters entirely -- 7-Zip
does not select any of those for generic files like ROMs unless a user
explicitly opts into them in the advanced compression settings, which no
ROM-archiving tool does). PPMd support is left off the same way upstream
already supports turning it off: Z7_PPMD_SUPPORT is simply never defined,
so Ppmd7.h is never included and no PPMd code is compiled in.

See win32/sevenzip.c for the small wrapper this port actually calls, and its
own tiny malloc/free-based ISzAlloc -- the SDK's own g_Alloc (Alloc.c, also
not duplicated here) adds large-page and aligned-allocation paths this
read-only use has no need for.
