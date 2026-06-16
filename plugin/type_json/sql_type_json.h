#ifndef SQL_TYPE_JSON_PLUGIN_INCLUDED
#define SQL_TYPE_JSON_PLUGIN_INCLUDED


#include "mariadb.h"
#include "sql_type.h"

class Type_handler_json: public Type_handler_long_blob{




    
};

extern Type_handler_json type_handler_json;

#include "field.h"

class Field_json:public Field_blob
{

public:
    Field_json(uchar *ptr_arg, uchar *null_ptr_arg, uchar null_bit_arg,
             enum utype unireg_check_arg, const LEX_CSTRING *field_name_arg,
             TABLE_SHARE *share, uint blob_pack_length,
             const DTCollation &collation)
      : Field_blob(ptr_arg, null_ptr_arg, null_bit_arg, unireg_check_arg,
                    field_name_arg, share, blob_pack_length, collation)
    {}
    const Type_handler *type_handler() const override
    {
        return &type_handler_json;
    }

    void sql_type(String $str) const override;

    uint size_of() const override {
        return sizeof(*this);
    }

};

#endif // SQL_TYPE_JSON_INCLUDED
