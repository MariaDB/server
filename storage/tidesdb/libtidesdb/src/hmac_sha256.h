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
#ifndef __HMAC_SHA256_H__
#define __HMAC_SHA256_H__
#include "compat.h"
#include "sha256.h"

/* HMAC-SHA-256 (RFC 2104) built on the bundled SHA-256. lets the S3 connector derive AWS SigV4
 * signing keys and signatures. */

#define HMAC_SHA256_DIGEST_SIZE SHA256_DIGEST_SIZE /* always 32 bytes */

/**
 * hmac_sha256
 * compute HMAC-SHA-256 of data under key, emitting a fixed 32-byte MAC
 * @param key      MAC key
 * @param key_len  key length in bytes (keys longer than the block size are hashed first)
 * @param data     message bytes
 * @param data_len message length in bytes
 * @param out      receives HMAC_SHA256_DIGEST_SIZE MAC bytes
 */
void hmac_sha256(const void *key, size_t key_len, const void *data, size_t data_len,
                 uint8_t out[HMAC_SHA256_DIGEST_SIZE]);

#endif /* __HMAC_SHA256_H__ */
