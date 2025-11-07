/* Copyright (c) 2017, MariaDB corporation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#ifndef SQL_SEQUENCE_INCLUDED
#define SQL_SEQUENCE_INCLUDED

#define seq_field_used_min_value 1
#define seq_field_used_max_value 2
#define seq_field_used_start     4
#define seq_field_used_increment 8
#define seq_field_used_cache     16
#define seq_field_used_cycle     32
#define seq_field_used_restart   64
#define seq_field_used_restart_value 128
#define seq_field_used_as 256
#define seq_field_specified_min_value 512
#define seq_field_specified_max_value 1024

/* Field position in sequence table for some fields we refer to directly */
#define NEXT_FIELD_NO 0
#define MIN_VALUE_FIELD_NO 1
#define ROUND_FIELD_NO 7

#include "mysql_com.h"
#include "sql_type_int.h"

class Create_field;
class Type_handler;

struct Sequence_field_definition
{
  const char *field_name;
  uint length;
  const Type_handler *type_handler;
  LEX_CSTRING comment;
  ulong flags;
};

struct Sequence_row_definition
{
  Sequence_field_definition fields[9];
};

/**
   sequence_definition is used when defining a sequence as part of create
*/

class sequence_definition :public Sql_alloc
{
public:
  sequence_definition():
    min_value_from_parser(1, false),
    max_value_from_parser(LONGLONG_MAX-1, false), start_from_parser(1, false),
    increment(1), cache(1000), round(0), restart_from_parser(0, false),
    cycle(0), used_fields(0),
    /*
      We use value type and is_unsigned instead of a handler because
      Type_handler is incomplete, which we cannot initialise here with
      &type_handler_slonglong.
    */
    value_type(MYSQL_TYPE_LONGLONG), is_unsigned(false)
  {}
  longlong reserved_until;
  longlong min_value;
  longlong max_value;
  longlong start;
  Longlong_hybrid min_value_from_parser;
  Longlong_hybrid max_value_from_parser;
  Longlong_hybrid start_from_parser;
  longlong increment;
  longlong cache;
  ulonglong round;
  longlong restart;              // alter sequence restart value
  Longlong_hybrid restart_from_parser;
  bool     cycle;
  uint used_fields;              // Which fields where used in CREATE
  enum_field_types value_type;    // value type of the sequence
  bool     is_unsigned;

  Type_handler const *value_type_handler();
  /* max value for the value type, e.g. 32767 for smallint. */
  longlong value_type_max();
  /* min value for the value type, e.g. -32768 for smallint. */
  longlong value_type_min();
  bool check_and_adjust(THD *thd, bool set_reserved_until,
                        bool adjust_next= true);
  void store_fields(TABLE *table);
  void read_fields(TABLE *table);
  int write_initial_sequence(TABLE *table);
  int write(TABLE *table, bool all_fields);
  /* This must be called after sequence data has been updated */
  void adjust_values(longlong next_value);
  longlong truncate_value(const Longlong_hybrid& original);
  inline void print_dbug()
  {
    DBUG_PRINT("sequence", ("reserved: %lld  start: %lld  increment: %lld  min_value: %lld  max_value: %lld  cache: %lld  round: %lld",
                      reserved_until, start, increment, min_value,
                        max_value, cache, round));
  }
  static bool is_allowed_value_type(enum_field_types type);
  bool prepare_sequence_fields(List<Create_field> *fields, bool alter);
  
protected:
  /*
    The following values are the values from sequence_definition
    merged with global auto_increment_offset and auto_increment_increment
  */
  longlong real_increment;
  longlong next_free_value;
};

/**
  SEQUENCE is in charge of managing the sequence values.
  It's also responsible to generate new values and updating the sequence
  table (engine=SQL_SEQUENCE) trough it's specialized handler interface.

  If increment is 0 then the sequence will be using
  auto_increment_increment and auto_increment_offset variables, just like
  AUTO_INCREMENT is using.
*/

class SEQUENCE :public sequence_definition
{
public:
  enum seq_init { SEQ_UNINTIALIZED, SEQ_IN_PREPARE, SEQ_IN_ALTER,
                  SEQ_READY_TO_USE };
  SEQUENCE();
  ~SEQUENCE();
  int  read_initial_values(TABLE *table);
  int  read_stored_values(TABLE *table);
  void write_lock(TABLE *table);
  void write_unlock(TABLE *table);
  void read_lock(TABLE *table);
  void read_unlock(TABLE *table);
  void copy(sequence_definition *seq)
  {
    sequence_definition::operator= (*seq);
    adjust_values(reserved_until);
    all_values_used= 0;
  }
  longlong next_value(TABLE *table, bool second_round, int *error);
  int set_value(TABLE *table, longlong next_value, ulonglong round_arg,
                bool is_used);
  bool has_run_out()
  {
    return all_values_used ||
      (!cycle &&
       !within_bound(next_free_value, max_value + 1, min_value - 1,
                     real_increment > 0));
  }

