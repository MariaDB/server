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
  along with this program; if not, write to the Free Software Foundation,
  51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#ifndef PFS_VISITOR_H
#define PFS_VISITOR_H

#include "pfs_stat.h"

typedef struct system_status_var STATUS_VAR;

/**
  @file storage/perfschema/pfs_visitor.h
  Visitors (declarations).
*/

/**
  @addtogroup Performance_schema_buffers
  @{
*/

struct PFS_user;
struct PFS_account;
struct PFS_host;
struct PFS_thread;
struct PFS_instr_class;
struct PFS_mutex_class;
struct PFS_rwlock_class;
struct PFS_cond_class;
struct PFS_file_class;
struct PFS_socket_class;
struct PFS_memory_class;
struct PFS_table_share;
struct PFS_mutex;
struct PFS_rwlock;
struct PFS_cond;
struct PFS_file;
struct PFS_table;
struct PFS_stage_class;
struct PFS_statement_class;
struct PFS_transaction_class;
struct PFS_socket;
struct PFS_connection_slice;

/**
  Interface class to visit groups of connections.
  @sa PFS_connection_iterator
*/
class PFS_connection_visitor
{
public:
  PFS_connection_visitor() = default;
  virtual ~PFS_connection_visitor() = default;
  /** Visit all connections. */
  virtual void visit_global() {}
  /** Visit all connections of a host. */
  virtual void visit_host(PFS_host *pfs) {}
  /** Visit all connections of a user+host. */
  virtual void visit_account(PFS_account *pfs) {}
  /** Visit all connections of a user. */
  virtual void visit_user(PFS_user *pfs) {}
  /** Visit a thread. */
  virtual void visit_thread(PFS_thread *pfs) {}
  /** Visit a THD associated with a thread. */
  virtual void visit_THD(THD *thd) {}
};

/**
  Iterator over groups of connections.
  @sa PFS_connection_visitor
*/
class PFS_connection_iterator
{
public:
  /**
    Visit all connections.
    @param with_hosts when true, visit also all hosts.
    @param with_users when true, visit also all users.
    @param with_accounts when true, visit also all user+host.
    @param with_threads when true, visit also all threads.
    @param with_THDs when true, visit also all threads THD.
    @param visitor the visitor to call
  */
  static void visit_global(bool with_hosts, bool with_users,
                           bool with_accounts, bool with_threads,
                           bool with_THDs,
                           PFS_connection_visitor *visitor);
  /**
    Visit all connections of a host.
    @param host the host to visit.
    @param with_accounts when true, visit also all related user+host.
    @param with_threads when true, visit also all related threads.
    @param with_THDs when true, visit also all related threads THD.
    @param visitor the visitor to call
  */
  static void visit_host(PFS_host *host, bool with_accounts, bool with_threads,
                         bool with_THDs,
                         PFS_connection_visitor *visitor);
  /**
    Visit all connections of a user.
    @param user the user to visit.
    @param with_accounts when true, visit also all related user+host.
    @param with_threads when true, visit also all related threads.
    @param with_THDs when true, visit also all related threads THD.
    @param visitor the visitor to call
  */
  static void visit_user(PFS_user *user, bool with_accounts, bool with_threads,
                         bool with_THDs,
                         PFS_connection_visitor *visitor);
  /**
    Visit all connections of a user+host.
    @param account the user+host to visit.
    @param with_threads when true, visit also all related threads.
    @param with_THDs when true, visit also all related threads THD.
    @param visitor the visitor to call
  */
  static void visit_account(PFS_account *account, bool with_threads,
                            bool with_THDs,
                            PFS_connection_visitor *visitor);
  /**
    Visit a thread or connection.
    @param thread the thread to visit.
    @param visitor the visitor to call
  */
  static inline void visit_thread(PFS_thread *thread,
                                  PFS_connection_visitor *visitor)
  { visitor->visit_thread(thread); }

  /**
    Visit THD.
    @param thd the THD to visit.
    @param visitor the visitor to call.
  */
  static void visit_THD(THD *thd, PFS_connection_visitor *visitor);
};

