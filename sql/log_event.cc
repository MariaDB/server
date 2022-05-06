/*
   Copyright (c) 2000, 2018, Oracle and/or its affiliates.
   Copyright (c) 2009, 2021, MariaDB

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


#include "mariadb.h"
#include "sql_priv.h"
#include "handler.h"
#ifndef MYSQL_CLIENT
#include "unireg.h"
#include "log_event.h"
#include "sql_base.h"                           // close_thread_tables
#include "sql_cache.h"                       // QUERY_CACHE_FLAGS_SIZE
#include "sql_locale.h" // MY_LOCALE, my_locale_by_number, my_locale_en_US
#include "key.h"        // key_copy
#include "lock.h"       // mysql_unlock_tables
#include "sql_parse.h"  // mysql_test_parse_for_slave
#include "tztime.h"     // struct Time_zone
#include "sql_load.h"   // mysql_load
#include "sql_db.h"     // load_db_opt_by_name
#include "slave.h"
#include "rpl_rli.h"
#include "rpl_mi.h"
#include "rpl_filter.h"
#include "rpl_record.h"
#include "transaction.h"
#include <my_dir.h>
#include "sql_show.h"    // append_identifier
#include <mysql/psi/mysql_statement.h>
#include <strfunc.h>
#include "compat56.h"
#include "sql_insert.h"
#else
#include "mysqld_error.h"
#endif /* MYSQL_CLIENT */

#include <my_bitmap.h>
#include "rpl_utility.h"
#include "rpl_constants.h"
#include "sql_digest.h"
#include "zlib.h"
#include "myisampack.h"
#include <algorithm>

#define my_b_write_string(A, B) my_b_write((A), (uchar*)(B), (uint) (sizeof(B) - 1))

#ifndef _AIX
PSI_memory_key key_memory_log_event;
#endif
PSI_memory_key key_memory_Incident_log_event_message;
PSI_memory_key key_memory_Rows_query_log_event_rows_query;

/**
  BINLOG_CHECKSUM variable.
*/
const char *binlog_checksum_type_names[]= {
  "NONE",
  "CRC32",
  NullS
};

unsigned int binlog_checksum_type_length[]= {
  sizeof("NONE") - 1,
  sizeof("CRC32") - 1,
  0
};

TYPELIB binlog_checksum_typelib=
{
  array_elements(binlog_checksum_type_names) - 1, "",
  binlog_checksum_type_names,
  binlog_checksum_type_length
};


#define FLAGSTR(V,F) ((V)&(F)?#F" ":"")

/*
  Size of buffer for printing a double in format %.<PREC>g

  optional '-' + optional zero + '.'  + PREC digits + 'e' + sign +
  exponent digits + '\0'
*/
#define FMT_G_BUFSIZE(PREC) (3 + (PREC) + 5 + 1)

/* 
   replication event checksum is introduced in the following "checksum-home" version.
   The checksum-aware servers extract FD's version to decide whether the FD event
   carries checksum info.

   TODO: correct the constant when it has been determined 
   (which main tree to push and when) 
*/
const Version checksum_version_split_mysql(5, 6, 1);
const Version checksum_version_split_mariadb(5, 3, 0);

// First MySQL version with fraction seconds
const Version fsp_version_split_mysql(5, 6, 0);

/*
  Cache that will automatically be written to a dedicated file on
  destruction.

  DESCRIPTION

 */
class Write_on_release_cache
{
public:
  enum flag
  {
    FLUSH_F
  };

  typedef unsigned short flag_set;

  /*
    Constructor.

    SYNOPSIS
      Write_on_release_cache
      cache  Pointer to cache to use
      file   File to write cache to upon destruction
      flags  Flags for the cache

    DESCRIPTION
      Cache common parameters and ensure common flush_data() code
      on successful copy of the cache, the cache will be reinited as a
      WRITE_CACHE.

      Currently, a pointer to the cache is provided in the
      constructor, but it would be possible to create a subclass
      holding the IO_CACHE itself.
   */
  Write_on_release_cache(IO_CACHE *cache, FILE *file, flag_set flags= 0, Log_event *ev= NULL)
    : m_cache(cache), m_file(file), m_flags(flags), m_ev(ev)
  {
    reinit_io_cache(m_cache, WRITE_CACHE, 0L, FALSE, TRUE);
  }

  ~Write_on_release_cache() {}

  bool flush_data()
  {
#ifdef MYSQL_CLIENT
    if (m_ev == NULL)
    {
      if (copy_event_cache_to_file_and_reinit(m_cache, m_file))
        return 1;
      if ((m_flags & FLUSH_F) && fflush(m_file))
        return 1;
    }
    else // if m_ev<>NULL, then storing the output in output_buf
    {
      LEX_STRING tmp_str;
      bool res;
      if (copy_event_cache_to_string_and_reinit(m_cache, &tmp_str))
        return 1;
      /* use 2 argument append as tmp_str is not \0 terminated */
      res= m_ev->output_buf.append(tmp_str.str, tmp_str.length);
      my_free(tmp_str.str);
      return res ? res : 0;
    }
#else /* MySQL_SERVER */
    if (copy_event_cache_to_file_and_reinit(m_cache, m_file))
      return 1;
    if ((m_flags & FLUSH_F) && fflush(m_file))
      return 1;
#endif
    return 0;
  }

  /*
    Return a pointer to the internal IO_CACHE.

    SYNOPSIS
      operator&()

    DESCRIPTION

      Function to return a pointer to the internal cache, so that the
      object can be treated as a IO_CACHE and used with the my_b_*
      IO_CACHE functions

    RETURN VALUE
      A pointer to the internal IO_CACHE.
   */
  IO_CACHE *operator&()
  {
    return m_cache;
  }

private:
  // Hidden, to prevent usage.
  Write_on_release_cache(Write_on_release_cache const&);

  IO_CACHE *m_cache;
  FILE *m_file;
  flag_set m_flags;
  Log_event *m_ev; // Used for Flashback
};

#ifndef DBUG_OFF
#define DBUG_DUMP_EVENT_BUF(B,L)                                         \
  do {                                                                   \
    const uchar *_buf=(uchar*)(B);                                       \
    size_t _len=(L);                                                     \
    if (_len >= LOG_EVENT_MINIMAL_HEADER_LEN)                            \
    {                                                                    \
      DBUG_PRINT("data", ("header: timestamp:%u type:%u server_id:%u len:%u log_pos:%u flags:%u",  \
                          uint4korr(_buf), _buf[EVENT_TYPE_OFFSET],      \
                          uint4korr(_buf+SERVER_ID_OFFSET),              \
                          uint4korr(_buf+EVENT_LEN_OFFSET),              \
                          uint4korr(_buf+LOG_POS_OFFSET),                \
                          uint4korr(_buf+FLAGS_OFFSET)));                \
      DBUG_DUMP("data", _buf+LOG_EVENT_MINIMAL_HEADER_LEN,               \
                _len-LOG_EVENT_MINIMAL_HEADER_LEN);                      \
    }                                                                    \
    else                                                                 \
      DBUG_DUMP("data", _buf, _len);                                     \
  } while(0)
#else
#define DBUG_DUMP_EVENT_BUF(B,L) do { } while(0)
#endif

/*
  read_str()
*/

static inline bool read_str(const uchar **buf, const uchar *buf_end,
                            const char **str, uint8 *len)
{
  if (*buf + ((uint) **buf) >= buf_end)
    return 1;
  *len= (uint8) **buf;
  *str= (char*) (*buf)+1;
  (*buf)+= (uint) *len+1;
  return 0;
}


/**
  Transforms a string into "" or its expression in X'HHHH' form.
*/

char *str_to_hex(char *to, const char *from, size_t len)
{
  if (len)
  {
    *to++= 'X';
    *to++= '\'';
    to= octet2hex(to, from, len);
    *to++= '\'';
    *to= '\0';
  }
  else
    to= strmov(to, "\"\"");
  return to;                               // pointer to end 0 of 'to'
}

#define BINLOG_COMPRESSED_HEADER_LEN 1
#define BINLOG_COMPRESSED_ORIGINAL_LENGTH_MAX_BYTES 4
/**
  Compressed Record
    Record Header: 1 Byte
             7 Bit: Always 1, mean compressed;
           4-6 Bit: Compressed algorithm - Always 0, means zlib
                    It maybe support other compression algorithm in the future.
           0-3 Bit: Bytes of "Record Original Length"
    Record Original Length: 1-4 Bytes
    Compressed Buf:
*/

/**
  Get the length of compress content.
*/

uint32 binlog_get_compress_len(uint32 len)
{
    /* 5 for the begin content, 1 reserved for a '\0'*/
    return ALIGN_SIZE((BINLOG_COMPRESSED_HEADER_LEN + BINLOG_COMPRESSED_ORIGINAL_LENGTH_MAX_BYTES) 
                        + compressBound(len) + 1);
}

/**
   Compress buf from 'src' to 'dst'.

   Note: 1) Then the caller should guarantee the length of 'dst', which
      can be got by binlog_get_uncompress_len, is enough to hold
      the content uncompressed.
         2) The 'comlen' should stored the length of 'dst', and it will
      be set as the size of compressed content after return.

   return zero if successful, others otherwise.
*/
int binlog_buf_compress(const uchar *src, uchar *dst, uint32 len, uint32 *comlen)
{
  uchar lenlen;
  if (len & 0xFF000000)
  {
    dst[1]= uchar(len >> 24);
    dst[2]= uchar(len >> 16);
    dst[3]= uchar(len >> 8);
    dst[4]= uchar(len);
    lenlen= 4;
  }
  else if (len & 0x00FF0000)
  {
    dst[1]= uchar(len >> 16);
    dst[2]= uchar(len >> 8);
    dst[3]= uchar(len);
    lenlen= 3;
  }
  else if (len & 0x0000FF00)
  {
    dst[1]= uchar(len >> 8);
    dst[2]= uchar(len);
    lenlen= 2;
  }
  else 
  {
    dst[1]= uchar(len);
    lenlen= 1;
  }
  dst[0]= 0x80 | (lenlen & 0x07);

  uLongf tmplen= (uLongf)*comlen - BINLOG_COMPRESSED_HEADER_LEN - lenlen - 1;
  if (compress((Bytef *)dst + BINLOG_COMPRESSED_HEADER_LEN + lenlen, &tmplen,
               (const Bytef *)src, (uLongf)len) != Z_OK)
  {
    return 1;
  }
  *comlen= (uint32)tmplen + BINLOG_COMPRESSED_HEADER_LEN + lenlen;
  return 0;
}

/**
   Convert a query_compressed_log_event to query_log_event
   from 'src' to 'dst', the size after compression stored in 'newlen'.

   @Note:
      1) The caller should call my_free to release 'dst' if *is_malloc is
         returned as true.
      2) If *is_malloc is retuened as false, then 'dst' reuses the passed-in
         'buf'.

   return zero if successful, non-zero otherwise.
*/

int
query_event_uncompress(const Format_description_log_event *description_event,
                       bool contain_checksum, const uchar *src, ulong src_len,
                       uchar* buf, ulong buf_size, bool* is_malloc, uchar **dst,
                       ulong *newlen)
{
  ulong len= uint4korr(src + EVENT_LEN_OFFSET);
  const uchar *tmp= src;
  const uchar *end= src + len;
  uchar *new_dst;

  // bad event
  if (src_len < len)
    return 1;

  DBUG_ASSERT((uchar)src[EVENT_TYPE_OFFSET] == QUERY_COMPRESSED_EVENT);

  uint8 common_header_len= description_event->common_header_len;
  uint8 post_header_len=
    description_event->post_header_len[QUERY_COMPRESSED_EVENT-1];

  *is_malloc= false;

  tmp+= common_header_len;
  // bad event
  if (end <= tmp)
    return 1;

  uint db_len= (uint)tmp[Q_DB_LEN_OFFSET];
  uint16 status_vars_len= uint2korr(tmp + Q_STATUS_VARS_LEN_OFFSET);

  tmp+= post_header_len + status_vars_len + db_len + 1;
  // bad event
  if (end <= tmp)
    return 1;

  int32 comp_len= (int32)(len - (tmp - src) -
                          (contain_checksum ? BINLOG_CHECKSUM_LEN : 0));
  uint32 un_len=  binlog_get_uncompress_len(tmp);

  // bad event 
  if (comp_len < 0 || un_len == 0)
    return 1;

  *newlen= (ulong)(tmp - src) + un_len;
  if (contain_checksum)
    *newlen+= BINLOG_CHECKSUM_LEN;
  
  uint32 alloc_size= (uint32)ALIGN_SIZE(*newlen);
  
  if (alloc_size <= buf_size) 
    new_dst= buf;
  else 
  {
    new_dst= (uchar *) my_malloc(PSI_INSTRUMENT_ME, alloc_size, MYF(MY_WME));
    if (!new_dst)
      return 1;
    *is_malloc= true;
  }

  /* copy the head*/
  memcpy(new_dst, src , tmp - src);
  if (binlog_buf_uncompress(tmp, new_dst + (tmp - src), comp_len, &un_len))
  {
    if (*is_malloc)
    {
      *is_malloc= false;
      my_free(new_dst);
    }
    return 1;
  }

  new_dst[EVENT_TYPE_OFFSET]= QUERY_EVENT;
  int4store(new_dst + EVENT_LEN_OFFSET, *newlen);
  if (contain_checksum)
  {
    ulong clear_len= *newlen - BINLOG_CHECKSUM_LEN;
    int4store(new_dst + clear_len,
              my_checksum(0L, (uchar *)new_dst, clear_len));
  }
  *dst= new_dst;
  return 0;
}

int
row_log_event_uncompress(const Format_description_log_event *description_event,
                         bool contain_checksum, const uchar *src, ulong src_len,
                         uchar* buf, ulong buf_size, bool* is_malloc,
                         uchar **dst, ulong *newlen)
{
  Log_event_type type= (Log_event_type)(uchar)src[EVENT_TYPE_OFFSET];
  ulong len= uint4korr(src + EVENT_LEN_OFFSET);
  const uchar *tmp= src;
  uchar *new_dst= NULL;
  const uchar *end= tmp + len;

  if (src_len < len)
    return 1;                                   // bad event

  DBUG_ASSERT(LOG_EVENT_IS_ROW_COMPRESSED(type));

  uint8 common_header_len= description_event->common_header_len;
  uint8 post_header_len= description_event->post_header_len[type-1];

  tmp+= common_header_len + ROWS_HEADER_LEN_V1;
  if (post_header_len == ROWS_HEADER_LEN_V2)
  {
    /*
      Have variable length header, check length,
      which includes length bytes
    */

    if (end - tmp <= 2)
      return 1;                                 // bad event

    uint16 var_header_len= uint2korr(tmp);
    DBUG_ASSERT(var_header_len >= 2);

    /* skip over var-len header, extracting 'chunks' */
    tmp+= var_header_len;

    /* get the uncompressed event type */
    type=
      (Log_event_type)(type - WRITE_ROWS_COMPRESSED_EVENT + WRITE_ROWS_EVENT);
  }
  else 
  {
    /* get the uncompressed event type */
    type= (Log_event_type)
      (type - WRITE_ROWS_COMPRESSED_EVENT_V1 + WRITE_ROWS_EVENT_V1);
  }

  if (end <= tmp)
    return 1;                                   //bad event

  ulong m_width= net_field_length((uchar **)&tmp);
  tmp+= (m_width + 7) / 8;

  if (type == UPDATE_ROWS_EVENT_V1 || type == UPDATE_ROWS_EVENT)
  {
    tmp+= (m_width + 7) / 8;
  }

  if (end <= tmp)
    return 1;                                   //bad event

  uint32 un_len= binlog_get_uncompress_len(tmp);
  if (un_len == 0)
    return 1;                                   //bad event

  int32 comp_len= (int32)(len - (tmp - src) -
                          (contain_checksum ? BINLOG_CHECKSUM_LEN : 0));
  if (comp_len <=0)
    return 1;                                   //bad event

  *newlen= ulong(tmp - src) + un_len;
  if (contain_checksum)
    *newlen+= BINLOG_CHECKSUM_LEN;

  size_t alloc_size= ALIGN_SIZE(*newlen);
  
  *is_malloc= false;
  if (alloc_size <= buf_size) 
  {
    new_dst= buf;
  }
  else
  {
    new_dst= (uchar*) my_malloc(PSI_INSTRUMENT_ME, alloc_size, MYF(MY_WME));
    if (!new_dst)
      return 1;
    *is_malloc= true;
  }

  /* Copy the head. */
  memcpy(new_dst, src , tmp - src);
  /* Uncompress the body. */
  if (binlog_buf_uncompress(tmp, new_dst + (tmp - src),
                            comp_len, &un_len))
  {
    if (*is_malloc)
      my_free(new_dst);
    return 1;
  }

  new_dst[EVENT_TYPE_OFFSET]= type;
  int4store(new_dst + EVENT_LEN_OFFSET, *newlen);
  if (contain_checksum)
  {
    ulong clear_len= *newlen - BINLOG_CHECKSUM_LEN;
    int4store(new_dst + clear_len,
              my_checksum(0L, (uchar *)new_dst, clear_len));
  }
  *dst= new_dst;
  return 0;
}

/**
  Get the length of uncompress content.
  return 0 means error.
*/

uint32 binlog_get_uncompress_len(const uchar *buf)
{
  uint32 len, lenlen;

  if ((buf == NULL) || ((buf[0] & 0xe0) != 0x80))
    return 0;

  lenlen= buf[0] & 0x07;

  buf++;
  /* Length is stored in high byte first order, like myisam keys */
  switch(lenlen) {
  case 1:
    len= buf[0];
    break;
  case 2:
    len= mi_uint2korr(buf);
    break;
  case 3:
    len= mi_uint3korr(buf);
    break;
  case 4:
    len= mi_uint4korr(buf);
    break;
  default:
    DBUG_ASSERT(lenlen >= 1 && lenlen <= 4);
    len= 0;
    break;
  }
  return len;
}

/**
   Uncompress the content in 'src' with length of 'len' to 'dst'.

   Note: 1) Then the caller should guarantee the length of 'dst' (which
      can be got by statement_get_uncompress_len) is enough to hold
      the content uncompressed.
         2) The 'newlen' should stored the length of 'dst', and it will
      be set as the size of uncompressed content after return.

   return zero if successful, others otherwise.
*/
int binlog_buf_uncompress(const uchar *src, uchar *dst, uint32 len,
                          uint32 *newlen)
{
  if ((src[0] & 0x80) == 0)
    return 1;

  uint32 lenlen= src[0] & 0x07;
  uLongf buflen= *newlen;                       // zlib type

  uint32 alg= (src[0] & 0x70) >> 4;
  switch(alg) {
  case 0:
    // zlib
    if (uncompress((Bytef *)dst, &buflen,
      (const Bytef*)src + 1 + lenlen, len - 1 - lenlen) != Z_OK)
      return 1;
    break;
  default:
    //TODO
    //bad algorithm
    return 1;
  }

  DBUG_ASSERT(*newlen == (uint32)buflen);
  *newlen= (uint32)buflen;
  return 0;
}


