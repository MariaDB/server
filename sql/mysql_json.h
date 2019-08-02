#ifndef MYSQL_JSON_INCLUDED
#define MYSQL_JSON_INCLUDED

#include "my_global.h"
#include "field.h"
#include "my_global.h"
#include "sql_string.h"                         // String
#include "mysql_com.h"
#include "mysqld_error.h"

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

/**
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
  opaque.}
  */
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

/*
  Extended type ids so that JSON_TYPE() can give useful type
  names to certain sub-types of J_OPAQUE.
*/
enum enum_json_opaque_type {
  J_OPAQUE_BLOB,
  J_OPAQUE_BIT,
  J_OPAQUE_GEOMETRY
};


size_t read_offset_or_size(const char *, bool);
bool get_mysql_string(String *buffer, size_t type, const char *data, size_t len,
                      bool large);
bool parse_value(String *buffer, size_t type, const char *data, size_t len,
                 bool large, size_t depth);
bool parse_array_or_object(String * buffer, Field_mysql_json::enum_type,
                           const char *, size_t, bool);
bool parse_mysql_scalar(String* buffer, size_t type,
                        const char *data, size_t len, bool large, size_t depth);
#endif  /* MYSQL_JSON_INCLUDED */