#include <algorithm>

#include "mysql_json.h"

#include "compat56.h"
#include "my_decimal.h"
#include "sql_time.h"

/*
  Json values in MySQL comprises the standard set of JSON values plus a MySQL
  specific set. A JSON number type is subdivided into int, uint, double and
  decimal.

  MySQL also adds four built-in date/time values: date, time, datetime and
  timestamp. An additional opaque value can store any other MySQL type.
*/


enum JSONB_LITERAL_TYPES {
  JSONB_NULL_LITERAL=      0x0,
  JSONB_TRUE_LITERAL=      0x1,
  JSONB_FALSE_LITERAL=     0x2,
};


/*
  The size of offset or size fields in the small and the large storage
  format for JSON objects and JSON arrays.
*/
static const uchar SMALL_OFFSET_SIZE= 2;
static const uchar LARGE_OFFSET_SIZE= 4;

/*
  The size of key entries for objects when using the small storage
  format or the large storage format. In the small format it is 4
  bytes (2 bytes for key length and 2 bytes for key offset). In the
  large format it is 6 (2 bytes for length, 4 bytes for offset).
*/
static const uchar KEY_ENTRY_SIZE_SMALL= (2 + SMALL_OFFSET_SIZE);
static const uchar KEY_ENTRY_SIZE_LARGE= (2 + LARGE_OFFSET_SIZE);

/*
  The size of value entries for objects or arrays. When using the
  small storage format, the entry size is 3 (1 byte for type, 2 bytes
  for offset). When using the large storage format, it is 5 (1 byte
  for type, 4 bytes for offset).
*/
static const uchar VALUE_ENTRY_SIZE_SMALL= (1 + SMALL_OFFSET_SIZE);
static const uchar VALUE_ENTRY_SIZE_LARGE= (1 + LARGE_OFFSET_SIZE);

/* The maximum number of nesting levels allowed in a JSON document. */
static const uchar JSON_DOCUMENT_MAX_DEPTH= 100;


/**
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


static inline size_t key_size(bool large)
{
  return large ? KEY_ENTRY_SIZE_LARGE : KEY_ENTRY_SIZE_SMALL;
}


static inline size_t value_size(bool large)
{
  return large ? VALUE_ENTRY_SIZE_LARGE : VALUE_ENTRY_SIZE_SMALL;
}


/**
  Inlined values are a space optimization. The actual value is stored
  instead of the offset pointer to the location where a non-inlined
  value would be located.

  @param[in] type   The type to check.
  @param[in] large tells if the large or small storage format is used;
*/
static inline bool type_is_stored_inline(JSONB_TYPES type, bool large)
{
  return (type == JSONB_TYPE_INT16 ||
          type == JSONB_TYPE_UINT16 ||
          type == JSONB_TYPE_LITERAL ||
          (large && (type == JSONB_TYPE_INT32 ||
                     type == JSONB_TYPE_UINT32)));
}

/**
  Read a variable length integer. A variable length integer uses the 8th bit in
  each byte to mark if there are more bytes needed to store the integer. The
  other 7 bits in the byte are used to store the actual integer's bits.

  @param[in]  data         the buffer to read from
  @param[in]  data_length  the maximum number of bytes to read from data
  @param[out] length       the length that was read
  @param[out] num          the number of bytes needed to represent the length
  @return  false on success, true on error
*/
static inline bool read_variable_length(const char *data, size_t data_length,
                                        size_t *length, size_t *num)
{
  /*
    It takes five bytes to represent UINT_MAX32, which is the largest
    supported length, so don't look any further.

    Use data_length as max value to prevent segfault when reading a corrupted
    JSON document.
  */
  const size_t max_bytes= std::min(data_length, static_cast<size_t>(5));

  size_t len= 0;
  for (size_t i= 0; i < max_bytes; i++)
  {
    /* Get the next 7 bits of the length. */
    len|= (data[i] & 0x7f) << (7 * i);
    if ((data[i] & 0x80) == 0)
    {
      /* The length shouldn't exceed 32 bits. */
      if (len > UINT_MAX32)
        return true;

      /* This was the last byte. Return successfully. */
      *num= i + 1;
      *length= len;
      return false;
    }
  }

  /* No more available bytes. Return true to signal error. This implies a
     corrupted JSON document. */
  return true;
}


/**
   JSON formatting in MySQL escapes a few special characters to prevent
   ambiguity.
*/
static bool append_string_json(String *buffer, const char *data, size_t len)
{
  const char *last= data + len;
  for (; data < last; data++)
  {
    const uchar c= *data;
    switch (c) {
    case '\\':
      buffer->append("\\\\");
      break;
    case '\n':
      buffer->append("\\n");
      break;
    case '\r':
      buffer->append("\\r");
      break;
    case '"':
      buffer->append("\\\"");
      break;
    case '\b':
      buffer->append("\\b");
      break;
    case '\f':
      buffer->append("\\f");
      break;
    case '\t':
      buffer->append("\\t");
      break;
    default:
      buffer->append(c);
      break;
    }
  }
  return false;
}


