/*****************************************************************************

Copyright (C) 2013, 2014, Fusion-io. All Rights Reserved.
Copyright (C) 2013, 2014, SkySQL Ab. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA

*****************************************************************************/

/******************************************************************//**
@file buf/buf0mtflu.cc
Multi-threaded flush method implementation

Created  06/11/2013 Dhananjoy Das DDas@fusionio.com
Modified 12/12/2013 Jan Lindström jan.lindstrom@skysql.com
Modified 03/02/2014 Dhananjoy Das DDas@fusionio.com
Modified 06/02/2014 Jan Lindström jan.lindstrom@skysql.com
***********************************************************************/

#include "buf0buf.h"
#include "buf0flu.h"
#include "buf0mtflu.h"
#include "buf0checksum.h"
#include "srv0start.h"
#include "srv0srv.h"
#include "page0zip.h"
#include "ut0byte.h"
#include "ut0lst.h"
#include "page0page.h"
#include "fil0fil.h"
#include "buf0lru.h"
#include "buf0rea.h"
#include "ibuf0ibuf.h"
#include "log0log.h"
#include "os0file.h"
#include "os0sync.h"
#include "trx0sys.h"
#include "srv0mon.h"
#include "mysql/plugin.h"
#include "mysql/service_thd_wait.h"
#include "fil0pagecompress.h"

#define	MT_COMP_WATER_MARK	50

/* Work item status */
typedef enum wrk_status {
	WRK_ITEM_SET=0,		/*!< Work item is set */
	WRK_ITEM_START=1,	/*!< Processing of work item has started */
	WRK_ITEM_DONE=2,	/*!< Processing is done usually set to
				SUCCESS/FAILED */
	WRK_ITEM_SUCCESS=2,	/*!< Work item successfully processed */
	WRK_ITEM_FAILED=3,	/*!< Work item process failed */
	WRK_ITEM_EXIT=4,	/*!< Exiting */
	WRK_ITEM_STATUS_UNDEFINED
} wrk_status_t;

/* Work item task type */
typedef enum mt_wrk_tsk {
	MT_WRK_NONE=0,		/*!< Exit queue-wait */
	MT_WRK_WRITE=1,		/*!< Flush operation */
	MT_WRK_READ=2,		/*!< Read operation  */
	MT_WRK_UNDEFINED
} mt_wrk_tsk_t;

/* Work thread status */
typedef enum wthr_status {
	WTHR_NOT_INIT=0,	/*!< Work thread not initialized */
	WTHR_INITIALIZED=1,	/*!< Work thread initialized */
	WTHR_SIG_WAITING=2,	/*!< Work thread wating signal */
	WTHR_RUNNING=3,		/*!< Work thread running */
	WTHR_NO_WORK=4,		/*!< Work thread has no work */
	WTHR_KILL_IT=5,		/*!< Work thread should exit */
	WTHR_STATUS_UNDEFINED
} wthr_status_t;

/* Write work task */
typedef struct wr_tsk {
	buf_pool_t	*buf_pool;	/*!< buffer-pool instance */
	enum buf_flush	flush_type;	/*!< flush-type for buffer-pool
					flush operation */
	ulint		min;		/*!< minimum number of pages
					requested to be flushed */
	lsn_t		lsn_limit;	/*!< lsn limit for the buffer-pool
					flush operation */
} wr_tsk_t;

/* Read work task */
typedef struct rd_tsk {
	buf_pool_t	*page_pool;	/*!< list of pages to decompress; */
} rd_tsk_t;

/* Work item */
typedef struct wrk_itm
{
	mt_wrk_tsk_t	tsk;		/*!< Task type. Based on task-type
					one of the entries wr_tsk/rd_tsk
					will be used */
	wr_tsk_t	wr;		/*!< Flush page list */
	rd_tsk_t	rd;		/*!< Decompress page list */
        ulint		n_flushed; 	/*!< Flushed pages count  */
 	os_thread_id_t	id_usr;		/*!< Thread-id currently working */
    	wrk_status_t    wi_status;	/*!< Work item status */
 	struct wrk_itm	*next;		/*!< Next work item */
} wrk_t;

