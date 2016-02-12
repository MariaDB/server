/***********************************************************************

Copyright (c) 2011, 2015, Oracle and/or its affiliates. All rights reserved.

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

***********************************************************************/

/**************************************************//**
@file innodb_engine.c
InnoDB Memcached Engine code

Extracted and modified from NDB memcached project
04/12/2011 Jimmy Yang
*******************************************************/

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <memcached/util.h>
#include <memcached/config_parser.h>
#include <memcached/context.h>
#include <unistd.h>

#include "innodb_engine.h"
#include "innodb_engine_private.h"
#include "plugin_api.h"
#include "innodb_api.h"
#include "hash_item_util.h"
#include "innodb_cb_api.h"
#include "print_log.h"

/** Define also present in daemon/memcached.h */
#define KEY_MAX_LENGTH	250

/** Time (in seconds) that background thread sleeps before it wakes
up and commit idle connection transactions */
#define BK_COMMIT_THREAD_SLEEP_INTERVAL		5

/** Maximum number of connections that background thread processes each
time */
#define	BK_MAX_PROCESS_COMMIT			5

/** Minimum time (in seconds) that a connection has been idle, that makes
it candidate for background thread to commit it */
#define CONN_IDLE_TIME_TO_BK_COMMIT		5

/** Tells whether memcached plugin is being shutdown */
static bool	plugin_shutdown		= false;
pthread_mutex_t	plugin_shutdown_mutex;
pthread_cond_t	plugin_shutdown_cv;

/** Tells whether the background thread is exited */
static bool	bk_thd_exited		= true;

/**********************************************************************//**
Unlock a table and commit the transaction
return 0 if fail to commit the transaction */
extern
int
handler_unlock_table(
/*=================*/
	void*	my_thd,			/*!< in: thread */
	void*	my_table,		/*!< in: Table metadata */
	int	my_lock_mode);		/*!< in: lock mode */

/*******************************************************************//**
Get InnoDB Memcached engine handle
@return InnoDB Memcached engine handle */
static inline
struct innodb_engine*
innodb_handle(
/*==========*/
	ENGINE_HANDLE*	handle)		/*!< in: Generic engine handle */
{
	return((struct innodb_engine*) handle);
}

/*******************************************************************//**
Cleanup idle connections if "clear_all" is false, and clean up all
connections if "clear_all" is true.
@return number of connection cleaned */
static
void
innodb_conn_clean_data(
/*===================*/
	innodb_conn_data_t*	conn_data,
	bool			has_lock,
	bool			free_all);

/*******************************************************************//**
check whether a table mapping switch is needed, if so, switch the table
mapping
@return ENGINE_SUCCESS if successful otherwise error code */
static
ENGINE_ERROR_CODE
check_container_for_map_switch(
/*==========================*/
		ENGINE_HANDLE*		handle,		/*!< in: Engine Handle */
		const void*		cookie);	/*!< in: connection cookie */

/*********** FUNCTIONS IMPLEMENTING THE PUBLISHED API BEGIN HERE ********/

/*******************************************************************//**
Create InnoDB Memcached Engine.
@return ENGINE_SUCCESS if successful, otherwise, error code */
ENGINE_ERROR_CODE
create_instance(
/*============*/
	uint64_t		interface,	/*!< in: protocol version,
						currently always 1 */
	GET_SERVER_API		get_server_api,	/*!< in: Callback the engines
						may call to get the public
						server interface */
	ENGINE_HANDLE**		handle )	/*!< out: Engine handle */
{
	struct innodb_engine*	innodb_eng;

	SERVER_HANDLE_V1 *api = get_server_api();

	if (interface != 1 || api == NULL) {
		return(ENGINE_ENOTSUP);
	}

	innodb_eng = malloc(sizeof(struct innodb_engine));
	memset(innodb_eng, 0, sizeof(*innodb_eng));

	if (innodb_eng == NULL) {
		return(ENGINE_ENOMEM);
	}

	innodb_eng->engine.interface.interface = 1;
	innodb_eng->engine.get_info = innodb_get_info;
	innodb_eng->engine.initialize = innodb_initialize;
	innodb_eng->engine.destroy = innodb_destroy;
	innodb_eng->engine.allocate = innodb_allocate;
	innodb_eng->engine.remove = innodb_remove;
	innodb_eng->engine.release = innodb_release;
	innodb_eng->engine.clean_engine= innodb_clean_engine;
	innodb_eng->engine.get = innodb_get;
	innodb_eng->engine.get_stats = innodb_get_stats;
	innodb_eng->engine.reset_stats = innodb_reset_stats;
	innodb_eng->engine.store = innodb_store;
	innodb_eng->engine.arithmetic = innodb_arithmetic;
	innodb_eng->engine.flush = innodb_flush;
	innodb_eng->engine.unknown_command = innodb_unknown_command;
	innodb_eng->engine.item_set_cas = innodb_item_set_cas;
	innodb_eng->engine.get_item_info = innodb_get_item_info;
	innodb_eng->engine.get_stats_struct = NULL;
	innodb_eng->engine.errinfo = NULL;

	innodb_eng->server = *api;
	innodb_eng->get_server_api = get_server_api;

	/* configuration, with default values*/
	innodb_eng->info.info.description = "daemon_memcached_engine_ib " VERSION;
	innodb_eng->info.info.num_features = 3;
	innodb_eng->info.info.features[0].feature = ENGINE_FEATURE_CAS;
	innodb_eng->info.info.features[1].feature =
		ENGINE_FEATURE_PERSISTENT_STORAGE;
	innodb_eng->info.info.features[0].feature = ENGINE_FEATURE_LRU;

	innodb_eng->clean_stale_conn = false;
	innodb_eng->initialized = true;

	*handle = (ENGINE_HANDLE*) &innodb_eng->engine;

	return(ENGINE_SUCCESS);
}

/*******************************************************************//**
background thread to commit trx.
@return dummy parameter */
static
void*
innodb_bk_thread(
/*=============*/
	void*   arg)
{
	ENGINE_HANDLE*		handle;
	struct innodb_engine*	innodb_eng;
	innodb_conn_data_t*	conn_data;
	void*			thd = NULL;

	bk_thd_exited = false;

	handle = (ENGINE_HANDLE*) (arg);
	innodb_eng = innodb_handle(handle);

	if (innodb_eng->enable_binlog) {
		/* This thread will commit the transactions
		on behalf of the other threads. It will "pretend"
		to be each connection thread while doing it. */
		thd = handler_create_thd(true);
	}

	conn_data = UT_LIST_GET_FIRST(innodb_eng->conn_data);

	pthread_mutex_lock(&plugin_shutdown_mutex);

	while (!plugin_shutdown) {
		innodb_conn_data_t*	next_conn_data;
		uint64_t                time;
		uint64_t		trx_start = 0;
		uint64_t		processed_count = 0;
		struct timespec		ts;

		/* Do the cleanup every innodb_eng->bk_commit_interval
		seconds. */

		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += innodb_eng->bk_commit_interval;
		pthread_cond_timedwait(&plugin_shutdown_cv, &plugin_shutdown_mutex, &ts);

		time = mci_get_time();

		if (UT_LIST_GET_LEN(innodb_eng->conn_data) == 0) {
			continue;
		}

		/* Set the clean_stale_conn to prevent force clean in
		innodb_conn_clean. */
		LOCK_CONN_IF_NOT_LOCKED(false, innodb_eng);
		innodb_eng->clean_stale_conn = true;
		UNLOCK_CONN_IF_NOT_LOCKED(false, innodb_eng);

		if (!conn_data) {
			conn_data = UT_LIST_GET_FIRST(innodb_eng->conn_data);
		}

		if (conn_data) {
			next_conn_data = UT_LIST_GET_NEXT(conn_list, conn_data);
		} else {
			next_conn_data = NULL;
		}

		while (conn_data) {
			LOCK_CURRENT_CONN_IF_NOT_LOCKED(false, conn_data);

			if (conn_data->is_stale) {
				UNLOCK_CURRENT_CONN_IF_NOT_LOCKED(
					false, conn_data);
				LOCK_CONN_IF_NOT_LOCKED(false, innodb_eng);
				UT_LIST_REMOVE(conn_list, innodb_eng->conn_data,
					       conn_data);
				UNLOCK_CONN_IF_NOT_LOCKED(false, innodb_eng);
				innodb_conn_clean_data(conn_data, false, true);
				goto next_item;
			}

			if (conn_data->crsr_trx) {
				trx_start = ib_cb_trx_get_start_time(
						conn_data->crsr_trx);
			}

			/* Check the trx, if it is qualified for
			reset and commit */
			if ((conn_data->n_writes_since_commit > 0
			     || conn_data->n_reads_since_commit > 0)
			    && trx_start
			    && (time - trx_start > CONN_IDLE_TIME_TO_BK_COMMIT)
			    && !conn_data->in_use) {
				/* binlog is running, make the thread
				attach to conn_data->thd for binlog
				committing */
				if (thd) {
					handler_thd_attach(
						conn_data->thd, NULL);
				}

				innodb_reset_conn(conn_data, true, true,
						  innodb_eng->enable_binlog);
				processed_count++;
			}

			UNLOCK_CURRENT_CONN_IF_NOT_LOCKED(false, conn_data);

next_item:
			conn_data = next_conn_data;

			/* Process BK_MAX_PROCESS_COMMIT (5) trx at a time */
			if (processed_count > BK_MAX_PROCESS_COMMIT) {
				break;
			}

			if (conn_data) {
				next_conn_data = UT_LIST_GET_NEXT(
					conn_list, conn_data);
			}
		}
		/* Set the clean_stale_conn back. */
		LOCK_CONN_IF_NOT_LOCKED(false, innodb_eng);
		innodb_eng->clean_stale_conn = false;
		UNLOCK_CONN_IF_NOT_LOCKED(false, innodb_eng);
	}

	pthread_mutex_unlock(&plugin_shutdown_mutex);

	bk_thd_exited = true;

	/* Change to its original state before close the MySQL THD */
	if (thd) {
		handler_thd_attach(thd, NULL);
		handler_close_thd(thd);
	}

	pthread_detach(pthread_self());
        pthread_exit(NULL);

	return((void*) 0);
}

