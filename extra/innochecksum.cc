/*
   Copyright (c) 2005, 2016, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2014, 2021, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/

/*
  InnoDB offline file checksum utility.  85% of the code in this utility
  is included from the InnoDB codebase.

  The final 15% was originally written by Mark Smith of Danga
  Interactive, Inc. <junior@danga.com>

  Published with a permission.
*/

#include <my_global.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifndef __WIN__
# include <unistd.h>
#endif
#include <my_getopt.h>
#include <m_string.h>
#include <welcome_copyright_notice.h> /* ORACLE_WELCOME_COPYRIGHT_NOTICE */

/* Only parts of these files are included from the InnoDB codebase.
The parts not included are excluded by #ifndef UNIV_INNOCHECKSUM. */

typedef void fil_space_t;

#include "page0size.h"

#define FLST_BASE_NODE_SIZE (4 + 2 * FIL_ADDR_SIZE)
#define FLST_NODE_SIZE (2 * FIL_ADDR_SIZE)
#define FSEG_PAGE_DATA FIL_PAGE_DATA
#define FSEG_HEADER_SIZE	10
#define UT_BITS_IN_BYTES(b) (((b) + 7) / 8)

#include "ut0ut.h"
#include "ut0byte.h"
#include "mtr0types.h"
#include "mach0data.h"
#include "fsp0types.h"
#include "rem0rec.h"
#include "buf0checksum.h"        /* buf_calc_page_*() */
#include "buf0buf.h"             /* buf_page_is_corrupted */
#include "fil0fil.h"             /* FIL_* */
#include "page0page.h"           /* PAGE_* */
#include "page0zip.h"            /* page_zip_*() */
#include "trx0undo.h"            /* TRX_* */
#include "fsp0fsp.h"             /* fsp_flags_get_page_size() &
                                    fsp_flags_get_zip_size() */
#include "ut0crc32.h"            /* ut_crc32_init() */
#include "fsp0pagecompress.h"    /* fil_get_compression_alg_name */
#include "fil0crypt.h"           /* fil_space_verify_crypt_checksum */

#include <string.h>

#ifdef UNIV_NONINL
# include "fsp0fsp.inl"
# include "mach0data.inl"
# include "ut0rnd.inl"
#endif

#ifndef PRIuMAX
#define PRIuMAX   "llu"
#endif

/* Global variables */
static bool			verbose;
static bool			just_count;
static uint32_t			start_page;
static uint32_t			end_page;
static uint32_t			do_page;
static bool			use_end_page;
static bool			do_one_page;
static my_bool do_leaf;
static my_bool per_page_details;
static ulint n_merge;
extern ulong			srv_checksum_algorithm;
static ulong physical_page_size;  /* Page size in bytes on disk. */
static ulong logical_page_size;   /* Page size when uncompressed. */
ulong srv_page_size;
page_size_t			univ_page_size(0, 0, false);
/* Current page number (0 based). */
uint32_t		cur_page_num;
/* Current space. */
uint32_t		cur_space;
/* Skip the checksum verification. */
static bool			no_check;
/* Enabled for strict checksum verification. */
bool				strict_verify = 0;
/* Enabled for rewrite checksum. */
static bool			do_write;
/* Mismatches count allowed (0 by default). */
static unsigned long long     	allow_mismatches=0;
static bool			page_type_summary;
static bool			page_type_dump;
/* Store filename for page-type-dump option. */
char*				page_dump_filename = 0;
/* skip the checksum verification & rewrite if page is doublewrite buffer. */
static bool			skip_page = 0;
const char			*dbug_setting = "FALSE";
char*				log_filename = NULL;
/* User defined filename for logging. */
FILE*				log_file = NULL;
/* Enabled for log write option. */
static bool			is_log_enabled = false;

#ifndef _WIN32
/* advisory lock for non-window system. */
struct flock			lk;
#endif /* _WIN32 */

/* Strict check algorithm name. */
static ulong			strict_check;
/* Rewrite checksum algorithm name. */
static ulong			write_check;

/* Innodb page type. */
struct innodb_page_type {
	int n_undo_state_active;
	int n_undo_state_cached;
	int n_undo_state_to_free;
	int n_undo_state_to_purge;
	int n_undo_state_prepared;
	int n_undo_state_other;
	int n_undo_insert;
	int n_undo_update;
	int n_undo_other;
	int n_fil_page_index;
	int n_fil_page_undo_log;
	int n_fil_page_inode;
	int n_fil_page_ibuf_free_list;
	int n_fil_page_ibuf_bitmap;
	int n_fil_page_type_sys;
	int n_fil_page_type_trx_sys;
	int n_fil_page_type_fsp_hdr;
	int n_fil_page_type_allocated;
	int n_fil_page_type_xdes;
	int n_fil_page_type_blob;
	int n_fil_page_type_zblob;
	int n_fil_page_type_other;
	int n_fil_page_type_zblob2;
	int n_fil_page_type_page_compressed;
	int n_fil_page_type_page_compressed_encrypted;
} page_type;

/* Possible values for "--strict-check" for strictly verify checksum
and "--write" for rewrite checksum. */
static const char *innochecksum_algorithms[] = {
	"crc32",
	"crc32",
	"innodb",
	"innodb",
	"none",
	"none",
	NullS
};

/* Used to define an enumerate type of the "innochecksum algorithm". */
static TYPELIB innochecksum_algorithms_typelib = {
	array_elements(innochecksum_algorithms)-1,"",
	innochecksum_algorithms, NULL
};

#define SIZE_RANGES_FOR_PAGE 10
#define NUM_RETRIES 3
#define DEFAULT_RETRY_DELAY 1000000

struct per_page_stats {
  ulint n_recs;
  ulint data_size;
  ulint left_page_no;
  ulint right_page_no;
  per_page_stats(ulint n, ulint data, ulint left, ulint right) :
      n_recs(n), data_size(data), left_page_no(left), right_page_no(right) {}
  per_page_stats() : n_recs(0), data_size(0), left_page_no(0), right_page_no(0) {}
};

struct per_index_stats {
  unsigned long long pages;
  unsigned long long leaf_pages;
  ulint first_leaf_page;
  ulint count;
  ulint free_pages;
  ulint max_data_size;
  unsigned long long total_n_recs;
  unsigned long long total_data_bytes;

  /*!< first element for empty pages,
  last element for pages with more than logical_page_size */
  unsigned long long pages_in_size_range[SIZE_RANGES_FOR_PAGE+2];

  std::map<unsigned long long, per_page_stats> leaves;

  per_index_stats():pages(0), leaf_pages(0), first_leaf_page(0),
                    count(0), free_pages(0), max_data_size(0), total_n_recs(0),
                    total_data_bytes(0)
  {
    memset(pages_in_size_range, 0, sizeof(pages_in_size_range));
  }
};

std::map<unsigned long long, per_index_stats> index_ids;

void print_index_leaf_stats(
	unsigned long long id,
	const per_index_stats& index,
	FILE*	fil_out)

{
	ulint page_no = index.first_leaf_page;
	std::map<unsigned long long, per_page_stats>::const_iterator it_page = index.leaves.find(page_no);
	fprintf(fil_out, "\nindex: %llu leaf page stats: n_pages = %llu\n",
		id, index.leaf_pages);
	fprintf(fil_out, "page_no\tdata_size\tn_recs\n");
	while (it_page != index.leaves.end()) {
		const per_page_stats& stat = it_page->second;
		fprintf(fil_out, "%llu\t" ULINTPF "\t" ULINTPF "\n", it_page->first, stat.data_size, stat.n_recs);
		page_no = stat.right_page_no;
		it_page = index.leaves.find(page_no);
	}
}

