#ifndef HA_PARQUET_INCLUDED
#define HA_PARQUET_INCLUDED


#include "ha_maria.h"


class ha_parquet final :public ha_maria 
{
    enum alter_table_op
    { S3_NO_ALTER, S3_ALTER_TABLE, S3_ADD_PARTITION, S3_ADD_TMP_PARTITION };
    alter_table_op in_alter_table;
    S3_INFO *open_args;

public:
    ha_parquet(handlerton *hton, TABLE_SHARE * table_arg);
    ~ha_parquet() {}

    int create(const char *name, TABLE *table_arg,
             HA_CREATE_INFO *ha_create_info) override;
    int open(const char *name, int mode, uint open_flags) override;
    int write_row(const uchar *buf) override;
    int close() override;
    
    
    
    
    void register_handler(MARIA_HA *file) override;
    
    // int delete_row() override;
    // int update_row(const uchar *, const uchar *) override;
    // int alter_table() override;



};
