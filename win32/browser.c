/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  browser.c -- a library view over a folder of ROMs, built into the main
 *  window itself rather than a separate dialog: when nothing is running,
 *  this panel occupies the video area instead of a black screen, and it
 *  steps aside the moment a game starts.
 *
 *  Extension list matches what the core can actually open, verified against
 *  core/loadrom.c and sdl/fileio.c rather than assumed: load_archive() only
 *  ever unpacks a ZIP (sniffed by its "PK" magic bytes) or falls through to
 *  zlib's gzopen(), which transparently handles both real .gz files and, as
 *  a side effect, plain uncompressed ones. .7z is handled separately, by
 *  this frontend rather than the core -- see sevenzip.c and main.c's
 *  emu_load_rom() -- and only when built with CHD=1 (the default), since it
 *  reuses that feature's LZMA decoder; #ifdef USE_LIBCHDR below matches
 *  Makefile.win32's own gate for it. There is no .m3u playlist support --
 *  that's a libretro frontend feature (it parses the playlist itself), not
 *  something core/loadrom.c understands.
 ****************************************************************************/

#include <windows.h>
#include <math.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commdlg.h>
#include <uxtheme.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#include "shared.h"
#include "gui.h"
#ifdef USE_LIBCHDR
#include "cd_hw/libchdr/include/libchdr/chd.h"
#endif
#include "theme.h"
#include "resource.h"
#include "coverart.h"
#include "sevenzip.h"
#include "toolbar.h"
#include <zlib.h>
#include "stb_image.h"

#define BROWSER_MAX_ENTRIES 4000
#define BROWSER_MAX_DEPTH   6
#define BROWSER_COLUMNS     7
#define COL_FAV             2   /* the star; shown first, but numbered after Name and Console */
#define COL_GENRE           3
#define COL_REGION          4
#define COL_SIZE            5
#define COL_TYPE            6
#define COL_RECENT          7   /* not a column: sorts by when a game was last played (the number just past the last column) */
#define RECENT_GROUP_SIZE   7   /* how many games the group at the top holds */
#define COL_FIRST_EXTRA     3   /* columns from here on start hidden (gui.browser_col_extra says which are shown) */

typedef struct
{
  char  full_path[GUI_PATH_LEN];
  char  name[128];
  char  console[40];
  char  region[24];        /* guessed from the file name, for the filter bar */
  char  genre[28];         /* looked up by title, "" when unknown, for the filter bar */
  signed char outside;     /* 1 for a game from outside the library folders: shown only in the Recently Played group */
  signed char fav;         /* 1 when the game is a favorite (refreshed whenever the list is built) */
  unsigned long long last_played;   /* Unix time, 0 = never (refreshed with fav) */
  ULONGLONG size_bytes;
  FILETIME mtime;          /* with size_bytes: tells a rescan the file is unchanged */
  signed char pico_checked; /* 0: a Mega Drive file whose header has not been read yet (see pico_scan_start) */
  signed char cover_ext;   /* COVERART_UNCHECKED until the grid first looks this ROM's cover up, then cached */
  int   img_slot;          /* grid view: index in the image list, valid while img_gen == grid_gen */
  int   img_gen;
} browser_entry_t;

static browser_entry_t *entries;
static int entry_count;
static int outside_shown;    /* of those, how many the list shows now (in the Recently Played group) */
static int outside_n;        /* how many of them are such games (not counted as part of the library) */

/* The previous scan's entries. A rescan (which happens every time the browser
   is shown again) reuses their console label for archives that have not
   changed instead of opening every .zip again. */
static browser_entry_t *spare;
static int spare_count;

static int spare_hint;      /* where the last file was found: the next one is usually the entry after it (a scan lists the files in the same order) */

static int spare_matches(int k, const char *full, const WIN32_FIND_DATAA *fd)
{
  return spare[k].pico_checked && spare[k].size_bytes == (((ULONGLONG)fd->nFileSizeHigh << 32) | fd->nFileSizeLow) &&
         CompareFileTime(&spare[k].mtime, &fd->ftLastWriteTime) == 0 && !lstrcmpA(spare[k].full_path, full);
}

/* Console label of an unchanged file from the previous scan, or NULL. */
static const char *cached_archive_console(const char *full, const WIN32_FIND_DATAA *fd)
{
  int k;

  if (spare_hint < spare_count && spare_matches(spare_hint, full, fd)) return spare[spare_hint++].console;
  for (k = 0; k < spare_count; k++)
    if (spare_matches(k, full, fd)) { spare_hint = k + 1; return spare[k].console; }
  return NULL;
}

/* Which entries match the current search text, in the order they're shown
   -- the ListView's row N is entries[filtered_index[N]], not entries[N],
   whenever a filter is active. */
static int filtered_index[BROWSER_MAX_ENTRIES];
static int filtered_count;
/* A row that is not a game: -1 = the "Recently Played" heading, -2 = the "All Games" heading (list view only). */
#define ROW_HEAD_RECENT (-1)
#define ROW_HEAD_ALL    (-2)
#define ROW_PAD         (-3)      /* grid view: an empty cell, to end the row of a heading or of the games before the next heading */
static int searching;        /* the search box has text: the whole library is searched */
#define ROW_HEAD_CONSOLE (-100)      /* -100 - n: the heading of the n-th console when the list is grouped by console */
static char console_heads[24][40];
static int  console_head_n;
static const char *heading_label(int kind)
{
  if (kind <= ROW_HEAD_CONSOLE) return console_heads[(ROW_HEAD_CONSOLE - kind) % 24];
  if (kind == ROW_HEAD_RECENT) return "Recently Played";
  return (gui.favorites_only && !searching) ? "Favorites" : "All Games";
}
static int heading_rows;     /* heading rows in the list (0, 1 or 2) */
static int recent_group_n;   /* games in the Recently Played group at the top */
static int recent_dup;       /* 1: those games are in the list a second time, in All Games (when there is room for the copies) */
static void browser_apply_filter(void);
static void grid_hide_top_blank(void);
void browser_grid_check_cols(void);
static int grid_headed;      /* the grid has heading rows (with empty cells that fill the rest of their rows) */
static int grid_built_cols;  /* how many covers a row held when they were made */
static int grid_cols_force;  /* the number measured on screen, when it was not what was worked out */

/* The game on a row, or NULL for a heading or no such row. */
static browser_entry_t *row_entry(int row)
{
  if (row < 0 || row >= filtered_count || filtered_index[row] < 0) return NULL;
  return &entries[filtered_index[row]];
}

/* The panel's own child controls, created once and reused for the life of
   the app -- shown when nothing is running, hidden the moment it is. The
   toolbar, search bar and filter bar sit inside panel_bar, one container
   window that fills its own background and passes its children's messages on
   to the main window. */
static HWND panel_toolbar;   /* the toolbar: also shown while a game runs in a window */
static HWND panel_bar;       /* search bar and filter bar */
static HWND panel_search;
static HWND panel_search_label;
static HWND panel_search_close;
static HWND panel_search_clear;     /* the x inside the search box */
static HWND panel_filter[3];       /* console, region, folder */
static HWND panel_filter_reset;
static HWND panel_list;
static HWND panel_az;        /* the A to Z strip beside the list */
static HWND panel_top;
static int  top_layered;       /* the back-to-top button (the round up-arrow that jumps to the first row once the list is scrolled down) is a layered window with soft edges */
static int  top_render(HWND hwnd);
void browser_rebuild_grid_images(void);
static void az_update(void);
static LRESULT CALLBACK az_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
static LRESULT CALLBACK top_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
#define AZ_W TBS(22)
static HWND panel_choose;
static int  panel_visible;

/* Toolbar buttons. */
static HWND tb_open, tb_refresh, tb_fav, tb_search, tb_play, tb_pause, tb_stop, tb_reset, tb_fs, tb_shot,
            tb_set, tb_video, tb_audio, tb_ctl, tb_sep[2];

static int toolbar_current_h(void);
#define TOOLBAR_H toolbar_current_h()    /* from the icon size, the labels and how many rows the window width needs */

/* Ctrl+F shows the search bar for now even when View > Toolbars > Show Search Bar is
   off; the Close button and the menu item both go through
   browser_toggle_searchbar(), so they always agree. */
static DWORD search_autohide_tick;   /* when an empty Ctrl+F search bar last went away because it lost focus */
static int  search_override = -1;   /* Ctrl+F: 1 = shown, 0 = hidden, for this run only; -1 = follow View > Toolbars > Show Search Bar */

/* Column layout: widths are recomputed proportionally on every relayout,
   so "remembered width" for a hidden column is just whatever it would be
   at the current window size, not a stale value from before a resize. */
static int col_hidden[BROWSER_COLUMNS];
static int last_list_w = 300;
static const char *col_names[BROWSER_COLUMNS] = { "Name", "Console", "Favorite", "Genre", "Region", "File Size", "File Type" };
static const int col_order[BROWSER_COLUMNS] = { COL_FAV, 0, 1, COL_GENRE, COL_REGION, COL_SIZE, COL_TYPE };   /* as laid out: the star first */

/* The star and Genre have a fixed width; the other four share what is
   left by percent. */
#define FAV_COL_W TBS(56)

static int fixed_col_w(int i)
{
  if (i >= COL_FAV && i < 12 && gui.browser_col_w[i] > 0) return gui.browser_col_w[i];   /* a width the person set */
  switch (i)
  {
    case COL_FAV:   return FAV_COL_W;
    case COL_GENRE: return TBS(120);
    case COL_REGION: return TBS(90);
    case COL_SIZE:  return TBS(80);
    case COL_TYPE:  return TBS(70);
  }
  return 0;
}

static int list_pct_base(void)
{
  int i, w = last_list_w;
  for (i = COL_FAV; i < BROWSER_COLUMNS; i++)
    if (!col_hidden[i]) w -= fixed_col_w(i);
  return w > 100 ? w : 100;
}

static int col_width(int i)
{
  if (col_hidden[i]) return 0;
  if (i >= COL_FAV) return fixed_col_w(i);
  return (list_pct_base() * gui.browser_col_pct[i]) / 100;
}

/****************************************************************************
 * Classification
 ****************************************************************************/

static const char *classify_extension(const char *ext)
{
  if (!lstrcmpiA(ext, "md")  || !lstrcmpiA(ext, "gen") || !lstrcmpiA(ext, "bin") ||
      !lstrcmpiA(ext, "smd") || !lstrcmpiA(ext, "mdx") || !lstrcmpiA(ext, "68k"))
    return "Mega Drive / Genesis";

  if (!lstrcmpiA(ext, "32x")) return "32X";

  if (!lstrcmpiA(ext, "sms")) return "Master System";
  if (!lstrcmpiA(ext, "gg"))  return "Game Gear";
  if (!lstrcmpiA(ext, "sg"))  return "SG-1000";

  if (!lstrcmpiA(ext, "cue") || !lstrcmpiA(ext, "iso") || !lstrcmpiA(ext, "chd"))
    return "Mega CD / Sega CD";

  if (!lstrcmpiA(ext, "zip") || !lstrcmpiA(ext, "gz")
#ifdef USE_LIBCHDR
      || !lstrcmpiA(ext, "7z")
#endif
      )
    return "Archive";

  return NULL;
}

/* A Mega Drive file whose header (at 0x100) says SEGA PICO is a Pico game. */
#define PICO_LABEL "Sega Pico"
static int header_is_pico(const unsigned char *b, int n)
{
  return n >= 0x110 && !memcmp(b + 0x100, "SEGA PICO", 9);
}

static int is_md_label(const char *console)
{
  return console && !lstrcmpiA(console, "Mega Drive / Genesis");
}

