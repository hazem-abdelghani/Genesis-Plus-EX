/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  settings.c -- Options > Settings: every option of the menus, on tabs.
 *
 *  The window holds no settings of its own. Each control stands for a menu
 *  item (or a group of them): changing it sends the same command the menu
 *  item sends, and the control then shows whatever the menu now says (its
 *  check marks), so the two can never disagree and no option is handled
 *  twice. Items that open a window of their own (Levels and Latency,
 *  Controls...) are buttons that close this window and open that one.
 ****************************************************************************/

#include <windows.h>
#include <commctrl.h>
#include <stdlib.h>
#include <string.h>

#include "shared.h"
#include "gui.h"
#include "resource.h"
#include "theme.h"
#include "shader_chain.h"
#include "eggs.h"
#include "toolbar.h"     /* TBS(): sizes that follow the Large interface */

enum { K_CHECK, K_CHOICE, K_BUTTON, K_FILTER, K_SHADER, K_BREAK, K_DYN, K_GROUP, K_GROUPEND };   /* K_BREAK: the next rows start a new column; K_GROUP .. K_GROUPEND: the rows between are drawn inside a frame */

typedef struct
{
  int kind;
  const char *label;
  int id;                          /* command (check, button) or first command of the group (choice) */
  int n;                           /* choice: number of entries */
  const char *const *items;
  const char *tip;                 /* its tooltip */
} SRow;

typedef struct { const char *title; const SRow *rows; int count; } SPage;

#define CHECK(l, id, tip)          { K_CHECK,  l, id, 0, NULL, tip }
#define CHOICE(l, id, items, tip)  { K_CHOICE, l, id, (int)(sizeof(items) / sizeof(items[0])), items, tip }
#define BUTTON(l, id, tip)         { K_BUTTON, l, id, 0, NULL, tip }
#define DYNCHOICE(l, id, tip)      { K_DYN,    l, id, 0, NULL, tip }       /* a drop-down filled when the window opens: gui_dyn_list() */

static const char *const it_viewmode[] = { "List View", "Grid View" };
static const char *const it_theme[]    = { "System", "Light", "Dark" };
static const char *const it_style[]    = { "Classic", "Modern" };
static const char *const it_scale[]    = { "1x", "2x", "3x", "4x", "5x", "6x" };
static const char *const it_lcd[]      = { "Off", "Light", "Medium", "Heavy" };
static const char *const it_scan[]     = { "Off", "25%", "50%", "75%", "100%" };
static const char *const it_ntsc[]     = { "Off", "Composite", "S-Video", "RGB" };
static const char *const it_aspect[]   = { "Original Console Pixels", "1:1 (Square Pixels)", "4:3 (Standard)", "16:9 (Widescreen)" };
static const char *const it_borders[]  = { "Hide All", "Top and Bottom", "Left and Right", "Show All" };
static const char *const it_interlace[] = { "Single Field", "Double Field" };
static const char *const it_frameskip[] = { "Off", "Auto", "Manual (Below 25% Audio Buffer)",
                                            "Manual (Below 33% Audio Buffer)", "Manual (Below 50% Audio Buffer)",
                                            "Manual (Below 75% Audio Buffer)" };
static const char *const it_renderer[] = { "GDI (Software)", "Direct3D 9", "Direct3D 11" };
static const char *const it_fm[]       = { "YM2612 (Nuked)", "YM3438 (Nuked)", "YM2612 (MAME, Discrete)",
                                           "YM3438 (MAME, ASIC)", "YM3438 (MAME, Enhanced)" };
static const char *const it_backend[] = { "Windows Classic", "XAudio2", "WASAPI (Shared)", "WASAPI (Exclusive)" };
static const char *const it_fmunit[]  = { "Auto", "Off", "On" };
static const char *const it_opll[]    = { "YM2413 (Nuked)", "YM2413 (MAME)" };
static const char *const it_uisize[]  = { "Normal", "Large" };
static const char *const it_rate[]     = { "22050 Hz", "44100 Hz", "48000 Hz" };
static const char *const it_shot[]     = { "Final", "Corrected", "Raw" };
static const char *const it_shotsize[] = { "1x", "2x", "3x", "4x" };
static const char *const it_recqual[]  = { "Automatic", "Low (4 Mbit/s)", "Medium (8 Mbit/s)", "High (16 Mbit/s)", "Very High (32 Mbit/s)" };
static const char *const it_recshape[] = { "Console's Own", "4:3 (TV)", "16:9 (Widescreen)" };
static const char *const it_region[]   = { "Detect from ROM", "USA", "Europe", "Japan (NTSC)", "Japan (PAL)" };
static const char *const it_rewmem[]   = { "128 MB", "256 MB", "512 MB", "768 MB" };
static const char *const it_vdp[]      = { "Auto", "NTSC (60 Hz)", "PAL (50 Hz)" };
static const char *const it_console[]  = { "Detect from ROM", "SG-1000", "SG-1000 II", "SG-1000 II + RAM Adapter",
                                           "Mark III", "Master System", "Master System II", "Game Gear",
                                           "Mega Drive / Genesis" };
static const char *const it_stereo[]  = { "Narrow (50%)", "Normal (100%)", "Wide (150%)", "Extra Wide (200%)" };
static const char *const it_msens[]   = { "50%", "75%", "100%", "150%", "200%" };
static const char *const it_gunoff[]  = { "-12 px", "-8 px", "-4 px", "0 px", "+4 px", "+8 px", "+12 px" };
static const char *const it_barcol[]  = { "Black", "Dark Gray", "Gray", "White", "Navy", "Dark Green", "Dark Red" };
static const char *const it_fsmode[]  = { "Borderless Window", "Exclusive Fullscreen" };
static const char *const it_accent[] = { "Blue", "Brown", "Crimson", "Gold", "Gray", "Green", "Indigo", "Lime", "Magenta", "Navy", "Olive", "Orange", "Pink", "Purple", "Red", "Sky Blue", "Teal", "Violet", "Yellow" };
static const char *const it_tbsize[] = { "Small", "Normal", "Large" };
static const char *const it_coversize[] = { "Tiny", "Extra Small", "Small", "Medium", "Medium Large", "Large", "Extra Large" };
static const char *const it_coverbox[] = { "Square", "Box (Portrait)", "Wide" };
static const char *const it_cursor[]  = { "Never", "After 2 Seconds", "After 3 Seconds", "After 5 Seconds", "After 10 Seconds" };
static const char *const it_fcap[]    = { "Off", "30 per Second", "40 per Second", "50 per Second", "60 per Second" };
static const char *const it_bass[]    = { "Off", "Light", "Medium", "Strong" };
static const char *const it_abackup[] = { "Off", "Every Minute", "Every 2 Minutes", "Every 5 Minutes", "Every 10 Minutes" };
static const char *const it_rewspeed[] = { "Slow", "Normal", "Fast", "Very Fast" };
static const char *const it_skiplen[] = { "Off", "5 Seconds", "10 Seconds", "15 Seconds", "20 Seconds", "25 Seconds", "30 Seconds" };
static const char *const it_osdpos[]  = { "Top Left", "Top Right", "Bottom Left", "Bottom Right" };
static const char *const it_osdtime[] = { "1 Second", "2 Seconds", "3 Seconds", "4 Seconds", "5 Seconds", "6 Seconds", "7 Seconds" };
static const char *const it_osdcolor[] = { "White", "Yellow", "Green", "Cyan", "Orange", "Pink", "Red", "Black" };
static const char *const it_osdlines[] = { "1 Message", "2 Messages", "3 Messages", "4 Messages", "5 Messages", "6 Messages", "7 Messages" };
static const char *const it_osdsize[] = { "Small", "Normal", "Large", "Extra Large" };
static const char *const it_lockon[]   = { "None", "Game Genie", "Action Replay (Pro)", "Sonic & Knuckles" };

static const SRow rows_view[] =
{
  { K_GROUP, "View", 0, 0, NULL, NULL },
  CHOICE("View Mode:",            IDM_VIEW_LIST, it_viewmode, "How the game browser shows your games: a list or a grid of covers."),
  CHOICE("Style:",                IDM_SET_STYLE_BASE, it_style, "Classic keeps the look the program has always had. Modern gives flat rounded buttons, check boxes, drop-down lists and tabs, softer toolbar buttons, rounded corners and a title bar in the theme's color (Windows 11), and Segoe UI text, all at once."),
  CHOICE("Theme:",                IDM_VIDEO_THEME_BASE, it_theme, "System follows Windows; Light or Dark forces one."),
  CHOICE("Accent Color:",         IDM_SET_ACCENT_BASE, it_accent, "The color of the selected row in the program's own lists (User Guide, Saved Combos, the Settings drop-downs) and of the button that waits for a key. The default is blue in the dark theme and the Windows accent color in the light one."),
  CHECK ("Colorful Icons", IDM_OPT_COLORICONS, "Draw the icons of the menus, the toolbar and the favorite star in color, one color for each kind of action. Off (the default) draws them black and white, in the color of the text."),
  CHOICE("Interface Size:",       IDM_SET_UISIZE_BASE, it_uisize, "Normal or Large menus, dialogs and toolbar. Changing it restarts the program (it asks first, and your game is opened again)."),
  CHOICE("Toolbar Icon Size:",    IDM_SET_TBSIZE_BASE, it_tbsize, "How big the buttons of the toolbar are: Small, Normal (the default) or Large (also in View > Toolbars > Toolbar Size)."),
  CHOICE("Cover Size:",             IDM_SET_COVERSIZE_BASE, it_coversize, "How big the covers are in the grid view, from Tiny to Extra Large (also in View > View Mode > Grid Size). Only with the Grid View chosen. Ctrl + mouse wheel over the grid sets any size in between."),
  CHOICE("Cover Frame:",            IDM_SET_COVERSHAPE_BASE, it_coverbox, "The shape of the covers in the grid view: Square, Box (portrait, like a game case) or Wide."),
  CHECK ("Rounded Cover Corners",   IDM_OPT_COVERROUND, "Round the corners of the covers in the grid view."),
  CHECK ("Show Name Under Covers",  IDM_OPT_COVERCAP, "Show the game's name under each cover in the grid view; switch it off for only the pictures."),
  CHECK ("Frame Around Cover and Name", IDM_OPT_COVERCARD, "Draw a frame around each cover and its name in the grid view, so every game looks like a card."),
  CHECK ("Keep a Cover Cache",      IDM_OPT_COVERCACHE, "Save each finished cover in one file (Covers\\thumbs.dat), so the next time the program starts the covers appear almost at once instead of being read and scaled again. A cover that changes, or a new size or frame, is made again by itself."),
  BUTTON("Clear Cover Cache",       IDM_OPT_COVERCACHE_CLEAR, "Delete the saved covers; they are made again as you browse."),
  { K_GROUPEND, NULL, 0, 0, NULL, NULL },
  { K_GROUP, "Game Directories", 0, 0, NULL, NULL },
  BUTTON("ROM Directories...",      IDM_FILE_ROMFOLDERS, "Choose the directories the game list is made of: add more or remove some, and exclude some from the scan."),
  CHECK ("Scan ROM Subfolders", IDM_VIEW_SCANSUB, "Also look for games in the folders inside the ROM directory. Switch it off to list only the games directly in it."),
  { K_GROUPEND, NULL, 0, 0, NULL, NULL },
  { K_BREAK, NULL, 0, 0, NULL, NULL },
  { K_GROUP, "Program Behavior", 0, 0, NULL, NULL },
  CHECK ("Pause When Inactive",   IDM_EMU_PAUSE_UNFOCUSED, "Pause the game while another window has focus."),
  CHECK ("Mute When Inactive",    IDM_OPT_MUTEUNFOCUSED, "Silence the game while another window has focus (the game keeps running; see Pause When Inactive to stop it)."),
  CHECK ("Keep Only One Instance", IDM_OPT_SINGLEINSTANCE, "Starting the program again (or opening a game with it) brings the copy that is already running to the front, and opens the game there, instead of starting a second copy. Two copies share the same settings and save files and would overwrite each other. Takes effect the next time the program starts. Launches from a frontend (--exit-on-close) are not affected."),
  CHECK ("Always on Top",         IDM_VIDEO_ALWAYS_ON_TOP, "Keep the window above every other window (also in the View menu)."),
  CHECK ("Keep the PC Awake While Playing", IDM_OPT_KEEPAWAKE, "Stop Windows from sleeping, dimming the screen or starting the screen saver while a game is running."),
  CHECK ("Confirm Exit While Playing", IDM_OPT_CONFIRMEXIT, "Ask \"Exit?\" when you close the program while a game is running."),
  { K_GROUPEND, NULL, 0, 0, NULL, NULL },
};



