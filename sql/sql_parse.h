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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#ifndef SQL_PARSE_INCLUDED
#define SQL_PARSE_INCLUDED

#include "sql_acl.h"                            /* GLOBAL_ACLS */

class Comp_creator;
class Item;
class Object_creation_ctx;
class Parser_state;
struct TABLE_LIST;
class THD;
class Table_ident;
struct LEX;

enum enum_mysql_completiontype {
  ROLLBACK_RELEASE=-2, ROLLBACK=1,  ROLLBACK_AND_CHAIN=7,
  COMMIT_RELEASE=-1,   COMMIT=0,    COMMIT_AND_CHAIN=6
};

extern "C" int path_starts_from_data_home_dir(const char *dir);
int test_if_data_home_dir(const char *dir);
int error_if_data_home_dir(const char *path, const char *what);
my_bool net_allocate_new_packet(NET *net, void *thd, uint my_flags);

bool multi_update_precheck(THD *thd, TABLE_LIST *tables);
bool multi_delete_precheck(THD *thd, TABLE_LIST *tables);
int mysql_multi_update_prepare(THD *thd);
int mysql_multi_delete_prepare(THD *thd);
bool mysql_insert_select_prepare(THD *thd);
bool update_precheck(THD *thd, TABLE_LIST *tables);
bool delete_precheck(THD *thd, TABLE_LIST *tables);
bool insert_precheck(THD *thd, TABLE_LIST *tables);
bool create_table_precheck(THD *thd, TABLE_LIST *tables,
                           TABLE_LIST *create_table);
bool check_fk_parent_table_access(THD *thd,
                                  HA_CREATE_INFO *create_info,
                                  Alter_info *alter_info,
                                  const char* create_db);

bool parse_sql(THD *thd, Parser_state *parser_state,
               Object_creation_ctx *creation_ctx, bool do_pfs_digest=false);

void free_items(Item *item);
void cleanup_items(Item *item);

Comp_creator *comp_eq_creator(bool invert);
Comp_creator *comp_ge_creator(bool invert);
Comp_creator *comp_gt_creator(bool invert);
Comp_creator *comp_le_creator(bool invert);
Comp_creator *comp_lt_creator(bool invert);
Comp_creator *comp_ne_creator(bool invert);

int prepare_schema_table(THD *thd, LEX *lex, Table_ident *table_ident,
                         enum enum_schema_tables schema_table_idx);
void get_default_definer(THD *thd, LEX_USER *definer, bool role);
LEX_USER *create_default_definer(THD *thd, bool role);
LEX_USER *create_definer(THD *thd, LEX_CSTRING *user_name, LEX_CSTRING *host_name);
LEX_USER *get_current_user(THD *thd, LEX_USER *user, bool lock=true);
bool sp_process_definer(THD *thd);
bool check_string_byte_length(const LEX_CSTRING *str, uint err_msg,
                              size_t max_byte_length);
bool check_string_char_length(const LEX_CSTRING *str, uint err_msg,
                              size_t max_char_length, CHARSET_INFO *cs,
                              bool no_error);
bool check_ident_length(const LEX_CSTRING *ident);
CHARSET_INFO* merge_charset_and_collation(CHARSET_INFO *cs, CHARSET_INFO *cl);
CHARSET_INFO *find_bin_collation(CHARSET_INFO *cs);
bool check_host_name(LEX_CSTRING *str);
bool check_identifier_name(LEX_CSTRING *str, uint max_char_length,
                           uint err_code, const char *param_for_err_msg);
bool mysql_test_parse_for_slave(THD *thd,char *inBuf,uint length);
bool sqlcom_can_generate_row_events(const THD *thd);
bool stmt_causes_implicit_commit(THD *thd, uint mask);
bool is_update_query(enum enum_sql_command command);
bool is_log_table_write_query(enum enum_sql_command command);
bool alloc_query(THD *thd, const char *packet, size_t packet_length);
void mysql_init_select(LEX *lex);
void mysql_parse(THD *thd, char *rawbuf, uint length,
                 Parser_state *parser_state, bool is_com_multi,
                 bool is_next_command);
