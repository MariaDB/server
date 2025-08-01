/* Copyright (c) 2003, 2016, Oracle and/or its affiliates.
   Copyright (c) 2009, 2020, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

/*
  This file is included by both libmysql.c (the MySQL client C API)
  and the mysqld server to connect to another MYSQL server.

  The differences for the two cases are:

  - Things that only works for the client:
  - Trying to automatically determinate user name if not supplied to
    mysql_real_connect()
  - Support for reading local file with LOAD DATA LOCAL
  - SHARED memory handling
  - Prepared statements
  
  - Things that only works for the server
  - Alarm handling on connect
  
  In all other cases, the code should be identical for the client and
  server.
*/ 

#include <my_global.h>
#include <my_default.h>
#include "mysql.h"
#include "hash.h"

/* Remove client convenience wrappers */
#undef max_allowed_packet
#undef net_buffer_length

#ifdef EMBEDDED_LIBRARY

#undef MYSQL_SERVER

#ifndef MYSQL_CLIENT
#define MYSQL_CLIENT
#endif

#define CLI_MYSQL_REAL_CONNECT STDCALL cli_mysql_real_connect

#undef net_flush
my_bool	net_flush(NET *net);

#else  /*EMBEDDED_LIBRARY*/
#define CLI_MYSQL_REAL_CONNECT STDCALL mysql_real_connect
#endif /*EMBEDDED_LIBRARY*/

#include <my_sys.h>
#include <mysys_err.h>
#include <m_string.h>
#include <m_ctype.h>
#include "mysql_version.h"
#include "mysqld_error.h"
#include "errmsg.h"
#include <violite.h>

#if !defined(_WIN32)
#include <my_pthread.h>				/* because of signal()	*/
#endif /* !defined(_WIN32) */

#include <sys/stat.h>
#include <signal.h>
#include <time.h>

#ifdef	 HAVE_PWD_H
#include <pwd.h>
#endif

#if !defined(_WIN32)
#ifdef HAVE_SELECT_H
#  include <select.h>
#endif
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#endif /* !defined(_WIN32) */
#ifdef HAVE_SYS_UN_H
#  include <sys/un.h>
#endif


#define CONNECT_TIMEOUT 0

#include "client_settings.h"
#include <ssl_compat.h>
#include <sql_common.h>
#include <mysql/client_plugin.h>

typedef enum {
  ALWAYS_ACCEPT,       /* heuristics is disabled, use CLIENT_LOCAL_FILES */
  WAIT_FOR_QUERY,      /* heuristics is enabled, not sending files */
  ACCEPT_FILE_REQUEST  /* heuristics is enabled, ready to send a file */
} auto_local_infile_state;

#define native_password_plugin_name "mysql_native_password"
#define old_password_plugin_name    "mysql_old_password"

PSI_memory_key key_memory_mysql_options;
PSI_memory_key key_memory_MYSQL_DATA;
PSI_memory_key key_memory_MYSQL;
PSI_memory_key key_memory_MYSQL_RES;
PSI_memory_key key_memory_MYSQL_ROW;
PSI_memory_key key_memory_MYSQL_state_change_info;
PSI_memory_key key_memory_MYSQL_HANDSHAKE;

#if defined (_WIN32) && !defined (EMBEDDED_LIBRARY)
PSI_memory_key key_memory_create_shared_memory;
#endif /* _WIN32 && ! EMBEDDED_LIBRARY */

#ifdef HAVE_PSI_INTERFACE
/*
  This code is common to the client and server,
  and also used in the server when server A connects to server B,
  for example with replication.
  Therefore, the code is also instrumented.
*/

static PSI_memory_info all_client_memory[]=
{
#if defined (_WIN32) && !defined (EMBEDDED_LIBRARY)
  { &key_memory_create_shared_memory, "create_shared_memory", 0},
#endif /* _WIN32 && ! EMBEDDED_LIBRARY */

  { &key_memory_mysql_options, "mysql_options", 0},
  { &key_memory_MYSQL_DATA, "MYSQL_DATA", 0},
  { &key_memory_MYSQL, "MYSQL", 0},
  { &key_memory_MYSQL_RES, "MYSQL_RES", 0},
  { &key_memory_MYSQL_ROW, "MYSQL_ROW", 0},
  { &key_memory_MYSQL_state_change_info, "MYSQL_STATE_CHANGE_INFO", 0},
  { &key_memory_MYSQL_HANDSHAKE, "MYSQL_HANDSHAKE", 0}
};

void init_client_psi_keys(void)
{
  const char *category= "client";
  int count;

  count= array_elements(all_client_memory);
  mysql_memory_register(category, all_client_memory, count);
}

#endif /* HAVE_PSI_INTERFACE */

uint            mariadb_deinitialize_ssl= 1;
uint		mysql_port=0;
char		*mysql_unix_port= 0;
const char	*unknown_sqlstate= "HY000";
const char	*not_error_sqlstate= "00000";
const char	*cant_connect_sqlstate= "08001";

static void mysql_close_free_options(MYSQL *mysql);
static void mysql_close_free(MYSQL *mysql);
static void mysql_prune_stmt_list(MYSQL *mysql);
static int cli_report_progress(MYSQL *mysql, char *packet, uint length);
static my_bool parse_ok_packet(MYSQL *mysql, ulong length);

CHARSET_INFO *default_client_charset_info = &my_charset_latin1;

/* Server error code and message */
unsigned int mysql_server_last_errno;
char mysql_server_last_error[MYSQL_ERRMSG_SIZE];

/**
  Convert the connect timeout option to a timeout value for VIO
  functions (vio_socket_connect() and vio_io_wait()).

  @param mysql  Connection handle (client side).

  @return The timeout value in milliseconds, or -1 if no timeout.
*/

static int get_vio_connect_timeout(MYSQL *mysql)
{
  int timeout_ms;
  uint timeout_sec;

  /*
    A timeout of 0 means no timeout. Also, the connect_timeout
    option value is in seconds, while VIO timeouts are measured
    in milliseconds. Hence, check for a possible overflow. In
    case of overflow, set to no timeout.
  */
  timeout_sec= mysql->options.connect_timeout;

  if (!timeout_sec || (timeout_sec > INT_MAX/1000))
    timeout_ms= -1;
  else
    timeout_ms= (int) (timeout_sec * 1000);

  return timeout_ms;
}


/**
  Set the internal error message to mysql handler

  @param mysql    connection handle (client side)
  @param errcode  CR_ error code, passed to ER macro to get
                  error text
  @parma sqlstate SQL standard sqlstate
*/

void set_mysql_error(MYSQL *mysql, int errcode, const char *sqlstate)
{
  NET *net;
  DBUG_ENTER("set_mysql_error");
  DBUG_PRINT("enter", ("error :%d '%s'", errcode, ER(errcode)));
  DBUG_ASSERT(mysql != 0);

  if (mysql)
  {
    net= &mysql->net;
    net->last_errno= errcode;
    strmov(net->last_error, ER(errcode));
    strmov(net->sqlstate, sqlstate);
  }
  else
  {
    mysql_server_last_errno= errcode;
    strmov(mysql_server_last_error, ER(errcode));
  }
  DBUG_VOID_RETURN;
}

/**
  Clear possible error state of struct NET

  @param net  clear the state of the argument
*/

void net_clear_error(NET *net)
{
  net->last_errno= 0;
  net->last_error[0]= '\0';
  strmov(net->sqlstate, not_error_sqlstate);
}

/**
  Set an error message on the client.

  @param mysql     connection handle
  @param errcode   CR_* errcode, for client errors
  @param sqlstate  SQL standard sql state, unknown_sqlstate for the
                   majority of client errors.
  @param format    error message template, in sprintf format
  @param ...       variable number of arguments
*/

void set_mysql_extended_error(MYSQL *mysql, int errcode,
                                     const char *sqlstate,
                                     const char *format, ...)
{
  NET *net;
  va_list args;
  DBUG_ENTER("set_mysql_extended_error");
  DBUG_PRINT("enter", ("error :%d '%s'", errcode, format));
  DBUG_ASSERT(mysql != 0);

  net= &mysql->net;
  net->last_errno= errcode;
  va_start(args, format);
  my_vsnprintf(net->last_error, sizeof(net->last_error)-1,
               format, args);
  va_end(args);
  strmov(net->sqlstate, sqlstate);

  DBUG_VOID_RETURN;
}



/*
  Create a named pipe connection
*/

#ifdef _WIN32

HANDLE create_named_pipe(MYSQL *mysql, uint connect_timeout, char **arg_host,
			 char **arg_unix_socket)
{
  HANDLE hPipe=INVALID_HANDLE_VALUE;
  char pipe_name[1024];
  DWORD dwMode;
  int i;
  char *host= *arg_host, *unix_socket= *arg_unix_socket;

  if ( ! unix_socket || (unix_socket)[0] == 0x00)
    unix_socket = mysql_unix_port;
  if (!host || !strcmp(host,LOCAL_HOST))
    host=LOCAL_HOST_NAMEDPIPE;

  
  pipe_name[sizeof(pipe_name)-1]= 0;		/* Safety if too long string */
  strxnmov(pipe_name, sizeof(pipe_name)-1, "\\\\", host, "\\pipe\\",
	   unix_socket, NullS);
  DBUG_PRINT("info",("Server name: '%s'.  Named Pipe: %s", host, unix_socket));

  for (i=0 ; i < 100 ; i++)			/* Don't retry forever */
  {
    if ((hPipe = CreateFile(pipe_name,
			    GENERIC_READ | GENERIC_WRITE,
			    0,
			    NULL,
			    OPEN_EXISTING,
			    FILE_FLAG_OVERLAPPED,
			    NULL )) != INVALID_HANDLE_VALUE)
      break;
    if (GetLastError() != ERROR_PIPE_BUSY)
    {
      set_mysql_extended_error(mysql, CR_NAMEDPIPEOPEN_ERROR,
                               unknown_sqlstate, ER(CR_NAMEDPIPEOPEN_ERROR),
                               host, unix_socket, (ulong) GetLastError());
      return INVALID_HANDLE_VALUE;
    }
    /* wait for for an other instance */
    if (! WaitNamedPipe(pipe_name, connect_timeout*1000) )
    {
      set_mysql_extended_error(mysql, CR_NAMEDPIPEWAIT_ERROR, unknown_sqlstate,
                               ER(CR_NAMEDPIPEWAIT_ERROR),
                               host, unix_socket, (ulong) GetLastError());
      return INVALID_HANDLE_VALUE;
    }
  }
  if (hPipe == INVALID_HANDLE_VALUE)
  {
    set_mysql_extended_error(mysql, CR_NAMEDPIPEOPEN_ERROR, unknown_sqlstate,
                             ER(CR_NAMEDPIPEOPEN_ERROR), host, unix_socket,
                             (ulong) GetLastError());
    return INVALID_HANDLE_VALUE;
  }
  dwMode = PIPE_READMODE_BYTE | PIPE_WAIT;
  if ( !SetNamedPipeHandleState(hPipe, &dwMode, NULL, NULL) )
  {
    CloseHandle( hPipe );
    set_mysql_extended_error(mysql, CR_NAMEDPIPESETSTATE_ERROR,
                             unknown_sqlstate, ER(CR_NAMEDPIPESETSTATE_ERROR),
                             host, unix_socket, (ulong) GetLastError());
    return INVALID_HANDLE_VALUE;
  }
  *arg_host=host ; *arg_unix_socket=unix_socket;	/* connect arg */
  return (hPipe);
}
#endif


/**
  Read a packet from server. Give error message if socket was down
  or packet is an error message

  @retval  packet_error    An error occurred during reading.
                           Error message is set.
  @retval  
*/
ulong
cli_safe_read(MYSQL *mysql)
{
  ulong reallen = 0;
  return cli_safe_read_reallen(mysql, &reallen);
}

ulong
cli_safe_read_reallen(MYSQL *mysql, ulong* reallen)
{
  NET *net= &mysql->net;
  ulong len=0;

restart:
  if (net->vio != 0)
    len= my_net_read_packet_reallen(net, 0, reallen);

  if (len == packet_error || len == 0)
  {
    DBUG_PRINT("error",("Wrong connection or packet. fd: %s  len: %lu",
			net->vio ? vio_description(net->vio) : NULL, len));
#ifdef MYSQL_SERVER
    if (net->vio && (net->last_errno == ER_NET_READ_INTERRUPTED))
      return (packet_error);
#endif /*MYSQL_SERVER*/
    end_server(mysql);
    set_mysql_error(mysql, net->last_errno == ER_NET_PACKET_TOO_LARGE ?
                    CR_NET_PACKET_TOO_LARGE: CR_SERVER_LOST, unknown_sqlstate);
    return (packet_error);
  }
  if (net->read_pos[0] == 255)
  {
    if (len > 3)
    {
      char *pos= (char*) net->read_pos+1;
      uint last_errno=uint2korr(pos);

      if (last_errno == 65535 &&
          (mysql->server_capabilities & CLIENT_PROGRESS_OBSOLETE))
      {
        if (cli_report_progress(mysql, pos+2, (uint) (len-3)))
        {
          /* Wrong packet */
          set_mysql_error(mysql,  CR_MALFORMED_PACKET, unknown_sqlstate);
          return (packet_error);
        }
        goto restart;
      }
      net->last_errno= last_errno;
      
      pos+=2;
      len-=2;
      if (protocol_41(mysql) && pos[0] == '#')
      {
	strmake_buf(net->sqlstate, pos+1);
	pos+= SQLSTATE_LENGTH+1;
      }
      else
      {
        /*
          The SQL state hasn't been received -- it should be reset to HY000
          (unknown error sql state).
        */

        strmov(net->sqlstate, unknown_sqlstate);
      }

      (void) strmake(net->last_error,(char*) pos,
		     MY_MIN((uint) len,(uint) sizeof(net->last_error)-1));
    }
    else
      set_mysql_error(mysql, CR_UNKNOWN_ERROR, unknown_sqlstate);
    /*
      Cover a protocol design error: error packet does not
      contain the server status. Therefore, the client has no way
      to find out whether there are more result sets of
      a multiple-result-set statement pending. Luckily, in 5.0 an
      error always aborts execution of a statement, wherever it is
      a multi-statement or a stored procedure, so it should be
      safe to unconditionally turn off the flag here.
    */
    mysql->server_status&= ~SERVER_MORE_RESULTS_EXISTS;

    DBUG_PRINT("error",("Got error: %d/%s (%s)",
                        net->last_errno,
                        net->sqlstate,
                        net->last_error));
    return(packet_error);
  }
  return len;
}

void free_rows(MYSQL_DATA *cur)
{
  if (cur)
  {
    free_root(&cur->alloc,MYF(0));
    my_free(cur);
  }
}

my_bool
cli_advanced_command(MYSQL *mysql, enum enum_server_command command,
		     const uchar *header, ulong header_length,
		     const uchar *arg, ulong arg_length, my_bool skip_check,
                     MYSQL_STMT *stmt)
{
  NET *net= &mysql->net;
  my_bool result= 1;
  my_bool stmt_skip= stmt ? stmt->state != MYSQL_STMT_INIT_DONE : FALSE;
  DBUG_ENTER("cli_advanced_command");

  if (mysql->net.vio == 0)
  {						/* Do reconnect if possible */
    if (mysql_reconnect(mysql) || stmt_skip)
      DBUG_RETURN(1);
  }
  if (mysql->status != MYSQL_STATUS_READY ||
      mysql->server_status & SERVER_MORE_RESULTS_EXISTS)
  {
    DBUG_PRINT("error",("state: %d", mysql->status));
    set_mysql_error(mysql, CR_COMMANDS_OUT_OF_SYNC, unknown_sqlstate);
    DBUG_RETURN(1);
  }

  net_clear_error(net);
  mysql->info=0;
  mysql->affected_rows= ~(my_ulonglong) 0;
  /*
    We don't want to clear the protocol buffer on COM_QUIT, because if
    the previous command was a shutdown command, we may have the
    response for the COM_QUIT already in the communication buffer
  */
  net_clear(&mysql->net, (command != COM_QUIT));

  if (net_write_command(net,(uchar) command, header, header_length,
			arg, arg_length))
  {
    DBUG_PRINT("error",("Can't send command to server. Error: %d",
			socket_errno));
    if (net->last_errno == ER_NET_PACKET_TOO_LARGE)
    {
      set_mysql_error(mysql, CR_NET_PACKET_TOO_LARGE, unknown_sqlstate);
      goto end;
    }
    if (net->last_errno == ER_NET_ERROR_ON_WRITE && command == COM_BINLOG_DUMP)
      goto end;
    end_server(mysql);
    if (mysql_reconnect(mysql) || stmt_skip)
      goto end;
    if (net_write_command(net,(uchar) command, header, header_length,
			  arg, arg_length))
    {
      set_mysql_error(mysql, CR_SERVER_GONE_ERROR, unknown_sqlstate);
      goto end;
    }
  }
  result=0;
  if (!skip_check)
    result= ((mysql->packet_length=cli_safe_read(mysql)) == packet_error ?
	     1 : 0);
end:
  DBUG_PRINT("exit",("result: %d", result));
  DBUG_RETURN(result);
}

void free_old_query(MYSQL *mysql)
{
  DBUG_ENTER("free_old_query");
  if (mysql->fields)
    free_root(&mysql->field_alloc,MYF(0));
 /* Assume rowlength < 8192 */
  init_alloc_root(PSI_INSTRUMENT_ME, &mysql->field_alloc, 8192, 0,
                  MYF(mysql->options.use_thread_specific_memory ?
                      MY_THREAD_SPECIFIC : 0));
  mysql->fields= 0;
  mysql->field_count= 0;			/* For API */
  mysql->warning_count= 0;
  mysql->info= 0;
  DBUG_VOID_RETURN;
}


/**
  Finish reading of a partial result set from the server.
  Get the EOF packet, and update mysql->status
  and mysql->warning_count.

  @return  TRUE if a communication or protocol error, an error
           is set in this case, FALSE otherwise.
*/

my_bool flush_one_result(MYSQL *mysql)
{
  ulong packet_length;

  DBUG_ASSERT(mysql->status != MYSQL_STATUS_READY);

  do
  {
    packet_length= cli_safe_read(mysql);
    /*
      There is an error reading from the connection,
      or (sic!) there were no error and no
      data in the stream, i.e. no more data from the server.
      Since we know our position in the stream (somewhere in
      the middle of a result set), this latter case is an error too
      -- each result set must end with a EOF packet.
      cli_safe_read() has set an error for us, just return.
    */
    if (packet_length == packet_error)
      return TRUE;
  }
  while (packet_length > 8 || mysql->net.read_pos[0] != 254);

  /* Analyze EOF packet of the result set. */

  if (protocol_41(mysql))
  {
    char *pos= (char*) mysql->net.read_pos + 1;
    mysql->warning_count=uint2korr(pos);
    pos+=2;
    mysql->server_status=uint2korr(pos);
    pos+=2;
  }
  return FALSE;
}


