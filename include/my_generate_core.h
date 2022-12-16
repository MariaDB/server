#ifndef MY_GENERATE_CORE_H
#define MY_GENERATE_CORE_H
#ifndef _WIN32
enum my_coredump_place_t
{
  GTID_SLAVE_POS,
  LOCK_REC_SET_NTH_BIT,
  LOCK_WAIT_TIMEOUT
};
void my_generate_coredump(enum my_coredump_place_t which,
                          const char *coredump_path);
#endif /* _WIN32 */
#endif /* MY_GENERATE_CORE_H */
