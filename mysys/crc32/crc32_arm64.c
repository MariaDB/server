#include <my_global.h>
#include <string.h>
#include <stdint.h>

static int pmull_supported;

#if defined(HAVE_ARMV8_CRC)

#if defined(__APPLE__)
#include <sys/sysctl.h>

int crc32_aarch64_available(void)
{
  int ret;
  size_t len = sizeof(ret);
  if (sysctlbyname("hw.optional.armv8_crc32", &ret, &len, NULL, 0) == -1)
    return 0;
  return ret;
}

const char *crc32c_aarch64_available(void)
{
  if (crc32_aarch64_available() == 0)
    return NULL;
  pmull_supported = 1;
  return "Using ARMv8 crc32 + pmull instructions";
}

#else
#include <sys/auxv.h>
#if defined(__FreeBSD__)
static unsigned long getauxval(unsigned int key)
{
  unsigned long val;
  if (elf_aux_info(key, (void *)&val, (int)sizeof(val) != 0)
    return 0ul;
  return val;
}
#else
# include <asm/hwcap.h>
#endif

#ifndef HWCAP_CRC32
# define HWCAP_CRC32 (1 << 7)
#endif

#ifndef HWCAP_PMULL
# define HWCAP_PMULL (1 << 4)
#endif

/* ARM made crc32 default from ARMv8.1 but optional in ARMv8A
 * Runtime check API.
 */
int crc32_aarch64_available(void)
{
  unsigned long auxv= getauxval(AT_HWCAP);
  return (auxv & HWCAP_CRC32) != 0;
}

const char *crc32c_aarch64_available(void)
{
  unsigned long auxv= getauxval(AT_HWCAP);

  if (!(auxv & HWCAP_CRC32))
    return NULL;

  pmull_supported= (auxv & HWCAP_PMULL) != 0;
  if (pmull_supported)
    return "Using ARMv8 crc32 + pmull instructions";
  else
    return "Using ARMv8 crc32 instructions";
}

#endif /* __APPLE__ */
#endif /* HAVE_ARMV8_CRC */

#ifndef HAVE_ARMV8_CRC_CRYPTO_INTRINSICS

/* Request crc extension capabilities from the assembler */
asm(".arch_extension crc");

# ifdef HAVE_ARMV8_CRYPTO
/* crypto extension  */
asm(".arch_extension crypto");
# endif

#define CRC32CX(crc, value) __asm__("crc32cx %w[c], %w[c], %x[v]":[c]"+r"(crc):[v]"r"(value))
#define CRC32CW(crc, value) __asm__("crc32cw %w[c], %w[c], %w[v]":[c]"+r"(crc):[v]"r"(value))
#define CRC32CH(crc, value) __asm__("crc32ch %w[c], %w[c], %w[v]":[c]"+r"(crc):[v]"r"(value))
#define CRC32CB(crc, value) __asm__("crc32cb %w[c], %w[c], %w[v]":[c]"+r"(crc):[v]"r"(value))

#define CRC32X(crc, value) __asm__("crc32x %w[c], %w[c], %x[v]":[c]"+r"(crc):[v]"r"(value))
#define CRC32W(crc, value) __asm__("crc32w %w[c], %w[c], %w[v]":[c]"+r"(crc):[v]"r"(value))
#define CRC32H(crc, value) __asm__("crc32h %w[c], %w[c], %w[v]":[c]"+r"(crc):[v]"r"(value))
#define CRC32B(crc, value) __asm__("crc32b %w[c], %w[c], %w[v]":[c]"+r"(crc):[v]"r"(value))


#define CRC32C3X8(buffer, ITR) \
  __asm__("crc32cx %w[c1], %w[c1], %x[v]":[c1]"+r"(crc1):[v]"r"(*((const uint64_t *)buffer + 42*1 + (ITR))));\
  __asm__("crc32cx %w[c2], %w[c2], %x[v]":[c2]"+r"(crc2):[v]"r"(*((const uint64_t *)buffer + 42*2 + (ITR))));\
  __asm__("crc32cx %w[c0], %w[c0], %x[v]":[c0]"+r"(crc0):[v]"r"(*((const uint64_t *)buffer + 42*0 + (ITR))));

#else /* HAVE_ARMV8_CRC_CRYPTO_INTRINSICS  */

/* Intrinsics header*/
#include <arm_acle.h>
#include <arm_neon.h>

#define CRC32CX(crc, value) (crc) = __crc32cd((crc), (value))
#define CRC32CW(crc, value) (crc) = __crc32cw((crc), (value))
#define CRC32CH(crc, value) (crc) = __crc32ch((crc), (value))
#define CRC32CB(crc, value) (crc) = __crc32cb((crc), (value))

#define CRC32X(crc, value) (crc) = __crc32d((crc), (value))
#define CRC32W(crc, value) (crc) = __crc32w((crc), (value))
#define CRC32H(crc, value) (crc) = __crc32h((crc), (value))
#define CRC32B(crc, value) (crc) = __crc32b((crc), (value))

#define CRC32C3X8(buffer, ITR) \
  crc1 = __crc32cd(crc1, *((const uint64_t *)buffer + 42*1 + (ITR)));\
  crc2 = __crc32cd(crc2, *((const uint64_t *)buffer + 42*2 + (ITR)));\
  crc0 = __crc32cd(crc0, *((const uint64_t *)buffer + 42*0 + (ITR)));

#endif /* HAVE_ARMV8_CRC_CRYPTO_INTRINSICS */

#define CRC32C7X3X8(buffer, ITR) do {\
  CRC32C3X8(buffer, ((ITR) * 7 + 0)) \
  CRC32C3X8(buffer, ((ITR) * 7 + 1)) \
  CRC32C3X8(buffer, ((ITR) * 7 + 2)) \
  CRC32C3X8(buffer, ((ITR) * 7 + 3)) \
  CRC32C3X8(buffer, ((ITR) * 7 + 4)) \
  CRC32C3X8(buffer, ((ITR) * 7 + 5)) \
  CRC32C3X8(buffer, ((ITR) * 7 + 6)) \
} while(0)

