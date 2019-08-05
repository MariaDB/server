#include "mysql_json.h"
#include "mysqld.h"             // key_memory_JSON
#include "sql_class.h"          // THD
#include "field.h"          // THD
#include "algorithm"

#define JSONB_TYPE_SMALL_OBJECT   0x0
#define JSONB_TYPE_LARGE_OBJECT   0x1
#define JSONB_TYPE_SMALL_ARRAY    0x2
#define JSONB_TYPE_LARGE_ARRAY    0x3
#define JSONB_TYPE_LITERAL        0x4
#define JSONB_TYPE_INT16          0x5
#define JSONB_TYPE_UINT16         0x6
#define JSONB_TYPE_INT32          0x7
#define JSONB_TYPE_UINT32         0x8
#define JSONB_TYPE_INT64          0x9
#define JSONB_TYPE_UINT64         0xA
#define JSONB_TYPE_DOUBLE         0xB
#define JSONB_TYPE_STRING         0xC
#define JSONB_TYPE_OPAQUE         0xF

#define JSONB_NULL_LITERAL        '\x00'
#define JSONB_TRUE_LITERAL        '\x01'
#define JSONB_FALSE_LITERAL       '\x02'
/*
  The size of offset or size fields in the small and the large storage
  format for JSON objects and JSON arrays.
*/
#define SMALL_OFFSET_SIZE         2
#define LARGE_OFFSET_SIZE         4

/*
  The size of key entries for objects when using the small storage
  format or the large storage format. In the small format it is 4
  bytes (2 bytes for key length and 2 bytes for key offset). In the
  large format it is 6 (2 bytes for length, 4 bytes for offset).
*/
#define KEY_ENTRY_SIZE_SMALL      (2 + SMALL_OFFSET_SIZE)
#define KEY_ENTRY_SIZE_LARGE      (2 + LARGE_OFFSET_SIZE)

/*
  The size of value entries for objects or arrays. When using the
  small storage format, the entry size is 3 (1 byte for type, 2 bytes
  for offset). When using the large storage format, it is 5 (1 byte
  for type, 4 bytes for offset).
*/
#define VALUE_ENTRY_SIZE_SMALL    (1 + SMALL_OFFSET_SIZE)
#define VALUE_ENTRY_SIZE_LARGE    (1 + LARGE_OFFSET_SIZE)

/// The maximum number of nesting levels allowed in a JSON document.
#define JSON_DOCUMENT_MAX_DEPTH 100

/*
    Json values in MySQL comprises the stand set of JSON values plus a
    MySQL specific set. A Json _number_ type is subdivided into _int_,
    _uint_, _double_ and _decimal_.

    MySQL also adds four built-in date/time values: _date_, _time_,
    _datetime_ and _timestamp_.  An additional _opaque_ value can
    store any other MySQL type.

    The enumeration is common to Json_dom and Json_wrapper.

    The enumeration is also used by Json_wrapper::compare() to
    determine the ordering when comparing values of different types,
    so the order in which the values are defined in the enumeration,
    is significant. The expected order is null < number < string <
    object < array < boolean < date < time < datetime/timestamp <
    opaque.
  enum enum_json_type {
    J_NULL,
    J_DECIMAL,
    J_INT,
    J_UINT,
    J_DOUBLE,
    J_STRING,
    J_OBJECT,
    J_ARRAY,
    J_BOOLEAN,
    J_DATE,
    J_TIME,
    J_DATETIME,
    J_TIMESTAMP,
    J_OPAQUE,
    J_ERROR
  };
*/

/*
  Read an offset or size field from a buffer. The offset could be either
  a two byte unsigned integer or a four byte unsigned integer.

  @param data  the buffer to read from
  @param large tells if the large or small storage format is used; true
               means read four bytes, false means read two bytes
*/

static inline size_t read_offset_or_size(const char *data, bool large)
{
  return large ? uint4korr(data) : uint2korr(data);
}


/**
  Read a variable length written by append_variable_length().

  @param[in] data  the buffer to read from
  @param[in] data_length  the maximum number of bytes to read from data
  @param[out] length  the length that was read
  @param[out] num  the number of bytes needed to represent the length
  @return  false on success, true on error
*/
static inline bool read_variable_length(const char *data, size_t data_length,
                                        size_t *length, size_t *num)
{
  /*
    It takes five bytes to represent UINT_MAX32, which is the largest
    supported length, so don't look any further.

    Use data_length as max value to prevent segfault when reading corrupted
    JSON document.
  */
  const size_t max_bytes= std::min(data_length, static_cast<size_t>(5));

  size_t len= 0;
  for (size_t i= 0; i < max_bytes; i++)
  {
    // Get the next 7 bits of the length.
    len|= (data[i] & 0x7f) << (7 * i);
    if ((data[i] & 0x80) == 0)
    {
      // The length shouldn't exceed 32 bits.
      if (len > UINT_MAX32)
        return true;

      // This was the last byte. Return successfully.
      *num= i + 1;
      *length= len;
      return false;
    }
  }

  // No more available bytes. Return true to signal error. This implies
  // corrupted JSON document.
  return true;
}