/**
  Interface class to visit groups of instrumentation point instances.
  @sa PFS_instance_iterator
*/
class PFS_instance_visitor
{
public:
  PFS_instance_visitor() = default;
  virtual ~PFS_instance_visitor() = default;
  /** Visit a mutex class. */
  virtual void visit_mutex_class(PFS_mutex_class *pfs) {}
  /** Visit a rwlock class. */
  virtual void visit_rwlock_class(PFS_rwlock_class *pfs) {}
  /** Visit a cond class. */
  virtual void visit_cond_class(PFS_cond_class *pfs) {}
  /** Visit a file class. */
  virtual void visit_file_class(PFS_file_class *pfs) {}
  /** Visit a socket class. */
  virtual void visit_socket_class(PFS_socket_class *pfs) {}
  /** Visit a mutex instance. */
  virtual void visit_mutex(PFS_mutex *pfs) {}
  /** Visit a rwlock instance. */
  virtual void visit_rwlock(PFS_rwlock *pfs) {}
  /** Visit a cond instance. */
  virtual void visit_cond(PFS_cond *pfs) {}
  /** Visit a file instance. */
  virtual void visit_file(PFS_file *pfs) {}
  /** Visit a socket instance. */
  virtual void visit_socket(PFS_socket *pfs) {}
};

/**
  Iterator over groups of instrumentation point instances.
  @sa PFS_instance_visitor
*/
class PFS_instance_iterator
{
public:
  static void visit_all(PFS_instance_visitor *visitor);
  static void visit_all_mutex(PFS_instance_visitor *visitor);
  static void visit_all_mutex_classes(PFS_instance_visitor *visitor);
  static void visit_all_mutex_instances(PFS_instance_visitor *visitor);
  static void visit_all_rwlock(PFS_instance_visitor *visitor);
  static void visit_all_rwlock_classes(PFS_instance_visitor *visitor);
  static void visit_all_rwlock_instances(PFS_instance_visitor *visitor);
  static void visit_all_cond(PFS_instance_visitor *visitor);
  static void visit_all_cond_classes(PFS_instance_visitor *visitor);
  static void visit_all_cond_instances(PFS_instance_visitor *visitor);
  static void visit_all_file(PFS_instance_visitor *visitor);
  static void visit_all_file_classes(PFS_instance_visitor *visitor);
  static void visit_all_file_instances(PFS_instance_visitor *visitor);

  /**
    Visit a mutex class and related instances.
    @param klass the klass to visit.
    @param visitor the visitor to call
  */
  static void visit_mutex_instances(PFS_mutex_class *klass,
                                    PFS_instance_visitor *visitor);
  /**
    Visit a rwlock class and related instances.
    @param klass the klass to visit.
    @param visitor the visitor to call
  */
  static void visit_rwlock_instances(PFS_rwlock_class *klass,
                                     PFS_instance_visitor *visitor);
  /**
    Visit a cond class and related instances.
    @param klass the klass to visit.
    @param visitor the visitor to call
  */
  static void visit_cond_instances(PFS_cond_class *klass,
                                   PFS_instance_visitor *visitor);
  /**
    Visit a file class and related instances.
    @param klass the klass to visit.
    @param visitor the visitor to call
  */
  static void visit_file_instances(PFS_file_class *klass,
                                   PFS_instance_visitor *visitor);
  /**
    Visit a socket class and related instances.
    @param klass the klass to visit.
    @param visitor the visitor to call
  */
  static void visit_socket_instances(PFS_socket_class *klass,
                                     PFS_instance_visitor *visitor);
  /**
    Visit a socket class and related instances.
    @param klass the klass to visit.
    @param visitor the visitor to call
    @param thread the owning thread to match
    @param visit_class if true then visit the socket class
  */
  static void visit_socket_instances(PFS_socket_class *klass,
                                     PFS_instance_visitor *visitor,
                                     PFS_thread *thread,
                                     bool visit_class= true);
  /**
    Visit an instrument class and related instances.
    @param klass the klass to visit.
    @param visitor the visitor to call
    @param thread comparison criteria
    @param visit_class if true then visit the class
  */
  static void visit_instances(PFS_instr_class *klass,
                              PFS_instance_visitor *visitor,
                              PFS_thread *thread,
                              bool visit_class= true);
};