void defrag_analysis(
	unsigned long long id,
	const per_index_stats& index,
	FILE*	fil_out)
{
	// TODO: make it work for compressed pages too
	std::map<unsigned long long, per_page_stats>::const_iterator it = index.leaves.find(index.first_leaf_page);
	ulint n_pages = 0;
	ulint n_leaf_pages = 0;
	while (it != index.leaves.end()) {
		ulint data_size_total = 0;
		for (ulong i = 0; i < n_merge; i++) {
			const per_page_stats& stat = it->second;
			n_leaf_pages ++;
			data_size_total += stat.data_size;
			it = index.leaves.find(stat.right_page_no);
			if (it == index.leaves.end()) {
				break;
			}
		}

		if (index.max_data_size) {
			n_pages += data_size_total / index.max_data_size;
			if (data_size_total % index.max_data_size != 0) {
				n_pages += 1;
			}
		}
	}

	if (index.leaf_pages) {
		fprintf(fil_out, "count = " ULINTPF " free = " ULINTPF "\n", index.count, index.free_pages);
	}

	if (!n_leaf_pages) {
		n_leaf_pages = 1;
	}

	fprintf(fil_out, "%llu\t\t%llu\t\t" ULINTPF "\t\t" ULINTPF "\t\t" ULINTPF "\t\t%.2f\t" ULINTPF "\n",
		id, index.leaf_pages, n_leaf_pages, n_merge, n_pages,
		1.0 - (double)n_pages / (double)n_leaf_pages, index.max_data_size);
}

void print_leaf_stats(
	FILE*	fil_out)
{
	fprintf(fil_out, "\n**************************************************\n");
	fprintf(fil_out, "index_id\t#leaf_pages\t#actual_leaf_pages\tn_merge\t"
		"#leaf_after_merge\tdefrag\n");
	for (std::map<unsigned long long, per_index_stats>::const_iterator it = index_ids.begin();
	     it != index_ids.end(); it++) {
		const per_index_stats& index = it->second;

		if (verbose) {
			print_index_leaf_stats(it->first, index, fil_out);
		}

		if (n_merge) {
			defrag_analysis(it->first, index, fil_out);
		}
	}
}

/** Get the page size of the filespace from the filespace header.
@param[in]	buf	buffer used to read the page.
@return page size */
static
const page_size_t
get_page_size(
	byte*	buf)
{
	const ulint	flags = mach_read_from_4(buf + FIL_PAGE_DATA
						 + FSP_SPACE_FLAGS);

	const ulint	ssize = FSP_FLAGS_GET_PAGE_SSIZE(flags);

	if (ssize == 0) {
		srv_page_size = UNIV_PAGE_SIZE_ORIG;
	} else {
		srv_page_size = ((UNIV_ZIP_SIZE_MIN >> 1) << ssize);
	}

	univ_page_size.copy_from(
		page_size_t(srv_page_size, srv_page_size, false));

	return(page_size_t(flags));
}

#ifdef _WIN32
/***********************************************//*
 @param		[in] error	error no. from the getLastError().

 @retval error message corresponding to error no.
*/
static
char*
error_message(
	int	error)
{
	static char err_msg[1024] = {'\0'};
	FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM,
		NULL, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR)err_msg, sizeof(err_msg), NULL );

	return (err_msg);
}
#endif /* _WIN32 */

/***********************************************//*
 @param>>_______[in] name>_____name of file.
 @retval file pointer; file pointer is NULL when error occured.
*/

FILE*
open_file(
	const char*	name)
{
	int	fd;		/* file descriptor. */
	FILE*	fil_in;
#ifdef _WIN32
	HANDLE		hFile;		/* handle to open file. */
	DWORD		access;		/* define access control */
	int		flags = 0;	/* define the mode for file
					descriptor */

	if (do_write) {
		access =  GENERIC_READ | GENERIC_WRITE;
		flags =  _O_RDWR | _O_BINARY;
	} else {
		access = GENERIC_READ;
		flags = _O_RDONLY | _O_BINARY;
	}

	/* CreateFile() also provide advisory lock with the usage of
	access and share mode of the file.*/
	hFile = CreateFile(
			(LPCTSTR) name, access, 0L, NULL,
			OPEN_EXISTING, NULL, NULL);

	if (hFile == INVALID_HANDLE_VALUE) {
		/* print the error message. */
		fprintf(stderr, "Filename::%s %s\n", name,
			error_message(GetLastError()));

			return (NULL);
		}

	/* get the file descriptor. */
	fd= _open_osfhandle((intptr_t)hFile, flags);
#else /* _WIN32 */

	int	create_flag;
	/* define the advisory lock and open file mode. */
	if (do_write) {
		create_flag = O_RDWR;
		lk.l_type = F_WRLCK;
	} else {
		create_flag = O_RDONLY;
		lk.l_type = F_RDLCK;
	}

	fd = open(name, create_flag);

	lk.l_whence = SEEK_SET;
	lk.l_start = lk.l_len = 0;

	if (fcntl(fd, F_SETLK, &lk) == -1) {
		fprintf(stderr, "Error: Unable to lock file::"
			" %s\n", name);
		perror("fcntl");
		return (NULL);
	}
#endif /* _WIN32 */

	if (do_write) {
		fil_in = fdopen(fd, "rb+");
	} else {
		fil_in = fdopen(fd, "rb");
	}

	return (fil_in);
}

/************************************************************//*
 Read the content of file

 @param  [in,out]	buf			read the file in buffer
 @param  [in]		partial_page_read	enable when to read the
						remaining buffer for first page.
 @param  [in]		physical_page_size	Physical/Commpressed page size.
 @param  [in,out]	fil_in			file pointer created for the
						tablespace.
 @retval no. of bytes read.
*/
ulong read_file(
	byte*	buf,
	bool	partial_page_read,
	ulong	physical_page_size,
	FILE*	fil_in)
{
	ulong bytes = 0;

	DBUG_ASSERT(physical_page_size >= UNIV_ZIP_SIZE_MIN);

	if (partial_page_read) {
		buf += UNIV_ZIP_SIZE_MIN;
		physical_page_size -= UNIV_ZIP_SIZE_MIN;
		bytes = UNIV_ZIP_SIZE_MIN;
	}

	bytes += ulong(fread(buf, 1, physical_page_size, fil_in));

	return bytes;
}

/** Check whether the page contains all zeroes.
@param[in]	buf	page
@param[in]	size	physical size of the page
@return true if the page is all zeroes; else false */
static bool is_page_all_zeroes(
	byte*	buf,
	ulint	size)
{
	/* On pages that are not all zero, the page number
	must match. */
	const ulint* p = reinterpret_cast<const ulint*>(buf);
	const ulint* const end = reinterpret_cast<const ulint*>(buf + size);
	do {
		if (*p++) {
			return false;
		}
	} while (p != end);

	return true;
}

/** Check if page is corrupted or not.
@param[in]	buf		page frame
@param[in]	page_size	page size
@param[in]	is_encrypted	true if page0 contained cryp_data
				with crypt_scheme encrypted
@param[in]	is_compressed	true if page0 fsp_flags contained
				page compression flag
@retval true if page is corrupted otherwise false. */
static
bool
is_page_corrupted(
	byte*			buf,
	const page_size_t&	page_size,
	bool			is_encrypted,
	bool			is_compressed)
{

	/* enable if page is corrupted. */
	bool is_corrupted;
	/* use to store LSN values. */
	uint32_t logseq;
	uint32_t logseqfield;
	ulint page_type = mach_read_from_2(buf+FIL_PAGE_TYPE);
	uint32_t key_version = mach_read_from_4(buf+FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION);
	uint32_t space_id = mach_read_from_4(
		buf + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID);

	if (mach_read_from_4(buf + FIL_PAGE_OFFSET) != cur_page_num
	    || space_id != cur_space) {
		/* On pages that are not all zero, the page number
		must match. */
		if (is_page_all_zeroes(buf, page_size.physical())) {
			return false;
		}

		if (is_log_enabled) {
			fprintf(log_file,
				"page id mismatch space::" UINT32PF
				" page::" UINT32PF " \n",
				space_id, cur_page_num);
		}

		return true;
	}

	/* We can't trust only a page type, thus we take account
	also fsp_flags or crypt_data on page 0 */
	if ((page_type == FIL_PAGE_PAGE_COMPRESSED && is_compressed) ||
	    (page_type == FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED &&
	     is_compressed && is_encrypted)) {
		/* Page compressed tables do not contain post compression
		checksum. */
		return (false);
	}

	if (page_size.is_compressed()) {
		/* check the stored log sequence numbers
		for uncompressed tablespace. */
		logseq = mach_read_from_4(buf + FIL_PAGE_LSN + 4);
		logseqfield = mach_read_from_4(
				buf + page_size.logical() -
				FIL_PAGE_END_LSN_OLD_CHKSUM + 4);

		if (is_log_enabled) {
			fprintf(log_file,
				"space::" UINT32PF " page::" UINT32PF
				"; log sequence number:first = " UINT32PF
				"; second = " UINT32PF "\n",
				space_id, cur_page_num, logseq, logseqfield);
			if (logseq != logseqfield) {
				fprintf(log_file,
					"Fail; space::" UINT32PF
					" page::" UINT32PF
					" invalid (fails log "
					"sequence number check)\n",
					space_id, cur_page_num);
			}
		}
	}

	/* Again we can't trust only FIL_PAGE_FILE_FLUSH_LSN field
	now repurposed as FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION,
	we need to check also crypt_data contents.

	If page is encrypted, use different checksum calculation
	as innochecksum can't decrypt pages. Note that some old InnoDB
	versions did not initialize FIL_PAGE_FILE_FLUSH_LSN field
	so if crypt checksum does not match we verify checksum using
	normal method. */
	if (is_encrypted && key_version != 0) {
		is_corrupted = !fil_space_verify_crypt_checksum(buf,
								page_size);
		if (is_corrupted && log_file) {
			fprintf(log_file,
				"[page id: space=" UINT32PF
				", page_number=" UINT32PF "] may be corrupted;"
				" key_version=" UINT32PF "\n",
				space_id, cur_page_num,
				mach_read_from_4(
					FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION
					+ buf));
		}
	} else {
		is_corrupted = true;
	}

	if (is_corrupted) {
		is_corrupted = buf_page_is_corrupted(
			true, buf, page_size, NULL);
	}

	return(is_corrupted);
}

