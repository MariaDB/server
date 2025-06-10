/*
   Copyright (c) 2006, 2010, Oracle and/or its affiliates.

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

#ifndef RPL_UTILITY_H
#define RPL_UTILITY_H

#ifndef __cplusplus
#error "Don't include this C++ header file from a non-C++ file!"
#endif

#include "sql_priv.h"
#include "m_string.h"                           /* bzero, memcpy */
#ifdef MYSQL_SERVER
#include "table.h"                              /* TABLE_LIST */
#endif
#include "mysql_com.h"
#include "log_event.h"

class Relay_log_info;
class Log_event;
struct rpl_group_info;
struct RPL_TABLE_LIST;

/**
  A table definition from the master.

  The responsibilities of this class is:
  - Extract and decode table definition data from the table map event
  - Check if table definition in table map is compatible with table
    definition on slave
 */

class table_def
{
public:
  /**
    Constructor.

    @param types Array of types, each stored as a byte
    @param size  Number of elements in array 'types'
    @param field_metadata Array of extra information about fields
    @param metadata_size Size of the field_metadata array
    @param null_bitmap The bitmap of fields that can be null
    @param optional_metadata_len Length of optional_metadata
    @param optional_metadata Optional metadata logged into Table Map Event
                             when binlog_row_metadata=FULL on master
   */
  table_def(unsigned char *types, ulong size, uchar *field_metadata,
            int metadata_size, uchar *null_bitmap, uint16 flags,
            unsigned int optional_metadata_len,
            unsigned char *optional_metadata);

  ~table_def();

  /**
    Return the number of fields there is type data for.

    @return The number of fields that there is type data for.
   */
  ulong size() const { return m_size; }


  /**
    Returns internal binlog type code for one field,
    without translation to real types.
  */
  enum_field_types binlog_type(ulong index) const
  {
    return static_cast<enum_field_types>(m_type[index]);
  }
  /*
    Return a representation of the type data for one field.

    @param index Field index to return data for

    @return Will return a representation of the type data for field
    <code>index</code>. Currently, only the type identifier is
    returned.
   */
  enum_field_types type(ulong index) const
  {
    DBUG_ASSERT(index < m_size);
    /*
      If the source type is MYSQL_TYPE_STRING, it can in reality be
      either MYSQL_TYPE_STRING, MYSQL_TYPE_ENUM, or MYSQL_TYPE_SET, so
      we might need to modify the type to get the real type.
    */
    enum_field_types source_type= binlog_type(index);
    uint16 source_metadata= m_field_metadata[index];
    switch (source_type)
    {
    case MYSQL_TYPE_STRING:
    {
      int real_type= source_metadata >> 8;
      if (real_type == MYSQL_TYPE_ENUM || real_type == MYSQL_TYPE_SET)
        source_type= static_cast<enum_field_types>(real_type);
      break;
    }

    /*
      This type has not been used since before row-based replication,
      so we can safely assume that it really is MYSQL_TYPE_NEWDATE.
    */
    case MYSQL_TYPE_DATE:
      source_type= MYSQL_TYPE_NEWDATE;
      break;

    default:
      /* Do nothing */
      break;
    }

    return source_type;
  }
#ifdef MYSQL_SERVER
  const Type_handler *field_type_handler(uint index) const;
#endif

  /*
    This function allows callers to get the extra field data from the
    table map for a given field. If there is no metadata for that field
    or there is no extra metadata at all, the function returns 0.

    The function returns the value for the field metadata for column at 
    position indicated by index. As mentioned, if the field was a type 
    that stores field metadata, that value is returned else zero (0) is 
    returned. This method is used in the unpack() methods of the 
    corresponding fields to properly extract the data from the binary log 
    in the event that the master's field is smaller than the slave.
  */
  uint16 field_metadata(uint index) const
  {
    DBUG_ASSERT(index < m_size);
    if (m_field_metadata_size)
      return m_field_metadata[index];
    else
      return 0;
  }

  /*
    This function returns whether the field on the master can be null.
    This value is derived from field->maybe_null().
  */
  my_bool maybe_null(uint index) const
  {
    DBUG_ASSERT(index < m_size);
    return ((m_null_bits[(index / 8)] & 
            (1 << (index % 8))) == (1 << (index %8)));
  }

  unsigned char *get_optional_metadata_str()
  {
    return m_optional_metadata;
  }

  unsigned int get_optional_metadata_len()
  {
    return m_optional_metadata_len;
  }

  /*
    This function returns the field size in raw bytes based on the type
    and the encoded field data from the master's raw data. This method can 
    be used for situations where the slave needs to skip a column (e.g., 
    WL#3915) or needs to advance the pointer for the fields in the raw 
    data from the master to a specific column.
  */
  uint32 calc_field_size(uint col, uchar *master_data) const;