/* The console of a plain Mega Drive file: Sega Pico when its header says so. */
static const char *refine_pico_file(const char *console, const char *full_path)
{
  unsigned char b[0x110];
  DWORD got = 0;
  HANDLE f;

  if (!is_md_label(console)) return console;
  f = CreateFileA(full_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (f == INVALID_HANDLE_VALUE) return console;
  if (!ReadFile(f, b, sizeof(b), &got, NULL)) got = 0;
  CloseHandle(f);
  return header_is_pico(b, (int)got) ? PICO_LABEL : console;
}

#ifdef USE_LIBCHDR
static int is_7z_path(const char *full_path)
{
  const char *dot = strrchr(full_path, '.');
  return dot && !lstrcmpiA(dot, ".7z");
}
#endif

/* Placeholder so browser_console_for_path (below) can use classify_archive. */
static const char *classify_archive(const char *full_path, int look);

/* Console for a ROM path -- by extension, looking inside zip archives -- or
   NULL when it isn't recognized. Used for the Open Recent labels. */
const char *browser_console_for_path(const char *full_path)
{
  const char *name = full_path, *p, *dot;
  const char *console;

  if (!full_path) return NULL;
  for (p = full_path; *p; p++)
    if (*p == '\\' || *p == '/') name = p + 1;

  dot = strrchr(name, '.');
  if (!dot || !dot[1]) return NULL;

  console = classify_extension(dot + 1);
  if (console && !lstrcmpiA(console, "Archive")) console = classify_archive(full_path, 1);
  else console = refine_pico_file(console, full_path);
  if (console && !lstrcmpiA(console, "Archive")) return NULL;
  return console;
}

#ifdef USE_LIBCHDR
/* Same idea as classify_archive's zip branch below, for a .7z archive. */
static const char *classify_7z(const char *full_path, int look)
{
  sevenzip_t *sz = sevenzip_open(full_path);
  int i, count;

  if (!sz) return "Archive";

  count = sevenzip_count(sz);
  for (i = 0; i < count; i++)
  {
    char inner_name[MAX_PATH];
    const char *inner_dot;

    if (sevenzip_entry_is_dir(sz, i)) continue;

    sevenzip_entry_name(sz, i, inner_name, sizeof(inner_name));
    inner_dot = strrchr(inner_name, '.');
    if (inner_dot && inner_dot[1])
    {
      const char *console = classify_extension(inner_dot + 1);
      if (console && lstrcmpiA(console, "Archive") != 0)
      {
        if (look && is_md_label(console))
        {
          unsigned char b[0x110];
          int got = sevenzip_extract(sz, i, b, (int)sizeof(b));
          if (got > 0 && header_is_pico(b, got)) console = PICO_LABEL;
        }
        sevenzip_close(sz);
        return console;
      }
    }
  }

  sevenzip_close(sz);
  return "Archive";
}
#endif

/* Peeks inside a zip or 7z archive to find what console it actually holds,
   based on the first entry whose own extension classify_extension
   recognizes -- multi-entry archives (a ROM plus a manual, or several
   discs) are still handled since every entry is checked in order, not
   just the first one. Falls back to plain "Archive" if the file can't
   be opened, isn't a real archive, or nothing inside is recognized.
   With `look`, a Mega Drive game inside is also read for the SEGA PICO header (an extra read of the archive, so a scan leaves it to a
   thread afterwards). */
static const char *classify_archive(const char *full_path, int look)
{
  unzFile zf;
  int result;

#ifdef USE_LIBCHDR
  if (is_7z_path(full_path)) return classify_7z(full_path, look);
#endif

  zf = unzOpen(full_path);
  if (!zf) return "Archive";

  result = unzGoToFirstFile(zf);
  while (result == UNZ_OK)
  {
    char inner_name[MAX_PATH];
    const char *inner_dot;

    if (unzGetCurrentFileInfo(zf, NULL, inner_name, sizeof(inner_name), NULL, 0, NULL, 0) == UNZ_OK)
    {
      inner_dot = strrchr(inner_name, '.');
      if (inner_dot && inner_dot[1])
      {
        const char *console = classify_extension(inner_dot + 1);
        /* Recognized and not itself another archive -- zip-in-zip isn't
           worth recursing into for a file browser column. */
        if (console && lstrcmpiA(console, "Archive") != 0)
        {
          if (look && is_md_label(console) && unzOpenCurrentFile(zf) == UNZ_OK)
          {
            unsigned char b[0x110];
            int got = unzReadCurrentFile(zf, b, sizeof(b));
            unzCloseCurrentFile(zf);
            if (header_is_pico(b, got)) console = PICO_LABEL;
          }
          unzClose(zf);
          return console;
        }
      }
    }

    result = unzGoToNextFile(zf);
  }

  unzClose(zf);
  return "Archive";
}

/* Case-insensitive substring test -- lstrcmpiA compares whole strings, not
   what's needed for "does the search text appear anywhere in this name". */
static int contains_ci(const char *haystack, const char *needle)
{
  int hlen, nlen, i;

  if (!needle[0]) return 1;

  hlen = lstrlenA(haystack);
  nlen = lstrlenA(needle);
  if (nlen > hlen) return 0;

  for (i = 0; i <= hlen - nlen; i++)
  {
    if (CompareStringA(LOCALE_USER_DEFAULT, NORM_IGNORECASE,
                        haystack + i, nlen, needle, nlen) == CSTR_EQUAL)
    {
      return 1;
    }
  }
  return 0;
}

/* Region from the file name's parenthesized tags -- "(USA)", "(Europe)",
   "(Japan, USA)", or the short forms "(U)", "(E)", "(J)", "(UE)" -- or
   "Unknown" when the name carries none. */
static const char *const region_words[] =
  { "USA", "Europe", "Japan", "World", "Asia", "Korea", "Brazil", "Australia", "Germany",
    "France", "Spain", "Italy", "Sweden", "Netherlands", "China", "Taiwan", "Canada",
    "UK", "Russia", "Hong Kong", NULL };

static int is_region_word(const char *s, int n)
{
  int i;
  for (i = 0; region_words[i]; i++)
    if ((int)lstrlenA(region_words[i]) == n &&
        CompareStringA(LOCALE_USER_DEFAULT, NORM_IGNORECASE, s, n, region_words[i], n) == CSTR_EQUAL)
      return 1;
  return 0;
}

static void classify_region(const char *name, char *out, int out_len)
{
  const char *p = name;

  lstrcpynA(out, "Unknown", out_len);

  while ((p = strchr(p, '(')) != NULL)
  {
    const char *end = strchr(p, ')');
    char group[40];
    int n, ok = 1, i, start;

    if (!end) break;
    n = (int)(end - p - 1);
    if (n < 1 || n >= (int)sizeof(group)) { p = end + 1; continue; }
    memcpy(group, p + 1, (size_t)n);
    group[n] = '\0';
    p = end + 1;

    /* Long form: every comma-separated word is a region name. */
    start = 0;
    for (i = 0; i <= n && ok; i++)
    {
      if (group[i] == ',' || group[i] == '\0')
      {
        int s = start, e = i;
        while (s < e && group[s] == ' ') s++;
        while (e > s && group[e - 1] == ' ') e--;
        if (e <= s || !is_region_word(group + s, e - s)) ok = 0;
        start = i + 1;
      }
    }
    if (ok) { lstrcpynA(out, group, out_len); return; }

    /* Short form: one to three letters, each a region. */
    if (n <= 3)
    {
      char text[48] = "";   /* (all six names with their separators are 40 characters) */
      for (i = 0; i < n; i++)
      {
        const char *w = NULL;
        switch (group[i])
        {
          case 'U': w = "USA"; break;
          case 'E': w = "Europe"; break;
          case 'J': w = "Japan"; break;
          case 'W': w = "World"; break;
          case 'K': w = "Korea"; break;
          case 'B': w = "Brazil"; break;
        }
        if (!w) { text[0] = '\0'; break; }
        if (text[0]) lstrcatA(text, ", ");
        lstrcatA(text, w);
      }
      if (text[0]) { lstrcpynA(out, text, out_len); return; }
    }
  }
}

/****************************************************************************
 * Scanning
 *
 * Bounded two ways: a depth cap so a directory junction loop can't recurse
 * forever, and a total entry cap so a huge tree can't hang the app. Both
 * are generous for how people actually organize a ROM collection.
 ****************************************************************************/

/****************************************************************************
 * Genres
 *
 * Which genre a game has comes from a small table built into the program
 * (genres.bin: the genre lists of the libretro database for the Mega Drive,
 * Master System, Game Gear and SG-1000), looked up by the game's title: the
 * file name up to its first "(" or "[", without the extension, "The", spaces
 * or punctuation, in lower case. A game that is not in it has no genre.
 ****************************************************************************/

static char  *genre_text;          /* the decompressed table, kept for the life of the program */
static const char *genre_name[40];
static int    genre_names;
typedef struct { const char *key; unsigned char g; } genre_row;
static genre_row *genre_rows;
static int    genre_nrows;

static void genres_load(void)
{
  HRSRC res;
  const unsigned char *src;
  uLongf len;
  char *p, *line;
  int cap = 0;

  if (genre_text) return;
  res = FindResourceA(NULL, MAKEINTRESOURCEA(IDR_GENRES), (LPCSTR)RT_RCDATA);
  if (!res) return;
  src = (const unsigned char *)LockResource(LoadResource(NULL, res));
  if (!src) return;
  len = (uLongf)*(const int *)src;
  genre_text = (char *)malloc((size_t)len + 1);
  if (!genre_text) return;
  if (uncompress((Bytef *)genre_text, &len, src + 4, SizeofResource(NULL, res) - 4) != Z_OK) { free(genre_text); genre_text = NULL; return; }
  genre_text[len] = '\0';

  /* line 1: the genre names, then one "key<TAB>number" line per game, sorted by key */
  p = strchr(genre_text, '\n');
  if (!p) return;
  *p++ = '\0';
  for (line = genre_text; line && genre_names < 40; )
  {
    char *bar = strchr(line, '|');
    genre_name[genre_names++] = line;
    if (!bar) break;
    *bar = '\0';
    line = bar + 1;
  }
  for (line = p; *line; )
  {
    char *eol = strchr(line, '\n'), *tab = strchr(line, '\t');
    if (!eol) break;
    *eol = '\0';
    if (tab)
    {
      *tab = '\0';
      if (genre_nrows >= cap)
      {
        cap = cap ? cap * 2 : 2048;
        genre_row *grown = (genre_row *)realloc(genre_rows, sizeof(genre_row) * (size_t)cap);
        if (!grown) { free(genre_rows); genre_rows = NULL; genre_nrows = 0; return; }       /* not a leak of the old list */
        genre_rows = grown;
      }
      genre_rows[genre_nrows].key = line;
      genre_rows[genre_nrows].g = (unsigned char)atoi(tab + 1);
      genre_nrows++;
    }
    line = eol + 1;
  }
}

static int genre_cmp(const void *a, const void *b)
{
  return strcmp((const char *)a, ((const genre_row *)b)->key);
}

/* The lookup key of a file name: see above. */
static void genre_key(const char *name, char *out, int out_len)
{
  char t[128];
  int n, cut, i, o = 0;
  const char *dot;

  lstrcpynA(t, name, sizeof(t));
  dot = strrchr(t, '.');
  if (dot && lstrlenA(dot) <= 5) t[dot - t] = '\0';
  cut = lstrlenA(t);
  for (i = 0; t[i]; i++)
    if ((t[i] == ' ' && (t[i + 1] == '(' || t[i + 1] == '[')) || (i == 0 && (t[0] == '(' || t[0] == '['))) { cut = i; break; }
  t[cut] = '\0';
  n = lstrlenA(t);
  while (n > 0 && t[n - 1] == ' ') t[--n] = '\0';
  for (i = 0; i < n; i++) if (t[i] >= 'A' && t[i] <= 'Z') t[i] = (char)(t[i] + 32);
  if (n >= 5 && !strcmp(t + n - 5, ", the")) t[n - 5] = '\0';
  else if (!strncmp(t, "the ", 4)) memmove(t, t + 4, (size_t)(n - 4 + 1));
  for (i = 0; t[i] && o < out_len - 1; i++)
    if ((t[i] >= 'a' && t[i] <= 'z') || (t[i] >= '0' && t[i] <= '9')) out[o++] = t[i];
  out[o] = '\0';
}

static void classify_genre(const char *name, char *out, int out_len)
{
  char key[128];
  const genre_row *r;

  out[0] = '\0';
  genres_load();
  if (!genre_rows || !genre_nrows) return;
  genre_key(name, key, sizeof(key));
  if (!key[0]) return;
  r = (const genre_row *)bsearch(key, genre_rows, (size_t)genre_nrows, sizeof(genre_row), genre_cmp);
  if (r && r->g < genre_names) lstrcpynA(out, genre_name[r->g], out_len);
}

/* Is this CD image a Sega CD one? The first sector tells: it starts with "SEGADISCSYSTEM" or
   "SEGABOOTDISC", right at the start (2048-byte sectors) or after the 16 / 24 byte sector header of a
   raw (2352-byte) image. Another Sega name that is not a Saturn / Dreamcast one is let through too, so
   an unusual Sega CD image is not lost. is_disc: the file is certainly a disc image (.cue sheet's file,
   .iso); a .bin that is not a raw disc image at all is a cartridge ROM and is left alone.
   Returns 0 when it is a disc of something else (Saturn, PlayStation, PC Engine CD ...); an unreadable
   file is kept. */
static int disc_buf_ok(const unsigned char *b, size_t got, int is_disc)
{
  static const unsigned char sync[12] = { 0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x00 };
  int off, raw;

  if (got < 40) return 1;

  raw = !memcmp(b, sync, 12);
  if (!raw && !is_disc) return 1;                     /* a cartridge ROM */

  for (off = 0; off <= 24; off += (off == 0 ? 16 : 8))
  {
    if (!memcmp(b + off, "SEGADISCSYSTEM", 14) || !memcmp(b + off, "SEGABOOTDISC", 12)) return 1;
    if (!memcmp(b + off, "SEGA", 4) && memcmp(b + off, "SEGA SEGASATURN", 15) && memcmp(b + off, "SEGA SEGAKATANA", 15)) return 1;
  }
  return 0;
}

static int disc_header_ok(const char *file, int is_disc)
{
  unsigned char b[48];
  FILE *fp = fopen(file, "rb");
  size_t got;

  if (!fp) return 1;
  got = fread(b, 1, sizeof(b), fp);
  fclose(fp);
  return disc_buf_ok(b, got, is_disc);
}

#ifdef USE_LIBCHDR
/* The same check for a .chd: its first sector, read out of the first hunk. A CHD that cannot be opened
   here (it needs a parent file, say) is kept. */
static int chd_disc_ok(const char *file)
{
  chd_file *chd = NULL;
  const chd_header *head;
  unsigned char *hunk;
  int ok = 1;

  if (chd_open(file, CHD_OPEN_READ, NULL, &chd) != CHDERR_NONE || !chd) return 1;
  head = chd_get_header(chd);
  if (head && head->hunkbytes >= 64 && head->hunkbytes <= (1u << 22))
  {
    hunk = (unsigned char *)malloc(head->hunkbytes);
    if (hunk)
    {
      if (chd_read(chd, 0, hunk) == CHDERR_NONE) ok = disc_buf_ok(hunk, head->hunkbytes, 1);
      free(hunk);
    }
  }
  chd_close(chd);
  return ok;
}
#endif

/* The nth (from 0) FILE of a .cue sheet, as a full path ("" when there is none). */
static void cue_file_n(const char *cue, int nth, char *out, int out_len)
{
  char line[GUI_PATH_LEN + 32];
  FILE *fp = fopen(cue, "rb");

  out[0] = '\0';
  if (!fp) return;
  while (fgets(line, sizeof(line), fp))
  {
    char *p = line, *q;

    while (*p == ' ' || *p == '\t') p++;
    if (_strnicmp(p, "FILE", 4)) continue;
    p += 4;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') { p++; q = strchr(p, '"'); }
    else { q = p; while (*q && *q != ' ' && *q != '\t' && *q != '\r' && *q != '\n') q++; }
    if (!q || q == p) break;
    *q = '\0';
    if (nth-- > 0) continue;
    if (p[0] && p[1] != ':' && p[0] != '\\')
    {
      char dir[GUI_PATH_LEN];
      char *slash;
      lstrcpynA(dir, cue, sizeof(dir));
      slash = strrchr(dir, '\\');
      if (slash) { slash[1] = '\0'; gui_snprintf(out, out_len, "%s%s", dir, p); }
      else lstrcpynA(out, p, out_len);
    }
    else lstrcpynA(out, p, out_len);
    break;
  }
  fclose(fp);
}

#define CUE_REFS_MAX 256

/* Every file the .cue sheets of a folder name (the tracks of a disc: they are not games of their own). */
static char (*cue_refs_collect(const char *dir, int *count))[GUI_PATH_LEN]
{
  char (*refs)[GUI_PATH_LEN] = (char (*)[GUI_PATH_LEN])malloc((size_t)CUE_REFS_MAX * GUI_PATH_LEN);
  WIN32_FIND_DATAA fd;
  HANDLE h;
  char pattern[GUI_PATH_LEN];

  *count = 0;
  if (!refs) return NULL;
  gui_snprintf(pattern, (int)sizeof(pattern), "%s\\*.cue", dir);
  h = FindFirstFileA(pattern, &fd);
  if (h == INVALID_HANDLE_VALUE) return refs;
  do
  {
    char cue[GUI_PATH_LEN];
    int k;

    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    gui_snprintf(cue, (int)sizeof(cue), "%s\\%s", dir, fd.cFileName);
    for (k = 0; k < 99 && *count < CUE_REFS_MAX; k++)
    {
      cue_file_n(cue, k, refs[*count], GUI_PATH_LEN);
      if (!refs[*count][0]) break;
      (*count)++;
    }
  }
  while (FindNextFileA(h, &fd) && *count < CUE_REFS_MAX);
  FindClose(h);
  return refs;
}

/* A folder (or something inside one) the person left out of the scan. */
static int path_under(const char *path, const char *base)
{
  size_t n = strlen(base);
  while (n > 0 && base[n - 1] == '\\') n--;
  return n > 0 && !_strnicmp(path, base, n) && (path[n] == '\0' || path[n] == '\\');
}

static int dir_excluded(const char *dir)
{
  int k;
  for (k = 0; k < gui.rom_exclude_n; k++)
    if (gui.rom_exclude[k][0] && path_under(dir, gui.rom_exclude[k])) return 1;
  return 0;
}

static void scan_dir(const char *dir, const char *rel, int depth)
{
  WIN32_FIND_DATAA fd;
  HANDLE h;
  char pattern[GUI_PATH_LEN];
  char (*cue_refs)[GUI_PATH_LEN] = NULL;
  int cue_ref_n = 0;

  if (depth > BROWSER_MAX_DEPTH) return;
  if (dir_excluded(dir)) return;
  if (entry_count >= BROWSER_MAX_ENTRIES) return;

  gui_snprintf(pattern, (int)sizeof(pattern), "%s\\*", dir);
  h = FindFirstFileA(pattern, &fd);
  if (h == INVALID_HANDLE_VALUE) return;

  cue_refs = cue_refs_collect(dir, &cue_ref_n);

  do
  {
    char full[GUI_PATH_LEN];

    if (entry_count >= BROWSER_MAX_ENTRIES) break;
    if (!lstrcmpA(fd.cFileName, ".") || !lstrcmpA(fd.cFileName, "..")) continue;

    gui_snprintf(full, (int)sizeof(full), "%s\\%s", dir, fd.cFileName);

    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    {
      char newrel[GUI_PATH_LEN];

      if (rel[0]) gui_snprintf(newrel, (int)sizeof(newrel), "%s\\%s", rel, fd.cFileName);
      else lstrcpynA(newrel, fd.cFileName, sizeof(newrel));

      if (gui.scan_subfolders) scan_dir(full, newrel, depth + 1);
    }
    else
    {
      const char *dot = strrchr(fd.cFileName, '.');
      const char *console;
      int plain = 0;                                  /* a file that is a game itself, not an archive */
      int pending = 0;                                /* a Mega Drive file that may be a Pico game: its header is read later, in the background */
      browser_entry_t *e;

      if (!dot || !dot[1]) continue;

      console = classify_extension(dot + 1);
      if (!console) continue;
      if (!lstrcmpiA(console, "Archive"))
      {
        const char *known = cached_archive_console(full, &fd);
        console = known ? known : classify_archive(full, 0);
        if (!known && is_md_label(console)) pending = 1;
        if (!lstrcmpiA(console, "Archive")) continue;   /* nothing in it is a game this program can run: not listed */
      }
      else plain = 1;

      /* A track file of a disc (named by a .cue of the folder, or "(Track 01)") is not a game: the .cue is. */
      if (!lstrcmpiA(dot, ".bin") || !lstrcmpiA(dot, ".iso") || !lstrcmpiA(dot, ".wav"))
      {
        int k, is_track = strstr(fd.cFileName, "(Track ") != NULL;
        for (k = 0; cue_refs && k < cue_ref_n && !is_track; k++)
          if (!lstrcmpiA(cue_refs[k], full)) is_track = 1;
        if (is_track) continue;
      }

      /* Discs of other systems (Saturn, PlayStation ...) use the same .cue / .bin / .iso files.
         A file the last scan listed and that has not changed since needs no second look (a .chd is
         opened and partly decoded for it, which adds up over a big library). */
      if (cached_archive_console(full, &fd)) { }
      else if (!lstrcmpiA(dot, ".cue"))
      {
        char first[GUI_PATH_LEN];
        cue_file_n(full, 0, first, (int)sizeof(first));
        if (first[0] && !disc_header_ok(first, 1)) continue;
      }
#ifdef USE_LIBCHDR
      else if (!lstrcmpiA(dot, ".chd"))
      {
        if (!chd_disc_ok(full)) continue;
      }
#endif
      else if (!lstrcmpiA(dot, ".iso") || !lstrcmpiA(dot, ".bin"))
      {
        if (!disc_header_ok(full, lstrcmpiA(dot, ".iso") == 0)) continue;
      }

      /* A standalone .bin belongs to a .cue in the same folder if one
         exists -- show the .cue only, since that's the actual entry point. */
      if (!lstrcmpiA(dot, ".bin"))
      {
        char cue_path[GUI_PATH_LEN];
        char base[128];
        int base_len = (int)(dot - fd.cFileName);

        if (base_len >= (int)sizeof(base)) base_len = sizeof(base) - 1;
        memcpy(base, fd.cFileName, (size_t)base_len);
        base[base_len] = '\0';

        gui_snprintf(cue_path, (int)sizeof(cue_path), "%s\\%s.cue", dir, base);
        if (GetFileAttributesA(cue_path) != INVALID_FILE_ATTRIBUTES) continue;
      }

      if (plain && is_md_label(console))      /* an unchanged file keeps the label of the last scan, the others are looked at after the list is shown */
      {
        const char *known = cached_archive_console(full, &fd);
        if (known) console = known; else pending = 1;
      }

      e = &entries[entry_count++];
      lstrcpynA(e->full_path, full, sizeof(e->full_path));
      lstrcpynA(e->name, fd.cFileName, sizeof(e->name));
      lstrcpynA(e->console, console, sizeof(e->console));
      e->size_bytes = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
      e->mtime = fd.ftLastWriteTime;
      e->pico_checked = pending ? 0 : 1;
      e->cover_ext = COVERART_UNCHECKED;
      classify_region(fd.cFileName, e->region, (int)sizeof(e->region));
      classify_genre(fd.cFileName, e->genre, (int)sizeof(e->genre));
      e->img_slot = -1;
      e->img_gen = 0;
      e->outside = 0;

      /* A big library takes a while the first time: show that it is working. */
      if ((entry_count & 63) == 0)
      {
        char progress[48];
        wsprintfA(progress, "Scanning... %d", entry_count);
        gui_status_slot(progress);
        if (g_status) UpdateWindow(g_status);
      }
    }
  }
  while (FindNextFileA(h, &fd) && entry_count < BROWSER_MAX_ENTRIES);

  FindClose(h);
  free(cue_refs);
}

/****************************************************************************
 * Folder picker
 ****************************************************************************/

static int pick_folder(HWND parent, char *out, int out_len)
{
  BROWSEINFOA bi;
  LPITEMIDLIST pidl;
  int ok = 0;

  ZeroMemory(&bi, sizeof(bi));
  bi.hwndOwner = parent;
  bi.lpszTitle = "Choose a folder to scan for ROMs";
  bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

  {
    int c = gui_cursor_show();   /* visible even if fullscreen has hidden it */
    pidl = SHBrowseForFolderA(&bi);
    gui_cursor_restore(c);
  }
  if (pidl)
  {
    char path[MAX_PATH];
    if (SHGetPathFromIDListA(pidl, path)) { lstrcpynA(out, path, out_len); ok = 1; }
    CoTaskMemFree(pidl);
  }

  return ok;
}

/****************************************************************************
 * List population and filtering
 ****************************************************************************/

static int search_shown(void)
{
  return search_override >= 0 ? search_override : !gui.hide_searchbar;
}

/* The filter bar: what the scanned games really have, one drop-down list
   each. The lists point into entries[] and are rebuilt after every scan; the
   choice is remembered by its text, so it survives a rescan. */
#define FILTER_MAX 64
static const char *f_list[3][FILTER_MAX];
static int  f_count[3];
static char sel_con[40];
static char sel_reg[24];
static char sel_fol[28];              /* the genre chosen (the second drop-down) */
static int  sel_fol_on;               /* sel_fol is "" for an unknown genre, so it needs its own flag */

static int cmp_text(const void *a, const void *b)
{
  return lstrcmpiA(*(const char *const *)a, *(const char *const *)b);
}

static void filter_add(int which, const char *s)
{
  int i;
  for (i = 0; i < f_count[which]; i++)
    if (!lstrcmpiA(f_list[which][i], s)) return;
  if (f_count[which] < FILTER_MAX) f_list[which][f_count[which]++] = s;
}

static void fill_filters(void)
{
  static const char *const all_text[3] = { "All Consoles", "All Genres", "All Regions" };
  int i, k;

  if (!panel_filter[0]) return;

  f_count[0] = f_count[1] = f_count[2] = 0;
  for (i = 0; i < entry_count; i++)
  {
    if (lstrcmpiA(entries[i].console, "Archive")) filter_add(0, entries[i].console);   /* an archive nothing was recognized in is not a console */
    filter_add(1, entries[i].genre);
    filter_add(2, entries[i].region);
  }
  for (k = 0; k < 3; k++) qsort(f_list[k], (size_t)f_count[k], sizeof(f_list[k][0]), cmp_text);

  for (k = 0; k < 3; k++)
  {
    int sel = 0;

    SendMessageA(panel_filter[k], CB_RESETCONTENT, 0, 0);
    SendMessageA(panel_filter[k], CB_ADDSTRING, 0, (LPARAM)all_text[k]);
    for (i = 0; i < f_count[k]; i++)
    {
      const char *s = f_list[k][i];
      SendMessageA(panel_filter[k], CB_ADDSTRING, 0, (LPARAM)((k == 1 && !s[0]) ? "(Unknown)" : s));

      if (k == 0 && sel_con[0] && !lstrcmpiA(s, sel_con)) sel = i + 1;
      if (k == 1 && sel_fol_on && !lstrcmpiA(s, sel_fol)) sel = i + 1;
      if (k == 2 && sel_reg[0] && !lstrcmpiA(s, sel_reg)) sel = i + 1;
    }
    SendMessageA(panel_filter[k], CB_SETCURSEL, (WPARAM)sel, 0);

    if (!sel && entry_count > 0)   /* what was chosen is gone from the folder (an empty list says nothing) */
    {
      if (k == 0) sel_con[0] = '\0';
      if (k == 1) { sel_fol[0] = '\0'; sel_fol_on = 0; }
      if (k == 2) sel_reg[0] = '\0';
    }
  }
}

/* What the filter bar shows is kept in the settings, for the next run. */
static void filters_to_settings(void)
{
  lstrcpynA(gui.filter_console, sel_con, sizeof(gui.filter_console));
  lstrcpynA(gui.filter_genre, sel_fol, sizeof(gui.filter_genre));
  gui.filter_genre_on = sel_fol_on;
  lstrcpynA(gui.filter_region, sel_reg, sizeof(gui.filter_region));
}

static void filter_chosen(int k)
{
  int idx = (int)SendMessageA(panel_filter[k], CB_GETCURSEL, 0, 0);
  const char *s = (idx > 0 && idx <= f_count[k]) ? f_list[k][idx - 1] : NULL;

  if (k == 0) lstrcpynA(sel_con, s ? s : "", sizeof(sel_con));
  if (k == 1) { lstrcpynA(sel_fol, s ? s : "", sizeof(sel_fol)); sel_fol_on = (s != NULL); }
  if (k == 2) lstrcpynA(sel_reg, s ? s : "", sizeof(sel_reg));
  filters_to_settings();
}

static void filters_reset(void)
{
  int k;
  sel_con[0] = sel_reg[0] = sel_fol[0] = '\0';
  sel_fol_on = 0;
  for (k = 0; k < 3; k++) SendMessageA(panel_filter[k], CB_SETCURSEL, 0, 0);
  filters_to_settings();
}

static void update_status_count(void)
{
  char count_text[64];

  int lib = entry_count - outside_n;      /* the games of the library, not those only in Recently Played */
  int shown = filtered_count - heading_rows - outside_shown - (recent_dup ? recent_group_n - outside_shown : 0);    /* (a game in both groups counts once) */

  if (!panel_visible) return;

  if (entry_count >= BROWSER_MAX_ENTRIES)
  {
    wsprintfA(count_text, "%d+ games (capped)", lib);
  }
  else if (shown != lib)
  {
    wsprintfA(count_text, "%d of %d game%s", shown, lib,
              (lib == 1) ? "" : "s");
  }
  else
  {
    wsprintfA(count_text, "%d game%s found", lib, (lib == 1) ? "" : "s");
  }

  gui_status_slot(count_text);
}

/* Compares two entries (by index into entries[]) on whichever column and
   direction is currently active -- used to sort filtered_index[] after
   filtering, before any rows are actually inserted, so rows always go
   into the ListView already in the right order rather than needing a
   second re-sort pass afterward. */
/* The file's extension without the dot ("" when it has none). */
static const char *file_ext(const char *path)
{
  const char *dot = strrchr(path, '.'), *slash = strrchr(path, '\\');
  return (dot && (!slash || dot > slash)) ? dot + 1 : "";
}

/* What a game is called in the list: the display name it was given, else its file name. */
static const char *entry_title(const browser_entry_t *e)
{
  return e->name;
}

static int group_sort;       /* the list is grouped by console: sort by that first */

static int compare_by_column(int sort_col, const browser_entry_t *ea, const browser_entry_t *eb)
{
  int result;

  switch (sort_col)
  {
    case 1:  result = lstrcmpiA(ea->console, eb->console); break;
    case COL_FAV: result = (int)eb->fav - (int)ea->fav; break;   /* favorites first when ascending */
    case COL_GENRE: result = lstrcmpiA(ea->genre, eb->genre); break;
    case COL_REGION: result = lstrcmpiA(ea->region, eb->region); break;
    case COL_SIZE: result = (ea->size_bytes > eb->size_bytes) - (ea->size_bytes < eb->size_bytes); break;
    case COL_TYPE: result = lstrcmpiA(file_ext(ea->full_path), file_ext(eb->full_path)); break;
    case COL_RECENT: result = (ea->last_played > eb->last_played) - (ea->last_played < eb->last_played); break;
    default: result = lstrcmpiA(entry_title(ea), entry_title(eb)); break;
  }
  return result;
}

static int compare_entries(const void *a, const void *b)
{
  const browser_entry_t *ea = &entries[*(const int *)a];
  const browser_entry_t *eb = &entries[*(const int *)b];
  int result, k;

  if (group_sort)
  {
    result = lstrcmpiA(ea->console, eb->console);
    if (result) return result;
  }

  /* The main sort column, then the 2nd and 3rd (Shift+click on a header), each in its own direction. */
  result = compare_by_column(gui.browser_sort_column, ea, eb);
  if (result) return gui.browser_sort_ascending ? result : -result;
  for (k = 0; k < 2; k++)
  {
    if (gui.browser_sort_extra[k] < 0) break;
    result = compare_by_column(gui.browser_sort_extra[k], ea, eb);
    if (result) return gui.browser_sort_extra_asc[k] ? result : -result;
  }

  /* Ties broken by name, ascending, regardless of the sort's own
     direction -- otherwise every console/folder/size tie (there are a
     lot of these; most ROMs share a folder) would fall back to
     whatever order they happened to be discovered in, undoing the
     point of sorting by that column at all. */
  return lstrcmpiA(entry_title(ea), entry_title(eb));
}

#define BROWSER_ICON_SIZE_MIN  48
#define BROWSER_ICON_SIZE_MAX  256
#define BROWSER_ICON_SIZE_STEP 32

static HIMAGELIST grid_imagelist;

/* Used for any entry with no cover art on disk -- a plain, theme-aware
   square rather than requiring a bundled placeholder resource file.
   Kept deliberately plain (no text, no icon glyph) since at small grid
   sizes anything more detailed just turns to noise. */
static HBITMAP make_placeholder_bitmap(int size)
{
  BITMAPINFO bmi;
  HBITMAP hbmp;
  unsigned char *bits;
  int dark = theme_is_dark();
  unsigned char fill = dark ? 70 : 190;
  unsigned char border = dark ? 110 : 140;
  int x, y;

  ZeroMemory(&bmi, sizeof(bmi));
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = size;
  bmi.bmiHeader.biHeight = -size;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  hbmp = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
  if (!hbmp) return NULL;

  {
    int fx, fy, fw, fh;

    coverart_frame_rect(size, &fx, &fy, &fw, &fh);
    ZeroMemory(bits, (size_t)size * size * 4);
    for (y = fy; y < fy + fh; y++)
    {
      for (x = fx; x < fx + fw; x++)
      {
        unsigned char *px = bits + ((size_t)y * size + x) * 4;
        int on_border = (x < fx + 2 || y < fy + 2 || x >= fx + fw - 2 || y >= fy + fh - 2);
        unsigned char shade = on_border ? border : fill;

        px[0] = shade;
        px[1] = shade;
        px[2] = shade;
        px[3] = 255;
      }
    }
    coverart_round_corners(bits, size, fx, fy, fw, fh);
  }

  return hbmp;
}

/* Grid view fills the image list lazily: an entry's picture is only read from
   disk and decoded when the list view first draws that entry (LVN_GETDISPINFO),
   not for every game in the library each time the list changes. Slot 0 of the
   list is the plain placeholder used for games without a cover. grid_gen
   changes whenever the list is rebuilt (zoom, rescan, cover changed), which
   makes every entry's remembered slot stale at once. */
static int grid_gen;
static int grid_placeholder_index;
static int grid_blank_index;           /* a see-through image: what a game shows while its cover is on its way */

/* Reading a cover from disk and decoding it (a large PNG takes tens of milliseconds) used to happen right in the
   drawing of the list, so scrolling a grid of covers stuttered. A thread of its own does that now: the grid shows the
   plain placeholder at once, and each cover pops in when it is ready. The thread never touches the list of games: a
   job carries copies of what it needs, and the result is matched back to its game by path on the UI thread. */
#define WM_COVER_READY   (WM_APP + 52)
#define COVER_PENDING    (-2)              /* img_slot: asked for, not here yet (the placeholder shows meanwhile) */
#define COVER_QUEUE      512

typedef struct { char path[GUI_PATH_LEN]; char console[40]; int size, gen; signed char ext; } cover_job;
typedef struct { char path[GUI_PATH_LEN]; HBITMAP bmp; int gen; signed char ext; } cover_done;

static CRITICAL_SECTION cover_cs;
static HANDLE cover_thread, cover_ev;
static cover_job  *cover_jobs;             /* a stack: the game asked for last is the one on screen now */
static cover_done *cover_results;
static int cover_n_jobs, cover_n_done;
static volatile LONG cover_gen_now, cover_posted, cover_quit;
static volatile DWORD cover_busy_since;      /* when the picture being read was started: 0 = idle */
static HANDLE cover_watch;

static DWORD WINAPI cover_thread_proc(LPVOID arg)
{
  (void)arg;
  for (;;)
  {
    WaitForSingleObject(cover_ev, INFINITE);
    for (;;)
    {
      cover_job job;
      cover_done d;

      if (cover_quit) return 0;

      EnterCriticalSection(&cover_cs);
      if (cover_n_jobs == 0) { ResetEvent(cover_ev); LeaveCriticalSection(&cover_cs); break; }
      job = cover_jobs[--cover_n_jobs];
      LeaveCriticalSection(&cover_cs);

      if (job.gen != cover_gen_now) continue;       /* the grid was rebuilt meanwhile: nobody wants this one */

      lstrcpyA(d.path, job.path);
      d.gen = job.gen;
      d.ext = job.ext;
      cover_busy_since = GetTickCount() | 1;
      d.bmp = coverart_load_cached(job.path, job.console, job.size, &d.ext);
      cover_busy_since = 0;

      EnterCriticalSection(&cover_cs);
      if (cover_n_done < COVER_QUEUE) cover_results[cover_n_done++] = d;
      else if (d.bmp) DeleteObject(d.bmp);
      LeaveCriticalSection(&cover_cs);

      if (panel_list && !InterlockedExchange(&cover_posted, 1)) PostMessage(panel_list, WM_COVER_READY, 0, 0);
    }
  }
  return 0;
}

/* A cover file on a network drive that does not answer would hold the cover thread (and so every cover behind it) for as long as
   Windows keeps waiting. This thread looks once a second and, when one picture has taken over five seconds, cancels that read:
   the game shows its plain tile and the thread goes on to the next. */
static DWORD WINAPI cover_watch_proc(LPVOID arg)
{
  (void)arg;
  while (!cover_quit)
  {
    DWORD since;
    Sleep(1000);
    since = cover_busy_since;
    if (since && GetTickCount() - since > 5000 && cover_thread) CancelSynchronousIo(cover_thread);
  }
  return 0;
}

/* At exit: both threads stopped, and the covers that were ready but never taken freed. */
void browser_covers_shutdown(void)
{
  int i;

  if (!cover_thread) return;
  cover_quit = 1;
  SetEvent(cover_ev);
  if (WaitForSingleObject(cover_thread, 700) != WAIT_OBJECT_0)
  {
    CancelSynchronousIo(cover_thread);                 /* stuck reading a file */
    WaitForSingleObject(cover_thread, 700);
  }
  if (cover_watch) { WaitForSingleObject(cover_watch, 1500); CloseHandle(cover_watch); cover_watch = NULL; }
  CloseHandle(cover_thread);
  cover_thread = NULL;
  for (i = 0; i < cover_n_done; i++) if (cover_results[i].bmp) DeleteObject(cover_results[i].bmp);
  cover_n_done = 0;
}

/* Asks for a game's cover. Returns 0 when the request could not be queued (it is asked again at the next draw). */
static int cover_request(const browser_entry_t *e)
{
  int ok = 0;

  if (!cover_thread)
  {
    coverart_prepare();
    { static int cs_ready; if (!cs_ready) { InitializeCriticalSection(&cover_cs); cs_ready = 1; } }
    free(cover_jobs); free(cover_results);
    if (cover_ev) CloseHandle(cover_ev);
    cover_jobs = (cover_job *)malloc(sizeof(cover_job) * COVER_QUEUE);
    cover_results = (cover_done *)malloc(sizeof(cover_done) * COVER_QUEUE);
    cover_ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (cover_jobs && cover_results && cover_ev) cover_thread = CreateThread(NULL, 0, cover_thread_proc, NULL, 0, NULL);
    if (!cover_thread) return 0;
    SetThreadPriority(cover_thread, THREAD_PRIORITY_BELOW_NORMAL);
    cover_watch = CreateThread(NULL, 0, cover_watch_proc, NULL, 0, NULL);
  }

  EnterCriticalSection(&cover_cs);
  if (cover_n_jobs < COVER_QUEUE)
  {
    cover_job *j = &cover_jobs[cover_n_jobs++];
    lstrcpynA(j->path, e->full_path, sizeof(j->path));
    lstrcpynA(j->console, e->console, sizeof(j->console));
    j->size = gui.browser_grid_size;
    j->gen = grid_gen;
    j->ext = e->cover_ext;
    SetEvent(cover_ev);
    ok = 1;
  }
  LeaveCriticalSection(&cover_cs);
  return ok;
}

/* Takes in the covers the thread has finished, and draws the list again. */
static void cover_collect(void)
{
  cover_done *got;
  int n, i, k;

  InterlockedExchange(&cover_posted, 0);
  if (!cover_thread) return;
  got = (cover_done *)malloc(sizeof(cover_done) * COVER_QUEUE);
  if (!got) return;

  EnterCriticalSection(&cover_cs);
  n = cover_n_done;
  memcpy(got, cover_results, sizeof(cover_done) * (size_t)n);
  cover_n_done = 0;
  LeaveCriticalSection(&cover_cs);

  for (i = 0; i < n; i++)
  {
    browser_entry_t *e = NULL;

    if (got[i].gen == grid_gen && grid_imagelist)
    {
      for (k = 0; k < entry_count; k++)
        if (!lstrcmpiA(entries[k].full_path, got[i].path)) { e = &entries[k]; break; }
    }
    if (e)
    {
      int idx = got[i].bmp ? ImageList_Add(grid_imagelist, got[i].bmp, NULL) : -1;
      e->cover_ext = got[i].ext;
      e->img_gen = grid_gen;
      e->img_slot = (idx >= 0) ? idx : grid_placeholder_index;
    }
    if (got[i].bmp) DeleteObject(got[i].bmp);
  }
  free(got);

  /* The image list only grows as covers come in. At the larger sizes that adds up (a 256 pixel cover is 256 KB): past about
     160 MB it is started again, and the covers on screen load again at once from the kept pictures. */
  if (grid_imagelist && (double)ImageList_GetImageCount(grid_imagelist) * gui.browser_grid_size * gui.browser_grid_size * 4.0 > 160.0 * 1024 * 1024)
  {
    browser_rebuild_grid_images();
    return;
  }
  if (n > 0 && panel_list) InvalidateRect(panel_list, NULL, FALSE);
}

/* More room between the rows of the list view, like the details view of Windows 11's file
   explorer: a list view makes its rows as tall as its small images, so a blank image of the
   wanted height is all it takes. */
static HIMAGELIST row_spacer;
static int row_spacer_h;

static void browser_apply_row_height(void)
{
  HDC dc;
  TEXTMETRICA tm;
  HFONT f, of;
  int h;

  if (!panel_list) return;
  dc = GetDC(panel_list);
  if (!dc) return;
  f = (HFONT)SendMessage(panel_list, WM_GETFONT, 0, 0);
  of = f ? (HFONT)SelectObject(dc, f) : NULL;
  GetTextMetricsA(dc, &tm);
  if (of) SelectObject(dc, of);
  ReleaseDC(panel_list, dc);

  {
    static const int mul[3] = { 14, 18, 24 }, least[3] = { 20, 24, 30 };   /* compact, normal, roomy */
    int d = gui.row_density < 0 || gui.row_density > 2 ? 1 : gui.row_density;
    h = tm.tmHeight * mul[d] / 10;
    if (h < DPIS(least[d])) h = DPIS(least[d]);
  }
  if (row_spacer && row_spacer_h == h) return;

  if (row_spacer) { ListView_SetImageList(panel_list, NULL, LVSIL_SMALL); ImageList_Destroy(row_spacer); }
  row_spacer = ImageList_Create(1, h, ILC_COLOR32, 1, 1);
  if (!row_spacer) { row_spacer_h = 0; return; }
  ImageList_SetImageCount(row_spacer, 1);
  row_spacer_h = h;
  ListView_SetImageList(panel_list, row_spacer, LVSIL_SMALL);
}

void browser_rebuild_grid_images(void)
{
  int size = gui.browser_grid_size;
  HBITMAP placeholder;

  if (!panel_list) return;

  if (grid_imagelist)
  {
    ListView_SetImageList(panel_list, NULL, LVSIL_NORMAL);
    ImageList_Destroy(grid_imagelist);
    grid_imagelist = NULL;
  }

  grid_imagelist = ImageList_Create(size, size, ILC_COLOR32 | ILC_MASK, 64, 64);
  if (!grid_imagelist) return;

  grid_placeholder_index = 0;
  placeholder = make_placeholder_bitmap(size);
  if (placeholder)
  {
    int idx = ImageList_Add(grid_imagelist, placeholder, NULL);
    grid_placeholder_index = (idx >= 0) ? idx : 0;
    DeleteObject(placeholder);
  }
  {
    BITMAPINFO bi;
    unsigned char *bits;
    HBITMAP blank;

    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    grid_blank_index = grid_placeholder_index;
    blank = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    if (blank)
    {
      int idx;
      ZeroMemory(bits, (size_t)size * size * 4);
      idx = ImageList_Add(grid_imagelist, blank, NULL);
      if (idx >= 0) grid_blank_index = idx;
      DeleteObject(blank);
    }
  }

  grid_gen++;
  InterlockedExchange(&cover_gen_now, grid_gen);

  ListView_SetImageList(panel_list, grid_imagelist, LVSIL_NORMAL);
  ListView_SetIconSpacing(panel_list, size + 24 + (gui.cover_card ? 8 : 0), size + 34 + (gui.cover_card ? 16 : 0));       /* a frame needs a little more room */
  browser_grid_check_cols();
  ListView_Arrange(panel_list, LVA_DEFAULT);
  RedrawWindow(panel_list, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

/* The image-list index to show for an entry, loading its cover the first time. */
static int cover_image_index(browser_entry_t *e)
{
  if (e->img_gen == grid_gen)
  {
    if (e->img_slot >= 0) return e->img_slot;
    if (e->img_slot == COVER_PENDING) return grid_blank_index;            /* on its way: nothing yet, so a frame and a gray tile do not show first */
  }

  if (cover_request(e))
  {
    e->img_gen = grid_gen;
    e->img_slot = COVER_PENDING;
    return grid_blank_index;
  }
  return grid_placeholder_index;
}

/* Refreshes just one entry's thumbnail after its cover was changed or
   removed via the context menu -- rebuilding the whole image list for
   a single-item change would be wasteful for a large library, but
   ImageList has no "replace in place" for a DIB source, so the
   practical equivalent is a full rebuild anyway; this at least skips
   it entirely when grid view isn't even active. */
static void browser_grid_refresh_cover(int item_index)
{
  /* The cover file for this entry just changed: forget what was remembered
     about it (including "no cover"), so it is looked up again. */
  if (row_entry(item_index)) row_entry(item_index)->cover_ext = COVERART_UNCHECKED;

  if (gui.browser_grid_view) browser_rebuild_grid_images();
}

/* Settings > General: the cover frame, rounded corners or caption changed. */
void browser_cover_style_changed(void)
{
  coverart_set_style(gui.cover_shape, gui.cover_round);
  coverart_set_cache(gui.cover_cache);
  if (!panel_list) return;
  SendMessage(panel_list, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_HIDELABELS, gui.cover_caption == 1 ? LVS_EX_HIDELABELS : 0);
  if (gui.browser_grid_view && grid_imagelist)
  {
    int k;
    for (k = 0; k < entry_count; k++) entries[k].img_gen = 0;
    browser_rebuild_grid_images();
  }
}

/* Switches the underlying ListView between report (list) and icon
   (grid) presentation. Both modes share the exact same items/columns
   underneath -- LVS_ICON just displays iImage/text instead of the
   detail columns, so no separate data structure is needed for grid
   view at all. */
void browser_apply_view_mode(void)
{
  if (!panel_list) return;

  ListView_SetView(panel_list, gui.browser_grid_view ? LV_VIEW_ICON : LV_VIEW_DETAILS);
  browser_cover_style_changed();
  browser_apply_row_height();
  if (gui.recent_mode || gui.group_console || gui.favorites_only) browser_apply_filter();   /* the headings are made for the view shown */

  if (gui.browser_grid_view)
  {
    browser_rebuild_grid_images();
  }
  else if (grid_imagelist)
  {
    ListView_SetImageList(panel_list, NULL, LVSIL_NORMAL);
    ImageList_Destroy(grid_imagelist);
    grid_imagelist = NULL;
  }
}

#define GRID_ZOOM_TIMER_ID 1

/* Ctrl+wheel while grid view is active: grow/shrink the thumbnail size
   by one step. delta follows the usual WM_MOUSEWHEEL sign convention
   (positive = away from the user = zoom in). Ignored entirely outside
   grid view, so the main window doesn't need to track view mode itself
   before deciding whether to forward wheel events here.

   The actual rebuild (icon spacing plus every thumbnail) is deferred
   to a debounced WM_TIMER rather than done on every single tick, so
   scrolling fast triggers it once after scrolling settles instead of
   once per tick. Everything about the visual change -- spacing and
   images together -- lands in that one rebuild rather than being
   split across ticks: an earlier version applied icon spacing
   immediately on each tick for a bit of extra responsiveness, but
   that only partially repositions items internally, and racing that
   partial layout against a manual repaint was what caused a stale
   ghost of the previous size to briefly show during rapid scrolling.
   With the rebuild itself now fast (well under 150ms even for a large
   library), that split isn't worth the risk anymore. See WM_TIMER in
   browser_list_subclass_proc. */
void browser_grid_zoom(int delta)
{
  int new_size;

  if (!gui.browser_grid_view) return;

  new_size = gui.browser_grid_size + ((delta > 0) ? BROWSER_ICON_SIZE_STEP : -BROWSER_ICON_SIZE_STEP);
  if (new_size < BROWSER_ICON_SIZE_MIN) new_size = BROWSER_ICON_SIZE_MIN;
  if (new_size > BROWSER_ICON_SIZE_MAX) new_size = BROWSER_ICON_SIZE_MAX;
  if (new_size == gui.browser_grid_size) return;

  gui.browser_grid_size = new_size;
  SetTimer(panel_list, GRID_ZOOM_TIMER_ID, 150, NULL);
}

/* The name for the list: with "Hide (...) and [...]" on, the tags in brackets are left out. */
static void display_name(const char *name, char *out, int out_len)
{
  int o = 0, depth = 0;
  const char *p, *end;

  if (!gui.clean_names) { lstrcpynA(out, name, out_len); return; }
  {
    const char *dot = strrchr(name, '.');
    end = (dot && dot > name && lstrlenA(dot) <= 6 && !strchr(dot, ' ')) ? dot : name + lstrlenA(name);   /* the extension goes too */
  }
  for (p = name; p < end && o < out_len - 1; p++)
  {
    if (*p == '(' || *p == '[') { depth++; continue; }
    if ((*p == ')' || *p == ']') && depth > 0) { depth--; continue; }
    if (depth) continue;
    if (*p == ' ' && (o == 0 || out[o - 1] == ' ')) continue;
    out[o++] = *p;
  }
  while (o > 0 && out[o - 1] == ' ') o--;
  out[o] = '\0';
  if (!out[0]) lstrcpynA(out, name, out_len);   /* nothing but tags: keep the name */
}

/* The same name for other lists (File > Recent Files). */
void browser_display_name(const char *name, char *out, int out_len)
{
  display_name(name, out, out_len);
}

/* Search words: "region:usa", "console:mega cd", "genre:action", "type:chd" and "fav:yes" narrow by that field; what is left is looked for in the name. */
enum { Q_REGION, Q_CONSOLE, Q_GENRE, Q_TYPE, Q_FAV, Q_N };
typedef struct { char text[128]; char val[Q_N][64]; } QUERY;

static int query_key(const char *s, int len)
{
  static const struct { const char *w; int k; } keys[] =
  {
    { "region", Q_REGION }, { "console", Q_CONSOLE }, { "system", Q_CONSOLE }, { "platform", Q_CONSOLE },
    { "genre", Q_GENRE }, { "type", Q_TYPE }, { "ext", Q_TYPE },
    { "fav", Q_FAV }, { "favorite", Q_FAV }
  };
  int i;

  for (i = 0; i < (int)(sizeof(keys) / sizeof(keys[0])); i++)
    if ((int)strlen(keys[i].w) == len && !_strnicmp(s, keys[i].w, (size_t)len)) return keys[i].k;
  return -1;
}

static void query_parse(const char *s, QUERY *q)
{
  int cur = -1, n = 0;
  const char *p = s;

  ZeroMemory(q, sizeof(*q));
  while (*p)
  {
    /* a key starts a word: letters, then a colon */
    if (p == s || p[-1] == ' ')
    {
      const char *e = p;
      while (isalpha((unsigned char)*e)) e++;
      if (e > p && *e == ':' && query_key(p, (int)(e - p)) >= 0)
      {
        while (cur >= 0 && n > 0 && q->val[cur][n - 1] == ' ') q->val[cur][--n] = '\0';
        cur = query_key(p, (int)(e - p));
        n = 0;
        q->val[cur][0] = '\0';
        p = e + 1;
        while (*p == ' ') p++;
        continue;
      }
    }
    if (cur < 0)
    {
      int l = (int)strlen(q->text);
      if (l < (int)sizeof(q->text) - 1) { q->text[l] = *p; q->text[l + 1] = '\0'; }
    }
    else if (n < (int)sizeof(q->val[0]) - 1)
    {
      q->val[cur][n++] = *p;
      q->val[cur][n] = '\0';
    }
    p++;
  }
  while (cur >= 0 && n > 0 && q->val[cur][n - 1] == ' ') q->val[cur][--n] = '\0';
  { int l = (int)strlen(q->text); while (l > 0 && q->text[l - 1] == ' ') q->text[--l] = '\0'; }
}

static int query_match(const QUERY *q, const browser_entry_t *e)
{
  if (q->text[0] && !contains_ci(e->name, q->text)) return 0;
  if (q->val[Q_REGION][0] && !contains_ci(e->region, q->val[Q_REGION])) return 0;
  if (q->val[Q_CONSOLE][0] && !contains_ci(e->console, q->val[Q_CONSOLE])) return 0;
  if (q->val[Q_GENRE][0] && !contains_ci(e->genre, q->val[Q_GENRE])) return 0;
  if (q->val[Q_TYPE][0])
  {
    const char *t = q->val[Q_TYPE];
    if (*t == '.') t++;
    if (lstrcmpiA(file_ext(e->full_path), t)) return 0;
  }
  if (q->val[Q_FAV][0])
  {
    int want = !(q->val[Q_FAV][0] == 'n' || q->val[Q_FAV][0] == 'N' || q->val[Q_FAV][0] == '0');
    if ((e->fav ? 1 : 0) != want) return 0;
  }
  return 1;
}

/* Grid view headings. A heading is an item of its own, followed by empty items up to the end of its row, and each part of
   the list ends with empty items too, so that the next heading starts a new row; the heading is drawn across the row
   (see heading_band_paint). How many covers a row holds comes from the width of the list and of a cell, and is checked
   against the real thing after the items are in. */
static int grid_retry;

static int grid_cols(void)
{
  RECT rc;
  int cx, c;

  if (grid_cols_force > 0) return grid_cols_force;
  GetClientRect(panel_list, &rc);
  cx = (int)LOWORD(ListView_GetItemSpacing(panel_list, FALSE));
  if (cx < 1) return 1;
  c = rc.right / cx;
  return c < 1 ? 1 : c;
}

/* How many items the first row really has: those that share the first one's top. */
static int grid_cols_actual(void)
{
  int n = ListView_GetItemCount(panel_list), k;
  POINT p0, p;

  if (n < 2 || !ListView_GetItemPosition(panel_list, 0, &p0)) return 0;
  for (k = 1; k < n && k < 200; k++)
  {
    if (!ListView_GetItemPosition(panel_list, k, &p)) return 0;
    if (p.y != p0.y) return k;
  }
  return n < 200 ? n : 0;
}

/* Puts the headings (and the empty items) into filtered_index for the grid. */
static void grid_headings(void)
{
  int cols, k, n = 0, secs = 0, s, cap;
  int *tmp;
  struct { int kind, from, to; } sec[26];

  if (filtered_count <= 0) return;
  if (gui.group_console)
  {
    for (k = 0; k < filtered_count; k++)
      if (k == 0 || lstrcmpiA(entries[filtered_index[k]].console, entries[filtered_index[k - 1]].console))
      {
        if (secs >= 24) return;                        /* too many parts: no headings */
        lstrcpynA(console_heads[secs], entries[filtered_index[k]].console, sizeof(console_heads[0]));
        sec[secs].kind = ROW_HEAD_CONSOLE - secs; sec[secs].from = k; sec[secs].to = filtered_count;
        if (secs) sec[secs - 1].to = k;
        secs++;
      }
    console_head_n = secs;
  }
  else if (recent_group_n > 0)
  {
    sec[0].kind = ROW_HEAD_RECENT; sec[0].from = 0; sec[0].to = recent_group_n;
    secs = 1;
    if (recent_group_n < filtered_count) { sec[1].kind = ROW_HEAD_ALL; sec[1].from = recent_group_n; sec[1].to = filtered_count; secs = 2; }
  }
  else if (gui.favorites_only && !searching)
  {
    sec[0].kind = ROW_HEAD_ALL; sec[0].from = 0; sec[0].to = filtered_count;      /* shown as "Favorites" */
    secs = 1;
  }
  if (!secs) return;

  cols = grid_cols();
  cap = filtered_count + secs * (2 * cols + 1);
  if (cap > BROWSER_MAX_ENTRIES) return;
  tmp = (int *)malloc(sizeof(int) * (size_t)cap);
  if (!tmp) return;
  for (s = 0; s < secs; s++)
  {
    int games = sec[s].to - sec[s].from, fill;
    tmp[n++] = sec[s].kind;
    for (k = 1; k < cols; k++) tmp[n++] = ROW_PAD;                         /* the rest of the heading's row */
    for (k = sec[s].from; k < sec[s].to; k++) tmp[n++] = filtered_index[k];
    fill = (cols - games % cols) % cols;
    for (k = 0; k < fill; k++) tmp[n++] = ROW_PAD;                         /* the rest of the last row */
  }
  memcpy(filtered_index, tmp, sizeof(int) * (size_t)n);
  heading_rows = n - filtered_count;
  filtered_count = n;
  grid_headed = 1;
  grid_built_cols = cols;
  free(tmp);
}

static void CALLBACK grid_reflow_timer(HWND hwnd, UINT msg, UINT_PTR id, DWORD now)
{
  KillTimer(hwnd, id);
  if (panel_list && gui.browser_grid_view && grid_headed && IsWindowVisible(panel_list))
  {
    int real = grid_cols_actual();
    if (real > 0 && real != grid_built_cols)      /* the grid wraps differently now: made again, and back to where it was */
    {
      int top = ListView_GetTopIndex(panel_list), keep = -1, r;
      POINT p;
      while (top >= 0 && top < filtered_count && filtered_index[top] < 0) top++;
      if (top >= 0 && top < filtered_count) keep = filtered_index[top];
      grid_cols_force = 0;
      browser_apply_filter();
      if (keep >= 0)
        for (r = 0; r < filtered_count; r++)
          if (filtered_index[r] == keep)
          {
            if (ListView_GetItemPosition(panel_list, r, &p) && p.y > 0) ListView_Scroll(panel_list, 0, p.y);
            break;
          }
    }
  }
  (void)msg; (void)now;
}

/* The grid's width or cell size changed: with headings, the rows are made again when the number of covers per row differs. */
void browser_grid_check_cols(void)
{
  if (!panel_list || !gui.browser_grid_view || !grid_headed) return;
  SetTimer(panel_list, 0x6A1D, 200, grid_reflow_timer);
}

static void browser_apply_filter(void)
{
  char search[128];
  QUERY query;
  int i, row;

  if (!panel_list) return;

  if (search_shown())
    GetWindowTextA(panel_search, search, sizeof(search));
  else
    search[0] = '\0';

  /* Typing in the search box searches every game: favorites only and the filter bar are set aside
     until the box is empty again. */
  searching = search[0] != '\0';
  query_parse(search, &query);

  filtered_count = 0;
  for (i = 0; i < entry_count; i++)
  {
    entries[i].fav = (signed char)config_fav_has(entries[i].full_path);
    entries[i].last_played = history_last(entries[i].full_path);
    if (gui.favorites_only && !searching && !entries[i].fav) continue;
    if (!query_match(&query, &entries[i])) continue;
    if (!gui.hide_filterbar && !searching)
    {
      if (sel_con[0] && lstrcmpiA(entries[i].console, sel_con)) continue;
      if (sel_reg[0] && lstrcmpiA(entries[i].region, sel_reg)) continue;
      if (sel_fol_on && lstrcmpiA(entries[i].genre, sel_fol)) continue;
    }
    filtered_index[filtered_count++] = i;
  }

  group_sort = gui.group_console;
  qsort(filtered_index, filtered_count, sizeof(filtered_index[0]), compare_entries);
  group_sort = 0;

  /* The last few games played move to the front, as a group of their own (not when the list is grouped by console). */
  recent_group_n = 0;
  recent_dup = 0;
  if (gui.recent_mode && filtered_count > 0 && !gui.group_console)
  {
    int *tmp = (int *)malloc(sizeof(int) * (size_t)(filtered_count + RECENT_GROUP_SIZE));
    if (tmp)
    {
      int n = 0, k, best;
      char *taken = (char *)calloc((size_t)filtered_count, 1);
      while (taken && n < RECENT_GROUP_SIZE)
      {
        best = -1;
        for (k = 0; k < filtered_count; k++)
          if (!taken[k] && entries[filtered_index[k]].last_played &&
              (best < 0 || entries[filtered_index[k]].last_played > entries[filtered_index[best]].last_played)) best = k;
        if (best < 0) break;
        taken[best] = 1;
        tmp[n++] = filtered_index[best];
      }
      if (taken && n > 0)
      {
        /* The group is a shortcut, not a move: the games stay in All Games too (when the list has room for the copies). */
        int rest = n, dup = filtered_count + n + 2 <= BROWSER_MAX_ENTRIES;
        for (k = 0; k < filtered_count; k++)
          if (dup || !taken[k]) tmp[rest++] = filtered_index[k];
        memcpy(filtered_index, tmp, sizeof(int) * (size_t)rest);
        filtered_count = rest;
        recent_group_n = n;
        recent_dup = dup;
      }
      free(taken);
      free(tmp);
    }
  }

  /* A game from outside the library folders is only for the Recently Played group (the first recent_group_n rows). */
  outside_shown = 0;
  {
    int k, keep = 0;
    for (k = 0; k < filtered_count; k++)
    {
      if (entries[filtered_index[k]].outside)
      {
        if (k >= recent_group_n) continue;
        outside_shown++;
      }
      filtered_index[keep++] = filtered_index[k];
    }
    filtered_count = keep;
  }

  heading_rows = 0;
  grid_headed = 0;
  /* In the list view the two parts get a heading row each. */
  console_head_n = 0;
  if (gui.browser_grid_view)
  {
    grid_headings();
  }
  else if (gui.group_console)
  {
    /* A heading row, in the style of Recently Played, before each console's games (list view only). */
    if (!gui.browser_grid_view && filtered_count > 0)
    {
      int *tmp = (int *)malloc(sizeof(int) * (size_t)(filtered_count + 24));
      int n = 0, k;
      if (tmp)
      {
        for (k = 0; k < filtered_count; k++)
        {
          if (k == 0 || lstrcmpiA(entries[filtered_index[k]].console, entries[filtered_index[k - 1]].console))
          {
            if (console_head_n < 24 && n + 1 + (filtered_count - k) <= BROWSER_MAX_ENTRIES)
            {
              lstrcpynA(console_heads[console_head_n], entries[filtered_index[k]].console, sizeof(console_heads[0]));
              tmp[n++] = ROW_HEAD_CONSOLE - console_head_n++;
            }
          }
          tmp[n++] = filtered_index[k];
        }
        memcpy(filtered_index, tmp, sizeof(int) * (size_t)n);
        filtered_count = n;
        heading_rows = console_head_n;
        free(tmp);
      }
    }
  }
  else if (recent_group_n > 0 && !gui.browser_grid_view && filtered_count + 2 <= BROWSER_MAX_ENTRIES)
  {
    memmove(filtered_index + recent_group_n + 2, filtered_index + recent_group_n, sizeof(int) * (size_t)(filtered_count - recent_group_n));
    memmove(filtered_index + 1, filtered_index, sizeof(int) * (size_t)recent_group_n);
    filtered_index[0] = ROW_HEAD_RECENT;
    filtered_index[recent_group_n + 1] = ROW_HEAD_ALL;
    filtered_count += 2;
    heading_rows = 2;
  }
  else if (gui.favorites_only && !searching && filtered_count > 0 && !gui.browser_grid_view && filtered_count + 1 <= BROWSER_MAX_ENTRIES)
  {
    memmove(filtered_index + 1, filtered_index, sizeof(int) * (size_t)filtered_count);
    filtered_index[0] = ROW_HEAD_ALL;       /* shown as "Favorites" */
    filtered_count += 1;
    heading_rows = 1;
  }

  SendMessage(panel_list, WM_SETREDRAW, FALSE, 0);
  SendMessage(panel_list, LVM_DELETEALLITEMS, 0, 0);

  for (row = 0; row < filtered_count; row++)
  {
    LVITEMA item;
    int idx = filtered_index[row];

    ZeroMemory(&item, sizeof(item));
    item.mask     = LVIF_TEXT | LVIF_IMAGE;
    item.iItem    = row;
    item.iSubItem = 0;
    item.iImage   = I_IMAGECALLBACK;    /* supplied on demand, see cover_image_index() */
    if (idx < 0)       /* a heading */
    {
      item.iImage = I_IMAGENONE;
      item.pszText = idx == ROW_PAD ? (char *)"" : (char *)heading_label(idx);
      SendMessageA(panel_list, LVM_INSERTITEMA, 0, (LPARAM)&item);
      continue;
    }
    {
      static char shown[160];
      display_name(entries[idx].name, shown, sizeof(shown));
      item.pszText = shown;
    }
    SendMessageA(panel_list, LVM_INSERTITEMA, 0, (LPARAM)&item);

    item.iSubItem = 1;
    item.pszText  = entries[idx].console;
    SendMessageA(panel_list, LVM_SETITEMTEXTA, (WPARAM)row, (LPARAM)&item);

    item.iSubItem = COL_GENRE;
    item.pszText  = entries[idx].genre;
    SendMessageA(panel_list, LVM_SETITEMTEXTA, (WPARAM)row, (LPARAM)&item);

    item.iSubItem = COL_REGION;
    item.pszText  = entries[idx].region;
    SendMessageA(panel_list, LVM_SETITEMTEXTA, (WPARAM)row, (LPARAM)&item);


    {
      char sz[32], ty[16];
      ULONGLONG kb64 = (entries[idx].size_bytes + 1023) / 1024;
      unsigned long kb = (unsigned long)kb64;
      if (kb64 >= 1024UL * 1024UL) gui_snprintf(sz, sizeof(sz), "%lu.%lu GB", (unsigned long)(kb64 / (1024UL * 1024UL)), (unsigned long)((kb64 % (1024UL * 1024UL)) * 10 / (1024UL * 1024UL)));
      else if (kb >= 1024) gui_snprintf(sz, sizeof(sz), "%lu.%lu MB", kb / 1024, (kb % 1024) * 10 / 1024);
      else gui_snprintf(sz, sizeof(sz), "%lu KB", kb);
      lstrcpynA(ty, file_ext(entries[idx].full_path), sizeof(ty));
      CharUpperA(ty);
      item.iSubItem = COL_SIZE;
      item.pszText  = sz;
      SendMessageA(panel_list, LVM_SETITEMTEXTA, (WPARAM)row, (LPARAM)&item);
      item.iSubItem = COL_TYPE;
      item.pszText  = ty;
      SendMessageA(panel_list, LVM_SETITEMTEXTA, (WPARAM)row, (LPARAM)&item);
    }
  }

  SendMessage(panel_list, WM_SETREDRAW, TRUE, 0);
  grid_hide_top_blank();
  InvalidateRect(panel_list, NULL, TRUE);

  /* The headings of the grid rely on how many covers a row holds; if the grid shows another number than was worked out
     (a scroll bar took some room), the rows are made again for the real one. */
  if (grid_headed && !grid_retry)
  {
    int real = grid_cols_actual();
    if (real > 0 && real != grid_built_cols)
    {
      grid_cols_force = real;
      grid_retry = 1;
      browser_apply_filter();
      grid_retry = 0;
      return;
    }
  }

  az_update();
  update_status_count();
  browser_toolbar_update();
}

/* A game opened from outside the library folders (the desktop, a download folder) is still one of the last played:
   the newest few history entries the scan did not find are added to the list while their file is still there. */
static void add_outside_recent(void)
{
  int n = history_count(), done;
  char *taken;

  if (n <= 0 || !entries) return;
  taken = (char *)calloc((size_t)n, 1);
  if (!taken) return;

  for (done = 0; done < RECENT_GROUP_SIZE; done++)
  {
    int i, k, best = -1, have = 0;
    const char *path, *dot, *console;
    WIN32_FIND_DATAA fd;
    HANDLE h;
    browser_entry_t *e;

    for (i = 0; i < n; i++)
      if (!taken[i] && history_entry_last(i) && (best < 0 || history_entry_last(i) > history_entry_last(best))) best = i;
    if (best < 0) break;
    taken[best] = 1;
    path = history_entry_path(best);

    for (k = 0; k < entry_count && !have; k++)
      if (!lstrcmpiA(entries[k].full_path, path)) have = 1;
    if (have || entry_count >= BROWSER_MAX_ENTRIES) continue;

    dot = strrchr(path, '.');
    if (!dot || !dot[1]) continue;
    console = classify_extension(dot + 1);
    if (!console) continue;
    h = FindFirstFileA(path, &fd);
    if (h == INVALID_HANDLE_VALUE) continue;      /* the file is gone: it stays out of the list */
    FindClose(h);
    if (!lstrcmpiA(console, "Archive"))
    {
      console = classify_archive(path, 1);
      if (!lstrcmpiA(console, "Archive")) continue;
    }
    else console = refine_pico_file(console, path);

    e = &entries[entry_count++];
    memset(e, 0, sizeof(*e));
    lstrcpynA(e->full_path, path, sizeof(e->full_path));
    lstrcpynA(e->name, fd.cFileName, sizeof(e->name));
    lstrcpynA(e->console, console, sizeof(e->console));
    e->pico_checked = 1;
    e->size_bytes = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    e->mtime = fd.ftLastWriteTime;
    e->cover_ext = COVERART_UNCHECKED;
    classify_region(fd.cFileName, e->region, (int)sizeof(e->region));
    classify_genre(fd.cFileName, e->genre, (int)sizeof(e->genre));
    e->img_slot = -1;
    e->outside = 1;
    outside_n++;
  }
  free(taken);
}

/* Which Mega Drive files are Sega Pico games is found out after the list is shown: opening every file would make a scan of a big library
   slow (a virus scanner looks at each file that is opened). A thread reads the headers of the files not looked at yet and hands back the
   ones that are Pico games; a newer scan makes its answer out of date. */
typedef struct { int index; char path[GUI_PATH_LEN]; signed char pico; } pico_job_t;
typedef struct { LONG gen; int n; pico_job_t job[1]; } pico_scan_t;
static volatile LONG pico_gen;

static DWORD WINAPI pico_scan_thread(LPVOID arg)
{
  pico_scan_t *ps = (pico_scan_t *)arg;
  int i;

  for (i = 0; i < ps->n && ps->gen == pico_gen; i++)
  {
    const char *dot = strrchr(ps->job[i].path, '.');
    int archive = dot && (!lstrcmpiA(dot, ".zip") || !lstrcmpiA(dot, ".gz") || !lstrcmpiA(dot, ".7z"));
    const char *label = archive ? classify_archive(ps->job[i].path, 1) : refine_pico_file("Mega Drive / Genesis", ps->job[i].path);

    ps->job[i].pico = lstrcmpA(label, PICO_LABEL) == 0 ? 1 : 0;
  }
  if (ps->gen != pico_gen || !g_hwnd || !PostMessage(g_hwnd, WM_APP_PICOSCAN, 0, (LPARAM)ps)) free(ps);
  return 0;
}

static void pico_scan_start(void)
{
  pico_scan_t *ps;
  HANDLE th;
  int i, n = 0;

  InterlockedIncrement(&pico_gen);                     /* what an earlier scan found no longer fits */
  for (i = 0; i < entry_count; i++) if (!entries[i].pico_checked) n++;
  if (!n) return;
  ps = (pico_scan_t *)malloc(sizeof(pico_scan_t) + (size_t)(n - 1) * sizeof(pico_job_t));
  if (!ps) return;
  ps->gen = pico_gen;
  ps->n = 0;
  for (i = 0; i < entry_count; i++)
    if (!entries[i].pico_checked)
    {
      ps->job[ps->n].index = i;
      lstrcpynA(ps->job[ps->n].path, entries[i].full_path, GUI_PATH_LEN);
      ps->job[ps->n].pico = 0;
      ps->n++;
    }
  th = CreateThread(NULL, 0, pico_scan_thread, ps, 0, NULL);
  if (th) CloseHandle(th); else free(ps);
}

/* The answer of the thread (WM_APP_PICOSCAN): the Pico games get their console, and the list and the console filter follow. */
void browser_pico_done(LPARAM lp)
{
  pico_scan_t *ps = (pico_scan_t *)lp;
  int i, found = 0;

  if (ps->gen == pico_gen)
    for (i = 0; i < ps->n; i++)
    {
      int k = ps->job[i].index;
      if (k >= entry_count || lstrcmpA(entries[k].full_path, ps->job[i].path)) continue;
      entries[k].pico_checked = 1;
      if (ps->job[i].pico) { lstrcpynA(entries[k].console, PICO_LABEL, sizeof(entries[k].console)); found++; }
    }
  free(ps);
  if (found && panel_list)
  {
    fill_filters();
    browser_apply_filter();
  }
}

static void browser_rescan(void)
{
  HCURSOR old_cursor;

  if (!entries || !spare) return;

  /* Keep the last scan to reuse its archive labels, and scan into the other
     buffer. */
  {
    browser_entry_t *t = spare;
    spare = entries;
    spare_count = entry_count;
    entries = t;
  }
  entry_count = 0;
  outside_n = 0;
  spare_hint = 0;

  if (gui.rom_dir[0])
  {
    /* The folders that make up the library: the ROM directory and the extra ones that are ticked. A folder
       inside another one that is also scanned (with subfolders) is not scanned twice. */
    const char *roots[8];
    int nroots = 0, r, s;

    old_cursor = SetCursor(LoadCursorA(NULL, IDC_WAIT));
    roots[nroots++] = gui.rom_dir;
    for (r = 0; r < gui.rom_extra_n; r++)
      if (gui.rom_extra[r][0]) roots[nroots++] = gui.rom_extra[r];
    for (r = 0; r < nroots; r++)
    {
      int skip = 0;
      for (s = 0; s < nroots && !skip; s++)
        if (s != r && (path_under(roots[r], roots[s]) && (gui.scan_subfolders || 0) && (strlen(roots[s]) < strlen(roots[r]) || s < r)))
          skip = 1;
      if (!skip) scan_dir(roots[r], "", 0);
    }
    SetCursor(old_cursor);
  }
  add_outside_recent();

  /* The entries are new objects: start the grid's images over. */
  if (gui.browser_grid_view) browser_rebuild_grid_images();

  fill_filters();
  browser_apply_filter();
  pico_scan_start();
}

void browser_panel_change_folder(void)
{
  char picked[GUI_PATH_LEN];
  if (pick_folder(g_hwnd, picked, sizeof(picked)))
  {
    lstrcpynA(gui.rom_dir, picked, sizeof(gui.rom_dir));
    config_save();
    browser_toolbar_update();

    if (panel_visible)
    {
      /* Switches from the Choose Folder button to the normal search+list
         view (or just re-lays-out the same view if a folder was already
         set and this came from the File menu instead of the button). */
      browser_relayout();
      browser_panel_show(1);
    }
  }
}

static void update_sort_arrow(void);

/* View > Game List > Show Recently Played. */
void browser_refilter(void)
{
  if (panel_list) browser_apply_filter();
}

void browser_toggle_recent(void)
{
  gui.recent_mode = !gui.recent_mode;
  browser_apply_filter();
  config_save();
}

/* View > Game List > List Spacing changed. */
void browser_density_changed(void)
{
  browser_apply_row_height();
  if (panel_list) { InvalidateRect(panel_list, NULL, TRUE); ListView_RedrawItems(panel_list, 0, ListView_GetItemCount(panel_list)); }
}

static void browser_launch_selected(void)
{
  int sel = ListView_GetNextItem(panel_list, -1, LVNI_SELECTED);
  char path[GUI_PATH_LEN];

  if (!row_entry(sel)) return;

  /* Copy before hiding: hiding the panel doesn't touch entries[], but
     there's no reason to rely on that not changing before emu_load_rom()
     actually reads it back out. */
  lstrcpynA(path, row_entry(sel)->full_path, sizeof(path));

  emu_load_rom(path);
}

/****************************************************************************
 * Column visibility
 ****************************************************************************/

static void apply_column_widths(void)
{
  int i;

  for (i = 0; i < BROWSER_COLUMNS; i++)
    SendMessage(panel_list, LVM_SETCOLUMNWIDTH, (WPARAM)i, (LPARAM)col_width(i));
}

/* Called when the settings are saved: the order the columns have been dragged into. */
void browser_capture_columns(void)
{
  int ord[BROWSER_COLUMNS], k;

  if (!panel_list) return;
  if (SendMessage(panel_list, LVM_GETCOLUMNORDERARRAY, BROWSER_COLUMNS, (LPARAM)ord))
    for (k = 0; k < BROWSER_COLUMNS; k++) gui.browser_col_order[k] = ord[k];
}

int browser_column_hidden(int col)
{
  return (col > 0 && col < BROWSER_COLUMNS) ? col_hidden[col] : 0;
}

/* Shows or hides one list column (Name, column 0, is always shown). */
void browser_toggle_column(int col)
{
  int i;

  if (col <= 0 || col >= BROWSER_COLUMNS) return;

  col_hidden[col] = !col_hidden[col];
  apply_column_widths();

  gui.browser_col_hidden = 0;
  gui.browser_col_extra = 0;
  for (i = 0; i < COL_FIRST_EXTRA; i++)
  {
    if (col_hidden[i]) gui.browser_col_hidden |= (1 << i);
  }
  for (i = COL_FIRST_EXTRA; i < BROWSER_COLUMNS; i++)
  {
    if (!col_hidden[i]) gui.browser_col_extra |= (1 << (i - COL_FIRST_EXTRA));
  }
  config_save();
}

static void show_column_menu(int screen_x, int screen_y)
{
  HMENU menu = CreatePopupMenu();
  int i, cmd;

  {
    const int *shown_order = col_order;
    int k;
    for (k = 0; k < BROWSER_COLUMNS; k++)
    {
      i = shown_order[k];
      UINT flags = MF_STRING | (col_hidden[i] ? MF_UNCHECKED : MF_CHECKED);
      if (i == 0) flags |= MF_GRAYED;   /* Name is the row's identity, always shown */
      AppendMenuA(menu, flags, (UINT_PTR)(2000 + i), col_names[i]);
    }
  }

  AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
  AppendMenuA(menu, MF_STRING, 2100, "Size All Columns to Fit");

  /* Documented Windows workaround (MSDN, TrackPopupMenu remarks): if the
     owning window isn't confirmed as the foreground window, the popup can
     fail to dismiss/behave correctly when an item is chosen. The trailing
     WM_NULL is the other half -- without it, some message-loop timing can
     still leave the menu visually stuck even though the selection itself
     went through. */
  SetForegroundWindow(g_hwnd);

  cmd = gui_popup(menu, TPM_RIGHTBUTTON, screen_x, screen_y);       /* (drawn like the other menus, the Large interface or not) */
  DestroyMenu(menu);

  PostMessage(g_hwnd, WM_NULL, 0, 0);

  if (cmd == 2100)
  {
    /* Every shown column gets the width its contents (or its title) need; the widths are kept like a hand-made resize. */
    int col;
    for (col = COL_FAV; col < BROWSER_COLUMNS; col++)
    {
      if (col_hidden[col]) continue;
      SendMessage(panel_list, LVM_SETCOLUMNWIDTH, (WPARAM)col, (LPARAM)LVSCW_AUTOSIZE_USEHEADER);
      { int w = (int)SendMessage(panel_list, LVM_GETCOLUMNWIDTH, (WPARAM)col, 0); gui.browser_col_w[col] = w > 20 ? w : 20; }
    }
    for (col = 0; col < COL_FAV; col++)
    {
      int w, pct;
      if (col_hidden[col]) continue;
      SendMessage(panel_list, LVM_SETCOLUMNWIDTH, (WPARAM)col, (LPARAM)LVSCW_AUTOSIZE_USEHEADER);
      w = (int)SendMessage(panel_list, LVM_GETCOLUMNWIDTH, (WPARAM)col, 0);
      pct = (w * 100 + list_pct_base() - 1) / list_pct_base() + 1;      /* rounded up, so the text is never cut short */
      gui.browser_col_pct[col] = pct < 5 ? 5 : (pct > 90 ? 90 : pct);
    }
    apply_column_widths();
    config_save();
  }
  else if (cmd >= 2001 && cmd < 2000 + BROWSER_COLUMNS)
  {
    browser_toggle_column(cmd - 2000);
    gui_update_menu();
  }
}

/****************************************************************************
 * Panel lifecycle
 ****************************************************************************/

static void update_sort_arrow(void);

/* Ctrl+wheel zooms the grid; plain wheel is left alone entirely (passed
   through to the control's own default scroll handling) so normal
   scrolling in both list and grid view is completely unaffected. */
/* A heading is a whole row of the grid with its words at the bottom; for the first one the empty part above them is scrolled out of view, and the
   list is kept from scrolling back up to it. */
static void grid_hide_top_blank(void)
{
  RECT it;
  POINT o;
  int blank;

  if (!panel_list || !gui.browser_grid_view || !grid_headed || filtered_count <= 0 || filtered_index[0] >= 0) return;
  if (!ListView_GetItemRect(panel_list, 0, &it, LVIR_BOUNDS) || !ListView_GetOrigin(panel_list, &o)) return;
  blank = (it.bottom - it.top) - TBS(34) - TBS(8);
  if (blank > 0 && o.y < blank) ListView_Scroll(panel_list, 0, blank - o.y);
}

static LRESULT CALLBACK browser_list_subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                                    UINT_PTR subclass_id, DWORD_PTR ref_data)
{
  (void)subclass_id;
  (void)ref_data;

  if (msg == WM_MOUSEWHEEL && (LOWORD(wp) & MK_CONTROL))
  {
    browser_grid_zoom((short)HIWORD(wp));
    return 0;
  }

  /* The grid keeps its first heading at the top: scrolling does not go up to the empty part above it. */
  if (gui.browser_grid_view && (msg == WM_VSCROLL || msg == WM_MOUSEWHEEL || msg == WM_KEYDOWN || msg == WM_SIZE))
  {
    LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
    grid_hide_top_blank();
    return r;
  }

  /* A hand over a star: it can be clicked. */
  if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT && !gui.browser_grid_view && !col_hidden[COL_FAV])
  {
    LVHITTESTINFO hit;
    ZeroMemory(&hit, sizeof(hit));
    GetCursorPos(&hit.pt);
    ScreenToClient(hwnd, &hit.pt);
    if (ListView_SubItemHitTest(hwnd, &hit) >= 0 && hit.iSubItem == COL_FAV && row_entry(hit.iItem))
    {
      SetCursor(LoadCursor(NULL, IDC_HAND));
      return TRUE;
    }
  }

  if (msg == WM_COVER_READY)
  {
    cover_collect();
    return 0;
  }

  if (msg == WM_TIMER && wp == GRID_ZOOM_TIMER_ID)
  {
    KillTimer(hwnd, GRID_ZOOM_TIMER_ID);
    browser_rebuild_grid_images();
    config_save();
    return 0;
  }

  return DefSubclassProc(hwnd, msg, wp, lp);
}

/* The container of the toolbar, search bar and filter bar: paints its own
   background in the theme's color and hands what its children send (clicks,
   drawing requests, colors) to the main window, which is where the rest of
   the program looks for them. */
/* A drop-down that draws its own rows is given a light 3D border by the system, also in the dark theme (the Classic style; the Modern style
   paints the whole drop-down itself). After it has been painted, the border is painted over in dark colors. */
static LRESULT CALLBACK filter_combo_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
  LRESULT r;

  (void)id; (void)ref;
  if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, filter_combo_proc, 3);
  r = DefSubclassProc(hwnd, msg, wp, lp);
  if ((msg == WM_PAINT || msg == WM_PRINTCLIENT || msg == WM_ERASEBKGND || msg == WM_NCPAINT) && theme_is_dark() && !gui.ui_style)
  {
    COMBOBOXINFO ci;
    RECT rc;
    HDC dc = GetDC(hwnd);

    ci.cbSize = sizeof(ci);
    if (dc && GetComboBoxInfo(hwnd, &ci) && GetClientRect(hwnd, &rc))
    {
      /* The colors of the drop-downs of the Settings window in the dark theme: a light line round the control, a dark gray line and a
         darker one inside it, then the field; a dark line between the field and the arrow's button. */
      static const COLORREF ring[3] = { RGB(155, 155, 155), RGB(56, 56, 56), RGB(32, 32, 32) };
      HBRUSH inner = CreateSolidBrush(RGB(0x2D, 0x2D, 0x2D));
      RECT f = ci.rcItem, strip;
      int bx = ci.rcButton.left > 0 ? ci.rcButton.left : rc.right, k;      /* (the arrow's button is left as it is) */

      SetRect(&strip, rc.left, rc.top, bx, f.top);       FillRect(dc, &strip, inner);       /* between the field and the edge: the border */
      SetRect(&strip, rc.left, f.bottom, bx, rc.bottom); FillRect(dc, &strip, inner);
      SetRect(&strip, rc.left, f.top, f.left, f.bottom); FillRect(dc, &strip, inner);
      SetRect(&strip, f.right, f.top, bx, f.bottom);     FillRect(dc, &strip, inner);
      for (k = 0; k < 3; k++)
      {
        HBRUSH lb = CreateSolidBrush(ring[k]);
        SetRect(&strip, rc.left + k, rc.top + k, bx, rc.top + k + 1);          FillRect(dc, &strip, lb);       /* top */
        SetRect(&strip, rc.left + k, rc.bottom - k - 1, bx, rc.bottom - k);    FillRect(dc, &strip, lb);       /* bottom */
        SetRect(&strip, rc.left + k, rc.top + k, rc.left + k + 1, rc.bottom - k); FillRect(dc, &strip, lb);    /* left */
        DeleteObject(lb);
      }
      {
        HBRUSH sep = CreateSolidBrush(ring[2]);
        SetRect(&strip, bx - 1, rc.top + 1, bx, rc.bottom - 1);                FillRect(dc, &strip, sep);       /* between the field and the arrow */
        DeleteObject(sep);
      }
      DeleteObject(inner);
    }
    if (dc) ReleaseDC(hwnd, dc);
  }
  return r;
}

