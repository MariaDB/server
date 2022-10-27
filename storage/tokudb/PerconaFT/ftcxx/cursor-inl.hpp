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
#include <cstdint>
#include <utility>

#include <db.h>

#include "buffer.hpp"
#include "db_txn.hpp"
#include "exceptions.hpp"
#include "slice.hpp"

namespace ftcxx {

    class DB;

    template<class Comparator>
    bool Bounds::check(Comparator &cmp, const IterationStrategy &strategy, const Slice &key) const {
        int c;
        if (strategy.forward) {
            if (_right_infinite) {
                return true;
            }
            c = cmp(key, _right);
        } else {
            if (_left_infinite) {
                return true;
            }
            c = cmp(_left, key);
        }
        if (c > 0 || (c == 0 && _end_exclusive)) {
            return false;
        }
        return true;
    }

    template<class Comparator, class Handler>
    CallbackCursor<Comparator, Handler>::CallbackCursor(const DBEnv &env, const DBTxn &txn,
                                                        Comparator &&cmp, Handler &&handler)
        : _dbc(env, txn),
          _iteration_strategy(IterationStrategy(true, true)),
          _bounds(DB(env.env()->get_db_for_directory(env.env())), Bounds::Infinite(), Bounds::Infinite(), false),
          _cmp(std::forward<Comparator>(cmp)),
          _handler(std::forward<Handler>(handler)),
          _finished(false)
    {
        init();
    }

    template<class Comparator, class Handler>
    CallbackCursor<Comparator, Handler>::CallbackCursor(const DB &db, const DBTxn &txn, int flags,
                                                        IterationStrategy iteration_strategy,
                                                        Bounds bounds,
                                                        Comparator &&cmp, Handler &&handler)
        : _dbc(db, txn, flags),
          _iteration_strategy(iteration_strategy),
          _bounds(std::move(bounds)),
          _cmp(std::forward<Comparator>(cmp)),
          _handler(std::forward<Handler>(handler)),
          _finished(false)
    {
        init();
    }

    template<class Comparator, class Handler>
    void CallbackCursor<Comparator, Handler>::init() {
        if (!_dbc.set_range(_iteration_strategy, _bounds, getf_callback, this)) {
            _finished = true;
        }
    }

    template<class Comparator, class Handler>
    int CallbackCursor<Comparator, Handler>::getf(const DBT *key, const DBT *val) {
        if (!_bounds.check(_cmp, _iteration_strategy, Slice(*key))) {
            _finished = true;
            return -1;
        }

        if (!_handler(key, val)) {
            return 0;
        }

        return TOKUDB_CURSOR_CONTINUE;
    }

    template<class Comparator, class Handler>
    bool CallbackCursor<Comparator, Handler>::consume_batch() {
        if (!_dbc.advance(_iteration_strategy, getf_callback, this)) {
            _finished = true;
        }
        return !_finished;
    }

    template<class Comparator, class Handler>
    void CallbackCursor<Comparator, Handler>::seek(const Slice &key) {
        if (_iteration_strategy.forward) {
            _bounds.set_left(key);
        } else {
            _bounds.set_right(key);
        }
        if (!_dbc.set_range(_iteration_strategy, _bounds, getf_callback, this)) {
            _finished = true;
        }
    }

    template<class Predicate>
    inline void BufferAppender<Predicate>::marshall(char *dest, const DBT *key, const DBT *val) {
        uint32_t *keylen = reinterpret_cast<uint32_t *>(&dest[0]);
        uint32_t *vallen = reinterpret_cast<uint32_t *>(&dest[sizeof *keylen]);
        *keylen = key->size;
        *vallen = val->size;

        char *p = &dest[(sizeof *keylen) + (sizeof *vallen)];

        const char *kp = static_cast<char *>(key->data);
        std::copy(kp, kp + key->size, p);

        p += key->size;

        const char *vp = static_cast<char *>(val->data);
        std::copy(vp, vp + val->size, p);
    }

