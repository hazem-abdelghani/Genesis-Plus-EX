/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  movie.c -- input recording and playback (.gpm). A recording holds the
 *  state of the game at the moment it started and every frame's controller
 *  input after it; playback loads that state and feeds the inputs back.
 ****************************************************************************/

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shared.h"
#include "gui.h"
#include "netplay.h"

#define MOVIE_MAGIC  0x314D5047u   /* "GPM1": the first layout (no version number) */
#define MOVIE_MAGIC2 0x324D5047u   /* "GPM2": magic, version, bytes per frame, then as GPM1 */
#define MOVIE_VERSION 2
#define FRAME_BYTES (MAX_DEVICES * 2 + MAX_DEVICES * 4)

static FILE *rec;
static long rec_count_pos;
static unsigned rec_frames;
static int rec_failed;            /* a write did not go through (disk full, drive removed) */
static unsigned char *play;
static size_t play_len, play_pos;

static void tmp_state_path(char *out, int n)
{
  char d[MAX_PATH];
  GetTempPathA(sizeof(d), d);
  snprintf(out, (size_t)n, "%sgpexmovie.tmp", d);
}

void movie_stop(void)
{
  if (rec)
  {
    int ok = !rec_failed && fseek(rec, rec_count_pos, SEEK_SET) == 0 && fwrite(&rec_frames, 4, 1, rec) == 1;
    if (fclose(rec) != 0) ok = 0;
    rec = NULL;
    if (ok) gui_notify("Input recording saved (%u frames)", rec_frames);
    else    gui_notify("The input recording could not be saved completely (is the disk full?)");
  }
  if (play) { free(play); play = NULL; gui_notify("Input playback stopped"); }
}

/* Starts recording from this moment into `path`. */
int movie_record(const char *path)
{
  char tmp[MAX_PATH];
  FILE *fp;
  unsigned char *st;
  long len;
  unsigned magic = MOVIE_MAGIC2, version = MOVIE_VERSION, fbytes = FRAME_BYTES, crc = (unsigned)emu_rom_crc32(), zero = 0;
  unsigned l;
  int ok;

  if (!emu_running || netplay_active()) return 0;
  movie_stop();
  tmp_state_path(tmp, sizeof(tmp));
  emu_save_state_file(tmp);
  fp = fopen(tmp, "rb");
  if (!fp) return 0;
  fseek(fp, 0, SEEK_END); len = ftell(fp); fseek(fp, 0, SEEK_SET);
  st = (unsigned char *)malloc((size_t)len);
  if (!st || fread(st, 1, (size_t)len, fp) != (size_t)len) { fclose(fp); free(st); return 0; }
  fclose(fp);
  DeleteFileA(tmp);

  rec = fopen(path, "wb");
  if (!rec) { free(st); return 0; }
  l = (unsigned)len;
  ok = fwrite(&magic, 4, 1, rec) == 1 && fwrite(&version, 4, 1, rec) == 1 && fwrite(&fbytes, 4, 1, rec) == 1 &&
       fwrite(&crc, 4, 1, rec) == 1 && fwrite(&l, 4, 1, rec) == 1 && fwrite(st, 1, (size_t)len, rec) == (size_t)len;
  free(st);
  rec_count_pos = ftell(rec);
  ok = ok && fwrite(&zero, 4, 1, rec) == 1;
  if (!ok)
  {
    fclose(rec);
    rec = NULL;
    DeleteFileA(path);                  /* not a half-written file */
    return 0;
  }
  rec_failed = 0;
  rec_frames = 0;
  gui_notify("Recording input...");
  return 1;
}

/* Loads the recording's state and plays its inputs. 0 with a reason in `err` if it cannot. */
int movie_play(const char *path, char *err, int err_len)
{
  FILE *fp = fopen(path, "rb");
  unsigned magic, crc, slen, frames;
  unsigned char *state;
  char tmp[MAX_PATH];
  long rest_pos;
  size_t want;

  if (!emu_running) { snprintf(err, (size_t)err_len, "Open the game first."); return 0; }
  if (netplay_active()) { snprintf(err, (size_t)err_len, "Not during netplay."); return 0; }
  if (!fp) { snprintf(err, (size_t)err_len, "The file could not be opened."); return 0; }
  if (fread(&magic, 4, 1, fp) != 1 || (magic != MOVIE_MAGIC && magic != MOVIE_MAGIC2))
  { fclose(fp); snprintf(err, (size_t)err_len, "That is not an input recording."); return 0; }
  if (magic == MOVIE_MAGIC2)
  {
    unsigned version = 0, fbytes = 0;
    if (fread(&version, 4, 1, fp) != 1 || fread(&fbytes, 4, 1, fp) != 1)
    { fclose(fp); snprintf(err, (size_t)err_len, "The recording is damaged."); return 0; }
    if (version > MOVIE_VERSION)
    { fclose(fp); snprintf(err, (size_t)err_len, "That recording was made by a newer version of the program."); return 0; }
    if (fbytes != FRAME_BYTES)
    { fclose(fp); snprintf(err, (size_t)err_len, "That recording uses a different controller layout."); return 0; }
  }
  if (fread(&crc, 4, 1, fp) != 1 || fread(&slen, 4, 1, fp) != 1 || slen > 64u * 1024 * 1024)
  { fclose(fp); snprintf(err, (size_t)err_len, "That is not an input recording."); return 0; }
  if (crc != (unsigned)emu_rom_crc32())
  { fclose(fp); snprintf(err, (size_t)err_len, "That recording was made for a different game."); return 0; }
  state = (unsigned char *)malloc(slen);
  if (!state || fread(state, 1, slen, fp) != slen || fread(&frames, 4, 1, fp) != 1)
  { fclose(fp); free(state); snprintf(err, (size_t)err_len, "The recording is damaged."); return 0; }
  rest_pos = ftell(fp);
  want = (size_t)frames * FRAME_BYTES;
  movie_stop();
  play = (unsigned char *)malloc(want ? want : 1);
  if (!play || fread(play, 1, want, fp) != want)
  { fclose(fp); free(state); free(play); play = NULL; snprintf(err, (size_t)err_len, "The recording is damaged."); return 0; }
  (void)rest_pos;
  fclose(fp);
  play_len = want;
  play_pos = 0;

  tmp_state_path(tmp, sizeof(tmp));
  fp = fopen(tmp, "wb");
  if (fp) { fwrite(state, 1, slen, fp); fclose(fp); }
  free(state);
  {
    unsigned char *keep = play;
    size_t kl = play_len;
    play = NULL;                      /* loading a state must not run the recording */
    emu_load_state_file(tmp);
    play = keep; play_len = kl; play_pos = 0;
  }
  DeleteFileA(tmp);
  gui_notify("Playing the recording...");
  return 1;
}

/* Called once a frame after the controllers were read. */
void movie_frame(void)
{
  int i;

  if (rec)
  {
    for (i = 0; i < MAX_DEVICES; i++)
    {
      if (fwrite(&input.pad[i], 2, 1, rec) != 1 || fwrite(input.analog[i], 2, 2, rec) != 2) rec_failed = 1;
    }
    rec_frames++;
    if (rec_failed) movie_stop();         /* say so now, not at the end of a long recording */
  }
  else if (play)
  {
    if (play_pos + FRAME_BYTES > play_len) { movie_stop(); return; }
    for (i = 0; i < MAX_DEVICES; i++)
    {
      const unsigned char *p = play + play_pos + (size_t)i * 6;
      memcpy(&input.pad[i], p, 2);
      memcpy(input.analog[i], p + 2, 4);
    }
    play_pos += FRAME_BYTES;
  }
}
