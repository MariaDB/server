/***********************************************************************

Copyright (c) 1995, 2019, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2009, Percona Inc.
Copyright (c) 2013, 2021, MariaDB Corporation.

Portions of this file contain modifications contributed and copyrighted
by Percona Inc.. Those modifications are
gratefully acknowledged and are described briefly in the InnoDB
documentation. The contributions by Percona Inc. are incorporated with
their permission, and subject to the conditions contained in the file
COPYING.Percona.

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

***********************************************************************/

/**************************************************//**
@file os/os0file.cc
The interface to the operating system file i/o primitives

Created 10/21/1995 Heikki Tuuri
*******************************************************/

#ifndef UNIV_INNOCHECKSUM
#include "os0file.h"
#include "sql_const.h"

#ifdef UNIV_LINUX
#include <sys/types.h>
#include <sys/stat.h>
#endif

#include "srv0srv.h"
#include "srv0start.h"
#include "fil0fil.h"
#include "srv0srv.h"
#ifdef HAVE_LINUX_UNISTD_H
#include "unistd.h"
#endif
#include "os0event.h"
#include "os0thread.h"

#include <vector>

#ifdef LINUX_NATIVE_AIO
#include <libaio.h>
#endif /* LINUX_NATIVE_AIO */

#ifdef HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE
# include <fcntl.h>
# include <linux/falloc.h>
#endif /* HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE */

#if defined(UNIV_LINUX) && defined(HAVE_SYS_IOCTL_H)
# include <sys/ioctl.h>
# ifndef DFS_IOCTL_ATOMIC_WRITE_SET
#  define DFS_IOCTL_ATOMIC_WRITE_SET _IOW(0x95, 2, uint)
# endif
#endif

#if defined(UNIV_LINUX) && defined(HAVE_SYS_STATVFS_H)
#include <sys/statvfs.h>
#endif

#if defined(UNIV_LINUX) && defined(HAVE_LINUX_FALLOC_H)
#include <linux/falloc.h>
#endif

#ifdef _WIN32
#include <winioctl.h>
#endif

/** Insert buffer segment id */
static const ulint IO_IBUF_SEGMENT = 0;

/** Log segment id */
static const ulint IO_LOG_SEGMENT = 1;

/** Number of retries for partial I/O's */
static const ulint NUM_RETRIES_ON_PARTIAL_IO = 10;

/* This specifies the file permissions InnoDB uses when it creates files in
Unix; the value of os_innodb_umask is initialized in ha_innodb.cc to
my_umask */

#ifndef _WIN32
/** Umask for creating files */
static ulint	os_innodb_umask = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;
#else
/** Umask for creating files */
static ulint	os_innodb_umask	= 0;
static HANDLE	data_completion_port;
static HANDLE	log_completion_port;

static DWORD	fls_sync_io  = FLS_OUT_OF_INDEXES;
#define IOCP_SHUTDOWN_KEY (ULONG_PTR)-1
#endif /* _WIN32 */

/** In simulated aio, merge at most this many consecutive i/os */
static const ulint	OS_AIO_MERGE_N_CONSECUTIVE = 64;

/** Flag indicating if the page_cleaner is in active state. */
extern bool buf_page_cleaner_is_active;

#ifdef WITH_INNODB_DISALLOW_WRITES
#define WAIT_ALLOW_WRITES() os_event_wait(srv_allow_writes_event)
#else
#define WAIT_ALLOW_WRITES() do { } while (0)
#endif /* WITH_INNODB_DISALLOW_WRITES */

/**********************************************************************

InnoDB AIO Implementation:
=========================

We support native AIO for Windows and Linux. For rest of the platforms
we simulate AIO by special IO-threads servicing the IO-requests.

Simulated AIO:
==============

On platforms where we 'simulate' AIO, the following is a rough explanation
of the high level design.
There are four io-threads (for ibuf, log, read, write).
All synchronous IO requests are serviced by the calling thread using
os_file_write/os_file_read. The Asynchronous requests are queued up
in an array (there are four such arrays) by the calling thread.
Later these requests are picked up by the IO-thread and are serviced
synchronously.

Windows native AIO:
==================

If srv_use_native_aio is not set then Windows follow the same
code as simulated AIO. If the flag is set then native AIO interface
is used. On windows, one of the limitation is that if a file is opened
for AIO no synchronous IO can be done on it. Therefore we have an
extra fifth array to queue up synchronous IO requests.
There are innodb_file_io_threads helper threads. These threads work
on the four arrays mentioned above in Simulated AIO. No thread is
required for the sync array.
If a synchronous IO request is made, it is first queued in the sync
array. Then the calling thread itself waits on the request, thus
making the call synchronous.
If an AIO request is made the calling thread not only queues it in the
array but also submits the requests. The helper thread then collects
the completed IO request and calls completion routine on it.

Linux native AIO:
=================

If we have libaio installed on the system and innodb_use_native_aio
is set to true we follow the code path of native AIO, otherwise we
do simulated AIO.
There are innodb_file_io_threads helper threads. These threads work
on the four arrays mentioned above in Simulated AIO.
If a synchronous IO request is made, it is handled by calling
os_file_write/os_file_read.
If an AIO request is made the calling thread not only queues it in the
array but also submits the requests. The helper thread then collects
the completed IO request and calls completion routine on it.

**********************************************************************/


#ifdef UNIV_PFS_IO
/* Keys to register InnoDB I/O with performance schema */
mysql_pfs_key_t  innodb_data_file_key;
mysql_pfs_key_t  innodb_log_file_key;
mysql_pfs_key_t  innodb_temp_file_key;
#endif /* UNIV_PFS_IO */

class AIO;

/** The asynchronous I/O context */
struct Slot {

#ifdef WIN_ASYNC_IO
	/** Windows control block for the aio request 
	must be at the very start of Slot, so we can
	cast Slot* to OVERLAPPED*
	*/
	OVERLAPPED		control;
#endif

	/** index of the slot in the aio array */
	uint16_t		pos;

	/** true if this slot is reserved */
	bool			is_reserved;

	/** time when reserved */
	time_t			reservation_time;

	/** buffer used in i/o */
	byte*			buf;

	/** Buffer pointer used for actual IO. We advance this
	when partial IO is required and not buf */
	byte*			ptr;

	/** OS_FILE_READ or OS_FILE_WRITE */
	IORequest		type;

	/** file offset in bytes */
	os_offset_t		offset;

	/** file where to read or write */
	pfs_os_file_t		file;

	/** file name or path */
	const char*		name;

	/** used only in simulated aio: true if the physical i/o
	already made and only the slot message needs to be passed
	to the caller of os_aio_simulated_handle */
	bool			io_already_done;

	/*!< file block size */
	ulint			file_block_size;

	/** The file node for which the IO is requested. */
	fil_node_t*		m1;

	/** the requester of an aio operation and which can be used
	to identify which pending aio operation was completed */
	void*			m2;

	/** AIO completion status */
	dberr_t			err;

#ifdef WIN_ASYNC_IO

	/** bytes written/read */
	DWORD			n_bytes;

	/** length of the block to read or write */
	DWORD			len;

	/** aio array containing this slot */
	AIO				*array;
#elif defined(LINUX_NATIVE_AIO)
	/** Linux control block for aio */
	struct iocb		control;

	/** AIO return code */
	int			ret;

	/** bytes written/read. */
	ssize_t			n_bytes;

	/** length of the block to read or write */
	ulint			len;
#else
	/** length of the block to read or write */
	ulint			len;

	/** bytes written/read. */
	ulint			n_bytes;
#endif /* WIN_ASYNC_IO */

	/** Length of the block before it was compressed */
	uint32			original_len;

};

/** The asynchronous i/o array structure */
class AIO {
public:
	/** Constructor
	@param[in]	id		Latch ID
	@param[in]	n_slots		Number of slots to configure
	@param[in]	segments	Number of segments to configure */
	AIO(latch_id_t id, ulint n_slots, ulint segments);

	/** Destructor */
	~AIO();

	/** Initialize the instance
	@return DB_SUCCESS or error code */
	dberr_t init();

	/** Requests for a slot in the aio array. If no slot is available, waits
	until not_full-event becomes signaled.

	@param[in]	type	IO context
	@param[in,out]	m1	message to be passed along with the AIO
				operation
	@param[in,out]	m2	message to be passed along with the AIO
				operation
	@param[in]	file	file handle
	@param[in]	name	name of the file or path as a null-terminated
				string
	@param[in,out]	buf	buffer where to read or from which to write
	@param[in]	offset	file offset, where to read from or start writing
	@param[in]	len	length of the block to read or write
	@return pointer to slot */
	Slot* reserve_slot(
		const IORequest&	type,
		fil_node_t*		m1,
		void*			m2,
		pfs_os_file_t		file,
		const char*		name,
		void*			buf,
		os_offset_t		offset,
		ulint			len)
		MY_ATTRIBUTE((warn_unused_result));

	/** @return number of reserved slots */
	ulint pending_io_count() const;

	/** Returns a pointer to the nth slot in the aio array.
	@param[in]	index	Index of the slot in the array
	@return pointer to slot */
	const Slot* at(ulint i) const
		MY_ATTRIBUTE((warn_unused_result))
	{
		ut_a(i < m_slots.size());

		return(&m_slots[i]);
	}

	/** Non const version */
	Slot* at(ulint i)
		MY_ATTRIBUTE((warn_unused_result))
	{
		ut_a(i < m_slots.size());

		return(&m_slots[i]);
	}

	/** Frees a slot in the AIO array, assumes caller owns the mutex.
	@param[in,out]	slot	Slot to release */
	void release(Slot* slot);

	/** Frees a slot in the AIO array, assumes caller doesn't own the mutex.
	@param[in,out]	slot	Slot to release */
	void release_with_mutex(Slot* slot);

	/** Prints info about the aio array.
	@param[in,out]	file	Where to print */
	void print(FILE* file);

	/** @return the number of slots per segment */
	ulint slots_per_segment() const
		MY_ATTRIBUTE((warn_unused_result))
	{
		return(m_slots.size() / m_n_segments);
	}

	/** @return accessor for n_segments */
	ulint get_n_segments() const
		MY_ATTRIBUTE((warn_unused_result))
	{
		return(m_n_segments);
	}

#ifdef UNIV_DEBUG
	/** @return true if the thread owns the mutex */
	bool is_mutex_owned() const
		MY_ATTRIBUTE((warn_unused_result))
	{
		return(mutex_own(&m_mutex));
	}
#endif /* UNIV_DEBUG */

	/** Acquire the mutex */
	void acquire() const
	{
		mutex_enter(&m_mutex);
	}

	/** Release the mutex */
	void release() const
	{
		mutex_exit(&m_mutex);
	}

	/** Write out the state to the file/stream
	@param[in, out]	file	File to write to */
	void to_file(FILE* file) const;

#ifdef LINUX_NATIVE_AIO
	/** Dispatch an AIO request to the kernel.
	@param[in,out]	slot	an already reserved slot
	@return true on success. */
	bool linux_dispatch(Slot* slot)
		MY_ATTRIBUTE((warn_unused_result));

	/** Accessor for an AIO event
	@param[in]	index	Index into the array
	@return the event at the index */
	io_event* io_events(ulint index)
		MY_ATTRIBUTE((warn_unused_result))
	{
		ut_a(index < m_events.size());

		return(&m_events[index]);
	}

	/** Accessor for the AIO context
	@param[in]	segment	Segment for which to get the context
	@return the AIO context for the segment */
	io_context_t io_ctx(ulint segment)
		MY_ATTRIBUTE((warn_unused_result))
	{
		ut_ad(segment < get_n_segments());

		return(m_aio_ctx[segment]);
	}

	/** Creates an io_context_t for native linux AIO.
	@param[in]	max_events	number of events
	@param[out]	io_ctx		io_ctx to initialize.
	@return true on success. */
	static bool linux_create_io_ctx(unsigned max_events, io_context_t& io_ctx)
		MY_ATTRIBUTE((warn_unused_result));

	/** Checks if the system supports native linux aio. On some kernel
	versions where native aio is supported it won't work on tmpfs. In such
	cases we can't use native aio as it is not possible to mix simulated
	and native aio.
	@return true if supported, false otherwise. */
	static bool is_linux_native_aio_supported()
		MY_ATTRIBUTE((warn_unused_result));
#endif /* LINUX_NATIVE_AIO */

#ifdef WIN_ASYNC_IO
	HANDLE m_completion_port;
	/** Wake up all AIO threads in Windows native aio */
	static void wake_at_shutdown() {
		AIO *all_arrays[] = {s_reads, s_writes, s_log, s_ibuf };
		for (size_t i = 0; i < array_elements(all_arrays); i++) {
			AIO *a = all_arrays[i];
			if (a) {
				PostQueuedCompletionStatus(a->m_completion_port, 0,
					IOCP_SHUTDOWN_KEY, 0);
			}
		}
	}
#endif /* WIN_ASYNC_IO */

#ifdef _WIN32
	/** This function can be called if one wants to post a batch of reads
	and prefers an I/O - handler thread to handle them all at once later.You
	must call os_aio_simulated_wake_handler_threads later to ensure the
	threads are not left sleeping! */
	static void simulated_put_read_threads_to_sleep();
#endif /* _WIN32 */

	/** Create an instance using new(std::nothrow)
	@param[in]	id		Latch ID
	@param[in]	n_slots		The number of AIO request slots
	@param[in]	segments	The number of segments
	@return a new AIO instance */
	static AIO* create(
		latch_id_t	id,
		ulint		n_slots,
		ulint		segments)
		MY_ATTRIBUTE((warn_unused_result));

	/** Initializes the asynchronous io system. Creates one array each
	for ibuf and log I/O. Also creates one array each for read and write
	where each array is divided logically into n_readers and n_writers
	respectively. The caller must create an i/o handler thread for each
	segment in these arrays. This function also creates the sync array.
	No I/O handler thread needs to be created for that
	@param[in]	n_per_seg	maximum number of pending aio
					operations allowed per segment
	@param[in]	n_readers	number of reader threads
	@param[in]	n_writers	number of writer threads
	@param[in]	n_slots_sync	number of slots in the sync aio array
	@return true if AIO sub-system was started successfully */
	static bool start(
		ulint		n_per_seg,
		ulint		n_readers,
		ulint		n_writers,
		ulint		n_slots_sync)
		MY_ATTRIBUTE((warn_unused_result));

	/** Free the AIO arrays */
	static void shutdown();

	/** Print all the AIO segments
	@param[in,out]	file		Where to print */
	static void print_all(FILE* file);

	/** Calculates local segment number and aio array from global
	segment number.
	@param[out]	array		AIO wait array
	@param[in]	segment		global segment number
	@return local segment number within the aio array */
	static ulint get_array_and_local_segment(
		AIO**		array,
		ulint		segment)
		MY_ATTRIBUTE((warn_unused_result));

	/** Select the IO slot array
	@param[in,out]	type		Type of IO, READ or WRITE
	@param[in]	read_only	true if running in read-only mode
	@param[in]	mode		IO mode
	@return slot array or NULL if invalid mode specified */
	static AIO* select_slot_array(
		IORequest&		type,
		bool			read_only,
		ulint			mode)
		MY_ATTRIBUTE((warn_unused_result));

	/** Calculates segment number for a slot.
	@param[in]	array		AIO wait array
	@param[in]	slot		slot in this array
	@return segment number (which is the number used by, for example,
		I/O handler threads) */
	static ulint get_segment_no_from_slot(
		const AIO*	array,
		const Slot*	slot)
		MY_ATTRIBUTE((warn_unused_result));

	/** Wakes up a simulated AIO I/O-handler thread if it has something
	to do.
	@param[in]	global_segment	the number of the segment in the
					AIO arrays */
	static void wake_simulated_handler_thread(ulint global_segment);

	/** Check if it is a read request
	@param[in]	aio		The AIO instance to check
	@return true if the AIO instance is for reading. */
	static bool is_read(const AIO* aio)
		MY_ATTRIBUTE((warn_unused_result))
	{
		return(s_reads == aio);
	}

	/** Wait on an event until no pending writes */
	static void wait_until_no_pending_writes()
	{
		os_event_wait(AIO::s_writes->m_is_empty);
	}

	/** Print to file
	@param[in]	file		File to write to */
	static void print_to_file(FILE* file);

	/** Check for pending IO. Gets the count and also validates the
	data structures.
	@return count of pending IO requests */
	static ulint total_pending_io_count();

private:
	/** Initialise the slots
	@return DB_SUCCESS or error code */
	dberr_t init_slots()
		MY_ATTRIBUTE((warn_unused_result));

	/** Wakes up a simulated AIO I/O-handler thread if it has something
	to do for a local segment in the AIO array.
	@param[in]	global_segment	the number of the segment in the
					AIO arrays
	@param[in]	segment		the local segment in the AIO array */
	void wake_simulated_handler_thread(ulint global_segment, ulint segment);

	/** Prints pending IO requests per segment of an aio array.
	We probably don't need per segment statistics but they can help us
	during development phase to see if the IO requests are being
	distributed as expected.
	@param[in,out]	file		file where to print
	@param[in]	segments	pending IO array */
	void print_segment_info(
		FILE*		file,
		const ulint*	segments);

#ifdef LINUX_NATIVE_AIO
	/** Initialise the Linux native AIO data structures
	@return DB_SUCCESS or error code */
	dberr_t init_linux_native_aio()
		MY_ATTRIBUTE((warn_unused_result));
#endif /* LINUX_NATIVE_AIO */

private:
	typedef std::vector<Slot> Slots;

	/** the mutex protecting the aio array */
	mutable SysMutex	m_mutex;

	/** Pointer to the slots in the array.
	Number of elements must be divisible by n_threads. */
	Slots			m_slots;

	/** Number of segments in the aio array of pending aio requests.
	A thread can wait separately for any one of the segments. */
	ulint			m_n_segments;

	/** The event which is set to the signaled state when
	there is space in the aio outside the ibuf segment;
	os_event_set() and os_event_reset() are protected by AIO::m_mutex */
	os_event_t		m_not_full;

	/** The event which is set to the signaled state when
	there are no pending i/os in this array;
	os_event_set() and os_event_reset() are protected by AIO::m_mutex */
	os_event_t		m_is_empty;

	/** Number of reserved slots in the AIO array outside
	the ibuf segment */
	ulint			m_n_reserved;


#if defined(LINUX_NATIVE_AIO)
	typedef std::vector<io_event> IOEvents;

	/** completion queue for IO. There is one such queue per
	segment. Each thread will work on one ctx exclusively. */
	std::vector<io_context_t>		m_aio_ctx;

	/** The array to collect completed IOs. There is one such
	event for each possible pending IO. The size of the array
	is equal to m_slots.size(). */
	IOEvents		m_events;
#endif /* LINUX_NATIV_AIO */

	/** The aio arrays for non-ibuf i/o and ibuf i/o, as well as
	sync AIO. These are NULL when the module has not yet been
	initialized. */

	/** Insert buffer */
	static AIO*		s_ibuf;

	/** Redo log */
	static AIO*		s_log;

	/** Reads */
	static AIO*		s_reads;

	/** Writes */
	static AIO*		s_writes;

	/** Synchronous I/O */
	static AIO*		s_sync;
};

/** Static declarations */
AIO*	AIO::s_reads;
AIO*	AIO::s_writes;
AIO*	AIO::s_ibuf;
AIO*	AIO::s_log;
AIO*	AIO::s_sync;

#if defined(LINUX_NATIVE_AIO)
/** timeout for each io_getevents() call = 500ms. */
static const ulint	OS_AIO_REAP_TIMEOUT = 500000000UL;

/** time to sleep, in microseconds if io_setup() returns EAGAIN. */
static const ulint	OS_AIO_IO_SETUP_RETRY_SLEEP = 500000UL;

/** number of attempts before giving up on io_setup(). */
static const int	OS_AIO_IO_SETUP_RETRY_ATTEMPTS = 5;
#endif /* LINUX_NATIVE_AIO */

/** Array of events used in simulated AIO */
static os_event_t*	os_aio_segment_wait_events;

/** Number of asynchronous I/O segments.  Set by os_aio_init(). */
static ulint		os_aio_n_segments = ULINT_UNDEFINED;

/** If the following is true, read i/o handler threads try to
wait until a batch of new read requests have been posted */
static bool		os_aio_recommend_sleep_for_read_threads;

ulint	os_n_file_reads;
static ulint	os_bytes_read_since_printout;
ulint	os_n_file_writes;
ulint	os_n_fsyncs;
static ulint	os_n_file_reads_old;
static ulint	os_n_file_writes_old;
static ulint	os_n_fsyncs_old;

static time_t	os_last_printout;
bool	os_has_said_disk_full;

/** Default Zip compression level */
extern uint page_zip_level;

#if DATA_TRX_ID_LEN > 6
#error "COMPRESSION_ALGORITHM will not fit"
#endif /* DATA_TRX_ID_LEN */

/** Validates the consistency of the aio system.
@return true if ok */
static
bool
os_aio_validate();

/** Handle errors for file operations.
@param[in]	name		name of a file or NULL
@param[in]	operation	operation
@param[in]	should_abort	whether to abort on an unknown error
@param[in]	on_error_silent	whether to suppress reports of non-fatal errors
@return true if we should retry the operation */
static MY_ATTRIBUTE((warn_unused_result))
bool
os_file_handle_error_cond_exit(
	const char*	name,
	const char*	operation,
	bool		should_abort,
	bool		on_error_silent);

/** Does error handling when a file operation fails.
@param[in]	name		name of a file or NULL
@param[in]	operation	operation name that failed
@return true if we should retry the operation */
static
bool
os_file_handle_error(
	const char*	name,
	const char*	operation)
{
	/* Exit in case of unknown error */
	return(os_file_handle_error_cond_exit(name, operation, true, false));
}

/** Does error handling when a file operation fails.
@param[in]	name		name of a file or NULL
@param[in]	operation	operation name that failed
@param[in]	on_error_silent	if true then don't print any message to the log.
@return true if we should retry the operation */
static
bool
os_file_handle_error_no_exit(
	const char*	name,
	const char*	operation,
	bool		on_error_silent)
{
	/* Don't exit in case of unknown error */
	return(os_file_handle_error_cond_exit(
			name, operation, false, on_error_silent));
}

/** Handle RENAME error.
@param name	old name of the file
@param new_name	new name of the file */
static void os_file_handle_rename_error(const char* name, const char* new_name)
{
	if (os_file_get_last_error(true) != OS_FILE_DISK_FULL) {
		ib::error() << "Cannot rename file '" << name << "' to '"
			<< new_name << "'";
	} else if (!os_has_said_disk_full) {
		os_has_said_disk_full = true;
		/* Disk full error is reported irrespective of the
		on_error_silent setting. */
		ib::error() << "Full disk prevents renaming file '"
			<< name << "' to '" << new_name << "'";
	}
}

/** Does simulated AIO. This function should be called by an i/o-handler
thread.

@param[in]	segment	The number of the segment in the aio arrays to wait
			for; segment 0 is the ibuf i/o thread, segment 1 the
			log i/o thread, then follow the non-ibuf read threads,
			and as the last are the non-ibuf write threads
@param[out]	m1	the messages passed with the AIO request; note that
			also in the case where the AIO operation failed, these
			output parameters are valid and can be used to restart
			the operation, for example
@param[out]	m2	Callback argument
@param[in]	type	IO context
@return DB_SUCCESS or error code */
static
dberr_t
os_aio_simulated_handler(
	ulint		global_segment,
	fil_node_t**	m1,
	void**		m2,
	IORequest*	type);

#ifdef _WIN32
static HANDLE win_get_syncio_event();

/**
 Wrapper around Windows DeviceIoControl() function.

 Works synchronously, also in case for handle opened
 for async access (i.e with FILE_FLAG_OVERLAPPED).

 Accepts the same parameters as DeviceIoControl(),except
 last parameter (OVERLAPPED).
*/
static
BOOL
os_win32_device_io_control(
	HANDLE handle,
	DWORD code,
	LPVOID inbuf,
	DWORD inbuf_size,
	LPVOID outbuf,
	DWORD outbuf_size,
	LPDWORD bytes_returned
)
{
	OVERLAPPED overlapped = { 0 };
	overlapped.hEvent = win_get_syncio_event();
	BOOL result = DeviceIoControl(handle, code, inbuf, inbuf_size, outbuf,
		outbuf_size,  NULL, &overlapped);

	if (result || (GetLastError() == ERROR_IO_PENDING)) {
		/* Wait for async io to complete */
		result = GetOverlappedResult(handle, &overlapped, bytes_returned, TRUE);
	}

	return result;
}

#endif

/***********************************************************************//**
Try to get number of bytes per sector from file system.
@return	file block size */
UNIV_INTERN
ulint
os_file_get_block_size(
/*===================*/
	os_file_t	file,	/*!< in: handle to a file */
	const char*	name)	/*!< in: file name */
{
	ulint		fblock_size = 512;

#if defined(UNIV_LINUX)
	struct stat local_stat;
	int		err;

	err = fstat((int)file, &local_stat);

	if (err != 0) {
		os_file_handle_error_no_exit(name, "fstat()", FALSE);
	} else {
		fblock_size = local_stat.st_blksize;
	}
#endif /* UNIV_LINUX */
#ifdef _WIN32

	fblock_size = 0;

	// Open volume for this file, find out it "physical bytes per sector"

	HANDLE volume_handle = INVALID_HANDLE_VALUE;
	char volume[MAX_PATH + 4]="\\\\.\\"; // Special prefix required for volume names.
	if (!GetVolumePathName(name , volume + 4, MAX_PATH)) {
		os_file_handle_error_no_exit(name,
			"GetVolumePathName()", FALSE);
		goto end;
	}

	size_t len = strlen(volume);
	if (volume[len - 1] == '\\') {
		// Trim trailing backslash from volume name.
		volume[len - 1] = 0;
	}

	volume_handle = CreateFile(volume, FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		0, OPEN_EXISTING, 0, 0);

	if (volume_handle == INVALID_HANDLE_VALUE) {
		if (GetLastError() != ERROR_ACCESS_DENIED) {
			os_file_handle_error_no_exit(volume,
				"CreateFile()", FALSE);
		}
		goto end;
	}

	DWORD tmp;
	STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR disk_alignment;

	STORAGE_PROPERTY_QUERY storage_query;
	memset(&storage_query, 0, sizeof(storage_query));
	storage_query.PropertyId = StorageAccessAlignmentProperty;
	storage_query.QueryType  = PropertyStandardQuery;

	BOOL result = os_win32_device_io_control(volume_handle,
		IOCTL_STORAGE_QUERY_PROPERTY,
		&storage_query,
		sizeof(storage_query),
		&disk_alignment,
		sizeof(disk_alignment),
		&tmp);

	if (!result) {
		DWORD err = GetLastError();
		if (err != ERROR_INVALID_FUNCTION && err != ERROR_NOT_SUPPORTED) {
				os_file_handle_error_no_exit(volume,
					"DeviceIoControl(IOCTL_STORAGE_QUERY_PROPERTY)", FALSE);
		}
		goto end;
	}

	fblock_size = disk_alignment.BytesPerPhysicalSector;

end:
	if (volume_handle != INVALID_HANDLE_VALUE) {
		CloseHandle(volume_handle);
	}
#endif /* _WIN32 */

	/* Currently we support file block size up to 4Kb */
	if (fblock_size > 4096 || fblock_size < 512) {
		if (fblock_size < 512) {
			fblock_size = 512;
		} else {
			fblock_size = 4096;
		}
	}

	return fblock_size;
}

