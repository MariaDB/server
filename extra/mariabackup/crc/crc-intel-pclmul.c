/******************************************************
Copyright (c) 2017 Percona LLC and/or its affiliates.

CRC32 using Intel's PCLMUL instruction.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA

*******************************************************/

/* crc-intel-pclmul.c - Intel PCLMUL accelerated CRC implementation
 * Copyright (C) 2016 Jussi Kivilinna <jussi.kivilinna@iki.fi>
 *
 * This file is part of Libgcrypt.
 *
 * Libgcrypt is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * Libgcrypt is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#  define U64_C(c) (c ## UL)

typedef uint32_t u32;
typedef uint16_t u16;
typedef uint64_t u64;
#ifndef byte
typedef uint8_t byte;
#endif

# define _gcry_bswap32 __builtin_bswap32

#if __GNUC__ >= 4 && defined(__x86_64__) && defined(HAVE_CLMUL_INSTRUCTION)

#if _GCRY_GCC_VERSION >= 40400 /* 4.4 */
/* Prevent compiler from issuing SSE instructions between asm blocks. */
#  pragma GCC target("no-sse")
#endif


#define ALIGNED_16 __attribute__ ((aligned (16)))


struct u16_unaligned_s
{
  u16 a;
} __attribute__((packed, aligned (1), may_alias));


/* Constants structure for generic reflected/non-reflected CRC32 CLMUL
 * functions. */
struct crc32_consts_s
{
  /* k: { x^(32*17), x^(32*15), x^(32*5), x^(32*3), x^(32*2), 0 } mod P(x) */
  u64 k[6];
  /* my_p: { floor(x^64 / P(x)), P(x) } */
  u64 my_p[2];
};


/* CLMUL constants for CRC32 and CRC32RFC1510. */
static const struct crc32_consts_s crc32_consts ALIGNED_16 =
{
  { /* k[6] = reverse_33bits( x^(32*y) mod P(x) ) */
    U64_C(0x154442bd4), U64_C(0x1c6e41596), /* y = { 17, 15 } */
    U64_C(0x1751997d0), U64_C(0x0ccaa009e), /* y = { 5, 3 } */
    U64_C(0x163cd6124), 0                   /* y = 2 */
  },
  { /* my_p[2] = reverse_33bits ( { floor(x^64 / P(x)), P(x) } ) */
    U64_C(0x1f7011641), U64_C(0x1db710641)
  }
};

/* Common constants for CRC32 algorithms. */
static const byte crc32_refl_shuf_shift[3 * 16] ALIGNED_16 =
  {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  };
static const byte crc32_partial_fold_input_mask[16 + 16] ALIGNED_16 =
  {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  };
static const u64 crc32_merge9to15_shuf[15 - 9 + 1][2] ALIGNED_16 =
  {
    { U64_C(0x0706050403020100), U64_C(0xffffffffffffff0f) }, /* 9 */
    { U64_C(0x0706050403020100), U64_C(0xffffffffffff0f0e) },
    { U64_C(0x0706050403020100), U64_C(0xffffffffff0f0e0d) },
    { U64_C(0x0706050403020100), U64_C(0xffffffff0f0e0d0c) },
    { U64_C(0x0706050403020100), U64_C(0xffffff0f0e0d0c0b) },
    { U64_C(0x0706050403020100), U64_C(0xffff0f0e0d0c0b0a) },
    { U64_C(0x0706050403020100), U64_C(0xff0f0e0d0c0b0a09) }, /* 15 */
  };
static const u64 crc32_merge5to7_shuf[7 - 5 + 1][2] ALIGNED_16 =
  {
    { U64_C(0xffffff0703020100), U64_C(0xffffffffffffffff) }, /* 5 */
    { U64_C(0xffff070603020100), U64_C(0xffffffffffffffff) },
    { U64_C(0xff07060503020100), U64_C(0xffffffffffffffff) }, /* 7 */
  };

