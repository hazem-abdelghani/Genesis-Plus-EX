/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *  Copyright (c) 2026 Hazem Abdelghani
 *
 *  disc.c -- the optical drives and the discs in them.
 *
 *  First step of playing a Sega CD disc straight from the drive (the core
 *  reads a disc through fopen / fread / fseek, so a later step can hand it a
 *  virtual file made from these reads): find the drives, read the table of
 *  contents, recognize a Sega CD disc by the text at the start of its data
 *  track, and try the reads the player will need (the data track, a raw
 *  sector, an audio sector). A virtual drive (WinCDEmu and the like) is an
 *  optical drive to Windows too.
 ****************************************************************************/

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#include <ntddcdrm.h>
#include <ntddscsi.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "disc.h"

extern void gui_log(const char *fmt, ...);

#define DISC_MAX_TRACKS 100

typedef struct
{
  int number;          /* the track's number (1 ...) */
  int data;            /* 1: a data track, 0: audio */
  long lba;            /* its first sector */
} disc_track;

static unsigned char sector_buf[2352 + 64] __attribute__((aligned(4096)));

/* One line to the text and to the log. */
static void put(char *text, int size, const char *fmt, ...)
{
  char line[400];
  va_list ap;
  int n = (int)strlen(text);

  va_start(ap, fmt);
  _vsnprintf(line, sizeof(line) - 1, fmt, ap);
  va_end(ap);
  line[sizeof(line) - 1] = '\0';
  gui_log("Disc: %s", line);
  if (n < size - 2) _snprintf(text + n, (size_t)(size - n - 2), "%s\r\n", line);
  text[size - 1] = '\0';
}

static long msf_to_lba(const UCHAR *a)
{
  return ((long)a[1] * 60 + a[2]) * 75 + a[3] - 150;
}

/* The tracks of the disc in a drive; returns how many (0 when the table of contents cannot be read), the lead-out in *end. */
static int read_toc(HANDLE h, disc_track *tr, long *end, DWORD *err)
{
  static CDROM_TOC toc;
  DWORD got = 0;
  int count, i, n = 0;

  memset(&toc, 0, sizeof(toc));
  if (!DeviceIoControl(h, IOCTL_CDROM_READ_TOC, NULL, 0, &toc, sizeof(toc), &got, NULL)) { *err = GetLastError(); return 0; }
  count = (((int)toc.Length[0] << 8) | toc.Length[1]) - 2;
  count = count / (int)sizeof(TRACK_DATA);
  *end = -1;
  for (i = 0; i < count && i < MAXIMUM_NUMBER_TRACKS; i++)
  {
    if (toc.TrackData[i].TrackNumber == 0xAA) { *end = msf_to_lba(toc.TrackData[i].Address); continue; }
    if (n < DISC_MAX_TRACKS)
    {
      tr[n].number = toc.TrackData[i].TrackNumber;
      tr[n].data = (toc.TrackData[i].Control & 4) ? 1 : 0;
      tr[n].lba = msf_to_lba(toc.TrackData[i].Address);
      n++;
    }
  }
  return n;
}

/* The text of a field of the disc header (spaces and zeros at the end cut off). */
static void header_text(const unsigned char *src, int len, char *out, int outsize)
{
  int i, n = 0;

  for (i = 0; i < len && n < outsize - 1; i++) out[n++] = (src[i] >= 32 && src[i] < 127) ? (char)src[i] : ' ';
  while (n > 0 && out[n - 1] == ' ') n--;
  out[n] = '\0';
}

static int read_cooked(HANDLE h, long lba, unsigned char *buf, DWORD *ms, DWORD *err)
{
  LARGE_INTEGER pos;
  DWORD got = 0, t0 = GetTickCount();
  BOOL ok;

  pos.QuadPart = (LONGLONG)lba * 2048;
  if (!SetFilePointerEx(h, pos, NULL, FILE_BEGIN)) { *err = GetLastError(); return 0; }
  ok = ReadFile(h, buf, 2048, &got, NULL);
  *ms = GetTickCount() - t0;
  if (!ok || got != 2048) { *err = ok ? ERROR_READ_FAULT : GetLastError(); return 0; }
  return 1;
}

static int read_raw(HANDLE h, long lba, int n, TRACK_MODE_TYPE mode, unsigned char *buf, DWORD *err)
{
  RAW_READ_INFO ri;
  DWORD got = 0;

  memset(&ri, 0, sizeof(ri));
  ri.DiskOffset.QuadPart = (LONGLONG)lba * 2048;
  ri.SectorCount = (ULONG)n;
  ri.TrackMode = mode;
  if (!DeviceIoControl(h, IOCTL_CDROM_RAW_READ, &ri, sizeof(ri), buf, (DWORD)(2352 * n), &got, NULL)) { *err = GetLastError(); return 0; }
  return got >= (DWORD)(2352 * n) ? 1 : 0;
}

/* The name of the drive (what a real drive or a virtual one calls itself). */
static void device_name(HANDLE h, char *out, int outsize)
{
  STORAGE_PROPERTY_QUERY q;
  static unsigned char desc[1024];
  DWORD got = 0;
  STORAGE_DEVICE_DESCRIPTOR *d = (STORAGE_DEVICE_DESCRIPTOR *)desc;
  char vendor[64] = "", product[64] = "", rev[32] = "";

  out[0] = '\0';
  memset(&q, 0, sizeof(q));
  q.PropertyId = StorageDeviceProperty;
  q.QueryType = PropertyStandardQuery;
  if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), desc, sizeof(desc), &got, NULL)) return;
  if (d->VendorIdOffset && d->VendorIdOffset < got) header_text(desc + d->VendorIdOffset, (int)strnlen((char *)desc + d->VendorIdOffset, 40), vendor, sizeof(vendor));
  if (d->ProductIdOffset && d->ProductIdOffset < got) header_text(desc + d->ProductIdOffset, (int)strnlen((char *)desc + d->ProductIdOffset, 40), product, sizeof(product));
  if (d->ProductRevisionOffset && d->ProductRevisionOffset < got) header_text(desc + d->ProductRevisionOffset, (int)strnlen((char *)desc + d->ProductRevisionOffset, 20), rev, sizeof(rev));
  _snprintf(out, (size_t)outsize - 1, "%s %s %s", vendor, product, rev);
  out[outsize - 1] = '\0';
}

