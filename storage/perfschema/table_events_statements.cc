/* Copyright (c) 2010, 2023, Oracle and/or its affiliates.

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

/**
  @file storage/perfschema/table_events_statements.cc
  Table EVENTS_STATEMENTS_xxx (implementation).
*/

#include "my_global.h"
#include "my_thread.h"
#include "table_events_statements.h"
#include "pfs_instr_class.h"
#include "pfs_instr.h"
#include "pfs_events_statements.h"
#include "pfs_timer.h"
#include "sp_head.h" /* TYPE_ENUM_FUNCTION, ... */
#include "table_helper.h"
#include "my_md5.h"
#include "pfs_buffer_container.h"

THR_LOCK table_events_statements_current::m_table_lock;

PFS_engine_table_share_state
table_events_statements_current::m_share_state = {
  false /* m_checked */
};

PFS_engine_table_share
table_events_statements_current::m_share=
{
  { C_STRING_WITH_LEN("events_statements_current") },
  &pfs_truncatable_acl,
  table_events_statements_current::create,
  NULL, /* write_row */
  table_events_statements_current::delete_all_rows,
  table_events_statements_current::get_row_count,
  sizeof(pos_events_statements_current), /* ref length */
  &m_table_lock,
  { C_STRING_WITH_LEN("CREATE TABLE events_statements_current("
                      "THREAD_ID BIGINT unsigned not null comment 'Thread associated with the event. Together with EVENT_ID uniquely identifies the row.',"
                      "EVENT_ID BIGINT unsigned not null comment 'Thread''s current event number at the start of the event. Together with THREAD_ID uniquely identifies the row.',"
                      "END_EVENT_ID BIGINT unsigned comment 'NULL when the event starts, set to the thread''s current event number at the end of the event.',"
                      "EVENT_NAME VARCHAR(128) not null comment 'Event instrument name and a NAME from the setup_instruments table',"
                      "SOURCE VARCHAR(64) comment 'Name and line number of the source file containing the instrumented code that produced the event.',"
                      "TIMER_START BIGINT unsigned comment 'Value in picoseconds when the event timing started or NULL if timing is not collected.',"
                      "TIMER_END BIGINT unsigned comment 'Value in picoseconds when the event timing ended, or NULL if the event has not ended or timing is not collected.',"
                      "TIMER_WAIT BIGINT unsigned comment 'Value in picoseconds of the event''s duration or NULL if the event has not ended or timing is not collected.',"
                      "LOCK_TIME bigint unsigned not null comment 'Time in picoseconds spent waiting for locks. The time is calculated in microseconds but stored in picoseconds for compatibility with other timings.',"
                      "SQL_TEXT LONGTEXT comment 'The SQL statement, or NULL if the command is not associated with an SQL statement.',"
                      "DIGEST VARCHAR(32) comment 'Statement digest.',"
                      "DIGEST_TEXT LONGTEXT comment 'Statement digest text.',"
                      "CURRENT_SCHEMA VARCHAR(64) comment 'Statement''s default database for the statement, or NULL if there was none.',"
                      "OBJECT_TYPE VARCHAR(64) comment 'NULL for top level statements. The parent statement object type for nested statements (stored programs).',"
                      "OBJECT_SCHEMA VARCHAR(64) comment 'NULL for top level statements. The parent statement object schema for nested statements (stored programs).',"
                      "OBJECT_NAME VARCHAR(64) comment 'NULL for top level statements. The parent statement object name for nested statements (stored programs).',"
                      "OBJECT_INSTANCE_BEGIN BIGINT unsigned comment 'Address in memory of the statement object.',"
                      "MYSQL_ERRNO INTEGER comment 'Error code. See MariaDB Error Codes for a full list.',"
                      "RETURNED_SQLSTATE VARCHAR(5) comment 'The SQLSTATE value.',"
                      "MESSAGE_TEXT VARCHAR(128) comment 'Statement error message. See MariaDB Error Codes.',"
                      "ERRORS BIGINT unsigned not null comment '0 if SQLSTATE signifies completion (starting with 00) or warning (01), otherwise 1.',"
                      "WARNINGS BIGINT unsigned not null comment 'Number of warnings from the diagnostics area.',"
                      "ROWS_AFFECTED BIGINT unsigned not null comment 'Number of rows affected the statement affected.',"
                      "ROWS_SENT BIGINT unsigned not null comment 'Number of rows returned.',"
                      "ROWS_EXAMINED BIGINT unsigned not null comment 'Number of rows read during the statement''s execution.',"
                      "CREATED_TMP_DISK_TABLES BIGINT unsigned not null comment 'Number of on-disk temp tables created by the statement.',"
                      "CREATED_TMP_TABLES BIGINT unsigned not null comment 'Number of temp tables created by the statement.',"
                      "SELECT_FULL_JOIN BIGINT unsigned not null comment 'Number of joins performed by the statement which did not use an index.',"
                      "SELECT_FULL_RANGE_JOIN BIGINT unsigned not null comment 'Number of joins performed by the statement which used a range search of the first table.',"
                      "SELECT_RANGE BIGINT unsigned not null comment 'Number of joins performed by the statement which used a range of the first table.',"
                      "SELECT_RANGE_CHECK BIGINT unsigned not null comment 'Number of joins without keys performed by the statement that check for key usage after each row.',"
                      "SELECT_SCAN BIGINT unsigned not null comment 'Number of joins performed by the statement which used a full scan of the first table.',"
                      "SORT_MERGE_PASSES BIGINT unsigned not null comment 'Number of merge passes by the sort algorithm performed by the statement. If too high, you may need to increase the sort_buffer_size.',"
                      "SORT_RANGE BIGINT unsigned not null comment 'Number of sorts performed by the statement which used a range.',"
                      "SORT_ROWS BIGINT unsigned not null comment 'Number of rows sorted by the statement.',"
                      "SORT_SCAN BIGINT unsigned not null comment 'Number of sorts performed by the statement which used a full table scan.',"
                      "NO_INDEX_USED BIGINT unsigned not null comment '0 if the statement performed a table scan with an index, 1 if without an index.',"
                      "NO_GOOD_INDEX_USED BIGINT unsigned not null comment '0 if a good index was found for the statement, 1 if no good index was found. See the Range checked for each record description in the EXPLAIN article.',"
                      "NESTING_EVENT_ID BIGINT unsigned comment 'NULL for top level statements. The parent statement event id for nested statements (stored programs).',"
                      "NESTING_EVENT_TYPE ENUM('TRANSACTION', 'STATEMENT', 'STAGE', 'WAIT') comment 'NULL for top level statements. The parent statement event type for nested statements (stored programs).',"
                      "NESTING_EVENT_LEVEL INT comment '0 for top level statements. The parent statement level plus 1 for nested statements (stored programs).')") },
  false, /* m_perpetual */
  false, /* m_optional */
  &m_share_state
};

