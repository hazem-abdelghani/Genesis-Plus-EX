/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  resource.h -- identifiers shared between gpex.rc and the C sources.
 ****************************************************************************/

#ifndef _RESOURCE_H_
#define _RESOURCE_H_

/* Controls that never need to be addressed by code. */
#define IDC_STATIC                 -1

#define IDI_APPICON                 1
#define IDR_MAINMENU                2
#define IDR_ACCELERATORS            3

/* --- File ------------------------------------------------------------- */
#define IDM_FILE_OPEN             100
#define IDM_FILE_CLOSE            101
#define IDM_FILE_LOADSTATE        102
#define IDM_FILE_SAVESTATE        103
#define IDM_FILE_SCREENSHOT       104
#define IDM_FILE_EXIT             105
#define IDM_FILE_RECENT_CLEAR     106
#define IDM_FILE_BROWSER          107
#define IDM_FILE_ROMINFO          108
#define IDM_FILE_REC_VIDEO        181   /* record video + audio (toggle) */
#define IDM_FILE_REC_AUDIO        182   /* record audio only (toggle)    */
#define IDM_FILE_REC_STOP         183
#define IDM_FILE_REC_GIF          187   /* record an animated GIF (toggle) */
#define IDM_FILE_OPENDIR          184   /* open the program folder in Explorer */
#define IDM_TOOLS_NETPLAY         185   /* host / join a LAN game */
#define IDM_TOOLS_NETPLAY_STOP    186
#define IDM_FILE_RECENT_BASE      110   /* .. 119 */
#define IDM_FILE_SLOT_BASE        120   /* .. 129 */

/* --- Emulation -------------------------------------------------------- */
#define IDM_EMU_PAUSE             200
#define IDM_EMU_STOP              203
#define IDM_EMU_RESET             201
#define IDM_EMU_HARDRESET         202
#define IDM_EMU_REGION_BASE       210   /* auto, US, EU, JP NTSC, JP PAL */
#define IDM_EMU_SYSTEM_BASE       220   /* .. 228 */
#define IDM_EMU_LOCKON_BASE       230   /* .. 233 */
#define IDM_EMU_VDPMODE_BASE      265   /* .. 267: auto, NTSC 60 Hz, PAL 50 Hz (250/251 belong to View) */
#define IDM_EMU_REWINDMEM_BASE    2570  /* .. 2573: rewind history size 128, 256, 512, 768 MB */
#define IDM_EMU_REWIND            259   /* rewind on / off */
#define IDM_EMU_UNDOLOAD          260   /* put back the game as it was before the last state load */
#define IDM_EMU_BIOS              240
#define IDM_EMU_ADDRERROR         241
#define IDM_EMU_CHEATSEARCH       249   /* Tools > Cheat Search... */
#define IDM_EMU_CDLATENCY         244   /* Mega CD disc access delay */
#define IDM_INPUT_INVERTMOUSE     245   /* Mega Mouse vertical axis */
#define IDM_INPUT_GUNCURSOR       246   /* crosshair over the picture for light guns */
#define IDM_EMU_PAUSE_UNFOCUSED   242
#define IDM_EMU_CHEATS            243

#define IDM_VIEW_LIST             250
#define IDM_VIEW_GRID             251
#define IDM_VIEW_COL_BASE         270   /* .. 271: Name (always on), Console */
#define IDM_VIEW_TOOLBAR          274
#define IDM_VIEW_SEARCHBAR        275
#define IDM_VIEW_FILTERBAR        276
#define IDM_VIEW_RESETWINDOW      277
#define IDM_VIEW_FINDSEARCH       278   /* Ctrl+F */
#define IDM_OPTIONS_SETTINGS      279   /* Options > Settings... */
#define IDM_VIEW_COL_FAV          284   /* List Columns > Favorite */
#define IDM_VIEW_CLEANNAMES       2533  /* View > hide (...) and [...] in game names */
#define IDM_VIEW_FSTOOLBAR        287   /* View > Toolbar in Fullscreen */
#define IDM_VIEW_FAVONLY          285   /* View > Show Favorites Only */
#define IDM_OPTIONS_SETTINGS_VIDEO 280  /* toolbar: Settings, on the Video tab */
#define IDM_OPTIONS_SETTINGS_AUDIO 281  /* toolbar: Settings, on the Audio tab */