/* The SCSI command READ CD (what ripping programs use) for one sector: audio (user data only) or data (the whole 2352 bytes). It needs the drive
   opened for writing as well. Returns 1 when the sector came; *sense has the SCSI sense key, additional code and qualifier when not. */
typedef struct
{
  SCSI_PASS_THROUGH_DIRECT sptd;
  ULONG filler;
  UCHAR sense[32];
} sptd_with_sense;

static int read_cd_scsi(HANDLE h, long lba, int n, int audio, unsigned char *buf, DWORD *err, unsigned *sense)
{
  static sptd_with_sense c;
  DWORD got = 0;

  memset(&c, 0, sizeof(c));
  c.sptd.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
  c.sptd.CdbLength = 12;
  c.sptd.SenseInfoLength = sizeof(c.sense);
  c.sptd.DataIn = SCSI_IOCTL_DATA_IN;
  c.sptd.DataTransferLength = (ULONG)(2352 * n);
  c.sptd.TimeOutValue = 10;
  c.sptd.DataBuffer = buf;
  c.sptd.SenseInfoOffset = (ULONG)offsetof(sptd_with_sense, sense);
  c.sptd.Cdb[0] = 0xBE;                                  /* READ CD */
  c.sptd.Cdb[1] = audio ? 0x04 : 0x00;                   /* the sector type: CD-DA, or any */
  c.sptd.Cdb[2] = (UCHAR)(lba >> 24); c.sptd.Cdb[3] = (UCHAR)(lba >> 16); c.sptd.Cdb[4] = (UCHAR)(lba >> 8); c.sptd.Cdb[5] = (UCHAR)lba;
  c.sptd.Cdb[8] = (UCHAR)n;                              /* how many sectors */
  c.sptd.Cdb[9] = audio ? 0x10 : 0xF8;                   /* user data only; or sync, headers, user data and the error codes */
  *sense = 0;
  if (!DeviceIoControl(h, IOCTL_SCSI_PASS_THROUGH_DIRECT, &c, sizeof(c), &c, sizeof(c), &got, NULL)) { *err = GetLastError(); return 0; }
  if (c.sptd.ScsiStatus != 0)
  {
    *sense = ((unsigned)(c.sense[2] & 0x0F) << 16) | ((unsigned)c.sense[12] << 8) | c.sense[13];
    *err = ERROR_IO_DEVICE;
    return 0;
  }
  return 1;
}

static void look_at_drive(char letter, char *text, int size)
{
  char path[16], root[8];
  HANDLE h;
  disc_track tr[DISC_MAX_TRACKS];
  long end = -1;
  int n, i, data_n = 0, audio_n = 0, first_data = -1, first_audio = -1;
  DWORD err = 0, ms = 0;

  _snprintf(path, sizeof(path), "\\\\.\\%c:", letter);
  _snprintf(root, sizeof(root), "%c:\\", letter);
  h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE)
  {
    put(text, size, "Drive %c: cannot be opened (error %lu)", letter, (unsigned long)GetLastError());
    return;
  }
  if (!DeviceIoControl(h, IOCTL_STORAGE_CHECK_VERIFY, NULL, 0, NULL, 0, &ms, NULL))
  {
    put(text, size, "Drive %c: no disc in the drive (error %lu)", letter, (unsigned long)GetLastError());
    CloseHandle(h);
    return;
  }
  {
    char name[160];

    device_name(h, name, sizeof(name));
    if (name[0]) put(text, size, "Drive %c: is %s", letter, name);
  }
  n = read_toc(h, tr, &end, &err);
  if (n == 0)
  {
    put(text, size, "Drive %c: a disc is in the drive, but its table of contents cannot be read (error %lu)", letter, (unsigned long)err);
    CloseHandle(h);
    return;
  }
  for (i = 0; i < n; i++)
  {
    if (tr[i].data) { data_n++; if (first_data < 0) first_data = i; }
    else { audio_n++; if (first_audio < 0) first_audio = i; }
  }

  {
    /* Sega CD: the first data track starts with "SEGADISCSYSTEM", and its disc header has the game's name. */
    char title[64] = "";
    int sega = 0, cooked = 0;
    DWORD cooked_ms = 0;

    if (first_data >= 0 && read_cooked(h, tr[first_data].lba, sector_buf, &cooked_ms, &err))
    {
      cooked = 1;
      if (!memcmp(sector_buf, "SEGADISCSYSTEM", 14))
      {
        sega = 1;
        header_text(sector_buf + 0x150, 48, title, sizeof(title));
        if (!title[0]) header_text(sector_buf + 0x120, 48, title, sizeof(title));
      }
    }
    put(text, size, "Drive %c: a disc with %d data and %d audio tracks%s%s%s", letter, data_n, audio_n,
        sega ? ", a Sega CD disc" : "", (sega && title[0]) ? ": " : "", sega ? title : "");
    for (i = 0; i < n; i++) put(text, size, "  track %d: %s, starts at sector %ld", tr[i].number, tr[i].data ? "data" : "audio", tr[i].lba);
    if (end >= 0) put(text, size, "  the disc ends at sector %ld", end);

    if (first_data < 0) put(text, size, "  no data track: not a game disc");
    else if (cooked) put(text, size, "  data track, sector read: ok (%lu ms)", (unsigned long)cooked_ms);
    else put(text, size, "  data track, sector read: failed (error %lu)", (unsigned long)err);

    if (first_data >= 0)
    {
      if (read_raw(h, tr[first_data].lba, 1, YellowMode2, sector_buf, &err)) put(text, size, "  data track, raw sector read (2352 bytes): ok");
      else put(text, size, "  data track, raw sector read (2352 bytes): failed (error %lu)", (unsigned long)err);
    }
    if (first_audio >= 0)
    {
      long at = tr[first_audio].lba;

      if (read_raw(h, at, 1, CDDA, sector_buf, &err)) put(text, size, "  audio track %d, raw sector read: ok", tr[first_audio].number);
      else put(text, size, "  audio track %d, raw sector read: failed (error %lu)", tr[first_audio].number, (unsigned long)err);
    }

    {
      /* The other way, the SCSI command READ CD (it needs the drive opened for writing too). */
      HANDLE hw = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
      unsigned sense = 0;

      if (hw == INVALID_HANDLE_VALUE) put(text, size, "  SCSI read: the drive cannot be opened for it (error %lu)", (unsigned long)GetLastError());
      else
      {
        if (first_data >= 0)
        {
          if (read_cd_scsi(hw, tr[first_data].lba, 1, 0, sector_buf, &err, &sense)) put(text, size, "  data track, SCSI READ CD (2352 bytes): ok");
          else put(text, size, "  data track, SCSI READ CD (2352 bytes): failed (error %lu, sense %06X)", (unsigned long)err, sense);
        }
        if (first_audio >= 0)
        {
          if (read_cd_scsi(hw, tr[first_audio].lba, 1, 1, sector_buf, &err, &sense)) put(text, size, "  audio track %d, SCSI READ CD: ok", tr[first_audio].number);
          else put(text, size, "  audio track %d, SCSI READ CD: failed (error %lu, sense %06X)", tr[first_audio].number, (unsigned long)err, sense);
        }
        CloseHandle(hw);
      }
    }
    if (audio_n == 0) put(text, size, "  (no audio track on this disc, so no audio read was tried)");
  }
  CloseHandle(h);
}

