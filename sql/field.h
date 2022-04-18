#ifndef FIELD_INCLUDED
#define FIELD_INCLUDED
/* Copyright (c) 2000, 2015, Oracle and/or its affiliates.
   Copyright (c) 2008, 2020, MariaDB Corporation.

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

class Send_field;
class Copy_field;
class Protocol;
class Create_field;
class Relay_log_info;
class Field;
class Column_statistics;
class Column_statistics_collected;
class Item_func;
class Item_bool_func;
class Item_equal;

enum enum_check_fields
{
  CHECK_FIELD_IGNORE,
  CHECK_FIELD_WARN,
  CHECK_FIELD_ERROR_FOR_NULL
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
      m_result= my_strntod(cs, (char *) str, length, &m_end_of_num, &m_error);
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
      m_result= my_strntoll(cs, str, length, 10, &m_end_of_num, &m_error);
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
      m_result= (*(cs->cset->strtoll10))(cs, str, &m_end_of_num, &m_error);
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
      m_error= str2my_decimal(mask, str,(uint) length, cs,
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

  longlong longlong_from_hex_hybrid(const char *str, uint32 length)
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
      Impostant only when ANY_SUBSTS.
    */
    Item_result m_compare_type;
    /*
      Collation of the comparison operation.
      Important only when ANY_SUBST.
    */
    CHARSET_INFO *m_compare_collation;
  public:
    Context(Subst_constraint subst, Item_result type, CHARSET_INFO *cs)
      :m_subst_constraint(subst),
       m_compare_type(type),
       m_compare_collation(cs) { }
    Subst_constraint subst_constraint() const { return m_subst_constraint; }
    Item_result compare_type() const
    {
      DBUG_ASSERT(m_subst_constraint == ANY_SUBST);
      return m_compare_type;
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
      :Context(IDENTITY_SUBST, STRING_RESULT, &my_charset_bin) { }
  };
  class Context_boolean: public Context
  { // Use this when an item is [a part of] a boolean expression
  public:
    Context_boolean() :Context(ANY_SUBST, INT_RESULT, &my_charset_bin) { }
  };
};


enum Derivation
{
  DERIVATION_IGNORABLE= 6,
  DERIVATION_NUMERIC= 5,
  DERIVATION_COERCIBLE= 4,
  DERIVATION_SYSCONST= 3,
  DERIVATION_IMPLICIT= 2,
  DERIVATION_NONE= 1,
  DERIVATION_EXPLICIT= 0
};


#define STORAGE_TYPE_MASK 7
#define COLUMN_FORMAT_MASK 7
#define COLUMN_FORMAT_SHIFT 3

#define my_charset_numeric      my_charset_latin1
#define MY_REPERTOIRE_NUMERIC   MY_REPERTOIRE_ASCII

/* The length of the header part for each virtual column in the .frm file */
#define FRM_VCOL_OLD_HEADER_SIZE(b) (3 + MY_TEST(b))
#define FRM_VCOL_NEW_BASE_SIZE 16
#define FRM_VCOL_NEW_HEADER_SIZE 6

class Count_distinct_field;

struct ha_field_option_struct;

struct st_cache_field;
int field_conv(Field *to,Field *from);
int truncate_double(double *nr, uint field_length, uint dec,
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
  default:
    return false;
  }
}


/**
   Recognizer for concrete data type (called real_type for some reason),
   returning true if it is one of the TIMESTAMP types.
*/
inline bool is_timestamp_type(enum_field_types type)
{
  return type == MYSQL_TYPE_TIMESTAMP || type == MYSQL_TYPE_TIMESTAMP2;
}


/**
  Convert temporal real types as returned by field->real_type()
  to field type as returned by field->type().
  
  @param real_type  Real type.
  @retval           Field type.
*/
inline enum_field_types real_type_to_type(enum_field_types real_type)
{
  switch (real_type)
  {
  case MYSQL_TYPE_TIME2:
    return MYSQL_TYPE_TIME;
  case MYSQL_TYPE_DATETIME2:
    return MYSQL_TYPE_DATETIME;
  case MYSQL_TYPE_TIMESTAMP2:
    return MYSQL_TYPE_TIMESTAMP;
  case MYSQL_TYPE_NEWDATE:
    return MYSQL_TYPE_DATE;
  /* Note: NEWDECIMAL is a type, not only a real_type */
  default: return real_type;
  }
}


static inline enum enum_mysql_timestamp_type
mysql_type_to_time_type(enum enum_field_types mysql_type)
{
  switch(mysql_type) {
  case MYSQL_TYPE_TIME2:
  case MYSQL_TYPE_TIME: return MYSQL_TIMESTAMP_TIME;
  case MYSQL_TYPE_TIMESTAMP2:
  case MYSQL_TYPE_TIMESTAMP:
  case MYSQL_TYPE_DATETIME2:
  case MYSQL_TYPE_DATETIME: return MYSQL_TIMESTAMP_DATETIME;
  case MYSQL_TYPE_NEWDATE:
  case MYSQL_TYPE_DATE: return MYSQL_TIMESTAMP_DATE;
  default: return MYSQL_TIMESTAMP_ERROR;
  }
}


/**
  Tests if field type is temporal, i.e. represents
  DATE, TIME, DATETIME or TIMESTAMP types in SQL.
     
  @param type    Field type, as returned by field->type().
  @retval true   If field type is temporal
  @retval false  If field type is not temporal
*/
inline bool is_temporal_type(enum_field_types type)
{
  return mysql_type_to_time_type(type) != MYSQL_TIMESTAMP_ERROR;
}


/**
  Tests if field type is temporal and has time part,
  i.e. represents TIME, DATETIME or TIMESTAMP types in SQL.

  @param type    Field type, as returned by field->type().
  @retval true   If field type is temporal type with time part.
  @retval false  If field type is not temporal type with time part.
*/
inline bool is_temporal_type_with_time(enum_field_types type)
{
  switch (type)
  {
  case MYSQL_TYPE_TIME:
  case MYSQL_TYPE_DATETIME:
  case MYSQL_TYPE_TIMESTAMP:
    return true;
  default:
    return false;
  }
}

enum enum_vcol_info_type
{
  VCOL_GENERATED_VIRTUAL, VCOL_GENERATED_STORED,
  VCOL_DEFAULT, VCOL_CHECK_FIELD, VCOL_CHECK_TABLE,
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

class Virtual_column_info: public Sql_alloc
{
private:
  enum_vcol_info_type vcol_type; /* Virtual column expression type */
  /*
    The following data is only updated by the parser and read
    when a Create_field object is created/initialized.
  */
  enum_field_types field_type;   /* Real field type*/
  /* Flag indicating that the field used in a partitioning expression */
  bool in_partitioning_expr;

public:
  /* Flag indicating  that the field is physically stored in the database */
  bool stored_in_db;
  bool utf8;                                    /* Already in utf8 */
  bool automatic_name;
  Item *expr;
  LEX_STRING name;                              /* Name of constraint */
  /* see VCOL_* (VCOL_FIELD_REF, ...) */
  uint flags;

  Virtual_column_info()
  : vcol_type((enum_vcol_info_type)VCOL_TYPE_NONE),
    field_type((enum enum_field_types)MYSQL_TYPE_VIRTUAL),
    in_partitioning_expr(FALSE), stored_in_db(FALSE),
    utf8(TRUE), automatic_name(FALSE), expr(NULL), flags(0)
  {
    name.str= NULL;
    name.length= 0;
  };
  ~Virtual_column_info() {}
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
  enum_field_types get_real_type() const
  {
    return field_type;
  }
  void set_field_type(enum_field_types fld_type)
  {
    /* Calling this function can only be done once. */
    field_type= fld_type;
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
  static void do_field_int(Copy_field *copy);
  static void do_field_real(Copy_field *copy);
  static void do_field_string(Copy_field *copy);
  static void do_field_temporal(Copy_field *copy);
  static void do_field_timestamp(Copy_field *copy);
  static void do_field_decimal(Copy_field *copy);
public:
  static void *operator new(size_t size, MEM_ROOT *mem_root) throw ()
  { return alloc_root(mem_root, size); }
  static void *operator new(size_t size) throw ()
  { return thd_alloc(current_thd, size); }
  static void operator delete(void *ptr_arg, size_t size) { TRASH_FREE(ptr_arg, size); }
  static void operator delete(void *ptr, MEM_ROOT *mem_root)
  { DBUG_ASSERT(0); }

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
  const char * const *table_name;
  const char *field_name;
  /** reference to the list of options or NULL */
  engine_option_value *option_list;
  ha_field_option_struct *option_struct;   /* structure with parsed options */
  LEX_STRING	comment;
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
  enum utype  {
    NONE=0,
    NEXT_NUMBER=15,             // AUTO_INCREMENT
    TIMESTAMP_OLD_FIELD=18,     // TIMESTAMP created before 4.1.3
    TIMESTAMP_DN_FIELD=21,      // TIMESTAMP DEFAULT NOW()
    TIMESTAMP_UN_FIELD=22,      // TIMESTAMP ON UPDATE NOW()
    TIMESTAMP_DNUN_FIELD=23     // TIMESTAMP DEFAULT NOW() ON UPDATE NOW()
    };
  enum geometry_type
  {
    GEOM_GEOMETRY = 0, GEOM_POINT = 1, GEOM_LINESTRING = 2, GEOM_POLYGON = 3,
    GEOM_MULTIPOINT = 4, GEOM_MULTILINESTRING = 5, GEOM_MULTIPOLYGON = 6,
    GEOM_GEOMETRYCOLLECTION = 7
  };
  enum imagetype { itRAW, itMBR};

  utype		unireg_check;
  uint32	field_length;		// Length of field
  uint32	flags;
  uint16        field_index;            // field number in fields array
  uchar		null_bit;		// Bit used to test null bit
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
        const char *field_name_arg);
  virtual ~Field() {}
  /**
    Convenience definition of a copy function returned by
    Field::get_copy_func()
  */
  typedef void Copy_func(Copy_field*);
  virtual Copy_func *get_copy_func(const Field *from) const= 0;
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
  virtual int  store(const char *to, uint length,CHARSET_INFO *cs)=0;
  virtual int  store_hex_hybrid(const char *str, uint length);
  virtual int  store(double nr)=0;
  virtual int  store(longlong nr, bool unsigned_val)=0;
  virtual int  store_decimal(const my_decimal *d)=0;
  virtual int  store_time_dec(MYSQL_TIME *ltime, uint dec);
  virtual int  store_timestamp(my_time_t timestamp, ulong sec_part);
  int store_time(MYSQL_TIME *ltime)
  { return store_time_dec(ltime, TIME_SECOND_PART_DIGITS); }
  int store(const char *to, uint length, CHARSET_INFO *cs,
            enum_check_fields check_level);
  int store(const LEX_STRING *ls, CHARSET_INFO *cs)
  { return store(ls->str, (uint32) ls->length, cs); }
#ifdef HAVE_valgrind_or_MSAN
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

  virtual double val_real(void)=0;
  virtual longlong val_int(void)=0;
  /*
    Get ulonglong representation.
    Negative values are truncated to 0.
  */
  virtual ulonglong val_uint(void)
  {
    longlong nr= val_int();
    return nr < 0 ? 0 : (ulonglong) nr;
  }
  virtual bool val_bool(void)= 0;
  virtual my_decimal *val_decimal(my_decimal *);
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
  String *val_int_as_str(String *val_buffer, bool unsigned_flag);
  fast_field_copier get_fast_field_copier(const Field *from);
  /*
   str_needs_quotes() returns TRUE if the value returned by val_str() needs
   to be quoted when used in constructing an SQL query.
  */
  virtual bool str_needs_quotes() { return FALSE; }
  virtual Item_result result_type () const=0;
  virtual Item_result cmp_type () const { return result_type(); }
  static bool type_can_have_key_part(enum_field_types);
  static enum_field_types field_type_merge(enum_field_types, enum_field_types);
  static Item_result result_merge_type(enum_field_types);
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
  virtual bool compatible_field_size(uint metadata, Relay_log_info *rli,
                                     uint16 mflags, int *order);
  virtual uint pack_length_from_metadata(uint field_metadata)
  {
    DBUG_ENTER("Field::pack_length_from_metadata");
    DBUG_RETURN(field_metadata);
  }
  virtual uint row_pack_length() const { return 0; }
  virtual int save_field_metadata(uchar *first_byte)
  { return do_save_field_metadata(first_byte); }

  /*
    data_length() return the "real size" of the data in memory.
  */
  virtual uint32 data_length() { return pack_length(); }
  virtual uint32 sort_length() const { return pack_length(); }

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

  virtual int reset(void) { bzero(ptr,pack_length()); return 0; }
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

  virtual bool binary() const { return 1; }
  virtual bool zero_pack() const { return 1; }
  virtual enum ha_base_keytype key_type() const { return HA_KEYTYPE_BINARY; }
  virtual uint32 key_length() const { return pack_length(); }
  virtual enum_field_types type() const =0;
  virtual enum_field_types real_type() const { return type(); }
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
  inline  int cmp(const uchar *str) { return cmp(ptr,str); }
  virtual int cmp(const uchar *,const uchar *)=0;
  /*
    The following method is used for comparing prefix keys.
    Currently it's only used in partitioning.
  */
  virtual int cmp_prefix(const uchar *a, const uchar *b, size_t prefix_len)
  { return cmp(a, b); }
  virtual int cmp_binary(const uchar *a,const uchar *b, uint32 max_length=~0U)
  { return memcmp(a,b,pack_length()); }
  virtual int cmp_offset(uint row_offset)
  { return cmp(ptr,ptr+row_offset); }
  virtual int cmp_binary_offset(uint row_offset)
  { return cmp_binary(ptr, ptr+row_offset); };
  virtual int key_cmp(const uchar *a,const uchar *b)
  { return cmp(a, b); }
  virtual int key_cmp(const uchar *str, uint length)
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
  virtual uint decimals() const { return 0; }
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
  inline bool real_maybe_null(void) const { return null_ptr != 0; }
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
    null_bit= p_null_bit;
  }

