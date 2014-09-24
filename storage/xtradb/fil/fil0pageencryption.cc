/*****************************************************************************

Copyright (C) 2014

eperi GmbH
*****************************************************************************/

/******************************************************************//**
@file fil/fil0pageencryption.cc
Implementation for page encryption file spaces.

Created 08/25/2014 Florin Fugaciu
***********************************************************************/

#include "fil0fil.h"
#include "fil0pageencryption.h"
#include "my_dbug.h"

#include "buf0checksum.h"

#include <my_aes.h>
#include <openssl/aes.h>


//#define UNIV_PAGEENCRIPTION_DEBUG
//#define CRYPT_FF

/* calculate a checksum to verify decryption */
ulint fil_page_encryption_calc_checksum(
		unsigned char* buf, size_t size) {
	ulint checksum = 0;
	checksum = ut_fold_binary(buf,
			size);
	checksum = checksum & 0xFFFFFFFFUL;
	return checksum;
}

/****************************************************************//**
For page encrypted pages encrypt the page before actual write
operation.
@return encrypted page to be written*/
byte*
fil_encrypt_page(
/*==============*/
    ulint		space_id,      /*!< in: tablespace id of the table. */
    byte*		buf,           /*!< in: buffer from which to write; in aio
                           this must be appropriately aligned */
    byte*		out_buf,       /*!< out: encrypted buffer */
    ulint		len,           /*!< in: length of input buffer.*/
    ulint		encryption_key,/*!< in: encryption key */
    ulint*		out_len,       /*!< out: actual length of encrypted page */
    ulint       unit_test
	)
{

	int err = AES_OK;
	int key = 0;
	ulint header_len = FIL_PAGE_DATA;
	ulint remainder = 0;
	ulint data_size = 0;
	ulint page_size = UNIV_PAGE_SIZE;
	ulint orig_page_type=0;
	ulint write_size = 0;
	ib_uint64_t flush_lsn = 0;
	ib_uint32_t checksum = 0;
	ulint page_compressed = 0;
	ulint offset_ctrl_data = 0;
	fil_space_t* space = NULL;
	byte* tmp_buf;


	ut_ad(buf); ut_ad(out_buf);
	key = encryption_key;

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

	/* read bytes 1..5 of FIL_PAGE_FILE_FLUSH_LSN */
	flush_lsn = ut_ull_create(mach_read_from_3(buf + FIL_PAGE_FILE_FLUSH_LSN), mach_read_from_2(buf + FIL_PAGE_FILE_FLUSH_LSN + 3));

	/* read original page type */
	orig_page_type = mach_read_from_2(buf + FIL_PAGE_TYPE);

	if (flush_lsn!=0x0) {
		/** currently no support because FIL_PAGE_FILE_FLUSH_LSN field is used to store control data */
		fprintf(stderr,
						"InnoDB: Warning: Encryption failed for space %lu name %s. Unsupported or unknown page type.\n",
						space_id, fil_space_name(space), len, err, write_size);

					srv_stats.pages_page_encryption_error.inc();
					*out_len = len;

		return (buf);
	}

	/* calculate a checksum, can be used to verify decryption */
	checksum = fil_page_encryption_calc_checksum(buf + header_len, page_size - (FIL_PAGE_DATA_END + header_len));


	char *stringkey = "BDE472A295675CA92E0467EADBC0E023";
	uint8 key_len = 16;
	char *stringiv = "2D1AF8D3974E0BD3EFED5A6F82594F5E";
	uint8 iv_len = 16;


		//AES_KEY aeskey;
		//AES_set_encrypt_key(hexkey, key_len*8, &aeskey);
		//AES_cbc_encrypt((uchar*)buf +header_len + offset, (uchar *)out_buf + header_len, data_size-offset, &aeskey, iv, AES_ENCRYPT);
		write_size = data_size;
		/*
		err = my_aes_encrypt_cbc((char*)buf + header_len + offset,
					data_size - offset,
					(char *) out_buf + header_len,
					&write_size,
					stringkey,
					key_len,
					stringiv,
					iv_len);

*/

	/* 1st encryption: data_size -1 bytes starting from FIL_PAGE_DATA */
	err = my_aes_encrypt_cbc((char*)buf +header_len,
			data_size-1,
			(char *)out_buf + header_len,
			&write_size,
			stringkey,
			key_len,
			stringiv,
			iv_len);;

	ut_ad(write_size == data_size);


	/* copy remaining bytes to output buffer */
	memcpy(out_buf + header_len + data_size, buf + header_len + data_size -1 , remainder);


	/* create temporary buffer for 2nd encryption */
	tmp_buf = static_cast<byte *>(ut_malloc(64));

	/* 2nd encryption: 63 bytes from out_buf, result length is 64 bytes */
	err = my_aes_encrypt_cbc((char*)out_buf + page_size -FIL_PAGE_DATA_END -62,
				63,
				(char*)tmp_buf,
				&write_size,
				stringkey,
				key_len,
				stringiv,
				iv_len);
	ut_ad(write_size == 64);
	//AES_cbc_encrypt((uchar*)out_buf + page_size -FIL_PAGE_DATA_END -62, tmp_buf, 63, &aeskey, iv, AES_ENCRYPT);
	/* copy 62 bytes from 2nd encryption to out_buf, last 2 bytes are copied later to a header field*/
	memcpy(out_buf + page_size - FIL_PAGE_DATA_END -62, tmp_buf, 62);

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
		ut_free(tmp_buf);


		return (buf);
	}


	/* Set up the page header. Copied from input buffer*/
	memcpy(out_buf, buf, FIL_PAGE_DATA);


	/* Set up the checksum. This is only usable to verify decryption */
	mach_write_to_4(out_buf + FIL_PAGE_SPACE_OR_CHKSUM, checksum);



	/* Set up the correct page type */
	mach_write_to_2(out_buf + FIL_PAGE_TYPE, FIL_PAGE_PAGE_ENCRYPTED);


	/* set up the trailer. The old style checksum is not used because
	 * in case of a compressed page, this data may be compressed or unused! */
	memcpy(out_buf + (page_size -FIL_PAGE_DATA_END), buf + (page_size - FIL_PAGE_DATA_END), FIL_PAGE_DATA_END);


	offset_ctrl_data = page_size - FIL_PAGE_DATA_END - FIL_PAGE_FILE_FLUSH_LSN;


	/* Set up the encryption key. Written to the 1st byte of FIL_PAGE_FILE_FLUSH_LSN */
	mach_write_to_1(out_buf+ page_size-FIL_PAGE_DATA_END - offset_ctrl_data, key);


	/* store original page type. Written to 2nd and 3rd byte of the FIL_PAGE_FILE_FLUSH_LSN */
	mach_write_to_2(out_buf+page_size-FIL_PAGE_DATA_END +1  - offset_ctrl_data, orig_page_type);

	/* write remaining bytes to FIL_PAGE_FILE_FLUSH_LSN byte 4 and 5 */
	memcpy(out_buf+ page_size -FIL_PAGE_DATA_END + 3 - offset_ctrl_data, tmp_buf+ 62, 2);




