/*****************************************************************************

Copyright (C) 2013 Fusion-io. All Rights Reserved.
Copyright (C) 2013 SkySQL Ab. All Rights Reserved.

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
***********************************************************************/

#include <time.h>

#ifdef UNIV_PFS_MUTEX
/* Key to register fil_system_mutex with performance schema */
UNIV_INTERN mysql_pfs_key_t	mtflush_mutex_key;
#endif /* UNIV_PFS_MUTEX */

/* Mutex to protect critical sections during multi-threaded flush */
ib_mutex_t mt_flush_mutex;

#define	MT_COMP_WATER_MARK	50

/* Work item status */
typedef enum {
	WORK_ITEM_SET=0,	/* Work item information set */
	WORK_ITEM_START=1,	/* Work item assigned to thread and
				execution started */
	WORK_ITEM_DONE=2,	/* Work item execution done */
} mtflu_witem_status_t;

/* Work thread status */
typedef enum {
	WORK_THREAD_NOT_INIT=0,		/* Work thread not initialized */
	WORK_THREAD_INITIALIZED=1,	/* Work thread initialized */
	WORK_THREAD_SIG_WAITING=2,	/* Work thred signaled */
	WORK_THREAD_RUNNING=3,		/* Work thread running */
	WORK_THREAD_NO_WORK=4,		/* Work thread has no work to do */
} mtflu_wthr_status_t;

/* Structure containing multi-treaded flush thread information */
typedef struct {
	os_thread_t  		wthread_id;		/* Thread id */
	opq_t			*wq;			/* Write queue ? */
	opq_t			*cq;			/* Commit queue ?*/
	ib_mutex_t 		thread_mutex;		/* Mutex proecting below
							structures */
	mtflu_wthr_status_t	thread_status;		/* Thread status */
	ib_uint64_t		total_num_processed;	/* Total number of
							pages processed */
	ib_uint64_t		cycle_num_processed;	/* Numper of pages
							processed on last
							cycle */
	ulint			check_wrk_done_count;	/* Number of pages
							to process in this
							work item ? */
	ulint			done_cnt_flag;		/* Number of pages
							processed in this
							work item ?*/
} mtflu_thread_t;

struct work_item_t {
	/****************************/
	/* Need to group into struct*/
	buf_pool_t*	buf_pool;	//buffer-pool instance
	int 		flush_type;	//flush-type for buffer-pool flush operation
	ulint 		min;		//minimum number of pages requested to be flushed
	lsn_t 		lsn_limit;	//lsn limit for the buffer-pool flush operation
	/****************************/

	unsigned long	result; 	//flush pages count
	unsigned long	t_usec;		//time-taken in usec
	os_thread_t		id_usr;		/* thread-id
						currently working , why ? */
	mtflu_witem_status_t    wi_status;     /* work item status */

	UT_LIST_NODE_T(work_node_t) next;
};

/* Multi-threaded flush system structure */
typedef struct {
	int 		pgc_n_threads = 8;// ??? why what this is

	mtflu_thread_t 	pc_sync[PGCOMP_MAX_WORKER];
	wrk_t 		work_items[PGCOMP_MAX_WORKER];
	int 		pgcomp_wrk_initialized = -1; /* ???? */
	opq_t		wq; /* write queue ? */
	opq_t		cq; /* commit queue ? */
} mtflu_system_t;

typedef enum op_q_status {
    Q_NOT_INIT=0,
    Q_EMPTY=1,
    Q_INITIALIZED=2,
    Q_PROCESS=3,
    Q_DONE=4,
    Q_ERROR=5,
    Q_STATUS_UNDEFINED
} q_status_t;

// NOTE: jan: could we use ut/ut0wqueue.(h|cc)
// NOTE: jan: here ????, it would handle waiting, signaling
// and contains simple interface

typedef struct op_queue
{
	ib_mutex_t		mtx;	/* Mutex protecting below variables
					*/
	os_cond_t 		cv;	/* ? is waiting here ? */
	q_status_t		flag;	/* Operation queue status */
	UT_LIST_BASE_NODE_T(work_item_t) work_list;
} opq_t;


