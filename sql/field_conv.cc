/* Copyright (c) 2000, 2016, Oracle and/or its affiliates.
   Copyright (c) 2010, 2020, MariaDB Corporation

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

  @brief
  Functions to copy data to or from fields

    This could be done with a single short function but opencoding this
    gives much more speed.
*/

#include "mariadb.h"
#include "sql_priv.h"
#include "sql_class.h"                          // THD
#include <m_ctype.h>

void Field::do_field_eq(Copy_field *copy)
{
  memcpy(copy->to_ptr,copy->from_ptr,copy->from_length);
}

static void do_field_1(Copy_field *copy)
{
  copy->to_ptr[0]=copy->from_ptr[0];
}

static void do_field_2(Copy_field *copy)
{
  copy->to_ptr[0]=copy->from_ptr[0];
  copy->to_ptr[1]=copy->from_ptr[1];
}

static void do_field_3(Copy_field *copy)
{
  copy->to_ptr[0]=copy->from_ptr[0];
  copy->to_ptr[1]=copy->from_ptr[1];
  copy->to_ptr[2]=copy->from_ptr[2];
}

static void do_field_4(Copy_field *copy)
{
  copy->to_ptr[0]=copy->from_ptr[0];
  copy->to_ptr[1]=copy->from_ptr[1];
  copy->to_ptr[2]=copy->from_ptr[2];
  copy->to_ptr[3]=copy->from_ptr[3];
}

static void do_field_6(Copy_field *copy)
{						// For blob field
  copy->to_ptr[0]=copy->from_ptr[0];
  copy->to_ptr[1]=copy->from_ptr[1];
  copy->to_ptr[2]=copy->from_ptr[2];
  copy->to_ptr[3]=copy->from_ptr[3];
  copy->to_ptr[4]=copy->from_ptr[4];
  copy->to_ptr[5]=copy->from_ptr[5];
}

static void do_field_8(Copy_field *copy)
{
  copy->to_ptr[0]=copy->from_ptr[0];
  copy->to_ptr[1]=copy->from_ptr[1];
  copy->to_ptr[2]=copy->from_ptr[2];
  copy->to_ptr[3]=copy->from_ptr[3];
  copy->to_ptr[4]=copy->from_ptr[4];
  copy->to_ptr[5]=copy->from_ptr[5];
  copy->to_ptr[6]=copy->from_ptr[6];
  copy->to_ptr[7]=copy->from_ptr[7];
}


static void do_field_to_null_str(Copy_field *copy)
{
  if (*copy->from_null_ptr & copy->from_bit)
  {
    bzero(copy->to_ptr,copy->from_length);
    copy->to_null_ptr[0]=1;			// Always bit 1
  }
  else
  {
    copy->to_null_ptr[0]=0;
    memcpy(copy->to_ptr,copy->from_ptr,copy->from_length);
  }
}


static void do_outer_field_to_null_str(Copy_field *copy)
{
  if (*copy->null_row ||
      (copy->from_null_ptr && (*copy->from_null_ptr & copy->from_bit)))
  {
    bzero(copy->to_ptr,copy->from_length);
    copy->to_null_ptr[0]=1;			// Always bit 1
  }
  else
  {
    copy->to_null_ptr[0]=0;
    memcpy(copy->to_ptr,copy->from_ptr,copy->from_length);
  }
}


static int set_bad_null_error(Field *field, int err)
{
  switch (field->table->in_use->count_cuted_fields) {
  case CHECK_FIELD_WARN:
    field->set_warning(Sql_condition::WARN_LEVEL_WARN, err, 1);
    /* fall through */
  case CHECK_FIELD_IGNORE:
  case CHECK_FIELD_EXPRESSION:
    return 0;
  case CHECK_FIELD_ERROR_FOR_NULL:
    if (!field->table->in_use->no_errors)
      my_error(ER_BAD_NULL_ERROR, MYF(0), field->field_name.str);
    return -1;
  }
  DBUG_ASSERT(0); // impossible
  return -1;
}


int set_field_to_null(Field *field)
{
  if (field->table->null_catch_flags & CHECK_ROW_FOR_NULLS_TO_REJECT)
  {
    field->table->null_catch_flags|= REJECT_ROW_DUE_TO_NULL_FIELDS;
    return -1;
  }
  if (field->real_maybe_null())
  {
    field->set_null();
    field->reset();
    return 0;
  }
  field->reset();
  return set_bad_null_error(field, WARN_DATA_TRUNCATED);
}


