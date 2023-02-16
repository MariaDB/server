/*****************************************************************************

Copyright (c) 2012, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2019, MariaDB Corporation.

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
@file include/row0import.h
Header file for import tablespace functions.

Created 2012-02-08 by Sunny Bains
*******************************************************/

#ifndef row0import_h
#define row0import_h

#include "dict0types.h"
#include "ut0new.h"
#include "os0file.h"
#include "buf0buf.h"
#include "trx0trx.h"
#include "page0page.h"

// Forward declarations
struct trx_t;
struct dict_table_t;
struct row_prebuilt_t;
struct fil_iterator_t;
struct row_import;

/** The size of the buffer to use for IO.
@param n physical page size
@return number of pages */
#define IO_BUFFER_SIZE(n)	((1024 * 1024) / (n))

/** Functor that is called for each physical page that is read from the
tablespace file.  */
class AbstractCallback
{
public:
  /** Constructor
  @param trx covering transaction */
  AbstractCallback(trx_t* trx, uint32_t space_id)
    :
    m_zip_size(0),
    m_trx(trx),
    m_space(space_id),
    m_xdes(),
    m_xdes_page_no(UINT32_MAX),
    m_space_flags(UINT32_MAX) UNIV_NOTHROW { }

  /** Free any extent descriptor instance */
  virtual ~AbstractCallback()
  {
    UT_DELETE_ARRAY(m_xdes);
  }

  /** Determine the page size to use for traversing the tablespace
  @param file_size size of the tablespace file in bytes
  @param block contents of the first page in the tablespace file.
  @retval DB_SUCCESS or error code. */
  virtual dberr_t init(
    os_offset_t     file_size,
    const buf_block_t*  block) UNIV_NOTHROW;

  /** @return true if compressed table. */
  bool is_compressed_table() const UNIV_NOTHROW
  {
    return get_zip_size();
  }

  /** @return the tablespace flags */
  uint32_t get_space_flags() const { return m_space_flags; }

  /**
  Set the name of the physical file and the file handle that is used
  to open it for the file that is being iterated over.
  @param filename the physical name of the tablespace file
  @param file OS file handle */
  void set_file(const char* filename, pfs_os_file_t file) UNIV_NOTHROW
  {
    m_file = file;
    m_filepath = filename;
  }

  ulint get_zip_size() const { return m_zip_size; }
  ulint physical_size() const
  {
    return m_zip_size ? m_zip_size : srv_page_size;
  }

  const char* filename() const { return m_filepath; }

  /**
  Called for every page in the tablespace. If the page was not
  updated then its state must be set to BUF_PAGE_NOT_USED. For
  compressed tables the page descriptor memory will be at offset:
    block->page.frame + srv_page_size;
  @param block block read from file, note it is not from the buffer pool
  @retval DB_SUCCESS or error code. */
  virtual dberr_t operator()(buf_block_t* block) UNIV_NOTHROW = 0;

  /** @return the tablespace identifier */
  uint32_t get_space_id() const { return m_space; }

  bool is_interrupted() const { return trx_is_interrupted(m_trx); }

  /**
  Get the data page depending on the table type, compressed or not.
  @param block - block read from disk
  @retval the buffer frame */
  static byte* get_frame(const buf_block_t* block)
  {
    return block->page.zip.data
      ? block->page.zip.data : block->page.frame;
  }

  /** Invoke the functionality for the callback */
  virtual dberr_t run(const fil_iterator_t& iter,
          buf_block_t* block) UNIV_NOTHROW = 0;

protected:
  /** Get the physical offset of the extent descriptor within the page.
  @param page_no page number of the extent descriptor
  @param page contents of the page containing the extent descriptor.
  @return the start of the xdes array in a page */
  const xdes_t* xdes(
    ulint     page_no,
    const page_t*   page) const UNIV_NOTHROW
  {
    ulint   offset;

    offset = xdes_calc_descriptor_index(get_zip_size(), page_no);

    return(page + XDES_ARR_OFFSET + XDES_SIZE * offset);
  }

