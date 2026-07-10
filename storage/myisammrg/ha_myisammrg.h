/*
   Copyright (c) 2000, 2011, Oracle and/or its affiliates

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

/* class for the the myisam merge handler */

#include <myisammrg.h>

/** 
  Represents one name of a MERGE child.

  @todo: Add MYRG_SHARE and store chlidren names in the
  share.
*/

class Mrg_child_def: public Sql_alloc
{
  /* Remembered MERGE child def version.  See top comment in ha_myisammrg.cc */
  enum_table_ref_type m_child_table_ref_type;
  ulonglong m_child_def_version;
public:
  LEX_STRING db;
  LEX_STRING name;

  /* Access MERGE child def version.  See top comment in ha_myisammrg.cc */
  inline enum_table_ref_type get_child_table_ref_type()
  {
    return m_child_table_ref_type;
  }
  inline ulonglong get_child_def_version()
  {
    return m_child_def_version;
  }
  inline void set_child_def_version(enum_table_ref_type child_table_ref_type,
                                    ulonglong version)
  {
    m_child_table_ref_type= child_table_ref_type;
    m_child_def_version= version;
  }

  Mrg_child_def(char *db_arg, size_t db_len_arg,
                char *table_name_arg, size_t table_name_len_arg)
  {
    db.str= db_arg;
    db.length= db_len_arg;
    name.str= table_name_arg;
    name.length= table_name_len_arg;
    m_child_def_version= ~0UL;
    m_child_table_ref_type= TABLE_REF_NULL;
  }
};


class ha_myisammrg final : public handler
{
  MYRG_INFO *file;
  my_bool is_cloned;                    /* This instance has been cloned */

public:
  MEM_ROOT      children_mem_root;      /* mem root for children list */
  List<Mrg_child_def> child_def_list;
  TABLE_LIST    *children_l;            /* children list */
  TABLE_LIST    **children_last_l;      /* children list end */
  uint          test_if_locked;         /* flags from ::open() */

