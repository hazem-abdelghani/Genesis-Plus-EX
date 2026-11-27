/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *  Copyright (c) 2026 Hazem Abdelghani
 *
 *  d3d12_video.c -- see d3d12_video.h for the rationale.
 *
 *  The same shape as d3d11_video.c, so that video.c drives the two the same
 *  way: a child window of the program's window that is kept exactly the size
 *  of the content area, a swap chain bound to it, the picture uploaded into
 *  a texture and drawn as one textured quad, the overlays (messages, bezel,
 *  scanlines) uploaded and drawn on top with alpha blending, then present.
 *
 *  What Direct3D 12 adds, and how it is kept small:
 *
 *   - d3d12.dll is loaded when the renderer is made, not linked (the program
 *     targets Windows 7 too). The shaders are compiled at run time with
 *     D3DCompile (d3dcompiler_47.dll, loaded the same way) as Shader Model 5
 *     bytecode, which Direct3D 12 takes.
 *   - One graphics command list. Each frame is recorded in render_frame()
 *     (clear, upload, draw), more is added by draw_overlay(), and present()
 *     closes, executes and presents it. Two frames may be in flight, each with
 *     its own command allocator and upload memory; a fence says when a frame's
 *     slot is free again.
 *   - The quad has no vertex buffer: the vertex shader makes its four corners
 *     from SV_VertexID and a rectangle passed as root constants.
 *   - The swap chain is a flip-model one with two buffers and its waitable
 *     object (a frame latency of one, as the Direct3D 11 path sets).
 *   - Uploads go through persistently mapped upload buffers, one set for
 *     each frame in flight, sub-allocated by each upload of the frame (a
 *     frame may upload several overlays, which the GPU copies later).
 ****************************************************************************/

#include <string.h>
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#include <d3d12.h>
#include <dxgi1_5.h>
#include <d3dcompiler.h>
#include "d3d12_video.h"
#include "shader_chain.h"

extern unsigned video_bar_color(void);       /* video.c: 0xRRGGBB of the bars around the picture */
extern int browser_toolbar_height(void);     /* browser.c: 0 when the toolbar is not shown */
extern void i18n_keep_ltr(const char *cls);
extern void gui_log(const char *fmt, ...);

#define FRAMES        2                      /* frames in flight (and swap chain buffers) */
#define UP_BUFS       6                      /* upload buffers a frame slot may have */
#define UP_MIN_CAP    (16u * 1024u * 1024u)
#define OV_MAX        16                     /* overlays a frame may draw (the scanlines, the bezel, and one for each part of the messages that changed) */
#define SLOT_FRAME    0                      /* the SRV heap: the picture, then the overlay textures of each frame slot */
#define SLOT_OVERLAY(s, i) (1 + (s) * OV_MAX + (i))

/* ---- the quad, in Shader Model 5 ---- */
static const char *vs_src =
  "cbuffer C : register(b0) { float4 rect; float lin; float3 pad; };\n"
  "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
  "VSOut main(uint id : SV_VertexID)\n"
  "{\n"
  "  float2 uv = float2((float)(id & 1u), (float)(id >> 1u));\n"
  "  VSOut o;\n"
  "  o.pos = float4(lerp(rect.x, rect.z, uv.x), lerp(rect.y, rect.w, uv.y), 0.0, 1.0);\n"
  "  o.uv = uv;\n"
  "  return o;\n"
  "}\n";

static const char *ps_src =
  "cbuffer C : register(b0) { float4 rect; float lin; float3 pad; };\n"
  "Texture2D tex0 : register(t0);\n"
  "SamplerState sPoint : register(s0);\n"
  "SamplerState sLin : register(s1);\n"
  "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
  "float4 main(VSOut i) : SV_TARGET\n"
  "{\n"
  "  float4 a = tex0.Sample(sPoint, i.uv);\n"
  "  float4 b = tex0.Sample(sLin, i.uv);\n"
  "  return lin > 0.5 ? b : a;\n"
  "}\n";

/* Scaling Filter pixel shaders over the picture, the same as the Direct3D 11 path has (see there): Bicubic (Catmull-Rom) in nine bilinear reads, Sharp
   Bilinear (the sample point follows the smootherstep curve between two pixel centers), Sharp Nearest (the blend between two source pixels is one screen
   pixel wide). They read with the linear sampler. */
static const char *ps_resample_src =
  "Texture2D tex0 : register(t0);\n"
  "SamplerState samp0 : register(s1);\n"
  "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
  "float4 main_catmull(VSOut i) : SV_TARGET\n"
  "{\n"
  "  uint tw, th; tex0.GetDimensions(tw, th);\n"
  "  float2 dims = float2(tw, th);\n"
  "  float2 sp = i.uv * dims;\n"
  "  float2 t1 = floor(sp - 0.5) + 0.5;\n"
  "  float2 f = sp - t1;\n"
  "  float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));\n"
  "  float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
  "  float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));\n"
  "  float2 w3 = f * f * (-0.5 + 0.5 * f);\n"
  "  float2 w12 = w1 + w2;\n"
  "  float2 o12 = w2 / w12;\n"
  "  float2 t0 = (t1 - 1.0) / dims;\n"
  "  float2 t3 = (t1 + 2.0) / dims;\n"
  "  float2 t12 = (t1 + o12) / dims;\n"
  "  float4 r = tex0.Sample(samp0, float2(t0.x, t0.y)) * (w0.x * w0.y)\n"
  "           + tex0.Sample(samp0, float2(t12.x, t0.y)) * (w12.x * w0.y)\n"
  "           + tex0.Sample(samp0, float2(t3.x, t0.y)) * (w3.x * w0.y)\n"
  "           + tex0.Sample(samp0, float2(t0.x, t12.y)) * (w0.x * w12.y)\n"
  "           + tex0.Sample(samp0, float2(t12.x, t12.y)) * (w12.x * w12.y)\n"
  "           + tex0.Sample(samp0, float2(t3.x, t12.y)) * (w3.x * w12.y)\n"
  "           + tex0.Sample(samp0, float2(t0.x, t3.y)) * (w0.x * w3.y)\n"
  "           + tex0.Sample(samp0, float2(t12.x, t3.y)) * (w12.x * w3.y)\n"
  "           + tex0.Sample(samp0, float2(t3.x, t3.y)) * (w3.x * w3.y);\n"
  "  return r;\n"
  "}\n"
  "float4 main_sharpbil(VSOut i) : SV_TARGET\n"
  "{\n"
  "  uint tw, th; tex0.GetDimensions(tw, th);\n"
  "  float2 dims = float2(tw, th);\n"
  "  float2 p = i.uv * dims + 0.5;\n"
  "  float2 fl = floor(p);\n"
  "  float2 f = p - fl;\n"
  "  f = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);\n"
  "  float2 uvn = (fl + f - 0.5) / dims;\n"
  "  return tex0.Sample(samp0, uvn);\n"
  "}\n"
  "float4 main_sharpnear(VSOut i) : SV_TARGET\n"
  "{\n"
  "  uint tw, th; tex0.GetDimensions(tw, th);\n"
  "  float2 dims = float2(tw, th);\n"
  "  float2 fp = max(float2(abs(ddx(i.uv.x)), abs(ddy(i.uv.y))) * dims, 0.0001);\n"
  "  float2 k = max(1.0 / fp, float2(1.0, 1.0));\n"
  "  float2 p = i.uv * dims;\n"
  "  float2 fl = floor(p);\n"
  "  float2 cd = p - fl - 0.5;\n"
  "  float2 rr = 0.5 - 0.5 / k;\n"
  "  float2 f = (cd - clamp(cd, -rr, rr)) * k + 0.5;\n"
  "  float2 uvn = (fl + f) / dims;\n"
  "  return tex0.Sample(samp0, uvn);\n"
  "}\n";