  /** Set the current page directory (xdes). If the extent descriptor is
  marked as free then free the current extent descriptor and set it to
  0. This implies that all pages that are covered by this extent
  descriptor are also freed.

  @param page_no offset of page within the file
  @param page page contents
  @return DB_SUCCESS or error code. */
  dberr_t   set_current_xdes(
    uint32_t  page_no,
    const page_t*   page) UNIV_NOTHROW
  {
    m_xdes_page_no = page_no;

    UT_DELETE_ARRAY(m_xdes);
    m_xdes = NULL;

    if (mach_read_from_4(XDES_ARR_OFFSET + XDES_STATE + page)
        != XDES_FREE) {
      const ulint physical_size = m_zip_size
        ? m_zip_size : srv_page_size;

      m_xdes = UT_NEW_ARRAY_NOKEY(xdes_t, physical_size);

      /* Trigger OOM */
      DBUG_EXECUTE_IF(
        "ib_import_OOM_13",
        UT_DELETE_ARRAY(m_xdes);
        m_xdes = NULL;
      );

      if (m_xdes == NULL) {
        return(DB_OUT_OF_MEMORY);
      }

      memcpy(m_xdes, page, physical_size);
    }

    return(DB_SUCCESS);
  }

  /** Check if the page is marked as free in the extent descriptor.
  @param page_no page number to check in the extent descriptor.
  @return true if the page is marked as free */
  bool is_free(uint32_t page_no) const UNIV_NOTHROW
  {
    ut_a(xdes_calc_descriptor_page(get_zip_size(), page_no)
         == m_xdes_page_no);

    if (m_xdes != 0) {
      const xdes_t*   xdesc = xdes(page_no, m_xdes);
      ulint     pos = page_no % FSP_EXTENT_SIZE;

      return xdes_is_free(xdesc, pos);
    }

    /* If the current xdes was free, the page must be free. */
    return(true);
  }

protected:
  /** The ROW_FORMAT=COMPRESSED page size, or 0. */
  ulint       m_zip_size;

  /** File handle to the tablespace */
  pfs_os_file_t     m_file;

  /** Physical file path. */
  const char*     m_filepath;

  /** Covering transaction. */
  trx_t*      m_trx;

  /** Space id of the file being iterated over. */
  uint32_t    m_space;

  /** Current extent descriptor page */
  xdes_t*       m_xdes;

  /** Physical page offset in the file of the extent descriptor */
  uint32_t    m_xdes_page_no;

  /** Flags value read from the header page */
  uint32_t    m_space_flags;
};

/**
Try and determine the index root pages by checking if the next/prev
pointers are both FIL_NULL. We need to ensure that skip deleted pages. */
struct FetchIndexRootPages : public AbstractCallback {

  /** Index information gathered from the .ibd file. */
  struct Index {

    Index(index_id_t id, uint32_t page_no)
      :
      m_id(id),
      m_page_no(page_no) { }

    index_id_t  m_id;     /*!< Index id */
    uint32_t  m_page_no;  /*!< Root page number */
  };

  /** Constructor
  @param trx covering (user) transaction
  @param table table definition in server .*/
  FetchIndexRootPages(const dict_table_t* table, trx_t* trx)
    :
    AbstractCallback(trx, UINT32_MAX),
    m_table(table), m_index(0, 0) UNIV_NOTHROW { }

  FetchIndexRootPages()
    :
    AbstractCallback(nullptr, UINT32_MAX),
    m_table(nullptr), m_index(0, 0) UNIV_NOTHROW { }

  /** Destructor */
  ~FetchIndexRootPages() UNIV_NOTHROW override = default;