/**
  Read a packet from network. If it's an OK packet, flush it.

  @return  TRUE if error, FALSE otherwise. In case of 
           success, is_ok_packet is set to TRUE or FALSE,
           based on what we got from network.
*/

my_bool opt_flush_ok_packet(MYSQL *mysql, my_bool *is_ok_packet)
{
  ulong packet_length= cli_safe_read(mysql);

  if (packet_length == packet_error)
    return TRUE;

  /* cli_safe_read always reads a non-empty packet. */
  DBUG_ASSERT(packet_length);

  *is_ok_packet= mysql->net.read_pos[0] == 0;
  if (*is_ok_packet)
  {
    uchar *pos= mysql->net.read_pos + 1;

    net_field_length_ll(&pos); /* affected rows */
    net_field_length_ll(&pos); /* insert id */

    mysql->server_status=uint2korr(pos);
    pos+=2;

    if (protocol_41(mysql))
    {
      mysql->warning_count=uint2korr(pos);
      pos+=2;
    }
  }
  return FALSE;
}


/*
  Flush result set sent from server
*/

static void cli_flush_use_result(MYSQL *mysql, my_bool flush_all_results)
{
  /* Clear the current execution status */
  DBUG_ENTER("cli_flush_use_result");
  DBUG_PRINT("warning",("Not all packets read, clearing them"));

  if (flush_one_result(mysql))
    DBUG_VOID_RETURN;                           /* An error occurred */

  if (! flush_all_results)
    DBUG_VOID_RETURN;

  while (mysql->server_status & SERVER_MORE_RESULTS_EXISTS)
  {
    my_bool is_ok_packet;
    if (opt_flush_ok_packet(mysql, &is_ok_packet))
      DBUG_VOID_RETURN;                         /* An error occurred. */
    if (is_ok_packet)
    {
      /*
        Indeed what we got from network was an OK packet, and we
        know that OK is the last one in a multi-result-set, so
        just return.
      */
      DBUG_VOID_RETURN;
    }
    /*
      It's a result set, not an OK packet. A result set contains
      of two result set subsequences: field metadata, terminated
      with EOF packet, and result set data, again terminated with
      EOF packet. Read and flush them.
    */
    if (flush_one_result(mysql) || flush_one_result(mysql))
      DBUG_VOID_RETURN;                         /* An error occurred. */
  }

  DBUG_VOID_RETURN;
}


/*
  Report progress to the client

  RETURN VALUES
    0  ok
    1  error
*/

static int cli_report_progress(MYSQL *mysql, char *pkt, uint length)
{
  uint stage, max_stage, proc_length;
  double progress;
  uchar *packet= (uchar*)pkt;
  uchar *start= packet;

  if (length < 5)
    return 1;                         /* Wrong packet */

  if (!(mysql->options.extension && mysql->options.extension->report_progress))
    return 0;                         /* No callback, ignore packet */

  packet++;                           /* Ignore number of strings */
  stage= (uint) *packet++;
  max_stage= (uint) *packet++;
  progress= uint3korr(packet)/1000.0;
  packet+= 3;
  proc_length= net_field_length(&packet);
  if (packet + proc_length > start + length)
    return 1;                         /* Wrong packet */
  (*mysql->options.extension->report_progress)(mysql, stage, max_stage,
                                               progress, (char*) packet,
                                               proc_length);
  return 0;
}


/**************************************************************************
  Shut down connection
**************************************************************************/

void end_server(MYSQL *mysql)
{
  int save_errno= errno;
  DBUG_ENTER("end_server");
  if (mysql->net.vio != 0)
  {
    struct st_VioSSLFd *ssl_fd= (struct st_VioSSLFd*) mysql->connector_fd;
    if (ssl_fd)
      SSL_CTX_free(ssl_fd->ssl_context);
    my_free(ssl_fd);
    mysql->connector_fd = 0;

    DBUG_PRINT("info",("Net: %s", vio_description(mysql->net.vio)));
#ifdef MYSQL_SERVER
    slave_io_thread_detach_vio();
#endif
    vio_delete(mysql->net.vio);
    mysql->net.vio= 0;          /* Marker */
    mysql_prune_stmt_list(mysql);
  }
  net_end(&mysql->net);
  free_old_query(mysql);
  errno= save_errno;
  DBUG_VOID_RETURN;
}


void STDCALL
mysql_free_result(MYSQL_RES *result)
{
  DBUG_ENTER("mysql_free_result");
  DBUG_PRINT("enter",("mysql_res: %p", result));
  if (result)
  {
    MYSQL *mysql= result->handle;
    if (mysql)
    {
      if (mysql->unbuffered_fetch_owner == &result->unbuffered_fetch_cancelled)
        mysql->unbuffered_fetch_owner= 0;
      if (mysql->status == MYSQL_STATUS_USE_RESULT)
      {
        (*mysql->methods->flush_use_result)(mysql, FALSE);
        mysql->status=MYSQL_STATUS_READY;
        if (mysql->unbuffered_fetch_owner)
          *mysql->unbuffered_fetch_owner= TRUE;
      }
    }
    free_rows(result->data);
    if (result->fields)
      free_root(&result->field_alloc,MYF(0));
    my_free(result->row);
    my_free(result);
  }
  DBUG_VOID_RETURN;
}

/****************************************************************************
  Get options from my.cnf
****************************************************************************/

static const char *default_options[]=
{
  "port","socket","compress","password","pipe", "timeout", "user",
  "init-command", "host", "database", "debug", "return-found-rows",
  "ssl-key" ,"ssl-cert" ,"ssl-ca" ,"ssl-capath",
  "character-sets-dir", "default-character-set", "interactive-timeout",
  "connect-timeout", "local-infile", "disable-local-infile",
  "ssl-cipher", "max-allowed-packet", "protocol",
  "multi-results", "multi-statements", "multi-queries", "secure-auth",
  "report-data-truncation", "plugin-dir", "default-auth",
  "bind-address", "ssl-crl", "ssl-crlpath",
  "enable-cleartext-plugin",
  NullS
};
enum option_id {
  OPT_port=1, OPT_socket, OPT_compress, OPT_password, OPT_pipe, OPT_timeout, OPT_user, 
  OPT_init_command, OPT_host, OPT_database, OPT_debug, OPT_return_found_rows, 
  OPT_ssl_key, OPT_ssl_cert, OPT_ssl_ca, OPT_ssl_capath, 
  OPT_character_sets_dir, OPT_default_character_set, OPT_interactive_timeout, 
  OPT_connect_timeout, OPT_local_infile, OPT_disable_local_infile, 
  OPT_ssl_cipher, OPT_max_allowed_packet, OPT_protocol,
  OPT_multi_results, OPT_multi_statements, OPT_multi_queries, OPT_secure_auth, 
  OPT_report_data_truncation, OPT_plugin_dir, OPT_default_auth, 
  OPT_bind_address, OPT_ssl_crl, OPT_ssl_crlpath,
  OPT_enable_cleartext_plugin,
  OPT_keep_this_one_last
};

static char *opt_strdup(const char *from, myf my_flags)
{
  return my_strdup(key_memory_mysql_options, from, my_flags);
}

static TYPELIB option_types=CREATE_TYPELIB_FOR(default_options);

static int add_init_command(struct st_mysql_options *options, const char *cmd)
{
  char *tmp;

  if (!options->init_commands)
  {
    options->init_commands= (DYNAMIC_ARRAY*)my_malloc(key_memory_mysql_options,
                                                      sizeof(DYNAMIC_ARRAY),
						      MYF(MY_WME));
    my_init_dynamic_array(key_memory_mysql_options, options->init_commands,
                          sizeof(char*),5, 5, MYF(0));
  }

  if (!(tmp= opt_strdup(cmd,MYF(MY_WME))) ||
      insert_dynamic(options->init_commands, &tmp))
  {
    my_free(tmp);
    return 1;
  }

  return 0;
}


#define ALLOCATE_EXTENSIONS(OPTS)                                \
      (OPTS)->extension= (struct st_mysql_options_extention *)   \
        my_malloc(key_memory_mysql_options,                      \
                  sizeof(struct st_mysql_options_extention),     \
                  MYF(MY_WME | MY_ZEROFILL))                     \


#define ENSURE_EXTENSIONS_PRESENT(OPTS)                          \
    do {                                                         \
      if (!(OPTS)->extension)                                    \
        ALLOCATE_EXTENSIONS(OPTS);                               \
    } while (0)


#define EXTENSION_SET_STRING_X(OPTS, X, STR, dup)                \
    do {                                                         \
      if ((OPTS)->extension)                                     \
        my_free((OPTS)->extension->X);                           \
      else                                                       \
        ALLOCATE_EXTENSIONS(OPTS);                               \
      (OPTS)->extension->X= ((STR) != NULL) ?                    \
        dup((STR), MYF(MY_WME)) : NULL;                          \
    } while (0)

#define EXTENSION_SET_STRING(OPTS, X, STR)      \
  EXTENSION_SET_STRING_X(OPTS, X, STR, opt_strdup)


#if defined(HAVE_OPENSSL) && !defined(EMBEDDED_LIBRARY)
#define SET_SSL_OPTION_X(OPTS, opt_var, arg, dup)                \
  my_free((OPTS)->opt_var);                                      \
  (OPTS)->opt_var= arg ? dup(arg, MYF(MY_WME)) : NULL;
#define EXTENSION_SET_SSL_STRING_X(OPTS, X, STR, dup)            \
  EXTENSION_SET_STRING_X((OPTS), X, (STR), dup);

static char *set_ssl_option_unpack_path(const char *arg, myf flags)
{
  char buff[FN_REFLEN + 1];
  unpack_filename(buff, (char *)arg);
  return opt_strdup(buff, flags);
}

#else
#define SET_SSL_OPTION_X(OPTS, opt_var,arg, dup) do { } while(0)
#define EXTENSION_SET_SSL_STRING_X(OPTS, X, STR, dup) do { } while(0)
#endif /* defined(HAVE_OPENSSL) && !defined(EMBEDDED_LIBRARY) */

#define SET_SSL_OPTION(OPTS, opt_var,arg) SET_SSL_OPTION_X(OPTS, opt_var, arg, opt_strdup)
#define EXTENSION_SET_SSL_STRING(OPTS, X, STR) EXTENSION_SET_SSL_STRING_X(OPTS, X, STR, opt_strdup)
#define SET_SSL_PATH_OPTION(OPTS, opt_var,arg) SET_SSL_OPTION_X(OPTS, opt_var, arg, set_ssl_option_unpack_path)
#define EXTENSION_SET_SSL_PATH_STRING(OPTS, X, STR) EXTENSION_SET_SSL_STRING_X(OPTS, X, STR, set_ssl_option_unpack_path)

void mysql_read_default_options(struct st_mysql_options *options,
				const char *filename,const char *group)
{
  int argc;
  char *argv_buff[1],**argv;
  const char *groups[5];
  DBUG_ENTER("mysql_read_default_options");
  DBUG_PRINT("enter",("file: %s  group: %s",filename,group ? group :"NULL"));

  compile_time_assert(OPT_keep_this_one_last ==
                      array_elements(default_options));

  argc=1; argv=argv_buff; argv_buff[0]= (char*) "client";
  groups[0]= (char*) "client";
  groups[1]= (char*) "client-server";
  groups[2]= (char*) "client-mariadb";
  groups[3]= (char*) group;
  groups[4]=0;

  my_load_defaults(filename, groups, &argc, &argv, NULL);
  if (argc != 1)				/* If some default option */
  {
    char **option=argv;
    while (*++option)
    {
      /* DBUG_PRINT("info",("option: %s",option[0])); */
      if (option[0][0] == '-' && option[0][1] == '-')
      {
	char *end=strcend(*option,'=');
	char *opt_arg=0;
	if (*end)
	{
	  opt_arg=end+1;
	  *end=0;				/* Remove '=' */
	}
	/* Change all '_' in variable name to '-' */
	for (end= *option ; *(end= strcend(end,'_')) ; )
	  *end= '-';
	switch (find_type(*option + 2, &option_types, FIND_TYPE_BASIC)) {
	case OPT_port:
	  if (opt_arg)
	    options->port=atoi(opt_arg);
	  break;
	case OPT_socket:
	  if (opt_arg)
	  {
	    my_free(options->unix_socket);
	    options->unix_socket=opt_strdup(opt_arg,MYF(MY_WME));
	  }
	  break;
	case OPT_compress:
	  options->compress=1;
	  options->client_flag|= CLIENT_COMPRESS;
	  break;
        case OPT_password:
	  if (opt_arg)
	  {
	    my_free(options->password);
	    options->password=opt_strdup(opt_arg,MYF(MY_WME));
	  }
	  break;
        case OPT_pipe:
          options->protocol = MYSQL_PROTOCOL_PIPE;
          break;
        case OPT_connect_timeout: 
	case OPT_timeout:
	  if (opt_arg)
	    options->connect_timeout=atoi(opt_arg);
	  break;
	case OPT_user:
	  if (opt_arg)
	  {
	    my_free(options->user);
	    options->user=opt_strdup(opt_arg,MYF(MY_WME));
	  }
	  break;
	case OPT_init_command:
	  add_init_command(options,opt_arg);
	  break;
	case OPT_host:
	  if (opt_arg)
	  {
	    my_free(options->host);
	    options->host=opt_strdup(opt_arg,MYF(MY_WME));
	  }
	  break;
        case OPT_database:
	  if (opt_arg)
	  {
	    my_free(options->db);
	    options->db=opt_strdup(opt_arg,MYF(MY_WME));
	  }
	  break;
	case OPT_debug:
#ifdef MYSQL_CLIENT
	  mysql_debug(opt_arg ? opt_arg : "d:t:o,/tmp/client.trace");
	  break;
#endif
	case OPT_return_found_rows:
	  options->client_flag|=CLIENT_FOUND_ROWS;
	  break;
	case OPT_ssl_key:
          SET_SSL_OPTION(options, ssl_key, opt_arg);
          break;
	case OPT_ssl_cert:
          SET_SSL_OPTION(options, ssl_cert, opt_arg);
          break;
	case OPT_ssl_ca:
          SET_SSL_OPTION(options, ssl_ca, opt_arg);
          break;
	case OPT_ssl_capath:
          SET_SSL_OPTION(options, ssl_capath, opt_arg);
          break;
        case OPT_ssl_cipher:
          SET_SSL_OPTION(options, ssl_cipher, opt_arg);
          break;
	case OPT_ssl_crl:
          EXTENSION_SET_SSL_STRING(options, ssl_crl, opt_arg);
          break;
	case OPT_ssl_crlpath:
          EXTENSION_SET_SSL_STRING(options, ssl_crlpath, opt_arg);
          break;
	case OPT_character_sets_dir:
	  my_free(options->charset_dir);
          options->charset_dir = opt_strdup(opt_arg, MYF(MY_WME));
	  break;
	case OPT_default_character_set:
	  my_free(options->charset_name);
          options->charset_name = opt_strdup(opt_arg, MYF(MY_WME));
	  break;
        case OPT_interactive_timeout:
	  options->client_flag|= CLIENT_INTERACTIVE;
	  break;
        case OPT_local_infile:
	  if (!opt_arg || atoi(opt_arg) != 0)
	    options->client_flag|= CLIENT_LOCAL_FILES;
	  else
	    options->client_flag&= ~CLIENT_LOCAL_FILES;
	  break;
        case OPT_disable_local_infile:
	  options->client_flag&= ~CLIENT_LOCAL_FILES;
          break;
	case OPT_max_allowed_packet:
          if (opt_arg)
	    options->max_allowed_packet= atoi(opt_arg);
	  break;
        case OPT_protocol:
          if (options->protocol != UINT_MAX32 &&
              (options->protocol= find_type(opt_arg, &sql_protocol_typelib,
                                            FIND_TYPE_BASIC)) <= 0)
          {
            fprintf(stderr, "Unknown option to protocol: %s\n", opt_arg);
            options->protocol= UINT_MAX32;
          }
          break;
        case OPT_multi_results:
	  options->client_flag|= CLIENT_MULTI_RESULTS;
	  break;
        case OPT_multi_statements:
        case OPT_multi_queries:
	  options->client_flag|= CLIENT_MULTI_STATEMENTS | CLIENT_MULTI_RESULTS;
	  break;
        case OPT_secure_auth:
          options->secure_auth= TRUE;
          break;
        case OPT_report_data_truncation:
          options->report_data_truncation= opt_arg ? MY_TEST(atoi(opt_arg)) : 1;
          break;
        case OPT_plugin_dir:
          {
            char buff[FN_REFLEN], buff2[FN_REFLEN], *buff2_ptr= buff2;
            if (strlen(opt_arg) >= FN_REFLEN)
              opt_arg[FN_REFLEN]= '\0';
            if (my_realpath(buff, opt_arg, 0))
            {
              DBUG_PRINT("warning",("failed to normalize the plugin path: %s",
                                    opt_arg));
              break;
            }
            convert_dirname(buff2, buff, NULL);
            EXTENSION_SET_STRING(options, plugin_dir, buff2_ptr);
          }
          break;
        case OPT_default_auth:
          EXTENSION_SET_STRING(options, default_auth, opt_arg);
          break;
        case OPT_enable_cleartext_plugin:
          break;
	default:
	  DBUG_PRINT("warning",("unknown option: %s",option[0]));
	}
      }
    }
  }
  free_defaults(argv);
  DBUG_VOID_RETURN;
}


/**************************************************************************
  Get column lengths of the current row
  If one uses mysql_use_result, res->lengths contains the length information,
  else the lengths are calculated from the offset between pointers.
**************************************************************************/

static void cli_fetch_lengths(ulong *to, MYSQL_ROW column,
			      unsigned int field_count)
{ 
  ulong *prev_length;
  char *start=0;
  MYSQL_ROW end;

  prev_length=0;				/* Keep gcc happy */
  for (end=column + field_count + 1 ; column != end ; column++, to++)
  {
    if (!*column)
    {
      *to= 0;					/* Null */
      continue;
    }
    if (start)					/* Found end of prev string */
      *prev_length= (ulong) (*column-start-1);
    start= *column;
    prev_length= to;
  }
}

/***************************************************************************
  Change field rows to field structs
***************************************************************************/

