/* Copyright (c) 2000, 2012, Oracle and/or its affiliates.
   Copyright (c) 2008, 2020, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

/**
  @file

  Low level functions for storing data to be send to the MySQL client.
  The actual communction is handled by the net_xxx functions in net_serv.cc
*/

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation				// gcc: Class implementation
#endif

#include "mariadb.h"
#include "sql_priv.h"
#include "protocol.h"
#include "sql_class.h"                          // THD
#include <stdarg.h>

static const unsigned int PACKET_BUFFER_EXTRA_ALLOC= 1024;
#ifndef EMBEDDED_LIBRARY
static bool write_eof_packet(THD *, NET *, uint, uint);
#endif

CHARSET_INFO *Protocol::character_set_results() const
{
  return thd->variables.character_set_results;
}


#ifndef EMBEDDED_LIBRARY
bool Protocol::net_store_data(const uchar *from, size_t length)
#else
bool Protocol_binary::net_store_data(const uchar *from, size_t length)
#endif
{
  ulong packet_length=packet->length();
  /* 
     The +9 comes from that strings of length longer than 16M require
     9 bytes to be stored (see net_store_length).
  */
  if (packet_length+9+length > packet->alloced_length() &&
      packet->realloc(packet_length+9+length))
    return 1;
  uchar *to= net_store_length((uchar*) packet->ptr()+packet_length, length);
  if (length)
    memcpy(to,from,length);
  packet->length((uint) (to+length-(uchar*) packet->ptr()));
  return 0;
}


/*
  net_store_data_cs() - extended version with character set conversion.
  
  It is optimized for short strings whose length after
  conversion is garanteed to be less than 251, which accupies
  exactly one byte to store length. It allows not to use
  the "convert" member as a temporary buffer, conversion
  is done directly to the "packet" member.
  The limit 251 is good enough to optimize send_result_set_metadata()
  because column, table, database names fit into this limit.
*/

#ifndef EMBEDDED_LIBRARY
bool Protocol::net_store_data_cs(const uchar *from, size_t length,
                              CHARSET_INFO *from_cs, CHARSET_INFO *to_cs)
#else
bool Protocol_binary::net_store_data_cs(const uchar *from, size_t length,
                              CHARSET_INFO *from_cs, CHARSET_INFO *to_cs)
#endif
{
  uint dummy_errors;
  /* Calculate maxumum possible result length */
  size_t conv_length= to_cs->mbmaxlen * length / from_cs->mbminlen;

  if (conv_length > 250)
  {
    /*
      For strings with conv_length greater than 250 bytes
      we don't know how many bytes we will need to store length: one or two,
      because we don't know result length until conversion is done.
      For example, when converting from utf8 (mbmaxlen=3) to latin1,
      conv_length=300 means that the result length can vary between 100 to 300.
      length=100 needs one byte, length=300 needs to bytes.
      
      Thus conversion directly to "packet" is not worthy.
      Let's use "convert" as a temporary buffer.
    */
    return (convert->copy((const char*) from, length, from_cs,
                          to_cs, &dummy_errors) ||
            net_store_data((const uchar*) convert->ptr(), convert->length()));
  }

  size_t packet_length= packet->length();
  size_t new_length= packet_length + conv_length + 1;

  if (new_length > packet->alloced_length() && packet->realloc(new_length))
    return 1;

  char *length_pos= (char*) packet->ptr() + packet_length;
  char *to= length_pos + 1;

  to+= copy_and_convert(to, conv_length, to_cs,
                        (const char*) from, length, from_cs, &dummy_errors);

  net_store_length((uchar*) length_pos, to - length_pos - 1);
  packet->length((uint) (to - packet->ptr()));
  return 0;
}


/**
  Send a error string to client.

  Design note:

  net_printf_error and net_send_error are low-level functions
  that shall be used only when a new connection is being
  established or at server startup.

  For SIGNAL/RESIGNAL and GET DIAGNOSTICS functionality it's
  critical that every error that can be intercepted is issued in one
  place only, my_message_sql.

  @param thd Thread handler
  @param sql_errno The error code to send
  @param err A pointer to the error message

  @return
    @retval FALSE The message was sent to the client
    @retval TRUE An error occurred and the message wasn't sent properly
*/

bool Protocol::net_send_error(THD *thd, uint sql_errno, const char *err,
                              const char* sqlstate)
{
  bool error;
  DBUG_ENTER("Protocol::net_send_error");

  DBUG_ASSERT(!thd->spcont);
  DBUG_ASSERT(sql_errno);
  DBUG_ASSERT(err);

  DBUG_PRINT("enter",("sql_errno: %d  err: %s", sql_errno, err));

  if (sqlstate == NULL)
    sqlstate= mysql_errno_to_sqlstate(sql_errno);

  /*
    It's one case when we can push an error even though there
    is an OK or EOF already.
  */
  thd->get_stmt_da()->set_overwrite_status(true);

  /* Abort multi-result sets */
  thd->server_status&= ~SERVER_MORE_RESULTS_EXISTS;

  error= net_send_error_packet(thd, sql_errno, err, sqlstate);

  thd->get_stmt_da()->set_overwrite_status(false);

  DBUG_RETURN(error);
}

/**
  Return ok to the client.

  The ok packet has the following structure:

  - 0               : Marker (1 byte)
  - affected_rows	: Stored in 1-9 bytes
  - id		: Stored in 1-9 bytes
  - server_status	: Copy of thd->server_status;  Can be used by client
  to check if we are inside an transaction.
  New in 4.0 protocol
  - warning_count	: Stored in 2 bytes; New in 4.1 protocol
  - message		: Stored as packed length (1-9 bytes) + message.
  Is not stored if no message.

  @param thd		   Thread handler
  @param server_status     The server status
  @param statement_warn_count  Total number of warnings
  @param affected_rows	   Number of rows changed by statement
  @param id		   Auto_increment id for first row (if used)
  @param message	   Message to send to the client (Used by mysql_status)
  @param is_eof            this called instead of old EOF packet

  @return
    @retval FALSE The message was successfully sent
    @retval TRUE An error occurred and the messages wasn't sent properly

*/

