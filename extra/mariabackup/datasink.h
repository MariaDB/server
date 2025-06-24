/******************************************************
Copyright (c) 2011-2013 Percona LLC and/or its affiliates.

Data sink interface.

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

#ifndef XB_DATASINK_H
#define XB_DATASINK_H

#include <my_global.h>
#include <my_dir.h>

#ifdef __cplusplus
extern "C" {
#endif

extern char *xtrabackup_tmpdir;
struct datasink_struct;
typedef struct datasink_struct datasink_t;

typedef struct ds_ctxt {
	datasink_t	*datasink;
	char 		*root;
	void		*ptr;
	struct ds_ctxt	*pipe_ctxt;
	/*
          Copy file for backup/restore.
          @return true in case of success.
        */
        bool copy_file(const char *src_file_path,
                       const char *dst_file_path,
                       uint thread_n,
                       bool rewrite = false);

        bool move_file(const char *src_file_path,
                       const char *dst_file_path,
                       const char *dst_dir,
                       uint thread_n);

        bool make_hardlink(const char *from_path, const char *to_path);

        void copy_or_move_dir(const char *from, const char *to,
                              bool do_copy, bool allow_hardlinks);

        bool backup_file_vprintf(const char *filename,
                                 const char *fmt, va_list ap);

        bool backup_file_print_buf(const char *filename,
                                   const char *buf,
                                   int buf_len);

        bool backup_file_printf(const char *filename,
                                const char *fmt, ...)
                                ATTRIBUTE_FORMAT(printf, 2, 0);

} ds_ctxt_t;

typedef struct {
	void		*ptr;
	char		*path;
	datasink_t	*datasink;
} ds_file_t;

struct datasink_struct {
	ds_ctxt_t *(*init)(const char *root);
	ds_file_t *(*open)(ds_ctxt_t *ctxt, const char *path,
		const MY_STAT *stat, bool rewrite);
	int (*write)(ds_file_t *file, const unsigned char *buf, size_t len);
	int (*seek_set)(ds_file_t *file, my_off_t offset);
	int (*close)(ds_file_t *file);
	int (*remove)(const char *path);
	// TODO: consider to return bool from "rename" and "remove"
	int (*rename)(ds_ctxt_t *ctxt, const char *old_path, const char *new_path);
	int (*mremove)(ds_ctxt_t *ctxt, const char *path);
	void (*deinit)(ds_ctxt_t *ctxt);
};


static inline int dummy_remove(const char *) {
	return 0;
}

/* Supported datasink types */
typedef enum {
	DS_TYPE_STDOUT,
	DS_TYPE_LOCAL,
	DS_TYPE_XBSTREAM,
	DS_TYPE_COMPRESS,
	DS_TYPE_ENCRYPT,
	DS_TYPE_DECRYPT,
	DS_TYPE_TMPFILE,
	DS_TYPE_BUFFER
} ds_type_t;

/************************************************************************
Create a datasink of the specified type */
ds_ctxt_t *ds_create(const char *root, ds_type_t type);

/************************************************************************
Open a datasink file */
ds_file_t *ds_open(
	ds_ctxt_t *ctxt, const char *path, const MY_STAT *stat, bool rewrite = false);

/************************************************************************
Write to a datasink file.
@return 0 on success, 1 on error. */
int ds_write(ds_file_t *file, const void *buf, size_t len);
int ds_seek_set(ds_file_t *file, my_off_t offset);

int ds_rename(ds_ctxt_t *ctxt, const char *old_path, const char *new_path);
int ds_remove(ds_ctxt_t *ctxt, const char *path);

/************************************************************************
Close a datasink file.
@return 0 on success, 1, on error. */
int ds_close(ds_file_t *file);

/************************************************************************
Destroy a datasink handle */
void ds_destroy(ds_ctxt_t *ctxt);

/************************************************************************
Set the destination pipe for a datasink (only makes sense for compress and
tmpfile). */
void ds_set_pipe(ds_ctxt_t *ctxt, ds_ctxt_t *pipe_ctxt);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* XB_DATASINK_H */
