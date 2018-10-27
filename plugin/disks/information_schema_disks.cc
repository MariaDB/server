/*
   Copyright (c) 2017, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA */

#include <my_global.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <mntent.h>
#include <sql_class.h>
#include <table.h>

bool schema_table_store_record(THD *thd, TABLE *table);

namespace
{

struct st_mysql_information_schema disks_table_info = { MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION };

ST_FIELD_INFO disks_table_fields[]=
{
    { "Disk",      PATH_MAX, MYSQL_TYPE_STRING, 0, 0 ,0, 0 },
    { "Path",      PATH_MAX, MYSQL_TYPE_STRING, 0, 0 ,0, 0 },
    { "Total",           32, MYSQL_TYPE_LONG,   0, 0 ,0 ,0 }, // Total amount available
    { "Used",            32, MYSQL_TYPE_LONG,   0, 0 ,0 ,0 }, // Amount of space used
    { "Available",       32, MYSQL_TYPE_LONG,   0, 0 ,0 ,0 }, // Amount available to users other than root.
    { 0, 0, MYSQL_TYPE_NULL, 0, 0, 0, 0 }
};

int disks_table_add_row(THD* pThd,
                        TABLE* pTable,
                        const char* zDisk,
                        const char* zPath,
                        const struct statvfs& info)
{
    // From: http://pubs.opengroup.org/onlinepubs/009695399/basedefs/sys/statvfs.h.html
    //
    // f_frsize   Fundamental file system block size.
    // f_blocks   Total number of blocks on file system in units of f_frsize.
    // f_bfree    Total number of free blocks.
    // f_bavail   Number of free blocks available to non-privileged process.

    size_t total = (info.f_frsize * info.f_blocks) / 1024;
    size_t used  = (info.f_frsize * (info.f_blocks - info.f_bfree)) / 1024;
    size_t avail = (info.f_frsize * info.f_bavail) / 1024;

    pTable->field[0]->store(zDisk, strlen(zDisk), system_charset_info);
    pTable->field[1]->store(zPath, strlen(zPath), system_charset_info);
    pTable->field[2]->store(total);
    pTable->field[3]->store(used);
    pTable->field[4]->store(avail);

    // 0 means success.
    return (schema_table_store_record(pThd, pTable) != 0) ? 1 : 0;
}

int disks_table_add_row(THD* pThd, TABLE* pTable, const char* zDisk, const char* zPath)
{
    int rv = 0;

    struct statvfs info;

    if (statvfs(zPath, &info) == 0) // We ignore failures.
    {
        rv = disks_table_add_row(pThd, pTable, zDisk, zPath, info);
    }

    return rv;
}

int disks_fill_table(THD* pThd, TABLE_LIST* pTables, Item* pCond)
{
    int rv = 1;
    TABLE* pTable = pTables->table;

    FILE* pFile = setmntent("/etc/mtab", "r");

    if (pFile)
    {
        const size_t BUFFER_SIZE = 4096; // 4K should be sufficient.

        char* pBuffer = new (std::nothrow) char [BUFFER_SIZE];

        if (pBuffer)
        {
            rv = 0;

            struct mntent ent;
            struct mntent* pEnt;

            while ((rv == 0) && (pEnt = getmntent_r(pFile, &ent, pBuffer, BUFFER_SIZE)))
            {
                // We only report the ones that refer to physical disks.
                if (pEnt->mnt_fsname[0] == '/')
                {
                    rv = disks_table_add_row(pThd, pTable, pEnt->mnt_fsname, pEnt->mnt_dir);
                }
            }

            delete [] pBuffer;
        }
        else
        {
            rv = 1;
        }

        endmntent(pFile);
    }

    return rv;
}

int disks_table_init(void *ptr)
{
    ST_SCHEMA_TABLE* pSchema_table = (ST_SCHEMA_TABLE*)ptr;

    pSchema_table->fields_info = disks_table_fields;
    pSchema_table->fill_table = disks_fill_table;
    return 0;
}

}

extern "C"
{

maria_declare_plugin(disks)
{
    MYSQL_INFORMATION_SCHEMA_PLUGIN,
    &disks_table_info,                 /* type-specific descriptor */
    "DISKS",                           /* table name */
    "Johan Wikman",                    /* author */
    "Disk space information",          /* description */
    PLUGIN_LICENSE_GPL,                /* license type */
    disks_table_init,                  /* init function */
    NULL,                              /* deinit function */
    0x0100,                            /* version = 1.0 */
    NULL,                              /* no status variables */
    NULL,                              /* no system variables */
    "1.0",                             /* String version representation */
    MariaDB_PLUGIN_MATURITY_BETA       /* Maturity (see include/mysql/plugin.h)*/
}
mysql_declare_plugin_end;

}
