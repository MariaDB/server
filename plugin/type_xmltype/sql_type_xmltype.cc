/*
   Copyright (c) 2025, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

#define MYSQL_SERVER
#include "mariadb.h"
#include "my_xml.h"
#include "sql_class.h"
#include "sql_lex.h"
#include "sp_rcontext.h"
#include "sql_type_xmltype.h"


Type_handler_xmltype type_handler_xmltype;
Type_collection_xmltype type_collection_xmltype;

const Type_collection *Type_handler_xmltype::type_collection() const
{
  return &type_collection_xmltype;
}

const Type_handler *Type_collection_xmltype::aggregate_for_comparison(
                       const Type_handler *a, const Type_handler *b) const
{
  if (a->type_collection() == this)
    swap_variables(const Type_handler *, a, b);
  if (a == &type_handler_xmltype      || a == &type_handler_hex_hybrid ||
      a == &type_handler_tiny_blob   || a == &type_handler_blob       ||
      a == &type_handler_medium_blob || a == &type_handler_long_blob  ||
      a == &type_handler_varchar     || a == &type_handler_string     ||
      a == &type_handler_null)
    return b;
  return NULL;
}

const Type_handler *Type_collection_xmltype::aggregate_for_result(
                       const Type_handler *a, const Type_handler *b) const
{
  return aggregate_for_comparison(a,b);
}

const Type_handler *Type_collection_xmltype::aggregate_for_min_max(
                       const Type_handler *a, const Type_handler *b) const
{
  return aggregate_for_comparison(a,b);
}

const Type_handler *Type_collection_xmltype::aggregate_for_num_op(
                      const Type_handler *a, const Type_handler *b) const
{
  return NULL;
}


constexpr LEX_CSTRING Type_handler_xmltype::name_on_client;

const Type_handler *Type_handler_xmltype::type_handler_for_comparison() const
{
  return &type_handler_xmltype;
}


Field *Type_handler_xmltype::make_conversion_table_field(
      MEM_ROOT *root, TABLE *table, uint metadata, const Field *target) const
{
  /* Copied from Type_handler_blob_common. */
  uint pack_length= metadata & 0x00ff;
  if (pack_length != 4)
    return NULL; // Broken binary log?

  return new(root)
    Field_xmltype(NULL, (uchar *) "", 1, Field::NONE, &empty_clex_str,
                  table->s, target->charset());
}


Item *Type_handler_xmltype::create_typecast_item(THD *thd, Item *item,
        const Type_cast_attributes &attr) const
{
  CHARSET_INFO *real_cs= attr.charset() ?
                  attr.charset() : thd->variables.collation_connection;

  if (real_cs == &my_charset_bin)
  {
    my_error(ER_ILLEGAL_PARAMETER_DATA_TYPE_FOR_OPERATION, MYF(0),
             name().ptr(), "CHARACTER SET binary");
    return NULL;
  }

  return new (thd->mem_root) Item_xmltype_typecast(thd, item, real_cs);
}


bool Type_handler_xmltype:: Column_definition_prepare_stage1(THD *thd,
  MEM_ROOT *mem_root, Column_definition *def, column_definition_type_t type,
  const Column_derived_attributes *derived_attr) const
{
  if (Type_handler_long_blob::
      Column_definition_prepare_stage1(thd, mem_root, def, type, derived_attr))
    return true;
  if (def->charset == &my_charset_bin)
  {
    my_error(ER_ILLEGAL_PARAMETER_DATA_TYPE_FOR_OPERATION, MYF(0),
             name().ptr(), "CHARACTER SET binary");
    return true;
  }
  return false;
}


Field *Type_handler_xmltype::make_table_field(MEM_ROOT *root,
         const LEX_CSTRING *name, const Record_addr &addr,
         const Type_all_attributes &attr, TABLE_SHARE *share) const
{
  return new (root) Field_xmltype(addr.ptr(), addr.null_ptr(), addr.null_bit(),
                                 Field::NONE, name, share, attr.collation);
}


Field *Type_handler_xmltype::make_table_field_from_def(TABLE_SHARE *share,
         MEM_ROOT *root, const LEX_CSTRING *name, const Record_addr &rec,
         const Bit_addr &bit, const Column_definition_attributes *attr,
         uint32 flags) const
{
  return new (root) Field_xmltype(rec.ptr(), rec.null_ptr(), rec.null_bit(),
            attr->unireg_check, name, share, attr->charset);
}


