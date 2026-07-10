/*
   Copyright (c) 2024, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA
*/


#ifndef MY_FUNCTION_NAME
#error MY_FUNCTION_NAME is not defined
#endif

#ifndef MY_MB_WC
#error MY_MB_WC is not defined
#endif

#ifndef MY_CHAR_EQ
#error MY_CHAR_EQ is not defined
#endif

/*
** Compare string against string with wildcard
**
**	0 if matched
**	-1 if not matched with wildcard
**	 1 if matched with wildcard
*/

static int
MY_FUNCTION_NAME(wildcmp)(CHARSET_INFO *cs,
                          const char *str, const char *str_end,
                          const char *wildstr,const char *wildend,
                          int escape, int w_one, int w_many,
                          int recurse_level)
{
  int result= -1;                             /* Not found, using wildcards */
  my_wc_t s_wc, w_wc;
  int scan;

  if (my_string_stack_guard && my_string_stack_guard(recurse_level))
    return 1;
  while (wildstr != wildend)
  {
    while (1)
    {
      my_bool escaped= 0;
      if ((scan= MY_MB_WC(cs, &w_wc, (const uchar*) wildstr,
                          (const uchar*) wildend)) <= 0)
        return 1;

      if (w_wc == (my_wc_t) w_many)
      {
        result= 1;                                /* Found an anchor char */
        break;
      }

      wildstr+= scan;
      if (w_wc ==  (my_wc_t) escape && wildstr < wildend)
      {
        if ((scan= MY_MB_WC(cs, &w_wc, (const uchar*) wildstr,
                            (const uchar*) wildend)) <= 0)
          return 1;
        wildstr+= scan;
        escaped= 1;
      }

      if ((scan= MY_MB_WC(cs, &s_wc, (const uchar*) str,
                          (const uchar*) str_end)) <= 0)
        return 1;
      str+= scan;

      if (!escaped && w_wc == (my_wc_t) w_one)
      {
        result= 1;                                /* Found an anchor char */
      }
      else
      {
        if (!MY_CHAR_EQ(cs, s_wc, w_wc))
          return 1;                               /* No match */
      }
      if (wildstr == wildend)
        return (str != str_end);                  /* Match if both are at end */
    }

    if (w_wc == (my_wc_t) w_many)
    {                                             /* Found w_many */
      /* Remove any '%' and '_' from the wild search string */
      for ( ; wildstr != wildend ; )
      {
        if ((scan= MY_MB_WC(cs, &w_wc, (const uchar*) wildstr,
                            (const uchar*) wildend)) <= 0)
          return 1;

        if (w_wc == (my_wc_t) w_many)
        {
          wildstr+= scan;
          continue;
        }

        if (w_wc == (my_wc_t) w_one)
        {
          wildstr+= scan;
          if ((scan= MY_MB_WC(cs, &s_wc, (const uchar*) str,
                              (const uchar*) str_end)) <= 0)
            return 1;
          str+= scan;
          continue;
        }
        break;                                        /* Not a wild character */
      }

      if (wildstr == wildend)
        return 0;                                /* Ok if w_many is last */

      if (str == str_end)
        return -1;

      if ((scan= MY_MB_WC(cs, &w_wc, (const uchar*) wildstr,
                          (const uchar*) wildend)) <= 0)
        return 1;
      wildstr+= scan;

      if (w_wc ==  (my_wc_t) escape)
      {
        if (wildstr < wildend)
        {
          if ((scan= MY_MB_WC(cs, &w_wc, (const uchar*) wildstr,
                              (const uchar*) wildend)) <= 0)
            return 1;
          wildstr+= scan;
        }
      }

      while (1)
      {
        /* Skip until the first character from wildstr is found */
        while (str != str_end)
        {
          if ((scan= MY_MB_WC(cs, &s_wc, (const uchar*) str,
                              (const uchar*) str_end)) <= 0)
            return 1;

          if (MY_CHAR_EQ(cs, s_wc, w_wc))
            break;
          str+= scan;
        }
        if (str == str_end)
          return -1;

        str+= scan;
        result= MY_FUNCTION_NAME(wildcmp)(cs,
                                          str, str_end,
                                          wildstr, wildend,
                                          escape, w_one, w_many,
                                          recurse_level + 1);
        if (result <= 0)
          return result;
      }
    }
  }
  return (str != str_end ? 1 : 0);
}


#undef MY_FUNCTION_NAME
#undef MY_MB_WC
#undef MY_CHAR_EQ
