/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  audio.c -- sound output through waveOut.
 *
 *  One buffer is queued per emulated frame and buffers are recycled by
 *  polling WHDR_DONE rather than through a callback, so nothing here runs on
 *  a second thread and no locking is needed. The number of buffers still in
 *  flight is what the main loop uses to pace emulation, which keeps video in
 *  step with the sound card's clock instead of the system timer.
 ****************************************************************************/

#include <windows.h>
#include <mmsystem.h>
#include <math.h>

#include "shared.h"
#include "gui.h"
#include "audio_be.h"

/* Enough headers to cover the deepest latency setting with room to spare. */
#define WAVE_BUFFERS 16

/* Worst case is a 50 Hz frame at 48 kHz, plus slack for jitter. */
#define WAVE_FRAMES_MAX 2048
#define WAVE_BUFFER_BYTES (WAVE_FRAMES_MAX * 2 * (int)sizeof(short))

static struct
{
  HWAVEOUT  dev;
  WAVEHDR   hdr[WAVE_BUFFERS];
  short    *data[WAVE_BUFFERS];
  int       next;
  int       volume;      /* 0-100 */
  int       rate;        /* sample rate the device is open at, 0 = closed */
  int       opened;
  int       fail_streak; /* consecutive submits the driver refused */
  DWORD     last_submit; /* when a buffer was last handed to the device */
} wav;

/* Sample rate the device is currently open at, or 0 when it is closed. */
int waveout_rate(void)
{
  if (be_active()) return be_rate_now();
  return wav.opened ? wav.rate : 0;
}

/* Stereo width, in percent: 100 leaves the image alone, less pulls it toward the middle,
   more spreads it out. */
static int stereo_pct = 100;

void waveout_set_width(int percent)
{
  stereo_pct = percent < 0 ? 0 : (percent > 300 ? 300 : percent);
}

/* The waveOut device whose name starts with `name` ("" or not found: the Windows default). */
static UINT find_device(const char *name)
{
  UINT i, n = waveOutGetNumDevs();

  if (!name || !name[0]) return WAVE_MAPPER;
  for (i = 0; i < n; i++)
  {
    WAVEOUTCAPSA caps;
    if (waveOutGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR && caps.szPname[0] &&
        !strncmp(name, caps.szPname, strlen(caps.szPname)))        /* (waveOut cuts the name to 31 characters; the saved one may be longer) */
      return i;
  }
  return WAVE_MAPPER;
}

