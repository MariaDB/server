// Copyright 2014 Google

#ifndef btr0scrub_h
#define btr0scrub_h

#include "dict0dict.h"

/**
 * enum describing page allocation status
 */
enum btr_scrub_page_allocation_status_t {
	BTR_SCRUB_PAGE_FREE,
	BTR_SCRUB_PAGE_ALLOCATED,
	BTR_SCRUB_PAGE_ALLOCATION_UNKNOWN
};

/**
* constants returned by btr_page_needs_scrubbing & btr_scrub_recheck_page
*/
#define BTR_SCRUB_PAGE                         1 /* page should be scrubbed */
#define BTR_SCRUB_SKIP_PAGE                    2 /* no scrub & no action */
#define BTR_SCRUB_SKIP_PAGE_AND_CLOSE_TABLE    3 /* no scrub & close table */
#define BTR_SCRUB_SKIP_PAGE_AND_COMPLETE_SPACE 4 /* no scrub & complete space */
#define BTR_SCRUB_TURNED_OFF                   5 /* we detected that scrubbing
						 was disabled by global
						 variable */

/**************************************************************//**
struct for keeping scrub statistics. */
struct btr_scrub_stat_t {
	/* page reorganizations */
	ulint page_reorganizations;
	/* page splits */
	ulint page_splits;
	/* scrub failures */
	ulint page_split_failures_underflow;
	ulint page_split_failures_out_of_filespace;
	ulint page_split_failures_missing_index;
	ulint page_split_failures_unknown;
};

/**************************************************************//**
struct for thread local scrub state. */
struct btr_scrub_t {

	/* current space */
	ulint space;

	/* is scrubbing enabled for this space */
	bool scrubbing;

	/* is current space compressed */
	bool compressed;

	dict_table_t* current_table;
	dict_index_t* current_index;
	/* savepoint for X_LATCH of block */
	ulint savepoint;

	/* statistic counters */
	btr_scrub_stat_t scrub_stat;
};

/*********************************************************************
Init scrub global variables */
UNIV_INTERN
void
btr_scrub_init();

/*********************************************************************
Cleanup scrub globals */
UNIV_INTERN
void
btr_scrub_cleanup();

/***********************************************************************
Return crypt statistics */
UNIV_INTERN
void
btr_scrub_total_stat(
/*==================*/
	btr_scrub_stat_t *stat); /*!< out: stats to update */

/**************************************************************//**
Check if a page needs scrubbing
* @return BTR_SCRUB_PAGE if page should be scrubbed
* else btr_scrub_skip_page should be called
* with this return value (and without any latches held)
*/
UNIV_INTERN
int
btr_page_needs_scrubbing(
/*=====================*/
	btr_scrub_t*	scrub_data, /*!< in: scrub data  */
	buf_block_t*	block,	    /*!< in: block to check, latched */
	btr_scrub_page_allocation_status_t allocated); /*!< in: is block
						       allocated, free or
						       unknown */

/****************************************************************
Recheck if a page needs scrubbing, and if it does load appropriate
table and index
* @return BTR_SCRUB_PAGE if page should be scrubbed
* else btr_scrub_skip_page should be called
* with this return value (and without any latches held)
*/
UNIV_INTERN
int
btr_scrub_recheck_page(
/*====================*/
	btr_scrub_t* scrub_data,  /*!< inut: scrub data */
	buf_block_t* block,       /*!< in: block */
	btr_scrub_page_allocation_status_t allocated, /*!< in: is block
						      allocated or free */
	mtr_t* mtr);              /*!< in: mtr */

/****************************************************************
Perform actual scrubbing of page */
UNIV_INTERN
int
btr_scrub_page(
/*============*/
	btr_scrub_t* scrub_data,  /*!< in/out: scrub data */
	buf_block_t* block,       /*!< in: block */
	btr_scrub_page_allocation_status_t allocated, /*!< in: is block
						      allocated or free */
	mtr_t* mtr);              /*!< in: mtr */

/****************************************************************
Perform cleanup needed for a page not needing scrubbing */
UNIV_INTERN
void
btr_scrub_skip_page(
/*============*/
	btr_scrub_t* scrub_data,  /*!< in/out: scrub data */
	int needs_scrubbing);     /*!< in:  return value from
				  btr_page_needs_scrubbing or
				  btr_scrub_recheck_page which encodes what kind
				  of cleanup is needed */

/****************************************************************
Start iterating a space
* @return true if scrubbing is turned on */
bool btr_scrub_start_space(const fil_space_t &space, btr_scrub_t *scrub_data);

/** Complete iterating a space.
@param[in,out]	scrub_data	 scrub data */
UNIV_INTERN
void
btr_scrub_complete_space(btr_scrub_t* scrub_data);

#endif
