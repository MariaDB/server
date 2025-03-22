/* Copyright (c) 2025, MariaDB

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

#include "strings_def.h"
#include "ctype-uca0900.h"


struct mysql_0900_to_mariadb_1400_mapping mysql_0900_mapping[]=
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


LEX_CSTRING
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