    template<class Predicate>
    inline void BufferAppender<Predicate>::unmarshall(char *src, DBT *key, DBT *val) {
        const uint32_t *keylen = reinterpret_cast<uint32_t *>(&src[0]);
        const uint32_t *vallen = reinterpret_cast<uint32_t *>(&src[sizeof *keylen]);
        key->size = *keylen;
        val->size = *vallen;
        char *p = &src[(sizeof *keylen) + (sizeof *vallen)];
        key->data = p;
        val->data = p + key->size;
    }

    template<class Predicate>
    inline void BufferAppender<Predicate>::unmarshall(char *src, Slice &key, Slice &val) {
        const uint32_t *keylen = reinterpret_cast<uint32_t *>(&src[0]);
        const uint32_t *vallen = reinterpret_cast<uint32_t *>(&src[sizeof *keylen]);
        char *p = &src[(sizeof *keylen) + (sizeof *vallen)];
        key = Slice(p, *keylen);
        val = Slice(p + *keylen, *vallen);
    }

    template<class Predicate>
    inline bool BufferAppender<Predicate>::operator()(const DBT *key, const DBT *val) {
        if (_filter(Slice(*key), Slice(*val))) {
            size_t needed = marshalled_size(key->size, val->size);
            char *dest = _buf.alloc(needed);
            marshall(dest, key, val);
        }
        return !_buf.full();
    }

    template<class Comparator, class Predicate>
    BufferedCursor<Comparator, Predicate>::BufferedCursor(const DBEnv &env, const DBTxn &txn,
                                                          Comparator &&cmp, Predicate &&filter)
        : _buf(),
          _cur(env, txn, std::forward<Comparator>(cmp), Appender(_buf, std::forward<Predicate>(filter)))
    {}

    template<class Comparator, class Predicate>
    BufferedCursor<Comparator, Predicate>::BufferedCursor(const DB &db, const DBTxn &txn, int flags,
                                                          IterationStrategy iteration_strategy,
                                                          Bounds bounds,
                                                          Comparator &&cmp, Predicate &&filter)
        : _buf(),
          _cur(db, txn, flags,
               iteration_strategy,
               std::move(bounds),
               std::forward<Comparator>(cmp), Appender(_buf, std::forward<Predicate>(filter)))
    {}

    template<class Comparator, class Predicate>
    bool BufferedCursor<Comparator, Predicate>::next(DBT *key, DBT *val) {
        if (!_buf.more() && !_cur.finished()) {
            _buf.clear();
            _cur.consume_batch();
        }

        if (!_buf.more()) {
            return false;
        }

        char *src = _buf.current();
        Appender::unmarshall(src, key, val);
        _buf.advance(Appender::marshalled_size(key->size, val->size));
        return true;
    }

    template<class Comparator, class Predicate>
    bool BufferedCursor<Comparator, Predicate>::next(Slice &key, Slice &val) {
        if (!_buf.more() && !_cur.finished()) {
            _buf.clear();
            _cur.consume_batch();
        }

        if (!_buf.more()) {
            return false;
        }

        char *src = _buf.current();
        Appender::unmarshall(src, key, val);
        _buf.advance(Appender::marshalled_size(key.size(), val.size()));
        return true;
    }

    template<class Comparator, class Predicate>
    void BufferedCursor<Comparator, Predicate>::seek(const Slice &key) {
        _buf.clear();
        _cur.seek(key);
    }

    template<class Comparator>
    SimpleCursor<Comparator>::SimpleCursor(const DBEnv &env, const DBTxn &txn, Comparator &&cmp,
                                           Slice &key, Slice &val)
        : _copier(key, val),
          _cur(env, txn, std::forward<Comparator>(cmp), _copier)
    {}

    template<class Comparator>
    SimpleCursor<Comparator>::SimpleCursor(const DB &db, const DBTxn &txn, int flags,
                                           IterationStrategy iteration_strategy,
                                           Bounds bounds, Comparator &&cmp,
                                           Slice &key, Slice &val)
        : _copier(key, val),
          _cur(db, txn, flags,
               iteration_strategy,
               std::move(bounds),
               std::forward<Comparator>(cmp), _copier)
    {}

    template<class Comparator>
    bool SimpleCursor<Comparator>::next() {
        return _cur.consume_batch();
    }

    template<class Comparator>
    void SimpleCursor<Comparator>::seek(const Slice &key) {
        _cur.seek(key);
    }