/********************************************//*
 Check if page is doublewrite buffer or not.
 @param [in] page	buffer page

 @retval true  if page is doublewrite buffer otherwise false.
*/
static
bool
is_page_doublewritebuffer(
	const byte*	page)
{
	if ((cur_page_num >= FSP_EXTENT_SIZE)
		&& (cur_page_num < FSP_EXTENT_SIZE * 3)) {
		/* page is doublewrite buffer. */
		return (true);
	}

	return (false);
}

/*******************************************************//*
Check if page is empty or not.
 @param		[in] page		page to checked for empty.
 @param		[in] len	size of page.

 @retval true if page is empty.
 @retval false if page is not empty.
*/
static
bool
is_page_empty(
	const byte*	page,
	size_t		len)
{
	while (len--) {
		if (*page++) {
			return (false);
		}
	}

	return (true);
}

/********************************************************************//**
Rewrite the checksum for the page.
@param	[in/out] page			page buffer
@param	[in] physical_page_size		page size in bytes on disk.
@param	[in] iscompressed		Is compressed/Uncompressed Page.

@retval true  : do rewrite
@retval false : skip the rewrite as checksum stored match with
		calculated or page is doublwrite buffer.
*/

bool
update_checksum(
	byte*	page,
	ulong	physical_page_size,
	bool	iscompressed)
{
	ib_uint32_t	checksum = 0;
	byte		stored1[4];	/* get FIL_PAGE_SPACE_OR_CHKSUM field checksum */
	byte		stored2[4];	/* get FIL_PAGE_END_LSN_OLD_CHKSUM field checksum */

	ut_ad(page);
	/* If page is doublewrite buffer, skip the rewrite of checksum. */
	if (skip_page) {
		return (false);
	}

	memcpy(stored1, page + FIL_PAGE_SPACE_OR_CHKSUM, 4);
	memcpy(stored2, page + physical_page_size -
	       FIL_PAGE_END_LSN_OLD_CHKSUM, 4);

	/* Check if page is empty, exclude the checksum field */
	if (is_page_empty(page + 4, physical_page_size - 12)
	    && is_page_empty(page + physical_page_size - 4, 4)) {

		memset(page + FIL_PAGE_SPACE_OR_CHKSUM, 0, 4);
		memset(page + physical_page_size -
		       FIL_PAGE_END_LSN_OLD_CHKSUM, 0, 4);

		goto func_exit;
	}

	if (iscompressed) {
		/* page is compressed */
		checksum = page_zip_calc_checksum(
			page, physical_page_size,
			static_cast<srv_checksum_algorithm_t>(write_check));

		mach_write_to_4(page + FIL_PAGE_SPACE_OR_CHKSUM, checksum);
		if (is_log_enabled) {
			fprintf(log_file, "page::" UINT32PF "; Updated checksum ="
				" " UINT32PF "\n", cur_page_num, checksum);
		}

	} else {
		/* page is uncompressed. */

		/* Store the new formula checksum */
		switch ((srv_checksum_algorithm_t) write_check) {

		case SRV_CHECKSUM_ALGORITHM_CRC32:
		case SRV_CHECKSUM_ALGORITHM_STRICT_CRC32:
			checksum = buf_calc_page_crc32(page);
			break;

		case SRV_CHECKSUM_ALGORITHM_INNODB:
		case SRV_CHECKSUM_ALGORITHM_STRICT_INNODB:
			checksum = (ib_uint32_t)
					buf_calc_page_new_checksum(page);
			break;

		case SRV_CHECKSUM_ALGORITHM_NONE:
		case SRV_CHECKSUM_ALGORITHM_STRICT_NONE:
			checksum = BUF_NO_CHECKSUM_MAGIC;
			break;
		/* no default so the compiler will emit a warning if new
		enum is added and not handled here */
		}

		mach_write_to_4(page + FIL_PAGE_SPACE_OR_CHKSUM, checksum);
		if (is_log_enabled) {
			fprintf(log_file, "page::" UINT32PF "; Updated checksum field1"
				" = " UINT32PF "\n", cur_page_num, checksum);
		}

		if (write_check == SRV_CHECKSUM_ALGORITHM_STRICT_INNODB
		    || write_check == SRV_CHECKSUM_ALGORITHM_INNODB) {
			checksum = (ib_uint32_t)
					buf_calc_page_old_checksum(page);
		}

		mach_write_to_4(page + physical_page_size -
				FIL_PAGE_END_LSN_OLD_CHKSUM,checksum);

		if (is_log_enabled) {
			fprintf(log_file, "page::" UINT32PF "; Updated checksum "
				"field2 = " UINT32PF "\n", cur_page_num, checksum);
		}

	}

func_exit:
	/* The following code is to check the stored checksum with the
	calculated checksum. If it matches, then return FALSE to skip
	the rewrite of checksum, otherwise return TRUE. */
	if (iscompressed) {
		if (!memcmp(stored1, page + FIL_PAGE_SPACE_OR_CHKSUM, 4)) {
			return (false);
		}
		return (true);
	}

	if (!memcmp(stored1, page + FIL_PAGE_SPACE_OR_CHKSUM, 4)
	    && !memcmp(stored2, page + physical_page_size -
		       FIL_PAGE_END_LSN_OLD_CHKSUM, 4)) {
		return (false);

	}

	return (true);
}

/**
 Write the content to the file
@param[in]		filename	name of the file.
@param[in,out]		file		file pointer where content
					have to be written
@param[in]		buf		file buffer read
@param[in]		compressed	Enabled if tablespace is
					compressed.
@param[in,out]		pos		current file position.
@param[in]		page_size	page size in bytes on disk.

@retval true	if successfully written
@retval false	if a non-recoverable error occurred
*/
static
bool
write_file(
	const char*	filename,
	FILE*		file,
	byte*		buf,
	bool		compressed,
	fpos_t*		pos,
	ulong		page_size)
{
	bool	do_update;

	do_update = update_checksum(buf, page_size, compressed);

	if (file != stdin) {
		if (do_update) {
			/* Set the previous file pointer position
			saved in pos to current file position. */
			if (0 != fsetpos(file, pos)) {
				perror("fsetpos");
				return(false);
			}
		} else {
			/* Store the current file position in pos */
			if (0 != fgetpos(file, pos)) {
				perror("fgetpos");
				return(false);
			}
			return(true);
		}
	}

	if (page_size
		!= fwrite(buf, 1, page_size, file == stdin ? stdout : file)) {
		fprintf(stderr, "Failed to write page::" UINT32PF " to %s: %s\n",
			cur_page_num, filename, strerror(errno));

		return(false);
	}
	if (file != stdin) {
		fflush(file);
		/* Store the current file position in pos */
		if (0 != fgetpos(file, pos)) {
			perror("fgetpos");
			return(false);
		}
	}

	return(true);
}