static const SRow rows_video[] =
{
  { K_GROUP, "Window and Screen", 0, 0, NULL, NULL },
  CHOICE("Window Size:",          IDM_VIDEO_SCALE_BASE, it_scale, "Size of the game window, 1x to 6x the console's picture."),
  CHOICE("Fullscreen Mode:",      IDM_SET_FSMODE_BASE, it_fsmode, "Borderless Window fills the screen without changing it. Exclusive Fullscreen switches the screen to the Fullscreen Resolution while the game is fullscreen."),
  DYNCHOICE("Fullscreen Resolution:", IDM_SET_FSRES_BASE, "The screen size used in fullscreen when Fullscreen Mode is Exclusive Fullscreen: the screen is switched to it while the game is fullscreen and put back after. Desktop Resolution changes nothing."),
  CHECK ("Start ROM in Fullscreen", IDM_VIDEO_FULLSCREEN_START, "Start every game in fullscreen. Esc or Alt+Enter toggles it."),
  { K_GROUPEND, NULL, 0, 0, NULL, NULL },
  { K_GROUP, "Scaling and Shape", 0, 0, NULL, NULL },
  { K_FILTER, "Render Filter:", 0, 0, NULL, "Pixel-art scalers and CRT looks (Scale2x, xBR, HQ2x, xBRZ, Catmull-Rom, Scanlines, CRT...) applied before the picture is shown. They run on the processor: xBRZ 5x and 6x and CRT Geom are very heavy and can make a game run below full speed (watch the frame rate); 2x to 4x are much lighter, and a shader below is lighter still." },
  CHECK ("Integer Scaling",       IDM_VIDEO_INTSCALE, "Enlarge the picture by whole multiples only (1x, 2x, 3x ...), so every pixel is the same size. It is centered, with borders, when the window is between two sizes."),
  CHOICE("Aspect Ratio:",         IDM_VIDEO_ASPECT_BASE, it_aspect, "The console's original pixel shape (the default; the Mega Drive's own pixels: 8:7 wide in the 256-pixel mode, 32:35 in the 320-pixel one; other consoles use square pixels), 1:1 square pixels, 4:3 as on a CRT television, or 16:9 (stretches the picture to fill a widescreen window)."),
  CHOICE("Borders:",              IDM_VIDEO_OVERSCAN_BASE, it_borders, "Hide or show the overscan borders (top/bottom, left/right). This covers every console; the Master System side borders and the Game Gear screen have their own options in Settings > Options."),
  CHOICE("Interlaced Mode:",      IDM_VIDEO_INTERLACE_BASE, it_interlace, "Single Field (the default) or Double Field, for games that use the 448-line interlaced mode. Single Field shows the same half of the picture every frame, so it stays steady. Double Field shows both fields at once; where something moves, the older field's lines are rebuilt from the new ones and both lines of each pair are drawn partly alike, so there are no comb teeth and the picture hardly shakes (a moving part is a little softer than a still one)."),
  { K_GROUPEND, NULL, 0, 0, NULL, NULL },
  { K_GROUP, "Bezel and Border", 0, 0, NULL, NULL },
  BUTTON("Bezel Image...",        IDM_VIDEO_BEZEL, "Draw an image (a PNG with a see-through middle: a TV, an arcade cabinet) over the picture as a frame. Shown in fullscreen only."),
  BUTTON("Remove Bezel",          IDM_VIDEO_BEZEL_OFF, "Go back to plain bars around the picture."),
  CHOICE("Border Color:",         IDM_SET_BARCOLOR_BASE, it_barcol, "The color of the bars around the picture when the window or screen is a different shape."),
  { K_GROUPEND, NULL, 0, 0, NULL, NULL },
  { K_BREAK, NULL, 0, 0, NULL, NULL },
  { K_GROUP, "Filters and Effects", 0, 0, NULL, NULL },
  CHOICE("NTSC Filter:",          IDM_VIDEO_NTSC_BASE, it_ntsc, "Composite, S-Video or RGB television look. For a 32X game it is the program's own version of the look (the picture keeps its size), not the Genesis Plus GX core's."),
  CHOICE("LCD Ghosting:",         IDM_VIDEO_LCD_BASE, it_lcd, "Bright pixels fade slowly, like a handheld screen."),
  CHOICE("Scanlines:",            IDM_VIDEO_SCANLINE_BASE, it_scan, "Dark lines between rows: Off, 25, 50, 75 or 100%."),
  CHECK ("Smooth Scaling",        IDM_VIDEO_SMOOTH, "Soft edges when the picture is enlarged."),
  CHECK ("Dither Smoothing",      IDM_VIDEO_DITHER, "Smooths the checkerboard patterns many Genesis games use to fake extra colors."),
  CHECK ("Flicker Blending",      IDM_VIDEO_FLICKER, "Some games fake transparency by showing an object every other frame. This blends the two frames so it looks solid."),
  CHECK ("Brighten",              IDM_VIDEO_BRIGHTEN, "A gentle brightness lift."),
  BUTTON("Picture Adjustments...", IDM_VIDEO_PICTUREADJ, "Open the window with the Contrast, Saturation, Gamma and Hue sliders (also in the Video menu)."),
  { K_GROUPEND, NULL, 0, 0, NULL, NULL },
  { K_GROUP, "Speed and Smoothness", 0, 0, NULL, NULL },
  CHOICE("Renderer:",             IDM_VIDEO_RENDERER_BASE, it_renderer, "GDI draws with the CPU; Direct3D 9 and 11 use the graphics card (each falls back to GDI if unavailable). Shaders need Direct3D 11."),
  CHECK ("VSync",                 IDM_VIDEO_VSYNC, "Wait for the screen's refresh after each frame so motion is even. Skipped for a frame whenever the sound buffer runs low, so it never causes crackle."),
  CHOICE("Limit Redraws:",        IDM_SET_FRAMECAP_BASE, it_fcap, "Draw the picture at most this many times a second while the game still runs at full speed. Helps a weak PC or a very fast monitor; Off draws every frame. Unlike Frameskip, which skips drawing only when the PC falls behind, this is a fixed cap on how often the picture is drawn."),
  CHOICE("Frameskip:",            IDM_VIDEO_FRAMESKIP_BASE, it_frameskip, "Skip drawing some frames on a slow PC so the game keeps up: Off, Auto, or Manual by audio-buffer level. Unlike Limit Redraws (a fixed cap), it skips drawing only when the PC falls behind, so the picture is choppier only then. Not used during netplay or recording."),
  { K_GROUPEND, NULL, 0, 0, NULL, NULL },
#ifdef _WIN64
  { K_SHADER, "Shader:", 0, 0, NULL, "Run a librashader (.slangp) shader chain on top of the picture. Direct3D 11 only." },      /* along the bottom, under both columns */
  BUTTON("Shader Parameters...",    IDM_VIDEO_SHADERPARAMS, "Adjust the loaded shader's own parameters (sharpness, curvature ...) while it runs, without editing its preset file. Grayed until a shader that has settings is loaded."),   /* inside the shader box, under its file */
  CHECK ("Skip Render Filter with Shaders", IDM_VIDEO_SHADERNOFILTER, "While a shader runs, do not apply the Render Filter first. The shader then gets the game's own picture instead of one the filter already enlarged. Shaders that scale by whole multiples or place a bezel work out their sizes from the picture they receive, so one that looks too small or wrong with a Render Filter on may look right with this ticked. Has no effect without a shader or a Render Filter."),
#endif
};

static const SRow rows_audio[] =
{
  { K_GROUP, "Mute", 0, 0, NULL, NULL },
  CHECK ("Mute All Sound",         IDM_AUDIO_ENABLE, "Silence the sound; untick to hear it again."),
  CHECK ("Mute FM Sound",         IDM_AUDIO_MUTE_FM, "Silence the FM chip (the main music) and keep the rest."),
  CHECK ("Mute PSG Sound",        IDM_AUDIO_MUTE_PSG, "Silence the PSG chip (the square-wave sound) and keep the rest."),
  CHECK ("Mute PWM Sound",        IDM_AUDIO_MUTE_PWM, "Silence the PWM sound of a 32X game (most of its music and effects) and keep the rest. Grayed out only while a game of another system is running. Its level is the PWM Preamp slider on the Levels and Latency window."),
  { K_GROUPEND, NULL, 0, 0, NULL, NULL },
  { K_GROUP, "Tone", 0, 0, NULL, NULL },
  CHOICE("Stereo Width:",         IDM_SET_STEREO_BASE, it_stereo, "Narrow pulls the left and right sound toward the middle; wide spreads them apart."),
  CHOICE("Bass Boost:",           IDM_SET_BASS_BASE, it_bass, "Lift the low notes so the music is fuller on laptop speakers and small headphones."),
  CHECK ("Low-Pass Filter",       IDM_AUDIO_LOWPASS, "A soft high-frequency roll-off. Only one of Low-Pass Filter and 3-Band EQ can be on."),
  CHECK ("3-Band EQ",             IDM_AUDIO_EQ, "A three-band equalizer: raise or lower the low, middle and high sounds. Set the gains and the two crossover frequencies in Levels and Latency. Only one of Low-Pass Filter and 3-Band EQ can be on."),
  CHECK ("Mono Output",           IDM_AUDIO_MONO, "Mix the sound to mono instead of stereo."),
  CHECK ("Overdrive",             IDM_OPT_OVERDRIVE, "Push the sound harder into a soft limiter: louder, fuller and a little gritty, like a console turned up. Off keeps the clean sound."),
  CHECK ("Even Out Loudness",     IDM_OPT_NORMALIZE, "Lower very loud games and lift quiet ones so they sit at a similar level, without clipping."),
  BUTTON("Levels and Latency...", IDM_AUDIO_SETTINGS, "Master volume, FM and PSG levels, CD audio, how many frames of sound are buffered, the audio filter (Off, Low-Pass or 3-Band EQ) and the Sega CD PCM volume."),
  { K_GROUPEND, NULL, 0, 0, NULL, NULL },
  { K_BREAK, NULL, 0, 0, NULL, NULL },
  { K_GROUP, "Mega Drive Sound Chip", 0, 0, NULL, NULL },
  CHOICE("FM Chip:",              IDM_AUDIO_FMCORE_BASE, it_fm, "Which Genesis FM sound chip is emulated: YM2612 or YM3438, the Nuked versions (default YM2612 Nuked) or MAME. A 32X game (PicoDrive) has only the two chip types, so the YM3438 kinds play as a YM3438 and the others as a YM2612."),
  CHECK ("High-Quality FM Synthesis", IDM_AUDIO_HQFM, "Band-limited FM synthesis: cleaner high notes, for a little more CPU. For a 32X game it switches PicoDrive's FM filtering."),
  { K_GROUPEND, NULL, 0, 0, NULL, NULL },
  { K_GROUP, "Master System Sound Chip", 0, 0, NULL, NULL },
  CHOICE("FM Unit:", IDM_SET_FMUNIT_BASE, it_fmunit, "Whether the Master System's FM sound unit is emulated: Auto turns it on when the game uses it, Off never, On always. Changing it restarts the sound chip, so some instruments may stay quiet until the game sends them again."),
  CHOICE("FM Chip:", IDM_SET_OPLL_BASE, it_opll, "Which chip emulates the Master System's FM sound unit."),
  { K_GROUPEND, NULL, 0, 0, NULL, NULL },
  { K_GROUP, "Sound Quality", 0, 0, NULL, NULL },
  CHECK ("High-Quality PSG Resampling", IDM_AUDIO_HQPSG, "Cleaner sound from the PSG (the console's square-wave chip)."),
  CHOICE("Sample Rate:",          IDM_AUDIO_RATE_BASE, it_rate, "Audio sample rate: 22050 Hz (lighter, duller, for a slow PC), 44100 or 48000 Hz."),
  { K_GROUPEND, NULL, 0, 0, NULL, NULL },
  { K_GROUP, "Output", 0, 0, NULL, NULL },
  CHOICE("Audio Backend:",        IDM_SET_BACKEND_BASE, it_backend, "How the sound reaches your speakers. Windows Classic is the long-standing way. XAudio2 and WASAPI are newer Windows sound systems that can have lower delay and cope better when the PC is busy. Exclusive gives the game the sound device for itself: the lowest delay, but no other program can play sound on that device while the game runs. If one does not work on your PC, the game falls back (Exclusive to Shared, then to Windows Classic). Sound Device applies to all of them."),
  DYNCHOICE("Sound Device:",      IDM_SET_DEVICE_BASE, "Which speakers or headphones the game plays through. Windows Default follows the Windows setting."),
  { K_GROUPEND, NULL, 0, 0, NULL, NULL },
};

static const SRow rows_osd[] =
{
  { K_GROUP, "Messages", 0, 0, NULL, NULL },
  CHECK ("Show Messages",         IDM_OPTIONS_OSD, "Show short messages over the picture, such as \"Saved to slot 3\" or the screenshot name. Off hides them all (the status bar still shows them)."),
  CHECK ("Messages for Changed Options", IDM_OPT_OSDOPTS, "When you change an option, show its name and new value (\"Smooth Scaling: On\"). Needs Show Messages."),
  CHOICE("Message Position:",     IDM_SET_OSDPOS_BASE, it_osdpos, "Which corner of the picture the messages appear in."),
  CHOICE("Messages Shown:",       IDM_SET_OSDLINES_BASE, it_osdlines, "How many of the latest messages stay on screen together: the newest nearest the corner, older ones stacked beside it."),
  CHOICE("Message Time:",         IDM_SET_OSDTIME_BASE, it_osdtime, "How long a message stays on screen."),
  { K_GROUPEND, NULL, 0, 0, NULL, NULL },
  { K_BREAK, NULL, 0, 0, NULL, NULL },
  { K_GROUP, "Text", 0, 0, NULL, NULL },
  CHOICE("Font Color:",           IDM_SET_OSDCOLOR_BASE, it_osdcolor, "The color of messages, the frame rate and the pause menu title. The outline turns light for dark colors, so the text stays readable on any picture."),
  CHOICE("Font Size:",            IDM_SET_OSDSIZE_BASE, it_osdsize, "The size of the text of messages, the frame rate and the pause menu."),
  { K_GROUPEND, NULL, 0, 0, NULL, NULL },
  { K_GROUP, "Frame Rate", 0, 0, NULL, NULL },
  CHECK ("Show Frame Rate",       IDM_VIDEO_SHOWFPS, "Show frames per second on screen."),
  CHOICE("Frame Rate Position:",  IDM_SET_FPSPOS_BASE, it_osdpos, "Which corner of the picture the frame rate appears in."),
  { K_GROUPEND, NULL, 0, 0, NULL, NULL },
};

static const SRow rows_input[] =
{
  { K_GROUP, "Controllers", 0, 0, NULL, NULL },
  BUTTON("Controls...",  IDM_INPUT_P1, "Set keyboard keys and gamepad buttons for Player 1 and Player 2, the hotkeys and the stick deadzone."),
  BUTTON("Hotkeys...",   IDM_INPUT_HOTKEYS, "Change the keyboard shortcuts: Save State, Pause, Fullscreen and the rest."),
  CHECK ("Pause When the Pad Disconnects", IDM_OPT_PAUSEPAD, "Pause the game with a message when Player 1's gamepad is unplugged or its battery runs out, instead of running on without input."),
  CHECK ("Enable Background Input", IDM_INPUT_BACKGROUND, "Keep reading the keyboard pad keys and gamepads while another window has focus."),
  { K_GROUPEND, NULL, 0, 0, NULL, NULL },
  { K_GROUP, "Mouse", 0, 0, NULL, NULL },
  CHOICE("Mouse Speed:",          IDM_SET_MOUSESENS_BASE, it_msens, "How fast the Sega Mouse moves for the same hand movement."),
  CHOICE("Hide Mouse Pointer:",   IDM_SET_CURSORIDLE_BASE, it_cursor, "Hide the pointer over the game after it has not moved for this long (in a window and in fullscreen). Moving the mouse shows it again."),
  CHECK ("Invert Mouse Y Axis",   IDM_INPUT_INVERTMOUSE, "Flip the up/down movement of the Sega Mouse."),
  CHECK ("Confine Mouse to the Picture", IDM_OPT_CONFINE, "While a Sega Mouse or light gun game runs, keep the pointer inside the picture. Pause the game (or press Alt+Tab) to let it go."),
  { K_GROUPEND, NULL, 0, 0, NULL, NULL },
  { K_BREAK, NULL, 0, 0, NULL, NULL },
  { K_GROUP, "Light Gun", 0, 0, NULL, NULL },
  CHOICE("Gun Aim Offset X:", IDM_SET_GUNX_BASE, it_gunoff, "Shifts where the light gun hits, sideways, if shots land beside where you point."),
  CHOICE("Gun Aim Offset Y:", IDM_SET_GUNY_BASE, it_gunoff, "Shifts where the light gun hits, up or down, if shots land above or below where you point."),
  CHECK ("Show Light Gun Crosshair", IDM_INPUT_GUNCURSOR, "Show a crosshair instead of the arrow over the picture while a light gun is plugged in."),
  { K_GROUPEND, NULL, 0, 0, NULL, NULL },
};

