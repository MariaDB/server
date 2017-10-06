typedef char my_bool;
typedef void * MYSQL_PLUGIN;
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
extern void (*debug_sync_C_callback_ptr)(void*, const char *, size_t);
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
enum thd_kill_levels {
  THD_IS_NOT_KILLED=0,
  THD_ABORT_SOFTLY=50,
  THD_ABORT_ASAP=100,
};
extern struct kill_statement_service_st {
  enum thd_kill_levels (*thd_kill_level_func)(const void*);
} *thd_kill_statement_service;
enum thd_kill_levels thd_kill_level(const void*);
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
extern struct my_print_error_service_st {
  void (*my_error_func)(unsigned int nr, unsigned long MyFlags, ...);
  void (*my_printf_error_func)(unsigned int nr, const char *fmt, unsigned long MyFlags,...);
  void (*my_printv_error_func)(unsigned int error, const char *format, unsigned long MyFlags, va_list ap);
} *my_print_error_service;
extern void my_error(unsigned int nr, unsigned long MyFlags, ...);
extern void my_printf_error(unsigned int my_err, const char *format, unsigned long MyFlags, ...);
extern void my_printv_error(unsigned int error, const char *format, unsigned long MyFlags,va_list ap);
extern struct my_snprintf_service_st {
  size_t (*my_snprintf_type)(char*, size_t, const char*, ...);
  size_t (*my_vsnprintf_type)(char *, size_t, const char*, va_list);
} *my_snprintf_service;
size_t my_snprintf(char* to, size_t n, const char* fmt, ...);
size_t my_vsnprintf(char *to, size_t n, const char* fmt, va_list ap);
extern struct progress_report_service_st {
  void (*thd_progress_init_func)(void* thd, unsigned int max_stage);
  void (*thd_progress_report_func)(void* thd,
                                   unsigned long long progress,
                                   unsigned long long max_progress);
  void (*thd_progress_next_stage_func)(void* thd);
  void (*thd_progress_end_func)(void* thd);
  const char *(*set_thd_proc_info_func)(void*, const char *info,
                                        const char *func,
                                        const char *file,
                                        unsigned int line);
} *progress_report_service;
void thd_progress_init(void* thd, unsigned int max_stage);
void thd_progress_report(void* thd,
                         unsigned long long progress,
                         unsigned long long max_progress);
void thd_progress_next_stage(void* thd);
void thd_progress_end(void* thd);
const char *set_thd_proc_info(void*, const char * info, const char *func,
                              const char *file, unsigned int line);
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
struct st_mysql_lex_string
{
  char *str;
  size_t length;
};
typedef struct st_mysql_lex_string MYSQL_LEX_STRING;
extern struct thd_alloc_service_st {
  void *(*thd_alloc_func)(void*, size_t);
  void *(*thd_calloc_func)(void*, size_t);
  char *(*thd_strdup_func)(void*, const char *);
  char *(*thd_strmake_func)(void*, const char *, size_t);
  void *(*thd_memdup_func)(void*, const void*, size_t);
  MYSQL_LEX_STRING *(*thd_make_lex_string_func)(void*, MYSQL_LEX_STRING *,
                                        const char *, size_t, int);
} *thd_alloc_service;
void *thd_alloc(void* thd, size_t size);
void *thd_calloc(void* thd, size_t size);
char *thd_strdup(void* thd, const char *str);
char *thd_strmake(void* thd, const char *str, size_t size);
void *thd_memdup(void* thd, const void* str, size_t size);
MYSQL_LEX_STRING *thd_make_lex_string(void* thd, MYSQL_LEX_STRING *lex_str,
                                      const char *str, size_t size,
                                      int allocate_lex_string);
extern struct thd_autoinc_service_st {
  void (*thd_get_autoinc_func)(const void* thd,
                               unsigned long* off, unsigned long* inc);
} *thd_autoinc_service;
void thd_get_autoinc(const void* thd,
                     unsigned long* off, unsigned long* inc);