// checks using current xdes page whether the page is free
static inline bool is_page_free(const byte *xdes, page_size_t page_size,
                                uint32_t page_no)
{
  const byte *des=
      xdes + XDES_ARR_OFFSET +
      XDES_SIZE * ((page_no & (page_size.physical() - 1)) / FSP_EXTENT_SIZE);
  return xdes_get_bit(des, XDES_FREE_BIT, page_no % FSP_EXTENT_SIZE);
}

/*
Parse the page and collect/dump the information about page type
@param [in] page	buffer page
@param [out] xdes	extend descriptor page
@param [in] file	file for diagnosis.
@param [in] page_size	page_size
@param [in] is_encrypted  tablespace is encrypted
*/
void
parse_page(
	const byte*	page,
	byte*		xdes,
	FILE*		file,
	const page_size_t& page_size,
	bool is_encrypted)
{
	unsigned long long id;
	uint16_t undo_page_type;
	char str[20]={'\0'};
	ulint n_recs;
	uint32_t page_no, left_page_no, right_page_no;
	ulint data_bytes;
	bool is_leaf;
	int size_range_id;

	/* Check whether page is doublewrite buffer. */
	if(skip_page) {
		strcpy(str, "Double_write_buffer");
	} else {
		strcpy(str, "-");
	}

	switch (mach_read_from_2(page + FIL_PAGE_TYPE)) {

	case FIL_PAGE_INDEX: {
		uint32_t key_version = mach_read_from_4(page + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION);
		page_type.n_fil_page_index++;

		/* If page is encrypted we can't read index header */
		if (!is_encrypted) {
			id = mach_read_from_8(page + PAGE_HEADER + PAGE_INDEX_ID);
			n_recs = mach_read_from_2(page + PAGE_HEADER + PAGE_N_RECS);
			page_no = mach_read_from_4(page + FIL_PAGE_OFFSET);
			left_page_no = mach_read_from_4(page + FIL_PAGE_PREV);
			right_page_no = mach_read_from_4(page + FIL_PAGE_NEXT);
			ulint is_comp = mach_read_from_2(page + PAGE_HEADER + PAGE_N_HEAP) & 0x8000;
			ulint level = mach_read_from_2(page + PAGE_HEADER + PAGE_LEVEL);
			ulint garbage = mach_read_from_2(page + PAGE_HEADER + PAGE_GARBAGE);


			data_bytes = (ulint)(mach_read_from_2(page + PAGE_HEADER + PAGE_HEAP_TOP)
				- (is_comp
					? PAGE_NEW_SUPREMUM_END
					: PAGE_OLD_SUPREMUM_END)
				- garbage);

			is_leaf = (!*(const uint16*) (page + (PAGE_HEADER + PAGE_LEVEL)));

			if (file) {
				fprintf(file, "#::" UINT32PF "\t\t|\t\tIndex page\t\t\t|"
					"\tindex id=%llu,", cur_page_num, id);

				fprintf(file,
					" page level=" ULINTPF
					", No. of records=" ULINTPF
					", garbage=" ULINTPF ", %s\n",
					level, n_recs, garbage, str);
			}

			size_range_id = (data_bytes * SIZE_RANGES_FOR_PAGE
				+ page_size.logical() - 1) /
				page_size.logical();

			if (size_range_id > SIZE_RANGES_FOR_PAGE + 1) {
				/* data_bytes is bigger than logical_page_size */
				size_range_id = SIZE_RANGES_FOR_PAGE + 1;
			}
			if (per_page_details) {
				printf("index id=%llu page " UINT32PF " leaf %d n_recs " ULINTPF " data_bytes " ULINTPF
					"\n", id, page_no, is_leaf, n_recs, data_bytes);
			}
			/* update per-index statistics */
			{
				per_index_stats &index = index_ids[id];
				if (is_page_free(xdes, page_size, page_no)) {
					index.free_pages++;
					return;
				}

				index.pages++;

				if (is_leaf) {
					index.leaf_pages++;
					if (data_bytes > index.max_data_size) {
						index.max_data_size = data_bytes;
					}
					struct per_page_stats pp(n_recs, data_bytes,
						left_page_no, right_page_no);

					index.leaves[page_no] = pp;

					if (left_page_no == ULINT32_UNDEFINED) {
						index.first_leaf_page = page_no;
						index.count++;
					}
				}

				index.total_n_recs += n_recs;
				index.total_data_bytes += data_bytes;
				index.pages_in_size_range[size_range_id] ++;
			}
		} else if (file) {
			fprintf(file, "#::" UINT32PF "\t\t|\t\tEncrypted Index page\t\t\t|"
				"\tkey_version " UINT32PF ",%s\n", cur_page_num, key_version, str);
		}

		break;
	}
	case FIL_PAGE_UNDO_LOG:
		page_type.n_fil_page_undo_log++;
		undo_page_type = mach_read_from_2(page +
				     TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_TYPE);
		if (file) {
			fprintf(file, "#::" UINT32PF "\t\t|\t\tUndo log page\t\t\t|",
				cur_page_num);
		}
		if (undo_page_type == TRX_UNDO_INSERT) {
			page_type.n_undo_insert++;
			if (file) {
				fprintf(file, "\t%s",
					"Insert Undo log page");
			}

		} else if (undo_page_type == TRX_UNDO_UPDATE) {
			page_type.n_undo_update++;
			if (file) {
				fprintf(file, "\t%s",
					"Update undo log page");
			}
		}

		undo_page_type = mach_read_from_2(page + TRX_UNDO_SEG_HDR +
						  TRX_UNDO_STATE);
		switch (undo_page_type) {
			case TRX_UNDO_ACTIVE:
				page_type.n_undo_state_active++;
				if (file) {
					fprintf(file, ", %s", "Undo log of "
						"an active transaction");
				}
				break;

			case TRX_UNDO_CACHED:
				page_type.n_undo_state_cached++;
				if (file) {
					fprintf(file, ", %s", "Page is "
						"cached for quick reuse");
				}
				break;

			case TRX_UNDO_TO_FREE:
				page_type.n_undo_state_to_free++;
				if (file) {
					fprintf(file, ", %s", "Insert undo "
						"segment that can be freed");
				}
				break;

			case TRX_UNDO_TO_PURGE:
				page_type.n_undo_state_to_purge++;
				if (file) {
					fprintf(file, ", %s", "Will be "
						"freed in purge when all undo"
					"data in it is removed");
				}
				break;

			case TRX_UNDO_PREPARED:
				page_type.n_undo_state_prepared++;
				if (file) {
					fprintf(file, ", %s", "Undo log of "
						"an prepared transaction");
				}
				break;

			default:
				page_type.n_undo_state_other++;
				break;
		}
		if(file) {
			fprintf(file, ", %s\n", str);
		}
		break;

	case FIL_PAGE_INODE:
		page_type.n_fil_page_inode++;
		if (file) {
			fprintf(file, "#::" UINT32PF "\t\t|\t\tInode page\t\t\t|"
				"\t%s\n",cur_page_num, str);
		}
		break;

	case FIL_PAGE_IBUF_FREE_LIST:
		page_type.n_fil_page_ibuf_free_list++;
		if (file) {
			fprintf(file, "#::" UINT32PF "\t\t|\t\tInsert buffer free list"
				" page\t|\t%s\n", cur_page_num, str);
		}
		break;

	case FIL_PAGE_TYPE_ALLOCATED:
		page_type.n_fil_page_type_allocated++;
		if (file) {
			fprintf(file, "#::" UINT32PF "\t\t|\t\tFreshly allocated "
				"page\t\t|\t%s\n", cur_page_num, str);
		}
		break;

	case FIL_PAGE_IBUF_BITMAP:
		page_type.n_fil_page_ibuf_bitmap++;
		if (file) {
			fprintf(file, "#::" UINT32PF "\t\t|\t\tInsert Buffer "
				"Bitmap\t\t|\t%s\n", cur_page_num, str);
		}
		break;

	case FIL_PAGE_TYPE_SYS:
		page_type.n_fil_page_type_sys++;
		if (file) {
			fprintf(file, "#::" UINT32PF "\t\t|\t\tSystem page\t\t\t|"
				"\t%s\n", cur_page_num, str);
		}
		break;

	case FIL_PAGE_TYPE_TRX_SYS:
		page_type.n_fil_page_type_trx_sys++;
		if (file) {
			fprintf(file, "#::" UINT32PF "\t\t|\t\tTransaction system "
				"page\t\t|\t%s\n", cur_page_num, str);
		}
		break;

	case FIL_PAGE_TYPE_FSP_HDR:
		page_type.n_fil_page_type_fsp_hdr++;
		if (file) {
			fprintf(file, "#::" UINT32PF "\t\t|\t\tFile Space "
				"Header\t\t|\t%s\n", cur_page_num, str);
		}
		break;

	case FIL_PAGE_TYPE_XDES:
		page_type.n_fil_page_type_xdes++;
		if (file) {
			fprintf(file, "#::" UINT32PF "\t\t|\t\tExtent descriptor "
				"page\t\t|\t%s\n", cur_page_num, str);
		}
		break;

	case FIL_PAGE_TYPE_BLOB:
		page_type.n_fil_page_type_blob++;
		if (file) {
			fprintf(file, "#::" UINT32PF "\t\t|\t\tBLOB page\t\t\t|\t%s\n",
				cur_page_num, str);
		}
		break;

	case FIL_PAGE_TYPE_ZBLOB:
		page_type.n_fil_page_type_zblob++;
		if (file) {
			fprintf(file, "#::" UINT32PF "\t\t|\t\tCompressed BLOB "
				"page\t\t|\t%s\n", cur_page_num, str);
		}
		break;

	case FIL_PAGE_TYPE_ZBLOB2:
		page_type.n_fil_page_type_zblob2++;
		if (file) {
			fprintf(file, "#::" UINT32PF "\t\t|\t\tSubsequent Compressed "
				"BLOB page\t|\t%s\n", cur_page_num, str);
		}
			break;

	case FIL_PAGE_PAGE_COMPRESSED:
		page_type.n_fil_page_type_page_compressed++;
		if (file) {
			fprintf(file, "#::" UINT32PF "\t\t|\t\tPage compressed "
				"page\t|\t%s\n", cur_page_num, str);
		}
		break;

	case FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED:
		page_type.n_fil_page_type_page_compressed_encrypted++;
		if (file) {
			fprintf(file, "#::" UINT32PF "\t\t|\t\tPage compressed encrypted "
				"page\t|\t%s\n", cur_page_num, str);
		}
		break;
	default:
		page_type.n_fil_page_type_other++;
		break;
	}
}
/**
@param [in/out] file_name	name of the filename

@retval FILE pointer if successfully created else NULL when error occured.
*/
FILE*
create_file(
	char*	file_name)
{
	FILE*	file = NULL;

#ifndef _WIN32
	file = fopen(file_name, "wb");
	if (file == NULL) {
		fprintf(stderr, "Failed to create file: %s: %s\n",
			file_name, strerror(errno));
		return(NULL);
	}
#else
	HANDLE		hFile;		/* handle to open file. */
	int fd = 0;
	hFile = CreateFile((LPCTSTR) file_name,
			  GENERIC_READ | GENERIC_WRITE,
			  FILE_SHARE_READ | FILE_SHARE_DELETE,
			  NULL, CREATE_NEW, NULL, NULL);

	if (hFile == INVALID_HANDLE_VALUE) {
		/* print the error message. */
		fprintf(stderr, "Filename::%s %s\n",
			file_name,
			error_message(GetLastError()));

			return(NULL);
		}

	/* get the file descriptor. */
	fd= _open_osfhandle((intptr_t)hFile, _O_RDWR | _O_BINARY);
	file = fdopen(fd, "wb");
#endif /* _WIN32 */

	return(file);
}

