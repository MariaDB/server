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

#include "manifest.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MANIFEST_TMP_EXT     ".tmp."
#define MANIFEST_TMP_EXT_LEN (sizeof(MANIFEST_TMP_EXT) - 1)

/**
 * tidesdb_manifest_add_sstable_unlocked
 * adds an sstable to the manifest
 * @param manifest manifest to add sstable to
 * @param level level of sstable
 * @param id id of sstable
 * @param num_entries number of entries in sstable
 * @param size_bytes size of sstable in bytes
 * @return 0 on success, -1 on error
 */
static int tidesdb_manifest_add_sstable_unlocked(tidesdb_manifest_t *manifest, int level,
                                                 uint64_t id, uint64_t num_entries,
                                                 uint64_t size_bytes);

tidesdb_manifest_t *tidesdb_manifest_open(const char *path)
{
    if (!path) return NULL;

    tidesdb_manifest_t *manifest = malloc(sizeof(tidesdb_manifest_t));
    if (!manifest) return NULL;

    manifest->entries = malloc(sizeof(tidesdb_manifest_entry_t) * MANIFEST_INITIAL_CAPACITY);
    if (!manifest->entries)
    {
        free(manifest);
        return NULL;
    }

    manifest->num_entries = 0;
    manifest->capacity = MANIFEST_INITIAL_CAPACITY;
    atomic_init(&manifest->sequence, 0);
    manifest->fp = NULL;
    atomic_init(&manifest->active_ops, 0);
    strncpy(manifest->path, path, MANIFEST_PATH_LEN - 1);
    manifest->path[MANIFEST_PATH_LEN - 1] = '\0';

    if (pthread_rwlock_init(&manifest->lock, NULL) != 0)
    {
        free(manifest->entries);
        free(manifest);
        return NULL;
    }

    /* we clean up orphaned temp files from incomplete commits
     * temp files are named -- <path>MANIFEST_TMP_EXT<thread_id>.<pid>
     * if main manifest exists, temp files are stale and can be removed */
    char dir_path[MANIFEST_PATH_LEN];
    const char *last_sep = strrchr(path, PATH_SEPARATOR[0]);
    if (last_sep)
    {
        const size_t dir_len = last_sep - path;
        if (dir_len < sizeof(dir_path))
        {
            memcpy(dir_path, path, dir_len);
            dir_path[dir_len] = '\0';
        }
        else
        {
            strcpy(dir_path, ".");
        }
    }
    else
    {
        strcpy(dir_path, ".");
    }

    /* base filename for pattern matching */
    const char *base_name = last_sep ? last_sep + 1 : path;
    const size_t base_len = strlen(base_name);

    /* we scan directory looking for orphaned temp files */
    DIR *dir = opendir(dir_path);
    if (dir)
    {
        const size_t dir_path_len = strlen(dir_path);
        const size_t sep_len = strlen(PATH_SEPARATOR);
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL)
        {
            /* we check if filename matches pattern -- <base_name>MANIFEST_TMP_EXT* */
            const size_t entry_len = strlen(entry->d_name);
            if (entry_len > base_len + MANIFEST_TMP_EXT_LEN &&
                strncmp(entry->d_name, base_name, base_len) == 0 &&
                strncmp(entry->d_name + base_len, MANIFEST_TMP_EXT, MANIFEST_TMP_EXT_LEN) == 0)
            {
                /* found orphaned temp file, we remove it */
                char temp_full_path[MANIFEST_PATH_LEN];
                /* we check if combined path fits in buffer (dir + separator + entry + null) */
                if (dir_path_len + sep_len + entry_len + 1 <= MANIFEST_PATH_LEN)
                {
                    size_t offset = 0;
                    memcpy(temp_full_path + offset, dir_path, dir_path_len);
                    offset += dir_path_len;
                    memcpy(temp_full_path + offset, PATH_SEPARATOR, sep_len);
                    offset += sep_len;
                    memcpy(temp_full_path + offset, entry->d_name, entry_len);
                    offset += entry_len;
                    temp_full_path[offset] = '\0';
                    remove(temp_full_path);
                }
            }
        }
        closedir(dir);
    }

    FILE *fp = tdb_fopen(path, "r");
    if (!fp)
    {
        /* the file doesnt exist, return empty manifest */
        if (errno == ENOENT) return manifest;
        /* other error */
        pthread_rwlock_destroy(&manifest->lock);
        free(manifest->entries);
        free(manifest);
        return NULL;
    }

    char line[MANIFEST_MAX_LINE_LEN];

    if (fgets(line, sizeof(line), fp))
    {
        char *endptr;
        const long version = strtol(line, &endptr, 10);
        if (endptr == line || version != MANIFEST_VERSION)
        {
            fclose(fp);
            pthread_rwlock_destroy(&manifest->lock);
            free(manifest->entries);
            free(manifest);
            return NULL;
        }
    }
    else
    {
        /* empty file, keep it open */
        manifest->fp = fp;
        return manifest;
    }

    if (fgets(line, sizeof(line), fp))
    {
        char *seq_endptr;
        const unsigned long long seq = strtoull(line, &seq_endptr, 10);
        /* the sequence line must be a number terminated by end-of-line. reject junk
         * (e.g. "123abc") rather than silently truncating it -- an under-parsed
         * sequence under-seeds next_sstable_id on recovery and risks id collisions */
        if (seq_endptr == line ||
            (*seq_endptr != '\0' && *seq_endptr != '\n' && *seq_endptr != '\r'))
        {
            fclose(fp);
            pthread_rwlock_destroy(&manifest->lock);
            free(manifest->entries);
            free(manifest);
            return NULL;
        }
        atomic_store(&manifest->sequence, seq);
    }

    int skipped_lines = 0;
    while (fgets(line, sizeof(line), fp))
    {
        const char *ptr = line;
        char *endptr;

        /* parse level */
        const long level_val = strtol(ptr, &endptr, 10);
        if (endptr == ptr || *endptr != ',')
        {
            skipped_lines++;
            continue;
        }
        const int level = (int)level_val;
        ptr = endptr + 1;

        /* parse id */
        const uint64_t id = strtoull(ptr, &endptr, 10);
        if (endptr == ptr || *endptr != ',')
        {
            skipped_lines++;
            continue;
        }
        ptr = endptr + 1;

        /* parse num_entries */
        const uint64_t num_entries = strtoull(ptr, &endptr, 10);
        if (endptr == ptr || *endptr != ',')
        {
            skipped_lines++;
            continue;
        }
        ptr = endptr + 1;

        /* parse size_bytes */
        const uint64_t size_bytes = strtoull(ptr, &endptr, 10);
        if (endptr == ptr)
        {
            skipped_lines++;
            continue;
        }

        tidesdb_manifest_add_sstable_unlocked(manifest, level, id, num_entries, size_bytes);
    }

    /* surface silent data loss, malformed entry lines were dropped. this leaf module has
     * no access to the db log, so a single stderr line is the best signal available. */
    if (skipped_lines > 0)
    {
        fprintf(stderr, "tidesdb manifest: skipped %d malformed entry line(s) while loading %s\n",
                skipped_lines, manifest->path[0] ? manifest->path : "(unknown)");
    }

    /* we keep file open for future use */
    manifest->fp = fp;

    return manifest;
}

