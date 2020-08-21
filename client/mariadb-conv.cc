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
#include "sql_string.h"
#include "my_dir.h"

#define CONV_VERSION "1.0"


class CmdOpt
{
public:
  const char *m_charset_from;
  const char *m_charset_to;
  const char *m_delimiter;
  my_bool m_continue;
  CmdOpt()
   :m_charset_from("latin1"),
    m_charset_to("latin1"),
    m_delimiter(NULL),
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
  {"continue", 'c', "Silently ignore conversion errors.",
   &opt.m_continue, &opt.m_continue, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"delimiter", 0, "Treat the specified characters as delimiters.",
    &opt.m_delimiter, &opt.m_delimiter, 0, GET_STR, REQUIRED_ARG,
    0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}
};


my_bool
get_one_option(const struct my_option *opt,
               char *value, const char *filename)
{
  return 0;
}


class File_buffer: public Binary_string
{
public:
  bool load_binary_stream(FILE *file);
  bool load_binary_file_by_name(const char *file);
};


/*
  Load data from a binary stream whose length is not known in advance,
  e.g. from stdin.
*/
bool File_buffer::load_binary_stream(FILE *file)
{
  for ( ; ; )
  {
    char buf[1024];
    if (length() + sizeof(buf) > UINT_MAX32 || reserve(sizeof(buf)))
    {
      fprintf(stderr, "Input data is too large\n");
      return true;
    }
    size_t nbytes= my_fread(file, (uchar *) end(), sizeof(buf), MYF(0));
    if (!nbytes || nbytes == (size_t) -1)
      return false;
    str_length+= (uint32) nbytes;
  }
  return false;
}


/*
  Load data from a file by name.
  The file size is know.
*/
bool File_buffer::load_binary_file_by_name(const char *filename)
{
  MY_STAT sbuf;
  File fd;

  if (!my_stat(filename, &sbuf, MYF(0)))
  {
    fprintf(stderr, "my_stat failed for '%s'\n", filename);
    return true;
  }
 
  if (!MY_S_ISREG(sbuf.st_mode))
  {
    fprintf(stderr, "'%s' is not a regular file\n", filename);
    return true;
  }

  if ((size_t) sbuf.st_size > UINT_MAX32)
  {
    fprintf(stderr, "File '%s' is too large\n", filename);
    return true;
  }

  if (alloc((uint32) sbuf.st_size))
  {
    fprintf(stderr, "Failed to allocate read buffer\n");
    return true;
  }

  if ((fd= my_open(filename, O_RDONLY, MYF(0))) == -1)
  {
    fprintf(stderr, "Could not open '%s'\n", filename);
    return true;
  }

  size_t nbytes= my_read(fd, (uchar*) Ptr, (size_t)sbuf.st_size, MYF(0));
  my_close(fd, MYF(0));
  length((uint32) nbytes);

  return false;
}


class Delimiter
{
protected:
  bool m_delimiter[127];
  bool m_has_delimiter_cached;
  bool has_delimiter_slow() const
  {
    for (size_t i= 0; i < sizeof(m_delimiter); i++)
    {
      if (m_delimiter[i])
        return true;
    }
    return false;
  }
  bool unescape(char *to, char from) const
  {
    switch (from) {
    case '\\': *to= '\\'; return false;
    case 'r':  *to= '\r'; return false;
    case 'n':  *to= '\n'; return false;
    case 't':  *to= '\t'; return false;
    case '0':  *to= '\0'; return false;
    }
    *to= '\0';
    return true;
  }
  bool is_delimiter(char ch) const
  {
    return (signed char) ch < 0 ? false : m_delimiter[(uint32) ch];
  }
public:
  Delimiter()
   :m_has_delimiter_cached(false)
  {
    bzero(&m_delimiter, sizeof(m_delimiter));
  }
  bool has_delimiter() const
  {
    return m_has_delimiter_cached;
  }
  bool set_delimiter_unescape(const char *str)
  {
    m_has_delimiter_cached= false;
    for ( ; *str; str++)
    {
      if ((signed char) *str < 0)
        return true;
      if (*str == '\\')
      {
        char unescaped;
        str++;
        if (!*str || unescape(&unescaped, *str))
          return true;
        m_delimiter[(uint) unescaped]= true;
      }
      else
        m_delimiter[(uint) *str]= true;
    }
    m_has_delimiter_cached= has_delimiter_slow();
    return false;
  }
  size_t get_delimiter_length(const char *str, const char *end) const
  {
    const char *str0= str;
    for ( ; str < end; str++)
    {
      if (!is_delimiter(*str))
        break;
    }
    return str - str0;
  }
  size_t get_data_length(const char *str, const char *end) const
  {
    const char *str0= str;
    for ( ; str < end; str++)
    {
      if (is_delimiter(*str))
        break;
    }
    return str - str0;
  }
};


