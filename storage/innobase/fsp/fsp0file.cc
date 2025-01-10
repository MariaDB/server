/*****************************************************************************

Copyright (c) 2013, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2022, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file fsp/fsp0file.cc
Tablespace data file implementation

Created 2013-7-26 by Kevin Lewis
*******************************************************/

#include "fil0fil.h"
#include "fsp0types.h"
#include "os0file.h"
#include "page0page.h"
#include "srv0start.h"
#include "log.h"

/** Release the resources. */
void
Datafile::shutdown()
{
	close();

	free_filepath();
	free_first_page();
}

/** Create/open a data file.
@param[in]	read_only_mode	if true, then readonly mode checks are enforced.
@return DB_SUCCESS or error code */
dberr_t
Datafile::open_or_create(bool read_only_mode)
{
	bool success;
	ut_a(m_filepath != NULL);
	ut_ad(m_handle == OS_FILE_CLOSED);

	m_handle = os_file_create(
		innodb_data_file_key, m_filepath, m_open_flags,
		OS_DATA_FILE, read_only_mode, &success);

	if (!success) {
		m_last_os_error = os_file_get_last_error(true);
		ib::error() << "Cannot open datafile '" << m_filepath << "'";
		return(DB_CANNOT_OPEN_FILE);
	}

	return(DB_SUCCESS);
}

/** Open a data file in read-only mode to check if it exists so that it
can be validated.
@param[in]	strict	whether to issue error messages
@return DB_SUCCESS or error code */
dberr_t
Datafile::open_read_only(bool strict)
{
	bool	success = false;
	ut_ad(m_handle == OS_FILE_CLOSED);

	/* This function can be called for file objects that do not need
	to be opened, which is the case when the m_filepath is NULL */
	if (m_filepath == NULL) {
		return(DB_ERROR);
	}

	set_open_flags(OS_FILE_OPEN);
	m_handle = os_file_create_simple_no_error_handling(
		innodb_data_file_key, m_filepath, m_open_flags,
		OS_FILE_READ_ONLY, true, &success);

	if (success) {
		m_exists = true;
		init_file_info();

		return(DB_SUCCESS);
	}

	if (strict) {
		m_last_os_error = os_file_get_last_error(true);
		ib::error() << "Cannot open datafile for read-only: '"
			<< m_filepath << "' OS error: " << m_last_os_error;
	}

	return(DB_CANNOT_OPEN_FILE);
}

/** Open a data file in read-write mode during start-up so that
doublewrite pages can be restored and then it can be validated.*
@return DB_SUCCESS or error code */
inline dberr_t Datafile::open_read_write()
{
	bool	success = false;
	ut_ad(m_handle == OS_FILE_CLOSED);
	ut_ad(!srv_read_only_mode);

	/* This function can be called for file objects that do not need
	to be opened, which is the case when the m_filepath is NULL */
	if (m_filepath == NULL) {
		return(DB_ERROR);
	}

	set_open_flags(OS_FILE_OPEN);
	m_handle = os_file_create_simple_no_error_handling(
		innodb_data_file_key, m_filepath, m_open_flags,
		OS_FILE_READ_WRITE, false, &success);

	if (!success) {
		m_last_os_error = os_file_get_last_error(true);
		ib::error() << "Cannot open datafile for read-write: '"
			<< m_filepath << "'";
		return(DB_CANNOT_OPEN_FILE);
	}

	m_exists = true;

	init_file_info();

	return(DB_SUCCESS);
}

/** Initialize OS specific file info. */
void
Datafile::init_file_info()
{
#ifdef _WIN32
	GetFileInformationByHandle((os_file_t)m_handle, &m_file_info);
#else
	fstat(m_handle, &m_file_info);
#endif	/* WIN32 */
}

/** Close a data file.
@return DB_SUCCESS or error code */
dberr_t
Datafile::close()
{
	if (m_handle != OS_FILE_CLOSED) {
		ibool	success = os_file_close(m_handle);
		ut_a(success);

		m_handle = OS_FILE_CLOSED;
	}

	return(DB_SUCCESS);
}