Item *
Type_handler_xmltype::make_constructor_item(THD *thd, List<Item> *args) const
{
  if (!args || args->elements != 1)
    return NULL;
  Item_args tmp(thd, *args);
  return new (thd->mem_root)
    Item_xmltype_typecast(thd, tmp.arguments()[0],
                          thd->variables.collation_connection);
}


bool Type_handler_xmltype::
  Item_hybrid_func_fix_attributes(THD *thd, const LEX_CSTRING &func_name,
         Type_handler_hybrid_field_type *handler, Type_all_attributes *func,
         Item **items, uint nitems) const
{
  if (func->aggregate_attributes_string(func_name, items, nitems))
    return true;

  handler->set_handler(&type_handler_xmltype);
  return false;
}


const Type_handler *Type_handler_xmltype::
  type_handler_for_tmp_table(const Item *item) const
{
  return &type_handler_xmltype;
}


bool Type_handler_xmltype::
       Item_func_round_fix_length_and_dec(Item_func_round *item) const
{
  return Item_func_or_sum_illegal_param(item);
}


bool Type_handler_xmltype::
       Item_func_int_val_fix_length_and_dec(Item_func_int_val *item) const
{
  return Item_func_or_sum_illegal_param(item);
}


bool Type_handler_xmltype::
       Item_func_abs_fix_length_and_dec(Item_func_abs *item) const
{
  return Item_func_or_sum_illegal_param(item);
}


bool Type_handler_xmltype::
       Item_func_neg_fix_length_and_dec(Item_func_neg *item) const
{
  return Item_func_or_sum_illegal_param(item);
}


bool Type_handler_xmltype::
       Item_sum_sum_fix_length_and_dec(Item_sum_sum *item) const
{
  LEX_CSTRING name= {STRING_WITH_LEN("sum") };
  return Item_func_or_sum_illegal_param(name);
}


bool Type_handler_xmltype::
       Item_sum_avg_fix_length_and_dec(Item_sum_avg *item) const
{
  LEX_CSTRING name= {STRING_WITH_LEN("avg") };
  return Item_func_or_sum_illegal_param(name);
}


bool Type_handler_xmltype::
       Item_sum_variance_fix_length_and_dec(Item_sum_variance *item) const
{
  return Item_func_or_sum_illegal_param(item);
}


bool Type_handler_xmltype::
       Item_func_signed_fix_length_and_dec(Item_func_signed *item) const
{
  return Item_func_or_sum_illegal_param(item);
}


bool Type_handler_xmltype::
       Item_func_unsigned_fix_length_and_dec(Item_func_unsigned *item) const
{
  return Item_func_or_sum_illegal_param(item);
}


bool Type_handler_xmltype::
       Item_double_typecast_fix_length_and_dec(Item_double_typecast *item) const
{
  return Item_func_or_sum_illegal_param(item);
}


bool Type_handler_xmltype::
       Item_float_typecast_fix_length_and_dec(Item_float_typecast *item) const
{
  return Item_func_or_sum_illegal_param(item);
}


bool Type_handler_xmltype::
       Item_decimal_typecast_fix_length_and_dec(Item_decimal_typecast *item) const
{
  return Item_func_or_sum_illegal_param(item);
}


bool Type_handler_xmltype::
       Item_time_typecast_fix_length_and_dec(Item_time_typecast *item) const
{
  return Item_func_or_sum_illegal_param(item);
}


bool Type_handler_xmltype::
       Item_date_typecast_fix_length_and_dec(Item_date_typecast *item) const
{
  return Item_func_or_sum_illegal_param(item);
}


bool Type_handler_xmltype::
       Item_datetime_typecast_fix_length_and_dec(Item_datetime_typecast *item)
                                                 const
{
  return Item_func_or_sum_illegal_param(item);

}


class Item_func_xml_method_str :public Item_str_func
{
protected:
  THD *m_thd;
  sp_rcontext_addr m_var_addr;
  String tmp_str;

public:
  Item_func_xml_method_str(THD *thd, uint var_idx,
       const Sp_rcontext_handler *rcontext_handler):
    Item_str_func(thd),
    m_thd(thd),
    m_var_addr(rcontext_handler, var_idx) {}

  bool fix_length_and_dec(THD *thd) override
  {
    Item_field *i= m_thd->get_variable(m_var_addr);
    
    collation= i->collation;
    return false;
  }
};