/* PCLMUL functions for reflected CRC32. */
static inline void
crc32_reflected_bulk (u32 *pcrc, const byte *inbuf, size_t inlen,
		      const struct crc32_consts_s *consts)
{
  if (inlen >= 8 * 16)
    {
      asm volatile ("movd %[crc], %%xmm4\n\t"
		    "movdqu %[inbuf_0], %%xmm0\n\t"
		    "movdqu %[inbuf_1], %%xmm1\n\t"
		    "movdqu %[inbuf_2], %%xmm2\n\t"
		    "movdqu %[inbuf_3], %%xmm3\n\t"
		    "pxor %%xmm4, %%xmm0\n\t"
		    :
		    : [inbuf_0] "m" (inbuf[0 * 16]),
		      [inbuf_1] "m" (inbuf[1 * 16]),
		      [inbuf_2] "m" (inbuf[2 * 16]),
		      [inbuf_3] "m" (inbuf[3 * 16]),
		      [crc] "m" (*pcrc)
		    );

      inbuf += 4 * 16;
      inlen -= 4 * 16;

      asm volatile ("movdqa %[k1k2], %%xmm4\n\t"
		    :
		    : [k1k2] "m" (consts->k[1 - 1])
		    );

      /* Fold by 4. */
      while (inlen >= 4 * 16)
	{
	  asm volatile ("movdqu %[inbuf_0], %%xmm5\n\t"
			"movdqa %%xmm0, %%xmm6\n\t"
			"pclmulqdq $0x00, %%xmm4, %%xmm0\n\t"
			"pclmulqdq $0x11, %%xmm4, %%xmm6\n\t"
			"pxor %%xmm5, %%xmm0\n\t"
			"pxor %%xmm6, %%xmm0\n\t"

			"movdqu %[inbuf_1], %%xmm5\n\t"
			"movdqa %%xmm1, %%xmm6\n\t"
			"pclmulqdq $0x00, %%xmm4, %%xmm1\n\t"
			"pclmulqdq $0x11, %%xmm4, %%xmm6\n\t"
			"pxor %%xmm5, %%xmm1\n\t"
			"pxor %%xmm6, %%xmm1\n\t"

			"movdqu %[inbuf_2], %%xmm5\n\t"
			"movdqa %%xmm2, %%xmm6\n\t"
			"pclmulqdq $0x00, %%xmm4, %%xmm2\n\t"
			"pclmulqdq $0x11, %%xmm4, %%xmm6\n\t"
			"pxor %%xmm5, %%xmm2\n\t"
			"pxor %%xmm6, %%xmm2\n\t"

			"movdqu %[inbuf_3], %%xmm5\n\t"
			"movdqa %%xmm3, %%xmm6\n\t"
			"pclmulqdq $0x00, %%xmm4, %%xmm3\n\t"
			"pclmulqdq $0x11, %%xmm4, %%xmm6\n\t"
			"pxor %%xmm5, %%xmm3\n\t"
			"pxor %%xmm6, %%xmm3\n\t"
			:
			: [inbuf_0] "m" (inbuf[0 * 16]),
			  [inbuf_1] "m" (inbuf[1 * 16]),
			  [inbuf_2] "m" (inbuf[2 * 16]),
			  [inbuf_3] "m" (inbuf[3 * 16])
			);

	  inbuf += 4 * 16;
	  inlen -= 4 * 16;
	}

      asm volatile ("movdqa %[k3k4], %%xmm6\n\t"
		    "movdqa %[my_p], %%xmm5\n\t"
		    :
		    : [k3k4] "m" (consts->k[3 - 1]),
		      [my_p] "m" (consts->my_p[0])
		    );

      /* Fold 4 to 1. */

      asm volatile ("movdqa %%xmm0, %%xmm4\n\t"
		    "pclmulqdq $0x00, %%xmm6, %%xmm0\n\t"
		    "pclmulqdq $0x11, %%xmm6, %%xmm4\n\t"
		    "pxor %%xmm1, %%xmm0\n\t"
		    "pxor %%xmm4, %%xmm0\n\t"

		    "movdqa %%xmm0, %%xmm4\n\t"
		    "pclmulqdq $0x00, %%xmm6, %%xmm0\n\t"
		    "pclmulqdq $0x11, %%xmm6, %%xmm4\n\t"
		    "pxor %%xmm2, %%xmm0\n\t"
		    "pxor %%xmm4, %%xmm0\n\t"

		    "movdqa %%xmm0, %%xmm4\n\t"
		    "pclmulqdq $0x00, %%xmm6, %%xmm0\n\t"
		    "pclmulqdq $0x11, %%xmm6, %%xmm4\n\t"
		    "pxor %%xmm3, %%xmm0\n\t"
		    "pxor %%xmm4, %%xmm0\n\t"
		    :
		    :
		    );
    }
  else
    {
      asm volatile ("movd %[crc], %%xmm1\n\t"
		    "movdqu %[inbuf], %%xmm0\n\t"
		    "movdqa %[k3k4], %%xmm6\n\t"
		    "pxor %%xmm1, %%xmm0\n\t"
		    "movdqa %[my_p], %%xmm5\n\t"
		    :
		    : [inbuf] "m" (*inbuf),
		      [crc] "m" (*pcrc),
		      [k3k4] "m" (consts->k[3 - 1]),
		      [my_p] "m" (consts->my_p[0])
		    );

      inbuf += 16;
      inlen -= 16;
    }

  /* Fold by 1. */
  if (inlen >= 16)
    {
      while (inlen >= 16)
	{
	  /* Load next block to XMM2. Fold XMM0 to XMM0:XMM1. */
	  asm volatile ("movdqu %[inbuf], %%xmm2\n\t"
			"movdqa %%xmm0, %%xmm1\n\t"
			"pclmulqdq $0x00, %%xmm6, %%xmm0\n\t"
			"pclmulqdq $0x11, %%xmm6, %%xmm1\n\t"
			"pxor %%xmm2, %%xmm0\n\t"
			"pxor %%xmm1, %%xmm0\n\t"
			:
			: [inbuf] "m" (*inbuf)
			);

	  inbuf += 16;
	  inlen -= 16;
	}
    }

  /* Partial fold. */
  if (inlen)
    {
      /* Load last input and add padding zeros. */
      asm volatile ("movdqu %[shr_shuf], %%xmm3\n\t"
		    "movdqu %[shl_shuf], %%xmm4\n\t"
		    "movdqu %[mask], %%xmm2\n\t"

		    "movdqa %%xmm0, %%xmm1\n\t"
		    "pshufb %%xmm4, %%xmm0\n\t"
		    "movdqu %[inbuf], %%xmm4\n\t"
		    "pshufb %%xmm3, %%xmm1\n\t"
		    "pand %%xmm4, %%xmm2\n\t"
		    "por %%xmm1, %%xmm2\n\t"

		    "movdqa %%xmm0, %%xmm1\n\t"
		    "pclmulqdq $0x00, %%xmm6, %%xmm0\n\t"
		    "pclmulqdq $0x11, %%xmm6, %%xmm1\n\t"
		    "pxor %%xmm2, %%xmm0\n\t"
		    "pxor %%xmm1, %%xmm0\n\t"
		    :
		    : [inbuf] "m" (*(inbuf - 16 + inlen)),
		      [mask] "m" (crc32_partial_fold_input_mask[inlen]),
		      [shl_shuf] "m" (crc32_refl_shuf_shift[inlen]),
		      [shr_shuf] "m" (crc32_refl_shuf_shift[inlen + 16])
		    );

      inbuf += inlen;
      inlen -= inlen;
    }

  /* Final fold. */
  asm volatile (/* reduce 128-bits to 96-bits */
		"movdqa %%xmm0, %%xmm1\n\t"
		"pclmulqdq $0x10, %%xmm6, %%xmm0\n\t"
		"psrldq $8, %%xmm1\n\t"
		"pxor %%xmm1, %%xmm0\n\t"

		/* reduce 96-bits to 64-bits */
		"pshufd $0xfc, %%xmm0, %%xmm1\n\t" /* [00][00][00][x] */
		"pshufd $0xf9, %%xmm0, %%xmm0\n\t" /* [00][00][x>>64][x>>32] */
		"pclmulqdq $0x00, %[k5], %%xmm1\n\t" /* [00][00][xx][xx] */
		"pxor %%xmm1, %%xmm0\n\t" /* top 64-bit are zero */

		/* barrett reduction */
		"pshufd $0xf3, %%xmm0, %%xmm1\n\t" /* [00][00][x>>32][00] */
		"pslldq $4, %%xmm0\n\t" /* [??][x>>32][??][??] */
		"pclmulqdq $0x00, %%xmm5, %%xmm1\n\t" /* [00][xx][xx][00] */
		"pclmulqdq $0x10, %%xmm5, %%xmm1\n\t" /* [00][xx][xx][00] */
		"pxor %%xmm1, %%xmm0\n\t"

		/* store CRC */
		"pextrd $2, %%xmm0, %[out]\n\t"
		: [out] "=m" (*pcrc)
		: [k5] "m" (consts->k[5 - 1])
	        );
}