  bool stored_in_db() const { return !vcol_info || vcol_info->stored_in_db; }
  bool check_vcol_sql_mode_dependency(THD *, vcol_init_mode mode) const;

  virtual sql_mode_t value_depends_on_sql_mode() const
  {
    return 0;
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

  void make_sort_key(uchar *buff, uint length);
  virtual void make_field(Send_field *);

  /*
    Some implementations actually may write up to 8 bytes regardless of what
    size was requested. This is due to the minimum value of the system variable
    max_sort_length.
  */

  virtual void sort_string(uchar *buff,uint length)=0;
  virtual bool optimize_range(uint idx, uint part);
  virtual void free() {}
  virtual Field *make_new_field(MEM_ROOT *root, TABLE *new_table,
                                bool keep_type);
  virtual Field *new_key_field(MEM_ROOT *root, TABLE *new_table,
                               uchar *new_ptr, uint32 length,
                               uchar *new_null_ptr, uint new_null_bit);
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
  virtual void get_image(uchar *buff, uint length, CHARSET_INFO *cs)
    { memcpy(buff,ptr,length); }
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

  virtual uint get_key_image(uchar *buff, uint length, imagetype type_arg)
  {
    get_image(buff, length, &my_charset_bin);
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
  virtual bool send_binary(Protocol *protocol);

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

  uint offset(uchar *record) const
  {
    return (uint) (ptr - record);
  }
  void copy_from_tmp(int offset);
  uint fill_cache_field(struct st_cache_field *copy);
  virtual bool get_date(MYSQL_TIME *ltime, ulonglong fuzzydate);
  bool get_time(MYSQL_TIME *ltime) { return get_date(ltime, TIME_TIME_ONLY); }
  virtual CHARSET_INFO *charset(void) const { return &my_charset_bin; }
  virtual CHARSET_INFO *charset_for_protocol(void) const
  { return binary() ? &my_charset_bin : charset(); }
  virtual CHARSET_INFO *sort_charset(void) const { return charset(); }
  virtual bool has_charset(void) const { return FALSE; }
  virtual enum Derivation derivation(void) const
  { return DERIVATION_IMPLICIT; }
  virtual uint repertoire(void) const { return MY_REPERTOIRE_UNICODE30; }
  virtual void set_derivation(enum Derivation derivation_arg,
                              uint repertoire_arg)
  { }
  virtual int set_time() { return 1; }
  bool set_warning(Sql_condition::enum_warning_level, unsigned int code,
                   int cuted_increment) const;
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
                            const ErrConv *str, timestamp_type ts_type,
                            int cuted_increment) const;
  void set_datetime_warning(uint code,
                            const ErrConv *str, timestamp_type ts_type,
                            int cuted_increment) const
  {
    set_datetime_warning(Sql_condition::WARN_LEVEL_WARN, code, str, ts_type,
                         cuted_increment);
  }
  void set_warning_truncated_wrong_value(const char *type, const char *value);
  inline bool check_overflow(int op_result)
  {
    return (op_result == E_DEC_OVERFLOW);
  }
  int warn_if_overflow(int op_result);
  Copy_func *get_identical_copy_func() const;
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
  virtual uint32 max_display_length()= 0;

  /**
    Whether a field being created is compatible with a existing one.

    Used by the ALTER TABLE code to evaluate whether the new definition
    of a table is compatible with the old definition so that it can
    determine if data needs to be copied over (table data change).
  */
  virtual uint is_equal(Create_field *new_field);
  /* convert decimal to longlong with overflow check */
  longlong convert_decimal2longlong(const my_decimal *val, bool unsigned_flag,
                                    int *err);
  /* The max. number of characters */
  virtual uint32 char_length() const
  {
    return field_length / charset()->mbmaxlen;
  }

  virtual geometry_type get_geometry_type()
  {
    /* shouldn't get here. */
    DBUG_ASSERT(0);
    return GEOM_GEOMETRY;
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

/**
   Retrieve the field metadata for fields.

   This default implementation returns 0 and saves 0 in the metadata_ptr
   value.

   @param   metadata_ptr   First byte of field metadata

   @returns 0 no bytes written.
*/
  virtual int do_save_field_metadata(uchar *metadata_ptr)
  { return 0; }

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
                                               const char *str, uint length,
                                               const char *end_of_num);
  int check_edom_and_truncation(const char *type, bool edom,
                                CHARSET_INFO *cs,
                                const char *str, uint length,
                                const char *end_of_num);
  int check_int(CHARSET_INFO *cs, const char *str, uint length,
                const char *int_end, int error)
  {
    return check_edom_and_truncation("integer",
                                     error == MY_ERRNO_EDOM || str == int_end,
                                     cs, str, length, int_end);
  }
  bool get_int(CHARSET_INFO *cs, const char *from, uint len,
               longlong *rnd, ulonglong unsigned_max,
               longlong signed_min, longlong signed_max);
  void prepend_zeros(String *value) const;
  Item *get_equal_zerofill_const_item(THD *thd, const Context &ctx,
                                      Item *const_item);
public:
  const uint8 dec;
  bool zerofill,unsigned_flag;	// Purify cannot handle bit fields
  Field_num(uchar *ptr_arg,uint32 len_arg, uchar *null_ptr_arg,
	    uchar null_bit_arg, utype unireg_check_arg,
	    const char *field_name_arg,
            uint8 dec_arg, bool zero_arg, bool unsigned_arg);
  enum Item_result result_type () const { return INT_RESULT; }
  enum Derivation derivation(void) const { return DERIVATION_NUMERIC; }
  uint repertoire(void) const { return MY_REPERTOIRE_NUMERIC; }
  CHARSET_INFO *charset(void) const { return &my_charset_numeric; }
  sql_mode_t can_handle_sql_mode_dependency_on_store() const;
  Item *get_equal_const_item(THD *thd, const Context &ctx, Item *const_item)
  {
    return (flags & ZEROFILL_FLAG) ?
           get_equal_zerofill_const_item(thd, ctx, const_item) :
           const_item;
  }
  void add_zerofill_and_unsigned(String &res) const;
  friend class Create_field;
  void make_field(Send_field *);
  uint decimals() const { return (uint) dec; }
  uint size_of() const { return sizeof(*this); }
  bool eq_def(const Field *field) const;
  Copy_func *get_copy_func(const Field *from) const
  {
    if (unsigned_flag && from->cmp_type() == DECIMAL_RESULT)
      return do_field_decimal;
    return do_field_int;
  }
  int save_in_field(Field *to)
  {
    return to->store(val_int(), MY_TEST(flags & UNSIGNED_FLAG));
  }
  bool memcpy_field_possible(const Field *from) const
  {
    return real_type() == from->real_type() &&
           pack_length() == from->pack_length() &&
           !((flags & UNSIGNED_FLAG) && !(from->flags & UNSIGNED_FLAG)) &&
           decimals() == from->decimals();
  }
  int store_decimal(const my_decimal *);
  my_decimal *val_decimal(my_decimal *);
  bool val_bool() { return val_int() != 0; }
  uint is_equal(Create_field *new_field);
  uint row_pack_length() const { return pack_length(); }
  uint32 pack_length_from_metadata(uint field_metadata) {
    uint32 length= pack_length();
    DBUG_PRINT("result", ("pack_length_from_metadata(%d): %u",
                          field_metadata, length));
    return length;
  }
  int  store_time_dec(MYSQL_TIME *ltime, uint dec);
  double pos_in_interval(Field *min, Field *max)
  {
    return pos_in_interval_val_real(min, max);
  }
  bool get_date(MYSQL_TIME *ltime, ulonglong fuzzydate);
};


class Field_str :public Field {
protected:
  // TODO-10.2: Reuse DTCollation instead of these three members
  CHARSET_INFO *field_charset;
  enum Derivation field_derivation;
  uint field_repertoire;
public:
  bool can_be_substituted_to_equal_item(const Context &ctx,
                                        const Item_equal *item_equal);
  Field_str(uchar *ptr_arg,uint32 len_arg, uchar *null_ptr_arg,
	    uchar null_bit_arg, utype unireg_check_arg,
	    const char *field_name_arg, CHARSET_INFO *charset);
  Item_result result_type () const { return STRING_RESULT; }
  uint decimals() const { return NOT_FIXED_DEC; }
  int  save_in_field(Field *to) { return save_in_field_str(to); }
  bool memcpy_field_possible(const Field *from) const
  {
    return real_type() == from->real_type() &&
           pack_length() == from->pack_length() &&
           charset() == from->charset();
  }
  int  store(double nr);
  int  store(longlong nr, bool unsigned_val)=0;
  int  store_decimal(const my_decimal *);
  int  store(const char *to,uint length,CHARSET_INFO *cs)=0;
  int  store_hex_hybrid(const char *str, uint length)
  {
    return store(str, length, &my_charset_bin);
  }
  uint repertoire(void) const { return field_repertoire; }
  CHARSET_INFO *charset(void) const { return field_charset; }
  enum Derivation derivation(void) const { return field_derivation; }
  void set_derivation(enum Derivation derivation_arg,
                      uint repertoire_arg)
  {
    field_derivation= derivation_arg;
    field_repertoire= repertoire_arg;
  }
  bool binary() const { return field_charset == &my_charset_bin; }
  uint32 max_display_length() { return field_length; }
  friend class Create_field;
  my_decimal *val_decimal(my_decimal *);
  bool val_bool() { return val_real() != 0e0; }
  virtual bool str_needs_quotes() { return TRUE; }
  uint is_equal(Create_field *new_field);
  bool eq_cmp_as_binary() { return MY_TEST(flags & BINARY_FLAG); }
  virtual uint length_size() { return 0; }
  double pos_in_interval(Field *min, Field *max)
  {
    return pos_in_interval_val_str(min, max, length_size());
  }
  bool test_if_equality_guarantees_uniqueness(const Item *const_item) const;
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
  bool cmp_to_string_with_same_collation(const Item_bool_func *cond,
                                         const Item *item) const;
  bool cmp_to_string_with_stricter_collation(const Item_bool_func *cond,
                                             const Item *item) const;
public:
  Field_longstr(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
                uchar null_bit_arg, utype unireg_check_arg,
                const char *field_name_arg, CHARSET_INFO *charset_arg)
    :Field_str(ptr_arg, len_arg, null_ptr_arg, null_bit_arg, unireg_check_arg,
               field_name_arg, charset_arg)
    {}

  int store_decimal(const my_decimal *d);
  uint32 max_data_length() const;

  bool is_varchar_and_in_write_set() const
  {
    DBUG_ASSERT(table && table->write_set);
    return bitmap_is_set(table->write_set, field_index);
  }
  bool match_collation_to_optimize_range() const { return true; }

  bool can_optimize_keypart_ref(const Item_bool_func *cond,
                                const Item *item) const;
  bool can_optimize_hash_join(const Item_bool_func *cond,
                              const Item *item) const;
  bool can_optimize_group_min_max(const Item_bool_func *cond,
                                  const Item *const_item) const;
  bool can_optimize_range(const Item_bool_func *cond,
                          const Item *item,
                          bool is_eq_func) const;
};

/* base class for float and double and decimal (old one) */
class Field_real :public Field_num {
protected:
  double get_double(const char *str, uint length, CHARSET_INFO *cs, int *err);
public:
  bool not_fixed;

