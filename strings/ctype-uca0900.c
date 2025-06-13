/* Copyright (c) 2025, MariaDB Corporation

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License as published by the Free Software Foundation; version 2
   of the License.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with this library; if not, write to the Free
   Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston,
   MA 02110-1335  USA */

#include "my_global.h"
#include "strings_def.h"
#include "ctype-uca.h"

struct mysql_0900_to_mariadb_1400_mapping
         mysql_0900_mapping[mysql_0900_collation_num]=
{
  /* 255 Ascent insensitive, Case insensitive 'ai_ci' */
  {"", "", "ai_ci", 2308},
  {"de_pb", "german2", "ai_ci", 2468},
  {"is", "icelandic", "ai_ci", 2316},
  {"lv", "latvian", "ai_ci", 2324},
  {"ro", "romanian", "ai_ci", 2332},
  {"sl", "slovenian", "ai_ci", 2340},
  {"pl", "polish", "ai_ci", 2348},
  {"et", "estonian", "ai_ci", 2356},
  {"es", "spanish", "ai_ci", 2364},
  {"sv", "swedish", "ai_ci", 2372},
  {"tr", "turkish", "ai_ci", 2380},
  {"cs", "czech", "ai_ci", 2388},
  {"da", "danish", "ai_ci", 2396},
  {"lt", "lithuanian", "ai_ci", 2404},
  {"sk", "slovak", "ai_ci", 2412},
  {"es_trad", "spanish2", "ai_ci", 2420},
  {"la", "roman", "ai_ci", 2428},
  {"fa", NullS, "ai_ci", 0},                          // Disabled in MySQL
  {"eo", "esperanto", "ai_ci", 2444},
  {"hu", "hungarian", "ai_ci", 2452},
  {"hr", "croatian", "ai_ci", 2500},
  {"si", NullS, "ai_ci", 0},                          // Disabled in MySQL
  {"vi", "vietnamese", "ai_ci", 2492},

  /* 278 Ascent sensitive, Case sensitive 'as_cs' */
  {"","", "as_cs", 2311},
  {"de_pb", "german2", "as_cs", 2471},
  {"is", "icelandic", "as_cs", 2319},
  {"lv", "latvian", "as_cs", 2327},
  {"ro", "romanian", "as_cs", 2335},
  {"sl", "slovenian", "as_cs", 2343},
  {"pl", "polish", "as_cs", 2351},
  {"et", "estonian", "as_cs", 2359},
  {"es", "spanish", "as_cs", 2367},
  {"sv", "swedish", "as_cs", 2375},
  {"tr", "turkish", "as_cs", 2383},
  {"cs", "czech", "as_cs", 2391},
  {"da", "danish", "as_cs", 2399},
  {"lt", "lithuanian", "as_cs", 2407},
  {"sk", "slovak", "as_cs", 2415},
  {"es_trad", "spanish2", "as_cs", 2423},
  {"la", "roman", "as_cs", 2431},
  {"fa", NullS, "as_cs", 0},                          // Disabled in MySQL
  {"eo", "esperanto", "as_cs", 2447},
  {"hu", "hungarian", "as_cs", 2455},
  {"hr", "croatian", "as_cs", 2503},
  {"si", NullS, "as_cs", 0},                          // Disabled in MySQL
  {"vi", "vietnamese", "as_cs", 2495},

  {"", NullS, "as_cs", 0},                            // Missing
  {"", NullS, "as_cs", 0},                            // Missing
  {"_ja_0900_as_cs", NullS, "as_cs", 0},              // Not supported
  {"_ja_0900_as_cs_ks", NullS, "as_cs", 0},           // Not supported

  /* 305 Ascent-sensitive, Case insensitive 'as_ci' */
  {"","", "as_ci", 2310},
  {"ru", NullS, "ai_ci", 0},                          // Not supported
  {"ru", NullS, "as_cs", 0},                          // Not supported
  {"zh", NullS, "as_cs", 0},                          // Not supported
  {NullS, NullS, "", 0}
};


static LEX_CSTRING
my_uca0900_collation_build_name(char *buffer, size_t buffer_size,
                                const char *cs_name,
                                const char *tailoring_name,
                                const char *sensitivity_suffix)
{
  LEX_CSTRING res;
  DBUG_ASSERT(buffer_size > 1);
  res.str= buffer;
  res.length= (strxnmov(buffer, buffer_size - 1,
                        cs_name, "_", tailoring_name,
                        (tailoring_name[0] ? "_" : ""),
                        "0900_",
                        sensitivity_suffix,
                        NullS) - buffer);
  return res;
}


