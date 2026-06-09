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

#ifdef TIDESDB_WITH_S3

#include "objstore_s3.h"

#include <ctype.h>
#include <curl/curl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "hmac_sha256.h"
#include "sha256.h"

/* path and buffer size constants */
#define TDB_S3_MAX_PATH      8192
#define TDB_S3_MAX_HEADER    2048
#define TDB_S3_DATE_LEN      9  /* YYYYMMDD + NUL */
#define TDB_S3_TIMESTAMP_LEN 17 /* YYYYMMDDTHHMMSSZ + NUL */
#define TDB_S3_HASH_HEX_LEN  65 /* SHA256 hex + NUL */
#define TDB_S3_SHA256_DIGEST 32 /* SHA256 raw digest bytes */
#define TDB_S3_DIR_MODE      0755

/* context struct buffer sizes */
#define TDB_S3_ENDPOINT_MAX 512
#define TDB_S3_BUCKET_MAX   256
#define TDB_S3_PREFIX_MAX   512
#define TDB_S3_KEY_MAX      128
#define TDB_S3_REGION_MAX   64

/* HTTP status codes */
#define TDB_S3_HTTP_OK        200
#define TDB_S3_HTTP_PARTIAL   206
#define TDB_S3_HTTP_REDIRECT  300
#define TDB_S3_HTTP_NOT_FOUND 404

/* signing and response buffers. host and key_date buffers must be large
 * enough for concatenated bucket+endpoint or "AWS4"+secret_key strings. */
#define TDB_S3_SCOPE_BUF      128
#define TDB_S3_STS_BUF        512
#define TDB_S3_HOST_BUF       1024
#define TDB_S3_RESPONSE_INIT  4096
#define TDB_S3_CONT_TOKEN_MAX 1024
#define TDB_S3_XML_TAG_BUF    128
#define TDB_S3_SIZE_BUF       32
#define TDB_S3_KEY_DATE_BUF   256

/* default region when none specified */
#define TDB_S3_DEFAULT_REGION "us-east-1"

/* network timeouts -- bound a hung connection so a dead or unreachable
 * endpoint cannot block an upload worker, or a wal_sync_on_commit commit,
 * forever. a hard total timeout is avoided so a legitimately slow large
 * upload is not cut off; instead a stalled-transfer detector is used. */
#define TDB_S3_CONNECT_TIMEOUT_S 15
#define TDB_S3_LOW_SPEED_LIMIT   1  /* bytes per second */
#define TDB_S3_LOW_SPEED_TIME_S  60 /* abort a transfer stalled below the limit this long */

/* multipart upload -- objects at or above the threshold are uploaded in
 * parts so the connector never buffers a whole large file in memory and is
 * not bound by S3's 5 GiB single-PUT limit. S3 requires parts of at least
 * 5 MiB (the final part may be smaller) and at most 10000 parts.
 * these match the documented objstore_config defaults (threshold 64 MiB,
 * part size 8 MiB); honoring per-config overrides at runtime additionally
 * requires plumbing multipart_threshold / multipart_part_size through the
 * public tidesdb_objstore_s3_create signature (deferred -- API change). */
#define TDB_S3_MULTIPART_THRESHOLD ((size_t)64 * 1024 * 1024)
#define TDB_S3_MULTIPART_PART_SIZE ((size_t)8 * 1024 * 1024)
#define TDB_S3_MAX_PARTS           10000
#define TDB_S3_ETAG_MAX            128
#define TDB_S3_UPLOAD_ID_MAX       512

/**
 * s3_uri_encode
 * URI-encode a string per the SigV4 spec. encodes all bytes except unreserved
 * characters (A-Z, a-z, 0-9, '-', '.', '_', '~'). forward slashes are encoded
 * as %2F since this is used for query parameter values, not object key paths.
 * @param src input string
 * @param dst output buffer
 * @param dst_size size of output buffer
 */
static void s3_uri_encode(const char *src, char *dst, size_t dst_size)
{
    static const char *unreserved =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    size_t pos = 0;
    for (; *src && pos + 3 < dst_size; src++)
    {
        if (strchr(unreserved, *src))
        {
            dst[pos++] = *src;
        }
        else
        {
            snprintf(dst + pos, dst_size - pos, "%%%02X", (unsigned char)*src);
            pos += 3;
        }
    }
    dst[pos] = '\0';
}

/**
 * s3_uri_encode_path
 * URI-encode an object key for use as a request path / SigV4 canonical URI. like
 * s3_uri_encode but leaves '/' unencoded so path segments are preserved. the request
 * URL (s3_build_url) and the canonical URI (s3_sign_request) MUST apply the exact same
 * encoding or the SigV4 signature will not match the request. for keys made only of
 * unreserved characters and '/' (which is what tidesdb cf/sstable keys are) this is a
 * passthrough, so normal operation is unchanged; it only matters for keys containing
 * spaces, '+', '?', '#', '&', etc.
 * @param src input key
 * @param dst output buffer
 * @param dst_size size of output buffer
 */
static void s3_uri_encode_path(const char *src, char *dst, size_t dst_size)
{
    static const char *unreserved =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~/";
    size_t pos = 0;
    for (; *src && pos + 3 < dst_size; src++)
    {
        if (strchr(unreserved, *src))
        {
            dst[pos++] = *src;
        }
        else
        {
            snprintf(dst + pos, dst_size - pos, "%%%02X", (unsigned char)*src);
            pos += 3;
        }
    }
    dst[pos] = '\0';
}

/**
 * s3_ctx_t
 * internal context for the S3 connector, credentials, endpoint, TLS, and multipart config.
 * defined before s3_curl_new so that helper can apply the per-connector TLS options.
 * @param endpoint S3 endpoint hostname
 * @param bucket S3 bucket name
 * @param prefix key prefix prepended to all object keys
 * @param access_key AWS access key ID
 * @param secret_key AWS secret access key
 * @param region AWS region string
 * @param use_ssl 1 for HTTPS, 0 for HTTP
 * @param use_path_style 1 for path-style URLs, 0 for virtual-hosted
 * @param tls_ca_path custom CA bundle file path (empty = libcurl default bundle)
 * @param tls_insecure_skip_verify 1 disables peer+host verification (test endpoints only)
 * @param multipart_threshold object size at/above which multipart upload is used
 * @param multipart_part_size multipart chunk size
 */
typedef struct
{
    char endpoint[TDB_S3_ENDPOINT_MAX];
    char bucket[TDB_S3_BUCKET_MAX];
    char prefix[TDB_S3_PREFIX_MAX];
    char access_key[TDB_S3_KEY_MAX];
    char secret_key[TDB_S3_KEY_MAX];
    char region[TDB_S3_REGION_MAX];
    int use_ssl;
    int use_path_style;
    char tls_ca_path[TDB_S3_MAX_PATH];
    int tls_insecure_skip_verify;
    size_t multipart_threshold;
    size_t multipart_part_size;
} s3_ctx_t;