  Field_real(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
             uchar null_bit_arg, utype unireg_check_arg,
             const char *field_name_arg,
             uint8 dec_arg, bool zero_arg, bool unsigned_arg)
    :Field_num(ptr_arg, len_arg, null_ptr_arg, null_bit_arg, unireg_check_arg,
               field_name_arg, dec_arg, zero_arg, unsigned_arg),
    not_fixed(dec_arg >= FLOATING_POINT_DECIMALS)
    {}
  Item_result result_type () const { return REAL_RESULT; }
  Copy_func *get_copy_func(const Field *from) const
  {
    return do_field_real;
  }
  int save_in_field(Field *to) { return to->store(val_real()); }
  bool memcpy_field_possible(const Field *from) const
  {
    /*
      Cannot do memcpy from a longer field to a shorter field,
      e.g. a DOUBLE(53,10) into a DOUBLE(10,10).
      But it should be OK the other way around.
    */
    return Field_num::memcpy_field_possible(from) &&
           field_length >= from->field_length;
  }
  int store_decimal(const my_decimal *);
  int  store_time_dec(MYSQL_TIME *ltime, uint dec);
  bool get_date(MYSQL_TIME *ltime, ulonglong fuzzydate);
  my_decimal *val_decimal(my_decimal *);
  bool val_bool() { return val_real() != 0e0; }
  uint32 max_display_length() { return field_length; }
  uint size_of() const { return sizeof(*this); }
  Item *get_equal_const_item(THD *thd, const Context &ctx, Item *const_item);
};


class Field_decimal :public Field_real {
public:
  Field_decimal(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
		uchar null_bit_arg,
		enum utype unireg_check_arg, const char *field_name_arg,
		uint8 dec_arg,bool zero_arg,bool unsigned_arg)
    :Field_real(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
                unireg_check_arg, field_name_arg,
                dec_arg, zero_arg, unsigned_arg)
    {}
  Field *make_new_field(MEM_ROOT *root, TABLE *new_table, bool keep_type);
  enum_field_types type() const { return MYSQL_TYPE_DECIMAL;}
  enum ha_base_keytype key_type() const
  { return zerofill ? HA_KEYTYPE_BINARY : HA_KEYTYPE_NUM; }
  Copy_func *get_copy_func(const Field *from) const
  {
    return eq_def(from) ? get_identical_copy_func() : do_field_string;
  }
  int reset(void);
  int store(const char *to,uint length,CHARSET_INFO *charset);
  int store(double nr);
  int store(longlong nr, bool unsigned_val);
  double val_real(void);
  longlong val_int(void);
  String *val_str(String*,String *);
  int cmp(const uchar *,const uchar *);
  void sort_string(uchar *buff,uint length);
  void overflow(bool negative);
  bool zero_pack() const { return 0; }
  void sql_type(String &str) const;
  virtual uchar *pack(uchar* to, const uchar *from, uint max_length)
  {
    return Field::pack(to, from, max_length);
  }
};


/* New decimal/numeric field which use fixed point arithmetic */
class Field_new_decimal :public Field_num {
private:
  int do_save_field_metadata(uchar *first_byte);
  void set_and_validate_prec(uint32 len_arg,
                             uint8 dec_arg, bool unsigned_arg);
public:
  /* The maximum number of decimal digits can be stored */
  uint precision;
  uint bin_size;
  /*
    Constructors take max_length of the field as a parameter - not the
    precision as the number of decimal digits allowed.
    So for example we need to count length from precision handling
    CREATE TABLE ( DECIMAL(x,y)) 
  */
  Field_new_decimal(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
                    uchar null_bit_arg,
                    enum utype unireg_check_arg, const char *field_name_arg,
                    uint8 dec_arg, bool zero_arg, bool unsigned_arg);
  Field_new_decimal(uint32 len_arg, bool maybe_null_arg,
                    const char *field_name_arg, uint8 dec_arg,
                    bool unsigned_arg);
  enum_field_types type() const { return MYSQL_TYPE_NEWDECIMAL;}
  enum ha_base_keytype key_type() const { return HA_KEYTYPE_BINARY; }
  Item_result result_type () const { return DECIMAL_RESULT; }
  Copy_func *get_copy_func(const Field *from) const
  {
    //  if (from->real_type() == MYSQL_TYPE_BIT) // QQ: why?
    //    return do_field_int;
    return do_field_decimal;
  }
  int save_in_field(Field *to)
  {
    my_decimal buff;
    return to->store_decimal(val_decimal(&buff));
  }
  bool memcpy_field_possible(const Field *from) const
  {
    return Field_num::memcpy_field_possible(from) &&
           field_length == from->field_length;
  }
  int  reset(void);
  bool store_value(const my_decimal *decimal_value);
  bool store_value(const my_decimal *decimal_value, int *native_error);
  void set_value_on_overflow(my_decimal *decimal_value, bool sign);
  int  store(const char *to, uint length, CHARSET_INFO *charset);
  int  store(double nr);
  int  store(longlong nr, bool unsigned_val);
  int  store_time_dec(MYSQL_TIME *ltime, uint dec);
  int  store_decimal(const my_decimal *);
  double val_real(void);
  longlong val_int(void);
  ulonglong val_uint(void);
  my_decimal *val_decimal(my_decimal *);
  String *val_str(String*, String *);
  bool get_date(MYSQL_TIME *ltime, ulonglong fuzzydate);
  bool val_bool()
  {
    my_decimal decimal_value;
    my_decimal *val= val_decimal(&decimal_value);
    return val ? !my_decimal_is_zero(val) : 0;
  }
  int cmp(const uchar *, const uchar *);
  void sort_string(uchar *buff, uint length);
  bool zero_pack() const { return 0; }
  void sql_type(String &str) const;
  uint32 max_display_length() { return field_length; }
  uint size_of() const { return sizeof(*this); } 
  uint32 pack_length() const { return (uint32) bin_size; }
  uint pack_length_from_metadata(uint field_metadata);
  uint row_pack_length() const { return pack_length(); }
  bool compatible_field_size(uint field_metadata, Relay_log_info *rli,
                             uint16 mflags, int *order_var);
  uint is_equal(Create_field *new_field);
  virtual const uchar *unpack(uchar* to, const uchar *from, const uchar *from_end, uint param_data);
  static Field *create_from_item(MEM_ROOT *root, Item *);
  Item *get_equal_const_item(THD *thd, const Context &ctx, Item *const_item);
};


class Field_integer: public Field_num
{
public:
  Field_integer(uchar *ptr_arg, uint32 len_arg,
                uchar *null_ptr_arg, uchar null_bit_arg,
                enum utype unireg_check_arg, const char *field_name_arg,
                bool zero_arg, bool unsigned_arg)
   :Field_num(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
               unireg_check_arg, field_name_arg, 0,
               zero_arg, unsigned_arg)
  { }
  ulonglong val_uint()
  {
    longlong nr= val_int();
    return nr < 0 && !unsigned_flag ? 0 : (ulonglong) nr;
  }
};


class Field_tiny :public Field_integer {
public:
  Field_tiny(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
             uchar null_bit_arg,
             enum utype unireg_check_arg, const char *field_name_arg,
             bool zero_arg, bool unsigned_arg)
    :Field_integer(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
                   unireg_check_arg, field_name_arg,
                   zero_arg, unsigned_arg)
    {}
  enum_field_types type() const { return MYSQL_TYPE_TINY;}
  enum ha_base_keytype key_type() const
    { return unsigned_flag ? HA_KEYTYPE_BINARY : HA_KEYTYPE_INT8; }
  int store(const char *to,uint length,CHARSET_INFO *charset);
  int store(double nr);
  int store(longlong nr, bool unsigned_val);
  int reset(void) { ptr[0]=0; return 0; }
  double val_real(void);
  longlong val_int(void);
  String *val_str(String*,String *);
  bool send_binary(Protocol *protocol);
  int cmp(const uchar *,const uchar *);
  void sort_string(uchar *buff,uint length);
  uint32 pack_length() const { return 1; }
  void sql_type(String &str) const;
  uint32 max_display_length() { return 4; }

  virtual uchar *pack(uchar* to, const uchar *from, uint max_length)
  {
    *to= *from;
    return to + 1;
  }

  virtual const uchar *unpack(uchar* to, const uchar *from,
                              const uchar *from_end, uint param_data)
  {
    if (from == from_end)
      return 0;
    *to= *from;
    return from + 1;
  }

  virtual ulonglong get_max_int_value() const
  {
    return unsigned_flag ? 0xFFULL : 0x7FULL;
  }
};


class Field_short :public Field_integer {
public:
  Field_short(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
              uchar null_bit_arg,
              enum utype unireg_check_arg, const char *field_name_arg,
              bool zero_arg, bool unsigned_arg)
    :Field_integer(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
                   unireg_check_arg, field_name_arg,
                   zero_arg, unsigned_arg)
    {}
  Field_short(uint32 len_arg,bool maybe_null_arg, const char *field_name_arg,
              bool unsigned_arg)
    :Field_integer((uchar*) 0, len_arg, maybe_null_arg ? (uchar*) "" : 0, 0,
                   NONE, field_name_arg, 0, unsigned_arg)
    {}
  enum_field_types type() const { return MYSQL_TYPE_SHORT;}
  enum ha_base_keytype key_type() const
    { return unsigned_flag ? HA_KEYTYPE_USHORT_INT : HA_KEYTYPE_SHORT_INT;}
  int store(const char *to,uint length,CHARSET_INFO *charset);
  int store(double nr);
  int store(longlong nr, bool unsigned_val);
  int reset(void) { ptr[0]=ptr[1]=0; return 0; }
  double val_real(void);
  longlong val_int(void);
  String *val_str(String*,String *);
  bool send_binary(Protocol *protocol);
  int cmp(const uchar *,const uchar *);
  void sort_string(uchar *buff,uint length);
  uint32 pack_length() const { return 2; }
  void sql_type(String &str) const;
  uint32 max_display_length() { return 6; }

  virtual uchar *pack(uchar* to, const uchar *from, uint max_length)
  { return pack_int16(to, from); }

  virtual const uchar *unpack(uchar* to, const uchar *from,
                              const uchar *from_end, uint param_data)
  { return unpack_int16(to, from, from_end); }

  virtual ulonglong get_max_int_value() const
  {
    return unsigned_flag ? 0xFFFFULL : 0x7FFFULL;
  }
};

class Field_medium :public Field_integer {
public:
  Field_medium(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
              uchar null_bit_arg,
              enum utype unireg_check_arg, const char *field_name_arg,
              bool zero_arg, bool unsigned_arg)
    :Field_integer(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
                   unireg_check_arg, field_name_arg,
                   zero_arg, unsigned_arg)
    {}
  enum_field_types type() const { return MYSQL_TYPE_INT24;}
  enum ha_base_keytype key_type() const
    { return unsigned_flag ? HA_KEYTYPE_UINT24 : HA_KEYTYPE_INT24; }
  int store(const char *to,uint length,CHARSET_INFO *charset);
  int store(double nr);
  int store(longlong nr, bool unsigned_val);
  int reset(void) { ptr[0]=ptr[1]=ptr[2]=0; return 0; }
  double val_real(void);
  longlong val_int(void);
  String *val_str(String*,String *);
  bool send_binary(Protocol *protocol);
  int cmp(const uchar *,const uchar *);
  void sort_string(uchar *buff,uint length);
  uint32 pack_length() const { return 3; }
  void sql_type(String &str) const;
  uint32 max_display_length() { return 8; }

  virtual uchar *pack(uchar* to, const uchar *from, uint max_length)
  {
    return Field::pack(to, from, max_length);
  }

  virtual ulonglong get_max_int_value() const
  {
    return unsigned_flag ? 0xFFFFFFULL : 0x7FFFFFULL;
  }
};


class Field_long :public Field_integer {
public:
  Field_long(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
             uchar null_bit_arg,
             enum utype unireg_check_arg, const char *field_name_arg,
             bool zero_arg, bool unsigned_arg)
    :Field_integer(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
                   unireg_check_arg, field_name_arg,
                   zero_arg, unsigned_arg)
    {}
  Field_long(uint32 len_arg,bool maybe_null_arg, const char *field_name_arg,
             bool unsigned_arg)
    :Field_integer((uchar*) 0, len_arg, maybe_null_arg ? (uchar*) "" : 0, 0,
                   NONE, field_name_arg, 0, unsigned_arg)
    {}
  enum_field_types type() const { return MYSQL_TYPE_LONG;}
  enum ha_base_keytype key_type() const
    { return unsigned_flag ? HA_KEYTYPE_ULONG_INT : HA_KEYTYPE_LONG_INT; }
  int store(const char *to,uint length,CHARSET_INFO *charset);
  int store(double nr);
  int store(longlong nr, bool unsigned_val);
  int reset(void) { ptr[0]=ptr[1]=ptr[2]=ptr[3]=0; return 0; }
  double val_real(void);
  longlong val_int(void);
  bool send_binary(Protocol *protocol);
  String *val_str(String*,String *);
  int cmp(const uchar *,const uchar *);
  void sort_string(uchar *buff,uint length);
  uint32 pack_length() const { return 4; }
  void sql_type(String &str) const;
  uint32 max_display_length() { return MY_INT32_NUM_DECIMAL_DIGITS; }
  virtual uchar *pack(uchar* to, const uchar *from,
                      uint max_length __attribute__((unused)))
  {
    return pack_int32(to, from);
  }
  virtual const uchar *unpack(uchar* to, const uchar *from,
                              const uchar *from_end,
                              uint param_data __attribute__((unused)))
  {
    return unpack_int32(to, from, from_end);
  }