/**************************************************************************
	Log_event methods (= the parent class of all events)
**************************************************************************/

/**
  @return
  returns the human readable name of the event's type
*/

const char* Log_event::get_type_str(Log_event_type type)
{
  switch(type) {
  case START_EVENT_V3:  return "Start_v3";
  case STOP_EVENT:   return "Stop";
  case QUERY_EVENT:  return "Query";
  case ROTATE_EVENT: return "Rotate";
  case INTVAR_EVENT: return "Intvar";
  case LOAD_EVENT:   return "Load";
  case NEW_LOAD_EVENT:   return "New_load";
  case SLAVE_EVENT:  return "Slave";
  case CREATE_FILE_EVENT: return "Create_file";
  case APPEND_BLOCK_EVENT: return "Append_block";
  case DELETE_FILE_EVENT: return "Delete_file";
  case EXEC_LOAD_EVENT: return "Exec_load";
  case RAND_EVENT: return "RAND";
  case XID_EVENT: return "Xid";
  case USER_VAR_EVENT: return "User var";
  case FORMAT_DESCRIPTION_EVENT: return "Format_desc";
  case TABLE_MAP_EVENT: return "Table_map";
  case PRE_GA_WRITE_ROWS_EVENT: return "Write_rows_event_old";
  case PRE_GA_UPDATE_ROWS_EVENT: return "Update_rows_event_old";
  case PRE_GA_DELETE_ROWS_EVENT: return "Delete_rows_event_old";
  case WRITE_ROWS_EVENT_V1: return "Write_rows_v1";
  case UPDATE_ROWS_EVENT_V1: return "Update_rows_v1";
  case DELETE_ROWS_EVENT_V1: return "Delete_rows_v1";
  case WRITE_ROWS_EVENT: return "Write_rows";
  case UPDATE_ROWS_EVENT: return "Update_rows";
  case DELETE_ROWS_EVENT: return "Delete_rows";
  case BEGIN_LOAD_QUERY_EVENT: return "Begin_load_query";
  case EXECUTE_LOAD_QUERY_EVENT: return "Execute_load_query";
  case INCIDENT_EVENT: return "Incident";
  case ANNOTATE_ROWS_EVENT: return "Annotate_rows";
  case BINLOG_CHECKPOINT_EVENT: return "Binlog_checkpoint";
  case GTID_EVENT: return "Gtid";
  case GTID_LIST_EVENT: return "Gtid_list";
  case START_ENCRYPTION_EVENT: return "Start_encryption";

  /* The following is only for mysqlbinlog */
  case IGNORABLE_LOG_EVENT: return "Ignorable log event";
  case ROWS_QUERY_LOG_EVENT: return "MySQL Rows_query";
  case GTID_LOG_EVENT: return "MySQL Gtid";
  case ANONYMOUS_GTID_LOG_EVENT: return "MySQL Anonymous_Gtid";
  case PREVIOUS_GTIDS_LOG_EVENT: return "MySQL Previous_gtids";
  case HEARTBEAT_LOG_EVENT: return "Heartbeat";
  case TRANSACTION_CONTEXT_EVENT: return "Transaction_context";
  case VIEW_CHANGE_EVENT: return "View_change";
  case XA_PREPARE_LOG_EVENT: return "XA_prepare";
  case QUERY_COMPRESSED_EVENT: return "Query_compressed";
  case WRITE_ROWS_COMPRESSED_EVENT: return "Write_rows_compressed";
  case UPDATE_ROWS_COMPRESSED_EVENT: return "Update_rows_compressed";
  case DELETE_ROWS_COMPRESSED_EVENT: return "Delete_rows_compressed";
  case WRITE_ROWS_COMPRESSED_EVENT_V1: return "Write_rows_compressed_v1";
  case UPDATE_ROWS_COMPRESSED_EVENT_V1: return "Update_rows_compressed_v1";
  case DELETE_ROWS_COMPRESSED_EVENT_V1: return "Delete_rows_compressed_v1";

  default: return "Unknown";				/* impossible */
  }
}

const char* Log_event::get_type_str()
{
  return get_type_str(get_type_code());
}


/*
  Log_event::Log_event()
*/

Log_event::Log_event(const uchar *buf,
                     const Format_description_log_event* description_event)
  :temp_buf(0), exec_time(0), cache_type(Log_event::EVENT_INVALID_CACHE),
    checksum_alg(BINLOG_CHECKSUM_ALG_UNDEF)
{
#ifndef MYSQL_CLIENT
  thd= 0;
#endif
  when= uint4korr(buf);
  when_sec_part= ~0UL;
  server_id= uint4korr(buf + SERVER_ID_OFFSET);
  data_written= uint4korr(buf + EVENT_LEN_OFFSET);
  if (description_event->binlog_version==1)
  {
    log_pos= 0;
    flags= 0;
    return;
  }
  /* 4.0 or newer */
  log_pos= uint4korr(buf + LOG_POS_OFFSET);
  /*
    If the log is 4.0 (so here it can only be a 4.0 relay log read by
    the SQL thread or a 4.0 master binlog read by the I/O thread),
    log_pos is the beginning of the event: we transform it into the end
    of the event, which is more useful.
    But how do you know that the log is 4.0: you know it if
    description_event is version 3 *and* you are not reading a
    Format_desc (remember that mysqlbinlog starts by assuming that 5.0
    logs are in 4.0 format, until it finds a Format_desc).
  */
  if (description_event->binlog_version==3 &&
      (uchar)buf[EVENT_TYPE_OFFSET]<FORMAT_DESCRIPTION_EVENT && log_pos)
  {
      /*
        If log_pos=0, don't change it. log_pos==0 is a marker to mean
        "don't change rli->group_master_log_pos" (see
        inc_group_relay_log_pos()). As it is unreal log_pos, adding the
        event len's is nonsense. For example, a fake Rotate event should
        not have its log_pos (which is 0) changed or it will modify
        Exec_master_log_pos in SHOW SLAVE STATUS, displaying a nonsense
        value of (a non-zero offset which does not exist in the master's
        binlog, so which will cause problems if the user uses this value
        in CHANGE MASTER).
      */
    log_pos+= data_written; /* purecov: inspected */
  }
  DBUG_PRINT("info", ("log_pos: %llu", log_pos));

  flags= uint2korr(buf + FLAGS_OFFSET);
  if (((uchar)buf[EVENT_TYPE_OFFSET] == FORMAT_DESCRIPTION_EVENT) ||
      ((uchar)buf[EVENT_TYPE_OFFSET] == ROTATE_EVENT))
  {
    /*
      These events always have a header which stops here (i.e. their
      header is FROZEN).
    */
    /*
      Initialization to zero of all other Log_event members as they're
      not specified. Currently there are no such members; in the future
      there will be an event UID (but Format_description and Rotate
      don't need this UID, as they are not propagated through
      --log-slave-updates (remember the UID is used to not play a query
      twice when you have two masters which are slaves of a 3rd master).
      Then we are done.
    */
    return;
  }
  /* otherwise, go on with reading the header from buf (nothing now) */
}


/**
  This needn't be format-tolerant, because we only parse the first
  LOG_EVENT_MINIMAL_HEADER_LEN bytes (just need the event's length).
*/

int Log_event::read_log_event(IO_CACHE* file, String* packet,
                              const Format_description_log_event *fdle,
                              enum enum_binlog_checksum_alg checksum_alg_arg)
{
  ulong data_len;
  char buf[LOG_EVENT_MINIMAL_HEADER_LEN];
  uchar ev_offset= packet->length();
#if !defined(MYSQL_CLIENT)
  THD *thd=current_thd;
  ulong max_allowed_packet= thd ? thd->slave_thread ? slave_max_allowed_packet
                                                    : thd->variables.max_allowed_packet
                                : ~(uint)0;
#endif
  DBUG_ENTER("Log_event::read_log_event(IO_CACHE*,String*...)");

  if (my_b_read(file, (uchar*) buf, sizeof(buf)))
  {
    /*
      If the read hits eof, we must report it as eof so the caller
      will know it can go into cond_wait to be woken up on the next
      update to the log.
    */
    DBUG_PRINT("error",("file->error: %d", file->error));
    DBUG_RETURN(file->error == 0 ? LOG_READ_EOF :
                file->error > 0 ? LOG_READ_TRUNC : LOG_READ_IO);
  }
  data_len= uint4korr(buf + EVENT_LEN_OFFSET);

  /* Append the log event header to packet */
  if (packet->append(buf, sizeof(buf)))
    DBUG_RETURN(LOG_READ_MEM);

  if (data_len < LOG_EVENT_MINIMAL_HEADER_LEN)
    DBUG_RETURN(LOG_READ_BOGUS);

  if (data_len > MY_MAX(max_allowed_packet,
                        opt_binlog_rows_event_max_size + MAX_LOG_EVENT_HEADER))
    DBUG_RETURN(LOG_READ_TOO_LARGE);

  if (likely(data_len > LOG_EVENT_MINIMAL_HEADER_LEN))
  {
    /* Append rest of event, read directly from file into packet */
    if (packet->append(file, data_len - LOG_EVENT_MINIMAL_HEADER_LEN))
    {
      /*
        Fatal error occurred when appending rest of the event
        to packet, possible failures:
	1. EOF occurred when reading from file, it's really an error
           as there's supposed to be more bytes available.
           file->error will have been set to number of bytes left to read
        2. Read was interrupted, file->error would normally be set to -1
        3. Failed to allocate memory for packet, my_errno
           will be ENOMEM(file->error should be 0, but since the
           memory allocation occurs before the call to read it might
           be uninitialized)
      */
      DBUG_RETURN(my_errno == ENOMEM ? LOG_READ_MEM :
                  (file->error >= 0 ? LOG_READ_TRUNC: LOG_READ_IO));
    }
  }

  if (fdle->crypto_data.scheme)
  {
    uchar iv[BINLOG_IV_LENGTH];
    fdle->crypto_data.set_iv(iv, (uint32) (my_b_tell(file) - data_len));
    size_t sz= data_len + ev_offset + 1;
#ifdef HAVE_WOLFSSL
    /*
      Workaround for MDEV-19582.
      WolfSSL reads memory out of bounds with decryption/NOPAD)
      We allocate a little more memory therefore.
    */
    sz+= MY_AES_BLOCK_SIZE;
#endif
    char *newpkt= (char*)my_malloc(PSI_INSTRUMENT_ME, sz, MYF(MY_WME));
    if (!newpkt)
      DBUG_RETURN(LOG_READ_MEM);
    memcpy(newpkt, packet->ptr(), ev_offset);

    uint dstlen;
    uchar *src= (uchar*)packet->ptr() + ev_offset;
    uchar *dst= (uchar*)newpkt + ev_offset;
    memcpy(src + EVENT_LEN_OFFSET, src, 4);
    if (encryption_crypt(src + 4, data_len - 4, dst + 4, &dstlen,
            fdle->crypto_data.key, fdle->crypto_data.key_length, iv,
            sizeof(iv), ENCRYPTION_FLAG_DECRYPT | ENCRYPTION_FLAG_NOPAD,
            ENCRYPTION_KEY_SYSTEM_DATA, fdle->crypto_data.key_version))
    {
      my_free(newpkt);
      DBUG_RETURN(LOG_READ_DECRYPT);
    }
    DBUG_ASSERT(dstlen == data_len - 4);
    memcpy(dst, dst + EVENT_LEN_OFFSET, 4);
    int4store(dst + EVENT_LEN_OFFSET, data_len);
    packet->reset(newpkt, data_len + ev_offset, data_len + ev_offset + 1,
                  &my_charset_bin);
  }

  /*
    CRC verification of the Dump thread
  */
  if (data_len > LOG_EVENT_MINIMAL_HEADER_LEN)
  {
    /* Corrupt the event for Dump thread*/
    DBUG_EXECUTE_IF("corrupt_read_log_event2",
      uchar *debug_event_buf_c= (uchar*) packet->ptr() + ev_offset;
      if (debug_event_buf_c[EVENT_TYPE_OFFSET] != FORMAT_DESCRIPTION_EVENT)
      {
        int debug_cor_pos= rand() % (data_len - BINLOG_CHECKSUM_LEN);
        debug_event_buf_c[debug_cor_pos] =~ debug_event_buf_c[debug_cor_pos];
        DBUG_PRINT("info", ("Corrupt the event at Log_event::read_log_event: byte on position %d", debug_cor_pos));
        DBUG_SET("-d,corrupt_read_log_event2");
      }
    );
    if (event_checksum_test((uchar*) packet->ptr() + ev_offset,
                             data_len, checksum_alg_arg))
      DBUG_RETURN(LOG_READ_CHECKSUM_FAILURE);
  }
  DBUG_RETURN(0);
}

Log_event* Log_event::read_log_event(IO_CACHE* file,
                                     const Format_description_log_event *fdle,
                                     my_bool crc_check)
{
  DBUG_ENTER("Log_event::read_log_event(IO_CACHE*,Format_description_log_event*...)");
  DBUG_ASSERT(fdle != 0);
  String event;
  const char *error= 0;
  Log_event *res= 0;

  switch (read_log_event(file, &event, fdle, BINLOG_CHECKSUM_ALG_OFF))
  {
    case 0:
      break;
    case LOG_READ_EOF: // no error here; we are at the file's end
      goto err;
    case LOG_READ_BOGUS:
      error= "Event invalid";
      goto err;
    case LOG_READ_IO:
      error= "read error";
      goto err;
    case LOG_READ_MEM:
      error= "Out of memory";
      goto err;
    case LOG_READ_TRUNC:
      error= "Event truncated";
      goto err;
    case LOG_READ_TOO_LARGE:
      error= "Event too big";
      goto err;
    case LOG_READ_DECRYPT:
      error= "Event decryption failure";
      goto err;
    case LOG_READ_CHECKSUM_FAILURE:
    default:
      DBUG_ASSERT(0);
      error= "internal error";
      goto err;
  }

  if ((res= read_log_event((uchar*) event.ptr(), event.length(),
                           &error, fdle, crc_check)))
    res->register_temp_buf((uchar*) event.release(), true);

err:
  if (unlikely(error))
  {
    DBUG_ASSERT(!res);
#ifdef MYSQL_CLIENT
    if (force_opt)
      DBUG_RETURN(new Unknown_log_event());
#endif
    if (event.length() >= OLD_HEADER_LEN)
      sql_print_error("Error in Log_event::read_log_event(): '%s',"
                      " data_len: %lu, event_type: %u", error,
                      (ulong) uint4korr(&event[EVENT_LEN_OFFSET]),
                      (uint) (uchar)event[EVENT_TYPE_OFFSET]);
    else
      sql_print_error("Error in Log_event::read_log_event(): '%s'", error);
    /*
      The SQL slave thread will check if file->error<0 to know
      if there was an I/O error. Even if there is no "low-level" I/O errors
      with 'file', any of the high-level above errors is worrying
      enough to stop the SQL thread now ; as we are skipping the current event,
      going on with reading and successfully executing other events can
      only corrupt the slave's databases. So stop.
    */
    file->error= -1;
  }
  DBUG_RETURN(res);
}


/**
  Binlog format tolerance is in (buf, event_len, fdle)
  constructors.
*/

