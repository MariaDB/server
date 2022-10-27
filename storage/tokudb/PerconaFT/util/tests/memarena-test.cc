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

#include <string.h>

#include "portability/toku_assert.h"

#include "util/memarena.h"

class memarena_unit_test {
private:
    static const int magic = 37;

    template <typename F>
    void iterate_chunks(memarena *ma, F &fn) {
        for (memarena::chunk_iterator it(ma); it.more(); it.next()) {
            size_t used = 0;
            const void *buf = it.current(&used);
            fn(buf, used);
        }
    }

    void test_create(size_t size) {
        memarena ma;
        ma.create(size);
        invariant(ma._current_chunk.size == size);
        invariant(ma._current_chunk.used == 0);
        if (size == 0) {
            invariant_null(ma._current_chunk.buf);
        } else {
            invariant_notnull(ma._current_chunk.buf);
        }

        // make sure memory was allocated ok by
        // writing to buf and reading it back
        if (size > 0) {
            memset(ma._current_chunk.buf, magic, size);
        }
        for (size_t i = 0; i < size; i++) {
            const char *buf = reinterpret_cast<char *>(ma._current_chunk.buf);
            invariant(buf[i] == magic);
        }
        ma.destroy();
    }

    void test_malloc(size_t size) {
        memarena ma;
        ma.create(14);
        void *v = ma.malloc_from_arena(size);
        invariant_notnull(v);

        // make sure memory was allocated ok by
        // writing to buf and reading it back
        if (size > 0) {
            memset(ma._current_chunk.buf, magic, size);
        }
        for (size_t i = 0; i < size; i++) {
            const char *c = reinterpret_cast<char *>(ma._current_chunk.buf);
            invariant(c[i] == magic);
        }
        ma.destroy();
    }

    static void test_iterate_fn(const void *buf, size_t used) {
        for (size_t i = 0; i < used; i++) {
            const char *c = reinterpret_cast<const char *>(buf);
            invariant(c[i] == (char) ((intptr_t) &c[i]));
        }
    }

    void test_iterate(size_t size) {
        memarena ma;
        ma.create(14);
        for (size_t k = 0; k < size / 64; k += 64) {
            void *v = ma.malloc_from_arena(64);
            for (size_t i = 0; i < 64; i++) {
                char *c = reinterpret_cast<char *>(v);
                c[i] = (char) ((intptr_t) &c[i]);
            }
        }
        size_t rest = size % 64;
        if (rest != 0) {
            void *v = ma.malloc_from_arena(64);
            for (size_t i = 0; i < 64; i++) {
                char *c = reinterpret_cast<char *>(v);
                c[i] = (char) ((intptr_t) &c[i]);
            }
        }

        iterate_chunks(&ma, test_iterate_fn);
        ma.destroy();
    }

    void test_move_memory(size_t size) {
        memarena ma;
        ma.create(14);
        for (size_t k = 0; k < size / 64; k += 64) {
            void *v = ma.malloc_from_arena(64);
            for (size_t i = 0; i < 64; i++) {
                char *c = reinterpret_cast<char *>(v);
                c[i] = (char) ((intptr_t) &c[i]);
            }
        }
        size_t rest = size % 64;
        if (rest != 0) {
            void *v = ma.malloc_from_arena(64);
            for (size_t i = 0; i < 64; i++) {
                char *c = reinterpret_cast<char *>(v);
                c[i] = (char) ((intptr_t) &c[i]);
            }
        }

        memarena ma2;
        ma.move_memory(&ma2);
        iterate_chunks(&ma2, test_iterate_fn);

        ma.destroy();
        ma2.destroy();
    }

public:
    void test() {
        test_create(0);
        test_create(64);
        test_create(128 * 1024 * 1024);
        test_malloc(0);
        test_malloc(63);
        test_malloc(64);
        test_malloc(64 * 1024 * 1024);
        test_malloc((64 * 1024 * 1024) + 1);
        test_iterate(0);
        test_iterate(63);
        test_iterate(128 * 1024);
        test_iterate(64 * 1024 * 1024);
        test_iterate((64 * 1024 * 1024) + 1);
        test_move_memory(0);
        test_move_memory(1);
        test_move_memory(63);
        test_move_memory(65);
        test_move_memory(65 * 1024 * 1024);
        test_move_memory(101 * 1024 * 1024);
    }
};

int main(void) {
    memarena_unit_test test;
    test.test();
    return 0;
}
