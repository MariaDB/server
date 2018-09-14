/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
/*======
This file is part of TokuDB


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    TokuDB is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2,
    as published by the Free Software Foundation.

    TokuDB is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TokuDB.  If not, see <http://www.gnu.org/licenses/>.

======= */

#ident "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

#include "tokudb_sysvars.h"
#include "toku_time.h"

namespace tokudb {
namespace analyze {

class recount_rows_t : public tokudb::background::job_manager_t::job_t {
public:
    void* operator new(size_t sz);
    void operator delete(void* p);

    recount_rows_t(
        bool user_schedued,
        THD* thd,
        TOKUDB_SHARE* share,
        DB_TXN* txn);

    virtual ~recount_rows_t();

    virtual const char* key();
    virtual const char* database();
    virtual const char* table();
    virtual const char* type();
    virtual const char* parameters();
    virtual const char* status();

protected:
    virtual void on_run();

    virtual void on_destroy();

private:
    // to be provided by the initiator of recount rows
    THD*            _thd;
    TOKUDB_SHARE*   _share;
    DB_TXN*         _txn;
    ulonglong       _throttle;

    // for recount rows status reporting
    char            _parameters[256];
    char            _status[1024];
    int             _result;
    ulonglong       _recount_start; // in microseconds
    ulonglong       _total_elapsed_time; // in microseconds

    bool            _local_txn;
    ulonglong       _rows;
    ulonglong       _deleted_rows;
    ulonglong       _ticks;

