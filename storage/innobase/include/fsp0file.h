/*****************************************************************************

Copyright (c) 2013, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2018, 2022, MariaDB Corporation.

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
@file include/fsp0file.h
Tablespace data file implementation.

Created 2013-7-26 by Kevin Lewis
*******************************************************/

#ifndef fsp0file_h
#define fsp0file_h

#include "mem0mem.h"
#include "os0file.h"
#include "fil0fil.h"

/** Types of raw partitions in innodb_data_file_path */
enum device_t {
	SRV_NOT_RAW = 0,	/*!< Not a raw partition */
	SRV_NEW_RAW,		/*!< A 'newraw' partition, only to be
				initialized */
	SRV_OLD_RAW		/*!< An initialized raw partition */
};

/** Data file control information. */
class Datafile {

	friend class Tablespace;
	friend class SysTablespace;

public:

	Datafile()
		:
		m_filepath(),
		m_filename(),
		m_handle(),
		m_open_flags(OS_FILE_OPEN),
		m_size(),
		m_order(),
		m_type(SRV_NOT_RAW),
		m_space_id(UINT32_MAX),
		m_flags(),
		m_exists(),
		m_is_valid(),
		m_first_page(),
		m_last_os_error(),
		m_file_info()
	{
		/* No op */
	}

	Datafile(uint32_t flags, uint32_t size, ulint order)
		:
		m_filepath(),
		m_filename(),
		m_handle(),
		m_open_flags(OS_FILE_OPEN),
		m_size(size),
		m_order(order),
		m_type(SRV_NOT_RAW),
		m_space_id(UINT32_MAX),
		m_flags(flags),
		m_exists(),
		m_is_valid(),
		m_first_page(),
		m_last_os_error(),
		m_file_info()
	{
	}

	Datafile(const Datafile& file)
		:
		m_handle(file.m_handle),
		m_open_flags(file.m_open_flags),
		m_size(file.m_size),
		m_order(file.m_order),
		m_type(file.m_type),
		m_space_id(file.m_space_id),
		m_flags(file.m_flags),
		m_exists(file.m_exists),
		m_is_valid(file.m_is_valid),
		m_first_page(),
		m_last_os_error(),
		m_file_info()
	{
		if (file.m_filepath != NULL) {
			m_filepath = mem_strdup(file.m_filepath);
			ut_a(m_filepath != NULL);
			set_filename();
		} else {
			m_filepath = NULL;
			m_filename = NULL;
		}
	}

	virtual ~Datafile()
	{
		shutdown();
	}

	Datafile& operator=(const Datafile& file)
	{
		ut_a(this != &file);

		m_size = file.m_size;
		m_order = file.m_order;
		m_type = file.m_type;

		ut_a(m_handle == OS_FILE_CLOSED);
		m_handle = file.m_handle;

		m_exists = file.m_exists;
		m_is_valid = file.m_is_valid;
		m_open_flags = file.m_open_flags;
		m_space_id = file.m_space_id;
		m_flags = file.m_flags;
		m_last_os_error = 0;

		if (m_filepath != NULL) {
			ut_free(m_filepath);
			m_filepath = NULL;
			m_filename = NULL;
		}

		if (file.m_filepath != NULL) {
			m_filepath = mem_strdup(file.m_filepath);
			ut_a(m_filepath != NULL);
			set_filename();
		}

		/* Do not make a copy of the first page,
		it should be reread if needed */
		m_first_page = NULL;

		return(*this);
	}

	/** Initialize the tablespace flags */
	void init(uint32_t flags) { m_flags= flags; }

	/** Release the resources. */
	virtual void shutdown();

	/** Open a data file in read-only mode to check if it exists
	so that it can be validated.
	@param[in]	strict	whether to issue error messages
	@return DB_SUCCESS or error code */
	dberr_t open_read_only(bool strict);

	/** Open a data file in read-write mode during start-up so that
	doublewrite pages can be restored and then it can be validated.
	@return DB_SUCCESS or error code */
	inline dberr_t open_read_write()
		MY_ATTRIBUTE((warn_unused_result));

	/** Initialize OS specific file info. */
	void init_file_info();

	/** Close a data file.
	@return DB_SUCCESS or error code */
	dberr_t close();

	/** Make a full filepath from a directory path and a filename.
	Prepend the dirpath to filename using the extension given.
	If dirpath is NULL, prepend the default datadir to filepath.
	Store the result in m_filepath.
	@param dirpath  directory path
	@param name     tablespace (table) name
	@param ext      filename extension */
	void make_filepath(const char* dirpath, fil_space_t::name_type name,
			   ib_extention ext);

	/** Set the filepath by duplicating the filepath sent in */
	void set_filepath(const char* filepath);

