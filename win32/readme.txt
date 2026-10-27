Genesis Plus EX v3.0.0
===================

Genesis Plus EX is a Windows emulator based on Genesis Plus GX (32X games run in PicoDrive).
Sega Mega Drive / Genesis, Master System, Game Gear, SG-1000, Mega CD
Pico and 32X emulator.

  Genesis Plus EX:     https://github.com/hazem-abdelghani/genesis-plus-ex
  Genesis Plus GX:  https://github.com/ekeeke/Genesis-Plus-GX

Core by Charles MacDonald (1998-2003) and Eke-Eke (2007-2026).
Genesis Plus EX front end by Hazem Abdelghani (2026).


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
to a USB stick and keep working. On first run these are created:

  genesis-plus-ex.ini  settings
  BIOS/             optional BIOS and add-on ROMs
  Saves/            battery saves and Mega CD backup RAM, a folder for each
                    game inside its console's folder (MegaCD\cart.brm is the
                    Mega CD's 512 KB RAM cartridge: extra save space)
  States/           save states, laid out the same way
  Screenshots/      PNG screenshots, a folder for each console
  Cheats/           per-game cheat lists, a folder for each console
  Covers/           cover art, a folder for each console, matched to each
                    game by file name
  Recordings/       Video (.mp4), Audio (.wav), GIF, VGM and Input (.gpm)
                    recordings, a folder for each kind and in it a folder
                    for each console
  Profiles/         saved controller profiles
  Bezels/           bezel images
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
                such as (USA) in the file name). Reset Filters clears them;
                what you picked is remembered for the next run.
  Columns       Name, Console, Favorite (a star: click it), Genre, Region,
                File Size and File Type (the last four hidden at first). Right-
                click the column header, View > List Columns, or Settings >
                Library. Click a title to sort.
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
                opened in a group at the top of the list. Right-click a
                game > Clear Recently Played empties it.
  Names         View > Clean Game Names hides the (...) and [...] tags and
                the extension in the list and in File > Recent Files.
  Lines         Thin lines separate the columns; View > Show Grid Lines
                adds lines between the rows.
  Scanning      The ROM directory and the folders inside it are searched
                (Settings > General > Scan ROM Subfolders). Archives with
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
  Backups       Backup State saves quietly every 1-10 minutes into three
                rolling backups (Load State > Load Backup State).
  Sound         Audio > Mute (Ctrl+M), Mute FM (Ctrl+N), Mute PSG (Ctrl+B).
                Output: waveOut, XAudio2, WASAPI shared (default) or
                exclusive. Also Stereo Width, Bass Boost, Overdrive,
                Console Model and Levels and Latency.
  Looks         View > Theme (System, Light, Dark), Style (Classic or
                Modern: flat rounded controls and a themed title bar) and
                Accent Color, and Colorful Icons (Settings > General:
                menu icons, toolbar and favorite star in color; off by default).
  Picture       Video > Visual Effects (Smooth Scaling, Integer Scaling,
                Brighten, LCD Ghosting), Scanlines, NTSC Filter, Aspect Ratio.
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
                is kept. A .cht file named after a game's CRC32 in
                Cheats/library is used by any game without its own list.
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
                code, optional UPnP; a differing setting names itself.

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
the PWM part plays in libvgm-based players only, unless Settings > Options > 32X: Also Record PWM as DAC is on) and netplay (both PCs start the game again
from power-on and compare a fingerprint of the machine; it needs the same game file and program
version). FM Chip (the YM3438 kinds play as a YM3438, the others as a YM2612), Console Model, High-Quality FM, the
3-Band EQ, the Sample Rate and the FM and PSG levels of Levels and Latency work too (its CD audio and PCM levels are grayed out). Not available for 32X games: the
Master System FM unit and chip, High-Quality PSG, borders, and controllers other than the six-button
pad (the port and controller model drop-downs in Controls too). They are grayed out in the menus and in Settings while a 32X game runs.
The NTSC Filter works for 32X games as the program's own version of the look (the picture keeps its size).

