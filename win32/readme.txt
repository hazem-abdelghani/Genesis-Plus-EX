Genesis Plus EX v3.1.0
===================

Genesis Plus EX is a Windows emulator based on Genesis Plus GX (32X games run in PicoDrive).
Sega Mega Drive / Genesis, Master System, Game Gear, SG-1000, Mega CD
Pico and 32X emulator.

  Genesis Plus EX:     https://github.com/hazem-abdelghani/genesis-plus-ex

Genesis Plus EX front end by Hazem Abdelghani (2026).
Core by Charles MacDonald (1998-2003) and Eke-Eke (2007-2026).
PicoDrive (32X) by Dave, notaz, irixxxx and others (2004-2026).


WHICH FILE TO RUN
-----------------
  Genesis Plus EX.exe          64-bit Windows (use this one if you are not sure).
  Genesis Plus EX-32bit.exe    32-bit Windows. It has no Shaders option, because
                            librashader has no 32-bit build.

No installation is needed. Both only use DLLs that ship with Windows.
Keep the Plugins folder next to Genesis Plus EX.exe: librashader.dll in it gives you
shaders, picodrive_libretro-64.dll / -32.dll (the 32X emulator) lets you play
32X games. (The files also work next to the exe itself.)


GETTING STARTED
---------------
  1. Run the exe.
  2. Open a game: File > Open ROM (Ctrl+O), drag a ROM onto the window, or
     pick your games folder with File > ROM Directory (Ctrl+R) and
     double-click a game in the list.
  3. .zip, .gz and .7z archives open directly.