MYSQL_FIELD *
unpack_fields(MYSQL *mysql, MYSQL_DATA *data,MEM_ROOT *alloc,uint fields,
	      my_bool default_value, uint server_capabilities)
{
  MYSQL_ROWS	*row;
  MYSQL_FIELD	*field,*result;
  ulong lengths[9];				/* Max of fields */
  DBUG_ENTER("unpack_fields");

  field= result= (MYSQL_FIELD*) alloc_root(alloc,
					   (uint) sizeof(*field)*fields);
  if (!result)
  {
    free_rows(data);				/* Free old data */
    set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
    DBUG_RETURN(0);
  }
  bzero((char*) field, (uint) sizeof(MYSQL_FIELD)*fields);
  if (server_capabilities & CLIENT_PROTOCOL_41)
  {
    /* server is 4.1, and returns the new field result format */
    for (row=data->data; row ; row = row->next,field++)
    {
      uchar *pos;
      /* fields count may be wrong */
      if (field >= result + fields)
        goto err;

      cli_fetch_lengths(&lengths[0], row->data, default_value ? 8 : 7);
      field->catalog=   strmake_root(alloc,(char*) row->data[0], lengths[0]);
      field->db=        strmake_root(alloc,(char*) row->data[1], lengths[1]);
      field->table=     strmake_root(alloc,(char*) row->data[2], lengths[2]);
      field->org_table= strmake_root(alloc,(char*) row->data[3], lengths[3]);
      field->name=      strmake_root(alloc,(char*) row->data[4], lengths[4]);
      field->org_name=  strmake_root(alloc,(char*) row->data[5], lengths[5]);

      field->catalog_length=	lengths[0];
      field->db_length=		lengths[1];
      field->table_length=	lengths[2];
      field->org_table_length=	lengths[3];
      field->name_length=	lengths[4];
      field->org_name_length=	lengths[5];

      /* Unpack fixed length parts */
      if (lengths[6] != 12)
        goto err;

      pos= (uchar*) row->data[6];
      field->charsetnr= uint2korr(pos);
      field->length=	(uint) uint4korr(pos+2);
      field->type=	(enum enum_field_types) pos[6];
      field->flags=	uint2korr(pos+7);
      field->decimals=  (uint) pos[9];

      if (IS_NUM(field->type))
        field->flags|= NUM_FLAG;
      if (default_value && row->data[7])
      {
        field->def=strmake_root(alloc,(char*) row->data[7], lengths[7]);
	field->def_length= lengths[7];
      }
      else
        field->def=0;
      field->max_length= 0;
    }
  }
#ifndef DELETE_SUPPORT_OF_4_0_PROTOCOL
  else
  {
    /* old protocol, for backward compatibility */
    for (row=data->data; row ; row = row->next,field++)
    {
      if (field >= result + fields)
        goto err;

      /*
       If any of the row->data[] below is NULL, it can result in a
       crash. Error out early as it indicates a malformed packet.
       For data[0], data[1] and data[5], strmake_root will handle
       NULL values.
      */
      if (!row->data[2] || !row->data[3] || !row->data[4])
      {
        free_rows(data);
        set_mysql_error(mysql, CR_MALFORMED_PACKET, unknown_sqlstate);
        DBUG_RETURN(0);
      }

      cli_fetch_lengths(&lengths[0], row->data, default_value ? 6 : 5);
      field->org_table= field->table=  strmake_root(alloc,(char*) row->data[0], lengths[0]);
      field->name=   strmake_root(alloc,(char*) row->data[1], lengths[1]);
      field->length= (uint) uint3korr(row->data[2]);
      field->type=   (enum enum_field_types) (uchar) row->data[3][0];

      field->catalog=(char*)  "";
      field->db=     (char*)  "";
      field->catalog_length= 0;
      field->db_length= 0;
      field->org_table_length=	field->table_length=	lengths[0];
      field->name_length=	lengths[1];

      if (server_capabilities & CLIENT_LONG_FLAG)
      {
        field->flags=   uint2korr(row->data[4]);
        field->decimals=(uint) (uchar) row->data[4][2];
      }
      else
      {
        field->flags=   (uint) (uchar) row->data[4][0];
        field->decimals=(uint) (uchar) row->data[4][1];
      }
      if (IS_NUM(field->type))
        field->flags|= NUM_FLAG;
      if (default_value && row->data[5])
      {
        field->def= strmake_root(alloc,(char*) row->data[5], lengths[5]);
	field->def_length= lengths[5];
      }
      else
        field->def=0;
      field->max_length= 0;
    }
  }
#endif /* DELETE_SUPPORT_OF_4_0_PROTOCOL */
  if (field < result + fields)
    goto err;
  free_rows(data);				/* Free old data */
  DBUG_RETURN(result);

err:
  /* malformed packet. signal an error. */
  free_rows(data);
  free_root(alloc, MYF(0));
  set_mysql_error(mysql, CR_MALFORMED_PACKET, unknown_sqlstate);
  DBUG_RETURN(0);
}

/* Read all rows (fields or data) from server */

MYSQL_DATA *cli_read_rows(MYSQL *mysql,MYSQL_FIELD *mysql_fields,
			  unsigned int fields)
{
  uint	field;
  ulong pkt_len;
  ulong len;
  uchar *cp;
  char	*to, *end_to;
  MYSQL_DATA *result;
  MYSQL_ROWS **prev_ptr,*cur;
  NET *net = &mysql->net;
  DBUG_ENTER("cli_read_rows");

  if ((pkt_len= cli_safe_read(mysql)) == packet_error)
    DBUG_RETURN(0);
  if (pkt_len == 0) DBUG_RETURN(0);
  if (!(result=(MYSQL_DATA*) my_malloc(key_memory_MYSQL_DATA, sizeof(MYSQL_DATA),
				       MYF(MY_WME | MY_ZEROFILL))))
  {
    set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
    DBUG_RETURN(0);
  }
  /* Assume rowlength < 8192 */
  init_alloc_root(key_memory_MYSQL_DATA, &result->alloc, 8192, 0,
                  MYF(mysql->options.use_thread_specific_memory ?
                      MY_THREAD_SPECIFIC : 0));
  result->alloc.min_malloc=sizeof(MYSQL_ROWS);
  prev_ptr= &result->data;
  result->rows=0;
  result->fields=fields;

  /*
    The last EOF packet is either a single 254 character or (in MySQL 4.1)
    254 followed by 1-7 status bytes.

    This doesn't conflict with normal usage of 254 which stands for a
    string where the length of the string is 8 bytes. (see net_field_length())
  */

  while (*(cp=net->read_pos) != 254 || pkt_len >= 8)
  {
    result->rows++;
    if (!(cur= (MYSQL_ROWS*) alloc_root(&result->alloc,
					sizeof(MYSQL_ROWS))) ||
	!(cur->data= ((MYSQL_ROW)
		      alloc_root(&result->alloc,
				 (fields+1)*sizeof(char *)+pkt_len))))
    {
      free_rows(result);
      set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
      DBUG_RETURN(0);
    }
    *prev_ptr=cur;
    prev_ptr= &cur->next;
    to= (char*) (cur->data+fields+1);
    end_to=to+pkt_len-1;
    for (field=0 ; field < fields ; field++)
    {
      if ((len=(ulong) net_field_length(&cp)) == NULL_LENGTH)
      {						/* null field */
	cur->data[field] = 0;
      }
      else
      {
	cur->data[field] = to;
        if (unlikely(len > (ulong)(end_to-to) || to > end_to))
        {
          free_rows(result);
          set_mysql_error(mysql, CR_MALFORMED_PACKET, unknown_sqlstate);
          DBUG_RETURN(0);
        }
	memcpy(to,(char*) cp,len); to[len]=0;
	to+=len+1;
	cp+=len;
	if (mysql_fields)
	{
	  if (mysql_fields[field].max_length < len)
	    mysql_fields[field].max_length=len;
	}
      }
    }
    cur->data[field]=to;			/* End of last field */
    if ((pkt_len=cli_safe_read(mysql)) == packet_error)
    {
      free_rows(result);
      DBUG_RETURN(0);
    }
  }
  *prev_ptr=0;					/* last pointer is null */
  if (pkt_len > 1)				/* MySQL 4.1 protocol */
  {
    mysql->warning_count= uint2korr(cp+1);
    mysql->server_status= uint2korr(cp+3);
    DBUG_PRINT("info",("status: %u  warning_count:  %u",
		       mysql->server_status, mysql->warning_count));
  }
  DBUG_PRINT("exit", ("Got %lu rows", (ulong) result->rows));
  DBUG_RETURN(result);
}

/*
  Read one row. Uses packet buffer as storage for fields.
  When next packet is read, the previous field values are destroyed
*/


static int
read_one_row(MYSQL *mysql,uint fields,MYSQL_ROW row, ulong *lengths)
{
  uint field;
  ulong pkt_len,len;
  uchar *pos, *prev_pos, *end_pos;
  NET *net= &mysql->net;

  if ((pkt_len=cli_safe_read(mysql)) == packet_error)
    return -1;
  if (pkt_len <= 8 && net->read_pos[0] == 254)
  {
    if (pkt_len > 1)				/* MySQL 4.1 protocol */
    {
      mysql->warning_count= uint2korr(net->read_pos+1);
      mysql->server_status= uint2korr(net->read_pos+3);
    }
    return 1;				/* End of data */
  }
  prev_pos= 0;				/* allowed to write at packet[-1] */
  pos=net->read_pos;
  end_pos=pos+pkt_len;
  for (field=0 ; field < fields ; field++)
  {
    if ((len=(ulong) net_field_length(&pos)) == NULL_LENGTH)
    {						/* null field */
      row[field] = 0;
      *lengths++=0;
    }
    else
    {
      if (unlikely(len > (ulong)(end_pos - pos) || pos > end_pos))
      {
        set_mysql_error(mysql, CR_UNKNOWN_ERROR, unknown_sqlstate);
        return -1;
      }
      row[field] = (char*) pos;
      pos+=len;
      *lengths++=len;
    }
    if (prev_pos)
      *prev_pos=0;				/* Terminate prev field */
    prev_pos=pos;
  }
  row[field]=(char*) prev_pos+1;		/* End of last field */
  *prev_pos=0;					/* Terminate last field */
  return 0;
}


/****************************************************************************
  Init MySQL structure or allocate one
****************************************************************************/

MYSQL * STDCALL
mysql_init(MYSQL *mysql)
{
  if (mysql_server_init(0, NULL, NULL))
    return 0;
  if (!mysql)
  {
    if (!(mysql=(MYSQL*) my_malloc(key_memory_MYSQL,
                                   sizeof(*mysql), MYF(MY_WME | MY_ZEROFILL))))
    {
      set_mysql_error(NULL, CR_OUT_OF_MEMORY, unknown_sqlstate);
      return 0;
    }
    mysql->free_me=1;
  }
  else
    bzero((char*) (mysql), sizeof(*(mysql)));
  mysql->options.connect_timeout= CONNECT_TIMEOUT;
  mysql->charset=default_client_charset_info;
  mysql->options.use_ssl= 1;
  strmov(mysql->net.sqlstate, not_error_sqlstate);

  /*
    Only enable LOAD DATA INFILE by default if configured with
    --enable-local-infile
  */

#if ENABLED_LOCAL_INFILE && !defined(MYSQL_SERVER)
  mysql->options.client_flag|= CLIENT_LOCAL_FILES;
  mysql->auto_local_infile= ENABLED_LOCAL_INFILE == LOCAL_INFILE_MODE_AUTO
                            ? WAIT_FOR_QUERY : ALWAYS_ACCEPT;
#endif

  mysql->options.methods_to_use= MYSQL_OPT_GUESS_CONNECTION;
  mysql->options.report_data_truncation= TRUE;  /* default */

  /*
    By default we don't reconnect because it could silently corrupt data (after
    reconnection you potentially lose table locks, user variables, session
    variables (transactions but they are specifically dealt with in
    mysql_reconnect()).
    This is a change: < 5.0.3 mysql->reconnect was set to 1 by default.
    How this change impacts existing apps:
    - existing apps which relied on the default will see a behaviour change;
    they will have to set reconnect=1 after mysql_real_connect().
    - existing apps which explicitly asked for reconnection (the only way they
    could do it was by setting mysql.reconnect to 1 after mysql_real_connect())
    will not see a behaviour change.
    - existing apps which explicitly asked for no reconnection
    (mysql.reconnect=0) will not see a behaviour change.
  */
  mysql->reconnect= 0;

  DBUG_PRINT("mysql",("mysql: %p", mysql));
  return mysql;
}


/*
  Fill in SSL part of MYSQL structure.
  NB! Errors are not reported until you do mysql_real_connect.
  use_ssl is set in send_client_reply_packet if any ssl option is set.
*/

my_bool STDCALL
mysql_ssl_set(MYSQL *mysql __attribute__((unused)) ,
	      const char *key __attribute__((unused)),
	      const char *cert __attribute__((unused)),
	      const char *ca __attribute__((unused)),
	      const char *capath __attribute__((unused)),
	      const char *cipher __attribute__((unused)))
{
  my_bool result= 0;
  DBUG_ENTER("mysql_ssl_set");
#if defined(HAVE_OPENSSL) && !defined(EMBEDDED_LIBRARY)
  result= (mysql_options(mysql, MYSQL_OPT_SSL_KEY, key) |
           mysql_options(mysql, MYSQL_OPT_SSL_CERT,   cert) |
           mysql_options(mysql, MYSQL_OPT_SSL_CA,     ca) |
           mysql_options(mysql, MYSQL_OPT_SSL_CAPATH, capath) |
           mysql_options(mysql, MYSQL_OPT_SSL_CIPHER, cipher) ?
           1 : 0);
#endif /* HAVE_OPENSSL && !EMBEDDED_LIBRARY */
  mysql->options.use_ssl= TRUE;
  DBUG_RETURN(result);
}


/*
  Free strings in the SSL structure and clear 'use_ssl' flag.
  NB! Errors are not reported until you do mysql_real_connect.
*/

#if defined(HAVE_OPENSSL) && !defined(EMBEDDED_LIBRARY)

static void
mysql_ssl_free(MYSQL *mysql __attribute__((unused)))
{
  DBUG_ENTER("mysql_ssl_free");

  my_free(mysql->options.ssl_key);
  my_free(mysql->options.ssl_cert);
  my_free(mysql->options.ssl_ca);
  my_free(mysql->options.ssl_capath);
  my_free(mysql->options.ssl_cipher);
  if (mysql->options.extension)
  {
    my_free(mysql->options.extension->ssl_crl);
    my_free(mysql->options.extension->ssl_crlpath);
  }
  mysql->options.ssl_key = 0;
  mysql->options.ssl_cert = 0;
  mysql->options.ssl_ca = 0;
  mysql->options.ssl_capath = 0;
  mysql->options.ssl_cipher= 0;
  if (mysql->options.extension)
  {
    mysql->options.extension->ssl_crl = 0;
    mysql->options.extension->ssl_crlpath = 0;
  }
  mysql->options.use_ssl = FALSE;
  DBUG_VOID_RETURN;
}

#endif /* HAVE_OPENSSL && !EMBEDDED_LIBRARY */

/*
  Return the SSL cipher (if any) used for current
  connection to the server.

  SYNOPSYS
    mysql_get_ssl_cipher()
      mysql pointer to the mysql connection

*/

const char * STDCALL
mysql_get_ssl_cipher(MYSQL *mysql __attribute__((unused)))
{
  DBUG_ENTER("mysql_get_ssl_cipher");
#if defined(HAVE_OPENSSL) && !defined(EMBEDDED_LIBRARY)
  if (mysql->net.vio && mysql->net.vio->ssl_arg)
    DBUG_RETURN(SSL_get_cipher_name((SSL*)mysql->net.vio->ssl_arg));
#endif /* HAVE_OPENSSL && !EMBEDDED_LIBRARY */
  DBUG_RETURN(NULL);
}


/*
  Check the server's (subject) Common Name against the
  hostname we connected to

  SYNOPSIS
  ssl_verify_server_cert()
    MYSQL            mysql
    errptr           if we fail, we'll return (a pointer to a string
                     describing) the reason here

  RETURN VALUES
   0 Success
   1 Failed to validate server

 */

#if defined(HAVE_OPENSSL)

#include <openssl/x509v3.h>

static int ssl_verify_server_cert(MYSQL *mysql, const char **errptr, int is_local)
{
  SSL *ssl;
  X509 *server_cert= NULL;
  int ret_validation= 1;

  DBUG_ENTER("ssl_verify_server_cert");
  DBUG_PRINT("enter", ("server_hostname: %s", mysql->host));

  if (!(ssl= (SSL*)mysql->net.vio->ssl_arg))
  {
    *errptr= "No SSL pointer found";
    goto error;
  }

  if (!mysql->host)
  {
    *errptr= "No server hostname supplied";
    goto error;
  }

  if (!(server_cert= SSL_get_peer_certificate(ssl)))
  {
    *errptr= "Could not get server certificate";
    goto error;
  }

  switch (SSL_get_verify_result(ssl))
  {
  case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:     /* OpenSSL */
  case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN:       /* OpenSSL */
  case X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE: /* wolfSSL */
    /*
      If the caller have specified CA - it'll define whether the
      cert is good. Otherwise we'll do more checks.
    */
    ret_validation= (mysql->options.ssl_ca && mysql->options.ssl_ca[0]) ||
                  (mysql->options.ssl_capath && mysql->options.ssl_capath[0]);
    mysql->tls_self_signed_error= *errptr= "SSL certificate is self-signed";
    break;
  case X509_V_OK:
    ret_validation= !is_local &&
                    X509_check_host(server_cert, mysql->host,
                                    strlen(mysql->host), 0, 0) != 1 &&
                    X509_check_ip_asc(server_cert, mysql->host, 0) != 1;
    *errptr= "SSL certificate validation failure";
    break;
  default:
    *errptr= "Failed to verify the server certificate";
    break;
  }

error:
  X509_free(server_cert);
  DBUG_RETURN(ret_validation);
}

#endif /* HAVE_OPENSSL */


/*
  Note that the mysql argument must be initialized with mysql_init()
  before calling mysql_real_connect !
*/

static my_bool cli_read_query_result(MYSQL *mysql);
static MYSQL_RES *cli_use_result(MYSQL *mysql);

int cli_read_change_user_result(MYSQL *mysql)
{
  return cli_safe_read(mysql);
}

static MYSQL_METHODS client_methods=
{
  cli_read_query_result,                       /* read_query_result */
  cli_advanced_command,                        /* advanced_command */
  cli_read_rows,                               /* read_rows */
  cli_use_result,                              /* use_result */
  cli_fetch_lengths,                           /* fetch_lengths */
  cli_flush_use_result,                        /* flush_use_result */
  cli_read_change_user_result,                 /* read_change_user_result */
  NULL                                         /* on_close_free */
#ifndef MYSQL_SERVER
  ,cli_list_fields,                            /* list_fields */
  cli_read_prepare_result,                     /* read_prepare_result */
  cli_stmt_execute,                            /* stmt_execute */
  cli_read_binary_rows,                        /* read_binary_rows */
  cli_unbuffered_fetch,                        /* unbuffered_fetch */
  cli_read_statistics,                         /* read_statistics */
  cli_read_query_result,                       /* next_result */
  cli_read_binary_rows                         /* read_rows_from_cursor */
#endif
};


