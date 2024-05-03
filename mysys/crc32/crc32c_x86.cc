#include <my_global.h>
#include <cstddef>
#include <cstdint>

#ifdef _MSC_VER
# include <intrin.h>
#else
# include <cpuid.h>
#endif

extern "C" unsigned crc32c_sse42(unsigned crc, const void* buf, size_t size);

constexpr uint32_t cpuid_ecx_SSE42= 1U << 20;
constexpr uint32_t cpuid_ecx_SSE42_AND_PCLMUL= cpuid_ecx_SSE42 | 1U << 1;

static uint32_t cpuid_ecx()
{
#ifdef __GNUC__
  uint32_t reax= 0, rebx= 0, recx= 0, redx= 0;
  __cpuid(1, reax, rebx, recx, redx);
  return recx;
#elif defined _MSC_VER
  int regs[4];
  __cpuid(regs, 1);
  return regs[2];
#else
# error "unknown compiler"
#endif
}

typedef unsigned (*my_crc32_t)(unsigned, const void *, size_t);
extern "C" unsigned int crc32_pclmul(unsigned int, const void *, size_t);
extern "C" unsigned int crc32c_3way(unsigned int, const void *, size_t);

extern "C" my_crc32_t crc32_pclmul_enabled(void)
{
  if (~cpuid_ecx() & cpuid_ecx_SSE42_AND_PCLMUL)
    return nullptr;
  return crc32_pclmul;
}

extern "C" my_crc32_t crc32c_x86_available(void)
{
#if SIZEOF_SIZE_T == 8
  switch (cpuid_ecx() & cpuid_ecx_SSE42_AND_PCLMUL) {
  case cpuid_ecx_SSE42_AND_PCLMUL:
    return crc32c_3way;
  case cpuid_ecx_SSE42:
    return crc32c_sse42;
  }
#else
  if (cpuid_ecx() & cpuid_ecx_SSE42)
    return crc32c_sse42;
#endif
  return nullptr;
}

extern "C" const char *crc32c_x86_impl(my_crc32_t c)
{
#if SIZEOF_SIZE_T == 8
  if (c == crc32c_3way)
    return "Using crc32 + pclmulqdq instructions";
#endif
  if (c == crc32c_sse42)
    return "Using SSE4.2 crc32 instructions";
  return nullptr;
}