Log_event* Log_event::read_log_event(const uchar *buf, uint event_len,
                                     const char **error,
                                     const Format_description_log_event *fdle,
                                     my_bool crc_check)
{
  Log_event* ev;
  enum enum_binlog_checksum_alg alg;
  DBUG_ENTER("Log_event::read_log_event(char*,...)");
  DBUG_ASSERT(fdle != 0);
  DBUG_PRINT("info", ("binlog_version: %d", fdle->binlog_version));
  DBUG_DUMP_EVENT_BUF(buf, event_len);

  /*
    Check the integrity; This is needed because handle_slave_io() doesn't
    check if packet is of proper length.
 */
  if (event_len < EVENT_LEN_OFFSET)
  {
    *error="Sanity check failed";		// Needed to free buffer
    DBUG_RETURN(NULL); // general sanity check - will fail on a partial read
  }

  uint event_type= buf[EVENT_TYPE_OFFSET];
  // all following START events in the current file are without checksum
  if (event_type == START_EVENT_V3)
    (const_cast< Format_description_log_event *>(fdle))->checksum_alg= BINLOG_CHECKSUM_ALG_OFF;
  /*
    CRC verification by SQL and Show-Binlog-Events master side.
    The caller has to provide @fdle->checksum_alg to
    be the last seen FD's (A) descriptor.
    If event is FD the descriptor is in it.
    Notice, FD of the binlog can be only in one instance and therefore
    Show-Binlog-Events executing master side thread needs just to know
    the only FD's (A) value -  whereas RL can contain more.
    In the RL case, the alg is kept in FD_e (@fdle) which is reset
    to the newer read-out event after its execution with possibly new alg descriptor.
    Therefore in a typical sequence of RL:
    {FD_s^0, FD_m, E_m^1} E_m^1 
    will be verified with (A) of FD_m.

    See legends definition on MYSQL_BIN_LOG::relay_log_checksum_alg docs
    lines (log.h).

    Notice, a pre-checksum FD version forces alg := BINLOG_CHECKSUM_ALG_UNDEF.
  */
  alg= (event_type != FORMAT_DESCRIPTION_EVENT) ?
    fdle->checksum_alg : get_checksum_alg(buf, event_len);
  // Emulate the corruption during reading an event
  DBUG_EXECUTE_IF("corrupt_read_log_event_char",
    if (event_type != FORMAT_DESCRIPTION_EVENT)
    {
      uchar *debug_event_buf_c= const_cast<uchar*>(buf);
      int debug_cor_pos= rand() % (event_len - BINLOG_CHECKSUM_LEN);
      debug_event_buf_c[debug_cor_pos]=~ debug_event_buf_c[debug_cor_pos];
      DBUG_PRINT("info", ("Corrupt the event at Log_event::read_log_event(char*,...): byte on position %d", debug_cor_pos));
      DBUG_SET("-d,corrupt_read_log_event_char");
    }
  );                                                 
  if (crc_check && event_checksum_test(const_cast<uchar*>(buf), event_len, alg))
  {
#ifdef MYSQL_CLIENT
    *error= "Event crc check failed! Most likely there is event corruption.";
    if (force_opt)
    {
      ev= new Unknown_log_event(buf, fdle);
      DBUG_RETURN(ev);
    }
    else
      DBUG_RETURN(NULL);
#else
    *error= ER_THD_OR_DEFAULT(current_thd, ER_BINLOG_READ_EVENT_CHECKSUM_FAILURE);
    sql_print_error("%s", *error);
    DBUG_RETURN(NULL);
#endif
  }

  if (event_type > fdle->number_of_event_types &&
      event_type != FORMAT_DESCRIPTION_EVENT)
  {
    /*
      It is unsafe to use the fdle if its post_header_len
      array does not include the event type.
    */
    DBUG_PRINT("error", ("event type %d found, but the current "
                         "Format_description_log_event supports only %d event "
                         "types", event_type,
                         fdle->number_of_event_types));
    ev= NULL;
  }
  else
  {
    /*
      In some previuos versions (see comment in
      Format_description_log_event::Format_description_log_event(char*,...)),
      event types were assigned different id numbers than in the
      present version. In order to replicate from such versions to the
      present version, we must map those event type id's to our event
      type id's.  The mapping is done with the event_type_permutation
      array, which was set up when the Format_description_log_event
      was read.
    */
    if (fdle->event_type_permutation)
    {
      int new_event_type= fdle->event_type_permutation[event_type];
      DBUG_PRINT("info", ("converting event type %d to %d (%s)",
                   event_type, new_event_type,
                   get_type_str((Log_event_type)new_event_type)));
      event_type= new_event_type;
    }

    if (alg != BINLOG_CHECKSUM_ALG_UNDEF &&
        (event_type == FORMAT_DESCRIPTION_EVENT ||
         alg != BINLOG_CHECKSUM_ALG_OFF))
      event_len= event_len - BINLOG_CHECKSUM_LEN;

    /*
      Create an object of Ignorable_log_event for unrecognized sub-class.
      So that SLAVE SQL THREAD will only update the position and continue.
      We should look for this flag first instead of judging by event_type
      Any event can be Ignorable_log_event if it has this flag on.
      look into @note of Ignorable_log_event
    */
    if (uint2korr(buf + FLAGS_OFFSET) & LOG_EVENT_IGNORABLE_F)
    {
      ev= new Ignorable_log_event(buf, fdle,
                                  get_type_str((Log_event_type) event_type));
      goto exit;
    }
    switch(event_type) {
    case QUERY_EVENT:
      ev= new Query_log_event(buf, event_len, fdle, QUERY_EVENT);
      break;
    case QUERY_COMPRESSED_EVENT:
      ev= new Query_compressed_log_event(buf, event_len, fdle,
                                          QUERY_COMPRESSED_EVENT);
      break;
    case LOAD_EVENT:
      ev= new Load_log_event(buf, event_len, fdle);
      break;
    case NEW_LOAD_EVENT:
      ev= new Load_log_event(buf, event_len, fdle);
      break;
    case ROTATE_EVENT:
      ev= new Rotate_log_event(buf, event_len, fdle);
      break;
    case BINLOG_CHECKPOINT_EVENT:
      ev= new Binlog_checkpoint_log_event(buf, event_len, fdle);
      break;
    case GTID_EVENT:
      ev= new Gtid_log_event(buf, event_len, fdle);
      break;
    case GTID_LIST_EVENT:
      ev= new Gtid_list_log_event(buf, event_len, fdle);
      break;
    case CREATE_FILE_EVENT:
      ev= new Create_file_log_event(buf, event_len, fdle);
      break;
    case APPEND_BLOCK_EVENT:
      ev= new Append_block_log_event(buf, event_len, fdle);
      break;
    case DELETE_FILE_EVENT:
      ev= new Delete_file_log_event(buf, event_len, fdle);
      break;
    case EXEC_LOAD_EVENT:
      ev= new Execute_load_log_event(buf, event_len, fdle);
      break;
    case START_EVENT_V3: /* this is sent only by MySQL <=4.x */
      ev= new Start_log_event_v3(buf, event_len, fdle);
      break;
    case STOP_EVENT:
      ev= new Stop_log_event(buf, fdle);
      break;
    case INTVAR_EVENT:
      ev= new Intvar_log_event(buf, fdle);
      break;
    case XID_EVENT:
      ev= new Xid_log_event(buf, fdle);
      break;
    case XA_PREPARE_LOG_EVENT:
      ev= new XA_prepare_log_event(buf, fdle);
      break;
    case RAND_EVENT:
      ev= new Rand_log_event(buf, fdle);
      break;
    case USER_VAR_EVENT:
      ev= new User_var_log_event(buf, event_len, fdle);
      break;
    case FORMAT_DESCRIPTION_EVENT:
      ev= new Format_description_log_event(buf, event_len, fdle);
      break;
#if defined(HAVE_REPLICATION) 
    case PRE_GA_WRITE_ROWS_EVENT:
      ev= new Write_rows_log_event_old(buf, event_len, fdle);
      break;
    case PRE_GA_UPDATE_ROWS_EVENT:
      ev= new Update_rows_log_event_old(buf, event_len, fdle);
      break;
    case PRE_GA_DELETE_ROWS_EVENT:
      ev= new Delete_rows_log_event_old(buf, event_len, fdle);
      break;
    case WRITE_ROWS_EVENT_V1:
    case WRITE_ROWS_EVENT:
      ev= new Write_rows_log_event(buf, event_len, fdle);
      break;
    case UPDATE_ROWS_EVENT_V1:
    case UPDATE_ROWS_EVENT:
      ev= new Update_rows_log_event(buf, event_len, fdle);
      break;
    case DELETE_ROWS_EVENT_V1:
    case DELETE_ROWS_EVENT:
      ev= new Delete_rows_log_event(buf, event_len, fdle);
      break;

    case WRITE_ROWS_COMPRESSED_EVENT:
    case WRITE_ROWS_COMPRESSED_EVENT_V1:
      ev= new Write_rows_compressed_log_event(buf, event_len, fdle);
      break;
    case UPDATE_ROWS_COMPRESSED_EVENT:
    case UPDATE_ROWS_COMPRESSED_EVENT_V1:
      ev= new Update_rows_compressed_log_event(buf, event_len, fdle);
      break;
    case DELETE_ROWS_COMPRESSED_EVENT:
    case DELETE_ROWS_COMPRESSED_EVENT_V1:
      ev= new Delete_rows_compressed_log_event(buf, event_len, fdle);
      break;

      /* MySQL GTID events are ignored */
    case GTID_LOG_EVENT:
    case ANONYMOUS_GTID_LOG_EVENT:
    case PREVIOUS_GTIDS_LOG_EVENT:
    case TRANSACTION_CONTEXT_EVENT:
    case VIEW_CHANGE_EVENT:
      ev= new Ignorable_log_event(buf, fdle,
                                  get_type_str((Log_event_type) event_type));
      break;

    case TABLE_MAP_EVENT:
      ev= new Table_map_log_event(buf, event_len, fdle);
      break;
#endif
    case BEGIN_LOAD_QUERY_EVENT:
      ev= new Begin_load_query_log_event(buf, event_len, fdle);
      break;
    case EXECUTE_LOAD_QUERY_EVENT:
      ev= new Execute_load_query_log_event(buf, event_len, fdle);
      break;
    case INCIDENT_EVENT:
      ev= new Incident_log_event(buf, event_len, fdle);
      break;
    case ANNOTATE_ROWS_EVENT:
      ev= new Annotate_rows_log_event(buf, event_len, fdle);
      break;
    case START_ENCRYPTION_EVENT:
      ev= new Start_encryption_log_event(buf, event_len, fdle);
      break;
    default:
      DBUG_PRINT("error",("Unknown event code: %d",
                          (uchar) buf[EVENT_TYPE_OFFSET]));
      ev= NULL;
      break;
    }
  }
exit:

  if (ev)
  {
    ev->checksum_alg= alg;
#ifdef MYSQL_CLIENT
    if (ev->checksum_alg != BINLOG_CHECKSUM_ALG_OFF &&
        ev->checksum_alg != BINLOG_CHECKSUM_ALG_UNDEF)
      ev->crc= uint4korr(buf + (event_len));
#endif
  }

  DBUG_PRINT("read_event", ("%s(type_code: %u; event_len: %u)",
                            ev ? ev->get_type_str() : "<unknown>",
                            (uchar)buf[EVENT_TYPE_OFFSET],
                            event_len));
  /*
    is_valid() are small event-specific sanity tests which are
    important; for example there are some my_malloc() in constructors
    (e.g. Query_log_event::Query_log_event(char*...)); when these
    my_malloc() fail we can't return an error out of the constructor
    (because constructor is "void") ; so instead we leave the pointer we
    wanted to allocate (e.g. 'query') to 0 and we test it in is_valid().
    Same for Format_description_log_event, member 'post_header_len'.

    SLAVE_EVENT is never used, so it should not be read ever.
  */
  if (!ev || !ev->is_valid() || (event_type == SLAVE_EVENT))
  {
    DBUG_PRINT("error",("Found invalid event in binary log"));

    delete ev;
#ifdef MYSQL_CLIENT
    if (!force_opt) /* then mysqlbinlog dies */
    {
      *error= "Found invalid event in binary log";
      DBUG_RETURN(0);
    }
    ev= new Unknown_log_event(buf, fdle);
#else
    *error= "Found invalid event in binary log";
    DBUG_RETURN(0);
#endif
  }
  DBUG_RETURN(ev);  
}



/* 2 utility functions for the next method */

/**
   Read a string with length from memory.

   This function reads the string-with-length stored at
   <code>src</code> and extract the length into <code>*len</code> and
   a pointer to the start of the string into <code>*dst</code>. The
   string can then be copied using <code>memcpy()</code> with the
   number of bytes given in <code>*len</code>.

   @param src Pointer to variable holding a pointer to the memory to
              read the string from.
   @param dst Pointer to variable holding a pointer where the actual
              string starts. Starting from this position, the string
              can be copied using @c memcpy().
   @param len Pointer to variable where the length will be stored.
   @param end One-past-the-end of the memory where the string is
              stored.

   @return    Zero if the entire string can be copied successfully,
              @c UINT_MAX if the length could not be read from memory
              (that is, if <code>*src >= end</code>), otherwise the
              number of bytes that are missing to read the full
              string, which happends <code>*dst + *len >= end</code>.
*/
static int
get_str_len_and_pointer(const Log_event::Byte **src,
                        const char **dst,
                        uint *len,
                        const Log_event::Byte *end)
{
  if (*src >= end)
    return -1;       // Will be UINT_MAX in two-complement arithmetics
  uint length= **src;
  if (length > 0)
  {
    if (*src + length >= end)
      return (int)(*src + length - end + 1);   // Number of bytes missing
    *dst= (char *)*src + 1;                    // Will be copied later
  }
  *len= length;
  *src+= length + 1;
  return 0;
}

static void copy_str_and_move(const char **src, Log_event::Byte **dst, 
                              size_t len)
{
  memcpy(*dst, *src, len);
  *src= (const char *)*dst;
  (*dst)+= len;
  *(*dst)++= 0;
}


#ifndef DBUG_OFF
static char const *
code_name(int code)
{
  static char buf[255];
  switch (code) {
  case Q_FLAGS2_CODE: return "Q_FLAGS2_CODE";
  case Q_SQL_MODE_CODE: return "Q_SQL_MODE_CODE";
  case Q_CATALOG_CODE: return "Q_CATALOG_CODE";
  case Q_AUTO_INCREMENT: return "Q_AUTO_INCREMENT";
  case Q_CHARSET_CODE: return "Q_CHARSET_CODE";
  case Q_TIME_ZONE_CODE: return "Q_TIME_ZONE_CODE";
  case Q_CATALOG_NZ_CODE: return "Q_CATALOG_NZ_CODE";
  case Q_LC_TIME_NAMES_CODE: return "Q_LC_TIME_NAMES_CODE";
  case Q_CHARSET_DATABASE_CODE: return "Q_CHARSET_DATABASE_CODE";
  case Q_TABLE_MAP_FOR_UPDATE_CODE: return "Q_TABLE_MAP_FOR_UPDATE_CODE";
  case Q_MASTER_DATA_WRITTEN_CODE: return "Q_MASTER_DATA_WRITTEN_CODE";
  case Q_HRNOW: return "Q_HRNOW";
  case Q_XID: return "XID";
  case Q_GTID_FLAGS3: return "Q_GTID_FLAGS3";
  }
  sprintf(buf, "CODE#%d", code);
  return buf;
}
#endif

#define VALIDATE_BYTES_READ(CUR_POS, START, EVENT_LEN)      \
  do {                                                      \
       uchar *cur_pos= (uchar *)CUR_POS;                    \
       uchar *start= (uchar *)START;                        \
       uint len= EVENT_LEN;                                 \
       uint bytes_read= (uint)(cur_pos - start);            \
       DBUG_PRINT("info", ("Bytes read: %u event_len:%u.\n",\
             bytes_read, len));                             \
       if (bytes_read >= len)                               \
         DBUG_VOID_RETURN;                                  \
  } while (0)

/**
   Macro to check that there is enough space to read from memory.

   @param PTR Pointer to memory
   @param END End of memory
   @param CNT Number of bytes that should be read.
 */
#define CHECK_SPACE(PTR,END,CNT)                      \
  do {                                                \
    DBUG_PRINT("info", ("Read %s", code_name(pos[-1]))); \
    if ((PTR) + (CNT) > (END)) {                      \
      DBUG_PRINT("info", ("query= 0"));               \
      query= 0;                                       \
      DBUG_VOID_RETURN;                               \
    }                                                 \
  } while (0)