#ifdef WIN_ASYNC_IO
/** This function is only used in Windows asynchronous i/o.
Waits for an aio operation to complete. This function is used to wait the
for completed requests. The aio array of pending requests is divided
into segments. The thread specifies which segment or slot it wants to wait
for. NOTE: this function will also take care of freeing the aio slot,
therefore no other thread is allowed to do the freeing!
@param[in]	segment		The number of the segment in the aio arrays to
wait for; segment 0 is the ibuf I/O thread,
segment 1 the log I/O thread, then follow the
non-ibuf read threads, and as the last are the
non-ibuf write threads; if this is
ULINT_UNDEFINED, then it means that sync AIO
is used, and this parameter is ignored
@param[in]	pos		this parameter is used only in sync AIO:
wait for the aio slot at this position
@param[out]	m1		the messages passed with the AIO request; note
that also in the case where the AIO operation
failed, these output parameters are valid and
can be used to restart the operation,
for example
@param[out]	m2		callback message
@param[out]	type		OS_FILE_WRITE or ..._READ
@return DB_SUCCESS or error code */
static
dberr_t
os_aio_windows_handler(
	ulint		segment,
	ulint		pos,
	fil_node_t**	m1,
	void**		m2,
	IORequest*	type);
#endif /* WIN_ASYNC_IO */

/** Generic AIO Handler methods. Currently handles IO post processing. */
class AIOHandler {
public:
	/** Do any post processing after a read/write
	@return DB_SUCCESS or error code. */
	static dberr_t post_io_processing(Slot* slot);
};

/** Helper class for doing synchronous file IO. Currently, the objective
is to hide the OS specific code, so that the higher level functions aren't
peppered with #ifdef. Makes the code flow difficult to follow.  */
class SyncFileIO {
public:
	/** Constructor
	@param[in]	fh	File handle
	@param[in,out]	buf	Buffer to read/write
	@param[in]	n	Number of bytes to read/write
	@param[in]	offset	Offset where to read or write */
	SyncFileIO(os_file_t fh, void* buf, ulint n, os_offset_t offset)
		:
		m_fh(fh),
		m_buf(buf),
		m_n(static_cast<ssize_t>(n)),
		m_offset(offset)
	{
		ut_ad(m_n > 0);
	}

	/** Destructor */
	~SyncFileIO()
	{
		/* No op */
	}

	/** Do the read/write
	@param[in]	request	The IO context and type
	@return the number of bytes read/written or negative value on error */
	ssize_t execute(const IORequest& request);

	/** Do the read/write
	@param[in,out]	slot	The IO slot, it has the IO context
	@return the number of bytes read/written or negative value on error */
	static ssize_t execute(Slot* slot);

	/** Move the read/write offset up to where the partial IO succeeded.
	@param[in]	n_bytes	The number of bytes to advance */
	void advance(ssize_t n_bytes)
	{
		m_offset += n_bytes;

		ut_ad(m_n >= n_bytes);

		m_n -=  n_bytes;

		m_buf = reinterpret_cast<uchar*>(m_buf) + n_bytes;
	}

private:
	/** Open file handle */
	os_file_t		m_fh;

	/** Buffer to read/write */
	void*			m_buf;

	/** Number of bytes to read/write */
	ssize_t			m_n;

	/** Offset from where to read/write */
	os_offset_t		m_offset;
};

/** Do any post processing after a read/write
@return DB_SUCCESS or error code. */
dberr_t
AIOHandler::post_io_processing(Slot* slot)
{
	ut_ad(slot->is_reserved);

	/* Total bytes read so far */
	ulint	n_bytes = (slot->ptr - slot->buf) + slot->n_bytes;

	return(n_bytes == slot->original_len ? DB_SUCCESS : DB_FAIL);
}

/** Count the number of free slots
@return number of reserved slots */
ulint
AIO::pending_io_count() const
{
	acquire();

#ifdef UNIV_DEBUG
	ut_a(m_n_segments > 0);
	ut_a(!m_slots.empty());

	ulint	count = 0;

	for (ulint i = 0; i < m_slots.size(); ++i) {

		const Slot&	slot = m_slots[i];

		if (slot.is_reserved) {
			++count;
			ut_a(slot.len > 0);
		}
	}

	ut_a(m_n_reserved == count);
#endif /* UNIV_DEBUG */

	ulint	reserved = m_n_reserved;

	release();

	return(reserved);
}

#ifdef UNIV_DEBUG
/** Validates the consistency the aio system some of the time.
@return true if ok or the check was skipped */
static
bool
os_aio_validate_skip()
{
/** Try os_aio_validate() every this many times */
# define OS_AIO_VALIDATE_SKIP	13

	/** The os_aio_validate() call skip counter.
	Use a signed type because of the race condition below. */
	static int os_aio_validate_count = OS_AIO_VALIDATE_SKIP;

	/* There is a race condition below, but it does not matter,
	because this call is only for heuristic purposes. We want to
	reduce the call frequency of the costly os_aio_validate()
	check in debug builds. */
	--os_aio_validate_count;

	if (os_aio_validate_count > 0) {
		return(true);
	}

	os_aio_validate_count = OS_AIO_VALIDATE_SKIP;
	return(os_aio_validate());
}
#endif /* UNIV_DEBUG */

#undef USE_FILE_LOCK
#ifndef _WIN32
/* On Windows, mandatory locking is used */
# define USE_FILE_LOCK
#endif
#ifdef USE_FILE_LOCK
/** Obtain an exclusive lock on a file.
@param[in]	fd		file descriptor
@param[in]	name		file name
@return 0 on success */
static
int
os_file_lock(
	int		fd,
	const char*	name)
{
	if (my_disable_locking) {
		return 0;
	}

	struct flock lk;

	lk.l_type = F_WRLCK;
	lk.l_whence = SEEK_SET;
	lk.l_start = lk.l_len = 0;

	if (fcntl(fd, F_SETLK, &lk) == -1) {

		ib::error()
			<< "Unable to lock " << name
			<< " error: " << errno;

		if (errno == EAGAIN || errno == EACCES) {

			ib::info()
				<< "Check that you do not already have"
				" another mysqld process using the"
				" same InnoDB data or log files.";
		}

		return(-1);
	}

	return(0);
}
#endif /* USE_FILE_LOCK */

/** Calculates local segment number and aio array from global segment number.
@param[out]	array		aio wait array
@param[in]	segment		global segment number
@return local segment number within the aio array */
ulint
AIO::get_array_and_local_segment(
	AIO**		array,
	ulint		segment)
{
	ulint		local_segment;
	ulint		n_extra_segs = (srv_read_only_mode) ? 0 : 2;

	ut_a(segment < os_aio_n_segments);

	if (!srv_read_only_mode && segment < n_extra_segs) {

		/* We don't support ibuf/log IO during read only mode. */

		if (segment == IO_IBUF_SEGMENT) {

			*array = s_ibuf;

		} else if (segment == IO_LOG_SEGMENT) {

			*array = s_log;

		} else {
			*array = NULL;
		}

		local_segment = 0;

	} else if (segment < s_reads->m_n_segments + n_extra_segs) {

		*array = s_reads;
		local_segment = segment - n_extra_segs;

	} else {
		*array = s_writes;

		local_segment = segment
			      - (s_reads->m_n_segments + n_extra_segs);
	}

	return(local_segment);
}

/** Frees a slot in the aio array. Assumes caller owns the mutex.
@param[in,out]	slot		Slot to release */
void
AIO::release(Slot* slot)
{
	ut_ad(is_mutex_owned());

	ut_ad(slot->is_reserved);

	slot->is_reserved = false;

	--m_n_reserved;

	if (m_n_reserved == m_slots.size() - 1) {
		os_event_set(m_not_full);
	}

	if (m_n_reserved == 0) {
		os_event_set(m_is_empty);
	}

#if defined(LINUX_NATIVE_AIO)

	if (srv_use_native_aio) {
		memset(&slot->control, 0x0, sizeof(slot->control));
		slot->ret = 0;
		slot->n_bytes = 0;
	} else {
		/* These fields should not be used if we are not
		using native AIO. */
		ut_ad(slot->n_bytes == 0);
		ut_ad(slot->ret == 0);
	}

#endif /* WIN_ASYNC_IO */
}

/** Frees a slot in the AIO array. Assumes caller doesn't own the mutex.
@param[in,out]	slot		Slot to release */
void
AIO::release_with_mutex(Slot* slot)
{
	acquire();

	release(slot);

	release();
}

/** Creates a temporary file.  This function is like tmpfile(3), but
the temporary file is created in the given parameter path. If the path
is NULL then it will create the file in the MySQL server configuration
parameter (--tmpdir).
@param[in]	path	location for creating temporary file
@@return temporary file handle, or NULL on error */
FILE*
os_file_create_tmpfile(
	const char*	path)
{
	FILE*	file	= NULL;
	WAIT_ALLOW_WRITES();
	int	fd	= innobase_mysql_tmpfile(path);

	if (fd >= 0) {
		file = fdopen(fd, "w+b");
	}

	if (file == NULL) {

		ib::error()
			<< "Unable to create temporary file; errno: "
			<< errno;

		if (fd >= 0) {
			close(fd);
		}
	}

	return(file);
}

/** Rewind file to its start, read at most size - 1 bytes from it to str, and
NUL-terminate str. All errors are silently ignored. This function is
mostly meant to be used with temporary files.
@param[in,out]	file		File to read from
@param[in,out]	str		Buffer where to read
@param[in]	size		Size of buffer */
void
os_file_read_string(
	FILE*		file,
	char*		str,
	ulint		size)
{
	if (size != 0) {
		rewind(file);

		size_t	flen = fread(str, 1, size - 1, file);

		str[flen] = '\0';
	}
}

/** This function returns a new path name after replacing the basename
in an old path with a new basename.  The old_path is a full path
name including the extension.  The tablename is in the normal
form "databasename/tablename".  The new base name is found after
the forward slash.  Both input strings are null terminated.

This function allocates memory to be returned.  It is the callers
responsibility to free the return value after it is no longer needed.

@param[in]	old_path		Pathname
@param[in]	tablename		Contains new base name
@return own: new full pathname */
char*
os_file_make_new_pathname(
	const char*	old_path,
	const char*	tablename)
{
	ulint		dir_len;
	char*		last_slash;
	char*		base_name;
	char*		new_path;
	ulint		new_path_len;

	/* Split the tablename into its database and table name components.
	They are separated by a '/'. */
	last_slash = strrchr((char*) tablename, '/');
	base_name = last_slash ? last_slash + 1 : (char*) tablename;

	/* Find the offset of the last slash. We will strip off the
	old basename.ibd which starts after that slash. */
	last_slash = strrchr((char*) old_path, OS_PATH_SEPARATOR);
	dir_len = last_slash ? last_slash - old_path : strlen(old_path);

	/* allocate a new path and move the old directory path to it. */
	new_path_len = dir_len + strlen(base_name) + sizeof "/.ibd";
	new_path = static_cast<char*>(ut_malloc_nokey(new_path_len));
	memcpy(new_path, old_path, dir_len);

	snprintf(new_path + dir_len, new_path_len - dir_len,
		 "%c%s.ibd", OS_PATH_SEPARATOR, base_name);

	return(new_path);
}

/** This function reduces a null-terminated full remote path name into
the path that is sent by MySQL for DATA DIRECTORY clause.  It replaces
the 'databasename/tablename.ibd' found at the end of the path with just
'tablename'.

Since the result is always smaller than the path sent in, no new memory
is allocated. The caller should allocate memory for the path sent in.
This function manipulates that path in place.

If the path format is not as expected, just return.  The result is used
to inform a SHOW CREATE TABLE command.
@param[in,out]	data_dir_path		Full path/data_dir_path */
void
os_file_make_data_dir_path(
	char*	data_dir_path)
{
	/* Replace the period before the extension with a null byte. */
	char*	ptr = strrchr((char*) data_dir_path, '.');

	if (ptr == NULL) {
		return;
	}

	ptr[0] = '\0';

	/* The tablename starts after the last slash. */
	ptr = strrchr((char*) data_dir_path, OS_PATH_SEPARATOR);

	if (ptr == NULL) {
		return;
	}

	ptr[0] = '\0';

	char*	tablename = ptr + 1;

	/* The databasename starts after the next to last slash. */
	ptr = strrchr((char*) data_dir_path, OS_PATH_SEPARATOR);

	if (ptr == NULL) {
		return;
	}

	ulint	tablename_len = ut_strlen(tablename);

	ut_memmove(++ptr, tablename, tablename_len);

	ptr[tablename_len] = '\0';
}

/** Check if the path refers to the root of a drive using a pointer
to the last directory separator that the caller has fixed.
@param[in]	path	path name
@param[in]	path	last directory separator in the path
@return true if this path is a drive root, false if not */
UNIV_INLINE
bool
os_file_is_root(
	const char*	path,
	const char*	last_slash)
{
	return(
#ifdef _WIN32
	       (last_slash == path + 2 && path[1] == ':') ||
#endif /* _WIN32 */
	       last_slash == path);
}

/** Return the parent directory component of a null-terminated path.
Return a new buffer containing the string up to, but not including,
the final component of the path.
The path returned will not contain a trailing separator.
Do not return a root path, return NULL instead.
The final component trimmed off may be a filename or a directory name.
If the final component is the only component of the path, return NULL.
It is the caller's responsibility to free the returned string after it
is no longer needed.
@param[in]	path		Path name
@return own: parent directory of the path */
static
char*
os_file_get_parent_dir(
	const char*	path)
{
	bool	has_trailing_slash = false;

	/* Find the offset of the last slash */
	const char* last_slash = strrchr(path, OS_PATH_SEPARATOR);

	if (!last_slash) {
		/* No slash in the path, return NULL */
		return(NULL);
	}

	/* Ok, there is a slash. Is there anything after it? */
	if (static_cast<size_t>(last_slash - path + 1) == strlen(path)) {
		has_trailing_slash = true;
	}

	/* Reduce repetative slashes. */
	while (last_slash > path
		&& last_slash[-1] == OS_PATH_SEPARATOR) {
		last_slash--;
	}

	/* Check for the root of a drive. */
	if (os_file_is_root(path, last_slash)) {
		return(NULL);
	}

	/* If a trailing slash prevented the first strrchr() from trimming
	the last component of the path, trim that component now. */
	if (has_trailing_slash) {
		/* Back up to the previous slash. */
		last_slash--;
		while (last_slash > path
		       && last_slash[0] != OS_PATH_SEPARATOR) {
			last_slash--;
		}

		/* Reduce repetative slashes. */
		while (last_slash > path
			&& last_slash[-1] == OS_PATH_SEPARATOR) {
			last_slash--;
		}
	}

	/* Check for the root of a drive. */
	if (os_file_is_root(path, last_slash)) {
		return(NULL);
	}

	if (last_slash - path < 0) {
		/* Sanity check, it prevents gcc from trying to handle this case which
		 * results in warnings for some optimized builds */
		return (NULL);
	}

	/* Non-trivial directory component */

	return(mem_strdupl(path, last_slash - path));
}
#ifdef UNIV_ENABLE_UNIT_TEST_GET_PARENT_DIR

/* Test the function os_file_get_parent_dir. */
void
test_os_file_get_parent_dir(
	const char*	child_dir,
	const char*	expected_dir)
{
	char* child = mem_strdup(child_dir);
	char* expected = expected_dir == NULL ? NULL
			 : mem_strdup(expected_dir);

	/* os_file_get_parent_dir() assumes that separators are
	converted to OS_PATH_SEPARATOR. */
	os_normalize_path(child);
	os_normalize_path(expected);

	char* parent = os_file_get_parent_dir(child);

	bool unexpected = (expected == NULL
			  ? (parent != NULL)
			  : (0 != strcmp(parent, expected)));
	if (unexpected) {
		ib::fatal() << "os_file_get_parent_dir('" << child
			<< "') returned '" << parent
			<< "', instead of '" << expected << "'.";
	}
	ut_free(parent);
	ut_free(child);
	ut_free(expected);
}

/* Test the function os_file_get_parent_dir. */
void
unit_test_os_file_get_parent_dir()
{
	test_os_file_get_parent_dir("/usr/lib/a", "/usr/lib");
	test_os_file_get_parent_dir("/usr/", NULL);
	test_os_file_get_parent_dir("//usr//", NULL);
	test_os_file_get_parent_dir("usr", NULL);
	test_os_file_get_parent_dir("usr//", NULL);
	test_os_file_get_parent_dir("/", NULL);
	test_os_file_get_parent_dir("//", NULL);
	test_os_file_get_parent_dir(".", NULL);
	test_os_file_get_parent_dir("..", NULL);
# ifdef _WIN32
	test_os_file_get_parent_dir("D:", NULL);
	test_os_file_get_parent_dir("D:/", NULL);
	test_os_file_get_parent_dir("D:\\", NULL);
	test_os_file_get_parent_dir("D:/data", NULL);
	test_os_file_get_parent_dir("D:/data/", NULL);
	test_os_file_get_parent_dir("D:\\data\\", NULL);
	test_os_file_get_parent_dir("D:///data/////", NULL);
	test_os_file_get_parent_dir("D:\\\\\\data\\\\\\\\", NULL);
	test_os_file_get_parent_dir("D:/data//a", "D:/data");
	test_os_file_get_parent_dir("D:\\data\\\\a", "D:\\data");
	test_os_file_get_parent_dir("D:///data//a///b/", "D:///data//a");
	test_os_file_get_parent_dir("D:\\\\\\data\\\\a\\\\\\b\\", "D:\\\\\\data\\\\a");
#endif  /* _WIN32 */
}
#endif /* UNIV_ENABLE_UNIT_TEST_GET_PARENT_DIR */


/** Creates all missing subdirectories along the given path.
@param[in]	path		Path name
@return DB_SUCCESS if OK, otherwise error code. */
dberr_t
os_file_create_subdirs_if_needed(
	const char*	path)
{
	if (srv_read_only_mode) {

		ib::error()
			<< "read only mode set. Can't create "
			<< "subdirectories '" << path << "'";

		return(DB_READ_ONLY);

	}

	char*	subdir = os_file_get_parent_dir(path);

	if (subdir == NULL) {
		/* subdir is root or cwd, nothing to do */
		return(DB_SUCCESS);
	}

	/* Test if subdir exists */
	os_file_type_t	type;
	bool	subdir_exists;
	bool	success = os_file_status(subdir, &subdir_exists, &type);

	if (success && !subdir_exists) {

		/* Subdir does not exist, create it */
		dberr_t	err = os_file_create_subdirs_if_needed(subdir);

		if (err != DB_SUCCESS) {

			ut_free(subdir);

			return(err);
		}

		success = os_file_create_directory(subdir, false);
	}

	ut_free(subdir);

	return(success ? DB_SUCCESS : DB_ERROR);
}

#ifndef _WIN32

/** Do the read/write
@param[in]	request	The IO context and type
@return the number of bytes read/written or negative value on error */
ssize_t
SyncFileIO::execute(const IORequest& request)
{
	ssize_t	n_bytes;

	if (request.is_read()) {
		n_bytes = pread(m_fh, m_buf, m_n, m_offset);
	} else {
		ut_ad(request.is_write());
		n_bytes = pwrite(m_fh, m_buf, m_n, m_offset);
	}

	return(n_bytes);
}
/** Free storage space associated with a section of the file.
@param[in]	fh		Open file handle
@param[in]	off		Starting offset (SEEK_SET)
@param[in]	len		Size of the hole
@return DB_SUCCESS or error code */
static
dberr_t
os_file_punch_hole_posix(
	os_file_t	fh,
	os_offset_t	off,
	os_offset_t	len)
{

#ifdef HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE
	const int	mode = FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE;

	int		ret = fallocate(fh, mode, off, len);

	if (ret == 0) {
		return(DB_SUCCESS);
	}

	if (errno == ENOTSUP) {
		return(DB_IO_NO_PUNCH_HOLE);
	}

	ib::warn()
		<< "fallocate("
		<<", FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, "
		<< off << ", " << len << ") returned errno: "
		<<  errno;

	return(DB_IO_ERROR);

#elif defined(UNIV_SOLARIS)

	// Use F_FREESP

#endif /* HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE */

	return(DB_IO_NO_PUNCH_HOLE);
}

#if defined(LINUX_NATIVE_AIO)

/** Linux native AIO handler */
class LinuxAIOHandler {
public:
	/**
	@param[in] global_segment	The global segment*/
	LinuxAIOHandler(ulint global_segment)
		:
		m_global_segment(global_segment)
	{
		/* Should never be doing Sync IO here. */
		ut_a(m_global_segment != ULINT_UNDEFINED);

		/* Find the array and the local segment. */

		m_segment = AIO::get_array_and_local_segment(
			&m_array, m_global_segment);

		m_n_slots = m_array->slots_per_segment();
	}

	/** Destructor */
	~LinuxAIOHandler()
	{
		// No op
	}

	/**
	Process a Linux AIO request
	@param[out]	m1		the messages passed with the
	@param[out]	m2		AIO request; note that in case the
					AIO operation failed, these output
					parameters are valid and can be used to
					restart the operation.
	@param[out]	request		IO context
	@return DB_SUCCESS or error code */
	dberr_t poll(fil_node_t** m1, void** m2, IORequest* request);

private:
	/** Resubmit an IO request that was only partially successful
	@param[in,out]	slot		Request to resubmit
	@return DB_SUCCESS or DB_FAIL if the IO resubmit request failed */
	dberr_t	resubmit(Slot* slot);

	/** Check if the AIO succeeded
	@param[in,out]	slot		The slot to check
	@return DB_SUCCESS, DB_FAIL if the operation should be retried or
		DB_IO_ERROR on all other errors */
	dberr_t	check_state(Slot* slot);

	/** @return true if a shutdown was detected */
	bool is_shutdown() const
	{
		return(srv_shutdown_state == SRV_SHUTDOWN_EXIT_THREADS
		       && !buf_page_cleaner_is_active);
	}

	/** If no slot was found then the m_array->m_mutex will be released.
	@param[out]	n_pending	The number of pending IOs
	@return NULL or a slot that has completed IO */
	Slot* find_completed_slot(ulint* n_pending);

	/** This is called from within the IO-thread. If there are no completed
	IO requests in the slot array, the thread calls this function to
	collect more requests from the Linux kernel.
	The IO-thread waits on io_getevents(), which is a blocking call, with
	a timeout value. Unless the system is very heavy loaded, keeping the
	IO-thread very busy, the io-thread will spend most of its time waiting
	in this function.
	The IO-thread also exits in this function. It checks server status at
	each wakeup and that is why we use timed wait in io_getevents(). */
	void collect();

private:
	/** Slot array */
	AIO*			m_array;

	/** Number of slots inthe local segment */
	ulint			m_n_slots;

	/** The local segment to check */
	ulint			m_segment;

	/** The global segment */
	ulint			m_global_segment;
};

/** Resubmit an IO request that was only partially successful
@param[in,out]	slot		Request to resubmit
@return DB_SUCCESS or DB_FAIL if the IO resubmit request failed */
dberr_t
LinuxAIOHandler::resubmit(Slot* slot)
{
#ifdef UNIV_DEBUG
	/* Bytes already read/written out */
	ulint	n_bytes = slot->ptr - slot->buf;

	ut_ad(m_array->is_mutex_owned());

	ut_ad(n_bytes < slot->original_len);
	ut_ad(static_cast<ulint>(slot->n_bytes) < slot->original_len - n_bytes);
	/* Partial read or write scenario */
	ut_ad(slot->len >= static_cast<ulint>(slot->n_bytes));
#endif /* UNIV_DEBUG */

	slot->len -= slot->n_bytes;
	slot->ptr += slot->n_bytes;
	slot->offset += slot->n_bytes;

	/* Resetting the bytes read/written */
	slot->n_bytes = 0;
	slot->io_already_done = false;

	compile_time_assert(sizeof(off_t) >= sizeof(os_offset_t));

	struct iocb*	iocb = &slot->control;

	if (slot->type.is_read()) {

		io_prep_pread(
			iocb,
			slot->file,
			slot->ptr,
			slot->len,
			slot->offset);
	} else {

		ut_a(slot->type.is_write());

		io_prep_pwrite(
			iocb,
			slot->file,
			slot->ptr,
			slot->len,
			slot->offset);
	}

	iocb->data = slot;

	ut_a(reinterpret_cast<size_t>(iocb->u.c.buf) % OS_FILE_LOG_BLOCK_SIZE
	     == 0);

	/* Resubmit an I/O request */
	int	ret = io_submit(m_array->io_ctx(m_segment), 1, &iocb);
	ut_a(ret != -EINVAL);

	if (ret < 0)  {
		errno = -ret;
	}

	return(ret < 0 ? DB_IO_PARTIAL_FAILED : DB_SUCCESS);
}