/* Thread syncronization data */
typedef struct thread_sync
{
	ulint           n_threads;	/*!< Number of threads */
	os_thread_id_t	wthread_id;	/*!< Identifier */
	os_thread_t 	wthread;	/*!< Thread id */
	ib_wqueue_t	*wq;		/*!< Work Queue */
	ib_wqueue_t     *wr_cq;		/*!< Write Completion Queue */
	ib_wqueue_t     *rd_cq;		/*!< Read Completion Queue */
	wthr_status_t   wt_status;	/*!< Worker thread status */
	mem_heap_t*     wheap;		/*!< Work heap where memory
					is allocated */
	wrk_t*          work_item;      /*!< Array of work-items that are
					individually accessed by multiple
					threads. Items are accessed in a
					thread safe manner.*/
} thread_sync_t;

/* TODO: REALLY NEEDED ? */
static int		mtflush_work_initialized = -1;
static os_fast_mutex_t	mtflush_mtx;
static thread_sync_t*   mtflush_ctx=NULL;

/******************************************************************//**
Initialize work items. */
static
void
mtflu_setup_work_items(
/*===================*/
	wrk_t*  work_items,     /*!< inout: Work items */
	ulint	n_items)	/*!< in: Number of work items */
{
	ulint i;
	for(i=0; i<n_items; i++) {
		work_items[i].rd.page_pool = NULL;
		work_items[i].wr.buf_pool = NULL;
		work_items[i].n_flushed = 0;
		work_items[i].id_usr = -1;
		work_items[i].wi_status = WRK_ITEM_STATUS_UNDEFINED;
		work_items[i].next = &work_items[(i+1)%n_items];
	}
	/* last node should be the tail */
	work_items[n_items-1].next = NULL;
}

/******************************************************************//**
Set multi-threaded flush work initialized. */
static inline
void
buf_mtflu_work_init(void)
/*=====================*/
{
	mtflush_work_initialized = 1;
}

/******************************************************************//**
Return true if multi-threaded flush is initialized
@return true if initialized */
bool
buf_mtflu_init_done(void)
/*=====================*/
{
	return(mtflush_work_initialized == 1);
}

/******************************************************************//**
Fush buffer pool instance.
@return number of flushed pages, or 0 if error happened
*/
static
ulint
buf_mtflu_flush_pool_instance(
/*==========================*/
	wrk_t	*work_item)	/*!< inout: work item to be flushed */
{
	ut_a(work_item != NULL);
	ut_a(work_item->wr.buf_pool != NULL);

	if (!buf_flush_start(work_item->wr.buf_pool, work_item->wr.flush_type)) {
		/* We have two choices here. If lsn_limit was
		specified then skipping an instance of buffer
		pool means we cannot guarantee that all pages
		up to lsn_limit has been flushed. We can
		return right now with failure or we can try
		to flush remaining buffer pools up to the
		lsn_limit. We attempt to flush other buffer
		pools based on the assumption that it will
		help in the retry which will follow the
		failure. */
#ifdef UNIV_DEBUG
		fprintf(stderr, "flush start failed.\n");
#endif
		return 0;
	}


    	if (work_item->wr.flush_type == BUF_FLUSH_LRU) {
        	/* srv_LRU_scan_depth can be arbitrarily large value.
        	 * We cap it with current LRU size.
        	 */
        	buf_pool_mutex_enter(work_item->wr.buf_pool);
        	work_item->wr.min = UT_LIST_GET_LEN(work_item->wr.buf_pool->LRU);
        	buf_pool_mutex_exit(work_item->wr.buf_pool);
        	work_item->wr.min = ut_min(srv_LRU_scan_depth,work_item->wr.min);
    	}

	work_item->n_flushed = buf_flush_batch(work_item->wr.buf_pool,
                                    		work_item->wr.flush_type,
                                    		work_item->wr.min,
						work_item->wr.lsn_limit);


	buf_flush_end(work_item->wr.buf_pool, work_item->wr.flush_type);
	buf_flush_common(work_item->wr.flush_type, work_item->n_flushed);

	return work_item->n_flushed;
}

