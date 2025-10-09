/*****************************************************************************

Copyright (C) 2013, 2015, Google Inc. All Rights Reserved.
Copyright (C) 2014, 2022, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/
/**************************************************//**
@file log0crypt.cc
Innodb log encrypt/decrypt

Created 11/25/2013 Minli Zhu Google
Modified           Jan Lindström jan.lindstrom@mariadb.com
MDEV-11782: Rewritten for MariaDB 10.2 by Marko Mäkelä, MariaDB Corporation.
*******************************************************/
#include <my_global.h>
#include "log0crypt.h"
#include <mysql/service_my_crypt.h>
#include "assume_aligned.h"

#include "log0crypt.h"
#include "log0recv.h"  // for recv_sys
#include "mach0data.h"

/** Redo log encryption key ID */
#define LOG_DEFAULT_ENCRYPTION_KEY 1

struct crypt_info_t {
	uint32_t	checkpoint_no; /*!< checkpoint no; 32 bits */
	uint32_t	key_version;   /*!< key version */
	/** random string for encrypting the key */
	alignas(8) byte	crypt_msg[MY_AES_BLOCK_SIZE];
	/** the secret key */
	alignas(8) byte crypt_key[MY_AES_BLOCK_SIZE];
	/** a random string for the per-block initialization vector */
	alignas(4) byte	crypt_nonce[4];
};

/** The crypt info */
static crypt_info_t info;

/** Initialization vector used for temporary files/tablespace */
static byte tmp_iv[MY_AES_BLOCK_SIZE];

/** Crypt info when upgrading from 10.1 */
static crypt_info_t infos[5 * 2];
/** First unused slot in infos[] */
static size_t infos_used;

/* Offsets of a log block header */
#define	LOG_BLOCK_HDR_NO	0	/* block number which must be > 0 and
					is allowed to wrap around at 2G; the
					highest bit is set to 1 if this is the
					first log block in a log flush write
					segment */
#define LOG_BLOCK_FLUSH_BIT_MASK 0x80000000UL
					/* mask used to get the highest bit in
					the preceding field */
#define	LOG_BLOCK_HDR_DATA_LEN	4	/* number of bytes of log written to
					this block */
#define	LOG_BLOCK_FIRST_REC_GROUP 6	/* offset of the first start of an
					mtr log record group in this log block,
					0 if none; if the value is the same
					as LOG_BLOCK_HDR_DATA_LEN, it means
					that the first rec group has not yet
					been catenated to this log block, but
					if it will, it will start at this
					offset; an archive recovery can
					start parsing the log records starting
					from this offset in this log block,
					if value not 0 */
#define LOG_BLOCK_HDR_SIZE	12	/* size of the log block header in
					bytes */

#define	LOG_BLOCK_KEY		4	/* encryption key version
					before LOG_BLOCK_CHECKSUM;
					after log_t::FORMAT_ENC_10_4 only */
#define	LOG_BLOCK_CHECKSUM	4	/* 4 byte checksum of the log block
					contents; in InnoDB versions
					< 3.23.52 this did not contain the
					checksum but the same value as
					LOG_BLOCK_HDR_NO */

/*********************************************************************//**
Get a log block's start lsn.
@return a log block's start lsn */
static inline
lsn_t
log_block_get_start_lsn(
/*====================*/
	lsn_t lsn,			/*!< in: checkpoint lsn */
	ulint log_block_no)		/*!< in: log block number */
{
	lsn_t start_lsn =
		(lsn & (lsn_t)0xffffffff00000000ULL) |
		(((log_block_no - 1) & (lsn_t)0x3fffffff) << 9);
	return start_lsn;
}

