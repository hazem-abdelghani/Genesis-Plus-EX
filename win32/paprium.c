/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  paprium.c -- Paprium
 *
 *  The cartridge of Paprium (Watermelon Development, product code T-574120-00)
 *  carries a chip of its own that decodes the game's graphics, plays its sound
 *  effects and runs its music. This is a high-level emulation of it: the commands
 *  the game sends to the chip are carried out here, instead of the chip being
 *  emulated.
 *
 *  The code comes from the Genesis-Plus-GX-Expanded-Rom-Size fork of Genesis Plus
 *  GX by BillyTimeGames (the file there is signed "Project Little Man"). The game's
 *  music is not emulated: it is played from the MP3 files of the game's soundtrack,
 *  which have to be put in a "paprium" folder next to the game.
 *
 *  It is attached to the core from outside, without a change to core/: the
 *  linker redirects a few of the core's own functions (see the Makefile, --wrap)
 *  to the __wrap_ functions at the end of this file.
 ****************************************************************************/

#include <string.h>
#include "shared.h"
#include "eeprom_spi.h"
#include "minimp3_ex.h"        /* its code is in core/cart_hw/yx5200.c */
#include "gui.h"
#include "sevenzip.h"

static mp3dec_t paprium_mp3d;
static mp3dec_file_info_t paprium_mp3d_info;

static mp3dec_t paprium_mp3d_boss1;
static mp3dec_file_info_t paprium_mp3d_info_boss1;

static mp3dec_t paprium_mp3d_boss2;
static mp3dec_file_info_t paprium_mp3d_info_boss2;

static mp3dec_t paprium_mp3d_boss3;
static mp3dec_file_info_t paprium_mp3d_info_boss3;

static mp3dec_t paprium_mp3d_boss4;
static mp3dec_file_info_t paprium_mp3d_info_boss4;

static int paprium_track_last;



#define PAPRIUM_BOSS1 0x17
#define PAPRIUM_BOSS2 0x21
#define PAPRIUM_BOSS3 0x22
#define PAPRIUM_BOSS4 0x23


static int paprium_active;                    /* the game in the core is Paprium */
static char paprium_rom_dir[GUI_PATH_LEN];    /* the folder of the game */
static char paprium_dir[GUI_PATH_LEN + 16];   /* the folder the music is read from, ending in a backslash */


static uint8 paprium_obj_ram[0x80000];

static int paprium_tmss = 1;

static int paprium_music_ptr;
static int paprium_sfx_ptr;
static int paprium_tile_ptr;
static int paprium_sprite_ptr;


typedef struct paprium_voice_t
{
	int volume;
	int panning;
	int flags;
	int type;

	int size;
	int ptr;
	int tick;

	int loop;
	int echo;
	int program;


	int count;

	int time;

	int start;
	int num;

	int decay;
	int decay2;
	int release;
	int sustain;

	int duration;
	int velocity;
	int keyon;
	int key_type;
	int pitch;
	int cents;
	int modulation;
} paprium_voice_t;


static struct paprium_t
{
	uint8 ram[0x10000];
	uint8 decoder_ram[0x10000];
	uint8 scaler_ram[0x1000];
	uint8 music_ram[0x8000];
	uint8 exps_ram[14*8];

	paprium_voice_t sfx[8];
	paprium_voice_t music[26];

	int music_section, audio_tick, music_segment;

	int out_l, out_r;
	int audio_flags, sfx_volume, music_volume;

	int decoder_mode, decoder_ptr, decoder_size;
	int draw_src, draw_dst;
	int obj[0x31];

	int echo_ptr, echo_pan;

	int music_track, mp3_ptr, music_tick;
} paprium_s;


/* The echo's delay lines: not in a saved state (it would not fit in the space of one), they start empty after a load. */
static int paprium_echo_l[48000/4], paprium_echo_r[48000/4];


static const uint8 paprium_volume_table[256] =
{
    0x00, 0x03, 0x07, 0x0B, 0x0E, 0x12, 0x15, 0x18, 0x1B, 0x1E, 0x21, 0x24, 0x27, 0x29, 0x2C, 0x2F, 
    0x31, 0x34, 0x36, 0x38, 0x3B, 0x3D, 0x3F, 0x41, 0x44, 0x46, 0x48, 0x4A, 0x4C, 0x4E, 0x50, 0x51, 
    0x53, 0x55, 0x57, 0x59, 0x5A, 0x5C, 0x5E, 0x5F, 0x61, 0x63, 0x64, 0x66, 0x67, 0x69, 0x6A, 0x6C, 
    0x6D, 0x6F, 0x70, 0x72, 0x73, 0x74, 0x76, 0x77, 0x78, 0x7A, 0x7B, 0x7C, 0x7E, 0x7F, 0x80, 0x81, 
    0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F, 0x90, 0x91, 0x92, 0x93, 
    0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F, 0xA0, 0xA1, 0xA2, 0xA3, 
    0xA4, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xAF, 0xB0, 
    0xB1, 0xB2, 0xB3, 0xB3, 0xB4, 0xB5, 0xB6, 0xB6, 0xB7, 0xB8, 0xB9, 0xB9, 0xBA, 0xBB, 0xBC, 0xBC, 
    0xBD, 0xBE, 0xBE, 0xBF, 0xC0, 0xC1, 0xC1, 0xC2, 0xC3, 0xC3, 0xC4, 0xC5, 0xC5, 0xC6, 0xC7, 0xC7, 
    0xC8, 0xC9, 0xC9, 0xCA, 0xCA, 0xCB, 0xCC, 0xCC, 0xCD, 0xCE, 0xCE, 0xCF, 0xCF, 0xD0, 0xD1, 0xD1, 
    0xD2, 0xD2, 0xD3, 0xD3, 0xD4, 0xD5, 0xD5, 0xD6, 0xD6, 0xD7, 0xD7, 0xD8, 0xD9, 0xD9, 0xDA, 0xDA, 
    0xDB, 0xDB, 0xDC, 0xDC, 0xDD, 0xDD, 0xDE, 0xDF, 0xDF, 0xE0, 0xE0, 0xE1, 0xE1, 0xE2, 0xE2, 0xE3, 
    0xE3, 0xE4, 0xE4, 0xE5, 0xE5, 0xE6, 0xE6, 0xE7, 0xE7, 0xE8, 0xE8, 0xE9, 0xE9, 0xEA, 0xEA, 0xEA, 
    0xEB, 0xEB, 0xEC, 0xEC, 0xED, 0xED, 0xEE, 0xEE, 0xEF, 0xEF, 0xF0, 0xF0, 0xF0, 0xF1, 0xF1, 0xF2, 
    0xF2, 0xF3, 0xF3, 0xF4, 0xF4, 0xF4, 0xF5, 0xF5, 0xF6, 0xF6, 0xF7, 0xF7, 0xF7, 0xF8, 0xF8, 0xF9, 
    0xF9, 0xF9, 0xFA, 0xFA, 0xFB, 0xFB, 0xFC, 0xFC, 0xFC, 0xFD, 0xFD, 0xFE, 0xFE, 0xFE, 0xFF, 0xFF, 
};


/* Where the soundtrack is: a "paprium" folder next to the game. */
static void paprium_dir_set(void)
{
	if( paprium_rom_dir[0] )
		wsprintfA(paprium_dir, "%s\\paprium\\", paprium_rom_dir);
	else
		lstrcpynA(paprium_dir, "paprium\\", sizeof(paprium_dir));
}


static void paprium_mp3_free(mp3dec_file_info_t *info)
{
	free(info->buffer);
	memset(info, 0, sizeof(*info));
}


/* Gives back the music of the game (the tracks are decoded to memory, tens of megabytes each). */
static void paprium_mp3_free_all(void)
{
	paprium_mp3_free(&paprium_mp3d_info);
	paprium_mp3_free(&paprium_mp3d_info_boss1);
	paprium_mp3_free(&paprium_mp3d_info_boss2);
	paprium_mp3_free(&paprium_mp3d_info_boss3);
	paprium_mp3_free(&paprium_mp3d_info_boss4);
}


static void paprium_file(char *out, const char *file)
{
	gui_snprintf(out, 512, "%s%s", paprium_dir, file);
}


static char paprium_archive[GUI_PATH_LEN];     /* the .zip or .7z the game was opened from: the soundtrack may be in it too ("" for a plain file) */


static const char *paprium_base(const char *path)
{
	const char *b = path, *p;

	for( p = path; *p; p++ )
		if( *p == '\\' || *p == '/' ) b = p + 1;
	return b;
}


static unzFile paprium_zf;                      /* the archive of the game, kept open while its game runs (a track is looked for in it each time) */
#ifdef USE_LIBCHDR
static sevenzip_t *paprium_sz;
#endif


