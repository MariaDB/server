/*
   Copyright (c) 2023, MariaDB Corporation

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
#include <sql_class.h>
#include <mysql/plugin_data_type.h>
#include "sql_type.h"


class Type_collection_local: public Type_collection
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
    return aggregate_common(h1, h2);
  }

  const Type_handler *aggregate_for_min_max(const Type_handler *h1,
                                            const Type_handler *h2)
                                            const override
  {
    return aggregate_common(h1, h2);
  }

  const Type_handler *aggregate_for_num_op(const Type_handler *h1,
                                           const Type_handler *h2)
                                           const override
  {
    return aggregate_common(h1, h2);
  }
};


static Type_collection_local type_collection_local;


/*
  A more MySQL compatible Field:
  it does not set the UNSIGNED_FLAG.
  This is how MySQL's Field_timestampf works.
*/
class Field_mysql_timestampf :public Field_timestampf
{
public:
  Field_mysql_timestampf(const LEX_CSTRING &name,
                         const Record_addr &addr,
                         enum utype unireg_check_arg,
                         TABLE_SHARE *share, decimal_digits_t dec_arg)
   :Field_timestampf(addr.ptr(), addr.null_ptr(), addr.null_bit(),
                     unireg_check_arg, &name, share, dec_arg)
  {
    flags&= ~UNSIGNED_FLAG; // MySQL compatibility
  }
  void sql_type(String &str) const override
  {
    sql_type_opt_dec_comment(str,
                             Field_mysql_timestampf::type_handler()->name(),
                             dec, type_version_mysql56());
  }
  const Type_handler *type_handler() const override;
};


class Type_handler_mysql_timestamp2: public Type_handler_timestamp2
{
public:
  const Type_collection *type_collection() const override
  {
    return &type_collection_local;
  }
  Field *make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *root,
                                   const LEX_CSTRING *name,
                                   const Record_addr &rec, const Bit_addr &bit,
                                   const Column_definition_attributes *attr,
                                   uint32 flags) const override
  {
    return new (root)
      Field_mysql_timestampf(*name, rec, attr->unireg_check, share,
                             attr->temporal_dec(MAX_DATETIME_WIDTH));
  }
  const Type_handler *type_handler_for_implicit_upgrade() const override
  {
    /*
      The derived method as of 10.11.8 does "return this;" anyway.
      However, in the future this may change to return a
      opt_mysql56_temporal_format dependent handler.
      Here in this class we need to make sure to do "return this;"
      not to depend on the derived method changes.
    */
    return this;
  }
  void Column_definition_implicit_upgrade_to_this(Column_definition *old)
                                                           const override
  {
    /*
      Suppress the automatic upgrade depending on opt_mysql56_temporal_format,
      derived from Type_handler_timestamp_common.
    */
  }
};


static Type_handler_mysql_timestamp2 type_handler_mysql_timestamp2;


const Type_handler *Field_mysql_timestampf::type_handler() const
{
  return &type_handler_mysql_timestamp2;
}


const Type_handler *
Type_collection_local::aggregate_common(const Type_handler *h1,
                                        const Type_handler *h2) const
{
  if (h1 == h2)
    return h1;

  static const Type_aggregator::Pair agg[]=
  {
    {
      &type_handler_timestamp2,
      &type_handler_mysql_timestamp2,
      &type_handler_mysql_timestamp2
    },
    {NULL,NULL,NULL}
  };

  return Type_aggregator::find_handler_in_array(agg, h1, h2, true);
}


static struct st_mariadb_data_type plugin_descriptor_type_mysql_timestamp=
{
  MariaDB_DATA_TYPE_INTERFACE_VERSION,
  &type_handler_mysql_timestamp2
};



/*************************************************************************/

maria_declare_plugin(type_mysql_timestamp)
{
  MariaDB_DATA_TYPE_PLUGIN,     // the plugin type (see include/mysql/plugin.h)
  &plugin_descriptor_type_mysql_timestamp, // pointer to type-specific plugin descriptor
  "type_mysql_timestamp",       // plugin name
  "MariaDB Corporation",        // plugin author
  "Data type TYPE_MYSQL_TIMESTAMP", // the plugin description
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
