#ifdef WIN32
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void * reserve)
{
  return TRUE;
}
#endif