/* Lets go of the archive (a game ends, or another is loaded). */
static void paprium_archive_close(void)
{
	if( paprium_zf ) { unzClose(paprium_zf); paprium_zf = NULL; }
#ifdef USE_LIBCHDR
	if( paprium_sz ) { sevenzip_close(paprium_sz); paprium_sz = NULL; }
#endif
}


/* A track of the soundtrack in the archive of the game (the entry with that file name, in whatever folder it is). Returns the file
   in memory (free it), or NULL. */
static uint8 *paprium_archive_read(const char *name, size_t *size)
{
	const char *want = paprium_base(name), *dot = strrchr(paprium_archive, '.');
	uint8 *buf = NULL;

	*size = 0;
	if( !paprium_archive[0] || !dot )
		return NULL;

	if( !lstrcmpiA(dot, ".zip") ) {
		int r;

		if( !paprium_zf )
			paprium_zf = unzOpen(paprium_archive);
		if( !paprium_zf )
			return NULL;
		for( r = unzGoToFirstFile(paprium_zf); r == UNZ_OK; r = unzGoToNextFile(paprium_zf) ) {
			char nm[512];
			unz_file_info info;

			if( unzGetCurrentFileInfo(paprium_zf, &info, nm, sizeof(nm), NULL, 0, NULL, 0) != UNZ_OK || lstrcmpiA(paprium_base(nm), want) )
				continue;
			if( info.uncompressed_size && unzOpenCurrentFile(paprium_zf) == UNZ_OK ) {
				buf = (uint8 *)malloc(info.uncompressed_size);
				if( buf && unzReadCurrentFile(paprium_zf, buf, (unsigned)info.uncompressed_size) == (int)info.uncompressed_size )
					*size = (size_t)info.uncompressed_size;
				else { free(buf); buf = NULL; }
				unzCloseCurrentFile(paprium_zf);
			}
			break;
		}
	}
#ifdef USE_LIBCHDR
	else if( !lstrcmpiA(dot, ".7z") ) {
		int i, n;

		if( !paprium_sz )
			paprium_sz = sevenzip_open(paprium_archive);
		if( !paprium_sz )
			return NULL;
		n = sevenzip_count(paprium_sz);
		for( i = 0; i < n; i++ ) {
			char nm[512];
			unsigned int len;

			if( sevenzip_entry_is_dir(paprium_sz, i) )
				continue;
			sevenzip_entry_name(paprium_sz, i, nm, sizeof(nm));
			if( lstrcmpiA(paprium_base(nm), want) )
				continue;
			len = sevenzip_entry_size(paprium_sz, i);
			if( len ) {
				buf = (uint8 *)malloc(len);
				if( buf && sevenzip_extract(paprium_sz, i, buf, (int)len) == (int)len )
					*size = len;
				else { free(buf); buf = NULL; }
			}
			break;
		}
	}
#endif
	return buf;
}


/* Loads a track like mp3dec_load: from the folder of the music, else from the archive of the game. 0 when it worked. */
static int paprium_mp3_open(mp3dec_t *dec, const char *name, mp3dec_file_info_t *info)
{
	size_t size;
	uint8 *buf;
	int r;

	if( GetFileAttributesA(name) != INVALID_FILE_ATTRIBUTES )
		return mp3dec_load(dec, name, info, NULL, NULL);

	buf = paprium_archive_read(name, &size);
	if( !buf )
		return mp3dec_load(dec, name, info, NULL, NULL);       /* (not there: the usual way says so) */
	r = mp3dec_load_buf(dec, buf, size, info, NULL, NULL);
	free(buf);
	return r;
}


static void paprium_load_mp3(int track, int reload)
{
	static char name[512];

	paprium_s.music_track = track;

	paprium_dir_set();

	switch(track) {
	case 0x01: paprium_file(name, "02 90's Acid Dub Character Select.mp3"); break;
	case 0x02: paprium_file(name, "08 90's Dance.mp3"); break;
	case 0x03: paprium_file(name, "42 1988 Commercial.mp3"); break;
	case 0x04: paprium_file(name, "05 Asian Chill.mp3"); break;
	case 0x05: paprium_file(name, "31 Bad Dudes vs Paprium.mp3"); break;
	case 0x06: paprium_file(name, "43 Blade FM.mp3"); break;
	case 0x07: paprium_file(name, "03 Bone Crusher.mp3"); break;
	case 0x0B: paprium_file(name, "26 Club Shuffle.mp3"); break;
	case 0x0C: paprium_file(name, "23 Continue.mp3"); break;  /* Continue Fast */
	case 0x0E: paprium_file(name, "07 Cool Groove.mp3"); break;
	case 0x0F: paprium_file(name, "36 Cyberpunk Ninja.mp3"); break;
	case 0x10: paprium_file(name, "35 Cyberpunk Funk.mp3"); break;
	case 0x11: paprium_file(name, "30 Cyber Interlude.mp3"); break;
	case 0x12: paprium_file(name, "21 Cyborg Invasion.mp3"); break;
	case 0x13: paprium_file(name, "44 Dark Alley.mp3"); break;
	case 0x14: paprium_file(name, "29 Dark & Power Mad.mp3"); break;
	case 0x15: paprium_file(name, "24 Intro.mp3"); break;  /* Dark Bounce */
	case 0x16: paprium_file(name, "27 Dark Rock.mp3"); break;
	case 0x17: paprium_file(name, "04 Drumbass Boss.mp3"); break;
	case 0x18: paprium_file(name, "45 Dubstep Groove.mp3"); break;
	case 0x19: paprium_file(name, "15 Electro Acid Funk.mp3"); break;
	case 0x1B: paprium_file(name, "28 Evolve.mp3"); break;
	case 0x1C: paprium_file(name, "33 Funk Enhanced Mix.mp3"); break;
	case 0x1D: paprium_file(name, "41 Game Over.mp3"); break;
	case 0x1E: paprium_file(name, "46 Gothic.mp3"); break;
	case 0x20: paprium_file(name, "13 Hard Rock.mp3"); break;
	case 0x21: paprium_file(name, "22 Hardcore BP1.mp3"); break;
	case 0x22: paprium_file(name, "11 Hardcore BP2.mp3"); break;
	case 0x23: paprium_file(name, "38 Hardcore BP3.mp3"); break;
	case 0x24: paprium_file(name, "40 Score.mp3"); break;  /* High Score */
	case 0x25: paprium_file(name, "47 House.mp3"); break;
	case 0x26: paprium_file(name, "17 Indie Shuffle.mp3"); break;
	case 0x27: paprium_file(name, "25 Indie Break Beat.mp3"); break;
	case 0x28: paprium_file(name, "16 Jazzy Shuffle.mp3"); break;
	case 0x2A: paprium_file(name, "19 Neo Metal.mp3"); break;
	case 0x2B: paprium_file(name, "14 Neon Rider.mp3"); break;
	case 0x2E: paprium_file(name, "09 Retro Beat.mp3"); break;
	case 0x2F: paprium_file(name, "20 Sadness.mp3"); break;
	case 0x31: paprium_file(name, "18 Slow Asian Beat.mp3"); break;
	case 0x32: paprium_file(name, "48 Slow Mood.mp3"); break;  /* Slow Mood Ext. ? */
	case 0x33: paprium_file(name, "49 Smooth Coords.mp3"); break;
	case 0x34: paprium_file(name, "10 Spiral.mp3"); break;
	case 0x35: paprium_file(name, "12 Stage Clear.mp3"); break;
	case 0x36: paprium_file(name, "32 Summer Breeze.mp3"); break;
	case 0x37: paprium_file(name, "06 Techno Beats.mp3"); break;
	case 0x38: paprium_file(name, "50 Tension.mp3"); break;
	case 0x39: paprium_file(name, "01 Theme of Paprium.mp3"); break;
	case 0x3A: paprium_file(name, "39 Ending.mp3"); break;  /* Tough Guy */
	case 0x3B: paprium_file(name, "34 Transe.mp3"); break;
	case 0x3C: paprium_file(name, "37 Urban.mp3"); break;
	case 0x3D: paprium_file(name, "51 Water.mp3"); break;
	case 0x3E: paprium_file(name, "52 Waterfront Beat.mp3"); break;
	default: paprium_s.music_track = 0; return;
	}

	if( reload )
		paprium_s.mp3_ptr = 0;

	paprium_s.music_tick = 0;

	if( track != PAPRIUM_BOSS1 && track != PAPRIUM_BOSS2 && track != PAPRIUM_BOSS3 && track != PAPRIUM_BOSS4 ) {
		paprium_mp3_free(&paprium_mp3d_info);

		if( paprium_mp3_open(&paprium_mp3d, name, &paprium_mp3d_info) ) {
			paprium_mp3_free(&paprium_mp3d_info);
			paprium_s.music_track = 0;
			return;
		}
	}
}