/**
 * s3_curl_new
 * create a curl easy handle with the connector's common options applied -- a connection
 * timeout and a stalled-transfer timeout so a dead endpoint cannot hang a worker, NOSIGNAL
 * for safe use from multiple threads, and (over https) the connector's TLS settings, a custom
 * CA bundle when configured, and an opt-in insecure skip-verify for test endpoints.
 * @param s3 connector context (for TLS settings)
 * @return a configured handle, or NULL on allocation failure
 */
static CURL *s3_curl_new(const s3_ctx_t *s3)
{
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, (long)TDB_S3_CONNECT_TIMEOUT_S);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, (long)TDB_S3_LOW_SPEED_LIMIT);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, (long)TDB_S3_LOW_SPEED_TIME_S);

    /* TLS only matters over https. leaving both branches untouched keeps libcurl's secure
     * defaults (verify peer + host against the system CA bundle). */
    if (s3 && s3->use_ssl)
    {
        if (s3->tls_ca_path[0]) curl_easy_setopt(curl, CURLOPT_CAINFO, s3->tls_ca_path);
        if (s3->tls_insecure_skip_verify)
        {
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        }
    }
    return curl;
}

/* SHA-256 (sha256_hex) and HMAC-SHA-256 (hmac_sha256) are provided by the bundled crypto modules
 * (src/sha256.c, src/hmac_sha256.c) -- only curl for HTTP. */

/**
 * s3_get_timestamp
 * get current UTC time in AWS SigV4 date and timestamp formats
 * @param date8 output YYYYMMDD (TDB_S3_DATE_LEN bytes)
 * @param timestamp16 output YYYYMMDDTHHMMSSZ (TDB_S3_TIMESTAMP_LEN bytes)
 */
static void s3_get_timestamp(char *date8, char *timestamp16)
{
    time_t now = time(NULL);
    struct tm gm;
    tdb_gmtime_r(&now, &gm);
    strftime(date8, TDB_S3_DATE_LEN, "%Y%m%d", &gm);
    strftime(timestamp16, TDB_S3_TIMESTAMP_LEN, "%Y%m%dT%H%M%SZ", &gm);
}

/**
 * s3_signing_key
 * derive the SigV4 signing key via HMAC chain date -> region -> service -> request
 * @param secret_key AWS secret access key
 * @param date8 date string YYYYMMDD
 * @param region AWS region
 * @param out output signing key (TDB_S3_SHA256_DIGEST bytes)
 */
static void s3_signing_key(const char *secret_key, const char *date8, const char *region,
                           unsigned char *out)
{
    char key_date[TDB_S3_KEY_DATE_BUF];
    snprintf(key_date, sizeof(key_date), "AWS4%s", secret_key);

    /* each HMAC-SHA-256 step emits exactly TDB_S3_SHA256_DIGEST bytes, which is the key for the
     * next step: date -> region -> service -> aws4_request */
    unsigned char k1[TDB_S3_SHA256_DIGEST], k2[TDB_S3_SHA256_DIGEST], k3[TDB_S3_SHA256_DIGEST];
    hmac_sha256(key_date, strlen(key_date), date8, strlen(date8), k1);
    hmac_sha256(k1, TDB_S3_SHA256_DIGEST, region, strlen(region), k2);
    hmac_sha256(k2, TDB_S3_SHA256_DIGEST, "s3", 2, k3);
    hmac_sha256(k3, TDB_S3_SHA256_DIGEST, "aws4_request", 12, out);
}

/**
 * s3_build_url
 * construct the full URL for an S3 object request
 * @param ctx S3 connector context
 * @param key object key
 * @param url output URL buffer
 * @param url_size size of the URL buffer
 */
#ifndef _MSC_VER
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif
static void s3_build_url(const s3_ctx_t *ctx, const char *key, char *url, size_t url_size)
{
    const char *scheme = ctx->use_ssl ? "https" : "http";
    char full_key[TDB_S3_MAX_PATH];
    if (ctx->prefix[0])
        snprintf(full_key, sizeof(full_key), "%s%s", ctx->prefix, key);
    else
        snprintf(full_key, sizeof(full_key), "%s", key);

    /* must match the canonical-URI encoding in s3_sign_request exactly */
    char enc_key[TDB_S3_MAX_PATH * 3];
    s3_uri_encode_path(full_key, enc_key, sizeof(enc_key));

    if (ctx->use_path_style)
        snprintf(url, url_size, "%s://%s/%s/%s", scheme, ctx->endpoint, ctx->bucket, enc_key);
    else
        snprintf(url, url_size, "%s://%s.%s/%s", scheme, ctx->bucket, ctx->endpoint, enc_key);
}
#ifndef _MSC_VER
#pragma GCC diagnostic pop
#endif

/**
 * s3_build_host
 * construct the Host header value for S3 requests
 * @param ctx S3 connector context
 * @param host output host string
 * @param host_size size of host buffer
 */
static void s3_build_host(const s3_ctx_t *ctx, char *host, size_t host_size)
{
    if (ctx->use_path_style)
        snprintf(host, host_size, "%s", ctx->endpoint);
    else
        snprintf(host, host_size, "%s.%s", ctx->bucket, ctx->endpoint);
}

#ifndef _MSC_VER
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif
/**
 * s3_sign_raw
 * create AWS SigV4 signed HTTP headers given explicit canonical URI and query string.
 * this is the low-level signing function used by both object operations and list requests.
 * @param ctx S3 connector context
 * @param method HTTP method (GET, PUT, DELETE, HEAD)
 * @param canonical_uri the URI path component of the request (e.g. "/bucket/key")
 * @param canonical_query_string the query string component (alphabetically sorted, or "")
 * @param content_sha256 hex-encoded SHA256 of the request body
 * @param extra_headers_canonical additional canonical headers (or NULL)
 * @param extra_signed_headers additional signed header names (or NULL)
 * @return curl_slist of signed headers (caller must free with curl_slist_free_all)
 */