  virtual ulonglong get_max_int_value() const
  {
    return unsigned_flag ? 0xFFFFFFFFULL : 0x7FFFFFFFULL;
  }
};


class Field_longlong :public Field_integer {
public:
  Field_longlong(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
              uchar null_bit_arg,
              enum utype unireg_check_arg, const char *field_name_arg,
              bool zero_arg, bool unsigned_arg)
    :Field_integer(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
                   unireg_check_arg, field_name_arg,
                   zero_arg, unsigned_arg)
    {}
  Field_longlong(uint32 len_arg,bool maybe_null_arg,
                 const char *field_name_arg,
                  bool unsigned_arg)
    :Field_integer((uchar*) 0, len_arg, maybe_null_arg ? (uchar*) "" : 0, 0,
                   NONE, field_name_arg,0, unsigned_arg)
    {}
  enum_field_types type() const { return MYSQL_TYPE_LONGLONG;}
  enum ha_base_keytype key_type() const
    { return unsigned_flag ? HA_KEYTYPE_ULONGLONG : HA_KEYTYPE_LONGLONG; }
  int store(const char *to,uint length,CHARSET_INFO *charset);
  int store(double nr);
  int store(longlong nr, bool unsigned_val);
  int reset(void)
  {
    ptr[0]=ptr[1]=ptr[2]=ptr[3]=ptr[4]=ptr[5]=ptr[6]=ptr[7]=0;
    return 0;
  }
  double val_real(void);
  longlong val_int(void);
  String *val_str(String*,String *);
  bool send_binary(Protocol *protocol);
  int cmp(const uchar *,const uchar *);
  void sort_string(uchar *buff,uint length);
  uint32 pack_length() const { return 8; }
  void sql_type(String &str) const;
  uint32 max_display_length() { return 20; }
  virtual uchar *pack(uchar* to, const uchar *from,
                      uint max_length  __attribute__((unused)))
  {
    return pack_int64(to, from);
  }
  const uchar *unpack(uchar* to, const uchar *from, const uchar *from_end,
                      uint param_data __attribute__((unused)))
  {
    return unpack_int64(to, from, from_end);
  }
  virtual ulonglong get_max_int_value() const
  {
    return unsigned_flag ? 0xFFFFFFFFFFFFFFFFULL : 0x7FFFFFFFFFFFFFFFULL;
  }
};


class Field_float :public Field_real {
public:
  Field_float(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
	      uchar null_bit_arg,
	      enum utype unireg_check_arg, const char *field_name_arg,
              uint8 dec_arg,bool zero_arg,bool unsigned_arg)
    :Field_real(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
                unireg_check_arg, field_name_arg,
                dec_arg, zero_arg, unsigned_arg)
    {
      if (dec_arg >= FLOATING_POINT_DECIMALS)
        dec_arg= NOT_FIXED_DEC;
    }
  Field_float(uint32 len_arg, bool maybe_null_arg, const char *field_name_arg,
	      uint8 dec_arg)
    :Field_real((uchar*) 0, len_arg, maybe_null_arg ? (uchar*) "": 0, (uint) 0,
                NONE, field_name_arg, dec_arg, 0, 0)
    {
      if (dec_arg >= FLOATING_POINT_DECIMALS)
        dec_arg= NOT_FIXED_DEC;
    }
  enum_field_types type() const { return MYSQL_TYPE_FLOAT;}
  enum ha_base_keytype key_type() const { return HA_KEYTYPE_FLOAT; }
  int store(const char *to,uint length,CHARSET_INFO *charset);
  int store(double nr);
  int store(longlong nr, bool unsigned_val);
  int reset(void) { bzero(ptr,sizeof(float)); return 0; }
  double val_real(void);
  longlong val_int(void);
  String *val_str(String*,String *);
  bool send_binary(Protocol *protocol);
  int cmp(const uchar *,const uchar *);
  void sort_string(uchar *buff,uint length);
  uint32 pack_length() const { return sizeof(float); }
  uint row_pack_length() const { return pack_length(); }
  void sql_type(String &str) const;
  virtual ulonglong get_max_int_value() const
  {
    /*
      We use the maximum as per IEEE754-2008 standard, 2^24
    */
    return 0x1000000ULL;
  }
private:
  int do_save_field_metadata(uchar *first_byte);
};


class Field_double :public Field_real {
  longlong val_int_from_real(bool want_unsigned_result);
public:
  Field_double(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
	       uchar null_bit_arg,
	       enum utype unireg_check_arg, const char *field_name_arg,
	       uint8 dec_arg,bool zero_arg,bool unsigned_arg)
    :Field_real(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
                unireg_check_arg, field_name_arg,
                dec_arg, zero_arg, unsigned_arg)
    {
      if (dec_arg >= FLOATING_POINT_DECIMALS)
        dec_arg= NOT_FIXED_DEC;
    }
  Field_double(uint32 len_arg, bool maybe_null_arg, const char *field_name_arg,
	       uint8 dec_arg)
    :Field_real((uchar*) 0, len_arg, maybe_null_arg ? (uchar*) "" : 0, (uint) 0,
                NONE, field_name_arg, dec_arg, 0, 0)
    {
      if (dec_arg >= FLOATING_POINT_DECIMALS)
        dec_arg= NOT_FIXED_DEC;
    }
  Field_double(uint32 len_arg, bool maybe_null_arg, const char *field_name_arg,
	       uint8 dec_arg, bool not_fixed_arg)
    :Field_real((uchar*) 0, len_arg, maybe_null_arg ? (uchar*) "" : 0, (uint) 0,
                NONE, field_name_arg, dec_arg, 0, 0)
    {
      not_fixed= not_fixed_arg;
      if (dec_arg >= FLOATING_POINT_DECIMALS)
        dec_arg= NOT_FIXED_DEC;
    }
  enum_field_types type() const { return MYSQL_TYPE_DOUBLE;}
  enum ha_base_keytype key_type() const { return HA_KEYTYPE_DOUBLE; }
  int  store(const char *to,uint length,CHARSET_INFO *charset);
  int  store(double nr);
  int  store(longlong nr, bool unsigned_val);
  int reset(void) { bzero(ptr,sizeof(double)); return 0; }
  double val_real(void);
  longlong val_int(void) { return val_int_from_real(false); }
  ulonglong val_uint(void) { return (ulonglong) val_int_from_real(true); }
  String *val_str(String*,String *);
  bool send_binary(Protocol *protocol);
  int cmp(const uchar *,const uchar *);
  void sort_string(uchar *buff,uint length);
  uint32 pack_length() const { return sizeof(double); }
  uint row_pack_length() const { return pack_length(); }
  void sql_type(String &str) const;
  virtual ulonglong get_max_int_value() const
  {
    /*
      We use the maximum as per IEEE754-2008 standard, 2^53
    */
    return 0x20000000000000ULL;
  }
private:
  int do_save_field_metadata(uchar *first_byte);
};


/* Everything saved in this will disappear. It will always return NULL */

class Field_null :public Field_str {
  static uchar null[1];
public:
  Field_null(uchar *ptr_arg, uint32 len_arg,
	     enum utype unireg_check_arg, const char *field_name_arg,
	     CHARSET_INFO *cs)
    :Field_str(ptr_arg, len_arg, null, 1,
	       unireg_check_arg, field_name_arg, cs)
    {}
  enum_field_types type() const { return MYSQL_TYPE_NULL;}
  Copy_func *get_copy_func(const Field *from) const
  {
    return do_field_string;
  }
  int  store(const char *to, uint length, CHARSET_INFO *cs)
  { null[0]=1; return 0; }
  int store(double nr)   { null[0]=1; return 0; }
  int store(longlong nr, bool unsigned_val) { null[0]=1; return 0; }
  int store_decimal(const my_decimal *d)  { null[0]=1; return 0; }
  int reset(void)	  { return 0; }
  double val_real(void)		{ return 0.0;}
  longlong val_int(void)	{ return 0;}
  bool val_bool(void) { return false; }
  my_decimal *val_decimal(my_decimal *) { return 0; }
  String *val_str(String *value,String *value2)
  { value2->length(0); return value2;}
  int cmp(const uchar *a, const uchar *b) { return 0;}
  void sort_string(uchar *buff, uint length)  {}
  uint32 pack_length() const { return 0; }
  void sql_type(String &str) const;
  uint size_of() const { return sizeof(*this); }
  uint32 max_display_length() { return 4; }
  void move_field_offset(my_ptrdiff_t ptr_diff) {}
  bool can_optimize_keypart_ref(const Item_bool_func *cond,
                                const Item *item) const
  {
    DBUG_ASSERT(0);
    return false;
  }
  bool can_optimize_group_min_max(const Item_bool_func *cond,
                                  const Item *const_item) const
  {
    DBUG_ASSERT(0);
    return false;
  }
};


class Field_temporal: public Field {
protected:
  Item *get_equal_const_item_datetime(THD *thd, const Context &ctx,
                                      Item *const_item);
public:
  Field_temporal(uchar *ptr_arg,uint32 len_arg, uchar *null_ptr_arg,
                 uchar null_bit_arg, utype unireg_check_arg,
                 const char *field_name_arg)
    :Field(ptr_arg, len_arg, null_ptr_arg, null_bit_arg, unireg_check_arg,
               field_name_arg)
    { flags|= BINARY_FLAG; }
  Item_result result_type () const { return STRING_RESULT; }   
  sql_mode_t can_handle_sql_mode_dependency_on_store() const;
  int  store_hex_hybrid(const char *str, uint length)
  {
    return store(str, length, &my_charset_bin);
  }
  Copy_func *get_copy_func(const Field *from) const;
  int save_in_field(Field *to)
  {
    MYSQL_TIME ltime;
    if (get_date(&ltime, 0))
      return to->reset();
    return to->store_time_dec(&ltime, decimals());
  }
  bool memcpy_field_possible(const Field *from) const;
  uint32 max_display_length() { return field_length; }
  bool str_needs_quotes() { return TRUE; }
  enum Derivation derivation(void) const { return DERIVATION_NUMERIC; }
  uint repertoire(void) const { return MY_REPERTOIRE_NUMERIC; }
  CHARSET_INFO *charset(void) const { return &my_charset_numeric; }
  CHARSET_INFO *sort_charset(void) const { return &my_charset_bin; }
  bool binary() const { return true; }
  enum Item_result cmp_type () const { return TIME_RESULT; }
  bool val_bool() { return val_real() != 0e0; }
  uint is_equal(Create_field *new_field);
  bool eq_def(const Field *field) const
  {
    return (Field::eq_def(field) && decimals() == field->decimals());
  }
  my_decimal *val_decimal(my_decimal*);
  void set_warnings(Sql_condition::enum_warning_level trunc_level,
                    const ErrConv *str, int was_cut, timestamp_type ts_type);
  double pos_in_interval(Field *min, Field *max)
  {
    return pos_in_interval_val_real(min, max);
  }
  bool can_optimize_keypart_ref(const Item_bool_func *cond,
                                const Item *item) const;
  bool can_optimize_group_min_max(const Item_bool_func *cond,
                                  const Item *const_item) const;
  bool can_optimize_range(const Item_bool_func *cond,
                                  const Item *item,
                                  bool is_eq_func) const
  {
    return true;
  }
};


/**
  Abstract class for:
  - DATE
  - DATETIME
  - DATETIME(1..6)
  - DATETIME(0..6) - MySQL56 version
*/
class Field_temporal_with_date: public Field_temporal {
protected:
  int store_TIME_with_warning(MYSQL_TIME *ltime, const ErrConv *str,
                              int was_cut, int have_smth_to_conv);
  virtual void store_TIME(MYSQL_TIME *ltime) = 0;
  virtual bool get_TIME(MYSQL_TIME *ltime, const uchar *pos,
                        ulonglong fuzzydate) const = 0;
  bool validate_MMDD(bool not_zero_date, uint month, uint day,
                     ulonglong fuzzydate) const
  {
    if (!not_zero_date)
      return fuzzydate & TIME_NO_ZERO_DATE;
    if (!month || !day)
      return fuzzydate & TIME_NO_ZERO_IN_DATE;
    return false;
  }
public:
  Field_temporal_with_date(uchar *ptr_arg, uint32 len_arg,
                           uchar *null_ptr_arg, uchar null_bit_arg,
                           utype unireg_check_arg,
                           const char *field_name_arg)
    :Field_temporal(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
                    unireg_check_arg, field_name_arg)
    {}
  int  store(const char *to, uint length, CHARSET_INFO *charset);
  int  store(double nr);
  int  store(longlong nr, bool unsigned_val);
  int  store_time_dec(MYSQL_TIME *ltime, uint dec);
  int  store_decimal(const my_decimal *);
  bool validate_value_in_record(THD *thd, const uchar *record) const;
};


class Field_timestamp :public Field_temporal {
protected:
  int store_TIME_with_warning(THD *, MYSQL_TIME *, const ErrConv *,
                              int warnings, bool have_smth_to_conv);
public:
  Field_timestamp(uchar *ptr_arg, uint32 len_arg,
                  uchar *null_ptr_arg, uchar null_bit_arg,
		  enum utype unireg_check_arg, const char *field_name_arg,
		  TABLE_SHARE *share);
  enum_field_types type() const { return MYSQL_TYPE_TIMESTAMP;}
  enum ha_base_keytype key_type() const { return HA_KEYTYPE_ULONG_INT; }
  Copy_func *get_copy_func(const Field *from) const;
  int  store(const char *to,uint length,CHARSET_INFO *charset);
  int  store(double nr);
  int  store(longlong nr, bool unsigned_val);
  int  store_time_dec(MYSQL_TIME *ltime, uint dec);
  int  store_decimal(const my_decimal *);
  int  store_timestamp(my_time_t timestamp, ulong sec_part);
  int  save_in_field(Field *to);
  double val_real(void);
  longlong val_int(void);
  String *val_str(String*,String *);
  bool send_binary(Protocol *protocol);
  int cmp(const uchar *,const uchar *);
  void sort_string(uchar *buff,uint length);
  uint32 pack_length() const { return 4; }
  void sql_type(String &str) const;
  bool zero_pack() const { return 0; }
  int set_time();
  /* Get TIMESTAMP field value as seconds since begging of Unix Epoch */
  virtual my_time_t get_timestamp(const uchar *pos, ulong *sec_part) const;
  my_time_t get_timestamp(ulong *sec_part) const
  {
    return get_timestamp(ptr, sec_part);
  }
  virtual void store_TIME(my_time_t timestamp, ulong sec_part)
  {
    int4store(ptr,timestamp);
  }
  bool get_date(MYSQL_TIME *ltime, ulonglong fuzzydate);
  uchar *pack(uchar *to, const uchar *from,
              uint max_length __attribute__((unused)))
  {
    return pack_int32(to, from);
  }
  const uchar *unpack(uchar* to, const uchar *from, const uchar *from_end,
                      uint param_data __attribute__((unused)))
  {
    return unpack_int32(to, from, from_end);
  }
  bool validate_value_in_record(THD *thd, const uchar *record) const;
  Item *get_equal_const_item(THD *thd, const Context &ctx, Item *const_item)
  {
    return get_equal_const_item_datetime(thd, ctx, const_item);
  }
  bool load_data_set_null(THD *thd);
  bool load_data_set_no_data(THD *thd, bool fixed_format);
  uint size_of() const { return sizeof(*this); }
};


/**
  Abstract class for:
  - TIMESTAMP(1..6)
  - TIMESTAMP(0..6) - MySQL56 version
*/
class Field_timestamp_with_dec :public Field_timestamp {
protected:
  uint dec;
public:
  Field_timestamp_with_dec(uchar *ptr_arg,
                           uchar *null_ptr_arg, uchar null_bit_arg,
                           enum utype unireg_check_arg,
                           const char *field_name_arg,
                           TABLE_SHARE *share, uint dec_arg) :
  Field_timestamp(ptr_arg,
                  MAX_DATETIME_WIDTH + dec_arg + MY_TEST(dec_arg), null_ptr_arg,
                  null_bit_arg, unireg_check_arg, field_name_arg, share),
  dec(dec_arg)
  {
    DBUG_ASSERT(dec <= TIME_SECOND_PART_DIGITS);
  }
  uint decimals() const { return dec; }
  enum ha_base_keytype key_type() const { return HA_KEYTYPE_BINARY; }
  uchar *pack(uchar *to, const uchar *from, uint max_length)
  { return Field::pack(to, from, max_length); }
  const uchar *unpack(uchar* to, const uchar *from, const uchar *from_end,
                      uint param_data)
  { return Field::unpack(to, from, from_end, param_data); }
  void make_field(Send_field *field);
  void sort_string(uchar *to, uint length)
  {
    DBUG_ASSERT(length == pack_length());
    memcpy(to, ptr, length);
  }
  bool send_binary(Protocol *protocol);
  double val_real(void);
  my_decimal* val_decimal(my_decimal*);
  int set_time();
};


class Field_timestamp_hires :public Field_timestamp_with_dec {
public:
  Field_timestamp_hires(uchar *ptr_arg,
                        uchar *null_ptr_arg, uchar null_bit_arg,
                        enum utype unireg_check_arg,
                        const char *field_name_arg,
                        TABLE_SHARE *share, uint dec_arg) :
  Field_timestamp_with_dec(ptr_arg, null_ptr_arg, null_bit_arg,
                           unireg_check_arg, field_name_arg, share, dec_arg)
  {
    DBUG_ASSERT(dec);
  }
  my_time_t get_timestamp(const uchar *pos, ulong *sec_part) const;
  void store_TIME(my_time_t timestamp, ulong sec_part);
  int cmp(const uchar *,const uchar *);
  uint32 pack_length() const;
  uint size_of() const { return sizeof(*this); }
};


/**
  TIMESTAMP(0..6) - MySQL56 version
*/
class Field_timestampf :public Field_timestamp_with_dec {
  int do_save_field_metadata(uchar *metadata_ptr)
  {
    *metadata_ptr= decimals();
    return 1;
  }
public:
  Field_timestampf(uchar *ptr_arg,
                   uchar *null_ptr_arg, uchar null_bit_arg,
                   enum utype unireg_check_arg,
                   const char *field_name_arg,
                   TABLE_SHARE *share, uint dec_arg) :
    Field_timestamp_with_dec(ptr_arg, null_ptr_arg, null_bit_arg,
                             unireg_check_arg, field_name_arg, share, dec_arg)
    {}
  enum_field_types real_type() const { return MYSQL_TYPE_TIMESTAMP2; }
  enum_field_types binlog_type() const { return MYSQL_TYPE_TIMESTAMP2; }
  uint32 pack_length() const
  {
    return my_timestamp_binary_length(dec);
  }
  uint row_pack_length() const { return pack_length(); }
  uint pack_length_from_metadata(uint field_metadata)
  {
    DBUG_ENTER("Field_timestampf::pack_length_from_metadata");
    uint tmp= my_timestamp_binary_length(field_metadata);
    DBUG_RETURN(tmp);
  }
  int cmp(const uchar *a_ptr,const uchar *b_ptr)
  {
    return memcmp(a_ptr, b_ptr, pack_length());
  }
  void store_TIME(my_time_t timestamp, ulong sec_part);
  my_time_t get_timestamp(const uchar *pos, ulong *sec_part) const;
  uint size_of() const { return sizeof(*this); }
};


class Field_year :public Field_tiny {
public:
  Field_year(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
	     uchar null_bit_arg,
	     enum utype unireg_check_arg, const char *field_name_arg)
    :Field_tiny(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
		unireg_check_arg, field_name_arg, 1, 1)
    {}
  enum_field_types type() const { return MYSQL_TYPE_YEAR;}
  Copy_func *get_copy_func(const Field *from) const
  {
    if (eq_def(from))
      return get_identical_copy_func();
    switch (from->cmp_type()) {
    case STRING_RESULT:
      return do_field_string;
    case TIME_RESULT:
      return do_field_temporal;
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
  int  store(const char *to,uint length,CHARSET_INFO *charset);
  int  store(double nr);
  int  store(longlong nr, bool unsigned_val);
  int  store_time_dec(MYSQL_TIME *ltime, uint dec);
  double val_real(void);
  longlong val_int(void);
  String *val_str(String*,String *);
  bool get_date(MYSQL_TIME *ltime, ulonglong fuzzydate);
  bool send_binary(Protocol *protocol);
  uint32 max_display_length() { return field_length; }
  void sql_type(String &str) const;
};


class Field_date :public Field_temporal_with_date {
  void store_TIME(MYSQL_TIME *ltime);
  bool get_TIME(MYSQL_TIME *ltime, const uchar *pos, ulonglong fuzzydate) const;
public:
  Field_date(uchar *ptr_arg, uchar *null_ptr_arg, uchar null_bit_arg,
	     enum utype unireg_check_arg, const char *field_name_arg)
    :Field_temporal_with_date(ptr_arg, MAX_DATE_WIDTH, null_ptr_arg, null_bit_arg,
                              unireg_check_arg, field_name_arg) {}
  enum_field_types type() const { return MYSQL_TYPE_DATE;}
  enum ha_base_keytype key_type() const { return HA_KEYTYPE_ULONG_INT; }
  int reset(void) { ptr[0]=ptr[1]=ptr[2]=ptr[3]=0; return 0; }
  bool get_date(MYSQL_TIME *ltime, ulonglong fuzzydate)
  { return Field_date::get_TIME(ltime, ptr, fuzzydate); }
  double val_real(void);
  longlong val_int(void);
  String *val_str(String*,String *);
  bool send_binary(Protocol *protocol);
  int cmp(const uchar *,const uchar *);
  void sort_string(uchar *buff,uint length);
  uint32 pack_length() const { return 4; }
  void sql_type(String &str) const;
  uchar *pack(uchar* to, const uchar *from,
              uint max_length __attribute__((unused)))
  {
    return pack_int32(to, from);
  }
  const uchar *unpack(uchar* to, const uchar *from, const uchar *from_end,
                      uint param_data __attribute__((unused)))
  {
    return unpack_int32(to, from, from_end);
  }
  uint size_of() const { return sizeof(*this); }
};


class Field_newdate :public Field_temporal_with_date {
  void store_TIME(MYSQL_TIME *ltime);
  bool get_TIME(MYSQL_TIME *ltime, const uchar *pos, ulonglong fuzzydate) const;
public:
  Field_newdate(uchar *ptr_arg, uchar *null_ptr_arg, uchar null_bit_arg,
		enum utype unireg_check_arg, const char *field_name_arg)
    :Field_temporal_with_date(ptr_arg, MAX_DATE_WIDTH, null_ptr_arg, null_bit_arg,
                              unireg_check_arg, field_name_arg)
    {}
  enum_field_types type() const { return MYSQL_TYPE_DATE;}
  enum_field_types real_type() const { return MYSQL_TYPE_NEWDATE; }
  enum ha_base_keytype key_type() const { return HA_KEYTYPE_UINT24; }
  int reset(void) { ptr[0]=ptr[1]=ptr[2]=0; return 0; }
  double val_real(void);
  longlong val_int(void);
  String *val_str(String*,String *);
  bool send_binary(Protocol *protocol);
  int cmp(const uchar *,const uchar *);
  void sort_string(uchar *buff,uint length);
  uint32 pack_length() const { return 3; }
  void sql_type(String &str) const;
  bool get_date(MYSQL_TIME *ltime, ulonglong fuzzydate)
  { return Field_newdate::get_TIME(ltime, ptr, fuzzydate); }
  uint size_of() const { return sizeof(*this); }
  Item *get_equal_const_item(THD *thd, const Context &ctx, Item *const_item);
};


class Field_time :public Field_temporal {
  /*
    when this Field_time instance is used for storing values for index lookups
    (see class store_key, Field::new_key_field(), etc), the following
    might be set to TO_DAYS(CURDATE()). See also Field_time::store_time_dec()
  */
  long curdays;
protected:
  virtual void store_TIME(MYSQL_TIME *ltime);
  int store_TIME_with_warning(MYSQL_TIME *ltime, const ErrConv *str,
                              int was_cut, int have_smth_to_conv);
  bool check_zero_in_date_with_warn(ulonglong fuzzydate);
  static void do_field_time(Copy_field *copy);
public:
  Field_time(uchar *ptr_arg, uint length_arg, uchar *null_ptr_arg,
             uchar null_bit_arg, enum utype unireg_check_arg,
             const char *field_name_arg)
    :Field_temporal(ptr_arg, length_arg, null_ptr_arg, null_bit_arg,
                    unireg_check_arg, field_name_arg), curdays(0)
    {}
  enum_field_types type() const { return MYSQL_TYPE_TIME;}
  enum ha_base_keytype key_type() const { return HA_KEYTYPE_INT24; }
  Copy_func *get_copy_func(const Field *from) const
  {
    return from->cmp_type() == REAL_RESULT ? do_field_string : // MDEV-9344
           from->type() == MYSQL_TYPE_YEAR ? do_field_int :
           from->type() == MYSQL_TYPE_BIT  ? do_field_int :
           eq_def(from)                    ? get_identical_copy_func() :
                                             do_field_time;
  }
  bool memcpy_field_possible(const Field *from) const
  {
    return real_type() == from->real_type() &&
           decimals() == from->decimals();
  }
  int store_time_dec(MYSQL_TIME *ltime, uint dec);
  int store(const char *to,uint length,CHARSET_INFO *charset);
  int store(double nr);
  int store(longlong nr, bool unsigned_val);
  int  store_decimal(const my_decimal *);
  double val_real(void);
  longlong val_int(void);
  String *val_str(String*,String *);
  bool get_date(MYSQL_TIME *ltime, ulonglong fuzzydate);
  bool send_binary(Protocol *protocol);
  int cmp(const uchar *,const uchar *);
  void sort_string(uchar *buff,uint length);
  uint32 pack_length() const { return 3; }
  void sql_type(String &str) const;
  uint size_of() const { return sizeof(*this); }
  void set_curdays(THD *thd);
  Field *new_key_field(MEM_ROOT *root, TABLE *new_table,
                       uchar *new_ptr, uint32 length,
                       uchar *new_null_ptr, uint new_null_bit);
  Item *get_equal_const_item(THD *thd, const Context &ctx, Item *const_item);
};


/**
  Abstract class for:
  - TIME(1..6)
  - TIME(0..6) - MySQL56 version
*/
class Field_time_with_dec :public Field_time {
protected:
  uint dec;
public:
  Field_time_with_dec(uchar *ptr_arg, uchar *null_ptr_arg, uchar null_bit_arg,
                      enum utype unireg_check_arg, const char *field_name_arg,
                      uint dec_arg)
    :Field_time(ptr_arg, MIN_TIME_WIDTH + dec_arg + MY_TEST(dec_arg),
                null_ptr_arg, null_bit_arg, unireg_check_arg, field_name_arg),
     dec(dec_arg)
  {
    DBUG_ASSERT(dec <= TIME_SECOND_PART_DIGITS);
  }
  uint decimals() const { return dec; }
  enum ha_base_keytype key_type() const { return HA_KEYTYPE_BINARY; }
  longlong val_int(void);
  double val_real(void);
  void make_field(Send_field *);
};


/**
  TIME(1..6)
*/
class Field_time_hires :public Field_time_with_dec {
  longlong zero_point;
  void store_TIME(MYSQL_TIME *ltime);
public:
  Field_time_hires(uchar *ptr_arg, uchar *null_ptr_arg, uchar null_bit_arg,
             enum utype unireg_check_arg, const char *field_name_arg,
             uint dec_arg)
    :Field_time_with_dec(ptr_arg, null_ptr_arg,
                         null_bit_arg, unireg_check_arg, field_name_arg,
                         dec_arg)
  {
    DBUG_ASSERT(dec);
    zero_point= sec_part_shift(
                   ((TIME_MAX_VALUE_SECONDS+1LL)*TIME_SECOND_PART_FACTOR), dec);
  }
  int reset(void);
  bool get_date(MYSQL_TIME *ltime, ulonglong fuzzydate);
  int cmp(const uchar *,const uchar *);
  void sort_string(uchar *buff,uint length);
  uint32 pack_length() const;
  uint size_of() const { return sizeof(*this); }
};


/**
  TIME(0..6) - MySQL56 version
*/
class Field_timef :public Field_time_with_dec {
  void store_TIME(MYSQL_TIME *ltime);
  int do_save_field_metadata(uchar *metadata_ptr)
  {
    *metadata_ptr= decimals();
    return 1;
  }
public:
  Field_timef(uchar *ptr_arg, uchar *null_ptr_arg, uchar null_bit_arg,
             enum utype unireg_check_arg, const char *field_name_arg,
             uint dec_arg)
    :Field_time_with_dec(ptr_arg, null_ptr_arg,
                         null_bit_arg, unireg_check_arg, field_name_arg,
                         dec_arg)
  {
    DBUG_ASSERT(dec <= TIME_SECOND_PART_DIGITS);
  }
  enum_field_types real_type() const { return MYSQL_TYPE_TIME2; }
  enum_field_types binlog_type() const { return MYSQL_TYPE_TIME2; }
  uint32 pack_length() const
  {
    return my_time_binary_length(dec);
  }
  uint row_pack_length() const { return pack_length(); }
  uint pack_length_from_metadata(uint field_metadata)
  {
    DBUG_ENTER("Field_timef::pack_length_from_metadata");
    uint tmp= my_time_binary_length(field_metadata);
    DBUG_RETURN(tmp);
  }
  void sort_string(uchar *to, uint length)
  {
    DBUG_ASSERT(length == Field_timef::pack_length());
    memcpy(to, ptr, length);
  }
  int cmp(const uchar *a_ptr, const uchar *b_ptr)
  {
    return memcmp(a_ptr, b_ptr, pack_length());
  }
  int reset();
  bool get_date(MYSQL_TIME *ltime, ulonglong fuzzydate);
  uint size_of() const { return sizeof(*this); }
};


class Field_datetime :public Field_temporal_with_date {
  void store_TIME(MYSQL_TIME *ltime);
  bool get_TIME(MYSQL_TIME *ltime, const uchar *pos, ulonglong fuzzydate) const;
public:
  Field_datetime(uchar *ptr_arg, uint length_arg, uchar *null_ptr_arg,
                 uchar null_bit_arg, enum utype unireg_check_arg,
                 const char *field_name_arg)
    :Field_temporal_with_date(ptr_arg, length_arg, null_ptr_arg, null_bit_arg,
                              unireg_check_arg, field_name_arg)
    {
      if (unireg_check == TIMESTAMP_UN_FIELD ||
          unireg_check == TIMESTAMP_DNUN_FIELD)
        flags|= ON_UPDATE_NOW_FLAG;
    }
  enum_field_types type() const { return MYSQL_TYPE_DATETIME;}
  enum ha_base_keytype key_type() const { return HA_KEYTYPE_ULONGLONG; }
  double val_real(void);
  longlong val_int(void);
  String *val_str(String*,String *);
  bool send_binary(Protocol *protocol);
  int cmp(const uchar *,const uchar *);
  void sort_string(uchar *buff,uint length);
  uint32 pack_length() const { return 8; }
  void sql_type(String &str) const;
  bool get_date(MYSQL_TIME *ltime, ulonglong fuzzydate)
  { return Field_datetime::get_TIME(ltime, ptr, fuzzydate); }
  int set_time();
  uchar *pack(uchar* to, const uchar *from,
              uint max_length __attribute__((unused)))
  {
    return pack_int64(to, from);
  }
  const uchar *unpack(uchar* to, const uchar *from, const uchar *from_end,
                      uint param_data __attribute__((unused)))
  {
    return unpack_int64(to, from, from_end);
  }
  Item *get_equal_const_item(THD *thd, const Context &ctx, Item *const_item)
  {
    return get_equal_const_item_datetime(thd, ctx, const_item);
  }
  uint size_of() const { return sizeof(*this); }
};


/**
  Abstract class for:
  - DATETIME(1..6)
  - DATETIME(0..6) - MySQL56 version
*/
class Field_datetime_with_dec :public Field_datetime {
protected:
  uint dec;
public:
  Field_datetime_with_dec(uchar *ptr_arg, uchar *null_ptr_arg,
                          uchar null_bit_arg, enum utype unireg_check_arg,
                          const char *field_name_arg, uint dec_arg)
    :Field_datetime(ptr_arg, MAX_DATETIME_WIDTH + dec_arg + MY_TEST(dec_arg),
                    null_ptr_arg, null_bit_arg, unireg_check_arg,
                    field_name_arg), dec(dec_arg)
  {
    DBUG_ASSERT(dec <= TIME_SECOND_PART_DIGITS);
  }
  uint decimals() const { return dec; }
  enum ha_base_keytype key_type() const { return HA_KEYTYPE_BINARY; }
  void make_field(Send_field *field);
  bool send_binary(Protocol *protocol);
  uchar *pack(uchar *to, const uchar *from, uint max_length)
  { return Field::pack(to, from, max_length); }
  const uchar *unpack(uchar* to, const uchar *from, const uchar *from_end,
                      uint param_data)
  { return Field::unpack(to, from, from_end, param_data); }
  void sort_string(uchar *to, uint length)
  {
    DBUG_ASSERT(length == pack_length());
    memcpy(to, ptr, length);
  }
  double val_real(void);
  longlong val_int(void);
  String *val_str(String*,String *);
};


/**
  DATETIME(1..6)
*/
class Field_datetime_hires :public Field_datetime_with_dec {
  void store_TIME(MYSQL_TIME *ltime);
  bool get_TIME(MYSQL_TIME *ltime, const uchar *pos, ulonglong fuzzydate) const;
public:
  Field_datetime_hires(uchar *ptr_arg, uchar *null_ptr_arg,
                       uchar null_bit_arg, enum utype unireg_check_arg,
                       const char *field_name_arg, uint dec_arg)
    :Field_datetime_with_dec(ptr_arg, null_ptr_arg, null_bit_arg,
                             unireg_check_arg, field_name_arg, dec_arg)
  {
    DBUG_ASSERT(dec);
  }
  int cmp(const uchar *,const uchar *);
  uint32 pack_length() const;
  bool get_date(MYSQL_TIME *ltime, ulonglong fuzzydate)
  { return Field_datetime_hires::get_TIME(ltime, ptr, fuzzydate); }
  uint size_of() const { return sizeof(*this); }
};


/**
  DATETIME(0..6) - MySQL56 version
*/
class Field_datetimef :public Field_datetime_with_dec {
  void store_TIME(MYSQL_TIME *ltime);
  bool get_TIME(MYSQL_TIME *ltime, const uchar *pos, ulonglong fuzzydate) const;
  int do_save_field_metadata(uchar *metadata_ptr)
  {
    *metadata_ptr= decimals();
    return 1;
  }
public:
  Field_datetimef(uchar *ptr_arg, uchar *null_ptr_arg,
                  uchar null_bit_arg, enum utype unireg_check_arg,
                  const char *field_name_arg, uint dec_arg)
    :Field_datetime_with_dec(ptr_arg, null_ptr_arg, null_bit_arg,
                             unireg_check_arg, field_name_arg, dec_arg)
  {}
  enum_field_types real_type() const { return MYSQL_TYPE_DATETIME2; }
  enum_field_types binlog_type() const { return MYSQL_TYPE_DATETIME2; }
  uint32 pack_length() const
  {
    return my_datetime_binary_length(dec);
  }
  uint row_pack_length() const { return pack_length(); }
  uint pack_length_from_metadata(uint field_metadata)
  {
    DBUG_ENTER("Field_datetimef::pack_length_from_metadata");
    uint tmp= my_datetime_binary_length(field_metadata);
    DBUG_RETURN(tmp);
  }
  int cmp(const uchar *a_ptr, const uchar *b_ptr)
  {
    return memcmp(a_ptr, b_ptr, pack_length());
  }
  int reset();
  bool get_date(MYSQL_TIME *ltime, ulonglong fuzzydate)
  { return Field_datetimef::get_TIME(ltime, ptr, fuzzydate); }
  uint size_of() const { return sizeof(*this); }
};


static inline Field_timestamp *
new_Field_timestamp(MEM_ROOT *root,uchar *ptr, uchar *null_ptr, uchar null_bit,
                    enum Field::utype unireg_check, const char *field_name,
                    TABLE_SHARE *share, uint dec)
{
  if (dec==0)
    return new (root)
      Field_timestamp(ptr, MAX_DATETIME_WIDTH, null_ptr,
                      null_bit, unireg_check, field_name, share);
  if (dec >= FLOATING_POINT_DECIMALS)
    dec= MAX_DATETIME_PRECISION;
  return new (root)
    Field_timestamp_hires(ptr, null_ptr, null_bit, unireg_check,
                          field_name, share, dec);
}

static inline Field_time *
new_Field_time(MEM_ROOT *root, uchar *ptr, uchar *null_ptr, uchar null_bit,
               enum Field::utype unireg_check, const char *field_name,
               uint dec)
{
  if (dec == 0)
    return new (root)
      Field_time(ptr, MIN_TIME_WIDTH, null_ptr, null_bit, unireg_check,
                 field_name);
  if (dec >= FLOATING_POINT_DECIMALS)
    dec= MAX_DATETIME_PRECISION;
  return new (root)
    Field_time_hires(ptr, null_ptr, null_bit, unireg_check, field_name, dec);
}

static inline Field_datetime *
new_Field_datetime(MEM_ROOT *root, uchar *ptr, uchar *null_ptr, uchar null_bit,
                   enum Field::utype unireg_check,
                   const char *field_name, uint dec)
{
  if (dec == 0)
    return new (root)
      Field_datetime(ptr, MAX_DATETIME_WIDTH, null_ptr, null_bit,
                     unireg_check, field_name);
  if (dec >= FLOATING_POINT_DECIMALS)
    dec= MAX_DATETIME_PRECISION;
  return new (root)
    Field_datetime_hires(ptr, null_ptr, null_bit,
                         unireg_check, field_name, dec);
}

class Field_string :public Field_longstr {
  class Warn_filter_string: public Warn_filter
  {
  public:
    Warn_filter_string(const THD *thd, const Field_string *field);
  };
public:
  bool can_alter_field_type;
  Field_string(uchar *ptr_arg, uint32 len_arg,uchar *null_ptr_arg,
	       uchar null_bit_arg,
	       enum utype unireg_check_arg, const char *field_name_arg,
	       CHARSET_INFO *cs)
    :Field_longstr(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
                   unireg_check_arg, field_name_arg, cs),
     can_alter_field_type(1) {};
  Field_string(uint32 len_arg,bool maybe_null_arg, const char *field_name_arg,
               CHARSET_INFO *cs)
    :Field_longstr((uchar*) 0, len_arg, maybe_null_arg ? (uchar*) "": 0, 0,
                   NONE, field_name_arg, cs),
     can_alter_field_type(1) {};

  enum_field_types type() const
  {
    return ((can_alter_field_type && orig_table &&
             orig_table->s->db_create_options & HA_OPTION_PACK_RECORD &&
	     field_length >= 4) &&
            orig_table->s->frm_version < FRM_VER_TRUE_VARCHAR ?
	    MYSQL_TYPE_VAR_STRING : MYSQL_TYPE_STRING);
  }
  enum ha_base_keytype key_type() const
    { return binary() ? HA_KEYTYPE_BINARY : HA_KEYTYPE_TEXT; }
  bool zero_pack() const { return 0; }
  Copy_func *get_copy_func(const Field *from) const;
  int reset(void)
  {
    charset()->cset->fill(charset(),(char*) ptr, field_length,
                          (has_charset() ? ' ' : 0));
    return 0;
  }
  int store(const char *to,uint length,CHARSET_INFO *charset);
  int store(longlong nr, bool unsigned_val);
  int store(double nr) { return Field_str::store(nr); } /* QQ: To be deleted */
  double val_real(void);
  longlong val_int(void);
  String *val_str(String*,String *);
  my_decimal *val_decimal(my_decimal *);
  int cmp(const uchar *,const uchar *);
  void sort_string(uchar *buff,uint length);
  void sql_type(String &str) const;
  void sql_rpl_type(String*) const;
  virtual uchar *pack(uchar *to, const uchar *from,
                      uint max_length);
  virtual const uchar *unpack(uchar* to, const uchar *from,
                              const uchar *from_end,uint param_data);
  uint pack_length_from_metadata(uint field_metadata)
  {
    DBUG_PRINT("debug", ("field_metadata: 0x%04x", field_metadata));
    if (field_metadata == 0)
      return row_pack_length();
    return (((field_metadata >> 4) & 0x300) ^ 0x300) + (field_metadata & 0x00ff);
  }
  bool compatible_field_size(uint field_metadata, Relay_log_info *rli,
                             uint16 mflags, int *order_var);
  uint row_pack_length() const { return field_length; }
  int pack_cmp(const uchar *a,const uchar *b,uint key_length,
               bool insert_or_update);
  int pack_cmp(const uchar *b,uint key_length,bool insert_or_update);
  uint packed_col_length(const uchar *to, uint length);
  uint max_packed_col_length(uint max_length);
  uint size_of() const { return sizeof(*this); }
  enum_field_types real_type() const { return MYSQL_TYPE_STRING; }
  bool has_charset(void) const
  { return charset() == &my_charset_bin ? FALSE : TRUE; }
  Field *make_new_field(MEM_ROOT *root, TABLE *new_table, bool keep_type);
  virtual uint get_key_image(uchar *buff,uint length, imagetype type);
  sql_mode_t value_depends_on_sql_mode() const;
  sql_mode_t can_handle_sql_mode_dependency_on_store() const;
private:
  int do_save_field_metadata(uchar *first_byte);
};


class Field_varstring :public Field_longstr {
public:
  uchar *get_data() const
  {
    return ptr + length_bytes;
  }
  uint get_length() const
  {
    return length_bytes == 1 ? (uint) *ptr : uint2korr(ptr);
  }
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
		  enum utype unireg_check_arg, const char *field_name_arg,
		  TABLE_SHARE *share, CHARSET_INFO *cs)
    :Field_longstr(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
                   unireg_check_arg, field_name_arg, cs),
     length_bytes(length_bytes_arg)
  {
    share->varchar_fields++;
  }
  Field_varstring(uint32 len_arg,bool maybe_null_arg,
                  const char *field_name_arg,
                  TABLE_SHARE *share, CHARSET_INFO *cs)
    :Field_longstr((uchar*) 0,len_arg, maybe_null_arg ? (uchar*) "": 0, 0,
                   NONE, field_name_arg, cs),
     length_bytes(len_arg < 256 ? 1 :2)
  {
    share->varchar_fields++;
  }

  enum_field_types type() const { return MYSQL_TYPE_VARCHAR; }
  enum ha_base_keytype key_type() const;
  uint row_pack_length() const { return field_length; }
  bool zero_pack() const { return 0; }
  int  reset(void) { bzero(ptr,field_length+length_bytes); return 0; }
  uint32 pack_length() const { return (uint32) field_length+length_bytes; }
  uint32 key_length() const { return (uint32) field_length; }
  uint32 sort_length() const
  {
    return (uint32) field_length + (field_charset == &my_charset_bin ?
                                    length_bytes : 0);
  }
  Copy_func *get_copy_func(const Field *from) const;
  bool memcpy_field_possible(const Field *from) const
  {
    return Field_str::memcpy_field_possible(from) &&
           length_bytes == ((Field_varstring*) from)->length_bytes;
  }
  int  store(const char *to,uint length,CHARSET_INFO *charset);
  int  store(longlong nr, bool unsigned_val);
  int  store(double nr) { return Field_str::store(nr); } /* QQ: To be deleted */
#ifdef HAVE_valgrind_or_MSAN
  void mark_unused_memory_as_defined();
#endif /* HAVE_valgrind_or_MSAN */
  double val_real(void);
  longlong val_int(void);
  String *val_str(String*,String *);
  my_decimal *val_decimal(my_decimal *);
  int cmp(const uchar *a,const uchar *b);
  int cmp_prefix(const uchar *a, const uchar *b, size_t prefix_len);
  void sort_string(uchar *buff,uint length);
  uint get_key_image(uchar *buff,uint length, imagetype type);
  void set_key_image(const uchar *buff,uint length);
  void sql_type(String &str) const;
  void sql_rpl_type(String*) const;
  virtual uchar *pack(uchar *to, const uchar *from, uint max_length);
  virtual const uchar *unpack(uchar* to, const uchar *from,
                              const uchar *from_end, uint param_data);
  int cmp_binary(const uchar *a,const uchar *b, uint32 max_length=~0U);
  int key_cmp(const uchar *,const uchar*);
  int key_cmp(const uchar *str, uint length);
  uint packed_col_length(const uchar *to, uint length);
  uint max_packed_col_length(uint max_length);
  uint32 data_length();
  uint size_of() const { return sizeof(*this); }
  enum_field_types real_type() const { return MYSQL_TYPE_VARCHAR; }
  bool has_charset(void) const
  { return charset() == &my_charset_bin ? FALSE : TRUE; }
  Field *make_new_field(MEM_ROOT *root, TABLE *new_table, bool keep_type);
  Field *new_key_field(MEM_ROOT *root, TABLE *new_table,
                       uchar *new_ptr, uint32 length,
                       uchar *new_null_ptr, uint new_null_bit);
  uint is_equal(Create_field *new_field);
  void hash(ulong *nr, ulong *nr2);
  uint length_size() { return length_bytes; }
private:
  int do_save_field_metadata(uchar *first_byte);
};


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
public:
  Field_blob(uchar *ptr_arg, uchar *null_ptr_arg, uchar null_bit_arg,
	     enum utype unireg_check_arg, const char *field_name_arg,
	     TABLE_SHARE *share, uint blob_pack_length, CHARSET_INFO *cs);
  Field_blob(uint32 len_arg,bool maybe_null_arg, const char *field_name_arg,
             CHARSET_INFO *cs)
    :Field_longstr((uchar*) 0, len_arg, maybe_null_arg ? (uchar*) "": 0, 0,
                   NONE, field_name_arg, cs),
    packlength(4)
  {
    flags|= BLOB_FLAG;
  }
  Field_blob(uint32 len_arg,bool maybe_null_arg, const char *field_name_arg,
	     CHARSET_INFO *cs, bool set_packlength)
    :Field_longstr((uchar*) 0,len_arg, maybe_null_arg ? (uchar*) "": 0, 0,
                   NONE, field_name_arg, cs)
  {
    flags|= BLOB_FLAG;
    packlength= 4;
    if (set_packlength)
    {
      packlength= len_arg <= 255 ? 1 :
                  len_arg <= 65535 ? 2 :
                  len_arg <= 16777215 ? 3 : 4;
    }
  }
  Field_blob(uint32 packlength_arg)
    :Field_longstr((uchar*) 0, 0, (uchar*) "", 0, NONE, "temp", system_charset_info),
    packlength(packlength_arg) {}
  /* Note that the default copy constructor is used, in clone() */
  enum_field_types type() const { return MYSQL_TYPE_BLOB;}
  enum ha_base_keytype key_type() const
    { return binary() ? HA_KEYTYPE_VARBINARY2 : HA_KEYTYPE_VARTEXT2; }
  Copy_func *get_copy_func(const Field *from) const
  {
    /*
    TODO: MDEV-9331
    if (from->type() == MYSQL_TYPE_BIT)
      return do_field_int;
    */
    if (!(from->flags & BLOB_FLAG) || from->charset() != charset())
      return do_conv_blob;
    if (from->pack_length() != Field_blob::pack_length())
      return do_copy_blob;
    return get_identical_copy_func();
  }
  int  store_field(Field *from)
  {                                             // Be sure the value is stored
    from->val_str(&value);
    if (table->copy_blobs ||
        (!value.is_alloced() && from->is_varchar_and_in_write_set()))
      value.copy();
    return store(value.ptr(), value.length(), from->charset());
  }
  bool memcpy_field_possible(const Field *from) const
  {
    return Field_str::memcpy_field_possible(from) &&
           !table->copy_blobs;
  }
  int  store(const char *to,uint length,CHARSET_INFO *charset);
  int  store(double nr);
  int  store(longlong nr, bool unsigned_val);
  double val_real(void);
  longlong val_int(void);
  String *val_str(String*,String *);
  my_decimal *val_decimal(my_decimal *);
  int cmp(const uchar *a,const uchar *b);
  int cmp_prefix(const uchar *a, const uchar *b, size_t prefix_len);
  int cmp(const uchar *a, uint32 a_length, const uchar *b, uint32 b_length);
  int cmp_binary(const uchar *a,const uchar *b, uint32 max_length=~0U);
  int key_cmp(const uchar *,const uchar*);
  int key_cmp(const uchar *str, uint length);
  /* Never update the value of min_val for a blob field */
  bool update_min(Field *min_val, bool force_update) { return FALSE; }
  /* Never update the value of max_val for a blob field */
  bool update_max(Field *max_val, bool force_update) { return FALSE; }
  uint32 key_length() const { return 0; }
  void sort_string(uchar *buff,uint length);
  uint32 pack_length() const
  { return (uint32) (packlength + portable_sizeof_char_ptr); }

