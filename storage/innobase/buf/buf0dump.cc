/*****************************************************************************

Copyright (c) 2011, 2015, Oracle and/or its affiliates. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file buf/buf0dump.cc
Implements a buffer pool dump/load.

Created April 08, 2011 Vasil Dimov
*******************************************************/

#include "univ.i"

#include <stdarg.h> /* va_* */
#include <string.h> /* strerror() */

#include "buf0buf.h" /* buf_pool_mutex_enter(), srv_buf_pool_instances */
#include "buf0dump.h"
#include "db0err.h"
#include "dict0dict.h" /* dict_operation_lock */
#include "os0file.h" /* OS_FILE_MAX_PATH */
#include "os0sync.h" /* os_event* */
#include "os0thread.h" /* os_thread_* */
#include "srv0srv.h" /* srv_fast_shutdown, srv_buf_dump* */
#include "srv0start.h" /* srv_shutdown_state */
#include "sync0rw.h" /* rw_lock_s_lock() */
#include "ut0byte.h" /* ut_ull_create() */
#include "ut0sort.h" /* UT_SORT_FUNCTION_BODY */

enum status_severity {
	STATUS_INFO,
	STATUS_NOTICE,
	STATUS_ERR
};

#define SHUTTING_DOWN()	(UNIV_UNLIKELY(srv_shutdown_state \
				       != SRV_SHUTDOWN_NONE))

/* Flags that tell the buffer pool dump/load thread which action should it
take after being waked up. */
static ibool	buf_dump_should_start = FALSE;
static ibool	buf_load_should_start = FALSE;

static ibool	buf_load_abort_flag = FALSE;

/* Used to temporary store dump info in order to avoid IO while holding
buffer pool mutex during dump and also to sort the contents of the dump
before reading the pages from disk during load.
We store the space id in the high 32 bits and page no in low 32 bits. */
typedef ib_uint64_t	buf_dump_t;

/* Aux macros to create buf_dump_t and to extract space and page from it */
#define BUF_DUMP_CREATE(space, page)	ut_ull_create(space, page)
#define BUF_DUMP_SPACE(a)		((ulint) ((a) >> 32))
#define BUF_DUMP_PAGE(a)		((ulint) ((a) & 0xFFFFFFFFUL))

/*****************************************************************//**
Wakes up the buffer pool dump/load thread and instructs it to start
a dump. This function is called by MySQL code via buffer_pool_dump_now()
and it should return immediately because the whole MySQL is frozen during
its execution. */
UNIV_INTERN
void
buf_dump_start()
/*============*/
{
	buf_dump_should_start = TRUE;
	os_event_set(srv_buf_dump_event);
}

/*****************************************************************//**
Wakes up the buffer pool dump/load thread and instructs it to start
a load. This function is called by MySQL code via buffer_pool_load_now()
and it should return immediately because the whole MySQL is frozen during
its execution. */
UNIV_INTERN
void
buf_load_start()
/*============*/
{
	buf_load_should_start = TRUE;
	os_event_set(srv_buf_dump_event);
}

/*****************************************************************//**
Sets the global variable that feeds MySQL's innodb_buffer_pool_dump_status
to the specified string. The format and the following parameters are the
same as the ones used for printf(3). The value of this variable can be
retrieved by:
SELECT variable_value FROM information_schema.global_status WHERE
variable_name = 'INNODB_BUFFER_POOL_DUMP_STATUS';
or by:
SHOW STATUS LIKE 'innodb_buffer_pool_dump_status'; */
static __attribute__((nonnull, format(printf, 2, 3)))
void
buf_dump_status(
/*============*/
	enum status_severity	severity,/*!< in: status severity */
	const char*		fmt,	/*!< in: format */
	...)				/*!< in: extra parameters according
					to fmt */
{
	va_list	ap;

	va_start(ap, fmt);

	ut_vsnprintf(
		export_vars.innodb_buffer_pool_dump_status,
		sizeof(export_vars.innodb_buffer_pool_dump_status),
		fmt, ap);

	ib_logf((ib_log_level_t) severity, "%s", export_vars.innodb_buffer_pool_dump_status);

	va_end(ap);
}

