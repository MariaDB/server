#include"table.h"
#include"sql_lex.h"
#include"protocol.h"
#include"sql_plugin.h"
#include"sql_insert.h"

#define MYSQL_SERVER 1
#define MYSQL_OPEN_FORCE_SHARED_HIGH_PRIO_MDL 0x0400

typedef enum
{
  WITHOUT_DB_NAME,
  WITH_DB_NAME
} enum_with_db_name;

bool append_identifier(String *packet, const char *name,
                       size_t length)
{
  const char *name_end;
  char quote_char;
  int q= get_quote_char_for_identifier(thd, name, length); //// ?????????

  if (q == EOF)
    return packet->append(name, length, packet->charset());

  /*
    The identifier must be quoted as it includes a quote character or
    it's a keyword
  */

  /*
    Special code for swe7. It encodes the letter "E WITH ACUTE" on
    the position 0x60, where backtick normally resides.
    In swe7 we cannot append 0x60 using system_charset_info,
    because it cannot be converted to swe7 and will be replaced to
    question mark '?'. Use &my_charset_bin to avoid this.
    It will prevent conversion and will append the backtick as is.
  */
  CHARSET_INFO *quote_charset=
      q == 0x60 && (packet->charset()->state & MY_CS_NONASCII) &&
              packet->charset()->mbmaxlen == 1
          ? &my_charset_bin  //// ?????????
          : system_charset_info;

  (void) packet->reserve(length * 2 + 2);
  quote_char= (char) q;
  if (packet->append(&quote_char, 1, quote_charset))
    return true;

  for (name_end= name + length; name < name_end;)
  {
    uchar chr= (uchar) *name;
    int char_length= system_charset_info->charlen(name, name_end);
    /*
      charlen can return 0 and negative numbers on a wrong multibyte
      sequence. It is possible when upgrading from 4.0,
      and identifier contains some accented characters.
      The manual says it does not work. So we'll just
      change char_length to 1 not to hang in the endless loop.
    */
    if (char_length <= 0)
      char_length= 1;
    if (char_length == 1 && chr == (uchar) quote_char &&
        packet->append(&quote_char, 1, quote_charset))
      return true;
    if (packet->append(name, char_length, system_charset_info))
      return true;
    name+= char_length;
  }
  return packet->append(&quote_char, 1, quote_charset);
}

static inline bool append_identifier(String *packet,
                                     const LEX_CSTRING *name)
{
  return append_identifier(packet, name->str, name->length);
}

static bool append_at_host(String *buffer, const LEX_CSTRING *host)
{
  if (!host->str || !host->str[0])
    return false;
  return buffer->append('@') || append_identifier(buffer, host);
}

bool append_definer(String *buffer, const LEX_CSTRING *definer_user,
                    const LEX_CSTRING *definer_host)
{
  return buffer->append(STRING_WITH_LEN("DEFINER=")) ||
         append_identifier(buffer, definer_user) ||
         append_at_host(buffer, definer_host) || buffer->append(' ');
}

int show_create_table(TABLE_LIST *table_list, String *packet,
                      Table_specification_st *create_info_arg,
                      enum_with_db_name with_db_name)
{
    //TODO
  //return show_create_table_ex(thd, table_list, NULL, NULL, packet,
  //                            create_info_arg, with_db_name);
}
