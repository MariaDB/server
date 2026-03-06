/*
  Usage:
  git clone https://github.com/google/benchmark
  build it
  g++ bench_isolated.cpp -std=c++20 -O3 -march=native -Ibenchmark/include -Lbenchmark/build/src -lbenchmark -lpthread -o test_benchmark
  ./test_benchmark
  See the chart:
  ./test_benchmark --benchmark_format=json > results_inlined.json && python3 -c "import sys; print(open('index.template.html').read().replace('{{JSON_DATA}}', open('results_inlined.json').read()))" > index.html
*/

#include <benchmark/benchmark.h>
#include <cstring>
#include <cassert>
#include <cstdint>
#include <new>
#include <memory>
#include <immintrin.h> // For AVX intrinsics



#define MY_ALIGN(A,L)     (((A) + (L) - 1) & ~((L) - 1))

const size_t MAX_SIZE = 32 * 1024;
// Ensure 64-byte alignment for AVX-512
alignas(64) char global_src[MAX_SIZE];
alignas(64) char global_dst[MAX_SIZE];

template <typename T>
__attribute__((always_inline)) inline T* wash(T* ptr) {
    asm volatile("" : "+r"(ptr));
    return ptr;
}

__attribute__((always_inline)) inline void *memcpy_best_aligned(void *dest, const void *src, size_t n) {
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

__attribute__((always_inline)) inline void *memcpy_best_unaligned(void *dest, const void *src, size_t n) {
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

__attribute__((noinline)) void my_exact_unaligned_memcpy(void *dest, const void *src, size_t n) {
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
        case 32: __builtin_memcpy(__builtin_assume_aligned(dest, 32), __builtin_assume_aligned(src, 32), 32); break;
        case 64: __builtin_memcpy(__builtin_assume_aligned(dest, 32), __builtin_assume_aligned(src, 32), 64); break;
        case 128: __builtin_memcpy(__builtin_assume_aligned(dest, 32), __builtin_assume_aligned(src, 32), 128); break;
        case 256: __builtin_memcpy(__builtin_assume_aligned(dest, 32), __builtin_assume_aligned(src, 32), 256); break;
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
    for (size_t i = 0; i < s; i += 32) {
        _mm256_storeu_si256((__m256i*)(dst + i), _mm256_loadu_si256((const __m256i*)(src_ptr + i)));
    }
}



static inline void __attribute__((always_inline)) memcpy_manual_avx_loop_aligned(void *dest, const void *src, size_t s) {
    char *dst = (char *)dest;
    const char *src_ptr = (const char *)src;
    for (size_t i = 0; i < s; i += 32) {
        _mm256_store_si256((__m256i*)(dst + i), _mm256_load_si256((const __m256i*)(src_ptr + i)));
    }
}

static inline void __attribute__((always_inline)) memcpy_manual_avx512_loop_aligned(void *dest, const void *src, size_t s) {
    char *dst = (char *)dest;
    const char *src_ptr = (const char *)src;
    for (size_t i = 0; i < s; i += 64) {
        _mm512_store_si512((__m512i*)(dst + i), _mm512_load_si512((const __m512i*)(src_ptr + i)));
    }
}


static inline void __attribute__((always_inline)) memcpy_manual_sse_loop(void *dest, const void *src, size_t s) {
    char *dst = (char *)dest;
    const char *src_ptr = (const char *)src;
    for (size_t i = 0; i < s; i += 16) {
        _mm_storeu_si128((__m128i*)(dst + i), _mm_loadu_si128((const __m128i*)(src_ptr + i)));
    }
}


// --- BENCHMARKS ---

static void BM_Threshold_8Aligned_builtins(benchmark::State& state) {
  size_t size = state.range(0);
  char* src = global_src + 8;
  char* dst = global_dst + 8;
  std::memset(src, 'x', size);

  for (auto _ : state) {
    volatile size_t s = size;
    memcpy_threshold_builtins8(dst, src, s);
    benchmark::DoNotOptimize(dst);
    benchmark::ClobberMemory();
  }
}

static void BM_Threshold_16Aligned_builtins(benchmark::State& state) {
  size_t size = MY_ALIGN(state.range(0), 16);
  std::memset(global_src, 'x', size + 8);

  for (auto _ : state) {
    volatile size_t s = size;
    memcpy_threshold_builtins16(global_dst, global_src, s);
    benchmark::DoNotOptimize(global_dst);
    benchmark::ClobberMemory();
  }
}

static void BM_Threshold_16Aligned_memcpy(benchmark::State& state) {
  size_t size = MY_ALIGN(state.range(0), 16);
  std::memset(global_src, 'x', size + 8);

  for (auto _ : state) {
    volatile size_t s = size;
    memcpy_threshold16(global_dst, global_src, s);
    benchmark::DoNotOptimize(global_dst);
    benchmark::ClobberMemory();
  }
}



static void BM_Threshold_AlignedAVX_8ByteAlignedPointers(benchmark::State& state) {
  size_t size = state.range(0);
  std::memset(global_src, 'x', size + 8);

  for (auto _ : state) {
    volatile size_t s = size;
    memcpy_best_aligned(global_dst, global_src, s);
    benchmark::DoNotOptimize(global_dst);
    benchmark::ClobberMemory();
  }
}



static void BM_Threshold_AlignedAVX_UnalignedPointers(benchmark::State& state) {
  size_t size = state.range(0);
  std::memset(global_src, 'x', size + 1);
  char* src = wash(global_src + 8);
  char* dst = wash(global_dst + 8);

  for (auto _ : state) {
    volatile size_t s = size;
    memcpy_best_unaligned(dst, src, s);
    benchmark::DoNotOptimize(dst);
    benchmark::ClobberMemory();
  }
}

static void BM_Switch_Inline_AlignedSize(benchmark::State& state) {
  size_t size = state.range(0);
  size_t aligned_size = size <= 8 ? 8 : (size <= 16 ? 16 : (size <= 32 ? 32 : (size <= 64 ? 64 : (size <= 128 ? 128 : 256))));
  std::memset(global_src, 'x', aligned_size);
  char* src = wash(global_src);
  char* dst = wash(global_dst);

  for (auto _ : state) {
    volatile size_t s = aligned_size;
    memcpy_switch_inline_aligned(dst, src, s);
    benchmark::DoNotOptimize(dst);
    benchmark::ClobberMemory();
  }
}

static void BM_Threshold_UnalignedAVX(benchmark::State& state) {
  size_t size = state.range(0);
  std::memset(global_src, 'x', size + 1);
  char* src = global_src + 8;
  char* dst = global_dst + 8;

  for (auto _ : state) {
    volatile size_t s = size;
    memcpy_threshold_unaligned_avx(dst, src, s);
    benchmark::DoNotOptimize(dst);
    benchmark::ClobberMemory();
  }
}

static void BM_Threshold_AlignedAVX(benchmark::State& state) {
  size_t size = MY_ALIGN(state.range(0), 16);
  char* src = wash(global_src);
  char* dst = wash(global_dst);
  std::memset(src, 'x', size + 256);

  for (auto _ : state) {
    volatile size_t s = size;
    memcpy_threshold_aligned_avx(dst, src, s);
    benchmark::DoNotOptimize(dst);
    benchmark::ClobberMemory();
  }
}

static void BM_Threshold_8ByteMovAVXFallback(benchmark::State& state) {
  size_t size = state.range(0);
  char* src = global_src + 8;
  char* dst = global_dst + 8;
  std::memset(src, 'x', size + 8);

  for (auto _ : state) {
    volatile size_t s = size;
    memcpy_threshold_8byte_mov_avx_fallback(dst, src, s);
    benchmark::DoNotOptimize(dst);
    benchmark::ClobberMemory();
  }
}

static void BM_Switch_NoInline_UnalignedAVX(benchmark::State& state) {
  size_t size = state.range(0);
  char* src = global_src + 8;
  char* dst = global_dst + 8;
  std::memset(global_src, 'x', size);

  for (auto _ : state) {
    volatile size_t s = size;
    my_exact_unaligned_memcpy(dst, src, s);
    benchmark::DoNotOptimize(dst);
    benchmark::ClobberMemory();
  }
}

static void BM_Manual_SSE_Loop_Aligned(benchmark::State& state) {
  size_t size = state.range(0);
  size_t aligned_size = MY_ALIGN(size, 16);
  char* src = wash(global_src);
  char* dst = wash(global_dst);
  std::memset(src, 'x', aligned_size);

  for (auto _ : state) {
    volatile size_t s = aligned_size;
    memcpy_manual_sse_loop(dst, src, s);
    benchmark::DoNotOptimize(dst);
    benchmark::ClobberMemory();
  }
}


static void BM_Manual_AVX_Loop(benchmark::State& state) {
  size_t size = state.range(0);
  char* src = global_src + 8;
  char* dst = global_dst + 8;
  std::memset(src, 'x', size);

  for (auto _ : state) {
    volatile size_t s = size;
    memcpy_manual_avx_loop_unaligned(dst, src, s);
    benchmark::DoNotOptimize(dst);
    benchmark::ClobberMemory();
  }
}


static void BM_Manual_AVX_Loop_Aligned(benchmark::State& state) {
  size_t size = state.range(0);
  size_t aligned_size = MY_ALIGN(size, 32);
  std::memset(global_src, 'x', aligned_size);
  char* src = global_src;
  char* dst = global_dst;

  for (auto _ : state) {
    volatile size_t s = aligned_size;
    memcpy_manual_avx_loop_aligned(dst, src, s);
    benchmark::DoNotOptimize(dst);
    benchmark::ClobberMemory();
  }
}

static void BM_Manual_AVX512_Loop_Aligned(benchmark::State& state) {
  size_t size = state.range(0);
  size_t aligned_size = MY_ALIGN(size, 64);
  std::memset(global_src, 'x', aligned_size);
  char* src = global_src;
  char* dst = global_dst;

  for (auto _ : state) {
    volatile size_t s = aligned_size;
    memcpy_manual_avx512_loop_aligned(dst, src, s);
    benchmark::DoNotOptimize(dst);
    benchmark::ClobberMemory();
  }
}



static void BM_BuiltinMemcpy(benchmark::State& state) {
  size_t size = state.range(0);
  char* src = wash(global_src + 8);
  char* dst = wash(global_dst + 8);
  std::memset(global_src, 'x', size);

  for (auto _ : state) {
    volatile size_t s = size;
    __builtin_memcpy(dst, src, s);
    benchmark::DoNotOptimize(dst);
    benchmark::ClobberMemory();
  }
}

static void BM_Libc_Memcpy(benchmark::State& state) {
  size_t size = state.range(0);
  char* src = wash(global_src + 8);
  char* dst = wash(global_dst + 8);
  std::memset(src, 'x', size);

  for (auto _ : state) {
    volatile size_t s = size;
    memcpy(dst, src, s);
    benchmark::DoNotOptimize(dst);
    benchmark::ClobberMemory();
  }
}
static void BM_Libc_Memcpy_aligned(benchmark::State& state) {
  size_t size = state.range(0);
  char* src = wash(global_src);
  char* dst = wash(global_dst);
  std::memset(src, 'x', size);

  for (auto _ : state) {
    volatile size_t s = size;
    memcpy(dst, src, s);
    benchmark::DoNotOptimize(dst);
    benchmark::ClobberMemory();
  }
}



// 14. BM_Rep_Movsq_Aligned
static void BM_Rep_Movsq_Aligned(benchmark::State& state) {
  size_t size = state.range(0);
  size_t aligned_size = MY_ALIGN(size, 8);
  if (aligned_size == 0) aligned_size = 8;
  std::memset(global_src, 'x', aligned_size);
  char* src = wash(global_src);
  char* dst = wash(global_dst);

  for (auto _ : state) {
    volatile size_t s = aligned_size;
    memcpy_rep_movsq(dst, src, s);
    benchmark::DoNotOptimize(dst);
    benchmark::ClobberMemory();
  }
}

static void CustomArgsAll(benchmark::Benchmark* b) {
  // Small
  // b->Arg(3);
  // b->Arg(5);
  // b->Arg(7);
  b->Arg(8);
  // b->Arg(15);
  b->Arg(16);
  // b->Arg(30);
  b->Arg(32);
  // b->Arg(61);
  b->Arg(64);
  // b->Arg(127);
  b->Arg(128);
  // b->Arg(255);
  b->Arg(256);
  
  b->Arg(1024);
  b->Arg(4096);
  b->Arg(8192);
  b->Arg(16384);
  
}

BENCHMARK(BM_Threshold_AlignedAVX_8ByteAlignedPointers)->Apply(CustomArgsAll);
BENCHMARK(BM_Threshold_AlignedAVX_UnalignedPointers)->Apply(CustomArgsAll);
BENCHMARK(BM_Switch_Inline_AlignedSize)->Apply(CustomArgsAll);
BENCHMARK(BM_Threshold_UnalignedAVX)->Apply(CustomArgsAll);
BENCHMARK(BM_Threshold_AlignedAVX)->Apply(CustomArgsAll);
BENCHMARK(BM_Threshold_8ByteMovAVXFallback)->Apply(CustomArgsAll);
BENCHMARK(BM_BuiltinMemcpy)->Apply(CustomArgsAll);
BENCHMARK(BM_Manual_AVX_Loop)->Apply(CustomArgsAll);
BENCHMARK(BM_Switch_NoInline_UnalignedAVX)->Apply(CustomArgsAll);
BENCHMARK(BM_Libc_Memcpy)->Apply(CustomArgsAll);
BENCHMARK(BM_Libc_Memcpy_aligned)->Apply(CustomArgsAll);
BENCHMARK(BM_Threshold_8Aligned_builtins)->Apply(CustomArgsAll);
BENCHMARK(BM_Threshold_16Aligned_builtins)->Apply(CustomArgsAll);
BENCHMARK(BM_Threshold_16Aligned_memcpy)->Apply(CustomArgsAll);




BENCHMARK(BM_Manual_SSE_Loop_Aligned)->Apply(CustomArgsAll);
BENCHMARK(BM_Manual_AVX_Loop_Aligned)->Apply(CustomArgsAll);
BENCHMARK(BM_Manual_AVX512_Loop_Aligned)->Apply(CustomArgsAll);


BENCHMARK(BM_Rep_Movsq_Aligned)->Apply(CustomArgsAll);
BENCHMARK_MAIN();
