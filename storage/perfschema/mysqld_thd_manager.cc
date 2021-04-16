#include "mysqld_thd_manager.h"
#include "sql_class.h"

static Global_THD_manager manager;
Global_THD_manager* Global_THD_manager::get_instance()
{
  return &manager;
}

struct find_thd_arg
{
  Find_THD_Impl *func;
  THD *cur;
};

static my_bool find_thd_cb(THD *tmp, find_thd_arg *arg)
{
  arg->cur= tmp;
  return (*arg->func)(tmp);
}

THD* Global_THD_manager::find_thd(Find_THD_Impl *func)
{
  find_thd_arg arg= {func, NULL};
  if (THD_list_iterator::iterator()->iterate(find_thd_cb, &arg))
    return arg.cur;
  return NULL;
}

static my_bool do_for_all_cb(THD *tmp, Do_THD_Impl *arg)
{
  (*arg)(tmp);
  return 0;
}

void Global_THD_manager::do_for_all_thd(Do_THD_Impl *arg)
{
  THD_list_iterator::iterator()->iterate(do_for_all_cb, arg);
}