bool mysql_new_select(LEX *lex, bool move_down, SELECT_LEX *sel);
void create_select_for_variable(THD *thd, LEX_CSTRING *var_name);
void create_table_set_open_action_and_adjust_tables(LEX *lex);
void mysql_init_multi_delete(LEX *lex);
bool multi_delete_set_locks_and_link_aux_tables(LEX *lex);
void create_table_set_open_action_and_adjust_tables(LEX *lex);
pthread_handler_t handle_bootstrap(void *arg);
int mysql_execute_command(THD *thd);
bool do_command(THD *thd);
void do_handle_bootstrap(THD *thd);
bool dispatch_command(enum enum_server_command command, THD *thd,
		      char* packet, uint packet_length,
                      bool is_com_multi, bool is_next_command);
void log_slow_statement(THD *thd);
bool append_file_to_dir(THD *thd, const char **filename_ptr,
                        const LEX_CSTRING *table_name);
void execute_init_command(THD *thd, LEX_STRING *init_command,
                          mysql_rwlock_t *var_lock);
bool add_to_list(THD *thd, SQL_I_List<ORDER> &list, Item *group, bool asc);
void add_join_on(THD *thd, TABLE_LIST *b, Item *expr);
void add_join_natural(TABLE_LIST *a,TABLE_LIST *b,List<String> *using_fields,
                      SELECT_LEX *lex);
bool add_proc_to_list(THD *thd, Item *item);
bool push_new_name_resolution_context(THD *thd,
                                      TABLE_LIST *left_op,
                                      TABLE_LIST *right_op);
void init_update_queries(void);
Item *normalize_cond(THD *thd, Item *cond);
Item *negate_expression(THD *thd, Item *expr);
bool check_stack_overrun(THD *thd, long margin, uchar *dummy);

/* Variables */

extern const char* any_db;
extern uint sql_command_flags[];
extern uint server_command_flags[];
extern const LEX_CSTRING command_name[];
extern uint server_command_flags[];

/* Inline functions */
inline bool check_identifier_name(LEX_CSTRING *str, uint err_code)
{
  return check_identifier_name(str, NAME_CHAR_LEN, err_code, "");
}

inline bool check_identifier_name(LEX_CSTRING *str)
{
  return check_identifier_name(str, NAME_CHAR_LEN, 0, "");
}

#ifndef NO_EMBEDDED_ACCESS_CHECKS
bool check_one_table_access(THD *thd, ulong privilege, TABLE_LIST *tables);
bool check_single_table_access(THD *thd, ulong privilege,
			   TABLE_LIST *tables, bool no_errors);
bool check_routine_access(THD *thd,ulong want_access,
                          const LEX_CSTRING *db,
                          const LEX_CSTRING *name,
                          const Sp_handler *sph, bool no_errors);
bool check_some_access(THD *thd, ulong want_access, TABLE_LIST *table);
bool check_some_routine_access(THD *thd, const char *db, const char *name,
                               const Sp_handler *sph);
bool check_table_access(THD *thd, ulong requirements,TABLE_LIST *tables,
                        bool any_combination_of_privileges_will_do,
                        uint number,
                        bool no_errors);
#else
inline bool check_one_table_access(THD *thd, ulong privilege, TABLE_LIST *tables)
{ return false; }
inline bool check_single_table_access(THD *thd, ulong privilege,
			   TABLE_LIST *tables, bool no_errors)
{ return false; }
inline bool check_routine_access(THD *thd,ulong want_access,
                                 const LEX_CSTRING *db,
                                 const LEX_CSTRING *name,
                                 const Sp_handler *sph, bool no_errors)
{ return false; }
inline bool check_some_access(THD *thd, ulong want_access, TABLE_LIST *table)
{
  table->grant.privilege= want_access;
  return false;
}
inline bool check_some_routine_access(THD *thd, const char *db,
                                      const char *name,
                                      const Sp_handler *sph)
{ return false; }
inline bool
check_table_access(THD *thd, ulong requirements,TABLE_LIST *tables,
                   bool any_combination_of_privileges_will_do,
                   uint number,
                   bool no_errors)
{ return false; }
#endif /*NO_EMBEDDED_ACCESS_CHECKS*/

#endif /* SQL_PARSE_INCLUDED */