static const SRow rows_options[] =
{
  { K_GROUP, "Console", 0, 0, NULL, NULL },
  CHOICE("Region:",               IDM_EMU_REGION_BASE, it_region, "Which region the console reports to the game: detect from the ROM, USA, Europe or Japan."),
  CHOICE("Force VDP Mode:",       IDM_EMU_VDPMODE_BASE, it_vdp, "Run the video timing at 60 Hz (NTSC) or 50 Hz (PAL) whatever the region says."),
  CHOICE("Console:",              IDM_EMU_SYSTEM_BASE, it_console, "Detect the console from the game, or force a specific model."),
  CHOICE("Lock-On Cartridge:",    IDM_EMU_LOCKON_BASE, it_lockon, "Attach a Game Genie, Action Replay or Sonic & Knuckles to the game."),
  CHECK ("Boot from BIOS When Available", IDM_EMU_BIOS, "Run the console's start-up ROM when a BIOS file is present in the BIOS folder."),
  CHECK ("Show Extended Game Gear Screen", IDM_VIDEO_GGEXTRA, "Show the whole Game Gear picture instead of only the part its screen displayed."),
  CHECK ("Show Master System Side Borders", IDM_VIDEO_SMSBORDER, "Show the 8-pixel columns at each side, which are cropped by default."),
  { K_GROUPEND, NULL, 0, 0, NULL, NULL },
  { K_GROUP, "Accuracy", 0, 0, NULL, NULL },
  CHECK ("Emulate Address Error Exceptions", IDM_EMU_ADDRERROR, "Hardware-accurate crashes on bad memory access; turn off only for a few hacked games."),
  CHECK ("Emulate Mega CD Disc Latency", IDM_EMU_CDLATENCY, "Make the Mega CD drive seek slowly like the real one; some games need it; off (the default) makes loading instant."),
  { K_GROUPEND, NULL, 0, 0, NULL, NULL },
  { K_GROUP, "Video Recording", 0, 0, NULL, NULL },
  CHOICE("Recording Size:",       IDM_SET_RECSIZE_BASE, it_shotsize, "How big a video recording (Video + Audio) is, as a multiple of the console's own picture: 1x is for example 320x224, 2x (default) 640x448, 4x 1280x896. Enlarged in whole pixels, so it stays sharp. Used for the next recording."),
  CHOICE("Recording Quality:",    IDM_SET_RECQUAL_BASE, it_recqual, "How much data a second of video gets. Automatic (default) picks it from the size. A higher value keeps more detail and makes a bigger file. Used for the next recording."),
  CHOICE("Recording Shape:",      IDM_SET_RECSHAPE_BASE, it_recshape, "Console's Own keeps the picture's pixels square. 4:3 (TV) stretches it to the shape of a TV, as the game looks in the window. 16:9 (Widescreen) stretches it over a widescreen video. Used for the next recording."),
  { K_GROUPEND, NULL, 0, 0, NULL, NULL },
  { K_GROUP, "Screenshots", 0, 0, NULL, NULL },
  CHOICE("Screenshot Output:",    IDM_TOOLS_SHOT_BASE, it_shot, "What a screenshot contains. Corrected (default): the picture stretched to the 4:3 shape of a TV, 640x480. Raw: the console's own picture at its own size (for example 320x224), not stretched. Final: like Corrected, but with the Render Filter applied (the same as Corrected when no filter is on). Scanlines and Brighten are never included."),
  CHOICE("Screenshot Size:",      IDM_TOOLS_SHOTSIZE_BASE, it_shotsize, "How big a screenshot is, as a multiple of the size the chosen output has: 1x is 640x480 for Corrected, the console's own size for Raw and the filter's size for Final. 2x, 3x and 4x make it that many times larger (Raw is enlarged in whole pixels, so it stays sharp)."),
  { K_GROUPEND, NULL, 0, 0, NULL, NULL },
  { K_BREAK, NULL, 0, 0, NULL, NULL },
  { K_GROUP, "Rewind and Resume", 0, 0, NULL, NULL },
  CHECK ("Enable Rewind",         IDM_EMU_REWIND, "Keep a history of recent play so holding Backspace runs it backwards. Uses 256 MB by default (Rewind History Size) and some CPU; not available during netplay."),
  CHOICE("Rewind Speed:",         IDM_SET_REWINDSPEED_BASE, it_rewspeed, "How fast Rewind runs backwards: Slow is half the speed of Normal (the default), Fast twice and Very Fast four times as fast. Faster rewinds go through the history sooner."),
  CHOICE("Rewind History Size:",  IDM_EMU_REWINDMEM_BASE, it_rewmem, "How much memory the Rewind history may use: the more, the further back you can go (how far depends on the game). Changing it starts the history again. Large sizes may not be available in the 32-bit program."),
  CHECK ("Auto Resume", IDM_EMU_AUTOSAVE, "Save the game when you close it, and offer to carry on from there the next time you open it."),
  { K_GROUPEND, NULL, 0, 0, NULL, NULL },
  { K_GROUP, "Start and Backup", 0, 0, NULL, NULL },
  CHOICE("Backup State:",         IDM_SET_AUTOSAVE_BASE, it_abackup, "While a game runs, quietly save its state every few minutes into a rolling backup (Emulation > Load Backup State), so a crash never loses much."),
  CHOICE("Skip Intro on Launch:", IDM_SET_SKIPLEN_BASE, it_skiplen, "When a game starts, run its first few seconds at full speed to get past logos and intros. A press of a game button ends the skip early. Not used when resuming a game or during netplay."),
  { K_GROUPEND, NULL, 0, 0, NULL, NULL },
  { K_GROUP, "Troubleshooting", 0, 0, NULL, NULL },
  CHECK ("Enable Log",      IDM_OPT_LOG, "Write genesis-plus-ex.log in the Logs folder next to the program: the program version, Windows and the main settings, games loaded and closed, resets, states, cheat codes and their timing, errors and netplay events. Off by default. Send it along with a bug report."),
  CHECK ("Enable Crash Report", IDM_OPT_CRASHREPORT, "If the program crashes, write genesis-plus-ex-crash.txt and genesis-plus-ex-crash.dmp in the Logs folder next to the program (only the last crash is kept). Off by default; send both with a bug report."),
  { K_GROUPEND, NULL, 0, 0, NULL, NULL },
};

#define PAGE(t, r) { t, r, (int)(sizeof(r) / sizeof(r[0])) }
static const SPage pages[] =
{
  PAGE("General",   rows_view),
  PAGE("Video",     rows_video),
  PAGE("Audio",     rows_audio),
  PAGE("Input",     rows_input),
  PAGE("Options",   rows_options),
  PAGE("OSD",       rows_osd),
};
#define NPAGES ((int)(sizeof(pages) / sizeof(pages[0])))

#define ROW_ID_BASE 4000
#define SHADER_EDIT_ID   5001      /* the shader box's file edit and its ... button */
#define SHADER_BROWSE_ID 5002
#define SHADER_CLEAR_ID  5004      /* the small x inside the file box */
#define RESUME_BTN_ID    5003      /* "Restore Dialog", beside Auto Resume */
#define MAX_ROWS    192

static const SRow *s_row[MAX_ROWS];
static HWND s_ctl[MAX_ROWS];
static int s_rowpage[MAX_ROWS];     /* which tab each row is on */
static int s_need_h;                /* the page height the tallest tab needs */
static HWND s_lab[MAX_ROWS];       /* the label in front of a slider or a drop-down (grayed with it) */
static HWND s_resume_btn;
static HWND s_page[NPAGES];
static int  s_nrows;

/****************************************************************************
 * Reading the menu
 ****************************************************************************/

/* State flags of the menu item with this command, wherever it is in the
   menus (looked for by hand, down through the submenus). */
static UINT find_state(HMENU menu, int id)
{
  int i, n = GetMenuItemCount(menu);

  for (i = 0; i < n; i++)
  {
    HMENU sub = GetSubMenu(menu, i);

    if (sub)
    {
      UINT st = find_state(sub, id);
      if (st != (UINT)-1) return st;
    }
    else if ((int)GetMenuItemID(menu, i) == id)
    {
      return GetMenuState(menu, (UINT)i, MF_BYPOSITION);
    }
  }
  return (UINT)-1;
}

static UINT menu_state(int id)
{
  if (gui_pico_unsupported(id) || gui_pico_only(id)) return MF_GRAYED | MF_DISABLED;      /* a 32X game is running: nothing here applies to it */
  if (id == IDM_VIEW_SCANSUB) return gui.scan_subfolders ? MF_CHECKED : 0;   /* a setting with no menu item */
  if (id == IDM_OPT_CONFIRMEXIT) return gui.confirm_exit ? MF_CHECKED : 0;
  if (id == IDM_OPT_LOG) return gui.log_on ? MF_CHECKED : 0;
  if (id == IDM_OPT_CRASHREPORT) return gui.crash_report ? MF_CHECKED : 0;
  if (id == IDM_EMU_REWIND) return gui.rewind ? MF_CHECKED : 0;      /* no longer in a menu: asked here */
  if (id == IDM_OPT_COVERCACHE) return gui.cover_cache ? MF_CHECKED : 0;
  if (id == IDM_OPT_COVERCARD) return gui.cover_card ? MF_CHECKED : 0;
  if (id == IDM_OPT_COLORICONS) return gui.color_icons ? MF_CHECKED : 0;
  if (id == IDM_OPT_PAUSEPAD) return gui.pause_pad ? MF_CHECKED : 0;
  if (id == IDM_OPT_OSDOPTS) return gui.osd_opts ? MF_CHECKED : 0;
  if (id == IDM_OPT_OVERDRIVE) return gui.overdrive ? MF_CHECKED : 0;
  if (id == IDM_OPT_NORMALIZE) return gui.normalize ? MF_CHECKED : 0;
  if (id == IDM_OPT_SINGLEINSTANCE) return gui.single_instance ? MF_CHECKED : 0;
  if (id == IDM_OPT_KEEPAWAKE) return gui.keep_awake ? MF_CHECKED : 0;
  if (id == IDM_OPT_MUTEUNFOCUSED) return gui.mute_unfocused ? MF_CHECKED : 0;
  if (id == IDM_EMU_BIOS) return config.bios ? MF_CHECKED : 0;                       /* no longer in the Options menu */
  if (id == IDM_EMU_ADDRERROR) return config.addr_error ? MF_CHECKED : 0;
  if (id == IDM_EMU_CDLATENCY) return config.cd_latency ? MF_CHECKED : 0;
  if (id == IDM_VIDEO_GGEXTRA) return config.gg_extra ? MF_CHECKED : 0;
  if (id == IDM_VIDEO_SMSBORDER) return gui.sms_show_border ? MF_CHECKED : 0;
  if (id == IDM_VIDEO_SHOWFPS) return gui.show_fps ? MF_CHECKED : 0;
  if (id == IDM_OPTIONS_OSD) return gui.osd ? MF_CHECKED : 0;
  if (id == IDM_INPUT_GUNCURSOR) return config.gun_cursor[0] ? MF_CHECKED : 0;       /* no longer in the Input menu */
  if (id == IDM_INPUT_INVERTMOUSE) return config.invert_mouse ? MF_CHECKED : 0;
  if (id == IDM_OPT_CONFINE) return gui.confine_mouse ? MF_CHECKED : 0;
  if (id == IDM_OPT_COVERROUND) return gui.cover_round ? MF_CHECKED : 0;
  if (id == IDM_VIEW_STATUSBAR) return gui.hide_statusbar ? 0 : MF_CHECKED;
  if (id == IDM_OPT_COVERCAP) return gui.cover_caption ? 0 : MF_CHECKED;
  if (id == IDM_INPUT_COMBOS) return 0;
  if (id == IDM_VIDEO_SHADERPARAMS) return shader_chain_param_count() ? 0 : MF_GRAYED;
  if (id == IDM_VIDEO_SHADERNOFILTER) return (gui.shader_nofilter ? MF_CHECKED : 0) | (gui.shader_preset_path[0] ? 0 : MF_GRAYED);
  if (id == IDM_AUDIO_HQFM) return config.hq_fm ? MF_CHECKED : 0;
  if (id >= IDM_SET_UISIZE_BASE && id < IDM_SET_UISIZE_BASE + 2) return (gui_setgroup_state(id) > 0 ? MF_CHECKED : 0) | (gui_game_running() ? (MF_GRAYED | MF_DISABLED) : 0);      /* the program restarts for it: not while a game runs */
  if (id == IDM_AUDIO_EQ) return config.filter == 2 ? MF_CHECKED : 0;
  if (id == IDM_VIDEO_FLICKER) return gui.flicker_blend ? MF_CHECKED : 0;
  if (id == IDM_VIDEO_DITHER) return gui.dither_smooth ? MF_CHECKED : 0;
  if (id == IDM_VIDEO_BEZEL_OFF) return gui.bezel_path[0] ? 0 : (MF_GRAYED | MF_DISABLED);
  if (id == IDM_VIDEO_BEZEL) return 0;
  { int g = gui_setgroup_state(id); if (g >= 0) return g ? MF_CHECKED : 0; }
  if (id == IDM_AUDIO_DEVICE) return 0;
  return find_state(gui_main_menu(), id);
}

static int menu_checked(int id)
{
  UINT st = menu_state(id);
  return st != (UINT)-1 && (st & MF_CHECKED);
}