/* --- Video ------------------------------------------------------------ */
#define IDM_VIDEO_SCALE_BASE      300   /* .. 305 == 1x .. 6x */
#define IDM_VIDEO_FULLSCREEN      310
#define IDM_VIDEO_FULLSCREEN_START 311
#define IDM_VIDEO_ALWAYS_ON_TOP   312
#define IDM_VIDEO_RENDERER_BASE   313   /* .. 315: GDI, Direct3D 9, Direct3D 11 */
#define IDM_VIDEO_ASPECT_BASE     320   /* 4: original console pixels, square, 4:3, 16:9 */
#define IDM_VIDEO_SMOOTH          330
#define IDM_VIDEO_SCANLINE_BASE   331   /* .. 335, 0/25/50/75/100% */
#define IDM_VIDEO_BRIGHTEN        336
#define IDM_VIDEO_NTSC_BASE       340   /* off, composite, s-video, rgb */
#define IDM_VIDEO_OVERSCAN_BASE   350   /* none, vertical, horizontal, all */
#define IDM_VIDEO_SHOWFPS         360
#define IDM_VIDEO_INTERLACE_BASE  440   /* single field, double field */
#define IDM_VIDEO_LCD_BASE        445   /* .. 448: off, light, medium, heavy */
#define IDM_VIDEO_FRAMESKIP_BASE  450   /* .. 455: off, auto, manual 25/33/50/75% */
#define IDM_VIDEO_SMSBORDER       460   /* show / crop the Master System side borders */
#define IDM_VIDEO_SHADER_PRESET   461   /* pick a librashader .slangp preset (D3D11 only) */
#define IDM_VIDEO_SHADER_NONE     462   /* turn the GPU shader off */
#define IDM_TOOLS_SHOT_BASE       465   /* .. 467: screenshot output: final, corrected, raw */
#define IDM_VIDEO_GGEXTRA         362
#define IDM_VIDEO_FILTER_NONE     363
                                        /* 364 .. 369 were the .rpi rescan and
                                           filter-scale items; left unused so
                                           the numbers below do not move */
#define IDM_VIDEO_FILTER_BASE     370   /* .. 393, one per built-in filter */
#define IDM_VIDEO_VSYNC           395
#define IDM_VIDEO_THEME_BASE      396   /* .. 398: Auto/Light/Dark */
#define IDM_VIDEO_LARGE_UI        399

/* --- Audio ------------------------------------------------------------ */
#define IDM_AUDIO_ENABLE          400
#define IDM_AUDIO_SETTINGS        401
#define IDM_AUDIO_MONO            402
#define IDM_AUDIO_LOWPASS         403
#define IDM_AUDIO_HQPSG           404
#define IDM_AUDIO_FMCORE_BASE     410   /* .. 414: Nuked YM2612, Nuked YM3438, MAME YM2612, MAME YM3438 ASIC, MAME YM3438 enhanced */
#define IDM_AUDIO_RATE_BASE       420   /* 22050, 44100, 48000 */

/* --- Input ------------------------------------------------------------ */
#define IDM_INPUT_P1              500
#define IDM_INPUT_PORTA_BASE      510   /* .. 522 (13 choices) */
#define IDM_INPUT_PORTB_BASE      550   /* .. 562 (13 choices) */
#define IDM_INPUT_BACKGROUND      540   /* keep reading the controller while another window has focus */

/* --- Help ------------------------------------------------------------- */
#define IDM_HELP_SHORTCUTS        600
#define IDM_HELP_ABOUT            601
#define IDM_HELP_MENUGUIDE         602

/* --- Accelerator-only commands ---------------------------------------- */
#define IDM_ACCEL_QUICKSAVE       700
#define IDM_ACCEL_QUICKLOAD       701
#define IDM_ACCEL_SLOT_NEXT       702
#define IDM_ACCEL_SLOT_PREV       703
#define IDM_ACCEL_LEAVE_FULLSCREEN 704