static LRESULT CALLBACK bar_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
    case WM_ERASEBKGND:
    {
      RECT r;
      HBRUSH br = CreateSolidBrush(tb_face_color());
      GetClientRect(hwnd, &r);
      FillRect((HDC)wp, &r, br);
      DeleteObject(br);
      return 1;
    }

    case WM_COMMAND:
    {
      LRESULT r = SendMessage(GetParent(hwnd), msg, wp, lp);

      /* While a game runs, a toolbar button must not keep the keyboard: the
         Enter key is the pad's Start button and would press it again. */
      if (hwnd == panel_toolbar && emu_running) SetFocus(GetParent(hwnd));
      return r;
    }

    case WM_MEASUREITEM:
    case WM_DRAWITEM:
    case WM_NOTIFY:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORLISTBOX:
      return SendMessage(GetParent(hwnd), msg, wp, lp);
  }
  return DefWindowProc(hwnd, msg, wp, lp);
}

void browser_panel_create(HWND parent)
{
  HFONT font = gui_get_ui_font();
  LVCOLUMNA col;
  int i;

  entries = (browser_entry_t *)malloc(sizeof(browser_entry_t) * BROWSER_MAX_ENTRIES);
  spare   = (browser_entry_t *)malloc(sizeof(browser_entry_t) * BROWSER_MAX_ENTRIES);
  if (!entries || !spare)
  {
    free(entries); free(spare);
    entries = spare = NULL;
    MessageBoxA(parent, "Not enough memory for the ROM browser.",
                APP_NAME, MB_OK | MB_ICONERROR);
    return;
  }

  {
    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc   = bar_proc;
    wc.hInstance     = g_inst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "GPBrowserBar";
    RegisterClassA(&wc);
  }

  panel_bar = CreateWindowExA(WS_EX_CONTROLPARENT, "GPBrowserBar", "",
                              WS_CHILD | WS_CLIPCHILDREN,
                              0, 0, 0, 0, parent, (HMENU)(UINT_PTR)IDC_BROWSER_BAR,
                              g_inst, NULL);

  panel_toolbar = CreateWindowExA(WS_EX_CONTROLPARENT, "GPBrowserBar", "",
                                  WS_CHILD | WS_CLIPCHILDREN,
                                  0, 0, 0, 0, parent, NULL, g_inst, NULL);

  tb_open    = tb_button(panel_toolbar, IDM_FILE_OPEN, TI_OPEN, "Open ROM");
  tb_refresh = tb_button(panel_toolbar, IDC_TB_REFRESH, TI_REFRESH, "Refresh the ROM List");
  tb_fav     = tb_button(panel_toolbar, IDC_TB_FAV, TI_FAV, "Show Favorites Only");
  tb_search  = tb_button(panel_toolbar, IDM_VIEW_FINDSEARCH, TI_SEARCH, "Search the Game List");
  tb_sep[0]  = tb_separator(panel_toolbar);
  tb_play    = tb_button(panel_toolbar, IDC_TB_PLAY, TI_PLAY, "Play the Selected Game");
  tb_pause   = tb_button(panel_toolbar, IDM_EMU_PAUSE, TI_PAUSE, "Pause or Resume the Game");
  tb_stop    = tb_button(panel_toolbar, IDM_EMU_STOP, TI_STOP, "Stop the Game and Go Back to the List");
  tb_reset   = tb_button(panel_toolbar, IDM_EMU_RESET, TI_RESET, "Reset the Game");
  tb_fs      = tb_button(panel_toolbar, IDM_VIDEO_FULLSCREEN, TI_FULLSCREEN, "Fullscreen On or Off");
  tb_shot    = tb_button(panel_toolbar, IDM_FILE_SCREENSHOT, TI_SCREENSHOT, "Save a Screenshot");
  tb_sep[1]  = tb_separator(panel_toolbar);
  tb_set     = tb_button(panel_toolbar, IDM_OPTIONS_SETTINGS, TI_SETTINGS, "All Settings");
  tb_video   = tb_button(panel_toolbar, IDM_OPTIONS_SETTINGS_VIDEO, TI_VIDEO, "Video Settings");
  tb_audio   = tb_button(panel_toolbar, IDM_OPTIONS_SETTINGS_AUDIO, TI_AUDIO, "Audio Settings");
  tb_ctl     = tb_button(panel_toolbar, IDM_INPUT_P1, TI_CONTROLS, "Controls (Keyboard and Gamepad Buttons)");
  tb_set_label(tb_open, "Open");        tb_set_label(tb_refresh, "Refresh");  tb_set_label(tb_fav, "Favorites");   tb_set_label(tb_search, "Search");
  tb_set_label(tb_play, "Play");        tb_set_label(tb_pause, "Pause");      tb_set_label(tb_stop, "Stop");        tb_set_label(tb_reset, "Reset");
  tb_set_label(tb_fs, "Fullscreen");    tb_set_label(tb_shot, "Screenshot");  tb_set_label(tb_set, "Settings");     tb_set_label(tb_video, "Video");
  tb_set_label(tb_audio, "Audio");      tb_set_label(tb_ctl, "Controls");

  panel_search_label = CreateWindowExA(0, "STATIC", "Search:", WS_CHILD | SS_LEFT,
                                       0, 0, 0, 0, panel_bar, (HMENU)(UINT_PTR)IDC_BROWSER_SEARCH_LABEL,
                                       g_inst, NULL);
  panel_search = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                                 WS_CHILD | WS_CLIPSIBLINGS | ES_AUTOHSCROLL | WS_TABSTOP,
                                 0, 0, 0, 0, panel_bar, (HMENU)(UINT_PTR)IDC_BROWSER_SEARCH,
                                 g_inst, NULL);
  panel_search_clear = CreateWindowExA(0, "STATIC", "x", WS_CHILD | WS_CLIPSIBLINGS | SS_NOTIFY | SS_CENTER | SS_CENTERIMAGE,   /* plain text: no button outline */
                                       0, 0, 0, 0, panel_bar, (HMENU)(UINT_PTR)IDC_BROWSER_SEARCH_CLEAR,
                                       g_inst, NULL);
  panel_search_close = CreateWindowExA(0, "BUTTON", "Close", WS_CHILD | BS_PUSHBUTTON | WS_TABSTOP,
                                       0, 0, 0, 0, panel_bar, (HMENU)(UINT_PTR)IDC_BROWSER_SEARCH_CLOSE,
                                       g_inst, NULL);

  lstrcpynA(sel_con, gui.filter_console, sizeof(sel_con));      /* the choices of the last run */
  lstrcpynA(sel_fol, gui.filter_genre, sizeof(sel_fol));
  sel_fol_on = gui.filter_genre_on;
  lstrcpynA(sel_reg, gui.filter_region, sizeof(sel_reg));

  for (i = 0; i < 3; i++)
  {
    panel_filter[i] = CreateWindowExA(0, "COMBOBOX", "",
                                      WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                                      0, 0, 0, 0, panel_bar,
                                      (HMENU)(UINT_PTR)(IDC_BROWSER_FILTER_CONSOLE + i), g_inst, NULL);
    SetWindowSubclass(panel_filter[i], filter_combo_proc, 3, 0);
  }
  panel_filter_reset = CreateWindowExA(0, "BUTTON", "Reset Filters", WS_CHILD | BS_PUSHBUTTON | WS_TABSTOP,
                                       0, 0, 0, 0, panel_bar, (HMENU)(UINT_PTR)IDC_BROWSER_FILTER_RESET,
                                       g_inst, NULL);

  panel_list = CreateWindowExA(WS_EX_CLIENTEDGE, "SysListView32", "",
                               WS_CHILD | LVS_REPORT | LVS_SINGLESEL |
                               LVS_SHOWSELALWAYS | LVS_AUTOARRANGE | WS_TABSTOP | WS_CLIPSIBLINGS,
                               0, 0, 0, 0, parent, (HMENU)(UINT_PTR)IDC_BROWSER_LIST,
                               g_inst, NULL);
  SetWindowSubclass(panel_list, browser_list_subclass_proc, 1, 0);

  {
    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc   = az_proc;
    wc.hInstance     = g_inst;
    wc.hCursor       = LoadCursor(NULL, IDC_HAND);
    wc.lpszClassName = "GPAZStrip";
    RegisterClassA(&wc);
    panel_az = CreateWindowExA(0, "GPAZStrip", "", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent, NULL, g_inst, NULL);

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc   = top_proc;
    wc.hInstance     = g_inst;
    wc.hCursor       = LoadCursor(NULL, IDC_HAND);
    wc.lpszClassName = "GPTopButton";
    RegisterClassA(&wc);
    panel_top = CreateWindowExA(WS_EX_LAYERED, "GPTopButton", "", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent, NULL, g_inst, NULL);
    top_layered = (panel_top != NULL);          /* drawn with soft edges; without layered child windows (Windows 7) the old region is used */
    if (!panel_top)                             /* Windows 7 refuses a layered child window: a plain one, drawn the old way */
      panel_top = CreateWindowExA(0, "GPTopButton", "", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent, NULL, g_inst, NULL);
    if (panel_top) SetTimer(panel_top, 1, 150, NULL);
  }

  panel_choose = CreateWindowExA(0, "BUTTON", "Choose ROM Directory",
                                 WS_CHILD | WS_TABSTOP,
                                 0, 0, 0, 0, parent, (HMENU)(UINT_PTR)IDC_BROWSER_CHOOSE,
                                 g_inst, NULL);

  SendMessage(panel_search, WM_SETFONT, (WPARAM)font, TRUE);
  SendMessage(panel_search_label, WM_SETFONT, (WPARAM)font, TRUE);
  SendMessage(panel_search_close, WM_SETFONT, (WPARAM)font, TRUE);
  SendMessage(panel_search_clear, WM_SETFONT, (WPARAM)font, TRUE);
  {
    static HWND clear_tip;
    TOOLINFOA ti;
    if (!clear_tip)
      clear_tip = CreateWindowExA(WS_EX_TOPMOST, TOOLTIPS_CLASSA, NULL, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0, panel_bar, NULL, g_inst, NULL);
    if (clear_tip)
    {
      memset(&ti, 0, sizeof(ti));
      ti.cbSize = sizeof(ti);
      ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
      ti.hwnd = panel_bar;
      ti.uId = (UINT_PTR)panel_search_clear;
      ti.lpszText = (LPSTR)"Clear";
      SendMessageA(clear_tip, TTM_ADDTOOLA, 0, (LPARAM)&ti);
    }
  }
  SendMessage(panel_filter_reset, WM_SETFONT, (WPARAM)font, TRUE);
  for (i = 0; i < 3; i++)
  {
    SendMessage(panel_filter[i], WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageA(panel_filter[i], CB_ADDSTRING, 0,
                 (LPARAM)(i == 0 ? "All Consoles" : i == 1 ? "All Genres" : "All Regions"));
    SendMessage(panel_filter[i], CB_SETCURSEL, 0, 0);
  }
  SendMessage(panel_list, WM_SETFONT, (WPARAM)font, TRUE);
  browser_apply_row_height();
  SendMessage(panel_choose, WM_SETFONT, (WPARAM)font, TRUE);

  SendMessage(panel_list, LVM_SETEXTENDEDLISTVIEWSTYLE,
              LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP);   /* no grid lines between the columns */

  for (i = 0; i < COL_FIRST_EXTRA; i++)
  {
    col_hidden[i] = (gui.browser_col_hidden & (1 << i)) ? 1 : 0;
  }
  for (i = COL_FIRST_EXTRA; i < BROWSER_COLUMNS; i++)
  {
    col_hidden[i] = (gui.browser_col_extra & (1 << (i - COL_FIRST_EXTRA))) ? 0 : 1;   /* hidden until switched on */
  }
  col_hidden[0] = 0;   /* Name is the row's identity, never actually toggleable */

  ZeroMemory(&col, sizeof(col));
  col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

  for (i = 0; i < BROWSER_COLUMNS; i++)
  {
    col.iSubItem = i;
    col.cx = col_width(i);
    col.pszText = (LPSTR)col_names[i];
    SendMessageA(panel_list, LVM_INSERTCOLUMNA, (WPARAM)i, (LPARAM)&col);
  }
  {
    SendMessage(panel_list, LVM_SETCOLUMNORDERARRAY, BROWSER_COLUMNS,
                (LPARAM)(gui.browser_col_order[0] >= 0 ? gui.browser_col_order : col_order));   /* the order the person left them in, else the star, Name, Console, Genre ... */
  }
  update_sort_arrow();
  browser_toolbar_update();
  browser_toolbar_tips();
}