/**
  Set TIMESTAMP to NOW(), AUTO_INCREMENT to the next number, or report an error

  @param field           Field to update

  @retval
    0    Field could take 0 or an automatic conversion was used
  @retval
    -1   Field could not take NULL and no conversion was used.
    If no_conversion was not set, an error message is printed
*/

int convert_null_to_field_value_or_error(Field *field)
{
  if (field->type() == MYSQL_TYPE_TIMESTAMP)
  {
    field->set_time();
    return 0;
  }

  field->reset(); // Note: we ignore any potential failure of reset() here.

  if (field == field->table->next_number_field)
  {
    field->table->auto_increment_field_not_null= FALSE;
    return 0;                             // field is set in fill_record()
  }
  return set_bad_null_error(field, ER_BAD_NULL_ERROR);
}

/**
  Set field to NULL or TIMESTAMP or to next auto_increment number.

  @param field           Field to update
  @param no_conversions  Set to 1 if we should return 1 if field can't
                         take null values.
                         If set to 0 we will do store the 'default value'
                         if the field is a special field. If not we will
                         give an error.

  @retval
    0    Field could take 0 or an automatic conversion was used
  @retval
    -1   Field could not take NULL and no conversion was used.
    If no_conversion was not set, an error message is printed
*/

int
set_field_to_null_with_conversions(Field *field, bool no_conversions)
{
  if (field->table->null_catch_flags & CHECK_ROW_FOR_NULLS_TO_REJECT)
  {
    field->table->null_catch_flags|= REJECT_ROW_DUE_TO_NULL_FIELDS;
    return -1;
  }
  if (field->real_maybe_null())
  {
    field->set_null();
    field->reset();
    return 0;
  }
  if (no_conversions)
    return -1;

  return convert_null_to_field_value_or_error(field);
}


static void do_skip(Copy_field *copy __attribute__((unused)))
{
}


/* 
  Copy: (NULLable field) -> (NULLable field) 

  note: if the record we're copying from is NULL-complemetned (i.e. 
  from_field->table->null_row==1), it will also have all NULLable columns to be
  set to NULLs, so we don't need to check table->null_row here.
*/

static void do_copy_null(Copy_field *copy)
{
  if (*copy->from_null_ptr & copy->from_bit)
  {
    *copy->to_null_ptr|=copy->to_bit;
    copy->to_field->reset();
  }
  else
  {
    *copy->to_null_ptr&= ~copy->to_bit;
    (copy->do_copy2)(copy);
  }
}

/*
  Copy: (not-NULL field in table that can be NULL-complemented) -> (NULLable 
     field)
*/

static void do_outer_field_null(Copy_field *copy)
{
  if (*copy->null_row ||
      (copy->from_null_ptr && (*copy->from_null_ptr & copy->from_bit)))
  {
    *copy->to_null_ptr|=copy->to_bit;
    copy->to_field->reset();
  }
  else
  {
    *copy->to_null_ptr&= ~copy->to_bit;
    (copy->do_copy2)(copy);
  }
}

/*
  Copy: (not-NULL field in table that can be NULL-complemented) -> (not-NULL
  field)
*/
static void do_copy_nullable_row_to_notnull(Copy_field *copy)
{
  if (*copy->null_row ||
      (copy->from_null_ptr && (*copy->from_null_ptr & copy->from_bit)))
  {
    copy->to_field->set_warning(Sql_condition::WARN_LEVEL_WARN,
                                WARN_DATA_TRUNCATED, 1);
    copy->to_field->reset();
  }
  else
  {
    (copy->do_copy2)(copy);
  }

}

/* Copy: (NULL-able field) -> (not NULL-able field) */
static void do_copy_not_null(Copy_field *copy)
{
  if (*copy->from_null_ptr & copy->from_bit)
  {
    copy->to_field->set_warning(Sql_condition::WARN_LEVEL_WARN,
                                WARN_DATA_TRUNCATED, 1);
    copy->to_field->reset();
  }
  else
    (copy->do_copy2)(copy);
}


/* Copy: (non-NULLable field) -> (NULLable field) */
static void do_copy_maybe_null(Copy_field *copy)
{
  *copy->to_null_ptr&= ~copy->to_bit;
  (copy->do_copy2)(copy);
}

/* timestamp and next_number has special handling in case of NULL values */

static void do_copy_timestamp(Copy_field *copy)
{
  if (*copy->from_null_ptr & copy->from_bit)
  {
    /* Same as in set_field_to_null_with_conversions() */
    copy->to_field->set_time();
  }
  else
    (copy->do_copy2)(copy);
}


