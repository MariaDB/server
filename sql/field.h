#ifndef FIELD_INCLUDED
#define FIELD_INCLUDED
/* Copyright (c) 2000, 2015, Oracle and/or its affiliates.
   Copyright (c) 2008, 2021, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

/*
  Because of the function make_new_field() all field classes that have static
  variables must declare the size_of() member function.
*/

#ifdef USE_PRAGMA_INTERFACE
#pragma interface			/* gcc class implementation */
#endif

#include "mysqld.h"                             /* system_charset_info */
#include "table.h"                              /* TABLE */
#include "sql_string.h"                         /* String */
#include "my_decimal.h"                         /* my_decimal */
#include "sql_error.h"                          /* Sql_condition */
#include "compat56.h"
#include "sql_type.h"                           /* Type_std_attributes */
#include "field_comp.h"

class Send_field;
class Copy_field;
class Protocol;
class Protocol_text;
class Create_field;
class Relay_log_info;
class Field;
class Column_statistics;
class Column_statistics_collected;
class Item_func;
class Item_bool_func;
class Item_equal;
class Virtual_tmp_table;
class Qualified_column_ident;
class Table_ident;
class SEL_ARG;
class RANGE_OPT_PARAM;
struct KEY_PART;
struct SORT_FIELD;
struct SORT_FIELD_ATTR;

enum enum_check_fields
{
  CHECK_FIELD_IGNORE,
  CHECK_FIELD_EXPRESSION,
  CHECK_FIELD_WARN,
  CHECK_FIELD_ERROR_FOR_NULL,
};


enum enum_conv_type
{
  CONV_TYPE_PRECISE,
  CONV_TYPE_VARIANT,
  CONV_TYPE_SUBSET_TO_SUPERSET,
  CONV_TYPE_SUPERSET_TO_SUBSET,
  CONV_TYPE_IMPOSSIBLE
};


class Conv_param
{
  uint16 m_table_def_flags;
public:
  Conv_param(uint16 table_def_flags)
   :m_table_def_flags(table_def_flags)
  { }
  uint16 table_def_flags() const { return m_table_def_flags; }
};


class Conv_source: public Type_handler_hybrid_field_type
{
  uint16 m_metadata;
  CHARSET_INFO *m_cs;
public:
  Conv_source(const Type_handler *h, uint16 metadata, CHARSET_INFO *cs)
   :Type_handler_hybrid_field_type(h),
    m_metadata(metadata),
    m_cs(cs)
  {
    DBUG_ASSERT(cs);
  }
  uint16 metadata() const { return m_metadata; }
  uint mbmaxlen() const { return m_cs->mbmaxlen; }
};


/*
  Common declarations for Field and Item
*/
class Value_source
{
protected:

  // Parameters for warning and note generation
  class Warn_filter
  {
    bool m_want_warning_edom;
    bool m_want_note_truncated_spaces;
  public:
    Warn_filter(bool want_warning_edom, bool want_note_truncated_spaces) :
     m_want_warning_edom(want_warning_edom),
     m_want_note_truncated_spaces(want_note_truncated_spaces)
    { }
    Warn_filter(const THD *thd);
    bool want_warning_edom() const
    { return m_want_warning_edom; }
    bool want_note_truncated_spaces() const
    { return m_want_note_truncated_spaces; }
  };
  class Warn_filter_all: public Warn_filter
  {
  public:
    Warn_filter_all() :Warn_filter(true, true) { }
  };

  class Converter_double_to_longlong
  {
  protected:
    bool m_error;
    longlong m_result;
  public:
    Converter_double_to_longlong(double nr, bool unsigned_flag);
    longlong result() const { return m_result; }
    bool error() const { return m_error; }
    void push_warning(THD *thd, double nr, bool unsigned_flag);
  };
  class Converter_double_to_longlong_with_warn:
    public Converter_double_to_longlong
  {
  public:
    Converter_double_to_longlong_with_warn(THD *thd, double nr,
                                           bool unsigned_flag)
      :Converter_double_to_longlong(nr, unsigned_flag)
    {
      if (m_error)
        push_warning(thd, nr, unsigned_flag);
    }
    Converter_double_to_longlong_with_warn(double nr, bool unsigned_flag)
      :Converter_double_to_longlong(nr, unsigned_flag)
    {
      if (m_error)
        push_warning(current_thd, nr, unsigned_flag);
    }
  };

  // String-to-number converters
  class Converter_string_to_number
  {
  protected:
    char *m_end_of_num; // Where the low-level conversion routine stopped
    int m_error;        // The error code returned by the low-level routine
    bool m_edom;        // If EDOM-alike error happened during conversion
    /**
      Check string-to-number conversion and produce a warning if
      - could not convert any digits (EDOM-alike error)
      - found garbage at the end of the string
      - found extra spaces at the end (a note)
      See also Field_num::check_edom_and_truncation() for a similar function.

      @param thd         - the thread that will be used to generate warnings.
                           Can be NULL (which means current_thd will be used
                           if a warning is really necessary).
      @param type        - name of the data type
                           (e.g. "INTEGER", "DECIMAL", "DOUBLE")
      @param cs          - character set of the original string
      @param str         - the original string
      @param end         - the end of the string
      @param allow_notes - tells if trailing space notes should be displayed
                           or suppressed.

      Unlike Field_num::check_edom_and_truncation(), this function does not
      distinguish between EDOM and truncation and reports the same warning for
      both cases. Perhaps we should eventually print different warnings,
      to make the explicit CAST work closer to the implicit cast in
      Field_xxx::store().
    */
    void check_edom_and_truncation(THD *thd, Warn_filter filter,
                                   const char *type,
                                   CHARSET_INFO *cs,
                                   const char *str,
                                   size_t length) const;
  public:
    int error() const { return m_error; }
  };

  class Converter_strntod: public Converter_string_to_number
  {
    double m_result;
  public:
    Converter_strntod(CHARSET_INFO *cs, const char *str, size_t length)
    {
      m_result= cs->strntod((char *) str, length, &m_end_of_num, &m_error);
      // strntod() does not set an error if the input string was empty
      m_edom= m_error !=0 || str == m_end_of_num;
    }
    double result() const { return m_result; }
  };

  class Converter_string_to_longlong: public Converter_string_to_number
  {
  protected:
    longlong m_result;
  public:
    longlong result() const { return m_result; }
  };

  class Converter_strntoll: public Converter_string_to_longlong
  {
  public:
    Converter_strntoll(CHARSET_INFO *cs, const char *str, size_t length)
    {
      m_result= cs->strntoll(str, length, 10, &m_end_of_num, &m_error);
      /*
         All non-zero errors means EDOM error.
         strntoll() does not set an error if the input string was empty.
         Check it here.
         Notice the different with the same condition in Converter_strntoll10.
      */
      m_edom= m_error != 0 || str == m_end_of_num;
    }
  };

  class Converter_strtoll10: public Converter_string_to_longlong
  {
  public:
    Converter_strtoll10(CHARSET_INFO *cs, const char *str, size_t length)
    {
      m_end_of_num= (char *) str + length;
      m_result= cs->strtoll10(str, &m_end_of_num, &m_error);
      /*
        Negative error means "good negative number".
        Only a positive m_error value means a real error.
        strtoll10() sets error to MY_ERRNO_EDOM in case of an empty string,
        so we don't have to additionally catch empty strings here.
      */
      m_edom= m_error > 0;
    }
  };

  class Converter_str2my_decimal: public Converter_string_to_number
  {
  public:
    Converter_str2my_decimal(uint mask,
                             CHARSET_INFO *cs, const char *str, size_t length,
                             my_decimal *buf)
    {
      DBUG_ASSERT(length < UINT_MAX32);
      m_error= str2my_decimal(mask, str, length, cs,
                              buf, (const char **) &m_end_of_num);
      // E_DEC_TRUNCATED means a very minor truncation: '1e-100' -> 0
      m_edom= m_error && m_error != E_DEC_TRUNCATED;
    }
  };


  // String-to-number converters with automatic warning generation
  class Converter_strntod_with_warn: public Converter_strntod
  {
  public:
    Converter_strntod_with_warn(THD *thd, Warn_filter filter,
                                CHARSET_INFO *cs,
                                const char *str, size_t length)
      :Converter_strntod(cs, str, length)
    {
      check_edom_and_truncation(thd, filter, "DOUBLE", cs, str, length);
    }
  };

  class Converter_strntoll_with_warn: public Converter_strntoll
  {
  public:
    Converter_strntoll_with_warn(THD *thd, Warn_filter filter,
                                 CHARSET_INFO *cs,
                                 const char *str, size_t length)
      :Converter_strntoll(cs, str, length)
    {
      check_edom_and_truncation(thd, filter, "INTEGER", cs, str, length);
    }
  };

  class Converter_strtoll10_with_warn: public Converter_strtoll10
  {
  public:
    Converter_strtoll10_with_warn(THD *thd, Warn_filter filter,
                                 CHARSET_INFO *cs,
                                 const char *str, size_t length)
      :Converter_strtoll10(cs, str, length)
    {
      check_edom_and_truncation(thd, filter, "INTEGER", cs, str, length);
    }
  };

  class Converter_str2my_decimal_with_warn: public Converter_str2my_decimal
  {
  public:
    Converter_str2my_decimal_with_warn(THD *thd, Warn_filter filter,
                                       uint mask, CHARSET_INFO *cs,
                                       const char *str, size_t length,
                                       my_decimal *buf)
     :Converter_str2my_decimal(mask, cs, str, length, buf)
    {
      check_edom_and_truncation(thd, filter, "DECIMAL", cs, str, length);
    }
  };


  // String-to-number conversion methods for the old code compatibility
  longlong longlong_from_string_with_check(CHARSET_INFO *cs, const char *cptr,
                                           const char *end) const
  {
    /*
      TODO: Give error if we wanted a signed integer and we got an unsigned
      one

      Notice, longlong_from_string_with_check() honors thd->no_error, because
      it's used to handle queries like this:
        SELECT COUNT(@@basedir);
      and is called when Item_func_get_system_var::update_null_value()
      suppresses warnings and then calls val_int().
      The other methods {double|decimal}_from_string_with_check() ignore
      thd->no_errors, because they are not used for update_null_value()
      and they always allow all kind of warnings.
    */
    THD *thd= current_thd;
    return Converter_strtoll10_with_warn(thd, Warn_filter(thd),
                                         cs, cptr, end - cptr).result();
  }

  double double_from_string_with_check(CHARSET_INFO *cs, const char *cptr,
                                       const char *end) const
  {
    return Converter_strntod_with_warn(NULL, Warn_filter_all(),
                                       cs, cptr, end - cptr).result();
  }
  my_decimal *decimal_from_string_with_check(my_decimal *decimal_value,
                                             CHARSET_INFO *cs,
                                             const char *cptr,
                                             const char *end)
  {
    Converter_str2my_decimal_with_warn(NULL, Warn_filter_all(),
                                       E_DEC_FATAL_ERROR & ~E_DEC_BAD_NUM,
                                       cs, cptr, end - cptr, decimal_value);
    return decimal_value;
  }

  longlong longlong_from_hex_hybrid(const char *str, size_t length)
  {
    const char *end= str + length;
    const char *ptr= end - MY_MIN(length, sizeof(longlong));
    ulonglong value= 0;
    for ( ; ptr != end ; ptr++)
      value= (value << 8) + (ulonglong) (uchar) *ptr;
    return (longlong) value;
  }

  longlong longlong_from_string_with_check(const String *str) const
  {
    return longlong_from_string_with_check(str->charset(),
                                           str->ptr(), str->end());
  }
  double double_from_string_with_check(const String *str) const
  {
    return double_from_string_with_check(str->charset(),
                                         str->ptr(), str->end());
  }
  my_decimal *decimal_from_string_with_check(my_decimal *decimal_value,
                                             const String *str)
  {
    return decimal_from_string_with_check(decimal_value, str->charset(),
                                          str->ptr(), str->end());
  }
  // End of String-to-number conversion methods

public:
  /*
    The enumeration Subst_constraint is currently used only in implementations
    of the virtual function subst_argument_checker.
  */
  enum Subst_constraint
  {
    ANY_SUBST,           /* Any substitution for a field is allowed  */
    IDENTITY_SUBST       /* Substitution for a field is allowed if any two
                            different values of the field type are not equal */
  };
  /*
    Item context attributes.
    Comparison functions pass their attributes to propagate_equal_fields().
    For example, for string comparison, the collation of the comparison
    operation is important inside propagate_equal_fields().
  */
  class Context
  {
    /*
      Which type of propagation is allowed:
      - ANY_SUBST (loose equality, according to the collation), or
      - IDENTITY_SUBST (strict binary equality).
    */
    Subst_constraint m_subst_constraint;
    /*
      Comparison type.
      Important only when ANY_SUBSTS.
    */
    const Type_handler *m_compare_handler;
    /*
      Collation of the comparison operation.
      Important only when ANY_SUBST.
    */
    CHARSET_INFO *m_compare_collation;
  public:
    Context(Subst_constraint subst, const Type_handler *h, CHARSET_INFO *cs)
      :m_subst_constraint(subst),
       m_compare_handler(h),
       m_compare_collation(cs)
    { DBUG_ASSERT(h == h->type_handler_for_comparison()); }
    Subst_constraint subst_constraint() const { return m_subst_constraint; }
    const Type_handler *compare_type_handler() const
    {
      DBUG_ASSERT(m_subst_constraint == ANY_SUBST);
      return m_compare_handler;
    }
    CHARSET_INFO *compare_collation() const
    {
      DBUG_ASSERT(m_subst_constraint == ANY_SUBST);
      return m_compare_collation;
    }
  };
  class Context_identity: public Context
  { // Use this to request only exact value, no invariants.
  public:
     Context_identity()
      :Context(IDENTITY_SUBST, &type_handler_long_blob, &my_charset_bin) { }
  };
  class Context_boolean: public Context
  { // Use this when an item is [a part of] a boolean expression
  public:
    Context_boolean()
      :Context(ANY_SUBST, &type_handler_slonglong, &my_charset_bin) { }
  };
};


#define STORAGE_TYPE_MASK 7
#define COLUMN_FORMAT_MASK 7
#define COLUMN_FORMAT_SHIFT 3

/* The length of the header part for each virtual column in the .frm file */
#define FRM_VCOL_OLD_HEADER_SIZE(b) (3 + MY_TEST(b))
#define FRM_VCOL_NEW_BASE_SIZE 16
#define FRM_VCOL_NEW_HEADER_SIZE 6

class Count_distinct_field;

struct ha_field_option_struct;

struct st_cache_field;
int field_conv(Field *to,Field *from);
int truncate_double(double *nr, uint field_length, decimal_digits_t dec,
                    bool unsigned_flag, double max_value);

inline uint get_enum_pack_length(int elements)
{
  return elements < 256 ? 1 : 2;
}

inline uint get_set_pack_length(int elements)
{
  uint len= (elements + 7) / 8;
  return len > 4 ? 8 : len;
}


/**
  Tests if field type is temporal and has date part,
  i.e. represents DATE, DATETIME or TIMESTAMP types in SQL.

  @param type    Field type, as returned by field->type().
  @retval true   If field type is temporal type with date part.
  @retval false  If field type is not temporal type with date part.
*/
inline bool is_temporal_type_with_date(enum_field_types type)
{
  switch (type)
  {
  case MYSQL_TYPE_DATE:
  case MYSQL_TYPE_DATETIME:
  case MYSQL_TYPE_TIMESTAMP:
    return true;
  case MYSQL_TYPE_DATETIME2:
  case MYSQL_TYPE_TIMESTAMP2:
    DBUG_ASSERT(0); // field->real_type() should not get to here.
    return false;
  default:
    return false;
  }
}


enum enum_vcol_info_type
{
  VCOL_GENERATED_VIRTUAL, VCOL_GENERATED_STORED,
  VCOL_DEFAULT, VCOL_CHECK_FIELD, VCOL_CHECK_TABLE,
  VCOL_USING_HASH,
  /* Additional types should be added here */
  /* Following is the highest value last   */
  VCOL_TYPE_NONE = 127 // Since the 0 value is already in use
};

static inline const char *vcol_type_name(enum_vcol_info_type type)
{
  switch (type)
  {
  case VCOL_GENERATED_VIRTUAL:
  case VCOL_GENERATED_STORED:
    return "GENERATED ALWAYS AS";
  case VCOL_DEFAULT:
    return "DEFAULT";
  case VCOL_CHECK_FIELD:
  case VCOL_CHECK_TABLE:
    return "CHECK";
  case VCOL_USING_HASH:
    return "USING HASH";
  case VCOL_TYPE_NONE:
    return "UNTYPED";
  }
  return 0;
}

/*
  Flags for Virtual_column_info. If none is set, the expression must be
  a constant with no side-effects, so it's calculated at CREATE TABLE time,
  stored in table->record[2], and not recalculated for every statement.
*/
#define VCOL_FIELD_REF         1
#define VCOL_NON_DETERMINISTIC 2
#define VCOL_SESSION_FUNC      4  /* uses session data, e.g. USER or DAYNAME */
#define VCOL_TIME_FUNC         8
#define VCOL_AUTO_INC         16
#define VCOL_IMPOSSIBLE       32
#define VCOL_NOT_VIRTUAL      64  /* Function can't be virtual */
#define VCOL_CHECK_CONSTRAINT_IF_NOT_EXISTS 128

#define VCOL_NOT_STRICTLY_DETERMINISTIC                       \
  (VCOL_NON_DETERMINISTIC | VCOL_TIME_FUNC | VCOL_SESSION_FUNC)

/*
  Virtual_column_info is the class to contain additional
  characteristics that is specific for a virtual/computed
  field such as:
   - the defining expression that is evaluated to compute the value
  of the field 
  - whether the field is to be stored in the database
  - whether the field is used in a partitioning expression
*/

class Virtual_column_info: public Sql_alloc,
                           private Type_handler_hybrid_field_type
{
private:
  enum_vcol_info_type vcol_type; /* Virtual column expression type */
  /*
    The following data is only updated by the parser and read
    when a Create_field object is created/initialized.
  */
  /* Flag indicating that the field used in a partitioning expression */
  bool in_partitioning_expr;

public:
  /* Flag indicating  that the field is physically stored in the database */
  bool stored_in_db;
  bool utf8;                                    /* Already in utf8 */
  bool automatic_name;
  Item *expr;
  Lex_ident name;                               /* Name of constraint */
  /* see VCOL_* (VCOL_FIELD_REF, ...) */
  uint flags;

  Virtual_column_info()
   :Type_handler_hybrid_field_type(&type_handler_null),
    vcol_type((enum_vcol_info_type)VCOL_TYPE_NONE),
    in_partitioning_expr(FALSE), stored_in_db(FALSE),
    utf8(TRUE), automatic_name(FALSE), expr(NULL), flags(0)
  {
    name.str= NULL;
    name.length= 0;
  };
  Virtual_column_info* clone(THD *thd);
  ~Virtual_column_info() {};
  enum_vcol_info_type get_vcol_type() const
  {
    return vcol_type;
  }
  void set_vcol_type(enum_vcol_info_type v_type)
  {
    vcol_type= v_type;
  }
  const char *get_vcol_type_name() const
  {
    DBUG_ASSERT(vcol_type != VCOL_TYPE_NONE);
    return vcol_type_name(vcol_type);
  }
  void set_handler(const Type_handler *handler)
  {
    /* Calling this function can only be done once. */
    DBUG_ASSERT(type_handler() == &type_handler_null);
    Type_handler_hybrid_field_type::set_handler(handler);
  }
  bool is_stored() const
  {
    return stored_in_db;
  }
  void set_stored_in_db_flag(bool stored)
  {
    stored_in_db= stored;
  }
  bool is_in_partitioning_expr() const
  {
    return in_partitioning_expr;
  }
  void mark_as_in_partitioning_expr()
  {
    in_partitioning_expr= TRUE;
  }
  bool need_refix() const
  {
    return flags & VCOL_SESSION_FUNC;
  }
  bool fix_expr(THD *thd);
  bool fix_session_expr(THD *thd);
  bool cleanup_session_expr();
  bool fix_and_check_expr(THD *thd, TABLE *table);
  inline bool is_equal(const Virtual_column_info* vcol) const;
  inline void print(String*);
};

class Binlog_type_info
{
public:
   enum binlog_sign_t
   {
     SIGN_SIGNED,
     SIGN_UNSIGNED,
     SIGN_NOT_APPLICABLE // for non-numeric types
   };
  /**
     Retrieve the field metadata for fields.
  */
   CHARSET_INFO *m_cs; // NULL if not relevant
   TYPELIB *m_enum_typelib; // NULL if not relevant
   TYPELIB *m_set_typelib; // NULL if not relevant
   binlog_sign_t m_signedness;
   uint16 m_metadata;
   uint8 m_metadata_size;
   uchar m_type_code;     // according to Field::binlog_type()
   uchar m_geom_type; // Non-geometry fields can return 0

   Binlog_type_info(uchar type_code,
                    uint16 metadata,
                    uint8 metadata_size)
    :m_cs(NULL),
     m_enum_typelib(NULL),
     m_set_typelib(NULL),
     m_signedness(SIGN_NOT_APPLICABLE),
     m_metadata(metadata),
     m_metadata_size(metadata_size),
     m_type_code(type_code),
     m_geom_type(0)
    {};
   Binlog_type_info(uchar type_code, uint16 metadata,
                   uint8 metadata_size,
                   binlog_sign_t signedness)
    : m_cs(NULL),
     m_enum_typelib(NULL),
     m_set_typelib(NULL),
     m_signedness(signedness),
     m_metadata(metadata),
     m_metadata_size(metadata_size),
     m_type_code(type_code),
     m_geom_type(0)
    {};
   Binlog_type_info(uchar type_code, uint16 metadata,
                   uint8 metadata_size, CHARSET_INFO *cs)
    :m_cs(cs),
     m_enum_typelib(NULL),
     m_set_typelib(NULL),
     m_signedness(SIGN_NOT_APPLICABLE),
     m_metadata(metadata),
     m_metadata_size(metadata_size),
     m_type_code(type_code),
     m_geom_type(0)
    {};
   Binlog_type_info(uchar type_code, uint16 metadata,
                   uint8 metadata_size,
                   CHARSET_INFO *cs,
                   TYPELIB *t_enum, TYPELIB *t_set)
    :m_cs(cs),
     m_enum_typelib(t_enum),
     m_set_typelib(t_set),
     m_signedness(SIGN_NOT_APPLICABLE),
     m_metadata(metadata),
     m_metadata_size(metadata_size),
     m_type_code(type_code),
     m_geom_type(0)
    {};
   Binlog_type_info(uchar type_code, uint16 metadata,
                   uint8 metadata_size, CHARSET_INFO *cs,
                   uchar geom_type)
    :m_cs(cs),
     m_enum_typelib(NULL),
     m_set_typelib(NULL),
     m_signedness(SIGN_NOT_APPLICABLE),
     m_metadata(metadata),
     m_metadata_size(metadata_size),
     m_type_code(type_code),
     m_geom_type(geom_type)
    {};
  static void *operator new(size_t size, MEM_ROOT *mem_root) throw ()
  { return alloc_root(mem_root, size); }
};


class Binlog_type_info_fixed_string: public Binlog_type_info
{
public:
  Binlog_type_info_fixed_string(uchar type_code,
                                uint32 octet_length,
                                CHARSET_INFO *cs);
};


class Field: public Value_source
{
  Field(const Item &);				/* Prevent use of these */
  void operator=(Field &);
protected:
  int save_in_field_str(Field *to)
  {
    StringBuffer<MAX_FIELD_WIDTH> result(charset());
    val_str(&result);
    return to->store(result.ptr(), result.length(), charset());
  }
  void error_generated_column_function_is_not_allowed(THD *thd, bool error)
                                                      const;
  static void do_field_eq(Copy_field *copy);
  static void do_field_int(Copy_field *copy);
  static void do_field_real(Copy_field *copy);
  static void do_field_string(Copy_field *copy);
  static void do_field_date(Copy_field *copy);
  static void do_field_temporal(Copy_field *copy, date_mode_t fuzzydate);
  static void do_field_datetime(Copy_field *copy);
  static void do_field_timestamp(Copy_field *copy);
  static void do_field_decimal(Copy_field *copy);
public:
  static void *operator new(size_t size, MEM_ROOT *mem_root) throw ()
  { return alloc_root(mem_root, size); }
  static void *operator new(size_t size) throw ()
  {
    DBUG_ASSERT(size < UINT_MAX32);
    return thd_alloc(current_thd, (uint) size);
  }
  static void operator delete(void *ptr_arg, size_t size) { TRASH_FREE(ptr_arg, size); }
  static void operator delete(void *ptr, MEM_ROOT *mem_root)
  { DBUG_ASSERT(0); }

  bool marked_for_read() const;
  bool marked_for_write_or_computed() const;

  /**
     Used by System Versioning.
   */
  virtual void set_max()
  { DBUG_ASSERT(0); }
  virtual bool is_max()
  { DBUG_ASSERT(0); return false; }

  uchar		*ptr;			// Position to field in record

  /**
     Byte where the @c NULL bit is stored inside a record. If this Field is a
     @c NOT @c NULL field, this member is @c NULL.
  */
  uchar		*null_ptr;
  /*
    Note that you can use table->in_use as replacement for current_thd member
    only inside of val_*() and store() members (e.g. you can't use it in cons)
  */
  TABLE *table;                                 // Pointer for table
  TABLE *orig_table;                            // Pointer to original table
  const char * const *table_name;               // Pointer to alias in TABLE
  LEX_CSTRING field_name;
  LEX_CSTRING comment;
  /** reference to the list of options or NULL */
  engine_option_value *option_list;
  ha_field_option_struct *option_struct;   /* structure with parsed options */
  /* Field is part of the following keys */
  key_map	key_start, part_of_key, part_of_key_not_clustered;

  /*
    Bitmap of indexes that have records ordered by col1, ... this_field, ...

    For example, INDEX (col(prefix_n)) is not present in col.part_of_sortkey.
  */
  key_map       part_of_sortkey;
  /*
    We use three additional unireg types for TIMESTAMP to overcome limitation
    of current binary format of .frm file. We'd like to be able to support
    NOW() as default and on update value for such fields but unable to hold
    this info anywhere except unireg_check field. This issue will be resolved
    in more clean way with transition to new text based .frm format.
    See also comment for Field_timestamp::Field_timestamp().
  */
  enum __attribute__((packed)) utype  {
    NONE=0,
    NEXT_NUMBER=15,             // AUTO_INCREMENT
    TIMESTAMP_OLD_FIELD=18,     // TIMESTAMP created before 4.1.3
    TIMESTAMP_DN_FIELD=21,      // TIMESTAMP DEFAULT NOW()
    TIMESTAMP_UN_FIELD=22,      // TIMESTAMP ON UPDATE NOW()
    TIMESTAMP_DNUN_FIELD=23,    // TIMESTAMP DEFAULT NOW() ON UPDATE NOW()
    TMYSQL_COMPRESSED= 24,      // Compatibility with TMySQL
    };
  enum imagetype { itRAW, itMBR};

  utype	unireg_check;
  field_visibility_t invisible;
  uint32        field_length;          // Length of field
  uint32        flags;
  field_index_t field_index;           // field number in fields array
  uchar null_bit;                      // Bit used to test null bit

