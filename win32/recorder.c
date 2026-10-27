/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  recorder.c -- audio and video+audio recording. See recorder.h.
 *
 *  Threading: the emulation thread only copies each frame's picture and audio
 *  into queue items. Everything else -- picture conversion, the encoders, the
 *  file -- happens on the recorder thread, which owns the file and (for
 *  video) the Media Foundation objects from start to finish.
 ****************************************************************************/

#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include "recorder.h"

/* Windows 7 Media Foundation API version (MF_SDK_VERSION 2, MF_API_VERSION 0x70). */
#define REC_MF_VERSION  0x00020070

#define QUEUE_CAP       32        /* items waiting for the recorder thread */
#define WAIT_MS         1500      /* longest the emulation waits on a full queue */
#define STOP_WAIT_MS    30000     /* longest stopping waits for the file to be finished */

enum { ITEM_VIDEO, ITEM_AUDIO, ITEM_STOP };

typedef struct
{
  int   kind;
  int   w, h;        /* video: picture size */
  int   frames;      /* audio: sample pairs */
  void *data;
} rec_item;

/* Media Foundation entry points, resolved at run time. */
typedef HRESULT (WINAPI *PF_MFStartup)(ULONG, DWORD);
typedef HRESULT (WINAPI *PF_MFShutdown)(void);
typedef HRESULT (WINAPI *PF_MFCreateAttributes)(IMFAttributes **, UINT32);
typedef HRESULT (WINAPI *PF_MFCreateMediaType)(IMFMediaType **);
typedef HRESULT (WINAPI *PF_MFCreateSample)(IMFSample **);
typedef HRESULT (WINAPI *PF_MFCreateMemoryBuffer)(DWORD, IMFMediaBuffer **);
typedef HRESULT (WINAPI *PF_MFCreateSinkWriterFromURL)(LPCWSTR, IMFByteStream *, IMFAttributes *, IMFSinkWriter **);

static struct
{
  /* shared */
  int      mode;
  volatile LONG active;
  volatile LONG failed;
  volatile LONG done;          /* a GIF has reached its length limit: stop it (not an error) */
  char     path[MAX_PATH * 2];
  char     error[256];
  int      rate;
  unsigned fps_x1000;          /* frame rate * 1000 */
  int      canvas_w, canvas_h; /* video output size (even) */
  int      start_sw, start_sh; /* the picture size the recording began with */
  unsigned bitrate;

  HANDLE   thread, sem_items, sem_free, ev_init;
  CRITICAL_SECTION cs;
  int      cs_ready;
  rec_item *queue[QUEUE_CAP];
  int      head, tail;

  volatile LONG audio_frames_in;   /* producer-side count, for the elapsed time */

  /* recorder-thread state */
  int      init_ok;
  FILE    *wav;
  unsigned wav_bytes;
  LONGLONG vframes, aframes;

  HMODULE  mfplat, mfrw;
  int      mf_started;
  PF_MFStartup                 pMFStartup;
  PF_MFShutdown                pMFShutdown;
  PF_MFCreateAttributes        pMFCreateAttributes;
  PF_MFCreateMediaType         pMFCreateMediaType;
  PF_MFCreateSample            pMFCreateSample;
  PF_MFCreateMemoryBuffer      pMFCreateMemoryBuffer;
  PF_MFCreateSinkWriterFromURL pMFCreateSinkWriterFromURL;
  IMFSinkWriter *writer;
  DWORD    vstream, astream;

  unsigned char *nv12;
  int      nv12_size;
  int     *xmap;
  int      geo_sw, geo_sh, geo_dw, geo_dh, geo_ox, geo_oy;
  int      matrix709;

  /* animated GIF */
  FILE    *gif;
  unsigned char *gif_idx;      /* one picture as palette indexes */
  int      gif_delay;          /* hundredths of a second a picture stays up */
  int      gif_tick;           /* emulated frames seen (producer side) */
} rec;

static unsigned int rgb_lut[65536];
static int rgb_lut_ready;

static void set_error(const char *msg)
{
  lstrcpynA(rec.error, msg, sizeof(rec.error));
}

static void set_error_hr(const char *what, HRESULT hr)
{
  char buf[256];
  wsprintfA(buf, "%s (error 0x%08lX)", what, (unsigned long)hr);
  set_error(buf);
}

/****************************************************************************
 * Queue
 ****************************************************************************/

static int queue_push(rec_item *it)
{
  if (WaitForSingleObject(rec.sem_free, WAIT_MS) != WAIT_OBJECT_0) return 0;

  EnterCriticalSection(&rec.cs);
  rec.queue[rec.tail] = it;
  rec.tail = (rec.tail + 1) % QUEUE_CAP;
  LeaveCriticalSection(&rec.cs);

  ReleaseSemaphore(rec.sem_items, 1, NULL);
  return 1;
}

static rec_item *queue_pop(void)
{
  rec_item *it;

  WaitForSingleObject(rec.sem_items, INFINITE);

  EnterCriticalSection(&rec.cs);
  it = rec.queue[rec.head];
  rec.head = (rec.head + 1) % QUEUE_CAP;
  LeaveCriticalSection(&rec.cs);

  return it;
}

/****************************************************************************
 * WAV
 ****************************************************************************/

static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, unsigned v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }

static void wav_write_header(FILE *fp, int rate, unsigned data_bytes)
{
  unsigned char h[44];

  memcpy(h, "RIFF", 4);          put32(h + 4, 36 + data_bytes);
  memcpy(h + 8, "WAVEfmt ", 8);  put32(h + 16, 16);
  put16(h + 20, 1);              /* PCM */
  put16(h + 22, 2);              /* stereo */
  put32(h + 24, (unsigned)rate);
  put32(h + 28, (unsigned)rate * 4);
  put16(h + 32, 4);
  put16(h + 34, 16);
  memcpy(h + 36, "data", 4);     put32(h + 40, data_bytes);

  fwrite(h, 1, sizeof(h), fp);
}

