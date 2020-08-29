#include <compression/bzlib.h>
#include <compression/lz4.h>
#include <compression/lzma.h>
#include <compression/lzo/lzo1x.h>
#include <compression/snappy-c.h>
#include <compression/zstd.h>

#define COMPRESSION_BZIP2   1 << 0
#define COMPRESSION_LZ4     1 << 1
#define COMPRESSION_LZMA    1 << 2
#define COMPRESSION_LZO     1 << 3
#define COMPRESSION_SNAPPY  1 << 4
#define COMPRESSION_ZLIB    1 << 5
#define COMPRESSION_ZSTD    1 << 6
#define COMPRESSION_ALL     1 << 7


void init_compression(
    struct compression_service_bzip2_st  *,
    struct compression_service_lz4_st    *,
    struct compression_service_lzma_st   *,
    struct compression_service_lzo_st    *,
    struct compression_service_snappy_st *,
    struct compression_service_zstd_st   *
);

void init_bzip2  (struct compression_service_bzip2_st  *, bool);
void init_lz4    (struct compression_service_lz4_st    *, bool);
void init_lzma   (struct compression_service_lzma_st   *, bool);
void init_lzo    (struct compression_service_lzo_st    *, bool);
void init_snappy (struct compression_service_snappy_st *, bool);
void init_zstd   (struct compression_service_zstd_st   *, bool);