/*
 Print the page type count of a tablespace.
 @param [in] fil_out	stream where the output goes.
*/
void
print_summary(
	FILE*	fil_out)
{
	fprintf(fil_out, "\n================PAGE TYPE SUMMARY==============\n");
	fprintf(fil_out, "#PAGE_COUNT\tPAGE_TYPE");
	fprintf(fil_out, "\n===============================================\n");
	fprintf(fil_out, "%8d\tIndex page\n",
		page_type.n_fil_page_index);
	fprintf(fil_out, "%8d\tUndo log page\n",
		page_type.n_fil_page_undo_log);
	fprintf(fil_out, "%8d\tInode page\n",
		page_type.n_fil_page_inode);
	fprintf(fil_out, "%8d\tInsert buffer free list page\n",
		page_type.n_fil_page_ibuf_free_list);
	fprintf(fil_out, "%8d\tFreshly allocated page\n",
		page_type.n_fil_page_type_allocated);
	fprintf(fil_out, "%8d\tInsert buffer bitmap\n",
		page_type.n_fil_page_ibuf_bitmap);
	fprintf(fil_out, "%8d\tSystem page\n",
		page_type.n_fil_page_type_sys);
	fprintf(fil_out, "%8d\tTransaction system page\n",
		page_type.n_fil_page_type_trx_sys);
	fprintf(fil_out, "%8d\tFile Space Header\n",
		page_type.n_fil_page_type_fsp_hdr);
	fprintf(fil_out, "%8d\tExtent descriptor page\n",
		page_type.n_fil_page_type_xdes);
	fprintf(fil_out, "%8d\tBLOB page\n",
		page_type.n_fil_page_type_blob);
	fprintf(fil_out, "%8d\tCompressed BLOB page\n",
		page_type.n_fil_page_type_zblob);
	fprintf(fil_out, "%8d\tPage compressed page\n",
		page_type.n_fil_page_type_page_compressed);
	fprintf(fil_out, "%8d\tPage compressed encrypted page\n",
		page_type.n_fil_page_type_page_compressed_encrypted);
	fprintf(fil_out, "%8d\tOther type of page\n",
		page_type.n_fil_page_type_other);

	fprintf(fil_out, "\n===============================================\n");
	fprintf(fil_out, "Additional information:\n");
	fprintf(fil_out, "Undo page type: %d insert, %d update, %d other\n",
		page_type.n_undo_insert,
		page_type.n_undo_update,
		page_type.n_undo_other);
	fprintf(fil_out, "Undo page state: %d active, %d cached, %d to_free, %d"
		" to_purge, %d prepared, %d other\n",
		page_type.n_undo_state_active,
		page_type.n_undo_state_cached,
		page_type.n_undo_state_to_free,
		page_type.n_undo_state_to_purge,
		page_type.n_undo_state_prepared,
		page_type.n_undo_state_other);

	fprintf(fil_out, "index_id\t#pages\t\t#leaf_pages\t#recs_per_page"
		"\t#bytes_per_page\n");

	for (std::map<unsigned long long, per_index_stats>::const_iterator it = index_ids.begin();
	     it != index_ids.end(); it++) {
		const per_index_stats& index = it->second;
		fprintf(fil_out, "%lld\t\t%lld\t\t%lld\t\t%lld\t\t%lld\n",
			it->first, index.pages, index.leaf_pages,
			index.total_n_recs / index.pages,
			index.total_data_bytes / index.pages);
	}

	fprintf(fil_out, "\n");
	fprintf(fil_out, "index_id\tpage_data_bytes_histgram(empty,...,oversized)\n");

	for (std::map<unsigned long long, per_index_stats>::const_iterator it = index_ids.begin();
	     it != index_ids.end(); it++) {
		fprintf(fil_out, "%lld\t", it->first);
		const per_index_stats& index = it->second;
		for (ulint i = 0; i < SIZE_RANGES_FOR_PAGE+2; i++) {
			fprintf(fil_out, "\t%lld", index.pages_in_size_range[i]);
		}
		fprintf(fil_out, "\n");
	}

	if (do_leaf) {
		print_leaf_stats(fil_out);
	}
}

