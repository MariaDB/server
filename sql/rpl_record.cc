/* Copyright (c) 2007, 2013, Oracle and/or its affiliates.
   Copyright (c) 2008, 2022, MariaDB Corporation.

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
#include "unireg.h"
#include "rpl_rli.h"
#include "rpl_record.h"
#include "slave.h"                  // Need to pull in slave_print_msg
#include "rpl_utility.h"
#include "rpl_rli.h"

/*

 This function is used instead of MY_BITMAP for null bits in the
 binary log image. The reason is that MY_BITMAP functions assumes the
 bits are aligned on 8 byte boundaries and all bits on the last 8 bytes
 are accessible. The above is not guaranteed for the row event null bits.
*/

inline bool rpl_bitmap_is_set(const uchar *null_bits, uint col)
{
  return null_bits[col/8] & (1 << (col % 8));
}

/**
   Pack a record of data for a table into a format suitable for
   transfer via the binary log.

   The format for a row in transfer with N fields is the following:

   ceil(N/8) null bytes:
       One null bit for every column *regardless of whether it can be
       null or not*. This simplifies the decoding. Observe that the
       number of null bits is equal to the number of set bits in the
       @c cols bitmap. The number of null bytes is the smallest number
       of bytes necessary to store the null bits.

       Padding bits are 1.

   N packets:
       Each field is stored in packed format.


   @param table    Table describing the format of the record

   @param cols     Bitmap with a set bit for each column that should
                   be stored in the row

   @param row_data Pointer to memory where row will be written

   @param record   Pointer to record that should be packed. It is
                   assumed that the pointer refers to either @c
                   record[0] or @c record[1], but no such check is
                   made since the code does not rely on that.

   @return The number of bytes written at @c row_data.
 */
#if !defined(MYSQL_CLIENT)
size_t
pack_row(TABLE *table, MY_BITMAP const* cols,
         uchar *row_data, const uchar *record)
{
  Field **p_field= table->field, *field;
  int const null_byte_count= (bitmap_bits_set(cols) + 7) / 8;
  uchar *pack_ptr = row_data + null_byte_count;
  uchar *null_ptr = row_data;
  my_ptrdiff_t const rec_offset= record - table->record[0];
  my_ptrdiff_t const def_offset= table->s->default_values - table->record[0];

  DBUG_ENTER("pack_row");

  /*
    We write the null bits and the packed records using one pass
    through all the fields. The null bytes are written little-endian,
    i.e., the first fields are in the first byte.
   */
  unsigned int null_bits= (1U << 8) - 1;
  // Mask to mask out the correct but among the null bits
  unsigned int null_mask= 1U;
  for ( ; (field= *p_field) ; p_field++)
  {
    if (bitmap_is_set(cols, (uint)(p_field - table->field)))
    {
      my_ptrdiff_t offset;
      if (field->is_null(rec_offset))
      {
        offset= def_offset;
        null_bits |= null_mask;
      }
      else
      {
        offset= rec_offset;
        null_bits &= ~null_mask;

        /*
          We only store the data of the field if it is non-null

          For big-endian machines, we have to make sure that the
          length is stored in little-endian format, since this is the
          format used for the binlog.
        */
#ifndef DBUG_OFF
        const uchar *old_pack_ptr= pack_ptr;
#endif
        pack_ptr= field->pack(pack_ptr, field->ptr + offset);
        DBUG_PRINT("debug", ("field: %s; real_type: %d, pack_ptr: %p;"
                             " pack_ptr':%p; bytes: %d",
                             field->field_name.str, field->real_type(),
                             old_pack_ptr,pack_ptr,
                             (int) (pack_ptr - old_pack_ptr)));
        DBUG_DUMP("packed_data", old_pack_ptr, pack_ptr - old_pack_ptr);
      }

      null_mask <<= 1;
      if ((null_mask & 0xFF) == 0)
      {
        DBUG_ASSERT(null_ptr < row_data + null_byte_count);
        null_mask = 1U;
        *null_ptr++ = null_bits;
        null_bits= (1U << 8) - 1;
      }
    }
  }

  /*
    Write the last (partial) byte, if there is one
  */
  if ((null_mask & 0xFF) > 1)
  {
    DBUG_ASSERT(null_ptr < row_data + null_byte_count);
    *null_ptr++ = null_bits;
  }

  /*
    The null pointer should now point to the first byte of the
    packed data. If it doesn't, something is very wrong.
  */
  DBUG_ASSERT(null_ptr == row_data + null_byte_count);
  DBUG_DUMP("row_data", row_data, pack_ptr - row_data);
  DBUG_RETURN(static_cast<size_t>(pack_ptr - row_data));
}
#endif

