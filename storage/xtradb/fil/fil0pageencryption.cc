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
#include "page0zip.h"

#include "buf0checksum.h"

#include <my_aes.h>
#include <KeySingleton.h>


/*
 * derived from libFLAC, which is gpl v2
 */
byte crc_table[] = {
		0x00,0x07,0x0E,0x09,0x1C,0x1B,0x12,0x15,0x38,0x3F,0x36,0x31,0x24,0x23,0x2A,0x2D,0x70,0x77,0x7E,0x79,
		0x6C,0x6B,0x62,0x65,0x48,0x4F,0x46,0x41,0x54,0x53,0x5A,0x5D,0xE0,0xE7,0xEE,0xE9,0xFC,0xFB,0xF2,0xF5,
		0xD8,0xDF,0xD6,0xD1,0xC4,0xC3,0xCA,0xCD,0x90,0x97,0x9E,0x99,0x8C,0x8B,0x82,0x85,0xA8,0xAF,0xA6,0xA1,
		0xB4,0xB3,0xBA,0xBD,0xC7,0xC0,0xC9,0xCE,0xDB,0xDC,0xD5,0xD2,0xFF,0xF8,0xF1,0xF6,0xE3,0xE4,0xED,0xEA,
		0xB7,0xB0,0xB9,0xBE,0xAB,0xAC,0xA5,0xA2,0x8F,0x88,0x81,0x86,0x93,0x94,0x9D,0x9A,0x27,0x20,0x29,0x2E,
		0x3B,0x3C,0x35,0x32,0x1F,0x18,0x11,0x16,0x03,0x04,0x0D,0x0A,0x57,0x50,0x59,0x5E,0x4B,0x4C,0x45,0x42,
		0x6F,0x68,0x61,0x66,0x73,0x74,0x7D,0x7A,0x89,0x8E,0x87,0x80,0x95,0x92,0x9B,0x9C,0xB1,0xB6,0xBF,0xB8,
		0xAD,0xAA,0xA3,0xA4,0xF9,0xFE,0xF7,0xF0,0xE5,0xE2,0xEB,0xEC,0xC1,0xC6,0xCF,0xC8,0xDD,0xDA,0xD3,0xD4,
		0x69,0x6E,0x67,0x60,0x75,0x72,0x7B,0x7C,0x51,0x56,0x5F,0x58,0x4D,0x4A,0x43,0x44,0x19,0x1E,0x17,0x10,
		0x05,0x02,0x0B,0x0C,0x21,0x26,0x2F,0x28,0x3D,0x3A,0x33,0x34,0x4E,0x49,0x40,0x47,0x52,0x55,0x5C,0x5B,
		0x76,0x71,0x78,0x7F,0x6A,0x6D,0x64,0x63,0x3E,0x39,0x30,0x37,0x22,0x25,0x2C,0x2B,0x06,0x01,0x08,0x0F,
		0x1A,0x1D,0x14,0x13,0xAE,0xA9,0xA0,0xA7,0xB2,0xB5,0xBC,0xBB,0x96,0x91,0x98,0x9F,0x8A,0x8D,0x84,0x83,
		0xDE,0xD9,0xD0,0xD7,0xC2,0xC5,0xCC,0xCB,0xE6,0xE1,0xE8,0xEF,0xFA,0xFD,0xF4,0xF3

};

/* this calculates a crc-8 checksum byte */
byte fil_page_encryption_calc_checksum(unsigned char* buf, ulint len) {
       byte crc = 0;
       for (ulint i = 0; i < len; i++)
           crc = crc_table[(crc ^ buf[i]) & 0xff];
        return crc;
}

