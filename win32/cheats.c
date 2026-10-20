/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  cheats.c -- Game Genie / Action Replay / raw patch support.
 *
 *  There is no cheat engine in the core: decoding and patching are frontend
 *  work, duplicated in the libretro and Wii ports. The decoder below is
 *  ported from libretro/libretro.c so the accepted code formats stay
 *  identical across ports.
 *
 *  Codes are kept as text and re-decoded on every apply, because what a code
 *  means depends on system_hw -- the same string decodes differently for a
 *  Mega Drive and a Master System game. Storing the decoded address would
 *  silently mis-patch after a console switch.
 *
 *  Patches come in two kinds and are maintained differently:
 *
 *    ROM patches are written once into cart.rom (16-bit systems) or into the
 *    currently banked page (8-bit systems), and the original bytes are kept
 *    so they can be put back. The 8-bit case has to be redone whenever the
 *    mapper switches banks, which is what the core's CHEATS_UPDATE hook is
 *    for.
 *
 *    RAM patches cannot be written once, because the game overwrites them.
 *    They are reapplied every frame.
 ****************************************************************************/

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <uxtheme.h>
#include <zlib.h>

#include "shared.h"
#include "gui.h"
#include "theme.h"
#include "resource.h"

#define CHEAT_CODE_LEN  24
#define CHEAT_DESC_LEN  128

typedef struct
{
  char   code[CHEAT_CODE_LEN];
  char   desc[CHEAT_DESC_LEN];
  int    enabled;
  int    valid;        /* the code decoded against the current system */

  uint32 address;
  uint16 data;
  uint16 old;          /* 16-bit: original word. 8-bit: reference byte (0 = no check) */
  uint8  orig;         /* 8-bit: the byte that was at *prev before it was patched */
  uint8 *prev;         /* patched location in the banked 8-bit ROM window */
} t_cheat;

static t_cheat cheats[CHEAT_MAX];
static int     cheat_count;

/* Indexes of the active patches: RAM grows from the front, ROM from the back. */
static unsigned short cheat_index[CHEAT_MAX];
static int   ram_patches;
static int   rom_patches;

static int   patches_live;   /* patches are currently written into memory */

/*
 * Set while the list view is being repopulated. Ticking a checkbox in code
 * raises the same LVN_ITEMCHANGED notification a user click does, and that
 * handler refills the list -- which would tick more boxes, and so on.
 */
static int   list_refilling;

/* While a whole file is being loaded every entry would re-decode and re-patch
   the whole list; that is done once at the end instead. */
static int   defer_apply;
static char  sim_files[32][MAX_PATH];   /* similar cheat files (see cheats_find_similar) */
static int   sim_n;
static char  sim_dir[MAX_PATH];         /* the console folder they were found in */

static const char ggvalidchars[] = "ABCDEFGHJKLMNPRSTVWXYZ0123456789";
static const char arvalidchars[] = "0123456789ABCDEF";

/****************************************************************************
 * Decoding
 *
 * Ported from libretro/libretro.c. Returns the code length, or 0 when the
 * string is not a valid code for the system currently running.
 ****************************************************************************/

/* strchr() treats the terminating NUL as part of the string, so a short code
   would have decoded its missing digits as valid ones. */
static const char *valid_char(const char *set, int c)
{
  return c ? strchr(set, c) : NULL;
}

static uint32 decode_cheat_to(const char *input, uint32 *out_address, uint16 *out_data, uint8 *out_ref)
{
  const char *string = input;
  const char *p;
  int i, n;
  uint32 len = 0;
  uint32 address = 0;
  uint16 data = 0;
  uint8  ref = 0;

  if ((system_hw & SYSTEM_PBC) == SYSTEM_MD)
  {
    /* ---- 16-bit systems ---- */

    /* Game Genie: ABCD-EFGH */
    if ((strlen(string) >= 9) && (string[4] == '-'))
    {
      for (i = 0; i < 8; i++)
      {
        if (i == 4) string++;
        p = valid_char(ggvalidchars, *string++);
        if (!p) return 0;
        n = (int)(p - ggvalidchars);

        switch (i)
        {
          case 0: data |= n << 3; break;
          case 1: data |= n >> 2; address |= (n & 3) << 14; break;
          case 2: address |= n << 9; break;
          case 3: address |= (n & 0xF) << 20 | (n >> 4) << 8; break;
          case 4: data |= (n & 1) << 12; address |= (n >> 1) << 16; break;
          case 5: data |= (n & 1) << 15 | (n >> 1) << 8; break;
          case 6: data |= (n >> 3) << 13; address |= (n & 7) << 5; break;
          case 7: address |= n; break;
        }
      }
      len = 9;
    }
    /* Action Replay / raw patch: aaaaaa:dddd */
    else if ((strlen(string) >= 11) && (string[6] == ':'))
    {
      for (i = 0; i < 6; i++)
      {
        p = valid_char(arvalidchars, *string++);
        if (!p) return 0;
        n = (int)(p - arvalidchars) & 0xF;
        address |= (uint32)n << ((5 - i) * 4);
      }

      string++;
      for (i = 0; i < 4; i++)
      {
        p = valid_char(arvalidchars, *string++);
        if (!p) return 0;
        n = (int)(p - arvalidchars) & 0xF;
        data |= (uint16)(n << ((3 - i) * 4));
      }
      len = 11;
    }
  }
  else
  {
    /* ---- 8-bit systems ---- */

    /* Game Genie: ABC-DEF or ABC-DEF-GHI */
    if ((strlen(string) >= 7) && (string[3] == '-'))
    {
      for (i = 0; i < 2; i++)
      {
        p = valid_char(arvalidchars, *string++);
        if (!p) return 0;
        n = (int)(p - arvalidchars) & 0xF;
        data |= (uint16)(n << ((1 - i) * 4));
      }

      for (i = 0; i < 3; i++)
      {
        if (i == 1) string++;   /* separator */
        p = valid_char(arvalidchars, *string++);
        if (!p) return 0;
        n = (int)(p - arvalidchars) & 0xF;
        address |= (uint32)n << ((2 - i) * 4);
      }

      p = valid_char(arvalidchars, *string++);
      if (!p) return 0;
      n = (int)(p - arvalidchars) & 0xF;
      n ^= 0xF;                 /* high nibble is inverted */
      address |= (uint32)n << 12;

      if (*string == '-')
      {
        /* optional reference byte */
        for (i = 0; i < 2; i++)
        {
          if (!*string) return 0;
          string++;
          p = valid_char(arvalidchars, *string++);
          if (!p) return 0;
          n = (int)(p - arvalidchars) & 0xF;
          ref |= (uint8)(n << ((1 - i) * 4));
        }
        ref = (uint8)((ref >> 2) | ((ref & 0x03) << 6));
        ref ^= 0xBA;
        len = 11;
      }
      else
      {
        len = 7;
      }
    }
    /* Action Replay: 00XX-YYYY */
    else if ((strlen(string) >= 9) && (string[4] == '-'))
    {
      string += 2;

      for (i = 0; i < 4; i++)
      {
        p = valid_char(arvalidchars, *string++);
        if (!p) return 0;
        n = (int)(p - arvalidchars) & 0xF;
        address |= (uint32)n << ((3 - i) * 4);
        if (i == 1) string++;
      }

      for (i = 0; i < 2; i++)
      {
        p = valid_char(arvalidchars, *string++);
        if (!p) return 0;
        n = (int)(p - arvalidchars) & 0xF;
        data |= (uint16)(n << ((1 - i) * 4));
      }
      len = 9;
    }
    /* Fusion RAM: aaaa:dd */
    else if ((strlen(string) >= 7) && (string[4] == ':'))
    {
      for (i = 0; i < 4; i++)
      {
        p = valid_char(arvalidchars, *string++);
        if (!p) return 0;
        n = (int)(p - arvalidchars) & 0xF;
        address |= (uint32)n << ((3 - i) * 4);
      }

      string++;
      for (i = 0; i < 2; i++)
      {
        p = valid_char(arvalidchars, *string++);
        if (!p) return 0;
        n = (int)(p - arvalidchars) & 0xF;
        data |= (uint16)(n << ((1 - i) * 4));
      }
      len = 7;
    }
    /* Fusion ROM: rraaaa:dd */
    else if ((strlen(string) >= 9) && (string[6] == ':'))
    {
      for (i = 0; i < 2; i++)
      {
        p = valid_char(arvalidchars, *string++);
        if (!p) return 0;
        n = (int)(p - arvalidchars) & 0xF;
        ref |= (uint8)(n << ((1 - i) * 4));
      }

      for (i = 0; i < 4; i++)
      {
        p = valid_char(arvalidchars, *string++);
        if (!p) return 0;
        n = (int)(p - arvalidchars) & 0xF;
        address |= (uint32)n << ((3 - i) * 4);
      }

      string++;
      for (i = 0; i < 2; i++)
      {
        p = valid_char(arvalidchars, *string++);
        if (!p) return 0;
        n = (int)(p - arvalidchars) & 0xF;
        data |= (uint16)(n << ((1 - i) * 4));
      }
      len = 9;
    }

    /* Map the Z80 Work RAM window onto a 24-bit address. */
    if (address >= 0xC000)
    {
      address = 0xFF0000 | (address & 0x1FFF);
    }
  }

  if (len)
  {
    *out_address = address;
    *out_data    = data;
    *out_ref     = ref;
  }

  return len;
}

static uint32 decode_cheat(const char *input, int index)
{
  uint32 address = 0;
  uint16 data = 0;
  uint8  ref = 0;
  uint32 len = decode_cheat_to(input, &address, &data, &ref);

  if (len)
  {
    cheats[index].address = address;
    cheats[index].data    = data;
    cheats[index].old     = ref;
  }

  return len;
}

/* Mega CD Word RAM as the main CPU sees it right now: 256 KB in 2M mode, or the 128 KB bank it has been
   given in 1M mode. Returns the start and sets `mask` to the last even offset. */
static uint8 *wordram_window(uint32 *mask)
{
  if (scd.regs[0x03 >> 1].byte.l & 0x04)
  {
    *mask = 0x1fffe;
    return m68k.memory_map[scd.cartridge.boot + 0x20].base;
  }
  *mask = 0x3fffe;
  return scd.word_ram_2M;
}

int cheats_wordram_1m(void)
{
  return (scd.regs[0x03 >> 1].byte.l & 0x04) != 0;
}

/****************************************************************************
 * Applying and removing patches
 ****************************************************************************/

