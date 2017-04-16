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

#include <errno.h>

#include <map>
#include <string>

#include <db.h>

#include "exceptions.hpp"
#include "slice.hpp"

namespace ftcxx {

    template<class Comparator, class Handler>
    class CallbackCursor;
    template<class Comparator, class Predicate>
    class BufferedCursor;
    template<class Comparator>
    class SimpleCursor;

    class DBTxn;

    class DBEnv {
    public:
        explicit DBEnv(DB_ENV *e, bool close_on_destroy=false)
            : _env(e),
              _close_on_destroy(close_on_destroy)
        {}

        ~DBEnv() {
            if (_env && _close_on_destroy) {
                close();
            }
        }

        DBEnv(const DBEnv &) = delete;
        DBEnv& operator=(const DBEnv &) = delete;

        DBEnv(DBEnv &&o)
            : _env(nullptr),
              _close_on_destroy(false)
        {
            std::swap(_env, o._env);
            std::swap(_close_on_destroy, o._close_on_destroy);
        }

        DBEnv& operator=(DBEnv &&o) {
            std::swap(_env, o._env);
            std::swap(_close_on_destroy, o._close_on_destroy);
            return *this;
        }

        DB_ENV *env() const { return _env; }

        void close() {
            int r = _env->close(_env, 0);
            handle_ft_retval(r);
            _env = nullptr;
        }

        typedef std::map<std::string, TOKU_ENGINE_STATUS_ROW_S> Status;
        void get_status(Status &status, fs_redzone_state &redzone_state, uint64_t &env_panic, std::string &panic_string) const;

        void log_flush() {
            int r = _env->log_flush(_env, NULL);
            handle_ft_retval(r);
        }

        int checkpointing_set_period(uint32_t period) {
            if (!_env) {
                return EINVAL;
            }
            _env->checkpointing_set_period(_env, period);
            return 0;
        }

        int cleaner_set_iterations(uint32_t iterations) {
            if (!_env) {
                return EINVAL;
            }
            _env->cleaner_set_iterations(_env, iterations);
            return 0;
        }

        int cleaner_set_period(uint32_t period) {
            if (!_env) {
                return EINVAL;
            }
            _env->cleaner_set_period(_env, period);
            return 0;
        }

        int change_fsync_log_period(uint32_t period) {
            if (!_env) {
                return EINVAL;
            }
            _env->change_fsync_log_period(_env, period);
            return 0;
        }

        uint64_t get_engine_status_num_rows() {
            if (!_env) {
                handle_ft_retval(EINVAL); // throws
            }
            uint64_t ret;
            int r = _env->get_engine_status_num_rows(_env, &ret);
            handle_ft_retval(r);
            return ret;
        }

        void get_engine_status(TOKU_ENGINE_STATUS_ROW_S *rows, uint64_t max_rows, uint64_t &num_rows,
                               uint64_t &panic, std::string &panic_string,
                               toku_engine_status_include_type include_type) {
            if (!_env) {
                handle_ft_retval(EINVAL);
            }
            fs_redzone_state dummy;  // this is duplicated in the actual engine status output
            const size_t panic_string_len = 1024;
            char panic_string_buf[panic_string_len];
            panic_string_buf[0] = '\0';
            int r = _env->get_engine_status(_env, rows, max_rows, &num_rows,
                                            &dummy, &panic, panic_string_buf, panic_string_len,
                                            include_type);
            handle_ft_retval(r);
            panic_string = panic_string_buf;
        }

        /**
         * Constructs a Cursor over this DBEnv's directory.
         */
        template<class Comparator, class Handler>
        CallbackCursor<Comparator, Handler> cursor(const DBTxn &txn, Comparator &&cmp, Handler &&handler) const;

        template<class Comparator, class Predicate>
        BufferedCursor<Comparator, Predicate> buffered_cursor(const DBTxn &txn, Comparator &&cmp, Predicate &&filter) const;

        template<class Comparator>
        SimpleCursor<Comparator> simple_cursor(const DBTxn &txn, Comparator &&cmp, Slice &key, Slice &val) const;

    private:
        DB_ENV *_env;
        bool _close_on_destroy;
    };

    class DBEnvBuilder {
        typedef int (*bt_compare_func)(DB *, const DBT *, const DBT *);
        bt_compare_func _bt_compare;

        typedef int (*update_func)(DB *, const DBT *, const DBT *, const DBT *, void (*)(const DBT *, void *), void *);
        update_func _update_function;

