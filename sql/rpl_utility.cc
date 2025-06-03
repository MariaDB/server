/* Copyright (c) 2006, 2013, Oracle and/or its affiliates.
   Copyright (c) 2011, 2013, Monty Program Ab

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

#include "mariadb.h"
#include <my_bit.h>
#include "rpl_utility.h"
#include "log_event.h"


/*********************************************************************
 *                   table_def member definitions                    *
 *********************************************************************/

/*
  This function returns the field size in raw bytes based on the type
  and the encoded field data from the master's raw data.
*/
uint32 table_def::calc_field_size(uint col, uchar *master_data) const
{
  uint32 length= 0;

  switch (type(col)) {
  case MYSQL_TYPE_NEWDECIMAL:
    length= my_decimal_get_binary_size(m_field_metadata[col] >> 8, 
                                       m_field_metadata[col] & 0xff);
    break;
  case MYSQL_TYPE_DECIMAL:
  case MYSQL_TYPE_FLOAT:
  case MYSQL_TYPE_DOUBLE:
    length= m_field_metadata[col];
    break;
  /*
    The cases for SET and ENUM are include for completeness, however
    both are mapped to type MYSQL_TYPE_STRING and their real types
    are encoded in the field metadata.
  */
  case MYSQL_TYPE_SET:
  case MYSQL_TYPE_ENUM:
  case MYSQL_TYPE_STRING:
  {
    uchar type= m_field_metadata[col] >> 8U;
    if ((type == MYSQL_TYPE_SET) || (type == MYSQL_TYPE_ENUM))
      length= m_field_metadata[col] & 0x00ff;
    else
    {
      /*
        We are reading the actual size from the master_data record
        because this field has the actual lengh stored in the first
        byte.
      */
      length= (uint) *master_data + 1;
      DBUG_ASSERT(length != 0);
    }
    break;
  }
  case MYSQL_TYPE_YEAR:
  case MYSQL_TYPE_TINY:
    length= 1;
    break;
  case MYSQL_TYPE_SHORT:
    length= 2;
    break;
  case MYSQL_TYPE_INT24:
    length= 3;
    break;
  case MYSQL_TYPE_LONG:
    length= 4;
    break;
#ifdef HAVE_LONG_LONG
  case MYSQL_TYPE_LONGLONG:
    length= 8;
    break;
#endif
  case MYSQL_TYPE_NULL:
    length= 0;
    break;
  case MYSQL_TYPE_NEWDATE:
    length= 3;
    break;
  case MYSQL_TYPE_DATE:
  case MYSQL_TYPE_TIME:
    length= 3;
    break;
  case MYSQL_TYPE_TIME2:
    length= my_time_binary_length(m_field_metadata[col]);
    break;
  case MYSQL_TYPE_TIMESTAMP:
    length= 4;
    break;
  case MYSQL_TYPE_TIMESTAMP2:
    length= my_timestamp_binary_length(m_field_metadata[col]);
    break;
  case MYSQL_TYPE_DATETIME:
    length= 8;
    break;
  case MYSQL_TYPE_DATETIME2:
    length= my_datetime_binary_length(m_field_metadata[col]);
    break;
  case MYSQL_TYPE_BIT:
  {
    /*
      Decode the size of the bit field from the master.
        from_len is the length in bytes from the master
        from_bit_len is the number of extra bits stored in the master record
      If from_bit_len is not 0, add 1 to the length to account for accurate
      number of bytes needed.
    */
    uint from_len= (m_field_metadata[col] >> 8U) & 0x00ff;
    uint from_bit_len= m_field_metadata[col] & 0x00ff;
    DBUG_ASSERT(from_bit_len <= 7);
    length= from_len + ((from_bit_len > 0) ? 1 : 0);
    break;
  }
  case MYSQL_TYPE_VARCHAR:
  case MYSQL_TYPE_VARCHAR_COMPRESSED:
  {
    length= m_field_metadata[col] > 255 ? 2 : 1; // c&p of Field_varstring::data_length()
    length+= length == 1 ? (uint32) *master_data : uint2korr(master_data);
    break;
  }
  case MYSQL_TYPE_TINY_BLOB:
  case MYSQL_TYPE_MEDIUM_BLOB:
  case MYSQL_TYPE_LONG_BLOB:
  case MYSQL_TYPE_BLOB:
  case MYSQL_TYPE_BLOB_COMPRESSED:
  case MYSQL_TYPE_GEOMETRY:
  {
    /*
      Compute the length of the data. We cannot use get_length() here
      since it is dependent on the specific table (and also checks the
      packlength using the internal 'table' pointer) and replication
      is using a fixed format for storing data in the binlog.
    */
    switch (m_field_metadata[col]) {
    case 1:
      length= *master_data;
      break;
    case 2:
      length= uint2korr(master_data);
      break;
    case 3:
      length= uint3korr(master_data);
      break;
    case 4:
      length= uint4korr(master_data);
      break;
    default:
      DBUG_ASSERT(0);		// Should not come here
      break;
    }

    length+= m_field_metadata[col];
    break;
  }
  default:
    length= ~(uint32) 0;
  }
  return length;
}

PSI_memory_key key_memory_table_def_memory;