#ifndef EMBEDDED_LIBRARY
bool
Protocol::net_send_ok(THD *thd,
                      uint server_status, uint statement_warn_count,
                      ulonglong affected_rows, ulonglong id,
                      const char *message, bool is_eof)
{
  NET *net= &thd->net;
  StringBuffer<MYSQL_ERRMSG_SIZE + 10> store;

  bool error= FALSE;
  DBUG_ENTER("Protocol::net_send_ok");

  if (! net->vio)	// hack for re-parsing queries
  {
    DBUG_PRINT("info", ("vio present: NO"));
    DBUG_RETURN(FALSE);
  }

  /*
    OK send instead of EOF still require 0xFE header, but OK packet content.
  */
  if (is_eof)
  {
    DBUG_ASSERT(thd->client_capabilities & CLIENT_DEPRECATE_EOF);
    store.q_append((char)254);
  }
  else
    store.q_append('\0');

  /* affected rows */
  store.q_net_store_length(affected_rows);

  /* last insert id */
  store.q_net_store_length(id);

  /* if client has not session tracking capability, don't send state change flag*/
  if (!(thd->client_capabilities & CLIENT_SESSION_TRACK)) {
    server_status &= ~SERVER_SESSION_STATE_CHANGED;
  }

  if (thd->client_capabilities & CLIENT_PROTOCOL_41)
  {
    DBUG_PRINT("info",
	       ("affected_rows: %lu  id: %lu  status: %u  warning_count: %u",
		(ulong) affected_rows,
		(ulong) id,
		(uint) (server_status & 0xffff),
		(uint) statement_warn_count));
    store.q_append2b(server_status);

    /* We can only return up to 65535 warnings in two bytes */
    uint tmp= MY_MIN(statement_warn_count, 65535);
    store.q_append2b(tmp);
  }
  else if (net->return_status)			// For 4.0 protocol
  {
    store.q_append2b(server_status);
  }
  thd->get_stmt_da()->set_overwrite_status(true);

  if ((server_status & SERVER_SESSION_STATE_CHANGED) || (message && message[0]))
  {
    DBUG_ASSERT(safe_strlen(message) <= MYSQL_ERRMSG_SIZE);
    store.q_net_store_data((uchar*) safe_str(message), safe_strlen(message));
  }

  if (unlikely(server_status & SERVER_SESSION_STATE_CHANGED))
  {
    store.set_charset(thd->variables.collation_database);
    thd->session_tracker.store(thd, &store);
    thd->server_status&= ~SERVER_SESSION_STATE_CHANGED;
  }

  DBUG_ASSERT(store.length() <= MAX_PACKET_LENGTH);

  error= my_net_write(net, (const unsigned char*)store.ptr(), store.length());
  if (likely(!error))
    error= net_flush(net);

  thd->get_stmt_da()->set_overwrite_status(false);
  DBUG_PRINT("info", ("OK sent, so no more error sending allowed"));

  DBUG_RETURN(error);
}


static uchar eof_buff[1]= { (uchar) 254 };      /* Marker for end of fields */

/**
  Send eof (= end of result set) to the client.

  The eof packet has the following structure:

  - 254		: Marker (1 byte)
  - warning_count	: Stored in 2 bytes; New in 4.1 protocol
  - status_flag	: Stored in 2 bytes;
  For flags like SERVER_MORE_RESULTS_EXISTS.

  Note that the warning count will not be sent if 'no_flush' is set as
  we don't want to report the warning count until all data is sent to the
  client.

  @param thd		Thread handler
  @param server_status The server status
  @param statement_warn_count Total number of warnings

  @return
    @retval FALSE The message was successfully sent
    @retval TRUE An error occurred and the message wasn't sent properly
*/    

bool
Protocol::net_send_eof(THD *thd, uint server_status, uint statement_warn_count)
{
  NET *net= &thd->net;
  bool error= FALSE;
  DBUG_ENTER("Protocol::net_send_eof");

  /*
    Check if client understand new format packets (OK instead of EOF)

    Normally end of statement reply is signaled by OK packet, but in case
    of binlog dump request an EOF packet is sent instead. Also, old clients
    expect EOF packet instead of OK
  */
  if ((thd->client_capabilities & CLIENT_DEPRECATE_EOF) &&
      (thd->get_command() != COM_BINLOG_DUMP ))
  {
    error= net_send_ok(thd, server_status, statement_warn_count, 0, 0, NULL,
                       true);
    DBUG_RETURN(error);
  }

  /* Set to TRUE if no active vio, to work well in case of --init-file */
  if (net->vio != 0)
  {
    thd->get_stmt_da()->set_overwrite_status(true);
    error= write_eof_packet(thd, net, server_status, statement_warn_count);
    if (likely(!error))
      error= net_flush(net);
    thd->get_stmt_da()->set_overwrite_status(false);
    DBUG_PRINT("info", ("EOF sent, so no more error sending allowed"));
  }
  DBUG_RETURN(error);
}


/**
  Format EOF packet according to the current protocol and
  write it to the network output buffer.

  @param thd The thread handler
  @param net The network handler
  @param server_status The server status
  @param statement_warn_count The number of warnings


  @return
    @retval FALSE The message was sent successfully
    @retval TRUE An error occurred and the messages wasn't sent properly
*/

static bool write_eof_packet(THD *thd, NET *net,
                             uint server_status,
                             uint statement_warn_count)
{
  bool error;
  if (thd->client_capabilities & CLIENT_PROTOCOL_41)
  {
    uchar buff[5];
    /*
      Don't send warn count during SP execution, as the warn_list
      is cleared between substatements, and mysqltest gets confused
    */
    uint tmp= MY_MIN(statement_warn_count, 65535);
    buff[0]= 254;
    int2store(buff+1, tmp);
    /*
      The following test should never be true, but it's better to do it
      because if 'is_fatal_error' is set the server is not going to execute
      other queries (see the if test in dispatch_command / COM_QUERY)
    */
    if (unlikely(thd->is_fatal_error))
      server_status&= ~SERVER_MORE_RESULTS_EXISTS;
    int2store(buff + 3, server_status);
    error= my_net_write(net, buff, 5);
  }
  else
    error= my_net_write(net, eof_buff, 1);
  
  return error;
}

/**
  @param thd Thread handler
  @param sql_errno The error code to send
  @param err A pointer to the error message

  @return
   @retval FALSE The message was successfully sent
   @retval TRUE  An error occurred and the messages wasn't sent properly
*/

bool Protocol::net_send_error_packet(THD *thd, uint sql_errno, const char *err,
                                     const char* sqlstate)

{
  NET *net= &thd->net;
  uint length;
  /*
    buff[]: sql_errno:2 + ('#':1 + SQLSTATE_LENGTH:5) + MYSQL_ERRMSG_SIZE:512
  */
  uint error;
  char converted_err[MYSQL_ERRMSG_SIZE];
  char buff[2+1+SQLSTATE_LENGTH+MYSQL_ERRMSG_SIZE], *pos;
  my_bool ret;
  uint8 save_compress;
  DBUG_ENTER("Protocol::send_error_packet");

  if (net->vio == 0)
  {
    if (thd->bootstrap)
    {
      /* In bootstrap it's ok to print on stderr */
      fprintf(stderr,"ERROR: %d  %s\n",sql_errno,err);
    }
    DBUG_RETURN(FALSE);
  }

  int2store(buff,sql_errno);
  pos= buff+2;
  if (thd->client_capabilities & CLIENT_PROTOCOL_41)
  {
    /* The first # is to make the protocol backward compatible */
    buff[2]= '#';
    pos= strmov(buff+3, sqlstate);
  }

  convert_error_message(converted_err, sizeof(converted_err),
                        thd->variables.character_set_results,
                        err, strlen(err), system_charset_info, &error);
  /* Converted error message is always null-terminated. */
  length= (uint) (strmake(pos, converted_err, MYSQL_ERRMSG_SIZE - 1) - buff);

  /*
    Ensure that errors are not compressed. This is to ensure we can
    detect out of bands error messages in the client
  */
  if ((save_compress= net->compress))
    net->compress= 2;

  /*
    Sometimes, we send errors "out-of-band", e.g ER_CONNECTION_KILLED
    on an idle connection. The current protocol "sequence number" is 0,
    however some client drivers would however always   expect packets
    coming from server to have seq_no > 0, due to missing awareness
    of "out-of-band" operations. Make these clients happy.
  */
  if (!net->pkt_nr &&
      (sql_errno == ER_CONNECTION_KILLED || sql_errno == ER_SERVER_SHUTDOWN ||
       sql_errno == ER_QUERY_INTERRUPTED))
  {
    net->pkt_nr= 1;
  }

  ret= net_write_command(net,(uchar) 255, (uchar*) "", 0, (uchar*) buff,
                         length);
  net->compress= save_compress;
  DBUG_RETURN(ret);
}