  /**
    Decide if the table definition is compatible with a table.

    Compare the definition with a table to see if it is compatible
    with it.

    A table definition is compatible with a table if:
      - The columns types of the table definition is a (not
        necessarily proper) prefix of the column type of the table.

      - The other way around.

      - Each column on the master that also exists on the slave can be
        converted according to the current settings of @c
        SLAVE_TYPE_CONVERSIONS.

    @param thd
    @param rli   Pointer to relay log info
    @param table Pointer to table to compare with.

    @param[out] tmp_table_var Pointer to temporary table for holding
    conversion table.

    @retval 1  if the table definition is not compatible with @c table
    @retval 0  if the table definition is compatible with @c table
  */
#ifndef MYSQL_CLIENT
  bool compatible_with(THD *thd, rpl_group_info *rgi,
                       RPL_TABLE_LIST *table_list,
                       TABLE **conv_table_var) const;

  /**
   Create a virtual in-memory temporary table structure.

   The table structure has records and field array so that a row can
   be unpacked into the record for further processing.

   In the virtual table, each field that requires conversion will
   have a non-NULL value, while fields that do not require
   conversion will have a NULL value.

   Some information that is missing in the events, such as the
   character set for string types, are taken from the table that the
   field is going to be pushed into, so the target table that the data
   eventually need to be pushed into need to be supplied.

   Note that the fields generated in the conversion table are not guaranteed to
   align with the fields from this table_def. If the slave doesn't have the
   target field, we don't generate a field in the conversion_table, as it would
   serve no purpose. If the conversion table is referenced while iterating
   through this table_def, one needs a separate index to keep track of the
   conv_table fields, which are only incremented when the slave has that
   column. This can be checked by using RPL_TABLE_LIST::lookup_slave_column().
   See other member function of table_def compatible_with() for an example of
   this.

   @param thd Thread to allocate memory from.
   @param rli Relay log info structure, for error reporting.
   @param target_table Target table for fields.

   @return A pointer to a temporary table with memory allocated in the
   thread's memroot, NULL if the table could not be created
   */
  TABLE *create_conversion_table(THD *thd, rpl_group_info *rgi,
                                 RPL_TABLE_LIST *target_table_list_el) const;
#endif


private:
  ulong m_size;           // Number of elements in the types array
  unsigned char *m_type;  // Array of type descriptors
  uint m_field_metadata_size;
  uint16 *m_field_metadata;
  uchar *m_null_bits;
  uint16 m_flags;         // Table flags
  uchar *m_memory;
  unsigned int   m_optional_metadata_len;
  unsigned char *m_optional_metadata;
};


#ifndef MYSQL_CLIENT
/**
   Extend the normal table list with a few new fields needed by the
   slave thread, but nowhere else.
 */