/* ---- the libraries, loaded when needed ---- */
typedef HRESULT (WINAPI *pfn_create_device)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
typedef HRESULT (WINAPI *pfn_serialize_rs)(const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **, ID3DBlob **);
typedef HRESULT (WINAPI *pfn_create_factory1)(REFIID, void **);

static HMODULE            d3d12_dll;
static HMODULE            dxgi_dll;
static HMODULE            d3dcompiler_dll;
static pfn_create_device  create_device_fn;
static pfn_serialize_rs   serialize_rs_fn;
static pfn_create_factory1 create_factory_fn;
static pD3DCompile        d3dcompile_fn;

static int load_libraries(void)
{
  if (create_device_fn && serialize_rs_fn && create_factory_fn && d3dcompile_fn) return 1;

  if (!d3d12_dll) d3d12_dll = LoadLibraryA("d3d12.dll");
  if (!dxgi_dll) dxgi_dll = LoadLibraryA("dxgi.dll");
  if (!d3dcompiler_dll) d3dcompiler_dll = LoadLibraryA("d3dcompiler_47.dll");
  if (!d3d12_dll || !dxgi_dll || !d3dcompiler_dll) return 0;

  create_device_fn = (pfn_create_device)(void *)GetProcAddress(d3d12_dll, "D3D12CreateDevice");
  serialize_rs_fn = (pfn_serialize_rs)(void *)GetProcAddress(d3d12_dll, "D3D12SerializeRootSignature");
  create_factory_fn = (pfn_create_factory1)(void *)GetProcAddress(dxgi_dll, "CreateDXGIFactory1");
  d3dcompile_fn = (pD3DCompile)(void *)GetProcAddress(d3dcompiler_dll, "D3DCompile");
  return create_device_fn && serialize_rs_fn && create_factory_fn && d3dcompile_fn;
}

/* ---- state ---- */
typedef struct
{
  ID3D12Resource *buf;
  UINT8          *map;
  size_t          cap, used;
} up_buf_t;

typedef struct
{
  ID3D12CommandAllocator *alloc;
  UINT64                  fence;         /* the value that says the frame in this slot has finished */
  up_buf_t                up[UP_BUFS];
  int                     up_n;
} frame_slot_t;

static HWND                       parent_hwnd;
static HWND                       child_hwnd;
static RECT                       child_keep;          /* where the picture window was when the device was let go: its replacement starts there, so the window behind it is not seen while the new device is made */
static int                        child_keep_ok;
static int                        child_top = -1;
static ATOM                       child_class;

static ID3D12Device              *device;
static ID3D12CommandQueue        *queue;
static ID3D12GraphicsCommandList *list;
static ID3D12Fence               *fence;
static HANDLE                     fence_event;
static UINT64                     fence_val;
static frame_slot_t               fr[FRAMES];
static unsigned                   frame_no;
static int                        cur_slot;
static int                        recording;            /* the command list is open (render_frame() has been called, present() not yet) */

static IDXGIFactory2             *factory;
static IDXGISwapChain            *swapchain;
static IDXGISwapChain3           *swapchain3;
static HANDLE                     waitable;
static ID3D12Resource            *backbuf[FRAMES];
static ID3D12DescriptorHeap      *rtv_heap;
static UINT                       rtv_inc;
static int                        swap_w, swap_h;
static UINT                       sc_flags;             /* the flags the swap chain was made with (ResizeBuffers() must repeat them) */

static ID3D12RootSignature       *root_sig;
static ID3D12PipelineState       *pso_base[2];          /* [0] opaque, [1] alpha blended */
static ID3D12PipelineState       *pso_resample[3];      /* Bicubic, Sharp Bilinear, Sharp Nearest (opaque); NULL when one did not compile */
static ID3D12DescriptorHeap      *srv_heap;
static UINT                       srv_inc;

static ID3D12Resource            *tex;
static int                        tex_w, tex_h;
static DXGI_FORMAT                tex_fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
static int                        tex_565;              /* the GPU takes RGB565 as it is */
/* The overlays of a frame are not drawn when they are recorded, and a descriptor is read when the GPU gets to the draw: so each overlay of a frame has a
   texture and a descriptor of its own (a pool for each frame slot), and they are changed only when the frame that used them has finished. */
typedef struct { ID3D12Resource *tex; int w, h; } ov_t;
static ov_t                       ovp[FRAMES][OV_MAX];
static int                        ov_used;              /* overlays drawn so far in the frame being recorded */

static int                        device_ok;
static int                        vrr_want, vrr_active;
static int                        excl_active;
static int                        present_sync;
static int                        excl_bad;             /* frames in a row that Present failed in True Exclusive Fullscreen */
static int                        wait_stuck;           /* times in a row that the swap chain's waitable object did not come: pacing by it is given up (Present() paces then) */
static char                       adapter_want[96];

#define CHILD_CLASS_NAME "GPEXD3D12Surface"

void d3d12_set_adapter(const char *name) { lstrcpynA(adapter_want, name ? name : "", sizeof(adapter_want)); }
void d3d12_set_vrr(int on) { vrr_want = on ? 1 : 0; }
int  d3d12_vrr_active(void) { return vrr_active; }
int  d3d12_available(void) { return device_ok; }
int  d3d12_exclusive_active(void) { return excl_active; }
void d3d12_overlay_slot(int slot) { (void)slot; }      /* (each overlay of a frame has a texture of its own here) */
void d3d12_set_sync(int on) { present_sync = on ? 1 : 0; }

int d3d12_exclusive_size(int *w, int *h)
{
  if (!excl_active || swap_w < 1 || swap_h < 1) return 0;
  *w = swap_w; *h = swap_h;
  return 1;
}

int d3d12_get_shader_device(void **dev)
{
  if (!device_ok || !device) return 0;
  *dev = (void *)device;
  return 1;
}

int d3d12_resample_available(int mode)
{
  int k = mode == 1 ? 2 : (mode == 3 ? 1 : (mode == 4 ? 0 : -1));
  return k >= 0 && pso_resample[k] != NULL;
}

static LRESULT CALLBACK child_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
  if (msg == WM_ERASEBKGND) return 1;   /* the swap chain covers the whole thing every frame */
  return DefWindowProcA(hwnd, msg, wp, lp);
}