#define IDM_BROWSER_PLAY           800
#define IDM_BROWSER_PLAY_STATE_BASE 810   /* .. 819, one per save slot (GUI_SLOT_MAX = 10) */
#define IDM_BROWSER_REFRESH        830
#define IDM_BROWSER_CHANGE_DIR     831
#define IDM_BROWSER_ROMINFO        832
#define IDM_BROWSER_CHEATS         833
#define IDM_BROWSER_SET_COVER      834
#define IDM_BROWSER_REMOVE_COVER   835
#define IDM_BROWSER_OPENDIR        840   /* context menu: Open ROMs Directory */
#define IDM_BROWSER_CLEAR_RECENT   841   /* context menu: Clear Recently Played */
#define IDM_BROWSER_REMOVE_RECENT  848   /* context menu: Remove From Recently Played (this game only) */
#define IDM_BROWSER_SHORTCUT_DESKTOP 842  /* context menu: Create Desktop Shortcut */
#define IDM_BROWSER_FAVORITE       838   /* context menu: add to / remove from favorites */

/* --- Dialogs ---------------------------------------------------------- */
#define IDD_ABOUT                 900
#define IDD_INPUT                 901
#define IDC_INPUT_TAB            1099
#define IDC_INPUT_TEST           1098
#define IDC_INPUT_PORTA_LABEL    1610   /* Controls > Ports tab */
#define IDC_INPUT_PORTA          1611
#define IDC_INPUT_PORTB_LABEL    1612
#define IDC_INPUT_PORTB          1613
#define IDC_INPUT_PORTS_HINT     1614
#define IDR_PADIMG               7001
#define IDR_RING                 7004
#define IDR_RINGBLINK            7005
#define IDR_LOGO                 7006
#define IDR_GENRES               7002
#define IDR_SEGA_SFX             7007
#define IDR_RING_SFX             7008
#define IDD_AUDIO                 902
#define IDD_SHORTCUTS             903
#define IDD_CHEATS                904
#define IDD_ROMINFO               905
#define IDD_MENUGUIDE              906
#define IDD_NETPLAY               920
#define IDD_NETPLAY_LARGE         921
#define IDD_ABOUT_LARGE           910
#define IDD_INPUT_LARGE           911
#define IDD_AUDIO_LARGE           912
#define IDD_SHORTCUTS_LARGE       913
#define IDD_CHEATS_LARGE          914
#define IDD_ROMINFO_LARGE         915
#define IDD_STATEMGR_LARGE        916
#define IDD_MENUGUIDE_LARGE        917
#define IDD_SETTINGS              930
#define IDD_SETTINGS_LARGE        931
#define IDC_SETTINGS_TAB          1400
#define IDC_SETTINGS_DEFAULTS     1401

#define IDC_ABOUT_TEXT           1000
#define IDC_ABOUT_LINK1          1069
#define IDC_ABOUT_ICON           1058

#define IDC_INPUT_AUTOASSIGN     1014
#define IDC_INPUT_DEFAULTS       1013
#define IDC_INPUT_LABEL_BASE     1071   /* .. 1084, one per row */
#define IDC_INPUT_DEVICE         1017
#define IDC_INPUT_PADTYPE        1015
#define IDC_INPUT_HINT           1016
#define IDC_INPUT_DEADZONE       1018
#define IDC_INPUT_DEADZONE_TEXT  1019
#define IDC_INPUT_DEADZONE_LABEL 1085

#define IDC_AUDIO_VOLUME         1020
#define IDC_AUDIO_VOLUME_TEXT    1021
#define IDC_AUDIO_FM             1022
#define IDC_AUDIO_FM_TEXT        1023
#define IDC_AUDIO_PSG            1024
#define IDC_AUDIO_PSG_TEXT       1025
#define IDC_AUDIO_LATENCY        1026
#define IDC_AUDIO_LATENCY_TEXT   1027
#define IDC_AUDIO_CDDA           1028
#define IDC_AUDIO_CDDA_TEXT      1029
#define IDC_AUDIO_DEFAULTS       1030
#define IDC_AUDIO_METER_L        1031
#define IDC_AUDIO_METER_R        1032

#define IDC_SHORTCUTS_TEXT       1040
#define IDC_SHORTCUTS_HINT      1043
#define IDC_MENUGUIDE_TEXT        1068
#define IDC_MENUGUIDE_LIST        1091
#define IDC_ROMINFO_TEXT         1041
#define IDC_ROMINFO_COPY         1044