    template<class Comparator, class Handler>
    CallbackCursor<Comparator, Handler> DB::cursor(const DBTxn &txn, DBT *left, DBT *right,
                                                   Comparator &&cmp, Handler &&handler, int flags,
                                                   bool forward, bool end_exclusive, bool prelock) const {
        IterationStrategy strategy(forward, prelock);
        return CallbackCursor<Comparator, Handler>(*this, txn, flags, strategy,
                                                   Bounds(*this, Slice(*left), Slice(*right), end_exclusive),
                                                   std::forward<Comparator>(cmp), std::forward<Handler>(handler));
    }

    template<class Comparator, class Handler>
    CallbackCursor<Comparator, Handler> DB::cursor(const DBTxn &txn, const Slice &start_key,
                                                   Comparator &&cmp, Handler &&handler, int flags,
                                                   bool forward, bool end_exclusive, bool prelock) const {
        IterationStrategy strategy(forward, prelock);
        Bounds bounds = forward
            ? Bounds(*this, start_key, Bounds::Infinite(), end_exclusive)
            : Bounds(*this, Bounds::Infinite(), start_key, end_exclusive);
        return CallbackCursor<Comparator, Handler>(*this, txn, flags, strategy, std::move(bounds),
                                                   std::forward<Comparator>(cmp), std::forward<Handler>(handler));
    }

    template<class Comparator, class Handler>
    CallbackCursor<Comparator, Handler> DB::cursor(const DBTxn &txn, const Slice &left, const Slice &right,
                                                   Comparator &&cmp, Handler &&handler, int flags,
                                                   bool forward, bool end_exclusive, bool prelock) const {
        IterationStrategy strategy(forward, prelock);
        return CallbackCursor<Comparator, Handler>(*this, txn, flags, strategy,
                                                   Bounds(*this, left, right, end_exclusive),
                                                   std::forward<Comparator>(cmp), std::forward<Handler>(handler));
    }

    template<class Comparator, class Handler>
    CallbackCursor<Comparator, Handler> DB::cursor(const DBTxn &txn, Comparator &&cmp, Handler &&handler,
                                                   int flags, bool forward, bool prelock) const {
        IterationStrategy strategy(forward, prelock);
        return CallbackCursor<Comparator, Handler>(*this, txn, flags, strategy,
                                                   Bounds(*this, Bounds::Infinite(), Bounds::Infinite(), false),
                                                   std::forward<Comparator>(cmp), std::forward<Handler>(handler));
    }

    template<class Comparator, class Predicate>
    BufferedCursor<Comparator, Predicate> DB::buffered_cursor(const DBTxn &txn, DBT *left, DBT *right,
                                                              Comparator &&cmp, Predicate &&filter, int flags,
                                                              bool forward, bool end_exclusive, bool prelock) const {
        IterationStrategy strategy(forward, prelock);
        return BufferedCursor<Comparator, Predicate>(*this, txn, flags, strategy,
                                                     Bounds(*this, Slice(*left), Slice(*right), end_exclusive),
                                                     std::forward<Comparator>(cmp), std::forward<Predicate>(filter));
    }

    template<class Comparator, class Predicate>
    BufferedCursor<Comparator, Predicate> DB::buffered_cursor(const DBTxn &txn, const Slice &start_key,
                                                              Comparator &&cmp, Predicate &&filter, int flags,
                                                              bool forward, bool end_exclusive, bool prelock) const {
        IterationStrategy strategy(forward, prelock);
        Bounds bounds = forward
            ? Bounds(*this, start_key, Bounds::Infinite(), end_exclusive)
            : Bounds(*this, Bounds::Infinite(), start_key, end_exclusive);
        return BufferedCursor<Comparator, Predicate>(*this, txn, flags, strategy, std::move(bounds),
                                                     std::forward<Comparator>(cmp), std::forward<Predicate>(filter));
    }