static struct curl_slist *s3_sign_raw(const s3_ctx_t *ctx, const char *method,
                                      const char *canonical_uri, const char *canonical_query_string,
                                      const char *content_sha256,
                                      const char *extra_headers_canonical,
                                      const char *extra_signed_headers)
{
    char date8[TDB_S3_DATE_LEN], timestamp[TDB_S3_TIMESTAMP_LEN];
    s3_get_timestamp(date8, timestamp);

    char host[TDB_S3_HOST_BUF];
    s3_build_host(ctx, host, sizeof(host));

    /* canonical request */
    char canonical_request[TDB_S3_MAX_PATH * 4];
    snprintf(canonical_request, sizeof(canonical_request),
             "%s\n%s\n%s\nhost:%s\nx-amz-content-sha256:%s\nx-amz-date:%s\n%s\n"
             "host;x-amz-content-sha256;x-amz-date%s\n%s",
             method, canonical_uri, canonical_query_string ? canonical_query_string : "", host,
             content_sha256, timestamp, extra_headers_canonical ? extra_headers_canonical : "",
             extra_signed_headers ? extra_signed_headers : "", content_sha256);

    char canonical_hash[TDB_S3_HASH_HEX_LEN];
    sha256_hex(canonical_request, strlen(canonical_request), canonical_hash);

    /* string to sign */
    char scope[TDB_S3_SCOPE_BUF];
    snprintf(scope, sizeof(scope), "%s/%s/s3/aws4_request", date8, ctx->region);

    char string_to_sign[TDB_S3_STS_BUF];
    snprintf(string_to_sign, sizeof(string_to_sign), "AWS4-HMAC-SHA256\n%s\n%s\n%s", timestamp,
             scope, canonical_hash);

    /* signature */
    unsigned char signing_key[TDB_S3_SHA256_DIGEST];
    s3_signing_key(ctx->secret_key, date8, ctx->region, signing_key);

    unsigned char sig_raw[TDB_S3_SHA256_DIGEST];
    hmac_sha256(signing_key, TDB_S3_SHA256_DIGEST, string_to_sign, strlen(string_to_sign), sig_raw);

    char sig_hex[TDB_S3_HASH_HEX_LEN];
    for (unsigned int i = 0; i < TDB_S3_SHA256_DIGEST; i++)
        sprintf(sig_hex + i * 2, "%02x", sig_raw[i]);
    sig_hex[TDB_S3_SHA256_DIGEST * 2] = '\0';

    char auth_header[TDB_S3_MAX_HEADER];
    snprintf(auth_header, sizeof(auth_header),
             "Authorization: AWS4-HMAC-SHA256 Credential=%s/%s, "
             "SignedHeaders=host;x-amz-content-sha256;x-amz-date%s, Signature=%s",
             ctx->access_key, scope, extra_signed_headers ? extra_signed_headers : "", sig_hex);

    /* we build curl headers */
    struct curl_slist *headers = NULL;
    char hdr[TDB_S3_MAX_HEADER];

    snprintf(hdr, sizeof(hdr), "Host: %s", host);
    headers = curl_slist_append(headers, hdr);

    snprintf(hdr, sizeof(hdr), "x-amz-date: %s", timestamp);
    headers = curl_slist_append(headers, hdr);

    snprintf(hdr, sizeof(hdr), "x-amz-content-sha256: %s", content_sha256);
    headers = curl_slist_append(headers, hdr);

    headers = curl_slist_append(headers, auth_header);

    return headers;
}
#ifndef _MSC_VER
#pragma GCC diagnostic pop
#endif

/**
 * s3_sign_request
 * create AWS SigV4 signed HTTP headers for an S3 object request.
 * computes the canonical URI from the key and connector prefix,
 * then delegates to s3_sign_raw with an empty query string.
 * @param ctx S3 connector context
 * @param method HTTP method (GET, PUT, DELETE, HEAD)
 * @param key object key
 * @param content_sha256 hex-encoded SHA256 of the request body
 * @param extra_headers_canonical additional canonical headers (or NULL)
 * @param extra_signed_headers additional signed header names (or NULL)
 * @return curl_slist of signed headers (caller must free with curl_slist_free_all)
 */
#ifndef _MSC_VER
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif
static struct curl_slist *s3_sign_request(const s3_ctx_t *ctx, const char *method, const char *key,
                                          const char *content_sha256,
                                          const char *extra_headers_canonical,
                                          const char *extra_signed_headers)
{
    char full_key[TDB_S3_MAX_PATH];
    if (ctx->prefix[0])
        snprintf(full_key, sizeof(full_key), "%s%s", ctx->prefix, key);
    else
        snprintf(full_key, sizeof(full_key), "%s", key);

    /* URI-encode the key path exactly as s3_build_url does, or the signature will not
     * match the request for keys containing characters outside [A-Za-z0-9-._~/] */
    char enc_key[TDB_S3_MAX_PATH * 3];
    s3_uri_encode_path(full_key, enc_key, sizeof(enc_key));

    char canonical_uri[TDB_S3_MAX_PATH * 3 + 256];
    if (ctx->use_path_style)
        snprintf(canonical_uri, sizeof(canonical_uri), "/%s/%s", ctx->bucket, enc_key);
    else
        snprintf(canonical_uri, sizeof(canonical_uri), "/%s", enc_key);

    return s3_sign_raw(ctx, method, canonical_uri, "", content_sha256, extra_headers_canonical,
                       extra_signed_headers);
}
#ifndef _MSC_VER
#pragma GCC diagnostic pop
#endif

/**
 * s3_write_ctx_t
 * context for curl write callbacks, supports writing to file or buffer
 * @param fp file pointer for file-based writes (NULL if writing to buffer)
 * @param buf buffer pointer for in-memory writes (NULL if writing to file)
 * @param buf_size total size of the output buffer
 * @param written number of bytes written so far
 */
typedef struct
{
    FILE *fp;
    char *buf;
    size_t buf_size;
    size_t written;
} s3_write_ctx_t;

/**
 * s3_write_to_file
 * curl write callback that writes received data to a file
 * @param ptr pointer to received data
 * @param size size of each element
 * @param nmemb number of elements
 * @param userdata pointer to s3_write_ctx_t with fp set
 * @return number of bytes written
 */
static size_t s3_write_to_file(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    s3_write_ctx_t *wctx = (s3_write_ctx_t *)userdata;
    return fwrite(ptr, size, nmemb, wctx->fp);
}

/**
 * s3_write_to_buf
 * curl write callback that copies received data into a fixed-size buffer
 * @param ptr pointer to received data
 * @param size size of each element
 * @param nmemb number of elements
 * @param userdata pointer to s3_write_ctx_t with buf and buf_size set
 * @return number of bytes consumed (always size * nmemb to avoid curl error)
 */
static size_t s3_write_to_buf(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    s3_write_ctx_t *wctx = (s3_write_ctx_t *)userdata;
    size_t bytes = size * nmemb;
    size_t avail = wctx->buf_size - wctx->written;
    size_t to_copy = bytes < avail ? bytes : avail;
    memcpy(wctx->buf + wctx->written, ptr, to_copy);
    wctx->written += to_copy;
    return bytes; /* always consume all data to avoid curl error */
}

/**
 * s3_write_discard
 * curl write callback that discards all received data
 * @param ptr pointer to received data (unused)
 * @param size size of each element
 * @param nmemb number of elements
 * @param userdata unused
 * @return number of bytes consumed (always size * nmemb)
 */
static size_t s3_write_discard(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    (void)ptr;
    (void)userdata;
    return size * nmemb;
}

/**
 * s3_get
 * download an S3 object to a local file, creating parent directories as needed
 * @param ctx opaque S3 connector context
 * @param key object key
 * @param local_path path to write the downloaded file
 * @return 0 on success, -1 on error (including not found)
 */
