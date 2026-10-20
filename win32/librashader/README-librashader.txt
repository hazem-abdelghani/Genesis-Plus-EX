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

To use the feature once the DLL is in place: Video > Render Filter >
Shaders > Preset... in the app, and pick a .slangp shader preset
(RetroArch's "slang-shaders" repository on GitHub has hundreds, e.g.
crt/crt-royale.slangp).

64-bit only: librashader ships no 32-bit Windows build, so this feature is
unavailable in gpex32.exe regardless -- the menu item is still there, but
loading a preset reports librashader.dll as not found (a 64-bit DLL simply
won't load into a 32-bit process).

Only D3D11 is used here (librashader also supports Vulkan, D3D12 and
Metal, and OpenGL, but this app doesn't have renderers for any of those),
and only reading a preset already built by someone else -- this port
never creates or edits .slangp files, and has no UI for the pass-by-pass
parameter tuning librashader also exposes.