/*******************************************************************//**
Get engine info.
@return engine info */
static
const engine_info*
innodb_get_info(
/*============*/
	ENGINE_HANDLE*	handle)		/*!< in: Engine handle */
{
	return(&innodb_handle(handle)->info.info);
}

/*******************************************************************//**
Populate containers array in memcached context.
@return ENGINE_SUCCESS if successful otherwise error code */
static
ENGINE_ERROR_CODE
populate_containers_array(
/*======================*/
	ENGINE_HANDLE*		handle,		/*!< in: Engine Handle */
	memcached_context_t*	context)	/*!< out: memcached context */
{
	struct innodb_engine*	innodb_eng = innodb_handle(handle);

	ib_ulint_t		i;
	hash_table_t*		TABLE;
	meta_cfg_info_t*	data;
	unsigned int		n = 0;

	TABLE = innodb_eng->meta_hash;

	for (i = 0; i < TABLE->n_cells; i++) {
		data = (meta_cfg_info_t*) HASH_GET_FIRST(TABLE, i);

		while (data) {
			n++;
			data = HASH_GET_NEXT(name_hash, data);
		}
	}

	/* This memory is freed in memcached_mysql.cc/daemon_memcached_plugin_deinit() */
	context->containers = (memcached_container_t *) malloc(n * sizeof(memcached_container_t));
	context->containers_number = n;

	n = 0;

	for (i = 0; i < TABLE->n_cells; i++) {
		data = (meta_cfg_info_t*) HASH_GET_FIRST(TABLE, i);

		while (data) {
			assert(n < context->containers_number);
			/* This memory is freed in memcached_mysql.cc/daemon_memcached_plugin_deinit() */
			context->containers[n].name = strdup(data->col_info[CONTAINER_NAME].col_name);
			n++;
			data = HASH_GET_NEXT(name_hash, data);
		}
	}

	assert(n == context->containers_number);

	return(ENGINE_SUCCESS);
}

/*******************************************************************//**
Initialize InnoDB Memcached Engine.
@return ENGINE_SUCCESS if successful */
static
ENGINE_ERROR_CODE
innodb_initialize(
/*==============*/
	ENGINE_HANDLE*	handle,		/*!< in/out: InnoDB memcached
					engine */
	const char*	config_str)	/*!< in: configure string */
{
	ENGINE_ERROR_CODE	return_status = ENGINE_SUCCESS;
	struct innodb_engine*	innodb_eng = innodb_handle(handle);
	memcached_context_t	*context;
	pthread_attr_t          attr;
	meta_cfg_info_t*	meta_info;
	ib_cb_t**		innodb_cb;

	context = (memcached_context_t *) config_str;

	pthread_mutex_init(&plugin_shutdown_mutex, NULL);
	pthread_cond_init(&plugin_shutdown_cv, NULL);

	innodb_cb = obtain_innodb_cb();

	/* If no call back function registered (InnoDB engine failed to load),
	load InnoDB Memcached engine should fail too */
	if (!(innodb_cb)) {
		return(ENGINE_TMPFAIL);
	}

	/* Register the call back function */
	register_innodb_cb((void*) innodb_cb);

	innodb_eng->read_batch_size = (context->config.r_batch_size
					? context->config.r_batch_size
					: CONN_NUM_READ_COMMIT);

	innodb_eng->write_batch_size = (context->config.w_batch_size
					? context->config.w_batch_size
					: CONN_NUM_WRITE_COMMIT);

	innodb_eng->enable_binlog = context->config.enable_binlog;

	innodb_eng->cfg_status = innodb_cb_get_cfg();

	/* If binlog is not enabled by InnoDB memcached plugin, let's
	check whether innodb_direct_access_enable_binlog is turned on */
	if (!innodb_eng->enable_binlog) {
		innodb_eng->enable_binlog = innodb_eng->cfg_status
					    & IB_CFG_BINLOG_ENABLED;
	}

	innodb_eng->enable_mdl = innodb_eng->cfg_status & IB_CFG_MDL_ENABLED;
	innodb_eng->trx_level = ib_cb_cfg_trx_level();
	innodb_eng->bk_commit_interval = ib_cb_cfg_bk_commit_interval();

	UT_LIST_INIT(innodb_eng->conn_data);
	pthread_mutex_init(&innodb_eng->conn_mutex, NULL);
	pthread_mutex_init(&innodb_eng->cas_mutex, NULL);
	pthread_mutex_init(&innodb_eng->flush_mutex, NULL);

	/* Fetch InnoDB specific settings */
	meta_info = innodb_config(NULL, 0, &innodb_eng->meta_hash);

	if (!meta_info) {
		print_log_warning(" No containers defined\n");
		return(ENGINE_TMPFAIL);
	}

	return_status = populate_containers_array(handle, context);

	if (return_status != ENGINE_SUCCESS) {
		return(return_status);
	}

	plugin_shutdown = false;

        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
        pthread_create(&innodb_eng->bk_thd_for_commit, &attr, innodb_bk_thread,
		       handle);

	return(return_status);
}

extern void handler_close_thd(void*);

/*******************************************************************//**
Cleanup idle connections if "clear_all" is false, and clean up all
connections if "clear_all" is true.
@return number of connection cleaned */
static
void
innodb_conn_clean_data(
/*===================*/
	innodb_conn_data_t*	conn_data,
	bool			has_lock,
	bool			free_all)
{
	if (!conn_data) {
		return;
	}

	LOCK_CURRENT_CONN_IF_NOT_LOCKED(has_lock, conn_data);

	if (conn_data->idx_crsr) {
		innodb_cb_cursor_close(conn_data->idx_crsr);
		conn_data->idx_crsr = NULL;
	}

	if (conn_data->idx_read_crsr) {
		innodb_cb_cursor_close(conn_data->idx_read_crsr);
		conn_data->idx_read_crsr = NULL;
	}

	if (conn_data->crsr) {
		innodb_cb_cursor_close(conn_data->crsr);
		conn_data->crsr = NULL;
	}

	if (conn_data->read_crsr) {
		innodb_cb_cursor_close(conn_data->read_crsr);
		conn_data->read_crsr = NULL;
	}

	if (conn_data->crsr_trx) {
		ib_err_t        err;
		innodb_cb_trx_commit(conn_data->crsr_trx);
		err = ib_cb_trx_release(conn_data->crsr_trx);
		assert(err == DB_SUCCESS);
		conn_data->crsr_trx = NULL;
	}

	if (conn_data->mysql_tbl) {
		assert(conn_data->thd);
		handler_unlock_table(conn_data->thd,
				     conn_data->mysql_tbl,
				     HDL_READ);
		conn_data->mysql_tbl = NULL;
	}

	if (conn_data->thd) {
		handler_close_thd(conn_data->thd);
		conn_data->thd = NULL;
	}

	if (conn_data->tpl) {
		ib_cb_tuple_delete(conn_data->tpl);
		conn_data->tpl = NULL;
	}

	if (conn_data->idx_tpl) {
		ib_cb_tuple_delete(conn_data->idx_tpl);
		conn_data->idx_tpl = NULL;
	}

	if (conn_data->read_tpl) {
		ib_cb_tuple_delete(conn_data->read_tpl);
		conn_data->read_tpl = NULL;
	}

	if (conn_data->sel_tpl) {
		ib_cb_tuple_delete(conn_data->sel_tpl);
		conn_data->sel_tpl = NULL;
	}

	UNLOCK_CURRENT_CONN_IF_NOT_LOCKED(has_lock, conn_data);

	if (free_all) {
		if (conn_data->result) {
			free(conn_data->result);
			conn_data->result = NULL;
		}

		if (conn_data->row_buf) {
			free(conn_data->row_buf);
			conn_data->row_buf = NULL;
			conn_data->row_buf_len = 0;
		}

		if (conn_data->cmd_buf) {
			free(conn_data->cmd_buf);
			conn_data->cmd_buf = NULL;
			conn_data->cmd_buf_len = 0;
		}

		if (conn_data->mul_col_buf) {
			free(conn_data->mul_col_buf);
			conn_data->mul_col_buf = NULL;
			conn_data->mul_col_buf_len = 0;
		}

		pthread_mutex_destroy(&conn_data->curr_conn_mutex);
		free(conn_data);
	}
}

