/******************************************************
Copyright (c) 2012 Percona LLC and/or its affiliates.

Declarations of XtraBackup functions called by InnoDB code.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA

*******************************************************/

#ifndef xb0xb_h
#define xb0xb_h


extern void os_io_init_simple(void);
extern pfs_os_file_t	files[1000];
extern const char *innodb_checksum_algorithm_names[];
extern TYPELIB innodb_checksum_algorithm_typelib;
extern dberr_t open_or_create_data_files(
  bool*			create_new_db,
#ifdef UNIV_LOG_ARCHIVE
  lsn_t*		min_arch_log_no,
  lsn_t*		max_arch_log_no,
#endif
  lsn_t*		flushed_lsn,
  ulint*		sum_of_new_sizes)
  ;
int
fil_file_readdir_next_file(
/*=======================*/
dberr_t*	err,	/*!< out: this is set to DB_ERROR if an error
                was encountered, otherwise not changed */
                const char*	dirname,/*!< in: directory name or path */
                os_file_dir_t	dir,	/*!< in: directory stream */
                os_file_stat_t*	info)	/*!< in/out: buffer where the
                                      info is returned */;
fil_space_t*
fil_space_get_by_name(const char *);
ibool
recv_check_cp_is_consistent(const byte*	buf);
void
innodb_log_checksum_func_update(
/*============================*/
ulint	algorithm)	/*!< in: algorithm */;
dberr_t
srv_undo_tablespaces_init(
/*======================*/
ibool		create_new_db,
ibool   backup_mode,
const ulint	n_conf_tablespaces,
ulint*		n_opened);

#endif
