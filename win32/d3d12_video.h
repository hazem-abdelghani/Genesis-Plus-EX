/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *  Copyright (c) 2026 Hazem Abdelghani
 *
 *  d3d12_video.h -- optional Direct3D 12 output path.
 *
 *  The same contract as d3d11_video.h (see it and d3d9_video.h for the
 *  rationale): every entry point is safe to call when the device is not
 *  available, and any failure is reported back so the caller draws that frame
 *  with the classic renderer instead of treating it as fatal.
 *
 *  Direct3D 12 needs Windows 10. d3d12.dll is loaded when the renderer is
 *  made, not linked, so the program still starts on Windows 7 and 8 and the
 *  renderer simply is not available there (the choice falls back to the
 *  classic renderer). The picture is drawn into a flip-model swap chain on
 *  a child window, as in the Direct3D 11 path, with a frame latency of one
 *  (the swap chain's waitable object), real exclusive fullscreen, variable
 *  refresh (tearing) and shader presets (librashader's Direct3D 12 runtime).
 ****************************************************************************/

#ifndef D3D12_VIDEO_H
#define D3D12_VIDEO_H

#include <windows.h>
#include "types.h"

int  d3d12_init(HWND hwnd);                 /* 1 when the device and swap chain were made, 0 otherwise (a normal outcome on a PC without Direct3D 12) */
void d3d12_shutdown(void);
int  d3d12_available(void);                 /* the device is usable now */

/* Renders one frame (the picture, stretched into dest; smooth is the Scaling Filter, 0 to 4) into the back buffer and leaves the command list open: the
   caller then draws its overlays and calls d3d12_present(). Returns 0 when the device is not usable (draw the frame with another renderer). */
int  d3d12_render_frame(const uint16 *src_pixels, int src_pitch, int src_w, int src_h, const RECT *dest, int client_w, int client_h, int smooth);

void d3d12_overlay_slot(int slot);          /* which of two overlay textures the next draw uses: 0 the full-picture ones, 1 the messages */
int  d3d12_draw_overlay(const uint32 *argb_pixels, int argb_pitch, int w, int h, const RECT *screen_area);

void d3d12_set_adapter(const char *name);   /* the graphics card to make the device on ("" = Auto): used the next time the device is made */
int  d3d12_apply_vrr(void);                 /* variable refresh was switched: only the swap chain is made again (1 = done, 0 = make the device again) */
void d3d12_set_vrr(int on);                 /* variable refresh wanted: used the next time the device is made */
int  d3d12_vrr_active(void);                /* the swap chain was made for variable refresh */
int  d3d12_resample_available(int mode);    /* Scaling Filter 1 (Sharp Nearest), 3 (Sharp Bilinear) and 4 (Bicubic): did its pixel shader compile? */

long d3d12_exclusive_enter(HWND win, int w, int h, int hz_num, int hz_den);   /* real exclusive fullscreen on win (0 = done, else the HRESULT that stopped it) */
void d3d12_exclusive_leave(void);
int  d3d12_exclusive_active(void);
int  d3d12_exclusive_size(int *w, int *h);  /* in exclusive fullscreen: the size of the screen mode the picture is drawn into (1), else 0 */

void d3d12_show_surface(int on);            /* hide (0) or show (1) the window the picture is drawn in */
void d3d12_set_sync(int on);                /* VSync: the next d3d12_present() waits for the display's refresh (1) or not (0) */
void d3d12_present(void);

/* For shader presets (shader_chain.c): the ID3D12Device, valid while the renderer is up. Returns 0 when it is not. */
int  d3d12_get_shader_device(void **device);

#endif