static void ensure_child_class(void)
{
  WNDCLASSEXA wc;

  if (child_class) return;
  ZeroMemory(&wc, sizeof(wc));
  wc.cbSize        = sizeof(wc);
  wc.style         = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc   = child_wnd_proc;
  wc.hInstance     = GetModuleHandleA(NULL);
  wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
  wc.lpszClassName = CHILD_CLASS_NAME; i18n_keep_ltr(CHILD_CLASS_NAME);
  child_class = RegisterClassExA(&wc);
}

/* ---- the GPU's progress ---- */
static void wait_fence(UINT64 v)
{
  if (!fence || !fence_event || !v) return;
  if (ID3D12Fence_GetCompletedValue(fence) < v)
  {
    if (SUCCEEDED(ID3D12Fence_SetEventOnCompletion(fence, v, fence_event))) WaitForSingleObject(fence_event, 5000);
  }
}

static void flush_gpu(void)
{
  if (!queue || !fence) return;
  fence_val++;
  if (SUCCEEDED(ID3D12CommandQueue_Signal(queue, fence, fence_val))) wait_fence(fence_val);
}

static D3D12_RESOURCE_BARRIER transition(ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
  D3D12_RESOURCE_BARRIER bb;

  ZeroMemory(&bb, sizeof(bb));
  bb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  bb.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  bb.Transition.pResource = r;
  bb.Transition.StateBefore = a;
  bb.Transition.StateAfter = b;
  bb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  return bb;
}

/* ---- upload memory ---- */
static int make_upload_buffer(up_buf_t *u, size_t cap)
{
  D3D12_HEAP_PROPERTIES hp;
  D3D12_RESOURCE_DESC rd;
  D3D12_RANGE none = { 0, 0 };
  void *p = NULL;

  ZeroMemory(&hp, sizeof(hp));
  hp.Type = D3D12_HEAP_TYPE_UPLOAD;
  hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
  hp.CreationNodeMask = 1; hp.VisibleNodeMask = 1;
  ZeroMemory(&rd, sizeof(rd));
  rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width = cap; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
  rd.Format = DXGI_FORMAT_UNKNOWN;
  rd.SampleDesc.Count = 1;
  rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(ID3D12Device_CreateCommittedResource(device, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void **)&u->buf))) return 0;
  if (FAILED(ID3D12Resource_Map(u->buf, 0, &none, &p)) || !p) { ID3D12Resource_Release(u->buf); u->buf = NULL; return 0; }
  u->map = (UINT8 *)p; u->cap = cap; u->used = 0;
  return 1;
}

static void free_upload_slot(frame_slot_t *s)
{
  int i;
  for (i = 0; i < s->up_n; i++)
    if (s->up[i].buf)
    {
      ID3D12Resource_Unmap(s->up[i].buf, 0, NULL);
      ID3D12Resource_Release(s->up[i].buf);
      s->up[i].buf = NULL; s->up[i].map = NULL;
    }
  s->up_n = 0;
}

/* A piece of this frame's upload memory: the buffer, the offset in it (a multiple of 512) and where to write. */
static int up_alloc(frame_slot_t *s, size_t size, ID3D12Resource **buf, size_t *offset, UINT8 **ptr)
{
  int i;
  size_t need = (size + 511u) & ~(size_t)511u;

  for (i = 0; i < s->up_n; i++)
    if (s->up[i].used + need <= s->up[i].cap)
    {
      *buf = s->up[i].buf; *offset = s->up[i].used; *ptr = s->up[i].map + s->up[i].used;
      s->up[i].used += need;
      return 1;
    }
  if (s->up_n >= UP_BUFS) return 0;
  if (!make_upload_buffer(&s->up[s->up_n], need > UP_MIN_CAP ? need : UP_MIN_CAP)) return 0;
  i = s->up_n++;
  *buf = s->up[i].buf; *offset = 0; *ptr = s->up[i].map;
  s->up[i].used = need;
  return 1;
}

/* ---- textures ---- */
static void srv_for(ID3D12Resource *res, DXGI_FORMAT fmt, int index)
{
  D3D12_SHADER_RESOURCE_VIEW_DESC sd;
  D3D12_CPU_DESCRIPTOR_HANDLE h = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(srv_heap);

  ZeroMemory(&sd, sizeof(sd));
  sd.Format = fmt;
  sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sd.Texture2D.MipLevels = 1;
  h.ptr += (SIZE_T)index * srv_inc;
  ID3D12Device_CreateShaderResourceView(device, res, &sd, h);
}

static ID3D12Resource *make_texture(int w, int h, DXGI_FORMAT fmt)
{
  D3D12_HEAP_PROPERTIES hp;
  D3D12_RESOURCE_DESC rd;
  ID3D12Resource *r = NULL;

  ZeroMemory(&hp, sizeof(hp));
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  hp.CreationNodeMask = 1; hp.VisibleNodeMask = 1;
  ZeroMemory(&rd, sizeof(rd));
  rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width = (UINT64)w; rd.Height = (UINT)h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
  rd.Format = fmt;
  rd.SampleDesc.Count = 1;
  rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  if (FAILED(ID3D12Device_CreateCommittedResource(device, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL, &IID_ID3D12Resource, (void **)&r))) return NULL;
  return r;
}

/* Copies a w x h picture from src (rows of src_pitch bytes, bytes_per_px each) into this frame's upload memory and records the copy into `dst`, which is
   left in the pixel shader resource state. Convert (RGB565 to 32 bits) when the GPU does not take 565. */