int disc_report(char *text, int size)
{
  DWORD drives = GetLogicalDrives();
  int letter, count = 0;

  if (size < 2) return 0;
  text[0] = '\0';
  for (letter = 0; letter < 26; letter++)
  {
    char root[4];

    if (!(drives & (1UL << letter))) continue;
    root[0] = (char)('A' + letter); root[1] = ':'; root[2] = '\\'; root[3] = '\0';
    if (GetDriveTypeA(root) != DRIVE_CDROM) continue;
    count++;
    look_at_drive((char)('A' + letter), text, size);
  }
  if (!count) put(text, size, "No optical drive found (a virtual drive counts too)");
  return count;
}


/****************************************************************************
 *  Playing a disc from the drive.
 *
 *  The core reads a disc through fopen / fread / fseek / ftell / fgets /
 *  fclose (cdStream, core/macros.h), and the program's fopen is wrapped (main.c).
 *  Here a disc is made to look like a .cue sheet with one file for each track:
 *
 *    disc:\<name of the game>.cue      the sheet, made from the table of contents
 *    disc:\track01.bin                 the data track (normal reads, 2048 bytes a sector)
 *    disc:\track02.bin ...             the audio tracks (raw reads, 2352 bytes a sector)
 *
 *  main.c's wrappers of fread, fseek, ftell, fgets and fclose hand such a
 *  stream to the functions below (disc_owns says which are). Nothing is written
 *  to disk.
 ****************************************************************************/

#define DISC_PREFIX "disc:\\"
#define DATA_CACHE_SECTORS 64
#define AUDIO_CACHE_SECTORS 16
#define MAX_STREAMS 64

typedef struct
{
  char letter;                       /* the drive the disc is in ('\0': none open) */
  char title[64];                    /* the game's name from the disc header */
  char path[128];                    /* the path of the sheet, disc:\<title>.cue */
  HANDLE hread, hrw;                 /* the drive opened for reading, and for reading and writing (the SCSI command) */
  disc_track tr[DISC_MAX_TRACKS];
  int ntracks;
  long end;                          /* the sector the disc ends at */
  long data_sectors;                 /* the length of the data track (up to the pause before the first audio track) */
  int audio_method;                  /* how the audio tracks are read: 0 not at all, 1 raw read, 2 the SCSI command READ CD */
  char sheet[16384];                 /* the text of the .cue sheet (about 75 bytes for each of 99 tracks at most) */
} disc_state;

static disc_state cur;

typedef struct
{
  int used;
  int kind;                          /* 0 the sheet, 1 the data track, 2 an audio track */
  int track;                         /* the index in cur.tr (kinds 1 and 2) */
  long lba0, sectors;                /* where it starts on the disc, and how many sectors it has */
  long long pos;                     /* the read position (bytes) */
} disc_stream;

static disc_stream streams[MAX_STREAMS];

/* The sectors are read in chunks. Each kind (data, audio) keeps the chunk being served and the next one, which a background thread reads while the game
   uses the first: a slow drive no longer stalls the game, as long as the game reads on in order (the music always does). */
typedef struct
{
  unsigned char *buf;
  long first;                        /* the first (absolute) sector held (-1: nothing) and how many */
  int n;
} slot_t;

typedef struct
{
  int unit;                          /* the bytes of a sector (2048 or 2352) */
  int max_n;                         /* the sectors of a chunk */
  slot_t cur, ahead;                 /* cur is only touched by the player's thread; ahead by the reader while busy is set */
  int req_pending;                   /* a chunk to read ahead was asked for: from req_first, not past req_limit */
  long req_first, req_limit;
  int busy;                          /* the reader is filling ahead (for the range busy_first .. busy_first + busy_n) */
  long busy_first;
  int busy_n;
  HANDLE done;                       /* signalled when the reader is not busy with this kind */
} cache_t;