/**
 * tidesdb_manifest_add_sstable_unlocked
 * adds an sstable to the manifest
 * @param manifest manifest to add sstable to
 * @param level level of sstable
 * @param id id of sstable
 * @param num_entries number of entries in sstable
 * @param size_bytes size of sstable in bytes
 * @return 0 on success, -1 on error
 */
static int tidesdb_manifest_add_sstable_unlocked(tidesdb_manifest_t *manifest, const int level,
                                                 const uint64_t id, const uint64_t num_entries,
                                                 const uint64_t size_bytes)
{
    for (int i = 0; i < manifest->num_entries; i++)
    {
        if (manifest->entries[i].level == level && manifest->entries[i].id == id)
        {
            manifest->entries[i].num_entries = num_entries;
            manifest->entries[i].size_bytes = size_bytes;
            return 0;
        }
    }

    if (manifest->num_entries >= manifest->capacity)
    {
        const int new_capacity = manifest->capacity * 2;
        tidesdb_manifest_entry_t *new_entries =
            realloc(manifest->entries, sizeof(tidesdb_manifest_entry_t) * new_capacity);
        if (!new_entries)
        {
            return -1;
        }

        manifest->entries = new_entries;
        manifest->capacity = new_capacity;
    }

    manifest->entries[manifest->num_entries].level = level;
    manifest->entries[manifest->num_entries].id = id;
    manifest->entries[manifest->num_entries].num_entries = num_entries;
    manifest->entries[manifest->num_entries].size_bytes = size_bytes;
    manifest->num_entries++;

    return 0;
}