	/** Validates the datafile and checks that it conforms with
	the expected space ID and flags.  The file should exist and be
	successfully opened in order for this function to validate it.
	@param[in]	space_id	The expected tablespace ID.
	@param[in]	flags		The expected tablespace flags.
	@retval DB_SUCCESS if tablespace is valid, DB_ERROR if not.
	m_is_valid is also set true on success, else false. */
	dberr_t validate_to_dd(uint32_t space_id, uint32_t flags)
		MY_ATTRIBUTE((warn_unused_result));

	/** Validates this datafile for the purpose of recovery.
	The file should exist and be successfully opened. We initially
	open it in read-only mode because we just want to read the SpaceID.
	However, if the first page is corrupt and needs to be restored
	from the doublewrite buffer, we will reopen it in write mode and
	ry to restore that page.
	@retval DB_SUCCESS if tablespace is valid, DB_ERROR if not.
	m_is_valid is also set true on success, else false. */
	dberr_t validate_for_recovery()
		MY_ATTRIBUTE((warn_unused_result));

	/** Checks the consistency of the first page of a datafile when the
	tablespace is opened.  This occurs before the fil_space_t is created
	so the Space ID found here must not already be open.
	m_is_valid is set true on success, else false.
	@retval DB_SUCCESS on if the datafile is valid
	@retval DB_CORRUPTION if the datafile is not readable
	@retval DB_TABLESPACE_EXISTS if there is a duplicate space_id */
	dberr_t validate_first_page()
		MY_ATTRIBUTE((warn_unused_result));

	/** Get Datafile::m_filepath.
	@return m_filepath */
	const char*	filepath()	const
	{
		return(m_filepath);
	}

	/** Get Datafile::m_handle.
	@return m_handle */
	pfs_os_file_t	handle()	const
	{
		return(m_handle);
	}

	/** @return detached file handle */
	pfs_os_file_t detach()
	{
		pfs_os_file_t detached = m_handle;
		m_handle = OS_FILE_CLOSED;
		return detached;
	}

	/** Get Datafile::m_order.
	@return m_order */
	ulint	order()	const
	{
		return(m_order);
	}

	/** Get Datafile::m_space_id.
	@return m_space_id */
	uint32_t space_id() const { return m_space_id; }

	/** Get Datafile::m_flags.
	@return m_flags */
	uint32_t flags() const { return m_flags; }

	/**
	@return true if m_handle is open, false if not */
	bool is_open() const { return m_handle != OS_FILE_CLOSED; }

	/** Get Datafile::m_is_valid.
	@return m_is_valid */
	bool	is_valid()	const
	{
		return(m_is_valid);
	}

	/** Get the last OS error reported
	@return m_last_os_error */
	ulint	last_os_error()		const
	{
		return(m_last_os_error);
	}

	/** Check whether the file is empty.
	@return true if file is empty */
	bool	is_empty_file()		const
	{
#ifdef _WIN32
		os_offset_t	offset =
			(os_offset_t) m_file_info.nFileSizeLow
			| ((os_offset_t) m_file_info.nFileSizeHigh << 32);

		return (offset == 0);
#else
		return (m_file_info.st_size == 0);
#endif
	}

	/** Check if the file exist.
	@return true if file exists. */
	bool exists()	const { return m_exists; }

	/** Test if the filepath provided looks the same as this filepath
	by string comparison. If they are two different paths to the same
	file, same_as() will be used to show that after the files are opened.
	@param[in]	other	filepath to compare with
	@retval true if it is the same filename by char comparison
	@retval false if it looks different */
	bool same_filepath_as(const char* other) const;

	/** Test if another opened datafile is the same file as this object.
	@param[in]	other	Datafile to compare with
	@return true if it is the same file, else false */
	bool same_as(const Datafile&	other) const;

	/** Get access to the first data page.
	It is valid after open_read_only() succeeded.
	@return the first data page */
	const byte* get_first_page() const { return(m_first_page); }

	void set_space_id(uint32_t space_id) { m_space_id= space_id; }

	void set_flags(uint32_t flags) { m_flags = flags; }
private:
	/** Free the filepath buffer. */
	void free_filepath();

	/** Set the filename pointer to the start of the file name
	in the filepath. */
	void set_filename()
	{
		if (!m_filepath) {
			return;
		}

		if (char *last_slash = strrchr(m_filepath, '/')) {
#if _WIN32
			if (char *last = strrchr(m_filepath, '\\')) {
				if (last > last_slash) {
					last_slash = last;
				}
			}
#endif
			m_filename = last_slash + 1;
		} else {
			m_filename = m_filepath;
		}
	}