  ha_myisammrg(handlerton *hton, TABLE_SHARE *table_arg);
  ~ha_myisammrg();
  ulonglong table_flags() const override
  {
    return (HA_REC_NOT_IN_SEQ | HA_AUTO_PART_KEY | HA_NO_TRANSACTIONS |
            HA_BINLOG_ROW_CAPABLE | HA_BINLOG_STMT_CAPABLE |
	    HA_NULL_IN_KEY | HA_CAN_INDEX_BLOBS | HA_FILE_BASED |
            HA_ANY_INDEX_MAY_BE_UNIQUE | HA_CAN_BIT_FIELD |
            HA_HAS_RECORDS | HA_CAN_EXPORT |
            HA_NO_COPY_ON_ALTER |
            HA_DUPLICATE_POS | HA_CAN_MULTISTEP_MERGE);
  }
  ulong index_flags(uint inx, uint part, bool all_parts) const override
  {
    return ((table_share->key_info[inx].algorithm == HA_KEY_ALG_FULLTEXT) ?
            0 : HA_READ_NEXT | HA_READ_PREV | HA_READ_RANGE |
            HA_READ_ORDER | HA_KEYREAD_ONLY);
  }
  uint max_supported_keys()          const override { return MI_MAX_KEY; }
  uint max_supported_key_length()    const override { return HA_MAX_KEY_LENGTH; }
  uint max_supported_key_part_length() const override
  { return HA_MAX_KEY_LENGTH; }
  IO_AND_CPU_COST scan_time() override
  {
    IO_AND_CPU_COST cost;
    cost.io= (ulonglong2double(stats.data_file_length) / IO_SIZE +
              file->tables),
    cost.cpu= records() * ROW_NEXT_FIND_COST;
    return cost;
  }
  IO_AND_CPU_COST rnd_pos_time(ha_rows rows) override;
  IO_AND_CPU_COST keyread_time(uint index, ulong ranges, ha_rows rows,
                                ulonglong blocks) override;
  int open(const char *name, int mode, uint test_if_locked) override;
  handler *clone(const char *name, MEM_ROOT *mem_root) override;
  int close(void) override;
  int write_row(const uchar * buf) override;
  int update_row(const uchar * old_data, const uchar * new_data) override;
  int delete_row(const uchar * buf) override;
  int index_read_map(uchar *buf, const uchar *key, key_part_map keypart_map,
                     enum ha_rkey_function find_flag) override;
  int index_read_idx_map(uchar *buf, uint index, const uchar *key,
                         key_part_map keypart_map,
                         enum ha_rkey_function find_flag) override;
  int index_read_last_map(uchar *buf, const uchar *key, key_part_map keypart_map) override;
  int index_next(uchar * buf) override;
  int index_prev(uchar * buf) override;
  int index_first(uchar * buf) override;
  int index_last(uchar * buf) override;
  int index_next_same(uchar *buf, const uchar *key, uint keylen) override;
  int rnd_init(bool scan) override;
  int rnd_next(uchar *buf) override;
  int rnd_pos(uchar * buf, uchar *pos) override;
  void position(const uchar *record) override;
  ha_rows records_in_range(uint inx, const key_range *start_key,
                           const key_range *end_key, page_range *pages) override;
  int delete_all_rows() override;
  int info(uint) override;
  int reset(void) override;
  int extra(enum ha_extra_function operation) override;
  int extra_opt(enum ha_extra_function operation, ulong cache_size) override;
  int external_lock(THD *thd, int lock_type) override;
  uint lock_count(void) const override;
  int create(const char *name, TABLE *form, HA_CREATE_INFO *create_info) override;
  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
			     enum thr_lock_type lock_type) override;
  void update_create_info(HA_CREATE_INFO *create_info) override;
  void append_create_info(String *packet) override;
  enum_alter_inplace_result check_if_supported_inplace_alter(TABLE *,
                                                Alter_inplace_info *) override;
  bool inplace_alter_table(TABLE *altered_table,
                           Alter_inplace_info *ha_alter_info) override;
  int check(THD* thd, HA_CHECK_OPT* check_opt) override;
  ha_rows records() override;
  virtual uint count_query_cache_dependant_tables(uint8 *tables_type) override;
  virtual my_bool
    register_query_cache_dependant_tables(THD *thd,
                                          Query_cache *cache,
                                          Query_cache_block_table **block,
                                          uint *n) override;
  virtual void set_lock_type(enum thr_lock_type lock) override;

  /* Internal interface functions, not part of the normal handler interface */
  int add_children_list(void);
  int attach_children(void);
  int detach_children(void);
  int create_mrg(const char *name, HA_CREATE_INFO *create_info);
  MYRG_INFO *myrg_info() { return file; }
  TABLE *table_ptr()  { return table; }

  /*
    Make an exact copy an identifier on children_mem_root.

    @param src    - The original identifier
    @return       - {NULL,0} in case of EOM,
                    or a non-NULL LEX_STRING with the identifier copy.
  */
  LEX_STRING make_child_ident(const LEX_CSTRING &src)
  {
    return lex_string_strmake_root(&children_mem_root, src.str, src.length);
  }

  /*
    Make an exact copy or a lower-cased copy of an identifier
    on children mem_root.

    @param src    - The original identifier
    @param casedn - If the name should be converted to lower case
    @return       - {NULL,0} in case of EOM,
                    or a non-NULL LEX_STRING with the identifier copy.
  */
  LEX_STRING make_child_ident_opt_casedn(const LEX_CSTRING &src, bool casedn)
  {
    return casedn ? lex_string_casedn_root(&children_mem_root,
                                           &my_charset_utf8mb3_general_ci,
                                           src.str, src.length) :
                    make_child_ident(src);
  }

  /*
    Make an optionally lower-cases filename_to_tablename-decoded identifier
    in children mem_root.
  */
  LEX_STRING make_child_ident_filename_to_tablename(const char *src,
                                                    bool casedn)
  {
    char buf[NAME_LEN];
    size_t len= filename_to_tablename(src, buf, sizeof(buf));
    return make_child_ident_opt_casedn({buf, len}, casedn);
  }
};