/** Make a full filepath from a directory path and a filename.
Prepend the dirpath to filename using the extension given.
If dirpath is NULL, prepend the default datadir to filepath.
Store the result in m_filepath.
@param dirpath  directory path
@param name     tablespace (table) name
@param ext      filename extension */
void Datafile::make_filepath(const char *dirpath, fil_space_t::name_type name,
                             ib_extention ext)
{
  ut_ad(dirpath || name.size());
  free_filepath();
  m_filepath= fil_make_filepath(dirpath, name, ext, false);
  ut_ad(m_filepath);
  set_filename();
}

/** Set the filepath by duplicating the filepath sent in. This is the
name of the file with its extension and absolute or relative path.
@param[in]	filepath	filepath to set */
void
Datafile::set_filepath(const char* filepath)
{
	free_filepath();
	m_filepath = static_cast<char*>(ut_malloc_nokey(strlen(filepath) + 1));
	::strcpy(m_filepath, filepath);
	set_filename();
}

/** Free the filepath buffer. */
void
Datafile::free_filepath()
{
	if (m_filepath != NULL) {
		ut_free(m_filepath);
		m_filepath = NULL;
		m_filename = NULL;
	}
}

/** Do a quick test if the filepath provided looks the same as this filepath
byte by byte. If they are two different looking paths to the same file,
same_as() will be used to show that after the files are opened.
@param[in]	other	filepath to compare with
@retval true if it is the same filename by byte comparison
@retval false if it looks different */
bool
Datafile::same_filepath_as(
	const char* other) const
{
	return(0 == strcmp(m_filepath, other));
}

/** Test if another opened datafile is the same file as this object.
@param[in]	other	Datafile to compare with
@return true if it is the same file, else false */
bool
Datafile::same_as(
	const Datafile&	other) const
{
#ifdef _WIN32
	return(m_file_info.dwVolumeSerialNumber
	       == other.m_file_info.dwVolumeSerialNumber
	       && m_file_info.nFileIndexHigh
	          == other.m_file_info.nFileIndexHigh
	       && m_file_info.nFileIndexLow
	          == other.m_file_info.nFileIndexLow);
#else
	return(m_file_info.st_ino == other.m_file_info.st_ino
	       && m_file_info.st_dev == other.m_file_info.st_dev);
#endif /* WIN32 */
}

dberr_t Datafile::read_first_page_flags(const page_t *page) noexcept
{
  ut_ad(m_order == 0);

  if (memcmp_aligned<2>(FIL_PAGE_SPACE_ID + page,
                        FSP_HEADER_OFFSET + FSP_SPACE_ID + page, 4))
  {
     sql_print_error("InnoDB: Inconsistent tablespace ID in %s", m_filepath);
     return DB_CORRUPTION;
  }

  m_space_id= mach_read_from_4(FIL_PAGE_SPACE_ID + page);
  m_flags= fsp_header_get_flags(page);
  if (!fil_space_t::is_valid_flags(m_flags, m_space_id))
  {
    uint32_t cflags= fsp_flags_convert_from_101(m_flags);
    if (cflags == UINT32_MAX)
      switch (fsp_flags_is_incompatible_mysql(m_flags)) {
      case 0:
        sql_print_error("InnoDB: Invalid flags 0x%" PRIx32 " in %s",
                        m_flags, m_filepath);
        return DB_CORRUPTION;
      case 3:
      case 2:
        sql_print_error("InnoDB: MySQL-8.0 tablespace in %s", m_filepath);
        goto unsupported;
      case 1:
        sql_print_error("InnoDB: MySQL Encrypted tablespace in %s",
                        m_filepath);
      unsupported:
        sql_print_error("InnoDB: Restart in MySQL for migration/recovery.");
        return DB_UNSUPPORTED;
      }
    else
      m_flags= cflags;
  }

  return DB_SUCCESS;
}