  /**
     Return the packed length without the pointer size added. 

     This is used to determine the size of the actual data in the row
     buffer.

     @returns The length of the raw data itself without the pointer.
  */
  uint32 pack_length_no_ptr() const
  { return (uint32) (packlength); }
  uint row_pack_length() const { return pack_length_no_ptr(); }
  uint32 sort_length() const;
  uint32 value_length() { return get_length(); }
  virtual uint32 max_data_length() const
  {
    return (uint32) (((ulonglong) 1 << (packlength*8)) -1);
  }
  int reset(void) { bzero(ptr, packlength+sizeof(uchar*)); return 0; }
  void reset_fields() { bzero((uchar*) &value,sizeof(value)); bzero((uchar*) &read_value,sizeof(read_value)); }
  uint32 get_field_buffer_size(void) { return value.alloced_length(); }
  void store_length(uchar *i_ptr, uint i_packlength, uint32 i_number);
  inline void store_length(uint32 number)
  {
    store_length(ptr, packlength, number);
  }
  inline uint32 get_length(my_ptrdiff_t row_offset= 0) const
  { return get_length(ptr+row_offset, this->packlength); }
  uint32 get_length(const uchar *ptr, uint packlength) const;
  uint32 get_length(const uchar *ptr_arg) const
  { return get_length(ptr_arg, this->packlength); }
  inline uchar *get_ptr() const { return get_ptr(0); }
  inline uchar *get_ptr(my_ptrdiff_t row_offset) const
  {
    uchar *s;
    memcpy(&s, ptr + packlength + row_offset, sizeof(uchar*));
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
  uint get_key_image(uchar *buff,uint length, imagetype type);
  void set_key_image(const uchar *buff,uint length);
  Field *new_key_field(MEM_ROOT *root, TABLE *new_table,
                       uchar *new_ptr, uint32 length,
                       uchar *new_null_ptr, uint new_null_bit);
  void sql_type(String &str) const;
  /**
     Copy blob buffer into internal storage "value" and update record pointer.

     @retval true     Memory allocation error
     @retval false    Success
  */
  inline bool copy()
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
  virtual uchar *pack(uchar *to, const uchar *from, uint max_length);
  virtual const uchar *unpack(uchar *to, const uchar *from,
                              const uchar *from_end, uint param_data);
  uint packed_col_length(const uchar *col_ptr, uint length);
  uint max_packed_col_length(uint max_length);
  void free()
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
  uint size_of() const { return sizeof(*this); }
  bool has_charset(void) const
  { return charset() == &my_charset_bin ? FALSE : TRUE; }
  uint32 max_display_length();
  uint32 char_length() const;
  uint is_equal(Create_field *new_field);

  friend void TABLE::remember_blob_values(String *blob_storage);
  friend void TABLE::restore_blob_values(String *blob_storage);

private:
  int do_save_field_metadata(uchar *first_byte);
};


#ifdef HAVE_SPATIAL
class Field_geom :public Field_blob {
public:
  enum geometry_type geom_type;
  uint srid;
  uint precision;
  enum storage_type { GEOM_STORAGE_WKB= 0, GEOM_STORAGE_BINARY= 1};
  enum storage_type storage;

