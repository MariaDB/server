/******************************************************
XtraBackup: hot backup tool for InnoDB
(c) 2009-2012 Percona Inc.
Originally Created 3/3/2009 Yasufumi Kinoshita
Written by Alexey Kopytov, Aleksandr Kuzminsky, Stewart Smith, Vadim Tkachenko,
Yasufumi Kinoshita, Ignacio Nin and Baron Schwartz.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA

*******************************************************/

/* Changed page bitmap implementation */

#include "changed_page_bitmap.h"

#include "common.h"
#include "xtrabackup.h"

/* TODO: copy-pasted shared definitions from the XtraDB bitmap write code.
Remove these on the first opportunity, i.e. single-binary XtraBackup.  */

/* log0online.h */

/** Single bitmap file information */
struct log_online_bitmap_file_t {
	char		name[FN_REFLEN];	/*!< Name with full path */
	pfs_os_file_t	file;			/*!< Handle to opened file */
	ib_uint64_t	size;			/*!< Size of the file */
	ib_uint64_t	offset;			/*!< Offset of the next read,
						or count of already-read bytes
						*/
};

/** A set of bitmap files containing some LSN range */
struct log_online_bitmap_file_range_t {
	size_t	count;				/*!< Number of files */
	/*!< Dynamically-allocated array of info about individual files */
	struct files_t {
		char	name[FN_REFLEN];/*!< Name of a file */
		lsn_t	start_lsn;	/*!< Starting LSN of data in this
					file */
		ulong	seq_num;	/*!< Sequence number of this file */
	}	*files;
};

/* log0online.c */

/** File name stem for bitmap files. */
static const char* bmp_file_name_stem = "ib_modified_log_";

/** The bitmap file block size in bytes.  All writes will be multiples of this.
 */
enum {
	MODIFIED_PAGE_BLOCK_SIZE = 4096
};

/** Offsets in a file bitmap block */
enum {
	MODIFIED_PAGE_IS_LAST_BLOCK = 0,/* 1 if last block in the current
					write, 0 otherwise. */
	MODIFIED_PAGE_START_LSN = 4,	/* The starting tracked LSN of this and
					other blocks in the same write */
	MODIFIED_PAGE_END_LSN = 12,	/* The ending tracked LSN of this and
					other blocks in the same write */
	MODIFIED_PAGE_SPACE_ID = 20,	/* The space ID of tracked pages in
					this block */
	MODIFIED_PAGE_1ST_PAGE_ID = 24,	/* The page ID of the first tracked
					page in this block */
	MODIFIED_PAGE_BLOCK_UNUSED_1 = 28,/* Unused in order to align the start
					  of bitmap at 8 byte boundary */
	MODIFIED_PAGE_BLOCK_BITMAP = 32,/* Start of the bitmap itself */
	MODIFIED_PAGE_BLOCK_UNUSED_2 = MODIFIED_PAGE_BLOCK_SIZE - 8,
					/* Unused in order to align the end of
					bitmap at 8 byte boundary */
	MODIFIED_PAGE_BLOCK_CHECKSUM = MODIFIED_PAGE_BLOCK_SIZE - 4
					/* The checksum of the current block */
};

/** Length of the bitmap data in a block */
enum { MODIFIED_PAGE_BLOCK_BITMAP_LEN
       = MODIFIED_PAGE_BLOCK_UNUSED_2 - MODIFIED_PAGE_BLOCK_BITMAP };

/** Length of the bitmap data in a block in page ids */
enum { MODIFIED_PAGE_BLOCK_ID_COUNT = MODIFIED_PAGE_BLOCK_BITMAP_LEN * 8 };

typedef ib_uint64_t	bitmap_word_t;

/****************************************************************//**
Calculate a bitmap block checksum.  Algorithm borrowed from
log_block_calc_checksum.
@return checksum */
UNIV_INLINE
ulint
log_online_calc_checksum(
/*=====================*/
	const byte*	block);	/*!<in: bitmap block */

