/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  shader_chain.c -- see shader_chain.h.
 ****************************************************************************/

#include <windows.h>
#include <stdio.h>
#include <string.h>

#define COBJMACROS
#define LIBRA_RUNTIME_D3D11
#include <d3d11.h>
#include "librashader/librashader.h"
#include "shader_chain.h"
#include <stdlib.h>
#include "shared.h"
#include "gui.h"

static HMODULE                          dll;
static PFN_libra_preset_create          pfn_preset_create;
static PFN_libra_preset_free            pfn_preset_free;
static PFN_libra_d3d11_filter_chain_create pfn_chain_create;
static PFN_libra_d3d11_filter_chain_frame  pfn_chain_frame;
static PFN_libra_d3d11_filter_chain_free   pfn_chain_free;
static PFN_libra_error_write            pfn_error_write;
static PFN_libra_error_free_string      pfn_error_free_string;
static PFN_libra_error_free             pfn_error_free;
static PFN_libra_preset_get_runtime_params  pfn_get_params;
static PFN_libra_preset_free_runtime_params pfn_free_params;
static PFN_libra_d3d11_filter_chain_set_param pfn_set_param;

/* The preset's own settings (the "#pragma parameter" lines of its shaders), read when it loads. */
static SHADER_PARAM sp[SHADER_PARAMS_MAX];
static int sp_n;

static libra_d3d11_filter_chain_t chain;
static char                       preset_path[512];
static char                       last_error[256];
static size_t                     frame_count;

static void set_error_from_libra(libra_error_t err)
{
  char *msg = NULL;

  last_error[0] = '\0';
  if (!err) return;

  if (pfn_error_write && pfn_error_write(err, &msg) == 0 && msg)
  {
    lstrcpynA(last_error, msg, sizeof(last_error));
    if (pfn_error_free_string) pfn_error_free_string(&msg);
  }
  else
  {
    lstrcpynA(last_error, "librashader reported an error", sizeof(last_error));
  }

  if (pfn_error_free) pfn_error_free(&err);
}

void shader_chain_init(void)
{
  if (dll) return;   /* already attempted */

#ifndef _WIN64
  return;            /* 32-bit build: shaders are not offered at all */
#endif

  {
    /* In the Plugins folder next to the exe, then next to the exe (how it used to be), then wherever Windows looks. */
    char path[MAX_PATH + 32], *slash;

    if (GetModuleFileNameA(NULL, path, MAX_PATH) > 0 && (slash = strrchr(path, '\\')) != NULL)
    {
      lstrcpyA(slash + 1, "Plugins\\librashader.dll");
      dll = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
      if (!dll)
      {
        lstrcpyA(slash + 1, "librashader.dll");
        dll = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
      }
    }
    if (!dll) dll = LoadLibraryA("librashader.dll");
  }
  if (!dll) return;

  /* GetProcAddress returns a function pointer type incompatible (by strict
     ANSI aliasing rules) with the PFN_* types by a direct cast in one step
     on some compilers' warning settings -- an intermediate void* silences
     that without changing what actually happens (a plain pointer-sized
     bit copy either way). */
  {
    void *p;

    p = (void *)GetProcAddress(dll, "libra_preset_create");
    pfn_preset_create = (PFN_libra_preset_create)p;
    p = (void *)GetProcAddress(dll, "libra_preset_free");
    pfn_preset_free = (PFN_libra_preset_free)p;
    p = (void *)GetProcAddress(dll, "libra_d3d11_filter_chain_create");
    pfn_chain_create = (PFN_libra_d3d11_filter_chain_create)p;
    p = (void *)GetProcAddress(dll, "libra_d3d11_filter_chain_frame");
    pfn_chain_frame = (PFN_libra_d3d11_filter_chain_frame)p;
    p = (void *)GetProcAddress(dll, "libra_d3d11_filter_chain_free");
    pfn_chain_free = (PFN_libra_d3d11_filter_chain_free)p;
    p = (void *)GetProcAddress(dll, "libra_preset_get_runtime_params");
    pfn_get_params = (PFN_libra_preset_get_runtime_params)p;
    p = (void *)GetProcAddress(dll, "libra_preset_free_runtime_params");
    pfn_free_params = (PFN_libra_preset_free_runtime_params)p;
    p = (void *)GetProcAddress(dll, "libra_d3d11_filter_chain_set_param");
    pfn_set_param = (PFN_libra_d3d11_filter_chain_set_param)p;
    p = (void *)GetProcAddress(dll, "libra_error_write");
    pfn_error_write = (PFN_libra_error_write)p;
    p = (void *)GetProcAddress(dll, "libra_error_free_string");
    pfn_error_free_string = (PFN_libra_error_free_string)p;
    p = (void *)GetProcAddress(dll, "libra_error_free");
    pfn_error_free = (PFN_libra_error_free)p;
  }

  if (!pfn_preset_create || !pfn_preset_free || !pfn_chain_create ||
      !pfn_chain_frame || !pfn_chain_free)
  {
    /* A librashader.dll too old/new to export what this was built against --
       treat exactly like it wasn't found at all. */
    FreeLibrary(dll);
    dll = NULL;
  }
}

int shader_chain_available(void)
{
  return dll != NULL;
}

