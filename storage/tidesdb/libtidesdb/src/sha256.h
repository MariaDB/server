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
#ifndef __SHA256_H__
#define __SHA256_H__
#include "compat.h"

/* self-contained SHA-256 (FIPS 180-4). the transform is derived from Brad Conte's
 * public-domain implementation (https://github.com/B-Con/crypto-algorithms). it exists so the
 * S3 connector can compute AWS SigV4 digests -- curl alone covers
 * the HTTP transport. this is a correctness implementation, not a hardened/constant-time one. */

#define SHA256_DIGEST_SIZE 32 /* raw digest bytes */
#define SHA256_BLOCK_SIZE  64 /* compression block bytes (used by HMAC) */
#define SHA256_HEX_SIZE    65 /* lowercase hex digest + NUL */

/**
 * sha256_ctx
 * streaming SHA-256 state
 * @param data    partial block buffer awaiting a full SHA256_BLOCK_SIZE
 * @param datalen bytes currently buffered in data
 * @param bitlen  total message length in bits processed so far
 * @param state   the eight 32-bit hash words (H0..H7)
 */
typedef struct
{
    uint8_t data[SHA256_BLOCK_SIZE];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
} sha256_ctx;

/**
 * sha256_init
 * initialize a streaming context to the FIPS 180-4 initial hash values
 * @param ctx context to initialize
 */
void sha256_init(sha256_ctx *ctx);

/**
 * sha256_update
 * absorb len bytes of message into the context
 * @param ctx  initialized context
 * @param data input bytes
 * @param len  number of input bytes
 */
void sha256_update(sha256_ctx *ctx, const uint8_t *data, size_t len);

/**
 * sha256_final
 * pad, finish, and emit the digest. the context must not be reused after this without re-init.
 * @param ctx context to finalize
 * @param out receives SHA256_DIGEST_SIZE digest bytes
 */
void sha256_final(sha256_ctx *ctx, uint8_t out[SHA256_DIGEST_SIZE]);

/**
 * sha256_hash
 * one-shot digest of a single buffer
 * @param data input bytes (may be NULL only when len is 0)
 * @param len  number of input bytes
 * @param out  receives SHA256_DIGEST_SIZE digest bytes
 */
void sha256_hash(const void *data, size_t len, uint8_t out[SHA256_DIGEST_SIZE]);

/**
 * sha256_hex
 * one-shot digest of a single buffer, written as a lowercase hex string
 * @param data    input bytes (may be NULL only when len is 0)
 * @param len     number of input bytes
 * @param hex_out receives SHA256_HEX_SIZE bytes (64 hex chars + NUL)
 */
void sha256_hex(const void *data, size_t len, char *hex_out);

#endif /* __SHA256_H__ */
