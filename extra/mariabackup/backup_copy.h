
#ifndef XTRABACKUP_BACKUP_COPY_H
#define XTRABACKUP_BACKUP_COPY_H

#include <my_global.h>
#include <mysql.h>
#include "datasink.h"

/* special files */
#define XTRABACKUP_SLAVE_INFO "xtrabackup_slave_info"
#define XTRABACKUP_GALERA_INFO "xtrabackup_galera_info"
#define XTRABACKUP_DONOR_GALERA_INFO "donor_galera_info"
#define XTRABACKUP_BINLOG_INFO "xtrabackup_binlog_info"
#define XTRABACKUP_INFO "xtrabackup_info"

extern bool binlog_locked;

/************************************************************************
Return true if first and second arguments are the same path. */
bool
equal_paths(const char *first, const char *second);

/** Start --backup */
bool backup_start(ds_ctxt *ds_data, ds_ctxt *ds_meta,
                  CorruptedPages &corrupted_pages);
/** Release resources after backup_start() */
void backup_release();
/** Finish after backup_start() and backup_release() */
bool backup_finish(ds_ctxt *ds_data);
bool
apply_log_finish();
bool
copy_back();
bool
decrypt_decompress();
bool
is_path_separator(char);
bool
directory_exists(const char *dir, bool create);

lsn_t
get_current_lsn(MYSQL *connection);

bool starts_with(const char *str, const char *prefix);
#endif
