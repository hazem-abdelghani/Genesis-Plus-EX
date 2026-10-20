/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  history.c -- when each game was last opened (View > Recently Played First).
 *
 *  Kept in the [history] section of genesis-plus-ex.ini (written by config_save), one entry
 *  per game: last opened (Unix time) and the full path. The newest 1000 games are kept.
 ****************************************************************************/

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define H_MAX 1000

#include "shared.h"
#include "gui.h"

typedef struct
{
  char path[GUI_PATH_LEN];
  unsigned long long last;
} hist_t;

static hist_t *h_list;
static int h_n, h_cap;
static int h_current = -1;      /* the game being played, or -1 */
static int h_dirty;

static int h_find(const char *path)
{
  int i;
  for (i = 0; i < h_n; i++)
    if (!lstrcmpiA(h_list[i].path, path)) return i;
  return -1;
}

static int h_add(const char *path)
{
  if (h_n >= H_MAX)                      /* full: the game opened longest ago makes room */
  {
    int i, old = 0;
    for (i = 1; i < h_n; i++) if (h_list[i].last < h_list[old].last) old = i;
    memset(&h_list[old], 0, sizeof(hist_t));
    lstrcpynA(h_list[old].path, path, GUI_PATH_LEN);
    return old;
  }
  if (h_n >= h_cap)
  {
    int cap = h_cap ? h_cap * 2 : 256;
    hist_t *n = (hist_t *)realloc(h_list, sizeof(hist_t) * (size_t)cap);
    if (!n) return -1;
    h_list = n;
    h_cap = cap;
  }
  memset(&h_list[h_n], 0, sizeof(hist_t));
  lstrcpynA(h_list[h_n].path, path, GUI_PATH_LEN);
  return h_n++;
}

void history_load(void)
{
  h_n = 0;
  h_current = -1;
  config_history_load();
  h_dirty = 0;
}

void history_restore(const char *path, unsigned long long last)
{
  int idx = h_find(path);
  if (idx < 0) idx = h_add(path);
  if (idx >= 0) h_list[idx].last = last;
}

int history_count(void) { return h_n; }
const char *history_entry_path(int i) { return h_list[i].path; }
unsigned long long history_entry_last(int i) { return h_list[i].last; }

void history_save(void)
{
  if (!h_dirty) return;
  config_save();
  h_dirty = 0;
}

/* A game was opened: now is when it was last played. */
void history_game_started(const char *path)
{
  int i;

  if (!path || !path[0]) return;
  i = h_find(path);
  if (i < 0) i = h_add(path);
  if (i < 0) return;

  h_list[i].last = (unsigned long long)time(NULL);
  h_current = i;
  h_dirty = 1;
  history_save();
}

void history_game_ended(void)
{
  h_current = -1;
  history_save();
}

/* Forgets every game's last-played time. */
/* Forgets when this one game was last opened. */
void history_remove(const char *path)
{
  int i = h_find(path);
  if (i < 0) return;
  if (h_current == i) h_current = -1;
  else if (h_current > i) h_current--;
  memmove(&h_list[i], &h_list[i + 1], sizeof(hist_t) * (size_t)(h_n - i - 1));
  h_n--;
  h_dirty = 1;
  history_save();
}

void history_clear(void)
{
  h_n = 0;
  h_current = -1;
  h_dirty = 1;
  history_save();
}

unsigned long long history_last(const char *path)
{
  int i = h_find(path);
  return i < 0 ? 0 : h_list[i].last;
}
