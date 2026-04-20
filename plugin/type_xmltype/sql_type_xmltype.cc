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
    Item_xmltype_typecast(thd, tmp.arguments()[0], NULL);
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


struct MY_XML_WF_DATA
{
  int root_node_counter;
  int root_items_counter;
  int level;
  MY_XML_WF_DATA(): root_node_counter(0), root_items_counter(0), level(0) {}
};


extern "C" {
/*
  An XML document must have exacly one root element
  that wraps all other content.
  So that the simple 'abc' or '123.4' and the 'abc <node> xyz </node>'
  are not well-formed XML-s.
  Though spaces are allowed, so we only return an error
  if we get not-space-only string on the level 0.
*/
static int wf_value(MY_XML_PARSER *st,const char *attr, size_t len)
{
  MY_XML_WF_DATA *data= (MY_XML_WF_DATA *) st->user_data;

  if (len == 0 || data->level > 0)
    return MY_XML_OK;

  while (isspace(*attr))
  {
    attr++;
    if (--len == 0)
      return MY_XML_OK;
  }

  return MY_XML_ERROR;
}


/*
  decrease the level count.
*/
static int wf_leave(MY_XML_PARSER *st,const char *attr, size_t len)
{
  MY_XML_WF_DATA *data= (MY_XML_WF_DATA *) st->user_data;

  data->level--;
  return MY_XML_OK;
}


/*
  The well-formed XML document must contain exactly one root element.
  So we check this.
  That root element can be preceded by preprocessing instructions
  as <?xml ... ?>. So we allow it.

    <?xml version="1.0"?>
    <note>
      <to>JOHN</to>
      <from>MARY</from>
      <heading>Reminder</heading>
      <body>Take care!</body>
    </note>
*/
static int wf_enter(MY_XML_PARSER *st,const char *attr, size_t len)
{
  MY_XML_WF_DATA *data= (MY_XML_WF_DATA *) st->user_data;

  if (data->level == 0)
  {
    if (st->current_node_type == MY_XML_NODE_TAG)
    {
      /* no second root element allowed. */
      if (data->root_node_counter++ > 0)
        return MY_XML_ERROR;
    }

    data->root_items_counter++;
  }

  data->level++;
  return MY_XML_OK;
}


/*
  The preprocessing instruction can appear anywhere
  except for the <?xml ..?>.
  It has special meaning and if appears has to go before anything elese.
    <?xml version="1.0"?>
    <note>
      <to>JOHN</to>
      <from>MARY</from>
      <heading>Reminder</heading>
      <body>Take care!</body>
    </note>
*/
int wf_processing_instruction(struct xml_stack_st *st,
                              const char *target, size_t target_len,
                              const char *value, size_t value_len)
{
  MY_XML_WF_DATA *data= (MY_XML_WF_DATA *) st->user_data;

  data->root_items_counter++;

  if (target_len == 3 && memcmp(target, "xml", 3) == 0)
  {
    if (data->root_items_counter > 1)
      return MY_XML_ERROR;
  }

  return MY_XML_OK;
}


} /* extern "C" */


static int check_parse_xml(const char *xml, size_t length, CHARSET_INFO *cs)
{
  MY_XML_PARSER p;
  MY_XML_WF_DATA data;

  /* Prepare XML parser */
  my_xml_parser_create(&p);
  p.flags= MY_XML_FLAG_RELATIVE_NAMES | MY_XML_FLAG_SKIP_TEXT_NORMALIZATION;

  my_xml_set_enter_handler(&p, wf_enter);
  my_xml_set_value_handler(&p, wf_value);
  my_xml_set_leave_handler(&p, wf_leave);
  my_xml_set_processing_instruction_handler(&p, wf_processing_instruction);
  my_xml_set_user_data(&p, (void *) &data);

  if (my_xml_parse(&p, xml, length))
    return MY_XML_ERROR;

  return data.root_node_counter == 1 ? MY_XML_OK : MY_XML_ERROR;
}


int Field_xmltype::store(const char *from, size_t length, CHARSET_INFO *cs)
{
  if (length < 4 ||
      check_parse_xml(from, length, cs) != MY_XML_OK)
    goto err;


  return Field_blob::store(from, length, cs);

err:
  get_thd()->push_warning_wrong_value(
               Sql_condition::WARN_LEVEL_WARN, "XML", from);
  return -1;
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
  return false;
}

void Item_xmltype_typecast::print(String *str, enum_query_type query_type)
{
  str->append(STRING_WITH_LEN("cast("));
  args[0]->print(str, query_type);
  str->append(STRING_WITH_LEN(" as xmltype"));
  print_charset(str);
  str->append(')');
}