static int s3_get(void *ctx, const char *key, const char *local_path)
{
    s3_ctx_t *s3 = (s3_ctx_t *)ctx;

    char empty_sha[TDB_S3_HASH_HEX_LEN];
    sha256_hex("", 0, empty_sha);

    struct curl_slist *headers = s3_sign_request(s3, "GET", key, empty_sha, NULL, NULL);

    char url[TDB_S3_MAX_PATH];
    s3_build_url(s3, key, url, sizeof(url));

    /* we create parent directories for local_path */
    char dir_buf[TDB_S3_MAX_PATH];
    snprintf(dir_buf, sizeof(dir_buf), "%s", local_path);
    char *sep = strrchr(dir_buf, '/');
    if (sep)
    {
        *sep = '\0';

        for (char *p = dir_buf + 1; *p; p++)
        {
            if (*p == '/')
            {
                *p = '\0';
                mkdir(dir_buf, TDB_S3_DIR_MODE);
                *p = '/';
            }
        }
        mkdir(dir_buf, 0755);
    }

    FILE *fp = fopen(local_path, "wb");
    if (!fp)
    {
        curl_slist_free_all(headers);
        return -1;
    }

    s3_write_ctx_t wctx = {.fp = fp};

    CURL *curl = s3_curl_new(s3);
    if (!curl)
    {
        fclose(fp);
        unlink(local_path);
        curl_slist_free_all(headers);
        return -1;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, s3_write_to_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &wctx);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    fclose(fp);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code < TDB_S3_HTTP_OK || http_code >= TDB_S3_HTTP_REDIRECT)
    {
        unlink(local_path);
        return -1;
    }
    return 0;
}

/**
 * s3_range_get
 * download a byte range of an S3 object into a caller-allocated buffer
 * @param ctx opaque S3 connector context
 * @param key object key
 * @param offset byte offset to start reading
 * @param buf output buffer (caller allocated)
 * @param size number of bytes to read
 * @return bytes read on success, -1 on error
 */
static ssize_t s3_range_get(void *ctx, const char *key, uint64_t offset, void *buf, size_t size)
{
    s3_ctx_t *s3 = (s3_ctx_t *)ctx;

    char empty_sha[TDB_S3_HASH_HEX_LEN];
    sha256_hex("", 0, empty_sha);

    /* we sign without Range header -- S3/MinIO does not require Range to be signed */
    struct curl_slist *headers = s3_sign_request(s3, "GET", key, empty_sha, NULL, NULL);

    char range_hdr[128];
    snprintf(range_hdr, sizeof(range_hdr), "Range: bytes=%" PRIu64 "-%" PRIu64, offset,
             offset + size - 1);
    headers = curl_slist_append(headers, range_hdr);

    char url[TDB_S3_MAX_PATH];
    s3_build_url(s3, key, url, sizeof(url));

    s3_write_ctx_t wctx = {.buf = (char *)buf, .buf_size = size, .written = 0};

    CURL *curl = s3_curl_new(s3);
    if (!curl)
    {
        curl_slist_free_all(headers);
        return -1;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, s3_write_to_buf);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &wctx);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || (http_code != TDB_S3_HTTP_OK && http_code != TDB_S3_HTTP_PARTIAL))
        return -1;
    return (ssize_t)wctx.written;
}

/**
 * s3_delete_object
 * delete an object from S3. not-found is not an error.
 * @param ctx opaque S3 connector context
 * @param key object key to delete
 * @return 0 on success, -1 on error
 */
static int s3_delete_object(void *ctx, const char *key)
{
    s3_ctx_t *s3 = (s3_ctx_t *)ctx;

    char empty_sha[TDB_S3_HASH_HEX_LEN];
    sha256_hex("", 0, empty_sha);

    struct curl_slist *headers = s3_sign_request(s3, "DELETE", key, empty_sha, NULL, NULL);

    char url[TDB_S3_MAX_PATH];
    s3_build_url(s3, key, url, sizeof(url));

    CURL *curl = s3_curl_new(s3);
    if (!curl)
    {
        curl_slist_free_all(headers);
        return -1;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, s3_write_discard);

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) return -1;
    /* 2xx (200/204 No Content) = deleted, 404 Not Found = already absent; both are success.
     * any other status (403, 5xx, ...) is a real failure that must NOT be masked, or the
     * integration layer's retry/cleanup is silently defeated. */
    if ((http_code >= TDB_S3_HTTP_OK && http_code < TDB_S3_HTTP_REDIRECT) ||
        http_code == TDB_S3_HTTP_NOT_FOUND)
        return 0;
    return -1;
}

/**
 * s3_exists
 * check if an S3 object exists and optionally return its size via HEAD request
 * @param ctx opaque S3 connector context
 * @param key object key
 * @param size_out if non-NULL, receives the object size in bytes
 * @return 1 if exists, 0 if not, -1 on error
 */
static int s3_exists(void *ctx, const char *key, size_t *size_out)
{
    s3_ctx_t *s3 = (s3_ctx_t *)ctx;

    char empty_sha[TDB_S3_HASH_HEX_LEN];
    sha256_hex("", 0, empty_sha);

    struct curl_slist *headers = s3_sign_request(s3, "HEAD", key, empty_sha, NULL, NULL);

    char url[TDB_S3_MAX_PATH];
    s3_build_url(s3, key, url, sizeof(url));

    CURL *curl = s3_curl_new(s3);
    if (!curl)
    {
        curl_slist_free_all(headers);
        return -1;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, s3_write_discard);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (size_out && res == CURLE_OK && http_code == TDB_S3_HTTP_OK)
    {
        curl_off_t cl = 0;
        curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
        *size_out = (size_t)cl;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) return -1;
    return (http_code == TDB_S3_HTTP_OK) ? 1 : 0;
}

/**
 * xml_find_tag
 * simple XML tag extraction for ListObjectsV2 response parsing
 * @param xml XML string to search in
 * @param tag tag name to find (without angle brackets)
 * @param value_len receives the length of the tag's text content
 * @return pointer to the start of the tag value, or NULL if not found
 */
static const char *xml_find_tag(const char *xml, const char *tag, size_t *value_len)
{
    char open_tag[TDB_S3_XML_TAG_BUF];
    snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
    const char *start = strstr(xml, open_tag);
    if (!start) return NULL;
    start += strlen(open_tag);

    char close_tag[TDB_S3_XML_TAG_BUF];
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);
    const char *end = strstr(start, close_tag);
    if (!end) return NULL;

    *value_len = end - start;
    return start;
}

/**
 * s3_response_buf_t
 * growable buffer for accumulating HTTP response data
 * @param data heap-allocated buffer holding response bytes
 * @param size number of bytes currently stored
 * @param capacity total allocated capacity of data buffer
 */
typedef struct
{
    char *data;
    size_t size;
    size_t capacity;
} s3_response_buf_t;

/**
 * s3_write_to_response
 * curl write callback that appends received data to a growable response buffer
 * @param ptr pointer to received data
 * @param size size of each element
 * @param nmemb number of elements
 * @param userdata pointer to s3_response_buf_t
 * @return number of bytes consumed, or 0 on allocation failure
 */
static size_t s3_write_to_response(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    s3_response_buf_t *buf = (s3_response_buf_t *)userdata;
    size_t bytes = size * nmemb;
    if (buf->size + bytes >= buf->capacity)
    {
        size_t new_cap = (buf->capacity + bytes) * 2;
        char *new_data = realloc(buf->data, new_cap);
        if (!new_data) return 0;
        buf->data = new_data;
        buf->capacity = new_cap;
    }
    memcpy(buf->data + buf->size, ptr, bytes);
    buf->size += bytes;
    buf->data[buf->size] = '\0';
    return bytes;
}

#ifndef _MSC_VER
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