    static int analyze_recount_rows_progress(
        uint64_t count,
        uint64_t deleted,
        void* extra);
    int analyze_recount_rows_progress(uint64_t count, uint64_t deleted);
};

void* recount_rows_t::operator new(size_t sz) {
    return tokudb::memory::malloc(sz, MYF(MY_WME|MY_ZEROFILL|MY_FAE));
}
void recount_rows_t::operator delete(void* p) {
    tokudb::memory::free(p);
}
recount_rows_t::recount_rows_t(
    bool user_scheduled,
    THD* thd,
    TOKUDB_SHARE* share,
    DB_TXN* txn) :
    tokudb::background::job_manager_t::job_t(user_scheduled),
    _share(share),
    _result(HA_ADMIN_OK),
    _recount_start(0),
    _total_elapsed_time(0),
    _local_txn(false),
    _rows(0),
    _deleted_rows(0),
    _ticks(0) {

    assert_debug(thd != NULL);
    assert_debug(share != NULL);

    if (tokudb::sysvars::analyze_in_background(thd)) {
        _thd = NULL;
        _txn = NULL;
    } else {
        _thd = thd;
        _txn = txn;
    }

    _throttle = tokudb::sysvars::analyze_throttle(thd);

    snprintf(_parameters,
             sizeof(_parameters),
             "TOKUDB_ANALYZE_THROTTLE=%llu;",
             _throttle);
    _status[0] = '\0';
}
recount_rows_t::~recount_rows_t() {
}
void recount_rows_t::on_run() {
    const char* orig_proc_info = NULL;
    if (_thd)
        orig_proc_info = tokudb_thd_get_proc_info(_thd);
    _recount_start = tokudb::time::microsec();
    _total_elapsed_time = 0;

    if (_txn == NULL) {
        _result = db_env->txn_begin(db_env, NULL, &_txn, DB_READ_UNCOMMITTED);

        if (_result != 0) {
            _txn = NULL;
            _result = HA_ADMIN_FAILED;
            goto error;
        }
        _local_txn = true;
    } else {
        _local_txn = false;
    }

    _result =
        _share->file->recount_rows(
            _share->file,
            analyze_recount_rows_progress,
            this);

    if (_result != 0) {
        if (_local_txn) {
            _txn->abort(_txn);
            _txn = NULL;
        }
        _result = HA_ADMIN_FAILED;
        goto error;
    }

    DB_BTREE_STAT64 dict_stats;
    _result = _share->file->stat64(_share->file, _txn, &dict_stats);
    if (_result == 0) {
        _share->set_row_count(dict_stats.bt_ndata, false);
    }
    if (_result != 0)
        _result = HA_ADMIN_FAILED;

    if (_local_txn) {
        if (_result == HA_ADMIN_OK) {
            _txn->commit(_txn, 0);
        } else {
            _txn->abort(_txn);
        }
        _txn = NULL;
    }

    sql_print_information(
        "tokudb analyze recount rows %d counted %lld",
        _result,
        _share->row_count());
error:
    if(_thd)
        tokudb_thd_set_proc_info(_thd, orig_proc_info);
    return;
}
void recount_rows_t::on_destroy() {
    _share->release();
}
const char* recount_rows_t::key() {
    return _share->full_table_name();
}
const char* recount_rows_t::database() {
    return _share->database_name();
}
const char* recount_rows_t::table() {
    return _share->table_name();
}
const char* recount_rows_t::type() {
    static const char* type = "TOKUDB_ANALYZE_MODE_RECOUNT_ROWS";
    return type;
}
const char* recount_rows_t::parameters() {
    return _parameters;
}
const char* recount_rows_t::status() {
    return _status;
}
int recount_rows_t::analyze_recount_rows_progress(
    uint64_t count,
    uint64_t deleted,
    void* extra) {

    recount_rows_t* context = (recount_rows_t*)extra;
    return context->analyze_recount_rows_progress(count, deleted);
}
int recount_rows_t::analyze_recount_rows_progress(
    uint64_t count,
    uint64_t deleted) {

    _rows = count;
    _deleted_rows += deleted;
    deleted > 0 ? _ticks += deleted : _ticks++;

    if (_ticks > 1000) {
        _ticks = 0;
        uint64_t now = tokudb::time::microsec();
        _total_elapsed_time = now - _recount_start;
        if ((_thd && thd_kill_level(_thd)) || cancelled()) {
            // client killed
            return ER_ABORTING_CONNECTION;
        }

        // rebuild status
        // There is a slight race condition here,
        // _status is used here for tokudb_thd_set_proc_info and it is also used
        // for the status column in i_s.background_job_status.
        // If someone happens to be querying/building the i_s table
        // at the exact same time that the status is being rebuilt here,
        // the i_s table could get some garbage status.
        // This solution is a little heavy handed but it works, it prevents us
        // from changing the status while someone might be immediately observing
        // us and it prevents someone from observing us while we change the
        // status
        tokudb::background::_job_manager->lock();
        snprintf(_status,
                 sizeof(_status),
                 "recount_rows %s.%s counted %llu rows and %llu deleted "
                 "in %llu seconds.",
                 _share->database_name(),
                 _share->table_name(),
                 _rows,
                 _deleted_rows,
                 _total_elapsed_time / tokudb::time::MICROSECONDS);
        tokudb::background::_job_manager->unlock();

        // report
        if (_thd)
            tokudb_thd_set_proc_info(_thd, _status);

        // throttle
        // given the throttle value, lets calculate the maximum number of rows
        // we should have seen so far in a .1 sec resolution
        if (_throttle > 0) {
            uint64_t estimated_rows = _total_elapsed_time / 100000;
            estimated_rows = estimated_rows * (_throttle / 10);
            if (_rows + _deleted_rows > estimated_rows) {
                // sleep for 1/10 of a second
                tokudb::time::sleep_microsec(100000);
            }
        }
    }
    return 0;
}

class standard_t : public tokudb::background::job_manager_t::job_t {
public:
    void* operator new(size_t sz);
    void operator delete(void* p);

    standard_t(bool user_scheduled, THD* thd, TOKUDB_SHARE* share, DB_TXN* txn);

    virtual ~standard_t();

    virtual const char* key(void);
    virtual const char* database();
    virtual const char* table();
    virtual const char* type();
    virtual const char* parameters();
    virtual const char* status();

protected:
    virtual void on_run();

    virtual void on_destroy();

private:
    // to be provided by initiator of analyze
    THD*            _thd;
    TOKUDB_SHARE*   _share;
    DB_TXN*         _txn;
    ulonglong       _throttle;      // in microseconds
    ulonglong       _time_limit;    // in microseconds
    double          _delete_fraction;

    // for analyze status reporting, may also use other state
    char            _parameters[256];
    char            _status[1024];
    int             _result;
    ulonglong       _analyze_start; // in microseconds
    ulonglong       _total_elapsed_time; // in microseconds

    // for analyze internal use, pretty much these are per-key/index
    ulonglong       _current_key;
    bool            _local_txn;
    ulonglong       _half_time;
    ulonglong       _half_rows;
    ulonglong       _rows;
    ulonglong       _deleted_rows;
    ulonglong       _ticks;
    ulonglong       _analyze_key_start; // in microseconds
    ulonglong       _key_elapsed_time; // in microseconds
    uint            _scan_direction;