table_def::table_def(unsigned char *types, ulong size,
                     uchar *field_metadata, int metadata_size,
                     uchar *null_bitmap, uint16 flags)
  : m_size(size), m_type(0), m_field_metadata_size(metadata_size),
    m_field_metadata(0), m_null_bits(0), m_flags(flags),
    m_memory(NULL)
{
  m_memory= (uchar *)my_multi_malloc(key_memory_table_def_memory, MYF(MY_WME),
                                     &m_type, size,
                                     &m_field_metadata,
                                     size * sizeof(uint16),
                                     &m_null_bits, (size + 7) / 8,
                                     NULL);

  bzero(m_field_metadata, size * sizeof(uint16));

  if (m_type)
    memcpy(m_type, types, size);
  else
    m_size= 0;
  /*
    Extract the data from the table map into the field metadata array
    iff there is field metadata. The variable metadata_size will be
    0 if we are replicating from an older version server since no field
    metadata was written to the table map. This can also happen if 
    there were no fields in the master that needed extra metadata.
  */
  if (m_size && metadata_size)
  { 
    int index= 0;
    for (unsigned int i= 0; i < m_size; i++)
    {
      switch (binlog_type(i)) {
      case MYSQL_TYPE_TINY_BLOB:
      case MYSQL_TYPE_BLOB:
      case MYSQL_TYPE_BLOB_COMPRESSED:
      case MYSQL_TYPE_MEDIUM_BLOB:
      case MYSQL_TYPE_LONG_BLOB:
      case MYSQL_TYPE_DOUBLE:
      case MYSQL_TYPE_FLOAT:
      case MYSQL_TYPE_GEOMETRY:
      {
        /*
          These types store a single byte.
        */
        m_field_metadata[i]= field_metadata[index];
        index++;
        break;
      }
      case MYSQL_TYPE_SET:
      case MYSQL_TYPE_ENUM:
      case MYSQL_TYPE_STRING:
      {
        uint16 x= field_metadata[index++] << 8U; // real_type
        x+= field_metadata[index++];            // pack or field length
        m_field_metadata[i]= x;
        break;
      }
      case MYSQL_TYPE_BIT:
      {
        uint16 x= field_metadata[index++]; 
        x = x + (field_metadata[index++] << 8U);
        m_field_metadata[i]= x;
        break;
      }
      case MYSQL_TYPE_VARCHAR:
      case MYSQL_TYPE_VARCHAR_COMPRESSED:
      {
        /*
          These types store two bytes.
        */
        char *ptr= (char *)&field_metadata[index];
        m_field_metadata[i]= uint2korr(ptr);
        index= index + 2;
        break;
      }
      case MYSQL_TYPE_NEWDECIMAL:
      {
        uint16 x= field_metadata[index++] << 8U; // precision
        x+= field_metadata[index++];            // decimals
        m_field_metadata[i]= x;
        break;
      }
      case MYSQL_TYPE_TIME2:
      case MYSQL_TYPE_DATETIME2:
      case MYSQL_TYPE_TIMESTAMP2:
        m_field_metadata[i]= field_metadata[index++];
        break;
      default:
        m_field_metadata[i]= 0;
        break;
      }
    }
  }
  if (m_size && null_bitmap)
    memcpy(m_null_bits, null_bitmap, (m_size + 7) / 8);
}


table_def::~table_def()
{
  my_free(m_memory);
#ifndef DBUG_OFF
  m_type= 0;
  m_size= 0;
#endif
}


/**
   @param   even_buf    point to the buffer containing serialized event
   @param   event_len   length of the event accounting possible checksum alg

   @return  TRUE        if test fails
            FALSE       as success

   @notes
    event_buf will have same values on return. However during the process of
    caluclating the checksum, it's temporary changed. Because of this the
    event_buf argument is not a pointer to const.

*/
bool event_checksum_test(uchar *event_buf, ulong event_len,
                         enum enum_binlog_checksum_alg alg)
{
  bool res= FALSE;
  uint16 flags= 0; // to store in FD's buffer flags orig value

  if (alg != BINLOG_CHECKSUM_ALG_OFF && alg != BINLOG_CHECKSUM_ALG_UNDEF)
  {
    ha_checksum incoming;
    ha_checksum computed;

    if (event_buf[EVENT_TYPE_OFFSET] == FORMAT_DESCRIPTION_EVENT)
    {
#ifdef DBUG_ASSERT_EXISTS
      int8 fd_alg= event_buf[event_len - BINLOG_CHECKSUM_LEN - 
                             BINLOG_CHECKSUM_ALG_DESC_LEN];
#endif
      /*
        FD event is checksummed and therefore verified w/o the binlog-in-use flag
      */
      flags= uint2korr(event_buf + FLAGS_OFFSET);
      if (flags & LOG_EVENT_BINLOG_IN_USE_F)
        event_buf[FLAGS_OFFSET] &= ~LOG_EVENT_BINLOG_IN_USE_F;
      /* 
         The only algorithm currently is CRC32. Zero indicates 
         the binlog file is checksum-free *except* the FD-event.
      */
      DBUG_ASSERT(fd_alg == BINLOG_CHECKSUM_ALG_CRC32 || fd_alg == 0);
      DBUG_ASSERT(alg == BINLOG_CHECKSUM_ALG_CRC32);
      /*
        Complile time guard to watch over  the max number of alg
      */
      compile_time_assert(BINLOG_CHECKSUM_ALG_ENUM_END <= 0x80);
    }
    incoming= uint4korr(event_buf + event_len - BINLOG_CHECKSUM_LEN);
    /* checksum the event content without the checksum part itself */
    computed= my_checksum(0, event_buf, event_len - BINLOG_CHECKSUM_LEN);
    if (flags != 0)
    {
      /* restoring the orig value of flags of FD */
      DBUG_ASSERT(event_buf[EVENT_TYPE_OFFSET] == FORMAT_DESCRIPTION_EVENT);
      event_buf[FLAGS_OFFSET]= (uchar) flags;
    }
    res= (DBUG_IF("simulate_checksum_test_failure") || computed != incoming);
  }
  return res;
}