/*****************************************************************//**
Sets the global variable that feeds MySQL's innodb_buffer_pool_load_status
to the specified string. The format and the following parameters are the
same as the ones used for printf(3). The value of this variable can be
retrieved by:
SELECT variable_value FROM information_schema.global_status WHERE
variable_name = 'INNODB_BUFFER_POOL_LOAD_STATUS';
or by:
SHOW STATUS LIKE 'innodb_buffer_pool_load_status'; */
static __attribute__((nonnull, format(printf, 2, 3)))
void
buf_load_status(
/*============*/
	enum status_severity	severity,/*!< in: status severity */
	const char*	fmt,	/*!< in: format */
	...)			/*!< in: extra parameters according to fmt */
{
	va_list	ap;

	va_start(ap, fmt);

	ut_vsnprintf(
		export_vars.innodb_buffer_pool_load_status,
		sizeof(export_vars.innodb_buffer_pool_load_status),
		fmt, ap);

	if (severity == STATUS_NOTICE || severity == STATUS_ERR) {
		ut_print_timestamp(stderr);
		fprintf(stderr, " InnoDB: %s\n",
			export_vars.innodb_buffer_pool_load_status);
	}

	va_end(ap);
}

/** Returns the directory path where the buffer pool dump file will be created.
@return directory path */
static
const char*
get_buf_dump_dir()
{
	const char*	dump_dir;

	/* The dump file should be created in the default data directory if
	innodb_data_home_dir is set as an empty string. */
	if (strcmp(srv_data_home, "") == 0) {
		dump_dir = fil_path_to_mysql_datadir;
	} else {
		dump_dir = srv_data_home;
	}

	return(dump_dir);
}

/*****************************************************************//**
Perform a buffer pool dump into the file specified by
innodb_buffer_pool_filename. If any errors occur then the value of
innodb_buffer_pool_dump_status will be set accordingly, see buf_dump_status().
The dump filename can be specified by (relative to srv_data_home):
SET GLOBAL innodb_buffer_pool_filename='filename'; */
static
void
buf_dump(
/*=====*/
	ibool	obey_shutdown)	/*!< in: quit if we are in a shutting down
				state */
{
#define SHOULD_QUIT()	(SHUTTING_DOWN() && obey_shutdown)

	char	full_filename[OS_FILE_MAX_PATH];
	char	tmp_filename[OS_FILE_MAX_PATH];
	char	now[32];
	FILE*	f;
	ulint	i;
	int	ret;

	ut_snprintf(full_filename, sizeof(full_filename),
		    "%s%c%s", get_buf_dump_dir(), SRV_PATH_SEPARATOR,
		    srv_buf_dump_filename);

	ut_snprintf(tmp_filename, sizeof(tmp_filename),
		    "%s.incomplete", full_filename);

	buf_dump_status(STATUS_NOTICE, "Dumping buffer pool(s) to %s",
			full_filename);

	f = fopen(tmp_filename, "w");
	if (f == NULL) {
		buf_dump_status(STATUS_ERR,
				"Cannot open '%s' for writing: %s",
				tmp_filename, strerror(errno));
		return;
	}
	/* else */

	/* walk through each buffer pool */
	for (i = 0; i < srv_buf_pool_instances && !SHOULD_QUIT(); i++) {
		buf_pool_t*		buf_pool;
		const buf_page_t*	bpage;
		buf_dump_t*		dump;
		ulint			n_pages;
		ulint			j;
		ulint			limit;
		ulint			counter;

		buf_pool = buf_pool_from_array(i);

		/* obtain buf_pool mutex before allocate, since
		UT_LIST_GET_LEN(buf_pool->LRU) could change */
		buf_pool_mutex_enter(buf_pool);

		n_pages = UT_LIST_GET_LEN(buf_pool->LRU);

		/* skip empty buffer pools */
		if (n_pages == 0) {
			buf_pool_mutex_exit(buf_pool);
			continue;
		}

		if (srv_buf_pool_dump_pct != 100) {
			ut_ad(srv_buf_pool_dump_pct < 100);

			n_pages = n_pages * srv_buf_pool_dump_pct / 100;

			if (n_pages == 0) {
				n_pages = 1;
			}
		}

		dump = static_cast<buf_dump_t*>(
			ut_malloc(n_pages * sizeof(*dump))) ;

		if (dump == NULL) {
			buf_pool_mutex_exit(buf_pool);
			fclose(f);
			buf_dump_status(STATUS_ERR,
					"Cannot allocate " ULINTPF " bytes: %s",
					(ulint) (n_pages * sizeof(*dump)),
					strerror(errno));
			/* leave tmp_filename to exist */
			return;
		}

		for (bpage = UT_LIST_GET_FIRST(buf_pool->LRU), j = 0;
		     bpage != NULL && j < n_pages;
		     bpage = UT_LIST_GET_NEXT(LRU, bpage), j++) {

			ut_a(buf_page_in_file(bpage));

			dump[j] = BUF_DUMP_CREATE(buf_page_get_space(bpage),
						  buf_page_get_page_no(bpage));
		}

		ut_a(j == n_pages);

		buf_pool_mutex_exit(buf_pool);

		limit = (ulint)((double)n_pages * ((double)srv_buf_dump_status_frequency / (double)100));
		counter = 0;

		for (j = 0; j < n_pages && !SHOULD_QUIT(); j++) {
			ret = fprintf(f, ULINTPF "," ULINTPF "\n",
				      BUF_DUMP_SPACE(dump[j]),
				      BUF_DUMP_PAGE(dump[j]));
			if (ret < 0) {
				ut_free(dump);
				fclose(f);
				buf_dump_status(STATUS_ERR,
						"Cannot write to '%s': %s",
						tmp_filename, strerror(errno));
				/* leave tmp_filename to exist */
				return;
			}

			counter++;

			/* Print buffer pool dump status only if
			srv_buf_dump_status_frequency is > 0 and
			we have processed that amount of pages. */
			if (srv_buf_dump_status_frequency &&
			    counter == limit) {
				counter = 0;
				buf_dump_status(
					STATUS_INFO,
					"Dumping buffer pool "
					ULINTPF "/" ULINTPF ", "
					"page " ULINTPF "/" ULINTPF,
					i + 1, srv_buf_pool_instances,
					j + 1, n_pages);
			}
		}

		ut_free(dump);
	}

	ret = fclose(f);
	if (ret != 0) {
		buf_dump_status(STATUS_ERR,
				"Cannot close '%s': %s",
				tmp_filename, strerror(errno));
		return;
	}
	/* else */

	ret = unlink(full_filename);
	if (ret != 0 && errno != ENOENT) {
		buf_dump_status(STATUS_ERR,
				"Cannot delete '%s': %s",
				full_filename, strerror(errno));
		/* leave tmp_filename to exist */
		return;
	}
	/* else */

	ret = rename(tmp_filename, full_filename);
	if (ret != 0) {
		buf_dump_status(STATUS_ERR,
				"Cannot rename '%s' to '%s': %s",
				tmp_filename, full_filename,
				strerror(errno));
		/* leave tmp_filename to exist */
		return;
	}
	/* else */

	/* success */

	ut_sprintf_timestamp(now);

	buf_dump_status(STATUS_NOTICE,
			"Buffer pool(s) dump completed at %s", now);
}