#endif /* EMBEDDED_LIBRARY */

/**
  Faster net_store_length when we know that length is less than 65536.
  We keep a separate version for that range because it's widely used in
  libmysql.

  uint is used as agrument type because of MySQL type conventions:
  - uint for 0..65536
  - ulong for 0..4294967296
  - ulonglong for bigger numbers.
*/

static uchar *net_store_length_fast(uchar *packet, size_t length)
{
  DBUG_ASSERT(length < UINT_MAX16);
  if (length < 251)
  {
    *packet=(uchar) length;
    return packet+1;
  }
  *packet++=252;
  int2store(packet,(uint) length);
  return packet+2;
}

/**
  Send the status of the current statement execution over network.

  @param  thd   in fact, carries two parameters, NET for the transport and
                Diagnostics_area as the source of status information.

  In MySQL, there are two types of SQL statements: those that return
  a result set and those that return status information only.

  If a statement returns a result set, it consists of 3 parts:
  - result set meta-data
  - variable number of result set rows (can be 0)
  - followed and terminated by EOF or ERROR packet

  Once the  client has seen the meta-data information, it always
  expects an EOF or ERROR to terminate the result set. If ERROR is
  received, the result set rows are normally discarded (this is up
  to the client implementation, libmysql at least does discard them).
  EOF, on the contrary, means "successfully evaluated the entire
  result set". Since we don't know how many rows belong to a result
  set until it's evaluated, EOF/ERROR is the indicator of the end
  of the row stream. Note, that we can not buffer result set rows
  on the server -- there may be an arbitrary number of rows. But
  we do buffer the last packet (EOF/ERROR) in the Diagnostics_area and
  delay sending it till the very end of execution (here), to be able to
  change EOF to an ERROR if commit failed or some other error occurred
  during the last cleanup steps taken after execution.

  A statement that does not return a result set doesn't send result
  set meta-data either. Instead it returns one of:
  - OK packet
  - ERROR packet.
  Similarly to the EOF/ERROR of the previous statement type, OK/ERROR
  packet is "buffered" in the diagnostics area and sent to the client
  in the end of statement.

  @note This method defines a template, but delegates actual 
  sending of data to virtual Protocol::send_{ok,eof,error}. This
  allows for implementation of protocols that "intercept" ok/eof/error
  messages, and store them in memory, etc, instead of sending to
  the client.

  @pre  The diagnostics area is assigned or disabled. It can not be empty
        -- we assume that every SQL statement or COM_* command
        generates OK, ERROR, or EOF status.

  @post The status information is encoded to protocol format and sent to the
        client.

  @return We conventionally return void, since the only type of error
          that can happen here is a NET (transport) error, and that one
          will become visible when we attempt to read from the NET the
          next command.
          Diagnostics_area::is_sent is set for debugging purposes only.
*/

void Protocol::end_statement()
{
#ifdef WITH_WSREP
  /*
    Commented out: This sanity check does not hold in general.
    Thd->LOCK_thd_data() must be unlocked before sending response
    to client, so BF abort may sneak in here.
    DBUG_ASSERT(!WSREP(thd) || thd->wsrep_conflict_state() == NO_CONFLICT);
  */

  /*
    sanity check, don't send end statement while replaying
  */
  DBUG_ASSERT(thd->wsrep_trx().state() != wsrep::transaction::s_replaying);
  if (WSREP(thd) && thd->wsrep_trx().state() ==
      wsrep::transaction::s_replaying)
  {
    WSREP_ERROR("attempting net_end_statement while replaying");
    return;
  }
#endif /* WITH_WSREP */

  DBUG_ENTER("Protocol::end_statement");
  DBUG_ASSERT(! thd->get_stmt_da()->is_sent());
  bool error= FALSE;

  /* Can not be true, but do not take chances in production. */
  if (thd->get_stmt_da()->is_sent())
    DBUG_VOID_RETURN;

  switch (thd->get_stmt_da()->status()) {
  case Diagnostics_area::DA_ERROR:
    /* The query failed, send error to log and abort bootstrap. */
    error= send_error(thd->get_stmt_da()->sql_errno(),
                      thd->get_stmt_da()->message(),
                      thd->get_stmt_da()->get_sqlstate());
    break;
  case Diagnostics_area::DA_EOF:
  case Diagnostics_area::DA_EOF_BULK:
    error= send_eof(thd->server_status,
                    thd->get_stmt_da()->statement_warn_count());
    break;
  case Diagnostics_area::DA_OK:
  case Diagnostics_area::DA_OK_BULK:
    error= send_ok(thd->server_status,
                   thd->get_stmt_da()->statement_warn_count(),
                   thd->get_stmt_da()->affected_rows(),
                   thd->get_stmt_da()->last_insert_id(),
                   thd->get_stmt_da()->message());
    break;
  case Diagnostics_area::DA_DISABLED:
    break;
  case Diagnostics_area::DA_EMPTY:
  default:
    DBUG_ASSERT(0);
    error= send_ok(thd->server_status, 0, 0, 0, NULL);
    break;
  }
  if (likely(!error))
    thd->get_stmt_da()->set_is_sent(true);
  DBUG_VOID_RETURN;
}

/**
  A default implementation of "OK" packet response to the client.

  Currently this implementation is re-used by both network-oriented
  protocols -- the binary and text one. They do not differ
  in their OK packet format, which allows for a significant simplification
  on client side.
*/

bool Protocol::send_ok(uint server_status, uint statement_warn_count,
                       ulonglong affected_rows, ulonglong last_insert_id,
                       const char *message)
{
  DBUG_ENTER("Protocol::send_ok");
  const bool retval=
    net_send_ok(thd, server_status, statement_warn_count,
                affected_rows, last_insert_id, message, false);
  DBUG_RETURN(retval);
}


/**
  A default implementation of "EOF" packet response to the client.

  Binary and text protocol do not differ in their EOF packet format.
*/

bool Protocol::send_eof(uint server_status, uint statement_warn_count)
{
  DBUG_ENTER("Protocol::send_eof");
  bool retval= net_send_eof(thd, server_status, statement_warn_count);
  DBUG_RETURN(retval);
}


/**
  A default implementation of "ERROR" packet response to the client.

  Binary and text protocol do not differ in ERROR packet format.
*/

bool Protocol::send_error(uint sql_errno, const char *err_msg,
                          const char *sql_state)
{
  DBUG_ENTER("Protocol::send_error");
  const bool retval= net_send_error_packet(thd, sql_errno, err_msg, sql_state);
  DBUG_RETURN(retval);
}


/**
   Send a progress report to the client

   What we send is:
   header (255,255,255,1)
   stage, max_stage as on byte integers
   percentage withing the stage as percentage*1000
   (that is, ratio*100000) as a 3 byte integer
   proc_info as a string
*/

const uchar progress_header[2]= {(uchar) 255, (uchar) 255 };