THR_LOCK table_events_statements_history::m_table_lock;

PFS_engine_table_share_state
table_events_statements_history::m_share_state = {
  false /* m_checked */
};

PFS_engine_table_share
table_events_statements_history::m_share=
{
  { C_STRING_WITH_LEN("events_statements_history") },
  &pfs_truncatable_acl,
  table_events_statements_history::create,
  NULL, /* write_row */
  table_events_statements_history::delete_all_rows,
  table_events_statements_history::get_row_count,
  sizeof(pos_events_statements_history), /* ref length */
  &m_table_lock,
  { C_STRING_WITH_LEN("CREATE TABLE events_statements_history("
                      "THREAD_ID BIGINT unsigned not null comment 'Thread associated with the event. Together with EVENT_ID uniquely identifies the row.',"
                      "EVENT_ID BIGINT unsigned not null comment 'Thread''s current event number at the start of the event. Together with THREAD_ID uniquely identifies the row.',"
                      "END_EVENT_ID BIGINT unsigned comment 'NULL when the event starts, set to the thread''s current event number at the end of the event.',"
                      "EVENT_NAME VARCHAR(128) not null comment 'Event instrument name and a NAME from the setup_instruments table',"
                      "SOURCE VARCHAR(64) comment 'Name and line number of the source file containing the instrumented code that produced the event.',"
                      "TIMER_START BIGINT unsigned comment 'Value in picoseconds when the event timing started or NULL if timing is not collected.',"
                      "TIMER_END BIGINT unsigned comment 'Value in picoseconds when the event timing ended, or NULL if the event has not ended or timing is not collected.',"
                      "TIMER_WAIT BIGINT unsigned comment 'Value in picoseconds of the event''s duration or NULL if the event has not ended or timing is not collected.',"
                      "LOCK_TIME bigint unsigned not null comment 'Time in picoseconds spent waiting for locks. The time is calculated in microseconds but stored in picoseconds for compatibility with other timings.',"
                      "SQL_TEXT LONGTEXT comment 'The SQL statement, or NULL if the command is not associated with an SQL statement.',"
                      "DIGEST VARCHAR(32) comment 'Statement digest.',"
                      "DIGEST_TEXT LONGTEXT comment 'Statement digest text.',"
                      "CURRENT_SCHEMA VARCHAR(64) comment 'Statement''s default database for the statement, or NULL if there was none.',"
                      "OBJECT_TYPE VARCHAR(64) comment 'NULL for top level statements. The parent statement object type for nested statements (stored programs).',"
                      "OBJECT_SCHEMA VARCHAR(64) comment 'NULL for top level statements. The parent statement object schema for nested statements (stored programs).',"
                      "OBJECT_NAME VARCHAR(64) comment 'NULL for top level statements. The parent statement object name for nested statements (stored programs).',"
                      "OBJECT_INSTANCE_BEGIN BIGINT unsigned comment 'Address in memory of the statement object.',"
                      "MYSQL_ERRNO INTEGER comment 'Error code. See MariaDB Error Codes for a full list.',"
                      "RETURNED_SQLSTATE VARCHAR(5) comment 'The SQLSTATE value.',"
                      "MESSAGE_TEXT VARCHAR(128) comment 'Statement error message. See MariaDB Error Codes.',"
                      "ERRORS BIGINT unsigned not null comment '0 if SQLSTATE signifies completion (starting with 00) or warning (01), otherwise 1.',"
                      "WARNINGS BIGINT unsigned not null comment 'Number of warnings from the diagnostics area.',"
                      "ROWS_AFFECTED BIGINT unsigned not null comment 'Number of rows affected the statement affected.',"
                      "ROWS_SENT BIGINT unsigned not null comment 'Number of rows returned.',"
                      "ROWS_EXAMINED BIGINT unsigned not null comment 'Number of rows read during the statement''s execution.',"
                      "CREATED_TMP_DISK_TABLES BIGINT unsigned not null comment 'Number of on-disk temp tables created by the statement.',"
                      "CREATED_TMP_TABLES BIGINT unsigned not null comment 'Number of temp tables created by the statement.',"
                      "SELECT_FULL_JOIN BIGINT unsigned not null comment 'Number of joins performed by the statement which did not use an index.',"
                      "SELECT_FULL_RANGE_JOIN BIGINT unsigned not null comment 'Number of joins performed by the statement which used a range search of the first table.',"
                      "SELECT_RANGE BIGINT unsigned not null comment 'Number of joins performed by the statement which used a range of the first table.',"
                      "SELECT_RANGE_CHECK BIGINT unsigned not null comment 'Number of joins without keys performed by the statement that check for key usage after each row.',"
                      "SELECT_SCAN BIGINT unsigned not null comment 'Number of joins performed by the statement which used a full scan of the first table.',"
                      "SORT_MERGE_PASSES BIGINT unsigned not null comment 'Number of merge passes by the sort algorithm performed by the statement. If too high, you may need to increase the sort_buffer_size.',"
                      "SORT_RANGE BIGINT unsigned not null comment 'Number of sorts performed by the statement which used a range.',"
                      "SORT_ROWS BIGINT unsigned not null comment 'Number of rows sorted by the statement.',"
                      "SORT_SCAN BIGINT unsigned not null comment 'Number of sorts performed by the statement which used a full table scan.',"
                      "NO_INDEX_USED BIGINT unsigned not null comment '0 if the statement performed a table scan with an index, 1 if without an index.',"
                      "NO_GOOD_INDEX_USED BIGINT unsigned not null comment '0 if a good index was found for the statement, 1 if no good index was found. See the Range checked for each record description in the EXPLAIN article.',"
                      "NESTING_EVENT_ID BIGINT unsigned comment 'NULL for top level statements. The parent statement event id for nested statements (stored programs).',"
                      "NESTING_EVENT_TYPE ENUM('TRANSACTION', 'STATEMENT', 'STAGE', 'WAIT') comment 'NULL for top level statements. The parent statement event type for nested statements (stored programs).',"
                      "NESTING_EVENT_LEVEL INT comment '0 for top level statements. The parent statement level plus 1 for nested statements (stored programs).')") },
  false, /* m_perpetual */
  false, /* m_optional */
  &m_share_state
};

