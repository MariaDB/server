#ifndef MY_GENERATE_CORE_H
#define MY_GENERATE_CORE_H
enum my_coredump_place_t
{
  SYS_VAR_UPDATE,
  GTID_SLAVE_POS,
  LOCK_REC_SET_NTH_BIT,
  LOCK_WAIT_TIMEOUT,
};

#ifndef _WIN32
void my_generate_coredump(enum my_coredump_place_t which);
#else /* _WIN32 */
static inline void my_generate_coredump(enum my_coredump_place_t) {};
#endif /* _WIN32 */
#endif /* MY_GENERATE_CORE_H */