/** Generate crypt key from crypt msg.
@param[in,out]	info	encryption key
@param[in]	upgrade	whether to use the key in MariaDB 10.1 format
@return whether the operation was successful */
static bool init_crypt_key(crypt_info_t* info, bool upgrade = false)
{
	byte	mysqld_key[MY_AES_MAX_KEY_LENGTH];
	uint	keylen = sizeof mysqld_key;

	compile_time_assert(16 == sizeof info->crypt_key);
	compile_time_assert(16 == MY_AES_BLOCK_SIZE);

	if (uint rc = encryption_key_get(LOG_DEFAULT_ENCRYPTION_KEY,
					 info->key_version, mysqld_key,
					 &keylen)) {
		ib::error()
			<< "Obtaining redo log encryption key version "
			<< info->key_version << " failed (" << rc
			<< "). Maybe the key or the required encryption "
			"key management plugin was not found.";
		info->key_version = ENCRYPTION_KEY_VERSION_INVALID;
		return false;
	}

	if (upgrade) {
		while (keylen < sizeof mysqld_key) {
			mysqld_key[keylen++] = 0;
		}
	}

	uint dst_len;
	int err= my_aes_crypt(MY_AES_ECB,
			      ENCRYPTION_FLAG_NOPAD | ENCRYPTION_FLAG_ENCRYPT,
			      info->crypt_msg, MY_AES_BLOCK_SIZE,
			      info->crypt_key, &dst_len,
			      mysqld_key, keylen, NULL, 0);

	if (err != MY_AES_OK || dst_len != MY_AES_BLOCK_SIZE) {
		ib::error() << "Getting redo log crypto key failed: err = "
			<< err << ", len = " << dst_len;
		info->key_version = ENCRYPTION_KEY_VERSION_INVALID;
		return false;
	}

	return true;
}

static ulint log_block_get_hdr_no(const byte *log_block)
{
  static_assert(LOG_BLOCK_HDR_NO == 0, "compatibility");
  return mach_read_from_4(my_assume_aligned<4>(log_block)) &
    ~LOG_BLOCK_FLUSH_BIT_MASK;
}

/** Decrypt log blocks.
@param[in,out]	buf	log blocks to decrypt
@param[in]	lsn	log sequence number of the start of the buffer
@param[in]	size	size of the buffer, in bytes
@return	whether the operation succeeded */
ATTRIBUTE_COLD bool log_decrypt(byte* buf, lsn_t lsn, ulint size)
{
	ut_ad(!(size & 511));
	ut_ad(!(ulint(buf) & 511));
	ut_a(info.key_version);

	alignas(8) byte aes_ctr_iv[MY_AES_BLOCK_SIZE];

#define LOG_CRYPT_HDR_SIZE 4
	lsn &= ~lsn_t{511};

	const bool has_encryption_key_rotation
		= log_sys.format == log_t::FORMAT_ENC_10_4
		|| log_sys.format == log_t::FORMAT_ENC_10_5;

	for (const byte* const end = buf + size; buf != end;
	     buf += 512, lsn += 512) {
		alignas(4) byte dst[512 - LOG_CRYPT_HDR_SIZE
				    - LOG_BLOCK_CHECKSUM];

		/* The log block number is not encrypted. */
		memcpy_aligned<4>(dst, buf + LOG_BLOCK_HDR_NO, 4);
		memcpy_aligned<4>(aes_ctr_iv, buf + LOG_BLOCK_HDR_NO, 4);
		*aes_ctr_iv &= byte(~(LOG_BLOCK_FLUSH_BIT_MASK >> 24));
		static_assert(LOG_BLOCK_HDR_NO + 4 == LOG_CRYPT_HDR_SIZE,
			      "compatibility");
		memcpy_aligned<4>(aes_ctr_iv + 4, info.crypt_nonce, 4);
		mach_write_to_8(my_assume_aligned<8>(aes_ctr_iv + 8), lsn);
		ut_ad(log_block_get_start_lsn(lsn,
					      log_block_get_hdr_no(buf))
		      == lsn);
		byte* key_ver = &buf[512 - LOG_BLOCK_KEY - LOG_BLOCK_CHECKSUM];

		const size_t dst_size = has_encryption_key_rotation
			? sizeof dst - LOG_BLOCK_KEY
			: sizeof dst;
		if (has_encryption_key_rotation) {
			const auto key_version = info.key_version;
			info.key_version = mach_read_from_4(key_ver);
			if (key_version == info.key_version) {
			} else if (!init_crypt_key(&info)) {
				return false;
#ifndef DBUG_OFF
			} else {
				DBUG_PRINT("ib_log", ("key_version: %x -> %x",
						      key_version,
						      info.key_version));
#endif /* !DBUG_OFF */
			}
		}

		ut_ad(LOG_CRYPT_HDR_SIZE + dst_size
		      == 512 - LOG_BLOCK_CHECKSUM - LOG_BLOCK_KEY);

		uint dst_len = static_cast<uint>(dst_size);
		int rc = encryption_crypt(
			buf + LOG_CRYPT_HDR_SIZE, dst_len,
			reinterpret_cast<byte*>(dst), &dst_len,
			const_cast<byte*>(info.crypt_key),
			MY_AES_BLOCK_SIZE,
			aes_ctr_iv, sizeof aes_ctr_iv,
			ENCRYPTION_FLAG_DECRYPT | ENCRYPTION_FLAG_NOPAD,
			LOG_DEFAULT_ENCRYPTION_KEY,
			info.key_version);
		ut_a(rc == MY_AES_OK);
		ut_a(dst_len == dst_size);
		memcpy(buf + LOG_CRYPT_HDR_SIZE, dst, dst_size);
	}

	return true;
}