#ifdef UNIV_DEBUG
/******************************************************************//**
Print flush statistics of work items.
*/
static
void
mtflu_print_thread_stat(
/*====================*/
	wrk_t* work_item)	/*!< in: Work items */
{
	ulint stat_tot=0;
	ulint i=0;

 	for(i=0; i< MTFLUSH_MAX_WORKER; i++) {
 		stat_tot+=work_item[i].n_flushed;

 		fprintf(stderr, "MTFLUSH: Thread[%lu] stat [%lu]\n",
			work_item[i].id_usr,
 			work_item[i].n_flushed);

		if (work_item[i].next == NULL) {
			break; /* No more filled work items */
		}
 	}
 	fprintf(stderr, "MTFLUSH: Stat-Total:%lu\n", stat_tot);
}
#endif /* UNIV_DEBUG */

/******************************************************************//**
Worker function to wait for work items and processing them and
sending reply back.
*/
static
void
mtflush_service_io(
/*===============*/
	thread_sync_t*	mtflush_io)	/*!< inout: multi-threaded flush
					syncronization data */
{
	wrk_t		*work_item = NULL;
	ulint		n_flushed=0;
	ib_time_t	max_wait_usecs = 50000;

   	mtflush_io->wt_status = WTHR_SIG_WAITING;
	work_item = (wrk_t *)ib_wqueue_timedwait(mtflush_io->wq, max_wait_usecs);

	if (work_item) {
		mtflush_io->wt_status = WTHR_RUNNING;
	} else {
		/* Because of timeout this thread did not get any work */
		mtflush_io->wt_status = WTHR_NO_WORK;
		return;
	}

	work_item->id_usr = os_thread_get_curr_id();

	/*  This works as a producer/consumer model, where in tasks are
         *  inserted into the work-queue (wq) and completions are based
         *  on the type of operations performed and as a result the WRITE/
         *  compression/flush operation completions get posted to wr_cq.
         *  And READ/decompress operations completions get posted to rd_cq.
         *  in future we may have others.
	*/

	switch(work_item->tsk) {
	case MT_WRK_NONE:
		ut_a(work_item->wi_status == WRK_ITEM_EXIT);
		work_item->wi_status = WRK_ITEM_EXIT;
		ib_wqueue_add(mtflush_io->wr_cq, work_item, mtflush_io->wheap);
		mtflush_io->wt_status = WTHR_KILL_IT;
        return;

	case MT_WRK_WRITE:
		work_item->wi_status = WRK_ITEM_START;
		/* Process work item */
		if (0 == (n_flushed = buf_mtflu_flush_pool_instance(work_item))) {
#ifdef UNIV_DEBUG
			fprintf(stderr, "No pages flushed\n");
#endif
			work_item->wi_status = WRK_ITEM_FAILED;
		}
		work_item->wi_status = WRK_ITEM_SUCCESS;
		ib_wqueue_add(mtflush_io->wr_cq, work_item, mtflush_io->wheap);
		break;

	case MT_WRK_READ:
		/* Need to also handle the read case */
		/* TODO: ? */
		ut_a(0);
		/* completed task get added to rd_cq */
		/* work_item->wi_status = WRK_ITEM_SUCCESS;
		ib_wqueue_add(mtflush_io->rd_cq, work_item, mtflush_io->wheap);*/
		break;

	default:
		/* None other than Write/Read handling planned */
		ut_a(0);
	}

	mtflush_io->wt_status = WTHR_NO_WORK;
}

/******************************************************************//**
Thead used to flush dirty pages when multi-threaded flush is
used.
@return a dummy parameter*/
extern "C" UNIV_INTERN
os_thread_ret_t
DECLARE_THREAD(mtflush_io_thread)(
/*==============================*/
	void * arg)
{
	thread_sync_t *mtflush_io = ((thread_sync_t *)arg);
#ifdef UNIV_DEBUG
	ib_uint64_t   stat_universal_num_processed = 0;
	ib_uint64_t   stat_cycle_num_processed = 0;
	wrk_t*	      work_item = mtflush_io[0].work_item;
	ulint i;
#endif

	while (TRUE) {
		mtflush_service_io(mtflush_io);

#ifdef UNIV_DEBUG
		for(i=0; i < MTFLUSH_MAX_WORKER; i++) {
			stat_cycle_num_processed+= work_item[i].n_flushed;
		}

		stat_universal_num_processed+=stat_cycle_num_processed;
		stat_cycle_num_processed = 0;
		fprintf(stderr, "MTFLUSH_IO_THREAD: total %lu cycle %lu\n",
			stat_universal_num_processed,
			stat_cycle_num_processed);
		mtflu_print_thread_stat(work_item);
#endif
		if (mtflush_io->wt_status == WTHR_KILL_IT) {
			break;
		}
	}

	os_thread_exit(NULL);
	OS_THREAD_DUMMY_RETURN;
}