/****************************************************************//**
Provide a comparisson function for the RB-tree tree (space,
block_start_page) pairs.  Actual implementation does not matter as
long as the ordering is full.
@return -1 if p1 < p2, 0 if p1 == p2, 1 if p1 > p2
*/
static
int
log_online_compare_bmp_keys(
/*========================*/
	const void* p1,	/*!<in: 1st key to compare */
	const void* p2)	/*!<in: 2nd key to compare */
{
	const byte *k1 = (const byte *)p1;
	const byte *k2 = (const byte *)p2;

	ulint k1_space = mach_read_from_4(k1 + MODIFIED_PAGE_SPACE_ID);
	ulint k2_space = mach_read_from_4(k2 + MODIFIED_PAGE_SPACE_ID);
	if (k1_space == k2_space) {

		ulint k1_start_page
			= mach_read_from_4(k1 + MODIFIED_PAGE_1ST_PAGE_ID);
		ulint k2_start_page
			= mach_read_from_4(k2 + MODIFIED_PAGE_1ST_PAGE_ID);
		return k1_start_page < k2_start_page
			? -1 : k1_start_page > k2_start_page ? 1 : 0;
	}
	return k1_space < k2_space ? -1 : 1;
}

/****************************************************************//**
Calculate a bitmap block checksum.  Algorithm borrowed from
log_block_calc_checksum.
@return checksum */
UNIV_INLINE
ulint
log_online_calc_checksum(
/*=====================*/
	const byte*	block)	/*!<in: bitmap block */
{
	ulint	sum;
	ulint	sh;
	ulint	i;

	sum = 1;
	sh = 0;

	for (i = 0; i < MODIFIED_PAGE_BLOCK_CHECKSUM; i++) {

		ulint	b = block[i];
		sum &= 0x7FFFFFFFUL;
		sum += b;
		sum += b << sh;
		sh++;
		if (sh > 24) {

			sh = 0;
		}
	}

	return sum;
}

/****************************************************************//**
Read one bitmap data page and check it for corruption.

@return TRUE if page read OK, FALSE if I/O error */
static
ibool
log_online_read_bitmap_page(
/*========================*/
	log_online_bitmap_file_t	*bitmap_file,	/*!<in/out: bitmap
							file */
	byte				*page,	/*!<out: read page.  Must be at
						least MODIFIED_PAGE_BLOCK_SIZE
						bytes long */
	ibool				*checksum_ok)	/*!<out: TRUE if page
							checksum OK */
{
	ulint	checksum;
	ulint	actual_checksum;
	ibool	success;

	ut_a(bitmap_file->size >= MODIFIED_PAGE_BLOCK_SIZE);
	ut_a(bitmap_file->offset
	     <= bitmap_file->size - MODIFIED_PAGE_BLOCK_SIZE);
	ut_a(bitmap_file->offset % MODIFIED_PAGE_BLOCK_SIZE == 0);
	success = os_file_read(IORequestRead,
			       bitmap_file->file, page, bitmap_file->offset,
			       MODIFIED_PAGE_BLOCK_SIZE) == DB_SUCCESS;

	if (UNIV_UNLIKELY(!success)) {

		/* The following call prints an error message */
		os_file_get_last_error(TRUE);
		msg("InnoDB: Warning: failed reading changed page bitmap "
		    "file \'%s\'", bitmap_file->name);
		return FALSE;
	}

	bitmap_file->offset += MODIFIED_PAGE_BLOCK_SIZE;
	ut_ad(bitmap_file->offset <= bitmap_file->size);

	checksum = mach_read_from_4(page + MODIFIED_PAGE_BLOCK_CHECKSUM);
	actual_checksum = log_online_calc_checksum(page);
	*checksum_ok = (checksum == actual_checksum);

	return TRUE;
}

/*********************************************************************//**
Check the name of a given file if it's a changed page bitmap file and
return file sequence and start LSN name components if it is.  If is not,
the values of output parameters are undefined.

@return TRUE if a given file is a changed page bitmap file.  */
static
ibool
log_online_is_bitmap_file(
/*======================*/
	const os_file_stat_t*	file_info,		/*!<in: file to
							check */
	ulong*			bitmap_file_seq_num,	/*!<out: bitmap file
							sequence number */
	lsn_t*			bitmap_file_start_lsn)	/*!<out: bitmap file
							start LSN */
{
	char	stem[FN_REFLEN];

	ut_ad (strlen(file_info->name) < OS_FILE_MAX_PATH);

	return ((file_info->type == OS_FILE_TYPE_FILE
		 || file_info->type == OS_FILE_TYPE_LINK)
		&& (sscanf(file_info->name, "%[a-z_]%lu_" LSN_PF ".xdb", stem,
			   bitmap_file_seq_num, bitmap_file_start_lsn) == 3)
		&& (!strcmp(stem, bmp_file_name_stem)));
}