static unsigned char dbuf[2][DATA_CACHE_SECTORS * 2048] __attribute__((aligned(4096)));
#define ACACHES 4                      /* the core reads several audio tracks in turn (the one that plays, and others it looks at): each keeps its own chunks */
static unsigned char abuf[ACACHES * 2][AUDIO_CACHE_SECTORS * 2352 + 64] __attribute__((aligned(4096)));
static cache_t dcache, acaches[ACACHES];
static int acache_owner[ACACHES];      /* the track (index in cur.tr) each audio cache holds, -1: none */
static unsigned acache_used[ACACHES];  /* when it was used last (to give the least recently used one to another track) */
static unsigned use_clock;
static CRITICAL_SECTION cache_cs;      /* the chunks' bookkeeping */
static CRITICAL_SECTION io_cs;         /* one read from the drive at a time (the file position of the handle is shared) */
static int cs_ready;
static HANDLE reader_thread, reader_work;
static volatile LONG reader_quit;
static int reader_stale;               /* reader_stop() gave up waiting for the thread (see there) */
static int read_errors, slow_reads;
static int miss_log;                    /* (for the log) the first chunks read at once are described */
static int now_data, now_audio;
static int chunks_ahead, chunks_now;      /* (for the log) chunks that were read ahead in time, and chunks the game had to wait for */

/* Is a drive's disc a Sega CD disc? title gets its name. The drive stays closed afterwards. */
static int probe_sega(char letter, char *title, int tsize)
{
  char path[16];
  HANDLE h;
  disc_track tr[DISC_MAX_TRACKS];
  long end = -1;
  DWORD err = 0, ms = 0, dummy = 0;
  int n, i, first_data = -1, ok = 0;

  _snprintf(path, sizeof(path), "\\\\.\\%c:", letter);
  h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL);        /* (not through Windows' cache: a disc put in its place is seen) */
  if (h == INVALID_HANDLE_VALUE) h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return 0;
  if (DeviceIoControl(h, IOCTL_STORAGE_CHECK_VERIFY, NULL, 0, NULL, 0, &dummy, NULL) && (n = read_toc(h, tr, &end, &err)) > 0)
  {
    for (i = 0; i < n && first_data < 0; i++) if (tr[i].data) first_data = i;
    if (first_data >= 0 && read_cooked(h, tr[first_data].lba, sector_buf, &ms, &err) && !memcmp(sector_buf, "SEGADISCSYSTEM", 14))
    {
      header_text(sector_buf + 0x150, 48, title, tsize);
      if (!title[0]) header_text(sector_buf + 0x120, 48, title, tsize);
      ok = 1;
    }
  }
  CloseHandle(h);
  return ok;
}

/* A name that can be a file name (the saves and the states of the game take it). */
static void clean_name(char *s)
{
  char *p;

  for (p = s; *p; p++) if (*p == '\\' || *p == '/' || *p == ':' || *p == '*' || *p == '?' || *p == '"' || *p == '<' || *p == '>' || *p == '|') *p = '-';
  while (p > s && (p[-1] == ' ' || p[-1] == '.')) *--p = '\0';
  if (!s[0]) lstrcpynA(s, "Sega CD disc", 64);
}

int disc_scan_sega(disc_found *out, int max)
{
  DWORD drives = GetLogicalDrives();
  int letter, n = 0;

  for (letter = 0; letter < 26 && n < max; letter++)
  {
    char root[4], title[64];

    if (!(drives & (1UL << letter))) continue;
    root[0] = (char)('A' + letter); root[1] = ':'; root[2] = '\\'; root[3] = '\0';
    if (GetDriveTypeA(root) != DRIVE_CDROM) continue;
    if (probe_sega((char)('A' + letter), title, sizeof(title)))
    {
      clean_name(title);
      out[n].letter = (char)('A' + letter);
      lstrcpynA(out[n].title, title, sizeof(out[n].title));
      n++;
    }
  }
  return n;
}

static void sheet_add(const char *fmt, ...)
{
  size_t n = strlen(cur.sheet);
  va_list ap;

  va_start(ap, fmt);
  _vsnprintf(cur.sheet + n, sizeof(cur.sheet) - n - 1, fmt, ap);
  va_end(ap);
  cur.sheet[sizeof(cur.sheet) - 1] = '\0';
}

static void msf(long frames, int *m, int *s, int *f)
{
  *m = (int)(frames / (60 * 75)); *s = (int)((frames / 75) % 60); *f = (int)(frames % 75);
}

static void reader_start(void);
static void reader_stop(void);

static char last_letter;               /* the drive of the disc that was opened last (kept when a swap finds no disc) */