/*****************************************************************//**
Compare two buffer pool dump entries, used to sort the dump on
space_no,page_no before loading in order to increase the chance for
sequential IO.
@return -1/0/1 if entry 1 is smaller/equal/bigger than entry 2 */
static
lint
buf_dump_cmp(
/*=========*/
	const buf_dump_t	d1,	/*!< in: buffer pool dump entry 1 */
	const buf_dump_t	d2)	/*!< in: buffer pool dump entry 2 */
{
	if (d1 < d2) {
		return(-1);
	} else if (d1 == d2) {
		return(0);
	} else {
		return(1);
	}
}

/*****************************************************************//**
Sort a buffer pool dump on space_no, page_no. */
static
void
buf_dump_sort(
/*==========*/
	buf_dump_t*	dump,	/*!< in/out: buffer pool dump to sort */
	buf_dump_t*	tmp,	/*!< in/out: temp storage */
	ulint		low,	/*!< in: lowest index (inclusive) */
	ulint		high)	/*!< in: highest index (non-inclusive) */
{
	UT_SORT_FUNCTION_BODY(buf_dump_sort, dump, tmp, low, high,
			      buf_dump_cmp);
}

/*****************************************************************//**
Artificially delay the buffer pool loading if necessary. The idea of
this function is to prevent hogging the server with IO and slowing down
too much normal client queries. */
UNIV_INLINE
void
buf_load_throttle_if_needed(
/*========================*/
	ulint*	last_check_time,	/*!< in/out: miliseconds since epoch
					of the last time we did check if
					throttling is needed, we do the check
					every srv_io_capacity IO ops. */
	ulint*	last_activity_count,
	ulint	n_io)			/*!< in: number of IO ops done since
					buffer pool load has started */
{
	if (n_io % srv_io_capacity < srv_io_capacity - 1) {
		return;
	}

	if (*last_check_time == 0 || *last_activity_count == 0) {
		*last_check_time = ut_time_ms();
		*last_activity_count = srv_get_activity_count();
		return;
	}

	/* srv_io_capacity IO operations have been performed by buffer pool
	load since the last time we were here. */

	/* If no other activity, then keep going without any delay. */
	if (srv_get_activity_count() == *last_activity_count) {
		return;
	}

	/* There has been other activity, throttle. */

	ulint	now = ut_time_ms();
	ulint	elapsed_time = now - *last_check_time;

	/* Notice that elapsed_time is not the time for the last
	srv_io_capacity IO operations performed by BP load. It is the
	time elapsed since the last time we detected that there has been
	other activity. This has a small and acceptable deficiency, e.g.:
	1. BP load runs and there is no other activity.
	2. Other activity occurs, we run N IO operations after that and
	   enter here (where 0 <= N < srv_io_capacity).
	3. last_check_time is very old and we do not sleep at this time, but
	   only update last_check_time and last_activity_count.
	4. We run srv_io_capacity more IO operations and call this function
	   again.
	5. There has been more other activity and thus we enter here.
	6. Now last_check_time is recent and we sleep if necessary to prevent
	   more than srv_io_capacity IO operations per second.
	The deficiency is that we could have slept at 3., but for this we
	would have to update last_check_time before the
	"cur_activity_count == *last_activity_count" check and calling
	ut_time_ms() that often may turn out to be too expensive. */

	if (elapsed_time < 1000 /* 1 sec (1000 mili secs) */) {
		os_thread_sleep((1000 - elapsed_time) * 1000 /* micro secs */);
	}

	*last_check_time = ut_time_ms();
	*last_activity_count = srv_get_activity_count();
}