void net_send_progress_packet(THD *thd)
{
  uchar buff[200], *pos;
  const char *proc_info= thd->proc_info ? thd->proc_info : "";
  size_t length= strlen(proc_info);
  ulonglong progress;
  DBUG_ENTER("net_send_progress_packet");

  if (unlikely(!thd->net.vio))
    DBUG_VOID_RETURN;                           // Socket is closed

  pos= buff;
  /*
    Store number of strings first. This allows us to later expand the
    progress indicator if needed.
  */
  *pos++= (uchar) 1;                            // Number of strings
  *pos++= (uchar) thd->progress.stage + 1;
  /*
    We have the MY_MAX() here to avoid problems if max_stage is not set,
    which may happen during automatic repair of table
  */
  *pos++= (uchar) MY_MAX(thd->progress.max_stage, thd->progress.stage + 1);
  progress= 0;
  if (thd->progress.max_counter)
    progress= 100000ULL * thd->progress.counter / thd->progress.max_counter;
  int3store(pos, progress);                          // Between 0 & 100000
  pos+= 3;
  pos= net_store_data(pos, (const uchar*) proc_info,
                      MY_MIN(length, sizeof(buff)-7));
  net_write_command(&thd->net, (uchar) 255, progress_header,
                    sizeof(progress_header), (uchar*) buff,
                    (uint) (pos - buff));
  DBUG_VOID_RETURN;
}

  
/****************************************************************************
  Functions used by the protocol functions (like net_send_ok) to store
  strings and numbers in the header result packet.
****************************************************************************/

/* The following will only be used for short strings < 65K */

uchar *net_store_data(uchar *to, const uchar *from, size_t length)
{
  to=net_store_length_fast(to,length);
  if (length)
    memcpy(to,from,length);
  return to+length;
}

uchar *net_store_data(uchar *to,int32 from)
{
  char buff[22];
  uint length=(uint) (int10_to_str(from,buff,10)-buff);
  to=net_store_length_fast(to,length);
  memcpy(to,buff,length);
  return to+length;
}

uchar *net_store_data(uchar *to,longlong from)
{
  char buff[22];
  uint length=(uint) (longlong10_to_str(from,buff,10)-buff);
  to=net_store_length_fast(to,length);
  memcpy(to,buff,length);
  return to+length;
}


/*****************************************************************************
  Default Protocol functions
*****************************************************************************/

void Protocol::init(THD *thd_arg)
{
  thd=thd_arg;
  packet= &thd->packet;
  convert= &thd->convert_buffer;
#ifndef DBUG_OFF
  field_handlers= 0;
  field_pos= 0;
#endif
}

/**
  Finish the result set with EOF packet, as is expected by the client,
  if there is an error evaluating the next row and a continue handler
  for the error.
*/

void Protocol::end_partial_result_set(THD *thd_arg)
{
  net_send_eof(thd_arg, thd_arg->server_status,
               0 /* no warnings, we're inside SP */);
}


bool Protocol::flush()
{
#ifndef EMBEDDED_LIBRARY
  bool error;
  thd->get_stmt_da()->set_overwrite_status(true);
  error= net_flush(&thd->net);
  thd->get_stmt_da()->set_overwrite_status(false);
  return error;
#else
  return 0;
#endif
}

#ifndef EMBEDDED_LIBRARY


class Send_field_packed_extended_metadata: public Binary_string
{
public:
  bool append_chunk(mariadb_field_attr_t type, const LEX_CSTRING &value)
  {
    /*
      If we eventually support many metadata chunk types and long metadata
      values, we'll need to encode type and length using net_store_length()
      and do corresponding changes to the unpacking code in libmariadb.
      For now let's just assert that type and length fit into one byte.
    */
    DBUG_ASSERT(net_length_size(type) == 1);
    DBUG_ASSERT(net_length_size(value.length) == 1);
    size_t nbytes= 1/*type*/ + 1/*length*/ + value.length;
    if (reserve(nbytes))
      return true;
    qs_append((char) (uchar) type);
    qs_append((char) (uchar) value.length);
    qs_append(&value);
    return false;
  }
  bool pack(const Send_field_extended_metadata &src)
  {
    for (uint i= 0 ; i <= MARIADB_FIELD_ATTR_LAST; i++)
    {
      const LEX_CSTRING attr= src.attr(i);
      if (attr.str && append_chunk((mariadb_field_attr_t) i, attr))
        return true;
    }
    return false;
  }
};


bool Protocol_text::store_field_metadata(const THD * thd,
                                         const Send_field &field,
                                         CHARSET_INFO *charset_for_protocol,
                                         uint fieldnr)
{
  CHARSET_INFO *thd_charset= thd->variables.character_set_results;
  char *pos;
  DBUG_ASSERT(field.is_sane());

  if (thd->client_capabilities & CLIENT_PROTOCOL_41)
  {
    const LEX_CSTRING def= {STRING_WITH_LEN("def")};
    if (store_ident(def) ||
        store_ident(field.db_name) ||
        store_ident(field.table_name) ||
        store_ident(field.org_table_name) ||
        store_ident(field.col_name) ||
        store_ident(field.org_col_name))
      return true;
    if (thd->client_capabilities & MARIADB_CLIENT_EXTENDED_METADATA)
    {
      Send_field_packed_extended_metadata metadata;
      metadata.pack(field);

      /*
        Don't apply character set conversion:
        extended metadata is a binary encoded data.
      */
      if (store_binary_string(metadata.ptr(), metadata.length()))
        return true;
    }
    if (packet->realloc(packet->length() + 12))
      return true;
    /* Store fixed length fields */
    pos= (char*) packet->end();
    *pos++= 12;                                // Length of packed fields
    /* inject a NULL to test the client */
    DBUG_EXECUTE_IF("poison_rs_fields", pos[-1]= (char) 0xfb;);
    if (charset_for_protocol == &my_charset_bin || thd_charset == NULL)
    {
      /* No conversion */
      int2store(pos, charset_for_protocol->number);
      int4store(pos + 2, field.length);
    }
    else
    {
      /* With conversion */
      int2store(pos, thd_charset->number);
      uint32 field_length= field.max_octet_length(charset_for_protocol,
                                                  thd_charset);
      int4store(pos + 2, field_length);
    }
    pos[6]= field.type_handler()->type_code_for_protocol();
    int2store(pos + 7, field.flags);
    pos[9]= (char) field.decimals;
    pos[10]= 0;                                // For the future
    pos[11]= 0;                                // For the future
    pos+= 12;
  }
  else
  {
    if (store_ident(field.table_name) ||
        store_ident(field.col_name) ||
        packet->realloc(packet->length() + 10))
      return true;
    pos= (char*) packet->end();
    pos[0]= 3;
    int3store(pos + 1, field.length);
    pos[4]= 1;
    pos[5]= field.type_handler()->type_code_for_protocol();
    pos[6]= 3;
    int2store(pos + 7, field.flags);
    pos[9]= (char) field.decimals;
    pos+= 10;
  }
  packet->length((uint) (pos - packet->ptr()));
  return false;
}


/*
  MARIADB_CLIENT_CACHE_METADATA  support.

  Bulk of the code below is dedicated to detecting whether column metadata has
  changed after prepare, or between executions of a prepared statement.

  For some prepared statements, metadata can't change without going through
  Prepared_Statement::reprepare(), which makes detecting changes easy.

  Others, "SELECT ?" & Co, are more fragile, and sensitive to input parameters,
  or user variables. Detecting metadata change for this class of PS is harder,
  we calculate signature (hash value), and check whether this changes between
  executions. This is a more expensive method.
*/


/**
  Detect whether column info can be changed without
  PS repreparing.

  Such colum info is called fragile. The opposite of
  fragile is.


  @param  it - Item representing column info
  @return true, if columninfo is "fragile", false if it is stable


  @todo does not work due to MDEV-23913. Currently,
  everything about prepared statements is fragile.
*/

