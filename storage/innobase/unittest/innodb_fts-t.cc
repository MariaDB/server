#include "tap.h"
#include "fts0fts.h"
#include "fts0vlc.h"

struct fts_encode_info
{
  const byte buf[10];
  size_t len;
  doc_id_t val;
};

/* Contains fts encoding min & max value for each length bytes */
static const fts_encode_info fts_info[]=
{
  {{0x80}, 1, 0},
  {{0xFF}, 1, (1 << 7) - 1},
  {{0x01, 0x80}, 2, 1 << 7},
  {{0x7F, 0XFF}, 2, (1 << 14) - 1},
  {{0x01, 0x00, 0x80}, 3, 1 << 14},
  {{0x7F, 0X7F, 0XFF}, 3, (1 << 21) - 1},
  {{0x01, 0x00, 0x00, 0x80}, 4, 1 << 21},
  {{0x7F, 0X7F, 0X7F, 0xFF}, 4, (1 << 28) - 1},
  {{0x01, 0x00, 0x00, 0x00, 0x80}, 5, 1 << 28},
  {{0x7F, 0X7F, 0X7F, 0x7F, 0xFF}, 5, (1ULL << 35) - 1},
  {{0x01, 0x00, 0x00, 0x00, 0x00, 0x80}, 6, 1ULL << 35},
  {{0x7F, 0X7F, 0X7F, 0x7F, 0x7F, 0xFF}, 6, (1ULL << 42) - 1},
  {{0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80}, 7, 1ULL << 42},
  {{0x7F, 0X7F, 0X7F, 0x7F, 0x7F, 0x7F, 0XFF}, 7, (1ULL << 49) - 1},
  {{0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80}, 8, 1ULL << 49},
  {{0x7F, 0X7F, 0X7F, 0x7F, 0x7F, 0x7F, 0X7F, 0XFF}, 8, (1ULL << 56) -1},
  {{0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80}, 9, 1ULL << 56},
  {{0x7F, 0X7F, 0X7F, 0x7F, 0x7F, 0x7F, 0X7F, 0x7F, 0XFF}, 9, (1ULL << 63) -1},
  {{0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80}, 10, 1ULL << 63},
  {{0x01, 0X7F, 0X7F, 0x7F, 0x7F, 0x7F, 0X7F, 0x7F, 0x7F, 0xFF}, 10, ~0ULL}
};

int main(int, char**)
{
  for (int i= array_elements(fts_info); i--;)
  {
    byte buf[10];
    const byte* fts_buf= buf;
    size_t len= fts_encode_int(fts_info[i].val, buf) - &buf[0];
    if (fts_info[i].len == len &&
        !memcmp(&fts_info[i].buf, buf, len) &&
        fts_decode_vlc(&fts_buf) == fts_info[i].val &&
        fts_buf == &buf[len])
      ok(true, "FTS Encoded for %zu bytes", fts_info[i].len);
    else
      ok(false, "FTS Encoded for %zu bytes", fts_info[i].len);
  }
}