/** Reads a few significant fields from the first page of the first
datafile.  The Datafile must already be open.
@param[in]	read_only_mode	If true, then readonly mode checks are enforced.
@return DB_SUCCESS or DB_IO_ERROR if page cannot be read */
dberr_t
Datafile::read_first_page(bool read_only_mode)
{
	if (m_handle == OS_FILE_CLOSED) {

		dberr_t err = open_or_create(read_only_mode);

		if (err != DB_SUCCESS) {
			return(err);
		}
	}

	/* Align the memory for a possible read from a raw device */

	m_first_page = static_cast<byte*>(
		aligned_malloc(UNIV_PAGE_SIZE_MAX, srv_page_size));

	dberr_t		err = DB_ERROR;
	size_t		page_size = UNIV_PAGE_SIZE_MAX;

	/* Don't want unnecessary complaints about partial reads. */

	while (page_size >= UNIV_PAGE_SIZE_MIN) {

		ulint	n_read = 0;

		err = os_file_read(
			IORequestReadPartial, m_handle, m_first_page, 0,
			page_size, &n_read);

		if (err == DB_SUCCESS) {
			break;
		}

		if (err == DB_IO_ERROR && n_read == 0) {
			break;
		}
		if (err == DB_IO_ERROR && n_read >= UNIV_PAGE_SIZE_MIN) {
			page_size >>= 1;
		} else if (srv_operation == SRV_OPERATION_BACKUP) {
			break;
		} else {
			ib::info() << "Cannot read first page of '"
				<< m_filepath << "': " << err;
			break;
		}
	}

	if (err == DB_SUCCESS && m_order == 0) {
		err = read_first_page_flags(m_first_page);
	}

	if (err == DB_SUCCESS
	    && fil_space_t::physical_size(m_flags) > page_size) {
		ib::error() << "File " << m_filepath
			<< " should be longer than "
			<< page_size << " bytes";
		err = DB_CORRUPTION;
	}

	return(err);
}

/** Free the first page from memory when it is no longer needed. */
void Datafile::free_first_page()
{
  aligned_free(m_first_page);
  m_first_page= nullptr;
}

/** Validates the datafile and checks that it conforms with the expected
space ID and flags.  The file should exist and be successfully opened
in order for this function to validate it.
@param[in]	space_id	The expected tablespace ID.
@param[in]	flags		The expected tablespace flags.
@retval DB_SUCCESS if tablespace is valid, DB_ERROR if not.
m_is_valid is also set true on success, else false. */
dberr_t Datafile::validate_to_dd(uint32_t space_id, uint32_t flags)
{
	dberr_t err;

	if (!is_open()) {
		return DB_ERROR;
	}

	/* Validate this single-table-tablespace with the data dictionary,
	but do not compare the DATA_DIR flag, in case the tablespace was
	remotely located. */
	err = validate_first_page(m_first_page);
	if (err != DB_SUCCESS) {
		return(err);
	}

	flags &= ~FSP_FLAGS_MEM_MASK;

	/* Make sure the datafile we found matched the space ID.
	If the datafile is a file-per-table tablespace then also match
	the row format and zip page size. */
	if (m_space_id == space_id
	    && (fil_space_t::is_flags_equal(flags, m_flags)
		|| fil_space_t::is_flags_equal(m_flags, flags))) {
		/* Datafile matches the tablespace expected. */
		return(DB_SUCCESS);
	}

	/* else do not use this tablespace. */
	m_is_valid = false;

	ib::error() << "Refusing to load '" << m_filepath << "' (id="
		<< m_space_id << ", flags=" << ib::hex(m_flags)
		<< "); dictionary contains id="
		<< space_id << ", flags=" << ib::hex(flags);

	return(DB_ERROR);
}

inline
uint32_t recv_dblwr_t::find_first_page(const char *name, pfs_os_file_t file)
  const noexcept
{
  os_offset_t file_size= os_file_get_size(file);
  if (file_size != (os_offset_t) -1)
  {
    for (const page_t *page : pages)
    {
      uint32_t space_id= page_get_space_id(page);
      byte *read_page= nullptr;
      if (page_get_page_no(page) > 0 || space_id == 0)
      {
next_page:
        aligned_free(read_page);
        continue;
      }
      uint32_t flags= mach_read_from_4(FSP_HEADER_OFFSET + FSP_SPACE_FLAGS +
                                       page);
      size_t page_size= fil_space_t::physical_size(flags);
      if (file_size < 4 * page_size)
        goto next_page;
      read_page=
        static_cast<byte*>(aligned_malloc(3 * page_size, page_size));
      /* Read 3 pages from the file and match the space id
      with the space id which is stored in
      doublewrite buffer page. */
      if (os_file_read(IORequestRead, file, read_page, page_size,
                       3 * page_size, nullptr) != DB_SUCCESS)
        goto next_page;
      for (ulint j= 0; j <= 2; j++)
      {
        byte *cur_page= read_page + j * page_size;
        if (buf_is_zeroes(span<const byte>(cur_page, page_size)))
        {
          aligned_free(read_page);
          return 0;
        }
        if (mach_read_from_4(cur_page + FIL_PAGE_OFFSET) != j + 1 ||
            memcmp(cur_page + FIL_PAGE_SPACE_ID,
                   page + FIL_PAGE_SPACE_ID, 4) ||
            buf_page_is_corrupted(false, cur_page, flags))
          goto next_page;
      }

      aligned_free(read_page);
      page= find_page(page_id_t{space_id, 0}, LSN_MAX);

      if (!page)
      {
        /* If the first page of the given user tablespace is not there
        in the doublewrite buffer, then the recovery is going to fail
        now. Report error only when doublewrite buffer is not empty */
        sql_print_error("InnoDB: Corrupted page "
                        "[page id: space=" UINT32PF ", page number=0]"
                        " of datafile '%s' could not be found"
                        " in the doublewrite buffer", space_id, name);
        break;
      }

      return space_id;
    }
  }
  return 0;
}