  /**
     If true, this field was created in create_tmp_field_from_item from a NULL
     value. This means that the type of the field is just a guess, and the type
     may be freely coerced to another type.

     @see create_tmp_field_from_item
     @see Item_type_holder::get_real_type

   */
  bool is_created_from_null_item;

  /* 
    Selectivity of the range condition over this field.
    When calculating this selectivity a range predicate
    is taken into account only if:
    - it is extracted from the WHERE clause
    - it depends only on the table the field belongs to 
  */
  double cond_selectivity;

  /* 
    The next field in the class of equal fields at the top AND level
    of the WHERE clause
  */ 
  Field *next_equal_field;

  /*
    This structure is used for statistical data on the column
    that has been read from the statistical table column_stat
  */ 
  Column_statistics *read_stats;
  /*
    This structure is used for statistical data on the column that
    is collected by the function collect_statistics_for_table
  */
  Column_statistics_collected *collected_stats;

  /* 
    This is additional data provided for any computed(virtual) field,
    default function or check constraint.
    In particular it includes a pointer to the item by which this field
    can be computed from other fields.
  */
  Virtual_column_info *vcol_info, *check_constraint, *default_value;

  Field(uchar *ptr_arg,uint32 length_arg,uchar *null_ptr_arg,
        uchar null_bit_arg, utype unireg_check_arg,
        const LEX_CSTRING *field_name_arg);
  virtual ~Field() {}

  virtual Type_numeric_attributes type_numeric_attributes() const
  {
    return Type_numeric_attributes(field_length, decimals(), is_unsigned());
  }
  Type_std_attributes type_std_attributes() const
  {
    return Type_std_attributes(type_numeric_attributes(), dtcollation());
  }

  bool is_unsigned() const { return flags & UNSIGNED_FLAG; }

  /**
    Convenience definition of a copy function returned by
    Field::get_copy_func()
  */
  typedef void Copy_func(Copy_field*);
  virtual Copy_func *get_copy_func(const Field *from) const= 0;
  virtual Copy_func *get_copy_func_to(const Field *to) const
  {
    return to->get_copy_func(this);
  }
  /* Store functions returns 1 on overflow and -1 on fatal error */
  virtual int  store_field(Field *from) { return from->save_in_field(this); }
  virtual int  save_in_field(Field *to)= 0;
  /**
    Check if it is possible just copy the value
    of the field 'from' to the field 'this', e.g. for
      INSERT INTO t1 (field1) SELECT field2 FROM t2;
    @param from   - The field to copy from
    @retval true  - it is possible to just copy value of 'from' to 'this'
    @retval false - conversion is needed
  */
  virtual bool memcpy_field_possible(const Field *from) const= 0;
  virtual bool make_empty_rec_store_default_value(THD *thd, Item *item);
  virtual void make_empty_rec_reset(THD *thd)
  {
    reset();
  }
  virtual int  store(const char *to, size_t length,CHARSET_INFO *cs)=0;
  /*
    This is used by engines like CSV and Federated to signal the field
    that the data is going to be in text (rather than binary) representation,
    even if cs points to &my_charset_bin.

    If a Field distinguishes between text and binary formats (e.g. INET6),
    we cannot call store(str,length,&my_charset_bin),
    to avoid "field" mis-interpreting the data format as binary.
  */
  virtual int  store_text(const char *to, size_t length, CHARSET_INFO *cs)
  {
    return store(to, length, cs);
  }
  virtual int  store_binary(const char *to, size_t length)
  {
    return store(to, length, &my_charset_bin);
  }
  virtual int  store_hex_hybrid(const char *str, size_t length);
  virtual int  store(double nr)=0;
  virtual int  store(longlong nr, bool unsigned_val)=0;
  virtual int  store_decimal(const my_decimal *d)=0;
  virtual int  store_time_dec(const MYSQL_TIME *ltime, uint dec);
  virtual int  store_timestamp_dec(const timeval &ts, uint dec);
  int store_timestamp(my_time_t timestamp, ulong sec_part)
  {
    return store_timestamp_dec(Timeval(timestamp, sec_part),
                               TIME_SECOND_PART_DIGITS);
  }
  /**
    Store a value represented in native format
  */
  virtual int store_native(const Native &value)
  {
    DBUG_ASSERT(0);
    reset();
    return 0;
  }
  int store_time(const MYSQL_TIME *ltime)
  { return store_time_dec(ltime, TIME_SECOND_PART_DIGITS); }
  int store(const char *to, size_t length, CHARSET_INFO *cs,
            enum_check_fields check_level);
  int store_text(const char *to, size_t length, CHARSET_INFO *cs,
                 enum_check_fields check_level);
  int store(const LEX_STRING *ls, CHARSET_INFO *cs)
  {
    DBUG_ASSERT(ls->length < UINT_MAX32);
    return store(ls->str, (uint) ls->length, cs);
  }
  int store(const LEX_CSTRING *ls, CHARSET_INFO *cs)
  {
    DBUG_ASSERT(ls->length < UINT_MAX32);
    return store(ls->str, (uint) ls->length, cs);
  }
  int store(const LEX_CSTRING &ls, CHARSET_INFO *cs)
  {
    DBUG_ASSERT(ls.length < UINT_MAX32);
    return store(ls.str, (uint) ls.length, cs);
  }

  /*
    @brief
      Store minimum/maximum value of a column in the statistics table.
    @param
      field                  statistical table field
      str                    value buffer
  */
  virtual int store_to_statistical_minmax_field(Field *field, String *str);

  /*
    @brief
      Store minimum/maximum value of a column from the statistical table.
    @param
      field                  statistical table field
      str                    value buffer
  */
  virtual int store_from_statistical_minmax_field(Field *field, String *str);

#ifdef HAVE_MEM_CHECK
  /**
    Mark unused memory in the field as defined. Mainly used to ensure
    that if we write full field to disk (for example in
    Count_distinct_field::add(), we don't write unitalized data to
    disk which would confuse valgrind or MSAN.
  */
  virtual void mark_unused_memory_as_defined() {}
#else
  void mark_unused_memory_as_defined() {}
#endif

  virtual double val_real()=0;
  virtual longlong val_int()=0;
  /*
    Get ulonglong representation.
    Negative values are truncated to 0.
  */
  virtual ulonglong val_uint(void)
  {
    longlong nr= val_int();
    return nr < 0 ? 0 : (ulonglong) nr;
  }
  virtual bool val_bool()= 0;
  virtual my_decimal *val_decimal(my_decimal *)=0;
  inline String *val_str(String *str) { return val_str(str, str); }
  /*
     val_str(buf1, buf2) gets two buffers and should use them as follows:
     if it needs a temp buffer to convert result to string - use buf1
       example Field_tiny::val_str()
     if the value exists as a string already - use buf2
       example Field_string::val_str()
     consequently, buf2 may be created as 'String buf;' - no memory
     will be allocated for it. buf1 will be allocated to hold a
     value if it's too small. Using allocated buffer for buf2 may result in
     an unnecessary free (and later, may be an alloc).
     This trickery is used to decrease a number of malloc calls.
  */
  virtual String *val_str(String*,String *)=0;
  virtual bool val_native(Native *to)
  {
    DBUG_ASSERT(!is_null());
    return to->copy((const char *) ptr, pack_length());
  }
  String *val_int_as_str(String *val_buffer, bool unsigned_flag);
  /*
    Return the field value as a LEX_CSTRING, without padding to full length
    (MODE_PAD_CHAR_TO_FULL_LENGTH is temporarily suppressed during the call).

    In case of an empty value, to[0] is assigned to empty_clex_string,
    memory is not allocated.
    In case of a non-empty value, the memory is allocated on mem_root.
    In case of a memory allocation failure, to[0] is assigned to {NULL,0}.

    @param  [IN] mem_root  store non-empty values here
    @param  [OUT to        return the string here
    @retval                false (success)
    @retval                true  (EOM)
  */
  bool val_str_nopad(MEM_ROOT *mem_root, LEX_CSTRING *to);
  fast_field_copier get_fast_field_copier(const Field *from);
  /*
   str_needs_quotes() returns TRUE if the value returned by val_str() needs
   to be quoted when used in constructing an SQL query.
  */
  virtual bool str_needs_quotes() const { return false; }
  const Type_handler *type_handler_for_comparison() const
  {
    return type_handler()->type_handler_for_comparison();
  }
  Item_result result_type () const
  {
    return type_handler()->result_type();
  }
  Item_result cmp_type () const
  {
    return type_handler()->cmp_type();
  }
  virtual bool eq(Field *field)
  {
    return (ptr == field->ptr && null_ptr == field->null_ptr &&
            null_bit == field->null_bit && field->type() == type());
  }
  virtual bool eq_def(const Field *field) const;
  
  /*
    pack_length() returns size (in bytes) used to store field data in memory
    (i.e. it returns the maximum size of the field in a row of the table,
    which is located in RAM).
  */
  virtual uint32 pack_length() const { return (uint32) field_length; }

  /*
    pack_length_in_rec() returns size (in bytes) used to store field data on
    storage (i.e. it returns the maximal size of the field in a row of the
    table, which is located on disk).
  */
  virtual uint32 pack_length_in_rec() const { return pack_length(); }
  virtual bool compatible_field_size(uint metadata, const Relay_log_info *rli,
                                     uint16 mflags, int *order) const;
  virtual uint pack_length_from_metadata(uint field_metadata) const
  {
    DBUG_ENTER("Field::pack_length_from_metadata");
    DBUG_RETURN(field_metadata);
  }
  virtual uint row_pack_length() const { return 0; }

  /*
    data_length() return the "real size" of the data in memory.
  */
  virtual uint32 data_length() { return pack_length(); }
  virtual uint32 sort_length() const { return pack_length(); }

  /*
    sort_suffix_length() return the length bytes needed to store the length
    for binary charset
  */
  virtual uint32 sort_suffix_length() const { return 0; }

  /* 
    Get the number bytes occupied by the value in the field.
    CHAR values are stripped of trailing spaces.
    Flexible values are stripped of their length.
  */
  virtual uint32 value_length()
  {
    uint len;
    if (!zero_pack() &&
	(type() == MYSQL_TYPE_STRING &&
        (len= pack_length()) >= 4 && len < 256))
    {
      uchar *str, *end;
      for (str= ptr, end= str+len; end > str && end[-1] == ' '; end--) {}
      len=(uint) (end-str); 
      return len;
    } 
    return data_length();
  }

  /**
     Get the maximum size of the data in packed format.

     @return Maximum data length of the field when packed using the
     Field::pack() function.
   */
  virtual uint32 max_data_length() const {
    return pack_length();
  };

  virtual int reset() { bzero(ptr,pack_length()); return 0; }
  virtual void reset_fields() {}
  const uchar *ptr_in_record(const uchar *record) const
  {
    my_ptrdiff_t l_offset= (my_ptrdiff_t) (ptr -  table->record[0]);
    DBUG_ASSERT(l_offset >= 0 && table->s->rec_buff_length - l_offset > 0);
    return record + l_offset;
  }
  virtual int set_default();

  bool has_update_default_function() const
  {
    return flags & ON_UPDATE_NOW_FLAG;
  }
  bool has_default_now_unireg_check() const
  {
    return unireg_check == TIMESTAMP_DN_FIELD
        || unireg_check == TIMESTAMP_DNUN_FIELD;
  }

  /*
    Mark the field as having a value supplied by the client, thus it should
    not be auto-updated.
  */
  void set_has_explicit_value()
  {
    bitmap_set_bit(&table->has_value_set, field_index);
  }
  bool has_explicit_value()
  {
    return bitmap_is_set(&table->has_value_set, field_index);
  }
  void clear_has_explicit_value()
  {
    bitmap_clear_bit(&table->has_value_set, field_index);
  }

  virtual my_time_t get_timestamp(const uchar *pos, ulong *sec_part) const
  { DBUG_ASSERT(0); return 0; }
  my_time_t get_timestamp(ulong *sec_part) const
  {
    return get_timestamp(ptr, sec_part);
  }

  virtual bool binary() const { return 1; }
  virtual bool zero_pack() const { return 1; }
  virtual enum ha_base_keytype key_type() const { return HA_KEYTYPE_BINARY; }
  virtual uint16 key_part_flag() const { return 0; }
  virtual uint16 key_part_length_bytes() const { return 0; }
  virtual uint32 key_length() const { return pack_length(); }
  virtual const Type_handler *type_handler() const = 0;
  virtual enum_field_types type() const
  {
    return type_handler()->field_type();
  }
  virtual enum_field_types real_type() const
  {
    return type_handler()->real_field_type();
  }
  virtual enum_field_types binlog_type() const
  {
    /*
      Binlog stores field->type() as type code by default. For example,
      it puts MYSQL_TYPE_STRING in case of CHAR, VARCHAR, SET and ENUM,
      with extra data type details put into metadata.

      Binlog behaviour slightly differs between various MySQL and MariaDB
      versions for the temporal data types TIME, DATETIME and TIMESTAMP.

      MySQL prior to 5.6 uses MYSQL_TYPE_TIME, MYSQL_TYPE_DATETIME 
      and MYSQL_TYPE_TIMESTAMP type codes in binlog and stores no 
      additional metadata.

      MariaDB-5.3 implements new versions for TIME, DATATIME, TIMESTAMP
      with fractional second precision, but uses the old format for the
      types TIME(0), DATETIME(0), TIMESTAMP(0), and it still stores
      MYSQL_TYPE_TIME, MYSQL_TYPE_DATETIME and MYSQL_TYPE_TIMESTAMP in binlog,
      with no additional metadata.
      So row-based replication between temporal data types of
      different precision is not possible in MariaDB.

      MySQL-5.6 also implements a new version of TIME, DATETIME, TIMESTAMP
      which support fractional second precision 0..6, and use the new
      format even for the types TIME(0), DATETIME(0), TIMESTAMP(0).
      For these new data types, MySQL-5.6 stores new type codes 
      MYSQL_TYPE_TIME2, MYSQL_TYPE_DATETIME2, MYSQL_TYPE_TIMESTAMP2 in binlog,
      with fractional precision 0..6 put into metadata.
      This makes it in theory possible to do row-based replication between
      columns of different fractional precision (e.g. from TIME(1) on master
      to TIME(6) on slave). However, it's not currently fully implemented yet.
      MySQL-5.6 can only do row-based replication from the old types
      TIME, DATETIME, TIMESTAMP (represented by MYSQL_TYPE_TIME,
      MYSQL_TYPE_DATETIME and MYSQL_TYPE_TIMESTAMP type codes in binlog)
      to the new corresponding types TIME(0), DATETIME(0), TIMESTAMP(0).

      Note: MariaDB starting from the version 10.0 understands the new
      MySQL-5.6 type codes MYSQL_TYPE_TIME2, MYSQL_TYPE_DATETIME2,
      MYSQL_TYPE_TIMESTAMP2. When started over MySQL-5.6 tables both on
      master and on slave, MariaDB-10.0 can also do row-based replication
      from the old types TIME, DATETIME, TIMESTAMP to the new MySQL-5.6
      types TIME(0), DATETIME(0), TIMESTAMP(0).

      Note: perhaps binlog should eventually be modified to store
      real_type() instead of type() for all column types.
    */
    return type();
  }
  virtual Binlog_type_info binlog_type_info() const
  {
    DBUG_ASSERT(Field::type() == binlog_type());
    return Binlog_type_info(Field::type(), 0, 0);
  }
  virtual en_fieldtype tmp_engine_column_type(bool use_packed_rows) const
  {
    return FIELD_NORMAL;
  }
  /*
    Conversion type for from the source to the current field.
  */
  virtual enum_conv_type rpl_conv_type_from(const Conv_source &source,
                                            const Relay_log_info *rli,
                                            const Conv_param &param)
                                            const= 0;
  enum_conv_type rpl_conv_type_from_same_data_type(uint16 metadata,
                                                   const Relay_log_info *rli,
                                                   const Conv_param &param)
                                                   const;
  inline  int cmp(const uchar *str) const { return cmp(ptr,str); }
  /*
    The following method is used for comparing prefix keys.
    Currently it's only used in partitioning.
  */
  virtual int cmp_prefix(const uchar *a, const uchar *b,
                         size_t prefix_len) const
  { return cmp(a, b); }
  virtual int cmp(const uchar *,const uchar *) const=0;
  virtual int cmp_binary(const uchar *a,const uchar *b, uint32 max_length=~0U) const
  { return memcmp(a,b,pack_length()); }
  virtual int cmp_offset(my_ptrdiff_t row_offset)
  { return cmp(ptr,ptr+row_offset); }
  virtual int cmp_binary_offset(uint row_offset)
  { return cmp_binary(ptr, ptr+row_offset); };
  virtual int key_cmp(const uchar *a,const uchar *b) const
  { return cmp(a, b); }
  virtual int key_cmp(const uchar *str, uint length) const
  { return cmp(ptr,str); }
  /*
    Update the value m of the 'min_val' field with the current value v
    of this field if force_update is set to TRUE or if v < m.
    Return TRUE if the value has been updated.
  */  
  virtual bool update_min(Field *min_val, bool force_update)
  { 
    bool update_fl= force_update || cmp(ptr, min_val->ptr) < 0;
    if (update_fl)
    {
      min_val->set_notnull();
      memcpy(min_val->ptr, ptr, pack_length());
    }
    return update_fl;
  }
  /*
    Update the value m of the 'max_val' field with the current value v
    of this field if force_update is set to TRUE or if v > m.
    Return TRUE if the value has been updated.
  */  
  virtual bool update_max(Field *max_val, bool force_update)
  { 
    bool update_fl= force_update || cmp(ptr, max_val->ptr) > 0;
    if (update_fl)
    {
      max_val->set_notnull();
      memcpy(max_val->ptr, ptr, pack_length());
    }
    return update_fl;
  }
  virtual void store_field_value(uchar *val, uint len)
  {
     memcpy(ptr, val, len);
  }
  virtual decimal_digits_t decimals() const { return 0; }
  virtual Information_schema_numeric_attributes
            information_schema_numeric_attributes() const
  {
    return Information_schema_numeric_attributes();
  }
  virtual Information_schema_character_attributes
            information_schema_character_attributes() const
  {
    return Information_schema_character_attributes();
  }
  virtual void update_data_type_statistics(Data_type_statistics *st) const
  { }
  /*
    Caller beware: sql_type can change str.Ptr, so check
    ptr() to see if it changed if you are using your own buffer
    in str and restore it with set() if needed
  */
  virtual void sql_type(String &str) const =0;
  virtual void sql_rpl_type(String *str) const { sql_type(*str); }
  virtual uint size_of() const =0;		// For new field
  inline bool is_null(my_ptrdiff_t row_offset= 0) const
  {
    /*
      The table may have been marked as containing only NULL values
      for all fields if it is a NULL-complemented row of an OUTER JOIN
      or if the query is an implicitly grouped query (has aggregate
      functions but no GROUP BY clause) with no qualifying rows. If
      this is the case (in which TABLE::null_row is true), the field
      is considered to be NULL.

      Note that if a table->null_row is set then also all null_bits are
      set for the row.

      In the case of the 'result_field' for GROUP BY, table->null_row might
      refer to the *next* row in the table (when the algorithm is: read the
      next row, see if any of group column values have changed, send the
      result - grouped - row to the client if yes). So, table->null_row might
      be wrong, but such a result_field is always nullable (that's defined by
      original_field->maybe_null()) and we trust its null bit.
    */
    return null_ptr ? null_ptr[row_offset] & null_bit : table->null_row;
  }
  inline bool is_real_null(my_ptrdiff_t row_offset= 0) const
    { return null_ptr && (null_ptr[row_offset] & null_bit); }
  inline bool is_null_in_record(const uchar *record) const
  {
    if (maybe_null_in_table())
      return record[(uint) (null_ptr - table->record[0])] & null_bit;
    return 0;
  }
  inline void set_null(my_ptrdiff_t row_offset= 0)
    { if (null_ptr) null_ptr[row_offset]|= null_bit; }
  inline void set_notnull(my_ptrdiff_t row_offset= 0)
    { if (null_ptr) null_ptr[row_offset]&= (uchar) ~null_bit; }
  inline bool maybe_null(void) const
  { return null_ptr != 0 || table->maybe_null; }
  // Set to NULL on LOAD DATA or LOAD XML
  virtual bool load_data_set_null(THD *thd);
  // Reset when a LOAD DATA file ended unexpectedly
  virtual bool load_data_set_no_data(THD *thd, bool fixed_format);
  void load_data_set_value(const char *pos, uint length, CHARSET_INFO *cs);

  /* @return true if this field is NULL-able (even if temporarily) */
  inline bool real_maybe_null() const { return null_ptr != 0; }
  uint null_offset(const uchar *record) const
  { return (uint) (null_ptr - record); }
  /*
    For a NULL-able field (that can actually store a NULL value in a table)
    null_ptr points to the "null bitmap" in the table->record[0] header. For
    NOT NULL fields it is either 0 or points outside table->record[0] into the
    table->triggers->extra_null_bitmap (so that the field can store a NULL
    value temporarily, only in memory)
  */
  bool maybe_null_in_table() const
  { return null_ptr >= table->record[0] && null_ptr <= ptr; }

  uint null_offset() const
  { return null_offset(table->record[0]); }
  void set_null_ptr(uchar *p_null_ptr, uint p_null_bit)
  {
    null_ptr= p_null_ptr;
    null_bit= static_cast<uchar>(p_null_bit);
  }

  bool stored_in_db() const { return !vcol_info || vcol_info->stored_in_db; }
  bool check_vcol_sql_mode_dependency(THD *, vcol_init_mode mode) const;

  virtual sql_mode_t value_depends_on_sql_mode() const
  {
    return 0;
  }
  virtual sql_mode_t conversion_depends_on_sql_mode(THD *thd,
                                                    Item *expr) const
  {
    return (sql_mode_t) 0;
  }
  virtual sql_mode_t can_handle_sql_mode_dependency_on_store() const
  {
    return 0;
  }

  inline THD *get_thd() const
  { return likely(table) ? table->in_use : current_thd; }

  enum {
    LAST_NULL_BYTE_UNDEF= 0
  };

  /*
    Find the position of the last null byte for the field.

    SYNOPSIS
      last_null_byte()

    DESCRIPTION
      Return a pointer to the last byte of the null bytes where the
      field conceptually is placed.

    RETURN VALUE
      The position of the last null byte relative to the beginning of
      the record. If the field does not use any bits of the null
      bytes, the value 0 (LAST_NULL_BYTE_UNDEF) is returned.
   */
  size_t last_null_byte() const {
    size_t bytes= do_last_null_byte();
    DBUG_PRINT("debug", ("last_null_byte() ==> %ld", (long) bytes));
    DBUG_ASSERT(bytes <= table->s->null_bytes);
    return bytes;
  }

  /*
    Create mem-comparable sort key part for a sort key
  */
  void make_sort_key_part(uchar *buff, uint length);

  /*
    create a compact sort key which can be compared with a comparison
    function. They are called packed sort keys
  */
  virtual uint make_packed_sort_key_part(uchar *buff,
                                         const SORT_FIELD_ATTR *sort_field);

  virtual void make_send_field(Send_field *);

  /*
    Some implementations actually may write up to 8 bytes regardless of what
    size was requested. This is due to the minimum value of the system variable
    max_sort_length.
  */

  virtual void sort_string(uchar *buff,uint length)=0;
  virtual bool optimize_range(uint idx, uint part) const;
  virtual void free() {}
  virtual Field *make_new_field(MEM_ROOT *root, TABLE *new_table,
                                bool keep_type);
  virtual Field *new_key_field(MEM_ROOT *root, TABLE *new_table,
                               uchar *new_ptr, uint32 length,
                               uchar *new_null_ptr, uint new_null_bit);
  Field *create_tmp_field(MEM_ROOT *root, TABLE *new_table,
                          bool maybe_null_arg);
  Field *create_tmp_field(MEM_ROOT *root, TABLE *new_table)
  {
    return create_tmp_field(root, new_table, maybe_null());
  }
  Field *clone(MEM_ROOT *mem_root, TABLE *new_table);
  Field *clone(MEM_ROOT *mem_root, TABLE *new_table, my_ptrdiff_t diff);
  inline void move_field(uchar *ptr_arg,uchar *null_ptr_arg,uchar null_bit_arg)
  {
    ptr=ptr_arg; null_ptr=null_ptr_arg; null_bit=null_bit_arg;
  }
  inline void move_field(uchar *ptr_arg) { ptr=ptr_arg; }
  inline uchar *record_ptr() // record[0] or wherever the field was moved to
  {
    my_ptrdiff_t offset= table->s->field[field_index]->ptr - table->s->default_values;
    return ptr - offset;
  }
  virtual void move_field_offset(my_ptrdiff_t ptr_diff)
  {
    ptr=ADD_TO_PTR(ptr,ptr_diff, uchar*);
    if (null_ptr)
      null_ptr=ADD_TO_PTR(null_ptr,ptr_diff,uchar*);
  }

  /*
    Copy the Field's value to buff. The value will be in table->record[]
    format.
  */
  void get_image(uchar *buff, uint length, CHARSET_INFO *cs) const
  { get_image(buff, length, ptr, cs); }
  virtual void get_image(uchar *buff, uint length,
                         const uchar *ptr_arg, CHARSET_INFO *cs) const
    { memcpy(buff,ptr_arg,length); }

  /*
    Set Field's value to the value in *buf.
  */
  virtual void set_image(const uchar *buff,uint length, CHARSET_INFO *cs)
    { memcpy(ptr,buff,length); }


  /*
    Copy a field part into an output buffer.

    SYNOPSIS
      Field::get_key_image()
      buff   [out] output buffer
      length       output buffer size
      type         itMBR for geometry blobs, otherwise itRAW

    DESCRIPTION
      This function makes a copy of field part of size equal to or
      less than "length" parameter value.
      For fields of string types (CHAR, VARCHAR, TEXT) the rest of buffer
      is padded by zero byte.

    NOTES
      For variable length character fields (i.e. UTF-8) the "length"
      parameter means a number of output buffer bytes as if all field
      characters have maximal possible size (mbmaxlen). In the other words,
      "length" parameter is a number of characters multiplied by
      field_charset->mbmaxlen.

    RETURN
      Number of copied bytes (excluding padded zero bytes -- see above).
  */

  uint get_key_image(uchar *buff, uint length, imagetype type_arg) const
  { return get_key_image(buff, length, ptr, type_arg); }
  virtual uint get_key_image(uchar *buff, uint length, const uchar *ptr_arg, imagetype type_arg) const
  {
    get_image(buff, length, ptr_arg, &my_charset_bin);
    return length;
  }
  virtual void set_key_image(const uchar *buff,uint length)
    { set_image(buff,length, &my_charset_bin); }
  inline longlong val_int_offset(uint row_offset)
    {
      ptr+=row_offset;
      longlong tmp=val_int();
      ptr-=row_offset;
      return tmp;
    }
  inline longlong val_int(const uchar *new_ptr)
  {
    uchar *old_ptr= ptr;
    longlong return_value;
    ptr= (uchar*) new_ptr;
    return_value= val_int();
    ptr= old_ptr;
    return return_value;
  }
  inline String *val_str(String *str, const uchar *new_ptr)
  {
    uchar *old_ptr= ptr;
    ptr= (uchar*) new_ptr;
    val_str(str);
    ptr= old_ptr;
    return str;
  }
  virtual bool send(Protocol *protocol);

