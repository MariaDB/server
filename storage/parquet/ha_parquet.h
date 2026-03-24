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

  const char *table_type() const override
  {
    return "PARQUET";
  }

  const char *index_type(uint) override
  {
    return "NONE";
  }

  ulonglong table_flags() const override
  {
    return 0;
  }

  ulong index_flags(uint, uint, bool) const override
  {
    return 0;
  }

  uint max_supported_record_length() const override
  {
    return HA_MAX_REC_LENGTH;
  }

  int open(const char *name, int mode, uint test_if_locked) override
  {
    return 0;
  }

  int close(void) override
  {
    return 0;
  }

  int write_row(const uchar *) override
  {
    return HA_ERR_WRONG_COMMAND;
  }

  int update_row(const uchar *, const uchar *) override
  {
    return HA_ERR_WRONG_COMMAND;
  }

  int delete_row(const uchar *) override
  {
    return HA_ERR_WRONG_COMMAND;
  }

  int rnd_init(bool) override
  {
    return 0;
  }

  int rnd_next(uchar *) override
  {
    return HA_ERR_END_OF_FILE;
  }

  int rnd_pos(uchar *, uchar *) override
  {
    return HA_ERR_WRONG_COMMAND;
  }

  void position(const uchar *) override
  {
  }

  int info(uint) override
  {
    return 0;
  }

  int extra(enum ha_extra_function) override
  {
    return 0;
  }

  int external_lock(THD *, int) override;

  int delete_all_rows(void) override
  {
    return HA_ERR_WRONG_COMMAND;
  }

  int rename_table(const char *, const char *) override
  {
    return HA_ERR_WRONG_COMMAND;
  }

  int create(const char *, TABLE *, HA_CREATE_INFO *) override
  {
    return 0;
  }

  THR_LOCK_DATA **store_lock(THD *, THR_LOCK_DATA **to,
                             enum thr_lock_type lock_type) override;

private:
  THR_LOCK_DATA lock;
};
#endif
