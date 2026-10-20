/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  eggs.h -- hidden extras, compiled in only when GPEX_EGGS is defined (the
 *  Makefile's EGGS=1, on for test builds; EGGS=0 leaves every call below empty).
 ****************************************************************************/

#ifndef GPEX_EGGS_H
#define GPEX_EGGS_H

#include <windows.h>

#ifdef GPEX_EGGS
void eggs_key(WPARAM vk, LPARAM lp);                 /* every key press of the main window's message loop */
void eggs_startup(void);                  /* once the window is up: the birthday greeting */
void eggs_ring(void);                     /* a click on the grid view's logo: ten of them play the ring sound */
void eggs_about_click(HWND dlg, LPARAM lp);   /* a click on the About icon: ten of them play the SEGA sound */
int  eggs_logo_dx(void);                  /* how far the grid logo is shaken sideways right now */
int  eggs_accent_tap(void);               /* a right click on the Accent Color list: 1 on the seventh */
#else
#define eggs_key(vk, lp)          ((void)0)
#define eggs_startup()            ((void)0)
#define eggs_ring()               ((void)0)
#define eggs_about_click(dlg, lp) ((void)0)
#define eggs_logo_dx()            0
#define eggs_accent_tap()         0
#endif

#endif