/*********************************************************************//**
List the bitmap files in srv_data_home and setup their range that contains the
specified LSN interval.  This range, if non-empty, will start with a file that
has the greatest LSN equal to or less than the start LSN and will include all
the files up to the one with the greatest LSN less than the end LSN.  Caller
must free bitmap_files->files when done if bitmap_files set to non-NULL and
this function returned TRUE.  Field bitmap_files->count might be set to a
larger value than the actual count of the files, and space for the unused array
slots will be allocated but cleared to zeroes.

@return TRUE if succeeded
*/
static
ibool
log_online_setup_bitmap_file_range(
/*===============================*/
	log_online_bitmap_file_range_t	*bitmap_files,	/*!<in/out: bitmap file
							range */
	lsn_t				range_start,	/*!<in: start LSN */
	lsn_t				range_end)	/*!<in: end LSN */
{
	os_file_dir_t	bitmap_dir;
	os_file_stat_t	bitmap_dir_file_info;
	ulong		first_file_seq_num	= ULONG_MAX;
	ulong		last_file_seq_num	= 0;
	lsn_t		first_file_start_lsn	= LSN_MAX;

	xb_ad(range_end >= range_start);

	bitmap_files->count = 0;
	bitmap_files->files = NULL;

	/* 1st pass: size the info array */

	bitmap_dir = os_file_opendir(srv_data_home, FALSE);
	if (UNIV_UNLIKELY(!bitmap_dir)) {

		msg("InnoDB: Error: failed to open bitmap directory \'%s\'",
		    srv_data_home);
		return FALSE;
	}

	while (!os_file_readdir_next_file(srv_data_home, bitmap_dir,
					  &bitmap_dir_file_info)) {

		ulong	file_seq_num;
		lsn_t	file_start_lsn;

		if (!log_online_is_bitmap_file(&bitmap_dir_file_info,
					       &file_seq_num,
					       &file_start_lsn)
		    || file_start_lsn >= range_end) {

			continue;
		}

		if (file_seq_num > last_file_seq_num) {

			last_file_seq_num = file_seq_num;
		}

		if (file_start_lsn >= range_start
		    || file_start_lsn == first_file_start_lsn
		    || first_file_start_lsn > range_start) {

			/* A file that falls into the range */

			if (file_start_lsn < first_file_start_lsn) {

				first_file_start_lsn = file_start_lsn;
			}
			if (file_seq_num < first_file_seq_num) {

				first_file_seq_num = file_seq_num;
			}
		} else if (file_start_lsn > first_file_start_lsn) {

			/* A file that has LSN closer to the range start
			but smaller than it, replacing another such file */
			first_file_start_lsn = file_start_lsn;
			first_file_seq_num = file_seq_num;
		}
	}

	if (UNIV_UNLIKELY(os_file_closedir(bitmap_dir))) {

		os_file_get_last_error(TRUE);
		msg("InnoDB: Error: cannot close \'%s\'",srv_data_home);
		return FALSE;
	}

	if (first_file_seq_num == ULONG_MAX && last_file_seq_num == 0) {

		bitmap_files->count = 0;
		return TRUE;
	}

	bitmap_files->count = last_file_seq_num - first_file_seq_num + 1;

	/* 2nd pass: get the file names in the file_seq_num order */

	bitmap_dir = os_file_opendir(srv_data_home, FALSE);
	if (UNIV_UNLIKELY(!bitmap_dir)) {

		msg("InnoDB: Error: failed to open bitmap directory \'%s\'",
		    srv_data_home);
		return FALSE;
	}

	bitmap_files->files =
		static_cast<log_online_bitmap_file_range_t::files_t *>
		(malloc(bitmap_files->count * sizeof(bitmap_files->files[0])));
	memset(bitmap_files->files, 0,
	       bitmap_files->count * sizeof(bitmap_files->files[0]));

	while (!os_file_readdir_next_file(srv_data_home, bitmap_dir,
					  &bitmap_dir_file_info)) {

		ulong	file_seq_num;
		lsn_t	file_start_lsn;
		size_t	array_pos;

		if (!log_online_is_bitmap_file(&bitmap_dir_file_info,
					       &file_seq_num,
					       &file_start_lsn)
		    || file_start_lsn >= range_end
		    || file_start_lsn < first_file_start_lsn) {

			continue;
		}

		array_pos = file_seq_num - first_file_seq_num;
		if (UNIV_UNLIKELY(array_pos >= bitmap_files->count)) {

			msg("InnoDB: Error: inconsistent bitmap file "
			    "directory");
			os_file_closedir(bitmap_dir);
			free(bitmap_files->files);
			return FALSE;
		}

		if (file_seq_num > bitmap_files->files[array_pos].seq_num) {

			bitmap_files->files[array_pos].seq_num = file_seq_num;
			strncpy(bitmap_files->files[array_pos].name,
				bitmap_dir_file_info.name, FN_REFLEN - 1);
			bitmap_files->files[array_pos].name[FN_REFLEN - 1]
				= '\0';
			bitmap_files->files[array_pos].start_lsn
				= file_start_lsn;
		}
	}

	if (UNIV_UNLIKELY(os_file_closedir(bitmap_dir))) {

		os_file_get_last_error(TRUE);
		msg("InnoDB: Error: cannot close \'%s\'", srv_data_home);
		free(bitmap_files->files);
		return FALSE;
	}

#ifdef UNIV_DEBUG
	ut_ad(bitmap_files->files[0].seq_num == first_file_seq_num);

	for (size_t i = 1; i < bitmap_files->count; i++) {
		if (!bitmap_files->files[i].seq_num) {

			break;
		}
		ut_ad(bitmap_files->files[i].seq_num
		      > bitmap_files->files[i - 1].seq_num);
		ut_ad(bitmap_files->files[i].start_lsn
		      >= bitmap_files->files[i - 1].start_lsn);
	}
#endif

	return TRUE;
}