static void do_copy_next_number(Copy_field *copy)
{
  if (*copy->from_null_ptr & copy->from_bit)
  {
    /* Same as in set_field_to_null_with_conversions() */
    copy->to_field->table->auto_increment_field_not_null= FALSE;
    copy->to_field->reset();
  }
  else
    (copy->do_copy2)(copy);
}


void Field_blob::do_copy_blob(Copy_field *copy)
{
  ((Field_blob*) copy->to_field)->copy_value(((Field_blob*) copy->from_field));
}

void Field_blob::do_conv_blob(Copy_field *copy)
{
  copy->from_field->val_str(&copy->tmp);
  ((Field_blob *) copy->to_field)->store(copy->tmp.ptr(),
					 copy->tmp.length(),
					 copy->tmp.charset());
}

/** Save blob in copy->tmp for GROUP BY. */

static void do_save_blob(Copy_field *copy)
{
  char buff[MAX_FIELD_WIDTH];
  String res(buff,sizeof(buff),copy->tmp.charset());
  copy->from_field->val_str(&res);
  copy->tmp.copy(res);
  ((Field_blob *) copy->to_field)->store(copy->tmp.ptr(),
					 copy->tmp.length(),
					 copy->tmp.charset());
}


void Field::do_field_string(Copy_field *copy)
{
  char buff[MAX_FIELD_WIDTH];
  String res(buff, sizeof(buff), copy->from_field->charset());
  res.length(0U);

  copy->from_field->val_str(&res);
  copy->to_field->store(res.ptr(), res.length(), res.charset());
}


void Field_enum::do_field_enum(Copy_field *copy)
{
  if (copy->from_field->val_int() == 0)
    ((Field_enum *) copy->to_field)->store_type((ulonglong) 0);
  else
    do_field_string(copy);
}


static void do_field_varbinary_pre50(Copy_field *copy)
{
  char buff[MAX_FIELD_WIDTH];
  copy->tmp.set_buffer_if_not_allocated(buff,sizeof(buff),copy->tmp.charset());
  copy->from_field->val_str(&copy->tmp);

  /* Use the same function as in 4.1 to trim trailing spaces */
  size_t length= my_lengthsp_8bit(&my_charset_bin, copy->tmp.ptr(),
                                copy->from_field->field_length);

  copy->to_field->store(copy->tmp.ptr(), length,
                        copy->tmp.charset());
}


void Field::do_field_int(Copy_field *copy)
{
  longlong value= copy->from_field->val_int();
  copy->to_field->store(value,
                        MY_TEST(copy->from_field->flags & UNSIGNED_FLAG));
}

void Field::do_field_real(Copy_field *copy)
{
  double value=copy->from_field->val_real();
  copy->to_field->store(value);
}


void Field::do_field_decimal(Copy_field *copy)
{
  my_decimal value(copy->from_field);
  copy->to_field->store_decimal(&value);
}


void Field::do_field_timestamp(Copy_field *copy)
{
  // XXX why couldn't we do it everywhere?
  copy->from_field->save_in_field(copy->to_field);
}


void Field::do_field_temporal(Copy_field *copy, date_mode_t fuzzydate)
{
  MYSQL_TIME ltime;
  // TODO: we now need to check result
  if (copy->from_field->get_date(&ltime, fuzzydate))
    copy->to_field->reset();
  else
    copy->to_field->store_time_dec(&ltime, copy->from_field->decimals());
}


void Field::do_field_datetime(Copy_field *copy)
{
  return do_field_temporal(copy, Datetime::Options(TIME_CONV_NONE, current_thd));
}


void Field::do_field_date(Copy_field *copy)
{
  return do_field_temporal(copy, Date::Options(TIME_CONV_NONE));
}


void Field_time::do_field_time(Copy_field *copy)
{
  return do_field_temporal(copy, Time::Options(current_thd));
}


/**
  string copy for single byte characters set when to string is shorter than
  from string.
*/

static void do_cut_string(Copy_field *copy)
{
  CHARSET_INFO *cs= copy->from_field->charset();
  memcpy(copy->to_ptr,copy->from_ptr,copy->to_length);

  /* Check if we loosed any important characters */
  if (cs->scan((char*) copy->from_ptr + copy->to_length,
               (char*) copy->from_ptr + copy->from_length,
               MY_SEQ_SPACES) < copy->from_length - copy->to_length)
  {
    copy->to_field->set_warning(Sql_condition::WARN_LEVEL_WARN,
                                WARN_DATA_TRUNCATED, 1);
  }
}