/**
  This is used by the SQL slave thread to prepare the event before execution.
*/
Query_log_event::Query_log_event(const uchar *buf, uint event_len,
                                 const Format_description_log_event
                                 *description_event,
                                 Log_event_type event_type)
  :Log_event(buf, description_event), data_buf(0), query(NullS),
   db(NullS), catalog_len(0), status_vars_len(0),
   flags2_inited(0), sql_mode_inited(0), charset_inited(0), flags2(0),
   auto_increment_increment(1), auto_increment_offset(1),
   time_zone_len(0), lc_time_names_number(0), charset_database_number(0),
   table_map_for_update(0), xid(0), master_data_written(0), gtid_flags_extra(0),
   sa_seq_no(0)
{
  ulong data_len;
  uint32 tmp;
  uint8 common_header_len, post_header_len;
  Log_event::Byte *start;
  const Log_event::Byte *end;
  bool catalog_nz= 1;
  DBUG_ENTER("Query_log_event::Query_log_event(char*,...)");

  memset(&user, 0, sizeof(user));
  memset(&host, 0, sizeof(host));
  common_header_len= description_event->common_header_len;
  post_header_len= description_event->post_header_len[event_type-1];
  DBUG_PRINT("info",("event_len: %u  common_header_len: %d  post_header_len: %d",
                     event_len, common_header_len, post_header_len));

  /*
    We test if the event's length is sensible, and if so we compute data_len.
    We cannot rely on QUERY_HEADER_LEN here as it would not be format-tolerant.
    We use QUERY_HEADER_MINIMAL_LEN which is the same for 3.23, 4.0 & 5.0.
  */
  if (event_len < (uint)(common_header_len + post_header_len))
    DBUG_VOID_RETURN;
  data_len= event_len - (common_header_len + post_header_len);
  buf+= common_header_len;

  thread_id = slave_proxy_id = uint4korr(buf + Q_THREAD_ID_OFFSET);
  exec_time = uint4korr(buf + Q_EXEC_TIME_OFFSET);
  db_len = (uchar)buf[Q_DB_LEN_OFFSET]; // TODO: add a check of all *_len vars
  error_code = uint2korr(buf + Q_ERR_CODE_OFFSET);

  /*
    5.0 format starts here.
    Depending on the format, we may or not have affected/warnings etc
    The remnent post-header to be parsed has length:
  */
  tmp= post_header_len - QUERY_HEADER_MINIMAL_LEN;
  if (tmp)
  {
    status_vars_len= uint2korr(buf + Q_STATUS_VARS_LEN_OFFSET);
    /*
      Check if status variable length is corrupt and will lead to very
      wrong data. We could be even more strict and require data_len to
      be even bigger, but this will suffice to catch most corruption
      errors that can lead to a crash.
    */
    if (status_vars_len > MY_MIN(data_len, MAX_SIZE_LOG_EVENT_STATUS))
    {
      DBUG_PRINT("info", ("status_vars_len (%u) > data_len (%lu); query= 0",
                          status_vars_len, data_len));
      query= 0;
      DBUG_VOID_RETURN;
    }
    data_len-= status_vars_len;
    DBUG_PRINT("info", ("Query_log_event has status_vars_len: %u",
                        (uint) status_vars_len));
    tmp-= 2;
  } 
  else
  {
    /*
      server version < 5.0 / binlog_version < 4 master's event is 
      relay-logged with storing the original size of the event in
      Q_MASTER_DATA_WRITTEN_CODE status variable.
      The size is to be restored at reading Q_MASTER_DATA_WRITTEN_CODE-marked
      event from the relay log.
    */
    DBUG_ASSERT(description_event->binlog_version < 4);
    master_data_written= (uint32)data_written;
  }
  /*
    We have parsed everything we know in the post header for QUERY_EVENT,
    the rest of post header is either comes from older version MySQL or
    dedicated to derived events (e.g. Execute_load_query...)
  */

  /* variable-part: the status vars; only in MySQL 5.0  */
  
  start= (Log_event::Byte*) (buf+post_header_len);
  end= (const Log_event::Byte*) (start+status_vars_len);
  for (const Log_event::Byte* pos= start; pos < end;)
  {
    switch (*pos++) {
    case Q_FLAGS2_CODE:
      CHECK_SPACE(pos, end, 4);
      flags2_inited= 1;
      flags2= uint4korr(pos);
      DBUG_PRINT("info",("In Query_log_event, read flags2: %lu", (ulong) flags2));
      pos+= 4;
      break;
    case Q_SQL_MODE_CODE:
    {
      CHECK_SPACE(pos, end, 8);
      sql_mode_inited= 1;
      sql_mode= (sql_mode_t) uint8korr(pos);
      DBUG_PRINT("info",("In Query_log_event, read sql_mode: %llu", sql_mode));
      pos+= 8;
      break;
    }
    case Q_CATALOG_NZ_CODE:
      DBUG_PRINT("info", ("case Q_CATALOG_NZ_CODE; pos:%p; end:%p",
                          pos, end));
      if (get_str_len_and_pointer(&pos, &catalog, &catalog_len, end))
      {
        DBUG_PRINT("info", ("query= 0"));
        query= 0;
        DBUG_VOID_RETURN;
      }
      break;
    case Q_AUTO_INCREMENT:
      CHECK_SPACE(pos, end, 4);
      auto_increment_increment= uint2korr(pos);
      auto_increment_offset=    uint2korr(pos+2);
      pos+= 4;
      break;
    case Q_CHARSET_CODE:
    {
      CHECK_SPACE(pos, end, 6);
      charset_inited= 1;
      memcpy(charset, pos, 6);
      pos+= 6;
      break;
    }
    case Q_TIME_ZONE_CODE:
    {
      if (get_str_len_and_pointer(&pos, &time_zone_str, &time_zone_len, end))
      {
        DBUG_PRINT("info", ("Q_TIME_ZONE_CODE: query= 0"));
        query= 0;
        DBUG_VOID_RETURN;
      }
      break;
    }
    case Q_CATALOG_CODE: /* for 5.0.x where 0<=x<=3 masters */
      CHECK_SPACE(pos, end, 1);
      if ((catalog_len= *pos))
        catalog= (char*) pos+1;                           // Will be copied later
      CHECK_SPACE(pos, end, catalog_len + 2);
      pos+= catalog_len+2; // leap over end 0
      catalog_nz= 0; // catalog has end 0 in event
      break;
    case Q_LC_TIME_NAMES_CODE:
      CHECK_SPACE(pos, end, 2);
      lc_time_names_number= uint2korr(pos);
      pos+= 2;
      break;
    case Q_CHARSET_DATABASE_CODE:
      CHECK_SPACE(pos, end, 2);
      charset_database_number= uint2korr(pos);
      pos+= 2;
      break;
    case Q_TABLE_MAP_FOR_UPDATE_CODE:
      CHECK_SPACE(pos, end, 8);
      table_map_for_update= uint8korr(pos);
      pos+= 8;
      break;
    case Q_MASTER_DATA_WRITTEN_CODE:
      CHECK_SPACE(pos, end, 4);
      data_written= master_data_written= uint4korr(pos);
      pos+= 4;
      break;
    case Q_INVOKER:
    {
      CHECK_SPACE(pos, end, 1);
      user.length= *pos++;
      CHECK_SPACE(pos, end, user.length);
      user.str= (char *)pos;
      pos+= user.length;

      CHECK_SPACE(pos, end, 1);
      host.length= *pos++;
      CHECK_SPACE(pos, end, host.length);
      host.str= (char *)pos;
      pos+= host.length;
      break;
    }
    case Q_HRNOW:
    {
      CHECK_SPACE(pos, end, 3);
      when_sec_part= uint3korr(pos);
      pos+= 3;
      break;
    }
   case Q_XID:
    {
      CHECK_SPACE(pos, end, 8);
      xid= uint8korr(pos);
      pos+= 8;
      break;
    }
    case Q_GTID_FLAGS3:
    {
      CHECK_SPACE(pos, end, 1);
      gtid_flags_extra= *pos++;
      if (gtid_flags_extra & (Gtid_log_event::FL_COMMIT_ALTER_E1 |
                              Gtid_log_event::FL_ROLLBACK_ALTER_E1))
      {
        CHECK_SPACE(pos, end, 8);
        sa_seq_no = uint8korr(pos);
        pos+= 8;
      }
      break;
    }
    default:
      /* That's why you must write status vars in growing order of code */
      DBUG_PRINT("info",("Query_log_event has unknown status vars (first has\
 code: %u), skipping the rest of them", (uint) *(pos-1)));
      pos= (const uchar*) end;                         // Break loop
    }
  }

#if !defined(MYSQL_CLIENT)
  if (description_event->server_version_split.kind ==
      Format_description_log_event::master_version_split::KIND_MYSQL)
  {
    // Handle MariaDB/MySQL incompatible sql_mode bits
    sql_mode_t mysql_sql_mode= sql_mode;
    sql_mode&= MODE_MASK_MYSQL_COMPATIBLE; // Unset MySQL specific bits

    /*
      sql_mode flags related to fraction second rounding/truncation
      have opposite meaning in MySQL vs MariaDB.
      MySQL:
       - rounds fractional seconds by default
       - truncates if TIME_TRUNCATE_FRACTIONAL is set
      MariaDB:
       - truncates fractional seconds by default
       - rounds if TIME_ROUND_FRACTIONAL is set
    */
    if (description_event->server_version_split >= fsp_version_split_mysql &&
       !(mysql_sql_mode & MODE_MYSQL80_TIME_TRUNCATE_FRACTIONAL))
      sql_mode|= MODE_TIME_ROUND_FRACTIONAL;
  }
#endif

  /**
    Layout for the data buffer is as follows
    +--------+-----------+------+------+---------+----+-------+
    | catlog | time_zone | user | host | db name | \0 | Query |
    +--------+-----------+------+------+---------+----+-------+

    To support the query cache we append the following buffer to the above
    +-------+----------------------------------------+-------+
    |db len | uninitiatlized space of size of db len | FLAGS |
    +-------+----------------------------------------+-------+

    The area of buffer starting from Query field all the way to the end belongs
    to the Query buffer and its structure is described in alloc_query() in
    sql_parse.cc
    */

#if !defined(MYSQL_CLIENT) && defined(HAVE_QUERY_CACHE)
  if (!(start= data_buf= (Log_event::Byte*) my_malloc(PSI_INSTRUMENT_ME,
                                                       catalog_len + 1
                                                    +  time_zone_len + 1
                                                    +  user.length + 1
                                                    +  host.length + 1
                                                    +  data_len + 1
                                                    +  sizeof(size_t)//for db_len
                                                    +  db_len + 1
                                                    +  QUERY_CACHE_DB_LENGTH_SIZE
                                                    +  QUERY_CACHE_FLAGS_SIZE,
                                                       MYF(MY_WME))))
#else
  if (!(start= data_buf= (Log_event::Byte*) my_malloc(PSI_INSTRUMENT_ME,
                                                       catalog_len + 1
                                                    +  time_zone_len + 1
                                                    +  user.length + 1
                                                    +  host.length + 1
                                                    +  data_len + 1,
                                                       MYF(MY_WME))))
#endif
      DBUG_VOID_RETURN;
  if (catalog_len)                                  // If catalog is given
  {
    /**
      @todo we should clean up and do only copy_str_and_move; it
      works for both cases.  Then we can remove the catalog_nz
      flag. /sven
    */
    if (likely(catalog_nz)) // true except if event comes from 5.0.0|1|2|3.
      copy_str_and_move(&catalog, &start, catalog_len);
    else
    {
      memcpy(start, catalog, catalog_len+1); // copy end 0
      catalog= (const char *)start;
      start+= catalog_len+1;
    }
  }
  if (time_zone_len)
    copy_str_and_move(&time_zone_str, &start, time_zone_len);

  if (user.length)
  {
    copy_str_and_move(&user.str, &start, user.length);
  }
  else
  {
    user.str= (char*) start;
    *(start++)= 0;
  }

  if (host.length)
    copy_str_and_move(&host.str, &start, host.length);
  else
  {
    host.str= (char*) start;
    *(start++)= 0;
  }

  /**
    if time_zone_len or catalog_len are 0, then time_zone and catalog
    are uninitialized at this point.  shouldn't they point to the
    zero-length null-terminated strings we allocated space for in the
    my_alloc call above? /sven
  */

  /* A 2nd variable part; this is common to all versions */ 
  memcpy((char*) start, end, data_len);          // Copy db and query
  start[data_len]= '\0';              // End query with \0 (For safetly)
  db= (char *)start;
  query= (char *)(start + db_len + 1);
  q_len= data_len - db_len -1;

  if (data_len && (data_len < db_len ||
                   data_len < q_len ||
                   data_len != (db_len + q_len + 1)))
  {
    q_len= 0;
    query= NULL;
    DBUG_VOID_RETURN;
  }

  uint32 max_length= uint32(event_len - ((end + db_len + 1) -
                                         (buf - common_header_len)));
  if (q_len != max_length ||
      (event_len < uint((end + db_len + 1) - (buf - common_header_len))))
  {
    q_len= 0;
    query= NULL;
    DBUG_VOID_RETURN;
  }
  /**
    Append the db length at the end of the buffer. This will be used by
    Query_cache::send_result_to_client() in case the query cache is On.
   */
#if !defined(MYSQL_CLIENT) && defined(HAVE_QUERY_CACHE)
  size_t db_length= (size_t)db_len;
  memcpy(start + data_len + 1, &db_length, sizeof(size_t));
#endif
  DBUG_VOID_RETURN;
}

Query_compressed_log_event::Query_compressed_log_event(const uchar *buf,
      uint event_len,
      const Format_description_log_event
      *description_event,
      Log_event_type event_type)
      :Query_log_event(buf, event_len, description_event, event_type),
       query_buf(NULL)
{
  if (query)
  {
    uint32 un_len= binlog_get_uncompress_len((uchar*) query);
    if (!un_len)
    {
      query= 0;
      return;
    }

    /* Reserve one byte for '\0' */
    query_buf= (Log_event::Byte*) my_malloc(PSI_INSTRUMENT_ME,
                                            ALIGN_SIZE(un_len + 1), MYF(MY_WME));
    if (query_buf && !binlog_buf_uncompress((uchar*) query, (uchar *) query_buf,
                                            q_len, &un_len))
    {
      query_buf[un_len]= 0;
      query= (char*) query_buf;
      q_len= un_len;
    }
    else
    {
      query= 0;
    }
  }
}


/*
  Replace a binlog event read into a packet with a dummy event. Either a
  Query_log_event that has just a comment, or if that will not fit in the
  space used for the event to be replaced, then a NULL user_var event.

  This is used when sending binlog data to a slave which does not understand
  this particular event and which is too old to support informational events
  or holes in the event stream.

  This allows to write such events into the binlog on the master and still be
  able to replicate against old slaves without them breaking.

  Clears the flag LOG_EVENT_THREAD_SPECIFIC_F and set LOG_EVENT_SUPPRESS_USE_F.
  Overwrites the type with QUERY_EVENT (or USER_VAR_EVENT), and replaces the
  body with a minimal query / NULL user var.

  Returns zero on success, -1 if error due to too little space in original
  event. A minimum of 25 bytes (19 bytes fixed header + 6 bytes in the body)
  is needed in any event to be replaced with a dummy event.
*/
int
Query_log_event::dummy_event(String *packet, ulong ev_offset,
                             enum enum_binlog_checksum_alg checksum_alg)
{
  uchar *p= (uchar *)packet->ptr() + ev_offset;
  size_t data_len= packet->length() - ev_offset;
  uint16 flags;
  static const size_t min_user_var_event_len=
    LOG_EVENT_HEADER_LEN + UV_NAME_LEN_SIZE + 1 + UV_VAL_IS_NULL; // 25
  static const size_t min_query_event_len=
    LOG_EVENT_HEADER_LEN + QUERY_HEADER_LEN + 1 + 1; // 34

  if (checksum_alg == BINLOG_CHECKSUM_ALG_CRC32)
    data_len-= BINLOG_CHECKSUM_LEN;
  else
    DBUG_ASSERT(checksum_alg == BINLOG_CHECKSUM_ALG_UNDEF ||
                checksum_alg == BINLOG_CHECKSUM_ALG_OFF);

  if (data_len < min_user_var_event_len)
    /* Cannot replace with dummy, event too short. */
    return -1;

  flags= uint2korr(p + FLAGS_OFFSET);
  flags&= ~LOG_EVENT_THREAD_SPECIFIC_F;
  flags|= LOG_EVENT_SUPPRESS_USE_F;
  int2store(p + FLAGS_OFFSET, flags);

  if (data_len < min_query_event_len)
  {
    /*
      Have to use dummy user_var event for such a short packet.

      This works, but the event will be considered part of an event group with
      the following event. So for example @@global.sql_slave_skip_counter=1
      will skip not only the dummy event, but also the immediately following
      event.

      We write a NULL user var with the name @`!dummyvar` (or as much
      as that as will fit within the size of the original event - so
      possibly just @`!`).
    */
    static const char var_name[]= "!dummyvar";
    size_t name_len= data_len - (min_user_var_event_len - 1);

    p[EVENT_TYPE_OFFSET]= USER_VAR_EVENT;
    int4store(p + LOG_EVENT_HEADER_LEN, name_len);
    memcpy(p + LOG_EVENT_HEADER_LEN + UV_NAME_LEN_SIZE, var_name, name_len);
    p[LOG_EVENT_HEADER_LEN + UV_NAME_LEN_SIZE + name_len]= 1; // indicates NULL
  }
  else
  {
    /*
      Use a dummy query event, just a comment.
    */
    static const char message[]=
      "# Dummy event replacing event type %u that slave cannot handle.";
    char buf[sizeof(message)+1];  /* +1, as %u can expand to 3 digits. */
    uchar old_type= p[EVENT_TYPE_OFFSET];
    uchar *q= p + LOG_EVENT_HEADER_LEN;
    size_t comment_len, len;

    p[EVENT_TYPE_OFFSET]= QUERY_EVENT;
    int4store(q + Q_THREAD_ID_OFFSET, 0);
    int4store(q + Q_EXEC_TIME_OFFSET, 0);
    q[Q_DB_LEN_OFFSET]= 0;
    int2store(q + Q_ERR_CODE_OFFSET, 0);
    int2store(q + Q_STATUS_VARS_LEN_OFFSET, 0);
    q[Q_DATA_OFFSET]= 0;                    /* Zero terminator for empty db */
    q+= Q_DATA_OFFSET + 1;
    len= my_snprintf(buf, sizeof(buf), message, old_type);
    comment_len= data_len - (min_query_event_len - 1);
    if (comment_len <= len)
      memcpy(q, buf, comment_len);
    else
    {
      memcpy(q, buf, len);
      memset(q+len, ' ', comment_len - len);
    }
  }

  if (checksum_alg == BINLOG_CHECKSUM_ALG_CRC32)
  {
    ha_checksum crc= my_checksum(0, p, data_len);
    int4store(p + data_len, crc);
  }
  return 0;
}

/*
  Replace an event (GTID event) with a BEGIN query event, to be compatible
  with an old slave.
*/
int
Query_log_event::begin_event(String *packet, ulong ev_offset,
                             enum enum_binlog_checksum_alg checksum_alg)
{
  uchar *p= (uchar *)packet->ptr() + ev_offset;
  uchar *q= p + LOG_EVENT_HEADER_LEN;
  size_t data_len= packet->length() - ev_offset;
  uint16 flags;

  if (checksum_alg == BINLOG_CHECKSUM_ALG_CRC32)
    data_len-= BINLOG_CHECKSUM_LEN;
  else
    DBUG_ASSERT(checksum_alg == BINLOG_CHECKSUM_ALG_UNDEF ||
                checksum_alg == BINLOG_CHECKSUM_ALG_OFF);

  /*
    Currently we only need to replace GTID event.
    The length of GTID differs depending on whether it contains commit id.
  */
  DBUG_ASSERT(data_len == LOG_EVENT_HEADER_LEN + GTID_HEADER_LEN ||
              data_len == LOG_EVENT_HEADER_LEN + GTID_HEADER_LEN + 2);
  if (data_len != LOG_EVENT_HEADER_LEN + GTID_HEADER_LEN &&
      data_len != LOG_EVENT_HEADER_LEN + GTID_HEADER_LEN + 2)
    return 1;

  flags= uint2korr(p + FLAGS_OFFSET);
  flags&= ~LOG_EVENT_THREAD_SPECIFIC_F;
  flags|= LOG_EVENT_SUPPRESS_USE_F;
  int2store(p + FLAGS_OFFSET, flags);

  p[EVENT_TYPE_OFFSET]= QUERY_EVENT;
  int4store(q + Q_THREAD_ID_OFFSET, 0);
  int4store(q + Q_EXEC_TIME_OFFSET, 0);
  q[Q_DB_LEN_OFFSET]= 0;
  int2store(q + Q_ERR_CODE_OFFSET, 0);
  if (data_len == LOG_EVENT_HEADER_LEN + GTID_HEADER_LEN)
  {
    int2store(q + Q_STATUS_VARS_LEN_OFFSET, 0);
    q[Q_DATA_OFFSET]= 0;                    /* Zero terminator for empty db */
    q+= Q_DATA_OFFSET + 1;
  }
  else
  {
    DBUG_ASSERT(data_len == LOG_EVENT_HEADER_LEN + GTID_HEADER_LEN + 2);
    /* Put in an empty time_zone_str to take up the extra 2 bytes. */
    int2store(q + Q_STATUS_VARS_LEN_OFFSET, 2);
    q[Q_DATA_OFFSET]= Q_TIME_ZONE_CODE;
    q[Q_DATA_OFFSET+1]= 0;           /* Zero length for empty time_zone_str */
    q[Q_DATA_OFFSET+2]= 0;                  /* Zero terminator for empty db */
    q+= Q_DATA_OFFSET + 3;
  }
  memcpy(q, "BEGIN", 5);

  if (checksum_alg == BINLOG_CHECKSUM_ALG_CRC32)
  {
    ha_checksum crc= my_checksum(0, p, data_len);
    int4store(p + data_len, crc);
  }
  return 0;
}


/**************************************************************************
	Start_log_event_v3 methods
**************************************************************************/


Start_log_event_v3::Start_log_event_v3(const uchar *buf, uint event_len,
                                       const Format_description_log_event
                                       *description_event)
  :Log_event(buf, description_event), binlog_version(BINLOG_VERSION)
{
  if (event_len < LOG_EVENT_MINIMAL_HEADER_LEN + ST_COMMON_HEADER_LEN_OFFSET)
  {
    server_version[0]= 0;
    return;
  }
  buf+= LOG_EVENT_MINIMAL_HEADER_LEN;
  binlog_version= uint2korr(buf+ST_BINLOG_VER_OFFSET);
  memcpy(server_version, buf+ST_SERVER_VER_OFFSET,
	 ST_SERVER_VER_LEN);
  // prevent overrun if log is corrupted on disk
  server_version[ST_SERVER_VER_LEN-1]= 0;
  created= uint4korr(buf+ST_CREATED_OFFSET);
  dont_set_created= 1;
}


/***************************************************************************
       Format_description_log_event methods
****************************************************************************/

/**
  Format_description_log_event 1st ctor.

    Ctor. Can be used to create the event to write to the binary log (when the
    server starts or when FLUSH LOGS), or to create artificial events to parse
    binlogs from MySQL 3.23 or 4.x.
    When in a client, only the 2nd use is possible.

  @param binlog_version         the binlog version for which we want to build
                                an event. Can be 1 (=MySQL 3.23), 3 (=4.0.x
                                x>=2 and 4.1) or 4 (MySQL 5.0). Note that the
                                old 4.0 (binlog version 2) is not supported;
                                it should not be used for replication with
                                5.0.
  @param server_ver             a string containing the server version.
*/

