/*****************************************************************************

Copyright (c) 2013, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2021, MariaDB Corporation.

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
@file include/fsp0space.h
Shared tablespace interface

Created 2013-7-26 by Kevin Lewis
*******************************************************/

#ifndef fsp0space_h
#define fsp0space_h

#include "fsp0file.h"
#include "fsp0fsp.h"
#include "fsp0types.h"

#include <vector>

/** Data structure that contains the information about shared tablespaces.
Currently this can be the system tablespace or a temporary table tablespace */
class Tablespace {

public:
	typedef std::vector<Datafile, ut_allocator<Datafile> >	files_t;

	/** Data file information - each Datafile can be accessed globally */
	files_t		m_files;
	/** Data file iterator */
	typedef files_t::iterator iterator;
	/** Data file iterator */
	typedef files_t::const_iterator const_iterator;

	Tablespace() {}

	virtual ~Tablespace()
	{
		shutdown();
		ut_ad(m_files.empty());
		ut_ad(m_space_id == UINT32_MAX);
	}

	// Disable copying
	Tablespace(const Tablespace&);
	Tablespace& operator=(const Tablespace&);

	/** Data file iterator */
	const_iterator begin() const { return m_files.begin(); }
	/** Data file iterator */
	const_iterator end() const { return m_files.end(); }
	/** Data file iterator */
	iterator begin() { return m_files.begin(); }
	/** Data file iterator */
	iterator end() { return m_files.end(); }

	/** Set tablespace path and filename members.
	@param[in]	path	where tablespace file(s) resides
	@param[in]	len	length of the file path */
	void set_path(const char* path, size_t len)
	{
		ut_ad(m_path == NULL);
		m_path = mem_strdupl(path, len);
		ut_ad(m_path != NULL);
	}

	/** Set tablespace path and filename members.
	@param[in]	path	where tablespace file(s) resides */
	void set_path(const char* path)
	{
		set_path(path, strlen(path));
	}

	/** Get tablespace path
	@return tablespace path */
	const char* path()	const
	{
		return(m_path);
	}

	/** Set the space id of the tablespace
	@param[in]	space_id	 tablespace ID to set */
	void set_space_id(uint32_t space_id)
	{
		ut_ad(m_space_id == UINT32_MAX);
		m_space_id = space_id;
	}

	/** Get the space id of the tablespace
	@return m_space_id space id of the tablespace */
	uint32_t space_id() const { return m_space_id; }

	/** Set the tablespace flags
	@param[in]	fsp_flags	tablespace flags */
	void set_flags(uint32_t fsp_flags)
	{
		ut_ad(fil_space_t::is_valid_flags(fsp_flags, false));
		m_flags = fsp_flags;
	}

	/** Get the tablespace flags
	@return m_flags tablespace flags */
	uint32_t flags() const { return m_flags; }

	/** Get the tablespace encryption mode
	@return m_mode tablespace encryption mode */
	fil_encryption_t encryption_mode() const { return m_mode; }

	/** Get the tablespace encryption key_id
	@return m_key_id tablespace encryption key_id */
	uint32_t key_id() const { return m_key_id; }

	/** Set Ignore Read Only Status for tablespace.
	@param[in]	read_only_status	read only status indicator */
	void set_ignore_read_only(bool read_only_status)
	{
		m_ignore_read_only = read_only_status;
	}

	/** Free the memory allocated by the Tablespace object */
	void shutdown();

	/** @return the sum of the file sizes of each Datafile */
	uint32_t get_sum_of_sizes() const
	{
		uint32_t sum = 0;

		for (const_iterator it = begin(); it != end(); ++it) {
			sum += it->m_size;
		}

		return(sum);
	}

	/** Open or Create the data files if they do not exist.
	@param[in]	is_temp	whether this is a temporary tablespace
	@return DB_SUCCESS or error code */
	dberr_t open_or_create(bool is_temp)
		MY_ATTRIBUTE((warn_unused_result));

	/** Delete all the data files. */
	void delete_files();

	/** Check if two tablespaces have common data file names.
	@param[in]	other_space	Tablespace to check against this.
	@return true if they have the same data filenames and paths */
	bool intersection(const Tablespace* other_space);

	/** Use the ADD DATAFILE path to create a Datafile object and add
	it to the front of m_files. Parse the datafile path into a path
	and a basename with extension 'ibd'. This datafile_path provided
	may be an absolute or relative path, but it must end with the
	extension .ibd and have a basename of at least 1 byte.

	Set tablespace m_path member and add a Datafile with the filename.
	@param[in]	datafile_path	full path of the tablespace file. */
	dberr_t add_datafile(
		const char*	datafile_path);

	/* Return a pointer to the first Datafile for this Tablespace
	@return pointer to the first Datafile for this Tablespace*/
	Datafile* first_datafile()
	{
		ut_a(!m_files.empty());
		return(&m_files.front());
	}
private:
	/**
	@param[in]	filename	Name to lookup in the data files.
	@return true if the filename exists in the data files */
	bool find(const char* filename) const;

	/** Note that the data file was found.
	@param[in]	file	data file object */
	void file_found(Datafile& file);

	/** Tablespace ID */
	uint32_t	m_space_id = UINT32_MAX;
	/** Tablespace flags */
	uint32_t	m_flags = UINT32_MAX;

	/** Path where tablespace files will reside, excluding a filename */
	char*		m_path;

	/** Encryption mode and key_id */
	fil_encryption_t m_mode;
	uint32_t	m_key_id;

protected:
	/** Ignore server read only configuration for this tablespace. */
	bool		m_ignore_read_only = false;
};

#endif /* fsp0space_h */