/** Check if the AIO succeeded
@param[in,out]	slot		The slot to check
@return DB_SUCCESS, DB_FAIL if the operation should be retried or
	DB_IO_ERROR on all other errors */
dberr_t
LinuxAIOHandler::check_state(Slot* slot)
{
	ut_ad(m_array->is_mutex_owned());

	/* Note that it may be that there is more then one completed
	IO requests. We process them one at a time. We may have a case
	here to improve the performance slightly by dealing with all
	requests in one sweep. */

	srv_set_io_thread_op_info(
		m_global_segment, "processing completed aio requests");

	ut_ad(slot->io_already_done);

	dberr_t	err = DB_SUCCESS;

	if (slot->ret == 0) {

		err = AIOHandler::post_io_processing(slot);

	} else {
		errno = -slot->ret;

		/* os_file_handle_error does tell us if we should retry
		this IO. As it stands now, we don't do this retry when
		reaping requests from a different context than
		the dispatcher. This non-retry logic is the same for
		Windows and Linux native AIO.
		We should probably look into this to transparently
		re-submit the IO. */
		os_file_handle_error(slot->name, "Linux aio");

		err = DB_IO_ERROR;
	}

	return(err);
}

/** If no slot was found then the m_array->m_mutex will be released.
@param[out]	n_pending		The number of pending IOs
@return NULL or a slot that has completed IO */
Slot*
LinuxAIOHandler::find_completed_slot(ulint* n_pending)
{
	ulint	offset = m_n_slots * m_segment;

	*n_pending = 0;

	m_array->acquire();

	Slot*	slot = m_array->at(offset);

	for (ulint i = 0; i < m_n_slots; ++i, ++slot) {

		if (slot->is_reserved) {

			++*n_pending;

			if (slot->io_already_done) {

				/* Something for us to work on.
				Note: We don't release the mutex. */
				return(slot);
			}
		}
	}

	m_array->release();

	return(NULL);
}

/** This function is only used in Linux native asynchronous i/o. This is
called from within the io-thread. If there are no completed IO requests
in the slot array, the thread calls this function to collect more
requests from the kernel.
The io-thread waits on io_getevents(), which is a blocking call, with
a timeout value. Unless the system is very heavy loaded, keeping the
io-thread very busy, the io-thread will spend most of its time waiting
in this function.
The io-thread also exits in this function. It checks server status at
each wakeup and that is why we use timed wait in io_getevents(). */
void
LinuxAIOHandler::collect()
{
	ut_ad(m_n_slots > 0);
	ut_ad(m_array != NULL);
	ut_ad(m_segment < m_array->get_n_segments());

	/* Which io_context_t we are going to use. */
	io_context_t	io_ctx = m_array->io_ctx(m_segment);

	/* Starting point of the m_segment we will be working on. */
	ulint	start_pos = m_segment * m_n_slots;

	/* End point. */
	ulint	end_pos = start_pos + m_n_slots;

	for (;;) {
		struct io_event*	events;

		/* Which part of event array we are going to work on. */
		events = m_array->io_events(m_segment * m_n_slots);

		/* Initialize the events. */
		memset(events, 0, sizeof(*events) * m_n_slots);

		/* The timeout value is arbitrary. We probably need
		to experiment with it a little. */
		struct timespec		timeout;

		timeout.tv_sec = 0;
		timeout.tv_nsec = OS_AIO_REAP_TIMEOUT;

		int	ret;

		ret = io_getevents(io_ctx, 1, m_n_slots, events, &timeout);
		ut_a(ret != -EINVAL);
		ut_ad(ret != -EFAULT);

		for (int i = 0; i < ret; ++i) {

			struct iocb*	iocb;

			iocb = reinterpret_cast<struct iocb*>(events[i].obj);
			ut_a(iocb != NULL);

			Slot*	slot = reinterpret_cast<Slot*>(iocb->data);

			/* Some sanity checks. */
			ut_a(slot != NULL);
			ut_a(slot->is_reserved);

			/* We are not scribbling previous segment. */
			ut_a(slot->pos >= start_pos);

			/* We have not overstepped to next segment. */
			ut_a(slot->pos < end_pos);

			/* Deallocate unused blocks from file system.
			This is newer done to page 0 or to log files.*/
			if (slot->offset > 0
			    && !slot->type.is_log()
			    && slot->type.is_write()
			    && slot->type.punch_hole()) {

				slot->err = slot->type.punch_hole(
					slot->file,
					slot->offset, slot->len);
			} else {
				slot->err = DB_SUCCESS;
			}

			/* Mark this request as completed. The error handling
			will be done in the calling function. */
			m_array->acquire();

			/* events[i].res2 should always be ZERO */
			ut_ad(events[i].res2 == 0);
			slot->io_already_done = true;

			/*Even though events[i].res is an unsigned number
			in libaio, it is used to return a negative value
			(negated errno value) to indicate error and a positive
			value to indicate number of bytes read or written. */

			if (events[i].res > slot->len) {
				/* failure */
				slot->n_bytes = 0;
				slot->ret = events[i].res;
			} else {
				/* success */
				slot->n_bytes = events[i].res;
				slot->ret = 0;
			}
			m_array->release();
		}

		if (srv_shutdown_state == SRV_SHUTDOWN_EXIT_THREADS
		    || !buf_page_cleaner_is_active
		    || ret > 0) {

			break;
		}

		/* This error handling is for any error in collecting the
		IO requests. The errors, if any, for any particular IO
		request are simply passed on to the calling routine. */

		switch (ret) {
		case -EAGAIN:
			/* Not enough resources! Try again. */

		case -EINTR:
			/* Interrupted! The behaviour in case of an interrupt.
			If we have some completed IOs available then the
			return code will be the number of IOs. We get EINTR
			only if there are no completed IOs and we have been
			interrupted. */

		case 0:
			/* No pending request! Go back and check again. */

			continue;
		}

		/* All other errors should cause a trap for now. */
		ib::fatal()
			<< "Unexpected ret_code[" << ret
			<< "] from io_getevents()!";

		break;
	}
}

/** Process a Linux AIO request
@param[out]	m1		the messages passed with the
@param[out]	m2		AIO request; note that in case the
				AIO operation failed, these output
				parameters are valid and can be used to
				restart the operation.
@param[out]	request		IO context
@return DB_SUCCESS or error code */
dberr_t
LinuxAIOHandler::poll(fil_node_t** m1, void** m2, IORequest* request)
{
	dberr_t		err = DB_SUCCESS;
	Slot*		slot;

	/* Loop until we have found a completed request. */
	for (;;) {

		ulint	n_pending;

		slot = find_completed_slot(&n_pending);

		if (slot != NULL) {

			ut_ad(m_array->is_mutex_owned());

			err = check_state(slot);

			/* DB_FAIL is not a hard error, we should retry */
			if (err != DB_FAIL) {
				break;
			}

			/* Partial IO, resubmit request for
			remaining bytes to read/write */
			err = resubmit(slot);

			if (err != DB_SUCCESS) {
				break;
			}

			m_array->release();

		} else if (is_shutdown() && n_pending == 0) {

			/* There is no completed request. If there is
			no pending request at all, and the system is
			being shut down, exit. */

			*m1 = NULL;
			*m2 = NULL;

			return(DB_SUCCESS);

		} else {

			/* Wait for some request. Note that we return
			from wait if we have found a request. */

			srv_set_io_thread_op_info(
				m_global_segment,
				"waiting for completed aio requests");

			collect();
		}
	}

	if (err == DB_IO_PARTIAL_FAILED) {
		/* Aborting in case of submit failure */
		ib::fatal()
			<< "Native Linux AIO interface. "
			"io_submit() call failed when "
			"resubmitting a partial I/O "
			"request on the file " << slot->name
			<< ".";
	}

	*m1 = slot->m1;
	*m2 = slot->m2;

	*request = slot->type;

	m_array->release(slot);

	m_array->release();

	return(err);
}

/** This function is only used in Linux native asynchronous i/o.
Waits for an aio operation to complete. This function is used to wait for
the completed requests. The aio array of pending requests is divided
into segments. The thread specifies which segment or slot it wants to wait
for. NOTE: this function will also take care of freeing the aio slot,
therefore no other thread is allowed to do the freeing!

@param[in]	global_seg	segment number in the aio array
				to wait for; segment 0 is the ibuf
				i/o thread, segment 1 is log i/o thread,
				then follow the non-ibuf read threads,
				and the last are the non-ibuf write
				threads.
@param[out]	m1		the messages passed with the
@param[out]	m2			AIO request; note that in case the
				AIO operation failed, these output
				parameters are valid and can be used to
				restart the operation.
@param[out]xi	 request	IO context
@return DB_SUCCESS if the IO was successful */
static
dberr_t
os_aio_linux_handler(
	ulint		global_segment,
	fil_node_t**	m1,
	void**		m2,
	IORequest*	request)
{
	return LinuxAIOHandler(global_segment).poll(m1, m2, request);
}

/** Dispatch an AIO request to the kernel.
@param[in,out]	slot		an already reserved slot
@return true on success. */
bool
AIO::linux_dispatch(Slot* slot)
{
	ut_a(slot->is_reserved);
	ut_ad(slot->type.validate());

	/* Find out what we are going to work with.
	The iocb struct is directly in the slot.
	The io_context_t is one per segment. */

	ulint		io_ctx_index;
	struct iocb*	iocb = &slot->control;

	io_ctx_index = (slot->pos * m_n_segments) / m_slots.size();

	ut_a(reinterpret_cast<size_t>(iocb->u.c.buf) % OS_FILE_LOG_BLOCK_SIZE
	     == 0);

	int	ret = io_submit(io_ctx(io_ctx_index), 1, &iocb);
	ut_a(ret != -EINVAL);

	/* io_submit() returns number of successfully queued requests
	or -errno. */

	if (ret != 1) {
		errno = -ret;
	}

	return(ret == 1);
}

/** Creates an io_context_t for native linux AIO.
@param[in]	max_events	number of events
@param[out]	io_ctx		io_ctx to initialize.
@return true on success. */
bool
AIO::linux_create_io_ctx(
	unsigned	max_events,
	io_context_t&	io_ctx)
{
	ssize_t		n_retries = 0;

	for (;;) {

		memset(&io_ctx, 0x0, sizeof(io_ctx));

		/* Initialize the io_ctx. Tell it how many pending
		IO requests this context will handle. */

		int	ret = io_setup(max_events, &io_ctx);
		ut_a(ret != -EINVAL);

		if (ret == 0) {
			/* Success. Return now. */
			return(true);
		}

		/* If we hit EAGAIN we'll make a few attempts before failing. */

		switch (ret) {
		case -EAGAIN:
			if (n_retries == 0) {
				/* First time around. */
				ib::warn()
					<< "io_setup() failed with EAGAIN."
					" Will make "
					<< OS_AIO_IO_SETUP_RETRY_ATTEMPTS
					<< " attempts before giving up.";
			}

			if (n_retries < OS_AIO_IO_SETUP_RETRY_ATTEMPTS) {

				++n_retries;

				ib::warn()
					<< "io_setup() attempt "
					<< n_retries << ".";

				os_thread_sleep(OS_AIO_IO_SETUP_RETRY_SLEEP);

				continue;
			}

			/* Have tried enough. Better call it a day. */
			ib::warn()
				<< "io_setup() failed with EAGAIN after "
				<< OS_AIO_IO_SETUP_RETRY_ATTEMPTS
				<< " attempts.";
			break;

		case -ENOSYS:
			ib::warn()
				<< "Linux Native AIO interface"
				" is not supported on this platform. Please"
				" check your OS documentation and install"
				" appropriate binary of InnoDB.";

			break;

		default:
			ib::warn()
				<< "Linux Native AIO setup"
				<< " returned following error["
				<< ret << "]";
			break;
		}

		ib::info()
			<< "You can disable Linux Native AIO by"
			" setting innodb_use_native_aio = 0 in my.cnf";

		break;
	}

	return(false);
}

/** Checks if the system supports native linux aio. On some kernel
versions where native aio is supported it won't work on tmpfs. In such
cases we can't use native aio as it is not possible to mix simulated
and native aio.
@return: true if supported, false otherwise. */
bool
AIO::is_linux_native_aio_supported()
{
	int		fd;
	io_context_t	io_ctx;
	char		name[1000];

	if (!linux_create_io_ctx(1, io_ctx)) {

		/* The platform does not support native aio. */

		return(false);

	} else if (!srv_read_only_mode) {

		/* Now check if tmpdir supports native aio ops. */
		fd = innobase_mysql_tmpfile(NULL);

		if (fd < 0) {
			ib::warn()
				<< "Unable to create temp file to check"
				" native AIO support.";

			int ret = io_destroy(io_ctx);
			ut_a(ret != -EINVAL);
			ut_ad(ret != -EFAULT);

			return(false);
		}
	} else {

		os_normalize_path(srv_log_group_home_dir);

		ulint	dirnamelen = strlen(srv_log_group_home_dir);

		ut_a(dirnamelen < (sizeof name) - 10 - sizeof "ib_logfile");

		memcpy(name, srv_log_group_home_dir, dirnamelen);

		/* Add a path separator if needed. */
		if (dirnamelen && name[dirnamelen - 1] != OS_PATH_SEPARATOR) {

			name[dirnamelen++] = OS_PATH_SEPARATOR;
		}

		strcpy(name + dirnamelen, "ib_logfile0");

		fd = open(name, O_RDONLY | O_CLOEXEC);

		if (fd == -1) {

			ib::warn()
				<< "Unable to open"
				<< " \"" << name << "\" to check native"
				<< " AIO read support.";

			int ret = io_destroy(io_ctx);
			ut_a(ret != EINVAL);
			ut_ad(ret != EFAULT);

			return(false);
		}
	}

	struct io_event	io_event;

	memset(&io_event, 0x0, sizeof(io_event));

	byte*	buf = static_cast<byte*>(ut_malloc_nokey(UNIV_PAGE_SIZE * 2));
	byte*	ptr = static_cast<byte*>(ut_align(buf, UNIV_PAGE_SIZE));

	struct iocb	iocb;

	/* Suppress valgrind warning. */
	memset(buf, 0x00, UNIV_PAGE_SIZE * 2);
	memset(&iocb, 0x0, sizeof(iocb));

	struct iocb*	p_iocb = &iocb;

	if (!srv_read_only_mode) {

		io_prep_pwrite(p_iocb, fd, ptr, UNIV_PAGE_SIZE, 0);

	} else {
		ut_a(srv_page_size >= 4096);
		io_prep_pread(p_iocb, fd, ptr, srv_page_size, 0);
	}

	ut_a(reinterpret_cast<size_t>(p_iocb->u.c.buf) % OS_FILE_LOG_BLOCK_SIZE
	     == 0);
	int	err = io_submit(io_ctx, 1, &p_iocb);
	ut_a(err != -EINVAL);

	if (err >= 1) {
		/* Now collect the submitted IO request. */
		err = io_getevents(io_ctx, 1, 1, &io_event, NULL);
		ut_a(err != -EINVAL);
	}

	ut_free(buf);
	close(fd);

	switch (err) {
	case 1:
		{
			int ret = io_destroy(io_ctx);
			ut_a(ret != -EINVAL);
			ut_ad(ret != -EFAULT);

			return(true);
		}

	case -EINVAL:
	case -ENOSYS:
		ib::error()
			<< "Linux Native AIO not supported. You can either"
			" move "
			<< (srv_read_only_mode ? name : "tmpdir")
			<< " to a file system that supports native"
			" AIO or you can set innodb_use_native_aio to"
			" FALSE to avoid this message.";

		/* fall through. */
	default:
		ib::error()
			<< "Linux Native AIO check on "
			<< (srv_read_only_mode ? name : "tmpdir")
			<< "returned error[" << -err << "]";
	}

	int ret = io_destroy(io_ctx);
	ut_a(ret != -EINVAL);
	ut_ad(ret != -EFAULT);

	return(false);
}

#endif /* LINUX_NATIVE_AIO */

/** Retrieves the last error number if an error occurs in a file io function.
The number should be retrieved before any other OS calls (because they may
overwrite the error number). If the number is not known to this program,
the OS error number + OS_FILE_ERROR_MAX is returned.
@param[in]	report_all_errors	true if we want an error message
					printed of all errors
@param[in]	on_error_silent		true then don't print any diagnostic
					to the log
@return error number, or OS error number + OS_FILE_ERROR_MAX */
static
ulint
os_file_get_last_error_low(
	bool	report_all_errors,
	bool	on_error_silent)
{
	int	err = errno;

	if (err == 0) {
		return(0);
	}

	if (report_all_errors
	    || (err != ENOSPC && err != EEXIST && !on_error_silent)) {

		ib::error()
			<< "Operating system error number "
			<< err
			<< " in a file operation.";

		if (err == ENOENT) {

			ib::error()
				<< "The error means the system"
				" cannot find the path specified.";

			if (srv_is_being_started) {

				ib::error()
					<< "If you are installing InnoDB,"
					" remember that you must create"
					" directories yourself, InnoDB"
					" does not create them.";
			}
		} else if (err == EACCES) {

			ib::error()
				<< "The error means mysqld does not have"
				" the access rights to the directory.";

		} else {
			if (strerror(err) != NULL) {

				ib::error()
					<< "Error number " << err << " means '"
					<< strerror(err) << "'";
			}

			ib::info() << OPERATING_SYSTEM_ERROR_MSG;
		}
	}

	switch (err) {
	case ENOSPC:
		return(OS_FILE_DISK_FULL);
	case ENOENT:
		return(OS_FILE_NOT_FOUND);
	case EEXIST:
		return(OS_FILE_ALREADY_EXISTS);
	case EXDEV:
	case ENOTDIR:
	case EISDIR:
		return(OS_FILE_PATH_ERROR);
	case EAGAIN:
		if (srv_use_native_aio) {
			return(OS_FILE_AIO_RESOURCES_RESERVED);
		}
		break;
	case EINTR:
		if (srv_use_native_aio) {
			return(OS_FILE_AIO_INTERRUPTED);
		}
		break;
	case EACCES:
		return(OS_FILE_ACCESS_VIOLATION);
	}
	return(OS_FILE_ERROR_MAX + err);
}

/** Wrapper to fsync(2) that retries the call on some errors.
Returns the value 0 if successful; otherwise the value -1 is returned and
the global variable errno is set to indicate the error.
@param[in]	file		open file handle
@return 0 if success, -1 otherwise */
static
int
os_file_fsync_posix(
	os_file_t	file)
{
	ulint		failures = 0;

	for (;;) {

		++os_n_fsyncs;

		int	ret = fsync(file);

		if (ret == 0) {
			return(ret);
		}

		switch(errno) {
		case ENOLCK:

			++failures;
			ut_a(failures < 1000);

			if (!(failures % 100)) {

				ib::warn()
					<< "fsync(): "
					<< "No locks available; retrying";
			}

			/* 0.2 sec */
			os_thread_sleep(200000);
			break;

		case EINTR:

			++failures;
			ut_a(failures < 2000);
			break;

		default:
			ib::fatal() << "fsync() returned " << errno;
		}
	}
}

/** Check the existence and type of the given file.
@param[in]	path		path name of file
@param[out]	exists		true if the file exists
@param[out]	type		Type of the file, if it exists
@return true if call succeeded */
static
bool
os_file_status_posix(
	const char*	path,
	bool*		exists,
	os_file_type_t* type)
{
	struct stat	statinfo;

	int	ret = stat(path, &statinfo);

	*exists = !ret;

	if (!ret) {
		/* file exists, everything OK */

	} else if (errno == ENOENT || errno == ENOTDIR || errno == ENAMETOOLONG) {
		/* file does not exist */
		return(true);

	} else {
		/* file exists, but stat call failed */
		os_file_handle_error_no_exit(path, "stat", false);
		return(false);
	}

	if (S_ISDIR(statinfo.st_mode)) {
		*type = OS_FILE_TYPE_DIR;

	} else if (S_ISLNK(statinfo.st_mode)) {
		*type = OS_FILE_TYPE_LINK;

	} else if (S_ISREG(statinfo.st_mode)) {
		*type = OS_FILE_TYPE_FILE;
	} else {
		*type = OS_FILE_TYPE_UNKNOWN;
	}

	return(true);
}

/** NOTE! Use the corresponding macro os_file_flush(), not directly this
function!
Flushes the write buffers of a given file to the disk.
@param[in]	file		handle to a file
@return true if success */
bool
os_file_flush_func(
	os_file_t	file)
{
	int	ret;

	WAIT_ALLOW_WRITES();
	ret = os_file_fsync_posix(file);

	if (ret == 0) {
		return(true);
	}

	/* Since Linux returns EINVAL if the 'file' is actually a raw device,
	we choose to ignore that error if we are using raw disks */

	if (srv_start_raw_disk_in_use && errno == EINVAL) {

		return(true);
	}

	ib::error() << "The OS said file flush did not succeed";

	os_file_handle_error(NULL, "flush");

	/* It is a fatal error if a file flush does not succeed, because then
	the database can get corrupt on disk */
	ut_error;

	return(false);
}

/** NOTE! Use the corresponding macro os_file_create_simple(), not directly
this function!
A simple function to open or create a file.
@param[in]	name		name of the file or path as a null-terminated
				string
@param[in]	create_mode	create mode
@param[in]	access_type	OS_FILE_READ_ONLY or OS_FILE_READ_WRITE
@param[in]	read_only	if true, read only checks are enforced
@param[out]	success		true if succeed, false if error
@return handle to the file, not defined if error, error number
	can be retrieved with os_file_get_last_error */
pfs_os_file_t
os_file_create_simple_func(
	const char*	name,
	ulint		create_mode,
	ulint		access_type,
	bool		read_only,
	bool*		success)
{
	pfs_os_file_t	file;

	*success = false;

	int		create_flag;
	const char*	mode_str	= NULL;

	if (create_mode != OS_FILE_OPEN && create_mode != OS_FILE_OPEN_RAW) {
		WAIT_ALLOW_WRITES();
	}

	ut_a(!(create_mode & OS_FILE_ON_ERROR_SILENT));
	ut_a(!(create_mode & OS_FILE_ON_ERROR_NO_EXIT));

	if (create_mode == OS_FILE_OPEN) {
		mode_str = "OPEN";

		if (access_type == OS_FILE_READ_ONLY) {

			create_flag = O_RDONLY;

		} else if (read_only) {

			create_flag = O_RDONLY;

		} else {
			create_flag = O_RDWR;
		}

	} else if (read_only) {

		mode_str = "OPEN";
		create_flag = O_RDONLY;

	} else if (create_mode == OS_FILE_CREATE) {

		mode_str = "CREATE";
		create_flag = O_RDWR | O_CREAT | O_EXCL;

	} else if (create_mode == OS_FILE_CREATE_PATH) {

		mode_str = "CREATE PATH";
		/* Create subdirs along the path if needed. */

		*success = os_file_create_subdirs_if_needed(name);

		if (!*success) {

			ib::error()
				<< "Unable to create subdirectories '"
				<< name << "'";

			return(OS_FILE_CLOSED);
		}

		create_flag = O_RDWR | O_CREAT | O_EXCL;
		create_mode = OS_FILE_CREATE;
	} else {

		ib::error()
			<< "Unknown file create mode ("
			<< create_mode
			<< " for file '" << name << "'";

		return(OS_FILE_CLOSED);
	}

	bool	retry;

	do {
		file = open(name, create_flag | O_CLOEXEC, os_innodb_umask);

		if (file == -1) {
			*success = false;
			retry = os_file_handle_error(
				name,
				create_mode == OS_FILE_OPEN
				? "open" : "create");
		} else {
			*success = true;
			retry = false;
		}

	} while (retry);

	/* This function is always called for data files, we should disable
	OS caching (O_DIRECT) here as we do in os_file_create_func(), so
	we open the same file in the same mode, see man page of open(2). */
       if (!srv_read_only_mode
	   && *success
	   && (srv_file_flush_method == SRV_O_DIRECT
	       || srv_file_flush_method == SRV_O_DIRECT_NO_FSYNC)) {

	       os_file_set_nocache(file, name, mode_str);
	}

#ifdef USE_FILE_LOCK
	if (!read_only
	    && *success
	    && (access_type == OS_FILE_READ_WRITE)
	    && os_file_lock(file, name)) {

		*success = false;
		close(file);
		file = -1;
	}
#endif /* USE_FILE_LOCK */

	return(file);
}

/** This function attempts to create a directory named pathname. The new
directory gets default permissions. On Unix the permissions are
(0770 & ~umask). If the directory exists already, nothing is done and
the call succeeds, unless the fail_if_exists arguments is true.
If another error occurs, such as a permission error, this does not crash,
but reports the error and returns false.
@param[in]	pathname	directory name as null-terminated string
@param[in]	fail_if_exists	if true, pre-existing directory is treated as
				an error.
@return true if call succeeds, false on error */
bool
os_file_create_directory(
	const char*	pathname,
	bool		fail_if_exists)
{
	int	rcode;

	WAIT_ALLOW_WRITES();
	rcode = mkdir(pathname, 0770);

	if (!(rcode == 0 || (errno == EEXIST && !fail_if_exists))) {
		/* failure */
		os_file_handle_error_no_exit(pathname, "mkdir", false);

		return(false);
	}

	return(true);
}

/**
The os_file_opendir() function opens a directory stream corresponding to the
directory named by the dirname argument. The directory stream is positioned
at the first entry. In both Unix and Windows we automatically skip the '.'
and '..' items at the start of the directory listing.
@param[in]	dirname		directory name; it must not contain a trailing
				'\' or '/'
@param[in]	is_fatal	true if we should treat an error as a fatal
				error; if we try to open symlinks then we do
				not wish a fatal error if it happens not to be
				a directory
@return directory stream, NULL if error */
os_file_dir_t
os_file_opendir(
	const char*	dirname,
	bool		error_is_fatal)
{
	os_file_dir_t		dir;
	dir = opendir(dirname);

	if (dir == NULL && error_is_fatal) {
		os_file_handle_error(dirname, "opendir");
	}

	return(dir);
}

/** Closes a directory stream.
@param[in]	dir		directory stream
@return 0 if success, -1 if failure */
int
os_file_closedir(
	os_file_dir_t	dir)
{
	int	ret = closedir(dir);

	if (ret != 0) {
		os_file_handle_error_no_exit(NULL, "closedir", false);
	}

	return(ret);
}