/****************************************************************//**
Open a bitmap file for reading.

@return whether opened successfully */
static
bool
log_online_open_bitmap_file_read_only(
/*==================================*/
	const char*			name,		/*!<in: bitmap file
							name without directory,
							which is assumed to be
							srv_data_home */
	log_online_bitmap_file_t*	bitmap_file)	/*!<out: opened bitmap
							file */
{
	bool	success	= false;

	xb_ad(name[0] != '\0');

	snprintf(bitmap_file->name, FN_REFLEN, "%s%s", srv_data_home, name);
	bitmap_file->file = os_file_create_simple_no_error_handling(
		0, bitmap_file->name,
		OS_FILE_OPEN, OS_FILE_READ_ONLY, true, &success);
	if (UNIV_UNLIKELY(!success)) {

		/* Here and below assume that bitmap file names do not
		contain apostrophes, thus no need for ut_print_filename(). */
		msg("InnoDB: Warning: error opening the changed page "
		    "bitmap \'%s\'", bitmap_file->name);
		return success;
	}

	bitmap_file->size = os_file_get_size(bitmap_file->file);
	bitmap_file->offset = 0;

#ifdef UNIV_LINUX
	posix_fadvise(bitmap_file->file, 0, 0, POSIX_FADV_SEQUENTIAL);
	posix_fadvise(bitmap_file->file, 0, 0, POSIX_FADV_NOREUSE);
#endif

	return success;
}

/****************************************************************//**
Diagnose one or both of the following situations if we read close to
the end of bitmap file:
1) Warn if the remainder of the file is less than one page.
2) Error if we cannot read any more full pages but the last read page
did not have the last-in-run flag set.

@return FALSE for the error */
static
ibool
log_online_diagnose_bitmap_eof(
/*===========================*/
	const log_online_bitmap_file_t*	bitmap_file,	/*!< in: bitmap file */
	ibool				last_page_in_run)/*!< in: "last page in
							run" flag value in the
							last read page */
{
	/* Check if we are too close to EOF to read a full page */
	if ((bitmap_file->size < MODIFIED_PAGE_BLOCK_SIZE)
	    || (bitmap_file->offset
		> bitmap_file->size - MODIFIED_PAGE_BLOCK_SIZE)) {

		if (UNIV_UNLIKELY(bitmap_file->offset != bitmap_file->size)) {

			/* If we are not at EOF and we have less than one page
			to read, it's junk.  This error is not fatal in
			itself. */

			msg("InnoDB: Warning: junk at the end of changed "
			    "page bitmap file \'%s\'.", bitmap_file->name);
		}

		if (UNIV_UNLIKELY(!last_page_in_run)) {

			/* We are at EOF but the last read page did not finish
			a run */
			/* It's a "Warning" here because it's not a fatal error
			for the whole server */
			msg("InnoDB: Warning: changed page bitmap "
			    "file \'%s\' does not contain a complete run "
			    "at the end.", bitmap_file->name);
			return FALSE;
		}
	}
	return TRUE;
}

