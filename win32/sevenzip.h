/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  sevenzip.h -- reading .7z archives (browsing and ROM loading).
 *
 *  Thin wrapper over the vendored LZMA SDK (lzmasdk/) that mirrors the shape
 *  of minizip's unzOpen/unzGoToFirstFile/... calls used elsewhere in this
 *  port for .zip, so browser.c's classification code and main.c's ROM
 *  loading can treat .7z the same way. Read-only: this never creates or
 *  modifies a .7z file.
 ****************************************************************************/

#ifndef _SEVENZIP_H_
#define _SEVENZIP_H_

typedef struct sevenzip_s sevenzip_t;

/* Opens path for reading. NULL if it can't be opened or isn't a valid,
   supported .7z archive (encrypted, split into multiple volumes, or using
   a compression method this build doesn't include -- see lzmasdk/README). */
sevenzip_t *sevenzip_open(const char *path);

/* Number of entries (files and directories) the archive lists. */
int sevenzip_count(sevenzip_t *sz);

/* True if entry i is a directory (no data to extract). */
int sevenzip_entry_is_dir(sevenzip_t *sz, int i);

/* Entry i's path within the archive, converted from the format's UTF-16 to
   this app's ANSI codepage (as WideCharToMultiByte(CP_ACP, ...) renders it --
   matches every other file name this app displays). Always NUL-terminated. */
void sevenzip_entry_name(sevenzip_t *sz, int i, char *name_out, int name_len);

/* Entry i's uncompressed size (from the archive's own header -- free to call
   before extracting). */
unsigned int sevenzip_entry_size(sevenzip_t *sz, int i);

/* Decompresses entry i into buffer, up to maxsize bytes (extra bytes past
   maxsize are decoded internally but not copied out, same as minizip's
   unzReadCurrentFile capped by the caller). Returns the byte count written,
   or -1 on error. Repeated calls reuse the SDK's own solid-block cache, so
   extracting several entries from the same solid archive is still cheap. */
int sevenzip_extract(sevenzip_t *sz, int i, unsigned char *buffer, int maxsize);

void sevenzip_close(sevenzip_t *sz);

#endif
