/* pcm_test.c -- the Mega CD's PCM chip chain with Run-Ahead (Linux). The chip is driven directly (no disc or BIOS needed): every frame sets a new
 * volume and panning, then the same real frame / frame from ahead / state back sequence as core_test.c, with the chip's own state saved by a copy.
 * NORESTORE=1 leaves out the chain restore (to see that the test can fail). Writes rec0.bin / rec1.bin like core_test.c. */
#include "shared.h"
#include "config.h"
#include "cd_hw/scd.h"
extern void set_config_defaults(void);
extern int ra_audio_usable(void); extern void ra_audio_snapshot(void), ra_audio_ahead_done(void), ra_audio_restore(void);
#define NF 60
static int16 buf[4096]; static int16 rec[2][NF][2*2000]; static int recn[2][NF];
static void events(int k)
{
  int env = 0x40 + ((k * 37) % 0xB0);
  pcm_write(0x07, 0xC0, 0);              /* channel 0, chip on */
  pcm_write(0x00, env, 0);
  pcm_write(0x01, (k & 1) ? 0xFF : 0x3F, 0);
}
static void frame(int k) { events(k); mcycles_vdp = 896040; }
int main(int argc, char **argv)
{
  int mode = argc > 1 ? atoi(argv[1]) : 0, f, i;
  set_config_defaults();
  memset(&bitmap, 0, sizeof(bitmap)); bitmap.width = 720; bitmap.height = 576; bitmap.pitch = 1440; bitmap.data = calloc(720 * 576, 2);
  if (!load_rom("test_snd.bin")) return 1;
  system_hw = SYSTEM_MCD;
  audio_init(48000, 0);
  system_init(); system_reset();
  pcm_init(50000000.0 / 4, 48000);        /* roughly the sub-CPU clock: the exact value does not matter here */
  pcm_reset();
  pcm_write(0x02, 0x00, 0); pcm_write(0x03, 0x08, 0); pcm_write(0x04, 0, 0); pcm_write(0x05, 0, 0); pcm_write(0x06, 0, 0);
  for (i = 0; i < 255; i++) scd.pcm_hw.ram[i] = (i & 0x40) ? (0x80 | (i & 0x7F)) : (i & 0x7F);
  scd.pcm_hw.ram[255] = 0xFF;
  pcm_write(0x08, 0xFE, 0);
  if (mode == 1) printf("usable %d blips %d %d %d %d\n", ra_audio_usable(), !!snd.blips[0], !!snd.blips[1], !!snd.blips[2], !!snd.blips[3]);
  for (f = 0; f < NF; f++)
  {
    if (mode == 0) { frame(f); recn[0][f] = audio_update(buf); memcpy(rec[0][f], buf, recn[0][f] * 4); }
    else
    {
      static pcm_t pcm_state; static uint8 *bank_off;
      frame(f); audio_update(buf);
      ra_audio_snapshot();
      memcpy(&pcm_state, &scd.pcm_hw, sizeof(pcm_state)); { int bk = scd.pcm_hw.bank - scd.pcm_hw.ram; (void)bk; }
      frame(f + 1); recn[1][f] = audio_update(buf); memcpy(rec[1][f], buf, recn[1][f] * 4);
      ra_audio_ahead_done();
      { uint8 *bank = scd.pcm_hw.bank; memcpy(&scd.pcm_hw, &pcm_state, sizeof(pcm_state)); (void)bank; scd.pcm_hw.bank = scd.pcm_hw.ram + (pcm_state.bank - pcm_state.ram); }
      scd.pcm_hw.cycles = 0;
      blip_clear(snd.blips[1]);           /* what pcm_reset() in the load does */
      if (!getenv("NORESTORE")) ra_audio_restore();
    }
  }
  { FILE *o = fopen(mode ? "rec1.bin" : "rec0.bin", "wb"); fwrite(recn, 1, sizeof(recn), o); fwrite(rec, 1, sizeof(rec), o); fclose(o); }
  return 0;
}
int debug_on = 0; int log_error = 0;
int sdl_input_update(void) { return 1; }
#include "md_ntsc.h"
#include "sms_ntsc.h"
md_ntsc_t *md_ntsc; sms_ntsc_t *sms_ntsc;
