#include <perfmon/pfmlib_perf_event.h>
/*
#include <linux/perf_event.h>
*/

# ifdef __cplusplus
extern "C" {
# endif

extern my_bool my_perf_init(void);
extern void my_perf_deinit(void);
extern my_bool my_perf_start_record(void);
extern void my_perf_end_record(void);

# ifdef __cplusplus
}
# endif