/** This function returns information of the next file in the directory. We jump
over the '.' and '..' entries in the directory.
@param[in]	dirname		directory name or path
@param[in]	dir		directory stream
@param[out]	info		buffer where the info is returned
@return 0 if ok, -1 if error, 1 if at the end of the directory */
int
os_file_readdir_next_file(
	const char*	dirname,
	os_file_dir_t	dir,
	os_file_stat_t*	info)
{
	struct dirent*	ent;
	char*		full_path;
	int		ret;
	struct stat	statinfo;

next_file:

	ent = readdir(dir);

	if (ent == NULL) {

		return(1);
	}

	ut_a(strlen(ent->d_name) < OS_FILE_MAX_PATH);

	if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {

		goto next_file;
	}

	strcpy(info->name, ent->d_name);

	full_path = static_cast<char*>(
		ut_malloc_nokey(strlen(dirname) + strlen(ent->d_name) + 10));

	sprintf(full_path, "%s/%s", dirname, ent->d_name);

	ret = stat(full_path, &statinfo);

	if (ret) {

		if (errno == ENOENT) {
			/* readdir() returned a file that does not exist,
			it must have been deleted in the meantime. Do what
			would have happened if the file was deleted before
			readdir() - ignore and go to the next entry.
			If this is the last entry then info->name will still
			contain the name of the deleted file when this
			function returns, but this is not an issue since the
			caller shouldn't be looking at info when end of
			directory is returned. */

			ut_free(full_path);

			goto next_file;
		}

		os_file_handle_error_no_exit(full_path, "stat", false);

		ut_free(full_path);

		return(-1);
	}

	info->size = statinfo.st_size;

	if (S_ISDIR(statinfo.st_mode)) {
		info->type = OS_FILE_TYPE_DIR;
	} else if (S_ISLNK(statinfo.st_mode)) {
		info->type = OS_FILE_TYPE_LINK;
	} else if (S_ISREG(statinfo.st_mode)) {
		info->type = OS_FILE_TYPE_FILE;
	} else {
		info->type = OS_FILE_TYPE_UNKNOWN;
	}

	ut_free(full_path);

	return(0);
}

/** NOTE! Use the corresponding macro os_file_create(), not directly
this function!
Opens an existing file or creates a new.
@param[in]	name		name of the file or path as a null-terminated
				string
@param[in]	create_mode	create mode
@param[in]	purpose		OS_FILE_AIO, if asynchronous, non-buffered I/O
				is desired, OS_FILE_NORMAL, if any normal file;
				NOTE that it also depends on type, os_aio_..
				and srv_.. variables whether we really use async
				I/O or unbuffered I/O: look in the function
				source code for the exact rules
@param[in]	type		OS_DATA_FILE or OS_LOG_FILE
@param[in]	read_only	true, if read only checks should be enforcedm
@param[in]	success		true if succeeded
@return handle to the file, not defined if error, error number
	can be retrieved with os_file_get_last_error */
pfs_os_file_t
os_file_create_func(
	const char*	name,
	ulint		create_mode,
	ulint		purpose,
	ulint		type,
	bool		read_only,
	bool*		success)
{
	bool		on_error_no_exit;
	bool		on_error_silent;

	*success = false;

	DBUG_EXECUTE_IF(
		"ib_create_table_fail_disk_full",
		*success = false;
		errno = ENOSPC;
		return(OS_FILE_CLOSED);
	);

	int		create_flag;
	const char*	mode_str	= NULL;

	on_error_no_exit = create_mode & OS_FILE_ON_ERROR_NO_EXIT
		? true : false;
	on_error_silent = create_mode & OS_FILE_ON_ERROR_SILENT
		? true : false;

	create_mode &= ulint(~(OS_FILE_ON_ERROR_NO_EXIT
			       | OS_FILE_ON_ERROR_SILENT));

	if (create_mode == OS_FILE_OPEN
	    || create_mode == OS_FILE_OPEN_RAW
	    || create_mode == OS_FILE_OPEN_RETRY) {

		mode_str = "OPEN";

		create_flag = read_only ? O_RDONLY : O_RDWR;

	} else if (read_only) {

		mode_str = "OPEN";

		create_flag = O_RDONLY;

	} else if (create_mode == OS_FILE_CREATE) {

		mode_str = "CREATE";
		create_flag = O_RDWR | O_CREAT | O_EXCL;

	} else if (create_mode == OS_FILE_OVERWRITE) {

		mode_str = "OVERWRITE";
		create_flag = O_RDWR | O_CREAT | O_TRUNC;

	} else {
		ib::error()
			<< "Unknown file create mode (" << create_mode << ")"
			<< " for file '" << name << "'";

		return(OS_FILE_CLOSED);
	}

	ut_a(type == OS_LOG_FILE
	     || type == OS_DATA_FILE
	     || type == OS_DATA_FILE_NO_O_DIRECT);

	ut_a(purpose == OS_FILE_AIO || purpose == OS_FILE_NORMAL);

#ifdef O_SYNC
	/* We let O_SYNC only affect log files; note that we map O_DSYNC to
	O_SYNC because the datasync options seemed to corrupt files in 2001
	in both Linux and Solaris */

	if (!read_only
	    && type == OS_LOG_FILE
	    && srv_file_flush_method == SRV_O_DSYNC) {

		create_flag |= O_SYNC;
	}
#endif /* O_SYNC */

	os_file_t	file;
	bool		retry;

	do {
		file = open(name, create_flag | O_CLOEXEC, os_innodb_umask);

		if (file == -1) {
			const char*	operation;

			operation = (create_mode == OS_FILE_CREATE
				     && !read_only) ? "create" : "open";

			*success = false;

			if (on_error_no_exit) {
				retry = os_file_handle_error_no_exit(
					name, operation, on_error_silent);
			} else {
				retry = os_file_handle_error(name, operation);
			}
		} else {
			*success = true;
			retry = false;
		}

	} while (retry);

	/* We disable OS caching (O_DIRECT) only on data files */
	if (!read_only
	    && *success
	    && (type != OS_LOG_FILE
		&& type != OS_DATA_FILE_NO_O_DIRECT)
	    && (srv_file_flush_method == SRV_O_DIRECT
		|| srv_file_flush_method == SRV_O_DIRECT_NO_FSYNC)) {

	       os_file_set_nocache(file, name, mode_str);
	}

#ifdef USE_FILE_LOCK
	if (!read_only
	    && *success
	    && create_mode != OS_FILE_OPEN_RAW
	    && os_file_lock(file, name)) {

		if (create_mode == OS_FILE_OPEN_RETRY) {

			ib::info()
				<< "Retrying to lock the first data file";

			for (int i = 0; i < 100; i++) {
				os_thread_sleep(1000000);

				if (!os_file_lock(file, name)) {
					*success = true;
					return(file);
				}
			}

			ib::info()
				<< "Unable to open the first data file";
		}

		*success = false;
		close(file);
		file = -1;
	}
#endif /* USE_FILE_LOCK */

	return(file);
}

/** NOTE! Use the corresponding macro
os_file_create_simple_no_error_handling(), not directly this function!
A simple function to open or create a file.
@param[in]	name		name of the file or path as a null-terminated
				string
@param[in]	create_mode	create mode
@param[in]	access_type	OS_FILE_READ_ONLY, OS_FILE_READ_WRITE, or
				OS_FILE_READ_ALLOW_DELETE; the last option
				is used by a backup program reading the file
@param[in]	read_only	if true read only mode checks are enforced
@param[out]	success		true if succeeded
@return own: handle to the file, not defined if error, error number
	can be retrieved with os_file_get_last_error */
pfs_os_file_t
os_file_create_simple_no_error_handling_func(
	const char*	name,
	ulint		create_mode,
	ulint		access_type,
	bool		read_only,
	bool*		success)
{
	os_file_t	file;
	int		create_flag;

	if (create_mode != OS_FILE_OPEN && create_mode != OS_FILE_OPEN_RAW) {
		WAIT_ALLOW_WRITES();
	}

	ut_a(!(create_mode & OS_FILE_ON_ERROR_SILENT));
	ut_a(!(create_mode & OS_FILE_ON_ERROR_NO_EXIT));

	*success = false;

	if (create_mode == OS_FILE_OPEN) {

		if (access_type == OS_FILE_READ_ONLY) {

			create_flag = O_RDONLY;

		} else if (read_only) {

			create_flag = O_RDONLY;

		} else {

			ut_a(access_type == OS_FILE_READ_WRITE
			     || access_type == OS_FILE_READ_ALLOW_DELETE);

			create_flag = O_RDWR;
		}

	} else if (read_only) {

		create_flag = O_RDONLY;

	} else if (create_mode == OS_FILE_CREATE) {

		create_flag = O_RDWR | O_CREAT | O_EXCL;

	} else {

		ib::error()
			<< "Unknown file create mode "
			<< create_mode << " for file '" << name << "'";

		return(OS_FILE_CLOSED);
	}

	file = open(name, create_flag | O_CLOEXEC, os_innodb_umask);

	*success = (file != -1);

#ifdef USE_FILE_LOCK
	if (!read_only
	    && *success
	    && access_type == OS_FILE_READ_WRITE
	    && os_file_lock(file, name)) {

		*success = false;
		close(file);
		file = -1;

	}
#endif /* USE_FILE_LOCK */

	return(file);
}

/** Deletes a file if it exists. The file has to be closed before calling this.
@param[in]	name		file path as a null-terminated string
@param[out]	exist		indicate if file pre-exist
@return true if success */
bool
os_file_delete_if_exists_func(
	const char*	name,
	bool*		exist)
{
	if (exist != NULL) {
		*exist = true;
	}

	int	ret;
	WAIT_ALLOW_WRITES();

	ret = unlink(name);

	if (ret != 0 && errno == ENOENT) {
		if (exist != NULL) {
			*exist = false;
		}
	} else if (ret != 0 && errno != ENOENT) {
		os_file_handle_error_no_exit(name, "delete", false);

		return(false);
	}

	return(true);
}

/** Deletes a file. The file has to be closed before calling this.
@param[in]	name		file path as a null-terminated string
@return true if success */
bool
os_file_delete_func(
	const char*	name)
{
	int	ret;
	WAIT_ALLOW_WRITES();

	ret = unlink(name);

	if (ret != 0) {
		os_file_handle_error_no_exit(name, "delete", FALSE);

		return(false);
	}

	return(true);
}

/** NOTE! Use the corresponding macro os_file_rename(), not directly this
function!
Renames a file (can also move it to another directory). It is safest that the
file is closed before calling this function.
@param[in]	oldpath		old file path as a null-terminated string
@param[in]	newpath		new file path
@return true if success */
bool
os_file_rename_func(
	const char*	oldpath,
	const char*	newpath)
{
#ifdef UNIV_DEBUG
	os_file_type_t	type;
	bool		exists;

	/* New path must not exist. */
	ut_ad(os_file_status(newpath, &exists, &type));
	ut_ad(!exists);

	/* Old path must exist. */
	ut_ad(os_file_status(oldpath, &exists, &type));
	ut_ad(exists);
#endif /* UNIV_DEBUG */

	int	ret;
	WAIT_ALLOW_WRITES();

	ret = rename(oldpath, newpath);

	if (ret != 0) {
		os_file_handle_rename_error(oldpath, newpath);

		return(false);
	}

	return(true);
}

/** NOTE! Use the corresponding macro os_file_close(), not directly this
function!
Closes a file handle. In case of error, error number can be retrieved with
os_file_get_last_error.
@param[in]	file		Handle to close
@return true if success */
bool
os_file_close_func(
	os_file_t	file)
{
	int	ret = close(file);

	if (ret == -1) {
		os_file_handle_error(NULL, "close");

		return(false);
	}

	return(true);
}

/** Gets a file size.
@param[in]	file		handle to an open file
@return file size, or (os_offset_t) -1 on failure */
os_offset_t
os_file_get_size(os_file_t file)
{
	struct stat statbuf;
	return fstat(file, &statbuf) ? os_offset_t(-1) : statbuf.st_size;
}

/** Gets a file size.
@param[in]	filename	Full path to the filename to check
@return file size if OK, else set m_total_size to ~0 and m_alloc_size to
	errno */
os_file_size_t
os_file_get_size(
	const char*	filename)
{
	struct stat	s;
	os_file_size_t	file_size;

	int	ret = stat(filename, &s);

	if (ret == 0) {
		file_size.m_total_size = s.st_size;
		/* st_blocks is in 512 byte sized blocks */
		file_size.m_alloc_size = s.st_blocks * 512;
	} else {
		file_size.m_total_size = ~0;
		file_size.m_alloc_size = (os_offset_t) errno;
	}

	return(file_size);
}

/** This function returns information about the specified file
@param[in]	path		pathname of the file
@param[out]	stat_info	information of a file in a directory
@param[in,out]	statinfo	information of a file in a directory
@param[in]	check_rw_perm	for testing whether the file can be opened
				in RW mode
@param[in]	read_only	if true read only mode checks are enforced
@return DB_SUCCESS if all OK */
static
dberr_t
os_file_get_status_posix(
	const char*	path,
	os_file_stat_t* stat_info,
	struct stat*	statinfo,
	bool		check_rw_perm,
	bool		read_only)
{
	int	ret = stat(path, statinfo);

	if (ret && (errno == ENOENT || errno == ENOTDIR
		    || errno == ENAMETOOLONG)) {
		/* file does not exist */

		return(DB_NOT_FOUND);

	} else if (ret) {
		/* file exists, but stat call failed */

		os_file_handle_error_no_exit(path, "stat", false);

		return(DB_FAIL);
	}

	switch (statinfo->st_mode & S_IFMT) {
	case S_IFDIR:
		stat_info->type = OS_FILE_TYPE_DIR;
		break;
	case S_IFLNK:
		stat_info->type = OS_FILE_TYPE_LINK;
		break;
	case S_IFBLK:
		/* Handle block device as regular file. */
	case S_IFCHR:
		/* Handle character device as regular file. */
	case S_IFREG:
		stat_info->type = OS_FILE_TYPE_FILE;
		break;
	default:
		stat_info->type = OS_FILE_TYPE_UNKNOWN;
	}

	stat_info->size = statinfo->st_size;
	stat_info->block_size = statinfo->st_blksize;
	stat_info->alloc_size = statinfo->st_blocks * 512;

	if (check_rw_perm
	    && (stat_info->type == OS_FILE_TYPE_FILE
		|| stat_info->type == OS_FILE_TYPE_BLOCK)) {

		stat_info->rw_perm = !access(path, read_only
					     ? R_OK : R_OK | W_OK);
	}

	return(DB_SUCCESS);
}

/** Truncates a file to a specified size in bytes.
Do nothing if the size to preserve is greater or equal to the current
size of the file.
@param[in]	pathname	file path
@param[in]	file		file to be truncated
@param[in]	size		size to preserve in bytes
@return true if success */
static
bool
os_file_truncate_posix(
	const char*	pathname,
	os_file_t	file,
	os_offset_t	size)
{
	int	res = ftruncate(file, size);

	if (res == -1) {

		bool	retry;

		retry = os_file_handle_error_no_exit(
			pathname, "truncate", false);

		if (retry) {
			ib::warn()
				<< "Truncate failed for '"
				<< pathname << "'";
		}
	}

	return(res == 0);
}

/** Truncates a file at its current position.
@return true if success */
bool
os_file_set_eof(
	FILE*		file)	/*!< in: file to be truncated */
{
	WAIT_ALLOW_WRITES();
	return(!ftruncate(fileno(file), ftell(file)));
}

#else /* !_WIN32 */

#include <WinIoCtl.h>

/*
Windows : Handling synchronous IO on files opened asynchronously.

If file is opened for asynchronous IO (FILE_FLAG_OVERLAPPED) and also bound to
a completion port, then every IO on this file would normally be enqueued to the
completion port. Sometimes however we would like to do a synchronous IO. This is
possible if we initialitze have overlapped.hEvent with a valid event and set its
lowest order bit to 1 (see MSDN ReadFile and WriteFile description for more info)

We'll create this special event once for each thread and store in thread local
storage.
*/


static void __stdcall win_free_syncio_event(void *data) {
	if (data) {
		CloseHandle((HANDLE)data);
	}
}


/*
Initialize tls index.for event handle used for synchronized IO on files that
might be opened with FILE_FLAG_OVERLAPPED.
*/
static void win_init_syncio_event() {
	fls_sync_io = FlsAlloc(win_free_syncio_event);
	ut_a(fls_sync_io != FLS_OUT_OF_INDEXES);
}


/*
Retrieve per-thread event for doing synchronous io on asyncronously opened files
*/
static HANDLE win_get_syncio_event()
{
	HANDLE h;

	h = (HANDLE)FlsGetValue(fls_sync_io);
	if (h) {
		return h;
	}
	h = CreateEventA(NULL, FALSE, FALSE, NULL);
	ut_a(h);
	/* Set low-order bit to keeps I/O completion from being queued */
	h = (HANDLE)((uintptr_t)h | 1);
	FlsSetValue(fls_sync_io, h);
	return h;
}


/** Do the read/write
@param[in]	request	The IO context and type
@return the number of bytes read/written or negative value on error */
ssize_t
SyncFileIO::execute(const IORequest& request)
{
	OVERLAPPED	seek;

	memset(&seek, 0x0, sizeof(seek));

	seek.hEvent = win_get_syncio_event();
	seek.Offset = (DWORD) m_offset & 0xFFFFFFFF;
	seek.OffsetHigh = (DWORD) (m_offset >> 32);

	BOOL	ret;
	DWORD	n_bytes;

	if (request.is_read()) {
		ret = ReadFile(m_fh, m_buf,
			static_cast<DWORD>(m_n), NULL, &seek);

	} else {
		ut_ad(request.is_write());
		ret = WriteFile(m_fh, m_buf,
			static_cast<DWORD>(m_n), NULL, &seek);
	}
	if (ret || (GetLastError() == ERROR_IO_PENDING)) {
		/* Wait for async io to complete */
		ret = GetOverlappedResult(m_fh, &seek, &n_bytes, TRUE);
	}

	return(ret ? static_cast<ssize_t>(n_bytes) : -1);
}

/** Do the read/write
@param[in,out]	slot	The IO slot, it has the IO context
@return the number of bytes read/written or negative value on error */
ssize_t
SyncFileIO::execute(Slot* slot)
{
	BOOL	ret;
	slot->control.hEvent = win_get_syncio_event();
	if (slot->type.is_read()) {

		ret = ReadFile(
			slot->file, slot->ptr, slot->len,
			NULL, &slot->control);

	} else {
		ut_ad(slot->type.is_write());

		ret = WriteFile(
			slot->file, slot->ptr, slot->len,
			NULL, &slot->control);

	}
	if (ret || (GetLastError() == ERROR_IO_PENDING)) {
		/* Wait for async io to complete */
		ret = GetOverlappedResult(slot->file, &slot->control, &slot->n_bytes, TRUE);
	}

	return(ret ? static_cast<ssize_t>(slot->n_bytes) : -1);
}

/* Startup/shutdown */

struct WinIoInit
{
	WinIoInit() {
		fls_sync_io = FlsAlloc(win_free_syncio_event);
		ut_a(fls_sync_io != FLS_OUT_OF_INDEXES);
	}

	~WinIoInit() {
		FlsFree(fls_sync_io);
	}
};

/* Ensures proper initialization and shutdown */
static WinIoInit win_io_init;

/** Check if the file system supports sparse files.
@param[in]	 name		File name
@return true if the file system supports sparse files */
static
bool
os_is_sparse_file_supported_win32(const char* filename)
{
	char	volname[MAX_PATH];
	BOOL	result = GetVolumePathName(filename, volname, MAX_PATH);

	if (!result) {

		ib::error()
			<< "os_is_sparse_file_supported: "
			<< "Failed to get the volume path name for: "
			<< filename
			<< "- OS error number " << GetLastError();

		return(false);
	}

	DWORD	flags;

	result = GetVolumeInformation(
		volname, NULL, MAX_PATH, NULL, NULL,
		&flags, NULL, MAX_PATH);


	if (!result) {
		ib::error()
			<< "os_is_sparse_file_supported: "
			<< "Failed to get the volume info for: "
			<< volname
			<< "- OS error number " << GetLastError();

		return(false);
	}

	return(flags & FILE_SUPPORTS_SPARSE_FILES) ? true : false;
}

/** Free storage space associated with a section of the file.
@param[in]	fh		Open file handle
@param[in]	page_size	Tablespace page size
@param[in]	block_size	File system block size
@param[in]	off		Starting offset (SEEK_SET)
@param[in]	len		Size of the hole
@return 0 on success or errno */
static
dberr_t
os_file_punch_hole_win32(
	os_file_t	fh,
	os_offset_t	off,
	os_offset_t	len)
{
	FILE_ZERO_DATA_INFORMATION	punch;

	punch.FileOffset.QuadPart = off;
	punch.BeyondFinalZero.QuadPart = off + len;

	/* If lpOverlapped is NULL, lpBytesReturned cannot be NULL,
	therefore we pass a dummy parameter. */
	DWORD	temp;
	BOOL	success = os_win32_device_io_control(
		fh, FSCTL_SET_ZERO_DATA, &punch, sizeof(punch),
		NULL, 0, &temp);

	return(success ? DB_SUCCESS: DB_IO_NO_PUNCH_HOLE);
}

/** Check the existence and type of the given file.
@param[in]	path		path name of file
@param[out]	exists		true if the file exists
@param[out]	type		Type of the file, if it exists
@return true if call succeeded */
static
bool
os_file_status_win32(
	const char*	path,
	bool*		exists,
	os_file_type_t* type)
{
	int		ret;
	struct _stat64	statinfo;

	ret = _stat64(path, &statinfo);

	*exists = !ret;

	if (!ret) {
		/* file exists, everything OK */

	} else if (errno == ENOENT || errno == ENOTDIR || errno == ENAMETOOLONG) {
		/* file does not exist */
		return(true);

	} else {
		/* file exists, but stat call failed */
		os_file_handle_error_no_exit(path, "stat", false);
		return(false);
	}

	if (_S_IFDIR & statinfo.st_mode) {
		*type = OS_FILE_TYPE_DIR;

	} else if (_S_IFREG & statinfo.st_mode) {
		*type = OS_FILE_TYPE_FILE;

	} else {
		*type = OS_FILE_TYPE_UNKNOWN;
	}

	return(true);
}

/** NOTE! Use the corresponding macro os_file_flush(), not directly this
function!
Flushes the write buffers of a given file to the disk.
@param[in]	file		handle to a file
@return true if success */
bool
os_file_flush_func(
	os_file_t	file)
{
	++os_n_fsyncs;

	BOOL	ret = FlushFileBuffers(file);

	if (ret) {
		return(true);
	}

	/* Since Windows returns ERROR_INVALID_FUNCTION if the 'file' is
	actually a raw device, we choose to ignore that error if we are using
	raw disks */

	if (srv_start_raw_disk_in_use && GetLastError()
	    == ERROR_INVALID_FUNCTION) {
		return(true);
	}

	os_file_handle_error(NULL, "flush");

	/* It is a fatal error if a file flush does not succeed, because then
	the database can get corrupt on disk */
	ut_error;

	return(false);
}

/** Retrieves the last error number if an error occurs in a file io function.
The number should be retrieved before any other OS calls (because they may
overwrite the error number). If the number is not known to this program,
the OS error number + 100 is returned.
@param[in]	report_all_errors	true if we want an error message printed
					of all errors
@param[in]	on_error_silent		true then don't print any diagnostic
					to the log
@return error number, or OS error number + 100 */
static
ulint
os_file_get_last_error_low(
	bool	report_all_errors,
	bool	on_error_silent)
{
	ulint	err = (ulint) GetLastError();

	if (err == ERROR_SUCCESS) {
		return(0);
	}

	if (report_all_errors
	    || (!on_error_silent
		&& err != ERROR_DISK_FULL
		&& err != ERROR_FILE_EXISTS)) {

		ib::error()
			<< "Operating system error number " << err
			<< " in a file operation.";

		if (err == ERROR_PATH_NOT_FOUND) {
			ib::error()
				<< "The error means the system"
				" cannot find the path specified.";

			if (srv_is_being_started) {
				ib::error()
					<< "If you are installing InnoDB,"
					" remember that you must create"
					" directories yourself, InnoDB"
					" does not create them.";
			}

		} else if (err == ERROR_ACCESS_DENIED) {

			ib::error()
				<< "The error means mysqld does not have"
				" the access rights to"
				" the directory. It may also be"
				" you have created a subdirectory"
				" of the same name as a data file.";

		} else if (err == ERROR_SHARING_VIOLATION
			   || err == ERROR_LOCK_VIOLATION) {

			ib::error()
				<< "The error means that another program"
				" is using InnoDB's files."
				" This might be a backup or antivirus"
				" software or another instance"
				" of MySQL."
				" Please close it to get rid of this error.";

		} else if (err == ERROR_WORKING_SET_QUOTA
			   || err == ERROR_NO_SYSTEM_RESOURCES) {

			ib::error()
				<< "The error means that there are no"
				" sufficient system resources or quota to"
				" complete the operation.";

		} else if (err == ERROR_OPERATION_ABORTED) {

			ib::error()
				<< "The error means that the I/O"
				" operation has been aborted"
				" because of either a thread exit"
				" or an application request."
				" Retry attempt is made.";
		} else {

			ib::info() << OPERATING_SYSTEM_ERROR_MSG;
		}
	}

	if (err == ERROR_FILE_NOT_FOUND) {
		return(OS_FILE_NOT_FOUND);
	} else if (err == ERROR_DISK_FULL) {
		return(OS_FILE_DISK_FULL);
	} else if (err == ERROR_FILE_EXISTS) {
		return(OS_FILE_ALREADY_EXISTS);
	} else if (err == ERROR_SHARING_VIOLATION
		   || err == ERROR_LOCK_VIOLATION) {
		return(OS_FILE_SHARING_VIOLATION);
	} else if (err == ERROR_WORKING_SET_QUOTA
		   || err == ERROR_NO_SYSTEM_RESOURCES) {
		return(OS_FILE_INSUFFICIENT_RESOURCE);
	} else if (err == ERROR_OPERATION_ABORTED) {
		return(OS_FILE_OPERATION_ABORTED);
	} else if (err == ERROR_ACCESS_DENIED) {
		return(OS_FILE_ACCESS_VIOLATION);
	}

	return(OS_FILE_ERROR_MAX + err);
}