  virtual uchar *pack(uchar *to, const uchar *from, uint max_length);
  /**
     @overload Field::pack(uchar*, const uchar*, uint, bool)
  */
  uchar *pack(uchar *to, const uchar *from)
  {
    DBUG_ENTER("Field::pack");
    uchar *result= this->pack(to, from, UINT_MAX);
    DBUG_RETURN(result);
  }

  virtual const uchar *unpack(uchar* to, const uchar *from,
                              const uchar *from_end, uint param_data=0);

  virtual uint packed_col_length(const uchar *to, uint length)
  { return length;}
  virtual uint max_packed_col_length(uint max_length)
  { return max_length;}
  virtual bool is_packable() const { return false; }

  uint offset(const uchar *record) const
  {
    return (uint) (ptr - record);
  }
  void copy_from_tmp(int offset);
  uint fill_cache_field(struct st_cache_field *copy);
  virtual bool get_date(MYSQL_TIME *ltime, date_mode_t fuzzydate);
  virtual longlong val_datetime_packed(THD *thd);
  virtual longlong val_time_packed(THD *thd);
  virtual const TYPELIB *get_typelib() const { return NULL; }
  virtual CHARSET_INFO *charset() const= 0;
  /* returns TRUE if the new charset differs. */
  virtual void change_charset(const DTCollation &new_cs) {}
  virtual const DTCollation &dtcollation() const= 0;
  virtual CHARSET_INFO *charset_for_protocol(void) const
  { return binary() ? &my_charset_bin : charset(); }
  virtual CHARSET_INFO *sort_charset(void) const { return charset(); }
  virtual bool has_charset(void) const { return FALSE; }
  virtual int set_time() { return 1; }
  bool set_warning(Sql_condition::enum_warning_level, unsigned int code,
                   int cuted_increment, ulong current_row=0) const;
  virtual void print_key_value(String *out, uint32 length);
  void print_key_part_value(String *out, const uchar *key, uint32 length);
  void print_key_value_binary(String *out, const uchar* key, uint32 length);
protected:
  bool set_warning(unsigned int code, int cuted_increment) const
  {
    return set_warning(Sql_condition::WARN_LEVEL_WARN, code, cuted_increment);
  }
  bool set_note(unsigned int code, int cuted_increment) const
  {
    return set_warning(Sql_condition::WARN_LEVEL_NOTE, code, cuted_increment);
  }
  void set_datetime_warning(Sql_condition::enum_warning_level, uint code,
                            const ErrConv *str, const char *typestr,
                            int cuted_increment) const;
  void set_datetime_warning(uint code,
                            const ErrConv *str, const char *typestr,
                            int cuted_increment) const
  {
    set_datetime_warning(Sql_condition::WARN_LEVEL_WARN, code, str, typestr,
                         cuted_increment);
  }
  void set_warning_truncated_wrong_value(const char *type, const char *value);
  inline bool check_overflow(int op_result)
  {
    return (op_result == E_DEC_OVERFLOW);
  }
  int warn_if_overflow(int op_result);
  Copy_func *get_identical_copy_func() const;
  bool cmp_is_done_using_type_handler_of_this(const Item_bool_func *cond,
                                              const Item *item) const;
  bool can_optimize_scalar_range(const RANGE_OPT_PARAM *param,
                                 const KEY_PART *key_part,
                                 const Item_bool_func *cond,
                                 scalar_comparison_op op,
                                 const Item *value) const;
  uchar *make_key_image(MEM_ROOT *mem_root, const KEY_PART *key_part);
  SEL_ARG *get_mm_leaf_int(RANGE_OPT_PARAM *param, KEY_PART *key_part,
                           const Item_bool_func *cond,
                           scalar_comparison_op op, Item *value,
                           bool unsigned_field);
  /*
    Make a leaf tree for the cases when the value was stored
    to the field exactly, without any truncation, rounding or adjustments.
    For example, if we stored an INT value into an INT column,
    and value->save_in_field_no_warnings() returned 0,
    we know that the value was stored exactly.
  */
  SEL_ARG *stored_field_make_mm_leaf_exact(RANGE_OPT_PARAM *param,
                                           KEY_PART *key_part,
                                           scalar_comparison_op op,
                                           Item *value);
  /*
    Make a leaf tree for the cases when we don't know if
    the value was stored to the field without any data loss,
    or was modified to a smaller or a greater value.
    Used for the data types whose methods Field::store*()
    silently adjust the value. This is the most typical case.
  */
  SEL_ARG *stored_field_make_mm_leaf(RANGE_OPT_PARAM *param,
                                     KEY_PART *key_part,
                                     scalar_comparison_op op, Item *value);
  /*
    Make a leaf tree when an INT value was stored into a field of INT type,
    and some truncation happened. Tries to adjust the range search condition
    when possible, e.g. "tinytint < 300" -> "tinyint <= 127".
    Can also return SEL_ARG_IMPOSSIBLE(), and NULL (not sargable).
  */
  SEL_ARG *stored_field_make_mm_leaf_bounded_int(RANGE_OPT_PARAM *param,
                                                 KEY_PART *key_part,
                                                 scalar_comparison_op op,
                                                 Item *value,
                                                 bool unsigned_field);
  /*
    Make a leaf tree when some truncation happened during
    value->save_in_field_no_warning(this), and we cannot yet adjust the range
    search condition for the current combination of the field and the value
    data types.
    Returns SEL_ARG_IMPOSSIBLE() for "=" and "<=>".
    Returns NULL (not sargable) for other comparison operations.
  */
  SEL_ARG *stored_field_make_mm_leaf_truncated(RANGE_OPT_PARAM *prm,
                                               scalar_comparison_op,
                                               Item *value);
public:
  void set_table_name(String *alias)
  {
    table_name= &alias->Ptr;
  }
  void init(TABLE *table_arg)
  {
    orig_table= table= table_arg;
    set_table_name(&table_arg->alias);
  }
  virtual void init_for_tmp_table(Field *org_field, TABLE *new_table)
  {
    init(new_table);
    orig_table= org_field->orig_table;
    vcol_info= 0;
    cond_selectivity= 1.0;
    next_equal_field= NULL;
    option_list= NULL;
    option_struct= NULL;
    if (org_field->type() == MYSQL_TYPE_VAR_STRING ||
        org_field->type() == MYSQL_TYPE_VARCHAR)
      new_table->s->db_create_options|= HA_OPTION_PACK_RECORD;
  }
  void init_for_make_new_field(TABLE *new_table_arg, TABLE *orig_table_arg)
  {
    init(new_table_arg);
    /*
      Normally orig_table is different from table only if field was
      created via ::make_new_field.  Here we alter the type of field,
      so ::make_new_field is not applicable. But we still need to
      preserve the original field metadata for the client-server
      protocol.
    */
    orig_table= orig_table_arg;
  }

  /* maximum possible display length */
  virtual uint32 max_display_length() const= 0;
  /**
    Whether a field being created has the samle type.
    Used by the ALTER TABLE
  */
  virtual bool is_equal(const Column_definition &new_field) const= 0;
  // Used as double dispatch pattern: calls virtual method of handler
  virtual bool
  can_be_converted_by_engine(const Column_definition &new_type) const
  {
    return false;
  }
  /* convert decimal to longlong with overflow check */
  longlong convert_decimal2longlong(const my_decimal *val, bool unsigned_flag,
                                    int *err);
  /*
    Maximum number of bytes in character representation.
    - For string types it is equal to the field capacity, in bytes.
    - For non-string types it represents the longest possible string length
      after conversion to string.
  */
  virtual uint32 character_octet_length() const
  {
    return field_length;
  }
  /* The max. number of characters */
  virtual uint32 char_length() const
  {
    return field_length / charset()->mbmaxlen;
  }

  ha_storage_media field_storage_type() const
  {
    return (ha_storage_media)
      ((flags >> FIELD_FLAGS_STORAGE_MEDIA) & 3);
  }

  void set_storage_type(ha_storage_media storage_type_arg)
  {
    DBUG_ASSERT(field_storage_type() == HA_SM_DEFAULT);
    flags |= static_cast<uint32>(storage_type_arg) <<
      FIELD_FLAGS_STORAGE_MEDIA;
  }

  column_format_type column_format() const
  {
    return (column_format_type)
      ((flags >> FIELD_FLAGS_COLUMN_FORMAT) & 3);
  }

  void set_column_format(column_format_type column_format_arg)
  {
    DBUG_ASSERT(column_format() == COLUMN_FORMAT_TYPE_DEFAULT);
    flags |= static_cast<uint32>(column_format_arg) <<
      FIELD_FLAGS_COLUMN_FORMAT;
  }

  bool vers_sys_field() const
  {
    return flags & (VERS_ROW_START | VERS_ROW_END);
  }

  bool vers_update_unversioned() const
  {
    return flags & VERS_UPDATE_UNVERSIONED_FLAG;
  }

  /*
    Validate a non-null field value stored in the given record
    according to the current thread settings, e.g. sql_mode.
    @param thd     - the thread
    @param record  - the record to check in
  */
  virtual bool validate_value_in_record(THD *thd, const uchar *record) const
  { return false; }
  bool validate_value_in_record_with_warn(THD *thd, const uchar *record);
  key_map get_possible_keys();

  /* Hash value */
  virtual void hash(ulong *nr, ulong *nr2);

  /**
    Get the upper limit of the MySQL integral and floating-point type.

    @return maximum allowed value for the field
  */
  virtual ulonglong get_max_int_value() const
  {
    DBUG_ASSERT(false);
    return 0ULL;
  }

/**
  Checks whether a string field is part of write_set.

  @return
    FALSE  - If field is not char/varchar/....
           - If field is char/varchar/.. and is not part of write set.
    TRUE   - If field is char/varchar/.. and is part of write set.
*/
  virtual bool is_varchar_and_in_write_set() const { return FALSE; }

  /* Check whether the field can be used as a join attribute in hash join */
  virtual bool hash_join_is_possible() { return TRUE; }
  virtual bool eq_cmp_as_binary() { return TRUE; }

  /* Position of the field value within the interval of [min, max] */
  virtual double pos_in_interval(Field *min, Field *max)
  {
    return (double) 0.5; 
  }
  virtual bool pos_through_val_str() { return false;}

  /*
    Check if comparison between the field and an item unambiguously
    identifies a distinct field value.

    Example1: SELECT * FROM t1 WHERE int_column=10;
              This example returns distinct integer value of 10.

    Example2: SELECT * FROM t1 WHERE varchar_column=DATE'2001-01-01'
              This example returns non-distinct values.
              Comparison as DATE will return '2001-01-01' and '2001-01-01x',
              but these two values are not equal to each other as VARCHARs.
    See also the function with the same name in sql_select.cc.
  */
  virtual bool test_if_equality_guarantees_uniqueness(const Item *const_item)
                                                      const;
  virtual bool can_be_substituted_to_equal_item(const Context &ctx,
                                        const Item_equal *item);
  virtual Item *get_equal_const_item(THD *thd, const Context &ctx,
                                     Item *const_item)
  {
    return const_item;
  }
  virtual bool can_optimize_keypart_ref(const Item_bool_func *cond,
                                        const Item *item) const;
  virtual bool can_optimize_hash_join(const Item_bool_func *cond,
                                      const Item *item) const
  {
    return can_optimize_keypart_ref(cond, item);
  }
  virtual bool can_optimize_group_min_max(const Item_bool_func *cond,
                                          const Item *const_item) const;
  /**
    Test if Field can use range optimizer for a standard comparison operation:
      <=, <, =, <=>, >, >=
    Note, this method does not cover spatial operations.
  */
  virtual bool can_optimize_range(const Item_bool_func *cond,
                                  const Item *item,
                                  bool is_eq_func) const;

  virtual SEL_ARG *get_mm_leaf(RANGE_OPT_PARAM *param, KEY_PART *key_part,
                               const Item_bool_func *cond,
                               scalar_comparison_op op, Item *value)= 0;

  bool can_optimize_outer_join_table_elimination(const Item_bool_func *cond,
                                                 const Item *item) const
  {
    // Exactly the same rules with REF access
    return can_optimize_keypart_ref(cond, item);
  }

  bool save_in_field_default_value(bool view_eror_processing);
  bool save_in_field_ignore_value(bool view_error_processing);

  /* Mark field in read map. Updates also virtual fields */
  void register_field_in_read_map();

  virtual Compression_method *compression_method() const { return 0; }

  virtual Virtual_tmp_table **virtual_tmp_table_addr()
  {
    return NULL;
  }
  virtual bool sp_prepare_and_store_item(THD *thd, Item **value);

  friend int cre_myisam(char * name, TABLE *form, uint options,
			ulonglong auto_increment_value);
  friend class Copy_field;
  friend class Item_avg_field;
  friend class Item_std_field;
  friend class Item_sum_num;
  friend class Item_sum_sum;
  friend class Item_sum_count;
  friend class Item_sum_avg;
  friend class Item_sum_std;
  friend class Item_sum_min;
  friend class Item_sum_max;
  friend class Item_func_group_concat;

private:
  /*
    Primitive for implementing last_null_byte().

    SYNOPSIS
      do_last_null_byte()

    DESCRIPTION
      Primitive for the implementation of the last_null_byte()
      function. This represents the inheritance interface and can be
      overridden by subclasses.
   */
  virtual size_t do_last_null_byte() const;

protected:
  uchar *pack_int(uchar *to, const uchar *from, size_t size)
  {
    memcpy(to, from, size);
    return to + size;
  }

  const uchar *unpack_int(uchar* to, const uchar *from,
                          const uchar *from_end, size_t size)
  {
    if (from + size > from_end)
      return 0;
    memcpy(to, from, size);
    return from + size;
  }

  uchar *pack_int16(uchar *to, const uchar *from)
  { return pack_int(to, from, 2); }
  const uchar *unpack_int16(uchar* to, const uchar *from, const uchar *from_end)
  { return unpack_int(to, from, from_end, 2); }
  uchar *pack_int24(uchar *to, const uchar *from)
  { return pack_int(to, from, 3); }
  const uchar *unpack_int24(uchar* to, const uchar *from, const uchar *from_end)
  { return unpack_int(to, from, from_end, 3); }
  uchar *pack_int32(uchar *to, const uchar *from)
  { return pack_int(to, from, 4); }
  const uchar *unpack_int32(uchar* to, const uchar *from, const uchar *from_end)
  { return unpack_int(to, from, from_end, 4); }
  uchar *pack_int64(uchar* to, const uchar *from)
  { return pack_int(to, from, 8); }
  const uchar *unpack_int64(uchar* to, const uchar *from,  const uchar *from_end)
  { return unpack_int(to, from, from_end, 8); }

  double pos_in_interval_val_real(Field *min, Field *max);
  double pos_in_interval_val_str(Field *min, Field *max, uint data_offset);
};


class Field_num :public Field {
protected:
  int check_edom_and_important_data_truncation(const char *type, bool edom,
                                               CHARSET_INFO *cs,
                                               const char *str, size_t length,
                                               const char *end_of_num);
  int check_edom_and_truncation(const char *type, bool edom,
                                CHARSET_INFO *cs,
                                const char *str, size_t length,
                                const char *end_of_num);
  int check_int(CHARSET_INFO *cs, const char *str, size_t length,
                const char *int_end, int error)
  {
    return check_edom_and_truncation("integer",
                                     error == MY_ERRNO_EDOM || str == int_end,
                                     cs, str, length, int_end);
  }
  bool get_int(CHARSET_INFO *cs, const char *from, size_t len,
               longlong *rnd, ulonglong unsigned_max,
               longlong signed_min, longlong signed_max);
  void prepend_zeros(String *value) const;
  Item *get_equal_zerofill_const_item(THD *thd, const Context &ctx,
                                      Item *const_item);
  Binlog_type_info::binlog_sign_t binlog_signedness() const
  {
    return (flags & UNSIGNED_FLAG) ? Binlog_type_info::SIGN_UNSIGNED :
                                     Binlog_type_info::SIGN_SIGNED;
  }
  bool send_numeric_zerofill_str(Protocol_text *protocol,
                                 protocol_send_type_t send_type);

public:
  const decimal_digits_t dec;
  bool zerofill,unsigned_flag;	// Purify cannot handle bit fields
  Field_num(uchar *ptr_arg,uint32 len_arg, uchar *null_ptr_arg,
	    uchar null_bit_arg, utype unireg_check_arg,
	    const LEX_CSTRING *field_name_arg,
            decimal_digits_t dec_arg, bool zero_arg, bool unsigned_arg);
  CHARSET_INFO *charset() const override
  {
    return DTCollation_numeric::singleton().collation;
  }
  const DTCollation &dtcollation() const override
  {
    return DTCollation_numeric::singleton();
  }
  sql_mode_t can_handle_sql_mode_dependency_on_store() const override;
  Item *get_equal_const_item(THD *thd, const Context &ctx, Item *const_item)
    override
  {
    return (flags & ZEROFILL_FLAG) ?
           get_equal_zerofill_const_item(thd, ctx, const_item) :
           const_item;
  }
  void add_zerofill_and_unsigned(String &res) const;
  friend class Create_field;
  void make_send_field(Send_field *) override;
  decimal_digits_t decimals() const override { return dec; }
  uint size_of() const override { return sizeof(*this); }
  bool eq_def(const Field *field) const override;
  Copy_func *get_copy_func(const Field *from) const override
  {
    if (unsigned_flag && from->cmp_type() == DECIMAL_RESULT)
      return do_field_decimal;
    return do_field_int;
  }
  int save_in_field(Field *to) override
  {
    return to->store(val_int(), MY_TEST(flags & UNSIGNED_FLAG));
  }
  bool is_equal(const Column_definition &new_field) const override;
  uint row_pack_length() const override { return pack_length(); }
  uint32 pack_length_from_metadata(uint field_metadata) const override
  {
    uint32 length= pack_length();
    DBUG_PRINT("result", ("pack_length_from_metadata(%d): %u",
                          field_metadata, length));
    return length;
  }
  double pos_in_interval(Field *min, Field *max) override
  {
    return pos_in_interval_val_real(min, max);
  }
  SEL_ARG *get_mm_leaf(RANGE_OPT_PARAM *param, KEY_PART *key_part,
                       const Item_bool_func *cond,
                       scalar_comparison_op op, Item *value) override;
  Binlog_type_info binlog_type_info() const override
  {
    DBUG_ASSERT(Field_num::type() == binlog_type());
    return Binlog_type_info(Field_num::type(), 0, 0, binlog_signedness());
  }
};


class Field_str :public Field {
protected:
  DTCollation m_collation;
  // A short alias for m_collation.collation with non-virtual linkage
  const CHARSET_INFO *field_charset() const { return m_collation.collation; }
  uint mbmaxlen() const { return m_collation.collation->mbmaxlen; }
public:
  bool can_be_substituted_to_equal_item(const Context &ctx,
                                        const Item_equal *item_equal) override;
  Field_str(uchar *ptr_arg,uint32 len_arg, uchar *null_ptr_arg,
	    uchar null_bit_arg, utype unireg_check_arg,
	    const LEX_CSTRING *field_name_arg,
	    const DTCollation &collation);
  decimal_digits_t decimals() const override
  { return is_created_from_null_item ? 0 : NOT_FIXED_DEC; }
  int  save_in_field(Field *to) override { return save_in_field_str(to); }
  bool memcpy_field_possible(const Field *from) const override
  {
    return real_type() == from->real_type() &&
           pack_length() == from->pack_length() &&
           charset() == from->charset();
  }
  int  store(double nr) override;
  int  store(longlong nr, bool unsigned_val) override;
  int  store_decimal(const my_decimal *) override;
  int  store(const char *to,size_t length,CHARSET_INFO *cs) override=0;
  int  store_hex_hybrid(const char *str, size_t length) override
  {
    return store(str, length, &my_charset_bin);
  }
  CHARSET_INFO *charset() const override { return m_collation.collation; }
  const DTCollation &dtcollation() const override
  {
    return m_collation;
  }
  void change_charset(const DTCollation &new_cs) override;
  bool binary() const override { return field_charset() == &my_charset_bin; }
  uint32 max_display_length() const override { return field_length; }
  uint32 character_octet_length() const override { return field_length; }
  uint32 char_length() const override
  {
    return field_length / mbmaxlen();
  }
  Information_schema_character_attributes
    information_schema_character_attributes() const override
  {
    return Information_schema_character_attributes(max_display_length(),
                                                   char_length());
  }
  friend class Create_field;
  my_decimal *val_decimal(my_decimal *) override;
  bool val_bool() override { return val_real() != 0e0; }
  bool str_needs_quotes() const override { return true; }
  bool eq_cmp_as_binary() override { return MY_TEST(flags & BINARY_FLAG); }
  virtual uint length_size() const { return 0; }
  double pos_in_interval(Field *min, Field *max) override
  {
    return pos_in_interval_val_str(min, max, length_size());
  }
  bool pos_through_val_str() override {return true;}

  bool test_if_equality_guarantees_uniqueness(const Item *const_item) const
    override;
  SEL_ARG *get_mm_leaf(RANGE_OPT_PARAM *param, KEY_PART *key_part,
                       const Item_bool_func *cond,
                       scalar_comparison_op op, Item *value) override;
  Binlog_type_info binlog_type_info() const override
  {
    DBUG_ASSERT(Field_str::type() == binlog_type());
    return Binlog_type_info(Field_str::type(), 0, 0, charset());
  }
};

/* base class for Field_string, Field_varstring and Field_blob */

class Field_longstr :public Field_str
{
protected:
  int report_if_important_data(const char *ptr, const char *end,
                               bool count_spaces);
  bool check_string_copy_error(const String_copier *copier,
                               const char *end, CHARSET_INFO *cs);
  int check_conversion_status(const String_copier *copier,
                              const char *end, CHARSET_INFO *cs,
                              bool count_spaces)
  {
    if (check_string_copy_error(copier, end, cs))
      return 2;
    return report_if_important_data(copier->source_end_pos(),
                                    end, count_spaces);
  }
  int well_formed_copy_with_check(char *to, size_t to_length,
                                  CHARSET_INFO *from_cs,
                                  const char *from, size_t from_length,
                                  size_t nchars, bool count_spaces,
                                  uint *copy_length)
  {
    String_copier copier;

    *copy_length= copier.well_formed_copy(field_charset(), to, to_length,
                                          from_cs, from, from_length,
                                          nchars);

    return check_conversion_status(&copier, from + from_length, from_cs, count_spaces);
  }
  bool cmp_to_string_with_same_collation(const Item_bool_func *cond,
                                         const Item *item) const;
  bool cmp_to_string_with_stricter_collation(const Item_bool_func *cond,
                                             const Item *item) const;
  int compress(char *to, uint to_length,
               const char *from, uint length,
               uint max_length,
               uint *out_length,
               CHARSET_INFO *cs, size_t nchars);
  String *uncompress(String *val_buffer, String *val_ptr,
                     const uchar *from, uint from_length) const;
public:
  Field_longstr(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
                uchar null_bit_arg, utype unireg_check_arg,
                const LEX_CSTRING *field_name_arg,
                const DTCollation &collation)
    :Field_str(ptr_arg, len_arg, null_ptr_arg, null_bit_arg, unireg_check_arg,
               field_name_arg, collation)
    {}
  enum_conv_type rpl_conv_type_from(const Conv_source &source,
                                    const Relay_log_info *rli,
                                    const Conv_param &param) const override;
  int store_decimal(const my_decimal *d) override;
  uint32 max_data_length() const override;
  void make_send_field(Send_field *) override;
  bool send(Protocol *protocol) override;

  bool is_varchar_and_in_write_set() const override
  {
    DBUG_ASSERT(table && table->write_set);
    return bitmap_is_set(table->write_set, field_index);
  }
  bool match_collation_to_optimize_range() const { return true; }

  bool can_optimize_keypart_ref(const Item_bool_func *cond,
                                const Item *item) const override;
  bool can_optimize_hash_join(const Item_bool_func *cond,
                              const Item *item) const override;
  bool can_optimize_group_min_max(const Item_bool_func *cond,
                                  const Item *const_item) const override;
  bool can_optimize_range(const Item_bool_func *cond,
                          const Item *item,
                          bool is_eq_func) const override;
  bool is_packable() const override { return true; }
  uint make_packed_sort_key_part(uchar *buff,
                                 const SORT_FIELD_ATTR *sort_field)override;
  uchar* pack_sort_string(uchar *to, const SORT_FIELD_ATTR *sort_field);
};

/* base class for float and double and decimal (old one) */
class Field_real :public Field_num {
protected:
  double get_double(const char *str, size_t length, CHARSET_INFO *cs, int *err);
public:
  bool not_fixed;

  Field_real(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
             uchar null_bit_arg, utype unireg_check_arg,
             const LEX_CSTRING *field_name_arg,
             decimal_digits_t dec_arg, bool zero_arg, bool unsigned_arg)
    :Field_num(ptr_arg, len_arg, null_ptr_arg, null_bit_arg, unireg_check_arg,
               field_name_arg, dec_arg, zero_arg, unsigned_arg),
    not_fixed(dec_arg >= FLOATING_POINT_DECIMALS)
    {}
  Copy_func *get_copy_func(const Field *from) const override
  {
    return do_field_real;
  }
  enum_conv_type rpl_conv_type_from(const Conv_source &source,
                                    const Relay_log_info *rli,
                                    const Conv_param &param) const override;
  Information_schema_numeric_attributes
    information_schema_numeric_attributes() const override
  {
    return dec == NOT_FIXED_DEC ?
                  Information_schema_numeric_attributes(field_length) :
                  Information_schema_numeric_attributes(field_length, dec);
  }
  void sql_type(String &str) const override;
  int save_in_field(Field *to) override { return to->store(val_real()); }
  bool memcpy_field_possible(const Field *from) const override
  {
    /*
      Cannot do memcpy from a longer field to a shorter field,
      e.g. a DOUBLE(53,10) into a DOUBLE(10,10).
      But it should be OK the other way around.
    */
    return real_type() == from->real_type() &&
           pack_length() == from->pack_length() &&
           is_unsigned() <= from->is_unsigned() &&
           decimals() == from->decimals() &&
           field_length >= from->field_length;
  }
  int store_decimal(const my_decimal *dec) override
  { return store(dec->to_double()); }
  int  store_time_dec(const MYSQL_TIME *ltime, uint dec) override;
  bool get_date(MYSQL_TIME *ltime, date_mode_t fuzzydate) override;
  my_decimal *val_decimal(my_decimal *) override;
  bool val_bool() override { return val_real() != 0e0; }
  uint32 max_display_length() const override { return field_length; }
  uint size_of() const override { return sizeof *this; }
  Item *get_equal_const_item(THD *thd, const Context &ctx, Item *const_item)
    override;
};