static void paprium_load_mp3_boss()
{
	static char name[512];

	paprium_dir_set();

	paprium_file(name, "04 Drumbass Boss.mp3");
	if( !paprium_mp3d_info_boss1.buffer && paprium_mp3_open(&paprium_mp3d_boss1, name, &paprium_mp3d_info_boss1) ) paprium_mp3_free(&paprium_mp3d_info_boss1);

	paprium_file(name, "22 Hardcore BP1.mp3");
	if( !paprium_mp3d_info_boss2.buffer && paprium_mp3_open(&paprium_mp3d_boss2, name, &paprium_mp3d_info_boss2) ) paprium_mp3_free(&paprium_mp3d_info_boss2);

	paprium_file(name, "11 Hardcore BP2.mp3");
	if( !paprium_mp3d_info_boss3.buffer && paprium_mp3_open(&paprium_mp3d_boss3, name, &paprium_mp3d_info_boss3) ) paprium_mp3_free(&paprium_mp3d_info_boss3);

	paprium_file(name, "38 Hardcore BP3.mp3");
	if( !paprium_mp3d_info_boss4.buffer && paprium_mp3_open(&paprium_mp3d_boss4, name, &paprium_mp3d_info_boss4) ) paprium_mp3_free(&paprium_mp3d_info_boss4);
}


static void paprium_music_sheet()
{
	int ch;

	/* 00-04 = WMMM */
	/* 05, 06, 07 */
	int sections = paprium_s.music_ram[0x09];
	/* 09 */
	int bars = (paprium_s.music_ram[0x0B] ? paprium_s.music_ram[0x0B] : 0x100) + 8;
	/* 0B */
	int cmds = paprium_s.music_ram[0x0D];
	/* 10-29 */
	/* 2A-43 */
	/* 44-5D */
	/* 5E-77 */
	/* 78-97 = title ^ 0xA5 */
	/* 98-B7 = author ^ 0xA5 */
	/* B8-D7 = comment ^ 0xA5 */


	if( paprium_s.music_segment == -1 ) {
		paprium_s.music_track = 0;
		return;
	}

	if( paprium_s.audio_tick < 4 )
		return;
	paprium_s.audio_tick = 0;




	for( ch = 0; ch < 26; ch++ ) {
		int ptr = *(uint16*)(paprium_s.music_ram + 0xD8 + (ch*sections*2) + (paprium_s.music_segment*2));
		int index = paprium_s.music_ram[(ptr + paprium_s.music_section)^1];
		int code = 0, arg = 0;
		int keyon = 0, freq = -1;
		paprium_voice_t *voice = paprium_s.music + ch;




		if( index == 0 )
			goto next;
		ptr += bars + (index-1) * 8;


		for( int lcv = 0; lcv < cmds; lcv++ ) {
			code = *(uint16*)(paprium_s.music_ram + ptr + lcv*2);
			arg = code & 0xFF;
			code >>= 8;


			if( code == 0x00 )
				{} //break;

			else if( code == 0x01 ) {  /* ?? */
				if( ch >= 11-1 ) keyon = code;
				voice->volume = 255 - paprium_volume_table[arg];  /* z80 table ? */
			}

			else if( code == 0x02 )
				voice->panning = arg;

			else if( code == 0x03 ) {
				voice->volume = 255 - paprium_volume_table[arg];  /* z80 table ? */
			}

			else if( code == 0x05 ) {
				voice->volume = 255 - paprium_volume_table[arg];  /* z80 table ? */
			}

			/* velocity */

			else if( code == 0x08 ) {

				freq = arg;
			}

			else if( code == 0x0A ) {
				freq = 0;  /* faster ? */
			}

			else if( code == 0x0B ) {
				//MessageBoxA(0, paprium_dir, "---", 0);
			}

			else if( code == 0x0C ) {
				//MessageBoxA(0, paprium_dir, "---", 0);
			}

			else if( code == 0x0E ) {  /* stop ? */
				keyon = 0;
				voice->size = 0;
			}

			else if( code == 0x0F ) {  /* program */
				voice->program = arg;

				if( ch < 6 ) {  /* YM */
				}

				else if( ch < 10 ) {  /* PSG */
				}

				else
				{  /* wave */
				}

			}
		}


		if( keyon ) {
		}

		if( freq >= 0 ) {
			voice->type = freq;
			//if(ch == 16-1) MessageBoxA(0,"hit","-",0);
		}

next:
		*(uint16*)(paprium_s.ram + ch*4 + 0x1B98) = index ? 0xE0 : 0;  /* L */
		*(uint16*)(paprium_s.ram + ch*4 + 0x1B9A) = index ? 0xE0 : 0;  /* R */
	}


	if( (++paprium_s.music_section) >= bars ) {
		paprium_s.music_section = 0;

		if( (++paprium_s.music_segment) >= sections )
			paprium_s.music_segment = 0;
	}
}


/* The soundtrack file of the track playing now, or NULL when it was not found (the music is then silent). */
static mp3dec_file_info_t *paprium_music_info(void)
{
	mp3dec_file_info_t *info;

	switch( paprium_s.music_track ) {
	case PAPRIUM_BOSS1: info = &paprium_mp3d_info_boss1; break;
	case PAPRIUM_BOSS2: info = &paprium_mp3d_info_boss2; break;
	case PAPRIUM_BOSS3: info = &paprium_mp3d_info_boss3; break;
	case PAPRIUM_BOSS4: info = &paprium_mp3d_info_boss4; break;
	default: info = &paprium_mp3d_info; break;
	}

	return (info->buffer && info->samples >= 2 && info->channels >= 1 && info->hz > 0) ? info : NULL;
}


/* One sample of the music: the track is played at its own rate (resampled to 48000) and wraps around at its end. */
static void paprium_music_synth(int *out_l, int *out_r)
{
	int l = 0, r = 0;

	if( paprium_s.music_track ) {
		const mp3dec_file_info_t *info = paprium_music_info();

		if( info ) {
			int ch = (info->channels > 1) ? 2 : 1;

			if( paprium_s.mp3_ptr < 0 || (size_t)paprium_s.mp3_ptr + ch > info->samples )
				paprium_s.mp3_ptr = 0;

			l = info->buffer[paprium_s.mp3_ptr];
			r = info->buffer[paprium_s.mp3_ptr + ch - 1];

			l = (l * paprium_s.music_volume) / 256;
			r = (r * paprium_s.music_volume) / 256;

			paprium_s.music_tick += (int)(((uint32)info->hz << 16) / 48000);
			while( paprium_s.music_tick >= 0x10000 ) {
				paprium_s.music_tick -= 0x10000;
				paprium_s.mp3_ptr += ch;
			}

			if( (size_t)paprium_s.mp3_ptr >= info->samples )
				paprium_s.mp3_ptr = 0;
		}
	}

	*out_l = l;
	*out_r = r;
}


static void paprium_sfx_voice(int *out_l, int *out_r)
{
	int ch;
	int l = 0, r = 0;

	for( ch = 0; ch < 8; ch++ ) {
		paprium_voice_t *voice = paprium_s.sfx + ch;
		const int _rates[] = {1,2,4,5,8,9};  /* 48000, 24000, 12000, 9600, 6000, 5333 */

		int rate = _rates[voice->type >> 4] << 16;  /* 16.16 */
		int depth = voice->type & 0x03;
		/* voice->type & 0xC0; */

		int vol = voice->volume;
		int pan = voice->panning;

		int sample, sample_l, sample_r;


		if( voice->size == 0 ) goto next;


		sample = *(uint8 *)(cart.rom + paprium_sfx_ptr + (voice->ptr^1));

		if( depth == 1 )
			sample = (((sample & 0xFF) * 65536) / 256) - 32768;

		else if( depth == 2 ) {
			if( voice->count == 0 )
				sample >>= 4;

			sample = (((sample & 0x0F) * 65536) / 16) - 32768;
		}


		sample = (sample * vol) / 0x400;

		sample_l = (sample * ((pan <= 0x80) ? 0x80 : 0x100 - pan)) / 0x80;
		sample_r = (sample * ((pan >= 0x80) ? 0x80 : pan)) / 0x80;

		l += sample_l;
		r += sample_r;


		if( voice->flags & 0x4000 ) {  /* echo */
			if( voice->echo & 1 )
				paprium_echo_l[paprium_s.echo_ptr % (48000/6)] += (sample_l * 33) / 100;
			else
				paprium_echo_r[paprium_s.echo_ptr % (48000/6)] += (sample_r * 33) / 100;
		}


		if( voice->flags & 0x100 ) {  /* amplify */
			l = (l * 125) / 100;
			r = (r * 125) / 100;
		}

		voice->time++;

		voice->tick += 0x10000;
		voice->tick -= (voice->flags & 0x8000) ? 0x800 : 0;  /* tiny pitch */
		voice->tick -= (voice->flags & 0x2000) ? 0x8000 : 0;  /* huge pitch */


		if( voice->tick >= rate ) {
			voice->tick -= rate;

			voice->count++;
			voice->size--;

			if( voice->count >= depth ) {
				voice->ptr++;

				voice->count = 0;
			}
		}
 

next:
		if( voice->size == 0 ) {
			voice->count = 0;

			if( voice->loop ) {
				uint8 *sfx = paprium_sfx_ptr + cart.rom;

				voice->ptr = (*(uint16 *)(sfx + voice->num*8) << 16) | (*(uint16 *)(sfx + voice->num*8 + 2));
				voice->size = (*(uint8 *)(sfx + voice->num*8 + 4) << 16) | (*(uint16 *)(sfx + voice->num*8 + 6));
			}
		}
	}

	*out_l += l;
	*out_r += r;
}