int disc_open(char letter)
{
  char path[16], title[64];
  DWORD err = 0, ms = 0;
  int i, first_data = -1, first_audio = -1;
  long end = -1;

  disc_close();
  if (!probe_sega(letter, title, sizeof(title))) return 0;
  clean_name(title);
  memset(&cur, 0, sizeof(cur));
  cur.letter = letter;
  lstrcpynA(cur.title, title, sizeof(cur.title));
  _snprintf(cur.path, sizeof(cur.path) - 1, DISC_PREFIX "%s.cue", cur.title);
  _snprintf(path, sizeof(path), "\\\\.\\%c:", letter);
  /* Unbuffered: the data comes from the drive itself, not from Windows' cache of the volume (a first log from a virtual drive showed a wrong first sector
     now and then, with a start that took seconds: the reads are all whole, aligned sectors, as unbuffered reads need). */
  cur.hread = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL);
  if (cur.hread == INVALID_HANDLE_VALUE) cur.hread = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (cur.hread == INVALID_HANDLE_VALUE) { cur.hread = NULL; cur.letter = '\0'; return 0; }
  cur.ntracks = read_toc(cur.hread, cur.tr, &end, &err);
  if (cur.ntracks == 0) { disc_close(); return 0; }
  cur.end = end >= 0 ? end : cur.tr[cur.ntracks - 1].lba + 1;
  for (i = 0; i < cur.ntracks; i++)
  {
    if (cur.tr[i].data) { if (first_data < 0) first_data = i; }
    else if (first_audio < 0) first_audio = i;
  }
  if (first_data != 0) { gui_log("Disc: the first track of the disc in drive %c: is not a data track", letter); disc_close(); return 0; }
  /* the data track ends where the pause before the first audio track begins (150 sectors, 2 seconds, on a normal disc) */
  cur.data_sectors = (first_audio >= 0 ? cur.tr[first_audio].lba : cur.end) - cur.tr[0].lba;
  if (first_audio >= 0 && cur.data_sectors > 150) cur.data_sectors -= 150;

  /* the audio tracks: a raw read, else the SCSI command READ CD */
  if (first_audio >= 0)
  {
    sector_buf[0] = 0;
    if (read_raw(cur.hread, cur.tr[first_audio].lba, 1, CDDA, sector_buf, &err)) cur.audio_method = 1;
    else
    {
      cur.hrw = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
      if (cur.hrw == INVALID_HANDLE_VALUE) cur.hrw = NULL;
      else
      {
        unsigned sense;
        if (read_cd_scsi(cur.hrw, cur.tr[first_audio].lba, 1, 1, sector_buf, &err, &sense)) cur.audio_method = 2;
        else { CloseHandle(cur.hrw); cur.hrw = NULL; }
      }
    }
  }
  gui_log("Disc: drive %c: \"%s\", %d tracks, ends at sector %ld, data track %ld sectors, audio read: %s", letter, cur.title, cur.ntracks, cur.end, cur.data_sectors,
          first_audio < 0 ? "no audio track" : cur.audio_method == 1 ? "raw read" : cur.audio_method == 2 ? "SCSI READ CD" : "not possible (the music will not play)");

  /* the sheet */
  for (i = 0; i < cur.ntracks; i++)
  {
    sheet_add("FILE \"track%02d.bin\" BINARY\r\n", cur.tr[i].number);
    if (cur.tr[i].data) sheet_add("  TRACK %02d MODE1/2048\r\n", cur.tr[i].number);
    else
    {
      long gap = (i == first_audio) ? cur.tr[i].lba - (cur.tr[0].lba + cur.data_sectors) : 0;

      sheet_add("  TRACK %02d AUDIO\r\n", cur.tr[i].number);
      if (gap > 0) { int m, s, f; msf(gap, &m, &s, &f); sheet_add("    PREGAP %02d:%02d:%02d\r\n", m, s, f); }
    }
    sheet_add("    INDEX 01 00:00:00\r\n");
  }
  if (strlen(cur.sheet) >= sizeof(cur.sheet) - 8) { gui_log("Disc: the sheet of %d tracks does not fit", cur.ntracks); disc_close(); return 0; }
  (void)ms;
  read_errors = slow_reads = 0;
  last_letter = letter;
  reader_start();
  return 1;
}

void disc_close(void)
{
  int i;

  reader_stop();
  if (cur.letter && (chunks_ahead || chunks_now)) gui_log("Disc: %d chunks were read ahead in time, %d were read when the game asked for them, %d read errors", chunks_ahead, chunks_now, read_errors);
  chunks_ahead = chunks_now = 0;
  for (i = 0; i < MAX_STREAMS; i++) streams[i].used = 0;
  if (cur.hread) CloseHandle(cur.hread);
  if (cur.hrw) CloseHandle(cur.hrw);
  memset(&cur, 0, sizeof(cur));
}


/* The drive of the disc that was opened last ('\0' before any). */
char disc_letter(void) { return last_letter; }

/* A game asks for another disc: the disc that is in the drive now is opened in the place of the one that was. The streams that the game still holds
   from the old disc stay as they are (it closes them when it loads the new disc: a slot that was given to a new stream meanwhile would be closed
   with them). 1 when the drive holds a Sega CD disc. */
int disc_swap(void)
{
  static disc_stream saved[MAX_STREAMS];
  char letter = last_letter;
  int ok;

  if (!letter) return 0;
  memcpy(saved, streams, sizeof(saved));
  ok = disc_open(letter);
  memcpy(streams, saved, sizeof(saved));
  last_letter = letter;
  return ok;
}

const char *disc_path(void) { return cur.letter ? cur.path : NULL; }
int disc_is_path(const char *path) { return path && !_strnicmp(path, DISC_PREFIX, strlen(DISC_PREFIX)); }
/* To the log: how the reading of the disc went (called when the game of a disc closes). */
/* The counts of the log start again (the check of a disc for the 32X reads 16 MB before the game starts: that is not the game's reading). */
void disc_stats_reset(void)
{
  chunks_ahead = chunks_now = now_data = now_audio = 0;
  read_errors = slow_reads = 0;
}

void disc_log_stats(void)
{
  if (!cur.letter) return;
  gui_log("Disc: %d chunks were read ahead in time, %d were read when the game asked for them (%d data, %d audio), %d read errors", chunks_ahead, chunks_now, now_data, now_audio, read_errors);
  chunks_ahead = chunks_now = now_data = now_audio = 0;
  miss_log = 0;
}

int disc_audio_ok(void)
{
  int i;

  if (!cur.letter) return 0;
  for (i = 0; i < cur.ntracks; i++) if (!cur.tr[i].data) return cur.audio_method != 0;
  return 1;                      /* no audio track: nothing to read */
}

/* The disc of a path (disc:\<title>.cue) is in a drive: opens it. A disc that is open already is used as it is. */
int disc_open_for_path(const char *path)
{
  disc_found found[8];
  int n, i;

  if (!disc_is_path(path)) return 0;
  if (cur.letter && !lstrcmpiA(cur.path, path)) return 1;
  n = disc_scan_sega(found, 8);
  for (i = 0; i < n; i++)
  {
    char want[160];

    _snprintf(want, sizeof(want) - 1, DISC_PREFIX "%s.cue", found[i].title);
    want[sizeof(want) - 1] = '\0';
    if (!lstrcmpiA(want, path)) return disc_open(found[i].letter);
  }
  return 0;
}

