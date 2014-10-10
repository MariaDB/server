/*****************************************************************************

Copyright (C) 2014 eperi GmbH. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA

*****************************************************************************/

/*****************************************************************
 @file fil/fil0pageencryption.cc
 Implementation for page encryption file spaces.

 Created 08/25/2014
 ***********************************************************************/

#include "fil0fil.h"
#include "fil0pageencryption.h"
#include "fsp0pageencryption.h"
#include "my_dbug.h"

#include "buf0checksum.h"

#include <my_aes.h>
#include <KeySingleton.h>

//#define UNIV_PAGEENCRIPTION_DEBUG
//#define CRYPT_FF

/* calculate a 3 byte checksum to verify decryption. One byte is needed for other things */
ulint fil_page_encryption_calc_checksum(unsigned char* buf, size_t size) {
	ulint checksum = 0;
	checksum = ut_fold_binary(buf, size);
	checksum = checksum & 0x0FFFFFF0UL;
	checksum = checksum >> 8;
	checksum = checksum & 0x00FFFFFFUL;

	return checksum;
}

/****************************************************************//**
 For page encrypted pages encrypt the page before actual write
 operation.
 @return encrypted page to be written*/
byte*
fil_encrypt_page(
/*==============*/
ulint space_id, /*!< in: tablespace id of the table. */
byte* buf, /*!< in: buffer from which to write; in aio
 this must be appropriately aligned */
byte* out_buf, /*!< out: encrypted buffer */
ulint len, /*!< in: length of input buffer.*/
ulint encryption_key,/*!< in: encryption key */
ulint* out_len, /*!< out: actual length of encrypted page */
ulint mode
) {

	int err = AES_OK;
	int key = 0;
	ulint header_len = FIL_PAGE_DATA;
	ulint remainder = 0;
	ulint data_size = 0;
	ulint page_size = UNIV_PAGE_SIZE;
	ulint orig_page_type = 0;
	ulint write_size = 0;
	ib_uint32_t checksum = 0;
	ulint offset_ctrl_data = 0;
	fil_space_t* space = NULL;
	byte* tmp_buf = NULL;
	ulint unit_test = 0;
	ibool page_compressed = 0 ;
	ut_ad(buf);ut_ad(out_buf);
	key = encryption_key;
	ulint offset = 0;
	byte remaining_byte = 0 ;

	unit_test = 0x01 & mode;

	//TODO encryption default key
	/* If no encryption key was provided to this table, use system
	 default key
	 if (key == 0) {
	 key = 0;
	 }*/

	if (!unit_test) {
		ut_ad(fil_space_is_page_encrypted(space_id));
		fil_system_enter();
		space = fil_space_get_by_id(space_id);
		fil_system_exit();

#ifdef UNIV_DEBUG
		fprintf(stderr,
				"InnoDB: Note: Preparing for encryption for space %lu name %s len %lu\n",
				space_id, fil_space_name(space), len);
#endif /* UNIV_DEBUG */

	}

	/* data_size -1 bytes will be encrypted at first.
	 * data_size is the length of the cipher text.*/
	data_size = ((page_size - header_len - FIL_PAGE_DATA_END) / 16) * 16;

	/* following number of bytes are not encrypted at first */
	remainder = (page_size - header_len - FIL_PAGE_DATA_END) - (data_size - 1);

	/* read original page type */
	orig_page_type = mach_read_from_2(buf + FIL_PAGE_TYPE);

	if (orig_page_type == FIL_PAGE_PAGE_COMPRESSED) {
		page_compressed = 1;
	}

	/* calculate a checksum, can be used to verify decryption */
	checksum = fil_page_encryption_calc_checksum(buf + header_len, page_size - (FIL_PAGE_DATA_END + header_len));

	const unsigned char rkey[] = {0xbd, 0xe4, 0x72, 0xa2, 0x95, 0x67, 0x5c, 0xa9,
			0x2e, 0x04, 0x67, 0xea, 0xdb, 0xc0,0xe0, 0x23,
			0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0};
	uint8 key_len = 16;
	if (!unit_test) {
		KeySingleton& keys = KeySingleton::getInstance();
		if (!keys.isAvailable()) return buf;
		if (keys.getKeys(encryption_key) == NULL) return buf;
		char* keyString = keys.getKeys(encryption_key)->key;
		key_len = strlen(keyString)/2;
		my_aes_hexToUint(keyString, (unsigned char*)&rkey, key_len);
	}
	const unsigned char iv[] = {0x2d, 0x1a, 0xf8, 0xd3, 0x97, 0x4e, 0x0b, 0xd3, 0xef, 0xed,
		0x5a, 0x6f, 0x82, 0x59, 0x4f,0x5e};
	if (!unit_test) {
		KeySingleton& keys = KeySingleton::getInstance();
		if (!keys.isAvailable()) return buf;
		if (keys.getKeys(encryption_key) == NULL) return buf;
		char* ivString = keys.getKeys(encryption_key)->iv;
		if (ivString == NULL) return buf;
		my_aes_hexToUint(ivString, (unsigned char*)&iv, 16);
	}
	uint8 iv_len = 16;
	write_size = data_size;
	/* 1st encryption: data_size -1 bytes starting from FIL_PAGE_DATA */
	err = my_aes_encrypt_cbc((char*)buf + header_len,
			data_size-1,
			(char *)out_buf + header_len,
			&write_size,
			(const unsigned char *)&rkey,
			key_len,
			(const unsigned char *)&iv,
			iv_len);;
	ut_ad(write_size == data_size);

	if (page_compressed) {
		/* page compressed pages: only one encryption. 3 bytes remain unencrypted. 2 bytes are appended to the encrypted buffer.
		 * one byte is later written to the checksum header.
		 */
		offset = 1;
	}

	/* copy remaining bytes to output buffer */
	memcpy(out_buf + header_len + data_size, buf + header_len + data_size - 1,
			remainder - offset);

	if (page_compressed) {
		remaining_byte = mach_read_from_1(buf + header_len + data_size +1);
	} else {

		/* create temporary buffer for 2nd encryption */
		tmp_buf = static_cast<byte *>(ut_malloc(64));

	/* 2nd encryption: 63 bytes from out_buf, result length is 64 bytes */
	err = my_aes_encrypt_cbc((char*)out_buf + page_size -FIL_PAGE_DATA_END -62,
				63,
				(char*)tmp_buf,
				&write_size,
				(const unsigned char *)&rkey,
				key_len,
				(const unsigned char *)&iv,
				iv_len);
		ut_ad(write_size == 64);
		//AES_cbc_encrypt((uchar*)out_buf + page_size -FIL_PAGE_DATA_END -62, tmp_buf, 63, &aeskey, iv, AES_ENCRYPT);
		/* copy 62 bytes from 2nd encryption to out_buf, last 2 bytes are copied later to a header field*/
		memcpy(out_buf + page_size - FIL_PAGE_DATA_END -62, tmp_buf, 62);
	}

	/* error handling */
	if (err != AES_OK) {
		/* If error we leave the actual page as it was */

		fprintf(stderr,
				"InnoDB: Warning: Encryption failed for space %lu name %s len %lu rt %d write %lu\n",
			space_id, fil_space_name(space), len, err, data_size);
		fflush(stderr);
		srv_stats.pages_page_encryption_error.inc();
		*out_len = len;

		/* free temporary buffer */
		if (tmp_buf!=NULL) {
			ut_free(tmp_buf);
		}

		return (buf);
	}

	/* set up the trailer.*/
	memcpy(out_buf + (page_size -FIL_PAGE_DATA_END),
				buf + (page_size - FIL_PAGE_DATA_END), FIL_PAGE_DATA_END);


	/* Set up the page header. Copied from input buffer*/
	memcpy(out_buf, buf, FIL_PAGE_DATA);


	ulint compressed_size = mach_read_from_2(buf+ FIL_PAGE_DATA);
	/* checksum */
	if (!page_compressed) {
		/* Set up the checksum. This is only usable to verify decryption */
		mach_write_to_3(out_buf + page_size - FIL_PAGE_DATA_END, checksum);
	} else {
		ulint pos_checksum = page_size - FIL_PAGE_DATA_END;
		if (compressed_size + FIL_PAGE_DATA > pos_checksum) {
			pos_checksum = compressed_size + FIL_PAGE_DATA;
			if (pos_checksum > page_size - 3) {
				// checksum not supported, because no space available
			} else {
				/* Set up the checksum. This is only usable to verify decryption */
				mach_write_to_3(out_buf + pos_checksum, checksum);
			}
		} else {
			mach_write_to_3(out_buf + page_size - FIL_PAGE_DATA_END, checksum);
		}
	}

	/* Set up the correct page type */
	mach_write_to_2(out_buf + FIL_PAGE_TYPE, FIL_PAGE_PAGE_ENCRYPTED);


	offset_ctrl_data = page_size - FIL_PAGE_DATA_END;

	/* checksum fields are used to store original page type, etc.
	 * checksum check for page encrypted pages is omitted. PAGE_COMPRESSED pages does not seem to have a
	 * Old-style checksum trailer, therefore this field is only used, if there is space. Payload length is expected as
	 * two byte value at position FIL_PAGE_DATA */


	/* Set up the encryption key. Written to the 1st byte of the checksum header field. This header is currently used to store data. */
	mach_write_to_1(out_buf + page_size - FIL_PAGE_DATA_END - offset_ctrl_data,
			key);

	/* store original page type. Written to 2nd and 3rd byte of the checksum header field */
	mach_write_to_2(
			out_buf + page_size - FIL_PAGE_DATA_END + 1 - offset_ctrl_data,
			orig_page_type);

	/* write remaining bytes to checksum header byte 4 and old style checksum byte 4 */
	if (!page_compressed) {
		memcpy(out_buf+ page_size - FIL_PAGE_DATA_END + 3 - offset_ctrl_data,
				tmp_buf + 62, 1);
		memcpy(out_buf + page_size - FIL_PAGE_DATA_END +3 , tmp_buf + 63, 1);
	} else {
		/* if page is compressed, only one byte must be placed */
		memset(out_buf+ page_size - FIL_PAGE_DATA_END + 3 - offset_ctrl_data,
				remaining_byte, 1);
	}

#ifdef UNIV_DEBUG
	/* Verify */
	ut_ad(fil_page_is_encrypted(out_buf));

#endif /* UNIV_DEBUG */

	srv_stats.pages_page_encrypted.inc();
	*out_len = page_size;

	/* free temporary buffer */
	if (tmp_buf!=NULL) {
		ut_free(tmp_buf);
	}
	return (out_buf);
}