#if !defined(MYSQL_CLIENT) && defined(HAVE_REPLICATION)

struct Unpack_record_state
{
  uchar const *const row_data;
  uchar const *const row_end;
  size_t const master_null_byte_count;
  uchar const *null_ptr;
  uchar const *pack_ptr;

  /** Current offset in null_ptr */
  unsigned int null_pos;
  Unpack_record_state(uchar const *const row_data,
                      uchar const *const row_end,
                      size_t const master_null_byte_count)
  : row_data(row_data), row_end(row_end),
    master_null_byte_count(master_null_byte_count),
    null_ptr(row_data), pack_ptr(row_data + master_null_byte_count),
    null_pos(0)
  {}
};

/*
  When unpacking a row, if no value was provided for a field (i.e. it is NULL),
  the field needs to be prepared because there may be an existing record in the
  table with data. Nullable fields are reset, and non-null fields are set to
  their default value.
*/
static void prepare_null_field(Field *f, Unpack_record_state *st)
{
  /*
    Ensure that the null bit for the current field is set. One is subtracted
    from the null_pos because the null_pos is incremented before calling into
    this function.
  */
  DBUG_ASSERT(st->null_pos);
  DBUG_ASSERT(rpl_bitmap_is_set(st->null_ptr, st->null_pos - 1));

  if (f->maybe_null())
  {
    /**
      Calling reset just in case one is unpacking on top a
      record with data.

      This could probably go into set_null() but doing so,
      (i) triggers assertion in other parts of the code at
      the moment; (ii) it would make us reset the field,
      always when setting null, which right now doesn't seem
      needed anywhere else except here.

      TODO: maybe in the future we should consider moving
            the reset to make it part of set_null. But then
            the assertions triggered need to be
            addressed/revisited.
     */

#ifndef DBUG_OFF
    /*
      f->reset() may call store_value() to reset the value, for example
      Field_new_decimal. store_value() has below assertion:

      DBUG_ASSERT(marked_for_write_or_computed());

      It asserts write bitmap must be set. That caused an assertion
      failure for row images generated by FULL_NODUP mode.
      The assertion is meaningless for unpacking a row image, so
      the field is marked in write_set temporarily to avoid the
      assertion failure.
    */
    bool was_not_set= !bitmap_is_set(f->table->write_set, f->field_index);
    if (was_not_set)
      bitmap_set_bit(f->table->write_set, f->field_index);
#endif
    f->reset();
#ifndef DBUG_OFF
    if (was_not_set)
      bitmap_clear_bit(f->table->write_set, f->field_index);
#endif
    f->set_null();
  }
  else
  {
    THD *thd= f->table->in_use;

    f->set_default();
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN, ER_BAD_NULL_ERROR,
                        ER_THD(thd, ER_BAD_NULL_ERROR), f->field_name.str);
  }
}