/* ---- the streams ---- */

int disc_owns(const void *f)
{
  return f && (const disc_stream *)f >= streams && (const disc_stream *)f < streams + MAX_STREAMS;
}

static long long stream_size(const disc_stream *s)
{
  return s->kind == 0 ? (long long)strlen(cur.sheet) : (long long)s->sectors * (s->kind == 1 ? 2048 : 2352);
}

void *disc_fopen(const char *path)
{
  const char *name;
  int i, k = -1;
  disc_stream *s;

  if (!cur.letter || !disc_is_path(path)) return NULL;
  name = path + strlen(DISC_PREFIX);
  for (i = 0; i < MAX_STREAMS; i++) if (!streams[i].used) { k = i; break; }
  if (k < 0) return NULL;
  s = &streams[k];
  memset(s, 0, sizeof(*s));
  if (!_strnicmp(name, "track", 5))
  {
    int number = atoi(name + 5);

    for (i = 0; i < cur.ntracks; i++) if (cur.tr[i].number == number) break;
    if (i == cur.ntracks) return NULL;
    s->track = i;
    s->kind = cur.tr[i].data ? 1 : 2;
    s->lba0 = cur.tr[i].lba;
    s->sectors = cur.tr[i].data ? cur.data_sectors : ((i + 1 < cur.ntracks ? cur.tr[i + 1].lba : cur.end) - cur.tr[i].lba);
    if (s->sectors < 0) s->sectors = 0;
  }
  else if (strlen(name) > 4 && !lstrcmpiA(name + strlen(name) - 4, ".cue")) s->kind = 0;
  else return NULL;
  s->used = 1;
  return s;
}

int disc_fclose(void *f)
{
  disc_stream *s = (disc_stream *)f;

  if (!disc_owns(f)) return EOF;
  s->used = 0;
  return 0;
}

int disc_fseek(void *f, long off, int whence)
{
  disc_stream *s = (disc_stream *)f;
  long long p = s->pos, size = stream_size(s);

  if (!disc_owns(f)) return -1;
  if (whence == SEEK_SET) p = off;
  else if (whence == SEEK_CUR) p += off;
  else if (whence == SEEK_END) p = size + off;
  else return -1;
  if (p < 0) return -1;
  s->pos = p;
  return 0;
}

long disc_ftell(void *f) { return disc_owns(f) ? (long)((disc_stream *)f)->pos : -1; }

/* Sectors of the data track into a chunk (normal reads; a sector that cannot be read is zeros, and counted). */
static void fill_data(slot_t *sl, long first_abs, long last_abs)
{
  LARGE_INTEGER pos;
  long count = last_abs - first_abs;
  DWORD got = 0, t0 = GetTickCount(), took;
  long i;
  unsigned char *buf = sl->buf;

  if (count > DATA_CACHE_SECTORS) count = DATA_CACHE_SECTORS;
  if (count < 1) count = 1;
  EnterCriticalSection(&io_cs);
  pos.QuadPart = (LONGLONG)first_abs * 2048;
  if (!(SetFilePointerEx(cur.hread, pos, NULL, FILE_BEGIN) && ReadFile(cur.hread, buf, (DWORD)(count * 2048), &got, NULL) && got == (DWORD)(count * 2048)))
  {
    /* one sector at a time: the bad ones are zeros */
    for (i = 0; i < count; i++)
    {
      DWORD e = 0, m = 0;

      if (!read_cooked(cur.hread, first_abs + i, buf + i * 2048, &m, &e))
      {
        memset(buf + i * 2048, 0, 2048);
        if (read_errors++ < 20) gui_log("Disc: the data sector %ld could not be read (error %lu)", first_abs + i, (unsigned long)e);
      }
    }
  }
  if (first_abs == cur.tr[0].lba)
  {
    /* The first sector of a Sega CD disc starts with SEGADISCSYSTEM: when it does not, the read is done again (and the log says what came). */
    int tries;

    for (tries = 0; tries < 4 && memcmp(buf, "SEGADISCSYSTEM", 14); tries++)
    {
      gui_log("Disc: the first sector read did not start with SEGADISCSYSTEM (%02X %02X %02X %02X %02X %02X %02X %02X): reading it again (%d)",
              buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7], tries + 1);
      Sleep(150);
      pos.QuadPart = (LONGLONG)first_abs * 2048;
      if (!(SetFilePointerEx(cur.hread, pos, NULL, FILE_BEGIN) && ReadFile(cur.hread, buf, (DWORD)(count * 2048), &got, NULL) && got == (DWORD)(count * 2048)))
        for (i = 0; i < count; i++) { DWORD e = 0, m = 0; if (!read_cooked(cur.hread, first_abs + i, buf + i * 2048, &m, &e)) memset(buf + i * 2048, 0, 2048); }
    }
    if (tries == 0) gui_log("Disc: the first sector read is as it should be");
    else if (memcmp(buf, "SEGADISCSYSTEM", 14)) gui_log("Disc: the first sector could not be read right (the game will probably not start)");
    else gui_log("Disc: the first sector was read right at try %d", tries + 1);
  }
  LeaveCriticalSection(&io_cs);
  took = GetTickCount() - t0;
  if (took > 300 && slow_reads++ < 10) gui_log("Disc: reading %ld data sectors at %ld took %lu ms", count, first_abs, (unsigned long)took);
  sl->first = first_abs;
  sl->n = (int)count;
}

