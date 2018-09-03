#pragma once

#include <stdio.h>  // FILE

// Performance instrumentation object identifier type
typedef unsigned int pfs_key_t;

enum class toku_instr_object_type { mutex, rwlock, cond, thread, file };

struct PSI_file;

struct TOKU_FILE {
    /** The real file. */
    FILE *file;
    struct PSI_file *key;
    TOKU_FILE() : file(nullptr), key(nullptr) {}
};

struct PSI_mutex;
struct PSI_cond;
struct PSI_rwlock;

struct toku_mutex_t;
struct toku_cond_t;
struct toku_pthread_rwlock_t;

class toku_instr_key;

class toku_instr_probe_empty {
   public:
    explicit toku_instr_probe_empty(UU(const toku_instr_key &key)) {}

    void start_with_source_location(UU(const char *src_file),
                                    UU(int src_line)) {}

    void stop() {}
};

#define TOKU_PROBE_START(p) p->start_with_source_location(__FILE__, __LINE__)
#define TOKU_PROBE_STOP(p) p->stop

extern toku_instr_key toku_uninstrumented;

#ifndef MYSQL_TOKUDB_ENGINE

#include <pthread.h>

class toku_instr_key {
   public:
    toku_instr_key(UU(toku_instr_object_type type),
                   UU(const char *group),
                   UU(const char *name)) {}

    explicit toku_instr_key(UU(pfs_key_t key_id)) {}

    ~toku_instr_key() {}
};

typedef toku_instr_probe_empty toku_instr_probe;

enum class toku_instr_file_op {
    file_stream_open,
    file_create,
    file_open,
    file_delete,
    file_rename,
    file_read,
    file_write,
    file_sync,
    file_stream_close,
    file_close,
    file_stat
};

struct PSI_file {};
struct PSI_mutex {};

struct toku_io_instrumentation {};

inline int toku_pthread_create(UU(const toku_instr_key &key),
                               pthread_t *thread,
                               const pthread_attr_t *attr,
                               void *(*start_routine)(void *),
                               void *arg) {
    return pthread_create(thread, attr, start_routine, arg);
}

inline void toku_instr_register_current_thread() {}

inline void toku_instr_delete_current_thread() {}

// Instrument file creation, opening, closing, and renaming
inline void toku_instr_file_open_begin(UU(toku_io_instrumentation &io_instr),
                                       UU(const toku_instr_key &key),
                                       UU(toku_instr_file_op op),
                                       UU(const char *name),
                                       UU(const char *src_file),
                                       UU(int src_line)) {}

inline void toku_instr_file_stream_open_end(
    UU(toku_io_instrumentation &io_instr),
    UU(TOKU_FILE &file)) {}

inline void toku_instr_file_open_end(UU(toku_io_instrumentation &io_instr),
                                     UU(int fd)) {}

inline void toku_instr_file_name_close_begin(
    UU(toku_io_instrumentation &io_instr),
    UU(const toku_instr_key &key),
    UU(toku_instr_file_op op),
    UU(const char *name),
    UU(const char *src_file),
    UU(int src_line)) {}

inline void toku_instr_file_stream_close_begin(
    UU(toku_io_instrumentation &io_instr),
    UU(toku_instr_file_op op),
    UU(TOKU_FILE &file),
    UU(const char *src_file),
    UU(int src_line)) {}

inline void toku_instr_file_fd_close_begin(
    UU(toku_io_instrumentation &io_instr),
    UU(toku_instr_file_op op),
    UU(int fd),
    UU(const char *src_file),
    UU(int src_line)) {}

inline void toku_instr_file_close_end(UU(toku_io_instrumentation &io_instr),
                                      UU(int result)) {}

inline void toku_instr_file_io_begin(UU(toku_io_instrumentation &io_instr),
                                     UU(toku_instr_file_op op),
                                     UU(int fd),
                                     UU(unsigned int count),
                                     UU(const char *src_file),
                                     UU(int src_line)) {}

inline void toku_instr_file_name_io_begin(UU(toku_io_instrumentation &io_instr),
                                          UU(const toku_instr_key &key),
                                          UU(toku_instr_file_op op),
                                          UU(const char *name),
                                          UU(unsigned int count),
                                          UU(const char *src_file),
                                          UU(int src_line)) {}