/** NOTE! Use the corresponding macro os_file_create_simple(), not directly
this function!
A simple function to open or create a file.
@param[in]	name		name of the file or path as a null-terminated
				string
@param[in]	create_mode	create mode
@param[in]	access_type	OS_FILE_READ_ONLY or OS_FILE_READ_WRITE
@param[in]	read_only	if true read only mode checks are enforced
@param[out]	success		true if succeed, false if error
@return handle to the file, not defined if error, error number
	can be retrieved with os_file_get_last_error */
pfs_os_file_t
os_file_create_simple_func(
	const char*	name,
	ulint		create_mode,
	ulint		access_type,
	bool		read_only,
	bool*		success)
{
	os_file_t	file;

	*success = false;

	DWORD		access;
	DWORD		create_flag;
	DWORD		attributes = 0;

	ut_a(!(create_mode & OS_FILE_ON_ERROR_SILENT));
	ut_a(!(create_mode & OS_FILE_ON_ERROR_NO_EXIT));
	ut_ad(srv_operation == SRV_OPERATION_NORMAL);

	if (create_mode == OS_FILE_OPEN) {

		create_flag = OPEN_EXISTING;

	} else if (read_only) {

		create_flag = OPEN_EXISTING;

	} else if (create_mode == OS_FILE_CREATE) {

		create_flag = CREATE_NEW;

	} else if (create_mode == OS_FILE_CREATE_PATH) {

		/* Create subdirs along the path if needed. */
		*success = os_file_create_subdirs_if_needed(name);

		if (!*success) {

			ib::error()
				<< "Unable to create subdirectories '"
				<< name << "'";

			return(OS_FILE_CLOSED);
		}

		create_flag = CREATE_NEW;
		create_mode = OS_FILE_CREATE;

	} else {

		ib::error()
			<< "Unknown file create mode ("
			<< create_mode << ") for file '"
			<< name << "'";

		return(OS_FILE_CLOSED);
	}

	if (access_type == OS_FILE_READ_ONLY) {

		access = GENERIC_READ;

	} else if (read_only) {

		ib::info()
			<< "Read only mode set. Unable to"
			" open file '" << name << "' in RW mode, "
			<< "trying RO mode", name;

		access = GENERIC_READ;

	} else if (access_type == OS_FILE_READ_WRITE) {

		access = GENERIC_READ | GENERIC_WRITE;

	} else {

		ib::error()
			<< "Unknown file access type (" << access_type << ") "
			"for file '" << name << "'";

		return(OS_FILE_CLOSED);
	}

	bool	retry;

	do {
		/* Use default security attributes and no template file. */

		file = CreateFile(
			(LPCTSTR) name, access,
			FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
			create_flag, attributes, NULL);

		if (file == INVALID_HANDLE_VALUE) {

			*success = false;

			retry = os_file_handle_error(
				name, create_mode == OS_FILE_OPEN ?
				"open" : "create");

		} else {

			retry = false;

			*success = true;
		}

	} while (retry);

	return(file);
}

/** This function attempts to create a directory named pathname. The new
directory gets default permissions. On Unix the permissions are
(0770 & ~umask). If the directory exists already, nothing is done and
the call succeeds, unless the fail_if_exists arguments is true.
If another error occurs, such as a permission error, this does not crash,
but reports the error and returns false.
@param[in]	pathname	directory name as null-terminated string
@param[in]	fail_if_exists	if true, pre-existing directory is treated
				as an error.
@return true if call succeeds, false on error */
bool
os_file_create_directory(
	const char*	pathname,
	bool		fail_if_exists)
{
	BOOL	rcode;

	rcode = CreateDirectory((LPCTSTR) pathname, NULL);
	if (!(rcode != 0
	      || (GetLastError() == ERROR_ALREADY_EXISTS
		  && !fail_if_exists))) {

		os_file_handle_error_no_exit(
			pathname, "CreateDirectory", false);

		return(false);
	}

	return(true);
}

/** The os_file_opendir() function opens a directory stream corresponding to the
directory named by the dirname argument. The directory stream is positioned
at the first entry. In both Unix and Windows we automatically skip the '.'
and '..' items at the start of the directory listing.
@param[in]	dirname		directory name; it must not contain a trailing
				'\' or '/'
@param[in]	is_fatal	true if we should treat an error as a fatal
				error; if we try to open symlinks then we do
				not wish a fatal error if it happens not to
				be a directory
@return directory stream, NULL if error */
os_file_dir_t
os_file_opendir(
	const char*	dirname,
	bool		error_is_fatal)
{
	os_file_dir_t		dir;
	LPWIN32_FIND_DATA	lpFindFileData;
	char			path[OS_FILE_MAX_PATH + 3];

	ut_a(strlen(dirname) < OS_FILE_MAX_PATH);

	strcpy(path, dirname);
	strcpy(path + strlen(path), "\\*");

	/* Note that in Windows opening the 'directory stream' also retrieves
	the first entry in the directory. Since it is '.', that is no problem,
	as we will skip over the '.' and '..' entries anyway. */

	lpFindFileData = static_cast<LPWIN32_FIND_DATA>(
		ut_malloc_nokey(sizeof(WIN32_FIND_DATA)));

	dir = FindFirstFile((LPCTSTR) path, lpFindFileData);

	ut_free(lpFindFileData);

	if (dir == INVALID_HANDLE_VALUE) {

		if (error_is_fatal) {
			os_file_handle_error(dirname, "opendir");
		}

		return(NULL);
	}

	return(dir);
}

/** Closes a directory stream.
@param[in]	dir	directory stream
@return 0 if success, -1 if failure */
int
os_file_closedir(
	os_file_dir_t	dir)
{
	BOOL		ret;

	ret = FindClose(dir);

	if (!ret) {
		os_file_handle_error_no_exit(NULL, "closedir", false);

		return(-1);
	}

	return(0);
}

/** This function returns information of the next file in the directory. We
jump over the '.' and '..' entries in the directory.
@param[in]	dirname		directory name or path
@param[in]	dir		directory stream
@param[out]	info		buffer where the info is returned
@return 0 if ok, -1 if error, 1 if at the end of the directory */
int
os_file_readdir_next_file(
	const char*	dirname,
	os_file_dir_t	dir,
	os_file_stat_t*	info)
{
	BOOL		ret;
	int		status;
	WIN32_FIND_DATA	find_data;

next_file:

	ret = FindNextFile(dir, &find_data);

	if (ret > 0) {

		const char* name;

		name = static_cast<const char*>(find_data.cFileName);

		ut_a(strlen(name) < OS_FILE_MAX_PATH);

		if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {

			goto next_file;
		}

		strcpy(info->name, name);

		info->size = find_data.nFileSizeHigh;
		info->size <<= 32;
		info->size |= find_data.nFileSizeLow;

		if (find_data.dwFileAttributes
		    & FILE_ATTRIBUTE_REPARSE_POINT) {

			/* TODO: test Windows symlinks */
			/* TODO: MySQL has apparently its own symlink
			implementation in Windows, dbname.sym can
			redirect a database directory:
			REFMAN "windows-symbolic-links.html" */

			info->type = OS_FILE_TYPE_LINK;

		} else if (find_data.dwFileAttributes
			   & FILE_ATTRIBUTE_DIRECTORY) {

			info->type = OS_FILE_TYPE_DIR;

		} else {

			/* It is probably safest to assume that all other
			file types are normal. Better to check them rather
			than blindly skip them. */

			info->type = OS_FILE_TYPE_FILE;
		}

		status = 0;

	} else if (GetLastError() == ERROR_NO_MORE_FILES) {

		status = 1;

	} else {

		os_file_handle_error_no_exit(NULL, "readdir_next_file", false);

		status = -1;
	}

	return(status);
}

/** Check that IO of specific size is possible for the file
opened with FILE_FLAG_NO_BUFFERING.

The requirement is that IO is multiple of the disk sector size.

@param[in]	file      file handle
@param[in]	io_size   expected io size
@return true - unbuffered io of requested size is possible, false otherwise.

@note: this function only works correctly with Windows 8 or later,
(GetFileInformationByHandleEx with FileStorageInfo is only supported there).
It will return true on earlier Windows version.
 */
static bool unbuffered_io_possible(HANDLE file, size_t io_size)
{
	FILE_STORAGE_INFO info;
	if (GetFileInformationByHandleEx(
		file, FileStorageInfo, &info, sizeof(info))) {
			ULONG sector_size = info.LogicalBytesPerSector;
			if (sector_size)
				return io_size % sector_size == 0;
	}
	return true;
}


/** NOTE! Use the corresponding macro os_file_create(), not directly
this function!
Opens an existing file or creates a new.
@param[in]	name		name of the file or path as a null-terminated
				string
@param[in]	create_mode	create mode
@param[in]	purpose		OS_FILE_AIO, if asynchronous, non-buffered I/O
				is desired, OS_FILE_NORMAL, if any normal file;
				NOTE that it also depends on type, os_aio_..
				and srv_.. variables whether we really use async
				I/O or unbuffered I/O: look in the function
				source code for the exact rules
@param[in]	type		OS_DATA_FILE or OS_LOG_FILE
@param[in]	success		true if succeeded
@return handle to the file, not defined if error, error number
	can be retrieved with os_file_get_last_error */
pfs_os_file_t
os_file_create_func(
	const char*	name,
	ulint		create_mode,
	ulint		purpose,
	ulint		type,
	bool		read_only,
	bool*		success)
{
	os_file_t	file;
	bool		retry;
	bool		on_error_no_exit;
	bool		on_error_silent;

	*success = false;

	DBUG_EXECUTE_IF(
		"ib_create_table_fail_disk_full",
		*success = false;
		SetLastError(ERROR_DISK_FULL);
		return(OS_FILE_CLOSED);
	);

	DWORD		create_flag;
	DWORD		share_mode = srv_operation != SRV_OPERATION_NORMAL
		? FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE
		: FILE_SHARE_READ | FILE_SHARE_DELETE;

	if (create_mode != OS_FILE_OPEN && create_mode != OS_FILE_OPEN_RAW) {
		WAIT_ALLOW_WRITES();
	}

	on_error_no_exit = create_mode & OS_FILE_ON_ERROR_NO_EXIT
		? true : false;

	on_error_silent = create_mode & OS_FILE_ON_ERROR_SILENT
		? true : false;

	create_mode &= ~(OS_FILE_ON_ERROR_NO_EXIT | OS_FILE_ON_ERROR_SILENT);

	if (create_mode == OS_FILE_OPEN_RAW) {

		ut_a(!read_only);

		create_flag = OPEN_EXISTING;

		/* On Windows Physical devices require admin privileges and
		have to have the write-share mode set. See the remarks
		section for the CreateFile() function documentation in MSDN. */

		share_mode |= FILE_SHARE_WRITE;

	} else if (create_mode == OS_FILE_OPEN
		   || create_mode == OS_FILE_OPEN_RETRY) {

		create_flag = OPEN_EXISTING;

	} else if (read_only) {

		create_flag = OPEN_EXISTING;

	} else if (create_mode == OS_FILE_CREATE) {

		create_flag = CREATE_NEW;

	} else if (create_mode == OS_FILE_OVERWRITE) {

		create_flag = CREATE_ALWAYS;

	} else {
		ib::error()
			<< "Unknown file create mode (" << create_mode << ") "
			<< " for file '" << name << "'";

		return(OS_FILE_CLOSED);
	}

	DWORD		attributes = 0;

	if (purpose == OS_FILE_AIO) {

#ifdef WIN_ASYNC_IO
		/* If specified, use asynchronous (overlapped) io and no
		buffering of writes in the OS */

		if (srv_use_native_aio) {
			attributes |= FILE_FLAG_OVERLAPPED;
		}
#endif /* WIN_ASYNC_IO */

	} else if (purpose == OS_FILE_NORMAL) {

		/* Use default setting. */

	} else {

		ib::error()
			<< "Unknown purpose flag (" << purpose << ") "
			<< "while opening file '" << name << "'";

		return(OS_FILE_CLOSED);
	}

	if (type == OS_LOG_FILE) {
		/* There is not reason to use buffered write to logs.*/
		attributes |= FILE_FLAG_NO_BUFFERING;
	}

	switch (srv_file_flush_method)
	{
	case SRV_O_DSYNC: 
		if (type == OS_LOG_FILE) {
			/* Map O_SYNC to FILE_WRITE_THROUGH */
			attributes |= FILE_FLAG_WRITE_THROUGH;
		}
		break;

	case SRV_O_DIRECT_NO_FSYNC:
	case SRV_O_DIRECT:
		if (type == OS_DATA_FILE) {
			attributes |= FILE_FLAG_NO_BUFFERING;
		}
		break;

	case SRV_ALL_O_DIRECT_FSYNC:
		/*Traditional Windows behavior, no buffering for any files.*/
		if (type != OS_DATA_FILE_NO_O_DIRECT) {
			attributes |= FILE_FLAG_NO_BUFFERING;
		}
		break;

	case SRV_FSYNC:
	case SRV_LITTLESYNC:
		break;

	case SRV_NOSYNC:
		/* Let Windows cache manager handle all writes.*/
		attributes &= ~(FILE_FLAG_WRITE_THROUGH | FILE_FLAG_NO_BUFFERING);
		break;

	default:
		ut_a(false); /* unknown flush mode.*/
	}


	// TODO: Create a bug, this looks wrong. The flush log
	// parameter is dynamic.
	if (type == OS_LOG_FILE && srv_flush_log_at_trx_commit == 2) {
		/* Do not use unbuffered i/o for the log files because
		value 2 denotes that we do not flush the log at every
		commit, but only once per second */
		attributes &= ~(FILE_FLAG_WRITE_THROUGH | FILE_FLAG_NO_BUFFERING);
	}


	DWORD	access = GENERIC_READ;

	if (!read_only) {
		access |= GENERIC_WRITE;
	}

	for (;;) {
		const  char *operation;

		/* Use default security attributes and no template file. */
		file = CreateFile(
			name, access, share_mode, NULL,
			create_flag, attributes, NULL);

		/* If FILE_FLAG_NO_BUFFERING was set, check if this can work at all,
		for expected IO sizes. Reopen without the unbuffered flag, if it is won't work*/
		if ((file != INVALID_HANDLE_VALUE)
			&& (attributes & FILE_FLAG_NO_BUFFERING)
			&& (type == OS_LOG_FILE)
			&& !unbuffered_io_possible(file, OS_FILE_LOG_BLOCK_SIZE)) {
				ut_a(CloseHandle(file));
				attributes &= ~FILE_FLAG_NO_BUFFERING;
				create_flag = OPEN_ALWAYS;
				continue;
		}

		*success = (file != INVALID_HANDLE_VALUE);
		if (*success) {
			break;
		}

		operation = (create_mode == OS_FILE_CREATE && !read_only) ?
			"create" : "open";

		if (on_error_no_exit) {
			retry = os_file_handle_error_no_exit(
				name, operation, on_error_silent);
		}
		else {
			retry = os_file_handle_error(name, operation);
		}

		if (!retry) {
			break;
		}
	}

	if (*success && srv_use_native_aio &&  (attributes & FILE_FLAG_OVERLAPPED)) {
		/* Bind the file handle to completion port. Completion port
		might not be created yet, in some stages of backup, but
		must always be there for the server.*/
		HANDLE port = (type == OS_LOG_FILE) ?
			log_completion_port : data_completion_port;
		ut_a(port || srv_operation != SRV_OPERATION_NORMAL);
		if (port) {
			ut_a(CreateIoCompletionPort(file, port, 0, 0));
		}
	}

	return(file);
}

/** NOTE! Use the corresponding macro os_file_create_simple_no_error_handling(),
not directly this function!
A simple function to open or create a file.
@param[in]	name		name of the file or path as a null-terminated
				string
@param[in]	create_mode	create mode
@param[in]	access_type	OS_FILE_READ_ONLY, OS_FILE_READ_WRITE, or
				OS_FILE_READ_ALLOW_DELETE; the last option is
				used by a backup program reading the file
@param[out]	success		true if succeeded
@return own: handle to the file, not defined if error, error number
	can be retrieved with os_file_get_last_error */
pfs_os_file_t
os_file_create_simple_no_error_handling_func(
	const char*	name,
	ulint		create_mode,
	ulint		access_type,
	bool		read_only,
	bool*		success)
{
	os_file_t	file;

	*success = false;

	DWORD		access;
	DWORD		create_flag;
	DWORD		attributes	= 0;
	DWORD		share_mode = srv_operation != SRV_OPERATION_NORMAL
		? FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE
		: FILE_SHARE_READ | FILE_SHARE_DELETE;

	ut_a(name);

	ut_a(!(create_mode & OS_FILE_ON_ERROR_SILENT));
	ut_a(!(create_mode & OS_FILE_ON_ERROR_NO_EXIT));

	if (create_mode == OS_FILE_OPEN) {

		create_flag = OPEN_EXISTING;

	} else if (read_only) {

		create_flag = OPEN_EXISTING;

	} else if (create_mode == OS_FILE_CREATE) {

		create_flag = CREATE_NEW;

	} else {

		ib::error()
			<< "Unknown file create mode (" << create_mode << ") "
			<< " for file '" << name << "'";

		return(OS_FILE_CLOSED);
	}

	if (access_type == OS_FILE_READ_ONLY) {

		access = GENERIC_READ;

	} else if (read_only) {

		access = GENERIC_READ;

	} else if (access_type == OS_FILE_READ_WRITE) {

		access = GENERIC_READ | GENERIC_WRITE;

	} else if (access_type == OS_FILE_READ_ALLOW_DELETE) {

		ut_a(!read_only);

		access = GENERIC_READ;

		/*!< A backup program has to give mysqld the maximum
		freedom to do what it likes with the file */

		share_mode |= FILE_SHARE_DELETE | FILE_SHARE_WRITE
			| FILE_SHARE_READ;
	} else {

		ib::error()
			<< "Unknown file access type (" << access_type << ") "
			<< "for file '" << name << "'";

		return(OS_FILE_CLOSED);
	}

	file = CreateFile((LPCTSTR) name,
			  access,
			  share_mode,
			  NULL,			// Security attributes
			  create_flag,
			  attributes,
			  NULL);		// No template file

	*success = (file != INVALID_HANDLE_VALUE);

	return(file);
}

/** Deletes a file if it exists. The file has to be closed before calling this.
@param[in]	name		file path as a null-terminated string
@param[out]	exist		indicate if file pre-exist
@return true if success */
bool
os_file_delete_if_exists_func(
	const char*	name,
	bool*		exist)
{
	ulint	count	= 0;

	if (exist != NULL) {
		*exist = true;
	}

	for (;;) {
		/* In Windows, deleting an .ibd file may fail if
		the file is being accessed by an external program,
		such as a backup tool. */

		bool	ret = DeleteFile((LPCTSTR) name);

		if (ret) {
			return(true);
		}

		DWORD	lasterr = GetLastError();

		if (lasterr == ERROR_FILE_NOT_FOUND
		    || lasterr == ERROR_PATH_NOT_FOUND) {

			/* the file does not exist, this not an error */
			if (exist != NULL) {
				*exist = false;
			}

			return(true);
		}

		++count;

		if (count > 100 && 0 == (count % 10)) {

			/* Print error information */
			os_file_get_last_error(true);

			ib::warn() << "Delete of file '" << name << "' failed.";
		}

		/* Sleep for a second */
		os_thread_sleep(1000000);

		if (count > 2000) {

			return(false);
		}
	}
}

/** Deletes a file. The file has to be closed before calling this.
@param[in]	name		File path as NUL terminated string
@return true if success */
bool
os_file_delete_func(
	const char*	name)
{
	ulint	count	= 0;

	for (;;) {
		/* In Windows, deleting an .ibd file may fail if
		the file is being accessed by an external program,
		such as a backup tool. */

		BOOL	ret = DeleteFile((LPCTSTR) name);

		if (ret) {
			return(true);
		}

		if (GetLastError() == ERROR_FILE_NOT_FOUND) {
			/* If the file does not exist, we classify this as
			a 'mild' error and return */

			return(false);
		}

		++count;

		if (count > 100 && 0 == (count % 10)) {

			/* print error information */
			os_file_get_last_error(true);

			ib::warn()
				<< "Cannot delete file '" << name << "'. Is "
				<< "another program accessing it?";
		}

		/* sleep for a second */
		os_thread_sleep(1000000);

		if (count > 2000) {

			return(false);
		}
	}

	ut_error;
	return(false);
}

/** NOTE! Use the corresponding macro os_file_rename(), not directly this
function!
Renames a file (can also move it to another directory). It is safest that the
file is closed before calling this function.
@param[in]	oldpath		old file path as a null-terminated string
@param[in]	newpath		new file path
@return true if success */
bool
os_file_rename_func(
	const char*	oldpath,
	const char*	newpath)
{
#ifdef UNIV_DEBUG
	os_file_type_t	type;
	bool		exists;

	/* New path must not exist. */
	ut_ad(os_file_status(newpath, &exists, &type));
	ut_ad(!exists);

	/* Old path must exist. */
	ut_ad(os_file_status(oldpath, &exists, &type));
	ut_ad(exists);
#endif /* UNIV_DEBUG */

	if (MoveFile((LPCTSTR) oldpath, (LPCTSTR) newpath)) {
		return(true);
	}

	os_file_handle_rename_error(oldpath, newpath);
	return(false);
}

/** NOTE! Use the corresponding macro os_file_close(), not directly
this function!
Closes a file handle. In case of error, error number can be retrieved with
os_file_get_last_error.
@param[in,own]	file		Handle to a file
@return true if success */
bool
os_file_close_func(
	os_file_t	file)
{
	ut_a(file > 0);

	if (CloseHandle(file)) {
		return(true);
	}

	os_file_handle_error(NULL, "close");

	return(false);
}

/** Gets a file size.
@param[in]	file		Handle to a file
@return file size, or (os_offset_t) -1 on failure */
os_offset_t
os_file_get_size(
	os_file_t	file)
{
	DWORD		high;
	DWORD		low = GetFileSize(file, &high);

	if (low == 0xFFFFFFFF && GetLastError() != NO_ERROR) {
		return((os_offset_t) -1);
	}

	return(os_offset_t(low | (os_offset_t(high) << 32)));
}

/** Gets a file size.
@param[in]	filename	Full path to the filename to check
@return file size if OK, else set m_total_size to ~0 and m_alloc_size to
	errno */
os_file_size_t
os_file_get_size(
	const char*	filename)
{
	struct __stat64	s;
	os_file_size_t	file_size;

	int		ret = _stat64(filename, &s);

	if (ret == 0) {

		file_size.m_total_size = s.st_size;

		DWORD	low_size;
		DWORD	high_size;

		low_size = GetCompressedFileSize(filename, &high_size);

		if (low_size != INVALID_FILE_SIZE) {

			file_size.m_alloc_size = high_size;
			file_size.m_alloc_size <<= 32;
			file_size.m_alloc_size |= low_size;

		} else {
			ib::error()
				<< "GetCompressedFileSize("
				<< filename << ", ..) failed.";

			file_size.m_alloc_size = (os_offset_t) -1;
		}
	} else {
		file_size.m_total_size = ~0;
		file_size.m_alloc_size = (os_offset_t) ret;
	}

	return(file_size);
}

/** This function returns information about the specified file
@param[in]	path		pathname of the file
@param[out]	stat_info	information of a file in a directory
@param[in,out]	statinfo	information of a file in a directory
@param[in]	check_rw_perm	for testing whether the file can be opened
				in RW mode
@param[in]	read_only	true if the file is opened in read-only mode
@return DB_SUCCESS if all OK */
static
dberr_t
os_file_get_status_win32(
	const char*	path,
	os_file_stat_t* stat_info,
	struct _stat64*	statinfo,
	bool		check_rw_perm,
	bool		read_only)
{
	int	ret = _stat64(path, statinfo);

	if (ret && (errno == ENOENT || errno == ENOTDIR
		    || errno == ENAMETOOLONG)) {
		/* file does not exist */

		return(DB_NOT_FOUND);

	} else if (ret) {
		/* file exists, but stat call failed */

		os_file_handle_error_no_exit(path, "STAT", false);

		return(DB_FAIL);

	} else if (_S_IFDIR & statinfo->st_mode) {

		stat_info->type = OS_FILE_TYPE_DIR;

	} else if (_S_IFREG & statinfo->st_mode) {

		DWORD	access = GENERIC_READ;

		if (!read_only) {
			access |= GENERIC_WRITE;
		}

		stat_info->type = OS_FILE_TYPE_FILE;

		/* Check if we can open it in read-only mode. */

		if (check_rw_perm) {
			HANDLE	fh;

			fh = CreateFile(
				(LPCTSTR) path,		// File to open
				access,
				FILE_SHARE_READ | FILE_SHARE_WRITE
				| FILE_SHARE_DELETE,	// Full sharing
				NULL,			// Default security
				OPEN_EXISTING,		// Existing file only
				FILE_ATTRIBUTE_NORMAL,	// Normal file
				NULL);			// No attr. template

			if (fh == INVALID_HANDLE_VALUE) {
				stat_info->rw_perm = false;
			} else {
				stat_info->rw_perm = true;
				CloseHandle(fh);
			}
		}
	} else {
		stat_info->type = OS_FILE_TYPE_UNKNOWN;
	}

	return(DB_SUCCESS);
}

/**
Sets a sparse flag on Windows file.
@param[in]	file  file handle
@return true on success, false on error
*/
#include <versionhelpers.h>
bool os_file_set_sparse_win32(os_file_t file, bool is_sparse)
{
	if (!is_sparse && !IsWindows8OrGreater()) {
		/* Cannot  unset sparse flag on older Windows.
		Until Windows8 it is documented to produce unpredictable results,
		if there are unallocated ranges in file.*/
		return false;
	}
	DWORD temp;
	FILE_SET_SPARSE_BUFFER sparse_buffer;
	sparse_buffer.SetSparse = is_sparse;
	return os_win32_device_io_control(file,
		FSCTL_SET_SPARSE, &sparse_buffer, sizeof(sparse_buffer), 0, 0,&temp);
}


