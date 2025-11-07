/*
   Copyright (c) 2023-2025, MariaDB Corporation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#define MYSQL_SERVER
#include "my_global.h"
#include "sql_class.h"          // THD
#include "sp_head.h"
#include "mysql/plugin_data_type.h"
#include "mysql/plugin_function.h"
#include "sp_instr.h"
#include "sql_type.h"


static constexpr LEX_CSTRING sys_refcursor_str=
{STRING_WITH_LEN("sys_refcursor")};


class Type_collection_cursor: public Type_collection
{
protected:
  const Type_handler *aggregate_common(const Type_handler *h1,
                                       const Type_handler *h2) const;
public:
  const Type_handler *aggregate_for_result(const Type_handler *h1,
                                           const Type_handler *h2)
                                           const override
  {
    return aggregate_common(h1, h2);
  }
  const Type_handler *aggregate_for_comparison(const Type_handler *h1,
                                               const Type_handler *h2)
                                               const override
  {
    DBUG_ASSERT(h1 == h1->type_handler_for_comparison());
    DBUG_ASSERT(h2 == h2->type_handler_for_comparison());
    return nullptr;
  }
  const Type_handler *aggregate_for_min_max(const Type_handler *h1,
                                            const Type_handler *h2)
                                            const override
  {
    return nullptr;
  }
  const Type_handler *aggregate_for_num_op(const Type_handler *h1,
                                           const Type_handler *h2)
                                           const override
  {
    return nullptr;
  }
};


static Type_collection_cursor type_collection_cursor;


/*
  Basic SYS_REFCURSOR traits
*/
class Sys_refcursor_traits
{
public:

  static const Type_handler *storage_type_handler()
  {
    return &type_handler_ushort;
  }

  static const Type_limits_int & type_limits_int()
  {
    static const Type_limits_uint16 limits;
    return limits;
  }

  static enum_field_types field_type() { return MYSQL_TYPE_SHORT; }

  static uint flags() { return UNSIGNED_FLAG; }

  static protocol_send_type_t protocol_send_type()
  {
    return PROTOCOL_SEND_SHORT;
  }

  static uint32 calc_pack_length()
  {
    return 2;
  }

  static uint32 max_display_length_for_field()
  {
    return 6;
  }

  static uint32 max_display_length()
  {
    return type_limits_int().char_length();
  }

  static uint32 Item_decimal_notation_int_digits()
  {
    // Used in Item_func_format
    return type_limits_int().precision();
  }

  static sp_cursor_array *cursor_array(THD *thd)
  {
    return thd->statement_cursors();
  }
};




