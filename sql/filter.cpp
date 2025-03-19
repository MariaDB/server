/* Copyright (c) 2025, MariaDB Corporation.
   
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

#include "mysqld.h"
#include "hash.h"
#include "filter.hpp"

template<typename K, typename V>
bool HashFilter<K, V>::set_rules(const char* spec)
{
  const char *rule_begin;
  if (!spec)
    return true;
  my_hash_reset(&hashset);
  for (rule_begin= spec; *spec; ++spec)
    if (*spec == ',')
    {
      if (add_rule(rule_begin, spec))
        return true;
      // Skip ',' and trailing spaces
      while (my_isspace(system_charset_info, *(++spec)));
      rule_begin= spec;
    }
  return add_rule(rule_begin, spec); // Last rule before '\0'
}

template<typename K, typename V> HashFilter<K, V>::~HashFilter()
{ my_hash_free(&hashset); }
template<typename K, typename V>
void HashFilter<K, V>::to_string(String *out_string)
{
  string->length(0);
  for (ulong idx= 0; idx < hashset.records; string->append(','), ++idx)
    hash_element_append_string(my_hash_element(hashset, idx), out_string);
}

#define TABLE_RULE_HASH_SIZE 16
RewriteDB::RewriteDB(): HashFilter()
{
  my_hash_init(key_memory_TABLE_RULE_ENT, &hashset,
               Lex_ident_rpl_filter::charset_info(), TABLE_RULE_HASH_SIZE, 0, 0,
               [] (const void *p, size_t *len, my_bool)
               {
                 auto e= static_cast<const StringPair *>(p);
                 (*len)= e->from.length();
                 return reinterpret_cast<const uchar *>(e->from.ptr());
               },
               [] (void *p)
               { // my_free() is not `operator delete`
                 auto e= static_cast<StringPair *>(p);
                 e->from.~String();
                 e->to.~String();
                 my_free(p);
               }, 0);
}
const String *RewriteDB::operator[](const Binary_string *key)
{
  uchar *e= my_hash_search
    (&hashset, reinterpret_cast<const uchar *>(key->ptr()), key->length());
  return e ? &(reinterpret_cast<StringPair *>(e)->to) : nullptr;
}

bool RewriteDB::add_rule(const char *rule, const char *rule_end)
{
  // TODO also OPT_REWRITE_DB handling in client/mysqlbinlog
  const char *from_end= nullptr, *to, *to_end;
  for(to= rule + 2; to < rule_end; ++to)
    if (to[-2] == '-' && to[-1] == '>')
    {
      from_end= to - 2;
      break;
    }
  // Bad syntax, missing "->"
  if (!from_end)
    return true;
  // Skip blanks at the end of FROM
  while(from_end > rule && my_isspace(system_charset_info, from_end[-1]))
    --from_end;
  // Bad syntax: empty FROM DB
  if (from_end == rule)
    return true;
  // Skip blanks at the beginning of TO
  while(*to && my_isspace(system_charset_info, *to))
    ++to;
  // Bad syntax: empty TO DB
  if (to == rule_end)
    return true;
  // TO DB ends with `rule_end` or space
  for (to_end= to;
       to_end < rule_end && !my_isspace(system_charset_info, *to_end);
       ++to_end);
  auto element= static_cast<StringPair *>
    (my_malloc(key_memory_TABLE_RULE_ENT, sizeof(StringPair), MYF(MY_WME)));
  if (!element)
    return true;
  element->from.set(rule, from_end - rule, hashset.charset);
  element->to  .set(to  , rule_end - to  , hashset.charset);
  return my_hash_insert(&hashset, reinterpret_cast<const uchar *>(element));
}
void RewriteDB::hash_element_append_string(uchar *element, String *out_string)
{
  auto e= reinterpret_cast<StringPair *>(element);
  out_string->append(e->from);
  out_string->append(STRING_WITH_LEN("->"));
  out_string->append(e->to);
}