class Conv_inbuf
{
  const char *m_ptr;
  const char *m_end;
public:
  Conv_inbuf(const char *from, size_t length)
   :m_ptr(from), m_end(from + length)
  { }
  const char *ptr() const { return m_ptr; }
  const char *end() const { return m_end; }
  size_t length() const
  {
    return m_end - m_ptr;
  }
private:
  LEX_CSTRING get_prefix(size_t len)
  {
    LEX_CSTRING res;
    res.str= ptr();
    res.length= len;
    m_ptr+= len;
    return res;
  }
  LEX_CSTRING get_empty_string() const
  {
    static LEX_CSTRING str= {NULL, 0};
    return str;
  }
public:
  LEX_CSTRING get_delimiter_chunk(const Delimiter &delimiter)
  {
    if (!delimiter.has_delimiter())
      return get_empty_string();
    size_t len= delimiter.get_delimiter_length(ptr(), end());
    return get_prefix(len);
  }
  LEX_CSTRING get_data_chunk(const Delimiter &delimiter)
  {
    if (!delimiter.has_delimiter())
      return get_prefix(length());
    size_t len= delimiter.get_data_length(ptr(), end());
    return get_prefix(len);
  }
};


class Conv_outbuf: public Binary_string
{
public:
  bool alloc(size_t out_max_length)
  {
    if (out_max_length >= UINT_MAX32)
    {
      fprintf(stderr, "The data needs a too large output buffer\n");
      return true;
    }
    if (Binary_string::alloc((uint32) out_max_length))
    {
      fprintf(stderr, "Failed to allocate the output buffer\n");
      return true;
    }
    return false;
  }
};


class Conv: public String_copier, public Delimiter
{
  CHARSET_INFO *m_tocs;
  CHARSET_INFO *m_fromcs;
  bool m_continue;
public:
  Conv(CHARSET_INFO *tocs, CHARSET_INFO *fromcs, bool opt_continue)
   :m_tocs(tocs), m_fromcs(fromcs), m_continue(opt_continue)
  { }
  size_t out_buffer_max_length(size_t from_length) const
  {
    return from_length / m_fromcs->mbminlen * m_tocs->mbmaxlen;
  }
  bool convert_data(const char *from, size_t length);
  bool convert_binary_stream(FILE *file)
  {
    File_buffer buf;
    return buf.load_binary_stream(file) ||
           convert_data(buf.ptr(), buf.length());
  }
  bool convert_binary_file_by_name(const char *filename)
  {
    File_buffer buf;
    return buf.load_binary_file_by_name(filename)||
           convert_data(buf.ptr(), buf.length());
  }
private:
  void report_error(const char *from) const
  {
    if (well_formed_error_pos())
    {
      fflush(stdout);
      fprintf(stderr,
              "Illegal %s byte sequence at position %d\n",
              m_fromcs->cs_name.str,
              (uint) (well_formed_error_pos() - from));
    }
    else if (cannot_convert_error_pos())
    {
      fflush(stdout);
      fprintf(stderr,
              "Conversion from %s to %s failed at position %d\n",
              m_fromcs->cs_name.str, m_tocs->cs_name.str,
              (uint) (cannot_convert_error_pos() - from));
    }
  }
  size_t write(const char *str, size_t length) const
  {
    return my_fwrite(stdout, (uchar *) str, length, MY_WME);
  }
};


bool Conv::convert_data(const char *from, size_t from_length)
{
  Conv_inbuf inbuf(from, from_length);
  Conv_outbuf outbuf;

  if (outbuf.alloc(out_buffer_max_length(from_length)))
    return true;

  for ( ; ; )
  {
    LEX_CSTRING delim, data;
    
    delim= inbuf.get_delimiter_chunk(*this);
    if (delim.length)
      write(delim.str, delim.length);

    data= inbuf.get_data_chunk(*this);
    if (!data.length)
      break;
    size_t length= well_formed_copy(m_tocs,
                                    (char *) outbuf.ptr(),
                                    outbuf.alloced_length(),
                                    m_fromcs, data.str, data.length);
    outbuf.length((uint32) length);

    if (most_important_error_pos() && !m_continue)
    {
      report_error(from);
      return true;
    }
    write(outbuf.ptr(), outbuf.length());
  }
  return false;
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
    printf("%s [OPTION...] [FILE...]\n", my_progname);
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
  if (opt.m_delimiter)
  {
    if (charset_info_from->mbminlen > 1 ||
        charset_info_to->mbminlen > 1)
    {
      fprintf(stderr, "--delimiter cannot be used with %s to %s conversion\n",
              charset_info_from->cs_name.str, charset_info_to->cs_name.str);
      return 1;
    }
    if (conv.set_delimiter_unescape(opt.m_delimiter))
    {
      fprintf(stderr, "Bad --delimiter value\n");
      return 1;
    }
  }

  if (argc == 0)
  {
    if (conv.convert_binary_stream(stdin))
      return 1;
  }
  else
  {
    for (int i= 0; i < argc; i++)
    {
      if (conv.convert_binary_file_by_name(argv[i]))
       return 1;
    }
  }

  return 0;
} /* main */