/*******************************************************************//**
Cleanup idle connections if "clear_all" is false, and clean up all
connections if "clear_all" is true.
@return number of connection cleaned */
static
int
innodb_conn_clean(
/*==============*/
	innodb_engine_t*	engine,		/*!< in/out: InnoDB memcached
						engine */
	bool			clear_all,	/*!< in: Clear all connection */
	bool			has_lock)	/*!< in: Has engine mutext */
{
	innodb_conn_data_t*	conn_data;
	innodb_conn_data_t*	next_conn_data;
	int			num_freed = 0;
	void*			thd = NULL;

	if (engine->enable_binlog && clear_all) {
		thd = handler_create_thd(true);
	}

	LOCK_CONN_IF_NOT_LOCKED(has_lock, engine);

	conn_data = UT_LIST_GET_FIRST(engine->conn_data);

	while (conn_data) {
		void*	cookie = conn_data->conn_cookie;

		next_conn_data = UT_LIST_GET_NEXT(conn_list, conn_data);

		if (!clear_all && !conn_data->in_use) {
			innodb_conn_data_t*	check_data;
			check_data = engine->server.cookie->get_engine_specific(
				cookie);

			/* The check data is the original conn_data stored
			in connection "cookie", it can be set to NULL if
			connection closed, or to a new conn_data if it is
			closed and reopened. So verify and see if our
			current conn_data is stale */
			if (!check_data || check_data != conn_data) {
				assert(conn_data->is_stale);
			}
		}

		/* If current conn is stale or clear_all is true,
		clean up it.*/
		if (conn_data->is_stale) {
			/* If bk thread is doing the same thing, stop
			the loop to avoid confliction.*/
			if (engine->clean_stale_conn)
				break;

			UT_LIST_REMOVE(conn_list, engine->conn_data,
				       conn_data);
			innodb_conn_clean_data(conn_data, false, true);
			num_freed++;
		} else {
			if (clear_all) {
				UT_LIST_REMOVE(conn_list, engine->conn_data,
					       conn_data);

				if (thd) {
					handler_thd_attach(conn_data->thd,
							   NULL);
				}

				innodb_reset_conn(conn_data, false, true,
						  engine->enable_binlog);
				if (conn_data->thd) {
					handler_thd_attach(
						conn_data->thd, NULL);
				}
				innodb_conn_clean_data(conn_data, false, true);

				engine->server.cookie->store_engine_specific(
					cookie, NULL);
				num_freed++;
			}
		}

		conn_data = next_conn_data;
	}

	assert(!clear_all || engine->conn_data.count == 0);

	UNLOCK_CONN_IF_NOT_LOCKED(has_lock, engine);

	if (thd) {
		handler_thd_attach(thd, NULL);
		handler_close_thd(thd);
	}

	return(num_freed);
}

/*******************************************************************//**
Destroy and Free InnoDB Memcached engine */
static
void
innodb_destroy(
/*===========*/
	ENGINE_HANDLE*	handle,		/*!< in: Destroy the engine instance */
	bool		force __attribute__((unused)))		/*!< in: Force to destroy */
{
	struct innodb_engine* innodb_eng = innodb_handle(handle);

	pthread_mutex_lock(&plugin_shutdown_mutex);
	plugin_shutdown = true;
	pthread_cond_signal(&plugin_shutdown_cv);
	pthread_mutex_unlock(&plugin_shutdown_mutex);

	/* Wait for the background thread to exit */
	while (!bk_thd_exited) {
		usleep(1000);
	}

	innodb_conn_clean(innodb_eng, true, false);

	if (innodb_eng->meta_hash) {
		HASH_CLEANUP(innodb_eng->meta_hash, meta_cfg_info_t*);
	}

	pthread_cond_destroy(&plugin_shutdown_cv);
	pthread_mutex_destroy(&plugin_shutdown_mutex);

	pthread_mutex_destroy(&innodb_eng->conn_mutex);
	pthread_mutex_destroy(&innodb_eng->cas_mutex);
	pthread_mutex_destroy(&innodb_eng->flush_mutex);

	free(innodb_eng);
}

/** Defines for connection initialization to indicate if we will
do a read or write operation, or in the case of CONN_MODE_NONE, just get
the connection's conn_data structure */
enum conn_mode {
	CONN_MODE_READ,
	CONN_MODE_WRITE,
	CONN_MODE_NONE
};