  Field_geom(uchar *ptr_arg, uchar *null_ptr_arg, uint null_bit_arg,
	     enum utype unireg_check_arg, const char *field_name_arg,
	     TABLE_SHARE *share, uint blob_pack_length,
	     enum geometry_type geom_type_arg, uint field_srid)
     :Field_blob(ptr_arg, null_ptr_arg, null_bit_arg, unireg_check_arg, 
                 field_name_arg, share, blob_pack_length, &my_charset_bin)
  { geom_type= geom_type_arg; srid= field_srid; }
  Field_geom(uint32 len_arg,bool maybe_null_arg, const char *field_name_arg,
	     TABLE_SHARE *share, enum geometry_type geom_type_arg)
    :Field_blob(len_arg, maybe_null_arg, field_name_arg, &my_charset_bin)
  { geom_type= geom_type_arg; srid= 0; }
  enum ha_base_keytype key_type() const { return HA_KEYTYPE_VARBINARY2; }
  enum_field_types type() const { return MYSQL_TYPE_GEOMETRY; }
  bool can_optimize_range(const Item_bool_func *cond,
                                  const Item *item,
                                  bool is_eq_func) const;
  void sql_type(String &str) const;
  uint is_equal(Create_field *new_field);
  int  store(const char *to, uint length, CHARSET_INFO *charset);
  int  store(double nr);
  int  store(longlong nr, bool unsigned_val);
  int  store_decimal(const my_decimal *);
  uint size_of() const { return sizeof(*this); }
  /**
   Key length is provided only to support hash joins. (compared byte for byte)
   Ex: SELECT .. FROM t1,t2 WHERE t1.field_geom1=t2.field_geom2.

   The comparison is not very relevant, as identical geometry might be
   represented differently, but we need to support it either way.
  */
  uint32 key_length() const { return packlength; }