/* End of copy-pasted definitions */

/** Iterator structure over changed page bitmap */
struct xb_page_bitmap_range_struct {
	const xb_page_bitmap	*bitmap;	/* Bitmap with data */
	ulint			space_id;	/* Space id for this
					        iterator */
	ulint			bit_i;		/* Bit index of the iterator
						position in the current page */
	const ib_rbt_node_t	*bitmap_node;	/* Current bitmap tree node */
	const byte		*bitmap_page;	/* Current bitmap page */
	ulint			current_page_id;/* Current page id */
};

/****************************************************************//**
Print a diagnostic message on missing bitmap data for an LSN range.  */
static
void
xb_msg_missing_lsn_data(
/*====================*/
	lsn_t	missing_interval_start,	/*!<in: interval start */
	lsn_t	missing_interval_end)	/*!<in: interval end */
{
	msg("mariabackup: warning: changed page data missing for LSNs between "
	    LSN_PF " and " LSN_PF, missing_interval_start,
	    missing_interval_end);
}

/****************************************************************//**
Scan a bitmap file until data for a desired LSN or EOF is found and check that
the page before the starting one is not corrupted to ensure that the found page
indeed contains the very start of the desired LSN data.  The caller must check
the page LSN values to determine if the bitmap file was scanned until the data
was found or until EOF.  Page must be at least MODIFIED_PAGE_BLOCK_SIZE big.

@return TRUE if the scan successful without corruption detected
*/
static
ibool
xb_find_lsn_in_bitmap_file(
/*=======================*/
	log_online_bitmap_file_t	*bitmap_file,	/*!<in/out: bitmap
							file */
	byte				*page,		/*!<in/out: last read
							bitmap page */
	lsn_t				*page_end_lsn,	/*!<out: end LSN of the
							last read page */
	lsn_t				lsn)		/*!<in: LSN to find */
{
	ibool		last_page_ok		= TRUE;
	ibool		next_to_last_page_ok	= TRUE;

	xb_ad (bitmap_file->size >= MODIFIED_PAGE_BLOCK_SIZE);

	*page_end_lsn = 0;

	while ((*page_end_lsn <= lsn)
	       && (bitmap_file->offset
		   <= bitmap_file->size - MODIFIED_PAGE_BLOCK_SIZE)) {

		next_to_last_page_ok = last_page_ok;
		if (!log_online_read_bitmap_page(bitmap_file, page,
						 &last_page_ok)) {

			return FALSE;
		}

		*page_end_lsn = mach_read_from_8(page + MODIFIED_PAGE_END_LSN);
	}

	/* We check two pages here because the last read page already contains
	the required LSN data. If the next to the last one page is corrupted,
	then we have no way of telling if that page contained the required LSN
	range data too */
	return last_page_ok && next_to_last_page_ok;
}

