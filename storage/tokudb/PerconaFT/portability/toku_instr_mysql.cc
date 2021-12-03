#ifdef TOKU_MYSQL_WITH_PFS
#include "toku_portability.h"
#include "toku_pthread.h"

toku_instr_probe_pfs::toku_instr_probe_pfs(const toku_instr_key &key)
    : mutex(new toku_mutex_t) {
    toku_mutex_init(key, mutex.get(), nullptr);
}

toku_instr_probe_pfs::~toku_instr_probe_pfs() {
    toku_mutex_destroy(mutex.get());
}

// Thread instrumentation

int toku_pthread_create(const toku_instr_key &key,
                        pthread_t *thread,
                        const pthread_attr_t *attr,
                        void *(*start_routine)(void *),
                        void *arg) {
#if (50700 <= MYSQL_VERSION_ID && MYSQL_VERSION_ID <= 50799)
    return PSI_THREAD_CALL(spawn_thread)(
        key.id(), reinterpret_cast<my_thread_handle *>(thread),
        attr, start_routine, arg);
#else
    return PSI_THREAD_CALL(spawn_thread)(
        key.id(), thread, attr, start_routine, arg);
#endif
}

void toku_instr_register_current_thread(const toku_instr_key &key) {
    struct PSI_thread *psi_thread =
        PSI_THREAD_CALL(new_thread)(key.id(), nullptr, 0);
    PSI_THREAD_CALL(set_thread)(psi_thread);
}

void toku_instr_delete_current_thread() {
    PSI_THREAD_CALL(delete_current_thread)();
}

// I/O instrumentation

void toku_instr_file_open_begin(toku_io_instrumentation &io_instr,
                                const toku_instr_key &key,
                                toku_instr_file_op op,
                                const char *name,
                                const char *src_file,
                                int src_line) {
    io_instr.locker =
        PSI_FILE_CALL(get_thread_file_name_locker)(
          &io_instr.state, key.id(), static_cast<PSI_file_operation>(op),
          name, io_instr.locker);
    if (io_instr.locker != nullptr) {
        PSI_FILE_CALL(start_file_open_wait)
        (io_instr.locker, src_file, src_line);
    }
}

void toku_instr_file_stream_open_end(toku_io_instrumentation &io_instr,
                                     TOKU_FILE &file) {
    file.key = nullptr;
    if (FT_LIKELY(io_instr.locker)) {
        file.key =
            PSI_FILE_CALL(end_file_open_wait)(io_instr.locker, file.file);
    }
}

void toku_instr_file_open_end(toku_io_instrumentation &io_instr, int fd) {
    if (FT_LIKELY(io_instr.locker))
        PSI_FILE_CALL(end_file_open_wait_and_bind_to_descriptor)
    (io_instr.locker, fd);
}

void toku_instr_file_name_close_begin(toku_io_instrumentation &io_instr,
                                      const toku_instr_key &key,
                                      toku_instr_file_op op,
                                      const char *name,
                                      const char *src_file,
                                      int src_line) {
    io_instr.locker =
        PSI_FILE_CALL(get_thread_file_name_locker)(
          &io_instr.state, key.id(), static_cast<PSI_file_operation>(op),
                                                   name,
                                                   io_instr.locker);
    if (FT_LIKELY(io_instr.locker)) {
        PSI_FILE_CALL(start_file_close_wait)
        (io_instr.locker, src_file, src_line);
    }
}

void toku_instr_file_stream_close_begin(toku_io_instrumentation &io_instr,
                                        toku_instr_file_op op,
                                        const TOKU_FILE &file,
                                        const char *src_file,
                                        int src_line) {
    io_instr.locker = nullptr;
    if (FT_LIKELY(file.key)) {
        io_instr.locker = PSI_FILE_CALL(get_thread_file_stream_locker)(
            &io_instr.state, file.key, (PSI_file_operation)op);
        if (FT_LIKELY(io_instr.locker)) {
            PSI_FILE_CALL(start_file_close_wait)
            (io_instr.locker, src_file, src_line);
        }
    }
}

void toku_instr_file_fd_close_begin(toku_io_instrumentation &io_instr,
                                    toku_instr_file_op op,
                                    int fd,
                                    const char *src_file,
                                    int src_line) {
    io_instr.locker = PSI_FILE_CALL(get_thread_file_descriptor_locker)(
        &io_instr.state, fd, (PSI_file_operation)op);
    if (FT_LIKELY(io_instr.locker)) {
        PSI_FILE_CALL(start_file_close_wait)
        (io_instr.locker, src_file, src_line);
    }
}

void toku_instr_file_close_end(const toku_io_instrumentation &io_instr,
                               int result) {
    if (FT_LIKELY(io_instr.locker))
        PSI_FILE_CALL(end_file_close_wait)
    (io_instr.locker, result);
}

