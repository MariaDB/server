/* Copyright (c) 2008, 2017, Oracle and/or its affiliates. All rights reserved.

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

#include <my_global.h>
#include <my_pthread.h>
#include <pfs_instr.h>
#include <pfs_stat.h>
#include <pfs_global.h>
#include <pfs_instr_class.h>
#include <tap.h>

#include "stub_pfs_global.h"

#include <string.h> /* memset */

void test_oom()
{
  int rc;
  PFS_global_param param;

  stub_alloc_always_fails= true;

  memset(& param, 0xFF, sizeof(param));
  param.m_enabled= true;
  param.m_mutex_class_sizing= 10;
  param.m_rwlock_class_sizing= 0;
  param.m_cond_class_sizing= 0;
  param.m_thread_class_sizing= 0;
  param.m_table_share_sizing= 0;
  param.m_file_class_sizing= 0;
  param.m_socket_class_sizing= 0;
  param.m_mutex_sizing= 1000;
  param.m_rwlock_sizing= 0;
  param.m_cond_sizing= 0;
  param.m_thread_sizing= 0;
  param.m_table_sizing= 0;
  param.m_file_sizing= 0;
  param.m_file_handle_sizing= 0;
  param.m_socket_sizing= 0;
  param.m_events_waits_history_sizing= 0;
  param.m_events_waits_history_long_sizing= 0;
  param.m_setup_actor_sizing= 0;
  param.m_setup_object_sizing= 0;
  param.m_host_sizing= 0;
  param.m_user_sizing= 0;
  param.m_account_sizing= 0;
  param.m_stage_class_sizing= 0;
  param.m_events_stages_history_sizing= 0;
  param.m_events_stages_history_long_sizing= 0;
  param.m_statement_class_sizing= 0;
  param.m_events_statements_history_sizing= 0;
  param.m_events_statements_history_long_sizing= 0;
  param.m_session_connect_attrs_sizing= 0;

  init_event_name_sizing(& param);
  rc= init_instruments(& param);
  ok(rc == 1, "oom (mutex)");
  cleanup_instruments();

  param.m_enabled= true;
  param.m_mutex_class_sizing= 0;
  param.m_rwlock_class_sizing= 10;
  param.m_cond_class_sizing= 0;
  param.m_thread_class_sizing= 0;
  param.m_table_share_sizing= 0;
  param.m_file_class_sizing= 0;
  param.m_socket_class_sizing= 0;
  param.m_mutex_sizing= 0;
  param.m_rwlock_sizing= 1000;
  param.m_cond_sizing= 0;
  param.m_thread_sizing= 0;
  param.m_table_sizing= 0;
  param.m_file_sizing= 0;
  param.m_file_handle_sizing= 0;
  param.m_socket_sizing= 0;
  param.m_events_waits_history_sizing= 0;
  param.m_events_waits_history_long_sizing= 0;
  param.m_setup_actor_sizing= 0;
  param.m_setup_object_sizing= 0;
  param.m_host_sizing= 0;
  param.m_user_sizing= 0;
  param.m_account_sizing= 0;
  param.m_stage_class_sizing= 0;
  param.m_events_stages_history_sizing= 0;
  param.m_events_stages_history_long_sizing= 0;
  param.m_statement_class_sizing= 0;
  param.m_events_statements_history_sizing= 0;
  param.m_events_statements_history_long_sizing= 0;
  param.m_session_connect_attrs_sizing= 0;

  init_event_name_sizing(& param);
  rc= init_instruments(& param);
  ok(rc == 1, "oom (rwlock)");
  cleanup_instruments();

  param.m_enabled= true;
  param.m_mutex_class_sizing= 0;
  param.m_rwlock_class_sizing= 0;
  param.m_cond_class_sizing= 10;
  param.m_thread_class_sizing= 0;
  param.m_table_share_sizing= 0;
  param.m_file_class_sizing= 0;
  param.m_socket_class_sizing= 0;
  param.m_mutex_sizing= 0;
  param.m_rwlock_sizing= 0;
  param.m_cond_sizing= 1000;
  param.m_thread_sizing= 0;
  param.m_table_sizing= 0;
  param.m_file_sizing= 0;
  param.m_file_handle_sizing= 0;
  param.m_socket_sizing= 0;
  param.m_events_waits_history_sizing= 0;
  param.m_events_waits_history_long_sizing= 0;
  param.m_setup_actor_sizing= 0;
  param.m_setup_object_sizing= 0;
  param.m_host_sizing= 0;
  param.m_user_sizing= 0;
  param.m_account_sizing= 0;
  param.m_stage_class_sizing= 0;
  param.m_events_stages_history_sizing= 0;
  param.m_events_stages_history_long_sizing= 0;
  param.m_statement_class_sizing= 0;
  param.m_events_statements_history_sizing= 0;
  param.m_events_statements_history_long_sizing= 0;
  param.m_session_connect_attrs_sizing= 0;

  init_event_name_sizing(& param);
  rc= init_instruments(& param);
  ok(rc == 1, "oom (cond)");
  cleanup_instruments();

  param.m_enabled= true;
  param.m_mutex_class_sizing= 0;
  param.m_rwlock_class_sizing= 0;
  param.m_cond_class_sizing= 0;
  param.m_thread_class_sizing= 0;
  param.m_table_share_sizing= 0;
  param.m_file_class_sizing= 10;
  param.m_socket_class_sizing= 0;
  param.m_mutex_sizing= 0;
  param.m_rwlock_sizing= 0;
  param.m_cond_sizing= 0;
  param.m_thread_sizing= 0;
  param.m_table_sizing= 0;
  param.m_file_sizing= 1000;
  param.m_file_handle_sizing= 1000;
  param.m_socket_sizing= 0;
  param.m_events_waits_history_sizing= 0;
  param.m_events_waits_history_long_sizing= 0;
  param.m_setup_actor_sizing= 0;
  param.m_setup_object_sizing= 0;
  param.m_host_sizing= 0;
  param.m_user_sizing= 0;
  param.m_account_sizing= 0;
  param.m_stage_class_sizing= 0;
  param.m_events_stages_history_sizing= 0;
  param.m_events_stages_history_long_sizing= 0;
  param.m_statement_class_sizing= 0;
  param.m_events_statements_history_sizing= 0;
  param.m_events_statements_history_long_sizing= 0;
  param.m_session_connect_attrs_sizing= 0;

  init_event_name_sizing(& param);
  rc= init_instruments(& param);
  ok(rc == 1, "oom (file)");
  cleanup_instruments();

  param.m_enabled= true;
  param.m_mutex_class_sizing= 0;
  param.m_rwlock_class_sizing= 0;
  param.m_cond_class_sizing= 0;
  param.m_thread_class_sizing= 0;
  param.m_table_share_sizing= 0;
  param.m_file_class_sizing= 0;
  param.m_mutex_sizing= 0;
  param.m_rwlock_sizing= 0;
  param.m_cond_sizing= 0;
  param.m_thread_sizing= 0;
  param.m_table_sizing= 0;
  param.m_file_sizing= 0;
  param.m_file_handle_sizing= 1000;
  param.m_events_waits_history_sizing= 0;
  param.m_events_waits_history_long_sizing= 0;
  param.m_setup_actor_sizing= 0;
  param.m_setup_object_sizing= 0;
  param.m_host_sizing= 0;
  param.m_user_sizing= 0;
  param.m_account_sizing= 0;
  param.m_stage_class_sizing= 0;
  param.m_events_stages_history_sizing= 0;
  param.m_events_stages_history_long_sizing= 0;
  param.m_statement_class_sizing= 0;
  param.m_events_statements_history_sizing= 0;
  param.m_events_statements_history_long_sizing= 0;
  param.m_session_connect_attrs_sizing= 0;

  init_event_name_sizing(& param);
  rc= init_instruments(& param);
  ok(rc == 1, "oom (file handle)");
  cleanup_instruments();

  param.m_enabled= true;
  param.m_mutex_class_sizing= 0;
  param.m_rwlock_class_sizing= 0;
  param.m_cond_class_sizing= 0;
  param.m_thread_class_sizing= 0;
  param.m_table_share_sizing= 10;
  param.m_file_class_sizing= 0;
  param.m_socket_class_sizing= 0;
  param.m_mutex_sizing= 0;
  param.m_rwlock_sizing= 0;
  param.m_cond_sizing= 0;
  param.m_thread_sizing= 0;
  param.m_table_sizing= 1000;
  param.m_file_sizing= 0;
  param.m_file_handle_sizing= 0;
  param.m_socket_sizing= 0;
  param.m_events_waits_history_sizing= 0;
  param.m_events_waits_history_long_sizing= 0;
  param.m_setup_actor_sizing= 0;
  param.m_setup_object_sizing= 0;
  param.m_host_sizing= 0;
  param.m_user_sizing= 0;
  param.m_account_sizing= 0;
  param.m_stage_class_sizing= 0;
  param.m_events_stages_history_sizing= 0;
  param.m_events_stages_history_long_sizing= 0;
  param.m_statement_class_sizing= 0;
  param.m_events_statements_history_sizing= 0;
  param.m_events_statements_history_long_sizing= 0;
  param.m_session_connect_attrs_sizing= 0;

  init_event_name_sizing(& param);
  rc= init_instruments(& param);
  ok(rc == 1, "oom (table)");
  cleanup_instruments();

  param.m_enabled= true;
  param.m_mutex_class_sizing= 0;
  param.m_rwlock_class_sizing= 0;
  param.m_cond_class_sizing= 0;
  param.m_thread_class_sizing= 10;
  param.m_table_share_sizing= 0;
  param.m_file_class_sizing= 0;
  param.m_socket_class_sizing= 0;
  param.m_mutex_sizing= 0;
  param.m_rwlock_sizing= 0;
  param.m_cond_sizing= 0;
  param.m_thread_sizing= 1000;
  param.m_table_sizing= 0;
  param.m_file_sizing= 0;
  param.m_file_handle_sizing= 0;
  param.m_socket_sizing= 0;
  param.m_events_waits_history_sizing= 0;
  param.m_events_waits_history_long_sizing= 0;
  param.m_setup_actor_sizing= 0;
  param.m_setup_object_sizing= 0;
  param.m_host_sizing= 0;
  param.m_user_sizing= 0;
  param.m_account_sizing= 0;
  param.m_stage_class_sizing= 0;
  param.m_events_stages_history_sizing= 0;
  param.m_events_stages_history_long_sizing= 0;
  param.m_statement_class_sizing= 0;
  param.m_events_statements_history_sizing= 0;
  param.m_events_statements_history_long_sizing= 0;
  param.m_session_connect_attrs_sizing= 0;

  init_event_name_sizing(& param);
  rc= init_instruments(& param);
  ok(rc == 1, "oom (thread)");
  cleanup_instruments();

  stub_alloc_always_fails= false;

  param.m_enabled= true;
  param.m_mutex_class_sizing= 0;
  param.m_rwlock_class_sizing= 0;
  param.m_cond_class_sizing= 0;
  param.m_thread_class_sizing= 10;
  param.m_table_share_sizing= 0;
  param.m_file_class_sizing= 0;
  param.m_socket_class_sizing= 0;
  param.m_mutex_sizing= 0;
  param.m_rwlock_sizing= 0;
  param.m_cond_sizing= 0;
  param.m_thread_sizing= 1000;
  param.m_table_sizing= 0;
  param.m_file_sizing= 0;
  param.m_file_handle_sizing= 0;
  param.m_socket_sizing= 0;
  param.m_events_waits_history_sizing= 10;
  param.m_events_waits_history_long_sizing= 0;
  param.m_setup_actor_sizing= 0;
  param.m_setup_object_sizing= 0;
  param.m_host_sizing= 0;
  param.m_user_sizing= 0;
  param.m_account_sizing= 0;
  param.m_stage_class_sizing= 0;
  param.m_events_stages_history_sizing= 0;
  param.m_events_stages_history_long_sizing= 0;
  param.m_statement_class_sizing= 0;
  param.m_events_statements_history_sizing= 0;
  param.m_events_statements_history_long_sizing= 0;
  param.m_session_connect_attrs_sizing= 0;

  stub_alloc_fails_after_count= 2;
  init_event_name_sizing(& param);
  rc= init_instruments(& param);
  ok(rc == 1, "oom (thread waits history sizing)");
  cleanup_instruments();

  param.m_enabled= true;
  param.m_mutex_class_sizing= 50;
  param.m_rwlock_class_sizing= 50;
  param.m_cond_class_sizing= 50;
  param.m_thread_class_sizing= 10;
  param.m_table_share_sizing= 0;
  param.m_file_class_sizing= 50;
  param.m_socket_class_sizing= 0;
  param.m_mutex_sizing= 0;
  param.m_rwlock_sizing= 0;
  param.m_cond_sizing= 0;
  param.m_thread_sizing= 1000;
  param.m_table_sizing= 0;
  param.m_file_sizing= 0;
  param.m_file_handle_sizing= 0;
  param.m_socket_sizing= 0;
  param.m_events_waits_history_sizing= 0;
  param.m_events_waits_history_long_sizing= 0;
  param.m_setup_actor_sizing= 0;
  param.m_setup_object_sizing= 0;
  param.m_stage_class_sizing= 0;
  param.m_events_stages_history_sizing= 0;
  param.m_events_stages_history_long_sizing= 0;
  param.m_statement_class_sizing= 0;
  param.m_events_statements_history_sizing= 0;
  param.m_events_statements_history_long_sizing= 0;
  param.m_session_connect_attrs_sizing= 0;

  stub_alloc_fails_after_count= 2;
  init_event_name_sizing(& param);
  rc= init_instruments(& param);
  ok(rc == 1, "oom (per thread wait)");

  cleanup_sync_class();
  cleanup_thread_class();
  cleanup_file_class();
  cleanup_instruments();

  param.m_enabled= true;
  param.m_mutex_class_sizing= 0;
  param.m_rwlock_class_sizing= 0;
  param.m_cond_class_sizing= 0;
  param.m_thread_class_sizing= 0;
  param.m_table_share_sizing= 0;
  param.m_file_class_sizing= 0;
  param.m_socket_class_sizing= 10;
  param.m_mutex_sizing= 0;
  param.m_rwlock_sizing= 0;
  param.m_cond_sizing= 0;
  param.m_thread_sizing= 0;
  param.m_table_sizing= 0;
  param.m_file_sizing= 0;
  param.m_file_handle_sizing= 0;
  param.m_socket_sizing= 1000;
  param.m_events_waits_history_sizing= 0;
  param.m_events_waits_history_long_sizing= 0;
  param.m_setup_actor_sizing= 0;
  param.m_setup_object_sizing= 0;

  init_event_name_sizing(& param);
  rc= init_instruments(& param);
  ok(rc == 1, "oom (socket)");

  cleanup_instruments();

  param.m_host_sizing= 0;
  param.m_user_sizing= 0;
  param.m_account_sizing= 0;
  param.m_stage_class_sizing= 0;
  param.m_events_stages_history_sizing= 0;
  param.m_events_stages_history_long_sizing= 0;
  param.m_statement_class_sizing= 0;
  param.m_events_statements_history_sizing= 0;
  param.m_events_statements_history_long_sizing= 0;
  param.m_session_connect_attrs_sizing= 0;

  stub_alloc_fails_after_count= 1;
  init_event_name_sizing(& param);
  rc= init_instruments(& param);
  ok(rc == 1, "oom (per thread waits)");
  cleanup_instruments();

  param.m_enabled= true;
  param.m_mutex_class_sizing= 0;
  param.m_rwlock_class_sizing= 0;
  param.m_cond_class_sizing= 0;
  param.m_thread_class_sizing= 10;
  param.m_table_share_sizing= 0;
  param.m_file_class_sizing= 0;
  param.m_mutex_sizing= 0;
  param.m_rwlock_sizing= 0;
  param.m_cond_sizing= 0;
  param.m_thread_sizing= 1000;
  param.m_table_sizing= 0;
  param.m_file_sizing= 0;
  param.m_file_handle_sizing= 0;
  param.m_events_waits_history_sizing= 0;
  param.m_events_waits_history_long_sizing= 0;
  param.m_setup_actor_sizing= 0;
  param.m_setup_object_sizing= 0;
  param.m_host_sizing= 0;
  param.m_user_sizing= 0;
  param.m_account_sizing= 0;
  param.m_stage_class_sizing= 0;
  param.m_events_stages_history_sizing= 10;
  param.m_events_stages_history_long_sizing= 0;
  param.m_statement_class_sizing= 0;
  param.m_events_statements_history_sizing= 0;
  param.m_events_statements_history_long_sizing= 0;
  param.m_session_connect_attrs_sizing= 0;

  stub_alloc_fails_after_count= 3;
  init_event_name_sizing(& param);
  rc= init_instruments(& param);
  ok(rc == 1, "oom (thread stages history sizing)");

  cleanup_thread_class();
  cleanup_instruments();

  param.m_enabled= true;
  param.m_mutex_class_sizing= 0;
  param.m_rwlock_class_sizing= 0;
  param.m_cond_class_sizing= 0;
  param.m_thread_class_sizing= 10;
  param.m_table_share_sizing= 0;
  param.m_file_class_sizing= 0;
  param.m_mutex_sizing= 0;
  param.m_rwlock_sizing= 0;
  param.m_cond_sizing= 0;
  param.m_thread_sizing= 1000;
  param.m_table_sizing= 0;
  param.m_file_sizing= 0;
  param.m_file_handle_sizing= 0;
  param.m_events_waits_history_sizing= 0;
  param.m_events_waits_history_long_sizing= 0;
  param.m_setup_actor_sizing= 0;
  param.m_setup_object_sizing= 0;
  param.m_host_sizing= 0;
  param.m_user_sizing= 0;
  param.m_account_sizing= 0;
  param.m_stage_class_sizing= 50;
  param.m_events_stages_history_sizing= 0;
  param.m_events_stages_history_long_sizing= 0;
  param.m_statement_class_sizing= 0;
  param.m_events_statements_history_sizing= 0;
  param.m_events_statements_history_long_sizing= 0;
  param.m_session_connect_attrs_sizing= 0;

  stub_alloc_fails_after_count= 2;
  init_event_name_sizing(& param);
  rc= init_instruments(& param);
  ok(rc == 1, "oom (per thread stages)");
  
  cleanup_stage_class();
  cleanup_thread_class();
  cleanup_instruments();

  param.m_enabled= true;
  param.m_mutex_class_sizing= 0;
  param.m_rwlock_class_sizing= 0;
  param.m_cond_class_sizing= 0;
  param.m_thread_class_sizing= 10;
  param.m_table_share_sizing= 0;
  param.m_file_class_sizing= 0;
  param.m_mutex_sizing= 0;
  param.m_rwlock_sizing= 0;
  param.m_cond_sizing= 0;
  param.m_thread_sizing= 1000;
  param.m_table_sizing= 0;
  param.m_file_sizing= 0;
  param.m_file_handle_sizing= 0;
  param.m_events_waits_history_sizing= 0;
  param.m_events_waits_history_long_sizing= 0;
  param.m_setup_actor_sizing= 0;
  param.m_setup_object_sizing= 0;
  param.m_host_sizing= 0;
  param.m_user_sizing= 0;
  param.m_account_sizing= 0;
  param.m_stage_class_sizing= 0;
  param.m_events_stages_history_sizing= 0;
  param.m_events_stages_history_long_sizing= 0;
  param.m_statement_class_sizing= 0;
  param.m_events_statements_history_sizing= 10;
  param.m_events_statements_history_long_sizing= 0;
  param.m_session_connect_attrs_sizing= 0;

  stub_alloc_fails_after_count= 2;
  init_event_name_sizing(& param);
  rc= init_instruments(& param);
  ok(rc == 1, "oom (thread statements history sizing)");

  cleanup_thread_class();
  cleanup_instruments();

  param.m_enabled= true;
  param.m_mutex_class_sizing= 0;
  param.m_rwlock_class_sizing= 0;
  param.m_cond_class_sizing= 0;
  param.m_thread_class_sizing= 10;
  param.m_table_share_sizing= 0;
  param.m_file_class_sizing= 0;
  param.m_mutex_sizing= 0;
  param.m_rwlock_sizing= 0;
  param.m_cond_sizing= 0;
  param.m_thread_sizing= 1000;
  param.m_table_sizing= 0;
  param.m_file_sizing= 0;
  param.m_file_handle_sizing= 0;
  param.m_events_waits_history_sizing= 0;
  param.m_events_waits_history_long_sizing= 0;
  param.m_setup_actor_sizing= 0;
  param.m_setup_object_sizing= 0;
  param.m_host_sizing= 0;
  param.m_user_sizing= 0;
  param.m_account_sizing= 0;
  param.m_stage_class_sizing= 0;
  param.m_events_stages_history_sizing= 0;
  param.m_events_stages_history_long_sizing= 0;
  param.m_statement_class_sizing= 50;
  param.m_events_statements_history_sizing= 0;
  param.m_events_statements_history_long_sizing= 0;
  param.m_session_connect_attrs_sizing= 0;

  stub_alloc_fails_after_count= 2;
  init_event_name_sizing(& param);
  rc= init_instruments(& param);
  ok(rc == 1, "oom (per thread statements)");

  cleanup_statement_class();
  cleanup_thread_class();
  cleanup_instruments();

  param.m_enabled= true;
  param.m_mutex_class_sizing= 10;
  param.m_rwlock_class_sizing= 0;
  param.m_cond_class_sizing= 0;
  param.m_thread_class_sizing= 0;
  param.m_table_share_sizing= 0;
  param.m_file_class_sizing= 0;
  param.m_mutex_sizing= 0;
  param.m_rwlock_sizing= 0;
  param.m_cond_sizing= 0;
  param.m_thread_sizing= 0;
  param.m_table_sizing= 0;
  param.m_file_sizing= 0;
  param.m_file_handle_sizing= 0;
  param.m_events_waits_history_sizing= 0;
  param.m_events_waits_history_long_sizing= 0;
  param.m_setup_actor_sizing= 0;
  param.m_setup_object_sizing= 0;
  param.m_host_sizing= 0;
  param.m_user_sizing= 0;
  param.m_account_sizing= 0;
  param.m_stage_class_sizing= 0;
  param.m_events_stages_history_sizing= 0;
  param.m_events_stages_history_long_sizing= 0;
  param.m_statement_class_sizing= 0;
  param.m_events_statements_history_sizing= 0;
  param.m_events_statements_history_long_sizing= 0;
  param.m_session_connect_attrs_sizing= 0;

  stub_alloc_fails_after_count= 1;
  init_event_name_sizing(& param);
  rc= init_instruments(& param);
  ok(rc == 1, "oom (global waits)");

  cleanup_sync_class();
  cleanup_instruments();

  param.m_enabled= true;
  param.m_mutex_class_sizing= 10;
  param.m_rwlock_class_sizing= 0;
  param.m_cond_class_sizing= 0;
  param.m_thread_class_sizing= 0;
  param.m_table_share_sizing= 0;
  param.m_file_class_sizing= 0;
  param.m_mutex_sizing= 0;
  param.m_rwlock_sizing= 0;
  param.m_cond_sizing= 0;
  param.m_thread_sizing= 0;
  param.m_table_sizing= 0;
  param.m_file_sizing= 0;
  param.m_file_handle_sizing= 0;
  param.m_events_waits_history_sizing= 0;
  param.m_events_waits_history_long_sizing= 0;
  param.m_setup_actor_sizing= 0;
  param.m_setup_object_sizing= 0;
  param.m_host_sizing= 0;
  param.m_user_sizing= 0;
  param.m_account_sizing= 0;
  param.m_stage_class_sizing= 20;
  param.m_events_stages_history_sizing= 0;
  param.m_events_stages_history_long_sizing= 0;
  param.m_statement_class_sizing= 0;
  param.m_events_statements_history_sizing= 0;
  param.m_events_statements_history_long_sizing= 0;
  param.m_session_connect_attrs_sizing= 0;

  stub_alloc_fails_after_count= 3;
  init_event_name_sizing(& param);
  rc= init_stage_class(param.m_stage_class_sizing);
  ok(rc == 0, "init stage class");
  rc= init_instruments(& param);
  ok(rc == 1, "oom (global stages)");

  cleanup_sync_class();
  cleanup_stage_class();
  cleanup_instruments();

  param.m_enabled= true;
  param.m_mutex_class_sizing= 10;
  param.m_rwlock_class_sizing= 0;
  param.m_cond_class_sizing= 0;
  param.m_thread_class_sizing= 0;
  param.m_table_share_sizing= 0;
  param.m_file_class_sizing= 0;
  param.m_mutex_sizing= 0;
  param.m_rwlock_sizing= 0;
  param.m_cond_sizing= 0;
  param.m_thread_sizing= 0;
  param.m_table_sizing= 0;
  param.m_file_sizing= 0;
  param.m_file_handle_sizing= 0;
  param.m_events_waits_history_sizing= 0;
  param.m_events_waits_history_long_sizing= 0;
  param.m_setup_actor_sizing= 0;
  param.m_setup_object_sizing= 0;
  param.m_host_sizing= 0;
  param.m_user_sizing= 0;
  param.m_account_sizing= 0;
  param.m_stage_class_sizing= 0;
  param.m_events_stages_history_sizing= 0;
  param.m_events_stages_history_long_sizing= 0;
  param.m_statement_class_sizing= 20;
  param.m_events_statements_history_sizing= 0;
  param.m_events_statements_history_long_sizing= 0;
  param.m_session_connect_attrs_sizing= 0;

  stub_alloc_fails_after_count= 3;
  init_event_name_sizing(& param);
  rc= init_statement_class(param.m_statement_class_sizing);
  ok(rc == 0, "init statement class");
  rc= init_instruments(& param);
  ok(rc == 1, "oom (global statements)");

  cleanup_sync_class();
  cleanup_statement_class();
  cleanup_instruments();
}

void do_all_tests()
{
  test_oom();
}

int main(int argc, char **argv)
{
  plan(20);
  MY_INIT(argv[0]);
  do_all_tests();
  my_end(0);
  return (exit_status());
}