static void sync_shader(void);

static void sync_all(void)
{
  int i;

  if (s_resume_btn) EnableWindow(s_resume_btn, gui.resume_choice != 0);

  for (i = 0; i < s_nrows; i++)
  {
    const SRow *r = s_row[i];
    HWND c = s_ctl[i];

    if (r->kind == K_CHECK)
    {
      UINT st = menu_state(r->id);
      SendMessage(c, BM_SETCHECK, (st != (UINT)-1 && (st & MF_CHECKED)) ? BST_CHECKED : BST_UNCHECKED, 0);
      EnableWindow(c, st == (UINT)-1 || !(st & (MF_GRAYED | MF_DISABLED)));
    }
    else if (r->kind == K_CHOICE)
    {
      int k, sel = -1;
      UINT st0 = menu_state(r->id);
      for (k = 0; k < r->n; k++)
        if (menu_checked(r->id + k)) { sel = k; break; }
      if (r->id == IDM_SET_ACCENT_BASE && gui.accent >= r->n && gui.accent < (int)SendMessage(c, CB_GETCOUNT, 0, 0)) sel = gui.accent;   /* the hidden one, once it is in the list */
      SendMessage(c, CB_SETCURSEL, (WPARAM)sel, 0);
      EnableWindow(c, st0 == (UINT)-1 || !(st0 & (MF_GRAYED | MF_DISABLED)));     /* a choice grayed in the menu is grayed here too */
      if (s_lab[i]) EnableWindow(s_lab[i], IsWindowEnabled(c));                  /* and so is its label */
    }
    else if (r->kind == K_DYN)
    {
      char names[64][96];
      int n, cur = 0, k;
      n = gui_dyn_list(r->id, names, 64, &cur);
      if ((int)SendMessage(c, CB_GETCOUNT, 0, 0) != n)
      {
        SendMessage(c, CB_RESETCONTENT, 0, 0);
        for (k = 0; k < n; k++) SendMessageA(c, CB_ADDSTRING, 0, (LPARAM)names[k]);
        theme_fit_dropdown(c);
      }
      SendMessage(c, CB_SETCURSEL, (WPARAM)cur, 0);
    }
    else if (r->kind == K_FILTER)
    {
      int cur = video_filter_current();
      SendMessage(c, CB_SETCURSEL, (WPARAM)(cur >= 0 ? cur + 1 : 0), 0);
    }
    else if (r->kind == K_SHADER)
    {
      sync_shader();
    }
    else if (r->kind == K_BUTTON)
    {
      UINT st = menu_state(r->id);
      EnableWindow(c, st == (UINT)-1 || !(st & (MF_GRAYED | MF_DISABLED)));
    }
  }
}

/****************************************************************************
 * The shader box: a frame with "Enable Shader", and the preset file below
 * it (typed, or picked with the ... button). Turning it off keeps the file
 * in the box so it can be switched back on.
 ****************************************************************************/

static HWND s_shader_edit;         /* the file box */
static HWND s_shader_browse;       /* its ... button */
static HWND s_shader_clear;        /* the x inside the file box */
static HWND s_shader_check;
static int  s_shader_want;         /* "Enable Shader" ticked while no file is chosen yet */
static RECT s_frame_rc;            /* the shader box's frame, in its page's coordinates */
#define GRP_MAX 32
static RECT s_grp_rc[GRP_MAX];           /* frames around groups of rows, in their page's coordinates */
static HWND s_grp_page[GRP_MAX];
static const char *s_grp_title[GRP_MAX];
static const char *s_frame_title;     /* the shader box's title */
static int  s_grp_n;
static HWND s_frame_page;        /* "Enable Shader" */
static char s_shader_last[GUI_PATH_LEN];

static void sync_shader(void)
{
  if (!s_shader_check) return;

  if (gui.shader_preset_path[0]) lstrcpynA(s_shader_last, gui.shader_preset_path, sizeof(s_shader_last));

  if (gui.shader_preset_path[0]) s_shader_want = 0;
  {
    int on = gui.shader_preset_path[0] != 0 || s_shader_want;
    SendMessage(s_shader_check, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowTextA(s_shader_edit, s_shader_last);

    /* The file box and its button are only usable while the shader is on. */
    EnableWindow(s_shader_edit, on);
    EnableWindow(s_shader_browse, on && shader_chain_available());
    if (s_shader_clear)
    {
      SendMessage(s_shader_edit, EM_SETMARGINS, EC_RIGHTMARGIN, MAKELPARAM(0, TBS(26)));   /* the text stays clear of the x, as in the search box */
      ShowWindow(s_shader_clear, (on && s_shader_last[0]) ? SW_SHOWNA : SW_HIDE);
      SetWindowPos(s_shader_clear, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
  }
  {
    /* "Skip Render Filter with Shaders" only matters while a shader is loaded. */
    int i;
    for (i = 0; i < s_nrows; i++)
      if (s_row[i] && s_row[i]->kind == K_CHECK && s_row[i]->id == IDM_VIDEO_SHADERNOFILTER && s_ctl[i])
        EnableWindow(s_ctl[i], gui.shader_preset_path[0] != 0);
  }
}

/****************************************************************************
 * The tab strip: drawn here, in the theme's colors (the system tab control
 * keeps a white body and frame in dark mode).
 ****************************************************************************/

#define TABS_CHANGED GP_TABS_CHANGED   /* notification code of WM_COMMAND, selection in the control's userdata */

static int tabs_hot = -1;

static COLORREF page_color(void);

static int tab_h(void) { return TBS(24); }      /* height of the tabs; the page frame starts here */

/* Classic tabs: a framed page with raised tabs on top, in the theme's colors. */
static void tabs_colors(COLORREF *face, COLORREF *page, COLORREF *idle, COLORREF *hot,
                        COLORREF *text, COLORREF *dim, COLORREF *border)
{
  int dark = theme_is_dark();
  *face   = dark ? THEME_DARK_BG    : GetSysColor(COLOR_BTNFACE);     /* behind the control */
  *page   = page_color();                                               /* the page and the selected tab */
  *idle   = dark ? RGB(44, 44, 44)    : RGB(226, 226, 226);             /* the other tabs */
  *hot    = dark ? RGB(58, 58, 58)    : RGB(238, 238, 238);             /* a tab under the mouse */
  *text   = dark ? THEME_DARK_TEXT : GetSysColor(COLOR_BTNTEXT);
  *dim    = dark ? RGB(175, 175, 175) : RGB(70, 70, 70);
  *border = dark ? RGB(95, 95, 95)    : RGB(160, 160, 160);
}

/* The strip shows the Settings pages unless a window gave it its own titles
   (settings_set_tabs). */
static int tab_count(HWND h)
{
  int n = (int)(INT_PTR)GetPropA(h, "gp_ntabs");
  return n ? n : NPAGES;
}

static const char *tab_title(HWND h, int i)
{
  const char *const *t = (const char *const *)GetPropA(h, "gp_tabs");
  return t ? t[i] : pages[i].title;
}

/* Width of tab i: its text and some room either side. */
static int tab_width(HWND h, HDC dc, int i)
{
  SIZE sz;
  GetTextExtentPoint32A(dc, tab_title(h, i), lstrlenA(tab_title(h, i)), &sz);
  return sz.cx + 2 * TBS(14);
}

/* The letter that opens tab i with Alt: a number ending its title if there is one ("Player 2" is Alt+2), else the first letter
   of the title that no tab before it has taken. Returns the letter (lower case) and where it stands in the title, or 0. */
static int tab_mnemonic(HWND h, int i, int *at)
{
  char used[256];
  int j, k;

  memset(used, 0, sizeof(used));
  for (j = 0; j <= i; j++)
  {
    const char *t = tab_title(h, j);
    int n = lstrlenA(t), pick = -1;

    if (n > 0 && t[n - 1] >= '0' && t[n - 1] <= '9' && !used[(unsigned char)t[n - 1]]) pick = n - 1;
    for (k = 0; pick < 0 && k < n; k++)
      if (isalnum((unsigned char)t[k]) && !used[tolower((unsigned char)t[k])]) pick = k;
    if (pick < 0) continue;
    used[tolower((unsigned char)t[pick])] = 1;
    if (j == i) { if (at) *at = pick; return tolower((unsigned char)t[pick]); }
  }
  return 0;
}

static int tab_by_mnemonic(HWND h, int ch)
{
  int i;
  for (i = 0; i < tab_count(h); i++)
    if (tab_mnemonic(h, i, NULL) == tolower(ch)) return i;
  return -1;
}

#define TAB_LEFT TBS(2)           /* the first tab starts a little in from the frame */

static int tab_at(HWND h, int x, int y)
{
  HDC dc = GetDC(h);
  HFONT old = (HFONT)SelectObject(dc, (HFONT)SendMessage(GetParent(h), WM_GETFONT, 0, 0));
  int i, left = TAB_LEFT, hit = -1;

  if (y < tab_h())
    for (i = 0; i < tab_count(h); i++)
    {
      int w = tab_width(h, dc, i);
      if (x >= left && x < left + w) { hit = i; break; }
      left += w;
    }
  SelectObject(dc, old);
  ReleaseDC(h, dc);
  return hit;
}

/* Where tab `sel` is, from the strip's left edge: its page's frame leaves a gap
   there, so the tab and the page run into each other. Returns 0 without a strip. */
static HWND s_tabs;

static int tab_span(int sel, int *x0, int *x1)
{
  HDC dc;
  HFONT old;
  int i, left = TAB_LEFT;

  if (!s_tabs || sel < 0 || sel >= NPAGES) return 0;

  dc = GetDC(s_tabs);
  old = (HFONT)SelectObject(dc, (HFONT)SendMessage(GetParent(s_tabs), WM_GETFONT, 0, 0));
  for (i = 0; i < sel; i++) left += tab_width(s_tabs, dc, i);
  *x0 = left - TBS(1);
  *x1 = left + tab_width(s_tabs, dc, sel) + TBS(1);
  SelectObject(dc, old);
  ReleaseDC(s_tabs, dc);
  return 1;
}

static void tabs_select(HWND h, int sel)
{
  if (sel < 0 || sel >= tab_count(h) || sel == (int)GetWindowLongPtr(h, GWLP_USERDATA)) return;
  SetWindowLongPtr(h, GWLP_USERDATA, sel);
  InvalidateRect(h, NULL, FALSE);
  SendMessage(GetParent(h), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(h), TABS_CHANGED), (LPARAM)h);
}

/* Alt and a letter opens the tab that letter belongs to (the dialog manager would only beep: the tabs are not buttons). The message is
   taken while the dialog box loop reads it. */
static HHOOK s_mn_hook;
static HWND s_mn_tab;

static LRESULT CALLBACK tabs_alt_hook(int code, WPARAM wp, LPARAM lp)
{
  const MSG *m = (const MSG *)lp;

  if (code == MSGF_DIALOGBOX && m && m->message == WM_SYSCHAR && s_mn_tab && IsWindow(s_mn_tab))
  {
    HWND dlg = GetParent(s_mn_tab);
    if (m->hwnd == dlg || IsChild(dlg, m->hwnd))
    {
      int idx = tab_by_mnemonic(s_mn_tab, (int)m->wParam);
      if (idx >= 0)
      {
        SetFocus(s_mn_tab);
        tabs_select(s_mn_tab, idx);
        return 1;
      }
    }
  }
  return CallNextHookEx(s_mn_hook, code, wp, lp);
}

static LRESULT CALLBACK tabs_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
  switch (m)
  {
    case WM_GETDLGCODE:
      return DLGC_WANTARROWS;

    case WM_ERASEBKGND:
      return 1;

    case WM_PAINT:
    {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      RECT rc, r;
      COLORREF face, page, idle, hotc, text, dim, border;
      HBRUSH bface, bborder, bpage;
      HFONT old = (HFONT)SelectObject(dc, (HFONT)SendMessage(GetParent(h), WM_GETFONT, 0, 0));
      int i, left = TAB_LEFT, sel = (int)GetWindowLongPtr(h, GWLP_USERDATA), th = tab_h();

      GetClientRect(h, &rc);
      tabs_colors(&face, &page, &idle, &hotc, &text, &dim, &border);
      bface = CreateSolidBrush(face);
      bborder = CreateSolidBrush(border);
      bpage = CreateSolidBrush(page);

      FillRect(dc, &rc, bface);

      /* The line along the bottom of the strip: the top of the pages' frame
         continues it just below. */
      r = rc; r.top = r.bottom - 1;
      FillRect(dc, &r, bborder);

      SetBkMode(dc, TRANSPARENT);
      for (i = 0; i < tab_count(h); i++)
      {
        int tw = tab_width(h, dc, i);
        int is_sel = (i == sel);
        RECT t;
        HBRUSH bt;

        if (gui.ui_style)        /* Modern: no boxes, a soft plate under the tab the mouse is on, an accent line under the chosen one */
        {
          RECT mt;
          mt.left = left; mt.right = left + tw; mt.top = 0; mt.bottom = th;
          if (!is_sel && i == tabs_hot)
          {
            HBRUSH hb = CreateSolidBrush(hotc);
            HGDIOBJ ob = SelectObject(dc, hb), op = SelectObject(dc, GetStockObject(NULL_PEN));
            RoundRect(dc, mt.left + TBS(2), mt.top + TBS(3), mt.right - TBS(2), mt.bottom - TBS(4), TBS(8), TBS(8));
            SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(hb);
          }
          SetTextColor(dc, is_sel ? text : dim);
          DrawTextA(dc, tab_title(h, i), -1, &mt, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
          if (is_sel)
          {
            RECT bar = mt;
            HBRUSH ab = CreateSolidBrush(theme_modern_accent());
            bar.left += TBS(8); bar.right -= TBS(8); bar.top = bar.bottom - TBS(3); bar.bottom -= 1;
            FillRect(dc, &bar, ab);
            DeleteObject(ab);
          }
          left += tw;
          continue;
        }

        /* The selected tab is raised: taller, and open at the bottom so the
           page's frame does not run under it. */
        t.left = left; t.right = left + tw;
        t.top = is_sel ? 0 : TBS(3);
        t.bottom = th;
        if (is_sel) { t.left -= TBS(1); t.right += TBS(1); }
        if (is_sel) t.bottom = th;   /* its bottom row is page-colored below, the frame has a gap there */

        bt = CreateSolidBrush(is_sel ? page : (i == tabs_hot ? hotc : idle));
        FillRect(dc, &t, bborder);                        /* the tab's outline ... */
        { RECT in = t; in.left++; in.right--; in.top++; if (!is_sel) in.bottom--; FillRect(dc, &in, bt); }   /* ... and its face */
        if (is_sel)                                        /* the accent color (the default blue too): a bar along the top of the chosen tab */
        {
          RECT bar = t;
          HBRUSH ab = CreateSolidBrush(theme_accent_bg());
          bar.left++; bar.right--; bar.top++; bar.bottom = bar.top + TBS(3);
          FillRect(dc, &bar, ab);
          DeleteObject(ab);
        }
        DeleteObject(bt);

        SetTextColor(dc, is_sel ? text : dim);
        {
          RECT tx = t;
          char shown[100];
          const char *ti = tab_title(h, i);
          int at = 0, mn = tab_mnemonic(h, i, &at), n = lstrlenA(ti);

          if (!is_sel) tx.top += 1;
          if (mn && n < 90) { memcpy(shown, ti, (size_t)at); shown[at] = '&'; memcpy(shown + at + 1, ti + at, (size_t)(n - at) + 1); }
          else lstrcpynA(shown, ti, sizeof(shown));
          /* the letter that opens the tab with Alt is underlined once Alt has been pressed */
          DrawTextA(dc, shown, -1, &tx, DT_CENTER | DT_VCENTER | DT_SINGLELINE | (GetPropA(h, "gpacc") ? 0 : DT_HIDEPREFIX));
        }

        if (is_sel && GetFocus() == h && GetPropA(h, "gpcues"))      /* the dotted focus rectangle, as in classic tabs: only once Alt (or Tab) was pressed */
        {
          RECT f = t;
          InflateRect(&f, -TBS(4), -TBS(4));
          DrawFocusRect(dc, &f);
        }
        left += tw;
      }

      DeleteObject(bface); DeleteObject(bborder); DeleteObject(bpage);
      SelectObject(dc, old);
      EndPaint(h, &ps);
      return 0;
    }

    case WM_LBUTTONDOWN:
      SetFocus(h);
      tabs_select(h, tab_at(h, (short)LOWORD(l), (short)HIWORD(l)));
      return 0;

    case WM_MOUSEMOVE:
    {
      int hot = tab_at(h, (short)LOWORD(l), (short)HIWORD(l));
      if (hot != tabs_hot)
      {
        TRACKMOUSEEVENT te;
        te.cbSize = sizeof(te); te.dwFlags = TME_LEAVE; te.hwndTrack = h; te.dwHoverTime = 0;
        TrackMouseEvent(&te);
        tabs_hot = hot;
        InvalidateRect(h, NULL, FALSE);
      }
      return 0;
    }

    case WM_MOUSELEAVE:
      tabs_hot = -1;
      InvalidateRect(h, NULL, FALSE);
      return 0;

    case WM_KEYDOWN:
      if (w == VK_LEFT)  tabs_select(h, (int)GetWindowLongPtr(h, GWLP_USERDATA) - 1);
      if (w == VK_RIGHT) tabs_select(h, (int)GetWindowLongPtr(h, GWLP_USERDATA) + 1);
      return 0;

    case WM_SETFOCUS:
    case WM_KILLFOCUS:
      InvalidateRect(h, NULL, FALSE);
      return 0;

    case WM_UPDATEUISTATE:                      /* Windows says so when Alt or Tab is pressed: the focus rectangle may show */
    {
      LRESULT r = DefWindowProc(h, m, w, l);
      if (HIWORD(w) & UISF_HIDEFOCUS)
      {
        if (LOWORD(w) == UIS_CLEAR) SetPropA(h, "gpcues", (HANDLE)1);
        else if (LOWORD(w) == UIS_SET) RemovePropA(h, "gpcues");
        InvalidateRect(h, NULL, FALSE);
      }
      if (HIWORD(w) & UISF_HIDEACCEL)
      {
        if (LOWORD(w) == UIS_CLEAR) SetPropA(h, "gpacc", (HANDLE)1);
        else if (LOWORD(w) == UIS_SET) RemovePropA(h, "gpacc");
        InvalidateRect(h, NULL, FALSE);
      }
      return r;
    }

    case WM_CREATE:
      if (!s_mn_hook) { s_mn_tab = h; s_mn_hook = SetWindowsHookExA(WH_MSGFILTER, tabs_alt_hook, NULL, GetCurrentThreadId()); }
      break;

    case WM_NCDESTROY:
      RemovePropA(h, "gpcues");
      RemovePropA(h, "gpacc");
      if (s_mn_tab == h && s_mn_hook) { UnhookWindowsHookEx(s_mn_hook); s_mn_hook = NULL; s_mn_tab = NULL; }
      break;
  }
  return DefWindowProc(h, m, w, l);
}

/****************************************************************************
 * Building the pages
 ****************************************************************************/

static COLORREF page_color(void)
{
  return theme_is_dark() ? THEME_DARK_BG : GetSysColor(COLOR_BTNFACE);
}

/* A page is a plain window that paints its own background and hands what its
   controls send to the dialog. */
/* A frame's title, sitting in a gap of its top line. */
static void frame_title(HDC dc, HWND page, const RECT *fr, const char *title)
{
  COLORREF face, pg, idle, hotc, text, dim, border;
  HFONT f = (HFONT)SendMessage(GetParent(page), WM_GETFONT, 0, 0);
  HGDIOBJ of = SelectObject(dc, f ? f : GetStockObject(DEFAULT_GUI_FONT));
  SIZE sz;
  RECT gap;
  HBRUSH gb = CreateSolidBrush(page_color());
  int tx = fr->left + TBS(12);

  tabs_colors(&face, &pg, &idle, &hotc, &text, &dim, &border);
  GetTextExtentPoint32A(dc, title, lstrlenA(title), &sz);
  SetRect(&gap, tx - TBS(4), fr->top - sz.cy / 2, tx + sz.cx + TBS(4), fr->top - sz.cy / 2 + sz.cy);
  FillRect(dc, &gap, gb);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, text);
  TextOutA(dc, tx, gap.top, title, lstrlenA(title));
  SelectObject(dc, of);
  DeleteObject(gb);
}

/* A frame with a title, drawn on a dialog (the Combos tab of Controls) in the same style as the Settings frames. */
void settings_draw_frame(HDC dc, HWND dlg, const RECT *fr, const char *title)
{
  int dark = theme_is_dark();
  COLORREF bg = dark ? THEME_DARK_BG : GetSysColor(COLOR_BTNFACE);
  HPEN pen = CreatePen(PS_SOLID, 1, dark ? THEME_DARK_LINE : THEME_LIGHT_LINE);
  HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
  HFONT f = (HFONT)SendMessage(dlg, WM_GETFONT, 0, 0);
  HGDIOBJ of;
  HBRUSH gb = CreateSolidBrush(bg);
  SIZE sz;
  RECT gap;
  int tx = fr->left + TBS(12);

  RoundRect(dc, fr->left, fr->top, fr->right, fr->bottom, TBS(6), TBS(6));
  SelectObject(dc, op); SelectObject(dc, ob);
  DeleteObject(pen);

  of = SelectObject(dc, f ? f : GetStockObject(DEFAULT_GUI_FONT));
  GetTextExtentPoint32A(dc, title, lstrlenA(title), &sz);
  SetRect(&gap, tx - TBS(4), fr->top - sz.cy / 2, tx + sz.cx + TBS(4), fr->top - sz.cy / 2 + sz.cy);
  FillRect(dc, &gap, gb);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, dark ? THEME_DARK_TEXT : GetSysColor(COLOR_WINDOWTEXT));
  TextOutA(dc, tx, gap.top, title, lstrlenA(title));
  SelectObject(dc, of);
  DeleteObject(gb);
}

