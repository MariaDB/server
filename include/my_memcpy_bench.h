#pragma once
#include <atomic>
#include <chrono>
#include <cstring>
#include <cassert>
#include <cstdint>
#include <new>
#include <memory>
#include <immintrin.h>
#include <time.h>




static __attribute__((always_inline)) inline void *memcpy_best_aligned(void *dest, const void *src, size_t n) {
    char *d = (char *)__builtin_assume_aligned(dest, 8);
    const char *s = (const char *)__builtin_assume_aligned(src, 8);
    
    if (n <= 8) {
        if (n >= 4) {
            uint32_t a, b;
            __builtin_memcpy(&a, s, 4);
            __builtin_memcpy(&b, s + n - 4, 4);
            __builtin_memcpy(d, &a, 4);
            __builtin_memcpy(d + n - 4, &b, 4);
            return dest;
        }
        if (n >= 2) {
            uint16_t a, b;
            __builtin_memcpy(&a, s, 2);
            __builtin_memcpy(&b, s + n - 2, 2);
            __builtin_memcpy(d, &a, 2);
            __builtin_memcpy(d + n - 2, &b, 2);
            return dest;
        }
        if (n == 1) {
            *d = *s;
        }
        return dest;
    }
    if (n <= 16) {
        uint64_t a, b;
        __builtin_memcpy(&a, s, 8);
        __builtin_memcpy(&b, s + n - 8, 8);
        __builtin_memcpy(d, &a, 8);
        __builtin_memcpy(d + n - 8, &b, 8);
        return dest;
    }
    if (n <= 32) {
        __m128i a = _mm_loadu_si128((const __m128i*)s);
        __m128i b = _mm_loadu_si128((const __m128i*)(s + n - 16));
        _mm_storeu_si128((__m128i*)d, a);
        _mm_storeu_si128((__m128i*)(d + n - 16), b);
        return dest;
    }
    if (n <= 64) {
        __m256i a = _mm256_loadu_si256((const __m256i*)s);
        __m256i b = _mm256_loadu_si256((const __m256i*)(s + n - 32));
        _mm256_storeu_si256((__m256i*)d, a);
        _mm256_storeu_si256((__m256i*)(d + n - 32), b);
        return dest;
    }
    if (n <= 128) {
        __m256i a = _mm256_loadu_si256((const __m256i*)s);
        __m256i b = _mm256_loadu_si256((const __m256i*)(s + 32));
        __m256i c = _mm256_loadu_si256((const __m256i*)(s + n - 64));
        __m256i d_vec = _mm256_loadu_si256((const __m256i*)(s + n - 32));
        _mm256_storeu_si256((__m256i*)d, a);
        _mm256_storeu_si256((__m256i*)(d + 32), b);
        _mm256_storeu_si256((__m256i*)(d + n - 64), c);
        _mm256_storeu_si256((__m256i*)(d + n - 32), d_vec);
        return dest;
    }
    if (n <= 256) {
        __m256i a = _mm256_loadu_si256((const __m256i*)s);
        __m256i b = _mm256_loadu_si256((const __m256i*)(s + 32));
        __m256i c = _mm256_loadu_si256((const __m256i*)(s + 64));
        __m256i d_vec = _mm256_loadu_si256((const __m256i*)(s + 96));
        __m256i e = _mm256_loadu_si256((const __m256i*)(s + n - 128));
        __m256i f_vec = _mm256_loadu_si256((const __m256i*)(s + n - 96));
        __m256i g = _mm256_loadu_si256((const __m256i*)(s + n - 64));
        __m256i h = _mm256_loadu_si256((const __m256i*)(s + n - 32));
        _mm256_storeu_si256((__m256i*)d, a);
        _mm256_storeu_si256((__m256i*)(d + 32), b);
        _mm256_storeu_si256((__m256i*)(d + 64), c);
        _mm256_storeu_si256((__m256i*)(d + 96), d_vec);
        _mm256_storeu_si256((__m256i*)(d + n - 128), e);
        _mm256_storeu_si256((__m256i*)(d + n - 96), f_vec);
        _mm256_storeu_si256((__m256i*)(d + n - 64), g);
        _mm256_storeu_si256((__m256i*)(d + n - 32), h);
        return dest;
    }
    return __builtin_memcpy(d, s, n);
}

