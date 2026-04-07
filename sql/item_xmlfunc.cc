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
    The expression is returned in xpath->expr

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


const Type_handler *Item_func_xml_update::xml_handler= NULL;


bool Item_func_xml_update::fix_length_and_dec(THD *thd)
{
  static LEX_CSTRING name= {STRING_WITH_LEN("XMLTYPE")};

  if (!xml_handler)
    xml_handler= Type_handler::handler_by_name(thd, name);

  return Item_xml_str_func::fix_length_and_dec(thd);
}


const Type_handler *Item_func_xml_update::type_handler() const
{
  return xml_handler ? xml_handler : Item_xml_str_func::type_handler();
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


/* XML Schema validation. */

struct xs_word
{
  LEX_CSTRING m_w;
  xs_word(const char *word, size_t len): m_w(LEX_CSTRING{word, len}) {}
  bool eq(const char *name, size_t len) const
  {
    return len == m_w.length && memcmp(m_w.str, name, len) == 0;
  }
};

/* XML schema keywords. */
static xs_word xs_abstract(STRING_WITH_LEN("abstract"));
static xs_word xs_all(STRING_WITH_LEN("all"));
static xs_word xs_any(STRING_WITH_LEN("any"));
static xs_word xs_anyAttribute(STRING_WITH_LEN("anyAttribute"));
static xs_word xs_attribute(STRING_WITH_LEN("attribute"));
static xs_word xs_attributeFormDefault(
                 STRING_WITH_LEN("attributeFormDefault"));
static xs_word xs_attributeGroup(STRING_WITH_LEN("attributeGroup"));
static xs_word xs_base(STRING_WITH_LEN("base"));
static xs_word xs_block(STRING_WITH_LEN("block"));
static xs_word xs_choice(STRING_WITH_LEN("choice"));
static xs_word xs_complexContent(STRING_WITH_LEN("complexContent"));
static xs_word xs_complexType(STRING_WITH_LEN("complexType"));
static xs_word xs_default(STRING_WITH_LEN("default"));
static xs_word xs_encoding(STRING_WITH_LEN("encoding"));
static xs_word xs_element(STRING_WITH_LEN("element"));
static xs_word xs_elementFormDefault(STRING_WITH_LEN("elementFormDefault"));
static xs_word xs_enumeration(STRING_WITH_LEN("enumeration"));
static xs_word xs_extension(STRING_WITH_LEN("extersion"));
static xs_word xs_final(STRING_WITH_LEN("final"));
static xs_word xs_fixed(STRING_WITH_LEN("fixed"));
static xs_word xs_form(STRING_WITH_LEN("form"));
static xs_word xs_fractionDigits(STRING_WITH_LEN("fractionDigits"));
static xs_word xs_group(STRING_WITH_LEN("group"));
static xs_word xs_id(STRING_WITH_LEN("id"));
static xs_word xs_key(STRING_WITH_LEN("key"));
static xs_word xs_keyref(STRING_WITH_LEN("keyref"));
static xs_word xs_itemType(STRING_WITH_LEN("itemType"));
static xs_word xs_length(STRING_WITH_LEN("length"));
static xs_word xs_list(STRING_WITH_LEN("list"));
static xs_word xs_memberTypes(STRING_WITH_LEN("memberTypes"));
static xs_word xs_maxExclusive(STRING_WITH_LEN("maxExclusive"));
static xs_word xs_maxInclusive(STRING_WITH_LEN("maxInclusive"));
static xs_word xs_maxLength(STRING_WITH_LEN("maxLength"));
static xs_word xs_maxOccurs(STRING_WITH_LEN("maxOccurs"));
static xs_word xs_minExclusive(STRING_WITH_LEN("minExclusive"));
static xs_word xs_minInclusive(STRING_WITH_LEN("minInclusive"));
static xs_word xs_minLength(STRING_WITH_LEN("minLength"));
static xs_word xs_minOccurs(STRING_WITH_LEN("minOccurs"));
static xs_word xs_mixed(STRING_WITH_LEN("mixed"));
static xs_word xs_name(STRING_WITH_LEN("name"));
static xs_word xs_namespace(STRING_WITH_LEN("namespace"));
static xs_word xs_notation(STRING_WITH_LEN("notation"));
static xs_word xs_nillable(STRING_WITH_LEN("nillable"));
static xs_word xs_ref(STRING_WITH_LEN("ref"));
static xs_word xs_pattern(STRING_WITH_LEN("pattrern"));
static xs_word xs_processContent(STRING_WITH_LEN("processContent"));
static xs_word xs_restriction(STRING_WITH_LEN("restriction"));
static xs_word xs_schema(STRING_WITH_LEN("schema"));
static xs_word xs_sequence(STRING_WITH_LEN("sequence"));
static xs_word xs_smipleContent(STRING_WITH_LEN("simpleContent"));
static xs_word xs_simpleType(STRING_WITH_LEN("simpleType"));
static xs_word xs_substitutionGroup(STRING_WITH_LEN("substitutionGroup"));
static xs_word xs_targetNamespace(STRING_WITH_LEN("targetNamespace"));
static xs_word xs_totalDigits(STRING_WITH_LEN("totalDigits"));
static xs_word xs_type(STRING_WITH_LEN("type"));
static xs_word xs_unbounded(STRING_WITH_LEN("unbounded"));
static xs_word xs_unique(STRING_WITH_LEN("unique"));
static xs_word xs_union(STRING_WITH_LEN("union"));
static xs_word xs_value(STRING_WITH_LEN("value"));
static xs_word xs_version(STRING_WITH_LEN("version"));
static xs_word xs_whiteSpace(STRING_WITH_LEN("whiteSpace"));
static xs_word xs_xml(STRING_WITH_LEN("xml"));
static xs_word xs_xml_lang(STRING_WITH_LEN("xml:lang"));

class XMLSchema_tag;
class XMLSchema_attribute;
class XMLSchema_xml;
class XMLSchema_schema;
class XMLSchema_attributeGroup_reference;
class XMLSchema_type;

class XMLSchema_item
{
public:
  static void *operator new(size_t size, MEM_ROOT *mem_root) throw ()
  {
    return alloc_root(mem_root, size);
  }
  static void operator delete(void *ptr __attribute__((unused)),
                              size_t size __attribute__((unused)))
  { TRASH_FREE(ptr, size); }

  static void operator delete(void *, MEM_ROOT*) {}
  virtual ~XMLSchema_item() = default;


  /* Parsing of the schema. */
  virtual bool enter_tag(MY_XML_VALIDATION_DATA *st,
                         const char *attr, size_t len);
  virtual bool enter_attr(MY_XML_VALIDATION_DATA *st,
                          const char *attr, size_t len);
  virtual bool value(MY_XML_VALIDATION_DATA *st, const char *attr, size_t len)
  {
    return MY_XML_OK;
  }
  virtual bool leave(MY_XML_VALIDATION_DATA *st, const char *attr, size_t len);

  /* Validation of an XML. */
  virtual bool validate_name(const char *attr, size_t len) { return false; }
  virtual void validate_prepare() {}
  virtual bool validate_done() { return MY_XML_OK; }

  virtual bool validate_value(MY_XML_VALIDATION_DATA *st,
                              const char *attr, size_t len)
  {
    return MY_XML_OK;
  }
  virtual bool validate_leave(MY_XML_VALIDATION_DATA *st,
                              const char *attr, size_t len)
  {
    return MY_XML_OK;
  }
  virtual bool validate_tag(MY_XML_VALIDATION_DATA *st,
                            const char *attr, size_t len)
  {
    return MY_XML_OK;
  }
  virtual bool validate_attr(MY_XML_VALIDATION_DATA *st,
                             const char *attr, size_t len)
  {
    return MY_XML_OK;
  }

  int validate_failed(MY_XML_VALIDATION_DATA *st);

  class XMLSchema_item *m_next;
};


class XMLSchema_annotation: public XMLSchema_item
{
  int m_level;
public:
  XMLSchema_annotation(): XMLSchema_item(), m_level(0) {}

  bool enter_tag(MY_XML_VALIDATION_DATA *st,
                 const char *attr, size_t len) override
  {
    m_level++;
    return MY_XML_OK;
  }
  bool enter_attr(MY_XML_VALIDATION_DATA *st,
                  const char *attr, size_t len) override
  {
    return XMLSchema_annotation::enter_tag(st, attr, len);
  }
  bool leave(MY_XML_VALIDATION_DATA *st, const char *attr, size_t len) override
  {
    if (m_level == 0)
      return XMLSchema_item::leave(st, attr, len);
    m_level--;
    return MY_XML_OK;
  }
};


class XMLSchema_root: public XMLSchema_item
{
public:
  XMLSchema_root(): XMLSchema_item() {}

  bool enter_tag(MY_XML_VALIDATION_DATA *st,
                 const char *attr, size_t len) override;

  bool validate_tag(MY_XML_VALIDATION_DATA *st,
                    const char *attr, size_t len) override;
#ifndef DBUG_OFF
  bool enter_attr(MY_XML_VALIDATION_DATA *st,
                  const char *attr, size_t len) override
  {
    DBUG_ASSERT(0); /* should never be called. */
    return MY_XML_OK;
  }
  bool leave(MY_XML_VALIDATION_DATA *st, const char *attr, size_t len) override
  {
    DBUG_ASSERT(0); /* should never be called. */
    return MY_XML_OK;
  }
  bool validate_leave(MY_XML_VALIDATION_DATA *st,
                      const char *attr, size_t len) override
  {
    DBUG_ASSERT(0); /* should never be called. */
    return MY_XML_OK;
  }
#endif /* DBUG_OFF */
};


/*
  That item reads the attribute value of some Schema tag.
  pops from stack after it.
*/
class XMLSchema_tag_attribute: public XMLSchema_item
{
public:
  const xs_word *m_name;
  const char *m_val;
  size_t m_val_len;
  XMLSchema_tag_attribute *m_next_attribute;

  XMLSchema_tag_attribute(const xs_word *name): XMLSchema_item(),
    m_name(name), m_val(NULL), m_val_len(0) {}

#ifndef DBUG_OFF
  /* should never happen. */
  bool enter_tag(MY_XML_VALIDATION_DATA *st,
                 const char *attr, size_t len) override
  {
    DBUG_ASSERT(0);
    return MY_XML_OK;
  }
  bool enter_attr(MY_XML_VALIDATION_DATA *st,
                 const char *attr, size_t len) override
  {
    DBUG_ASSERT(0);
    return MY_XML_OK;
  }
#endif /*DBUG_OFF*/

  bool value(MY_XML_VALIDATION_DATA *st, const char *attr, size_t len) override
  {
    m_val= attr;
    m_val_len= len;
    return MY_XML_OK;
  }

  bool eq_name(const char *name, size_t len) const
  {
    return m_name->eq(name, len);
  }
  bool is_set() const
  {
    return m_val_len > 0;
  }
  bool eq_value(const char *name, size_t len) const
  {
    return len == m_val_len && memcmp(m_val, name, len) == 0;
  }
};


class XMLSchema_tag_integer_attribute: public XMLSchema_tag_attribute
{
  int m_error;
public:
  longlong m_value_int;

  XMLSchema_tag_integer_attribute(const xs_word *name, longlong def_value= 1):
    XMLSchema_tag_attribute(name), m_error(0), m_value_int(def_value) {}
  bool value(MY_XML_VALIDATION_DATA *st, const char *attr, size_t len) override
  {
    char *tmp= (char *) attr + len;
    m_value_int= (longlong) my_strtoll10(attr, &tmp, &m_error);

    (void) XMLSchema_tag_attribute::value(st, attr, len);

    return m_error ? MY_XML_ERROR : MY_XML_OK;
  }
};


class XMLSchema_tag_unbounded_integer_attribute:
  public XMLSchema_tag_integer_attribute
{
public:
  XMLSchema_tag_unbounded_integer_attribute(const xs_word *name,
                                            longlong def_value= 1):
    XMLSchema_tag_integer_attribute(name, def_value) {}
  bool value(MY_XML_VALIDATION_DATA *st, const char *attr, size_t len) override
  {
    if (XMLSchema_tag_integer_attribute::value(st, attr, len) == MY_XML_OK)
      return MY_XML_OK;

    if (xs_unbounded.eq(attr, len))
    {
      m_value_int= LONGLONG_MAX;
      return MY_XML_OK;
    }

    return MY_XML_ERROR;
  }
};


class XMLSchema_tag_namespaced_attribute:public XMLSchema_tag_attribute
{
public:
  XMLSchema_tag_namespaced_attribute(const xs_word *name):
    XMLSchema_tag_attribute(name) {}
  bool value(MY_XML_VALIDATION_DATA *st, const char *attr, size_t len) override
  {
    size_t col_pos= 0;
    while (col_pos < len)
    {
      if (attr[col_pos++] == MY_XPATH_LEX_COLON)
      {
        attr+= col_pos;
        len-= col_pos;
        break;
      }
    }

    m_val= attr;
    m_val_len= len;
    return MY_XML_OK;
  }
};


class MY_XML_VALIDATION_DATA
{
public:
  int validation_failed;
  uint attr;
  XMLSchema_item skipped_attr;
  XMLSchema_annotation annotation;

  XMLSchema_root root;
  XMLSchema_item *s_stack;
  XMLSchema_schema *schema;
  XMLSchema_xml *xml;
  MEM_ROOT *mem_root;

  static void *operator new(size_t size, MEM_ROOT *mem_root) throw ()
  { return alloc_root(mem_root, size); }
  static void operator delete(void *ptr __attribute__((unused)),
                              size_t size __attribute__((unused)))
  { TRASH_FREE(ptr, size); }
  static void operator delete(void *, MEM_ROOT*) {}

  MY_XML_VALIDATION_DATA():
    s_stack(&root), schema(NULL), xml(NULL), mem_root(NULL)
  {
    root.m_next= NULL;
  };

  void push(XMLSchema_item *s)
  {
    s->m_next= s_stack;
    s_stack= s;
  }

  void pop() { s_stack= s_stack->m_next; }
};


bool XMLSchema_item::enter_tag(MY_XML_VALIDATION_DATA *st,
                               const char *attr, size_t len)
{
  st->push(&st->annotation);
  return MY_XML_OK;
}


bool XMLSchema_item::enter_attr(MY_XML_VALIDATION_DATA *st,
                                const char *attr, size_t len)
{
  st->push(&st->skipped_attr);
  return MY_XML_OK;
}


bool XMLSchema_item::leave(MY_XML_VALIDATION_DATA *st,
                           const char *attr, size_t len)
{
  st->pop();
  return MY_XML_OK;
}


int XMLSchema_item::validate_failed(MY_XML_VALIDATION_DATA *st)
{
  st->validation_failed= 1;
  return MY_XML_ERROR;
}


/*
  Parsing schema's tag. Handling tag's attributes.
*/

class XMLSchema_tag: public XMLSchema_item
{
public:
  XMLSchema_tag_attribute *m_tag_attributes;
  XMLSchema_tag_attribute m_id; /* eveny tag in schema has the "id" attr */
  XMLSchema_tag *m_next_tag;

  XMLSchema_annotation *m_annotation;
  bool declare_attribute(XMLSchema_tag_attribute *attr)
  {
    attr->m_next_attribute= m_tag_attributes;
    m_tag_attributes= attr;
    return FALSE;
  }

  XMLSchema_tag(): XMLSchema_item(),
                   m_tag_attributes(&m_id), m_id(&xs_id)
  {
    m_id.m_next_attribute= NULL;
  }
  XMLSchema_tag_attribute *find_attr(const char *attr, size_t len);

  bool enter_attr(MY_XML_VALIDATION_DATA *st,
                  const char *attr, size_t len) override;

  virtual bool validate_min_counter() const
  {
    return true;
  }
  virtual bool validate_max_counter() const
  {
    return true;
  }
  virtual void push_self(MY_XML_VALIDATION_DATA *st)
  {
    st->push(this);
  }
};


XMLSchema_tag_attribute *XMLSchema_tag::find_attr(const char *name, size_t len)
{
  for (XMLSchema_tag_attribute *atr= m_tag_attributes;
       atr;
       atr= atr->m_next_attribute)
  {
    if (atr->eq_name(name, len))
      return atr;
  }
  return NULL;
}


bool XMLSchema_tag::enter_attr(MY_XML_VALIDATION_DATA *st,
                               const char *attr, size_t len)
{
  XMLSchema_tag_attribute *atr= find_attr(attr, len);
  if (!atr)
    return XMLSchema_item::enter_attr(st, attr, len);
  st->push(atr);
  return MY_XML_OK;
}


/*
  Stores the description of an XML attribute and then validates it.
*/
class XMLSchema_attribute: public XMLSchema_tag
{
public:
  XMLSchema_tag_attribute m_atr_name;
  XMLSchema_tag_attribute m_atr_type;
  XMLSchema_tag_attribute m_atr_default;
  XMLSchema_tag_attribute m_atr_fixed;

  XMLSchema_type *m_type;
  XMLSchema_attribute *m_next_attribute;
  XMLSchema_attribute(): XMLSchema_tag(),
    m_atr_name(&xs_name),
    m_atr_type(&xs_type),
    m_atr_default(&xs_default),
    m_atr_fixed(&xs_fixed),
    m_type(NULL)
  {
    declare_attribute(&m_atr_name);
    declare_attribute(&m_atr_type);
    declare_attribute(&m_atr_default);
    declare_attribute(&m_atr_fixed);
  }

  bool enter_tag(MY_XML_VALIDATION_DATA *st,
                 const char *attr, size_t len) override;
  bool leave(MY_XML_VALIDATION_DATA *st,
             const char *attr, size_t len) override;

  void validate_prepare() override;
  bool validate_value(MY_XML_VALIDATION_DATA *st,
                      const char *attr, size_t len) override;
  bool validate_leave(MY_XML_VALIDATION_DATA *st,
                      const char *attr, size_t len) override;
  bool validate_tag(MY_XML_VALIDATION_DATA *st,
                    const char *attr, size_t len) override
  {
    return MY_XML_ERROR;
  }
  bool validate_attr(MY_XML_VALIDATION_DATA *st,
                     const char *attr, size_t len) override
  {
    return MY_XML_ERROR;
  }

  bool validate_name(const char *attr, size_t len) override
  {
    return m_atr_name.eq_value(attr, len);
  }
};


class XMLSchema_anyAttribute: public XMLSchema_tag
{
public:
  XMLSchema_tag_attribute m_atr_namespace;
  XMLSchema_tag_attribute m_atr_processContent;

  XMLSchema_anyAttribute(): XMLSchema_tag(),
    m_atr_namespace(&xs_namespace),
    m_atr_processContent(&xs_processContent)
  {
    declare_attribute(&m_atr_namespace);
    declare_attribute(&m_atr_processContent);
  }
};


class XMLSchema_any: public XMLSchema_anyAttribute
{
public:
  XMLSchema_tag_integer_attribute           m_minOccurs;
  XMLSchema_tag_unbounded_integer_attribute m_maxOccurs;

  XMLSchema_any(): XMLSchema_anyAttribute(),
    m_minOccurs(&xs_minOccurs),
    m_maxOccurs(&xs_maxOccurs)
  {
    declare_attribute(&m_minOccurs);
    declare_attribute(&m_maxOccurs);
  }
};


/*
  Supposed to be a member of tags supporting these inside:
    <attribute>
    <attributeGroup>
    <anyAttribute>
*/
class XMLSchema_std_attributes
{
public:
  XMLSchema_attribute *m_attributes; /* nested attributes. */
  XMLSchema_attributeGroup_reference *m_groups;    /* nested goups. */
  XMLSchema_anyAttribute *m_anyAttribute;

  XMLSchema_std_attributes():
    m_attributes(NULL), m_groups(NULL),
    m_anyAttribute(NULL) {}

  /*
    returns
      1 if the tag recognised and handled
      0 if the tag wasn't recognised
      -1 if an error happened
  */
  int enter_tag(MY_XML_VALIDATION_DATA *st, const char *attr, size_t len);
  int validate_attr(MY_XML_VALIDATION_DATA *st, const char *attr, size_t len);
};


class XMLSchema_attributeGroup_def: public XMLSchema_tag
{
  XMLSchema_attributeGroup_def *m_next_group;
  XMLSchema_std_attributes m_attributes;
public:
  XMLSchema_tag_attribute m_atr_name;

  XMLSchema_type *m_type;
  XMLSchema_attributeGroup_def(): XMLSchema_tag(),
    m_atr_name(&xs_name)
  {
    declare_attribute(&m_atr_name);
  }

  bool enter_tag(MY_XML_VALIDATION_DATA *st,
                 const char *attr, size_t len) override
  {
    int res;

    if (!(res= m_attributes.enter_tag(st, attr, len)))
      return XMLSchema_tag::enter_tag(st, attr, len);

    return res > 0 ? MY_XML_OK : MY_XML_ERROR;
  }
};


class XMLSchema_attributeGroup_reference: public XMLSchema_tag
{
  XMLSchema_attributeGroup_def *m_group;
public:
  XMLSchema_attributeGroup_reference *m_next_ref;
  XMLSchema_tag_attribute m_atr_ref;

  XMLSchema_type *m_type;
  XMLSchema_attributeGroup_reference(): XMLSchema_tag(),
    m_atr_ref(&xs_ref)
  {
    declare_attribute(&m_atr_ref);
  }

  bool leave(MY_XML_VALIDATION_DATA *st,
            const char *attr, size_t len) override;
  bool validate_attr(MY_XML_VALIDATION_DATA *st,
                     const char *attr, size_t len) override
  {
    /* TODO return MY_XML_OK if name found */
    return MY_XML_ERROR;
  }
};


class XMLSchema_builtin_type
{
protected:
  virtual ~XMLSchema_builtin_type() = default;
public:
  static void *operator new(size_t size, MEM_ROOT *mem_root) throw ()
  {
    return alloc_root(mem_root, size);
  }
  static void operator delete(void *ptr __attribute__((unused)),
                              size_t size __attribute__((unused)))
  { TRASH_FREE(ptr, size); }
  static void operator delete(void *, MEM_ROOT*) {}

  virtual bool valid_value(const char *value, size_t len) { return TRUE; }
  static XMLSchema_builtin_type *get_builtin_type_by_name(
           MY_XML_VALIDATION_DATA *st, const char *name, size_t len);

  XMLSchema_builtin_type() {}
};


class XMLSchema_string_builtin_type: public XMLSchema_builtin_type
{
public:
};


enum xml_num_char_classes {
  N_MNS,
  N_PLS,
  N_DIG,
  N_PNT,
  N_EXP,
  N_SPC,
  N_EOF,
  n_er,
  N_NUM_CLASSES
};


static enum xml_num_char_classes xml_num_chr_map[104] = {
  n_er, n_er,  n_er,  n_er, n_er, n_er,  n_er, n_er,
  n_er, N_SPC, N_SPC, n_er, n_er, N_SPC, n_er, n_er,
  n_er, n_er,  n_er,  n_er, n_er, n_er,  n_er, n_er,
  n_er, n_er,  n_er,  n_er, n_er, n_er,  n_er, n_er,

  N_SPC, n_er,  n_er,  n_er,  n_er,  n_er,  n_er,  n_er, /* !"#$%&'*/
  n_er,  n_er,  n_er,  N_PLS, n_er,  N_MNS, N_PNT, n_er, /*()*+,-./ */
  N_DIG, N_DIG, N_DIG, N_DIG, N_DIG, N_DIG, N_DIG, N_DIG,/*01234567*/
  N_DIG, N_DIG, n_er,  n_er,  n_er,  n_er,  n_er,  n_er, /*89:;<=>?*/

  n_er,  n_er,  n_er,  n_er,  n_er,  N_EXP, n_er,  n_er, /*@ABCDEFG*/
  n_er,  n_er,  n_er,  n_er,  n_er,  n_er,  n_er,  n_er, /*HIJKLMNO*/
  n_er,  n_er,  n_er,  n_er,  n_er,  n_er,  n_er,  n_er, /*PQRSTUVW*/
  n_er,  n_er,  n_er,  n_er,  n_er,  n_er,  n_er,  n_er, /*XYZ[\]^_*/

  n_er,  n_er,  n_er,  n_er,  n_er,  N_EXP, n_er,  n_er  /*`abcdefg*/
};


enum xml_num_states {
  NS_GO,  /* Initial state. */
  NS_END, /* Number ended. */
  NS_GMI, /* If the number starts with '-'. */
  NS_GPL, /* If the number starts with '+'. */
  NS_INT, /* Integer part. */
  NS_FRC, /* Fractional part. */
  NS_EXP, /* Exponential part begins. */
  NS_EX1, /* Exponential part started with + or -. */
  NS_EX2, /* Exponential part continues. */
  NS_NUM_STATES,
  E_SYN   /* Syntax error. */
};


static int xml_num_states[NS_NUM_STATES][N_NUM_CLASSES]=
{
/*         -        +        0..9   POINT   E      SPACE   EOF   BAD_SYM*/
/*GO*/   { NS_GMI, NS_GPL, NS_INT, NS_FRC, E_SYN,  NS_GO,  E_SYN,  E_SYN},
/*END*/  { E_SYN,  E_SYN,  E_SYN,  E_SYN,  E_SYN,  NS_END, NS_END, E_SYN},
/*GMI*/  { E_SYN,  E_SYN,  NS_INT, NS_FRC, E_SYN,  E_SYN,  E_SYN,  E_SYN},
/*GPL*/  { E_SYN,  E_SYN,  NS_INT, NS_FRC, E_SYN,  E_SYN,  E_SYN,  E_SYN},
/*INT*/  { E_SYN,  E_SYN,  NS_INT, NS_FRC, NS_EXP, NS_END, NS_END, E_SYN},
/*FRC*/  { E_SYN,  E_SYN,  NS_FRC, E_SYN,  NS_EXP, NS_END, NS_END, E_SYN},
/*EXP*/  { NS_EX1, NS_EX1, NS_EX2, E_SYN,  E_SYN,  E_SYN,  E_SYN,  E_SYN},
/*EX1*/  { E_SYN,  E_SYN,  NS_EX2, E_SYN,  E_SYN,  E_SYN,  E_SYN,  E_SYN},
/*EX2*/  { E_SYN,  E_SYN,  NS_EX2, E_SYN,  E_SYN,  NS_END, NS_END, E_SYN}
};


enum xml_num_types
{
  NUM_TYPE_NEG=1,       /* Number is negative. */
  NUM_TYPE_FRAC_PART=2, /* The fractional part is not empty. */
  NUM_TYPE_EXP=4,       /* The number has the 'e' part. */
};
const uint NUM_TYPE_UINT= NUM_TYPE_NEG | NUM_TYPE_FRAC_PART | NUM_TYPE_EXP;
const uint NUM_TYPE_INT= NUM_TYPE_FRAC_PART | NUM_TYPE_EXP;
const uint NUM_TYPE_DEC= NUM_TYPE_EXP;
const uint NUM_TYPE_FLOAT= 0; 

static uint xml_num_state_types[NS_NUM_STATES]=
{
/*GO*/   0,
/*END*/  0,
/*GMI*/  NUM_TYPE_NEG,
/*GPL*/  0,
/*INT*/  0,
/*FRC*/  NUM_TYPE_FRAC_PART,
/*EXP*/  NUM_TYPE_EXP,
/*EX1*/  0,
/*EX2*/  0,
};


class XMLSchema_num_builtin_type: public XMLSchema_builtin_type
{
public:
  int m_disallowed_types;
  XMLSchema_num_builtin_type(int disallowed_types): XMLSchema_builtin_type(),
    m_disallowed_types(disallowed_types) {}
  bool valid_value(const char *value, size_t len) override
  {
    int state= NS_GO;
    size_t pos= 0;

    while (len > pos)
    {
      int c= (int) value[pos++];
      if (c > 103)
        return 0;

      state= xml_num_states[state][xml_num_chr_map[c]];
      if (state == E_SYN ||
          xml_num_state_types[state] & m_disallowed_types)
        return 0;
    }

    return xml_num_states[state][N_EOF] == NS_END;
  }
};


/* Just to make type control possible. */
class XMLSchema_type: public XMLSchema_tag
{
};


class XMLSchema_any_type: public XMLSchema_type
{
public:
  int m_level;
  XMLSchema_any_type(): XMLSchema_type(), m_level(0) {}

  bool validate_leave(MY_XML_VALIDATION_DATA *st,
                      const char *attr, size_t len) override
  {
    if (m_level == 0)
      return MY_XML_ERROR;

    m_level--;
    return MY_XML_OK;
  }
  bool validate_tag(MY_XML_VALIDATION_DATA *st,
                    const char *attr, size_t len) override
  {
    m_level++;
    return MY_XML_OK;
  }
  bool validate_attr(MY_XML_VALIDATION_DATA *st,
                     const char *attr, size_t len) override
  {
    m_level++;
    return MY_XML_OK;
  }
  bool validate_done() override
  {
    return m_level == 0 ? MY_XML_OK : MY_XML_ERROR;
  }
};


class XMLSchema_simple_builtin_type: public XMLSchema_type
{
public:
  XMLSchema_builtin_type *m_int_type;
  XMLSchema_simple_builtin_type(XMLSchema_builtin_type *int_type):
    XMLSchema_type(), m_int_type(int_type) {}
  bool validate_tag(MY_XML_VALIDATION_DATA *st,
                    const char *attr, size_t len) override
  {
    return validate_failed(st);
  }
  virtual bool validate_attr(MY_XML_VALIDATION_DATA *st,
                             const char *attr, size_t len)
  {
    return validate_failed(st);
  }
  bool validate_value(MY_XML_VALIDATION_DATA *st,
                      const char *attr, size_t len) override
  {
    return m_int_type->valid_value(attr, len) ? MY_XML_OK :
                                                validate_failed(st);
  }
};


class XMLSchema_user_type: public XMLSchema_type
{
protected:
  XMLSchema_schema *m_schema;
  XMLSchema_tag_attribute m_type_name;
  XMLSchema_tag_attribute m_final;
public:
  XMLSchema_tag *m_compositor;
  XMLSchema_user_type *m_next_type;

  XMLSchema_user_type(XMLSchema_schema *schema): XMLSchema_type(),
    m_schema(schema),
    m_type_name(&xs_name),
    m_final(&xs_final),
    m_compositor(NULL)
  {
    declare_attribute(&m_type_name);
    declare_attribute(&m_final);
  }
  bool validate_name(const char *attr, size_t len) override
  {
    return m_type_name.eq_value(attr, len);
  }
  void validate_prepare() override
  {
    m_compositor->validate_prepare();
  }
  bool validate_tag(MY_XML_VALIDATION_DATA *st,
                    const char *attr, size_t len) override
  {
    return validate_failed(st);
  }
};


class XMLSchema_simpleType: public XMLSchema_user_type
{
public:
  XMLSchema_simpleType(XMLSchema_schema *schema=NULL):
    XMLSchema_user_type(schema) {}
  bool enter_tag(MY_XML_VALIDATION_DATA *st,
                 const char *attr, size_t len) override;
  bool leave(MY_XML_VALIDATION_DATA *st,
             const char *attr, size_t len) override;

  bool validate_value(MY_XML_VALIDATION_DATA *st,
                    const char *attr, size_t len) override
  {
    return m_compositor->validate_value(st, attr, len);
  }
};


class XMLSchema_complexType: public XMLSchema_user_type
{
  XMLSchema_tag_attribute m_mixed;
  XMLSchema_tag_attribute m_abstract;
  XMLSchema_tag_attribute m_block;
  /* XMLSchema_tag_attribute m_defaultAttributesApply; for 1.1 schema */

  XMLSchema_std_attributes m_attributes;
public:
  XMLSchema_complexType(XMLSchema_schema *schema=NULL):
    XMLSchema_user_type(schema),
    m_mixed(&xs_mixed),
    m_abstract(&xs_abstract),
    m_block(&xs_block)
  {
    declare_attribute(&m_mixed);
    declare_attribute(&m_abstract);
    declare_attribute(&m_block);
  }
  bool enter_tag(MY_XML_VALIDATION_DATA *st,
                 const char *attr, size_t len) override;
  bool leave(MY_XML_VALIDATION_DATA *st,
             const char *attr, size_t len) override;

  bool validate_attr(MY_XML_VALIDATION_DATA *st,
                     const char *attr, size_t len) override
  {
    return m_attributes.validate_attr(st, attr, len);
  }
  bool validate_tag(MY_XML_VALIDATION_DATA *st,
                    const char *attr, size_t len) override
  {
    return m_compositor->validate_tag(st, attr, len);
  }
  bool validate_leave(MY_XML_VALIDATION_DATA *st,
                      const char *attr, size_t len) override
  {
    return m_compositor->validate_leave(st, attr, len);
  }
};


class XMLSchema_facet: public XMLSchema_tag
{
protected:
  XMLSchema_tag_attribute m_value;
  XMLSchema_tag_attribute m_fixed;
public:
  XMLSchema_facet(): XMLSchema_tag(),
    m_value(&xs_value),
    m_fixed(&xs_fixed)
  {
    declare_attribute(&m_value);
    declare_attribute(&m_fixed);
  }

  bool is_set() const { return m_value.is_set(); }
};


class XMLSchema_enum_facet: public XMLSchema_facet
{
public:
  XMLSchema_enum_facet *m_next_enum;
  bool eq(const char *value, size_t len) const
  {
    return m_value.eq_value(value, len);
  }
};


class XMLSchema_restriction_in_simpleType: public XMLSchema_tag
{
  XMLSchema_type *m_base_type;
  XMLSchema_tag_attribute m_base;

  /* for numeric types. */
  XMLSchema_facet m_minInclusive, m_maxInclusive,
                  m_minExclusive, m_maxExclusive,
                  m_totalDigits, m_fractionDigits;

  /* for strings. */
  XMLSchema_facet m_length, m_minLength, m_maxLength,
                  m_pattern, m_whiteSpace;

  /* enum */
  XMLSchema_enum_facet *m_enumeration;

public:
  XMLSchema_restriction_in_simpleType(): XMLSchema_tag(),
    m_base_type(NULL),
    m_base(&xs_base),
    m_enumeration(NULL)
  {
    declare_attribute(&m_base);
  }
  bool enter_tag(MY_XML_VALIDATION_DATA *st,
                 const char *attr, size_t len) override;
  bool leave(MY_XML_VALIDATION_DATA *st,
            const char *attr, size_t len) override;
  bool validate_value(MY_XML_VALIDATION_DATA *st,
                      const char *attr, size_t len) override;
};


class XMLSchema_restriction_in_simpleContent:
  public XMLSchema_restriction_in_simpleType
{
  XMLSchema_std_attributes m_attributes;
public:
  bool enter_tag(MY_XML_VALIDATION_DATA *st,
                const char *attr, size_t len) override
  {
    int res;

    if (!(res= m_attributes.enter_tag(st, attr, len)))
      return XMLSchema_restriction_in_simpleType::enter_tag(st, attr, len);

    return res > 0 ? MY_XML_OK : MY_XML_ERROR;
  }
};


class XMLSchema_restriction_in_complexContent: public XMLSchema_complexType
{
public:
  XMLSchema_tag_attribute m_base;
  XMLSchema_restriction_in_complexContent(): XMLSchema_complexType(),
    m_base(&xs_base)
  {
    declare_attribute(&m_base);
  }
};


class XMLSchema_extension_in_simpleContent: public XMLSchema_tag
{
  XMLSchema_tag_attribute m_base;
  XMLSchema_std_attributes m_attributes;
public:
  XMLSchema_extension_in_simpleContent(): XMLSchema_tag(),
    m_base(&xs_base)
  {
    declare_attribute(&m_base);
  }
  bool enter_tag(MY_XML_VALIDATION_DATA *st,
                 const char *attr, size_t len) override
  {
    int res;

    if (!(res= m_attributes.enter_tag(st, attr, len)))
      return XMLSchema_tag::enter_tag(st, attr, len);

    return res > 0 ? MY_XML_OK : MY_XML_ERROR;
  }
};


class XMLSchema_extension_in_complexContent:
  public XMLSchema_restriction_in_complexContent
{
};


class XMLSchema_list: public XMLSchema_tag
{
  XMLSchema_tag_attribute m_attr_itemType;
  XMLSchema_type *m_type;
public:
  XMLSchema_list(): XMLSchema_tag(),
    m_attr_itemType(&xs_itemType),
    m_type(NULL)
  {
    declare_attribute(&m_attr_itemType);
  }
  bool enter_tag(MY_XML_VALIDATION_DATA *st,
                 const char *attr, size_t len) override;
  bool leave(MY_XML_VALIDATION_DATA *st,
             const char *attr, size_t len) override;
};


class XMLSchema_union: public XMLSchema_tag
{
  XMLSchema_tag_attribute m_attr_memberTypes;
  XMLSchema_user_type *m_nested_types; /* we have to preserver the order here */
  XMLSchema_user_type **m_nested_hook;
public:
  XMLSchema_union(): XMLSchema_tag(),
    m_attr_memberTypes(&xs_memberTypes),
    m_nested_types(NULL),
    m_nested_hook(&m_nested_types)
  {
    declare_attribute(&m_attr_memberTypes);
  }
  bool enter_tag(MY_XML_VALIDATION_DATA *st,
                 const char *attr, size_t len) override;
  bool leave(MY_XML_VALIDATION_DATA *st,
             const char *attr, size_t len) override;
};


class XMLSchema_simpleContent: public XMLSchema_tag
{
  XMLSchema_tag *m_nested; /* extension or restriction */
public:
  XMLSchema_simpleContent(): XMLSchema_tag(),
    m_nested(NULL) {}

  bool enter_tag(MY_XML_VALIDATION_DATA *st,
                 const char *attr, size_t len) override;
};


class XMLSchema_complexContent: public XMLSchema_simpleContent
{
  XMLSchema_tag_attribute m_atr_mixed;
  XMLSchema_tag *m_nested; /* extension or restriction */

public:
  XMLSchema_complexContent(): XMLSchema_simpleContent(),
    m_atr_mixed(&xs_mixed)
  {
    declare_attribute(&m_atr_mixed);
  }
  bool enter_tag(MY_XML_VALIDATION_DATA *st,
                 const char *attr, size_t len) override;
};


class XMLSchema_element_global;

class XMLSchema_all: public XMLSchema_tag
{
  XMLSchema_tag_integer_attribute           m_minOccurs;
  XMLSchema_tag_unbounded_integer_attribute m_maxOccurs;
  XMLSchema_tag **m_tags_hook;

public:
  XMLSchema_tag *m_tags;

  XMLSchema_all(): XMLSchema_tag(),
    m_minOccurs(&xs_minOccurs),
    m_maxOccurs(&xs_maxOccurs),
    m_tags_hook(&m_tags), m_tags(NULL)
  {
    declare_attribute(&m_minOccurs);
    declare_attribute(&m_maxOccurs);
  }

  void append_tag(XMLSchema_tag *tag);

  bool enter_tag(MY_XML_VALIDATION_DATA *st,
                 const char *attr, size_t len) override;
  bool leave(MY_XML_VALIDATION_DATA *st,
             const char *attr, size_t len) override
  {
    *m_tags_hook= NULL;
    return XMLSchema_tag::leave(st, attr, len);
  }
  void validate_prepare() override
  {
    for (XMLSchema_tag *cur= m_tags; cur; cur= cur->m_next_tag)
      cur->validate_prepare();
  }
};


class XMLSchema_sequence: public XMLSchema_all
{
  XMLSchema_tag *m_cur_tag;
public:
  bool enter_tag(MY_XML_VALIDATION_DATA *st,
                 const char *attr, size_t len) override;
  void validate_prepare() override
  {
    m_cur_tag= NULL;
  }
  bool validate_done() override { return m_cur_tag == NULL; }

  bool validate_tag(MY_XML_VALIDATION_DATA *st,
                    const char *attr, size_t len) override;
  bool validate_leave(MY_XML_VALIDATION_DATA *st,
                      const char *attr, size_t len) override;
};


class XMLSchema_choice: public XMLSchema_sequence
{
};


class XMLSchema_group_def: public XMLSchema_tag
{
  XMLSchema_tag *m_compositor;
public:
  XMLSchema_tag_attribute m_atr_name;

  XMLSchema_type *m_type;
  XMLSchema_group_def(): XMLSchema_tag(),
    m_compositor(NULL),
    m_atr_name(&xs_name)
  {
    declare_attribute(&m_atr_name);
  }

  bool enter_tag(MY_XML_VALIDATION_DATA *st,
                 const char *attr, size_t len) override;
};


class XMLSchema_group_reference: public XMLSchema_tag
{
  XMLSchema_tag_integer_attribute           m_minOccurs;
  XMLSchema_tag_unbounded_integer_attribute m_maxOccurs;
  XMLSchema_tag_attribute m_ref;
  XMLSchema_group_def *m_group;
public:
  XMLSchema_group_reference(): XMLSchema_tag(),
    m_minOccurs(&xs_minOccurs),
    m_maxOccurs(&xs_maxOccurs),
    m_ref(&xs_ref),
    m_group(NULL)
  {
    declare_attribute(&m_minOccurs);
    declare_attribute(&m_maxOccurs);
    declare_attribute(&m_ref);
  }

  bool leave(MY_XML_VALIDATION_DATA *st,
             const char *attr, size_t len) override
  {
    /*find global group in <schema> */
    return XMLSchema_tag::leave(st, attr, len);
  }
};


class XMLSchema_element_global: public XMLSchema_attribute
{
public:
  XMLSchema_element_global *m_next_element_global;
  XMLSchema_tag_attribute m_atr_nillable;
  XMLSchema_tag_attribute m_atr_abstract;
  XMLSchema_tag_attribute m_atr_substitutionGroup;
  XMLSchema_tag_attribute m_atr_block;
  XMLSchema_tag_attribute m_atr_final;

  XMLSchema_element_global(): XMLSchema_attribute(),
    m_atr_nillable(&xs_nillable),
    m_atr_abstract(&xs_abstract),
    m_atr_substitutionGroup(&xs_substitutionGroup),
    m_atr_block(&xs_block),
    m_atr_final(&xs_final)
  {
    declare_attribute(&m_atr_nillable);
    declare_attribute(&m_atr_abstract);
    declare_attribute(&m_atr_substitutionGroup);
    declare_attribute(&m_atr_block);
    declare_attribute(&m_atr_final);
  }
  bool enter_tag(MY_XML_VALIDATION_DATA *st,
                 const char *attr, size_t len) override;
  bool validate_attr(MY_XML_VALIDATION_DATA *st,
                     const char *attr, size_t len) override;
  bool validate_value(MY_XML_VALIDATION_DATA *st,
                      const char *attr, size_t len) override;
  bool validate_tag(MY_XML_VALIDATION_DATA *st,
                    const char *attr, size_t len) override;
  bool validate_leave(MY_XML_VALIDATION_DATA *st,
                      const char *attr, size_t len) override;

};


class XMLSchema_element_local: public XMLSchema_element_global
{
public:
  int m_counter;
  XMLSchema_tag_attribute m_atr_ref;
  XMLSchema_tag_integer_attribute           m_atr_minOccurs;
  XMLSchema_tag_unbounded_integer_attribute m_atr_maxOccurs;
  XMLSchema_tag_attribute m_atr_form;

  XMLSchema_element_local(): XMLSchema_element_global(),
    m_atr_ref(&xs_ref),
    m_atr_minOccurs(&xs_minOccurs),
    m_atr_maxOccurs(&xs_maxOccurs),
    m_atr_form(&xs_form)
  {
    declare_attribute(&m_atr_ref);
    declare_attribute(&m_atr_minOccurs);
    declare_attribute(&m_atr_maxOccurs);
    declare_attribute(&m_atr_form);
  }
  void validate_prepare() override
  {
    m_counter= 0;
  }
  bool validate_name(const char *attr, size_t len) override
  {
    return XMLSchema_element_global::validate_name(attr, len);
  }
  bool validate_min_counter() const override
  {
    return m_counter >= m_atr_minOccurs.m_value_int;
  }
  bool validate_max_counter() const override
  {
    return m_counter < m_atr_maxOccurs.m_value_int;
  }
  void push_self(MY_XML_VALIDATION_DATA *st) override
  {
    m_counter++;
    m_type->validate_prepare();
    st->push(this);
  }
};


/*
  This <xml> tag can appear both in the SCHEMA and the XML
  itself. So the rules are equal for both schema parsing and
  validation.
*/
class XMLSchema_xml_tag_attribute: public  XMLSchema_tag_attribute
{
public:
   XMLSchema_xml_tag_attribute(const xs_word *name):
     XMLSchema_tag_attribute(name) {}

  bool validate_value(MY_XML_VALIDATION_DATA *st,
                      const char *attr, size_t len) override
  {
    return value(st, attr, len);
  }
  bool validate_leave(MY_XML_VALIDATION_DATA *st,
                      const char *attr, size_t len) override
  {
    return leave(st, attr, len);
  }
};


class XMLSchema_xml: public XMLSchema_tag
{
public:
  XMLSchema_xml_tag_attribute m_atr_version;
  XMLSchema_xml_tag_attribute m_atr_encoding;
  XMLSchema_xml(): XMLSchema_tag(),
    m_atr_version(&xs_version),
    m_atr_encoding(&xs_encoding)
  {
    declare_attribute(&m_atr_version);
    declare_attribute(&m_atr_encoding);
  }
  bool validate_leave(MY_XML_VALIDATION_DATA *st,
                      const char *attr, size_t len) override
  {
    return leave(st, attr, len);
  }
  bool validate_tag(MY_XML_VALIDATION_DATA *st,
                    const char *attr, size_t len) override
  {
    return enter_tag(st, attr, len);
  }
  bool validate_attr(MY_XML_VALIDATION_DATA *st,
                     const char *attr, size_t len) override
  {
    return enter_attr(st, attr, len);
  }
};


class XMLSchema_schema: public XMLSchema_tag
{
  XMLSchema_tag_attribute m_atr_elementFormDefault;
  XMLSchema_tag_attribute m_atr_targetNamespace;
  XMLSchema_tag_attribute m_atr_attributeFormDefault;
  XMLSchema_tag_attribute m_atr_version;
  XMLSchema_tag_attribute m_atr_xml_lang;

public:
  XMLSchema_user_type *m_global_simpleTypes;
  XMLSchema_user_type *m_global_complexTypes;
  XMLSchema_attribute *m_global_attributes;
  XMLSchema_element_global *m_global_elements;
  XMLSchema_attributeGroup_def *m_global_attr_groups;

  /*
  TODO: should be supported.
  XMLSchema_attribute_group *m_attribute_groups;
  XMLSchema_type_def *m_type_defs;
  XMLSchema_simpleType *m_simple_types;
  */

  XMLSchema_schema(): XMLSchema_tag(),
    m_atr_elementFormDefault(&xs_elementFormDefault),
    m_atr_targetNamespace(&xs_targetNamespace),
    m_atr_attributeFormDefault(&xs_attributeFormDefault),
    m_atr_version(&xs_version),
    m_atr_xml_lang(&xs_xml_lang),
    m_global_simpleTypes(NULL),
    m_global_complexTypes(NULL),
    m_global_attributes(NULL),
    m_global_elements(NULL),
    m_global_attr_groups(NULL)
  {
    declare_attribute(&m_atr_elementFormDefault);
    declare_attribute(&m_atr_attributeFormDefault);
    declare_attribute(&m_atr_targetNamespace);
    declare_attribute(&m_atr_version);
    declare_attribute(&m_atr_xml_lang);
  }

  XMLSchema_type *find_type_by_name(MY_XML_VALIDATION_DATA *st,
                                    const char *name, size_t len) const;
  XMLSchema_attributeGroup_def *find_attribute_group_by_name(const char *name,
                                                             size_t len) const
  {
    return NULL;
  }
  XMLSchema_element_global *find_element(const char *name, size_t len) const
  {
    XMLSchema_element_global *el= m_global_elements;
    for(; el; el= el->m_next_element_global)
    {
      if (el->validate_name(name, len))
        return el;
    }
    return NULL;
  }

  bool validate_element(MY_XML_VALIDATION_DATA *st,
                        const char *attr, size_t len);
  bool enter_tag(MY_XML_VALIDATION_DATA *st,
                 const char *attr, size_t len) override;
  XMLSchema_type *find_simple_type(const char *name, size_t len) const;
  XMLSchema_type *find_complex_type(const char *name, size_t len) const;
};


/* implementations */

/* XML schema builtin types. */
static xs_word xs_anyType(STRING_WITH_LEN("anyType"));
static xs_word xs_anySimpleType(STRING_WITH_LEN("anySimpleType"));

/* integers */
static xs_word xs_integer(STRING_WITH_LEN("integer"));
static xs_word xs_long(STRING_WITH_LEN("long"));
static xs_word xs_int(STRING_WITH_LEN("int"));
static xs_word xs_short( STRING_WITH_LEN("short"));
static xs_word xs_byte(STRING_WITH_LEN("byte"));

/* non-negative integers */
static xs_word xs_nonNegativeInteger(STRING_WITH_LEN("nonNegativeInteger"));
static xs_word xs_positiveInteger(STRING_WITH_LEN("positiveInteger"));
static xs_word xs_unsignedInt(STRING_WITH_LEN("unsignedInt"));
static xs_word xs_unsignedLong(STRING_WITH_LEN("unsignedLong"));

/* numeric */
static xs_word xs_decimal(STRING_WITH_LEN("decimal"));
static xs_word xs_double(STRING_WITH_LEN("double"));
static xs_word xs_float(STRING_WITH_LEN("float"));
static xs_word xs_boolean(STRING_WITH_LEN("boolean"));
static xs_word xs_true(STRING_WITH_LEN("true"));
static xs_word xs_false(STRING_WITH_LEN("false"));

/* strings */
static xs_word xs_string(STRING_WITH_LEN("string"));
static xs_word xs_anyURI(STRING_WITH_LEN("anyURI"));
static xs_word xs_QName(STRING_WITH_LEN("QName"));
static xs_word xs_NOTATION(STRING_WITH_LEN("NOTATION"));
static xs_word xs_normalizedString(STRING_WITH_LEN("normalizedString"));
static xs_word xs_language(STRING_WITH_LEN("language"));
static xs_word xs_NMTOKEN(STRING_WITH_LEN("NMTOKEN"));

/* date/time */
static xs_word xs_date(STRING_WITH_LEN("date"));
static xs_word xs_time(STRING_WITH_LEN("time"));
static xs_word xs_dateTime(STRING_WITH_LEN("dateTime"));
static xs_word xs_duration(STRING_WITH_LEN("duration"));
static xs_word xs_gDay(STRING_WITH_LEN("gDay"));
static xs_word xs_gMonty(STRING_WITH_LEN("gMonth"));
static xs_word xs_gMonthDay(STRING_WITH_LEN("gMonthDay"));
static xs_word xs_gYear(STRING_WITH_LEN("gYear"));
static xs_word xs_gYearMonth(STRING_WITH_LEN("gYearMonth"));

/* identifiers */
static xs_word xs_ID(STRING_WITH_LEN("ID"));
static xs_word xs_IDREF(STRING_WITH_LEN("IDREF"));
static xs_word xs_ENTITY(STRING_WITH_LEN("ENTITY"));

/* binary */
static xs_word xs_base64Binary(STRING_WITH_LEN("base64Binary"));
static xs_word xs_hexBinary(STRING_WITH_LEN("hexBinary"));


class XMLSchema_bool_builtin_type: public XMLSchema_builtin_type
{
public:
  bool valid_value(const char *value, size_t len) override
  {
    return xs_true.eq(value, len) || xs_false.eq(value, len);
  }
};


XMLSchema_builtin_type *XMLSchema_builtin_type::get_builtin_type_by_name(
  MY_XML_VALIDATION_DATA *st, const char *name, size_t len)
{
  /* strings */
  if (xs_string.eq(name, len) ||
      xs_anyURI.eq(name, len) ||
      xs_QName.eq(name, len) ||
      xs_NOTATION.eq(name, len) ||
      xs_normalizedString.eq(name, len) ||
      xs_language.eq(name, len) ||
      xs_NMTOKEN.eq(name, len))
    return new(st->mem_root) XMLSchema_string_builtin_type;

  /* integers */
  if (xs_integer.eq(name, len))
    return new(st->mem_root) XMLSchema_num_builtin_type(NUM_TYPE_INT);

  if (xs_long.eq(name, len))
    return new(st->mem_root) XMLSchema_num_builtin_type(NUM_TYPE_INT);

  if (xs_int.eq(name, len))
    return new(st->mem_root) XMLSchema_num_builtin_type(NUM_TYPE_INT);

  if (xs_short.eq(name, len))
    return new(st->mem_root) XMLSchema_num_builtin_type(NUM_TYPE_INT);

  if (xs_byte.eq(name, len))
    return new(st->mem_root) XMLSchema_num_builtin_type(NUM_TYPE_INT);

/* non-negative integers */
  if (xs_nonNegativeInteger.eq(name, len))
    return new(st->mem_root) XMLSchema_num_builtin_type(NUM_TYPE_UINT);

  if (xs_positiveInteger.eq(name, len))
    return new(st->mem_root) XMLSchema_num_builtin_type(NUM_TYPE_UINT);

  if (xs_unsignedInt.eq(name, len))
    return new(st->mem_root) XMLSchema_num_builtin_type(NUM_TYPE_UINT);

  if (xs_unsignedLong.eq(name, len))
    return new(st->mem_root) XMLSchema_num_builtin_type(NUM_TYPE_UINT);

/* numeric */
  if (xs_decimal.eq(name, len))
    return new(st->mem_root) XMLSchema_num_builtin_type(NUM_TYPE_DEC);

  if (xs_double.eq(name, len))
    return new(st->mem_root) XMLSchema_num_builtin_type(NUM_TYPE_FLOAT);

  if (xs_float.eq(name, len))
    return new(st->mem_root) XMLSchema_num_builtin_type(NUM_TYPE_FLOAT);

  if (xs_boolean.eq(name, len))
    return new(st->mem_root) XMLSchema_bool_builtin_type;

  /* various types */
  if (xs_anySimpleType.eq(name, len))
    return new(st->mem_root) XMLSchema_builtin_type;

  return NULL;
}


bool XMLSchema_root::enter_tag(MY_XML_VALIDATION_DATA *st,
                               const char *attr, size_t len)
{
  if (xs_xml.eq(attr, len))
  {
    st->push(st->xml);
  }
  else if (xs_schema.eq(attr, len))
  {
    st->schema= new(st->mem_root) XMLSchema_schema;
    st->push(st->schema);
  }
  else
    return XMLSchema_item::enter_tag(st, attr, len);

  return MY_XML_OK;
}


bool XMLSchema_root::validate_tag(MY_XML_VALIDATION_DATA *st,
                                  const char *attr, size_t len)
{
  if (xs_xml.eq(attr, len))
  {
    st->push(st->xml);
  }
  else
  {
    return st->schema->validate_element(st, attr, len);
  }

  return MY_XML_OK;
}


int XMLSchema_std_attributes::enter_tag(MY_XML_VALIDATION_DATA *st,
                                        const char *attr, size_t len)
{
  if (xs_attribute.eq(attr, len))
  {
    XMLSchema_attribute *atr= new(st->mem_root) XMLSchema_attribute;

    atr->m_next_attribute= m_attributes;
    m_attributes= atr;

    st->push(atr);
  }
  else if (xs_attributeGroup.eq(attr, len))
  {
    XMLSchema_attributeGroup_reference *ref=
      new(st->mem_root) XMLSchema_attributeGroup_reference;
    ref->m_next_ref= m_groups;
    m_groups= ref;

    st->push(ref);
  }
  else if (xs_anyAttribute.eq(attr, len))
  {
    if (m_anyAttribute)
      return -1; /* can't have two anyAttribute-s */

    m_anyAttribute= new(st->mem_root) XMLSchema_anyAttribute;
    st->push(m_anyAttribute);
  }
  else
    return 0;

  return 1;
}


int XMLSchema_std_attributes::validate_attr(MY_XML_VALIDATION_DATA *st,
                                            const char *attr, size_t len)
{
  for (XMLSchema_attribute *atr= m_attributes;
       atr; atr= atr->m_next_attribute)
  {
    if (atr->validate_name(attr, len))
    {
      atr->push_self(st);
      return MY_XML_OK;
    }
  }

  for (XMLSchema_attributeGroup_reference *g= m_groups;
       g; g= g->m_next_ref)
  {
    if (g->validate_attr(st, attr, len) == MY_XML_OK)
      return MY_XML_OK;

    /* attribute wasn't found in the group. */
  }
  /* TODO handle anyAttributes */
  return MY_XML_ERROR;
}


bool XMLSchema_attribute::enter_tag(MY_XML_VALIDATION_DATA *st,
                                    const char *attr, size_t len)
{
  XMLSchema_type *t;

  if (xs_simpleType.eq(attr, len))
  {
    if (m_type)
      return MY_XML_ERROR; /* several types specified. */

    t= new(st->mem_root) XMLSchema_simpleType;
    m_type= t;
    st->push(t);
    return MY_XML_OK;
  }

  return XMLSchema_tag::enter_tag(st, attr, len);
}


bool XMLSchema_attribute::leave(MY_XML_VALIDATION_DATA *st,
                                const char *attr, size_t len)
{
  if (!m_atr_name.is_set())
    return MY_XML_ERROR; /* name must be specified. */

  if (m_type)
  {
    if (m_atr_type.is_set())
      return MY_XML_ERROR; /* only one type should be specified. */
  }
  else
  {
    m_type= st->schema->find_type_by_name(
                          st, m_atr_type.m_val, m_atr_type.m_val_len);
    if (!m_type)
      return MY_XML_ERROR;
  }

  return XMLSchema_item::leave(st, attr, len);
}


bool XMLSchema_attributeGroup_reference::leave(MY_XML_VALIDATION_DATA *st,
        const char *attr, size_t len)
{
  if (!m_atr_ref.is_set())
    return MY_XML_ERROR;

  m_group= st->schema->find_attribute_group_by_name(m_atr_ref.m_val,
                                                    m_atr_ref.m_val_len);

  return m_group ? MY_XML_OK : MY_XML_ERROR;
}


bool XMLSchema_simpleType::enter_tag(MY_XML_VALIDATION_DATA *st,
                                     const char *attr, size_t len)
{
  XMLSchema_tag *def= NULL;

  if (xs_restriction.eq(attr, len))
  {
    def= new(st->mem_root) XMLSchema_restriction_in_simpleType;
  }
  else if (xs_list.eq(attr, len))
  {
    def= new(st->mem_root) XMLSchema_list;
  }
  else if (xs_union.eq(attr, len))
  {
    def= new(st->mem_root) XMLSchema_union;
  }

  if (def)
  {
    if (m_compositor)
      return MY_XML_ERROR; /* exactly one tag allowed. */

    m_compositor= def;
    st->push(def);

    return MY_XML_OK;
  }

  return XMLSchema_user_type::enter_tag(st, attr, len);
}


bool XMLSchema_simpleType::leave(MY_XML_VALIDATION_DATA *st,
                                 const char *attr, size_t len)
{
  if (m_schema)
  {
    if (!m_type_name.is_set())
      return MY_XML_OK; /* type neme should be specified here. */

    m_next_type= m_schema->m_global_simpleTypes;
    m_schema->m_global_simpleTypes= this;
  }
  return XMLSchema_user_type::leave(st, attr, len);
}


bool XMLSchema_complexType::enter_tag(MY_XML_VALIDATION_DATA *st,
                                      const char *attr, size_t len)
{
  int res;
  XMLSchema_tag *def= NULL;

  if (xs_smipleContent.eq(attr, len))
  {
    def= new(st->mem_root) XMLSchema_simpleContent;
  }
  else if (xs_complexContent.eq(attr, len))
  {
    def= new(st->mem_root) XMLSchema_complexContent;
  }
  else if (xs_sequence.eq(attr, len))
  {
    def= new(st->mem_root) XMLSchema_sequence;
  }
  else if (xs_choice.eq(attr, len))
  {
    def= new(st->mem_root) XMLSchema_choice;
  }
  else if (xs_all.eq(attr, len))
  {
    def= new(st->mem_root) XMLSchema_all;
  }
  else if (xs_group.eq(attr, len))
  {
    def= new(st->mem_root) XMLSchema_group_reference;
  }
  else if ((res= m_attributes.enter_tag(st, attr, len)))
  {
    if (res < 0)
      return MY_XML_ERROR;
  }
  else
    return XMLSchema_user_type::enter_tag(st, attr, len);

  /* Can have exactly one of these tags.. */
  if (def)
  {
    if (m_compositor)
      return MY_XML_ERROR;

    st->push(def);
    m_compositor= def;
  }

  return MY_XML_OK;
}


bool XMLSchema_complexType::leave(MY_XML_VALIDATION_DATA *st,
                                  const char *attr, size_t len)
{
  if (m_schema)
  {
    if (!m_type_name.is_set())
      return MY_XML_OK; /* type neme should be specified here. */

    m_next_type= m_schema->m_global_complexTypes;
    m_schema->m_global_complexTypes= this;
  }
  return XMLSchema_user_type::leave(st, attr, len);
}


bool XMLSchema_restriction_in_simpleType::enter_tag(MY_XML_VALIDATION_DATA *st,
                                                    const char *attr,
                                                    size_t len)
{
  if (xs_minInclusive.eq(attr, len))
  {
    st->push(&m_minInclusive);
  }
  else if (xs_maxInclusive.eq(attr, len))
  {
    st->push(&m_maxInclusive);
  }
  if (xs_minExclusive.eq(attr, len))
  {
    st->push(&m_minExclusive);
  }
  else if (xs_maxExclusive.eq(attr, len))
  {
    st->push(&m_maxExclusive);
  }
  else if (xs_totalDigits.eq(attr, len))
  {
    st->push(&m_totalDigits);
  }
  else if (xs_fractionDigits.eq(attr, len))
  {
    st->push(&m_fractionDigits);
  }
  else if (xs_length.eq(attr, len))
  {
    st->push(&m_length);
  }
  else if (xs_minLength.eq(attr, len))
  {
    st->push(&m_minLength);
  }
  else if (xs_maxLength.eq(attr, len))
  {
    st->push(&m_maxLength);
  }
  else if (xs_pattern.eq(attr, len))
  {
    st->push(&m_pattern);
  }
  else if (xs_whiteSpace.eq(attr, len))
  {
    st->push(&m_whiteSpace);
  }
  else if (xs_enumeration.eq(attr, len))
  {
    XMLSchema_enum_facet *en= new(st->mem_root) XMLSchema_enum_facet;
    en->m_next_enum= m_enumeration;
    m_enumeration= en;
    st->push(en);
  }
  else
    return XMLSchema_tag::enter_tag(st, attr, len);

  return MY_XML_OK;
}


bool XMLSchema_restriction_in_simpleType::leave(
    MY_XML_VALIDATION_DATA *st, const char *attr, size_t len)
{
  if (m_base.is_set())
  {
    if (m_base_type)
      return MY_XML_ERROR; /* type should be specified only once. */

    m_base_type= st->schema->find_type_by_name(
                               st, m_base.m_val, m_base.m_val_len);
    if (!m_base_type)
      return MY_XML_ERROR;
  }
  else
  {
    if (!m_base_type)
      return MY_XML_ERROR; /* no type specified. */
  }

  return XMLSchema_tag::leave(st, attr, len);
}


bool XMLSchema_restriction_in_simpleType::validate_value(
            MY_XML_VALIDATION_DATA *st, const char *attr, size_t len)
{
  if (m_enumeration)
  {
    for (XMLSchema_enum_facet *en= m_enumeration; en; en= en->m_next_enum)
    {
      if (en->eq(attr, len))
        return MY_XML_OK;
    }
    return MY_XML_ERROR;
  }

  /* TODO check other facets. */
  return MY_XML_OK;
}


bool XMLSchema_list::enter_tag(MY_XML_VALIDATION_DATA *st,
                               const char *attr, size_t len)
{
  if (xs_simpleType.eq(attr, len))
  {
    if (m_type)
      return MY_XML_ERROR; /* several types specified. */

    m_type= new(st->mem_root) XMLSchema_simpleType;
    st->push(m_type);

    return MY_XML_OK;
  }

  return XMLSchema_tag::enter_tag(st, attr, len);
}


bool XMLSchema_list::leave(MY_XML_VALIDATION_DATA *st,
                           const char *attr, size_t len)
{
  bool res= XMLSchema_tag::leave(st, attr, len);

  if (m_type)
  {
    return m_attr_itemType.is_set() ? MY_XML_ERROR /* can't have two */ :
                                      res;
  }
  if (!m_attr_itemType.is_set())
    return MY_XML_ERROR; /* type must be specified. */

  m_type= st->schema->find_type_by_name(st, attr, len);

  return m_type ? res : MY_XML_ERROR;
}


bool XMLSchema_union::enter_tag(MY_XML_VALIDATION_DATA *st,
                                const char *attr, size_t len)
{
  if (xs_simpleType.eq(attr, len))
  {
    XMLSchema_simpleType *t= new(st->mem_root) XMLSchema_simpleType;
    st->push(t);
    *m_nested_hook= t->m_next_type;
    m_nested_hook= &t->m_next_type;
    return MY_XML_OK;
  }

  return XMLSchema_tag::enter_tag(st, attr, len);
}


bool XMLSchema_union::leave(MY_XML_VALIDATION_DATA *st,
                            const char *attr, size_t len)
{
  bool res= XMLSchema_tag::leave(st, attr, len);

  *m_nested_hook= NULL;

  /* TODO: add handling the typelist */
  return m_nested_types || m_attr_memberTypes.is_set() ? res : MY_XML_ERROR;
}


bool XMLSchema_simpleContent::enter_tag(MY_XML_VALIDATION_DATA *st,
                                        const char *attr, size_t len)
{
  XMLSchema_tag *t= NULL;

  if (xs_restriction.eq(attr, len))
  {
    t= new(st->mem_root) XMLSchema_restriction_in_simpleContent;
  }
  else if (xs_extension.eq(attr, len))
  {
    t= new(st->mem_root) XMLSchema_extension_in_simpleContent;
  }

  if (!t)
    return XMLSchema_tag::enter_tag(st, attr, len);

  if (m_nested)
    return MY_XML_ERROR; /* only one nested allowed. */

  st->push(t);
  m_nested= t;
  return MY_XML_OK;
}


bool XMLSchema_complexContent::enter_tag(MY_XML_VALIDATION_DATA *st,
                                         const char *attr, size_t len)
{
  XMLSchema_tag *t= NULL;

  if (xs_restriction.eq(attr, len))
  {
    t= new(st->mem_root) XMLSchema_restriction_in_complexContent;
  }
  else if (xs_extension.eq(attr, len))
  {
    t= new(st->mem_root) XMLSchema_extension_in_complexContent;
  }

  if (!t)
    return XMLSchema_simpleContent::enter_tag(st, attr, len);

  if (m_nested)
    return MY_XML_ERROR; /* only one nested allowed. */

  st->push(t);
  m_nested= t;
  return MY_XML_OK;
}


void XMLSchema_all::append_tag(XMLSchema_tag *tag)
{
  *m_tags_hook= tag;
  m_tags_hook= &tag->m_next_tag;
}


bool XMLSchema_all::enter_tag(MY_XML_VALIDATION_DATA *st,
                              const char *attr, size_t len)
{
  XMLSchema_tag *def= NULL;

  if (xs_element.eq(attr, len))
  {
    def= new(st->mem_root) XMLSchema_element_local;
  }
  else
    return XMLSchema_tag::enter_tag(st, attr, len);

  if (def == NULL)
    return MY_XML_ERROR; /* OOM */

  append_tag(def);
  st->push(def);

  return MY_XML_OK;
}


bool XMLSchema_sequence::enter_tag(MY_XML_VALIDATION_DATA *st,
                                   const char *attr, size_t len)
{
  XMLSchema_tag *def= NULL;

  if (xs_sequence.eq(attr, len))
  {
    def= new(st->mem_root) XMLSchema_sequence;
  }
  else if (xs_choice.eq(attr, len))
  {
    def= new(st->mem_root) XMLSchema_choice;
  }
  else if (xs_any.eq(attr, len))
  {
    def= new(st->mem_root) XMLSchema_any;
  }
  else if (xs_group.eq(attr, len))
  {
    def= new(st->mem_root) XMLSchema_group_reference;
  }
  else
    return XMLSchema_all::enter_tag(st, attr, len);

  if (def == NULL)
    return MY_XML_ERROR; /* OOM */

  append_tag(def);
  st->push(def);

  return MY_XML_OK;
}


bool XMLSchema_sequence::validate_tag(MY_XML_VALIDATION_DATA *st,
                                      const char *attr, size_t len)
{
  if (!m_cur_tag)
  {
    if (!(m_cur_tag= m_tags))
      return MY_XML_ERROR;
    m_cur_tag->validate_prepare();
  }

  for (;;)
  {
    if (m_cur_tag->validate_name(attr, len))
    {
      if (m_cur_tag->validate_max_counter())
        break;
      /* can't have another element with this name. */
      return validate_failed(st);
    }
    else
    {
      if (!m_cur_tag->validate_min_counter())
      {
        /* had to be another one with this name. */
        return validate_failed(st);
      }
      if (!(m_cur_tag= m_cur_tag->m_next_tag))
        return MY_XML_ERROR;
      m_cur_tag->validate_prepare();
    }
  }

  m_cur_tag->push_self(st);
  return MY_XML_OK;
}


bool XMLSchema_sequence::validate_leave(MY_XML_VALIDATION_DATA *st,
                                        const char *attr, size_t len)
{
  while (m_cur_tag) /* not all tags were met */
  {
    if (!m_cur_tag->validate_min_counter())
    {
      return validate_failed(st);
    }
    m_cur_tag= m_cur_tag->m_next_tag;
  }

  return MY_XML_OK;
}


bool XMLSchema_group_def::enter_tag(MY_XML_VALIDATION_DATA *st,
                                    const char *attr, size_t len)
{
  XMLSchema_tag *def= NULL;

  if (xs_sequence.eq(attr, len))
  {
    def= new(st->mem_root) XMLSchema_sequence;
  }
  else if (xs_choice.eq(attr, len))
  {
    def= new(st->mem_root) XMLSchema_choice;
  }
  else if (xs_all.eq(attr, len))
  {
    def= new(st->mem_root) XMLSchema_all;
  }
  else
    return XMLSchema_tag::enter_tag(st, attr, len);

  if (def == NULL || m_compositor != NULL)
    return MY_XML_ERROR;

  m_compositor= def;
  st->push(def);

  return MY_XML_OK;
}


bool XMLSchema_element_global::enter_tag(MY_XML_VALIDATION_DATA *st,
                                         const char *attr, size_t len)
{
  XMLSchema_type *def= NULL;

  if (xs_complexType.eq(attr, len))
  {
    def= new(st->mem_root) XMLSchema_complexType;
  }
/* TODO implement these
  else if (xs_unique.eq(attr, len))
  {
  }
  else if (xs_key.eq(attr, len))
  {
  }
  else if (xs_keyref.eq(attr, len))
  {
  }
*/
  else
    return XMLSchema_attribute::enter_tag(st, attr, len);

  if (def == NULL || m_type != NULL)
    return MY_XML_ERROR;

  m_type= def;
  st->push(def);

  return MY_XML_OK;
}


bool XMLSchema_element_global::validate_attr(MY_XML_VALIDATION_DATA *st,
                                             const char *attr, size_t len)
{
  return m_type->validate_attr(st, attr, len);
}


bool XMLSchema_element_global::validate_value(MY_XML_VALIDATION_DATA *st,
                                              const char *attr, size_t len)
{
  return m_type->validate_value(st, attr, len);
}


bool XMLSchema_element_global::validate_tag(MY_XML_VALIDATION_DATA *st,
                                            const char *attr, size_t len)
{
  return m_type->validate_tag(st, attr, len);
}


bool XMLSchema_element_global::validate_leave(MY_XML_VALIDATION_DATA *st,
                                              const char *attr, size_t len)
{
  st->pop();
  return m_type->validate_leave(st, attr, len);
}


bool XMLSchema_schema::enter_tag(MY_XML_VALIDATION_DATA *st,
                                 const char *attr, size_t len)
{
  XMLSchema_item *def= NULL;

  if (xs_element.eq(attr, len))
  {
    XMLSchema_element_global *el= new(st->mem_root) XMLSchema_element_global;
    el->m_next_element_global= m_global_elements;
    m_global_elements= el;
    def= el;
  }
  else if (xs_attribute.eq(attr, len))
  {
    XMLSchema_attribute *atr= new(st->mem_root) XMLSchema_attribute;
    atr->m_next_attribute= m_global_attributes;;
    m_global_attributes= atr;
    def= atr;
  }
  else if (xs_complexType.eq(attr, len))
  {
    def= new(st->mem_root) XMLSchema_complexType(this);
  }
  else if (xs_simpleType.eq(attr, len))
  {
    def= new(st->mem_root) XMLSchema_simpleType(this);
  }
  else if (xs_group.eq(attr, len))
  {
    def= new(st->mem_root) XMLSchema_group_def;
  }
  else if (xs_attributeGroup.eq(attr, len))
  {
    def= new(st->mem_root) XMLSchema_attributeGroup_def;
  }
  else if (xs_notation.eq(attr, len))
  {
    def= new(st->mem_root) XMLSchema_annotation;
  }
  else
    return XMLSchema_tag::enter_tag(st, attr, len);

  if (def == NULL)
    return MY_XML_ERROR; /*OOM*/

  st->push(def);
  return MY_XML_OK;
}


XMLSchema_type *XMLSchema_schema::find_simple_type(
                                    const char *name, size_t len) const
{
  XMLSchema_user_type *t= m_global_simpleTypes;
  for(; t; t= t->m_next_type)
  {
    if (t->validate_name(name, len))
      return t;
  }
  return NULL;
}


XMLSchema_type *XMLSchema_schema::find_complex_type(
                                    const char *name, size_t len) const
{
  XMLSchema_user_type *t= m_global_complexTypes;
  for(; t; t= t->m_next_type)
  {
    if (t->validate_name(name, len))
      return t;
  }
  return NULL;
}


XMLSchema_type *XMLSchema_schema::find_type_by_name(
   MY_XML_VALIDATION_DATA *st, const char *name, size_t len) const
{
  size_t col_pos= 0;
  XMLSchema_builtin_type *builtin_type;
  XMLSchema_type *result;

  /* TODO check the namespace properly. */
  while (col_pos < len)
  {
    if (name[col_pos++] == MY_XPATH_LEX_COLON)
    {
      name+= col_pos;
      len-= col_pos;
      break;
    }
  }

  builtin_type= XMLSchema_builtin_type::get_builtin_type_by_name(
      st, name, len);

  if (builtin_type)
  {
    result= new(st->mem_root) XMLSchema_simple_builtin_type(builtin_type);
  }
  else
  {
    if (xs_anyType.eq(name, len))
      result= new(st->mem_root) XMLSchema_any_type;
    else
    {
      result= find_simple_type(name, len);
      if (!result)
        result= find_complex_type(name, len);
    }
  }

  return result;
}

bool XMLSchema_schema::validate_element(MY_XML_VALIDATION_DATA *st,
                                        const char *attr, size_t len)
{
  XMLSchema_element_global *e= find_element(attr, len);
  if (!e)
    return validate_failed(st);

  e->validate_prepare();
  st->push(e);
  return MY_XML_OK;
}


/* implementations cur */

/*
XMLSchema_attribute *
XMLSchema_element_global::find_attribute(const char *name, size_t len)
{
  for (XMLSchema_attribute *atr= m_attributes;
       atr;
       atr= atr->m_next_attribute)
  {
    if (len == atr->m_atr_name.m_val_len &&
        memcmp(atr->m_atr_name.m_val, name, len) == 0)
      return atr;
  }
  return NULL;
}


bool XMLSchema_element::validate_attr(MY_XML_VALIDATION_DATA *st,
    const char *name, size_t len)
{
  XMLSchema_attribute *attr= find_attribute(name, len);
  if (!attr)
    return MY_XML_ERROR;

  st->push(attr);
  return MY_XML_OK;
}


bool XMLSchema_schema::validate_tag(MY_XML_VALIDATION_DATA *st,
                                    const char *name, size_t len)
{
  XMLSchema_tag *tag= find_tag(name, len);
  if (!tag)
    return validate_failed(st);
  st->push(tag);
  return MY_XML_OK;
}
*/


void XMLSchema_attribute::validate_prepare()
{
  m_type->validate_prepare();
}


bool XMLSchema_attribute::validate_value(MY_XML_VALIDATION_DATA *st,
                                         const char *attr, size_t len)
{
  return m_type->validate_value(st, attr, len);
}


bool XMLSchema_attribute::validate_leave(MY_XML_VALIDATION_DATA *st,
                                         const char *attr, size_t len)
{
  st->pop();
  return m_type->validate_leave(st, attr, len);
}


extern "C" {
static int schema_enter(MY_XML_PARSER *st,const char *attr, size_t len)
{
  MY_XML_VALIDATION_DATA *data= (MY_XML_VALIDATION_DATA *) st->user_data;

  if (st->current_node_type == MY_XML_NODE_TAG)
  {
    size_t col_pos= 0;
    /* TODO check the namespace properly. */
    while (col_pos < len)
    {
      if (attr[col_pos++] == MY_XPATH_LEX_COLON)
      {
        attr+= col_pos;
        len-= col_pos;
        break;
      }
    }

    if (!data->s_stack)
    {
      DBUG_ASSERT(0); /* Shouldn't happen. */
      return MY_XML_ERROR;
    }
    return data->s_stack->enter_tag(data, attr, len);
  }
  else if (st->current_node_type == MY_XML_NODE_TEXT)
  {
  }
  else if (st->current_node_type == MY_XML_NODE_ATTR)
  {
    if (!data->s_stack)
    {
      DBUG_ASSERT(0); /* Shouldn't happen. */
      return MY_XML_ERROR;
    }
    return data->s_stack->enter_attr(data, attr, len);
  }

  return MY_XML_OK;
}


static int schema_value(MY_XML_PARSER *st,const char *attr, size_t len)
{
  MY_XML_VALIDATION_DATA *data= (MY_XML_VALIDATION_DATA *) st->user_data;

  return data->s_stack->value(data, attr, len);
}


static int schema_leave(MY_XML_PARSER *st,const char *attr, size_t len)
{
  MY_XML_VALIDATION_DATA *data= (MY_XML_VALIDATION_DATA *) st->user_data;

  return data->s_stack->leave(data, attr, len);
}

} /* extern "C" */


static int schema_parse(THD *thd, const String *xml,
                        MY_XML_VALIDATION_DATA *user_data)
{
  MY_XML_PARSER p;
  int rc;

  user_data->mem_root= thd->mem_root;

  /* Prepare XML parser */
  my_xml_parser_create(&p);
  p.flags= MY_XML_FLAG_RELATIVE_NAMES | MY_XML_FLAG_SKIP_TEXT_NORMALIZATION;

  my_xml_set_enter_handler(&p, schema_enter);
  my_xml_set_value_handler(&p, schema_value);
  my_xml_set_leave_handler(&p, schema_leave);
  my_xml_set_user_data(&p, (void*) user_data);

  /* Execute XML parser */
  if ((rc= my_xml_parse(&p, xml->ptr(), xml->length())) != MY_XML_OK)
  {
    char buf[128];
    my_snprintf(buf, sizeof(buf)-1,
                "XML Schema parse error at line %d pos %lu: %s",
                my_xml_error_lineno(&p) + 1,
                (ulong) my_xml_error_pos(&p) + 1,
                my_xml_error_string(&p));
    my_printf_error(ER_WRONG_VALUE, ER_THD(thd, ER_WRONG_VALUE), MYF(0),
                    "XML", buf);
    return 1;
  }

  if (user_data->schema == NULL)
  {
    my_printf_error(ER_UNKNOWN_ERROR, "Invalid XML Schema.", MYF(0));
    return 1;
  }

  return 0;
}


bool Item_func_xml_isvalid::fix_length_and_dec(THD *thd)
{
  String *schema_str;

  if (Item_bool_func::fix_length_and_dec(thd))
    return TRUE;

  status_var_increment(current_thd->status_var.feature_xml);

  set_maybe_null();

  m_data= NULL;
  if (!args[1]->const_item())
  {
    my_printf_error(ER_UNKNOWN_ERROR,
                    "Only constant XML Schema-s are supported.", MYF(0));
    return TRUE;
  }
  if (!(schema_str= args[1]->val_str(&m_tmp_schema)))
  {
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_WRONG_VALUE,
                        ER_THD(thd, ER_WRONG_VALUE),
                        "XML", "NULL as XML Schema");
    return FALSE;
  }

  m_data= new(thd->mem_root) MY_XML_VALIDATION_DATA;
  m_data->xml= new(thd->mem_root) XMLSchema_xml;

  if (schema_parse(thd, schema_str, m_data))
    return TRUE;

  return FALSE;
}


