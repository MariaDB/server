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
#ifndef __OBJSTORE_H__
#define __OBJSTORE_H__

#include "compat.h"

/**
 * tidesdb_objstore_backend_t
 * identifies the object store backend in use.
 * prevents misuse by restricting to known, supported backends.
 */
typedef enum
{
    TDB_BACKEND_FS = 0, /* filesystem connector (local/NFS, always available) */
    TDB_BACKEND_S3 = 1, /* S3-compatible (AWS S3, MinIO, requires TIDESDB_WITH_S3) */
    TDB_BACKEND_UNKNOWN = 99
} tidesdb_objstore_backend_t;

/**
 * tidesdb_objstore_backend_name
 * return a human-readable string for a backend enum value
 * @param backend backend enum value
 * @return static string (e.g. "fs", "s3", "unknown")
 */
static inline const char *tidesdb_objstore_backend_name(tidesdb_objstore_backend_t backend)
{
    switch (backend)
    {
        case TDB_BACKEND_FS:
            return "fs";
        case TDB_BACKEND_S3:
            return "s3";
        default:
            return "unknown";
    }
}

/**
 * tidesdb_objstore_t
 * pluggable object store connector interface.
 * each function receives the opaque ctx pointer set at registration.
 * object keys are path-like strings (e.g. "cf_name/L1_100.klog").
 * connectors must be thread-safe -- multiple threads may call concurrently.
 * @param backend identifies the object store backend
 * @param put function pointer to upload an object from a local file
 * @param get function pointer to download an object to a local file
 * @param range_get function pointer to download a byte range into a buffer
 * @param delete_object function pointer to delete an object
 * @param exists function pointer to check if an object exists
 * @param list function pointer to enumerate objects under a prefix
 * @param destroy function pointer to free connector resources
 * @param ctx opaque connector context (client handle, credentials, etc.)
 */
typedef struct
{
    tidesdb_objstore_backend_t backend; /* identifies the object store backend */

    /**
     * put -- upload an object from a local file path.
     * the connector reads the file and uploads it as an atomic object.
     * @param ctx       opaque connector context
     * @param key       object key (path-like, e.g. "cf/L1_5.klog")
     * @param local_path path to the local file to upload
     * @return 0 on success, -1 on error
     */
    int (*put)(void *ctx, const char *key, const char *local_path);

    /**
     * get -- download an object to a local file path.
     * the connector creates intermediate directories as needed.
     * @param ctx       opaque connector context
     * @param key       object key
     * @param local_path path to write the downloaded file
     * @return 0 on success, -1 on error (including not found)
     */
    int (*get)(void *ctx, const char *key, const char *local_path);

    /**
     * range_get -- download a byte range of an object into a buffer.
     * used for fetching individual blocks without downloading the full file.
     * @param ctx       opaque connector context
     * @param key       object key
     * @param offset    byte offset to start reading
     * @param buf       output buffer (caller allocated)
     * @param size      number of bytes to read
     * @return bytes read on success, -1 on error
     */
    ssize_t (*range_get)(void *ctx, const char *key, uint64_t offset, void *buf, size_t size);

    /**
     * delete_object -- delete an object.
     * not-found is not an error.
     * @param ctx       opaque connector context
     * @param key       object key
     * @return 0 on success, -1 on error
     */
    int (*delete_object)(void *ctx, const char *key);

    /**
     * exists -- check if an object exists and optionally return its size.
     * @param ctx       opaque connector context
     * @param key       object key
     * @param size_out  if non-NULL, receives the object size in bytes
     * @return 1 if exists, 0 if not, -1 on error
     */
    int (*exists)(void *ctx, const char *key, size_t *size_out);

    /**
     * list -- enumerate objects under a key prefix.
     * calls the callback for each object found.
     * @param ctx       opaque connector context
     * @param prefix    key prefix to list (e.g. "cf/")
     * @param cb        callback invoked for each object (key, size, cb_ctx)
     * @param cb_ctx    opaque context passed to callback
     * @return number of objects listed, -1 on error
     */
    int (*list)(void *ctx, const char *prefix,
                void (*cb)(const char *key, size_t size, void *cb_ctx), void *cb_ctx);

    /**
     * destroy -- free connector resources.
     * called during tidesdb_close.
     * @param ctx       opaque connector context
     */
    void (*destroy)(void *ctx);

    void *ctx; /* opaque connector context (client handle, credentials, etc.) */
} tidesdb_objstore_t;

/**
 * tidesdb_objstore_config_t
 * configuration for object store mode behavior.
 * passed to tidesdb_config_t.object_store_config.
 * NULL means use defaults.
 * @param local_cache_path local directory for cached sstable files (NULL = use db_path)
 * @param local_cache_max_bytes maximum cache size in bytes (0 = unlimited)
 * @param cache_on_read whether to cache downloaded files locally (default 1)
 * @param cache_on_write whether to keep local copy after upload (default 1)
 * @param max_concurrent_uploads number of parallel upload threads (default 4)
 * @param max_concurrent_downloads number of parallel download threads (default 8)
 * @param multipart_threshold byte threshold above which multipart upload is used (default 64MB)
 * @param multipart_part_size chunk size for multipart uploads (default 8MB)
 * @param sync_manifest_to_object whether to upload MANIFEST after each compaction (default 1)
 * @param replicate_wal whether to upload closed WAL segments (default 1)
 * @param wal_upload_sync 0 for background WAL upload (default), 1 to block flush
 * @param wal_sync_threshold_bytes sync active WAL to object store when it grows by this many bytes
 *        since the last sync (default 1MB, 0 = disable periodic WAL sync). uses the block manager
 *        atomic file size for lock-free detection. the reaper thread checks every cycle (~100ms)
 *        and uploads when the threshold is exceeded, bounding the data loss window to the
 *        write volume rather than wall clock time
 * @param wal_sync_on_commit upload WAL after every txn commit for RPO=0 replication (default 0)
 * @param replica_mode enable read-only replica mode (default 0)
 * @param replica_sync_interval_us MANIFEST poll interval in microseconds (default 5000000)
 * @param replica_replay_wal replay WAL for near-real-time reads on replicas (default 1)
 */
typedef struct
{
    const char *local_cache_path;
    size_t local_cache_max_bytes;
    int cache_on_read;
    int cache_on_write;
    int max_concurrent_uploads;
    int max_concurrent_downloads;
    size_t multipart_threshold;
    size_t multipart_part_size;
    int sync_manifest_to_object;
    int replicate_wal;
    int wal_upload_sync;
    size_t wal_sync_threshold_bytes;
    int wal_sync_on_commit;
    int replica_mode;
    uint64_t replica_sync_interval_us;
    int replica_replay_wal;
} tidesdb_objstore_config_t;

/**
 * tidesdb_objstore_default_config
 * @return default object store configuration
 */
tidesdb_objstore_config_t tidesdb_objstore_default_config(void);

/**
 * tidesdb_objstore_fs_create
 * create a filesystem-backed connector (for testing and local replication).
 * stores objects as files under root_dir, mirroring the key path structure.
 * @param root_dir directory to store objects in
 * @return connector handle, or NULL on error. caller must eventually call destroy.
 */
tidesdb_objstore_t *tidesdb_objstore_fs_create(const char *root_dir);

#ifdef TIDESDB_WITH_S3
#include "objstore_s3.h"
#endif

#endif /* __OBJSTORE_H__ */