/** Validates this datafile for the purpose of recovery.  The file should
exist and be successfully opened. We initially open it in read-only mode
because we just want to read the SpaceID.  However, if the first page is
corrupt and needs to be restored from the doublewrite buffer, we will
reopen it in write mode and ry to restore that page.
@retval DB_SUCCESS if tablespace is valid, DB_ERROR if not.
m_is_valid is also set true on success, else false. */
dberr_t
Datafile::validate_for_recovery()
{
	dberr_t err;

	ut_ad(is_open());
	ut_ad(!srv_read_only_mode);

	err = validate_first_page(m_first_page);

	switch (err) {
	case DB_TABLESPACE_EXISTS:
		break;
	case DB_SUCCESS:
		if (!m_defer || !m_space_id) {
			break;
		}
		/* InnoDB should check whether the deferred
		tablespace page0 can be recovered from
		double write buffer. InnoDB should try
	        to recover only if m_space_id exists because
		dblwr pages can be searched via {space_id, 0}.
		m_space_id is set in read_first_page(). */
		/* fall through */
	default:
		const page_t *first_page = nullptr;

		if (!m_space_id) {
			m_space_id = recv_sys.dblwr.find_first_page(
				m_filepath, m_handle);
			if (m_space_id) {
				m_defer= false;
				goto free_first_page;
			} else return err;
		}

		if (!m_defer) {
			err = find_space_id();
			if (err != DB_SUCCESS || m_space_id == 0) {
				sql_print_error(
					"InnoDB: Datafile '%s' is corrupted."
					" Cannot determine the space ID from"
					" the first 64 pages.", m_filepath);
				return(err);
			}
		}

		if (m_space_id == UINT32_MAX) {
			return DB_SUCCESS; /* empty file */
		}

		first_page = recv_sys.dblwr.find_page(
			page_id_t(m_space_id, 0), LSN_MAX);

		if (!first_page) {
			return m_defer ? err : DB_CORRUPTION;
		}
free_first_page:
		/* Free the previously read first page and then re-validate. */
		free_first_page();
		m_defer = false;
		err = validate_first_page(first_page);
	}

	return(err);
}