Format_description_log_event::
Format_description_log_event(uint8 binlog_ver, const char* server_ver)
  :Start_log_event_v3(), event_type_permutation(0)
{
  binlog_version= binlog_ver;
  switch (binlog_ver) {
  case 4: /* MySQL 5.0 */
    memcpy(server_version, ::server_version, ST_SERVER_VER_LEN);
    DBUG_EXECUTE_IF("pretend_version_50034_in_binlog",
                    strmov(server_version, "5.0.34"););
    common_header_len= LOG_EVENT_HEADER_LEN;
    number_of_event_types= LOG_EVENT_TYPES;
    /* we'll catch my_malloc() error in is_valid() */
    post_header_len=(uint8*) my_malloc(PSI_INSTRUMENT_ME,
                                       number_of_event_types*sizeof(uint8)
                                       + BINLOG_CHECKSUM_ALG_DESC_LEN,
                                       MYF(0));
    /*
      This long list of assignments is not beautiful, but I see no way to
      make it nicer, as the right members are #defines, not array members, so
      it's impossible to write a loop.
    */
    if (post_header_len)
    {
#ifndef DBUG_OFF
      // Allows us to sanity-check that all events initialized their
      // events (see the end of this 'if' block).
      memset(post_header_len, 255, number_of_event_types*sizeof(uint8));
#endif

      /* Note: all event types must explicitly fill in their lengths here. */
      post_header_len[START_EVENT_V3-1]= START_V3_HEADER_LEN;
      post_header_len[QUERY_EVENT-1]= QUERY_HEADER_LEN;
      post_header_len[STOP_EVENT-1]= STOP_HEADER_LEN;
      post_header_len[ROTATE_EVENT-1]= ROTATE_HEADER_LEN;
      post_header_len[INTVAR_EVENT-1]= INTVAR_HEADER_LEN;
      post_header_len[LOAD_EVENT-1]= LOAD_HEADER_LEN;
      post_header_len[SLAVE_EVENT-1]= SLAVE_HEADER_LEN;
      post_header_len[CREATE_FILE_EVENT-1]= CREATE_FILE_HEADER_LEN;
      post_header_len[APPEND_BLOCK_EVENT-1]= APPEND_BLOCK_HEADER_LEN;
      post_header_len[EXEC_LOAD_EVENT-1]= EXEC_LOAD_HEADER_LEN;
      post_header_len[DELETE_FILE_EVENT-1]= DELETE_FILE_HEADER_LEN;
      post_header_len[NEW_LOAD_EVENT-1]= NEW_LOAD_HEADER_LEN;
      post_header_len[RAND_EVENT-1]= RAND_HEADER_LEN;
      post_header_len[USER_VAR_EVENT-1]= USER_VAR_HEADER_LEN;
      post_header_len[FORMAT_DESCRIPTION_EVENT-1]= FORMAT_DESCRIPTION_HEADER_LEN;
      post_header_len[XID_EVENT-1]= XID_HEADER_LEN;
      post_header_len[XA_PREPARE_LOG_EVENT-1]= XA_PREPARE_HEADER_LEN;
      post_header_len[BEGIN_LOAD_QUERY_EVENT-1]= BEGIN_LOAD_QUERY_HEADER_LEN;
      post_header_len[EXECUTE_LOAD_QUERY_EVENT-1]= EXECUTE_LOAD_QUERY_HEADER_LEN;
      /*
        The PRE_GA events are never be written to any binlog, but
        their lengths are included in Format_description_log_event.
        Hence, we need to be assign some value here, to avoid reading
        uninitialized memory when the array is written to disk.
      */
      post_header_len[PRE_GA_WRITE_ROWS_EVENT-1]= 0;
      post_header_len[PRE_GA_UPDATE_ROWS_EVENT-1]= 0;
      post_header_len[PRE_GA_DELETE_ROWS_EVENT-1]= 0;

      post_header_len[TABLE_MAP_EVENT-1]=       TABLE_MAP_HEADER_LEN;
      post_header_len[WRITE_ROWS_EVENT_V1-1]=   ROWS_HEADER_LEN_V1;
      post_header_len[UPDATE_ROWS_EVENT_V1-1]=  ROWS_HEADER_LEN_V1;
      post_header_len[DELETE_ROWS_EVENT_V1-1]=  ROWS_HEADER_LEN_V1;
      /*
        We here have the possibility to simulate a master of before we changed
        the table map id to be stored in 6 bytes: when it was stored in 4
        bytes (=> post_header_len was 6). This is used to test backward
        compatibility.
        This code can be removed after a few months (today is Dec 21st 2005),
        when we know that the 4-byte masters are not deployed anymore (check
        with Tomas Ulin first!), and the accompanying test (rpl_row_4_bytes)
        too.
      */
      DBUG_EXECUTE_IF("old_row_based_repl_4_byte_map_id_master",
                      post_header_len[TABLE_MAP_EVENT-1]=
                      post_header_len[WRITE_ROWS_EVENT_V1-1]=
                      post_header_len[UPDATE_ROWS_EVENT_V1-1]=
                      post_header_len[DELETE_ROWS_EVENT_V1-1]= 6;);
      post_header_len[INCIDENT_EVENT-1]= INCIDENT_HEADER_LEN;
      post_header_len[HEARTBEAT_LOG_EVENT-1]= 0;
      post_header_len[IGNORABLE_LOG_EVENT-1]= 0;
      post_header_len[ROWS_QUERY_LOG_EVENT-1]= 0;
      post_header_len[GTID_LOG_EVENT-1]= 0;
      post_header_len[ANONYMOUS_GTID_LOG_EVENT-1]= 0;
      post_header_len[PREVIOUS_GTIDS_LOG_EVENT-1]= 0;
      post_header_len[TRANSACTION_CONTEXT_EVENT-1]= 0;
      post_header_len[VIEW_CHANGE_EVENT-1]= 0;
      post_header_len[XA_PREPARE_LOG_EVENT-1]= 0;
      post_header_len[WRITE_ROWS_EVENT-1]=  ROWS_HEADER_LEN_V2;
      post_header_len[UPDATE_ROWS_EVENT-1]= ROWS_HEADER_LEN_V2;
      post_header_len[DELETE_ROWS_EVENT-1]= ROWS_HEADER_LEN_V2;

      // Set header length of the reserved events to 0
      memset(post_header_len + MYSQL_EVENTS_END - 1, 0,
             (MARIA_EVENTS_BEGIN - MYSQL_EVENTS_END)*sizeof(uint8));

      // Set header lengths of Maria events
      post_header_len[ANNOTATE_ROWS_EVENT-1]= ANNOTATE_ROWS_HEADER_LEN;
      post_header_len[BINLOG_CHECKPOINT_EVENT-1]=
        BINLOG_CHECKPOINT_HEADER_LEN;
      post_header_len[GTID_EVENT-1]= GTID_HEADER_LEN;
      post_header_len[GTID_LIST_EVENT-1]= GTID_LIST_HEADER_LEN;
      post_header_len[START_ENCRYPTION_EVENT-1]= START_ENCRYPTION_HEADER_LEN;

      //compressed event
      post_header_len[QUERY_COMPRESSED_EVENT-1]= QUERY_HEADER_LEN;
      post_header_len[WRITE_ROWS_COMPRESSED_EVENT-1]=   ROWS_HEADER_LEN_V2;
      post_header_len[UPDATE_ROWS_COMPRESSED_EVENT-1]=  ROWS_HEADER_LEN_V2;
      post_header_len[DELETE_ROWS_COMPRESSED_EVENT-1]=  ROWS_HEADER_LEN_V2;
      post_header_len[WRITE_ROWS_COMPRESSED_EVENT_V1-1]=   ROWS_HEADER_LEN_V1;
      post_header_len[UPDATE_ROWS_COMPRESSED_EVENT_V1-1]=  ROWS_HEADER_LEN_V1;
      post_header_len[DELETE_ROWS_COMPRESSED_EVENT_V1-1]=  ROWS_HEADER_LEN_V1;

      // Sanity-check that all post header lengths are initialized.
      int i;
      for (i=0; i<number_of_event_types; i++)
        DBUG_ASSERT(post_header_len[i] != 255);
    }
    break;

  case 1: /* 3.23 */
  case 3: /* 4.0.x x>=2 */
    /*
      We build an artificial (i.e. not sent by the master) event, which
      describes what those old master versions send.
    */
    if (binlog_ver==1)
      strmov(server_version, server_ver ? server_ver : "3.23");
    else
      strmov(server_version, server_ver ? server_ver : "4.0");
    common_header_len= binlog_ver==1 ? OLD_HEADER_LEN :
      LOG_EVENT_MINIMAL_HEADER_LEN;
    /*
      The first new event in binlog version 4 is Format_desc. So any event type
      after that does not exist in older versions. We use the events known by
      version 3, even if version 1 had only a subset of them (this is not a
      problem: it uses a few bytes for nothing but unifies code; it does not
      make the slave detect less corruptions).
    */
    number_of_event_types= FORMAT_DESCRIPTION_EVENT - 1;
    post_header_len=(uint8*) my_malloc(PSI_INSTRUMENT_ME,
                                  number_of_event_types*sizeof(uint8), MYF(0));
    if (post_header_len)
    {
      post_header_len[START_EVENT_V3-1]= START_V3_HEADER_LEN;
      post_header_len[QUERY_EVENT-1]= QUERY_HEADER_MINIMAL_LEN;
      post_header_len[STOP_EVENT-1]= 0;
      post_header_len[ROTATE_EVENT-1]= (binlog_ver==1) ? 0 : ROTATE_HEADER_LEN;
      post_header_len[INTVAR_EVENT-1]= 0;
      post_header_len[LOAD_EVENT-1]= LOAD_HEADER_LEN;
      post_header_len[SLAVE_EVENT-1]= 0;
      post_header_len[CREATE_FILE_EVENT-1]= CREATE_FILE_HEADER_LEN;
      post_header_len[APPEND_BLOCK_EVENT-1]= APPEND_BLOCK_HEADER_LEN;
      post_header_len[EXEC_LOAD_EVENT-1]= EXEC_LOAD_HEADER_LEN;
      post_header_len[DELETE_FILE_EVENT-1]= DELETE_FILE_HEADER_LEN;
      post_header_len[NEW_LOAD_EVENT-1]= post_header_len[LOAD_EVENT-1];
      post_header_len[RAND_EVENT-1]= 0;
      post_header_len[USER_VAR_EVENT-1]= 0;
    }
    break;
  default: /* Includes binlog version 2 i.e. 4.0.x x<=1 */
    post_header_len= 0; /* will make is_valid() fail */
    break;
  }
  calc_server_version_split();
  checksum_alg= BINLOG_CHECKSUM_ALG_UNDEF;
  reset_crypto();
}


/**
  The problem with this constructor is that the fixed header may have a
  length different from this version, but we don't know this length as we
  have not read the Format_description_log_event which says it, yet. This
  length is in the post-header of the event, but we don't know where the
  post-header starts.

  So this type of event HAS to:
  - either have the header's length at the beginning (in the header, at a
  fixed position which will never be changed), not in the post-header. That
  would make the header be "shifted" compared to other events.
  - or have a header of size LOG_EVENT_MINIMAL_HEADER_LEN (19), in all future
  versions, so that we know for sure.

  I (Guilhem) chose the 2nd solution. Rotate has the same constraint (because
  it is sent before Format_description_log_event).
*/

Format_description_log_event::
Format_description_log_event(const uchar *buf, uint event_len,
                             const Format_description_log_event*
                             description_event)
  :Start_log_event_v3(buf, event_len, description_event),
   common_header_len(0), post_header_len(NULL), event_type_permutation(0)
{
  DBUG_ENTER("Format_description_log_event::Format_description_log_event(char*,...)");
  if (!Start_log_event_v3::is_valid())
    DBUG_VOID_RETURN; /* sanity check */
  buf+= LOG_EVENT_MINIMAL_HEADER_LEN;
  if ((common_header_len=buf[ST_COMMON_HEADER_LEN_OFFSET]) < OLD_HEADER_LEN)
    DBUG_VOID_RETURN; /* sanity check */
  number_of_event_types=
    event_len - (LOG_EVENT_MINIMAL_HEADER_LEN + ST_COMMON_HEADER_LEN_OFFSET + 1);
  DBUG_PRINT("info", ("common_header_len=%d number_of_event_types=%d",
                      common_header_len, number_of_event_types));
  /* If alloc fails, we'll detect it in is_valid() */

  post_header_len= (uint8*) my_memdup(PSI_INSTRUMENT_ME,
                                      buf+ST_COMMON_HEADER_LEN_OFFSET+1,
                                      number_of_event_types*
                                      sizeof(*post_header_len),
                                      MYF(0));
  calc_server_version_split();
  if (!is_version_before_checksum(&server_version_split))
  {
    /* the last bytes are the checksum alg desc and value (or value's room) */
    number_of_event_types -= BINLOG_CHECKSUM_ALG_DESC_LEN;
    checksum_alg= (enum_binlog_checksum_alg)post_header_len[number_of_event_types];
  }
  else
  {
    checksum_alg= BINLOG_CHECKSUM_ALG_UNDEF;
  }
  reset_crypto();

  DBUG_VOID_RETURN;
}

bool Format_description_log_event::start_decryption(Start_encryption_log_event* sele)
{
  DBUG_ASSERT(crypto_data.scheme == 0);

  if (!sele->is_valid())
    return 1;

  memcpy(crypto_data.nonce, sele->nonce, BINLOG_NONCE_LENGTH);
  return crypto_data.init(sele->crypto_scheme, sele->key_version);
}


Version::Version(const char *version, const char **endptr)
{
  const char *p= version;
  ulong number;
  for (uint i= 0; i<=2; i++)
  {
    char *r;
    number= strtoul(p, &r, 10);
    /*
      It is an invalid version if any version number greater than 255 or
      first number is not followed by '.'.
    */
    if (number < 256 && (*r == '.' || i != 0))
      m_ver[i]= (uchar) number;
    else
    {
      *this= Version();
      break;
    }

    p= r;
    if (*r == '.')
      p++; // skip the dot
  }
  endptr[0]= p;
}


Format_description_log_event::
  master_version_split::master_version_split(const char *version)
{
  const char *p;
  static_cast<Version*>(this)[0]= Version(version, &p);
  if (strstr(p, "MariaDB") != 0 || strstr(p, "-maria-") != 0)
    kind= KIND_MARIADB;
  else
    kind= KIND_MYSQL;
}


/**
   Splits the event's 'server_version' string into three numeric pieces stored
   into 'server_version_split':
   X.Y.Zabc (X,Y,Z numbers, a not a digit) -> {X,Y,Z}
   X.Yabc -> {X,Y,0}
   'server_version_split' is then used for lookups to find if the server which
   created this event has some known bug.
*/
void Format_description_log_event::calc_server_version_split()
{
  server_version_split= master_version_split(server_version);

  DBUG_PRINT("info",("Format_description_log_event::server_version_split:"
                     " '%s' %d %d %d", server_version,
                     server_version_split[0],
                     server_version_split[1], server_version_split[2]));
}


/**
   @return TRUE is the event's version is earlier than one that introduced
   the replication event checksum. FALSE otherwise.
*/
bool
Format_description_log_event::is_version_before_checksum(const master_version_split
                                                         *version_split)
{
  return *version_split <
    (version_split->kind == master_version_split::KIND_MARIADB ?
     checksum_version_split_mariadb : checksum_version_split_mysql);
}

/**
   @param buf buffer holding serialized FD event
   @param len netto (possible checksum is stripped off) length of the event buf
   
   @return  the version-safe checksum alg descriptor where zero
            designates no checksum, 255 - the orginator is
            checksum-unaware (effectively no checksum) and the actuall
            [1-254] range alg descriptor.
*/
enum enum_binlog_checksum_alg get_checksum_alg(const uchar *buf, ulong len)
{
  enum enum_binlog_checksum_alg ret;
  char version[ST_SERVER_VER_LEN];

  DBUG_ENTER("get_checksum_alg");
  DBUG_ASSERT(buf[EVENT_TYPE_OFFSET] == FORMAT_DESCRIPTION_EVENT);

  memcpy(version,
         buf + LOG_EVENT_MINIMAL_HEADER_LEN + ST_SERVER_VER_OFFSET,
         ST_SERVER_VER_LEN);
  version[ST_SERVER_VER_LEN - 1]= 0;
  
  Format_description_log_event::master_version_split version_split(version);
  ret= Format_description_log_event::is_version_before_checksum(&version_split)
    ? BINLOG_CHECKSUM_ALG_UNDEF
    : (enum_binlog_checksum_alg)buf[len - BINLOG_CHECKSUM_LEN - BINLOG_CHECKSUM_ALG_DESC_LEN];
  DBUG_ASSERT(ret == BINLOG_CHECKSUM_ALG_OFF ||
              ret == BINLOG_CHECKSUM_ALG_UNDEF ||
              ret == BINLOG_CHECKSUM_ALG_CRC32);
  DBUG_RETURN(ret);
}

Start_encryption_log_event::
Start_encryption_log_event(const uchar *buf, uint event_len,
                           const Format_description_log_event* description_event)
  :Log_event(buf, description_event)
{
  if ((int)event_len ==
      LOG_EVENT_MINIMAL_HEADER_LEN + Start_encryption_log_event::get_data_size())
  {
    buf+= LOG_EVENT_MINIMAL_HEADER_LEN;
    crypto_scheme= *buf;
    key_version= uint4korr(buf + BINLOG_CRYPTO_SCHEME_LENGTH);
    memcpy(nonce,
           buf + BINLOG_CRYPTO_SCHEME_LENGTH + BINLOG_KEY_VERSION_LENGTH,
           BINLOG_NONCE_LENGTH);
  }
  else
    crypto_scheme= ~0; // invalid
}


/**************************************************************************
        Load_log_event methods
   General note about Load_log_event: the binlogging of LOAD DATA INFILE is
   going to be changed in 5.0 (or maybe in 5.1; not decided yet).
   However, the 5.0 slave could still have to read such events (from a 4.x
   master), convert them (which just means maybe expand the header, when 5.0
   servers have a UID in events) (remember that whatever is after the header
   will be like in 4.x, as this event's format is not modified in 5.0 as we
   will use new types of events to log the new LOAD DATA INFILE features).
   To be able to read/convert, we just need to not assume that the common
   header is of length LOG_EVENT_HEADER_LEN (we must use the description
   event).
   Note that I (Guilhem) manually tested replication of a big LOAD DATA INFILE
   between 3.23 and 5.0, and between 4.0 and 5.0, and it works fine (and the
   positions displayed in SHOW SLAVE STATUS then are fine too).
**************************************************************************/


/**
  @note
    The caller must do buf[event_len]= 0 before he starts using the
    constructed event.
*/

Load_log_event::Load_log_event(const uchar *buf, uint event_len,
                               const Format_description_log_event
                               *description_event)
  :Log_event(buf, description_event), num_fields(0), fields(0),
   field_lens(0),field_block_len(0),
   table_name(0), db(0), fname(0), local_fname(FALSE),
   /*
     Load_log_event which comes from the binary log does not contain
     information about the type of insert which was used on the master.
     Assume that it was an ordinary, non-concurrent LOAD DATA.
    */
   is_concurrent(FALSE)
{
  DBUG_ENTER("Load_log_event");
  /*
    I (Guilhem) manually tested replication of LOAD DATA INFILE for 3.23->5.0,
    4.0->5.0 and 5.0->5.0 and it works.
  */
  if (event_len)
    copy_log_event(buf, event_len,
                   (((uchar)buf[EVENT_TYPE_OFFSET] == LOAD_EVENT) ?
                   LOAD_HEADER_LEN + 
                    description_event->common_header_len :
                    LOAD_HEADER_LEN + LOG_EVENT_HEADER_LEN),
                   description_event);
  /* otherwise it's a derived class, will call copy_log_event() itself */
  DBUG_VOID_RETURN;
}


/*
  Load_log_event::copy_log_event()
*/