static inline void
crc32_reflected_less_than_16 (u32 *pcrc, const byte *inbuf, size_t inlen,
			      const struct crc32_consts_s *consts)
{
  if (inlen < 4)
    {
      u32 crc = *pcrc;
      u32 data;

      asm volatile ("movdqa %[my_p], %%xmm5\n\t"
		    :
		    : [my_p] "m" (consts->my_p[0])
		    );

      if (inlen == 1)
	{
	  data = inbuf[0];
	  data ^= crc;
	  data <<= 24;
	  crc >>= 8;
	}
      else if (inlen == 2)
	{
	  data = ((const struct u16_unaligned_s *)inbuf)->a;
	  data ^= crc;
	  data <<= 16;
	  crc >>= 16;
	}
      else
	{
	  data = ((const struct u16_unaligned_s *)inbuf)->a;
	  data |= inbuf[2] << 16;
	  data ^= crc;
	  data <<= 8;
	  crc >>= 24;
	}

      /* Barrett reduction */
      asm volatile ("movd %[in], %%xmm0\n\t"
		    "movd %[crc], %%xmm1\n\t"

		    "pclmulqdq $0x00, %%xmm5, %%xmm0\n\t" /* [00][00][xx][xx] */
		    "psllq $32, %%xmm1\n\t"
		    "pshufd $0xfc, %%xmm0, %%xmm0\n\t" /* [00][00][00][x] */
		    "pclmulqdq $0x10, %%xmm5, %%xmm0\n\t" /* [00][00][xx][xx] */
		    "pxor %%xmm1, %%xmm0\n\t"

		    "pextrd $1, %%xmm0, %[out]\n\t"
		    : [out] "=m" (*pcrc)
		    : [in] "rm" (data),
		      [crc] "rm" (crc)
		    );
    }
  else if (inlen == 4)
    {
      /* Barrett reduction */
      asm volatile ("movd %[crc], %%xmm1\n\t"
		    "movd %[in], %%xmm0\n\t"
		    "movdqa %[my_p], %%xmm5\n\t"
		    "pxor %%xmm1, %%xmm0\n\t"

		    "pclmulqdq $0x00, %%xmm5, %%xmm0\n\t" /* [00][00][xx][xx] */
		    "pshufd $0xfc, %%xmm0, %%xmm0\n\t" /* [00][00][00][x] */
		    "pclmulqdq $0x10, %%xmm5, %%xmm0\n\t" /* [00][00][xx][xx] */

		    "pextrd $1, %%xmm0, %[out]\n\t"
		    : [out] "=m" (*pcrc)
		    : [in] "m" (*inbuf),
		      [crc] "m" (*pcrc),
		      [my_p] "m" (consts->my_p[0])
		    );
    }
  else
    {
      asm volatile ("movdqu %[shuf], %%xmm4\n\t"
		    "movd %[crc], %%xmm1\n\t"
		    "movdqa %[my_p], %%xmm5\n\t"
		    "movdqa %[k3k4], %%xmm6\n\t"
		    :
		    : [shuf] "m" (crc32_refl_shuf_shift[inlen]),
		      [crc] "m" (*pcrc),
		      [my_p] "m" (consts->my_p[0]),
		      [k3k4] "m" (consts->k[3 - 1])
		    );

      if (inlen >= 8)
	{
	  asm volatile ("movq %[inbuf], %%xmm0\n\t"
			:
			: [inbuf] "m" (*inbuf)
			);
	  if (inlen > 8)
	    {
	      asm volatile (/*"pinsrq $1, %[inbuf_tail], %%xmm0\n\t"*/
			    "movq %[inbuf_tail], %%xmm2\n\t"
			    "punpcklqdq %%xmm2, %%xmm0\n\t"
			    "pshufb %[merge_shuf], %%xmm0\n\t"
			    :
			    : [inbuf_tail] "m" (inbuf[inlen - 8]),
			      [merge_shuf] "m"
				(*crc32_merge9to15_shuf[inlen - 9])
			    );
	    }
	}
      else
	{
	  asm volatile ("movd %[inbuf], %%xmm0\n\t"
			"pinsrd $1, %[inbuf_tail], %%xmm0\n\t"
			"pshufb %[merge_shuf], %%xmm0\n\t"
			:
			: [inbuf] "m" (*inbuf),
			  [inbuf_tail] "m" (inbuf[inlen - 4]),
			  [merge_shuf] "m"
			    (*crc32_merge5to7_shuf[inlen - 5])
			);
	}

      /* Final fold. */
      asm volatile ("pxor %%xmm1, %%xmm0\n\t"
		    "pshufb %%xmm4, %%xmm0\n\t"

		    /* reduce 128-bits to 96-bits */
		    "movdqa %%xmm0, %%xmm1\n\t"
		    "pclmulqdq $0x10, %%xmm6, %%xmm0\n\t"
		    "psrldq $8, %%xmm1\n\t"
		    "pxor %%xmm1, %%xmm0\n\t" /* top 32-bit are zero */

		    /* reduce 96-bits to 64-bits */
		    "pshufd $0xfc, %%xmm0, %%xmm1\n\t" /* [00][00][00][x] */
		    "pshufd $0xf9, %%xmm0, %%xmm0\n\t" /* [00][00][x>>64][x>>32] */
		    "pclmulqdq $0x00, %[k5], %%xmm1\n\t" /* [00][00][xx][xx] */
		    "pxor %%xmm1, %%xmm0\n\t" /* top 64-bit are zero */

		    /* barrett reduction */
		    "pshufd $0xf3, %%xmm0, %%xmm1\n\t" /* [00][00][x>>32][00] */
		    "pslldq $4, %%xmm0\n\t" /* [??][x>>32][??][??] */
		    "pclmulqdq $0x00, %%xmm5, %%xmm1\n\t" /* [00][xx][xx][00] */
		    "pclmulqdq $0x10, %%xmm5, %%xmm1\n\t" /* [00][xx][xx][00] */
		    "pxor %%xmm1, %%xmm0\n\t"

		    /* store CRC */
		    "pextrd $2, %%xmm0, %[out]\n\t"
		    : [out] "=m" (*pcrc)
		    : [k5] "m" (consts->k[5 - 1])
		    );
    }
}

void
crc32_intel_pclmul (u32 *pcrc, const byte *inbuf, size_t inlen)
{
  const struct crc32_consts_s *consts = &crc32_consts;
#if defined(__x86_64__) && defined(__WIN64__)
  char win64tmp[2 * 16];

  /* XMM6-XMM7 need to be restored after use. */
  asm volatile ("movdqu %%xmm6, 0*16(%0)\n\t"
                "movdqu %%xmm7, 1*16(%0)\n\t"
                :
                : "r" (win64tmp)
                : "memory");
#endif

  if (!inlen)
    return;

  if (inlen >= 16)
    crc32_reflected_bulk(pcrc, inbuf, inlen, consts);
  else
    crc32_reflected_less_than_16(pcrc, inbuf, inlen, consts);

#if defined(__x86_64__) && defined(__WIN64__)
  /* Restore used registers. */
  asm volatile("movdqu 0*16(%0), %%xmm6\n\t"
               "movdqu 1*16(%0), %%xmm7\n\t"
               :
               : "r" (win64tmp)
               : "memory");
#endif
}

#endif