static bool is_fragile_columnifo(Item *it)
{
#define MDEV_23913_FIXED 0
#if MDEV_23913_FIXED
  if (dynamic_cast<Item_param *>(it))
    return true;

  if (dynamic_cast<Item_func_user_var *>(it))
    return true;

  if (dynamic_cast <Item_sp_variable*>(it))
    return true;

  /* Check arguments of functions.*/
  auto item_args= dynamic_cast<Item_args *>(it);
  if (!item_args)
    return false;
  auto args= item_args->arguments();
  auto arg_count= item_args->argument_count();
  for (uint i= 0; i < arg_count; i++)
  {
    if (is_fragile_columnifo(args[i]))
      return true;
  }
  return false;
#else /* MDEV-23913 fixed*/
  return true;
#endif
}


#define INVALID_METADATA_CHECKSUM 0


/**
  Calculate signature for column info sent to the client as CRC32 over data, 
  that goes into the column info packet.
  We assume that if checksum does not change, then column info was not
  modified.

  @param thd THD
  @param list column info

  @return CRC32 of the metadata
*/

static uint32 calc_metadata_hash(THD *thd, List<Item> *list)
{
  List_iterator_fast<Item> it(*list);
  Item *item;
  uint32 crc32_c= 0;
  while ((item= it++))
  {
    Send_field field(thd, item);
    auto field_type= item->type_handler()->field_type();
    auto charset= item->charset_for_protocol();
    /*
       The data below should contain everything that influences
       content of the column info packet.
    */
    LEX_CSTRING data[]=
    {
      field.table_name,
      field.org_table_name,
      field.col_name,
      field.org_col_name,
      field.db_name,
      field.attr(MARIADB_FIELD_ATTR_DATA_TYPE_NAME),
      field.attr(MARIADB_FIELD_ATTR_FORMAT_NAME),
      {(const char *) &field.length, sizeof(field.length)},
      {(const char *) &field.flags, sizeof(field.flags)},
      {(const char *) &field.decimals, sizeof(field.decimals)},
      {(const char *) &charset, sizeof(charset)},
      {(const char *) &field_type, sizeof(field_type)},
    };
    for (const auto &chunk : data)
      crc32_c= my_crc32c(crc32_c, chunk.str, chunk.length);
  }

  if (crc32_c == INVALID_METADATA_CHECKSUM)
     return 1;
  return crc32_c;
}



/**
  Check if metadata columns have changed since last call to this
  function.

  @param send_column_info_state  saved state, changed if the function
         return true. 
  @param thd THD
  @param list columninfo Items
  @return true,if metadata columns have changed since last call,
          false otherwise 
*/

static bool metadata_columns_changed(send_column_info_state &state, THD *thd,
                                     List<Item> &list)
{
  if (!state.initialized)
  {
    state.initialized= true;
    state.immutable= true;
    Item *item;
    List_iterator_fast<Item> it(list);
    while ((item= it++))
    {
      if (is_fragile_columnifo(item))
      {
        state.immutable= false;
        state.checksum= calc_metadata_hash(thd, &list);
        break;
      }
    }
    state.last_charset= thd->variables.character_set_client;
    return true;
  }
  
  /*
    Since column info can change under our feet, we use more expensive
    checksumming to check if column metadata has not changed since last time.
  */
  if (!state.immutable)
  {
    uint32 checksum= calc_metadata_hash(thd, &list);
    if (checksum != state.checksum)
    {
      state.checksum= checksum;
      state.last_charset= thd->variables.character_set_client;
      return true;
    }
  }

  /*
    Character_set_client influences result set metadata, thus resend metadata
    whenever it changes.
  */
  if (state.last_charset != thd->variables.character_set_client)
  {
    state.last_charset= thd->variables.character_set_client;
    return true;
  }

  return false;
}


/**
  Determine whether column info must be sent to the client.
  Skip column info, if client supports caching, and (prepared) statement
  output fields have not changed.

  @param thd THD
  @param list column info
  @param flags send flags. If Protocol::SEND_FORCE_COLUMN_INFO is set,
         this function will return true
  @return true, if column info must be sent to the client.
          false otherwise
*/

static bool should_send_column_info(THD* thd, List<Item>* list, uint flags) 
{
  if (!(thd->client_capabilities & MARIADB_CLIENT_CACHE_METADATA))
  {
    /* Client does not support abbreviated metadata.*/
    return true;
  }

  if (!thd->cur_stmt)
  {
    /* Neither COM_PREPARE nor COM_EXECUTE run.*/
    return true;
  }

  if (thd->spcont)
  {
    /* Always sent full metadata from inside the stored procedure.*/
    return true;
  }

  if (flags & Protocol::SEND_FORCE_COLUMN_INFO)
    return true;

  auto &column_info_state= thd->cur_stmt->column_info_state;
#ifndef DBUG_OFF
  auto cmd= thd->get_command();
#endif

  DBUG_ASSERT(cmd == COM_STMT_EXECUTE || cmd == COM_STMT_PREPARE
              || cmd == COM_STMT_BULK_EXECUTE);
  DBUG_ASSERT(cmd != COM_STMT_PREPARE || !column_info_state.initialized);

  bool ret= metadata_columns_changed(column_info_state, thd, *list);

  DBUG_ASSERT(cmd != COM_STMT_PREPARE || ret);
  if (!ret)
    thd->status_var.skip_metadata_count++;

  return ret;
}


/**
  Send name and type of result to client.

  Sum fields has table name empty and field_name.

  @param THD		Thread data object
  @param list	        List of items to send to client
  @param flag	        Bit mask with the following functions:
                        - 1 send number of rows
                        - 2 send default values
                        - 4 don't write eof packet

  @retval
    0	ok
  @retval
    1	Error  (Note that in this case the error is not sent to the
    client)
*/
bool Protocol::send_result_set_metadata(List<Item> *list, uint flags)
{
  DBUG_ENTER("Protocol::send_result_set_metadata");

  bool send_column_info= should_send_column_info(thd, list, flags);

  if (flags & SEND_NUM_ROWS)
  {
    /*
      Packet with number of columns.

      Will also have a 1 byte column info indicator, in case
      MARIADB_CLIENT_CACHE_METADATA client capability is set.
    */
    uchar buff[MAX_INT_WIDTH+1];
    uchar *pos= net_store_length(buff, list->elements);
    if (thd->client_capabilities & MARIADB_CLIENT_CACHE_METADATA)
      *pos++= (uchar)send_column_info;

    DBUG_ASSERT(pos <= buff + sizeof(buff));
    if (my_net_write(&thd->net, buff, (size_t) (pos-buff)))
      DBUG_RETURN(1);
  }

  if (send_column_info)
  {
    List_iterator_fast<Item> it(*list);
    Item *item;
    Protocol_text prot(thd, thd->variables.net_buffer_length);
#ifndef DBUG_OFF
    field_handlers= (const Type_handler **) thd->alloc(
        sizeof(field_handlers[0]) * list->elements);
#endif

    for (uint pos= 0; (item= it++); pos++)
    {
      prot.prepare_for_resend();
      if (prot.store_item_metadata(thd, item, pos))
        goto err;
      if (prot.write())
        DBUG_RETURN(1);
#ifndef DBUG_OFF
      field_handlers[pos]= item->type_handler();
#endif
    }
  }

  if (flags & SEND_EOF)
  {

    /* if it is new client do not send EOF packet */
    if (!(thd->client_capabilities & CLIENT_DEPRECATE_EOF))
    {
      /*
        Mark the end of meta-data result set, and store thd->server_status,
        to show that there is no cursor.
        Send no warning information, as it will be sent at statement end.
      */
      if (write_eof_packet(thd, &thd->net, thd->server_status,
                           thd->get_stmt_da()->current_statement_warn_count()))
        DBUG_RETURN(1);
    }
  }
  DBUG_RETURN(prepare_for_send(list->elements));

err:
  my_message(ER_OUT_OF_RESOURCES, ER_THD(thd, ER_OUT_OF_RESOURCES),
             MYF(0));	/* purecov: inspected */
  DBUG_RETURN(1);				/* purecov: inspected */
}