/****************************************************************//**
 For page encrypted pages encrypt the page before actual write
 operation.

 Note, that FIL_PAGE_TYPE_FSP_HDR and FIL_PAGE_TYPE_XDES type pages are not encrypted!

 Pages are encrypted with AES/CBC/NoPadding algorithm.

 "No padding" is used to ensure, that the encrypted page does not exceed the page size.
 If "no padding" is used, the input for encryption must be of size (multiple * AES blocksize). AES Blocksize is usually 16 (bytes).

 Everything in the page is encrypted except for the 38 byte FIL header.
 Since the length of the payload is not a multiple of the AES blocksize,
 and to ensure that every byte of the payload is encrypted, two encryption operations are done.
 Each time with a block of adequate size as input.
 1st block contains everything from beginning of payload bytes except for the remainder.
 2nd block is of size 64 and contains the remainder and the last (64 - sizeof(remainder)) bytes of the encrypted 1st block.

 Each encrypted page receives a new page type for PAGE_ENCRYPTION.
 The original page type (2 bytes) is stored in the Checksum header of the page (position FIL_PAGE_SPACE_OR_CHKSUM).
 Additionally the encryption key identifier is stored in the Checksum Header. This uses 1 byte.
 Checksum verification for encrypted pages is disabled. This checksum should be restored after decryption.

 To be able to verify decryption in a later stage, a 1-byte checksum at position 4 of the FIL_PAGE_SPACE_OR_CHKSUM header is stored.


 @return encrypted page to be written*/