static LEX_CSTRING
my_ci_make_comment_for_alias(char *buffer, size_t buffer_size,
                             const char *srcname)
{
  LEX_CSTRING res= {buffer, 0};
  DBUG_ASSERT(buffer_size > 0);
  res.length= strxnmov(buffer, buffer_size - 1, "Alias for ", srcname, NullS) -
              buffer;
  return res;
}


/*
  Add a MySQL UCA-0900 collation as an alias for a MariaDB UCA-1400 collation.
*/
static my_bool
mysql_uca0900_collation_definition_add(MY_CHARSET_LOADER *loader,
                                       const struct
                                       mysql_0900_to_mariadb_1400_mapping *map,
                                       uint alias_id)
{
  char comment_buffer[MY_CS_COLLATION_NAME_SIZE + 15];
  char alias_buffer[MY_CS_COLLATION_NAME_SIZE + 1];
  char name1400_buffer[MY_CS_COLLATION_NAME_SIZE + 1];
  LEX_CSTRING comment= {comment_buffer, 0};
  LEX_CSTRING alias_name= {alias_buffer, 0};
  LEX_CSTRING name1400= {name1400_buffer, 0};
  LEX_CSTRING utf8mb4= {STRING_WITH_LEN("utf8mb4")};
  uint id1400= map->collation_id;
  uca_collation_def_param_t param= my_uca1400_collation_param_by_id(id1400);
  const MY_UCA1400_COLLATION_DEFINITION *def1400=
    &my_uca1400_collation_definitions[param.tailoring_id];

  DBUG_ASSERT(my_collation_id_is_mysql_uca0900(alias_id));

  alias_name= my_uca0900_collation_build_name(alias_buffer,
                                              sizeof(alias_buffer),
                                              "utf8mb4",
                                              map->mysql_col_name,
                                              map->case_sensitivity);

  name1400= my_uca1400_collation_build_name(name1400_buffer,
                                            sizeof(name1400_buffer),
                                            &utf8mb4, def1400->name, &param);
  comment= my_ci_make_comment_for_alias(comment_buffer, sizeof(comment_buffer),
                                        name1400.str);

#ifdef DEBUG_PRINT_ALIAS
  fprintf(stderr, "alias[%u] %-26s -> [%u] %s\n",
          id, alias_name.str, id1400, name1400.str);
#endif

  return my_uca1400_collation_alloc_and_init(loader, alias_name,
                                             comment, &param, alias_id);
}


/*
  Add support for MySQL 8.0 utf8mb4_0900_.. UCA collations.

  The collation id's were collected from fprintf()
  in mysql_uca0900_collation_definition_add().

  Map mysql character sets to MariaDB using the same definition but
  with the MySQL collation name and id.
*/

my_bool
mysql_uca0900_utf8mb4_collation_definitions_add(MY_CHARSET_LOADER *loader)
{
  uint alias_id= mysql_0900_collation_start;
  struct mysql_0900_to_mariadb_1400_mapping *map;

  for (map= mysql_0900_mapping; map->mysql_col_name ; map++, alias_id++)
  {
    if (map->mariadb_col_name)               /* Supported collation */
    {
      if (mysql_uca0900_collation_definition_add(loader, map, alias_id))
        return TRUE;
    }
  }

  return FALSE;
}


/*
  Add MySQL utf8mb4_0900_bin collation as
  an alias for MariaDB utf8mb4_nopad_bin.
*/
my_bool mysql_utf8mb4_0900_bin_add(MY_CHARSET_LOADER *loader)
{
  CHARSET_INFO *src= &my_charset_utf8mb4_nopad_bin;
  LEX_CSTRING alias_name= {STRING_WITH_LEN("utf8mb4_0900_bin")};
  uint alias_id= 309;
  char comment_buffer[MY_CS_COLLATION_NAME_SIZE+15];
  LEX_CSTRING comment= my_ci_make_comment_for_alias(comment_buffer,
                                                    sizeof(comment_buffer),
                                                    src->coll_name.str);
  struct charset_info_st *dst= my_ci_alloc(loader, alias_name, &alias_name,
                                           comment, &comment);
  if (!dst)
    return TRUE;

  *dst= *src;

  dst->number= alias_id;
  dst->coll_name= alias_name;
  dst->comment= comment.str;

  (loader->add_collation)(dst);

  return FALSE;
}
