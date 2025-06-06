/* Copyright (c) 2005, 2019, Oracle and/or its affiliates.
   Copyright (c) 2009, 2021, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA */

#include "mariadb.h"
#include "sql_priv.h"
/*
  It is necessary to include set_var.h instead of item.h because there
  are dependencies on include order for set_var.h and item.h. This
  will be resolved later.
*/
#include "sql_class.h"                          // set_var.h: THD
#include "set_var.h"
#include "my_xml.h"
#include "sp_pcontext.h"
#include "sql_class.h"                          // THD

/*
  TODO: future development directions:
  1. add nodeset_to_nodeset_comparator
  2. add lacking functions:
       - name()
       - lang()
       - string()
       - id()
       - translate()
       - local-name()
       - starts-with()
       - namespace-uri()
       - substring-after()
       - normalize-space()
       - substring-before()
  3. add lacking axis:
       - following-sibling
       - following, 
       - preceding-sibling 
       - preceding
*/


/* Structure to store a parsed XML tree */
typedef struct my_xml_node_st
{
  uint level;                 /* level in XML tree, 0 means root node   */
  enum my_xml_node_type type; /* node type: node, or attribute, or text */
  uint parent;                /* link to the parent                     */
  const char *beg;            /* beginning of the name or text          */
  const char *end;            /* end of the name or text                */
  const char *tagend;         /* where this tag ends                    */
} MY_XML_NODE;


/* Lexical analyzer token */
typedef struct my_xpath_lex_st
{
  int        term;  /* token type, see MY_XPATH_LEX_XXXXX below */
  const char *beg;  /* beginning of the token                   */
  const char *end;  /* end of the token                         */
} MY_XPATH_LEX;


/* XPath function creator */
typedef struct my_xpath_function_names_st
{
  const char *name;  /* function name           */
  size_t length;     /* function name length    */
  size_t minargs;    /* min number of arguments */
  size_t maxargs;    /* max number of arguments */
  Item *(*create)(struct my_xpath_st *xpath, Item **args, uint nargs);
} MY_XPATH_FUNC;


/* XPath query parser */
typedef struct my_xpath_st
{
  THD *thd;
  int debug;
  MY_XPATH_LEX query;    /* Whole query                               */
  MY_XPATH_LEX lasttok;  /* last scanned token                        */
  MY_XPATH_LEX prevtok;  /* previous scanned token                    */
  int axis;              /* last scanned axis                         */
  int extra;             /* last scanned "extra", context dependent   */
  MY_XPATH_FUNC *func;   /* last scanned function creator             */
  Item *item;            /* current expression                        */
  Item *context;         /* last scanned context                      */
  Item *rootelement;     /* The root element                          */
  Native *context_cache; /* last context provider                     */
  String *pxml;          /* Parsed XML, an array of MY_XML_NODE       */
  CHARSET_INFO *cs;      /* character set/collation string comparison */
  int error;
} MY_XPATH;


static Type_handler_long_blob type_handler_xpath_nodeset;


/*
  Common features of the functions returning a node set.
*/
class Item_nodeset_func :public Item_str_func
{
protected:
  NativeNodesetBuffer tmp_native_value, tmp2_native_value;
  MY_XPATH_FLT *fltbeg, *fltend;
  MY_XML_NODE *nodebeg, *nodeend;
  uint numnodes;
public:
  String *pxml;
  NativeNodesetBuffer context_cache;
  Item_nodeset_func(THD *thd, String *pxml_arg):
    Item_str_func(thd), pxml(pxml_arg) {}
  Item_nodeset_func(THD *thd, Item *a, String *pxml_arg):
    Item_str_func(thd, a), pxml(pxml_arg) {}
  Item_nodeset_func(THD *thd, Item *a, Item *b, String *pxml_arg):
    Item_str_func(thd, a, b), pxml(pxml_arg) {}
  Item_nodeset_func(THD *thd, Item *a, Item *b, Item *c, String *pxml_arg):
    Item_str_func(thd, a, b, c), pxml(pxml_arg) {}
  void prepare_nodes()
  {
    nodebeg= (MY_XML_NODE*) pxml->ptr();
    nodeend= (MY_XML_NODE*) (pxml->ptr() + pxml->length());
    numnodes= (uint)(nodeend - nodebeg);
  }
  void prepare(THD *thd, Native *nodeset)
  {
    prepare_nodes();
    args[0]->val_native(thd, &tmp_native_value);
    fltbeg= (MY_XPATH_FLT*) tmp_native_value.ptr();
    fltend= (MY_XPATH_FLT*) tmp_native_value.end();
    nodeset->length(0);
  }
  const Type_handler *type_handler() const override
  {
    return &type_handler_xpath_nodeset;
  }
  const Type_handler *fixed_type_handler() const override
  {
    return &type_handler_xpath_nodeset;
  }
  Field *create_tmp_field_ex(MEM_ROOT *root, TABLE *table, Tmp_field_src *src,
                             const Tmp_field_param *param) override
  {
    DBUG_ASSERT(0);
    return NULL;
  }
  String *val_str(String *str) override
  {
    prepare_nodes();
    val_native(current_thd, &tmp2_native_value);
    fltbeg= (MY_XPATH_FLT*) tmp2_native_value.ptr();
    fltend= (MY_XPATH_FLT*) tmp2_native_value.end();
    String active;
    active.alloc(numnodes);
    bzero((char*) active.ptr(), numnodes);
    for (MY_XPATH_FLT *flt= fltbeg; flt < fltend; flt++)
    {
      MY_XML_NODE *node;
      uint j;
      for (j=0, node= nodebeg ; j < numnodes; j++, node++)
      {
        if (node->type == MY_XML_NODE_TEXT &&
            node->parent == flt->num)
          active[j]= 1;
      }
    }

    // Make sure we never return {Ptr=nullptr, str_length=0}
    str->copy("", 0, collation.collation);
    for (uint i=0 ; i < numnodes; i++)
    {
      if(active[i])
      {
        if (str->length())
          str->append(" ", 1, &my_charset_latin1);
        str->append(nodebeg[i].beg, nodebeg[i].end - nodebeg[i].beg);
      }
    }
    return str;
  }
  bool fix_length_and_dec(THD *thd) override
  {
    max_length= MAX_BLOB_WIDTH;
    collation.collation= pxml->charset();
    // To avoid premature evaluation, mark all nodeset functions as non-const.
    used_tables_cache= RAND_TABLE_BIT;
    const_item_cache= false;
    return FALSE;
  }
  LEX_CSTRING func_name_cstring() const override
  {
    return { STRING_WITH_LEN("nodeset") };
  }
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function(func_name(), arg, VCOL_IMPOSSIBLE);
  }

};


/* Returns an XML root */
class Item_nodeset_func_rootelement :public Item_nodeset_func
{
public:
  Item_nodeset_func_rootelement(THD *thd, String *pxml):
    Item_nodeset_func(thd, pxml) {}
  LEX_CSTRING func_name_cstring() const override
  {
    return { STRING_WITH_LEN("xpath_rootelement") };
  }
  bool val_native(THD *thd, Native *nodeset) override;
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_nodeset_func_rootelement>(thd, this); }
};


/* Returns a Union of two node sets */
class Item_nodeset_func_union :public Item_nodeset_func
{
public:
  Item_nodeset_func_union(THD *thd, Item *a, Item *b, String *pxml):
    Item_nodeset_func(thd, a, b, pxml) {}
  LEX_CSTRING func_name_cstring() const override
  {
    return { STRING_WITH_LEN("xpath_union") };
  }
  bool val_native(THD *thd, Native *nodeset) override;
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_nodeset_func_union>(thd, this); }
};


/* Makes one step towards the given axis */
class Item_nodeset_func_axisbyname :public Item_nodeset_func
{
  const char *node_name;
  uint node_namelen;
public:
  Item_nodeset_func_axisbyname(THD *thd, Item *a, const char *n_arg, uint l_arg,
                               String *pxml):
    Item_nodeset_func(thd, a, pxml), node_name(n_arg), node_namelen(l_arg) { }
  LEX_CSTRING func_name_cstring() const override
  {
    return { STRING_WITH_LEN("xpath_axisbyname") };
  }
  bool validname(MY_XML_NODE *n)
  {
    if (node_name[0] == '*')
      return 1;
    return (node_namelen == (uint) (n->end - n->beg)) &&
            !memcmp(node_name, n->beg, node_namelen);
  }
};


/* Returns self */
class Item_nodeset_func_selfbyname: public Item_nodeset_func_axisbyname
{
public:
  Item_nodeset_func_selfbyname(THD *thd, Item *a, const char *n_arg, uint l_arg,
                               String *pxml):
    Item_nodeset_func_axisbyname(thd, a, n_arg, l_arg, pxml) {}
  LEX_CSTRING func_name_cstring() const override
  {
    return { STRING_WITH_LEN("xpath_selfbyname") };
  }
  bool val_native(THD *thd, Native *nodeset) override;
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_nodeset_func_selfbyname>(thd, this); }
};


/* Returns children */
class Item_nodeset_func_childbyname: public Item_nodeset_func_axisbyname
{
public:
  Item_nodeset_func_childbyname(THD *thd, Item *a, const char *n_arg, uint l_arg,
                                String *pxml): 
    Item_nodeset_func_axisbyname(thd, a, n_arg, l_arg, pxml) {}
  LEX_CSTRING func_name_cstring() const override
  {
    return { STRING_WITH_LEN("xpath_childbyname") };
  }
  bool val_native(THD *thd, Native *nodeset) override;
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_nodeset_func_childbyname>(thd, this); }
};


/* Returns descendants */
class Item_nodeset_func_descendantbyname: public Item_nodeset_func_axisbyname
{
  bool need_self;
public:
  Item_nodeset_func_descendantbyname(THD *thd, Item *a, const char *n_arg, uint l_arg,
                                     String *pxml, bool need_self_arg):
    Item_nodeset_func_axisbyname(thd, a, n_arg, l_arg, pxml),
      need_self(need_self_arg) {}
  LEX_CSTRING func_name_cstring() const override
  {
    return { STRING_WITH_LEN("xpath_descendantbyname") };
  }
  bool val_native(THD *thd, Native *nodeset) override;
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_nodeset_func_descendantbyname>(thd, this); }
};


/* Returns ancestors */
class Item_nodeset_func_ancestorbyname: public Item_nodeset_func_axisbyname
{
  bool need_self;
public:
  Item_nodeset_func_ancestorbyname(THD *thd, Item *a, const char *n_arg, uint l_arg,
                                   String *pxml, bool need_self_arg):
    Item_nodeset_func_axisbyname(thd, a, n_arg, l_arg, pxml),
      need_self(need_self_arg) {}
  LEX_CSTRING func_name_cstring() const override
  {
    return { STRING_WITH_LEN("xpath_ancestorbyname") };
  }
  bool val_native(THD *thd, Native *nodeset) override;
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_nodeset_func_ancestorbyname>(thd, this); }
};


/* Returns parents */
class Item_nodeset_func_parentbyname: public Item_nodeset_func_axisbyname
{
public:
  Item_nodeset_func_parentbyname(THD *thd, Item *a, const char *n_arg, uint l_arg,
                                 String *pxml):
    Item_nodeset_func_axisbyname(thd, a, n_arg, l_arg, pxml) {}

  LEX_CSTRING func_name_cstring() const override
  {
    return { STRING_WITH_LEN("xpath_parentbyname") };
  }
  bool val_native(THD *thd, Native *nodeset) override;
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_nodeset_func_parentbyname>(thd, this); }
};


/* Returns attributes */
class Item_nodeset_func_attributebyname: public Item_nodeset_func_axisbyname
{
public:
  Item_nodeset_func_attributebyname(THD *thd, Item *a, const char *n_arg,
                                    uint l_arg, String *pxml):
    Item_nodeset_func_axisbyname(thd, a, n_arg, l_arg, pxml) {}
  LEX_CSTRING func_name_cstring() const override
  {
    return { STRING_WITH_LEN("xpath_attributebyname") };
  }
  bool val_native(THD *thd, Native *nodeset) override;
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_nodeset_func_attributebyname>(thd, this); }
};