/** Initialize the redo log encryption key and random parameters
when creating a new redo log.
The random parameters will be persisted in the log checkpoint pages.
@see log_crypt_write_header()
@see log_crypt_read_header()
@return whether the operation succeeded */
bool log_crypt_init()
{
  info.key_version=
    encryption_key_get_latest_version(LOG_DEFAULT_ENCRYPTION_KEY);

  if (info.key_version == ENCRYPTION_KEY_VERSION_INVALID)
    ib::error() << "log_crypt_init(): cannot get key version";
  else if (my_random_bytes(tmp_iv, MY_AES_BLOCK_SIZE) != MY_AES_OK ||
           my_random_bytes(info.crypt_msg, sizeof info.crypt_msg) !=
           MY_AES_OK ||
           my_random_bytes(info.crypt_nonce, sizeof info.crypt_nonce) !=
           MY_AES_OK)
    ib::error() << "log_crypt_init(): my_random_bytes() failed";
  else if (init_crypt_key(&info))
    goto func_exit;

  info.key_version= 0;
func_exit:
  return info.key_version != 0;
}

/** Read the MariaDB 10.1 checkpoint crypto (version, msg and iv) info.
@param[in]	buf	checkpoint buffer
@return	whether the operation was successful */
ATTRIBUTE_COLD bool log_crypt_101_read_checkpoint(const byte* buf)
{
	buf += 20 + 32 * 9;

	const size_t n = *buf++ == 2 ? std::min(unsigned(*buf++), 5U) : 0;

	for (size_t i = 0; i < n; i++) {
		struct crypt_info_t& info = infos[infos_used];
		unsigned checkpoint_no = mach_read_from_4(buf);
		for (size_t j = 0; j < infos_used; j++) {
			if (infos[j].checkpoint_no == checkpoint_no) {
				/* Do not overwrite an existing slot. */
				goto next_slot;
			}
		}
		if (infos_used >= UT_ARR_SIZE(infos)) {
			ut_ad("too many checkpoint pages" == 0);
			goto next_slot;
		}
		infos_used++;
		info.checkpoint_no = checkpoint_no;
		info.key_version = mach_read_from_4(buf + 4);
		memcpy(info.crypt_msg, buf + 8, MY_AES_BLOCK_SIZE);
		memcpy(info.crypt_nonce, buf + 24, sizeof info.crypt_nonce);

		if (!init_crypt_key(&info, true)) {
			return false;
		}
next_slot:
		buf += 4 + 4 + 2 * MY_AES_BLOCK_SIZE;
	}

	return true;
}