static void write_patches(void)
{
  uint8 *ptr;
  int i;

  ram_patches = 0;
  rom_patches = 0;

  if (!emu_running || !gui.cheats_enabled) return;

  for (i = 0; i < cheat_count; i++)
  {
    if (!cheats[i].enabled || !cheats[i].valid) continue;

    /* Work RAM */
    if (cheats[i].address >= 0xFF0000)
    {
      cheat_index[ram_patches++] = (unsigned short)i;
    }
    /* Mega CD RAM areas */
    else if ((system_hw == SYSTEM_MCD) && !scd.cartridge.boot)
    {
      if (cheats[i].address < 0x80000)
      {
        cheat_index[ram_patches++] = (unsigned short)i;
      }
      else if ((cheats[i].address >= 0x200000) && (cheats[i].address < 0x240000))
      {
        cheat_index[ram_patches++] = (unsigned short)i;
      }
    }
    /* Cartridge ROM */
    else if (cheats[i].address < cart.romsize)
    {
      if ((system_hw & SYSTEM_PBC) == SYSTEM_MD)
      {
        /* Keep the original word so the patch can be undone. */
        cheats[i].old = *(uint16 *)(cart.rom + (cheats[i].address & 0xFFFFFE));
        *(uint16 *)(cart.rom + (cheats[i].address & 0xFFFFFE)) = cheats[i].data;
      }
      else
      {
        rom_patches++;
        cheat_index[CHEAT_MAX - rom_patches] = (unsigned short)i;

        ptr = &z80_readmap[(cheats[i].address) >> 10][cheats[i].address & 0x03FF];

        /*
         * A basic Game Genie code carries no reference byte, so .old is 0.
         * Treated as "no check" rather than "only patch a byte that is
         * already 0" -- the latter would silently fail to apply most
         * ordinary codes until the next bank switch. Must match the same
         * condition in ROMCheatUpdate() below, or a patch applied here would
         * be second-guessed the moment banking changes.
         */
        if (!cheats[i].old || ((uint8)cheats[i].old) == *ptr)
        {
          cheats[i].orig = *ptr;      /* what to put back later */
          *ptr = (uint8)cheats[i].data;
          cheats[i].prev = ptr;
        }
        else
        {
          cheats[i].prev = NULL;
        }
      }
    }
  }

  patches_live = 1;
}

static void remove_patches(void)
{
  int i;

  if (!patches_live) return;
  patches_live = 0;

  if (!emu_running) return;

  /* Mega CD games get no ROM patches, so there is nothing to undo. */
  if ((system_hw == SYSTEM_MCD) && !scd.cartridge.boot) return;

  /* Reverse order, in case two patches share an address. */
  i = cheat_count;
  while (i > 0)
  {
    i--;

    if (!cheats[i].enabled || !cheats[i].valid) continue;
    if (cheats[i].address >= cart.romsize) continue;

    if ((system_hw & SYSTEM_PBC) == SYSTEM_MD)
    {
      *(uint16 *)(cart.rom + (cheats[i].address & 0xFFFFFE)) = cheats[i].old;
    }
    else if (cheats[i].prev != NULL)
    {
      *cheats[i].prev = cheats[i].orig;
      cheats[i].prev = NULL;
    }
  }
}

void cheats_apply(void)
{
  int i;

  remove_patches();

  ram_patches = 0;
  rom_patches = 0;

  /* Re-decode: what a code means depends on the system that is running. */
  for (i = 0; i < cheat_count; i++)
  {
    cheats[i].prev  = NULL;
    cheats[i].valid = emu_running ? (decode_cheat(cheats[i].code, i) != 0) : 0;
  }

  write_patches();
}

void cheats_suspend(void)
{
  remove_patches();
  ram_patches = 0;
  rom_patches = 0;
}

/* Called once per frame: RAM patches are overwritten by the game otherwise. */
void cheats_ram_update(void)
{
  uint8 *base;
  uint32 mask;
  int index, cnt = ram_patches;

  while (cnt)
  {
    index = cheat_index[--cnt];

    switch ((cheats[index].address >> 20) & 0xF)
    {
      case 0x0:   /* Mega CD PRG-RAM, 512 KB */
        base = scd.prg_ram;
        mask = 0x7fffe;
        break;

      case 0x2:   /* Mega CD Word-RAM: 256 KB in 2M mode, the main CPU's 128 KB bank in 1M mode */
        if ((cheats[index].address & 0x20000) && cheats_wordram_1m()) continue;   /* the cell image area in 1M mode */
        base = wordram_window(&mask);
        break;

      default:    /* Work RAM, 64 KB */
        base = work_ram;
        mask = 0xfffe;
        break;
    }

    if (cheats[index].data & 0xFF00)
    {
      *(uint16 *)(base + (cheats[index].address & mask)) = cheats[index].data;
    }
    else
    {
      uint32 offset;

      mask |= 1;
      offset = cheats[index].address & mask;

#ifdef LSB_FIRST
      /* On the 16-bit consoles the core keeps its RAM with every pair of bytes
         swapped on a little-endian PC, so byte N lives at N ^ 1 (the word
         writes above already come out right). Without this a byte code such as
         FFFE12:0009 wrote to the neighbouring address. The 8-bit consoles' RAM
         is a plain array and is left alone. */
      if ((system_hw & SYSTEM_PBC) == SYSTEM_MD) offset ^= 1;
#endif

      base[offset] = (uint8)cheats[index].data;
    }
  }
}

/*
 * Called by the core through the CHEATS_UPDATE macro whenever an 8-bit
 * mapper switches banks: the patched bytes live in the banked window, so the
 * old location has to be restored and the new one patched.
 */
void ROMCheatUpdate(void)
{
  int index, cnt = rom_patches;
  uint8 *ptr;

  while (cnt)
  {
    index = cheat_index[CHEAT_MAX - cnt];

    if (cheats[index].prev != NULL)
    {
      *cheats[index].prev = cheats[index].orig;
      cheats[index].prev = NULL;
    }

    ptr = &z80_readmap[(cheats[index].address) >> 10][cheats[index].address & 0x03FF];

    if (!cheats[index].old || ((uint8)cheats[index].old) == *ptr)
    {
      cheats[index].orig = *ptr;
      *ptr = (uint8)cheats[index].data;
      cheats[index].prev = ptr;
    }

    cnt--;
  }
}

/****************************************************************************
 * List management
 ****************************************************************************/

int cheats_is_valid(int index)
{
  if (index < 0 || index >= cheat_count) return 0;
  return cheats[index].valid;
}

/* Codes that are really changing the game: none while the Enable Cheats
   switch is off, whatever the individual ticks say. */
int cheats_effective(void)
{
  return gui.cheats_enabled ? cheats_active() : 0;
}

int cheats_active(void)
{
  int i, n = 0;
  for (i = 0; i < cheat_count; i++)
  {
    if (cheats[i].enabled && cheats[i].valid) n++;
  }
  return n;
}

/* Normalises to upper case and strips spaces so pasted codes just work. */
static void normalise_code(const char *in, char *out, int out_len)
{
  int n = 0;

  while (*in && n < out_len - 1)
  {
    if (*in != ' ' && *in != '\t')
    {
      out[n++] = (char)((*in >= 'a' && *in <= 'z') ? (*in - 'a' + 'A') : *in);
    }
    in++;
  }
  out[n] = '\0';
}

int cheats_add(const char *code, const char *desc)
{
  char clean[CHEAT_CODE_LEN];
  int index;

  if (!code || !code[0]) return -1;
  if (cheat_count >= CHEAT_MAX) return -1;

  normalise_code(code, clean, sizeof(clean));
  if (!clean[0]) return -1;

  /* A code that is already in the list is added again as a new row (for
     example to keep a copy under another name); the same patch applied twice
     changes nothing. */

  remove_patches();

  index = cheat_count++;
  ZeroMemory(&cheats[index], sizeof(t_cheat));
  lstrcpynA(cheats[index].code, clean, CHEAT_CODE_LEN);
  lstrcpynA(cheats[index].desc, (desc && desc[0]) ? desc : "Unnamed", CHEAT_DESC_LEN);
  cheats[index].enabled = 1;

  if (!defer_apply) cheats_apply();
  return index;
}

void cheats_remove(int index)
{
  int i;

  if (index < 0 || index >= cheat_count) return;

  remove_patches();

  for (i = index; i < cheat_count - 1; i++)
  {
    cheats[i] = cheats[i + 1];
  }
  cheat_count--;

  cheats_apply();
}

void cheats_set_enabled(int index, int on)
{
  if (index < 0 || index >= cheat_count) return;
  if (cheats[index].enabled == (on ? 1 : 0)) return;

  remove_patches();
  cheats[index].enabled = on ? 1 : 0;
  if (!defer_apply) cheats_apply();
}

/* Switches every code on or off at once (one re-patch, not one per code). */
void cheats_set_all_enabled(int on)
{
  int i;

  remove_patches();
  for (i = 0; i < cheat_count; i++) cheats[i].enabled = on ? 1 : 0;
  cheats_apply();
}

/* Replaces the code and description of one entry where it sits, keeping its
   position and its enabled flag. Returns 0 on success, -1 for a bad index or
   empty code, -2 if a different entry already uses that code. */
static int cheats_update_entry(int index, const char *code, const char *desc)
{
  char clean[CHEAT_CODE_LEN];

  if (index < 0 || index >= cheat_count) return -1;
  if (!code || !code[0]) return -1;

  normalise_code(code, clean, sizeof(clean));
  if (!clean[0]) return -1;

  /* (A code already in the list is allowed here too, as it is for Add.) */

  /* Undo the patches while the entry still describes what was written;
     cheats_apply() then re-decodes every entry, this one included. */
  remove_patches();

  lstrcpynA(cheats[index].code, clean, CHEAT_CODE_LEN);
  lstrcpynA(cheats[index].desc, (desc && desc[0]) ? desc : "Unnamed", CHEAT_DESC_LEN);

  cheats_apply();
  return 0;
}

void cheats_remove_all(void)
{
  remove_patches();
  cheat_count = 0;
  ram_patches = 0;
  rom_patches = 0;
}

/****************************************************************************
 * Per-ROM persistence
 *
 * Plain text so the files can be edited by hand or shared:
 *
 *     1 ABCD-EFGH Infinite lives
 *     0 FFFE21:0063 Max rings
 ****************************************************************************/

/* <exe>\cheats\<Console>\<ROM name>.cht: named exactly like the game's file, inside its console's folder, so a cheat file put
   there by hand (or copied from another PC) is found. Nothing is looked for or moved anywhere else. */
static void cheat_file_path(const char *rom_base, int for_read, char *out)
{
  (void)rom_base;   /* the loaded (or inspected) game's own name is used */
  (void)for_read;
  emu_console_file_path("cheats", ".cht", out, GUI_PATH_LEN);
}

/* Reads cheat lines ("<0|1> <code> <name>") from an open file into the list; returns how many were added.
   The caller sets defer_apply and applies afterwards. */