/*
  Condition iterator: goes through all nodes in the current
  context and checks a condition, returning those nodes
  giving TRUE condition result.
*/
class Item_nodeset_func_predicate :public Item_nodeset_func
{
public:
  Item_nodeset_func_predicate(THD *thd, Item *a, Item *b, String *pxml):
    Item_nodeset_func(thd, a, b, pxml) {}
  LEX_CSTRING func_name_cstring() const override
  {
    return { STRING_WITH_LEN("xpath_predicate") };
  }
  bool val_native(THD *thd, Native *nodeset) override;
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_nodeset_func_predicate>(thd, this); }
};


/* Selects nodes with a given position in context */
class Item_nodeset_func_elementbyindex :public Item_nodeset_func
{
public:
  Item_nodeset_func_elementbyindex(THD *thd, Item *a, Item *b, String *pxml):
    Item_nodeset_func(thd, a, b, pxml) { }
  LEX_CSTRING func_name_cstring() const override
  {
    return { STRING_WITH_LEN("xpath_elementbyindex") };
  }
  bool val_native(THD *thd, Native *nodeset) override;
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_nodeset_func_elementbyindex>(thd, this); }
};


/*
  Converts its argument into a boolean value.
  * a number is true if it is non-zero
  * a node-set is true if and only if it is non-empty
  * a string is true if and only if its length is non-zero
*/
class Item_xpath_cast_bool :public Item_bool_func
{
  String *pxml;
  NativeNodesetBuffer tmp_native_value;
public:
  Item_xpath_cast_bool(THD *thd, Item *a, String *pxml_arg):
    Item_bool_func(thd, a), pxml(pxml_arg) {}
  LEX_CSTRING func_name_cstring() const override
  {
    return { STRING_WITH_LEN("xpath_cast_bool") };
  }
  bool val_bool() override
  {
    if (args[0]->fixed_type_handler() == &type_handler_xpath_nodeset)
    {
      args[0]->val_native(current_thd, &tmp_native_value);
      return tmp_native_value.elements() == 1 ? 1 : 0;
    }
    return args[0]->val_real() ? 1 : 0;
  }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_xpath_cast_bool>(thd, this); }
};


/*
  Converts its argument into a number
*/
class Item_xpath_cast_number :public Item_real_func
{
public:
  Item_xpath_cast_number(THD *thd, Item *a): Item_real_func(thd, a) {}
  LEX_CSTRING func_name_cstring() const override
  {
    return { STRING_WITH_LEN("xpath_cast_number") };
  }
  double val_real() override { return args[0]->val_real(); }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_xpath_cast_number>(thd, this); }
};


/*
  Context cache, for predicate
*/
class Item_nodeset_context_cache :public Item_nodeset_func
{
public:
  Native *native_cache;
  Item_nodeset_context_cache(THD *thd, Native *native_arg, String *pxml):
    Item_nodeset_func(thd, pxml), native_cache(native_arg) { }
  bool val_native(THD *, Native *nodeset) override
  {
    return nodeset->copy(*native_cache);
  }
  bool fix_length_and_dec(THD *thd) override
  { max_length= MAX_BLOB_WIDTH; return FALSE; }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_nodeset_context_cache>(thd, this); }
};


class Item_func_xpath_position :public Item_long_func
{
  String *pxml;
  NativeNodesetBuffer tmp_native_value;
public:
  Item_func_xpath_position(THD *thd, Item *a, String *p):
    Item_long_func(thd, a), pxml(p) {}
  LEX_CSTRING func_name_cstring() const override
  {
    return { STRING_WITH_LEN("xpath_position") };
  }
  bool fix_length_and_dec(THD *thd) override { max_length=10; return FALSE; }
  longlong val_int() override
  {
    args[0]->val_native(current_thd, &tmp_native_value);
    if (tmp_native_value.elements() == 1)
      return tmp_native_value.element(0).pos + 1;
    return 0;
  }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_xpath_position>(thd, this); }
};


class Item_func_xpath_count :public Item_long_func
{
  String *pxml;
  NativeNodesetBuffer tmp_native_value;
public:
  Item_func_xpath_count(THD *thd, Item *a, String *p):
    Item_long_func(thd, a), pxml(p) {}
  LEX_CSTRING func_name_cstring() const override
  {
    return { STRING_WITH_LEN("xpath_count") };
  }
  bool fix_length_and_dec(THD *thd) override { max_length=10; return FALSE; }
  longlong val_int() override
  {
    uint predicate_supplied_context_size;
    args[0]->val_native(current_thd, &tmp_native_value);
    if (tmp_native_value.elements() == 1 &&
        (predicate_supplied_context_size= tmp_native_value.element(0).size))
      return predicate_supplied_context_size;
    return tmp_native_value.elements();
  }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_xpath_count>(thd, this); }
};


class Item_func_xpath_sum :public Item_real_func
{
  String *pxml;
  NativeNodesetBuffer tmp_native_value;
public:
  Item_func_xpath_sum(THD *thd, Item *a, String *p):
    Item_real_func(thd, a), pxml(p) {}

  LEX_CSTRING func_name_cstring() const override
  {
    return { STRING_WITH_LEN("xpath_sum") };
  }
  double val_real() override
  {
    double sum= 0;
    args[0]->val_native(current_thd, &tmp_native_value);
    MY_XPATH_FLT *fltbeg= (MY_XPATH_FLT*) tmp_native_value.ptr();
    MY_XPATH_FLT *fltend= (MY_XPATH_FLT*) tmp_native_value.end();
    uint numnodes= pxml->length() / sizeof(MY_XML_NODE);
    MY_XML_NODE *nodebeg= (MY_XML_NODE*) pxml->ptr();
  
    for (MY_XPATH_FLT *flt= fltbeg; flt < fltend; flt++)
    {
      MY_XML_NODE *self= &nodebeg[flt->num];
      for (uint j= flt->num + 1; j < numnodes; j++)
      {
        MY_XML_NODE *node= &nodebeg[j];
        if (node->level <= self->level)
          break;
        if ((node->parent == flt->num) &&
            (node->type == MY_XML_NODE_TEXT))
        {
          char *end;
          int err;
          double add= collation.collation->strntod((char*) node->beg,
                                                   node->end - node->beg, &end, &err);
          if (!err)
            sum+= add;
        }
      }
    }
    return sum;
  }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_xpath_sum>(thd, this); }
};


/**
  A string whose value may be changed during execution.
*/
class Item_string_xml_non_const: public Item_string
{
public:
  Item_string_xml_non_const(THD *thd, const char *str, uint length,
                            CHARSET_INFO *cs):
    Item_string(thd, str, length, cs)
  { }
  bool const_item() const override { return false ; }
  bool basic_const_item() const override { return false; }
  void set_value(const char *str, uint length, CHARSET_INFO *cs)
  {
    str_value.set(str, length, cs);
  }
  Item *safe_charset_converter(THD *thd, CHARSET_INFO *tocs) override
  {
    /*
      Item_string::safe_charset_converter() does not accept non-constants.
      Note, conversion is not really needed here anyway.
    */
    return this;
  }
};


class Item_nodeset_to_const_comparator :public Item_bool_func
{
  String *pxml;
  NativeNodesetBuffer tmp_nodeset;
public:
  Item_nodeset_to_const_comparator(THD *thd, Item *nodeset, Item *cmpfunc,
                                   String *p):
    Item_bool_func(thd, nodeset, cmpfunc), pxml(p) {}
  LEX_CSTRING func_name_cstring() const override
  {
    return { STRING_WITH_LEN("xpath_nodeset_to_const_comparator") };
  }
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function(func_name(), arg, VCOL_IMPOSSIBLE);
  }
  Field *create_tmp_field_ex(MEM_ROOT *root, TABLE *table, Tmp_field_src *src,
                             const Tmp_field_param *param) override
  {
    DBUG_ASSERT(0);
    return NULL;
  }
  bool val_bool() override
  {
    Item_func *comp= (Item_func*)args[1];
    Item_string_xml_non_const *fake=
      (Item_string_xml_non_const*)(comp->arguments()[0]);
    args[0]->val_native(current_thd, &tmp_nodeset);
    MY_XPATH_FLT *fltbeg= (MY_XPATH_FLT*) tmp_nodeset.ptr();
    MY_XPATH_FLT *fltend= (MY_XPATH_FLT*) tmp_nodeset.end();
    MY_XML_NODE *nodebeg= (MY_XML_NODE*) pxml->ptr();
    uint numnodes= pxml->length() / sizeof(MY_XML_NODE);

    for (MY_XPATH_FLT *flt= fltbeg; flt < fltend; flt++)
    {
      MY_XML_NODE *self= &nodebeg[flt->num];
      for (uint j= flt->num + 1; j < numnodes; j++)
      {
        MY_XML_NODE *node= &nodebeg[j];
        if (node->level <= self->level)
          break;
        if ((node->parent == flt->num) &&
            (node->type == MY_XML_NODE_TEXT))
        {
          fake->set_value(node->beg, (uint)(node->end - node->beg),
                          collation.collation);
          if (args[1]->val_int())
            return 1;
        }
      }
    }
    return 0;
  }
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_nodeset_to_const_comparator>(thd, this); }
};


bool Item_nodeset_func_rootelement::val_native(THD *thd, Native *nodeset)
{
  nodeset->length(0);
  return MY_XPATH_FLT(0, 0).append_to(nodeset);
}


bool Item_nodeset_func_union::val_native(THD *thd, Native *nodeset)
{
  uint num_nodes= pxml->length() / sizeof(MY_XML_NODE);
  NativeNodesetBuffer set0, set1;
  args[0]->val_native(thd, &set0);
  args[1]->val_native(thd, &set1);
  String both_str;
  both_str.alloc(num_nodes);
  char *both= (char*) both_str.ptr();
  bzero((void*)both, num_nodes);
  MY_XPATH_FLT *flt;

  fltbeg= (MY_XPATH_FLT*) set0.ptr();
  fltend= (MY_XPATH_FLT*) set0.end();
  for (flt= fltbeg; flt < fltend; flt++)
    both[flt->num]= 1;

  fltbeg= (MY_XPATH_FLT*) set1.ptr();
  fltend= (MY_XPATH_FLT*) set1.end();
  for (flt= fltbeg; flt < fltend; flt++)
    both[flt->num]= 1;

  nodeset->length(0);
  for (uint i= 0, pos= 0; i < num_nodes; i++)
  {
    if (both[i])
     MY_XPATH_FLT(i, pos++).append_to(nodeset);
  }
  return false;
}


bool Item_nodeset_func_selfbyname::val_native(THD *thd, Native *nodeset)
{
  prepare(thd, nodeset);
  for (MY_XPATH_FLT *flt= fltbeg; flt < fltend; flt++)
  {
    uint pos= 0;
    MY_XML_NODE *self= &nodebeg[flt->num];
    if (validname(self))
      MY_XPATH_FLT(flt->num, pos++).append_to(nodeset);
  }
  return false;
}


bool Item_nodeset_func_childbyname::val_native(THD *thd, Native *nodeset)
{
  prepare(thd, nodeset);
  for (MY_XPATH_FLT *flt= fltbeg; flt < fltend; flt++)
  {
    MY_XML_NODE *self= &nodebeg[flt->num];
    for (uint pos= 0, j= flt->num + 1 ; j < numnodes; j++)
    {
      MY_XML_NODE *node= &nodebeg[j];
      if (node->level <= self->level)
        break;
      if ((node->parent == flt->num) &&
          (node->type == MY_XML_NODE_TAG) &&
          validname(node))
        MY_XPATH_FLT(j, pos++).append_to(nodeset);
    }
  }
  return false;
}


bool Item_nodeset_func_descendantbyname::val_native(THD *thd, Native *nodeset)
{
  prepare(thd, nodeset);
  for (MY_XPATH_FLT *flt= fltbeg; flt < fltend; flt++)
  {
    uint pos= 0;
    MY_XML_NODE *self= &nodebeg[flt->num];
    if (need_self && validname(self))
      MY_XPATH_FLT(flt->num, pos++).append_to(nodeset);
    for (uint j= flt->num + 1 ; j < numnodes ; j++)
    {
      MY_XML_NODE *node= &nodebeg[j];
      if (node->level <= self->level)
        break;
      if ((node->type == MY_XML_NODE_TAG) && validname(node))
        MY_XPATH_FLT(j, pos++).append_to(nodeset);
    }
  }
  return false;
}