static void paprium_audio(int cycles)
{
	int lcv;
	int samples = blip_clocks_needed(snd.blips[3], cycles);




	if( paprium_s.music_track != paprium_track_last ) {
		paprium_load_mp3(paprium_s.music_track, 0);
	}


	paprium_s.audio_tick++;


	for( lcv = 0; lcv < samples; lcv++ ) {
		int l = 0, r = 0;


		paprium_echo_l[paprium_s.echo_ptr] = 0;
		paprium_echo_r[paprium_s.echo_ptr] = 0;


		paprium_music_synth(&l, &r);
		paprium_sfx_voice(&l, &r);


		paprium_s.echo_ptr = (paprium_s.echo_ptr+1) % (48000/6);

		l += paprium_echo_l[paprium_s.echo_ptr];
		r += paprium_echo_r[paprium_s.echo_ptr];


		l = (l * paprium_s.sfx_volume) / 0x100;
		r = (r * paprium_s.sfx_volume) / 0x100;


		if( paprium_s.audio_flags & 0x08 ) {  /* gain */
			l = l * 4 / 2;
			r = r * 4 / 2;
		}


		if( l > 32767 ) l = 32767;
		else if( l < -32768 ) l = -32768;

		if( r > 32767 ) r = 32767;
		else if( r < -32768 ) r = -32768;


		blip_add_delta_fast(snd.blips[3], lcv, l-paprium_s.out_l, r-paprium_s.out_r);


		paprium_s.out_l = l;
		paprium_s.out_r = r;
	}


	paprium_music_sheet();




	blip_end_frame(snd.blips[3], samples);


	paprium_track_last = paprium_s.music_track;
}


static void paprium_decoder_lz_rle(uint src, uint8 *dst)
{
	int size = 0;
	int len, lz = 0, rle = 0, code;




	while(1) {
		int type = cart.rom[(src++)^1];

		code = type >> 6;
		len = type & 0x3F;


		if( (code == 0) && (len == 0))
			break;

		else if( code == 1 )
			rle = cart.rom[(src++)^1];

		else if( code == 2 )
			lz = size - cart.rom[(src++)^1];


		while( len-- > 0 ) {
			switch(code) {
			case 0: dst[(size++)^1] = cart.rom[(src++)^1]; break;
			case 1: dst[(size++)^1] = rle; break;
			case 2: dst[(size++)^1] = dst[(lz++)^1]; break;
			case 3: dst[(size++)^1] = 0; break;
			}
		}
	}



	paprium_s.decoder_size = size;
}


static void paprium_decoder_lzo(uint src, uint8 *dst)
{
	int size = 0;
	int len, lz = 0, raw = 0;
	int state = 0;




	while(1) {
		int code = cart.rom[(src++)^1];

		if( code & 0x80 ) goto code_80;
		if( code & 0x40 ) goto code_40;
		if( code & 0x20 ) goto code_20;
		if( code & 0x10 ) goto code_10;


		if( state == 0 ) {
			raw = code & 0x0F;

			if( raw == 0 ) {
				int extra = 0;

				while(1) {
					raw = cart.rom[(src++)^1];
					if( raw ) break;

					extra += 255;
				}

				raw += extra;
				raw += 15;
			}
			raw += 3;

			len = 0;
			state = 4;
			goto copy_loop;
		}

		else if( state < 4 ) {
			raw = code & 0x03;
			lz = (code >> 2) & 0x03;

			lz += cart.rom[(src++)^1] << 2;
			lz += 1;

			len = 2;
			goto copy_loop;
		}

		else {
			raw = code & 0x03;
			lz = (code >> 2) & 0x03;

			lz += cart.rom[(src++)^1] << 2;
			lz += 2049;

			len = 3;
			goto copy_loop;
		}


code_10:
		len = code & 0x07;

		if( len == 0 ) {
			int extra = 0;

			while(1) {
				len = cart.rom[(src++)^1];
				if( len ) break;

				extra += 255;
			}

			len += extra;
			len += 7;
		}
		len += 2;


		lz = ((code >> 3) & 1) << 14;

		code = cart.rom[(src++)^1];
		raw = code & 0x03;
		lz += code >> 2;

		lz += cart.rom[(src++)^1] << 6;
		lz += 16384;

		if( lz == 16384 ) break;
		goto copy_loop;


code_20:
		len = code & 0x1F;

		if( len == 0 ) {
			int extra = 0;

			while(1) {
				len = cart.rom[(src++)^1];
				if( len ) break;

				extra += 255;
			}

			len += extra;
			len += 31;
		}
		len += 2;


		code = cart.rom[(src++)^1];
		raw = code & 0x03;

		lz = code >> 2;
		lz += cart.rom[(src++)^1] << 6;
		lz += 1;

		goto copy_loop;


code_40:
		raw = code & 0x03;
		len = ((code >> 5) & 1) + 3;
		lz = (code >> 2) & 0x07;

		lz += cart.rom[(src++)^1] << 3;
		lz += 1;

		goto copy_loop;


code_80:
		raw = code & 0x03;
		len = ((code >> 5) & 0x03) + 5;
		lz = (code >> 2) & 0x07;

		lz += cart.rom[(src++)^1] << 3;
		lz += 1;


copy_loop:
		if( len > 0 )
			state = raw;
		else
			state = 4;

		
		lz = size - lz;

		while(1) {
			if( len > 0 ) {
				dst[(size++)^1] = dst[(lz++)^1];
				len--;
			}

			else if( raw > 0 ) {
				dst[(size++)^1] = cart.rom[(src++)^1];
				raw--;
			}

			else
				break;
		}		
	}




	paprium_s.decoder_size = size;
}


static void paprium_decoder_type(int src, uint8 *dst)
{
	int type;

	if( src < 0 || src >= (int)cart.romsize - 0x10 )
		return;

	type = cart.rom[(src++)^1];

	if( type == 0x80 )
		paprium_decoder_lz_rle(src, dst);

	else if( type == 0x81 )
		paprium_decoder_lzo(src, dst);

	else {
	}
}


static void paprium_decoder(int mode)
{


	int offset = *(uint16 *)(paprium_s.ram + 0x1E10);
	int ptr = (*(uint16 *)(paprium_s.ram + 0x1E12) << 16) + *(uint16 *)(paprium_s.ram + 0x1E14);




	offset &= 0xFFFF;
	paprium_decoder_type(ptr, paprium_s.decoder_ram + offset);


	paprium_s.decoder_mode = mode;
	paprium_s.decoder_ptr = offset;


}


static void paprium_decoder_copy(int arg)
{
	/* *(uint16 *)(paprium_s.ram + 0x1E10); */
	int offset = *(uint16 *)(paprium_s.ram + 0x1E12);
	int size = *(uint16 *)(paprium_s.ram + 0x1E14);






	paprium_s.decoder_ptr = offset;
	paprium_s.decoder_size = size;


}


