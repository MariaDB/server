/* RISC-V Zbc accelerated CRC-32 core.

Zbc scalar path: clmul/clmulh inline asm + 128-bit 4-way parallel folding +
Barrett reduction (adapted from apache/brpc#3312).

This is the only compilation unit for which the ISA extension is enabled:
it is compiled with -march=rv64gc_zbc_zbb (see mysys/CMakeLists.txt), so that
the compiler may emit the carry-less multiplication instructions. The runtime
detection and dispatch live in crc32c_riscv.cc, which is compiled with the
normal settings, so that no function that must run on a CPU without Zbc can
be compiled for it (see MDEV-24745).

Two polynomials share this core: CRC-32C (Castagnoli, my_crc32c()) and the
ISO 3309 CRC-32 (my_checksum()). They differ only in their constants, which
are passed in as a table, mirroring how the x86 crc32_avx512() accepts a
const crc32_tab & parameter.
*/

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "assume_aligned.h"

namespace mysys_namespace {
namespace crc32c {

/* Constants of one CRC-32 polynomial, in bit-reflected form.
   See the use sites in rv_crc32_impl() for what each field is. */
struct rv_crc32_tab {
  alignas(16) uint32_t fold[4];  /* x^(64*i+64) mod P for i=1..4 */
  uint64_t const0;               /* x^64 mod P */
  uint64_t const1;               /* x^96 mod P */
  uint64_t quo;                  /* floor(x^64 / P) */
  uint64_t poly;                 /* P(x) true LE full */
  uint32_t bitwise_poly;         /* reflected polynomial, small-chunk path */
};

// RISC-V Zbc carry-less multiplication inline helpers
static inline uint64_t rv_clmul(uint64_t a, uint64_t b) {
  uint64_t result;
  __asm__ volatile ("clmul %0, %1, %2" : "=r"(result) : "r"(a), "r"(b));
  return result;
}

static inline uint64_t rv_clmulh(uint64_t a, uint64_t b) {
  uint64_t result;
  __asm__ volatile ("clmulh %0, %1, %2" : "=r"(result) : "r"(a), "r"(b));
  return result;
}

// Bitwise CRC fallback for small chunks
static inline uint32_t rv_crc32_bitwise(uint32_t crc, const uint8_t* buf,
                                        size_t len, uint32_t poly) {
  uint32_t c = crc;
  for (size_t i = 0; i < len; ++i) {
    c ^= buf[i];
    for (int k = 0; k < 8; ++k) {
      c = (c >> 1) ^ ((c & 1) ? poly : 0);
    }
  }
  return c;
}

// Fold the 128-bit CRC state (lo:hi) with fold constants k0/k1, then XOR in
// the value (v0:v1). The latter is either the next chunk of input data or
// another folded state to be combined.
static inline void rv_fold_pair(uint64_t* lo, uint64_t* hi,
                                uint64_t k0, uint64_t k1,
                                uint64_t v0, uint64_t v1) {
  uint64_t l = rv_clmul(*lo, k0) ^ rv_clmul(*hi, k1);
  uint64_t h = rv_clmulh(*lo, k0) ^ rv_clmulh(*hi, k1);
  *lo = l ^ v0;
  *hi = h ^ v1;
}

/* CRC-32C (Castagnoli), polynomial 0x1EDC6F41 */
static constexpr rv_crc32_tab rv_crc32c_tab= {
  { 0x740eef02, 0x9e4addf8, 0xf20c0dfe, 0x493c7d27 },
  0x00000000dd45aab8ULL, 0x00000000493c7d27ULL,
  0x0000000dea713f1ULL,  0x0000000105ec76f1ULL,
  0x82F63B78
};

/* ISO 3309 CRC-32 (Ethernet, zlib), polynomial 0x04C11DB7 */
static constexpr rv_crc32_tab rv_crc32_iso3309_tab= {
  { 0x8f352d95, 0x1d9513d7, 0xae689191, 0xccaa009e },
  0x00000000b8bc6765ULL, 0x00000000ccaa009eULL,
  0x0000001f7011641ULL,  0x0000001db710641ULL,
  0xEDB88320
};

constexpr uint64_t RV_CRC32_MASK32=      0x00000000FFFFFFFFULL;

// Hardware-accelerated CRC-32 using RISC-V Zbc carry-less multiplication.
// Processes data in 64-byte chunks with 128-bit folding, then Barrett reduces.
static uint32_t rv_crc32_impl(uint32_t crc, const char* buf, size_t len,
                              const rv_crc32_tab &tab) {
  // Convert external CRC to internal register state
  crc ^= 0xFFFFFFFF;

  const uint8_t* p = reinterpret_cast<const uint8_t*>(buf);
  size_t n = len;

  // Small data: use bitwise fallback
  if (n < 64) {
    return rv_crc32_bitwise(crc, p, n, tab.bitwise_poly) ^ 0xFFFFFFFF;
  }

  // Align to 16-byte boundary
  if (uintptr_t mis= reinterpret_cast<uintptr_t>(p) & 0xF) {
    size_t pre = 16 - mis;
    if (pre > n) pre = n;
    crc = rv_crc32_bitwise(crc, p, pre, tab.bitwise_poly);
    p += pre;
    n -= pre;
    if (n < 64) {
      return rv_crc32_bitwise(crc, p, n, tab.bitwise_poly) ^ 0xFFFFFFFF;
    }
  }

  // Load first 64 bytes and XOR CRC into the first 8 bytes.
  // p is 16-byte aligned here, hence memcpy_aligned<8> is safe.
  uint64_t x0, x1, y0, y1, z0, z1, w0, w1;
  memcpy_aligned<8>(&x0, p + 0, 8);
  memcpy_aligned<8>(&x1, p + 8, 8);
  memcpy_aligned<8>(&y0, p + 16, 8);
  memcpy_aligned<8>(&y1, p + 24, 8);
  memcpy_aligned<8>(&z0, p + 32, 8);
  memcpy_aligned<8>(&z1, p + 40, 8);
  memcpy_aligned<8>(&w0, p + 48, 8);
  memcpy_aligned<8>(&w1, p + 56, 8);

  x0 ^= (uint64_t)crc;
  p += 64;
  n -= 64;

  const uint64_t k1 = tab.fold[0];
  const uint64_t k2 = tab.fold[1];
  const uint64_t k3 = tab.fold[2];
  const uint64_t k4 = tab.fold[3];

  // Main loop: fold 64 bytes per iteration using 128-bit folding
  // p stays 16-byte aligned across iterations
  while (n >= 64) {
    uint64_t d0, d1;
    memcpy_aligned<8>(&d0, p + 0, 8);
    memcpy_aligned<8>(&d1, p + 8, 8);
    rv_fold_pair(&x0, &x1, k1, k2, d0, d1);
    memcpy_aligned<8>(&d0, p + 16, 8);
    memcpy_aligned<8>(&d1, p + 24, 8);
    rv_fold_pair(&y0, &y1, k1, k2, d0, d1);
    memcpy_aligned<8>(&d0, p + 32, 8);
    memcpy_aligned<8>(&d1, p + 40, 8);
    rv_fold_pair(&z0, &z1, k1, k2, d0, d1);
    memcpy_aligned<8>(&d0, p + 48, 8);
    memcpy_aligned<8>(&d1, p + 56, 8);
    rv_fold_pair(&w0, &w1, k1, k2, d0, d1);
    p += 64;
    n -= 64;
  }

  // Reduce 4x128-bit to 1x128-bit
  rv_fold_pair(&x0, &x1, k3, k4, y0, y1);
  rv_fold_pair(&x0, &x1, k3, k4, z0, z1);
  rv_fold_pair(&x0, &x1, k3, k4, w0, w1);

  // Barrett reduction: 128-bit -> 32-bit CRC
  uint64_t t4 = rv_clmul(x0, tab.const1);
  uint64_t t3 = rv_clmulh(x0, tab.const1);
  uint64_t t1 = x1 ^ t4;
  t4 = t1 & RV_CRC32_MASK32;
  t1 >>= 32;
  uint64_t t0 = rv_clmul(t4, tab.const0);
  t3 = (t3 << 32) ^ t1 ^ t0;

  t4 = t3 & RV_CRC32_MASK32;
  t4 = rv_clmul(t4, tab.quo);
  t4 &= RV_CRC32_MASK32;
  t4 = rv_clmul(t4, tab.poly);
  t4 ^= t3;

  uint32_t c = (uint32_t)((t4 >> 32) & RV_CRC32_MASK32);
  // Handle remaining bytes
  if (n) {
    c = rv_crc32_bitwise(c, p, n, tab.bitwise_poly);
  }
  // Convert internal register state to external CRC
  return c ^ 0xFFFFFFFF;
}

}  // namespace crc32c
}  // namespace mysys_namespace

extern "C" unsigned crc32c_riscv_zbc(unsigned crc, const void *buf,
                                     size_t size)
{
  return mysys_namespace::crc32c::rv_crc32_impl(crc,
                                        (const char *) buf, size,
                                        mysys_namespace::crc32c::rv_crc32c_tab);
}

extern "C" unsigned crc32_riscv_zbc(unsigned crc, const void *buf,
                                    size_t size)
{
  return mysys_namespace::crc32c::rv_crc32_impl(crc,
                                   (const char *) buf, size,
                                   mysys_namespace::crc32c::rv_crc32_iso3309_tab);
}