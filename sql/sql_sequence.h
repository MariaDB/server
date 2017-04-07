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

/**
   sequence_definition is used when defining a sequence as part of create
*/

class sequence_definition :public Sql_alloc
{
public:
  sequence_definition():
    min_value(1), max_value(LONGLONG_MAX-1), start(1), increment(1),
    cache(1000), round(0), cycle(0), used_fields(0)
  {}
  longlong reserved_until;
  longlong min_value;
  longlong max_value;
  longlong start;
  longlong increment;
  longlong cache;
  ulonglong round;
  bool     cycle;
  uint used_fields;              // Which fields where used in CREATE

  bool check_and_adjust();
  void store_fields(TABLE *table);
  void read_fields(TABLE *table);
  void print_dbug()
  {
    DBUG_PRINT("sequence", ("reserved: %lld  start: %lld  increment: %lld  min_value: %lld  max_value: %lld  cache: %lld  round: %lld",
                      reserved_until, start, increment, min_value,
                        max_value, cache, round));
  }
};

/**
  SEQUENCE is in charge of managing the sequence values.
  It's also responsible to generate new values and updating the sequence
  table (engine=SQL_SEQUENCE) trough it's specialized handler interface.

  If increment is 0 then the sequence will be be using
  auto_increment_increment and auto_increment_offset variables, just like
  AUTO_INCREMENT is using.
*/

class SEQUENCE :public sequence_definition
{
public:
  SEQUENCE();
  ~SEQUENCE();
  int  read_initial_values(TABLE *table);
  int  read_stored_values();
  void lock()
  {
    mysql_mutex_lock(&mutex);
  }
  void unlock()
  {
    mysql_mutex_unlock(&mutex);
  }
  /* This must be called after sequence data has been updated */
  void adjust_values();
  void copy(sequence_definition *seq)
  {
    sequence_definition::operator= (*seq);
    adjust_values();
  }
  longlong next_value(TABLE *table, bool second_round, int *error);

  bool initialized;                         // If row has been read
  bool all_values_used;
private:
  TABLE         *table;
  mysql_mutex_t mutex;
  longlong next_free_value;
  /*
    The following values are the values from sequence_definition
    merged with global auto_increment_offset and auto_increment_increment
  */
  longlong real_increment;
  longlong offset;
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


class Create_field;
extern bool prepare_sequence_fields(THD *thd, List<Create_field> *fields);
extern bool check_sequence_fields(LEX *lex, List<Create_field> *fields);
extern bool sequence_insert(THD *thd, LEX *lex, TABLE_LIST *table_list);
#endif /* SQL_SEQUENCE_INCLUDED */
