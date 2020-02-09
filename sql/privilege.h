#ifndef PRIVILEGE_H_INCLUDED
#define PRIVILEGE_H_INCLUDED

/* Copyright (c) 2020, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#include "my_global.h" // ulonglong


/*
  A strict enum to store privilege bits.

  We should eventually make if even stricter using "enum class privilege_t" and:
  - Replace all code pieces like `if (priv)` to `if (priv != NO_ACL)`
  - Remove "delete" comparison operators below
*/
enum privilege_t: unsigned long long
{
  NO_ACL                = (0),
  SELECT_ACL            = (1UL << 0),
  INSERT_ACL            = (1UL << 1),
  UPDATE_ACL            = (1UL << 2),
  DELETE_ACL            = (1UL << 3),
  CREATE_ACL            = (1UL << 4),
  DROP_ACL              = (1UL << 5),
  RELOAD_ACL            = (1UL << 6),
  SHUTDOWN_ACL          = (1UL << 7),
  PROCESS_ACL           = (1UL << 8),
  FILE_ACL              = (1UL << 9),
  GRANT_ACL             = (1UL << 10),
  REFERENCES_ACL        = (1UL << 11),
  INDEX_ACL             = (1UL << 12),
  ALTER_ACL             = (1UL << 13),
  SHOW_DB_ACL           = (1UL << 14),
  SUPER_ACL             = (1UL << 15),
  CREATE_TMP_ACL        = (1UL << 16),
  LOCK_TABLES_ACL       = (1UL << 17),
  EXECUTE_ACL           = (1UL << 18),
  REPL_SLAVE_ACL        = (1UL << 19),
  REPL_CLIENT_ACL       = (1UL << 20),
  CREATE_VIEW_ACL       = (1UL << 21),
  SHOW_VIEW_ACL         = (1UL << 22),
  CREATE_PROC_ACL       = (1UL << 23),
  ALTER_PROC_ACL        = (1UL << 24),
  CREATE_USER_ACL       = (1UL << 25),
  EVENT_ACL             = (1UL << 26),
  TRIGGER_ACL           = (1UL << 27),
  CREATE_TABLESPACE_ACL = (1UL << 28),
  DELETE_HISTORY_ACL    = (1UL << 29),
  /*
    don't forget to update
    1. static struct show_privileges_st sys_privileges[]
    2. static const char *command_array[] and static uint command_lengths[]
    3. mysql_system_tables.sql and mysql_system_tables_fix.sql
    4. acl_init() or whatever - to define behaviour for old privilege tables
    5. sql_yacc.yy - for GRANT/REVOKE to work
    6. ALL_KNOWN_ACL
  */
  ALL_KNOWN_ACL         = (1UL << 30) - 1 // A combination of all defined bits
};


// Unary operators
static inline constexpr ulonglong operator~(privilege_t access)
{
  return ~static_cast<ulonglong>(access);
}

/*
  Comparison operators.
  Delete automatic conversion between to/from integer types as much as possible.
  This forces to use `(priv == NO_ACL)` instead of `(priv == 0)`.

  Note: these operators will be gone when we change privilege_t to
  "enum class privilege_t". See comments above.
*/
static inline bool operator==(privilege_t, ulonglong)= delete;
static inline bool operator==(privilege_t,     ulong)= delete;
static inline bool operator==(privilege_t,      uint)= delete;
static inline bool operator==(privilege_t,     uchar)= delete;
static inline bool operator==(privilege_t,  longlong)= delete;
static inline bool operator==(privilege_t,      long)= delete;
static inline bool operator==(privilege_t,       int)= delete;
static inline bool operator==(privilege_t,      char)= delete;
static inline bool operator==(privilege_t,      bool)= delete;

static inline bool operator==(ulonglong, privilege_t)= delete;
static inline bool operator==(ulong,     privilege_t)= delete;
static inline bool operator==(uint,      privilege_t)= delete;
static inline bool operator==(uchar,     privilege_t)= delete;
static inline bool operator==(longlong,  privilege_t)= delete;
static inline bool operator==(long,      privilege_t)= delete;
static inline bool operator==(int,       privilege_t)= delete;
static inline bool operator==(char,      privilege_t)= delete;
static inline bool operator==(bool,      privilege_t)= delete;

static inline bool operator!=(privilege_t, ulonglong)= delete;
static inline bool operator!=(privilege_t,     ulong)= delete;
static inline bool operator!=(privilege_t,      uint)= delete;
static inline bool operator!=(privilege_t,     uchar)= delete;
static inline bool operator!=(privilege_t,  longlong)= delete;
static inline bool operator!=(privilege_t,      long)= delete;
static inline bool operator!=(privilege_t,       int)= delete;
static inline bool operator!=(privilege_t,      char)= delete;
static inline bool operator!=(privilege_t,      bool)= delete;

