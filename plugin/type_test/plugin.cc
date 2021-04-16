/*
   Copyright (c) 2019, MariaDB Corporation

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

#include <my_global.h>
#include <sql_class.h>          // THD
#include <mysql/plugin_data_type.h>
#include "sql_type.h"


class Type_collection_test: public Type_collection
{
protected:
  const Type_handler *aggregate_common(const Type_handler *h1,
                                       const Type_handler *h2) const;
public:
  const Type_handler *handler_by_name(const LEX_CSTRING &name) const override
  {
    return NULL;
  }
  const Type_handler *aggregate_for_result(const Type_handler *h1,
                                           const Type_handler *h2)
                                           const override;
  const Type_handler *aggregate_for_comparison(const Type_handler *h1,
                                               const Type_handler *h2)
                                               const override;
  const Type_handler *aggregate_for_min_max(const Type_handler *h1,
                                            const Type_handler *h2)
                                            const override;
  const Type_handler *aggregate_for_num_op(const Type_handler *h1,
                                           const Type_handler *h2)
                                           const override;
};


static Type_collection_test type_collection_test;


class Field_test_int8 :public Field_longlong
{
public:
  Field_test_int8(const LEX_CSTRING &name, const Record_addr &addr,
                  enum utype unireg_check_arg,
                  uint32 len_arg, bool zero_arg, bool unsigned_arg)
    :Field_longlong(addr.ptr(), len_arg, addr.null_ptr(), addr.null_bit(),
                    Field::NONE, &name, zero_arg, unsigned_arg)
  {}
  const Type_handler *type_handler() const override;
};


class Type_handler_test_int8: public Type_handler_longlong
{
public:
  const Type_collection *type_collection() const override
  {
    return &type_collection_test;
  }
  const Type_handler *type_handler_signed() const override
  {
    return this;
  }
  Field *make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &rec, const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const override
  {
    return new (root)
      Field_test_int8(*name, rec, attr->unireg_check,
                      (uint32) attr->length,
                      f_is_zerofill(attr->pack_flag) != 0,
                      f_is_dec(attr->pack_flag) == 0);
  }
};

static Type_handler_test_int8 type_handler_test_int8;


const Type_handler *Field_test_int8::type_handler() const
{
  return &type_handler_test_int8;
}


static struct st_mariadb_data_type plugin_descriptor_type_test_int8=
{
  MariaDB_DATA_TYPE_INTERFACE_VERSION,
  &type_handler_test_int8
};


/*************************************************************************/

class Field_test_double :public Field_double
{
public:
  Field_test_double(const LEX_CSTRING &name, const Record_addr &addr,
                    enum utype unireg_check_arg,
                    uint32 len_arg, uint8 dec_arg,
                    bool zero_arg, bool unsigned_arg)
    :Field_double(addr.ptr(), len_arg, addr.null_ptr(), addr.null_bit(),
                  Field::NONE, &name, dec_arg, zero_arg, unsigned_arg)
  {}
  const Type_handler *type_handler() const override;
};


class Type_handler_test_double: public Type_handler_double
{
public:
  const Type_collection *type_collection() const override
  {
    return &type_collection_test;
  }
  const Type_handler *type_handler_signed() const override
  {
    return this;
  }
  bool Column_definition_data_type_info_image(Binary_string *to,
                                              const Column_definition &def)
                                              const override
  {
    return to->append(Type_handler_test_double::name().lex_cstring());
  }
  Field *make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &rec, const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const override
  {
    return new (root)
      Field_test_double(*name, rec, attr->unireg_check,
                        (uint32) attr->length, (uint8) attr->decimals,
                        f_is_zerofill(attr->pack_flag) != 0,
                        f_is_dec(attr->pack_flag) == 0);
  }
};

static Type_handler_test_double type_handler_test_double;


const Type_handler *Field_test_double::type_handler() const
{
  return &type_handler_test_double;
}


static struct st_mariadb_data_type plugin_descriptor_type_test_double=
{
  MariaDB_DATA_TYPE_INTERFACE_VERSION,
  &type_handler_test_double
};