/*******************************************************************//**
Cleanup connections
@return number of connection cleaned */
/* Initialize a connection's cursor and transactions
@return the connection's conn_data structure */
static
innodb_conn_data_t*
innodb_conn_init(
/*=============*/
	innodb_engine_t*	engine,		/*!< in/out: InnoDB memcached
						engine */
	const void*		cookie,		/*!< in: This connection's
						cookie */
	int			conn_option,	/*!< in: whether it is
						for read or write operation*/
	ib_lck_mode_t		lock_mode,	/*!< in: Table lock mode */
	bool			has_lock,	/*!< in: Has engine mutex */
	meta_cfg_info_t*	new_meta_info)	/*!< in: meta info for
						table to open or NULL */
{
	innodb_conn_data_t*	conn_data;
	meta_cfg_info_t*	meta_info;
	meta_index_t*		meta_index;
	ib_err_t		err = DB_SUCCESS;
	ib_crsr_t		crsr;
	ib_crsr_t		read_crsr;
	ib_crsr_t		idx_crsr;
	bool			trx_updated = false;

	/* Get this connection's conn_data */
	conn_data = engine->server.cookie->get_engine_specific(cookie);

	assert(!conn_data || !conn_data->in_use);

	if (!conn_data) {
		assert(new_meta_info);

		LOCK_CONN_IF_NOT_LOCKED(has_lock, engine);
		conn_data = engine->server.cookie->get_engine_specific(cookie);

		if (conn_data) {
                        UNLOCK_CONN_IF_NOT_LOCKED(has_lock, engine);
			goto have_conn;
		}

		if (UT_LIST_GET_LEN(engine->conn_data) > 2048) {
			/* Some of conn_data can be stale, recycle them */
			innodb_conn_clean(engine, false, true);
		}

		conn_data = malloc(sizeof(*conn_data));

		if (!conn_data) {
			UNLOCK_CONN_IF_NOT_LOCKED(has_lock, engine);
			return(NULL);
		}

		memset(conn_data, 0, sizeof(*conn_data));
		conn_data->result = malloc(sizeof(mci_item_t));
		conn_data->conn_cookie = (void*) cookie;
		UT_LIST_ADD_LAST(conn_list, engine->conn_data, conn_data);
		engine->server.cookie->store_engine_specific(
			cookie, conn_data);
		conn_data->conn_meta = new_meta_info;
		conn_data->row_buf = malloc(1024);
		conn_data->row_buf_len = 1024;

		conn_data->cmd_buf = malloc(1024);
		conn_data->cmd_buf_len = 1024;

		conn_data->is_flushing = false;
		conn_data->is_memcached_sync = false;

		pthread_mutex_init(&conn_data->curr_conn_mutex, NULL);
		UNLOCK_CONN_IF_NOT_LOCKED(has_lock, engine);
	}
have_conn:
	meta_info = conn_data->conn_meta;
	meta_index = &meta_info->index_info;

	assert(engine->conn_data.count > 0);

	if (conn_option == CONN_MODE_NONE) {
		return(conn_data);
	}

	LOCK_CURRENT_CONN_IF_NOT_LOCKED(has_lock, conn_data);

	/* If flush is running, then wait for it complete. */
	if (conn_data->is_flushing) {
		/* Request flush_mutex for waiting for flush
		completed. */
		pthread_mutex_lock(&engine->flush_mutex);
		pthread_mutex_unlock(&engine->flush_mutex);
	}

	conn_data->in_use = true;

	crsr = conn_data->crsr;
	read_crsr = conn_data->read_crsr;

	if (lock_mode == IB_LOCK_TABLE_X) {
		if(!conn_data->crsr_trx) {
			conn_data->crsr_trx = ib_cb_trx_begin(
				engine->trx_level, true, false);
		} else {
			/* Write cursor transaction exists.
			   Reuse this transaction.*/
			if (ib_cb_trx_read_only(conn_data->crsr_trx)) {
				innodb_cb_trx_commit(
					conn_data->crsr_trx);
			}

			err = ib_cb_trx_start(conn_data->crsr_trx,
					      engine->trx_level,
					      true, false, NULL);
			assert(err == DB_SUCCESS);
		}

		err = innodb_api_begin(
			engine,
			meta_info->col_info[CONTAINER_DB].col_name,
			meta_info->col_info[CONTAINER_TABLE].col_name,
			conn_data, conn_data->crsr_trx,
			&conn_data->crsr, &conn_data->idx_crsr,
			lock_mode);

		if (err != DB_SUCCESS) {
			innodb_cb_cursor_close(
				conn_data->crsr);
			conn_data->crsr = NULL;
			innodb_cb_trx_commit(
				conn_data->crsr_trx);
			err = ib_cb_trx_release(conn_data->crsr_trx);
			assert(err == DB_SUCCESS);
			conn_data->crsr_trx = NULL;
			conn_data->in_use = false;
			UNLOCK_CURRENT_CONN_IF_NOT_LOCKED(
				has_lock, conn_data);
			return(NULL);
		}

		UNLOCK_CURRENT_CONN_IF_NOT_LOCKED(has_lock, conn_data);
		return(conn_data);
	}

	/* Write operation */
	if (conn_option == CONN_MODE_WRITE) {
		if (!crsr) {
			if (!conn_data->crsr_trx) {
				conn_data->crsr_trx = ib_cb_trx_begin(
					engine->trx_level, true, false);
				trx_updated = true;
			} else {
				if (ib_cb_trx_read_only(conn_data->crsr_trx)) {
					innodb_cb_trx_commit(
						conn_data->crsr_trx);
				}

				ib_cb_trx_start(conn_data->crsr_trx,
						engine->trx_level,
						true, false, NULL);
			}

			err = innodb_api_begin(
				engine,
				meta_info->col_info[CONTAINER_DB].col_name,
				meta_info->col_info[CONTAINER_TABLE].col_name,
				conn_data, conn_data->crsr_trx,
				&conn_data->crsr, &conn_data->idx_crsr,
				lock_mode);

			if (err != DB_SUCCESS) {
				innodb_cb_cursor_close(
					conn_data->crsr);
				conn_data->crsr = NULL;
				innodb_cb_trx_commit(
					conn_data->crsr_trx);
				err = ib_cb_trx_release(conn_data->crsr_trx);
				assert(err == DB_SUCCESS);
				conn_data->crsr_trx = NULL;
				conn_data->in_use = false;

				UNLOCK_CURRENT_CONN_IF_NOT_LOCKED(
					has_lock, conn_data);
				return(NULL);
			}

		} else if (!conn_data->crsr_trx) {

			/* There exists a cursor, just need update
			with a new transaction */
			conn_data->crsr_trx = ib_cb_trx_begin(
				engine->trx_level, true, false);

			innodb_cb_cursor_new_trx(crsr, conn_data->crsr_trx);
			trx_updated = true;

			err = innodb_cb_cursor_lock(engine, conn_data, crsr, lock_mode);

			if (err != DB_SUCCESS) {
				innodb_cb_cursor_close(
					conn_data->crsr);
				conn_data->crsr = NULL;
				innodb_cb_trx_commit(
					conn_data->crsr_trx);
				err = ib_cb_trx_release(conn_data->crsr_trx);
				assert(err == DB_SUCCESS);
				conn_data->crsr_trx = NULL;
				conn_data->in_use = false;
				UNLOCK_CURRENT_CONN_IF_NOT_LOCKED(
					has_lock, conn_data);
				return(NULL);
			}

			if (meta_index->srch_use_idx == META_USE_SECONDARY) {

				idx_crsr = conn_data->idx_crsr;
				innodb_cb_cursor_new_trx(
					idx_crsr, conn_data->crsr_trx);
				innodb_cb_cursor_lock(
					engine, conn_data, idx_crsr, lock_mode);
			}
		} else {

			if (ib_cb_trx_read_only(conn_data->crsr_trx)) {
				innodb_cb_trx_commit(
					conn_data->crsr_trx);
			}

			ib_cb_trx_start(conn_data->crsr_trx,
					engine->trx_level,
					true, false, NULL);
			ib_cb_cursor_stmt_begin(crsr);
			err = innodb_cb_cursor_lock(engine, conn_data, crsr, lock_mode);

			if (err != DB_SUCCESS) {
				innodb_cb_cursor_close(
					conn_data->crsr);
				conn_data->crsr = NULL;
				innodb_cb_trx_commit(
					conn_data->crsr_trx);
				err = ib_cb_trx_release(conn_data->crsr_trx);
				assert(err == DB_SUCCESS);
				conn_data->crsr_trx = NULL;
				conn_data->in_use = false;
				UNLOCK_CURRENT_CONN_IF_NOT_LOCKED(
					has_lock, conn_data);
				return(NULL);
			}
		}

		if (trx_updated) {
			if (conn_data->read_crsr) {
				innodb_cb_cursor_new_trx(
					conn_data->read_crsr,
					conn_data->crsr_trx);
			}

			if (conn_data->idx_read_crsr) {
				innodb_cb_cursor_new_trx(
					conn_data->idx_read_crsr,
					conn_data->crsr_trx);
			}
		}
	} else {
		assert(conn_option == CONN_MODE_READ);

		if (!read_crsr) {
			if (!conn_data->crsr_trx) {
				/* This is read operation, start a trx
				with "read_write" parameter set to false */
				conn_data->crsr_trx = ib_cb_trx_begin(
					engine->trx_level, false,
					engine->read_batch_size == 1);
				trx_updated = true;
			} else {
				ib_cb_trx_start(conn_data->crsr_trx,
						engine->trx_level,
						false,
						engine->read_batch_size == 1,
						NULL);
			}

			err = innodb_api_begin(
				engine,
				meta_info->col_info[CONTAINER_DB].col_name,
				meta_info->col_info[CONTAINER_TABLE].col_name,
				conn_data,
				conn_data->crsr_trx,
				&conn_data->read_crsr,
				&conn_data->idx_read_crsr,
				lock_mode);

			if (err != DB_SUCCESS) {
				innodb_cb_cursor_close(
					conn_data->read_crsr);
				innodb_cb_trx_commit(
					conn_data->crsr_trx);
				err = ib_cb_trx_release(conn_data->crsr_trx);
				assert(err == DB_SUCCESS);
				conn_data->crsr_trx = NULL;
				conn_data->read_crsr = NULL;
				conn_data->in_use = false;
				UNLOCK_CURRENT_CONN_IF_NOT_LOCKED(
					has_lock, conn_data);

				return(NULL);
			}

		} else if (!conn_data->crsr_trx) {
			/* This is read operation, start a trx
			with "read_write" parameter set to false */
			conn_data->crsr_trx = ib_cb_trx_begin(
				engine->trx_level, false,
				engine->read_batch_size == 1);

			trx_updated = true;

			innodb_cb_cursor_new_trx(
				conn_data->read_crsr,
				conn_data->crsr_trx);

			if (conn_data->crsr) {
				innodb_cb_cursor_new_trx(
					conn_data->crsr,
					conn_data->crsr_trx);
			}

			err = innodb_cb_cursor_lock(
				engine, conn_data, conn_data->read_crsr, lock_mode);

			if (err != DB_SUCCESS) {
				innodb_cb_cursor_close(
					conn_data->read_crsr);
				innodb_cb_trx_commit(
					conn_data->crsr_trx);
				err = ib_cb_trx_release(conn_data->crsr_trx);
				assert(err == DB_SUCCESS);
				conn_data->crsr_trx = NULL;
				conn_data->read_crsr = NULL;
				conn_data->in_use = false;
				UNLOCK_CURRENT_CONN_IF_NOT_LOCKED(
					has_lock, conn_data);

				return(NULL);
                        }

			if (meta_index->srch_use_idx == META_USE_SECONDARY) {
				ib_crsr_t idx_crsr = conn_data->idx_read_crsr;

				innodb_cb_cursor_new_trx(
					idx_crsr, conn_data->crsr_trx);
				innodb_cb_cursor_lock(
					engine, conn_data, idx_crsr, lock_mode);
			}
		} else {
			/* This is read operation, start a trx
			with "read_write" parameter set to false */
			ib_cb_trx_start(conn_data->crsr_trx,
					engine->trx_level,
					false,
					engine->read_batch_size == 1,
					NULL);

			ib_cb_cursor_stmt_begin(conn_data->read_crsr);

			err = innodb_cb_cursor_lock(
				engine, conn_data, conn_data->read_crsr, lock_mode);

			if (err != DB_SUCCESS) {
				innodb_cb_cursor_close(
					conn_data->read_crsr);
				innodb_cb_trx_commit(
					conn_data->crsr_trx);
				err = ib_cb_trx_release(conn_data->crsr_trx);
				assert(err == DB_SUCCESS);
				conn_data->crsr_trx = NULL;
				conn_data->read_crsr = NULL;
				conn_data->in_use = false;
				UNLOCK_CURRENT_CONN_IF_NOT_LOCKED(
					has_lock, conn_data);

				return(NULL);
                        }

			if (meta_index->srch_use_idx == META_USE_SECONDARY) {
				ib_crsr_t idx_crsr = conn_data->idx_read_crsr;
				ib_cb_cursor_stmt_begin(idx_crsr);
				innodb_cb_cursor_lock(
					engine, conn_data, idx_crsr, lock_mode);
			}
		}

		if (trx_updated) {
			if (conn_data->crsr) {
				innodb_cb_cursor_new_trx(
					conn_data->crsr,
					conn_data->crsr_trx);
			}

			if (conn_data->idx_crsr) {
				innodb_cb_cursor_new_trx(
					conn_data->idx_crsr,
					conn_data->crsr_trx);
			}
		}
	}

	UNLOCK_CURRENT_CONN_IF_NOT_LOCKED( has_lock, conn_data);

	return(conn_data);
}