bool Item_nodeset_func_ancestorbyname::val_native(THD *thd, Native *nodeset)
{
  char *active;
  String active_str;
  prepare(thd, nodeset);
  active_str.alloc(numnodes);
  active= (char*) active_str.ptr();
  bzero((void*)active, numnodes);
  uint pos= 0;

  for (MY_XPATH_FLT *flt= fltbeg; flt < fltend; flt++)
  {
    /*
       Go to the root and add all nodes on the way.
       Don't add the root if context is the root itself
    */
    MY_XML_NODE *self= &nodebeg[flt->num];
    if (need_self && validname(self))
    {
      active[flt->num]= 1;
      pos++;
    }

    for (uint j= self->parent; nodebeg[j].parent != j; j= nodebeg[j].parent)
    {
      if (flt->num && validname(&nodebeg[j]))
      {
        active[j]= 1;
        pos++;
      }
    }
  }

  for (uint j= 0; j < numnodes ; j++)
  {
    if (active[j])
      MY_XPATH_FLT(j, --pos).append_to(nodeset);
  }
  return false;
}


bool Item_nodeset_func_parentbyname::val_native(THD *thd, Native *nodeset)
{
  char *active;
  String active_str;
  prepare(thd, nodeset);
  active_str.alloc(numnodes);
  active= (char*) active_str.ptr();
  bzero((void*)active, numnodes);
  for (MY_XPATH_FLT *flt= fltbeg; flt < fltend; flt++)
  {
    uint j= nodebeg[flt->num].parent;
    if (flt->num && validname(&nodebeg[j]))
        active[j]= 1;
  }
  for (uint j= 0, pos= 0; j < numnodes ; j++)
  {
    if (active[j])
      MY_XPATH_FLT(j, pos++).append_to(nodeset);
  }
  return false;
}


bool Item_nodeset_func_attributebyname::val_native(THD *thd, Native *nodeset)
{
  prepare(thd, nodeset);
  for (MY_XPATH_FLT *flt= fltbeg; flt < fltend; flt++)
  {
    MY_XML_NODE *self= &nodebeg[flt->num];
    for (uint pos=0, j= flt->num + 1 ; j < numnodes; j++)
    {
      MY_XML_NODE *node= &nodebeg[j];
      if (node->level <= self->level)
        break;
      if ((node->parent == flt->num) &&
         (node->type == MY_XML_NODE_ATTR) &&
          validname(node))
        MY_XPATH_FLT(j, pos++).append_to(nodeset);
    }
  }
  return false;
}


bool Item_nodeset_func_predicate::val_native(THD *thd, Native *str)
{
  Item_nodeset_func *nodeset_func= (Item_nodeset_func*) args[0];
  uint pos= 0, size;
  prepare(thd, str);
  size= (uint)(fltend - fltbeg);
  for (MY_XPATH_FLT *flt= fltbeg; flt < fltend; flt++)
  {
    nodeset_func->context_cache.length(0);
    MY_XPATH_FLT(flt->num, flt->pos, size).
      append_to(&nodeset_func->context_cache);
    if (args[1]->val_int())
      MY_XPATH_FLT(flt->num, pos++).append_to(str);
  }
  return false;
}


bool Item_nodeset_func_elementbyindex::val_native(THD *thd, Native *nodeset)
{
  Item_nodeset_func *nodeset_func= (Item_nodeset_func*) args[0];
  prepare(thd, nodeset);
  MY_XPATH_FLT *flt;
  uint pos, size= (uint)(fltend - fltbeg);
  for (pos= 0, flt= fltbeg; flt < fltend; flt++)
  {
    nodeset_func->context_cache.length(0);
    MY_XPATH_FLT(flt->num, flt->pos, size).
      append_to(&nodeset_func->context_cache);
    int index= (int) (args[1]->val_int()) - 1;
    if (index >= 0 &&
        (flt->pos == (uint) index ||
         (args[1]->type_handler()->is_bool_type())))
      MY_XPATH_FLT(flt->num, pos++).append_to(nodeset);
  }
  return false;
}


/*
  If item is a node set, then casts it to boolean,
  otherwise returns the item itself.
*/
static Item* nodeset2bool(MY_XPATH *xpath, Item *item)
{
  if (item->fixed_type_handler() == &type_handler_xpath_nodeset)
    return new (xpath->thd->mem_root)
      Item_xpath_cast_bool(xpath->thd, item, xpath->pxml);
  return item;
}


/*
  XPath lexical tokens
*/
#define MY_XPATH_LEX_DIGITS   'd'
#define MY_XPATH_LEX_IDENT    'i'
#define MY_XPATH_LEX_STRING   's'
#define MY_XPATH_LEX_SLASH    '/'
#define MY_XPATH_LEX_LB       '['
#define MY_XPATH_LEX_RB       ']'
#define MY_XPATH_LEX_LP       '('
#define MY_XPATH_LEX_RP       ')'
#define MY_XPATH_LEX_EQ       '='
#define MY_XPATH_LEX_LESS     '<'
#define MY_XPATH_LEX_GREATER  '>'
#define MY_XPATH_LEX_AT       '@'
#define MY_XPATH_LEX_COLON    ':'
#define MY_XPATH_LEX_ASTERISK '*'
#define MY_XPATH_LEX_DOT      '.'
#define MY_XPATH_LEX_VLINE    '|'
#define MY_XPATH_LEX_MINUS    '-'
#define MY_XPATH_LEX_PLUS     '+'
#define MY_XPATH_LEX_EXCL     '!'
#define MY_XPATH_LEX_COMMA    ','
#define MY_XPATH_LEX_DOLLAR   '$'
#define MY_XPATH_LEX_ERROR    'A'
#define MY_XPATH_LEX_EOF      'B'
#define MY_XPATH_LEX_AND      'C'
#define MY_XPATH_LEX_OR       'D'
#define MY_XPATH_LEX_DIV      'E'
#define MY_XPATH_LEX_MOD      'F'
#define MY_XPATH_LEX_FUNC     'G'
#define MY_XPATH_LEX_NODETYPE 'H'
#define MY_XPATH_LEX_AXIS     'I'
#define MY_XPATH_LEX_LE       'J'
#define MY_XPATH_LEX_GE       'K'


/*
  XPath axis type
*/
#define MY_XPATH_AXIS_ANCESTOR            0
#define MY_XPATH_AXIS_ANCESTOR_OR_SELF    1
#define MY_XPATH_AXIS_ATTRIBUTE           2
#define MY_XPATH_AXIS_CHILD               3
#define MY_XPATH_AXIS_DESCENDANT          4
#define MY_XPATH_AXIS_DESCENDANT_OR_SELF  5
#define MY_XPATH_AXIS_FOLLOWING           6
#define MY_XPATH_AXIS_FOLLOWING_SIBLING   7
#define MY_XPATH_AXIS_NAMESPACE           8
#define MY_XPATH_AXIS_PARENT              9
#define MY_XPATH_AXIS_PRECEDING          10
#define MY_XPATH_AXIS_PRECEDING_SIBLING  11
#define MY_XPATH_AXIS_SELF               12


/*
  Create scalar comparator

  SYNOPSYS
    Create a comparator function for scalar arguments,
    for the given arguments and operation.

  RETURN
    The newly created item.
*/
static Item *eq_func(THD *thd, int oper, Item *a, Item *b)
{
  MEM_ROOT *mem_root= thd->mem_root;
  switch (oper)
  {
    case '=': return new (mem_root) Item_func_eq(thd, a, b);
    case '!': return new (mem_root) Item_func_ne(thd, a, b);
    case MY_XPATH_LEX_GE: return new (mem_root) Item_func_ge(thd, a, b);
    case MY_XPATH_LEX_LE: return new (mem_root) Item_func_le(thd, a, b);
    case MY_XPATH_LEX_GREATER: return new (mem_root) Item_func_gt(thd, a, b);
    case MY_XPATH_LEX_LESS: return new (mem_root) Item_func_lt(thd, a, b);
  }
  return 0;
}


/*
  Create scalar comparator

  SYNOPSYS
    Create a comparator function for scalar arguments,
    for the given arguments and reverse operation, e.g.

    A > B  is converted into  B < A

  RETURN
    The newly created item.
*/
static Item *eq_func_reverse(THD *thd, int oper, Item *a, Item *b)
{
  MEM_ROOT *mem_root= thd->mem_root;
  switch (oper)
  {
    case '=': return new (mem_root) Item_func_eq(thd, a, b);
    case '!': return new (mem_root) Item_func_ne(thd, a, b);
    case MY_XPATH_LEX_GE: return new (mem_root) Item_func_le(thd, a, b);
    case MY_XPATH_LEX_LE: return new (mem_root) Item_func_ge(thd, a, b);
    case MY_XPATH_LEX_GREATER: return new (mem_root) Item_func_lt(thd, a, b);
    case MY_XPATH_LEX_LESS: return new (mem_root) Item_func_gt(thd, a, b);
  }
  return 0;
}


/*
  Create a comparator

  SYNOPSYS
    Create a comparator for scalar or non-scalar arguments,
    for the given arguments and operation.

  RETURN
    The newly created item.
*/
static Item *create_comparator(MY_XPATH *xpath,
                               int oper, MY_XPATH_LEX *context,
                               Item *a, Item *b)
{
  if (a->fixed_type_handler() != &type_handler_xpath_nodeset &&
      b->fixed_type_handler() != &type_handler_xpath_nodeset)
  {
    return eq_func(xpath->thd, oper, a, b); // two scalar arguments
  }
  else if (a->fixed_type_handler() == &type_handler_xpath_nodeset &&
           b->fixed_type_handler() == &type_handler_xpath_nodeset)
  {
    uint len= (uint)(xpath->query.end - context->beg);
    if (len <= 32)
      my_printf_error(ER_UNKNOWN_ERROR,
                      "XPATH error: "
                      "comparison of two nodesets is not supported: '%.*s'",
                      MYF(0), len, context->beg);
    else
      my_printf_error(ER_UNKNOWN_ERROR,
                      "XPATH error: "
                      "comparison of two nodesets is not supported: '%.32sT'",
                      MYF(0), context->beg);

    return 0; // TODO: Comparison of two nodesets
  }
  else
  {
    /*
     Compare a node set to a scalar value.
     We just create a fake Item_string_xml_non_const() argument,
     which will be filled to the particular value
     in a loop through all of the nodes in the node set.
    */

    THD *thd= xpath->thd;
    Item_string *fake= (new (thd->mem_root)
                        Item_string_xml_non_const(thd, "", 0, xpath->cs));
    Item_nodeset_func *nodeset;
    Item *scalar, *comp;
    if (a->fixed_type_handler() == &type_handler_xpath_nodeset)
    {
      nodeset= (Item_nodeset_func*) a;
      scalar= b;
      comp= eq_func(thd, oper, (Item*)fake, scalar);
    }
    else
    {
      nodeset= (Item_nodeset_func*) b;
      scalar= a;
      comp= eq_func_reverse(thd, oper, fake, scalar);
    }
    return (new (thd->mem_root)
            Item_nodeset_to_const_comparator(thd, nodeset, comp, xpath->pxml));
  }
}


/*
  Create a step

  SYNOPSYS
    Create a step function for the given argument and axis.

  RETURN
    The newly created item.
*/
static Item* nametestfunc(MY_XPATH *xpath,
                          int type, Item *arg, const char *beg, uint len)
{
  THD *thd= xpath->thd;
  MEM_ROOT *mem_root= thd->mem_root;

  DBUG_ASSERT(arg != 0);
  DBUG_ASSERT(arg->fixed_type_handler() == &type_handler_xpath_nodeset);
  DBUG_ASSERT(beg != 0);
  DBUG_ASSERT(len > 0);

  Item *res;
  switch (type)
  {
  case MY_XPATH_AXIS_ANCESTOR:
    res= new (mem_root) Item_nodeset_func_ancestorbyname(thd, arg, beg, len,
                                              xpath->pxml, 0);
    break;
  case MY_XPATH_AXIS_ANCESTOR_OR_SELF:
    res= new (mem_root) Item_nodeset_func_ancestorbyname(thd, arg, beg, len,
                                              xpath->pxml, 1);
    break;
  case MY_XPATH_AXIS_PARENT:
    res= new (mem_root) Item_nodeset_func_parentbyname(thd, arg, beg, len,
                                            xpath->pxml);
    break;
  case MY_XPATH_AXIS_DESCENDANT:
    res= new (mem_root) Item_nodeset_func_descendantbyname(thd, arg, beg, len,
                                                xpath->pxml, 0);
    break;
  case MY_XPATH_AXIS_DESCENDANT_OR_SELF:
    res= new (mem_root) Item_nodeset_func_descendantbyname(thd, arg, beg, len,
                                                xpath->pxml, 1);
    break;
  case MY_XPATH_AXIS_ATTRIBUTE:
    res= new (mem_root) Item_nodeset_func_attributebyname(thd, arg, beg, len,
                                               xpath->pxml);
    break;
  case MY_XPATH_AXIS_SELF:
    res= new (mem_root) Item_nodeset_func_selfbyname(thd, arg, beg, len,
                                          xpath->pxml);
    break;
  default:
    res= new (mem_root) Item_nodeset_func_childbyname(thd, arg, beg, len,
                                           xpath->pxml);
  }
  return res;
}