class Field_sys_refcursor final :public Field_short,
                                 public Sys_refcursor_traits
{

  int update_to_null(bool no_conversion)
  {
    DBUG_ENTER("Field_sys_refcursor::update_to_null");
    /*
      As SP variables cannot be NOT NULL, it's not needed to call
      set_field_to_null_with_conversions() on updating to NULL.
      set_null() is enough.
    */
    DBUG_ASSERT(real_maybe_null());
    if (!is_null())
    {
      THD *thd= get_thd();
      cursor_array(thd)->ref_count_dec(thd, (ulonglong) val_int());
      set_null();
      reset();
    }
    DBUG_RETURN(0);
  }

  int update_to_not_null_ref(ulonglong ref)
  {
    DBUG_ENTER("Field_sys_refcursor::update_to_not_null");
    THD *thd= get_thd();
    const Type_ref_null old_value= val_ref(thd);
    set_notnull();
    int rc= store((longlong) ref, true/*unsigned*/);
    if (!rc)
      cursor_array(thd)->ref_count_update(thd, old_value, val_ref(thd));
    DBUG_RETURN(rc);
  }

public:
  Field_sys_refcursor(const LEX_CSTRING &name, const Record_addr &addr,
                      enum utype unireg_check_arg, uint32 len_arg)
    :Field_short(addr.ptr(), len_arg, addr.null_ptr(), addr.null_bit(),
                 Field::NONE, &name, false/*zerofill*/, true/*unsigned*/)
  {}
  void sql_type(String &res) const override
  {
    res.set_ascii(sys_refcursor_str.str, sys_refcursor_str.length);
  }
  const Type_handler *type_handler() const override;
  /*
    Field_sys_refcursor has a side effect.
    Cannot use memcpy when copying data from another field.
  */
  bool memcpy_field_possible(const Field *from) const override
  {
    return false;
  }

  /*
    expr_event_handler()

    This method is called at various points in time,
    (for example when an SP execution is leaving a BEGIN..END block),
    when the Field value needs some additional handling other than just
    changing or destructing its value.
    See the definition of expr_event_t for all event types.

    Details about SYS_REFCURSOR implementation
    ------------------------------------------
    Suppose m_statement_cursors.at(0..4) were opened earlier,
    in the upper level BEGIN..END blocks.

    BEGIN
      DECLARE ref1 SYS_RECURSOR;
      -- The OPEN statement below attaches the reference ref1 to
      -- the next available cursor thd->m_statement_cursors.at(5)
      OPEN ref1 FOR SELECT 1;
      BEGIN
        DECLARE ref2 DEFAULT ref1;   -- one more reference to the same cursor
        BEGIN
          DECLARE ref3 DEFAULT ref1; -- one more reference to the same cursor
            -- Here we have these relationships between variables:
            --    ref1==5  ---> +------------------------------------------+
            --    ref2==5  ---> | m_statement_cursors.at(5).m_ref_count==3 |
            --    ref3==5  ---> +------------------------------------------+
          END;
        END;
      END;
    END;

    The referenced object m_statement_cursors.at(5) is not necessarily
    destructed/modified every time when a referece SP variable pointing to the
    object is destructed, as every object can have more than one references
    declared in different BEGIN..END blocks, like in the chart above.

    A call for `expr_event_handler(thd, DESTRUCT*, 5)` detaches the
    reference SP variable (e.g. ref3) from the referenced object
    (e.g. m_statement_cursors.at(5)) by decrementing the reference
    counter in sp_cursor_array_element::m_ref_count.
    When m_ref_count gets down to zero, the sp_cursor_array_element
    instance m_statement_cursors.at(5) gets closed and re-initialized
    for possible new OPEN statements.
  */

  void expr_event_handler(THD *thd, expr_event_t event) override
  {
    if ((bool) (event & expr_event_t::DESTRUCT_ANY))
    {
      update_to_null(false);
      return;
    }
    DBUG_ASSERT(0);
  }

  Type_ref_null val_ref(THD *thd) override
  {
    return is_null() ? Type_ref_null() : Type_ref_null((ulonglong) val_int());
  }

  int store_ref(const Type_ref_null &ref, bool no_conversions) override
  {
    DBUG_ENTER("Field_sys_refcursor::store_ref");
    DBUG_RETURN(ref.is_null() ? update_to_null(no_conversions) :
                                update_to_not_null_ref(ref.value()));
  }

  bool store_item(Item *item)
  {
    THD *thd= get_thd();
    bool rc= store_ref(item->val_ref(thd), true/*no_conversion*/);
    item->expr_event_handler(thd, expr_event_t::DESTRUCT_ASSIGNMENT_RIGHT_HAND);
    return rc;
  }

  bool sp_prepare_and_store_item(THD *thd, Item **value) override
  {
    DBUG_ENTER("Field_sys_refcursor::sp_prepare_and_store_item");
    DBUG_ASSERT(value);
    Item *expr_item;
    bool rc= !(expr_item= thd->sp_fix_func_item_for_assignment(this, value)) ||
             expr_item->check_is_evaluable_expression_or_error() ||
             store_item(expr_item) ||
             thd->is_error();
    DBUG_RETURN(rc);
  }

  Field *make_new_field(MEM_ROOT *root, TABLE *new_table, bool keep_type)
                                                                 override
  {
    my_error(ER_ILLEGAL_PARAMETER_DATA_TYPE_FOR_OPERATION, MYF(0),
             sys_refcursor_str.str, "CREATE TABLE");
    return nullptr;
  }

};