static LRESULT CALLBACK page_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
  switch (m)
  {
    case WM_NOTIFY:
      if (((const NMHDR *)l)->code == NM_CUSTOMDRAW)
      {
        char cls[32];
        GetClassNameA(((const NMHDR *)l)->hwndFrom, cls, sizeof(cls));
        if (!lstrcmpiA(cls, "msctls_trackbar32")) return theme_trackbar_customdraw((NMHDR *)l, CDRF_DODEFAULT);     /* the sliders of the page */
      }
      break;

#ifdef GPEX_EGGS
    case WM_CONTEXTMENU:
    {
      int i;
      for (i = 0; i < s_nrows; i++)
        if (s_row[i] && s_row[i]->id == IDM_SET_ACCENT_BASE && s_ctl[i] && (HWND)w == s_ctl[i])
        {
          if (eggs_accent_tap())
          {
            if (SendMessage(s_ctl[i], CB_GETCOUNT, 0, 0) < GUI_ACCENT_HIDDEN + 1) SendMessageA(s_ctl[i], CB_ADDSTRING, 0, (LPARAM)"Chaos Emerald");
            SendMessage(s_ctl[i], CB_SETCURSEL, GUI_ACCENT_HIDDEN, 0);
            SendMessage(GetParent(h), WM_COMMAND, MAKEWPARAM(ROW_ID_BASE + i, CBN_SELCHANGE), (LPARAM)s_ctl[i]);
          }
          return 0;
        }
      break;
    }
#endif

    case WM_ERASEBKGND:
    case WM_PRINTCLIENT:
    {
      RECT r;
      HBRUSH b = CreateSolidBrush(page_color());
      GetClientRect(h, &r);
      FillRect((HDC)w, &r, b);
      DeleteObject(b);

      {   /* The page's frame; its top edge has a gap under the selected tab. */
        COLORREF face, pg, idle, hotc, text, dim, border;
        HBRUSH bb;
        RECT l;
        int x0, x1;

        tabs_colors(&face, &pg, &idle, &hotc, &text, &dim, &border);
        bb = CreateSolidBrush(border);

        SetRect(&l, 0, r.bottom - 1, r.right, r.bottom);  FillRect((HDC)w, &l, bb);   /* bottom */
        SetRect(&l, 0, 0, 1, r.bottom);                   FillRect((HDC)w, &l, bb);   /* left */
        SetRect(&l, r.right - 1, 0, r.right, r.bottom);   FillRect((HDC)w, &l, bb);   /* right */

        if (tab_span((int)GetWindowLongPtr(s_tabs, GWLP_USERDATA), &x0, &x1))
        {
          SetRect(&l, 0, 0, x0 + 1, 1);                   FillRect((HDC)w, &l, bb);
          SetRect(&l, x1 - 1, 0, r.right, 1);             FillRect((HDC)w, &l, bb);
        }
        else
        {
          SetRect(&l, 0, 0, r.right, 1);                  FillRect((HDC)w, &l, bb);
        }
        DeleteObject(bb);
      }

      {   /* frames around groups of rows */
        int g;
        for (g = 0; g < s_grp_n; g++)
          if (h == s_grp_page[g])
          {
            HPEN pen = CreatePen(PS_SOLID, 1, theme_is_dark() ? THEME_DARK_LINE : THEME_LIGHT_LINE);
            HGDIOBJ op = SelectObject((HDC)w, pen), ob = SelectObject((HDC)w, GetStockObject(NULL_BRUSH));
            RoundRect((HDC)w, s_grp_rc[g].left, s_grp_rc[g].top, s_grp_rc[g].right, s_grp_rc[g].bottom, TBS(6), TBS(6));
            SelectObject((HDC)w, op); SelectObject((HDC)w, ob);
            DeleteObject(pen);

            if (s_grp_title[g]) frame_title((HDC)w, h, &s_grp_rc[g], s_grp_title[g]);
          }
      }

      if (h == s_frame_page && !IsRectEmpty(&s_frame_rc))   /* the shader box's frame */
      {
        HPEN pen = CreatePen(PS_SOLID, 1, theme_is_dark() ? THEME_DARK_LINE : THEME_LIGHT_LINE);
        HGDIOBJ op = SelectObject((HDC)w, pen), ob = SelectObject((HDC)w, GetStockObject(NULL_BRUSH));
        RoundRect((HDC)w, s_frame_rc.left, s_frame_rc.top, s_frame_rc.right, s_frame_rc.bottom, TBS(6), TBS(6));
        SelectObject((HDC)w, op); SelectObject((HDC)w, ob);
        DeleteObject(pen);
        if (s_frame_title) frame_title((HDC)w, h, &s_frame_rc, s_frame_title);
      }
      return 1;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    {
      HBRUSH br;

      /* The shader file box is #2D2D2D in dark mode, a shade lighter than the page
         (also while it is grayed, when it is asked for as a static). */
      if ((HWND)l == s_shader_edit && s_shader_edit)
      {
        br = theme_ctlcolor_custom((HDC)w, RGB(0x2D, 0x2D, 0x2D), 2);
        if (br) return (LRESULT)br;
      }

      /* The x inside the file box looks like part of it. */
      if ((HWND)l == s_shader_clear && s_shader_clear)
      {
        br = theme_ctlcolor_custom((HDC)w, RGB(0x2D, 0x2D, 0x2D), 2);
        if (br) return (LRESULT)br;
        SetBkColor((HDC)w, GetSysColor(COLOR_WINDOW));
        SetTextColor((HDC)w, GetSysColor(COLOR_GRAYTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
      }

      br = theme_ctlcolor((HDC)w);
      if (br) return (LRESULT)br;

      /* Light theme: text boxes and drop-down lists keep their normal white. */
      if (m == WM_CTLCOLOREDIT || m == WM_CTLCOLORLISTBOX) return DefWindowProc(h, m, w, l);

      SetBkColor((HDC)w, GetSysColor(COLOR_BTNFACE));
      SetTextColor((HDC)w, GetSysColor(COLOR_BTNTEXT));
      return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }

    case WM_MEASUREITEM:
    {
      MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT *)l;
      if (mi->CtlType == ODT_COMBOBOX) { mi->itemHeight = (UINT)TBS(18); return TRUE; }
      break;
    }

    case WM_DRAWITEM:
    {
      /* The lists: the chosen row in the accent color; the color lists have a small square of each color before its name. */
      DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT *)l;
      int idx = (int)di->CtlID - ROW_ID_BASE;
      if (di->CtlType == ODT_COMBOBOX && di->itemID != (UINT)-1 && idx >= 0 && idx < s_nrows && s_row[idx] &&
          s_row[idx]->id != IDM_SET_ACCENT_BASE && s_row[idx]->id != IDM_SET_BARCOLOR_BASE && s_row[idx]->id != IDM_SET_OSDCOLOR_BASE)
        return theme_combo_draw(di) ? TRUE : FALSE;
      if (di->CtlType == ODT_COMBOBOX && di->itemID != (UINT)-1 && idx >= 0 && idx < s_nrows && s_row[idx])
      {
        int dark = theme_is_dark(), sel = (di->itemState & ODS_SELECTED) && !(di->itemState & ODS_COMBOBOXEDIT);
        int off = (di->itemState & ODS_DISABLED) != 0;
        COLORREF bg = sel ? theme_accent_bg() : (dark ? RGB(0x2D, 0x2D, 0x2D) : GetSysColor(COLOR_WINDOW));
        int colors = s_row[idx]->id == IDM_SET_ACCENT_BASE || s_row[idx]->id == IDM_SET_BARCOLOR_BASE || s_row[idx]->id == IDM_SET_OSDCOLOR_BASE;
        COLORREF fg = off ? GetSysColor(COLOR_GRAYTEXT) : (sel ? (dark ? RGB(255, 255, 255) : GetSysColor(COLOR_HIGHLIGHTTEXT)) : (dark ? RGB(0xE0, 0xE0, 0xE0) : GetSysColor(COLOR_WINDOWTEXT)));
        HBRUSH bb = CreateSolidBrush(bg), sw = CreateSolidBrush(!colors || off ? RGB(0x80, 0x80, 0x80) : (s_row[idx]->id == IDM_SET_ACCENT_BASE ? theme_accent_swatch((int)di->itemID) : (s_row[idx]->id == IDM_SET_BARCOLOR_BASE ? video_bar_color_of((int)di->itemID) : video_osd_color_of((int)di->itemID))));
        HBRUSH edge = CreateSolidBrush(dark ? RGB(0x90, 0x90, 0x90) : RGB(0x60, 0x60, 0x60));
        RECT r = di->rcItem, box;
        char text[64] = "";
        int sh = TBS(10), sw_w = TBS(16);

        if (!(gui.ui_style && (di->itemState & ODS_COMBOBOXEDIT))) FillRect(di->hDC, &r, bb);      /* (Modern: the rounded box behind is already drawn) */
        box.left = r.left + TBS(5);
        box.right = colors ? box.left + sw_w : box.left;
        box.top = r.top + (r.bottom - r.top - sh) / 2;
        box.bottom = box.top + sh;
        if (colors) { FillRect(di->hDC, &box, sw); FrameRect(di->hDC, &box, edge); }
        SendMessageA(di->hwndItem, CB_GETLBTEXT, di->itemID, (LPARAM)text);
        SetBkMode(di->hDC, TRANSPARENT);
        SetTextColor(di->hDC, fg);
        r.left = box.right + TBS(colors ? 6 : 1);
        DrawTextA(di->hDC, text, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        if (di->itemState & ODS_FOCUS && !(di->itemState & ODS_COMBOBOXEDIT)) { RECT fr = di->rcItem; DrawFocusRect(di->hDC, &fr); }
        DeleteObject(bb); DeleteObject(sw); DeleteObject(edge);
        return TRUE;
      }
      break;
    }

    case WM_COMMAND:
    case WM_HSCROLL:
      return SendMessage(GetParent(h), m, w, l);
  }
  return DefWindowProc(h, m, w, l);
}

static int row_span(const SRow *r)
{
  if (r->kind == K_BUTTON && r->id == IDM_VIDEO_SHADERPARAMS) return 0;     /* it sits inside the shader box */
  if (r->kind == K_CHECK && r->id == IDM_VIDEO_SHADERNOFILTER) return 0;    /* and so does this one, beside it */
  return (r->kind == K_SHADER || r->kind == K_BREAK || r->kind == K_GROUP || r->kind == K_GROUPEND) ? 0 : 1;     /* the shader box sits under the columns, outside them */
}

/* One tooltip window serves every control; it hides itself after 30 seconds. */
static HWND s_tip;
static int s_start_tab;          /* which tab the dialog opens on */
#define TIP_AUTOPOP_MS 30000

static void add_tip(HWND dlg, HWND ctl, const char *text, HFONT font)
{
  TOOLINFOA ti;

  if (!ctl || !text) return;
  if (!s_tip)
  {
    s_tip = CreateWindowExA(WS_EX_TOPMOST, TOOLTIPS_CLASSA, NULL, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                            0, 0, 0, 0, dlg, NULL, g_inst, NULL);
    if (!s_tip) return;
    SendMessage(s_tip, TTM_SETMAXTIPWIDTH, 0, TBS(320));
    SendMessage(s_tip, TTM_SETDELAYTIME, TTDT_AUTOPOP, TIP_AUTOPOP_MS);
    SendMessage(s_tip, TTM_SETDELAYTIME, TTDT_INITIAL, 400);
    SendMessage(s_tip, WM_SETFONT, (WPARAM)font, FALSE);
  }
  memset(&ti, 0, sizeof(ti));
  ti.cbSize = sizeof(ti);
  ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
  ti.hwnd = dlg;
  ti.uId = (UINT_PTR)ctl;
  ti.lpszText = (LPSTR)text;
  SendMessageA(s_tip, TTM_ADDTOOLA, 0, (LPARAM)&ti);
}

static void build_page(HWND dlg, int p, const RECT *rc, HFONT font)
{
  const SPage *pg = &pages[p];
  int w = rc->right - rc->left, h = rc->bottom - rc->top;
  int m = TBS(12), rowh = TBS(30), total = 0, cols, per_col;
  int colw, colw0, gw, ytop = m, lwp, i, col = 0, slot = 0, maxh = 0, ypad = 0, gstart = 0, inset = 0, breaks = 0;
  const char *gtitle = NULL;

  for (i = 0; i < pg->count; i++) { total += row_span(&pg->rows[i]); if (pg->rows[i].kind == K_BREAK) breaks++; }
  cols = breaks ? 2 : (total > 8) ? 2 : 1;      /* (a page with its own column break lays the columns out itself) */
  per_col = (total + cols - 1) / cols;
  colw = (w - 2 * m - TBS(16)) / 2;   /* the same width on every tab, one column or two */
  colw0 = colw;
  gw = colw0;

  /* The label column is as wide as the longest label on the page (so none is clipped), but no more than 60%. */
  lwp = (colw0 - TBS(24)) * 2 / 5;
  {
    HDC mdc = GetDC(dlg);
    HGDIOBJ of = SelectObject(mdc, theme_style_font(font));       /* (the labels have the style's font: Modern is wider) */
    for (i = 0; i < pg->count; i++)
    {
      const SRow *lr = &pg->rows[i];
      SIZE sz;
      if ((lr->kind == K_CHOICE || lr->kind == K_DYN || lr->kind == K_FILTER) && lr->label &&
          GetTextExtentPoint32A(mdc, lr->label, lstrlenA(lr->label), &sz) && sz.cx + TBS(14) > lwp)
        lwp = sz.cx + TBS(14);
    }
    SelectObject(mdc, of);
    ReleaseDC(dlg, mdc);
    if (lwp > (colw0 - TBS(24)) * 3 / 5) lwp = (colw0 - TBS(24)) * 3 / 5;
  }

  s_page[p] = CreateWindowExA(WS_EX_CONTROLPARENT, "GPSettingsPage", "",
                              WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | (p == 0 ? WS_VISIBLE : 0),
                              rc->left, rc->top, w, h, dlg, NULL, g_inst, NULL);

  for (i = 0; i < pg->count && s_nrows < MAX_ROWS; i++)
  {
    const SRow *r = &pg->rows[i];
    int span = row_span(r), x, y, idx;
    HWND c = NULL;

    if (r->kind == K_BREAK) { if (col + 1 < cols) { col++; slot = 0; ypad = 0; } continue; }
    if (r->kind == K_GROUP)
    {
      /* A frame around the rows that follow, painted by the page like the shader box's. */
      gstart = ytop + slot * rowh + ypad + TBS(6);
      gtitle = r->label;
      ypad += TBS(22);
      inset = TBS(12);
      gw = colw0;
      colw = gw - 2 * inset;
      continue;
    }
    if (r->kind == K_GROUPEND)
    {
      if (s_grp_n < GRP_MAX)
      {
        int gx = m + col * (colw0 + TBS(16));
        SetRect(&s_grp_rc[s_grp_n], gx, gstart, gx + gw, ytop + slot * rowh + ypad - TBS(2));
        s_grp_title[s_grp_n] = gtitle;
        s_grp_page[s_grp_n++] = s_page[p];
      }
      ypad += TBS(12);
      if (ytop + slot * rowh + ypad > maxh) maxh = ytop + slot * rowh + ypad;
      inset = 0;
      colw = colw0;
      gw = colw0;
      continue;
    }
    idx = s_nrows++;

    if (!breaks && slot + span > per_col && slot > 0 && col + 1 < cols) { col++; slot = 0; }   /* next column */
    x = m + col * (colw0 + TBS(16)) + inset;
    y = ytop + slot * rowh + ypad;
    slot += span;
    if (ytop + slot * rowh + ypad > maxh) maxh = ytop + slot * rowh + ypad;

    s_row[idx] = r;
    s_rowpage[idx] = p;

    if (r->kind == K_CHECK && r->id == IDM_VIDEO_SHADERNOFILTER)
    {
      /* In the shader box, beside the Shader Parameters button. */
      int bx = s_frame_rc.left + TBS(10) + colw * 3 / 4 + TBS(14);
      c = CreateWindowExA(0, "BUTTON", r->label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                          bx, s_frame_rc.top + TBS(78), s_frame_rc.right - TBS(10) - bx, TBS(22), s_page[p],
                          (HMENU)(INT_PTR)(ROW_ID_BASE + idx), g_inst, NULL);
    }
    else if (r->kind == K_CHECK)
    {
      /* Only as wide as the box and its words, so a click beside them does nothing. */
      int cw = colw;
      {
        HDC mdc = GetDC(dlg);
        HGDIOBJ of = SelectObject(mdc, font);
        SIZE sz;
        if (GetTextExtentPoint32A(mdc, r->label, lstrlenA(r->label), &sz)) cw = sz.cx + TBS(26);
        SelectObject(mdc, of);
        ReleaseDC(dlg, mdc);
        if (cw > colw) cw = colw;
      }
      c = CreateWindowExA(0, "BUTTON", r->label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                          x, y, cw, TBS(22), s_page[p], (HMENU)(INT_PTR)(ROW_ID_BASE + idx), g_inst, NULL);
      if (r->id == IDM_EMU_AUTOSAVE)
      {
        /* Beside the tick, the size of the drop-down lists above it. */
        int bw = colw - lwp, bh = TBS(24), j;
        for (j = 0; j < idx; j++)
        {
          RECT cr;
          if (s_row[j] && s_row[j]->kind == K_CHOICE && s_ctl[j] && GetWindowRect(s_ctl[j], &cr))
          {
            bh = cr.bottom - cr.top;
            break;
          }
        }
        int bx = x + lwp;   /* in line with the drop-down lists */
        if (bx < x + cw + TBS(6))    /* the tick's words reach past that: just after them */
        {
          bx = x + cw + TBS(8);
          bw = x + colw - bx;
        }
        s_resume_btn = CreateWindowExA(0, "BUTTON", "Restore Dialog", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                       bx, y, bw, bh, s_page[p], (HMENU)(INT_PTR)RESUME_BTN_ID, g_inst, NULL);
        SendMessage(s_resume_btn, WM_SETFONT, (WPARAM)font, TRUE);
        add_tip(dlg, s_resume_btn, "Bring back the \"Resume where you left off?\" question if you ticked \"Do not ask again\". Grayed while the question is already switched on.", font);
      }
    }
    else if (r->kind == K_BUTTON && r->id == IDM_VIDEO_SHADERPARAMS)
    {
      /* In the shader box, under the file box. */
      c = CreateWindowExA(0, "BUTTON", r->label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                          s_frame_rc.left + TBS(10), s_frame_rc.top + TBS(76), colw * 3 / 4, TBS(24), s_page[p],
                          (HMENU)(INT_PTR)(ROW_ID_BASE + idx), g_inst, NULL);
    }
    else if (r->kind == K_BUTTON)
    {
      c = CreateWindowExA(0, "BUTTON", r->label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                          x, y, colw * 3 / 4, TBS(24), s_page[p], (HMENU)(INT_PTR)(ROW_ID_BASE + idx), g_inst, NULL);
    }
    else if (r->kind == K_SHADER)
    {
      /* Under both columns: a frame with "Enable Shader" and, below it, the
         file box and its ... button. */
      int colw = w - 2 * m;                 /* (the full width, not a column's) */
      int bh = TBS(108), bw = TBS(30);                 /* room for the Shader Parameters button under the file box */

      x = m;
      y = maxh + TBS(4);

      /* The frame is painted by the page itself (see page_proc), so its line
         can be a quiet gray in dark mode; a group box draws it bright white. */
      SetRect(&s_frame_rc, x, y, x + colw, y + bh);
      s_frame_title = "Shaders";
      s_frame_page = s_page[p];

      c = CreateWindowExA(0, "BUTTON", "Enable Shader", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                          x + TBS(10), y + TBS(18), TBS(120), TBS(20), s_page[p], (HMENU)(INT_PTR)(ROW_ID_BASE + idx), g_inst, NULL);
      s_shader_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP | ES_AUTOHSCROLL,
                                      x + TBS(10), y + TBS(44), colw - TBS(20) - bw - TBS(6), TBS(23), s_page[p],
                                      (HMENU)(INT_PTR)(SHADER_EDIT_ID), g_inst, NULL);
      s_shader_clear = CreateWindowExA(0, "STATIC", "x", WS_CHILD | WS_CLIPSIBLINGS | SS_NOTIFY | SS_CENTER | SS_CENTERIMAGE,
                                       x + colw - TBS(10) - bw - TBS(6) - TBS(20) - TBS(4), y + TBS(46), TBS(18), TBS(19), s_page[p],
                                       (HMENU)(INT_PTR)(SHADER_CLEAR_ID), g_inst, NULL);
      s_shader_browse = CreateWindowExA(0, "BUTTON", "...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                        x + colw - TBS(10) - bw, y + TBS(43), bw, TBS(25), s_page[p],
                                        (HMENU)(INT_PTR)(SHADER_BROWSE_ID), g_inst, NULL);
      s_shader_check = c;

      SendMessage(s_shader_edit, WM_SETFONT, (WPARAM)font, TRUE);
      SendMessage(s_shader_browse, WM_SETFONT, (WPARAM)font, TRUE);
      SendMessage(s_shader_clear, WM_SETFONT, (WPARAM)font, TRUE);
      SetWindowPos(s_shader_clear, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
      add_tip(dlg, s_shader_clear, "Clear the shader path (this turns the shader off).", font);
      add_tip(dlg, s_shader_edit, "The shader preset (.slangp) to run.", font);
      add_tip(dlg, s_shader_browse, "Choose a shader preset file.", font);
    }
    else if (r->kind == K_DYN)
    {
      int lw = lwp;
      HWND lab = CreateWindowExA(0, "STATIC", r->label, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOTIFY | SS_CENTERIMAGE,
                                 x, y, lw - TBS(6), TBS(26), s_page[p], NULL, g_inst, NULL);
      SendMessage(lab, WM_SETFONT, (WPARAM)font, TRUE);
      s_lab[idx] = lab;
      add_tip(dlg, lab, r->tip, font);
      c = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                          x + lw, y, colw - lw, TBS(240), s_page[p], (HMENU)(INT_PTR)(ROW_ID_BASE + idx), g_inst, NULL);
      SendMessage(c, WM_SETFONT, (WPARAM)font, TRUE);
      /* the list is filled by sync_all() */
    }
    else   /* K_CHOICE, K_FILTER */
    {
      int lw = lwp, k;
      HWND lab = CreateWindowExA(0, "STATIC", r->label, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOTIFY | SS_CENTERIMAGE,
                                 x, y, lw - TBS(6), TBS(26), s_page[p], NULL, g_inst, NULL);
      SendMessage(lab, WM_SETFONT, (WPARAM)font, TRUE);
      s_lab[idx] = lab;
      add_tip(dlg, lab, r->tip, font);

      c = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL |
                                 CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,       /* (owner-drawn, so that the chosen row takes the accent color) */
                          x + lw, y, colw - lw, TBS(240), s_page[p], (HMENU)(INT_PTR)(ROW_ID_BASE + idx), g_inst, NULL);
      SendMessage(c, WM_SETFONT, (WPARAM)font, TRUE);

      if (r->kind == K_FILTER)
      {
        int n = video_filter_count();
        SendMessageA(c, CB_ADDSTRING, 0, (LPARAM)"None");
        for (k = 0; k < n; k++) SendMessageA(c, CB_ADDSTRING, 0, (LPARAM)video_filter_name(k));
      }
      else
      {
        for (k = 0; k < r->n; k++) SendMessageA(c, CB_ADDSTRING, 0, (LPARAM)r->items[k]);
        if (r->id == IDM_SET_ACCENT_BASE && gui.accent == GUI_ACCENT_HIDDEN) SendMessageA(c, CB_ADDSTRING, 0, (LPARAM)"Chaos Emerald");   /* once found, it stays in the list */
      }
    }

    s_ctl[idx] = c;
    add_tip(dlg, c, r->tip, font);
    if (c && r->kind != K_CHOICE && r->kind != K_FILTER && r->kind != K_DYN) SendMessage(c, WM_SETFONT, (WPARAM)font, TRUE);
  }

  {
    int need = maxh + m;
    if (s_frame_page == s_page[p] && s_frame_rc.bottom + m > need) need = s_frame_rc.bottom + m;
    if (need > s_need_h) s_need_h = need;
  }
}

