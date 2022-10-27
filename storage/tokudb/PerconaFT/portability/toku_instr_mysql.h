#ifdef TOKU_INSTR_MYSQL_H
// This file can be included only from toku_instumentation.h because
// it replaces the defintitions for the case if MySQL PFS is available
#error "toku_instr_mysql.h can be included only once"
#else  // TOKU_INSTR_MYSQL_H
#define TOKU_INSTR_MYSQL_H

#include <memory>

// As these macros are defined in my_global.h
// and they are also defined in command line
// undefine them here to avoid compilation errors.
#undef __STDC_FORMAT_MACROS
#undef __STDC_LIMIT_MACROS
#include "mysql/psi/mysql_file.h"    // PSI_file
#include "mysql/psi/mysql_thread.h"  // PSI_mutex
#include "mysql/psi/mysql_stage.h"   // PSI_stage

#if (MYSQL_VERSION_ID >= 80000) && ( MYSQL_VERSION_ID <= 100000)
#include "mysql/psi/mysql_cond.h"
#include "mysql/psi/mysql_mutex.h"
#include "mysql/psi/mysql_rwlock.h"
#endif  //  (MYSQL_VERSION_ID >= nn)

#ifndef HAVE_PSI_MUTEX_INTERFACE
#error HAVE_PSI_MUTEX_INTERFACE required
#endif
#ifndef HAVE_PSI_RWLOCK_INTERFACE
#error HAVE_PSI_RWLOCK_INTERFACE required
#endif
#ifndef HAVE_PSI_THREAD_INTERFACE
#error HAVE_PSI_THREAD_INTERFACE required
#endif

// Instrumentation keys

class toku_instr_key {
   private:
    pfs_key_t m_id;

   public:
    toku_instr_key(toku_instr_object_type type,
                   const char *group,
                   const char *name) {
        switch (type) {
            case toku_instr_object_type::mutex: {
                PSI_mutex_info mutex_info{&m_id, name, 0};
                mysql_mutex_register(group, &mutex_info, 1);
            } break;
            case toku_instr_object_type::rwlock: {
                PSI_rwlock_info rwlock_info{&m_id, name, 0};
                mysql_rwlock_register(group, &rwlock_info, 1);
            } break;
            case toku_instr_object_type::cond: {
                PSI_cond_info cond_info{&m_id, name, 0};
                mysql_cond_register(group, &cond_info, 1);
            } break;
            case toku_instr_object_type::thread: {
                PSI_thread_info thread_info{&m_id, name, 0};
                mysql_thread_register(group, &thread_info, 1);
            } break;
            case toku_instr_object_type::file: {
                PSI_file_info file_info{&m_id, name, 0};
                mysql_file_register(group, &file_info, 1);
            } break;
        }
    }

    explicit toku_instr_key(pfs_key_t key_id) : m_id(key_id) {}

    pfs_key_t id() const { return m_id; }
};

// Thread instrumentation
int toku_pthread_create(const toku_instr_key &key,
                        pthread_t *thread,
                        const pthread_attr_t *attr,
                        void *(*start_routine)(void *),
                        void *arg);
void toku_instr_register_current_thread(const toku_instr_key &key);
void toku_instr_delete_current_thread();

// I/O instrumentation

enum class toku_instr_file_op {
    file_stream_open = PSI_FILE_STREAM_OPEN,
    file_create = PSI_FILE_CREATE,
    file_open = PSI_FILE_OPEN,
    file_delete = PSI_FILE_DELETE,
    file_rename = PSI_FILE_RENAME,
    file_read = PSI_FILE_READ,
    file_write = PSI_FILE_WRITE,
    file_sync = PSI_FILE_SYNC,
    file_stream_close = PSI_FILE_STREAM_CLOSE,
    file_close = PSI_FILE_CLOSE,
    file_stat = PSI_FILE_STAT
};

struct toku_io_instrumentation {
    struct PSI_file_locker *locker;
    PSI_file_locker_state state;

    toku_io_instrumentation() : locker(nullptr) {}
};

void toku_instr_file_open_begin(toku_io_instrumentation &io_instr,
                                const toku_instr_key &key,
                                toku_instr_file_op op,
                                const char *name,
                                const char *src_file,
                                int src_line);
void toku_instr_file_stream_open_end(toku_io_instrumentation &io_instr,
                                     TOKU_FILE &file);
void toku_instr_file_open_end(toku_io_instrumentation &io_instr, int fd);
void toku_instr_file_name_close_begin(toku_io_instrumentation &io_instr,
                                      const toku_instr_key &key,
                                      toku_instr_file_op op,
                                      const char *name,
                                      const char *src_file,
                                      int src_line);
void toku_instr_file_stream_close_begin(toku_io_instrumentation &io_instr,
                                        toku_instr_file_op op,
                                        const TOKU_FILE &file,
                                        const char *src_file,
                                        int src_line);
void toku_instr_file_fd_close_begin(toku_io_instrumentation &io_instr,
                                    toku_instr_file_op op,
                                    int fd,
                                    const char *src_file,
                                    int src_line);