/**
Change file size on Windows.

If file is extended, the bytes between old and new EOF
are zeros.

If file is sparse, "virtual" block is added at the end of
allocated area.

If file is normal, file system allocates storage.

@param[in]	pathname	file path
@param[in]	file		file handle
@param[in]	size		size to preserve in bytes
@return true if success */
bool
os_file_change_size_win32(
	const char*	pathname,
	os_file_t	file,
	os_offset_t	size)
{
	LARGE_INTEGER	length;

	length.QuadPart = size;

	BOOL	success = SetFilePointerEx(file, length, NULL, FILE_BEGIN);

	if (!success) {
		os_file_handle_error_no_exit(
			pathname, "SetFilePointerEx", false);
	} else {
		success = SetEndOfFile(file);
		if (!success) {
			os_file_handle_error_no_exit(
				pathname, "SetEndOfFile", false);
		}
	}
	return(success);
}

/** Truncates a file at its current position.
@param[in]	file		Handle to be truncated
@return true if success */
bool
os_file_set_eof(
	FILE*		file)
{
	HANDLE	h = (HANDLE) _get_osfhandle(fileno(file));

	return(SetEndOfFile(h));
}

/** This function can be called if one wants to post a batch of reads and
prefers an i/o-handler thread to handle them all at once later. You must
call os_aio_simulated_wake_handler_threads later to ensure the threads
are not left sleeping! */
void
os_aio_simulated_put_read_threads_to_sleep()
{
	AIO::simulated_put_read_threads_to_sleep();
}

/** This function can be called if one wants to post a batch of reads and
prefers an i/o-handler thread to handle them all at once later. You must
call os_aio_simulated_wake_handler_threads later to ensure the threads
are not left sleeping! */
void
AIO::simulated_put_read_threads_to_sleep()
{
	/* The idea of putting background IO threads to sleep is only for
	Windows when using simulated AIO. Windows XP seems to schedule
	background threads too eagerly to allow for coalescing during
	readahead requests. */

	if (srv_use_native_aio) {
		/* We do not use simulated AIO: do nothing */

		return;
	}

	os_aio_recommend_sleep_for_read_threads	= true;

	for (ulint i = 0; i < os_aio_n_segments; i++) {
		AIO*	array;

		get_array_and_local_segment(&array, i);

		if (array == s_reads) {

			os_event_reset(os_aio_segment_wait_events[i]);
		}
	}
}

#endif /* !_WIN32*/

/** Does a syncronous read or write depending upon the type specified
In case of partial reads/writes the function tries
NUM_RETRIES_ON_PARTIAL_IO times to read/write the complete data.
@param[in]	type,		IO flags
@param[in]	file		handle to an open file
@param[out]	buf		buffer where to read
@param[in]	offset		file offset from the start where to read
@param[in]	n		number of bytes to read, starting from offset
@param[out]	err		DB_SUCCESS or error code
@return number of bytes read/written, -1 if error */
static MY_ATTRIBUTE((warn_unused_result))
ssize_t
os_file_io(
	const IORequest&in_type,
	os_file_t	file,
	void*		buf,
	ulint		n,
	os_offset_t	offset,
	dberr_t*	err)
{
	ulint		original_n = n;
	IORequest	type = in_type;
	ssize_t		bytes_returned = 0;

	SyncFileIO	sync_file_io(file, buf, n, offset);

	for (ulint i = 0; i < NUM_RETRIES_ON_PARTIAL_IO; ++i) {

		ssize_t	n_bytes = sync_file_io.execute(type);

		/* Check for a hard error. Not much we can do now. */
		if (n_bytes < 0) {

			break;

		} else if ((ulint) n_bytes + bytes_returned == n) {

			bytes_returned += n_bytes;

			if (offset > 0
			    && !type.is_log()
			    && type.is_write()
			    && type.punch_hole()) {
				*err = type.punch_hole(file, offset, n);

			} else {
				*err = DB_SUCCESS;
			}

			return(original_n);
		}

		/* Handle partial read/write. */

		ut_ad((ulint) n_bytes + bytes_returned < n);

		bytes_returned += (ulint) n_bytes;

		if (!type.is_partial_io_warning_disabled()) {

			const char*	op = type.is_read()
				? "read" : "written";

			ib::warn()
				<< n
				<< " bytes should have been " << op << ". Only "
				<< bytes_returned
				<< " bytes " << op << ". Retrying"
				<< " for the remaining bytes.";
		}

		/* Advance the offset and buffer by n_bytes */
		sync_file_io.advance(n_bytes);
	}

	*err = DB_IO_ERROR;

	if (!type.is_partial_io_warning_disabled()) {
		ib::warn()
			<< "Retry attempts for "
			<< (type.is_read() ? "reading" : "writing")
			<< " partial data failed.";
	}

	return(bytes_returned);
}

/** Does a synchronous write operation in Posix.
@param[in]	type		IO context
@param[in]	file		handle to an open file
@param[out]	buf		buffer from which to write
@param[in]	n		number of bytes to read, starting from offset
@param[in]	offset		file offset from the start where to read
@param[out]	err		DB_SUCCESS or error code
@return number of bytes written, -1 if error */
static MY_ATTRIBUTE((warn_unused_result))
ssize_t
os_file_pwrite(
	const IORequest&	type,
	os_file_t		file,
	const byte*		buf,
	ulint			n,
	os_offset_t		offset,
	dberr_t*		err)
{
	ut_ad(type.validate());
	ut_ad(type.is_write());

	++os_n_file_writes;

	const bool monitor = MONITOR_IS_ON(MONITOR_OS_PENDING_WRITES);
	MONITOR_ATOMIC_INC_LOW(MONITOR_OS_PENDING_WRITES, monitor);
	ssize_t	n_bytes = os_file_io(type, file, const_cast<byte*>(buf),
				     n, offset, err);
	MONITOR_ATOMIC_DEC_LOW(MONITOR_OS_PENDING_WRITES, monitor);

	return(n_bytes);
}

/** NOTE! Use the corresponding macro os_file_write(), not directly
Requests a synchronous write operation.
@param[in]	type		IO flags
@param[in]	file		handle to an open file
@param[out]	buf		buffer from which to write
@param[in]	offset		file offset from the start where to read
@param[in]	n		number of bytes to read, starting from offset
@return error code
@retval	DB_SUCCESS	if the operation succeeded */
dberr_t
os_file_write_func(
	const IORequest&	type,
	const char*		name,
	os_file_t		file,
	const void*		buf,
	os_offset_t		offset,
	ulint			n)
{
	dberr_t		err;

	ut_ad(type.validate());
	ut_ad(n > 0);

	WAIT_ALLOW_WRITES();

	ssize_t	n_bytes = os_file_pwrite(type, file, (byte*)buf, n, offset, &err);

	if ((ulint) n_bytes != n && !os_has_said_disk_full) {

		ib::error()
			<< "Write to file " << name << " failed at offset "
			<< offset << ", " << n
			<< " bytes should have been written,"
			" only " << n_bytes << " were written."
			" Operating system error number " << IF_WIN(GetLastError(),errno) << "."
			" Check that your OS and file system"
			" support files of this size."
			" Check also that the disk is not full"
			" or a disk quota exceeded.";
#ifndef _WIN32
		if (strerror(errno) != NULL) {

			ib::error()
				<< "Error number " << errno
				<< " means '" << strerror(errno) << "'";
		}

		ib::info() << OPERATING_SYSTEM_ERROR_MSG;
#endif
		os_has_said_disk_full = true;
	}

	return(err);
}

/** Does a synchronous read operation in Posix.
@param[in]	type		IO flags
@param[in]	file		handle to an open file
@param[out]	buf		buffer where to read
@param[in]	offset		file offset from the start where to read
@param[in]	n		number of bytes to read, starting from offset
@param[out]	err		DB_SUCCESS or error code
@return number of bytes read, -1 if error */
static MY_ATTRIBUTE((warn_unused_result))
ssize_t
os_file_pread(
	const IORequest&	type,
	os_file_t		file,
	void*			buf,
	ulint			n,
	os_offset_t		offset,
	dberr_t*		err)
{
	ut_ad(type.is_read());

	++os_n_file_reads;

	const bool monitor = MONITOR_IS_ON(MONITOR_OS_PENDING_READS);
	MONITOR_ATOMIC_INC_LOW(MONITOR_OS_PENDING_READS, monitor);
	ssize_t	n_bytes = os_file_io(type, file, buf, n, offset, err);
	MONITOR_ATOMIC_DEC_LOW(MONITOR_OS_PENDING_READS, monitor);

	return(n_bytes);
}

/** Requests a synchronous positioned read operation.
@return DB_SUCCESS if request was successful, false if fail
@param[in]	type		IO flags
@param[in]	file		handle to an open file
@param[out]	buf		buffer where to read
@param[in]	offset		file offset from the start where to read
@param[in]	n		number of bytes to read, starting from offset
@param[out]	o		number of bytes actually read
@param[in]	exit_on_err	if true then exit on error
@return DB_SUCCESS or error code */
static MY_ATTRIBUTE((warn_unused_result))
dberr_t
os_file_read_page(
	const IORequest&	type,
	os_file_t		file,
	void*			buf,
	os_offset_t		offset,
	ulint			n,
	ulint*			o,
	bool			exit_on_err)
{
	dberr_t		err;

	os_bytes_read_since_printout += n;

	ut_ad(type.validate());
	ut_ad(n > 0);

	ssize_t	n_bytes = os_file_pread(type, file, buf, n, offset, &err);

	if (o) {
		*o = n_bytes;
	}

	if (ulint(n_bytes) == n || (err != DB_SUCCESS && !exit_on_err)) {
		return err;
	}

	ib::error() << "Tried to read " << n << " bytes at offset "
		    << offset << ", but was only able to read " << n_bytes;

	if (!os_file_handle_error_cond_exit(
		    NULL, "read", exit_on_err, false)) {
		ib::fatal()
			<< "Cannot read from file. OS error number "
			<< errno << ".";
	}

	if (err == DB_SUCCESS) {
		err = DB_IO_ERROR;
	}

	return err;
}

/** Retrieves the last error number if an error occurs in a file io function.
The number should be retrieved before any other OS calls (because they may
overwrite the error number). If the number is not known to this program,
the OS error number + 100 is returned.
@param[in]	report_all_errors	true if we want an error printed
					for all errors
@return error number, or OS error number + 100 */
ulint
os_file_get_last_error(
	bool	report_all_errors)
{
	return(os_file_get_last_error_low(report_all_errors, false));
}

/** Handle errors for file operations.
@param[in]	name		name of a file or NULL
@param[in]	operation	operation
@param[in]	should_abort	whether to abort on an unknown error
@param[in]	on_error_silent	whether to suppress reports of non-fatal errors
@return true if we should retry the operation */
static MY_ATTRIBUTE((warn_unused_result))
bool
os_file_handle_error_cond_exit(
	const char*	name,
	const char*	operation,
	bool		should_abort,
	bool		on_error_silent)
{
	ulint	err;

	err = os_file_get_last_error_low(false, on_error_silent);

	switch (err) {
	case OS_FILE_DISK_FULL:
		/* We only print a warning about disk full once */

		if (os_has_said_disk_full) {

			return(false);
		}

		/* Disk full error is reported irrespective of the
		on_error_silent setting. */

		if (name) {

			ib::error()
				<< "Encountered a problem with file '"
				<< name << "'";
		}

		ib::error()
			<< "Disk is full. Try to clean the disk to free space.";

		os_has_said_disk_full = true;

		return(false);

	case OS_FILE_AIO_RESOURCES_RESERVED:
	case OS_FILE_AIO_INTERRUPTED:

		return(true);

	case OS_FILE_PATH_ERROR:
	case OS_FILE_ALREADY_EXISTS:
	case OS_FILE_ACCESS_VIOLATION:

		return(false);

	case OS_FILE_SHARING_VIOLATION:

		os_thread_sleep(10000000);	/* 10 sec */
		return(true);

	case OS_FILE_OPERATION_ABORTED:
	case OS_FILE_INSUFFICIENT_RESOURCE:

		os_thread_sleep(100000);	/* 100 ms */
		return(true);

	default:

		/* If it is an operation that can crash on error then it
		is better to ignore on_error_silent and print an error message
		to the log. */

		if (should_abort || !on_error_silent) {
			ib::error() << "File "
				<< (name != NULL ? name : "(unknown)")
				<< ": '" << operation << "'"
				" returned OS error " << err << "."
				<< (should_abort
				    ? " Cannot continue operation" : "");
		}

		if (should_abort) {
			abort();
		}
	}

	return(false);
}

#ifndef _WIN32
/** Tries to disable OS caching on an opened file descriptor.
@param[in]	fd		file descriptor to alter
@param[in]	file_name	file name, used in the diagnostic message
@param[in]	name		"open" or "create"; used in the diagnostic
				message */
void
os_file_set_nocache(
	int	fd		MY_ATTRIBUTE((unused)),
	const char*	file_name	MY_ATTRIBUTE((unused)),
	const char*	operation_name	MY_ATTRIBUTE((unused)))
{
	/* some versions of Solaris may not have DIRECTIO_ON */
#if defined(UNIV_SOLARIS) && defined(DIRECTIO_ON)
	if (directio(fd, DIRECTIO_ON) == -1) {
		int	errno_save = errno;

		ib::error()
			<< "Failed to set DIRECTIO_ON on file "
			<< file_name << ": " << operation_name
			<< strerror(errno_save) << ","
			" continuing anyway.";
	}
#elif defined(O_DIRECT)
	if (fcntl(fd, F_SETFL, O_DIRECT) == -1) {
		int		errno_save = errno;
		static bool	warning_message_printed = false;
		if (errno_save == EINVAL) {
			if (!warning_message_printed) {
				warning_message_printed = true;
# ifdef UNIV_LINUX
				ib::warn()
					<< "Failed to set O_DIRECT on file"
					<< file_name << ";" << operation_name
					<< ": " << strerror(errno_save) << ", "
					<< "ccontinuing anyway. O_DIRECT is "
					"known to result in 'Invalid argument' "
					"on Linux on tmpfs, "
					"see MySQL Bug#26662.";
# else /* UNIV_LINUX */
				goto short_warning;
# endif /* UNIV_LINUX */
			}
		} else {
# ifndef UNIV_LINUX
short_warning:
# endif
			ib::warn()
				<< "Failed to set O_DIRECT on file "
				<< file_name << "; " << operation_name
				<< " : " << strerror(errno_save)
				<< " continuing anyway.";
		}
	}
#endif /* defined(UNIV_SOLARIS) && defined(DIRECTIO_ON) */
}

#endif /* _WIN32 */

/** Extend a file.

On Windows, extending a file allocates blocks for the file,
unless the file is sparse.

On Unix, we will extend the file with ftruncate(), if
file needs to be sparse. Otherwise posix_fallocate() is used
when available, and if not, binary zeroes are added to the end
of file.

@param[in]	name	file name
@param[in]	file	file handle
@param[in]	size	desired file size
@param[in]	sparse	whether to create a sparse file (no preallocating)
@return	whether the operation succeeded */
bool
os_file_set_size(
	const char*	name,
	os_file_t	file,
	os_offset_t	size,
	bool	is_sparse)
{
	ut_ad(!(size & 4095));

#ifdef _WIN32
	/* On Windows, changing file size works well and as expected for both
	sparse and normal files.

	However, 10.2 up until 10.2.9 made every file sparse in innodb,
	causing NTFS fragmentation issues(MDEV-13941). We try to undo
	the damage, and unsparse the file.*/

	if (!is_sparse && os_is_sparse_file_supported(file)) {
		if (!os_file_set_sparse_win32(file, false))
			/* Unsparsing file failed. Fallback to writing binary
			zeros, to avoid even higher fragmentation.*/
			goto fallback;
	}

	return os_file_change_size_win32(name, file, size);

fallback:
#else
	struct stat statbuf;

	if (is_sparse) {
		bool success = !ftruncate(file, size);
		if (!success) {
			ib::error() << "ftruncate of file " << name << " to "
				    << size << " bytes failed with error "
				    << errno;
		}
		return(success);
	}

# ifdef HAVE_POSIX_FALLOCATE
	int err;
	do {
		if (fstat(file, &statbuf)) {
			err = errno;
		} else {
			os_offset_t current_size = statbuf.st_size;
			if (current_size >= size) {
				return true;
			}
			current_size &= ~4095ULL;
			err = posix_fallocate(file, current_size,
					      size - current_size);
		}
	} while (err == EINTR
		 && srv_shutdown_state <= SRV_SHUTDOWN_INITIATED);

	switch (err) {
	case 0:
		return true;
	default:
		ib::error() << "preallocating "
			    << size << " bytes for file " << name
			    << " failed with error " << err;
		/* fall through */
	case EINTR:
		errno = err;
		return false;
	case EINVAL:
	case EOPNOTSUPP:
		/* fall back to the code below */
		break;
	}
# endif /* HAVE_POSIX_ALLOCATE */
#endif /* _WIN32*/

#ifdef _WIN32
	os_offset_t	current_size = os_file_get_size(file);
	FILE_STORAGE_INFO info;
	if (GetFileInformationByHandleEx(file, FileStorageInfo, &info,
					 sizeof info)) {
		if (info.LogicalBytesPerSector) {
			current_size &= ~os_offset_t(info.LogicalBytesPerSector
						     - 1);
		}
	}
#else
	if (fstat(file, &statbuf)) {
		return false;
	}
	os_offset_t current_size = statbuf.st_size & ~4095ULL;
#endif
	if (current_size >= size) {
		return true;
	}

	/* Write up to 1 megabyte at a time. */
	ulint	buf_size = ut_min(
		static_cast<ulint>(64),
		static_cast<ulint>(size / UNIV_PAGE_SIZE));

	buf_size *= UNIV_PAGE_SIZE;

	/* Align the buffer for possible raw i/o */
	byte*	buf2;

	buf2 = static_cast<byte*>(ut_malloc_nokey(buf_size + UNIV_PAGE_SIZE));

	byte*	buf = static_cast<byte*>(ut_align(buf2, UNIV_PAGE_SIZE));

	/* Write buffer full of zeros */
	memset(buf, 0, buf_size);

	while (current_size < size
	       && srv_shutdown_state <= SRV_SHUTDOWN_INITIATED) {
		ulint	n_bytes;

		if (size - current_size < (os_offset_t) buf_size) {
			n_bytes = (ulint) (size - current_size);
		} else {
			n_bytes = buf_size;
		}

		dberr_t		err;
		IORequest	request(IORequest::WRITE);

		err = os_file_write(
			request, name, file, buf, current_size, n_bytes);

		if (err != DB_SUCCESS) {
			break;
		}

		current_size += n_bytes;
	}

	ut_free(buf2);

	return(current_size >= size && os_file_flush(file));
}

/** Truncate a file to a specified size in bytes.
@param[in]	pathname	file path
@param[in]	file		file to be truncated
@param[in]	size		size preserved in bytes
@param[in]	allow_shrink	whether to allow the file to become smaller
@return true if success */
bool
os_file_truncate(
	const char*	pathname,
	os_file_t	file,
	os_offset_t	size,
	bool		allow_shrink)
{
	if (!allow_shrink) {
		/* Do nothing if the size preserved is larger than or
		equal to the current size of file */
		os_offset_t	size_bytes = os_file_get_size(file);

		if (size >= size_bytes) {
			return(true);
		}
	}

#ifdef _WIN32
	return(os_file_change_size_win32(pathname, file, size));
#else /* _WIN32 */
	return(os_file_truncate_posix(pathname, file, size));
#endif /* _WIN32 */
}

/** NOTE! Use the corresponding macro os_file_read(), not directly this
function!
Requests a synchronous positioned read operation.
@return DB_SUCCESS if request was successful, DB_IO_ERROR on failure
@param[in]	type		IO flags
@param[in]	file		handle to an open file
@param[out]	buf		buffer where to read
@param[in]	offset		file offset from the start where to read
@param[in]	n		number of bytes to read, starting from offset
@return error code
@retval	DB_SUCCESS	if the operation succeeded */
dberr_t
os_file_read_func(
	const IORequest&	type,
	os_file_t		file,
	void*			buf,
	os_offset_t		offset,
	ulint			n)
{
	return(os_file_read_page(type, file, buf, offset, n, NULL, true));
}

/** NOTE! Use the corresponding macro os_file_read_no_error_handling(),
not directly this function!
Requests a synchronous positioned read operation.
@return DB_SUCCESS if request was successful, DB_IO_ERROR on failure
@param[in]	type		IO flags
@param[in]	file		handle to an open file
@param[out]	buf		buffer where to read
@param[in]	offset		file offset from the start where to read
@param[in]	n		number of bytes to read, starting from offset
@param[out]	o		number of bytes actually read
@return DB_SUCCESS or error code */
dberr_t
os_file_read_no_error_handling_func(
	const IORequest&	type,
	os_file_t		file,
	void*			buf,
	os_offset_t		offset,
	ulint			n,
	ulint*			o)
{
	return(os_file_read_page(type, file, buf, offset, n, o, false));
}

/** Check the existence and type of the given file.
@param[in]	path		path name of file
@param[out]	exists		true if the file exists
@param[out]	type		Type of the file, if it exists
@return true if call succeeded */
bool
os_file_status(
	const char*	path,
	bool*		exists,
	os_file_type_t* type)
{
#ifdef _WIN32
	return(os_file_status_win32(path, exists, type));
#else
	return(os_file_status_posix(path, exists, type));
#endif /* _WIN32 */
}

/** Free storage space associated with a section of the file.
@param[in]	fh		Open file handle
@param[in]	off		Starting offset (SEEK_SET)
@param[in]	len		Size of the hole
@return DB_SUCCESS or error code */
dberr_t
os_file_punch_hole(
	os_file_t	fh,
	os_offset_t	off,
	os_offset_t	len)
{
	dberr_t err;

#ifdef _WIN32
	err = os_file_punch_hole_win32(fh, off, len);
#else
	err = os_file_punch_hole_posix(fh, off, len);
#endif /* _WIN32 */

	return (err);
}

/** Free storage space associated with a section of the file.
@param[in]	fh		Open file handle
@param[in]	off		Starting offset (SEEK_SET)
@param[in]	len		Size of the hole
@return DB_SUCCESS or error code */
dberr_t
IORequest::punch_hole(os_file_t fh, os_offset_t off, ulint len)
{
	/* In this debugging mode, we act as if punch hole is supported,
	and then skip any calls to actually punch a hole here.
	In this way, Transparent Page Compression is still being tested. */
	DBUG_EXECUTE_IF("ignore_punch_hole",
		return(DB_SUCCESS);
	);

	ulint trim_len = get_trim_length(len);

	if (trim_len == 0) {
		return(DB_SUCCESS);
	}

	off += len;

	/* Check does file system support punching holes for this
	tablespace. */
	if (!should_punch_hole() || !srv_use_trim) {
		return DB_IO_NO_PUNCH_HOLE;
	}

	dberr_t err = os_file_punch_hole(fh, off, trim_len);

	if (err == DB_SUCCESS) {
		srv_stats.page_compressed_trim_op.inc();
	} else {
		/* If punch hole is not supported,
		set space so that it is not used. */
		if (err == DB_IO_NO_PUNCH_HOLE) {
			space_no_punch_hole();
			err = DB_SUCCESS;
		}
	}

	return (err);
}

/** Check if the file system supports sparse files.

Warning: On POSIX systems we try and punch a hole from offset 0 to
the system configured page size. This should only be called on an empty
file.
@param[in]	fh		File handle for the file - if opened
@return true if the file system supports sparse files */
bool
os_is_sparse_file_supported(os_file_t fh)
{
	/* In this debugging mode, we act as if punch hole is supported,
	then we skip any calls to actually punch a hole.  In this way,
	Transparent Page Compression is still being tested. */
	DBUG_EXECUTE_IF("ignore_punch_hole",
		return(true);
	);

#ifdef _WIN32
	FILE_ATTRIBUTE_TAG_INFO info;
	if (GetFileInformationByHandleEx(fh, FileAttributeTagInfo,
		&info, (DWORD)sizeof(info))) {
		if (info.FileAttributes != INVALID_FILE_ATTRIBUTES) {
			return (info.FileAttributes & FILE_ATTRIBUTE_SPARSE_FILE) != 0;
		}
	}
	return false;
#else
	dberr_t	err;

	/* We don't know the FS block size, use the sector size. The FS
	will do the magic. */
	err = os_file_punch_hole_posix(fh, 0, UNIV_PAGE_SIZE);

	return(err == DB_SUCCESS);
#endif /* _WIN32 */
}

/*
  Get file system block size, by path.

  This is expensive on Windows, and not very useful in general,
  (only shown in some I_S table), so we keep that out of usual
  stat.
*/
size_t os_file_get_fs_block_size(const char *path)
{
#ifdef _WIN32
  char volname[MAX_PATH];
  if (!GetVolumePathName(path, volname, MAX_PATH))
    return 0;
  DWORD sectorsPerCluster;
  DWORD bytesPerSector;
  DWORD numberOfFreeClusters;
  DWORD totalNumberOfClusters;

  if (GetDiskFreeSpace(volname, &sectorsPerCluster, &bytesPerSector,
                       &numberOfFreeClusters, &totalNumberOfClusters))
    return ((size_t) bytesPerSector) * sectorsPerCluster;
#else
  os_file_stat_t info;
  if (os_file_get_status(path, &info, false, false) == DB_SUCCESS)
    return info.block_size;
#endif
  return 0;
}