THR_LOCK table_events_statements_history_long::m_table_lock;

PFS_engine_table_share_state
table_events_statements_history_long::m_share_state = {
  false /* m_checked */
};

PFS_engine_table_share
table_events_statements_history_long::m_share=
{
  { C_STRING_WITH_LEN("events_statements_history_long") },
  &pfs_truncatable_acl,
  table_events_statements_history_long::create,
  NULL, /* write_row */
  table_events_statements_history_long::delete_all_rows,
  table_events_statements_history_long::get_row_count,
  sizeof(PFS_simple_index), /* ref length */
  &m_table_lock,
  { C_STRING_WITH_LEN("CREATE TABLE events_statements_history_long("
                      "THREAD_ID BIGINT unsigned not null comment 'Thread associated with the event. Together with EVENT_ID uniquely identifies the row.',"
                      "EVENT_ID BIGINT unsigned not null comment 'Thread''s current event number at the start of the event. Together with THREAD_ID uniquely identifies the row.',"
                      "END_EVENT_ID BIGINT unsigned comment 'NULL when the event starts, set to the thread''s current event number at the end of the event.',"
                      "EVENT_NAME VARCHAR(128) not null comment 'Event instrument name and a NAME from the setup_instruments table',"
                      "SOURCE VARCHAR(64) comment 'Name and line number of the source file containing the instrumented code that produced the event.',"
                      "TIMER_START BIGINT unsigned comment 'Value in picoseconds when the event timing started or NULL if timing is not collected.',"
                      "TIMER_END BIGINT unsigned comment 'Value in picoseconds when the event timing ended, or NULL if the event has not ended or timing is not collected.',"
                      "TIMER_WAIT BIGINT unsigned comment 'Value in picoseconds of the event''s duration or NULL if the event has not ended or timing is not collected.',"
                      "LOCK_TIME bigint unsigned not null comment 'Time in picoseconds spent waiting for locks. The time is calculated in microseconds but stored in picoseconds for compatibility with other timings.',"
                      "SQL_TEXT LONGTEXT comment 'The SQL statement, or NULL if the command is not associated with an SQL statement.',"
                      "DIGEST VARCHAR(32) comment 'Statement digest.',"
                      "DIGEST_TEXT LONGTEXT comment 'Statement digest text.',"
                      "CURRENT_SCHEMA VARCHAR(64) comment 'Statement''s default database for the statement, or NULL if there was none.',"
                      "OBJECT_TYPE VARCHAR(64) comment 'NULL for top level statements. The parent statement object type for nested statements (stored programs).',"
                      "OBJECT_SCHEMA VARCHAR(64) comment 'NULL for top level statements. The parent statement object schema for nested statements (stored programs).',"
                      "OBJECT_NAME VARCHAR(64) comment 'NULL for top level statements. The parent statement object name for nested statements (stored programs).',"
                      "OBJECT_INSTANCE_BEGIN BIGINT unsigned comment 'Address in memory of the statement object.',"
                      "MYSQL_ERRNO INTEGER comment 'Error code. See MariaDB Error Codes for a full list.',"
                      "RETURNED_SQLSTATE VARCHAR(5) comment 'The SQLSTATE value.',"
                      "MESSAGE_TEXT VARCHAR(128) comment 'Statement error message. See MariaDB Error Codes.',"
                      "ERRORS BIGINT unsigned not null comment '0 if SQLSTATE signifies completion (starting with 00) or warning (01), otherwise 1.',"
                      "WARNINGS BIGINT unsigned not null comment 'Number of warnings from the diagnostics area.',"
                      "ROWS_AFFECTED BIGINT unsigned not null comment 'Number of rows affected the statement affected.',"
                      "ROWS_SENT BIGINT unsigned not null comment 'Number of rows returned.',"
                      "ROWS_EXAMINED BIGINT unsigned not null comment 'Number of rows read during the statement''s execution.',"
                      "CREATED_TMP_DISK_TABLES BIGINT unsigned not null comment 'Number of on-disk temp tables created by the statement.',"
                      "CREATED_TMP_TABLES BIGINT unsigned not null comment 'Number of temp tables created by the statement.',"
                      "SELECT_FULL_JOIN BIGINT unsigned not null comment 'Number of joins performed by the statement which did not use an index.',"
                      "SELECT_FULL_RANGE_JOIN BIGINT unsigned not null comment 'Number of joins performed by the statement which used a range search of the first table.',"
                      "SELECT_RANGE BIGINT unsigned not null comment 'Number of joins performed by the statement which used a range of the first table.',"
                      "SELECT_RANGE_CHECK BIGINT unsigned not null comment 'Number of joins without keys performed by the statement that check for key usage after each row.',"
                      "SELECT_SCAN BIGINT unsigned not null comment 'Number of joins performed by the statement which used a full scan of the first table.',"
                      "SORT_MERGE_PASSES BIGINT unsigned not null comment 'Number of merge passes by the sort algorithm performed by the statement. If too high, you may need to increase the sort_buffer_size.',"
                      "SORT_RANGE BIGINT unsigned not null comment 'Number of sorts performed by the statement which used a range.',"
                      "SORT_ROWS BIGINT unsigned not null comment 'Number of rows sorted by the statement.',"
                      "SORT_SCAN BIGINT unsigned not null comment 'Number of sorts performed by the statement which used a full table scan.',"
                      "NO_INDEX_USED BIGINT unsigned not null comment '0 if the statement performed a table scan with an index, 1 if without an index.',"
                      "NO_GOOD_INDEX_USED BIGINT unsigned not null comment '0 if a good index was found for the statement, 1 if no good index was found. See the Range checked for each record description in the EXPLAIN article.',"
                      "NESTING_EVENT_ID BIGINT unsigned comment 'NULL for top level statements. The parent statement event id for nested statements (stored programs).',"
                      "NESTING_EVENT_TYPE ENUM('TRANSACTION', 'STATEMENT', 'STAGE', 'WAIT') comment 'NULL for top level statements. The parent statement event type for nested statements (stored programs).',"
                      "NESTING_EVENT_LEVEL INT comment '0 for top level statements. The parent statement level plus 1 for nested statements (stored programs).')") },
  false, /* m_perpetual */
  false, /* m_optional */
  &m_share_state
};