  bool all_values_used;
  seq_init initialized;

private:
  /**
    Check that a value is within a relevant bound

    If increasing sequence, check that the value is lower than an
    upper bound, otherwise check that the value is higher than a lower
    bound.

    @param in value       The value to check
    @param in upper       The upper bound
    @param in lower       The lower bound
    @param in increasing  Which bound to check

    @retval   true        The value is within the bound.
              false       The value is out of the bound.
  */
  bool within_bound(const longlong value, const longlong upper,
                    const longlong lower, bool increasing)
  {
    return
      (is_unsigned && increasing && (ulonglong) value < (ulonglong) upper) ||
      (is_unsigned && !increasing && (ulonglong) value > (ulonglong) lower) ||
      (!is_unsigned && increasing && value < upper) ||
      (!is_unsigned && !increasing && value > lower);
  }

  /**
    Increment a value, subject to truncation

    Truncating to the nearer value between max_value + 1 and min_value
    - 1

    @param in value      The original value
    @param in increment  The increment to add to the value

    @return              The value after increment and possible truncation
  */
  longlong increment_value(longlong value, const longlong increment)
  {
    if (is_unsigned)
    {
      if (increment > 0)
        {
          if (/* in case value + increment overflows */
              (ulonglong) value >
              (ulonglong) max_value - (ulonglong) increment ||
              /* in case max_value - increment underflows */
              (ulonglong) value + (ulonglong) increment >
              (ulonglong) max_value ||
              /* in case both overflow and underflow happens (very
              rarely, if not impossible) */
              (ulonglong) value > (ulonglong) max_value)
            /* Cast to ulonglong then back, in case max_value ==
            LONGLONG_MAX as a ulonglong */
            value= (longlong) ((ulonglong) max_value + 1);
          else
            value = (longlong) ((ulonglong) value + (ulonglong) increment);
        }
      else
      {
        if ((ulonglong) value - (ulonglong) (-increment) <
            (ulonglong) min_value ||
            (ulonglong) value <
            (ulonglong) min_value + (ulonglong) (-increment) ||
            (ulonglong) value < (ulonglong) min_value)
          /* Cast to ulonglong then back, in case min_value ==
          LONGLONG_MAX + 1 as a ulonglong */
          value= (longlong) ((ulonglong) min_value - 1);
        else
          value = (longlong) ((ulonglong) value - (ulonglong) (-increment));
      }
    } else
      if (increment > 0)
      {
        if (value >
              (longlong) ((ulonglong) max_value - (ulonglong) increment) ||
            (longlong) ((ulonglong) value + (ulonglong) increment) >
              max_value ||
            value > max_value)
          value= max_value + 1;
        else
          value+= increment;
      }
      else
      {
        if ((longlong) ((ulonglong) value + (ulonglong) increment) <
              min_value ||
            value <
              (longlong) ((ulonglong) min_value - (ulonglong) increment) ||
            value < min_value)
          value= min_value - 1;
        else
          value+= increment;
      }
    return value;
  }
  mysql_rwlock_t mutex;
};


/**
  Class to cache last value of NEXT VALUE from the sequence
*/

class SEQUENCE_LAST_VALUE
{
public:
  SEQUENCE_LAST_VALUE(uchar *key_arg, uint length_arg)
    :key(key_arg), length(length_arg)
  {}
  ~SEQUENCE_LAST_VALUE()
  { my_free((void*) key); }
  /* Returns 1 if table hasn't been dropped or re-created */
  bool check_version(TABLE *table);
  void set_version(TABLE *table);

  const uchar *key;
  uint length;
  bool null_value;
  longlong value;
  uchar table_version[MY_UUID_SIZE];
};

extern bool check_sequence_fields(LEX *lex, List<Create_field> *fields,
                                  const LEX_CSTRING db,
                                  const LEX_CSTRING table_name);
extern bool sequence_insert(THD *thd, LEX *lex, TABLE_LIST *table_list);
#endif /* SQL_SEQUENCE_INCLUDED */