/****************************************************************//**
Read the disk bitmap and build the changed page bitmap tree for the
LSN interval incremental_lsn to checkpoint_lsn_start.

@return the built bitmap tree or NULL if unable to read the full interval for
any reason. */
xb_page_bitmap*
xb_page_bitmap_init(void)
/*=====================*/
{
	log_online_bitmap_file_t	bitmap_file;
	lsn_t				bmp_start_lsn	= incremental_lsn;
	lsn_t				bmp_end_lsn	= checkpoint_lsn_start;
	byte				page[MODIFIED_PAGE_BLOCK_SIZE];
	lsn_t				current_page_end_lsn;
	xb_page_bitmap			*result;
	ibool				last_page_in_run= FALSE;
	log_online_bitmap_file_range_t	bitmap_files;
	size_t				bmp_i;
	ibool				last_page_ok	= TRUE;

	if (UNIV_UNLIKELY(bmp_start_lsn > bmp_end_lsn)) {

		msg("mariabackup: incremental backup LSN " LSN_PF
		    " is larger than than the last checkpoint LSN " LSN_PF
		    , bmp_start_lsn, bmp_end_lsn);
		return NULL;
	}

	if (!log_online_setup_bitmap_file_range(&bitmap_files, bmp_start_lsn,
						bmp_end_lsn)) {

		return NULL;
	}

	/* Only accept no bitmap files returned if start LSN == end LSN */
	if (bitmap_files.count == 0 && bmp_end_lsn != bmp_start_lsn) {

		return NULL;
	}

	result = rbt_create(MODIFIED_PAGE_BLOCK_SIZE,
			    log_online_compare_bmp_keys);

	if (bmp_start_lsn == bmp_end_lsn) {

		/* Empty range - empty bitmap */
		return result;
	}

	bmp_i = 0;

	if (UNIV_UNLIKELY(bitmap_files.files[bmp_i].start_lsn
			  > bmp_start_lsn)) {

		/* The 1st file does not have the starting LSN data */
		xb_msg_missing_lsn_data(bmp_start_lsn,
					bitmap_files.files[bmp_i].start_lsn);
		rbt_free(result);
		free(bitmap_files.files);
		return NULL;
	}

	/* Skip any zero-sized files at the start */
	while ((bmp_i < bitmap_files.count - 1)
	       && (bitmap_files.files[bmp_i].start_lsn
		   == bitmap_files.files[bmp_i + 1].start_lsn)) {

		bmp_i++;
	}

	/* Is the 1st bitmap file missing? */
	if (UNIV_UNLIKELY(bitmap_files.files[bmp_i].name[0] == '\0')) {

		/* TODO: this is not the exact missing range */
		xb_msg_missing_lsn_data(bmp_start_lsn, bmp_end_lsn);
		rbt_free(result);
		free(bitmap_files.files);
		return NULL;
	}

	/* Open the 1st bitmap file */
	if (UNIV_UNLIKELY(!log_online_open_bitmap_file_read_only(
				  bitmap_files.files[bmp_i].name,
				  &bitmap_file))) {

		rbt_free(result);
		free(bitmap_files.files);
		return NULL;
	}

	/* If the 1st file is truncated, no data.  Not merged with the case
	below because zero-length file indicates not a corruption but missing
	subsequent files instead.  */
	if (UNIV_UNLIKELY(bitmap_file.size < MODIFIED_PAGE_BLOCK_SIZE)) {

		xb_msg_missing_lsn_data(bmp_start_lsn, bmp_end_lsn);
		rbt_free(result);
		free(bitmap_files.files);
		os_file_close(bitmap_file.file);
		return NULL;
	}

	/* Find the start of the required LSN range in the file */
	if (UNIV_UNLIKELY(!xb_find_lsn_in_bitmap_file(&bitmap_file, page,
						      &current_page_end_lsn,
						      bmp_start_lsn))) {

		msg("mariabackup: Warning: changed page bitmap file "
		    "\'%s\' corrupted", bitmap_file.name);
		rbt_free(result);
		free(bitmap_files.files);
		os_file_close(bitmap_file.file);
		return NULL;
	}

	last_page_in_run
		= mach_read_from_4(page + MODIFIED_PAGE_IS_LAST_BLOCK);

	if (UNIV_UNLIKELY(!log_online_diagnose_bitmap_eof(&bitmap_file,
							  last_page_in_run))) {

		rbt_free(result);
		free(bitmap_files.files);
		os_file_close(bitmap_file.file);
		return NULL;
	}

	if (UNIV_UNLIKELY(current_page_end_lsn < bmp_start_lsn)) {

		xb_msg_missing_lsn_data(current_page_end_lsn, bmp_start_lsn);
		rbt_free(result);
		free(bitmap_files.files);
		os_file_close(bitmap_file.file);
		return NULL;
	}

	/* 1st bitmap page found, add it to the tree.  */
	rbt_insert(result, page, page);

	/* Read next pages/files until all required data is read */
	while (last_page_ok
	       && (current_page_end_lsn < bmp_end_lsn
		   || (current_page_end_lsn == bmp_end_lsn
		       && !last_page_in_run))) {

		ib_rbt_bound_t	tree_search_pos;

		/* If EOF, advance the file skipping over any empty files */
		while (bitmap_file.size < MODIFIED_PAGE_BLOCK_SIZE
		       || (bitmap_file.offset
			   > bitmap_file.size - MODIFIED_PAGE_BLOCK_SIZE)) {

			os_file_close(bitmap_file.file);

			if (UNIV_UNLIKELY(
				!log_online_diagnose_bitmap_eof(
					&bitmap_file, last_page_in_run))) {

				rbt_free(result);
				free(bitmap_files.files);
				return NULL;
			}

			bmp_i++;

			if (UNIV_UNLIKELY(bmp_i == bitmap_files.count
					  || (bitmap_files.files[bmp_i].seq_num
					      == 0))) {

				xb_msg_missing_lsn_data(current_page_end_lsn,
							bmp_end_lsn);
				rbt_free(result);
				free(bitmap_files.files);
				return NULL;
			}

			/* Is the next file missing? */
			if (UNIV_UNLIKELY(bitmap_files.files[bmp_i].name[0]
					  == '\0')) {

				/* TODO: this is not the exact missing range */
				xb_msg_missing_lsn_data(bitmap_files.files
							[bmp_i - 1].start_lsn,
							bmp_end_lsn);
				rbt_free(result);
				free(bitmap_files.files);
				return NULL;
			}

			if (UNIV_UNLIKELY(
				    !log_online_open_bitmap_file_read_only(
					    bitmap_files.files[bmp_i].name,
					    &bitmap_file))) {

				rbt_free(result);
				free(bitmap_files.files);
				return NULL;
			}
		}

		if (UNIV_UNLIKELY(
			    !log_online_read_bitmap_page(&bitmap_file, page,
							 &last_page_ok))) {

			rbt_free(result);
			free(bitmap_files.files);
			os_file_close(bitmap_file.file);
			return NULL;
		}

		if (UNIV_UNLIKELY(!last_page_ok)) {

			msg("mariabackup: warning: changed page bitmap file "
			    "\'%s\' corrupted.", bitmap_file.name);
			rbt_free(result);
			free(bitmap_files.files);
			os_file_close(bitmap_file.file);
			return NULL;
		}

		/* Merge the current page with an existing page or insert a new
		page into the tree */

		if (!rbt_search(result, &tree_search_pos, page)) {

			/* Merge the bitmap pages */
			byte	*existing_page
				= rbt_value(byte, tree_search_pos.last);
			bitmap_word_t *bmp_word_1	= (bitmap_word_t *)
				(existing_page + MODIFIED_PAGE_BLOCK_BITMAP);
			bitmap_word_t *bmp_end = (bitmap_word_t *)
				(existing_page + MODIFIED_PAGE_BLOCK_UNUSED_2);
			bitmap_word_t *bmp_word_2	= (bitmap_word_t *)
				(page + MODIFIED_PAGE_BLOCK_BITMAP);
			while (bmp_word_1 < bmp_end) {

				*bmp_word_1++ |= *bmp_word_2++;
			}
			xb_a (bmp_word_1 == bmp_end);
		} else {

			/* Add a new page */
			rbt_add_node(result, &tree_search_pos, page);
		}

		current_page_end_lsn
			= mach_read_from_8(page + MODIFIED_PAGE_END_LSN);
		last_page_in_run
			= mach_read_from_4(page + MODIFIED_PAGE_IS_LAST_BLOCK);
	}

	xb_a (current_page_end_lsn >= bmp_end_lsn);

	free(bitmap_files.files);
	os_file_close(bitmap_file.file);

	return result;
}