static int record_upload(ID3D12Resource *dst, DXGI_FORMAT fmt, int w, int h, const void *src, int src_pitch, int bytes_per_px, int convert565)
{
  frame_slot_t *s = &fr[cur_slot];
  D3D12_TEXTURE_COPY_LOCATION dl, sl;
  D3D12_RESOURCE_BARRIER b;
  ID3D12Resource *ubuf = NULL;
  size_t off = 0, pitch, size;
  UINT8 *ptr = NULL;
  int y;

  pitch = ((size_t)w * (size_t)bytes_per_px + 255u) & ~(size_t)255u;      /* (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) */
  size = pitch * (size_t)h;
  if (!up_alloc(s, size, &ubuf, &off, &ptr)) return 0;

  for (y = 0; y < h; y++)
  {
    const uint8 *srow = (const uint8 *)src + (size_t)y * (size_t)src_pitch;
    UINT8 *drow = ptr + (size_t)y * pitch;

    if (!convert565) memcpy(drow, srow, (size_t)w * (size_t)bytes_per_px);
    else
    {
      const uint16 *s16 = (const uint16 *)srow;
      uint32 *d32 = (uint32 *)drow;
      int x;
      for (x = 0; x < w; x++)
      {
        uint16 p = s16[x];
        uint32 r5 = (p >> 11) & 0x1F, g6 = (p >> 5) & 0x3F, b5 = p & 0x1F;
        d32[x] = (0xFFu << 24) | (((r5 << 3) | (r5 >> 2)) << 16) | (((g6 << 2) | (g6 >> 4)) << 8) | ((b5 << 3) | (b5 >> 2));
      }
    }
  }

  b = transition(dst, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);

  ZeroMemory(&dl, sizeof(dl));
  dl.pResource = dst;
  dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dl.SubresourceIndex = 0;
  ZeroMemory(&sl, sizeof(sl));
  sl.pResource = ubuf;
  sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  sl.PlacedFootprint.Offset = (UINT64)off;
  sl.PlacedFootprint.Footprint.Format = fmt;
  sl.PlacedFootprint.Footprint.Width = (UINT)w;
  sl.PlacedFootprint.Footprint.Height = (UINT)h;
  sl.PlacedFootprint.Footprint.Depth = 1;
  sl.PlacedFootprint.Footprint.RowPitch = (UINT)pitch;
  ID3D12GraphicsCommandList_CopyTextureRegion(list, &dl, 0, 0, 0, &sl, NULL);

  b = transition(dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
  return 1;
}

/* ---- release ---- */
static void release_backbuffers(void)
{
  int i;
  for (i = 0; i < FRAMES; i++) if (backbuf[i]) { ID3D12Resource_Release(backbuf[i]); backbuf[i] = NULL; }
}

static void release_textures(void)
{
  int s, i;
  if (tex) { ID3D12Resource_Release(tex); tex = NULL; tex_w = 0; tex_h = 0; }
  for (s = 0; s < FRAMES; s++)
    for (i = 0; i < OV_MAX; i++)
      if (ovp[s][i].tex) { ID3D12Resource_Release(ovp[s][i].tex); ovp[s][i].tex = NULL; ovp[s][i].w = 0; ovp[s][i].h = 0; }
}

static void release_pipeline(void)
{
  int i;
  for (i = 0; i < 2; i++) if (pso_base[i]) { ID3D12PipelineState_Release(pso_base[i]); pso_base[i] = NULL; }
  for (i = 0; i < 3; i++) if (pso_resample[i]) { ID3D12PipelineState_Release(pso_resample[i]); pso_resample[i] = NULL; }
  if (root_sig) { ID3D12RootSignature_Release(root_sig); root_sig = NULL; }
  if (srv_heap) { ID3D12DescriptorHeap_Release(srv_heap); srv_heap = NULL; }
  if (rtv_heap) { ID3D12DescriptorHeap_Release(rtv_heap); rtv_heap = NULL; }
}

static void release_device(void)
{
  int i;

  recording = 0;
  if (swapchain && excl_active) { IDXGISwapChain_SetFullscreenState(swapchain, FALSE, NULL); excl_active = 0; }      /* (a swap chain must not be let go of while fullscreen) */
  flush_gpu();
  shader_chain_unload();                       /* holds GPU resources bound to this device */
  release_textures();
  release_backbuffers();
  for (i = 0; i < FRAMES; i++)
  {
    free_upload_slot(&fr[i]);
    if (fr[i].alloc) { ID3D12CommandAllocator_Release(fr[i].alloc); fr[i].alloc = NULL; }
    fr[i].fence = 0;
  }
  release_pipeline();
  if (waitable) waitable = NULL;               /* (owned by the swap chain) */
  if (swapchain3) { IDXGISwapChain3_Release(swapchain3); swapchain3 = NULL; }
  if (swapchain) { IDXGISwapChain_Release(swapchain); swapchain = NULL; }
  if (list) { ID3D12GraphicsCommandList_Release(list); list = NULL; }
  if (fence) { ID3D12Fence_Release(fence); fence = NULL; }
  if (fence_event) { CloseHandle(fence_event); fence_event = NULL; }
  if (queue) { ID3D12CommandQueue_Release(queue); queue = NULL; }
  if (factory) { IDXGIFactory2_Release(factory); factory = NULL; }
  if (device) { ID3D12Device_Release(device); device = NULL; }
  fence_val = 0; frame_no = 0; swap_w = 0; swap_h = 0;
  vrr_active = 0;
}

/* ---- the card ---- */
static void adapter_name(const DXGI_ADAPTER_DESC1 *d, char *out, int n)
{
  if (!WideCharToMultiByte(CP_UTF8, 0, d->Description, -1, out, n, NULL, NULL)) lstrcpynA(out, "Graphics Adapter", n);
}

/* The card asked for, or NULL (Auto, or the card is gone: Windows picks). Release it after use. */
static IUnknown *pick_adapter(void)
{
  IDXGIFactory1 *f = NULL;
  IDXGIAdapter1 *a = NULL;
  IUnknown *found = NULL;
  UINT i;

  if (!adapter_want[0]) return NULL;
  if (FAILED(create_factory_fn(&IID_IDXGIFactory1, (void **)&f)) || !f) return NULL;
  for (i = 0; !found && IDXGIFactory1_EnumAdapters1(f, i, &a) != DXGI_ERROR_NOT_FOUND; i++)
  {
    DXGI_ADAPTER_DESC1 d;
    char nm[96];
    if (SUCCEEDED(IDXGIAdapter1_GetDesc1(a, &d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
    {
      adapter_name(&d, nm, sizeof(nm));
      if (!lstrcmpiA(nm, adapter_want) && SUCCEEDED(IDXGIAdapter1_QueryInterface(a, &IID_IUnknown, (void **)&found))) { /* found */ }
    }
    IDXGIAdapter1_Release(a);
  }
  IDXGIFactory1_Release(f);
  return found;
}

/* ---- the swap chain ---- */
static int create_backbuffers(void)
{
  D3D12_CPU_DESCRIPTOR_HANDLE h;
  int i;

  release_backbuffers();
  h = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap);
  for (i = 0; i < FRAMES; i++)
  {
    if (FAILED(IDXGISwapChain_GetBuffer(swapchain, (UINT)i, &IID_ID3D12Resource, (void **)&backbuf[i])) || !backbuf[i]) return 0;
    ID3D12Device_CreateRenderTargetView(device, backbuf[i], NULL, h);
    h.ptr += rtv_inc;
  }
  return 1;
}

/* A swap chain for window `win` (flip model, two buffers, waitable). Returns 1 with swapchain, swapchain3 and waitable set. */
static int make_swapchain(HWND win, int w, int h, UINT flags)
{
  DXGI_SWAP_CHAIN_DESC1 sd;
  IDXGISwapChain1 *sc1 = NULL;
  IDXGISwapChain2 *sc2 = NULL;

  ZeroMemory(&sd, sizeof(sd));
  sd.Width = (UINT)(w < 1 ? 1 : w);
  sd.Height = (UINT)(h < 1 ? 1 : h);
  sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = FRAMES;
  sd.Scaling = DXGI_SCALING_STRETCH;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  sd.Flags = flags;
  if (FAILED(IDXGIFactory2_CreateSwapChainForHwnd(factory, (IUnknown *)queue, win, &sd, NULL, NULL, &sc1)) || !sc1) return 0;
  if (FAILED(IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain, (void **)&swapchain)) || !swapchain) { IDXGISwapChain1_Release(sc1); swapchain = NULL; return 0; }
  if (FAILED(IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3, (void **)&swapchain3))) swapchain3 = NULL;
  waitable = NULL;
  if ((flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) && SUCCEEDED(IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain2, (void **)&sc2)) && sc2)
  {
    IDXGISwapChain2_SetMaximumFrameLatency(sc2, 1);
    waitable = IDXGISwapChain2_GetFrameLatencyWaitableObject(sc2);
    IDXGISwapChain2_Release(sc2);
  }
  IDXGISwapChain1_Release(sc1);
  IDXGIFactory2_MakeWindowAssociation(factory, win, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);     /* (the program has its own fullscreen) */
  sc_flags = flags;
  wait_stuck = 0;
  swap_w = (int)sd.Width; swap_h = (int)sd.Height;
  return 1;
}

