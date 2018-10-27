#ifndef CLANG_WORKAROUNDS_H
#define CLANG_WORKAROUNDS_H

/*
 * These stubs fix clang incompatibilities with GCC builtins.
 */

#ifndef __builtin_crypto_vpmsumw
#define __builtin_crypto_vpmsumw __builtin_crypto_vpmsumb
#endif
#ifndef __builtin_crypto_vpmsumd
#define __builtin_crypto_vpmsumd __builtin_crypto_vpmsumb
#endif

static inline
__vector unsigned long long __attribute__((overloadable))
vec_ld(int __a, const __vector unsigned long long* __b)
{
	return (__vector unsigned long long)__builtin_altivec_lvx(__a, __b);
}

/*
 * GCC __builtin_pack_vector_int128 returns a vector __int128_t but Clang
 * does not recognize this type. On GCC this builtin is translated to a
 * xxpermdi instruction that only moves the registers __a, __b instead generates
 * a load.
 *
 * Clang has vec_xxpermdi intrinsics. It was implemented in 4.0.0.
 */
static inline
__vector unsigned long long  __builtin_pack_vector (unsigned long __a,
						    unsigned long __b)
{
	#if defined(__BIG_ENDIAN__)
	__vector unsigned long long __v = {__a, __b};
	#else
	__vector unsigned long long __v = {__b, __a};
	#endif
	return __v;
}

#ifndef vec_xxpermdi

static inline
unsigned long __builtin_unpack_vector (__vector unsigned long long __v,
				       int __o)
{
	return __v[__o];
}

#if defined(__BIG_ENDIAN__)
#define __builtin_unpack_vector_0(a) __builtin_unpack_vector ((a), 0)
#define __builtin_unpack_vector_1(a) __builtin_unpack_vector ((a), 1)
#else
#define __builtin_unpack_vector_0(a) __builtin_unpack_vector ((a), 1)
#define __builtin_unpack_vector_1(a) __builtin_unpack_vector ((a), 0)
#endif

#else

static inline
unsigned long __builtin_unpack_vector_0 (__vector unsigned long long __v)
{
	#if defined(__BIG_ENDIAN__)
	return vec_xxpermdi(__v, __v, 0x0)[1];
	#else
	return vec_xxpermdi(__v, __v, 0x0)[0];
	#endif
}

static inline
unsigned long __builtin_unpack_vector_1 (__vector unsigned long long __v)
{
	#if defined(__BIG_ENDIAN__)
	return vec_xxpermdi(__v, __v, 0x3)[1];
	#else
	return vec_xxpermdi(__v, __v, 0x3)[0];
	#endif
}
#endif /* vec_xxpermdi */

#endif
