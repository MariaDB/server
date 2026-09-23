/* RISC-V Zvbc vector accelerated CRC-32C.

K-lane Zvbc implementation with *adaptive* vector length.  The CRC fold
itself keeps the exact 4-lane / 64-byte span of the scalar Zbc core (same
fold constants k1..k4, same Barrett math) so the result is bit-exact; the
vector length only decides how many e64m1 vector pairs carry the four
lanes in parallel (one pair of vl>=4 vectors on VLEN>=256, two pairs of
vl=2 vectors on VLEN=128).  A CPU with VLEN=128 therefore still executes
the vector path instead of being forced back to scalar code.

All carry-less multiplies run through the single-element vector vclmul
path (a scalar `clmul` would trap on cores that implement Zvbc but not
Zbc).  Small inputs, VLEN < 2 lanes, and the post-alignment remainder
fall back to the in-TU bitwise walk with the external CRC state.

This is the only compilation unit enabled for the vector extension; it is
compiled with -march=rv64gc_zbb_zvbc (see mysys/CMakeLists.txt).  zbc is
deliberately absent from that -march: with zbc present, GCC's
-foptimize-crc (on by default at -O2 since GCC 15) rewrites the bitwise
fallback below into scalar clmul/clmulh, which do not exist on the cores
this file is for.
*/

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#if defined(__riscv_zvbc)
#include <riscv_vector.h>
#endif

typedef unsigned (*my_crc32_t)(unsigned, const void *, size_t);

namespace mysys_namespace {
namespace crc32c {

struct rv_crc32_tab {
  alignas(16) uint32_t fold[4];
  uint64_t const0;
  uint64_t const1;
  uint64_t quo;
  uint64_t poly;
  uint32_t bitwise_poly;
};

/* The scalar Zbc `clmul` instruction is intentionally NOT used: some cores
   implement Zvbc but not B-ext scalar clmul.  Every CLMUL in this TU goes
   through the single-element vector vclmul path instead. */
static inline uint64_t rv_clmul(uint64_t a, uint64_t b) {
  const size_t vl = 1;
  vuint64m1_t va = __riscv_vmv_v_x_u64m1(a, vl);
  vuint64m1_t vb = __riscv_vmv_v_x_u64m1(b, vl);
  return __riscv_vmv_x_s_u64m1_u64(__riscv_vclmul_vv_u64m1(va, vb, vl));
}
static inline uint64_t rv_clmulh(uint64_t a, uint64_t b) {
  const size_t vl = 1;
  vuint64m1_t va = __riscv_vmv_v_x_u64m1(a, vl);
  vuint64m1_t vb = __riscv_vmv_v_x_u64m1(b, vl);
  return __riscv_vmv_x_s_u64m1_u64(__riscv_vclmulh_vv_u64m1(va, vb, vl));
}

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

static inline void rv_fold_pair(uint64_t* lo, uint64_t* hi,
                                uint64_t k0, uint64_t k1,
                                uint64_t v0, uint64_t v1) {
  uint64_t l = rv_clmul(*lo, k0) ^ rv_clmul(*hi, k1);
  uint64_t h = rv_clmulh(*lo, k0) ^ rv_clmulh(*hi, k1);
  *lo = l ^ v0;
  *hi = h ^ v1;
}

/* CRC-32C (Castagnoli), polynomial 0x1EDC6F41 -- same values as
   crc32c_riscv_zbc.cc, kept here for the vector core. */
static constexpr rv_crc32_tab rv_crc32c_tab = {
  { 0x740eef02, 0x9e4addf8, 0xf20c0dfe, 0x493c7d27 },
  0x00000000dd45aab8ULL, 0x00000000493c7d27ULL,
  0x0000000dea713f1ULL,  0x0000000105ec76f1ULL,
  0x82F63B78
};

static constexpr uint64_t RV_CRC32_MASK32 = 0x00000000FFFFFFFFULL;

#if defined(__riscv_zvbc)
static uint32_t rv_crc32c_zvbc_impl(uint32_t crc, const char* buf,
                                    size_t len, const rv_crc32_tab& tab) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(buf);
  size_t n = len;

  size_t max_vl = __riscv_vsetvlmax_e64m1();
  /* Lanes per vector pair; the CRC core always folds four lanes. */
  const size_t vl = max_vl >= 4 ? 4 : (max_vl >= 2 ? 2 : 0);
  const uint64_t chunk = 64; /* one fixed 64-byte fold span, Zbc-exact */

  crc ^= 0xFFFFFFFF; /* to internal state */

  if (n < chunk || vl == 0) {
    return rv_crc32_bitwise(crc, reinterpret_cast<const uint8_t*>(buf), len, tab.bitwise_poly) ^ 0xFFFFFFFF;
  }

  /* Align to 16-byte boundary (same bitwise walk as the Zbc core). */
  if (uintptr_t mis = reinterpret_cast<uintptr_t>(p) & 0xF) {
    size_t pre = 16 - mis;
    if (pre > n) pre = n;
    crc = rv_crc32_bitwise(crc, p, pre, tab.bitwise_poly);
    p += pre;
    n -= pre;
    if (n < chunk) {
      return rv_crc32_bitwise(crc, p, n, tab.bitwise_poly) ^ 0xFFFFFFFF;
    }
  }

  /* Four lanes, carried by `segs` e64m1 vector pairs:
       vl=4 (VLEN>=256): 1 pair, one vlseg2e64(4) reads the full 64 bytes
       vl=2 (VLEN=128):  2 pairs, each vlseg2e64(2) reads 32 bytes
     Lane j always owns bytes [16j, 16j+16) of the chunk, matching the
     scalar core's d0/d1 ... d6/d7 assignment. */
  const uint32_t segs = 4 / vl;