static int cheats_read_stream(FILE *fp)
{
  char line[256];
  int added = 0;

  while (fgets(line, sizeof(line), fp))
  {
    char code[CHEAT_CODE_LEN];
    char desc[CHEAT_DESC_LEN];
    char *p = line;
    int enabled, n;

    /* A line longer than the buffer: use what fits and throw away the rest,
       instead of reading the tail as a line of its own. */
    n = (int)strlen(p);
    if (n == (int)sizeof(line) - 1 && p[n - 1] != '\n')
    {
      int c;
      while ((c = fgetc(fp)) != EOF && c != '\n') { }
    }

    /* strip the newline */
    while (n > 0 && (p[n - 1] == '\n' || p[n - 1] == '\r')) p[--n] = '\0';

    while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == '#') continue;

    /* "<0|1> <code> <name>". A line that begins straight with a code (no flag,
       for example one typed in by hand) counts as switched on. */
    enabled = 1;
    if ((*p == '0' || *p == '1') && (p[1] == ' ' || p[1] == '\t'))
    {
      enabled = (*p != '0');
      p++;
    }
    while (*p == ' ' || *p == '\t') p++;

    n = 0;
    while (*p && *p != ' ' && *p != '\t' && n < CHEAT_CODE_LEN - 1) code[n++] = *p++;
    code[n] = '\0';
    if (!code[0]) continue;

    /* A token longer than the code buffer stops here on the length cap,
       not on a delimiter -- p is left mid-token, not at the space before
       the description. Skip whatever's left of it so that tail doesn't
       get read as (part of) the description below. No-op for a normal,
       properly-delimited token: p is already on whitespace at this point. */
    while (*p && *p != ' ' && *p != '\t') p++;

    while (*p == ' ' || *p == '\t') p++;
    lstrcpynA(desc, p, CHEAT_DESC_LEN);

    /* Must go through cheats_set_enabled: cheats_add() applies the patch
       straight away, so flipping the flag behind its back would leave a
       patch written that nothing knows how to undo. */
    if (cheats_add(code, desc) >= 0)
    {
      cheats_set_enabled(cheat_count - 1, enabled);
      added++;
    }
  }

  return added;
}

/* CRC32 of the game file as a ROM database knows it (the cartridge ROM is kept with every pair of bytes
   swapped on a little-endian PC for the Mega Drive, so that is undone here). 0 when there is no ROM. */
unsigned long emu_rom_crc32(void)
{
  unsigned long crc = crc32(0L, Z_NULL, 0);

  if (!cart.romsize) return 0;
#ifdef LSB_FIRST
  if (system_hw == SYSTEM_MD)
  {
    uint8 buf[4096];
    uint32 pos = 0;

    while (pos < cart.romsize)
    {
      uint32 n = cart.romsize - pos, i;
      if (n > sizeof(buf)) n = sizeof(buf);
      for (i = 0; i + 1 < n; i += 2) { buf[i] = cart.rom[pos + i + 1]; buf[i + 1] = cart.rom[pos + i]; }
      if (n & 1) buf[n - 1] = cart.rom[pos + n - 1];
      crc = crc32(crc, buf, (uInt)n);
      pos += n;
    }
    return crc;
  }
#endif
  return crc32(crc, cart.rom, (uInt)cart.romsize);
}

/* A cheat file in the library folder (cheats\library) for the running game: one named after its CRC32
   (12345678.cht), or any .cht that says "crc32 = 12345678" in a comment line near its top. */
static int cheats_library_find(char *out, int out_len)
{
  char dir[GUI_PATH_LEN], pat[GUI_PATH_LEN + 8], want[16];
  WIN32_FIND_DATAA fd;
  HANDLE h;
  unsigned long crc = emu_rom_crc32();
  int found = 0;

  if (!crc || system_hw == SYSTEM_MCD) return 0;
  gui_snprintf(want, sizeof(want), "%08lX", crc);
  gui_snprintf(dir, sizeof(dir), "%s\\library", osd_path("cheats"));
  gui_snprintf(out, out_len, "%s\\%s.cht", dir, want);
  if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return 1;

  gui_snprintf(pat, sizeof(pat), "%s\\*.cht", dir);
  h = FindFirstFileA(pat, &fd);
  if (h == INVALID_HANDLE_VALUE) return 0;
  do
  {
    FILE *fp;
    char line[256];
    int n = 0;

    gui_snprintf(out, out_len, "%s\\%s", dir, fd.cFileName);
    fp = fopen(out, "r");
    if (!fp) continue;
    while (n++ < 12 && fgets(line, sizeof(line), fp))
    {
      char *p = strstr(line, "crc32");
      if (!p) p = strstr(line, "CRC32");
      if (p && line[0] == '#')
      {
        p = strchr(p, '=');
        if (p)
        {
          while (*++p == ' ') { }
          if (!_strnicmp(p, want, 8)) { found = 1; break; }
        }
      }
    }
    fclose(fp);
  } while (!found && FindNextFileA(h, &fd));
  FindClose(h);
  return found;
}

static char cheats_game_base[128];

void cheats_load_for_rom(const char *rom_base)
{
  char path[GUI_PATH_LEN];
  FILE *fp;

  cheats_remove_all();

  if (!rom_base || !rom_base[0]) { cheats_game_base[0] = '\0'; return; }
  lstrcpynA(cheats_game_base, rom_base, sizeof(cheats_game_base));

  cheat_file_path(rom_base, 1, path);
  fp = fopen(path, "r");
  if (!fp && cheats_library_find(path, sizeof(path))) fp = fopen(path, "r");   /* no cheats of your own: the library's */
  if (!fp) return;

  defer_apply = 1;
  cheats_read_stream(fp);
  fclose(fp);
  defer_apply = 0;
  cheats_apply();
}

/****************************************************************************
 * Importing RetroArch-format .cht files
 *
 * A completely different layout from the plain per-ROM format just
 * above (which this app also happens to use the same ".cht" extension
 * for, coincidentally) -- key/value pairs, grouped by a numeric index:
 *
 *     cheats = 2
 *     cheat0_desc = "Infinite Lives"
 *     cheat0_code = "FFFE01:63"
 *     cheat0_enable = true
 *     cheat1_desc = "Infinite Energy"
 *     cheat1_code = "FFFC00:F0+FFFC02:F0"
 *     cheat1_enable = false
 ****************************************************************************/

/* Pulls out the value after '=' -- quoted ("...") for desc/code, bare
   (true/false) for enable. Handles both without needing to know which
   one is being read. */
static void cht_extract_value(const char *line, char *out, int out_len)
{
  const char *eq = strchr(line, '=');
  const char *v;
  int n;

  out[0] = '\0';
  if (!eq) return;
  eq++;

  v = strchr(eq, '"');
  if (v)
  {
    const char *end;
    v++;
    end = strchr(v, '"');
    n = end ? (int)(end - v) : (int)strlen(v);
  }
  else
  {
    while (*eq == ' ' || *eq == '\t') eq++;
    v = eq;
    n = (int)strlen(v);
    while (n > 0 && (v[n - 1] == ' ' || v[n - 1] == '\t' ||
                     v[n - 1] == '\r' || v[n - 1] == '\n')) n--;
  }

  if (n >= out_len) n = out_len - 1;
  if (n < 0) n = 0;
  memcpy(out, v, n);
  out[n] = '\0';
}

/* RetroArch allows several address:value patches in one cheat, joined
   with '+' -- GPGX's own code field is one patch per entry with no
   equivalent multi-patch syntax, so each '+'-separated piece becomes
   its own entry here, all sharing the same description. */
static int cht_add_split_code(const char *code, const char *desc, int enabled)
{
  char part[CHEAT_CODE_LEN];
  const char *p = code;
  int added = 0;

  while (*p)
  {
    int n = 0;
    while (*p && *p != '+' && n < CHEAT_CODE_LEN - 1) part[n++] = *p++;
    part[n] = '\0';

    /* A segment longer than the code buffer stops here on the length cap,
       not on '+' -- p is left mid-segment. Skip the rest of it so the
       leftover tail isn't re-entered as if it were its own, separate
       segment next time around the loop. No-op for a normal segment: p is
       already on '+' or NUL at this point. */
    while (*p && *p != '+') p++;

    if (*p == '+') p++;

    if (part[0])
    {
      int idx = cheats_add(part, desc);
      if (idx >= 0)
      {
        cheats_set_enabled(idx, enabled);
        added++;
      }
    }
  }

  return added;
}

/* Returns the number of cheat entries actually added (a multi-patch
   RetroArch cheat counts as more than one), or -1 if the file couldn't
   be opened at all. */
int cheats_import_retroarch_cht(const char *path)
{
  FILE *fp;
  char line[512];
  char cur_desc[CHEAT_DESC_LEN];
  char cur_code[256];
  int  cur_enable;
  int  cur_n;
  int  have_entry;
  int  added;

  fp = fopen(path, "r");
  if (!fp) return -1;

  defer_apply = 1;

  cur_desc[0]  = '\0';
  cur_code[0]  = '\0';
  cur_enable   = 1;
  cur_n        = -1;
  have_entry   = 0;
  added        = 0;

  while (fgets(line, sizeof(line), fp))
  {
    char *p = line;
    char value[256];
    int digits, this_n;

    while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == '#' || *p == ';') continue;
    if (strncmp(p, "cheat", 5) != 0) continue;
    p += 5;

    digits = 0;
    while (p[digits] >= '0' && p[digits] <= '9') digits++;
    if (digits == 0) continue;   /* the "cheats = N" total-count line has no index */

    this_n = atoi(p);
    p += digits;

    if (this_n != cur_n)
    {
      /* Starting a new index -- flush whatever was accumulated for the
         previous one first. */
      if (have_entry && cur_code[0])
        added += cht_add_split_code(cur_code, cur_desc[0] ? cur_desc : "Unnamed", cur_enable);

      cur_desc[0] = '\0';
      cur_code[0] = '\0';
      cur_enable  = 1;
      cur_n       = this_n;
      have_entry  = 1;
    }

    if (strncmp(p, "_desc", 5) == 0)
    {
      cht_extract_value(p, value, sizeof(value));
      lstrcpynA(cur_desc, value, sizeof(cur_desc));
    }
    else if (strncmp(p, "_code", 5) == 0)
    {
      cht_extract_value(p, value, sizeof(value));
      lstrcpynA(cur_code, value, sizeof(cur_code));
    }
    else if (strncmp(p, "_enable", 7) == 0)
    {
      cht_extract_value(p, value, sizeof(value));
      cur_enable = (lstrcmpiA(value, "true") == 0);
    }
  }

  if (have_entry && cur_code[0])
    added += cht_add_split_code(cur_code, cur_desc[0] ? cur_desc : "Unnamed", cur_enable);

  fclose(fp);
  defer_apply = 0;
  cheats_apply();
  return added;
}

void cheats_save_for_rom(const char *rom_base)
{
  char path[GUI_PATH_LEN];
  FILE *fp;
  int i;

  if (!rom_base || !rom_base[0]) return;

  

  if (!cheat_count)
  {
    /* An empty list means no file -- including any older-named copy, which
       would otherwise be read back in next time. */
    int i;
    for (i = 0; i < 8; i++)
    {
      cheat_file_path(rom_base, 1, path);
      if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) break;
      DeleteFileA(path);
    }
    return;
  }

  cheat_file_path(rom_base, 0, path);
  fp = fopen(path, "w");
  if (!fp) return;

  fprintf(fp, "# Genesis Plus EX cheats\n");
  fprintf(fp, "# <enabled> <code> <description>\n");

  for (i = 0; i < cheat_count; i++)
  {
    fprintf(fp, "%d %s %s\n", cheats[i].enabled ? 1 : 0,
            cheats[i].code, cheats[i].desc);
  }

  fclose(fp);
}

