/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  sevenzip.c -- see sevenzip.h.
 *
 *  Reuses the LzmaDec/CpuArch/Alloc-interface objects already built for CHD
 *  support (core/cd_hw/libchdr/deps/lzma-24.05, same SDK version) rather
 *  than vendoring a second copy -- linking both would redefine the same
 *  LzmaDec_ and CpuArch_ symbols twice. See Makefile.win32 and lzmasdk/README.
 ****************************************************************************/

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "shared.h"
#include "lzmasdk/7z.h"
#include "lzmasdk/7zBuf.h"
#include "lzmasdk/7zCrc.h"
#include "lzmasdk/7zFile.h"
#include "sevenzip.h"

struct sevenzip_s
{
  CFileInStream archiveStream;
  CLookToRead2 lookStream;
  CSzArEx db;

  /* SzArEx_Extract's solid-block cache: reusing these across calls avoids
     re-decompressing the same solid block once per file extracted from it. */
  UInt32 blockIndex;
  Byte *outBuffer;
  size_t outBufferSize;
};

#define LOOKAHEAD_BUF_SIZE ((size_t)1 << 18)

static int crc_table_ready;

/* A plain malloc/free ISzAlloc -- the SDK's own g_Alloc (Alloc.c) adds large-page
   and aligned-allocation paths this read-only use has no need for. */
static void *sz_alloc(ISzAllocPtr p, size_t size) { (void)p; return size ? malloc(size) : NULL; }
static void sz_free(ISzAllocPtr p, void *addr) { (void)p; free(addr); }
static const ISzAlloc s_alloc = { sz_alloc, sz_free };

sevenzip_t *sevenzip_open(const char *path)
{
  sevenzip_t *sz;

  if (!crc_table_ready) { CrcGenerateTable(); crc_table_ready = 1; }

  sz = (sevenzip_t *)calloc(1, sizeof(*sz));
  if (!sz) return NULL;

  sz->blockIndex = (UInt32)-1;

  if (InFile_Open(&sz->archiveStream.file, path) != 0)
  {
    free(sz);
    return NULL;
  }

  FileInStream_CreateVTable(&sz->archiveStream);
  sz->archiveStream.wres = 0;

  LookToRead2_CreateVTable(&sz->lookStream, False);
  sz->lookStream.buf = (Byte *)ISzAlloc_Alloc(&s_alloc, LOOKAHEAD_BUF_SIZE);
  if (!sz->lookStream.buf)
  {
    File_Close(&sz->archiveStream.file);
    free(sz);
    return NULL;
  }
  sz->lookStream.bufSize = LOOKAHEAD_BUF_SIZE;
  sz->lookStream.realStream = &sz->archiveStream.vt;
  LookToRead2_INIT(&sz->lookStream)

  SzArEx_Init(&sz->db);
  if (SzArEx_Open(&sz->db, &sz->lookStream.vt, &s_alloc, &s_alloc) != SZ_OK)
  {
    SzArEx_Free(&sz->db, &s_alloc);
    ISzAlloc_Free(&s_alloc, sz->lookStream.buf);
    File_Close(&sz->archiveStream.file);
    free(sz);
    return NULL;
  }

  return sz;
}

int sevenzip_count(sevenzip_t *sz)
{
  return (int)sz->db.NumFiles;
}

int sevenzip_entry_is_dir(sevenzip_t *sz, int i)
{
  return SzArEx_IsDir(&sz->db, (UInt32)i) ? 1 : 0;
}

void sevenzip_entry_name(sevenzip_t *sz, int i, char *name_out, int name_len)
{
  size_t need;
  UInt16 *wide;

  if (name_len <= 0) return;
  name_out[0] = '\0';

  /* dest == NULL: returns the required size (in UTF-16 units, including the
     terminator) rather than writing anything -- the name can be longer than
     any fixed stack buffer, so it's sized on the heap for this one call. */
  need = SzArEx_GetFileNameUtf16(&sz->db, (size_t)i, NULL);
  if (need == 0) return;

  wide = (UInt16 *)malloc(need * sizeof(UInt16));
  if (!wide) return;

  SzArEx_GetFileNameUtf16(&sz->db, (size_t)i, wide);
  if (!WideCharToMultiByte(CP_ACP, 0, (LPCWSTR)wide, -1, name_out, name_len, NULL, NULL))
  {
    name_out[0] = '\0';
  }
  name_out[name_len - 1] = '\0';
  free(wide);
}

unsigned int sevenzip_entry_size(sevenzip_t *sz, int i)
{
  return (unsigned int)SzArEx_GetFileSize(&sz->db, (UInt32)i);
}

int sevenzip_extract(sevenzip_t *sz, int i, unsigned char *buffer, int maxsize)
{
  size_t offset = 0, outSizeProcessed = 0;
  int copy;

  if (SzArEx_Extract(&sz->db, &sz->lookStream.vt, (UInt32)i,
                      &sz->blockIndex, &sz->outBuffer, &sz->outBufferSize,
                      &offset, &outSizeProcessed,
                      &s_alloc, &s_alloc) != SZ_OK)
  {
    return -1;
  }

  copy = (int)outSizeProcessed;
  if (copy > maxsize) copy = maxsize;
  if (copy > 0) memcpy(buffer, sz->outBuffer + offset, (size_t)copy);
  return copy;
}

void sevenzip_close(sevenzip_t *sz)
{
  if (!sz) return;

  if (sz->outBuffer) ISzAlloc_Free(&s_alloc, sz->outBuffer);
  SzArEx_Free(&sz->db, &s_alloc);
  ISzAlloc_Free(&s_alloc, sz->lookStream.buf);
  File_Close(&sz->archiveStream.file);
  free(sz);
}
