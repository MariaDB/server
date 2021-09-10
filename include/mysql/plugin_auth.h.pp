class THD;
class Item;
typedef char my_bool;
typedef void * MYSQL_PLUGIN;
extern "C" {
extern "C" {
extern struct base64_service_st {
  int (*base64_needed_encoded_length_ptr)(int length_of_data);
  int (*base64_encode_max_arg_length_ptr)(void);
  int (*base64_needed_decoded_length_ptr)(int length_of_encoded_data);
  int (*base64_decode_max_arg_length_ptr)();
  int (*base64_encode_ptr)(const void *src, size_t src_len, char *dst);
  int (*base64_decode_ptr)(const char *src, size_t src_len,
                           void *dst, const char **end_ptr, int flags);
} *base64_service;
int my_base64_needed_encoded_length(int length_of_data);
int my_base64_encode_max_arg_length(void);
int my_base64_needed_decoded_length(int length_of_encoded_data);
int my_base64_decode_max_arg_length();
int my_base64_encode(const void *src, size_t src_len, char *dst);
int my_base64_decode(const char *src, size_t src_len,
                  void *dst, const char **end_ptr, int flags);
}
extern "C" {
extern void (*debug_sync_C_callback_ptr)(THD*, const char *, size_t);
}
extern "C" {
struct encryption_service_st {
  unsigned int (*encryption_key_get_latest_version_func)(unsigned int key_id);
  unsigned int (*encryption_key_get_func)(unsigned int key_id, unsigned int key_version,
                                          unsigned char* buffer, unsigned int* length);
  unsigned int (*encryption_ctx_size_func)(unsigned int key_id, unsigned int key_version);
  int (*encryption_ctx_init_func)(void *ctx, const unsigned char* key, unsigned int klen,
                                  const unsigned char* iv, unsigned int ivlen,
                                  int flags, unsigned int key_id,
                                  unsigned int key_version);
  int (*encryption_ctx_update_func)(void *ctx, const unsigned char* src, unsigned int slen,
                                    unsigned char* dst, unsigned int* dlen);
  int (*encryption_ctx_finish_func)(void *ctx, unsigned char* dst, unsigned int* dlen);
  unsigned int (*encryption_encrypted_length_func)(unsigned int slen, unsigned int key_id, unsigned int key_version);
};
extern struct encryption_service_st encryption_handler;
static inline unsigned int encryption_key_id_exists(unsigned int id)
{
  return encryption_handler.encryption_key_get_latest_version_func(id) != (~(unsigned int)0);
}
static inline unsigned int encryption_key_version_exists(unsigned int id, unsigned int version)
{
  unsigned int unused;
  return encryption_handler.encryption_key_get_func((id),(version),(NULL),(&unused)) != (~(unsigned int)0);
}
static inline int encryption_crypt(const unsigned char* src, unsigned int slen,
                                   unsigned char* dst, unsigned int* dlen,
                                   const unsigned char* key, unsigned int klen,
                                   const unsigned char* iv, unsigned int ivlen,
                                   int flags, unsigned int key_id, unsigned int key_version)
{
  void *ctx= alloca(encryption_handler.encryption_ctx_size_func((key_id),(key_version)));
  int res1, res2;
  unsigned int d1, d2;
  if ((res1= encryption_handler.encryption_ctx_init_func((ctx),(key),(klen),(iv),(ivlen),(flags),(key_id),(key_version))))
    return res1;
  res1= encryption_handler.encryption_ctx_update_func((ctx),(src),(slen),(dst),(&d1));
  res2= encryption_handler.encryption_ctx_finish_func((ctx),(dst + d1),(&d2));
  *dlen= d1 + d2;
  return res1 ? res1 : res2;
}
}
extern "C" {
struct st_encryption_scheme_key {
  unsigned int version;
  unsigned char key[16];
};
struct st_encryption_scheme {
  unsigned char iv[16];
  struct st_encryption_scheme_key key[3];
  unsigned int keyserver_requests;
  unsigned int key_id;
  unsigned int type;
  void (*locker)(struct st_encryption_scheme *self, int release);
};
extern struct encryption_scheme_service_st {
  int (*encryption_scheme_encrypt_func)
                               (const unsigned char* src, unsigned int slen,
                                unsigned char* dst, unsigned int* dlen,
                                struct st_encryption_scheme *scheme,
                                unsigned int key_version, unsigned int i32_1,
                                unsigned int i32_2, unsigned long long i64);
  int (*encryption_scheme_decrypt_func)
                               (const unsigned char* src, unsigned int slen,
                                unsigned char* dst, unsigned int* dlen,
                                struct st_encryption_scheme *scheme,
                                unsigned int key_version, unsigned int i32_1,
                                unsigned int i32_2, unsigned long long i64);
} *encryption_scheme_service;
int encryption_scheme_encrypt(const unsigned char* src, unsigned int slen,
                              unsigned char* dst, unsigned int* dlen,
                              struct st_encryption_scheme *scheme,
                              unsigned int key_version, unsigned int i32_1,
                              unsigned int i32_2, unsigned long long i64);
int encryption_scheme_decrypt(const unsigned char* src, unsigned int slen,
                              unsigned char* dst, unsigned int* dlen,
                              struct st_encryption_scheme *scheme,
                              unsigned int key_version, unsigned int i32_1,
                              unsigned int i32_2, unsigned long long i64);
}
extern "C" {
enum thd_kill_levels {
  THD_IS_NOT_KILLED=0,
  THD_ABORT_SOFTLY=50,
  THD_ABORT_ASAP=100,
};
extern struct kill_statement_service_st {
  enum thd_kill_levels (*thd_kill_level_func)(const THD*);
} *thd_kill_statement_service;
enum thd_kill_levels thd_kill_level(const THD*);
}
extern "C" {
typedef struct logger_handle_st LOGGER_HANDLE;
extern struct logger_service_st {
  void (*logger_init_mutexes)();
  LOGGER_HANDLE* (*open)(const char *path,
                         unsigned long long size_limit,
                         unsigned int rotations);
  int (*close)(LOGGER_HANDLE *log);
  int (*vprintf)(LOGGER_HANDLE *log, const char *fmt, va_list argptr);
  int (*printf)(LOGGER_HANDLE *log, const char *fmt, ...);
  int (*write)(LOGGER_HANDLE *log, const char *buffer, size_t size);
  int (*rotate)(LOGGER_HANDLE *log);
} *logger_service;
  void logger_init_mutexes();
  LOGGER_HANDLE *logger_open(const char *path,
                             unsigned long long size_limit,
                             unsigned int rotations);
  int logger_close(LOGGER_HANDLE *log);
  int logger_vprintf(LOGGER_HANDLE *log, const char *fmt, va_list argptr);
  int logger_printf(LOGGER_HANDLE *log, const char *fmt, ...);
  int logger_write(LOGGER_HANDLE *log, const char *buffer, size_t size);
  int logger_rotate(LOGGER_HANDLE *log);
}
extern "C" {
extern struct my_md5_service_st {
  void (*my_md5_type)(unsigned char*, const char*, size_t);
  void (*my_md5_multi_type)(unsigned char*, ...);
  size_t (*my_md5_context_size_type)();
  void (*my_md5_init_type)(void *);
  void (*my_md5_input_type)(void *, const unsigned char *, size_t);
  void (*my_md5_result_type)(void *, unsigned char *);
} *my_md5_service;
void my_md5(unsigned char*, const char*, size_t);
void my_md5_multi(unsigned char*, ...);
size_t my_md5_context_size();
void my_md5_init(void *context);
void my_md5_input(void *context, const unsigned char *buf, size_t len);
void my_md5_result(void *context, unsigned char *digest);
}
extern "C" {
enum my_aes_mode {
    MY_AES_ECB, MY_AES_CBC
};
extern struct my_crypt_service_st {
  int (*my_aes_crypt_init)(void *ctx, enum my_aes_mode mode, int flags,
                      const unsigned char* key, unsigned int klen,
                      const unsigned char* iv, unsigned int ivlen);
  int (*my_aes_crypt_update)(void *ctx, const unsigned char *src, unsigned int slen,
                        unsigned char *dst, unsigned int *dlen);
  int (*my_aes_crypt_finish)(void *ctx, unsigned char *dst, unsigned int *dlen);
  int (*my_aes_crypt)(enum my_aes_mode mode, int flags,
                 const unsigned char *src, unsigned int slen, unsigned char *dst, unsigned int *dlen,
                 const unsigned char *key, unsigned int klen, const unsigned char *iv, unsigned int ivlen);
  unsigned int (*my_aes_get_size)(enum my_aes_mode mode, unsigned int source_length);
  unsigned int (*my_aes_ctx_size)(enum my_aes_mode mode);
  int (*my_random_bytes)(unsigned char* buf, int num);
} *my_crypt_service;
int my_aes_crypt_init(void *ctx, enum my_aes_mode mode, int flags,
                      const unsigned char* key, unsigned int klen,
                      const unsigned char* iv, unsigned int ivlen);
int my_aes_crypt_update(void *ctx, const unsigned char *src, unsigned int slen,
                        unsigned char *dst, unsigned int *dlen);
int my_aes_crypt_finish(void *ctx, unsigned char *dst, unsigned int *dlen);
int my_aes_crypt(enum my_aes_mode mode, int flags,
                 const unsigned char *src, unsigned int slen, unsigned char *dst, unsigned int *dlen,
                 const unsigned char *key, unsigned int klen, const unsigned char *iv, unsigned int ivlen);
int my_random_bytes(unsigned char* buf, int num);
unsigned int my_aes_get_size(enum my_aes_mode mode, unsigned int source_length);
unsigned int my_aes_ctx_size(enum my_aes_mode mode);
}
extern "C" {
extern struct my_print_error_service_st {
  void (*my_error_func)(unsigned int nr, unsigned long MyFlags, ...);
  void (*my_printf_error_func)(unsigned int nr, const char *fmt, unsigned long MyFlags,...);
  void (*my_printv_error_func)(unsigned int error, const char *format, unsigned long MyFlags, va_list ap);
} *my_print_error_service;
extern void my_error(unsigned int nr, unsigned long MyFlags, ...);
extern void my_printf_error(unsigned int my_err, const char *format, unsigned long MyFlags, ...);
extern void my_printv_error(unsigned int error, const char *format, unsigned long MyFlags,va_list ap);
}
extern "C" {
extern struct my_snprintf_service_st {
  size_t (*my_snprintf_type)(char*, size_t, const char*, ...);
  size_t (*my_vsnprintf_type)(char *, size_t, const char*, va_list);
} *my_snprintf_service;
size_t my_snprintf(char* to, size_t n, const char* fmt, ...);
size_t my_vsnprintf(char *to, size_t n, const char* fmt, va_list ap);
}
extern "C" {
extern struct progress_report_service_st {
  void (*thd_progress_init_func)(THD* thd, unsigned int max_stage);
  void (*thd_progress_report_func)(THD* thd,
                                   unsigned long long progress,
                                   unsigned long long max_progress);
  void (*thd_progress_next_stage_func)(THD* thd);
  void (*thd_progress_end_func)(THD* thd);
  const char *(*set_thd_proc_info_func)(THD*, const char *info,
                                        const char *func,
                                        const char *file,
                                        unsigned int line);
} *progress_report_service;
void thd_progress_init(THD* thd, unsigned int max_stage);
void thd_progress_report(THD* thd,
                         unsigned long long progress,
                         unsigned long long max_progress);
void thd_progress_next_stage(THD* thd);
void thd_progress_end(THD* thd);
const char *set_thd_proc_info(THD*, const char * info, const char *func,
                              const char *file, unsigned int line);
}
extern "C" {
extern struct my_sha1_service_st {
  void (*my_sha1_type)(unsigned char*, const char*, size_t);
  void (*my_sha1_multi_type)(unsigned char*, ...);
  size_t (*my_sha1_context_size_type)();
  void (*my_sha1_init_type)(void *);
  void (*my_sha1_input_type)(void *, const unsigned char *, size_t);
  void (*my_sha1_result_type)(void *, unsigned char *);
} *my_sha1_service;
void my_sha1(unsigned char*, const char*, size_t);
void my_sha1_multi(unsigned char*, ...);
size_t my_sha1_context_size();
void my_sha1_init(void *context);
void my_sha1_input(void *context, const unsigned char *buf, size_t len);
void my_sha1_result(void *context, unsigned char *digest);
}
extern "C" {
extern struct my_sha2_service_st {
  void (*my_sha224_type)(unsigned char*, const char*, size_t);
  void (*my_sha224_multi_type)(unsigned char*, ...);
  size_t (*my_sha224_context_size_type)();
  void (*my_sha224_init_type)(void *);
  void (*my_sha224_input_type)(void *, const unsigned char *, size_t);
  void (*my_sha224_result_type)(void *, unsigned char *);
  void (*my_sha256_type)(unsigned char*, const char*, size_t);
  void (*my_sha256_multi_type)(unsigned char*, ...);
  size_t (*my_sha256_context_size_type)();
  void (*my_sha256_init_type)(void *);
  void (*my_sha256_input_type)(void *, const unsigned char *, size_t);
  void (*my_sha256_result_type)(void *, unsigned char *);
  void (*my_sha384_type)(unsigned char*, const char*, size_t);
  void (*my_sha384_multi_type)(unsigned char*, ...);
  size_t (*my_sha384_context_size_type)();
  void (*my_sha384_init_type)(void *);
  void (*my_sha384_input_type)(void *, const unsigned char *, size_t);
  void (*my_sha384_result_type)(void *, unsigned char *);
  void (*my_sha512_type)(unsigned char*, const char*, size_t);
  void (*my_sha512_multi_type)(unsigned char*, ...);
  size_t (*my_sha512_context_size_type)();
  void (*my_sha512_init_type)(void *);
  void (*my_sha512_input_type)(void *, const unsigned char *, size_t);
  void (*my_sha512_result_type)(void *, unsigned char *);
} *my_sha2_service;
void my_sha224(unsigned char*, const char*, size_t);
void my_sha224_multi(unsigned char*, ...);
size_t my_sha224_context_size();
void my_sha224_init(void *context);
void my_sha224_input(void *context, const unsigned char *buf, size_t len);
void my_sha224_result(void *context, unsigned char *digest);
void my_sha256(unsigned char*, const char*, size_t);
void my_sha256_multi(unsigned char*, ...);
size_t my_sha256_context_size();
void my_sha256_init(void *context);
void my_sha256_input(void *context, const unsigned char *buf, size_t len);
void my_sha256_result(void *context, unsigned char *digest);
void my_sha384(unsigned char*, const char*, size_t);
void my_sha384_multi(unsigned char*, ...);
size_t my_sha384_context_size();
void my_sha384_init(void *context);
void my_sha384_input(void *context, const unsigned char *buf, size_t len);
void my_sha384_result(void *context, unsigned char *digest);
void my_sha512(unsigned char*, const char*, size_t);
void my_sha512_multi(unsigned char*, ...);
size_t my_sha512_context_size();
void my_sha512_init(void *context);
void my_sha512_input(void *context, const unsigned char *buf, size_t len);
void my_sha512_result(void *context, unsigned char *digest);
}
extern "C" {
struct st_mysql_lex_string
{
  char *str;
  size_t length;
};
typedef struct st_mysql_lex_string MYSQL_LEX_STRING;
struct st_mysql_const_lex_string
{
  const char *str;
  size_t length;
};
typedef struct st_mysql_const_lex_string MYSQL_CONST_LEX_STRING;
extern struct thd_alloc_service_st {
  void *(*thd_alloc_func)(THD*, size_t);
  void *(*thd_calloc_func)(THD*, size_t);
  char *(*thd_strdup_func)(THD*, const char *);
  char *(*thd_strmake_func)(THD*, const char *, size_t);
  void *(*thd_memdup_func)(THD*, const void*, size_t);
  MYSQL_CONST_LEX_STRING *(*thd_make_lex_string_func)(THD*,
                                        MYSQL_CONST_LEX_STRING *,
                                        const char *, size_t, int);
} *thd_alloc_service;
void *thd_alloc(THD* thd, size_t size);
void *thd_calloc(THD* thd, size_t size);
char *thd_strdup(THD* thd, const char *str);
char *thd_strmake(THD* thd, const char *str, size_t size);
void *thd_memdup(THD* thd, const void* str, size_t size);
MYSQL_CONST_LEX_STRING
*thd_make_lex_string(THD* thd, MYSQL_CONST_LEX_STRING *lex_str,
                     const char *str, size_t size,
                     int allocate_lex_string);
}
extern "C" {
extern struct thd_autoinc_service_st {
  void (*thd_get_autoinc_func)(const THD* thd,
                               unsigned long* off, unsigned long* inc);
} *thd_autoinc_service;
void thd_get_autoinc(const THD* thd,
                     unsigned long* off, unsigned long* inc);
}
extern "C" {
extern struct thd_error_context_service_st {
  const char *(*thd_get_error_message_func)(const THD* thd);
  unsigned int (*thd_get_error_number_func)(const THD* thd);
  unsigned long (*thd_get_error_row_func)(const THD* thd);
  void (*thd_inc_error_row_func)(THD* thd);
  char *(*thd_get_error_context_description_func)(THD* thd,
                                                  char *buffer,
                                                  unsigned int length,
                                                  unsigned int max_query_length);
} *thd_error_context_service;
const char *thd_get_error_message(const THD* thd);
unsigned int thd_get_error_number(const THD* thd);
unsigned long thd_get_error_row(const THD* thd);
void thd_inc_error_row(THD* thd);
char *thd_get_error_context_description(THD* thd,
                                        char *buffer, unsigned int length,
                                        unsigned int max_query_length);
}
extern "C" {
extern struct thd_rnd_service_st {
  double (*thd_rnd_ptr)(THD* thd);
  void (*thd_c_r_p_ptr)(THD* thd, char *to, size_t length);
} *thd_rnd_service;
double thd_rnd(THD* thd);
void thd_create_random_password(THD* thd, char *to, size_t length);
}
extern "C" {
typedef int MYSQL_THD_KEY_T;
extern struct thd_specifics_service_st {
  int (*thd_key_create_func)(MYSQL_THD_KEY_T *key);
  void (*thd_key_delete_func)(MYSQL_THD_KEY_T *key);
  void *(*thd_getspecific_func)(THD* thd, MYSQL_THD_KEY_T key);
  int (*thd_setspecific_func)(THD* thd, MYSQL_THD_KEY_T key, void *value);
} *thd_specifics_service;
int thd_key_create(MYSQL_THD_KEY_T *key);
void thd_key_delete(MYSQL_THD_KEY_T *key);
void* thd_getspecific(THD* thd, MYSQL_THD_KEY_T key);
int thd_setspecific(THD* thd, MYSQL_THD_KEY_T key, void *value);
}
typedef long my_time_t;
enum enum_mysql_timestamp_type
{
  MYSQL_TIMESTAMP_NONE= -2, MYSQL_TIMESTAMP_ERROR= -1,
  MYSQL_TIMESTAMP_DATE= 0, MYSQL_TIMESTAMP_DATETIME= 1, MYSQL_TIMESTAMP_TIME= 2
};
typedef struct st_mysql_time
{
  unsigned int year, month, day, hour, minute, second;
  unsigned long second_part;
  my_bool neg;
  enum enum_mysql_timestamp_type time_type;
} MYSQL_TIME;
extern "C" {
extern struct thd_timezone_service_st {
  my_time_t (*thd_TIME_to_gmt_sec)(THD* thd, const MYSQL_TIME *ltime, unsigned int *errcode);
  void (*thd_gmt_sec_to_TIME)(THD* thd, MYSQL_TIME *ltime, my_time_t t);
} *thd_timezone_service;
my_time_t thd_TIME_to_gmt_sec(THD* thd, const MYSQL_TIME *ltime, unsigned int *errcode);
void thd_gmt_sec_to_TIME(THD* thd, MYSQL_TIME *ltime, my_time_t t);
}
extern "C" {
typedef enum _thd_wait_type_e {
  THD_WAIT_SLEEP= 1,
  THD_WAIT_DISKIO= 2,
  THD_WAIT_ROW_LOCK= 3,
  THD_WAIT_GLOBAL_LOCK= 4,
  THD_WAIT_META_DATA_LOCK= 5,
  THD_WAIT_TABLE_LOCK= 6,
  THD_WAIT_USER_LOCK= 7,
  THD_WAIT_BINLOG= 8,
  THD_WAIT_GROUP_COMMIT= 9,
  THD_WAIT_SYNC= 10,
  THD_WAIT_NET= 11,
  THD_WAIT_LAST= 12
} thd_wait_type;
extern struct thd_wait_service_st {
  void (*thd_wait_begin_func)(THD*, int);
  void (*thd_wait_end_func)(THD*);
} *thd_wait_service;
void thd_wait_begin(THD* thd, int wait_type);
void thd_wait_end(THD* thd);
}
extern "C" {
enum json_types
{
  JSV_BAD_JSON=-1,
  JSV_NOTHING=0,
  JSV_OBJECT=1,
  JSV_ARRAY=2,
  JSV_STRING=3,
  JSV_NUMBER=4,
  JSV_TRUE=5,
  JSV_FALSE=6,
  JSV_NULL=7
};
extern struct json_service_st {
  enum json_types (*json_type)(const char *js, const char *js_end,
                               const char **value, int *value_len);
  enum json_types (*json_get_array_item)(const char *js, const char *js_end,
                                         int n_item,
                                         const char **value, int *value_len);
  enum json_types (*json_get_object_key)(const char *js, const char *js_end,
                                         const char *key,
                                         const char **value, int *value_len);
  enum json_types (*json_get_object_nkey)(const char *js,const char *js_end,
                             int nkey,
                             const char **keyname, const char **keyname_end,
                             const char **value, int *value_len);
  int (*json_escape_string)(const char *str,const char *str_end,
                          char *json, char *json_end);
  int (*json_unescape_json)(const char *json_str, const char *json_end,
                          char *res, char *res_end);
} *json_service;
enum json_types json_type(const char *js, const char *js_end,
                          const char **value, int *value_len);
enum json_types json_get_array_item(const char *js, const char *js_end,
                                    int n_item,
                                    const char **value, int *value_len);
enum json_types json_get_object_key(const char *js, const char *js_end,
                                    const char *key,
                                    const char **value, int *value_len);
enum json_types json_get_object_nkey(const char *js,const char *js_end, int nkey,
                       const char **keyname, const char **keyname_end,
                       const char **value, int *value_len);
int json_escape_string(const char *str,const char *str_end,
                       char *json, char *json_end);
int json_unescape_json(const char *json_str, const char *json_end,
                       char *res, char *res_end);
}
extern "C" {
extern struct sql_service_st {
  MYSQL *(STDCALL *mysql_init_func)(MYSQL *mysql);
  MYSQL *(*mysql_real_connect_local_func)(MYSQL *mysql,
    const char *host, const char *user, const char *db,
    unsigned long clientflag);
  MYSQL *(STDCALL *mysql_real_connect_func)(MYSQL *mysql, const char *host,
      const char *user, const char *passwd, const char *db, unsigned int port,
      const char *unix_socket, unsigned long clientflag);
  unsigned int(STDCALL *mysql_errno_func)(MYSQL *mysql);
  const char *(STDCALL *mysql_error_func)(MYSQL *mysql);
  int (STDCALL *mysql_real_query_func)(MYSQL *mysql, const char *q,
                                  unsigned long length);
  my_ulonglong (STDCALL *mysql_affected_rows_func)(MYSQL *mysql);
  my_ulonglong (STDCALL *mysql_num_rows_func)(MYSQL_RES *res);
  MYSQL_RES *(STDCALL *mysql_store_result_func)(MYSQL *mysql);
  void (STDCALL *mysql_free_result_func)(MYSQL_RES *result);
  MYSQL_ROW (STDCALL *mysql_fetch_row_func)(MYSQL_RES *result);
  void (STDCALL *mysql_close_func)(MYSQL *mysql);
} *sql_service;
MYSQL *mysql_real_connect_local(MYSQL *mysql,
    const char *host, const char *user, const char *db,
    unsigned long clientflag);
}
}
struct st_mysql_xid {
  long formatID;
  long gtrid_length;
  long bqual_length;
  char data[128];
};
typedef struct st_mysql_xid MYSQL_XID;
enum enum_mysql_show_type
{
  SHOW_UNDEF, SHOW_BOOL, SHOW_UINT, SHOW_ULONG,
  SHOW_ULONGLONG, SHOW_CHAR, SHOW_CHAR_PTR,
  SHOW_ARRAY, SHOW_FUNC, SHOW_DOUBLE,
  SHOW_SINT, SHOW_SLONG, SHOW_SLONGLONG, SHOW_SIMPLE_FUNC,
  SHOW_SIZE_T, SHOW_always_last
};
enum enum_var_type
{
  SHOW_OPT_DEFAULT= 0, SHOW_OPT_SESSION, SHOW_OPT_GLOBAL
};
struct st_mysql_show_var {
  const char *name;
  void *value;
  enum enum_mysql_show_type type;
};
struct system_status_var;
typedef int (*mysql_show_var_func)(THD*, struct st_mysql_show_var*, void *, struct system_status_var *status_var, enum enum_var_type);
struct st_mysql_sys_var;
struct st_mysql_value;
typedef int (*mysql_var_check_func)(THD* thd,
                                    struct st_mysql_sys_var *var,
                                    void *save, struct st_mysql_value *value);