/**
  string copy for multi byte characters set when to string is shorter than
  from string.
*/

static void do_cut_string_complex(Copy_field *copy)
{						// Shorter string field
  CHARSET_INFO *cs= copy->from_field->charset();
  const uchar *from_end= copy->from_ptr + copy->from_length;
  Well_formed_prefix prefix(cs,
                           (char*) copy->from_ptr,
                           (char*) from_end,
                           copy->to_length / cs->mbmaxlen);
  size_t copy_length= prefix.length();
  if (copy->to_length < copy_length)
    copy_length= copy->to_length;
  memcpy(copy->to_ptr, copy->from_ptr, copy_length);

  /* Check if we lost any important characters */
  if (unlikely(prefix.well_formed_error_pos() ||
               cs->scan((char*) copy->from_ptr + copy_length,
                        (char*) from_end,
                        MY_SEQ_SPACES) <
               (copy->from_length - copy_length)))
  {
    copy->to_field->set_warning(Sql_condition::WARN_LEVEL_WARN,
                                WARN_DATA_TRUNCATED, 1);
  }

  if (copy_length < copy->to_length)
    cs->fill((char*) copy->to_ptr + copy_length,
             copy->to_length - copy_length, ' ');
}




static void do_expand_binary(Copy_field *copy)
{
  CHARSET_INFO *cs= copy->from_field->charset();
  memcpy(copy->to_ptr,copy->from_ptr,copy->from_length);
  cs->fill((char*) copy->to_ptr+copy->from_length,
           copy->to_length-copy->from_length, '\0');
}



static void do_expand_string(Copy_field *copy)
{
  CHARSET_INFO *cs= copy->from_field->charset();
  memcpy(copy->to_ptr,copy->from_ptr,copy->from_length);
  cs->fill((char*) copy->to_ptr+copy->from_length,
           copy->to_length-copy->from_length, ' ');
}


static void do_varstring1(Copy_field *copy)
{
  uint length= (uint) *(uchar*) copy->from_ptr;
  if (length > copy->to_length- 1)
  {
    length=copy->to_length - 1;
    if (copy->from_field->table->in_use->count_cuted_fields >
        CHECK_FIELD_EXPRESSION &&
        copy->to_field)
      copy->to_field->set_warning(Sql_condition::WARN_LEVEL_WARN,
                                  WARN_DATA_TRUNCATED, 1);
  }
  *(uchar*) copy->to_ptr= (uchar) length;
  memcpy(copy->to_ptr+1, copy->from_ptr + 1, length);
}


static void do_varstring1_mb(Copy_field *copy)
{
  CHARSET_INFO *cs= copy->from_field->charset();
  uint from_length= (uint) *(uchar*) copy->from_ptr;
  const uchar *from_ptr= copy->from_ptr + 1;
  uint to_char_length= (copy->to_length - 1) / cs->mbmaxlen;
  Well_formed_prefix prefix(cs, (char*) from_ptr, from_length, to_char_length);
  if (prefix.length() < from_length)
  {
    if (current_thd->count_cuted_fields > CHECK_FIELD_EXPRESSION)
      copy->to_field->set_warning(Sql_condition::WARN_LEVEL_WARN,
                                  WARN_DATA_TRUNCATED, 1);
  }
  *copy->to_ptr= (uchar) prefix.length();
  memcpy(copy->to_ptr + 1, from_ptr, prefix.length());
}


static void do_varstring2(Copy_field *copy)
{
  uint length=uint2korr(copy->from_ptr);
  if (length > copy->to_length- HA_KEY_BLOB_LENGTH)
  {
    length=copy->to_length-HA_KEY_BLOB_LENGTH;
    if (copy->from_field->table->in_use->count_cuted_fields >
        CHECK_FIELD_EXPRESSION &&
        copy->to_field)
      copy->to_field->set_warning(Sql_condition::WARN_LEVEL_WARN,
                                  WARN_DATA_TRUNCATED, 1);
  }
  int2store(copy->to_ptr,length);
  memcpy(copy->to_ptr+HA_KEY_BLOB_LENGTH, copy->from_ptr + HA_KEY_BLOB_LENGTH,
         length);
}