bool Protocol::send_list_fields(List<Field> *list, const TABLE_LIST *table_list)
{
  DBUG_ENTER("Protocol::send_list_fields");
  List_iterator_fast<Field> it(*list);
  Field *fld;
  Protocol_text prot(thd, thd->variables.net_buffer_length);

#ifndef DBUG_OFF
  field_handlers= (const Type_handler **) thd->alloc(sizeof(field_handlers[0]) *
                                                     list->elements);
#endif

  for (uint pos= 0; (fld= it++); pos++)
  {
    prot.prepare_for_resend();
    if (prot.store_field_metadata_for_list_fields(thd, fld, table_list, pos))
      goto err;
    prot.store(fld);   // Send default value
    if (prot.write())
      DBUG_RETURN(1);
#ifndef DBUG_OFF
    /*
      Historically all BLOB variant Fields are displayed as
      MYSQL_TYPE_BLOB in metadata.
      See Field_blob::make_send_field() for more comments.
    */
    field_handlers[pos]= Send_field(fld).type_handler();
#endif
  }
  DBUG_RETURN(prepare_for_send(list->elements));

err:
  my_message(ER_OUT_OF_RESOURCES, ER_THD(thd, ER_OUT_OF_RESOURCES), MYF(0));
  DBUG_RETURN(1);
}


bool Protocol::write()
{
  DBUG_ENTER("Protocol::write");
  DBUG_RETURN(my_net_write(&thd->net, (uchar*) packet->ptr(),
                           packet->length()));
}
#endif /* EMBEDDED_LIBRARY */


bool Protocol_text::store_item_metadata(THD *thd, Item *item, uint pos)
{
  Send_field field(thd, item);
  return store_field_metadata(thd, field, item->charset_for_protocol(), pos);
}


bool Protocol_text::store_field_metadata_for_list_fields(const THD *thd,
                                                         Field *fld,
                                                         const TABLE_LIST *tl,
                                                         uint pos)
{
  Send_field field= tl->view ?
                    Send_field(fld, tl->view_db, tl->view_name) :
                    Send_field(fld);
  return store_field_metadata(thd, field, fld->charset_for_protocol(), pos);
}


/**
  Send one result set row.

  @param row_items a collection of column values for that row

  @return Error status.
    @retval TRUE  Error.
    @retval FALSE Success.
*/

bool Protocol::send_result_set_row(List<Item> *row_items)
{
  List_iterator_fast<Item> it(*row_items);
  ValueBuffer<MAX_FIELD_WIDTH> value_buffer;
  DBUG_ENTER("Protocol::send_result_set_row");

  for (Item *item= it++; item; item= it++)
  {
    value_buffer.reset_buffer();
    if (item->send(this, &value_buffer))
    {
      // If we're out of memory, reclaim some, to help us recover.
      this->free();
      DBUG_RETURN(TRUE);
    }
    /* Item::send() may generate an error. If so, abort the loop. */
    if (unlikely(thd->is_error()))
      DBUG_RETURN(TRUE);
  }

  DBUG_RETURN(FALSE);
}


/**
  Send \\0 end terminated string or NULL

  @param from    NullS or \\0 terminated string

  @note
    In most cases one should use store(from, length) instead of this function

  @retval
    0		ok
  @retval
    1		error
*/

bool Protocol::store_string_or_null(const char *from, CHARSET_INFO *cs)
{
  if (!from)
    return store_null();
  return store(from, strlen(from), cs);
}


/**
  Send a set of strings as one long string with ',' in between.
*/

bool Protocol::store(I_List<i_string>* str_list)
{
  char buf[256];
  String tmp(buf, sizeof(buf), &my_charset_bin);
  uint32 len;
  I_List_iterator<i_string> it(*str_list);
  i_string* s;

  tmp.length(0);
  while ((s=it++))
  {
    tmp.append(s->ptr, strlen(s->ptr));
    tmp.append(',');
  }
  if ((len= tmp.length()))
    len--;					// Remove last ','
  return store((char*) tmp.ptr(), len,  tmp.charset());
}

/****************************************************************************
  Functions to handle the simple (default) protocol where everything is
  This protocol is the one that is used by default between the MySQL server
  and client when you are not using prepared statements.

  All data are sent as 'packed-string-length' followed by 'string-data'
****************************************************************************/

#ifndef EMBEDDED_LIBRARY
void Protocol_text::prepare_for_resend()
{
  packet->length(0);
#ifndef DBUG_OFF
  field_pos= 0;
#endif
}

bool Protocol_text::store_null()
{
#ifndef DBUG_OFF
  field_pos++;
#endif
  char buff[1];
  buff[0]= (char)251;
  return packet->append(buff, sizeof(buff), PACKET_BUFFER_EXTRA_ALLOC);
}
#endif


/**
  Auxilary function to convert string to the given character set
  and store in network buffer.
*/

bool Protocol::store_string_aux(const char *from, size_t length,
                                CHARSET_INFO *fromcs, CHARSET_INFO *tocs)
{
  /* 'tocs' is set 0 when client issues SET character_set_results=NULL */
  if (needs_conversion(fromcs, tocs))
  {
    /* Store with conversion */
    return net_store_data_cs((uchar*) from, length, fromcs, tocs);
  }
  /* Store without conversion */
  return net_store_data((uchar*) from, length);
}


bool Protocol_text::store_numeric_string_aux(const char *from, size_t length)
{
  CHARSET_INFO *tocs= thd->variables.character_set_results;
  // 'tocs' is NULL when the client issues SET character_set_results=NULL
  if (tocs && (tocs->state & MY_CS_NONASCII))   // Conversion needed
    return net_store_data_cs((uchar*) from, length, &my_charset_latin1, tocs);
  return net_store_data((uchar*) from, length); // No conversion
}


bool Protocol::store_warning(const char *from, size_t length)
{
  BinaryStringBuffer<MYSQL_ERRMSG_SIZE> tmp;
  CHARSET_INFO *cs= thd->variables.character_set_results;
  if (!cs || cs == &my_charset_bin)
    cs= system_charset_info;
  if (tmp.copy_printable_hhhh(cs, system_charset_info, from, length))
    return net_store_data((const uchar*)"", 0);
  return net_store_data((const uchar *) tmp.ptr(), tmp.length());
}


bool Protocol_text::store_str(const char *from, size_t length,
                              CHARSET_INFO *fromcs, CHARSET_INFO *tocs)
{
#ifndef DBUG_OFF
  DBUG_PRINT("info", ("Protocol_text::store field %u : %.*b", field_pos,
                      (int) length, (length == 0 ? "" : from)));
  DBUG_ASSERT(field_handlers == 0 || field_pos < field_count);
  DBUG_ASSERT(valid_handler(field_pos, PROTOCOL_SEND_STRING));
  field_pos++;
#endif
  return store_string_aux(from, length, fromcs, tocs);
}