/*** allocate ***/

/*******************************************************************//**
Allocate gets a struct item from the slab allocator, and fills in
everything but the value.  It seems like we can just pass this on to
the default engine; we'll intercept it later in store(). */
static
ENGINE_ERROR_CODE
innodb_allocate(
/*============*/
	ENGINE_HANDLE*	handle,		/*!< in: Engine handle */
	const void*	cookie,		/*!< in: connection cookie */
	item **		item,		/*!< out: item to allocate */
	const void*	key,		/*!< in: key */
	const size_t	nkey,		/*!< in: key length */
	const size_t	nbytes,		/*!< in: estimated value length */
	const int	flags,		/*!< in: flag */
	const rel_time_t exptime)	/*!< in: expiration time */
{
	size_t			len;
	struct innodb_engine*	innodb_eng = innodb_handle(handle);
	innodb_conn_data_t*	conn_data;
	hash_item*		it = NULL;

	ENGINE_ERROR_CODE	err_ret = ENGINE_SUCCESS;
	err_ret = check_container_for_map_switch(handle, cookie);
	if (err_ret != ENGINE_SUCCESS) {
		return err_ret;
	}

	conn_data = innodb_eng->server.cookie->get_engine_specific(cookie);

	if (!conn_data) {
		conn_data = innodb_conn_init(innodb_eng, cookie,
					     CONN_MODE_WRITE,
					     IB_LOCK_X, false, NULL);
		if (!conn_data) {
			return(ENGINE_TMPFAIL);
		}
	}

	conn_data->use_default_mem = false;
	len = sizeof(*it) + nkey + nbytes + sizeof(uint64_t);
	if (len > conn_data->cmd_buf_len) {
		free(conn_data->cmd_buf);
		conn_data->cmd_buf = malloc(len);
		conn_data->cmd_buf_len = len;
	}

	it = (hash_item*) conn_data->cmd_buf;

	it->next = it->prev = it->h_next = 0;
	it->refcount = 1;
	it->iflag = conn_data->conn_meta->cas_enabled ? ITEM_WITH_CAS : 0;
	it->nkey = nkey;
	it->nbytes = nbytes;
	it->flags = flags;
	it->slabs_clsid = 1;
	/* item_get_key() is a memcached code, here we cast away const return */
	memcpy((void*) hash_item_get_key(it), key, nkey);
	it->exptime = exptime;

	*item = it;
	conn_data->in_use = false;

	return(ENGINE_SUCCESS);
}
/*******************************************************************//**
Cleanup connections
@return number of connection cleaned */
static
ENGINE_ERROR_CODE
innodb_remove(
/*==========*/
	ENGINE_HANDLE*		handle,		/*!< in: Engine handle */
	const void*		cookie,		/*!< in: connection cookie */
	const void*		key,		/*!< in: key */
	const size_t		nkey,		/*!< in: key length */
	uint64_t		cas __attribute__((unused)),
						/*!< in: cas */
	uint16_t		vbucket __attribute__((unused)))
						/*!< in: bucket, used by default
						engine only */
{
	struct innodb_engine*	innodb_eng = innodb_handle(handle);
	ENGINE_ERROR_CODE	err_ret = ENGINE_SUCCESS;
	innodb_conn_data_t*	conn_data;

	err_ret = check_container_for_map_switch(handle, cookie);
	if (err_ret != ENGINE_SUCCESS) {
		return err_ret;
	}

	conn_data = innodb_conn_init(innodb_eng, cookie,
				     CONN_MODE_WRITE, IB_LOCK_X, false,
				     NULL);

	if (!conn_data) {
		return(ENGINE_TMPFAIL);
	}

	/* In the binary protocol there is such a thing as a CAS delete.
	This is the CAS check. If we will also be deleting from the database,
	there are two possibilities:
	  1: The CAS matches; perform the delete.
	  2: The CAS doesn't match; delete the item because it's stale.
	Therefore we skip the check altogether if(do_db_delete) */

	err_ret = innodb_api_delete(innodb_eng, conn_data, key, nkey);

	innodb_api_cursor_reset(innodb_eng, conn_data, CONN_OP_DELETE,
				err_ret == ENGINE_SUCCESS);

	return(err_ret);
}

/*******************************************************************//**
Switch the table mapping.
@return ENGINE_SUCCESS if successful, otherwise error code */
static
ENGINE_ERROR_CODE
innodb_switch_mapping(
/*==================*/
	ENGINE_HANDLE*		handle,		/*!< in: Engine handle */
	const void*		cookie,		/*!< in: connection cookie */
	const char*		name,		/*!< in: full name contains
						table map name, and possible
						key value */
	size_t			name_len)	/*!< in: name length,
						out with length excludes
						the table map name */
{
	struct innodb_engine*	innodb_eng = innodb_handle(handle);
	innodb_conn_data_t*	conn_data;
	char*			new_map_name;
	unsigned int		new_map_name_len = 0;
	meta_cfg_info_t*	new_meta_info;

	if (name == NULL) {
		return(ENGINE_KEY_ENOENT);
	}

	new_map_name = (char*) name;
	new_map_name_len = name_len;

	conn_data = innodb_eng->server.cookie->get_engine_specific(cookie);

	/* Check if we are getting the same configure setting as existing one */
	if (conn_data && conn_data->conn_meta
	    && (new_map_name_len
		== conn_data->conn_meta->col_info[CONTAINER_NAME].col_name_len)
	    && (strcmp(
		new_map_name,
		conn_data->conn_meta->col_info[CONTAINER_NAME].col_name) == 0)) {
		return(ENGINE_SUCCESS);
	}

	new_meta_info = innodb_config(
		new_map_name, new_map_name_len, &innodb_eng->meta_hash);

	if (!new_meta_info) {
		return(ENGINE_KEY_ENOENT);
	}

	/* Clean up the existing connection metadata if exists */
	if (conn_data) {
		innodb_conn_clean_data(conn_data, false, false);

		/* Point to the new metadata */
		conn_data->conn_meta = new_meta_info;
	}

	conn_data = innodb_conn_init(innodb_eng, cookie,
				     CONN_MODE_NONE, 0, false,
				     new_meta_info);

	assert(conn_data->conn_meta == new_meta_info);

	return(ENGINE_SUCCESS);
}