int Load_log_event::copy_log_event(const uchar *buf, ulong event_len,
                                   int body_offset,
                                   const Format_description_log_event
                                   *description_event)
{
  DBUG_ENTER("Load_log_event::copy_log_event");
  uint data_len;
  if ((int) event_len <= body_offset)
    DBUG_RETURN(1);
  const uchar *buf_end= buf + event_len;
  /* this is the beginning of the post-header */
  const uchar *data_head= buf + description_event->common_header_len;
  thread_id= slave_proxy_id= uint4korr(data_head + L_THREAD_ID_OFFSET);
  exec_time= uint4korr(data_head + L_EXEC_TIME_OFFSET);
  skip_lines= uint4korr(data_head + L_SKIP_LINES_OFFSET);
  table_name_len= (uint)data_head[L_TBL_LEN_OFFSET];
  db_len= (uint)data_head[L_DB_LEN_OFFSET];
  num_fields= uint4korr(data_head + L_NUM_FIELDS_OFFSET);

  /*
    Sql_ex.init() on success returns the pointer to the first byte after
    the sql_ex structure, which is the start of field lengths array.
  */
  if (!(field_lens= (uchar*) sql_ex.init(buf + body_offset, buf_end,
                                         buf[EVENT_TYPE_OFFSET] != LOAD_EVENT)))
    DBUG_RETURN(1);

  data_len= event_len - body_offset;
  if (num_fields > data_len) // simple sanity check against corruption
    DBUG_RETURN(1);
  for (uint i= 0; i < num_fields; i++)
    field_block_len+= (uint)field_lens[i] + 1;

  fields= (char*) field_lens + num_fields;
  table_name= fields + field_block_len;
  if (strlen(table_name) > NAME_LEN)
    goto err;

  db= table_name + table_name_len + 1;
  DBUG_EXECUTE_IF("simulate_invalid_address", db_len= data_len;);
  fname= db + db_len + 1;
  if ((db_len > data_len) || (fname > (char*) buf_end))
    goto err;
  fname_len= (uint) strlen(fname);
  if ((fname_len > data_len) || (fname + fname_len > (char*) buf_end))
    goto err;
  // null termination is accomplished by the caller doing buf[event_len]=0

  DBUG_RETURN(0);

err:
  // Invalid event.
  table_name= 0;
  DBUG_RETURN(1);
}


/**************************************************************************
  Rotate_log_event methods
**************************************************************************/

Rotate_log_event::Rotate_log_event(const uchar *buf, uint event_len,
                                   const Format_description_log_event*
                                   description_event)
  :Log_event(buf, description_event) ,new_log_ident(0), flags(DUP_NAME)
{
  DBUG_ENTER("Rotate_log_event::Rotate_log_event(char*,...)");
  // The caller will ensure that event_len is what we have at EVENT_LEN_OFFSET
  uint8 post_header_len= description_event->post_header_len[ROTATE_EVENT-1];
  uint ident_offset;
  if (event_len < (uint)(LOG_EVENT_MINIMAL_HEADER_LEN + post_header_len))
    DBUG_VOID_RETURN;
  buf+= LOG_EVENT_MINIMAL_HEADER_LEN;
  pos= post_header_len ? uint8korr(buf + R_POS_OFFSET) : 4;
  ident_len= (uint)(event_len - (LOG_EVENT_MINIMAL_HEADER_LEN + post_header_len));
  ident_offset= post_header_len;
  set_if_smaller(ident_len,FN_REFLEN-1);
  new_log_ident= my_strndup(PSI_INSTRUMENT_ME, (char*) buf + ident_offset,
                            (uint) ident_len, MYF(MY_WME));
  DBUG_PRINT("debug", ("new_log_ident: '%s'", new_log_ident));
  DBUG_VOID_RETURN;
}


/**************************************************************************
  Binlog_checkpoint_log_event methods
**************************************************************************/

Binlog_checkpoint_log_event::Binlog_checkpoint_log_event(
       const uchar *buf, uint event_len,
       const Format_description_log_event *description_event)
  :Log_event(buf, description_event), binlog_file_name(0)
{
  uint8 header_size= description_event->common_header_len;
  uint8 post_header_len=
    description_event->post_header_len[BINLOG_CHECKPOINT_EVENT-1];
  if (event_len < (uint) header_size + (uint) post_header_len ||
      post_header_len < BINLOG_CHECKPOINT_HEADER_LEN)
    return;
  buf+= header_size;
  /* See uint4korr and int4store below */
  compile_time_assert(BINLOG_CHECKPOINT_HEADER_LEN == 4);
  binlog_file_len= uint4korr(buf);
  if (event_len - (header_size + post_header_len) < binlog_file_len)
    return;
  binlog_file_name= my_strndup(PSI_INSTRUMENT_ME, (char*) buf + post_header_len,
                               binlog_file_len, MYF(MY_WME));
  return;
}


/**************************************************************************
        Global transaction ID stuff
**************************************************************************/

Gtid_log_event::Gtid_log_event(const uchar *buf, uint event_len,
                               const Format_description_log_event
                               *description_event)
  : Log_event(buf, description_event), seq_no(0), commit_id(0),
    flags_extra(0), extra_engines(0)
{
  uint8 header_size= description_event->common_header_len;
  uint8 post_header_len= description_event->post_header_len[GTID_EVENT-1];
  const uchar *buf_0= buf;
  if (event_len < (uint) header_size + (uint) post_header_len ||
      post_header_len < GTID_HEADER_LEN)
    return;

  buf+= header_size;
  seq_no= uint8korr(buf);
  buf+= 8;
  domain_id= uint4korr(buf);
  buf+= 4;
  flags2= *(buf++);
  if (flags2 & FL_GROUP_COMMIT_ID)
  {
    if (event_len < (uint)header_size + GTID_HEADER_LEN + 2)
    {
      seq_no= 0;                                // So is_valid() returns false
      return;
    }
    commit_id= uint8korr(buf);
    buf+= 8;
  }
  if (flags2 & (FL_PREPARED_XA | FL_COMPLETED_XA))
  {
    xid.formatID= uint4korr(buf);
    buf+= 4;

    xid.gtrid_length= (long) buf[0];
    xid.bqual_length= (long) buf[1];
    buf+= 2;

    long data_length= xid.bqual_length + xid.gtrid_length;
    memcpy(xid.data, buf, data_length);
    buf+= data_length;
  }

  /* the extra flags check and actions */
  if (static_cast<uint>(buf - buf_0) < event_len)
  {
    flags_extra= *buf++;
    /*
      extra engines flags presence is identifed by non-zero byte value
      at this point
    */
    if (flags_extra & FL_EXTRA_MULTI_ENGINE_E1)
    {
      DBUG_ASSERT(static_cast<uint>(buf - buf_0) < event_len);

      extra_engines= *buf++;

      DBUG_ASSERT(extra_engines > 0);
    }
    if (flags_extra & (FL_COMMIT_ALTER_E1 | FL_ROLLBACK_ALTER_E1))
    {
      sa_seq_no= uint8korr(buf);
      buf+= 8;
    }
  }
  /*
    the strict '<' part of the assert corresponds to extra zero-padded
    trailing bytes,
  */
  DBUG_ASSERT(static_cast<uint>(buf - buf_0) <= event_len);
  /* and the last of them is tested. */
#ifdef MYSQL_SERVER
#ifdef WITH_WSREP
  if (!WSREP_ON)
#endif
#endif
  DBUG_ASSERT(static_cast<uint>(buf - buf_0) == event_len ||
              buf_0[event_len - 1] == 0);
}

int compare_glle_gtids(const void * _gtid1, const void *_gtid2)
{
  rpl_gtid *gtid1= (rpl_gtid *) _gtid1;
  rpl_gtid *gtid2= (rpl_gtid *) _gtid2;

  int ret;
  if (*gtid1 < *gtid2)
    ret= -1;
  else if (*gtid1 > *gtid2)
    ret= 1;
  else
    ret= 0;
  return ret;
}

/* GTID list. */

Gtid_list_log_event::Gtid_list_log_event(const uchar *buf, uint event_len,
                                         const Format_description_log_event
                                         *description_event)
  : Log_event(buf, description_event), count(0), list(0), sub_id_list(0)
{
  uint32 i;
  uint32 val;
  uint8 header_size= description_event->common_header_len;
  uint8 post_header_len= description_event->post_header_len[GTID_LIST_EVENT-1];
  if (event_len < (uint) header_size + (uint) post_header_len ||
      post_header_len < GTID_LIST_HEADER_LEN)
    return;

  buf+= header_size;
  val= uint4korr(buf);
  count= val & ((1<<28)-1);
  gl_flags= val & ((uint32)0xf << 28);
  buf+= 4;
  if (event_len - (header_size + post_header_len) < count*element_size ||
      (!(list= (rpl_gtid *)my_malloc(PSI_INSTRUMENT_ME,
                            count*sizeof(*list) + (count == 0), MYF(MY_WME)))))
    return;

  for (i= 0; i < count; ++i)
  {
    list[i].domain_id= uint4korr(buf);
    buf+= 4;
    list[i].server_id= uint4korr(buf);
    buf+= 4;
    list[i].seq_no= uint8korr(buf);
    buf+= 8;
  }

#if defined(HAVE_REPLICATION) && !defined(MYSQL_CLIENT)
  if ((gl_flags & FLAG_IGN_GTIDS))
  {
    uint32 i;
    if (!(sub_id_list= (uint64 *)my_malloc(PSI_INSTRUMENT_ME,
                                           count*sizeof(uint64), MYF(MY_WME))))
    {
      my_free(list);
      list= NULL;
      return;
    }
    for (i= 0; i < count; ++i)
    {
      if (!(sub_id_list[i]=
            rpl_global_gtid_slave_state->next_sub_id(list[i].domain_id)))
      {
        my_free(list);
        my_free(sub_id_list);
        list= NULL;
        sub_id_list= NULL;
        return;
      }
    }
  }
#endif
}


/*
  Used to record gtid_list event while sending binlog to slave, without having to
  fully contruct the event object.
*/
bool
Gtid_list_log_event::peek(const char *event_start, size_t event_len,
                          enum enum_binlog_checksum_alg checksum_alg,
                          rpl_gtid **out_gtid_list, uint32 *out_list_len,
                          const Format_description_log_event *fdev)
{
  const char *p;
  uint32 count_field, count;
  rpl_gtid *gtid_list;

  if (checksum_alg == BINLOG_CHECKSUM_ALG_CRC32)
  {
    if (event_len > BINLOG_CHECKSUM_LEN)
      event_len-= BINLOG_CHECKSUM_LEN;
    else
      event_len= 0;
  }
  else
    DBUG_ASSERT(checksum_alg == BINLOG_CHECKSUM_ALG_UNDEF ||
                checksum_alg == BINLOG_CHECKSUM_ALG_OFF);

  if (event_len < (uint32)fdev->common_header_len + GTID_LIST_HEADER_LEN)
    return true;
  p= event_start + fdev->common_header_len;
  count_field= uint4korr(p);
  p+= 4;
  count= count_field & ((1<<28)-1);
  if (event_len < (uint32)fdev->common_header_len + GTID_LIST_HEADER_LEN +
      element_size * count)
    return true;
  if (!(gtid_list= (rpl_gtid *)my_malloc(PSI_INSTRUMENT_ME,
                          sizeof(rpl_gtid)*count + (count == 0), MYF(MY_WME))))
    return true;
  *out_gtid_list= gtid_list;
  *out_list_len= count;
  while (count--)
  {
    gtid_list->domain_id= uint4korr(p);
    p+= 4;
    gtid_list->server_id= uint4korr(p);
    p+= 4;
    gtid_list->seq_no= uint8korr(p);
    p+= 8;
    ++gtid_list;
  }

  return false;
}


/**************************************************************************
	Intvar_log_event methods
**************************************************************************/

/*
  Intvar_log_event::Intvar_log_event()
*/

Intvar_log_event::Intvar_log_event(const uchar *buf,
                                   const Format_description_log_event* description_event)
  :Log_event(buf, description_event)
{
  /* The Post-Header is empty. The Variable Data part begins immediately. */
  buf+= description_event->common_header_len +
    description_event->post_header_len[INTVAR_EVENT-1];
  type= buf[I_TYPE_OFFSET];
  val= uint8korr(buf+I_VAL_OFFSET);
}


/*
  Intvar_log_event::get_var_type_name()
*/

const char* Intvar_log_event::get_var_type_name()
{
  switch(type) {
  case LAST_INSERT_ID_EVENT: return "LAST_INSERT_ID";
  case INSERT_ID_EVENT: return "INSERT_ID";
  default: /* impossible */ return "UNKNOWN";
  }
}


/**************************************************************************
  Rand_log_event methods
**************************************************************************/

Rand_log_event::Rand_log_event(const uchar *buf,
                               const Format_description_log_event* description_event)
  :Log_event(buf, description_event)
{
  /* The Post-Header is empty. The Variable Data part begins immediately. */
  buf+= description_event->common_header_len +
    description_event->post_header_len[RAND_EVENT-1];
  seed1= uint8korr(buf+RAND_SEED1_OFFSET);
  seed2= uint8korr(buf+RAND_SEED2_OFFSET);
}


/**************************************************************************
  Xid_log_event methods
**************************************************************************/

/**
  @note
  It's ok not to use int8store here,
  as long as xid_t::set(ulonglong) and
  xid_t::get_my_xid doesn't do it either.
  We don't care about actual values of xids as long as
  identical numbers compare identically
*/

Xid_log_event::
Xid_log_event(const uchar *buf,
              const Format_description_log_event *description_event)
  :Xid_apply_log_event(buf, description_event)
{
  /* The Post-Header is empty. The Variable Data part begins immediately. */
  buf+= description_event->common_header_len +
    description_event->post_header_len[XID_EVENT-1];
  memcpy((char*) &xid, buf, sizeof(xid));
}

/**************************************************************************
  XA_prepare_log_event methods
**************************************************************************/
XA_prepare_log_event::
XA_prepare_log_event(const uchar *buf,
                     const Format_description_log_event *description_event)
  :Xid_apply_log_event(buf, description_event)
{
  buf+= description_event->common_header_len +
    description_event->post_header_len[XA_PREPARE_LOG_EVENT-1];
  one_phase= * (bool *) buf;
  buf+= 1;

  m_xid.formatID= uint4korr(buf);
  buf+= 4;
  m_xid.gtrid_length= uint4korr(buf);
  buf+= 4;
  // Todo: validity here and elsewhere checks to be replaced by MDEV-21839 fixes
  if (m_xid.gtrid_length <= 0 || m_xid.gtrid_length > MAXGTRIDSIZE)
  {
    m_xid.formatID= -1;
    return;
  }
  m_xid.bqual_length= uint4korr(buf);
  buf+= 4;
  if (m_xid.bqual_length < 0 || m_xid.bqual_length > MAXBQUALSIZE)
  {
    m_xid.formatID= -1;
    return;
  }
  DBUG_ASSERT(m_xid.gtrid_length + m_xid.bqual_length <= XIDDATASIZE);

  memcpy(m_xid.data, buf, m_xid.gtrid_length + m_xid.bqual_length);

  xid= NULL;
}


/**************************************************************************
  User_var_log_event methods
**************************************************************************/

User_var_log_event::
User_var_log_event(const uchar *buf, uint event_len,
                   const Format_description_log_event* description_event)
  :Log_event(buf, description_event)
#ifndef MYSQL_CLIENT
  , deferred(false), query_id(0)
#endif
{
  bool error= false;
  const uchar *buf_start= buf, *buf_end= buf + event_len;

  /* The Post-Header is empty. The Variable Data part begins immediately. */
  buf+= description_event->common_header_len +
    description_event->post_header_len[USER_VAR_EVENT-1];
  name_len= uint4korr(buf);
  /* Avoid reading out of buffer */
  if ((buf - buf_start) + UV_NAME_LEN_SIZE + name_len > event_len)
  {
    error= true;
    goto err;
  }

  name= (char *) buf + UV_NAME_LEN_SIZE;

  /*
    We don't know yet is_null value, so we must assume that name_len
    may have the bigger value possible, is_null= True and there is no
    payload for val, or even that name_len is 0.
  */
  if (name + name_len + UV_VAL_IS_NULL > (char*) buf_end)
  {
    error= true;
    goto err;
  }

  buf+= UV_NAME_LEN_SIZE + name_len;
  is_null= (bool) *buf;
  flags= User_var_log_event::UNDEF_F;    // defaults to UNDEF_F
  if (is_null)
  {
    type= STRING_RESULT;
    charset_number= my_charset_bin.number;
    val_len= 0;
    val= 0;  
  }
  else
  {
    val= (char *) (buf + UV_VAL_IS_NULL + UV_VAL_TYPE_SIZE +
                   UV_CHARSET_NUMBER_SIZE + UV_VAL_LEN_SIZE);

    if (val > (char*) buf_end)
    {
      error= true;
      goto err;
    }

    type= (Item_result) buf[UV_VAL_IS_NULL];
    charset_number= uint4korr(buf + UV_VAL_IS_NULL + UV_VAL_TYPE_SIZE);
    val_len= uint4korr(buf + UV_VAL_IS_NULL + UV_VAL_TYPE_SIZE +
                       UV_CHARSET_NUMBER_SIZE);

    /**
      We need to check if this is from an old server
      that did not pack information for flags.
      We do this by checking if there are extra bytes
      after the packed value. If there are we take the
      extra byte and it's value is assumed to contain
      the flags value.

      Old events will not have this extra byte, thence,
      we keep the flags set to UNDEF_F.
    */
    size_t bytes_read= (val + val_len) - (char*) buf_start;
    if (bytes_read > event_len)
    {
      error= true;
      goto err;
    }
    if ((data_written - bytes_read) > 0)
    {
      flags= (uint) *(buf + UV_VAL_IS_NULL + UV_VAL_TYPE_SIZE +
                    UV_CHARSET_NUMBER_SIZE + UV_VAL_LEN_SIZE +
                    val_len);
    }
  }

err:
  if (unlikely(error))
    name= 0;
}


/**************************************************************************
	Create_file_log_event methods
**************************************************************************/

/*
  Create_file_log_event ctor
*/

Create_file_log_event::
Create_file_log_event(const uchar *buf, uint len,
                      const Format_description_log_event* description_event)
  :Load_log_event(buf,0,description_event),fake_base(0),block(0),
   inited_from_old(0)
{
  DBUG_ENTER("Create_file_log_event::Create_file_log_event(char*,...)");
  uint block_offset;
  uint header_len= description_event->common_header_len;
  uint8 load_header_len= description_event->post_header_len[LOAD_EVENT-1];
  uint8 create_file_header_len= description_event->post_header_len[CREATE_FILE_EVENT-1];
  if (!(event_buf= (uchar*) my_memdup(PSI_INSTRUMENT_ME, buf, len,
                                      MYF(MY_WME))) ||
      copy_log_event(event_buf,len,
                     (((uchar)buf[EVENT_TYPE_OFFSET] == LOAD_EVENT) ?
                      load_header_len + header_len :
                      (fake_base ? (header_len+load_header_len) :
                       (header_len+load_header_len) +
                       create_file_header_len)),
                     description_event))
    DBUG_VOID_RETURN;
  if (description_event->binlog_version!=1)
  {
    file_id= uint4korr(buf + 
                       header_len +
		       load_header_len + CF_FILE_ID_OFFSET);
    /*
      Note that it's ok to use get_data_size() below, because it is computed
      with values we have already read from this event (because we called
      copy_log_event()); we are not using slave's format info to decode
      master's format, we are really using master's format info.
      Anyway, both formats should be identical (except the common_header_len)
      as these Load events are not changed between 4.0 and 5.0 (as logging of
      LOAD DATA INFILE does not use Load_log_event in 5.0).

      The + 1 is for \0 terminating fname  
    */
    block_offset= (description_event->common_header_len +
                   Load_log_event::get_data_size() +
                   create_file_header_len + 1);
    if (len < block_offset)
      DBUG_VOID_RETURN;
    block= const_cast<uchar*>(buf) + block_offset;
    block_len= len - block_offset;
  }
  else
  {
    sql_ex.force_new_format();
    inited_from_old= 1;
  }
  DBUG_VOID_RETURN;
}