extern "C" {
static int validation_enter(MY_XML_PARSER *st,const char *attr, size_t len)
{
  MY_XML_VALIDATION_DATA *data= (MY_XML_VALIDATION_DATA *) st->user_data;
  int result= MY_XML_OK;

  if (st->current_node_type == MY_XML_NODE_TAG)
  {
    result= data->s_stack->validate_tag(data, attr, len);
  }
  else if (st->current_node_type == MY_XML_NODE_ATTR)
  {
    result= data->s_stack->validate_attr(data, attr, len);
  }
  else if (st->current_node_type == MY_XML_NODE_TEXT)
  {
  }

  return result;
}


int validation_value(MY_XML_PARSER *st,const char *attr, size_t len)
{
  MY_XML_VALIDATION_DATA *data= (MY_XML_VALIDATION_DATA *) st->user_data;

  return data->s_stack->validate_value(data, attr, len);
}


int validation_leave(MY_XML_PARSER *st,const char *attr, size_t len)
{
  MY_XML_VALIDATION_DATA *data= (MY_XML_VALIDATION_DATA *) st->user_data;

  return data->s_stack->validate_leave(data, attr, len);
}

} /* extern "C" */


static int validate_schema(const String *xml,
                           MY_XML_VALIDATION_DATA *user_data)
{
  MY_XML_PARSER p;

  /* Prepare XML parser */
  my_xml_parser_create(&p);
  p.flags= MY_XML_FLAG_RELATIVE_NAMES | MY_XML_FLAG_SKIP_TEXT_NORMALIZATION;

  my_xml_set_enter_handler(&p, validation_enter);
  my_xml_set_value_handler(&p, validation_value);
  my_xml_set_leave_handler(&p, validation_leave);
  my_xml_set_user_data(&p, (void*) user_data);

  user_data->validation_failed= 0;
  user_data->schema->m_next= NULL;
  user_data->s_stack= &user_data->root;

  /* Execute XML parser */
  return my_xml_parse(&p, xml->ptr(), xml->length()) == MY_XML_OK;
}


bool Item_func_xml_isvalid::val_bool()
{
  String *xml= args[0]->val_str(&m_tmp_xml);

  if ((null_value= !xml || m_data == NULL))
    return FALSE;

  return validate_schema(xml, m_data);
}