/*
  Tokens consisting of one character, for faster lexical analyzer.
*/
static char simpletok[128]=
{
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
/*
    ! " # $ % & ' ( ) * + , - . / 0 1 2 3 4 5 6 7 8 9 : ; < = > ?
  @ A B C D E F G H I J K L M N O P Q R S T U V W X Y Z [ \ ] ^ _
  ` a b c d e f g h i j k l m n o p q r s t u v w x y z { | } ~ \200
*/
  0,1,0,0,1,0,0,0,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,1,0,1,1,1,0,
  1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,1,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0  
};


/*
  XPath keywords
*/
struct my_xpath_keyword_names_st
{
  int tok;
  const char *name;
  size_t length;
  int extra;
};


static struct my_xpath_keyword_names_st my_keyword_names[] =
{
  {MY_XPATH_LEX_AND     , "and"                    ,  3, 0 },
  {MY_XPATH_LEX_OR      , "or"                     ,  2, 0 },
  {MY_XPATH_LEX_DIV     , "div"                    ,  3, 0 },
  {MY_XPATH_LEX_MOD     , "mod"                    ,  3, 0 },
  {0,NULL,0,0}
};  


static struct my_xpath_keyword_names_st my_axis_names[]=
{
  {MY_XPATH_LEX_AXIS,"ancestor"          , 8,MY_XPATH_AXIS_ANCESTOR          },
  {MY_XPATH_LEX_AXIS,"ancestor-or-self"  ,16,MY_XPATH_AXIS_ANCESTOR_OR_SELF  },
  {MY_XPATH_LEX_AXIS,"attribute"         , 9,MY_XPATH_AXIS_ATTRIBUTE         },
  {MY_XPATH_LEX_AXIS,"child"             , 5,MY_XPATH_AXIS_CHILD             },
  {MY_XPATH_LEX_AXIS,"descendant"        ,10,MY_XPATH_AXIS_DESCENDANT        },
  {MY_XPATH_LEX_AXIS,"descendant-or-self",18,MY_XPATH_AXIS_DESCENDANT_OR_SELF},
  {MY_XPATH_LEX_AXIS,"following"         , 9,MY_XPATH_AXIS_FOLLOWING         },
  {MY_XPATH_LEX_AXIS,"following-sibling" ,17,MY_XPATH_AXIS_FOLLOWING_SIBLING },
  {MY_XPATH_LEX_AXIS,"namespace"         , 9,MY_XPATH_AXIS_NAMESPACE         },
  {MY_XPATH_LEX_AXIS,"parent"            , 6,MY_XPATH_AXIS_PARENT            },
  {MY_XPATH_LEX_AXIS,"preceding"         , 9,MY_XPATH_AXIS_PRECEDING         },
  {MY_XPATH_LEX_AXIS,"preceding-sibling" ,17,MY_XPATH_AXIS_PRECEDING_SIBLING },
  {MY_XPATH_LEX_AXIS,"self"              , 4,MY_XPATH_AXIS_SELF              },
  {0,NULL,0,0}
};


static struct my_xpath_keyword_names_st my_nodetype_names[]=
{
  {MY_XPATH_LEX_NODETYPE, "comment"                ,  7, 0 },
  {MY_XPATH_LEX_NODETYPE, "text"                   ,  4, 0 },
  {MY_XPATH_LEX_NODETYPE, "processing-instruction" ,  22,0 },
  {MY_XPATH_LEX_NODETYPE, "node"                   ,  4, 0 },
  {0,NULL,0,0}
};


/*
  Lookup a keyword

  SYNOPSYS
    Check that the last scanned identifier is a keyword.

  RETURN
    - Token type, on lookup success.
    - MY_XPATH_LEX_IDENT, on lookup failure.
*/
static int
my_xpath_keyword(MY_XPATH *x,
                 struct my_xpath_keyword_names_st *keyword_names,
                 const char *beg, const char *end)
{
  struct my_xpath_keyword_names_st *k;
  size_t length= end-beg;
  for (k= keyword_names; k->name; k++)
  {
    if (length == k->length && !strncasecmp(beg, k->name, length))
    {
      x->extra= k->extra;
      return k->tok;
    }
  }
  return MY_XPATH_LEX_IDENT;
}


/*
  Functions to create an item, a-la those in item_create.cc
*/

static Item *create_func_true(MY_XPATH *xpath, Item **args, uint nargs)
{
  return (Item*) Item_true;
}


static Item *create_func_false(MY_XPATH *xpath, Item **args, uint nargs)
{
  return (Item*) Item_false;
}


static Item *create_func_not(MY_XPATH *xpath, Item **args, uint nargs)
{
  return new (xpath->thd->mem_root)
    Item_func_not(xpath->thd, nodeset2bool(xpath, args[0]));
}


static Item *create_func_ceiling(MY_XPATH *xpath, Item **args, uint nargs)
{
  return new (xpath->thd->mem_root) Item_func_ceiling(xpath->thd, args[0]);
}


static Item *create_func_floor(MY_XPATH *xpath, Item **args, uint nargs)
{
  return new (xpath->thd->mem_root) Item_func_floor(xpath->thd, args[0]);
}


static Item *create_func_bool(MY_XPATH *xpath, Item **args, uint nargs)
{
  return new (xpath->thd->mem_root)
    Item_xpath_cast_bool(xpath->thd, args[0], xpath->pxml);
}


static Item *create_func_number(MY_XPATH *xpath, Item **args, uint nargs)
{
  return new (xpath->thd->mem_root)
    Item_xpath_cast_number(xpath->thd, args[0]);
}


static Item *create_func_string_length(MY_XPATH *xpath, Item **args,
                                       uint nargs)
{
  Item *arg= nargs ? args[0] : xpath->context;
  return arg ? new (xpath->thd->mem_root)
    Item_func_char_length(xpath->thd, arg) : 0;
}


static Item *create_func_round(MY_XPATH *xpath, Item **args, uint nargs)
{
  return new (xpath->thd->mem_root)
    Item_func_round(xpath->thd, args[0],
                    new (xpath->thd->mem_root)
                    Item_int(xpath->thd, (char *) "0", 0, 1), 0);
}


static Item *create_func_last(MY_XPATH *xpath, Item **args, uint nargs)
{
  return (xpath->context ?
          new (xpath->thd->mem_root)
          Item_func_xpath_count(xpath->thd, xpath->context, xpath->pxml) :
          NULL);
}


static Item *create_func_position(MY_XPATH *xpath, Item **args, uint nargs)
{
  return (xpath->context ?
          new (xpath->thd->mem_root)
          Item_func_xpath_position(xpath->thd, xpath->context, xpath->pxml) :
          NULL);
}


static Item *create_func_contains(MY_XPATH *xpath, Item **args, uint nargs)
{
  return (new (xpath->thd->mem_root)
          Item_xpath_cast_bool(xpath->thd,
                               new (xpath->thd->mem_root)
                               Item_func_locate(xpath->thd, args[0], args[1]),
                               xpath->pxml));
}


static Item *create_func_concat(MY_XPATH *xpath, Item **args, uint nargs)
{
  return new (xpath->thd->mem_root)
    Item_func_concat(xpath->thd, args[0], args[1]);
}


static Item *create_func_substr(MY_XPATH *xpath, Item **args, uint nargs)
{
  THD *thd= xpath->thd;
  if (nargs == 2)
    return new (thd->mem_root) Item_func_substr(thd, args[0], args[1]);
  return new (thd->mem_root) Item_func_substr(thd, args[0], args[1], args[2]);
}


static Item *create_func_count(MY_XPATH *xpath, Item **args, uint nargs)
{
  if (args[0]->fixed_type_handler() != &type_handler_xpath_nodeset)
    return 0;
  return new (xpath->thd->mem_root) Item_func_xpath_count(xpath->thd, args[0], xpath->pxml);
}


static Item *create_func_sum(MY_XPATH *xpath, Item **args, uint nargs)
{
  if (args[0]->fixed_type_handler() != &type_handler_xpath_nodeset)
    return 0;
  return new (xpath->thd->mem_root)
    Item_func_xpath_sum(xpath->thd, args[0], xpath->pxml);
}


/*
  Functions names. Separate lists for names with
  lengths 3,4,5 and 6 for faster lookups.
*/
static MY_XPATH_FUNC my_func_names3[]=
{
  {"sum", 3, 1 , 1  , create_func_sum},
  {"not", 3, 1 , 1  , create_func_not},
  {0    , 0, 0 , 0, 0}
};


static MY_XPATH_FUNC my_func_names4[]=
{
  {"last", 4, 0, 0, create_func_last},
  {"true", 4, 0, 0, create_func_true},
  {"name", 4, 0, 1, 0},
  {"lang", 4, 1, 1, 0},
  {0     , 0, 0, 0, 0}
};


static MY_XPATH_FUNC my_func_names5[]=
{
  {"count", 5, 1, 1, create_func_count},
  {"false", 5, 0, 0, create_func_false},
  {"floor", 5, 1, 1, create_func_floor},
  {"round", 5, 1, 1, create_func_round},
  {0      , 0, 0, 0, 0}
};


static MY_XPATH_FUNC my_func_names6[]=
{
  {"concat", 6, 2, 255, create_func_concat},
  {"number", 6, 0, 1  , create_func_number},
  {"string", 6, 0, 1  , 0},
  {0       , 0, 0, 0  , 0}
};


/* Other functions, with name longer than 6, all together */
static MY_XPATH_FUNC my_func_names[] =
{
  {"id"               , 2  ,  1 , 1  , 0},
  {"boolean"          , 7  ,  1 , 1  , create_func_bool},
  {"ceiling"          , 7  ,  1 , 1  , create_func_ceiling},
  {"position"         , 8  ,  0 , 0  , create_func_position},
  {"contains"         , 8  ,  2 , 2  , create_func_contains},
  {"substring"        , 9  ,  2 , 3  , create_func_substr},
  {"translate"        , 9  ,  3 , 3  , 0},

  {"local-name"       , 10 ,  0 , 1  , 0},
  {"starts-with"      , 11 ,  2 , 2  , 0},
  {"namespace-uri"    , 13 ,  0 , 1  , 0},
  {"string-length"    , 13 ,  0 , 1  , create_func_string_length},
  {"substring-after"  , 15 ,  2 , 2  , 0},
  {"normalize-space"  , 15 ,  0 , 1  , 0},
  {"substring-before" , 16 ,  2 , 2  , 0},

  {NULL,0,0,0,0}
};


/*
  Lookup a function by name

  SYNOPSYS
    Lookup a function by its name.

  RETURN
    Pointer to a MY_XPATH_FUNC variable on success.
    0 - on failure.

*/
MY_XPATH_FUNC *
my_xpath_function(const char *beg, const char *end)
{
  MY_XPATH_FUNC *k, *function_names;
  uint length= (uint)(end-beg);
  switch (length)
  {
    case 1: return 0;
    case 3: function_names= my_func_names3; break;
    case 4: function_names= my_func_names4; break;
    case 5: function_names= my_func_names5; break;
    case 6: function_names= my_func_names6; break;
    default: function_names= my_func_names;
  }
  for (k= function_names; k->name; k++)
    if (k->create && length == k->length && !strncasecmp(beg, k->name, length))
      return k;
  return NULL;
}


/* Initialize a lex analyzer token */
static void
my_xpath_lex_init(MY_XPATH_LEX *lex,
                  const char *str, const char *strend)
{
  lex->beg= str;
  lex->end= strend;
}