#define IDC_CHEATS_LIST          1050
#define IDC_CHEATS_CODE          1051
#define IDC_CHEATS_DESC          1052
#define IDC_CHEATS_ADD           1053
#define IDC_CHEATS_REMOVE        1054
#define IDC_CHEATS_CLEAR         1055
#define IDC_CHEATS_HINT          1056
#define IDC_INPUT_CB_FIRST       1620   /* Controls > Advanced tab, combos frame, 1620 - 1639 */
#define IDC_CB_LISTLBL           1621
#define IDC_CB_LIST              1622
#define IDC_CB_BIND              1624   /* click, then press the key or pad button to bind */
#define IDC_CB_UNDO              1626   /* Remove Last */
#define IDC_CB_DEL               1627
#define IDC_CB_G0                1628   /* .. + 11: A B C X Y Z Start Mode Up Down Left Right */
#define IDC_INPUT_CB_LAST        1639
#define IDC_CB_PAUSE             1650   /* Combos tab: the Pause button (its menu: a pause before the next action) */
#define IDC_CB_COL1              1651   /* the list's column titles: # (IDC_CB_LISTLBL), Trigger, Actions */
#define IDC_CB_COL2              1652
#define IDC_CB_HELP              1653   /* "How it works" */
#define IDC_CB_HOLDTIME          1649   /* Combos tab: the Hold button (its menu: how long the action just added stays down) */
#define IDC_CHEATS_FILES         1790   /* drop-down of similar cheat files (only shown when there are some) */
#define IDC_CHEATS_LOADCHT       1057
#define IDC_CHEATS_SAVE          1058
#define IDC_CHEATS_ENABLE        1059

#define IDC_BROWSER_LIST         1060
#define IDC_BROWSER_SEARCH       1065
#define IDC_BROWSER_CHOOSE       1066
#define IDC_BROWSER_BAR          1200   /* container of the toolbar, search bar and filter bar */
#define IDC_TB_REFRESH           1201
#define IDC_TB_PLAY              1202
#define IDC_TB_FAV               1220   /* toolbar: Favorites Only */
#define IDC_BROWSER_SEARCH_LABEL 1203
#define IDC_BROWSER_SEARCH_CLOSE 1204
#define IDC_BROWSER_FILTER_CONSOLE 1205
#define IDC_BROWSER_FILTER_REGION  1206
#define IDC_BROWSER_FILTER_FOLDER  1207
#define IDC_BROWSER_FILTER_RESET   1208
#define IDC_BROWSER_SEARCH_CLEAR   1212   /* the small x inside the search box */
#define IDC_INPUT_TURBORATE        1086   /* Controls: Turbo Rate drop-down */
#define IDC_INPUT_TURBORATE_LABEL  1087
#define IDC_INPUT_TURBO_BASE      1500   /* .. 1511: the Turbo tick beside A B C X Y Z (one id per pad button) */
#define IDC_INPUT_REFRESH          1088   /* Controls: Refresh the device list */

#define IDD_STATEMGR              908
#define IDM_FILE_STATEMGR         109
#define IDC_STATEMGR_SLOT_BASE   1100  /* .. 1109, one BS_BITMAP button per slot */
#define IDC_STATEMGR_LABEL_BASE  1110  /* .. 1119, one static label per slot */

#define IDC_NP_HOST_PORT         1200
#define IDC_NP_DELAY             1201
#define IDC_NP_HOST_CODE         1202
#define IDC_NP_HOST              1203
#define IDC_NP_MYADDR            1204
#define IDC_NP_LIST              1205
#define IDC_NP_FIND              1206
#define IDC_NP_ADDR              1207
#define IDC_NP_PORT              1208
#define IDC_NP_JOIN_CODE         1209
#define IDC_NP_JOIN              1210
#define IDC_NP_STATUS            1211
#define IDC_NP_UPNP              1214

#define IDC_ADV_FILTER           1300
#define IDC_ADV_LP               1301
#define IDC_ADV_LP_TEXT          1302
#define IDC_ADV_LG               1303
#define IDC_ADV_LG_TEXT          1304
#define IDC_ADV_MG               1305
#define IDC_ADV_MG_TEXT          1306
#define IDC_ADV_HG               1307
#define IDC_ADV_HG_TEXT          1308
#define IDC_ADV_LOWF             1309
#define IDC_ADV_HIGHF            1310
#define IDC_ADV_PCM              1314
#define IDC_ADV_PCM_TEXT         1315
#define IDC_ADV_LABEL_LP         1317
#define IDC_ADV_LABEL_LG         1318
#define IDC_ADV_LABEL_MG         1319
#define IDC_ADV_LABEL_HG         1320
#define IDC_ADV_LABEL_LOWF       1321
#define IDC_ADV_LABEL_HIGHF      1322

