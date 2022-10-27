/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#ident "$Id$"
/*======
This file is part of PerconaFT.


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.

----------------------------------------

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License, version 3,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.
======= */

#ident "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

#pragma once

/**
 * These functions are extracted from Facebook's folly library, which
 * integrates well with jemalloc.  See
 * https://github.com/facebook/folly/blob/master/folly/Malloc.h
 */

#include <algorithm>
#include <cassert>
#include <cstdlib>

#if defined(HAVE_BITS_FUNCTEXCEPT_H) && HAVE_BITS_FUNCTEXCEPT_H

# include <bits/functexcept.h>

#else

# include <stdexcept>

namespace std {

    void __throw_bad_alloc();

}

#endif

/**
 * Declare *allocx() and mallctl() as weak symbols. These will be provided by
 * jemalloc if we are using jemalloc, or will be NULL if we are using another
 * malloc implementation.
 */
extern "C" void* mallocx(size_t, int)
    __attribute__((__weak__));
extern "C" void* rallocx(void*, size_t, int)
    __attribute__((__weak__));
extern "C" size_t xallocx(void*, size_t, size_t, int)
    __attribute__((__weak__));
extern "C" size_t sallocx(const void*, int)
    __attribute__((__weak__));
extern "C" void dallocx(void*, int)
    __attribute__((__weak__));
extern "C" size_t nallocx(size_t, int)
    __attribute__((__weak__));
extern "C" int mallctl(const char*, void*, size_t*, void*, size_t)
    __attribute__((__weak__));

namespace malloc_utils {

    bool usingJEMallocSlow();

    /**
     * Determine if we are using jemalloc or not.
     */
    inline bool usingJEMalloc() {
        // Checking for rallocx != NULL is not sufficient; we may be in a
        // dlopen()ed module that depends on libjemalloc, so rallocx is
        // resolved, but the main program might be using a different
        // memory allocator. Look at the implementation of
        // usingJEMallocSlow() for the (hacky) details.
        static const bool result = usingJEMallocSlow();
        return result;
    }

    /**
     * For jemalloc's size classes, see
     * http://www.canonware.com/download/jemalloc/jemalloc-latest/doc/jemalloc.html
     */
    inline size_t goodMallocSize(size_t minSize) noexcept {
        if (!usingJEMalloc()) {
            // Not using jemalloc - no smarts
            return minSize;
        }
        size_t goodSize;
        if (minSize <= 64) {
            // Choose smallest allocation to be 64 bytes - no tripping
            // over cache line boundaries, and small string optimization
            // takes care of short strings anyway.
            goodSize = 64;
        } else if (minSize <= 512) {
            // Round up to the next multiple of 64; we don't want to trip
            // over cache line boundaries.
            goodSize = (minSize + 63) & ~size_t(63);
        } else if (minSize <= 3584) {
            // Round up to the next multiple of 256.  For some size
            // classes jemalloc will additionally round up to the nearest
            // multiple of 512, hence the nallocx() call.
            goodSize = nallocx((minSize + 255) & ~size_t(255), 0);
        } else if (minSize <= 4072 * 1024) {
            // Round up to the next multiple of 4KB
            goodSize = (minSize + 4095) & ~size_t(4095);
        } else {
            // Holy Moly
            // Round up to the next multiple of 4MB
            goodSize = (minSize + 4194303) & ~size_t(4194303);
        }
        assert(nallocx(goodSize, 0) == goodSize);
        return goodSize;
    }

    static const size_t jemallocMinInPlaceExpandable = 4096;

    /**
     * Trivial wrappers around malloc, calloc, realloc that check for
     * allocation failure and throw std::bad_alloc in that case.
     */
    inline void* checkedMalloc(size_t size) {
        void* p = malloc(size);
        if (!p) std::__throw_bad_alloc();
        return p;
    }

    inline void* checkedCalloc(size_t n, size_t size) {
        void* p = calloc(n, size);
        if (!p) std::__throw_bad_alloc();
        return p;
    }

    inline void* checkedRealloc(void* ptr, size_t size) {
        void* p = realloc(ptr, size);
        if (!p) std::__throw_bad_alloc();
        return p;
    }

    /**
     * This function tries to reallocate a buffer of which only the first
     * currentSize bytes are used. The problem with using realloc is that
     * if currentSize is relatively small _and_ if realloc decides it
     * needs to move the memory chunk to a new buffer, then realloc ends
     * up copying data that is not used. It's impossible to hook into
     * GNU's malloc to figure whether expansion will occur in-place or as
     * a malloc-copy-free troika. (If an expand_in_place primitive would
     * be available, smartRealloc would use it.) As things stand, this
     * routine just tries to call realloc() (thus benefitting of potential
     * copy-free coalescing) unless there's too much slack memory.
     */
    inline void* smartRealloc(void* p,
                              const size_t currentSize,
                              const size_t currentCapacity,
                              const size_t newCapacity,
                              size_t &realNewCapacity) {
        assert(p);
        assert(currentSize <= currentCapacity &&
               currentCapacity < newCapacity);

        if (usingJEMalloc()) {
            // using jemalloc's API. Don't forget that jemalloc can never
            // grow in place blocks smaller than 4096 bytes.
            //
            // NB: newCapacity may not be precisely equal to a jemalloc
            // size class, i.e. newCapacity is not guaranteed to be the
            // result of a goodMallocSize() call, therefore xallocx() may
            // return more than newCapacity bytes of space.  Use >= rather
            // than == to check whether xallocx() successfully expanded in
            // place.
            size_t realNewCapacity_;
            if (currentCapacity >= jemallocMinInPlaceExpandable &&
                (realNewCapacity_ = xallocx(p, newCapacity, 0, 0)) >= newCapacity) {
                // Managed to expand in place
                realNewCapacity = realNewCapacity_;
                return p;
            }
            // Cannot expand; must move
            char * const result = static_cast<char *>(checkedMalloc(newCapacity));
            char *cp = static_cast<char *>(p);
            std::copy(cp, cp + currentSize, result);
            free(p);
            realNewCapacity = newCapacity;
            return result;
        }

        // No jemalloc no honey
        auto const slack = currentCapacity - currentSize;
        if (slack * 2 > currentSize) {
            // Too much slack, malloc-copy-free cycle:
            char * const result = static_cast<char *>(checkedMalloc(newCapacity));
            char *cp = static_cast<char *>(p);
            std::copy(cp, cp + currentSize, result);
            free(p);
            realNewCapacity = newCapacity;
            return result;
        }
        // If there's not too much slack, we realloc in hope of coalescing
        realNewCapacity = newCapacity;
        return checkedRealloc(p, newCapacity);
    }

} // namespace malloc_utils
