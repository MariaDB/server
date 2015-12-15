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

#include <algorithm>
#include <cstdlib>
#include <memory>

namespace ftcxx {

    /**
     * Buffer implements a flat memory buffer intended for FIFO usage
     * where allocations are piecemeal but consumption is total.  That is,
     * we repeatedly fill up the buffer with small allocations, and
     * periodically consume all entries and clear the buffer.
     *
     * For now, the implementation uses a doubling array strategy,
     * starting at 1kB growing to a maximum advised capacity of 256kB,
     * never shrinking the buffer.
     *
     * However, we hope to find a better strategy.
     *
     * Facebook's FBVector claims that a reallocation growth factor of 1.5
     * rather than 2 hits their sweet spot, and they claim to have
     * additional improvements by integrating with jemalloc (which we use
     * as well).
     *
     * Additionally, it may be advantageous to use some memarena-style
     * tricks like allocating a separate overflow buffer to avoid
     * memcpying when we're close to our intended maximum capacity, and
     * also to avoid wasting extra memory if we overflow our maximum
     * capacity once but never do so again.
     */
    class Buffer {
    public:

        Buffer();

        explicit Buffer(size_t capacity);

        Buffer(const Buffer &) = delete;
        Buffer& operator=(const Buffer &) = delete;

        Buffer(Buffer&& other)
            : _cur(0),
              _end(0),
              _capacity(0),
              _buf(nullptr, &std::free)
        {
            std::swap(_cur, other._cur);
            std::swap(_end, other._end);
            std::swap(_capacity, other._capacity);
            std::swap(_buf, other._buf);
        }

        Buffer& operator=(Buffer&& other) {
            std::swap(_cur, other._cur);
            std::swap(_end, other._end);
            std::swap(_capacity, other._capacity);
            std::swap(_buf, other._buf);
            return *this;
        }

        // Producer API:

        /**
         * Allocate room for sz more bytes at the end, and return a
         * pointer to the allocated space.  This causes at most one
         * realloc and memcpy of existing data.
         */
        char *alloc(size_t sz);

        /**
         * Returns true if we're close to our maximum capacity.  If so,
         * the producer should stop and allow the consumer to clear the
         * buffer.
         */
        bool full() const;

        // Consumer API:

        /**
         * Returns true if there are more unconsumed bytes in the buffer.
         */
        bool more() const;

        /**
         * Returns a pointer to the next unconsumed byte in the buffer.
         */
        char *current() const;

        /**
         * Advances the unconsumed position pointer by sz bytes.
         */
        void advance(size_t sz);

        /**
         * Free all allocated space.
         */
        void clear();

    private:

        size_t _cur;
        size_t _end;
        size_t _capacity;
        std::unique_ptr<char, void (*)(void*)> _buf;

        static const size_t INITIAL_CAPACITY;
        static const size_t MAXIMUM_CAPACITY;
        static const double FULLNESS_RATIO;

        void init();

        static size_t next_alloc_size(size_t sz);

        void grow(size_t sz);

        char *raw(size_t i=0) const {
            return &(_buf.get()[i]);
        }
    };

} // namespace ftcxx
