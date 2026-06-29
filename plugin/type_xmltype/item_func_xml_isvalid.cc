/* Copyright (c) 2026, MariaDB

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
#define MYSQL_SERVER
#include "mariadb.h"
#include "my_xml.h"
#include "sql_class.h"
#include "sql_lex.h"
#include "sql_type_xmltype.h"

/* XML Schema validation. */

struct xs_word
{
  LEX_CSTRING m_w;
  xs_word(const char *word, size_t len): m_w(LEX_CSTRING{word, len}) {}
  bool eq(const char *name, size_t len) const
  {
    return len == m_w.length && memcmp(m_w.str, name, len) == 0;
  }

  size_t length() const { return m_w.length; }
  const char *str() const { return m_w.str; }
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
static xs_word xs_extension(STRING_WITH_LEN("extension"));
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
static xs_word xs_notation(STRING_WITH_LEN("annotation"));
static xs_word xs_nillable(STRING_WITH_LEN("nillable"));
static xs_word xs_ref(STRING_WITH_LEN("ref"));
static xs_word xs_pattern(STRING_WITH_LEN("pattern"));
static xs_word xs_processContents(STRING_WITH_LEN("processContents"));
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
static xs_word xs_use(STRING_WITH_LEN("use"));
static xs_word xs_value(STRING_WITH_LEN("value"));
static xs_word xs_version(STRING_WITH_LEN("version"));
static xs_word xs_whiteSpace(STRING_WITH_LEN("whiteSpace"));
static xs_word xs_xml(STRING_WITH_LEN("xml"));
static xs_word xs_xmlns(STRING_WITH_LEN("xmlns"));
static xs_word xs_xml_lang(STRING_WITH_LEN("xml:lang"));

static xs_word xs_uri_short(STRING_WITH_LEN("http://w3.org"));
static xs_word xs_uri_www(STRING_WITH_LEN("http://www.w3.org"));
static xs_word xs_uri(STRING_WITH_LEN("http://www.w3.org/2001/XMLSchema"));

class XMLSchema_tag;
class XMLSchema_attribute;
class XMLSchema_xml;
class XMLSchema_schema;
class XMLSchema_group_def;
class XMLSchema_attributeGroup_def;
class XMLSchema_attributeGroup_reference;
class XMLSchema_type;

class XMLSchema_item: public Sql_alloc
{
public:
  XMLSchema_item(const XMLSchema_item &)= delete;
  void operator=(const XMLSchema_item &) = delete;

  XMLSchema_item() {}
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
  /*
    If the type can't be resolved, the function returns TRUE and
    sets the unresolved name in the 'bad_type'.
  */
  virtual bool resolve_type(MY_XML_VALIDATION_DATA *st, LEX_CSTRING *bad_type)
  {
    DBUG_ASSERT(0);
    return false;
  }

  /* Validation of an XML. */

  /* check the name of the rule. */
  virtual bool validate_name(const char *attr, size_t len)
  {
    DBUG_ASSERT(0);
    return false;
  }

  virtual void validate_prepare() {}
  enum vtn_result{
    VTN_ACCEPTED,  /* tag accepted by this rule. */
    VTN_CONTINUE,  /* tag wasn't accepted, need to check other rules. */
    VTN_ERROR      /* XML is invalid. */
  };

  virtual enum vtn_result validate_tag_name(MY_XML_VALIDATION_DATA *st,
                    const char *attr, size_t len)
  {
    return VTN_CONTINUE;
  }