    static bool analyze_standard_cursor_callback(
        void* extra,
        uint64_t deleted_rows);
    bool analyze_standard_cursor_callback(uint64_t deleted_rows);

    int analyze_key_progress();
    int analyze_key(uint64_t* rec_per_key_part);
};

void* standard_t::operator new(size_t sz) {
    return tokudb::memory::malloc(sz, MYF(MY_WME|MY_ZEROFILL|MY_FAE));
}
void standard_t::operator delete(void* p) {
    tokudb::memory::free(p);
}
standard_t::standard_t(
    bool user_scheduled,
    THD* thd,
    TOKUDB_SHARE* share,
    DB_TXN* txn) :
    tokudb::background::job_manager_t::job_t(user_scheduled),
    _share(share),
    _result(HA_ADMIN_OK),
    _analyze_start(0),
    _total_elapsed_time(0),
    _current_key(0),
    _local_txn(false),
    _half_time(0),
    _half_rows(0),
    _rows(0),
    _deleted_rows(0),
    _ticks(0),
    _analyze_key_start(0),
    _key_elapsed_time(0),
    _scan_direction(0) {

    assert_debug(thd != NULL);
    assert_debug(share != NULL);

    if (tokudb::sysvars::analyze_in_background(thd)) {
       _thd = NULL;
       _txn = NULL;
    } else {
       _thd = thd;
       _txn = txn;
    }
    _throttle = tokudb::sysvars::analyze_throttle(thd);
    _time_limit =
        tokudb::sysvars::analyze_time(thd) * tokudb::time::MICROSECONDS;
    _delete_fraction = tokudb::sysvars::analyze_delete_fraction(thd);

    snprintf(_parameters,
             sizeof(_parameters),
             "TOKUDB_ANALYZE_DELETE_FRACTION=%f; "
             "TOKUDB_ANALYZE_TIME=%llu; TOKUDB_ANALYZE_THROTTLE=%llu;",
             _delete_fraction,
             _time_limit / tokudb::time::MICROSECONDS,
             _throttle);

    _status[0] = '\0';
}
standard_t::~standard_t() {
}
void standard_t::on_run() {
    DB_BTREE_STAT64 stat64;
    uint64_t rec_per_key_part[_share->_max_key_parts];
    uint64_t total_key_parts = 0;
    const char* orig_proc_info = NULL;
    if (_thd)
        orig_proc_info = tokudb_thd_get_proc_info(_thd);

    _analyze_start = tokudb::time::microsec();
    _half_time = _time_limit > 0 ? _time_limit/2 : 0;

    if (_txn == NULL) {
        _result = db_env->txn_begin(db_env, NULL, &_txn, DB_READ_UNCOMMITTED);

        if (_result != 0) {
            _txn = NULL;
            _result = HA_ADMIN_FAILED;
            goto error;
        }
        _local_txn = true;
    } else {
        _local_txn = false;
    }

    assert_always(_share->key_file[0] != NULL);
    _result = _share->key_file[0]->stat64(_share->key_file[0], _txn, &stat64);
    if (_result != 0) {
        _result = HA_ADMIN_FAILED;
        goto cleanup;
    }
    _half_rows = stat64.bt_ndata / 2;

    for (ulonglong current_key = 0;
         _result == HA_ADMIN_OK && current_key < _share->_keys;
         current_key++) {

        _current_key = current_key;
        _rows = _deleted_rows = _ticks = 0;
        _result = analyze_key(&rec_per_key_part[total_key_parts]);

        if ((_result != 0 && _result != ETIME) ||
            (_result != 0 && _rows == 0 && _deleted_rows > 0)) {
            _result = HA_ADMIN_FAILED;
        }
        if (_thd && (_result == HA_ADMIN_FAILED ||
            static_cast<double>(_deleted_rows) >
                _delete_fraction * (_rows + _deleted_rows))) {

            char name[256]; int namelen;
            namelen =
                snprintf(
                    name,
                    sizeof(name),
                    "%s.%s.%s",
                    _share->database_name(),
                    _share->table_name(),
                    _share->_key_descriptors[_current_key]._name);
            _thd->protocol->prepare_for_resend();
            _thd->protocol->store(name, namelen,  system_charset_info);
            _thd->protocol->store("analyze", 7, system_charset_info);
            _thd->protocol->store("info", 4, system_charset_info);
            char rowmsg[256];
            int rowmsglen;
            rowmsglen =
                snprintf(
                    rowmsg,
                    sizeof(rowmsg),
                    "rows processed %llu rows deleted %llu",
                    _rows,
                    _deleted_rows);
            _thd->protocol->store(rowmsg, rowmsglen, system_charset_info);
            _thd->protocol->write();

            sql_print_information(
                "tokudb analyze on %.*s %.*s",
                namelen,
                name,
                rowmsglen,
                rowmsg);
        }

        total_key_parts += _share->_key_descriptors[_current_key]._parts;
    }
    if (_result == HA_ADMIN_OK) {
        int error =
            tokudb::set_card_in_status(
                _share->status_block,
                _txn,
                total_key_parts,
                rec_per_key_part);
        if (error)
            _result = HA_ADMIN_FAILED;

        _share->lock();
        _share->update_cardinality_counts(total_key_parts, rec_per_key_part);
        _share->allow_auto_analysis(true);
        _share->unlock();
    }

cleanup:
    if (_local_txn) {
        if (_result == HA_ADMIN_OK) {
            _txn->commit(_txn, 0);
        } else {
            _txn->abort(_txn);
        }
        _txn = NULL;
    }

error:
    if (_thd)
        tokudb_thd_set_proc_info(_thd, orig_proc_info);
    return;
}
void standard_t::on_destroy() {
    _share->lock();
    _share->allow_auto_analysis(false);
    _share->unlock();
    _share->release();
}
const char* standard_t::key() {
    return _share->full_table_name();
}
const char* standard_t::database() {
    return _share->database_name();
}
const char* standard_t::table() {
    return _share->table_name();
}
const char* standard_t::type() {
    static const char* type = "TOKUDB_ANALYZE_MODE_STANDARD";
    return type;
}
const char* standard_t::parameters() {
    return _parameters;
}
const char* standard_t::status() {
    return _status;
}
bool standard_t::analyze_standard_cursor_callback(
    void* extra,
    uint64_t deleted_rows) {
    standard_t* context = (standard_t*)extra;
    return context->analyze_standard_cursor_callback(deleted_rows);
}
bool standard_t::analyze_standard_cursor_callback(uint64_t deleted_rows) {
    _deleted_rows += deleted_rows;
    _ticks += deleted_rows;
    return analyze_key_progress() != 0;
}
int standard_t::analyze_key_progress(void) {
    if (_ticks > 1000) {
        _ticks = 0;
        uint64_t now = tokudb::time::microsec();
        _total_elapsed_time = now - _analyze_start;
        _key_elapsed_time = now - _analyze_key_start;
        if ((_thd && thd_kill_level(_thd)) || cancelled()) {
            // client killed
            return ER_ABORTING_CONNECTION;
        } else if (_time_limit > 0 &&
                   static_cast<uint64_t>(_key_elapsed_time) > _time_limit) {
            // time limit reached
            return ETIME;
        }

        // rebuild status
        // There is a slight race condition here,
        // _status is used here for tokudb_thd_set_proc_info and it is also used
        // for the status column in i_s.background_job_status.
        // If someone happens to be querying/building the i_s table
        // at the exact same time that the status is being rebuilt here,
        // the i_s table could get some garbage status.
        // This solution is a little heavy handed but it works, it prevents us
        // from changing the status while someone might be immediately observing
        // us and it prevents someone from observing us while we change the
        // status.
        static const char* scan_direction_str[] = {"not scanning",
                                                   "scanning forward",
                                                   "scanning backward",
                                                   "scan unknown"};

        const char* scan_direction = NULL;
        switch (_scan_direction) {
            case 0:
                scan_direction = scan_direction_str[0];
                break;
            case DB_NEXT:
                scan_direction = scan_direction_str[1];
                break;
            case DB_PREV:
                scan_direction = scan_direction_str[2];
                break;
            default:
                scan_direction = scan_direction_str[3];
                break;
        }

        float progress_rows = 0.0;
        if (_share->row_count() > 0)
            progress_rows = static_cast<float>(_rows) /
                            static_cast<float>(_share->row_count());
        float progress_time = 0.0;
        if (_time_limit > 0)
            progress_time = static_cast<float>(_key_elapsed_time) /
                            static_cast<float>(_time_limit);
        tokudb::background::_job_manager->lock();
        snprintf(
            _status,
            sizeof(_status),
            "analyze table standard %s.%s.%s %llu of %u %.lf%% rows %.lf%% "
            "time, %s",
            _share->database_name(),
            _share->table_name(),
            _share->_key_descriptors[_current_key]._name,
            _current_key,
            _share->_keys,
            progress_rows * 100.0,
            progress_time * 100.0,
            scan_direction);
        tokudb::background::_job_manager->unlock();

        // report
        if (_thd)
            tokudb_thd_set_proc_info(_thd, _status);

        // throttle
        // given the throttle value, lets calculate the maximum number of rows
        // we should have seen so far in a .1 sec resolution
        if (_throttle > 0) {
            uint64_t estimated_rows = _key_elapsed_time / 100000;
            estimated_rows = estimated_rows * (_throttle / 10);
            if (_rows + _deleted_rows > estimated_rows) {
                // sleep for 1/10 of a second
                tokudb::time::sleep_microsec(100000);
            }
        }
    }
    return 0;
}
int standard_t::analyze_key(uint64_t* rec_per_key_part) {
    int error = 0;
    DB* db = _share->key_file[_current_key];
    assert_always(db != NULL);
    uint64_t num_key_parts = _share->_key_descriptors[_current_key]._parts;
    uint64_t unique_rows[num_key_parts];
    bool is_unique = _share->_key_descriptors[_current_key]._is_unique;
    DBC* cursor = NULL;
    int close_error = 0;
    DBT key, prev_key;
    bool copy_key = false;

    _analyze_key_start = tokudb::time::microsec();
    _key_elapsed_time = 0;
    _scan_direction = DB_NEXT;

    if (is_unique && num_key_parts == 1) {
        // don't compute for unique keys with a single part. we already know
        // the answer.
        _rows = unique_rows[0] = 1;
        goto done;
    }

    for (uint64_t i = 0; i < num_key_parts; i++)
        unique_rows[i] = 1;

    // stop looking when the entire dictionary was analyzed, or a
    // cap on execution time was reached, or the analyze was killed.
    while (1) {
        if (cursor == NULL) {
            error = db->cursor(db, _txn, &cursor, 0);
            if (error != 0)
                goto done;

            cursor->c_set_check_interrupt_callback(
                cursor,
                analyze_standard_cursor_callback,
                this);

            memset(&key, 0, sizeof(DBT));
            memset(&prev_key, 0, sizeof(DBT));
            copy_key = true;
        }

        error = cursor->c_get(cursor, &key, 0, _scan_direction);
        if (error != 0) {
            if (error == DB_NOTFOUND || error == TOKUDB_INTERRUPTED)
                error = 0; // not an error
            break;
        } else if (cancelled()) {
            error = ER_ABORTING_CONNECTION;
            break;
        }

        _rows++;
        _ticks++;

        // if copy_key is false at this pont, we have some value sitting in
        // prev_key that we can compare to
        // if the comparison reveals a unique key, we must set copy_key to true
        // so the code following can copy he current key into prev_key for the
        // next iteration
        if (copy_key == false) {
            // compare this key with the previous key. ignore
            // appended PK for SK's.
            // TODO if a prefix is different, then all larger keys
            // that include the prefix are also different.
            // TODO if we are comparing the entire primary key or
            // the entire unique secondary key, then the cardinality
            // must be 1, so we can avoid computing it.
            for (uint64_t i = 0; i < num_key_parts; i++) {
                int cmp = tokudb_cmp_dbt_key_parts(db, &prev_key, &key, i+1);
                if (cmp != 0) {
                    unique_rows[i]++;
                    copy_key = true;
                }
            }
        }

        // prev_key = key or prev_key is NULL
        if (copy_key) {
            prev_key.data =
                tokudb::memory::realloc(
                    prev_key.data,
                    key.size,
                    MYF(MY_WME|MY_ZEROFILL|MY_FAE));
            assert_always(prev_key.data);
            prev_key.size = key.size;
            memcpy(prev_key.data, key.data, prev_key.size);
            copy_key = false;
        }

        error = analyze_key_progress();
        if (error == ETIME) {
            error = 0;
            break;
        } else if (error) {
            break;
        }

        // if we have a time limit, are scanning forward and have exceed the
        // _half_time and not passed the _half_rows number of the rows in the
        // index: clean up the keys, close the cursor and reverse direction.
        if (TOKUDB_UNLIKELY(_half_time > 0 &&
            _scan_direction == DB_NEXT &&
            _key_elapsed_time >= _half_time &&
            _rows < _half_rows)) {

            tokudb::memory::free(prev_key.data); prev_key.data = NULL;
            close_error = cursor->c_close(cursor);
            assert_always(close_error == 0);
            cursor = NULL;
            _scan_direction = DB_PREV;
        }
    }
    // cleanup
    if (prev_key.data) tokudb::memory::free(prev_key.data);
    if (cursor) close_error = cursor->c_close(cursor);
    assert_always(close_error == 0);

done:
    // in case we timed out (bunch of deleted records) without hitting a
    // single row
    if (_rows == 0)
        _rows = 1;

    // return cardinality
    for (uint64_t i = 0; i < num_key_parts; i++) {
        rec_per_key_part[i] = _rows / unique_rows[i];
    }
    return error;
}

} // namespace analyze
} // namespace tokudb