/* Adds the cheats of a file to the list: a RetroArch .cht file or this program's own plain text lines
   ("1 FFFE01:0009 Infinite lives"). Returns how many were added, -1 when the file cannot be read. */
int cheats_import_file(const char *path)
{
  FILE *fp = fopen(path, "r");
  char line[256];
  int retro = 0, n;

  if (!fp) return -1;
  while (fgets(line, sizeof(line), fp))
    if (strstr(line, "cheat0_") || strstr(line, "cheats =") || strstr(line, "cheats=")) { retro = 1; break; }

  if (retro)
  {
    fclose(fp);
    return cheats_import_retroarch_cht(path);
  }

  rewind(fp);
  defer_apply = 1;
  n = cheats_read_stream(fp);
  fclose(fp);
  defer_apply = 0;
  cheats_apply();
  return n;
}

/****************************************************************************
 * Cheat code converter: a code in one form shown in the others
 ****************************************************************************/

static char *code_forms(const char *typed, char *out, int out_len)
{
  char code[CHEAT_CODE_LEN];
  uint32 address = 0;
  uint16 data = 0;
  uint8 ref = 0;
  char *o = out;
  int i;

  normalise_code(typed, code, sizeof(code));
  /* Action Replay codes are often written as ten digits, FFFE120009 */
  if ((system_hw & SYSTEM_PBC) == SYSTEM_MD && strlen(code) == 10 && code[6] != ':')
  {
    memmove(code + 7, code + 6, 5);
    code[6] = ':';
  }
  out[0] = '\0';
  if (!decode_cheat_to(code, &address, &data, &ref)) return NULL;

  if ((system_hw & SYSTEM_PBC) == SYSTEM_MD)
  {
    char gg[16];
    int n[8];
    uint32 a = address & 0xFFFFFF;

    n[0] = (data >> 3) & 0x1F;
    n[1] = ((data & 7) << 2) | ((a >> 14) & 3);
    n[2] = (a >> 9) & 0x1F;
    n[3] = ((a >> 20) & 0xF) | (((a >> 8) & 1) << 4);
    n[4] = ((data >> 12) & 1) | (((a >> 16) & 0xF) << 1);
    n[5] = ((data >> 15) & 1) | (((data >> 8) & 0xF) << 1);
    n[6] = (((data >> 13) & 3) << 3) | ((a >> 5) & 7);
    n[7] = a & 0x1F;
    for (i = 0; i < 8; i++) gg[i < 4 ? i : i + 1] = ggvalidchars[n[i]];
    gg[4] = '-';
    gg[9] = '\0';
    o += snprintf(o, out_len, "Raw / Action Replay:  %06X:%04X\r\n", (unsigned)a, data);
    snprintf(o, out_len - (int)(o - out), "Game Genie:  %s\r\n", gg);
  }
  else
  {
    unsigned a16 = address >= 0xFF0000 ? (0xC000 | (address & 0x1FFF)) : (address & 0xFFFF);
    unsigned d = data & 0xFF;

    if (!ref) o += snprintf(o, out_len, "Raw (RAM):  %04X:%02X\r\n", a16, d);
    else o += snprintf(o, out_len, "Raw (ROM):  %02X%04X:%02X\r\n", ref, a16, d);
    o += snprintf(o, out_len - (int)(o - out), "Action Replay:  00%02X-%02X%02X\r\n", a16 >> 8, a16 & 0xFF, d);
    if (ref)
    {
      unsigned r = (unsigned)(ref ^ 0xBA);
      r = ((r << 2) | (r >> 6)) & 0xFF;
      snprintf(o, out_len - (int)(o - out), "Game Genie:  %X%X%X-%X%X%X-%X0%X\r\n", d >> 4, d & 15, (a16 >> 8) & 15,
                   (a16 >> 4) & 15, a16 & 15, ((a16 >> 12) & 15) ^ 15, r >> 4, r & 15);
    }
    else
      snprintf(o, out_len - (int)(o - out), "Game Genie:  %X%X%X-%X%X%X\r\n", d >> 4, d & 15, (a16 >> 8) & 15,
                   (a16 >> 4) & 15, a16 & 15, ((a16 >> 12) & 15) ^ 15);
  }
  return out;
}

static INT_PTR CALLBACK cheatconv_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
    case WM_INITDIALOG:
      theme_apply_to_window(dlg);
      SetDlgItemTextA(dlg, IDC_CC_HELP,
        "Type a code (Game Genie, Action Replay or raw, such as the ones Cheat Search makes) and Convert shows it in the other forms.");
      SendDlgItemMessage(dlg, IDC_CC_IN, EM_LIMITTEXT, CHEAT_CODE_LEN - 1, 0);
      EnableWindow(GetDlgItem(dlg, IDC_CC_ADD), FALSE);
      return TRUE;

    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    {
      /* the code box and the result box have the colour of the Name and Code boxes of the Cheats window */
      int cid = GetDlgCtrlID((HWND)lp);
      HBRUSH br = (cid == IDC_CC_IN || cid == IDC_CC_OUT) ? theme_ctlcolor_custom((HDC)wp, RGB(0x38, 0x38, 0x38), 1)
                                                          : theme_ctlcolor((HDC)wp);
      if (br) return (LRESULT)br;
      break;
    }

    case WM_COMMAND:
      switch (LOWORD(wp))
      {
        case IDC_CC_CONVERT:
        {
          char in[CHEAT_CODE_LEN], text[512];

          GetDlgItemTextA(dlg, IDC_CC_IN, in, sizeof(in));
          if (!emu_running)
            SetDlgItemTextA(dlg, IDC_CC_OUT, "Open a game first: the code forms depend on the console.");
          else if (!in[0])
            SetDlgItemTextA(dlg, IDC_CC_OUT, "Type a code first.");
          else if (!code_forms(in, text, sizeof(text)))
            SetDlgItemTextA(dlg, IDC_CC_OUT, "That is not a valid code for this console.");
          else
          {
            SetDlgItemTextA(dlg, IDC_CC_OUT, text);
            EnableWindow(GetDlgItem(dlg, IDC_CC_ADD), TRUE);
          }
          return TRUE;
        }

        case IDC_CC_ADD:
        {
          char in[CHEAT_CODE_LEN];

          GetDlgItemTextA(dlg, IDC_CC_IN, in, sizeof(in));
          if (cheats_add(in, "Converted code") >= 0) SetDlgItemTextA(dlg, IDC_CC_OUT, "Added to the cheats list.");
          else SetDlgItemTextA(dlg, IDC_CC_OUT, "The code could not be added.");
          return TRUE;
        }

        case IDCANCEL:
        case IDOK:
          EndDialog(dlg, 0);
          return TRUE;
      }
      break;
  }
  return FALSE;
}

/****************************************************************************
 * Dialog
 ****************************************************************************/

static void cheats_hint_text(char *out, int out_len)
{
  if (!emu_running && !emu_rom_filename()[0])
  {
    lstrcpynA(out, "Load a ROM first: how a code is read depends on the console.", out_len);
  }
  else if ((system_hw & SYSTEM_PBC) == SYSTEM_MD)
  {
    lstrcpynA(out, "Accepted: Game Genie ABCD-EFGH, or Action Replay aaaaaa:dddd", out_len);
  }
  else
  {
    lstrcpynA(out, "Accepted: Game Genie ABC-DEF(-GHI), Action Replay 00XX-YYYY, or aaaa:dd", out_len);
  }
}

#define CHEATS_KEEP_SELECTION  (-2)   /* cheats_fill_list(): leave the selection as it is */

/* Save only means something with a cheat selected and cheats switched on. */
static void cheats_update_save_state(HWND dlg)
{
  HWND list = GetDlgItem(dlg, IDC_CHEATS_LIST);
  int sel = ListView_GetNextItem(list, -1, LVNI_SELECTED);

  EnableWindow(GetDlgItem(dlg, IDC_CHEATS_SAVE),
               (gui.cheats_enabled && sel >= 0 && sel < cheat_count) ? TRUE : FALSE);
}

/* The checkbox in the "Code" column header is a small picture (unticked or
   ticked) shown at the left of the title, drawn here so that it looks right in
   both light and dark mode and does not depend on any header checkbox support. */
#define HDR_BOX 16

static HIMAGELIST hdr_images;

static void cheats_make_header_images(void)
{
  static const COLORREF key = RGB(255, 0, 255);
  HDC screen = GetDC(NULL);
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP bm = CreateCompatibleBitmap(screen, HDR_BOX * 2, HDR_BOX);
  HGDIOBJ old = SelectObject(mem, bm);
  int dark = theme_is_dark();
  COLORREF line = dark ? RGB(220, 220, 220) : RGB(51, 51, 51);
  COLORREF fill = dark ? RGB(32, 32, 32)    : RGB(255, 255, 255);
  HBRUSH kb = CreateSolidBrush(key), fb = CreateSolidBrush(fill), lb = CreateSolidBrush(line);
  int i;

  {
    RECT all = { 0, 0, HDR_BOX * 2, HDR_BOX };
    FillRect(mem, &all, kb);
  }

  for (i = 0; i < 2; i++)
  {
    int x = i * HDR_BOX;
    RECT box = { x + 1, 1, x + 14, 14 };
    RECT inner = { x + 2, 2, x + 13, 13 };

    FillRect(mem, &box, lb);
    FillRect(mem, &inner, fb);

    if (i == 1)
    {
      HPEN pen = CreatePen(PS_SOLID, 2, line);
      HGDIOBJ op = SelectObject(mem, pen);

      MoveToEx(mem, x + 4, 7, NULL);
      LineTo(mem, x + 6, 10);
      LineTo(mem, x + 11, 4);
      SelectObject(mem, op);
      DeleteObject(pen);
    }
  }

  SelectObject(mem, old);

  if (hdr_images) ImageList_Destroy(hdr_images);
  hdr_images = ImageList_Create(HDR_BOX, HDR_BOX, ILC_COLOR24 | ILC_MASK, 2, 0);
  if (hdr_images) ImageList_AddMasked(hdr_images, bm, key);

  DeleteObject(kb); DeleteObject(fb); DeleteObject(lb);
  DeleteObject(bm);
  DeleteDC(mem);
  ReleaseDC(NULL, screen);
}

/* The state shown in the checkbox in the "Code" column header. */
static void cheats_header_set_check(HWND list, int checked)
{
  HWND header = ListView_GetHeader(list);
  HDITEMA hdi;

  if (!header || !hdr_images) return;
  memset(&hdi, 0, sizeof(hdi));
  hdi.mask = HDI_FORMAT | HDI_IMAGE;
  if (!SendMessageA(header, HDM_GETITEMA, 0, (LPARAM)&hdi)) return;

  hdi.fmt |= HDF_IMAGE;
  hdi.iImage = checked ? 1 : 0;
  SendMessageA(header, HDM_SETITEMA, 0, (LPARAM)&hdi);
}