/*****************************************************************//**
Perform a buffer pool load from the file specified by
innodb_buffer_pool_filename. If any errors occur then the value of
innodb_buffer_pool_load_status will be set accordingly, see buf_load_status().
The dump filename can be specified by (relative to srv_data_home):
SET GLOBAL innodb_buffer_pool_filename='filename'; */
static
void
buf_load()
/*======*/
{
	char		full_filename[OS_FILE_MAX_PATH];
	char		now[32];
	FILE*		f;
	buf_dump_t*	dump;
	buf_dump_t*	dump_tmp;
	ulint		dump_n;
	ulint		total_buffer_pools_pages;
	ulint		i;
	ulint		space_id;
	ulint		page_no;
	int		fscanf_ret;

	/* Ignore any leftovers from before */
	buf_load_abort_flag = FALSE;

	ut_snprintf(full_filename, sizeof(full_filename),
		    "%s%c%s", get_buf_dump_dir(), SRV_PATH_SEPARATOR,
		    srv_buf_dump_filename);

	buf_load_status(STATUS_NOTICE,
			"Loading buffer pool(s) from %s", full_filename);

	f = fopen(full_filename, "r");
	if (f == NULL) {
		buf_load_status(STATUS_ERR,
				"Cannot open '%s' for reading: %s",
				full_filename, strerror(errno));
		return;
	}
	/* else */

	/* First scan the file to estimate how many entries are in it.
	This file is tiny (approx 500KB per 1GB buffer pool), reading it
	two times is fine. */
	dump_n = 0;
	while (fscanf(f, ULINTPF "," ULINTPF, &space_id, &page_no) == 2
	       && !SHUTTING_DOWN()) {
		dump_n++;
	}

	if (!SHUTTING_DOWN() && !feof(f)) {
		/* fscanf() returned != 2 */
		const char*	what;
		if (ferror(f)) {
			what = "reading";
		} else {
			what = "parsing";
		}
		fclose(f);
		buf_load_status(STATUS_ERR, "Error %s '%s', "
				"unable to load buffer pool (stage 1)",
				what, full_filename);
		return;
	}

	/* If dump is larger than the buffer pool(s), then we ignore the
	extra trailing. This could happen if a dump is made, then buffer
	pool is shrunk and then load it attempted. */
	total_buffer_pools_pages = buf_pool_get_n_pages()
		* srv_buf_pool_instances;
	if (dump_n > total_buffer_pools_pages) {
		dump_n = total_buffer_pools_pages;
	}

	dump = static_cast<buf_dump_t*>(ut_malloc(dump_n * sizeof(*dump)));

	if (dump == NULL) {
		fclose(f);
		buf_load_status(STATUS_ERR,
				"Cannot allocate " ULINTPF " bytes: %s",
				(ulint) (dump_n * sizeof(*dump)),
				strerror(errno));
		return;
	}

	dump_tmp = static_cast<buf_dump_t*>(
		ut_malloc(dump_n * sizeof(*dump_tmp)));

	if (dump_tmp == NULL) {
		ut_free(dump);
		fclose(f);
		buf_load_status(STATUS_ERR,
				"Cannot allocate " ULINTPF " bytes: %s",
				(ulint) (dump_n * sizeof(*dump_tmp)),
				strerror(errno));
		return;
	}

	rewind(f);

	for (i = 0; i < dump_n && !SHUTTING_DOWN(); i++) {
		fscanf_ret = fscanf(f, ULINTPF "," ULINTPF,
				    &space_id, &page_no);

		if (fscanf_ret != 2) {
			if (feof(f)) {
				break;
			}
			/* else */

			ut_free(dump);
			ut_free(dump_tmp);
			fclose(f);
			buf_load_status(STATUS_ERR,
					"Error parsing '%s', unable "
					"to load buffer pool (stage 2)",
					full_filename);
			return;
		}

		if (space_id > ULINT32_MASK || page_no > ULINT32_MASK) {
			ut_free(dump);
			ut_free(dump_tmp);
			fclose(f);
			buf_load_status(STATUS_ERR,
					"Error parsing '%s': bogus "
					"space,page " ULINTPF "," ULINTPF
					" at line " ULINTPF ", "
					"unable to load buffer pool",
					full_filename,
					space_id, page_no,
					i);
			return;
		}

		dump[i] = BUF_DUMP_CREATE(space_id, page_no);
	}

	/* Set dump_n to the actual number of initialized elements,
	i could be smaller than dump_n here if the file got truncated after
	we read it the first time. */
	dump_n = i;

	fclose(f);

	if (dump_n == 0) {
		ut_free(dump);
		ut_sprintf_timestamp(now);
		buf_load_status(STATUS_NOTICE,
				"Buffer pool(s) load completed at %s "
				"(%s was empty)", now, full_filename);
		return;
	}

	if (!SHUTTING_DOWN()) {
		buf_dump_sort(dump, dump_tmp, 0, dump_n);
	}

	ut_free(dump_tmp);

	ulint	last_check_time = 0;
	ulint	last_activity_cnt = 0;

	for (i = 0; i < dump_n && !SHUTTING_DOWN(); i++) {

		buf_read_page_async(BUF_DUMP_SPACE(dump[i]),
				    BUF_DUMP_PAGE(dump[i]));

		if (i % 64 == 63) {
			os_aio_simulated_wake_handler_threads();
		}

		if (i % 128 == 0) {
			buf_load_status(STATUS_INFO,
					"Loaded " ULINTPF "/" ULINTPF " pages",
					i + 1, dump_n);
		}

		if (buf_load_abort_flag) {
			buf_load_abort_flag = FALSE;
			ut_free(dump);
			buf_load_status(
				STATUS_NOTICE,
				"Buffer pool(s) load aborted on request");
			return;
		}

		buf_load_throttle_if_needed(
			&last_check_time, &last_activity_cnt, i);
	}

	ut_free(dump);

	ut_sprintf_timestamp(now);

	buf_load_status(STATUS_NOTICE,
			"Buffer pool(s) load completed at %s", now);
}