#include <my_sys.h>
static int
mysql_autodetect_character_set(MYSQL *mysql)
{
  if (mysql->options.charset_name)
    my_free(mysql->options.charset_name);
  if (!(mysql->options.charset_name= opt_strdup(my_default_csname(),MYF(MY_WME))))
    return 1;
  return 0;
}


static void
mysql_set_character_set_with_default_collation(MYSQL *mysql)
{
  const char *save= charsets_dir;
  if (mysql->options.charset_dir)
    charsets_dir=mysql->options.charset_dir;

  if ((mysql->charset= get_charset_by_csname(mysql->options.charset_name,
                                             MY_CS_PRIMARY, MYF(MY_WME|
                                                                MY_UTF8_IS_UTF8MB3))))
  {
    /* Try to set compiled default collation when it's possible. */
    CHARSET_INFO *collation;
    if ((collation= 
         get_charset_by_name(MYSQL_DEFAULT_COLLATION_NAME, MYF(MY_UTF8_IS_UTF8MB3 |
                                                               MY_WME))) &&
                             my_charset_same(mysql->charset, collation))
    {
      mysql->charset= collation;
    }
    else
    {
      /*
        Default compiled collation not found, or is not applicable
        to the requested character set.
        Continue with the default collation of the character set.
      */
    }
  }
  charsets_dir= save;
}


C_MODE_START
int mysql_init_character_set(MYSQL *mysql)
{
  /* Set character set */
  if (!mysql->options.charset_name ||
      !strcmp(mysql->options.charset_name,
              MYSQL_AUTODETECT_CHARSET_NAME))
  {
    if (mysql_autodetect_character_set(mysql))
      return 1;
  }

  mysql_set_character_set_with_default_collation(mysql);

  if (!mysql->charset)
  {
    if (mysql->options.charset_dir)
      set_mysql_extended_error(mysql, CR_CANT_READ_CHARSET, unknown_sqlstate,
                               ER(CR_CANT_READ_CHARSET),
                               mysql->options.charset_name,
                               mysql->options.charset_dir);
    else
    {
      char cs_dir_name[FN_REFLEN];
      get_charsets_dir(cs_dir_name);
      set_mysql_extended_error(mysql, CR_CANT_READ_CHARSET, unknown_sqlstate,
                               ER(CR_CANT_READ_CHARSET),
                               mysql->options.charset_name,
                               cs_dir_name);
    }
    return 1;
  }
  return 0;
}
C_MODE_END

/*********** client side authentication support **************************/

typedef struct st_mysql_client_plugin_AUTHENTICATION auth_plugin_t;
static int client_mpvio_write_packet(struct st_plugin_vio*, const uchar*, int);
static int native_password_auth_client(MYSQL_PLUGIN_VIO *vio, MYSQL *mysql);
static int native_password_auth_hash(MYSQL *mysql, uchar *out, size_t *outlen);
static int old_password_auth_client(MYSQL_PLUGIN_VIO *vio, MYSQL *mysql);

static auth_plugin_t native_password_client_plugin=
{
  MYSQL_CLIENT_AUTHENTICATION_PLUGIN,
  MYSQL_CLIENT_AUTHENTICATION_PLUGIN_INTERFACE_VERSION,
  native_password_plugin_name,
  "R.J.Silk, Sergei Golubchik",
  "Native MySQL authentication",
  {1, 0, 1},
  "GPL",
  NULL,
  NULL,
  NULL,
  NULL,
  native_password_auth_client,
  native_password_auth_hash
};

static auth_plugin_t old_password_client_plugin=
{
  MYSQL_CLIENT_AUTHENTICATION_PLUGIN,
  MYSQL_CLIENT_AUTHENTICATION_PLUGIN_INTERFACE_VERSION,
  old_password_plugin_name,
  "R.J.Silk, Sergei Golubchik",
  "Old MySQL-3.23 authentication",
  {1, 0, 0},
  "GPL",
  NULL,
  NULL,
  NULL,
  NULL,
  old_password_auth_client,
  NULL
};


struct st_mysql_client_plugin *mysql_client_builtins[]=
{
  (struct st_mysql_client_plugin *)&native_password_client_plugin,
  (struct st_mysql_client_plugin *)&old_password_client_plugin,
  0
};


static uchar *
write_length_encoded_string3(uchar *buf, char *string, size_t length)
{
  buf= net_store_length(buf, length);
  memcpy(buf, string, length);
  buf+= length;
  return buf;
}


uchar *
send_client_connect_attrs(MYSQL *mysql, uchar *buf)
{
  /* check if the server supports connection attributes */
  if (mysql->server_capabilities & CLIENT_CONNECT_ATTRS)
  {

    /* Always store the length if the client supports it */
    buf= net_store_length(buf,
                          mysql->options.extension ?
                          mysql->options.extension->connection_attributes_length :
                          0);

    /* check if we have connection attributes */
    if (mysql->options.extension &&
        my_hash_inited(&mysql->options.extension->connection_attributes))
    {
      HASH *attrs= &mysql->options.extension->connection_attributes;
      ulong idx;

      /* loop over and dump the connection attributes */
      for (idx= 0; idx < attrs->records; idx++)
      {
        LEX_STRING *attr= (LEX_STRING *) my_hash_element(attrs, idx);
        LEX_STRING *key= attr, *value= attr + 1;

        /* we can't have zero length keys */
        DBUG_ASSERT(key->length);

        buf= write_length_encoded_string3(buf, key->str, key->length);
        buf= write_length_encoded_string3(buf, value->str, value->length);
      }
    }
  }
  return buf;
}


static size_t get_length_store_length(size_t length)
{
  /* as defined in net_store_length */
  #define MAX_VARIABLE_STRING_LENGTH 9
  uchar length_buffer[MAX_VARIABLE_STRING_LENGTH], *ptr;

  ptr= net_store_length(length_buffer, length);

  return ptr - &length_buffer[0];
}


/* this is a "superset" of MYSQL_PLUGIN_VIO, in C++ I use inheritance */
typedef struct {
  int (*read_packet)(struct st_plugin_vio *vio, uchar **buf);
  int (*write_packet)(struct st_plugin_vio *vio, const uchar *pkt, int pkt_len);
  void (*info)(struct st_plugin_vio *vio, struct st_plugin_vio_info *info);
  /* -= end of MYSQL_PLUGIN_VIO =- */
  MYSQL *mysql;
  auth_plugin_t *plugin;            /**< what plugin we're under */
  const char *db;
  struct {
    uchar *pkt;                     /**< pointer into NET::buff */
    uint pkt_len;
  } cached_server_reply;
  int packets_read, packets_written; /**< counters for send/received packets */
  int mysql_change_user;            /**< if it's mysql_change_user() */
  int last_read_packet_len;         /**< the length of the last *read* packet */
} MCPVIO_EXT;


/*
  Write 1-8 bytes of string length header information to dest depending on
  value of src_len, then copy src_len bytes from src to dest.
 
 @param dest Destination buffer of size src_len+8
 @param dest_end One byte past the end of the dest buffer
 @param src Source buff of size src_len
 @param src_end One byte past the end of the src buffer
 
 @return pointer dest+src_len+header size or NULL if 
*/

static uchar *write_length_encoded_string4(uchar *dst, size_t dst_len,
                                           const uchar *src, size_t src_len)
{
  uchar *to= safe_net_store_length(dst, dst_len, src_len);
  if (to == NULL)
    return NULL;
  memcpy(to, src, src_len);
  return to + src_len;
}

/**
  sends a COM_CHANGE_USER command with a caller provided payload

  Packet format:
   
    Bytes       Content
    -----       ----
    n           user name - \0-terminated string
    n           password
                  3.23 scramble - \0-terminated string (9 bytes)
                  otherwise - length (1 byte) coded
    n           database name - \0-terminated string
    2           character set number (if the server >= 4.1.x)
    n           client auth plugin name - \0-terminated string,
                  (if the server supports plugin auth)

  @retval 0 ok
  @retval 1 error
*/
static int send_change_user_packet(MCPVIO_EXT *mpvio,
                                   const uchar *data, int data_len)
{
  MYSQL *mysql= mpvio->mysql;
  char *buff, *end;
  int res= 1;
  size_t connect_attrs_len=
    (mysql->server_capabilities & CLIENT_CONNECT_ATTRS &&
     mysql->options.extension) ?
    mysql->options.extension->connection_attributes_length : 0;

  buff= my_alloca(USERNAME_LENGTH + data_len + 1 + NAME_LEN + 2 + NAME_LEN +
                  connect_attrs_len + 9 /* for the length of the attrs */);

  end= strmake(buff, mysql->user, USERNAME_LENGTH) + 1;

  if (!data_len)
    *end++= 0;
  else
  {
    if (mysql->client_flag & CLIENT_SECURE_CONNECTION)
    {
      DBUG_ASSERT(data_len <= 255);
      if (data_len > 255)
      {
        set_mysql_error(mysql, CR_MALFORMED_PACKET, unknown_sqlstate);
        goto error;
      }
      *end++= data_len;
    }
    else
    {
      DBUG_ASSERT(data_len == SCRAMBLE_LENGTH_323 + 1);
      DBUG_ASSERT(data[SCRAMBLE_LENGTH_323] == 0);
    }
    memcpy(end, data, data_len);
    end+= data_len;
  }
  end= strmake(end, mpvio->db ? mpvio->db : "", NAME_LEN) + 1;

  if (mysql->server_capabilities & CLIENT_PROTOCOL_41)
  {
    int2store(end, (ushort) mysql->charset->number);
    end+= 2;
  }

  if (mysql->server_capabilities & CLIENT_PLUGIN_AUTH)
    end= strmake(end, mpvio->plugin->name, NAME_LEN) + 1;

  end= (char *) send_client_connect_attrs(mysql, (uchar *) end);

  res= simple_command(mysql, COM_CHANGE_USER,
                      (uchar*)buff, (ulong)(end-buff), 1);

error:
  my_afree(buff);
  return res;
}

#define MAX_CONNECTION_ATTR_STORAGE_LENGTH 65536

/**
  sends a client authentication packet (second packet in the 3-way handshake)

  Packet format (when the server is 4.0 or earlier):
   
    Bytes       Content
    -----       ----
    2           client capabilities
    3           max packet size
    n           user name, \0-terminated
    9           scramble_323, \0-terminated

  Packet format (when the server is 4.1 or newer):
   
    Bytes       Content
    -----       ----
    4           client capabilities
    4           max packet size
    1           charset number
    23          reserved (always 0)
    n           user name, \0-terminated
    n           plugin auth data (e.g. scramble), length encoded
    n           database name, \0-terminated
                (if CLIENT_CONNECT_WITH_DB is set in the capabilities)
    n           client auth plugin name - \0-terminated string,
                (if CLIENT_PLUGIN_AUTH is set in the capabilities)

  @retval 0 ok
  @retval 1 error
*/
static int send_client_reply_packet(MCPVIO_EXT *mpvio,
                                    const uchar *data, int data_len)
{
  MYSQL *mysql= mpvio->mysql;
  NET *net= &mysql->net;
  enum enum_vio_type vio_type= net->vio->type;
  char *buff, *end;
  size_t buff_size;
  size_t connect_attrs_len=
    (mysql->server_capabilities & CLIENT_CONNECT_ATTRS &&
     mysql->options.extension) ?
    mysql->options.extension->connection_attributes_length : 0;

  DBUG_ASSERT(connect_attrs_len < MAX_CONNECTION_ATTR_STORAGE_LENGTH);

  /*
    see end= buff+32 below, fixed size of the packet is 32 bytes.
     +9 because data is a length encoded binary where meta data size is max 9.
  */
  buff_size= 33 + USERNAME_LENGTH + data_len + 9 + NAME_LEN + NAME_LEN + connect_attrs_len + 9;
  buff= my_alloca(buff_size);

  mysql->client_flag|= mysql->options.client_flag;
  mysql->client_flag|= CLIENT_CAPABILITIES;

  if (mysql->client_flag & CLIENT_MULTI_STATEMENTS)
    mysql->client_flag|= CLIENT_MULTI_RESULTS;

#ifdef HAVE_OPENSSL
  if (mysql->options.ssl_key || mysql->options.ssl_cert ||
      mysql->options.ssl_ca || mysql->options.ssl_capath ||
      mysql->options.ssl_cipher)
    mysql->options.use_ssl = 1;
  if (mysql->options.use_ssl)
    mysql->client_flag|= CLIENT_SSL;
#endif /* HAVE_OPENSSL */

  if (mpvio->db)
    mysql->client_flag|= CLIENT_CONNECT_WITH_DB;

  if (vio_type == VIO_TYPE_NAMEDPIPE)
  {
    mysql->server_capabilities&= ~CLIENT_SSL;
    mysql->options.use_ssl= 0;
  }

  /* Remove options that server doesn't support */
  mysql->client_flag= mysql->client_flag &
                       (~(CLIENT_COMPRESS | CLIENT_SSL | CLIENT_PROTOCOL_41) 
                       | mysql->server_capabilities);

#ifndef HAVE_COMPRESS
  mysql->client_flag&= ~CLIENT_COMPRESS;
#endif

  if (mysql->client_flag & CLIENT_PROTOCOL_41)
  {
    /* 4.1 server and 4.1 client has a 32 byte option flag */
    int4store(buff,mysql->client_flag);
    int4store(buff+4, net->max_packet_size);
    buff[8]= (char) mysql->charset->number;
    bzero(buff+9, 32-9);
    end= buff+32;
  }
  else
  {
    int2store(buff, mysql->client_flag);
    int3store(buff+2, net->max_packet_size);
    end= buff+5;
  }

  /*
     If client uses ssl and client also has to verify the server
     certificate, a ssl connection is required.
     If the server does not support ssl, we abort the connection.
  */
  if (mysql->options.use_ssl && !(mysql->server_capabilities & CLIENT_SSL) &&
      (!mysql->options.extension ||
       !mysql->options.extension->tls_allow_invalid_server_cert))
  {
    set_mysql_extended_error(mysql, CR_SSL_CONNECTION_ERROR, unknown_sqlstate,
                             ER(CR_SSL_CONNECTION_ERROR),
                             "SSL is required, but the server does not "
                             "support it");
    goto error;
  }

#ifdef HAVE_OPENSSL
  if (mysql->client_flag & CLIENT_SSL)
  {
    /* Do the SSL layering. */
    struct st_mysql_options *options= &mysql->options;
    struct st_VioSSLFd *ssl_fd;
    enum enum_ssl_init_error ssl_init_error;
    const char *cert_error;
    unsigned long ssl_error;

    /*
      Send mysql->client_flag, max_packet_size - unencrypted otherwise
      the server does not know we want to do SSL
    */
    if (my_net_write(net, (uchar*)buff, (size_t) (end-buff)) || net_flush(net))
    {
      set_mysql_extended_error(mysql, CR_SERVER_LOST, unknown_sqlstate,
                               ER(CR_SERVER_LOST_EXTENDED),
                               "sending connection information to server",
                               errno);
      goto error;
    }

    /* Create the VioSSLConnectorFd - init SSL and load certs */
    if (!(ssl_fd= new_VioSSLConnectorFd(options->ssl_key,
                                        options->ssl_cert,
                                        options->ssl_ca,
                                        options->ssl_capath,
                                        options->ssl_cipher,
                                        &ssl_init_error,
                                        options->extension ? 
                                        options->extension->ssl_crl : NULL,
                                        options->extension ? 
                                        options->extension->ssl_crlpath : NULL)))
    {
      set_mysql_extended_error(mysql, CR_SSL_CONNECTION_ERROR, unknown_sqlstate,
                               ER(CR_SSL_CONNECTION_ERROR), sslGetErrString(ssl_init_error));
      goto error;
    }
    mysql->connector_fd= (uchar *) ssl_fd;

    /* Connect to the server */
    DBUG_PRINT("info", ("IO layer change in progress..."));
    if (sslconnect(ssl_fd, net->vio,
                   (long) (mysql->options.connect_timeout), &ssl_error))
    {    
      char buf[512];
      ERR_error_string_n(ssl_error, buf, 512);
      buf[511]= 0;
      set_mysql_extended_error(mysql, CR_SSL_CONNECTION_ERROR, unknown_sqlstate,
                               ER(CR_SSL_CONNECTION_ERROR),
                               buf);
      goto error;
    }
    DBUG_PRINT("info", ("IO layer change done!"));

    /* Verify server cert */
    if ((!mysql->options.extension ||
         !mysql->options.extension->tls_allow_invalid_server_cert) &&
        ssl_verify_server_cert(mysql, &cert_error, vio_type == VIO_TYPE_SOCKET))
    {
      set_mysql_extended_error(mysql, CR_SSL_CONNECTION_ERROR, unknown_sqlstate,
                               ER(CR_SSL_CONNECTION_ERROR), cert_error);
      goto error;
    }
    if (mysql->tls_self_signed_error)
    {
      /*
        If the transport is secure (see opt_require_secure_transport) we
        allow a self-signed cert as we know it came from the server.

        If no password or plugin uses insecure protocol - refuse the cert.

        Otherwise one last cert check after auth.
      */
      if (vio_type == VIO_TYPE_SOCKET)
        mysql->tls_self_signed_error= 0;
      else if (!mysql->passwd || !mysql->passwd[0] ||
               !mpvio->plugin->hash_password_bin)
      {
        set_mysql_extended_error(mysql, CR_SSL_CONNECTION_ERROR, unknown_sqlstate,
                    ER(CR_SSL_CONNECTION_ERROR), mysql->tls_self_signed_error);
        goto error;
      }
    }
  }
#endif /* HAVE_OPENSSL */

  DBUG_PRINT("info",("Server version = '%s'  capabilities: %lu  status: %u  client_flag: %lu",
		     mysql->server_version, mysql->server_capabilities,
		     mysql->server_status, mysql->client_flag));

  /* This needs to be changed as it's not useful with big packets */
  if (mysql->user[0])
    strmake(end, mysql->user, USERNAME_LENGTH);
  else
    read_user_name(end);

  /* We have to handle different version of handshake here */
  DBUG_PRINT("info",("user: %s",end));
  end= strend(end) + 1;
  if (data_len)
  {
    if (mysql->server_capabilities & CLIENT_SECURE_CONNECTION)
    {
      if (mysql->server_capabilities & CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA)
        end= (char*)write_length_encoded_string4((uchar*)end,
                                                 buff_size, data, data_len);
      else
      {
        if (data_len > 255)
          goto error;
        *end++= data_len;
        memcpy(end, data, data_len);
        end+= data_len;
      }
      if (end == NULL)
        goto error;
    }
    else
    {
      DBUG_ASSERT(data_len == SCRAMBLE_LENGTH_323 + 1); /* incl. \0 at the end */
      memcpy(end, data, data_len);
      end+= data_len;
    }
  }
  else
    *end++= 0;

  /* Add database if needed */
  if (mpvio->db && (mysql->server_capabilities & CLIENT_CONNECT_WITH_DB))
  {
    end= strmake(end, mpvio->db, NAME_LEN) + 1;
    mysql->db= my_strdup(key_memory_MYSQL, mpvio->db, MYF(MY_WME));
  }

  if (mysql->server_capabilities & CLIENT_PLUGIN_AUTH)
    end= strmake(end, mpvio->plugin->name, NAME_LEN) + 1;

  end= (char *) send_client_connect_attrs(mysql, (uchar *) end);

  /* Write authentication package */
  if (my_net_write(net, (uchar*) buff, (size_t) (end-buff)) || net_flush(net))
  {
    set_mysql_extended_error(mysql, CR_SERVER_LOST, unknown_sqlstate,
                             ER(CR_SERVER_LOST_EXTENDED),
                             "sending authentication information",
                             errno);
    goto error;
  }
  my_afree(buff);
  return 0;
  
error:
  my_afree(buff);
  return 1;
}