static void drop_swapchain(void)
{
  waitable = NULL;
  release_backbuffers();
  if (swapchain3) { IDXGISwapChain3_Release(swapchain3); swapchain3 = NULL; }
  if (swapchain) { IDXGISwapChain_Release(swapchain); swapchain = NULL; }
}

/* Resizes the swap chain (and the child window) in place. Every reference to the old buffers has to go first, and the GPU has to be done with them. */
static int resize_swapchain(int w, int h)
{
  HRESULT hr;

  if (w < 1) w = 1;
  if (h < 1) h = 1;
  MoveWindow(child_hwnd, 0, browser_toolbar_height(), w, h, FALSE);
  flush_gpu();
  release_backbuffers();
  hr = IDXGISwapChain_ResizeBuffers(swapchain, FRAMES, (UINT)w, (UINT)h, DXGI_FORMAT_B8G8R8A8_UNORM, sc_flags);
  if (FAILED(hr))
  {
    gui_log("Direct3D 12: the swap chain could not be resized (0x%08lX)", (unsigned long)hr);
    release_device();
    return 0;
  }
  swap_w = w; swap_h = h;
  return create_backbuffers();
}

/* ---- the pipeline ---- */
static int compile_shader(const char *src, const char *entry, const char *target, ID3DBlob **out)
{
  ID3DBlob *errors = NULL;
  HRESULT hr = d3dcompile_fn(src, strlen(src), NULL, NULL, NULL, entry, target, 0, 0, out, &errors);

  if (errors)
  {
    if (FAILED(hr)) gui_log("Direct3D 12: the shader %s did not compile: %.300s", entry, (const char *)ID3D10Blob_GetBufferPointer(errors));
    ID3D10Blob_Release(errors);
  }
  else if (FAILED(hr)) gui_log("Direct3D 12: the shader %s did not compile (0x%08lX)", entry, (unsigned long)hr);
  return SUCCEEDED(hr);
}

static ID3D12PipelineState *make_pso(ID3DBlob *vs, ID3DBlob *ps, int alpha)
{
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
  ID3D12PipelineState *p = NULL;
  HRESULT hr;

  ZeroMemory(&pd, sizeof(pd));
  pd.pRootSignature = root_sig;
  pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vs); pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vs);
  pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(ps); pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(ps);
  pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pd.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;                 /* (valid values also when blending is off) */
  pd.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
  pd.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
  pd.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
  pd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
  pd.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
  pd.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
  if (alpha)
  {
    pd.BlendState.RenderTarget[0].BlendEnable = TRUE;
    pd.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    pd.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
  }
  pd.SampleMask = 0xFFFFFFFFu;
  pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pd.RasterizerState.DepthClipEnable = TRUE;
  pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pd.NumRenderTargets = 1;
  pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
  pd.SampleDesc.Count = 1;
  hr = ID3D12Device_CreateGraphicsPipelineState(device, &pd, &IID_ID3D12PipelineState, (void **)&p);
  if (FAILED(hr)) { gui_log("Direct3D 12: a pipeline state could not be made (0x%08lX)", (unsigned long)hr); return NULL; }
  return p;
}

