/* Copyright (c) 2008, 2013, Oracle and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is also distributed with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have included with MySQL.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software Foundation,
  51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#ifndef PFS_INSTR_CLASS_H
#define PFS_INSTR_CLASS_H

#include "my_global.h"
#include "mysql_com.h"                          /* NAME_LEN */
#include "lf.h"
#include "pfs_global.h"

/**
  @file storage/perfschema/pfs_instr_class.h
  Performance schema instruments meta data (declarations).
*/

/**
  Maximum length of an instrument name.
  For example, 'wait/sync/mutex/sql/LOCK_open' is an instrument name.
*/
#define PFS_MAX_INFO_NAME_LENGTH 128

/**
  Maximum length of the 'full' prefix of an instrument name.
  For example, for the instrument name 'wait/sync/mutex/sql/LOCK_open',
  the full prefix is 'wait/sync/mutex/sql/', which in turn derives from
  a prefix 'wait/sync/mutex' for mutexes, and a category of 'sql' for mutexes
  of the sql layer in the server.
*/
#define PFS_MAX_FULL_PREFIX_NAME_LENGTH 32

#include <my_global.h>
#include <my_sys.h>
#include <mysql/psi/psi.h>
#include "pfs_lock.h"
#include "pfs_stat.h"
#include "pfs_column_types.h"

struct PFS_global_param;

/**
  @addtogroup Performance_schema_buffers
  @{
*/

extern my_bool pfs_enabled;
extern enum_timer_name *class_timers[];

/** Key, naming a synch instrument (mutex, rwlock, cond). */
typedef unsigned int PFS_sync_key;
/** Key, naming a thread instrument. */
typedef unsigned int PFS_thread_key;
/** Key, naming a file instrument. */
typedef unsigned int PFS_file_key;
/** Key, naming a stage instrument. */
typedef unsigned int PFS_stage_key;
/** Key, naming a statement instrument. */
typedef unsigned int PFS_statement_key;
/** Key, naming a socket instrument. */
typedef unsigned int PFS_socket_key;

enum PFS_class_type
{
  PFS_CLASS_NONE=        0,
  PFS_CLASS_MUTEX=       1,
  PFS_CLASS_RWLOCK=      2,
  PFS_CLASS_COND=        3,
  PFS_CLASS_FILE=        4,
  PFS_CLASS_TABLE=       5,
  PFS_CLASS_STAGE=       6,
  PFS_CLASS_STATEMENT=   7,
  PFS_CLASS_SOCKET=      8,
  PFS_CLASS_TABLE_IO=    9,
  PFS_CLASS_TABLE_LOCK= 10,
  PFS_CLASS_IDLE=       11,
  PFS_CLASS_LAST=       PFS_CLASS_IDLE,
  PFS_CLASS_MAX=        PFS_CLASS_LAST + 1
};

/** User-defined instrument configuration. */
struct PFS_instr_config
{
  /* Instrument name. */
  char *m_name;
  /* Name length. */
  uint m_name_length;
  /** Enabled flag. */
  bool m_enabled;
  /** Timed flag. */
  bool m_timed;
};

extern DYNAMIC_ARRAY pfs_instr_config_array;
extern int pfs_instr_config_state;

static const int PFS_INSTR_CONFIG_NOT_INITIALIZED= 0;
static const int PFS_INSTR_CONFIG_ALLOCATED= 1;
static const int PFS_INSTR_CONFIG_DEALLOCATED= 2;

struct PFS_thread;

extern uint mutex_class_start;
extern uint rwlock_class_start;
extern uint cond_class_start;
extern uint file_class_start;
extern uint socket_class_start;
extern uint wait_class_max;