/*******************************************************************//**
Initialize multi-threaded flush.
*/
void
buf_mtflu_init(void)
/*================*/
{
	mutex_create(mtflush_mutex_key,
			     &mt_flush_mutex, SYNC_ANY_LATCH);
}

/*******************************************************************//**
This utility flushes dirty blocks from the end of the LRU list and also
puts replaceable clean pages from the end of the LRU list to the free
list.
NOTE: The calling thread is not allowed to own any latches on pages!
@return true if a batch was queued successfully. false if another batch
of same type was already running. */
bool
buf_mtflu_flush_LRU(
/*================*/
	buf_pool_t*	buf_pool,	/*!< in/out: buffer pool instance */
	ulint		min_n,		/*!< in: wished minimum mumber of blocks
					flushed (it is not guaranteed that the
					actual number is that big, though) */
	ulint*		n_processed)	/*!< out: the number of pages
					which were processed is passed
					back to caller. Ignored if NULL */
{
	ulint		page_count;

	if (n_processed) {
		*n_processed = 0;
	}

	if (!buf_flush_start(buf_pool, BUF_FLUSH_LRU)) {
		return(false);
	}

	page_count = buf_flush_batch(buf_pool, BUF_FLUSH_LRU, min_n, 0);

	buf_flush_end(buf_pool, BUF_FLUSH_LRU);

	buf_flush_common(BUF_FLUSH_LRU, page_count);

	if (n_processed) {
		*n_processed = page_count;
	}

	return(true);
}

#ifdef UNIV_DEBUG
/*******************************************************************//**
Utility function to calculate time difference between start time
and end time.
@return Time difference.
*/
UNIV_INTERN
void
mtflu_timediff(
/*===========*/
	struct timeval *g_time, /*!< in/out: Start time*/
	struct timeval *s_time, /*!< in/out: End time */
	struct timeval *d_time) /*!< out: Time difference */
{
	if (g_time->tv_usec < s_time->tv_usec)
	{
		int nsec = (s_time->tv_usec - g_time->tv_usec) / 1000000 + 1;
		s_time->tv_usec -= 1000000 * nsec;
		s_time->tv_sec += nsec;
	}
	if (g_time->tv_usec - s_time->tv_usec > 1000000)
	{
		int nsec = (s_time->tv_usec - g_time->tv_usec) / 1000000;
		s_time->tv_usec += 1000000 * nsec;
		s_time->tv_sec -= nsec;
	}
	d_time->tv_sec = g_time->tv_sec - s_time->tv_sec;
	d_time->tv_usec = g_time->tv_usec - s_time->tv_usec;
}
#endif

/*******************************************************************//**
This utility flushes dirty blocks from the end of the flush list of
all buffer pool instances. This is multi-threaded version of buf_flush_list.
NOTE: The calling thread is not allowed to own any latches on pages!
@return true if a batch was queued successfully for each buffer pool
instance. false if another batch of same type was already running in
at least one of the buffer pool instance */
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
	struct timeval p_start_time, p_end_time, d_time;

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

#ifdef UNIV_DEBUG
	gettimeofday(&p_start_time, 0x0);