static int cheats_all_enabled(void)
{
  int i;

  if (cheat_count == 0) return 0;
  for (i = 0; i < cheat_count; i++)
    if (!cheats[i].enabled) return 0;
  return 1;
}

/* The last column takes whatever width is left, so the header has no blank
   cell after it. Redone after every refill, because a vertical scroll bar
   appearing or going away changes how much room there is. */
static void cheats_fit_columns(HWND list)
{
  ListView_SetColumnWidth(list, 2, LVSCW_AUTOSIZE_USEHEADER);
}

/* select: an index to select afterwards, -1 for none, or
   CHEATS_KEEP_SELECTION to reselect whatever was selected before. */
static void cheats_fill_list(HWND dlg, int select)
{
  HWND list = GetDlgItem(dlg, IDC_CHEATS_LIST);
  int i;
  int top = ListView_GetTopIndex(list);

  if (select == CHEATS_KEEP_SELECTION)
  {
    select = ListView_GetNextItem(list, -1, LVNI_SELECTED);
  }

  list_refilling = 1;
  SendMessage(list, LVM_DELETEALLITEMS, 0, 0);

  for (i = 0; i < cheat_count; i++)
  {
    LVITEMA item;

    ZeroMemory(&item, sizeof(item));
    item.mask     = LVIF_TEXT;
    item.iItem    = i;
    item.iSubItem = 0;
    item.pszText  = (LPSTR)cheats[i].code;
    SendMessageA(list, LVM_INSERTITEMA, 0, (LPARAM)&item);

    item.iSubItem = 1;
    item.pszText  = (LPSTR)cheats[i].desc;
    SendMessageA(list, LVM_SETITEMTEXTA, (WPARAM)i, (LPARAM)&item);

    item.iSubItem = 2;
    item.pszText  = (LPSTR)(cheats[i].valid ? "Active" : "Not valid here");
    SendMessageA(list, LVM_SETITEMTEXTA, (WPARAM)i, (LPARAM)&item);

    ListView_SetCheckState(list, i, cheats[i].enabled ? TRUE : FALSE);
  }

  /* Scroll to the end first, then back up to the previously-topmost item --
     EnsureVisible on an item that's now below the viewport scrolls up just
     enough to bring it into view, which lands it at the top rather than
     merely "somewhere visible". Harmless when top was already 0. */
  if (cheat_count > 0)
  {
    ListView_EnsureVisible(list, cheat_count - 1, FALSE);
    ListView_EnsureVisible(list, (top < cheat_count) ? top : cheat_count - 1, FALSE);
  }

  if (select >= 0 && select < cheat_count)
  {
    ListView_SetItemState(list, select, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
  }

  list_refilling = 0;

  cheats_header_set_check(list, cheats_all_enabled());
  cheats_fit_columns(list);
  cheats_update_save_state(dlg);
}

/* Selecting a cheat puts it in the Name and Code fields, ready to edit. */
static void cheats_show_selected(HWND dlg, int index)
{
  if (index < 0 || index >= cheat_count) return;

  SetDlgItemTextA(dlg, IDC_CHEATS_CODE, cheats[index].code);
  SetDlgItemTextA(dlg, IDC_CHEATS_DESC, cheats[index].desc);
  SetDlgItemTextA(dlg, IDC_CHEATS_HINT, "Change the name or code, then press Save.");
}

#define WM_CHEATS_ALL (WM_APP + 21)   /* wParam: 1 = tick every code, 0 = untick every code */

/* The header has to be watched directly: a click on its checkbox is turned
   into a message for the dialog. */
static LRESULT CALLBACK cheats_header_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                           UINT_PTR id, DWORD_PTR ref)
{
  if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK)
  {
    RECT r;
    int x = (int)(short)LOWORD(lp), y = (int)(short)HIWORD(lp);

    /* On the box at the left of the first column's title. */
    if (SendMessageA(hwnd, HDM_GETITEMRECT, 0, (LPARAM)&r) &&
        x >= r.left && x < r.left + HDR_BOX + 8 && y >= r.top && y < r.bottom)
    {
      if (gui.cheats_enabled)
        PostMessage((HWND)ref, WM_CHEATS_ALL, cheats_all_enabled() ? 0 : 1, 0);
      return 0;    /* the click is the checkbox's, not a column click */
    }
  }
  else if (msg == WM_NCDESTROY)
  {
    RemoveWindowSubclass(hwnd, cheats_header_proc, id);
  }

  return DefSubclassProc(hwnd, msg, wp, lp);
}

static void cheats_init_list(HWND dlg)
{
  HWND list = GetDlgItem(dlg, IDC_CHEATS_LIST);
  LVCOLUMNA col;

  SendMessage(list, LVM_SETEXTENDEDLISTVIEWSTYLE,
              LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT,
              LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);

  ZeroMemory(&col, sizeof(col));
  col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

  col.iSubItem = 0; col.cx = 150; col.pszText = (LPSTR)"Code";
  SendMessageA(list, LVM_INSERTCOLUMNA, 0, (LPARAM)&col);

  col.iSubItem = 1; col.cx = 255; col.pszText = (LPSTR)"Description";
  SendMessageA(list, LVM_INSERTCOLUMNA, 1, (LPARAM)&col);

  col.iSubItem = 2; col.cx = 90;  col.pszText = (LPSTR)"Status";   /* widened to fill by cheats_fit_columns() */
  SendMessageA(list, LVM_INSERTCOLUMNA, 2, (LPARAM)&col);

  /* A checkbox beside the "Code" title that ticks or unticks every code. */
  {
    HWND header = ListView_GetHeader(list);

    if (header)
    {
      cheats_make_header_images();
      if (hdr_images) SendMessageA(header, HDM_SETIMAGELIST, 0, (LPARAM)hdr_images);
      SetWindowSubclass(header, cheats_header_proc, 1, (DWORD_PTR)dlg);
      cheats_header_set_check(list, 0);
    }
  }

  cheats_fit_columns(list);
}

static void cheats_do_add(HWND dlg)
{
  char code[CHEAT_CODE_LEN] = "";
  char desc[CHEAT_DESC_LEN] = "";
  int index;

  GetDlgItemTextA(dlg, IDC_CHEATS_CODE, code, sizeof(code));
  GetDlgItemTextA(dlg, IDC_CHEATS_DESC, desc, sizeof(desc));

  if (!code[0])
  {
    SetDlgItemTextA(dlg, IDC_CHEATS_HINT, "Type a code before adding it.");
    return;
  }

  index = cheats_add(code, desc);

  if (index < 0)
  {
    SetDlgItemTextA(dlg, IDC_CHEATS_HINT,
                    (cheat_count >= CHEAT_MAX) ? "The list is full." : "That code could not be added.");
    return;
  }

  cheats_fill_list(dlg, -1);

  SetDlgItemTextA(dlg, IDC_CHEATS_CODE, "");
  SetDlgItemTextA(dlg, IDC_CHEATS_DESC, "");
  SetFocus(GetDlgItem(dlg, IDC_CHEATS_CODE));

  if (!cheats_is_valid(index))
  {
    /* Kept in the list rather than dropped: it may be a code for another
       console, and losing what the user typed is worse than showing it. */
    SetDlgItemTextA(dlg, IDC_CHEATS_HINT,
                    "Added, but that code is not valid for the running game.");
  }
  else
  {
    char hint[128];
    cheats_hint_text(hint, sizeof(hint));
    SetDlgItemTextA(dlg, IDC_CHEATS_HINT, hint);
  }
}

/* Writes the Name and Code fields back into the selected cheat, in place: the
   entry keeps its position in the list and its checkbox state. */
static void cheats_do_save(HWND dlg)
{
  HWND list = GetDlgItem(dlg, IDC_CHEATS_LIST);
  int sel = ListView_GetNextItem(list, -1, LVNI_SELECTED);
  char code[CHEAT_CODE_LEN] = "";
  char desc[CHEAT_DESC_LEN] = "";
  char hint[128];
  int result;

  if (sel < 0 || sel >= cheat_count)
  {
    SetDlgItemTextA(dlg, IDC_CHEATS_HINT, "Select a code to edit it.");
    return;
  }

  GetDlgItemTextA(dlg, IDC_CHEATS_CODE, code, sizeof(code));
  GetDlgItemTextA(dlg, IDC_CHEATS_DESC, desc, sizeof(desc));

  if (!code[0])
  {
    SetDlgItemTextA(dlg, IDC_CHEATS_HINT, "Type a code before saving it.");
    return;
  }

  result = cheats_update_entry(sel, code, desc);

  if (result < 0)
  {
    SetDlgItemTextA(dlg, IDC_CHEATS_HINT, "That code could not be saved.");
    return;
  }

  /* Nothing left selected, and the fields emptied, like after Add. Leaving
     the row selected would also mean clicking it again changes nothing, so
     its fields could not be filled in a second time without picking another
     row first. The entry itself stays exactly where it was. */
  cheats_fill_list(dlg, -1);

  SetDlgItemTextA(dlg, IDC_CHEATS_CODE, "");
  SetDlgItemTextA(dlg, IDC_CHEATS_DESC, "");

  cheats_save_current();

  if (!cheats_is_valid(sel))
  {
    SetDlgItemTextA(dlg, IDC_CHEATS_HINT, "Saved, but that code is not valid for the running game.");
  }
  else
  {
    cheats_hint_text(hint, sizeof(hint));
    SetDlgItemTextA(dlg, IDC_CHEATS_HINT, hint);
  }
}

/* Grays out everything in the dialog except the checkbox itself and
   Close when the master toggle is off -- there's nothing useful to do
   with individual cheats while they're all globally disabled. */
static void cheats_update_enable_state(HWND dlg)
{
  static const int controlled[] =
  {
    IDC_CHEATS_DESC, IDC_CHEATS_CODE,
    IDC_CHEATS_ADD, IDC_CHEATS_REMOVE, IDC_CHEATS_CLEAR, IDC_CHEATS_LOADCHT, IDC_CHEATS_FILES
  };
  int i;
  HWND list = GetDlgItem(dlg, IDC_CHEATS_LIST);
  BOOL on = gui.cheats_enabled ? TRUE : FALSE;

  CheckDlgButton(dlg, IDC_CHEATS_ENABLE, on ? BST_CHECKED : BST_UNCHECKED);

  for (i = 0; i < (int)(sizeof(controlled) / sizeof(controlled[0])); i++)
  {
    EnableWindow(GetDlgItem(dlg, controlled[i]), on);
  }
  if (!sim_n) ShowWindow(GetDlgItem(dlg, IDC_CHEATS_FILES), SW_HIDE);

  /* The list is never disabled: a disabled list view keeps its normal text
     colour in light mode and paints itself in system colours (white on
     white) in dark mode. Instead it stays enabled, is kept inert by refusing
     its item changes (see cheats_proc), and is grayed out by its text colour
     on the same background as before. */
  {
    int dark = theme_is_dark();
    COLORREF normal = dark ? RGB(240, 240, 240) : RGB(0, 0, 0);
    COLORREF gray   = dark ? RGB(120, 120, 120) : GetSysColor(COLOR_GRAYTEXT);

    EnableWindow(list, TRUE);
    ListView_SetTextColor(list, on ? normal : gray);
    InvalidateRect(list, NULL, TRUE);
  }

  cheats_update_save_state(dlg);
}