bool Protocol_text::store_numeric_zerofill_str(const char *from,
                                               size_t length,
                                               protocol_send_type_t send_type)
{
#ifndef DBUG_OFF
  DBUG_PRINT("info",
       ("Protocol_text::store_numeric_zerofill_str field %u : %.*b",
        field_pos, (int) length, (length == 0 ? "" : from)));
  DBUG_ASSERT(field_handlers == 0 || field_pos < field_count);
  DBUG_ASSERT(valid_handler(field_pos, send_type));
  field_pos++;
#endif
  return store_numeric_string_aux(from, length);
}


bool Protocol_text::store_tiny(longlong from)
{
#ifndef DBUG_OFF
  DBUG_ASSERT(valid_handler(field_pos, PROTOCOL_SEND_TINY));
  field_pos++;
#endif
  char buff[22];
  size_t length= (size_t) (int10_to_str((int) from, buff, -10) - buff);
  return store_numeric_string_aux(buff, length);
}


bool Protocol_text::store_short(longlong from)
{
#ifndef DBUG_OFF
  DBUG_ASSERT(valid_handler(field_pos, PROTOCOL_SEND_SHORT));
  field_pos++;
#endif
  char buff[22];
  size_t length= (size_t) (int10_to_str((int) from, buff, -10) - buff);
  return store_numeric_string_aux(buff, length);
}


bool Protocol_text::store_long(longlong from)
{
#ifndef DBUG_OFF
  DBUG_ASSERT(valid_handler(field_pos, PROTOCOL_SEND_LONG));
  field_pos++;
#endif
  char buff[22];
  size_t length= (size_t) (int10_to_str((long int)from, buff,
                                        (from < 0) ? - 10 : 10) - buff);
  return store_numeric_string_aux(buff, length);
}


bool Protocol_text::store_longlong(longlong from, bool unsigned_flag)
{
#ifndef DBUG_OFF
  DBUG_ASSERT(valid_handler(field_pos, PROTOCOL_SEND_LONGLONG));
  field_pos++;
#endif
  char buff[22];
  size_t length= (size_t) (longlong10_to_str(from, buff,
                                             unsigned_flag ? 10 : -10) -
                           buff);
  return store_numeric_string_aux(buff, length);
}


bool Protocol_text::store_decimal(const my_decimal *d)
{
#ifndef DBUG_OFF
  DBUG_ASSERT(0); // This method is not used yet
  field_pos++;
#endif
  StringBuffer<DECIMAL_MAX_STR_LENGTH> str;
  (void) d->to_string(&str);
  return store_numeric_string_aux(str.ptr(), str.length());
}


bool Protocol_text::store_float(float from, uint32 decimals)
{
#ifndef DBUG_OFF
  DBUG_ASSERT(valid_handler(field_pos, PROTOCOL_SEND_FLOAT));
  field_pos++;
#endif
  Float(from).to_string(&buffer, decimals);
  return store_numeric_string_aux(buffer.ptr(), buffer.length());
}


bool Protocol_text::store_double(double from, uint32 decimals)
{
#ifndef DBUG_OFF
  DBUG_ASSERT(valid_handler(field_pos, PROTOCOL_SEND_DOUBLE));
  field_pos++;
#endif
  buffer.set_real(from, decimals, thd->charset());
  return store_numeric_string_aux(buffer.ptr(), buffer.length());
}


bool Protocol_text::store(Field *field)
{
  if (field->is_null())
    return store_null();
#ifdef DBUG_ASSERT_EXISTS
  TABLE *table= field->table;
  MY_BITMAP *old_map= 0;
  if (table->file)
    old_map= dbug_tmp_use_all_columns(table, &table->read_set);
#endif

  bool rc= field->send(this);

#ifdef DBUG_ASSERT_EXISTS
  if (old_map)
    dbug_tmp_restore_column_map(&table->read_set, old_map);
#endif

  return rc;
}


bool Protocol_text::store_datetime(MYSQL_TIME *tm, int decimals)
{
#ifndef DBUG_OFF
  DBUG_ASSERT(valid_handler(field_pos, PROTOCOL_SEND_DATETIME));
  field_pos++;
#endif
  char buff[MAX_DATE_STRING_REP_LENGTH];
  uint length= my_datetime_to_str(tm, buff, decimals);
  return store_numeric_string_aux(buff, length);
}


bool Protocol_text::store_date(MYSQL_TIME *tm)
{
#ifndef DBUG_OFF
  DBUG_ASSERT(valid_handler(field_pos, PROTOCOL_SEND_DATE));
  field_pos++;
#endif
  char buff[MAX_DATE_STRING_REP_LENGTH];
  size_t length= my_date_to_str(tm, buff);
  return store_numeric_string_aux(buff, length);
}


bool Protocol_text::store_time(MYSQL_TIME *tm, int decimals)
{
#ifndef DBUG_OFF
  DBUG_ASSERT(valid_handler(field_pos, PROTOCOL_SEND_TIME));
  field_pos++;
#endif
  char buff[MAX_DATE_STRING_REP_LENGTH];
  uint length= my_time_to_str(tm, buff, decimals);
  return store_numeric_string_aux(buff, length);
}

/**
  Assign OUT-parameters to user variables.

  @param sp_params  List of PS/SP parameters (both input and output).

  @return Error status.
    @retval FALSE Success.
    @retval TRUE  Error.
*/

bool Protocol_text::send_out_parameters(List<Item_param> *sp_params)
{
  DBUG_ASSERT(sp_params->elements == thd->lex->prepared_stmt.param_count());

  List_iterator_fast<Item_param> item_param_it(*sp_params);
  List_iterator_fast<Item> param_it(thd->lex->prepared_stmt.params());

  while (true)
  {
    Item_param *item_param= item_param_it++;
    Item *param= param_it++;
    Settable_routine_parameter *sparam;

    if (!item_param || !param)
      break;

    if (!item_param->get_out_param_info())
      continue; // It's an IN-parameter.

    if (!(sparam= param->get_settable_routine_parameter()))
    {
      DBUG_ASSERT(0);
      continue;
    }

    DBUG_ASSERT(sparam->get_item_param() == NULL);
    sparam->set_value(thd, thd->spcont, reinterpret_cast<Item **>(&item_param));
  }

  return FALSE;
}

/****************************************************************************
  Functions to handle the binary protocol used with prepared statements

  Data format:

   [ok:1]                            reserved ok packet
   [null_field:(field_count+7+2)/8]  reserved to send null data. The size is
                                     calculated using:
                                     bit_fields= (field_count+7+2)/8; 
                                     2 bits are reserved for identifying type
				     of package.
   [[length]data]                    data field (the length applies only for 
                                     string/binary/time/timestamp fields and 
                                     rest of them are not sent as they have 
                                     the default length that client understands
                                     based on the field type
   [..]..[[length]data]              data
****************************************************************************/

bool Protocol_binary::prepare_for_send(uint num_columns)
{
  Protocol::prepare_for_send(num_columns);
  bit_fields= (field_count+9)/8;
  return packet->alloc(bit_fields+1);

  /* prepare_for_resend will be called after this one */
}