int ha_tokudb::analyze(THD *thd, TOKUDB_UNUSED(HA_CHECK_OPT *check_opt)) {
    TOKUDB_HANDLER_DBUG_ENTER("%s", share->table_name());
    int result = HA_ADMIN_OK;
    tokudb::sysvars::analyze_mode_t mode = tokudb::sysvars::analyze_mode(thd);

    switch (mode) {
    case tokudb::sysvars::TOKUDB_ANALYZE_RECOUNT_ROWS:
        result = share->analyze_recount_rows(thd, transaction);
        break;
    case tokudb::sysvars::TOKUDB_ANALYZE_STANDARD:
        share->lock();
        result = share->analyze_standard(thd, transaction);
        share->unlock();
        break;
    case tokudb::sysvars::TOKUDB_ANALYZE_CANCEL:
        share->cancel_background_jobs();
        break;
    default:
        break; // no-op
    }
    TOKUDB_HANDLER_DBUG_RETURN(result);
}

int TOKUDB_SHARE::analyze_recount_rows(THD* thd,DB_TXN* txn) {
    TOKUDB_HANDLER_DBUG_ENTER("%s", table_name());

    assert_always(thd != NULL);

    int result = HA_ADMIN_OK;

    tokudb::analyze::recount_rows_t* job
        = new tokudb::analyze::recount_rows_t(true, thd, this, txn);
    assert_always(job != NULL);

    // job->destroy will drop the ref
    addref();
    unlock();

    bool ret = tokudb::background::_job_manager->
        run_job(job, tokudb::sysvars::analyze_in_background(thd));

    if (!ret) {
        job->destroy();
        delete job;
        result = HA_ADMIN_FAILED;
    }

    TOKUDB_HANDLER_DBUG_RETURN(result);
}