class Type_handler_sys_refcursor final: public Type_handler_int_result,
                                        public Sys_refcursor_traits
{
public:
  static const Type_handler *singleton();

  /*
     Data type features determined by Sys_refcursor_traits.
     If we ever want to change the reference value storage type
     from uint16 to something bigger, nothing in the below code
     should be changed. Only Sys_refcursor_traits should be changed.
  */
  uint32 max_display_length(const Item *item) const override
  {
    return Sys_refcursor_traits::type_limits_int().char_length();
  }
  uint32 Item_decimal_notation_int_digits(const Item *item) const override
  {
    // Used in Item_func_format
    return Sys_refcursor_traits::type_limits_int().precision();
  }

  enum_field_types field_type() const override
  {
    return Sys_refcursor_traits::field_type();
  }

  uint flags() const override
  {
    return Sys_refcursor_traits::flags();
  }

  protocol_send_type_t protocol_send_type() const override
  {
    return Sys_refcursor_traits::protocol_send_type();
  }

  uint32 max_display_length_for_field(const Conv_source &src) const override
  {
    return Sys_refcursor_traits::max_display_length_for_field();
  }

  uint32 calc_pack_length(uint32 length) const  override
  {
    return Sys_refcursor_traits::calc_pack_length();
  }

  bool Item_send(Item *item, Protocol *protocol, st_value *buf) const override
  {
    return storage_type_handler()->Item_send(item, protocol, buf);
  }

  bool Column_definition_fix_attributes(Column_definition *def) const override
  {
    return storage_type_handler()->Column_definition_fix_attributes(def);
  }

  bool Column_definition_prepare_stage2(Column_definition *c,
                                        handler *file,
                                        ulonglong table_flags) const override
  {
    return storage_type_handler()->Column_definition_prepare_stage2(c, file,
                                                                 table_flags);
  }

  /*** Basic data type feautures ***/

  const Type_collection *type_collection() const override
  {
    return &type_collection_cursor;
  }

  const Type_handler *type_handler_for_comparison() const override
  {
    return this;
  }

  bool can_return_bool() const override { return false; }
  bool can_return_int() const override { return false; }
  bool can_return_decimal() const override { return false; }
  bool can_return_real() const override { return false; }
  bool can_return_str() const override { return false; }
  bool can_return_text() const override { return false; }
  bool can_return_date() const override { return false; }
  bool can_return_time() const override { return false; }
  bool can_return_extract_source(interval_type type) const override
  {
    return false;
  }

  bool is_complex() const override
  {
    return true;
  }

  void Item_update_null_value(Item *item) const override
  {
    // This method is used by IS NULL and IS NOT NULL predicates
    item->null_value= item->val_ref(current_thd).is_null();
    item->expr_event_handler(current_thd, expr_event_t::DESTRUCT_ROUTINE_ARG);
  }

  bool Item_eq_value(THD *thd, const Type_cmp_attributes *attr,
                     Item *a, Item *b) const override
  {
    /*
      This method is used to simplify conditions like this:
        WHERE field=const1 AND field=const2
      In case of SYS_REFCURSOR we should never get to here, because:
      - the comparison predicate is disallowed by
        Type_collection_cursor::aggregate_for_comparison()
      - Also Item_sp_variable::const_item() returns false if the item
        has the COMPLEX_DATA_TYPE flag (like SYS_REFCURSOR items).
    */
    DBUG_ASSERT(0);
    return false;
  }

  /*** Field and SP variable related methods ***/

  bool Spvar_definition_with_complex_data_types(Spvar_definition *def)
                                                        const override
  {
    return true;
  }

  int Item_save_in_field(Item *item, Field *field, bool no_conversions)
                                                         const override
  {
    Field_sys_refcursor *fc= dynamic_cast<Field_sys_refcursor*>(field);
    return fc ? fc->store_item(item) :
                item->save_int_in_field(field, no_conversions);
  }

  Field *make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &rec, const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const override
  {
    /*
      Create a Field as a storage for an SP variable.
      Note, creating a field for a real table is prevented in these methods:
      - make_table_field()
      - Column_definition_set_attributes()
    */
    return new (root) Field_sys_refcursor(*name, rec, attr->unireg_check,
                                          (uint32) attr->length);
  }

  Field *make_table_field(MEM_ROOT *root,
                          const LEX_CSTRING *name,
                          const Record_addr &addr,
                          const Type_all_attributes &attr,
                          TABLE_SHARE *share) const override
  {
    // Disallow "CREATE TABLE t1 AS SELECT sys_refcursor_var;"
    my_error(ER_ILLEGAL_PARAMETER_DATA_TYPE_FOR_OPERATION, MYF(0),
             sys_refcursor_str.str, "CREATE TABLE");
    return nullptr;
  }

  bool Column_definition_set_attributes(THD *thd,
                                        Column_definition *def,
                                        const Lex_field_type_st &attr,
                                        column_definition_type_t type)
                                        const override
  {
    // Disallow "CREATE TABLE t1 (a SYS_REFCURSOR)"
    if (type == COLUMN_DEFINITION_TABLE_FIELD)
    {
      my_error(ER_ILLEGAL_PARAMETER_DATA_TYPE_FOR_OPERATION, MYF(0),
               sys_refcursor_str.str, "CREATE TABLE");
      return true;
    }

    DBUG_ASSERT(thd->lex->sphead);
    DBUG_ASSERT(thd->lex->spcont);

    /*
      Oracle returns an error on SYS_REFCURSOR variable declarations
      in the top level frame of a package or a package body,
      i.e. in the frame following immediately after IS/AS.
      For example, this script:
        CREATE PACKAGE BODY pkg AS
          cur SYS_REFCURSOR;
          ... functions and procedures ...
        END;
      returns an error:
        "Cursor Variables cannot be declared as part of a package"
      SYS_REFCURSOR can only appear in a package as:
      - a package routine parameter
      - a package function return value
      - in the package body initialization section
      Let's return an error on the top level, like Oracle does.

      Let's also disasallow SYS_REFCURSOR in stored aggregate functions.
    */
    if ((type == COLUMN_DEFINITION_ROUTINE_LOCAL &&
         thd->lex->spcont->scope() == sp_pcontext::PACKAGE_BODY_SCOPE) ||
         thd->lex->sphead->chistics().agg_type == GROUP_AGGREGATE)
    {
      my_error(ER_NOT_ALLOWED_IN_THIS_CONTEXT, MYF(0), sys_refcursor_str.str);
      return true;
    }
    return def->fix_attributes_int(MAX_SMALLINT_WIDTH + def->sign_length());
  }


  /*** Item_param related methods ***/

  void Item_param_set_param_func(Item_param *param,
                                 uchar **pos, ulong len) const override
  {
    storage_type_handler()->Item_param_set_param_func(param, pos, len);
  }

  Type_ref_null Item_param_val_ref(THD *thd, const Item_param *param)
                                                       const override
  {
    return param->val_ref_from_int();
  }

  bool Item_param_set_from_value(THD *thd, Item_param *param,
                                 const Type_all_attributes *attr,
                                 const st_value *val) const override
  {
    const Type_ref_null old_value= param->val_ref_from_int();
    param->unsigned_flag= attr->unsigned_flag;
    param->set_int(val->value.m_longlong, attr->max_length);
    const Type_ref_null new_value= Type_ref_null((ulonglong)
                                                 val->value.m_longlong);
    cursor_array(thd)->ref_count_update(thd, old_value, new_value);
    param->with_flags|= item_with_t::COMPLEX_DATA_TYPE;
    return false;
  }

  void Item_param_expr_event_handler(THD *thd, Item_param *param,
                                     expr_event_t event)  const override
  {
    /*
      A reference stored in Item_param detaches from the object it
      refers to when at the end of a prepared statement the value of ?
      gets copied to the routine actual OUT or INOUT parameter, e.g.:
        EXECUTE IMMEDIATE 'CALL p1_with_out_or_inout_param(?)' USING spvar;
      It does not change per row, in statements like this:
        EXECUTE IMMEDIATE 'SELECT ? FROM t1' USING ref_value;
      So it ignores most of the expr_event_t::DESTRUCT_XXX events.
    */
    if ((bool) (event & expr_event_t::DESTRUCT_DYNAMIC_PARAM))
    {
      const Type_ref_null ref= param->val_ref_from_int();
      if (!ref.is_null())
      {
        cursor_array(thd)->ref_count_dec(thd, ref.value());
        param->set_null();
      }
    }
  }

  /*** Item_func_hybrid related methods ***/

  bool Item_hybrid_func_fix_attributes(THD *thd,
                                       const LEX_CSTRING &name,
                                       Type_handler_hybrid_field_type *handler,
                                       Type_all_attributes *func,
                                       Item **items, uint nitems) const override
  {
    /*
      Suppress the inherited behavior which converts the data type
      from *INT to NEWDECIMAL if arguments have different signess.
    */
    return false;
  }

  Type_ref_null Item_func_hybrid_field_type_val_ref(THD *thd,
                                             Item_func_hybrid_field_type *item)
                                                                 const override
  {
    DBUG_ASSERT(item->type_handler() == this);
    const Type_ref_null ref= item->ref_op(thd);
    item->expr_event_handler_args(thd, expr_event_t::DESTRUCT_ROUTINE_ARG);
    return ref;
  }


  /*** Unary Item_func related methods ***/
  bool Item_func_round_fix_length_and_dec(Item_func_round *item) const override
  {
    return Item_func_or_sum_illegal_param(item);
  }

  bool Item_func_int_val_fix_length_and_dec(Item_func_int_val *item)
                                                                const override
  {
    return Item_func_or_sum_illegal_param(item);
  }

  bool Item_func_abs_fix_length_and_dec(Item_func_abs *item) const override
  {
    return Item_func_or_sum_illegal_param(item);
  }

  bool Item_func_neg_fix_length_and_dec(Item_func_neg *item) const override
  {
    return Item_func_or_sum_illegal_param(item);
  }


  /*** Item_sum related methods ***/

  bool Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *func) const override
  {
    // Convert "max(" to "max"  and  "min(" to "min"
    const LEX_CSTRING name= func->func_name_cstring();
    DBUG_ASSERT(name.length > 0);
    size_t lp= name.str[name.length - 1] == '(' ?  1 : 0;
    ErrConvString str(name.str, name.length - lp, system_charset_info);
    return Item_func_or_sum_illegal_param(str.lex_cstring());
  }

  bool Item_sum_sum_fix_length_and_dec(Item_sum_sum *item) const override
  {
    static const LEX_CSTRING name= {STRING_WITH_LEN("sum") };
    return Item_func_or_sum_illegal_param(name);
  }

  bool Item_sum_avg_fix_length_and_dec(Item_sum_avg *item) const override
  {
    static const LEX_CSTRING name= {STRING_WITH_LEN("avg") };
    return Item_func_or_sum_illegal_param(name);
  }

  bool Item_sum_variance_fix_length_and_dec(Item_sum_variance *item)
                                                                const override
  {
    return Item_func_or_sum_illegal_param(item);
  }

  Field *make_num_distinct_aggregator_field(MEM_ROOT *, const Item *)
                                                       const override
  {
    /*
      A call for this method should never happen because
      Item_sum_*_fix_length_and_dec() was called earlier and raised an error.
    */
    DBUG_ASSERT(0);
    return nullptr;
  }


  /*** CAST related classes and methods ***/

  class Item_cast_sys_refcursor_to_varchar_func_handler:
                                        public Item_handled_func::Handler_str
  {
  public:
    static const Item_cast_sys_refcursor_to_varchar_func_handler *singleton()
    {
      static Item_cast_sys_refcursor_to_varchar_func_handler fh;
      return &fh;
    }
    const Type_handler *return_type_handler(const Item_handled_func *item)
                                                            const override
    {
      return &type_handler_varchar;
    }
    bool fix_length_and_dec(Item_handled_func *item) const override
    {
      return false;
    }
    String *val_str(Item_handled_func *item, String *to) const override
    {
      DBUG_ASSERT(dynamic_cast<const Item_char_typecast*>(item));
      THD *thd= current_thd;
      Item *arg= item->arguments()[0];
      const Type_ref_null ref= arg->val_ref(thd);
      if ((item->null_value= ref.is_null()))
        return 0;
      DBUG_ASSERT(arg->with_complex_data_types());
      arg->expr_event_handler(thd, expr_event_t::DESTRUCT_ROUTINE_ARG);
      to->set(ref.value(), &my_charset_latin1);
      return static_cast<Item_char_typecast*>(item)->
        val_str_generic_finalize(to, to);
    }
  };

  bool Item_char_typecast_fix_length_and_dec(Item_char_typecast *item)
                                                        const override
  {
    item->fix_length_and_dec_numeric();
    item->set_func_handler(Item_cast_sys_refcursor_to_varchar_func_handler::
                                                              singleton());
    return false;
  }

  bool Item_func_signed_fix_length_and_dec(Item_func_signed *item)
                                                    const override
  {
    return Item_func_or_sum_illegal_param(item);
  }

  bool Item_func_unsigned_fix_length_and_dec(Item_func_unsigned *item)
                                                                const override
  {
    return Item_func_or_sum_illegal_param(item);
  }

  bool Item_double_typecast_fix_length_and_dec(Item_double_typecast *item)
                                                                const override
  {
    return Item_func_or_sum_illegal_param(item);
  }

  bool Item_float_typecast_fix_length_and_dec(Item_float_typecast *item)
                                                                const override
  {
    return Item_func_or_sum_illegal_param(item);
  }

  bool Item_decimal_typecast_fix_length_and_dec(Item_decimal_typecast *item)
                                                                const override
  {
    return Item_func_or_sum_illegal_param(item);
  }

  bool Item_time_typecast_fix_length_and_dec(Item_time_typecast *item)
                                                                 const override
  {
    return Item_func_or_sum_illegal_param(item);
  }

  bool Item_date_typecast_fix_length_and_dec(Item_date_typecast *item)
                                                                 const override
  {
    return Item_func_or_sum_illegal_param(item);
  }

  bool Item_datetime_typecast_fix_length_and_dec(Item_datetime_typecast *item)
                                                                 const override
  {
    return Item_func_or_sum_illegal_param(item);
  }

  Item *create_typecast_item(THD *thd, Item *item,
                             const Type_cast_attributes &attr) const override
  {
    return NULL;
  }


  /*** Methods related to other Item types ***/

  Item_cache *Item_get_cache(THD *thd, const Item *item) const override
  {
    /*
      It's not clear how to maintain cursor reference counters in Item_cache.
      Let's disallow all operations that need caching.
    */
    static const LEX_CSTRING name=
      {STRING_WITH_LEN("EXPRESSION CACHE (e.g. SUBSELECT)") };
    Item_func_or_sum_illegal_param(name);
    return nullptr;
  }

  Item_copy *create_item_copy(THD *thd, Item *item) const override
  {
    // Let's also disallow GROUP BY
    static const LEX_CSTRING name= {STRING_WITH_LEN("GROUP BY") };
    Item_func_or_sum_illegal_param(name);
    return nullptr;
  }

  /*
    Methods used by table columns.
    They should not be called, as SYS_REFCURSOR is only an SP data type.
  */
  bool type_can_have_auto_increment_attribute() const override
  {
    DBUG_ASSERT(0);
    return false;
  }

  bool partition_field_check(const LEX_CSTRING &, Item *item_expr)
                                                    const override
  {
    DBUG_ASSERT(0);
    return partition_field_check_result_type(item_expr, INT_RESULT);
  }

  bool partition_field_append_value(String *str,
                                    Item *item_expr,
                                    CHARSET_INFO *field_cs,
                                    partition_value_print_mode_t)
                                    const override
  {
    DBUG_ASSERT(0);
    return true;
  }
  const Vers_type_handler *vers() const override
  {
    DBUG_ASSERT(0);
    return NULL;
  }

  Field *make_conversion_table_field(MEM_ROOT *root,
                                     TABLE *table, uint metadata,
                                     const Field *target) const override
  {
    DBUG_ASSERT(0);
    return nullptr;
  }

  Field *make_schema_field(MEM_ROOT *root,
                           TABLE *table,
                           const Record_addr &addr,
                           const ST_FIELD_INFO &def) const override
  {
    DBUG_ASSERT(0);
    return nullptr;
  }

};