#endif
	if(is_pgcomp_wrk_init_done() && (min_n > MT_COMP_WATER_MARK)) {
		int cnt_flush[32];

                mutex_enter(&mt_flush_mutex);

#ifdef UNIV_DEBUG
		fprintf(stderr, "Calling into wrk-pgcomp [min:%lu]", min_n);
#endif
		pgcomp_flush_work_items(srv_buf_pool_instances,
					cnt_flush, BUF_FLUSH_LIST,
					min_n, lsn_limit);

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

		mutex_exit(&pgcomp_mtx);

#ifdef UNIV_DEBUG
		gettimeofday(&p_end_time, 0x0);
		timediff(&p_end_time, &p_start_time, &d_time);
		fprintf(stderr, "[1] [*n_processed: (min:%lu)%lu %llu usec]\n", (
				min_n * srv_buf_pool_instances), *n_processed,
				(unsigned long long)(d_time.tv_usec+(d_time.tv_sec*1000000)));
#endif
		return(success);
	}

	/* Flush to lsn_limit in all buffer pool instances */
	for (i = 0; i < srv_buf_pool_instances; i++) {
		buf_pool_t*	buf_pool;
		ulint		page_count = 0;

		buf_pool = buf_pool_from_array(i);

		if (!buf_flush_start(buf_pool, BUF_FLUSH_LIST)) {
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
			success = false;

			continue;
		}

		page_count = buf_flush_batch(
			buf_pool, BUF_FLUSH_LIST, min_n, lsn_limit);

		buf_flush_end(buf_pool, BUF_FLUSH_LIST);

		buf_flush_common(BUF_FLUSH_LIST, page_count);

		if (n_processed) {
			*n_processed += page_count;
		}

		if (page_count) {
			MONITOR_INC_VALUE_CUMULATIVE(
				MONITOR_FLUSH_BATCH_TOTAL_PAGE,
				MONITOR_FLUSH_BATCH_COUNT,
				MONITOR_FLUSH_BATCH_PAGES,
				page_count);
		}
	}

#ifdef UNIV_DEBUG
	gettimeofday(&p_end_time, 0x0);
	timediff(&p_end_time, &p_start_time, &d_time);

	fprintf(stderr, "[2] [*n_processed: (min:%lu)%lu %llu usec]\n", (
			min_n * srv_buf_pool_instances), *n_processed,
			(unsigned long long)(d_time.tv_usec+(d_time.tv_sec*1000000)));
#endif
	return(success);
}

/*********************************************************************//**
Clear up tail of the LRU lists:
* Put replaceable pages at the tail of LRU to the free list
* Flush dirty pages at the tail of LRU to the disk
The depth to which we scan each buffer pool is controlled by dynamic
config parameter innodb_LRU_scan_depth.
@return total pages flushed */
ulint
buf_mtflu_flush_LRU_tail(void)
/*==========================*/
{
	ulint   total_flushed=0, i=0;
	int cnt_flush[32];

#ifdef UNIV_DEBUG
	struct  timeval p_start_time, p_end_time, d_time;
	gettimeofday(&p_start_time, 0x0);
#endif
	assert(is_pgcomp_wrk_init_done());

	mutex_enter(&pgcomp_mtx);
	pgcomp_flush_work_items(srv_buf_pool_instances,
		cnt_flush, BUF_FLUSH_LRU, srv_LRU_scan_depth, 0);

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

	mutex_exit(&pgcomp_mtx);

#if UNIV_DEBUG
	gettimeofday(&p_end_time, 0x0);
	timediff(&p_end_time, &p_start_time, &d_time);

	fprintf(stderr, "[1] [*n_processed: (min:%lu)%lu %llu usec]\n", (
			srv_LRU_scan_depth * srv_buf_pool_instances), total_flushed,
		(unsigned long long)(d_time.tv_usec+(d_time.tv_sec*1000000)));
#endif

	return(total_flushed);
}

/*******************************************************************//**
Set work done count to given count.
@return 1 if still work to do, 0 if no work left */
int
set_check_done_flag_count(int cnt)
/*================*/
{
	return(check_wrk_done_count = cnt);
}

/*******************************************************************//**
?
@return why ? */
int
set_pgcomp_wrk_init_done(void)
/*================*/
{
	pgcomp_wrk_initialized = 1;
	return 0;
}

/*******************************************************************//**
?
@return true if work is initialized */
bool
is_pgcomp_wrk_init_done(void)
/*================*/
{
	return(pgcomp_wrk_initialized == 1);
}

/*******************************************************************//**
Set current done pages count to the given value
@return number of pages flushed */
int 
set_done_cnt_flag(int val)
/*================*/
{
	/*
 	 * Assumption: The thread calling into set_done_cnt_flag
 	 * needs to have "cq.mtx" acquired, else not safe.
 	 */
	done_cnt_flag = val;
	return done_cnt_flag;
}