/****************************************************************************
 * The dialog
 ****************************************************************************/

static void show_page(int p)
{
  int i;
  for (i = 0; i < NPAGES; i++) ShowWindow(s_page[i], i == p ? SW_SHOW : SW_HIDE);
}

/* Puts every setting on these tabs back to its default. The defaults are read
   the way the rows are: set_config_defaults() is run for a moment, the menu
   marks it gives are noted, the real values are put back, and then each
   row that differs is changed with the same command the menu sends -- so
   every change takes effect exactly as if it had been made by hand.
   Controls, the ROM folder, cheats and the window's place are left alone. */
static void restore_defaults(HWND dlg, int page)     /* page: the tab to restore, -1 = all of them */
{
  int def[MAX_ROWS], i, k, theme_before = gui.theme_mode, style_before = gui.ui_style;
  t_gui_config og = gui;
  t_config oc = config;
  uint8 os0 = input.system[0], os1 = input.system[1];

  if (MessageBoxA(dlg, page < 0 ? "Put every setting in this window back to its default?\n\n"
                                  "Your controls, ROM folder and cheats are kept."
                                : "Put every setting on this tab back to its default?\n\n"
                                  "The other tabs are left as they are.",
                  page < 0 ? "Restore Defaults" : "Restore Tab Defaults", MB_YESNO | MB_ICONQUESTION) != IDYES)
    return;

  set_config_defaults();
  gui_update_menu();
  for (i = 0; i < s_nrows; i++)
  {
    const SRow *r = s_row[i];
    def[i] = 0;
    if (page >= 0 && s_rowpage[i] != page) continue;
    if (r->kind == K_CHECK) def[i] = menu_checked(r->id) ? 1 : 0;
    else if (r->kind == K_CHOICE)
      for (k = 0; k < r->n; k++)
        if (menu_checked(r->id + k)) { def[i] = k; break; }
  }

  gui = og;
  config = oc;
  input.system[0] = os0;
  input.system[1] = os1;
  gui_update_menu();

  for (i = 0; i < s_nrows; i++)
  {
    const SRow *r = s_row[i];

    if (page >= 0 && s_rowpage[i] != page) continue;      /* not on this tab */
    if (r->kind == K_CHECK)
    {
      if ((menu_checked(r->id) ? 1 : 0) != def[i]) SendMessage(g_hwnd, WM_COMMAND, MAKEWPARAM(r->id, 0), 0);
    }
    else if (r->kind == K_CHOICE)
    {
      int cur = -1;
      if (r->id >= IDM_SET_UISIZE_BASE && r->id < IDM_SET_UISIZE_BASE + 2) continue;      /* it restarts the program: left as it is */
      for (k = 0; k < r->n; k++)
        if (menu_checked(r->id + k)) { cur = k; break; }
      if (cur != def[i]) SendMessage(g_hwnd, WM_COMMAND, MAKEWPARAM(r->id + def[i], 0), 0);
    }
    else if (r->kind == K_DYN)
    {
      SendMessage(g_hwnd, WM_COMMAND, MAKEWPARAM(r->id, 0), 0);      /* the first entry is the default */
    }
    else if (r->kind == K_FILTER)
    {
      if (video_filter_current() >= 0) SendMessage(g_hwnd, WM_COMMAND, MAKEWPARAM(IDM_VIDEO_FILTER_NONE, 0), 0);
    }
    else if (r->kind == K_SHADER)
    {
      if (gui.shader_preset_path[0]) gui_set_shader(NULL);
    }
  }

  if (gui.theme_mode != theme_before || gui.ui_style != style_before) PostMessage(dlg, WM_APP + 1, 0, 0);
  sync_all();
}