        generate_row_for_put_func _generate_row_for_put;
        generate_row_for_del_func _generate_row_for_del;

        uint32_t _cleaner_period;
        uint32_t _cleaner_iterations;
        uint32_t _checkpointing_period;
        uint32_t _fsync_log_period_msec;
        int _fs_redzone;

        uint64_t _lk_max_memory;
        uint64_t _lock_wait_time_msec;

        typedef uint64_t (*get_lock_wait_time_cb_func)(uint64_t);
        get_lock_wait_time_cb_func _get_lock_wait_time_cb;
        lock_timeout_callback _lock_timeout_callback;
        lock_wait_callback _lock_wait_needed_callback;
        uint64_t (*_loader_memory_size_callback)(void);

        uint32_t _cachesize_gbytes;
        uint32_t _cachesize_bytes;
        uint32_t _cachetable_bucket_mutexes;

        std::string _product_name;

        std::string _lg_dir;
        std::string _tmp_dir;

        bool _direct_io;
        bool _compress_buffers;

    public:
        DBEnvBuilder()
            : _bt_compare(nullptr),
              _update_function(nullptr),
              _generate_row_for_put(nullptr),
              _generate_row_for_del(nullptr),
              _cleaner_period(0),
              _cleaner_iterations(0),
              _checkpointing_period(0),
              _fsync_log_period_msec(0),
              _fs_redzone(0),
              _lk_max_memory(0),
              _lock_wait_time_msec(0),
              _get_lock_wait_time_cb(nullptr),
              _lock_timeout_callback(nullptr),
              _lock_wait_needed_callback(nullptr),
              _loader_memory_size_callback(nullptr),
              _cachesize_gbytes(0),
              _cachesize_bytes(0),
              _cachetable_bucket_mutexes(0),
              _product_name(""),
              _lg_dir(""),
              _tmp_dir(""),
              _direct_io(false),
              _compress_buffers(true)
        {}

        DBEnv open(const char *env_dir, uint32_t flags, int mode) const {
            db_env_set_direct_io(_direct_io);
            db_env_set_compress_buffers_before_eviction(_compress_buffers);
            if (_cachetable_bucket_mutexes) {
                db_env_set_num_bucket_mutexes(_cachetable_bucket_mutexes);
            }

            if (!_product_name.empty()) {
                db_env_set_toku_product_name(_product_name.c_str());
            }

            DB_ENV *env;
            int r = db_env_create(&env, 0);
            handle_ft_retval(r);

            if (_bt_compare) {
                r = env->set_default_bt_compare(env, _bt_compare);
                handle_ft_retval(r);
            }

            if (_update_function) {
                env->set_update(env, _update_function);
            }

            if (_generate_row_for_put) {
                r = env->set_generate_row_callback_for_put(env, _generate_row_for_put);
                handle_ft_retval(r);
            }

            if (_generate_row_for_del) {
                r = env->set_generate_row_callback_for_del(env, _generate_row_for_del);
                handle_ft_retval(r);
            }

            if (_lk_max_memory) {
                r = env->set_lk_max_memory(env, _lk_max_memory);
                handle_ft_retval(r);
            }

            if (_lock_wait_time_msec || _get_lock_wait_time_cb) {
                uint64_t wait_time = _lock_wait_time_msec;
                if (!wait_time) {
                    r = env->get_lock_timeout(env, &wait_time);
                    handle_ft_retval(r);
                }
                r = env->set_lock_timeout(env, wait_time, _get_lock_wait_time_cb);
                handle_ft_retval(r);
            }

            if (_lock_timeout_callback) {
                r = env->set_lock_timeout_callback(env, _lock_timeout_callback);
                handle_ft_retval(r);
            }

            if (_lock_wait_needed_callback) {
                r = env->set_lock_wait_callback(env, _lock_wait_needed_callback);
                handle_ft_retval(r);
            }

            if (_loader_memory_size_callback) {
                env->set_loader_memory_size(env, _loader_memory_size_callback);
            }

            if (_cachesize_gbytes || _cachesize_bytes) {
                r = env->set_cachesize(env, _cachesize_gbytes, _cachesize_bytes, 1);
                handle_ft_retval(r);
            }

            if (_fs_redzone) {
                env->set_redzone(env, _fs_redzone);
            }

            if (!_lg_dir.empty()) {
                r = env->set_lg_dir(env, _lg_dir.c_str());
                handle_ft_retval(r);
            }

            if (!_tmp_dir.empty()) {
                r = env->set_tmp_dir(env, _tmp_dir.c_str());
                handle_ft_retval(r);
            }

            r = env->open(env, env_dir, flags, mode);
            handle_ft_retval(r);

            if (_cleaner_period) {
                r = env->cleaner_set_period(env, _cleaner_period);
                handle_ft_retval(r);
            }

            if (_cleaner_iterations) {
                r = env->cleaner_set_iterations(env, _cleaner_iterations);
                handle_ft_retval(r);
            }

            if (_checkpointing_period) {
                r = env->checkpointing_set_period(env, _checkpointing_period);
                handle_ft_retval(r);
            }

            if (_fsync_log_period_msec) {
                env->change_fsync_log_period(env, _fsync_log_period_msec);
            }

            return DBEnv(env, true);
        }

