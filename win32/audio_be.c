/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  audio_be.c -- the two modern sound outputs: XAudio2 and WASAPI (shared).
 *
 *  Both give audio.c the same few calls the old waveOut code gave it. The
 *  main loop paces emulation on "how many frames of sound are still queued",
 *  so each backend reports that as a count of submitted chunks (one per
 *  emulated frame) that have not finished playing yet.
 ****************************************************************************/

#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <xaudio2.h>
#include <propidl.h>

#include "audio_be.h"

#define RING 16                         /* chunks that can be in flight */
#define CHUNK_FRAMES_MAX 2048
#define CHUNK_BYTES (CHUNK_FRAMES_MAX * 2 * (int)sizeof(short))

/* The few interface ids needed, spelled out here so no import library has to supply them. */
static const GUID MY_CLSID_MMDeviceEnumerator = { 0xbcde0395, 0xe52f, 0x467c, { 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e } };
static const GUID MY_IID_IMMDeviceEnumerator  = { 0xa95664d2, 0x9614, 0x4f35, { 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6 } };
static const GUID MY_IID_IAudioClient         = { 0x1cb9ad4c, 0xdbfa, 0x4c32, { 0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2 } };
static const GUID MY_IID_IAudioRenderClient   = { 0xf294acfc, 0x3146, 0x4483, { 0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2 } };

static int be_kind;                     /* 0 none, 1 XAudio2, 2 WASAPI */
static int be_rate;

static void be_log(const char *what, HRESULT hr)
{
  (void)what; (void)hr;
}

/* ---- finding the output device the user chose ---------------------------------------------------------------- */