/** Information for all instrumentation. */
struct PFS_instr_class
{
  /** Class type */
  PFS_class_type m_type;
  /** True if this instrument is enabled. */
  bool m_enabled;
  /** True if this instrument is timed. */
  bool m_timed;
  /** Instrument flags. */
  int m_flags;
  /**
    Instrument name index.
    Self index in:
    - EVENTS_WAITS_SUMMARY_*_BY_EVENT_NAME for waits
    - EVENTS_STAGES_SUMMARY_*_BY_EVENT_NAME for stages
    - EVENTS_STATEMENTS_SUMMARY_*_BY_EVENT_NAME for statements
  */
  uint m_event_name_index;
  /** Instrument name. */
  char m_name[PFS_MAX_INFO_NAME_LENGTH];
  /** Length in bytes of @c m_name. */
  uint m_name_length;
  /** Timer associated with this class. */
  enum_timer_name *m_timer;

  bool is_singleton() const
  {
    return m_flags & PSI_FLAG_GLOBAL;
  }

  bool is_mutable() const
  {
    return m_flags & PSI_FLAG_MUTABLE;
  }

  static void set_enabled(PFS_instr_class *pfs, bool enabled);
  static void set_timed(PFS_instr_class *pfs, bool timed);

  bool is_deferred() const
  {
    switch(m_type)
    {
      case PFS_CLASS_SOCKET:
        return true;
        break;
      default:
        return false;
        break;
    };
  }
};

struct PFS_mutex;

/** Instrumentation metadata for a MUTEX. */
struct PFS_ALIGNED PFS_mutex_class : public PFS_instr_class
{
  /** Mutex usage statistics. */
  PFS_mutex_stat m_mutex_stat;
  /** Singleton instance. */
  PFS_mutex *m_singleton;
};

struct PFS_rwlock;

/** Instrumentation metadata for a RWLOCK. */
struct PFS_ALIGNED PFS_rwlock_class : public PFS_instr_class
{
  /** Rwlock usage statistics. */
  PFS_rwlock_stat m_rwlock_stat;
  /** Singleton instance. */
  PFS_rwlock *m_singleton;
};

struct PFS_cond;

/** Instrumentation metadata for a COND. */
struct PFS_ALIGNED PFS_cond_class : public PFS_instr_class
{
  /**
    Condition usage statistics.
    This statistic is not exposed in user visible tables yet.
  */
  PFS_cond_stat m_cond_stat;
  /** Singleton instance. */
  PFS_cond *m_singleton;
};

/** Instrumentation metadata of a thread. */
struct PFS_ALIGNED PFS_thread_class
{
  /** True if this thread instrument is enabled. */
  bool m_enabled;
  /** Singleton instance. */
  PFS_thread *m_singleton;
  /** Thread instrument name. */
  char m_name[PFS_MAX_INFO_NAME_LENGTH];
  /** Length in bytes of @c m_name. */
  uint m_name_length;
};

#define PFS_TABLESHARE_HASHKEY_SIZE (NAME_LEN + 1 + NAME_LEN + 1)

/** Key identifying a table share. */
struct PFS_table_share_key
{
  /**
    Hash search key.
    This has to be a string for LF_HASH,
    the format is "<enum_object_type><schema_name><0x00><object_name><0x00>"
    @see create_table_def_key
  */
  char m_hash_key[PFS_TABLESHARE_HASHKEY_SIZE];
  /** Length in bytes of @c m_hash_key. */
  uint m_key_length;
};

/** Table index or 'key' */
struct PFS_table_key
{
  /** Index name */
  char m_name[NAME_LEN];
  /** Length in bytes of @c m_name. */
  uint m_name_length;
};

/** Instrumentation metadata for a table share. */
struct PFS_ALIGNED PFS_table_share
{
public:
  uint32 get_version()
  { return m_lock.get_version(); }

  enum_object_type get_object_type()
  {
    return (enum_object_type) m_key.m_hash_key[0];
  }

  void aggregate_io(void);
  void aggregate_lock(void);

  inline void aggregate(void)
  {
    aggregate_io();
    aggregate_lock();
  }

  inline void init_refcount(void)
  {
    PFS_atomic::store_32(& m_refcount, 1);
  }

  inline int get_refcount(void)
  {
    return PFS_atomic::load_32(& m_refcount);
  }