/****************************************************************//**
Free the bitmap tree. */
void
xb_page_bitmap_deinit(
/*==================*/
	xb_page_bitmap*	bitmap)	/*!<in/out: bitmap tree */
{
	if (bitmap) {

		rbt_free(bitmap);
	}
}

/****************************************************************//**
Advance to the next bitmap page or setup the first bitmap page for the
given bitmap range.  Assumes that bitmap_range->bitmap_page has been
already found/bumped by rbt_search()/rbt_next().

@return FALSE if no more bitmap data for the range space ID */
static
ibool
xb_page_bitmap_setup_next_page(
/*===========================*/
	xb_page_bitmap_range*	bitmap_range)	/*!<in/out: the bitmap range */
{
	ulint	new_space_id;
	ulint	new_1st_page_id;

	if (bitmap_range->bitmap_node == NULL) {

		bitmap_range->current_page_id = ULINT_UNDEFINED;
		return FALSE;
	}

	bitmap_range->bitmap_page = rbt_value(byte, bitmap_range->bitmap_node);

	new_space_id = mach_read_from_4(bitmap_range->bitmap_page
					+ MODIFIED_PAGE_SPACE_ID);
	if (new_space_id != bitmap_range->space_id) {

		/* No more data for the current page id. */
		xb_a(new_space_id > bitmap_range->space_id);
		bitmap_range->current_page_id = ULINT_UNDEFINED;
		return FALSE;
	}

	new_1st_page_id = mach_read_from_4(bitmap_range->bitmap_page +
					   MODIFIED_PAGE_1ST_PAGE_ID);
	xb_a (new_1st_page_id >= bitmap_range->current_page_id
	      || bitmap_range->current_page_id == ULINT_UNDEFINED);

	bitmap_range->current_page_id = new_1st_page_id;
	bitmap_range->bit_i = 0;

	return TRUE;
}