static void paprium_sprite(int index)
{
	int lcv, spr_x, spr_y, count;
	int spritePtr;

	int dmaSize = 0;
	int dmaPtr = *(uint16*) (paprium_s.ram + 0x1F16);

	int spriteCount = *(uint16*) (paprium_s.ram + 0x1F18);

	int anim = *(uint16*) (paprium_s.ram + 0xF80 + index*16);
	int nextAnim = *(uint16*) (paprium_s.ram + 0xF82 + index*16);
	int obj = *(uint16*) (paprium_s.ram + 0xF84 + index*16) & 0x7FFF;
	//*(uint16*) (paprium_s.ram + 0xF86 + index*16);
	int objAttr = *(uint16*) (paprium_s.ram + 0xF88 + index*16);
	int reset = *(uint16*) (paprium_s.ram + 0xF8A + index*16);
	int pos_x = *(uint16*) (paprium_s.ram + 0xF8C + index*16);
	int pos_y = *(uint16*) (paprium_s.ram + 0xF8E + index*16);

	int src = paprium_s.draw_src;
	int vram = paprium_s.draw_dst;
	int flip_h = objAttr & 0x800;
	int animFlags;


	int animPtr = *(uint32*) (paprium_obj_ram + (obj+1)*4);
	int framePtr = paprium_s.obj[index];


	if( index != 0x30 ) {
		//return;
	}






reload:
	if( reset == 1 ) {
		framePtr = *(uint32*) (paprium_obj_ram + animPtr + anim*4);

		*(uint16*) (paprium_s.ram + 0xF8A + index*16) = 0;
	}
	if( (framePtr == 0) || (framePtr == -1) ) {
		*(uint16*) (paprium_s.ram + 0xF80 + index*16) = 0;
		return;
	}


	spritePtr  = paprium_obj_ram[framePtr + 0] << 0;
	spritePtr += paprium_obj_ram[framePtr + 1] << 8;
	spritePtr += paprium_obj_ram[framePtr + 2] << 16;
	animFlags = paprium_obj_ram[framePtr + 3];




	if( spritePtr == 0 ) return;


	framePtr += 4; 

	if( animFlags < 0x80 ) {
		if( nextAnim == 0xFFFF ) {
			int nextFrame;

			nextFrame  = paprium_obj_ram[framePtr + 0] << 0;
			nextFrame += paprium_obj_ram[framePtr + 1] << 8;
			nextFrame += paprium_obj_ram[framePtr + 2] << 16;

			framePtr = nextFrame;
		}
		else if( reset == 0 ) {
			anim = nextAnim;
			reset = 1;

			goto reload;
		}
	}

	paprium_s.obj[index] = framePtr;


	count = paprium_obj_ram[(spritePtr++)^1];
	spritePtr++;                                    /* (a flags byte nothing reads) */




	spr_x = pos_x;
	spr_y = pos_y;

	for( lcv = 0; lcv < count; lcv++ ) {
		int size_x = ((paprium_obj_ram[spritePtr + 3] >> 2) & 0x03) + 1;
		int size_y = ((paprium_obj_ram[spritePtr + 3] >> 0) & 0x03) + 1;
		int tile = *(uint16*) (paprium_obj_ram + spritePtr + 4);
		int tileAttr = *(uint16*) (paprium_obj_ram + spritePtr + 6) & ~0x1FF;
		int ofs = *(uint16*) (paprium_obj_ram + spritePtr + 6) & 0x1FF;

		int tilePtr = paprium_tile_ptr + tile*4;
		int tileSize = size_x * size_y * 0x20;

		int sprAttr = 0;


		if( !flip_h )
			spr_x += (int8) paprium_obj_ram[spritePtr + 1];
		else
			spr_x -= (int8) paprium_obj_ram[spritePtr + 1];

		spr_y += (int8) paprium_obj_ram[spritePtr + 0];
		spritePtr += 8;


		if( tile == 0 ) continue;
		if( spriteCount >= 94 ) continue;


		sprAttr  = (tileAttr & 0x8000) ? 0x8000 : (objAttr & 0x8000);
		sprAttr += (tileAttr & 0x6000) ? (tileAttr & 0x6000) : (objAttr & 0x6000);
		sprAttr += (tileAttr & 0x1800) ^ (objAttr & 0x1800);


		if( (spr_y >= 128) && (spr_y + size_y*8 < 368) ) {
			if( (!flip_h && ((spr_x + size_x*8 >= 128) && spr_x < 448)) ||
				 (flip_h && ((spr_x - size_x*8 < 448) && spr_x >= 128)) ) {
				if( spriteCount < 80 ) {
					*(uint16*) (paprium_s.ram + 0xB00 + spriteCount*8) = spr_y & 0x3FF;
					*(uint16*) (paprium_s.ram + 0xB02 + spriteCount*8) = ((size_x-1) << 10) + ((size_y-1) << 8) + (spriteCount+1);
					*(uint16*) (paprium_s.ram + 0xB04 + spriteCount*8) = sprAttr + ((vram / 0x20) & 0x7FF);
					*(uint16*) (paprium_s.ram + 0xB06 + spriteCount*8) = (spr_x - ((!flip_h) ? 0 : size_x*8)) & 0x1FF;

				}
				else {
					if( spriteCount == 80 ) {  /* sprite 0 lock */
						paprium_s.ram[0xD7A] = 1;  /* exp.s list */

						memcpy(paprium_s.exps_ram, paprium_s.ram + 0xB00, 8);
						paprium_s.exps_ram[2] = 14;  /* normal list */

						spriteCount++;
					}

					*(uint16*) (paprium_s.exps_ram + 0 + (spriteCount-80)*8) = spr_y & 0x3FF;
					*(uint16*) (paprium_s.exps_ram + 2 + (spriteCount-80)*8) = ((size_x-1) << 10) + ((size_y-1) << 8) + ((spriteCount-80)+1);
					*(uint16*) (paprium_s.exps_ram + 4 + (spriteCount-80)*8) = sprAttr + ((vram / 0x20) & 0x7FF);
					*(uint16*) (paprium_s.exps_ram + 6 + (spriteCount-80)*8) = (spr_x - ((!flip_h) ? 0 : size_x*8)) & 0x1FF;

				}

				spriteCount++;

			}
		}



		int ptr = paprium_tile_ptr + (*(uint16*)(cart.rom + tilePtr) << 16) + *(uint16*)(cart.rom + tilePtr + 2);
		static uint8 tile_ram[0x10000];

		paprium_decoder_type(ptr, tile_ram);
		memcpy(paprium_s.ram + src, tile_ram + ofs * 0x20, tileSize);


		//if( !tiledupe && dmaPtr < dmalimit && vram < vramlimit && tile < tilelimit)

		*(uint16*) (paprium_s.ram + 0x1400 + dmaPtr*16) = 0x8F02;
		*(uint16*) (paprium_s.ram + 0x1402 + dmaPtr*16) = 0x9300 + ((tileSize >> 1) & 0xFF);
		*(uint16*) (paprium_s.ram + 0x1404 + dmaPtr*16) = 0x9500 + ((src >> 1) & 0xFF);
		*(uint16*) (paprium_s.ram + 0x1406 + dmaPtr*16) = 0x9400 + ((tileSize >> 9) & 0xFF);
		*(uint16*) (paprium_s.ram + 0x1408 + dmaPtr*16) = 0x9700;
		*(uint16*) (paprium_s.ram + 0x140A + dmaPtr*16) = 0x9600 + ((src >> 9) & 0xFF);
		*(uint16*) (paprium_s.ram + 0x140C + dmaPtr*16) = 0x4000 + (vram & 0x3FFF);
		*(uint16*) (paprium_s.ram + 0x140E + dmaPtr*16) = 0x0080 + (vram >> 14);

		*(uint16*) (paprium_s.ram + 0x1F16) = ++dmaPtr;
		*(uint16*) (paprium_s.ram + 0x1F18) = spriteCount;


		src += tileSize;
		dmaSize += tileSize;
		vram += tileSize;
	}


	paprium_s.draw_src = src;
	paprium_s.draw_dst = vram;
}


static void paprium_sprite_init(int arg)
{

	memset(paprium_s.ram + 0x1F20, 0, 14*8);


}


static void paprium_sprite_start(int arg)
{

	int count = *(uint16*)(paprium_s.ram + 0x1F18);

	*(uint16*) (paprium_s.ram + 0x1F16) = 0x0001;  /* dma count */

	*(uint16*) (paprium_s.ram + 0x1400) = 0x8f02;
	*(uint16*) (paprium_s.ram + 0x1402) = 0x9340;
	*(uint16*) (paprium_s.ram + 0x1404) = 0x9580;
	*(uint16*) (paprium_s.ram + 0x1406) = 0x9401;
	*(uint16*) (paprium_s.ram + 0x1408) = 0x9700;
	*(uint16*) (paprium_s.ram + 0x140a) = 0x9605;
	*(uint16*) (paprium_s.ram + 0x140c) = 0x7000;
	*(uint16*) (paprium_s.ram + 0x140e) = 0x0083;


	if( count < 80 )
		memcpy(paprium_s.ram + 0x1F20, paprium_s.ram + 0xB00, 14*8);
	else
		memcpy(paprium_s.ram + 0x1F20, paprium_s.exps_ram, 14*8);


	paprium_s.draw_src = 0x2000;
	paprium_s.draw_dst = 0x200;


}