  /**
    Non-nullable GEOMETRY types cannot have defaults,
    but the underlying blob must still be reset.
   */
  int reset(void) { return Field_blob::reset() || !maybe_null(); }
  bool load_data_set_null(THD *thd);
  bool load_data_set_no_data(THD *thd, bool fixed_format);

  geometry_type get_geometry_type() { return geom_type; };
  static geometry_type geometry_type_merge(geometry_type, geometry_type);
  uint get_srid() { return srid; }
};

uint gis_field_options_image(uchar *buff, List<Create_field> &create_fields);
uint gis_field_options_read(const uchar *buf, uint buf_len,
      Field_geom::storage_type *st_type,uint *precision, uint *scale, uint *srid);

#endif /*HAVE_SPATIAL*/


class Field_enum :public Field_str {
  static void do_field_enum(Copy_field *copy_field);
  bool can_optimize_range_or_keypart_ref(const Item_bool_func *cond,
                                         const Item *item) const;
protected:
  uint packlength;
public:
  TYPELIB *typelib;
  Field_enum(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
             uchar null_bit_arg,
             enum utype unireg_check_arg, const char *field_name_arg,
             uint packlength_arg,
             TYPELIB *typelib_arg,
             CHARSET_INFO *charset_arg)
    :Field_str(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
	       unireg_check_arg, field_name_arg, charset_arg),
    packlength(packlength_arg),typelib(typelib_arg)
  {
      flags|=ENUM_FLAG;
  }
  Field *make_new_field(MEM_ROOT *root, TABLE *new_table, bool keep_type);
  enum_field_types type() const { return MYSQL_TYPE_STRING; }
  enum Item_result cmp_type () const { return INT_RESULT; }
  enum ha_base_keytype key_type() const;
  sql_mode_t can_handle_sql_mode_dependency_on_store() const;
  Copy_func *get_copy_func(const Field *from) const
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
  int store_field(Field *from)
  {
    if (from->real_type() == MYSQL_TYPE_ENUM && from->val_int() == 0)
    {
      store_type(0);
      return 0;
    }
    return from->save_in_field(this);
  }
  int save_in_field(Field *to)
  {
    if (to->result_type() != STRING_RESULT)
      return to->store(val_int(), 0);
    return save_in_field_str(to);
  }
  bool memcpy_field_possible(const Field *from) const { return false; }
  int  store(const char *to,uint length,CHARSET_INFO *charset);
  int  store(double nr);
  int  store(longlong nr, bool unsigned_val);
  double val_real(void);
  longlong val_int(void);
  String *val_str(String*,String *);
  int cmp(const uchar *,const uchar *);
  void sort_string(uchar *buff,uint length);
  uint32 pack_length() const { return (uint32) packlength; }
  void store_type(ulonglong value);
  void sql_type(String &str) const;
  uint size_of() const { return sizeof(*this); }
  enum_field_types real_type() const { return MYSQL_TYPE_ENUM; }
  uint pack_length_from_metadata(uint field_metadata)
  { return (field_metadata & 0x00ff); }
  uint row_pack_length() const { return pack_length(); }
  virtual bool zero_pack() const { return 0; }
  bool optimize_range(uint idx, uint part) { return 0; }
  bool eq_def(const Field *field) const;
  bool has_charset(void) const { return TRUE; }
  /* enum and set are sorted as integers */
  CHARSET_INFO *sort_charset(void) const { return &my_charset_bin; }
  uint decimals() const { return 0; }