/** Check the consistency of the first page of a datafile when the
tablespace is opened.  This occurs before the fil_space_t is created
so the Space ID found here must not already be open.
m_is_valid is set true on success, else false.
@param[in]	first_page	the contents of the first page
@retval DB_SUCCESS on if the datafile is valid
@retval DB_CORRUPTION if the datafile is not readable
@retval DB_TABLESPACE_EXISTS if there is a duplicate space_id */
dberr_t Datafile::validate_first_page(const page_t *first_page) noexcept
{
	const char*	error_txt = NULL;

	m_is_valid = true;
	ut_ad(first_page || !m_first_page);

	if (first_page) {
		if (dberr_t err = read_first_page_flags(first_page)) {
			m_is_valid = false;
			return err;
		}
	} else if (read_first_page(srv_read_only_mode) != DB_SUCCESS) {
		error_txt = "Cannot read first page";
err_exit:
		free_first_page();

		if (recv_recovery_is_on()
		    || srv_operation == SRV_OPERATION_BACKUP) {
			m_defer= true;
			return DB_SUCCESS;
		}

		sql_print_information(
			"InnoDB: %s in datafile: %s, Space ID: " UINT32PF
			", " "Flags: " UINT32PF,
			error_txt, m_filepath, m_space_id, m_flags);
		m_is_valid = false;
		return DB_CORRUPTION;
	} else {
		first_page = m_first_page;
		ut_ad(first_page);
	}

	/* Check if the whole page is blank. */
	if (!m_space_id && !m_flags) {
		const byte*	b		= first_page;
		ulint		nonzero_bytes	= srv_page_size;

		while (*b == '\0' && --nonzero_bytes != 0) {

			b++;
		}

		if (nonzero_bytes == 0) {
			error_txt = "Header page consists of zero bytes";
			goto err_exit;
		}
	}

	if (!fil_space_t::is_valid_flags(m_flags, m_space_id)) {
		/* Tablespace flags must be valid. */
		error_txt = "Tablespace flags are invalid";
		goto err_exit;
	}

	ulint logical_size = fil_space_t::logical_size(m_flags);

	if (srv_page_size != logical_size) {
		free_first_page();
		if (recv_recovery_is_on()
		    || srv_operation == SRV_OPERATION_BACKUP) {
			m_defer= true;
			return DB_SUCCESS;
		}
		/* Logical size must be innodb_page_size. */
		ib::error()
			<< "Data file '" << m_filepath << "' uses page size "
			<< logical_size << ", but the innodb_page_size"
			" start-up parameter is "
			<< srv_page_size;
		return(DB_ERROR);
	}

	if (page_get_page_no(first_page) != 0) {
		/* First page must be number 0 */
		error_txt = "Header page contains inconsistent data";
		goto err_exit;
	}

	if (m_space_id >= SRV_SPACE_ID_UPPER_BOUND) {
		error_txt = "A bad Space ID was found";
		goto err_exit;
	}

	switch (buf_page_is_corrupted(false, first_page, m_flags)) {
	case NOT_CORRUPTED:
		break;
	case CORRUPTED_FUTURE_LSN:
		error_txt = "LSN is in the future";
		goto err_exit;
	case CORRUPTED_OTHER:
		error_txt = "Checksum mismatch";
		goto err_exit;
	}

	mysql_mutex_lock(&fil_system.mutex);

	fil_space_t* space = fil_space_get_by_id(m_space_id);

	if (space) {
		fil_node_t* node = UT_LIST_GET_FIRST(space->chain);

		if (node && !strcmp(m_filepath, node->name)) {
ok_exit:
			mysql_mutex_unlock(&fil_system.mutex);
			return DB_SUCCESS;
		}

		if (!m_space_id
		    && (recv_recovery_is_on()
			|| srv_operation == SRV_OPERATION_BACKUP)) {
			m_defer= true;
			goto ok_exit;
		}

		/* Make sure the space_id has not already been opened. */
		ib::error() << "Attempted to open a previously opened"
			" tablespace. Previous tablespace: "
			    << (node ? node->name : "(unknown)")
			    << " uses space ID: " << m_space_id
			    << ". Cannot open filepath: " << m_filepath
			    << " which uses the same space ID.";
	}

	mysql_mutex_unlock(&fil_system.mutex);

	if (space) {
		m_is_valid = false;

		free_first_page();

		return(is_predefined_tablespace(m_space_id)
		       ? DB_CORRUPTION
		       : DB_TABLESPACE_EXISTS);
	}

	return(DB_SUCCESS);
}

