#ifndef VTMD_INCLUDED
#define VTMD_INCLUDED

#include <mysqld_error.h>

#include "mariadb.h"
#include "sql_priv.h"

#include "my_sys.h"
#include "table.h"
#include "unireg.h"

#include "vers_utils.h"

class key_buf_t
{
  uchar* buf;

  key_buf_t(const key_buf_t&); // disabled
  key_buf_t& operator= (const key_buf_t&); // disabled

public:
  key_buf_t() : buf(NULL)
  {}

  ~key_buf_t()
  {
    if (buf)
      my_free(buf);
  }

  bool allocate(size_t alloc_size)
  {
    DBUG_ASSERT(!buf);
    buf= static_cast<uchar *>(my_malloc(alloc_size, MYF(0)));
    if (!buf)
    {
      my_message(ER_VERS_VTMD_ERROR, "failed to allocate key buffer", MYF(0));
      return true;
    }
    return false;
  }

  operator uchar* ()
  {
    DBUG_ASSERT(buf);
    return reinterpret_cast<uchar *>(buf);
  }
};

class THD;

class VTMD_table
{
  Open_tables_backup open_tables_backup;

protected:
  TABLE_LIST vtmd;
  TABLE_LIST &about;
  SString_t vtmd_name;

private:
  VTMD_table(const VTMD_table&); // prohibit copying references

public:
  enum {
    FLD_START= 0,
    FLD_END,
    FLD_NAME,
    FLD_ARCHIVE_NAME,
    FLD_COL_RENAMES,
    FIELD_COUNT
  };

  enum {
    IDX_TRX_END= 0,
    IDX_ARCHIVE_NAME
  };

  VTMD_table(TABLE_LIST &_about) :
    about(_about)
  {
    vtmd.table= NULL;
  }

  bool create(THD *thd);
  bool find_record(ulonglong row_end, bool &found);
  bool open(THD *thd, Local_da &local_da, bool *created= NULL);
  bool update(THD *thd, const char* archive_name= NULL);
  bool setup_select(THD *thd);

  static void archive_name(THD *thd, const char *table_name, char *new_name, size_t new_name_size);
  void archive_name(THD *thd, char *new_name, size_t new_name_size)
  {
    archive_name(thd, about.table_name.str, new_name, new_name_size);
  }

  bool find_archive_name(THD *thd, String &out);
  static bool get_archive_tables(THD *thd, const char *db, size_t db_length,
                                 Dynamic_array<String> &result);
};

class VTMD_exists : public VTMD_table
{
protected:
  handlerton *hton;

public:
  bool exists;

public:
  VTMD_exists(TABLE_LIST &_about) :
    VTMD_table(_about),
    hton(NULL),
    exists(false)
  {}

  bool check_exists(THD *thd); // returns error status
};

class VTMD_rename : public VTMD_exists
{
  SString_t vtmd_new_name;

public:
  VTMD_rename(TABLE_LIST &_about) :
    VTMD_exists(_about)
  {}

  bool try_rename(THD *thd, LString new_db, LString new_alias, const char* archive_name= NULL);
  bool revert_rename(THD *thd, LString new_db);

private:
  bool move_archives(THD *thd, LString &new_db);
  bool move_table(THD *thd, SString_fs &table_name, LString &new_db);
};

class VTMD_drop : public VTMD_exists
{
  char archive_name_[NAME_CHAR_LEN];

public:
  VTMD_drop(TABLE_LIST &_about) :
    VTMD_exists(_about)
  {
    *archive_name_= 0;
  }

  const char* archive_name(THD *thd)
  {
    VTMD_table::archive_name(thd, archive_name_, sizeof(archive_name_));
    return archive_name_;
  }

  const char* archive_name() const
  {
    DBUG_ASSERT(*archive_name_);
    return archive_name_;
  }

  bool update(THD *thd)
  {
    DBUG_ASSERT(*archive_name_);
    return VTMD_exists::update(thd, archive_name_);
  }
};


inline
bool
VTMD_exists::check_exists(THD *thd)
{
  LEX_CSTRING name;
  if (about.vers_vtmd_name(vtmd_name))
    return true;

  name.str=    vtmd_name.ptr();
  name.length= vtmd_name.length();
  exists= ha_table_exists(thd, &about.db, &name, &hton);

  if (exists && !hton)
  {
    my_printf_error(ER_VERS_VTMD_ERROR, "`%s.%s` handlerton empty!", MYF(0),
                        about.db.str, vtmd_name.ptr());
    return true;
  }
  return false;
}

#endif // VTMD_INCLUDED