class Item_func_xml_method_getRootElement :public Item_func_xml_method_str
{
public:
  static LEX_CSTRING m_func_name;
  Item_func_xml_method_getRootElement(THD *thd, uint var_idx,
       const Sp_rcontext_handler *rcontext_handler):
    Item_func_xml_method_str(thd, var_idx, rcontext_handler) {}

  bool check_arguments() const override
  {
    return false;
  }
  String *val_str(String *str) override;
  LEX_CSTRING func_name_cstring() const override
  {
    return m_func_name;
  }
  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Item_func_xml_method_getRootElement>(thd, this); }
};


class Item_func_xml_method_getNamespace
  :public Item_func_xml_method_str
{
public:
  static LEX_CSTRING m_func_name;
  struct Parser_data
  {
    const char *name;
    size_t len;
    bool in_namespace;
    int level;
    int n_xmlns;
  };
  
  Item_func_xml_method_getNamespace(THD *thd, uint var_idx,
                           const Sp_rcontext_handler *rcontext_handler):
     Item_func_xml_method_str(thd, var_idx, rcontext_handler) {}
  LEX_CSTRING func_name_cstring() const override
  {
    return m_func_name;
  }
  bool check_arguments() const override
  {
    return false;
  }
  String *val_str(String *str) override;
};


class Item_func_xml_method_getSchemaURL :public Item_str_func
{
  THD *m_thd;
  sp_rcontext_addr m_var_addr;

public:
  Item_func_xml_method_getSchemaURL(THD *thd, uint var_idx,
       const Sp_rcontext_handler *rcontext_handler):
    Item_str_func(thd),
    m_thd(thd),
    m_var_addr(rcontext_handler, var_idx) {}

  bool check_arguments() const override
  {
    return false;
  }
  String *val_str(String *str) override
  {
    /*
      We don't register the XML Schema yet,
      so this function only returns NULL until it's done.
    */
    null_value= 1;
    return NULL;
  }
  bool fix_length_and_dec(THD *thd) override
  {
    Item_field *i= m_thd->get_variable(m_var_addr);
    
    collation= i->collation;
    return false;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("getSchemaURL") };
    return name;
  }
  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Item_func_xml_method_getSchemaURL>(thd, this); }
};


class Item_func_xml_method_getStringVal :public Item_func_xml_method_str
{
public:
  static LEX_CSTRING m_func_name;
  Item_func_xml_method_getStringVal(THD *thd, uint var_idx,
       const Sp_rcontext_handler *rcontext_handler):
    Item_func_xml_method_str(thd, var_idx, rcontext_handler) {}

  bool check_arguments() const override
  {
    return false;
  }
  String *val_str(String *str) override;
  LEX_CSTRING func_name_cstring() const override
  {
    return m_func_name;
  }
  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Item_func_xml_method_getStringVal>(thd, this); }
};


class Item_func_xml_method_getNumberVal :public Item_real_func
{
  THD *m_thd;
  sp_rcontext_addr m_var_addr;
  String tmp_str;

public:
  static LEX_CSTRING m_func_name;
  struct Parser_data
  {
    bool in_attribute;
    const char *str;
    size_t length;
  };

  Item_func_xml_method_getNumberVal(THD *thd, uint var_idx,
       const Sp_rcontext_handler *rcontext_handler):
    Item_real_func(thd),
    m_thd(thd),
    m_var_addr(rcontext_handler, var_idx) {}

  bool check_arguments() const override
  {
    return false;
  }
  double val_real() override;
  LEX_CSTRING func_name_cstring() const override
  {
    return m_func_name;
  }
  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Item_func_xml_method_getNumberVal>(thd, this); }
};


class Item_func_xml_method_isFragment :public Item_long_func
{
  THD *m_thd;
  sp_rcontext_addr m_var_addr;
  String tmp_str;

public:
  static LEX_CSTRING m_func_name;
  Item_func_xml_method_isFragment(THD *thd, uint var_idx,
       const Sp_rcontext_handler *rcontext_handler):
    Item_long_func(thd),
    m_thd(thd),
    m_var_addr(rcontext_handler, var_idx) {}

  bool check_arguments() const override
  {
    return false;
  }
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  {
    return m_func_name;
  }
  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Item_func_xml_method_isFragment>(thd, this); }
};


