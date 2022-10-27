/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
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

#include <string.h>

namespace toku {

    class scoped_malloc {
    public:
        // Memory is allocated from thread-local storage if available, otherwise from malloc(3).
        scoped_malloc(const size_t size);

        ~scoped_malloc();

        void *get() const {
            return m_buf;
        }

    private:
        // Non-copyable
        scoped_malloc();

        const size_t m_size;
        const bool m_local;
        void *const m_buf;
    };

    class scoped_calloc : public scoped_malloc {
    public:
        // A scoped malloc whose bytes are initialized to zero, as in calloc(3)
        scoped_calloc(const size_t size) :
            scoped_malloc(size) {
            memset(scoped_malloc::get(), 0, size);
        }
    };

    class scoped_malloc_aligned : public scoped_malloc {
    public:
        scoped_malloc_aligned(const size_t size, const size_t alignment) :
            scoped_malloc(size + alignment) {
            invariant(size >= alignment);
            invariant(alignment > 0);
            const uintptr_t addr = reinterpret_cast<uintptr_t>(scoped_malloc::get());
            const uintptr_t aligned_addr = (addr + alignment) - (addr % alignment);
            invariant(aligned_addr < addr + size + alignment);
            m_aligned_buf = reinterpret_cast<char *>(aligned_addr);
        }

        void *get() const {
            return m_aligned_buf;
        }

    private:
        void *m_aligned_buf;
    };

} // namespace toku

void toku_scoped_malloc_init(void);

void toku_scoped_malloc_destroy(void);

void toku_scoped_malloc_destroy_set(void);

void toku_scoped_malloc_destroy_key(void);