/** Find the node with the smallest key that greater than equal to search key.
@param[in]	tree	red-black tree
@param[in]	key	search key
@return	node with the smallest greater-than-or-equal key
@retval	NULL	if none was found */
static
const ib_rbt_node_t*
rbt_lower_bound(const ib_rbt_t* tree, const void* key)
{
	ut_ad(!tree->cmp_arg);
	const ib_rbt_node_t* ge = NULL;

	for (const ib_rbt_node_t *node = tree->root->left;
	     node != tree->nil; ) {
		int	result = tree->compare(node->value, key);

		if (result < 0) {
			node = node->right;
		} else {
			ge = node;
			if (result == 0) {
				break;
			}

			node = node->left;
		}
	}

	return(ge);
}

/****************************************************************//**
Set up a new bitmap range iterator over a given space id changed
pages in a given bitmap.

@return bitmap range iterator */
xb_page_bitmap_range*
xb_page_bitmap_range_init(
/*======================*/
	xb_page_bitmap*	bitmap,		/*!< in: bitmap to iterate over */
	ulint		space_id)	/*!< in: space id */
{
	byte			search_page[MODIFIED_PAGE_BLOCK_SIZE];
	xb_page_bitmap_range	*result
		= static_cast<xb_page_bitmap_range *>(malloc(sizeof(*result)));

	memset(result, 0, sizeof(*result));
	result->bitmap = bitmap;
	result->space_id = space_id;
	result->current_page_id = ULINT_UNDEFINED;

	/* Search for the 1st page for the given space id */
	/* This also sets MODIFIED_PAGE_1ST_PAGE_ID to 0, which is what we
	want. */
	memset(search_page, 0, MODIFIED_PAGE_BLOCK_SIZE);
	mach_write_to_4(search_page + MODIFIED_PAGE_SPACE_ID, space_id);

	result->bitmap_node = rbt_lower_bound(result->bitmap, search_page);

	xb_page_bitmap_setup_next_page(result);

	return result;
}

/****************************************************************//**
Get the value of the bitmap->range->bit_i bitmap bit

@return the current bit value */
static inline
ibool
is_bit_set(
/*=======*/
	const xb_page_bitmap_range*	bitmap_range)	/*!< in: bitmap
							range */
{
	return ((*(((bitmap_word_t *)(bitmap_range->bitmap_page
				     + MODIFIED_PAGE_BLOCK_BITMAP))
		  + (bitmap_range->bit_i >> 6)))
		& (1ULL << (bitmap_range->bit_i & 0x3F))) ? TRUE : FALSE;
}

/****************************************************************//**
Get the next page id that has its bit set or cleared, i.e. equal to
bit_value.

@return page id */
ulint
xb_page_bitmap_range_get_next_bit(
/*==============================*/
	xb_page_bitmap_range*	bitmap_range,	/*!< in/out: bitmap range */
	ibool			bit_value)	/*!< in: bit value */
{
	if (UNIV_UNLIKELY(bitmap_range->current_page_id
			  == ULINT_UNDEFINED)) {

		return ULINT_UNDEFINED;
	}

	do {
		while (bitmap_range->bit_i < MODIFIED_PAGE_BLOCK_ID_COUNT) {

			while (is_bit_set(bitmap_range) != bit_value
			       && (bitmap_range->bit_i
				   < MODIFIED_PAGE_BLOCK_ID_COUNT)) {

				bitmap_range->current_page_id++;
				bitmap_range->bit_i++;
			}

			if (bitmap_range->bit_i
			    < MODIFIED_PAGE_BLOCK_ID_COUNT) {

				ulint result = bitmap_range->current_page_id;
				bitmap_range->current_page_id++;
				bitmap_range->bit_i++;
				return result;
			}
		}

		bitmap_range->bitmap_node
			= rbt_next(bitmap_range->bitmap,
				   bitmap_range->bitmap_node);

	} while (xb_page_bitmap_setup_next_page(bitmap_range));

	return ULINT_UNDEFINED;
}

/****************************************************************//**
Free the bitmap range iterator. */
void
xb_page_bitmap_range_deinit(
/*========================*/
	xb_page_bitmap_range*	bitmap_range)	/*! in/out: bitmap range */
{
	free(bitmap_range);
}