/******************************************************************//**
Add exit work item to work queue to signal multi-threded flush
threads that they should exit.
*/
void
buf_mtflu_io_thread_exit(void)
/*==========================*/
{
	long i;
	thread_sync_t* mtflush_io = mtflush_ctx;

	ut_a(mtflush_io != NULL);

	/* Confirm if the io-thread KILL is in progress, bailout */
	if (mtflush_io->wt_status == WTHR_KILL_IT) {
		return;
	}

	fprintf(stderr, "signal mtflush_io_threads to exit [%lu]\n",
		srv_buf_pool_instances);

	/* Send one exit work item/thread */
	for (i=0; i < srv_mtflush_threads; i++) {
		mtflush_io->work_item[i].wr.buf_pool = NULL;
		mtflush_io->work_item[i].rd.page_pool = NULL;
		mtflush_io->work_item[i].tsk = MT_WRK_NONE;
		mtflush_io->work_item[i].wi_status = WRK_ITEM_EXIT;

		ib_wqueue_add(mtflush_io->wq,
			(void *)&(mtflush_io->work_item[i]),
			mtflush_io->wheap);
	}

	/* Wait until all work items on a work queue are processed */
	while(!ib_wqueue_is_empty(mtflush_io->wq)) {
		/* Wait */
		os_thread_sleep(500000);
	}

	ut_a(ib_wqueue_is_empty(mtflush_io->wq));

	/* Collect all work done items */
	for (i=0; i < srv_mtflush_threads;) {
		wrk_t* work_item;

		work_item = (wrk_t *)ib_wqueue_timedwait(mtflush_io->wr_cq, 50000);

		/* If we receive reply to work item and it's status is exit,
		thead has processed this message and existed */
		if (work_item && work_item->wi_status == WRK_ITEM_EXIT) {
			i++;
		}
	}

	/* Wait about 1/2 sec to allow threads really exit */
	os_thread_sleep(50000);

	ut_a(ib_wqueue_is_empty(mtflush_io->wq));
	ut_a(ib_wqueue_is_empty(mtflush_io->wr_cq));
	ut_a(ib_wqueue_is_empty(mtflush_io->rd_cq));

	/* Free all queues */
	ib_wqueue_free(mtflush_io->wq);
	ib_wqueue_free(mtflush_io->wr_cq);
	ib_wqueue_free(mtflush_io->rd_cq);

	os_fast_mutex_free(&mtflush_mtx);

	/* Free heap */
	mem_heap_free(mtflush_io->wheap);
}