/* Other cheat files in <exe>\cheats that look like they belong to this game ("Sonic The Hedgehog (Action Replay).cht"
   beside "Sonic The Hedgehog (USA).cht"): those whose name starts with the game's title, the part before the first ( or [. */
#define SIM_MAX 32

static void cheats_find_similar(void)
{
  char key[MAX_PATH], self[MAX_PATH], pat[GUI_PATH_LEN];
  WIN32_FIND_DATAA fd;
  HANDLE h;
  const char *base = emu_rom_base();
  int n;

  sim_n = 0;
  if (!base || !base[0]) return;
  lstrcpynA(key, base, sizeof(key));
  { char *p = strpbrk(key, "(["); if (p) *p = '\0'; }
  n = (int)lstrlenA(key);
  while (n > 0 && key[n - 1] == ' ') key[--n] = '\0';
  if (!key[0]) return;
  wsprintfA(self, "%s.cht", base);

  { char f[GUI_PATH_LEN], *sl; emu_console_file_path("cheats", ".cht", f, sizeof(f)); sl = strrchr(f, '\\'); if (sl) *sl = '\0'; lstrcpynA(sim_dir, f, sizeof(sim_dir)); }
  wsprintfA(pat, "%s\\*.cht", sim_dir);
  h = FindFirstFileA(pat, &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do
  {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    if (!lstrcmpiA(fd.cFileName, self)) continue;
    if (CompareStringA(LOCALE_USER_DEFAULT, NORM_IGNORECASE, fd.cFileName, (int)lstrlenA(key), key, (int)lstrlenA(key)) != CSTR_EQUAL) continue;
    if (sim_n < SIM_MAX) lstrcpynA(sim_files[sim_n++], fd.cFileName, MAX_PATH);
  } while (FindNextFileA(h, &fd));
  FindClose(h);
}

static INT_PTR CALLBACK cheats_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
    case WM_INITDIALOG:
    {
      char hint[128];

      cheats_init_list(dlg);
      cheats_fill_list(dlg, -1);
      theme_apply_to_window(dlg);
      {
        HWND header = ListView_GetHeader(GetDlgItem(dlg, IDC_CHEATS_LIST));
        if (header && theme_is_dark())
        {
          SetWindowTheme(header, L"ItemsView", NULL);
          InvalidateRect(header, NULL, TRUE);
        }
      }
      {
        static const GuiTip tips[] =
        {
          { IDC_CHEATS_ENABLE, NULL, "Turns every cheat on or off at once, whatever each cheat's own box says." },
          { IDC_CHEATS_LIST, NULL, "Your cheats. Tick the box in front of a cheat to switch that one on; select one to edit it below." },
          { IDC_CHEATS_DESC, "Name:", "A name to remember the cheat by." },
          { IDC_CHEATS_CODE, "Code:", "A Game Genie, Action Replay or raw code. The hint under the buttons shows what this console accepts." },
          { IDC_CHEATS_ADD, NULL, "Add the name and code above to the list." },
          { IDC_CHEATS_SAVE, NULL, "Save your changes to the name and code above into the selected cheat." },
          { IDC_CHEATS_REMOVE, NULL, "Remove the selected cheat." },
          { IDC_CHEATS_CLEAR, NULL, "Remove every cheat in the list (it asks first)." },
          { IDC_CHEATS_LOADCHT, NULL, "Load cheats from a RetroArch .cht file." },
          { IDC_CHEATS_FILES, NULL, "Other cheat files in the cheats folder whose name starts like this game's. Choose one to add its cheats to the list." },
          { IDC_CHEATS_CONVERT, NULL, "Show a code in the other forms (Game Genie, Action Replay, raw)." },
          { IDOK, NULL, "Close this window." },
        };
        gui_dialog_tips(dlg, tips, (int)(sizeof(tips) / sizeof(tips[0])));
      }
      cheats_hint_text(hint, sizeof(hint));
      SetDlgItemTextA(dlg, IDC_CHEATS_HINT, hint);
      SendDlgItemMessage(dlg, IDC_CHEATS_CODE, EM_LIMITTEXT, CHEAT_CODE_LEN - 1, 0);
      SendDlgItemMessage(dlg, IDC_CHEATS_DESC, EM_LIMITTEXT, CHEAT_DESC_LEN - 1, 0);
      cheats_update_enable_state(dlg);
      {
        /* The drop-down of similar cheat files only exists when there are some. */
        HWND box = GetDlgItem(dlg, IDC_CHEATS_FILES);
        int i;

        cheats_find_similar();
        SendMessage(box, CB_RESETCONTENT, 0, 0);
        SendMessageA(box, CB_ADDSTRING, 0, (LPARAM)"Cheat files with a similar name...");
        for (i = 0; i < sim_n; i++) SendMessageA(box, CB_ADDSTRING, 0, (LPARAM)sim_files[i]);
        SendMessage(box, CB_SETCURSEL, 0, 0);
        ShowWindow(box, sim_n ? SW_SHOW : SW_HIDE);
        EnableWindow(box, sim_n && gui.cheats_enabled);
      }
      return TRUE;
    }

    case WM_DESTROY:
      if (hdr_images) { ImageList_Destroy(hdr_images); hdr_images = NULL; }
      return FALSE;

    case WM_CHEATS_ALL:
      if (gui.cheats_enabled)
      {
        cheats_set_all_enabled(wp ? 1 : 0);
        cheats_fill_list(dlg, CHEATS_KEEP_SELECTION);
      }
      return TRUE;

    case WM_CTLCOLORDLG:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    {
      HWND ctrl = (HWND)lp;
      int ctrl_id = GetDlgCtrlID(ctrl);
      HBRUSH br;

      if (ctrl_id == IDC_CHEATS_CODE || ctrl_id == IDC_CHEATS_DESC)
        br = theme_ctlcolor_custom((HDC)wp, RGB(0x38, 0x38, 0x38), 1);
      else
        br = theme_ctlcolor((HDC)wp);

      if (br) return (LRESULT)br;
      break;
    }

    case WM_NOTIFY:
    {
      NMHDR *hdr = (NMHDR *)lp;
      LRESULT hdr_result = theme_header_customdraw(hdr);

      if (hdr_result != -1)
      {
        SetWindowLongPtr(dlg, DWLP_MSGRESULT, hdr_result);
        return TRUE;
      }

      /* With cheats switched off the list stays enabled (see
         cheats_update_enable_state), so it has to be kept inert here: refuse
         every change a click or key would make. Our own refills are exempt. */
      if (hdr->idFrom == IDC_CHEATS_LIST && hdr->code == LVN_ITEMCHANGING
          && !list_refilling && !gui.cheats_enabled)
      {
        SetWindowLongPtr(dlg, DWLP_MSGRESULT, TRUE);
        return TRUE;
      }

      if (hdr->idFrom == IDC_CHEATS_LIST && hdr->code == LVN_ITEMCHANGED
          && !list_refilling)
      {
        NMLISTVIEW *nm = (NMLISTVIEW *)lp;

        /* State change carries the checkbox in the upper state bits. */
        if (nm->uChanged & LVIF_STATE)
        {
          int was = (int)((nm->uOldState & LVIS_STATEIMAGEMASK) >> 12);
          int now = (int)((nm->uNewState & LVIS_STATEIMAGEMASK) >> 12);

          /* A newly selected cheat is shown in the Name and Code fields. */
          if ((nm->uNewState & LVIS_SELECTED) && !(nm->uOldState & LVIS_SELECTED))
          {
            cheats_show_selected(dlg, nm->iItem);
          }

          if (was && now && was != now)
          {
            cheats_set_enabled(nm->iItem, (now == 2));
            cheats_fill_list(dlg, CHEATS_KEEP_SELECTION);
          }
          else
          {
            cheats_update_save_state(dlg);
          }
        }
      }
      return FALSE;
    }

    case WM_COMMAND:
      switch (LOWORD(wp))
      {
        case IDC_CHEATS_ENABLE:
          gui.cheats_enabled = (IsDlgButtonChecked(dlg, IDC_CHEATS_ENABLE) == BST_CHECKED);
          config_save();
          cheats_apply();
          cheats_update_enable_state(dlg);
          return TRUE;

        case IDC_CHEATS_FILES:
          if (HIWORD(wp) == CBN_SELCHANGE)
          {
            int k = (int)SendDlgItemMessage(dlg, IDC_CHEATS_FILES, CB_GETCURSEL, 0, 0);
            if (k >= 1 && k <= sim_n)
            {
              char path[GUI_PATH_LEN], msg[160];
              int n;

              wsprintfA(path, "%s\\%s", sim_dir, sim_files[k - 1]);
              n = cheats_import_file(path);
              cheats_fill_list(dlg, CHEATS_KEEP_SELECTION);
              if (n < 0) lstrcpynA(msg, "Couldn't open that file.", sizeof(msg));
              else if (n == 0) lstrcpynA(msg, "No cheats found in that file.", sizeof(msg));
              else wsprintfA(msg, "Loaded %d code%s from %s.", n, n == 1 ? "" : "s", sim_files[k - 1]);
              SetDlgItemTextA(dlg, IDC_CHEATS_HINT, msg);
              SendDlgItemMessage(dlg, IDC_CHEATS_FILES, CB_SETCURSEL, 0, 0);
            }
          }
          return TRUE;

        case IDC_CHEATS_ADD:
          cheats_do_add(dlg);
          return TRUE;

        case IDC_CHEATS_SAVE:
          cheats_do_save(dlg);
          return TRUE;

        case IDC_CHEATS_REMOVE:
        {
          HWND list = GetDlgItem(dlg, IDC_CHEATS_LIST);
          int sel = ListView_GetNextItem(list, -1, LVNI_SELECTED);

          if (sel >= 0)
          {
            cheats_remove(sel);
            cheats_fill_list(dlg, -1);
          }
          else
          {
            SetDlgItemTextA(dlg, IDC_CHEATS_HINT, "Select a code to remove it.");
          }
          return TRUE;
        }

        case IDC_CHEATS_CLEAR:
          if (cheat_count &&
              MessageBoxA(dlg, "Remove every code from this list?",
                          "Cheats", MB_YESNO | MB_ICONQUESTION) == IDYES)
          {
            cheats_remove_all();
            cheats_fill_list(dlg, -1);
          }
          return TRUE;

        case IDC_CHEATS_LOADCHT:
        {
          char file[GUI_PATH_LEN] = "";
          char cheat_dir[GUI_PATH_LEN];
          int result;

          /* Always open in ./cheats (next to the exe). */
          CreateDirectoryA(osd_path("cheats"), NULL);
          lstrcpynA(cheat_dir, osd_path("cheats"), sizeof(cheat_dir));

          if (!gui_pick_file(dlg, GUI_PICK_CHEAT, "Load Cheat File (RetroArch .cht format)",
                             "RetroArch cheat files\0*.cht\0All files\0*.*\0\0",
                             cheat_dir, file, sizeof(file)))
            return TRUE;

          result = cheats_import_retroarch_cht(file);
          cheats_fill_list(dlg, CHEATS_KEEP_SELECTION);

          if (result < 0)
          {
            SetDlgItemTextA(dlg, IDC_CHEATS_HINT, "Couldn't open that file.");
          }
          else if (result == 0)
          {
            SetDlgItemTextA(dlg, IDC_CHEATS_HINT,
                            "No cheats found -- expected RetroArch's cheatN_code format.");
          }
          else
          {
            char msg[96];
            wsprintfA(msg, "Loaded %d code%s from file.", result, result == 1 ? "" : "s");
            SetDlgItemTextA(dlg, IDC_CHEATS_HINT, msg);
          }
          return TRUE;
        }

        case IDC_CHEATS_CONVERT:
          gui_modal(gui.large_ui ? IDD_CHEATCONV_LARGE : IDD_CHEATCONV, dlg, cheatconv_proc, 0);
          cheats_fill_list(dlg, CHEATS_KEEP_SELECTION);
          return TRUE;

        case IDOK:
          EndDialog(dlg, IDOK);
          return TRUE;

        case IDCANCEL:
          EndDialog(dlg, IDCANCEL);
          return TRUE;
      }
      return FALSE;

    case WM_CLOSE:
      EndDialog(dlg, IDOK);
      return TRUE;
  }

  return FALSE;
}

