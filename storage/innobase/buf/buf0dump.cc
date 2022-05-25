/*****************************************************************************

Copyright (c) 2011, 2017, Oracle and/or its affiliates. All Rights Reserved.
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
@file buf/buf0dump.cc
Implements a buffer pool dump/load.

Created April 08, 2011 Vasil Dimov
*******************************************************/

#include "my_global.h"
#include "mysqld.h"
#include "my_sys.h"

#include "mysql/psi/mysql_stage.h"
#include "mysql/psi/psi.h"

#include "buf0buf.h"
#include "buf0dump.h"
#include "dict0dict.h"
#include "os0file.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "ut0byte.h"

#include <algorithm>

#include "mysql/service_wsrep.h" /* wsrep_recovery */
#include <my_service_manager.h>

static void buf_do_load_dump();

enum status_severity {
	STATUS_INFO,
	STATUS_ERR
};

#define SHUTTING_DOWN()	(srv_shutdown_state != SRV_SHUTDOWN_NONE)

/* Flags that tell the buffer pool dump/load thread which action should it
take after being waked up. */
static volatile bool	buf_dump_should_start;
static volatile bool	buf_load_should_start;

static bool	buf_load_abort_flag;

/** Start the buffer pool dump/load task and instructs it to start a dump. */
void buf_dump_start()
{
  buf_dump_should_start= true;
  buf_do_load_dump();
}

