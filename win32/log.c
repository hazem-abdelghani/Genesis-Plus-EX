/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  log.c -- the optional genesis-plus-ex.log (Settings > Options > Enable Log)
 *
 *  Each line is appended and the file closed again at once, so what happened
 *  right before a crash is still in it. Off by default; the file is started
 *  fresh once it grows past 512 KB.
 ****************************************************************************/

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include "shared.h"
#include "gui.h"

static char log_path[MAX_PATH];
static volatile LONG log_lock;            /* one line at a time, from whichever thread */

/* Called from more than one thread (the controller thread logs slow answers): the path is put together in a local
   copy and published whole, so another thread never sees it half built. */
static const char *log_file(void)
{
  if (!log_path[0])
  {
    char tmp[MAX_PATH], *slash;
    DWORD n = GetModuleFileNameA(NULL, tmp, sizeof(tmp) - 16);
    if (n == 0 || n >= sizeof(tmp) - 16) lstrcpyA(tmp, "Logs\\genesis-plus-ex.log");
    else
    {
      slash = strrchr(tmp, '\\');
      if (slash) slash[1] = 0;
      lstrcatA(tmp, "Logs");
      CreateDirectoryA(tmp, NULL);                    /* the log, the crash report and the dump live in a folder of their own */
      lstrcatA(tmp, "\\genesis-plus-ex.log");
    }
    lstrcpyA(log_path, tmp);
  }
  return log_path;
}

void gui_log(const char *fmt, ...)
{
  char line[1100], text[1024];
  SYSTEMTIME t;
  va_list ap;
  FILE *fp;
  long size;

  if (!gui.log_on) return;

  va_start(ap, fmt);
  wvsprintfA(text, fmt, ap);
  va_end(ap);

  GetLocalTime(&t);
  wsprintfA(line, "%04d-%02d-%02d %02d:%02d:%02d  %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, text);

  {
    int spins = 0, locked = 1;

    while (InterlockedCompareExchange(&log_lock, 1, 0))
      if (++spins > 2000) { locked = 0; break; }          /* the holder is stuck (a crash in the middle of a line): write anyway */
      else Sleep(0);

    fp = fopen(log_file(), "ab");
    if (fp)
    {
      fseek(fp, 0, SEEK_END);
      size = ftell(fp);
      if (size > 512 * 1024)
      {
        fclose(fp);
        fp = fopen(log_file(), "wb");
      }
      if (fp)
      {
        fputs(line, fp);
        fclose(fp);
      }
    }
    if (locked) InterlockedExchange(&log_lock, 0);
  }
}
