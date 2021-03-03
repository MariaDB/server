/* Copyright (c) 2010, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */


#include "mariadb.h"
#include <ctype.h>
#include <string.h>
#include "sql_bootstrap.h"
#include <string>

static bool is_end_of_query(const char *line, size_t len,
                            const std::string& delimiter)
{
  if (delimiter.length() > len)
    return false;
  return !strcmp(line + len-delimiter.length(),delimiter.c_str());
}

static std::string delimiter= ";";
extern "C" int read_bootstrap_query(char *query, int *query_length,
                         fgets_input_t input, fgets_fn_t fgets_fn,
                         int preserve_delimiter, int *error)
{
  char line_buffer[MAX_BOOTSTRAP_LINE_SIZE];
  const char *line;
  size_t len;
  size_t query_len= 0;
  int fgets_error= 0;
  *error= 0;

  *query_length= 0;
  for ( ; ; )
  {
    line= (*fgets_fn)(line_buffer, sizeof(line_buffer), input, &fgets_error);
    
    if (error)
      *error= fgets_error;

    if (fgets_error != 0)
      return READ_BOOTSTRAP_ERROR;
      
    if (line == NULL)
      return (query_len == 0) ? READ_BOOTSTRAP_EOF : READ_BOOTSTRAP_ERROR;

    len= strlen(line);

    /*
      Remove trailing whitespace characters.
      This assumes:
      - no multibyte encoded character can be found at the very end of a line,
      - whitespace characters from the "C" locale only.
     which is sufficient for the kind of queries found
     in the bootstrap scripts.
    */
    while (len && (isspace(line[len - 1])))
      len--;
    /*
      Cleanly end the string, so we don't have to test len > x
      all the time before reading line[x], in the code below.
    */
    line_buffer[len]= '\0';

    /* Skip blank lines */
    if (len == 0)
      continue;

    /* Skip # comments */
    if (line[0] == '#')
      continue;
    
    /* Skip -- comments */
    if ((line[0] == '-') && (line[1] == '-'))
      continue;

    size_t i=0;
    while (line[i] == ' ')
     i++;

    /* Skip -- comments */
    if (line[i] == '-' && line[i+1] == '-')
      continue;

    if (strncmp(line, "DELIMITER", 9) == 0)
    {
      const char *p= strrchr(line,' ');
      if (!p || !p[1])
      {
        /* Invalid DELIMITER specifier */
        return READ_BOOTSTRAP_ERROR;
      }
      delimiter.assign(p+1);
      if (preserve_delimiter)
      {
        memcpy(query,line,len);
        query[len]=0;
        *query_length = (int)len;
        return READ_BOOTSTRAP_SUCCESS;
      }
      continue;
    }

    /* Append the current line to a multi line query. If the new line will make
       the query too long, preserve the partial line to provide context for the
       error message.
    */
    if (query_len + len + 1 >= MAX_BOOTSTRAP_QUERY_SIZE)
    {
      size_t new_len= MAX_BOOTSTRAP_QUERY_SIZE - query_len - 1;
      if ((new_len > 0) && (query_len < MAX_BOOTSTRAP_QUERY_SIZE))
      {
        memcpy(query + query_len, line, new_len);
        query_len+= new_len;
      }
      query[query_len]= '\0';
      *query_length= (int)query_len;
      return READ_BOOTSTRAP_QUERY_SIZE;
    }

    if (query_len != 0)
    {
      /*
        Append a \n to the current line, if any,
        to preserve the intended presentation.
       */
      query[query_len++]= '\n';
    }
    memcpy(query + query_len, line, len);
    query_len+= len;

    if (is_end_of_query(line, len, delimiter))
    {
      /*
        The last line is terminated by delimiter
        Return the query found.
      */
      if (!preserve_delimiter)
      {
        query_len-= delimiter.length();
        query[query_len++]= ';';
      }
      query[query_len]= 0;
      *query_length= (int)query_len;
      return READ_BOOTSTRAP_SUCCESS;
    }
  }
}

