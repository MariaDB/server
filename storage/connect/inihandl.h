#ifndef __INIHANDL_H__
#define __INIHANDL_H__

#if defined(UNIX) || defined(UNIV_LINUX)

#ifdef __cplusplus
extern "C" {
#endif

void PROFILE_Close(LPCSTR filename);
void PROFILE_End(void);

int GetPrivateProfileString(
  LPCTSTR lpAppName,        // section name
  LPCTSTR lpKeyName,        // key name
  LPCTSTR lpDefault,        // default string
  LPTSTR lpReturnedString,  // destination buffer
  DWORD nSize,              // size of destination buffer
  LPCTSTR lpFileName        // initialization file name
  );

uint GetPrivateProfileInt(
  LPCTSTR lpAppName,        // section name
  LPCTSTR lpKeyName,        // key name
  INT nDefault,             // return value if key name not found
  LPCTSTR lpFileName        // initialization file name
  );

BOOL WritePrivateProfileString(
  LPCTSTR lpAppName,        // section name
  LPCTSTR lpKeyName,        // key name
  LPCTSTR lpString,         // string to add
  LPCTSTR lpFileName        // initialization file
  );

int GetPrivateProfileSection(
  LPCTSTR lpAppName,        // section name
  LPTSTR lpReturnedString,  // return buffer
  DWORD nSize,              // size of return buffer
  LPCTSTR lpFileName        // initialization file name
  );

BOOL WritePrivateProfileSection(
  LPCTSTR lpAppName,        // section name
  LPCTSTR lpString,         // data
  LPCTSTR lpFileName        // file name
  );

#ifdef __cplusplus
}
#endif

#endif /* defined(UNIX) */

#endif /* __INIHANDL_H__ */