/****************************************************************//**
 For page encrypted pages decrypt the page after actual read
 operation.
 @return decrypted page */
ulint fil_decrypt_page(
/*================*/
byte* page_buf, /*!< in: preallocated buffer or NULL */
byte* buf, /*!< in/out: buffer from which to read; in aio
 this must be appropriately aligned */
ulint len, /*!< in: length of output buffer.*/
ulint* write_size, /*!< in/out: Actual payload size of the decrypted data. */
ibool* page_compressed, /*!<out: is page compressed.*/
ulint mode
) {
	int err = AES_OK;
	ulint page_encryption_key;

	ulint data_size = 0;

	ulint page_size = UNIV_PAGE_SIZE;
	ulint orig_page_type = 0;

	ulint header_len = FIL_PAGE_DATA;
	ulint remainder = 0;
	ulint offset_ctrl_data = 0;
	ulint checksum = 0;
	ulint stored_checksum = 0;
	ulint tmp_write_size = 0;
	ulint offset = 0;
	byte * in_buf;
	byte * tmp_buf;
	byte * tmp_page_buf;
	ulint flags = 0;
	fil_space_t* space = NULL;

	ulint page_num = 0;
	ulint page_encrypted = 0;
	ulint page_compression_flag = 0;
	ulint unit_test = mode & 0x01;
	ulint space_id = 0;


	ut_ad(buf);ut_ad(len);

	/* Before actual decrypt, make sure that page type is correct */

	if (mach_read_from_2(buf + FIL_PAGE_TYPE) != FIL_PAGE_PAGE_ENCRYPTED) {
		fprintf(stderr, "InnoDB: Corruption: We try to decrypt corrupted page\n"
				"InnoDB: CRC %lu type %lu.\n"
				"InnoDB: len %lu\n",
				mach_read_from_4(buf + FIL_PAGE_SPACE_OR_CHKSUM),
				mach_read_from_2(buf + FIL_PAGE_TYPE), len);

		fflush(stderr);
		return PAGE_ENCRYPTION_WRONG_PAGE_TYPE;

	}

	space_id = mach_read_from_4(buf + FIL_PAGE_SPACE_ID);


	/* checksum fields are used to store original page type, etc.
	 * checksum check for page encrypted pages is omitted. PAGE_COMPRESSED pages does not seem to have a
	 * Old-style checksum trailer, therefore this field is only used, if there is space. Payload is expected as
	 * two byte value at position FIL_PAGE_DATA */

	offset_ctrl_data = page_size - FIL_PAGE_DATA_END;

	/* Get encryption key. it was not successful here to read the page encryption key from the tablespace flags, because
	 * of unresolved deadlocks while accessing the tablespace memory structure.*/
	page_encryption_key = mach_read_from_1(
				buf + page_size - FIL_PAGE_DATA_END  - offset_ctrl_data);


	/* Get the page type */
	orig_page_type = mach_read_from_2(
			buf + page_size - FIL_PAGE_DATA_END + 1 - offset_ctrl_data);

	if (FIL_PAGE_PAGE_COMPRESSED == orig_page_type) {
		if (page_compressed != NULL) {
			*page_compressed = 1L;
		}
		page_compression_flag = 1;
		offset = 1;
	}

// If no buffer was given, we need to allocate temporal buffer
	if (NULL == page_buf) {
#ifdef UNIV_PAGEENCRIPTION_DEBUG
		fprintf(stderr,
				"InnoDB: FIL: Note: Decryption buffer not given, allocating...\n");
		fflush(stderr);
#endif /* UNIV_PAGEENCRIPTION_DEBUG */
		in_buf = static_cast<byte *>(ut_malloc(len + 16));
		//in_buf = static_cast<byte *>(ut_malloc(UNIV_PAGE_SIZE));
	} else {
		in_buf = page_buf;
	}

	data_size = ((page_size - header_len - FIL_PAGE_DATA_END) / 16) * 16;
	remainder = (page_size - header_len - FIL_PAGE_DATA_END) - (data_size - 1);

#ifdef UNIV_PAGEENCRIPTION_DEBUG
	fprintf(stderr,
			"InnoDB: Note: Preparing for decrypt for len %lu\n", actuulint compressed_size = mach_read_from_2(buf+ FIL_PAGE_DATA);
	if (!page_compressed) {
		/* Set up the checksum. This is only usable to verify decryption */
		mach_write_to_3(out_buf + page_size - FIL_PAGE_DATA_END, checksum);
	} else {
		ulint pos_checksum = page_size - FIL_PAGE_DATA_END;
		if (compressed_size + FIL_PAGE_DATA > pos_checksum) {
			pos_checksum = compressed_size + FIL_PAGE_DATA;
			if (pos_checksum > page_size - 3) {
				// checksum not supported, because no space available
			} else {
				/* Set up the checksum. This is only usable to verify decryption */
				mach_write_to_3(out_buf + pos_checksum, checksum);
			}
		}
	}
al_size);
	fflush(stderr);
#endif /* UNIV_PAGEENCRIPTION_DEBUG */

	tmp_buf= static_cast<byte *>(ut_malloc(64));
	tmp_page_buf = static_cast<byte *>(ut_malloc(page_size));
	memset(tmp_page_buf,0, page_size);

	const unsigned char rkey[] = {0xbd, 0xe4, 0x72, 0xa2, 0x95, 0x67, 0x5c, 0xa9,
			0x2e, 0x04, 0x67, 0xea, 0xdb, 0xc0,0xe0, 0x23,
			0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0};
		uint8 key_len = 16;
		if (!unit_test) {
			KeySingleton& keys = KeySingleton::getInstance();
			if (!keys.isAvailable()) return PAGE_ENCRYPTION_ERROR;
			if (keys.getKeys(page_encryption_key) == NULL) return PAGE_ENCRYPTION_ERROR;
			char* keyString = keys.getKeys(page_encryption_key)->key;
			key_len = strlen(keyString)/2;
			my_aes_hexToUint(keyString, (unsigned char*)&rkey, key_len);
		}


		const unsigned char iv[] = {0x2d, 0x1a, 0xf8, 0xd3, 0x97, 0x4e, 0x0b, 0xd3, 0xef, 0xed,
				0x5a, 0x6f, 0x82, 0x59, 0x4f,0x5e};
		if (!unit_test) {
			KeySingleton& keys = KeySingleton::getInstance();
			if (!keys.isAvailable()) return PAGE_ENCRYPTION_ERROR;
			if (keys.getKeys(page_encryption_key) == NULL) return PAGE_ENCRYPTION_ERROR;
			my_aes_hexToUint(keys.getKeys(page_encryption_key)->iv, (unsigned char*)&iv, 16);
		}
		uint8 iv_len = 16;

	if (!page_compression_flag) {
		tmp_page_buf = static_cast<byte *>(ut_malloc(page_size));
		tmp_buf= static_cast<byte *>(ut_malloc(64));
		memset(tmp_page_buf,0, page_size);


		/* 1st decryption: 64 bytes */
		/* 62 bytes from data area and 2 bytes from header are copied to temporary buffer */
		memcpy(tmp_buf, buf + page_size - FIL_PAGE_DATA_END -62, 62);
		memcpy(tmp_buf + 62, buf + FIL_PAGE_SPACE_OR_CHKSUM + 3, 1);
		memcpy(tmp_buf + 63, buf + page_size - FIL_PAGE_DATA_END +3, 1);

		err = my_aes_decrypt_cbc((const char*) tmp_buf,
				64,
				(char *) tmp_page_buf + page_size -FIL_PAGE_DATA_END -62,
				&tmp_write_size,
				(const unsigned char *)&rkey,
				key_len,
				(const unsigned char *)&iv,
				iv_len
				);


		/* If decrypt fails it means that page is corrupted or has an unknown key */
		if (err != AES_OK) {
			fprintf(stderr, "InnoDB: Corruption: Page is marked as encrypted\n"
					"InnoDB: but decrypt failed with error %d.\n"
					"InnoDB: size %lu len %lu, key%d\n", err, data_size,
					len, (int)page_encryption_key);
			fflush(stderr);
			if (NULL == page_buf) {
				ut_free(in_buf);
			}
			return err;
		}

		ut_ad(tmp_write_size == 63);

		/* copy 1st part from buf to tmp_page_buf */
		/* do not override result of 1st decryption */
		memcpy(tmp_page_buf + FIL_PAGE_DATA, buf + FIL_PAGE_DATA, data_size - 60);
		memset(in_buf, 0, page_size);



	} else {
		tmp_page_buf = buf;
	}

	err = my_aes_decrypt_cbc((char*) tmp_page_buf + FIL_PAGE_DATA,
			data_size,
			(char *) in_buf + FIL_PAGE_DATA,
			&tmp_write_size,
			(const unsigned char *)&rkey,
			key_len,
			(const unsigned char *)&iv,
			iv_len
			);
	ut_ad(tmp_write_size = data_size-1);

	memcpy(in_buf + FIL_PAGE_DATA + data_size -1, tmp_page_buf + page_size - FIL_PAGE_DATA_END  - 2, remainder - offset);
	if (page_compression_flag) {
		/* the last byte was stored in position 4 of the checksum header */
		memcpy(in_buf + FIL_PAGE_DATA + data_size -1 + 2, tmp_page_buf+ FIL_PAGE_SPACE_OR_CHKSUM + 3, 1);
	}


	/* calculate a checksum to verify decryption*/
	checksum = fil_page_encryption_calc_checksum(in_buf + header_len, page_size - (FIL_PAGE_DATA_END + header_len) );
	/* compare with stored checksum */
	ulint compressed_size = mach_read_from_2(in_buf+ FIL_PAGE_DATA);
	ibool no_checksum_support = 0;
	ulint pos_checksum;
	if (!page_compression_flag) {
			/* Read the checksum. This is only usable to verify decryption */
		stored_checksum = mach_read_from_3(buf + page_size - FIL_PAGE_DATA_END);
	} else {
		pos_checksum = page_size - FIL_PAGE_DATA_END;
		if (compressed_size + FIL_PAGE_DATA > pos_checksum) {
			pos_checksum = compressed_size + FIL_PAGE_DATA;
			if (pos_checksum > page_size - 3) {
				// checksum not supported, because no space available
				no_checksum_support = 1;
			} else {
				/* Read the checksum. This is only usable to verify decryption */
				stored_checksum = mach_read_from_3(buf + pos_checksum);
			}
		} else {
			/* Read the checksum. This is only usable to verify decryption */
			stored_checksum = mach_read_from_3(buf + page_size - FIL_PAGE_DATA_END);
		}
	}

	if (!page_compression_flag) {
		ut_free(tmp_page_buf);
		ut_free(tmp_buf);
	}
	if (no_checksum_support) {
		fprintf(stderr, "InnoDB: decrypting page can not be verified!\n");
		fflush(stderr);

	} else {
		if ((stored_checksum != checksum)) {
			err = PAGE_ENCRYPTION_WRONG_KEY;
			// Need to free temporal buffer if no buffer was given
			if (NULL == page_buf) {
				ut_free(in_buf);
			}
			return err;
		}
	}


#ifdef UNIV_PAGEENCRIPTION_DEBUG
	fprintf(stderr, "InnoDB: Note: Decryption succeeded for len %lu\n", len);
	fflush(stderr);
#endif /* UNIV_PAGEENCRIPTIONulint page_compressed = 0;
	_DEBUG */

	/* copy header */
	memcpy(in_buf, buf, FIL_PAGE_DATA);

	/* copy trailer */
	memcpy(in_buf + page_size - FIL_PAGE_DATA_END,
			buf + page_size - FIL_PAGE_DATA_END, FIL_PAGE_DATA_END);

	/* Copy the decrypted page to the buffer pool*/
	memcpy(buf, in_buf, page_size);

	/* setting original page type */

	mach_write_to_2(buf + FIL_PAGE_TYPE, orig_page_type);


	/* calc check sums and write to the buffer, if page was not compressed */
	if (!(page_compression_flag )) {
		do_check_sum(page_size, buf);
	} else {
		/* page_compression uses BUF_NO_CHECKSUM_MAGIC as checksum */
		mach_write_to_4(buf + FIL_PAGE_SPACE_OR_CHKSUM, BUF_NO_CHECKSUM_MAGIC);
		if (!no_checksum_support) {
			/* reset the checksum bytes - if used */
			memset(buf + pos_checksum, 0, 3);
		}

	}


	flags = mach_read_from_4(FSP_HEADER_OFFSET + FSP_SPACE_FLAGS + buf);

	if (!page_compression_flag) {

		page_size = fsp_flags_get_page_size(flags);

		page_num = mach_read_from_4(buf+ 4);
		page_encryption_key = FSP_FLAGS_GET_PAGE_ENCRYPTION_KEY(flags);
		page_encrypted = FSP_FLAGS_GET_PAGE_ENCRYPTION(flags);
		page_compression_flag = FSP_FLAGS_GET_PAGE_COMPRESSION(flags);
//		fprintf(stderr,"Page num, page size, key, enc, compr: %lu, %lu, %lu, %lu %lu\n", page_num, page_size, page_encryption_key, page_encrypted, page_compression_flag);
	}
	// Need to free temporal buffer if no buffer was given
	if (NULL == page_buf) {
		ut_free(in_buf);
	}

	srv_stats.pages_page_decrypted.inc();
	return err;
}


