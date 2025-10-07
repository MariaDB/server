/* Copyright (C) 2013-2023 Codership Oy <info@codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA. */

#ifndef WSREP_VAR_H
#define WSREP_VAR_H

#include <set>
#include <my_config.h>

#ifdef WITH_WSREP

#define WSREP_CLUSTER_NAME              "my_wsrep_cluster"
#define WSREP_NODE_INCOMING_AUTO        "AUTO"
#define WSREP_START_POSITION_ZERO       "00000000-0000-0000-0000-000000000000:-1"
#define WSREP_START_POSITION_ZERO_GTID  "00000000-0000-0000-0000-000000000000:-1,0-0-0"

// MySQL variables funcs

#include "sql_priv.h"
#include <sql_plugin.h>
#include <mysql/plugin.h>

class sys_var;
class set_var;
class THD;

/*
   Wrapper class for thread scheduling parameters. For details, about
   values see sched_setscheduler(2) and pthread_setschedparams(3)
   documentation.
*/
class Thread_sched_param
{
public:
  /*
     Default constructor. Initializes to default system
     scheduling parameters.
  */
Thread_sched_param()
    : policy_(SCHED_OTHER),
      priority_(0)
  { }

  /*
     Construct Thread_sched_param from given policy and priority
     integer values.
  */
Thread_sched_param(int policy, int prio)
    : policy_(policy),
      priority_(prio)
  { }

  /*
     Construct Thread_sched_param from given string representation
      which must have form of

        <policy>:<priority>

     where policy is one of "other", "fifo", "rr" and priority
     is an integer.
  */
  Thread_sched_param(const std::string& param);

  /*
     Set the policy and priority from the given string representation.

     Return false if the call succeeds and true on failure.
  */
  bool set(const std::string& param);

  // Return scheduling policy
  int policy() const { return policy_; }

  // Return scheduling priority
  int priority() const { return priority_  ; }

  // Equal to operator overload
  bool operator==(const Thread_sched_param& other) const {
    return (policy_ == other.policy_ && priority_ == other.priority_);
  }

  // Not equal to operator overload
  bool operator!=(const Thread_sched_param& other) const {
    return !(*this == other);
  }

  // Default system Thread_sched_param
  static Thread_sched_param system_default;

  void print(std::ostream& os) const;

private:

  int policy_;     // Scheduling policy
  int priority_;   // Scheduling priority
};


/*
  A manager of thread priorities for pthreads.

  The manager keeps a list of threads (identified by pthread_t).
  New threads can be added to the list with the add() method
  and removed from the list by the remove() method.
  The priorities of all the threads on the list are modified
  with the update_priorities() call.
 */
class Thread_priority_manager
{
public:
  Thread_priority_manager();
  ~Thread_priority_manager();

  void add(pthread_t thread);
  void remove(pthread_t thread);
  bool update_priorities(const char *priority_string);

private:
  /* list of threads */
  std::set<pthread_t> m_threads;
  /* current thread scheduling parameters */
  Thread_sched_param m_sched_param;
  /* for serializing all method calls */
  mutable mysql_mutex_t LOCK_thread_priority_manager;

  int set_priority(pthread_t thread);
};

/*
   Return current scheduling parameters for given thread
*/
Thread_sched_param thread_get_schedparam(pthread_t thread);

/*
  Insertion operator for Thread_sched_param
*/
inline std::ostream& operator<<(std::ostream& os,
                                const Thread_sched_param& sp)
{
  sp.print(os); return os;
}


int wsrep_init_vars();
void wsrep_set_wsrep_on(THD *thd);
void wsrep_free_status_vars();

#define CHECK_ARGS   (sys_var *self, THD* thd, set_var *var)
#define UPDATE_ARGS  (sys_var *self, THD* thd, enum_var_type type)
#define DEFAULT_ARGS (THD* thd, enum_var_type var_type)
#define INIT_ARGS    (const char* opt)