  virtual uchar *pack(uchar *to, const uchar *from, uint max_length);
  virtual const uchar *unpack(uchar *to, const uchar *from,
                              const uchar *from_end, uint param_data);

  bool can_optimize_keypart_ref(const Item_bool_func *cond,
                                const Item *item) const
  {
    return can_optimize_range_or_keypart_ref(cond, item);
  }
  bool can_optimize_group_min_max(const Item_bool_func *cond,
                                  const Item *const_item) const
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
                          bool is_eq_func) const
  {
    return can_optimize_range_or_keypart_ref(cond, item);
  }
private:
  int do_save_field_metadata(uchar *first_byte);
  uint is_equal(Create_field *new_field);
};


class Field_set :public Field_enum {
public:
  Field_set(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
	    uchar null_bit_arg,
	    enum utype unireg_check_arg, const char *field_name_arg,
	    uint32 packlength_arg,
	    TYPELIB *typelib_arg, CHARSET_INFO *charset_arg)
    :Field_enum(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
		    unireg_check_arg, field_name_arg,
                packlength_arg,
                typelib_arg,charset_arg),
      empty_set_string("", 0, charset_arg)
    {
      flags=(flags & ~ENUM_FLAG) | SET_FLAG;
    }
  int  store_field(Field *from) { return from->save_in_field(this); }
  int  store(const char *to,uint length,CHARSET_INFO *charset);
  int  store(double nr) { return Field_set::store((longlong) nr, FALSE); }
  int  store(longlong nr, bool unsigned_val);

  virtual bool zero_pack() const { return 1; }
  String *val_str(String*,String *);
  void sql_type(String &str) const;
  uint size_of() const { return sizeof(*this); }
  enum_field_types real_type() const { return MYSQL_TYPE_SET; }
  bool has_charset(void) const { return TRUE; }
private:
  const String empty_set_string;
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
            enum utype unireg_check_arg, const char *field_name_arg);
  enum_field_types type() const { return MYSQL_TYPE_BIT; }
  enum ha_base_keytype key_type() const { return HA_KEYTYPE_BIT; }
  uint32 key_length() const { return (uint32) (field_length + 7) / 8; }
  uint32 max_data_length() const { return (field_length + 7) / 8; }
  uint32 max_display_length() { return field_length; }
  uint size_of() const { return sizeof(*this); }
  Item_result result_type () const { return INT_RESULT; }
  int reset(void) { 
    bzero(ptr, bytes_in_rec); 
    if (bit_ptr && (bit_len > 0))  // reset odd bits among null bits
      clr_rec_bits(bit_ptr, bit_ofs, bit_len);
    return 0; 
  }
  Copy_func *get_copy_func(const Field *from) const
  {
    if (from->cmp_type() == DECIMAL_RESULT)
      return do_field_decimal;
    return do_field_int;
  }
  int save_in_field(Field *to) { return to->store(val_int(), true); }
  bool memcpy_field_possible(const Field *from) const { return false; }
  int store(const char *to, uint length, CHARSET_INFO *charset);
  int store(double nr);
  int store(longlong nr, bool unsigned_val);
  int store_decimal(const my_decimal *);
  double val_real(void);
  longlong val_int(void);
  String *val_str(String*, String *);
  virtual bool str_needs_quotes() { return TRUE; }
  my_decimal *val_decimal(my_decimal *);
  bool val_bool() { return val_int() != 0; }
  int cmp(const uchar *a, const uchar *b)
  {
    DBUG_ASSERT(ptr == a || ptr == b);
    if (ptr == a)
      return Field_bit::key_cmp(b, bytes_in_rec + MY_TEST(bit_len));
    else
      return Field_bit::key_cmp(a, bytes_in_rec + MY_TEST(bit_len)) * -1;
  }
  int cmp_binary_offset(uint row_offset)
  { return cmp_offset(row_offset); }
  int cmp_prefix(const uchar *a, const uchar *b, size_t prefix_len);
  int key_cmp(const uchar *a, const uchar *b)
  { return cmp_binary((uchar *) a, (uchar *) b); }
  int key_cmp(const uchar *str, uint length);
  int cmp_offset(uint row_offset);
  bool update_min(Field *min_val, bool force_update)
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
  bool update_max(Field *max_val, bool force_update)
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
  void store_field_value(uchar *val, uint len)
  {
    store(*((longlong *)val), TRUE);
  }
  double pos_in_interval(Field *min, Field *max)
  {
    return pos_in_interval_val_real(min, max);
  }
  void get_image(uchar *buff, uint length, CHARSET_INFO *cs)
  { get_key_image(buff, length, itRAW); }   
  void set_image(const uchar *buff,uint length, CHARSET_INFO *cs)
  { Field_bit::store((char *) buff, length, cs); }
  uint get_key_image(uchar *buff, uint length, imagetype type);
  void set_key_image(const uchar *buff, uint length)
  { Field_bit::store((char*) buff, length, &my_charset_bin); }
  void sort_string(uchar *buff, uint length)
  { get_key_image(buff, length, itRAW); }
  uint32 pack_length() const { return (uint32) (field_length + 7) / 8; }
  uint32 pack_length_in_rec() const { return bytes_in_rec; }
  uint pack_length_from_metadata(uint field_metadata);
  uint row_pack_length() const
  { return (bytes_in_rec + ((bit_len > 0) ? 1 : 0)); }
  bool compatible_field_size(uint metadata, Relay_log_info *rli,
                             uint16 mflags, int *order_var);
  void sql_type(String &str) const;
  virtual uchar *pack(uchar *to, const uchar *from, uint max_length);
  virtual const uchar *unpack(uchar *to, const uchar *from,
                              const uchar *from_end, uint param_data);
  virtual int set_default();

  Field *new_key_field(MEM_ROOT *root, TABLE *new_table,
                       uchar *new_ptr, uint32 length,
                       uchar *new_null_ptr, uint new_null_bit);
  void set_bit_ptr(uchar *bit_ptr_arg, uchar bit_ofs_arg)
  {
    bit_ptr= bit_ptr_arg;
    bit_ofs= bit_ofs_arg;
  }
  bool eq(Field *field)
  {
    return (Field::eq(field) &&
            bit_ptr == ((Field_bit *)field)->bit_ptr &&
            bit_ofs == ((Field_bit *)field)->bit_ofs);
  }
  uint is_equal(Create_field *new_field);
  void move_field_offset(my_ptrdiff_t ptr_diff)
  {
    Field::move_field_offset(ptr_diff);
    bit_ptr= ADD_TO_PTR(bit_ptr, ptr_diff, uchar*);
  }
  void hash(ulong *nr, ulong *nr2);

private:
  virtual size_t do_last_null_byte() const;
  int do_save_field_metadata(uchar *first_byte);
};


/**
  BIT field represented as chars for non-MyISAM tables.

  @todo The inheritance relationship is backwards since Field_bit is
  an extended version of Field_bit_as_char and not the other way
  around. Hence, we should refactor it to fix the hierarchy order.
 */
class Field_bit_as_char: public Field_bit {
public:
  Field_bit_as_char(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
                    uchar null_bit_arg,
                    enum utype unireg_check_arg, const char *field_name_arg);
  enum ha_base_keytype key_type() const { return HA_KEYTYPE_BINARY; }
  uint size_of() const { return sizeof(*this); }
  int store(const char *to, uint length, CHARSET_INFO *charset);
  int store(double nr) { return Field_bit::store(nr); }
  int store(longlong nr, bool unsigned_val)
  { return Field_bit::store(nr, unsigned_val); }
  void sql_type(String &str) const;
};


extern const LEX_STRING null_lex_str;


Field *make_field(TABLE_SHARE *share, MEM_ROOT *mem_root,
                  uchar *ptr, uint32 field_length,
                  uchar *null_pos, uchar null_bit,
                  uint pack_flag, enum_field_types field_type,
                  CHARSET_INFO *cs,
                  Field::geometry_type geom_type, uint srid,
                  Field::utype unireg_check,
                  TYPELIB *interval, const char *field_name);

/*
  Create field class for CREATE TABLE
*/
class Column_definition: public Sql_alloc
{
public:
  const char *field_name;
  LEX_STRING comment;			// Comment for field
  Item *on_update;		        // ON UPDATE NOW()
  enum	enum_field_types sql_type;
  /*
    At various stages in execution this can be length of field in bytes or
    max number of characters. 
  */
  ulonglong length;
  /*
    The value of `length' as set by parser: is the number of characters
    for most of the types, or of bytes for BLOBs or numeric types.
  */
  uint32 char_length;
  uint  decimals, flags, pack_length, key_length;
  Field::utype unireg_check;
  TYPELIB *interval;			// Which interval to use
  List<String> interval_list;
  CHARSET_INFO *charset;
  uint32 srid;
  Field::geometry_type geom_type;
  engine_option_value *option_list;

  uint pack_flag;

  /*
    This is additinal data provided for any computed(virtual) field.
    In particular it includes a pointer to the item by  which this field
    can be computed from other fields.
  */
  Virtual_column_info
    *vcol_info,                      // Virtual field
    *default_value,                  // Default value
    *check_constraint;               // Check constraint

  Column_definition():
    comment(null_lex_str),
    on_update(0), sql_type(MYSQL_TYPE_NULL),
    flags(0), pack_length(0), key_length(0), unireg_check(Field::NONE),
    interval(0), srid(0), geom_type(Field::GEOM_GEOMETRY),
    option_list(NULL),
    vcol_info(0), default_value(0), check_constraint(0)
  {
    interval_list.empty();
  }

  Column_definition(THD *thd, Field *field, Field *orig_field);
  void create_length_to_internal_length(void);

  bool check(THD *thd);

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
                    uchar *ptr, uchar *null_pos, uchar null_bit,
                    const char *field_name_arg) const
  {
    return ::make_field(share, mem_root, ptr,
                        (uint32)length, null_pos, null_bit,
                        pack_flag, sql_type, charset,
                        geom_type, srid, unireg_check, interval,
                        field_name_arg);
  }
  Field *make_field(TABLE_SHARE *share, MEM_ROOT *mem_root,
                    const char *field_name_arg)
  {
    return make_field(share, mem_root, (uchar *) 0, (uchar *) "", 0,
                      field_name_arg);
  }
  /* Return true if default is an expression that must be saved explicitely */
  bool has_default_expression();

  bool has_default_now_unireg_check() const
  {
    return unireg_check == Field::TIMESTAMP_DN_FIELD
        || unireg_check == Field::TIMESTAMP_DNUN_FIELD;
  }
};


class Create_field :public Column_definition
{
public:
  const char *change;			// If done with alter table
  const char *after;			// Put column after this one
  Field *field;				// For alter table
  TYPELIB *save_interval;               // Temporary copy for the above
                                        // Used only for UCS2 intervals

  /** structure with parsed options (for comparing fields in ALTER TABLE) */
  ha_field_option_struct *option_struct;
  uint	offset;
  uint8 interval_id;                    // For rea_create_table
  bool create_if_not_exists;            // Used in ALTER TABLE IF NOT EXISTS

  Create_field():
    Column_definition(), change(0), after(0),
    field(0), option_struct(NULL),
    create_if_not_exists(false)
  { }
  Create_field(THD *thd, Field *old_field, Field *orig_field):
    Column_definition(thd, old_field, orig_field),
    change(old_field->field_name), after(0),
    field(old_field), option_struct(old_field->option_struct),
    create_if_not_exists(false)
  { }
  /* Used to make a clone of this object for ALTER/CREATE TABLE */
  Create_field *clone(MEM_ROOT *mem_root) const;
};


/*
  A class for sending info to the client
*/

class Send_field :public Sql_alloc {
 public:
  const char *db_name;
  const char *table_name,*org_table_name;
  const char *col_name,*org_col_name;
  ulong length;
  uint charsetnr, flags, decimals;
  enum_field_types type;
  Send_field() {}
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
uint32 calc_pack_length(enum_field_types type,uint32 length);
int set_field_to_null(Field *field);
int set_field_to_null_with_conversions(Field *field, bool no_conversions);
int convert_null_to_field_value_or_error(Field *field);
bool check_expression(Virtual_column_info *vcol, const char *name,
                      enum_vcol_info_type type);

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

#define MTYP_TYPENR(type) (type & 127U)	/* Remove bits from type */

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

#endif /* FIELD_INCLUDED */
