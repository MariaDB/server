INCLUDE (CheckIncludeFiles)

CHECK_SYMBOL_EXISTS (getmntent "mntent.h" HAVE_GETMNTENT)
CHECK_SYMBOL_EXISTS (getmntent "sys/mnttab.h" HAVE_GETMNTENT_IN_SYS_MNTAB)
CHECK_SYMBOL_EXISTS (setmntent "mntent.h" HAVE_SETMNTENT)
CHECK_SYMBOL_EXISTS (getmntinfo "sys/types.h;sys/mount.h" HAVE_GETMNTINFO)
CHECK_SYMBOL_EXISTS (getmntinfo64 "sys/types.h;sys/mount.h" HAVE_GETMNTINFO64)

IF (HAVE_GETMNTINFO)
CHECK_CXX_SOURCE_COMPILES("
#include <sys/types.h>
#include <sys/statvfs.h>
int main()
{
  struct statvfs *s;
  return getmntinfo(&s, ST_WAIT);
}
  " HAVE_GETMNTINFO_TAKES_statvfs)
ENDIF()
IF (HAVE_GETMNTENT OR HAVE_GETMNTENT_IN_SYS_MNTAB OR
    HAVE_GETMNTINFO OR HAVE_GETMNTINFO64)
  INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/sql)
  MYSQL_ADD_PLUGIN(DISKS information_schema_disks.cc MODULE_ONLY RECOMPILE_FOR_EMBEDDED)
ENDIF()
