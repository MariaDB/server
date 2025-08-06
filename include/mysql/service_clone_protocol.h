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

typedef struct st_net_server NET_SERVER;
typedef struct st_mysql MYSQL;
typedef struct st_mysql_socket MYSQL_SOCKET;

// #include "mysql_com_server.h"

/** Connection parameters including SSL */
typedef struct mysql_clone_ssl_context_t {
  /** Clone ssl mode. Same as mysql client --ssl-mode */
  int m_ssl_mode;
  /** Clone ssl private key. Same as mysql client --ssl-key */
  const char *m_ssl_key;
  /** Clone ssl certificate. Same as mysql client --ssl-cert */
  const char *m_ssl_cert;
  /** Clone ssl certificate authority. Same as mysql client --ssl-ca */
  const char *m_ssl_ca;

  /** Enable network compression. */
  bool m_enable_compression;
  NET_SERVER *m_server_extn;
} mysql_clone_ssl_context;

extern struct clone_protocol_service_st {
/**
  Start and set session and statement key form current thread
  @param[in,out] thd  server session THD
  @param[in]     thread_key  PSI key for thread
  @param[in]     statement_key  PSI Key for statement
*/
  MYSQL_THD (*start_statement_fn)(MYSQL_THD thd, unsigned int thread_key,
                                  unsigned int statement_key);
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
  Check if all characters sets are supported by server
  @param[in,out]  thd        server session THD
  @param[in]      char_sets  all character set collations to validate
  @return error code.
*/
  int (*validate_charsets_fn)(MYSQL_THD thd, void *char_sets);

/**
  Get system configuration parameter values.
  @param[in,out]  thd        server session THD
  @param[in,out]  configs    a list of configuration key value pair
                             keys are input and values are output
  @return error code.
*/
  int (*get_configs_fn)(MYSQL_THD thd, void *configs);

/**
  Check if configuration parameter values match
  @param[in,out]  thd        server session THD
  @param[in]      configs    a list of configuration key value pair
  @return error code.
*/
  int (*validate_configs_fn)(MYSQL_THD thd, void *configs);

/**
  Connect to a remote server and switch to clone protocol
  @param[in,out] thd      server session THD
  @param[in]     host     host name to connect to
  @param[in]     port     port number to connect to
  @param[in]     user     user name on remote host
  @param[in]     passwd   password for the user
  @param[in]     ssl_ctx  client ssl context
  @param[out]    socket   Network socket for the connection

  @return Connection object if successful.
*/
  MYSQL* (*connect_fn)(MYSQL_THD thd, const char *host, uint32_t port,
                             const char *user, const char *passwd,
                             mysql_clone_ssl_context *ssl_ctx,
                             MYSQL_SOCKET *socket);
/**
  Execute clone command on remote server
  @param[in,out] thd            local session THD
  @param[in,out] connection     connection object
  @param[in]     set_active     set socket active for current THD
  @param[in]     command        remote command
  @param[in]     com_buffer     data following command
  @param[in]     buffer_length  data length
  @return error code.
*/
  int (*send_command_fn)(MYSQL_THD thd, MYSQL *connection, bool set_active,
                         unsigned char command, unsigned char *com_buffer,
                         size_t buffer_length);

/**
  Get response from remote server
  @param[in,out] thd            local session THD
  @param[in,out] connection     connection object
  @param[in]     set_active     set socket active for current THD
  @param[in]     timeout        timeout in seconds
  @param[out]    packet         response packet
  @param[out]    length         packet length
  @param[out]    net_length     network data length for compressed data
  @return error code.
*/
  int (*get_response_fn)(MYSQL_THD thd, MYSQL *connection, bool set_active,
                         uint32_t timeout, unsigned char **packet,
                         size_t *length, size_t *net_length);

/**
  Kill a remote connection
  @param[in,out] connection        connection object
  @param[in]     kill_connection   connection to kill
  @return error code.
*/
  int (*kill_fn)(MYSQL *connection, MYSQL *kill_connection);

/**
  Disconnect from a remote server
  @param[in,out] thd         local session THD
  @param[in,out] connection  connection object
  @param[in]     is_fatal    if closing after fatal error
  @param[in]     clear_error clear any earlier error in session
*/
  void (*disconnect_fn)(MYSQL_THD thd, MYSQL *connection, bool is_fatal,
                        bool clear_error);
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
  Send error to client
  @param[in,out] thd           server session THD
  @param[in]     err_cmd       error response command
  @param[in]     is_fatal      if fatal error
  @return error code.
*/
  int (*send_error_fn)(MYSQL_THD thd, unsigned char err_cmd, bool is_fatal);

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
#define clone_start_statement(thd, thd_key, stmt_key) \
  (clone_protocol_service->start_statement_fn((thd), (thd_key), (stmt_key)))

#define clone_finish_statement(thd) \
  (clone_protocol_service->finish_statement_fn(thd))

 #define clone_get_charsets(thd, char_sets) \
   (clone_protocol_service->get_charsets_fn((thd), (char_sets)))

 #define clone_validate_charsets(thd, char_sets) \
    (clone_protocol_service->validate_charsets_fn((thd), (char_sets)))

#define clone_get_configs(thd, configs) \
   (clone_protocol_service->get_configs_fn((thd), (configs)))

 #define clone_validate_configs(thd, configs) \
    (clone_protocol_service->validate_configs_fn((thd), (configs)))

#define clone_connect(thd, host, port, user, passwd, ssl_ctx, socket) \
  (clone_protocol_service->connect_fn((thd), (host), (port), (user), (passwd), \
  (ssl_ctx), (socket)))

#define clone_send_command(thd, connection, set_active, command, com_buffer, \
  buffer_length) \
  (clone_protocol_service->send_command_fn((thd), (connection), \
  (set_active), (command), (com_buffer), (buffer_length)))