/*******************************************************************//**
?
@return number of pages flushed */
int
cv_done_inc_flag_sig(thread_sync_t * ppc)
/*================*/
{
	mutex_enter(&ppc->cq->mtx);
	ppc->stat_universal_num_processed++;
	ppc->stat_cycle_num_processed++;
	done_cnt_flag++;
	if(!(done_cnt_flag <= check_wrk_done_count)) {
		fprintf(stderr, "ERROR: done_cnt:%d check_wrk_done_count:%d\n",
			done_cnt_flag, check_wrk_done_count);
	}
	assert(done_cnt_flag <= check_wrk_done_count);
	mutex_exit(&ppc->cq->mtx);
	if(done_cnt_flag == check_wrk_done_count) {
		// why below does not need mutex protection ?
		ppc->wq->flag = Q_DONE;
		mutex_enter(&ppc->cq->mtx);
		ppc->cq->flag = Q_DONE;
		os_cond_signal(&ppc->cq->cv);
		mutex_exit(&ppc->cq->mtx);
	}
	return(done_cnt_flag);
}

/*******************************************************************//**
Remove work item from queue, in my opinion not needed after we use
UT_LIST
@return number of pages flushed */
int
q_remove_wrk(opq_t *q, wrk_t **wi)
/*================*/
{
	int ret = 0;

	if(!wi || !q) {
		return -1;
	}

	mutex_enter(&q->mtx);
	assert(!((q->tail == NULL) && (q->head != NULL)));
	assert(!((q->tail != NULL) && (q->head == NULL)));

	/* get the first in the list*/
	*wi = q->head;
	if(q->head) {
		ret = 0;
		q->head = q->head->next;
		(*wi)->next = NULL;
		if(!q->head) {
			q->tail = NULL;
		}
	} else {
		q->tail = NULL;
		ret = 1; /* indicating remove from queue failed */
	}
	mutex_exit(&q->mtx);
	return (ret);
}

/*******************************************************************//**
Return true if work item has being assigned to a thread or false
if work item is not assigned.
@return true if work is assigned, false if not */
bool
is_busy_wrk_itm(wrk_t *wi)
/*================*/
{
	if(!wi) {
		return -1;
	}
	return(!(wi->id_usr == -1));
}

/*******************************************************************//**
Initialize work items.
@return why ? */
int
setup_wrk_itm(int items)
/*================*/
{
	int i;
	for(i=0; i<items; i++) {
		work_items[i].buf_pool = NULL;
		work_items[i].result = 0;
		work_items[i].t_usec = 0;
		work_items[i].id_usr = -1;
		work_items[i].wi_status = WRK_ITEM_STATUS_UNDEFINED;
		work_items[i].next = &work_items[(i+1)%items];
	}
	/* last node should be the tail */
	work_items[items-1].next = NULL;
	return 0;
}

/*******************************************************************//**
Initialize queue
@return why ? */
int
init_queue(opq_t *q)
/*================*/
{
	if(!q) {
		return -1;
	}
	/* Initialize Queue mutex and CV */
	q->mtx = os_mutex_create();
        os_cond_init(&q->cv);
	q->flag = Q_INITIALIZED;
	q->head = q->tail = NULL;

	return 0;
}

/// NEEDED ?
#if 0
int drain_cq(opq_t *cq, int items)
{
	int i=0;

	if(!cq) {
		return -1;
	}
	mutex_enter(&cq->mtx);
	for(i=0; i<items; i++) {
		work_items[i].result=0;
		work_items[i].t_usec = 0;
		work_items[i].id_usr = -1;
	}
	cq->head = cq->tail = NULL;
	mutex_unlock(&cq->mtx);
	return 0;
}
#endif

/*******************************************************************//**
Insert work item list to queue, not needed with UT_LIST
@return why ? */
int
q_insert_wrk_list(opq_t *q, wrk_t *w_list)
/*================*/
{
	if((!q) || (!w_list)) {
		fprintf(stderr, "insert failed q:%p w:%p\n", q, w_list);
		return -1;
	}

	mutex_enter(&q->mtx);

	assert(!((q->tail == NULL) && (q->head != NULL)));
	assert(!((q->tail != NULL) && (q->head == NULL)));

	/* list is empty */
	if(!q->tail) {
		q->head = q->tail = w_list;
	} else {
		/* added the first of the node to list */
        	assert(q->head != NULL);
		q->tail->next = w_list;
	}

	/* move tail to the last node */
	while(q->tail->next) {
		q->tail = q->tail->next;
	}
	mutex_exit(&q->mtx);

	return 0;
}