void toku_instr_file_close_end(const toku_io_instrumentation &io_instr,
                               int result);
void toku_instr_file_io_begin(toku_io_instrumentation &io_instr,
                              toku_instr_file_op op,
                              int fd,
                              ssize_t count,
                              const char *src_file,
                              int src_line);
void toku_instr_file_name_io_begin(toku_io_instrumentation &io_instr,
                                   const toku_instr_key &key,
                                   toku_instr_file_op op,
                                   const char *name,
                                   ssize_t count,
                                   const char *src_file,
                                   int src_line);
void toku_instr_file_stream_io_begin(toku_io_instrumentation &io_instr,
                                     toku_instr_file_op op,
                                     const TOKU_FILE &file,
                                     ssize_t count,
                                     const char *src_file,
                                     int src_line);
void toku_instr_file_io_end(toku_io_instrumentation &io_instr, ssize_t count);

// Mutex instrumentation

struct toku_mutex_instrumentation {
    struct PSI_mutex_locker *locker;
    PSI_mutex_locker_state state;

    toku_mutex_instrumentation() : locker(nullptr) {}
};

void toku_instr_mutex_init(const toku_instr_key &key, toku_mutex_t &mutex);
void toku_instr_mutex_destroy(PSI_mutex *&mutex_instr);
void toku_instr_mutex_lock_start(toku_mutex_instrumentation &mutex_instr,
                                 toku_mutex_t &mutex,
                                 const char *src_file,
                                 int src_line);
void toku_instr_mutex_trylock_start(toku_mutex_instrumentation &mutex_instr,
                                    toku_mutex_t &mutex,
                                    const char *src_file,
                                    int src_line);
void toku_instr_mutex_lock_end(toku_mutex_instrumentation &mutex_instr,
                               int pthread_mutex_lock_result);
void toku_instr_mutex_unlock(PSI_mutex *mutex_instr);

// Instrumentation probes

class toku_instr_probe_pfs {
   private:
    std::unique_ptr<toku_mutex_t> mutex;
    toku_mutex_instrumentation mutex_instr;

   public:
    explicit toku_instr_probe_pfs(const toku_instr_key &key);

    ~toku_instr_probe_pfs();

    void start_with_source_location(const char *src_file, int src_line) {
        mutex_instr.locker = nullptr;
        toku_instr_mutex_lock_start(mutex_instr, *mutex, src_file, src_line);
    }

    void stop() { toku_instr_mutex_lock_end(mutex_instr, 0); }
};

typedef toku_instr_probe_pfs toku_instr_probe;

// Condvar instrumentation

struct toku_cond_instrumentation {
    struct PSI_cond_locker *locker;
    PSI_cond_locker_state state;

    toku_cond_instrumentation() : locker(nullptr) {}
};

enum class toku_instr_cond_op {
    cond_wait = PSI_COND_WAIT,
    cond_timedwait = PSI_COND_TIMEDWAIT,
};

void toku_instr_cond_init(const toku_instr_key &key, toku_cond_t &cond);
void toku_instr_cond_destroy(PSI_cond *&cond_instr);
void toku_instr_cond_wait_start(toku_cond_instrumentation &cond_instr,
                                toku_instr_cond_op op,
                                toku_cond_t &cond,
                                toku_mutex_t &mutex,
                                const char *src_file,
                                int src_line);
void toku_instr_cond_wait_end(toku_cond_instrumentation &cond_instr,
                              int pthread_cond_wait_result);
void toku_instr_cond_signal(const toku_cond_t &cond);
void toku_instr_cond_broadcast(const toku_cond_t &cond);

// rwlock instrumentation

struct toku_rwlock_instrumentation {
    struct PSI_rwlock_locker *locker;
    PSI_rwlock_locker_state state;

    toku_rwlock_instrumentation() : locker(nullptr) { }
};

void toku_instr_rwlock_init(const toku_instr_key &key,
                            toku_pthread_rwlock_t &rwlock);
void toku_instr_rwlock_destroy(PSI_rwlock *&rwlock_instr);
void toku_instr_rwlock_rdlock_wait_start(
    toku_rwlock_instrumentation &rwlock_instr,
    toku_pthread_rwlock_t &rwlock,
    const char *src_file,
    int src_line);
void toku_instr_rwlock_wrlock_wait_start(
    toku_rwlock_instrumentation &rwlock_instr,
    toku_pthread_rwlock_t &rwlock,
    const char *src_file,
    int src_line);
void toku_instr_rwlock_rdlock_wait_end(
    toku_rwlock_instrumentation &rwlock_instr,
    int pthread_rwlock_wait_result);
void toku_instr_rwlock_wrlock_wait_end(
    toku_rwlock_instrumentation &rwlock_instr,
    int pthread_rwlock_wait_result);
void toku_instr_rwlock_unlock(toku_pthread_rwlock_t &rwlock);

#endif  // TOKU_INSTR_MYSQL_H
