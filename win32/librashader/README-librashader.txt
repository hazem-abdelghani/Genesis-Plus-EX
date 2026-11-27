This directory holds librashader.h, the C header for librashader
(https://github.com/SnowflakePowered/librashader), by Ronny Chan (chyyran)
and contributors, and librashader.dll (x86_64, v0.12.0), the prebuilt
Windows binary from their own GitHub releases. The header is MIT-licensed;
librashader as a whole is dual-licensed MIT/Apache-2.0/MPL-2.0, your
choice -- either permits redistributing the built DLL as-is, which is all
this is.

librashader.dll is NOT built by this project -- it's a Rust library, and
this project's toolchain is a plain C/C++ MinGW cross-compiler with no
Rust involved anywhere. It's vendored here purely as a binary so the
feature works out of the box, the same reasoning zlib's source or xBRZ's
are vendored elsewhere in this port, just as a prebuilt DLL instead of
source this toolchain can't compile anyway.

See win32/shader_chain.c for how this port actually uses it: the DLL is
loaded at runtime with LoadLibraryA(), exactly like d3dcompiler_47.dll
already is in d3d11_video.c, rather than linked in at build time -- so it
still has to end up sitting next to the built exe to actually be found.
`make -f Makefile.win32 pack` copies it there automatically; building
without `pack` (plain `make`) doesn't, so copy it by hand from here next
to the exe if you build that way.

To use the feature once the DLL is in place: Video > Shaders >
Preset... in the app, and pick a .slangp shader preset
(RetroArch's "slang-shaders" repository on GitHub has hundreds, e.g.
crt/crt-royale.slangp).

64-bit only: librashader ships no 32-bit Windows build, so the 32-bit program has no Shaders
menu and no Shader box in Settings.

The Direct3D 11 and Direct3D 12 renderers use it (librashader also supports Vulkan, OpenGL and
Metal, but this app has no renderers for those). It only reads a preset already built by someone
else: this port never creates or edits .slangp files. The pass-by-pass parameters of a preset can
be changed while it runs (Video > Shaders > Shader Parameters...), and the Preset Browser lists the
presets of the Shaders folder.

dxcompiler.dll and dxil.dll (x86_64, Microsoft DirectX Shader Compiler 1.8.2505.1,
from github.com/microsoft/DirectXShaderCompiler, licenses in LICENSE-LLVM.txt and
LICENSE-MS.txt here) are what librashader's Direct3D 12 runtime compiles with: it loads
them itself when the first chain is made and ends the program if they are missing (the
Direct3D 11 runtime does not use them). shader_chain.c loads them first, from the
Plugins folder, and says so when they are not there.