  virtual bool is_validate_done() { return true; }


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
    DBUG_ASSERT(0);
    return MY_XML_OK;
  }
  virtual bool validate_attr(MY_XML_VALIDATION_DATA *st,
                             const char *attr, size_t len)
  {
    return MY_XML_ERROR;
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

  bool validate_leave(MY_XML_VALIDATION_DATA *st,
                      const char *attr, size_t len) override
  {
    return XMLSchema_annotation::leave(st, attr, len);
  }
  bool validate_attr(MY_XML_VALIDATION_DATA *st,
                     const char *attr, size_t len) override
  {
    return XMLSchema_annotation::enter_tag(st, attr, len);
  }
  bool validate_tag(MY_XML_VALIDATION_DATA *st,
                    const char *attr, size_t len) override
  {
    return XMLSchema_annotation::enter_tag(st, attr, len);
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


class XMLSchema_tag_xmlns_attribute: public XMLSchema_tag_attribute
{
public:
  LEX_CSTRING m_ns_name;
  XMLSchema_tag_xmlns_attribute();
  bool value(MY_XML_VALIDATION_DATA *st, const char *attr, size_t len) override;
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


#define MY_XPATH_LEX_COLON    ':'

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


class MY_XML_VALIDATION_DATA: public Sql_alloc
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

  LEX_CSTRING schema_namespace{NULL, 0};

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

  void add_item_to_resolve(XMLSchema_item *t);
  bool set_schema_namespace(const LEX_CSTRING *ns)
  {
    /*
      It's weird feature of the XML Schema.
      <nsp:schema xmlns:nsp="http://www.w3.org">
      Here the namespace for the <schema> and the namespace
      declared with the xmlns must exactly coincide.
      So we don't actually set anything. It supposed to be set at this point.
      Just do the necessary check.
    */

    if (ns->length == 0)
    {
      return schema_namespace.length;
    }

    if (schema_namespace.length != ns->length+1 ||
        memcmp(schema_namespace.str, ns->str, ns->length) != 0)
      return true;

    return false;
  }
  bool namespace_not_specified() const
  {
    return schema_namespace.str == NULL;
  }
  bool xs_namespace(const char **name, size_t *len) const;
  bool namespace_empty() const
  {
    return schema_namespace.str != NULL && schema_namespace.length == 0;
  }
  XMLSchema_attributeGroup_def *find_attribute_group_by_name(const char *name,
                                                             size_t len) const;
  XMLSchema_group_def *find_element_group_by_name(const char *name,
                                                  size_t len) const;
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
  XMLSchema_tag_attribute m_atr_use;
  XMLSchema_tag_attribute m_atr_ref;

  XMLSchema_type *m_type;
  XMLSchema_attribute *m_next_attribute;
  XMLSchema_attribute(): XMLSchema_tag(),
    m_atr_name(&xs_name),
    m_atr_type(&xs_type),
    m_atr_default(&xs_default),
    m_atr_fixed(&xs_fixed),
    m_atr_use(&xs_use),
    m_atr_ref(&xs_ref),
    m_type(NULL)
  {
    declare_attribute(&m_atr_name);
    declare_attribute(&m_atr_type);
    declare_attribute(&m_atr_default);
    declare_attribute(&m_atr_fixed);
    declare_attribute(&m_atr_use);
    declare_attribute(&m_atr_ref);
  }

  bool enter_tag(MY_XML_VALIDATION_DATA *st,
                 const char *attr, size_t len) override;
  bool leave(MY_XML_VALIDATION_DATA *st,
             const char *attr, size_t len) override;
  bool resolve_type(MY_XML_VALIDATION_DATA *st,
                    LEX_CSTRING *bad_type) override;

  void validate_prepare() override;
  bool validate_value(MY_XML_VALIDATION_DATA *st,
                      const char *attr, size_t len) override;
  bool validate_leave(MY_XML_VALIDATION_DATA *st,
                      const char *attr, size_t len) override;
  bool validate_name(const char *attr, size_t len) override
  {
    return m_atr_name.eq_value(attr, len);
  }
};


class XMLSchema_anyAttribute: public XMLSchema_tag
{
public:
  XMLSchema_tag_attribute m_atr_namespace;
  XMLSchema_tag_attribute m_atr_processContents;

  XMLSchema_anyAttribute(): XMLSchema_tag(),
    m_atr_namespace(&xs_namespace),
    m_atr_processContents(&xs_processContents)
  {
    declare_attribute(&m_atr_namespace);
    declare_attribute(&m_atr_processContents);
  }
  bool validate_leave(MY_XML_VALIDATION_DATA *st,
                      const char *attr, size_t len) override
  {
    st->pop();
    return MY_XML_OK;
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
  enum vtn_result validate_tag_name(MY_XML_VALIDATION_DATA *st,
                         const char *attr, size_t len) override
  {
    st->push(&st->annotation);
    return VTN_ACCEPTED;
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
public:
  XMLSchema_std_attributes m_attributes;
  XMLSchema_attributeGroup_def *m_next_group;
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
  bool resolve_type(MY_XML_VALIDATION_DATA *st,
                    LEX_CSTRING *bad_type) override;

  bool validate_attr(MY_XML_VALIDATION_DATA *st,
                     const char *attr, size_t len) override
  {
    if (!m_group)
      return MY_XML_ERROR;

    return m_group->m_attributes.validate_attr(st, attr, len);
  }
};


class XMLSchema_builtin_type: public Sql_alloc
{
protected:
  virtual ~XMLSchema_builtin_type() = default;
public:
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


enum enum_xml_num_states {
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


class XMLSchema_empty_builtin_type: public XMLSchema_builtin_type
{
public:
  bool valid_value(const char *value, size_t len) override
  {
    /* Only allow some whitespaces as a value. */
    size_t pos= 0;

    while (len > pos)
    {
      unsigned int c= ((uchar *) value)[pos++];
      if (c >= array_elements(xml_num_chr_map))
        return 0;

      if (xml_num_chr_map[c] != N_SPC)
        return 0;
    }

    return 1;
  }
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
      unsigned int c= ((uchar *) value)[pos++];
      if (c >= array_elements(xml_num_chr_map))
        return 0;

      state= xml_num_states[state][xml_num_chr_map[c]];
      if (state == E_SYN ||
          xml_num_state_types[state] & m_disallowed_types)
        return 0;
    }

    return xml_num_states[state][N_EOF] == NS_END;
  }
};


enum xml_time_char_classes {
  TC_MNS,
  TC_PLS,
  TC_DIG,
  TC_PNT,
  TC_T,
  TC_Z,
  TC_CLN,
  TC_SPC,
  TC_EOF,
  tc_er,
  TC_NUM_CLASSES
};


static enum xml_time_char_classes xml_time_chr_map[96] = {
  tc_er, tc_er,  tc_er,  tc_er, tc_er, tc_er,  tc_er, tc_er,
  tc_er, TC_SPC, TC_SPC, tc_er, tc_er, TC_SPC, tc_er, tc_er,
  tc_er, tc_er,  tc_er,  tc_er, tc_er, tc_er,  tc_er, tc_er,
  tc_er, tc_er,  tc_er,  tc_er, tc_er, tc_er,  tc_er, tc_er,

  TC_SPC, tc_er,  tc_er,  tc_er,  tc_er,  tc_er,  tc_er,  tc_er, /* !"#$%&'*/
  tc_er,  tc_er,  tc_er,  TC_PLS, tc_er,  TC_MNS, TC_PNT, tc_er, /*()*+,-./ */
  TC_DIG, TC_DIG, TC_DIG, TC_DIG, TC_DIG, TC_DIG, TC_DIG, TC_DIG,/*01234567*/
  TC_DIG, TC_DIG, TC_CLN, tc_er,  tc_er,  tc_er,  tc_er,  tc_er, /*89:;<=>?*/

  tc_er,  tc_er,  tc_er,  tc_er,  tc_er,  tc_er,  tc_er,  tc_er, /*@ABCDEFG*/
  tc_er,  tc_er,  tc_er,  tc_er,  tc_er,  tc_er,  tc_er,  tc_er, /*HIJKLMNO*/
  tc_er,  tc_er,  tc_er,  tc_er,  TC_T,   tc_er,  tc_er,  tc_er, /*PQRSTUVW*/
  tc_er,  tc_er,  TC_Z,   tc_er,  tc_er,  tc_er,  tc_er,  tc_er  /*XYZ[\]^_*/
};


enum enum_xml_time_states {
  /* datetime */
  T_GO,  /* Initial state. */
  T_END, /* Datetime ended. */
  T_YMI, /* If the year starts with '-'. */
  T_Y1,
  T_Y2,
  T_Y3,
  T_Y4,
  T_YE,
  T_M1,
  T_M2,
  T_ME,
  T_D1,
  T_D2,
  T_DE,
  T_H1,
  T_H2,
  T_HE,
  T_MI1,
  T_MI2,
  T_MIE,
  T_S1,
  T_S2,
  T_SFP,
  T_SFR,
  T_ZH0,
  T_ZH1,
  T_ZH2,
  T_ZHE,
  T_ZM1,
  T_ZM2,
  T_Z,

  /* date */
  T_dGO,  /* Initial state. */
  T_dYMI, /* If the year starts with '-'. */
  T_dY1,
  T_dY2,
  T_dY3,
  T_dY4,
  T_dYE,
  T_dM1,
  T_dM2,
  T_dME,
  T_dD1,
  T_dD2,

  /* time */
  T_tGO,

  /* gYear */
  T_yGO,
  T_yYMI,
  T_yY1,
  T_yY2,
  T_yY3,
  T_yY4,

  /* gMonth */
  T_mGO,
  T_mG1,
  T_mG2,
  T_mM1,
  T_mM2,
  T_mE1,
  T_mE2,

  /* gDay */
  T_aGO,
  T_aG1,
  T_aG2,
  T_aG3,
  T_aD1,
  T_aD2,

  /* gYearMonth */
  T_hGO,
  T_hYMI,
  T_hY1,
  T_hY2,
  T_hY3,
  T_hY4,
  T_hM1,
  T_hM2,

  /* gMonthDay */
  T_nGO,
  T_nG1,
  T_nG2,
  T_nM1,
  T_nM2,

  T_NUM_STATES,
  T_SYN   /* Syntax error. */
};


static int xml_time_states[T_NUM_STATES][TC_NUM_CLASSES]= {
/*       -       +     0..9    .       T      Z     :     SPACE  EOF  BAD_SYM*/
/* dateTime */
/*GO*/ {T_YMI, T_SYN, T_Y1,  T_SYN, T_SYN, T_SYN, T_SYN, T_GO,  T_SYN, T_SYN},
/*END*/{T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_END, T_SYN, T_SYN},
/*YMI*/{T_SYN, T_SYN, T_Y1,  T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*Y1*/ {T_SYN, T_SYN, T_Y2,  T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*Y2*/ {T_SYN, T_SYN, T_Y3,  T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*Y3*/ {T_SYN, T_SYN, T_Y4,  T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*Y4*/ {T_YE,  T_SYN, T_Y4,  T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*YE*/ {T_SYN, T_SYN, T_M1,  T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*M1*/ {T_SYN, T_SYN, T_M2,  T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*M2*/ {T_ME,  T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*ME*/ {T_SYN, T_SYN, T_D1,  T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*D1*/ {T_SYN, T_SYN, T_D2,  T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*D2*/ {T_SYN, T_SYN, T_SYN, T_SYN, T_DE,  T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*DE*/ {T_SYN, T_SYN, T_H1,  T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},

/*H1*/ {T_SYN, T_SYN, T_H2,  T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*H2*/ {T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_HE,  T_SYN, T_SYN, T_SYN},
/*HE*/ {T_SYN, T_SYN, T_MI1, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*MI1*/{T_SYN, T_SYN, T_MI2, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*MI2*/{T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_MIE, T_SYN, T_SYN, T_SYN},
/*MIE*/{T_SYN, T_SYN, T_S1,  T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*S1*/ {T_SYN, T_SYN, T_S2,  T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*S2*/ {T_ZH0, T_ZH0, T_SYN, T_SFP, T_SYN, T_Z,   T_SYN, T_END, T_END, T_SYN},
/*SFP*/{T_SYN, T_SYN, T_SFR, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*SFR*/{T_ZH0, T_ZH0, T_SFR, T_SYN, T_SYN, T_Z,   T_SYN, T_END, T_END, T_SYN},
/*ZH0*/{T_SYN, T_SYN, T_ZH1, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*ZH1*/{T_SYN, T_SYN, T_ZH2, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*ZH2*/{T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_ZHE, T_SYN, T_SYN, T_SYN},
/*ZHE*/{T_SYN, T_SYN, T_ZM1, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*ZM1*/{T_SYN, T_SYN, T_ZM2, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*ZM2*/{T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_END, T_END, T_SYN},
/*Z*/  {T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_END, T_END, T_SYN},

/* date */
/*       -       +     0..9    .       T      Z     :     SPACE  EOF  BAD_SYM*/
/*GO*/ {T_dYMI,T_SYN, T_dY1, T_SYN, T_SYN, T_SYN, T_SYN, T_dGO, T_SYN, T_SYN},
/*YMI*/{T_SYN, T_SYN, T_dY1, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*Y1*/ {T_SYN, T_SYN, T_dY2, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*Y2*/ {T_SYN, T_SYN, T_dY3, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*Y3*/ {T_SYN, T_SYN, T_dY4, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*Y4*/ {T_dYE, T_SYN, T_dY4, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*YE*/ {T_SYN, T_SYN, T_dM1, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*M1*/ {T_SYN, T_SYN, T_dM2, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*M2*/ {T_dME, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*ME*/ {T_SYN, T_SYN, T_dD1, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*D1*/ {T_SYN, T_SYN, T_dD2, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*D2*/ {T_ZH0, T_ZH0, T_SYN, T_SYN, T_SYN, T_Z,   T_SYN, T_END, T_END, T_SYN},

/* time */
/*       -       +     0..9    .       T      Z     :     SPACE  EOF  BAD_SYM*/
/*GO*/ {T_SYN, T_SYN, T_H1,  T_SYN, T_SYN, T_SYN, T_SYN, T_tGO, T_SYN, T_SYN},

/* gYear "2026+01:00" */
/*       -       +     0..9    .       T      Z     :     SPACE  EOF  BAD_SYM*/
/*GO*/ {T_yYMI,T_SYN, T_yY1, T_SYN, T_SYN, T_SYN, T_SYN, T_yGO, T_SYN, T_SYN},
/*YMI*/{T_SYN, T_SYN, T_yY1, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*Y1*/ {T_SYN, T_SYN, T_yY2, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*Y2*/ {T_SYN, T_SYN, T_yY3, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*Y3*/ {T_SYN, T_SYN, T_yY4, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*Y4*/ {T_ZH0, T_ZH0, T_yY4, T_SYN, T_SYN, T_Z,   T_SYN, T_END, T_END, T_SYN},

/* gMonth "--02--+01:00" */
/*       -       +     0..9    .       T      Z     :     SPACE  EOF  BAD_SYM*/
/*GO*/ {T_mG1, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_mGO, T_SYN, T_SYN},
/*G1*/ {T_mG2, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*G2*/ {T_SYN, T_SYN, T_mM1, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*M1*/ {T_SYN, T_SYN, T_mM2, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*M2*/ {T_mE1, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*E1*/ {T_mE2, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*E2*/ {T_ZH0, T_ZH0, T_SYN, T_SYN, T_SYN, T_Z,   T_SYN, T_END, T_END, T_SYN},

/* gDay "---10+02:00" */
/*       -       +     0..9    .       T      Z     :     SPACE  EOF  BAD_SYM*/
/*GO*/ {T_aG1, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_aGO, T_SYN, T_SYN},
/*G1*/ {T_aG2, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*G2*/ {T_aG3, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*G3*/ {T_SYN, T_SYN, T_aD1, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*D1*/ {T_SYN, T_SYN, T_aD2, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*D2*/ {T_ZH0, T_ZH0, T_SYN, T_SYN, T_SYN, T_Z,   T_SYN, T_END, T_END, T_SYN},

/* gYearMonth "2026-10+02:00" */
/*       -       +     0..9    .       T      Z     :     SPACE  EOF  BAD_SYM*/
/*GO*/ {T_hYMI,T_SYN, T_hY1, T_SYN, T_SYN, T_SYN, T_SYN, T_hGO, T_SYN, T_SYN},
/*YMI*/{T_SYN, T_SYN, T_hY1, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*Y1*/ {T_SYN, T_SYN, T_hY2, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*Y2*/ {T_SYN, T_SYN, T_hY3, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*Y3*/ {T_SYN, T_SYN, T_hY4, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*Y4*/ {T_hM1, T_SYN, T_hY4, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*M1*/ {T_SYN, T_SYN, T_hM2, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*M2*/ {T_ZH0, T_ZH0, T_SYN, T_SYN, T_SYN, T_Z,   T_SYN, T_END, T_END, T_SYN},

/* gMonthDay "--05-02+01:00" */
/*       -       +     0..9    .       T      Z     :     SPACE  EOF  BAD_SYM*/
/*GO*/ {T_nG1, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_nGO, T_SYN, T_SYN},
/*G1*/ {T_nG2, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*G2*/ {T_SYN, T_SYN, T_nM1, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*M1*/ {T_SYN, T_SYN, T_nM2, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
/*M2*/ {T_aG3, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN, T_SYN},
};


class XMLSchema_datetime_builtin_type: public XMLSchema_builtin_type
{
public:
  enum enum_xml_time_states m_state0;
  bool m_spaces_allowed;

  XMLSchema_datetime_builtin_type(
      enum enum_xml_time_states state0, bool spaces_allowed= TRUE):
    XMLSchema_builtin_type(),
    m_state0(state0), m_spaces_allowed(spaces_allowed) {}

  bool valid_value(const char *value, size_t len) override
  {
    int state= m_state0;

    size_t pos= 0;

    while (len > pos)
    {
      unsigned int c= ((uchar *) value)[pos++];
      if (c >= array_elements(xml_time_chr_map))
        return 0;

      state= xml_time_states[state][xml_time_chr_map[c]];
      if (state == T_SYN)
        return 0;
    }

    return xml_time_states[state][TC_EOF] == T_END;
  }
};


enum xml_bin_char_classes {
  BC_HEX,
  BC_B64,
  BC_EQ,
  BC_SPC,
  BC_EOF,
  bc_er,
  BC_NUM_CLASSES
};


static enum xml_bin_char_classes xml_bin_chr_map[128]=
{
   bc_er, bc_er, bc_er, bc_er, bc_er, bc_er, bc_er, bc_er,
   bc_er,BC_SPC,BC_SPC, bc_er, bc_er,BC_SPC, bc_er, bc_er,
   bc_er, bc_er, bc_er, bc_er, bc_er, bc_er, bc_er, bc_er,
   bc_er, bc_er, bc_er, bc_er, bc_er, bc_er, bc_er, bc_er,
  BC_SPC, bc_er, bc_er, bc_er, bc_er, bc_er, bc_er, bc_er, /*  !"#$%&' */
   bc_er, bc_er, bc_er,BC_B64, bc_er, bc_er, bc_er,BC_B64, /* ()*+,-./ */
  BC_HEX,BC_HEX,BC_HEX,BC_HEX,BC_HEX,BC_HEX,BC_HEX,BC_HEX, /* 01234567 */
  BC_HEX,BC_HEX, bc_er, bc_er, bc_er, BC_EQ, bc_er, bc_er, /* 89:;<=>? */
   bc_er,BC_HEX,BC_HEX,BC_HEX,BC_HEX,BC_HEX,BC_HEX,BC_B64, /* @ABCDEFG */
  BC_B64,BC_B64,BC_B64,BC_B64,BC_B64,BC_B64,BC_B64,BC_B64, /* HIJKLMNO */
  BC_B64,BC_B64,BC_B64,BC_B64,BC_B64,BC_B64,BC_B64,BC_B64, /* PQRSTUVW */
  BC_B64,BC_B64,BC_B64, bc_er, bc_er, bc_er, bc_er, bc_er, /* XYZ[\]^_ */
   bc_er,BC_HEX,BC_HEX,BC_HEX,BC_HEX,BC_HEX,BC_HEX,BC_B64, /* `abcdefg */
  BC_B64,BC_B64,BC_B64,BC_B64,BC_B64,BC_B64,BC_B64,BC_B64, /* hijklmno */
  BC_B64,BC_B64,BC_B64,BC_B64,BC_B64,BC_B64,BC_B64,BC_B64, /* pqrstuvwxyz{|}~  */
  BC_B64,BC_B64,BC_B64, bc_er, bc_er, bc_er, bc_er, bc_er  /* pqrstuvwxyz{|}~  */
};


enum enum_xml_bin_states {
  B_END,
  B_HEX0,
  B_HEX1,
  B_B640,
  B_B641,
  B_B642,
  B_B643,
  B_NUM_STATES,
  B_SYN   /* Syntax error. */
};


static int xml_bin_states[B_NUM_STATES][BC_NUM_CLASSES]= {
/*          HEX     B64     =      SPACE   EOF    BAD_SYM */
/*END*/  { B_SYN,  B_SYN,  B_SYN,  B_END,  B_END,  B_SYN },
/*HEX0*/ { B_HEX1, B_SYN,  B_SYN,  B_HEX0, B_END,  B_SYN },
/*HEX1*/ { B_HEX0, B_SYN,  B_SYN,  B_HEX1, B_SYN,  B_SYN },
/*B640*/ { B_B641, B_B641, B_SYN,  B_B640, B_END,  B_SYN },
/*B641*/ { B_B642, B_B642, B_SYN,  B_B641, B_SYN,  B_SYN },
/*B642*/ { B_B643, B_B643, B_SYN,  B_B642, B_SYN,  B_SYN },
/*B643*/ { B_B640, B_B640, B_END,  B_B643, B_SYN,  B_SYN }
};


class XMLSchema_binary_builtin_type: public XMLSchema_builtin_type
{
public:
  enum enum_xml_bin_states m_state0;

  XMLSchema_binary_builtin_type(enum enum_xml_bin_states state0):
    XMLSchema_builtin_type(), m_state0(state0) {}

  bool valid_value(const char *value, size_t len) override
  {
    int state= m_state0;

    size_t pos= 0;

    while (len > pos)
    {
      unsigned int c= ((uchar *) value)[pos++];
      if (c >= array_elements(xml_bin_chr_map))
        return 0;

      state= xml_bin_states[state][xml_bin_chr_map[c]];
      if (state == B_SYN)
        return 0;
    }

    return xml_bin_states[state][BC_EOF] == B_END;
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
  bool validate_attr(MY_XML_VALIDATION_DATA *st,
                     const char *attr, size_t len) override
  {
    m_level++;
    return MY_XML_OK;
  }
  enum vtn_result validate_tag_name(MY_XML_VALIDATION_DATA *st,
                    const char *attr, size_t len) override
  {
    st->push(&st->annotation);
    return VTN_ACCEPTED;
  }
  bool is_validate_done() override
  {
    return true;
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
                             const char *attr, size_t len) override
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
  bool leave(MY_XML_VALIDATION_DATA *st,
            const char *attr, size_t len) override;
  bool validate_name(const char *attr, size_t len) override
  {
    return m_type_name.eq_value(attr, len);
  }
  void validate_prepare() override
  {
    m_compositor->validate_prepare();
  }
  bool is_validate_done() override
  {
    return m_compositor->is_validate_done();
  }
  bool validate_value(MY_XML_VALIDATION_DATA *st,
                    const char *attr, size_t len) override
  {
    return m_compositor->validate_value(st, attr, len);
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
    if (m_attributes.validate_attr(st, attr, len) == MY_XML_OK ||
        m_compositor->validate_attr(st, attr, len) == MY_XML_OK)
      return MY_XML_OK;
    return MY_XML_ERROR;
  }
  enum vtn_result validate_tag_name(MY_XML_VALIDATION_DATA *st,
                         const char *attr, size_t len) override
  {
    return m_compositor->validate_tag_name(st, attr, len);
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
  bool resolve_type(MY_XML_VALIDATION_DATA *st,
                    LEX_CSTRING *bad_type) override;
  void validate_prepare() override
  {
    m_base_type->validate_prepare();
  }
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
  bool validate_attr(MY_XML_VALIDATION_DATA *st,
                     const char *attr, size_t len) override
  {
    return m_attributes.validate_attr(st, attr, len);
  }
};


/* TODO implement and enable the complexContext
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
*/


class XMLSchema_extension_in_simpleContent: public XMLSchema_tag
{
  XMLSchema_tag_attribute m_base;
  XMLSchema_std_attributes m_attributes;
public:
  XMLSchema_type *m_base_type;
  XMLSchema_extension_in_simpleContent(): XMLSchema_tag(),
    m_base(&xs_base), m_base_type(NULL)
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
  bool leave(MY_XML_VALIDATION_DATA *st,
            const char *attr, size_t len) override;
  bool resolve_type(MY_XML_VALIDATION_DATA *st,
                    LEX_CSTRING *bad_type) override;

  void validate_prepare() override
  {
    m_base_type->validate_prepare();
  }

  bool validate_attr(MY_XML_VALIDATION_DATA *st,
                     const char *attr, size_t len) override
  {
    return m_attributes.validate_attr(st, attr, len);
  }
  bool validate_value(MY_XML_VALIDATION_DATA *st,
                      const char *attr, size_t len) override;
};


/* TODO implement and enable the complexContext
class XMLSchema_extension_in_complexContent:
  public XMLSchema_restriction_in_complexContent
{
};
*/


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
  bool resolve_type(MY_XML_VALIDATION_DATA *st,
                    LEX_CSTRING *bad_type) override;
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
  void validate_prepare() override { m_nested->validate_prepare(); }
  bool validate_attr(MY_XML_VALIDATION_DATA *st,
                     const char *attr, size_t len) override
  {
    return m_nested->validate_attr(st, attr, len);
  }
  bool validate_value(MY_XML_VALIDATION_DATA *st,
                      const char *attr, size_t len) override
  {
    return m_nested->validate_value(st, attr, len);
  }
};


/* TODO implement and enable the complexContext
class XMLSchema_complexContent: public XMLSchema_simpleContent
{
  XMLSchema_tag_attribute m_atr_mixed;
  XMLSchema_tag *m_nested;  //  extension or restriction

public:
  XMLSchema_complexContent(): XMLSchema_simpleContent(),
    m_atr_mixed(&xs_mixed)
  {
    declare_attribute(&m_atr_mixed);
  }
  bool enter_tag(MY_XML_VALIDATION_DATA *st,
                 const char *attr, size_t len) override;
};
*/


class XMLSchema_element_global;

class XMLSchema_all: public XMLSchema_tag
{
protected:
  XMLSchema_tag_integer_attribute           m_minOccurs;
  XMLSchema_tag_unbounded_integer_attribute m_maxOccurs;
  XMLSchema_tag **m_tags_hook;

public:
  int m_counter;
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
    m_counter= 0;
    for (XMLSchema_tag *cur= m_tags; cur; cur= cur->m_next_tag)
      cur->validate_prepare();
  }

  enum vtn_result validate_tag_name(MY_XML_VALIDATION_DATA *st,
                    const char *attr, size_t len) override
  {
    enum vtn_result tn_result;
    for (XMLSchema_tag *cur= m_tags; cur; cur= cur->m_next_tag)
    {
      if ((tn_result= cur->validate_tag_name(st, attr, len)) != VTN_CONTINUE)
      {
        if (tn_result == VTN_ACCEPTED)
          m_counter= 1;
        return tn_result;
      }
    }
    return is_validate_done() ? VTN_CONTINUE : VTN_ERROR;
  }
  bool is_validate_done() override
  {
    if (m_counter == 0)
    {
      return m_minOccurs.m_value_int == 0;
    }

    for (XMLSchema_tag *cur= m_tags; cur; cur= cur->m_next_tag)
    {
      if (!cur->is_validate_done())
      {
        return false;
      }
    }
    return true;
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
    m_counter= 0;
  }
  enum vtn_result validate_tag_name(MY_XML_VALIDATION_DATA *st,
                    const char *attr, size_t len) override
  {
    bool full_loop= false;
    bool beginning= false;
    enum vtn_result tn_result;

    if (!m_cur_tag)
    {
      if (!m_tags)
        return VTN_CONTINUE;
turn_over:
      if (m_counter >= m_maxOccurs.m_value_int)
        return VTN_CONTINUE;

      m_cur_tag= m_tags;
      m_counter++;
      beginning= true;
      m_cur_tag->validate_prepare();
    }
    
    while ((tn_result= m_cur_tag->validate_tag_name(st, attr, len)) ==
           VTN_CONTINUE)
    {
      if (!m_cur_tag->is_validate_done())
      {
        /*
           If we already applied some elements of the sequence,
           then we have to break the validation with the error.
           Otherwise we still can try other rules.
        */
        return beginning ? VTN_CONTINUE : VTN_ERROR;
      }

      m_cur_tag= m_cur_tag->m_next_tag;
      if (!m_cur_tag)
      {
        if (full_loop)
        {
          /*
            Weird case when all elements are optional but
            maxOccurs set to "unbound". Cut the endless loop.
          */
          return VTN_CONTINUE;
        }
        full_loop= true;
        goto turn_over;
      }
      m_cur_tag->validate_prepare();
    }

    return tn_result;
  }
  bool is_validate_done() override
  {
    if (m_counter < m_minOccurs.m_value_int)
      return false;
    while (m_cur_tag)
    {
      if (!m_cur_tag->is_validate_done())
        return false;
      m_cur_tag= m_cur_tag->m_next_tag;
      if (m_cur_tag)
        m_cur_tag->validate_prepare();

    }

    return true;
  }
};


class XMLSchema_choice: public XMLSchema_sequence
{
  XMLSchema_tag *m_found;
public:
  void validate_prepare() override
  {
    m_counter= 0;
    m_found= NULL;
    for (XMLSchema_tag *cur= m_tags; cur; cur= cur->m_next_tag)
    {
      cur->validate_prepare();
    }
  }
  enum vtn_result validate_tag_name(MY_XML_VALIDATION_DATA *st,
                    const char *attr, size_t len) override
  {
    enum vtn_result tn_result;

    if (m_found)
    {
      if ((tn_result= m_found->validate_tag_name(st, attr, len)) ==
          VTN_CONTINUE)
      {
        if (!m_found->is_validate_done())
          return VTN_ERROR;
        m_found= NULL;
      }
      else
        return tn_result;
    }

    if (m_counter >= m_maxOccurs.m_value_int)
      return VTN_CONTINUE;


    for (XMLSchema_tag *cur= m_tags; cur; cur= cur->m_next_tag)
    {
      if ((tn_result= cur->validate_tag_name(st, attr, len)) == VTN_ACCEPTED)
      {
        m_found= cur;
        m_counter++;
        return VTN_ACCEPTED;
      }

      if (tn_result == VTN_ERROR)
        return VTN_ERROR;
    }

    return VTN_CONTINUE;
  }
  bool is_validate_done() override
  {
    if (m_found)
    {
      if (!m_found->is_validate_done())
        return false;
    }
    return m_counter >= m_minOccurs.m_value_int;
  }
};


class XMLSchema_group_def: public XMLSchema_tag
{
public:
  XMLSchema_tag *m_compositor;
  XMLSchema_tag_attribute m_atr_name;
  XMLSchema_group_def *m_next_group;

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
    st->add_item_to_resolve(this);
    return XMLSchema_tag::leave(st, attr, len);
  }
  bool resolve_type(MY_XML_VALIDATION_DATA *st,
                    LEX_CSTRING *bad_type) override;

  void validate_prepare() override
  {
    m_group->m_compositor->validate_prepare();
  }
  enum vtn_result validate_tag_name(MY_XML_VALIDATION_DATA *st,
                    const char *attr, size_t len) override
  {
    return m_group->m_compositor->validate_tag_name(st, attr, len);
  }
  bool is_validate_done() override
  {
    return m_group->m_compositor->is_validate_done();
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
  bool resolve_type(MY_XML_VALIDATION_DATA *st,
                    LEX_CSTRING *bad_type) override;
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
  enum vtn_result validate_tag_name(MY_XML_VALIDATION_DATA *st,
                    const char *attr, size_t len) override
  {
    if (m_counter >= m_atr_maxOccurs.m_value_int)
      return VTN_CONTINUE;

    if (m_atr_name.eq_value(attr, len))
    {
      m_counter++;
      m_type->validate_prepare();
      st->push(this);
      return VTN_ACCEPTED;
    }

    return VTN_CONTINUE;
  }
  bool is_validate_done() override
  {
    return m_counter >= m_atr_minOccurs.m_value_int;
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
  XMLSchema_tag_xmlns_attribute m_atr_xmlns;

public:
  XMLSchema_user_type *m_global_simpleTypes;
  XMLSchema_user_type *m_global_complexTypes;
  XMLSchema_attribute *m_global_attributes;
  XMLSchema_element_global *m_global_elements;
  XMLSchema_attributeGroup_def *m_global_attr_groups;
  XMLSchema_group_def *m_global_element_groups;

  List<XMLSchema_item> m_items_to_resolve;

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
    m_global_attr_groups(NULL),
    m_global_element_groups(NULL)
  {
    declare_attribute(&m_atr_elementFormDefault);
    declare_attribute(&m_atr_attributeFormDefault);
    declare_attribute(&m_atr_targetNamespace);
    declare_attribute(&m_atr_version);
    declare_attribute(&m_atr_xml_lang);
  }

  XMLSchema_type *find_simple_type_by_name(MY_XML_VALIDATION_DATA *st,
                                           const char *name, size_t len) const;
  XMLSchema_type *find_type_by_name(MY_XML_VALIDATION_DATA *st,
                                    const char *name, size_t len) const;
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
  bool enter_attr(MY_XML_VALIDATION_DATA *st,
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
static xs_word xs_gMonth(STRING_WITH_LEN("gMonth"));
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


static XMLSchema_tag empty_compositor;

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

  /* date/time types */
  if (xs_dateTime.eq(name, len))
    return new(st->mem_root) XMLSchema_datetime_builtin_type(T_GO);

  if (xs_date.eq(name, len))
    return new(st->mem_root) XMLSchema_datetime_builtin_type(T_dGO);

  if (xs_time.eq(name, len))
    return new(st->mem_root) XMLSchema_datetime_builtin_type(T_tGO);

  if (xs_gYear.eq(name, len))
    return new(st->mem_root) XMLSchema_datetime_builtin_type(T_yGO);

  if (xs_gMonth.eq(name, len))
    return new(st->mem_root) XMLSchema_datetime_builtin_type(T_mGO);

  if (xs_gDay.eq(name, len))
    return new(st->mem_root) XMLSchema_datetime_builtin_type(T_aGO);

  if (xs_gMonthDay.eq(name, len))
    return new(st->mem_root) XMLSchema_datetime_builtin_type(T_nGO);

  if (xs_base64Binary.eq(name, len))
    return new(st->mem_root) XMLSchema_binary_builtin_type(B_B640);

  if (xs_hexBinary.eq(name, len))
    return new(st->mem_root) XMLSchema_binary_builtin_type(B_HEX0);

  /* various types */
  if (xs_anySimpleType.eq(name, len))
    return new(st->mem_root) XMLSchema_builtin_type;

  return NULL;
}


void MY_XML_VALIDATION_DATA::add_item_to_resolve(XMLSchema_item *t)
{
  schema->m_items_to_resolve.push_back(t, mem_root);
}


bool MY_XML_VALIDATION_DATA::xs_namespace(const char **name, size_t *len) const
{
  if (*len > schema_namespace.length &&
      memcmp(*name, schema_namespace.str, schema_namespace.length) == 0)
  {
    (*name)+= schema_namespace.length;
    (*len)-= schema_namespace.length;
    return true;
  }

  return false;
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
  return st->schema->validate_element(st, attr, len);
}


XMLSchema_tag_xmlns_attribute::XMLSchema_tag_xmlns_attribute():
  XMLSchema_tag_attribute(&xs_xmlns)
{}


bool XMLSchema_tag_xmlns_attribute::value(
         MY_XML_VALIDATION_DATA *st, const char *attr, size_t len)
{
  m_val= attr;
  m_val_len= len;

  if ((len >= xs_uri_short.length() &&
       memcmp(xs_uri_short.str(), attr, xs_uri_short.length()) == 0) ||
      (len >= xs_uri_www.length() &&
       memcmp(xs_uri_www.str(), attr, xs_uri_www.length()) == 0))
  {
    if (st->set_schema_namespace(&m_ns_name))
      return MY_XML_ERROR;
  }

  return MY_XML_OK;
}


bool XMLSchema_user_type::leave(MY_XML_VALIDATION_DATA *st,
                                const char *attr, size_t len)
{
  if (!m_compositor)
    m_compositor= &empty_compositor;
  return XMLSchema_type::leave(st, attr, len);
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

  if (m_anyAttribute)
  {
    m_anyAttribute->push_self(st);
    return MY_XML_OK;
  }
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
    st->add_item_to_resolve(this);
  }

  return XMLSchema_item::leave(st, attr, len);
}


bool XMLSchema_attribute::resolve_type(
                            MY_XML_VALIDATION_DATA *st, LEX_CSTRING *bad_type)
{
  m_type= st->schema->find_simple_type_by_name(
                        st, m_atr_type.m_val, m_atr_type.m_val_len);
  if (m_type)
    return MY_XML_OK;

  bad_type->str= m_atr_type.m_val;
  bad_type->length= m_atr_type.m_val_len;
  return MY_XML_ERROR;
}


bool XMLSchema_attributeGroup_reference::leave(MY_XML_VALIDATION_DATA *st,
        const char *attr, size_t len)
{
  if (!m_atr_ref.is_set())
    return MY_XML_ERROR;

  st->add_item_to_resolve(this);

  return XMLSchema_tag::leave(st, attr, len);
}


bool XMLSchema_attributeGroup_reference::resolve_type(
            MY_XML_VALIDATION_DATA *st, LEX_CSTRING *bad_type)
{
  m_group= st->find_attribute_group_by_name(
                 m_atr_ref.m_val, m_atr_ref.m_val_len);
  if (m_group)
    return MY_XML_OK;
  bad_type->str= m_atr_ref.m_val;
  bad_type->length= m_atr_ref.m_val_len;
  return MY_XML_ERROR;
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
/* TODO implement and enable the complexContext
  else if (xs_complexContent.eq(attr, len))
  {
    def= new(st->mem_root) XMLSchema_complexContent;
  }
*/
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
  else if (xs_minExclusive.eq(attr, len))
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

    st->add_item_to_resolve(this);
  }
  else
  {
    if (!m_base_type)
      return MY_XML_ERROR; /* no type specified. */
  }

  return XMLSchema_tag::leave(st, attr, len);
}


bool XMLSchema_restriction_in_simpleType::resolve_type(
    MY_XML_VALIDATION_DATA *st, LEX_CSTRING *bad_type)
{
  m_base_type= st->schema->find_type_by_name(
                               st, m_base.m_val, m_base.m_val_len);
  if (m_base_type)
    return MY_XML_OK;

  bad_type->str= m_base.m_val;
  bad_type->length= m_base.m_val_len;
  return MY_XML_ERROR;
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
  return m_base_type->validate_value(st, attr, len);
}


bool XMLSchema_extension_in_simpleContent::leave(
    MY_XML_VALIDATION_DATA *st, const char *attr, size_t len)
{
  if (m_base.is_set())
  {
    if (m_base_type)
      return MY_XML_ERROR; /* type should be specified only once. */

    st->add_item_to_resolve(this);
  }
  else
  {
    if (!m_base_type)
      return MY_XML_ERROR; /* no type specified. */
  }

  return XMLSchema_tag::leave(st, attr, len);
}


bool XMLSchema_extension_in_simpleContent::resolve_type(
    MY_XML_VALIDATION_DATA *st, LEX_CSTRING *bad_type)
{
  m_base_type= st->schema->find_type_by_name(
                               st, m_base.m_val, m_base.m_val_len);
  if (m_base_type)
    return MY_XML_OK;

  bad_type->str= m_base.m_val;
  bad_type->length= m_base.m_val_len;
  return MY_XML_ERROR;
}


bool XMLSchema_extension_in_simpleContent::validate_value(
            MY_XML_VALIDATION_DATA *st, const char *attr, size_t len)
{
  return m_base_type->validate_value(st, attr, len);
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

  st->add_item_to_resolve(this);
  return res;
}


bool XMLSchema_list::resolve_type(
                       MY_XML_VALIDATION_DATA *st, LEX_CSTRING *bad_type)
{
  m_type= st->schema->find_type_by_name(st,
                        m_attr_itemType.m_val, m_attr_itemType.m_val_len);

  if (m_type)
    return MY_XML_OK;

  bad_type->str= m_attr_itemType.m_val;
  bad_type->length= m_attr_itemType.m_val_len;
  return MY_XML_ERROR;
}


bool XMLSchema_union::enter_tag(MY_XML_VALIDATION_DATA *st,
                                const char *attr, size_t len)
{
  if (xs_simpleType.eq(attr, len))
  {
    XMLSchema_simpleType *t= new(st->mem_root) XMLSchema_simpleType;
    st->push(t);
    *m_nested_hook= t;
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

  if (m_nested_types)
    return m_attr_memberTypes.is_set() ? MY_XML_ERROR : res;

  return m_attr_memberTypes.is_set() ? res : MY_XML_ERROR;
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


/* TODO implement and enable the complexContext
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
    return MY_XML_ERROR; //

  st->push(t);
  m_nested= t;
  return MY_XML_OK;
}
*/


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


bool XMLSchema_group_reference::resolve_type(MY_XML_VALIDATION_DATA *st,
                                             LEX_CSTRING *bad_type)
{
  m_group= st->find_element_group_by_name( m_ref.m_val, m_ref.m_val_len);
  if (m_group)
    return MY_XML_OK;
  bad_type->str= m_ref.m_val;
  bad_type->length= m_ref.m_val_len;
  return MY_XML_ERROR;
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


bool XMLSchema_element_global::resolve_type(
                            MY_XML_VALIDATION_DATA *st, LEX_CSTRING *bad_type)
{
  if (!m_atr_type.is_set())
  {
    XMLSchema_builtin_type *builtin_type;
    builtin_type= new(st->mem_root) XMLSchema_empty_builtin_type();
    m_type= new(st->mem_root) XMLSchema_simple_builtin_type(builtin_type);
  }
  else
  {
    m_type= st->schema->find_type_by_name(
                          st, m_atr_type.m_val, m_atr_type.m_val_len);
  }
  if (m_type)
    return MY_XML_OK;

  bad_type->str= m_atr_type.m_val;
  bad_type->length= m_atr_type.m_val_len;
  return MY_XML_ERROR;
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
  if (m_type->validate_tag_name(st, attr, len) != VTN_ACCEPTED)
    return validate_failed(st);

  return MY_XML_OK;
}


bool XMLSchema_element_global::validate_leave(MY_XML_VALIDATION_DATA *st,
                                              const char *attr, size_t len)
{
  st->pop();
  return m_type->is_validate_done() ? MY_XML_OK : MY_XML_ERROR;
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
    XMLSchema_group_def *g= new(st->mem_root) XMLSchema_group_def;
    g->m_next_group= m_global_element_groups;
    m_global_element_groups= g;
    def= g;
  }
  else if (xs_attributeGroup.eq(attr, len))
  {
    XMLSchema_attributeGroup_def *g=
      new(st->mem_root) XMLSchema_attributeGroup_def;
    g->m_next_group= m_global_attr_groups;
    m_global_attr_groups= g;
    def= g;
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


bool XMLSchema_schema::enter_attr(MY_XML_VALIDATION_DATA *st,
                                  const char *attr, size_t len)
{
  if (len >= xs_xmlns.length() &&
      memcmp(attr, xs_xmlns.str(), xs_xmlns.length()) == 0)
  {
    /* set namespace for the schema */
    if (len == xs_xmlns.length())
    {
      /* empty namespace name. */
      m_atr_xmlns.m_ns_name.str= "";
      m_atr_xmlns.m_ns_name.length= 0;
    }
    else if (attr[xs_xmlns.length()] == MY_XPATH_LEX_COLON)
    {
      m_atr_xmlns.m_ns_name.str= attr + xs_xmlns.length()+1;
      m_atr_xmlns.m_ns_name.length= len - (xs_xmlns.length()+1);
    }
    else
      goto run_inherited;

    st->push(&m_atr_xmlns);
    return MY_XML_OK;
  }
run_inherited:
  return XMLSchema_tag::enter_attr(st, attr, len);
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


XMLSchema_type *XMLSchema_schema::find_simple_type_by_name(
   MY_XML_VALIDATION_DATA *st, const char *name, size_t len) const
{
  XMLSchema_type *result= NULL;

  if (st->xs_namespace(&name, &len))
  {
    XMLSchema_builtin_type *builtin_type;
    builtin_type= XMLSchema_builtin_type::get_builtin_type_by_name(
                    st, name, len);

    if (builtin_type)
    {
      result= new(st->mem_root) XMLSchema_simple_builtin_type(builtin_type);
    }

    if (xs_anyType.eq(name, len))
      result= new(st->mem_root) XMLSchema_any_type;

    if (result || !st->namespace_empty())
      goto exit;
  }

  result= find_simple_type(name, len);

exit:
  return result;
}


XMLSchema_type *XMLSchema_schema::find_type_by_name(
   MY_XML_VALIDATION_DATA *st, const char *name, size_t len) const
{
  XMLSchema_type *result;

  if ((result= find_simple_type_by_name(st, name, len)))
    return result;

  result= find_complex_type(name, len);

  return result;
}


XMLSchema_attributeGroup_def *
MY_XML_VALIDATION_DATA::find_attribute_group_by_name(
                              const char *name, size_t len) const
{
  for (XMLSchema_attributeGroup_def *g=schema->m_global_attr_groups;
       g; g=g->m_next_group)
  {
    if (g->m_atr_name.eq_value(name, len))
      return g;
  }
  return NULL;
}


XMLSchema_group_def *MY_XML_VALIDATION_DATA::find_element_group_by_name(
                                        const char *name, size_t len) const
{
  for (XMLSchema_group_def *g=schema->m_global_element_groups;
       g; g=g->m_next_group)
  {
    if (g->m_atr_name.eq_value(name, len))
      return g;
  }
  return NULL;
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
    if (data->namespace_not_specified() && !xs_xml.eq(attr, len))
    {
      size_t col_pos= 0;
      data->schema_namespace.str= "";
      data->schema_namespace.length= 0;

      while (col_pos < len)
      {
        if (attr[col_pos++] == MY_XPATH_LEX_COLON)
        {
          data->schema_namespace.str= attr;
          data->schema_namespace.length= col_pos;

          attr+= col_pos;
          len-= col_pos;
          break;
        }
      }
    }
    else if (!data->xs_namespace(&attr, &len))
    {
      /* all the schema tags must be in same schema namespace. */
      return MY_XML_ERROR;
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
  rc= my_xml_parse(&p, xml->ptr(), xml->length()) != MY_XML_OK;

  if (rc)
  {
    char buf[128];
    my_snprintf(buf, sizeof(buf)-1,
                "XML Schema parse error at line %d pos %lu: %s",
                my_xml_error_lineno(&p) + 1,
                (ulong) my_xml_error_pos(&p) + 1,
                my_xml_error_string(&p));
    my_printf_error(ER_WRONG_VALUE, ER_THD(thd, ER_WRONG_VALUE), MYF(0),
                    "XML", buf);
    goto exit;
  }

  if (user_data->schema == NULL)
  {
    my_printf_error(ER_UNKNOWN_ERROR, "Invalid XML Schema.", MYF(0));
    rc= 1;
    goto exit;
  }

  /* resolve typenames */
  {
    List_iterator_fast<XMLSchema_item> li(
        user_data->schema->m_items_to_resolve);
    LEX_CSTRING bad_type;
    XMLSchema_item *i;

    while ((i= li++))
    {
      if (i->resolve_type(user_data, &bad_type))
      {
        my_printf_error(ER_UNKNOWN_ERROR,
          "Invalid XML schema, type %.*s not defined.", MYF(0),
          (int) bad_type.length, bad_type.str);
        rc= 1;
        goto exit;
      }
    }
  }

exit:
  my_xml_parser_free(&p);
  return rc;
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
  int rc;

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
  rc= my_xml_parse(&p, xml->ptr(), xml->length()) == MY_XML_OK;
  my_xml_parser_free(&p);

  return rc;
}


bool Item_func_xml_isvalid::val_bool()
{
  String *xml= args[0]->val_str(&m_tmp_xml);

  if ((null_value= !xml || m_data == NULL))
    return FALSE;

  return validate_schema(xml, m_data);
}