table_events_statements_common::table_events_statements_common
(const PFS_engine_table_share *share, void *pos)
  : PFS_engine_table(share, pos),
  m_row_exists(false)
{
  m_normalizer = time_normalizer::get_statement();
}

/**
  Build a row.
  @param statement                      the statement the cursor is reading
*/
void table_events_statements_common::make_row_part_1(PFS_events_statements *statement,
                                                     sql_digest_storage *digest)
{
  ulonglong timer_end;

  m_row_exists= false;

  PFS_statement_class *unsafe= (PFS_statement_class*)
    statement->m_event.m_class;
  PFS_statement_class *klass= sanitize_statement_class(unsafe);
  if (unlikely(klass == NULL))
    return;

  m_row.m_thread_internal_id= statement->m_event.m_thread_internal_id;
  m_row.m_event_id= statement->m_event.m_event_id;
  m_row.m_end_event_id= statement->m_event.m_end_event_id;
  m_row.m_nesting_event_id= statement->m_event.m_nesting_event_id;
  m_row.m_nesting_event_type= statement->m_event.m_nesting_event_type;
  m_row.m_nesting_event_level= statement->m_event.m_nesting_event_level;

  if (m_row.m_end_event_id == 0)
  {
    timer_end = get_statement_timer();
  }
  else
  {
    timer_end= statement->m_event.m_timer_end;
  }

  m_normalizer->to_pico(statement->m_event.m_timer_start, timer_end,
                      & m_row.m_timer_start, & m_row.m_timer_end, & m_row.m_timer_wait);
  m_row.m_lock_time= statement->m_lock_time * MICROSEC_TO_PICOSEC;

  m_row.m_name= klass->m_name;
  m_row.m_name_length= klass->m_name_length;

  CHARSET_INFO *cs= get_charset(statement->m_sqltext_cs_number, MYF(0));
  size_t valid_length= statement->m_sqltext_length;

  if (cs != NULL)
  {
    if (cs->mbmaxlen > 1)
    {
      valid_length= Well_formed_prefix(cs,
                                       statement->m_sqltext,
                                       valid_length).length();
    }
  }

  m_row.m_sqltext.set_charset(cs);
  m_row.m_sqltext.length(0);
  m_row.m_sqltext.append(statement->m_sqltext, (uint32)valid_length, cs);

  /* Indicate that sqltext is truncated or not well-formed. */
  if (statement->m_sqltext_truncated || valid_length < statement->m_sqltext_length)
  {
    size_t chars= m_row.m_sqltext.numchars();
    if (chars > 3)
    {
      chars-= 3;
      uint32 bytes_offset= m_row.m_sqltext.charpos(chars, 0);
      m_row.m_sqltext.length(bytes_offset);
      m_row.m_sqltext.append("...", 3);
    }
  }

  m_row.m_current_schema_name_length= statement->m_current_schema_name_length;
  if (m_row.m_current_schema_name_length > 0)
    memcpy(m_row.m_current_schema_name, statement->m_current_schema_name, m_row.m_current_schema_name_length);

  m_row.m_object_type= statement->m_sp_type;

  m_row.m_schema_name_length= statement->m_schema_name_length;
  if (m_row.m_schema_name_length > 0)
    memcpy(m_row.m_schema_name, statement->m_schema_name, m_row.m_schema_name_length);

  m_row.m_object_name_length= statement->m_object_name_length;
  if (m_row.m_object_name_length > 0)
    memcpy(m_row.m_object_name, statement->m_object_name, m_row.m_object_name_length);

  /* Disable source file and line to avoid stale __FILE__ pointers. */
  m_row.m_source_length= 0;

  memcpy(m_row.m_message_text, statement->m_message_text, sizeof(m_row.m_message_text));
  m_row.m_sql_errno= statement->m_sql_errno;
  memcpy(m_row.m_sqlstate, statement->m_sqlstate, SQLSTATE_LENGTH);
  m_row.m_error_count= statement->m_error_count;
  m_row.m_warning_count= statement->m_warning_count;
  m_row.m_rows_affected= statement->m_rows_affected;

  m_row.m_rows_sent= statement->m_rows_sent;
  m_row.m_rows_examined= statement->m_rows_examined;
  m_row.m_created_tmp_disk_tables= statement->m_created_tmp_disk_tables;
  m_row.m_created_tmp_tables= statement->m_created_tmp_tables;
  m_row.m_select_full_join= statement->m_select_full_join;
  m_row.m_select_full_range_join= statement->m_select_full_range_join;
  m_row.m_select_range= statement->m_select_range;
  m_row.m_select_range_check= statement->m_select_range_check;
  m_row.m_select_scan= statement->m_select_scan;
  m_row.m_sort_merge_passes= statement->m_sort_merge_passes;
  m_row.m_sort_range= statement->m_sort_range;
  m_row.m_sort_rows= statement->m_sort_rows;
  m_row.m_sort_scan= statement->m_sort_scan;
  m_row.m_no_index_used= statement->m_no_index_used;
  m_row.m_no_good_index_used= statement->m_no_good_index_used;

  /*
    Making a copy of digest storage.
  */
  digest->copy(& statement->m_digest_storage);

  m_row_exists= true;
  return;
}