        DBEnvBuilder& set_direct_io(bool direct_io) {
            _direct_io = direct_io;
            return *this;
        }

        DBEnvBuilder& set_compress_buffers_before_eviction(bool compress_buffers) {
            _compress_buffers = compress_buffers;
            return *this;
        }

        DBEnvBuilder& set_default_bt_compare(bt_compare_func bt_compare) {
            _bt_compare = bt_compare;
            return *this;
        }

        DBEnvBuilder& set_update(update_func update_function) {
            _update_function = update_function;
            return *this;
        }

        DBEnvBuilder& set_generate_row_callback_for_put(generate_row_for_put_func generate_row_for_put) {
            _generate_row_for_put = generate_row_for_put;
            return *this;
        }

        DBEnvBuilder& set_generate_row_callback_for_del(generate_row_for_del_func generate_row_for_del) {
            _generate_row_for_del = generate_row_for_del;
            return *this;
        }

        DBEnvBuilder& cleaner_set_period(uint32_t period) {
            _cleaner_period = period;
            return *this;
        }

        DBEnvBuilder& cleaner_set_iterations(uint32_t iterations) {
            _cleaner_iterations = iterations;
            return *this;
        }

        DBEnvBuilder& checkpointing_set_period(uint32_t period) {
            _checkpointing_period = period;
            return *this;
        }

        DBEnvBuilder& change_fsync_log_period(uint32_t period) {
            _fsync_log_period_msec = period;
            return *this;
        }

        DBEnvBuilder& set_fs_redzone(int fs_redzone) {
            _fs_redzone = fs_redzone;
            return *this;
        }

        DBEnvBuilder& set_lk_max_memory(uint64_t sz) {
            _lk_max_memory = sz;
            return *this;
        }

        DBEnvBuilder& set_lock_wait_time_msec(uint64_t lock_wait_time_msec) {
            _lock_wait_time_msec = lock_wait_time_msec;
            return *this;
        }

        DBEnvBuilder& set_lock_wait_time_cb(get_lock_wait_time_cb_func get_lock_wait_time_cb) {
            _get_lock_wait_time_cb = get_lock_wait_time_cb;
            return *this;
        }

        DBEnvBuilder& set_lock_timeout_callback(lock_timeout_callback callback) {
            _lock_timeout_callback = callback;
            return *this;
        }

        DBEnvBuilder& set_lock_wait_callback(lock_wait_callback callback) {
            _lock_wait_needed_callback = callback;
            return *this;
        }

        DBEnvBuilder& set_loader_memory_size(uint64_t (*callback)(void)) {
            _loader_memory_size_callback = callback;
            return *this;
        }

        DBEnvBuilder& set_cachesize(uint32_t gbytes, uint32_t bytes) {
            _cachesize_gbytes = gbytes;
            _cachesize_bytes = bytes;
            return *this;
        }

        DBEnvBuilder& set_cachetable_bucket_mutexes(uint32_t mutexes) {
            _cachetable_bucket_mutexes = mutexes;
            return *this;
        }

        DBEnvBuilder& set_product_name(const char *product_name) {
            _product_name = std::string(product_name);
            return *this;
        }

        DBEnvBuilder& set_lg_dir(const char *lg_dir) {
            _lg_dir = std::string(lg_dir);
            return *this;
        }

        DBEnvBuilder& set_tmp_dir(const char *tmp_dir) {
            _tmp_dir = std::string(tmp_dir);
            return *this;
        }
    };

} // namespace ftcxx