class Field_decimal final  :public Field_real {
public:
  Field_decimal(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
		uchar null_bit_arg,
		enum utype unireg_check_arg, const LEX_CSTRING *field_name_arg,
		decimal_digits_t dec_arg, bool zero_arg,bool unsigned_arg)
    :Field_real(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
                unireg_check_arg, field_name_arg,
                dec_arg, zero_arg, unsigned_arg)
    {}
  Field *make_new_field(MEM_ROOT *root, TABLE *new_table, bool keep_type)
    override;
  const Type_handler *type_handler() const override
  { return &type_handler_olddecimal; }
  enum ha_base_keytype key_type() const override
  { return zerofill ? HA_KEYTYPE_BINARY : HA_KEYTYPE_NUM; }
  Information_schema_numeric_attributes
    information_schema_numeric_attributes() const override
  {
    uint tmp= dec ? 2 : 1; // The sign and the decimal point
    return Information_schema_numeric_attributes(field_length - tmp, dec);
  }
  Copy_func *get_copy_func(const Field *from) const override
  {
    return eq_def(from) ? get_identical_copy_func() : do_field_string;
  }
  int reset() override;
  int store(const char *to,size_t length,CHARSET_INFO *charset) override;
  int store(double nr) override;
  int store(longlong nr, bool unsigned_val) override;
  double val_real() override;
  longlong val_int() override;
  String *val_str(String *, String *) override;
  int cmp(const uchar *,const uchar *) const override;
  void sort_string(uchar *buff,uint length) override;
  void overflow(bool negative);
  bool zero_pack() const override { return false; }
  void sql_type(String &str) const override;
  uchar *pack(uchar* to, const uchar *from, uint max_length) override
  {
    return Field::pack(to, from, max_length);
  }
};


/* New decimal/numeric field which use fixed point arithmetic */
class Field_new_decimal final :public Field_num {
public:
  /* The maximum number of decimal digits can be stored */
  decimal_digits_t precision;
  uint32 bin_size;
  /*
    Constructors take max_length of the field as a parameter - not the
    precision as the number of decimal digits allowed.
    So for example we need to count length from precision handling
    CREATE TABLE ( DECIMAL(x,y)) 
  */
  Field_new_decimal(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
                    uchar null_bit_arg,
                    enum utype unireg_check_arg,
                    const LEX_CSTRING *field_name_arg,
                    decimal_digits_t dec_arg, bool zero_arg, bool unsigned_arg);
  const Type_handler *type_handler() const override
  { return &type_handler_newdecimal; }
  enum ha_base_keytype key_type() const override { return HA_KEYTYPE_BINARY; }
  Copy_func *get_copy_func(const Field *from) const override
  {
    //  if (from->real_type() == MYSQL_TYPE_BIT) // QQ: why?
    //    return do_field_int;
    return do_field_decimal;
  }
  int save_in_field(Field *to) override
  {
    my_decimal tmp(ptr, precision, dec);
    return to->store_decimal(&tmp);
  }
  bool memcpy_field_possible(const Field *from) const override
  {
    return real_type() == from->real_type() &&
           pack_length() == from->pack_length() &&
           is_unsigned() <= from->is_unsigned() &&
           decimals() == from->decimals() &&
           field_length == from->field_length;
  }
  enum_conv_type rpl_conv_type_from(const Conv_source &source,
                                    const Relay_log_info *rli,
                                    const Conv_param &param) const override;
  int  reset() override;
  bool store_value(const my_decimal *decimal_value);
  bool store_value(const my_decimal *decimal_value, int *native_error);
  void set_value_on_overflow(my_decimal *decimal_value, bool sign);
  int  store(const char *to, size_t length, CHARSET_INFO *charset) override;
  int  store(double nr) override;
  int  store(longlong nr, bool unsigned_val) override;
  int  store_time_dec(const MYSQL_TIME *ltime, uint dec) override;
  int  store_decimal(const my_decimal *) override;
  double val_real() override
  {
    return my_decimal(ptr, precision, dec).to_double();
  }
  longlong val_int() override
  {
    return my_decimal(ptr, precision, dec).to_longlong(unsigned_flag);
  }
  ulonglong val_uint() override
  {
    return (ulonglong) my_decimal(ptr, precision, dec).to_longlong(true);
  }
  my_decimal *val_decimal(my_decimal *) override;
  String *val_str(String *val_buffer, String *) override
  {
    uint fixed_precision= zerofill ? precision : 0;
    return my_decimal(ptr, precision, dec).
             to_string(val_buffer, fixed_precision, dec, '0');
  }
  bool get_date(MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  {
    my_decimal nr(ptr, precision, dec);
    return decimal_to_datetime_with_warn(get_thd(), &nr, ltime,
                                         fuzzydate, table->s, field_name.str);
  }
  bool val_bool() override
  {
    return my_decimal(ptr, precision, dec).to_bool();
  }
  int cmp(const uchar *, const uchar *) const override;
  void sort_string(uchar *buff, uint length) override;
  bool zero_pack() const override { return false; }
  void sql_type(String &str) const override;
  uint32 max_display_length() const override { return field_length; }
  Information_schema_numeric_attributes
    information_schema_numeric_attributes() const override
  {
    return Information_schema_numeric_attributes(precision, dec);
  }
  uint size_of() const override { return sizeof *this; }
  uint32 pack_length() const override { return bin_size; }
  uint pack_length_from_metadata(uint field_metadata) const override;
  uint row_pack_length() const override { return pack_length(); }
  bool compatible_field_size(uint field_metadata, const Relay_log_info *rli,
                             uint16 mflags, int *order_var) const override;
  bool is_equal(const Column_definition &new_field) const override;
  const uchar *unpack(uchar* to, const uchar *from, const uchar *from_end,
                      uint param_data) override;
  Item *get_equal_const_item(THD *thd, const Context &ctx, Item *const_item)
    override;
  Binlog_type_info binlog_type_info() const override;
};


class Field_int :public Field_num
{
protected:
  String *val_str_from_long(String *val_buffer, uint max_char_length,
                            int radix, long nr);
public:
  Field_int(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
            uchar null_bit_arg, enum utype unireg_check_arg,
            const LEX_CSTRING *field_name_arg, bool zero_arg, bool unsigned_arg)
    :Field_num(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
               unireg_check_arg, field_name_arg, 0, zero_arg, unsigned_arg)
    {}
  enum_conv_type rpl_conv_type_from(const Conv_source &source,
                                    const Relay_log_info *rli,
                                    const Conv_param &param) const override;
  bool memcpy_field_possible(const Field *from) const override
  {
    return real_type() == from->real_type() &&
           pack_length() == from->pack_length() &&
           is_unsigned() == from->is_unsigned();
  }
  int store_decimal(const my_decimal *) override;
  my_decimal *val_decimal(my_decimal *) override;
  bool val_bool() override { return val_int() != 0; }
  ulonglong val_uint() override
  {
    longlong nr= val_int();
    return nr < 0 && !unsigned_flag ? 0 : (ulonglong) nr;
  }
  int  store_time_dec(const MYSQL_TIME *ltime, uint dec) override;
  bool get_date(MYSQL_TIME *ltime, date_mode_t fuzzydate) override;
  virtual const Type_limits_int *type_limits_int() const= 0;
  uint32 max_display_length() const override
  {
    return type_limits_int()->char_length();
  }
  Type_numeric_attributes type_numeric_attributes() const override
  {
    /*
      For integer data types, the user-specified length does not constrain the
      supported range, so e.g. a column of the INT(1) data type supports the
      full integer range anyway.
      Choose the maximum from the user-specified length and the maximum
      possible length determined by the data type capacity:
        INT(1)  -> 11
        INT(10) -> 11
        INT(40) -> 40
    */
    uint32 length1= max_display_length();
    uint32 length2= field_length;
    return Type_numeric_attributes(MY_MAX(length1, length2),
                                   decimals(), is_unsigned());
  }
  Information_schema_numeric_attributes
    information_schema_numeric_attributes() const override
  {
    uint32 prec= type_limits_int()->precision();
    return Information_schema_numeric_attributes(prec, 0);
  }
  void sql_type(String &str) const override;
  SEL_ARG *get_mm_leaf(RANGE_OPT_PARAM *param, KEY_PART *key_part,
                       const Item_bool_func *cond,
                       scalar_comparison_op op, Item *value) override
  {
    return get_mm_leaf_int(param, key_part, cond, op, value, unsigned_flag);
  }
};


class Field_tiny :public Field_int
{
  const Type_handler_general_purpose_int *type_handler_priv() const
  {
    if (is_unsigned())
      return &type_handler_utiny;
    return &type_handler_stiny;
  }
public:
  Field_tiny(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
	     uchar null_bit_arg,
	     enum utype unireg_check_arg, const LEX_CSTRING *field_name_arg,
	     bool zero_arg, bool unsigned_arg)
    :Field_int(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
               unireg_check_arg, field_name_arg, zero_arg, unsigned_arg)
    {}
  const Type_handler *type_handler() const override
    { return type_handler_priv(); }
  enum ha_base_keytype key_type() const override
    { return unsigned_flag ? HA_KEYTYPE_BINARY : HA_KEYTYPE_INT8; }
  int store(const char *to,size_t length,CHARSET_INFO *charset) override;
  int store(double nr) override;
  int store(longlong nr, bool unsigned_val) override;
  int reset() override { ptr[0]=0; return 0; }
  double val_real() override;
  longlong val_int() override;
  String *val_str(String *, String *) override;
  bool send(Protocol *protocol) override;
  int cmp(const uchar *,const uchar *) const override;
  void sort_string(uchar *buff,uint length) override;
  uint32 pack_length() const override { return 1; }
  const Type_limits_int *type_limits_int() const override
  {
    return type_handler_priv()->type_limits_int();
  }

  uchar *pack(uchar* to, const uchar *from, uint max_length) override
  {
    *to= *from;
    return to + 1;
  }

  const uchar *unpack(uchar* to, const uchar *from,
		      const uchar *from_end, uint param_data) override
  {
    if (from == from_end)
      return 0;
    *to= *from;
    return from + 1;
  }
  ulonglong get_max_int_value() const override
  {
    return unsigned_flag ? 0xFFULL : 0x7FULL;
  }
};


class Field_short final :public Field_int
{
  const Type_handler_general_purpose_int *type_handler_priv() const
  {
    if (is_unsigned())
      return &type_handler_ushort;
    return &type_handler_sshort;
  }
public:
  Field_short(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
	      uchar null_bit_arg,
	      enum utype unireg_check_arg, const LEX_CSTRING *field_name_arg,
	      bool zero_arg, bool unsigned_arg)
    :Field_int(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
               unireg_check_arg, field_name_arg, zero_arg, unsigned_arg)
    {}
  Field_short(uint32 len_arg,bool maybe_null_arg,
              const LEX_CSTRING *field_name_arg,
	      bool unsigned_arg)
    :Field_int((uchar*) 0, len_arg, maybe_null_arg ? (uchar*) "": 0,0,
               NONE, field_name_arg, 0, unsigned_arg)
    {}
  const Type_handler *type_handler() const override
  { return type_handler_priv(); }
  enum ha_base_keytype key_type() const override
    { return unsigned_flag ? HA_KEYTYPE_USHORT_INT : HA_KEYTYPE_SHORT_INT;}
  int store(const char *to,size_t length,CHARSET_INFO *charset) override;
  int store(double nr) override;
  int store(longlong nr, bool unsigned_val) override;
  int reset() override { ptr[0]=ptr[1]=0; return 0; }
  double val_real() override;
  longlong val_int() override;
  String *val_str(String *, String *) override;
  bool send(Protocol *protocol) override;
  int cmp(const uchar *,const uchar *) const override;
  void sort_string(uchar *buff,uint length) override;
  uint32 pack_length() const override { return 2; }
  const Type_limits_int *type_limits_int() const override
  {
    return type_handler_priv()->type_limits_int();
  }
  uchar *pack(uchar* to, const uchar *from, uint) override
  { return pack_int16(to, from); }