static int create_pipeline(void)
{
  D3D12_ROOT_PARAMETER params[2];
  D3D12_DESCRIPTOR_RANGE range;
  D3D12_STATIC_SAMPLER_DESC ss[2];
  D3D12_ROOT_SIGNATURE_DESC rsd;
  D3D12_DESCRIPTOR_HEAP_DESC hd;
  ID3DBlob *rs_blob = NULL, *rs_err = NULL, *vs = NULL, *ps = NULL;
  HRESULT hr;
  int i;

  /* the root signature: eight root constants (the quad's rectangle and the filter choice), the picture's texture, and the two samplers */
  ZeroMemory(&range, sizeof(range));
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = 1;
  range.BaseShaderRegister = 0;
  range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
  ZeroMemory(params, sizeof(params));
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.ShaderRegister = 0;
  params[0].Constants.Num32BitValues = 8;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 1;
  params[1].DescriptorTable.pDescriptorRanges = &range;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  ZeroMemory(ss, sizeof(ss));
  for (i = 0; i < 2; i++)
  {
    ss[i].Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss[i].AddressU = ss[i].AddressV = ss[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss[i].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    ss[i].MaxAnisotropy = 1;
    ss[i].BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    ss[i].MaxLOD = D3D12_FLOAT32_MAX;
    ss[i].ShaderRegister = (UINT)i;
    ss[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  }
  ZeroMemory(&rsd, sizeof(rsd));
  rsd.NumParameters = 2; rsd.pParameters = params;
  rsd.NumStaticSamplers = 2; rsd.pStaticSamplers = ss;
  rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  hr = serialize_rs_fn(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &rs_err);
  if (rs_err) ID3D10Blob_Release(rs_err);
  if (FAILED(hr) || !rs_blob) { gui_log("Direct3D 12: the root signature could not be made (0x%08lX)", (unsigned long)hr); return 0; }
  hr = ID3D12Device_CreateRootSignature(device, 0, ID3D10Blob_GetBufferPointer(rs_blob), ID3D10Blob_GetBufferSize(rs_blob), &IID_ID3D12RootSignature, (void **)&root_sig);
  ID3D10Blob_Release(rs_blob);
  if (FAILED(hr)) { gui_log("Direct3D 12: the root signature was refused (0x%08lX)", (unsigned long)hr); return 0; }

  /* the descriptor heaps: the picture and the two overlays (shader visible), the back buffers */
  ZeroMemory(&hd, sizeof(hd));
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = 1 + FRAMES * OV_MAX;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(ID3D12Device_CreateDescriptorHeap(device, &hd, &IID_ID3D12DescriptorHeap, (void **)&srv_heap))) return 0;
  srv_inc = ID3D12Device_GetDescriptorHandleIncrementSize(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  ZeroMemory(&hd, sizeof(hd));
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  hd.NumDescriptors = FRAMES;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  if (FAILED(ID3D12Device_CreateDescriptorHeap(device, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap))) return 0;
  rtv_inc = ID3D12Device_GetDescriptorHandleIncrementSize(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

  /* the shaders and the pipeline states */
  if (!compile_shader(vs_src, "main", "vs_5_0", &vs)) return 0;
  if (!compile_shader(ps_src, "main", "ps_5_0", &ps)) { ID3D10Blob_Release(vs); return 0; }
  pso_base[0] = make_pso(vs, ps, 0);
  pso_base[1] = make_pso(vs, ps, 1);
  ID3D10Blob_Release(ps);
  if (!pso_base[0] || !pso_base[1]) { ID3D10Blob_Release(vs); return 0; }
  {
    static const char *const entries[3] = { "main_catmull", "main_sharpbil", "main_sharpnear" };

    for (i = 0; i < 3; i++)                      /* (an extra: when one does not compile, the picture is scaled with the bilinear filter instead) */
    {
      ID3DBlob *rb = NULL;
      if (compile_shader(ps_resample_src, entries[i], "ps_5_0", &rb))
      {
        pso_resample[i] = make_pso(vs, rb, 0);
        ID3D10Blob_Release(rb);
      }
    }
  }
  ID3D10Blob_Release(vs);
  return 1;
}

/* Makes the device, the queue, the command list and the swap chain for the child window. */
/* The flags of the windowed swap chain: with variable refresh wanted (and the display chain able to) it can tear. Sets vrr_active. */
static UINT windowed_flags(void)
{
  UINT flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

  vrr_active = 0;
  if (vrr_want && factory)
  {
    IDXGIFactory5 *f5 = NULL;
    BOOL allow = FALSE;
    if (SUCCEEDED(IDXGIFactory2_QueryInterface(factory, &IID_IDXGIFactory5, (void **)&f5)) && f5)
    {
      if (SUCCEEDED(IDXGIFactory5_CheckFeatureSupport(f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))) && allow) { flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING; vrr_active = 1; }
      IDXGIFactory5_Release(f5);
    }
  }
  return flags;
}

static int create_device(void)
{
  IUnknown *pick;
  IDXGIFactory1 *f1 = NULL;
  UINT flags;
  D3D12_COMMAND_QUEUE_DESC qd;
  HRESULT hr;
  int i;

  pick = pick_adapter();
  hr = create_device_fn(pick, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&device);
  if (pick) IUnknown_Release(pick);
  if (FAILED(hr) && pick) hr = create_device_fn(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&device);     /* the chosen card cannot make it: Windows picks */
  if (FAILED(hr) || !device) { gui_log("Direct3D 12: no device (0x%08lX)", (unsigned long)hr); device = NULL; return 0; }

  ZeroMemory(&qd, sizeof(qd));
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  if (FAILED(ID3D12Device_CreateCommandQueue(device, &qd, &IID_ID3D12CommandQueue, (void **)&queue))) { gui_log("Direct3D 12: no command queue"); return 0; }
  if (FAILED(ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence))) return 0;
  fence_event = CreateEventA(NULL, FALSE, FALSE, NULL);
  if (!fence_event) return 0;
  for (i = 0; i < FRAMES; i++)
    if (FAILED(ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&fr[i].alloc))) return 0;
  if (FAILED(ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, fr[0].alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list))) return 0;
  ID3D12GraphicsCommandList_Close(list);

  if (FAILED(create_factory_fn(&IID_IDXGIFactory1, (void **)&f1)) || !f1) return 0;
  hr = IDXGIFactory1_QueryInterface(f1, &IID_IDXGIFactory2, (void **)&factory);
  IDXGIFactory1_Release(f1);
  if (FAILED(hr) || !factory) { gui_log("Direct3D 12: DXGI 1.2 is not here"); factory = NULL; return 0; }

  flags = windowed_flags();
  if (!make_swapchain(child_hwnd, 1, 1, flags)) { gui_log("Direct3D 12: the swap chain could not be made"); vrr_active = 0; return 0; }
  return 1;
}

/* ---- the picture ---- */
static void put_in_clip(float *c, const RECT *dest)
{
  c[0] = ((float)dest->left   / (float)swap_w) * 2.0f - 1.0f;
  c[2] = ((float)dest->right  / (float)swap_w) * 2.0f - 1.0f;
  c[1] = 1.0f - ((float)dest->top    / (float)swap_h) * 2.0f;
  c[3] = 1.0f - ((float)dest->bottom / (float)swap_h) * 2.0f;
}

static void draw_quad(const RECT *dest, int srv_index, ID3D12PipelineState *pso, int linear)
{
  float c[8];
  D3D12_VIEWPORT vp;
  D3D12_RECT sc;
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap);
  D3D12_GPU_DESCRIPTOR_HANDLE gpu = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(srv_heap);
  ID3D12DescriptorHeap *heaps[1];
  UINT bi = swapchain3 ? IDXGISwapChain3_GetCurrentBackBufferIndex(swapchain3) : 0;

  rtv.ptr += (SIZE_T)bi * rtv_inc;
  gpu.ptr += (UINT64)srv_index * srv_inc;
  put_in_clip(c, dest);
  c[4] = linear ? 1.0f : 0.0f; c[5] = c[6] = c[7] = 0.0f;

  vp.TopLeftX = 0; vp.TopLeftY = 0; vp.Width = (float)swap_w; vp.Height = (float)swap_h; vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
  sc.left = 0; sc.top = 0; sc.right = swap_w; sc.bottom = swap_h;
  heaps[0] = srv_heap;

  ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, root_sig);
  ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, heaps);
  ID3D12GraphicsCommandList_SetPipelineState(list, pso);
  ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 8, c, 0);
  ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 1, gpu);
  ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, NULL);
  ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
  ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
  ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  ID3D12GraphicsCommandList_DrawInstanced(list, 4, 1, 0, 0);
}

/* The command list of a frame that was begun and not presented is dropped: nothing of it has run. */
static void abandon_frame(void)
{
  if (!recording) return;
  ID3D12GraphicsCommandList_Close(list);
  recording = 0;
}

static int ensure_texture(int w, int h)
{
  if (tex && tex_w == w && tex_h == h) return 1;
  flush_gpu();                                   /* (frames in flight may still read the old one) */
  if (tex) { ID3D12Resource_Release(tex); tex = NULL; }
  tex = make_texture(w, h, tex_fmt);
  if (!tex) { tex_w = 0; tex_h = 0; return 0; }
  srv_for(tex, tex_fmt, SLOT_FRAME);
  tex_w = w; tex_h = h;
  return 1;
}

/* The next overlay texture of this frame, of this size (the one it had in the frame slot's last frame when that fits). */
static ov_t *next_overlay(int w, int h)
{
  ov_t *e;

  if (ov_used >= OV_MAX) return NULL;
  e = &ovp[cur_slot][ov_used];
  if (!e->tex || e->w != w || e->h != h)
  {
    if (e->tex) { ID3D12Resource_Release(e->tex); e->tex = NULL; }       /* (the last frame of this slot has finished: wait_fence() in render_frame()) */
    e->tex = make_texture(w, h, DXGI_FORMAT_B8G8R8A8_UNORM);
    if (!e->tex) { e->w = 0; e->h = 0; return NULL; }
    srv_for(e->tex, DXGI_FORMAT_B8G8R8A8_UNORM, SLOT_OVERLAY(cur_slot, ov_used));
    e->w = w; e->h = h;
  }
  ov_used++;
  return e;
}

static void lost_check(void)
{
  if (device && FAILED(ID3D12Device_GetDeviceRemovedReason(device)))
  {
    release_device();
    device_ok = 0;
  }
}