/**
  vio->read_packet() callback method for client authentication plugins

  This function is called by a client authentication plugin, when it wants
  to read data from the server.
*/
static int client_mpvio_read_packet(struct st_plugin_vio *mpv, uchar **buf)
{
  MCPVIO_EXT *mpvio= (MCPVIO_EXT*)mpv;
  MYSQL *mysql= mpvio->mysql;
  ulong  pkt_len;

  /* there are cached data left, feed it to a plugin */
  if (mpvio->cached_server_reply.pkt)
  {
    *buf= mpvio->cached_server_reply.pkt;
    mpvio->cached_server_reply.pkt= 0;
    mpvio->packets_read++;
    return mpvio->cached_server_reply.pkt_len;
  }

  if (mpvio->packets_read == 0)
  {
    /*
      the server handshake packet came from the wrong plugin,
      or it's mysql_change_user(). Either way, there is no data
      for a plugin to read. send a dummy packet to the server
      to initiate a dialog.
    */
    if (client_mpvio_write_packet(mpv, 0, 0))
      return (int)packet_error;
  }

  /* otherwise read the data */
  pkt_len= (*mysql->methods->read_change_user_result)(mysql);
  mpvio->last_read_packet_len= pkt_len;
  *buf= mysql->net.read_pos;

  /* was it a request to change plugins ? */
  if (pkt_len == packet_error || **buf == 254)
    return (int)packet_error; /* if yes, this plugin shan't continue */

  /*
    the server sends \1\255 or \1\254 instead of just \255 or \254 -
    for us to not confuse it with an error or "change plugin" packets.
    We remove this escaping \1 here.

    See also server_mpvio_write_packet() where the escaping is done.
  */
  if (pkt_len && **buf == 1)
  {
    (*buf)++;
    pkt_len--;
  }
  mpvio->packets_read++;
  return pkt_len;
}


/**
  vio->write_packet() callback method for client authentication plugins

  This function is called by a client authentication plugin, when it wants
  to send data to the server.

  It transparently wraps the data into a change user or authentication
  handshake packet, if necessary.
*/
static int client_mpvio_write_packet(struct st_plugin_vio *mpv,
                                     const uchar *pkt, int pkt_len)
{
  int res;
  MCPVIO_EXT *mpvio= (MCPVIO_EXT*)mpv;

  if (mpvio->packets_written == 0)
  {
    if (mpvio->mysql_change_user)
      res= send_change_user_packet(mpvio, pkt, pkt_len);
    else
      res= send_client_reply_packet(mpvio, pkt, pkt_len);
  }
  else
  {
    NET *net= &mpvio->mysql->net;
    if (mpvio->mysql->thd)
      res= 1; /* no chit-chat in embedded */
    else
      res= my_net_write(net, pkt, pkt_len) || net_flush(net);
    if (res)
      set_mysql_extended_error(mpvio->mysql, CR_SERVER_LOST, unknown_sqlstate,
                               ER(CR_SERVER_LOST_EXTENDED),
                               "sending authentication information",
                               errno);
  }
  mpvio->packets_written++;
  return res;
}


/**
  fills MYSQL_PLUGIN_VIO_INFO structure with the information about the
  connection
*/
void mpvio_info(Vio *vio, MYSQL_PLUGIN_VIO_INFO *info)
{
  bzero(info, sizeof(*info));
  switch (vio->type) {
  case VIO_TYPE_TCPIP:
    info->protocol= MYSQL_VIO_TCP;
    info->socket= (int)vio_fd(vio);
    return;
  case VIO_TYPE_SOCKET:
    info->protocol= MYSQL_VIO_SOCKET;
    info->socket= (int)vio_fd(vio);
    return;
  case VIO_TYPE_SSL:
    {
      struct sockaddr addr;
      SOCKET_SIZE_TYPE addrlen= sizeof(addr);
      if (getsockname(vio_fd(vio), &addr, &addrlen))
        return;
      info->protocol= addr.sa_family == AF_UNIX ?
        MYSQL_VIO_SOCKET : MYSQL_VIO_TCP;
      info->socket= (int)vio_fd(vio);
      info->tls= 1;
      return;
    }
#ifdef _WIN32
  case VIO_TYPE_NAMEDPIPE:
    info->protocol= MYSQL_VIO_PIPE;
    info->handle= vio->hPipe;
    return;
#endif
  default: DBUG_ASSERT(0);
  }
}


static void client_mpvio_info(MYSQL_PLUGIN_VIO *vio,
                              MYSQL_PLUGIN_VIO_INFO *info)
{
  MCPVIO_EXT *mpvio= (MCPVIO_EXT*)vio;
  mpvio_info(mpvio->mysql->net.vio, info);
}


/**
  Client side of the plugin driver authentication.

  @note this is used by both the mysql_real_connect and mysql_change_user

  @param mysql       mysql
  @param data        pointer to the plugin auth data (scramble) in the
                     handshake packet
  @param data_len    the length of the data
  @param data_plugin a plugin that data were prepared for
                     or 0 if it's mysql_change_user()
  @param db          initial db to use, can be 0

  @retval 0 ok
  @retval 1 error
*/
int run_plugin_auth(MYSQL *mysql, char *data, uint data_len,
                    const char *data_plugin, const char *db)
{
  const char    *auth_plugin_name;
  auth_plugin_t *auth_plugin;
  MCPVIO_EXT    mpvio;
  ulong		pkt_length;
  int           res;

  DBUG_ENTER ("run_plugin_auth");
  /* determine the default/initial plugin to use */
  if (mysql->options.extension && mysql->options.extension->default_auth &&
      mysql->server_capabilities & CLIENT_PLUGIN_AUTH)
  {
    auth_plugin_name= mysql->options.extension->default_auth;
    if (!(auth_plugin= (auth_plugin_t*) mysql_client_find_plugin(mysql,
                       auth_plugin_name, MYSQL_CLIENT_AUTHENTICATION_PLUGIN)))
      DBUG_RETURN (1); /* oops, not found */
  }
  else
  {
    auth_plugin= mysql->server_capabilities & CLIENT_PROTOCOL_41 ?
      &native_password_client_plugin : &old_password_client_plugin;
    auth_plugin_name= auth_plugin->name;
  }

  DBUG_PRINT ("info", ("using plugin %s", auth_plugin_name));

  mysql->net.last_errno= 0; /* just in case */

  if (data_plugin && strcmp(data_plugin, auth_plugin_name))
  {
    /* data was prepared for a different plugin, don't show it to this one */
    data= 0;
    data_len= 0;
  }

  mpvio.mysql_change_user= data_plugin == 0;
  mpvio.cached_server_reply.pkt= (uchar*)data;
  mpvio.cached_server_reply.pkt_len= data_len;
  mpvio.read_packet= client_mpvio_read_packet;
  mpvio.write_packet= client_mpvio_write_packet;
  mpvio.info= client_mpvio_info;
  mpvio.mysql= mysql;
  mpvio.packets_read= mpvio.packets_written= 0;
  mpvio.db= db;
  mpvio.plugin= auth_plugin;

  DBUG_EXECUTE_IF("client_delay_run_plugin_auth", my_sleep(400000););

  res= auth_plugin->authenticate_user((struct st_plugin_vio *)&mpvio, mysql);
  DBUG_PRINT ("info", ("authenticate_user returned %s", 
                       res == CR_OK ? "CR_OK" : 
                       res == CR_ERROR ? "CR_ERROR" :
                       res == CR_OK_HANDSHAKE_COMPLETE ? 
                         "CR_OK_HANDSHAKE_COMPLETE" : "error"));

  compile_time_assert(CR_OK == -1);
  compile_time_assert(CR_ERROR == 0);
  if (res > CR_OK && (mysql->net.last_errno || mysql->net.read_pos[0] != 254))
  {
    /*
      the plugin returned an error. write it down in mysql,
      unless the error code is CR_ERROR and mysql->net.last_errno
      is already set (the plugin has done it)
    */
    DBUG_PRINT ("info", ("res=%d", res));
    if (res > CR_ERROR)
      set_mysql_error(mysql, res, unknown_sqlstate);
    else
      if (!mysql->net.last_errno)
        set_mysql_error(mysql, CR_UNKNOWN_ERROR, unknown_sqlstate);
    DBUG_RETURN (1);
  }

  /* read the OK packet (or use the cached value in mysql->net.read_pos */
  if (res == CR_OK)
    pkt_length= (*mysql->methods->read_change_user_result)(mysql);
  else /* res == CR_OK_HANDSHAKE_COMPLETE */
    pkt_length= mpvio.last_read_packet_len;

  DBUG_PRINT ("info", ("OK packet length=%lu", pkt_length));
  if (pkt_length == packet_error)
  {
    if (mysql->net.last_errno == CR_SERVER_LOST)
      set_mysql_extended_error(mysql, CR_SERVER_LOST, unknown_sqlstate,
                               ER(CR_SERVER_LOST_EXTENDED),
                               "reading authorization packet",
                               errno);
    DBUG_RETURN (1);
  }

  if (mysql->net.read_pos[0] == 254)
  {
    /* The server asked to use a different authentication plugin */
    if (pkt_length == 1)
    {
      /* old "use short scramble" packet */
      DBUG_PRINT ("info", ("old use short scramble packet from server"));
      auth_plugin_name= old_password_plugin_name;
      mpvio.cached_server_reply.pkt= (uchar*)mysql->scramble;
      mpvio.cached_server_reply.pkt_len= SCRAMBLE_LENGTH + 1;
    }
    else
    {
      /* new "use different plugin" packet */
      uint len;
      auth_plugin_name= (char*)mysql->net.read_pos + 1;
      len= (uint)strlen(auth_plugin_name); /* safe as my_net_read always appends \0 */
      mpvio.cached_server_reply.pkt_len= pkt_length - len - 2;
      mpvio.cached_server_reply.pkt= mysql->net.read_pos + len + 2;
      DBUG_PRINT ("info", ("change plugin packet from server for plugin %s",
                           auth_plugin_name));
    }

    if (!(auth_plugin= (auth_plugin_t *) mysql_client_find_plugin(mysql,
                         auth_plugin_name, MYSQL_CLIENT_AUTHENTICATION_PLUGIN)))
      DBUG_RETURN (1);

    /* refuse insecure plugin if TLS is in doubt */
    if (mysql->tls_self_signed_error && !auth_plugin->hash_password_bin)
    {
      set_mysql_extended_error(mysql, CR_SSL_CONNECTION_ERROR, unknown_sqlstate,
                  ER(CR_SSL_CONNECTION_ERROR), mysql->tls_self_signed_error);
      DBUG_RETURN (1);
    }

    mpvio.plugin= auth_plugin;
    res= auth_plugin->authenticate_user((struct st_plugin_vio *)&mpvio, mysql);

    DBUG_PRINT ("info", ("second authenticate_user returned %s", 
                         res == CR_OK ? "CR_OK" : 
                         res == CR_ERROR ? "CR_ERROR" :
                         res == CR_OK_HANDSHAKE_COMPLETE ? 
                         "CR_OK_HANDSHAKE_COMPLETE" : "error"));
    if (res > CR_OK)
    {
      if (res > CR_ERROR)
        set_mysql_error(mysql, res, unknown_sqlstate);
      else
        if (!mysql->net.last_errno)
          set_mysql_error(mysql, CR_UNKNOWN_ERROR, unknown_sqlstate);
      DBUG_RETURN (1);
    }

    if (res != CR_OK_HANDSHAKE_COMPLETE)
    {
      /* Read what server thinks about out new auth message report */
      if ((pkt_length= cli_safe_read(mysql)) == packet_error)
      {
        if (mysql->net.last_errno == CR_SERVER_LOST)
          set_mysql_extended_error(mysql, CR_SERVER_LOST, unknown_sqlstate,
                                   ER(CR_SERVER_LOST_EXTENDED),
                                   "reading final connect information",
                                   errno);
        DBUG_RETURN (1);
      }
    }
  }
  /*
    net->read_pos[0] should always be 0 here if the server implements
    the protocol correctly
  */
  if (mysql->net.read_pos[0] != 0)
    DBUG_RETURN(1);
  if (!mysql->tls_self_signed_error)
    DBUG_RETURN(0);

  /* Last attempt to validate the cert: compare cert info packet */
  DBUG_ASSERT(mysql->options.use_ssl);
  DBUG_ASSERT(mysql->net.vio->ssl_arg);
  DBUG_ASSERT(!mysql->options.extension ||
              !mysql->options.extension->tls_allow_invalid_server_cert);
  DBUG_ASSERT(!mysql->options.ssl_ca || !mysql->options.ssl_ca[0]);
  DBUG_ASSERT(!mysql->options.ssl_capath || !mysql->options.ssl_capath[0]);
  DBUG_ASSERT(auth_plugin->hash_password_bin);
  DBUG_ASSERT(mysql->passwd[0]);

  parse_ok_packet(mysql, pkt_length); /* set mysql->info */
  if (mysql->info && mysql->info[0] == '\1')
  {
    uchar fp[128], buf[1024], digest[256/8];
    size_t buflen= sizeof(buf);
    uint fplen= sizeof(fp);
    char *hexsig= mysql->info + 1, hexdigest[sizeof(digest)*2+1];
    X509 *cert= SSL_get_peer_certificate((SSL*)mysql->net.vio->ssl_arg);
    X509_digest(cert, EVP_sha256(), fp, &fplen);
    X509_free(cert);
    auth_plugin->hash_password_bin(mysql, buf, &buflen);
    my_sha256_multi(digest, buf, buflen, mysql->scramble, SCRAMBLE_LENGTH,
                    fp, fplen, NULL);
    mysql->info= NULL; /* no need to confuse the client with binary info */

    octet2hex(hexdigest, digest, sizeof(digest));
    if (strcmp(hexdigest, hexsig) == 0)
      DBUG_RETURN(0); /* phew. self-signed certificate is validated! */
  }

  set_mysql_extended_error(mysql, CR_SSL_CONNECTION_ERROR, unknown_sqlstate,
                    ER(CR_SSL_CONNECTION_ERROR), mysql->tls_self_signed_error);
  DBUG_RETURN(1);
}


static int
connect_sync(MYSQL *mysql, NET *net, my_socket fd,
                      struct sockaddr *name, uint namelen)
{
  int vio_timeout = get_vio_connect_timeout(mysql);

  return vio_socket_connect(net->vio, name, namelen, vio_timeout);
}


/** set some default attributes */
static int
set_connect_attributes(MYSQL *mysql, char *buff, size_t buf_len)
{
  int rc= 0;

  /*
    Clean up any values set by the client code. We want these options as
    consistent as possible
  */
  rc+= mysql_options(mysql, MYSQL_OPT_CONNECT_ATTR_DELETE, "_client_name");
  rc+= mysql_options(mysql, MYSQL_OPT_CONNECT_ATTR_DELETE, "_os");
  rc+= mysql_options(mysql, MYSQL_OPT_CONNECT_ATTR_DELETE, "_platform");
  rc+= mysql_options(mysql, MYSQL_OPT_CONNECT_ATTR_DELETE, "_server_host");
  rc+= mysql_options(mysql, MYSQL_OPT_CONNECT_ATTR_DELETE, "_pid");
  rc+= mysql_options(mysql, MYSQL_OPT_CONNECT_ATTR_DELETE, "_thread");
  rc+= mysql_options(mysql, MYSQL_OPT_CONNECT_ATTR_DELETE, "_client_version");

  /*
   Now let's set up some values
  */
  rc+= mysql_options4(mysql, MYSQL_OPT_CONNECT_ATTR_ADD,
                     "_client_name", "libmysql");
  rc+= mysql_options4(mysql, MYSQL_OPT_CONNECT_ATTR_ADD,
                      "_client_version", PACKAGE_VERSION);
  rc+= mysql_options4(mysql, MYSQL_OPT_CONNECT_ATTR_ADD,
                      "_os", SYSTEM_TYPE);
  rc+= mysql_options4(mysql, MYSQL_OPT_CONNECT_ATTR_ADD,
                      "_platform", MACHINE_TYPE);
  rc+= mysql_options4(mysql, MYSQL_OPT_CONNECT_ATTR_ADD,
                      "_server_host", mysql->host);                      
  snprintf(buff, buf_len, "%lu", (ulong) getpid());
  rc+= mysql_options4(mysql, MYSQL_OPT_CONNECT_ATTR_ADD, "_pid", buff);

#ifdef _WIN32
  snprintf(buff, buf_len, "%lu", (ulong) GetCurrentThreadId());
  rc+= mysql_options4(mysql, MYSQL_OPT_CONNECT_ATTR_ADD, "_thread", buff);
#endif

  return rc > 0 ? 1 : 0;
}