/*******************************************************************//**
Flush ?
@return why ? */
int
flush_pool_instance(wrk_t *wi)
/*================*/
{
	struct timeval p_start_time, p_end_time, d_time;

	if(!wi) {
		fprintf(stderr, "work item invalid wi:%p\n", wi);
		return -1;
	}

	wi->t_usec = 0;
	if (!buf_flush_start(wi->buf_pool, (buf_flush_t)wi->flush_type)) {
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
		fprintf(stderr, "flush_start Failed, flush_type:%d\n",
			(buf_flush_t)wi->flush_type);
		return -1;
	}

#ifdef UNIV_DEBUG
	/* Record time taken for the OP in usec */
	gettimeofday(&p_start_time, 0x0);
#endif

	if((buf_flush_t)wi->flush_type == BUF_FLUSH_LRU) {
		/* srv_LRU_scan_depth can be arbitrarily large value.
		* We cap it with current LRU size.
		*/
		buf_pool_mutex_enter(wi->buf_pool);
		wi->min = UT_LIST_GET_LEN(wi->buf_pool->LRU);
		buf_pool_mutex_exit(wi->buf_pool);
		wi->min = ut_min(srv_LRU_scan_depth,wi->min);
	}

	wi->result = buf_flush_batch(wi->buf_pool,
                                    (buf_flush_t)wi->flush_type,
                                    wi->min, wi->lsn_limit);

	buf_flush_end(wi->buf_pool, (buf_flush_t)wi->flush_type);
	buf_flush_common((buf_flush_t)wi->flush_type, wi->result);

#ifdef UNIV_DEBUG
	gettimeofday(&p_end_time, 0x0);
	timediff(&p_end_time, &p_start_time, &d_time);

	wi->t_usec = (unsigned long)(d_time.tv_usec+(d_time.tv_sec*1000000));
#endif
	return 0;
}

/*******************************************************************//**
?
@return why ? */
int
service_page_comp_io(thread_sync_t * ppc)
/*================*/
{
	wrk_t 		*wi = NULL;
	int 		ret=0;
	struct timespec	ts;

	mutex_enter(&ppc->wq->mtx);
	do{
		ppc->wt_status = WTHR_SIG_WAITING;
		ret = os_cond_wait(&ppc->wq->cv, &ppc->wq->mtx);
		ppc->wt_status = WTHR_RUNNING;
		if(ret == ETIMEDOUT) {
			fprintf(stderr, "ERROR ETIMEDOUT cnt_flag:[%d] ret:%d\n",
				done_cnt_flag, ret);
		} else if(ret == EINVAL || ret == EPERM) {
			fprintf(stderr, "ERROR EINVAL/EPERM cnt_flag:[%d] ret:%d\n",
				done_cnt_flag, ret);
		}
		if(ppc->wq->flag == Q_PROCESS) {
			break;
		} else {
			mutex_exit(&ppc->wq->mtx);
			return -1;
		}
	} while (ppc->wq->flag == Q_PROCESS && ret == 0);

	mutex_exit(&ppc->wq->mtx);

	while (ppc->cq->flag == Q_PROCESS) {
		wi = NULL;
		/* Get the work item */
		if (0 != (ret = q_remove_wrk(ppc->wq, &wi))) {
			ppc->wt_status = WTHR_NO_WORK;
			return -1;
		}

		assert(ret==0);
		assert(wi != NULL);
		assert(0 == is_busy_wrk_itm(wi));
		assert(wi->id_usr == -1);

		wi->id_usr = ppc->wthread;
		wi->wi_status = WRK_ITEM_START;

		/* Process work item */
		if(0 != (ret = flush_pool_instance(wi))) {
			fprintf(stderr, "FLUSH op failed ret:%d\n", ret);
			wi->wi_status = WRK_ITEM_FAILED;
		}
		ret = q_insert_wrk_list(ppc->cq, wi);

		assert(0==ret);
		assert(check_wrk_done_count >= done_cnt_flag);
		wi->wi_status = WRK_ITEM_SUCCESS;
		if(check_wrk_done_count == cv_done_inc_flag_sig(ppc)) {
			break;
		}
	}
	return(0);
}