static void paprium_sprite_stop(int arg)
{


	int count = *(uint16*)(paprium_s.ram + 0x1F18);

	if( count == 0 ) {
		memset(paprium_s.ram + 0xB00, 0, 8);
		memset(paprium_s.exps_ram, 0, 8);
	}
	else if( count <= 80 ) {
		paprium_s.ram[0xB02 + (count-1)*8] = 0;

		if( count <= 14 )
			paprium_s.exps_ram[2 + (count-1)*8] = 0;
	}
	else
		paprium_s.exps_ram[2 + (count-81)*8] = 0;


	if( arg == 2 )
		*(uint16*) (paprium_s.ram + 0x1F1C) = 1;  /* exp.s - force draw */


}


static void paprium_sprite_pause(int arg)
{

	int count = *(uint16*)(paprium_s.ram + 0x1F18);

	if( count == 0 )
		memset(paprium_s.ram + 0xB00, 0, 8);


}


static void paprium_scaler_init(int arg)
{
	int row, col, out;
	static uint8 temp[0x800];



	int ptr = (*(uint16 *)(paprium_s.ram + 0x1E10) << 16) + *(uint16 *)(paprium_s.ram + 0x1E12);
	paprium_decoder_type(ptr, temp);


	out = 0;

	for( col = 0; col < 64; col++ ) {
		for( row = 0; row < 64; row++ ) {
			if( col & 1 )
				paprium_s.scaler_ram[out] = temp[(row*32 + (col/2))^1] & 0x0F;
			else
				paprium_s.scaler_ram[out] = temp[(row*32 + (col/2))^1] >> 4;

			out++;
		}
	}


}


static void paprium_scaler(int arg)
{
	int row, col;

	int left = *(uint16 *)(paprium_s.ram + 0x1E10);
	int right = *(uint16 *)(paprium_s.ram + 0x1E12);
	int scale = *(uint16 *)(paprium_s.ram + 0x1E14);
	int ptr = *(uint16 *)(paprium_s.ram + 0x1E16);
	int step;
	int ptr_frac = 0;

	if( scale <= 0 )
		return;
	step = 64 * 0x10000 / scale;




	for( col = 0; col < 128; col++ ) {
		int base = (col & 4) ? 0x600 : 0x200;
		int out = ((col / 8) * 64) + ((col & 2) / 2);

		for( row = 0; row < 64; row += 2 ) {
			if( (col >= left) && (col < right) ) {
				if(col & 1)
					paprium_s.ram[(base + out)^1] += paprium_s.scaler_ram[(row*64 + ptr) & 0xFFF] & 0x0F;
				else
					paprium_s.ram[(base + out)^1] = paprium_s.scaler_ram[(row*64 + ptr) & 0xFFF] << 4;
			}
			else if( (col & 1) == 0 )
				paprium_s.ram[(base + out)^1] = 0;

			out += 2;
		}

		if( (col >= left) && (col < right) && (ptr < 64) ) {
			ptr_frac += 0x10000;

			while( ptr_frac >= step ) {
				ptr++;
				ptr_frac -= step;
			}
		}
	}


}


static void paprium_sram_read(int bank)
{

	int offset = *(uint16 *)(paprium_s.ram + 0x1E10);

	if( (bank >= 1) && (bank <= 4) )
		memcpy(paprium_s.ram + offset, sram.sram + ((bank-1) * 0x780), 0x780);


}


static void paprium_sram_write(int bank)
{


	/* *(uint16 *)(paprium_s.ram + 0x1E10) */
	int offset = *(uint16 *)(paprium_s.ram + 0x1E12);

	if( (bank >= 1) && (bank <= 4) )
		memcpy(sram.sram + ((bank-1) * 0x780), paprium_s.ram + offset, 0x780);


}


static void paprium_mapper(int arg)
{


	memcpy(paprium_s.ram + 0x8000, cart.rom + 0x8000, 0x8000);  /* troll */


}


static void paprium_boot(int arg)
{

	paprium_music_ptr = (*(uint16*)(paprium_s.ram + 0x1E10) << 16) + *(uint16*)(paprium_s.ram + 0x1E12);
	/* (*(uint16*)(paprium_s.ram + 0x1E14) << 16) + *(uint16*)(paprium_s.ram + 0x1E16); */
	/* (*(uint16*)(paprium_s.ram + 0x1E1C) << 16) + *(uint16*)(paprium_s.ram + 0x1E1E); */
	paprium_sfx_ptr = (*(uint16*)(paprium_s.ram + 0x1E20) << 16) + *(uint16*)(paprium_s.ram + 0x1E22);
	paprium_sprite_ptr = (*(uint16*)(paprium_s.ram + 0x1E24) << 16) + *(uint16*)(paprium_s.ram + 0x1E26);
	paprium_tile_ptr = (*(uint16*)(paprium_s.ram + 0x1E28) << 16) + *(uint16*)(paprium_s.ram + 0x1E2A);

	paprium_decoder_type(paprium_sprite_ptr, paprium_obj_ram);

	paprium_s.decoder_size = 0;


}


static void paprium_music(int track)
{

	track &= 0x7F;


	int ptr = (*(uint16*)(cart.rom + paprium_music_ptr + track*4)<<16) + (*(uint16*)(cart.rom + paprium_music_ptr + track*4 + 2));
	paprium_decoder_type(paprium_music_ptr + ptr, paprium_s.music_ram);
	paprium_s.decoder_size = 0;


	paprium_s.music_section = 0;
	paprium_s.music_segment = 0;
	paprium_s.audio_tick = 0;


	memset(paprium_s.music, 0, sizeof(paprium_s.music));
	for( int ch = 0; ch < 26; ch++ ) {
		paprium_voice_t *voice = paprium_s.music + ch;

		voice->panning = 0x80;
		voice->volume = 0x80;
		voice->program = paprium_s.music_ram[(0x2A + ch)^1];
	}




	paprium_load_mp3(track, 1);
}


static void paprium_music_volume(int level)
{

	paprium_s.music_volume = level;
	/* *(uint16 *)(paprium_s.ram + 0x1E10); */
	/* *(uint16 *)(paprium_s.ram + 0x1E12); */




}


static void paprium_music_setting(int flag)
{

	if( flag == 8 )
		paprium_s.music_segment = -1;

	else if( flag == 0 )
		paprium_s.music_segment = -1;




}


static void paprium_music_special(int flag)
{

	if( flag == 1 ) {
		//*(uint16 *)(paprium_s.ram + 0x1E10)
		//*(uint16 *)(paprium_s.ram + 0x1E12)

	}

	else if( flag == 2 ) {
		//*(uint16 *)(paprium_s.ram + 0x1E10)  /* 4 = crisis, 0 = normal ? */

	}

	else if( flag == 4 ) {
		//*(uint16 *)(paprium_s.ram + 0x1E10)  /* 81 = blu pill */

	}

	else if( flag == 6 ) {  /* sax man */
		//*(uint16 *)(paprium_s.ram + 0x1E10)
		//*(uint16 *)(paprium_s.ram + 0x1E12)
		//*(uint16 *)(paprium_s.ram + 0x1E14)

	}

	else if( flag == 7 ) {  /* blu pill - stage title */
		//*(uint16 *)(paprium_s.ram + 0x1E10)

	}

}


static void paprium_audio_setting(int flags)
{

	paprium_s.audio_flags = flags;

	paprium_s.ram[0x1801] = flags & 0x01;  /* dac */

	paprium_s.ram[0x1800]  = (flags & 0x01) ? 0x80 : 0x00;  /* dac */
	paprium_s.ram[0x1800] += (flags & 0x02) ? 0x40 : 0x00;  /* ntsc */


}


static void paprium_sfx_volume(int level)
{

	paprium_s.sfx_volume = level;
	/* *(uint16 *)(paprium_s.ram + 0x1E10); */
	/* *(uint16 *)(paprium_s.ram + 0x1E12); */




}


