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

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <memory>

#include "buffer.hpp"
#include "malloc_utils.hpp"

namespace mu = malloc_utils;

namespace ftcxx {

    const size_t Buffer::INITIAL_CAPACITY = 1<<10;
    const size_t Buffer::MAXIMUM_CAPACITY = 1<<18;
    const double Buffer::FULLNESS_RATIO = 0.9;

    Buffer::Buffer()
        : _cur(0),
          _end(0),
          _capacity(INITIAL_CAPACITY),
          _buf(nullptr, &std::free)
    {
        init();
    }

    Buffer::Buffer(size_t capacity)
        : _end(0),
          _capacity(capacity),
          _buf(nullptr, &std::free)
    {
        init();
    }

    char *Buffer::alloc(size_t sz) {
        grow(sz);
        char *p = raw(_end);
        _end += sz;
        return p;
    }

    bool Buffer::full() const {
        return _end > MAXIMUM_CAPACITY * FULLNESS_RATIO;
    }

    bool Buffer::more() const {
        return _cur < _end;
    }

    char *Buffer::current() const {
        return raw(_cur);
    }

    void Buffer::advance(size_t sz) {
        _cur += sz;
    }

    void Buffer::clear() {
        _cur = 0;
        _end = 0;
    }

    void Buffer::init() {
        _buf.reset(static_cast<char *>(mu::checkedMalloc(_capacity)));
    }

    /**
     * Implements our growth strategy.  Currently we double until we get
     * up to 4kB so that we can quickly reach the point where jemalloc can
     * help us resize in-place, but after that point we grow by a factor
     * of 1.5x.
     *
     * FBVector doubles once it is bigger than 128kB, but I don't think we
     * actually want to because that's about when we want to stop growing.
     */
    size_t Buffer::next_alloc_size(size_t sz) {
        if (sz < mu::jemallocMinInPlaceExpandable) {
            return sz * 2;
        }
#if 0
        else if (sz > (128<<10)) {
            return sz * 2;
        }
#endif
        else {
            return (sz * 3 + 1) / 2;
        }
    }

    void Buffer::grow(size_t sz) {
        size_t new_capacity = _capacity;
        while (new_capacity < _end + sz) {
            new_capacity = next_alloc_size(new_capacity);
        }
        assert(new_capacity >= _capacity);  // overflow?
        if (new_capacity > _capacity) {
            // This section isn't exception-safe, but smartRealloc already
            // isn't.  The only thing we can throw in here is
            // std::bad_alloc, in which case we're kind of screwed anyway.
            new_capacity = mu::goodMallocSize(new_capacity);
            _buf.reset(static_cast<char *>(mu::smartRealloc(_buf.release(), _end, _capacity, new_capacity, _capacity)));
        }
    }

} // namespace ftcxx
