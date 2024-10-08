/* Copyright (c) 2010, 2023, Oracle and/or its affiliates.

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
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335  USA */

#ifndef PFS_HOST_H
#define PFS_HOST_H

/**
  @file storage/perfschema/pfs_host.h
  Performance schema host (declarations).
*/

#include <atomic>

#include "pfs_lock.h"
#include "lf.h"
#include "pfs_con_slice.h"

struct PFS_global_param;
struct PFS_thread;

/**
  @addtogroup Performance_schema_buffers
  @{
*/

/** Hash key for a host. */
struct PFS_host_key
{
  /**
    Hash search key.
    This has to be a string for LF_HASH,
    the format is "<hostname><0x00>"
  */
  char m_hash_key[HOSTNAME_LENGTH + 1];
  uint m_key_length;
};

/** Per host statistics. */
struct PFS_ALIGNED PFS_host : PFS_connection_slice
{
public:
  inline void init_refcount(void)
  {
    m_refcount.store(1);
  }

  inline int get_refcount(void)
  {
    return m_refcount.load();
  }

  inline void inc_refcount(void)
  {
    m_refcount.fetch_add(1);
  }

  inline void dec_refcount(void)
  {
    m_refcount.fetch_sub(1);
  }

  void aggregate(bool alive);
  void aggregate_waits(void);
  void aggregate_stages(void);
  void aggregate_statements(void);
  void aggregate_transactions(void);
  void aggregate_memory(bool alive);
  void aggregate_status(void);
  void aggregate_stats(void);
  void release(void);

  void carry_memory_stat_delta(PFS_memory_stat_delta *delta, uint index);

  /* Internal lock. */
  pfs_lock m_lock;
  PFS_host_key m_key;
  const char *m_hostname;
  uint m_hostname_length;

  ulonglong m_disconnected_count;

private:
  std::atomic<int> m_refcount;
};

int init_host(const PFS_global_param *param);
void cleanup_host(void);
int init_host_hash(const PFS_global_param *param);
void cleanup_host_hash(void);

PFS_host *find_or_create_host(PFS_thread *thread,
                              const char *hostname, uint hostname_length);

PFS_host *sanitize_host(PFS_host *unsafe);
void purge_all_host(void);

/* For show status. */

extern LF_HASH host_hash;

/** @} */
#endif