/*
  Unpack the value from a packed row into a field. Field must be non-null.

  Returns true if the field was unpacked successfully, false otherwise.
*/
static bool unpack_field(const table_def *tabledef, Field *f,
                         Unpack_record_state *st, uint field_idx)
{
  DBUG_ASSERT(!f->is_null());
  uint16 const metadata= tabledef->field_metadata(field_idx);
#ifdef DBUG_TRACE
  uchar const *const old_pack_ptr= st->pack_ptr;
#endif

  st->pack_ptr= f->unpack(f->ptr, st->pack_ptr, st->row_end, metadata);
  DBUG_PRINT("debug", ("field: %s; metadata: 0x%x;"
                       " pack_ptr: %p; pack_ptr': %p; bytes: %d",
                       f->field_name.str, metadata, old_pack_ptr, st->pack_ptr,
                       (int) (st->pack_ptr - old_pack_ptr)));

  return static_cast<bool>(st->pack_ptr);
}

/*
  While unpacking a row, if the type of the field on the master is different
  from the type on the slave, convert the value to match the type on the slave.
  That is, the value should be initially unpacked into a conversion table
  field, so here we copy and convert the value from the conversion table field
  to the result field.
*/
static void convert_field(Field *result_field, Field *conv_field)
{
  DBUG_ASSERT(result_field);
  DBUG_ASSERT(conv_field);
#ifndef DBUG_OFF
  char type_buf[MAX_FIELD_WIDTH];
  char value_buf[MAX_FIELD_WIDTH];
  String source_type(type_buf, sizeof(type_buf), system_charset_info);
  String value_string(value_buf, sizeof(value_buf), system_charset_info);
  conv_field->sql_type(source_type);
  conv_field->val_str(&value_string);
  DBUG_PRINT("debug", ("Copying field '%s' of type '%s' with value '%s'",
                       result_field->field_name.str,
                       source_type.c_ptr_safe(), value_string.c_ptr_safe()));
#endif

  Copy_field copy;
  copy.set(result_field, conv_field, TRUE);
  (*copy.do_copy)(&copy);

#ifndef DBUG_OFF
  String target_type(type_buf, sizeof(type_buf), system_charset_info);
  result_field->sql_type(target_type);
  result_field->val_str(&value_string);
  DBUG_PRINT("debug", ("Value of field '%s' of type '%s' is now '%s'",
                       result_field->field_name.str,
                       target_type.c_ptr_safe(), value_string.c_ptr_safe()));
#endif
}

/**
  Updates a table's write_set to include slave-only fields that are
  automatically filled in (either with a default or virtual column value). That
  is, when replicating a rows log event, a table's write_set is initially
  determined by the event's column bitmaps (in the case of an update rows
  event, it is the after_image bitmap). However, if a field isn't present on
  the master, the binlog event's column mapping won't be able to include it; so
  we iterate through a table's fields which will be automatically populated,
  and add them to the write_set.

  @param table           Table to update the write_set for
  @param field_start_ptr Pointer to the first automatically populatable field
                         of the table (e.g. table->default_field or
                         table->vfield).
*/
static void update_write_set_for_auto_filled_fields(TABLE *table,
                                                    Field **field_start_ptr)
{
  DBUG_ENTER("update_write_set_for_auto_filled_fields");
  if (!field_start_ptr || !*field_start_ptr)
    DBUG_VOID_RETURN;

  Field **field_ptr, *field;
  for (field_ptr= field_start_ptr; *field_ptr; ++field_ptr)
  {
    field= *field_ptr;
    /*
      We only want to automatically populate the value of fields which don't
      have values provided by the master; so we check that either no value was
      provided, or the table's original write set accounts for the explicit
      value.
    */
    DBUG_ASSERT(!field->has_explicit_value() ||
                bitmap_is_set(table->write_set, field->field_index));
    if (field->stored_in_db())
      bitmap_set_bit(table->write_set, field->field_index);
  }
  DBUG_VOID_RETURN;
}