/**
 * s3_full_key
 * build the connector-prefixed object key (prefix + key).
 * @param s3 S3 connector context
 * @param key caller object key
 * @param out output buffer
 * @param out_size size of the output buffer
 */
static void s3_full_key(const s3_ctx_t *s3, const char *key, char *out, size_t out_size)
{
    if (s3->prefix[0])
        snprintf(out, out_size, "%s%s", s3->prefix, key);
    else
        snprintf(out, out_size, "%s", key);
}

/**
 * s3_canonical_uri
 * build the SigV4 canonical URI for a full object key -- "/bucket/key" for
 * path-style addressing, "/key" for virtual-hosted style.
 * @param s3 S3 connector context
 * @param full_key prefixed object key
 * @param out output buffer
 * @param out_size size of the output buffer
 */
static void s3_canonical_uri(const s3_ctx_t *s3, const char *full_key, char *out, size_t out_size)
{
    if (s3->use_path_style)
        snprintf(out, out_size, "/%s/%s", s3->bucket, full_key);
    else
        snprintf(out, out_size, "/%s", full_key);
}

/**
 * s3_header_ctx_t
 * context for the multipart ETag response-header capture callback.
 * @param etag receives the part ETag value (quotes included, as returned)
 * @param found set to 1 once an ETag header has been captured
 */
typedef struct
{
    char etag[TDB_S3_ETAG_MAX];
    int found;
} s3_header_ctx_t;

/**
 * s3_capture_etag_header
 * curl header callback that captures the ETag response header of an
 * UploadPart request. header field names are case-insensitive per RFC 7230.
 * @param buffer header line bytes (not NUL terminated)
 * @param size size of each element
 * @param nitems number of elements
 * @param userdata pointer to s3_header_ctx_t
 * @return number of bytes consumed (must equal size * nitems)
 */
static size_t s3_capture_etag_header(char *buffer, size_t size, size_t nitems, void *userdata)
{
    s3_header_ctx_t *h = (s3_header_ctx_t *)userdata;
    size_t len = size * nitems;
    if (len >= 5)
    {
        char name[6];
        for (int i = 0; i < 5; i++) name[i] = (char)tolower((unsigned char)buffer[i]);
        name[5] = '\0';
        if (strcmp(name, "etag:") == 0)
        {
            const char *v = buffer + 5;
            size_t vlen = len - 5;
            while (vlen > 0 && (*v == ' ' || *v == '\t'))
            {
                v++;
                vlen--;
            }
            while (vlen > 0 && (v[vlen - 1] == '\r' || v[vlen - 1] == '\n' || v[vlen - 1] == ' '))
                vlen--;
            if (vlen >= sizeof(h->etag)) vlen = sizeof(h->etag) - 1;
            memcpy(h->etag, v, vlen);
            h->etag[vlen] = '\0';
            h->found = 1;
        }
    }
    return len;
}

/**
 * s3_multipart_create
 * issue CreateMultipartUpload (POST <object>?uploads) and parse the upload
 * id out of the XML response.
 * @param s3 S3 connector context
 * @param key object key
 * @param upload_id_out receives the upload id
 * @param upload_id_size size of the upload id buffer
 * @return 0 on success, -1 on error
 */
static int s3_multipart_create(s3_ctx_t *s3, const char *key, char *upload_id_out,
                               size_t upload_id_size)
{
    char empty_sha[TDB_S3_HASH_HEX_LEN];
    sha256_hex("", 0, empty_sha);

    char full_key[TDB_S3_MAX_PATH];
    s3_full_key(s3, key, full_key, sizeof(full_key));
    char canonical_uri[TDB_S3_MAX_PATH + 512];
    s3_canonical_uri(s3, full_key, canonical_uri, sizeof(canonical_uri));

    struct curl_slist *headers =
        s3_sign_raw(s3, "POST", canonical_uri, "uploads=", empty_sha, NULL, NULL);

    char url[TDB_S3_MAX_PATH];
    s3_build_url(s3, key, url, sizeof(url));
    char full_url[TDB_S3_MAX_PATH + 16];
    snprintf(full_url, sizeof(full_url), "%s?uploads", url);

    s3_response_buf_t resp = {
        .data = malloc(TDB_S3_RESPONSE_INIT), .size = 0, .capacity = TDB_S3_RESPONSE_INIT};
    if (!resp.data)
    {
        curl_slist_free_all(headers);
        return -1;
    }

    CURL *curl = s3_curl_new(s3);
    if (!curl)
    {
        free(resp.data);
        curl_slist_free_all(headers);
        return -1;
    }
    curl_easy_setopt(curl, CURLOPT_URL, full_url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, s3_write_to_response);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    int rc = -1;
    if (res == CURLE_OK && http_code == TDB_S3_HTTP_OK)
    {
        size_t id_len = 0;
        const char *id = xml_find_tag(resp.data, "UploadId", &id_len);
        if (id && id_len > 0 && id_len < upload_id_size)
        {
            memcpy(upload_id_out, id, id_len);
            upload_id_out[id_len] = '\0';
            rc = 0;
        }
    }
    free(resp.data);
    return rc;
}

/**
 * s3_upload_part
 * upload one part of a multipart upload (PUT <object>?partNumber=N&uploadId=I)
 * and capture the part ETag from the response. the part body is small enough
 * to hash, so each part keeps end-to-end integrity via x-amz-content-sha256.
 * @param s3 S3 connector context
 * @param key object key
 * @param upload_id multipart upload id
 * @param part_number 1-based part number
 * @param part_data part bytes
 * @param part_len number of part bytes
 * @param etag_out receives the part ETag
 * @param etag_size size of the ETag buffer
 * @return 0 on success, -1 on error
 */
static int s3_upload_part(s3_ctx_t *s3, const char *key, const char *upload_id, int part_number,
                          const void *part_data, size_t part_len, char *etag_out, size_t etag_size)
{
    char part_sha[TDB_S3_HASH_HEX_LEN];
    sha256_hex(part_data, part_len, part_sha);

    char enc_id[TDB_S3_UPLOAD_ID_MAX * 4];
    s3_uri_encode(upload_id, enc_id, sizeof(enc_id));

    char canonical_qs[TDB_S3_UPLOAD_ID_MAX * 4 + 64];
    snprintf(canonical_qs, sizeof(canonical_qs), "partNumber=%d&uploadId=%s", part_number, enc_id);

    char full_key[TDB_S3_MAX_PATH];
    s3_full_key(s3, key, full_key, sizeof(full_key));
    char canonical_uri[TDB_S3_MAX_PATH + 512];
    s3_canonical_uri(s3, full_key, canonical_uri, sizeof(canonical_uri));

    struct curl_slist *headers =
        s3_sign_raw(s3, "PUT", canonical_uri, canonical_qs, part_sha, NULL, NULL);

    char url[TDB_S3_MAX_PATH];
    s3_build_url(s3, key, url, sizeof(url));
    char full_url[TDB_S3_MAX_PATH + TDB_S3_UPLOAD_ID_MAX * 4 + 64];
    snprintf(full_url, sizeof(full_url), "%s?partNumber=%d&uploadId=%s", url, part_number, enc_id);

    FILE *mem_fp = tdb_fmemopen((void *)part_data, part_len, "rb");
    if (!mem_fp)
    {
        curl_slist_free_all(headers);
        return -1;
    }

    s3_header_ctx_t hctx = {.found = 0};
    CURL *curl = s3_curl_new(s3);
    if (!curl)
    {
        fclose(mem_fp);
        curl_slist_free_all(headers);
        return -1;
    }
    curl_easy_setopt(curl, CURLOPT_URL, full_url);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)part_len);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_READDATA, mem_fp);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, s3_write_discard);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, s3_capture_etag_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &hctx);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    fclose(mem_fp);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != TDB_S3_HTTP_OK || !hctx.found) return -1;
    if (strlen(hctx.etag) >= etag_size) return -1;
    snprintf(etag_out, etag_size, "%s", hctx.etag);
    return 0;
}