void table_events_statements_common::make_row_part_2(const sql_digest_storage *digest)
{
  /*
    Filling up statement digest information.
  */
  size_t safe_byte_count= digest->m_byte_count;
  if (safe_byte_count > 0 &&
      safe_byte_count <= pfs_max_digest_length)
  {
    /* Generate the DIGEST string from the MD5 digest  */
    MD5_HASH_TO_STRING(digest->m_md5,
                       m_row.m_digest.m_digest);
    m_row.m_digest.m_digest_length= MD5_HASH_TO_STRING_LENGTH;

    /* Generate the DIGEST_TEXT string from the token array */
    compute_digest_text(digest, &m_row.m_digest.m_digest_text);

    if (m_row.m_digest.m_digest_text.length() == 0)
      m_row.m_digest.m_digest_length= 0;
  }
  else
  {
    m_row.m_digest.m_digest_length= 0;
    m_row.m_digest.m_digest_text.length(0);
  }

  return;
}

int table_events_statements_common::read_row_values(TABLE *table,
                                                    unsigned char *buf,
                                                    Field **fields,
                                                    bool read_all)
{
  Field *f;
  uint len;

  if (unlikely(! m_row_exists))
    return HA_ERR_RECORD_DELETED;

  /* Set the null bits */
  assert(table->s->null_bytes == 3);
  buf[0]= 0;
  buf[1]= 0;
  buf[2]= 0;

  for (; (f= *fields) ; fields++)
  {
    if (read_all || bitmap_is_set(table->read_set, f->field_index))
    {
      switch(f->field_index)
      {
      case 0: /* THREAD_ID */
        set_field_ulonglong(f, m_row.m_thread_internal_id);
        break;
      case 1: /* EVENT_ID */
        set_field_ulonglong(f, m_row.m_event_id);
        break;
      case 2: /* END_EVENT_ID */
        if (m_row.m_end_event_id > 0)
          set_field_ulonglong(f, m_row.m_end_event_id - 1);
        else
          f->set_null();
        break;
      case 3: /* EVENT_NAME */
        set_field_varchar_utf8(f, m_row.m_name, m_row.m_name_length);
        break;
      case 4: /* SOURCE */
        set_field_varchar_utf8(f, m_row.m_source, m_row.m_source_length);
        break;
      case 5: /* TIMER_START */
        if (m_row.m_timer_start != 0)
          set_field_ulonglong(f, m_row.m_timer_start);
        else
          f->set_null();
        break;
      case 6: /* TIMER_END */
        if (m_row.m_timer_end != 0)
          set_field_ulonglong(f, m_row.m_timer_end);
        else
          f->set_null();
        break;
      case 7: /* TIMER_WAIT */
        if (m_row.m_timer_wait != 0)
          set_field_ulonglong(f, m_row.m_timer_wait);
        else
          f->set_null();
        break;
      case 8: /* LOCK_TIME */
        if (m_row.m_lock_time != 0)
          set_field_ulonglong(f, m_row.m_lock_time);
        else
          f->set_null();
        break;
      case 9: /* SQL_TEXT */
        if (m_row.m_sqltext.length())
          set_field_longtext_utf8(f, m_row.m_sqltext.ptr(), m_row.m_sqltext.length());
        else
          f->set_null();
        break;
      case 10: /* DIGEST */
        if (m_row.m_digest.m_digest_length > 0)
          set_field_varchar_utf8(f, m_row.m_digest.m_digest,
                                 m_row.m_digest.m_digest_length);
        else
          f->set_null();
        break;
      case 11: /* DIGEST_TEXT */
        if (m_row.m_digest.m_digest_text.length() > 0)
           set_field_longtext_utf8(f, m_row.m_digest.m_digest_text.ptr(),
                                   m_row.m_digest.m_digest_text.length());
        else
          f->set_null();
        break;
      case 12: /* CURRENT_SCHEMA */
        if (m_row.m_current_schema_name_length)
          set_field_varchar_utf8(f, m_row.m_current_schema_name,
                                 m_row.m_current_schema_name_length);
        else
          f->set_null();
        break;
     case 13: /* OBJECT_TYPE */
        if (m_row.m_object_name_length > 0)
          set_field_object_type(f, m_row.m_object_type);
        else
          f->set_null();
        break;
      case 14: /* OBJECT_SCHEMA */
        if (m_row.m_schema_name_length)
          set_field_varchar_utf8(f, m_row.m_schema_name,
                                 m_row.m_schema_name_length);
        else
          f->set_null();
        break;
      case 15: /* OBJECT_NAME */
        if (m_row.m_object_name_length)
          set_field_varchar_utf8(f, m_row.m_object_name,
                                 m_row.m_object_name_length);
        else
          f->set_null();
        break;
      case 16: /* OBJECT_INSTANCE_BEGIN */
        f->set_null();
        break;
      case 17: /* MYSQL_ERRNO */
        set_field_ulong(f, m_row.m_sql_errno);
        break;
      case 18: /* RETURNED_SQLSTATE */
        if (m_row.m_sqlstate[0] != 0)
          set_field_varchar_utf8(f, m_row.m_sqlstate, SQLSTATE_LENGTH);
        else
          f->set_null();
        break;
      case 19: /* MESSAGE_TEXT */
        len= static_cast<uint>(strlen(m_row.m_message_text));
        if (len)
          set_field_varchar_utf8(f, m_row.m_message_text, len);
        else
          f->set_null();
        break;
      case 20: /* ERRORS */
        set_field_ulonglong(f, m_row.m_error_count);
        break;
      case 21: /* WARNINGS */
        set_field_ulonglong(f, m_row.m_warning_count);
        break;
      case 22: /* ROWS_AFFECTED */
        set_field_ulonglong(f, m_row.m_rows_affected);
        break;
      case 23: /* ROWS_SENT */
        set_field_ulonglong(f, m_row.m_rows_sent);
        break;
      case 24: /* ROWS_EXAMINED */
        set_field_ulonglong(f, m_row.m_rows_examined);
        break;
      case 25: /* CREATED_TMP_DISK_TABLES */
        set_field_ulonglong(f, m_row.m_created_tmp_disk_tables);
        break;
      case 26: /* CREATED_TMP_TABLES */
        set_field_ulonglong(f, m_row.m_created_tmp_tables);
        break;
      case 27: /* SELECT_FULL_JOIN */
        set_field_ulonglong(f, m_row.m_select_full_join);
        break;
      case 28: /* SELECT_FULL_RANGE_JOIN */
        set_field_ulonglong(f, m_row.m_select_full_range_join);
        break;
      case 29: /* SELECT_RANGE */
        set_field_ulonglong(f, m_row.m_select_range);
        break;
      case 30: /* SELECT_RANGE_CHECK */
        set_field_ulonglong(f, m_row.m_select_range_check);
        break;
      case 31: /* SELECT_SCAN */
        set_field_ulonglong(f, m_row.m_select_scan);
        break;
      case 32: /* SORT_MERGE_PASSES */
        set_field_ulonglong(f, m_row.m_sort_merge_passes);
        break;
      case 33: /* SORT_RANGE */
        set_field_ulonglong(f, m_row.m_sort_range);
        break;
      case 34: /* SORT_ROWS */
        set_field_ulonglong(f, m_row.m_sort_rows);
        break;
      case 35: /* SORT_SCAN */
        set_field_ulonglong(f, m_row.m_sort_scan);
        break;
      case 36: /* NO_INDEX_USED */
        set_field_ulonglong(f, m_row.m_no_index_used);
        break;
      case 37: /* NO_GOOD_INDEX_USED */
        set_field_ulonglong(f, m_row.m_no_good_index_used);
        break;
      case 38: /* NESTING_EVENT_ID */
        if (m_row.m_nesting_event_id != 0)
          set_field_ulonglong(f, m_row.m_nesting_event_id);
        else
          f->set_null();
        break;
      case 39: /* NESTING_EVENT_TYPE */
        if (m_row.m_nesting_event_id != 0)
          set_field_enum(f, m_row.m_nesting_event_type);
        else
          f->set_null();
        break;
      case 40: /* NESTING_EVENT_LEVEL */
          set_field_ulong(f, m_row.m_nesting_event_level);
        break;
      default:
        assert(false);
      }
    }
  }
  return 0;
}