typedef void (*mysql_var_update_func)(THD* thd,
                                      struct st_mysql_sys_var *var,
                                      void *var_ptr, const void *save);
struct st_mysql_plugin
{
  int type;
  void *info;
  const char *name;
  const char *author;
  const char *descr;
  int license;
  int (*init)(void *);
  int (*deinit)(void *);
  unsigned int version;
  struct st_mysql_show_var *status_vars;
  struct st_mysql_sys_var **system_vars;
  void * __reserved1;
  unsigned long flags;
};
struct st_maria_plugin
{
  int type;
  void *info;
  const char *name;
  const char *author;
  const char *descr;
  int license;
  int (*init)(void *);
  int (*deinit)(void *);
  unsigned int version;
  struct st_mysql_show_var *status_vars;
  struct st_mysql_sys_var **system_vars;
  const char *version_info;
  unsigned int maturity;
};
extern "C" {
enum enum_ftparser_mode
{
  MYSQL_FTPARSER_SIMPLE_MODE= 0,
  MYSQL_FTPARSER_WITH_STOPWORDS= 1,
  MYSQL_FTPARSER_FULL_BOOLEAN_INFO= 2
};
enum enum_ft_token_type
{
  FT_TOKEN_EOF= 0,
  FT_TOKEN_WORD= 1,
  FT_TOKEN_LEFT_PAREN= 2,
  FT_TOKEN_RIGHT_PAREN= 3,
  FT_TOKEN_STOPWORD= 4
};
typedef struct st_mysql_ftparser_boolean_info
{
  enum enum_ft_token_type type;
  int yesno;
  int weight_adjust;
  char wasign;
  char trunc;
  char prev;
  char *quot;
} MYSQL_FTPARSER_BOOLEAN_INFO;
typedef struct st_mysql_ftparser_param
{
  int (*mysql_parse)(struct st_mysql_ftparser_param *,
                     const char *doc, int doc_len);
  int (*mysql_add_word)(struct st_mysql_ftparser_param *,
                        const char *word, int word_len,
                        MYSQL_FTPARSER_BOOLEAN_INFO *boolean_info);
  void *ftparser_state;
  void *mysql_ftparam;
  const struct charset_info_st *cs;
  const char *doc;
  int length;
  unsigned int flags;
  enum enum_ftparser_mode mode;
} MYSQL_FTPARSER_PARAM;
struct st_mysql_ftparser
{
  int interface_version;
  int (*parse)(MYSQL_FTPARSER_PARAM *param);
  int (*init)(MYSQL_FTPARSER_PARAM *param);
  int (*deinit)(MYSQL_FTPARSER_PARAM *param);
};
}
struct st_mysql_daemon
{
  int interface_version;
};
struct st_mysql_information_schema
{
  int interface_version;
};
struct st_mysql_storage_engine
{
  int interface_version;
};
struct handlerton;
 struct Mysql_replication {
   int interface_version;
 };