/*******************************************************************//**
check whether a table mapping switch is needed, if so, switch the table
mapping
@return ENGINE_SUCCESS if successful otherwise error code */
static
ENGINE_ERROR_CODE
check_container_for_map_switch(
/*==========================*/
	ENGINE_HANDLE*		handle,		/*!< in: Engine Handle */
	const void*		cookie)		/*!< in: connection cookie */
{
	ENGINE_ERROR_CODE	err_ret = ENGINE_SUCCESS;

	struct innodb_engine* innodb_eng = innodb_handle(handle);
	memcached_container_t* container = innodb_eng->server.cookie->get_container(cookie);

	const char* name = container->name;
	size_t name_len = strlen(name);

	err_ret = innodb_switch_mapping(handle, cookie, name, name_len);

	return(err_ret);
}

/*******************************************************************//**
Release the connection, free resource allocated in innodb_allocate */
static
void
innodb_clean_engine(
/*================*/
	ENGINE_HANDLE*		handle,		/*!< in: Engine handle */
	const void*		cookie __attribute__((unused)),
						/*!< in: connection cookie */
	void*			conn)		/*!< in: item to free */
{
	innodb_conn_data_t*	conn_data = (innodb_conn_data_t*)conn;
	struct innodb_engine*	engine = innodb_handle(handle);
	void*			orignal_thd;

	LOCK_CURRENT_CONN_IF_NOT_LOCKED(false, conn_data);
	if (conn_data->thd) {
		handler_thd_attach(conn_data->thd, &orignal_thd);
	}
	innodb_reset_conn(conn_data, true, true, engine->enable_binlog);
	innodb_conn_clean_data(conn_data, true, false);
	conn_data->is_stale = true;
	UNLOCK_CURRENT_CONN_IF_NOT_LOCKED(false, conn_data);
}

/*******************************************************************//**
Release the connection, free resource allocated in innodb_allocate */
static
void
innodb_release(
/*===========*/
	ENGINE_HANDLE*		handle,		/*!< in: Engine handle */
	const void*		cookie __attribute__((unused)),
						/*!< in: connection cookie */
	item*			item __attribute__((unused)))
						/*!< in: item to free */
{
	struct innodb_engine*	innodb_eng = innodb_handle(handle);
	innodb_conn_data_t*	conn_data;

	conn_data = innodb_eng->server.cookie->get_engine_specific(cookie);

	if (!conn_data) {
		return;
	}

	conn_data->result_in_use = false;

	return;
}

/* maximum number of characters that an 8 bytes integer can convert to */
#define	MAX_INT_CHAR_LEN	21

/*******************************************************************//**
Convert an bit int to string
@return length of string */
static
int
convert_to_char(
/*============*/
	char*		buf,		/*!< out: converted integer value */
	int		buf_len,	/*!< in: buffer len */
	void*		value,		/*!< in: int value */
	int		value_len,	/*!< in: int len */
	bool		is_unsigned)	/*!< in: whether it is unsigned */
{
	assert(buf && buf_len);

	if (value_len == 8) {
		if (is_unsigned) {
			uint64_t	int_val = *(uint64_t*)value;
			snprintf(buf, buf_len, "%" PRIu64, int_val);
		} else {
			int64_t		int_val = *(int64_t*)value;
			snprintf(buf, buf_len, "%" PRIi64, int_val);
		}
	} else if (value_len == 4) {
		if (is_unsigned) {
			uint32_t	int_val = *(uint32_t*)value;
			snprintf(buf, buf_len, "%" PRIu32, int_val);
		} else {
			int32_t		int_val = *(int32_t*)value;
			snprintf(buf, buf_len, "%" PRIi32, int_val);
		}
	} else if (value_len == 2) {
		if (is_unsigned) {
			uint16_t	int_val = *(uint16_t*)value;
			snprintf(buf, buf_len, "%" PRIu16, int_val);
		} else {
			int16_t		int_val = *(int16_t*)value;
			snprintf(buf, buf_len, "%" PRIi16, int_val);
		}
	} else if (value_len == 1) {
		if (is_unsigned) {
			uint8_t		int_val = *(uint8_t*)value;
			snprintf(buf, buf_len, "%" PRIu8, int_val);
		} else {
			int8_t		int_val = *(int8_t*)value;
			snprintf(buf, buf_len, "%" PRIi8, int_val);
		}
	}

	return(strlen(buf));
}


/*******************************************************************//**
Free value assocaited with key */
static
void
innodb_free_item(
/*=====================*/
	void* item)	/*!< in: Item to be freed */
{

	mci_item_t*	result = (mci_item_t*) item;
	if (result->extra_col_value) {
		for (int i = 0; i < result->n_extra_col; i++) {
			if(result->extra_col_value[i].allocated)
				free(result->extra_col_value[i].value_str);
			}
			free(result->extra_col_value);
			result->extra_col_value=NULL;
		}
	if (result->col_value[MCI_COL_VALUE].allocated) {
		free(result->col_value[MCI_COL_VALUE].value_str);
		result->col_value[MCI_COL_VALUE].allocated =
			false;
	}
}
/*******************************************************************//**
Support memcached "GET" command, fetch the value according to key
@return ENGINE_SUCCESS if successfully, otherwise error code */
static
ENGINE_ERROR_CODE
innodb_get(
/*=======*/
	ENGINE_HANDLE*		handle,		/*!< in: Engine Handle */
	const void*		cookie,		/*!< in: connection cookie */
	item**			item,		/*!< out: item to fill */
	const void*		key,		/*!< in: search key */
	const int		nkey,		/*!< in: key length */
	uint16_t		vbucket __attribute__((unused)))
						/*!< in: bucket, used by default
						engine only */
{
	struct innodb_engine*	innodb_eng = innodb_handle(handle);
	ib_crsr_t		crsr;
	ib_err_t		err = DB_SUCCESS;
	mci_item_t*		result = NULL;
	ENGINE_ERROR_CODE	err_ret = ENGINE_SUCCESS;
	innodb_conn_data_t*	conn_data = NULL;
	int			option_length;
	const char*		option_delimiter;
	size_t			key_len = nkey;
	int			lock_mode;

	/* Check if we need to switch table mapping */
	err_ret = check_container_for_map_switch(handle, cookie);

	/* If specified new table map does not exist, or table does not
	qualify for InnoDB memcached, return error */
	if (err_ret != ENGINE_SUCCESS) {
		goto err_exit;
	}

	lock_mode = (innodb_eng->trx_level == IB_TRX_SERIALIZABLE
		     && innodb_eng->read_batch_size == 1)
			? IB_LOCK_S
			: IB_LOCK_NONE;

	conn_data = innodb_conn_init(innodb_eng, cookie, CONN_MODE_READ,
				     lock_mode, false, NULL);

	if (!conn_data) {
		return(ENGINE_TMPFAIL);
	}

	result = (mci_item_t*)(conn_data->result);

	err = innodb_api_search(conn_data, &crsr, key + nkey - key_len,
				key_len, result, NULL, true);

	if (err != DB_SUCCESS) {
		err_ret = ENGINE_KEY_ENOENT;
		goto func_exit;
	}

	result->col_value[MCI_COL_KEY].value_str = (char*)key;
	result->col_value[MCI_COL_KEY].value_len = nkey;

	/* Only if expiration field is enabled, and the value is not zero,
	we will check whether the item is expired */
	if (result->col_value[MCI_COL_EXP].is_valid
	    && result->col_value[MCI_COL_EXP].value_int) {
		uint64_t time;
		time = mci_get_time();
		if (time > result->col_value[MCI_COL_EXP].value_int) {
			innodb_free_item(result);
			err_ret = ENGINE_KEY_ENOENT;
			goto func_exit;
		}
	}

	if (result->extra_col_value) {
		int		i;
		char*		c_value;
		char*		value_end;
		unsigned int	total_len = 0;
		char		int_buf[MAX_INT_CHAR_LEN];

		option_delimiter = conn_data->conn_meta->col_info[CONTAINER_SEP].col_name;
		option_length = conn_data->conn_meta->col_info[CONTAINER_SEP].col_name_len;

		assert(option_length > 0 && option_delimiter);

		for (i = 0; i < result->n_extra_col; i++) {
			mci_column_t*   mci_item = &result->extra_col_value[i];

			if (mci_item->value_len == 0) {
				total_len += option_length;
				continue;
			}

			if (!mci_item->is_str) {
				memset(int_buf, 0, sizeof int_buf);
				assert(!mci_item->value_str);

				total_len += convert_to_char(
					int_buf, sizeof int_buf,
					&mci_item->value_int,
					mci_item->value_len,
					mci_item->is_unsigned);
			} else {
				total_len += result->extra_col_value[i].value_len;
			}

			total_len += option_length;
		}

		/* No need to add the last separator */
		total_len -= option_length;

		if (total_len > conn_data->mul_col_buf_len) {
			if (conn_data->mul_col_buf) {
				free(conn_data->mul_col_buf);
			}

			conn_data->mul_col_buf = malloc(total_len + 1);
			conn_data->mul_col_buf_len = total_len;
		}

		c_value = conn_data->mul_col_buf;
		value_end = conn_data->mul_col_buf + total_len;

		for (i = 0; i < result->n_extra_col; i++) {
			mci_column_t*   col_value;

			col_value = &result->extra_col_value[i];

			if (col_value->value_len != 0) {
				if (!col_value->is_str) {
					int	int_len;
					memset(int_buf, 0, sizeof int_buf);

					int_len = convert_to_char(
						int_buf,
						sizeof int_buf,
						&col_value->value_int,
						col_value->value_len,
						col_value->is_unsigned);
					memcpy(c_value, int_buf, int_len);
					c_value += int_len;
				} else {
					memcpy(c_value,
					       col_value->value_str,
					       col_value->value_len);
					c_value += col_value->value_len;
				}
			}

			if (i < result->n_extra_col - 1 ) {
				memcpy(c_value, option_delimiter, option_length);
				c_value += option_length;
			}

			assert(c_value <= value_end);

			if (col_value->allocated) {
				free(col_value->value_str);
			}
		}

		result->col_value[MCI_COL_VALUE].value_str = conn_data->mul_col_buf;
		result->col_value[MCI_COL_VALUE].value_len = total_len;
		((char*)result->col_value[MCI_COL_VALUE].value_str)[total_len] = 0;

		free(result->extra_col_value);
	} else if (!result->col_value[MCI_COL_VALUE].is_str
		&& result->col_value[MCI_COL_VALUE].value_len != 0) {
		unsigned int	int_len;
		char		int_buf[MAX_INT_CHAR_LEN];

		int_len = convert_to_char(
			int_buf, sizeof int_buf,
			&result->col_value[MCI_COL_VALUE].value_int,
			result->col_value[MCI_COL_VALUE].value_len,
			result->col_value[MCI_COL_VALUE].is_unsigned);

		if (int_len > conn_data->mul_col_buf_len) {
			if (conn_data->mul_col_buf) {
				free(conn_data->mul_col_buf);
			}

			conn_data->mul_col_buf = malloc(int_len + 1);
			conn_data->mul_col_buf_len = int_len;
		}

		memcpy(conn_data->mul_col_buf, int_buf, int_len);
		result->col_value[MCI_COL_VALUE].value_str =
			 conn_data->mul_col_buf;

		result->col_value[MCI_COL_VALUE].value_len = int_len;
	}

        *item = result;

func_exit:

	innodb_api_cursor_reset(innodb_eng, conn_data,
				CONN_OP_READ, true);

err_exit:

	/* If error return, memcached will not call InnoDB Memcached's
	callback function "innodb_release" to reset the result_in_use
	value. So we reset it here */
	if (err_ret != ENGINE_SUCCESS && conn_data) {
		conn_data->result_in_use = false;
	}
	return(err_ret);
}

