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

#include <portability/memory.h>

#include <util/scoped_malloc.h>

// The __thread storage class modifier isn't well supported on osx, but we
// aren't worried about the performance on osx, so we provide a
// pass-through implementation of scoped mallocs.
#ifdef __APPLE__

namespace toku {

    scoped_malloc::scoped_malloc(const size_t size)
        : m_size(size),
          m_local(false),
          m_buf(toku_xmalloc(size)) {}

    scoped_malloc::~scoped_malloc() {
        toku_free(m_buf);
    }

} // namespace toku

void toku_scoped_malloc_init(void) {}
void toku_scoped_malloc_destroy(void) {}
void toku_scoped_malloc_destroy_set(void) {}
void toku_scoped_malloc_destroy_key(void) {}

#else // __APPLE__

#include <set>
#include <pthread.h>

#include <portability/toku_pthread.h>

namespace toku {

    // see pthread_key handling at the bottom
    //
    // when we use gcc 4.8, we can use the 'thread_local' keyword and proper c++
    // constructors/destructors instead of this pthread / global set wizardy.
    static pthread_key_t tl_stack_destroy_pthread_key;
    class tl_stack;
    std::set<tl_stack *> *global_stack_set;
    toku_mutex_t global_stack_set_mutex = TOKU_MUTEX_INITIALIZER;

    class tl_stack {
        // 1MB
        static const size_t STACK_SIZE = 1 * 1024 * 1024;
        
    public:
        void init() {
            m_stack = reinterpret_cast<char *>(toku_xmalloc(STACK_SIZE));
            m_current_offset = 0;
            int r = pthread_setspecific(tl_stack_destroy_pthread_key, this);
            invariant_zero(r);
        }

        void destroy() {
#if TOKU_SCOPED_MALLOC_DEBUG
            printf("%s %p %p\n", __FUNCTION__, this, m_stack);
#endif
            if (m_stack != NULL) {
                toku_free(m_stack);
                m_stack = NULL;
            }
        }

        // initialize a tl_stack and insert it into the global map
        static void init_and_register(tl_stack *st) {
            st->init();
            invariant_notnull(global_stack_set);

            toku_mutex_lock(&global_stack_set_mutex);
            std::pair<std::set<tl_stack *>::iterator, bool> p = global_stack_set->insert(st);
            invariant(p.second);
            toku_mutex_unlock(&global_stack_set_mutex);
        }

        // destruct a tl_stack and remove it from the global map
        // passed in as void * to match the generic pthread destructor API
        static void destroy_and_deregister(void *key) {
            invariant_notnull(key);
            tl_stack *st = reinterpret_cast<tl_stack *>(key);

            size_t n = 0;
            toku_mutex_lock(&global_stack_set_mutex);
            if (global_stack_set) {
                n = global_stack_set->erase(st);
            }
            toku_mutex_unlock(&global_stack_set_mutex);

            if (n == 1) {
                st->destroy(); // destroy the stack if this function erased it from the set.  otherwise, somebody else destroyed it.
            }
        }

        // Allocate 'size' bytes and return a pointer to the first byte
        void *alloc(const size_t size) {
            if (m_stack == NULL) {
                init_and_register(this);
            }
            invariant(m_current_offset + size <= STACK_SIZE);
            void *mem = &m_stack[m_current_offset];
            m_current_offset += size;
            return mem;
        }

        // Give back a previously allocated region of 'size' bytes.
        void dealloc(const size_t size) {
            invariant(m_current_offset >= size);
            m_current_offset -= size;
        }

        // Get the current size of free-space in bytes.
        size_t get_free_space() const {
            invariant(m_current_offset <= STACK_SIZE);
            return STACK_SIZE - m_current_offset;
        }

    private:
        // Offset of the free region in the stack
        size_t m_current_offset;
        char *m_stack;
    };

    // Each thread has its own local stack.
    static __thread tl_stack local_stack;

    // Memory is allocated from thread-local storage if available, otherwise from malloc(1).
    scoped_malloc::scoped_malloc(const size_t size) :
        m_size(size),
        m_local(local_stack.get_free_space() >= m_size),
        m_buf(m_local ? local_stack.alloc(m_size) : toku_xmalloc(m_size)) {
    }

    scoped_malloc::~scoped_malloc() {
        if (m_local) {
            local_stack.dealloc(m_size);
        } else {
            toku_free(m_buf);
        }
    }

} // namespace toku

// pthread key handling:
// - there is a process-wide pthread key that is associated with the destructor for a tl_stack
// - on process construction, we initialize the key; on destruction, we clean it up.
// - when a thread first uses its tl_stack, it calls pthread_setspecific(&destroy_key, "some key"),
//   associating the destroy key with the tl_stack_destroy_and_deregister destructor
// - when a thread terminates, it calls the associated destructor; tl_stack_destroy_and_deregister.

void toku_scoped_malloc_init(void) {
    toku_mutex_lock(&toku::global_stack_set_mutex);
    invariant_null(toku::global_stack_set);
    toku::global_stack_set = new std::set<toku::tl_stack *>();
    toku_mutex_unlock(&toku::global_stack_set_mutex);

    int r = pthread_key_create(&toku::tl_stack_destroy_pthread_key,
                               toku::tl_stack::destroy_and_deregister);
    invariant_zero(r);
}

void toku_scoped_malloc_destroy(void) {
    toku_scoped_malloc_destroy_key();
    toku_scoped_malloc_destroy_set();
}

void toku_scoped_malloc_destroy_set(void) {
    toku_mutex_lock(&toku::global_stack_set_mutex);
    invariant_notnull(toku::global_stack_set);
    // Destroy any tl_stacks that were registered as thread locals but did not
    // get a chance to clean up using the pthread key destructor (because this code
    // is now running before those threads fully shutdown)
    for (std::set<toku::tl_stack *>::iterator i = toku::global_stack_set->begin();
         i != toku::global_stack_set->end(); i++) {
        (*i)->destroy();
    }
    delete toku::global_stack_set;
    toku::global_stack_set = nullptr;
    toku_mutex_unlock(&toku::global_stack_set_mutex);
}

void toku_scoped_malloc_destroy_key(void) {
    int r = pthread_key_delete(toku::tl_stack_destroy_pthread_key);
    invariant_zero(r);
}

#endif // !__APPLE__