inline void toku_instr_file_stream_io_begin(
    UU(toku_io_instrumentation &io_instr),
    UU(toku_instr_file_op op),
    UU(TOKU_FILE &file),
    UU(unsigned int count),
    UU(const char *src_file),
    UU(int src_line)) {}

inline void toku_instr_file_io_end(UU(toku_io_instrumentation &io_instr),
                                   UU(unsigned int count)) {}

struct toku_mutex_t;

struct toku_mutex_instrumentation {};

inline PSI_mutex *toku_instr_mutex_init(UU(const toku_instr_key &key),
                                        UU(toku_mutex_t &mutex)) {
    return nullptr;
}

inline void toku_instr_mutex_destroy(UU(PSI_mutex *&mutex_instr)) {}

inline void toku_instr_mutex_lock_start(
    UU(toku_mutex_instrumentation &mutex_instr),
    UU(toku_mutex_t &mutex),
    UU(const char *src_file),
    UU(int src_line)) {}

inline void toku_instr_mutex_trylock_start(
    UU(toku_mutex_instrumentation &mutex_instr),
    UU(toku_mutex_t &mutex),
    UU(const char *src_file),
    UU(int src_line)) {}

inline void toku_instr_mutex_lock_end(
    UU(toku_mutex_instrumentation &mutex_instr),
    UU(int pthread_mutex_lock_result)) {}

inline void toku_instr_mutex_unlock(UU(PSI_mutex *mutex_instr)) {}

struct toku_cond_instrumentation {};

enum class toku_instr_cond_op {
    cond_wait,
    cond_timedwait,
};

inline PSI_cond *toku_instr_cond_init(UU(const toku_instr_key &key),
                                      UU(toku_cond_t &cond)) {
    return nullptr;
}

inline void toku_instr_cond_destroy(UU(PSI_cond *&cond_instr)) {}

inline void toku_instr_cond_wait_start(
    UU(toku_cond_instrumentation &cond_instr),
    UU(toku_instr_cond_op op),
    UU(toku_cond_t &cond),
    UU(toku_mutex_t &mutex),
    UU(const char *src_file),
    UU(int src_line)) {}

inline void toku_instr_cond_wait_end(UU(toku_cond_instrumentation &cond_instr),
                                     UU(int pthread_cond_wait_result)) {}

inline void toku_instr_cond_signal(UU(toku_cond_t &cond)) {}

inline void toku_instr_cond_broadcast(UU(toku_cond_t &cond)) {}

// rwlock instrumentation
struct toku_rwlock_instrumentation {};

inline PSI_rwlock *toku_instr_rwlock_init(UU(const toku_instr_key &key),
                                          UU(toku_pthread_rwlock_t &rwlock)) {
    return nullptr;
}

inline void toku_instr_rwlock_destroy(UU(PSI_rwlock *&rwlock_instr)) {}

inline void toku_instr_rwlock_rdlock_wait_start(
    UU(toku_rwlock_instrumentation &rwlock_instr),
    UU(toku_pthread_rwlock_t &rwlock),
    UU(const char *src_file),
    UU(int src_line)) {}

inline void toku_instr_rwlock_wrlock_wait_start(
    UU(toku_rwlock_instrumentation &rwlock_instr),
    UU(toku_pthread_rwlock_t &rwlock),
    UU(const char *src_file),
    UU(int src_line)) {}

inline void toku_instr_rwlock_rdlock_wait_end(
    UU(toku_rwlock_instrumentation &rwlock_instr),
    UU(int pthread_rwlock_wait_result)) {}

inline void toku_instr_rwlock_wrlock_wait_end(
    UU(toku_rwlock_instrumentation &rwlock_instr),
    UU(int pthread_rwlock_wait_result)) {}

inline void toku_instr_rwlock_unlock(UU(toku_pthread_rwlock_t &rwlock)) {}

#else  // MYSQL_TOKUDB_ENGINE
// There can be not only mysql but also mongodb or any other PFS stuff
#include <toku_instr_mysql.h>
#endif  // MYSQL_TOKUDB_ENGINE

extern toku_instr_key toku_uninstrumented;

extern toku_instr_probe *toku_instr_probe_1;

// threads
extern toku_instr_key *extractor_thread_key;
extern toku_instr_key *fractal_thread_key;
extern toku_instr_key *io_thread_key;
extern toku_instr_key *eviction_thread_key;
extern toku_instr_key *kibbutz_thread_key;
extern toku_instr_key *minicron_thread_key;
extern toku_instr_key *tp_internal_thread_key;

