/**
 *
 * Copyright (C) TidesDB
 *
 * Original Author: Alex Gaetano Padula
 *
 * Licensed under the Mozilla Public License, v. 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.mozilla.org/en-US/MPL/2.0/
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef __MANIFEST_H__
#define __MANIFEST_H__

#define MANIFEST_INITIAL_CAPACITY 64
#define MANIFEST_VERSION          7
#define MANIFEST_PATH_LEN         4096
#define MANIFEST_MAX_LINE_LEN     256
/* microseconds to wait between checks */
#define MANIFEST_CLOSE_WAIT_US 100
/* max iterations (10000 × 100μs = 1 second) */
#define MANIFEST_CLOSE_MAX_WAITS 10000

#include "compat.h"

/**
 * tidesdb_manifest_entry_t
 * represents a single sstable entry in the manifest
 * @param level level number (1-based)
 * @param id sstable ID
 * @param num_entries number of entries in sstable
 * @param size_bytes total size in bytes
 */
typedef struct
{
    int level;
    uint64_t id;
    uint64_t num_entries;
    uint64_t size_bytes;
} tidesdb_manifest_entry_t;

/**
 * tidesdb_manifest_t
 * in-memory representation of manifest file
 * @param entries array of sstable entries
 * @param num_entries number of entries
 * @param capacity capacity of entries array
 * @param sequence current global sequence number
 * @param path path to manifest file
 * @param fp read-mode handle to the manifest file; opened when the manifest is parsed and
 *           reopened after each commit. commits write a temp file and atomically rename it into
 *           place, so they never write through this handle
 * @param lock reader-writer lock for thread safety
 * @param active_ops count of active operations (for safe shutdown)
 */
typedef struct
{
    tidesdb_manifest_entry_t *entries;
    int num_entries;
    int capacity;
    _Atomic(uint64_t) sequence;
    char path[MANIFEST_PATH_LEN];
    FILE *fp;
    pthread_rwlock_t lock;
    _Atomic(int) active_ops;
} tidesdb_manifest_t;

/**
 * tidesdb_manifest_open
 * opens manifest from file, creating new if it doesn't exist
 * @param path path to manifest file
 * @return opened manifest or NULL on error
 */
tidesdb_manifest_t *tidesdb_manifest_open(const char *path);

/**
 * tidesdb_manifest_add_sstable
 * adds an sstable entry to the manifest
 * @param manifest manifest to modify
 * @param level level number
 * @param id sstable ID
 * @param num_entries number of entries
 * @param size_bytes size in bytes
 * @return 0 on success, -1 on error
 */
int tidesdb_manifest_add_sstable(tidesdb_manifest_t *manifest, int level, uint64_t id,
                                 uint64_t num_entries, uint64_t size_bytes);

/**
 * tidesdb_manifest_remove_sstable
 * removes an sstable entry from the manifest
 * @param manifest manifest to modify
 * @param level level number
 * @param id sstable ID
 * @return 0 on success, -1 on error
 */
int tidesdb_manifest_remove_sstable(tidesdb_manifest_t *manifest, int level, uint64_t id);

/**
 * tidesdb_manifest_has_sstable
 * checks if manifest contains an sstable
 * @param manifest manifest to check
 * @param level level number
 * @param id sstable ID
 * @return 1 if exists, 0 if not
 */
int tidesdb_manifest_has_sstable(tidesdb_manifest_t *manifest, int level, uint64_t id);

/**
 * tidesdb_manifest_update_sequence
 * updates the global sequence number
 * @param manifest manifest to modify
 * @param sequence new sequence number
 */
void tidesdb_manifest_update_sequence(tidesdb_manifest_t *manifest, uint64_t sequence);

/**
 * tidesdb_manifest_commit
 * atomically writes the manifest to disk -- writes a temp file, fsyncs it, renames it into
 * place, then syncs the parent directory so the commit survives a crash. if path differs from
 * the manifest's currently stored path, the manifest is re-pointed to path.
 * @param manifest manifest to write
 * @param path destination path; also becomes the manifest's stored path if it differs
 * @return 0 on success, -1 on error
 */
int tidesdb_manifest_commit(tidesdb_manifest_t *manifest, const char *path);

/**
 * tidesdb_manifest_close
 * closes manifest and frees memory
 * @param manifest manifest to close
 */
void tidesdb_manifest_close(tidesdb_manifest_t *manifest);

#endif /* __MANIFEST_H__ */