#define PREF4X64L1(buffer, PREF_OFFSET, ITR) \
  __asm__("PRFM PLDL1KEEP, [%x[v],%[c]]"::[v]"r"(buffer), [c]"I"((PREF_OFFSET) + ((ITR) + 0)*64));\
  __asm__("PRFM PLDL1KEEP, [%x[v],%[c]]"::[v]"r"(buffer), [c]"I"((PREF_OFFSET) + ((ITR) + 1)*64));\
  __asm__("PRFM PLDL1KEEP, [%x[v],%[c]]"::[v]"r"(buffer), [c]"I"((PREF_OFFSET) + ((ITR) + 2)*64));\
  __asm__("PRFM PLDL1KEEP, [%x[v],%[c]]"::[v]"r"(buffer), [c]"I"((PREF_OFFSET) + ((ITR) + 3)*64));

#define PREF1KL1(buffer, PREF_OFFSET) \
  PREF4X64L1(buffer,(PREF_OFFSET), 0) \
  PREF4X64L1(buffer,(PREF_OFFSET), 4) \
  PREF4X64L1(buffer,(PREF_OFFSET), 8) \
  PREF4X64L1(buffer,(PREF_OFFSET), 12)

#define PREF4X64L2(buffer, PREF_OFFSET, ITR) \
  __asm__("PRFM PLDL2KEEP, [%x[v],%[c]]"::[v]"r"(buffer), [c]"I"((PREF_OFFSET) + ((ITR) + 0)*64));\
  __asm__("PRFM PLDL2KEEP, [%x[v],%[c]]"::[v]"r"(buffer), [c]"I"((PREF_OFFSET) + ((ITR) + 1)*64));\
  __asm__("PRFM PLDL2KEEP, [%x[v],%[c]]"::[v]"r"(buffer), [c]"I"((PREF_OFFSET) + ((ITR) + 2)*64));\
  __asm__("PRFM PLDL2KEEP, [%x[v],%[c]]"::[v]"r"(buffer), [c]"I"((PREF_OFFSET) + ((ITR) + 3)*64));