/** Determine the space id of the given file descriptor by reading a few
pages from the beginning of the .ibd file.
@return DB_SUCCESS if space id was successfully identified, else DB_ERROR. */
dberr_t
Datafile::find_space_id()
{
	os_offset_t	file_size;

	ut_ad(m_handle != OS_FILE_CLOSED);

	file_size = os_file_get_size(m_handle);

	if (!file_size) {
		return DB_SUCCESS;
	}

	if (file_size == (os_offset_t) -1) {
		ib::error() << "Could not get file size of datafile '"
			<< m_filepath << "'";
		return(DB_CORRUPTION);
	}

	/* Assuming a page size, read the space_id from each page and store it
	in a map.  Find out which space_id is agreed on by majority of the
	pages.  Choose that space_id. */
	for (ulint page_size = UNIV_ZIP_SIZE_MIN;
	     page_size <= UNIV_PAGE_SIZE_MAX;
	     page_size <<= 1) {
		/* map[space_id] = count of pages */
		typedef std::map<
			uint32_t,
			uint32_t,
			std::less<uint32_t>,
			ut_allocator<std::pair<const uint32_t, uint32_t> > >
			Pages;

		Pages	verify;
		uint32_t page_count = 64;
		uint32_t valid_pages = 0;

		/* Adjust the number of pages to analyze based on file size */
		while ((page_count * page_size) > file_size) {
			--page_count;
		}

		ib::info()
			<< "Page size:" << page_size
			<< ". Pages to analyze:" << page_count;

		byte*	page = static_cast<byte*>(
			aligned_malloc(page_size, page_size));

		uint32_t fsp_flags;
		/* provide dummy value if the first os_file_read() fails */
		switch (srv_checksum_algorithm) {
		case SRV_CHECKSUM_ALGORITHM_STRICT_FULL_CRC32:
		case SRV_CHECKSUM_ALGORITHM_FULL_CRC32:
			fsp_flags = 1U << FSP_FLAGS_FCRC32_POS_MARKER
				| FSP_FLAGS_FCRC32_PAGE_SSIZE()
				| uint(innodb_compression_algorithm)
				       << FSP_FLAGS_FCRC32_POS_COMPRESSED_ALGO;
			break;
		default:
			fsp_flags = 0;
		}

		for (ulint j = 0; j < page_count; ++j) {
			if (os_file_read(IORequestRead, m_handle, page,
					 j * page_size, page_size, nullptr)) {
				ib::info()
					<< "READ FAIL: page_no:" << j;
				continue;
			}

			if (j == 0) {
				fsp_flags = mach_read_from_4(
					page + FSP_HEADER_OFFSET + FSP_SPACE_FLAGS);
			}

			bool	noncompressed_ok = false;

			/* For noncompressed pages, the page size must be
			equal to srv_page_size. */
			if (page_size == srv_page_size
			    && !fil_space_t::zip_size(fsp_flags)) {
				noncompressed_ok = !buf_page_is_corrupted(
					false, page, fsp_flags);
			}

			bool	compressed_ok = false;

			if (srv_page_size <= UNIV_PAGE_SIZE_DEF
			    && page_size == fil_space_t::zip_size(fsp_flags)) {
				compressed_ok = !buf_page_is_corrupted(
					false, page, fsp_flags);
			}

			if (noncompressed_ok || compressed_ok) {

				uint32_t space_id = mach_read_from_4(page
					+ FIL_PAGE_SPACE_ID);

				if (space_id > 0) {

					ib::info()
						<< "VALID: space:"
						<< space_id << " page_no:" << j
						<< " page_size:" << page_size;

					++valid_pages;

					++verify[space_id];
				}
			}
		}

		aligned_free(page);

		ib::info()
			<< "Page size: " << page_size
			<< ". Possible space_id count:" << verify.size();

		const ulint	pages_corrupted = 3;

		for (ulint missed = 0; missed <= pages_corrupted; ++missed) {

			for (Pages::const_iterator it = verify.begin();
			     it != verify.end();
			     ++it) {

				ib::info() << "space_id:" << it->first
					<< ", Number of pages matched: "
					<< it->second << "/" << valid_pages
					<< " (" << page_size << ")";

				if (it->second == (valid_pages - missed)) {
					ib::info() << "Chosen space:"
						<< it->first;

					m_space_id = it->first;
					return(DB_SUCCESS);
				}
			}

		}
	}

	return(DB_CORRUPTION);
}

/** Read an InnoDB Symbolic Link (ISL) file by name.
@param link_filepath   filepath of the ISL file
@return data file name (must be freed by the caller)
@retval nullptr  on error */
static char *read_link_file(const char *link_filepath)
{
  if (FILE* file= fopen(link_filepath, "r+b" STR_O_CLOEXEC))
  {
    char *filepath= static_cast<char*>(ut_malloc_nokey(OS_FILE_MAX_PATH));

    os_file_read_string(file, filepath, OS_FILE_MAX_PATH);
    fclose(file);

    if (size_t len= strlen(filepath))
    {
      /* Trim whitespace from end of filepath */
      len--;
      while (static_cast<byte>(filepath[len]) <= 0x20)
      {
        if (!len)
          return nullptr;
        filepath[len--]= 0;
      }
      /* Ensure that the last 2 path separators are forward slashes,
      because elsewhere we are assuming that tablespace file names end
      in "/databasename/tablename.ibd". */
      unsigned trailing_slashes= 0;
      for (; len; len--)
      {
        switch (filepath[len]) {
#ifdef _WIN32
        case '\\':
          filepath[len]= '/';
          /* fall through */
#endif
        case '/':
          if (++trailing_slashes >= 2)
            return filepath;
        }
      }
    }
  }

  return nullptr;
}

