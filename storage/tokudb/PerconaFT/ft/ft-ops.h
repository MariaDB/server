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

// This must be first to make the 64-bit file mode work right in Linux
#define _FILE_OFFSET_BITS 64

#include <db.h>

#include "ft/cachetable/cachetable.h"
#include "ft/comparator.h"
#include "ft/msg.h"
#include "util/dbt.h"

#define OS_PATH_SEPARATOR '/'

typedef struct ft_handle *FT_HANDLE;

int toku_open_ft_handle (const char *fname, int is_create, FT_HANDLE *, int nodesize, int basementnodesize, enum toku_compression_method compression_method, CACHETABLE, TOKUTXN, int(*)(DB *,const DBT*,const DBT*)) __attribute__ ((warn_unused_result));

// effect: changes the descriptor for the ft of the given handle.
// requires: 
// - cannot change descriptor for same ft in two threads in parallel. 
// - can only update cmp descriptor immidiately after opening the FIRST ft handle for this ft and before 
//   ANY operations. to update the cmp descriptor after any operations have already happened, all handles 
//   and transactions must close and reopen before the change, then you can update the cmp descriptor
void toku_ft_change_descriptor(FT_HANDLE t, const DBT* old_descriptor, const DBT* new_descriptor, bool do_log, TOKUTXN txn, bool update_cmp_descriptor);
uint32_t toku_serialize_descriptor_size(DESCRIPTOR desc);

void toku_ft_handle_create(FT_HANDLE *ft);
void toku_ft_set_flags(FT_HANDLE, unsigned int flags);
void toku_ft_get_flags(FT_HANDLE, unsigned int *flags);
void toku_ft_handle_set_nodesize(FT_HANDLE, unsigned int nodesize);
void toku_ft_handle_get_nodesize(FT_HANDLE, unsigned int *nodesize);
void toku_ft_get_maximum_advised_key_value_lengths(unsigned int *klimit, unsigned int *vlimit);
void toku_ft_handle_set_basementnodesize(FT_HANDLE, unsigned int basementnodesize);
void toku_ft_handle_get_basementnodesize(FT_HANDLE, unsigned int *basementnodesize);
void toku_ft_handle_set_compression_method(FT_HANDLE, enum toku_compression_method);
void toku_ft_handle_get_compression_method(FT_HANDLE, enum toku_compression_method *);
void toku_ft_handle_set_fanout(FT_HANDLE, unsigned int fanout);
void toku_ft_handle_get_fanout(FT_HANDLE, unsigned int *fanout);
int toku_ft_handle_set_memcmp_magic(FT_HANDLE, uint8_t magic);

void toku_ft_set_bt_compare(FT_HANDLE ft_handle, ft_compare_func cmp_func);
const toku::comparator &toku_ft_get_comparator(FT_HANDLE ft_handle);

typedef void (*on_redirect_callback)(FT_HANDLE ft_handle, void *extra);
void toku_ft_set_redirect_callback(FT_HANDLE ft_handle, on_redirect_callback cb, void *extra);

// How updates (update/insert/deletes) work:
// There are two flavers of upsertdels:  Singleton and broadcast.
// When a singleton upsertdel message arrives it contains a key and an extra DBT.
//
// At the YDB layer, the function looks like
//
// int (*update_function)(DB*, DB_TXN*, const DBT *key, const DBT *old_val, const DBT *extra,
//                        void (*set_val)(const DBT *new_val, void *set_extra), void *set_extra);
//
// And there are two DB functions
//
// int DB->update(DB *, DB_TXN *, const DBT *key, const DBT *extra);
// Effect:
//    If there is a key-value pair visible to the txn with value old_val then the system calls
//      update_function(DB, key, old_val, extra, set_val, set_extra)
//    where set_val and set_extra are a function and a void* provided by the system.
//    The update_function can do one of two things:
//      a) call set_val(new_val, set_extra)
//         which has the effect of doing DB->put(db, txn, key, new_val, 0)
//         overwriting the old value.
//      b) Return DB_DELETE (a new return code)
//      c) Return 0 (success) without calling set_val, which leaves the old value unchanged.
//    If there is no such key-value pair visible to the txn, then the system calls
//       update_function(DB, key, NULL, extra, set_val, set_extra)
//    and the update_function can do one of the same three things.
// Implementation notes: Update acquires a write lock (just as DB->put
//    does).   This function works by sending a UPDATE message containing
//    the key and extra.
//
// int DB->update_broadcast(DB *, DB_TXN*, const DBT *extra);
// Effect: This has the same effect as building a cursor that walks
//  through the DB, calling DB->update() on every key that the cursor
//  finds.
// Implementation note: Acquires a write lock on the entire database.
//  This function works by sending an BROADCAST-UPDATE message containing
//   the key and the extra.
typedef int (*ft_update_func)(DB *db, const DBT *key, const DBT *old_val, const DBT *extra,
                              void (*set_val)(const DBT *new_val, void *set_extra),
                              void *set_extra);
