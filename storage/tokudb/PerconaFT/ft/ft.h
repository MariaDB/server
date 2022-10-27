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

#include <db.h>

#include "ft/cachetable/cachetable.h"
#include "ft/ft-ops.h"
#include "ft/logger/log.h"
#include "util/dbt.h"
#ifndef TOKU_MYSQL_WITH_PFS
#include <my_global.h>
#endif

typedef struct ft *FT;
typedef struct ft_options *FT_OPTIONS;

// unlink a ft from the filesystem with or without a txn.
// if with a txn, then the unlink happens on commit.
void toku_ft_unlink(FT_HANDLE handle);
void toku_ft_unlink_on_commit(FT_HANDLE handle, TOKUTXN txn);

int toku_ft_rename_iname(DB_TXN *txn,
                         const char *data_dir,
                         const char *old_iname,
                         const char *new_iname,
                         CACHETABLE ct);

void toku_ft_init_reflock(FT ft);
void toku_ft_destroy_reflock(FT ft);
void toku_ft_grab_reflock(FT ft);
void toku_ft_release_reflock(FT ft);

void toku_ft_lock(struct ft *ft);
void toku_ft_unlock(struct ft *ft);

void toku_ft_create(FT *ftp, FT_OPTIONS options, CACHEFILE cf, TOKUTXN txn);
void toku_ft_free (FT ft);

int toku_read_ft_and_store_in_cachefile (FT_HANDLE ft_h, CACHEFILE cf, LSN max_acceptable_lsn, FT *header);
void toku_ft_note_ft_handle_open(FT ft, FT_HANDLE live);

bool toku_ft_needed_unlocked(FT ft);
bool toku_ft_has_one_reference_unlocked(FT ft);

// evict a ft from memory by closing its cachefile. any future work
// will have to read in the ft in a new cachefile and new FT object.
void toku_ft_evict_from_memory(FT ft, bool oplsn_valid, LSN oplsn);

FT_HANDLE toku_ft_get_only_existing_ft_handle(FT ft);

void toku_ft_note_hot_begin(FT_HANDLE ft_h);
void toku_ft_note_hot_complete(FT_HANDLE ft_h, bool success, MSN msn_at_start_of_hot);

void
toku_ft_init(
    FT ft,
    BLOCKNUM root_blocknum_on_disk,
    LSN checkpoint_lsn,
    TXNID root_xid_that_created,
    uint32_t target_nodesize,
    uint32_t target_basementnodesize,
    enum toku_compression_method compression_method,
    uint32_t fanout
    );

int toku_dictionary_redirect_abort(FT old_h, FT new_h, TOKUTXN txn) __attribute__ ((warn_unused_result));
int toku_dictionary_redirect (const char *dst_fname_in_env, FT_HANDLE old_ft, TOKUTXN txn);
void toku_reset_root_xid_that_created(FT ft, TXNID new_root_xid_that_created);
// Reset the root_xid_that_created field to the given value.
// This redefines which xid created the dictionary.

void toku_ft_add_txn_ref(FT ft);
void toku_ft_remove_txn_ref(FT ft);

void toku_calculate_root_offset_pointer (FT ft, CACHEKEY* root_key, uint32_t *roothash);
void toku_ft_set_new_root_blocknum(FT ft, CACHEKEY new_root_key);
LSN toku_ft_checkpoint_lsn(FT ft)  __attribute__ ((warn_unused_result));
void toku_ft_stat64 (FT ft, struct ftstat64_s *s);
void toku_ft_get_fractal_tree_info64 (FT ft, struct ftinfo64 *s);
int toku_ft_iterate_fractal_tree_block_map(FT ft, int (*iter)(uint64_t,int64_t,int64_t,int64_t,int64_t,void*), void *iter_extra);