/* Fills `names` (up to `max` entries of 96 chars) with the output devices, by their full names; returns how many. */
int waveout_list_devices(char names[][96], int max)
{
  int i, n = (int)waveOutGetNumDevs(), k = be_list_endpoints(names, max);

  if (k > 0) return k;

  for (i = 0; i < n && k < max; i++)
  {
    WAVEOUTCAPSA caps;
    if (waveOutGetDevCapsA((UINT)i, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
      lstrcpynA(names[k++], caps.szPname, 96);
  }
  return k;
}

int waveout_open(int sample_rate)
{
  WAVEFORMATEX wfx;
  int i;

  if (wav.opened) waveout_close();

  ZeroMemory(&wav, sizeof(wav));
  wav.volume = 100;

  /* XAudio2 or WASAPI when chosen; if one cannot be opened the classic waveOut path below is used instead. */
  if (gui.audio_backend > 0 && be_open(gui.audio_backend, sample_rate, gui.audio_device, (gui.latency + 2) * 20))
  {
    wav.rate = sample_rate;
    wav.opened = 1;
    wav.last_submit = GetTickCount();
    return 1;
  }

  ZeroMemory(&wfx, sizeof(wfx));
  wfx.wFormatTag      = WAVE_FORMAT_PCM;
  wfx.nChannels       = 2;
  wfx.nSamplesPerSec  = (DWORD)sample_rate;
  wfx.wBitsPerSample  = 16;
  wfx.nBlockAlign     = (WORD)(wfx.nChannels * wfx.wBitsPerSample / 8);
  wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
  wfx.cbSize          = 0;

  if (waveOutOpen(&wav.dev, find_device(gui.audio_device), &wfx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
  {
    wav.dev = NULL;
    return 0;
  }

  for (i = 0; i < WAVE_BUFFERS; i++)
  {
    wav.data[i] = (short *)malloc(WAVE_BUFFER_BYTES);
    if (!wav.data[i])
    {
      waveout_close();
      return 0;
    }
    ZeroMemory(&wav.hdr[i], sizeof(WAVEHDR));
    wav.hdr[i].lpData         = (LPSTR)wav.data[i];
    wav.hdr[i].dwBufferLength = WAVE_BUFFER_BYTES;
    wav.hdr[i].dwFlags        = WHDR_DONE;   /* free */
  }

  wav.rate   = sample_rate;
  wav.opened = 1;
  wav.last_submit = GetTickCount();
  return 1;
}

void waveout_close(void)
{
  int i;

  be_close();

  if (wav.dev)
  {
    waveOutReset(wav.dev);

    for (i = 0; i < WAVE_BUFFERS; i++)
    {
      if (wav.hdr[i].dwFlags & WHDR_PREPARED)
      {
        waveOutUnprepareHeader(wav.dev, &wav.hdr[i], sizeof(WAVEHDR));
      }
    }

    waveOutClose(wav.dev);
    wav.dev = NULL;
  }

  for (i = 0; i < WAVE_BUFFERS; i++)
  {
    free(wav.data[i]);
    wav.data[i] = NULL;
  }

  wav.opened = 0;
}

void waveout_set_volume(int percent)
{
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  wav.volume = percent;
}

/* The device is gone or refusing everything (headset unplugged, driver
   reset): the caller should stop pacing on it and try to reopen it. */
int waveout_broken(void)
{
  return !wav.opened || wav.fail_streak >= 5;
}

/* The caller only asks while it is waiting for the device to finish a buffer
   (the queue is full). On a working device that never lasts more than a frame
   or two, because each finished buffer lets the next frame run and be
   submitted. So: nothing submitted for 1.5 s while waiting means the device
   has stopped playing. (Asking "did the queue length change?" does not work:
   sampled only while full, it always reads the same.) Returns nonzero about
   every 1.5 s while that lasts. */
int waveout_stalled(void)
{
  DWORD now = GetTickCount();

  if (!wav.opened || waveout_pending() == 0)
  {
    wav.last_submit = now;
    return 0;
  }
  if (now - wav.last_submit > 1500)
  {
    wav.last_submit = now;
    return 1;
  }
  return 0;
}

int waveout_pending(void)
{
  int i, count = 0;

  if (!wav.opened) return 0;
  if (be_active()) return be_pending();

  for (i = 0; i < WAVE_BUFFERS; i++)
  {
    if (!(wav.hdr[i].dwFlags & WHDR_DONE)) count++;
  }
  return count;
}

void waveout_flush(void)
{
  int i;

  if (!wav.opened) return;
  if (be_active()) { be_flush(); wav.last_submit = GetTickCount(); return; }

  waveOutReset(wav.dev);

  for (i = 0; i < WAVE_BUFFERS; i++)
  {
    if (wav.hdr[i].dwFlags & WHDR_PREPARED)
    {
      waveOutUnprepareHeader(wav.dev, &wav.hdr[i], sizeof(WAVEHDR));
    }
    wav.hdr[i].dwFlags = WHDR_DONE;
  }
  wav.next = 0;
  wav.last_submit = GetTickCount();
}

/* Sound clean-up applied to every frame handed to the device, before the master volume:
   bass boost (a low-shelf lift), loudness evening and the peak meter of Levels and Latency. */
static int enh_bass, enh_norm, enh_drive;
static float lp_l, lp_r;              /* bass filter state */
static float norm_avg, norm_gain = 1.0f;
static int peak_l, peak_r;            /* since the meter was last read */

void waveout_set_enhance(int bass, int normalize, int overdrive)
{
  enh_drive = overdrive ? 1 : 0;
  enh_bass = bass < 0 ? 0 : (bass > 3 ? 3 : bass);
  enh_norm = normalize ? 1 : 0;
  if (!enh_norm) { norm_gain = 1.0f; norm_avg = 0.0f; }
}

/* The loudest left and right sample since the last call (0 - 32767), for the live meter. */
void waveout_meter(int *l, int *r)
{
  *l = peak_l; *r = peak_r;
  peak_l = peak_r = 0;
}

static void enhance(short *d, int frames)
{
  static const float bass_gain[4] = { 0.0f, 0.35f, 0.7f, 1.2f };
  float g = bass_gain[enh_bass], a = 1.0f;
  float comp = 1.0f / (1.0f + g * 0.5f);
  float gain0 = norm_gain, gain1 = norm_gain;
  int i;

  if (wav.rate > 0) a = 1.0f - (float)exp(-6.2831853 * 150.0 / (double)wav.rate);

  if (enh_norm)
  {
    double sum = 0.0;
    for (i = 0; i < frames * 2; i++) sum += (d[i] < 0) ? -d[i] : d[i];
    sum /= (double)(frames * 2 > 0 ? frames * 2 : 1);
    norm_avg += ((float)sum - norm_avg) * 0.012f;               /* about 1.5 s to follow a change */
    if (norm_avg > 200.0f)                                       /* silence is not lifted */
    {
      float want = 5000.0f / norm_avg;
      if (want > 2.2f) want = 2.2f;
      if (want < 0.6f) want = 0.6f;
      norm_gain += (want - norm_gain) * 0.02f;
    }
    gain1 = norm_gain;
  }

  for (i = 0; i < frames; i++)
  {
    float l = d[2 * i], r = d[2 * i + 1];
    float k = gain0 + (gain1 - gain0) * (float)i / (float)(frames > 0 ? frames : 1);
    int ol, orr;

    if (g > 0.0f)
    {
      lp_l += a * (l - lp_l);
      lp_r += a * (r - lp_r);
      l = (l + g * lp_l) * comp;
      r = (r + g * lp_r) * comp;
    }
    l *= k; r *= k;
    if (enh_norm)                                  /* a soft knee, so lifted peaks do not click */
    {
      if (l > 28000.0f) l = 28000.0f + (l - 28000.0f) * 0.2f; else if (l < -28000.0f) l = -28000.0f + (l + 28000.0f) * 0.2f;
      if (r > 28000.0f) r = 28000.0f + (r - 28000.0f) * 0.2f; else if (r < -28000.0f) r = -28000.0f + (r + 28000.0f) * 0.2f;
    }
    if (enh_drive)                                  /* Overdrive: pushed into a soft limiter (tanh), then back to level */
    {
      l = 32767.0f * tanhf(l / 32767.0f * 2.2f) / 0.976f;
      r = 32767.0f * tanhf(r / 32767.0f * 2.2f) / 0.976f;
    }
    ol = (int)l; orr = (int)r;
    if (ol > 32767) ol = 32767;
    if (ol < -32768) ol = -32768;
    if (orr > 32767) orr = 32767;
    if (orr < -32768) orr = -32768;
    d[2 * i] = (short)ol; d[2 * i + 1] = (short)orr;
    if (ol < 0) ol = -ol;
    if (orr < 0) orr = -orr;
    if (ol > peak_l) peak_l = ol;
    if (orr > peak_r) peak_r = orr;
  }
}

/* Applies stereo width and the master volume to `frames` frames of samples. */
static void scale_into(const short *samples, short *dst, int frames)
{
  int scale = (wav.volume * 256) / 100;
  int i;

  if (stereo_pct != 100)
  {
    for (i = 0; i < frames; i++)
    {
      int l = samples[2 * i], r = samples[2 * i + 1];
      int m = (l + r) / 2, s = (l - r) / 2 * stereo_pct / 100;

      l = ((m + s) * scale) >> 8;
      r = ((m - s) * scale) >> 8;
      dst[2 * i]     = (short)(l > 32767 ? 32767 : (l < -32768 ? -32768 : l));
      dst[2 * i + 1] = (short)(r > 32767 ? 32767 : (r < -32768 ? -32768 : r));
    }
  }
  else if (wav.volume >= 100)
  {
    memcpy(dst, samples, (size_t)frames * 2 * sizeof(short));
  }
  else
  {
    /* Scale in software: waveOutSetVolume is not honored by every driver. */
    for (i = 0; i < frames * 2; i++) dst[i] = (short)((samples[i] * scale) >> 8);
  }
}

void waveout_submit(const short *samples, int frames)
{
  WAVEHDR *h;
  int slot, tries;
  int bytes;

  if (!wav.opened || frames <= 0) return;

  if (frames > WAVE_FRAMES_MAX) frames = WAVE_FRAMES_MAX;
  bytes = frames * 2 * (int)sizeof(short);

  if (be_active())
  {
    static short work_be[WAVE_FRAMES_MAX * 2], out_be[WAVE_FRAMES_MAX * 2];
    memcpy(work_be, samples, (size_t)bytes);
    enhance(work_be, frames);
    scale_into(work_be, out_be, frames);
    if (be_write(out_be, frames)) { wav.fail_streak = 0; wav.last_submit = GetTickCount(); }
    else wav.fail_streak++;
    return;
  }

  /* Find a buffer the device has finished with. */
  for (tries = 0; tries < WAVE_BUFFERS; tries++)
  {
    slot = (wav.next + tries) % WAVE_BUFFERS;
    if (wav.hdr[slot].dwFlags & WHDR_DONE) break;
  }

  /* Everything is still queued -- we are ahead of the device, drop the frame. */
  if (tries == WAVE_BUFFERS) return;

  h = &wav.hdr[slot];

  {
    static short work[WAVE_FRAMES_MAX * 2];
    memcpy(work, samples, (size_t)bytes);
    enhance(work, frames);
    samples = work;
  }

  if (h->dwFlags & WHDR_PREPARED)
  {
    waveOutUnprepareHeader(wav.dev, h, sizeof(WAVEHDR));
  }

  scale_into(samples, wav.data[slot], frames);

  h->lpData         = (LPSTR)wav.data[slot];
  h->dwBufferLength = (DWORD)bytes;
  h->dwFlags        = 0;
  h->dwLoops        = 0;

  if (waveOutPrepareHeader(wav.dev, h, sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
  {
    h->dwFlags = WHDR_DONE;
    wav.fail_streak++;
    return;
  }

  if (waveOutWrite(wav.dev, h, sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
  {
    waveOutUnprepareHeader(wav.dev, h, sizeof(WAVEHDR));
    h->dwFlags = WHDR_DONE;
    wav.fail_streak++;
    return;
  }

  wav.fail_streak = 0;
  wav.last_submit = GetTickCount();
  wav.next = (slot + 1) % WAVE_BUFFERS;
}
