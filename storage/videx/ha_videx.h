/* Copyright (c) 2025 Bytedance Ltd. and/or its affiliates

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is also distributed with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have included with MySQL.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/** @file ha_videx.h

    @brief
  The ha_videx engine is a stubbed storage engine for example purposes only;
  it does nothing at this point. Its purpose is to provide a source
  code illustration of how to begin writing new storage engines; see also
  /storage/videx/ha_videx.cc.

    @note
  Please read ha_videx.cc before reading this file.
  Reminder: The videx storage engine implements all methods that are
  *required* to be implemented. For a full list of all methods that you can
  implement, see handler.h.

   @see
  /sql/handler.h and /storage/videx/ha_videx.cc
*/

#include "my_global.h"                   /* ulonglong */
#include "thr_lock.h"                    /* THR_LOCK, THR_LOCK_DATA */
#include "handler.h"                     /* handler */
#include "my_base.h"                     /* ha_rows */

/** @brief
  videx_share is a class that will be shared among all open handlers.
  This videx implements the minimum of what you will probably need.
*/
class videx_share : public Handler_share {
public:
  mysql_mutex_t mutex; // ??? new in mariadb
  THR_LOCK lock;
  videx_share();
  ~videx_share
  {
    thr_lock_delete(&lock);
    mysql_mutex_destroy(&mutex);
  }
};

// namespace dd

/** @brief
  Class definition for the storage engine
*/
class ha_videx: public handler
{
  THR_LOCK_DATA lock;      ///< MariaDB lock
  videx_share *share;    ///< Shared lock info
  videx_share *get_share(); ///< Get the share
  // ha_rows index_next_cnt;

public:
  ha_videx(handlerton* hton, TABLE_SHARE* table_arg);
  ~ha_videx() override;

  /** @return the transaction that last modified the table definition
	@see dict_table_t::def_trx_id */
	ulonglong table_version() const override;

	/** Get the row type from the storage engine.  If this method returns
	ROW_TYPE_NOT_USED, the information in HA_CREATE_INFO should be used. */
  enum row_type get_row_type() const override;

  const char* table_type() const override;

	Table_flags table_flags() const override;

	ulong index_flags(uint idx, uint part, bool all_parts) const override;

	uint max_supported_keys() const override;

	uint max_supported_key_length() const override;

	uint max_supported_key_part_length() const override;

	const key_map* keys_to_use_for_scanning() override;

	void column_bitmaps_signal() override;

  /** Opens dictionary table object using table name. For partition, we need to
	try alternative lower/upper case names to support moving data files across
	platforms.
	@param[in]	table_name	name of the table/partition
	@param[in]	norm_name	normalized name of the table/partition
	@param[in]	is_partition	if this is a partition of a table
	@param[in]	ignore_err	error to ignore for loading dictionary object
	@return dictionary table object or NULL if not found */
  static dict_table_t* open_dict_table(
		const char*		table_name,
		const char*		norm_name,
		bool			is_partition,
		dict_err_ignore_t	ignore_err);

    int open(const char *name, int mode, uint test_if_locked) override;

    /** Fetch or recalculate InnoDB table statistics */
    dberr_t statistics_init(dict_table_t *table, bool recalc);
  
    handler* clone(const char *name, MEM_ROOT *mem_root) override;
  
    int close(void) override;
  
  // !!! There is a NOT_USED signal in ha_innodb.h
  IO_AND_CPU_COST scan_time() override;
  double rnd_pos_time(ha_rows rows) override;

  double read_time(uint index, uint ranges, ha_rows rows) override;
	
  int write_row(const uchar * buf) override;

	int update_row(const uchar * old_data, const uchar * new_data) override;

	int delete_row(const uchar * buf) override;

	bool was_semi_consistent_read() override; // ???

	void try_semi_consistent_read(bool yes) override; // ???

	void unlock_row() override;

  int index_next(uchar * buf) override;

  // int index_next_same(uchar * buf, const uchar * key, uint keylen) override;

  int index_prev(uchar * buf) override;

  int index_first(uchar * buf) override;

  int index_last(uchar * buf) override;

  int rnd_init(bool scan) override;

  int rnd_end() override;

  int rnd_next(uchar *buf) override;

  int rnd_pos(uchar * buf, uchar *pos) override;

  void position(const uchar *record) override;

  int info(uint) override;

  int extra(ha_extra_function operation) override;

  int reset() override;
  
  int external_lock(THD *thd, int lock_type) override;

	THR_LOCK_DATA** store_lock(
		THD*			thd,
		THR_LOCK_DATA**		to,
		thr_lock_type		lock_type) override;
 
  ha_rows records_in_range(
    uint                    inx,
    const key_range*        min_key,
    const key_range*        max_key,
    page_range*             pages) override;

  ha_rows estimate_rows_upper_bound() override;
	int create(
		const char*		name,
		TABLE*			form,
		HA_CREATE_INFO*		create_info) override;

  // int truncate() override;

  int delete_table(const char *name) override;

  int rename_table(const char* from, const char* to) override;

	/** Initialize multi range read @see DsMrr_impl::dsmrr_init
	@param seq
	@param seq_init_param
	@param n_ranges
	@param mode
	@param buf */
	int multi_range_read_init(
		RANGE_SEQ_IF*		seq,
		void*			seq_init_param,
		uint			n_ranges,
		uint			mode,
		HANDLER_BUFFER*		buf) override;

	/** Process next multi range read @see DsMrr_impl::dsmrr_next
	@param range_info */
	int multi_range_read_next(range_id_t *range_info) override;

	/** Initialize multi range read and get information.
	@see ha_myisam::multi_range_read_info_const
	@see DsMrr_impl::dsmrr_info_const
	@param keyno
	@param seq
	@param seq_init_param
	@param n_ranges
	@param bufsz
	@param flags
	@param cost */
	ha_rows multi_range_read_info_const(
		uint			keyno,
		RANGE_SEQ_IF*		seq,
		void*			seq_init_param,
		uint			n_ranges,
		uint*			bufsz,
		uint*			flags,
                ha_rows                 limit,
		Cost_estimate*		cost) override;

    /** Initialize multi range read and get information.
	@see DsMrr_impl::dsmrr_info
	@param keyno
	@param seq
	@param seq_init_param
	@param n_ranges
	@param bufsz
	@param flags
	@param cost */
	ha_rows multi_range_read_info(uint keyno, uint n_ranges, uint keys,
    uint key_parts, uint* bufsz, uint* flags,
    Cost_estimate* cost) override;

	int multi_range_read_explain_info(uint mrr_mode, char *str, size_t size) override;
	
	/** Attempt to push down an index condition.
	@param[in] keyno MySQL key number
	@param[in] idx_cond Index condition to be checked
	@return idx_cond if pushed; NULL if not pushed */
	Item* idx_cond_push(uint keyno, Item* idx_cond) override;

protected:
  int info_low(uint flag, bool is_analyze); // virtual in mysql version, but not in mariadb
};