  /** Fetch the clustered index root page in the tablespace
  @param iter   Tablespace iterator
  @param block  Block to use for IO
  @retval DB_SUCCESS or error code */
  dberr_t run(const fil_iterator_t& iter,
        buf_block_t* block) UNIV_NOTHROW override;

  /** Check that fsp flags and row formats match.
  @param block block to convert, it is not from the buffer pool.
  @retval DB_SUCCESS or error code. */
  dberr_t operator()(buf_block_t* block) UNIV_NOTHROW override;

  /** Get row format from the header and the root index page. */
  enum row_type get_row_format(buf_block_t *block)
  {
    if (!page_is_comp(block->page.frame))
      return ROW_TYPE_REDUNDANT;
    /* With full_crc32 we cannot tell between dynamic or compact, and
    return not_used. We cannot simply return dynamic or compact, as
    the client of this function will not be able to tell whether it is
    dynamic because of this or the other branch below. Returning
    default would also work if it is immediately handled, but is still
    more ambiguous than not_used, which is not a row_format at all. */
    if (fil_space_t::full_crc32(m_space_flags))
      return ROW_TYPE_NOT_USED;
    if (m_space_flags & FSP_FLAGS_MASK_ATOMIC_BLOBS)
    {
      if (FSP_FLAGS_GET_ZIP_SSIZE(m_space_flags))
        return ROW_TYPE_COMPRESSED;
      else
        return ROW_TYPE_DYNAMIC;
    }
    return ROW_TYPE_COMPACT;
  }

  /** Update the import configuration that will be used to import
  the tablespace. */
  dberr_t build_row_import(row_import* cfg) const UNIV_NOTHROW;

  /** Table definition in server. When the table is being created,
  there's no table yet so m_table is nullptr */
  const dict_table_t*   m_table;

  /** Table row format. Only used when a (stub) table is being created
  in which case m_table is null, for obtaining row format from the
  .ibd for the stub table. */
  enum row_type m_row_format;

  /** Index information */
  Index       m_index;
};

/*****************************************************************//**
Imports a tablespace. The space id in the .ibd file must match the space id
of the table in the data dictionary.
@return error code or DB_SUCCESS */
dberr_t
row_import_for_mysql(
/*=================*/
	dict_table_t*	table,		/*!< in/out: table */
	row_prebuilt_t*	prebuilt)	/*!< in: prebuilt struct
						in MySQL */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/** Update the DICT_TF2_DISCARDED flag in SYS_TABLES.MIX_LEN.
@param[in,out]	trx		dictionary transaction
@param[in]	table_id	table identifier
@param[in]	discarded	whether to set or clear the flag
@return DB_SUCCESS or error code */
dberr_t row_import_update_discarded_flag(trx_t* trx, table_id_t table_id,
					 bool discarded)
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/** Update the root page numbers and tablespace ID of a table.
@param[in,out]	trx	dictionary transaction
@param[in,out]	table	persistent table
@param[in]	reset	whether to reset the fields to FIL_NULL
@return DB_SUCCESS or error code */
dberr_t
row_import_update_index_root(trx_t* trx, dict_table_t* table, bool reset)
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/********************************************************************//**
Iterate over all or some pages in the tablespace.
@param dir_path - the path to data dir storing the tablespace
@param name - the table name
@param n_io_buffers - number of blocks to read and write together
@param callback - functor that will do the page queries or updates
@return   DB_SUCCESS or error code */
dberr_t
fil_tablespace_iterate(
/*===================*/
  const char* dir_path,
  const char*   name,
  ulint       n_io_buffers,
  AbstractCallback&   callback);

/**
Read the row type from a .cfg file.
@param[in]  dir_path  Path to the data directory containing the .cfg file
@param[in]  name      Name of the table
@param[in]  thd       Session
@param[out] result    The row format read from the .cfg file
@return DB_SUCCESS or error code. */
dberr_t get_row_type_from_cfg(const char* dir_path, const char* name, THD* thd,
                              rec_format_enum& result);

#endif /* row0import_h */
