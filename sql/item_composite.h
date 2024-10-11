#ifndef ITEM_COMPOSITE_INCLUDED
#define ITEM_COMPOSITE_INCLUDED

#include "item.h"

class Item_composite_base
{
public:
  virtual ~Item_composite_base() = default;

  virtual const char *composite_name() const = 0;

  // For associative arrays
  /// Returns the number of columns for the elements of the array
  virtual uint cols_for_elements() const { return 0; }
  virtual uint rows() const { return 1; }
  virtual bool get_key(String *key, bool is_first) { return true; }
  virtual bool get_next_key(const String *curr_key, String *next_key)
  {
    return true;
  }
  virtual Item *element_by_key(THD *thd, String *key) { return nullptr; }
  virtual Item **element_addr_by_key(THD *thd, Item **addr_arg, String *key)
  {
    return addr_arg;
  }
};

class Item_composite: public Item_fixed_hybrid,
                      protected Item_args,
                      public Item_composite_base
{
public:
  Item_composite(THD *thd, List<Item> &list)
    :Item_fixed_hybrid(thd), Item_args(thd, list)
  { }
  Item_composite(THD *thd, Item_args *other)
    :Item_fixed_hybrid(thd), Item_args(thd, other)
  { }
  Item_composite(THD *thd)
    :Item_fixed_hybrid(thd)
  { }

  void illegal_method_call(const char *);

  void make_send_field(THD *thd, Send_field *) override
  {
    illegal_method_call((const char*)"make_send_field");
  };
  double val_real() override
  {
    illegal_method_call((const char*)"val");
    return 0;
  };
  longlong val_int() override
  {
    illegal_method_call((const char*)"val_int");
    return 0;
  };
  String *val_str(String *) override
  {
    illegal_method_call((const char*)"val_str");
    return 0;
  };
  my_decimal *val_decimal(my_decimal *) override
  {
    illegal_method_call((const char*)"val_decimal");
    return 0;
  };
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  {
    illegal_method_call((const char*)"get_date");
    return true;
  }
};


#endif /* ITEM_COMPOSITE_INCLUDED */