void toku_ft_set_update(FT_HANDLE ft_h, ft_update_func update_fun);

int toku_ft_handle_open(FT_HANDLE, const char *fname_in_env,
		  int is_create, int only_create, CACHETABLE ct, TOKUTXN txn)  __attribute__ ((warn_unused_result));
int toku_ft_handle_open_recovery(FT_HANDLE, const char *fname_in_env, int is_create, int only_create, CACHETABLE ct, TOKUTXN txn, 
			   FILENUM use_filenum, LSN max_acceptable_lsn)  __attribute__ ((warn_unused_result));

// clone an ft handle. the cloned handle has a new dict_id but refers to the same fractal tree
int toku_ft_handle_clone(FT_HANDLE *cloned_ft_handle, FT_HANDLE ft_handle, TOKUTXN txn);

// close an ft handle during normal operation. the underlying ft may or may not close,
// depending if there are still references. an lsn for this close will come from the logger.
void toku_ft_handle_close(FT_HANDLE ft_handle);
// close an ft handle during recovery. the underlying ft must close, and will use the given lsn.
void toku_ft_handle_close_recovery(FT_HANDLE ft_handle, LSN oplsn);

// At the ydb layer, a DICTIONARY_ID uniquely identifies an open dictionary.
// With the introduction of the loader (ticket 2216), it is possible for the file that holds
// an open dictionary to change, so these are now separate and independent unique identifiers (see FILENUM)
struct DICTIONARY_ID {
    uint64_t dictid;
};
static const DICTIONARY_ID DICTIONARY_ID_NONE = { .dictid = 0 };

int
toku_ft_handle_open_with_dict_id(
    FT_HANDLE ft_h, 
    const char *fname_in_env, 
    int is_create, 
    int only_create, 
    CACHETABLE cachetable, 
    TOKUTXN txn, 
    DICTIONARY_ID use_dictionary_id
    )  __attribute__ ((warn_unused_result));

// Effect: Insert a key and data pair into an ft
void toku_ft_insert (FT_HANDLE ft_h, DBT *k, DBT *v, TOKUTXN txn);

// Returns: 0 if the key was inserted, DB_KEYEXIST if the key already exists
int toku_ft_insert_unique(FT_HANDLE ft, DBT *k, DBT *v, TOKUTXN txn, bool do_logging);

// Effect: Optimize the ft
void toku_ft_optimize (FT_HANDLE ft_h);

// Effect: Insert a key and data pair into an ft if the oplsn is newer than the ft's lsn.  This function is called during recovery.
void toku_ft_maybe_insert (FT_HANDLE ft_h, DBT *k, DBT *v, TOKUTXN txn, bool oplsn_valid, LSN oplsn, bool do_logging, enum ft_msg_type type);

// Effect: Send an update message into an ft.  This function is called
// during recovery.
void toku_ft_maybe_update(FT_HANDLE ft_h, const DBT *key, const DBT *update_function_extra, TOKUTXN txn, bool oplsn_valid, LSN oplsn, bool do_logging);