#define PREF1KL2(buffer, PREF_OFFSET) \
  PREF4X64L2(buffer,(PREF_OFFSET), 0) \
  PREF4X64L2(buffer,(PREF_OFFSET), 4) \
  PREF4X64L2(buffer,(PREF_OFFSET), 8) \
  PREF4X64L2(buffer,(PREF_OFFSET), 12)

uint32_t crc32c_aarch64(uint32_t crc, const unsigned char *buffer, uint64_t len)
{
  uint32_t crc0, crc1, crc2;
  int64_t length= (int64_t)len;

  crc^= 0xffffffff;

  /* Pmull runtime check here.
   * Raspberry Pi 4 supports crc32 but doesn't support pmull (MDEV-23030).
   *
   * Consider the condition that the target platform does support hardware crc32
   * but not support PMULL. In this condition, it should leverage the aarch64
   * crc32 instruction (__crc32c) and just only skip parallel computation (pmull/vmull)
   * rather than skip all hardware crc32 instruction of computation.
   */
  if (pmull_supported)
  {
/* The following Macro (HAVE_ARMV8_CRYPTO) is used for compiling check */
#ifdef HAVE_ARMV8_CRYPTO

/* Crypto extension Support
 * Parallel computation with 1024 Bytes (per block)
 * Intrinsics Support
 */
# ifdef HAVE_ARMV8_CRC_CRYPTO_INTRINSICS
    const poly64_t k1= 0xe417f38a, k2= 0x8f158014;
    uint64_t t0, t1;

    /* Process per block size of 1024 Bytes
     * A block size = 8 + 42*3*sizeof(uint64_t) + 8
     */
    while ((length-= 1024) >= 0)
    {
      /* Prefetch 3*1024 data for avoiding L2 cache miss */
      PREF1KL2(buffer, 1024*3);
      /* Do first 8 bytes here for better pipelining */
      crc0= __crc32cd(crc, *(const uint64_t *)buffer);
      crc1= 0;
      crc2= 0;
      buffer+= sizeof(uint64_t);

      /* Process block inline
       * Process crc0 last to avoid dependency with above
       */
      CRC32C7X3X8(buffer, 0);
      CRC32C7X3X8(buffer, 1);
      CRC32C7X3X8(buffer, 2);
      CRC32C7X3X8(buffer, 3);
      CRC32C7X3X8(buffer, 4);
      CRC32C7X3X8(buffer, 5);

      buffer+= 42*3*sizeof(uint64_t);
      /* Prefetch data for following block to avoid L1 cache miss */
      PREF1KL1(buffer, 1024);

      /* Last 8 bytes
       * Merge crc0 and crc1 into crc2
       * crc1 multiply by K2
       * crc0 multiply by K1
       */
      t1= (uint64_t)vmull_p64(crc1, k2);
      t0= (uint64_t)vmull_p64(crc0, k1);
      crc= __crc32cd(crc2, *(const uint64_t *)buffer);
      crc1= __crc32cd(0, t1);
      crc^= crc1;
      crc0= __crc32cd(0, t0);
      crc^= crc0;

      buffer+= sizeof(uint64_t);
    }

# else /* HAVE_ARMV8_CRC_CRYPTO_INTRINSICS */

    /*No intrinsics*/
    __asm__("mov    x16,            #0xf38a         \n\t"
            "movk   x16,            #0xe417, lsl 16 \n\t"
            "mov    v1.2d[0],       x16             \n\t"
            "mov    x16,            #0x8014         \n\t"
            "movk   x16,            #0x8f15, lsl 16 \n\t"
            "mov    v0.2d[0],       x16             \n\t"
            :::"x16");

    while ((length-= 1024) >= 0)
    {
      PREF1KL2(buffer, 1024*3);
      __asm__("crc32cx %w[c0], %w[c], %x[v]\n\t"
              :[c0]"=r"(crc0):[c]"r"(crc), [v]"r"(*(const uint64_t *)buffer):);
      crc1= 0;
      crc2= 0;
      buffer+= sizeof(uint64_t);

      CRC32C7X3X8(buffer, 0);
      CRC32C7X3X8(buffer, 1);
      CRC32C7X3X8(buffer, 2);
      CRC32C7X3X8(buffer, 3);
      CRC32C7X3X8(buffer, 4);
      CRC32C7X3X8(buffer, 5);

      buffer+= 42*3*sizeof(uint64_t);
      PREF1KL1(buffer, 1024);
      __asm__("mov            v2.2d[0],       %x[c1]          \n\t"
              "pmull          v2.1q,          v2.1d,  v0.1d   \n\t"
              "mov            v3.2d[0],       %x[c0]          \n\t"
              "pmull          v3.1q,          v3.1d,  v1.1d   \n\t"
              "crc32cx        %w[c],          %w[c2], %x[v]   \n\t"
              "mov            %x[c1],         v2.2d[0]        \n\t"
              "crc32cx        %w[c1],         wzr,    %x[c1]  \n\t"
              "eor            %w[c],          %w[c],  %w[c1]  \n\t"
              "mov            %x[c0],         v3.2d[0]        \n\t"
              "crc32cx        %w[c0],         wzr,    %x[c0]  \n\t"
              "eor            %w[c],          %w[c],  %w[c0]  \n\t"
              :[c1]"+r"(crc1), [c0]"+r"(crc0), [c2]"+r"(crc2), [c]"+r"(crc)
              :[v]"r"(*((const uint64_t *)buffer)));
      buffer+= sizeof(uint64_t);
    }
# endif /* HAVE_ARMV8_CRC_CRYPTO_INTRINSICS */

    /* Done if Input data size is aligned with 1024  */
    if (!(length+= 1024))
      return ~crc;

#endif /* HAVE_ARMV8_CRYPTO */

  }  // end if pmull_supported

  while ((length-= sizeof(uint64_t)) >= 0)
  {
    CRC32CX(crc, *(uint64_t *)buffer);
    buffer+= sizeof(uint64_t);
  }

  /* The following is more efficient than the straight loop */
  if (length & sizeof(uint32_t))
  {
    CRC32CW(crc, *(uint32_t *)buffer);
    buffer+= sizeof(uint32_t);
  }

  if (length & sizeof(uint16_t))
  {
    CRC32CH(crc, *(uint16_t *)buffer);
    buffer+= sizeof(uint16_t);
  }

  if (length & sizeof(uint8_t))
    CRC32CB(crc, *buffer);

  return ~crc;
}

/* There are multiple approaches to calculate crc.
Approach-1: Process 8 bytes then 4 bytes then 2 bytes and then 1 bytes
Approach-2: Process 8 bytes and remaining workload using 1 bytes
Apporach-3: Process 64 bytes at once by issuing 8 crc call and remaining
            using 8/1 combination.

Based on micro-benchmark testing we found that Approach-2 works best especially
given small chunk of variable data. */
unsigned int crc32_aarch64(unsigned int crc, const void *buf, size_t len)
{
  const uint8_t *buf1= buf;
  const uint64_t *buf8= (const uint64_t *) (((uintptr_t) buf + 7) & ~7);

  crc= ~crc;

  /* if start pointer is not 8 bytes aligned */
  while ((buf1 != (const uint8_t *) buf8) && len)
  {
    CRC32B(crc, *buf1++);
    len--;
  }

  for (; len >= 8; len-= 8)
    CRC32X(crc, *buf8++);

  buf1= (const uint8_t *) buf8;
  while (len--)
    CRC32B(crc, *buf1++);

  return ~crc;
}