static const LEX_CSTRING extract_funcname= {STRING_WITH_LEN("extract") };
static const LEX_CSTRING existsNode_funcname= {STRING_WITH_LEN("existsNode") };
static const LEX_CSTRING getSchemaURL_funcname=
  {STRING_WITH_LEN("getSchemaURL") };

Item *Type_handler_xmltype::create_item_method(
    THD *thd, object_method_type_t type, const Lex_ident_sys &a,
    const Lex_ident_sys &b, List<Item> *args,
    const Lex_ident_cli_st &query_fragment) const
{
  Item *item= NULL;
  sp_variable *spvar= NULL;
  const Sp_rcontext_handler *rcontext_handler;

  spvar= thd->lex->find_variable(&a, &rcontext_handler);

  if (type != object_method_type_t::FUNCTION ||
      spvar == NULL)
    return NULL;


  if (b.length == 7)
  {
    if (Lex_ident_routine(b).streq(extract_funcname))
    {
      if (!args ||(args->elements != 1 && args->elements != 2))
      {
        my_error(ER_SP_WRONG_NO_OF_ARGS, MYF(0), "XMLTYPE::extract",
            "", 1, args ? args->elements : 0);
        return NULL;
      }

      Item_args iargs(thd, *args);
      item= new (thd->mem_root) Item_func_xml_extractvalue(thd,
          iargs.arguments()[0], iargs.arguments()[1]);
    }
    else if (Lex_ident_routine(b).streq(existsNode_funcname))
    {
    }
  }
  if (b.length == 10)
  {
    if (Lex_ident_routine(b).streq(
          Item_func_xml_method_isFragment::m_func_name))
    {
      if (args && args->elements != 0)
      {
        my_error(ER_SP_WRONG_NO_OF_ARGS, MYF(0),
            Item_func_xml_method_isFragment::m_func_name.str,
            "", 0, args ? args->elements : 0);
        return NULL;
      }

      item= new (thd->mem_root) Item_func_xml_method_isFragment(
                                  thd, spvar->offset, rcontext_handler);
    }
  }
  if (b.length == 12)
  {
    if (Lex_ident_routine(b).streq(getSchemaURL_funcname))
    {
      if (args && args->elements != 0)
      {
        my_error(ER_SP_WRONG_NO_OF_ARGS, MYF(0), getSchemaURL_funcname.str,
            "", 0, args ? args->elements : 0);
        return NULL;
      }

      item= new (thd->mem_root) Item_func_xml_method_getSchemaURL(
                                  thd, spvar->offset, rcontext_handler);
    }
    else if (Lex_ident_routine(b).streq(
               Item_func_xml_method_getNumberVal::m_func_name))
    {
      if (args && args->elements != 0)
      {
        my_error(ER_SP_WRONG_NO_OF_ARGS, MYF(0),
            Item_func_xml_method_getNumberVal::m_func_name.str,
            "", 0, args ? args->elements : 0);
        return NULL;
      }

      item= new (thd->mem_root) Item_func_xml_method_getNumberVal(
                                  thd, spvar->offset, rcontext_handler);
    }
    else if (Lex_ident_routine(b).streq(
               Item_func_xml_method_getStringVal::m_func_name))
    {
      if (args && args->elements != 0)
      {
        my_error(ER_SP_WRONG_NO_OF_ARGS, MYF(0),
            Item_func_xml_method_getStringVal::m_func_name.str,
            "", 0, args ? args->elements : 0);
        return NULL;
      }

      item= new (thd->mem_root) Item_func_xml_method_getStringVal(
                                  thd, spvar->offset, rcontext_handler);
    }
  }
  if (b.length == 14)
  {
    if (Lex_ident_routine(b).streq(
          Item_func_xml_method_getRootElement::m_func_name))
    {
      if (args && args->elements != 0)
      {
        my_error(ER_SP_WRONG_NO_OF_ARGS, MYF(0),
            Item_func_xml_method_getRootElement::m_func_name.str,
            "", 0, args ? args->elements : 0);
        return NULL;
      }

      item= new (thd->mem_root) Item_func_xml_method_getRootElement(
                                  thd, spvar->offset, rcontext_handler);
    }
  }

  return item;
}


/*****************************************************************/
void Field_xmltype::sql_type(String &res) const
{
  res.set_ascii(STRING_WITH_LEN("xmltype"));
}