/*************************************************************************/
const Type_handler *
Type_collection_test::aggregate_common(const Type_handler *h1,
                                       const Type_handler *h2) const
{
  if (h1 == h2)
    return h1;

  static const Type_aggregator::Pair agg[]=
  {
    {
      &type_handler_slong,
      &type_handler_test_double,
      &type_handler_test_double
    },
    {
      &type_handler_newdecimal,
      &type_handler_test_double,
      &type_handler_test_double
    },
    {
      &type_handler_double,
      &type_handler_test_double,
      &type_handler_test_double
    },
    {
      &type_handler_slong,
      &type_handler_test_int8,
      &type_handler_test_int8
    },
    {
      &type_handler_newdecimal,
      &type_handler_test_int8,
      &type_handler_newdecimal
    },
    {
      &type_handler_double,
      &type_handler_test_int8,
      &type_handler_double
    },
    {
      &type_handler_stiny,
      &type_handler_test_double,
      &type_handler_test_double
    },
    {
      &type_handler_sshort,
      &type_handler_test_double,
      &type_handler_test_double
    },
    {
      &type_handler_sint24,
      &type_handler_test_double,
      &type_handler_test_double
    },
    {
      &type_handler_slonglong,
      &type_handler_test_double,
      &type_handler_test_double
    },
    {
      &type_handler_stiny,
      &type_handler_test_int8,
      &type_handler_test_int8
    },
    {
      &type_handler_sshort,
      &type_handler_test_int8,
      &type_handler_test_int8
    },
    {
      &type_handler_sint24,
      &type_handler_test_int8,
      &type_handler_test_int8
    },
    {
      &type_handler_slonglong,
      &type_handler_test_int8,
      &type_handler_test_int8
    },
    {NULL,NULL,NULL}
  };

  return Type_aggregator::find_handler_in_array(agg, h1, h2, true);
}


const Type_handler *
Type_collection_test::aggregate_for_result(const Type_handler *h1,
                                           const Type_handler *h2) const
{
  return aggregate_common(h1, h2);
}


const Type_handler *
Type_collection_test::aggregate_for_min_max(const Type_handler *h1,
                                            const Type_handler *h2) const
{
  return aggregate_common(h1, h2);
}


const Type_handler *
Type_collection_test::aggregate_for_num_op(const Type_handler *h1,
                                           const Type_handler *h2) const
{
  return aggregate_common(h1, h2);
}


const Type_handler *
Type_collection_test::aggregate_for_comparison(const Type_handler *h1,
                                               const Type_handler *h2) const
{
  DBUG_ASSERT(h1 == h1->type_handler_for_comparison());
  DBUG_ASSERT(h2 == h2->type_handler_for_comparison());
  return aggregate_common(h1, h2);
}


/*************************************************************************/

maria_declare_plugin(type_test)
{
  MariaDB_DATA_TYPE_PLUGIN,     // the plugin type (see include/mysql/plugin.h)
  &plugin_descriptor_type_test_int8,   // pointer to type-specific plugin descriptor
  "test_int8",                  // plugin name
  "MariaDB Corporation",        // plugin author
  "Data type TEST_INT8",        // the plugin description
  PLUGIN_LICENSE_GPL,           // the plugin license (see include/mysql/plugin.h)
  0,                            // Pointer to plugin initialization function
  0,                            // Pointer to plugin deinitialization function
  0x0100,                       // Numeric version 0xAABB means AA.BB version
  NULL,                         // Status variables
  NULL,                         // System variables
  "1.0",                        // String version representation
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL // Maturity(see include/mysql/plugin.h)*/
},
{
  MariaDB_DATA_TYPE_PLUGIN,     // the plugin type (see include/mysql/plugin.h)
  &plugin_descriptor_type_test_double,   // pointer to type-specific plugin descriptor
  "test_double",                // plugin name
  "MariaDB Corporation",        // plugin author
  "Data type TEST_DOUBLE",      // the plugin description
  PLUGIN_LICENSE_GPL,           // the plugin license (see include/mysql/plugin.h)
  0,                            // Pointer to plugin initialization function
  0,                            // Pointer to plugin deinitialization function
  0x0100,                       // Numeric version 0xAABB means AA.BB version
  NULL,                         // Status variables
  NULL,                         // System variables
  "1.0",                        // String version representation
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL // Maturity(see include/mysql/plugin.h)*/
}
maria_declare_plugin_end;