void dlg_cheats(HWND parent)
{
  gui_dialog_box(gui.large_ui ? IDD_CHEATS_LARGE : IDD_CHEATS, parent, cheats_proc);

  /* The list is edited live, so there is nothing to commit -- just persist. */
  cheats_save_current();

  if (cheats_effective())
  {
    gui_status("%d cheat%s active", cheats_effective(), (cheats_effective() == 1) ? "" : "s");
  }
  else
  {
    gui_status("No cheats active");
  }
}

/****************************************************************************
 * Cheat Search
 *
 * Finds the memory address of a one-byte value (lives, health, ammo ...) by
 * comparing the work RAM between steps, then turns an address into a RAM
 * cheat. A window of its own that stays open while the game runs.
 ****************************************************************************/

#define CS_MAX_SHOWN 300

static HWND  cs_dlg;
static uint16 cs_snap[0x80000], cs_prev[0x80000];
static uint8 cs_ok[0x80000];
static int   cs_wide;     /* 1 = two-byte values */
static int   cs_size, cs_started, cs_count;
static int   cs_region;   /* 0 = work RAM, 1 = Mega CD PRG-RAM, 2 = Mega CD Word RAM (2M or 1M mode) */

static int cs_md(void)
{
  return (system_hw & SYSTEM_PBC) == SYSTEM_MD;
}

static uint8 cs_read(uint32 a)
{
  uint32 m;
  const uint8 *base = cs_region == 1 ? scd.prg_ram : cs_region == 2 ? wordram_window(&m) : work_ram;
#ifdef LSB_FIRST
  if (cs_md()) a ^= 1;   /* the 16-bit consoles' RAM is kept with every pair of bytes swapped */
#endif
  return base[a];
}

/* The value at address a: one byte, or two (big-endian on the Mega Drive, little-endian on the 8-bit consoles). */
static unsigned cs_cur(uint32 a)
{
  if (!cs_wide) return cs_read(a);
  if (cs_md()) return ((unsigned)cs_read(a) << 8) | cs_read(a + 1);
  return cs_read(a) | ((unsigned)cs_read(a + 1) << 8);
}

/* Mega CD memory can be searched too, while a CD game runs (not a cartridge with the add-on). */
static int cs_cd_memory(void)
{
  return system_hw == SYSTEM_MCD && !scd.cartridge.boot;
}

static void cs_addr_text(uint32 a, char *out, int len)
{
  if (cs_region == 1) gui_snprintf(out, len, "%06X", (unsigned)(a & 0x7FFFF));
  else if (cs_region == 2) gui_snprintf(out, len, "%06X", (unsigned)(0x200000 + (a & 0x3FFFF)));
  else if (cs_md()) gui_snprintf(out, len, "FF%04X", (unsigned)(a & 0xFFFF));
  else gui_snprintf(out, len, "%04X", (unsigned)(0xC000 + (a & 0x1FFF)));
}

static void cs_set_text(const char *fmt, ...)
{
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  wvsprintfA(buf, fmt, ap);
  va_end(ap);
  SetDlgItemTextA(cs_dlg, IDC_CS_COUNT, buf);
}