// on entry, if txn is !NULL, it is a user session invoking ANALYZE directly
// and no lock will be held on 'this', else if txn is NULL it is an auto and
// 'this' will be locked.
int TOKUDB_SHARE::analyze_standard(THD* thd, DB_TXN* txn) {
    TOKUDB_HANDLER_DBUG_ENTER("%s", table_name());

    assert_always(thd != NULL);
    assert_debug(_mutex.is_owned_by_me() == true);

    int result = HA_ADMIN_OK;

    // stub out analyze if optimize is remapped to alter recreate + analyze
    // when not auto analyze or if this is an alter
    if ((txn &&
         thd_sql_command(thd) != SQLCOM_ANALYZE &&
         thd_sql_command(thd) != SQLCOM_ALTER_TABLE) ||
        thd_sql_command(thd) == SQLCOM_ALTER_TABLE) {
        TOKUDB_HANDLER_DBUG_RETURN(result);
    }

    tokudb::analyze::standard_t* job
        = new tokudb::analyze::standard_t(txn == NULL ? false : true, thd,
                                          this, txn);
    assert_always(job != NULL);

    // akin to calling addref, but we know, right here, right now, everything
    // in the share is set up, files open, etc...
    // job->destroy will drop the ref
    _use_count++;

    // don't want any autos kicking off while we are analyzing
    disallow_auto_analysis();

    unlock();

    bool ret =
        tokudb::background::_job_manager->run_job(
            job,
            tokudb::sysvars::analyze_in_background(thd));

    if (!ret) {
        job->destroy();
        delete job;
        result = HA_ADMIN_FAILED;
    }

    lock();

    TOKUDB_HANDLER_DBUG_RETURN(result);
}