static void do_varstring2_mb(Copy_field *copy)
{
  CHARSET_INFO *cs= copy->from_field->charset();
  uint char_length= (copy->to_length - HA_KEY_BLOB_LENGTH) / cs->mbmaxlen;
  uint from_length= uint2korr(copy->from_ptr);
  const uchar *from_beg= copy->from_ptr + HA_KEY_BLOB_LENGTH;
  Well_formed_prefix prefix(cs, (char*) from_beg, from_length, char_length);
  if (prefix.length() < from_length)
  {
    if (current_thd->count_cuted_fields > CHECK_FIELD_EXPRESSION)
      copy->to_field->set_warning(Sql_condition::WARN_LEVEL_WARN,
                                  WARN_DATA_TRUNCATED, 1);
  }  
  int2store(copy->to_ptr, prefix.length());
  memcpy(copy->to_ptr+HA_KEY_BLOB_LENGTH, from_beg, prefix.length());
}
 

/***************************************************************************
** The different functions that fills in a Copy_field class
***************************************************************************/

/**
  copy of field to maybe null string.
  If field is null then the all bytes are set to 0.
  if field is not null then the first byte is set to 1 and the rest of the
  string is the field value.
  The 'to' buffer should have a size of field->pack_length()+1
*/

void Copy_field::set(uchar *to,Field *from)
{
  from_ptr=from->ptr;
  to_ptr=to;
  from_length=from->pack_length_in_rec();
  if (from->maybe_null())
  {
    from_null_ptr=from->null_ptr;
    from_bit=	  from->null_bit;
    to_ptr[0]=	  1;				// Null as default value
    to_null_ptr=  (uchar*) to_ptr++;
    to_bit=	  1;
    if (from->table->maybe_null)
    {
      null_row=   &from->table->null_row;
      do_copy=	  do_outer_field_to_null_str;
    }
    else
      do_copy=	  do_field_to_null_str;
  }
  else
  { 
    to_null_ptr=  0;				// For easy debugging
    do_copy= Field::do_field_eq;
  }
}


/*
  To do: 

  If 'save' is set to true and the 'from' is a blob field, do_copy is set to
  do_save_blob rather than do_conv_blob.  The only differences between them
  appears to be:

  - do_save_blob allocates and uses an intermediate buffer before calling 
    Field_blob::store. Is this in order to trigger the call to 
    well_formed_copy_nchars, by changing the pointer copy->tmp.ptr()?
    That call will take place anyway in all known cases.
 */
void Copy_field::set(Field *to,Field *from,bool save)
{
  if (to->type() == MYSQL_TYPE_NULL)
  {
    to_null_ptr=0;				// For easy debugging
    to_ptr=0;
    do_copy=do_skip;
    return;
  }
  from_field=from;
  to_field=to;
  from_ptr=from->ptr;
  from_length=from->pack_length_in_rec();
  to_ptr=  to->ptr;
  to_length=to_field->pack_length_in_rec();

  // set up null handling
  from_null_ptr=to_null_ptr=0;
  if (from->maybe_null())
  {
    from_null_ptr=	from->null_ptr;
    from_bit=		from->null_bit;
    if (to_field->real_maybe_null())
    {
      to_null_ptr=	to->null_ptr;
      to_bit=		to->null_bit;
      if (from_null_ptr)
	do_copy=	do_copy_null;
      else
      {
	null_row=	&from->table->null_row;
	do_copy=	do_outer_field_null;
      }
    }
    else
    {
      if (to_field->type() == MYSQL_TYPE_TIMESTAMP)
        do_copy= do_copy_timestamp;               // Automatic timestamp
      else if (to_field == to_field->table->next_number_field)
        do_copy= do_copy_next_number;
      else
      {
        if (!from_null_ptr)
        {
          null_row= &from->table->null_row;
          do_copy= do_copy_nullable_row_to_notnull;
        }
        else
          do_copy= do_copy_not_null;
      }
    }
  }
  else if (to_field->real_maybe_null())
  {
    to_null_ptr=	to->null_ptr;
    to_bit=		to->null_bit;
    do_copy= do_copy_maybe_null;
  }
  else
   do_copy=0;

  if ((to->flags & BLOB_FLAG) && save)
    do_copy2= do_save_blob;
  else
    do_copy2= from->get_copy_func_to(to);
  if (!do_copy)					// Not null
    do_copy=do_copy2;
}


Field::Copy_func *Field_timestamp::get_copy_func(const Field *from) const
{
  Field::Copy_func *copy= Field_temporal::get_copy_func(from);
  if (copy == do_field_datetime && from->type() == MYSQL_TYPE_TIMESTAMP)
    return do_field_timestamp;
  else
    return copy;
}