static __attribute__((always_inline)) inline void *memcpy_best_unaligned(void *dest, const void *src, size_t n) {
    char *d = (char *)dest;
    const char *s = (const char *)src;
    
    if (n <= 8) {
        if (n >= 4) {
            uint32_t a, b;
            __builtin_memcpy(&a, s, 4);
            __builtin_memcpy(&b, s + n - 4, 4);
            __builtin_memcpy(d, &a, 4);
            __builtin_memcpy(d + n - 4, &b, 4);
            return dest;
        }
        if (n >= 2) {
            uint16_t a, b;
            __builtin_memcpy(&a, s, 2);
            __builtin_memcpy(&b, s + n - 2, 2);
            __builtin_memcpy(d, &a, 2);
            __builtin_memcpy(d + n - 2, &b, 2);
            return dest;
        }
        if (n == 1) {
            *d = *s;
        }
        return dest;
    }
    if (n <= 16) {
        uint64_t a, b;
        __builtin_memcpy(&a, s, 8);
        __builtin_memcpy(&b, s + n - 8, 8);
        __builtin_memcpy(d, &a, 8);
        __builtin_memcpy(d + n - 8, &b, 8);
        return dest;
    }
    if (n <= 32) {
        __m128i a = _mm_loadu_si128((const __m128i*)s);
        __m128i b = _mm_loadu_si128((const __m128i*)(s + n - 16));
        _mm_storeu_si128((__m128i*)d, a);
        _mm_storeu_si128((__m128i*)(d + n - 16), b);
        return dest;
    }
    if (n <= 64) {
        __m256i a = _mm256_loadu_si256((const __m256i*)s);
        __m256i b = _mm256_loadu_si256((const __m256i*)(s + n - 32));
        _mm256_storeu_si256((__m256i*)d, a);
        _mm256_storeu_si256((__m256i*)(d + n - 32), b);
        return dest;
    }
    if (n <= 128) {
        __m256i a = _mm256_loadu_si256((const __m256i*)s);
        __m256i b = _mm256_loadu_si256((const __m256i*)(s + 32));
        __m256i c = _mm256_loadu_si256((const __m256i*)(s + n - 64));
        __m256i d_vec = _mm256_loadu_si256((const __m256i*)(s + n - 32));
        _mm256_storeu_si256((__m256i*)d, a);
        _mm256_storeu_si256((__m256i*)(d + 32), b);
        _mm256_storeu_si256((__m256i*)(d + n - 64), c);
        _mm256_storeu_si256((__m256i*)(d + n - 32), d_vec);
        return dest;
    }
    if (n <= 256) {
        __m256i a = _mm256_loadu_si256((const __m256i*)s);
        __m256i b = _mm256_loadu_si256((const __m256i*)(s + 32));
        __m256i c = _mm256_loadu_si256((const __m256i*)(s + 64));
        __m256i d_vec = _mm256_loadu_si256((const __m256i*)(s + 96));
        __m256i e = _mm256_loadu_si256((const __m256i*)(s + n - 128));
        __m256i f_vec = _mm256_loadu_si256((const __m256i*)(s + n - 96));
        __m256i g = _mm256_loadu_si256((const __m256i*)(s + n - 64));
        __m256i h = _mm256_loadu_si256((const __m256i*)(s + n - 32));
        _mm256_storeu_si256((__m256i*)d, a);
        _mm256_storeu_si256((__m256i*)(d + 32), b);
        _mm256_storeu_si256((__m256i*)(d + 64), c);
        _mm256_storeu_si256((__m256i*)(d + 96), d_vec);
        _mm256_storeu_si256((__m256i*)(d + n - 128), e);
        _mm256_storeu_si256((__m256i*)(d + n - 96), f_vec);
        _mm256_storeu_si256((__m256i*)(d + n - 64), g);
        _mm256_storeu_si256((__m256i*)(d + n - 32), h);
        return dest;
    }
    return __builtin_memcpy(d, s, n);
}