static inline bool operator!=(ulonglong, privilege_t)= delete;
static inline bool operator!=(ulong,     privilege_t)= delete;
static inline bool operator!=(uint,      privilege_t)= delete;
static inline bool operator!=(uchar,     privilege_t)= delete;
static inline bool operator!=(longlong,  privilege_t)= delete;
static inline bool operator!=(long,      privilege_t)= delete;
static inline bool operator!=(int,       privilege_t)= delete;
static inline bool operator!=(char,      privilege_t)= delete;
static inline bool operator!=(bool,      privilege_t)= delete;


// Dyadic bitwise operators
static inline constexpr privilege_t operator&(privilege_t a, privilege_t b)
{
  return static_cast<privilege_t>(static_cast<ulonglong>(a) &
                                  static_cast<ulonglong>(b));
}

static inline constexpr privilege_t operator&(ulonglong a, privilege_t b)
{
  return static_cast<privilege_t>(a & static_cast<ulonglong>(b));
}

static inline constexpr privilege_t operator&(privilege_t a, ulonglong b)
{
  return static_cast<privilege_t>(static_cast<ulonglong>(a) & b);
}

static inline constexpr privilege_t operator|(privilege_t a, privilege_t b)
{
  return static_cast<privilege_t>(static_cast<ulonglong>(a) |
                                  static_cast<ulonglong>(b));
}


// Dyadyc bitwise assignment operators
static inline privilege_t& operator&=(privilege_t &a, privilege_t b)
{
  return a= a & b;
}

static inline privilege_t& operator&=(privilege_t &a, ulonglong b)
{
  return a= a & b;
}

static inline privilege_t& operator|=(privilege_t &a, privilege_t b)
{
  return a= a | b;
}



constexpr privilege_t COL_DML_ACLS=
  SELECT_ACL | INSERT_ACL | UPDATE_ACL | DELETE_ACL;

constexpr privilege_t VIEW_ACLS=
  CREATE_VIEW_ACL | SHOW_VIEW_ACL;

constexpr privilege_t STD_TABLE_DDL_ACLS=
  CREATE_ACL | DROP_ACL | ALTER_ACL;

constexpr privilege_t ALL_TABLE_DDL_ACLS=
  STD_TABLE_DDL_ACLS | INDEX_ACL;

constexpr privilege_t COL_ACLS=
  SELECT_ACL | INSERT_ACL | UPDATE_ACL | REFERENCES_ACL;

constexpr privilege_t PROC_DDL_ACLS=
  CREATE_PROC_ACL | ALTER_PROC_ACL;

constexpr privilege_t SHOW_PROC_ACLS=
  PROC_DDL_ACLS | EXECUTE_ACL;

constexpr privilege_t TABLE_ACLS=
  COL_DML_ACLS | ALL_TABLE_DDL_ACLS | VIEW_ACLS |
  GRANT_ACL | REFERENCES_ACL | 
  TRIGGER_ACL | DELETE_HISTORY_ACL;

constexpr privilege_t DB_ACLS=
   TABLE_ACLS | PROC_DDL_ACLS | EXECUTE_ACL |
   CREATE_TMP_ACL | LOCK_TABLES_ACL | EVENT_ACL;

constexpr privilege_t PROC_ACLS=
  ALTER_PROC_ACL | EXECUTE_ACL | GRANT_ACL;

constexpr privilege_t GLOBAL_ACLS=
  DB_ACLS | SHOW_DB_ACL |
  CREATE_USER_ACL | CREATE_TABLESPACE_ACL |
  SUPER_ACL | RELOAD_ACL | SHUTDOWN_ACL | PROCESS_ACL | FILE_ACL |
  REPL_SLAVE_ACL | REPL_CLIENT_ACL;

constexpr privilege_t DEFAULT_CREATE_PROC_ACLS=
  ALTER_PROC_ACL | EXECUTE_ACL;

constexpr privilege_t SHOW_CREATE_TABLE_ACLS=
  COL_DML_ACLS | ALL_TABLE_DDL_ACLS |
  TRIGGER_ACL | REFERENCES_ACL | GRANT_ACL | VIEW_ACLS;

/**
  Table-level privileges which are automatically "granted" to everyone on
  existing temporary tables (CREATE_ACL is necessary for ALTER ... RENAME).
*/
constexpr privilege_t TMP_TABLE_ACLS=
  COL_DML_ACLS | ALL_TABLE_DDL_ACLS;