/******************************************************************//**
Thread main function for multi-threaded flush
@return a dummy parameter*/
extern "C" UNIV_INTERN
os_thread_ret_t
DECLARE_THREAD(page_comp_io_thread)(
/*==========================================*/
	void * arg)
{
	thread_sync_t *ppc_io = ((thread_sync_t *)arg);

	while (srv_shutdown_state != SRV_SHUTDOWN_EXIT_THREADS) {
		service_page_comp_io(ppc_io);
		ppc_io->stat_cycle_num_processed = 0;
	}
	os_thread_exit(NULL);
	OS_THREAD_DUMMY_RETURN;
}

/*******************************************************************//**
Print queue work item
@return why ? */
int
print_queue_wrk_itm(opq_t *q)
/*================*/
{
#if UNIV_DEBUG
	wrk_t *wi = NULL;

	if(!q) {
		fprintf(stderr, "queue NULL\n");
		return -1;
	}

	if(!q->head || !q->tail) {
		assert(!(((q->tail==NULL) && (q->head!=NULL)) && ((q->tail != NULL) && (q->head == NULL))));
		fprintf(stderr, "queue empty (h:%p t:%p)\n", q->head, q->tail);
		return 0;
	}

	mutex_enter(&q->mtx);
	for(wi = q->head; (wi != NULL) ; wi = wi->next) {
		//fprintf(stderr, "- [%p] %p %lu %luus [%ld] >%p\n",
		//	wi, wi->buf_pool, wi->result, wi->t_usec, wi->id_usr, wi->next);
		fprintf(stderr, "- [%p] [%s] >%p\n",
			wi, (wi->id_usr == -1)?"free":"Busy", wi->next);
	}
	mutex_exit(&q->mtx);
#endif
	return(0);
}

/*******************************************************************//**
Print work list
@return why ? */
int
print_wrk_list(wrk_t *wi_list)
/*================*/
{
	wrk_t *wi = wi_list;
	int i=0;

	if(!wi_list) {
		fprintf(stderr, "list NULL\n");
	}

	while(wi) {
		fprintf(stderr, "-\t[%p]\t[%s]\t[%lu]\t[%luus] > %p\n",
			wi, (wi->id_usr == -1)?"free":"Busy", wi->result, wi->t_usec, wi->next);
		wi = wi->next;
		i++;
	}
	fprintf(stderr, "list len: %d\n", i);
	return 0;
}

