#ifndef VERS_UTILS_INCLUDED
#define VERS_UTILS_INCLUDED

#include "table.h"
#include "sql_class.h"

class MDL_auto_lock
{
  THD *thd;
  TABLE_LIST &table;
  bool error;

public:
  MDL_auto_lock(THD *_thd, TABLE_LIST &_table) :
    thd(_thd), table(_table)
  {
    DBUG_ASSERT(thd);
    table.mdl_request.init(MDL_key::TABLE, table.db, table.table_name, MDL_EXCLUSIVE, MDL_EXPLICIT);
    error= thd->mdl_context.acquire_lock(&table.mdl_request, thd->variables.lock_wait_timeout);
  }
  ~MDL_auto_lock()
  {
    if (!error)
    {
      DBUG_ASSERT(table.mdl_request.ticket);
      thd->mdl_context.release_lock(table.mdl_request.ticket);
      table.mdl_request.ticket= NULL;
    }
  }
  bool acquire_error() const { return error; }
};

struct Compare_strncmp
{
  int operator()(const LEX_STRING& a, const LEX_STRING& b) const
  {
    return strncmp(a.str, b.str, a.length);
  }
  static CHARSET_INFO* charset()
  {
    return system_charset_info;
  }
};

template <CHARSET_INFO* &CS= system_charset_info>
struct Compare_my_strcasecmp
{
  int operator()(const LEX_STRING& a, const LEX_STRING& b) const
  {
    DBUG_ASSERT(a.str[a.length] == 0 && b.str[b.length] == 0);
    return my_strcasecmp(CS, a.str, b.str);
  }
  static CHARSET_INFO* charset()
  {
    return CS;
  }
};

typedef Compare_my_strcasecmp<files_charset_info> Compare_fs;
typedef Compare_my_strcasecmp<table_alias_charset> Compare_t;

struct LEX_STRING_u : public LEX_STRING
{
  LEX_STRING_u()
  {
    str= NULL;
    LEX_STRING::length= 0;
  }
  LEX_STRING_u(const char *_str, uint32 _len, CHARSET_INFO *)
  {
    str= const_cast<char *>(_str);
    LEX_STRING::length= _len;
  }
  uint32 length() const
  {
    return LEX_STRING::length;
  }
  const char *ptr() const
  {
    return LEX_STRING::str;
  }
  const LEX_STRING& lex_string() const
  {
    return *this;
  }
};

template <class Compare= Compare_strncmp, class Storage= LEX_STRING_u>
struct XString : public Storage
{
public:
  XString() {}
  XString(char *_str, size_t _len) :
    Storage(_str, _len, Compare::charset())
  {
  }
  XString(LEX_STRING& src) :
    Storage(src.str, src.length, Compare::charset())
  {
  }
  XString(char *_str) :
    Storage(_str, strlen(_str), Compare::charset())
  {
  }
  bool operator== (const XString& b) const
  {
    return Storage::length() == b.length() && 0 == Compare()(this->lex_string(), b.lex_string());
  }
  bool operator!= (const XString& b) const
  {
    return !(*this == b);
  }
  operator const char* () const
  {
    return Storage::ptr();
  }
};

typedef XString<> LString;
typedef XString<Compare_fs> LString_fs;

typedef XString<Compare_strncmp, String> SString;
typedef XString<Compare_fs, String> SString_fs;
typedef XString<Compare_t, String> SString_t;


#define XSTRING_WITH_LEN(X) (X).ptr(), (X).length()
#define DB_WITH_LEN(X) (X).db, (X).db_length
#define TABLE_NAME_WITH_LEN(X) (X).table_name, (X).table_name_length


class Local_da : public Diagnostics_area
{
  THD *thd;
  uint sql_error;
  Diagnostics_area *saved_da;

public:
  Local_da(THD *_thd, uint _sql_error= 0) :
    Diagnostics_area(_thd->query_id, false, true),
    thd(_thd),
    sql_error(_sql_error),
    saved_da(_thd->get_stmt_da())
  {
    thd->set_stmt_da(this);
  }
  ~Local_da()
  {
    if (saved_da)
      finish();
  }
  void finish()
  {
    DBUG_ASSERT(saved_da && thd);
    thd->set_stmt_da(saved_da);
    if (is_error())
      my_error(sql_error ? sql_error : sql_errno(), MYF(0), message());
    if (warn_count() > error_count())
      saved_da->copy_non_errors_from_wi(thd, get_warning_info());
    saved_da= NULL;
  }
};


#endif // VERS_UTILS_INCLUDED