/*
  Defines to change the above bits to how things are stored in tables
  This is needed as the 'host' and 'db' table is missing a few privileges
*/

/* Privileges that need to be reallocated (in continous chunks) */
constexpr privilege_t DB_CHUNK0 (COL_DML_ACLS | CREATE_ACL | DROP_ACL);
constexpr privilege_t DB_CHUNK1 (GRANT_ACL | REFERENCES_ACL | INDEX_ACL | ALTER_ACL);
constexpr privilege_t DB_CHUNK2 (CREATE_TMP_ACL | LOCK_TABLES_ACL);
constexpr privilege_t DB_CHUNK3 (VIEW_ACLS | PROC_DDL_ACLS);
constexpr privilege_t DB_CHUNK4 (EXECUTE_ACL);
constexpr privilege_t DB_CHUNK5 (EVENT_ACL | TRIGGER_ACL);
constexpr privilege_t DB_CHUNK6 (DELETE_HISTORY_ACL);


static inline privilege_t fix_rights_for_db(privilege_t access)
{
  ulonglong A(access);
  return static_cast<privilege_t>
           (((A)      & DB_CHUNK0) |
            ((A << 4) & DB_CHUNK1) |
            ((A << 6) & DB_CHUNK2) |
            ((A << 9) & DB_CHUNK3) |
            ((A << 2) & DB_CHUNK4) |
            ((A << 9) & DB_CHUNK5) |
            ((A << 10) & DB_CHUNK6));
}

static inline privilege_t get_rights_for_db(privilege_t access)
{
  ulonglong A(access);
  return static_cast<privilege_t>
           ((A & DB_CHUNK0)       |
           ((A & DB_CHUNK1) >> 4) |
           ((A & DB_CHUNK2) >> 6) |
           ((A & DB_CHUNK3) >> 9) |
           ((A & DB_CHUNK4) >> 2) |
           ((A & DB_CHUNK5) >> 9) |
           ((A & DB_CHUNK6) >> 10));
}


#define TBL_CHUNK0 DB_CHUNK0
#define TBL_CHUNK1 DB_CHUNK1
#define TBL_CHUNK2 (CREATE_VIEW_ACL | SHOW_VIEW_ACL)
#define TBL_CHUNK3 TRIGGER_ACL
#define TBL_CHUNK4 (DELETE_HISTORY_ACL)


static inline privilege_t fix_rights_for_table(privilege_t access)
{
  ulonglong A(access);
  return static_cast<privilege_t>
           ((A        & TBL_CHUNK0) |
           ((A <<  4) & TBL_CHUNK1) |
           ((A << 11) & TBL_CHUNK2) |
           ((A << 15) & TBL_CHUNK3) |
           ((A << 16) & TBL_CHUNK4));
}


static inline privilege_t get_rights_for_table(privilege_t access)
{
  ulonglong A(access);
  return static_cast<privilege_t>
           ((A & TBL_CHUNK0)        |
           ((A & TBL_CHUNK1) >>  4) |
           ((A & TBL_CHUNK2) >> 11) |
           ((A & TBL_CHUNK3) >> 15) |
           ((A & TBL_CHUNK4) >> 16));
}


static inline privilege_t fix_rights_for_column(privilege_t A)
{
  const ulonglong mask(SELECT_ACL | INSERT_ACL | UPDATE_ACL);
  return (A & mask) | static_cast<privilege_t>((A & ~mask) << 8);
}


static inline privilege_t get_rights_for_column(privilege_t A)
{
  const ulonglong mask(SELECT_ACL | INSERT_ACL | UPDATE_ACL);
  return static_cast<privilege_t>((static_cast<ulonglong>(A) & mask) |
                                  (static_cast<ulonglong>(A) >> 8));
}


static inline privilege_t fix_rights_for_procedure(privilege_t access)
{
  ulonglong A(access);
  return static_cast<privilege_t>
           (((A << 18) & EXECUTE_ACL)    |
            ((A << 23) & ALTER_PROC_ACL) |
            ((A << 8) & GRANT_ACL));
}


static inline privilege_t get_rights_for_procedure(privilege_t access)
{
  ulonglong A(access);
  return static_cast<privilege_t>
           (((A & EXECUTE_ACL)    >> 18) |
            ((A & ALTER_PROC_ACL) >> 23) |
            ((A & GRANT_ACL) >> 8));
}


#endif /* PRIVILEGE_H_INCLUDED */