/**
   Unpack a row into @c table->record[0].

   The function will always unpack into the @c table->record[0]
   record.  This is because there are too many dependencies on where
   the various member functions of Field and subclasses expect to
   write.

   The row is assumed to only consist of the fields for which the
   corresponding bit in bitset @c cols is set; the other parts of the
   record are left alone.

   At most @c master_cols columns are read: if the table is larger than
   that, the remaining fields are not filled in.

   @note The relay log information can be NULL, which means that no
   checking or comparison with the source table is done, simply
   because it is not used.  This feature is used by MySQL Backup to
   unpack a row from from the backup image, but can be used for other
   purposes as well.

   @param rgi     Relay group info
   @param table   Table to unpack into
   @param master_cols  Number of columns to read from record
   @param row_data
                  Packed row datanull_ptr
   @param cols    Pointer to bitset describing columns to fill in
   @param curr_row_end
                  Pointer to variable that will hold the value of the
                  one-after-end position for the current row
   @param master_reclength
                  Pointer to variable that will be set to the length of the
                  record on the master side
   @param row_end
                  Pointer to variable that will hold the value of the
                  end position for the data in the row event

   @retval 0 No error

   @retval HA_ERR_GENERIC
   A generic, internal, error caused the unpacking to fail.
   @retval HA_ERR_CORRUPT_EVENT
   Found error when trying to unpack fields.
 */