struct RPL_TABLE_LIST
  : public TABLE_LIST
{
  bool m_tabledef_valid;
  table_def m_tabledef;
  TABLE *m_conv_table;
  bool master_had_triggers;

  /*
    Maps column index from master to slave. This is determined using the field
    names (provied by optional metadata when the master is configured with
    binlog_row_metadata=FULL).
  */
  std::map<uint, uint> master_to_slave_index_map;

  /*
    When using field names to map from master->slave columns, this keeps track
    of column indices on the master which aren't present on the slave.

    It is used to skip columns when checking type-conversions and unpacking
    row data.

    TODO: As-is, we don't actually need this anymore, we can just lookup if the
        column exists in master_to_slave_index_map. But, if we want to switch
        master_to_slave_index_map to be an array, then we'd still need this, as
        we couldn't test inclusivity in an array; however, we could make it a
        bitset for less overhead (alternatively, have the
        master_to_slave_index_map use '-1' to indicate missing; though that
        would probably induce more memory footprint than a bitmap here.)
  */
  std::set<uint> master_unmatched_cols;

  /*
    If field names are to be used to map columns from the master to slave, this
    tracks whether the respective data structures (master_to_slave_index_map
    and master_unmatched_cols) have been initialized, so we can destruct them.
  */
  bool master_to_slave_structs_inited;

  /*
    Function pointer that is configured to look up columns on the slave-side
    table. Options are
      1. Use a 1-to-1 mapping from numeric master column index number to slave
         column index number (function lookup_by_identity_func)
      2. Use the field name map, master_to_slave_index_map, to lookup the slave
         column index number (function lookup_by_col_mapping)

    Parameters
      master_idx [in]     : The column index of the table on the master
      slave_idx_var [out] : The variable to store the slave index number in

    Return based on error-code semantics:
      false indicates a column was found,
      true indicates that the record was not found
  */
  bool (RPL_TABLE_LIST::*lookup_slave_column_func)(uint master_idx, uint *slave_idx_var);

  /*
    Try to create column mappings using field names (i.e. when the master
    binlogged using binlog_row_metadata=FULL).
  */
  bool create_column_mappings(unsigned char *optional_metadata_raw,
                              unsigned int optional_metadata_len);

  /*
    Implementation for lookup_slave_column_func which uses field names to
    identify which slave column matches the master column.
  */
  bool lookup_by_col_mapping(uint master_col_idx, uint *slave_col_idx_var);

  /*
    Initialize state to prepare data structures and helper functions to lookup
    slave column indices by field_name
  */
  bool init_master_to_slave_structs()
  {
    if (!master_to_slave_structs_inited)
    {
      new (&master_to_slave_index_map) std::map<uint, uint>();
      new (&master_unmatched_cols) std::set<uint>();
      lookup_slave_column_func= &RPL_TABLE_LIST::lookup_by_col_mapping;
      master_to_slave_structs_inited= true;
    }
    return false;
  }

  /*
    Finds the slave-side column index for a column in a row event from the
    master. A function pointer, lookup_slave_column_func, keeps track of the
    correct strategy for this given table. That is, when a row event is logged
    on the master using binlog_row_metadata=FULL, it will use function
    lookup_by_col_mapping so we can lookup by field name. Otherwise, it will
    use lookup_by_identity_func, and assume the indices are ordered the same
    between the master and slave.
  */
  bool lookup_slave_column(uint master_col_idx, uint *slave_col_idx_var)
  {
    DBUG_ASSERT(slave_col_idx_var);
    return (this->*lookup_slave_column_func)(master_col_idx, slave_col_idx_var);
  }

  /*
    One implementation for lookup_slave_column_func which assumes master and
    slave have columns in the same ordering, and thereby says the slave
    column index is the same as the master index (identity function). The
    exception is if the master index extends beyond the number of fields on
    the slave table, in which case we return indicating the column is not
    found.
  */
  bool lookup_by_identity_func(uint master_col_idx, uint *slave_col_idx_var)
  {
    DBUG_ASSERT(slave_col_idx_var);
    /*
      table->s->fields is a count, whereas master_col_idx is an index (0 based)
      so we have to account for the 0-based index.
    */
    if (master_col_idx >= table->s->fields)
      return true;
    *slave_col_idx_var= master_col_idx;
    return false;
  }
};


/* Anonymous namespace for template functions/classes */
CPP_UNNAMED_NS_START

  /*
    Smart pointer that will automatically call my_afree (a macro) when
    the pointer goes out of scope.  This is used so that I do not have
    to remember to call my_afree() before each return.  There is no
    overhead associated with this, since all functions are inline.

    I (Matz) would prefer to use the free function as a template
    parameter, but that is not possible when the "function" is a
    macro.
  */
  template <class Obj>
  class auto_afree_ptr
  {
    Obj* m_ptr;
  public:
    auto_afree_ptr(Obj* ptr) : m_ptr(ptr) { }
    ~auto_afree_ptr() { if (m_ptr) my_afree(m_ptr); }
    void assign(Obj* ptr) {
      /* Only to be called if it hasn't been given a value before. */
      DBUG_ASSERT(m_ptr == NULL);
      m_ptr= ptr;
    }
    Obj* get() { return m_ptr; }
  };

CPP_UNNAMED_NS_END

class Deferred_log_events
{
private:
  DYNAMIC_ARRAY array;
  Log_event *last_added;

public:
  Deferred_log_events(Relay_log_info *rli);
  ~Deferred_log_events();
  /* queue for exection at Query-log-event time prior the Query */
  int add(Log_event *ev);
  bool is_empty();
  bool execute(struct rpl_group_info *rgi);
  void rewind();
  bool is_last(Log_event *ev) { return ev == last_added; };
};

#endif

// NB. number of printed bit values is limited to sizeof(buf) - 1
#define DBUG_PRINT_BITSET(N,FRM,BS)                \
  do {                                             \
    char buf[256];                                 \
    uint i;                                        \
    for (i = 0 ; i < MY_MIN(sizeof(buf) - 1, (BS)->n_bits) ; i++) \
      buf[i] = bitmap_is_set((BS), i) ? '1' : '0'; \
    buf[i] = '\0';                                 \
    DBUG_PRINT((N), ((FRM), buf));                 \
  } while (0)

#endif /* RPL_UTILITY_H */