static inline void my_exact_unaligned_memcpy(void *dest, const void *src, size_t n) {
    char *d = (char *)dest;
    const char *s = (const char *)src;

    switch (n) {
        case 8:
            __builtin_memcpy(d, s, 8);
            return;
        case 16:
            _mm_storeu_si128((__m128i*)d, _mm_loadu_si128((const __m128i*)s));
            return;
        case 32:
            _mm256_storeu_si256((__m256i*)d, _mm256_loadu_si256((const __m256i*)s));
            return;
        case 64:
            _mm256_storeu_si256((__m256i*)d, _mm256_loadu_si256((const __m256i*)s));
            _mm256_storeu_si256((__m256i*)(d + 32), _mm256_loadu_si256((const __m256i*)(s + 32)));
            return;
    }
    __builtin_memcpy(d, s, n);
}

static inline void __attribute__((always_inline)) memcpy_switch_inline_aligned(void *dest, const void *src, size_t s) {
    switch (s) {
        case 8:  (*(uint64_t*)dest = *(uint64_t*)src); break;
        case 16: __builtin_memcpy(__builtin_assume_aligned(dest, 16), __builtin_assume_aligned(src, 16), 16); break;
        case 32: __builtin_memcpy(__builtin_assume_aligned(dest, 16), __builtin_assume_aligned(src, 16), 32); break;
        case 64: __builtin_memcpy(__builtin_assume_aligned(dest, 16), __builtin_assume_aligned(src, 16), 64); break;
        case 128: __builtin_memcpy(__builtin_assume_aligned(dest, 16), __builtin_assume_aligned(src, 16), 128); break;
        case 256: __builtin_memcpy(__builtin_assume_aligned(dest, 16), __builtin_assume_aligned(src, 16), 256); break;
        default: memcpy(dest, src, s);
    }
}

static inline void __attribute__((always_inline)) memcpy_threshold_unaligned_avx(void *dest, const void *src, size_t s) {
    char *dst = (char *)dest;
    const char *src_ptr = (const char *)src;
    if (s <= 16) {
        _mm_storeu_si128((__m128i*)dst, _mm_loadu_si128((const __m128i*)src_ptr));
    } else if (s <= 32) {
        _mm256_storeu_si256((__m256i*)dst, _mm256_loadu_si256((const __m256i*)src_ptr));
    } else if (s <= 64) {
        _mm256_storeu_si256((__m256i*)dst, _mm256_loadu_si256((const __m256i*)src_ptr));
        _mm256_storeu_si256((__m256i*)(dst + 32), _mm256_loadu_si256((const __m256i*)(src_ptr + 32)));
    } else if (s <= 128) {
        _mm256_storeu_si256((__m256i*)dst, _mm256_loadu_si256((const __m256i*)src_ptr));
        _mm256_storeu_si256((__m256i*)(dst + 32), _mm256_loadu_si256((const __m256i*)(src_ptr + 32)));
        _mm256_storeu_si256((__m256i*)(dst + 64), _mm256_loadu_si256((const __m256i*)(src_ptr + 64)));
        _mm256_storeu_si256((__m256i*)(dst + 96), _mm256_loadu_si256((const __m256i*)(src_ptr + 96)));
    } else if (s <= 256) {
        _mm256_storeu_si256((__m256i*)dst, _mm256_loadu_si256((const __m256i*)src_ptr));
        _mm256_storeu_si256((__m256i*)(dst + 32), _mm256_loadu_si256((const __m256i*)(src_ptr + 32)));
        _mm256_storeu_si256((__m256i*)(dst + 64), _mm256_loadu_si256((const __m256i*)(src_ptr + 64)));
        _mm256_storeu_si256((__m256i*)(dst + 96), _mm256_loadu_si256((const __m256i*)(src_ptr + 96)));
        _mm256_storeu_si256((__m256i*)(dst + 128), _mm256_loadu_si256((const __m256i*)(src_ptr + 128)));
        _mm256_storeu_si256((__m256i*)(dst + 160), _mm256_loadu_si256((const __m256i*)(src_ptr + 160)));
        _mm256_storeu_si256((__m256i*)(dst + 192), _mm256_loadu_si256((const __m256i*)(src_ptr + 192)));
        _mm256_storeu_si256((__m256i*)(dst + 224), _mm256_loadu_si256((const __m256i*)(src_ptr + 224)));
    } else {
        __builtin_memcpy(dst, src_ptr, s);
    }
}