/* Sectors of an audio track into a chunk (raw reads; silence when the drive cannot give them). */
static void fill_audio(slot_t *sl, long first_abs, long last_abs)
{
  long count = last_abs - first_abs;
  DWORD err = 0, t0 = GetTickCount(), took;
  unsigned sense = 0;
  int ok = 0;

  if (count > AUDIO_CACHE_SECTORS) count = AUDIO_CACHE_SECTORS;
  if (count < 1) count = 1;
  EnterCriticalSection(&io_cs);
  if (cur.audio_method == 1) ok = read_raw(cur.hread, first_abs, (int)count, CDDA, sl->buf, &err);
  else if (cur.audio_method == 2) ok = read_cd_scsi(cur.hrw, first_abs, (int)count, 1, sl->buf, &err, &sense);
  LeaveCriticalSection(&io_cs);
  if (!ok)
  {
    memset(sl->buf, 0, (size_t)count * 2352);
    if (cur.audio_method && read_errors++ < 20) gui_log("Disc: the audio sectors at %ld could not be read (error %lu, sense %06X)", first_abs, (unsigned long)err, sense);
  }
  took = GetTickCount() - t0;
  if (took > 300 && slow_reads++ < 10) gui_log("Disc: reading %ld audio sectors at %ld took %lu ms", count, first_abs, (unsigned long)took);
  sl->first = first_abs;
  sl->n = (int)count;
}

static void fill_chunk(cache_t *c, slot_t *sl, long first, long limit)
{
  if (c == &dcache) fill_data(sl, first, limit);
  else fill_audio(sl, first, limit);
}

/* The reader thread: reads the chunk after the one being used, for each kind in turn, whenever one was asked for. */
static DWORD WINAPI reader_proc(LPVOID arg)
{
  cache_t *kinds[ACACHES + 1];
  int k;

  (void)arg;
  for (k = 0; k < ACACHES; k++) kinds[k] = &acaches[k];       /* (the music first: it has a deadline, the data does not) */
  kinds[ACACHES] = &dcache;
  while (!reader_quit)
  {
    WaitForSingleObject(reader_work, INFINITE);
    if (reader_quit) break;
    for (k = 0; k < ACACHES + 1; k++)
    {
      cache_t *c = kinds[k];
      long first, limit;

      EnterCriticalSection(&cache_cs);
      if (!c->req_pending) { LeaveCriticalSection(&cache_cs); continue; }
      c->req_pending = 0;
      first = c->req_first;
      limit = c->req_limit;
      c->ahead.first = -1;
      c->ahead.n = 0;
      c->busy = 1;
      c->busy_first = first;
      c->busy_n = (int)((limit - first) < c->max_n ? (limit - first) : c->max_n);
      ResetEvent(c->done);
      LeaveCriticalSection(&cache_cs);

      fill_chunk(c, &c->ahead, first, limit);

      EnterCriticalSection(&cache_cs);
      c->busy = 0;
      SetEvent(c->done);
      LeaveCriticalSection(&cache_cs);
    }
  }
  return 0;
}

static void reader_start(void)
{
  if (reader_stale)                                  /* a reader that was left running by reader_stop: it is given time to finish before its handles and caches are made again */
  {
    reader_stale = 0;
    if (reader_thread && WaitForSingleObject(reader_thread, 15000) != WAIT_OBJECT_0) gui_log("Disc: the old reader thread is still running");
    if (reader_thread) { CloseHandle(reader_thread); reader_thread = NULL; }
    if (reader_work) { CloseHandle(reader_work); reader_work = NULL; }
  }
  if (!cs_ready) { InitializeCriticalSection(&cache_cs); InitializeCriticalSection(&io_cs); cs_ready = 1; }
  memset(&dcache, 0, sizeof(dcache));
  memset(acaches, 0, sizeof(acaches));
  dcache.unit = 2048; dcache.max_n = DATA_CACHE_SECTORS; dcache.cur.buf = dbuf[0]; dcache.ahead.buf = dbuf[1];
  dcache.cur.first = dcache.ahead.first = -1;
  dcache.done = CreateEventA(NULL, TRUE, TRUE, NULL);
  {
    int i;

    for (i = 0; i < ACACHES; i++)
    {
      acaches[i].unit = 2352; acaches[i].max_n = AUDIO_CACHE_SECTORS;
      acaches[i].cur.buf = abuf[2 * i]; acaches[i].ahead.buf = abuf[2 * i + 1];
      acaches[i].cur.first = acaches[i].ahead.first = -1;
      acaches[i].done = CreateEventA(NULL, TRUE, TRUE, NULL);
      acache_owner[i] = -1;
      acache_used[i] = 0;
    }
  }
  use_clock = 0;
  reader_work = CreateEventA(NULL, FALSE, FALSE, NULL);
  reader_quit = 0;
  reader_thread = CreateThread(NULL, 0, reader_proc, NULL, 0, NULL);
  if (reader_thread) SetThreadPriority(reader_thread, THREAD_PRIORITY_ABOVE_NORMAL);
}

static void reader_stop(void)
{
  if (reader_thread)
  {
    reader_quit = 1;
    SetEvent(reader_work);
    if (WaitForSingleObject(reader_thread, 15000) != WAIT_OBJECT_0)
    {
      /* The drive does not answer: the thread is still inside a read. Its handles stay open (it uses them when it comes back and sees reader_quit);
         reader_start() waits for it again before it makes new ones. */
      gui_log("Disc: the reader thread did not stop in 15 seconds (the drive did not answer)");
      reader_stale = 1;
      return;
    }
    CloseHandle(reader_thread);
    reader_thread = NULL;
  }
  if (reader_work) { CloseHandle(reader_work); reader_work = NULL; }
  if (dcache.done) { CloseHandle(dcache.done); dcache.done = NULL; }
  {
    int i;

    for (i = 0; i < ACACHES; i++) if (acaches[i].done) { CloseHandle(acaches[i].done); acaches[i].done = NULL; }
  }
}