static bool print_mysql_datetime_value(String *buffer, enum_field_types type,
                                       const char *data, size_t len)
{
  if (len < 8)
    return true;

  MYSQL_TIME t;
  switch (type)
  {
    case MYSQL_TYPE_TIME:
      TIME_from_longlong_time_packed(&t, sint8korr(data));
      break;
    case MYSQL_TYPE_DATE:
      TIME_from_longlong_date_packed(&t, sint8korr(data));
      break;
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_TIMESTAMP:
      TIME_from_longlong_datetime_packed(&t, sint8korr(data));
      break;
    default:
      DBUG_ASSERT(0);
      return true;
  }
  /* Wrap all datetime strings within double quotes. */
  buffer->append('\"');
  buffer->reserve(MAX_DATE_STRING_REP_LENGTH);
  buffer->length(buffer->length() +
                 my_TIME_to_str(&t, const_cast<char *>(buffer->end()), 6));
  buffer->append('\"');
  return false;
}


static bool parse_mysql_scalar(String *buffer, size_t value_json_type,
                               const char *data, size_t len)
{
  switch (value_json_type) {
  case JSONB_TYPE_LITERAL:
  {
    if (len < 1)
      return true;
    switch (static_cast<JSONB_LITERAL_TYPES>(*data)) {
    case JSONB_NULL_LITERAL:
      return buffer->append("null");
    case JSONB_TRUE_LITERAL:
      return buffer->append("true");
    case JSONB_FALSE_LITERAL:
      return buffer->append("false");
    default: /* Invalid literal constant, malformed JSON. */
      return true;
    }
  }
  case JSONB_TYPE_INT16:
    return len < 2 || buffer->append_longlong(sint2korr(data));
  case JSONB_TYPE_INT32:
    return len < 4 || buffer->append_longlong(sint4korr(data));
  case JSONB_TYPE_INT64:
    return len < 8 || buffer->append_longlong(sint8korr(data));
  case JSONB_TYPE_UINT16:
    return len < 2 || buffer->append_ulonglong(uint2korr(data));
  case JSONB_TYPE_UINT32:
    return len < 4 || buffer->append_ulonglong(uint4korr(data));
  case JSONB_TYPE_UINT64:
    return len < 8 || buffer->append_ulonglong(uint8korr(data));
  case JSONB_TYPE_DOUBLE:
    if (len < 8)
      return true;
    buffer->reserve(FLOATING_POINT_BUFFER, 2 * FLOATING_POINT_BUFFER);
    buffer->qs_append(reinterpret_cast<const double *>(data));
    return false;
  case JSONB_TYPE_STRING:
  {
    size_t string_length, length_bytes;

    return read_variable_length(data, len, &string_length, &length_bytes) ||
           len < length_bytes + string_length ||
           buffer->append('"') ||
           append_string_json(buffer, data + length_bytes, string_length) ||
           buffer->append('"');
  }
  case JSONB_TYPE_OPAQUE:
  {
    /* The field_type maps directly to enum_field_types. */
    const uchar type_value= static_cast<uchar>(*data);
    const enum_field_types field_type= static_cast<enum_field_types>(type_value);

    size_t blob_length, length_bytes;
    const char *blob_start;

    if (read_variable_length(data + 1, len, &blob_length, &length_bytes) ||
        len < length_bytes + blob_length)
      return true;
    blob_start= data + length_bytes + 1;

    switch (field_type) {
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_TIMESTAMP:
      return print_mysql_datetime_value(buffer, field_type,
                                        blob_start, blob_length);
    case MYSQL_TYPE_NEWDECIMAL:
    {
      /* Expect at least two bytes, which contain precision and scale. */
      if (blob_length < 2)
        return true;

      const int precision= blob_start[0];
      const int scale= blob_start[1];

      my_decimal d;

      /* The decimal value is encoded after the two prec/scale bytes. */
      const size_t dec_size= my_decimal_get_binary_size(precision, scale);
      if (dec_size != blob_length - 2 ||
          binary2my_decimal(E_DEC_ERROR,
                            reinterpret_cast<const uchar *>(blob_start + 2),
                            &d, precision, scale) != E_DEC_OK)
        return true;
      d.fix_buffer_pointer();

      if (my_decimal2string(E_DEC_ERROR, &d, 0, 0, ' ', buffer) != E_DEC_OK)
        return true;
      return false;
    }
    default:
    {
      /* Any other MySQL type is presented as a base64 encoded string. */
      if (buffer->append("\"base64:type") ||
          buffer->append_longlong(field_type) ||
          buffer->append(':'))
        return true;

      const size_t needed= my_base64_needed_encoded_length(
          static_cast<int>(blob_length));
      if (buffer->reserve(needed) ||
          my_base64_encode(blob_start, blob_length,
                           const_cast<char*>(buffer->end())))
        return true;
      /* -1 to override the null terminator from my_base64_encode */
      DBUG_ASSERT(*(buffer->end() + needed) == '\0');
      buffer->length(buffer->length() + needed - 1);
      return buffer->append('"');
    }
    }
  }
  default:
    return true;
  }
}


