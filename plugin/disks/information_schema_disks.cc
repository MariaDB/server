/*
   Copyright (c) 2017, 2019, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

#include <my_global.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#if defined(HAVE_GETMNTENT)
#include <mntent.h>
#elif defined(HAVE_GETMNTINFO) && !defined(HAVE_GETMNTINFO_TAKES_statvfs)
/* getmntinfo (the not NetBSD variants) */
#include <sys/param.h>
#include <sys/ucred.h>
#include <sys/mount.h>
#endif
#if defined(HAVE_GETMNTENT_IN_SYS_MNTAB)
#include <sys/mnttab.h>
#define HAVE_GETMNTENT
#if defined(HAVE_SYS_MNTENT_H)
#include <sys/mntent.h>
#endif
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#include <sql_class.h>
#include <sql_i_s.h>
#include <sql_acl.h>                            /* check_global_access() */

/*
  This intends to support *BSD's, macOS, Solaris, AIX, HP-UX, and Linux.

  specifically:
  FreeBSD/OpenBSD/DragonFly/macOS (statfs) NetBSD (statvfs) uses getmntinfo().
  Linux can use getmntent_r(), but we've just used getmntent for simplification.
  Linux/Solaris/AIX/HP-UX uses setmntent()/getmntent().
  Solaris uses getmntent() with a diffent prototype, return structure, and
    no setmntent(fopen instead)
*/
#if defined(HAVE_GETMNTINFO_TAKES_statvfs) || defined(HAVE_GETMNTENT)
typedef struct statvfs st_info;
#else // GETMNTINFO
typedef struct statfs st_info;
#endif
#ifndef MOUNTED
/* HPUX - https://docstore.mik.ua/manuals/hp-ux/en/B2355-60130/getmntent.3X.html */
#define MOUNTED MNT_MNTTAB
#endif

bool schema_table_store_record(THD *thd, TABLE *table);


struct st_mysql_information_schema disks_table_info = { MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION };


