/**
  @file bzlib.h
  This service provides dynamic access to BZip2.
*/

#ifndef BZIP2_INCLUDED
#ifdef __cplusplus
extern "C" {
#endif

#ifndef MYSQL_ABI_CHECK
#include <stdbool.h>
#endif

#ifndef MYSQL_DYNAMIC_PLUGIN
#define provider_service_bzip2 provider_service_bzip2_static
#endif

#ifndef BZ_RUN
#define BZ_RUN          0
#define BZ_FINISH       2

#define BZ_OK           0
#define BZ_RUN_OK       1
#define BZ_FINISH_OK    3
#define BZ_STREAM_END   4

typedef struct
{
  char *next_in;
  unsigned int avail_in;
  unsigned int total_in_lo32;
  unsigned int total_in_hi32;

  char *next_out;
  unsigned int avail_out;
  unsigned int total_out_lo32;
  unsigned int total_out_hi32;

  void *state;

  void *(*bzalloc)(void *, int, int);
  void (*bzfree)(void *, void *);
  void *opaque;
} bz_stream;

#define BZ2_bzBuffToBuffCompress(...)   provider_service_bzip2->BZ2_bzBuffToBuffCompress_ptr   (__VA_ARGS__)
#define BZ2_bzBuffToBuffDecompress(...) provider_service_bzip2->BZ2_bzBuffToBuffDecompress_ptr (__VA_ARGS__)
#define BZ2_bzCompress(...)             provider_service_bzip2->BZ2_bzCompress_ptr             (__VA_ARGS__)
#define BZ2_bzCompressEnd(...)          provider_service_bzip2->BZ2_bzCompressEnd_ptr          (__VA_ARGS__)
#define BZ2_bzCompressInit(...)         provider_service_bzip2->BZ2_bzCompressInit_ptr         (__VA_ARGS__)
#define BZ2_bzDecompress(...)           provider_service_bzip2->BZ2_bzDecompress_ptr           (__VA_ARGS__)
#define BZ2_bzDecompressEnd(...)        provider_service_bzip2->BZ2_bzDecompressEnd_ptr        (__VA_ARGS__)
#define BZ2_bzDecompressInit(...)       provider_service_bzip2->BZ2_bzDecompressInit_ptr       (__VA_ARGS__)
#endif

#define DEFINE_BZ2_bzBuffToBuffCompress(NAME) NAME( \
    char *dest,                                     \
    unsigned int *destLen,                          \
    char *source,                                   \
    unsigned int sourceLen,                         \
    int blockSize100k,                              \
    int verbosity,                                  \
    int workFactor                                  \
)

#define DEFINE_BZ2_bzBuffToBuffDecompress(NAME) NAME( \
    char *dest,                                       \
    unsigned int *destLen,                            \
    char *source,                                     \
    unsigned int sourceLen,                           \
    int small,                                        \
    int verbosity                                     \
)

#define DEFINE_BZ2_bzCompress(NAME) NAME(   \
    bz_stream *strm,                        \
    int action                              \
)

#define DEFINE_BZ2_bzCompressEnd(NAME) NAME(    \
    bz_stream *strm                             \
)

#define DEFINE_BZ2_bzCompressInit(NAME) NAME(   \
    bz_stream *strm,                            \
    int blockSize100k,                          \
    int verbosity,                              \
    int workFactor                              \
)

#define DEFINE_BZ2_bzDecompress(NAME) NAME( \
    bz_stream *strm                         \
)

#define DEFINE_BZ2_bzDecompressEnd(NAME) NAME(  \
    bz_stream *strm                             \
)

#define DEFINE_BZ2_bzDecompressInit(NAME) NAME( \
    bz_stream *strm,                            \
    int verbosity,                              \
    int small                                   \
)

struct provider_service_bzip2_st{
  int DEFINE_BZ2_bzBuffToBuffCompress((*BZ2_bzBuffToBuffCompress_ptr));
  int DEFINE_BZ2_bzBuffToBuffDecompress((*BZ2_bzBuffToBuffDecompress_ptr));
  int DEFINE_BZ2_bzCompress((*BZ2_bzCompress_ptr));
  int DEFINE_BZ2_bzCompressEnd((*BZ2_bzCompressEnd_ptr));
  int DEFINE_BZ2_bzCompressInit((*BZ2_bzCompressInit_ptr));
  int DEFINE_BZ2_bzDecompress((*BZ2_bzDecompress_ptr));
  int DEFINE_BZ2_bzDecompressEnd((*BZ2_bzDecompressEnd_ptr));
  int DEFINE_BZ2_bzDecompressInit((*BZ2_bzDecompressInit_ptr));

  bool is_loaded;
};

extern struct provider_service_bzip2_st *provider_service_bzip2;

#ifdef __cplusplus
}
#endif

#define BZIP2_INCLUDED
#endif