/* command line argument for innochecksum tool. */
static struct my_option innochecksum_options[] = {
  {"help", '?', "Displays this help and exits.",
    0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"info", 'I', "Synonym for --help.",
    0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"version", 'V', "Displays version information and exits.",
    0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"verbose", 'v', "Verbose (prints progress every 5 seconds).",
    &verbose, &verbose, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
#ifndef DBUG_OFF
  {"debug", '#', "Output debug log. See https://mariadb.com/kb/en/library/creating-a-trace-file/",
    &dbug_setting, &dbug_setting, 0, GET_STR, OPT_ARG, 0, 0, 0, 0, 0, 0},
#endif /* !DBUG_OFF */
  {"count", 'c', "Print the count of pages in the file and exits.",
    &just_count, &just_count, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"start_page", 's', "Start on this page number (0 based).",
    &start_page, &start_page, 0, GET_UINT, REQUIRED_ARG,
    0, 0, FIL_NULL, 0, 1, 0},
  {"end_page", 'e', "End at this page number (0 based).",
    &end_page, &end_page, 0, GET_UINT, REQUIRED_ARG,
    0, 0, FIL_NULL, 0, 1, 0},
  {"page", 'p', "Check only this page (0 based).",
    &do_page, &do_page, 0, GET_UINT, REQUIRED_ARG,
    0, 0, FIL_NULL, 0, 1, 0},
  {"strict-check", 'C', "Specify the strict checksum algorithm by the user.",
    &strict_check, &strict_check, &innochecksum_algorithms_typelib,
    GET_ENUM, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"no-check", 'n', "Ignore the checksum verification.",
    &no_check, &no_check, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"allow-mismatches", 'a', "Maximum checksum mismatch allowed.",
    &allow_mismatches, &allow_mismatches, 0,
    GET_ULL, REQUIRED_ARG, 0, 0, ULLONG_MAX, 0, 1, 0},
  {"write", 'w', "Rewrite the checksum algorithm by the user.",
    &write_check, &write_check, &innochecksum_algorithms_typelib,
    GET_ENUM, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"page-type-summary", 'S', "Display a count of each page type "
   "in a tablespace.", &page_type_summary, &page_type_summary, 0,
   GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"page-type-dump", 'D', "Dump the page type info for each page in a "
   "tablespace.", &page_dump_filename, &page_dump_filename, 0,
   GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"per-page-details", 'i', "Print out per-page detail information.",
   &per_page_details, &per_page_details, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
   {"log", 'l', "log output.",
     &log_filename, &log_filename, 0,
      GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"leaf", 'f', "Examine leaf index pages",
    &do_leaf, &do_leaf, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"merge", 'm', "leaf page count if merge given number of consecutive pages",
   &n_merge, &n_merge, 0, GET_ULONG, REQUIRED_ARG, 0, 0, (longlong)10L, 0, 1, 0},

  {0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}
};

/* Print out the Innodb version and machine information. */
static void print_version(void)
{
#ifdef DBUG_OFF
	printf("%s Ver %s, for %s (%s)\n",
		my_progname, INNODB_VERSION_STR,
		SYSTEM_TYPE, MACHINE_TYPE);
#else
	printf("%s-debug Ver %s, for %s (%s)\n",
		my_progname, INNODB_VERSION_STR,
		SYSTEM_TYPE, MACHINE_TYPE);
#endif /* DBUG_OFF */
}

static void usage(void)
{
	print_version();
	puts(ORACLE_WELCOME_COPYRIGHT_NOTICE("2000"));
	printf("InnoDB offline file checksum utility.\n");
	printf("Usage: %s [-c] [-s <start page>] [-e <end page>] "
		"[-p <page>] [-i] [-v]  [-a <allow mismatches>] [-n] "
		"[-C <strict-check>] [-w <write>] [-S] [-D <page type dump>] "
		"[-l <log>] [-l] [-m <merge pages>] <filename or [-]>\n", my_progname);
	printf("See https://mariadb.com/kb/en/library/innochecksum/"
	       " for usage hints.\n");
	my_print_help(innochecksum_options);
	my_print_variables(innochecksum_options);
}

extern "C" my_bool
innochecksum_get_one_option(
	int			optid,
	const struct my_option	*opt MY_ATTRIBUTE((unused)),
	char			*argument MY_ATTRIBUTE((unused)))
{
	switch (optid) {
#ifndef DBUG_OFF
	case '#':
		dbug_setting = argument
			? argument
			: IF_WIN("d:O,innochecksum.trace",
				 "d:o,/tmp/innochecksum.trace");
		DBUG_PUSH(dbug_setting);
		break;
#endif /* !DBUG_OFF */
	case 'e':
		use_end_page = true;
		break;
	case 'p':
		end_page = start_page = do_page;
		use_end_page = true;
		do_one_page = true;
		break;
	case 'V':
		print_version();
		my_end(0);
		exit(EXIT_SUCCESS);
		break;
	case 'C':
		strict_verify = true;
		switch ((srv_checksum_algorithm_t) strict_check) {

		case SRV_CHECKSUM_ALGORITHM_STRICT_CRC32:
		case SRV_CHECKSUM_ALGORITHM_CRC32:
			srv_checksum_algorithm =
				SRV_CHECKSUM_ALGORITHM_STRICT_CRC32;
			break;

		case SRV_CHECKSUM_ALGORITHM_STRICT_INNODB:
		case SRV_CHECKSUM_ALGORITHM_INNODB:
			srv_checksum_algorithm =
				SRV_CHECKSUM_ALGORITHM_STRICT_INNODB;
			break;

		case SRV_CHECKSUM_ALGORITHM_STRICT_NONE:
		case SRV_CHECKSUM_ALGORITHM_NONE:
			srv_checksum_algorithm =
				SRV_CHECKSUM_ALGORITHM_STRICT_NONE;
			break;
		default:
			return(true);
		}
		break;
	case 'n':
		no_check = true;
		break;
	case 'a':
	case 'S':
		break;
	case 'w':
		do_write = true;
		break;
	case 'D':
		page_type_dump = true;
		break;
	case 'l':
		is_log_enabled = true;
		break;
	case 'I':
	case '?':
		usage();
		my_end(0);
		exit(EXIT_SUCCESS);
		break;
	}

	return(false);
}

static
bool
get_options(
	int	*argc,
	char	***argv)
{
	if (handle_options(argc, argv, innochecksum_options,
			innochecksum_get_one_option)) {
		my_end(0);
		exit(true);
	}

	/* The next arg must be the filename */
	if (!*argc) {
		usage();
		my_end(0);
		return (true);
	}

	return (false);
}

/** Check from page 0 if table is encrypted.
@param[in]	filename	Filename
@param[in]	page_size	page size
@param[in]	page		Page 0
@retval true if tablespace is encrypted, false if not
*/
static
bool check_encryption(
	const char* filename,
	const page_size_t& page_size,
	byte * page)
{
	ulint offset = (FSP_HEADER_OFFSET + (XDES_ARR_OFFSET + XDES_SIZE *
			(page_size.physical()) / FSP_EXTENT_SIZE));

	if (memcmp(page + offset, CRYPT_MAGIC, MAGIC_SZ) != 0) {
		return false;
	}

	ulint type = mach_read_from_1(page + offset + MAGIC_SZ + 0);

	if (! (type == CRYPT_SCHEME_UNENCRYPTED ||
	       type == CRYPT_SCHEME_1)) {
		return false;
	}

	ulint iv_length = mach_read_from_1(page + offset + MAGIC_SZ + 1);

	if (iv_length != CRYPT_SCHEME_1_IV_LEN) {
		return false;
	}

	uint32_t min_key_version = mach_read_from_4
		(page + offset + MAGIC_SZ + 2 + iv_length);

	uint32_t key_id = mach_read_from_4
		(page + offset + MAGIC_SZ + 2 + iv_length + 4);

	if (type == CRYPT_SCHEME_1 && is_log_enabled) {
		fprintf(log_file,"Tablespace %s encrypted key_version " UINT32PF " key_id " UINT32PF "\n",
			filename, min_key_version, key_id);
	}

	return (type == CRYPT_SCHEME_1);
}

/**
Verify page checksum.
@param[in] buf			page to verify
@param[in] page_size		page size
@param[in] is_encrypted		true if tablespace is encrypted
@param[in] is_compressed	true if tablespace is page compressed
@param[in,out] mismatch_count	Number of pages failed in checksum verify
@retval 0 if page checksum matches or 1 if it does not match
*/
static
int verify_checksum(
	byte* buf,
	const page_size_t& page_size,
	bool is_encrypted,
	bool is_compressed,
	unsigned long long* mismatch_count)
{
	int exit_status = 0;
	bool is_corrupted = false;

	is_corrupted = is_page_corrupted(
		buf, page_size, is_encrypted, is_compressed);

	if (is_corrupted) {
		fprintf(stderr, "Fail: page::" UINT32PF " invalid\n",
			cur_page_num);

		(*mismatch_count)++;

		if (*mismatch_count > allow_mismatches) {
			fprintf(stderr,
				"Exceeded the "
				"maximum allowed "
				"checksum mismatch "
				"count::%llu current::%llu\n",
				*mismatch_count,
				allow_mismatches);

			exit_status = 1;
		}
	}

	return (exit_status);
}

/** Rewrite page checksum if needed.
@param[in]	filename	File name
@param[in]	fil_in		File pointer
@param[in]	buf		page
@param[in]	page_size	page size
@param[in]	pos		File position
@param[in]	is_encrypted	true if tablespace is encrypted
@param[in]	is_compressed	true if tablespace is page compressed
@retval 0 if checksum rewrite was successful, 1 if error was detected */
static
int
rewrite_checksum(
	const char*	filename,
	FILE*		fil_in,
	byte*		buf,
	const page_size_t& page_size,
	fpos_t*		pos,
	bool is_encrypted,
	bool is_compressed)
{
	int exit_status = 0;
	/* Rewrite checksum. Note that for encrypted and
	page compressed tables this is not currently supported. */
	if (do_write &&
		!is_encrypted &&
		!is_compressed
		&& !write_file(filename, fil_in, buf,
			page_size.is_compressed(), pos,
			static_cast<ulong>(page_size.physical()))) {

		exit_status = 1;
	}

	return (exit_status);
}

int main(
	int	argc,
	char	**argv)
{
	/* our input file. */
	FILE*		fil_in = NULL;
	/* our input filename. */
	char*		filename;
	/* Buffer to store pages read. */
	byte*		buf_ptr = NULL;
	byte*		xdes_ptr = NULL;
	byte*		buf = NULL;
	byte*		xdes = NULL;
	/* bytes read count */
	ulint		bytes;
	/* last time */
	time_t		lastt = 0;
	/* stat, to get file size. */
#ifdef _WIN32
	struct _stat64	st;
#else
	struct stat	st;
#endif /* _WIN32 */

	int exit_status = 0;

	/* size of file (has to be 64 bits) */
	unsigned long long int	size		= 0;
	/* number of pages in file */
	uint32_t	pages;

	off_t		offset			= 0;
	/* count the no. of page corrupted. */
	unsigned long long   mismatch_count		= 0;

	bool		partial_page_read	= false;
	/* Enabled when read from stdin is done. */
	bool		read_from_stdin		= false;
	FILE*		fil_page_type		= NULL;
	fpos_t		pos;

	/* enable when space_id of given file is zero. */
	bool		is_system_tablespace = false;

	ut_crc32_init();
	MY_INIT(argv[0]);
	DBUG_ENTER("main");
	DBUG_PROCESS(argv[0]);

	if (get_options(&argc,&argv)) {
		exit_status = 1;
		goto my_exit;
	}

	if (strict_verify && no_check) {
		fprintf(stderr, "Error: --strict-check option cannot be used "
			"together with --no-check option.\n");
		exit_status = 1;
		goto my_exit;
	}

	if (no_check && !do_write) {
		fprintf(stderr, "Error: --no-check must be associated with "
			"--write option.\n");
		exit_status = 1;
		goto my_exit;
	}

	if (page_type_dump) {
		fil_page_type = create_file(page_dump_filename);
		if (!fil_page_type) {
			exit_status = 1;
			goto my_exit;
		}
	}

	if (is_log_enabled) {
		log_file = create_file(log_filename);
		if (!log_file) {
			exit_status = 1;
			goto my_exit;
		}
		fprintf(log_file, "InnoDB File Checksum Utility.\n");
	}

	if (verbose) {
		my_print_variables(innochecksum_options);
	}


	buf_ptr = (byte*) malloc(UNIV_PAGE_SIZE_MAX * 2);
	xdes_ptr = (byte*)malloc(UNIV_PAGE_SIZE_MAX * 2);
	buf = (byte *) ut_align(buf_ptr, UNIV_PAGE_SIZE_MAX);
	xdes = (byte *) ut_align(xdes_ptr, UNIV_PAGE_SIZE_MAX);

	/* The file name is not optional. */
	for (int i = 0; i < argc; ++i) {

		/* Reset parameters for each file. */
		filename = argv[i];
		memset(&page_type, 0, sizeof(innodb_page_type));
		partial_page_read = false;
		skip_page = false;

		if (is_log_enabled) {
			fprintf(log_file, "Filename = %s\n", filename);
		}

		if (*filename == '-') {
			/* read from stdin. */
			fil_in = stdin;
			read_from_stdin = true;

		}

		/* stat the file to get size and page count. */
		if (!read_from_stdin &&
#ifdef _WIN32
			_stat64(filename, &st)) {
#else
			stat(filename, &st)) {
#endif /* _WIN32 */
			fprintf(stderr, "Error: %s cannot be found\n",
				filename);

			exit_status = 1;
			goto my_exit;
		}

		if (!read_from_stdin) {
			size = st.st_size;
			fil_in = open_file(filename);
			/*If fil_in is NULL, terminate as some error encountered */
			if(fil_in == NULL) {
				exit_status = 1;
				goto my_exit;
			}
			/* Save the current file pointer in pos variable.*/
			if (0 != fgetpos(fil_in, &pos)) {
				perror("fgetpos");
				exit_status = 1;
				goto my_exit;
			}
		}

		/* Read the minimum page size. */
		bytes = fread(buf, 1, UNIV_ZIP_SIZE_MIN, fil_in);
		partial_page_read = true;

		if (bytes != UNIV_ZIP_SIZE_MIN) {
			fprintf(stderr, "Error: Was not able to read the "
				"minimum page size ");
			fprintf(stderr, "of %d bytes.  Bytes read was " ULINTPF "\n",
				UNIV_ZIP_SIZE_MIN, bytes);

			exit_status = 1;
			goto my_exit;
		}

		/* enable variable is_system_tablespace when space_id of given
		file is zero. Use to skip the checksum verification and rewrite
		for doublewrite pages. */
		cur_space = mach_read_from_4(buf + FIL_PAGE_SPACE_ID);
		cur_page_num = mach_read_from_4(buf + FIL_PAGE_OFFSET);

		/* Determine page size, zip_size and page compression
		from fsp_flags and encryption metadata from page 0 */
		const page_size_t&	page_size = get_page_size(buf);

		ulint flags = mach_read_from_4(FSP_HEADER_OFFSET + FSP_SPACE_FLAGS + buf);
		ulint zip_size = page_size.is_compressed() ? page_size.logical() : 0;
		logical_page_size = page_size.is_compressed() ? zip_size : 0;
		physical_page_size = page_size.physical();
		srv_page_size = page_size.logical();
		bool is_compressed = FSP_FLAGS_HAS_PAGE_COMPRESSION(flags);

		if (physical_page_size == UNIV_ZIP_SIZE_MIN) {
			partial_page_read = false;
		} else {
			/* Read rest of the page 0 to determine crypt_data */
			bytes = ulong(read_file(buf, partial_page_read, page_size.physical(), fil_in));

			if (bytes != page_size.physical()) {
				fprintf(stderr, "Error: Was not able to read the "
					"rest of the page ");
				fprintf(stderr, "of " ULINTPF " bytes.  Bytes read was " ULINTPF "\n",
					page_size.physical() - UNIV_ZIP_SIZE_MIN, bytes);

				exit_status = 1;
				goto my_exit;
			}
			partial_page_read = false;
		}


		/* Now that we have full page 0 in buffer, check encryption */
		bool is_encrypted = check_encryption(filename, page_size, buf);

		/* Verify page 0 contents. Note that we can't allow
		checksum mismatch on page 0, because that would mean we
		could not trust it content. */
		if (!no_check) {
			unsigned long long tmp_allow_mismatches = allow_mismatches;
			allow_mismatches = 0;

			exit_status = verify_checksum(
				buf, page_size, is_encrypted,
				is_compressed, &mismatch_count);

			if (exit_status) {
				fprintf(stderr, "Error: Page 0 checksum mismatch, can't continue. \n");
				goto my_exit;
			}
			allow_mismatches = tmp_allow_mismatches;
		}

		if ((exit_status = rewrite_checksum(filename, fil_in, buf,
					page_size, &pos, is_encrypted, is_compressed))) {
			goto my_exit;
		}

		if (page_type_dump) {
			fprintf(fil_page_type,
				"\n\nFilename::%s\n", filename);
			fprintf(fil_page_type,
				"========================================"
				"======================================\n");
			fprintf(fil_page_type,
				"\tPAGE_NO\t\t|\t\tPAGE_TYPE\t\t"
				"\t|\tEXTRA INFO\n");
			fprintf(fil_page_type,
				"========================================"
				"======================================\n");
		}

		if (per_page_details) {
			printf("page " UINT32PF " ", cur_page_num);
		}

		memcpy(xdes, buf, physical_page_size);

		if (page_type_summary || page_type_dump) {
			parse_page(buf, xdes, fil_page_type, page_size, is_encrypted);
		}

		pages = uint32_t(size / page_size.physical());

		if (just_count) {
			fprintf(read_from_stdin ? stderr : stdout,
				"Number of pages:" UINT32PF "\n", pages);
			continue;
		} else if (verbose && !read_from_stdin) {
			if (is_log_enabled) {
				fprintf(log_file, "file %s = %llu bytes "
					"(" UINT32PF " pages)\n",
					filename, size, pages);
				if (do_one_page) {
					fprintf(log_file, "Innochecksum: "
						"checking page::"
						UINT32PF ";\n",
						do_page);
				}
			}
		} else {
			if (is_log_enabled) {
				fprintf(log_file, "Innochecksum: checking "
					"pages in range::" UINT32PF
					" to " UINT32PF "\n",
					start_page, use_end_page ?
					end_page : (pages - 1));
			}
		}

		off_t cur_offset = 0;
		/* Find the first non all-zero page and fetch the
		space id from there. */
		while (is_page_all_zeroes(buf, physical_page_size)) {
			bytes = ulong(read_file(
					buf, false, physical_page_size,
					fil_in));

			if (feof(fil_in)) {
				fprintf(stderr, "All are "
					"zero-filled pages.");
				goto my_exit;
			}

			cur_offset++;
		}

		cur_space = mach_read_from_4(buf + FIL_PAGE_SPACE_ID);
		is_system_tablespace = (cur_space == 0);

		if (cur_offset > 0) {
			/* Re-read the non-zero page to check the
			checksum. So move the file pointer to
			previous position and reset the page number too. */
			cur_page_num = mach_read_from_4(buf + FIL_PAGE_OFFSET);
			if (!start_page) {
				goto first_non_zero;
			}
		}

		/* seek to the necessary position */
		if (start_page) {
			if (!read_from_stdin) {
				/* If read is not from stdin, we can use
				fseeko() to position the file pointer to
				the desired page. */
				partial_page_read = false;

				offset = off_t(ulonglong(start_page)
					       * page_size.physical());
#ifdef _WIN32
				if (_fseeki64(fil_in, offset, SEEK_SET)) {
#else
				if (fseeko(fil_in, offset, SEEK_SET)) {
#endif /* _WIN32 */
					perror("Error: Unable to seek to "
						"necessary offset");

					exit_status = 1;
					goto my_exit;
				}
				/* Save the current file pointer in
				pos variable. */
				if (0 != fgetpos(fil_in, &pos)) {
					perror("fgetpos");

					exit_status = 1;
					goto my_exit;
				}
			} else {

				ulong count = 0;

				while (!feof(fil_in)) {
					if (start_page == count) {
						break;
					}
					/* We read a part of page to find the
					minimum page size. We cannot reset
					the file pointer to the beginning of
					the page if we are reading from stdin
					(fseeko() on stdin doesn't work). So
					read only the remaining part of page,
					if partial_page_read is enable. */
					bytes = read_file(buf,
							  partial_page_read,
							  static_cast<ulong>(
								  page_size.physical()),
							  fil_in);

					partial_page_read = false;
					count++;

					if (!bytes || feof(fil_in)) {
						goto unexpected_eof;
					}
				}
			}
		}

		/* main checksumming loop */
		cur_page_num = start_page ? start_page : cur_page_num + 1;

		while (!feof(fil_in)) {

			bytes = read_file(buf, partial_page_read,
					  static_cast<ulong>(
						  page_size.physical()), fil_in);
			partial_page_read = false;

			if (!bytes && feof(fil_in)) {
				if (cur_page_num == start_page) {
unexpected_eof:
					fputs("Error: Unable "
					      "to seek to necessary offset\n",
					      stderr);

					exit_status = 1;
					goto my_exit;
				}
				break;
			}

			if (ferror(fil_in)) {
				fprintf(stderr, "Error reading " ULINTPF " bytes",
					page_size.physical());
				perror(" ");

				exit_status = 1;
				goto my_exit;
			}

			if (bytes != page_size.physical()) {
				fprintf(stderr, "Error: bytes read (" ULINTPF ") "
					"doesn't match page size (" ULINTPF ")\n",
					bytes, page_size.physical());
				exit_status = 1;
				goto my_exit;
			}

first_non_zero:
			if (is_system_tablespace) {
				/* enable when page is double write buffer.*/
				skip_page = is_page_doublewritebuffer(buf);
			} else {
				skip_page = false;
			}

			ulint cur_page_type = mach_read_from_2(buf+FIL_PAGE_TYPE);

			/* FIXME: Page compressed or Page compressed and encrypted
			pages do not contain checksum. */
			if (cur_page_type == FIL_PAGE_PAGE_COMPRESSED ||
			    cur_page_type == FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED) {
				skip_page = true;
			}

			/* If no-check is enabled, skip the
			checksum verification.*/
			if (!no_check &&
			    !is_page_free(xdes, page_size, cur_page_num) &&
			    !skip_page &&
			    (exit_status = verify_checksum(
						buf, page_size,
						is_encrypted, is_compressed,
						&mismatch_count))) {
				goto my_exit;
			}

			if ((exit_status = rewrite_checksum(filename, fil_in, buf,
						page_size, &pos, is_encrypted, is_compressed))) {
				goto my_exit;
			}

			/* end if this was the last page we were supposed to check */
			if (use_end_page && (cur_page_num >= end_page)) {
				break;
			}

			if (per_page_details) {
				printf("page " UINT32PF " ", cur_page_num);
			}

			if (page_get_page_no(buf) % physical_page_size == 0) {
				memcpy(xdes, buf, physical_page_size);
			}

			if (page_type_summary || page_type_dump) {
				parse_page(buf, xdes, fil_page_type, page_size, is_encrypted);
			}

			/* do counter increase and progress printing */
			cur_page_num++;

			if (verbose && !read_from_stdin) {
				if ((cur_page_num % 64) == 0) {
					time_t now = time(0);
					if (!lastt) {
						lastt= now;
					} else if (now - lastt >= 1 && is_log_enabled) {
						fprintf(log_file, "page::" UINT32PF " "
							"okay: %.3f%% done\n",
							(cur_page_num - 1),
							(double) cur_page_num / pages * 100);
						lastt = now;
					}
				}
			}
		}

		if (!read_from_stdin) {
			/* flcose() will flush the data and release the lock if
			any acquired. */
			fclose(fil_in);
		}

		/* Enabled for page type summary. */
		if (page_type_summary) {
			if (!read_from_stdin) {
				fprintf(stdout, "\nFile::%s",filename);
				print_summary(stdout);
			} else {
				print_summary(stderr);
			}
		}
	}

	if (is_log_enabled) {
		fclose(log_file);
	}

	free(buf_ptr);
	free(xdes_ptr);

	my_end(exit_status);
	DBUG_RETURN(exit_status);

my_exit:
	if (buf_ptr) {
		free(buf_ptr);
	}

	if (xdes_ptr) {
		free(xdes_ptr);
	}

	if (!read_from_stdin && fil_in) {
		fclose(fil_in);
	}

	if (log_file) {
		fclose(log_file);
	}

	my_end(exit_status);
	DBUG_RETURN(exit_status);
}
