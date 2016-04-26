/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
/*======
This file is part of TokuDB


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    TokuDBis is free software: you can redistribute it and/or modify
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

namespace tokudb {
    uint compute_total_key_parts(TABLE_SHARE *table_share) {
        uint total_key_parts = 0;
        for (uint i = 0; i < table_share->keys; i++) {
            total_key_parts += table_share->key_info[i].user_defined_key_parts;
        }
        return total_key_parts;
    }

    // Put the cardinality counters into the status dictionary.
    int set_card_in_status(
        DB* status_db,
        DB_TXN* txn,
        uint rec_per_keys,
        const uint64_t rec_per_key[]) {

        // encode cardinality into the buffer
        tokudb::buffer b;
        size_t s;
        s = b.append_ui<uint32_t>(rec_per_keys);
        assert_always(s > 0);
        for (uint i = 0; i < rec_per_keys; i++) {
            s = b.append_ui<uint64_t>(rec_per_key[i]);
            assert_always(s > 0);
        }
        // write cardinality to status
        int error =
            tokudb::metadata::write(
                status_db,
                hatoku_cardinality,
                b.data(),
                b.size(),
                txn);
        return error;
    }

    // Get the cardinality counters from the status dictionary.
    int get_card_from_status(
        DB* status_db,
        DB_TXN* txn,
        uint rec_per_keys,
        uint64_t rec_per_key[]) {

        // read cardinality from status
        void* buf = 0; size_t buf_size = 0;
        int error =
            tokudb::metadata::read_realloc(
                status_db,
                txn,
                hatoku_cardinality,
                &buf,
                &buf_size);
        if (error == 0) {
            // decode cardinality from the buffer
            tokudb::buffer b(buf, 0, buf_size);
            size_t s;
            uint32_t num_parts;
            s = b.consume_ui<uint32_t>(&num_parts);
            if (s == 0 || num_parts != rec_per_keys) 
                error = EINVAL;
            if (error == 0) {
                for (uint i = 0; i < rec_per_keys; i++) {
                    s = b.consume_ui<uint64_t>(&rec_per_key[i]);
                    if (s == 0) {
                        error = EINVAL;
                        break;
                    }
                }
            }
        }
        // cleanup
        free(buf);
        return error;
    }

    // Delete the cardinality counters from the status dictionary.
    int delete_card_from_status(DB* status_db, DB_TXN* txn) {
        int error =
            tokudb::metadata::remove(status_db, hatoku_cardinality, txn);
        return error;
    }

    bool find_index_of_key(
        const char* key_name,
        TABLE_SHARE* table_share,
        uint* index_offset_ptr) {

        for (uint i = 0; i < table_share->keys; i++) {
            if (strcmp(key_name, table_share->key_info[i].name) == 0) {
                *index_offset_ptr = i;
                return true;
            }
        }
        return false;
    }

    static void copy_card(uint64_t *dest, uint64_t *src, size_t n) {
        for (size_t i = 0; i < n; i++)
            dest[i] = src[i];
    }

    // Altered table cardinality = select cardinality data from current table
    // cardinality for keys that exist
    // in the altered table and the current table.
    int alter_card(
        DB* status_db,
        DB_TXN *txn,
        TABLE_SHARE* table_share,
        TABLE_SHARE* altered_table_share) {

        int error;
        // read existing cardinality data from status
        uint table_total_key_parts =
            tokudb::compute_total_key_parts(table_share);

        uint64_t rec_per_key[table_total_key_parts];
        error =
            get_card_from_status(
                status_db,
                txn,
                table_total_key_parts,
                rec_per_key);
        // set altered records per key to unknown
        uint altered_table_total_key_parts =
            tokudb::compute_total_key_parts(altered_table_share);
        uint64_t altered_rec_per_key[altered_table_total_key_parts];
        for (uint i = 0; i < altered_table_total_key_parts; i++)
            altered_rec_per_key[i] = 0;
        // compute the beginning of the key offsets in the original table
        uint orig_key_offset[table_share->keys];
        uint orig_key_parts = 0;
        for (uint i = 0; i < table_share->keys; i++) {
            orig_key_offset[i] = orig_key_parts;
            orig_key_parts += table_share->key_info[i].user_defined_key_parts;
        }
        // if orig card data exists, then use it to compute new card data
        if (error == 0) {
            uint next_key_parts = 0;
            for (uint i = 0; error == 0 && i < altered_table_share->keys; i++) {
                uint ith_key_parts =
                    altered_table_share->key_info[i].user_defined_key_parts;
                uint orig_key_index;
                if (find_index_of_key(
                        altered_table_share->key_info[i].name,
                        table_share,
                        &orig_key_index)) {
                    copy_card(
                        &altered_rec_per_key[next_key_parts],
                        &rec_per_key[orig_key_offset[orig_key_index]],
                        ith_key_parts);
                }
                next_key_parts += ith_key_parts;
            }
        }
        if (error == 0) {
            error =
                set_card_in_status(
                    status_db,
                    txn,
                    altered_table_total_key_parts,
                    altered_rec_per_key);
        } else {
            error = delete_card_from_status(status_db, txn);
        }
        return error;
    }
}
