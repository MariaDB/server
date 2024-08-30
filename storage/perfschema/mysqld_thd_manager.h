#ifndef STORAGE_PERFSCHEMA_MYSQL_THD_MANAGER_INCLUDED
#define STORAGE_PERFSCHEMA_MYSQL_THD_MANAGER_INCLUDED
#include "my_global.h"
#include "my_thread.h"

class Find_THD_Impl
{
  public:
  virtual ~Find_THD_Impl() {}
  virtual bool operator()(THD *thd) = 0;
};

class Do_THD_Impl
{
  public:
  virtual ~Do_THD_Impl() {}
  virtual void operator()(THD*) = 0;
};

class Global_THD_manager
{
  public:
  static Global_THD_manager* get_instance();
  THD* find_thd(Find_THD_Impl *func);
  void do_for_all_thd(Do_THD_Impl *arg);
};

ulong get_system_variable_hash_records(void);
#endif