// unconditionally set the descriptor for an open FT. can't do this when
// any operation has already occurred on the ft.
// see toku_ft_change_descriptor(), which is the transactional version
// used by the ydb layer. it better describes the client contract.
void toku_ft_update_descriptor(FT ft, DESCRIPTOR desc);
// use this version if the FT is not fully user-opened with a valid cachefile.
// this is a clean hack to get deserialization code to update a descriptor
// while the FT and cf are in the process of opening, for upgrade purposes
void toku_ft_update_descriptor_with_fd(FT ft, DESCRIPTOR desc, int fd);
void toku_ft_update_cmp_descriptor(FT ft);

// get the descriptor for a ft. safe to read as long as clients honor the
// strict contract put forth by toku_ft_update_descriptor/toku_ft_change_descriptor
// essentially, there should never be a reader while there is a writer, enforced
// by the client, not the FT.
DESCRIPTOR toku_ft_get_descriptor(FT_HANDLE ft_handle);
DESCRIPTOR toku_ft_get_cmp_descriptor(FT_HANDLE ft_handle);

typedef struct {
    // delta versions in basements could be negative
    // These represent the physical leaf entries and do not account
    // for pending deletes or other in-flight messages that have not been
    // applied to a leaf entry.
    int64_t numrows;
    int64_t numbytes;
} STAT64INFO_S, *STAT64INFO;
static const STAT64INFO_S ZEROSTATS = { .numrows = 0, .numbytes = 0 };

void toku_ft_update_stats(STAT64INFO headerstats, STAT64INFO_S delta);
void toku_ft_decrease_stats(STAT64INFO headerstats, STAT64INFO_S delta);
void toku_ft_adjust_logical_row_count(FT ft, int64_t delta);

typedef void (*remove_ft_ref_callback)(FT ft, void *extra);
void toku_ft_remove_reference(FT ft,
                              bool oplsn_valid, LSN oplsn,
                              remove_ft_ref_callback remove_ref, void *extra);

void toku_ft_set_nodesize(FT ft, unsigned int nodesize);
void toku_ft_get_nodesize(FT ft, unsigned int *nodesize);
void toku_ft_set_basementnodesize(FT ft, unsigned int basementnodesize);
void toku_ft_get_basementnodesize(FT ft, unsigned int *basementnodesize);
void toku_ft_set_compression_method(FT ft, enum toku_compression_method method);
void toku_ft_get_compression_method(FT ft, enum toku_compression_method *methodp);
void toku_ft_set_fanout(FT ft, unsigned int fanout);
void toku_ft_get_fanout(FT ft, unsigned int *fanout);

// mark the ft as a blackhole. any message injections will be a no op.
void toku_ft_set_blackhole(FT_HANDLE ft_handle);

// Effect: Calculates the total space and used space for a FT's leaf data.
//         The difference between the two is MVCC garbage.
void toku_ft_get_garbage(FT ft, uint64_t *total_space, uint64_t *used_space);

// TODO: Should be in portability
int get_num_cores(void);

// TODO: Use the cachetable's worker pool instead of something managed by the FT...
struct toku_thread_pool *get_ft_pool(void);

// TODO: Should be in portability
int toku_single_process_lock(const char *lock_dir, const char *which, int *lockfd);
int toku_single_process_unlock(int *lockfd);

void tokuft_update_product_name_strings(void);
#define TOKU_MAX_PRODUCT_NAME_LENGTH (256)
extern char toku_product_name[TOKU_MAX_PRODUCT_NAME_LENGTH];

struct toku_product_name_strings_struct {
    char db_version[sizeof(toku_product_name) + sizeof("1.2.3 build ") + 256 + 1];
    char environmentdictionary[sizeof(toku_product_name) + sizeof(".environment") + 1];
    char fileopsdirectory[sizeof(toku_product_name) + sizeof(".directory") + 1];
    char single_process_lock[sizeof(toku_product_name) + sizeof("___lock_dont_delete_me") + 1];
    char rollback_cachefile[sizeof(toku_product_name) + sizeof(".rollback") + 1];
};

extern struct toku_product_name_strings_struct toku_product_name_strings;
extern int tokuft_num_envs;