static inline void __attribute__((always_inline)) memcpy_threshold16(void *dest, const void *src, size_t s) {
    void *dst = __builtin_assume_aligned(dest, 16);
    const void *src_ptr = __builtin_assume_aligned(src, 16);
    if (s == 16) {memcpy(dst, src_ptr, s);
    } else if (s == 32) {memcpy(dst, src_ptr, s);
    } else if (s == 64) {memcpy(dst, src_ptr, s);
    } else if (s == 128) {memcpy(dst, src_ptr, s);
    } else if (s == 256) {memcpy(dst, src_ptr, s);
    } else {
        memcpy(dst, src_ptr, s);
    }
}

static inline void __attribute__((always_inline)) memcpy_threshold_builtins_avx(void *dest, const void *src, size_t s) {
    void *dst = __builtin_assume_aligned(dest, 32);
    const void *src_ptr = __builtin_assume_aligned(src, 32);
    if (s == 32) {__builtin_memcpy(dst, src_ptr, s);
    } else if (s == 64) {__builtin_memcpy(dst, src_ptr, s);
    } else if (s == 128) {__builtin_memcpy(dst, src_ptr, s);
    } else if (s == 256) {__builtin_memcpy(dst, src_ptr, s);
    } else {
        __builtin_memcpy(dst, src_ptr, s);
    }
}

static inline void __attribute__((always_inline)) memcpy_threshold_builtins16(void *dest, const void *src, size_t s) {
    void *dst = __builtin_assume_aligned(dest, 16);
    const void *src_ptr = __builtin_assume_aligned(src, 16);
    if (s == 16) {__builtin_memcpy(dst, src_ptr, s);
    } else if (s == 32) {__builtin_memcpy(dst, src_ptr, s);
    } else if (s == 64) {__builtin_memcpy(dst, src_ptr, s);
    } else if (s == 128) {__builtin_memcpy(dst, src_ptr, s);
    } else if (s == 256) {__builtin_memcpy(dst, src_ptr, s);
    } else {
        __builtin_memcpy(dst, src_ptr, s);
    }
}

static inline void __attribute__((always_inline)) memcpy_threshold_builtins8(void *dest, const void *src, size_t s) {
    void *dst = __builtin_assume_aligned(dest, 8);
    const void *src_ptr = __builtin_assume_aligned(src, 8);
    if (s == 8) {
      *(uint64_t*)dest = *(uint64_t*)src;
    } else if (s == 16) {__builtin_memcpy(dst, src_ptr, s);
    } else if (s == 32) {__builtin_memcpy(dst, src_ptr, s);
    } else if (s == 64) {__builtin_memcpy(dst, src_ptr, s);
    } else if (s == 128) {__builtin_memcpy(dst, src_ptr, s);
    } else if (s == 256) {__builtin_memcpy(dst, src_ptr, s);
    } else {
        __builtin_memcpy(dst, src_ptr, s);
    }
}