PFS_engine_table* table_events_statements_current::create(void)
{
  return new table_events_statements_current();
}

table_events_statements_current::table_events_statements_current()
  : table_events_statements_common(&m_share, &m_pos),
  m_pos(), m_next_pos()
{}

void table_events_statements_current::reset_position(void)
{
  m_pos.reset();
  m_next_pos.reset();
}

int table_events_statements_current::rnd_init(bool scan)
{
  return 0;
}

int table_events_statements_current::rnd_next(void)
{
  PFS_thread *pfs_thread;
  PFS_events_statements *statement;
  bool has_more_thread= true;

  for (m_pos.set_at(&m_next_pos);
       has_more_thread;
       m_pos.next_thread())
  {
    pfs_thread= global_thread_container.get(m_pos.m_index_1, & has_more_thread);
    if (pfs_thread != NULL)
    {
      uint safe_events_statements_count= pfs_thread->m_events_statements_count;

      if (safe_events_statements_count == 0)
      {
        /* Display the last top level statement, when completed */
        if (m_pos.m_index_2 >= 1)
          continue;
      }
      else
      {
        /* Display all pending statements, when in progress */
        if (m_pos.m_index_2 >= safe_events_statements_count)
          continue;
      }

      statement= &pfs_thread->m_statement_stack[m_pos.m_index_2];

      make_row(pfs_thread, statement);
      m_next_pos.set_after(&m_pos);
      return 0;
    }
  }

  return HA_ERR_END_OF_FILE;
}