static Type_handler_sys_refcursor type_handler_sys_refcursor;

const Type_handler *Type_handler_sys_refcursor::singleton()
{
  return &type_handler_sys_refcursor;
}

const Type_handler *Field_sys_refcursor::type_handler() const
{
  return &type_handler_sys_refcursor;
}


const Type_handler *
Type_collection_cursor::aggregate_common(const Type_handler *h1,
                                         const Type_handler *h2) const
{
  if (h1 == h2)
   return h1;

  static const Type_aggregator::Pair agg[]=
  {
    {
      &type_handler_sys_refcursor,
      &type_handler_null,
      &type_handler_sys_refcursor
    },
    {NULL,NULL,NULL}
  };
  return Type_aggregator::find_handler_in_array(agg, h1, h2, true);
}


static struct st_mariadb_data_type plugin_descriptor_type_sys_refcursor=
{
  MariaDB_DATA_TYPE_INTERFACE_VERSION,
  &type_handler_sys_refcursor
};


/*************************************************************************/

#ifndef DBUG_OFF
class Item_func_cursor_ref_count :public Item_long_func,
                                  public Sys_refcursor_traits
{
public:
  Item_func_cursor_ref_count(THD *thd, Item *pos)
   :Item_long_func(thd, pos)
  { }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("cursor_ref_count") };
    return name;
  }
  bool const_item() const override
  {
    return false;
  }
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function(func_name(), "()", arg, VCOL_SESSION_FUNC);
  }
  bool fix_length_and_dec(THD *thd) override
  {
    bool rc= Item_long_func::fix_length_and_dec(thd);
    set_maybe_null(true);
    return rc;
  }
  longlong val_int() override
  {
    THD *thd= current_thd;
    ulonglong offset= (ulonglong) args[0]->val_int();
    if ((null_value= args[0]->null_value))
      return 0;
    const ULonglong_null count= cursor_array(thd)->ref_count(offset);
    return (null_value= count.is_null()) ? 0LL : (longlong) count.value();
  }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_cursor_ref_count>(thd, this); }
};