/** Decrypt a MariaDB 10.1 redo log block.
@param[in,out]	buf		log block
@param[in]	start_lsn	server start LSN
@return	whether the decryption was successful */
ATTRIBUTE_COLD bool log_crypt_101_read_block(byte* buf, lsn_t start_lsn)
{
	const uint32_t checkpoint_no = mach_read_from_4(buf + 8);
	const crypt_info_t* info = infos;
	for (const crypt_info_t* const end = info + infos_used; info < end;
	     info++) {
		if (info->key_version
		    && info->key_version != ENCRYPTION_KEY_VERSION_INVALID
		    && info->checkpoint_no == checkpoint_no) {
			goto found;
		}
	}

	if (infos_used == 0) {
		return false;
	}
	/* MariaDB Server 10.1 would use the first key if it fails to
	find a key for the current checkpoint. */
	info = infos;
	if (info->key_version == ENCRYPTION_KEY_VERSION_INVALID) {
		return false;
	}
found:
	byte dst[512];
	byte aes_ctr_iv[MY_AES_BLOCK_SIZE];

	const uint src_len = 512 - LOG_BLOCK_HDR_SIZE;
	uint dst_len = src_len;

	ulint log_block_no = log_block_get_hdr_no(buf);

	/* The log block header is not encrypted. */
	memcpy(dst, buf, 512);

	memcpy(aes_ctr_iv, info->crypt_nonce, 3);
	mach_write_to_8(aes_ctr_iv + 3,
			log_block_get_start_lsn(start_lsn, log_block_no));
	memcpy(aes_ctr_iv + 11, buf, 4);
	aes_ctr_iv[11] &= byte(~(LOG_BLOCK_FLUSH_BIT_MASK >> 24));
	aes_ctr_iv[15] = 0;

	int rc = encryption_crypt(buf + LOG_BLOCK_HDR_SIZE, src_len,
				  dst + LOG_BLOCK_HDR_SIZE, &dst_len,
				  const_cast<byte*>(info->crypt_key),
				  MY_AES_BLOCK_SIZE,
				  aes_ctr_iv, MY_AES_BLOCK_SIZE,
				  ENCRYPTION_FLAG_DECRYPT
				  | ENCRYPTION_FLAG_NOPAD,
				  LOG_DEFAULT_ENCRYPTION_KEY,
				  info->key_version);

	if (rc != MY_AES_OK || dst_len != src_len) {
		return false;
	}

	memcpy(buf, dst, sizeof dst);
	return true;
}

/** MariaDB 10.2.5 encrypted redo log encryption key version (32 bits)*/
constexpr size_t LOG_CHECKPOINT_CRYPT_KEY= 32;
/** MariaDB 10.2.5 encrypted redo log random nonce (32 bits) */
constexpr size_t LOG_CHECKPOINT_CRYPT_NONCE= 36;
/** MariaDB 10.2.5 encrypted redo log random message (MY_AES_BLOCK_SIZE) */
constexpr size_t LOG_CHECKPOINT_CRYPT_MESSAGE= 40;

/** Add the encryption information to the log header buffer.
@param buf   part of log header buffer */
void log_crypt_write_header(byte *buf)
{
  ut_ad(info.key_version);
  mach_write_to_4(my_assume_aligned<4>(buf), LOG_DEFAULT_ENCRYPTION_KEY);
  mach_write_to_4(my_assume_aligned<4>(buf + 4), info.key_version);
  memcpy_aligned<8>(buf + 8, info.crypt_msg, MY_AES_BLOCK_SIZE);
  static_assert(MY_AES_BLOCK_SIZE == 16, "compatibility");
  memcpy_aligned<4>(buf + 24, info.crypt_nonce, sizeof info.crypt_nonce);
}