int table_events_statements_current::rnd_pos(const void *pos)
{
  PFS_thread *pfs_thread;
  PFS_events_statements *statement;

  set_position(pos);

  pfs_thread= global_thread_container.get(m_pos.m_index_1);
  if (pfs_thread != NULL)
  {
    uint safe_events_statements_count= pfs_thread->m_events_statements_count;

    if (safe_events_statements_count == 0)
    {
      /* Display the last top level statement, when completed */
      if (m_pos.m_index_2 >= 1)
        return HA_ERR_RECORD_DELETED;
    }
    else
    {
      /* Display all pending statements, when in progress */
      if (m_pos.m_index_2 >= safe_events_statements_count)
        return HA_ERR_RECORD_DELETED;
    }

    assert(m_pos.m_index_2 < statement_stack_max);

    statement= &pfs_thread->m_statement_stack[m_pos.m_index_2];

    if (statement->m_event.m_class)
    {
      make_row(pfs_thread, statement);
      return 0;
    }
  }

  return HA_ERR_RECORD_DELETED;
}

void table_events_statements_current::make_row(PFS_thread *pfs_thread,
                                               PFS_events_statements *statement)
{
  sql_digest_storage digest;
  pfs_optimistic_state lock;
  pfs_optimistic_state stmt_lock;

  digest.reset(m_token_array, MAX_DIGEST_STORAGE_SIZE);
  /* Protect this reader against thread termination. */
  pfs_thread->m_lock.begin_optimistic_lock(&lock);
  /* Protect this reader against writing on statement information. */
  pfs_thread->m_stmt_lock.begin_optimistic_lock(&stmt_lock);

  table_events_statements_common::make_row_part_1(statement, &digest);

  if (!pfs_thread->m_stmt_lock.end_optimistic_lock(&stmt_lock) ||
      !pfs_thread->m_lock.end_optimistic_lock(&lock))
  {
    m_row_exists= false;
    return;
  }
  table_events_statements_common::make_row_part_2(&digest);
  return;
}

int table_events_statements_current::delete_all_rows(void)
{
  reset_events_statements_current();
  return 0;
}

ha_rows
table_events_statements_current::get_row_count(void)
{
  return global_thread_container.get_row_count() * statement_stack_max;
}

PFS_engine_table* table_events_statements_history::create(void)
{
  return new table_events_statements_history();
}

table_events_statements_history::table_events_statements_history()
  : table_events_statements_common(&m_share, &m_pos),
  m_pos(), m_next_pos()
{}

void table_events_statements_history::reset_position(void)
{
  m_pos.reset();
  m_next_pos.reset();
}

int table_events_statements_history::rnd_init(bool scan)
{
  return 0;
}