    template<class Comparator, class Predicate>
    BufferedCursor<Comparator, Predicate> DB::buffered_cursor(const DBTxn &txn, const Slice &left, const Slice &right,
                                                              Comparator &&cmp, Predicate &&filter, int flags,
                                                              bool forward, bool end_exclusive, bool prelock) const {
        IterationStrategy strategy(forward, prelock);
        return BufferedCursor<Comparator, Predicate>(*this, txn, flags, strategy,
                                                     Bounds(*this, left, right, end_exclusive),
                                                     std::forward<Comparator>(cmp), std::forward<Predicate>(filter));
    }

    template<class Comparator, class Predicate>
    BufferedCursor<Comparator, Predicate> DB::buffered_cursor(const DBTxn &txn, Comparator &&cmp, Predicate &&filter,
                                                              int flags, bool forward, bool prelock) const {
        IterationStrategy strategy(forward, prelock);
        return BufferedCursor<Comparator, Predicate>(*this, txn, flags, strategy,
                                                     Bounds(*this, Bounds::Infinite(), Bounds::Infinite(), false),
                                                     std::forward<Comparator>(cmp), std::forward<Predicate>(filter));
    }

    template<class Comparator>
    SimpleCursor<Comparator> DB::simple_cursor(const DBTxn &txn, DBT *left, DBT *right,
                                               Comparator &&cmp, Slice &key, Slice &val, int flags,
                                               bool forward, bool end_exclusive, bool prelock) const {
        IterationStrategy strategy(forward, prelock);
        return SimpleCursor<Comparator>(*this, txn, flags, strategy,
                                        Bounds(*this, Slice(*left), Slice(*right), end_exclusive),
                                        std::forward<Comparator>(cmp), key, val);
    }

    template<class Comparator>
    SimpleCursor<Comparator> DB::simple_cursor(const DBTxn &txn, const Slice &start_key,
                                               Comparator &&cmp, Slice &key, Slice &val, int flags,
                                               bool forward, bool end_exclusive, bool prelock) const {
        IterationStrategy strategy(forward, prelock);
        Bounds bounds = forward
            ? Bounds(*this, start_key, Bounds::Infinite(), end_exclusive)
            : Bounds(*this, Bounds::Infinite(), start_key, end_exclusive);
        return SimpleCursor<Comparator>(*this, txn, flags, strategy, std::move(bounds),
                                        std::forward<Comparator>(cmp), key, val);
    }

    template<class Comparator>
    SimpleCursor<Comparator> DB::simple_cursor(const DBTxn &txn, const Slice &left, const Slice &right,
                                               Comparator &&cmp, Slice &key, Slice &val, int flags,
                                               bool forward, bool end_exclusive, bool prelock) const {
        IterationStrategy strategy(forward, prelock);
        return SimpleCursor<Comparator>(*this, txn, flags, strategy,
                                        Bounds(*this, left, right, end_exclusive),
                                        std::forward<Comparator>(cmp), key, val);
    }

    template<class Comparator>
    SimpleCursor<Comparator> DB::simple_cursor(const DBTxn &txn, Comparator &&cmp, Slice &key, Slice &val,
                                               int flags, bool forward, bool prelock) const {
        IterationStrategy strategy(forward, prelock);
        return SimpleCursor<Comparator>(*this, txn, flags, strategy,
                                        Bounds(*this, Bounds::Infinite(), Bounds::Infinite(), false),
                                        std::forward<Comparator>(cmp), key, val);
    }

    template<class Comparator, class Handler>
    CallbackCursor<Comparator, Handler> DBEnv::cursor(const DBTxn &txn, Comparator &&cmp, Handler &&handler) const {
        return CallbackCursor<Comparator, Handler>(*this, txn, std::forward<Comparator>(cmp), std::forward<Handler>(handler));
    }

    template<class Comparator, class Predicate>
    BufferedCursor<Comparator, Predicate> DBEnv::buffered_cursor(const DBTxn &txn, Comparator &&cmp, Predicate &&filter) const {
        return BufferedCursor<Comparator, Predicate>(*this, txn, std::forward<Comparator>(cmp), std::forward<Predicate>(filter));
    }

    template<class Comparator>
    SimpleCursor<Comparator> DBEnv::simple_cursor(const DBTxn &txn, Comparator &&cmp, Slice &key, Slice &val) const {
        return SimpleCursor<Comparator>(*this, txn, std::forward<Comparator>(cmp), key, val);
    }

} // namespace ftcxx