static inline void __attribute__((always_inline)) memcpy_threshold_aligned_avx(void *dest, const void *src, size_t s) {
    char *dst = (char *)dest;
    const char *src_ptr = (const char *)src;
    if (s <= 16) {
        _mm_store_si128((__m128i*)dst, _mm_load_si128((const __m128i*)src_ptr));
    } else if (s <= 32) {
        _mm256_store_si256((__m256i*)dst, _mm256_load_si256((const __m256i*)src_ptr));
    } else if (s <= 64) {
        _mm256_store_si256((__m256i*)dst, _mm256_load_si256((const __m256i*)src_ptr));
        _mm256_store_si256((__m256i*)(dst + 32), _mm256_load_si256((const __m256i*)(src_ptr + 32)));
    } else if (s <= 128) {
        _mm256_store_si256((__m256i*)dst, _mm256_load_si256((const __m256i*)src_ptr));
        _mm256_store_si256((__m256i*)(dst + 32), _mm256_load_si256((const __m256i*)(src_ptr + 32)));
        _mm256_store_si256((__m256i*)(dst + 64), _mm256_load_si256((const __m256i*)(src_ptr + 64)));
        _mm256_store_si256((__m256i*)(dst + 96), _mm256_load_si256((const __m256i*)(src_ptr + 96)));
    } else if (s <= 256) {
        _mm256_store_si256((__m256i*)dst, _mm256_load_si256((const __m256i*)src_ptr));
        _mm256_store_si256((__m256i*)(dst + 32), _mm256_load_si256((const __m256i*)(src_ptr + 32)));
        _mm256_store_si256((__m256i*)(dst + 64), _mm256_load_si256((const __m256i*)(src_ptr + 64)));
        _mm256_store_si256((__m256i*)(dst + 96), _mm256_load_si256((const __m256i*)(src_ptr + 96)));
        _mm256_store_si256((__m256i*)(dst + 128), _mm256_load_si256((const __m256i*)(src_ptr + 128)));
        _mm256_store_si256((__m256i*)(dst + 160), _mm256_load_si256((const __m256i*)(src_ptr + 160)));
        _mm256_store_si256((__m256i*)(dst + 192), _mm256_load_si256((const __m256i*)(src_ptr + 192)));
        _mm256_store_si256((__m256i*)(dst + 224), _mm256_load_si256((const __m256i*)(src_ptr + 224)));
    } else {
        __builtin_memcpy(dst, src_ptr, s);
    }
}

static inline void __attribute__((always_inline)) memcpy_threshold_8byte_mov_avx_fallback(void *dest, const void *src, size_t s) {
    char *dst = (char *)dest;
    const char *src_ptr = (const char *)src;
    if (s <= 8) {
        _mm_storel_epi64((__m128i*)dst, _mm_loadl_epi64((const __m128i*)src_ptr));
    } else if (s <= 16) {
        _mm_storeu_si128((__m128i*)dst, _mm_loadu_si128((const __m128i*)src_ptr));
    } else if (s <= 32) {
        _mm256_storeu_si256((__m256i*)dst, _mm256_loadu_si256((const __m256i*)src_ptr));
    } else if (s <= 64) {
        _mm256_storeu_si256((__m256i*)dst, _mm256_loadu_si256((const __m256i*)src_ptr));
        _mm256_storeu_si256((__m256i*)(dst + 32), _mm256_loadu_si256((const __m256i*)(src_ptr + 32)));
    } else if (s <= 128) {
        _mm256_storeu_si256((__m256i*)dst, _mm256_loadu_si256((const __m256i*)src_ptr));
        _mm256_storeu_si256((__m256i*)(dst + 32), _mm256_loadu_si256((const __m256i*)(src_ptr + 32)));
        _mm256_storeu_si256((__m256i*)(dst + 64), _mm256_loadu_si256((const __m256i*)(src_ptr + 64)));
        _mm256_storeu_si256((__m256i*)(dst + 96), _mm256_loadu_si256((const __m256i*)(src_ptr + 96)));
    } else if (s <= 256) {
        _mm256_storeu_si256((__m256i*)dst, _mm256_loadu_si256((const __m256i*)src_ptr));
        _mm256_storeu_si256((__m256i*)(dst + 32), _mm256_loadu_si256((const __m256i*)(src_ptr + 32)));
        _mm256_storeu_si256((__m256i*)(dst + 64), _mm256_loadu_si256((const __m256i*)(src_ptr + 64)));
        _mm256_storeu_si256((__m256i*)(dst + 96), _mm256_loadu_si256((const __m256i*)(src_ptr + 96)));
        _mm256_storeu_si256((__m256i*)(dst + 128), _mm256_loadu_si256((const __m256i*)(src_ptr + 128)));
        _mm256_storeu_si256((__m256i*)(dst + 160), _mm256_loadu_si256((const __m256i*)(src_ptr + 160)));
        _mm256_storeu_si256((__m256i*)(dst + 192), _mm256_loadu_si256((const __m256i*)(src_ptr + 192)));
        _mm256_storeu_si256((__m256i*)(dst + 224), _mm256_loadu_si256((const __m256i*)(src_ptr + 224)));
    } else {
        __builtin_memcpy(dst, src_ptr, s);
    }
}