int tidesdb_manifest_add_sstable(tidesdb_manifest_t *manifest, const int level, const uint64_t id,
                                 const uint64_t num_entries, const uint64_t size_bytes)
{
    if (!manifest) return -1;

    atomic_fetch_add(&manifest->active_ops, 1);
    pthread_rwlock_wrlock(&manifest->lock);
    const int result =
        tidesdb_manifest_add_sstable_unlocked(manifest, level, id, num_entries, size_bytes);
    pthread_rwlock_unlock(&manifest->lock);
    atomic_fetch_sub(&manifest->active_ops, 1);
    return result;
}

int tidesdb_manifest_remove_sstable(tidesdb_manifest_t *manifest, const int level,
                                    const uint64_t id)
{
    if (!manifest) return -1;

    atomic_fetch_add(&manifest->active_ops, 1);
    pthread_rwlock_wrlock(&manifest->lock);

    for (int i = 0; i < manifest->num_entries; i++)
    {
        if (manifest->entries[i].level == level && manifest->entries[i].id == id)
        {
            /* we swap with last element for O(1) removal (order not required) */
            manifest->entries[i] = manifest->entries[manifest->num_entries - 1];
            manifest->num_entries--;
            pthread_rwlock_unlock(&manifest->lock);
            atomic_fetch_sub(&manifest->active_ops, 1);
            return 0;
        }
    }

    pthread_rwlock_unlock(&manifest->lock);
    atomic_fetch_sub(&manifest->active_ops, 1);
    return -1;
}

int tidesdb_manifest_has_sstable(tidesdb_manifest_t *manifest, const int level, const uint64_t id)
{
    if (!manifest) return 0;

    atomic_fetch_add(&manifest->active_ops, 1);
    pthread_rwlock_rdlock(&manifest->lock);

    for (int i = 0; i < manifest->num_entries; i++)
    {
        if (manifest->entries[i].level == level && manifest->entries[i].id == id)
        {
            pthread_rwlock_unlock(&manifest->lock);
            atomic_fetch_sub(&manifest->active_ops, 1);
            return 1;
        }
    }

    pthread_rwlock_unlock(&manifest->lock);
    atomic_fetch_sub(&manifest->active_ops, 1);
    return 0;
}

void tidesdb_manifest_update_sequence(tidesdb_manifest_t *manifest, uint64_t sequence)
{
    if (!manifest) return;

    /* monotonic guard, the sequence seeds next_sstable_id on recovery, so it must never
     * regress or recovery would re-hand-out live sstable ids and collide. cas loop so a
     * concurrent larger store is never clobbered by a smaller one. */
    uint64_t cur = atomic_load(&manifest->sequence);
    while (sequence > cur && !atomic_compare_exchange_weak(&manifest->sequence, &cur, sequence))
    {
        /* cur reloaded with the live value on failure; loop re-checks sequence > cur */
    }
}

