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

#include <cstdint>

#include "malloc_utils.hpp"

#if !HAVE_BITS_FUNCTEXCEPT_H

namespace std {

    void __throw_bad_alloc() {
        throw bad_alloc();
    }

} // namespace std

#endif

namespace malloc_utils {

    // How do we determine that we're using jemalloc?
    // In the hackiest way possible. We allocate memory using malloc() and see if
    // the per-thread counter of allocated memory increases. This makes me feel
    // dirty inside. Also note that this requires jemalloc to have been compiled
    // with --enable-stats.
    bool usingJEMallocSlow() {
        // Some platforms (*cough* OSX *cough*) require weak symbol checks to be
        // in the form if (mallctl != nullptr). Not if (mallctl) or if (!mallctl)
        // (!!). http://goo.gl/xpmctm
        if (mallocx == nullptr || rallocx == nullptr || xallocx == nullptr
            || sallocx == nullptr || dallocx == nullptr || nallocx == nullptr
            || mallctl == nullptr) {
            return false;
        }

        // "volatile" because gcc optimizes out the reads from *counter, because
        // it "knows" malloc doesn't modify global state...
        volatile uint64_t* counter;
        size_t counterLen = sizeof(uint64_t*);

        if (mallctl("thread.allocatedp", static_cast<void*>(&counter), &counterLen,
                    nullptr, 0) != 0) {
            return false;
        }

        if (counterLen != sizeof(uint64_t*)) {
            return false;
        }

        uint64_t origAllocated = *counter;

        void* ptr = malloc(1);
        if (!ptr) {
            // wtf, failing to allocate 1 byte
            return false;
        }
        free(ptr);

        return (origAllocated != *counter);
    }

} // namespace malloc_utils