/** Read the encryption information from a log header buffer.
@param buf   part of log header buffer
@return whether the operation was successful */
bool log_crypt_read_header(const byte *buf)
{
  MEM_UNDEFINED(&info.checkpoint_no, sizeof info.checkpoint_no);
  MEM_NOACCESS(&info.checkpoint_no, sizeof info.checkpoint_no);
  if (mach_read_from_4(my_assume_aligned<4>(buf)) !=
      LOG_DEFAULT_ENCRYPTION_KEY)
    return false;
  info.key_version= mach_read_from_4(my_assume_aligned<4>(buf + 4));
  memcpy_aligned<8>(info.crypt_msg, buf + 8, MY_AES_BLOCK_SIZE);
  memcpy_aligned<4>(info.crypt_nonce, buf + 24, sizeof info.crypt_nonce);
  return init_crypt_key(&info);
}

/** Read the checkpoint crypto (version, msg and iv) info.
@param[in]	buf	checkpoint buffer
@return	whether the operation was successful */
ATTRIBUTE_COLD bool log_crypt_read_checkpoint_buf(const byte* buf)
{
	info.checkpoint_no = mach_read_from_4(buf + 4);
	info.key_version = mach_read_from_4(buf + LOG_CHECKPOINT_CRYPT_KEY);

#if MY_AES_BLOCK_SIZE != 16
# error "MY_AES_BLOCK_SIZE != 16; redo log checkpoint format affected"
#endif
	compile_time_assert(16 == sizeof info.crypt_msg);
	compile_time_assert(16 == MY_AES_BLOCK_SIZE);
	compile_time_assert(LOG_CHECKPOINT_CRYPT_MESSAGE
			    - LOG_CHECKPOINT_CRYPT_NONCE
			    == sizeof info.crypt_nonce);

	memcpy(info.crypt_msg, buf + LOG_CHECKPOINT_CRYPT_MESSAGE,
	       MY_AES_BLOCK_SIZE);
	memcpy(info.crypt_nonce, buf + LOG_CHECKPOINT_CRYPT_NONCE,
	       sizeof info.crypt_nonce);

	return init_crypt_key(&info);
}

/** Encrypt or decrypt a temporary file block.
@param[in]	src		block to encrypt or decrypt
@param[in]	size		length of both src and dst blocks in bytes
@param[out]	dst		destination block
@param[in]	offs		offset to block
@param[in]	encrypt		true=encrypt; false=decrypt
@return whether the operation succeeded */
bool log_tmp_block_encrypt(
	const byte*	src,
	ulint		size,
	byte*		dst,
	uint64_t	offs,
	bool		encrypt)
{
	uint dst_len = static_cast<uint>(size);
	uint64_t iv[MY_AES_BLOCK_SIZE / sizeof(uint64_t)];
	iv[0] = offs;
	memcpy(iv + 1, tmp_iv, sizeof iv - sizeof *iv);

	int rc = encryption_crypt(
		src, uint(size), dst, &dst_len,
		const_cast<byte*>(info.crypt_key), MY_AES_BLOCK_SIZE,
		reinterpret_cast<byte*>(iv), uint(sizeof iv),
		encrypt
		? ENCRYPTION_FLAG_ENCRYPT|ENCRYPTION_FLAG_NOPAD
		: ENCRYPTION_FLAG_DECRYPT|ENCRYPTION_FLAG_NOPAD,
		LOG_DEFAULT_ENCRYPTION_KEY, info.key_version);

	if (rc != MY_AES_OK) {
		ib::error() << (encrypt ? "Encryption" : "Decryption")
			    << " failed for temporary file: " << rc;
	}

	return rc == MY_AES_OK;
}

/** Decrypt part of a log record.
@param iv    initialization vector
@param buf   buffer for the decrypted data
@param data  the encrypted data
@param len   length of the data, in bytes
@return buf */
byte *log_decrypt_buf(const byte *iv, byte *buf, const byte *data, uint len)
{
  ut_a(MY_AES_OK == encryption_crypt(data, len, buf, &len,
                                     info.crypt_key, MY_AES_BLOCK_SIZE,
                                     iv, MY_AES_BLOCK_SIZE,
                                     ENCRYPTION_FLAG_DECRYPT |
                                     ENCRYPTION_FLAG_NOPAD,
                                     LOG_DEFAULT_ENCRYPTION_KEY,
                                     info.key_version));
  return buf;
}