/******************************************************************//**
Initialize multi-threaded flush thread syncronization data.
@return Initialized multi-threaded flush thread syncroniztion data. */
void*
buf_mtflu_handler_init(
/*===================*/
	ulint n_threads,	/*!< in: Number of threads to create */
	ulint wrk_cnt)		/*!< in: Number of work items */
{
	ulint   	i;
	mem_heap_t*	mtflush_heap;
	ib_wqueue_t*	mtflush_work_queue;
	ib_wqueue_t*	mtflush_write_comp_queue;
	ib_wqueue_t*	mtflush_read_comp_queue;
	wrk_t*		work_items;

	os_fast_mutex_init(PFS_NOT_INSTRUMENTED, &mtflush_mtx);

	/* Create heap, work queue, write completion queue, read
	completion queue for multi-threaded flush, and init
	handler. */
	mtflush_heap = mem_heap_create(0);
	ut_a(mtflush_heap != NULL);
	mtflush_work_queue = ib_wqueue_create();
	ut_a(mtflush_work_queue != NULL);
	mtflush_write_comp_queue = ib_wqueue_create();
	ut_a(mtflush_write_comp_queue != NULL);
	mtflush_read_comp_queue = ib_wqueue_create();
	ut_a(mtflush_read_comp_queue != NULL);

	mtflush_ctx = (thread_sync_t *)mem_heap_alloc(mtflush_heap,
				MTFLUSH_MAX_WORKER * sizeof(thread_sync_t));
	ut_a(mtflush_ctx != NULL);
	work_items = (wrk_t*)mem_heap_alloc(mtflush_heap,
					    MTFLUSH_MAX_WORKER * sizeof(wrk_t));
	ut_a(work_items != NULL);
	memset(work_items, 0, sizeof(wrk_t) * MTFLUSH_MAX_WORKER);
	memset(mtflush_ctx, 0, sizeof(thread_sync_t) * MTFLUSH_MAX_WORKER);

	/* Initialize work items */
	mtflu_setup_work_items(work_items, n_threads);

	/* Create threads for page-compression-flush */
	for(i=0; i < n_threads; i++) {
		os_thread_id_t new_thread_id;
		mtflush_ctx[i].n_threads = n_threads;
		mtflush_ctx[i].wq = mtflush_work_queue;
		mtflush_ctx[i].wr_cq = mtflush_write_comp_queue;
		mtflush_ctx[i].rd_cq = mtflush_read_comp_queue;
		mtflush_ctx[i].wheap = mtflush_heap;
		mtflush_ctx[i].wt_status = WTHR_INITIALIZED;
		mtflush_ctx[i].work_item = work_items;

		mtflush_ctx[i].wthread = os_thread_create(
			mtflush_io_thread,
			((void *)(mtflush_ctx + i)),
	                &new_thread_id);

		mtflush_ctx[i].wthread_id = new_thread_id;
	}

	buf_mtflu_work_init();

	return((void *)mtflush_ctx);
}

/******************************************************************//**
Flush buffer pool instances.
@return number of pages flushed. */
ulint
buf_mtflu_flush_work_items(
/*=======================*/
	ulint buf_pool_inst,		/*!< in: Number of buffer pool instances */
	ulint *per_pool_pages_flushed,	/*!< out: Number of pages
					flushed/instance */
	enum buf_flush flush_type,	/*!< in: Type of flush */
	ulint min_n,			/*!< in: Wished minimum number of
					blocks to be flushed */
	lsn_t lsn_limit)		/*!< in: All blocks whose
					oldest_modification is smaller than
					this should be flushed (if their
					number does not exceed min_n) */
{
	ulint n_flushed=0, i;
	wrk_t *done_wi;

	for(i=0;i<buf_pool_inst; i++) {
		mtflush_ctx->work_item[i].tsk = MT_WRK_WRITE;
		mtflush_ctx->work_item[i].rd.page_pool = NULL;
		mtflush_ctx->work_item[i].wr.buf_pool = buf_pool_from_array(i);
		mtflush_ctx->work_item[i].wr.flush_type = flush_type;
		mtflush_ctx->work_item[i].wr.min = min_n;
		mtflush_ctx->work_item[i].wr.lsn_limit = lsn_limit;
		mtflush_ctx->work_item[i].id_usr = -1;
		mtflush_ctx->work_item[i].wi_status = WRK_ITEM_SET;

		ib_wqueue_add(mtflush_ctx->wq,
			(void *)(&(mtflush_ctx->work_item[i])),
			mtflush_ctx->wheap);
	}

	/* wait on the completion to arrive */
   	for(i=0; i< buf_pool_inst;) {
		done_wi = (wrk_t *)ib_wqueue_timedwait(mtflush_ctx->wr_cq, 50000);

		if (done_wi != NULL) {
			if(done_wi->n_flushed == 0) {
				per_pool_pages_flushed[i] = 0;
			} else {
				per_pool_pages_flushed[i] = done_wi->n_flushed;
			}

			if((int)done_wi->id_usr == -1 &&
			   done_wi->wi_status == WRK_ITEM_SET ) {
#ifdef UNIV_DEBUG
				fprintf(stderr,
					"**Set/Unused work_item[%lu] flush_type=%d\n",
					i,
					done_wi->wr.flush_type);
				ut_ad(0);
#endif
			}

			n_flushed+= done_wi->n_flushed;
			i++;
		}
	}

	return(n_flushed);
}