MYSQL * STDCALL 
CLI_MYSQL_REAL_CONNECT(MYSQL *mysql,const char *host, const char *user,
		       const char *passwd, const char *db,
		       uint port, const char *unix_socket,ulong client_flag)
{
  char		buff[NAME_LEN+USERNAME_LENGTH+100];
  int           scramble_data_len, UNINIT_VAR(pkt_scramble_len);
  char          *end,*host_info= 0, *server_version_end, *pkt_end;
  char          *scramble_data;
  const char    *scramble_plugin;
  ulong		pkt_length;
  NET		*net= &mysql->net;
#ifdef _WIN32
  HANDLE	hPipe=INVALID_HANDLE_VALUE;
#endif
#ifdef HAVE_SYS_UN_H
  struct	sockaddr_un UNIXaddr;
#endif
  DBUG_ENTER("mysql_real_connect");

  DBUG_PRINT("enter",("host: %s  db: %s  user: %s (client)",
		      host ? host : "(Null)",
		      db ? db : "(Null)",
		      user ? user : "(Null)"));

  /* Test whether we're already connected */
  if (net->vio)
  {
    set_mysql_error(mysql, CR_ALREADY_CONNECTED, unknown_sqlstate);
    DBUG_RETURN(0);
  }

  mysql->methods= &client_methods;
  mysql->client_flag=0;			/* For handshake */
  mysql->tls_self_signed_error= 0;

  /* use default options */
  if (mysql->options.my_cnf_file || mysql->options.my_cnf_group)
  {
    mysql_read_default_options(&mysql->options,
			       (mysql->options.my_cnf_file ?
				mysql->options.my_cnf_file : "my"),
			       mysql->options.my_cnf_group);
    my_free(mysql->options.my_cnf_file);
    my_free(mysql->options.my_cnf_group);
    mysql->options.my_cnf_file=mysql->options.my_cnf_group=0;
    if (mysql->options.protocol == UINT_MAX32)
      goto error;
  }

  /* Some empty-string-tests are done because of ODBC */
  if (!host || !host[0])
    host=mysql->options.host;
  if (!user || !user[0])
  {
    user=mysql->options.user;
    if (!user)
      user= "";
  }
  if (!passwd)
  {
    passwd=mysql->options.password;
#if !defined(DONT_USE_MYSQL_PWD) && !defined(MYSQL_SERVER)
    if (!passwd)
      passwd=getenv("MYSQL_PWD");		/* get it from environment */
#endif
    if (!passwd)
      passwd= "";
  }
  if (!db || !db[0])
    db=mysql->options.db;
  if (!port)
    port=mysql->options.port;
  if (!unix_socket)
    unix_socket=mysql->options.unix_socket;

  mysql->server_status=SERVER_STATUS_AUTOCOMMIT;
  DBUG_PRINT("info", ("Connecting"));

  /*
    Part 0: Grab a socket and connect it to the server
  */

#if defined(HAVE_SYS_UN_H)
  if (!net->vio &&
      (!mysql->options.protocol ||
       mysql->options.protocol == MYSQL_PROTOCOL_SOCKET) &&
      (unix_socket || mysql_unix_port) &&
      (!host || !strcmp(host,LOCAL_HOST)))
  {
    my_socket sock= socket(AF_UNIX, SOCK_STREAM, 0);
    DBUG_PRINT("info", ("Using socket"));
    if (sock == INVALID_SOCKET)
    {
      set_mysql_extended_error(mysql, CR_SOCKET_CREATE_ERROR,
                               unknown_sqlstate,
                               ER(CR_SOCKET_CREATE_ERROR),
                               socket_errno);
      goto error;
    }

    net->vio= vio_new(sock, VIO_TYPE_SOCKET,
                      VIO_LOCALHOST | VIO_BUFFERED_READ);
    if (!net->vio)
    {
      DBUG_PRINT("error",("Unknown protocol %d ", mysql->options.protocol));
      set_mysql_error(mysql, CR_CONN_UNKNOW_PROTOCOL, unknown_sqlstate);
      closesocket(sock);
      goto error;
    }

    host= LOCAL_HOST;
    if (!unix_socket)
      unix_socket= mysql_unix_port;
    host_info= (char*) ER(CR_LOCALHOST_CONNECTION);
    DBUG_PRINT("info", ("Using UNIX sock '%s'", unix_socket));

    bzero((char*) &UNIXaddr, sizeof(UNIXaddr));
    UNIXaddr.sun_family= AF_UNIX;
    strmake_buf(UNIXaddr.sun_path, unix_socket);
    if (connect_sync(mysql, net, sock,
                              (struct sockaddr *) &UNIXaddr, sizeof(UNIXaddr)))
    {
      DBUG_PRINT("error",("Got error %d on connect to local server",
			  socket_errno));
      set_mysql_extended_error(mysql, CR_CONNECTION_ERROR,
                               unknown_sqlstate,
                               ER(CR_CONNECTION_ERROR),
                               unix_socket, socket_errno);
      vio_delete(net->vio);
      net->vio= 0;
      goto error;
    }
    mysql->options.protocol=MYSQL_PROTOCOL_SOCKET;
  }
#elif defined(_WIN32)
  if (!net->vio &&
      (mysql->options.protocol == MYSQL_PROTOCOL_PIPE ||
       (host && !strcmp(host,LOCAL_HOST_NAMEDPIPE))))
  {
    if ((hPipe= create_named_pipe(mysql, mysql->options.connect_timeout,
                                  (char**) &host, (char**) &unix_socket)) ==
	INVALID_HANDLE_VALUE)
    {
      DBUG_PRINT("error",
		 ("host: '%s'  socket: '%s'",
		  host ? host : "<null>",
		  unix_socket ? unix_socket : "<null>"));
      if (mysql->options.protocol == MYSQL_PROTOCOL_PIPE ||
	  (host && !strcmp(host,LOCAL_HOST_NAMEDPIPE)) ||
	  (unix_socket && !strcmp(unix_socket,MYSQL_NAMEDPIPE)))
	goto error;
      /* Try also with TCP/IP */
    }
    else
    {
      net->vio= vio_new_win32pipe(hPipe);
      my_snprintf(host_info=buff, sizeof(buff)-1,
                  ER(CR_NAMEDPIPE_CONNECTION), unix_socket);
    }
  }
#endif
  DBUG_PRINT("info", ("net->vio: %p  protocol: %d",
                      net->vio, mysql->options.protocol));
  if (!net->vio &&
      (!mysql->options.protocol ||
       mysql->options.protocol == MYSQL_PROTOCOL_TCP))
  {
    struct addrinfo *res_lst, hints, *t_res;
    int gai_errno;
    char port_buf[NI_MAXSERV];
    my_socket sock= INVALID_SOCKET;
    int saved_error= 0, status= -1;

    unix_socket=0;				/* This is not used */

    if (!port)
      port= mysql_port;

    if (!host)
      host= LOCAL_HOST;

    my_snprintf(host_info=buff, sizeof(buff)-1, ER(CR_TCP_CONNECTION), host);
    DBUG_PRINT("info",("Server name: '%s'.  TCP sock: %d", host, port));
    DBUG_PRINT("info",("IP '%s'", "client"));

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype= SOCK_STREAM;
    hints.ai_protocol= IPPROTO_TCP;
    hints.ai_family= AF_UNSPEC;

    DBUG_PRINT("info",("IPV6 getaddrinfo %s", host));
    my_snprintf(port_buf, NI_MAXSERV, "%d", port);
    gai_errno= getaddrinfo(host, port_buf, &hints, &res_lst);

    if (gai_errno != 0) 
    { 
      /* 
        For DBUG we are keeping the right message but for client we default to
        historical error message.
      */
      DBUG_PRINT("info",("IPV6 getaddrinfo error %d", gai_errno));
      set_mysql_extended_error(mysql, CR_UNKNOWN_HOST, unknown_sqlstate,
                               ER(CR_UNKNOWN_HOST), host, gai_errno);

      goto error;
    }

    /*
      A hostname might map to multiple IP addresses (IPv4/IPv6). Go over the
      list of IP addresses until a successful connection can be established.
    */
    DBUG_PRINT("info", ("Try connect on all addresses for host."));
    for (t_res= res_lst; t_res; t_res= t_res->ai_next)
    {
      DBUG_PRINT("info", ("Create socket, family: %d  type: %d  proto: %d",
                          t_res->ai_family, t_res->ai_socktype,
                          t_res->ai_protocol));
      sock= socket(t_res->ai_family, t_res->ai_socktype, t_res->ai_protocol);
      if (sock == INVALID_SOCKET)
      {
        saved_error= socket_errno;
        continue;
      }

      net->vio= vio_new(sock, VIO_TYPE_TCPIP, VIO_BUFFERED_READ);
      if (!net->vio)
      {
        set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
        closesocket(sock);
        freeaddrinfo(res_lst);
        goto error;
      }

      DBUG_PRINT("info", ("Connect socket"));
      status= connect_sync(mysql, net, sock,
                                    t_res->ai_addr, (uint)t_res->ai_addrlen);
      /*
        Here we rely on my_connect() to return success only if the
        connect attempt was really successful. Otherwise we would stop
        trying another address, believing we were successful.
      */
      if (!status)
        break;

      /*
        Save value as socket errno might be overwritten due to
        calling a socket function below.
      */
      saved_error= socket_errno;

      DBUG_PRINT("info", ("No success, close socket, try next address."));
      vio_delete(mysql->net.vio);
      mysql->net.vio= 0;
    }
    DBUG_PRINT("info",
               ("End of connect attempts, sock: %d  status: %d  error: %d",
                (int)sock, status, saved_error));

    freeaddrinfo(res_lst);

    if (sock == INVALID_SOCKET)
    {
      set_mysql_extended_error(mysql, CR_IPSOCK_ERROR, unknown_sqlstate,
                                ER(CR_IPSOCK_ERROR), saved_error);
      goto error;
    }

    if (status)
    {
      DBUG_PRINT("error",("Got error %d on connect to '%s'", saved_error, host));
      set_mysql_extended_error(mysql, CR_CONN_HOST_ERROR, unknown_sqlstate,
                                ER(CR_CONN_HOST_ERROR), host, saved_error);
      goto error;
    }
  }

  DBUG_PRINT("info", ("net->vio: %p", net->vio));
  if (!net->vio)
  {
    DBUG_PRINT("error",("Unknown protocol %d ",mysql->options.protocol));
    set_mysql_error(mysql, CR_CONN_UNKNOW_PROTOCOL, unknown_sqlstate);
    goto error;
  }

  if (my_net_init(net, net->vio, _current_thd(), MYF(0)))
  {
    vio_delete(net->vio);
    net->vio = 0;
    set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
    goto error;
  }
  vio_keepalive(net->vio,TRUE);

  /* If user set read_timeout, let it override the default */
  if (mysql->options.read_timeout)
    my_net_set_read_timeout(net, mysql->options.read_timeout);

  /* If user set write_timeout, let it override the default */
  if (mysql->options.write_timeout)
    my_net_set_write_timeout(net, mysql->options.write_timeout);

  if (mysql->options.max_allowed_packet)
    net->max_packet_size= mysql->options.max_allowed_packet;

  /* Get version info */
  mysql->protocol_version= PROTOCOL_VERSION;	/* Assume this */
  if (mysql->options.connect_timeout &&
      (vio_io_wait(net->vio, VIO_IO_EVENT_READ,
                   get_vio_connect_timeout(mysql)) < 1))
  {
    set_mysql_extended_error(mysql, CR_SERVER_LOST, unknown_sqlstate,
                             ER(CR_SERVER_LOST_EXTENDED),
                             "waiting for initial communication packet",
                             errno);
    goto error;
  }

  /*
    Part 1: Connection established, read and parse first packet
  */
  DBUG_PRINT("info", ("Read first packet."));

  if ((pkt_length=cli_safe_read(mysql)) == packet_error)
  {
    if (mysql->net.last_errno == CR_SERVER_LOST)
      set_mysql_extended_error(mysql, CR_SERVER_LOST, unknown_sqlstate,
                               ER(CR_SERVER_LOST_EXTENDED),
                               "reading initial communication packet",
                               errno);
    goto error;
  }
  pkt_end= (char*)net->read_pos + pkt_length;
  /* Check if version of protocol matches current one */
  mysql->protocol_version= net->read_pos[0];
  DBUG_DUMP("packet",(uchar*) net->read_pos,10);
  DBUG_PRINT("info",("mysql protocol version %d, server=%d",
		     PROTOCOL_VERSION, mysql->protocol_version));
  if (mysql->protocol_version != PROTOCOL_VERSION)
  {
    set_mysql_extended_error(mysql, CR_VERSION_ERROR, unknown_sqlstate,
                             ER(CR_VERSION_ERROR), mysql->protocol_version,
                             PROTOCOL_VERSION);
    goto error;
  }
  server_version_end= end= strend((char*) net->read_pos+1);
  mysql->thread_id=uint4korr(end+1);
  end+=5;
  /* 
    Scramble is split into two parts because old clients do not understand
    long scrambles; here goes the first part.
  */
  scramble_data= end;
  scramble_data_len= SCRAMBLE_LENGTH_323 + 1;
  scramble_plugin= old_password_plugin_name;
  end+= scramble_data_len;

  if (pkt_end >= end + 1)
    mysql->server_capabilities=uint2korr(end);
  if (pkt_end >= end + 18)
  {
    /* New protocol with 16 bytes to describe server characteristics */
    mysql->server_language=end[2];
    mysql->server_status=uint2korr(end+3);
    mysql->server_capabilities|= ((unsigned) uint2korr(end+5)) << 16;
    pkt_scramble_len= end[7];
    if (pkt_scramble_len < 0)
    {
      set_mysql_error(mysql, CR_MALFORMED_PACKET,
                      unknown_sqlstate);        /* purecov: inspected */
      goto error;
    }
  }
  end+= 18;

  if (mysql->options.secure_auth && passwd[0] &&
      !(mysql->server_capabilities & CLIENT_SECURE_CONNECTION))
  {
    set_mysql_error(mysql, CR_SECURE_AUTH, unknown_sqlstate);
    goto error;
  }

  if (mysql_init_character_set(mysql))
    goto error;

  /* Save connection information */
  if (!my_multi_malloc(key_memory_MYSQL, MYF(0),
		       &mysql->host_info, (uint) strlen(host_info)+1,
		       &mysql->host,      (uint) strlen(host)+1,
		       &mysql->unix_socket,unix_socket ?
		       (uint) strlen(unix_socket)+1 : (uint) 1,
		       &mysql->server_version,
		       (uint) (server_version_end - (char*) net->read_pos + 1),
		       NullS) ||
      !(mysql->user=my_strdup(key_memory_MYSQL, user,MYF(0))) ||
      !(mysql->passwd=my_strdup(key_memory_MYSQL, passwd,MYF(0))))
  {
    set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
    goto error;
  }
  strmov(mysql->host_info,host_info);
  strmov(mysql->host,host);
  if (unix_socket)
    strmov(mysql->unix_socket,unix_socket);
  else
    mysql->unix_socket=0;
  strmov(mysql->server_version,(char*) net->read_pos+1);
  mysql->port=port;

  /*
    remove the rpl hack from the version string, in case we're connecting
    to a pre-11.0 server
  */
#define RPL_VERSION_HACK "5.5.5-"
  if ((mysql->server_capabilities & CLIENT_PLUGIN_AUTH) &&
      strncmp(mysql->server_version, RPL_VERSION_HACK,
              sizeof(RPL_VERSION_HACK) - 1) == 0)
    mysql->server_version+= sizeof(RPL_VERSION_HACK) - 1;

  if (pkt_end >= end + SCRAMBLE_LENGTH - SCRAMBLE_LENGTH_323 + 1)
  {
    /*
     move the first scramble part - directly in the NET buffer -
     to get a full continuous scramble. We've read all the header,
     and can overwrite it now.
    */
    memmove(end - SCRAMBLE_LENGTH_323, scramble_data,
            SCRAMBLE_LENGTH_323);
    scramble_data= end - SCRAMBLE_LENGTH_323;
    if (mysql->server_capabilities & CLIENT_PLUGIN_AUTH)
    {
      scramble_data_len= pkt_scramble_len;
      scramble_plugin= scramble_data + scramble_data_len;
      if (scramble_data + scramble_data_len > pkt_end)
      {
        set_mysql_error(mysql, CR_MALFORMED_PACKET, unknown_sqlstate);
        goto error;
      }
    }
    else
    {
      scramble_data_len= (int)(pkt_end - scramble_data);
      scramble_plugin= native_password_plugin_name;
    }
  }
  else
    mysql->server_capabilities&= ~CLIENT_SECURE_CONNECTION;

  mysql->client_flag= client_flag;

  set_connect_attributes(mysql, buff, sizeof(buff));

  /*
    Part 2: invoke the plugin to send the authentication data to the server
  */

  if (run_plugin_auth(mysql, scramble_data, scramble_data_len,
                      scramble_plugin, db))
    goto error;

  /*
    Part 3: authenticated, finish the initialization of the connection
  */

  if (mysql->client_flag & CLIENT_COMPRESS)      /* We will use compression */
    net->compress=1;

  if (db && !mysql->db && mysql_select_db(mysql, db))
  {
    if (mysql->net.last_errno == CR_SERVER_LOST)
        set_mysql_extended_error(mysql, CR_SERVER_LOST, unknown_sqlstate,
                                 ER(CR_SERVER_LOST_EXTENDED),
                                 "Setting initial database",
                                 errno);
    goto error;
  }

  /*
     Using init_commands is not supported when connecting from within the
     server.
  */
#ifndef MYSQL_SERVER
  if (mysql->options.init_commands)
  {
    DYNAMIC_ARRAY *init_commands= mysql->options.init_commands;
    char **ptr= (char**)init_commands->buffer;
    char **end_command= ptr + init_commands->elements;

    my_bool reconnect=mysql->reconnect;
    mysql->reconnect=0;

    for (; ptr < end_command; ptr++)
    {
      int status;

      if (mysql_real_query(mysql,*ptr, (ulong) strlen(*ptr)))
	goto error;

      do {
        if (mysql->fields)
        {
          MYSQL_RES *res;
          if (!(res= cli_use_result(mysql)))
            goto error;
          mysql_free_result(res);
        }
        if ((status= mysql_next_result(mysql)) > 0)
          goto error;
      } while (status == 0);
    }
    mysql->reconnect=reconnect;
  }
#endif

  DBUG_PRINT("exit", ("Mysql handler: %p",mysql));
  DBUG_RETURN(mysql);

error:
  DBUG_PRINT("error",("message: %u/%s (%s)",
                      net->last_errno,
                      net->sqlstate,
                      net->last_error));
  {
    /* Free alloced memory */
    end_server(mysql);
    mysql_close_free(mysql);
    if (!(client_flag & CLIENT_REMEMBER_OPTIONS))
      mysql_close_free_options(mysql);
  }
  DBUG_RETURN(0);
}