/**
 * s3_multipart_complete
 * issue CompleteMultipartUpload with the XML manifest of part numbers and
 * ETags. S3 can return HTTP 200 with an <Error> body on failure, so the
 * response payload is inspected, not only the status code.
 * @param s3 S3 connector context
 * @param key object key
 * @param upload_id multipart upload id
 * @param etags packed part ETags, TDB_S3_ETAG_MAX bytes per entry
 * @param part_count number of parts
 * @return 0 on success, -1 on error
 */
static int s3_multipart_complete(s3_ctx_t *s3, const char *key, const char *upload_id,
                                 const char *etags, int part_count)
{
    size_t body_cap = (size_t)part_count * (TDB_S3_ETAG_MAX + 64) + 64;
    char *body = malloc(body_cap);
    if (!body) return -1;

    size_t off = 0;
    off += (size_t)snprintf(body + off, body_cap - off, "<CompleteMultipartUpload>");
    for (int i = 0; i < part_count; i++)
    {
        off += (size_t)snprintf(body + off, body_cap - off,
                                "<Part><PartNumber>%d</PartNumber><ETag>%s</ETag></Part>", i + 1,
                                etags + (size_t)i * TDB_S3_ETAG_MAX);
    }
    off += (size_t)snprintf(body + off, body_cap - off, "</CompleteMultipartUpload>");

    char body_sha[TDB_S3_HASH_HEX_LEN];
    sha256_hex(body, off, body_sha);

    char enc_id[TDB_S3_UPLOAD_ID_MAX * 4];
    s3_uri_encode(upload_id, enc_id, sizeof(enc_id));
    char canonical_qs[TDB_S3_UPLOAD_ID_MAX * 4 + 32];
    snprintf(canonical_qs, sizeof(canonical_qs), "uploadId=%s", enc_id);

    char full_key[TDB_S3_MAX_PATH];
    s3_full_key(s3, key, full_key, sizeof(full_key));
    char canonical_uri[TDB_S3_MAX_PATH + 512];
    s3_canonical_uri(s3, full_key, canonical_uri, sizeof(canonical_uri));

    struct curl_slist *headers =
        s3_sign_raw(s3, "POST", canonical_uri, canonical_qs, body_sha, NULL, NULL);

    char url[TDB_S3_MAX_PATH];
    s3_build_url(s3, key, url, sizeof(url));
    char full_url[TDB_S3_MAX_PATH + TDB_S3_UPLOAD_ID_MAX * 4 + 32];
    snprintf(full_url, sizeof(full_url), "%s?uploadId=%s", url, enc_id);

    s3_response_buf_t resp = {
        .data = malloc(TDB_S3_RESPONSE_INIT), .size = 0, .capacity = TDB_S3_RESPONSE_INIT};
    if (!resp.data)
    {
        free(body);
        curl_slist_free_all(headers);
        return -1;
    }

    CURL *curl = s3_curl_new(s3);
    if (!curl)
    {
        free(body);
        free(resp.data);
        curl_slist_free_all(headers);
        return -1;
    }
    curl_easy_setopt(curl, CURLOPT_URL, full_url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)off);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, s3_write_to_response);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body);

    /* CompleteMultipartUpload can return HTTP 200 with an <Error> body, so the
     * success result element must be present and no error element present */
    int rc = -1;
    if (res == CURLE_OK && http_code == TDB_S3_HTTP_OK && resp.data &&
        strstr(resp.data, "<CompleteMultipartUploadResult") != NULL &&
        strstr(resp.data, "<Error") == NULL)
    {
        rc = 0;
    }
    free(resp.data);
    return rc;
}

/**
 * s3_multipart_abort
 * issue AbortMultipartUpload to discard the parts of a failed multipart
 * upload so they do not linger and accrue storage cost.
 * @param s3 S3 connector context
 * @param key object key
 * @param upload_id multipart upload id
 * @return 0 on success, -1 on error
 */
static int s3_multipart_abort(s3_ctx_t *s3, const char *key, const char *upload_id)
{
    char empty_sha[TDB_S3_HASH_HEX_LEN];
    sha256_hex("", 0, empty_sha);

    char enc_id[TDB_S3_UPLOAD_ID_MAX * 4];
    s3_uri_encode(upload_id, enc_id, sizeof(enc_id));
    char canonical_qs[TDB_S3_UPLOAD_ID_MAX * 4 + 32];
    snprintf(canonical_qs, sizeof(canonical_qs), "uploadId=%s", enc_id);

    char full_key[TDB_S3_MAX_PATH];
    s3_full_key(s3, key, full_key, sizeof(full_key));
    char canonical_uri[TDB_S3_MAX_PATH + 512];
    s3_canonical_uri(s3, full_key, canonical_uri, sizeof(canonical_uri));

    struct curl_slist *headers =
        s3_sign_raw(s3, "DELETE", canonical_uri, canonical_qs, empty_sha, NULL, NULL);

    char url[TDB_S3_MAX_PATH];
    s3_build_url(s3, key, url, sizeof(url));
    char full_url[TDB_S3_MAX_PATH + TDB_S3_UPLOAD_ID_MAX * 4 + 32];
    snprintf(full_url, sizeof(full_url), "%s?uploadId=%s", url, enc_id);

    CURL *curl = s3_curl_new(s3);
    if (!curl)
    {
        curl_slist_free_all(headers);
        return -1;
    }
    curl_easy_setopt(curl, CURLOPT_URL, full_url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, s3_write_discard);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return (res == CURLE_OK) ? 0 : -1;
}

/**
 * s3_put_single
 * upload an object with a single streaming PUT. the body is read straight
 * from the open file by curl's default reader, and the request is signed
 * with x-amz-content-sha256 UNSIGNED-PAYLOAD so the connector never buffers
 * or hashes the whole file. transit integrity is covered by TLS and by the
 * upload pipeline's post-upload size verification.
 * @param s3 S3 connector context
 * @param key object key
 * @param fp open file positioned at offset 0
 * @param file_size size of the file in bytes
 * @return 0 on success, -1 on error
 */
