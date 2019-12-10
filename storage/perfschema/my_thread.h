#ifndef STORAGE_PERFSCHEMA_MY_THREAD_INCLUDED
#define STORAGE_PERFSCHEMA_MY_THREAD_INCLUDED

#include <my_pthread.h>
#include <m_string.h>
#include "rpl_gtid.h"

typedef pthread_key_t thread_local_key_t;
typedef pthread_t my_thread_handle;
typedef pthread_attr_t my_thread_attr_t;
typedef uint32 my_thread_os_id_t;

static inline int my_create_thread_local_key(thread_local_key_t *key, void (*destructor)(void*))
{ return pthread_key_create(key, destructor); }

static inline int my_delete_thread_local_key(thread_local_key_t key)
{ return pthread_key_delete(key); }

static inline void *my_get_thread_local(thread_local_key_t key)
{ return pthread_getspecific(key); }

static inline int my_set_thread_local(thread_local_key_t key, const void *ptr)
{ return pthread_setspecific(key, ptr); }

static inline int my_thread_create(my_thread_handle *thread,
        const my_thread_attr_t *attr, void *(*start_routine)(void *), void *arg)
{ return pthread_create(thread, attr, start_routine, arg); }

static inline my_thread_os_id_t my_thread_os_id()
{
#ifdef __NR_gettid
  return (uint32)syscall(__NR_gettid);
#else
  return 0;
#endif
}

enum enum_sp_type
{
  SP_TYPE_FUNCTION= 1,
  SP_TYPE_PROCEDURE,
  SP_TYPE_TRIGGER,
  SP_TYPE_EVENT
};

#define CHANNEL_NAME_LENGTH MAX_CONNECTION_NAME

enum enum_mysql_show_scope
{
  SHOW_SCOPE_UNDEF,
  SHOW_SCOPE_GLOBAL,
  SHOW_SCOPE_SESSION,
  SHOW_SCOPE_ALL
};
typedef enum enum_mysql_show_scope SHOW_SCOPE;

#define SHOW_VAR_MAX_NAME_LEN NAME_LEN

static inline char *my_stpnmov(char *dst, const char *src, size_t n)
{ return strnmov(dst, src, n); }

class Gtid_specification: public rpl_gtid
{
public:
  size_t to_string(char *buf)
  {
    return my_snprintf(buf, GTID_MAX_STR_LENGTH, "%u-%u-%llu",
                       domain_id, server_id, seq_no);
  }
};

static inline size_t bin_to_hex_str(char *to, size_t to_len,
                                    const char *from, size_t from_len)
{
  if (to_len < from_len * 2 + 1)
    return 0 ;
  for (size_t i=0; i < from_len; i++, from++)
  {
    *to++=_dig_vec_upper[((unsigned char) *from) >> 4];
    *to++=_dig_vec_upper[((unsigned char) *from) & 0xF];
  }
  *to= '\0';
  return from_len * 2 + 1;
}

enum enum_psi_status { PENDING = 0, GRANTED,
                       PRE_ACQUIRE_NOTIFY, POST_RELEASE_NOTIFY };

PSI_thread* thd_get_psi(THD *thd);
#endif
