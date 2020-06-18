#include <compression/lzma.h>

#define COMPRESSION_BZIP2   1 << 0
#define COMPRESSION_LZ4     1 << 1
#define COMPRESSION_LZMA    1 << 2
#define COMPRESSION_LZO     1 << 3
#define COMPRESSION_SNAPPY  1 << 4
#define COMPRESSION_ZLIB    1 << 5
#define COMPRESSION_ZSTD    1 << 6
#define COMPRESSION_ALL     1 << 7


void init_compression(struct compression_service_lzma_st *);

void init_lzma(struct compression_service_lzma_st *, bool);
