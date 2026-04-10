
/*
 * Include file constants (processed in LibmysqlIncludeFiles.txt 1
 */
/* #undef HAVE_OPENSSL_APPLINK_C */
#define HAVE_evp_pkey 1
#define HAVE_ALLOCA_H 1
/* #undef HAVE_BIGENDIAN */
#define HAVE_SETLOCALE 1
#define HAVE_NL_LANGINFO 1
#define HAVE_DLFCN_H 1
#define HAVE_FCNTL_H 1
#define HAVE_FLOAT_H 1
#define HAVE_LIMITS_H 1
/* #undef HAVE_LINUX_LIMITS_H */
#define HAVE_PWD_H 1
/* #undef HAVE_SELECT_H */
#define HAVE_STDDEF_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_IOCTL_H 1
#define HAVE_SYS_SELECT_H 1
#define HAVE_SYS_SOCKET_H 1
/* #undef HAVE_SYS_STREAM_H */
#define HAVE_SYS_STAT_H 1
/* #undef HAVE_SYS_SYSCTL_H */
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_UN_H 1
#define HAVE_UNISTD_H 1
#define HAVE_UCONTEXT_H 1
/* #undef HAVE_BOOST_CONTEXT_H */

/*
 * function definitions - processed in LibmysqlFunctions.txt 
 */

#define HAVE_DLERROR 1
#define HAVE_DLOPEN 1
#define HAVE_GETPWUID 1
#define HAVE_MEMCPY 1
#define HAVE_POLL 1
#define HAVE_STRTOK_R 1
/* #undef HAVE_STRTOL */
#define HAVE_STRTOLL 1
#define HAVE_STRTOUL 1
#define HAVE_STRTOULL 1
/* #undef HAVE_TELL */
/* #undef HAVE_THR_SETCONCURRENCY */
/* #undef HAVE_THR_YIELD */
#define HAVE_VASPRINTF 1
#define HAVE_VSNPRINTF 1
/* #undef HAVE_CUSERID */

/*
 * types and sizes
 */


#define SIZEOF_CHARP 8
#if defined(SIZEOF_CHARP)
# define HAVE_CHARP 1
#endif


#define SIZEOF_INT 4
#if defined(SIZEOF_INT)
# define HAVE_INT 1
#endif

#define SIZEOF_LONG 8
#if defined(SIZEOF_LONG)
# define HAVE_LONG 1
#endif

#define SIZEOF_LONG_LONG 8
#if defined(SIZEOF_LONG_LONG)
# define HAVE_LONG_LONG 1
#endif


#define SIZEOF_SIZE_T 8
#if defined(SIZEOF_SIZE_T)
# define HAVE_SIZE_T 1
#endif


#define SIZEOF_UINT 4
#if defined(SIZEOF_UINT)
# define HAVE_UINT 1
#endif

/* #undef SIZEOF_USHORT */
#if defined(SIZEOF_USHORT)
# define HAVE_USHORT 1
#endif

/* #undef SIZEOF_ULONG */
#if defined(SIZEOF_ULONG)
# define HAVE_ULONG 1
#endif

/* #undef SIZEOF_INT8 */
#if defined(SIZEOF_INT8)
# define HAVE_INT8 1
#endif
/* #undef SIZEOF_UINT8 */
#if defined(SIZEOF_UINT8)
# define HAVE_UINT8 1
#endif

/* #undef SIZEOF_INT16 */
#if defined(SIZEOF_INT16)
# define HAVE_INT16 1
#endif
/* #undef SIZEOF_UINT16 */
#if defined(SIZEOF_UINT16)
# define HAVE_UINT16 1
#endif

/* #undef SIZEOF_INT32 */
#if defined(SIZEOF_INT32)
# define HAVE_INT32 1
#endif
/* #undef SIZEOF_UINT32 */
#if defined(SIZEOF_UINT32)
# define HAVE_UINT32 1
#endif

/* #undef SIZEOF_INT64 */
#if defined(SIZEOF_INT64)
# define HAVE_INT64 1
#endif
/* #undef SIZEOF_UINT64 */
#if defined(SIZEOF_UINT64)
# define HAVE_UINT64 1
#endif

/* #undef SIZEOF_SOCKLEN_T */
#if defined(SIZEOF_SOCKLEN_T)
# define HAVE_SOCKLEN_T 1
#endif

#define SOCKET_SIZE_TYPE socklen_t

#define LOCAL_INFILE_MODE_OFF  0
#define LOCAL_INFILE_MODE_ON   1
#define LOCAL_INFILE_MODE_AUTO 2
#define ENABLED_LOCAL_INFILE LOCAL_INFILE_MODE_AUTO

#define MARIADB_DEFAULT_CHARSET "utf8mb4"

#define DEFAULT_SSL_VERIFY_SERVER_CERT 1