/**
  Interface class to visit groups of SQL objects.
  @sa PFS_object_iterator
*/
class PFS_object_visitor
{
public:
  PFS_object_visitor() = default;
  virtual ~PFS_object_visitor() = default;
  /** Visit global data. */
  virtual void visit_global() {}
  /** Visit a table share. */
  virtual void visit_table_share(PFS_table_share *pfs) {}
  /** Visit a table share index. */
  virtual void visit_table_share_index(PFS_table_share *pfs, uint index) {}
  /** Visit a table. */
  virtual void visit_table(PFS_table *pfs) {}
  /** Visit a table index. */
  virtual void visit_table_index(PFS_table *pfs, uint index) {}
};

/**
  Iterator over groups of SQL objects.
  @sa PFS_object_visitor
*/
class PFS_object_iterator
{
public:
  /** Visit all objects. */
  static void visit_all(PFS_object_visitor *visitor);
  /** Visit all tables and related handles. */
  static void visit_all_tables(PFS_object_visitor *visitor);
  /** Visit a table and related table handles. */
  static void visit_tables(PFS_table_share *share,
                           PFS_object_visitor *visitor);
  /** Visit a table index and related table handles indexes. */
  static void visit_table_indexes(PFS_table_share *share,
                                  uint index,
                                  PFS_object_visitor *visitor);
};

/**
  A concrete connection visitor that aggregates
  wait statistics for a given event_name.
*/
class PFS_connection_wait_visitor : public PFS_connection_visitor
{
public:
  /** Constructor. */
  PFS_connection_wait_visitor(PFS_instr_class *klass);
  virtual ~PFS_connection_wait_visitor();
  void visit_global() override;
  void visit_host(PFS_host *pfs) override;
  void visit_account(PFS_account *pfs) override;
  void visit_user(PFS_user *pfs) override;
  void visit_thread(PFS_thread *pfs) override;

  /** EVENT_NAME instrument index. */
  uint m_index;
  /** Wait statistic collected. */
  PFS_single_stat m_stat;
};

/**
  A concrete connection visitor that aggregates
  wait statistics for all events.
*/
class PFS_connection_all_wait_visitor : public PFS_connection_visitor
{
public:
  /** Constructor. */
  PFS_connection_all_wait_visitor();
  virtual ~PFS_connection_all_wait_visitor();
  void visit_global() override;
  void visit_host(PFS_host *pfs) override;
  void visit_account(PFS_account *pfs) override;
  void visit_user(PFS_user *pfs) override;
  void visit_thread(PFS_thread *pfs) override;

  /** Wait statistic collected. */
  PFS_single_stat m_stat;

private:
  void visit_connection_slice(PFS_connection_slice *pfs);
};

/**
  A concrete connection visitor that aggregates
  stage statistics.
*/
class PFS_connection_stage_visitor : public PFS_connection_visitor
{
public:
  /** Constructor. */
  PFS_connection_stage_visitor(PFS_stage_class *klass);
  virtual ~PFS_connection_stage_visitor();
  void visit_global() override;
  void visit_host(PFS_host *pfs) override;
  void visit_account(PFS_account *pfs) override;
  void visit_user(PFS_user *pfs) override;
  void visit_thread(PFS_thread *pfs) override;

  /** EVENT_NAME instrument index. */
  uint m_index;
  /** Stage statistic collected. */
  PFS_stage_stat m_stat;
};

/**
  A concrete connection visitor that aggregates
  statement statistics for a given event_name.
*/
class PFS_connection_statement_visitor : public PFS_connection_visitor
{
public:
  /** Constructor. */
  PFS_connection_statement_visitor(PFS_statement_class *klass);
  virtual ~PFS_connection_statement_visitor();
  void visit_global() override;
  void visit_host(PFS_host *pfs) override;
  void visit_account(PFS_account *pfs) override;
  void visit_user(PFS_user *pfs) override;
  void visit_thread(PFS_thread *pfs) override;

  /** EVENT_NAME instrument index. */
  uint m_index;
  /** Statement statistic collected. */
  PFS_statement_stat m_stat;
};

/**
  A concrete connection visitor that aggregates
  statement statistics for all events.
*/
class PFS_connection_all_statement_visitor : public PFS_connection_visitor
{
public:
  /** Constructor. */
  PFS_connection_all_statement_visitor();
  virtual ~PFS_connection_all_statement_visitor();
  void visit_global() override;
  void visit_host(PFS_host *pfs) override;
  void visit_account(PFS_account *pfs) override;
  void visit_user(PFS_user *pfs) override;
  void visit_thread(PFS_thread *pfs) override;

