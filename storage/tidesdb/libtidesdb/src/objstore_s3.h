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
#ifndef __OBJSTORE_S3_H__
#define __OBJSTORE_S3_H__

#include "objstore.h"

/**
 * tidesdb_objstore_s3_create
 * create an S3-compatible object store connector.
 * works with AWS S3, MinIO, etc.
 *
 * @param endpoint      S3 endpoint (e.g. "s3.amazonaws.com" or "minio.local:9000")
 * @param bucket        bucket name
 * @param prefix        key prefix (e.g. "production/db1/"), can be NULL
 * @param access_key    AWS access key ID
 * @param secret_key    AWS secret access key
 * @param region        AWS region (e.g. "us-east-1"), NULL for MinIO
 * @param use_ssl       1 for HTTPS, 0 for HTTP
 * @param use_path_style 1 for path-style URLs (MinIO), 0 for virtual-hosted (AWS)
 * @return connector handle, or NULL on error
 */
tidesdb_objstore_t *tidesdb_objstore_s3_create(const char *endpoint, const char *bucket,
                                               const char *prefix, const char *access_key,
                                               const char *secret_key, const char *region,
                                               int use_ssl, int use_path_style);

/**
 * tidesdb_objstore_s3_config_t
 * full configuration for an S3 connector, including TLS and multipart tuning that the
 * positional tidesdb_objstore_s3_create cannot express. zero-initialize and set the fields
 * you need the all-zero defaults are secure (TLS verify on, no custom CA) and use the
 * built-in multipart sizes.
 * @param endpoint S3 endpoint (required)
 * @param bucket bucket name (required)
 * @param prefix key prefix, or NULL
 * @param access_key AWS access key ID (required)
 * @param secret_key AWS secret access key (required)
 * @param region AWS region, or NULL for the default
 * @param use_ssl 1 for HTTPS, 0 for HTTP
 * @param use_path_style 1 for path-style URLs (MinIO), 0 for virtual-hosted (AWS)
 * @param tls_ca_path custom CA bundle file path, or NULL for the system bundle
 * @param tls_insecure_skip_verify 1 disables TLS peer+host verification (test endpoints
 *                                 ONLY -- insecure); 0 keeps verification on (default)
 * @param multipart_threshold object size at/above which multipart upload is used; 0 = default
 * @param multipart_part_size multipart chunk size in bytes; 0 = default
 */
typedef struct
{
    const char *endpoint;
    const char *bucket;
    const char *prefix;
    const char *access_key;
    const char *secret_key;
    const char *region;
    int use_ssl;
    int use_path_style;
    const char *tls_ca_path;
    int tls_insecure_skip_verify;
    size_t multipart_threshold;
    size_t multipart_part_size;
} tidesdb_objstore_s3_config_t;

/**
 * tidesdb_objstore_s3_create_config
 * create an S3-compatible connector from a full configuration struct (TLS + multipart).
 * tidesdb_objstore_s3_create is a thin wrapper over this with secure/default settings.
 * @param config connector configuration (fields are copied; need not outlive the call)
 * @return connector handle, or NULL on error
 */
tidesdb_objstore_t *tidesdb_objstore_s3_create_config(const tidesdb_objstore_s3_config_t *config);

#endif /* __OBJSTORE_S3_H__ */