/** Create a link filename,
open that file, and read the contents into m_filepath.
@param name   table name
@return filepath()
@retval nullptr  if the .isl file does not exist or cannot be read */
const char *RemoteDatafile::open_link_file(const fil_space_t::name_type name)
{
  if (!m_link_filepath)
    m_link_filepath= fil_make_filepath(nullptr, name, ISL, false);
  m_filepath= read_link_file(m_link_filepath);
  return m_filepath;
}

/** Release the resources. */
void
RemoteDatafile::shutdown()
{
	Datafile::shutdown();

	if (m_link_filepath != 0) {
		ut_free(m_link_filepath);
		m_link_filepath = 0;
	}
}

/** Create InnoDB Symbolic Link (ISL) file.
@param name     tablespace name
@param filepath full file name
@return DB_SUCCESS or error code */
dberr_t RemoteDatafile::create_link_file(fil_space_t::name_type name,
                                         const char *filepath)
{
	bool		success;
	dberr_t		err = DB_SUCCESS;
	char*		link_filepath = NULL;
	char*		prev_filepath = NULL;

	ut_ad(!srv_read_only_mode);

	link_filepath = fil_make_filepath(NULL, name, ISL, false);

	if (link_filepath == NULL) {
		return(DB_ERROR);
	}

	prev_filepath = read_link_file(link_filepath);
	if (prev_filepath) {
		/* Truncate (starting with MySQL 5.6, probably no
		longer since MariaDB Server 10.2.19) used to call this
		with an existing link file which contains the same filepath. */
		bool same = !strncmp(prev_filepath, name.data(), name.size())
			&& !strcmp(prev_filepath + name.size(), DOT_IBD);
		ut_free(prev_filepath);
		if (same) {
			ut_free(link_filepath);
			return(DB_SUCCESS);
		}
	}

	/** Check if the file already exists. */
	FILE*			file = NULL;
	bool			exists;
	os_file_type_t		ftype;

	success = os_file_status(link_filepath, &exists, &ftype);
	ulint error = 0;

	if (success && !exists) {

		file = fopen(link_filepath, "w");
		if (file == NULL) {
			/* This call will print its own error message */
			error = os_file_get_last_error(true);
		}
	} else {
		error = OS_FILE_ALREADY_EXISTS;
	}

	if (error != 0) {

		ib::error() << "Cannot create file " << link_filepath << ".";

		if (error == OS_FILE_ALREADY_EXISTS) {
			ib::error() << "The link file: " << link_filepath
				<< " already exists.";
			err = DB_TABLESPACE_EXISTS;

		} else if (error == OS_FILE_DISK_FULL) {
			err = DB_OUT_OF_FILE_SPACE;

		} else {
			err = DB_ERROR;
		}

		/* file is not open, no need to close it. */
		ut_free(link_filepath);
		return(err);
	}

	const size_t len = strlen(filepath);
	if (fwrite(filepath, 1, len, file) != len) {
		error = os_file_get_last_error(true);
		ib::error() <<
			"Cannot write link file: "
			    << link_filepath << " filepath: " << filepath;
		err = DB_ERROR;
	}

	/* Close the file, we only need it at startup */
	fclose(file);

	ut_free(link_filepath);

	return(err);
}

/** Delete an InnoDB Symbolic Link (ISL) file. */
void
RemoteDatafile::delete_link_file(void)
{
	ut_ad(m_link_filepath != NULL);

	if (m_link_filepath != NULL) {
		os_file_delete_if_exists(innodb_data_file_key,
					 m_link_filepath, NULL);
	}
}

/** Delete an InnoDB Symbolic Link (ISL) file by name.
@param name	tablespace name */
void RemoteDatafile::delete_link_file(fil_space_t::name_type name)
{
  if (char *link_filepath= fil_make_filepath(NULL, name, ISL, false))
  {
    os_file_delete_if_exists(innodb_data_file_key, link_filepath, nullptr);
    ut_free(link_filepath);
  }
}
