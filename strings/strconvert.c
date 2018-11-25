/*
   Copyright (c) 2010, 2018, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

/*
  Converts a string between character sets

  SYNOPSIS
    strconvert()
    from_cs       source character set
    from          source, a null terminated string
    to            destination buffer
    to_length     destination buffer length

  NOTES
    'to' is always terminated with a '\0' character.
    If there is no enough space to convert whole string,
    only prefix is converted, and terminated with '\0'.

  RETURN VALUES
    result string length
*/

#include "strings_def.h"
#include "m_ctype.h"

uint strconvert(CHARSET_INFO *from_cs, const char *from, size_t from_length,
                CHARSET_INFO *to_cs, char *to, size_t to_length, uint *errors)
{
  int cnvres;
  my_wc_t wc;
  char *to_start= to;
  uchar *to_end= (uchar*) to + to_length - 1;
  const uchar *from_end= (const uchar*) from + from_length;
  my_charset_conv_mb_wc mb_wc= from_cs->cset->mb_wc;
  my_charset_conv_wc_mb wc_mb= to_cs->cset->wc_mb;
  uint error_count= 0;

  while (1)
  {
    if ((cnvres= (*mb_wc)(from_cs, &wc,
                          (uchar*) from, from_end)) > 0)
    {
      if (!wc)
        break;
      from+= cnvres;
    }
    else if (cnvres == MY_CS_ILSEQ)
    {
      error_count++;
      from++;
      wc= '?';
    }
    else
      break; // Impossible char.

outp:

    if ((cnvres= (*wc_mb)(to_cs, wc, (uchar*) to, to_end)) > 0)
      to+= cnvres;
    else if (cnvres == MY_CS_ILUNI && wc != '?')
    {
      error_count++;
      wc= '?';
      goto outp;
    }
    else
      break;
  }
  *to= '\0';
  *errors= error_count;
  return (uint32) (to - to_start);

}