static void paprium_cmd(int data)
{
	int cmd = data >> 8;
	int lcv;

	data &= 0xFF;

	switch(cmd) {
	case 0x84: paprium_mapper(data); break;
	case 0x88: paprium_audio_setting(data); break;
	case 0x8C: paprium_music(data); break;
	case 0x8D: paprium_music_setting(data); break;
	case 0xAD: paprium_sprite(data); break;
	case 0xAE: paprium_sprite_start(data); break;
	case 0xAF: paprium_sprite_stop(data); break;
	case 0xB0: paprium_sprite_init(data); break;
	case 0xB1: paprium_sprite_pause(data); break;
	case 0xC6: paprium_boot(data); break;
	case 0xC9: paprium_music_volume(data); break;
	case 0xCA: paprium_sfx_volume(data); break;
	case 0xD1: goto sfx_play;
	case 0xD2: goto sfx_off;
	case 0xD3: goto sfx_loop;
	case 0xD6: paprium_music_special(data); break;
	case 0xDA: paprium_decoder(data); break;
	case 0xDB: paprium_decoder_copy(data); break;
	case 0xDF: paprium_sram_read(data); break;
	case 0xE0: paprium_sram_write(data); break;
	//case 0xF2: paprium_block_viewer(data); break;
	case 0xF4: paprium_scaler_init(data); break;
	case 0xF5: paprium_scaler(data); break;

	default: break;   /* the other commands need nothing here */
	}


done:
	*(uint16*)(paprium_s.ram + 0x1FEA) &= 0x7FFF;
	return;



sfx_play:
	{
		int chan, vol, pan, flags;
		int ptr, size, type;
		int lcv;
		int newch = 0, maxtime = 0;
		uint8 *sfx = paprium_sfx_ptr + cart.rom;



		chan = *(uint16 *)(paprium_s.ram + 0x1E10);
		vol = *(uint16 *)(paprium_s.ram + 0x1E12);
		pan = *(uint16 *)(paprium_s.ram + 0x1E14);
		flags = *(uint16 *)(paprium_s.ram + 0x1E16);




		ptr = (*(uint16 *)(sfx + data*8) << 16) | (*(uint16 *)(sfx + data*8 + 2));
		size = (*(uint8 *)(sfx + data*8 + 4) << 16) | (*(uint16 *)(sfx + data*8 + 6));
		type = *(uint8 *)(sfx + data*8 + 5);




		for( lcv = 0; lcv < 8; lcv++, chan >>= 1 ) {
			if( (chan & 1) == 0 ) continue;

			if( paprium_s.sfx[lcv].size ) {
				if( maxtime < paprium_s.sfx[lcv].time ) {
					maxtime = paprium_s.sfx[lcv].time;
					newch = lcv;
				}
				continue;
			}

			newch = lcv;
			break;
		}


		paprium_s.sfx[newch].volume = vol;
		paprium_s.sfx[newch].panning = pan;
		paprium_s.sfx[newch].type = type;

		paprium_s.sfx[newch].num = data;
		paprium_s.sfx[newch].flags = flags;

		paprium_s.sfx[newch].loop = 0;
		paprium_s.sfx[newch].count = 0;
		paprium_s.sfx[newch].time = 0;
		paprium_s.sfx[newch].tick = 0;
		paprium_s.sfx[newch].decay = 0;

		if( flags & 0x4000 )
			paprium_s.sfx[newch].echo = (paprium_s.echo_pan++) & 1;

		paprium_s.sfx[newch].ptr = ptr;
		paprium_s.sfx[newch].start = ptr;
		paprium_s.sfx[newch].size = size;
	}
	goto done;


sfx_loop:
	{
		int lcv;




		for( lcv = 0; lcv < 8; lcv++, data >>= 1 ) {
			if( (data & 1) == 0 ) continue;

			paprium_s.sfx[lcv].volume = *(uint16 *)(paprium_s.ram + 0x1E10);
			paprium_s.sfx[lcv].panning = *(uint16 *)(paprium_s.ram + 0x1E12);
			paprium_s.sfx[lcv].decay = *(uint16 *)(paprium_s.ram + 0x1E14);

			paprium_s.sfx[lcv].loop = 1;

			break;
		}
	}
	goto done;


sfx_off:
	{
		int flags;




		flags = *(uint16 *)(paprium_s.ram + 0x1E10);


		if(0) {}
		//else if( flags == 6 ) {}
		else if( flags == 0 ) {}
		else {} //MessageBoxA(0, "Sfx_Off flags", "Warn", MB_OK );


		for( lcv = 0; lcv < 8; lcv++ ) {
			if( !(data & (1 << lcv) ) ) continue;

			if( flags == 0 ) {
				paprium_s.sfx[lcv].size = 0;
			}

			paprium_s.sfx[lcv].decay = flags;
			paprium_s.sfx[lcv].loop = 0;

			break;
		}
	}
	goto done;
}


/* The random numbers the game reads back (its own, so the program's rand() is not touched). */
static uint32 paprium_rand_state = 1;

static int paprium_rand(void)
{
	paprium_rand_state = paprium_rand_state * 1103515245u + 12345u;
	return (int)((paprium_rand_state >> 16) & 0x7FFF);
}


static uint32 paprium_r8(uint32 address)
{
	int data = paprium_s.ram[address^1];




	if( address == 0x1800 )
		data = 0;

	else if( address >= 0x1880 && address < 0x1b00 ) {
		data = paprium_rand() % 256;
		//paprium_s.ram[address^1] = data;  /* dma blowout if no limit */
	}


	return data;
}


static uint32 paprium_r16(uint32 address)
{
	int data = 0;


	if( (address == 0xC000) && (paprium_s.decoder_size > 0) ) {

		int max = 0, size;

		if( paprium_s.decoder_mode == 2 )
			max = 0x4000;

		else if( paprium_s.decoder_mode == 7 )
			max = 0x800;


		size = (paprium_s.decoder_size > max) ? max : paprium_s.decoder_size;

		memcpy(paprium_s.ram + 0xC000, paprium_s.decoder_ram + paprium_s.decoder_ptr, size);

		paprium_s.decoder_ptr += size;
		paprium_s.decoder_size -= size;
	}


	switch( address ) {
	case 0x1FE4:
		data = 0xFFFF;
		data &= ~(1<<2);
		data &= ~(1<<6);
		break;

	case 0x1FE6:
		data = 0xFFFF;
		data &= ~(1<<14);
		data &= ~(1<<8);  /* sram */
		data &= ~(1<<9);  /* sram */
		break;
	
	case 0x1FEA:
		data = 0xFFFF;
		data &= ~(1<<15);
		break;

	default:
		data = *(uint16 *)(paprium_s.ram + address);
		break;
	}



	return data;
}


static void paprium_w8(uint32 address, uint32 data)
{

	paprium_s.ram[address^1] = data;
}


static void paprium_w16(uint32 address, uint32 data)
{

	*(uint16 *)(paprium_s.ram + address) = data;

	if( address == 0x1FEA ) {
		paprium_cmd(data);

	}
}


static uint32 paprium_io_r8(uint32 address)
{
	if( address == 0xA14101 )
		return paprium_tmss;

	return ctrl_io_read_byte(address);
}


static void paprium_io_w8(uint32 address, uint32 data)
{
	if( address >= 0xA130F3 && address <= 0xA130FF )  /* no bank mapper */
		return;

	if( address == 0xA14101 ) {
		paprium_tmss = data;
		return;
	}

	ctrl_io_write_byte(address, data);
}


static void paprium_map()
{
    m68k.memory_map[0x00].base    = paprium_s.ram;
    m68k.memory_map[0x00].read8   = paprium_r8;
    m68k.memory_map[0x00].read16  = paprium_r16;
    m68k.memory_map[0x00].write8  = paprium_w8;
    m68k.memory_map[0x00].write16 = paprium_w16;

	m68k.memory_map[0xA1].read8  = paprium_io_r8;
	m68k.memory_map[0xA1].write8 = paprium_io_w8;

	zbank_memory_map[0x00].read  = paprium_r8;
	zbank_memory_map[0x00].write = paprium_w8;
}


