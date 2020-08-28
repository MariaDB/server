/**
  @file include/compression/bzlib.h
  This service provides dynamic access to BZip2.
*/

#ifndef SERVICE_BZIP2_INCLUDED
#ifdef __cplusplus
extern "C" {
#endif

#ifndef MYSQL_ABI_CHECK
#include <stdbool.h>
#endif

extern bool COMPRESSION_LOADED_BZIP2;

#define BZ_RUN          0
#define BZ_FINISH       2

#define BZ_OK           0
#define BZ_RUN_OK       1
#define BZ_FINISH_OK    3
#define BZ_STREAM_END   4

typedef struct{
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
}
    bz_stream;

#define DEFINE_BZ2_bzBuffToBuffCompress(NAME) int NAME( \
    char *dest,                                         \
    unsigned int *destLen,                              \
    char *source,                                       \
    unsigned int sourceLen,                             \
    int blockSize100k,                                  \
    int verbosity,                                      \
    int workFactor                                      \
)

#define DEFINE_BZ2_bzBuffToBuffDecompress(NAME) int NAME(   \
    char *dest,                                             \
    unsigned int *destLen,                                  \
    char *source,                                           \
    unsigned int sourceLen,                                 \
    int small,                                              \
    int verbosity                                           \
)

#define DEFINE_BZ2_bzCompress(NAME) int NAME(   \
    bz_stream *strm,                            \
    int action                                  \
)

#define DEFINE_BZ2_bzCompressEnd(NAME) int NAME(    \
    bz_stream *strm                                 \
)

#define DEFINE_BZ2_bzCompressInit(NAME) int NAME(   \
    bz_stream *strm,                                \
    int blockSize100k,                              \
    int verbosity,                                  \
    int workFactor                                  \
)

#define DEFINE_BZ2_bzDecompress(NAME) int NAME( \
    bz_stream *strm                             \
)

#define DEFINE_BZ2_bzDecompressEnd(NAME) int NAME(  \
    bz_stream *strm                                 \
)

#define DEFINE_BZ2_bzDecompressInit(NAME) int NAME( \
    bz_stream *strm,                                \
    int verbosity,                                  \
    int small                                       \
)


typedef DEFINE_BZ2_bzBuffToBuffCompress   ((*PTR_BZ2_bzBuffToBuffCompress));
typedef DEFINE_BZ2_bzBuffToBuffDecompress ((*PTR_BZ2_bzBuffToBuffDecompress));
typedef DEFINE_BZ2_bzCompress             ((*PTR_BZ2_bzCompress));
typedef DEFINE_BZ2_bzCompressEnd          ((*PTR_BZ2_bzCompressEnd));
typedef DEFINE_BZ2_bzCompressInit         ((*PTR_BZ2_bzCompressInit));
typedef DEFINE_BZ2_bzDecompress           ((*PTR_BZ2_bzDecompress));
typedef DEFINE_BZ2_bzDecompressEnd        ((*PTR_BZ2_bzDecompressEnd));
typedef DEFINE_BZ2_bzDecompressInit       ((*PTR_BZ2_bzDecompressInit));


struct compression_service_bzip2_st{
    PTR_BZ2_bzBuffToBuffCompress   BZ2_bzBuffToBuffCompress_ptr;
    PTR_BZ2_bzBuffToBuffDecompress BZ2_bzBuffToBuffDecompress_ptr;
    PTR_BZ2_bzCompress             BZ2_bzCompress_ptr;
    PTR_BZ2_bzCompressEnd          BZ2_bzCompressEnd_ptr;
    PTR_BZ2_bzCompressInit         BZ2_bzCompressInit_ptr;
    PTR_BZ2_bzDecompress           BZ2_bzDecompress_ptr;
    PTR_BZ2_bzDecompressEnd        BZ2_bzDecompressEnd_ptr;
    PTR_BZ2_bzDecompressInit       BZ2_bzDecompressInit_ptr;
};

extern struct compression_service_bzip2_st *compression_service_bzip2;


#define BZ2_bzBuffToBuffCompress(...)   compression_service_bzip2->BZ2_bzBuffToBuffCompress_ptr   (__VA_ARGS__)
#define BZ2_bzBuffToBuffDecompress(...) compression_service_bzip2->BZ2_bzBuffToBuffDecompress_ptr (__VA_ARGS__)
#define BZ2_bzCompress(...)             compression_service_bzip2->BZ2_bzCompress_ptr             (__VA_ARGS__)
#define BZ2_bzCompressEnd(...)          compression_service_bzip2->BZ2_bzCompressEnd_ptr          (__VA_ARGS__)
#define BZ2_bzCompressInit(...)         compression_service_bzip2->BZ2_bzCompressInit_ptr         (__VA_ARGS__)
#define BZ2_bzDecompress(...)           compression_service_bzip2->BZ2_bzDecompress_ptr           (__VA_ARGS__)
#define BZ2_bzDecompressEnd(...)        compression_service_bzip2->BZ2_bzDecompressEnd_ptr        (__VA_ARGS__)
#define BZ2_bzDecompressInit(...)       compression_service_bzip2->BZ2_bzDecompressInit_ptr       (__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#define SERVICE_BZIP2_INCLUDED
#endif