/** Start the buffer pool dump/load task and instructs it to start a load. */
void buf_load_start()
{
  buf_load_should_start= true;
  buf_do_load_dump();
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
static MY_ATTRIBUTE((nonnull, format(printf, 2, 3)))
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

	vsnprintf(
		export_vars.innodb_buffer_pool_dump_status,
		sizeof(export_vars.innodb_buffer_pool_dump_status),
		fmt, ap);

	switch (severity) {
	case STATUS_INFO:
		ib::info() << export_vars.innodb_buffer_pool_dump_status;
		break;

	case STATUS_ERR:
		ib::error() << export_vars.innodb_buffer_pool_dump_status;
		break;
	}

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
static MY_ATTRIBUTE((nonnull, format(printf, 2, 3)))
void
buf_load_status(
/*============*/
	enum status_severity	severity,/*!< in: status severity */
	const char*	fmt,	/*!< in: format */
	...)			/*!< in: extra parameters according to fmt */
{
	va_list	ap;

	va_start(ap, fmt);

	vsnprintf(
		export_vars.innodb_buffer_pool_load_status,
		sizeof(export_vars.innodb_buffer_pool_load_status),
		fmt, ap);

	switch (severity) {
	case STATUS_INFO:
		ib::info() << export_vars.innodb_buffer_pool_load_status;
		break;

	case STATUS_ERR:
		ib::error() << export_vars.innodb_buffer_pool_load_status;
		break;
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
	if (!*srv_data_home) {
		dump_dir = fil_path_to_mysql_datadir;
	} else {
		dump_dir = srv_data_home;
	}

	return(dump_dir);
}

/** Generate the path to the buffer pool dump/load file.
@param[out]	path		generated path
@param[in]	path_size	size of 'path', used as in snprintf(3). */
static void buf_dump_generate_path(char *path, size_t path_size)
{
	char	buf[FN_REFLEN];

	mysql_mutex_lock(&LOCK_global_system_variables);
	snprintf(buf, sizeof buf, "%s/%s", get_buf_dump_dir(),
		 srv_buf_dump_filename);
	mysql_mutex_unlock(&LOCK_global_system_variables);

	os_file_type_t	type;
	bool		exists = false;
	bool		ret;

	ret = os_file_status(buf, &exists, &type);

	/* For realpath() to succeed the file must exist. */

	if (ret && exists) {
		/* my_realpath() assumes the destination buffer is big enough
		to hold FN_REFLEN bytes. */
		ut_a(path_size >= FN_REFLEN);

		my_realpath(path, buf, 0);
	} else {
		/* If it does not exist, then resolve only srv_data_home
		and append srv_buf_dump_filename to it. */
		char	srv_data_home_full[FN_REFLEN];

		my_realpath(srv_data_home_full, get_buf_dump_dir(), 0);
		const char *format;

		switch (srv_data_home_full[strlen(srv_data_home_full) - 1]) {
#ifdef _WIN32
		case '\\':
#endif
		case '/':
			format = "%s%s";
			break;
		default:
			format = "%s/%s";
		}

		snprintf(path, path_size, format,
			 srv_data_home_full, srv_buf_dump_filename);
	}
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
	char	tmp_filename[OS_FILE_MAX_PATH + sizeof "incomplete"];
	char	now[32];
	FILE*	f;
	int	ret;

	buf_dump_generate_path(full_filename, sizeof(full_filename));

	snprintf(tmp_filename, sizeof(tmp_filename),
		 "%s.incomplete", full_filename);

	buf_dump_status(STATUS_INFO, "Dumping buffer pool(s) to %s",
			full_filename);

#ifdef _WIN32
	/* use my_fopen() for correct permissions during bootstrap*/
	f = my_fopen(tmp_filename, O_RDWR|O_TRUNC|O_CREAT, 0);
#elif defined(__GLIBC__) || O_CLOEXEC == 0
	f = fopen(tmp_filename, "w" STR_O_CLOEXEC);
#else
	{
		int	fd;
		fd = open(tmp_filename, O_CREAT | O_TRUNC | O_CLOEXEC | O_WRONLY, 0640);
		if (fd >= 0) {
			f = fdopen(fd, "w");
		}
		else {
			f = NULL;
		}
	}
#endif
	if (f == NULL) {
		buf_dump_status(STATUS_ERR,
				"Cannot open '%s' for writing: %s",
				tmp_filename, strerror(errno));
		return;
	}
	const buf_page_t*	bpage;
	page_id_t*		dump;
	ulint			n_pages;
	ulint			j;

	mysql_mutex_lock(&buf_pool.mutex);

	n_pages = UT_LIST_GET_LEN(buf_pool.LRU);

	/* skip empty buffer pools */
	if (n_pages == 0) {
		mysql_mutex_unlock(&buf_pool.mutex);
		goto done;
	}

	if (srv_buf_pool_dump_pct != 100) {
		ulint		t_pages;

		/* limit the number of total pages dumped to X% of the
		total number of pages */
		t_pages = buf_pool.curr_size * srv_buf_pool_dump_pct / 100;
		if (n_pages > t_pages) {
			buf_dump_status(STATUS_INFO,
					"Restricted to " ULINTPF
					" pages due to "
					"innodb_buf_pool_dump_pct=%lu",
					t_pages, srv_buf_pool_dump_pct);
			n_pages = t_pages;
		}

		if (n_pages == 0) {
			n_pages = 1;
		}
	}

	dump = static_cast<page_id_t*>(ut_malloc_nokey(
					       n_pages * sizeof(*dump)));

	if (dump == NULL) {
		std::ostringstream str_bytes;
		mysql_mutex_unlock(&buf_pool.mutex);
		fclose(f);
		str_bytes << ib::bytes_iec{n_pages * sizeof(*dump)};
		buf_dump_status(STATUS_ERR,
				"Cannot allocate %s: %s",
				str_bytes.str().c_str(),
				strerror(errno));
		/* leave tmp_filename to exist */
		return;
	}

	for (bpage = UT_LIST_GET_FIRST(buf_pool.LRU), j = 0;
	     bpage != NULL && j < n_pages;
	     bpage = UT_LIST_GET_NEXT(LRU, bpage)) {
		const auto status = bpage->state();
		if (status < buf_page_t::UNFIXED) {
			ut_a(status >= buf_page_t::FREED);
			continue;
		}
		const page_id_t id{bpage->id()};

		if (id.space() == SRV_TMP_SPACE_ID) {
			/* Ignore the innodb_temporary tablespace. */
			continue;
		}

		dump[j++] = id;
	}

	mysql_mutex_unlock(&buf_pool.mutex);

	ut_a(j <= n_pages);
	n_pages = j;

	for (j = 0; j < n_pages && !SHOULD_QUIT(); j++) {
		ret = fprintf(f, "%u,%u\n",
			      dump[j].space(), dump[j].page_no());
		if (ret < 0) {
			ut_free(dump);
			fclose(f);
			buf_dump_status(STATUS_ERR,
					"Cannot write to '%s': %s",
					tmp_filename, strerror(errno));
			/* leave tmp_filename to exist */
			return;
		}
		if (SHUTTING_DOWN() && !(j & 1023)) {
			service_manager_extend_timeout(
				INNODB_EXTEND_TIMEOUT_INTERVAL,
				"Dumping buffer pool page "
				ULINTPF "/" ULINTPF, j + 1, n_pages);
		}
	}

	ut_free(dump);

done:
	ret = IF_WIN(my_fclose(f,0),fclose(f));
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

	buf_dump_status(STATUS_INFO,
			"Buffer pool(s) dump completed at %s", now);

	/* Though dumping doesn't related to an incomplete load,
	 we reset this to 0 here to indicate that a shutdown can also perform
	 a dump */
	export_vars.innodb_buffer_pool_load_incomplete = 0;
}

/*****************************************************************//**
Artificially delay the buffer pool loading if necessary. The idea of
this function is to prevent hogging the server with IO and slowing down
too much normal client queries. */
UNIV_INLINE
void
buf_load_throttle_if_needed(
/*========================*/
	ulint*	last_check_time,	/*!< in/out: milliseconds since epoch
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

	if (elapsed_time < 1000 /* 1 sec (1000 milli secs) */) {
		std::this_thread::sleep_for(
			std::chrono::milliseconds(1000 - elapsed_time));
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
	page_id_t*	dump;
	ulint		dump_n;
	ulint		i;
	uint32_t	space_id;
	uint32_t	page_no;
	int		fscanf_ret;

	/* Ignore any leftovers from before */
	buf_load_abort_flag = false;

	buf_dump_generate_path(full_filename, sizeof(full_filename));

	buf_load_status(STATUS_INFO,
			"Loading buffer pool(s) from %s", full_filename);

	f = fopen(full_filename, "r" STR_O_CLOEXEC);
	if (f == NULL) {
		buf_load_status(STATUS_INFO,
				"Cannot open '%s' for reading: %s",
				full_filename, strerror(errno));
		return;
	}
	/* else */

	/* First scan the file to estimate how many entries are in it.
	This file is tiny (approx 500KB per 1GB buffer pool), reading it
	two times is fine. */
	dump_n = 0;
	while (fscanf(f, "%u,%u", &space_id, &page_no) == 2
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
		buf_load_status(STATUS_ERR, "Error %s '%s',"
				" unable to load buffer pool (stage 1)",
				what, full_filename);
		return;
	}

	/* If dump is larger than the buffer pool(s), then we ignore the
	extra trailing. This could happen if a dump is made, then buffer
	pool is shrunk and then load is attempted. */
	dump_n = std::min(dump_n, buf_pool.get_n_pages());

	if (dump_n != 0) {
		dump = static_cast<page_id_t*>(ut_malloc_nokey(
				dump_n * sizeof(*dump)));
	} else {
		fclose(f);
		ut_sprintf_timestamp(now);
		buf_load_status(STATUS_INFO,
				"Buffer pool(s) load completed at %s"
				" (%s was empty)", now, full_filename);
		return;
	}

	if (dump == NULL) {
		std::ostringstream str_bytes;
		fclose(f);
		str_bytes << ib::bytes_iec{dump_n * sizeof(*dump)};
		buf_dump_status(STATUS_ERR,
				"Cannot allocate %s: %s",
				str_bytes.str().c_str(),
				strerror(errno));
		/* leave tmp_filename to exist */
		return;
	}

	rewind(f);

	export_vars.innodb_buffer_pool_load_incomplete = 1;

	for (i = 0; i < dump_n && !SHUTTING_DOWN(); i++) {
		fscanf_ret = fscanf(f, "%u,%u", &space_id, &page_no);

		if (fscanf_ret != 2) {
			if (feof(f)) {
				break;
			}
			/* else */

			ut_free(dump);
			fclose(f);
			buf_load_status(STATUS_ERR,
					"Error parsing '%s', unable"
					" to load buffer pool (stage 2)",
					full_filename);
			return;
		}

		if (space_id > ULINT32_MASK || page_no > ULINT32_MASK) {
			ut_free(dump);
			fclose(f);
			buf_load_status(STATUS_ERR,
					"Error parsing '%s': bogus"
					" space,page %u,%u at line " ULINTPF
					", unable to load buffer pool",
					full_filename,
					space_id, page_no,
					i);
			return;
		}

		dump[i] = page_id_t(space_id, page_no);
	}

	/* Set dump_n to the actual number of initialized elements,
	i could be smaller than dump_n here if the file got truncated after
	we read it the first time. */
	dump_n = i;

	fclose(f);

	if (dump_n == 0) {
		ut_free(dump);
		ut_sprintf_timestamp(now);
		buf_load_status(STATUS_INFO,
				"Buffer pool(s) load completed at %s"
				" (%s was empty or had errors)", now, full_filename);
		return;
	}

	if (!SHUTTING_DOWN()) {
		std::sort(dump, dump + dump_n);
	}

	ulint		last_check_time = 0;
	ulint		last_activity_cnt = 0;

	/* Avoid calling the expensive fil_space_t::get() for each
	page within the same tablespace. dump[] is sorted by (space, page),
	so all pages from a given tablespace are consecutive. */
	uint32_t	cur_space_id = dump[0].space();
	fil_space_t*	space = fil_space_t::get(cur_space_id);
	ulint		zip_size = space ? space->zip_size() : 0;

	PSI_stage_progress*	pfs_stage_progress __attribute__((unused))
		= mysql_set_stage(srv_stage_buffer_pool_load.m_key);
	mysql_stage_set_work_estimated(pfs_stage_progress, dump_n);
	mysql_stage_set_work_completed(pfs_stage_progress, 0);

	for (i = 0; i < dump_n && !SHUTTING_DOWN(); i++) {

		/* space_id for this iteration of the loop */
		const uint32_t this_space_id = dump[i].space();

		if (this_space_id >= SRV_SPACE_ID_UPPER_BOUND) {
			continue;
		}

		if (this_space_id != cur_space_id) {
			if (space) {
				space->release();
			}

			cur_space_id = this_space_id;
			space = fil_space_t::get(cur_space_id);

			if (!space) {
				continue;
			}

			zip_size = space->zip_size();
		}

		/* JAN: TODO: As we use background page read below,
		if tablespace is encrypted we cant use it. */
		if (!space || dump[i].page_no() >= space->get_size() ||
		    (space->crypt_data &&
		     space->crypt_data->encryption != FIL_ENCRYPTION_OFF &&
		     space->crypt_data->type != CRYPT_SCHEME_UNENCRYPTED)) {
			continue;
		}

		if (space->is_stopping()) {
			space->release();
			space = nullptr;
			continue;
		}

		space->reacquire();
		buf_read_page_background(space, dump[i], zip_size);

		if (buf_load_abort_flag) {
			if (space) {
				space->release();
			}
			buf_load_abort_flag = false;
			ut_free(dump);
			buf_load_status(
				STATUS_INFO,
				"Buffer pool(s) load aborted on request");
			/* Premature end, set estimated = completed = i and
			end the current stage event. */

			mysql_stage_set_work_estimated(pfs_stage_progress, i);
			mysql_stage_set_work_completed(pfs_stage_progress, i);

			mysql_end_stage();
			return;
		}

		buf_load_throttle_if_needed(
			&last_check_time, &last_activity_cnt, i);

#ifdef UNIV_DEBUG
		if ((i+1) >= srv_buf_pool_load_pages_abort) {
			buf_load_abort_flag = true;
		}
#endif
	}

	if (space) {
		space->release();
	}

	ut_free(dump);

	if (i == dump_n) {
		os_aio_wait_until_no_pending_reads();
	}

	ut_sprintf_timestamp(now);

	if (i == dump_n) {
		buf_load_status(STATUS_INFO,
			"Buffer pool(s) load completed at %s", now);
		export_vars.innodb_buffer_pool_load_incomplete = 0;
	} else if (!buf_load_abort_flag) {
		buf_load_status(STATUS_INFO,
			"Buffer pool(s) load aborted due to user instigated abort at %s",
			now);
		/* intentionally don't reset innodb_buffer_pool_load_incomplete
                   as we don't want a shutdown to save the buffer pool */
	} else {
		buf_load_status(STATUS_INFO,
			"Buffer pool(s) load aborted due to shutdown at %s",
			now);
		/* intentionally don't reset innodb_buffer_pool_load_incomplete
                   as we want to abort without saving the buffer pool */
	}

	/* Make sure that estimated = completed when we end. */
	mysql_stage_set_work_completed(pfs_stage_progress, dump_n);
	/* End the stage progress event. */
	mysql_end_stage();
}

/** Abort a currently running buffer pool load. */
void buf_load_abort()
{
  buf_load_abort_flag= true;
}

/*****************************************************************//**
This is the main task for buffer pool dump/load. when scheduled
either performs a dump or load, depending on server state, state of the variables etc- */
static void buf_dump_load_func(void *)
{
	ut_ad(!srv_read_only_mode);
	static bool first_time = true;
	if (first_time && srv_buffer_pool_load_at_startup) {

#ifdef WITH_WSREP
		if (!get_wsrep_recovery()) {
#endif /* WITH_WSREP */
			buf_load();
#ifdef WITH_WSREP
		}
#endif /* WITH_WSREP */
	}
	first_time = false;

	while (!SHUTTING_DOWN()) {
		if (buf_dump_should_start) {
			buf_dump_should_start = false;
			buf_dump(true);
		}
		if (buf_load_should_start) {
			buf_load_should_start = false;
			buf_load();
		}

		if (!buf_dump_should_start && !buf_load_should_start) {
			return;
		}
	}

	/* In shutdown */
	if (srv_buffer_pool_dump_at_shutdown && srv_fast_shutdown != 2) {
		if (export_vars.innodb_buffer_pool_load_incomplete) {
			buf_dump_status(STATUS_INFO,
				"Dumping of buffer pool not started"
				" as load was incomplete");
#ifdef WITH_WSREP
		} else if (get_wsrep_recovery()) {
#endif /* WITH_WSREP */
		} else {
			buf_dump(false/* do complete dump at shutdown */);
		}
	}
}


/* Execute task with max.concurrency */
static tpool::task_group tpool_group(1);
static tpool::waitable_task buf_dump_load_task(buf_dump_load_func, &tpool_group);
static bool load_dump_enabled;

/** Start async buffer pool load, if srv_buffer_pool_load_at_startup was set.*/
void buf_load_at_startup()
{
  load_dump_enabled= true;
  if (srv_buffer_pool_load_at_startup)
    buf_do_load_dump();
}

static void buf_do_load_dump()
{
  if (load_dump_enabled && !buf_dump_load_task.is_running())
    srv_thread_pool->submit_task(&buf_dump_load_task);
}

/** Wait for currently running load/dumps to finish*/
void buf_load_dump_end()
{
  ut_ad(SHUTTING_DOWN());
  buf_dump_load_task.wait();
}