/*******************************************************************//**
?
@return why ? */
int
pgcomp_handler(wrk_t *w_list)
/*================*/
{
	struct timespec   ts;
	int ret=0, t_flag=0;
	opq_t *wrk_q=NULL, *comp_q=NULL;
	wrk_t *tw_list=NULL;

	wrk_q=&wq;
	comp_q=&cq;

	mutex_enter(&wrk_q->mtx);
	/* setup work queue here.. */
	wrk_q->flag = Q_EMPTY;
	mutex_exit(&wrk_q->mtx);

	ret = q_insert_wrk_list(wrk_q, w_list);
	if(ret != 0) {
		fprintf(stderr, "%s():work-queue setup FAILED wq:%p w_list:%p \n",
			__FUNCTION__, &wq, w_list);
		return -1;
	}

retry_submit:
	mutex_enter(&wrk_q->mtx);
	/* setup work queue here.. */
	wrk_q->flag = Q_INITIALIZED;
	mutex_exit(&wrk_q->mtx);


	mutex_enter(&comp_q->mtx);
	if(0 != set_done_cnt_flag(0)) {
		fprintf(stderr, "FAILED %s:%d\n", __FILE__, __LINE__);
		mutex_exit(&comp_q->mtx);
		return -1;
	}
	comp_q->flag = Q_PROCESS;
	mutex_enter(&comp_q->mtx);

	/* if threads are waiting request them to start */
	mutex_enter(&wrk_q->mtx);
	wrk_q->flag = Q_PROCESS;
	os_cond_broadcast(&wrk_q->cv);
	mutex_exit(&wrk_q->mtx);

	/* Wait on all worker-threads to complete */
	mutex_enter(&comp_q->mtx);
	if (comp_q->flag != Q_DONE) {
		do {
			os_cond_wait(&comp_q->cv, &comp_q->mtx);
			if(comp_q->flag != Q_DONE) {
				fprintf(stderr, "[1] cv wait on CQ failed flag:%d cnt:%d\n",
					comp_q->flag, done_cnt_flag);
				if (done_cnt_flag != srv_buf_pool_instances) {
					fprintf(stderr, "[2] cv wait on CQ failed flag:%d cnt:%d\n",
						comp_q->flag, done_cnt_flag);
					fprintf(stderr, "============\n");
					print_wrk_list(w_list);
					fprintf(stderr, "============\n");
				}
				continue;
			} else if (done_cnt_flag != srv_buf_pool_instances) {
				fprintf(stderr, "[3]cv wait on CQ failed flag:%d cnt:%d\n",
					comp_q->flag, done_cnt_flag);
				fprintf(stderr, "============\n");
				print_wrk_list(w_list);
				fprintf(stderr, "============\n");
				comp_q->flag = Q_INITIALIZED;
				mutex_exit(&comp_q->mtx);
				goto retry_submit;

				ut_ad(!done_cnt_flag);
				continue;
			}
			ut_ad(done_cnt_flag == srv_buf_pool_instances);

			if ((comp_q->flag == Q_DONE) &&
				(done_cnt_flag == srv_buf_pool_instances)) {
				break;
			}
		} while((comp_q->flag == Q_INITIALIZED) &&
			(done_cnt_flag != srv_buf_pool_instances));
	} else {
		fprintf(stderr, "[4] cv wait on CQ failed flag:%d cnt:%d\n",
			comp_q->flag, done_cnt_flag);
		if (!done_cnt_flag) {
			fprintf(stderr, "============\n");
			print_wrk_list(w_list);
			fprintf(stderr, "============\n");
			comp_q->flag = Q_INITIALIZED;
			mutex_enter(&comp_q->mtx);
			goto retry_submit;
			ut_ad(!done_cnt_flag);
		}
		ut_ad(done_cnt_flag == srv_buf_pool_instances);
	}

	mutex_exit(&comp_q->mtx);
	mutex_enter(&wrk_q->mtx);
	wrk_q->flag = Q_DONE;
        mutex_exit(&wrk_q->mtx);

	return 0;
}

/******************************************************************//**
@return a dummy parameter*/
int 
pgcomp_handler_init(
	int num_threads, 
	int wrk_cnt, 
	opq_t *wq, 
	opq_t *cq)
/*================*/
{
	int   	i=0;

	if(is_pgcomp_wrk_init_done()) {
		fprintf(stderr, "pgcomp_handler_init(): ERROR already initialized\n");
		return -1;
	}

	if(!wq || !cq) {
		fprintf(stderr, "%s() FAILED wq:%p cq:%p\n", __FUNCTION__, wq, cq);
		return -1;
	}
	
	/* work-item setup */
	setup_wrk_itm(wrk_cnt);

	/* wq & cq setup */
	init_queue(wq);
	init_queue(cq);

	/* Mark each of the thread sync entires */
	for(i=0; i < PGCOMP_MAX_WORKER; i++) {
		pc_sync[i].wthread_id = i;
	}

	/* Create threads for page-compression-flush */
	for(i=0; i < num_threads; i++) {
		pc_sync[i].wthread_id = i;
		pc_sync[i].wq = wq;
		pc_sync[i].cq = cq;
		os_thread_create(page_comp_io_thread, ((void *)(pc_sync + i)),
					thread_ids + START_PGCOMP_CNT + i);
		//pc_sync[i].wthread = thread_ids[START_PGCOMP_CNT + i];
		pc_sync[i].wthread = (START_PGCOMP_CNT + i);
		pc_sync[i].wt_status = WTHR_INITIALIZED;
	}

	set_check_done_flag_count(wrk_cnt);
	set_pgcomp_wrk_init_done();

	return 0;
}