/** This function returns information about the specified file
@param[in]	path		pathname of the file
@param[out]	stat_info	information of a file in a directory
@param[in]	check_rw_perm	for testing whether the file can be opened
				in RW mode
@param[in]	read_only	true if file is opened in read-only mode
@return DB_SUCCESS if all OK */
dberr_t
os_file_get_status(
	const char*	path,
	os_file_stat_t* stat_info,
	bool		check_rw_perm,
	bool		read_only)
{
	dberr_t	ret;

#ifdef _WIN32
	struct _stat64	info;

	ret = os_file_get_status_win32(
		path, stat_info, &info, check_rw_perm, read_only);

#else
	struct stat	info;

	ret = os_file_get_status_posix(
		path, stat_info, &info, check_rw_perm, read_only);

#endif /* _WIN32 */

	if (ret == DB_SUCCESS) {
		stat_info->ctime = info.st_ctime;
		stat_info->atime = info.st_atime;
		stat_info->mtime = info.st_mtime;
		stat_info->size  = info.st_size;
	}

	return(ret);
}

/**
Waits for an AIO operation to complete. This function is used to wait the
for completed requests. The aio array of pending requests is divided
into segments. The thread specifies which segment or slot it wants to wait
for. NOTE: this function will also take care of freeing the aio slot,
therefore no other thread is allowed to do the freeing!
@param[in]	segment		The number of the segment in the aio arrays to
				wait for; segment 0 is the ibuf I/O thread,
				segment 1 the log I/O thread, then follow the
				non-ibuf read threads, and as the last are the
				non-ibuf write threads; if this is
				ULINT_UNDEFINED, then it means that sync AIO
				is used, and this parameter is ignored
@param[out]	m1		the messages passed with the AIO request; note
				that also in the case where the AIO operation
				failed, these output parameters are valid and
				can be used to restart the operation,
				for example
@param[out]	m2		callback message
@param[out]	type		OS_FILE_WRITE or ..._READ
@return DB_SUCCESS or error code */
dberr_t
os_aio_handler(
	ulint		segment,
	fil_node_t**	m1,
	void**		m2,
	IORequest*	request)
{
	dberr_t	err;

	if (srv_use_native_aio) {
		srv_set_io_thread_op_info(segment, "native aio handle");

#ifdef WIN_ASYNC_IO

		err = os_aio_windows_handler(segment, 0, m1, m2, request);

#elif defined(LINUX_NATIVE_AIO)

		err = os_aio_linux_handler(segment, m1, m2, request);

#else
		ut_error;

		err = DB_ERROR; /* Eliminate compiler warning */

#endif /* WIN_ASYNC_IO */

	} else {
		srv_set_io_thread_op_info(segment, "simulated aio handle");

		err = os_aio_simulated_handler(segment, m1, m2, request);
	}

	return(err);
}

#ifdef WIN_ASYNC_IO
static HANDLE new_completion_port()
{
	HANDLE h = CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, 0);
	ut_a(h);
	return h;
}
#endif

/** Constructor
@param[in]	id		The latch ID
@param[in]	n		Number of AIO slots
@param[in]	segments	Number of segments */
AIO::AIO(
	latch_id_t	id,
	ulint		n,
	ulint		segments)
	:
	m_slots(n),
	m_n_segments(segments),
	m_n_reserved()
# ifdef LINUX_NATIVE_AIO
	,m_events(m_slots.size())
# endif /* LINUX_NATIVE_AIO */
#ifdef WIN_ASYNC_IO
	,m_completion_port(new_completion_port())
#endif
{
	ut_a(n > 0);
	ut_a(m_n_segments > 0);

	mutex_create(id, &m_mutex);

	m_not_full = os_event_create("aio_not_full");
	m_is_empty = os_event_create("aio_is_empty");

	memset((void*)&m_slots[0], 0x0, sizeof(m_slots[0]) * m_slots.size());
#ifdef LINUX_NATIVE_AIO
	memset(&m_events[0], 0x0, sizeof(m_events[0]) * m_events.size());
#endif /* LINUX_NATIVE_AIO */

	os_event_set(m_is_empty);
}

/** Initialise the slots */
dberr_t
AIO::init_slots()
{
	for (ulint i = 0; i < m_slots.size(); ++i) {
		Slot&	slot = m_slots[i];

		slot.pos = static_cast<uint16_t>(i);

		slot.is_reserved = false;

#ifdef WIN_ASYNC_IO

		slot.array = this;

#elif defined(LINUX_NATIVE_AIO)

		slot.ret = 0;

		slot.n_bytes = 0;

		memset(&slot.control, 0x0, sizeof(slot.control));

#endif /* WIN_ASYNC_IO */
	}

	return(DB_SUCCESS);
}

#ifdef LINUX_NATIVE_AIO
/** Initialise the Linux Native AIO interface */
dberr_t
AIO::init_linux_native_aio()
{

	/* Initialize the io_context_t array. One io_context_t
	per segment in the array. */
	m_aio_ctx.resize(get_n_segments());

	ulint		max_events = slots_per_segment();

	for (std::vector<io_context_t>::iterator it = m_aio_ctx.begin(),
						 end = m_aio_ctx.end();
	     it != end; ++it) {

		if (!linux_create_io_ctx(max_events, *it)) {
			/* If something bad happened during aio setup
			we disable linux native aio.
			This frequently happens when running the test suite
			with many threads on a system with low fs.aio-max-nr!
			*/

			ib::warn()
				<< "Warning: Linux Native AIO disabled "
				<< "because _linux_create_io_ctx() "
				<< "failed. To get rid of this warning you can "
				<< "try increasing system "
				<< "fs.aio-max-nr to 1048576 or larger or "
				<< "setting innodb_use_native_aio = 0 in my.cnf";

			for (std::vector<io_context_t>::iterator it2
			     = m_aio_ctx.begin();
			     it2 != it; ++it2) {
				int ret = io_destroy(*it2);
				ut_a(ret != -EINVAL);
			}

			m_aio_ctx.clear();
			srv_use_native_aio = FALSE;
			return(DB_SUCCESS);
		}
	}

	return(DB_SUCCESS);
}
#endif /* LINUX_NATIVE_AIO */

/** Initialise the array */
dberr_t
AIO::init()
{
	ut_a(!m_slots.empty());


	if (srv_use_native_aio) {
#ifdef LINUX_NATIVE_AIO
		dberr_t	err = init_linux_native_aio();

		if (err != DB_SUCCESS) {
			return(err);
		}

#endif /* LINUX_NATIVE_AIO */
	}

	return(init_slots());
}

/** Creates an aio wait array. Note that we return NULL in case of failure.
We don't care about freeing memory here because we assume that a
failure will result in server refusing to start up.
@param[in]	id		Latch ID
@param[in]	n		maximum number of pending AIO operations
				allowed; n must be divisible by m_n_segments
@param[in]	n_segments	number of segments in the AIO array
@return own: AIO array, NULL on failure */
AIO*
AIO::create(
	latch_id_t	id,
	ulint		n,
	ulint		n_segments)
{
	if ((n % n_segments)) {

		ib::error()
			<< "Maximum number of AIO operations must be "
			<< "divisible by number of segments";

		return(NULL);
	}

	AIO*	array = UT_NEW_NOKEY(AIO(id, n, n_segments));

	if (array != NULL && array->init() != DB_SUCCESS) {

		UT_DELETE(array);

		array = NULL;
	}

	return(array);
}

/** AIO destructor */
AIO::~AIO()
{
	mutex_destroy(&m_mutex);

	os_event_destroy(m_not_full);
	os_event_destroy(m_is_empty);

#if defined(LINUX_NATIVE_AIO)
	if (srv_use_native_aio) {
		for (ulint i = 0; i < m_aio_ctx.size(); i++) {
			int ret = io_destroy(m_aio_ctx[i]);
			ut_a(ret != -EINVAL);
		}
	}
#endif /* LINUX_NATIVE_AIO */
#if defined(WIN_ASYNC_IO)
	CloseHandle(m_completion_port);
#endif
}

/** Initializes the asynchronous io system. Creates one array each for ibuf
and log i/o. Also creates one array each for read and write where each
array is divided logically into n_readers and n_writers
respectively. The caller must create an i/o handler thread for each
segment in these arrays. This function also creates the sync array.
No i/o handler thread needs to be created for that
@param[in]	n_per_seg	maximum number of pending aio
				operations allowed per segment
@param[in]	n_readers	number of reader threads
@param[in]	n_writers	number of writer threads
@param[in]	n_slots_sync	number of slots in the sync aio array
@return true if the AIO sub-system was started successfully */
bool
AIO::start(
	ulint		n_per_seg,
	ulint		n_readers,
	ulint		n_writers,
	ulint		n_slots_sync)
{
#if defined(LINUX_NATIVE_AIO)
	/* Check if native aio is supported on this system and tmpfs */
	if (srv_use_native_aio && !is_linux_native_aio_supported()) {

		ib::warn() << "Linux Native AIO disabled.";

		srv_use_native_aio = FALSE;
	}
#endif /* LINUX_NATIVE_AIO */

	srv_reset_io_thread_op_info();

	s_reads = create(
		LATCH_ID_OS_AIO_READ_MUTEX, n_readers * n_per_seg, n_readers);

	if (s_reads == NULL) {
		return(false);
	}

	ulint	start = srv_read_only_mode ? 0 : 2;
	ulint	n_segs = n_readers + start;

	/* 0 is the ibuf segment and 1 is the redo log segment. */
	for (ulint i = start; i < n_segs; ++i) {
		ut_a(i < SRV_MAX_N_IO_THREADS);
		srv_io_thread_function[i] = "read thread";
	}

	ulint	n_segments = n_readers;

	if (!srv_read_only_mode) {

		s_ibuf = create(LATCH_ID_OS_AIO_IBUF_MUTEX, n_per_seg, 1);

		if (s_ibuf == NULL) {
			return(false);
		}

		++n_segments;

		srv_io_thread_function[0] = "insert buffer thread";

		s_log = create(LATCH_ID_OS_AIO_LOG_MUTEX, n_per_seg, 1);

		if (s_log == NULL) {
			return(false);
		}

		++n_segments;

		srv_io_thread_function[1] = "log thread";

	} else {
		s_ibuf = s_log = NULL;
	}

	s_writes = create(
		LATCH_ID_OS_AIO_WRITE_MUTEX, n_writers * n_per_seg, n_writers);

	if (s_writes == NULL) {
		return(false);
	}

#ifdef WIN_ASYNC_IO
	data_completion_port = s_writes->m_completion_port;
	log_completion_port =
		s_log ? s_log->m_completion_port : data_completion_port;
#endif

	n_segments += n_writers;

	for (ulint i = start + n_readers; i < n_segments; ++i) {
		ut_a(i < SRV_MAX_N_IO_THREADS);
		srv_io_thread_function[i] = "write thread";
	}

	ut_ad(n_segments >= static_cast<ulint>(srv_read_only_mode ? 2 : 4));

	s_sync = create(LATCH_ID_OS_AIO_SYNC_MUTEX, n_slots_sync, 1);

	if (s_sync == NULL) {

		return(false);
	}

	os_aio_n_segments = n_segments;

	os_aio_validate();

	os_last_printout = time(NULL);

	if (srv_use_native_aio) {
		return(true);
	}

	os_aio_segment_wait_events = static_cast<os_event_t*>(
		ut_zalloc_nokey(
			n_segments * sizeof *os_aio_segment_wait_events));

	if (os_aio_segment_wait_events == NULL) {

		return(false);
	}

	for (ulint i = 0; i < n_segments; ++i) {
		os_aio_segment_wait_events[i] = os_event_create(0);
	}

	return(true);
}

/** Free the AIO arrays */
void
AIO::shutdown()
{
	UT_DELETE(s_ibuf);
	s_ibuf = NULL;

	UT_DELETE(s_log);
	s_log = NULL;

	UT_DELETE(s_writes);
	s_writes = NULL;

	UT_DELETE(s_sync);
	s_sync = NULL;

	UT_DELETE(s_reads);
	s_reads = NULL;
}

/** Initializes the asynchronous io system. Creates one array each for ibuf
and log i/o. Also creates one array each for read and write where each
array is divided logically into n_readers and n_writers
respectively. The caller must create an i/o handler thread for each
segment in these arrays. This function also creates the sync array.
No i/o handler thread needs to be created for that
@param[in]	n_readers	number of reader threads
@param[in]	n_writers	number of writer threads
@param[in]	n_slots_sync	number of slots in the sync aio array */
bool
os_aio_init(
	ulint		n_readers,
	ulint		n_writers,
	ulint		n_slots_sync)
{
	/* Maximum number of pending aio operations allowed per segment */
	ulint		limit = 8 * OS_AIO_N_PENDING_IOS_PER_THREAD;

	return(AIO::start(limit, n_readers, n_writers, n_slots_sync));
}

/** Frees the asynchronous io system. */
void
os_aio_free()
{
	AIO::shutdown();

	ut_ad(!os_aio_segment_wait_events || !srv_use_native_aio);
	ut_ad(srv_use_native_aio || os_aio_segment_wait_events
	      || !srv_was_started);

	if (!srv_use_native_aio && os_aio_segment_wait_events) {
		for (ulint i = 0; i < os_aio_n_segments; i++) {
			os_event_destroy(os_aio_segment_wait_events[i]);
		}

		ut_free(os_aio_segment_wait_events);
		os_aio_segment_wait_events = 0;
	}
	os_aio_n_segments = 0;
}

/** Wakes up all async i/o threads so that they know to exit themselves in
shutdown. */
void
os_aio_wake_all_threads_at_shutdown()
{
#ifdef WIN_ASYNC_IO
	AIO::wake_at_shutdown();
#elif defined(LINUX_NATIVE_AIO)
	/* When using native AIO interface the io helper threads
	wait on io_getevents with a timeout value of 500ms. At
	each wake up these threads check the server status.
	No need to do anything to wake them up. */
#endif /* !WIN_ASYNC_AIO */

	if (srv_use_native_aio) {
		return;
	}

	/* This loop wakes up all simulated ai/o threads */

	for (ulint i = 0; i < os_aio_n_segments; ++i) {

		os_event_set(os_aio_segment_wait_events[i]);
	}
}

/** Waits until there are no pending writes in AIO::s_writes. There can
be other, synchronous, pending writes. */
void
os_aio_wait_until_no_pending_writes()
{
	AIO::wait_until_no_pending_writes();
}

/** Calculates segment number for a slot.
@param[in]	array		AIO wait array
@param[in]	slot		slot in this array
@return segment number (which is the number used by, for example,
	I/O-handler threads) */
ulint
AIO::get_segment_no_from_slot(
	const AIO*	array,
	const Slot*	slot)
{
	ulint	segment;
	ulint	seg_len;

	if (array == s_ibuf) {
		ut_ad(!srv_read_only_mode);

		segment = IO_IBUF_SEGMENT;

	} else if (array == s_log) {
		ut_ad(!srv_read_only_mode);

		segment = IO_LOG_SEGMENT;

	} else if (array == s_reads) {
		seg_len = s_reads->slots_per_segment();

		segment = (srv_read_only_mode ? 0 : 2) + slot->pos / seg_len;
	} else {
		ut_a(array == s_writes);

		seg_len = s_writes->slots_per_segment();

		segment = s_reads->m_n_segments
			+ (srv_read_only_mode ? 0 : 2) + slot->pos / seg_len;
	}

	return(segment);
}

/** Requests for a slot in the aio array. If no slot is available, waits until
not_full-event becomes signaled.

@param[in]	type		IO context
@param[in,out]	m1		message to be passed along with the AIO
				operation
@param[in,out]	m2		message to be passed along with the AIO
				operation
@param[in]	file		file handle
@param[in]	name		name of the file or path as a NUL-terminated
				string
@param[in,out]	buf		buffer where to read or from which to write
@param[in]	offset		file offset, where to read from or start writing
@param[in]	len		length of the block to read or write
@return pointer to slot */
Slot*
AIO::reserve_slot(
	const IORequest&	type,
	fil_node_t*		m1,
	void*			m2,
	pfs_os_file_t		file,
	const char*		name,
	void*			buf,
	os_offset_t		offset,
	ulint			len)
{
	ut_ad(reinterpret_cast<size_t>(buf) % OS_FILE_LOG_BLOCK_SIZE == 0);
	ut_ad(offset % OS_FILE_LOG_BLOCK_SIZE == 0);
	ut_ad(len % OS_FILE_LOG_BLOCK_SIZE == 0);

#ifdef WIN_ASYNC_IO
	ut_a((len & 0xFFFFFFFFUL) == len);
#endif /* WIN_ASYNC_IO */

	/* No need of a mutex. Only reading constant fields */
	ulint		slots_per_seg;

	ut_ad(type.validate());

	slots_per_seg = slots_per_segment();

	/* We attempt to keep adjacent blocks in the same local
	segment. This can help in merging IO requests when we are
	doing simulated AIO */
	ulint		local_seg;

	local_seg = (offset >> (UNIV_PAGE_SIZE_SHIFT + 6)) % m_n_segments;

	for (;;) {

		acquire();

		if (m_n_reserved != m_slots.size()) {
			break;
		}

		release();

		if (!srv_use_native_aio) {
			/* If the handler threads are suspended,
			wake them so that we get more slots */

			os_aio_simulated_wake_handler_threads();
		}

		os_event_wait(m_not_full);
	}

	ulint	counter = 0;
	Slot*	slot = NULL;

	/* We start our search for an available slot from our preferred
	local segment and do a full scan of the array. We are
	guaranteed to find a slot in full scan. */
	for (ulint i = local_seg * slots_per_seg;
	     counter < m_slots.size();
	     ++i, ++counter) {

		i %= m_slots.size();

		slot = at(i);

		if (slot->is_reserved == false) {
			break;
		}
	}

	/* We MUST always be able to get hold of a reserved slot. */
	ut_a(counter < m_slots.size());

	ut_a(slot->is_reserved == false);

	++m_n_reserved;

	if (m_n_reserved == 1) {
		os_event_reset(m_is_empty);
	}

	if (m_n_reserved == m_slots.size()) {
		os_event_reset(m_not_full);
	}

	slot->is_reserved = true;
	slot->reservation_time = time(NULL);
	slot->m1       = m1;
	slot->m2       = m2;
	slot->file     = file;
	slot->name     = name;
#ifdef _WIN32
	slot->len      = static_cast<DWORD>(len);
#else
	slot->len      = len;
#endif /* _WIN32 */
	slot->type     = type;
	slot->buf      = static_cast<byte*>(buf);
	slot->ptr      = slot->buf;
	slot->offset   = offset;
	slot->err      = DB_SUCCESS;
	slot->original_len = static_cast<uint32>(len);
	slot->io_already_done = false;
	slot->buf      = static_cast<byte*>(buf);

#ifdef WIN_ASYNC_IO
	{
		OVERLAPPED*	control;

		control = &slot->control;
		control->Offset = (DWORD) offset & 0xFFFFFFFF;
		control->OffsetHigh = (DWORD) (offset >> 32);
	}
#elif defined(LINUX_NATIVE_AIO)

	/* If we are not using native AIO skip this part. */
	if (srv_use_native_aio) {

		off_t		aio_offset;

		/* Check if we are dealing with 64 bit arch.
		If not then make sure that offset fits in 32 bits. */
		aio_offset = (off_t) offset;

		ut_a(sizeof(aio_offset) >= sizeof(offset)
		     || ((os_offset_t) aio_offset) == offset);

		struct iocb*	iocb = &slot->control;

		if (type.is_read()) {

			io_prep_pread(
				iocb, file, slot->ptr, slot->len, aio_offset);
		} else {
			ut_ad(type.is_write());

			io_prep_pwrite(
				iocb, file, slot->ptr, slot->len, aio_offset);
		}

		iocb->data = slot;

		slot->n_bytes = 0;
		slot->ret = 0;
	}
#endif /* LINUX_NATIVE_AIO */

	release();

	return(slot);
}

/** Wakes up a simulated aio i/o-handler thread if it has something to do.
@param[in]	global_segment	The number of the segment in the AIO arrays */
void
AIO::wake_simulated_handler_thread(ulint global_segment)
{
	ut_ad(!srv_use_native_aio);

	AIO*	array;
	ulint	segment = get_array_and_local_segment(&array, global_segment);

	array->wake_simulated_handler_thread(global_segment, segment);
}

/** Wakes up a simulated AIO I/O-handler thread if it has something to do
for a local segment in the AIO array.
@param[in]	global_segment	The number of the segment in the AIO arrays
@param[in]	segment		The local segment in the AIO array */
void
AIO::wake_simulated_handler_thread(ulint global_segment, ulint segment)
{
	ut_ad(!srv_use_native_aio);

	ulint	n = slots_per_segment();
	ulint	offset = segment * n;

	/* Look through n slots after the segment * n'th slot */

	acquire();

	const Slot*	slot = at(offset);

	for (ulint i = 0; i < n; ++i, ++slot) {

		if (slot->is_reserved) {

			/* Found an i/o request */

			release();

			os_event_t	event;

			event = os_aio_segment_wait_events[global_segment];

			os_event_set(event);

			return;
		}
	}

	release();
}

/** Wakes up simulated aio i/o-handler threads if they have something to do. */
void
os_aio_simulated_wake_handler_threads()
{
	if (srv_use_native_aio) {
		/* We do not use simulated aio: do nothing */

		return;
	}

	os_aio_recommend_sleep_for_read_threads	= false;

	for (ulint i = 0; i < os_aio_n_segments; i++) {
		AIO::wake_simulated_handler_thread(i);
	}
}

/** Select the IO slot array
@param[in,out]	type		Type of IO, READ or WRITE
@param[in]	read_only	true if running in read-only mode
@param[in]	mode		IO mode
@return slot array or NULL if invalid mode specified */
AIO*
AIO::select_slot_array(IORequest& type, bool read_only, ulint mode)
{
	AIO*	array;

	ut_ad(type.validate());

	switch (mode) {
	case OS_AIO_NORMAL:

		array = type.is_read() ? AIO::s_reads : AIO::s_writes;
		break;

	case OS_AIO_IBUF:
		ut_ad(type.is_read());

		/* Reduce probability of deadlock bugs in connection with ibuf:
		do not let the ibuf i/o handler sleep */

		type.clear_do_not_wake();

		array = read_only ? AIO::s_reads : AIO::s_ibuf;
		break;

	case OS_AIO_LOG:

		array = read_only ? AIO::s_reads : AIO::s_log;
		break;

	case OS_AIO_SYNC:

		array = AIO::s_sync;
#if defined(LINUX_NATIVE_AIO)
		/* In Linux native AIO we don't use sync IO array. */
		ut_a(!srv_use_native_aio);
#endif /* LINUX_NATIVE_AIO */
		break;

	default:
		ut_error;
		array = NULL; /* Eliminate compiler warning */
	}

	return(array);
}

#ifdef WIN_ASYNC_IO
/** This function is only used in Windows asynchronous i/o.
Waits for an aio operation to complete. This function is used to wait the
for completed requests. The aio array of pending requests is divided
into segments. The thread specifies which segment or slot it wants to wait
for. NOTE: this function will also take care of freeing the aio slot,
therefore no other thread is allowed to do the freeing!
@param[in]	segment		The number of the segment in the aio arrays to
				wait for; segment 0 is the ibuf I/O thread,
				segment 1 the log I/O thread, then follow the
				non-ibuf read threads, and as the last are the
				non-ibuf write threads; if this is
				ULINT_UNDEFINED, then it means that sync AIO
				is used, and this parameter is ignored
@param[in]	pos		this parameter is used only in sync AIO:
				wait for the aio slot at this position
@param[out]	m1		the messages passed with the AIO request; note
				that also in the case where the AIO operation
				failed, these output parameters are valid and
				can be used to restart the operation,
				for example
@param[out]	m2		callback message
@param[out]	type		OS_FILE_WRITE or ..._READ
@return DB_SUCCESS or error code */



static
dberr_t
os_aio_windows_handler(
	ulint		segment,
	ulint		pos,
	fil_node_t**	m1,
	void**		m2,
	IORequest*	type)
{
	Slot*		slot= 0;
	dberr_t		err;

	BOOL		ret;
	ULONG_PTR	key;

	ut_a(segment != ULINT_UNDEFINED);

	/* NOTE! We only access constant fields in os_aio_array. Therefore
	we do not have to acquire the protecting mutex yet */

	ut_ad(os_aio_validate_skip());
	AIO *my_array;
	AIO::get_array_and_local_segment(&my_array, segment);

	HANDLE port = my_array->m_completion_port;
	ut_ad(port);
	for (;;) {
		DWORD len;
		ret = GetQueuedCompletionStatus(port, &len, &key,
		(OVERLAPPED **)&slot, INFINITE);

		/* If shutdown key was received, repost the shutdown message and exit */
		if (ret && key == IOCP_SHUTDOWN_KEY) {
			PostQueuedCompletionStatus(port, 0, key, NULL);
			*m1 = NULL;
			*m2 = NULL;
			return (DB_SUCCESS);
		}

		ut_a(slot);

		if (!ret) {
			/* IO failed */
			break;
		}

		slot->n_bytes= len;
		ut_a(slot->array);
		HANDLE slot_port = slot->array->m_completion_port;
		if (slot_port != port) {
			/* there are no redirections between data and log */
			ut_ad(port == data_completion_port);
			ut_ad(slot_port != log_completion_port);

			/*
			Redirect completions  to the dedicated completion port
			and threads.

			"Write array" threads receive write,read and ibuf
			notifications, read and ibuf completions are redirected.

			Forwarding IO completion this way costs a context switch,
			and this seems tolerable  since asynchronous reads are by
			far less frequent.
			*/
			ut_a(PostQueuedCompletionStatus(slot_port,
				len, key, &slot->control));
		}
		else {
			break;
		}
	}

	ut_a(slot->is_reserved);

	*m1 = slot->m1;
	*m2 = slot->m2;

	*type = slot->type;

	bool retry = false;

	if (ret && slot->n_bytes == slot->len) {

		err = DB_SUCCESS;

	} else if (os_file_handle_error(slot->name, "Windows aio")) {

		retry = true;

	} else {

		err = DB_IO_ERROR;
	}


	if (retry) {
		/* Retry failed read/write operation synchronously. */

#ifdef UNIV_PFS_IO
		/* This read/write does not go through os_file_read
		and os_file_write APIs, need to register with
		performance schema explicitly here. */
		PSI_file_locker_state	state;
		struct PSI_file_locker* locker = NULL;

		register_pfs_file_io_begin(
			&state, locker, slot->file, slot->len,
			slot->type.is_write()
			? PSI_FILE_WRITE : PSI_FILE_READ, __FILE__, __LINE__);
#endif /* UNIV_PFS_IO */

		ut_a((slot->len & 0xFFFFFFFFUL) == slot->len);

		ssize_t	n_bytes = SyncFileIO::execute(slot);

#ifdef UNIV_PFS_IO
		register_pfs_file_io_end(locker, slot->len);
#endif /* UNIV_PFS_IO */

		err = (n_bytes == slot->len) ? DB_SUCCESS : DB_IO_ERROR;
	}

	if (err == DB_SUCCESS) {
		err = AIOHandler::post_io_processing(slot);
	}

	slot->array->release_with_mutex(slot);

	if (srv_shutdown_state == SRV_SHUTDOWN_EXIT_THREADS
		&& !buf_page_cleaner_is_active
		&& os_aio_all_slots_free()) {
			/* Last IO, wakeup other io  threads */
			AIO::wake_at_shutdown();
	}
	return(err);
}
#endif /* WIN_ASYNC_IO */