  inline void inc_refcount(void)
  {
    PFS_atomic::add_32(& m_refcount, 1);
  }

  inline void dec_refcount(void)
  {
    PFS_atomic::add_32(& m_refcount, -1);
  }

  void refresh_setup_object_flags(PFS_thread *thread);

  /** Internal lock. */
  pfs_lock m_lock;
  /**
    True if table instrumentation is enabled.
    This flag is computed from the content of table setup_objects.
  */
  bool m_enabled;
  /**
    True if table instrumentation is timed.
    This flag is computed from the content of table setup_objects.
  */
  bool m_timed;
  /** Search key. */
  PFS_table_share_key m_key;
  /** Schema name. */
  const char *m_schema_name;
  /** Length in bytes of @c m_schema_name. */
  uint m_schema_name_length;
  /** Table name. */
  const char *m_table_name;
  /** Length in bytes of @c m_table_name. */
  uint m_table_name_length;
  /** Number of indexes. */
  uint m_key_count;
  /** Table statistics. */
  PFS_table_stat m_table_stat;
  /** Index names. */
  PFS_table_key m_keys[MAX_INDEXES];

private:
  /** Number of opened table handles. */
  int m_refcount;
};

/** Statistics for the IDLE instrument. */
extern PFS_single_stat global_idle_stat;
/** Statistics for dropped table io. */
extern PFS_table_io_stat global_table_io_stat;
/** Statistics for dropped table lock. */
extern PFS_table_lock_stat global_table_lock_stat;

inline uint sanitize_index_count(uint count)
{
  if (likely(count <= MAX_INDEXES))
    return count;
  return 0;
}

#define GLOBAL_TABLE_IO_EVENT_INDEX 0
#define GLOBAL_TABLE_LOCK_EVENT_INDEX 1
#define GLOBAL_IDLE_EVENT_INDEX 2

/**
  Instrument controlling all table io.
  This instrument is used with table SETUP_OBJECTS.
*/
extern PFS_instr_class global_table_io_class;

/**
  Instrument controlling all table lock.
  This instrument is used with table SETUP_OBJECTS.
*/
extern PFS_instr_class global_table_lock_class;

/**
  Instrument controlling all idle waits.
*/
extern PFS_instr_class global_idle_class;

struct PFS_file;

/** Instrumentation metadata for a file. */
struct PFS_ALIGNED PFS_file_class : public PFS_instr_class
{
  /** File usage statistics. */
  PFS_file_stat m_file_stat;
  /** Singleton instance. */
  PFS_file *m_singleton;
};

/** Instrumentation metadata for a stage. */
struct PFS_ALIGNED PFS_stage_class : public PFS_instr_class
{
  /**
    Length of the 'stage/<component>/' prefix.
    This is to extract 'foo' from 'stage/sql/foo'.
  */
  uint m_prefix_length;
  /** Stage usage statistics. */
  PFS_stage_stat m_stage_stat;
};

/** Instrumentation metadata for a statement. */
struct PFS_ALIGNED PFS_statement_class : public PFS_instr_class
{
};

struct  PFS_socket;

/** Instrumentation metadata for a socket. */
struct PFS_ALIGNED PFS_socket_class : public PFS_instr_class
{
  /** Socket usage statistics. */
  PFS_socket_stat m_socket_stat;
  /** Singleton instance. */
  PFS_socket *m_singleton;
};

void init_event_name_sizing(const PFS_global_param *param);

void register_global_classes();

int init_sync_class(uint mutex_class_sizing,
                    uint rwlock_class_sizing,
                    uint cond_class_sizing);

void cleanup_sync_class();
int init_thread_class(uint thread_class_sizing);
void cleanup_thread_class();
int init_table_share(uint table_share_sizing);
void cleanup_table_share();
int init_table_share_hash();
void cleanup_table_share_hash();
int init_file_class(uint file_class_sizing);
void cleanup_file_class();
int init_stage_class(uint stage_class_sizing);
void cleanup_stage_class();
int init_statement_class(uint statement_class_sizing);
void cleanup_statement_class();
int init_socket_class(uint socket_class_sizing);
void cleanup_socket_class();