#endif /* _RESOURCE_H_ */

/* --- later additions ------------------------------------------------------ */
#define IDM_VIDEO_INTSCALE         2502   /* Video > Integer Scaling */
#define IDM_AUDIO_MUTE_FM          2503   /* Audio > Mute FM Sound */
#define IDM_AUDIO_MUTE_PSG         2504   /* Audio > Mute PSG Sound */
#define IDM_EMU_AUTOSAVE           2505   /* Emulation > Auto Resume */
#define IDM_OPTIONS_OSD            2506   /* Options > Show On-Screen Display */
#define IDM_VIEW_COL_EXTRA        2510   /* List Columns > Genre, Region, File Size, File Type (.. 2513) */
#define IDM_VIEW_SORT_RECENT      2514   /* View > Recently Played at the Top */
#define IDM_INPUT_HOTKEYS          2523   /* Input > Hotkeys... */
#define IDD_HOTKEYS               940
#define IDD_HOTKEYS_LARGE         941
#define IDC_HK_LIST                1500
#define IDC_HK_ASSIGN              1501
#define IDC_HK_CLEAR               1502
#define IDC_HK_DEFAULTS            1503
#define IDC_HK_HINT                1504
#define IDM_FILE_LOADSLOT_BASE    2540   /* .. 2549: Load State > Load State from Slot */
#define IDM_FILE_SAVESLOT_BASE    2550   /* .. 2559: Save State > Save State to Slot */
#define IDM_VIEW_GRIDLINES        2561   /* View > Show Grid Lines */
#define IDM_INPUT_TURBO_ALL       2564   /* Gamepad hotkey: turbo on / off for every button */
#define IDM_VIEW_SCANSUB          2562   /* Settings > General > Scan ROM Subfolders (no menu item) */
#define IDM_EMU_UNDOSAVE          2560   /* Save State > Undo Save State */
#define IDC_INPUT_PROFILES        1640   /* Controls: Profile drop-down and its buttons, 1640 - 1645 */
#define IDC_INPUT_PROFLABEL       1641
#define IDC_INPUT_PROFSAVE        1642
#define IDC_INPUT_PROFDELETE      1645
#define IDC_INPUT_PROFLOAD        1646   /* Load: the profile named in the drop-down */
#define IDC_INPUT_PROFNEW         1647   /* New: a new profile from the window, named in the box */
#define IDC_INPUT_PADHK           1097   /* Controls: Gamepad Hotkeys... */
#define IDD_PADHK                  944
#define IDD_PADHK_LARGE            945
#define IDD_CHEATSEARCH            950
#define IDD_CHEATSEARCH_LARGE      951
#define IDC_CS_HELP                1600
#define IDC_CS_MODE                1601
#define IDC_CS_VALUE               1602
#define IDC_CS_NEW                 1603
#define IDC_CS_SEARCH              1604
#define IDC_CS_COUNT               1605
#define IDC_CS_LIST                1606
#define IDC_CS_NAME                1607
#define IDC_CS_SETVAL              1608
#define IDC_CS_ADD                 1609
#define IDC_CS_REGION              1610
#define IDC_CS_SIZE                1611
#define IDC_CHEATS_CONVERT         1743
#define IDC_CC_IN                  1720
#define IDC_CC_OUT                 1721
#define IDC_CC_CONVERT             1722
#define IDC_CC_ADD                 1723
#define IDC_CC_HELP                1724
#define IDD_ROMFOLDERS             966
#define IDD_ROMFOLDERS_LARGE       967
#define IDC_RF_LIST                1750
#define IDC_RF_ADD                 1751
#define IDC_RF_REMOVE              1752
#define IDC_RF_EXLIST              1753
#define IDC_RF_EXADD               1754
#define IDC_RF_EXREMOVE            1755
#define IDC_RF_HELP                1756
#define IDD_SHADERPARAMS           972
#define IDD_SHADERPARAMS_LARGE     973
#define IDC_SP_LIST                1780
#define IDC_SP_SLIDER              1781
#define IDC_SP_VALUE               1782
#define IDC_SP_RESET               1783
#define IDC_SP_RESETALL            1784
#define IDM_VIDEO_SHADERPARAMS     2695
#define IDM_VIDEO_SHADERNOFILTER   2836   /* the Render Filter is skipped while a GPU shader runs */
#define IDD_CHEATCONV              964
#define IDD_CHEATCONV_LARGE        965
#define IDC_PH_LIST               1521
#define IDC_PH_HINT               1522
#define IDC_PH_ASSIGN             1523
#define IDC_PH_CLEAR              1524
#define IDC_PH_DEFAULTS           1525
#define IDC_PH_HOLD               1526
#define IDM_FILE_LOADSTATEFILE     2600   /* Load State > Load State from File... */
#define IDM_FILE_SAVESTATEFILE     2601   /* Save State > Save State to File... */
#define IDM_OPT_CONFIRMEXIT        2602   /* Settings: ask before exiting while a game runs (no menu item) */
#define IDM_OPT_KEEPAWAKE          2603   /* Settings: keep the PC awake while a game runs (no menu item) */
#define IDM_OPT_CRASHREPORT        3671   /* Settings: write genesis-plus-ex-crash.txt / .dmp after a crash (no menu item) */
#define IDM_OPT_COVERCACHE         3676   /* Settings: keep cover thumbnails on disk (no menu item) */
#define IDM_OPT_COVERCACHE_CLEAR   3677   /* Settings: delete them */
#define IDM_OPT_COVERCARD          3675   /* grid covers: a frame around the cover and its name */
#define IDM_SET_COVERSIZE_BASE     3680   /* 7: grid cover size, 48 to 256 pixels (View > View Mode > Grid Size) */
#define IDM_SET_TBSIZE_BASE        3690   /* 3: toolbar icon size (small, normal, large) */
#define IDM_OPT_TBLABELS           3693   /* View > Toolbars > Show Toolbar Labels: names under the toolbar icons */
#define IDM_SET_ACCENT_BASE        3700   /* 8: accent colour of the selection in the program's own lists and buttons */
#define IDM_OPT_SINGLEINSTANCE     3670   /* Settings: only one copy of the program at a time (no menu item) */
#define IDM_OPT_MUTEUNFOCUSED      2604   /* Settings: mute while another window has focus (no menu item) */
#define IDM_VIEW_DENSITY_BASE      2605   /* .. 2607: compact, normal, roomy list rows */
#define IDM_EMU_INSERTDISC         2608   /* Emulation > Insert Disc... (Mega CD) */
#define IDM_EMU_SPEEDUP            2611   /* Emulation > Game Speed > Faster */
#define IDM_EMU_SPEEDDOWN          2612   /* Emulation > Game Speed > Slower */
#define IDM_EMU_SPEEDRESET         2613   /* Emulation > Game Speed > Normal */
/* Settings-window-only choices: each group is a base id + one per item. */
#define IDM_TOOLS_VGM              2694
#define IDM_TOOLS_MOVIE_REC        2691
#define IDM_TOOLS_MOVIE_PLAY       2692
#define IDM_TOOLS_MOVIE_STOP       2693
#define IDM_INPUT_COMBOS           2680
#define IDM_VIDEO_FLICKER          2670
#define IDM_VIDEO_DITHER           2671
#define IDM_VIDEO_BEZEL            2673   /* pick a bezel image */
#define IDM_VIDEO_BEZEL_OFF        2674
#define IDM_OPT_RUNINDIALOGS       2666   /* keep the game running while Settings and other windows are open */
#define IDM_FILE_APPLYPATCH        2664   /* File > Apply Patch... */
#define IDM_INPUT_PAUSEMENU        2665   /* in-game pause menu */
#define IDM_VIEW_GROUPCONSOLE      2631   /* View > Group by Console */
#define IDM_VIEW_AZSTRIP           2632   /* View > Show A-Z Strip */
#define IDM_FILE_ROMFOLDERS        2633   /* File > ROM Folders... */
#define IDM_SET_COVERSHAPE_BASE    2840   /* 3: grid cover frame shape */
#define IDM_VIEW_STATUSBAR         2829   /* View > Toolbars > Show Status Bar */
#define IDM_OPT_COVERCAP           2828   /* grid covers: show the game name under each cover */
#define IDM_OPT_COVERROUND         2827   /* grid covers: rounded corners */
#define IDM_SET_DEVICE_BASE        2900   /* up to 40: sound device (0 = the Windows default) */
#define IDM_SET_FSRES_BASE         2940   /* up to 60: fullscreen resolution (0 = the desktop's) */
#define IDM_SET_STEREO_BASE        2700   /* 4: stereo width */
#define IDM_SET_MOUSESENS_BASE     2720   /* 5: mouse / light gun speed */
#define IDM_SET_GUNX_BASE          2730   /* 7: light gun aim offset X */
#define IDM_SET_GUNY_BASE          2740   /* 7: light gun aim offset Y */
#define IDM_SET_CONTRAST_BASE      2750   /* 7 */
#define IDM_SET_SATUR_BASE         2760   /* 7 */
#define IDM_SET_GAMMA_BASE         2770   /* 7 */
#define IDM_SET_HUE_BASE           2780   /* 7 */
#define IDM_SET_BARCOLOR_BASE      2790   /* 7: colour of the bars around the picture */
#define IDM_SET_FSMODE_BASE        2810   /* 2: borderless window / exclusive */
#define IDM_OPT_CONFINE            2820   /* confine the mouse to the picture */
#define IDM_AUDIO_DEVICE           2821   /* pick the sound output device */
#define IDM_VIDEO_ADJ_RESET        2826   /* picture adjustments back to normal */
#define IDM_EMU_REWINDNOW          2597   /* Emulation > Rewind (the key, Backspace, is held; the menu item latches it) */
#define IDM_EMU_FASTFWD            2598   /* Emulation > Fast Forward (Tab) */