  /* RVV types cannot live in arrays with GCC, so the (at most two)
     vector pairs are held in named variables.  segs==4/vl: 1 pair for
     vl=4, 2 pairs for vl=2. */
  vuint64m1_t lo_a, hi_a, lo_b, hi_b;
  const size_t seg_bytes = vl * 16;
  {
    vuint64m1x2_t sg = __riscv_vlseg2e64_v_u64m1x2(
        reinterpret_cast<const uint64_t*>(p), vl);
    lo_a = __riscv_vget_v_u64m1x2_u64m1(sg, 0);
    hi_a = __riscv_vget_v_u64m1x2_u64m1(sg, 1);
  }
  if (segs == 2) {
    vuint64m1x2_t sg = __riscv_vlseg2e64_v_u64m1x2(
        reinterpret_cast<const uint64_t*>(p + seg_bytes), vl);
    lo_b = __riscv_vget_v_u64m1x2_u64m1(sg, 0);
    hi_b = __riscv_vget_v_u64m1x2_u64m1(sg, 1);
  }
  if (vl == 2) {
    /* Lane 0's low word is element 0 of the first pair. */
    uint64_t l0[2], h0[2];
    __riscv_vse64_v_u64m1(l0, lo_a, vl);
    __riscv_vse64_v_u64m1(h0, hi_a, vl);
    l0[0] ^= (uint64_t)crc;
    lo_a = __riscv_vle64_v_u64m1(l0, vl);
    hi_a = __riscv_vle64_v_u64m1(h0, vl);
  } else {
    uint64_t l0[4];
    __riscv_vse64_v_u64m1(l0, lo_a, vl);
    l0[0] ^= (uint64_t)crc;
    lo_a = __riscv_vle64_v_u64m1(l0, vl);
  }
  p += chunk;
  n -= chunk;

  const uint64_t k1 = tab.fold[0];
  const uint64_t k2 = tab.fold[1];

  while (n >= chunk) {
    vuint64m1x2_t sg = __riscv_vlseg2e64_v_u64m1x2(
        reinterpret_cast<const uint64_t*>(p), vl);
    vuint64m1_t dl = __riscv_vget_v_u64m1x2_u64m1(sg, 0);
    vuint64m1_t dh = __riscv_vget_v_u64m1x2_u64m1(sg, 1);
    vuint64m1_t lv = __riscv_vxor_vv_u64m1(
        __riscv_vclmul_vx_u64m1(lo_a, k1, vl),
        __riscv_vclmul_vx_u64m1(hi_a, k2, vl), vl);
    vuint64m1_t hv = __riscv_vxor_vv_u64m1(
        __riscv_vclmulh_vx_u64m1(lo_a, k1, vl),
        __riscv_vclmulh_vx_u64m1(hi_a, k2, vl), vl);
    lo_a = __riscv_vxor_vv_u64m1(lv, dl, vl);
    hi_a = __riscv_vxor_vv_u64m1(hv, dh, vl);
    if (segs == 2) {
      vuint64m1x2_t sg2 = __riscv_vlseg2e64_v_u64m1x2(
          reinterpret_cast<const uint64_t*>(p + seg_bytes), vl);
      vuint64m1_t dl2 = __riscv_vget_v_u64m1x2_u64m1(sg2, 0);
      vuint64m1_t dh2 = __riscv_vget_v_u64m1x2_u64m1(sg2, 1);
      vuint64m1_t lv2 = __riscv_vxor_vv_u64m1(
          __riscv_vclmul_vx_u64m1(lo_b, k1, vl),
          __riscv_vclmul_vx_u64m1(hi_b, k2, vl), vl);
      vuint64m1_t hv2 = __riscv_vxor_vv_u64m1(
          __riscv_vclmulh_vx_u64m1(lo_b, k1, vl),
          __riscv_vclmulh_vx_u64m1(hi_b, k2, vl), vl);
      lo_b = __riscv_vxor_vv_u64m1(lv2, dl2, vl);
      hi_b = __riscv_vxor_vv_u64m1(hv2, dh2, vl);
    }
    p += chunk;
    n -= chunk;
  }

  /* Merge the four lanes (same order as the scalar 4-way fold). */
  uint64_t loa[4], hia[4];
  __riscv_vse64_v_u64m1(loa, lo_a, vl);
  __riscv_vse64_v_u64m1(hia, hi_a, vl);
  if (segs == 2) {
    __riscv_vse64_v_u64m1(loa + 2, lo_b, vl);
    __riscv_vse64_v_u64m1(hia + 2, hi_b, vl);
  }
  const uint64_t k3 = tab.fold[2];
  const uint64_t k4 = tab.fold[3];
  uint64_t x0 = loa[0], x1 = hia[0];
  for (uint32_t j = 1; j < 4; ++j)
    rv_fold_pair(&x0, &x1, k3, k4, loa[j], hia[j]);

  /* Barrett reduction: 128-bit -> 32-bit (same as the Zbc core). */
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

  uint32_t c = static_cast<uint32_t>((t4 >> 32) & RV_CRC32_MASK32);
  if (n)
    c = rv_crc32_bitwise(c, p, n, tab.bitwise_poly);
  return c ^ 0xFFFFFFFF;
}
#endif /* __riscv_zvbc */

}  // namespace crc32c
}  // namespace mysys_namespace

extern "C" unsigned crc32c_riscv_zvbc(unsigned crc, const void *buf,
                                      size_t len)
{
#if defined(__riscv_zvbc)
  return mysys_namespace::crc32c::rv_crc32c_zvbc_impl(
      crc, reinterpret_cast<const char*>(buf), len,
      mysys_namespace::crc32c::rv_crc32c_tab);
#else
  return 0;
#endif
}
