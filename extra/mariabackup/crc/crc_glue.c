/******************************************************
Copyright (c) 2017 Percona LLC and/or its affiliates.

Zlib compatible CRC-32 implementation.

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
#include "my_config.h"
#include "crc_glue.h"
#include "crc-intel-pclmul.h"
#include <stdint.h>
#include <string.h>
#include <zlib.h>

#if defined(__GNUC__) && defined(__x86_64__)
static int pclmul_enabled = 0;
#endif

#if defined(__GNUC__) && defined(__x86_64__)
static
uint32_t
cpuid(uint32_t*	ecx, uint32_t*	edx)
{
	uint32_t level;

	asm("cpuid" : "=a"  (level) : "a" (0) : "ebx", "ecx", "edx");

	if (level < 1) {
		return level;
	}

	asm("cpuid" : "=c" (*ecx), "=d" (*edx)
	    : "a" (1)
	    : "ebx");

	return level;
}
#endif

void crc_init() {
#if defined(__GNUC__) && defined(__x86_64__)
	uint32_t ecx, edx;

	if (cpuid(&ecx, &edx) > 0) {
		pclmul_enabled = ((ecx >> 19) & 1) && ((ecx >> 1) & 1);
	}
#endif
}

unsigned long crc32_iso3309(unsigned long crc, const unsigned char *buf, unsigned int len)
{
#if __GNUC__ >= 4 && defined(__x86_64__) && defined(HAVE_CLMUL_INSTRUCTION)
	if (pclmul_enabled) {
		uint32_t crc_accum = crc ^ 0xffffffffL;
		crc32_intel_pclmul(&crc_accum, buf, len);
		return crc_accum ^ 0xffffffffL;
	}
#endif
	return crc32(crc, buf, len);
}