byte*
fil_encrypt_page(
/*==============*/
		ulint 		space_id, 	/*!< in: tablespace id of the table. */
		byte* 		buf, 		/*!< in: buffer from which to write; in aio
 	 	 	 	 	 	 			this must be appropriately aligned */
		byte* 		out_buf, 	/*!< out: encrypted buffer */
		ulint 		len, 		/*!< in: length of input buffer.*/
		ulint 		encryption_key,/*!< in: encryption key */
		ulint* 		out_len, 	/*!< out: actual length of encrypted page */
		ulint* 	    errorCode,  /*!< out: an error code. set, if page is intentionally not encrypted */
		ulint 		mode 		/*!< in: calling mode. Should be 0. Can be used for unit tests */
) {

	int err = AES_OK;
	int key = 0;
	ulint data_size = 0;
	ulint orig_page_type = 0;
	ulint write_size = 0;
	fil_space_t* space = NULL;
	byte* tmp_buf = NULL;
	ulint unit_test = 0;
	ut_ad(buf);ut_ad(out_buf);
	key = encryption_key;
	ulint offset = 0;
	unit_test = mode ? 1 : 0;

	*errorCode = AES_OK;

	if (!unit_test) {
		ut_ad(fil_space_is_page_encrypted(space_id));
		fil_system_enter();
		space = fil_space_get_by_id(space_id);
		fil_system_exit();

#ifdef UNIV_DEBUG
		ulint pageno = mach_read_from_4(buf + FIL_PAGE_OFFSET);
	
		fprintf(stderr,
				"InnoDB: Note: Preparing for encryption for space %lu name %s len %lu, page no %lu\n",
				space_id, fil_space_name(space), len, pageno);
#endif /* UNIV_DEBUG */
	}
	/* read original page type */
	orig_page_type = mach_read_from_2(buf + FIL_PAGE_TYPE);

	if ((orig_page_type == FIL_PAGE_TYPE_FSP_HDR) || (orig_page_type == FIL_PAGE_TYPE_XDES) ) {
			memcpy(out_buf, buf, len);

			*errorCode = PAGE_ENCRYPTION_WILL_NOT_ENCRYPT;
			return (out_buf);
	}

	byte checksum_byte = fil_page_encryption_calc_checksum(buf + FIL_PAGE_DATA, len - FIL_PAGE_DATA);

	/* data_size bytes will be encrypted at first.
	 * data_size will be the length of the cipher text since no padding is used.*/
	data_size = ((len - FIL_PAGE_DATA - FIL_PAGE_DATA_END) / MY_AES_BLOCK_SIZE) * MY_AES_BLOCK_SIZE;





	const unsigned char rkey[] = {0xbd, 0xe4, 0x72, 0xa2, 0x95, 0x67, 0x5c, 0xa9,
								  0x2e, 0x04, 0x67, 0xea, 0xdb, 0xc0, 0xe0, 0x23,
								  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
								  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	uint8 key_len = 16;
	if (!unit_test) {
		KeySingleton& keys = KeySingleton::getInstance();
		if (!keys.isAvailable()) {
			err = AES_KEY_CREATION_FAILED;
		} else if (keys.getKeys(encryption_key) == NULL) {
			err = PAGE_ENCRYPTION_KEY_MISSING;
		} else {
			char* keyString = keys.getKeys(encryption_key)->key;
			key_len = strlen(keyString)/2;
			my_aes_hexToUint(keyString, (unsigned char*)&rkey, key_len);
		}
	}
	const unsigned char iv[] = {0x2d, 0x1a, 0xf8, 0xd3, 0x97, 0x4e, 0x0b, 0xd3, 0xef, 0xed,
		0x5a, 0x6f, 0x82, 0x59, 0x4f,0x5e};
	ulint iv_len = 16;
	if (!unit_test) {
		KeySingleton& keys = KeySingleton::getInstance();
		if (!keys.isAvailable()) {
			err = AES_KEY_CREATION_FAILED;
		} else if (keys.getKeys(encryption_key) == NULL) {
			err = PAGE_ENCRYPTION_KEY_MISSING;
		} else {
			char* ivString = keys.getKeys(encryption_key)->iv;
			if (ivString == NULL) return buf;
			my_aes_hexToUint(ivString, (unsigned char*)&iv, 16);
		}
	}

	/* 1st encryption: data_size bytes starting from FIL_PAGE_DATA */
	if (err == AES_OK) {
		err = my_aes_encrypt_cbc((char*) buf + FIL_PAGE_DATA, data_size,
				(char *) out_buf + FIL_PAGE_DATA, &write_size,
				(const unsigned char *) &rkey, key_len,
				(const unsigned char *) &iv, iv_len, 1);
		ut_ad(write_size == data_size);
		if (err == AES_OK) {
			/* copy remaining bytes from input buffer to output buffer.
			 * Note, that this copies the final 8 bytes of a page, which consists of the
			 * Old-style checksum and the "Low 32 bits of LSN */
			memcpy(out_buf + FIL_PAGE_DATA + data_size , buf + FIL_PAGE_DATA + data_size , len - FIL_PAGE_DATA -data_size);


			//create temporary buffer for 2nd encryption
			tmp_buf = static_cast<byte *>(ut_malloc(64));
			/* 2nd encryption: 63 bytes from out_buf, result length is 64 bytes */
			err = my_aes_encrypt_cbc((char*)out_buf + len -offset -64,
					64,
					(char*)tmp_buf,
					&write_size,
					(const unsigned char *)&rkey,
					key_len,
					(const unsigned char *)&iv,
					iv_len, 1);
			ut_ad(write_size == 64);
			/* copy 64 bytes from 2nd encryption to out_buf*/
			memcpy(out_buf + len - offset -64, tmp_buf, 64);
		}

	}
	/* error handling */
	if (err != AES_OK) {
		/* If an error occurred we leave the actual page as it was */

		fprintf(stderr,
				"InnoDB: Warning: Encryption failed for space %lu name %s len %lu rt %d write %lu, error: %d\n",
			space_id, fil_space_name(space), len, err, data_size, err);
		fflush(stderr);
		srv_stats.pages_page_encryption_error.inc();
		*out_len = len;

		/* free temporary buffer */
		if (tmp_buf!=NULL) {
			ut_free(tmp_buf);
		}
		*errorCode = err;

		return (buf);
	}



	/* Set up the page header. Copied from input buffer*/
	memcpy(out_buf, buf, FIL_PAGE_DATA);


	/* Set up the correct page type */
	mach_write_to_2(out_buf + FIL_PAGE_TYPE, FIL_PAGE_PAGE_ENCRYPTED);

	/* The 1st checksum field is used to store original page type, etc.
	 * checksum check for page encrypted pages is omitted.
	 */

	/* Set up the encryption key. Written to the 1st byte of the checksum header field. This header is currently used to store data. */
	mach_write_to_1(out_buf + FIL_PAGE_SPACE_OR_CHKSUM, key);

	/* store original page type. Written to 2nd and 3rd byte of the checksum header field */
	mach_write_to_2(out_buf + FIL_PAGE_SPACE_OR_CHKSUM + 1, orig_page_type);

	/* set byte 4 of checksum field to checksum byte */
	memset(out_buf + FIL_PAGE_SPACE_OR_CHKSUM + 3, checksum_byte, 1);

#ifdef UNIV_DEBUG
	/* Verify */
	ut_ad(fil_page_is_encrypted(out_buf));

#endif /* UNIV_DEBUG */

	srv_stats.pages_page_encrypted.inc();
	*out_len = len;

	/* free temporary buffer */
	if (tmp_buf!=NULL) {
		ut_free(tmp_buf);
	}
	return (out_buf);
}

/****************************************************************//**
 For page encrypted pages decrypt the page after actual read
 operation.

 See fil_encrypt_page for details, how the encryption works.

 If the decryption can be verified, original page should be completely restored.
 This includes original page type, 4-byte checksum field at page start.
 Decryption is verified against a 1-byte checksum built over the plain data bytes. If this verification fails, an error state is returned..


 @return decrypted page */
ulint fil_decrypt_page(
/*================*/
		byte* 		page_buf, 	/*!< in: preallocated buffer or NULL */
		byte* 		buf, 		/*!< in/out: buffer from which to read; in aio
 	 	 	 	 	 	 	 	 this must be appropriately aligned */
		ulint 		len, 		/*!< in: length buffer, which should be decrypted.*/
		ulint* 		write_size, /*!< out: size of the decrypted data. If no error occurred equal to len */
		ibool* 		page_compressed, /*!<out: is page compressed.*/
		ulint 		mode		/*!< in: calling mode. Should be 0. Can be used for unit tests */
) {
	int err = AES_OK;
	ulint page_decryption_key;
	ulint data_size = 0;
	ulint orig_page_type = 0;
	ulint tmp_write_size = 0;
	ulint offset = 0;
	byte * in_buffer;
	byte * in_buf;
	byte * tmp_buf;
	byte * tmp_page_buf;
	fil_space_t* space = NULL;

	ulint page_compression_flag = 0;
	ulint unit_test = mode ? 0x01: 0;

	ut_ad(buf);
	ut_ad(len);

	/* Before actual decrypt, make sure that page type is correct */
	ulint current_page_type = mach_read_from_2(buf + FIL_PAGE_TYPE);
	if ((current_page_type == FIL_PAGE_TYPE_FSP_HDR) || (current_page_type == FIL_PAGE_TYPE_XDES)) {
		/* assumed as unencrypted */
		if (write_size!=NULL)  {
			* write_size = len;
		}
		return AES_OK;
	}
	if (current_page_type != FIL_PAGE_PAGE_ENCRYPTED)
	{
		if (mach_read_from_2(buf + FIL_PAGE_TYPE) != FIL_PAGE_PAGE_ENCRYPTED)
		fprintf(stderr, "InnoDB: Corruption: We try to decrypt corrupted page\n"
				"InnoDB: CRC %lu type %lu.\n"
				"InnoDB: len %lu\n",
				mach_read_from_4(buf + FIL_PAGE_SPACE_OR_CHKSUM),
				mach_read_from_2(buf + FIL_PAGE_TYPE), len);

		fflush(stderr);
		return PAGE_ENCRYPTION_WRONG_PAGE_TYPE;
	}


	/* 1st checksum field is used to store original page type, etc.
	 * checksum check for page encrypted pages is omitted.
	 */


	/* read page encryption key */
	page_decryption_key = mach_read_from_1(buf + FIL_PAGE_SPACE_OR_CHKSUM);


	/* Get the page type */
	orig_page_type = mach_read_from_2(buf + FIL_PAGE_SPACE_OR_CHKSUM + 1);

	/* read checksum byte */
	byte stored_checksum_byte = mach_read_from_1(buf + FIL_PAGE_SPACE_OR_CHKSUM + 3);

	if (FIL_PAGE_PAGE_COMPRESSED == orig_page_type) {
		if (page_compressed != NULL) {
			*page_compressed = 1L;
		}
		page_compression_flag = 1;
		offset = 0;
	}

// If no buffer was given, we need to allocate temporal buffer
	if (NULL == page_buf) {
#ifdef UNIV_PAGEENCRIPTION_DEBUG
		fprintf(stderr,
				"InnoDB: FIL: Note: Decryption buffer not given, allocating...\n");
		fflush(stderr);
#endif /* UNIV_PAGEENCRIPTION_DEBUG */
		/* it was requested to align this buffer */
		in_buffer = static_cast<byte*>(ut_malloc(2 * (len)));
		in_buf = static_cast<byte*>(ut_align(in_buffer, len));

	} else {
		in_buf = page_buf;
	}
	data_size = ((len - FIL_PAGE_DATA - FIL_PAGE_DATA_END) / MY_AES_BLOCK_SIZE) * MY_AES_BLOCK_SIZE;

	const unsigned char rkey[] = {0xbd, 0xe4, 0x72, 0xa2, 0x95, 0x67, 0x5c, 0xa9,
			0x2e, 0x04, 0x67, 0xea, 0xdb, 0xc0,0xe0, 0x23,
			0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
			0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
	uint8 key_len = 16;
	if (!unit_test) {
		KeySingleton& keys = KeySingleton::getInstance();
		if (!keys.isAvailable()) {
			err = PAGE_ENCRYPTION_ERROR;
		} else if (keys.getKeys(page_decryption_key) == NULL) {
			err = PAGE_ENCRYPTION_KEY_MISSING;
		} else {
			char* keyString = keys.getKeys(page_decryption_key)->key;
			key_len = strlen(keyString)/2;
			my_aes_hexToUint(keyString, (unsigned char*)&rkey, key_len);
		}
	}


	const unsigned char iv[] = {0x2d, 0x1a, 0xf8, 0xd3, 0x97, 0x4e, 0x0b, 0xd3, 0xef, 0xed,
			0x5a, 0x6f, 0x82, 0x59, 0x4f,0x5e};


	uint8 iv_len = 16;
	if (!unit_test) {
		KeySingleton& keys = KeySingleton::getInstance();
		if (!keys.isAvailable()) {
			err =  PAGE_ENCRYPTION_ERROR;
		} else if (keys.getKeys(page_decryption_key) == NULL) {
			err = PAGE_ENCRYPTION_KEY_MISSING;
		} else {
			my_aes_hexToUint(keys.getKeys(page_decryption_key)->iv, (unsigned char*)&iv, 16);
		}
	}


	if (err != AES_OK) {
		/* surely key could not be determined. */
		fprintf(stderr, "InnoDB: Corruption: Page is marked as encrypted\n"
				"InnoDB: but decrypt failed with error %d, encryption key %d.\n",
				err, (int)page_decryption_key);
		fflush(stderr);
		if (NULL == page_buf) {
			ut_free(in_buffer);
		}
		return err;
	}


	tmp_page_buf = static_cast<byte *>(ut_malloc(len));
	tmp_buf= static_cast<byte *>(ut_malloc(64));
	memset(tmp_page_buf,0, len);


	/* 1st decryption: 64 bytes */
	/* 64 bytes from data area are copied to temporary buffer.
	 * These are the last 64 of the (encrypted) payload */
	memcpy(tmp_buf, buf + len - offset - 64, 64);
	if (err == AES_OK) {
		err = my_aes_decrypt_cbc((const char*) tmp_buf, 64,
				(char *) tmp_page_buf + len - offset - 64,
				&tmp_write_size, (const unsigned char *) &rkey, key_len,
				(const unsigned char *) &iv, iv_len, 1);
	}
	ut_ad(tmp_write_size == 64);


	/* If decrypt fails it means that page is corrupted or has an unknown key */
	if (err != AES_OK) {
		fprintf(stderr, "InnoDB: Corruption: Page is marked as encrypted\n"
				"InnoDB: but decrypt failed with error %d.\n"
				"InnoDB: size %lu len %lu, key %d\n", err, data_size,
				len, (int)page_decryption_key);
		fflush(stderr);
		if (NULL == page_buf) {
			ut_free(in_buffer);
		}
		ut_free(tmp_page_buf);
		ut_free(tmp_buf);

		return err;
	}

	ut_ad(tmp_write_size == 64);

	/* copy 1st part of payload from buf to tmp_page_buf */
	/* do not override result of 1st decryption */
	memcpy(tmp_page_buf + FIL_PAGE_DATA, buf + FIL_PAGE_DATA, len -offset -64 - FIL_PAGE_DATA);


	/* fill target buffer with zeros */
	memset(in_buf, 0, len);




	err = my_aes_decrypt_cbc((char*) tmp_page_buf + FIL_PAGE_DATA,
			data_size,
			(char *) in_buf + FIL_PAGE_DATA,
			&tmp_write_size,
			(const unsigned char *)&rkey,
			key_len,
			(const unsigned char *)&iv,
			iv_len,
			1);
	ut_ad(tmp_write_size = data_size);

	/* copy remaining bytes from tmp_page_buf to in_buf.
	 */
	ulint bytes_to_copy = len - FIL_PAGE_DATA - data_size - offset;
	memcpy(in_buf + FIL_PAGE_DATA + data_size, tmp_page_buf + FIL_PAGE_DATA + data_size, bytes_to_copy);

	/* apart from header data everything is now in in_buf */






	ut_free(tmp_page_buf);
	ut_free(tmp_buf);



#ifdef UNIV_PAGEENCRIPTION_DEBUG
	fprintf(stderr, "InnoDB: Note: Decryption succeeded for len %lu\n", len);
	fflush(stderr);
#endif

	/* copy header */
	memcpy(in_buf, buf, FIL_PAGE_DATA);


	/* Copy the decrypted page to the buffer pool*/
	memcpy(buf, in_buf, len);

	if (NULL == page_buf) {
		ut_free(in_buffer);
	}

	/* setting original page type */

	mach_write_to_2(buf + FIL_PAGE_TYPE, orig_page_type);



	ulint pageno = mach_read_from_4(buf + FIL_PAGE_OFFSET);
	ulint flags = 0;
	ulint zip_size = 0;
	/* please note, that pane with number 0 is not encrypted */
	if (pageno == 0 ) {

		flags = mach_read_from_4(FSP_HEADER_OFFSET + FSP_SPACE_FLAGS + buf);
	} else {
		if (unit_test) {
			/* in simple unit test, the tablespace memory cache is n.a. */
			if ((mode & 0x01) != 0x01) {
				zip_size = mode;
			}
		} else {
			ulint space_id = mach_read_from_4(buf + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID);
			fil_system_enter();
			space = fil_space_get_by_id(space_id);
			flags = fil_space_flags(space);
			fil_system_exit();
		}
	}
	if (!unit_test || pageno==0) {
		zip_size = fsp_flags_get_zip_size(flags);
	}

	if (write_size!=NULL) {
			*write_size = len;
		}


	byte checksum_byte = fil_page_encryption_calc_checksum(buf + FIL_PAGE_DATA, len - FIL_PAGE_DATA);
	if (checksum_byte != stored_checksum_byte) {
		err = PAGE_ENCRYPTION_WRONG_KEY;
		fprintf(stderr, "InnoDB: Corruption: Page is marked as encrypted\n"
						"InnoDB: but decryption verification failed with error %d, encryption key %d.\n",
						err, (int)page_decryption_key);
		fflush(stderr);

		return err;
	}

	if (!(page_compression_flag)) {
		/* calc check sums and write to the buffer, if page is not of type PAGE_COMPRESSED.
		 * if the decryption is verified, it is assumed that the original page was restored, re-calculating the original
		 * checksums should be ok
		 */
		do_check_sum(len, zip_size, buf);
	} else {
		/* page_compression uses BUF_NO_CHECKSUM_MAGIC as checksum */
		mach_write_to_4(buf + FIL_PAGE_SPACE_OR_CHKSUM, BUF_NO_CHECKSUM_MAGIC);
	}




	srv_stats.pages_page_decrypted.inc();
	return err;
}


/* recalculate check sum  - from buf0flu.cc*/
void do_check_sum(
		ulint 	page_size,
		ulint 	zip_size,
		byte* 	buf) {
	ib_uint32_t checksum = 0;

	if (zip_size) {
				checksum = page_zip_calc_checksum(buf,zip_size,
				static_cast<srv_checksum_algorithm_t>(
								srv_checksum_algorithm));

				mach_write_to_4(buf + FIL_PAGE_SPACE_OR_CHKSUM, checksum);
				return;
	}

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

	/* old style checksum is omitted */

}
