#ifndef MYSQL_JSON_INCLUDED
#define MYSQL_JSON_INCLUDED

#include "my_global.h"
#include "sql_string.h"                         // String

bool parse_mysql_json_value(String *buffer, size_t type, const char *data, size_t len,
                 bool large, size_t depth);
#endif  /* MYSQL_JSON_INCLUDED */