static bool parse_mysql_scalar(String* buffer, size_t value_json_type,
                        const char *data, size_t len, bool large, size_t depth)
{
  if (++depth > JSON_DOCUMENT_MAX_DEPTH)
    return true;

  switch (value_json_type)
  {
    case JSONB_TYPE_LITERAL:
    {
      switch (static_cast<uint8>(*data))
      {
        case JSONB_NULL_LITERAL:
        {
          if (buffer->append("null"))
            return true;
          break;
        }
        case JSONB_TRUE_LITERAL:
        {
          if (buffer->append("true"))
            return true;
          break;
        }
        case JSONB_FALSE_LITERAL:
        {
          if (buffer->append("false"))
            return true;
          break;
        }
        default:
          return true;
      }
      break;
    }
    /** FINISHED WORKS **/
    case JSONB_TYPE_INT16:
    {
      if (buffer->append_longlong((longlong) (sint2korr(data))))
        return true;
      break;
    }
    /** FINISHED WORKS **/
    case JSONB_TYPE_INT32:
    {
      const uint num_bytes= MAX_BIGINT_WIDTH + 1;
      char value_element [num_bytes + 2];
      memmove(value_element, &data[0], num_bytes);
      value_element[num_bytes + 1]= '\0';

      if (buffer->append_longlong(sint4korr(value_element)))
        return true;
      break;
    }
    /* FINISHED WORKS  */
    case JSONB_TYPE_INT64:
    {
      const uint num_bytes= MAX_BIGINT_WIDTH + 1;
      char value_element [num_bytes + 2];
      memmove(value_element, &data[0], num_bytes);
      value_element[num_bytes + 1]= '\0';
      if (buffer->append_longlong(sint8korr(value_element)))
        return true;
      break;
    }
    /** FINISHED WORKS **/
    case JSONB_TYPE_UINT16:
    {
      if (buffer->append_longlong((longlong) (uint2korr(data))))
        return true;
      break;
    }
    /** FINISHED WORKS **/
    case JSONB_TYPE_UINT32:
    {
      if (buffer->append_longlong((longlong) (uint4korr(data))))
        return true;
      break;
    }
    /** FINISHED WORKS **/
    case JSONB_TYPE_UINT64:
    {
      const uint num_bytes= MAX_BIGINT_WIDTH + 1;
      char value_element [num_bytes + 2];
      memmove(value_element, &data[0], num_bytes);
      value_element[num_bytes + 1]= '\0';
      if (buffer->append_ulonglong(uint8korr(value_element)))
        return true;
      break;
    }
    /** FINISHED WORKS **/
    case JSONB_TYPE_DOUBLE:
    {
      double d;
      float8get(d, data);
      buffer->qs_append(&d);
      break;
    }
    /** FINISHED WORKS **/
    case JSONB_TYPE_STRING:
    {
      size_t value_length, n;
      char *value_element;

      if (read_variable_length(data, len, &value_length, &n))
        return true;
      if (len < n + value_length)
        return true;

      value_element= new char[value_length + 1];
      memmove(value_element, &data[n], value_length);
      value_element[value_length]= '\0';

      if (buffer->append('"'))
      {
        delete[] value_element;
        return true;
      }
      if (buffer->append(String((const char *)value_element, &my_charset_bin)))
      {
        delete[] value_element;
        return true;
      }
      delete[] value_element;
      if (buffer->append('"'))
        return true;
      break;
    }

    /** FINISHED WORKS **/
    case JSONB_TYPE_OPAQUE:
    {
      // The type_byte is encoded as a uint8 that maps to an enum_field_types
      uint8 type_byte= static_cast<uint8>(*data);
      enum_field_types field_type=
        static_cast<enum_field_types>(type_byte);

      size_t value_length, n;
      char *value_element;

      if (read_variable_length(data + 1, len, &value_length, &n))
        return true;
      if (len < n + value_length)
        return true;

      value_element= new char[value_length + 1];
      memmove(value_element, &data[n + 1], value_length);
      value_element[value_length]= '\0';

      MYSQL_TIME t;
      switch (field_type)
      {
        case MYSQL_TYPE_TIME:
        {
          TIME_from_longlong_time_packed(&t, sint8korr(value_element));
          break;
        }
        case MYSQL_TYPE_DATE:
        {
          // The bellow line cannot work since it is not defined in sql/compat56.h
          //TIME_from_longlong_date_packed(ltime, packed_value);
          TIME_from_longlong_datetime_packed(&t, sint8korr(value_element));
          t.time_type= MYSQL_TIMESTAMP_DATE;
          break;
        }
        case MYSQL_TYPE_DATETIME:
        case MYSQL_TYPE_TIMESTAMP:
        {
          TIME_from_longlong_datetime_packed(&t, sint8korr(value_element));
          break;
        }
        case MYSQL_TYPE_NEWDECIMAL:
        {
          my_decimal m; //@todo anel // need to add test case !
          // Expect at least two bytes, which contain precision and scale.
          bool error= (value_length < 2);

          if (!error)
          {
            int precision= value_element[0];
            int scale= value_element[1];

            // The decimal value is encoded after the two precision/scale bytes.
            size_t bin_size= my_decimal_get_binary_size(precision, scale);
            error=
              (bin_size != value_length - 2) ||
              (binary2my_decimal(E_DEC_ERROR,
                                ((const uchar*)value_element) + 2,
                                &m, precision, scale) != E_DEC_OK);
            m.fix_buffer_pointer();
            // Convert my_decimal to decimal and append to string.
            double d;
            const decimal_t *mptr= &m;
            my_decimal2double(E_DEC_FATAL_ERROR, mptr, &d);
            buffer->qs_append(&d);
          }

          return error;
        }
        default:
        {
          /* The same encoding is applied on MYSQL_TYPE_BIT, MYSQL_TYPE_VARCHAR,
             MYSQL_TYPE_YEAR, MYSQL_TYPE_LONG_BLOB, MYSQL_TYPE_MEDIUM_BLOB,
             MYSQL_TYPE_TINY_BLOB, MYSQL_TYPE_BLOB.
          */
          if (field_type == MYSQL_TYPE_BIT || field_type == MYSQL_TYPE_VARCHAR ||
              field_type == MYSQL_TYPE_YEAR || field_type == MYSQL_TYPE_LONG_BLOB ||
              field_type == MYSQL_TYPE_MEDIUM_BLOB ||
              field_type == MYSQL_TYPE_TINY_BLOB || field_type == MYSQL_TYPE_BLOB)
              {
                if (buffer->append('"'))
                  return true;
                if (buffer->append("base64:type") || buffer->append(':'))
                  return true;

                size_t pos= buffer->length();
                const size_t needed=
                  static_cast<size_t>(my_base64_needed_encoded_length(value_length));
                buffer->reserve(needed);
                if(my_base64_encode(value_element, value_length,
                                    const_cast<char*>(buffer->ptr() + pos)))
                  return true;
                buffer->length(pos + needed - 1);
                if (buffer->append('"'))
                  return true;
                return false;
              }
          return false;
        }
      }
      delete[] value_element;
      // This part is common to datetime/date/timestamp
      char *ptr= const_cast<char *>(buffer->ptr()) + buffer->length();
      const int size= my_TIME_to_str(&t, ptr, 6);
      buffer->length(buffer->length() + size);
    } // opaque
  }
  return false;
}