static const PROPERTYKEY pkey_friendly =
  { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

/* The render endpoint whose name starts like `name` (waveOut cuts names to 31 characters), else the default one. */
static IMMDevice *find_endpoint(IMMDeviceEnumerator *en, const char *name)
{
  IMMDevice *dev = NULL;

  if (name && name[0])
  {
    IMMDeviceCollection *col = NULL;
    if (SUCCEEDED(IMMDeviceEnumerator_EnumAudioEndpoints(en, eRender, DEVICE_STATE_ACTIVE, &col)) && col)
    {
      UINT i, n = 0;
      IMMDeviceCollection_GetCount(col, &n);
      for (i = 0; i < n && !dev; i++)
      {
        IMMDevice *d = NULL;
        IPropertyStore *ps = NULL;
        if (FAILED(IMMDeviceCollection_Item(col, i, &d)) || !d) continue;
        if (SUCCEEDED(IMMDevice_OpenPropertyStore(d, STGM_READ, &ps)) && ps)
        {
          PROPVARIANT pv;
          PropVariantInit(&pv);
          if (SUCCEEDED(IPropertyStore_GetValue(ps, &pkey_friendly, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal)
          {
            char friendly[256];
            WideCharToMultiByte(CP_ACP, 0, pv.pwszVal, -1, friendly, sizeof(friendly), NULL, NULL);
            if (!strncmp(friendly, name, strlen(name))) { dev = d; IMMDevice_AddRef(dev); }
          }
          PropVariantClear(&pv);
          IPropertyStore_Release(ps);
        }
        IMMDevice_Release(d);
      }
      IMMDeviceCollection_Release(col);
    }
  }
  if (!dev) IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
  return dev;
}

/* The full names of the active output devices (waveOut cuts them to 31 characters); returns how many, 0 when they cannot be listed. */
int be_list_endpoints(char names[][96], int max)
{
  IMMDeviceEnumerator *en = NULL;
  IMMDeviceCollection *col = NULL;
  int com = SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)), k = 0;

  if (SUCCEEDED(CoCreateInstance(&MY_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &MY_IID_IMMDeviceEnumerator, (void **)&en)) && en)
  {
    if (SUCCEEDED(IMMDeviceEnumerator_EnumAudioEndpoints(en, eRender, DEVICE_STATE_ACTIVE, &col)) && col)
    {
      UINT i, n = 0;
      IMMDeviceCollection_GetCount(col, &n);
      for (i = 0; i < n && k < max; i++)
      {
        IMMDevice *d = NULL;
        IPropertyStore *ps = NULL;
        if (FAILED(IMMDeviceCollection_Item(col, i, &d)) || !d) continue;
        if (SUCCEEDED(IMMDevice_OpenPropertyStore(d, STGM_READ, &ps)) && ps)
        {
          PROPVARIANT pv;
          PropVariantInit(&pv);
          if (SUCCEEDED(IPropertyStore_GetValue(ps, &pkey_friendly, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal)
          {
            WideCharToMultiByte(CP_ACP, 0, pv.pwszVal, -1, names[k], 96, NULL, NULL);
            names[k][95] = '\0';
            if (names[k][0]) k++;
          }
          PropVariantClear(&pv);
          IPropertyStore_Release(ps);
        }
        IMMDevice_Release(d);
      }
      IMMDeviceCollection_Release(col);
    }
    IMMDeviceEnumerator_Release(en);
  }
  if (com) CoUninitialize();
  return k;
}

static WAVEFORMATEX make_format(int rate)
{
  WAVEFORMATEX w;
  ZeroMemory(&w, sizeof(w));
  w.wFormatTag      = WAVE_FORMAT_PCM;
  w.nChannels       = 2;
  w.nSamplesPerSec  = (DWORD)rate;
  w.wBitsPerSample  = 16;
  w.nBlockAlign     = (WORD)(w.nChannels * w.wBitsPerSample / 8);
  w.nAvgBytesPerSec = w.nSamplesPerSec * w.nBlockAlign;
  return w;
}

/* ---- XAudio2 ------------------------------------------------------------------------------------------------- */

typedef HRESULT (WINAPI *XA2CREATE)(IXAudio2 **, UINT32, XAUDIO2_PROCESSOR);

static struct
{
  HMODULE                dll;
  IXAudio2              *xa;
  IXAudio2MasteringVoice *mv;
  IXAudio2SourceVoice   *sv;
  short                 *buf[RING];
  unsigned               n_sub;
  int                    com;
} xa;

static void xa_close(void)
{
  int i;
  if (xa.sv) { IXAudio2SourceVoice_Stop(xa.sv, 0, XAUDIO2_COMMIT_NOW); IXAudio2SourceVoice_DestroyVoice(xa.sv); }
  if (xa.mv) IXAudio2MasteringVoice_DestroyVoice(xa.mv);
  if (xa.xa) IXAudio2_Release(xa.xa);
  if (xa.dll) FreeLibrary(xa.dll);
  for (i = 0; i < RING; i++) free(xa.buf[i]);
  if (xa.com) CoUninitialize();
  ZeroMemory(&xa, sizeof(xa));
}

static int xa_open(int rate, const char *devname)
{
  XA2CREATE create;
  WAVEFORMATEX wfx = make_format(rate);
  WCHAR *devid = NULL;
  IMMDeviceEnumerator *en = NULL;
  IMMDevice *dev = NULL;
  HRESULT hr;
  int i;

  ZeroMemory(&xa, sizeof(xa));
  xa.com = SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED));
  xa.dll = LoadLibraryExA("XAudio2_9.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!xa.dll) xa.dll = LoadLibraryExA("XAudio2_8.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!xa.dll) goto fail;
  create = (XA2CREATE)(void *)GetProcAddress(xa.dll, "XAudio2Create");
  if (!create) goto fail;
  hr = create(&xa.xa, 0, XAUDIO2_DEFAULT_PROCESSOR);
  if (FAILED(hr) || !xa.xa) { be_log("XAudio2Create", hr); goto fail; }

  /* A chosen output device is given to XAudio2 by its endpoint id. */
  if (devname && devname[0] &&
      SUCCEEDED(CoCreateInstance(&MY_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &MY_IID_IMMDeviceEnumerator, (void **)&en)) && en)
  {
    dev = find_endpoint(en, devname);
    if (dev) IMMDevice_GetId(dev, &devid);
  }
  hr = IXAudio2_CreateMasteringVoice(xa.xa, &xa.mv, 2, 0, 0, devid, NULL, AudioCategory_GameMedia);
  if (devid) CoTaskMemFree(devid);
  if (dev) IMMDevice_Release(dev);
  if (en) IMMDeviceEnumerator_Release(en);
  if (FAILED(hr) || !xa.mv) { be_log("CreateMasteringVoice", hr); goto fail; }

  hr = IXAudio2_CreateSourceVoice(xa.xa, &xa.sv, &wfx, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
  if (FAILED(hr) || !xa.sv) { be_log("CreateSourceVoice", hr); goto fail; }

  for (i = 0; i < RING; i++)
  {
    xa.buf[i] = (short *)malloc(CHUNK_BYTES);
    if (!xa.buf[i]) goto fail;
  }
  if (FAILED(IXAudio2SourceVoice_Start(xa.sv, 0, XAUDIO2_COMMIT_NOW))) goto fail;
  return 1;

fail:
  xa_close();
  return 0;
}

static int xa_pending(void)
{
  XAUDIO2_VOICE_STATE st;
  if (!xa.sv) return 0;
  IXAudio2SourceVoice_GetState(xa.sv, &st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
  return (int)st.BuffersQueued;
}

static int xa_write(const short *s, int frames)
{
  XAUDIO2_BUFFER b;
  short *dst;
  int bytes = frames * 2 * (int)sizeof(short);

  if (!xa.sv) return 0;
  if (xa_pending() >= RING) return 1;            /* ahead of the device: drop this frame, like waveOut does */
  dst = xa.buf[xa.n_sub % RING];
  memcpy(dst, s, (size_t)bytes);
  ZeroMemory(&b, sizeof(b));
  b.AudioBytes = (UINT32)bytes;
  b.pAudioData = (const BYTE *)dst;
  if (FAILED(IXAudio2SourceVoice_SubmitSourceBuffer(xa.sv, &b, NULL))) return 0;
  xa.n_sub++;
  return 1;
}

static void xa_flush(void)
{
  if (xa.sv) IXAudio2SourceVoice_FlushSourceBuffers(xa.sv);
}

/* ---- WASAPI (shared mode) ------------------------------------------------------------------------------------ */

static struct
{
  IMMDeviceEnumerator *en;
  IMMDevice           *dev;
  IAudioClient        *ac;
  IAudioRenderClient  *rc;
  UINT32               bufsize;
  UINT32               chunk[64];       /* frames of each submitted chunk, oldest first (a ring) */
  unsigned             head, count;
  int                  com;
} wa;

static void wa_close(void)
{
  if (wa.ac) { IAudioClient_Stop(wa.ac); }
  if (wa.rc) IAudioRenderClient_Release(wa.rc);
  if (wa.ac) IAudioClient_Release(wa.ac);
  if (wa.dev) IMMDevice_Release(wa.dev);
  if (wa.en) IMMDeviceEnumerator_Release(wa.en);
  if (wa.com) CoUninitialize();
  ZeroMemory(&wa, sizeof(wa));
}

/* exclusive: the game takes the device for itself (lowest delay, no other program can play sound on it meanwhile).
   need_ms is the room the pacing needs for queued sound; a device that cannot give that much is not used. */
static int wa_open(int rate, const char *devname, int exclusive, int need_ms)
{
  WAVEFORMATEX wfx = make_format(rate);
  HRESULT hr;

  ZeroMemory(&wa, sizeof(wa));
  wa.com = SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED));
  hr = CoCreateInstance(&MY_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &MY_IID_IMMDeviceEnumerator, (void **)&wa.en);
  if (FAILED(hr) || !wa.en) { be_log("enumerator", hr); goto fail; }
  wa.dev = find_endpoint(wa.en, devname);
  if (!wa.dev) goto fail;
  hr = IMMDevice_Activate(wa.dev, &MY_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&wa.ac);
  if (FAILED(hr) || !wa.ac) { be_log("Activate", hr); goto fail; }

  if (exclusive)
  {
    REFERENCE_TIME dur = (REFERENCE_TIME)(need_ms < 60 ? 60 : need_ms) * 10000;

    /* the device must take our exact format: nothing converts it in exclusive mode */
    hr = IAudioClient_IsFormatSupported(wa.ac, AUDCLNT_SHAREMODE_EXCLUSIVE, &wfx, NULL);
    if (hr != S_OK) { be_log("exclusive format", hr); goto fail; }
    hr = IAudioClient_Initialize(wa.ac, AUDCLNT_SHAREMODE_EXCLUSIVE, 0, dur, 0, &wfx, NULL);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED)
    {
      /* the device wants whole periods: ask again with the size it reports */
      UINT32 frames = 0;
      IAudioClient_GetBufferSize(wa.ac, &frames);
      IAudioClient_Release(wa.ac);
      wa.ac = NULL;
      if (!frames || FAILED(IMMDevice_Activate(wa.dev, &MY_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&wa.ac)) || !wa.ac) goto fail;
      dur = (REFERENCE_TIME)(10000000.0 * frames / rate + 0.5);
      hr = IAudioClient_Initialize(wa.ac, AUDCLNT_SHAREMODE_EXCLUSIVE, 0, dur, 0, &wfx, NULL);
    }
    if (FAILED(hr)) { be_log("Initialize (exclusive)", hr); goto fail; }
    if (FAILED(IAudioClient_GetBufferSize(wa.ac, &wa.bufsize))) goto fail;
    if ((long long)wa.bufsize * 1000 / rate < need_ms) goto fail;      /* too little room for the pacing */
  }
  else
  {
    /* 400 ms of room covers the deepest latency setting; the engine converts our rate and format as needed. */
    hr = IAudioClient_Initialize(wa.ac, AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                 4000000, 0, &wfx, NULL);
    if (FAILED(hr)) { be_log("Initialize", hr); goto fail; }
    if (FAILED(IAudioClient_GetBufferSize(wa.ac, &wa.bufsize))) goto fail;
  }
  hr = IAudioClient_GetService(wa.ac, &MY_IID_IAudioRenderClient, (void **)&wa.rc);
  if (FAILED(hr) || !wa.rc) goto fail;
  if (FAILED(IAudioClient_Start(wa.ac))) goto fail;
  return 1;

fail:
  wa_close();
  return 0;
}

