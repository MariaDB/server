#ifndef __MAPUTIL_H__
#define __MAPUTIL_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  void *memory;
  DWORD lenL;
  DWORD lenH;
  } MEMMAP;

DllExport HANDLE  CreateFileMap(PGLOBAL, LPCSTR, MEMMAP *, MODE, bool);
DllExport bool    CloseMemMap(void *memory, size_t dwSize);

#ifdef __cplusplus
}
#endif

#endif /* __MAPUTIL_H__ */