typedef struct hot_optimize_context {
    THD* thd;
    char* write_status_msg;
    ha_tokudb* ha;
    uint progress_stage;
    uint current_table;
    uint num_tables;
    float progress_limit;
    uint64_t progress_last_time;
    uint64_t throttle;
} *HOT_OPTIMIZE_CONTEXT;

static int hot_optimize_progress_fun(void *extra, float progress) {
    HOT_OPTIMIZE_CONTEXT context = (HOT_OPTIMIZE_CONTEXT)extra;
    if (thd_kill_level(context->thd)) {
        sprintf(
            context->write_status_msg,
            "The process has been killed, aborting hot optimize.");
        return ER_ABORTING_CONNECTION;
    }
    float percentage = progress * 100;
    sprintf(
        context->write_status_msg,
        "Optimization of index %u of %u about %.lf%% done",
        context->current_table + 1,
        context->num_tables,
        percentage);
    thd_proc_info(context->thd, context->write_status_msg);
#ifdef HA_TOKUDB_HAS_THD_PROGRESS
    if (context->progress_stage < context->current_table) {
        // the progress stage is behind the current table, so move up
        // to the next stage and set the progress stage to current.
        thd_progress_next_stage(context->thd);
        context->progress_stage = context->current_table;
    }
    // the percentage we report here is for the current stage/db
    thd_progress_report(context->thd, (unsigned long long) percentage, 100);
#endif

    // throttle the optimize table
    if (context->throttle) {
        uint64_t time_now = toku_current_time_microsec();
        uint64_t dt = time_now - context->progress_last_time;
        uint64_t throttle_time = 1000000ULL / context->throttle;
        if (throttle_time > dt) {
            usleep(throttle_time - dt);
        }
        context->progress_last_time = toku_current_time_microsec();
    }

    // return 1 if progress has reach the progress limit
    return progress >= context->progress_limit;
}