int unpack_row(const rpl_group_info *rgi, TABLE *table, uint const master_cols,
               uchar const *const row_data, MY_BITMAP const *cols,
               uchar const **const current_row_end,
               ulong *const master_reclength, uchar const *const row_end)
{
  int error;
  bool null_value;
  DBUG_ENTER("unpack_row");
  DBUG_ASSERT(row_data);
  DBUG_ASSERT(table);
  DBUG_ASSERT(rgi);

  Unpack_record_state st(row_data, row_end, (bitmap_bits_set(cols) + 7) / 8);

  if (bitmap_is_clear_all(cols))
  {
    /**
       There was no data sent from the master, so there is
       nothing to unpack.
     */
    *current_row_end= st.pack_ptr;
    *master_reclength= 0;
    DBUG_RETURN(0);
  }

  Rpl_table_data rpl_data= *(RPL_TABLE_LIST*)table->pos_in_table_list;
  const table_def *tabledef= rpl_data.tabledef;
  const TABLE *conv_table= rpl_data.conv_table;
  uint conv_table_idx= 0, master_idx= 0;
  DBUG_PRINT("debug", ("Table data: tabldef: %p, conv_table: %p",
                       tabledef, conv_table));

  /*
    A slave needs additional checks when unpacking a row than from the ONLINE
    ALTER use case. The slave must account for its tables having either columns
    in different positions,  or with different types, than on the master.
  */
  if (!rpl_data.is_online_alter())
  {
    Field *result_field= NULL;
    /*
      Two phases:
        1. First, perform sanity checks to see if the value should actually be
           unpacked, i.e. if nothing was binlogged (NULL) or the column doesn't
           exist on the slave. If there is nothing to unpack, we can just skip
           that column; but the unpack state needs to be maintained (i.e.
           pack_ptr and conv_table_idx need to be incremented appropriately), and
           the field must be configured with the correct default value (or NULL).

        2. Unpack the actual value into the slave field with any necessary
           conversions.
    */
    for (; master_idx < master_cols; master_idx++)
    {
      Field *field=NULL, *conv_field= NULL;
      /*

        Check 1: Skip unpacking if the field wasn't written in this record.
        This can happen for update row events when the before_image and
        after_image are disjoint (e.g. when binlogged with
        binlog_row_image=MINIMAL).
      */
      if (!bitmap_is_set(cols, master_idx))
      {

        /*
          Check if we need to update the conv_table_idx. A field is only added
          to the conv_table when it exists on the slave.

          conv_table_idx tracks the index of the field in the conversion
          table, but
        */
        if (!(tabledef->master_to_slave_error[master_idx])) // field exists on slave
        {
          /*
            Review-only-comment: Removed this assertion becaues it assumes that
            the table->write_set is a superset of the read and write sets, but
            it isn't. When an update row event is binlogged with
            binlog_row_image=MINIMAL, the read and write sets can be disjoint.
            As this is is only used for debug assertions, I decided to remove
            the assertion altogether, rather than have a mode of callling the
            function in to use either the read or write set.
          */
          //DBUG_ASSERT(!bitmap_is_set(table->write_set,
          //                           master_to_slave_map[master_idx]));
          conv_table_idx++;
        }

        continue;
      }

      /*
        Check 2: Skip unpacking if the field was written in this record, but
        the slave doesn't have the column. Note though that because the field
        is set on the master, we still have to update the null_pos and
        pack_ptr.
      */
      null_value= rpl_bitmap_is_set(st.null_ptr, st.null_pos++);
      if (tabledef->master_to_slave_error[master_idx])
      {
        /* Column does not exist on slave, skip over it */
        if (!null_value)
          st.pack_ptr+=
              tabledef->calc_field_size(master_idx, (uchar *) st.pack_ptr);
        continue;
      }


      uint slave_idx= tabledef->master_to_slave_map[master_idx];
      DBUG_ASSERT(bitmap_is_set(table->write_set, slave_idx) ||
                  bitmap_is_set(table->read_set, slave_idx));
      result_field= field= table->field[slave_idx];

      /*
        Check 3: Skip unpacking if NULL is explicitly provided for the field.
        In this case, note that the field must be prepared with the correct
        default value,
      */
      if (null_value)
      {
        prepare_null_field(field, &st);
        conv_table_idx++;
        continue;
      }

      /*
        Phase 2: Unpack the actual value into the slave table with any
        necessary conversions.
      */

      /* Set attributes for the slave-side field */
      result_field->set_has_explicit_value();
      result_field->set_notnull();

      /*
        If there is a conversion table, we pick up the field pointer to
        the conversion table.  If the conversion table or the field
        pointer is NULL, no conversions are necessary.
      */
      if (conv_table && (conv_field= conv_table->field[conv_table_idx++]))
        field= conv_field;


      DBUG_PRINT("debug", ("Conversion %srequired for field '%s' (#%u)",
                           conv_field ? "" : "not ",
                           result_field->field_name.str, master_idx));
      DBUG_ASSERT(field != NULL);

      bool unpack_result= unpack_field(tabledef, field, &st, master_idx);
      if (!unpack_result)
      {
        rgi->rli->report(ERROR_LEVEL, ER_SLAVE_CORRUPT_EVENT,
                    rgi->gtid_info(),
                    "Could not read field '%s' of table '%s.%s'",
                    field->field_name.str, table->s->db.str,
                    table->s->table_name.str);
        DBUG_RETURN(HA_ERR_CORRUPT_EVENT);
      }

      /*
        If conv_field is set, then we are doing a conversion. In this
        case, we have unpacked the master data to the conversion
        table, so we need to copy the value stored in the conversion
        table into the final table and do the conversion at the same time.

        If copy_fields is set, it means we are doing an online alter table,
        and will use copy_fields set up in copy_data_between_tables
       */
      if (conv_field)
        convert_field(result_field, conv_field);
    }

    if (master_reclength)
    {
      if (result_field)
        *master_reclength = (ulong)(result_field->ptr - table->record[0]);
      else
        *master_reclength = table->s->reclength;
    }
  }
  else
  {
    /*
      For Online Alter, iterate through old table fields to unpack,
      then iterate through copy_field array to copy to the new table's record.
     */

    DBUG_ASSERT(master_cols == conv_table->s->fields);
    for (;master_idx < master_cols; master_idx++)
    {
      DBUG_ASSERT(bitmap_is_set(cols, master_idx));
      Field *f= conv_table->field[master_idx];
      if (rpl_bitmap_is_set(st.null_ptr, st.null_pos++))
      {
        prepare_null_field(f, &st);
        continue;
      }
      f->set_notnull();
      f->set_has_explicit_value();
#ifndef DBUG_OFF
      bool result=
#endif
        unpack_field(tabledef, f, &st, master_idx);
      DBUG_ASSERT(result);
    }

    for (const auto *copy=rpl_data.copy_fields;
         copy != rpl_data.copy_fields_end; copy++)
    {
      copy->to_field->set_has_explicit_value();
      copy->do_copy(copy);
    }
    if (master_reclength)
      *master_reclength = conv_table->s->reclength;
  } // if (rpl_data.is_online_alter())

  /*
    We should now have read all the null bytes, otherwise something is
    really wrong.
   */
  DBUG_ASSERT(((st.null_pos + 7) / 8) == st.master_null_byte_count);
  DBUG_DUMP("row_data", row_data, st.pack_ptr - row_data);

  *current_row_end = st.pack_ptr;

  if (table->default_field && (rpl_data.is_online_alter() ||
      LOG_EVENT_IS_WRITE_ROW(rgi->current_event->get_type_code())))
  {
    update_write_set_for_auto_filled_fields(table, table->default_field);
    error= table->update_default_fields(table->in_use->lex->ignore);
    if (unlikely(error))
      DBUG_RETURN(error);
  }
  if (table->vfield)
  {
    /*
      TODO MDEV-36892: Data Loss Replicating Persistent Fields if Slave Has
                       Different Function

      If a master provides values for a persisted virtual column, the slave
      overwrites these values using its own function.
    */
    update_write_set_for_auto_filled_fields(table, table->vfield);
    error= table->update_virtual_fields(table->file, VCOL_UPDATE_FOR_WRITE);
    if (unlikely(error))
      DBUG_RETURN(error);
  }

  if (rpl_data.is_online_alter())
  {
    /* we only check constraints for ALTER TABLE */
    DBUG_ASSERT(table->in_use->lex->ignore == FALSE);
    error = table->verify_constraints(false);
    DBUG_ASSERT(error != VIEW_CHECK_SKIP);
    if (error)
      DBUG_RETURN(HA_ERR_GENERIC);
  }

  DBUG_RETURN(0);
}