Field::Copy_func *Field_date_common::get_copy_func(const Field *from) const
{
  Field::Copy_func *copy= Field_temporal::get_copy_func(from);
  return copy == do_field_datetime ? do_field_date : copy;
}


Field::Copy_func *Field_temporal::get_copy_func(const Field *from) const
{
  /* If types are not 100 % identical then convert trough get_date() */
  if (from->cmp_type() == REAL_RESULT)
    return do_field_string; // TODO: MDEV-9344
  if (from->type() == MYSQL_TYPE_YEAR)
    return do_field_string; // TODO: MDEV-9343
  if (from->type() == MYSQL_TYPE_BIT)
    return do_field_int;
  if (!eq_def(from) ||
      (table->in_use->variables.sql_mode &
       (MODE_NO_ZERO_IN_DATE | MODE_NO_ZERO_DATE)))
    return do_field_datetime;
  return get_identical_copy_func();
}


Field::Copy_func *Field_varstring::get_copy_func(const Field *from) const
{
  if (from->type() == MYSQL_TYPE_BIT)
    return do_field_int;
  /*
    Detect copy from pre 5.0 varbinary to varbinary as of 5.0 and
    use special copy function that removes trailing spaces and thus
    repairs data.
  */
  if (from->type() == MYSQL_TYPE_VAR_STRING && !from->has_charset() &&
      !Field_varstring::has_charset())
    return do_field_varbinary_pre50;
  if (Field_varstring::real_type() != from->real_type() ||
      Field_varstring::charset() != from->charset() ||
      length_bytes != ((const Field_varstring*) from)->length_bytes ||
      !compression_method() != !from->compression_method())
    return do_field_string;
  return length_bytes == 1 ?
         (from->charset()->mbmaxlen == 1 ? do_varstring1 : do_varstring1_mb) :
         (from->charset()->mbmaxlen == 1 ? do_varstring2 : do_varstring2_mb);
}


Field::Copy_func *Field_string::get_copy_func(const Field *from) const
{
  if (from->type() == MYSQL_TYPE_BIT)
    return do_field_int;
  if (Field_string::type_handler() != from->type_handler() ||
      Field_string::charset() != from->charset())
    return do_field_string;
  if (Field_string::pack_length() < from->pack_length())
    return (Field_string::charset()->mbmaxlen == 1 ?
            do_cut_string : do_cut_string_complex);
  if (Field_string::pack_length() > from->pack_length())
    return Field_string::charset() == &my_charset_bin ? do_expand_binary :
                                                        do_expand_string;
  return get_identical_copy_func();
}


Field::Copy_func *Field::get_identical_copy_func() const
{
  /* Identical field types */
  switch (pack_length()) {
  case 1: return do_field_1;
  case 2: return do_field_2;
  case 3: return do_field_3;
  case 4: return do_field_4;
  case 6: return do_field_6;
  case 8: return do_field_8;
  }
  return do_field_eq;
}


bool Field_temporal::memcpy_field_possible(const Field *from) const
{
  return real_type() == from->real_type() &&
         decimals() == from->decimals() &&
         !sql_mode_for_dates(table->in_use);
}


static int field_conv_memcpy(Field *to, Field *from)
{
  /*
    This may happen if one does 'UPDATE ... SET x=x'
    The test is here mostly for valgrind, but can also be relevant
    if memcpy() is implemented with prefetch-write
  */
  if (to->ptr != from->ptr)
    memcpy(to->ptr,from->ptr, to->pack_length());
  return 0;
}


/**
  Copy value of the field with conversion.

  @note Impossibility of simple copy should be checked before this call.

  @param to              The field to copy to

  @retval TRUE ERROR
  @retval FALSE OK

*/
static int field_conv_incompatible(Field *to, Field *from)
{
  return to->store_field(from);
}


/**
  Simple quick field converter that is called on insert, e.g.:
    INSERT INTO t1 (field1) SELECT field2 FROM t2;
*/

int field_conv(Field *to,Field *from)
{
  return to->memcpy_field_possible(from) ?
         field_conv_memcpy(to, from) :
         field_conv_incompatible(to, from);
}


fast_field_copier Field::get_fast_field_copier(const Field *from)
{
  DBUG_ENTER("Field::get_fast_field_copier");
  DBUG_RETURN(memcpy_field_possible(from) ?
              &field_conv_memcpy :
              &field_conv_incompatible);
}