/*******************************************************************//**
Get statistics info
@return ENGINE_SUCCESS if successfully, otherwise error code */
static
ENGINE_ERROR_CODE
innodb_get_stats(
/*=============*/
	ENGINE_HANDLE*		handle __attribute__((unused)),		/*!< in: Engine Handle */
	const void*		cookie __attribute__((unused)),		/*!< in: connection cookie */
	const char*		stat_key __attribute__((unused)),	/*!< in: statistics key */
	int			nkey __attribute__((unused)),		/*!< in: key length */
	ADD_STAT		add_stat __attribute__((unused)))	/*!< out: stats to fill */
{
	return(ENGINE_SUCCESS);
}

/*******************************************************************//**
reset statistics
@return ENGINE_SUCCESS if successfully, otherwise error code */
static
void
innodb_reset_stats(
/*===============*/
	ENGINE_HANDLE*		handle __attribute__((unused)),		/*!< in: Engine Handle */
	const void*		cookie __attribute__((unused)))		/*!< in: connection cookie */
{
}

/*******************************************************************//**
API interface for memcached's "SET", "ADD", "REPLACE", "APPEND"
"PREPENT" and "CAS" commands
@return ENGINE_SUCCESS if successfully, otherwise error code */
static
ENGINE_ERROR_CODE
innodb_store(
/*=========*/
	ENGINE_HANDLE*		handle,		/*!< in: Engine Handle */
	const void*		cookie,		/*!< in: connection cookie */
	item*			item,		/*!< out: result to fill */
	uint64_t*		cas,		/*!< in: cas value */
	ENGINE_STORE_OPERATION	op,		/*!< in: type of operation */
	uint16_t		vbucket	__attribute__((unused)))
						/*!< in: bucket, used by default
						engine only */
{
	struct innodb_engine*	innodb_eng = innodb_handle(handle);
	uint16_t		len = hash_item_get_key_len(item);
	char*			value = hash_item_get_key(item);
	uint64_t		exptime = hash_item_get_exp(item);
	uint64_t		flags = hash_item_get_flag(item);
	ENGINE_ERROR_CODE	result;
	uint64_t		input_cas;
	innodb_conn_data_t*	conn_data;
	uint32_t		val_len = ((hash_item*)item)->nbytes;
	size_t			key_len = len;
	ENGINE_ERROR_CODE	err_ret = ENGINE_SUCCESS;

	err_ret = check_container_for_map_switch(handle, cookie);

	if (err_ret != ENGINE_SUCCESS) {
		return(err_ret);
	}

	/* If no key is provided, return here */
	if (key_len <= 0) {
		return(ENGINE_NOT_STORED);
	}

	conn_data = innodb_conn_init(innodb_eng, cookie, CONN_MODE_WRITE,
				     IB_LOCK_X, false, NULL);

	if (!conn_data) {
		return(ENGINE_NOT_STORED);
	}

	input_cas = hash_item_get_cas(item);

	result = innodb_api_store(innodb_eng, conn_data, value + len - key_len,
				  key_len, val_len, exptime, cas, input_cas,
				  flags, op);

	innodb_api_cursor_reset(innodb_eng, conn_data, CONN_OP_WRITE,
				result == ENGINE_SUCCESS);
	return(result);
}

/*******************************************************************//**
Support memcached "INCR" and "DECR" command, add or subtract a "delta"
value from an integer key value
@return ENGINE_SUCCESS if successfully, otherwise error code */
static
ENGINE_ERROR_CODE
innodb_arithmetic(
/*==============*/
	ENGINE_HANDLE*	handle,		/*!< in: Engine Handle */
	const void*	cookie,		/*!< in: connection cookie */
	const void*	key,		/*!< in: key for the value to add */
	const int	nkey,		/*!< in: key length */
	const bool	increment,	/*!< in: whether to increment
					or decrement */
	const bool	create,		/*!< in: whether to create the key
					value pair if can't find */
	const uint64_t	delta,		/*!< in: value to add/substract */
	const uint64_t	initial,	/*!< in: initial */
	const rel_time_t exptime,	/*!< in: expiration time */
	uint64_t*	cas,		/*!< out: new cas value */
	uint64_t*	result,		/*!< out: result value */
	uint16_t	vbucket __attribute__((unused)),	/*!< in: bucket, used by default
								engine only */
	char*		result_str)	/*!< out: result value as string */
{
	struct innodb_engine*	innodb_eng = innodb_handle(handle);
	innodb_conn_data_t*	conn_data;
	ENGINE_ERROR_CODE	err_ret;

	err_ret = check_container_for_map_switch(handle, cookie);
	if (err_ret != ENGINE_SUCCESS) {
		return err_ret;
	}

	conn_data = innodb_conn_init(innodb_eng, cookie, CONN_MODE_WRITE,
				     IB_LOCK_X, false, NULL);

	if (!conn_data) {
		return(ENGINE_NOT_STORED);
	}

	err_ret = innodb_api_arithmetic(innodb_eng, conn_data, key, nkey,
					delta, increment, cas, exptime,
					create, initial, result, result_str);

	innodb_api_cursor_reset(innodb_eng, conn_data, CONN_OP_WRITE,
				true);

	return(err_ret);
}