	/** Create/open a data file.
	@param[in]	read_only_mode	if true, then readonly mode checks
					are enforced.
	@return DB_SUCCESS or error code */
	dberr_t open_or_create(bool read_only_mode)
		MY_ATTRIBUTE((warn_unused_result));

	/** Reads a few significant fields from the first page of the
	datafile, which must already be open.
	@param[in]	read_only_mode	if true, then readonly mode checks
					are enforced.
	@return DB_SUCCESS or DB_IO_ERROR if page cannot be read */
	dberr_t read_first_page(bool read_only_mode)
		MY_ATTRIBUTE((warn_unused_result));

	/** Free the first page from memory when it is no longer needed. */
	void free_first_page();

	/** Set the Datafile::m_open_flags.
	@param open_flags	The Open flags to set. */
	void set_open_flags(os_file_create_t	open_flags)
	{
		m_open_flags = open_flags;
	};

	/** Determine if this datafile is on a Raw Device
	@return true if it is a RAW device. */
	bool is_raw_device()
	{
		return(m_type != SRV_NOT_RAW);
	}

	/* DATA MEMBERS */

protected:
	/** Physical file path with base name and extension */
	char*			m_filepath;

private:
	/** Determine the space id of the given file descriptor by reading
	a few pages from the beginning of the .ibd file.
	@return DB_SUCCESS if space id was successfully identified,
	else DB_ERROR. */
	dberr_t find_space_id();

	/** Restore the first page of the tablespace from
	the double write buffer.
	@return whether the operation failed */
	bool restore_from_doublewrite();

	/** Points into m_filepath to the file name with extension */
	char*			m_filename;

	/** Open file handle */
	pfs_os_file_t		m_handle;

	/** Flags to use for opening the data file */
	os_file_create_t	m_open_flags;

	/** size in megabytes or pages; converted from megabytes to
	pages in SysTablespace::normalize_size() */
	uint32_t		m_size;

	/** ordinal position of this datafile in the tablespace */
	ulint			m_order;

	/** The type of the data file */
	device_t		m_type;

	/** Tablespace ID. Contained in the datafile header.
	If this is a system tablespace, FSP_SPACE_ID is only valid
	in the first datafile. */
	uint32_t		m_space_id;

	/** Tablespace flags. Contained in the datafile header.
	If this is a system tablespace, FSP_SPACE_FLAGS are only valid
	in the first datafile. */
	uint32_t		m_flags;

	/** true if file already existed on startup */
	bool			m_exists;

	/* true if the tablespace is valid */
	bool			m_is_valid;

	/** Aligned buffer to hold first page */
	byte*			m_first_page;

protected:
	/** Last OS error received so it can be reported if needed. */
	ulint			m_last_os_error;

public:
	/** true if table is deferred during recovery */
	bool			m_defer=false;
	/** Use the following to determine the uniqueness of this datafile. */
#ifdef _WIN32
	/* Use fields dwVolumeSerialNumber, nFileIndexLow, nFileIndexHigh. */
	BY_HANDLE_FILE_INFORMATION	m_file_info;
#else
	/* Use field st_ino. */
	struct stat			m_file_info;
#endif	/* WIN32 */
};


/** Data file control information. */
class RemoteDatafile : public Datafile
{
private:
	/** Link filename (full path) */
	char*	m_link_filepath;

public:

	RemoteDatafile()
		:
		m_link_filepath()
	{
		/* No op - base constructor is called. */
	}

	RemoteDatafile(const char*, ulint, ulint)
		:
		m_link_filepath()
	{
		/* No op - base constructor is called. */
	}

	~RemoteDatafile() override
	{
		shutdown();
	}

	/** Release the resources. */
	void shutdown() override;

	/** Get the link filepath.
	@return m_link_filepath */
	const char*	link_filepath()	const
	{
		return(m_link_filepath);
	}

	/** Attempt to read the contents of an .isl file into m_filepath.
	@param name   table name
	@return filepath()
	@retval nullptr  if the .isl file does not exist or cannot be read */
	const char* open_link_file(const fil_space_t::name_type name);

	/** Delete an InnoDB Symbolic Link (ISL) file. */
	void delete_link_file(void);

	/******************************************************************
	Global Static Functions;  Cannot refer to data members.
	******************************************************************/

	/** Create InnoDB Symbolic Link (ISL) file.
	@param name     tablespace name
	@param filepath full file name
	@return DB_SUCCESS or error code */
	static dberr_t create_link_file(fil_space_t::name_type name,
					const char *filepath);

	/** Delete an InnoDB Symbolic Link (ISL) file by name.
	@param name   tablespace name */
	static void delete_link_file(fil_space_t::name_type name);
};
#endif /* fsp0file_h */