struct st_mysql_value
{
  int (*value_type)(struct st_mysql_value *);
  const char *(*val_str)(struct st_mysql_value *, char *buffer, int *length);
  int (*val_real)(struct st_mysql_value *, double *realbuf);
  int (*val_int)(struct st_mysql_value *, long long *intbuf);
  int (*is_unsigned)(struct st_mysql_value *);
};
extern "C" {
int thd_in_lock_tables(const THD* thd);
int thd_tablespace_op(const THD* thd);
long long thd_test_options(const THD* thd, long long test_options);
int thd_sql_command(const THD* thd);
struct DDL_options_st;
struct DDL_options_st *thd_ddl_options(const THD* thd);
void thd_storage_lock_wait(THD* thd, long long value);
int thd_tx_isolation(const THD* thd);
int thd_tx_is_read_only(const THD* thd);
int mysql_tmpfile(const char *prefix);
unsigned long thd_get_thread_id(const THD* thd);
void thd_get_xid(const THD* thd, MYSQL_XID *xid);
void mysql_query_cache_invalidate4(THD* thd,
                                   const char *key, unsigned int key_length,
                                   int using_trx);
void *thd_get_ha_data(const THD* thd, const struct handlerton *hton);
void thd_set_ha_data(THD* thd, const struct handlerton *hton,
                     const void *ha_data);
void thd_wakeup_subsequent_commits(THD* thd, int wakeup_error);
}
typedef struct st_plugin_vio_info
{
  enum { MYSQL_VIO_INVALID, MYSQL_VIO_TCP, MYSQL_VIO_SOCKET,
         MYSQL_VIO_PIPE, MYSQL_VIO_MEMORY } protocol;
  int socket;
} MYSQL_PLUGIN_VIO_INFO;
typedef struct st_plugin_vio
{
  int (*read_packet)(struct st_plugin_vio *vio,
                     unsigned char **buf);
  int (*write_packet)(struct st_plugin_vio *vio,
                      const unsigned char *packet,
                      int packet_len);
  void (*info)(struct st_plugin_vio *vio, struct st_plugin_vio_info *info);
} MYSQL_PLUGIN_VIO;
extern "C" {
typedef struct st_mysql_server_auth_info
{
  const char *user_name;
  unsigned int user_name_length;
  const char *auth_string;
  unsigned long auth_string_length;
  char authenticated_as[512 +1];
  char external_user[512 +1];
  int password_used;
  const char *host_or_ip;
  unsigned int host_or_ip_length;
  THD* thd;
} MYSQL_SERVER_AUTH_INFO;
struct st_mysql_auth
{
  int interface_version;
  const char *client_auth_plugin;
  int (*authenticate_user)(MYSQL_PLUGIN_VIO *vio, MYSQL_SERVER_AUTH_INFO *info);
  int (*hash_password)(const char *password, size_t password_length,
                       char *hash, size_t *hash_length);
  int (*preprocess_hash)(const char *hash, size_t hash_length,
                         unsigned char *out, size_t *out_length);
};
}