/* new settings (no menu items) */
#define IDM_SET_CURSORIDLE_BASE    3500   /* 5: hide the mouse pointer after: never, 2, 3, 5, 10 s */
#define IDM_SET_FRAMECAP_BASE      3510   /* 5: limit how often the picture is redrawn: off, 30, 40, 50, 60 */
#define IDM_SET_BASS_BASE          3520   /* 4: bass boost: off, light, medium, strong */
#define IDM_OPT_LOG                3530   /* write genesis-plus-ex.log */
#define IDM_OPT_PAUSEPAD           3531   /* pause when Player 1's pad disconnects */
#define IDM_OPT_NORMALIZE          3533   /* even out loud and quiet games */
#define IDM_SET_AUTOSAVE_BASE      3540   /* 5: save a state every: off, 1, 2, 5, 10 minutes */
#define IDM_SET_SKIPLEN_BASE       3600   /* 7: Skip Intro When a Game Starts: off, 5, 10, 15, 20, 25, 30 s */
#define IDM_FILE_LOADBACKUP_BASE   3560   /* 3: Load State > Load Backup State (the rolling backups) */
#define IDM_OPT_OVERDRIVE          3556   /* Audio: push the sound into a soft limiter */
#define IDM_SET_OSDPOS_BASE        3570   /* 4: where messages appear: top left, top right, bottom left, bottom right */
#define IDM_SET_OSDTIME_BASE       3591   /* 7: how long a message stays: 1 - 7 s */
#define IDM_SET_OSDSIZE_BASE       3580   /* 4: on-screen text size */
#define IDM_SET_FPSPOS_BASE        3585   /* 4: where the frame rate appears */
#define IDM_SET_MODEL_BASE         2860   /* Audio menu > Console Model: 3: Default, Genesis Model 1 (duller), Model 2 (cleaner) */
#define IDM_SET_BACKEND_BASE       3610   /* 4: sound output: Windows Classic (waveOut), XAudio2, WASAPI shared, WASAPI exclusive */
#define IDM_SET_OSDCOLOR_BASE      3620   /* 8: colour of on-screen text */
#define IDM_SET_OSDLINES_BASE      3630   /* 7: how many messages show at once, 1 - 7 */
#define IDM_AUDIO_EQ               3655   /* 3-Band EQ on / off (Settings only; the bands are in Levels and Latency) */
#define IDM_SET_STYLE_BASE         3662   /* 2: look of the windows: Classic, Modern */
#define IDM_SET_UISIZE_BASE        3660   /* 2: interface size (Settings only): Normal, Large; Large is what View > Larger UI was */
#define IDM_AUDIO_HQFM             3640   /* High-Quality FM Synthesis (Settings only) */
#define IDM_SET_FMUNIT_BASE        3645   /* 3: Master System FM unit: auto, off, on */
#define IDM_SET_OPLL_BASE          3650   /* 2: Master System FM chip: Nuked, MAME */
#define IDM_OPT_OSDOPTS            3590   /* messages for changed options */

