/* XAudio2 and WASAPI sound outputs (audio_be.c); audio.c picks between these and waveOut. */
#ifndef AUDIO_BE_H
#define AUDIO_BE_H

extern int  be_open(int kind, int rate, const char *devname, int need_ms);   /* kind: 1 XAudio2, 2 WASAPI shared, 3 WASAPI exclusive (shared if refused); 1 when it opened */
extern void be_close(void);
extern int  be_active(void);                                      /* 0 none, 1 XAudio2, 2 WASAPI */
extern int  be_rate_now(void);
extern int  be_pending(void);                                     /* chunks submitted and not finished playing */
extern int  be_write(const short *samples, int frames);           /* 1 ok (also when the frame was dropped), 0 failed */
extern void be_flush(void);
extern int  be_list_endpoints(char names[][96], int max);        /* the full names of the output devices */

#endif