int Field_xmltype::report_wrong_value(const ErrConv &val)
{
  get_thd()->push_warning_truncated_value_for_field(
    Sql_condition::WARN_LEVEL_WARN, "xmltype", val.ptr(),
    table->s->db.str, table->s->table_name.str, field_name.str);
  reset();
  return 1;
}


static int check_parse_xml(const char *xml, size_t length, CHARSET_INFO *cs)
{
  MY_XML_PARSER p;
  int result;

  /* Prepare XML parser */
  my_xml_parser_create(&p);
  p.flags= MY_XML_FLAG_RELATIVE_NAMES |
           MY_XML_FLAG_SKIP_TEXT_NORMALIZATION |
           MY_XML_FLAG_ASSERT_WELL_FORMED;

  result= my_xml_parse(&p, xml, length);
  my_xml_parser_free(&p);

  return result;
}


int Field_xmltype::store(const char *from, size_t length, CHARSET_INFO *cs)
{
  if (length < 4 ||
      check_parse_xml(from, length, cs) != MY_XML_OK)
    goto err;


  return Field_blob::store(from, length, cs);

err:
  my_error(ER_WRONG_VALUE, MYF(0),
           "XMLTYPE", ErrConvString(from, length, cs).ptr());

  if (maybe_null())
    set_null();
  else
    Field_blob::store(STRING_WITH_LEN("<invalid_xml_replaced />"), cs);
  return -1;
}


/*
  We allow any string input into the XMLTYPE,
  as it can fit into LONG BLOB without any loss.
  But in any case values themselves can be invalid XML-s.

  TODO: when the replication start sending UDT informatio,
  we should only return CONV_TYPE_PRECISE for the XMLTYPE.
*/
enum_conv_type
Field_xmltype::rpl_conv_type_from(const Conv_source &source,
                                  const Relay_log_info *rli,
                                  const Conv_param &param) const
{
  const Type_handler *th= source.type_handler();
  if (th == &type_handler_tiny_blob ||
      th == &type_handler_medium_blob ||
      th == &type_handler_long_blob ||
      th == &type_handler_blob ||
      th == &type_handler_blob_compressed ||
      th == &type_handler_string ||
      th == &type_handler_var_string ||
      th == &type_handler_varchar ||
      th == &type_handler_varchar_compressed)
  {
    return CONV_TYPE_PRECISE;
  }

  return CONV_TYPE_IMPOSSIBLE;
}


class Item_xmltype_typecast_func_handler: public Item_handled_func::Handler_str
{
public:
  const Type_handler *
    return_type_handler(const Item_handled_func *item) const override
  { return &type_handler_xmltype; }

  const Type_handler *
    type_handler_for_create_select(const Item_handled_func *item) const override
  { return &type_handler_xmltype; }

  bool fix_length_and_dec(Item_handled_func *item) const override
  {
    return false;
  }
  String *val_str(Item_handled_func *item, String *to) const override
  {
    DBUG_ASSERT(dynamic_cast<const Item_xmltype_typecast*>(item));
    return static_cast<Item_xmltype_typecast*>(item)->val_str_generic(to);
  }
};


static Item_xmltype_typecast_func_handler item_xmltype_typecast_func_handler;

bool Item_xmltype_typecast::fix_length_and_dec(THD *thd)
{
  Item_char_typecast::fix_length_and_dec_str();
  set_func_handler(&item_xmltype_typecast_func_handler);

  if (cast_charset()->mbminlen > 1)
  {
    my_error(ER_NOT_SUPPORTED_YET, MYF(0),
             "CAST(AS XMLTYPE CHARACTER SET ucs2/utf16/utf32)");
    return true;
  }

  return false;
}


String *Item_xmltype_typecast::val_str(String *to)
{
  String *res= Item_char_typecast::val_str(to);
  if (!res)
    return NULL;

  if (check_parse_xml(res->ptr(), res->length(), res->charset()) != MY_XML_OK)
  {
    THD *thd= current_thd;
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_TRUNCATED_WRONG_VALUE,
                        ER_THD(thd, ER_TRUNCATED_WRONG_VALUE), "xmltype",
                        ErrConvString(res->ptr(), res->length(),
                                      res->charset()).ptr());
    null_value= 1;
    return NULL;
  }

  return res;
}


void Item_xmltype_typecast::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("cast("));
  args[0]->print(str, query_type);
  str->append(STRING_WITH_LEN(" as xmltype"));
  print_charset(str);
  str->append(')');
}