  /** Statement statistic collected. */
  PFS_statement_stat m_stat;

private:
  void visit_connection_slice(PFS_connection_slice *pfs);
};

/**
  A concrete connection visitor that aggregates
  transaction statistics for a given event_name.
*/
class PFS_connection_transaction_visitor : public PFS_connection_visitor
{
public:
  /** Constructor. */
  PFS_connection_transaction_visitor(PFS_transaction_class *klass);
  virtual ~PFS_connection_transaction_visitor();
  void visit_global() override;
  void visit_host(PFS_host *pfs) override;
  void visit_account(PFS_account *pfs) override;
  void visit_user(PFS_user *pfs) override;
  void visit_thread(PFS_thread *pfs) override;

  /** EVENT_NAME instrument index. */
  uint m_index;
  /** Statement statistic collected. */
  PFS_transaction_stat m_stat;
};

/** Disabled pending code review */
#if 0
/**
  A concrete connection visitor that aggregates
  transaction statistics for all events.
*/
class PFS_connection_all_transaction_visitor : public PFS_connection_visitor
{
public:
  /** Constructor. */
  PFS_connection_all_transaction_visitor();
  virtual ~PFS_connection_all_transaction_visitor();
  void visit_global() override;
  void visit_host(PFS_host *pfs) override;
  void visit_account(PFS_account *pfs) override;
  void visit_user(PFS_user *pfs) override;
  void visit_thread(PFS_thread *pfs) override;

  /** Statement statistic collected. */
  PFS_transaction_stat m_stat;

private:
  void visit_connection_slice(PFS_connection_slice *pfs);
};
#endif

/**
  A concrete connection visitor that aggregates
  connection statistics.
*/
class PFS_connection_stat_visitor : public PFS_connection_visitor
{
public:
  /** Constructor. */
  PFS_connection_stat_visitor();
  virtual ~PFS_connection_stat_visitor();
  void visit_global() override;
  void visit_host(PFS_host *pfs) override;
  void visit_account(PFS_account *pfs) override;
  void visit_user(PFS_user *pfs) override;
  void visit_thread(PFS_thread *pfs) override;

  /** Connection statistic collected. */
  PFS_connection_stat m_stat;
};

/**
  A concrete connection visitor that aggregates
  memory statistics for a given event_name.
*/
class PFS_connection_memory_visitor : public PFS_connection_visitor
{
public:
  /** Constructor. */
  PFS_connection_memory_visitor(PFS_memory_class *klass);
  virtual ~PFS_connection_memory_visitor();
  void visit_global() override;
  void visit_host(PFS_host *pfs) override;
  void visit_account(PFS_account *pfs) override;
  void visit_user(PFS_user *pfs) override;
  void visit_thread(PFS_thread *pfs) override;

  /** EVENT_NAME instrument index. */
  uint m_index;
  /** Statement statistic collected. */
  PFS_memory_stat m_stat;
};

/**
  A concrete connection visitor that aggregates
  status variables.
*/
class PFS_connection_status_visitor : public PFS_connection_visitor
{
public:
  /** Constructor. */
  PFS_connection_status_visitor(STATUS_VAR *status_vars);
  virtual ~PFS_connection_status_visitor();
  void visit_global() override;
  void visit_host(PFS_host *pfs) override;
  void visit_account(PFS_account *pfs) override;
  void visit_user(PFS_user *pfs) override;
  void visit_thread(PFS_thread *pfs) override;
  void visit_THD(THD *thd) override;

private:
  STATUS_VAR *m_status_vars;
};

/**
  A concrete instance visitor that aggregates
  wait statistics.
*/
class PFS_instance_wait_visitor : public PFS_instance_visitor
{
public:
  PFS_instance_wait_visitor();
  virtual ~PFS_instance_wait_visitor();
  void visit_mutex_class(PFS_mutex_class *pfs) override;
  void visit_rwlock_class(PFS_rwlock_class *pfs) override;
  void visit_cond_class(PFS_cond_class *pfs) override;
  void visit_file_class(PFS_file_class *pfs) override;
  void visit_socket_class(PFS_socket_class *pfs) override;
  void visit_mutex(PFS_mutex *pfs) override;
  void visit_rwlock(PFS_rwlock *pfs) override;
  void visit_cond(PFS_cond *pfs) override;
  void visit_file(PFS_file *pfs) override;
  void visit_socket(PFS_socket *pfs) override;