int d3d12_render_frame(const uint16 *src_pixels, int src_pitch, int src_w, int src_h, const RECT *dest, int client_w, int client_h, int smooth)
{
  unsigned bar = video_bar_color();
  FLOAT clear[4];
  D3D12_RESOURCE_BARRIER b;
  D3D12_CPU_DESCRIPTOR_HANDLE rtv;
  int slot, top;
  UINT bi;
  ID3D12PipelineState *pso;

  clear[0] = ((bar >> 16) & 0xFF) / 255.0f; clear[1] = ((bar >> 8) & 0xFF) / 255.0f; clear[2] = (bar & 0xFF) / 255.0f; clear[3] = 1.0f;

  if (!device_ok) return 0;
  if (excl_active) { client_w = swap_w; client_h = swap_h; }       /* (the whole monitor: the mode's size, whatever the window says) */
  if (client_w < 1 || client_h < 1) return 0;
  if (src_w < 1 || src_h < 1 || src_w > 8192 || src_h > 8192) return 0;

  abandon_frame();
  if (client_w != swap_w || client_h != swap_h)
  {
    if (!resize_swapchain(client_w, client_h)) { device_ok = 0; return 0; }
  }
  top = browser_toolbar_height();                 /* the surface sits below the toolbar; it moves when the toolbar appears or goes */
  if (top != child_top)
  {
    MoveWindow(child_hwnd, 0, top, swap_w, swap_h, FALSE);
    child_top = top;
  }
  if (!backbuf[0] || !swapchain3) return 0;

  /* a frame latency of one: wait until the swap chain can take another frame (not when VSync is off or the display follows the game) */
  if (waitable && present_sync && !vrr_active && wait_stuck < 3)
  {
    if (WaitForSingleObjectEx(waitable, 100, TRUE) == WAIT_OBJECT_0) wait_stuck = 0;
    else if (++wait_stuck == 3) gui_log("Direct3D 12: the swap chain's waitable object does not come; Present() paces the frames instead");
  }

  slot = (int)(frame_no % FRAMES);
  wait_fence(fr[slot].fence);
  {
    int i;
    for (i = 0; i < fr[slot].up_n; i++) fr[slot].up[i].used = 0;      /* this slot's upload memory is free again */
  }
  if (FAILED(ID3D12CommandAllocator_Reset(fr[slot].alloc)) || FAILED(ID3D12GraphicsCommandList_Reset(list, fr[slot].alloc, NULL)))
  {
    lost_check();
    return 0;
  }
  recording = 1;
  cur_slot = slot;
  ov_used = 0;

  bi = IDXGISwapChain3_GetCurrentBackBufferIndex(swapchain3);
  rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap);
  rtv.ptr += (SIZE_T)bi * rtv_inc;
  b = transition(backbuf[bi], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
  ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
  ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, NULL);
  ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, clear, 0, NULL);

  if (!ensure_texture(src_w, src_h) ||
      !record_upload(tex, tex_fmt, src_w, src_h, src_pixels, src_pitch, tex_565 ? 2 : 4, !tex_565))
  {
    abandon_frame();
    lost_check();
    return 0;
  }

  if (!shader_chain_active() ||
      !shader_chain_render12((void *)list, (void *)tex, (unsigned long long)rtv.ptr, (int)DXGI_FORMAT_B8G8R8A8_UNORM, swap_w, swap_h,
                             dest->left, dest->top, dest->right - dest->left, dest->bottom - dest->top))
  {
    int k = smooth == 1 ? 2 : (smooth == 3 ? 1 : (smooth == 4 ? 0 : -1));         /* (the pixel shader of the Scaling Filter, when it has one) */

    pso = (k >= 0 && pso_resample[k]) ? pso_resample[k] : pso_base[0];
    draw_quad(dest, SLOT_FRAME, pso, smooth != 0);
  }
  return 1;
}

int d3d12_draw_overlay(const uint32 *argb_pixels, int argb_pitch, int w, int h, const RECT *screen_area)
{
  ov_t *e;

  if (!device_ok || !recording) return 0;
  if (w < 1 || h < 1) return 0;
  e = next_overlay(w, h);
  if (!e) return 0;
  if (!record_upload(e->tex, DXGI_FORMAT_B8G8R8A8_UNORM, w, h, argb_pixels, argb_pitch, 4, 0)) return 0;
  draw_quad(screen_area, SLOT_OVERLAY(cur_slot, ov_used - 1), pso_base[1], 0);        /* (point sampled: 1:1 pixel text and pictures) */
  return 1;
}

void d3d12_present(void)
{
  D3D12_RESOURCE_BARRIER b;
  ID3D12CommandList *lists[1];
  UINT bi;
  HRESULT hr;

  if (!device_ok || !recording) return;
  bi = swapchain3 ? IDXGISwapChain3_GetCurrentBackBufferIndex(swapchain3) : 0;
  b = transition(backbuf[bi], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
  ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
  if (FAILED(ID3D12GraphicsCommandList_Close(list))) { recording = 0; lost_check(); return; }
  recording = 0;
  lists[0] = (ID3D12CommandList *)list;
  ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);

  if (vrr_active) hr = IDXGISwapChain_Present(swapchain, 0, DXGI_PRESENT_ALLOW_TEARING);      /* the display follows: no wait */
  else            hr = IDXGISwapChain_Present(swapchain, (UINT)present_sync, 0);

  {
    static HRESULT last_hr;
    if (hr != last_hr)
    {
      last_hr = hr;
      gui_log("Direct3D 12: Present returned 0x%08lX%s", (unsigned long)hr, excl_active ? " (True Exclusive Fullscreen)" : "");
    }
  }
  fence_val++;
  if (SUCCEEDED(ID3D12CommandQueue_Signal(queue, fence, fence_val))) fr[cur_slot].fence = fence_val;
  frame_no++;

  /* In True Exclusive Fullscreen the swap chain can answer INVALID_CALL for every frame after the mode change (the picture then stays as it was): the buffers
     are made again to the screen's size; when that does not help either, the screen is given back and the game goes on in a borderless window. */
  if (excl_active && hr == DXGI_ERROR_INVALID_CALL)
  {
    BOOL fs = FALSE;
    HRESULT hr2 = E_FAIL;

    if (++excl_bad == 1)
    {
      IDXGIOutput *out = NULL;
      IDXGISwapChain_GetFullscreenState(swapchain, &fs, &out);
      if (out) IDXGIOutput_Release(out);
      flush_gpu();
      release_backbuffers();
      hr2 = IDXGISwapChain_ResizeBuffers(swapchain, FRAMES, 0, 0, DXGI_FORMAT_UNKNOWN, sc_flags);
      gui_log("Direct3D 12: Present failed in True Exclusive Fullscreen (fullscreen state %d); the buffers are made again: 0x%08lX", (int)fs, (unsigned long)hr2);
      if (FAILED(hr2) || !create_backbuffers()) excl_bad = 99;
    }
    if (excl_bad >= 30)
    {
      gui_log("Direct3D 12: True Exclusive Fullscreen does not present on this PC: the screen is given back");
      d3d12_exclusive_leave();
      return;
    }
  }
  else if (excl_active && SUCCEEDED(hr)) excl_bad = 0;

  if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)      /* the graphics driver was reset or the GPU went away */
  {
    release_device();
    device_ok = 0;                 /* video.c brings the device back */
  }
}