extern struct thd_error_context_service_st {
  const char *(*thd_get_error_message_func)(const void* thd);
  unsigned int (*thd_get_error_number_func)(const void* thd);
  unsigned long (*thd_get_error_row_func)(const void* thd);
  void (*thd_inc_error_row_func)(void* thd);
  char *(*thd_get_error_context_description_func)(void* thd,
                                                  char *buffer,
                                                  unsigned int length,
                                                  unsigned int max_query_length);
} *thd_error_context_service;
const char *thd_get_error_message(const void* thd);
unsigned int thd_get_error_number(const void* thd);
unsigned long thd_get_error_row(const void* thd);
void thd_inc_error_row(void* thd);
char *thd_get_error_context_description(void* thd,
                                        char *buffer, unsigned int length,
                                        unsigned int max_query_length);
extern struct thd_rnd_service_st {
  double (*thd_rnd_ptr)(void* thd);
  void (*thd_c_r_p_ptr)(void* thd, char *to, size_t length);
} *thd_rnd_service;
double thd_rnd(void* thd);
void thd_create_random_password(void* thd, char *to, size_t length);
typedef int MYSQL_THD_KEY_T;
extern struct thd_specifics_service_st {
  int (*thd_key_create_func)(MYSQL_THD_KEY_T *key);
  void (*thd_key_delete_func)(MYSQL_THD_KEY_T *key);
  void *(*thd_getspecific_func)(void* thd, MYSQL_THD_KEY_T key);
  int (*thd_setspecific_func)(void* thd, MYSQL_THD_KEY_T key, void *value);
} *thd_specifics_service;
int thd_key_create(MYSQL_THD_KEY_T *key);
void thd_key_delete(MYSQL_THD_KEY_T *key);
void* thd_getspecific(void* thd, MYSQL_THD_KEY_T key);
int thd_setspecific(void* thd, MYSQL_THD_KEY_T key, void *value);
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
extern struct thd_timezone_service_st {
  my_time_t (*thd_TIME_to_gmt_sec)(void* thd, const MYSQL_TIME *ltime, unsigned int *errcode);
  void (*thd_gmt_sec_to_TIME)(void* thd, MYSQL_TIME *ltime, my_time_t t);
} *thd_timezone_service;
my_time_t thd_TIME_to_gmt_sec(void* thd, const MYSQL_TIME *ltime, unsigned int *errcode);
void thd_gmt_sec_to_TIME(void* thd, MYSQL_TIME *ltime, my_time_t t);
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
  void (*thd_wait_begin_func)(void*, int);
  void (*thd_wait_end_func)(void*);
} *thd_wait_service;
void thd_wait_begin(void* thd, int wait_type);
void thd_wait_end(void* thd);
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
  SHOW_always_last
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
typedef int (*mysql_show_var_func)(void*, struct st_mysql_show_var*, void *, struct system_status_var *status_var, enum enum_var_type);
struct st_mysql_sys_var;
struct st_mysql_value;
typedef int (*mysql_var_check_func)(void* thd,
                                    struct st_mysql_sys_var *var,
                                    void *save, struct st_mysql_value *value);
typedef void (*mysql_var_update_func)(void* thd,
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
int thd_in_lock_tables(const void* thd);
int thd_tablespace_op(const void* thd);
long long thd_test_options(const void* thd, long long test_options);
int thd_sql_command(const void* thd);
void **thd_ha_data(const void* thd, const struct handlerton *hton);
void thd_storage_lock_wait(void* thd, long long value);
int thd_tx_isolation(const void* thd);
int thd_tx_is_read_only(const void* thd);
int thd_rpl_is_parallel(const void* thd);
int mysql_tmpfile(const char *prefix);
unsigned long thd_get_thread_id(const void* thd);
void thd_get_xid(const void* thd, MYSQL_XID *xid);
void mysql_query_cache_invalidate4(void* thd,
                                   const char *key, unsigned int key_length,
                                   int using_trx);
void *thd_get_ha_data(const void* thd, const struct handlerton *hton);
void thd_set_ha_data(void* thd, const struct handlerton *hton,
                     const void *ha_data);
void thd_wakeup_subsequent_commits(void* thd, int wakeup_error);
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