  /** Wait statistic collected. */
  PFS_single_stat m_stat;
};

/**
  A concrete object visitor that aggregates
  object wait statistics.
*/
class PFS_object_wait_visitor : public PFS_object_visitor
{
public:
  PFS_object_wait_visitor();
  virtual ~PFS_object_wait_visitor();
  void visit_global() override;
  void visit_table_share(PFS_table_share *pfs) override;
  void visit_table(PFS_table *pfs) override;

  /** Object wait statistic collected. */
  PFS_single_stat m_stat;
};

/**
  A concrete object visitor that aggregates
  table io wait statistics.
*/
class PFS_table_io_wait_visitor : public PFS_object_visitor
{
public:
  PFS_table_io_wait_visitor();
  virtual ~PFS_table_io_wait_visitor();
  void visit_global() override;
  void visit_table_share(PFS_table_share *pfs) override;
  void visit_table(PFS_table *pfs) override;

  /** Table io wait statistic collected. */
  PFS_single_stat m_stat;
};

/**
  A concrete object visitor that aggregates
  table io statistics.
*/
class PFS_table_io_stat_visitor : public PFS_object_visitor
{
public:
  PFS_table_io_stat_visitor();
  virtual ~PFS_table_io_stat_visitor();
  void visit_table_share(PFS_table_share *pfs) override;
  void visit_table(PFS_table *pfs) override;

  /** Table io statistic collected. */
  PFS_table_io_stat m_stat;
};

/**
  A concrete object visitor that aggregates
  index io statistics.
*/
class PFS_index_io_stat_visitor : public PFS_object_visitor
{
public:
  PFS_index_io_stat_visitor();
  virtual ~PFS_index_io_stat_visitor();
  void visit_table_share_index(PFS_table_share *pfs, uint index) override;
  void visit_table_index(PFS_table *pfs, uint index) override;

  /** Index io statistic collected. */
  PFS_table_io_stat m_stat;
};

/**
  A concrete object visitor that aggregates
  table lock wait statistics.
*/
class PFS_table_lock_wait_visitor : public PFS_object_visitor
{
public:
  PFS_table_lock_wait_visitor();
  virtual ~PFS_table_lock_wait_visitor();
  void visit_global() override;
  void visit_table_share(PFS_table_share *pfs) override;
  void visit_table(PFS_table *pfs) override;

  /** Table lock wait statistic collected. */
  PFS_single_stat m_stat;
};

/**
  A concrete object visitor that aggregates
  table lock statistics.
*/
class PFS_table_lock_stat_visitor : public PFS_object_visitor
{
public:
  PFS_table_lock_stat_visitor();
  virtual ~PFS_table_lock_stat_visitor();
  void visit_table_share(PFS_table_share *pfs) override;
  void visit_table(PFS_table *pfs) override;

  /** Table lock statistic collected. */
  PFS_table_lock_stat m_stat;
};

/**
  A concrete instance visitor that aggregates
  socket wait and byte count statistics.
*/
class PFS_instance_socket_io_stat_visitor : public PFS_instance_visitor
{
public:
  PFS_instance_socket_io_stat_visitor();
  virtual ~PFS_instance_socket_io_stat_visitor();
  void visit_socket_class(PFS_socket_class *pfs) override;
  void visit_socket(PFS_socket *pfs) override;

  /** Wait and byte count statistics collected. */
  PFS_socket_io_stat m_socket_io_stat;
};

/**
  A concrete instance visitor that aggregates
  file wait and byte count statistics.
*/
class PFS_instance_file_io_stat_visitor : public PFS_instance_visitor
{
public:
  PFS_instance_file_io_stat_visitor();
  virtual ~PFS_instance_file_io_stat_visitor();
  void visit_file_class(PFS_file_class *pfs) override;
  void visit_file(PFS_file *pfs) override;

  /** Wait and byte count statistics collected. */
  PFS_file_io_stat m_file_io_stat;
};

/** @} */
#endif