  const uchar *unpack(uchar* to, const uchar *from,
                      const uchar *from_end, uint) override
  { return unpack_int16(to, from, from_end); }
  ulonglong get_max_int_value() const override
  {
    return unsigned_flag ? 0xFFFFULL : 0x7FFFULL;
  }
};

class Field_medium final :public Field_int
{
  const Type_handler_general_purpose_int *type_handler_priv() const
  {
    if (is_unsigned())
      return &type_handler_uint24;
    return &type_handler_sint24;
  }
public:
  Field_medium(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
	      uchar null_bit_arg,
	      enum utype unireg_check_arg, const LEX_CSTRING *field_name_arg,
	      bool zero_arg, bool unsigned_arg)
    :Field_int(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
               unireg_check_arg, field_name_arg, zero_arg, unsigned_arg)
    {}
  const Type_handler *type_handler() const override
  { return type_handler_priv(); }
  enum ha_base_keytype key_type() const override
    { return unsigned_flag ? HA_KEYTYPE_UINT24 : HA_KEYTYPE_INT24; }
  int store(const char *to,size_t length,CHARSET_INFO *charset) override;
  int store(double nr) override;
  int store(longlong nr, bool unsigned_val) override;
  int reset() override { ptr[0]=ptr[1]=ptr[2]=0; return 0; }
  double val_real() override;
  longlong val_int() override;
  String *val_str(String *, String *) override;
  bool send(Protocol *protocol) override;
  int cmp(const uchar *,const uchar *) const override;
  void sort_string(uchar *buff,uint length) override;
  uint32 pack_length() const override { return 3; }
  const Type_limits_int *type_limits_int() const override
  {
    return type_handler_priv()->type_limits_int();
  }
  uchar *pack(uchar* to, const uchar *from, uint max_length) override
  {
    return Field::pack(to, from, max_length);
  }
  ulonglong get_max_int_value() const override
  {
    return unsigned_flag ? 0xFFFFFFULL : 0x7FFFFFULL;
  }
};


class Field_long final :public Field_int
{
  const Type_handler_general_purpose_int *type_handler_priv() const
  {
    if (is_unsigned())
      return &type_handler_ulong;
    return &type_handler_slong;
  }
public:
  Field_long(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
	     uchar null_bit_arg,
	     enum utype unireg_check_arg, const LEX_CSTRING *field_name_arg,
	     bool zero_arg, bool unsigned_arg)
    :Field_int(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
               unireg_check_arg, field_name_arg, zero_arg, unsigned_arg)
    {}
  Field_long(uint32 len_arg,bool maybe_null_arg,
             const LEX_CSTRING *field_name_arg,
	     bool unsigned_arg)
    :Field_int((uchar*) 0, len_arg, maybe_null_arg ? (uchar*) "": 0,0,
               NONE, field_name_arg, 0, unsigned_arg)
    {}
  const Type_handler *type_handler() const override
  { return type_handler_priv(); }
  enum ha_base_keytype key_type() const override
    { return unsigned_flag ? HA_KEYTYPE_ULONG_INT : HA_KEYTYPE_LONG_INT; }
  int store(const char *to,size_t length,CHARSET_INFO *charset) override;
  int store(double nr) override;
  int store(longlong nr, bool unsigned_val) override;
  int reset() override { ptr[0]=ptr[1]=ptr[2]=ptr[3]=0; return 0; }
  double val_real() override;
  longlong val_int() override;
  bool send(Protocol *protocol) override;
  String *val_str(String *, String *) override;
  int cmp(const uchar *,const uchar *) const override;
  void sort_string(uchar *buff,uint length) override;
  uint32 pack_length() const override { return 4; }
  const Type_limits_int *type_limits_int() const override
  {
    return type_handler_priv()->type_limits_int();
  }
  uchar *pack(uchar* to, const uchar *from, uint) override
  {
    return pack_int32(to, from);
  }
  const uchar *unpack(uchar* to, const uchar *from,
                      const uchar *from_end, uint) override
  {
    return unpack_int32(to, from, from_end);
  }
  ulonglong get_max_int_value() const override
  {
    return unsigned_flag ? 0xFFFFFFFFULL : 0x7FFFFFFFULL;
  }
};


class Field_longlong :public Field_int
{
  const Type_handler_general_purpose_int *type_handler_priv() const
  {
    if (is_unsigned())
      return &type_handler_ulonglong;
    return &type_handler_slonglong;
  }
public:
  Field_longlong(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
	      uchar null_bit_arg,
	      enum utype unireg_check_arg, const LEX_CSTRING *field_name_arg,
	      bool zero_arg, bool unsigned_arg)
    :Field_int(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
               unireg_check_arg, field_name_arg, zero_arg, unsigned_arg)
    {}
  Field_longlong(uint32 len_arg,bool maybe_null_arg,
		 const LEX_CSTRING *field_name_arg,
                 bool unsigned_arg)
    :Field_int((uchar*) 0, len_arg, maybe_null_arg ? (uchar*) "": 0,0,
                NONE, field_name_arg, 0, unsigned_arg)
    {}
  const Type_handler *type_handler() const override
  { return type_handler_priv(); }
  enum ha_base_keytype key_type() const override
  { return unsigned_flag ? HA_KEYTYPE_ULONGLONG : HA_KEYTYPE_LONGLONG; }
  int store(const char *to,size_t length,CHARSET_INFO *charset) override;
  int store(double nr) override;
  int store(longlong nr, bool unsigned_val) override;
  int reset() override
  {
    ptr[0]=ptr[1]=ptr[2]=ptr[3]=ptr[4]=ptr[5]=ptr[6]=ptr[7]=0;
    return 0;
  }
  double val_real() override;
  longlong val_int() override;
  String *val_str(String *, String *) override;
  bool send(Protocol *protocol) override;
  int cmp(const uchar *,const uchar *) const override;
  void sort_string(uchar *buff,uint length) override;
  uint32 pack_length() const override { return 8; }
  const Type_limits_int *type_limits_int() const override
  {
    return type_handler_priv()->type_limits_int();
  }
  uchar *pack(uchar* to, const uchar *from, uint) override
  {
    return pack_int64(to, from);
  }
  const uchar *unpack(uchar* to, const uchar *from, const uchar *from_end,
		      uint) override
  {
    return unpack_int64(to, from, from_end);
  }
  void set_max() override;
  bool is_max() override;
  ulonglong get_max_int_value() const override
  {
    return unsigned_flag ? 0xFFFFFFFFFFFFFFFFULL : 0x7FFFFFFFFFFFFFFFULL;
  }
};


class Field_vers_trx_id :public Field_longlong {
  MYSQL_TIME cache;
  ulonglong cached;
public:
  Field_vers_trx_id(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
                    uchar null_bit_arg, enum utype unireg_check_arg,
                    const LEX_CSTRING *field_name_arg, bool zero_arg,
                    bool unsigned_arg)
      : Field_longlong(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
                       unireg_check_arg, field_name_arg, zero_arg,
                       unsigned_arg),
        cached(0)
  {}
  const Type_handler *type_handler() const override
  { return &type_handler_vers_trx_id; }
  uint size_of() const override { return sizeof *this; }
  bool get_date(MYSQL_TIME *ltime, date_mode_t fuzzydate, ulonglong trx_id);
  bool get_date(MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  {
    return get_date(ltime, fuzzydate, (ulonglong) val_int());
  }
  bool test_if_equality_guarantees_uniqueness(const Item *item) const override;
  bool can_optimize_keypart_ref(const Item_bool_func *, const Item *)
    const override
  {
    return true;
  }

  bool can_optimize_group_min_max(const Item_bool_func *, const Item *)
    const override
  {
    return true;
  }
  bool can_optimize_range(const Item_bool_func *, const Item *, bool)
    const override
  {
    return true;
  }
  /* cmp_type() cannot be TIME_RESULT, because we want to compare this field against
     integers. But in all other cases we treat it as TIME_RESULT! */
};


class Field_float final :public Field_real {
public:
  Field_float(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
	      uchar null_bit_arg,
	      enum utype unireg_check_arg, const LEX_CSTRING *field_name_arg,
              decimal_digits_t dec_arg,bool zero_arg,bool unsigned_arg)
    :Field_real(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
                unireg_check_arg, field_name_arg,
                dec_arg, zero_arg, unsigned_arg)
    {
      if (dec_arg >= FLOATING_POINT_DECIMALS)
        dec_arg= NOT_FIXED_DEC;
    }
  Field_float(uint32 len_arg, bool maybe_null_arg,
              const LEX_CSTRING *field_name_arg, decimal_digits_t dec_arg)
    :Field_real((uchar*) 0, len_arg, maybe_null_arg ? (uchar*) "": 0, (uint) 0,
                NONE, field_name_arg, dec_arg, 0, 0)
    {
      if (dec_arg >= FLOATING_POINT_DECIMALS)
        dec_arg= NOT_FIXED_DEC;
    }
  const Type_handler *type_handler() const override
  { return &type_handler_float; }
  enum ha_base_keytype key_type() const override { return HA_KEYTYPE_FLOAT; }
  int store(const char *to,size_t length,CHARSET_INFO *charset) override;
  int store(double nr) override;
  int store(longlong nr, bool unsigned_val) override;
  int reset() override { bzero(ptr,sizeof(float)); return 0; }
  double val_real() override;
  longlong val_int() override;
  String *val_str(String *, String *) override;
  bool send(Protocol *protocol) override;
  int cmp(const uchar *,const uchar *) const override;
  void sort_string(uchar *buff, uint length) override;
  uint32 pack_length() const override { return sizeof(float); }
  uint row_pack_length() const override { return pack_length(); }
  ulonglong get_max_int_value() const override
  {
    /*
      We use the maximum as per IEEE754-2008 standard, 2^24
    */
    return 0x1000000ULL;
  }
  Binlog_type_info binlog_type_info() const override;
};


class Field_double :public Field_real {
  longlong val_int_from_real(bool want_unsigned_result);
public:
  Field_double(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
	       uchar null_bit_arg,
	       enum utype unireg_check_arg, const LEX_CSTRING *field_name_arg,
	       decimal_digits_t dec_arg,bool zero_arg,bool unsigned_arg)
    :Field_real(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
                unireg_check_arg, field_name_arg,
                dec_arg, zero_arg, unsigned_arg)
    {
      if (dec_arg >= FLOATING_POINT_DECIMALS)
        dec_arg= NOT_FIXED_DEC;
    }
  Field_double(uint32 len_arg, bool maybe_null_arg,
               const LEX_CSTRING *field_name_arg, decimal_digits_t dec_arg)
    :Field_real((uchar*) 0, len_arg, maybe_null_arg ? (uchar*) "" : 0, (uint) 0,
                NONE, field_name_arg, dec_arg, 0, 0)
    {
      if (dec_arg >= FLOATING_POINT_DECIMALS)
        dec_arg= NOT_FIXED_DEC;
    }
  Field_double(uint32 len_arg, bool maybe_null_arg,
               const LEX_CSTRING *field_name_arg,
	       decimal_digits_t dec_arg, bool not_fixed_arg)
    :Field_real((uchar*) 0, len_arg, maybe_null_arg ? (uchar*) "" : 0, (uint) 0,
                NONE, field_name_arg, dec_arg, 0, 0)
    {
      not_fixed= not_fixed_arg;
      if (dec_arg >= FLOATING_POINT_DECIMALS)
        dec_arg= NOT_FIXED_DEC;
    }
  void init_for_tmp_table(Field *org_field, TABLE *new_table) override
  {
    Field::init_for_tmp_table(org_field, new_table);
    not_fixed= true;
  }
  const Type_handler *type_handler() const override
  { return &type_handler_double; }
  enum ha_base_keytype key_type() const override final { return HA_KEYTYPE_DOUBLE; }
  int  store(const char *to,size_t length,CHARSET_INFO *charset) override final;
  int  store(double nr) override final;
  int  store(longlong nr, bool unsigned_val) override final;
  int reset() override final { bzero(ptr,sizeof(double)); return 0; }
  double val_real() override final;
  longlong val_int() override final { return val_int_from_real(false); }
  ulonglong val_uint() override final { return (ulonglong) val_int_from_real(true); }
  String *val_str(String *, String *) override final;
  bool send(Protocol *protocol) override;
  int cmp(const uchar *,const uchar *) const override final;
  void sort_string(uchar *buff, uint length) override final;
  uint32 pack_length() const override final { return sizeof(double); }
  uint row_pack_length() const override final { return pack_length(); }
  ulonglong get_max_int_value() const override final
  {
    /*
      We use the maximum as per IEEE754-2008 standard, 2^53
    */
    return 0x20000000000000ULL;
  }
  Binlog_type_info binlog_type_info() const override final;
};


/* Everything saved in this will disappear. It will always return NULL */

class Field_null :public Field_str {
  static uchar null[1];
public:
  Field_null(uchar *ptr_arg, uint32 len_arg,
	     enum utype unireg_check_arg, const LEX_CSTRING *field_name_arg,
	     const DTCollation &collation)
    :Field_str(ptr_arg, len_arg, null, 1,
	       unireg_check_arg, field_name_arg, collation)
    {}
  const Type_handler *type_handler() const override
  { return &type_handler_null; }
  enum_conv_type rpl_conv_type_from(const Conv_source &source,
                                    const Relay_log_info *rli,
                                    const Conv_param &param) const override;
  Information_schema_character_attributes
    information_schema_character_attributes() const override
  {
    return Information_schema_character_attributes();
  }
  Copy_func *get_copy_func(const Field *from) const override
  {
    return do_field_string;
  }
  int  store(const char *to, size_t length, CHARSET_INFO *cs) override final
  { null[0]=1; return 0; }
  int store(double nr) override final { null[0]=1; return 0; }
  int store(longlong nr, bool unsigned_val) override final { null[0]=1; return 0; }
  int store_decimal(const my_decimal *d) override final { null[0]=1; return 0; }
  int reset() override final { return 0; }
  double val_real() override final { return 0.0;}
  longlong val_int() override final { return 0;}
  bool val_bool() override final { return false; }
  my_decimal *val_decimal(my_decimal *) override final { return 0; }
  String *val_str(String *value,String *value2) override final
  { value2->length(0); return value2;}
  bool is_equal(const Column_definition &new_field) const override final;
  int cmp(const uchar *a, const uchar *b) const override final { return 0;}
  void sort_string(uchar *buff, uint length) override final {}
  uint32 pack_length() const override final { return 0; }
  void sql_type(String &str) const override final;
  uint size_of() const override final { return sizeof *this; }
  uint32 max_display_length() const override final { return 4; }
  void move_field_offset(my_ptrdiff_t ptr_diff) override final {}
  bool can_optimize_keypart_ref(const Item_bool_func *cond,
                                const Item *item) const override final
  {
    return false;
  }
  bool can_optimize_group_min_max(const Item_bool_func *cond,
                                  const Item *const_item) const override final
  {
    return false;
  }
};


class Field_temporal :public Field {
protected:
  Item *get_equal_const_item_datetime(THD *thd, const Context &ctx,
                                      Item *const_item);
  void set_warnings(Sql_condition::enum_warning_level trunc_level,
                    const ErrConv *str, int was_cut, const char *typestr);
  int store_TIME_return_code_with_warnings(int warn, const ErrConv *str,
                                           const char *typestr)
  {
    if (!MYSQL_TIME_WARN_HAVE_WARNINGS(warn) &&
        MYSQL_TIME_WARN_HAVE_NOTES(warn))
    {
      set_warnings(Sql_condition::WARN_LEVEL_NOTE, str,
                   warn | MYSQL_TIME_WARN_TRUNCATED, typestr);
      return 3;
    }
    set_warnings(Sql_condition::WARN_LEVEL_WARN, str, warn, typestr);
    return warn ? 2 : 0;
  }
  int store_invalid_with_warning(const ErrConv *str, int was_cut,
                                 const char *typestr)
  {
    DBUG_ASSERT(was_cut);
    reset();
    Sql_condition::enum_warning_level level= Sql_condition::WARN_LEVEL_WARN;
    if (was_cut & MYSQL_TIME_WARN_ZERO_DATE)
    {
      set_warnings(level, str, MYSQL_TIME_WARN_OUT_OF_RANGE, typestr);
      return 2;
    }
    set_warnings(level, str, MYSQL_TIME_WARN_TRUNCATED, typestr);
    return 1;
  }
  void sql_type_comment(String &str,
                        const Name &name,
                        const Name &comment) const;
  void sql_type_dec_comment(String &str,
                            const Name &name, uint dec,
                            const Name &comment) const;
  void sql_type_opt_dec_comment(String &str,
                                const Name &name, uint dec,
                                const Name &comment) const
  {
    if (dec)
      sql_type_dec_comment(str, name, dec, comment);
    else
      sql_type_comment(str, name, comment);
  }
  static const Name &type_version_mysql56();
public:
  Field_temporal(uchar *ptr_arg,uint32 len_arg, uchar *null_ptr_arg,
                 uchar null_bit_arg, utype unireg_check_arg,
                 const LEX_CSTRING *field_name_arg)
    :Field(ptr_arg, len_arg, null_ptr_arg, null_bit_arg, unireg_check_arg,
               field_name_arg)
    { flags|= BINARY_FLAG; }
  int  store_hex_hybrid(const char *str, size_t length) override
  {
    return store(str, length, &my_charset_bin);
  }
  sql_mode_t can_handle_sql_mode_dependency_on_store() const override;
  Copy_func *get_copy_func(const Field *from) const override;
  int save_in_field(Field *to) override
  {
    MYSQL_TIME ltime;
    // For temporal types no truncation needed. Rounding mode is not important.
    if (get_date(&ltime, TIME_CONV_NONE | TIME_FRAC_NONE))
      return to->reset();
    return to->store_time_dec(&ltime, decimals());
  }
  bool memcpy_field_possible(const Field *from) const override;
  uint32 max_display_length() const override { return field_length; }
  bool str_needs_quotes() const override { return true; }
  CHARSET_INFO *charset() const override
  {
    return DTCollation_numeric::singleton().collation;
  }
  const DTCollation &dtcollation() const override
  {
    return DTCollation_numeric::singleton();
  }
  CHARSET_INFO *sort_charset() const override { return &my_charset_bin; }
  bool binary() const override { return true; }
  bool val_bool() override { return val_real() != 0e0; }
  bool is_equal(const Column_definition &new_field) const override;
  bool eq_def(const Field *field) const override
  {
    return (Field::eq_def(field) && decimals() == field->decimals());
  }
  my_decimal *val_decimal(my_decimal*) override;
  double pos_in_interval(Field *min, Field *max) override
  {
    return pos_in_interval_val_real(min, max);
  }
  bool can_optimize_keypart_ref(const Item_bool_func *cond,
                                const Item *item) const override;
  bool can_optimize_group_min_max(const Item_bool_func *cond,
                                  const Item *const_item) const override;
  bool can_optimize_range(const Item_bool_func *cond,
                                  const Item *item,
                                  bool is_eq_func) const override
  {
    return true;
  }
  SEL_ARG *get_mm_leaf(RANGE_OPT_PARAM *param, KEY_PART *key_part,
                       const Item_bool_func *cond,
                       scalar_comparison_op op, Item *value) override;
};


/**
  Abstract class for:
  - DATE
  - DATETIME
  - DATETIME(1..6)
  - DATETIME(0..6) - MySQL56 version
*/
class Field_temporal_with_date :public Field_temporal {
protected:
  virtual void store_TIME(const MYSQL_TIME *ltime) = 0;
  void store_datetime(const Datetime &dt)
  {
    return store_TIME(dt.get_mysql_time());
  }
  virtual bool get_TIME(MYSQL_TIME *ltime, const uchar *pos,
                        date_mode_t fuzzydate) const = 0;
  bool validate_MMDD(bool not_zero_date, uint month, uint day,
                     date_mode_t fuzzydate) const
  {
    if (!not_zero_date)
      return bool(fuzzydate & TIME_NO_ZERO_DATE);
    if (!month || !day)
      return bool(fuzzydate & TIME_NO_ZERO_IN_DATE);
    return false;
  }
public:
  Field_temporal_with_date(uchar *ptr_arg, uint32 len_arg,
                           uchar *null_ptr_arg, uchar null_bit_arg,
                           utype unireg_check_arg,
                           const LEX_CSTRING *field_name_arg)
    :Field_temporal(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
                    unireg_check_arg, field_name_arg)
    {}
  bool validate_value_in_record(THD *thd, const uchar *record) const;
};


class Field_timestamp :public Field_temporal {
protected:
  int store_TIME_with_warning(THD *, const Datetime *,
                              const ErrConv *, int warn);
  virtual void store_TIMEVAL(const timeval &tv)= 0;
  void store_TIMESTAMP(const Timestamp &ts)
  {
    store_TIMEVAL(ts.tv());
  }
  int zero_time_stored_return_code_with_warning();
public:
  Field_timestamp(uchar *ptr_arg, uint32 len_arg,
                  uchar *null_ptr_arg, uchar null_bit_arg,
		  enum utype unireg_check_arg,
                  const LEX_CSTRING *field_name_arg,
		  TABLE_SHARE *share);
  const Type_handler *type_handler() const override
  { return &type_handler_timestamp; }
  enum_conv_type rpl_conv_type_from(const Conv_source &source,
                                    const Relay_log_info *rli,
                                    const Conv_param &param) const override;
  Copy_func *get_copy_func(const Field *from) const override;
  sql_mode_t conversion_depends_on_sql_mode(THD *, Item *) const override;
  int  store(const char *to,size_t length,CHARSET_INFO *charset) override;
  int  store(double nr) override;
  int  store(longlong nr, bool unsigned_val) override;
  int  store_time_dec(const MYSQL_TIME *ltime, uint dec) override;
  int  store_decimal(const my_decimal *) override;
  int  store_timestamp_dec(const timeval &ts, uint dec) override;
  int  save_in_field(Field *to) override;
  longlong val_int() override;
  String *val_str(String *, String *) override;
  bool zero_pack() const override { return false; }
  /*
    This method is used by storage/perfschema and
    Item_func_now_local::save_in_field().
  */
  void store_TIME(my_time_t ts, ulong sec_part)
  {
    int warn;
    time_round_mode_t mode= Datetime::default_round_mode(get_thd());
    store_TIMESTAMP(Timestamp(ts, sec_part).round(decimals(), mode, &warn));
  }
  bool get_date(MYSQL_TIME *ltime, date_mode_t fuzzydate) override;
  int store_native(const Native &value) override;
  bool validate_value_in_record(THD *thd, const uchar *record) const override;
  Item *get_equal_const_item(THD *thd, const Context &ctx, Item *const_item)
    override
  {
    return get_equal_const_item_datetime(thd, ctx, const_item);
  }
  bool load_data_set_null(THD *thd) override;
  bool load_data_set_no_data(THD *thd, bool fixed_format) override;
};


class Field_timestamp0 :public Field_timestamp
{
  void store_TIMEVAL(const timeval &tv) override
  {
    int4store(ptr, tv.tv_sec);
  }
public:
  Field_timestamp0(uchar *ptr_arg, uint32 len_arg,
                   uchar *null_ptr_arg, uchar null_bit_arg,
		   enum utype unireg_check_arg,
                   const LEX_CSTRING *field_name_arg,
		   TABLE_SHARE *share)
   :Field_timestamp(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
                    unireg_check_arg, field_name_arg, share)
  { }
  enum ha_base_keytype key_type() const override
  { return HA_KEYTYPE_ULONG_INT; }
  void sql_type(String &str) const override
  {
    sql_type_comment(str, Field_timestamp0::type_handler()->name(),
                     Type_handler::version_mariadb53());
  }
  double val_real() override
  {
    return (double) Field_timestamp0::val_int();
  }
  bool send(Protocol *protocol) override;
  int cmp(const uchar *,const uchar *) const override;
  void sort_string(uchar *buff,uint length) override;
  uint32 pack_length() const override { return 4; }
  int set_time() override;
  /* Get TIMESTAMP field value as seconds since begging of Unix Epoch */
  my_time_t get_timestamp(const uchar *pos, ulong *sec_part) const override;
  bool val_native(Native *to) override;
  uchar *pack(uchar *to, const uchar *from, uint) override
  {
    return pack_int32(to, from);
  }
  const uchar *unpack(uchar* to, const uchar *from, const uchar *from_end,
                      uint) override
  {
    return unpack_int32(to, from, from_end);
  }
  uint size_of() const override { return sizeof *this; }
};


/**
  Abstract class for:
  - TIMESTAMP(1..6)
  - TIMESTAMP(0..6) - MySQL56 version
*/
class Field_timestamp_with_dec :public Field_timestamp {
protected:
  decimal_digits_t dec;
public:
  Field_timestamp_with_dec(uchar *ptr_arg,
                           uchar *null_ptr_arg, uchar null_bit_arg,
                           enum utype unireg_check_arg,
                           const LEX_CSTRING *field_name_arg,
                           TABLE_SHARE *share, decimal_digits_t dec_arg) :
  Field_timestamp(ptr_arg,
                  MAX_DATETIME_WIDTH + dec_arg + MY_TEST(dec_arg),
                  null_ptr_arg,
                  null_bit_arg, unireg_check_arg, field_name_arg, share),
  dec(dec_arg)
  {
    DBUG_ASSERT(dec <= TIME_SECOND_PART_DIGITS);
  }
  decimal_digits_t decimals() const override { return dec; }
  enum ha_base_keytype key_type() const override { return HA_KEYTYPE_BINARY; }
  uchar *pack(uchar *to, const uchar *from, uint max_length) override
  { return Field::pack(to, from, max_length); }
  const uchar *unpack(uchar* to, const uchar *from, const uchar *from_end,
                      uint param_data) override
  { return Field::unpack(to, from, from_end, param_data); }
  void make_send_field(Send_field *field) override;
  void sort_string(uchar *to, uint length) override
  {
    DBUG_ASSERT(length == pack_length());
    memcpy(to, ptr, length);
  }
  bool send(Protocol *protocol) override;
  double val_real() override;
  my_decimal* val_decimal(my_decimal*) override;
  int set_time() override;
};


class Field_timestamp_hires :public Field_timestamp_with_dec {
  uint sec_part_bytes(uint dec) const
  {
    return Type_handler_timestamp::sec_part_bytes(dec);
  }
  void store_TIMEVAL(const timeval &tv) override;
public:
  Field_timestamp_hires(uchar *ptr_arg,
                        uchar *null_ptr_arg, uchar null_bit_arg,
                        enum utype unireg_check_arg,
                        const LEX_CSTRING *field_name_arg,
                        TABLE_SHARE *share, decimal_digits_t dec_arg) :
  Field_timestamp_with_dec(ptr_arg, null_ptr_arg, null_bit_arg,
                           unireg_check_arg, field_name_arg, share, dec_arg)
  {
    DBUG_ASSERT(dec);
  }
  void sql_type(String &str) const override
  {
    sql_type_dec_comment(str, Field_timestamp_hires::type_handler()->name(),
                         dec, Type_handler::version_mariadb53());
  }
  bool val_native(Native *to) override;
  my_time_t get_timestamp(const uchar *pos, ulong *sec_part) const override;
  int cmp(const uchar *,const uchar *) const override;
  uint32 pack_length() const override { return 4 + sec_part_bytes(dec); }
  uint size_of() const override { return sizeof *this; }
};


/**
  TIMESTAMP(0..6) - MySQL56 version
*/
class Field_timestampf final :public Field_timestamp_with_dec {
  void store_TIMEVAL(const timeval &tv) override;
public:
  Field_timestampf(uchar *ptr_arg,
                   uchar *null_ptr_arg, uchar null_bit_arg,
                   enum utype unireg_check_arg,
                   const LEX_CSTRING *field_name_arg,
                   TABLE_SHARE *share, decimal_digits_t dec_arg) :
    Field_timestamp_with_dec(ptr_arg, null_ptr_arg, null_bit_arg,
                             unireg_check_arg, field_name_arg, share, dec_arg)
    {}
  const Type_handler *type_handler() const override
  { return &type_handler_timestamp2; }
  enum_field_types binlog_type() const override
  { return MYSQL_TYPE_TIMESTAMP2; }
  void sql_type(String &str) const override
  {
    sql_type_opt_dec_comment(str, Field_timestampf::type_handler()->name(),
                             dec, type_version_mysql56());

  }
  enum_conv_type rpl_conv_type_from(const Conv_source &source,
                                    const Relay_log_info *rli,
                                    const Conv_param &param) const override;
  uint32 pack_length() const override
  {
    return my_timestamp_binary_length(dec);
  }
  uint row_pack_length() const override { return pack_length(); }
  uint pack_length_from_metadata(uint field_metadata) const override
  {
    DBUG_ENTER("Field_timestampf::pack_length_from_metadata");
    uint tmp= my_timestamp_binary_length(field_metadata);
    DBUG_RETURN(tmp);
  }
  int cmp(const uchar *a_ptr,const uchar *b_ptr) const override
  {
    return memcmp(a_ptr, b_ptr, pack_length());
  }
  void set_max() override;
  bool is_max() override;
  my_time_t get_timestamp(const uchar *pos, ulong *sec_part) const override;
  bool val_native(Native *to) override;
  uint size_of() const override { return sizeof *this; }
  Binlog_type_info binlog_type_info() const override;
};


class Field_year final :public Field_tiny {
public:
  Field_year(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
	     uchar null_bit_arg,
	     enum utype unireg_check_arg, const LEX_CSTRING *field_name_arg)
    :Field_tiny(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
		unireg_check_arg, field_name_arg, 1, 1)
    {}
  const Type_handler *type_handler() const override
  {
    return field_length == 2 ? &type_handler_year2 : &type_handler_year;
  }
  enum_conv_type rpl_conv_type_from(const Conv_source &source,
                                    const Relay_log_info *rli,
                                    const Conv_param &param) const override;
  Copy_func *get_copy_func(const Field *from) const override
  {
    if (eq_def(from))
      return get_identical_copy_func();
    switch (from->cmp_type()) {
    case STRING_RESULT:
    {
      const Type_handler *handler= from->type_handler();
      if (handler == &type_handler_enum || handler == &type_handler_set)
        return do_field_int;
      return do_field_string;
    }
    case TIME_RESULT:
      return do_field_date;
    case DECIMAL_RESULT:
      return do_field_decimal;
    case REAL_RESULT:
      return do_field_real;
    case INT_RESULT:
      break;
    case ROW_RESULT:
    default:
      DBUG_ASSERT(0);
      break;
    }
    return do_field_int;
  }
  int  store(const char *to,size_t length,CHARSET_INFO *charset) override;
  int  store(double nr) override;
  int  store(longlong nr, bool unsigned_val) override;
  int  store_time_dec(const MYSQL_TIME *ltime, uint dec) override;
  double val_real() override;
  longlong val_int() override;
  String *val_str(String *, String *) override;
  bool get_date(MYSQL_TIME *ltime, date_mode_t fuzzydate) override;
  bool send(Protocol *protocol) override;
  Information_schema_numeric_attributes
    information_schema_numeric_attributes() const override
  {
    return Information_schema_numeric_attributes();
  }
  uint32 max_display_length() const override { return field_length; }
  void sql_type(String &str) const override;
};


class Field_date_common :public Field_temporal_with_date
{
protected:
  int store_TIME_with_warning(const Datetime *ltime, const ErrConv *str,
                              int was_cut);
public:
  Field_date_common(uchar *ptr_arg, uchar *null_ptr_arg, uchar null_bit_arg,
                    enum utype unireg_check_arg,
                    const LEX_CSTRING *field_name_arg)
    :Field_temporal_with_date(ptr_arg, MAX_DATE_WIDTH,
                              null_ptr_arg, null_bit_arg,
                              unireg_check_arg, field_name_arg)
  {}
  Copy_func *get_copy_func(const Field *from) const override;
  SEL_ARG *get_mm_leaf(RANGE_OPT_PARAM *param, KEY_PART *key_part,
                       const Item_bool_func *cond,
                       scalar_comparison_op op, Item *value) override;
  int  store(const char *to, size_t length, CHARSET_INFO *charset) override;
  int  store(double nr) override;
  int  store(longlong nr, bool unsigned_val) override;
  int  store_time_dec(const MYSQL_TIME *ltime, uint dec) override;
  int  store_decimal(const my_decimal *) override;
};


class Field_date final :public Field_date_common
{
  void store_TIME(const MYSQL_TIME *ltime) override;
  bool get_TIME(MYSQL_TIME *ltime, const uchar *pos, date_mode_t fuzzydate)
    const override;
public:
  Field_date(uchar *ptr_arg, uchar *null_ptr_arg, uchar null_bit_arg,
	     enum utype unireg_check_arg, const LEX_CSTRING *field_name_arg)
    :Field_date_common(ptr_arg, null_ptr_arg, null_bit_arg,
                       unireg_check_arg, field_name_arg) {}
  const Type_handler *type_handler() const override
  { return &type_handler_date; }
  enum ha_base_keytype key_type() const override
  { return HA_KEYTYPE_ULONG_INT; }
  enum_conv_type rpl_conv_type_from(const Conv_source &source,
                                    const Relay_log_info *rli,
                                    const Conv_param &param) const override;
  int reset() override { ptr[0]=ptr[1]=ptr[2]=ptr[3]=0; return 0; }
  bool get_date(MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  { return Field_date::get_TIME(ltime, ptr, fuzzydate); }
  double val_real() override;
  longlong val_int() override;
  String *val_str(String *, String *) override;
  bool send(Protocol *protocol) override;
  int cmp(const uchar *,const uchar *) const override;
  void sort_string(uchar *buff,uint length) override;
  uint32 pack_length() const override { return 4; }
  void sql_type(String &str) const override;
  uchar *pack(uchar* to, const uchar *from, uint) override
  {
    return pack_int32(to, from);
  }
  const uchar *unpack(uchar* to, const uchar *from, const uchar *from_end,
                      uint) override
  {
    return unpack_int32(to, from, from_end);
  }
  uint size_of() const override { return sizeof *this; }
};


class Field_newdate final :public Field_date_common
{
  void store_TIME(const MYSQL_TIME *ltime) override;
  bool get_TIME(MYSQL_TIME *ltime, const uchar *pos, date_mode_t fuzzydate)
    const override;
public:
  Field_newdate(uchar *ptr_arg, uchar *null_ptr_arg, uchar null_bit_arg,
		enum utype unireg_check_arg, const LEX_CSTRING *field_name_arg)
    :Field_date_common(ptr_arg, null_ptr_arg, null_bit_arg,
                       unireg_check_arg, field_name_arg)
    {}
  const Type_handler *type_handler() const override
  { return &type_handler_newdate; }
  enum ha_base_keytype key_type() const override { return HA_KEYTYPE_UINT24; }
  enum_conv_type rpl_conv_type_from(const Conv_source &source,
                                    const Relay_log_info *rli,
                                    const Conv_param &param) const override;
  int reset() override { ptr[0]=ptr[1]=ptr[2]=0; return 0; }
  double val_real() override;
  longlong val_int() override;
  String *val_str(String *, String *) override;
  bool send(Protocol *protocol) override;
  int cmp(const uchar *,const uchar *) const override;
  void sort_string(uchar *buff,uint length) override;
  uint32 pack_length() const override { return 3; }
  void sql_type(String &str) const override;
  bool get_date(MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  { return Field_newdate::get_TIME(ltime, ptr, fuzzydate); }
  longlong val_datetime_packed(THD *thd) override;
  uint size_of() const override { return sizeof *this; }
  Item *get_equal_const_item(THD *thd, const Context &ctx, Item *const_item)
    override;
};


class Field_time :public Field_temporal {
  /*
    when this Field_time instance is used for storing values for index lookups
    (see class store_key, Field::new_key_field(), etc), the following
    might be set to TO_DAYS(CURDATE()). See also Field_time::store_time_dec()
  */
  long curdays;
protected:
  virtual void store_TIME(const MYSQL_TIME *ltime)= 0;
  void store_TIME(const Time &t) { return store_TIME(t.get_mysql_time()); }
  int store_TIME_with_warning(const Time *ltime, const ErrConv *str, int warn);
  bool check_zero_in_date_with_warn(date_mode_t fuzzydate);
  static void do_field_time(Copy_field *copy);
public:
  Field_time(uchar *ptr_arg, uint length_arg, uchar *null_ptr_arg,
             uchar null_bit_arg, enum utype unireg_check_arg,
             const LEX_CSTRING *field_name_arg)
    :Field_temporal(ptr_arg, length_arg, null_ptr_arg, null_bit_arg,
                    unireg_check_arg, field_name_arg), curdays(0)
    {}
  bool can_be_substituted_to_equal_item(const Context &ctx,
                                        const Item_equal *item_equal) override;
  const Type_handler *type_handler() const override
  { return &type_handler_time; }
  enum_conv_type rpl_conv_type_from(const Conv_source &source,
                                    const Relay_log_info *rli,
                                    const Conv_param &param) const override;
  Copy_func *get_copy_func(const Field *from) const override
  {
    return from->cmp_type() == REAL_RESULT ? do_field_string : // MDEV-9344
           from->type() == MYSQL_TYPE_YEAR ? do_field_int :
           from->type() == MYSQL_TYPE_BIT  ? do_field_int :
           eq_def(from)                    ? get_identical_copy_func() :
                                             do_field_time;
  }
  bool memcpy_field_possible(const Field *from) const override
  {
    return real_type() == from->real_type() &&
           decimals() == from->decimals();
  }
  sql_mode_t conversion_depends_on_sql_mode(THD *, Item *) const override;
  int store_native(const Native &value) override;
  bool val_native(Native *to) override;
  int store_time_dec(const MYSQL_TIME *ltime, uint dec) override;
  int store(const char *to,size_t length,CHARSET_INFO *charset) override;
  int store(double nr) override;
  int store(longlong nr, bool unsigned_val) override;
  int  store_decimal(const my_decimal *) override;
  String *val_str(String *, String *) override;
  bool send(Protocol *protocol) override;
  void set_curdays(THD *thd);
  Field *new_key_field(MEM_ROOT *root, TABLE *new_table,
                       uchar *new_ptr, uint32 length,
                       uchar *new_null_ptr, uint new_null_bit) override;
  Item *get_equal_const_item(THD *thd, const Context &ctx, Item *const_item)
    override;
};


class Field_time0 final :public Field_time
{
protected:
  void store_TIME(const MYSQL_TIME *ltime) override;
public:
  Field_time0(uchar *ptr_arg, uint length_arg, uchar *null_ptr_arg,
             uchar null_bit_arg, enum utype unireg_check_arg,
             const LEX_CSTRING *field_name_arg)
    :Field_time(ptr_arg, length_arg, null_ptr_arg, null_bit_arg,
                unireg_check_arg, field_name_arg)
  { }
  enum ha_base_keytype key_type() const override { return HA_KEYTYPE_INT24; }
  void sql_type(String &str) const override
  {
    sql_type_comment(str, Field_time0::type_handler()->name(),
                     Type_handler::version_mariadb53());
  }
  double val_real() override;
  longlong val_int() override;
  bool get_date(MYSQL_TIME *ltime, date_mode_t fuzzydate) override;
  int cmp(const uchar *,const uchar *) const override;
  void sort_string(uchar *buff,uint length) override;
  uint32 pack_length() const override { return 3; }
  uint size_of() const override { return sizeof *this; }
};


/**
  Abstract class for:
  - TIME(1..6)
  - TIME(0..6) - MySQL56 version
*/
class Field_time_with_dec :public Field_time {
protected:
  decimal_digits_t dec;
public:
  Field_time_with_dec(uchar *ptr_arg, uchar *null_ptr_arg, uchar null_bit_arg,
                      enum utype unireg_check_arg,
                      const LEX_CSTRING *field_name_arg,
                      decimal_digits_t dec_arg)
    :Field_time(ptr_arg, MIN_TIME_WIDTH + dec_arg + MY_TEST(dec_arg),
                null_ptr_arg, null_bit_arg, unireg_check_arg, field_name_arg),
     dec(dec_arg)
  {
    DBUG_ASSERT(dec <= TIME_SECOND_PART_DIGITS);
  }
  decimal_digits_t decimals() const override { return dec; }
  enum ha_base_keytype key_type() const override { return HA_KEYTYPE_BINARY; }
  longlong val_int() override;
  double val_real() override;
  void make_send_field(Send_field *) override;
};


/**
  TIME(1..6)
*/
class Field_time_hires final :public Field_time_with_dec {
  longlong zero_point;
  void store_TIME(const MYSQL_TIME *) override;
public:
  Field_time_hires(uchar *ptr_arg, uchar *null_ptr_arg, uchar null_bit_arg,
                   enum utype unireg_check_arg,
                   const LEX_CSTRING *field_name_arg,
                   decimal_digits_t dec_arg)
    :Field_time_with_dec(ptr_arg, null_ptr_arg,
                         null_bit_arg, unireg_check_arg, field_name_arg,
                         dec_arg)
  {
    DBUG_ASSERT(dec);
    zero_point= sec_part_shift(
                   ((TIME_MAX_VALUE_SECONDS+1LL)*TIME_SECOND_PART_FACTOR), dec);
  }
  void sql_type(String &str) const override
  {
    sql_type_dec_comment(str, Field_time_hires::type_handler()->name(),
                         dec, Type_handler::version_mariadb53());
  }
  int reset() override;
  bool get_date(MYSQL_TIME *ltime, date_mode_t fuzzydate) override;
  int cmp(const uchar *,const uchar *) const override;
  void sort_string(uchar *buff,uint length) override;
  uint32 pack_length() const override
  { return Type_handler_time::hires_bytes(dec); }
  uint size_of() const override { return sizeof *this; }
};


/**
  TIME(0..6) - MySQL56 version
*/
class Field_timef final :public Field_time_with_dec {
  void store_TIME(const MYSQL_TIME *ltime) override;
public:
  Field_timef(uchar *ptr_arg, uchar *null_ptr_arg, uchar null_bit_arg,
              enum utype unireg_check_arg, const LEX_CSTRING *field_name_arg,
              decimal_digits_t dec_arg)
    :Field_time_with_dec(ptr_arg, null_ptr_arg,
                         null_bit_arg, unireg_check_arg, field_name_arg,
                         dec_arg)
  {
    DBUG_ASSERT(dec <= TIME_SECOND_PART_DIGITS);
  }
  const Type_handler *type_handler() const override
  { return &type_handler_time2; }
  enum_field_types binlog_type() const override { return MYSQL_TYPE_TIME2; }
  void sql_type(String &str) const override
  {
    sql_type_opt_dec_comment(str, Field_timef::type_handler()->name(),
                             dec, type_version_mysql56());
  }
  enum_conv_type rpl_conv_type_from(const Conv_source &source,
                                    const Relay_log_info *rli,
                                    const Conv_param &param) const override;
  uint32 pack_length() const override
  {
    return my_time_binary_length(dec);
  }
  uint row_pack_length() const override { return pack_length(); }
  uint pack_length_from_metadata(uint field_metadata) const override
  {
    DBUG_ENTER("Field_timef::pack_length_from_metadata");
    uint tmp= my_time_binary_length(field_metadata);
    DBUG_RETURN(tmp);
  }
  void sort_string(uchar *to, uint length) override
  {
    DBUG_ASSERT(length == Field_timef::pack_length());
    memcpy(to, ptr, length);
  }
  int cmp(const uchar *a_ptr, const uchar *b_ptr) const override
  {
    return memcmp(a_ptr, b_ptr, pack_length());
  }
  int reset() override;
  bool get_date(MYSQL_TIME *ltime, date_mode_t fuzzydate) override;
  longlong val_time_packed(THD *thd) override;
  int store_native(const Native &value) override;
  bool val_native(Native *to) override;
  uint size_of() const override { return sizeof *this; }
  Binlog_type_info binlog_type_info() const override;
};


class Field_datetime :public Field_temporal_with_date {
protected:
  int store_TIME_with_warning(const Datetime *ltime, const ErrConv *str,
                              int was_cut);
public:
  Field_datetime(uchar *ptr_arg, uint length_arg, uchar *null_ptr_arg,
                 uchar null_bit_arg, enum utype unireg_check_arg,
                 const LEX_CSTRING *field_name_arg)
    :Field_temporal_with_date(ptr_arg, length_arg, null_ptr_arg, null_bit_arg,
                              unireg_check_arg, field_name_arg)
    {
      if (unireg_check == TIMESTAMP_UN_FIELD ||
          unireg_check == TIMESTAMP_DNUN_FIELD)
        flags|= ON_UPDATE_NOW_FLAG;
    }
  const Type_handler *type_handler() const override
  { return &type_handler_datetime; }
  sql_mode_t conversion_depends_on_sql_mode(THD *, Item *) const override;
  enum_conv_type rpl_conv_type_from(const Conv_source &source,
                                    const Relay_log_info *rli,
                                    const Conv_param &param) const override;
  int  store(const char *to, size_t length, CHARSET_INFO *charset) override;
  int  store(double nr) override;
  int  store(longlong nr, bool unsigned_val) override;
  int  store_time_dec(const MYSQL_TIME *ltime, uint dec) override;
  int  store_decimal(const my_decimal *) override;
  int set_time() override;
  Item *get_equal_const_item(THD *thd, const Context &ctx, Item *const_item)
    override
  {
    return get_equal_const_item_datetime(thd, ctx, const_item);
  }
};


/*
  Stored as a 8 byte unsigned int. Should sometimes be change to a 6 byte
*/

class Field_datetime0 final :public Field_datetime
{
  void store_TIME(const MYSQL_TIME *ltime) override;
  bool get_TIME(MYSQL_TIME *ltime, const uchar *pos, date_mode_t fuzzydate)
    const override;
public:
  Field_datetime0(uchar *ptr_arg, uint length_arg, uchar *null_ptr_arg,
                 uchar null_bit_arg, enum utype unireg_check_arg,
                 const LEX_CSTRING *field_name_arg)
    :Field_datetime(ptr_arg, length_arg, null_ptr_arg, null_bit_arg,
                    unireg_check_arg, field_name_arg)
  {}
  enum ha_base_keytype key_type() const override
  { return HA_KEYTYPE_ULONGLONG; }
  void sql_type(String &str) const override
  {
    sql_type_comment(str, Field_datetime0::type_handler()->name(),
                     Type_handler::version_mariadb53());
  }
  double val_real() override
  {
    return (double) Field_datetime0::val_int();
  }
  longlong val_int() override;
  String *val_str(String *, String *) override;
  bool send(Protocol *protocol) override;
  int cmp(const uchar *,const uchar *) const override;
  void sort_string(uchar *buff,uint length) override;
  uint32 pack_length() const override { return 8; }
  bool get_date(MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  { return Field_datetime0::get_TIME(ltime, ptr, fuzzydate); }
  uchar *pack(uchar* to, const uchar *from, uint) override
  {
    return pack_int64(to, from);
  }
  const uchar *unpack(uchar* to, const uchar *from, const uchar *from_end,
		      uint) override
  {
    return unpack_int64(to, from, from_end);
  }
  uint size_of() const override { return sizeof *this; }
};


/**
  Abstract class for:
  - DATETIME(1..6)
  - DATETIME(0..6) - MySQL56 version
*/
class Field_datetime_with_dec :public Field_datetime {
protected:
  decimal_digits_t dec;
public:
  Field_datetime_with_dec(uchar *ptr_arg, uchar *null_ptr_arg,
                          uchar null_bit_arg, enum utype unireg_check_arg,
                          const LEX_CSTRING *field_name_arg, decimal_digits_t dec_arg)
    :Field_datetime(ptr_arg, MAX_DATETIME_WIDTH + dec_arg + MY_TEST(dec_arg),
                    null_ptr_arg, null_bit_arg, unireg_check_arg,
                    field_name_arg), dec(dec_arg)
  {
    DBUG_ASSERT(dec <= TIME_SECOND_PART_DIGITS);
  }
  decimal_digits_t decimals() const override final { return dec; }
  enum ha_base_keytype key_type() const override final { return HA_KEYTYPE_BINARY; }
  void make_send_field(Send_field *field) override final;
  bool send(Protocol *protocol) override final;
  uchar *pack(uchar *to, const uchar *from, uint max_length) override final
  { return Field::pack(to, from, max_length); }
  const uchar *unpack(uchar* to, const uchar *from, const uchar *from_end,
                      uint param_data) override final
  { return Field::unpack(to, from, from_end, param_data); }
  void sort_string(uchar *to, uint length) override final
  {
    DBUG_ASSERT(length == pack_length());
    memcpy(to, ptr, length);
  }
  double val_real() override final;
  longlong val_int() override final;
  String *val_str(String *, String *) override final;
};


/**
  DATETIME(1..6)
*/
class Field_datetime_hires final :public Field_datetime_with_dec {
  void store_TIME(const MYSQL_TIME *ltime) override;
  bool get_TIME(MYSQL_TIME *ltime, const uchar *pos, date_mode_t fuzzydate)
    const override;
public:
  Field_datetime_hires(uchar *ptr_arg, uchar *null_ptr_arg,
                       uchar null_bit_arg, enum utype unireg_check_arg,
                       const LEX_CSTRING *field_name_arg, decimal_digits_t dec_arg)
    :Field_datetime_with_dec(ptr_arg, null_ptr_arg, null_bit_arg,
                             unireg_check_arg, field_name_arg, dec_arg)
  {
    DBUG_ASSERT(dec);
  }
  void sql_type(String &str) const override
  {
    sql_type_dec_comment(str, Field_datetime_hires::type_handler()->name(),
                         dec, Type_handler::version_mariadb53());
  }
  int cmp(const uchar *,const uchar *) const override;
  uint32 pack_length() const override
  { return Type_handler_datetime::hires_bytes(dec); }
  bool get_date(MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  { return Field_datetime_hires::get_TIME(ltime, ptr, fuzzydate); }
  uint size_of() const override { return sizeof *this; }
};


/**
  DATETIME(0..6) - MySQL56 version
*/

class Field_datetimef final :public Field_datetime_with_dec {
  void store_TIME(const MYSQL_TIME *ltime) override;
  bool get_TIME(MYSQL_TIME *ltime, const uchar *pos, date_mode_t fuzzydate)
    const override;
public:
  Field_datetimef(uchar *ptr_arg, uchar *null_ptr_arg,
                  uchar null_bit_arg, enum utype unireg_check_arg,
                  const LEX_CSTRING *field_name_arg,  decimal_digits_t dec_arg)
    :Field_datetime_with_dec(ptr_arg, null_ptr_arg, null_bit_arg,
                             unireg_check_arg, field_name_arg, dec_arg)
  {}
  const Type_handler *type_handler() const override
  { return &type_handler_datetime2; }
  enum_field_types binlog_type() const override
  { return MYSQL_TYPE_DATETIME2; }
  void sql_type(String &str) const override
  {
    sql_type_opt_dec_comment(str, Field_datetimef::type_handler()->name(),
                             dec, type_version_mysql56());
  }
  enum_conv_type rpl_conv_type_from(const Conv_source &source,
                                    const Relay_log_info *rli,
                                    const Conv_param &param) const override;
  uint32 pack_length() const override
  {
    return my_datetime_binary_length(dec);
  }
  uint row_pack_length() const override { return pack_length(); }
  uint pack_length_from_metadata(uint field_metadata) const override
  {
    DBUG_ENTER("Field_datetimef::pack_length_from_metadata");
    uint tmp= my_datetime_binary_length(field_metadata);
    DBUG_RETURN(tmp);
  }
  int cmp(const uchar *a_ptr, const uchar *b_ptr) const override
  {
    return memcmp(a_ptr, b_ptr, pack_length());
  }
  int reset() override;
  bool get_date(MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  { return Field_datetimef::get_TIME(ltime, ptr, fuzzydate); }
  longlong val_datetime_packed(THD *thd) override;
  uint size_of() const override { return sizeof *this; }
  Binlog_type_info binlog_type_info() const override;
};


static inline Field_timestamp *
new_Field_timestamp(MEM_ROOT *root,uchar *ptr, uchar *null_ptr, uchar null_bit,
                    enum Field::utype unireg_check,
                    const LEX_CSTRING *field_name,
                    TABLE_SHARE *share, decimal_digits_t dec)
{
  if (dec==0)
    return new (root)
      Field_timestamp0(ptr, MAX_DATETIME_WIDTH, null_ptr,
                       null_bit, unireg_check, field_name, share);
  if (dec >= FLOATING_POINT_DECIMALS)
    dec= MAX_DATETIME_PRECISION;
  return new (root)
    Field_timestamp_hires(ptr, null_ptr, null_bit, unireg_check,
                          field_name, share, dec);
}

static inline Field_time *
new_Field_time(MEM_ROOT *root, uchar *ptr, uchar *null_ptr, uchar null_bit,
               enum Field::utype unireg_check, const LEX_CSTRING *field_name,
               decimal_digits_t dec)
{
  if (dec == 0)
    return new (root)
      Field_time0(ptr, MIN_TIME_WIDTH, null_ptr, null_bit, unireg_check,
                  field_name);
  if (dec >= FLOATING_POINT_DECIMALS)
    dec= MAX_DATETIME_PRECISION;
  return new (root)
    Field_time_hires(ptr, null_ptr, null_bit, unireg_check, field_name, dec);
}

static inline Field_datetime *
new_Field_datetime(MEM_ROOT *root, uchar *ptr, uchar *null_ptr, uchar null_bit,
                   enum Field::utype unireg_check,
                   const LEX_CSTRING *field_name, decimal_digits_t dec)
{
  if (dec == 0)
    return new (root)
      Field_datetime0(ptr, MAX_DATETIME_WIDTH, null_ptr, null_bit,
                      unireg_check, field_name);
  if (dec >= FLOATING_POINT_DECIMALS)
    dec= MAX_DATETIME_PRECISION;
  return new (root)
    Field_datetime_hires(ptr, null_ptr, null_bit,
                         unireg_check, field_name, dec);
}

class Field_string final :public Field_longstr {
  class Warn_filter_string: public Warn_filter
  {
  public:
    Warn_filter_string(const THD *thd, const Field_string *field);
  };
  bool is_var_string() const
  {
    return can_alter_field_type &&
           orig_table &&
           (orig_table->s->db_create_options & HA_OPTION_PACK_RECORD) &&
           field_length >= 4 &&
           orig_table->s->frm_version < FRM_VER_TRUE_VARCHAR;
  }
public:
  bool can_alter_field_type;
  Field_string(uchar *ptr_arg, uint32 len_arg,uchar *null_ptr_arg,
	       uchar null_bit_arg,
	       enum utype unireg_check_arg, const LEX_CSTRING *field_name_arg,
	       const DTCollation &collation)
    :Field_longstr(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
                   unireg_check_arg, field_name_arg, collation),
     can_alter_field_type(1) {};
  Field_string(uint32 len_arg,bool maybe_null_arg,
               const LEX_CSTRING *field_name_arg,
               const DTCollation &collation)
    :Field_longstr((uchar*) 0, len_arg, maybe_null_arg ? (uchar*) "": 0, 0,
                   NONE, field_name_arg, collation),
     can_alter_field_type(1) {};