// flatten all DB's in this table, to do so, peform hot optimize on each db
int ha_tokudb::do_optimize(THD* thd) {
    TOKUDB_HANDLER_DBUG_ENTER("%s", share->table_name());
    int error = 0;
    const char* orig_proc_info = tokudb_thd_get_proc_info(thd);
    uint curr_num_DBs = table->s->keys + tokudb_test(hidden_primary_key);

#ifdef HA_TOKUDB_HAS_THD_PROGRESS
    // each DB is its own stage. as HOT goes through each db, we'll
    // move on to the next stage.
    thd_progress_init(thd, curr_num_DBs);
#endif

    // for each DB, run optimize and hot_optimize
    for (uint i = 0; i < curr_num_DBs; i++) {
        // only optimize the index if it matches the optimize_index_name
        // session variable
        const char* optimize_index_name =
            tokudb::sysvars::optimize_index_name(thd);
        if (optimize_index_name) {
            const char* this_index_name =
                i >= table_share->keys ?
                    "primary" :
                    table_share->key_info[i].name;
            if (strcasecmp(optimize_index_name, this_index_name) != 0) {
                continue;
            }
        }

        DB* db = share->key_file[i];
        assert_always(db != NULL);
        error = db->optimize(db);
        if (error) {
            goto cleanup;
        }

        struct hot_optimize_context hc;
        memset(&hc, 0, sizeof hc);
        hc.thd = thd;
        hc.write_status_msg = this->write_status_msg;
        hc.ha = this;
        hc.current_table = i;
        hc.num_tables = curr_num_DBs;
        hc.progress_limit = tokudb::sysvars::optimize_index_fraction(thd);
        hc.progress_last_time = toku_current_time_microsec();
        hc.throttle = tokudb::sysvars::optimize_throttle(thd);
        uint64_t loops_run;
        error =
            db->hot_optimize(
                db,
                NULL,
                NULL,
                hot_optimize_progress_fun,
                &hc,
                &loops_run);
        if (error) {
            goto cleanup;
        }
    }
    error = 0;

cleanup:
#ifdef HA_TOKUDB_HAS_THD_PROGRESS
    thd_progress_end(thd);
#endif
    thd_proc_info(thd, orig_proc_info);
    TOKUDB_HANDLER_DBUG_RETURN(error);
}

int ha_tokudb::optimize(TOKUDB_UNUSED(THD* thd),
                        TOKUDB_UNUSED(HA_CHECK_OPT* check_opt)) {
    TOKUDB_HANDLER_DBUG_ENTER("%s", share->table_name());
    int error;
#if TOKU_OPTIMIZE_WITH_RECREATE
    error = HA_ADMIN_TRY_ALTER;
#else
    error = do_optimize(thd);
#endif
    TOKUDB_HANDLER_DBUG_RETURN(error);
}

struct check_context {
    THD* thd;
};

static int ha_tokudb_check_progress(void* extra,
                                    TOKUDB_UNUSED(float progress)) {
    struct check_context* context = (struct check_context*)extra;
    int result = 0;
    if (thd_kill_level(context->thd))
        result = ER_ABORTING_CONNECTION;
    return result;
}