/**
NOTE! Use the corresponding macro os_aio(), not directly this function!
Requests an asynchronous i/o operation.
@param[in,out]	type		IO request context
@param[in]	mode		IO mode
@param[in]	name		Name of the file or path as NUL terminated
				string
@param[in]	file		Open file handle
@param[out]	buf		buffer where to read
@param[in]	offset		file offset where to read
@param[in]	n		number of bytes to read
@param[in]	read_only	if true read only mode checks are enforced
@param[in,out]	m1		Message for the AIO handler, (can be used to
				identify a completed AIO operation); ignored
				if mode is OS_AIO_SYNC
@param[in,out]	m2		message for the AIO handler (can be used to
				identify a completed AIO operation); ignored
				if mode is OS_AIO_SYNC

@return DB_SUCCESS or error code */
dberr_t
os_aio_func(
	IORequest&	type,
	ulint		mode,
	const char*	name,
	pfs_os_file_t	file,
	void*		buf,
	os_offset_t	offset,
	ulint		n,
	bool		read_only,
	fil_node_t*	m1,
	void*		m2)
{
#ifdef WIN_ASYNC_IO
	BOOL		ret = TRUE;
#endif /* WIN_ASYNC_IO */

	ut_ad(n > 0);
	ut_ad((n % OS_FILE_LOG_BLOCK_SIZE) == 0);
	ut_ad((offset % OS_FILE_LOG_BLOCK_SIZE) == 0);
	ut_ad(os_aio_validate_skip());

#ifdef WIN_ASYNC_IO
	ut_ad((n & 0xFFFFFFFFUL) == n);
#endif /* WIN_ASYNC_IO */

	DBUG_EXECUTE_IF("ib_os_aio_func_io_failure_28",
			mode = OS_AIO_SYNC; os_has_said_disk_full = FALSE;);

	if (mode == OS_AIO_SYNC) {
		if (type.is_read()) {
			return(os_file_read_func(type, file, buf, offset, n));
		}

		ut_ad(type.is_write());

		return(os_file_write_func(type, name, file, buf, offset, n));
	}

try_again:

	AIO*	array;

	array = AIO::select_slot_array(type, read_only, mode);

	Slot*	slot;

	slot = array->reserve_slot(type, m1, m2, file, name, buf, offset, n);

	if (type.is_read()) {


		if (srv_use_native_aio) {

			++os_n_file_reads;

			os_bytes_read_since_printout += n;
#ifdef WIN_ASYNC_IO
			ret = ReadFile(
				file, slot->ptr, slot->len,
				NULL, &slot->control);
#elif defined(LINUX_NATIVE_AIO)
			if (!array->linux_dispatch(slot)) {
				goto err_exit;
			}
#endif /* WIN_ASYNC_IO */
		} else if (type.is_wake()) {
			AIO::wake_simulated_handler_thread(
				AIO::get_segment_no_from_slot(array, slot));
		}
	} else if (type.is_write()) {

		if (srv_use_native_aio) {
			++os_n_file_writes;

#ifdef WIN_ASYNC_IO
			ret = WriteFile(
				file, slot->ptr, slot->len,
				NULL, &slot->control);
#elif defined(LINUX_NATIVE_AIO)
			if (!array->linux_dispatch(slot)) {
				goto err_exit;
			}
#endif /* WIN_ASYNC_IO */

		} else if (type.is_wake()) {
			AIO::wake_simulated_handler_thread(
				AIO::get_segment_no_from_slot(array, slot));
		}
	} else {
		ut_error;
	}

#ifdef WIN_ASYNC_IO
	if (ret || (GetLastError() == ERROR_IO_PENDING)) {
		/* aio completed or was queued successfully! */
		return(DB_SUCCESS);
	}

	goto err_exit;

#endif /* WIN_ASYNC_IO */

	/* AIO request was queued successfully! */
	return(DB_SUCCESS);

#if defined LINUX_NATIVE_AIO || defined WIN_ASYNC_IO
err_exit:
#endif /* LINUX_NATIVE_AIO || WIN_ASYNC_IO */

	array->release_with_mutex(slot);

	if (os_file_handle_error(
		name, type.is_read() ? "aio read" : "aio write")) {

		goto try_again;
	}

	return(DB_IO_ERROR);
}

/** Simulated AIO handler for reaping IO requests */
class SimulatedAIOHandler {

public:

	/** Constructor
	@param[in,out]	array	The AIO array
	@param[in]	segment	Local segment in the array */
	SimulatedAIOHandler(AIO* array, ulint segment)
		:
		m_oldest(),
		m_n_elems(),
		m_lowest_offset(IB_UINT64_MAX),
		m_array(array),
		m_n_slots(),
		m_segment(segment),
		m_ptr(),
		m_buf()
	{
		ut_ad(m_segment < 100);

		m_slots.resize(OS_AIO_MERGE_N_CONSECUTIVE);
	}

	/** Destructor */
	~SimulatedAIOHandler()
	{
		if (m_ptr != NULL) {
			ut_free(m_ptr);
		}
	}

	/** Reset the state of the handler
	@param[in]	n_slots	Number of pending AIO operations supported */
	void init(ulint n_slots)
	{
		m_oldest = 0;
		m_n_elems = 0;
		m_n_slots = n_slots;
		m_lowest_offset = IB_UINT64_MAX;

		if (m_ptr != NULL) {
			ut_free(m_ptr);
			m_ptr = m_buf = NULL;
		}

		m_slots[0] = NULL;
	}

	/** Check if there is a slot for which the i/o has already been done
	@param[out]	n_reserved	Number of reserved slots
	@return the first completed slot that is found. */
	Slot* check_completed(ulint* n_reserved)
	{
		ulint	offset = m_segment * m_n_slots;

		*n_reserved = 0;

		Slot*	slot;

		slot = m_array->at(offset);

		for (ulint i = 0; i < m_n_slots; ++i, ++slot) {

			if (slot->is_reserved) {

				if (slot->io_already_done) {

					ut_a(slot->is_reserved);

					return(slot);
				}

				++*n_reserved;
			}
		}

		return(NULL);
	}

	/** If there are at least 2 seconds old requests, then pick the
	oldest one to prevent starvation.  If several requests have the
	same age, then pick the one at the lowest offset.
	@return true if request was selected */
	bool select()
	{
		if (!select_oldest()) {

			return(select_lowest_offset());
		}

		return(true);
	}

	/** Check if there are several consecutive blocks
	to read or write. Merge them if found. */
	void merge()
	{
		/* if m_n_elems != 0, then we have assigned
		something valid to consecutive_ios[0] */
		ut_ad(m_n_elems != 0);
		ut_ad(first_slot() != NULL);

		Slot*	slot = first_slot();

		while (!merge_adjacent(slot)) {
			/* No op */
		}
	}

	/** We have now collected n_consecutive I/O requests
	in the array; allocate a single buffer which can hold
	all data, and perform the I/O
	@return the length of the buffer */
	ulint allocate_buffer()
		MY_ATTRIBUTE((warn_unused_result))
	{
		ulint	len;
		Slot*	slot = first_slot();

		ut_ad(m_ptr == NULL);

		if (slot->type.is_read() && m_n_elems > 1) {

			len = 0;

			for (ulint i = 0; i < m_n_elems; ++i) {
				len += m_slots[i]->len;
			}

			m_ptr = static_cast<byte*>(
				ut_malloc_nokey(len + UNIV_PAGE_SIZE));

			m_buf = static_cast<byte*>(
				ut_align(m_ptr, UNIV_PAGE_SIZE));

		} else {
			len = first_slot()->len;
			m_buf = first_slot()->buf;
		}

		return(len);
	}

	/** We have to compress the individual pages and punch
	holes in them on a page by page basis when writing to
	tables that can be compresed at the IO level.
	@param[in]	len		Value returned by allocate_buffer */
	void copy_to_buffer(ulint len)
	{
		Slot*	slot = first_slot();

		if (len > slot->len && slot->type.is_write()) {

			byte*	ptr = m_buf;

			ut_ad(ptr != slot->buf);

			/* Copy the buffers to the combined buffer */
			for (ulint i = 0; i < m_n_elems; ++i) {

				slot = m_slots[i];

				memmove(ptr, slot->buf, slot->len);

				ptr += slot->len;
			}
		}
	}

	/** Do the I/O with ordinary, synchronous i/o functions:
	@param[in]	len		Length of buffer for IO */
	void io()
	{
		if (first_slot()->type.is_write()) {

			for (ulint i = 0; i < m_n_elems; ++i) {
				write(m_slots[i]);
			}

		} else {

			for (ulint i = 0; i < m_n_elems; ++i) {
				read(m_slots[i]);
			}
		}
	}

	/** Mark the i/os done in slots */
	void done()
	{
		for (ulint i = 0; i < m_n_elems; ++i) {
			m_slots[i]->io_already_done = true;
		}
	}

	/** @return the first slot in the consecutive array */
	Slot* first_slot()
		MY_ATTRIBUTE((warn_unused_result))
	{
		ut_a(m_n_elems > 0);

		return(m_slots[0]);
	}

	/** Wait for I/O requests
	@param[in]	global_segment	The global segment
	@param[in,out]	event		Wait on event if no active requests
	@return the number of slots */
	ulint check_pending(
		ulint		global_segment,
		os_event_t	event)
		MY_ATTRIBUTE((warn_unused_result));
private:

	/** Do the file read
	@param[in,out]	slot		Slot that has the IO context */
	void read(Slot* slot)
	{
		dberr_t	err = os_file_read(
			slot->type,
			slot->file,
			slot->ptr,
			slot->offset,
			slot->len);

		ut_a(err == DB_SUCCESS);
	}

	/** Do the file read
	@param[in,out]	slot		Slot that has the IO context */
	void write(Slot* slot)
	{
		dberr_t	err = os_file_write(
			slot->type,
			slot->name,
			slot->file,
			slot->ptr,
			slot->offset,
			slot->len);

		ut_a(err == DB_SUCCESS);
	}

	/** @return true if the slots are adjacent and can be merged */
	bool adjacent(const Slot* s1, const Slot* s2) const
	{
		return(s1 != s2
		       && s1->file == s2->file
		       && s2->offset == s1->offset + s1->len
		       && s1->type == s2->type);
	}

	/** @return true if merge limit reached or no adjacent slots found. */
	bool merge_adjacent(Slot*& current)
	{
		Slot*	slot;
		ulint	offset = m_segment * m_n_slots;

		slot = m_array->at(offset);

		for (ulint i = 0; i < m_n_slots; ++i, ++slot) {

			if (slot->is_reserved && adjacent(current, slot)) {

				current = slot;

				/* Found a consecutive i/o request */

				m_slots[m_n_elems] = slot;

				++m_n_elems;

				return(m_n_elems >= m_slots.capacity());
			}
		}

		return(true);
	}

	/** There were no old requests. Look for an I/O request at the lowest
	offset in the array (we ignore the high 32 bits of the offset in these
	heuristics) */
	bool select_lowest_offset()
	{
		ut_ad(m_n_elems == 0);

		ulint	offset = m_segment * m_n_slots;

		m_lowest_offset = IB_UINT64_MAX;

		for (ulint i = 0; i < m_n_slots; ++i) {
			Slot*	slot;

			slot = m_array->at(i + offset);

			if (slot->is_reserved
			    && slot->offset < m_lowest_offset) {

				/* Found an i/o request */
				m_slots[0] = slot;

				m_n_elems = 1;

				m_lowest_offset = slot->offset;
			}
		}

		return(m_n_elems > 0);
	}

	/** Select the slot if it is older than the current oldest slot.
	@param[in]	slot		The slot to check */
	void select_if_older(Slot* slot)
	{
		ulint	age;

		age = (ulint) difftime(time(NULL), slot->reservation_time);

		if ((age >= 2 && age > m_oldest)
		    || (age >= 2
			&& age == m_oldest
			&& slot->offset < m_lowest_offset)) {

			/* Found an i/o request */
			m_slots[0] = slot;

			m_n_elems = 1;

			m_oldest = age;

			m_lowest_offset = slot->offset;
		}
	}

	/** Select th oldest slot in the array
	@return true if oldest slot found */
	bool select_oldest()
	{
		ut_ad(m_n_elems == 0);

		Slot*	slot;
		ulint	offset = m_n_slots * m_segment;

		slot = m_array->at(offset);

		for (ulint i = 0; i < m_n_slots; ++i, ++slot) {

			if (slot->is_reserved) {
				select_if_older(slot);
			}
		}

		return(m_n_elems > 0);
	}

	typedef std::vector<Slot*> slots_t;

private:
	ulint		m_oldest;
	ulint		m_n_elems;
	os_offset_t	m_lowest_offset;

	AIO*		m_array;
	ulint		m_n_slots;
	ulint		m_segment;

	slots_t		m_slots;

	byte*		m_ptr;
	byte*		m_buf;
};

/** Wait for I/O requests
@return the number of slots */
ulint
SimulatedAIOHandler::check_pending(
	ulint		global_segment,
	os_event_t	event)
{
	/* NOTE! We only access constant fields in os_aio_array.
	Therefore we do not have to acquire the protecting mutex yet */

	ut_ad(os_aio_validate_skip());

	ut_ad(m_segment < m_array->get_n_segments());

	/* Look through n slots after the segment * n'th slot */

	if (AIO::is_read(m_array)
	    && os_aio_recommend_sleep_for_read_threads) {

		/* Give other threads chance to add several
		I/Os to the array at once. */

		srv_set_io_thread_op_info(
			global_segment, "waiting for i/o request");

		os_event_wait(event);

		return(0);
	}

	return(m_array->slots_per_segment());
}

/** Does simulated AIO. This function should be called by an i/o-handler
thread.

@param[in]	segment	The number of the segment in the aio arrays to wait
			for; segment 0 is the ibuf i/o thread, segment 1 the
			log i/o thread, then follow the non-ibuf read threads,
			and as the last are the non-ibuf write threads
@param[out]	m1	the messages passed with the AIO request; note that
			also in the case where the AIO operation failed, these
			output parameters are valid and can be used to restart
			the operation, for example
@param[out]	m2	Callback argument
@param[in]	type	IO context
@return DB_SUCCESS or error code */
static
dberr_t
os_aio_simulated_handler(
	ulint		global_segment,
	fil_node_t**	m1,
	void**		m2,
	IORequest*	type)
{
	Slot*		slot;
	AIO*		array;
	ulint		segment;
	os_event_t	event = os_aio_segment_wait_events[global_segment];

	segment = AIO::get_array_and_local_segment(&array, global_segment);

	SimulatedAIOHandler	handler(array, segment);

	for (;;) {

		srv_set_io_thread_op_info(
			global_segment, "looking for i/o requests (a)");

		ulint	n_slots = handler.check_pending(global_segment, event);

		if (n_slots == 0) {
			continue;
		}

		handler.init(n_slots);

		srv_set_io_thread_op_info(
			global_segment, "looking for i/o requests (b)");

		array->acquire();

		ulint	n_reserved;

		slot = handler.check_completed(&n_reserved);

		if (slot != NULL) {

			break;

		} else if (n_reserved == 0
			   && !buf_page_cleaner_is_active
			   && srv_shutdown_state == SRV_SHUTDOWN_EXIT_THREADS) {

			/* There is no completed request. If there
			are no pending request at all, and the system
			is being shut down, exit. */

			array->release();

			*m1 = NULL;

			*m2 = NULL;

			return(DB_SUCCESS);

		} else if (handler.select()) {

			break;
		}

		/* No I/O requested at the moment */

		srv_set_io_thread_op_info(
			global_segment, "resetting wait event");

		/* We wait here until tbere are more IO requests
		for this segment. */

		os_event_reset(event);

		array->release();

		srv_set_io_thread_op_info(
			global_segment, "waiting for i/o request");

		os_event_wait(event);
	}

	/** Found a slot that has already completed its IO */

	if (slot == NULL) {
		/* Merge adjacent requests */
		handler.merge();

		/* Check if there are several consecutive blocks
		to read or write */

		srv_set_io_thread_op_info(
			global_segment, "consecutive i/o requests");

		// Note: We don't support write combining for simulated AIO.
		//ulint	total_len = handler.allocate_buffer();

		/* We release the array mutex for the time of the I/O: NOTE that
		this assumes that there is just one i/o-handler thread serving
		a single segment of slots! */

		array->release();

		// Note: We don't support write combining for simulated AIO.
		//handler.copy_to_buffer(total_len);

		srv_set_io_thread_op_info(global_segment, "doing file i/o");

		handler.io();

		srv_set_io_thread_op_info(global_segment, "file i/o done");

		array->acquire();

		handler.done();

		/* We return the messages for the first slot now, and if there
		were several slots, the messages will be returned with
		subsequent calls of this function */

		slot = handler.first_slot();
	}

	ut_ad(slot->is_reserved);

	*m1 = slot->m1;
	*m2 = slot->m2;

	*type = slot->type;

	array->release(slot);

	array->release();

	return(DB_SUCCESS);
}

/** Get the total number of pending IOs
@return the total number of pending IOs */
ulint
AIO::total_pending_io_count()
{
	ulint	count = s_reads->pending_io_count();

	if (s_writes != NULL) {
		count += s_writes->pending_io_count();
	}

	if (s_ibuf != NULL) {
		count += s_ibuf->pending_io_count();
	}

	if (s_log != NULL) {
		count += s_log->pending_io_count();
	}

	if (s_sync != NULL) {
		count += s_sync->pending_io_count();
	}

	return(count);
}

/** Validates the consistency the aio system.
@return true if ok */
static
bool
os_aio_validate()
{
	/* The methods countds and validates, we ignore the count. */
	AIO::total_pending_io_count();

	return(true);
}

/** Prints pending IO requests per segment of an aio array.
We probably don't need per segment statistics but they can help us
during development phase to see if the IO requests are being
distributed as expected.
@param[in,out]	file		File where to print
@param[in]	segments	Pending IO array */
void
AIO::print_segment_info(
	FILE*		file,
	const ulint*	segments)
{
	ut_ad(m_n_segments > 0);

	if (m_n_segments > 1) {

		fprintf(file, " [");

		for (ulint i = 0; i < m_n_segments; ++i, ++segments) {

			if (i != 0) {
				fprintf(file, ", ");
			}

			fprintf(file, ULINTPF, *segments);
		}

		fprintf(file, "] ");
	}
}

/** Prints info about the aio array.
@param[in,out]	file		Where to print */
void
AIO::print(FILE* file)
{
	ulint	count = 0;
	ulint	n_res_seg[SRV_MAX_N_IO_THREADS];

	mutex_enter(&m_mutex);

	ut_a(!m_slots.empty());
	ut_a(m_n_segments > 0);

	memset(n_res_seg, 0x0, sizeof(n_res_seg));

	for (ulint i = 0; i < m_slots.size(); ++i) {
		Slot&	slot = m_slots[i];
		ulint	segment = (i * m_n_segments) / m_slots.size();

		if (slot.is_reserved) {

			++count;

			++n_res_seg[segment];

			ut_a(slot.len > 0);
		}
	}

	ut_a(m_n_reserved == count);

	print_segment_info(file, n_res_seg);

	mutex_exit(&m_mutex);
}

/** Print all the AIO segments
@param[in,out]	file		Where to print */
void
AIO::print_all(FILE* file)
{
	s_reads->print(file);

	if (s_writes != NULL) {
		fputs(", aio writes:", file);
		s_writes->print(file);
	}

	if (s_ibuf != NULL) {
		fputs(",\n ibuf aio reads:", file);
		s_ibuf->print(file);
	}

	if (s_log != NULL) {
		fputs(", log i/o's:", file);
		s_log->print(file);
	}

	if (s_sync != NULL) {
		fputs(", sync i/o's:", file);
		s_sync->print(file);
	}
}

/** Prints info of the aio arrays.
@param[in,out]	file		file where to print */
void
os_aio_print(FILE*	file)
{
	time_t		current_time;
	double		time_elapsed;
	double		avg_bytes_read;

	for (ulint i = 0; i < srv_n_file_io_threads; ++i) {
		fprintf(file, "I/O thread " ULINTPF " state: %s (%s)",
			i,
			srv_io_thread_op_info[i],
			srv_io_thread_function[i]);

#ifndef _WIN32
		if (!srv_use_native_aio
		    && os_event_is_set(os_aio_segment_wait_events[i])) {
			fprintf(file, " ev set");
		}
#endif /* _WIN32 */

		fprintf(file, "\n");
	}

	fputs("Pending normal aio reads:", file);

	AIO::print_all(file);

	putc('\n', file);
	current_time = time(NULL);
	time_elapsed = 0.001 + difftime(current_time, os_last_printout);

	fprintf(file,
		"Pending flushes (fsync) log: " ULINTPF
		"; buffer pool: " ULINTPF "\n"
		ULINTPF " OS file reads, "
		ULINTPF " OS file writes, "
		ULINTPF " OS fsyncs\n",
		fil_n_pending_log_flushes,
		fil_n_pending_tablespace_flushes,
		os_n_file_reads,
		os_n_file_writes,
		os_n_fsyncs);

	const ulint n_reads = ulint(MONITOR_VALUE(MONITOR_OS_PENDING_READS));
	const ulint n_writes = ulint(MONITOR_VALUE(MONITOR_OS_PENDING_WRITES));

	if (n_reads != 0 || n_writes != 0) {
		fprintf(file,
			ULINTPF " pending reads, " ULINTPF " pending writes\n",
			n_reads, n_writes);
	}

	if (os_n_file_reads == os_n_file_reads_old) {
		avg_bytes_read = 0.0;
	} else {
		avg_bytes_read = (double) os_bytes_read_since_printout
			/ (os_n_file_reads - os_n_file_reads_old);
	}

	fprintf(file,
		"%.2f reads/s, " ULINTPF " avg bytes/read,"
		" %.2f writes/s, %.2f fsyncs/s\n",
		(os_n_file_reads - os_n_file_reads_old)
		/ time_elapsed,
		(ulint) avg_bytes_read,
		(os_n_file_writes - os_n_file_writes_old)
		/ time_elapsed,
		(os_n_fsyncs - os_n_fsyncs_old)
		/ time_elapsed);

	os_n_file_reads_old = os_n_file_reads;
	os_n_file_writes_old = os_n_file_writes;
	os_n_fsyncs_old = os_n_fsyncs;
	os_bytes_read_since_printout = 0;

	os_last_printout = current_time;
}

/** Refreshes the statistics used to print per-second averages. */
void
os_aio_refresh_stats()
{
	os_n_fsyncs_old = os_n_fsyncs;

	os_bytes_read_since_printout = 0;

	os_n_file_reads_old = os_n_file_reads;

	os_n_file_writes_old = os_n_file_writes;

	os_n_fsyncs_old = os_n_fsyncs;

	os_bytes_read_since_printout = 0;

	os_last_printout = time(NULL);
}

/** Checks that all slots in the system have been freed, that is, there are
no pending io operations.
@return true if all free */
bool
os_aio_all_slots_free()
{
	return(AIO::total_pending_io_count() == 0);
}

#ifdef UNIV_DEBUG
/** Prints all pending IO for the array
@param[in]	file	file where to print
@param[in]	array	array to process */
void
AIO::to_file(FILE* file) const
{
	acquire();

	fprintf(file, " " ULINTPF "\n", m_n_reserved);

	for (ulint i = 0; i < m_slots.size(); ++i) {

		const Slot&	slot = m_slots[i];

		if (slot.is_reserved) {

			fprintf(file,
				"%s IO for %s (offset=" UINT64PF
				", size=%lu)\n",
				slot.type.is_read() ? "read" : "write",
				slot.name, slot.offset, (unsigned long)(slot.len));
		}
	}

	release();
}

/** Print pending IOs for all arrays */
void
AIO::print_to_file(FILE* file)
{
	fprintf(file, "Pending normal aio reads:");

	s_reads->to_file(file);

	if (s_writes != NULL) {
		fprintf(file, "Pending normal aio writes:");
		s_writes->to_file(file);
	}

	if (s_ibuf != NULL) {
		fprintf(file, "Pending ibuf aio reads:");
		s_ibuf->to_file(file);
	}

	if (s_log != NULL) {
		fprintf(file, "Pending log i/o's:");
		s_log->to_file(file);
	}

	if (s_sync != NULL) {
		fprintf(file, "Pending sync i/o's:");
		s_sync->to_file(file);
	}
}

/** Prints all pending IO
@param[in]	file		File where to print */
void
os_aio_print_pending_io(
	FILE*	file)
{
	AIO::print_to_file(file);
}

#endif /* UNIV_DEBUG */

/**
Set the file create umask
@param[in]	umask		The umask to use for file creation. */
void
os_file_set_umask(ulint umask)
{
	os_innodb_umask = umask;
}

#else
#include "univ.i"
#endif /* !UNIV_INNOCHECKSUM */

/** Normalizes a directory path for the current OS:
On Windows, we convert '/' to '\', else we convert '\' to '/'.
@param[in,out] str A null-terminated directory and file path */
void
os_normalize_path(
	char*	str)
{
	if (str != NULL) {
		for (; *str; str++) {
			if (*str == OS_PATH_SEPARATOR_ALT) {
				*str = OS_PATH_SEPARATOR;
			}
		}
	}
}
