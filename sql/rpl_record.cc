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
        pack_ptr= field->pack(pack_ptr, field->ptr + offset,
                              field->max_data_length());
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

   @param rli     Relay log info, which can be NULL
   @param table   Table to unpack into
   @param master_cols Number of columns in the binlog
   @param row_data
                  Packed row data
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

#if !defined(MYSQL_CLIENT) && defined(HAVE_REPLICATION)
int
unpack_row(rpl_group_info *rgi,
           TABLE *table, uint const master_cols,
           uchar const *const row_data, MY_BITMAP const *cols,
           uchar const **const current_row_end, ulong *const master_reclength,
           uchar const *const row_end)
{
  int error;
  size_t const master_null_byte_count= (bitmap_bits_set(cols) + 7) / 8;
  uchar const *null_ptr= row_data;
  uchar const *pack_ptr= row_data + master_null_byte_count;
  DBUG_ENTER("unpack_row");

  DBUG_ASSERT(rgi);
  DBUG_ASSERT(row_data);
  DBUG_ASSERT(table);

  if (bitmap_is_clear_all(cols))
  {
    /*
      There was no data sent from the master, so there is
      nothing to unpack.
    */
    *current_row_end= pack_ptr;
    *master_reclength= 0;
    DBUG_RETURN(0);
  }

  RPL_TABLE_LIST *table_list= rgi->get_table_data(table);
  table_def *tabledef= &table_list->m_tabledef;
  uint *master_to_slave_map= tabledef->master_to_slave_map;
  TABLE *conv_table= table_list->m_conv_table;
  uint conv_table_idx= 0, null_pos= 0;

  /*
    Two phases:
      1. First, perform a sanity check to see if the value should actually be
         unpacked, i.e. if nothing was binlogged (NULL) or the column doesn't
         exist on the slave. If there is nothing to unpack, we can just skip
         that column; but the unpack state needs to be maintained (i.e.
         pack_ptr and conv_table_idx need to be incremented appropriately), and
         the field must be configured with the correct default value (or NULL).

      2. Unpack the actual value into the slave field with any necessary
         conversions.
  */
  for (uint master_idx= 0; master_idx < master_cols; master_idx++)
  {
    bool null_value;
    uint slave_idx;
    Field *field, *conv_field= 0;
    Copy_field copy;

    /*
      Part 1: Skip columns on the master that were not replicated
    */
    if (!bitmap_is_set(cols, master_idx))
    {
      if (!(tabledef->master_to_slave_error[master_idx]))
      {
        DBUG_ASSERT(!bitmap_is_set(table->write_set,
                                   master_to_slave_map[master_idx]));
        conv_table_idx++;
        continue;
      }
    }
    /*
      TODO BN: I Think null_ptr needs to be initially offset like in the base
      patch
    */
    null_value= rpl_bitmap_is_set(null_ptr, null_pos++);
    if (tabledef->master_to_slave_error[master_idx])
    {
      /* Column does not exist on slave, skip over it */
      if (!null_value)
        pack_ptr+= tabledef->calc_field_size(master_idx, (uchar *) pack_ptr);
      continue;
    }
    slave_idx= master_to_slave_map[master_idx];
    DBUG_ASSERT(bitmap_is_set(table->write_set, slave_idx) ||
                bitmap_is_set(table->read_set, slave_idx));
    field= table->field[slave_idx];

    if (null_value)
    {
      if (field->maybe_null())
      {
        /**
           Calling reset to ensure that the field data is zeroed.
           This is needed as the default value for the field can
           be different from the 'zero' value.

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
        field->reset();
        field->set_null();
      }
      else
      {
        THD *thd= field->table->in_use;

        field->set_default();
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_BAD_NULL_ERROR,
                            ER_THD(thd, ER_BAD_NULL_ERROR),
                            field->field_name.str);
      }
      conv_table_idx++;
      continue;
    }

    /*
      Part 2: Unpack the actual value into the slave field with any necessary
      conversions.
    */

    /* Found not null field */
    field->set_notnull();

    /*
      If there is a conversion table, we pick up the field pointer to
      the conversion table.  If the conversion table or the field
      pointer is NULL, no conversions are necessary.
     */
    if (conv_table && (conv_field= conv_table->field[conv_table_idx++]))
      field= conv_field;

    /*
      Use the master's size information if available else call
      normal unpack operation.
    */
    uint16 const metadata= tabledef->field_metadata(master_idx);
#ifndef DBUG_OFF
    uchar const *const old_pack_ptr= pack_ptr;
#endif
    pack_ptr= field->unpack(field->ptr, pack_ptr, row_end, metadata);
    DBUG_PRINT("rpl", ("field: %s; metadata: 0x%x;"
                       " pack_ptr: %p; pack_ptr': %p; bytes: %d",
                       field->field_name.str, metadata,
                       old_pack_ptr, pack_ptr,
                       (int) (pack_ptr - old_pack_ptr)));
    if (!pack_ptr)
    {
      rgi->rli->report(ERROR_LEVEL, ER_SLAVE_CORRUPT_EVENT,
                       rgi->gtid_info(),
                       "Could not read field '%s' of table '%s.%s'",
                       field->field_name.str, table->s->db.str,
                       table->s->table_name.str);
      DBUG_RETURN(HA_ERR_CORRUPT_EVENT);
    }

    if (!conv_field)
      continue;

    field= table->field[slave_idx];             // Restore field
    DBUG_PRINT("rpl", ("Conversion required for field '%s' (#%ld)",
                       field->field_name.str, master_idx));
    /*
      If conv_field was set, we are doing a conversion. In this
      case, we have unpacked the master data to the conversion
      table, so we need to copy the value stored in the conversion
      table into the final table and do the conversion at the same time.
    */

#ifndef DBUG_OFF
    char source_buf[MAX_FIELD_WIDTH];
    char value_buf[MAX_FIELD_WIDTH];
    String source_type(source_buf, sizeof(source_buf), system_charset_info);
    String value_string(value_buf, sizeof(value_buf), system_charset_info);
    conv_field->sql_type(source_type);
    conv_field->val_str(&value_string);
    DBUG_PRINT("rpl", ("Copying field '%s' of type '%s' with value '%s'",
                       field->field_name.str,
                       source_type.c_ptr_safe(), value_string.c_ptr_safe()));
#endif
    copy.set(field, conv_field, TRUE);
    (*copy.do_copy)(&copy);
#ifndef DBUG_OFF
    char target_buf[MAX_FIELD_WIDTH];
    String target_type(target_buf, sizeof(target_buf), system_charset_info);
    field->sql_type(target_type);
    field->val_str(&value_string);
    DBUG_PRINT("rpl", ("Value of field '%s' of type '%s' is now '%s'",
                         field->field_name.str,
                         target_type.c_ptr_safe(), value_string.c_ptr_safe()));
#endif
  }

  /*
    Add Extra slave persistent columns
  */
  if (unlikely(error= fill_extra_persistent_columns(table)))
    DBUG_RETURN(error);

  DBUG_DUMP("row_data", row_data, pack_ptr - row_data);

  *current_row_end= pack_ptr;
  *master_reclength= table->s->reclength;

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
  if (bitmap_is_set_all(table->write_set))
    DBUG_RETURN(0);                             // All fields used

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
        (f->real_type() != MYSQL_TYPE_ENUM) &&
        !f->vcol_info)
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
/**
  Fills @c table->record[0] with computed values of extra persistent columns
  which are present on slave but not on master.

  @param table         Table whose record[0] buffer is prepared.
                       Table->cond_set should contain the bitmap of
                       all columns in the original write set.
  @param master_cols   No of columns on master 
  @returns 0 on        success
 */

int fill_extra_persistent_columns(TABLE *table)
{
  int error= 0;
  Field **vfield_ptr, *vfield;

  if (!table->vfield)
    return 0;
  for (vfield_ptr= table->vfield; *vfield_ptr; ++vfield_ptr)
  {
    vfield= *vfield_ptr;
    if (!bitmap_is_set(&table->cond_set, vfield->field_index-1) &&
                       vfield->stored_in_db())
    {
      bitmap_set_bit(table->write_set, vfield->field_index);
      if ((error= vfield->vcol_info->expr->save_in_field(vfield,0)))
        break;
    }
  }
  return error;
}
#endif // HAVE_REPLICATION