int table_events_statements_history::rnd_next(void)
{
  PFS_thread *pfs_thread;
  PFS_events_statements *statement;
  bool has_more_thread= true;

  if (events_statements_history_per_thread == 0)
    return HA_ERR_END_OF_FILE;

  for (m_pos.set_at(&m_next_pos);
       has_more_thread;
       m_pos.next_thread())
  {
    pfs_thread= global_thread_container.get(m_pos.m_index_1, & has_more_thread);
    if (pfs_thread != NULL)
    {
      if (m_pos.m_index_2 >= events_statements_history_per_thread)
      {
        /* This thread does not have more (full) history */
        continue;
      }

      if ( ! pfs_thread->m_statements_history_full &&
          (m_pos.m_index_2 >= pfs_thread->m_statements_history_index))
      {
        /* This thread does not have more (not full) history */
        continue;
      }

      statement= &pfs_thread->m_statements_history[m_pos.m_index_2];

      if (statement->m_event.m_class)
      {
        make_row(pfs_thread, statement);
        /* Next iteration, look for the next history in this thread */
        m_next_pos.set_after(&m_pos);
        return 0;
      }
    }
  }

  return HA_ERR_END_OF_FILE;
}

int table_events_statements_history::rnd_pos(const void *pos)
{
  PFS_thread *pfs_thread;
  PFS_events_statements *statement;

  assert(events_statements_history_per_thread != 0);
  set_position(pos);

  pfs_thread= global_thread_container.get(m_pos.m_index_1);
  if (pfs_thread != NULL)
  {
    assert(m_pos.m_index_2 < events_statements_history_per_thread);

    if ( ! pfs_thread->m_statements_history_full &&
        (m_pos.m_index_2 >= pfs_thread->m_statements_history_index))
      return HA_ERR_RECORD_DELETED;

    statement= &pfs_thread->m_statements_history[m_pos.m_index_2];
    if (statement->m_event.m_class)
    {
      make_row(pfs_thread, statement);
      return 0;
    }
  }

  return HA_ERR_RECORD_DELETED;
}

void table_events_statements_history::make_row(PFS_thread *pfs_thread,
                                               PFS_events_statements *statement)
{
  sql_digest_storage digest;
  pfs_optimistic_state lock;

  digest.reset(m_token_array, MAX_DIGEST_STORAGE_SIZE);
  /* Protect this reader against thread termination. */
  pfs_thread->m_lock.begin_optimistic_lock(&lock);

  table_events_statements_common::make_row_part_1(statement, &digest);

  if (!pfs_thread->m_lock.end_optimistic_lock(&lock))
  {
    m_row_exists= false;
    return;
  }
  table_events_statements_common::make_row_part_2(&digest);
  return;
}


int table_events_statements_history::delete_all_rows(void)
{
  reset_events_statements_history();
  return 0;
}

ha_rows
table_events_statements_history::get_row_count(void)
{
  return events_statements_history_per_thread * global_thread_container.get_row_count();
}

PFS_engine_table* table_events_statements_history_long::create(void)
{
  return new table_events_statements_history_long();
}

table_events_statements_history_long::table_events_statements_history_long()
  : table_events_statements_common(&m_share, &m_pos),
  m_pos(0), m_next_pos(0)
{}

void table_events_statements_history_long::reset_position(void)
{
  m_pos.m_index= 0;
  m_next_pos.m_index= 0;
}

int table_events_statements_history_long::rnd_init(bool scan)
{
  return 0;
}

int table_events_statements_history_long::rnd_next(void)
{
  PFS_events_statements *statement;
  uint limit;

  if (events_statements_history_long_size == 0)
    return HA_ERR_END_OF_FILE;

  if (events_statements_history_long_full)
    limit= static_cast<uint>(events_statements_history_long_size);
  else
    limit= events_statements_history_long_index.m_u32 % events_statements_history_long_size;

  for (m_pos.set_at(&m_next_pos); m_pos.m_index < limit; m_pos.next())
  {
    statement= &events_statements_history_long_array[m_pos.m_index];

    if (statement->m_event.m_class)
    {
      make_row(statement);
      /* Next iteration, look for the next entry */
      m_next_pos.set_after(&m_pos);
      return 0;
    }
  }

  return HA_ERR_END_OF_FILE;
}

int table_events_statements_history_long::rnd_pos(const void *pos)
{
  PFS_events_statements *statement;
  uint limit;

  if (events_statements_history_long_size == 0)
    return HA_ERR_RECORD_DELETED;

  set_position(pos);

  if (events_statements_history_long_full)
    limit= static_cast<uint>(events_statements_history_long_size);
  else
    limit= events_statements_history_long_index.m_u32 % events_statements_history_long_size;

  if (m_pos.m_index >= limit)
    return HA_ERR_RECORD_DELETED;

  statement= &events_statements_history_long_array[m_pos.m_index];

  if (!statement->m_event.m_class)
    return HA_ERR_RECORD_DELETED;

  make_row(statement);
  return 0;
}

void table_events_statements_history_long::make_row(PFS_events_statements *statement)
{
  sql_digest_storage digest;

  digest.reset(m_token_array, MAX_DIGEST_STORAGE_SIZE);
  table_events_statements_common::make_row_part_1(statement, &digest);

  table_events_statements_common::make_row_part_2(&digest);
  return;
}

int table_events_statements_history_long::delete_all_rows(void)
{
  reset_events_statements_history_long();
  return 0;
}

ha_rows
table_events_statements_history_long::get_row_count(void)
{
  return events_statements_history_long_size;
}