static int wav_open(void)
{
  rec.wav = fopen(rec.path, "wb");
  if (!rec.wav)
  {
    set_error("Could not create the recording file.");
    return 0;
  }
  wav_write_header(rec.wav, rec.rate, 0);
  rec.wav_bytes = 0;
  return 1;
}

static void wav_close(void)
{
  if (!rec.wav) return;
  fflush(rec.wav);
  fseek(rec.wav, 0, SEEK_SET);
  wav_write_header(rec.wav, rec.rate, rec.wav_bytes);
  fclose(rec.wav);
  rec.wav = NULL;
}

/****************************************************************************
 * Picture conversion: RGB565 -> NV12 (8-bit 4:2:0), scaled to the fixed
 * output size with nearest-neighbor so pixels stay sharp.
 ****************************************************************************/

static void build_rgb_lut(void)
{
  int i;

  if (rgb_lut_ready) return;
  for (i = 0; i < 65536; i++)
  {
    unsigned r = (i >> 11) & 0x1F, g = (i >> 5) & 0x3F, b = i & 0x1F;
    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    rgb_lut[i] = (r << 16) | (g << 8) | b;
  }
  rgb_lut_ready = 1;
}

static int opt_scale = 1, opt_mbps, opt_tv;      /* (set before each recording from the settings) */

void recorder_set_video_options(int scale, int mbps, int tv_shape)
{
  opt_scale = scale < 1 ? 1 : scale > 4 ? 4 : scale;
  opt_mbps = mbps;
  opt_tv = tv_shape;
}

static void geometry_update(int sw, int sh)
{
  int W = rec.canvas_w, H = rec.canvas_h;
  int x;

  if (sw == rec.geo_sw && sh == rec.geo_sh) return;

  if (sw == rec.start_sw && sh == rec.start_sh)
  {
    rec.geo_dw = W; rec.geo_dh = H; rec.geo_ox = 0; rec.geo_oy = 0;
  }
  else
  {
    /* Picture size changed mid-recording: fit it inside the fixed frame. */
    double sx = (double)W / sw, sy = (double)H / sh;
    double s = (sx < sy) ? sx : sy;
    int dw = ((int)(sw * s)) & ~1, dh = ((int)(sh * s)) & ~1;

    if (dw < 2) dw = 2;
    if (dh < 2) dh = 2;
    rec.geo_dw = dw; rec.geo_dh = dh;
    rec.geo_ox = ((W - dw) / 2) & ~1;
    rec.geo_oy = ((H - dh) / 2) & ~1;
  }

  for (x = 0; x < rec.geo_dw; x++) rec.xmap[x] = x * sw / rec.geo_dw;

  rec.geo_sw = sw;
  rec.geo_sh = sh;
}

static void frame_to_nv12(const unsigned short *src, int sw, int sh)
{
  const int W = rec.canvas_w, H = rec.canvas_h;
  unsigned char *Y = rec.nv12;
  unsigned char *UV = rec.nv12 + (size_t)W * H;
  int ox, oy, dw, dh;
  int y, x;

  /* Color matrix: BT.601 for SD-sized pictures, BT.709 above -- what players
     assume when a file doesn't say. */
  const int cy0 = rec.matrix709 ?  47 :  66, cy1 = rec.matrix709 ? 157 : 129, cy2 = rec.matrix709 ? 16 : 25;
  const int cu0 = rec.matrix709 ? -26 : -38, cu1 = rec.matrix709 ? -87 : -74;
  const int cv1 = rec.matrix709 ? -102 : -94, cv2 = rec.matrix709 ? -10 : -18;

  /* Must run before the geometry is read below. */
  geometry_update(sw, sh);
  ox = rec.geo_ox; oy = rec.geo_oy; dw = rec.geo_dw; dh = rec.geo_dh;

  for (y = 0; y < H; y += 2)
  {
    unsigned char *y0 = Y + (size_t)y * W;
    unsigned char *y1 = y0 + W;
    unsigned char *uv = UV + (size_t)(y / 2) * W;

    if (y < oy || y + 1 >= oy + dh)
    {
      memset(y0, 16, (size_t)W * 2);
      memset(uv, 128, (size_t)W);
      continue;
    }

    {
      const unsigned short *r0 = src + (size_t)(((y - oy) * sh) / dh) * sw;
      const unsigned short *r1 = src + (size_t)(((y + 1 - oy) * sh) / dh) * sw;

      for (x = 0; x < W; x += 2)
      {
        unsigned int p0, p1, p2, p3;
        int R0, G0, B0, R1, G1, B1, R2, G2, B2, R3, G3, B3, R, G, B;

        if (x < ox || x + 1 >= ox + dw)
        {
          y0[x] = y0[x + 1] = y1[x] = y1[x + 1] = 16;
          uv[x] = uv[x + 1] = 128;
          continue;
        }

        p0 = rgb_lut[r0[rec.xmap[x - ox]]];
        p1 = rgb_lut[r0[rec.xmap[x + 1 - ox]]];
        p2 = rgb_lut[r1[rec.xmap[x - ox]]];
        p3 = rgb_lut[r1[rec.xmap[x + 1 - ox]]];

        R0 = (p0 >> 16) & 255; G0 = (p0 >> 8) & 255; B0 = p0 & 255;
        R1 = (p1 >> 16) & 255; G1 = (p1 >> 8) & 255; B1 = p1 & 255;
        R2 = (p2 >> 16) & 255; G2 = (p2 >> 8) & 255; B2 = p2 & 255;
        R3 = (p3 >> 16) & 255; G3 = (p3 >> 8) & 255; B3 = p3 & 255;

        y0[x]     = (unsigned char)(((cy0 * R0 + cy1 * G0 + cy2 * B0 + 128) >> 8) + 16);
        y0[x + 1] = (unsigned char)(((cy0 * R1 + cy1 * G1 + cy2 * B1 + 128) >> 8) + 16);
        y1[x]     = (unsigned char)(((cy0 * R2 + cy1 * G2 + cy2 * B2 + 128) >> 8) + 16);
        y1[x + 1] = (unsigned char)(((cy0 * R3 + cy1 * G3 + cy2 * B3 + 128) >> 8) + 16);

        R = (R0 + R1 + R2 + R3 + 2) >> 2;
        G = (G0 + G1 + G2 + G3 + 2) >> 2;
        B = (B0 + B1 + B2 + B3 + 2) >> 2;

        uv[x]     = (unsigned char)(((cu0 * R + cu1 * G + 112 * B + 128) >> 8) + 128);
        uv[x + 1] = (unsigned char)(((112 * R + cv1 * G + cv2 * B + 128) >> 8) + 128);
      }
    }
  }
}