/*******************************************************************//**
Print work thread status information
@return why ? */
int 
wrk_thread_stat(
	thread_sync_t *wthr, 
	unsigned int num_threads)
/*================*/
{
	long stat_tot=0;
	int i=0;
	for(i=0; i<num_threads;i++) {
		stat_tot+=wthr[i].stat_universal_num_processed;
		fprintf(stderr, "[%d] stat [%lu]\n", wthr[i].wthread_id,
			wthr[i].stat_universal_num_processed);
	}
	fprintf(stderr, "Stat-Total:%lu\n", stat_tot);
}

/*******************************************************************//**
Reset work items
@return why ? */
int
reset_wrk_itm(int items)
/*================*/
{
	int i;

	mutex_enter(&wq.mtx);
	wq.head = wq.tail = NULL;
	mutex_exit(&wq.mtx);

	mutex_enter(&cq.mtx);
	for(i=0;i<items; i++) {
		work_items[i].id_usr = -1;
	}
	cq.head = cq.tail = NULL;
	mutex_exit(&cq.mtx);
	return 0;
}

/*******************************************************************//**
?
@return why ? */
int 
pgcomp_flush_work_items(
/*================*/
	int buf_pool_inst, 
	int *per_pool_pages_flushed,
	int flush_type, 
	int min_n, 
	lsn_t lsn_limit)
{
	int ret=0, i=0;

   	mutex_enter(&wq.mtx);
   	mutex_enter(&cq.mtx);
    
	assert(wq.head == NULL);
    	assert(wq.tail == NULL);
	if(cq.head) {
		print_wrk_list(cq.head);
	}
    	assert(cq.head == NULL);
    	assert(cq.tail == NULL);

	for(i=0;i<buf_pool_inst; i++) {
		work_items[i].buf_pool = buf_pool_from_array(i);
		work_items[i].flush_type = flush_type;
		work_items[i].min = min_n;
		work_items[i].lsn_limit = lsn_limit;
		work_items[i].id_usr = -1;
		work_items[i].next = &work_items[(i+1)%buf_pool_inst];
		work_items[i].wi_status = WRK_ITEM_SET;
	}
	work_items[i-1].next=NULL;

	mutex_exit(&cq.mtx);
   	mutex_exit(&wq.mtx);

	pgcomp_handler(work_items);

   	mutex_enter(&wq.mtx);
   	mutex_enter(&cq.mtx);
	/* collect data/results total pages flushed */
	for(i=0; i<buf_pool_inst; i++) {
		if(work_items[i].result == -1) {
			ret = -1;
			per_pool_pages_flushed[i] = 0;
		} else {
			per_pool_pages_flushed[i] = work_items[i].result;
		}
		if((work_items[i].id_usr == -1) && (work_items[i].wi_status == WRK_ITEM_SET )) {
           		fprintf(stderr, "**Set/Unused work_item[%d] flush_type=%d\n", i, work_items[i].flush_type);
			assert(0);
		}
	}

	wq.flag = cq.flag = Q_INITIALIZED;

	mutex_exit(&cq.mtx);
   	mutex_exit(&wq.mtx);

#if UNIV_DEBUG
	/* Print work-list stats */
	fprintf(stderr, "==wq== [DONE]\n");
	print_wrk_list(wq.head);
	fprintf(stderr, "==cq== [DONE]\n");
	print_wrk_list(cq.head);
	fprintf(stderr, "==worker-thread-stats==\n");
	wrk_thread_stat(pc_sync, pgc_n_threads);
#endif

	/* clear up work-queue for next flush */
	reset_wrk_itm(buf_pool_inst);
	return(ret);
}

 

