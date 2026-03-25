#ifndef HA_PARQUET_INCLUDED
#define HA_PARQUET_INCLUDED

#define MYSQL_SERVER 1

#include "handler.h"
#include "thr_lock.h"
#include "my_base.h"

class ha_parquet final : public handler
{
public:
  ha_parquet(handlerton *hton, TABLE_SHARE *table_arg);

  ~ha_parquet() override = default;

  ulonglong table_flags() const override;
  ulong index_flags(uint, uint, bool) const override;

  int open(const char *name, int mode, uint test_if_locked) override;
  int close(void) override;
  int create(const char *, TABLE *, HA_CREATE_INFO *) override;


  int write_row(const uchar *) override;
  int update_row(const uchar *, const uchar *) override;
  int delete_row(const uchar *) override;
  
  int rnd_init(bool scan) override;
  int rnd_next(uchar *buf) override;
  int rnd_pos(uchar *buf, uchar *pos) override;
  void position(const uchar *record) override;
  int info(uint) override;

  enum_alter_inplace_result check_if_supported_inplace_alter(TABLE *altered_table, Alter_inplace_info *ha_alter_info) override;

  int external_lock(THD *, int) override;

  THR_LOCK_DATA **store_lock(THD *, THR_LOCK_DATA **to,
                             enum thr_lock_type lock_type) override;

private:
  THR_LOCK_DATA lock;
};
#endif