void toku_instr_file_io_begin(toku_io_instrumentation &io_instr,
                              toku_instr_file_op op,
                              int fd,
                              ssize_t count,
                              const char *src_file,
                              int src_line) {
    io_instr.locker = PSI_FILE_CALL(get_thread_file_descriptor_locker)(
        &io_instr.state, fd, (PSI_file_operation)op);
    if (FT_LIKELY(io_instr.locker)) {
        PSI_FILE_CALL(start_file_wait)
        (io_instr.locker, count, src_file, src_line);
    }
}

void toku_instr_file_name_io_begin(toku_io_instrumentation &io_instr,
                                   const toku_instr_key &key,
                                   toku_instr_file_op op,
                                   const char *name,
                                   ssize_t count,
                                   const char *src_file,
                                   int src_line) {
    io_instr.locker =
        PSI_FILE_CALL(get_thread_file_name_locker)(&io_instr.state,
                                                   key.id(),
                                                   (PSI_file_operation)op,
                                                   name,
                                                   &io_instr.locker);
    if (FT_LIKELY(io_instr.locker)) {
        PSI_FILE_CALL(start_file_wait)
        (io_instr.locker, count, src_file, src_line);
    }
}

void toku_instr_file_stream_io_begin(toku_io_instrumentation &io_instr,
                                     toku_instr_file_op op,
                                     const TOKU_FILE &file,
                                     ssize_t count,
                                     const char *src_file,
                                     int src_line) {
    io_instr.locker = nullptr;
    if (FT_LIKELY(file.key)) {
        io_instr.locker = PSI_FILE_CALL(get_thread_file_stream_locker)(
            &io_instr.state, file.key, (PSI_file_operation)op);
        if (FT_LIKELY(io_instr.locker)) {
            PSI_FILE_CALL(start_file_wait)
            (io_instr.locker, count, src_file, src_line);
        }
    }
}

void toku_instr_file_io_end(toku_io_instrumentation &io_instr, ssize_t count) {
    if (FT_LIKELY(io_instr.locker))
        PSI_FILE_CALL(end_file_wait)
    (io_instr.locker, count);
}

// Mutex instrumentation

void toku_instr_mutex_init(const toku_instr_key &key, toku_mutex_t &mutex) {
    mutex.psi_mutex = PSI_MUTEX_CALL(init_mutex)(key.id(), &mutex.pmutex);
#if defined(TOKU_PTHREAD_DEBUG)
    mutex.instr_key_id = key.id();
#endif  // defined(TOKU_PTHREAD_DEBUG)
}

void toku_instr_mutex_destroy(PSI_mutex *&mutex_instr) {
    if (mutex_instr != nullptr) {
        PSI_MUTEX_CALL(destroy_mutex)(mutex_instr);
        mutex_instr = nullptr;
    }
}

void toku_instr_mutex_lock_start(toku_mutex_instrumentation &mutex_instr,
                                 toku_mutex_t &mutex,
                                 const char *src_file,
                                 int src_line) {
    mutex_instr.locker = nullptr;
    if (mutex.psi_mutex) {
        mutex_instr.locker =
            PSI_MUTEX_CALL(start_mutex_wait)(&mutex_instr.state,
                                             mutex.psi_mutex,
                                             PSI_MUTEX_LOCK,
                                             src_file,
                                             src_line);
    }
}

void toku_instr_mutex_trylock_start(toku_mutex_instrumentation &mutex_instr,
                                    toku_mutex_t &mutex,
                                    const char *src_file,
                                    int src_line) {
    mutex_instr.locker = nullptr;
    if (mutex.psi_mutex) {
        mutex_instr.locker =
            PSI_MUTEX_CALL(start_mutex_wait)(&mutex_instr.state,
                                             mutex.psi_mutex,
                                             PSI_MUTEX_TRYLOCK,
                                             src_file,
                                             src_line);
    }
}

void toku_instr_mutex_lock_end(toku_mutex_instrumentation &mutex_instr,
                               int pthread_mutex_lock_result) {
    if (mutex_instr.locker)
        PSI_MUTEX_CALL(end_mutex_wait)
        (mutex_instr.locker, pthread_mutex_lock_result);
}

void toku_instr_mutex_unlock(PSI_mutex *mutex_instr) {
    if (mutex_instr)
        PSI_MUTEX_CALL(unlock_mutex)(mutex_instr);
}

// Condvar instrumentation

void toku_instr_cond_init(const toku_instr_key &key, toku_cond_t &cond) {
    cond.psi_cond = PSI_COND_CALL(init_cond)(key.id(), &cond.pcond);
#if defined(TOKU_PTHREAD_DEBUG)
    cond.instr_key_id = key.id();
#endif  //   // defined(TOKU_PTHREAD_DEBUG)
}

void toku_instr_cond_destroy(PSI_cond *&cond_instr) {
    if (cond_instr != nullptr) {
        PSI_COND_CALL(destroy_cond)(cond_instr);
        cond_instr = nullptr;
    }
}

