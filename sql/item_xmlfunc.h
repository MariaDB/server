#ifndef ITEM_XMLFUNC_INCLUDED
#define ITEM_XMLFUNC_INCLUDED

/* Copyright (c) 2000, 2019, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2009, 2019, MariaDB

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


/* This file defines all XML functions */


typedef struct my_xml_node_st MY_XML_NODE;


/* Structure to store nodeset elements */
class MY_XPATH_FLT
{
public:
  uint num;     // Absolute position in MY_XML_NODE array
  uint pos;     // Relative position in context
  uint size;    // Context size
public:
  MY_XPATH_FLT(uint32 num_arg, uint32 pos_arg)
   :num(num_arg), pos(pos_arg), size(0)
  { }
  MY_XPATH_FLT(uint32 num_arg, uint32 pos_arg, uint32 size_arg)
   :num(num_arg), pos(pos_arg), size(size_arg)
  { }
  bool append_to(Native *to) const
  {
    return to->append((const char*) this, (uint32) sizeof(*this));
  }
};


class NativeNodesetBuffer: public NativeBuffer<16*sizeof(MY_XPATH_FLT)>
{
public:
  const MY_XPATH_FLT &element(uint i) const
  {
    const MY_XPATH_FLT *p= (MY_XPATH_FLT*) (ptr() + i * sizeof(MY_XPATH_FLT));
    return *p;
  }
  uint32 elements() const
  {
    return length() / sizeof(MY_XPATH_FLT);
  }
};


class Item_xml_str_func: public Item_str_func
{
protected:
  /*
    A helper class to store raw and parsed XML.
  */
  class XML
  {
    bool m_cached;
    String *m_raw_ptr;   // Pointer to text representation
    String m_raw_buf;    // Cached text representation
    String m_parsed_buf; // Array of MY_XML_NODEs, pointing to raw_buffer
    bool parse();
    void reset()
    {
      m_cached= false;
      m_raw_ptr= (String *) 0;
    }
  public:
    XML() { reset(); }
    void set_charset(CHARSET_INFO *cs) { m_parsed_buf.set_charset(cs); }
    String *raw() { return m_raw_ptr; }
    String *parsed() { return &m_parsed_buf; }
    const MY_XML_NODE *node(uint idx);
    bool cached() { return m_cached; }
    bool parse(String *raw, bool cache);
    bool parse(Item *item, bool cache)
    {
      String *res;
      if (!(res= item->val_str(&m_raw_buf)))
      {
        m_raw_ptr= (String *) 0;
        m_cached= cache;
        return true;
      }
      return parse(res, cache);
    }
  };
  String m_xpath_query; // XPath query text
  Item *nodeset_func;
  XML xml;
  bool get_xml(XML *xml_arg, bool cache= false)
  {
    if (!cache && xml_arg->cached())
      return xml_arg->raw() == 0;
    return xml_arg->parse(args[0], cache);
  }
public:
  Item_xml_str_func(THD *thd, Item *a, Item *b): Item_str_func(thd, a, b)
  {
    set_maybe_null();
  }
  Item_xml_str_func(THD *thd, Item *a, Item *b, Item *c):
    Item_str_func(thd, a, b, c)
  {
    set_maybe_null();
  }
  bool fix_fields(THD *thd, Item **ref) override;
  bool fix_length_and_dec() override;
  bool const_item() const override
  {
    return const_item_cache && (!nodeset_func || nodeset_func->const_item());
  }
};


class Item_func_xml_extractvalue: public Item_xml_str_func
{
public:
  Item_func_xml_extractvalue(THD *thd, Item *a, Item *b):
    Item_xml_str_func(thd, a, b) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("extractvalue") };
    return name;
  }
  String *val_str(String *) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_xml_extractvalue>(thd, this); }
};


class Item_func_xml_update: public Item_xml_str_func
{
  NativeNodesetBuffer tmp_native_value2;
  String tmp_value3;
  bool collect_result(String *str,
                      const MY_XML_NODE *cut,
                      const String *replace);
public:
  Item_func_xml_update(THD *thd, Item *a, Item *b, Item *c):
    Item_xml_str_func(thd, a, b, c) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("updatexml") };
    return name;
  }
  String *val_str(String *) override;
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_func_xml_update>(thd, this); }
};

#endif /* ITEM_XMLFUNC_INCLUDED */
