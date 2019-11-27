/*
   Copyright (c) 2001, 2013, Oracle and/or its affiliates.
   Copyright (c) 2010, 2019, MariaDB

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
   Character set conversion utility
*/

#include "mariadb.h"
#include "client_priv.h"

#define CONV_VERSION "1.0"


class CmdOpt
{
public:
  const char *m_charset_from;
  const char *m_charset_to;
  my_bool m_continue;
  CmdOpt()
   :m_charset_from("latin1"),
    m_charset_to("latin1"),
    m_continue(FALSE)
  { }
  static CHARSET_INFO *csinfo_by_name(const char *csname)
  {
    return get_charset_by_csname(csname, MY_CS_PRIMARY, MYF(0));
  }
  CHARSET_INFO *csinfo_from() const
  {
    return m_charset_from ? csinfo_by_name(m_charset_from) : NULL;
  }
  CHARSET_INFO *csinfo_to() const
  {
    return m_charset_to ? csinfo_by_name(m_charset_to) : NULL;
  }
};


static CmdOpt opt;


static struct my_option long_options[] =
{
  {"from", 'f', "Specifies the encoding of the input.", &opt.m_charset_from,
   &opt.m_charset_from, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"to", 't', "Specifies the encoding of the output.", &opt.m_charset_to,
   &opt.m_charset_to, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"continue", 'c', "When this option is given, characters that cannot be "
   "converted are silently discarded, instead of leading to a conversion error.",
   &opt.m_continue, &opt.m_continue, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}
};


my_bool
get_one_option(const struct my_option *opt,
               char *value, const char *filename)
{
  return 0;
}


class Conv
{
  CHARSET_INFO *m_tocs;
  CHARSET_INFO *m_fromcs;
  bool m_continue;
public:
  Conv(CHARSET_INFO *tocs, CHARSET_INFO *fromcs, bool opt_continue)
   :m_tocs(tocs), m_fromcs(fromcs), m_continue(opt_continue)
  { }
  bool convert_file(FILE *infile) const;
  bool convert_file_by_name(const char *filename) const;
};


bool Conv::convert_file(FILE *infile) const
{
  char from[FN_REFLEN + 1], to[FN_REFLEN + 2];

  while (fgets(from, sizeof(from), infile) != NULL)
  {
    uint errors;
    size_t length= 0;
    for (char *s= from; s < from + sizeof(from); s++)
    {
      if (*s == '\0' || *s == '\r' || *s == '\n')
      {
        *s= '\0';
        length= s - from;
        break;
      }
    }

    if (!length)
    {
      puts("");
      continue;
    }

    length= my_convert(to, (uint32) (sizeof(to) - 1), m_tocs,
                       from, (uint32) length, m_fromcs,
                       &errors);
    to[length]= '\0';
    if (unlikely(!length || errors) && !m_continue)
      return true;
    else
      puts(to);
  }

  return false;
} /* convert */


bool Conv::convert_file_by_name(const char *filename) const
{
  FILE *fp;
  if ((fp= fopen(filename, "r")) == NULL)
  {
    printf("can't open file %s", filename);
    return 1;
  }
  bool rc= convert_file(fp);
  fclose(fp);
  return rc;
}


class Session
{
public:
  Session(const char *prog)
  {
    MY_INIT(prog);
  }
  ~Session()
  {
    my_end(0);
  }
  void usage(void)
  {
    printf("%s Ver %s Distrib %s for %s on %s\n", my_progname, CONV_VERSION,
      MYSQL_SERVER_VERSION, SYSTEM_TYPE, MACHINE_TYPE);
    puts("Character set conversion utility for MariaDB");
    puts("Usage:");
    printf("%s [-f encoding] [-t encoding] [inputfile]...\n", my_progname);
    my_print_help(long_options);
  }
};


int main(int argc, char *argv[])
{
  Session session(argv[0]);
  CHARSET_INFO *charset_info_from= NULL;
  CHARSET_INFO *charset_info_to= NULL;

  if (handle_options(&argc, &argv, long_options, get_one_option))
  {
    session.usage();
    return 1;
  }

  if (!(charset_info_from= opt.csinfo_from()))
  {
    fprintf(stderr, "Character set %s is not supported\n", opt.m_charset_from);
    return 1;
  }

  if (!(charset_info_to= opt.csinfo_to()))
  {
    fprintf(stderr, "Character set %s is not supported\n", opt.m_charset_to);
    return 1;
  }

  Conv conv(charset_info_to, charset_info_from, opt.m_continue);

  if (argc == 0)
  {
    if (conv.convert_file(stdin))
      return 1;
  }
  else
  {
    for (int i= 0; i < argc; i++)
    {
      if (conv.convert_file_by_name(argv[i]))
       return 1;
    }
  }

  return 0;
} /* main */