class Create_func_cursor_ref_count :public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *pos) override;
  static Create_func_cursor_ref_count s_singleton;
protected:
  Create_func_cursor_ref_count() {}
};


Create_func_cursor_ref_count Create_func_cursor_ref_count::s_singleton;

Item* Create_func_cursor_ref_count::create_1_arg(THD *thd, Item *pos)
{
  // Disallow query cache. This also disallows partitioning
  thd->lex->safe_to_cache_query= false;
  return new (thd->mem_root) Item_func_cursor_ref_count(thd, pos);
}

#define BUILDER(F) & F::s_singleton

static Plugin_function
  plugin_descriptor_function_cursor_ref_count(BUILDER(Create_func_cursor_ref_count));

#endif

/*************************************************************************/

maria_declare_plugin(type_cursor)
{
  MariaDB_DATA_TYPE_PLUGIN,     // the plugin type (see include/mysql/plugin.h)
  &plugin_descriptor_type_sys_refcursor, // a pointer to the plugin descriptor
  sys_refcursor_str.str,        // plugin name
  "MariaDB Corporation",        // plugin author
  "Data type SYS_REFCURSOR",    // the plugin description
  PLUGIN_LICENSE_GPL,           // the plugin license (see include/mysql/plugin.h)
  0,                            // Pointer to plugin initialization function
  0,                            // Pointer to plugin deinitialization function
  0x0100,                       // Numeric version 0xAABB means AA.BB version
  NULL,                         // Status variables
  NULL,                         // System variables
  "1.0",                        // String version representation
  MariaDB_PLUGIN_MATURITY_STABLE // Maturity(see include/mysql/plugin.h)*/
}
#ifndef DBUG_OFF
,
{
  MariaDB_FUNCTION_PLUGIN,      // the plugin type (see include/mysql/plugin.h)
  &plugin_descriptor_function_cursor_ref_count, // ptr to the plugin descriptor
  "cursor_ref_count",           // plugin name
  "MariaDB Corporation",        // plugin author
  "Function CURSOR_REF_COUNT()",// the plugin description
  PLUGIN_LICENSE_GPL,           // the plugin license (see include/mysql/plugin.h)
  0,                            // Pointer to plugin initialization function
  0,                            // Pointer to plugin deinitialization function
  0x0100,                       // Numeric version 0xAABB means AA.BB version
  NULL,                         // Status variables
  NULL,                         // System variables
  "1.0",                        // String version representation
  MariaDB_PLUGIN_MATURITY_STABLE // Maturity(see include/mysql/plugin.h)*/
}
#endif
maria_declare_plugin_end;