// Effect: Send a broadcasting update message into an ft.  This function
// is called during recovery.
void toku_ft_maybe_update_broadcast(FT_HANDLE ft_h, const DBT *update_function_extra, TOKUTXN txn, bool oplsn_valid, LSN oplsn, bool do_logging, bool is_resetting_op);

void toku_ft_load_recovery(TOKUTXN txn, FILENUM old_filenum, char const * new_iname, int do_fsync, int do_log, LSN *load_lsn);
void toku_ft_load(FT_HANDLE ft_h, TOKUTXN txn, char const * new_iname, int do_fsync, LSN *get_lsn);
void toku_ft_hot_index_recovery(TOKUTXN txn, FILENUMS filenums, int do_fsync, int do_log, LSN *hot_index_lsn);
void toku_ft_hot_index(FT_HANDLE ft_h, TOKUTXN txn, FILENUMS filenums, int do_fsync, LSN *lsn);

void toku_ft_log_put_multiple (TOKUTXN txn, FT_HANDLE src_ft, FT_HANDLE *fts, uint32_t num_fts, const DBT *key, const DBT *val);
void toku_ft_log_put (TOKUTXN txn, FT_HANDLE ft_h, const DBT *key, const DBT *val);
void toku_ft_log_del_multiple (TOKUTXN txn, FT_HANDLE src_ft, FT_HANDLE *fts, uint32_t num_fts, const DBT *key, const DBT *val);
void toku_ft_log_del (TOKUTXN txn, FT_HANDLE ft_h, const DBT *key);

// Effect: Delete a key from an ft
void toku_ft_delete (FT_HANDLE ft_h, DBT *k, TOKUTXN txn);

// Effect: Delete a key from an ft if the oplsn is newer than the ft lsn.  This function is called during recovery.
void toku_ft_maybe_delete (FT_HANDLE ft_h, DBT *k, TOKUTXN txn, bool oplsn_valid, LSN oplsn, bool do_logging);

TXNID toku_ft_get_oldest_referenced_xid_estimate(FT_HANDLE ft_h);
struct txn_manager *toku_ft_get_txn_manager(FT_HANDLE ft_h);

struct txn_gc_info;
void toku_ft_send_insert(FT_HANDLE ft_h, DBT *key, DBT *val, XIDS xids, enum ft_msg_type type, txn_gc_info *gc_info);
void toku_ft_send_delete(FT_HANDLE ft_h, DBT *key, XIDS xids, txn_gc_info *gc_info);
void toku_ft_send_commit_any(FT_HANDLE ft_h, DBT *key, XIDS xids, txn_gc_info *gc_info);

int toku_close_ft_handle_nolsn (FT_HANDLE, char **error_string)  __attribute__ ((warn_unused_result));

int toku_dump_ft (FILE *,FT_HANDLE ft_h)  __attribute__ ((warn_unused_result));

extern int toku_ft_debug_mode;
int toku_verify_ft (FT_HANDLE ft_h)  __attribute__ ((warn_unused_result));
int toku_verify_ft_with_progress (FT_HANDLE ft_h, int (*progress_callback)(void *extra, float progress), void *extra, int verbose, int keep_going)  __attribute__ ((warn_unused_result));

int toku_ft_recount_rows(
    FT_HANDLE ft,
    int (*progress_callback)(
        uint64_t count,
        uint64_t deleted,
        void* progress_extra),
    void* progress_extra);


DICTIONARY_ID toku_ft_get_dictionary_id(FT_HANDLE);

enum ft_flags {
    //TOKU_DB_DUP             = (1<<0),  //Obsolete #2862
    //TOKU_DB_DUPSORT         = (1<<1),  //Obsolete #2862
    TOKU_DB_KEYCMP_BUILTIN  = (1<<2),
    TOKU_DB_VALCMP_BUILTIN_13  = (1<<3),
};

void toku_ft_keyrange(FT_HANDLE ft_h, DBT *key, uint64_t *less,  uint64_t *equal,  uint64_t *greater);
void toku_ft_keysrange(FT_HANDLE ft_h, DBT* key_left, DBT* key_right, uint64_t *less_p, uint64_t* equal_left_p, uint64_t* middle_p, uint64_t* equal_right_p, uint64_t* greater_p, bool* middle_3_exact_p);