  const Type_handler *type_handler() const override;
  enum ha_base_keytype key_type() const override
    { return binary() ? HA_KEYTYPE_BINARY : HA_KEYTYPE_TEXT; }
  en_fieldtype tmp_engine_column_type(bool use_packed_rows) const override;
  bool zero_pack() const override { return false; }
  Copy_func *get_copy_func(const Field *from) const override;
  int reset() override
  {
    charset()->fill((char*) ptr, field_length, (has_charset() ? ' ' : 0));
    return 0;
  }
  int store(const char *to,size_t length,CHARSET_INFO *charset) override;
  using Field_str::store;
  double val_real() override;
  longlong val_int() override;
  String *val_str(String *, String *) override;
  my_decimal *val_decimal(my_decimal *) override;
  int cmp(const uchar *,const uchar *) const override;
  void sort_string(uchar *buff,uint length) override;
  void update_data_type_statistics(Data_type_statistics *st) const override
  {
    st->m_fixed_string_count++;
    st->m_fixed_string_total_length+= pack_length();
  }
  void sql_type(String &str) const override;
  void sql_rpl_type(String*) const override;
  bool is_equal(const Column_definition &new_field) const override;
  bool can_be_converted_by_engine(const Column_definition &new_type) const
    override
  {
    return table->file->can_convert_string(this, new_type);
  }
  uchar *pack(uchar *to, const uchar *from, uint max_length) override;
  const uchar *unpack(uchar* to, const uchar *from, const uchar *from_end,
                      uint param_data) override;
  uint pack_length_from_metadata(uint field_metadata) const override
  {
    DBUG_PRINT("debug", ("field_metadata: 0x%04x", field_metadata));
    if (field_metadata == 0)
      return row_pack_length();
    return (((field_metadata >> 4) & 0x300) ^ 0x300) + (field_metadata & 0x00ff);
  }
  bool compatible_field_size(uint field_metadata, const Relay_log_info *rli,
                             uint16 mflags, int *order_var) const override;
  uint row_pack_length() const override { return field_length; }
  int pack_cmp(const uchar *a,const uchar *b,uint key_length,
               bool insert_or_update);
  int pack_cmp(const uchar *b,uint key_length,bool insert_or_update);
  uint packed_col_length(const uchar *to, uint length) override;
  uint max_packed_col_length(uint max_length) override;
  uint size_of() const override { return sizeof *this; }
  bool has_charset() const override { return charset() != &my_charset_bin; }
  Field *make_new_field(MEM_ROOT *root, TABLE *new_table, bool keep_type)
    override;
  uint get_key_image(uchar *buff, uint length,
                     const uchar *ptr_arg, imagetype type) const override;
  sql_mode_t value_depends_on_sql_mode() const override;
  sql_mode_t can_handle_sql_mode_dependency_on_store() const override;
  void print_key_value(String *out, uint32 length) override;
  Binlog_type_info binlog_type_info() const override;
};


class Field_varstring :public Field_longstr {
public:
  const uchar *get_data() const
  {
    return get_data(ptr);
  }
  const uchar *get_data(const uchar *ptr_arg) const
  {
    return ptr_arg + length_bytes;
  }
  uint get_length() const
  {
    return get_length(ptr);
  }
  uint get_length(const uchar *ptr_arg) const
  {
    return length_bytes == 1 ? (uint) *ptr_arg : uint2korr(ptr_arg);
  }
protected:
  void store_length(uint32 number)
  {
    if (length_bytes == 1)
      *ptr= (uchar) number;
    else
      int2store(ptr, number);
  }
  virtual void val_str_from_ptr(String *val, const uchar *ptr) const;
public:
  /*
    The maximum space available in a Field_varstring, in bytes. See
    length_bytes.
  */
  static const uint MAX_SIZE;
  /* Store number of bytes used to store length (1 or 2) */
  uint32 length_bytes;
  Field_varstring(uchar *ptr_arg,
                  uint32 len_arg, uint length_bytes_arg,
                  uchar *null_ptr_arg, uchar null_bit_arg,
		  enum utype unireg_check_arg, const LEX_CSTRING *field_name_arg,
		  TABLE_SHARE *share, const DTCollation &collation)
    :Field_longstr(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
                   unireg_check_arg, field_name_arg, collation),
     length_bytes(length_bytes_arg)
  {
    share->varchar_fields++;
  }
  Field_varstring(uint32 len_arg,bool maybe_null_arg,
                  const LEX_CSTRING *field_name_arg,
                  TABLE_SHARE *share, const DTCollation &collation)
    :Field_longstr((uchar*) 0,len_arg, maybe_null_arg ? (uchar*) "": 0, 0,
                   NONE, field_name_arg, collation),
     length_bytes(len_arg < 256 ? 1 :2)
  {
    share->varchar_fields++;
  }

  const Type_handler *type_handler() const override;
  en_fieldtype tmp_engine_column_type(bool use_packed_rows) const override
  {
    return FIELD_VARCHAR;
  }
  enum ha_base_keytype key_type() const override;
  uint16 key_part_flag() const override { return HA_VAR_LENGTH_PART; }
  uint16 key_part_length_bytes() const override { return HA_KEY_BLOB_LENGTH; }
  uint row_pack_length() const override { return field_length; }
  bool zero_pack() const override { return false; }
  int  reset() override { bzero(ptr,field_length+length_bytes); return 0; }
  uint32 pack_length() const override
  { return (uint32) field_length+length_bytes; }
  uint32 key_length() const override { return (uint32) field_length; }
  uint32 sort_length() const override
  {
    return (uint32) field_length + sort_suffix_length();
  }
  uint32 sort_suffix_length() const override
  {
    return (field_charset() == &my_charset_bin ? length_bytes : 0);
  }
  Copy_func *get_copy_func(const Field *from) const override;
  bool memcpy_field_possible(const Field *from) const override;
  void update_data_type_statistics(Data_type_statistics *st) const override
  {
    st->m_variable_string_count++;
    st->m_variable_string_total_length+= pack_length();
  }
  int  store(const char *to,size_t length,CHARSET_INFO *charset) override;
  using Field_str::store;
#ifdef HAVE_MEM_CHECK
  void mark_unused_memory_as_defined() override;
#endif
  double val_real() override;
  longlong val_int() override;
  String *val_str(String *, String *) override;
  my_decimal *val_decimal(my_decimal *) override;
  bool send(Protocol *protocol) override;
  int cmp(const uchar *a,const uchar *b) const override;
  int cmp_prefix(const uchar *a, const uchar *b, size_t prefix_len) const
    override;
  void sort_string(uchar *buff,uint length) override;
  uint get_key_image(uchar *buff, uint length,
                     const uchar *ptr_arg, imagetype type) const override;
  void set_key_image(const uchar *buff,uint length) override;
  void sql_type(String &str) const override;
  void sql_rpl_type(String*) const override;
  uchar *pack(uchar *to, const uchar *from, uint max_length) override;
  const uchar *unpack(uchar* to, const uchar *from, const uchar *from_end,
                      uint param_data) override;
  int cmp_binary(const uchar *a,const uchar *b, uint32 max_length=~0U) const
    override;
  int key_cmp(const uchar *,const uchar*) const override;
  int key_cmp(const uchar *str, uint length) const override;
  uint packed_col_length(const uchar *to, uint length) override;
  uint max_packed_col_length(uint max_length) override;
  uint32 data_length() override;
  uint size_of() const override { return sizeof *this; }
  bool has_charset() const override
  { return charset() == &my_charset_bin ? FALSE : TRUE; }
  Field *make_new_field(MEM_ROOT *root, TABLE *new_table, bool keep_type)
    override;
  Field *new_key_field(MEM_ROOT *root, TABLE *new_table,
                       uchar *new_ptr, uint32 length,
                       uchar *new_null_ptr, uint new_null_bit) override;
  bool is_equal(const Column_definition &new_field) const override;
  bool can_be_converted_by_engine(const Column_definition &new_type) const
    override
  {
    return table->file->can_convert_varstring(this, new_type);
  }
  void hash(ulong *nr, ulong *nr2) override;
  uint length_size() const override { return length_bytes; }
  void print_key_value(String *out, uint32 length) override;
  Binlog_type_info binlog_type_info() const override;
};


class Field_varstring_compressed final :public Field_varstring {
public:
  Field_varstring_compressed(uchar *ptr_arg,
                             uint32 len_arg, uint length_bytes_arg,
                             uchar *null_ptr_arg, uchar null_bit_arg,
                             enum utype unireg_check_arg,
                             const LEX_CSTRING *field_name_arg,
                             TABLE_SHARE *share, const DTCollation &collation,
                             Compression_method *compression_method_arg):
    Field_varstring(ptr_arg, len_arg, length_bytes_arg, null_ptr_arg,
                    null_bit_arg, unireg_check_arg, field_name_arg,
                    share, collation),
    compression_method_ptr(compression_method_arg) { DBUG_ASSERT(len_arg > 0); }
  Compression_method *compression_method() const override
  { return compression_method_ptr; }
private:
  Compression_method *compression_method_ptr;
  void val_str_from_ptr(String *val, const uchar *ptr) const override;
  int store(const char *to, size_t length, CHARSET_INFO *charset) override;
  using Field_str::store;
  String *val_str(String *, String *) override;
  double val_real() override;
  longlong val_int() override;
  uint size_of() const override { return sizeof *this; }
  /*
    We use the default Field::send() implementation,
    because the derived optimized version (from Field_longstr)
    is not suitable for compressed fields.
  */
  bool send(Protocol *protocol) override
  {
    return Field::send(protocol);
  }
  enum_field_types binlog_type() const override
  { return MYSQL_TYPE_VARCHAR_COMPRESSED; }
  void sql_type(String &str) const override
  {
    Field_varstring::sql_type(str);
    str.append(STRING_WITH_LEN(" /*!100301 COMPRESSED*/"));
  }
  uint32 max_display_length() const override { return field_length - 1; }
  uint32 character_octet_length() const override { return field_length - 1; }
  uint32 char_length() const override
  {
    return (field_length - 1) / mbmaxlen();
  }
  int cmp(const uchar *a_ptr, const uchar *b_ptr) const override;

  /*
    Compressed fields can't have keys as two rows may have different
    compression methods or compression levels.
  */

  int key_cmp(const uchar *str, uint length) const override
  { DBUG_ASSERT(0); return 0; }
  using Field_varstring::key_cmp;
  Binlog_type_info binlog_type_info() const override;
};


static inline uint8 number_storage_requirement(uint32 n)
{
  return n < 256 ? 1 : n < 65536 ? 2 : n < 16777216 ? 3 : 4;
}


static inline void store_bigendian(ulonglong num, uchar *to, uint bytes)
{
  switch(bytes) {
  case 1: mi_int1store(to, num); break;
  case 2: mi_int2store(to, num); break;
  case 3: mi_int3store(to, num); break;
  case 4: mi_int4store(to, num); break;
  case 5: mi_int5store(to, num); break;
  case 6: mi_int6store(to, num); break;
  case 7: mi_int7store(to, num); break;
  case 8: mi_int8store(to, num); break;
  default: DBUG_ASSERT(0);
  }
}


static inline longlong read_bigendian(const uchar *from, uint bytes)
{
  switch(bytes) {
  case 1: return mi_uint1korr(from);
  case 2: return mi_uint2korr(from);
  case 3: return mi_uint3korr(from);
  case 4: return mi_uint4korr(from);
  case 5: return mi_uint5korr(from);
  case 6: return mi_uint6korr(from);
  case 7: return mi_uint7korr(from);
  case 8: return mi_sint8korr(from);
  default: DBUG_ASSERT(0); return 0;
  }
}

static inline void store_lowendian(ulonglong num, uchar *to, uint bytes)
{
  switch(bytes) {
  case 1: *to= (uchar)num;    break;
  case 2: int2store(to, num); break;
  case 3: int3store(to, num); break;
  case 4: int4store(to, num); break;
  case 8: int8store(to, num); break;
  default: DBUG_ASSERT(0);
  }
}

static inline longlong read_lowendian(const uchar *from, uint bytes)
{
  switch(bytes) {
  case 1: return from[0];
  case 2: return uint2korr(from);
  case 3: return uint3korr(from);
  case 4: return uint4korr(from);
  case 8: return sint8korr(from);
  default: DBUG_ASSERT(0); return 0;
  }
}


extern LEX_CSTRING temp_lex_str;

class Field_blob :public Field_longstr {
protected:
  /**
    The number of bytes used to represent the length of the blob.
  */
  uint packlength;
  
  /**
    The 'value'-object is a cache fronting the storage engine.
  */
  String value;
  /**
     Cache for blob values when reading a row with a virtual blob
     field. This is needed to not destroy the old cached value when
     updating the blob with a new value when creating the new row.
  */
  String read_value;

  static void do_copy_blob(Copy_field *copy);
  static void do_conv_blob(Copy_field *copy);
  uint get_key_image_itRAW(const uchar *ptr_arg, uchar *buff, uint length) const;
public:
  Field_blob(uchar *ptr_arg, uchar *null_ptr_arg, uchar null_bit_arg,
	     enum utype unireg_check_arg, const LEX_CSTRING *field_name_arg,
	     TABLE_SHARE *share, uint blob_pack_length,
	     const DTCollation &collation);
  Field_blob(uint32 len_arg,bool maybe_null_arg, const LEX_CSTRING *field_name_arg,
             const DTCollation &collation)
    :Field_longstr((uchar*) 0, len_arg, maybe_null_arg ? (uchar*) "": 0, 0,
                   NONE, field_name_arg, collation),
    packlength(4)
  {
    flags|= BLOB_FLAG;
  }
  Field_blob(uint32 len_arg,bool maybe_null_arg,
             const LEX_CSTRING *field_name_arg,
             const DTCollation &collation, bool set_packlength)
    :Field_longstr((uchar*) 0,len_arg, maybe_null_arg ? (uchar*) "": 0, 0,
                   NONE, field_name_arg, collation)
  {
    flags|= BLOB_FLAG;
    packlength= set_packlength ? number_storage_requirement(len_arg) : 4;
  }
  Field_blob(uint32 packlength_arg)
    :Field_longstr((uchar*) 0, 0, (uchar*) "", 0, NONE, &temp_lex_str,
                   system_charset_info),
    packlength(packlength_arg) {}
  const Type_handler *type_handler() const override;
  /* Note that the default copy constructor is used, in clone() */
  enum_field_types type() const override
  {
    /*
      We cannot return type_handler()->field_type() here.
      Some pieces of the code (e.g. in engines) rely on the fact
      that Field::type(), Field::real_type() and Item_field::field_type()
      return MYSQL_TYPE_BLOB for all blob variants.
      We should eventually fix all such code pieces to expect
      all BLOB type codes.
    */
    return MYSQL_TYPE_BLOB;
  }
  enum_field_types real_type() const override
  {
    return MYSQL_TYPE_BLOB;
  }
  enum ha_base_keytype key_type() const override
    { return binary() ? HA_KEYTYPE_VARBINARY2 : HA_KEYTYPE_VARTEXT2; }
  uint16 key_part_flag() const override { return HA_BLOB_PART; }
  uint16 key_part_length_bytes() const override { return HA_KEY_BLOB_LENGTH; }
  en_fieldtype tmp_engine_column_type(bool use_packed_rows) const override
  {
    return FIELD_BLOB;
  }
  Type_numeric_attributes type_numeric_attributes() const override
  {
    return Type_numeric_attributes(Field_blob::max_display_length(),
                                   decimals(), is_unsigned());
  }
  Information_schema_character_attributes
    information_schema_character_attributes() const override
  {
    uint32 octets= Field_blob::character_octet_length();
    uint32 chars= octets / field_charset()->mbminlen;
    return Information_schema_character_attributes(octets, chars);
  }
  void update_data_type_statistics(Data_type_statistics *st) const override
  {
    st->m_blob_count++;
  }
  void make_send_field(Send_field *) override;
  Copy_func *get_copy_func(const Field *from) const override
  {
    /*
    TODO: MDEV-9331
    if (from->type() == MYSQL_TYPE_BIT)
      return do_field_int;
    */
    if (!(from->flags & BLOB_FLAG) || from->charset() != charset() ||
        !from->compression_method() != !compression_method())
      return do_conv_blob;
    if (from->pack_length() != Field_blob::pack_length())
      return do_copy_blob;
    return get_identical_copy_func();
  }
  int  store_field(Field *from) override
  {                                             // Be sure the value is stored
    if (field_charset() == &my_charset_bin &&
        from->type_handler()->convert_to_binary_using_val_native())
    {
      NativeBuffer<64> tmp;
      from->val_native(&tmp);
      value.copy(tmp.ptr(), tmp.length(), &my_charset_bin);
      return store(value.ptr(), value.length(), &my_charset_bin);
    }
    from->val_str(&value);
    if (table->copy_blobs ||
        (!value.is_alloced() && from->is_varchar_and_in_write_set()))
      value.copy();
    return store(value.ptr(), value.length(), from->charset());
  }
  bool memcpy_field_possible(const Field *from) const override
  {
    return Field_str::memcpy_field_possible(from) &&
           !compression_method() == !from->compression_method() &&
           !table->copy_blobs;
  }
  bool make_empty_rec_store_default_value(THD *thd, Item *item) override;
  int store(const char *to, size_t length, CHARSET_INFO *charset) override;
  using Field_str::store;
  double val_real() override;
  longlong val_int() override;
  String *val_str(String *, String *) override;
  my_decimal *val_decimal(my_decimal *) override;
  int cmp(const uchar *a, const uchar *b) const override;
  int cmp_prefix(const uchar *a, const uchar *b, size_t prefix_len) const
    override;
  int cmp(const uchar *a, uint32 a_length, const uchar *b, uint32 b_length)
    const;
  int cmp_binary(const uchar *a,const uchar *b, uint32 max_length=~0U) const
     override;
  int key_cmp(const uchar *,const uchar*) const override;
  int key_cmp(const uchar *str, uint length) const override;
  /* Never update the value of min_val for a blob field */
  bool update_min(Field *min_val, bool force_update) override { return false; }
  /* Never update the value of max_val for a blob field */
  bool update_max(Field *max_val, bool force_update) override { return false; }
  uint32 key_length() const override { return 0; }
  void sort_string(uchar *buff,uint length) override;
  uint32 pack_length() const override
  { return (uint32) (packlength + portable_sizeof_char_ptr); }