/* Shows the toolbar, search bar and filter bar rows that are switched on
   (and none of them while the "Choose ROM Directory" button is what is shown). */
static void show_bars(void)
{
  int on = panel_visible && gui.rom_dir[0];
  int sb = on && search_shown(), fb = on && !gui.hide_filterbar;
  int i;

  ShowWindow(panel_bar, (sb || fb) ? SW_SHOWNA : SW_HIDE);
  ShowWindow(panel_search_label, sb ? SW_SHOWNA : SW_HIDE);
  ShowWindow(panel_search, sb ? SW_SHOWNA : SW_HIDE);
  ShowWindow(panel_search_close, sb ? SW_SHOWNA : SW_HIDE);
  ShowWindow(panel_search_clear, (sb && GetWindowTextLengthA(panel_search) > 0) ? SW_SHOWNA : SW_HIDE);
  for (i = 0; i < 3; i++) ShowWindow(panel_filter[i], fb ? SW_SHOWNA : SW_HIDE);
  ShowWindow(panel_filter_reset, fb ? SW_SHOWNA : SW_HIDE);
}

/* Places the bar and its rows; returns the height it takes. */
static int layout_bar(int x, int y, int w)
{
  int m = TBS(4), rows = 0, ry = TBS(2);

  if (search_shown())
  {
    int lw = TBS(54), cw = TBS(72), eh = TBS(22);
    MoveWindow(panel_search_label, m, ry + TBS(4), lw, TBS(18), TRUE);
    {
      int sw = w - 2 * m - lw - cw - TBS(6), bw = eh - TBS(6);
      MoveWindow(panel_search, m + lw, ry, sw, eh, TRUE);
      MoveWindow(panel_search_clear, m + lw + sw - bw - TBS(4), ry + TBS(3), bw, eh - TBS(6), TRUE);
      SendMessage(panel_search, EM_SETMARGINS, EC_RIGHTMARGIN, MAKELPARAM(0, bw + TBS(6)));   /* the text stays clear of the x */
      SetWindowPos(panel_search_clear, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    MoveWindow(panel_search_close, w - m - cw, ry - 1, cw, eh + 2, TRUE);
    ry += eh + TBS(6);
    rows++;
  }

  if (!gui.hide_filterbar)
  {
    int rw = TBS(96), g3 = TBS(6), eh = TBS(22), k;
    int cw = (w - 2 * m - rw - 3 * g3) / 3;
    if (cw < 40) cw = 40;
    for (k = 0; k < 3; k++)
      MoveWindow(panel_filter[k], m + k * (cw + g3), ry, cw, TBS(220), TRUE);
    MoveWindow(panel_filter_reset, w - m - rw, ry - 1, rw, eh + 2, TRUE);
    ry += eh + TBS(6);
    rows++;
  }

  if (!rows) { MoveWindow(panel_bar, x, y, w, 0, TRUE); return 0; }

  MoveWindow(panel_bar, x, y, w, ry, TRUE);
  return ry;
}

static void ensure_rom_loaded(const char *path);

/* Whether a game is picked in the list (File > ROM Information works on it). */
int browser_has_selection(void)
{
  return panel_visible && panel_list && ListView_GetNextItem(panel_list, -1, LVNI_SELECTED) >= 0;
}

/* File > ROM Information with no game running: the game picked in the list. */
void browser_show_rom_info(void)
{
  int sel;
  char path[GUI_PATH_LEN];

  if (!browser_has_selection()) return;
  sel = ListView_GetNextItem(panel_list, -1, LVNI_SELECTED);
  if (!row_entry(sel)) return;
  lstrcpynA(path, row_entry(sel)->full_path, sizeof(path));
  ensure_rom_loaded(path);
  dlg_rom_info(g_hwnd);
}

/* Tools > Cheats with no game running: the cheats of the game picked in the list. */
void browser_edit_cheats(void)
{
  int sel;
  char path[GUI_PATH_LEN];

  if (!browser_has_selection()) return;
  sel = ListView_GetNextItem(panel_list, -1, LVNI_SELECTED);
  if (!row_entry(sel)) return;
  lstrcpynA(path, row_entry(sel)->full_path, sizeof(path));
  ensure_rom_loaded(path);
  dlg_cheats(g_hwnd);
}

int browser_panel_visible(void)
{
  return panel_visible;
}

void browser_panel_show(int show)
{
  int has_folder = gui.rom_dir[0] != '\0';

  panel_visible = show ? 1 : 0;

  ShowWindow(panel_list, (show && has_folder) ? SW_SHOW : SW_HIDE);
  ShowWindow(panel_choose, (show && !has_folder) ? SW_SHOW : SW_HIDE);
  if (panel_az) ShowWindow(panel_az, (show && has_folder && gui.az_strip) ? SW_SHOWNA : SW_HIDE);
  if (panel_top && !show) ShowWindow(panel_top, SW_HIDE);
  show_bars();

  if (show)
  {
    browser_apply_view_mode();
    if (has_folder) browser_rescan();
    InvalidateRect(g_hwnd, NULL, TRUE);

    /* Coming back from a game: bring the browser's windows above whatever
       the renderer left behind and repaint every one of them, so the filter
       bar and the toolbar are drawn from scratch. */
    {
      HWND w[] = { panel_list, panel_bar, panel_toolbar };
      int i;
      for (i = 0; i < 3; i++)
        if (w[i])
        {
          SetWindowPos(w[i], HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
          RedrawWindow(w[i], NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
        }
      if (panel_top) SetWindowPos(panel_top, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);   /* the list was just raised: the arrow goes back on top of it */
    }
  }
}

/* Lays the panel out within the given area -- same region video would
   otherwise occupy (below the menu, above the status bar). */
void browser_panel_layout(const RECT *area)
{
  int x = area->left;
  int w = area->right - area->left;
  int y = area->top;

  if (w < 40) w = 40;

  if (!gui.rom_dir[0])
  {
    int btn_w = gui.large_ui ? 300 : 200;
    int btn_h = gui.large_ui ? 39 : 26;
    int cx = (area->left + area->right) / 2;
    int cy = (area->top + area->bottom) / 2;

    MoveWindow(panel_choose, cx - btn_w / 2, cy - btn_h / 2, btn_w, btn_h, TRUE);
    return;
  }

  y += layout_bar(x, y, w);

  {
    int list_h = area->bottom - y;
    if (list_h < 40) list_h = 40;

    int strip = gui.az_strip ? AZ_W : 0;

    MoveWindow(panel_list, x, y, w - strip, list_h, TRUE);
    browser_grid_check_cols();
    if (panel_az)
    {
      if (strip) MoveWindow(panel_az, x + w - strip, y, strip, list_h, TRUE);
      ShowWindow(panel_az, (strip && panel_visible) ? SW_SHOWNA : SW_HIDE);
    }
    w -= strip;

    if (panel_top)
    {
      int sz = TBS(34), gap = TBS(10);
      HRGN rgn;
      /* Up in the corner of the list, clear of its vertical scroll bar; above the horizontal one only when that one is showing
         (the grid has none, which left the button floating well above the corner). */
      int hbar = (GetWindowLongPtr(panel_list, GWL_STYLE) & WS_HSCROLL) ? GetSystemMetrics(SM_CYHSCROLL) : 0;
      MoveWindow(panel_top, x + w - GetSystemMetrics(SM_CXVSCROLL) - gap - sz, y + list_h - gap - sz - hbar, sz, sz, TRUE);
      if (!top_render(panel_top))
      {
        rgn = CreateEllipticRgn(0, 0, sz + 1, sz + 1);
        SetWindowRgn(panel_top, rgn, TRUE);
      }
      SetWindowPos(panel_top, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    if (gui.browser_grid_view) ListView_Arrange(panel_list, LVA_DEFAULT);

    /* Vertical scrollbar (~18px) eats into the usable width regardless of
       column sizing, so it's subtracted up front rather than left to force
       a horizontal scrollbar that fixed pixel widths would risk causing. */
    last_list_w = w - 18;
    if (last_list_w < 100) last_list_w = 100;
    apply_column_widths();
  }
}

/****************************************************************************
 * A to Z strip: a narrow column of letters beside the list; a click jumps to the games starting with it
 ****************************************************************************/

static char az_present[27];      /* [0] = names starting with a digit or a symbol, [1..26] = A..Z */
static int  az_hot = -1;

static int az_index_of(const char *title)
{
  unsigned char c = (unsigned char)title[0];
  return isalpha(c) ? toupper(c) - 'A' + 1 : 0;
}

/* What the list shows for a row. */
static void row_title(const browser_entry_t *e, char *out, int out_len)
{
  display_name(e->name, out, out_len);
}

static void az_update(void)
{
  int row, seen = 0;      /* the games of the Recently Played group are not part of the alphabet */
  char t[160];

  ZeroMemory(az_present, sizeof(az_present));
  for (row = 0; row < filtered_count; row++)
    if (row_entry(row))
    {
      if (seen++ < recent_group_n) continue;
      row_title(row_entry(row), t, sizeof(t));
      az_present[az_index_of(t)] = 1;
    }
  if (panel_az) InvalidateRect(panel_az, NULL, TRUE);
}

/* A letter typed while the game list is showing and the keyboard is elsewhere (the toolbar, the window itself): the list
   takes the focus and the letter, so its own type-to-find jumps to that game. Returns 1 when the key was handed over. */
int browser_type_ahead(MSG *msg)
{
  char cls[24] = "";
  HWND f;

  if (!panel_list || !IsWindowVisible(panel_list) || msg->message != WM_KEYDOWN) return 0;
  if (msg->wParam < 'A' || msg->wParam > 'Z') return 0;
  if ((GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000)) return 0;

  f = GetFocus();
  if (f == panel_list) return 0;
  if (f) GetClassNameA(f, cls, sizeof(cls));
  if (!lstrcmpiA(cls, "Edit") || !lstrcmpiA(cls, "ComboBox") || !lstrcmpiA(cls, "ComboLBox") || !lstrcmpiA(cls, "ListBox")) return 0;

  SetFocus(panel_list);
  msg->hwnd = panel_list;
  return 1;
}

/* The floating "back to the top" button: a round arrow over the bottom right of the list, shown only once the list has
   been scrolled down. */
static int top_hot;

/* The distance from a point to a line segment. */
static double seg_dist(double px, double py, double ax, double ay, double bx, double by)
{
  double dx = bx - ax, dy = by - ay, t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy);
  if (t < 0) t = 0; else if (t > 1) t = 1;
  return sqrt((px - ax - t * dx) * (px - ax - t * dx) + (py - ay - t * dy) * (py - ay - t * dy));
}

/* Draws the button into a layered window with its own alpha, every pixel from 4 x 4 samples, so the edge of the disc, its thin
   ring and the round-ended arrow are smooth (a window region has hard, stair-stepped edges). Returns 0 when that is not
   possible, and the caller falls back to the region and plain GDI drawing. */
static int top_render(HWND hwnd)
{
  RECT rc;
  BITMAPINFO bi;
  unsigned char *bits;
  HBITMAP bmp;
  HDC mem;
  HGDIOBJ old;
  BLENDFUNCTION bf;
  POINT src = { 0, 0 };
  SIZE sz;
  int w, h, x, y, i, j, dark = theme_is_dark(), ok;
  const int SS = 4;
  double R, cx, cy, u, half;
  int face[3], ring[3], arrow[3];

  if (!top_layered || !hwnd) return 0;
  GetClientRect(hwnd, &rc);
  w = rc.right; h = rc.bottom;
  if (w < 8 || h < 8) return top_layered;                  /* not sized yet: nothing to draw, nothing wrong */

  if (dark)
  {
    int f = top_hot ? 0x5A : 0x44, r = top_hot ? 0x90 : 0x70;
    face[0] = face[1] = face[2] = f; ring[0] = ring[1] = ring[2] = r; arrow[0] = arrow[1] = arrow[2] = 0xF2;
  }
  else
  {
    int f = top_hot ? 0xFF : 0xEC;
    face[0] = face[1] = face[2] = f; ring[0] = ring[1] = ring[2] = 0xB0; arrow[0] = arrow[1] = arrow[2] = 0x38;
  }

  ZeroMemory(&bi, sizeof(bi));
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  mem = CreateCompatibleDC(NULL);
  bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
  if (!bmp) { DeleteDC(mem); return 0; }

  cx = w / 2.0; cy = h / 2.0;
  R = w / 2.0 - 0.3;                  /* the disc, a hair inside the window */
  u = w / 2.0;
  half = u * 0.085;                   /* half the stroke of the arrow */

  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
    {
      int sa = 0, sr = 0, sg = 0, sb = 0;

      for (j = 0; j < SS; j++)
        for (i = 0; i < SS; i++)
        {
          double px = x + (i + 0.5) / SS, py = y + (j + 0.5) / SS;
          double d = sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
          const int *c;

          if (d > R) continue;                                   /* outside the disc: see-through */
          if (seg_dist(px, py, cx, cy + u * 0.36, cx, cy - u * 0.36) <= half ||                /* the arrow: a stem ... */
              seg_dist(px, py, cx, cy - u * 0.36, cx - u * 0.34, cy - u * 0.02) <= half ||      /* ... and a head, with round ends */
              seg_dist(px, py, cx, cy - u * 0.36, cx + u * 0.34, cy - u * 0.02) <= half) c = arrow;
          else if (d > R - 1.3) c = ring;
          else c = face;
          sa++; sr += c[0]; sg += c[1]; sb += c[2];
        }
      {
        unsigned char *p = bits + ((size_t)y * w + x) * 4;
        int n = SS * SS;
        p[0] = (unsigned char)(sb / n);                          /* premultiplied: the sums already carry the alpha */
        p[1] = (unsigned char)(sg / n);
        p[2] = (unsigned char)(sr / n);
        p[3] = (unsigned char)(sa * 255 / n);
      }
    }

  old = SelectObject(mem, bmp);
  sz.cx = w; sz.cy = h;
  bf.BlendOp = AC_SRC_OVER; bf.BlendFlags = 0; bf.SourceConstantAlpha = 255; bf.AlphaFormat = AC_SRC_ALPHA;
  ok = UpdateLayeredWindow(hwnd, NULL, NULL, &sz, mem, &src, 0, &bf, ULW_ALPHA);
  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);
  if (!ok)
  {
    top_layered = 0;                                       /* not available here: back to the plain window */
    SetWindowLongPtr(hwnd, GWL_EXSTYLE, GetWindowLongPtr(hwnd, GWL_EXSTYLE) & ~(LONG_PTR)WS_EX_LAYERED);
    return 0;
  }
  return 1;
}

static LRESULT CALLBACK top_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
    case WM_ERASEBKGND:
      return 1;

    case WM_PAINT:
    {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      RECT rc;
      int dark = theme_is_dark(), w, h, cx, cy, s;

      if (top_layered)                         /* the layered picture is the whole button */
      {
        EndPaint(hwnd, &ps);
        return 0;
      }
      /* Dark theme: a dark disc with a light arrow. Light theme: the other way round, a light disc with a dark arrow. */
      COLORREF face = dark ? (top_hot ? RGB(0x5A, 0x5A, 0x5A) : RGB(0x44, 0x44, 0x44)) : (top_hot ? RGB(0xFF, 0xFF, 0xFF) : RGB(0xEC, 0xEC, 0xEC));
      HBRUSH fb = CreateSolidBrush(face), wb = CreateSolidBrush(dark ? RGB(0xF2, 0xF2, 0xF2) : RGB(0x38, 0x38, 0x38));
      HPEN ring = CreatePen(PS_SOLID, 1, dark ? RGB(0x70, 0x70, 0x70) : RGB(0xB0, 0xB0, 0xB0));
      HPEN np = ring;
      POINT tri[3];
      HGDIOBJ ob, op;

      GetClientRect(hwnd, &rc);
      w = rc.right; h = rc.bottom;
      ob = SelectObject(dc, fb); op = SelectObject(dc, np);
      Ellipse(dc, 0, 0, w, h);
      SelectObject(dc, GetStockObject(NULL_PEN));
      SelectObject(dc, wb);
      cx = w / 2; cy = h / 2; s = w / 5;
      tri[0].x = cx;         tri[0].y = cy - s;       /* an arrow pointing up: a triangle over a short stem */
      tri[1].x = cx - s - 1; tri[1].y = cy + 1;
      tri[2].x = cx + s + 1; tri[2].y = cy + 1;
      Polygon(dc, tri, 3);
      Rectangle(dc, cx - s / 2, cy, cx + s / 2 + 2, cy + s + 1);
      SelectObject(dc, ob); SelectObject(dc, op);
      DeleteObject(fb); DeleteObject(wb); DeleteObject(ring);
      EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_MOUSEMOVE:
      if (!top_hot)
      {
        TRACKMOUSEEVENT t;
        top_hot = 1;
        t.cbSize = sizeof(t); t.dwFlags = TME_LEAVE; t.hwndTrack = hwnd; t.dwHoverTime = 0;
        TrackMouseEvent(&t);
        if (!top_render(hwnd)) InvalidateRect(hwnd, NULL, FALSE);
      }
      return 0;

    case WM_MOUSELEAVE:
      top_hot = 0;
      if (!top_render(hwnd)) InvalidateRect(hwnd, NULL, FALSE);
      return 0;

    case WM_LBUTTONDOWN:
      if (panel_list)
      {
        ListView_EnsureVisible(panel_list, 0, FALSE);
        SendMessage(panel_list, WM_VSCROLL, SB_TOP, 0);
        {
          int n = ListView_GetItemCount(panel_list), r = 0;
          while (r < n && !row_entry(r)) r++;            /* the first game, not a heading */
          if (r < n) ListView_SetItemState(panel_list, r, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        }
        ShowWindow(hwnd, SW_HIDE);
      }
      return 0;

    case WM_TIMER:
      if (wp == 1)
      {
        static int last_dark = -1;
        if (top_layered && last_dark != theme_is_dark()) { last_dark = theme_is_dark(); top_render(hwnd); }   /* the theme changed */
        if (panel_list && IsWindowVisible(panel_list))
        {
          /* The horizontal scroll bar of the list comes and goes after the layout (the columns grew wider than the window):
             the arrow keeps clear of it, wherever it is. */
          RECT lr, cur;
          HWND par = GetParent(hwnd);
          int sz = TBS(34), gap = TBS(10), nx, ny;
          int hb = (GetWindowLongPtr(panel_list, GWL_STYLE) & WS_HSCROLL) ? GetSystemMetrics(SM_CYHSCROLL) : 0;
          GetWindowRect(panel_list, &lr);
          MapWindowPoints(NULL, par, (POINT *)&lr, 2);
          nx = lr.right - GetSystemMetrics(SM_CXVSCROLL) - gap - sz;
          ny = lr.bottom - gap - sz - hb;
          GetWindowRect(hwnd, &cur);
          MapWindowPoints(NULL, par, (POINT *)&cur, 2);
          if (cur.left != nx || cur.top != ny) SetWindowPos(hwnd, NULL, nx, ny, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        int scrolled = panel_list && (gui.browser_grid_view ? GetScrollPos(panel_list, SB_VERT) > 0 : ListView_GetTopIndex(panel_list) > 0);
        int want = panel_visible && panel_list && IsWindowVisible(panel_list) && scrolled;
        if (want != (IsWindowVisible(hwnd) ? 1 : 0))
        {
          if (want) SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);   /* above the list, whatever raised it last */
          else      ShowWindow(hwnd, SW_HIDE);
        }
      }
      return 0;

    case WM_DESTROY:
      KillTimer(hwnd, 1);
      return 0;
  }
  return DefWindowProc(hwnd, msg, wp, lp);
}

/* The game that starts with the letter (or digit, or #) picked in the strip. A letter can have several runs of games in the list (the games
   of one console, say, when the list is grouped or sorted by console): pressing the same letter again goes to the start of the next run, and
   after the last one back to the first. */
static void az_jump(int idx)
{
  int row, seen = 0, cur, cur_ok = 0, first = -1, next = -1;
  char t[160];

  if (idx < 0 || idx > 26) return;
  cur = ListView_GetNextItem(panel_list, -1, LVNI_SELECTED);
  if (cur >= 0 && row_entry(cur))
  {
    row_title(row_entry(cur), t, sizeof(t));
    cur_ok = az_index_of(t) == idx;
  }
  for (row = 0; row < filtered_count && next < 0; row++)
    if (row_entry(row))
    {
      int starts;

      if (seen++ < recent_group_n) continue;      /* not the Recently Played group */
      row_title(row_entry(row), t, sizeof(t));
      if (az_index_of(t) != idx) continue;
      starts = row == 0 || !row_entry(row - 1);
      if (!starts)
      {
        char pt[160];
        row_title(row_entry(row - 1), pt, sizeof(pt));
        starts = az_index_of(pt) != idx;          /* the one before is another letter: a new run */
      }
      if (!starts) continue;
      if (first < 0) first = row;
      if (cur_ok && row > cur) next = row;
    }
  row = next >= 0 ? next : first;
  if (row >= 0)
  {
    ListView_EnsureVisible(panel_list, filtered_count - 1, FALSE);   /* the game ends up at the top, not the bottom */
    ListView_EnsureVisible(panel_list, row, FALSE);
    ListView_SetItemState(panel_list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
  }
}

static LRESULT CALLBACK az_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
    case WM_ERASEBKGND:
      return 1;

    case WM_PAINT:
    {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      RECT rc;
      int i, dark = theme_is_dark();
      HFONT of = (HFONT)SelectObject(dc, gui_get_ui_font());
      HBRUSH bg = CreateSolidBrush(dark ? RGB(0x20, 0x20, 0x20) : GetSysColor(COLOR_BTNFACE));

      GetClientRect(hwnd, &rc);
      FillRect(dc, &rc, bg);
      DeleteObject(bg);
      SetBkMode(dc, TRANSPARENT);
      for (i = 0; i < 27; i++)
      {
        RECT r;
        char s[2] = { (char)(i ? 'A' + i - 1 : '#'), 0 };

        r.left = 0; r.right = rc.right;
        r.top = rc.bottom * i / 27; r.bottom = rc.bottom * (i + 1) / 27;
        if (i == az_hot && az_present[i])
        {
          HBRUSH hb = CreateSolidBrush(dark ? RGB(0x50, 0x50, 0x50) : RGB(0xC0, 0xD8, 0xF0));
          FillRect(dc, &r, hb);
          DeleteObject(hb);
        }
        SetTextColor(dc, az_present[i] ? (dark ? RGB(0xF0, 0xF0, 0xF0) : RGB(0, 0, 0)) : (dark ? RGB(0x70, 0x70, 0x70) : RGB(0xA8, 0xA8, 0xA8)));
        if (i == az_hot)
        {
          /* The letter under the pointer is drawn bigger (and bold), over the letters next to it. */
          static HFONT big;
          static int big_h;
          LOGFONTA lf;
          HFONT base = gui_get_ui_font();
          if (base && GetObjectA(base, sizeof(lf), &lf))
          {
            int h = lf.lfHeight < 0 ? lf.lfHeight * 17 / 10 : lf.lfHeight * 17 / 10;
            if (!big || big_h != h)
            {
              if (big) DeleteObject(big);
              lf.lfHeight = h; lf.lfWeight = FW_BOLD; lf.lfWidth = 0;
              big = CreateFontIndirectA(&lf);
              big_h = h;
            }
          }
          if (big)
          {
            HGDIOBJ prev = SelectObject(dc, big);
            DrawTextA(dc, s, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
            SelectObject(dc, prev);
            continue;
          }
        }
        DrawTextA(dc, s, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      }
      SelectObject(dc, of);
      EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_MOUSEMOVE:
    {
      RECT rc;
      int idx;
      GetClientRect(hwnd, &rc);
      idx = rc.bottom > 0 ? (int)((short)HIWORD(lp)) * 27 / rc.bottom : -1;
      if (idx < 0 || idx > 26) idx = -1;
      if (idx != az_hot)
      {
        TRACKMOUSEEVENT te;
        az_hot = idx;
        InvalidateRect(hwnd, NULL, FALSE);
        te.cbSize = sizeof(te); te.dwFlags = TME_LEAVE; te.hwndTrack = hwnd; te.dwHoverTime = 0;
        TrackMouseEvent(&te);
        if ((wp & MK_LBUTTON) && az_hot >= 0) az_jump(az_hot);
      }
      return 0;
    }

    case WM_MOUSELEAVE:
      az_hot = -1;
      InvalidateRect(hwnd, NULL, FALSE);
      return 0;

    case WM_LBUTTONDOWN:
    {
      RECT rc;
      GetClientRect(hwnd, &rc);
      SetCapture(hwnd);
      if (rc.bottom > 0) az_jump((int)((short)HIWORD(lp)) * 27 / rc.bottom);
      return 0;
    }

    case WM_LBUTTONUP:
      if (GetCapture() == hwnd) ReleaseCapture();
      return 0;
  }
  return DefWindowProc(hwnd, msg, wp, lp);
}

void browser_layout_changed(void)
{
  browser_relayout();
}

/****************************************************************************
 * ROM Folders window: which folders make up the library, and which are left out
 ****************************************************************************/

static char rf_dir[7][GUI_PATH_LEN];     /* [0] is the ROM directory */
static int  rf_n;
static char rf_ex[6][GUI_PATH_LEN];
static int  rf_ex_n;

/* The same folder, however it was written: no difference between / and \\, upper and lower case, or a \\ at the end. */
static int rf_same_path(const char *a, const char *b)
{
  for (;;)
  {
    char ca = *a, cb = *b;
    if (ca == '/') ca = '\\';
    if (cb == '/') cb = '\\';
    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
    if (!ca || !cb)
    {
      /* one ended: the other may only have separators left */
      const char *rest = ca ? a : b;
      if (!ca && !cb) return 1;
      while (*rest == '\\' || *rest == '/') rest++;
      return *rest == '\0';
    }
    if (ca != cb) return 0;
    a++; b++;
  }
}

static void rf_fill(HWND dlg)
{
  HWND l = GetDlgItem(dlg, IDC_RF_LIST), e = GetDlgItem(dlg, IDC_RF_EXLIST);
  LVITEMA it;
  int i;

  ListView_DeleteAllItems(l);
  ListView_DeleteAllItems(e);
  ZeroMemory(&it, sizeof(it));
  it.mask = LVIF_TEXT;
  for (i = 0; i < rf_n; i++)
  {
    it.iItem = i;
    it.pszText = rf_dir[i][0] ? rf_dir[i] : (char *)"(not chosen yet)";
    SendMessageA(l, LVM_INSERTITEMA, 0, (LPARAM)&it);
  }
  for (i = 0; i < rf_ex_n; i++)
  {
    it.iItem = i;
    it.pszText = rf_ex[i];
    SendMessageA(e, LVM_INSERTITEMA, 0, (LPARAM)&it);
  }
}

static INT_PTR CALLBACK romfolders_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
    case WM_INITDIALOG:
    {
      int i;
      LVCOLUMNA col;
      HWND l = GetDlgItem(dlg, IDC_RF_LIST), e = GetDlgItem(dlg, IDC_RF_EXLIST);
      RECT rc;

      lstrcpynA(rf_dir[0], gui.rom_dir, GUI_PATH_LEN);
      rf_n = 1 + gui.rom_extra_n;
      for (i = 0; i < gui.rom_extra_n; i++) { lstrcpynA(rf_dir[i + 1], gui.rom_extra[i], GUI_PATH_LEN); }
      rf_ex_n = gui.rom_exclude_n;
      for (i = 0; i < rf_ex_n; i++) lstrcpynA(rf_ex[i], gui.rom_exclude[i], GUI_PATH_LEN);

      SetDlgItemTextA(dlg, IDC_RF_HELP,
        "The game list is made of all the directories above. "
        "An excluded directory is skipped, with everything inside it.");
      SendMessage(l, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT);
      SendMessage(e, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT);
      ZeroMemory(&col, sizeof(col));
      col.mask = LVCF_WIDTH;
      GetClientRect(l, &rc);
      col.cx = rc.right - GetSystemMetrics(SM_CXVSCROLL);
      SendMessageA(l, LVM_INSERTCOLUMNA, 0, (LPARAM)&col);
      SendMessageA(e, LVM_INSERTCOLUMNA, 0, (LPARAM)&col);
      theme_apply_to_window(dlg);
      rf_fill(dlg);
      return TRUE;
    }

    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    {
      HBRUSH br = theme_ctlcolor((HDC)wp);
      if (br) return (LRESULT)br;
      break;
    }

    case WM_COMMAND:
      switch (LOWORD(wp))
      {
        case IDC_RF_ADD:
        {
          char p[GUI_PATH_LEN];
          if (rf_n < 7 && pick_folder(dlg, p, sizeof(p)))
          {
            int i, dup = 0;
            for (i = 0; i < rf_n && !dup; i++) if (rf_dir[i][0] && rf_same_path(rf_dir[i], p)) dup = 1;
            if (dup) { MessageBoxA(dlg, "That directory is already in the list.", "ROM Directories", MB_OK | MB_ICONINFORMATION); return TRUE; }
            if (rf_n == 1 && !rf_dir[0][0]) lstrcpynA(rf_dir[0], p, GUI_PATH_LEN);      /* the empty first row is used first */
            else { lstrcpynA(rf_dir[rf_n], p, GUI_PATH_LEN); rf_n++; }
            rf_fill(dlg);
          }
          return TRUE;
        }
        case IDC_RF_REMOVE:
        {
          int sel = ListView_GetNextItem(GetDlgItem(dlg, IDC_RF_LIST), -1, LVNI_SELECTED), i;
          if (sel >= 0)
          {
            if (rf_n == 1) rf_dir[0][0] = '\0';       /* the only one: the row stays, "not chosen yet" */
            else
            {
              for (i = sel; i + 1 < rf_n; i++) lstrcpyA(rf_dir[i], rf_dir[i + 1]);
              rf_n--;                                                 /* (the first row is the ROM directory of the File menu: the next one takes its place) */
            }
            rf_fill(dlg);
          }
          return TRUE;
        }
        case IDC_RF_EXADD:
        {
          char p[GUI_PATH_LEN];
          if (rf_ex_n < 6 && pick_folder(dlg, p, sizeof(p)))
          {
            int i, dup = 0, lib = 0;
            for (i = 0; i < rf_ex_n && !dup; i++) if (rf_same_path(rf_ex[i], p)) dup = 1;
            for (i = 0; i < rf_n && !lib; i++) if (rf_dir[i][0] && rf_same_path(rf_dir[i], p)) lib = 1;
            if (dup) { MessageBoxA(dlg, "That directory is already excluded.", "ROM Directories", MB_OK | MB_ICONINFORMATION); return TRUE; }
            if (lib)
            {
              MessageBoxA(dlg, "That is one of the ROM directories itself: excluding it would leave it empty. Remove it from the list above instead, or exclude a folder inside it.",
                          "ROM Directories", MB_OK | MB_ICONINFORMATION);
              return TRUE;
            }
            lstrcpynA(rf_ex[rf_ex_n++], p, GUI_PATH_LEN);
            rf_fill(dlg);
          }
          return TRUE;
        }
        case IDC_RF_EXREMOVE:
        {
          int sel = ListView_GetNextItem(GetDlgItem(dlg, IDC_RF_EXLIST), -1, LVNI_SELECTED), i;
          if (sel >= 0)
          {
            for (i = sel; i + 1 < rf_ex_n; i++) lstrcpyA(rf_ex[i], rf_ex[i + 1]);
            rf_ex_n--;
            rf_fill(dlg);
          }
          return TRUE;
        }
        case IDOK:
        {
          int i;

          lstrcpynA(gui.rom_dir, rf_dir[0], sizeof(gui.rom_dir));      /* (the first row can be removed or replaced by the next one) */
          gui.rom_extra_n = rf_n - 1;
          for (i = 1; i < rf_n; i++) lstrcpynA(gui.rom_extra[i - 1], rf_dir[i], GUI_PATH_LEN);
          gui.rom_exclude_n = rf_ex_n;
          for (i = 0; i < rf_ex_n; i++) lstrcpynA(gui.rom_exclude[i], rf_ex[i], GUI_PATH_LEN);
          EndDialog(dlg, IDOK);
          return TRUE;
        }
        case IDCANCEL:
          EndDialog(dlg, IDCANCEL);
          return TRUE;
      }
      break;
  }
  return FALSE;
}

void dlg_rom_folders(HWND parent)
{
  if (gui_dialog_box(gui.large_ui ? IDD_ROMFOLDERS_LARGE : IDD_ROMFOLDERS, parent, romfolders_proc) == IDOK)
  {
    config_save();
    browser_toolbar_update();
    if (panel_visible)
    {
      browser_rescan();
      browser_relayout();         /* with no directory left the Choose Folder button takes the place of the list */
      browser_panel_show(1);
    }
  }
}

/****************************************************************************
 * Message routing -- called from main.c's window procedure. Control IDs
 * here (1060+) never overlap the menu command range, so there's no
 * ambiguity in claiming them before the menu dispatch sees them.
 ****************************************************************************/

int browser_panel_handle_command(WORD id, WORD notify_code)
{
  switch (id)
  {
    case IDC_BROWSER_SEARCH_CLEAR:
      SetWindowTextA(panel_search, "");
      SetFocus(panel_search);
      return 1;

    case IDC_BROWSER_SEARCH:
      if (notify_code == EN_CHANGE)
      {
        browser_apply_filter();
        ShowWindow(panel_search_clear, (search_shown() && GetWindowTextLengthA(panel_search) > 0) ? SW_SHOWNA : SW_HIDE);
        SetWindowPos(panel_search_clear, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        InvalidateRect(panel_search_clear, NULL, TRUE);
      }

      /* A search bar brought up with Ctrl+F while it is switched off goes
         away again once it is left empty. */
      if (notify_code == EN_KILLFOCUS && search_override == 1 && GetWindowTextLengthA(panel_search) == 0)
      {
        search_override = -1;     /* left empty: back to what View says */
        search_autohide_tick = GetTickCount();
        browser_bars_changed();
      }
      return 1;

    case IDC_BROWSER_SEARCH_CLOSE:
      browser_toggle_searchbar();
      gui_update_menu();
      return 1;

    case IDC_BROWSER_FILTER_CONSOLE:
    case IDC_BROWSER_FILTER_REGION:
    case IDC_BROWSER_FILTER_FOLDER:
      if (notify_code == CBN_SELCHANGE)
      {
        filter_chosen(id - IDC_BROWSER_FILTER_CONSOLE);
        browser_apply_filter();
      }
      if (notify_code == CBN_CLOSEUP && panel_list) SetFocus(panel_list);       /* the choice is made: the focus (its outline) goes back to the game list, not left on the drop-down */
      return 1;

    case IDC_BROWSER_FILTER_RESET:
      filters_reset();
      browser_apply_filter();
      if (panel_list) SetFocus(panel_list);
      return 1;

    case IDC_TB_REFRESH:
      browser_rescan();
      return 1;

    case IDC_TB_PLAY:
      browser_launch_selected();
      return 1;

    case IDC_TB_FAV:
      browser_toggle_favorites_only();
      gui_update_menu();
      return 1;

    case IDC_BROWSER_CHOOSE:
      browser_panel_change_folder();
      return 1;
  }

  return 0;
}

/* Reflects the current sort column/direction as the little up/down arrow
   glyph in that column's header -- standard Explorer-list behavior,
   rendered by the header control itself (comctl32 v6, already required
   by this app's manifest) once told which column and which way via
   HDF_SORTUP/HDF_SORTDOWN. */
static void update_sort_arrow(void)
{
  HWND header = ListView_GetHeader(panel_list);
  int i;

  if (!header) return;

  for (i = 0; i < BROWSER_COLUMNS; i++)
  {
    HDITEMA hdi;
    hdi.mask = HDI_FORMAT;
    SendMessageA(header, HDM_GETITEMA, i, (LPARAM)&hdi);
    hdi.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
    if (i == gui.browser_sort_column)
      hdi.fmt |= gui.browser_sort_ascending ? HDF_SORTUP : HDF_SORTDOWN;
    else if (i == gui.browser_sort_extra[0])
      hdi.fmt |= gui.browser_sort_extra_asc[0] ? HDF_SORTUP : HDF_SORTDOWN;
    else if (i == gui.browser_sort_extra[1])
      hdi.fmt |= gui.browser_sort_extra_asc[1] ? HDF_SORTUP : HDF_SORTDOWN;
    SendMessageA(header, HDM_SETITEMA, i, (LPARAM)&hdi);
  }
}

/* Favorites ------------------------------------------------------------ */

static void toggle_favorite_row(int row)
{
  browser_entry_t *e;
  int on;

  e = row_entry(row);
  if (!e) return;
  on = !config_fav_has(e->full_path);
  if (!config_fav_set(e->full_path, on))
  {
    gui_status("The favorites list is full: remove one first");
    return;
  }
  e->fav = (signed char)on;
  config_save();
  gui_notify(on ? "Added to favorites: %s" : "Removed from favorites: %s", e->name);

  if (gui.favorites_only || gui.browser_sort_column == COL_FAV)
    browser_apply_filter();   /* the list changes: a game leaves it, or moves */
  else if (recent_dup)
    ListView_RedrawItems(panel_list, 0, filtered_count - 1);       /* the game may be in the list twice (Recently Played and All Games): both stars change */
  else
    ListView_RedrawItems(panel_list, row, row);
}

int browser_favorites_only(void)
{
  return gui.favorites_only;
}

void browser_rescan_now(void) { browser_rescan(); }

/* Clear Recently Played in the right-click menu (also in the game's right-click menu). */
void browser_clear_recent(void)
{
  if (MessageBoxA(g_hwnd, "Clear the list of recently played games?\n\nThis forgets when each game was last opened.",
                  "Clear Recently Played", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
  history_clear();
  browser_apply_filter();
  gui_status("Recently played list cleared");
}
void browser_redraw(void) { if (panel_list) InvalidateRect(panel_list, NULL, TRUE); }

/* The star on the toolbar / View > Game List > Show Favorites Only. */
void browser_refresh_names(void)
{
  browser_apply_filter();
}

void browser_toggle_favorites_only(void)
{
  gui.favorites_only = !gui.favorites_only;
  browser_apply_filter();
  config_save();
  if (gui.favorites_only)
  {
    int i, any = 0;
    for (i = 0; i < entry_count && !any; i++) any = entries[i].fav > 0;
    if (!any) gui_notify("No favorites yet. Right-click a game and choose Add to Favorites");
  }
}

/* The color Windows itself gives a list heading in the current theme (the original look). */
static COLORREF heading_theme_color(void)
{
  COLORREF c = 0;
  HTHEME th = OpenThemeData(panel_list, L"LISTVIEW");

  if (th)
  {
    /* LVP_GROUPHEADER = 6, LVGH_OPEN = 1, TMT_TEXTCOLOR = 3803 */
    if (SUCCEEDED(GetThemeColor(th, 6, 1, 3803, &c)))
    {
      CloseThemeData(th);
      return c;
    }
    CloseThemeData(th);
  }
  return theme_is_dark() ? RGB(60, 90, 190) : RGB(30, 50, 135);
}

/* The color of the headings (Recently Played, All Games, Favorites, the console names): the accent color chosen, in a shade that
   reads well on the list's background; with the Modern style and no accent, its blue; otherwise the system's own. */
static COLORREF heading_color(void)
{
  if (gui.accent) return theme_is_dark() ? theme_accent_edge() : theme_accent_bg();
  if (gui.ui_style) return theme_is_dark() ? RGB(0x6C, 0xAE, 0xFF) : theme_modern_accent();
  return heading_theme_color();
}

/* The "Recently Played" / "All Games" heading rows: drawn here so they follow the Large interface (a bigger version of the
   list's own font) and have a blue that is easy to read in both themes. */
static LRESULT heading_band_paint(HDC dc, RECT rc, int kind, int text_h)
{
  static HFONT hfont;
  static int hfont_h;
  RECT tr;
  HFONT base = (HFONT)SendMessage(panel_list, WM_GETFONT, 0, 0);
  LOGFONTA lf;
  COLORREF fg = heading_color();
  HBRUSH bg = CreateSolidBrush(ListView_GetBkColor(panel_list));
  HGDIOBJ of;
  HPEN pen, op;

  if (base && GetObjectA(base, sizeof(lf), &lf))
  {
    if (gui.large_ui) lf.lfHeight = lf.lfHeight * 12 / 10;   /* bigger only with the Large interface; otherwise the list's own size */
    if (!hfont || hfont_h != lf.lfHeight)
    {
      if (hfont) DeleteObject(hfont);
      lf.lfWidth = 0;
      hfont = CreateFontIndirectA(&lf);
      hfont_h = lf.lfHeight;
    }
  }
  FillRect(dc, &rc, bg);
  DeleteObject(bg);
  of = SelectObject(dc, hfont ? hfont : base);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, fg);
  tr = rc;
  tr.left += TBS(8);
  tr.bottom -= 2;
  if (text_h > 0 && rc.bottom - rc.top > text_h) tr.top = rc.bottom - text_h;      /* the grid's cell is tall: the title sits at its foot, over its covers */
  DrawTextA(dc, heading_label(kind), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
  SelectObject(dc, of);
  pen = CreatePen(PS_SOLID, 1, fg);
  op = SelectObject(dc, pen);
  MoveToEx(dc, rc.left + TBS(8), rc.bottom - 1, NULL);
  LineTo(dc, rc.right - TBS(8), rc.bottom - 1);
  SelectObject(dc, op);
  DeleteObject(pen);
  return CDRF_SKIPDEFAULT;
}

/* The program's logo as a premultiplied bitmap `dh` pixels high (the caller deletes it); *pw is its width. NULL if it cannot be made. */
HBITMAP gui_logo_bitmap(int dh, int *pw)
{
  HRSRC res = FindResourceA(NULL, MAKEINTRESOURCEA(IDR_LOGO), (LPCSTR)RT_RCDATA);
  const unsigned char *src = res ? (const unsigned char *)LockResource(LoadResource(NULL, res)) : NULL;
  unsigned char *img;
  HBITMAP bmp;
  int sw, sh, ch;

  if (!src) return NULL;
  img = stbi_load_from_memory(src, (int)SizeofResource(NULL, res), &sw, &sh, &ch, 4);
  if (!img) return NULL;
  bmp = gui_scale_rgba(img, sw, sh, dh, pw);
  stbi_image_free(img);
  return bmp;
}

static LRESULT heading_row_paint(NMLVCUSTOMDRAW *cd, int kind)
{
  return heading_band_paint(cd->nmcd.hdc, cd->nmcd.rc, kind, 0);
}

/* The exact cell of a row in a column: the column's left and right edges from the header (a
   subitem 0 label rectangle starts after its image, which left a gap in the lines) and the row's
   top and bottom. */
static int GetCellRect(HWND list, int row, int col, RECT *out)
{
  RECT ir, hr;
  HWND header = ListView_GetHeader(list);

  if (!header || !ListView_GetItemRect(list, row, &ir, LVIR_BOUNDS) || !Header_GetItemRect(header, col, &hr)) return 0;
  MapWindowPoints(header, list, (POINT *)&hr, 2);
  out->left = hr.left;
  out->right = hr.right;
  out->top = ir.top;
  out->bottom = ir.bottom;
  return 1;
}

/* A selected game of the grid in the accent color: the system draws its own highlight for the icon view whatever the item's
   state is set to, so the item is drawn here (cover and name over a rounded block in the accent color). */
static void grid_selected_paint(HDC dc, int row)
{
  RECT ib, ri, rl;
  LVITEM it;
  char text[260] = "";
  HIMAGELIST hil = ListView_GetImageList(panel_list, LVSIL_NORMAL);
  HBRUSH br = CreateSolidBrush(theme_accent_bg());
  HPEN pen = CreatePen(PS_SOLID, 1, theme_accent_edge());
  HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
  HFONT font = (HFONT)SendMessage(panel_list, WM_GETFONT, 0, 0), of = font ? (HFONT)SelectObject(dc, font) : NULL;
  int iw = 0, ih = 0;

  if (!ListView_GetItemRect(panel_list, row, &ib, LVIR_BOUNDS)) goto done;
  InflateRect(&ib, -2, -2);
  RoundRect(dc, ib.left, ib.top, ib.right, ib.bottom, TBS(10), TBS(10));

  ZeroMemory(&it, sizeof(it));
  it.mask = LVIF_IMAGE | LVIF_TEXT;
  it.iItem = row;
  it.pszText = text;
  it.cchTextMax = (int)sizeof(text);
  ListView_GetItem(panel_list, &it);

  if (hil && it.iImage >= 0 && ListView_GetItemRect(panel_list, row, &ri, LVIR_ICON))
  {
    ImageList_GetIconSize(hil, &iw, &ih);
    ImageList_Draw(hil, it.iImage, dc, ri.left + (ri.right - ri.left - iw) / 2, ri.top + (ri.bottom - ri.top - ih) / 2, ILD_NORMAL);
  }
  if (text[0] && ListView_GetItemRect(panel_list, row, &rl, LVIR_LABEL))
  {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextA(dc, text, -1, &rl, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
  }
done:
  if (of) SelectObject(dc, of);
  SelectObject(dc, ob); SelectObject(dc, op);
  DeleteObject(br); DeleteObject(pen);
}

/* Draws the star of a row over its (empty) Favorite cell. */
LRESULT browser_list_customdraw(NMHDR *hdr)
{
  NMLVCUSTOMDRAW *cd = (NMLVCUSTOMDRAW *)hdr;

  if (!hdr || hdr->code != NM_CUSTOMDRAW || !panel_list || hdr->hwndFrom != panel_list) return -1;
  if (gui.browser_grid_view)
  {
    /* Frame Around Cover and Name: each game's frame is drawn right after the game itself, when that item is painted, so only
       the items that need painting get one (nothing walks through the whole list on every paint), and it is there whether or not
       the cover has arrived yet (an empty card until it does). Headings and empty cells are drawn by the code below. */
    if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return (grid_headed || gui.cover_card || gui.accent || gui.ui_style) ? CDRF_NOTIFYITEMDRAW : CDRF_DODEFAULT;
    if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
    {
      /* A heading is drawn across its whole row, and an empty cell shows that same row's heading where it covers it. */
      int row = (int)cd->nmcd.dwItemSpec, head = row;
      if (row >= 0 && row < filtered_count && filtered_index[row] >= 0 && (gui.accent || gui.ui_style) &&
          (ListView_GetItemState(panel_list, row, LVIS_SELECTED) & LVIS_SELECTED))
      {
        grid_selected_paint(cd->nmcd.hdc, row);
        return gui.cover_card ? (CDRF_SKIPDEFAULT | CDRF_NOTIFYPOSTPAINT) : CDRF_SKIPDEFAULT;
      }
      if (row < 0 || row >= filtered_count || filtered_index[row] >= 0) return gui.cover_card ? CDRF_NOTIFYPOSTPAINT : CDRF_DODEFAULT;
      while (head > 0 && filtered_index[head] == ROW_PAD) head--;
      if (filtered_index[head] < 0 && filtered_index[head] != ROW_PAD && row - head < grid_built_cols)      /* only the cells of the heading's own row; the empty cells after the games stay blank */
      {
        RECT hr, client;
        if (ListView_GetItemRect(panel_list, head, &hr, LVIR_BOUNDS))
        {
          GetClientRect(panel_list, &client);
          hr.left = 0; hr.right = client.right;
          if (hr.top > cd->nmcd.rc.bottom || hr.bottom < cd->nmcd.rc.top) return CDRF_SKIPDEFAULT;
          {
            return heading_band_paint(cd->nmcd.hdc, hr, filtered_index[head], TBS(34));
          }
        }
      }
      return CDRF_SKIPDEFAULT;
    }
    if (cd->nmcd.dwDrawStage == CDDS_ITEMPOSTPAINT && gui.cover_card)
    {
      static HPEN frame_pen;
      static int frame_dark = -1;
      RECT ri;
      int row = (int)cd->nmcd.dwItemSpec, cx, cell_w = gui.browser_grid_size + 24 + 8, cell_h = gui.browser_grid_size + 34 + 16;
      HGDIOBJ op, ob;

      if (row < 0 || row >= filtered_count || filtered_index[row] < 0 || !ListView_GetItemRect(panel_list, row, &ri, LVIR_ICON)) return CDRF_DODEFAULT;
      if (!frame_pen || frame_dark != theme_is_dark())
      {
        if (frame_pen) DeleteObject(frame_pen);
        frame_dark = theme_is_dark();
        frame_pen = CreatePen(PS_SOLID, 1, frame_dark ? RGB(0x5A, 0x5A, 0x5A) : RGB(0xB8, 0xB8, 0xB8));
      }
      op = SelectObject(cd->nmcd.hdc, frame_pen);
      ob = SelectObject(cd->nmcd.hdc, GetStockObject(NULL_BRUSH));
      cx = (ri.left + ri.right) / 2;
      RoundRect(cd->nmcd.hdc, cx - (cell_w - 8) / 2, ri.top - 6, cx + (cell_w - 8) / 2, ri.top - 6 + cell_h - 8, TBS(10), TBS(10));
      SelectObject(cd->nmcd.hdc, ob);
      SelectObject(cd->nmcd.hdc, op);
    }
    return CDRF_DODEFAULT;
  }

  switch (cd->nmcd.dwDrawStage)
  {
    case CDDS_PREPAINT:
      return CDRF_NOTIFYITEMDRAW;

    case CDDS_ITEMPREPAINT:
      {
        int row = (int)cd->nmcd.dwItemSpec;
        if (row >= 0 && row < filtered_count && filtered_index[row] < 0) return heading_row_paint(cd, filtered_index[row]);
      }
      return CDRF_NOTIFYSUBITEMDRAW;

    case CDDS_SUBITEM | CDDS_ITEMPREPAINT:
      /* An accent color chosen: the selected row is drawn in it (Windows would use its own color). */
      if ((gui.accent || gui.ui_style) && (ListView_GetItemState(panel_list, (int)cd->nmcd.dwItemSpec, LVIS_SELECTED) & LVIS_SELECTED))
      {
        RECT cell;
        if (GetCellRect(panel_list, (int)cd->nmcd.dwItemSpec, cd->iSubItem, &cell))
        {
          HBRUSH ab = CreateSolidBrush(theme_accent_bg());
          FillRect(cd->nmcd.hdc, &cell, ab);
          DeleteObject(ab);
        }
        cd->clrTextBk = theme_accent_bg();
        cd->clrText = RGB(255, 255, 255);
        cd->nmcd.uItemState &= ~(UINT)(CDIS_SELECTED | CDIS_FOCUS);
        return CDRF_NEWFONT | CDRF_NOTIFYPOSTPAINT;
      }
      /* Light theme: the selection highlight of the Name column starts at its text, leaving a white gap before it (where
         the row's small image would be). That strip is filled with the highlight's color first. */
      if (cd->iSubItem == 0 && !theme_is_dark())
      {
        int row = (int)cd->nmcd.dwItemSpec;
        RECT cell, lab;
        if ((ListView_GetItemState(panel_list, row, LVIS_SELECTED) & LVIS_SELECTED) &&
            GetCellRect(panel_list, row, 0, &cell) && ListView_GetItemRect(panel_list, row, &lab, LVIR_LABEL) && lab.left > cell.left)
        {
          HBRUSH hb = CreateSolidBrush(GetSysColor(GetFocus() == panel_list ? COLOR_HIGHLIGHT : COLOR_BTNFACE));
          cell.right = lab.left;
          FillRect(cd->nmcd.hdc, &cell, hb);
          DeleteObject(hb);
        }
      }
      return CDRF_NOTIFYPOSTPAINT;

    case CDDS_SUBITEM | CDDS_ITEMPOSTPAINT:
      /* Thin lines after each column (not after the last one), always; View > Game List > Show Grid Lines adds them under each row. */
      {
        RECT cr;
        int col = cd->iSubItem, w = ListView_GetColumnWidth(panel_list, col), c, last = -1;
        int order[BROWSER_COLUMNS], ncol = Header_GetItemCount(ListView_GetHeader(panel_list));

        /* "Last" is the last one on screen, which is not the highest number once columns have been moved. */
        if (ncol > BROWSER_COLUMNS) ncol = BROWSER_COLUMNS;
        if (ncol > 0 && ListView_GetColumnOrderArray(panel_list, ncol, order))
        {
          for (c = 0; c < ncol; c++) if (ListView_GetColumnWidth(panel_list, order[c]) > 0) last = order[c];
        }
        else
          for (c = 0; c < BROWSER_COLUMNS; c++) if (ListView_GetColumnWidth(panel_list, c) > 0) last = c;
        if (w > 0 && GetCellRect(panel_list, (int)cd->nmcd.dwItemSpec, col, &cr))
        {
          HPEN pen = CreatePen(PS_SOLID, 1, theme_is_dark() ? RGB(0x3A, 0x3A, 0x3A) : RGB(0xDD, 0xDD, 0xDD));
          HGDIOBJ op = SelectObject(cd->nmcd.hdc, pen);
          if (col != last)       /* after the column */
          {
            MoveToEx(cd->nmcd.hdc, cr.right - 1, cr.top, NULL);
            LineTo(cd->nmcd.hdc, cr.right - 1, cr.bottom);
          }
          if (gui.browser_gridlines)   /* View > Game List > Show Grid Lines: and under the row */
          {
            MoveToEx(cd->nmcd.hdc, cr.left, cr.bottom - 1, NULL);
            LineTo(cd->nmcd.hdc, cr.right, cr.bottom - 1);
          }
          SelectObject(cd->nmcd.hdc, op);
          DeleteObject(pen);
        }
      }
      if (cd->iSubItem == COL_FAV && !col_hidden[COL_FAV])
      {
        int row = (int)cd->nmcd.dwItemSpec, sz = TBS(16);
        RECT rc;

        if (row_entry(row) && ListView_GetSubItemRect(panel_list, row, COL_FAV, LVIR_BOUNDS, &rc))
        {
          int on = row_entry(row)->fav;
          COLORREF c = on ? (gui.color_icons ? tb_icon_color(TI_FAVON)                 /* Colorful Icons: a gold star */
                                              : (theme_is_dark() ? RGB(255, 255, 255) : RGB(0, 0, 0)))   /* a solid star in the text color */
                          : (theme_is_dark() ? RGB(120, 120, 120) : RGB(150, 150, 150));
          tb_paint_icon(cd->nmcd.hdc, rc.left + (rc.right - rc.left - sz) / 2, rc.top + (rc.bottom - rc.top - sz) / 2,
                        sz, on ? TI_FAVON : TI_FAV, c);
        }
      }
      return CDRF_DODEFAULT;
  }
  return CDRF_DODEFAULT;
}

int browser_panel_handle_notify(NMHDR *hdr)
{
  /* Grid view asks for each visible entry's picture as it draws it. */
  if (hdr->idFrom == IDC_BROWSER_LIST &&
      (hdr->code == LVN_GETDISPINFOA || hdr->code == LVN_GETDISPINFOW))
  {
    NMLVDISPINFOA *di = (NMLVDISPINFOA *)hdr;

    if ((di->item.mask & LVIF_IMAGE) && grid_imagelist &&
        di->item.iItem >= 0 && di->item.iItem < filtered_count)
    {
      di->item.iImage = row_entry(di->item.iItem) ? cover_image_index(row_entry(di->item.iItem)) : I_IMAGENONE;
    }
    return 1;
  }

  if (hdr->idFrom == IDC_BROWSER_LIST && hdr->code == LVN_ITEMCHANGED)
  {
    NMLISTVIEW *nm = (NMLISTVIEW *)hdr;
    if (nm->uChanged & LVIF_STATE) browser_toolbar_update();
    if ((nm->uChanged & LVIF_STATE) && (nm->uNewState & LVIS_SELECTED) && !(nm->uOldState & LVIS_SELECTED) &&
        nm->iItem >= 0 && nm->iItem < filtered_count)
    {
      static int last_sel = -1;
      if (filtered_index[nm->iItem] < 0)
      {
        /* A heading cannot be picked: carry on to the game beyond it, in the direction of travel. */
        int dir = (nm->iItem < last_sel) ? -1 : 1, to = nm->iItem + dir;
        while (to >= 0 && to < filtered_count && filtered_index[to] < 0) to += dir;       /* past the empty cells of the grid too */
        if (to < 0 || to >= filtered_count)
        {
          to = nm->iItem - dir;
          while (to >= 0 && to < filtered_count && filtered_index[to] < 0) to -= dir;
        }
        if (to >= 0 && to < filtered_count && filtered_index[to] >= 0)
        {
          ListView_SetItemState(panel_list, to, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
          ListView_EnsureVisible(panel_list, to, FALSE);
        }
      }
      else last_sel = nm->iItem;
    }
    if ((nm->uChanged & LVIF_STATE) && ((nm->uNewState ^ nm->uOldState) & LVIS_SELECTED)) gui_update_menu();   /* File > ROM Information follows the selection */
    return 0;
  }

  /* A click on a star toggles that game. */
  if (hdr->idFrom == IDC_BROWSER_LIST && hdr->code == NM_CLICK && !gui.browser_grid_view)
  {
    LVHITTESTINFO hit;
    ZeroMemory(&hit, sizeof(hit));
    GetCursorPos(&hit.pt);
    ScreenToClient(panel_list, &hit.pt);
    if (ListView_SubItemHitTest(panel_list, &hit) >= 0 && hit.iSubItem == COL_FAV && !col_hidden[COL_FAV])
    {
      toggle_favorite_row(hit.iItem);
      return 1;
    }
    return 0;
  }

  if (hdr->idFrom == IDC_BROWSER_LIST && hdr->code == NM_DBLCLK)
  {
    /* A double click on the star is two toggles (the first click was handled as NM_CLICK), not "play". */
    LVHITTESTINFO hit;
    ZeroMemory(&hit, sizeof(hit));
    GetCursorPos(&hit.pt);
    ScreenToClient(panel_list, &hit.pt);
    if (!gui.browser_grid_view && ListView_SubItemHitTest(panel_list, &hit) >= 0 &&
        hit.iSubItem == COL_FAV && !col_hidden[COL_FAV])
    {
      toggle_favorite_row(hit.iItem);
      return 1;
    }
    browser_launch_selected();
    return 1;
  }

  if (hdr->code == HDN_ITEMCLICKA || hdr->code == HDN_ITEMCLICKW)
  {
    NMHEADERA *nmh = (NMHEADERA *)hdr;
    int col = nmh->iItem;

    if (col < 0 || col >= BROWSER_COLUMNS) return 0;

    if (GetKeyState(VK_SHIFT) & 0x8000)
    {
      /* Shift+click: a column joins the sort as its 2nd or 3rd key (a click on one already in it flips its direction). */
      if (col == gui.browser_sort_column)
        gui.browser_sort_ascending = !gui.browser_sort_ascending;
      else if (col == gui.browser_sort_extra[0])
        gui.browser_sort_extra_asc[0] = !gui.browser_sort_extra_asc[0];
      else if (col == gui.browser_sort_extra[1])
        gui.browser_sort_extra_asc[1] = !gui.browser_sort_extra_asc[1];
      else
      {
        int slot = gui.browser_sort_extra[0] < 0 ? 0 : 1;   /* a 4th replaces the 3rd */
        gui.browser_sort_extra[slot] = col;
        gui.browser_sort_extra_asc[slot] = 1;
      }
    }
    else if (col == gui.browser_sort_column)
      gui.browser_sort_ascending = !gui.browser_sort_ascending;
    else
    {
      gui.browser_sort_column = col;
      gui.browser_sort_ascending = 1;
      gui.browser_sort_extra[0] = gui.browser_sort_extra[1] = -1;   /* a plain click starts a new sort */
    }

    update_sort_arrow();
    browser_apply_filter();
    config_save();
    gui_update_menu();
    return 1;
  }

  if (hdr->code == HDN_ENDTRACKA || hdr->code == HDN_ENDTRACKW)
  {
    NMHEADERA *nmh = (NMHEADERA *)hdr;
    int col = nmh->iItem;
    int new_width;

    if (col < 0 || col >= BROWSER_COLUMNS || col_hidden[col] || last_list_w <= 0)
      return 0;

    new_width = (int)SendMessage(panel_list, LVM_GETCOLUMNWIDTH, (WPARAM)col, 0);
    if (col >= COL_FAV)
    {
      gui.browser_col_w[col] = new_width > 20 ? new_width : 20;    /* kept in pixels, like the person left it */
      config_save();
      return 0;
    }
    {
      int pct = (new_width * 100) / list_pct_base();
      if (pct < 5) pct = 5;
      if (pct > 90) pct = 90;
      gui.browser_col_pct[col] = pct;
    }
    config_save();
    return 0;   /* FALSE allows the resize the user just made to actually take effect */
  }

  return 0;
}

/* WM_CONTEXTMENU reports the ListView itself as its target for a header
   right-click too, not the header's own child handle -- confirmed by
   direct testing, not assumed. Checking whether the click coordinates
   actually land inside the header's screen rectangle is what reliably
   tells the two apart. */
/* Extracts the base name (no directory, no extension) the same way
   coverart.c and main.c's rom_base both do, for locating this ROM's
   save state files without needing it to be the currently-loaded one. */
static void guess_base_name(const char *path, char *out, int out_len)
{
  const char *slash = strrchr(path, '\\');
  const char *name = slash ? slash + 1 : path;
  const char *dot = strrchr(name, '.');
  int len = dot ? (int)(dot - name) : (int)lstrlenA(name);
  if (len >= out_len) len = out_len - 1;
  memcpy(out, name, (size_t)len);
  out[len] = '\0';
}

/* Loads this ROM only if it isn't already the one running -- ROM
   Information and Edit Cheats both need a loaded ROM to have anything
   meaningful to show, but there's no reason to interrupt/reset a game
   already in progress if the user right-clicked the same entry.
   When nothing at all is running, uses the non-disruptive peek (no
   audio, no system reset, browser stays visible) rather than actually
   launching the game just to look at its header or cheat list -- a
   full load is only used as a last resort, when a genuinely different
   ROM is already mid-game and there's no safe way to inspect this one
   without touching the memory the running game is using. */
static void ensure_rom_loaded(const char *path)
{
  if (emu_running && lstrcmpiA(emu_rom_path(), path) == 0) return;
  if (!emu_running && emu_peek_rom(path)) return;
  emu_load_rom(path);
}

/* The menu of the game list: of a game (item_index is its row), or, with -1, of the empty space (what belongs to a game is grayed). */
static void show_item_context_menu(int screen_x, int screen_y, int item_index)
{
  HMENU menu, state_menu;
  int cmd, i;
  char path[GUI_PATH_LEN];
  char base[160];
  char cover_path[GUI_PATH_LEN];
  char console[40];
  int has_cover;
  int peeked;
  int game = row_entry(item_index) != NULL;       /* 0: the click was on empty space (or on the title of a group of games) */
  UINT off = game ? 0 : (UINT)MF_GRAYED;

  path[0] = console[0] = base[0] = '\0';
  if (game)
  {
    lstrcpynA(path, row_entry(item_index)->full_path, sizeof(path));
    lstrcpynA(console, row_entry(item_index)->console, sizeof(console));
    guess_base_name(path, base, sizeof(base));
  }

  menu = CreatePopupMenu();
  state_menu = CreatePopupMenu();

  /* Save states are named after the game's data, so the game has to be
     inspected (without starting it) to find them. Nothing is running while
     the browser is shown. If it cannot be inspected, fall back to the plain
     name older versions used. */
  peeked = (game && !emu_running && emu_peek_rom(path));

  for (i = 0; game && i < GUI_SLOT_MAX; i++)
  {
    char state_file[GUI_PATH_LEN];
    char label[64], stamp[32];
    UINT flags = MF_STRING;

    if (peeked) state_path(i, state_file, sizeof(state_file));
    else        gui_snprintf(state_file, (int)sizeof(state_file), "%s\\%s.gp%d", osd_path("States"), base, i);
    if (state_file_time(state_file, stamp, sizeof(stamp)))
    {
      wsprintfA(label, "Slot %d - %s", i, stamp);
    }
    else
    {
      flags |= MF_GRAYED;
      wsprintfA(label, "Slot %d - Empty", i);
    }
    AppendMenuA(state_menu, flags, (UINT_PTR)(IDM_BROWSER_PLAY_STATE_BASE + i), label);
  }

  has_cover = game && coverart_find(path, console, cover_path, sizeof(cover_path));

  AppendMenuA(menu, MF_STRING | off, IDM_BROWSER_PLAY, "Play Game");
  AppendMenuA(menu, MF_STRING | MF_POPUP | off, (UINT_PTR)state_menu, "Play Game with State");
  AppendMenuA(menu, MF_STRING | off, IDM_BROWSER_FAVORITE,
              game && config_fav_has(path) ? "Remove from Favorites" : "Add to Favorites");
  AppendMenuA(menu, MF_STRING | (game && history_last(path) ? 0 : (UINT)MF_GRAYED), IDM_BROWSER_REMOVE_RECENT, "Remove from Recently Played");   /* this game is not in the list (or no game): grayed */
  AppendMenuA(menu, MF_STRING | (history_count() > 0 ? 0 : (UINT)MF_GRAYED), IDM_BROWSER_CLEAR_RECENT, "Clear Recently Played");   /* nothing to clear: grayed */
  AppendMenuA(menu, MF_STRING | off, IDM_BROWSER_SHORTCUT_DESKTOP, "Create Desktop Shortcut");
  AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
  {
    /* With more than one ROM folder the item opens a list of them. */
    int nfold = 0, k;
    char fold[7][GUI_PATH_LEN];

    if (gui.rom_dir[0]) lstrcpynA(fold[nfold++], gui.rom_dir, GUI_PATH_LEN);
    for (k = 0; k < gui.rom_extra_n && k < 6; k++)
      if (gui.rom_extra[k][0]) lstrcpynA(fold[nfold++], gui.rom_extra[k], GUI_PATH_LEN);
    if (nfold > 1)
    {
      HMENU folders = CreatePopupMenu();

      for (k = 0; k < nfold; k++)
      {
        char label[GUI_PATH_LEN];
        lstrcpynA(label, fold[k], sizeof(label));
        PathCompactPathExA(label, fold[k], 64, 0);                      /* a long path is shortened in the middle */
        AppendMenuA(folders, MF_STRING, (UINT_PTR)(IDM_BROWSER_OPENDIR_BASE + k), label);
      }
      AppendMenuA(menu, MF_STRING | MF_POPUP, (UINT_PTR)folders, "Open ROM Directory");
      {
        MENUITEMINFOA mii;                      /* a command number for the item that opens the list, so that it can be given the same picture */
        ZeroMemory(&mii, sizeof(mii));
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_ID;
        mii.wID = IDM_BROWSER_OPENDIR;
        SetMenuItemInfoA(menu, (UINT)(GetMenuItemCount(menu) - 1), TRUE, &mii);
      }
      if (theme_menus_owner()) theme_ownerdraw_menu(folders);
    }
    else AppendMenuA(menu, MF_STRING, IDM_BROWSER_OPENDIR, "Open ROM Directory");
  }
  AppendMenuA(menu, MF_STRING, IDM_BROWSER_REFRESH, "Refresh ROM Directory");
  AppendMenuA(menu, MF_STRING, IDM_BROWSER_CHANGE_DIR, "Change ROM Directory");
  AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
  AppendMenuA(menu, MF_STRING | off, IDM_BROWSER_ROMINFO, "ROM Information");
  AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
  AppendMenuA(menu, MF_STRING | off, IDM_BROWSER_CHEATS, "Edit Cheats");
  AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
  AppendMenuA(menu, MF_STRING | off, IDM_BROWSER_SET_COVER, "Change Cover Image");
  AppendMenuA(menu, MF_STRING | (has_cover ? 0 : (UINT)MF_GRAYED), IDM_BROWSER_REMOVE_COVER, "Remove Cover Image");

  /* Same owner-draw system the main menu bar uses for large_ui -- these
     are standard WM_MEASUREITEM/WM_DRAWITEM messages, delivered to
     g_hwnd regardless of which menu they originate from, so the
     handlers already wired up for the main menu pick these up with no
     extra plumbing. Dark/light coloring for a *non*-owner-drawn popup
     (large_ui off) is handled by Windows itself via the system theme
     this app already sets up; nothing extra is needed here for that
     case. */
  if (theme_menus_owner())
  {
    theme_ownerdraw_menu(menu);
    theme_ownerdraw_menu(state_menu);
  }

  /* A small picture beside each action. */
  {
    static const struct { UINT id; unsigned short glyph; } pics[] =
    {
      { IDM_BROWSER_PLAY, 0xE768 }, { IDM_BROWSER_FAVORITE, 0xE734 }, { IDM_BROWSER_REMOVE_RECENT, 0xE74D }, { IDM_BROWSER_CLEAR_RECENT, 0xE75C },
      { IDM_BROWSER_SHORTCUT_DESKTOP, 0xE71B }, { IDM_BROWSER_OPENDIR, 0xE838 }, { IDM_BROWSER_REFRESH, 0xFFF1 }, { IDM_BROWSER_CHANGE_DIR, 0xE8B7 },
      { IDM_BROWSER_ROMINFO, 0xE946 }, { IDM_BROWSER_CHEATS, 0xE8EC }, { IDM_BROWSER_SET_COVER, 0xE8B9 }, { IDM_BROWSER_REMOVE_COVER, 0xE738 }
    };
    int pi;
    for (pi = 0; pi < (int)(sizeof(pics) / sizeof(pics[0])); pi++) theme_menu_icon(menu, pics[pi].id, pics[pi].glyph);
  }

  SetForegroundWindow(g_hwnd);
  cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                       screen_x, screen_y, 0, g_hwnd, NULL);
  DestroyMenu(menu);   /* also destroys state_menu, since it's attached as a submenu */
  PostMessage(g_hwnd, WM_NULL, 0, 0);

  if (cmd == 0) return;

  if (game) ListView_SetItemState(panel_list, item_index, LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);

  if (cmd == IDM_BROWSER_PLAY)
  {
    emu_load_rom(path);
  }
  else if (cmd >= IDM_BROWSER_PLAY_STATE_BASE && cmd < IDM_BROWSER_PLAY_STATE_BASE + GUI_SLOT_MAX)
  {
    int slot = cmd - IDM_BROWSER_PLAY_STATE_BASE;
    emu_load_rom(path);
    emu_load_state(slot);
  }
  else if (cmd == IDM_BROWSER_FAVORITE)
  {
    toggle_favorite_row(item_index);
  }
  else if (cmd == IDM_BROWSER_REFRESH)
  {
    browser_rescan();
  }
  else if (cmd == IDM_BROWSER_SHORTCUT_DESKTOP)
  {
    if (extras_make_shortcut(path, 0)) gui_status("Shortcut created");
    else MessageBoxA(g_hwnd, "The shortcut could not be created.", APP_NAME, MB_OK | MB_ICONWARNING);
  }
  else if (cmd == IDM_BROWSER_OPENDIR)
  {
    if (gui.rom_dir[0]) ShellExecuteA(g_hwnd, "open", gui.rom_dir, NULL, NULL, SW_SHOWNORMAL);
  }
  else if (cmd >= IDM_BROWSER_OPENDIR_BASE && cmd < IDM_BROWSER_OPENDIR_BASE + 7)
  {
    /* the same list as the menu was made from: the ROM directory, then the extra folders that are filled in */
    int want = cmd - IDM_BROWSER_OPENDIR_BASE, n = 0, k;
    const char *dir = NULL;

    if (gui.rom_dir[0]) { if (n++ == want) dir = gui.rom_dir; }
    for (k = 0; k < gui.rom_extra_n && k < 6; k++)
      if (gui.rom_extra[k][0] && n++ == want) dir = gui.rom_extra[k];
    if (dir) ShellExecuteA(g_hwnd, "open", dir, NULL, NULL, SW_SHOWNORMAL);
  }
  else if (cmd == IDM_BROWSER_REMOVE_RECENT)
  {
    history_remove(path);
    browser_apply_filter();
    gui_status("Removed from the recently played list");
  }
  else if (cmd == IDM_BROWSER_CLEAR_RECENT)
  {
    browser_clear_recent();
  }
  else if (cmd == IDM_BROWSER_CHANGE_DIR)
  {
    browser_panel_change_folder();
  }
  else if (cmd == IDM_BROWSER_ROMINFO)
  {
    ensure_rom_loaded(path);
    dlg_rom_info(g_hwnd);
  }
  else if (cmd == IDM_BROWSER_CHEATS)
  {
    ensure_rom_loaded(path);
    dlg_cheats(g_hwnd);
  }
  else if (cmd == IDM_BROWSER_SET_COVER)
  {
    char picked[GUI_PATH_LEN];
    char covers_dir[GUI_PATH_LEN];
    picked[0] = '\0';

    /* Always start in ./covers (next to the exe); created on demand in
       case it was deleted while the app was running. */
    CreateDirectoryA(osd_path("Covers"), NULL);
    lstrcpynA(covers_dir, osd_path("Covers"), sizeof(covers_dir));

    if (gui_pick_file(g_hwnd, GUI_PICK_COVER, "Choose Cover Image",
                      "Image Files\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\0All Files\0*.*\0\0",
                      covers_dir, picked, sizeof(picked)))
    {
      coverart_set(path, console, picked);
      browser_grid_refresh_cover(item_index);
    }
  }
  else if (cmd == IDM_BROWSER_REMOVE_COVER)
  {
    coverart_remove(path, console);
    browser_grid_refresh_cover(item_index);
  }
}

int browser_panel_handle_contextmenu(HWND target, int x, int y)
{
  HWND header;
  RECT header_rect;
  POINT pt;
  LVHITTESTINFO hit;

  if (!panel_visible || !panel_list) return 0;
  if (target != panel_list) return 0;

  header = ListView_GetHeader(panel_list);
  if (header)
  {
    GetWindowRect(header, &header_rect);
    if (x >= header_rect.left && x < header_rect.right &&
        y >= header_rect.top  && y < header_rect.bottom)
    {
      show_column_menu(x, y);
      return 1;
    }
  }

  /* Not on the header -- check whether it landed on a row (client
     coordinates, since that's what LVM_HITTEST expects) and show the
     per-item menu if so. A right-click on empty list space shows the same
     menu with what belongs to a game grayed (the folder items and Clear
     Recently Played still work), not the menu of whatever game was
     selected before. */
  if (x == -1 && y == -1)
  {
    /* Keyboard-triggered (Shift+F10 / the Menu key) rather than an
       actual mouse click -- Windows deliberately sends (-1,-1) here to
       mean "you pick a sensible spot", based on the current selection
       rather than any cursor position. */
    int sel = ListView_GetNextItem(panel_list, -1, LVNI_SELECTED);
    RECT item_rect;

    if (sel < 0)                              /* no game selected: the menu of the empty space, at the top of the list */
    {
      pt.x = 8;
      pt.y = 8;
      ClientToScreen(panel_list, &pt);
      show_item_context_menu(pt.x, pt.y, -1);
      return 1;
    }
    ListView_GetItemRect(panel_list, sel, &item_rect, LVIR_LABEL);
    pt.x = item_rect.left + 8;
    pt.y = (item_rect.top + item_rect.bottom) / 2;
    ClientToScreen(panel_list, &pt);

    show_item_context_menu(pt.x, pt.y, sel);
    return 1;
  }

  pt.x = x;
  pt.y = y;
  ScreenToClient(panel_list, &pt);
  hit.pt = pt;
  if (ListView_HitTest(panel_list, &hit) < 0 || !(hit.flags & LVHT_ONITEM))
  {
    show_item_context_menu(x, y, -1);           /* empty space (or a group's title): the menu without a game */
    return 1;
  }

  show_item_context_menu(x, y, hit.iItem);
  return 1;
}

HBRUSH browser_panel_ctlcolor(HWND ctrl, HDC hdc)
{
  if (ctrl == panel_search_label)
  {
    if (theme_is_dark()) return theme_ctlcolor(hdc);

    SetBkColor(hdc, GetSysColor(COLOR_BTNFACE));
    SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));
    return GetSysColorBrush(COLOR_BTNFACE);
  }

  if (ctrl != panel_search && ctrl != panel_search_clear) return NULL;   /* the x looks like part of the box */

  if (theme_is_dark())
    return theme_ctlcolor_custom(hdc, RGB(0x27, 0x27, 0x27), 0);

  SetBkMode(hdc, OPAQUE);
  SetBkColor(hdc, GetSysColor(COLOR_WINDOW));
  SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
  return (HBRUSH)(COLOR_WINDOW + 1);
}

/* Enter, pressed anywhere in the panel (including the search box), plays
   the selected -- or first, if nothing is explicitly selected -- entry. */
int browser_panel_handle_return(void)
{
  HWND f;

  if (!panel_visible) return 0;

  /* Enter plays the game only from the list or the search box. From the
     toolbar, the filter drop-downs and the bar's buttons it belongs to that
     control (pick an item, press the button). */
  f = GetFocus();
  if (f && f != panel_list && f != panel_search && !IsChild(panel_list, f) && f != g_hwnd)
    return 0;

  if (ListView_GetNextItem(panel_list, -1, LVNI_SELECTED) < 0 && filtered_count > 0)
  {
    int first = 0;
    while (first < filtered_count - 1 && filtered_index[first] < 0) first++;
    ListView_SetItemState(panel_list, first, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
  }
  browser_launch_selected();
  return 1;
}

/****************************************************************************
 * Toolbar, search bar and filter bar: called from main.c
 ****************************************************************************/

/* Brings the bars in line with the View menu's switches and lays out again. */
void browser_bars_changed(void)
{
  if (!panel_bar) return;

  if (!search_shown()) SetWindowTextA(panel_search, "");
  if (gui.hide_filterbar) filters_reset();      /* a hidden filter bar goes back to showing everything */
  show_bars();
  browser_toolbar_layout();
  browser_relayout();
  if (panel_visible) browser_apply_filter();
}

/* Ctrl+F: shows the search bar if it is hidden, and puts the cursor in it. */
void browser_focus_search(void)
{
  if (!panel_visible || !gui.rom_dir[0]) return;

  /* The click on the toolbar's search button took the focus away and so already hid the bar: that click is the "hide". */
  if (!search_shown() && search_autohide_tick && GetTickCount() - search_autohide_tick < 500)
  {
    POINT pt;
    RECT br;
    if (tb_search && GetCursorPos(&pt) && GetWindowRect(tb_search, &br) && PtInRect(&br, pt)) return;   /* that click: only a click on this button counts */
  }

  if (!search_shown())
  {
    search_override = 1;
    browser_bars_changed();
  }
  SetFocus(panel_search);
  SendMessage(panel_search, EM_SETSEL, 0, -1);
}

/* The toolbar's tooltips name the shortcut that is set now (Input > Hotkeys can change them). */
static void tb_tip_keys(HWND b, const char *text, int cmd)
{
  char keys[96], tip[160];

  if (!b) return;
  hotkey_menu_keys(cmd, keys, sizeof(keys));
  if (keys[0]) gui_snprintf(tip, sizeof(tip), "%s (%s)", text, keys);
  else lstrcpynA(tip, text, sizeof(tip));
  tb_set_tip(b, tip);
}

void browser_toolbar_tips(void)
{
  if (!panel_toolbar) return;
  tb_tip_keys(tb_open,   "Open ROM", IDM_FILE_OPEN);
  tb_tip_keys(tb_search, "Search the Game List", IDM_VIEW_FINDSEARCH);
  tb_tip_keys(tb_pause,  "Pause or Resume the Game", IDM_EMU_PAUSE);
  tb_tip_keys(tb_stop,   "Stop the Game and Go Back to the List", IDM_EMU_STOP);
  tb_tip_keys(tb_reset,  "Reset the Game", IDM_EMU_RESET);
  tb_tip_keys(tb_fs,     "Fullscreen On or Off", IDM_VIDEO_FULLSCREEN);
  tb_tip_keys(tb_shot,   "Save a Screenshot", IDM_FILE_SCREENSHOT);
}

/* The pressed look of the toolbar's switches. */
void browser_toolbar_update(void)
{
  if (!tb_fs) return;

  tb_set_checked(tb_fs, gui.fullscreen);
  EnableWindow(tb_fs, emu_running);   /* it switches the running game in and out of fullscreen */
  tb_set_checked(tb_fav, gui.favorites_only);
  tb_set_checked(tb_search, browser_search_shown());

  /* These act on the running game, so they are grayed while there is none. */
  /* Play needs a game picked in the list. */
  EnableWindow(tb_play, !emu_running && panel_list && ListView_GetSelectedCount(panel_list) > 0);

  /* Refresh and Favorites Only are about the game list: nothing to do until a ROM folder is set, or while a game runs. */
  EnableWindow(tb_refresh, !emu_running && gui.rom_dir[0] != 0);
  EnableWindow(tb_fav, !emu_running && gui.rom_dir[0] != 0);
  EnableWindow(tb_search, !emu_running);
  EnableWindow(tb_stop, emu_running);
  EnableWindow(tb_reset, emu_running);
  EnableWindow(tb_pause, emu_running);
  EnableWindow(tb_shot, emu_running);
  tb_set_checked(tb_pause, emu_running && emu_paused);
}

/* The toolbar is shown above the game too, in a window; in fullscreen it
   goes away. 0 when it is not shown. */
static int fs_overlay;   /* fullscreen: the toolbar is showing (the mouse is at the top edge) */

/* The toolbar's buttons in order (the separators are the two 0 entries between the groups). */
#define TB_ITEMS 16
static void toolbar_items(HWND *it)
{
  HWND row[TB_ITEMS] = { tb_open, tb_refresh, tb_fav, tb_search, tb_sep[0], tb_play, tb_pause, tb_stop, tb_reset, tb_fs, tb_shot,
                         tb_sep[1], tb_set, tb_video, tb_audio, tb_ctl };
  memcpy(it, row, sizeof(row));
}

/* Lays the buttons out in a window `width` wide: where each goes (x, and the row; x -1 = a divider that is not shown) and how
   many rows it takes. When they do not all fit in one row the rest goes on the next, like a menu bar. */
static int toolbar_flow(int width, int *ox, int *orow)
{
  HWND it[TB_ITEMS];
  int k, x = TBS(4), r = 0, left = TBS(4);

  toolbar_items(it);
  for (k = 0; k < TB_ITEMS; k++)
  {
    int is_sep = (it[k] == tb_sep[0] || it[k] == tb_sep[1]);
    int w = is_sep ? TBS(14) : tb_button_w(it[k]);

    if (x + w > width - TBS(4) && x > left) { x = left; r++; }       /* does not fit: the next row */
    ox[k] = x;
    orow[k] = r;
    if (is_sep && x == left) ox[k] = -1;                               /* a divider at the start of a row is not shown */
    else x += w + (is_sep ? 0 : TBS(2));
  }
  for (k = 0; k + 1 < TB_ITEMS; k++)                                    /* nor one at the end of a row */
    if ((it[k] == tb_sep[0] || it[k] == tb_sep[1]) && orow[k + 1] > orow[k]) ox[k] = -1;
  return r + 1;
}

/* The width at which all the toolbar's buttons fit in one row (0 with no toolbar). */
int browser_toolbar_one_row_width(void)
{
  HWND it[TB_ITEMS];
  int k, x = TBS(4);

  if (!panel_toolbar) return 0;
  toolbar_items(it);
  for (k = 0; k < TB_ITEMS; k++)
  {
    int is_sep = (it[k] == tb_sep[0] || it[k] == tb_sep[1]);
    x += (is_sep ? TBS(14) : tb_button_w(it[k]) + TBS(2));
  }
  return x + TBS(4);
}

/* How many rows the toolbar takes in a window that wide; worked out again only when the width or the settings change. */
static int toolbar_rows_for(int width)
{
  static int cw = -1, csz = -1, clb = -1, cui = -1, cdpi = -1, crows = 1;
  int ox[TB_ITEMS], orow[TB_ITEMS];

  if (!tb_open) return 1;
  if (width == cw && gui.toolbar_size == csz && gui.toolbar_labels == clb && gui.large_ui == cui && gui_dpi == cdpi) return crows;
  crows = toolbar_flow(width, ox, orow);
  cw = width; csz = gui.toolbar_size; clb = gui.toolbar_labels; cui = gui.large_ui; cdpi = gui_dpi;
  return crows;
}

/* Its height when the window is `width` wide (a window about to be sized), 0 when it is not shown. */
int browser_toolbar_height_at(int width)
{
  if (!panel_toolbar || gui.hide_toolbar || gui.fullscreen) return 0;
  return tb_bar_h_rows(toolbar_rows_for(width));
}

static int toolbar_current_h(void)
{
  RECT c;
  return (g_hwnd && GetClientRect(g_hwnd, &c)) ? tb_bar_h_rows(toolbar_rows_for(c.right)) : tb_bar_h_rows(1);
}

int browser_toolbar_height(void)
{
  if (!panel_toolbar || gui.hide_toolbar) return 0;
  if (gui.fullscreen) return fs_overlay ? TOOLBAR_H : 0;
  return TOOLBAR_H;
}

/* Places the toolbar (or hides it) and moves the picture's area with it. */
/* Fullscreen: the toolbar comes in at the top of the screen while the mouse is there, together
   with the menu bar; the picture makes room for it, as it does for the menu bar. */
int browser_fullscreen_toolbar_shown_height(void)
{
  return fs_overlay ? TOOLBAR_H : 0;
}

void browser_fullscreen_toolbar(int show)
{
  int want = show && panel_toolbar && gui.fullscreen && !gui.hide_toolbar && gui.fullscreen_toolbar;

  if (want == fs_overlay) return;
  fs_overlay = want;
  browser_toolbar_layout();   /* places (or hides) the toolbar and moves the picture's top with it */

  if (want && panel_toolbar)
  {
    /* The picture's own window may have been made again since the toolbar was last up (a new video mode, a new swap chain)
       and so sit above it: the toolbar goes on top, and is painted at once, not whenever the game loop leaves time for it. */
    SetWindowPos(panel_toolbar, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    RedrawWindow(panel_toolbar, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
  }
}

/* While the fullscreen toolbar is up: its size and place checked, kept above the picture and painted again (now and then it was
   left half drawn, with the picture showing through where it had not been painted). */
void browser_fullscreen_toolbar_refresh(void)
{
  RECT c, w;

  if (!fs_overlay || !panel_toolbar || !gui.fullscreen || !g_hwnd) return;
  if (GetClientRect(g_hwnd, &c) && GetWindowRect(panel_toolbar, &w) && (w.right - w.left) != c.right) browser_toolbar_layout();
  SetWindowPos(panel_toolbar, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  RedrawWindow(panel_toolbar, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

void browser_toolbar_layout(void)
{
  static int last_h = -1;
  int h = browser_toolbar_height();
  int show_h;
  RECT c;

  if (!panel_toolbar) return;

  if (!gui.fullscreen) fs_overlay = 0;
  show_h = h;

  if (show_h)
  {
    HWND row[TB_ITEMS];
    int ox[TB_ITEMS], orow[TB_ITEMS];
    int bh = tb_button_h(), gap = TBS(2), sep = TBS(14), by0 = TBS(4), k;

    toolbar_items(row);
    GetClientRect(g_hwnd, &c);
    toolbar_flow(c.right, ox, orow);
    MoveWindow(panel_toolbar, 0, 0, c.right, show_h, TRUE);

    for (k = 0; k < TB_ITEMS; k++)
    {
      int is_sep = (row[k] == tb_sep[0] || row[k] == tb_sep[1]);
      if (ox[k] < 0) { ShowWindow(row[k], SW_HIDE); continue; }         /* a divider that would stand at the edge of a row */
      MoveWindow(row[k], ox[k], by0 + orow[k] * (bh + gap), is_sep ? sep : tb_button_w(row[k]), bh, TRUE);
      ShowWindow(row[k], SW_SHOWNA);
    }
    ShowWindow(panel_toolbar, SW_SHOWNA);
  }
  else
  {
    ShowWindow(panel_toolbar, SW_HIDE);
  }

  if (h != last_h)
  {
    last_h = h;
    video_invalidate();
    InvalidateRect(g_hwnd, NULL, TRUE);
  }
}

/* Whether the search bar is up (the View menu's check mark follows this). */
int browser_search_shown(void)
{
  return search_shown();
}

/* View > Toolbars > Show Search Bar and the bar's Close button: shown <-> hidden, and
   remembered. */
/* Ctrl+F while the bar is shown: hidden for this run only (the View setting is not touched). */
void browser_hide_search_now(void)
{
  search_override = 0;
  browser_bars_changed();
}

void browser_toggle_searchbar(void)
{
  gui.hide_searchbar = search_shown() ? 1 : 0;
  search_override = -1;
  browser_bars_changed();
  config_save();
}

/* The genre the list shows for a game, looked up by its file name ("" when unknown). */
void browser_genre_for(const char *path, char *out, int out_len)
{
  const char *name = strrchr(path, '\\');
  classify_genre(name ? name + 1 : path, out, out_len);
}