/* The audio cache of a track: the one that holds it, else the least recently used one is given to it. */
static cache_t *audio_cache_for(int track)
{
  int i, best = 0;
  cache_t *c;

  EnterCriticalSection(&cache_cs);
  use_clock++;
  for (i = 0; i < ACACHES; i++)
    if (acache_owner[i] == track) { acache_used[i] = use_clock; LeaveCriticalSection(&cache_cs); return &acaches[i]; }
  for (i = 1; i < ACACHES; i++) if (acache_used[i] < acache_used[best]) best = i;
  if (acaches[best].busy)                           /* (the one used longest ago is still being filled: another one that is not will do) */
    for (i = 0; i < ACACHES; i++) if (!acaches[i].busy && (acaches[best].busy || acache_used[i] < acache_used[best])) best = i;
  c = &acaches[best];
  {
    int waits = 0;

    while (c->busy && waits < 3)                    /* (the reader is still filling what it held: three tries at most, as in get_chunk, so a drive that stopped answering cannot hang the game) */
    {
      LeaveCriticalSection(&cache_cs);
      WaitForSingleObject(c->done, 5000);
      EnterCriticalSection(&cache_cs);
      waits++;
    }
  }
  acache_owner[best] = track;
  acache_used[best] = use_clock;
  c->cur.first = c->ahead.first = -1;
  c->cur.n = c->ahead.n = 0;
  c->req_pending = 0;
  LeaveCriticalSection(&cache_cs);
  return c;
}

/* The chunk that holds a sector, in the chunk being used (cache_cs held by the caller). */
static int in_slot(const slot_t *sl, long abs_sector) { return sl->first >= 0 && abs_sector >= sl->first && abs_sector < sl->first + sl->n; }

/* The chunk that holds abs_sector (limit: the sector the track ends at). The next chunk is asked of the reader. */
static const slot_t *get_chunk(cache_t *c, long abs_sector, long limit)
{
  const slot_t *got;
  int sync_fill = 0;

  EnterCriticalSection(&cache_cs);
  if (!in_slot(&c->cur, abs_sector))
  {
    /* wait for the reader when it is reading just this */
    {
      int waits = 0;

      while (c->busy && abs_sector >= c->busy_first && abs_sector < c->busy_first + c->busy_n && waits < 3)       /* (three tries at most: a stuck reader must not hang the game) */
      {
        LeaveCriticalSection(&cache_cs);
        WaitForSingleObject(c->done, 5000);
        EnterCriticalSection(&cache_cs);
        waits++;
      }
    }
    if (in_slot(&c->ahead, abs_sector) && !c->busy)
    {
      slot_t t = c->cur;                         /* the chunk read ahead is the one to use now */

      c->cur = c->ahead;
      c->ahead = t;
      c->ahead.first = -1;
      c->ahead.n = 0;
      chunks_ahead++;
    }
    else sync_fill = 1;
  }
  if (sync_fill)
  {
    LeaveCriticalSection(&cache_cs);
    chunks_now++;
    if (c == &dcache) now_data++; else now_audio++;
    if (c == &dcache && abs_sector - 16 >= cur.tr[0].lba) abs_sector -= 16;       /* (the game reads back a little now and then: those sectors are held too) */
    if (miss_log < 40)
    {
      miss_log++;
      gui_log("Disc: %s chunk read at once: sector %ld (the chunk in use %ld + %d, the one ahead %ld + %d, reader busy %d, asked %d)", c == &dcache ? "data" : "audio", abs_sector,
              c->cur.first, c->cur.n, c->ahead.first, c->ahead.n, c->busy, c->req_pending);
    }
    fill_chunk(c, (slot_t *)&c->cur, abs_sector, limit);               /* not read ahead: read now (only the player's thread touches cur) */
    EnterCriticalSection(&cache_cs);
  }
  got = &c->cur;
  if (!c->busy && !c->req_pending && cur.letter)
  {
    long next = c->cur.first + c->cur.n;

    /* the chunk read ahead must be the one right after the chunk in use: after a jump the old one (from another place) is not, and is read again */
    if (next < limit && c->ahead.first != next) { c->req_pending = 1; c->req_first = next; c->req_limit = limit; SetEvent(reader_work); }
  }
  LeaveCriticalSection(&cache_cs);
  return got;
}

size_t disc_fread(void *buf, size_t size, size_t n, void *f)
{
  disc_stream *s = (disc_stream *)f;
  unsigned char *dst = (unsigned char *)buf;
  long long total, want, have = 0, ssize;
  int unit;

  if (!disc_owns(f) || !size || !n) return 0;
  total = (long long)size * (long long)n;
  ssize = stream_size(s);
  if (s->pos >= ssize) return 0;
  want = total;
  if (s->pos + want > ssize) want = ssize - s->pos;

  if (s->kind == 0)
  {
    memcpy(dst, cur.sheet + s->pos, (size_t)want);
    s->pos += want;
    return (size_t)(want / (long long)size);
  }
  unit = s->kind == 1 ? 2048 : 2352;
  while (have < want)
  {
    long rel = (long)(s->pos / unit);
    long abs_sector = s->lba0 + rel;
    int off = (int)(s->pos % unit);
    long long chunk;
    const unsigned char *src;

    {
      const slot_t *sl = get_chunk(s->kind == 1 ? &dcache : audio_cache_for(s->track), abs_sector, s->lba0 + s->sectors);

      src = sl->buf + (abs_sector - sl->first) * unit;
      chunk = (long long)(sl->first + sl->n - abs_sector) * unit - off;
    }
    if (chunk > want - have) chunk = want - have;
    memcpy(dst + have, src + off, (size_t)chunk);
    have += chunk;
    s->pos += chunk;
  }
  return (size_t)(have / (long long)size);
}

char *disc_fgets(char *out, int n, void *f)
{
  disc_stream *s = (disc_stream *)f;
  int k = 0;
  long long size;

  if (!disc_owns(f) || n < 2 || s->kind != 0) return NULL;
  size = stream_size(s);
  if (s->pos >= size) return NULL;
  while (k < n - 1 && s->pos < size)
  {
    char c = cur.sheet[s->pos++];

    out[k++] = c;
    if (c == '\n') break;
  }
  out[k] = '\0';
  return out;
}