#ifdef UNIV_DEBUG
	/* Verify */
	ut_ad(fil_page_is_encrypted(out_buf));

	//ut_ad(mach_read_from_8(out_buf+FIL_PAGE_FILE_FLUSH_LSN) == FIL_PAGE_COMPRESSION_ZLIB);

#endif /* UNIV_DEBUG */


	srv_stats.pages_page_encrypted.inc();
	*out_len = page_size;

	/* free temporary buffer */
	ut_free(tmp_buf);

	return (out_buf);
}



/****************************************************************//**
For page encrypted pages decrypt the page after actual read
operation.
@return decrypted page */
void
fil_decrypt_page(
/*================*/
	byte*		page_buf,      /*!< in: preallocated buffer or NULL */
	byte*		buf,           /*!< in/out: buffer from which to read; in aio
	                       this must be appropriately aligned */
	ulint		len,           /*!< in: length of output buffer.*/
    ulint*		write_size,   /*!< in/out: Actual payload size of the decrypted data. */
    ulint       unit_test)
{
	int err = AES_OK;
	ulint page_encryption_key;

	ulint data_size = 0;
	ulint page_size = UNIV_PAGE_SIZE;
	ulint orig_page_type=0;
	ulint remaining_bytes = 0;

	ulint header_len = FIL_PAGE_DATA;
	ulint remainder = 0;
	ulint offset = 1;
	ulint offset_ctrl_data = 0;
	ulint flush_lsn = 0;
	ulint page_compressed = 0;
	ulint checksum = 0;
	ulint stored_checksum = 0;
	ulint tmp_write_size = 0;

	byte * in_buf;
	byte * tmp_buf;
	byte * tmp_page_buf;

	ut_ad(buf); ut_ad(len);

	/* Before actual decrypt, make sure that page type is correct */

	if (mach_read_from_2(buf+FIL_PAGE_TYPE) != FIL_PAGE_PAGE_ENCRYPTED) {
		fprintf(stderr,
				"InnoDB: Corruption: We try to decrypt corrupted page\n"
						"InnoDB: CRC %lu type %lu.\n"
						"InnoDB: len %lu\n",
				mach_read_from_4(buf + FIL_PAGE_SPACE_OR_CHKSUM),
				mach_read_from_2(buf + FIL_PAGE_TYPE), len);

		fflush(stderr);
		ut_error
		;
	}


	/* flush lsn bytes 1..5 are used to store original page type, etc.*/

	offset_ctrl_data = page_size - FIL_PAGE_DATA_END - FIL_PAGE_FILE_FLUSH_LSN;

	/* Get encryption key */
	page_encryption_key = mach_read_from_1(buf + page_size -FIL_PAGE_DATA_END - offset_ctrl_data);

	/* Get the page type */
	orig_page_type = mach_read_from_2(buf + page_size -FIL_PAGE_DATA_END + 1 - offset_ctrl_data);


	if (FIL_PAGE_PAGE_COMPRESSED==orig_page_type) {
		page_compressed = 1;
	}

// If no buffer was given, we need to allocate temporal buffer
	if (NULL == page_buf) {
#ifdef UNIV_PAGEENCRIPTION_DEBUG
		fprintf(stderr,
				"InnoDB: FIL: Note: Decryption buffer not given, allocating...\n");
		fflush(stderr);
#endif /* UNIV_PAGEENCRIPTION_DEBUG */
		in_buf = static_cast<byte *>(ut_malloc(len+16));
		//in_buf = static_cast<byte *>(ut_malloc(UNIV_PAGE_SIZE));
	} else {
		in_buf = page_buf;
	}


	data_size = ((page_size - header_len - FIL_PAGE_DATA_END) / 16) * 16;
	remainder = (page_size - header_len - FIL_PAGE_DATA_END) - (data_size - 1);




#ifdef UNIV_PAGEENCRIPTION_DEBUG
	fprintf(stderr,
			"InnoDB: Note: Preparing for decrypt for len %lu\n", actual_size);
	fflush(stderr);
#endif /* UNIV_PAGEENCRIPTION_DEBUG */



	tmp_buf= static_cast<byte *>(ut_malloc(64));
	tmp_page_buf = static_cast<byte *>(ut_malloc(page_size));
	memset(tmp_page_buf,0, page_size);
		char *stringkey = "BDE472A295675CA92E0467EADBC0E023";
		uint8 key_len = 16;

		char *stringiv = "2D1AF8D3974E0BD3EFED5A6F82594F5E";
		uint8 iv_len = 16;

		/* 1st decryption: 64 bytes */
		/* 62 bytes from data area and 2 bytes from header are copied to temporary buffer */
		memcpy(tmp_buf, buf + page_size - FIL_PAGE_DATA_END -62, 62);
		memcpy(tmp_buf + 62, buf + FIL_PAGE_FILE_FLUSH_LSN +3, 2);
		err = my_aes_decrypt_cbc((const char*) tmp_buf,
				64,
				(char *) tmp_page_buf + page_size -FIL_PAGE_DATA_END -62,
				&tmp_write_size,
				stringkey,
				key_len,
				stringiv,
				iv_len
				);


		/* If decrypt fails it means that page is corrupted or has an unknown key */
		if (err != AES_OK) {
			fprintf(stderr, "InnoDB: Corruption: Page is marked as encrypted\n"
					"InnoDB: but decrypt failed with error %d.\n"
					"InnoDB: size %lu len %lu, key%d\n", err, data_size,
					len, page_encryption_key);
			fflush(stderr);
			ut_error;
		}

		ut_ad(tmp_write_size == 63);

		/* copy 1st part from buf to tmp_page_buf */
		/* do not override result of 1st decryption */
		memcpy(tmp_page_buf + FIL_PAGE_DATA, buf + FIL_PAGE_DATA, data_size - 60);
		memset(in_buf, 0, page_size);



		err = my_aes_decrypt_cbc((char*) tmp_page_buf + FIL_PAGE_DATA,
				data_size,
				(char *) in_buf + FIL_PAGE_DATA,
				&tmp_write_size,
				stringkey,
				key_len,
				stringiv,
				iv_len
				);
		ut_ad(tmp_write_size = data_size-1);
		memcpy(in_buf + FIL_PAGE_DATA + data_size -1, tmp_page_buf + page_size - FIL_PAGE_DATA_END  - 2, remainder);


		/* calculate a checksum to verify decryption*/
		checksum = fil_page_encryption_calc_checksum(in_buf + header_len, page_size - (FIL_PAGE_DATA_END + header_len) );
		/* compare with stored checksum */
		stored_checksum = mach_read_from_4(buf);

		ut_free(tmp_page_buf);
		ut_free(tmp_buf);

		if (checksum != stored_checksum) {
			err = PAGE_ENCRYPTION_WRONG_KEY;
		}

	/* If decrypt fails it means that page is corrupted or has an unknown key */
	if (err != AES_OK) {
		fprintf(stderr, "InnoDB: Corruption: Page is marked as encrypted\n"
				"InnoDB: but decrypt failed with error %d.\n"
				"InnoDB: size %lu len %lu, key%d\n", err, data_size,
				len, page_encryption_key);
		fflush(stderr);
		ut_error;
	}

#ifdef UNIV_PAGEENCRIPTION_DEBUG
	fprintf(stderr,	"InnoDB: Note: Decryption succeeded for len %lu\n", len);
	fflush(stderr);
#endif /* UNIV_PAGEENCRIPTION_DEBUG */



	/* copy header */
	memcpy(in_buf, buf, FIL_PAGE_DATA);

	/* copy trailer */
	memcpy(in_buf + page_size - FIL_PAGE_DATA_END, buf + page_size - FIL_PAGE_DATA_END, FIL_PAGE_DATA_END);

	/* Copy the decrypted page to the buffer pool*/
	memcpy(buf , in_buf, page_size);

	/* setting original page type */

	mach_write_to_2(buf + FIL_PAGE_TYPE , orig_page_type);


	/* fill flush lsn bytes 1..5 with 0 because this header field was used to store control data
	 */
	mach_write_to_4(buf+FIL_PAGE_FILE_FLUSH_LSN, 0);
	mach_write_to_1(buf+FIL_PAGE_FILE_FLUSH_LSN + 4, 0);


	/* calc check sums and write to the buffer, if page was not compressed */
	if (!page_compressed) {
		do_check_sum(page_size, buf);
	} else {
		/* page_compression uses BUF_NO_CHECKSUM_MAGIC as checksum */
		mach_write_to_4(buf+FIL_PAGE_SPACE_OR_CHKSUM, BUF_NO_CHECKSUM_MAGIC);

	}


	// Need to free temporal buffer if no buffer was given
	if (NULL == page_buf) {
		ut_free(in_buf);
	}

	srv_stats.pages_page_decrypted.inc();

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