/* An option was changed here: the on-screen message names it and says what it is now, e.g. "Scanlines: 50%". */
static void settings_osd(const char *label, const char *value)
{
  char name[96];
  int n;

  lstrcpynA(name, label ? label : "", sizeof(name));
  n = lstrlenA(name);
  while (n > 0 && (name[n - 1] == ':' || name[n - 1] == ' ')) name[--n] = '\0';
  if (name[0] && gui.osd_opts) gui_notify("%s: %s", name, value);
}

/* The settings are applied as they are changed (so the window shows their effect), and are kept by OK. Cancel (also the X and Esc)
   puts every setting of the rows back as it was when the window opened, the way the rows set them: with the same commands. */
static int  s_snap[MAX_ROWS];
static char s_snap_shader[GUI_PATH_LEN];

static int row_snapshot_value(const SRow *r)
{
  int k;
  if (r->kind == K_CHECK) return menu_checked(r->id) ? 1 : 0;
  if (r->kind == K_CHOICE)
  {
    for (k = 0; k < r->n; k++) if (menu_checked(r->id + k)) return k;
    return -1;
  }
  if (r->kind == K_DYN) { char names[64][96]; int cur = 0; gui_dyn_list(r->id, names, 64, &cur); return cur; }
  if (r->kind == K_FILTER) return video_filter_current();
  return 0;
}

static void snapshot_rows(void)
{
  int i;
  for (i = 0; i < s_nrows; i++) s_snap[i] = row_snapshot_value(s_row[i]);
  lstrcpynA(s_snap_shader, gui.shader_preset_path, sizeof(s_snap_shader));
}

static void cancel_changes(void)
{
  int i;
  for (i = 0; i < s_nrows; i++)
  {
    const SRow *r = s_row[i];
    int now;

    if (r->kind == K_BUTTON || r->kind == K_BREAK || r->kind == K_GROUP || r->kind == K_GROUPEND) continue;
    if (r->kind == K_SHADER)
    {
      if (lstrcmpA(gui.shader_preset_path, s_snap_shader)) gui_set_shader(s_snap_shader[0] ? s_snap_shader : NULL);
      continue;
    }
    if (r->kind == K_CHOICE && r->id >= IDM_SET_UISIZE_BASE && r->id < IDM_SET_UISIZE_BASE + 2) continue;      /* the program has restarted for it already */
    now = row_snapshot_value(r);
    if (now == s_snap[i]) continue;
    if (r->kind == K_CHECK) SendMessage(g_hwnd, WM_COMMAND, MAKEWPARAM(r->id, 0), 0);
    else if (r->kind == K_CHOICE) { if (s_snap[i] >= 0) SendMessage(g_hwnd, WM_COMMAND, MAKEWPARAM(r->id + s_snap[i], 0), 0); }
    else if (r->kind == K_DYN) SendMessage(g_hwnd, WM_COMMAND, MAKEWPARAM(r->id + s_snap[i], 0), 0);
    else if (r->kind == K_FILTER) SendMessage(g_hwnd, WM_COMMAND, MAKEWPARAM(s_snap[i] < 0 ? IDM_VIDEO_FILTER_NONE : IDM_VIDEO_FILTER_BASE + s_snap[i], 0), 0);
  }
}

static int s_refresh_ticks;      /* a shader takes a moment to load: the window looks again a few times, so Shader Parameters wakes up by itself */
#define REFRESH_SOON() do { SetTimer(dlg, 91, 250, NULL); s_refresh_ticks = 0; } while (0)

/* The Settings window written out for the User Guide, from the same table the window is built from: one title for each tab, then a
   row for each option, "name <tab> what it does" (and the choices it offers). */
size_t settings_guide_text(char *out, size_t cap)
{
  size_t len = 0;
  int p, i, k;

#define GUIDE_ADD(str) do { const char *s_ = (str); size_t l_ = strlen(s_); if (out && len + l_ + 1 < cap) memcpy(out + len, s_, l_); len += l_; } while (0)

  GUIDE_ADD("SETTINGS WINDOW\r\n");
  GUIDE_ADD("  Where it is:\r\n");
  GUIDE_ADD("   Options > Settings... or the Settings button on the toolbar: the options of the menus, and some more, on tabs. Hover over an option for the same description. Changes show at once while the window is open, so you can see their effect; OK keeps them and Cancel (also the X and Esc) puts every setting back as it was. Alt and the underlined letter of a tab (shown once Alt is pressed) opens it. Restore Defaults (bottom left) puts the settings of this tab, or of all tabs, back to their defaults, keeping your controls, ROM folder and cheats.\r\n");
  for (p = 0; p < NPAGES; p++)
  {
    const SPage *pg = &pages[p];

    GUIDE_ADD("  ");
    GUIDE_ADD(pg->title);
    GUIDE_ADD(" tab:\r\n");
    for (i = 0; i < pg->count; i++)
    {
      const SRow *r = &pg->rows[i];
      char label[96];
      size_t ll;

      if (!r->label || r->kind == K_BREAK || r->kind == K_GROUPEND) continue;
      lstrcpynA(label, r->label, sizeof(label));
      ll = strlen(label);
      if (ll && label[ll - 1] == ':') label[ll - 1] = '\0';
      if (r->kind == K_GROUP) { GUIDE_ADD("  "); GUIDE_ADD(label); GUIDE_ADD(":\r\n"); continue; }
      GUIDE_ADD("\t");
      GUIDE_ADD(label);
      GUIDE_ADD("\t");
      GUIDE_ADD(r->tip ? r->tip : "");
      if (r->kind == K_CHOICE && r->items && r->n >= 2 && r->n <= 8)
      {
        size_t total = 0;
        for (k = 0; k < r->n; k++) total += strlen(r->items[k]) + 2;
        if (total < 140)
        {
          GUIDE_ADD(" Choices: ");
          for (k = 0; k < r->n; k++) { if (k) GUIDE_ADD(", "); GUIDE_ADD(r->items[k]); }
          GUIDE_ADD(".");
        }
      }
      GUIDE_ADD("\r\n");
    }
  }
#undef GUIDE_ADD
  if (out && cap) out[len < cap ? len : cap - 1] = '\0';
  return len;
}