  /**
     Return the packed length without the pointer size added. 

     This is used to determine the size of the actual data in the row
     buffer.

     @returns The length of the raw data itself without the pointer.
  */
  uint32 pack_length_no_ptr() const
  { return (uint32) (packlength); }
  uint row_pack_length() const override { return pack_length_no_ptr(); }
  uint32 sort_length() const override;
  uint32 sort_suffix_length() const override;
  uint32 value_length() override { return get_length(); }
  uint32 max_data_length() const override
  {
    return (uint32) (((ulonglong) 1 << (packlength*8)) -1);
  }
  int reset() override { bzero(ptr, packlength+sizeof(uchar*)); return 0; }
  void reset_fields() override
  {
    bzero((uchar*) &value, sizeof value);
    bzero((uchar*) &read_value, sizeof read_value);
  }
  uint32 get_field_buffer_size() { return value.alloced_length(); }
  void store_length(uchar *i_ptr, uint i_packlength, uint32 i_number);
  void store_length(size_t number)
  {
    DBUG_ASSERT(number < UINT_MAX32);
    store_length(ptr, packlength, (uint32)number);
  }
  inline uint32 get_length(my_ptrdiff_t row_offset= 0) const
  { return get_length(ptr+row_offset, this->packlength); }
  uint32 get_length(const uchar *ptr, uint packlength) const;
  uint32 get_length(const uchar *ptr_arg) const
  { return get_length(ptr_arg, this->packlength); }
  inline uchar *get_ptr() const { return get_ptr(ptr); }
  inline uchar *get_ptr(const uchar *ptr_arg) const
  {
    uchar *s;
    memcpy(&s, ptr_arg + packlength, sizeof(uchar*));
    return s;
  }
  inline void set_ptr(uchar *length, uchar *data)
  {
    memcpy(ptr,length,packlength);
    memcpy(ptr+packlength, &data,sizeof(char*));
  }
  void set_ptr_offset(my_ptrdiff_t ptr_diff, uint32 length, const uchar *data)
  {
    uchar *ptr_ofs= ADD_TO_PTR(ptr,ptr_diff,uchar*);
    store_length(ptr_ofs, packlength, length);
    memcpy(ptr_ofs+packlength, &data, sizeof(char*));
  }
  inline void set_ptr(uint32 length, uchar *data)
  {
    set_ptr_offset(0, length, data);
  }
  int copy_value(Field_blob *from);
  uint get_key_image(uchar *buff, uint length,
                     const uchar *ptr_arg, imagetype type) const override
  {
    DBUG_ASSERT(type == itRAW);
    return get_key_image_itRAW(ptr_arg, buff, length);
  }
  void set_key_image(const uchar *buff,uint length) override;
  Field *new_key_field(MEM_ROOT *root, TABLE *new_table,
                       uchar *new_ptr, uint32 length,
                       uchar *new_null_ptr, uint new_null_bit) override;
  void sql_type(String &str) const override;
  /**
     Copy blob buffer into internal storage "value" and update record pointer.

     @retval true     Memory allocation error
     @retval false    Success
  */
  bool copy()
  {
    uchar *tmp= get_ptr();
    if (value.copy((char*) tmp, get_length(), charset()))
    {
      Field_blob::reset();
      return 1;
    }
    tmp=(uchar*) value.ptr();
    memcpy(ptr+packlength, &tmp, sizeof(char*));
    return 0;
  }
  void swap(String &inout, bool set_read_value)
  {
    if (set_read_value)
      read_value.swap(inout);
    else
      value.swap(inout);
  }
  /**
     Return pointer to blob cache or NULL if not cached.
  */
  String * cached(bool *set_read_value)
  {
    char *tmp= (char *) get_ptr();
    if (!value.is_empty() && tmp == value.ptr())
    {
      *set_read_value= false;
      return &value;
    }

    if (!read_value.is_empty() && tmp == read_value.ptr())
    {
      *set_read_value= true;
      return &read_value;
    }

    return NULL;
  }
  /* store value for the duration of the current read record */
  inline void swap_value_and_read_value()
  {
    read_value.swap(value);
  }
  inline void set_value(uchar *data)
  {
    /* Set value pointer. Lengths are not important */
    value.reset((char*) data, 1, 1, &my_charset_bin);
  }
  uchar *pack(uchar *to, const uchar *from, uint max_length) override;
  const uchar *unpack(uchar *to, const uchar *from, const uchar *from_end,
                      uint param_data) override;
  uint packed_col_length(const uchar *col_ptr, uint length) override;
  uint max_packed_col_length(uint max_length) override;
  void free() override
  {
    value.free();
    read_value.free();
  }
  inline void clear_temporary()
  {
    uchar *tmp= get_ptr();
    if (likely(value.ptr() == (char*) tmp))
      bzero((uchar*) &value, sizeof(value));
    else
    {
      /*
        Currently read_value should never point to tmp, the following code
        is mainly here to make things future proof.
      */
      if (unlikely(read_value.ptr() == (char*) tmp))
        bzero((uchar*) &read_value, sizeof(read_value));
    }
  }
  uint size_of() const override { return sizeof *this; }
  bool has_charset() const override { return charset() != &my_charset_bin; }
  uint32 max_display_length() const override;
  uint32 char_length() const override;
  uint32 character_octet_length() const override;
  bool is_equal(const Column_definition &new_field) const override;
  bool can_be_converted_by_engine(const Column_definition &new_type) const
    override
  {
    return table->file->can_convert_blob(this, new_type);
  }
  void print_key_value(String *out, uint32 length) override;
  Binlog_type_info binlog_type_info() const override;

  friend void TABLE::remember_blob_values(String *blob_storage);
  friend void TABLE::restore_blob_values(String *blob_storage);
};


class Field_blob_compressed final :public Field_blob {
public:
  Field_blob_compressed(uchar *ptr_arg, uchar *null_ptr_arg,
                        uchar null_bit_arg, enum utype unireg_check_arg,
                        const LEX_CSTRING *field_name_arg, TABLE_SHARE *share,
                        uint blob_pack_length, const DTCollation &collation,
                        Compression_method *compression_method_arg):
    Field_blob(ptr_arg, null_ptr_arg, null_bit_arg, unireg_check_arg,
               field_name_arg, share, blob_pack_length, collation),
    compression_method_ptr(compression_method_arg) {}
  Compression_method *compression_method() const override
  { return compression_method_ptr; }
private:
  Compression_method *compression_method_ptr;
  int store(const char *to, size_t length, CHARSET_INFO *charset) override;
  using Field_str::store;
  String *val_str(String *, String *) override;
  double val_real() override;
  longlong val_int() override;
  /*
    We use the default Field::send() implementation,
    because the derived optimized version (from Field_longstr)
    is not suitable for compressed fields.
  */
  bool send(Protocol *protocol) override
  {
    return Field::send(protocol);
  }
  uint size_of() const override { return sizeof *this; }
  enum_field_types binlog_type() const override
  { return MYSQL_TYPE_BLOB_COMPRESSED; }
  void sql_type(String &str) const override
  {
    Field_blob::sql_type(str);
    str.append(STRING_WITH_LEN(" /*!100301 COMPRESSED*/"));
  }

  /*
    Compressed fields can't have keys as two rows may have different
    compression methods or compression levels.
  */

  uint get_key_image(uchar *buff, uint length,
                     const uchar *ptr_arg, imagetype type_arg) const override
  { DBUG_ASSERT(0); return 0; }
  void set_key_image(const uchar *, uint) override
  { DBUG_ASSERT(0); }
  int key_cmp(const uchar *, const uchar *) const override
  { DBUG_ASSERT(0); return 0; }
  int key_cmp(const uchar *, uint) const override
  { DBUG_ASSERT(0); return 0; }
  Field *new_key_field(MEM_ROOT *, TABLE *, uchar *, uint32, uchar *, uint)
    override
  { DBUG_ASSERT(0); return 0; }
  Binlog_type_info binlog_type_info() const override;
};


class Field_enum :public Field_str {
  static void do_field_enum(Copy_field *copy_field);
  longlong val_int(const uchar *) const;
  bool can_optimize_range_or_keypart_ref(const Item_bool_func *cond,
                                         const Item *item) const;
protected:
  uint packlength;
public:
  const TYPELIB *typelib;
  Field_enum(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
             uchar null_bit_arg,
             enum utype unireg_check_arg, const LEX_CSTRING *field_name_arg,
             uint packlength_arg,
             const TYPELIB *typelib_arg,
             const DTCollation &collation)
    :Field_str(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
	       unireg_check_arg, field_name_arg, collation),
    packlength(packlength_arg),typelib(typelib_arg)
  {
      flags|=ENUM_FLAG;
  }
  Field *make_new_field(MEM_ROOT *root, TABLE *new_table, bool keep_type)
    override;
  const Type_handler *type_handler() const override
  { return &type_handler_enum; }
  enum ha_base_keytype key_type() const override;
  sql_mode_t can_handle_sql_mode_dependency_on_store() const override;
  enum_conv_type rpl_conv_type_from(const Conv_source &source,
                                    const Relay_log_info *rli,
                                    const Conv_param &param) const override;
  Copy_func *get_copy_func(const Field *from) const override
  {
    if (eq_def(from))
      return get_identical_copy_func();
    if (real_type() == MYSQL_TYPE_ENUM &&
        from->real_type() == MYSQL_TYPE_ENUM)
      return do_field_enum;
    if (from->result_type() == STRING_RESULT)
      return do_field_string;
    return do_field_int;
  }
  int store_field(Field *from) override
  {
    if (from->real_type() == MYSQL_TYPE_ENUM && from->val_int() == 0)
    {
      store_type(0);
      return 0;
    }
    return from->save_in_field(this);
  }
  int save_in_field(Field *to) override
  {
    if (to->result_type() != STRING_RESULT)
      return to->store(val_int(), 0);
    return save_in_field_str(to);
  }
  bool memcpy_field_possible(const Field *from) const override
  { return false; }
  void make_empty_rec_reset(THD *) override
  {
    if (flags & NOT_NULL_FLAG)
    {
      set_notnull();
      store((longlong) 1, true);
    }
    else
      reset();
  }
  int  store(const char *to,size_t length,CHARSET_INFO *charset) override;
  int  store(double nr) override;
  int  store(longlong nr, bool unsigned_val) override;
  double val_real() override;
  longlong val_int() override;
  String *val_str(String *, String *) override;
  int cmp(const uchar *,const uchar *) const override;
  void sort_string(uchar *buff,uint length) override;
  uint32 pack_length() const override { return (uint32) packlength; }
  void store_type(ulonglong value);
  void sql_type(String &str) const override;
  uint size_of() const override { return sizeof *this; }
  uint pack_length_from_metadata(uint field_metadata) const override
  { return (field_metadata & 0x00ff); }
  uint row_pack_length() const override { return pack_length(); }
  bool zero_pack() const override { return false; }
  bool optimize_range(uint, uint) const override { return false; }
  bool eq_def(const Field *field) const override;
  bool has_charset() const override { return true; }
  /* enum and set are sorted as integers */
  CHARSET_INFO *sort_charset() const override { return &my_charset_bin; }
  decimal_digits_t decimals() const override { return 0; }
  const TYPELIB *get_typelib() const override { return typelib; }

  uchar *pack(uchar *to, const uchar *from, uint max_length) override;
  const uchar *unpack(uchar *to, const uchar *from, const uchar *from_end,
                      uint param_data) override;

  bool can_optimize_keypart_ref(const Item_bool_func *cond,
                                const Item *item) const override
  {
    return can_optimize_range_or_keypart_ref(cond, item);
  }
  bool can_optimize_group_min_max(const Item_bool_func *cond,
                                  const Item *const_item) const override
  {
    /*
      Can't use GROUP_MIN_MAX optimization for ENUM and SET,
      because the values are stored as numbers in index,
      while MIN() and MAX() work as strings.
      It would return the records with min and max enum numeric indexes.
     "Bug#45300 MAX() and ENUM type" should be fixed first.
    */
    return false;
  }
  bool can_optimize_range(const Item_bool_func *cond,
                          const Item *item,
                          bool is_eq_func) const override
  {
    return can_optimize_range_or_keypart_ref(cond, item);
  }
  Binlog_type_info binlog_type_info() const override;
private:
  bool is_equal(const Column_definition &new_field) const override;
};


class Field_set final :public Field_enum {
public:
  Field_set(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
	    uchar null_bit_arg, enum utype unireg_check_arg,
            const LEX_CSTRING *field_name_arg, uint32 packlength_arg,
	    const TYPELIB *typelib_arg, const DTCollation &collation)
    :Field_enum(ptr_arg, len_arg, null_ptr_arg, null_bit_arg, unireg_check_arg,
                field_name_arg, packlength_arg, typelib_arg, collation)
    {
      flags=(flags & ~ENUM_FLAG) | SET_FLAG;
    }
  void make_empty_rec_reset(THD *thd) override
  {
    Field::make_empty_rec_reset(thd);
  }

  int  store_field(Field *from) override { return from->save_in_field(this); }
  int  store(const char *to,size_t length,CHARSET_INFO *charset) override;
  int  store(double nr) override
  { return Field_set::store((longlong) nr, FALSE); }
  int  store(longlong nr, bool unsigned_val) override;

  bool zero_pack() const override { return true; }
  String *val_str(String *, String *) override;
  void sql_type(String &str) const override;
  uint size_of() const override { return sizeof *this; }
  const Type_handler *type_handler() const override
  { return &type_handler_set; }
  bool has_charset() const override { return true; }
  Binlog_type_info binlog_type_info() const override;
};


/*
  Note:
    To use Field_bit::cmp_binary() you need to copy the bits stored in
    the beginning of the record (the NULL bytes) to each memory you
    want to compare (where the arguments point).

    This is the reason:
    - Field_bit::cmp_binary() is only implemented in the base class
      (Field::cmp_binary()).
    - Field::cmp_binary() currently uses pack_length() to calculate how
      long the data is.
    - pack_length() includes size of the bits stored in the NULL bytes
      of the record.
*/
class Field_bit :public Field {
public:
  uchar *bit_ptr;     // position in record where 'uneven' bits store
  uchar bit_ofs;      // offset to 'uneven' high bits
  uint bit_len;       // number of 'uneven' high bits
  uint bytes_in_rec;
  Field_bit(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
            uchar null_bit_arg, uchar *bit_ptr_arg, uchar bit_ofs_arg,
            enum utype unireg_check_arg, const LEX_CSTRING *field_name_arg);
  const Type_handler *type_handler() const override
  { return &type_handler_bit; }
  enum ha_base_keytype key_type() const override { return HA_KEYTYPE_BIT; }
  uint16 key_part_flag() const override { return HA_BIT_PART; }
  uint32 key_length() const override
  { return (uint32) (field_length + 7) / 8; }
  uint32 max_data_length() const override { return key_length(); }
  uint32 max_display_length() const override { return field_length; }
  enum_conv_type rpl_conv_type_from(const Conv_source &source,
                                    const Relay_log_info *rli,
                                    const Conv_param &param) const override;
  CHARSET_INFO *charset() const override { return &my_charset_bin; }
  const DTCollation & dtcollation() const override;
  Information_schema_numeric_attributes
    information_schema_numeric_attributes() const override
  {
    return Information_schema_numeric_attributes(field_length);
  }
  void update_data_type_statistics(Data_type_statistics *st) const override
  {
    st->m_uneven_bit_length+= field_length & 7;
  }
  uint size_of() const override { return sizeof *this; }
  int reset() override
  {
    bzero(ptr, bytes_in_rec); 
    if (bit_ptr && (bit_len > 0))  // reset odd bits among null bits
      clr_rec_bits(bit_ptr, bit_ofs, bit_len);
    return 0; 
  }
  Copy_func *get_copy_func(const Field *from) const override
  {
    if (from->cmp_type() == DECIMAL_RESULT)
      return do_field_decimal;
    return do_field_int;
  }
  int save_in_field(Field *to) override { return to->store(val_int(), true); }
  bool memcpy_field_possible(const Field *from) const override{ return false; }
  int store(const char *to, size_t length, CHARSET_INFO *charset) override;
  int store(double nr) override;
  int store(longlong nr, bool unsigned_val) override;
  int store_decimal(const my_decimal *) override;
  double val_real() override;
  longlong val_int() override;
  String *val_str(String*, String *) override;
  bool str_needs_quotes() const override { return true; }
  my_decimal *val_decimal(my_decimal *) override;
  bool val_bool() override { return val_int() != 0; }
  int cmp(const uchar *a, const uchar *b) const override
  {
    DBUG_ASSERT(ptr == a || ptr == b);
    if (ptr == a)
      return Field_bit::key_cmp(b, bytes_in_rec + MY_TEST(bit_len));
    else
      return Field_bit::key_cmp(a, bytes_in_rec + MY_TEST(bit_len)) * -1;
  }
  int cmp_binary_offset(uint row_offset) override
  { return cmp_offset(row_offset); }
  int cmp_prefix(const uchar *a, const uchar *b,
                 size_t  max_length) const override;
  int key_cmp(const uchar *a, const uchar *b) const override
  { return cmp_binary((uchar *) a, (uchar *) b); }
  int key_cmp(const uchar *str, uint length) const override;
  int cmp_offset(my_ptrdiff_t row_offset) override;
  bool update_min(Field *min_val, bool force_update) override
  {
    longlong val= val_int();
    bool update_fl= force_update || val < min_val->val_int();
    if (update_fl)
    {
      min_val->set_notnull();
      min_val->store(val, FALSE);
    }
    return update_fl;
  }
  bool update_max(Field *max_val, bool force_update) override
  {
    longlong val= val_int();
    bool update_fl= force_update || val > max_val->val_int();
    if (update_fl)
    {
      max_val->set_notnull();
      max_val->store(val, FALSE);
    }
    return update_fl;
  }
  void store_field_value(uchar *val, uint) override
  {
    store(*((longlong *)val), TRUE);
  }
  double pos_in_interval(Field *min, Field *max) override
  {
    return pos_in_interval_val_real(min, max);
  }
  void get_image(uchar *buff, uint length,
                 const uchar *ptr_arg, CHARSET_INFO *cs) const override
  { get_key_image(buff, length, ptr_arg, itRAW); }
  void set_image(const uchar *buff,uint length, CHARSET_INFO *cs) override
  { Field_bit::store((char *) buff, length, cs); }
  uint get_key_image(uchar *buff, uint length,
                     const uchar *ptr_arg, imagetype type) const override;
  void set_key_image(const uchar *buff, uint length) override
  { Field_bit::store((char*) buff, length, &my_charset_bin); }
  void sort_string(uchar *buff, uint length) override
  { get_key_image(buff, length, ptr, itRAW); }
  uint32 pack_length() const override
  { return (uint32) (field_length + 7) / 8; }
  uint32 pack_length_in_rec() const override { return bytes_in_rec; }
  uint pack_length_from_metadata(uint field_metadata) const override;
  uint row_pack_length() const override
  { return (bytes_in_rec + ((bit_len > 0) ? 1 : 0)); }
  bool compatible_field_size(uint metadata, const Relay_log_info *rli,
                             uint16 mflags, int *order_var) const override;
  void sql_type(String &str) const override;
  uchar *pack(uchar *to, const uchar *from, uint max_length) override;
  const uchar *unpack(uchar *to, const uchar *from,
                      const uchar *from_end, uint param_data) override;
  int set_default() override;

  Field *new_key_field(MEM_ROOT *root, TABLE *new_table,
                       uchar *new_ptr, uint32 length,
                       uchar *new_null_ptr, uint new_null_bit) override;
  void set_bit_ptr(uchar *bit_ptr_arg, uchar bit_ofs_arg)
  {
    bit_ptr= bit_ptr_arg;
    bit_ofs= bit_ofs_arg;
  }
  bool eq(Field *field) override
  {
    return (Field::eq(field) &&
            bit_ptr == ((Field_bit *)field)->bit_ptr &&
            bit_ofs == ((Field_bit *)field)->bit_ofs);
  }
  bool is_equal(const Column_definition &new_field) const override;
  void move_field_offset(my_ptrdiff_t ptr_diff) override
  {
    Field::move_field_offset(ptr_diff);

    /*
      clang does not like when things are added to a null pointer, even if
      it is never referenced.
    */
    if (bit_ptr)
      bit_ptr= ADD_TO_PTR(bit_ptr, ptr_diff, uchar*);
  }
  void hash(ulong *nr, ulong *nr2) override;