/****************************************************************************
 * Media Foundation (MP4: H.264 + AAC)
 ****************************************************************************/

static int mf_load(void)
{
  rec.mfplat = LoadLibraryExA("mfplat.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
  rec.mfrw   = LoadLibraryExA("mfreadwrite.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);

  if (rec.mfplat && rec.mfrw)
  {
    rec.pMFStartup                 = (PF_MFStartup)GetProcAddress(rec.mfplat, "MFStartup");
    rec.pMFShutdown                = (PF_MFShutdown)GetProcAddress(rec.mfplat, "MFShutdown");
    rec.pMFCreateAttributes        = (PF_MFCreateAttributes)GetProcAddress(rec.mfplat, "MFCreateAttributes");
    rec.pMFCreateMediaType         = (PF_MFCreateMediaType)GetProcAddress(rec.mfplat, "MFCreateMediaType");
    rec.pMFCreateSample            = (PF_MFCreateSample)GetProcAddress(rec.mfplat, "MFCreateSample");
    rec.pMFCreateMemoryBuffer      = (PF_MFCreateMemoryBuffer)GetProcAddress(rec.mfplat, "MFCreateMemoryBuffer");
    rec.pMFCreateSinkWriterFromURL = (PF_MFCreateSinkWriterFromURL)GetProcAddress(rec.mfrw, "MFCreateSinkWriterFromURL");
  }

  if (!rec.pMFStartup || !rec.pMFShutdown || !rec.pMFCreateAttributes || !rec.pMFCreateMediaType ||
      !rec.pMFCreateSample || !rec.pMFCreateMemoryBuffer || !rec.pMFCreateSinkWriterFromURL)
  {
    set_error("Windows Media Foundation isn't available on this system, so video can't be recorded.\n"
              "(On Windows N editions, install the Media Feature Pack.)");
    return 0;
  }
  return 1;
}

static void mf_close(void)
{
  if (rec.writer)
  {
    IMFSinkWriter_Finalize(rec.writer);   /* writes the MP4 index; without it the file won't play */
    IMFSinkWriter_Release(rec.writer);
    rec.writer = NULL;
  }
  if (rec.mf_started)
  {
    rec.pMFShutdown();
    rec.mf_started = 0;
  }
  if (rec.mfrw)   { FreeLibrary(rec.mfrw);   rec.mfrw = NULL; }
  if (rec.mfplat) { FreeLibrary(rec.mfplat); rec.mfplat = NULL; }
}

#define SETU32(t, key, v)   IMFAttributes_SetUINT32((IMFAttributes *)(t), &(key), (UINT32)(v))
#define SETU64(t, key, hi, lo) \
  IMFAttributes_SetUINT64((IMFAttributes *)(t), &(key), ((UINT64)(UINT32)(hi) << 32) | (UINT32)(lo))
#define SETGUID(t, key, g)  IMFAttributes_SetGUID((IMFAttributes *)(t), &(key), &(g))

static int mf_open(void)
{
  IMFAttributes *attr = NULL;
  IMFMediaType *t = NULL;
  wchar_t wpath[MAX_PATH * 2];
  HRESULT hr;
  int ok = 0;
  const int W = rec.canvas_w, H = rec.canvas_h;

  if (!mf_load()) return 0;

  hr = rec.pMFStartup(REC_MF_VERSION, 0);
  if (FAILED(hr)) { set_error_hr("Could not start Windows Media Foundation", hr); return 0; }
  rec.mf_started = 1;

  hr = rec.pMFCreateAttributes(&attr, 2);
  if (FAILED(hr)) { set_error_hr("Could not create the encoder settings", hr); return 0; }
  SETGUID(attr, MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
  SETU32(attr, MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);

  MultiByteToWideChar(CP_ACP, 0, rec.path, -1, wpath, MAX_PATH * 2);
  hr = rec.pMFCreateSinkWriterFromURL(wpath, NULL, attr, &rec.writer);
  IMFAttributes_Release(attr);
  if (FAILED(hr)) { set_error_hr("Could not create the MP4 file", hr); return 0; }

  /* --- video: NV12 in, H.264 out ------------------------------------- */
  hr = rec.pMFCreateMediaType(&t);
  if (FAILED(hr)) { set_error_hr("Could not set up video encoding", hr); return 0; }
  SETGUID(t, MF_MT_MAJOR_TYPE, MFMediaType_Video);
  SETGUID(t, MF_MT_SUBTYPE, MFVideoFormat_H264);
  SETU32(t, MF_MT_AVG_BITRATE, rec.bitrate);
  SETU32(t, MF_MT_INTERLACE_MODE, 2);                       /* progressive */
  SETU64(t, MF_MT_FRAME_SIZE, W, H);
  SETU64(t, MF_MT_FRAME_RATE, rec.fps_x1000, 1000);
  SETU64(t, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  SETU32(t, MF_MT_MPEG2_PROFILE, 77);                       /* H.264 Main */
  hr = IMFSinkWriter_AddStream(rec.writer, t, &rec.vstream);
  IMFMediaType_Release(t); t = NULL;
  if (FAILED(hr))
  {
    set_error_hr("Windows' H.264 video encoder isn't available.\n"
                 "(On Windows N editions, install the Media Feature Pack.)", hr);
    return 0;
  }

  hr = rec.pMFCreateMediaType(&t);
  if (FAILED(hr)) { set_error_hr("Could not set up video encoding", hr); return 0; }
  SETGUID(t, MF_MT_MAJOR_TYPE, MFMediaType_Video);
  SETGUID(t, MF_MT_SUBTYPE, MFVideoFormat_NV12);
  SETU32(t, MF_MT_INTERLACE_MODE, 2);
  SETU64(t, MF_MT_FRAME_SIZE, W, H);
  SETU64(t, MF_MT_FRAME_RATE, rec.fps_x1000, 1000);
  SETU64(t, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  SETU32(t, MF_MT_YUV_MATRIX, rec.matrix709 ? 1 : 2);       /* MFVideoTransferMatrix: BT.709 = 1, BT.601 = 2 */
  SETU32(t, MF_MT_VIDEO_NOMINAL_RANGE, 2);                  /* 16-235 */
  hr = IMFSinkWriter_SetInputMediaType(rec.writer, rec.vstream, t, NULL);
  IMFMediaType_Release(t); t = NULL;
  if (FAILED(hr)) { set_error_hr("The video encoder rejected the picture format", hr); return 0; }

  /* --- audio: 16-bit stereo PCM in, AAC out --------------------------- */
  hr = rec.pMFCreateMediaType(&t);
  if (FAILED(hr)) { set_error_hr("Could not set up audio encoding", hr); return 0; }
  SETGUID(t, MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  SETGUID(t, MF_MT_SUBTYPE, MFAudioFormat_AAC);
  SETU32(t, MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
  SETU32(t, MF_MT_AUDIO_SAMPLES_PER_SECOND, rec.rate);
  SETU32(t, MF_MT_AUDIO_NUM_CHANNELS, 2);
  SETU32(t, MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 20000);       /* 160 kbit/s */
  SETU32(t, MF_MT_AAC_PAYLOAD_TYPE, 0);                     /* raw AAC */
  SETU32(t, MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
  hr = IMFSinkWriter_AddStream(rec.writer, t, &rec.astream);
  IMFMediaType_Release(t); t = NULL;
  if (FAILED(hr))
  {
    set_error_hr("Windows' AAC audio encoder isn't available.\n"
                 "(On Windows N editions, install the Media Feature Pack.)", hr);
    return 0;
  }

  hr = rec.pMFCreateMediaType(&t);
  if (FAILED(hr)) { set_error_hr("Could not set up audio encoding", hr); return 0; }
  SETGUID(t, MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  SETGUID(t, MF_MT_SUBTYPE, MFAudioFormat_PCM);
  SETU32(t, MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
  SETU32(t, MF_MT_AUDIO_SAMPLES_PER_SECOND, rec.rate);
  SETU32(t, MF_MT_AUDIO_NUM_CHANNELS, 2);
  SETU32(t, MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
  SETU32(t, MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rec.rate * 4);
  SETU32(t, MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
  hr = IMFSinkWriter_SetInputMediaType(rec.writer, rec.astream, t, NULL);
  IMFMediaType_Release(t); t = NULL;
  if (FAILED(hr)) { set_error_hr("The audio encoder rejected the sound format", hr); return 0; }

  hr = IMFSinkWriter_BeginWriting(rec.writer);
  if (FAILED(hr)) { set_error_hr("Could not start writing the MP4 file", hr); return 0; }

  ok = 1;
  return ok;
}

static int mf_write(DWORD stream, const void *data, DWORD len, LONGLONG time, LONGLONG duration)
{
  IMFMediaBuffer *buf = NULL;
  IMFSample *sample = NULL;
  BYTE *p = NULL;
  HRESULT hr;
  int ok = 0;

  hr = rec.pMFCreateMemoryBuffer(len, &buf);
  if (FAILED(hr)) goto done;

  hr = IMFMediaBuffer_Lock(buf, &p, NULL, NULL);
  if (FAILED(hr)) goto done;
  memcpy(p, data, len);
  IMFMediaBuffer_Unlock(buf);
  IMFMediaBuffer_SetCurrentLength(buf, len);

  hr = rec.pMFCreateSample(&sample);
  if (FAILED(hr)) goto done;
  IMFSample_AddBuffer(sample, buf);
  IMFSample_SetSampleTime(sample, time);
  IMFSample_SetSampleDuration(sample, duration);

  hr = IMFSinkWriter_WriteSample(rec.writer, stream, sample);
  ok = SUCCEEDED(hr);

done:
  if (sample) IMFSample_Release(sample);
  if (buf) IMFMediaBuffer_Release(buf);
  return ok;
}


/****************************************************************************
 * Animated GIF
 *
 * Every third emulated frame is kept (20 pictures a second on NTSC), turned
 * into palette indexes with a fixed 6 x 7 x 6 color cube and ordered
 * dithering, and LZW compressed as it arrives, so nothing but the current
 * picture is held in memory.
 ****************************************************************************/

#define GIF_STEP        3
#define GIF_MAX_SECONDS 6

static unsigned gif_bitbuf;
static int      gif_nbits;
static unsigned char gif_block[256];
static int      gif_blen;

static void gif_flush_block(void)
{
  if (gif_blen)
  {
    unsigned char n = (unsigned char)gif_blen;
    fwrite(&n, 1, 1, rec.gif);
    fwrite(gif_block, 1, (size_t)gif_blen, rec.gif);
    gif_blen = 0;
  }
}

static void gif_put_byte(unsigned char b)
{
  gif_block[gif_blen++] = b;
  if (gif_blen == 255) gif_flush_block();
}

static void gif_put_code(unsigned code, int bits)
{
  gif_bitbuf |= code << gif_nbits;
  gif_nbits += bits;
  while (gif_nbits >= 8)
  {
    gif_put_byte((unsigned char)(gif_bitbuf & 0xFF));
    gif_bitbuf >>= 8;
    gif_nbits -= 8;
  }
}

static void gif_le16(unsigned char *p, int v)
{
  p[0] = (unsigned char)(v & 0xFF);
  p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static int gif_open(void)
{
  unsigned char hdr[13];

  rec.gif = fopen(rec.path, "wb");
  if (!rec.gif)
  {
    set_error("Could not create the recording file.");
    return 0;
  }
  rec.gif_idx = (unsigned char *)malloc((size_t)rec.canvas_w * rec.canvas_h);
  if (!rec.gif_idx) { set_error("Out of memory."); return 0; }

  memcpy(hdr, "GIF89a", 6);
  gif_le16(hdr + 6, rec.canvas_w);
  gif_le16(hdr + 8, rec.canvas_h);
  hdr[10] = 0x70;   /* no global color table: every picture brings its own */
  hdr[11] = 0;
  hdr[12] = 0;
  fwrite(hdr, 1, 13, rec.gif);

  /* Loop forever. */
  fwrite("\x21\xFF\x0BNETSCAPE2.0\x03\x01\x00\x00\x00", 1, 19, rec.gif);
  return 1;
}

static int gif_cnt[65536];                 /* how many times each RGB565 color is in the picture being made */
static int gif_map[65536];                 /* ... and the palette index it gets */
static unsigned short gif_used[65536];
static int gif_nused;

static int gif_by_count(const void *a, const void *b)
{
  return gif_cnt[*(const unsigned short *)b] - gif_cnt[*(const unsigned short *)a];
}

/* RGB565 picture (any size) -> palette indexes at the file's size (nearest neighbor). The picture gets a palette of its
   own: a console picture rarely has more than 256 different colors, so they are kept exactly, with no dithering. If it
   has more, the 256 most used are kept and the others take the nearest of them. pal receives the 256 RGB entries. */
static void gif_quantise(const unsigned short *src, int w, int h, unsigned char *pal)
{
  int x, y, i, k;
  int pr[256], pg[256], pb[256];
  int npal;

  gif_nused = 0;
  for (y = 0; y < rec.canvas_h; y++)
  {
    const unsigned short *row = src + (size_t)(y * h / rec.canvas_h) * w;
    for (x = 0; x < rec.canvas_w; x++)
    {
      unsigned short p = row[x * w / rec.canvas_w];
      if (gif_cnt[p]++ == 0) gif_used[gif_nused++] = p;
    }
  }

  if (gif_nused > 256) qsort(gif_used, (size_t)gif_nused, sizeof(gif_used[0]), gif_by_count);
  npal = gif_nused < 256 ? gif_nused : 256;

  memset(pal, 0, 768);
  for (i = 0; i < npal; i++)
  {
    unsigned p = gif_used[i];
    int r = (int)((p >> 11) & 31), g = (int)((p >> 5) & 63), b = (int)(p & 31);
    pr[i] = (r << 3) | (r >> 2);
    pg[i] = (g << 2) | (g >> 4);
    pb[i] = (b << 3) | (b >> 2);
    pal[i * 3 + 0] = (unsigned char)pr[i];
    pal[i * 3 + 1] = (unsigned char)pg[i];
    pal[i * 3 + 2] = (unsigned char)pb[i];
    gif_map[p] = i;
  }
  for (i = npal; i < gif_nused; i++)         /* the colors that did not fit: the nearest one that did */
  {
    unsigned p = gif_used[i];
    int r = (int)((p >> 11) & 31), g = (int)((p >> 5) & 63), b = (int)(p & 31), best = 0, bd = 0x7FFFFFFF;
    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    for (k = 0; k < npal; k++)
    {
      int dr = r - pr[k], dg = g - pg[k], db = b - pb[k], d = dr * dr + dg * dg + db * db;
      if (d < bd) { bd = d; best = k; }
    }
    gif_map[p] = best;
  }

  for (y = 0; y < rec.canvas_h; y++)
  {
    const unsigned short *row = src + (size_t)(y * h / rec.canvas_h) * w;
    unsigned char *out = rec.gif_idx + (size_t)y * rec.canvas_w;
    for (x = 0; x < rec.canvas_w; x++) out[x] = (unsigned char)gif_map[row[x * w / rec.canvas_w]];
  }

  for (i = 0; i < gif_nused; i++) gif_cnt[gif_used[i]] = 0;
}

#define GIF_HASH 8209
static int   gif_hkey[GIF_HASH];
static short gif_hval[GIF_HASH];

static int gif_hfind(int key)
{
  unsigned h = ((unsigned)key * 2654435761u) % GIF_HASH;
  while (gif_hkey[h] != -1 && gif_hkey[h] != key) h = (h + 1) % GIF_HASH;
  return (int)h;
}

static void gif_lzw(const unsigned char *px, size_t n)
{
  int bits = 9, running = 258, maxcode1 = 512;
  size_t i;
  int prefix, h;

  gif_bitbuf = 0; gif_nbits = 0; gif_blen = 0;
  memset(gif_hkey, 0xFF, sizeof(gif_hkey));

  fputc(8, rec.gif);   /* minimum code size */
  gif_put_code(256, bits);
  if (!n) { gif_put_code(257, bits); gif_flush_block(); fputc(0, rec.gif); return; }

  prefix = px[0];
  for (i = 1; i < n; i++)
  {
    int c = px[i], key = (prefix << 8) | c;
    h = gif_hfind(key);
    if (gif_hkey[h] == key) { prefix = gif_hval[h]; continue; }

    gif_put_code((unsigned)prefix, bits);
    if (running >= maxcode1 && prefix <= 4095) { bits++; maxcode1 = 1 << bits; }
    prefix = c;

    if (running >= 4095)
    {
      gif_put_code(256, bits);
      bits = 9; running = 258; maxcode1 = 512;
      memset(gif_hkey, 0xFF, sizeof(gif_hkey));
    }
    else
    {
      gif_hkey[h] = key;
      gif_hval[h] = (short)running++;
    }
  }
  gif_put_code((unsigned)prefix, bits);
  if (running >= maxcode1) bits++;
  gif_put_code(257, bits);
  if (gif_nbits > 0) gif_put_byte((unsigned char)(gif_bitbuf & 0xFF));
  gif_flush_block();
  fputc(0, rec.gif);
}

static void gif_frame(const unsigned short *px, int w, int h)
{
  unsigned char gce[8], idesc[10], pal[768];

  gif_quantise(px, w, h, pal);

  gce[0] = 0x21; gce[1] = 0xF9; gce[2] = 4;
  gce[3] = 0x04;                       /* leave the picture, draw the next over it */
  gif_le16(gce + 4, rec.gif_delay);
  gce[6] = 0; gce[7] = 0;
  fwrite(gce, 1, 8, rec.gif);

  idesc[0] = 0x2C;
  gif_le16(idesc + 1, 0); gif_le16(idesc + 3, 0);
  gif_le16(idesc + 5, rec.canvas_w); gif_le16(idesc + 7, rec.canvas_h);
  idesc[9] = 0x87;                     /* this picture's own color table, 256 entries */
  fwrite(idesc, 1, 10, rec.gif);
  fwrite(pal, 1, sizeof(pal), rec.gif);

  gif_lzw(rec.gif_idx, (size_t)rec.canvas_w * rec.canvas_h);

  if (ferror(rec.gif))
  {
    set_error("Could not write to the recording file (disk full?).");
    InterlockedExchange(&rec.failed, 1);
  }
}

static void gif_close(void)
{
  if (rec.gif)
  {
    fputc(0x3B, rec.gif);
    fclose(rec.gif);
    rec.gif = NULL;
  }
  free(rec.gif_idx);
  rec.gif_idx = NULL;
}

/****************************************************************************
 * Recorder thread
 ****************************************************************************/

static LONGLONG video_time(LONGLONG n)
{
  return n * 10000000LL * 1000LL / (LONGLONG)rec.fps_x1000;   /* 100 ns units */
}

static void handle_item(rec_item *it)
{
  if (InterlockedCompareExchange(&rec.failed, 0, 0)) return;

  if (it->kind == ITEM_AUDIO)
  {
    if (rec.mode == REC_AUDIO)
    {
      size_t n = (size_t)it->frames * 4;
      size_t written = rec.wav ? fwrite(it->data, 1, n, rec.wav) : 0;

      if (written != n)
      {
        set_error("Could not write to the recording file (disk full?).");
        InterlockedExchange(&rec.failed, 1);
      }

      /* Only what actually made it to disk -- this feeds the RIFF/data
         chunk sizes wav_close() stamps into the header, so counting the
         full intended amount regardless of a short write would claim more
         audio data than the file actually contains. */
      rec.wav_bytes += (unsigned)written;
    }
    else
    {
      LONGLONG t = rec.aframes * 10000000LL / rec.rate;
      LONGLONG d = ((rec.aframes + it->frames) * 10000000LL / rec.rate) - t;

      if (!mf_write(rec.astream, it->data, (DWORD)it->frames * 4, t, d))
      {
        set_error("The audio encoder stopped working.");
        InterlockedExchange(&rec.failed, 1);
      }
    }
    rec.aframes += it->frames;
  }
  else if (it->kind == ITEM_VIDEO && rec.mode == REC_GIF)
  {
    gif_frame((const unsigned short *)it->data, it->w, it->h);
  }
  else if (it->kind == ITEM_VIDEO)
  {
    LONGLONG t = video_time(rec.vframes);
    LONGLONG d = video_time(rec.vframes + 1) - t;

    frame_to_nv12((const unsigned short *)it->data, it->w, it->h);

    if (!mf_write(rec.vstream, rec.nv12, (DWORD)rec.nv12_size, t, d))
    {
      set_error("The video encoder stopped working.");
      InterlockedExchange(&rec.failed, 1);
    }
    rec.vframes++;
  }
}

static DWORD WINAPI recorder_thread(LPVOID unused)
{
  HRESULT co;
  int ok;

  (void)unused;
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);      /* encoding must never take the CPU from the game */
  co = CoInitializeEx(NULL, COINIT_MULTITHREADED);

  if (rec.mode == REC_VIDEO)
  {
    build_rgb_lut();
    rec.nv12_size = rec.canvas_w * rec.canvas_h * 3 / 2;
    rec.nv12 = (unsigned char *)malloc((size_t)rec.nv12_size);
    rec.xmap = (int *)malloc(sizeof(int) * (size_t)rec.canvas_w);
    ok = (rec.nv12 && rec.xmap) ? mf_open() : 0;
    if (!rec.nv12 || !rec.xmap) set_error("Out of memory.");
  }
  else if (rec.mode == REC_GIF)
  {
    ok = gif_open();
  }
  else
  {
    ok = wav_open();
  }

  rec.init_ok = ok;
  SetEvent(rec.ev_init);

  if (ok)
  {
    for (;;)
    {
      rec_item *it = queue_pop();
      int stop = (it->kind == ITEM_STOP);

      if (!stop) handle_item(it);
      free(it->data);
      free(it);
      ReleaseSemaphore(rec.sem_free, 1, NULL);

      if (stop) break;
    }
  }

  if (rec.mode == REC_VIDEO) mf_close(); else if (rec.mode == REC_GIF) gif_close(); else wav_close();

  free(rec.nv12); rec.nv12 = NULL;
  free(rec.xmap); rec.xmap = NULL;

  if (SUCCEEDED(co)) CoUninitialize();
  return 0;
}

/****************************************************************************
 * Public interface (emulation thread)
 ****************************************************************************/

static int reap(DWORD wait_ms);

int recorder_start(int mode, const char *path, int sample_rate, double fps)
{
  const unsigned short *px;
  int pitch, sw = 0, sh = 0;

  if (rec.active) return 0;

  /* A previous recording may still be finishing its file on its own thread: it owns `rec`, so it is waited for (a moment)
     rather than wiped. */
  if (!reap(4000))
  {
    set_error("The previous recording is still being finished. Try again in a moment.");
    return 0;
  }

  memset(&rec, 0, sizeof(rec));
  rec.mode  = mode;
  rec.rate  = (sample_rate == 44100) ? 44100 : 48000;
  if (fps < 1.0) fps = 60.0;
  rec.fps_x1000 = (unsigned)(fps * 1000.0 + 0.5);
  lstrcpynA(rec.path, path, sizeof(rec.path));

  if (mode == REC_GIF)
  {
    if (!video_capture_frame(&px, &pitch, &sw, &sh) || sw < 16 || sh < 16)
    {
      set_error("There is no picture to record yet.");
      return 0;
    }
    rec.canvas_w = sw;
    rec.canvas_h = sh;
    rec.gif_delay = (fps > 55.0) ? 5 : 6;   /* 3 frames at 60 or 50 a second, in hundredths */
  }
  else if (mode == REC_VIDEO)
  {
    if (!video_capture_frame(&px, &pitch, &sw, &sh) || sw < 16 || sh < 16)
    {
      set_error("There is no picture to record yet.");
      return 0;
    }

    /* A multiple of the current picture (Settings > Options > Video Recording), sharp when it is the picture's own shape. Fixed for the whole
       file: a later change in the game's resolution is letterboxed into it. */
    rec.start_sw = sw;
    rec.start_sh = sh;
    rec.canvas_h = (sh * opt_scale + 1) & ~1;
    rec.canvas_w = ((opt_tv == 2 ? rec.canvas_h * 16 / 9 : opt_tv ? rec.canvas_h * 4 / 3 : sw * opt_scale) + 1) & ~1;
    rec.matrix709 = (rec.canvas_w >= 1280 || rec.canvas_h >= 720);

    if (opt_mbps > 0) rec.bitrate = (unsigned)opt_mbps * 1000000u;
    else
    {
      double bps = (double)rec.canvas_w * rec.canvas_h * fps * 0.5;
      if (bps < 4000000.0)  bps = 4000000.0;
      if (bps > 20000000.0) bps = 20000000.0;
      rec.bitrate = (unsigned)bps;
    }
  }

  InitializeCriticalSection(&rec.cs);
  rec.cs_ready  = 1;
  rec.sem_items = CreateSemaphoreA(NULL, 0, QUEUE_CAP, NULL);
  rec.sem_free  = CreateSemaphoreA(NULL, QUEUE_CAP, QUEUE_CAP, NULL);
  rec.ev_init   = CreateEventA(NULL, TRUE, FALSE, NULL);
  rec.geo_sw = rec.geo_sh = -1;

  if (rec.sem_items && rec.sem_free && rec.ev_init)
    rec.thread = CreateThread(NULL, 0, recorder_thread, NULL, 0, NULL);
  if (!rec.thread)
  {
    set_error("Could not start the recorder.");
    rec.init_ok = 0;
  }
  else
  {
    WaitForSingleObject(rec.ev_init, 20000);
  }

  if (!rec.init_ok)
  {
    /* The thread has already cleaned up after itself when init failed --
       but only once it has actually exited. The wait's result used to be
       ignored, so a thread still hung (inside mf_open(), say) after the
       20s timeout had rec.sem_items/rec.sem_free/rec.ev_init closed and
       rec.cs deleted out from under it anyway -- the same use-after-free
       class recorder_stop()'s queue-jammed path has above. Closing the
       thread handle itself is always safe regardless (it only drops this
       process's reference, it doesn't touch the thread), so that much
       still always happens. */
    int thread_exited = 1;

    if (rec.thread)
    {
      thread_exited = (WaitForSingleObject(rec.thread, 10000) == WAIT_OBJECT_0);
      /* A thread still hung keeps its handle in rec.thread: the guard at the top of this function then refuses
         a new recording instead of wiping `rec` under it. */
      if (thread_exited) { CloseHandle(rec.thread); rec.thread = NULL; }
    }

    if (thread_exited)
    {
      if (rec.sem_items) CloseHandle(rec.sem_items);
      if (rec.sem_free)  CloseHandle(rec.sem_free);
      if (rec.ev_init)   CloseHandle(rec.ev_init);
      DeleteCriticalSection(&rec.cs);
      rec.cs_ready = 0;
      DeleteFileA(rec.path);   /* don't leave an empty file behind */
    }

    return 0;
  }

  InterlockedExchange(&rec.active, mode);
  return 1;
}

void recorder_capture(const short *samples, int frames)
{
  rec_item *it;

  if (!rec.active || InterlockedCompareExchange(&rec.failed, 0, 0)) return;

  if (rec.mode == REC_GIF)
  {
    const unsigned short *px;
    int pitch, w, h, y;

    InterlockedExchangeAdd(&rec.audio_frames_in, frames);   /* only to measure the length */
    if (rec.audio_frames_in / rec.rate >= GIF_MAX_SECONDS)
    {
      InterlockedExchange(&rec.done, 1);   /* the main loop stops the recording */
      return;
    }
    if (rec.done) return;
    if (rec.gif_tick++ % GIF_STEP) return;
    if (video_capture_frame(&px, &pitch, &w, &h) && w > 0 && h > 0)
    {
      unsigned char *dst;

      it = (rec_item *)malloc(sizeof(*it));
      dst = (unsigned char *)malloc((size_t)w * h * 2);
      if (!it || !dst) { free(it); free(dst); return; }
      it->kind = ITEM_VIDEO; it->w = w; it->h = h; it->frames = 0; it->data = dst;
      for (y = 0; y < h; y++)
        memcpy(dst + (size_t)y * w * 2, (const unsigned char *)px + (size_t)y * pitch, (size_t)w * 2);
      if (!queue_push(it))
      {
        free(dst); free(it);
        set_error("The encoder can't keep up.");
        InterlockedExchange(&rec.failed, 1);
      }
    }
    return;
  }

  if (rec.mode == REC_VIDEO)
  {
    const unsigned short *px;
    int pitch, w, h, y;

    if (video_capture_frame(&px, &pitch, &w, &h) && w > 0 && h > 0)
    {
      unsigned char *dst;

      it = (rec_item *)malloc(sizeof(*it));
      dst = (unsigned char *)malloc((size_t)w * h * 2);
      if (!it || !dst) { free(it); free(dst); goto audio; }

      it->kind = ITEM_VIDEO; it->w = w; it->h = h; it->frames = 0; it->data = dst;
      for (y = 0; y < h; y++)
        memcpy(dst + (size_t)y * w * 2, (const unsigned char *)px + (size_t)y * pitch, (size_t)w * 2);

      if (!queue_push(it))
      {
        free(dst); free(it);
        set_error("The encoder can't keep up.");
        InterlockedExchange(&rec.failed, 1);
        return;
      }
    }
  }

audio:
  if (samples && frames > 0)
  {
    void *copy = malloc((size_t)frames * 4);
    it = (rec_item *)malloc(sizeof(*it));
    if (!copy || !it) { free(copy); free(it); return; }

    memcpy(copy, samples, (size_t)frames * 4);
    it->kind = ITEM_AUDIO; it->w = it->h = 0; it->frames = frames; it->data = copy;

    if (!queue_push(it))
    {
      free(copy); free(it);
      set_error("The encoder can't keep up.");
      InterlockedExchange(&rec.failed, 1);
      return;
    }
    InterlockedExchangeAdd(&rec.audio_frames_in, frames);
  }
}

/* Closes the handles of a recording whose thread has ended. With wait_ms it waits that long for the thread first.
   Returns 1 when nothing of a previous recording is left running. */
static int reap(DWORD wait_ms)
{
  if (!rec.thread) return 1;
  if (WaitForSingleObject(rec.thread, wait_ms) != WAIT_OBJECT_0) return 0;

  CloseHandle(rec.thread);
  CloseHandle(rec.sem_items);
  CloseHandle(rec.sem_free);
  CloseHandle(rec.ev_init);
  if (rec.cs_ready) DeleteCriticalSection(&rec.cs);
  rec.cs_ready = 0;
  rec.thread = NULL;
  rec.sem_items = rec.sem_free = rec.ev_init = NULL;
  return 1;
}

/* Ends the recording. With wait the file is complete when this returns; without it the recorder thread finishes the file
   by itself (the emulation is not held up while the encoder empties its queue) and recorder_start() waits for it if a new
   recording is begun at once. */
static void stop_recording(int wait)
{
  rec_item *it;

  if (!rec.active)
  {
    if (wait) reap(STOP_WAIT_MS);          /* a recording stopped earlier in the background: the file is finished before the caller goes on (closing the game, exiting) */
    return;
  }

  it = (rec_item *)calloc(1, sizeof(*it));
  if (it) it->kind = ITEM_STOP;

  if (!it || !queue_push(it))
  {
    /* Queue jammed: the thread is stuck, and (unlike the timeout case just
       below) was never even told to stop, so there is no reason to expect
       it to exit on its own either. Bails out the same way that case does
       -- reports inactive, but leaves the thread and its handles alone --
       rather than falling through to the unconditional teardown below,
       which would close rec.thread/rec.sem_items/rec.sem_free/rec.ev_init
       and DeleteCriticalSection(&rec.cs) out from under a thread that is
       still actively using every one of them, and would then let the next
       recorder_start() memset(&rec) this same struct while that orphaned
       thread keeps running against it. */
    free(it);
    InterlockedExchange(&rec.active, 0);
    return;
  }

  InterlockedExchange(&rec.active, 0);      /* nothing more is fed to it */
  if (wait) reap(STOP_WAIT_MS);              /* (still finishing after 30 s: it is left to finish on its own) */
}

void recorder_stop(void)       { stop_recording(1); }
void recorder_stop_async(void) { stop_recording(0); }

int recorder_active(void)         { return (int)rec.active; }
int recorder_failed(void)         { return rec.failed ? 1 : 0; }
int recorder_done(void)           { return rec.done ? 1 : 0; }
const char *recorder_error(void)  { return rec.error; }
const char *recorder_path(void)   { return rec.path; }

unsigned recorder_seconds(void)
{
  if (!rec.active || rec.rate <= 0) return 0;
  return (unsigned)((LONGLONG)rec.audio_frames_in / rec.rate);
}