static bool parse_array_or_object(String *buffer, Field_mysql_json::enum_type t,
                                  const char *data, size_t len, bool large)
{
  DBUG_ASSERT((t == Field_mysql_json::enum_type::ARRAY) ||
              (t == Field_mysql_json::enum_type::OBJECT));
  /*
    Make sure the document is long enough to contain the two length fields
    (both number of elements or members, and number of bytes).
  */
  const size_t offset_size= large ? LARGE_OFFSET_SIZE : SMALL_OFFSET_SIZE;
  // The length has to be at least double offset size (header).
  if (len < 2 * offset_size)
    return true;

  // Calculate number of elements and length of binary (number of bytes).
  size_t element_count, bytes;

  element_count= read_offset_or_size(data, large);
  bytes= read_offset_or_size(data + offset_size, large);

  // The value can't have more bytes than what's available in the data buffer.
  if (bytes > len)
    return true;

  // Handling start of object or arrays.
  if (t==Field_mysql_json::enum_type::OBJECT)
  {
    if (buffer->append('{'))
      return true;
  }
  else
  {
    if (buffer->append('['))
      return true;
  }

  // Variables used for an object - vector of keys.
  size_t key_json_offset, key_json_start, key_json_len;
  char *key_element;
  // Variables used for an object and array - vector of values.
  size_t type, value_type_offset;

  for (size_t i=0; i < element_count; i++)
  {
    if (t==Field_mysql_json::enum_type::OBJECT)
    {
      /*
        Calculate the size of the header. It consists of:
        - two length fields,
        - if it is a JSON object, key entries with pointers to where the keys
          are stored (key_json_offset),
        - value entries with pointers to where the actual values are stored
          (value_type_offset).
      */
      key_json_offset= 2 * offset_size + i * (large ? KEY_ENTRY_SIZE_LARGE :
                                                      KEY_ENTRY_SIZE_SMALL);
      key_json_start= read_offset_or_size(data + key_json_offset, large);
      // The length of keys is always on 2 bytes (large == false)
      key_json_len= read_offset_or_size(data + key_json_offset + offset_size,
                                        false);

      key_element= new char[key_json_len + 1];
      memmove(key_element, &data[key_json_start], key_json_len);
      key_element[key_json_len]= '\0';

      if (buffer->append('"'))
      {
        delete[] key_element;
        return true;
      }
      if (buffer->append(String((const char *)key_element, &my_charset_bin)))
      {
        delete[] key_element;
        return true;
      }
      delete[] key_element;
      if (buffer->append('"'))
        return true;

      if (buffer->append(':'))
        return true;

      value_type_offset= 2 * offset_size +
        (large ? KEY_ENTRY_SIZE_LARGE : KEY_ENTRY_SIZE_SMALL) * (element_count) +
        (large ? VALUE_ENTRY_SIZE_LARGE : VALUE_ENTRY_SIZE_SMALL) * i;

      // Get the type of the actual value.
      type= data[value_type_offset];

      // Inlined values are sort of optimization obtained from raw data,
      // where actual value is obtained as a first next byte from value_type_offset
      if (type == JSONB_TYPE_INT16 || type == JSONB_TYPE_UINT16 ||
          type == JSONB_TYPE_LITERAL ||
          (large && (type == JSONB_TYPE_INT32 || type == JSONB_TYPE_UINT32)))
      {
        if (parse_mysql_scalar(buffer, type, data + value_type_offset + 1,
                               len, large, 0))
          return true;
      }
      else // Non-inlined values - we need to get the lenght of data and use
           // recursively parse_value()
      {
        size_t val_start_offset= read_offset_or_size(data + value_type_offset + 1,
                                                large);
        if (parse_value(buffer, type, data + val_start_offset, bytes - val_start_offset,
                        large, 0))
          return true;
      }
      if (!(i == (element_count - 1)))
      {
        buffer->append(',');
      }
    } // end object

    else // t==Field_mysql::enum_type::Array
    {
      value_type_offset= 2 * offset_size +
        (large ? VALUE_ENTRY_SIZE_LARGE : VALUE_ENTRY_SIZE_SMALL) * i;

      type= data[value_type_offset];

     // Inlined values are sort of optimization obtained from raw data,
      // where actual value is obtained as a first next byte from value_type_offset
      if (type == JSONB_TYPE_INT16 || type == JSONB_TYPE_UINT16 ||
          type == JSONB_TYPE_LITERAL ||
          (large && (type == JSONB_TYPE_INT32 || type == JSONB_TYPE_UINT32)))
      {
        if (parse_mysql_scalar(buffer, type, data + value_type_offset + 1,
                               bytes, large, 0))
          return true;
      }
      else // Non-inlined values - we need to get the lenght of data and use
           // recursively parse_value()
      {
        size_t val_len_ptr= read_offset_or_size(data + value_type_offset + 1,
                                                large);
        if (parse_value(buffer, type, data + val_len_ptr, bytes - val_len_ptr,
                        large, 0))
          return true;
      }

      if(!(i==(element_count-1)))
      {
        buffer->append(',');
      }
    } // end array
  } // end for

// Handling ending of objects and arrays.
  if (t==Field_mysql_json::enum_type::OBJECT)
  {
    if (buffer->append('}'))
      return true;
  }
  else
  {
    if (buffer->append(']'))
      return true;
  }

  return false;
}


bool parse_value(String *buffer, size_t type, const char *data, size_t len,
                 bool large, size_t depth)
{
  switch (type)
  {
  case JSONB_TYPE_SMALL_OBJECT:
    return parse_array_or_object(buffer, Field_mysql_json::enum_type::OBJECT,
                                 data, len, false);
  case JSONB_TYPE_LARGE_OBJECT:
    return parse_array_or_object(buffer, Field_mysql_json::enum_type::OBJECT,
                                 data, len, true);
  case JSONB_TYPE_SMALL_ARRAY:
    return parse_array_or_object(buffer, Field_mysql_json::enum_type::ARRAY,
                                 data, len, false);
  case JSONB_TYPE_LARGE_ARRAY:
    return parse_array_or_object(buffer, Field_mysql_json::enum_type::ARRAY,
                                 data, len, true);
  default:
    return parse_mysql_scalar(buffer, type, data, len, large, depth);
  }
}