void shader_chain_missing_text(char *out, int len)
{
  char folder[MAX_PATH + 16], *slash;
  char text[MAX_PATH + 400];

  folder[0] = '\0';
  if (GetModuleFileNameA(NULL, folder, MAX_PATH) > 0 && (slash = strrchr(folder, '\\')) != NULL) lstrcpyA(slash + 1, "Plugins");
  wsprintfA(text, "Shaders need librashader.dll, and it was not found or Windows could not load it.\n\n"
                  "Put librashader.dll (it is in the zip the program came in) in the Plugins folder:\n%s\n\nThen turn the shader on again.", folder);
  lstrcpynA(out, text, len);
}

void shader_chain_unload(void)
{
  if (chain)
  {
    pfn_chain_free(&chain);
    chain = NULL;
  }
}

int shader_chain_load(void *device, const char *preset_file)
{
  libra_shader_preset_t preset = NULL;
  libra_error_t err;

  shader_chain_unload();
  last_error[0] = '\0';
  frame_count = 0;

  if (!dll)
  {
    lstrcpynA(last_error, "librashader.dll was not found", sizeof(last_error));
    return 0;
  }

  err = pfn_preset_create(preset_file, &preset);
  if (err) { set_error_from_libra(err); return 0; }

  sp_n = 0;
  if (pfn_get_params)
  {
    libra_preset_param_list_t l;
    if (!pfn_get_params(&preset, &l))
    {
      uint64_t i;
      for (i = 0; i < l.length && sp_n < SHADER_PARAMS_MAX; i++)
      {
        SHADER_PARAM *q = &sp[sp_n++];
        lstrcpynA(q->name, l.parameters[i].name ? l.parameters[i].name : "", sizeof(q->name));
        lstrcpynA(q->desc, l.parameters[i].description ? l.parameters[i].description : q->name, sizeof(q->desc));
        q->init = q->cur = l.parameters[i].initial;
        q->min = l.parameters[i].minimum; q->max = l.parameters[i].maximum; q->step = l.parameters[i].step;
      }
      if (pfn_free_params) pfn_free_params(l);
    }
  }

  err = pfn_chain_create(&preset, (ID3D11Device *)device, NULL, &chain);
  if (err)
  {
    set_error_from_libra(err);
    chain = NULL;
    return 0;
  }

  lstrcpynA(preset_path, preset_file, sizeof(preset_path));
  shader_chain_params_restore();
  return 1;
}

int shader_chain_param_count(void) { return chain ? sp_n : 0; }
SHADER_PARAM *shader_chain_param(int i) { return (i >= 0 && i < sp_n) ? &sp[i] : NULL; }

/* gui.shader_params holds "name=value;" for each parameter changed from the preset's own value. */
static void params_store(void)
{
  int i;
  char *o = gui.shader_params;
  int left = (int)sizeof(gui.shader_params);

  gui.shader_params[0] = '\0';
  lstrcpynA(gui.shader_params_for, preset_path, sizeof(gui.shader_params_for));
  for (i = 0; i < sp_n; i++)
    if (sp[i].cur != sp[i].init)
    {
      int n = snprintf(o, (size_t)left, "%s=%g;", sp[i].name, sp[i].cur);
      if (n < 0 || n >= left) break;
      o += n; left -= n;
    }
  config_save();
}

int shader_chain_param_set(int i, float v)
{
  if (!chain || !pfn_set_param || i < 0 || i >= sp_n) return 0;
  if (v < sp[i].min) v = sp[i].min;
  if (v > sp[i].max) v = sp[i].max;
  {
    libra_error_t err = pfn_set_param(&chain, sp[i].name, v);
    if (err) { set_error_from_libra(err); return 0; }
  }
  sp[i].cur = v;
  params_store();
  return 1;
}

void shader_chain_params_restore(void)
{
  char buf[sizeof(gui.shader_params)], *p, *next;
  int i;

  if (!chain || !pfn_set_param || lstrcmpA(gui.shader_params_for, preset_path)) return;
  lstrcpynA(buf, gui.shader_params, sizeof(buf));
  for (p = buf; p && *p; p = next)
  {
    char *eq;
    next = strchr(p, ';');
    if (next) *next++ = '\0';
    eq = strchr(p, '=');
    if (!eq) continue;
    *eq++ = '\0';
    for (i = 0; i < sp_n; i++)
      if (!lstrcmpA(sp[i].name, p))
      {
        float v = (float)atof(eq);
        libra_error_t err = pfn_set_param(&chain, p, v);
        if (err) set_error_from_libra(err); else sp[i].cur = v;
      }
  }
}

int shader_chain_active(void)
{
  return chain != NULL;
}

const char *shader_chain_preset_path(void)
{
  return preset_path;
}

int shader_chain_render(void *context, void *input, void *output,
                        int x, int y, int w, int h)
{
  libra_viewport_t viewport;
  libra_error_t err;

  if (!chain) return 0;
  if (w < 1 || h < 1) return 0;

  viewport.x = (float)x;
  viewport.y = (float)y;
  viewport.width = (UINT)w;
  viewport.height = (UINT)h;

  err = pfn_chain_frame(&chain, (ID3D11DeviceContext *)context, frame_count,
                        (ID3D11ShaderResourceView *)input,
                        (ID3D11RenderTargetView *)output,
                        &viewport, NULL, NULL);
  frame_count++;

  if (err) { set_error_from_libra(err); return 0; }
  return 1;
}

const char *shader_chain_last_error(void)
{
  return last_error;
}
