/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *  Copyright (c) 2026 Hazem Abdelghani
 *
 *  disc.h -- the optical drives and the discs in them (disc.c).
 ****************************************************************************/

#ifndef DISC_H
#define DISC_H

/* Looks at every optical drive (a virtual one included) and writes what it found as text (one block of lines for each drive): whether a disc is in it,
   its tracks (data or audio, where they start), whether it is a Sega CD disc (and its name), and whether the data and the audio can be read. The same lines go
   to the log. Returns the number of optical drives. */
extern int disc_report(char *text, int size);


/* Playing a disc from the drive (the first track a data track, a Sega CD disc). */
typedef struct { char letter; char title[64]; } disc_found;
extern int disc_scan_sega(disc_found *out, int max);   /* the Sega CD discs in the optical drives */
extern int disc_open(char letter);                      /* opens the disc of a drive for playing; 1 when it can be played */
extern void disc_close(void);
extern char disc_letter(void);                        /* the drive of the disc opened last */
extern int disc_swap(void);                            /* the game asks for another disc: the one in the same drive is opened instead; 1 when it is a Sega CD disc */
extern const char *disc_path(void);                     /* what emu_load_rom() is given: disc:\<name of the game>.cue (NULL when no disc is open) */
extern int disc_is_path(const char *path);
extern int disc_open_for_path(const char *path);        /* the disc of such a path is in a drive: opens it */
extern void disc_stats_reset(void);                    /* the counts of the log start again */
extern void disc_log_stats(void);                      /* the log: how the reading of the disc went */
extern int disc_audio_ok(void);                         /* the audio tracks can be read (or there are none to read) */

/* The wrappers of the C file functions (main.c) ask these first: a disc is read as a .cue sheet and one file for each track. */
extern int disc_owns(const void *f);
extern void *disc_fopen(const char *path);
extern size_t disc_fread(void *buf, size_t size, size_t n, void *f);
extern int disc_fseek(void *f, long off, int whence);
extern long disc_ftell(void *f);
extern int disc_fclose(void *f);
extern char *disc_fgets(char *s, int n, void *f);

#endif