LEX_CSTRING Item_func_xml_method_getRootElement::m_func_name=
  {STRING_WITH_LEN("getRootElement") };

extern "C" {
static int get_root_element_enter(MY_XML_PARSER *st,
                                  const char *attr, size_t len)
{
  LEX_CSTRING *data= (LEX_CSTRING*) st->user_data;
  data->str= attr;
  data->length= len;
  return MY_XML_ERROR;
}
} /*extern "C"*/

String *Item_func_xml_method_getRootElement::val_str(String *str)
{
  MY_XML_PARSER p;
  LEX_CSTRING user_data;

  Item_field *i= m_thd->get_variable(m_var_addr);
  String *xml= i->val_str(&tmp_str);

  if (!xml)
    goto err_ret;

  user_data.str= NULL;

  /* Prepare XML parser */
  my_xml_parser_create(&p);
  p.flags= MY_XML_FLAG_RELATIVE_NAMES | MY_XML_FLAG_SKIP_TEXT_NORMALIZATION;

  my_xml_set_enter_handler(&p, get_root_element_enter);
  my_xml_set_user_data(&p, (void*) &user_data);

  /* Execute XML parser */
  my_xml_parse(&p, xml->ptr(), xml->length());

  if (user_data.str == NULL)
  {
    char buf[128];
    my_snprintf(buf, sizeof(buf)-1,
                "XML Schema parse error at line %d pos %lu: %s",
                my_xml_error_lineno(&p) + 1,
                (ulong) my_xml_error_pos(&p) + 1,
                my_xml_error_string(&p));
    my_printf_error(ER_WRONG_VALUE, ER_THD(m_thd, ER_WRONG_VALUE), MYF(0),
                    "XMLTYPE", buf);
    my_xml_parser_free(&p);
    goto err_ret;
  }

  my_xml_parser_free(&p);

  null_value= 0;
  str->set(user_data.str, user_data.length, collation.collation);
  return str;

err_ret:
  null_value= 1;
  return NULL;
}


LEX_CSTRING Item_func_xml_method_getNamespace::m_func_name=
  {STRING_WITH_LEN("getNamespace") };


extern "C" {
static int get_namespace_enter(MY_XML_PARSER *st,const char *attr, size_t len)
{
  Item_func_xml_method_getNamespace::Parser_data *data=
    (Item_func_xml_method_getNamespace::Parser_data *) st->user_data;


  if (st->current_node_type == MY_XML_NODE_TAG)
  {
    if (data->level > 0)
    {
      return MY_XML_ERROR;
    }
    data->level= 1;
  }
  else if (st->current_node_type == MY_XML_NODE_ATTR)
  {
    if ((len == 5 && memcmp(attr, "xmlns", 5)) ||
        (len >=6 && memcmp(attr, "xmlns:", 6)))
      data->in_namespace= true;
  }

  return MY_XML_OK;
}


static int get_namespace_value(MY_XML_PARSER *st,const char *attr, size_t len)
{
  Item_func_xml_method_getNamespace::Parser_data *data=
    (Item_func_xml_method_getNamespace::Parser_data *) st->user_data;

  if (data->in_namespace)
  {
    data->n_xmlns++;
    data->name= attr;
    data->len= len;
  }
  return MY_XML_OK;
}
} /*extern "C"*/


String *Item_func_xml_method_getNamespace::val_str(String *str)
{
  MY_XML_PARSER p;
  Item_func_xml_method_getNamespace::Parser_data user_data;

  Item_field *i= m_thd->get_variable(m_var_addr);
  String *xml= i->val_str(&tmp_str);

  if (!xml)
    goto err_ret;

  /* Prepare XML parser */
  my_xml_parser_create(&p);
  p.flags= MY_XML_FLAG_RELATIVE_NAMES | MY_XML_FLAG_SKIP_TEXT_NORMALIZATION;

  my_xml_set_enter_handler(&p, get_namespace_enter);
  my_xml_set_value_handler(&p, get_namespace_value);
  my_xml_set_user_data(&p, (void*) &user_data);

  user_data.n_xmlns= 0;
  user_data.level= 0;
  user_data.in_namespace= false;

  /* Execute XML parser */
  my_xml_parse(&p, xml->ptr(), xml->length());

  if (user_data.level == 0)
  {
    char buf[128];
    my_snprintf(buf, sizeof(buf)-1,
                "XML Schema parse error at line %d pos %lu: %s",
                my_xml_error_lineno(&p) + 1,
                (ulong) my_xml_error_pos(&p) + 1,
                my_xml_error_string(&p));
    my_printf_error(ER_WRONG_VALUE, ER_THD(m_thd, ER_WRONG_VALUE), MYF(0),
                    "XMLTYPE", buf);
    my_xml_parser_free(&p);
    goto err_ret;
  }

  my_xml_parser_free(&p);

  null_value= 0;
  str->set(user_data.name, user_data.len, collation.collation);
  return str;

err_ret:
  null_value= 1;
  return NULL;
}


