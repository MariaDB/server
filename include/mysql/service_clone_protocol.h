/* Copyright (c) 2018, 2024, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with
   the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef CLONE_PROTOCOL_SERVICE
#define CLONE_PROTOCOL_SERVICE

/**
  @file
  This service provides functions for clone plugin to
  connect and interact with remote server's clone plugin
  counterpart.

*/

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MYSQL_ABI_CHECK
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#endif /* MYSQL_ABI_CHECK */

// #include "mysql_com_server.h"

extern struct clone_protocol_service_st {
/**
  Start and set session and statement key form current thread
  @param[in,out] thd  server session THD
  @param[in]     thread_key  PSI key for thread
  @param[in]     statement_key  PSI Key for statement
  @param[in]	 thd_name	thread name based on PSI key
*/
  MYSQL_THD (*start_statement_fn)(MYSQL_THD thd, unsigned int thread_key,
                                  unsigned int statement_key,
				  const char* thd_name);
/**
  Finish statement and session
  @param[in,out]  thd    server session THD
*/
  void (*finish_statement_fn)(MYSQL_THD thd);

/**
  Get all character set and collations
  @param[in,out]  thd        server session THD
  @param[out]     char_sets  all character set collations
  @return error code.
*/
  int (*get_charsets_fn)(MYSQL_THD thd, void *char_sets);

/**
  Get system configuration parameter values.
  @param[in,out]  thd        server session THD
  @param[in,out]  configs    a list of configuration key value pair
                             keys are input and values are output
  @return error code.
*/
  int (*get_configs_fn)(MYSQL_THD thd, void *configs);

/**
  Get error number and message.
  @param[in,out] thd         local session THD
  @param[out]    err_num     error number
  @param[out]    err_mesg    error message text
*/
  void (*get_error_fn)(MYSQL_THD thd, uint32_t *err_num,
                       const char **err_mesg);

/**
  Get command from client
  @param[in,out] thd            server session THD
  @param[out]    command        remote command
  @param[out]    com_buffer     data following command
  @param[out]    buffer_length  data length
  @return error code.
*/
  int (*get_command_fn)(MYSQL_THD thd, unsigned char *command,
                        unsigned char **com_buffer, size_t *buffer_length);

/**
  Send response to client.
  @param[in,out] thd     server session THD
  @param[in]     secure  needs to be sent over secure connection
  @param[in]     packet  response packet
  @param[in]     length  packet length
  @return error code.
*/
  int (*send_response_fn)(MYSQL_THD thd, bool secure, unsigned char *packet,
                          size_t length);

/**
  Set server to desired backup stage
  @param[in,out] thd	server session THD
  @param[in]	 stage	backup stage
  @return error code.
*/
  int (*set_backup_stage_fn)(MYSQL_THD thd, unsigned char stage);
/**
  Set backup lock on the given table
  @param thd server session thread
  @param db  database name
  @param tbl table name
  @return error code.
*/
  int (*backup_lock_fn)(MYSQL_THD thd, const char *db, const char *tbl);
/**
  Unlock the backup lock on the table
  @param thd server session thread
  @return error code
*/
  int (*backup_unlock_fn)(MYSQL_THD thd);

} *clone_protocol_service;

#ifdef MYSQL_DYNAMIC_PLUGIN
#define clone_start_statement(thd, thd_key, stmt_key, thd_name) \
  (clone_protocol_service->start_statement_fn((thd), (thd_key), (stmt_key), (thd_name)))

#define clone_finish_statement(thd) \
  (clone_protocol_service->finish_statement_fn(thd))

 #define clone_get_charsets(thd, char_sets) \
   (clone_protocol_service->get_charsets_fn((thd), (char_sets)))

#define clone_get_configs(thd, configs) \
   (clone_protocol_service->get_configs_fn((thd), (configs)))

#define clone_get_error(thd, err_num, err_mesg) \
  (clone_protocol_service->get_error_fn((thd), (err_num), (err_mesg)))

#define clone_get_command(thd, command, com_buffer, buffer_length) \
  (clone_protocol_service->get_command_fn((thd), (command), (com_buffer), \
  (buffer_length)))

#define clone_send_response(thd, secure, packet, length) \
  (clone_protocol_service->send_response_fn((thd), (secure), (packet), (length)))

#define clone_set_backup_stage(thd, stage) \
   (clone_protocol_service->set_backup_stage_fn((thd), (stage)))

#define clone_backup_lock(thd, db, tbl) \
   (clone_protocol_service->backup_lock_fn((thd), (db), (tbl)))

#define clone_backup_unlock(thd) \
   (clone_protocol_service->backup_unlock_fn((thd)))
#else
  MYSQL_THD clone_start_statement(MYSQL_THD thd, unsigned int thread_key,
                                  unsigned int statement_key,
				  const char* thd_name);
  void clone_finish_statement(MYSQL_THD thd);

  int clone_get_charsets(MYSQL_THD thd, void *char_sets);

  int clone_get_configs(MYSQL_THD thd, void *configs);

  void clone_get_error(MYSQL_THD thd, uint32_t *err_num,
                       const char **err_mesg);

  int clone_get_command(MYSQL_THD thd, unsigned char *command,
                        unsigned char **com_buffer, size_t *buffer_length);

  int clone_send_response(MYSQL_THD thd, bool secure, unsigned char *packet,
                          size_t length);

  int clone_set_backup_stage(MYSQL_THD thd, unsigned char stage);

  int clone_backup_lock(MYSQL_THD thd, const char *db, const char *tbl);
  int clone_backup_unlock(MYSQL_THD thd);
#endif

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* CLONE_PROTOCOL_SERVICE */
