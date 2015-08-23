#ifndef ITEM_XMLFUNC_INCLUDED
#define ITEM_XMLFUNC_INCLUDED

/* Copyright (c) 2000-2007 MySQL AB, 2009 Sun Microsystems, Inc.
   Use is subject to license terms.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */


/* This file defines all XML functions */


#ifdef USE_PRAGMA_INTERFACE
#pragma interface			/* gcc class implementation */
#endif


typedef struct my_xml_node_st MY_XML_NODE;


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
    maybe_null= TRUE;
  }
  Item_xml_str_func(THD *thd, Item *a, Item *b, Item *c):
    Item_str_func(thd, a, b, c)
  {
    maybe_null= TRUE;
  }
  bool fix_fields(THD *thd, Item **ref);
  void fix_length_and_dec();
  bool const_item() const
  {
    return const_item_cache && (!nodeset_func || nodeset_func->const_item());
  }
  bool check_vcol_func_processor(uchar *int_arg) 
  {
    return trace_unsupported_by_check_vcol_func_processor(func_name());
  }
};


class Item_func_xml_extractvalue: public Item_xml_str_func
{
public:
  Item_func_xml_extractvalue(THD *thd, Item *a, Item *b):
    Item_xml_str_func(thd, a, b) {}
  const char *func_name() const { return "extractvalue"; }
  String *val_str(String *);
};


class Item_func_xml_update: public Item_xml_str_func
{
  String tmp_value2, tmp_value3;
  bool collect_result(String *str,
                      const MY_XML_NODE *cut,
                      const String *replace);
public:
  Item_func_xml_update(THD *thd, Item *a, Item *b, Item *c):
    Item_xml_str_func(thd, a, b, c) {}
  const char *func_name() const { return "updatexml"; }
  String *val_str(String *);
};

#endif /* ITEM_XMLFUNC_INCLUDED */