void do_check_sum( ulint page_size, byte* buf) {
	ib_uint32_t checksum;
	/* recalculate check sum  - from buf0flu.cc*/
	switch ((srv_checksum_algorithm_t) srv_checksum_algorithm) {
	case SRV_CHECKSUM_ALGORITHM_CRC32:
	case SRV_CHECKSUM_ALGORITHM_STRICT_CRC32:
		checksum = buf_calc_page_crc32(buf);
		break;
	case SRV_CHECKSUM_ALGORITHM_INNODB:
	case SRV_CHECKSUM_ALGORITHM_STRICT_INNODB:
		checksum = (ib_uint32_t) buf_calc_page_new_checksum(buf);
		break;
	case SRV_CHECKSUM_ALGORITHM_NONE:
	case SRV_CHECKSUM_ALGORITHM_STRICT_NONE:

		checksum = BUF_NO_CHECKSUM_MAGIC;
		break;
		/* no default so the compiler will emit a warning if new enum
		 is added and not handled here */
	}
	mach_write_to_4(buf + FIL_PAGE_SPACE_OR_CHKSUM, checksum);
	/* We overwrite the first 4 bytes of the end lsn field to store
	 the old formula checksum. Since it depends also on the field
	 FIL_PAGE_SPACE_OR_CHKSUM, it has to be calculated after storing the
	 new formula checksum. */
	if (srv_checksum_algorithm == SRV_CHECKSUM_ALGORITHM_STRICT_INNODB
			|| srv_checksum_algorithm == SRV_CHECKSUM_ALGORITHM_INNODB) {
		checksum = (ib_uint32_t) (buf_calc_page_old_checksum(buf));
		/* In other cases we use the value assigned from above.
		 If CRC32 is used then it is faster to use that checksum
		 (calculated above) instead of calculating another one.
		 We can afford to store something other than
		 buf_calc_page_old_checksum() or BUF_NO_CHECKSUM_MAGIC in
		 this field because the file will not be readable by old
		 versions of MySQL/InnoDB anyway (older than MySQL 5.6.3) */
	}
	mach_write_to_4(buf + page_size - FIL_PAGE_END_LSN_OLD_CHKSUM, checksum);
}