#include "mtr0log.h"

/**
@brief Interface for log_t::FORMAT_ENC_11 encryption and decryption

Each mtr_t::m_log record comprise one or more bytes that determine the
length of the remaining bytes, which will be encrypted. The encryption
plugins are assumed to be variants of AES, with
encryption_ctx_update() operating on fixed-size blocks that are
integer multiples of MY_AES_BLOCK_SIZE. */
class log_crypt
{
  /** Temporary buffer with data to be encrypted */
  alignas(MY_AES_BLOCK_SIZE) byte tmp[MY_AES_BLOCK_SIZE * 16];
  /** Destination for encryption buffer */
  alignas(MY_AES_BLOCK_SIZE) byte dst[MY_AES_BLOCK_SIZE * 16];

  /** Incompletely filled buffers */
  st_::span<byte> backlog[MY_AES_BLOCK_SIZE - 1] {};

  /** Sum of backlog.size() */
  size_t deferred{0};
  /** First unused backlog entry (0 to MY_AES_BLOCK_SIZE) */
  size_t back{0};

  /** Write back the encrypted data to the buffers. */
  void scatter() noexcept
  {
    byte *d{dst};
    for (size_t i{0}; i < back; i++)
    {
      st_::span<byte> &b= backlog[i];
      memcpy(b.data(), d, b.size());
      deferred-= b.size();
      d+= b.size();
    }
    ut_ad(!deferred);
    back= 0;
  }

  /** Finish the encryption.

  It is assumed that encryption_ctx_init(ctx, ... ENCRYPTION_FLAG_NOPAD ...)
  will have been invoked.

  If the total length of the encrypted data of a mini-transaction is
  not an integer multiple of MY_AES_BLOCK_SIZE, we expect the partial
  final block (deferred bytes) to be encrypted or decrypted in place,
  without being expanded in size. */
  void finish() noexcept
  {
    uint d{uint(deferred)};
    ut_a(!d || MY_AES_OK == encryption_ctx_update(ctx, tmp, d, dst, &d));
    ut_d(const bool ok{d == deferred});
    ut_a(MY_AES_OK == encryption_ctx_finish(ctx, dst, &d));
    ut_ad(ok || d == deferred);
    scatter();
  }

public:
  /** pointer to the encryption context, must be initialized by
  encryption_ctx_init(ctx, ...) before any use of the constructed object */
  void *const ctx;

  log_crypt(void *ctx) : ctx(ctx) {}

  ~log_crypt() { finish(); }

  /** Append some more data to be encrypted in place. The data will
  be encrypted in chunks of MY_AES_BLOCK_SIZE. Some of the encrypted data
  may be replaced on a subsequent append() or finish().

  It is assumed that encryption_ctx_init(ctx, ...) will have been invoked.

  @param buf   the source and target buffer
  @param size  size of the data
  @return buf + size */
  byte *append(byte *buf, size_t size) noexcept
  {
    ut_ad(!deferred == !back);
    ut_ad(size);

    do
    {
      ut_ad(deferred < MY_AES_BLOCK_SIZE);
      size_t s{std::min(deferred + size, sizeof tmp)};
      ut_ad(s > deferred);
      ::memcpy(tmp + deferred, buf, s - deferred);

      if (s < MY_AES_BLOCK_SIZE)
      {
        ut_ad(back < array_elements(backlog));
        deferred+= size;
        backlog[back++]= {buf, size};
        return buf + size;
      }

      s&= ~(MY_AES_BLOCK_SIZE - 1);
      ut_ad(s > deferred);
      uint d;
      ut_a(MY_AES_OK == encryption_ctx_update(ctx, tmp, uint(s), dst, &d));
      ut_ad(s == d);
      s-= deferred;
      ut_ad(size >= s);
      memcpy(buf, dst + deferred, s);
      scatter();
      buf+= s;
      size-= s;
    }
    while (size);

    return buf;
  }
};