/**************************************************************************
	Append_block_log_event methods
**************************************************************************/

/*
  Append_block_log_event ctor
*/

Append_block_log_event::
Append_block_log_event(const uchar *buf, uint len,
                       const Format_description_log_event* description_event)
  :Log_event(buf, description_event),block(0)
{
  DBUG_ENTER("Append_block_log_event::Append_block_log_event(char*,...)");
  uint8 common_header_len= description_event->common_header_len; 
  uint8 append_block_header_len=
    description_event->post_header_len[APPEND_BLOCK_EVENT-1];
  uint total_header_len= common_header_len+append_block_header_len;
  if (len < total_header_len)
    DBUG_VOID_RETURN;
  file_id= uint4korr(buf + common_header_len + AB_FILE_ID_OFFSET);
  block= const_cast<uchar*>(buf) + total_header_len;
  block_len= len - total_header_len;
  DBUG_VOID_RETURN;
}


/**************************************************************************
	Delete_file_log_event methods
**************************************************************************/

/*
  Delete_file_log_event ctor
*/

Delete_file_log_event::
Delete_file_log_event(const uchar *buf, uint len,
                      const Format_description_log_event* description_event)
  :Log_event(buf, description_event),file_id(0)
{
  uint8 common_header_len= description_event->common_header_len;
  uint8 delete_file_header_len= description_event->post_header_len[DELETE_FILE_EVENT-1];
  if (len < (uint)(common_header_len + delete_file_header_len))
    return;
  file_id= uint4korr(buf + common_header_len + DF_FILE_ID_OFFSET);
}


/**************************************************************************
	Execute_load_log_event methods
**************************************************************************/

/*
  Execute_load_log_event ctor
*/

Execute_load_log_event::
Execute_load_log_event(const uchar *buf, uint len,
                       const Format_description_log_event* description_event)
  :Log_event(buf, description_event), file_id(0)
{
  uint8 common_header_len= description_event->common_header_len;
  uint8 exec_load_header_len= description_event->post_header_len[EXEC_LOAD_EVENT-1];
  if (len < (uint)(common_header_len+exec_load_header_len))
    return;
  file_id= uint4korr(buf + common_header_len + EL_FILE_ID_OFFSET);
}


/**************************************************************************
	Begin_load_query_log_event methods
**************************************************************************/

Begin_load_query_log_event::
Begin_load_query_log_event(const uchar *buf, uint len,
                           const Format_description_log_event* desc_event)
  :Append_block_log_event(buf, len, desc_event)
{
}


/**************************************************************************
	Execute_load_query_log_event methods
**************************************************************************/


Execute_load_query_log_event::
Execute_load_query_log_event(const uchar *buf, uint event_len,
                             const Format_description_log_event* desc_event):
  Query_log_event(buf, event_len, desc_event, EXECUTE_LOAD_QUERY_EVENT),
  file_id(0), fn_pos_start(0), fn_pos_end(0)
{
  if (!Query_log_event::is_valid())
    return;

  buf+= desc_event->common_header_len;

  fn_pos_start= uint4korr(buf + ELQ_FN_POS_START_OFFSET);
  fn_pos_end= uint4korr(buf + ELQ_FN_POS_END_OFFSET);
  dup_handling= (enum_load_dup_handling)(*(buf + ELQ_DUP_HANDLING_OFFSET));

  if (fn_pos_start > q_len || fn_pos_end > q_len ||
      dup_handling > LOAD_DUP_REPLACE)
    return;

  file_id= uint4korr(buf + ELQ_FILE_ID_OFFSET);
}


ulong Execute_load_query_log_event::get_post_header_size_for_derived()
{
  return EXECUTE_LOAD_QUERY_EXTRA_HEADER_LEN;
}


/**************************************************************************
	sql_ex_info methods
**************************************************************************/

/*
  sql_ex_info::init()
*/

const uchar *sql_ex_info::init(const uchar *buf, const uchar *buf_end,
                              bool use_new_format)
{
  cached_new_format= use_new_format;
  if (use_new_format)
  {
    empty_flags=0;
    /*
      The code below assumes that buf will not disappear from
      under our feet during the lifetime of the event. This assumption
      holds true in the slave thread if the log is in new format, but is not
      the case when we have old format because we will be reusing net buffer
      to read the actual file before we write out the Create_file event.
    */
    if (read_str(&buf, buf_end, &field_term, &field_term_len) ||
        read_str(&buf, buf_end, &enclosed,   &enclosed_len) ||
        read_str(&buf, buf_end, &line_term,  &line_term_len) ||
        read_str(&buf, buf_end, &line_start, &line_start_len) ||
        read_str(&buf, buf_end, &escaped,    &escaped_len))
      return 0;
    opt_flags= *buf++;
  }
  else
  {
    if (buf_end - buf < 7)
      return 0;                                 // Wrong data
    field_term_len= enclosed_len= line_term_len= line_start_len= escaped_len=1;
    field_term=  (char*) buf++;                 // Use first byte in string
    enclosed=    (char*) buf++;
    line_term=   (char*) buf++;
    line_start=  (char*) buf++;
    escaped=     (char*) buf++;
    opt_flags=   *buf++;
    empty_flags= *buf++;
    if (empty_flags & FIELD_TERM_EMPTY)
      field_term_len=0;
    if (empty_flags & ENCLOSED_EMPTY)
      enclosed_len=0;
    if (empty_flags & LINE_TERM_EMPTY)
      line_term_len=0;
    if (empty_flags & LINE_START_EMPTY)
      line_start_len=0;
    if (empty_flags & ESCAPED_EMPTY)
      escaped_len=0;
  }
  return buf;
}



/**************************************************************************
	Rows_log_event member functions
**************************************************************************/


Rows_log_event::Rows_log_event(const uchar *buf, uint event_len,
                               const Format_description_log_event
                               *description_event)
  : Log_event(buf, description_event),
    m_row_count(0),
#ifndef MYSQL_CLIENT
    m_table(NULL),
#endif
    m_table_id(0), m_rows_buf(0), m_rows_cur(0), m_rows_end(0),
    m_extra_row_data(0)
#if !defined(MYSQL_CLIENT) && defined(HAVE_REPLICATION)
    , m_curr_row(NULL), m_curr_row_end(NULL),
    m_key(NULL), m_key_info(NULL), m_key_nr(0),
    master_had_triggers(0)
#endif
{
  DBUG_ENTER("Rows_log_event::Rows_log_event(const char*,...)");
  uint8 const common_header_len= description_event->common_header_len;
  Log_event_type event_type= (Log_event_type)(uchar)buf[EVENT_TYPE_OFFSET];
  m_type= event_type;
  m_cols_ai.bitmap= 0;

  uint8 const post_header_len= description_event->post_header_len[event_type-1];

  if (event_len < (uint)(common_header_len + post_header_len))
  {
    m_cols.bitmap= 0;
    DBUG_VOID_RETURN;
  }

  DBUG_PRINT("enter",("event_len: %u  common_header_len: %d  "
		      "post_header_len: %d",
		      event_len, common_header_len,
		      post_header_len));

  const uchar *post_start= buf + common_header_len;
  post_start+= RW_MAPID_OFFSET;
  if (post_header_len == 6)
  {
    /* Master is of an intermediate source tree before 5.1.4. Id is 4 bytes */
    m_table_id= uint4korr(post_start);
    post_start+= 4;
  }
  else
  {
    m_table_id= (ulong) uint6korr(post_start);
    post_start+= RW_FLAGS_OFFSET;
  }

  m_flags_pos= post_start - buf;
  m_flags= uint2korr(post_start);
  post_start+= 2;

  uint16 var_header_len= 0;
  if (post_header_len == ROWS_HEADER_LEN_V2)
  {
    /*
      Have variable length header, check length,
      which includes length bytes
    */
    var_header_len= uint2korr(post_start);
    /* Check length and also avoid out of buffer read */
    if (var_header_len < 2 ||
        event_len < static_cast<unsigned int>(var_header_len +
          (post_start - buf)))
    {
      m_cols.bitmap= 0;
      DBUG_VOID_RETURN;
    }
    var_header_len-= 2;

    /* Iterate over var-len header, extracting 'chunks' */
    const uchar *start= post_start + 2;
    const uchar *end= start + var_header_len;
    for (const uchar* pos= start; pos < end;)
    {
      switch(*pos++)
      {
      case RW_V_EXTRAINFO_TAG:
      {
        /* Have an 'extra info' section, read it in */
        assert((end - pos) >= EXTRA_ROW_INFO_HDR_BYTES);
        uint8 infoLen= pos[EXTRA_ROW_INFO_LEN_OFFSET];
        assert((end - pos) >= infoLen);
        /* Just store/use the first tag of this type, skip others */
        if (likely(!m_extra_row_data))
        {
          m_extra_row_data= (uchar*) my_malloc(PSI_INSTRUMENT_ME, infoLen,
                                               MYF(MY_WME));
          if (likely(m_extra_row_data != NULL))
          {
            memcpy(m_extra_row_data, pos, infoLen);
          }
        }
        pos+= infoLen;
        break;
      }
      default:
        /* Unknown code, we will not understand anything further here */
        pos= end; /* Break loop */
      }
    }
  }

  uchar const *const var_start=
    (const uchar *)buf + common_header_len + post_header_len + var_header_len;
  uchar const *const ptr_width= var_start;
  uchar *ptr_after_width= (uchar*) ptr_width;
  DBUG_PRINT("debug", ("Reading from %p", ptr_after_width));
  m_width= net_field_length(&ptr_after_width);
  DBUG_PRINT("debug", ("m_width=%lu", m_width));

  /* Avoid reading out of buffer */
  if (ptr_after_width + (m_width + 7) / 8 > (uchar*)buf + event_len)
  {
    m_cols.bitmap= NULL;
    DBUG_VOID_RETURN;
  }

  /* if my_bitmap_init fails, caught in is_valid() */
  if (likely(!my_bitmap_init(&m_cols,
                          m_width <= sizeof(m_bitbuf)*8 ? m_bitbuf : NULL,
                          m_width)))
  {
    DBUG_PRINT("debug", ("Reading from %p", ptr_after_width));
    memcpy(m_cols.bitmap, ptr_after_width, (m_width + 7) / 8);
    create_last_word_mask(&m_cols);
    ptr_after_width+= (m_width + 7) / 8;
    DBUG_DUMP("m_cols", (uchar*) m_cols.bitmap, no_bytes_in_map(&m_cols));
  }
  else
  {
    // Needed because my_bitmap_init() does not set it to null on failure
    m_cols.bitmap= NULL;
    DBUG_VOID_RETURN;
  }

  m_cols_ai.bitmap= m_cols.bitmap; /* See explanation in is_valid() */

  if (LOG_EVENT_IS_UPDATE_ROW(event_type))
  {
    DBUG_PRINT("debug", ("Reading from %p", ptr_after_width));

    /* if my_bitmap_init fails, caught in is_valid() */
    if (likely(!my_bitmap_init(&m_cols_ai,
                            m_width <= sizeof(m_bitbuf_ai)*8 ? m_bitbuf_ai : NULL,
                            m_width)))
    {
      DBUG_PRINT("debug", ("Reading from %p", ptr_after_width));
      memcpy(m_cols_ai.bitmap, ptr_after_width, (m_width + 7) / 8);
      create_last_word_mask(&m_cols_ai);
      ptr_after_width+= (m_width + 7) / 8;
      DBUG_DUMP("m_cols_ai", (uchar*) m_cols_ai.bitmap,
                no_bytes_in_map(&m_cols_ai));
    }
    else
    {
      // Needed because my_bitmap_init() does not set it to null on failure
      m_cols_ai.bitmap= 0;
      DBUG_VOID_RETURN;
    }
  }

  const uchar* const ptr_rows_data= (const uchar*) ptr_after_width;

  size_t const read_size= ptr_rows_data - (const unsigned char *) buf;
  if (read_size > event_len)
  {
    DBUG_VOID_RETURN;
  }
  size_t const data_size= event_len - read_size;
  DBUG_PRINT("info",("m_table_id: %llu  m_flags: %d  m_width: %lu  data_size: %lu",
                     m_table_id, m_flags, m_width, (ulong) data_size));

  m_rows_buf= (uchar*) my_malloc(PSI_INSTRUMENT_ME, data_size, MYF(MY_WME));
  if (likely((bool)m_rows_buf))
  {
#if !defined(MYSQL_CLIENT) && defined(HAVE_REPLICATION)
    m_curr_row= m_rows_buf;
#endif
    m_rows_end= m_rows_buf + data_size;
    m_rows_cur= m_rows_end;
    memcpy(m_rows_buf, ptr_rows_data, data_size);
    m_rows_before_size= ptr_rows_data - (const uchar *) buf; // Get the size that before SET part
  }
  else
    m_cols.bitmap= 0; // to not free it

  DBUG_VOID_RETURN;
}

void Rows_log_event::uncompress_buf()
{
  uint32 un_len= binlog_get_uncompress_len(m_rows_buf);
  if (!un_len)
    return;

  uchar *new_buf= (uchar*) my_malloc(PSI_INSTRUMENT_ME, ALIGN_SIZE(un_len),
                                     MYF(MY_WME));
  if (new_buf)
  {
    if (!binlog_buf_uncompress(m_rows_buf, new_buf,
                              (uint32)(m_rows_cur - m_rows_buf), &un_len))
    {
      my_free(m_rows_buf);
      m_rows_buf= new_buf;
#if !defined(MYSQL_CLIENT) && defined(HAVE_REPLICATION)
      m_curr_row= m_rows_buf;
#endif
      m_rows_end= m_rows_buf + un_len;
      m_rows_cur= m_rows_end;
      return;
    }
    else
    {
      my_free(new_buf);
    }
  }
  m_cols.bitmap= 0; // catch it in is_valid
}

Rows_log_event::~Rows_log_event()
{
  if (m_cols.bitmap == m_bitbuf) // no my_malloc happened
    m_cols.bitmap= 0; // so no my_free in my_bitmap_free
  my_bitmap_free(&m_cols); // To pair with my_bitmap_init().
  my_free(m_rows_buf);
  my_free(m_extra_row_data);
}

int Rows_log_event::get_data_size()
{
  int const general_type_code= get_general_type_code();

  uchar buf[MAX_INT_WIDTH];
  uchar *end= net_store_length(buf, m_width);

  DBUG_EXECUTE_IF("old_row_based_repl_4_byte_map_id_master",
                  return (int)(6 + no_bytes_in_map(&m_cols) + (end - buf) +
                  (general_type_code == UPDATE_ROWS_EVENT ? no_bytes_in_map(&m_cols_ai) : 0) +
                  m_rows_cur - m_rows_buf););
  int data_size= 0;
  Log_event_type type= get_type_code();
  bool is_v2_event= LOG_EVENT_IS_ROW_V2(type);
  if (is_v2_event)
  {
    data_size= ROWS_HEADER_LEN_V2 +
      (m_extra_row_data ?
       RW_V_TAG_LEN + m_extra_row_data[EXTRA_ROW_INFO_LEN_OFFSET]:
       0);
  }
  else
  {
    data_size= ROWS_HEADER_LEN_V1;
  }
  data_size+= no_bytes_in_map(&m_cols);
  data_size+= (uint) (end - buf);

  if (general_type_code == UPDATE_ROWS_EVENT)
    data_size+= no_bytes_in_map(&m_cols_ai);

  data_size+= (uint) (m_rows_cur - m_rows_buf);
  return data_size; 
}


/**************************************************************************
	Annotate_rows_log_event member functions
**************************************************************************/

Annotate_rows_log_event::
Annotate_rows_log_event(const uchar *buf,
                        uint event_len,
                        const Format_description_log_event *desc)
  : Log_event(buf, desc),
    m_save_thd_query_txt(0),
    m_save_thd_query_len(0),
    m_saved_thd_query(false),
    m_used_query_txt(0)
{
  m_query_len= event_len - desc->common_header_len;
  m_query_txt= (char*) buf + desc->common_header_len;
}

Annotate_rows_log_event::~Annotate_rows_log_event()
{
  DBUG_ENTER("Annotate_rows_log_event::~Annotate_rows_log_event");
#ifndef MYSQL_CLIENT
  if (m_saved_thd_query)
    thd->set_query(m_save_thd_query_txt, m_save_thd_query_len);
  else if (m_used_query_txt)
    thd->reset_query();
#endif
  DBUG_VOID_RETURN;
}

int Annotate_rows_log_event::get_data_size()
{
  return m_query_len;
}

Log_event_type Annotate_rows_log_event::get_type_code()
{
  return ANNOTATE_ROWS_EVENT;
}

bool Annotate_rows_log_event::is_valid() const
{
  return (m_query_txt != NULL && m_query_len != 0);
}


/**************************************************************************
	Table_map_log_event member functions and support functions
**************************************************************************/

/**
  @page How replication of field metadata works.
  
  When a table map is created, the master first calls 
  Table_map_log_event::save_field_metadata() which calculates how many 
  values will be in the field metadata. Only those fields that require the 
  extra data are added. The method also loops through all of the fields in 
  the table calling the method Field::save_field_metadata() which returns the
  values for the field that will be saved in the metadata and replicated to
  the slave. Once all fields have been processed, the table map is written to
  the binlog adding the size of the field metadata and the field metadata to
  the end of the body of the table map.

  When a table map is read on the slave, the field metadata is read from the 
  table map and passed to the table_def class constructor which saves the 
  field metadata from the table map into an array based on the type of the 
  field. Field metadata values not present (those fields that do not use extra 
  data) in the table map are initialized as zero (0). The array size is the 
  same as the columns for the table on the slave.

  Additionally, values saved for field metadata on the master are saved as a 
  string of bytes (uchar) in the binlog. A field may require 1 or more bytes
  to store the information. In cases where values require multiple bytes 
  (e.g. values > 255), the endian-safe methods are used to properly encode 
  the values on the master and decode them on the slave. When the field
  metadata values are captured on the slave, they are stored in an array of
  type uint16. This allows the least number of casts to prevent casting bugs
  when the field metadata is used in comparisons of field attributes. When
  the field metadata is used for calculating addresses in pointer math, the
  type used is uint32. 
*/

/*
  Constructor used by slave to read the event from the binary log.
 */
#if defined(HAVE_REPLICATION)
Table_map_log_event::Table_map_log_event(const uchar *buf, uint event_len,
                                         const Format_description_log_event
                                         *description_event)

  : Log_event(buf, description_event),