SETTINGS
--------
Options > Settings (or the gear on the toolbar) holds every option of the
menus on tabs: General, View, Video, Picture, OSD, Audio, Input and Options. A change takes
effect at once, exactly as if you had used the menu. Windows with their own
settings (Levels and Latency, Advanced Audio, Controls...) open from a
button and Settings stays open underneath.

The Video tab also has the shader switch (64-bit only): tick Enable Shader,
pick a .slangp preset with the ... button.

New in Settings: Real Console Pixel Shape (Aspect Ratio), Limit Redraws and Hide Mouse Pointer (Video); Bass Boost, Even Out
Loudness (Audio); Pause When the Pad Disconnects (Input); Backup State
(a rolling state backup, restored with Load State > Load Backup State), Skip
Intro on Launch and Write a Log File (Options). Changing a hotkey to one that is
already used asks whether to swap the two.

Also: a taskbar progress bar while rewinding, fast forwarding or recording, a
live Level meter in Audio > Levels and Latency. Sound now follows the game speed, and typing a
letter in the game list jumps to that game, and a round up-arrow button
returns to the first row once the list is scrolled down.

Moved: Port A / B devices are on the Advanced tab of Input > Controls; Input
Recording and VGM logging are in Tools > Record; the rarely used options below
Console (Lock-On, BIOS, address errors, Game Gear / Master System screens) and
Screenshot Output are in Settings only. Slow Motion is gone (use Game Speed).
There is a new OSD tab in Settings (messages, their position, time and font
size, the frame rate).

New: Overdrive and a 22050 Hz sample rate (Audio), Restore This Tab beside
Restore Defaults in Settings, on-screen messages for changed options, question
boxes centered over the program window, and Reset Window Size centers the
window.

Netplay: a start check right after the reset, automatic return after a lost
connection (up to a minute), and an "Open the port on my router (UPnP)" tick
in the Host box (off until you tick it).


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
default). Every keyboard shortcut can be changed under Input > Hotkeys.


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
  \                       Advance one frame while paused
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
  --scale N             window size, 1 to 6 times the picture
  -h, --help            show this list

Example:
  "Genesis Plus EX.exe" --fullscreen --exit-on-close "C:\Roms\Sonic.zip"

The fullscreen options apply to that launch only. For Mega CD games give the
.cue (or .chd) file of the first disc. More frontend setups are in README.md.


BIOS FILES (optional)
---------------------
Put these in the BIOS folder. Only the Mega CD BIOS files are required,
and only for Mega CD games (the one matching the game's region).

  bios_MD.bin                           Mega Drive TMSS boot ROM
  bios_CD_U.bin / _E.bin / _J.bin       Mega CD BIOS (USA / Europe / Japan)
  bios_U.sms / bios_E.sms / bios_J.sms  Master System
  bios.gg                               Game Gear
  ggenie.bin, areplay.bin               Game Genie, Action Replay
  sk.bin, sk2chip.bin                   Sonic & Knuckles lock-on
  32X_G_BIOS.BIN, 32X_M_BIOS.BIN,       32X BIOS (Genesis, Master SH-2, Slave SH-2): not
  32X_S_BIOS.BIN                        needed and not used, 32X games start without them

Turn the BIOS on under Settings > Options > Boot from BIOS When Available.
BIOS files are not included.


MORE HELP
---------
  Help > User Guide           Every menu explained
  Help > Keyboard Shortcuts   The list of hotkeys
  README.md                   Full documentation, in the project repository


LICENSE
-------
Same terms as Genesis Plus GX: source must accompany modified
redistributions, and it may not be sold or used commercially.
See LICENSE.txt in the repository root.
Other libraries: PicoDrive (Dave, notaz, irixxxx and others), xBRZ (Zenju), xBR / HQx
(Hyllian and others), 7-Zip LZMA SDK (Igor Pavlov), stb_image (Sean Barrett),
librashader (chyyran). Details are in README.md.