extern bool wsrep_causal_reads_update        UPDATE_ARGS;
extern bool wsrep_on_check                   CHECK_ARGS;
extern bool wsrep_on_update                  UPDATE_ARGS;
extern bool wsrep_sync_wait_update           UPDATE_ARGS;
extern bool wsrep_start_position_check       CHECK_ARGS;
extern bool wsrep_start_position_update      UPDATE_ARGS;
extern bool wsrep_start_position_init        INIT_ARGS;

extern bool wsrep_provider_check             CHECK_ARGS;
extern bool wsrep_provider_update            UPDATE_ARGS;
extern void wsrep_provider_init              INIT_ARGS;

extern bool wsrep_provider_options_check     CHECK_ARGS;
extern bool wsrep_provider_options_update    UPDATE_ARGS;
extern void wsrep_provider_options_init      INIT_ARGS;

extern bool wsrep_cluster_address_check      CHECK_ARGS;
extern bool wsrep_cluster_address_update     UPDATE_ARGS;
extern void wsrep_cluster_address_init       INIT_ARGS;

extern bool wsrep_cluster_name_check         CHECK_ARGS;
extern bool wsrep_cluster_name_update        UPDATE_ARGS;

extern bool wsrep_node_name_check            CHECK_ARGS;
extern bool wsrep_node_name_update           UPDATE_ARGS;

extern bool wsrep_node_address_check         CHECK_ARGS;
extern bool wsrep_node_address_update        UPDATE_ARGS;
extern void wsrep_node_address_init          INIT_ARGS;

extern bool wsrep_sst_method_check           CHECK_ARGS;
extern bool wsrep_sst_method_update          UPDATE_ARGS;
extern void wsrep_sst_method_init            INIT_ARGS;

extern bool wsrep_sst_receive_address_check  CHECK_ARGS;
extern bool wsrep_sst_receive_address_update UPDATE_ARGS;

extern bool wsrep_sst_auth_check             CHECK_ARGS;
extern bool wsrep_sst_auth_update            UPDATE_ARGS;

extern bool wsrep_sst_donor_check            CHECK_ARGS;
extern bool wsrep_sst_donor_update           UPDATE_ARGS;

extern bool wsrep_slave_threads_check        CHECK_ARGS;
extern bool wsrep_slave_threads_update       UPDATE_ARGS;

extern bool wsrep_desync_check               CHECK_ARGS;
extern bool wsrep_desync_update              UPDATE_ARGS;

extern bool wsrep_trx_fragment_size_check    CHECK_ARGS;
extern bool wsrep_trx_fragment_size_update   UPDATE_ARGS;

extern bool wsrep_trx_fragment_unit_update   UPDATE_ARGS;

extern bool wsrep_max_ws_size_check          CHECK_ARGS;
extern bool wsrep_max_ws_size_update         UPDATE_ARGS;

extern bool wsrep_reject_queries_update      UPDATE_ARGS;

extern bool wsrep_debug_update               UPDATE_ARGS;

extern bool wsrep_gtid_seq_no_check          CHECK_ARGS;

extern bool wsrep_gtid_domain_id_update      UPDATE_ARGS;

extern bool wsrep_mode_check                 CHECK_ARGS;
extern bool wsrep_strict_ddl_update          UPDATE_ARGS;
extern bool wsrep_replicate_myisam_update    UPDATE_ARGS;
extern bool wsrep_replicate_myisam_check     CHECK_ARGS;
extern bool wsrep_forced_binlog_format_check CHECK_ARGS;

extern bool wsrep_applier_priority_check     CHECK_ARGS;
extern bool wsrep_applier_priority_update    UPDATE_ARGS;

extern Thread_priority_manager *thread_priority_manager;

#else  /* WITH_WSREP */

#define wsrep_provider_init(X)
#define wsrep_init_vars() (0)
#define wsrep_start_position_init(X)

#endif /* WITH_WSREP */
#endif /* WSREP_VAR_H */