/* Initialize an XPath query parser */
static void
my_xpath_init(MY_XPATH *xpath)
{
  bzero((void*)xpath, sizeof(xpath[0]));
}


static int
my_xdigit(int c)
{
  return ((c) >= '0' && (c) <= '9');
}


/*
  Scan the next token

  SYNOPSYS
    Scan the next token from the input.
    lex->term is set to the scanned token type.
    lex->beg and lex->end are set to the beginning
    and to the end of the token.
  RETURN
    N/A
*/
static void
my_xpath_lex_scan(MY_XPATH *xpath,
                  MY_XPATH_LEX *lex, const char *beg, const char *end)
{
  int ch, ctype, length;
  for ( ; beg < end && *beg == ' ' ; beg++) ; // skip leading spaces
  lex->beg= beg;
  
  if (beg >= end)
  {
    lex->end= beg;
    lex->term= MY_XPATH_LEX_EOF; // end of line reached
    return;
  }

  // Check ident, or a function call, or a keyword
  if ((length= xpath->cs->ctype(&ctype,
                                (const uchar*) beg,
                                (const uchar*) end)) > 0 &&
      ((ctype & (_MY_L | _MY_U)) || *beg == '_'))
  {
    // scan until the end of the identifier
    for (beg+= length; 
         (length= xpath->cs->ctype(&ctype,
                                   (const uchar*) beg,
                                   (const uchar*) end)) > 0 &&
         ((ctype & (_MY_L | _MY_U | _MY_NMR)) ||
          *beg == '_' || *beg == '-' || *beg == '.') ;
         beg+= length) /* no op */;
    lex->end= beg;

    if (beg < end)
    {
      if (*beg == '(')
      {
        /*
         check if a function call, e.g.: count(/a/b)
         or a nodetype test,       e.g.: /a/b/text()
        */
        if ((xpath->func= my_xpath_function(lex->beg, beg)))
          lex->term= MY_XPATH_LEX_FUNC;
        else
          lex->term= my_xpath_keyword(xpath, my_nodetype_names,
                                      lex->beg, beg);
        return;
      }
      // check if an axis specifier, e.g.: /a/b/child::*
      else if (*beg == ':' && beg + 1 < end && beg[1] == ':')
      {
        lex->term= my_xpath_keyword(xpath, my_axis_names,
                                    lex->beg, beg);
        return;
      }
    }
    // check if a keyword
    lex->term= my_xpath_keyword(xpath, my_keyword_names,
                                lex->beg, beg);
    return;
  }


  ch= *beg++;
  
  if (ch > 0 && ch < 128 && simpletok[ch])
  {
    // a token consisting of one character found
    lex->end= beg;
    lex->term= ch;
    return;
  }


  if (my_xdigit(ch)) // a sequence of digits
  {
    for ( ; beg < end && my_xdigit(*beg) ; beg++) ;
    lex->end= beg;
    lex->term= MY_XPATH_LEX_DIGITS;
    return;
  }

  if (ch == '"' || ch == '\'')  // a string: either '...' or "..."
  {
    for ( ; beg < end && *beg != ch ; beg++) ;
    if (beg < end)
    {
      lex->end= beg+1;
      lex->term= MY_XPATH_LEX_STRING;
      return;
    } 
    else
    {
      // unexpected end-of-line, without closing quot sign
      lex->end= end;
      lex->term= MY_XPATH_LEX_ERROR;
      return;
    }
  }

  lex->end= beg;
  lex->term= MY_XPATH_LEX_ERROR; // unknown character
  return;
}


/*
  Scan the given token

  SYNOPSYS
    Scan the given token and rotate lasttok to prevtok on success.

  RETURN
    1 - success
    0 - failure
*/
static int
my_xpath_parse_term(MY_XPATH *xpath, int term)
{
  if (xpath->lasttok.term == term && !xpath->error)
  {
    xpath->prevtok= xpath->lasttok;
    my_xpath_lex_scan(xpath, &xpath->lasttok,
                      xpath->lasttok.end, xpath->query.end);
    return 1;
  }
  return 0;
}


/*
  Scan AxisName

  SYNOPSYS
    Scan an axis name and store the scanned axis type into xpath->axis.

  RETURN
    1 - success
    0 - failure
*/
static int my_xpath_parse_AxisName(MY_XPATH *xpath)
{
  int rc= my_xpath_parse_term(xpath, MY_XPATH_LEX_AXIS);
  xpath->axis= xpath->extra;
  return rc;
}


/*********************************************
** Grammar rules, according to http://www.w3.org/TR/xpath
** Implemented using recursive descendant method.
** All the following grammar processing functions accept
** a single "xpath" argument and return 1 on success and 0 on error.
** They also modify "xpath" argument by creating new items.
*/

/* [9]  PredicateExpr ::= Expr */
#define my_xpath_parse_PredicateExpr(x) my_xpath_parse_Expr((x))

/* [14] Expr ::= OrExpr */
#define my_xpath_parse_Expr(x) my_xpath_parse_OrExpr((x))

static int my_xpath_parse_LocationPath(MY_XPATH *xpath);
static int my_xpath_parse_AbsoluteLocationPath(MY_XPATH *xpath);
static int my_xpath_parse_RelativeLocationPath(MY_XPATH *xpath);
static int my_xpath_parse_AbbreviatedStep(MY_XPATH *xpath);
static int my_xpath_parse_Step(MY_XPATH *xpath);
static int my_xpath_parse_AxisSpecifier(MY_XPATH *xpath);
static int my_xpath_parse_NodeTest(MY_XPATH *xpath);
static int my_xpath_parse_AbbreviatedAxisSpecifier(MY_XPATH *xpath);
static int my_xpath_parse_NameTest(MY_XPATH *xpath);
static int my_xpath_parse_FunctionCall(MY_XPATH *xpath);
static int my_xpath_parse_Number(MY_XPATH *xpath);
static int my_xpath_parse_FilterExpr(MY_XPATH *xpath);
static int my_xpath_parse_PathExpr(MY_XPATH *xpath);
static int my_xpath_parse_OrExpr(MY_XPATH *xpath);
static int my_xpath_parse_UnaryExpr(MY_XPATH *xpath);
static int my_xpath_parse_MultiplicativeExpr(MY_XPATH *xpath);
static int my_xpath_parse_AdditiveExpr(MY_XPATH *xpath);
static int my_xpath_parse_RelationalExpr(MY_XPATH *xpath);
static int my_xpath_parse_AndExpr(MY_XPATH *xpath);
static int my_xpath_parse_EqualityExpr(MY_XPATH *xpath);
static int my_xpath_parse_VariableReference(MY_XPATH *xpath);


/*
  Scan LocationPath

  SYNOPSYS

    [1] LocationPath ::=   RelativeLocationPath
                         | AbsoluteLocationPath

  RETURN
    1 - success
    0 - failure
*/
static int my_xpath_parse_LocationPath(MY_XPATH *xpath)
{
  Item *context= xpath->context;

  if (!xpath->context)
    xpath->context= xpath->rootelement;
  int rc= my_xpath_parse_RelativeLocationPath(xpath) || 
          my_xpath_parse_AbsoluteLocationPath(xpath);

  xpath->item= xpath->context;
  xpath->context= context;
  return rc;
}


/*
  Scan Absolute Location Path

  SYNOPSYS

    [2]     AbsoluteLocationPath ::=   '/' RelativeLocationPath?	
                                     | AbbreviatedAbsoluteLocationPath
    [10]    AbbreviatedAbsoluteLocationPath ::=  '//' RelativeLocationPath

    We combine these two rules into one rule for better performance:

    [2,10]  AbsoluteLocationPath ::=  '/'   RelativeLocationPath?
                                     | '//' RelativeLocationPath

  RETURN
    1 - success
    0 - failure
*/
static int my_xpath_parse_AbsoluteLocationPath(MY_XPATH *xpath)
{
  if (!my_xpath_parse_term(xpath, MY_XPATH_LEX_SLASH))
    return 0;

  xpath->context= xpath->rootelement;

  if (my_xpath_parse_term(xpath, MY_XPATH_LEX_SLASH))
  {
    xpath->context= new (xpath->thd->mem_root)
      Item_nodeset_func_descendantbyname(xpath->thd,
                                         xpath->context,
                                         "*", 1,
                                         xpath->pxml, 1);
    return my_xpath_parse_RelativeLocationPath(xpath);
  }

  my_xpath_parse_RelativeLocationPath(xpath);
 
  return (xpath->error == 0);
}


/*
  Scan Relative Location Path

  SYNOPSYS

    For better performance we combine these two rules 

    [3] RelativeLocationPath ::=   Step
                                 | RelativeLocationPath '/' Step
                                 | AbbreviatedRelativeLocationPath
    [11] AbbreviatedRelativeLocationPath ::=  RelativeLocationPath '//' Step


    Into this one:

    [3-11] RelativeLocationPath ::=   Step
                                    | RelativeLocationPath '/'  Step
                                    | RelativeLocationPath '//' Step
  RETURN
    1 - success
    0 - failure
*/
static int my_xpath_parse_RelativeLocationPath(MY_XPATH *xpath)
{
  if (!my_xpath_parse_Step(xpath))
    return 0;
  while (my_xpath_parse_term(xpath, MY_XPATH_LEX_SLASH))
  {
    if (my_xpath_parse_term(xpath, MY_XPATH_LEX_SLASH))
      xpath->context= new (xpath->thd->mem_root)
        Item_nodeset_func_descendantbyname(xpath->thd,
                                           xpath->context,
                                           "*", 1,
                                           xpath->pxml, 1);
    if (!my_xpath_parse_Step(xpath))
    {
      xpath->error= 1;
      return 0;
    }
  }
  return 1;
}


/*
  Scan non-abbreviated or abbreviated Step

  SYNOPSYS

  [4] Step ::=   AxisSpecifier NodeTest Predicate*
               | AbbreviatedStep
  [8] Predicate ::= '[' PredicateExpr ']'

  RETURN
    1 - success
    0 - failure
*/
static int 
my_xpath_parse_AxisSpecifier_NodeTest_opt_Predicate_list(MY_XPATH *xpath)
{
  if (!my_xpath_parse_AxisSpecifier(xpath))
    return 0;
 
  if (!my_xpath_parse_NodeTest(xpath))
    return 0;

  while (my_xpath_parse_term(xpath, MY_XPATH_LEX_LB))
  {
    Item *prev_context= xpath->context;
    Native *context_cache;
    context_cache= &((Item_nodeset_func*)xpath->context)->context_cache;
    xpath->context= new (xpath->thd->mem_root)
      Item_nodeset_context_cache(xpath->thd, context_cache, xpath->pxml);
    xpath->context_cache= context_cache;

    if(!my_xpath_parse_PredicateExpr(xpath))
    {
      xpath->error= 1;
      return 0;
    }

    if (!my_xpath_parse_term(xpath, MY_XPATH_LEX_RB))
    {
      xpath->error= 1;
      return 0;
    }

    xpath->item= nodeset2bool(xpath, xpath->item);

    const Type_handler *fh;
    if ((fh= xpath->item->fixed_type_handler()) && fh->is_bool_type())
    {
      xpath->context= new (xpath->thd->mem_root)
        Item_nodeset_func_predicate(xpath->thd, prev_context,
                                    xpath->item,
                                    xpath->pxml);
    }
    else
    {
      xpath->context= new (xpath->thd->mem_root)
        Item_nodeset_func_elementbyindex(xpath->thd,
                                         prev_context,
                                         xpath->item,
                                         xpath->pxml);
    }
  }
  return 1;
}


static int my_xpath_parse_Step(MY_XPATH *xpath)
{
  return
    my_xpath_parse_AxisSpecifier_NodeTest_opt_Predicate_list(xpath) ||
    my_xpath_parse_AbbreviatedStep(xpath);
}


/*
  Scan Abbreviated Axis Specifier

  SYNOPSYS
  [5] AxisSpecifier ::=  AxisName '::' 
                         | AbbreviatedAxisSpecifier

  RETURN
    1 - success
    0 - failure
*/
static int my_xpath_parse_AbbreviatedAxisSpecifier(MY_XPATH *xpath)
{
  if (my_xpath_parse_term(xpath, MY_XPATH_LEX_AT))
    xpath->axis= MY_XPATH_AXIS_ATTRIBUTE;
  else
    xpath->axis= MY_XPATH_AXIS_CHILD;
  return 1;
}