LEX_CSTRING Item_func_xml_method_getStringVal::m_func_name=
  {STRING_WITH_LEN("getStringVal") };

String *Item_func_xml_method_getStringVal::val_str(String *str)
{
  Item_field *i= m_thd->get_variable(m_var_addr);
  String *xml= i->val_str(str);

  null_value= i->null_value;
  return xml;
}


LEX_CSTRING Item_func_xml_method_getNumberVal::m_func_name=
  {STRING_WITH_LEN("getNumberVal") };

extern "C" {
static int get_number_val_enter(MY_XML_PARSER *st,
                                const char *attr, size_t len)
{
  Item_func_xml_method_getNumberVal::Parser_data *data=
    (Item_func_xml_method_getNumberVal::Parser_data *) st->user_data;

  if (st->current_node_type == MY_XML_NODE_ATTR)
    data->in_attribute= true;
  return MY_XML_OK;
}

static int get_number_val_leave(MY_XML_PARSER *st,
                                const char *attr, size_t len)
{
  Item_func_xml_method_getNumberVal::Parser_data *data=
    (Item_func_xml_method_getNumberVal::Parser_data *) st->user_data;

  data->in_attribute= false;
  return MY_XML_OK;
}

static int get_number_val_value(MY_XML_PARSER *st,
                                const char *val, size_t len)
{
  Item_func_xml_method_getNumberVal::Parser_data *data=
    (Item_func_xml_method_getNumberVal::Parser_data *) st->user_data;

  if (data->in_attribute)
    return MY_XML_OK;

  data->str= val;
  data->length= len;
  return MY_XML_ERROR;
}
} /*extern "C"*/

double Item_func_xml_method_getNumberVal::val_real()
{
  MY_XML_PARSER p;
  Parser_data user_data;

  Item_field *i= m_thd->get_variable(m_var_addr);
  String *xml= i->val_str(&tmp_str);

  if (!xml)
  {
    null_value= 1;
    return 0;
  }

  user_data.in_attribute= false;
  user_data.str= NULL;

  /* Prepare XML parser */
  my_xml_parser_create(&p);
  p.flags= MY_XML_FLAG_RELATIVE_NAMES | MY_XML_FLAG_SKIP_TEXT_NORMALIZATION;

  my_xml_set_enter_handler(&p, get_number_val_enter);
  my_xml_set_leave_handler(&p, get_number_val_leave);
  my_xml_set_value_handler(&p, get_number_val_value);
  my_xml_set_user_data(&p, (void*) &user_data);

  /* Execute XML parser */
  my_xml_parse(&p, xml->ptr(), xml->length());
  my_xml_parser_free(&p);

  null_value= 0;
  if (user_data.str == NULL)
    return 0;

  return double_from_string_with_check(xml->charset(), user_data.str,
                                       user_data.str + user_data.length);
}


LEX_CSTRING Item_func_xml_method_isFragment::m_func_name=
  {STRING_WITH_LEN("isFragment") };

longlong Item_func_xml_method_isFragment::val_int()
{
  MY_XML_PARSER p;

  Item_field *i= m_thd->get_variable(m_var_addr);
  String *xml= i->val_str(&tmp_str);

  if (!xml)
  {
    null_value= 1;
    return 0;
  }

  /* Prepare XML parser */
  my_xml_parser_create(&p);
  p.flags= MY_XML_FLAG_RELATIVE_NAMES | MY_XML_FLAG_SKIP_TEXT_NORMALIZATION |
           MY_XML_FLAG_ASSERT_WELL_FORMED;

  /* Execute XML parser */
  bool wellformed= my_xml_parse(&p, xml->ptr(), xml->length()) == MY_XML_OK;
  my_xml_parser_free(&p);

  null_value= 0;
  return wellformed ? 0 : 1;
}
