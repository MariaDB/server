#ifndef MYSQL_JSON_INCLUDED
#define MYSQL_JSON_INCLUDED

#include "my_global.h"
#include "sql_string.h"                         // String

enum JSONB_TYPES {
  JSONB_TYPE_SMALL_OBJECT= 0x0,
  JSONB_TYPE_LARGE_OBJECT= 0x1,
  JSONB_TYPE_SMALL_ARRAY=  0x2,
  JSONB_TYPE_LARGE_ARRAY=  0x3,
  JSONB_TYPE_LITERAL=      0x4,
  JSONB_TYPE_INT16=        0x5,
  JSONB_TYPE_UINT16=       0x6,
  JSONB_TYPE_INT32=        0x7,
  JSONB_TYPE_UINT32=       0x8,
  JSONB_TYPE_INT64=        0x9,
  JSONB_TYPE_UINT64=       0xA,
  JSONB_TYPE_DOUBLE=       0xB,
  JSONB_TYPE_STRING=       0xC,
  JSONB_TYPE_OPAQUE=       0xF,
};

bool parse_mysql_json_value(String *buffer, JSONB_TYPES type, const char *data,
                            size_t len, size_t depth);
#endif  /* MYSQL_JSON_INCLUDED */
