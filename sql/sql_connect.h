/* Copyright (c) 2006, 2011, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA */

#ifndef SQL_CONNECT_INCLUDED
#define SQL_CONNECT_INCLUDED

#include "my_sys.h"                          /* pthread_handler_t */
#include "mysql_com.h"                         /* enum_server_command */
#include "structs.h"
#include <mysql/psi/mysql_socket.h>
#include <hash.h>

/*
  Object to hold connect information to be given to the newly created thread
*/

struct scheduler_functions;

class CONNECT : public ilink {
public:
  /* To be copied to THD */
  Vio *vio;                           /* Copied to THD with my_net_init() */
  const char *host;
  scheduler_functions *scheduler;
  my_thread_id thread_id;
  pthread_t    real_id;
  bool extra_port;

  /* Own variables */
  bool thread_count_incremented;
  ulonglong    prior_thr_create_utime;

  CONNECT()
    :vio(0), host(0), scheduler(thread_scheduler), thread_id(0), real_id(0),
    extra_port(0),
    thread_count_incremented(0), prior_thr_create_utime(0)
  {
  };
  ~CONNECT();
  void close_and_delete();
  void close_with_error(uint sql_errno,
                        const char *message, uint close_error);
  THD *create_thd(THD *thd);
};


class THD;
typedef struct st_lex_user LEX_USER;
typedef struct user_conn USER_CONN;

void init_max_user_conn(void);
void init_global_user_stats(void);
void init_global_table_stats(void);
void init_global_index_stats(void);
void init_global_client_stats(void);
void free_max_user_conn(void);
void free_global_user_stats(void);
void free_global_table_stats(void);
void free_global_index_stats(void);
void free_global_client_stats(void);

pthread_handler_t handle_one_connection(void *arg);
void do_handle_one_connection(CONNECT *connect);
bool init_new_connection_handler_thread();
void reset_mqh(LEX_USER *lu, bool get_them);
bool check_mqh(THD *thd, uint check_command);
void time_out_user_resource_limits(THD *thd, USER_CONN *uc);
#ifndef NO_EMBEDDED_ACCESS_CHECKS
void decrease_user_connections(USER_CONN *uc);
#else
#define decrease_user_connections(X) do { } while(0)       /* nothing */
#endif
bool thd_init_client_charset(THD *thd, uint cs_number);
bool setup_connection_thread_globals(THD *thd);
bool thd_prepare_connection(THD *thd);
bool thd_is_connection_alive(THD *thd);

bool login_connection(THD *thd);
void prepare_new_connection_state(THD* thd);
void end_connection(THD *thd);
void update_global_user_stats(THD* thd, bool create_user, time_t now);
int get_or_create_user_conn(THD *thd, const char *user,
                            const char *host, const USER_RESOURCES *mqh);
int check_for_max_user_connections(THD *thd, USER_CONN *uc);

extern HASH global_user_stats;
extern HASH global_client_stats;
extern HASH global_table_stats;
extern HASH global_index_stats;

extern mysql_mutex_t LOCK_global_user_client_stats;
extern mysql_mutex_t LOCK_global_table_stats;
extern mysql_mutex_t LOCK_global_index_stats;
extern mysql_mutex_t LOCK_stats;

#endif /* SQL_CONNECT_INCLUDED */