#define clone_get_response(thd, connection, set_active, timeout, packet, length, \
  net_length) \
  (clone_protocol_service->get_response_fn((thd), (connection), \
  (set_active), (timeout), (packet), (length), (net_length)))

#define clone_kill(connection, kill_connection) \
  (clone_protocol_service->kill_fn((connection), (kill_connection)))

#define clone_disconnect(thd, connection, is_fatal, clear_error) \
  (clone_protocol_service->disconnect_fn((thd), (connection), (is_fatal), \
  (clear_error)))

#define clone_get_error(thd, err_num, err_mesg) \
  (clone_protocol_service->get_error_fn((thd), (err_num), (err_mesg)))

#define clone_get_command(thd, command, com_buffer, buffer_length) \
  (clone_protocol_service->get_command_fn((thd), (command), (com_buffer), \
  (buffer_length)))

#define clone_send_response(thd, secure, packet, length) \
  (clone_protocol_service->send_response_fn((thd), (secure), (packet), (length)))

#define clone_send_error(thd, err_cmd, is_fatal) \
  (clone_protocol_service->send_error_fn((thd), (err_cmd), (is_fatal)))

#define clone_set_backup_stage(thd, stage) \
   (clone_protocol_service->set_backup_stage_fn((thd), (stage)))

#define clone_backup_lock(thd, db, tbl) \
   (clone_protocol_service->backup_lock_fn((thd), (db), (tbl)))

#define clone_backup_unlock(thd) \
   (clone_protocol_service->backup_unlock_fn((thd)))
#else
  MYSQL_THD clone_start_statement(MYSQL_THD thd, unsigned int thread_key,
                                  unsigned int statement_key);
  void clone_finish_statement(MYSQL_THD thd);

  int clone_get_charsets(MYSQL_THD thd, void *char_sets);

  int clone_validate_charsets(MYSQL_THD thd, void *char_sets);

  int clone_get_configs(MYSQL_THD thd, void *configs);

  int clone_validate_configs(MYSQL_THD thd, void *configs);

  MYSQL* clone_connect(MYSQL_THD thd, const char *host, uint32_t port,
                       const char *user, const char *passwd,
                       mysql_clone_ssl_context *ssl_ctx,
                       MYSQL_SOCKET *socket);

  int clone_send_command(MYSQL_THD thd, MYSQL *connection, bool set_active,
                         unsigned char command, unsigned char *com_buffer,
                         size_t buffer_length);

  int clone_get_response(MYSQL_THD thd, MYSQL *connection, bool set_active,
                         uint32_t timeout, unsigned char **packet,
                         size_t *length, size_t *net_length);

  int clone_kill(MYSQL *connection, MYSQL *kill_connection);

  void clone_disconnect(MYSQL_THD thd, MYSQL *connection, bool is_fatal,
                        bool clear_error);
  void clone_get_error(MYSQL_THD thd, uint32_t *err_num,
                       const char **err_mesg);

  int clone_get_command(MYSQL_THD thd, unsigned char *command,
                        unsigned char **com_buffer, size_t *buffer_length);

  int clone_send_response(MYSQL_THD thd, bool secure, unsigned char *packet,
                          size_t length);

  int clone_send_error(MYSQL_THD thd, unsigned char err_cmd, bool is_fatal);

  int clone_set_backup_stage(MYSQL_THD thd, unsigned char stage);

  int clone_backup_lock(MYSQL_THD thd, const char *db, const char *tbl);
  int clone_backup_unlock(MYSQL_THD thd);
#endif

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* CLONE_PROTOCOL_SERVICE */