static INT_PTR CALLBACK settings_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
    case WM_INITDIALOG:
    {
      HWND tab = GetDlgItem(dlg, IDC_SETTINGS_TAB);
      HFONT font = (HFONT)SendMessage(dlg, WM_GETFONT, 0, 0);
      RECT rc, trc;
      int p;
      s_nrows = 0;
      memset(s_lab, 0, sizeof(s_lab));
      s_shader_want = 0;
      s_resume_btn = NULL;
      s_tip = NULL;
      tabs_hot = -1;
      SetRectEmpty(&s_frame_rc);
      s_frame_page = NULL;
      s_frame_title = NULL;
      s_grp_n = 0;

      /* The Large interface: the rows are laid out 17% bigger than in the normal window, so the window is made 30% wider than the normal one or the
         drop-downs are squeezed and the buttons' words cut. */
      if (gui.large_ui)
      {
        RECT cr, tr, ok, cn;
        int want = 670 * 130 / 100, diff;

        GetClientRect(dlg, &cr);
        diff = want - (cr.right - cr.left);
        if (diff > 0)
        {
          RECT wr;
          GetWindowRect(dlg, &wr);
          SetWindowPos(dlg, NULL, wr.left - diff / 2, wr.top, wr.right - wr.left + diff, wr.bottom - wr.top, SWP_NOZORDER | SWP_NOACTIVATE);
          GetWindowRect(tab, &tr);
          MapWindowPoints(NULL, dlg, (POINT *)&tr, 2);
          SetWindowPos(tab, NULL, 0, 0, tr.right - tr.left + diff, tr.bottom - tr.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
          GetWindowRect(GetDlgItem(dlg, IDOK), &ok);
          MapWindowPoints(NULL, dlg, (POINT *)&ok, 2);
          SetWindowPos(GetDlgItem(dlg, IDOK), NULL, ok.left + diff, ok.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
          GetWindowRect(GetDlgItem(dlg, IDCANCEL), &cn);
          MapWindowPoints(NULL, dlg, (POINT *)&cn, 2);
          SetWindowPos(GetDlgItem(dlg, IDCANCEL), NULL, cn.left + diff, cn.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
      }

      /* The tab strip is as tall as its tabs; the pages (each with its own
         frame) fill the rest, down to the Close button. */
      {
        RECT btn;
        s_tabs = tab;
        GetWindowRect(tab, &trc);
        GetWindowRect(GetDlgItem(dlg, IDOK), &btn);
        MapWindowPoints(NULL, dlg, (POINT *)&trc, 2);
        MapWindowPoints(NULL, dlg, (POINT *)&btn, 2);
        MoveWindow(tab, trc.left, trc.top, trc.right - trc.left, tab_h(), TRUE);
        rc.left = trc.left;
        rc.right = trc.right;
        rc.top = trc.top + tab_h();
        rc.bottom = btn.top - TBS(8);
      }

      s_need_h = 0;
      for (p = 0; p < NPAGES; p++) build_page(dlg, p, &rc, font);

      /* A tab that needs more room than the window has makes the window taller (as far as the screen allows). */
      if (s_need_h > rc.bottom - rc.top)
      {
        RECT wr, ok;
        MONITORINFO mi;
        int diff = s_need_h - (rc.bottom - rc.top), maxh;

        GetWindowRect(dlg, &wr);
        mi.cbSize = sizeof(mi);
        maxh = GetMonitorInfo(MonitorFromWindow(g_hwnd ? g_hwnd : dlg, MONITOR_DEFAULTTONEAREST), &mi) ? (mi.rcWork.bottom - mi.rcWork.top) : 2000;
        if ((wr.bottom - wr.top) + diff > maxh) diff = maxh - (wr.bottom - wr.top);
        if (diff > 0)
        {
          SetWindowPos(dlg, NULL, 0, 0, wr.right - wr.left, (wr.bottom - wr.top) + diff, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
          GetWindowRect(GetDlgItem(dlg, IDOK), &ok);
          MapWindowPoints(NULL, dlg, (POINT *)&ok, 2);
          SetWindowPos(GetDlgItem(dlg, IDOK), NULL, ok.left, ok.top + diff, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
          GetWindowRect(GetDlgItem(dlg, IDCANCEL), &ok);
          MapWindowPoints(NULL, dlg, (POINT *)&ok, 2);
          SetWindowPos(GetDlgItem(dlg, IDCANCEL), NULL, ok.left, ok.top + diff, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
          for (p = 0; p < NPAGES; p++)
            if (s_page[p]) SetWindowPos(s_page[p], NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top + diff, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
          rc.bottom += diff;
        }
      }

      /* Restore Defaults sits at the left, level with Close. */
      {
        RECT btn;
        HWND rd;
        GetWindowRect(GetDlgItem(dlg, IDOK), &btn);
        MapWindowPoints(NULL, dlg, (POINT *)&btn, 2);
        int bw = btn.right - btn.left;
        {
          /* as wide as Close, or a little wider if the text would not fit */
          HDC hdc = GetDC(dlg);
          HGDIOBJ of = SelectObject(hdc, font);
          SIZE tsz;
          GetTextExtentPoint32A(hdc, "Restore Defaults", 16, &tsz);
          tsz.cx += TBS(18);                                                  /* room for the arrow */
          SelectObject(hdc, of);
          ReleaseDC(dlg, hdc);
          if (bw < tsz.cx + TBS(16)) bw = tsz.cx + TBS(16);
        }
        rd = CreateWindowExA(0, "BUTTON", "Restore Defaults", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                             trc.left, btn.top, bw, btn.bottom - btn.top, dlg,
                             (HMENU)(INT_PTR)IDC_SETTINGS_DEFAULTS, g_inst, NULL);      /* drawn with an arrow: All Tabs or This Tab */
        SendMessage(rd, WM_SETFONT, (WPARAM)font, TRUE);
        add_tip(dlg, rd, "Choose This Tab or All Tabs to put those settings back to their defaults. Controls, ROM folder and cheats are kept.", font);
      }
      if (s_start_tab) { SetWindowLongPtr(tab, GWLP_USERDATA, s_start_tab); show_page(s_start_tab); }

      theme_apply_to_window(dlg);
      sync_all();
      snapshot_rows();
      return TRUE;
    }

    case WM_DRAWITEM:
    {
      const DRAWITEMSTRUCT *dd = (const DRAWITEMSTRUCT *)lp;
      if (dd && dd->CtlType == ODT_BUTTON && dd->CtlID == (UINT)IDC_SETTINGS_DEFAULTS) { theme_draw_dropdown_button(dd); return TRUE; }
      break;
    }

    case WM_APP + 1:   /* re-theme after the theme was changed here */
    {
      int i;

      theme_apply_to_window(dlg);

      /* Every combo box gets its frame and contents drawn again. */
      for (i = 0; i < s_nrows; i++)
        if (s_ctl[i] && (s_row[i]->kind == K_CHOICE || s_row[i]->kind == K_FILTER))
        {
          SetWindowPos(s_ctl[i], NULL, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
          RedrawWindow(s_ctl[i], NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW);
        }

      RedrawWindow(dlg, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
      return TRUE;
    }

    case WM_CTLCOLORDLG:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORLISTBOX:
    {
      HBRUSH br = theme_ctlcolor((HDC)wp);
      if (br) return (LRESULT)br;
      break;
    }

    case WM_TIMER:
      if (wp == 91)
      {
        sync_all();
        if (++s_refresh_ticks >= 16) KillTimer(dlg, 91);
        return TRUE;
      }
      break;

    case WM_COMMAND:
    {
      int id = LOWORD(wp), code = HIWORD(wp);

      if (id == IDC_SETTINGS_TAB && code == TABS_CHANGED)
      {
        show_page((int)GetWindowLongPtr((HWND)lp, GWLP_USERDATA));
        return TRUE;
      }

      if (id >= ROW_ID_BASE && id < ROW_ID_BASE + s_nrows)
      {
        const SRow *r = s_row[id - ROW_ID_BASE];

        if (r->kind == K_CHECK && code == BN_CLICKED)
        {
          SendMessage(g_hwnd, WM_COMMAND, MAKEWPARAM(r->id, 0), 0);
          sync_all();
          settings_osd(r->label, menu_checked(r->id) ? "On" : "Off");
        }
        else if ((r->kind == K_CHOICE || r->kind == K_FILTER) && code == CBN_SELCHANGE)
        {
          int sel = (int)SendMessage((HWND)lp, CB_GETCURSEL, 0, 0), cmd;

          if (sel >= 0)
          {
            if (r->kind == K_FILTER) cmd = sel == 0 ? IDM_VIDEO_FILTER_NONE : IDM_VIDEO_FILTER_BASE + sel - 1;
            else                     cmd = r->id + sel;

            SendMessage(g_hwnd, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);

            /* The theme changes this window too -- but not from inside the
               drop-down's own notification: re-theming a combo box while it is
               still closing leaves it blank until the mouse passes over it. */
            if (r->id == IDM_VIDEO_THEME_BASE || r->id == IDM_SET_STYLE_BASE) PostMessage(dlg, WM_APP + 1, 0, 0);
            sync_all();
            {
              char val[96];
              if (SendMessageA((HWND)lp, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)val) > 0) settings_osd(r->label, val);
            }
          }
        }
        else if (r->kind == K_DYN && code == CBN_SELCHANGE)
        {
          int sel = (int)SendMessage((HWND)lp, CB_GETCURSEL, 0, 0);
          if (sel >= 0) SendMessage(g_hwnd, WM_COMMAND, MAKEWPARAM(r->id + sel, 0), 0);
          sync_all();
          {
            char val[96];
            if (sel >= 0 && SendMessageA((HWND)lp, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)val) > 0) settings_osd(r->label, val);
          }
        }
        else if (r->kind == K_SHADER && code == BN_CLICKED)
        {
          if (SendMessage(s_shader_check, BM_GETCHECK, 0, 0) == BST_CHECKED && !shader_chain_available())
          {
            char text[MAX_PATH + 400];
            shader_chain_missing_text(text, (int)sizeof(text));
            MessageBoxA(dlg, text, "Enable Shader", MB_OK | MB_ICONINFORMATION);
            s_shader_want = 0;
            sync_shader();
          }
          else if (SendMessage(s_shader_check, BM_GETCHECK, 0, 0) == BST_CHECKED)
          {
            char path[GUI_PATH_LEN];
            GetWindowTextA(s_shader_edit, path, sizeof(path));

            if (path[0]) gui_set_shader(path);
            else s_shader_want = 1;          /* ticked with no file yet: the box and ... are enabled to give it one */
          }
          else
          {
            s_shader_want = 0;
            gui_set_shader(NULL);
          }
          sync_shader();
          sync_all();
          REFRESH_SOON();
        }
        else if (r->kind == K_BUTTON && code == BN_CLICKED)
        {
          /* The other window opens on top of this one, which waits (and
             stays open) until it is closed. */
          EnableWindow(dlg, FALSE);
          SendMessage(g_hwnd, WM_COMMAND, MAKEWPARAM(r->id, 0), 0);
          EnableWindow(dlg, TRUE);
          SetForegroundWindow(dlg);
          sync_all();
        }
        return TRUE;
      }

      if (id == RESUME_BTN_ID && code == BN_CLICKED)
      {
        gui.resume_choice = 0;      /* the Auto Resume question is asked again */
        config_save();
        sync_all();
        return TRUE;
      }

      if (id == SHADER_EDIT_ID && code == EN_KILLFOCUS && SendMessage(s_shader_check, BM_GETCHECK, 0, 0) == BST_CHECKED)
      {
        char path[GUI_PATH_LEN];
        GetWindowTextA(s_shader_edit, path, sizeof(path));
        if (path[0] && lstrcmpA(path, gui.shader_preset_path)) { gui_set_shader(path); sync_shader(); sync_all(); REFRESH_SOON(); }
        return TRUE;
      }

      if (id == SHADER_EDIT_ID && code == EN_CHANGE)
      {
        if (s_shader_clear) ShowWindow(s_shader_clear, (IsWindowEnabled(s_shader_edit) && GetWindowTextLengthA(s_shader_edit) > 0) ? SW_SHOWNA : SW_HIDE);
        return TRUE;
      }

      if (id == SHADER_CLEAR_ID && code == STN_CLICKED)
      {
        /* The path goes and the shader with it; "Enable Shader" stays ticked so a new file can be chosen. */
        gui_set_shader(NULL);
        s_shader_last[0] = '\0';
        s_shader_want = 1;
        SetWindowTextA(s_shader_edit, "");
        sync_shader();
        sync_all();
        REFRESH_SOON();
        SetFocus(s_shader_edit);
        return TRUE;
      }

      if (id == SHADER_BROWSE_ID && code == BN_CLICKED)
      {
        EnableWindow(dlg, FALSE);
        SendMessage(g_hwnd, WM_COMMAND, MAKEWPARAM(IDM_VIDEO_SHADER_PRESET, 0), 0);   /* the file picker, then loads it */
        EnableWindow(dlg, TRUE);
        SetForegroundWindow(dlg);
        sync_shader();
        sync_all();
        REFRESH_SOON();
        return TRUE;
      }

      if (id == IDC_SETTINGS_DEFAULTS && code == BN_CLICKED)
      {
        /* A drop-down: only the tab that is showing, or all the tabs. */
        HMENU m = CreatePopupMenu();
        RECT br;
        int cmd;

        AppendMenuA(m, MF_STRING, 2, "This Tab");
        AppendMenuA(m, MF_STRING, 1, "All Tabs");
        GetWindowRect(GetDlgItem(dlg, IDC_SETTINGS_DEFAULTS), &br);
        cmd = (int)gui_popup(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, br.left, br.bottom);      /* below the button */
        DestroyMenu(m);
        if (cmd == 1) restore_defaults(dlg, -1);
        else if (cmd == 2) restore_defaults(dlg, (int)GetWindowLongPtr(s_tabs, GWLP_USERDATA));
        return TRUE;
      }

      if (id == IDOK || id == IDCANCEL)
      {
        if (id == IDCANCEL) cancel_changes();          /* OK keeps what was changed; Cancel puts it back */
        EndDialog(dlg, id);
        return TRUE;
      }
      return FALSE;
    }

    case WM_CLOSE:
      cancel_changes();                                 /* the X is Cancel */
      EndDialog(dlg, IDCANCEL);
      return TRUE;
  }

  return FALSE;
}

/* For another window that wants the same strip (the controls window). */
int settings_tab_height(void) { return tab_h(); }

void settings_set_tabs(HWND tab, const char *const *titles, int n)
{
  SetPropA(tab, "gp_tabs", (HANDLE)titles);
  SetPropA(tab, "gp_ntabs", (HANDLE)(INT_PTR)n);
}

void settings_register_classes(void)
{
  WNDCLASSA wc;

  /* Both window classes have to exist before a dialog is created from a
     template that names the tab strip. */
  ZeroMemory(&wc, sizeof(wc));
  wc.hInstance = g_inst;
  wc.hCursor   = LoadCursor(NULL, IDC_ARROW);

  wc.lpfnWndProc   = page_proc;
  wc.lpszClassName = "GPSettingsPage";
  RegisterClassA(&wc);

  wc.lpfnWndProc   = tabs_proc;
  wc.lpszClassName = "GPSettingsTabs";
  RegisterClassA(&wc);
}

void dlg_settings_tab(HWND parent, int tab)
{
  s_start_tab = (tab > 0 && tab < NPAGES) ? tab : 0;
  settings_register_classes();
  gui_dialog_box(gui.large_ui ? IDD_SETTINGS_LARGE : IDD_SETTINGS, parent, settings_proc);
  gui_update_menu();
}

void dlg_settings(HWND parent)
{
  dlg_settings_tab(parent, 0);
}
