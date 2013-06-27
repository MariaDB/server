
// vim:sw=2:ai

/*
 * Copyright (C) 2010-2011 DeNA Co.,Ltd.. All rights reserved.
 * Copyright (C) 2011 Kentoku SHIBA
 * See COPYRIGHT.txt for details.
 */

#ifndef DENA_STRING_UTIL_HPP
#define DENA_STRING_UTIL_HPP

#include "string_buffer.hpp"
#include "string_ref.hpp"

namespace dena {

inline const char *
memchr_char(const char *s, int c, size_t n)
{
  return static_cast<const char *>(memchr(s, c, n));
}

inline char *
memchr_char(char *s, int c, size_t n)
{
  return static_cast<char *>(memchr(s, c, n));
}

string_wref get_token(char *& wp, char *wp_end, char delim);
uint32 atoi_uint32_nocheck(const char *start, const char *finish);
/*
String *to_stdstring(uint32 v);
*/
void append_uint32(string_buffer& buf, uint32 v);
long long atoll_nocheck(const char *start, const char *finish);

int errno_string(const char *s, int en, String& err_r);

size_t split(char delim, const string_ref& buf, string_ref *parts,
  size_t parts_len);
size_t split(char delim, const string_wref& buf, string_wref *parts,
  size_t parts_len);
size_t split(char delim, const string_ref& buf,
  DYNAMIC_ARRAY& parts_r);
size_t split(char delim, const string_wref& buf,
  DYNAMIC_ARRAY& parts_r);
};

#endif