static int wa_pending(void)
{
  UINT32 pad = 0, acc = 0;
  unsigned n = 0, i;

  if (!wa.ac) return 0;
  if (FAILED(IAudioClient_GetCurrentPadding(wa.ac, &pad))) return 0;
  /* the newest chunks that together cover what is still queued */
  for (i = 0; i < wa.count && acc < pad; i++)
  {
    acc += wa.chunk[(wa.head + wa.count - 1 - i) % 64];
    n++;
  }
  wa.head = (wa.head + wa.count - n) % 64;      /* forget the chunks that have finished */
  wa.count = n;
  return (int)n;
}

static int wa_write(const short *s, int frames)
{
  UINT32 pad = 0;
  BYTE *p = NULL;

  if (!wa.ac || !wa.rc) return 0;
  if (FAILED(IAudioClient_GetCurrentPadding(wa.ac, &pad))) return 0;
  if (wa.bufsize - pad < (UINT32)frames || wa.count >= 64) return 1;      /* ahead of the device: drop the frame */
  if (FAILED(IAudioRenderClient_GetBuffer(wa.rc, (UINT32)frames, &p)) || !p) return 0;
  memcpy(p, s, (size_t)frames * 2 * sizeof(short));
  IAudioRenderClient_ReleaseBuffer(wa.rc, (UINT32)frames, 0);
  wa.chunk[(wa.head + wa.count) % 64] = (UINT32)frames;
  wa.count++;
  return 1;
}

