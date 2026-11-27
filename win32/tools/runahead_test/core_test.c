/* core_test.c -- Run-Ahead check on the Genesis Plus GX core (Linux, no Windows needed).
 *
 * It does what win32/main.c does around a frame, on a small test ROM:
 *   mode 0: a normal run (frame, sound);
 *   mode 1: Run-Ahead with 1 frame (real frame, state saved, frame from ahead, state loaded, what the load clears put back).
 * Both write the sound of every frame to rec0.bin / rec1.bin; compare.py checks that the sound played with Run-Ahead (the frame from ahead) is
 * the sound a normal run plays one frame later. Options (environment): HQ=1 (hq_fm), NUKED=1, FILTER=1|2, HQPSG=0|1, PRESS=n (button A held from
 * frame n for 8 frames), START_LIST=2,4,.. (frames where Start goes down), PIX=1 (print the picture's first pixel and the ROM's delay each frame).
 * The core is not changed: raudio.c and the wrapped system_reset() are the same code as in the program (win32/raudio.c, vgm.c).
 */
#include "shared.h"
#include "config.h"
extern void set_config_defaults(void);
extern int ra_audio_keep; extern int ra_audio_usable(void); extern void ra_audio_snapshot(void), ra_audio_ahead_done(void), ra_audio_restore(void);
extern void __real_system_reset(void);
void __wrap_system_reset(void)       /* as vgm.c: a load that Run-Ahead does leaves the sound chain alone */
{
  if (ra_audio_keep) { gen_reset(1); io_reset(); render_reset(); vdp_reset(); }
  else __real_system_reset();
}
#define W 720
#define H 576
#define NF 60
static int16 sndb[4096];
static int16 rec[2][NF][2 * 2000];
static int recn[2][NF];
static uint8 *st, *pic_a;
static void run_frame(void)
{
  if (system_hw == SYSTEM_MCD) system_frame_scd(0);
  else if ((system_hw & SYSTEM_PBC) == SYSTEM_MD) system_frame_gen(0);
  else system_frame_sms(0);
}
int main(int argc, char **argv)
{
  int f, mode = argc > 1 ? atoi(argv[1]) : 0;
  const char *rom = argc > 2 ? argv[2] : "test.bin";
  size_t pb = (size_t)W * H * 2;
  set_config_defaults();
  if (getenv("HQ")) config.hq_fm = atoi(getenv("HQ"));
  if (getenv("NUKED")) { config.ym3438 = atoi(getenv("NUKED")); config.opll = atoi(getenv("NUKED")); }
  if (getenv("FILTER")) config.filter = atoi(getenv("FILTER"));
  if (getenv("HQPSG")) config.hq_psg = atoi(getenv("HQPSG"));
  memset(&bitmap, 0, sizeof(bitmap));
  bitmap.width = W; bitmap.height = H; bitmap.pitch = W * 2;
  bitmap.data = (uint8 *)calloc(W * H, 2);
  bitmap.viewport.changed = 3;
  if (!load_rom((char *)rom)) { printf("load_rom failed\n"); return 1; }
  audio_init(48000, 0);
  system_init();
  system_reset();
  st = (uint8 *)malloc(STATE_SIZE * 4); pic_a = (uint8 *)malloc(pb);
  if (mode == 1 && !ra_audio_usable()) { printf("the sound chain cannot be kept here\n"); return 1; }
  for (f = 0; f < NF; f++)
  {
    { int a = getenv("PRESS") ? atoi(getenv("PRESS")) : 1000; input.pad[0] = 0; if (f >= a && f < a + 8) input.pad[0] |= INPUT_A;
      if (getenv("START_LIST")) { char *p = getenv("START_LIST"); while (p && *p) { if (atoi(p) == f) input.pad[0] |= INPUT_START; p = strchr(p, ','); if (p) p++; } } }
    if (mode == 0) { run_frame(); recn[0][f] = audio_update(sndb); memcpy(rec[0][f], sndb, recn[0][f] * 4); }
    else
    {
      unsigned char vpr[sizeof(bitmap.viewport)];
      uint8 r_int, r_odd, r_im2; void (*rbg)(int), (*rob)(int), (*rps)(int);
      int r_refresh; cpu_idle_t r_poll;
      run_frame();
      audio_update(sndb);
      state_save(st);
      ra_audio_snapshot();
      memcpy(vpr, &bitmap.viewport, sizeof(vpr));
      r_int = interlaced; r_odd = odd_frame; r_im2 = im2_flag; rbg = render_bg; rob = render_obj; rps = parse_satb;
      r_refresh = m68k.refresh_cycles; r_poll = m68k.poll;
      run_frame();
      recn[1][f] = audio_update(sndb); memcpy(rec[1][f], sndb, recn[1][f] * 4);
      ra_audio_ahead_done();
      memcpy(pic_a, bitmap.data, pb);
      ra_audio_keep = 1;
      state_load(st);
      ra_audio_keep = 0;
      ra_audio_restore();
      memcpy(&bitmap.viewport, vpr, sizeof(vpr));
      interlaced = r_int; odd_frame = r_odd; im2_flag = r_im2; render_bg = rbg; render_obj = rob; parse_satb = rps;
      m68k.refresh_cycles = r_refresh; m68k.poll = r_poll;
      memcpy(bitmap.data, pic_a, pb);
    }
    if (getenv("PIX")) printf("%s frame %2d: pad %04X pixel(0,0) %04X  rom delay %d\n", mode ? "RA" : "normal", f, input.pad[0], *(uint16 *)bitmap.data, work_ram[0 ^ 1]);
    if (bitmap.viewport.changed & 1) { bitmap.viewport.changed &= ~1; memset(bitmap.data, 0, pb); }
  }
  { FILE *o = fopen(mode ? "rec1.bin" : "rec0.bin", "wb"); fwrite(recn, 1, sizeof(recn), o); fwrite(rec, 1, sizeof(rec), o); fclose(o); }
  return 0;
}
int debug_on = 0; int log_error = 0;
int sdl_input_update(void) { return 1; }
#include "md_ntsc.h"
#include "sms_ntsc.h"
md_ntsc_t *md_ntsc; sms_ntsc_t *sms_ntsc;