static void ha_tokudb_check_info(THD* thd, TABLE* table, const char* msg) {
    if (thd->vio_ok()) {
        char tablename[
            table->s->db.length + 1 +
            table->s->table_name.length + 1];
        snprintf(
            tablename,
            sizeof(tablename),
            "%.*s.%.*s",
            (int)table->s->db.length,
            table->s->db.str,
            (int)table->s->table_name.length,
            table->s->table_name.str);
        thd->protocol->prepare_for_resend();
        thd->protocol->store(tablename, strlen(tablename), system_charset_info);
        thd->protocol->store("check", 5, system_charset_info);
        thd->protocol->store("info", 4, system_charset_info);
        thd->protocol->store(msg, strlen(msg), system_charset_info);
        thd->protocol->write();
    }
}

int ha_tokudb::check(THD* thd, HA_CHECK_OPT* check_opt) {
    TOKUDB_HANDLER_DBUG_ENTER("%s", share->table_name());
    const char* orig_proc_info = tokudb_thd_get_proc_info(thd);
    int result = HA_ADMIN_OK;
    int r;

    int keep_going = 1;
    if (check_opt->flags & T_QUICK) {
        keep_going = 0;
    }
    if (check_opt->flags & T_EXTEND) {
        keep_going = 1;
    }

    r = acquire_table_lock(transaction, lock_write);
    if (r != 0)
        result = HA_ADMIN_INTERNAL_ERROR;
    if (result == HA_ADMIN_OK) {
        uint32_t num_DBs = table_share->keys + tokudb_test(hidden_primary_key);
        snprintf(
            write_status_msg,
            sizeof(write_status_msg),
            "%s primary=%d num=%d",
            share->table_name(),
            primary_key,
            num_DBs);
        if (TOKUDB_UNLIKELY(TOKUDB_DEBUG_FLAGS(TOKUDB_DEBUG_CHECK))) {
            ha_tokudb_check_info(thd, table, write_status_msg);
            time_t now = time(0);
            char timebuf[32];
            TOKUDB_HANDLER_TRACE(
                "%.24s %s",
                ctime_r(&now, timebuf),
                write_status_msg);
        }
        for (uint i = 0; i < num_DBs; i++) {
            DB* db = share->key_file[i];
            assert_always(db != NULL);
            const char* kname =
                i == primary_key ? "primary" : table_share->key_info[i].name;
            snprintf(
                write_status_msg,
                sizeof(write_status_msg),
                "%s key=%s %u",
                share->table_name(),
                kname,
                i);
            thd_proc_info(thd, write_status_msg);
            if (TOKUDB_UNLIKELY(TOKUDB_DEBUG_FLAGS(TOKUDB_DEBUG_CHECK))) {
                ha_tokudb_check_info(thd, table, write_status_msg);
                time_t now = time(0);
                char timebuf[32];
                TOKUDB_HANDLER_TRACE(
                    "%.24s %s",
                    ctime_r(&now, timebuf),
                    write_status_msg);
            }
            struct check_context check_context = { thd };
            r = db->verify_with_progress(
                db,
                ha_tokudb_check_progress,
                &check_context,
                (tokudb::sysvars::debug & TOKUDB_DEBUG_CHECK) != 0,
                keep_going);
            if (r != 0) {
                char msg[32 + strlen(kname)];
                sprintf(msg, "Corrupt %s", kname);
                ha_tokudb_check_info(thd, table, msg);
            }
            snprintf(
                write_status_msg,
                sizeof(write_status_msg),
                "%s key=%s %u result=%d",
                share->full_table_name(),
                kname,
                i,
                r);
            thd_proc_info(thd, write_status_msg);
            if (TOKUDB_UNLIKELY(TOKUDB_DEBUG_FLAGS(TOKUDB_DEBUG_CHECK))) {
                ha_tokudb_check_info(thd, table, write_status_msg);
                time_t now = time(0);
                char timebuf[32];
                TOKUDB_HANDLER_TRACE(
                    "%.24s %s",
                    ctime_r(&now, timebuf),
                    write_status_msg);
            }
            if (result == HA_ADMIN_OK && r != 0) {
                result = HA_ADMIN_CORRUPT;
                if (!keep_going)
                    break;
            }
        }
    }
    thd_proc_info(thd, orig_proc_info);
    TOKUDB_HANDLER_DBUG_RETURN(result);
}