static void wa_flush(void)
{
  if (!wa.ac) return;
  IAudioClient_Stop(wa.ac);
  IAudioClient_Reset(wa.ac);
  wa.head = wa.count = 0;
  IAudioClient_Start(wa.ac);
}

/* ---- the calls audio.c uses ---------------------------------------------------------------------------------- */

int be_open(int kind, int rate, const char *devname, int need_ms)
{
  be_close();
  if (kind == 1 && xa_open(rate, devname)) { be_kind = 1; be_rate = rate; return 1; }
  /* kind 3, exclusive: when the device will not give itself up (or will not take the format), shared mode is used */
  if (kind == 3 && wa_open(rate, devname, 1, need_ms)) { be_kind = 2; be_rate = rate; return 1; }
  if ((kind == 2 || kind == 3) && wa_open(rate, devname, 0, need_ms)) { be_kind = 2; be_rate = rate; return 1; }
  return 0;
}

void be_close(void)
{
  if (be_kind == 1) xa_close();
  else if (be_kind == 2) wa_close();
  be_kind = 0;
}

int  be_active(void) { return be_kind; }
int  be_rate_now(void) { return be_kind ? be_rate : 0; }
int  be_pending(void) { return be_kind == 1 ? xa_pending() : (be_kind == 2 ? wa_pending() : 0); }
int  be_write(const short *s, int frames) { return be_kind == 1 ? xa_write(s, frames) : (be_kind == 2 ? wa_write(s, frames) : 0); }
void be_flush(void) { if (be_kind == 1) xa_flush(); else if (be_kind == 2) wa_flush(); }
