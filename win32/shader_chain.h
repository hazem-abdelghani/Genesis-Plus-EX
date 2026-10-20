/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  shader_chain.h -- GPU shader presets (librashader) for the D3D11 renderer.
 *
 *  librashader (https://github.com/SnowflakePowered/librashader, MIT) is a
 *  Rust library with a C ABI, loaded from librashader.dll at runtime -- like
 *  d3dcompiler_47.dll in d3d11_video.c, its absence just means the feature
 *  is unavailable, not that the app fails to start. This module owns that
 *  loading and the filter chain lifetime; d3d11_video.c only calls
 *  shader_chain_active()/shader_chain_render() from its own render path.
 *
 *  D3D11 only: librashader ships no 32-bit Windows build, and this app has
 *  no Vulkan/D3D12 renderer to use its other backends with. See
 *  librashader/README-librashader.txt.
 ****************************************************************************/

#ifndef _SHADER_CHAIN_H_
#define _SHADER_CHAIN_H_

/* Locates and loads librashader.dll (next to the exe, then the normal DLL
   search path) and resolves the handful of exports this module calls.
   Safe to call once at startup even on a machine without it -- every other
   function below is then simply a no-op. Idempotent. */
void shader_chain_init(void);

/* Whether librashader.dll was found and loaded. */
int  shader_chain_available(void);

/* Loads a .slangp preset and builds a filter chain for it against `device`
   (an ID3D11Device*, passed as void* so this header stays includable
   without <d3d11.h>). Replaces whatever chain was already loaded. Returns
   1 on success. On failure, shader_chain_active() is false afterward and
   shader_chain_last_error() has a message worth showing the user. */
int  shader_chain_load(void *device, const char *preset_path);

/* Frees the current chain, if any. Safe to call when none is loaded, and
   safe (expected) to call before the device it was created against is
   released -- the chain holds device-bound GPU resources internally. */
void shader_chain_unload(void);

/* Whether a preset is currently loaded and ready to render with. */
int  shader_chain_active(void);

/* The .slangp path last given to shader_chain_load(), or "" if none. */
const char *shader_chain_preset_path(void);

/* Renders one frame through the loaded chain: `input` (an
   ID3D11ShaderResourceView*) is the emulator's current picture, `output`
   (an ID3D11RenderTargetView*) is where the shaded result is drawn,
   confined to the x/y/w/h rectangle within it. `context` is the
   ID3D11DeviceContext* to record onto (NULL means the device's immediate
   context). Returns 1 on success; on failure the caller should fall back
   to its own passthrough draw for this frame. */
int  shader_chain_render(void *context, void *input, void *output,
                          int x, int y, int w, int h);

/* Message for the most recent shader_chain_load()/shader_chain_render()
   failure, or "" if the last call succeeded. */
const char *shader_chain_last_error(void);

/* A setting of the loaded preset that can be adjusted while it runs. */
#define SHADER_PARAMS_MAX 96
typedef struct { char name[64]; char desc[96]; float init, min, max, step, cur; } SHADER_PARAM;
int  shader_chain_param_count(void);                 /* 0 when no preset is loaded */
SHADER_PARAM *shader_chain_param(int i);
int  shader_chain_param_set(int i, float value);     /* applied at once, and remembered for this preset */
void shader_chain_params_restore(void);

#endif
