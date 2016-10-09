/*
  A temporary header to resolve WebScaleSQL vs MariaDB differences 
  when porting MyRocks to MariaDB.
*/
#ifndef RDB_MARIADB_PORT_H
#define RDB_MARIADB_PORT_H

#include "atomic_stat.h"

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


#endif