static int s3_put_single(s3_ctx_t *s3, const char *key, FILE *fp, long file_size)
{
    struct curl_slist *headers = s3_sign_request(s3, "PUT", key, "UNSIGNED-PAYLOAD", NULL, NULL);

    char url[TDB_S3_MAX_PATH];
    s3_build_url(s3, key, url, sizeof(url));

    CURL *curl = s3_curl_new(s3);
    if (!curl)
    {
        curl_slist_free_all(headers);
        return -1;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)file_size);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, s3_write_discard);
    curl_easy_setopt(curl, CURLOPT_READDATA, fp);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return (res == CURLE_OK && http_code >= TDB_S3_HTTP_OK && http_code < TDB_S3_HTTP_REDIRECT)
               ? 0
               : -1;
}

/**
 * s3_put_multipart
 * upload a large object as a multipart upload -- create, stream fixed-size
 * parts from the file, then complete. on any failure the upload is aborted
 * so no orphaned parts remain. only one part is held in memory at a time, so
 * memory use is bounded regardless of file size.
 * @param s3 S3 connector context
 * @param key object key
 * @param fp open file positioned at offset 0
 * @param file_size size of the file in bytes
 * @return 0 on success, -1 on error
 */
static int s3_put_multipart(s3_ctx_t *s3, const char *key, FILE *fp, long file_size)
{
    const size_t part_size = s3->multipart_part_size; /* resolved to a default at create time */

    long parts_needed = (long)(((size_t)file_size + part_size - 1) / part_size);
    if (parts_needed < 1) parts_needed = 1;
    if (parts_needed > TDB_S3_MAX_PARTS) return -1; /* file too large for the part size */

    char upload_id[TDB_S3_UPLOAD_ID_MAX];
    if (s3_multipart_create(s3, key, upload_id, sizeof(upload_id)) != 0) return -1;

    char *part_buf = malloc(part_size);
    char *etags = malloc((size_t)parts_needed * TDB_S3_ETAG_MAX);
    if (!part_buf || !etags)
    {
        free(part_buf);
        free(etags);
        s3_multipart_abort(s3, key, upload_id);
        return -1;
    }

    int part_count = 0;
    int failed = 0;
    for (;;)
    {
        size_t got = fread(part_buf, 1, part_size, fp);
        if (got == 0)
        {
            if (ferror(fp)) failed = 1;
            break;
        }
        if (part_count >= parts_needed)
        {
            failed = 1; /* file grew underneath us */
            break;
        }
        if (s3_upload_part(s3, key, upload_id, part_count + 1, part_buf, got,
                           etags + (size_t)part_count * TDB_S3_ETAG_MAX, TDB_S3_ETAG_MAX) != 0)
        {
            failed = 1;
            break;
        }
        part_count++;
        if (got < part_size) break; /* short read -- last part */
    }

    int rc = -1;
    if (!failed && part_count > 0)
    {
        rc = s3_multipart_complete(s3, key, upload_id, etags, part_count);
    }
    if (rc != 0) s3_multipart_abort(s3, key, upload_id);

    free(part_buf);
    free(etags);
    return rc;
}

/**
 * s3_put
 * upload a local file to S3 as an object. files below the multipart
 * threshold use a single streaming PUT; files at or above it use a
 * multipart upload, so the connector never buffers a whole large file in
 * memory and is not bound by the 5 GiB single-PUT limit.
 * @param ctx opaque S3 connector context
 * @param key object key (path-like)
 * @param local_path path to the local file to upload
 * @return 0 on success, -1 on error
 */
static int s3_put(void *ctx, const char *key, const char *local_path)
{
    s3_ctx_t *s3 = (s3_ctx_t *)ctx;

    FILE *fp = fopen(local_path, "rb");
    if (!fp) return -1;

    if (fseek(fp, 0, SEEK_END) != 0)
    {
        fclose(fp);
        return -1;
    }
    long file_size = ftell(fp);
    if (file_size < 0)
    {
        fclose(fp);
        return -1;
    }
    rewind(fp);

    int rc;
    if ((size_t)file_size >= s3->multipart_threshold)
        rc = s3_put_multipart(s3, key, fp, file_size);
    else
        rc = s3_put_single(s3, key, fp, file_size);

    fclose(fp);
    return rc;
}

#ifndef _MSC_VER
#pragma GCC diagnostic pop
#endif

/**
 * s3_list
 * enumerate S3 objects under a key prefix using ListObjectsV2, handling pagination
 * @param ctx opaque S3 connector context
 * @param prefix key prefix to list (e.g. "cf_name/")
 * @param cb callback invoked for each object (key, size, cb_ctx)
 * @param cb_ctx opaque context passed to callback
 * @return number of objects listed, -1 on error
 */