void Protocol_binary::prepare_for_resend()
{
  packet->length(bit_fields+1);
  bzero((uchar*) packet->ptr(), 1+bit_fields);
  field_pos=0;
}


bool Protocol_binary::store_str(const char *from, size_t length,
                                CHARSET_INFO *fromcs, CHARSET_INFO *tocs)
{
  field_pos++;
  return store_string_aux(from, length, fromcs, tocs);
}

bool Protocol_binary::store_null()
{
  uint offset= (field_pos+2)/8+1, bit= (1 << ((field_pos+2) & 7));
  /* Room for this as it's allocated in prepare_for_send */
  char *to= (char*) packet->ptr()+offset;
  *to= (char) ((uchar) *to | (uchar) bit);
  field_pos++;
  return 0;
}


bool Protocol_binary::store_tiny(longlong from)
{
  char buff[1];
  field_pos++;
  buff[0]= (uchar) from;
  return packet->append(buff, sizeof(buff), PACKET_BUFFER_EXTRA_ALLOC);
}


bool Protocol_binary::store_short(longlong from)
{
  field_pos++;
  char *to= packet->prep_append(2, PACKET_BUFFER_EXTRA_ALLOC);
  if (!to)
    return 1;
  int2store(to, (int) from);
  return 0;
}


bool Protocol_binary::store_long(longlong from)
{
  field_pos++;
  char *to= packet->prep_append(4, PACKET_BUFFER_EXTRA_ALLOC);
  if (!to)
    return 1;
  int4store(to, from);
  return 0;
}


bool Protocol_binary::store_longlong(longlong from, bool unsigned_flag)
{
  field_pos++;
  char *to= packet->prep_append(8, PACKET_BUFFER_EXTRA_ALLOC);
  if (!to)
    return 1;
  int8store(to, from);
  return 0;
}

bool Protocol_binary::store_decimal(const my_decimal *d)
{
#ifndef DBUG_OFF
  DBUG_ASSERT(0); // This method is not used yet
#endif
  StringBuffer<DECIMAL_MAX_STR_LENGTH> str;
  (void) d->to_string(&str);
  return store_str(str.ptr(), str.length(), str.charset(),
                   thd->variables.character_set_results);
}

bool Protocol_binary::store_float(float from, uint32 decimals)
{
  field_pos++;
  char *to= packet->prep_append(4, PACKET_BUFFER_EXTRA_ALLOC);
  if (!to)
    return 1;
  float4store(to, from);
  return 0;
}


bool Protocol_binary::store_double(double from, uint32 decimals)
{
  field_pos++;
  char *to= packet->prep_append(8, PACKET_BUFFER_EXTRA_ALLOC);
  if (!to)
    return 1;
  float8store(to, from);
  return 0;
}


bool Protocol_binary::store(Field *field)
{
  /*
    We should not increment field_pos here as send() will call another
    protocol function to do this for us
  */
  if (field->is_null())
    return store_null();
  return field->send(this);
}


bool Protocol_binary::store_datetime(MYSQL_TIME *tm, int decimals)
{
  char buff[12],*pos;
  uint length;
  field_pos++;
  pos= buff+1;

  int2store(pos, tm->year);
  pos[2]= (uchar) tm->month;
  pos[3]= (uchar) tm->day;
  pos[4]= (uchar) tm->hour;
  pos[5]= (uchar) tm->minute;
  pos[6]= (uchar) tm->second;
  DBUG_ASSERT(decimals == AUTO_SEC_PART_DIGITS ||
              (decimals >= 0 && decimals <= TIME_SECOND_PART_DIGITS));
  if (decimals != AUTO_SEC_PART_DIGITS)
    my_datetime_trunc(tm, decimals);
  int4store(pos+7, tm->second_part);
  if (tm->second_part)
    length=11;
  else if (tm->hour || tm->minute || tm->second)
    length=7;
  else if (tm->year || tm->month || tm->day)
    length=4;
  else
    length=0;
  buff[0]=(char) length;			// Length is stored first
  return packet->append(buff, length+1, PACKET_BUFFER_EXTRA_ALLOC);
}

bool Protocol_binary::store_date(MYSQL_TIME *tm)
{
  tm->hour= tm->minute= tm->second=0;
  tm->second_part= 0;
  return Protocol_binary::store_datetime(tm, 0);
}


bool Protocol_binary::store_time(MYSQL_TIME *tm, int decimals)
{
  char buff[13], *pos;
  uint length;
  field_pos++;
  pos= buff+1;
  pos[0]= tm->neg ? 1 : 0;
  if (tm->hour >= 24)
  {
    uint days= tm->hour/24;
    tm->hour-= days*24;
    tm->day+= days;
  }
  int4store(pos+1, tm->day);
  pos[5]= (uchar) tm->hour;
  pos[6]= (uchar) tm->minute;
  pos[7]= (uchar) tm->second;
  DBUG_ASSERT(decimals == AUTO_SEC_PART_DIGITS ||
              (decimals >= 0 && decimals <= TIME_SECOND_PART_DIGITS));
  if (decimals != AUTO_SEC_PART_DIGITS)
    my_time_trunc(tm, decimals);
  int4store(pos+8, tm->second_part);
  if (tm->second_part)
    length=12;
  else if (tm->hour || tm->minute || tm->second || tm->day)
    length=8;
  else
    length=0;
  buff[0]=(char) length;			// Length is stored first
  return packet->append(buff, length+1, PACKET_BUFFER_EXTRA_ALLOC);
}

/**
  Send a result set with OUT-parameter values by means of PS-protocol.

  @param sp_params  List of PS/SP parameters (both input and output).

  @return Error status.
    @retval FALSE Success.
    @retval TRUE  Error.
*/

bool Protocol_binary::send_out_parameters(List<Item_param> *sp_params)
{
  bool ret;
  if (!(thd->client_capabilities & CLIENT_PS_MULTI_RESULTS))
  {
    /* The client does not support OUT-parameters. */
    return FALSE;
  }

  List<Item> out_param_lst;

  {
    List_iterator_fast<Item_param> item_param_it(*sp_params);

    while (true)
    {
      Item_param *item_param= item_param_it++;

      if (!item_param)
        break;

      if (!item_param->get_out_param_info())
        continue; // It's an IN-parameter.

      if (out_param_lst.push_back(item_param, thd->mem_root))
        return TRUE;
    }
  }

  if (!out_param_lst.elements)
    return FALSE;

  /*
    We have to set SERVER_PS_OUT_PARAMS in THD::server_status, because it
    is used in send_result_set_metadata().
  */

  thd->server_status|= SERVER_PS_OUT_PARAMS | SERVER_MORE_RESULTS_EXISTS;

  /* Send meta-data. */
  if (send_result_set_metadata(&out_param_lst,
        SEND_NUM_ROWS | SEND_EOF | SEND_FORCE_COLUMN_INFO))
    return TRUE;

  /* Send data. */

  prepare_for_resend();

  if (send_result_set_row(&out_param_lst))
    return TRUE;

  if (write())
    return TRUE;

  ret= net_send_eof(thd, thd->server_status, 0);

  /*
    Reset server_status:
    - SERVER_MORE_RESULTS_EXISTS bit, because this is the last packet for sure.
    - Restore SERVER_PS_OUT_PARAMS status.
  */
  thd->server_status&= ~(SERVER_PS_OUT_PARAMS | SERVER_MORE_RESULTS_EXISTS);

  return ret ? FALSE : TRUE;
}