static void cs_fill_list(void)
{
  HWND list = GetDlgItem(cs_dlg, IDC_CS_LIST);
  int a, row = 0;

  SendMessage(list, WM_SETREDRAW, FALSE, 0);
  ListView_DeleteAllItems(list);
  if (cs_started && cs_count <= CS_MAX_SHOWN)
  {
    for (a = 0; a < cs_size; a++)
    {
      LVITEMA it;
      char t[16];

      if (!cs_ok[a]) continue;
      ZeroMemory(&it, sizeof(it));
      it.mask = LVIF_TEXT | LVIF_PARAM;
      it.iItem = row;
      it.lParam = a;
      cs_addr_text((uint32)a, t, sizeof(t));
      it.pszText = t;
      SendMessageA(list, LVM_INSERTITEMA, 0, (LPARAM)&it);
      it.mask = LVIF_TEXT;
      it.iSubItem = 1;
      gui_snprintf(t, sizeof(t), "%u", cs_cur((uint32)a));
      it.pszText = t;
      SendMessageA(list, LVM_SETITEMTEXTA, (WPARAM)row, (LPARAM)&it);
      it.iSubItem = 2;
      gui_snprintf(t, sizeof(t), "%u", (unsigned)cs_prev[a]);
      it.pszText = t;
      SendMessageA(list, LVM_SETITEMTEXTA, (WPARAM)row, (LPARAM)&it);
      row++;
    }
  }
  SendMessage(list, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(list, NULL, TRUE);

  EnableWindow(GetDlgItem(cs_dlg, IDC_CS_SEARCH), cs_started && cs_count > 0);   /* nothing to narrow down yet */
  /* Enter does the next step: Next Search once there is something to narrow down, else First Search. */
  SendMessage(cs_dlg, DM_SETDEFID, (cs_started && cs_count > 0) ? IDC_CS_SEARCH : IDC_CS_NEW, 0);
  if (!cs_started) cs_set_text("Click First Search to begin.");
  else if (cs_count == 0) cs_set_text("Nothing found. Click First Search to start over.");
  else if (cs_count <= CS_MAX_SHOWN) cs_set_text("%d address%s left.", cs_count, cs_count == 1 ? "" : "es");
  else cs_set_text("%d addresses: click Next Search to narrow it down.", cs_count);
}

static void cs_new(void)
{
  int a;

  if (!cs_cd_memory()) cs_region = 0;
  cs_size = cs_region == 1 ? 0x80000 : cs_region == 2 ? (cheats_wordram_1m() ? 0x20000 : 0x40000) : cs_md() ? 0x10000 : 0x2000;
  cs_count = 0;
  for (a = 0; a < cs_size; a++)
  {
    cs_snap[a] = cs_prev[a] = (uint16)cs_cur((uint32)a);
    /* two-byte values: the Mega Drive keeps them at even addresses, and the last byte has no partner */
    cs_ok[a] = (!cs_wide || (a + 1 < cs_size && (!cs_md() || !(a & 1)))) ? 1 : 0;
    cs_count += cs_ok[a];
  }
  cs_started = 1;
  cs_fill_list();
}

static void cs_search(void)
{
  int mode = (int)SendDlgItemMessage(cs_dlg, IDC_CS_MODE, CB_GETCURSEL, 0, 0), a;
  char buf[16];
  int val = 0;

  if (!cs_started) { cs_set_text("Click First Search first."); return; }
  if (mode == 0)
  {
    GetDlgItemTextA(cs_dlg, IDC_CS_VALUE, buf, sizeof(buf));
    if (!buf[0]) { cs_set_text("Type the value to look for."); return; }
    val = atoi(buf);
    if (val < 0 || val > (cs_wide ? 65535 : 255)) { cs_set_text(cs_wide ? "Only values from 0 to 65535 can be searched." : "Only values from 0 to 255 can be searched."); return; }
  }

  for (a = 0; a < cs_size; a++)
  {
    unsigned cur;
    int keep;

    if (!cs_ok[a]) continue;
    cur = cs_cur((uint32)a);
    switch (mode)
    {
      case 0:  keep = (cur == val); break;
      case 1:  keep = (cur != cs_snap[a]); break;
      case 2:  keep = (cur == cs_snap[a]); break;
      case 3:  keep = (cur > cs_snap[a]); break;
      default: keep = (cur < cs_snap[a]); break;
    }
    cs_prev[a] = cs_snap[a];
    cs_snap[a] = (uint16)cur;
    if (!keep) { cs_ok[a] = 0; cs_count--; }
  }
  cs_fill_list();
}

static void cs_add_cheat(void)
{
  HWND list = GetDlgItem(cs_dlg, IDC_CS_LIST);
  int sel = ListView_GetNextItem(list, -1, LVNI_SELECTED);
  LVITEMA it;
  char name[CHEAT_DESC_LEN], vtxt[16], addr[16], code[CHEAT_CODE_LEN];
  int val;

  if (sel < 0) { cs_set_text("Select an address in the list first."); return; }
  ZeroMemory(&it, sizeof(it));
  it.mask = LVIF_PARAM;
  it.iItem = sel;
  SendMessageA(list, LVM_GETITEMA, 0, (LPARAM)&it);

  GetDlgItemTextA(cs_dlg, IDC_CS_SETVAL, vtxt, sizeof(vtxt));
  if (!vtxt[0]) { cs_set_text("Type the value to hold in Set to."); return; }
  val = atoi(vtxt);
  if (val < 0 || val > (cs_wide ? 65535 : 255)) { cs_set_text(cs_wide ? "Set to: 0 to 65535." : "Set to: 0 to 255."); return; }

  cs_addr_text((uint32)it.lParam, addr, sizeof(addr));
  GetDlgItemTextA(cs_dlg, IDC_CS_NAME, name, sizeof(name));
  if (!name[0]) gui_snprintf(name, sizeof(name), "Cheat search %s", addr);

  if (cs_wide)
  {
    /* A two-byte value is kept as two byte cheats: a code cannot hold a word whose high byte is 0. */
    char code2[CHEAT_CODE_LEN], addr2[16], name2[CHEAT_DESC_LEN];
    int hi = (val >> 8) & 0xFF, lo = val & 0xFF;

    cs_addr_text((uint32)it.lParam + 1, addr2, sizeof(addr2));
    if (cs_md())
    {
      gui_snprintf(code, sizeof(code), "%s:00%02X", addr, hi);
      gui_snprintf(code2, sizeof(code2), "%s:00%02X", addr2, lo);
    }
    else
    {
      gui_snprintf(code, sizeof(code), "%s:%02X", addr, lo);
      gui_snprintf(code2, sizeof(code2), "%s:%02X", addr2, hi);
    }
    if (cheat_count + 2 > CHEAT_MAX) { cs_set_text("The cheat list is full (%d cheats).", CHEAT_MAX); return; }
    {
      char base[128];
      lstrcpynA(base, name, sizeof(base));       /* the name is both read and rewritten: from a copy */
      gui_snprintf(name2, sizeof(name2), "%.100s (2/2)", base);
      gui_snprintf(name, sizeof(name), "%.100s (1/2)", base);
    }
    cheats_add(code, name);
    cheats_add(code2, name2);
    cheats_save_current();
    if (!gui.cheats_enabled) cs_set_text("Added %s and %s, but cheats are switched off: tick Enable Cheats in the Cheats window.", code, code2);
    else cs_set_text("Added %s and %s. Switch them on or off under Cheats.", code, code2);
    return;
  }

  if (cs_md()) gui_snprintf(code, sizeof(code), "%s:00%02X", addr, val);
  else gui_snprintf(code, sizeof(code), "%s:%02X", addr, val);

  if (cheats_add(code, name) < 0) { cs_set_text("The cheat list is full (%d cheats).", CHEAT_MAX); return; }
  cheats_save_current();
  if (!gui.cheats_enabled) cs_set_text("Added %s, but cheats are switched off: tick Enable Cheats in the Cheats window.", code);
  else cs_set_text("Added %s. Switch it on or off under Cheats.", code);
}

static INT_PTR CALLBACK cheatsearch_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
    case WM_INITDIALOG:
    {
      HWND list = GetDlgItem(dlg, IDC_CS_LIST);
      LVCOLUMNA col;
      RECT rc;
      static const char *const modes[] = { "Equal to value", "Changed", "Unchanged", "Increased", "Decreased" };
      static const char *const regions[] = { "Work RAM", "Mega CD PRG-RAM", "Mega CD Word RAM" };
      static const char *const sizes[] = { "1 byte", "2 bytes" };
      static const GuiTip tips[] =
      {
        { IDC_CS_REGION, "Memory:", "Which memory to search. Work RAM is where most games keep lives, health and score; a Mega CD game can also use the CD's own memory. Word RAM is searched in whichever mode the game has it (in its 1M mode only the half the main CPU has at that moment is seen). Changing it starts a new search." },
        { IDC_CS_SIZE, "Size:", "How big the value is: 1 byte (0 to 255: lives, ammo) or 2 bytes (0 to 65535: score, money). A 2-byte value is saved as two cheats. Changing it starts a new search." },
        { IDC_CS_MODE, "Compare:", "How to compare each remaining address with the last time you searched." },
        { IDC_CS_VALUE, "Value:", "For \"Equal to value\": the number the game shows now (lives, ammo ...)." },
        { IDC_CS_NEW, NULL, "Start over: remember the memory as it is now (do this first)." },
        { IDC_CS_SEARCH, NULL, "Keep only the addresses that match; repeat until a few are left. Available after a First Search." },
        { IDC_CS_LIST, NULL, "The addresses left. The value is what is there now." },
        { IDC_CS_NAME, "Name:", "A name for the cheat." },
        { IDC_CS_SETVAL, "Set to:", "The value the cheat keeps the address at (0 to 255, or up to 65535 with Size 2 bytes)." },
        { IDC_CS_ADD, NULL, "Add the selected address to the Cheats list as a RAM cheat." },
        { IDCANCEL, NULL, "Close this window." },
      };
      int i;

      cs_dlg = dlg;
      theme_apply_to_window(dlg);
      SetDlgItemTextA(dlg, IDC_CS_HELP,
        "Finds where a value (lives, health, ammo, or with Size 2 bytes a score) is kept. 1. First Search. 2. Change it in the game "
        "(lose a life ...). 3. Choose Compare (and Value), click Next Search; repeat until a few addresses are left. "
        "4. Select one, type the value to hold in Set to, Add Cheat.");
      for (i = 0; i < 2; i++) SendDlgItemMessageA(dlg, IDC_CS_SIZE, CB_ADDSTRING, 0, (LPARAM)sizes[i]);
      cs_wide = 0;
      SendDlgItemMessage(dlg, IDC_CS_SIZE, CB_SETCURSEL, 0, 0);
      for (i = 0; i < 5; i++) SendDlgItemMessageA(dlg, IDC_CS_MODE, CB_ADDSTRING, 0, (LPARAM)modes[i]);
      for (i = 0; i < 3; i++) SendDlgItemMessageA(dlg, IDC_CS_REGION, CB_ADDSTRING, 0, (LPARAM)regions[i]);
      cs_region = 0;
      SendDlgItemMessage(dlg, IDC_CS_REGION, CB_SETCURSEL, 0, 0);
      EnableWindow(GetDlgItem(dlg, IDC_CS_REGION), cs_cd_memory());
      SendDlgItemMessage(dlg, IDC_CS_MODE, CB_SETCURSEL, 0, 0);
      SendMessage(list, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT);
      GetClientRect(list, &rc);
      ZeroMemory(&col, sizeof(col));
      col.mask = LVCF_TEXT | LVCF_WIDTH;
      col.pszText = "Address";  col.cx = (rc.right - rc.left) * 36 / 100;
      SendMessageA(list, LVM_INSERTCOLUMNA, 0, (LPARAM)&col);
      col.pszText = "Value now"; col.cx = (rc.right - rc.left) * 30 / 100;
      SendMessageA(list, LVM_INSERTCOLUMNA, 1, (LPARAM)&col);
      col.pszText = "Before";  col.cx = (rc.right - rc.left) * 30 / 100;
      SendMessageA(list, LVM_INSERTCOLUMNA, 2, (LPARAM)&col);
      theme_apply_to_window(dlg);
      {
        HWND header = ListView_GetHeader(list);
        if (header && theme_is_dark()) { SetWindowTheme(header, L"ItemsView", NULL); InvalidateRect(header, NULL, TRUE); }
      }
      gui_dialog_tips(dlg, tips, (int)(sizeof(tips) / sizeof(tips[0])));
      cs_started = 0;
      cs_fill_list();
      return TRUE;
    }

    case WM_CTLCOLORDLG:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    {
      int id = GetDlgCtrlID((HWND)lp);
      HBRUSH br;
      if (id == IDC_CS_VALUE || id == IDC_CS_NAME || id == IDC_CS_SETVAL)
        br = theme_ctlcolor_custom((HDC)wp, RGB(0x38, 0x38, 0x38), 1);
      else
        br = theme_ctlcolor((HDC)wp);
      if (br) return (LRESULT)br;
      break;
    }

    case WM_NOTIFY:
    {
      NMHDR *hdr = (NMHDR *)lp;
      LRESULT hr = theme_header_customdraw(hdr);

      if (hr != -1)
      {
        SetWindowLongPtr(dlg, DWLP_MSGRESULT, hr);
        return TRUE;
      }
      if (hdr->idFrom == IDC_CS_LIST && hdr->code == LVN_ITEMCHANGED)
      {
        NMLISTVIEW *nm = (NMLISTVIEW *)lp;
        if ((nm->uChanged & LVIF_STATE) && (nm->uNewState & LVIS_SELECTED))
        {
          LVITEMA it;
          char t[16];
          ZeroMemory(&it, sizeof(it));
          it.mask = LVIF_PARAM;
          it.iItem = nm->iItem;
          SendMessageA(hdr->hwndFrom, LVM_GETITEMA, 0, (LPARAM)&it);
          gui_snprintf(t, sizeof(t), "%u", cs_cur((uint32)it.lParam));
          SetDlgItemTextA(dlg, IDC_CS_SETVAL, t);
        }
      }
      break;
    }

    case WM_COMMAND:
      switch (LOWORD(wp))
      {
        case IDC_CS_REGION:
          if (HIWORD(wp) == CBN_SELCHANGE)
          {
            cs_region = (int)SendDlgItemMessage(dlg, IDC_CS_REGION, CB_GETCURSEL, 0, 0);
            if (cs_region < 0 || cs_region > 2) cs_region = 0;
            cs_started = 0;          /* a different memory: a new search */
            cs_fill_list();
          }
          return TRUE;
        case IDC_CS_SIZE:
          if (HIWORD(wp) == CBN_SELCHANGE)
          {
            cs_wide = (int)SendDlgItemMessage(dlg, IDC_CS_SIZE, CB_GETCURSEL, 0, 0) == 1;
            cs_started = 0;          /* a different size: a new search */
            cs_fill_list();
          }
          return TRUE;
        case IDC_CS_NEW:    cs_new(); return TRUE;
        case IDC_CS_SEARCH: cs_search(); return TRUE;
        case IDC_CS_ADD:    cs_add_cheat(); return TRUE;
        case IDCANCEL:
        case IDOK:
          DestroyWindow(dlg);
          return TRUE;
      }
      break;

    case WM_CLOSE:
      DestroyWindow(dlg);
      return TRUE;

    case WM_DESTROY:
      cs_dlg = NULL;
      cs_started = 0;
      return FALSE;
  }

  return FALSE;
}

void dlg_cheat_search(HWND parent)
{
  if (cs_dlg)
  {
    SetForegroundWindow(cs_dlg);
    return;
  }
  cs_dlg = CreateDialogParamA(g_inst, MAKEINTRESOURCEA(gui.large_ui ? IDD_CHEATSEARCH_LARGE : IDD_CHEATSEARCH),
                              parent, cheatsearch_proc, 0);
  if (cs_dlg) { gui_center_window(cs_dlg); gui_window_icon(cs_dlg); ShowWindow(cs_dlg, SW_SHOW); }
}

int cheat_search_message(MSG *msg)
{
  return cs_dlg && IsDialogMessage(cs_dlg, msg);
}

void cheat_search_close(void)
{
  if (cs_dlg) DestroyWindow(cs_dlg);
}

/* The game was reset or a state was loaded: what the search remembered no longer fits. */
void cheat_search_invalidate(void)
{
  if (!cs_dlg) return;
  cs_started = 0;
  cs_fill_list();
  cs_set_text("The game was reset or a state was loaded: click First Search to start again.");
}

/* While a modal window (Settings, Controls ...) is open the search window is switched off too, so
   it does not sit there clickable with its keyboard handling out of reach. */
void cheat_search_enable(int on)
{
  if (cs_dlg) EnableWindow(cs_dlg, on ? TRUE : FALSE);
}

HWND cheat_search_hwnd(void)
{
  return cs_dlg;
}
