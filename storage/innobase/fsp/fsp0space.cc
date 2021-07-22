/*****************************************************************************

Copyright (c) 2013, 2015, Oracle and/or its affiliates. All Rights Reserved.
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
@file fsp/fsp0space.cc
Shared tablespace implementation.

Created 2012-11-16 by Sunny Bains as srv/srv0space.cc
*******************************************************/

#include "fsp0sysspace.h"
#include "fsp0fsp.h"
#include "os0file.h"
#include "my_sys.h"

/** Check if two tablespaces have common data file names.
@param other_space	Tablespace to check against this.
@return true if they have the same data filenames and paths */
bool
Tablespace::intersection(
	const Tablespace*	other_space)
{
	for (files_t::const_iterator it(other_space->begin()),
		     end(other_space->end()); it != end; ++it) {

		if (find(it->m_filename)) {

			return(true);
		}
	}

	return(false);
}

/** Frees the memory allocated by the SysTablespace object. */
void
Tablespace::shutdown()
{
	for (iterator it = begin(); it != end(); ++it) {
		it->shutdown();
	}

	m_files.clear();
	ut_free(m_path);
	m_path = NULL;
	m_space_id = UINT32_MAX;
}

/** Note that the data file was found.
@param[in,out] file	Data file object to set */
void
Tablespace::file_found(Datafile& file)
{
	/* Note that the file exists and can be opened
	in the appropriate mode. */
	file.m_exists = true;

	file.set_open_flags(
		&file == &m_files.front()
		? OS_FILE_OPEN_RETRY : OS_FILE_OPEN);
}

/** Open or Create the data files if they do not exist.
@param[in]	is_temp	whether this is a temporary tablespace
@return DB_SUCCESS or error code */
dberr_t
Tablespace::open_or_create(bool is_temp)
{
	fil_space_t*		space = NULL;
	dberr_t			err = DB_SUCCESS;

	ut_ad(!m_files.empty());

	for (iterator it = begin(); it != end(); ++it) {

		if (it->m_exists) {
			err = it->open_or_create(
				m_ignore_read_only
				? false : srv_read_only_mode);
		} else {
			err = it->open_or_create(
				m_ignore_read_only
				? false : srv_read_only_mode);

			/* Set the correct open flags now that we have
			successfully created the file. */
			if (err == DB_SUCCESS) {
				file_found(*it);
			}
		}

		if (err != DB_SUCCESS) {
			break;
		}

		/* We can close the handle now and open the tablespace
		the proper way. */
		it->close();

		if (it == begin()) {
			/* First data file. */

			/* Create the tablespace entry for the multi-file
			tablespace in the tablespace manager. */
			uint32_t fsp_flags;

			switch (srv_checksum_algorithm) {
			case SRV_CHECKSUM_ALGORITHM_FULL_CRC32:
			case SRV_CHECKSUM_ALGORITHM_STRICT_FULL_CRC32:
				fsp_flags = (FSP_FLAGS_FCRC32_MASK_MARKER
					     | FSP_FLAGS_FCRC32_PAGE_SSIZE());
				break;
			default:
				fsp_flags = FSP_FLAGS_PAGE_SSIZE();
			}

			space = fil_space_t::create(
				m_space_id, fsp_flags,
				is_temp
				? FIL_TYPE_TEMPORARY : FIL_TYPE_TABLESPACE,
				NULL);
			if (!space) {
				return DB_ERROR;
			}
		}

		ut_a(fil_validate());

		space->add(it->m_filepath, OS_FILE_CLOSED, it->m_size,
			   false, true);
	}

	return(err);
}

/** Find a filename in the list of Datafiles for a tablespace
@return true if the filename exists in the data files */
bool
Tablespace::find(const char* filename) const
{
	for (const_iterator it = begin(); it != end(); ++it) {

		if (innobase_strcasecmp(filename, it->m_filename) == 0) {
			return(true);
		}
	}

	return(false);
}

/** Delete all the data files. */
void
Tablespace::delete_files()
{
	for (iterator it = begin(); it != end(); ++it) {

		it->close();

		bool file_pre_exists;
		bool success = os_file_delete_if_exists(
			innodb_data_file_key, it->m_filepath, &file_pre_exists);

		if (success && file_pre_exists) {
			ib::info() << "Removed temporary tablespace data"
				" file: \"" << it->m_filepath << "\"";
		}
	}
}

/** Use the ADD DATAFILE path to create a Datafile object and add it to the
front of m_files.
Parse the datafile path into a path and a filename with extension 'ibd'.
This datafile_path provided may or may not be an absolute path, but it
must end with the extension .ibd and have a basename of at least 1 byte.

Set tablespace m_path member and add a Datafile with the filename.
@param[in]	datafile_path	full path of the tablespace file. */
dberr_t Tablespace::add_datafile(const char *filepath)
{
	/* The path provided ends in ".ibd".  This was assured by
	validate_create_tablespace_info() */
	ut_d(const char* dot = strrchr(filepath, '.'));
	ut_ad(dot != NULL && 0 == strcmp(dot, DOT_IBD));

	/* If the path is an absolute path, separate it onto m_path and a
	basename. For relative paths, make the whole thing a basename so that
	it can be appended to the datadir. */
	bool	is_abs_path = is_absolute_path(filepath);
	size_t	dirlen = (is_abs_path ? dirname_length(filepath) : 0);
	const char* basename = filepath + dirlen;

	/* If the pathname contains a directory separator, fill the
	m_path member which is the default directory for files in this
	tablespace. Leave it null otherwise. */
	if (dirlen > 0) {
		set_path(filepath, dirlen);
	}

	/* Now add a new Datafile and set the filepath
	using the m_path created above. */
	m_files.push_back(Datafile(m_flags, FIL_IBD_FILE_INITIAL_SIZE, 0));
	m_files.back().make_filepath(m_path, {basename, strlen(basename) - 4},
				     IBD);

	return(DB_SUCCESS);
}