static inline void __attribute__((always_inline)) memcpy_rep_movsq(void *dest, const void *src, size_t s) {
    // s is total bytes. rep movsq moves 8 bytes at a time.
    // Assuming size is a multiple of 8 for this specific test
    size_t count = s / 8;
    asm volatile (
        "rep movsq"
        : "+D" (dest), "+S" (src), "+c" (count)
        :
        : "memory"
    );
}

static inline void __attribute__((always_inline)) memcpy_manual_avx_loop_unaligned(void *dest, const void *src, size_t s) {
    char *dst = (char *)dest;
    const char *src_ptr = (const char *)src;
    size_t i = 0;
    for (; i + 32 <= s; i += 32) {
        _mm256_storeu_si256((__m256i*)(dst + i), _mm256_loadu_si256((const __m256i*)(src_ptr + i)));
    }
    if (i < s) {
        __builtin_memcpy(dst + i, src_ptr + i, s - i);
    }
}



static inline void __attribute__((always_inline)) memcpy_manual_avx_loop_aligned(void *dest, const void *src, size_t s) {
    char *dst = (char *)dest;
    const char *src_ptr = (const char *)src;
    size_t i = 0;
    for (; i + 32 <= s; i += 32) {
        _mm256_store_si256((__m256i*)(dst + i), _mm256_load_si256((const __m256i*)(src_ptr + i)));
    }
    if (i < s) {
        __builtin_memcpy(dst + i, src_ptr + i, s - i);
    }
}


static inline void __attribute__((always_inline)) memcpy_manual_sse_loop(void *dest, const void *src, size_t s) {
    char *dst = (char *)dest;
    const char *src_ptr = (const char *)src;
    for (size_t i = 0; i < s; i += 16) {
        _mm_storeu_si128((__m128i*)(dst + i), _mm_loadu_si128((const __m128i*)(src_ptr + i)));
    }
}


extern unsigned long long total_memcpy_time;
extern unsigned long long total_memcpy_calls;

#define copy_record_func(t, dst, src) memcpy_threshold_builtins_avx(dst, src, (t)->s->rec_buff_length)

#define copy_record(t, dst, src) do { \
    unsigned int __aux; \
    auto __m_start = __builtin_ia32_rdtscp(&__aux); \
    copy_record_func(t, dst, src); \
    auto __m_end = __builtin_ia32_rdtscp(&__aux); \
    __atomic_fetch_add(&total_memcpy_time, __m_end - __m_start, __ATOMIC_RELAXED); \
    __atomic_fetch_add(&total_memcpy_calls, 1, __ATOMIC_RELAXED); \
} while(0)