// Files
extern toku_instr_key *tokudb_file_data_key;
extern toku_instr_key *tokudb_file_load_key;
extern toku_instr_key *tokudb_file_tmp_key;
extern toku_instr_key *tokudb_file_log_key;

// Mutexes
extern toku_instr_key *kibbutz_mutex_key;
extern toku_instr_key *minicron_p_mutex_key;
extern toku_instr_key *queue_result_mutex_key;
extern toku_instr_key *tpool_lock_mutex_key;
extern toku_instr_key *workset_lock_mutex_key;
extern toku_instr_key *bjm_jobs_lock_mutex_key;
extern toku_instr_key *log_internal_lock_mutex_key;
extern toku_instr_key *cachetable_ev_thread_lock_mutex_key;
extern toku_instr_key *cachetable_disk_nb_mutex_key;
extern toku_instr_key *cachetable_m_mutex_key;
extern toku_instr_key *safe_file_size_lock_mutex_key;
extern toku_instr_key *checkpoint_safe_mutex_key;
extern toku_instr_key *ft_ref_lock_mutex_key;
extern toku_instr_key *loader_error_mutex_key;
extern toku_instr_key *bfs_mutex_key;
extern toku_instr_key *loader_bl_mutex_key;
extern toku_instr_key *loader_fi_lock_mutex_key;
extern toku_instr_key *loader_out_mutex_key;
extern toku_instr_key *result_output_condition_lock_mutex_key;
extern toku_instr_key *block_table_mutex_key;
extern toku_instr_key *rollback_log_node_cache_mutex_key;
extern toku_instr_key *txn_lock_mutex_key;
extern toku_instr_key *txn_state_lock_mutex_key;
extern toku_instr_key *txn_child_manager_mutex_key;
extern toku_instr_key *txn_manager_lock_mutex_key;
extern toku_instr_key *treenode_mutex_key;
extern toku_instr_key *manager_mutex_key;
extern toku_instr_key *manager_escalation_mutex_key;
extern toku_instr_key *manager_escalator_mutex_key;
extern toku_instr_key *db_txn_struct_i_txn_mutex_key;
extern toku_instr_key *indexer_i_indexer_lock_mutex_key;
extern toku_instr_key *indexer_i_indexer_estimate_lock_mutex_key;
extern toku_instr_key *locktree_request_info_mutex_key;
extern toku_instr_key *locktree_request_info_retry_mutex_key;

// condition vars
extern toku_instr_key *result_state_cond_key;
extern toku_instr_key *bjm_jobs_wait_key;
extern toku_instr_key *cachetable_p_refcount_wait_key;
extern toku_instr_key *cachetable_m_flow_control_cond_key;
extern toku_instr_key *cachetable_m_ev_thread_cond_key;
extern toku_instr_key *bfs_cond_key;
extern toku_instr_key *result_output_condition_key;
extern toku_instr_key *manager_m_escalator_done_key;
extern toku_instr_key *lock_request_m_wait_cond_key;
extern toku_instr_key *queue_result_cond_key;
extern toku_instr_key *ws_worker_wait_key;
extern toku_instr_key *rwlock_wait_read_key;
extern toku_instr_key *rwlock_wait_write_key;
extern toku_instr_key *rwlock_cond_key;
extern toku_instr_key *tp_thread_wait_key;
extern toku_instr_key *tp_pool_wait_free_key;
extern toku_instr_key *frwlock_m_wait_read_key;
extern toku_instr_key *kibbutz_k_cond_key;
extern toku_instr_key *minicron_p_condvar_key;
extern toku_instr_key *locktree_request_info_retry_cv_key;

// rwlocks
extern toku_instr_key *multi_operation_lock_key;
extern toku_instr_key *low_priority_multi_operation_lock_key;
extern toku_instr_key *cachetable_m_list_lock_key;
extern toku_instr_key *cachetable_m_pending_lock_expensive_key;
extern toku_instr_key *cachetable_m_pending_lock_cheap_key;
extern toku_instr_key *cachetable_m_lock_key;
extern toku_instr_key *result_i_open_dbs_rwlock_key;
extern toku_instr_key *checkpoint_safe_rwlock_key;
extern toku_instr_key *cachetable_value_key;
extern toku_instr_key *safe_file_size_lock_rwlock_key;
extern toku_instr_key *cachetable_disk_nb_rwlock_key;