PFS_sync_key register_mutex_class(const char *name, uint name_length,
                                  int flags);

PFS_sync_key register_rwlock_class(const char *name, uint name_length,
                                   int flags);

PFS_sync_key register_cond_class(const char *name, uint name_length,
                                 int flags);

PFS_thread_key register_thread_class(const char *name, uint name_length,
                                     int flags);

PFS_file_key register_file_class(const char *name, uint name_length,
                                 int flags);

PFS_stage_key register_stage_class(const char *name,
                                   uint prefix_length,
                                   uint name_length,
                                   int flags);

PFS_statement_key register_statement_class(const char *name, uint name_length,
                                           int flags);

PFS_socket_key register_socket_class(const char *name, uint name_length,
                                     int flags);

PFS_mutex_class *find_mutex_class(PSI_mutex_key key);
PFS_mutex_class *sanitize_mutex_class(PFS_mutex_class *unsafe);
PFS_rwlock_class *find_rwlock_class(PSI_rwlock_key key);
PFS_rwlock_class *sanitize_rwlock_class(PFS_rwlock_class *unsafe);
PFS_cond_class *find_cond_class(PSI_cond_key key);
PFS_cond_class *sanitize_cond_class(PFS_cond_class *unsafe);
PFS_thread_class *find_thread_class(PSI_thread_key key);
PFS_thread_class *sanitize_thread_class(PFS_thread_class *unsafe);
PFS_file_class *find_file_class(PSI_file_key key);
PFS_file_class *sanitize_file_class(PFS_file_class *unsafe);
PFS_stage_class *find_stage_class(PSI_stage_key key);
PFS_stage_class *sanitize_stage_class(PFS_stage_class *unsafe);
PFS_statement_class *find_statement_class(PSI_statement_key key);
PFS_statement_class *sanitize_statement_class(PFS_statement_class *unsafe);
PFS_instr_class *find_table_class(uint index);
PFS_instr_class *sanitize_table_class(PFS_instr_class *unsafe);
PFS_socket_class *find_socket_class(PSI_socket_key key);
PFS_socket_class *sanitize_socket_class(PFS_socket_class *unsafe);
PFS_instr_class *find_idle_class(uint index);
PFS_instr_class *sanitize_idle_class(PFS_instr_class *unsafe);

PFS_table_share *find_or_create_table_share(PFS_thread *thread,
                                            bool temporary,
                                            const TABLE_SHARE *share);
void release_table_share(PFS_table_share *pfs);
void drop_table_share(PFS_thread *thread,
                      bool temporary,
                      const char *schema_name, uint schema_name_length,
                      const char *table_name, uint table_name_length);

PFS_table_share *sanitize_table_share(PFS_table_share *unsafe);

extern ulong mutex_class_max;
extern ulong mutex_class_lost;
extern ulong rwlock_class_max;
extern ulong rwlock_class_lost;
extern ulong cond_class_max;
extern ulong cond_class_lost;
extern ulong thread_class_max;
extern ulong thread_class_lost;
extern ulong file_class_max;
extern ulong file_class_lost;
extern ulong stage_class_max;
extern ulong stage_class_lost;
extern ulong statement_class_max;
extern ulong statement_class_lost;
extern ulong socket_class_max;
extern ulong socket_class_lost;
extern ulong table_share_max;
extern ulong table_share_lost;

/* Exposing the data directly, for iterators. */

extern PFS_mutex_class *mutex_class_array;
extern PFS_rwlock_class *rwlock_class_array;
extern PFS_cond_class *cond_class_array;
extern PFS_file_class *file_class_array;
extern PFS_table_share *table_share_array;

void reset_events_waits_by_class();
void reset_file_class_io();
void reset_socket_class_io();

/** Update derived flags for all table shares. */
void update_table_share_derived_flags(PFS_thread *thread);

extern LF_HASH table_share_hash;

/** @} */
#endif