int toku_ft_get_key_after_bytes(FT_HANDLE ft_h, const DBT *start_key, uint64_t skip_len, void (*callback)(const DBT *end_key, uint64_t actually_skipped, void *extra), void *cb_extra);

struct ftstat64_s {
    uint64_t nkeys; /* estimate how many unique keys (even when flattened this may be an estimate)     */
    uint64_t ndata; /* estimate the number of pairs (exact when flattened and committed)               */
    uint64_t dsize; /* estimate the sum of the sizes of the pairs (exact when flattened and committed) */
    uint64_t fsize;  /* the size of the underlying file                                                */
    uint64_t ffree; /* Number of free bytes in the underlying file                                    */
    uint64_t create_time_sec; /* creation time in seconds. */
    uint64_t modify_time_sec; /* time of last serialization, in seconds. */ 
    uint64_t verify_time_sec; /* time of last verification, in seconds */
};

void toku_ft_handle_stat64 (FT_HANDLE, TOKUTXN, struct ftstat64_s *stat);

struct ftinfo64 {
    uint64_t num_blocks_allocated;  // number of blocks in the blocktable
    uint64_t num_blocks_in_use;     // number of blocks in use by most recent checkpoint
    uint64_t size_allocated;        // sum of sizes of blocks in blocktable
    uint64_t size_in_use;           // sum of sizes of blocks in use by most recent checkpoint
};

void toku_ft_handle_get_fractal_tree_info64(FT_HANDLE, struct ftinfo64 *);

int toku_ft_handle_iterate_fractal_tree_block_map(FT_HANDLE, int (*)(uint64_t,int64_t,int64_t,int64_t,int64_t,void*), void *);

int toku_ft_layer_init(void) __attribute__ ((warn_unused_result));
void toku_ft_open_close_lock(void);
void toku_ft_open_close_unlock(void);
void toku_ft_layer_destroy(void);
void toku_ft_serialize_layer_init(void);
void toku_ft_serialize_layer_destroy(void);

void toku_maybe_truncate_file (int fd, uint64_t size_used, uint64_t expected_size, uint64_t *new_size);
// Effect: truncate file if overallocated by at least 32MiB

void toku_maybe_preallocate_in_file (int fd, int64_t size, int64_t expected_size, int64_t *new_size);
// Effect: make the file bigger by either doubling it or growing by 16MiB whichever is less, until it is at least size
// Return 0 on success, otherwise an error number.

int toku_ft_get_fragmentation(FT_HANDLE ft_h, TOKU_DB_FRAGMENTATION report) __attribute__ ((warn_unused_result));

bool toku_ft_is_empty_fast (FT_HANDLE ft_h) __attribute__ ((warn_unused_result));
// Effect: Return true if there are no messages or leaf entries in the tree.  If so, it's empty.  If there are messages  or leaf entries, we say it's not empty
// even though if we were to optimize the tree it might turn out that they are empty.

int toku_ft_strerror_r(int error, char *buf, size_t buflen);
// Effect: LIke the XSI-compliant strerorr_r, extended to db_strerror().
// If error>=0 then the result is to do strerror_r(error, buf, buflen), that is fill buf with a descriptive error message.
// If error<0 then return a PerconaFT-specific error code.  For unknown cases, we return -1 and set errno=EINVAL, even for cases that *should* be known.  (Not all DB errors are known by this function which is a bug.)

extern bool garbage_collection_debug;

// This is a poor place to put global options like these.
void toku_ft_set_direct_io(bool direct_io_on);
void toku_ft_set_compress_buffers_before_eviction(bool compress_buffers);

void toku_note_deserialized_basement_node(bool fixed_key_size);

// Creates all directories for the path if necessary,
// returns true if all dirs are created successfully or
// all dirs exist, false otherwise.
bool toku_create_subdirs_if_needed(const char* path);