/*******************************************************************//**
Multi-threaded version of buf_flush_list
*/
bool
buf_mtflu_flush_list(
/*=================*/
	ulint		min_n,		/*!< in: wished minimum mumber of blocks
					flushed (it is not guaranteed that the
					actual number is that big, though) */
	lsn_t		lsn_limit,	/*!< in the case BUF_FLUSH_LIST all
					blocks whose oldest_modification is
					smaller than this should be flushed
					(if their number does not exceed
					min_n), otherwise ignored */
	ulint*		n_processed)	/*!< out: the number of pages
					which were processed is passed
					back to caller. Ignored if NULL */

{
	ulint		i;
	bool		success = true;
	ulint		cnt_flush[MTFLUSH_MAX_WORKER];

	if (n_processed) {
		*n_processed = 0;
	}

	if (min_n != ULINT_MAX) {
		/* Ensure that flushing is spread evenly amongst the
		buffer pool instances. When min_n is ULINT_MAX
		we need to flush everything up to the lsn limit
		so no limit here. */
		min_n = (min_n + srv_buf_pool_instances - 1)
			 / srv_buf_pool_instances;
	}

	/* This lock is to safequard against re-entry if any. */
	os_fast_mutex_lock(&mtflush_mtx);
	buf_mtflu_flush_work_items(srv_buf_pool_instances,
                cnt_flush, BUF_FLUSH_LIST,
                min_n, lsn_limit);
	os_fast_mutex_unlock(&mtflush_mtx);

	for (i = 0; i < srv_buf_pool_instances; i++) {
		if (n_processed) {
			*n_processed += cnt_flush[i];
		}
		if (cnt_flush[i]) {
			MONITOR_INC_VALUE_CUMULATIVE(
				MONITOR_FLUSH_BATCH_TOTAL_PAGE,
				MONITOR_FLUSH_BATCH_COUNT,
				MONITOR_FLUSH_BATCH_PAGES,
				cnt_flush[i]);
		}
	}
#ifdef UNIV_DEBUG
	fprintf(stderr, "%s: [1] [*n_processed: (min:%lu)%lu ]\n",
		__FUNCTION__, (min_n * srv_buf_pool_instances), *n_processed);
#endif
	return(success);
}

/*********************************************************************//**
Clears up tail of the LRU lists:
* Put replaceable pages at the tail of LRU to the free list
* Flush dirty pages at the tail of LRU to the disk
The depth to which we scan each buffer pool is controlled by dynamic
config parameter innodb_LRU_scan_depth.
@return total pages flushed */
UNIV_INTERN
ulint
buf_mtflu_flush_LRU_tail(void)
/*==========================*/
{
	ulint	total_flushed=0, i;
	ulint	cnt_flush[MTFLUSH_MAX_WORKER];

	ut_a(buf_mtflu_init_done());

	/* This lock is to safeguard against re-entry if any */
	os_fast_mutex_lock(&mtflush_mtx);
	buf_mtflu_flush_work_items(srv_buf_pool_instances,
		cnt_flush, BUF_FLUSH_LRU, srv_LRU_scan_depth, 0);
	os_fast_mutex_unlock(&mtflush_mtx);

	for (i = 0; i < srv_buf_pool_instances; i++) {
		if (cnt_flush[i]) {
			total_flushed += cnt_flush[i];

			MONITOR_INC_VALUE_CUMULATIVE(
			        MONITOR_LRU_BATCH_TOTAL_PAGE,
			        MONITOR_LRU_BATCH_COUNT,
			        MONITOR_LRU_BATCH_PAGES,
			        cnt_flush[i]);
		}
	}

#if UNIV_DEBUG
	fprintf(stderr, "[1] [*n_processed: (min:%lu)%lu ]\n", (
			srv_LRU_scan_depth * srv_buf_pool_instances), total_flushed);
#endif

	return(total_flushed);
}

/*********************************************************************//**
Set correct thread identifiers to io thread array based on
information we have. */
void
buf_mtflu_set_thread_ids(
/*=====================*/
	ulint		n_threads,	/*!<in: Number of threads to fill */
        void*		ctx,		/*!<in: thread context */
	os_thread_id_t*	thread_ids)	/*!<in: thread id array */
{
	thread_sync_t *mtflush_io = ((thread_sync_t *)ctx);
	ulint i;
	ut_a(mtflush_io != NULL);
	ut_a(thread_ids != NULL);

	for(i = 0; i < n_threads; i++) {
		thread_ids[i] = mtflush_io[i].wthread_id;
	}
}