/*******************************************************************//**
Cleanup idle connections if "clear_all" is false, and clean up all
connections if "clear_all" is true.
@return number of connection cleaned */
static
bool
innodb_flush_sync_conn(
/*===================*/
	innodb_engine_t*	engine,		/*!< in/out: InnoDB memcached
						engine */
	const void*		cookie,		/*!< in: connection cookie */
	bool			flush_flag)	/*!< in: flush is running or not */
{
	innodb_conn_data_t*	conn_data = NULL;
	innodb_conn_data_t*	curr_conn_data;
	bool			ret = true;

	curr_conn_data = engine->server.cookie->get_engine_specific(cookie);
	assert(curr_conn_data);

	conn_data = UT_LIST_GET_FIRST(engine->conn_data);

	while (conn_data) {
		if (conn_data != curr_conn_data && (!conn_data->is_stale)) {
			if (conn_data->thd) {
				handler_thd_attach(conn_data->thd, NULL);
			}
			LOCK_CURRENT_CONN_IF_NOT_LOCKED(false, conn_data);
			if (flush_flag == false) {
				conn_data->is_flushing = flush_flag;
				UNLOCK_CURRENT_CONN_IF_NOT_LOCKED(false, conn_data);
				conn_data = UT_LIST_GET_NEXT(conn_list, conn_data);
				continue;
			}
			if (!conn_data->in_use) {
				/* Set flushing flag to conn_data for preventing
				it is get by other request.  */
				conn_data->is_flushing = flush_flag;
				UNLOCK_CURRENT_CONN_IF_NOT_LOCKED(false, conn_data);
			} else {
				ret = false;
				UNLOCK_CURRENT_CONN_IF_NOT_LOCKED(false, conn_data);
				break;
			}
		}
		conn_data = UT_LIST_GET_NEXT(conn_list, conn_data);
	}

	if (curr_conn_data->thd) {
		handler_thd_attach(curr_conn_data->thd, NULL);
	}

	return(ret);
}

/*******************************************************************//**
Support memcached "FLUSH_ALL" command, clean up storage (trunate InnoDB Table)
@return ENGINE_SUCCESS if successfully, otherwise error code */
static
ENGINE_ERROR_CODE
innodb_flush(
/*=========*/
	ENGINE_HANDLE*	handle,		/*!< in: Engine Handle */
	const void*	cookie,		/*!< in: connection cookie */
	time_t		when __attribute__((unused)))		/*!< in: when to flush, not used by
								InnoDB */
{
	struct innodb_engine*	innodb_eng = innodb_handle(handle);
	ib_err_t		ib_err = DB_SUCCESS;
	innodb_conn_data_t*	conn_data;

	ENGINE_ERROR_CODE	err_ret = ENGINE_SUCCESS;
	err_ret = check_container_for_map_switch(handle, cookie);
	if (err_ret != ENGINE_SUCCESS) {
		return err_ret;
	}

	/* Lock the whole engine, so no other connection can start
	new opeartion */
        pthread_mutex_lock(&innodb_eng->conn_mutex);

	/* Lock the flush_mutex for blocking other DMLs. */
	pthread_mutex_lock(&innodb_eng->flush_mutex);

	conn_data = innodb_eng->server.cookie->get_engine_specific(cookie);

	if (conn_data) {
		/* Commit any work on this connection */
		innodb_api_cursor_reset(innodb_eng, conn_data,
					CONN_OP_FLUSH, true);
	}

        conn_data = innodb_conn_init(innodb_eng, cookie, CONN_MODE_WRITE,
				     IB_LOCK_TABLE_X, true, NULL);

	if (!conn_data) {
		pthread_mutex_unlock(&innodb_eng->flush_mutex);
		pthread_mutex_unlock(&innodb_eng->conn_mutex);
		return(ENGINE_TMPFAIL);
	}

	/* Commit any previous work on this connection */
	innodb_api_cursor_reset(innodb_eng, conn_data, CONN_OP_FLUSH, true);

	if (!innodb_flush_sync_conn(innodb_eng, cookie, true)) {
		pthread_mutex_unlock(&innodb_eng->flush_mutex);
		pthread_mutex_unlock(&innodb_eng->conn_mutex);
		innodb_flush_sync_conn(innodb_eng, cookie, false);
		return(ENGINE_TMPFAIL);
	}

	ib_err = innodb_api_flush(innodb_eng, conn_data,
				  conn_data->conn_meta->col_info[CONTAINER_DB].col_name,
				  conn_data->conn_meta->col_info[CONTAINER_TABLE].col_name);

	/* Commit work and release the MDL table. */
	innodb_api_cursor_reset(innodb_eng, conn_data, CONN_OP_FLUSH, true);
	innodb_conn_clean_data(conn_data, false, false);

        pthread_mutex_unlock(&innodb_eng->flush_mutex);
        pthread_mutex_unlock(&innodb_eng->conn_mutex);

	innodb_flush_sync_conn(innodb_eng, cookie, false);

	return((ib_err == DB_SUCCESS) ? ENGINE_SUCCESS : ENGINE_TMPFAIL);
}

/*******************************************************************//**
Deal with unknown command. Currently not used
@return ENGINE_SUCCESS if successfully processed, otherwise error code */
static
ENGINE_ERROR_CODE
innodb_unknown_command(
/*===================*/
	ENGINE_HANDLE*	handle __attribute__((unused)),		/*!< in: Engine Handle */
	const void*	cookie __attribute__((unused)),		/*!< in: connection cookie */
	protocol_binary_request_header *request __attribute__((unused)), /*!< in: request */
	ADD_RESPONSE	response __attribute__((unused)))	/*!< out: respondse */
{
	return(ENGINE_FAILED);
}

/*******************************************************************//**
TODO
@return void */
static
void
innodb_item_set_cas(
/*===================*/
	ENGINE_HANDLE*	handle __attribute__((unused)),		/*!< in: Engine Handle */
	const void*	cookie __attribute__((unused)),		/*!< in: connection cookie */
	item*		item,		/*!< in: item in question */
	uint64_t cas)			/*!< in: CAS */
{
	hash_item_set_cas(item, cas);
}

/*******************************************************************//**
Callback functions used by Memcached's process_command() function
to get the result key/value information
@return true if info fetched */
static
bool
innodb_get_item_info(
/*=================*/
	ENGINE_HANDLE*		handle,
						/*!< in: Engine Handle */
	const void*		cookie,
						/*!< in: connection cookie */
	const item*		item,		/*!< in: item in question */
	item_info*		item_info)	/*!< out: item info got */
{
	struct innodb_engine* innodb_eng = innodb_handle(handle);
	innodb_conn_data_t*     conn_data;

	ENGINE_ERROR_CODE	err_ret = ENGINE_SUCCESS;
	err_ret = check_container_for_map_switch(handle, cookie);
	if (err_ret != ENGINE_SUCCESS) {
		return err_ret;
	}

	conn_data = innodb_eng->server.cookie->get_engine_specific(cookie);

	if (!conn_data || !conn_data->result_in_use) {
		hash_item*      it;

		if (item_info->nvalue < 1) {
			return(false);
		}

		/* Use a hash item */
		it = (hash_item*) item;
		item_info->cas = hash_item_get_cas(it);
		item_info->exptime = it->exptime;
		item_info->nbytes = it->nbytes;
		item_info->flags = it->flags;
		item_info->clsid = it->slabs_clsid;
		item_info->nkey = it->nkey;
		item_info->nvalue = 1;
		item_info->key = hash_item_get_key(it);
		item_info->value[0].iov_base = hash_item_get_data(it);
		item_info->value[0].iov_len = it->nbytes;
	} else {
		mci_item_t*	it;

		if (item_info->nvalue < 1) {
			return(false);
		}

		/* Use a hash item */
		it = (mci_item_t*) item;
		if (it->col_value[MCI_COL_CAS].is_valid) {
			item_info->cas = it->col_value[MCI_COL_CAS].value_int;
		} else {
			item_info->cas = 0;
		}

		if (it->col_value[MCI_COL_EXP].is_valid) {
			item_info->exptime = it->col_value[MCI_COL_EXP].value_int;
		} else {
			item_info->exptime = 0;
		}

		item_info->nbytes = it->col_value[MCI_COL_VALUE].value_len;

		if (it->col_value[MCI_COL_FLAG].is_valid) {
			item_info->flags = ntohl(
				it->col_value[MCI_COL_FLAG].value_int);
		} else {
			item_info->flags = 0;
		}

		item_info->clsid = 1;

		item_info->nkey = it->col_value[MCI_COL_KEY].value_len;

		item_info->nvalue = 1;

		item_info->key = it->col_value[MCI_COL_KEY].value_str;

		item_info->value[0].iov_base = it->col_value[
					       MCI_COL_VALUE].value_str;;

		item_info->value[0].iov_len = it->col_value[
						MCI_COL_VALUE].value_len;

	}

	return(true);
}