/**
  Read a value from a JSON Object or Array, given the position of it.
  This function handles both inlined values as well as values stored at
  an offset.

  @param[out] buffer            Where to print the results.
  @param[in] data               The raw binary data of the Object or Array.
  @param[in] len                The length of the binary data.
  @param[in] value_type_offset  Where the type of the value is stored.
  @param[in] large              true if the large storage format is used;
  @param[in] depth              How deep the JSON object is in the hierarchy.
*/
static bool parse_mysql_scalar_or_value(String *buffer, const char *data,
                                        size_t len, size_t value_type_offset,
                                        bool large, size_t depth)
{
  /* Get the type of the value stored at the key. */
  const JSONB_TYPES value_type=
    static_cast<JSONB_TYPES>(data[value_type_offset]);

  if (type_is_stored_inline(value_type, large))
  {
    const size_t value_start = value_type_offset + 1;
    if (parse_mysql_scalar(buffer, value_type, data + value_start,
                           len - value_start))
      return true;
  }
  else
  {
    /* The offset to where the value is stored is relative to the start
       of the Object / Array */
    const size_t value_start= read_offset_or_size(
                                      data + value_type_offset + 1, large);
    if (parse_mysql_json_value(buffer, value_type, data + value_start,
                               len - value_start, depth))
      return true;
  }
  return false;

}


static bool parse_array_or_object(String *buffer, const char *data, size_t len,
                                  bool handle_as_object, bool large,
                                  size_t depth)
{
  if (depth > JSON_DOCUMENT_MAX_DEPTH)
    return true;

  /*
    Make sure the document is long enough to contain the two length fields
    (both number of elements or members, and number of bytes).
  */
  const size_t offset_size= large ? LARGE_OFFSET_SIZE : SMALL_OFFSET_SIZE;
  /* The length has to be at least double offset size (header). */
  if (len < 2 * offset_size)
    return true;


  /*
     Every JSON Object or Array contains two numbers in the header:
     - The number of elements in the Object / Array (Keys)
     - The total number of bytes occupied by the JSON Object / Array, including
       the two numbers in the header.
     Depending on the Object / Array type (small / large) the numbers are stored
     in 2 bytes or 4 bytes each.
  */
  const size_t element_count= read_offset_or_size(data, large);
  const size_t bytes= read_offset_or_size(data + offset_size, large);

  /* The value can't have more bytes than what's available in the buffer. */
  if (bytes > len)
    return true;

  if (buffer->append(handle_as_object ? '{' : '['))
    return true;


  for (size_t i= 0; i < element_count; i++)
  {
    if (handle_as_object)
    {
      /*
        The JSON Object is stored as a header part and a data part.
        Header consists of:
        - two length fields,
        - an array of pointers to keys.
        - an array of tuples (type, pointer to values)
          * For certain types, the pointer to values is replaced by the actual
            value. (see type_is_stored_inline)
        Data consists of:
        - All Key data, in order
        - All Value data, in order
      */
      const size_t key_offset= 2 * offset_size + i * key_size(large);
      const size_t key_start= read_offset_or_size(data + key_offset, large);
      /* The length of keys is always stored in 2 bytes (large == false) */
      const size_t key_len= read_offset_or_size(
                                   data + key_offset + offset_size, false);

      const size_t value_type_offset= 2 * offset_size +
                                      element_count * key_size(large) +
                                      i * value_size(large);

      /* First print the key. */
      if (buffer->append('"') ||
          append_string_json(buffer, data + key_start, key_len) ||
          buffer->append("\": "))
      {
        return true;
      }

      /* Then print the value. */
      if (parse_mysql_scalar_or_value(buffer, data, bytes, value_type_offset,
                                      large, depth))
        return true;
    }
    else
    {
      /*
         Arrays do not have the keys vector and its associated data.
         We jump straight to reading values.
      */
      const size_t value_type_offset= 2 * offset_size + value_size(large) * i;

      if (parse_mysql_scalar_or_value(buffer, data, bytes, value_type_offset,
                                      large, depth))
        return true;
    }

    if (i != element_count - 1 && buffer->append(", "))
      return true;
  }

  return buffer->append(handle_as_object ? '}' : ']');
}


bool parse_mysql_json_value(String *buffer, JSONB_TYPES type, const char *data,
                            size_t len, size_t depth)
{
  switch (type) {
  case JSONB_TYPE_SMALL_OBJECT:
    return parse_array_or_object(buffer, data, len, true, false, depth + 1);
  case JSONB_TYPE_LARGE_OBJECT:
    return parse_array_or_object(buffer, data, len, true, true, depth + 1);
  case JSONB_TYPE_SMALL_ARRAY:
    return parse_array_or_object(buffer, data, len, false, false, depth + 1);
  case JSONB_TYPE_LARGE_ARRAY:
    return parse_array_or_object(buffer, data, len, false, true, depth + 1);
  default:
    return parse_mysql_scalar(buffer, type, data, len);
  }
}