/* ---- real exclusive fullscreen: see d3d11_video.c; the same, with a flip-model swap chain on a window of its own ---- */
long d3d12_exclusive_enter(HWND win, int w, int h, int hz_num, int hz_den)
{
  DXGI_MODE_DESC md;
  HRESULT hr;
  IDXGISwapChain *old_sc = swapchain;
  IDXGISwapChain3 *old_sc3 = swapchain3;
  HANDLE old_wait = waitable;
  UINT old_flags = sc_flags;
  int old_w = swap_w, old_h = swap_h;

  if (!device_ok || !device || !swapchain || vrr_active || excl_active || !win || w < 1 || h < 1) return (long)E_FAIL;

  flush_gpu();
  abandon_frame();
  release_backbuffers();
  swapchain = NULL; swapchain3 = NULL; waitable = NULL;
  if (!make_swapchain(win, w, h, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH))
  {
    swapchain = old_sc; swapchain3 = old_sc3; waitable = old_wait; sc_flags = old_flags; swap_w = old_w; swap_h = old_h;
    create_backbuffers();
    return (long)E_FAIL;
  }
  ZeroMemory(&md, sizeof(md));
  md.Width = (UINT)w; md.Height = (UINT)h;
  md.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  md.RefreshRate.Numerator = (UINT)hz_num;
  md.RefreshRate.Denominator = (UINT)(hz_den > 0 ? hz_den : 1);
  hr = IDXGISwapChain_ResizeTarget(swapchain, &md);
  if (SUCCEEDED(hr)) hr = IDXGISwapChain_SetFullscreenState(swapchain, TRUE, NULL);
  if (SUCCEEDED(hr)) hr = IDXGISwapChain_ResizeBuffers(swapchain, FRAMES, (UINT)w, (UINT)h, DXGI_FORMAT_B8G8R8A8_UNORM, sc_flags);
  if (FAILED(hr) || !create_backbuffers())
  {
    if (swapchain) IDXGISwapChain_SetFullscreenState(swapchain, FALSE, NULL);        /* (a swap chain must not be let go of while fullscreen) */
    drop_swapchain();                            /* the new one goes, the windowed one comes back */
    swapchain = old_sc; swapchain3 = old_sc3; waitable = old_wait; sc_flags = old_flags; swap_w = old_w; swap_h = old_h;
    create_backbuffers();
    return (long)(FAILED(hr) ? hr : E_FAIL);
  }
  if (old_sc3) IDXGISwapChain3_Release(old_sc3);
  if (old_sc) IDXGISwapChain_Release(old_sc);
  excl_active = 1;
  excl_bad = 0;
  swap_w = w; swap_h = h;
  return 0;
}

/* Variable Refresh Rate was switched: only the swap chain is made again (same device, same picture window, so nothing flashes). 0 = it could not be: make everything again. */
int d3d12_apply_vrr(void)
{
  RECT rc;

  if (!device_ok || !device || !factory || !child_hwnd || excl_active) return 0;
  flush_gpu();
  abandon_frame();
  drop_swapchain();
  GetClientRect(child_hwnd, &rc);
  if (!make_swapchain(child_hwnd, rc.right, rc.bottom, windowed_flags()) || !create_backbuffers()) return 0;
  return 1;
}

/* Back to the windowed swap chain on the child window. */
void d3d12_exclusive_leave(void)
{
  RECT rc;

  if (!excl_active) return;
  if (swapchain) IDXGISwapChain_SetFullscreenState(swapchain, FALSE, NULL);
  excl_active = 0;
  if (!device_ok || !device) return;
  flush_gpu();
  abandon_frame();
  drop_swapchain();
  GetClientRect(child_hwnd, &rc);
  if (!make_swapchain(child_hwnd, rc.right, rc.bottom, windowed_flags()) || !create_backbuffers())
  {
    release_device();            /* video.c makes the whole device again */
    device_ok = 0;
  }
}

void d3d12_show_surface(int on)
{
  if (child_hwnd) ShowWindow(child_hwnd, on ? SW_SHOWNA : SW_HIDE);
}

/* ---- start and stop ---- */
static void remember_child(void)
{
  RECT rc;
  POINT p;

  child_keep_ok = 0;
  if (!child_hwnd || !parent_hwnd || !IsWindowVisible(child_hwnd)) return;
  GetClientRect(child_hwnd, &rc);
  p.x = 0; p.y = 0;
  MapWindowPoints(child_hwnd, parent_hwnd, &p, 1);
  if (rc.right < 8 || rc.bottom < 8) return;
  child_keep.left = p.x; child_keep.top = p.y; child_keep.right = p.x + rc.right; child_keep.bottom = p.y + rc.bottom;
  child_keep_ok = 1;
}

int d3d12_init(HWND hwnd)
{
  device_ok = 0;
  parent_hwnd = hwnd;

  if (device) release_device();
  if (child_hwnd) { remember_child(); DestroyWindow(child_hwnd); child_hwnd = NULL; }
  if (!load_libraries()) { gui_log("Direct3D 12: d3d12.dll is not here (it needs Windows 10)"); return 0; }
  ensure_child_class();
  if (!child_class) return 0;
  if (child_keep_ok)
    child_hwnd = CreateWindowExA(0, CHILD_CLASS_NAME, "", WS_CHILD | WS_VISIBLE, child_keep.left, child_keep.top, child_keep.right - child_keep.left, child_keep.bottom - child_keep.top, hwnd, NULL, GetModuleHandleA(NULL), NULL);
  else
    child_hwnd = CreateWindowExA(0, CHILD_CLASS_NAME, "", WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, hwnd, NULL, GetModuleHandleA(NULL), NULL);
  if (!child_hwnd) return 0;
  child_top = child_keep_ok ? child_keep.top : 0;

  if (!create_device() || !create_pipeline() || !create_backbuffers())
  {
    release_device();
    DestroyWindow(child_hwnd);
    child_hwnd = NULL;
    return 0;
  }

  /* RGB565 as it is when the GPU says it can sample it; otherwise it is made 32 bits on the CPU for each frame */
  {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs;
    ZeroMemory(&fs, sizeof(fs));
    fs.Format = DXGI_FORMAT_B5G6R5_UNORM;
    tex_565 = SUCCEEDED(ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))) &&
              (fs.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) && (fs.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE);
    tex_fmt = tex_565 ? DXGI_FORMAT_B5G6R5_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
  }

  device_ok = 1;
  shader_chain_init();
  if (shader_chain_preset_path()[0]) shader_chain_load12((void *)device, shader_chain_preset_path());
  return 1;
}

void d3d12_shutdown(void)
{
  release_device();
  if (child_hwnd) { remember_child(); DestroyWindow(child_hwnd); child_hwnd = NULL; }
  device_ok = 0;
}