/*****************************************************************//**
Aborts a currently running buffer pool load. This function is called by
MySQL code via buffer_pool_load_abort() and it should return immediately
because the whole MySQL is frozen during its execution. */
UNIV_INTERN
void
buf_load_abort()
/*============*/
{
	buf_load_abort_flag = TRUE;
}

/*****************************************************************//**
This is the main thread for buffer pool dump/load. It waits for an
event and when waked up either performs a dump or load and sleeps
again.
@return this function does not return, it calls os_thread_exit() */
extern "C" UNIV_INTERN
os_thread_ret_t
DECLARE_THREAD(buf_dump_thread)(
/*============================*/
	void*	arg __attribute__((unused)))	/*!< in: a dummy parameter
						required by os_thread_create */
{
	ut_ad(!srv_read_only_mode);

	srv_buf_dump_thread_active = TRUE;

	buf_dump_status(STATUS_INFO, "Dumping buffer pool(s) not yet started");
	buf_load_status(STATUS_INFO, "Loading buffer pool(s) not yet started");

	if (srv_buffer_pool_load_at_startup) {
		buf_load();
	}

	while (!SHUTTING_DOWN()) {

		os_event_wait(srv_buf_dump_event);

		if (buf_dump_should_start) {
			buf_dump_should_start = FALSE;
			buf_dump(TRUE /* quit on shutdown */);
		}

		if (buf_load_should_start) {
			buf_load_should_start = FALSE;
			buf_load();
		}

		os_event_reset(srv_buf_dump_event);
	}

	if (srv_buffer_pool_dump_at_shutdown && srv_fast_shutdown != 2) {
		buf_dump(FALSE /* ignore shutdown down flag,
		keep going even if we are in a shutdown state */);
	}

	srv_buf_dump_thread_active = FALSE;

	/* We count the number of threads in os_thread_exit(). A created
	thread should always use that to exit and not use return() to exit. */
	os_thread_exit(NULL);

	OS_THREAD_DUMMY_RETURN;
}