#ifndef MYSQL_CLIENT
    m_table(NULL),
#endif
    m_dbnam(NULL), m_dblen(0), m_tblnam(NULL), m_tbllen(0),
    m_colcnt(0), m_coltype(0),
    m_memory(NULL), m_table_id(ULONGLONG_MAX), m_flags(0),
    m_data_size(0), m_field_metadata(0), m_field_metadata_size(0),
    m_null_bits(0), m_meta_memory(NULL),
    m_optional_metadata_len(0), m_optional_metadata(NULL)
{
  unsigned int bytes_read= 0;
  DBUG_ENTER("Table_map_log_event::Table_map_log_event(const char*,uint,...)");

  uint8 common_header_len= description_event->common_header_len;
  uint8 post_header_len= description_event->post_header_len[TABLE_MAP_EVENT-1];
  DBUG_PRINT("info",("event_len: %u  common_header_len: %d  post_header_len: %d",
                     event_len, common_header_len, post_header_len));

  /*
    Don't print debug messages when running valgrind since they can
    trigger false warnings.
   */
#ifndef HAVE_valgrind
  DBUG_DUMP("event buffer", (uchar*) buf, event_len);
#endif

	if (event_len < (uint)(common_header_len + post_header_len))
		DBUG_VOID_RETURN;

  /* Read the post-header */
  const uchar *post_start= buf + common_header_len;

  post_start+= TM_MAPID_OFFSET;
  VALIDATE_BYTES_READ(post_start, buf, event_len);
  if (post_header_len == 6)
  {
    /* Master is of an intermediate source tree before 5.1.4. Id is 4 bytes */
    m_table_id= uint4korr(post_start);
    post_start+= 4;
  }
  else
  {
    DBUG_ASSERT(post_header_len == TABLE_MAP_HEADER_LEN);
    m_table_id= (ulong) uint6korr(post_start);
    post_start+= TM_FLAGS_OFFSET;
  }

  DBUG_ASSERT(m_table_id != ~0ULL);

  m_flags= uint2korr(post_start);

  /* Read the variable part of the event */
  const uchar *const vpart= buf + common_header_len + post_header_len;

  /* Extract the length of the various parts from the buffer */
  uchar const *const ptr_dblen= (uchar const*)vpart + 0;
  VALIDATE_BYTES_READ(ptr_dblen, buf, event_len);
  m_dblen= *(uchar*) ptr_dblen;

  /* Length of database name + counter + terminating null */
  uchar const *const ptr_tbllen= ptr_dblen + m_dblen + 2;
  VALIDATE_BYTES_READ(ptr_tbllen, buf, event_len);
  m_tbllen= *(uchar*) ptr_tbllen;

  /* Length of table name + counter + terminating null */
  uchar const *const ptr_colcnt= ptr_tbllen + m_tbllen + 2;
  uchar *ptr_after_colcnt= (uchar*) ptr_colcnt;
  VALIDATE_BYTES_READ(ptr_after_colcnt, buf, event_len);
  m_colcnt= net_field_length(&ptr_after_colcnt);

  DBUG_PRINT("info",("m_dblen: %lu  off: %ld  m_tbllen: %lu  off: %ld  m_colcnt: %lu  off: %ld",
                     (ulong) m_dblen, (long) (ptr_dblen - vpart),
                     (ulong) m_tbllen, (long) (ptr_tbllen - vpart),
                     m_colcnt, (long) (ptr_colcnt - vpart)));

  /* Allocate mem for all fields in one go. If fails, caught in is_valid() */
  m_memory= (uchar*) my_multi_malloc(PSI_INSTRUMENT_ME, MYF(MY_WME),
                                     &m_dbnam, (uint) m_dblen + 1,
                                     &m_tblnam, (uint) m_tbllen + 1,
                                     &m_coltype, (uint) m_colcnt,
                                     NullS);

  if (m_memory)
  {
    /* Copy the different parts into their memory */
    strncpy(const_cast<char*>(m_dbnam), (const char*)ptr_dblen  + 1, m_dblen + 1);
    strncpy(const_cast<char*>(m_tblnam), (const char*)ptr_tbllen + 1, m_tbllen + 1);
    memcpy(m_coltype, ptr_after_colcnt, m_colcnt);

    ptr_after_colcnt= ptr_after_colcnt + m_colcnt;
    VALIDATE_BYTES_READ(ptr_after_colcnt, buf, event_len);
    m_field_metadata_size= net_field_length(&ptr_after_colcnt);
    if (m_field_metadata_size <= (m_colcnt * 2))
    {
      uint num_null_bytes= (m_colcnt + 7) / 8;
      m_meta_memory= (uchar *)my_multi_malloc(PSI_INSTRUMENT_ME, MYF(MY_WME),
          &m_null_bits, num_null_bytes,
          &m_field_metadata, m_field_metadata_size,
          NULL);
      memcpy(m_field_metadata, ptr_after_colcnt, m_field_metadata_size);
      ptr_after_colcnt= (uchar*)ptr_after_colcnt + m_field_metadata_size;
      memcpy(m_null_bits, ptr_after_colcnt, num_null_bytes);
      ptr_after_colcnt= (unsigned char*)ptr_after_colcnt + num_null_bytes;
    }
    else
    {
      m_coltype= NULL;
      my_free(m_memory);
      m_memory= NULL;
      DBUG_VOID_RETURN;
    }

    bytes_read= (uint) (ptr_after_colcnt - (uchar *)buf);

    /* After null_bits field, there are some new fields for extra metadata. */
    if (bytes_read < event_len)
    {
      m_optional_metadata_len= event_len - bytes_read;
      m_optional_metadata=
        static_cast<unsigned char*>(my_malloc(PSI_INSTRUMENT_ME, m_optional_metadata_len, MYF(MY_WME)));
      memcpy(m_optional_metadata, ptr_after_colcnt, m_optional_metadata_len);
    }
  }
#ifdef MYSQL_SERVER
  if (!m_table)
    DBUG_VOID_RETURN;
  binlog_type_info_array= (Binlog_type_info *)thd->alloc(m_table->s->fields *
                                                         sizeof(Binlog_type_info));
  for (uint i= 0; i <  m_table->s->fields; i++)
    binlog_type_info_array[i]= m_table->field[i]->binlog_type_info();
#endif

  DBUG_VOID_RETURN;
}
#endif

Table_map_log_event::~Table_map_log_event()
{
  my_free(m_meta_memory);
  my_free(m_memory);
  my_free(m_optional_metadata);
  m_optional_metadata= NULL;
}

/**
   Parses SIGNEDNESS field.

   @param[out] vec     stores the signedness flags extracted from field.
   @param[in]  field   SIGNEDNESS field in table_map_event.
   @param[in]  length  length of the field
 */
static void parse_signedness(std::vector<bool> &vec,
                             unsigned char *field, unsigned int length)
{
  for (unsigned int i= 0; i < length; i++)
  {
    for (unsigned char c= 0x80; c != 0; c>>= 1)
      vec.push_back(field[i] & c);
  }
}

/**
   Parses DEFAULT_CHARSET field.

   @param[out] default_charset  stores collation numbers extracted from field.
   @param[in]  field   DEFAULT_CHARSET field in table_map_event.
   @param[in]  length  length of the field
 */
static void parse_default_charset(Table_map_log_event::Optional_metadata_fields::
                                  Default_charset &default_charset,
                                  unsigned char *field, unsigned int length)
{
  unsigned char* p= field;

  default_charset.default_charset= net_field_length(&p);
  while (p < field + length)
  {
    unsigned int col_index= net_field_length(&p);
    unsigned int col_charset= net_field_length(&p);

    default_charset.charset_pairs.push_back(std::make_pair(col_index,
                                                           col_charset));
  }
}

/**
   Parses COLUMN_CHARSET field.

   @param[out] vec     stores collation numbers extracted from field.
   @param[in]  field   COLUMN_CHARSET field in table_map_event.
   @param[in]  length  length of the field
 */
static void parse_column_charset(std::vector<unsigned int> &vec,
                                 unsigned char *field, unsigned int length)
{
  unsigned char* p= field;

  while (p < field + length)
    vec.push_back(net_field_length(&p));
}

/**
   Parses COLUMN_NAME field.

   @param[out] vec     stores column names extracted from field.
   @param[in]  field   COLUMN_NAME field in table_map_event.
   @param[in]  length  length of the field
 */
static void parse_column_name(std::vector<std::string> &vec,
                              unsigned char *field, unsigned int length)
{
  unsigned char* p= field;

  while (p < field + length)
  {
    unsigned len= net_field_length(&p);
    vec.push_back(std::string(reinterpret_cast<char *>(p), len));
    p+= len;
  }
}

/**
   Parses SET_STR_VALUE/ENUM_STR_VALUE field.

   @param[out] vec     stores SET/ENUM column's string values extracted from
                       field. Each SET/ENUM column's string values are stored
                       into a string separate vector. All of them are stored
                       in 'vec'.
   @param[in]  field   COLUMN_NAME field in table_map_event.
   @param[in]  length  length of the field
 */
static void parse_set_str_value(std::vector<Table_map_log_event::
                                Optional_metadata_fields::str_vector> &vec,
                                unsigned char *field, unsigned int length)
{
  unsigned char* p= field;

  while (p < field + length)
  {
    unsigned int count= net_field_length(&p);

    vec.push_back(std::vector<std::string>());
    for (unsigned int i= 0; i < count; i++)
    {
      unsigned len1= net_field_length(&p);
      vec.back().push_back(std::string(reinterpret_cast<char *>(p), len1));
      p+= len1;
    }
  }
}

/**
   Parses GEOMETRY_TYPE field.

   @param[out] vec     stores geometry column's types extracted from field.
   @param[in]  field   GEOMETRY_TYPE field in table_map_event.
   @param[in]  length  length of the field
 */
static void parse_geometry_type(std::vector<unsigned int> &vec,
                                unsigned char *field, unsigned int length)
{
  unsigned char* p= field;

  while (p < field + length)
    vec.push_back(net_field_length(&p));
}

/**
   Parses SIMPLE_PRIMARY_KEY field.

   @param[out] vec     stores primary key's column information extracted from
                       field. Each column has an index and a prefix which are
                       stored as a unit_pair. prefix is always 0 for
                       SIMPLE_PRIMARY_KEY field.
   @param[in]  field   SIMPLE_PRIMARY_KEY field in table_map_event.
   @param[in]  length  length of the field
 */
static void parse_simple_pk(std::vector<Table_map_log_event::
                            Optional_metadata_fields::uint_pair> &vec,
                            unsigned char *field, unsigned int length)
{
  unsigned char* p= field;

  while (p < field + length)
    vec.push_back(std::make_pair(net_field_length(&p), 0));
}

/**
   Parses PRIMARY_KEY_WITH_PREFIX field.

   @param[out] vec     stores primary key's column information extracted from
                       field. Each column has an index and a prefix which are
                       stored as a unit_pair.
   @param[in]  field   PRIMARY_KEY_WITH_PREFIX field in table_map_event.
   @param[in]  length  length of the field
 */

static void parse_pk_with_prefix(std::vector<Table_map_log_event::
                                 Optional_metadata_fields::uint_pair> &vec,
                                 unsigned char *field, unsigned int length)
{
  unsigned char* p= field;

  while (p < field + length)
  {
    unsigned int col_index= net_field_length(&p);
    unsigned int col_prefix= net_field_length(&p);
    vec.push_back(std::make_pair(col_index, col_prefix));
  }
}

Table_map_log_event::Optional_metadata_fields::
Optional_metadata_fields(unsigned char* optional_metadata,
                         unsigned int optional_metadata_len)
{
  unsigned char* field= optional_metadata;

  if (optional_metadata == NULL)
    return;

  while (field < optional_metadata + optional_metadata_len)
  {
    unsigned int len;
    Optional_metadata_field_type type=
      static_cast<Optional_metadata_field_type>(field[0]);

    // Get length and move field to the value.
    field++;
    len= net_field_length(&field);

    switch(type)
    {
    case SIGNEDNESS:
      parse_signedness(m_signedness, field, len);
      break;
    case DEFAULT_CHARSET:
      parse_default_charset(m_default_charset, field, len);
      break;
    case COLUMN_CHARSET:
      parse_column_charset(m_column_charset, field, len);
      break;
    case COLUMN_NAME:
      parse_column_name(m_column_name, field, len);
      break;
    case SET_STR_VALUE:
      parse_set_str_value(m_set_str_value, field, len);
      break;
    case ENUM_STR_VALUE:
      parse_set_str_value(m_enum_str_value, field, len);
      break;
    case GEOMETRY_TYPE:
      parse_geometry_type(m_geometry_type, field, len);
      break;
    case SIMPLE_PRIMARY_KEY:
      parse_simple_pk(m_primary_key, field, len);
      break;
    case PRIMARY_KEY_WITH_PREFIX:
      parse_pk_with_prefix(m_primary_key, field, len);
      break;
    case ENUM_AND_SET_DEFAULT_CHARSET:
      parse_default_charset(m_enum_and_set_default_charset, field, len);
      break;
    case ENUM_AND_SET_COLUMN_CHARSET:
      parse_column_charset(m_enum_and_set_column_charset, field, len);
      break;
    default:
      DBUG_ASSERT(0);
    }
    // next field
    field+= len;
  }
}


/**************************************************************************
	Write_rows_log_event member functions
**************************************************************************/


/*
  Constructor used by slave to read the event from the binary log.
 */
#ifdef HAVE_REPLICATION
Write_rows_log_event::Write_rows_log_event(const uchar *buf, uint event_len,
                                           const Format_description_log_event
                                           *description_event)
: Rows_log_event(buf, event_len, description_event)
{
}

Write_rows_compressed_log_event::Write_rows_compressed_log_event(
                                           const uchar *buf, uint event_len,
                                           const Format_description_log_event
                                           *description_event)
: Write_rows_log_event(buf, event_len, description_event)
{
  uncompress_buf();
}
#endif


/**************************************************************************
	Delete_rows_log_event member functions
**************************************************************************/

/*
  Constructor used by slave to read the event from the binary log.
 */
#ifdef HAVE_REPLICATION
Delete_rows_log_event::Delete_rows_log_event(const uchar *buf, uint event_len,
                                             const Format_description_log_event
                                             *description_event)
  : Rows_log_event(buf, event_len, description_event)
{
}

Delete_rows_compressed_log_event::Delete_rows_compressed_log_event(
                                           const uchar *buf, uint event_len,
                                           const Format_description_log_event
                                           *description_event)
  : Delete_rows_log_event(buf, event_len, description_event)
{
  uncompress_buf();
}
#endif

/**************************************************************************
	Update_rows_log_event member functions
**************************************************************************/

Update_rows_log_event::~Update_rows_log_event()
{
  if (m_cols_ai.bitmap)
  {
    if (m_cols_ai.bitmap == m_bitbuf_ai) // no my_malloc happened
      m_cols_ai.bitmap= 0; // so no my_free in my_bitmap_free
    my_bitmap_free(&m_cols_ai); // To pair with my_bitmap_init().
  }
}


/*
  Constructor used by slave to read the event from the binary log.
 */
#ifdef HAVE_REPLICATION
Update_rows_log_event::Update_rows_log_event(const uchar *buf, uint event_len,
                                             const
                                             Format_description_log_event
                                             *description_event)
  : Rows_log_event(buf, event_len, description_event)
{
}

Update_rows_compressed_log_event::Update_rows_compressed_log_event(
                                             const uchar *buf, uint event_len,
                                             const Format_description_log_event
                                             *description_event)
  : Update_rows_log_event(buf, event_len, description_event)
{
  uncompress_buf();
}
#endif

Incident_log_event::Incident_log_event(const uchar *buf, uint event_len,
                                       const Format_description_log_event *descr_event)
  : Log_event(buf, descr_event)
{
  DBUG_ENTER("Incident_log_event::Incident_log_event");
  uint8 const common_header_len=
    descr_event->common_header_len;
  uint8 const post_header_len=
    descr_event->post_header_len[INCIDENT_EVENT-1];

  DBUG_PRINT("info",("event_len: %u; common_header_len: %d; post_header_len: %d",
                     event_len, common_header_len, post_header_len));

  m_message.str= NULL;
  m_message.length= 0;
  int incident_number= uint2korr(buf + common_header_len);
  if (incident_number >= INCIDENT_COUNT ||
      incident_number <= INCIDENT_NONE)
  {
    // If the incident is not recognized, this binlog event is
    // invalid.  If we set incident_number to INCIDENT_NONE, the
    // invalidity will be detected by is_valid().
    m_incident= INCIDENT_NONE;
    DBUG_VOID_RETURN;
  }
  m_incident= static_cast<Incident>(incident_number);
  uchar const *ptr= buf + common_header_len + post_header_len;
  uchar const *const str_end= buf + event_len;
  uint8 len= 0;                   // Assignment to keep compiler happy
  const char *str= NULL;          // Assignment to keep compiler happy
  if (read_str(&ptr, str_end, &str, &len))
  {
    /* Mark this event invalid */
    m_incident= INCIDENT_NONE;
    DBUG_VOID_RETURN;
  }
  if (!(m_message.str= (char*) my_malloc(key_memory_log_event, len+1, MYF(MY_WME))))
  {
    /* Mark this event invalid */
    m_incident= INCIDENT_NONE;
    DBUG_VOID_RETURN;
  }
  strmake(m_message.str, str, len);
  m_message.length= len;
  DBUG_PRINT("info", ("m_incident: %d", m_incident));
  DBUG_VOID_RETURN;
}


Incident_log_event::~Incident_log_event()
{
  if (m_message.str)
    my_free(m_message.str);
}


const char *
Incident_log_event::description() const
{
  static const char *const description[]= {
    "NOTHING",                                  // Not used
    "LOST_EVENTS"
  };

  DBUG_PRINT("info", ("m_incident: %d", m_incident));
  return description[m_incident];
}


Ignorable_log_event::Ignorable_log_event(const uchar *buf,
                                         const Format_description_log_event
                                         *descr_event,
                                         const char *event_name)
  :Log_event(buf, descr_event), number((int) (uchar) buf[EVENT_TYPE_OFFSET]),
   description(event_name)
{
  DBUG_ENTER("Ignorable_log_event::Ignorable_log_event");
  DBUG_VOID_RETURN;
}

Ignorable_log_event::~Ignorable_log_event()
{
}

bool copy_event_cache_to_file_and_reinit(IO_CACHE *cache, FILE *file)
{
  return (my_b_copy_all_to_file(cache, file) ||
          reinit_io_cache(cache, WRITE_CACHE, 0, FALSE, TRUE));
}

#if defined(MYSQL_SERVER) && defined(HAVE_REPLICATION)
int Log_event::apply_event(rpl_group_info* rgi)
{
  int res;
  THD_STAGE_INFO(thd, stage_apply_event);
  rgi->current_event= this;
  res= do_apply_event(rgi);
  rgi->current_event= NULL;
  THD_STAGE_INFO(thd, stage_after_apply_event);
  return res;
}
#endif