void toku_instr_cond_wait_start(toku_cond_instrumentation &cond_instr,
                                toku_instr_cond_op op,
                                toku_cond_t &cond,
                                toku_mutex_t &mutex,
                                const char *src_file,
                                int src_line) {
    cond_instr.locker = nullptr;
    if (cond.psi_cond) {
        /* Instrumentation start */
        cond_instr.locker =
            PSI_COND_CALL(start_cond_wait)(&cond_instr.state,
                                           cond.psi_cond,
                                           mutex.psi_mutex,
                                           (PSI_cond_operation)op,
                                           src_file,
                                           src_line);
    }
}

void toku_instr_cond_wait_end(toku_cond_instrumentation &cond_instr,
                              int pthread_cond_wait_result) {
    if (cond_instr.locker)
        PSI_COND_CALL(end_cond_wait)
        (cond_instr.locker, pthread_cond_wait_result);
}

void toku_instr_cond_signal(const toku_cond_t &cond) {
    if (cond.psi_cond)
        PSI_COND_CALL(signal_cond)(cond.psi_cond);
}

void toku_instr_cond_broadcast(const toku_cond_t &cond) {
    if (cond.psi_cond)
        PSI_COND_CALL(broadcast_cond)(cond.psi_cond);
}

// rwlock instrumentation

void toku_instr_rwlock_init(const toku_instr_key &key,
                            toku_pthread_rwlock_t &rwlock) {
    rwlock.psi_rwlock = PSI_RWLOCK_CALL(init_rwlock)(key.id(), &rwlock.rwlock);
#if defined(TOKU_PTHREAD_DEBUG)
    rwlock.instr_key_id = key.id();
#endif  // defined(TOKU_PTHREAD_DEBUG)
}

void toku_instr_rwlock_destroy(PSI_rwlock *&rwlock_instr) {
    if (rwlock_instr != nullptr) {
        PSI_RWLOCK_CALL(destroy_rwlock)(rwlock_instr);
        rwlock_instr = nullptr;
    }
}

void toku_instr_rwlock_rdlock_wait_start(
    toku_rwlock_instrumentation &rwlock_instr,
    toku_pthread_rwlock_t &rwlock,
    const char *src_file,
    int src_line) {
    rwlock_instr.locker = nullptr;
    if (rwlock.psi_rwlock) {
        /* Instrumentation start */
        rwlock_instr.locker =
            PSI_RWLOCK_CALL(start_rwlock_rdwait)(&rwlock_instr.state,
                                                 rwlock.psi_rwlock,
                                                 PSI_RWLOCK_READLOCK,
                                                 src_file,
                                                 src_line);
    }
}

void toku_instr_rwlock_wrlock_wait_start(
    toku_rwlock_instrumentation &rwlock_instr,
    toku_pthread_rwlock_t &rwlock,
    const char *src_file,
    int src_line) {
    rwlock_instr.locker = nullptr;
    if (rwlock.psi_rwlock) {
        /* Instrumentation start */
        rwlock_instr.locker =
            PSI_RWLOCK_CALL(start_rwlock_wrwait)(&rwlock_instr.state,
                                                 rwlock.psi_rwlock,
                                                 PSI_RWLOCK_WRITELOCK,
                                                 src_file,
                                                 src_line);
    }
}

void toku_instr_rwlock_rdlock_wait_end(
    toku_rwlock_instrumentation &rwlock_instr,
    int pthread_rwlock_wait_result) {
    if (rwlock_instr.locker)
        PSI_RWLOCK_CALL(end_rwlock_rdwait)
        (rwlock_instr.locker, pthread_rwlock_wait_result);
}

void toku_instr_rwlock_wrlock_wait_end(
    toku_rwlock_instrumentation &rwlock_instr,
    int pthread_rwlock_wait_result) {
    if (rwlock_instr.locker)
        PSI_RWLOCK_CALL(end_rwlock_wrwait)
        (rwlock_instr.locker, pthread_rwlock_wait_result);
}

void toku_instr_rwlock_unlock(toku_pthread_rwlock_t &rwlock) {
    if (rwlock.psi_rwlock)

// Due to change introduced in e4148f2a22922687f7652c4e3d21a22da07c9e78
// PSI rwlock version and interface changed
// PSI_CURRENT_RWLOCK_VERSION is not defined in MySQL 5.6 and is defined
// as 1 in 5.7 and < 8.0.17
#if defined(PSI_CURRENT_RWLOCK_VERSION) && (PSI_CURRENT_RWLOCK_VERSION == 2)
        PSI_RWLOCK_CALL(unlock_rwlock)(rwlock.psi_rwlock, PSI_RWLOCK_UNLOCK);
#else
        PSI_RWLOCK_CALL(unlock_rwlock)(rwlock.psi_rwlock);
#endif
}

#endif  // TOKU_MYSQL_WITH_PFS