namespace Show {

ST_FIELD_INFO disks_table_fields[]=
{
  Column("Disk",      Varchar(PATH_MAX), NOT_NULL),
  Column("Path",      Varchar(PATH_MAX), NOT_NULL),
  Column("Total",     SLonglong(32),     NOT_NULL), // Total amount available
  Column("Used",      SLonglong(32),     NOT_NULL), // Amount of space used
  Column("Available", SLonglong(32),     NOT_NULL), // Amount available to users other than root.
  CEnd()
};


static int disks_table_add_row_stat(
                                    THD* pThd,
                                    TABLE* pTable,
                                    const char* zDisk,
                                    const char* zPath,
                                    const st_info &info)
{
    // From: http://pubs.opengroup.org/onlinepubs/009695399/basedefs/sys/statvfs.h.html
    // and same for statfs:
    // From: https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/statfs.2.html#//apple_ref/doc/man/2/statfs
    // and: https://www.freebsd.org/cgi/man.cgi?query=statfs&sektion=2&apropos=0&manpath=FreeBSD+13.1-RELEASE+and+Ports
    //
    // f_bsize    Fundamental file system block size.
    // f_blocks   Total number of blocks on file system in units of f_frsize.
    // f_bfree    Total number of free blocks.
    // f_bavail   Number of free blocks available to non-privileged process.
    ulong block_size= (ulong) info.f_bsize;

    ulonglong total = ((ulonglong) block_size * info.f_blocks) / 1024;
    ulonglong used  = ((ulonglong) block_size *
                            (info.f_blocks - info.f_bfree)) / 1024;
    ulonglong avail = ((ulonglong) block_size * info.f_bavail) / 1024;

    /* skip filesystems that don't have any space */
    if (!info.f_blocks)
        return 0;

    /* skip RO mounted filesystems */
#if defined(HAVE_GETMNTINFO_TAKES_statvfs) || defined(HAVE_GETMNTENT)
    if (info.f_flag & ST_RDONLY)
#else
    if (info.f_flags & MNT_RDONLY)
#endif
       return 0;

    pTable->field[0]->store(zDisk, strlen(zDisk), system_charset_info);
    pTable->field[1]->store(zPath, strlen(zPath), system_charset_info);
    pTable->field[2]->store(total);
    pTable->field[3]->store(used);
    pTable->field[4]->store(avail);

    // 0 means success.
    return (schema_table_store_record(pThd, pTable) != 0) ? 1 : 0;
}


#ifdef HAVE_GETMNTENT
static int disks_table_add_row(THD* pThd, TABLE* pTable, const char* zDisk, const char* zPath)
{
    int rv = 0;

    st_info info;

    if (statvfs(zPath, &info) == 0) // We ignore failures.
    {
        rv = disks_table_add_row_stat(pThd, pTable, zDisk, zPath, info);
    }

    return rv;
}
#endif


#ifdef HAVE_GETMNTINFO
static int disks_fill_table(THD* pThd, TABLE_LIST* pTables, Item* pCond)
{
    st_info *s;
    int count, rv= 0;
    TABLE* pTable= pTables->table;

    if (check_global_access(pThd, FILE_ACL, true))
        return 0;

#if defined(HAVE_GETMNTINFO_TAKES_statvfs)
    count= getmntinfo(&s, ST_WAIT);
#else
    count= getmntinfo(&s, MNT_WAIT);
#endif
    if (count == 0)
        return 1;

    while (count && rv == 0)
    {
        rv= disks_table_add_row_stat(pThd, pTable, s->f_mntfromname, s->f_mntonname, *s);
        count--;
        s++;
    }
    return rv;
}
#else /* HAVE_GETMNTINFO */

static mysql_mutex_t m_getmntent;

/* HAVE_GETMNTENT */
static int disks_fill_table(THD* pThd, TABLE_LIST* pTables, Item* pCond)
{
    int rv= 1;
#ifdef HAVE_SETMNTENT
    struct mntent* pEnt;
#else
    struct mnttab mnttabent, *pEnt= &mnttabent;
#endif
    FILE* pFile;
    TABLE* pTable= pTables->table;

    if (check_global_access(pThd, FILE_ACL, true))
        return 0;

#ifdef HAVE_SETMNTENT
    pFile= setmntent(MOUNTED, "r");
#else
    /* Solaris */
    pFile= fopen("/etc/mnttab", "r");
#endif

    if (!pFile)
        return 1;

    rv= 0;

    /*
      We lock the outer loop rather than between getmntent so the multiple
      infomation_schema.disks reads don't all start blocking each other and
      no-one gets any answers.
    */
    mysql_mutex_lock(&m_getmntent);

    while ((rv == 0) &&
#if defined(HAVE_SETMNTENT)
        (pEnt = getmntent(pFile))

#else
        getmntent(pFile, pEnt) != 0
#endif
        )
    {
        struct stat f;
        const char *path, *point;
#ifdef HAVE_SETMNTENT
        path= pEnt->mnt_dir;
        point= pEnt->mnt_fsname;
#else
        path= pEnt->mnt_mountp;
        point= pEnt->mnt_special;
#endif
        // Try to keep to real storage by excluding
        // read only mounts, and mount points that aren't directories
        if (hasmntopt(pEnt, MNTOPT_RO) != NULL)
            continue;
        if (stat(path, &f))
            continue;
        if (!S_ISDIR(f.st_mode))
            continue;
        rv= disks_table_add_row(pThd, pTable, point, path);
    }
    mysql_mutex_unlock(&m_getmntent);

#ifdef HAVE_SETMNTENT
    endmntent(pFile);
#else
    fclose(pFile);
#endif

    return rv;
}
#endif /* HAVE_GETMNTINFO */

static int disks_table_init(void *ptr)
{
    ST_SCHEMA_TABLE* pSchema_table = (ST_SCHEMA_TABLE*)ptr;

    pSchema_table->fields_info = disks_table_fields;
    pSchema_table->fill_table = disks_fill_table;
#ifndef HAVE_GETMNTINFO
    mysql_mutex_init(0, &m_getmntent, MY_MUTEX_INIT_SLOW);
#endif
    return 0;
}

static int disks_table_deinit(void *ptr __attribute__((unused)))
{
#ifndef HAVE_GETMNTINFO
    mysql_mutex_destroy(&m_getmntent);
#endif
    return 0;
}

} // namespace Show

extern "C"
{

maria_declare_plugin(disks)
{
    MYSQL_INFORMATION_SCHEMA_PLUGIN,
    &disks_table_info,                 /* type-specific descriptor */
    "DISKS",                           /* table name */
    "Johan Wikman, Daniel Black",      /* author */
    "Disk space information",          /* description */
    PLUGIN_LICENSE_GPL,                /* license type */
    Show::disks_table_init,            /* init function */
    Show::disks_table_deinit,          /* deinit function */
    0x0102,                            /* version = 1.2 */
    NULL,                              /* no status variables */
    NULL,                              /* no system variables */
    "1.2",                             /* String version representation */
    MariaDB_PLUGIN_MATURITY_STABLE     /* Maturity (see include/mysql/plugin.h)*/
}
mysql_declare_plugin_end;

}