static void paprium_init()
{
	int ptr;

	paprium_map();


	memset(&paprium_s, 0, sizeof(paprium_s));
	memset(paprium_echo_l, 0, sizeof(paprium_echo_l));
	memset(paprium_echo_r, 0, sizeof(paprium_echo_r));
	memcpy(paprium_s.ram, cart.rom, 0x10000);


	ptr = (*(uint16*)(cart.rom + 0xaf77c) << 16) + *(uint16*)(cart.rom + 0xaf77e);

	paprium_music_ptr = (*(uint16*)(cart.rom + 0x10054)<< 16) + *(uint16*)(cart.rom + 0x10056);
	// (*(uint16*)(cart.rom + 0x10058)<< 16) + *(uint16*)(cart.rom + 0x1005a);
	paprium_sfx_ptr = (*(uint16*)(cart.rom + ptr + 0x778)<< 16) + *(uint16*)(cart.rom + ptr + 0x77a);
	paprium_sprite_ptr = (*(uint16*)(cart.rom + 0x10014)<< 16) + *(uint16*)(cart.rom + 0x10016);
	paprium_tile_ptr = (*(uint16*)(cart.rom + ptr + 0x780)<< 16) + *(uint16*)(cart.rom + ptr + 0x782);

	paprium_decoder_type(paprium_sprite_ptr, paprium_obj_ram);
	paprium_s.decoder_size = 0;


	*(uint16*)(paprium_s.ram + 0x192) = 0x3634;  /* 6-button, multitap */


	paprium_s.ram[0x1D1D^1] = 0x04;  /* rom ok - dynamic patch */
	paprium_s.ram[0x1D2C^1] = 0x67;


	*(uint16*) (paprium_s.ram + 0x1560) = 0x4EF9;
	*(uint16*) (paprium_s.ram + 0x1562) = 0x0001;
	*(uint16*) (paprium_s.ram + 0x1564) = 0x0100;






	*(uint16*)(cart.rom + 0xb9094) = 0x2079;
	*(uint16*)(cart.rom + 0xb9096) = 0x000a;
	*(uint16*)(cart.rom + 0xb9098) = 0xf85c;

	*(uint16*)(cart.rom + 0xb909a) = 0x20bc;
	*(uint16*)(cart.rom + 0xb909c) = 0x0000;
	*(uint16*)(cart.rom + 0xb909e) = 0x0003;

	*(uint16*)(cart.rom + 0xb90a0) = 0x20bc;
	*(uint16*)(cart.rom + 0xb90a2) = 0x0000;
	*(uint16*)(cart.rom + 0xb90a4) = 0x0003;

	*(uint16*)(cart.rom + 0xb90a6) = 0x20bc;
	*(uint16*)(cart.rom + 0xb90a8) = 0x0000;
	*(uint16*)(cart.rom + 0xb90aa) = 0x0003;

	*(uint16*)(cart.rom + 0xb90ac) = 0x20bc;
	*(uint16*)(cart.rom + 0xb90ae) = 0x0000;
	*(uint16*)(cart.rom + 0xb90b0) = 0x0003;

	*(uint16*)(cart.rom + 0xb90b2) = 0x20bc;
	*(uint16*)(cart.rom + 0xb90b4) = 0x0000;
	*(uint16*)(cart.rom + 0xb90b6) = 0x0003;


	paprium_s.echo_pan = paprium_rand();


	paprium_s.music_segment = -1;
}


static void paprium_reset()
{
	paprium_init();

	mp3dec_init(&paprium_mp3d);

	mp3dec_init(&paprium_mp3d_boss1);
	mp3dec_init(&paprium_mp3d_boss2);
	mp3dec_init(&paprium_mp3d_boss3);
	mp3dec_init(&paprium_mp3d_boss4);
	paprium_load_mp3_boss();
}


int paprium_in_use(void)
{
	return paprium_active;
}

/* The game is closed or another is loaded: the music, decoded in memory, is given back. */
void paprium_unload(void)
{
	paprium_mp3_free_all();
	paprium_archive_close();
	paprium_active = 0;
	paprium_rom_dir[0] = '\0';
	paprium_archive[0] = '\0';
}


/****************************************************************************
 * The hooks
 *
 * The linker sends these calls of the core to the __wrap_ functions below (see
 * --wrap in the Makefile); each does what the core does, and then what Paprium
 * needs. For any other game they pass straight through.
 ****************************************************************************/

extern int  __real_load_rom(char *filename);
extern void __real_md_cart_init(void);
extern void __real_md_cart_reset(int hard_reset);
extern int  __real_md_cart_context_save(uint8 *state);
extern int  __real_md_cart_context_load(uint8 *state);
extern int  __real_audio_init(int samplerate, double framerate);
extern void __real_yx5200_update(unsigned int samples);
extern void __real_vdp_68k_ctrl_w(unsigned int data);
extern uint32 dma_endCycles;

static char paprium_source[GUI_PATH_LEN];    /* where the game really is, when it is loaded from a temporary copy (a game in a .7z archive, a patched game) */

void paprium_source_set(const char *path)
{
	lstrcpynA(paprium_source, path ? path : "", sizeof(paprium_source));
}


/* A game is loaded: is it Paprium (its product code), and where is it. */
int __wrap_load_rom(char *filename)
{
	int size = __real_load_rom(filename);
	char *slash;

	paprium_unload();
	if( size <= 0 || (system_hw & SYSTEM_PBC) != SYSTEM_MD || cart.romsize < 0x100000 || !strstr(rominfo.product, "T-574120-00") )
		return size;

	paprium_active = 1;
	gui_log("Paprium: recognized (%u KB of ROM)", (unsigned)(cart.romsize >> 10));
	lstrcpynA(paprium_rom_dir, paprium_source[0] ? paprium_source : filename, sizeof(paprium_rom_dir));
	{
		const char *dot = strrchr(paprium_rom_dir, '.');

		paprium_archive[0] = '\0';
		if( dot && (!lstrcmpiA(dot, ".zip") || !lstrcmpiA(dot, ".7z")) )
			lstrcpynA(paprium_archive, paprium_rom_dir, sizeof(paprium_archive));
	}
	slash = strrchr(paprium_rom_dir, '\\');
	if( !slash ) slash = strrchr(paprium_rom_dir, '/');
	if( slash ) *slash = '\0';
	else paprium_rom_dir[0] = '\0';

	return size;
}

/* The cartridge starts up: its memory, and the backup RAM it saves in. */
void __wrap_md_cart_init(void)
{
	__real_md_cart_init();

	if( paprium_active ) {
		paprium_init();
		eeprom_spi_init();
	}
}

/* The console is reset. */
void __wrap_md_cart_reset(int hard_reset)
{
	__real_md_cart_reset(hard_reset);

	if( paprium_active )
		paprium_reset();
}

/* Its state goes after the cartridge's own in a saved state. */
int __wrap_md_cart_context_save(uint8 *state)
{
	int size = __real_md_cart_context_save(state);

	if( paprium_active ) {
		memcpy(state + size, &paprium_s, sizeof(paprium_s));
		size += (int)sizeof(paprium_s);
	}

	return size;
}

int __wrap_md_cart_context_load(uint8 *state)
{
	int size = __real_md_cart_context_load(state);

	if( paprium_active ) {
		memcpy(&paprium_s, state + size, sizeof(paprium_s));
		memset(paprium_echo_l, 0, sizeof(paprium_echo_l));
		memset(paprium_echo_r, 0, sizeof(paprium_echo_r));
		size += (int)sizeof(paprium_s);
		paprium_map();
	}

	return size;
}

/* Its sound is the cartridge's stream in the mix (the stream of the Kaiser Wave board's YX5200 chip is the one the core mixes). */
int __wrap_audio_init(int samplerate, double framerate)
{
	int result = __real_audio_init(samplerate, framerate);

	/* The MegaSD overlay (MD+ games) has the Mega CD's sound streams (PCM and CD audio) on a Mega Drive cartridge.
	   md_cart_init() makes their buffers, but this call starts the audio over and only makes them for a real Mega CD: with the overlay on they
	   are made again here, or the first thing that clears them (the reset) stops the program. */
	if( result >= 0 && (cart.special & HW_MEGASD) && !snd.blips[1] && !snd.blips[2] ) {
		snd.blips[1] = blip_new(samplerate / 10);
		snd.blips[2] = blip_new(samplerate / 10);
		if( snd.blips[1] && snd.blips[2] ) {
			blip_clear(snd.blips[1]);
			blip_clear(snd.blips[2]);
			audio_set_rate(samplerate, framerate);
		}
		else {
			if( snd.blips[1] ) blip_delete(snd.blips[1]);
			if( snd.blips[2] ) blip_delete(snd.blips[2]);
			snd.blips[1] = snd.blips[2] = NULL;
		}
	}

	if( result >= 0 && paprium_active ) {
		snd.blips[3] = blip_new(samplerate / 10);
		if( snd.blips[3] ) {
			blip_clear(snd.blips[3]);
			blip_set_rates(snd.blips[3], 48000, samplerate);
		}
	}

	return result;
}

void __wrap_yx5200_update(unsigned int samples)
{
	if( paprium_active ) paprium_audio((int)samples);
	else if( cart.special & HW_YX5200 ) __real_yx5200_update(samples);
}

/* The game does not wait for the video chip's DMA: the original fork shortens it (one scanline for the 68000, whatever the size), and so the
   68000 here, held by a DMA from its own bus, is let go after a moment. The rest of the data still moves line by line, as the core does it. */
void __wrap_vdp_68k_ctrl_w(unsigned int data)
{
	unsigned int before = m68k.cycles;

	__real_vdp_68k_ctrl_w(data);

	if( paprium_active && m68k.cycles > before + MCYCLES_PER_LINE / 204 ) {
		m68k.cycles = before + MCYCLES_PER_LINE / 204;
		dma_endCycles = m68k.cycles;
	}
}
