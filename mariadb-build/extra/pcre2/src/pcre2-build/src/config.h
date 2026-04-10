/* config.h for CMake builds */

#define HAVE_ASSERT_H 1
/* #undef HAVE_BUILTIN_ASSUME */
#define HAVE_BUILTIN_MUL_OVERFLOW 1
#define HAVE_BUILTIN_UNREACHABLE 1
#define HAVE_ATTRIBUTE_UNINITIALIZED 1
#define HAVE_DIRENT_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
/* #undef HAVE_WINDOWS_H */

/* #undef HAVE_MEMFD_CREATE */
/* #undef HAVE_SECURE_GETENV */

#define SUPPORT_PCRE2_8 1
/* #undef SUPPORT_PCRE2_16 */
/* #undef SUPPORT_PCRE2_32 */
/* #undef DISABLE_PERCENT_ZT */

#define SUPPORT_LIBBZ2 1
/* #undef SUPPORT_LIBEDIT */
#define SUPPORT_LIBREADLINE 1
#define SUPPORT_LIBZ 1

/* #undef SUPPORT_JIT */
/* #undef SLJIT_PROT_EXECUTABLE_ALLOCATOR */
#define SUPPORT_PCRE2GREP_JIT 1
#define SUPPORT_PCRE2GREP_CALLOUT 1
#define SUPPORT_PCRE2GREP_CALLOUT_FORK 1
#define SUPPORT_UNICODE 1
/* #undef SUPPORT_VALGRIND */

/* #undef BSR_ANYCRLF */
/* #undef EBCDIC */
/* #undef EBCDIC_NL25 */
/* #undef EBCDIC_IGNORING_COMPILER */
/* #undef NEVER_BACKSLASH_C */

#define PCRE2_EXPORT            __attribute__ ((visibility ("default")))
#define LINK_SIZE               2
#define HEAP_LIMIT              20000000
#define MATCH_LIMIT             10000000
#define MATCH_LIMIT_DEPTH       MATCH_LIMIT
#define MAX_VARLOOKBEHIND       255
#define NEWLINE_DEFAULT         2
#define PARENS_NEST_LIMIT       250
#define PCRE2GREP_BUFSIZE       20480
#define PCRE2GREP_MAX_BUFSIZE   1048576

#define MAX_NAME_SIZE           128
#define MAX_NAME_COUNT          10000

/* end config.h for CMake builds */