/*
  Scan non-abbreviated axis specifier

  SYNOPSYS

  RETURN
    1 - success
    0 - failure
*/
static int my_xpath_parse_AxisName_colon_colon(MY_XPATH *xpath)
{
  return my_xpath_parse_AxisName(xpath) &&
         my_xpath_parse_term(xpath, MY_XPATH_LEX_COLON) &&
         my_xpath_parse_term(xpath, MY_XPATH_LEX_COLON);
}


/*
  Scan Abbreviated AxisSpecifier

  SYNOPSYS
    [13] AbbreviatedAxisSpecifier  ::=  '@'?

  RETURN
    1 - success
    0 - failure
*/
static int my_xpath_parse_AxisSpecifier(MY_XPATH *xpath)
{
  return my_xpath_parse_AxisName_colon_colon(xpath) ||
         my_xpath_parse_AbbreviatedAxisSpecifier(xpath);
}


/*
  Scan NodeType followed by parens

  SYNOPSYS

  RETURN
    1 - success
    0 - failure
*/
static int my_xpath_parse_NodeTest_lp_rp(MY_XPATH *xpath)
{
  return my_xpath_parse_term(xpath, MY_XPATH_LEX_NODETYPE) &&
         my_xpath_parse_term(xpath, MY_XPATH_LEX_LP) &&
         my_xpath_parse_term(xpath, MY_XPATH_LEX_RP);
}


/*
  Scan NodeTest

  SYNOPSYS

  [7] NodeTest ::=   NameTest
                   | NodeType '(' ')'
                   | 'processing-instruction' '(' Literal ')'
  RETURN
    1 - success
    0 - failure
*/
static int my_xpath_parse_NodeTest(MY_XPATH *xpath)
{
  return my_xpath_parse_NameTest(xpath) ||
         my_xpath_parse_NodeTest_lp_rp(xpath);
}


/*
  Scan Abbreviated Step

  SYNOPSYS

  [12] AbbreviatedStep  ::= '.'	| '..'

  RETURN
    1 - success
    0 - failure
*/
static int my_xpath_parse_AbbreviatedStep(MY_XPATH *xpath)
{
  if (!my_xpath_parse_term(xpath, MY_XPATH_LEX_DOT))
    return 0;
  if (my_xpath_parse_term(xpath, MY_XPATH_LEX_DOT))
    xpath->context= new (xpath->thd->mem_root)
      Item_nodeset_func_parentbyname(xpath->thd,
                                     xpath->context, "*",
                                     1, xpath->pxml);
  return 1;
}


/*
  Scan Primary Expression

  SYNOPSYS

  [15] PrimaryExpr ::= VariableReference	
                       | '(' Expr ')'	
                       | Literal	
                       | Number	
                       | FunctionCall
  RETURN
    1 - success
    0 - failure
*/
static int my_xpath_parse_lp_Expr_rp(MY_XPATH *xpath)
{
  return my_xpath_parse_term(xpath, MY_XPATH_LEX_LP) &&
         my_xpath_parse_Expr(xpath) &&
         my_xpath_parse_term(xpath, MY_XPATH_LEX_RP);
}
static int my_xpath_parse_PrimaryExpr_literal(MY_XPATH *xpath)
{
  if (!my_xpath_parse_term(xpath, MY_XPATH_LEX_STRING))
    return 0;
  xpath->item= new (xpath->thd->mem_root)
    Item_string(xpath->thd, xpath->prevtok.beg + 1,
                (uint)(xpath->prevtok.end - xpath->prevtok.beg - 2),
                xpath->cs);
  return 1;
}
static int my_xpath_parse_PrimaryExpr(MY_XPATH *xpath)
{
  return 
      my_xpath_parse_lp_Expr_rp(xpath)          ||
      my_xpath_parse_VariableReference(xpath)   ||
      my_xpath_parse_PrimaryExpr_literal(xpath) ||
      my_xpath_parse_Number(xpath)              ||
      my_xpath_parse_FunctionCall(xpath);
}


/*
  Scan Function Call

  SYNOPSYS
    [16] FunctionCall ::= FunctionName '(' ( Argument ( ',' Argument )* )? ')'
    [17] Argument      ::= Expr

  RETURN
    1 - success
    0 - failure

*/
static int my_xpath_parse_FunctionCall(MY_XPATH *xpath)
{
  Item *args[256];
  uint nargs;

  if (!my_xpath_parse_term(xpath, MY_XPATH_LEX_FUNC))
    return 0;

  MY_XPATH_FUNC *func= xpath->func;

  if (!my_xpath_parse_term(xpath, MY_XPATH_LEX_LP))
    return 0;

  for (nargs= 0 ; nargs < func->maxargs; )
  {
    if (!my_xpath_parse_Expr(xpath))
    {
      if (nargs < func->minargs)
        return 0;
      goto right_paren;
    }
    args[nargs++]= xpath->item;
    if (!my_xpath_parse_term(xpath, MY_XPATH_LEX_COMMA))
    {
      if (nargs < func->minargs)
        return 0;
      else
        break;
    }
  }

right_paren:
  if (!my_xpath_parse_term(xpath, MY_XPATH_LEX_RP))
    return 0;

  return ((xpath->item= func->create(xpath, args, nargs))) ? 1 : 0;
}


/*
  Scan Union Expression

  SYNOPSYS
    [18] UnionExpr ::=   PathExpr	
                       | UnionExpr '|' PathExpr

  RETURN
    1 - success
    0 - failure
*/
static int my_xpath_parse_UnionExpr(MY_XPATH *xpath)
{
  if (!my_xpath_parse_PathExpr(xpath))
    return 0;
 
  while (my_xpath_parse_term(xpath, MY_XPATH_LEX_VLINE))
  {
    Item *prev= xpath->item;
    if (prev->fixed_type_handler() != &type_handler_xpath_nodeset)
      return 0;
    
    if (!my_xpath_parse_PathExpr(xpath)
        || xpath->item->fixed_type_handler() != &type_handler_xpath_nodeset)
    {
      xpath->error= 1;
      return 0;
    }
    xpath->item= new (xpath->thd->mem_root)
      Item_nodeset_func_union(xpath->thd, prev, xpath->item,
                              xpath->pxml);
  }
  return 1;
}


/*
  Scan Path Expression

  SYNOPSYS

  [19] PathExpr ::=   LocationPath
                    | FilterExpr
                    | FilterExpr '/' RelativeLocationPath
                    | FilterExpr '//' RelativeLocationPath
  RETURN
    1 - success
    0 - failure
*/
static int
my_xpath_parse_FilterExpr_opt_slashes_RelativeLocationPath(MY_XPATH *xpath)
{
  Item *context= xpath->context;
  int rc;

  if (!my_xpath_parse_FilterExpr(xpath))
    return 0;

  if (!my_xpath_parse_term(xpath, MY_XPATH_LEX_SLASH))
    return 1;

  if (xpath->item->fixed_type_handler() != &type_handler_xpath_nodeset)
  {
    xpath->lasttok= xpath->prevtok;
    xpath->error= 1;
    return 0;
  }

  /*
    The context for the next relative path is the nodeset
    returned by FilterExpr
  */
  xpath->context= xpath->item;

  /* treat double slash (//) as /descendant-or-self::node()/ */
  if (my_xpath_parse_term(xpath, MY_XPATH_LEX_SLASH))
    xpath->context= new (xpath->thd->mem_root)
      Item_nodeset_func_descendantbyname(xpath->thd,
                                         xpath->context,
                                         "*", 1,
                                         xpath->pxml, 1);
  rc= my_xpath_parse_RelativeLocationPath(xpath);

  /* push back the context and restore the item */
  xpath->item= xpath->context;
  xpath->context= context;
  return rc;
}
static int my_xpath_parse_PathExpr(MY_XPATH *xpath)
{
  return my_xpath_parse_LocationPath(xpath) || 
         my_xpath_parse_FilterExpr_opt_slashes_RelativeLocationPath(xpath);
}



/*
  Scan Filter Expression

  SYNOPSYS
    [20]  FilterExpr ::=   PrimaryExpr	
                         | FilterExpr Predicate

    or in other words:

    [20]  FilterExpr ::=   PrimaryExpr Predicate*

  RETURN
    1 - success
    0 - failure

*/
static int my_xpath_parse_FilterExpr(MY_XPATH *xpath)
{
  return my_xpath_parse_PrimaryExpr(xpath);
}


/*
  Scan Or Expression

  SYNOPSYS
    [21] OrExpr ::=   AndExpr
                    | OrExpr 'or' AndExpr

  RETURN
    1 - success
    0 - failure
*/
static int my_xpath_parse_OrExpr(MY_XPATH *xpath)
{
  if (!my_xpath_parse_AndExpr(xpath))
    return 0;

  while (my_xpath_parse_term(xpath, MY_XPATH_LEX_OR))
  {
    Item *prev= xpath->item;
    if (!my_xpath_parse_AndExpr(xpath))
    {
      xpath->error= 1;
      return 0;
    }
    xpath->item= new (xpath->thd->mem_root)
      Item_cond_or(xpath->thd, nodeset2bool(xpath, prev),
                   nodeset2bool(xpath, xpath->item));
  }
  return 1;
}


/*
  Scan And Expression

  SYNOPSYS
    [22] AndExpr ::=   EqualityExpr	
                     | AndExpr 'and' EqualityExpr

  RETURN
    1 - success
    0 - failure
*/
static int my_xpath_parse_AndExpr(MY_XPATH *xpath)
{
  if (!my_xpath_parse_EqualityExpr(xpath))
    return 0;

  while (my_xpath_parse_term(xpath, MY_XPATH_LEX_AND))
  {
    Item *prev= xpath->item;
    if (!my_xpath_parse_EqualityExpr(xpath))
    {
      xpath->error= 1;
      return 0;
    }

    xpath->item= new (xpath->thd->mem_root)
      Item_cond_and(xpath->thd, nodeset2bool(xpath, prev),
                    nodeset2bool(xpath, xpath->item));
  }
  return 1;
}


/*
  Scan Equality Expression

  SYNOPSYS
    [23] EqualityExpr ::=   RelationalExpr
                          | EqualityExpr '=' RelationalExpr
                          | EqualityExpr '!=' RelationalExpr
    or in other words:

    [23] EqualityExpr ::= RelationalExpr ( EqualityOperator EqualityExpr )*

  RETURN
    1 - success
    0 - failure
*/
static int my_xpath_parse_ne(MY_XPATH *xpath)
{ 
  MY_XPATH_LEX prevtok= xpath->prevtok;
  if (!my_xpath_parse_term(xpath, MY_XPATH_LEX_EXCL))
    return 0;
  if (!my_xpath_parse_term(xpath, MY_XPATH_LEX_EQ))
  {
    /* Unget the exclamation mark */
    xpath->lasttok= xpath->prevtok;
    xpath->prevtok= prevtok;
    return 0;
  }
  return 1;
}
static int my_xpath_parse_EqualityOperator(MY_XPATH *xpath)
{
  if (my_xpath_parse_ne(xpath))
  {
    xpath->extra= '!';
    return 1;
  }
  if (my_xpath_parse_term(xpath, MY_XPATH_LEX_EQ))
  {
    xpath->extra= '=';
    return 1;
  }
  return 0;
}
static int my_xpath_parse_EqualityExpr(MY_XPATH *xpath)
{
  MY_XPATH_LEX operator_context;
  if (!my_xpath_parse_RelationalExpr(xpath))
    return 0;

  operator_context= xpath->lasttok;
  while (my_xpath_parse_EqualityOperator(xpath))
  {
    Item *prev= xpath->item;
    int oper= xpath->extra;
    if (!my_xpath_parse_RelationalExpr(xpath))
    {
      xpath->error= 1;
      return 0;
    }

    if (!(xpath->item= create_comparator(xpath, oper, &operator_context,
                                         prev, xpath->item)))
      return 0;

    operator_context= xpath->lasttok;
  }
  return 1;
}