int tidesdb_manifest_commit(tidesdb_manifest_t *manifest, const char *path)
{
    if (!manifest || !path) return -1;

    atomic_fetch_add(&manifest->active_ops, 1);
    pthread_rwlock_wrlock(&manifest->lock);

    /* we update stored path if it changed */
    if (strcmp(manifest->path, path) != 0)
    {
        strncpy(manifest->path, path, MANIFEST_PATH_LEN - 1);
        manifest->path[MANIFEST_PATH_LEN - 1] = '\0';
    }

    if (manifest->fp)
    {
        fclose(manifest->fp);
        manifest->fp = NULL;
    }

    char temp_path[MANIFEST_PATH_LEN];
    snprintf(temp_path, sizeof(temp_path), "%s" MANIFEST_TMP_EXT "%lu.%d", path,
             (unsigned long)TDB_THREAD_ID(), TDB_GETPID());

    FILE *fp = tdb_fopen(temp_path, "w");
    if (!fp)
    {
        pthread_rwlock_unlock(&manifest->lock);
        atomic_fetch_sub(&manifest->active_ops, 1);
        return -1;
    }

    fprintf(fp, "%d\n", MANIFEST_VERSION);
    fprintf(fp, "%" PRIu64 "\n", atomic_load(&manifest->sequence));

    for (int i = 0; i < manifest->num_entries; i++)
    {
        fprintf(fp, "%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n", manifest->entries[i].level,
                manifest->entries[i].id, manifest->entries[i].num_entries,
                manifest->entries[i].size_bytes);
    }

    if (fflush(fp) != 0)
    {
        fclose(fp);
        remove(temp_path);
        pthread_rwlock_unlock(&manifest->lock);
        atomic_fetch_sub(&manifest->active_ops, 1);
        return -1;
    }

    const int fd = tdb_fileno(fp);
    if (fd >= 0)
    {
        if (tdb_fsync(fd) != 0)
        {
            fclose(fp);
            remove(temp_path);
            pthread_rwlock_unlock(&manifest->lock);
            atomic_fetch_sub(&manifest->active_ops, 1);
            return -1;
        }
    }

    fclose(fp);

    /* atomic rename -- this is the commit point */
    if (atomic_rename_file(temp_path, path) != 0)
    {
        remove(temp_path);
        pthread_rwlock_unlock(&manifest->lock);
        atomic_fetch_sub(&manifest->active_ops, 1);
        return -1;
    }

    /* we sync the parent directory to ensure the rename is durable.
     * without this, a crash after rename could lose the directory entry
     * on POSIX systems that don't flush directory metadata automatically. */
    {
        /* sized to the full manifest path length -- a 1024-byte buffer silently truncated
         * paths > 1023 chars and synced the wrong directory */
        char dir_buf[MANIFEST_PATH_LEN];
        strncpy(dir_buf, path, sizeof(dir_buf) - 1);
        dir_buf[sizeof(dir_buf) - 1] = '\0';
        char *last_sep = strrchr(dir_buf, '/');
#ifdef _WIN32
        if (!last_sep) last_sep = strrchr(dir_buf, '\\');
#endif
        if (last_sep)
        {
            *last_sep = '\0';
            tdb_sync_directory(dir_buf);
        }
    }

    /* we reopen for reading */
    manifest->fp = tdb_fopen(path, "r");

    pthread_rwlock_unlock(&manifest->lock);
    atomic_fetch_sub(&manifest->active_ops, 1);
    return 0;
}

void tidesdb_manifest_close(tidesdb_manifest_t *manifest)
{
    if (!manifest) return;

    /* wait for all active operations to complete before destroying */
    int wait_count = 0;
    while (atomic_load(&manifest->active_ops) > 0 && wait_count < MANIFEST_CLOSE_MAX_WAITS)
    {
        usleep(MANIFEST_CLOSE_WAIT_US);
        wait_count++;
    }

    pthread_rwlock_wrlock(&manifest->lock);

    if (manifest->fp)
    {
        fclose(manifest->fp);
        manifest->fp = NULL;
    }

    pthread_rwlock_unlock(&manifest->lock);
    pthread_rwlock_destroy(&manifest->lock);
    free(manifest->entries);
    free(manifest);
}