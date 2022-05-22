#include "mariadb.h"
#include "field.h"

static decimal_digits_t get_decimal_precision(uint len, decimal_digits_t dec,
                                              bool unsigned_val)
{
  uint precision= my_decimal_length_to_precision(len, dec, unsigned_val);
  return (decimal_digits_t) MY_MIN(precision, DECIMAL_MAX_PRECISION);
}

Field_new_decimal::Field_new_decimal(uchar *ptr_arg,
                                     uint32 len_arg, uchar *null_ptr_arg,
                                     uchar null_bit_arg,
                                     enum utype unireg_check_arg,
                                     const LEX_CSTRING *field_name_arg,
                                     decimal_digits_t dec_arg,bool zero_arg,
                                     bool unsigned_arg)
        :Field_num(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
                   unireg_check_arg, field_name_arg, dec_arg, zero_arg, unsigned_arg)
{
  precision= get_decimal_precision(len_arg, dec_arg, unsigned_arg);
  DBUG_ASSERT((precision <= DECIMAL_MAX_PRECISION) &&
              (dec <= DECIMAL_MAX_SCALE));
  bin_size= my_decimal_get_binary_size(precision, dec);
}


Field_bit_as_char::Field_bit_as_char(uchar *ptr_arg, uint32 len_arg,
                                     uchar *null_ptr_arg, uchar null_bit_arg,
                                     enum utype unireg_check_arg,
                                     const LEX_CSTRING *field_name_arg)
        :Field_bit(ptr_arg, len_arg, null_ptr_arg, null_bit_arg, 0, 0,
                   unireg_check_arg, field_name_arg)
{
  flags|= UNSIGNED_FLAG;
  bit_len= 0;
  bytes_in_rec= (len_arg + 7) / 8;
}

Field_bit::Field_bit(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
                     uchar null_bit_arg, uchar *bit_ptr_arg, uchar bit_ofs_arg,
                     enum utype unireg_check_arg,
                     const LEX_CSTRING *field_name_arg)
        : Field(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
                unireg_check_arg, field_name_arg),
          bit_ptr(bit_ptr_arg), bit_ofs(bit_ofs_arg), bit_len(len_arg & 7),
          bytes_in_rec(len_arg / 8)
{
  DBUG_ENTER("Field_bit::Field_bit");
  DBUG_PRINT("enter", ("ptr_arg: %p, null_ptr_arg: %p, len_arg: %u, bit_len: %u, bytes_in_rec: %u",
          ptr_arg, null_ptr_arg, len_arg, bit_len, bytes_in_rec));
  flags|= UNSIGNED_FLAG;
  /*
    Ensure that Field::eq() can distinguish between two different bit fields.
    (two bit fields that are not null, may have same ptr and null_ptr)
  */
  if (!null_ptr_arg)
    null_bit= bit_ofs_arg;
  DBUG_VOID_RETURN;
}

uint Column_definition_attributes::pack_flag_to_pack_length() const
{
  uint type= f_packtype(pack_flag); // 0..15
  DBUG_ASSERT(type < 16);
  switch (type) {
    case MYSQL_TYPE_TINY:     return 1;
    case MYSQL_TYPE_SHORT:    return 2;
    case MYSQL_TYPE_LONG:     return 4;
    case MYSQL_TYPE_LONGLONG: return 8;
    case MYSQL_TYPE_INT24:    return 3;
  }
  return 0; // This should not happen
}

#define BLOB_PACK_LENGTH_TO_MAX_LENGH(arg) \
                        ((ulong) ((1LL << MY_MIN(arg, 4) * 8) - 1))

Field_blob::Field_blob(uchar *ptr_arg, uchar *null_ptr_arg, uchar null_bit_arg,
                       enum utype unireg_check_arg,
                       const LEX_CSTRING *field_name_arg,
                       TABLE_SHARE *share, uint blob_pack_length,
                       const DTCollation &collation)
        :Field_longstr(ptr_arg, BLOB_PACK_LENGTH_TO_MAX_LENGH(blob_pack_length),
                       null_ptr_arg, null_bit_arg, unireg_check_arg, field_name_arg,
                       collation),
         packlength(blob_pack_length)
{
  DBUG_ASSERT(blob_pack_length <= 4); // Only pack lengths 1-4 supported currently
  flags|= BLOB_FLAG;
  share->blob_fields++;
  /* TODO: why do not fill table->s->blob_field array here? */
}