/*
  Scan Relational Expression

  SYNOPSYS

    [24] RelationalExpr ::=   AdditiveExpr
                            | RelationalExpr '<' AdditiveExpr
                            | RelationalExpr '>' AdditiveExpr
                            | RelationalExpr '<=' AdditiveExpr
                            | RelationalExpr '>=' AdditiveExpr
  or in other words:

    [24] RelationalExpr ::= AdditiveExpr (RelationalOperator RelationalExpr)*

  RETURN
    1 - success
    0 - failure
*/
static int my_xpath_parse_RelationalOperator(MY_XPATH *xpath)
{
  if (my_xpath_parse_term(xpath, MY_XPATH_LEX_LESS))
  {
    xpath->extra= my_xpath_parse_term(xpath, MY_XPATH_LEX_EQ) ? 
                  MY_XPATH_LEX_LE : MY_XPATH_LEX_LESS;
    return 1;
  }
  else if (my_xpath_parse_term(xpath, MY_XPATH_LEX_GREATER))
  {
    xpath->extra= my_xpath_parse_term(xpath, MY_XPATH_LEX_EQ) ?
                  MY_XPATH_LEX_GE : MY_XPATH_LEX_GREATER;
    return 1;
  }
  return 0;
}
static int my_xpath_parse_RelationalExpr(MY_XPATH *xpath)
{
  MY_XPATH_LEX operator_context;
  if (!my_xpath_parse_AdditiveExpr(xpath))
    return 0;
  operator_context= xpath->lasttok;
  while (my_xpath_parse_RelationalOperator(xpath))
  {
    Item *prev= xpath->item;
    int oper= xpath->extra;

    if (!my_xpath_parse_AdditiveExpr(xpath))
    {
      xpath->error= 1;
      return 0;
    }

    if (!(xpath->item= create_comparator(xpath, oper, &operator_context,
                                         prev, xpath->item)))
      return 0;
    operator_context= xpath->lasttok;
  }
  return 1;
}


/*
  Scan Additive Expression

  SYNOPSYS

    [25] AdditiveExpr ::=   MultiplicativeExpr	
                          | AdditiveExpr '+' MultiplicativeExpr	
                          | AdditiveExpr '-' MultiplicativeExpr
  RETURN
    1 - success
    0 - failure
*/
static int my_xpath_parse_AdditiveOperator(MY_XPATH *xpath)
{
 return my_xpath_parse_term(xpath, MY_XPATH_LEX_PLUS) ||
        my_xpath_parse_term(xpath, MY_XPATH_LEX_MINUS);
}
static int my_xpath_parse_AdditiveExpr(MY_XPATH *xpath)
{
  if (!my_xpath_parse_MultiplicativeExpr(xpath))
    return 0;

  while (my_xpath_parse_AdditiveOperator(xpath))
  {
    int oper= xpath->prevtok.term;
    Item *prev= xpath->item;
    THD *thd= xpath->thd;

    if (!my_xpath_parse_MultiplicativeExpr(xpath))
    {
      xpath->error= 1;
      return 0;
    }

    if (oper == MY_XPATH_LEX_PLUS)
      xpath->item= new (thd->mem_root)
        Item_func_plus(thd, prev, xpath->item);
    else
      xpath->item= new (thd->mem_root)
        Item_func_minus(thd, prev, xpath->item);
  };
  return 1;
}


/*
  Scan Multiplicative Expression

  SYNOPSYS

    [26] MultiplicativeExpr ::=   UnaryExpr	
                                | MultiplicativeExpr MultiplyOperator UnaryExpr	
                                | MultiplicativeExpr 'div' UnaryExpr	
                                | MultiplicativeExpr 'mod' UnaryExpr
    or in other words:  
  
    [26]  MultiplicativeExpr ::= UnaryExpr (MulOper MultiplicativeExpr)*
  
  RETURN
    1 - success
    0 - failure
*/
static int my_xpath_parse_MultiplicativeOperator(MY_XPATH *xpath)
{
  return
      my_xpath_parse_term(xpath, MY_XPATH_LEX_ASTERISK) ||
      my_xpath_parse_term(xpath, MY_XPATH_LEX_DIV)      ||
      my_xpath_parse_term(xpath, MY_XPATH_LEX_MOD);
}
static int my_xpath_parse_MultiplicativeExpr(MY_XPATH *xpath)
{
  if (!my_xpath_parse_UnaryExpr(xpath))
    return 0;

  THD *thd= xpath->thd;
  while (my_xpath_parse_MultiplicativeOperator(xpath))
  {
    int oper= xpath->prevtok.term;
    Item *prev= xpath->item;
    if (!my_xpath_parse_UnaryExpr(xpath))
    {
      xpath->error= 1;
      return 0;
    }
    switch (oper)
    {
      case MY_XPATH_LEX_ASTERISK:
        xpath->item= new (thd->mem_root) Item_func_mul(thd, prev, xpath->item);
        break;
      case MY_XPATH_LEX_DIV:
        xpath->item= new (thd->mem_root) Item_func_int_div(thd, prev, xpath->item);
        break;
      case MY_XPATH_LEX_MOD:
        xpath->item= new (thd->mem_root) Item_func_mod(thd, prev, xpath->item);
        break;
    }
  }
  return 1;
}


/*
  Scan Unary Expression

  SYNOPSYS

    [27] UnaryExpr ::=   UnionExpr	
                       | '-' UnaryExpr
  RETURN
    1 - success
    0 - failure
*/
static int my_xpath_parse_UnaryExpr(MY_XPATH *xpath)
{
  if (!my_xpath_parse_term(xpath, MY_XPATH_LEX_MINUS))
    return my_xpath_parse_UnionExpr(xpath);
  if (!my_xpath_parse_UnaryExpr(xpath))
    return 0;
  xpath->item= new (xpath->thd->mem_root)
    Item_func_neg(xpath->thd, xpath->item);
  return 1;
}


/**
  A helper class to make a null-terminated string from XPath fragments.
  The string is allocated on the THD memory root.
*/
class XPath_cstring_null_terminated: public LEX_CSTRING
{
public:
  XPath_cstring_null_terminated(THD *thd, const char *str, size_t length)
  {
    if (thd->make_lex_string(this, str, length))
      static_cast<LEX_CSTRING>(*this)= empty_clex_str;
  }
};


/*
  Scan Number

  SYNOPSYS

    [30] Number ::= Digits ('.' Digits?)? | '.' Digits)

  or in other words:

    [30] Number ::= Digits
                    | Digits '.'
                    | Digits '.' Digits
                    | '.' Digits

  Note: the last rule is not supported yet,
  as it is in conflict with abbreviated step.
  1 + .123    does not work,
  1 + 0.123   does.
  Perhaps it is better to move this code into lex analyzer.

  RETURN
    1 - success
    0 - failure
*/
static int my_xpath_parse_Number(MY_XPATH *xpath)
{
  const char *beg;
  THD *thd;
  if (!my_xpath_parse_term(xpath, MY_XPATH_LEX_DIGITS))
    return 0;
  beg= xpath->prevtok.beg;
  thd= xpath->thd;
  if (!my_xpath_parse_term(xpath, MY_XPATH_LEX_DOT))
  {
    XPath_cstring_null_terminated nr(thd, beg, xpath->prevtok.end - beg);
    xpath->item= new (thd->mem_root) Item_int(thd, nr.str, (uint) nr.length);
  }
  else
  {
    my_xpath_parse_term(xpath, MY_XPATH_LEX_DIGITS);
    XPath_cstring_null_terminated nr(thd, beg, xpath->prevtok.end - beg);
    xpath->item= new (thd->mem_root) Item_float(thd, nr.str, (uint) nr.length);
  }
  return 1;
}


/*
  Scan NCName.
  
  SYNOPSYS
    
    The keywords AND, OR, MOD, DIV are valid identifiers
    when they are in identifier context:
    
    SELECT
    ExtractValue('<and><or><mod><div>VALUE</div></mod></or></and>',
                 '/and/or/mod/div')
    ->  VALUE
    
  RETURN
    1 - success
    0 - failure
*/

static int
my_xpath_parse_NCName(MY_XPATH *xpath)
{
  return
    my_xpath_parse_term(xpath, MY_XPATH_LEX_IDENT) ||
    my_xpath_parse_term(xpath, MY_XPATH_LEX_AND)   ||
    my_xpath_parse_term(xpath, MY_XPATH_LEX_OR)    ||
    my_xpath_parse_term(xpath, MY_XPATH_LEX_MOD)   ||
    my_xpath_parse_term(xpath, MY_XPATH_LEX_DIV) ? 1 : 0;
}


/*
  QName grammar can be found in a separate document
  http://www.w3.org/TR/REC-xml-names/#NT-QName

  [6] 	QName     ::= (Prefix ':')? LocalPart
  [7] 	Prefix    ::= NCName
  [8] 	LocalPart ::= NCName
*/

static int
my_xpath_parse_QName(MY_XPATH *xpath)
{
  const char *beg;
  if (!my_xpath_parse_NCName(xpath))
    return 0;
  beg= xpath->prevtok.beg;
  if (!my_xpath_parse_term(xpath, MY_XPATH_LEX_COLON))
    return 1; /* Non qualified name */
  if (!my_xpath_parse_NCName(xpath))
    return 0;
  xpath->prevtok.beg= beg;
  return 1;
}


/**
  Scan Variable reference

  @details Implements parsing of two syntax structures:

    1. Standard XPath syntax [36], for SP variables:

      VariableReference ::= '$' QName     

      Finds a SP variable with the given name.
      If outside of a SP context, or variable with
      the given name doesn't exists, then error is returned.

    2. Non-standard syntax - MySQL extension for user variables:

      VariableReference ::= '$' '@' QName

    Item, corresponding to the variable, is returned
    in xpath->item in both cases.

  @param  xpath pointer to XPath structure

  @return Operation status
    @retval 1 Success
    @retval 0 Failure
*/

static int
my_xpath_parse_VariableReference(MY_XPATH *xpath)
{
  LEX_CSTRING name;
  THD *thd= xpath->thd;
  if (!my_xpath_parse_term(xpath, MY_XPATH_LEX_DOLLAR))
    return 0;
  const char *dollar_pos= xpath->prevtok.beg;
  if (!dollar_pos)
    return 0;
  int user_var= my_xpath_parse_term(xpath, MY_XPATH_LEX_AT);
  if (!((user_var &&
         my_xpath_parse_term(xpath, MY_XPATH_LEX_IDENT))) &&
      !my_xpath_parse_term(xpath, MY_XPATH_LEX_IDENT))
    return 0;

  name.length= xpath->prevtok.end - xpath->prevtok.beg;
  name.str= (char*) xpath->prevtok.beg;

  if (user_var)
    xpath->item= new (thd->mem_root) Item_func_get_user_var(thd, &name);
  else
  {
    sp_variable *spv;
    const Sp_rcontext_handler *rh;
    LEX *lex;
    /*
      We call lex->find_variable() rather than thd->lex->spcont->find_variable()
      to make sure package body variables are properly supported.
    */
    if ((lex= thd->lex) &&
        (spv= lex->find_variable(&name, &rh)))
    {
      Item_splocal *splocal= new (thd->mem_root)
        Item_splocal(thd, rh, &name, spv->offset, spv->type_handler(), 0);
#ifdef DBUG_ASSERT_EXISTS
      if (splocal)
        splocal->m_sp= lex->sphead;
#endif
      xpath->item= (Item*) splocal;
    }
    else
    {
      xpath->item= NULL;
      DBUG_ASSERT(xpath->query.end > dollar_pos);
      uint len= (uint)(xpath->query.end - dollar_pos);
      if (len <= 32)
        my_printf_error(ER_UNKNOWN_ERROR, "Unknown XPATH variable at: '%.*s'",
                        MYF(0), len, dollar_pos);
      else
        my_printf_error(ER_UNKNOWN_ERROR, "Unknown XPATH variable at: '%.32sT'",
                        MYF(0), dollar_pos);
    }
  }
  return xpath->item ? 1 : 0;
}


/*
  Scan Name Test

  SYNOPSYS

    [37] NameTest ::=  '*'
                      | NCName ':' '*'
                      | QName
  RETURN
    1 - success
    0 - failure
*/
static int
my_xpath_parse_NodeTest_QName(MY_XPATH *xpath)
{
  if (!my_xpath_parse_QName(xpath))
    return 0;
  DBUG_ASSERT(xpath->context);
  uint len= (uint)(xpath->prevtok.end - xpath->prevtok.beg);
  xpath->context= nametestfunc(xpath, xpath->axis, xpath->context,
                               xpath->prevtok.beg, len);
  return 1;
}
static int
my_xpath_parse_NodeTest_asterisk(MY_XPATH *xpath)
{
  if (!my_xpath_parse_term(xpath, MY_XPATH_LEX_ASTERISK))
    return 0;
  DBUG_ASSERT(xpath->context);
  xpath->context= nametestfunc(xpath, xpath->axis, xpath->context, "*", 1);
  return 1;
}
static int
my_xpath_parse_NameTest(MY_XPATH *xpath)
{
  return my_xpath_parse_NodeTest_asterisk(xpath) ||
         my_xpath_parse_NodeTest_QName(xpath);
}


