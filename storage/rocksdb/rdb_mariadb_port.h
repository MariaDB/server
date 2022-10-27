/*
  A temporary header to resolve WebScaleSQL vs MariaDB differences 
  when porting MyRocks to MariaDB.
*/
#ifndef RDB_MARIADB_PORT_H
#define RDB_MARIADB_PORT_H

#include "my_global.h"                   /* ulonglong */
#include "atomic_stat.h"

// These are for split_into_vector:
#include <vector>
#include <string>

/* The following is copied from storage/innobase/univ.i: */
#ifndef MY_ATTRIBUTE
#if defined(__GNUC__)
#  define MY_ATTRIBUTE(A) __attribute__(A)
#else
#  define MY_ATTRIBUTE(A)
#endif
#endif

/* Struct used for IO performance counters, shared among multiple threads */
struct my_io_perf_atomic_struct {
  atomic_stat<ulonglong> bytes;
  atomic_stat<ulonglong> requests;
  atomic_stat<ulonglong> svc_time; /*!< time to do read or write operation */
  atomic_stat<ulonglong> svc_time_max;
  atomic_stat<ulonglong> wait_time; /*!< total time in the request array */
  atomic_stat<ulonglong> wait_time_max;
  atomic_stat<ulonglong> slow_ios; /*!< requests that take too long */
};
typedef struct my_io_perf_atomic_struct my_io_perf_atomic_t;

////////////////////////////////////////////////////////////////////////////

/*
  Temporary stand-in for 
  fae59683dc116be2cc78b0b30d61c84659c33bd3
  Print stack traces before committing suicide

*/
#define abort_with_stack_traces()  { abort(); }

////////////////////////////////////////////////////////////////////////////
typedef struct my_io_perf_struct my_io_perf_t;

std::vector<std::string> split_into_vector(const std::string& input,
                                           char delimiter);

void
mysql_bin_log_commit_pos(THD *thd, ulonglong *out_pos, const char **out_file);

#endif