my_bool mysql_reconnect(MYSQL *mysql)
{
  MYSQL tmp_mysql;
  DBUG_ENTER("mysql_reconnect");
  DBUG_ASSERT(mysql);
  DBUG_PRINT("enter", ("mysql->reconnect: %d", mysql->reconnect));

  if (!mysql->reconnect ||
      (mysql->server_status & SERVER_STATUS_IN_TRANS) || !mysql->host_info)
  {
    /* Allow reconnect next time */
    mysql->server_status&= ~SERVER_STATUS_IN_TRANS;
    set_mysql_error(mysql, CR_SERVER_GONE_ERROR, unknown_sqlstate);
    DBUG_RETURN(1);
  }
  mysql_init(&tmp_mysql);
  tmp_mysql.options= mysql->options;
  tmp_mysql.options.my_cnf_file= tmp_mysql.options.my_cnf_group= 0;


  if (!mysql_real_connect(&tmp_mysql,mysql->host,mysql->user,mysql->passwd,
			  mysql->db, mysql->port, mysql->unix_socket,
			  mysql->client_flag | CLIENT_REMEMBER_OPTIONS))
  {
    mysql->net.last_errno= tmp_mysql.net.last_errno;
    strmov(mysql->net.last_error, tmp_mysql.net.last_error);
    strmov(mysql->net.sqlstate, tmp_mysql.net.sqlstate);
    DBUG_RETURN(1);
  }
  if (mysql_set_character_set(&tmp_mysql, mysql->charset->cs_name.str))
  {
    DBUG_PRINT("error", ("mysql_set_character_set() failed"));
    bzero((char*) &tmp_mysql.options,sizeof(tmp_mysql.options));
    mysql_close(&tmp_mysql);
    mysql->net.last_errno= tmp_mysql.net.last_errno;
    strmov(mysql->net.last_error, tmp_mysql.net.last_error);
    strmov(mysql->net.sqlstate, tmp_mysql.net.sqlstate);
    DBUG_RETURN(1);
  }

  DBUG_PRINT("info", ("reconnect succeeded"));
  tmp_mysql.reconnect= 1;
  tmp_mysql.free_me= mysql->free_me;

  /* Move prepared statements (if any) over to the new mysql object */
  tmp_mysql.stmts= mysql->stmts;
  mysql->stmts= 0;

  /* Don't free options as these are now used in tmp_mysql */
  bzero((char*) &mysql->options,sizeof(mysql->options));
  mysql->free_me=0;
  mysql_close(mysql);
  *mysql=tmp_mysql;
  net_clear(&mysql->net, 1);
  mysql->affected_rows= ~(my_ulonglong) 0;
  DBUG_RETURN(0);
}


/**************************************************************************
  Set current database
**************************************************************************/

int STDCALL
mysql_select_db(MYSQL *mysql, const char *db)
{
  int error;
  DBUG_ENTER("mysql_select_db");
  DBUG_PRINT("enter",("db: '%s'",db));

  if ((error=simple_command(mysql,COM_INIT_DB, (const uchar*) db,
                            (ulong) strlen(db),0)))
    DBUG_RETURN(error);
  my_free(mysql->db);
  mysql->db=my_strdup(key_memory_MYSQL, db,MYF(MY_WME));
  DBUG_RETURN(0);
}


/*************************************************************************
  Send a QUIT to the server and close the connection
  If handle is alloced by mysql connect free it.
*************************************************************************/

static void mysql_close_free_options(MYSQL *mysql)
{
  DBUG_ENTER("mysql_close_free_options");

  my_free(mysql->options.user);
  my_free(mysql->options.host);
  my_free(mysql->options.password);
  my_free(mysql->options.unix_socket);
  my_free(mysql->options.db);
  my_free(mysql->options.my_cnf_file);
  my_free(mysql->options.my_cnf_group);
  my_free(mysql->options.charset_dir);
  my_free(mysql->options.charset_name);
  my_free(mysql->options.client_ip);
  if (mysql->options.init_commands)
  {
    DYNAMIC_ARRAY *init_commands= mysql->options.init_commands;
    char **ptr= (char**)init_commands->buffer;
    char **end= ptr + init_commands->elements;
    for (; ptr<end; ptr++)
      my_free(*ptr);
    delete_dynamic(init_commands);
    my_free(init_commands);
  }
#if defined(HAVE_OPENSSL) && !defined(EMBEDDED_LIBRARY)
  mysql_ssl_free(mysql);
#endif /* HAVE_OPENSSL && !EMBEDDED_LIBRARY */
  if (mysql->options.extension)
  {
    my_free(mysql->options.extension->plugin_dir);
    my_free(mysql->options.extension->default_auth);
    my_hash_free(&mysql->options.extension->connection_attributes);
    my_free(mysql->options.extension);
  }
  bzero((char*) &mysql->options,sizeof(mysql->options));
  DBUG_VOID_RETURN;
}


static void mysql_close_free(MYSQL *mysql)
{
  my_free(mysql->host_info);
  my_free(mysql->user);
  my_free(mysql->passwd);
  my_free(mysql->db);
  if (mysql->methods && mysql->methods->on_close_free)
    (*mysql->methods->on_close_free)(mysql);
  /* Clear pointers for better safety */
  mysql->host_info= mysql->user= mysql->passwd= mysql->db= 0;
}


/**
  For use when the connection to the server has been lost (in which case 
  the server has discarded all information about prepared statements
  associated with the connection).

  Mark all statements in mysql->stmts by setting stmt->mysql= 0 if the
  statement has transitioned beyond the MYSQL_STMT_INIT_DONE state, and
  unlink the statement from the mysql->stmts list.

  The remaining pruned list of statements (if any) is kept in mysql->stmts.

  @param mysql       pointer to the MYSQL object

  @return none
*/
static void mysql_prune_stmt_list(MYSQL *mysql)
{
  LIST *element= mysql->stmts;
  for (; element; element= element->next)
  {
    MYSQL_STMT *stmt= (MYSQL_STMT *) element->data;
    if (stmt->state != MYSQL_STMT_INIT_DONE)
    {
      stmt->mysql= 0;
      stmt->last_errno= CR_SERVER_LOST;
      strmov(stmt->last_error, ER(CR_SERVER_LOST));
      strmov(stmt->sqlstate, unknown_sqlstate);
      mysql->stmts= list_delete(mysql->stmts, element);
    }
  }
}


/*
  Clear connection pointer of every statement: this is necessary
  to give error on attempt to use a prepared statement of closed
  connection.

  SYNOPSYS
    mysql_detach_stmt_list()
      stmt_list  pointer to mysql->stmts
      func_name  name of calling function

  NOTE
    There is similar code in mysql_reconnect(), so changes here
    should also be reflected there.
*/

void mysql_detach_stmt_list(LIST **stmt_list __attribute__((unused)),
                            const char *func_name __attribute__((unused)))
{
#ifdef MYSQL_CLIENT
  /* Reset connection handle in all prepared statements. */
  LIST *element= *stmt_list;
  char buff[MYSQL_ERRMSG_SIZE];
  DBUG_ENTER("mysql_detach_stmt_list");

  my_snprintf(buff, sizeof(buff)-1, ER(CR_STMT_CLOSED), func_name);
  for (; element; element= element->next)
  {
    MYSQL_STMT *stmt= (MYSQL_STMT *) element->data;
    set_stmt_error(stmt, CR_STMT_CLOSED, unknown_sqlstate, buff);
    stmt->mysql= 0;
    /* No need to call list_delete for statement here */
  }
  *stmt_list= 0;
  DBUG_VOID_RETURN;
#endif /* MYSQL_CLIENT */
}


/*
  Close a MySQL connection and free all resources attached to it.

  This function is coded in such that it can be called multiple times
  (As some clients call this after mysql_real_connect() fails)
*/

/*
  mysql_close() can actually block, at least in theory, if the socket buffer
  is full when sending the COM_QUIT command.

  On the other hand, the latter part of mysql_close() needs to free the stack
  used for non-blocking operation of blocking stuff, so that later part can
  _not_ be done non-blocking.

  Therefore, mysql_close_slow_part() is used to run the parts of mysql_close()
  that may block. It can be called before mysql_close(), and in that case
  mysql_close() is guaranteed not to need to block.  */
void STDCALL mysql_close_slow_part(MYSQL *mysql)
{
  /* If connection is still up, send a QUIT message */
  if (mysql->net.vio != 0)
  {
    free_old_query(mysql);
    mysql->status=MYSQL_STATUS_READY; /* Force command */
    mysql->reconnect=0;
    simple_command(mysql,COM_QUIT,(uchar*) 0,0,1);
    end_server(mysql);			/* Sets mysql->net.vio= 0 */
  }
}

void STDCALL mysql_close(MYSQL *mysql)
{
  DBUG_ENTER("mysql_close");
  DBUG_PRINT("enter", ("mysql: %p",  mysql));

  if (mysql)					/* Some simple safety */
  {
    mysql_close_slow_part(mysql);
    mysql_close_free_options(mysql);
    mysql_close_free(mysql);
    mysql_detach_stmt_list(&mysql->stmts, "mysql_close");
    if (mysql->free_me)
      my_free(mysql);
  }
  DBUG_VOID_RETURN;
}


static my_bool parse_ok_packet(MYSQL *mysql, ulong length)
{
  uchar *pos= mysql->net.read_pos + 1;
  DBUG_ASSERT(pos[-1] == 0);

  mysql->affected_rows= net_field_length_ll(&pos);
  mysql->insert_id=	  net_field_length_ll(&pos);
  if (protocol_41(mysql))
  {
    mysql->server_status=uint2korr(pos); pos+=2;
    mysql->warning_count=uint2korr(pos); pos+=2;
  }
  else if (mysql->server_capabilities & CLIENT_TRANSACTIONS)
  {
    mysql->server_status=uint2korr(pos); pos+=2;
    mysql->warning_count= 0;
  }
  if (pos < mysql->net.read_pos + length && net_field_length(&pos))
    mysql->info=(char*) pos;
  return 0;
}


static my_bool cli_read_query_result(MYSQL *mysql)
{
  uchar *pos;
  ulong field_count;
  MYSQL_DATA *fields;
  ulong length;
#ifdef MYSQL_CLIENT
  my_bool can_local_infile= mysql->auto_local_infile != WAIT_FOR_QUERY;
#endif
  DBUG_ENTER("cli_read_query_result");

  if (mysql->auto_local_infile == ACCEPT_FILE_REQUEST)
    mysql->auto_local_infile= WAIT_FOR_QUERY;

  if ((length = cli_safe_read(mysql)) == packet_error)
    DBUG_RETURN(1);
  free_old_query(mysql);		/* Free old result */
#ifdef MYSQL_CLIENT			/* Avoid warn of unused labels*/
get_info:
#endif
  pos= mysql->net.read_pos;
  if ((field_count= net_field_length(&pos)) == 0)
    DBUG_RETURN(parse_ok_packet(mysql, length));

#ifdef MYSQL_CLIENT
  if (field_count == NULL_LENGTH)		/* LOAD DATA LOCAL INFILE */
  {
    int error;

    if (!(mysql->options.client_flag & CLIENT_LOCAL_FILES) ||
        !can_local_infile)
    {
      set_mysql_error(mysql, CR_MALFORMED_PACKET, unknown_sqlstate);
      DBUG_RETURN(1);
    }   

    error= handle_local_infile(mysql,(char*) pos);
    if ((length= cli_safe_read(mysql)) == packet_error || error)
      DBUG_RETURN(1);
    goto get_info;				/* Get info packet */
  }
#endif
  if (!(mysql->server_status & SERVER_STATUS_AUTOCOMMIT))
    mysql->server_status|= SERVER_STATUS_IN_TRANS;

  if (!(fields=cli_read_rows(mysql,(MYSQL_FIELD*)0, protocol_41(mysql) ? 7:5)))
    DBUG_RETURN(1);
  if (!(mysql->fields=unpack_fields(mysql, fields,&mysql->field_alloc,
				    (uint) field_count,0,
				    mysql->server_capabilities)))
    DBUG_RETURN(1);
  mysql->status= MYSQL_STATUS_GET_RESULT;
  mysql->field_count= (uint) field_count;
  DBUG_PRINT("exit",("ok"));
  DBUG_RETURN(0);
}


/*
  Send the query and return so we can do something else.
  Needs to be followed by mysql_read_query_result() when we want to
  finish processing it.
*/

int STDCALL
mysql_send_query(MYSQL* mysql, const char* query, ulong length)
{
  DBUG_ENTER("mysql_send_query");
  if (mysql->options.client_flag & CLIENT_LOCAL_FILES &&
      mysql->auto_local_infile == WAIT_FOR_QUERY &&
      (*query == 'l' || *query == 'L'))
  {
    if (strncasecmp(query, STRING_WITH_LEN("load")) == 0)
      mysql->auto_local_infile= ACCEPT_FILE_REQUEST;
  }
  DBUG_RETURN(simple_command(mysql, COM_QUERY, (uchar*) query, length, 1));
}

int STDCALL
mysql_real_query(MYSQL *mysql, const char *query, ulong length)
{
  DBUG_ENTER("mysql_real_query");
  DBUG_PRINT("enter",("handle: %p", mysql));
  DBUG_PRINT("query",("Query = '%-.4096s'",query));

  if (mysql_send_query(mysql,query,length))
    DBUG_RETURN(1);
  DBUG_RETURN((int) (*mysql->methods->read_query_result)(mysql));
}


/**************************************************************************
  Alloc result struct for buffered results. All rows are read to buffer.
  mysql_data_seek may be used.
**************************************************************************/

MYSQL_RES * STDCALL mysql_store_result(MYSQL *mysql)
{
  MYSQL_RES *result;
  DBUG_ENTER("mysql_store_result");

  if (!mysql->fields)
    DBUG_RETURN(0);
  if (mysql->status != MYSQL_STATUS_GET_RESULT)
  {
    set_mysql_error(mysql, CR_COMMANDS_OUT_OF_SYNC, unknown_sqlstate);
    DBUG_RETURN(0);
  }
  mysql->status=MYSQL_STATUS_READY;		/* server is ready */
  if (!(result=(MYSQL_RES*) my_malloc(key_memory_MYSQL_RES,
                                      (uint) (sizeof(MYSQL_RES)+
					      sizeof(ulong) *
					      mysql->field_count),
				      MYF(MY_WME | MY_ZEROFILL))))
  {
    set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
    DBUG_RETURN(0);
  }
  result->methods= mysql->methods;
  result->eof=1;				/* Marker for buffered */
  result->lengths=(ulong*) (result+1);
  if (!(result->data=
	(*mysql->methods->read_rows)(mysql,mysql->fields,mysql->field_count)))
  {
    my_free(result);
    DBUG_RETURN(0);
  }
  mysql->affected_rows= result->row_count= result->data->rows;
  result->data_cursor=	result->data->data;
  result->fields=	mysql->fields;
  result->field_alloc=	mysql->field_alloc;
  result->field_count=	mysql->field_count;
  /* The rest of result members is bzeroed in malloc */
  mysql->fields=0;				/* fields is now in result */
  clear_alloc_root(&mysql->field_alloc);
  /* just in case this was mistakenly called after mysql_stmt_execute() */
  mysql->unbuffered_fetch_owner= 0;
  DBUG_RETURN(result);				/* Data fetched */
}


/**************************************************************************
  Alloc struct for use with unbuffered reads. Data is fetched by domand
  when calling to mysql_fetch_row.
  mysql_data_seek is a noop.

  No other queries may be specified with the same MYSQL handle.
  There shouldn't be much processing per row because mysql server shouldn't
  have to wait for the client (and will not wait more than 30 sec/packet).
**************************************************************************/

static MYSQL_RES * cli_use_result(MYSQL *mysql)
{
  MYSQL_RES *result;
  DBUG_ENTER("cli_use_result");

  if (!mysql->fields)
    DBUG_RETURN(0);
  if (mysql->status != MYSQL_STATUS_GET_RESULT)
  {
    set_mysql_error(mysql, CR_COMMANDS_OUT_OF_SYNC, unknown_sqlstate);
    DBUG_RETURN(0);
  }
  if (!(result=(MYSQL_RES*) my_malloc(key_memory_MYSQL_RES,
                                      sizeof(*result)+
				      sizeof(ulong)*mysql->field_count,
				      MYF(MY_WME | MY_ZEROFILL))))
    DBUG_RETURN(0);
  result->lengths=(ulong*) (result+1);
  result->methods= mysql->methods;
  if (!(result->row=(MYSQL_ROW)
	my_malloc(key_memory_MYSQL_ROW,
                  sizeof(result->row[0])*(mysql->field_count+1), MYF(MY_WME))))
  {                                     /* Ptrs: to one row */
    my_free(result);
    DBUG_RETURN(0);
  }
  result->fields=	mysql->fields;
  result->field_alloc=	mysql->field_alloc;
  result->field_count=	mysql->field_count;
  result->current_field=0;
  result->handle=	mysql;
  result->current_row=	0;
  mysql->fields=0;			/* fields is now in result */
  clear_alloc_root(&mysql->field_alloc);
  mysql->status=MYSQL_STATUS_USE_RESULT;
  mysql->unbuffered_fetch_owner= &result->unbuffered_fetch_cancelled;
  DBUG_RETURN(result);			/* Data is read to be fetched */
}


my_ulonglong STDCALL mysql_affected_rows(MYSQL *mysql)
{
  return mysql->affected_rows;
}


/**************************************************************************
  Return next row of the query results
**************************************************************************/

MYSQL_ROW STDCALL
mysql_fetch_row(MYSQL_RES *res)
{
  DBUG_ENTER("mysql_fetch_row");
  if (!res->data)
  {						/* Unbufferred fetch */
    if (!res->eof)
    {
      MYSQL *mysql= res->handle;
      if (mysql->status != MYSQL_STATUS_USE_RESULT)
      {
        set_mysql_error(mysql,
                        res->unbuffered_fetch_cancelled ? 
                        CR_FETCH_CANCELED : CR_COMMANDS_OUT_OF_SYNC,
                        unknown_sqlstate);
      }
      else if (!(read_one_row(mysql, res->field_count, res->row, res->lengths)))
      {
	res->row_count++;
	DBUG_RETURN(res->current_row=res->row);
      }
      DBUG_PRINT("info",("end of data"));
      res->eof=1;
      mysql->status=MYSQL_STATUS_READY;
      /*
        Reset only if owner points to us: there is a chance that somebody
        started new query after mysql_stmt_close():
      */
      if (mysql->unbuffered_fetch_owner == &res->unbuffered_fetch_cancelled)
        mysql->unbuffered_fetch_owner= 0;
      /* Don't clear handle in mysql_free_result */
      res->handle=0;
    }
    DBUG_RETURN((MYSQL_ROW) NULL);
  }
  {
    MYSQL_ROW tmp;
    if (!res->data_cursor)
    {
      DBUG_PRINT("info",("end of data"));
      DBUG_RETURN(res->current_row=(MYSQL_ROW) NULL);
    }
    tmp = res->data_cursor->data;
    res->data_cursor = res->data_cursor->next;
    DBUG_RETURN(res->current_row=tmp);
  }
}


/**************************************************************************
  Get column lengths of the current row
  If one uses mysql_use_result, res->lengths contains the length information,
  else the lengths are calculated from the offset between pointers.
**************************************************************************/

ulong * STDCALL
mysql_fetch_lengths(MYSQL_RES *res)
{
  MYSQL_ROW column;

  if (!(column=res->current_row))
    return 0;					/* Something is wrong */
  if (res->data)
    (*res->methods->fetch_lengths)(res->lengths, column, res->field_count);
  return res->lengths;
}