/*
  Scan an XPath expression

  SYNOPSYS
    Scan xpath expression.
    The expression is returned in xpath->expr. 

  RETURN
    1 - success
    0 - failure
*/
static int
my_xpath_parse(MY_XPATH *xpath, const char *str, const char *strend)
{
  my_xpath_lex_init(&xpath->query, str, strend);
  my_xpath_lex_init(&xpath->prevtok, str, strend);
  my_xpath_lex_scan(xpath, &xpath->lasttok, str, strend);

  xpath->rootelement= new (xpath->thd->mem_root)
    Item_nodeset_func_rootelement(xpath->thd,
                                  xpath->pxml);

  return (my_xpath_parse_Expr(xpath) &&
          my_xpath_parse_term(xpath, MY_XPATH_LEX_EOF));
}


bool Item_xml_str_func::fix_length_and_dec(THD *thd)
{
  max_length= MAX_BLOB_WIDTH;
  return agg_arg_charsets_for_comparison(collation, args, arg_count);
}


bool Item_xml_str_func::fix_fields(THD *thd, Item **ref)
{
  String *xp;
  MY_XPATH xpath;
  int rc;

  if (Item_str_func::fix_fields(thd, ref))
    return true;
  
  status_var_increment(current_thd->status_var.feature_xml);

  nodeset_func= 0;


  if (collation.collation->mbminlen > 1)
  {
    /* UCS2 is not supported */
    my_printf_error(ER_UNKNOWN_ERROR,
                    "Character set '%s' is not supported by XPATH",
                    MYF(0), collation.collation->cs_name.str);
    return true;
  }

  if (!args[1]->const_item())
  {
    my_printf_error(ER_UNKNOWN_ERROR,
                    "Only constant XPATH queries are supported", MYF(0));
    return true;
  }

  /*
    Get the XPath query text from args[1] and cache it in m_xpath_query.
    Its fragments will be referenced by items created during my_xpath_parse(),
    e.g. by Item_nodeset_func_axisbyname::node_name.
  */
  if (!(xp= args[1]->val_str(&m_xpath_query)) ||
      (xp != &m_xpath_query && m_xpath_query.copy(*xp)))
    return false; // Will return NULL
  my_xpath_init(&xpath);
  xpath.thd= thd;
  xpath.cs= collation.collation;
  xpath.debug= 0;
  xpath.pxml= xml.parsed();
  xml.set_charset(collation.collation);

  rc= my_xpath_parse(&xpath, xp->ptr(), xp->ptr() + xp->length());

  if (!rc)
  {
    uint clen= (uint)(xpath.query.end - xpath.lasttok.beg);
    if (clen <= 32)
      my_printf_error(ER_UNKNOWN_ERROR, "XPATH syntax error: '%.*s'",
                      MYF(0), clen, xpath.lasttok.beg);
    else
      my_printf_error(ER_UNKNOWN_ERROR, "XPATH syntax error: '%.32sT'",
                      MYF(0), xpath.lasttok.beg);

    return true;
  }

  /*
     Parsing XML is a heavy operation, so if the first argument is constant,
     then parse XML only one time and cache the parsed representation
     together with raw text representation.

     Note, we cannot cache the entire function result even if
     the first and the second arguments are constants, because
     the XPath expression may have user and SP variable references,
     so the function result can vary between executions.
  */
  if ((args[0]->const_item() && get_xml(&xml, true)) ||
      !(nodeset_func= xpath.item))
    return false; // Will return NULL

  return nodeset_func->fix_fields(thd, &nodeset_func);
}


#define MAX_LEVEL 256
typedef struct 
{
  uint level;
  String *pxml;         // parsed XML
  uint pos[MAX_LEVEL];  // Tag position stack
  uint parent;          // Offset of the parent of the current node
} MY_XML_USER_DATA;


static bool
append_node(String *str, MY_XML_NODE *node)
{
  /*
   If "str" doesn't have space for a new node,
   it will allocate two times more space that it has had so far.
   (2*len+512) is a heuristic value,
   which gave the best performance during tests.
   The ideas behind this formula are:
   - It allows to have a very small number of reallocs:
     about 10 reallocs on a 1Mb-long XML value.
   - At the same time, it avoids excessive memory use.
  */
  if (str->reserve(sizeof(MY_XML_NODE), 2 * str->length() + 512))
    return TRUE;
  str->q_append((const char*) node, sizeof(MY_XML_NODE));
  return FALSE;
}


/*
  Process tag beginning

  SYNOPSYS

    A call-back function executed when XML parser
    is entering a tag or an attribute.
    Appends the new node into data->pxml.
    Increments data->level.

  RETURN
    Currently only MY_XML_OK
*/
extern "C" int xml_enter(MY_XML_PARSER *st,const char *attr, size_t len);

int xml_enter(MY_XML_PARSER *st,const char *attr, size_t len)
{
  MY_XML_USER_DATA *data= (MY_XML_USER_DATA*)st->user_data;
  uint numnodes= data->pxml->length() / sizeof(MY_XML_NODE);
  MY_XML_NODE node;

  node.parent= data->parent; // Set parent for the new node to old parent
  data->parent= numnodes;    // Remember current node as new parent
  DBUG_ASSERT(data->level < MAX_LEVEL);
  data->pos[data->level]= numnodes;
  if (data->level < MAX_LEVEL - 1)
    node.level= data->level++;
  else
    return MY_XML_ERROR;
  node.type= st->current_node_type; // TAG or ATTR
  node.beg= attr;
  node.end= attr + len;
  return append_node(data->pxml, &node) ? MY_XML_ERROR : MY_XML_OK;
}


/*
  Process text node

  SYNOPSYS

    A call-back function executed when XML parser
    is entering into a tag or an attribute textual value.
    The value is appended into data->pxml.

  RETURN
    Currently only MY_XML_OK
*/
extern "C" int xml_value(MY_XML_PARSER *st,const char *attr, size_t len);

int xml_value(MY_XML_PARSER *st,const char *attr, size_t len)
{
  MY_XML_USER_DATA *data= (MY_XML_USER_DATA*)st->user_data;
  MY_XML_NODE node;
  
  node.parent= data->parent; // Set parent for the new text node to old parent
  node.level= data->level;
  node.type= MY_XML_NODE_TEXT;
  node.beg= attr;
  node.end= attr + len;
  return append_node(data->pxml, &node) ? MY_XML_ERROR : MY_XML_OK;
}


/*
  Leave a tag or an attribute

  SYNOPSYS

    A call-back function executed when XML parser
    is leaving a tag or an attribute.
    Decrements data->level.

  RETURN
    Currently only MY_XML_OK
*/
extern "C" int xml_leave(MY_XML_PARSER *st,const char *attr, size_t len);

int xml_leave(MY_XML_PARSER *st,const char *attr, size_t len)
{
  MY_XML_USER_DATA *data= (MY_XML_USER_DATA*)st->user_data;
  DBUG_ASSERT(data->level > 0);
  data->level--;

  MY_XML_NODE *nodes= (MY_XML_NODE*) data->pxml->ptr();
  data->parent= nodes[data->parent].parent;
  nodes+= data->pos[data->level];
  nodes->tagend= st->cur;

  return MY_XML_OK;
}


/*
  Parse raw XML

  SYNOPSYS

  RETURN
    false on success
    true on error
*/
bool Item_xml_str_func::XML::parse()
{
  MY_XML_PARSER p;
  MY_XML_USER_DATA user_data;
  int rc;

  m_parsed_buf.length(0);

  /* Prepare XML parser */
  my_xml_parser_create(&p);
  p.flags= MY_XML_FLAG_RELATIVE_NAMES | MY_XML_FLAG_SKIP_TEXT_NORMALIZATION;
  user_data.level= 0;
  user_data.pxml= &m_parsed_buf;
  user_data.parent= 0;
  my_xml_set_enter_handler(&p, xml_enter);
  my_xml_set_value_handler(&p, xml_value);
  my_xml_set_leave_handler(&p, xml_leave);
  my_xml_set_user_data(&p, (void*) &user_data);

  /* Add root node */
  p.current_node_type= MY_XML_NODE_TAG;
  xml_enter(&p, m_raw_ptr->ptr(), 0);

  /* Execute XML parser */
  if ((rc= my_xml_parse(&p, m_raw_ptr->ptr(), m_raw_ptr->length())) != MY_XML_OK)
  {
    THD *thd= current_thd;
    char buf[128];
    my_snprintf(buf, sizeof(buf)-1, "parse error at line %d pos %lu: %s",
                my_xml_error_lineno(&p) + 1,
                (ulong) my_xml_error_pos(&p) + 1,
                my_xml_error_string(&p));
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_WRONG_VALUE,
                        ER_THD(thd, ER_WRONG_VALUE), "XML", buf);
    m_raw_ptr= (String *) 0;
  }
  my_xml_parser_free(&p);

  return rc != MY_XML_OK;
}


/*
  Parse the raw XML from the given source,
  optionally cache the raw XML,
  remember the pointer to the raw XML.
*/
bool Item_xml_str_func::XML::parse(String *raw_xml, bool cache)
{
  m_raw_ptr= raw_xml;
  if (cache)
  {
    m_cached= true;
    if (m_raw_ptr != &m_raw_buf && m_raw_buf.copy(*m_raw_ptr))
    {
      m_raw_ptr= (String *) 0;
      return true;
    }
    m_raw_ptr= &m_raw_buf;
  }
  return parse();
}


const MY_XML_NODE *Item_xml_str_func::XML::node(uint idx)
{
  const MY_XML_NODE *nodebeg= (MY_XML_NODE*) m_parsed_buf.ptr();
  DBUG_ASSERT(idx < m_parsed_buf.length() / sizeof (MY_XML_NODE));
  return nodebeg + idx;
}


String *Item_func_xml_extractvalue::val_str(String *str)
{
  String *res;
  null_value= 0;
  if (!nodeset_func || get_xml(&xml) ||
      !(res= nodeset_func->val_str(str)))
  {
    null_value= 1;
    return 0;
  }
  return res;  
}


bool Item_func_xml_update::collect_result(String *str,
                                          const MY_XML_NODE *cut,
                                          const String *replace)
{
  uint offs= cut->type == MY_XML_NODE_TAG ? 1 : 0;
  const char *end= cut->tagend + offs;
  str->length(0);
  str->set_charset(collation.collation);
  return
    /* Put the XML part preceding the replaced piece */
    str->append(xml.raw()->ptr(), cut->beg - xml.raw()->ptr() - offs) ||
    /* Put the replacement */
    str->append(replace->ptr(), replace->length()) ||
    /* Put the XML part following the replaced piece */
    str->append(end, xml.raw()->ptr() + xml.raw()->length() - end);
}


String *Item_func_xml_update::val_str(String *str)
{
  String *rep;

  null_value= 0;
  if (!nodeset_func || get_xml(&xml) ||
      !(rep= args[2]->val_str(&tmp_value3)) ||
      nodeset_func->type_handler() != &type_handler_xpath_nodeset ||
      nodeset_func->val_native(current_thd, &tmp_native_value2))
  {
    null_value= 1;
    return 0;
  }

  MY_XPATH_FLT *fltbeg= (MY_XPATH_FLT*) tmp_native_value2.ptr();
  MY_XPATH_FLT *fltend= (MY_XPATH_FLT*) tmp_native_value2.end();

  /* Allow replacing of one tag only */
  if (fltend - fltbeg != 1)
  {
    /* TODO: perhaps add a warning that more than one tag selected */
    return xml.raw();
  }

  const MY_XML_NODE *nodebeg= xml.node(fltbeg->num);

  if (!nodebeg->level)
  {
    /*
      Root element, without NameTest:
      UpdateXML(xml, '/', 'replacement');
      Just return the replacement string.
    */
    return rep;
  }

  return collect_result(str, nodebeg, rep) ? (String *) NULL : str;
}