/** Decrypt a mini-transaction in place.
@param buf   start of the mini-transaction
@param end   end of data (followed by sequence byte and the 8-byte nonce) */
void log_decrypt_mtr(byte *buf, const byte *end) noexcept
{
  ut_ad(log_sys.format == log_t::FORMAT_ENC_11);

  uint32_t rlen;
  log_crypt c(alloca(encryption_ctx_size(LOG_DEFAULT_ENCRYPTION_KEY,
                                         info.key_version)));
  {
    alignas(8) byte iv[MY_AES_BLOCK_SIZE];
    memcpy(iv, end + 1, 8);
    memset_aligned<8>(iv + 8, 0, (sizeof iv) - 8);
    /* Append the initial type,length to the initialization vector. */
    const byte *start{buf};
    buf= const_cast<byte*>(mtr_t::parse_length(buf, &rlen));
    ut_ad(buf < end);
    ::memcpy(iv + 8, start, buf - start);
    ut_a(MY_AES_OK ==
         encryption_ctx_init(c.ctx, info.crypt_key, MY_AES_BLOCK_SIZE,
                             iv, MY_AES_BLOCK_SIZE,
                             ENCRYPTION_FLAG_DECRYPT | ENCRYPTION_FLAG_NOPAD,
                             LOG_DEFAULT_ENCRYPTION_KEY, info.key_version));
  }

  for (;;)
  {
    buf= c.append(buf, rlen);
    if (buf >= end)
      break;
    buf= const_cast<byte*>(mtr_t::parse_length(buf, &rlen));
    ut_ad(buf < end);
  }

  ut_ad(buf == end);
}

ATTRIBUTE_NOINLINE size_t mtr_t::encrypt() noexcept
{
  ut_ad(log_sys.format == log_t::FORMAT_ENC_11);

  {
    auto i= m_log.begin();
    ut_ad(i != m_log.end());
    ut_ad(i->used());

    byte *buf= i->start();
    const byte *end= buf + i->used();
    uint32_t rlen;
    log_crypt c(alloca(encryption_ctx_size(LOG_DEFAULT_ENCRYPTION_KEY,
                                           info.key_version)));
    {
      alignas(8) byte iv[MY_AES_BLOCK_SIZE];
      m_commit_lsn= log_sys.get_flushed_lsn();
      ut_ad(m_commit_lsn);
      mach_write_to_8(iv, m_commit_lsn);
      memset_aligned<8>(iv + 8, 0, (sizeof iv) - 8);
      /* Append the initial type,length to the initialization vector. */
      const byte *start{buf};
      buf= const_cast<byte*>(mtr_t::parse_length(buf, &rlen));
      ut_ad(buf < end);
      ::memcpy(iv + 8, start, buf - start);
      ut_a(MY_AES_OK ==
           encryption_ctx_init(c.ctx, info.crypt_key, MY_AES_BLOCK_SIZE,
                               iv, MY_AES_BLOCK_SIZE,
                               ENCRYPTION_FLAG_ENCRYPT | ENCRYPTION_FLAG_NOPAD,
                               LOG_DEFAULT_ENCRYPTION_KEY, info.key_version));
    }

    for (;;)
    {
      while (buf + rlen > end)
      {
        rlen-= uint32_t(end - buf);
        c.append(buf, end - buf);
        ++i;
        ut_ad(i != m_log.end());
        buf= i->start();
        end= buf + i->used();
      }

      buf= c.append(buf, rlen);

      if (buf >= end)
      {
        ut_ad(buf == end);
        if (++i == m_log.end())
          break;
        buf= i->start();
        end= buf + i->used();
      }

      buf= const_cast<byte*>(mtr_t::parse_length(buf, &rlen));
      ut_ad(buf < end);
    }
  }

  return crc32c();
}