int STDCALL
mysql_options(MYSQL *mysql,enum mysql_option option, const void *arg)
{
  DBUG_ENTER("mysql_options");
  DBUG_PRINT("enter",("option: %d",(int) option));
  switch (option) {
  case MYSQL_OPT_CONNECT_TIMEOUT:
    mysql->options.connect_timeout= *(uint*) arg;
    break;
  case MYSQL_OPT_READ_TIMEOUT:
    mysql->options.read_timeout= *(uint*) arg;
    break;
  case MYSQL_OPT_WRITE_TIMEOUT:
    mysql->options.write_timeout= *(uint*) arg;
    break;
  case MYSQL_OPT_COMPRESS:
    mysql->options.compress= 1;			/* Remember for connect */
    mysql->options.client_flag|= CLIENT_COMPRESS;
    break;
  case MYSQL_OPT_NAMED_PIPE:			/* This option is deprecated */
    mysql->options.protocol=MYSQL_PROTOCOL_PIPE; /* Force named pipe */
    break;
  case MYSQL_OPT_LOCAL_INFILE:			/* Allow LOAD DATA LOCAL ?*/
    if (!arg || *(uint*) arg)
      mysql->options.client_flag|= CLIENT_LOCAL_FILES;
    else
      mysql->options.client_flag&= ~CLIENT_LOCAL_FILES;
    mysql->auto_local_infile= arg && *(uint*)arg == LOCAL_INFILE_MODE_AUTO
                              ? WAIT_FOR_QUERY : ALWAYS_ACCEPT;
    break;
  case MYSQL_INIT_COMMAND:
    add_init_command(&mysql->options,arg);
    break;
  case MYSQL_READ_DEFAULT_FILE:
    my_free(mysql->options.my_cnf_file);
    mysql->options.my_cnf_file= opt_strdup(arg,MYF(MY_WME));
    break;
  case MYSQL_READ_DEFAULT_GROUP:
    my_free(mysql->options.my_cnf_group);
    mysql->options.my_cnf_group= opt_strdup(arg,MYF(MY_WME));
    break;
  case MYSQL_SET_CHARSET_DIR:
    my_free(mysql->options.charset_dir);
    mysql->options.charset_dir= opt_strdup(arg,MYF(MY_WME));
    break;
  case MYSQL_SET_CHARSET_NAME:
    my_free(mysql->options.charset_name);
    mysql->options.charset_name= opt_strdup(arg,MYF(MY_WME));
    break;
  case MYSQL_OPT_PROTOCOL:
    mysql->options.protocol= *(uint*) arg;
    break;
  case MYSQL_OPT_USE_REMOTE_CONNECTION:
  case MYSQL_OPT_USE_EMBEDDED_CONNECTION:
  case MYSQL_OPT_GUESS_CONNECTION:
    mysql->options.methods_to_use= option;
    break;
  case MYSQL_SET_CLIENT_IP:
    my_free(mysql->options.client_ip);
    mysql->options.client_ip= opt_strdup(arg, MYF(MY_WME));
    break;
  case MYSQL_SECURE_AUTH:
    mysql->options.secure_auth= *(my_bool *) arg;
    break;
  case MYSQL_REPORT_DATA_TRUNCATION:
    mysql->options.report_data_truncation= MY_TEST(*(my_bool*) arg);
    break;
  case MYSQL_OPT_RECONNECT:
    mysql->reconnect= *(my_bool *) arg;
    break;
  case MYSQL_OPT_USE_THREAD_SPECIFIC_MEMORY:
    mysql->options.use_thread_specific_memory= *(my_bool *) arg;
    break;
  case MYSQL_OPT_SSL_VERIFY_SERVER_CERT:
    if (!mysql->options.extension)
      mysql->options.extension= (struct st_mysql_options_extention *)
        my_malloc(PSI_INSTRUMENT_ME,
                  sizeof(struct st_mysql_options_extention),
                  MYF(MY_WME | MY_ZEROFILL));
    if (mysql->options.extension)
      mysql->options.extension->tls_allow_invalid_server_cert= !*(my_bool*) arg;
    break;
  case MYSQL_PLUGIN_DIR:
    EXTENSION_SET_STRING(&mysql->options, plugin_dir, arg);
    break;
  case MYSQL_DEFAULT_AUTH:
    EXTENSION_SET_STRING(&mysql->options, default_auth, arg);
    break;
  case MYSQL_ENABLE_CLEARTEXT_PLUGIN:
    break;
  case MYSQL_PROGRESS_CALLBACK:
    if (!mysql->options.extension)
      mysql->options.extension= (struct st_mysql_options_extention *)
        my_malloc(key_memory_mysql_options,
                  sizeof(struct st_mysql_options_extention),
                  MYF(MY_WME | MY_ZEROFILL));
    if (mysql->options.extension)
      mysql->options.extension->report_progress= 
        (void (*)(const MYSQL *, uint, uint, double, const char *, uint)) arg;
    break;
  case MYSQL_OPT_NONBLOCK:
    break;
  case MYSQL_OPT_SSL_KEY:
    SET_SSL_PATH_OPTION(&mysql->options,ssl_key, arg);
    break;
  case MYSQL_OPT_SSL_CERT:
    SET_SSL_PATH_OPTION(&mysql->options, ssl_cert, arg);
    break;
  case MYSQL_OPT_SSL_CA:
    SET_SSL_PATH_OPTION(&mysql->options,ssl_ca, arg);
    break;
  case MYSQL_OPT_SSL_CAPATH:
    SET_SSL_PATH_OPTION(&mysql->options,ssl_capath, arg);
    break;
  case MYSQL_OPT_SSL_CIPHER:
    SET_SSL_OPTION(&mysql->options,ssl_cipher, arg);
    break;
  case MYSQL_OPT_SSL_CRL:
    EXTENSION_SET_SSL_PATH_STRING(&mysql->options, ssl_crl, arg);
    break;
  case MYSQL_OPT_SSL_CRLPATH:
    EXTENSION_SET_SSL_PATH_STRING(&mysql->options, ssl_crlpath, arg);
    break;
  case MYSQL_OPT_CONNECT_ATTR_RESET:
    ENSURE_EXTENSIONS_PRESENT(&mysql->options);
    if (my_hash_inited(&mysql->options.extension->connection_attributes))
    {
      my_hash_free(&mysql->options.extension->connection_attributes);
      mysql->options.extension->connection_attributes_length= 0;
    }
    break;
  case MYSQL_OPT_CONNECT_ATTR_DELETE:
    ENSURE_EXTENSIONS_PRESENT(&mysql->options);
    if (my_hash_inited(&mysql->options.extension->connection_attributes))
    {
      size_t len;
      uchar *elt;

      len= arg ? strlen(arg) : 0;

      if (len)
      {
        elt= my_hash_search(&mysql->options.extension->connection_attributes,
                            arg, len);
        if (elt)
        {
          LEX_STRING *attr= (LEX_STRING *) elt;
          LEX_STRING *key= attr, *value= attr + 1;

          mysql->options.extension->connection_attributes_length-=
            get_length_store_length(key->length) + key->length +
            get_length_store_length(value->length) + value->length;

          my_hash_delete(&mysql->options.extension->connection_attributes,
                         elt);

        }
      }
    }
    break;
  case MYSQL_SHARED_MEMORY_BASE_NAME:
  default:
    DBUG_RETURN(1);
  }
  DBUG_RETURN(0);
}
/**
  A function to return the key from a connection attribute
*/
const uchar *
get_attr_key(const void *part_, size_t *length,
             my_bool not_used __attribute__((unused)))
{
  const LEX_STRING *part= part_;
  *length= part[0].length;
  return (const uchar *) part[0].str;
}

int STDCALL
mysql_options4(MYSQL *mysql,enum mysql_option option,
               const void *arg1, const void *arg2)
{
  DBUG_ENTER("mysql_option");
  DBUG_PRINT("enter",("option: %d",(int) option));

  switch (option)
  {
  case MYSQL_OPT_CONNECT_ATTR_ADD:
    {
      LEX_STRING *elt;
      char *key, *value;
      size_t key_len= arg1 ? strlen(arg1) : 0,
             value_len= arg2 ? strlen(arg2) : 0;
      size_t attr_storage_length= key_len + value_len;

      /* we can't have a zero length key */
      if (!key_len)
      {
        set_mysql_error(mysql, CR_INVALID_PARAMETER_NO, unknown_sqlstate);
        DBUG_RETURN(1);
      }

      /* calculate the total storage length of the attribute */
      attr_storage_length+= get_length_store_length(key_len);
      attr_storage_length+= get_length_store_length(value_len);

      ENSURE_EXTENSIONS_PRESENT(&mysql->options);

      /*
        Throw and error if the maximum combined length of the attribute value
        will be greater than the maximum that we can safely transmit.
      */
      if (attr_storage_length +
          mysql->options.extension->connection_attributes_length >
          MAX_CONNECTION_ATTR_STORAGE_LENGTH)
      {
        set_mysql_error(mysql, CR_INVALID_PARAMETER_NO, unknown_sqlstate);
        DBUG_RETURN(1);
      }

      if (!my_hash_inited(&mysql->options.extension->connection_attributes))
      {
        if (my_hash_init(key_memory_mysql_options,
                         &mysql->options.extension->connection_attributes,
                         &my_charset_bin, 0, 0, 0,
                         get_attr_key, my_free, HASH_UNIQUE))
        {
          set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
          DBUG_RETURN(1);
        }
      }
      if (!my_multi_malloc(key_memory_mysql_options, MYF(MY_WME),
                           &elt, 2 * sizeof(LEX_STRING),
                           &key, key_len + 1,
                           &value, value_len + 1,
                           NULL))
      {
        set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
        DBUG_RETURN(1);
      }
      elt[0].str= key; elt[0].length= key_len;
      elt[1].str= value; elt[1].length= value_len;
      if (key_len)
        memcpy(key, arg1, key_len);
      key[key_len]= 0;
      if (value_len)
        memcpy(value, arg2, value_len);
      value[value_len]= 0;
      if (my_hash_insert(&mysql->options.extension->connection_attributes,
                     (uchar *) elt))
      {
        /* can't insert the value */
        my_free(elt);
        set_mysql_error(mysql, CR_DUPLICATE_CONNECTION_ATTR,
                        unknown_sqlstate);
        DBUG_RETURN(1);
      }

      mysql->options.extension->connection_attributes_length+=
        attr_storage_length;

      break;
    }

  default:
    DBUG_RETURN(1);
  }
  DBUG_RETURN(0);
}


/****************************************************************************
  Functions to get information from the MySQL structure
  These are functions to make shared libraries more usable.
****************************************************************************/

/* MYSQL_RES */
my_ulonglong STDCALL mysql_num_rows(MYSQL_RES *res)
{
  return res->row_count;
}

unsigned int STDCALL mysql_num_fields(MYSQL_RES *res)
{
  return res->field_count;
}

uint STDCALL mysql_errno(MYSQL *mysql)
{
  return mysql ? mysql->net.last_errno : mysql_server_last_errno;
}


const char * STDCALL mysql_error(MYSQL *mysql)
{
  return mysql ? mysql->net.last_error : mysql_server_last_error;
}


/*
  Get version number for server in a form easy to test on

  SYNOPSIS
    mysql_get_server_version()
    mysql		Connection

  EXAMPLE
    MariaDB-4.1.0-alfa ->  40100
  
  NOTES
    We will ensure that a newer server always has a bigger number.

  RETURN
   Signed number > 323000
   Zero if there is no connection
*/

ulong STDCALL
mysql_get_server_version(MYSQL *mysql)
{
  ulong major= 0, minor= 0, version= 0;

  if (mysql->server_version)
  {
    const char *pos= mysql->server_version;
    char *end_pos;
    /* Skip possible prefix */
    while (*pos && !my_isdigit(&my_charset_latin1, *pos))
      pos++;
    major=   strtoul(pos, &end_pos, 10);	pos=end_pos+1;
    minor=   strtoul(pos, &end_pos, 10);	pos=end_pos+1;
    version= strtoul(pos, &end_pos, 10);
  }
  else
  {
    set_mysql_error(mysql, CR_COMMANDS_OUT_OF_SYNC, unknown_sqlstate);
  }

  return major*10000 + minor*100 + version;
}


/* 
   mysql_set_character_set function sends SET NAMES cs_name to
   the server (which changes character_set_client, character_set_result
   and character_set_connection) and updates mysql->charset so other
   functions like mysql_real_escape will work correctly.
*/
int STDCALL mysql_set_character_set(MYSQL *mysql, const char *cs_name)
{
  CHARSET_INFO *cs;
  const char *save_csdir= charsets_dir;

  if (mysql->options.charset_dir)
    charsets_dir= mysql->options.charset_dir;

  if (strlen(cs_name) < MY_CS_CHARACTER_SET_NAME_SIZE &&
     (cs= get_charset_by_csname(cs_name, MY_CS_PRIMARY, MYF(MY_UTF8_IS_UTF8MB3))))
  {
    char buff[MY_CS_CHARACTER_SET_NAME_SIZE + 10];
    charsets_dir= save_csdir;
    /* Skip execution of "SET NAMES" for pre-4.1 servers */
    if (mysql_get_server_version(mysql) < 40100)
      return 0;
    snprintf(buff, sizeof(buff),  "SET NAMES %s", cs_name);
    if (!mysql_real_query(mysql, buff, (uint) strlen(buff)))
    {
      mysql->charset= cs;
    }
  }
  else
  {
    char cs_dir_name[FN_REFLEN];
    get_charsets_dir(cs_dir_name);
    set_mysql_extended_error(mysql, CR_CANT_READ_CHARSET, unknown_sqlstate,
                             ER(CR_CANT_READ_CHARSET), cs_name, cs_dir_name);
  }
  charsets_dir= save_csdir;
  return mysql->net.last_errno;
}

/**
  client authentication plugin that does native MySQL authentication
  using a 20-byte (4.1+) scramble
*/
static int native_password_auth_client(MYSQL_PLUGIN_VIO *vio, MYSQL *mysql)
{
  int pkt_len;
  uchar *pkt;

  DBUG_ENTER("native_password_auth_client");


  if (((MCPVIO_EXT *)vio)->mysql_change_user)
  {
    /*
      in mysql_change_user() the client sends the first packet.
      we use the old scramble.
    */
    pkt= (uchar*)mysql->scramble;
    pkt_len= SCRAMBLE_LENGTH + 1;
  }
  else
  {
    /* read the scramble */
    if ((pkt_len= vio->read_packet(vio, &pkt)) < 0)
      DBUG_RETURN(CR_ERROR);

    if (pkt_len != SCRAMBLE_LENGTH + 1)
      DBUG_RETURN(CR_SERVER_HANDSHAKE_ERR);

    /* save it in MYSQL */
    memcpy(mysql->scramble, pkt, SCRAMBLE_LENGTH);
    mysql->scramble[SCRAMBLE_LENGTH] = 0;
  }

  if (mysql->passwd[0])
  {
    char scrambled[SCRAMBLE_LENGTH + 1];
    DBUG_PRINT("info", ("sending scramble"));
    scramble(scrambled, (char*)pkt, mysql->passwd);
    if (vio->write_packet(vio, (uchar*)scrambled, SCRAMBLE_LENGTH))
      DBUG_RETURN(CR_ERROR);
  }
  else
  {
    DBUG_PRINT("info", ("no password"));
    if (vio->write_packet(vio, 0, 0)) /* no password */
      DBUG_RETURN(CR_ERROR);
  }

  DBUG_RETURN(CR_OK);
}


static int native_password_auth_hash(MYSQL *mysql, uchar *out, size_t *out_length)
{
  uchar hash_stage1[MY_SHA1_HASH_SIZE];

  if (*out_length < MY_SHA1_HASH_SIZE)
    return 1;
  *out_length= MY_SHA1_HASH_SIZE;

  my_sha1(hash_stage1, mysql->passwd, strlen(mysql->passwd));
  my_sha1(out, (char*)hash_stage1, MY_SHA1_HASH_SIZE);

  return 0;
}


/**
  client authentication plugin that does old MySQL authentication
  using an 8-byte (4.0-) scramble
*/
static int old_password_auth_client(MYSQL_PLUGIN_VIO *vio, MYSQL *mysql)
{
  uchar *pkt;
  int pkt_len;

  DBUG_ENTER("old_password_auth_client");

  if (((MCPVIO_EXT *)vio)->mysql_change_user)
  {
    /*
      in mysql_change_user() the client sends the first packet.
      we use the old scramble.
    */
    pkt= (uchar*)mysql->scramble;
    pkt_len= SCRAMBLE_LENGTH_323 + 1;
  }
  else
  {
    /* read the scramble */
    if ((pkt_len= vio->read_packet(vio, &pkt)) < 0)
      DBUG_RETURN(CR_ERROR);

    if (pkt_len != SCRAMBLE_LENGTH_323 + 1 &&
        pkt_len != SCRAMBLE_LENGTH + 1)
        DBUG_RETURN(CR_SERVER_HANDSHAKE_ERR);

    /* save it in MYSQL */
    memmove(mysql->scramble, pkt, pkt_len - 1);
    mysql->scramble[pkt_len - 1] = 0;
  }

  if (mysql->passwd[0])
  {
    char scrambled[SCRAMBLE_LENGTH_323 + 1];
    scramble_323(scrambled, (char*)pkt, mysql->passwd);
    if (vio->write_packet(vio, (uchar*)scrambled, SCRAMBLE_LENGTH_323 + 1))
      DBUG_RETURN(CR_ERROR);
  }
  else
    if (vio->write_packet(vio, 0, 0)) /* no password */
      DBUG_RETURN(CR_ERROR);

  DBUG_RETURN(CR_OK);
}


my_socket STDCALL
mysql_get_socket(const MYSQL *mysql)
{
  if (mysql->net.vio)
    return vio_fd(mysql->net.vio);
  return INVALID_SOCKET;
}


int STDCALL mysql_cancel(MYSQL *mysql)
{
  if (mysql->net.vio)
	return vio_shutdown(mysql->net.vio, SHUT_RDWR);
  return -1;
}


MYSQL_RES *STDCALL mysql_use_result(MYSQL *mysql)
{
  return (*mysql->methods->use_result)(mysql);
}


MYSQL_FIELD *STDCALL mysql_fetch_fields(MYSQL_RES *res)
{
  return (res)->fields;
}


ulong STDCALL
mysql_real_escape_string(MYSQL *mysql, char *to,const char *from,
			 ulong length)
{
  my_bool overflow;
  if (mysql->server_status & SERVER_STATUS_NO_BACKSLASH_ESCAPES)
    return (ulong) escape_quotes_for_mysql(mysql->charset, to, 0, from, length,
                                           &overflow);
  return (ulong) escape_string_for_mysql(mysql->charset, to, 0, from, length,
                                         &overflow);
}