/**
  Fills @c table->record[0] with default values.

  First @c restore_record() is called to restore the default values for
  record concerning the given table. Then, if @c check is true, 
  a check is performed to see if fields are have default value or can 
  be NULL. Otherwise error is reported.
 
  @param table  Table whose record[0] buffer is prepared. 

  @returns 0 on success
 */ 
int prepare_record(TABLE *const table)
{
  uint col= 0;
  DBUG_ENTER("prepare_record");

  restore_record(table, s->default_values);

  /*
    All fields are used
  */
  if (bitmap_is_set_all(table->write_set))
    DBUG_RETURN(0);

  /*
    For fields on the slave that are not going to be updated from the row image,
    we check if they have a default.
    The check follows the same rules as the INSERT query without specifying an
    explicit value for a field not having the explicit default 
    (@c check_that_all_fields_are_given_values()).
  */
  col= bitmap_get_first_clear(table->write_set);
  for (Field **field_ptr= table->field + col; *field_ptr; field_ptr++, col++)
  {
    if (!bitmap_is_set(table->write_set, col))
      continue;

    Field *const f= *field_ptr;
    DBUG_ASSERT(!((f->flags & NO_DEFAULT_VALUE_FLAG) && f->vcol_info)); // QQ
    if ((f->flags & NO_DEFAULT_VALUE_FLAG) &&
        (f->real_type() != MYSQL_TYPE_ENUM) && !f->vcol_info)
    {
      THD *thd= f->table->in_use;
      f->set_default();
      push_warning_printf(thd,
                          Sql_condition::WARN_LEVEL_WARN,
                          ER_NO_DEFAULT_FOR_FIELD,
                          ER_THD(thd, ER_NO_DEFAULT_FOR_FIELD),
                          f->field_name.str);
    }
  }

  DBUG_RETURN(0);
}
#endif // HAVE_REPLICATION