#ifndef _MSC_VER
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif
static int s3_list(void *ctx, const char *prefix,
                   void (*cb)(const char *key, size_t size, void *cb_ctx), void *cb_ctx)
{
    s3_ctx_t *s3 = (s3_ctx_t *)ctx;
    int count = 0;
    char continuation_token[TDB_S3_CONT_TOKEN_MAX] = {0};

    do
    {
        char empty_sha[TDB_S3_HASH_HEX_LEN];
        sha256_hex("", 0, empty_sha);

        /* we build full prefix with connector prefix */
        char full_prefix[TDB_S3_MAX_PATH];
        if (s3->prefix[0])
            snprintf(full_prefix, sizeof(full_prefix), "%s%s", s3->prefix, prefix);
        else
            snprintf(full_prefix, sizeof(full_prefix), "%s", prefix);

        /* ListObjectsV2 -- prefix goes in query string, not in the URL path.
         * the canonical URI is just /<bucket> (path-style) or / (virtual-hosted).
         * the canonical query string must include all query parameters sorted
         * alphabetically with URI-encoded values per the SigV4 spec. */
        char url[TDB_S3_MAX_PATH + TDB_S3_CONT_TOKEN_MAX * 2];
        const char *scheme = s3->use_ssl ? "https" : "http";

        /* URI-encode prefix and continuation token for query string */
        char encoded_prefix[TDB_S3_MAX_PATH * 3];
        s3_uri_encode(full_prefix, encoded_prefix, sizeof(encoded_prefix));

        char encoded_token[TDB_S3_CONT_TOKEN_MAX * 3];
        if (continuation_token[0])
            s3_uri_encode(continuation_token, encoded_token, sizeof(encoded_token));

        /* we build canonical query string (params sorted alphabetically) */
        char canonical_qs[TDB_S3_MAX_PATH * 4];
        if (continuation_token[0])
            snprintf(canonical_qs, sizeof(canonical_qs),
                     "continuation-token=%s&list-type=2&prefix=%s", encoded_token, encoded_prefix);
        else
            snprintf(canonical_qs, sizeof(canonical_qs), "list-type=2&prefix=%s", encoded_prefix);

        if (s3->use_path_style)
        {
            snprintf(url, sizeof(url), "%s://%s/%s?%s", scheme, s3->endpoint, s3->bucket,
                     canonical_qs);
        }
        else
        {
            snprintf(url, sizeof(url), "%s://%s.%s/?%s", scheme, s3->bucket, s3->endpoint,
                     canonical_qs);
        }

        /* we sign with the correct canonical URI (bucket path only, no object prefix) */
        char canonical_uri[TDB_S3_MAX_PATH];
        if (s3->use_path_style)
            snprintf(canonical_uri, sizeof(canonical_uri), "/%s", s3->bucket);
        else
            snprintf(canonical_uri, sizeof(canonical_uri), "/");

        struct curl_slist *headers =
            s3_sign_raw(s3, "GET", canonical_uri, canonical_qs, empty_sha, NULL, NULL);

        s3_response_buf_t resp = {
            .data = malloc(TDB_S3_RESPONSE_INIT), .size = 0, .capacity = TDB_S3_RESPONSE_INIT};
        if (!resp.data)
        {
            curl_slist_free_all(headers);
            return count > 0 ? count : -1;
        }

        CURL *curl = s3_curl_new(s3);
        if (!curl)
        {
            free(resp.data);
            curl_slist_free_all(headers);
            return count > 0 ? count : -1;
        }
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, s3_write_to_response);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

        CURLcode res = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK || http_code != TDB_S3_HTTP_OK)
        {
            free(resp.data);
            return count > 0 ? count : -1;
        }

        /* we parse XML response for <Key> and <Size> tags within <Contents> */
        const char *pos = resp.data;
        while ((pos = strstr(pos, "<Contents>")) != NULL)
        {
            const char *end = strstr(pos, "</Contents>");
            if (!end) break;

            size_t key_len = 0, size_len = 0;
            const char *key_val = xml_find_tag(pos, "Key", &key_len);
            const char *size_val = xml_find_tag(pos, "Size", &size_len);

            if (key_val && key_len > 0)
            {
                char key_buf[TDB_S3_MAX_PATH];
                size_t copy_len = key_len < sizeof(key_buf) - 1 ? key_len : sizeof(key_buf) - 1;
                memcpy(key_buf, key_val, copy_len);
                key_buf[copy_len] = '\0';

                /* we strip the connector prefix to get relative key */
                const char *relative = key_buf;
                if (s3->prefix[0] && strncmp(relative, s3->prefix, strlen(s3->prefix)) == 0)
                {
                    relative += strlen(s3->prefix);
                }

                size_t obj_size = 0;
                if (size_val && size_len > 0)
                {
                    char size_buf[TDB_S3_SIZE_BUF];
                    size_t sl = size_len < sizeof(size_buf) - 1 ? size_len : sizeof(size_buf) - 1;
                    memcpy(size_buf, size_val, sl);
                    size_buf[sl] = '\0';
                    obj_size = (size_t)strtoull(size_buf, NULL, 10);
                }

                cb(relative, obj_size, cb_ctx);
                count++;
            }

            pos = end + 1;
        }

        /* we check for truncation (pagination) */
        continuation_token[0] = '\0';
        size_t ct_len = 0;
        const char *ct = xml_find_tag(resp.data, "NextContinuationToken", &ct_len);
        if (ct && ct_len > 0 && ct_len < TDB_S3_CONT_TOKEN_MAX)
        {
            memcpy(continuation_token, ct, ct_len);
            continuation_token[ct_len] = '\0';
        }

        /* we check IsTruncated */
        size_t trunc_len = 0;
        const char *trunc = xml_find_tag(resp.data, "IsTruncated", &trunc_len);
        int is_truncated = (trunc && trunc_len == 4 && memcmp(trunc, "true", 4) == 0);

        free(resp.data);

        if (!is_truncated) break;

    } while (1);

    return count;
}
#ifndef _MSC_VER
#pragma GCC diagnostic pop
#endif

/**
 * s3_destroy
 * free S3 connector resources
 * @param ctx opaque S3 connector context to free
 */
static void s3_destroy(void *ctx)
{
    free(ctx);
}

tidesdb_objstore_t *tidesdb_objstore_s3_create_config(const tidesdb_objstore_s3_config_t *config)
{
    if (!config || !config->endpoint || !config->bucket || !config->access_key ||
        !config->secret_key)
        return NULL;

    curl_global_init(CURL_GLOBAL_DEFAULT);

    s3_ctx_t *s3 = calloc(1, sizeof(s3_ctx_t));
    if (!s3) return NULL;

    snprintf(s3->endpoint, sizeof(s3->endpoint), "%s", config->endpoint);
    snprintf(s3->bucket, sizeof(s3->bucket), "%s", config->bucket);
    if (config->prefix) snprintf(s3->prefix, sizeof(s3->prefix), "%s", config->prefix);
    snprintf(s3->access_key, sizeof(s3->access_key), "%s", config->access_key);
    snprintf(s3->secret_key, sizeof(s3->secret_key), "%s", config->secret_key);
    snprintf(s3->region, sizeof(s3->region), "%s",
             config->region ? config->region : TDB_S3_DEFAULT_REGION);
    s3->use_ssl = config->use_ssl;
    s3->use_path_style = config->use_path_style;

    /* TLS copy a custom CA bundle path if given; the secure default (empty path +
     * skip_verify 0) leaves libcurl verifying peer+host against the system CA bundle. */
    if (config->tls_ca_path)
        snprintf(s3->tls_ca_path, sizeof(s3->tls_ca_path), "%s", config->tls_ca_path);
    s3->tls_insecure_skip_verify = config->tls_insecure_skip_verify;

    /* multipart honor the caller's tuning, falling back to the documented defaults */
    s3->multipart_threshold =
        config->multipart_threshold ? config->multipart_threshold : TDB_S3_MULTIPART_THRESHOLD;
    s3->multipart_part_size =
        config->multipart_part_size ? config->multipart_part_size : TDB_S3_MULTIPART_PART_SIZE;

    tidesdb_objstore_t *store = calloc(1, sizeof(tidesdb_objstore_t));
    if (!store)
    {
        free(s3);
        return NULL;
    }

    store->backend = TDB_BACKEND_S3;
    store->put = s3_put;
    store->get = s3_get;
    store->range_get = s3_range_get;
    store->delete_object = s3_delete_object;
    store->exists = s3_exists;
    store->list = s3_list;
    store->destroy = s3_destroy;
    store->ctx = s3;

    return store;
}

tidesdb_objstore_t *tidesdb_objstore_s3_create(const char *endpoint, const char *bucket,
                                               const char *prefix, const char *access_key,
                                               const char *secret_key, const char *region,
                                               int use_ssl, int use_path_style)
{
    /* thin wrapper preserving the original signature, secure TLS defaults (verify peer+host
     * against the system CA bundle) and default multipart tuning -- identical behavior to
     * before this entry point existed. */
    const tidesdb_objstore_s3_config_t config = {.endpoint = endpoint,
                                                 .bucket = bucket,
                                                 .prefix = prefix,
                                                 .access_key = access_key,
                                                 .secret_key = secret_key,
                                                 .region = region,
                                                 .use_ssl = use_ssl,
                                                 .use_path_style = use_path_style,
                                                 .tls_ca_path = NULL,
                                                 .tls_insecure_skip_verify = 0,
                                                 .multipart_threshold = 0,
                                                 .multipart_part_size = 0};
    return tidesdb_objstore_s3_create_config(&config);
}

#endif /* TIDESDB_WITH_S3 */