Everything is stored next to the exe, so the whole folder can be copied
to a USB stick and keep working. (If the program's folder cannot be written
to, for example in Program Files, it keeps them in %LOCALAPPDATA%\Genesis Plus EX
instead.) On first run these are created:

  genesis-plus-ex.ini  settings
  ROMs/             a place for your games (add it with ROM Directories; any
                    other folder works just as well)
  BIOS/             optional BIOS and add-on ROMs
  Saves/            battery saves and Mega CD backup RAM, a folder for each
                    game inside its console's folder (MegaCD\cart.brm is the
                    Mega CD's 512 KB RAM cartridge: extra save space)
  States/           save states, laid out the same way
  Screenshots/      PNG screenshots, a folder for each console
  Cheats/           per-game cheat lists, a folder for each console
  Images/           pictures: Covers (cover art, a folder for each console,
                    matched to each game by file name), Bezels (bezel images) and
                    Pico-Storyware (the pages of Sega Pico books). Move the
                    Covers and Bezels folders of an older version into it
  Recordings/       Video (.mp4), Audio (.wav), GIF, VGM and Input (.gpm)
                    recordings, a folder for each kind and in it a folder
                    for each console
  Profiles/         saved controller profiles
  Languages/        translations: Options > Language lists the Name.po files in it
                    (copy Template.pot to Name.po, fill in msgstr; Poedit,
                https://poedit.net, does it with a window; see README.md)
  Shaders/          Shader presets (64-bit only)
  Logs/             genesis-plus-ex.log, crash report and dump
  Plugins/          librashader.dll and the 32X library (see above)


THE GAME LIST
-------------
Above the list (and above the game, in a window) is the toolbar:

  Open ROM, Refresh, Favorites | Play, Pause, Stop, Reset, Fullscreen,
  Screenshot | Settings, Video, Audio, Controls

Play needs a game picked in the list; Pause, Stop, Reset, Fullscreen and
Screenshot act on the running game. The toolbar goes away in fullscreen
(View > Toolbar in Fullscreen slides it in at the top edge).

  Search bar    Narrows the list as you type. Ctrl+F shows or hides it.
  Filter bar    Narrow the list by console, genre and region (read from tags
                such as (USA) in the file name). Reset clears them;
                what you picked is remembered for the next run.
  Columns       Name, Console, Favorite (a star: click it), Genre, Region,
                File Size and File Type (the last four hidden at first; for a game
                in an archive the type shows what is in it, such as MD (ZIP)).
                Right-click the column header or use View > List Columns.
                Click a title to sort.
  View mode     View > View Mode: list or grid with cover art. The grid
                size runs from Tiny to Extra Large (Ctrl + mouse wheel);
                shape, corners, captions and card frame are in Settings.
  Search words  region:usa  console:mega cd  genre:action  type:chd  fav:yes
  Folders       Settings > General > ROM Directories adds several folders
                and excludes some from the scan.
  A-Z strip     Jump to a letter; Group by Console groups the list.
  Favorites     Click a star (or right-click > Add to Favorites). The star
                on the toolbar, or View > Show Favorites Only, shows only them.
  Recent        View > Show Recently Played puts the last few games you
                opened in a group at the top of the list (the filter bar does
                not hide them there). Right-click a game > Clear Recently
                Played empties it.
  Names         View > Clean Game Names hides the (...) and [...] tags and
                the extension in the list and in File > Recent Files.
  Lines         Thin lines separate the columns; View > Show Grid Lines
                adds lines between the rows.
  Scanning      The ROM directory and the folders inside it are searched
                (Scan Subfolders in the ROM Directories window). Archives with
                nothing playable in them, and discs of other systems (Saturn,
                PlayStation...), are left out.
  Right-click   On a game: play it (or with a saved state), favorites, open,
                refresh or change the ROM directory, ROM information, cheats
                and cover image.
  Covers        Right-click a game > Change Cover Image. Covers are
                cached so they appear at once the next time.

The toolbar, search bar and filter bar can each be switched off under View.


PLAYING
-------
  Save states   Emulation > Save State / Load State (to the selected slot or
                to any slot 0-9: keys 0-9 load, Shift+0-9 save), Select State
                Slot, Undo Save State, Undo Load State and Manage States
                (thumbnails of every slot).
  Auto Resume   Emulation > Auto Resume saves the game when you close it
                and offers to carry on next time. The question has a "Do not
                ask again" box; Settings has a Restore Dialog button.
  Game Speed    Ctrl++ / Ctrl+- steps the speed from 25 % to 400 %; the
                sound follows (lower when slower).
  Fast Forward  Hold Tab, or tick Emulation > Fast Forward to keep it on.
  Rewind        Enable it in Settings > Options, then hold Backspace (or
                choose Emulation > Rewind). The history is 128-768 MB and
                Rewind Speed is Slow, Normal, Fast or Very Fast.
  Taskbar       A progress bar shows on the taskbar while rewinding, fast
                forwarding or recording.
  Backups       Backup State saves quietly every 1-10 minutes into three
                rolling backups (Load State > Load Backup State).
  Sound         Audio > Mute (Ctrl+M), Mute FM (Ctrl+N), Mute PSG (Ctrl+B).
                Output: waveOut, XAudio2, WASAPI shared (default) or
                exclusive. Also Stereo Width, Bass Boost, Overdrive,
                Console Model and Levels and Latency (with a live Level meter).
  Looks         View > Theme: System, Light, Dark, or a colored dark theme
                (Sapphire, Ruby, Emerald, Amethyst,
                Amber, Aquamarine, Diamond); it tints the dark theme and sets the
                default accent color (choosing a theme resets Accent Color).
                Style (Classic or Modern: flat rounded controls and a themed
                title bar), Accent Color (Settings > General), and Colorful Icons
                (Settings > General: menu icons, toolbar and favorite star in
                color; off by default).
  Languages     Options > Language lists the Name.po files of the Languages
                folder (only English ships; Template.pot is the template,
                and Poedit, https://poedit.net, is a good editor for it).
  Picture       Video > Visual Effects (Smooth Scaling, Integer Scaling,
                Brighten, LCD Ghosting), Video > Picture Adjustments (Brightness,
                Contrast, Saturation, Gamma, Hue), Scanlines, NTSC Filter, Aspect Ratio, Rotation
                (turns the picture by 90, 180 or 270 degrees). New render filters:
                MMPX and MMPX Enhanced. File > Run Mega CD BIOS starts the BIOS with
                no disc. Controls > Player 1 / Player 2: Allow Opposing Directions.
                Video > Visual Effects > Deinterlacing draws both fields of an
                interlaced game every frame (no comb teeth).
  ROM info      File > ROM Information shows the game, its header, controllers,
                save RAM and CRC32 (also for a game picked in the list). Copy,
                or right-click a row, puts it on the clipboard.


CHEATS, RECORDING AND NETPLAY
-----------------------------
  Cheats        Tools > Cheats (Ctrl+C): Game Genie, Action Replay and raw
                patch codes and work RAM bytes (aaaaaa:dd), saved per game.
                Click Code, Description or Status to sort the list. Status
                says Active, Off, Cheats are off, Game not running or Code
                not valid for this system. ROM codes wait for the first button press (twenty seconds at most) after
                a game starts or is reset. In netplay both PCs need the same
                cheats on. Convert shows a code in another
                format. Cheat Search finds where a value (lives, health)
                is kept. Tools > Fix Checksum saves a copy of a Mega Drive game with
                the checksum of its header made right. Tools > Debugger > Memory shows the game's memory in hex
                (click a byte and type to change it: any memory, also VRAM, CRAM and the ROM). VDP Viewer, Sound Viewer and CPU Viewer
                show the video chip (registers, palette, tiles, sprites,
                planes), the sound chips and the processors, also for a 32X game (with the plugin of
                the same version). A .cht file named after a game's CRC32 in
                Cheats/library is used by any game without its own list.
                RAM Watch keeps a list of addresses and their values (Gens .wch
                files), Memory Viewer has Copy / Paste / Dump on its right-click
                menu, and the VDP Viewer has Dump / Load buttons for the palette
                and VRAM, an Edit Register window, a Sprite Layer and Preview, a
                plane view with tile details on hover, and H / V Scroll plots.
                RAM Search narrows down RAM addresses by how their values change
                (previous value, a value, an address, the number of changes) and sends
                them to RAM Watch.
                All the debug tools open in one window, the Debugger: the CPU
                Debugger on top, and tabs under it for Memory, VDP, Sound, CPU,
                RAM Search and RAM Watch (drag the bar between them to resize).
                CPU Debugger stops the 68000 (breakpoints on execute, read and
                write; step into, over and out; disassembly, registers, calls);
                in a 32X game also the Master and Slave SH-2 (execute
                breakpoints). RAM Search covers a 32X game's SDRAM too.
                The debug tools are a port of gpgx_debugger by Vladimir
                Kononovich (lab313ru, https://github.com/lab313ru).
  Recording     Tools > Record: Video (.mp4), Audio (.wav), GIF and VGM,
                each in its own folder under Recordings (and in it the
                console's folder). They stop in the background. Settings > Options >
                Video Recording: size, quality and shape of a video. Record
                Input saves every button press to a .gpm file and Play Input
                Recording replays it.
  Screenshots   Settings > Options > Screenshot Output: Corrected
                (the default, 4:3 at 640x480), Raw (the console's own
                size) or Final (Corrected plus the render filter). Screenshot Size
                makes it 1x to 4x that size.
  Netplay       Tools > Netplay: two players on a LAN. Find hosts, session
                code, optional UPnP (the "Open the port on my router (UPnP)"
                tick in the Host box); a differing setting names itself. Both
                games reset and are checked right after the reset, and a lost
                connection is taken back by itself for up to a minute.

32X GAMES
---------
Sega 32X games (.32x, or a .bin / .zip / .7z with the "SEGA 32X" header) run on
PicoDrive, which is loaded from picodrive_libretro-64.dll or -32.dll (in the Plugins
folder next to the program, 64-bit with the 64-bit program, 32-bit with the 32-bit one) only when a 32X game is
opened. Picture, sound, controls, states and recording are the program's own, so
filters, shaders, screenshots, volume and the rest work as usual. Controls are
six-button pads on both ports. Saves and states go in their own 32X folder.
Cheats and Cheat Search work on the Mega Drive half of a 32X game (cartridge ROM and the
68000 work RAM; not the memory of the two SH-2 processors), and so does Rewind (its history
holds fewer seconds than for a Mega Drive game). Input recording, Region, Low-Pass Filter,
Mute FM, Mute PSG and LCD Ghosting work too, and so do VGM recording (FM, PSG and 32X PWM;
the PWM part plays in libvgm-based players only) and netplay (both PCs start the game again
from power-on and compare a fingerprint of the machine; it needs the same game file and program
version). FM Chip (the YM3438 kinds play as a YM3438, the others as a YM2612), Console Model, High-Quality FM, the
3-Band EQ, the Sample Rate and the FM and PSG levels of Levels and Latency work too (its CD audio and PCM levels are grayed out). Not available for 32X games: the
Master System FM unit and chip, High-Quality PSG, borders, and controllers other than the six-button
pad (the port and controller model drop-downs in Controls too). They are grayed out in the menus and in Settings while a 32X game runs.
The NTSC Filter works for 32X games as the program's own version of the look (the picture keeps its size).

MD+ AND MSU-MD GAMES (ENHANCED SOUND)
------------------------------------
MD+ and MSU-MD games (for the Mega Drive, and for the 32X) play their music from
audio files: the ROM, a .cue sheet with the same name and the tracks the sheet
names (.wav, .ogg, or one .bin) go in the same folder. Open the ROM, not the .cue.
MSU-MD games need the Mega CD BIOS for the game's region in the BIOS folder. A
game can be kept in a .zip or .7z (unpacked whole to a temporary folder while
it plays). If the sheet names a file that is missing, the program says which.

SETTINGS
--------
Options > Settings (or the gear on the toolbar) holds the options of the menus,
and some more, on six tabs. Hover over an option for a description. A change
shows at once while the window is open; OK keeps the changes, and Cancel (also
the X and Esc) puts every setting back as it was. Restore Defaults (bottom left)
puts back the defaults of this tab or of all tabs (your controls, ROM folder
and cheats are kept). Windows with their own settings (Levels and Latency,
Picture Adjustments, Controls...) open from a button, and Settings stays open
underneath.

  General     View: Language, View Mode (list or grid), Style, Theme, Accent
                Color, Colorful Icons, Interface Size, Toolbar Icon Size.
              Covers: Cover Size, Cover Frame, Rounded Cover Corners, Show
                Name Under Covers, Frame Around Cover and Name, Console Badge
                on Covers, Keep a Cover Cache, Clear Cover Cache.
              Program Behavior: Pause When Inactive, Mute When Inactive,
                Keep Only One Instance, Always on Top, Keep the PC Awake While
                Playing, Confirm Exit While Playing.
              Directories: Data Folders... (moves each folder beside the
                program: BIOS, Cheats, Images, Languages, Profiles, Recordings,
                Saves, Screenshots, Shaders, States) and
                ROM Directories... (the folders of the game list, and Scan
                Subfolders).

  Video       Window and Screen: Window Size (1x to 10x), Fullscreen Mode
                (borderless or exclusive), Fullscreen Resolution, Start ROM in
                Fullscreen.
              Scaling and Shape: Render Filter, Integer Scaling, Aspect
                Ratio, Rotation, Borders, Interlaced Mode.
              Bezel and Border: Bezel Image, Remove Bezel, Border Color.
              Filters and Effects: NTSC Filter, LCD Ghosting, Scanlines,
                Smooth Scaling, Deinterlacing, Dither Smoothing, Flicker
                Blending, Brighten, Picture Adjustments (Brightness, Contrast,
                Saturation, Gamma, Hue).
              Speed and Smoothness: Renderer (GDI, Direct3D 9 or 11), VSync,
                Limit Redraws, Frameskip, Shader (a .slangp preset, 64-bit
                only), Shader Parameters, Skip Render Filter with Shaders.

  Audio       Mute: All Sound, FM, PSG and PWM (the 32X).
              Tone: Stereo Width, Bass Boost, Low-Pass Filter, 3-Band EQ,
                Mono Output, Overdrive, Even Out Loudness, Levels and Latency.
              Sound chips: Mega Drive FM Chip and High-Quality FM Synthesis;
                Master System FM Unit and FM Chip.
              Sound Quality: High-Quality PSG Resampling, Sample Rate
                (22050, 44100 or 48000 Hz).
              Output: Audio Backend (waveOut, XAudio2, WASAPI shared or
                exclusive), Sound Device.

  Input       Controllers: Controls..., Hotkeys..., Pause When the Pad
                Disconnects, Enable Background Input.
              Mouse: Mouse Speed, Hide Mouse Pointer, Invert Mouse Y Axis,
                Confine Mouse to the Picture.
              Light Gun: Gun Aim Offset X and Y, Show Light Gun Crosshair.

  Options     Console: Region, Force VDP Mode, Console, Lock-On Cartridge,
                Boot from BIOS When Available, Show Extended Game Gear Screen,
                Show Master System Side Borders.
              Accuracy: Emulate Address Error Exceptions, Emulate Mega CD
                Disc Latency.
              Video Recording: Recording Size, Quality and Shape.
              Screenshots: Screenshot Output (Corrected, Raw or Final) and
                Screenshot Size.
              Rewind and Resume: Enable Rewind, Rewind Speed, Rewind History
                Size, Auto Resume (with a Restore Dialog button).
              Start and Backup: Backup State (a rolling state backup,
                restored with Load State > Load Backup State), Skip Intro on
                Launch.
              Troubleshooting: Enable Log, Enable Crash Report.

  OSD         Messages: Show Messages, Messages for Changed Options, Message
                Position, Messages Shown, Message Time.
              Text: Font Color, Font Size.
              Frame Rate: Show Frame Rate, Frame Rate Position.

The Video tab's shader switch needs the 64-bit program and librashader.dll
(see WHICH FILE TO RUN).


CONTROLS (PLAYER 1 DEFAULTS)
----------------------------
  Arrow keys        D-pad
  Z, X, C           A, B, C
  A, S, D           X, Y, Z
  Enter             Start
  Right Shift       Mode

A connected gamepad works with no setup. Change everything under
Input > Controls: click a button, then press the key or pad button you want
(the right stick can be assigned too; right-click A, B, C, X, Y or Z to turn
Turbo on). The window tells you when a key or button was already used.

Controls window: tabs Player 1, Player 2 and Advanced. One press works for
keyboard and gamepad, Esc or right click cancels, Auto Assign goes through
all buttons, and Turbo Rate and Deadzone are beside them. The Advanced tab
chooses what is in each port (Control Pad, Mouse, Light Phaser, Menacer,
Justifier, XE-1AP, Activator, Paddle Control, Sports Pad, Graphic Board,
Team Player, EA 4-Way Play, Master Tap, Smash Controller).

Combo buttons (Advanced tab, up to 12): one key or pad button presses
several game buttons. Pick a row, choose the Trigger, then click the game
buttons to add. Hold keeps a button down for 0.25-10 s (or until you press
it) and Pause waits before the next one, e.g. Gamepad Y -> Down (Hold
1.0 s) + A is a Sonic spin dash. How It Works in the window has an example.

Profiles (Load / New / Save / Delete) keep both players, pad types and
combos in the Profiles folder.

Controls > Gamepad Hotkeys sets pad buttons for fast forward, rewind,
save / load state and previous / next slot (the right stick by
default). Every keyboard shortcut can be changed under Input > Hotkeys; a
hotkey already in use asks whether to swap the two.


KEYBOARD SHORTCUTS
------------------
  F1                      Keyboard shortcuts
  Ctrl+O                  Open a ROM
  Ctrl+R                  ROM Directory
  Ctrl+W                  Close the ROM
  F2 / Pause              Pause and resume
  F3                      Stop the ROM
  F4 / Ctrl+F4            Reset / hard reset
  F5 / F8                 Save / load the current slot
  0 - 9                   Load state from that slot
  Shift+0 - Shift+9       Save state to that slot
  F6 / F7                 Previous / next slot
  Ctrl+F8 / Ctrl+F5       Undo the last state load / save
  Tab (hold)              Fast forward
  Ctrl++ / Ctrl+- / Ctrl+0  Game speed faster / slower / normal (25-400 %)
  Backspace (hold)        Rewind (enable it in Settings > Options)
  \  or  Tab             Advance one frame while paused
  PgDn / PgUp             Sega Pico: previous / next page of the book (the Storyware
                          window shows its picture: Images\Pico-Storyware\<game>\<game>_1.png ...)
  Ctrl+F                  Show / hide the search bar
  Alt+Enter / Esc         Toggle fullscreen
  Ctrl+M / Ctrl+N / Ctrl+B  Mute / mute FM / mute PSG
  Ctrl+C                  Cheats
  Shift+F11               Record video + audio (start / stop)
  Shift+F10               Record a VGM file (start; Alt+F11 stops)
  Ctrl+F11                Record audio only (start / stop)
  Ctrl+F12                Record a GIF (up to 6 s)
  Alt+F11                 Stop recording
  F12                     Save a screenshot

In fullscreen, move the mouse to the top edge of the screen to show the
menu bar. The cursor hides itself after a few idle seconds.


LAUNCHING FROM A FRONTEND
-------------------------
LaunchBox, Pegasus, Playnite, ES-DE, Steam ROM Manager and similar launchers
start the program with the game as its argument:

  "Genesis Plus EX.exe" [options] "<rom>"

  -f, --fullscreen      start the game in fullscreen
  -w, --windowed        start the game in a window
  -x, --exit-on-close   quit when the game is closed (Stop, Close ROM), so
                        the frontend gets control back
  -s N, --state N       load save state slot N (0-9) once the game has started
  --state-file FILE     load a state file once the game has started
  -m, --mute            start with the sound muted
  -p, --paused          start the game paused
  --scale N             window size, 1 to 10 times the picture
  --config FILE         use another settings file
  --profile NAME        use a controls profile (Profiles folder)
  -h, --help            show this list

Example:
  "Genesis Plus EX.exe" --fullscreen --exit-on-close "C:\Roms\Sonic.zip"

The fullscreen options apply to that launch only. For Mega CD games give the
.cue (or .chd) file of the first disc; for MD+ and MSU-MD games give the ROM. More frontend setups are in README.md.


BIOS FILES (optional)
---------------------
Put these in the BIOS folder. Only the Mega CD BIOS files are required,
and only for Mega CD games (the one matching the game's region).

  bios_MD.bin        Mega Drive TMSS start-up ROM (optional)
  bios_CD_U.bin      Mega CD BIOS, USA / Sega CD (required for USA Mega CD games)
  bios_CD_E.bin      Mega CD BIOS, Europe (required for European Mega CD games)
  bios_CD_J.bin      Mega CD BIOS, Japan (required for Japanese Mega CD games)
  bios_U.sms         Master System BIOS, USA (optional)
  bios_E.sms         Master System BIOS, Europe (optional)
  bios_J.sms         Master System BIOS, Japan (optional)
  bios.gg            Game Gear BIOS (optional)
  sk.bin             Sonic & Knuckles ROM, for lock-on (optional)
  sk2chip.bin        Sonic & Knuckles UPMEM ROM, for lock-on (optional)
  areplay.bin        Action Replay ROM, for lock-on (optional)
  ggenie.bin         Game Genie ROM, for lock-on (optional)
  32X_G_BIOS.BIN     32X Genesis-side BIOS: not needed, not used
  32X_M_BIOS.BIN     32X Master SH-2 BIOS: not needed, not used
  32X_S_BIOS.BIN     32X Slave SH-2 BIOS: not needed, not used
                     (32X games start without these three)

Turn the BIOS on under Settings > Options > Boot from BIOS When Available.
BIOS files are not included.
A Mega CD game without its BIOS does not start: the message names the file it
needs (by the game's region) and the BIOS folder it belongs in.


MORE HELP
---------
  Help > User Guide           Every menu item and window explained
  Help > Keyboard Shortcuts   The list of hotkeys
  README.md                   Full documentation, in the project repository


LICENSE
-------
Same terms as Genesis Plus GX: source must accompany modified
redistributions, and it may not be sold or used commercially.
See LICENSE.txt in the repository root.
Other code used: Paprium code (BillyTimeGames), the debug tools (ported from gpgx_debugger by Vladimir Kononovich, lab313ru), PicoDrive (Dave, notaz, irixxxx and others), xBRZ (Zenju), xBR / HQx
(Hyllian and others), 7-Zip LZMA SDK (Igor Pavlov), stb_image (Sean Barrett),
librashader (chyyran). Details are in README.md.