  SEL_ARG *get_mm_leaf(RANGE_OPT_PARAM *param, KEY_PART *key_part,
                       const Item_bool_func *cond,
                       scalar_comparison_op op, Item *value) override
  {
    return get_mm_leaf_int(param, key_part, cond, op, value, true);
  }
  void print_key_value(String *out, uint32 length) override
  {
    val_int_as_str(out, 1);
  }
  /**
     Save the field metadata for bit fields.
     Saves the bit length in the first byte and bytes in record in the
     second byte of the field metadata array at index of *metadata_ptr and
     *(metadata_ptr + 1).

     @param   metadata_ptr   First byte of field metadata

     @returns number of bytes written to metadata_ptr
  */
  Binlog_type_info binlog_type_info() const override
  {
    DBUG_PRINT("debug", ("bit_len: %d, bytes_in_rec: %d",
                       bit_len, bytes_in_rec));
    /*
      Since this class and Field_bit_as_char have different ideas of
      what should be stored here, we compute the values of the metadata
      explicitly using the field_length.
    */
    return Binlog_type_info(type(),
                            static_cast<uint16>((field_length & 7) |
                                                ((field_length / 8) << 8)), 2);
  }

private:
  size_t do_last_null_byte() const override;
};


/**
  BIT field represented as chars for non-MyISAM tables.

  @todo The inheritance relationship is backwards since Field_bit is
  an extended version of Field_bit_as_char and not the other way
  around. Hence, we should refactor it to fix the hierarchy order.
 */
class Field_bit_as_char final :public Field_bit {
public:
  Field_bit_as_char(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
                    uchar null_bit_arg,
                    enum utype unireg_check_arg, const LEX_CSTRING *field_name_arg);
  enum ha_base_keytype key_type() const override { return HA_KEYTYPE_BINARY; }
  uint size_of() const override { return sizeof *this; }
  int store(const char *to, size_t length, CHARSET_INFO *charset) override;
  int store(double nr) override { return Field_bit::store(nr); }
  int store(longlong nr, bool unsigned_val) override
  { return Field_bit::store(nr, unsigned_val); }
  void sql_type(String &str) const override;
};


class Field_row final :public Field_null
{
  class Virtual_tmp_table *m_table;
public:
  Field_row(uchar *ptr_arg, const LEX_CSTRING *field_name_arg)
    :Field_null(ptr_arg, 0, Field::NONE, field_name_arg, &my_charset_bin),
     m_table(NULL)
    {}
  ~Field_row();
  en_fieldtype tmp_engine_column_type(bool use_packed_rows) const
  {
    DBUG_ASSERT(0);
    return Field::tmp_engine_column_type(use_packed_rows);
  }
  enum_conv_type rpl_conv_type_from(const Conv_source &source,
                                    const Relay_log_info *rli,
                                    const Conv_param &param) const
  {
    DBUG_ASSERT(0);
    return CONV_TYPE_IMPOSSIBLE;
  }
  Virtual_tmp_table **virtual_tmp_table_addr() { return &m_table; }
  bool sp_prepare_and_store_item(THD *thd, Item **value);
};


extern const LEX_CSTRING null_clex_str;

class Column_definition_attributes
{
public:
  /*
    At various stages in execution this can be length of field in bytes or
    max number of characters.
  */
  ulonglong length;
  const TYPELIB *interval;
  CHARSET_INFO *charset;
  uint32 srid;
  uint32 pack_flag;
  decimal_digits_t decimals;
  Field::utype unireg_check;
  Column_definition_attributes()
   :length(0),
    interval(NULL),
    charset(&my_charset_bin),
    srid(0),
    pack_flag(0),
    decimals(0),
    unireg_check(Field::NONE)
  { }
  Column_definition_attributes(const Field *field);
  Column_definition_attributes(const Type_all_attributes &attr);
  Field *make_field(TABLE_SHARE *share, MEM_ROOT *mem_root,
                    const Record_addr *rec,
                    const Type_handler *handler,
                    const LEX_CSTRING *field_name,
                    uint32 flags) const;
  uint temporal_dec(uint intlen) const
  {
    return (uint) (length > intlen ? length - intlen - 1 : 0);
  }
  uint pack_flag_to_pack_length() const;
  void frm_pack_basic(uchar *buff) const;
  void frm_pack_charset(uchar *buff) const;
  void frm_pack_numeric_with_dec(uchar *buff) const;
  void frm_unpack_basic(const uchar *buff);
  bool frm_unpack_charset(TABLE_SHARE *share, const uchar *buff);
  bool frm_unpack_numeric_with_dec(TABLE_SHARE *share, const uchar *buff);
  bool frm_unpack_temporal_with_dec(TABLE_SHARE *share, uint intlen,
                                    const uchar *buff);
  void set_length_and_dec(const Lex_length_and_dec_st &attr);
  CHARSET_INFO *explicit_or_derived_charset(const Column_derived_attributes
                                                  *derived_attr) const
  {
    return charset ? charset : derived_attr->charset();
  }
};


/*
  Create field class for CREATE TABLE
*/
class Column_definition: public Sql_alloc,
                         public Type_handler_hybrid_field_type,
                         public Column_definition_attributes
{
  /**
    Create "interval" from "interval_list".
    @param mem_root                   - memory root to create the TYPELIB
                                        instance and its values on
    @param reuse_interval_list_values - determines if TYPELIB can reuse strings
                                        from interval_list, or should always
                                        allocate a copy on mem_root, even if
                                        character set conversion is not needed
    @retval false on success
    @retval true  on error (bad values, or EOM)
  */
  bool create_interval_from_interval_list(MEM_ROOT *mem_root,
                                          bool reuse_interval_list_values);

  /*
    Calculate TYPELIB (set or enum) max and total lengths

    @param  cs            charset+collation pair of the interval
    @param  max_length    length of the longest item
    @param  tot_length    sum of the item lengths

    After this method call:
    - ENUM uses max_length
    - SET uses tot_length.
  */
  void calculate_interval_lengths(uint32 *max_length, uint32 *tot_length)
  {
    const char **pos;
    uint *len;
    *max_length= *tot_length= 0;
    for (pos= interval->type_names, len= interval->type_lengths;
         *pos ; pos++, len++)
    {
      size_t length= charset->numchars(*pos, *pos + *len);
      DBUG_ASSERT(length < UINT_MAX32);
      *tot_length+= (uint) length;
      set_if_bigger(*max_length, (uint32)length);
    }
  }
  bool prepare_stage1_check_typelib_default();
  bool prepare_stage1_convert_default(THD *, MEM_ROOT *, CHARSET_INFO *to);
  const Type_handler *field_type() const; // Prevent using this
  Compression_method *compression_method_ptr;
public:
  LEX_CSTRING field_name;
  LEX_CSTRING comment;			// Comment for field
  enum enum_column_versioning
  {
    VERSIONING_NOT_SET,
    WITH_VERSIONING,
    WITHOUT_VERSIONING
  };
  Item *on_update;		        // ON UPDATE NOW()
  field_visibility_t invisible;
  /*
    The value of `length' as set by parser: is the number of characters
    for most of the types, or of bytes for BLOBs or numeric types.
  */
  uint32 char_length;
  uint  flags, pack_length;
  List<String> interval_list;
  engine_option_value *option_list;


  /*
    This is additinal data provided for any computed(virtual) field.
    In particular it includes a pointer to the item by  which this field
    can be computed from other fields.
  */
  Virtual_column_info
    *vcol_info,                      // Virtual field
    *default_value,                  // Default value
    *check_constraint;               // Check constraint

  enum_column_versioning versioning;

  Table_period_info *period;

  Column_definition()
   :Type_handler_hybrid_field_type(&type_handler_null),
    compression_method_ptr(0),
    comment(null_clex_str),
    on_update(NULL), invisible(VISIBLE), char_length(0),
    flags(0), pack_length(0),
    option_list(NULL),
    vcol_info(0), default_value(0), check_constraint(0),
    versioning(VERSIONING_NOT_SET), period(NULL)
  {
    interval_list.empty();
  }

  Column_definition(THD *thd, Field *field, Field *orig_field);
  bool set_attributes(THD *thd,
                      const Lex_field_type_st &attr,
                      column_definition_type_t type);
  void create_length_to_internal_length_null()
  {
    DBUG_ASSERT(length == 0);
    pack_length= 0;
  }
  void create_length_to_internal_length_simple()
  {
    pack_length= type_handler()->calc_pack_length((uint32) length);
  }
  void create_length_to_internal_length_string()
  {
    length*= charset->mbmaxlen;
    if (real_field_type() == MYSQL_TYPE_VARCHAR && compression_method())
      length++;
    set_if_smaller(length, UINT_MAX32);
    pack_length= type_handler()->calc_pack_length((uint32) length);
  }
  void create_length_to_internal_length_typelib()
  {
    /* Pack_length already calculated in sql_parse.cc */
    length*= charset->mbmaxlen;
  }
  bool vers_sys_field() const
  {
    return flags & (VERS_ROW_START | VERS_ROW_END);
  }
  void create_length_to_internal_length_bit();
  void create_length_to_internal_length_newdecimal();

  /*
    Prepare the "charset" member for string data types,
    such as CHAR, VARCHAR, TEXT, ENUM, SET:
    - derive the charset if not specified explicitly
    - find a _bin collation if the BINARY comparison style was specified, e.g.:
       CREATE TABLE t1 (a VARCHAR(10) BINARY) CHARSET utf8;
  */
  bool prepare_charset_for_string(const Column_derived_attributes *dattr);

  /**
    Prepare a SET/ENUM field.
    Create "interval" from "interval_list" if needed, and adjust "length".
    @param mem_root                   - Memory root to allocate TYPELIB and
                                        its values on
    @param reuse_interval_list_values - determines if TYPELIB can reuse value
                                        buffers from interval_list, or should
                                        always allocate a copy on mem_root,
                                        even if character set conversion
                                        is not needed
  */
  bool prepare_interval_field(MEM_ROOT *mem_root,
                              bool reuse_interval_list_values);

  void prepare_interval_field_calc_length()
  {
    uint32 field_length, dummy;
    if (real_field_type() == MYSQL_TYPE_SET)
    {
      calculate_interval_lengths(&dummy, &field_length);
      length= field_length + (interval->count - 1);
    }
    else /* MYSQL_TYPE_ENUM */
    {
      calculate_interval_lengths(&field_length, &dummy);
      length= field_length;
    }
    set_if_smaller(length, MAX_FIELD_WIDTH - 1);
  }

  bool prepare_blob_field(THD *thd);

  bool sp_prepare_create_field(THD *thd, MEM_ROOT *mem_root);

  bool prepare_stage1(THD *thd, MEM_ROOT *mem_root,
                      handler *file, ulonglong table_flags,
                      const Column_derived_attributes *derived_attr);
  void prepare_stage1_simple(CHARSET_INFO *cs)
  {
    charset= cs;
    create_length_to_internal_length_simple();
  }
  bool prepare_stage1_typelib(THD *thd, MEM_ROOT *mem_root,
                              handler *file, ulonglong table_flags);
  bool prepare_stage1_string(THD *thd, MEM_ROOT *mem_root,
                             handler *file, ulonglong table_flags);
  bool prepare_stage1_bit(THD *thd, MEM_ROOT *mem_root,
                          handler *file, ulonglong table_flags);

  bool bulk_alter(const Column_derived_attributes *derived_attr,
                  const Column_bulk_alter_attributes *bulk_attr)
  {
    return type_handler()->Column_definition_bulk_alter(this,
                                                        derived_attr,
                                                        bulk_attr);
  }
  void redefine_stage1_common(const Column_definition *dup_field,
                              const handler *file);
  bool redefine_stage1(const Column_definition *dup_field, const handler *file)
  {
    const Type_handler *handler= dup_field->type_handler();
    return handler->Column_definition_redefine_stage1(this, dup_field, file);
  }
  bool prepare_stage2(handler *handler, ulonglong table_flags);
  bool prepare_stage2_blob(handler *handler,
                           ulonglong table_flags, uint field_flags);
  bool prepare_stage2_varchar(ulonglong table_flags);
  bool prepare_stage2_typelib(const char *type_name, uint field_flags,
                              uint *dup_val_count);
  uint pack_flag_numeric() const;
  uint sign_length() const { return flags & UNSIGNED_FLAG ? 0 : 1; }
  bool check_length(uint mysql_errno, uint max_allowed_length) const;
  bool fix_attributes_real(uint default_length);
  bool fix_attributes_int(uint default_length);
  bool fix_attributes_decimal();
  bool fix_attributes_temporal_with_time(uint int_part_length);
  bool fix_attributes_bit();

  bool check(THD *thd);
  bool validate_check_constraint(THD *thd);

  bool stored_in_db() const { return !vcol_info || vcol_info->stored_in_db; }

  ha_storage_media field_storage_type() const
  {
    return (ha_storage_media)
      ((flags >> FIELD_FLAGS_STORAGE_MEDIA) & 3);
  }

  column_format_type column_format() const
  {
    return (column_format_type)
      ((flags >> FIELD_FLAGS_COLUMN_FORMAT) & 3);
  }

  bool has_default_function() const
  {
    return unireg_check != Field::NONE;
  }

  Field *make_field(TABLE_SHARE *share, MEM_ROOT *mem_root,
                    const Record_addr *addr,
                    const LEX_CSTRING *field_name_arg) const
  {
    return Column_definition_attributes::make_field(share, mem_root, addr,
                                                    type_handler(),
                                                    field_name_arg, flags);
  }
  Field *make_field(TABLE_SHARE *share, MEM_ROOT *mem_root,
                    const LEX_CSTRING *field_name_arg) const
  {
    Record_addr addr(true);
    return make_field(share, mem_root, &addr, field_name_arg);
  }
  /* Return true if default is an expression that must be saved explicitely */
  bool has_default_expression();

  bool has_default_now_unireg_check() const
  {
    return unireg_check == Field::TIMESTAMP_DN_FIELD
        || unireg_check == Field::TIMESTAMP_DNUN_FIELD;
  }

  void set_type(const Column_definition &other)
  {
    set_handler(other.type_handler());
    length= other.length;
    char_length= other.char_length;
    decimals= other.decimals;
    flags= other.flags;
    pack_length= other.pack_length;
    unireg_check= other.unireg_check;
    interval= other.interval;
    charset= other.charset;
    srid= other.srid;
    pack_flag= other.pack_flag;
  }

  // Replace the entire value by another definition
  void set_column_definition(const Column_definition *def)
  {
    *this= *def;
  }
  bool set_compressed(const char *method);
  bool set_compressed_deprecated(THD *thd, const char *method);
  bool set_compressed_deprecated_column_attribute(THD *thd,
                                                  const char *pos,
                                                  const char *method);
  void set_compression_method(Compression_method *compression_method_arg)
  { compression_method_ptr= compression_method_arg; }
  Compression_method *compression_method() const
  { return compression_method_ptr; }

  bool check_vcol_for_key(THD *thd) const;

  void set_lex_charset_collation(const Lex_charset_collation_st &lc)
  {
    charset= lc.charset_collation();
    if (lc.is_contextually_typed_collation())
      flags|= CONTEXT_COLLATION_FLAG;
    else
      flags&= ~CONTEXT_COLLATION_FLAG;
  }
  Lex_charset_collation lex_charset_collation() const
  {
    return Lex_charset_collation(
             charset,
             !charset ? Lex_charset_collation_st::TYPE_EMPTY :
             flags & CONTEXT_COLLATION_FLAG ?
             Lex_charset_collation_st::TYPE_COLLATE_CONTEXTUALLY_TYPED :
             Lex_charset_collation_st::TYPE_CHARACTER_SET);
  }
};


/**
  List of ROW element definitions, e.g.:
    DECLARE a ROW(a INT,b VARCHAR(10))
*/
class Row_definition_list: public List<class Spvar_definition>
{
public:
  inline bool eq_name(const Spvar_definition *def, const LEX_CSTRING *name) const;
  /**
    Find a ROW field by name.
    @param [IN]  name   - the name
    @param [OUT] offset - if the ROW field found, its offset it returned here
    @retval NULL        - the ROW field was not found
    @retval !NULL       - the pointer to the found ROW field
  */
  Spvar_definition *find_row_field_by_name(const LEX_CSTRING *name, uint *offset) const
  {
    // Cast-off the "const" qualifier
    List_iterator<Spvar_definition> it(*((List<Spvar_definition>*)this));
    Spvar_definition *def;
    for (*offset= 0; (def= it++); (*offset)++)
    {
      if (eq_name(def, name))
        return def;
    }
    return 0;
  }
  static Row_definition_list *make(MEM_ROOT *mem_root, Spvar_definition *var)
  {
    Row_definition_list *list;
    if (!(list= new (mem_root) Row_definition_list()))
      return NULL;
    return list->push_back(var, mem_root) ? NULL : list;
  }
  bool append_uniq(MEM_ROOT *thd, Spvar_definition *var);
  bool adjust_formal_params_to_actual_params(THD *thd, List<Item> *args);
  bool adjust_formal_params_to_actual_params(THD *thd,
                                             Item **args, uint arg_count);
  bool resolve_type_refs(THD *);
};

/**
  This class is used during a stored routine or a trigger execution,
  at sp_rcontext::create() time.
  Currently it can represent:
  - variables with explicit data types:   DECLARE a INT;
  - variables with data type references:  DECLARE a t1.a%TYPE;
  - ROW type variables

  Notes:
  - Scalar variables have m_field_definitions==NULL.
  - ROW variables are defined as having MYSQL_TYPE_NULL,
    with a non-empty m_field_definitions.

  Data type references to other object types will be added soon, e.g.:
  - DECLARE a table_name%ROWTYPE;
  - DECLARE a cursor_name%ROWTYPE;
  - DECLARE a record_name%TYPE;
  - DECLARE a variable_name%TYPE;
*/
class Spvar_definition: public Column_definition
{
  Qualified_column_ident *m_column_type_ref; // for %TYPE
  Table_ident *m_table_rowtype_ref;          // for table%ROWTYPE
  bool m_cursor_rowtype_ref;                       // for cursor%ROWTYPE
  uint m_cursor_rowtype_offset;                    // for cursor%ROWTYPE
  Row_definition_list *m_row_field_definitions;    // for ROW
public:
  Spvar_definition()
   :m_column_type_ref(NULL),
    m_table_rowtype_ref(NULL),
    m_cursor_rowtype_ref(false),
    m_cursor_rowtype_offset(0),
    m_row_field_definitions(NULL)
  { }
  Spvar_definition(THD *thd, Field *field)
   :Column_definition(thd, field, NULL),
    m_column_type_ref(NULL),
    m_table_rowtype_ref(NULL),
    m_cursor_rowtype_ref(false),
    m_cursor_rowtype_offset(0),
    m_row_field_definitions(NULL)
  { }
  const Type_handler *type_handler() const
  {
    return Type_handler_hybrid_field_type::type_handler();
  }
  bool is_column_type_ref() const { return m_column_type_ref != 0; }
  bool is_table_rowtype_ref() const { return m_table_rowtype_ref != 0; }
  bool is_cursor_rowtype_ref() const { return m_cursor_rowtype_ref; }
  bool is_explicit_data_type() const
  {
    return !is_column_type_ref() &&
           !is_table_rowtype_ref() &&
           !is_cursor_rowtype_ref();
  }
  Qualified_column_ident *column_type_ref() const
  {
    return m_column_type_ref;
  }
  void set_column_type_ref(Qualified_column_ident *ref)
  {
    m_column_type_ref= ref;
  }

  Table_ident *table_rowtype_ref() const
  {
    return m_table_rowtype_ref;
  }
  void set_table_rowtype_ref(Table_ident *ref)
  {
    DBUG_ASSERT(ref);
    set_handler(&type_handler_row);
    m_table_rowtype_ref= ref;
  }

  uint cursor_rowtype_offset() const
  {
    return m_cursor_rowtype_offset;
  }
  void set_cursor_rowtype_ref(uint offset)
  {
    set_handler(&type_handler_row);
    m_cursor_rowtype_ref= true;
    m_cursor_rowtype_offset= offset;
  }

  /*
    Find a ROW field by name.
    See Row_field_list::find_row_field_by_name() for details.
  */
  Spvar_definition *find_row_field_by_name(const LEX_CSTRING *name, uint *offset) const
  {
    DBUG_ASSERT(m_row_field_definitions);
    return m_row_field_definitions->find_row_field_by_name(name, offset);
  }
  uint is_row() const
  {
    return m_row_field_definitions != NULL;
  }
  // Check if "this" defines a ROW variable with n elements
  uint is_row(uint n) const
  {
    return m_row_field_definitions != NULL &&
           m_row_field_definitions->elements == n;
  }
  Row_definition_list *row_field_definitions() const
  {
    return m_row_field_definitions;
  }
  void set_row_field_definitions(Row_definition_list *list)
  {
    DBUG_ASSERT(list);
    set_handler(&type_handler_row);
    m_row_field_definitions= list;
  }

};


inline bool Row_definition_list::eq_name(const Spvar_definition *def,
                                         const LEX_CSTRING *name) const
{
  return def->field_name.length == name->length && my_strcasecmp(system_charset_info, def->field_name.str, name->str) == 0;
}


class Create_field :public Column_definition
{
public:
  LEX_CSTRING change;			// If done with alter table
  LEX_CSTRING after;			// Put column after this one
  Field *field;				// For alter table
  const TYPELIB *save_interval;         // Temporary copy for the above
                                        // Used only for UCS2 intervals

  /** structure with parsed options (for comparing fields in ALTER TABLE) */
  ha_field_option_struct *option_struct;
  uint	offset;
  uint8 interval_id;
  bool create_if_not_exists;            // Used in ALTER TABLE IF NOT EXISTS

  Create_field():
    Column_definition(),
    field(0), option_struct(NULL),
    create_if_not_exists(false)
  {
    change= after= null_clex_str;
  }
  Create_field(THD *thd, Field *old_field, Field *orig_field):
    Column_definition(thd, old_field, orig_field),
    change(old_field->field_name),
    field(old_field), option_struct(old_field->option_struct),
    create_if_not_exists(false)
  {
    after= null_clex_str;
  }
  /* Used to make a clone of this object for ALTER/CREATE TABLE */
  Create_field *clone(MEM_ROOT *mem_root) const;
};


/*
  A class for sending info to the client
*/

class Send_field :public Sql_alloc,
                  public Type_handler_hybrid_field_type,
                  public Send_field_extended_metadata
{
public:
  LEX_CSTRING db_name;
  LEX_CSTRING table_name, org_table_name;
  LEX_CSTRING col_name, org_col_name;
  ulong length;
  uint flags;
  decimal_digits_t decimals;
  Send_field(Field *field)
  {
    field->make_send_field(this);
    DBUG_ASSERT(table_name.str != 0);
    normalize();
  }
  Send_field(THD *thd, Item *item);
  Send_field(Field *field,
             const LEX_CSTRING &db_name_arg,
             const LEX_CSTRING &table_name_arg)
   :Type_handler_hybrid_field_type(field->type_handler()),
    db_name(db_name_arg),
    table_name(table_name_arg),
    org_table_name(table_name_arg),
    col_name(field->field_name),
    org_col_name(field->field_name),
    length(field->field_length),
    flags(field->table->maybe_null ?
          (field->flags & ~NOT_NULL_FLAG) : field->flags),
    decimals(field->decimals())
  {
    normalize();
  }

private:
  void normalize()
  {
    /* limit number of decimals for float and double */
    if (type_handler()->field_type() == MYSQL_TYPE_FLOAT ||
        type_handler()->field_type() == MYSQL_TYPE_DOUBLE)
      set_if_smaller(decimals, FLOATING_POINT_DECIMALS);
  }
public:
  // This should move to Type_handler eventually
  uint32 max_char_length(CHARSET_INFO *cs) const
  {
    return type_handler()->field_type() >= MYSQL_TYPE_TINY_BLOB &&
           type_handler()->field_type() <= MYSQL_TYPE_BLOB
      ? static_cast<uint32>(length / cs->mbminlen)
      : static_cast<uint32>(length / cs->mbmaxlen);
  }
  uint32 max_octet_length(CHARSET_INFO *from, CHARSET_INFO *to) const
  {
    /*
      For TEXT/BLOB columns, field_length describes the maximum data
      length in bytes. There is no limit to the number of characters
      that a TEXT column can store, as long as the data fits into
      the designated space.
      For the rest of textual columns, field_length is evaluated as
      char_count * mbmaxlen, where character count is taken from the
      definition of the column. In other words, the maximum number
      of characters here is limited by the column definition.

      When one has a LONG TEXT column with a single-byte
      character set, and the connection character set is multi-byte, the
      client may get fields longer than UINT_MAX32, due to
      <character set column> -> <character set connection> conversion.
      In that case column max length would not fit into the 4 bytes
      reserved for it in the protocol. So we cut it here to UINT_MAX32.
    */
    return char_to_byte_length_safe(max_char_length(from), to->mbmaxlen);
  }

  // This should move to Type_handler eventually
  bool is_sane_float() const
  {
    return (decimals <= FLOATING_POINT_DECIMALS ||
            (type_handler()->field_type() != MYSQL_TYPE_FLOAT &&
             type_handler()->field_type() != MYSQL_TYPE_DOUBLE));
  }
  bool is_sane_signess() const
  {
    if (type_handler() == type_handler()->type_handler_signed() &&
        type_handler() == type_handler()->type_handler_unsigned())
      return true; // Any signess is allowed, e.g. DOUBLE, DECIMAL
    /*
      We are here e.g. in case of INT data type.
      The UNSIGNED_FLAG bit must match in flags and in the type handler.
    */
    return ((bool) (flags & UNSIGNED_FLAG)) == type_handler()->is_unsigned();
  }
  bool is_sane() const
  {
    return is_sane_float() && is_sane_signess();
  }
};


/*
  A class for quick copying data to fields
*/

class Copy_field :public Sql_alloc {
public:
  uchar *from_ptr,*to_ptr;
  uchar *from_null_ptr,*to_null_ptr;
  bool *null_row;
  uint	from_bit,to_bit;
  /**
    Number of bytes in the fields pointed to by 'from_ptr' and
    'to_ptr'. Usually this is the number of bytes that are copied from
    'from_ptr' to 'to_ptr'.

    For variable-length fields (VARCHAR), the first byte(s) describe
    the actual length of the text. For VARCHARs with length 
       < 256 there is 1 length byte 
       >= 256 there is 2 length bytes
    Thus, if from_field is VARCHAR(10), from_length (and in most cases
    to_length) is 11. For VARCHAR(1024), the length is 1026. @see
    Field_varstring::length_bytes

    Note that for VARCHARs, do_copy() will be do_varstring*() which
    only copies the length-bytes (1 or 2) + the actual length of the
    text instead of from/to_length bytes.
  */
  uint from_length,to_length;
  Field *from_field,*to_field;
  String tmp;					// For items

  Copy_field() {}
  ~Copy_field() {}
  void set(Field *to,Field *from,bool save);	// Field to field 
  void set(uchar *to,Field *from);		// Field to string
  void (*do_copy)(Copy_field *);
  void (*do_copy2)(Copy_field *);		// Used to handle null values
};


uint pack_length_to_packflag(uint type);
enum_field_types get_blob_type_from_length(ulong length);
int set_field_to_null(Field *field);
int set_field_to_null_with_conversions(Field *field, bool no_conversions);
int convert_null_to_field_value_or_error(Field *field);
bool check_expression(Virtual_column_info *vcol, const LEX_CSTRING *name,
                      enum_vcol_info_type type, Alter_info *alter_info= NULL);

/*
  The following are for the interface with the .frm file
*/

#define FIELDFLAG_DECIMAL		1U
#define FIELDFLAG_BINARY		1U	// Shares same flag
#define FIELDFLAG_NUMBER		2U
#define FIELDFLAG_ZEROFILL		4U
#define FIELDFLAG_PACK			120U	// Bits used for packing
#define FIELDFLAG_INTERVAL		256U    // mangled with decimals!
#define FIELDFLAG_BITFIELD		512U	// mangled with decimals!
#define FIELDFLAG_BLOB			1024U	// mangled with decimals!
#define FIELDFLAG_GEOM			2048U   // mangled with decimals!

#define FIELDFLAG_TREAT_BIT_AS_CHAR     4096U   /* use Field_bit_as_char */
#define FIELDFLAG_LONG_DECIMAL          8192U
#define FIELDFLAG_NO_DEFAULT		16384U  /* sql */
#define FIELDFLAG_MAYBE_NULL		32768U	// sql
#define FIELDFLAG_HEX_ESCAPE		0x10000U
#define FIELDFLAG_PACK_SHIFT		3
#define FIELDFLAG_DEC_SHIFT		8
#define FIELDFLAG_MAX_DEC               63U

#define FIELDFLAG_DEC_MASK              0x3F00U

#define MTYP_TYPENR(type) ((type) & 127U) // Remove bits from type

#define f_is_dec(x)		((x) & FIELDFLAG_DECIMAL)
#define f_is_num(x)		((x) & FIELDFLAG_NUMBER)
#define f_is_zerofill(x)	((x) & FIELDFLAG_ZEROFILL)
#define f_is_packed(x)		((x) & FIELDFLAG_PACK)
#define f_packtype(x)		(((x) >> FIELDFLAG_PACK_SHIFT) & 15)
#define f_decimals(x)		((uint8) (((x) >> FIELDFLAG_DEC_SHIFT) & FIELDFLAG_MAX_DEC))
#define f_is_alpha(x)		(!f_is_num(x))
#define f_is_binary(x)          ((x) & FIELDFLAG_BINARY) // 4.0- compatibility
#define f_is_enum(x)            (((x) & (FIELDFLAG_INTERVAL | FIELDFLAG_NUMBER)) == FIELDFLAG_INTERVAL)
#define f_is_bitfield(x)        (((x) & (FIELDFLAG_BITFIELD | FIELDFLAG_NUMBER)) == FIELDFLAG_BITFIELD)
#define f_is_blob(x)		(((x) & (FIELDFLAG_BLOB | FIELDFLAG_NUMBER)) == FIELDFLAG_BLOB)
#define f_is_geom(x)		(((x) & (FIELDFLAG_GEOM | FIELDFLAG_NUMBER)) == FIELDFLAG_GEOM)
#define f_settype(x)		(((uint) (x)) << FIELDFLAG_PACK_SHIFT)
#define f_maybe_null(x)		((x) & FIELDFLAG_MAYBE_NULL)
#define f_no_default(x)		((x) & FIELDFLAG_NO_DEFAULT)
#define f_bit_as_char(x)        ((x) & FIELDFLAG_TREAT_BIT_AS_CHAR)
#define f_is_hex_escape(x)      ((x) & FIELDFLAG_HEX_ESCAPE)
#define f_visibility(x)         (static_cast<field_visibility_t> ((x) & INVISIBLE_MAX_BITS))

inline
ulonglong TABLE::vers_end_id() const
{
  DBUG_ASSERT(versioned(VERS_TRX_ID));
  return static_cast<ulonglong>(vers_end_field()->val_int());
}

inline
ulonglong TABLE::vers_start_id() const
{
  DBUG_ASSERT(versioned(VERS_TRX_ID));
  return static_cast<ulonglong>(vers_start_field()->val_int());
}

double pos_in_interval_for_string(CHARSET_INFO *cset,
                                  const uchar *midp_val, uint32 midp_len,
                                  const uchar *min_val,  uint32 min_len,
                                  const uchar *max_val,  uint32 max_len);

double pos_in_interval_for_double(double midp_val,
                                  double min_val, double max_val);

#endif /* FIELD_INCLUDED */
