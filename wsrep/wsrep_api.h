/* Copyright (C) 2009-2011 Codership Oy <info@codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef WSREP_H
#define WSREP_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 *  wsrep replication API
 */

#define WSREP_INTERFACE_VERSION "23"

/*!
 *  Certain provider capabilities application may need to know
 */
#define WSREP_CAP_MULTI_MASTER          ( 1ULL << 0 )
#define WSREP_CAP_CERTIFICATION         ( 1ULL << 1 )
#define WSREP_CAP_PARALLEL_APPLYING     ( 1ULL << 2 )
#define WSREP_CAP_TRX_REPLAY            ( 1ULL << 3 )
#define WSREP_CAP_ISOLATION             ( 1ULL << 4 )
#define WSREP_CAP_PAUSE                 ( 1ULL << 5 )
#define WSREP_CAP_CAUSAL_READS          ( 1ULL << 6 )
#define WSREP_CAP_CAUSAL_TRX            ( 1ULL << 7 )
#define WSREP_CAP_WRITE_SET_INCREMENTS  ( 1ULL << 8 )
#define WSREP_CAP_SESSION_LOCKS         ( 1ULL << 9 )
#define WSREP_CAP_DISTRIBUTED_LOCKS     ( 1ULL << 10 )
#define WSREP_CAP_CONSISTENCY_CHECK     ( 1ULL << 11 )

/*!
 *  Write set replication flags
 */
#define WSREP_FLAG_PA_SAFE              ( 1ULL << 0 )

/* Empty backend spec */
#define WSREP_NONE "none"

typedef uint64_t wsrep_trx_id_t;  //!< application transaction ID
typedef uint64_t wsrep_conn_id_t; //!< application connection ID
typedef int64_t  wsrep_seqno_t;   //!< sequence number of a writeset, etc.

/*! undefined seqno */
#define WSREP_SEQNO_UNDEFINED (-1)

/*! wsrep status codes */
typedef enum wsrep_status {
    WSREP_OK        = 0,   //!< success
    WSREP_WARNING,         //!< minor warning, error logged
    WSREP_TRX_MISSING,     //!< transaction is not known by wsrep
    WSREP_TRX_FAIL,        //!< transaction aborted, server can continue
    WSREP_BF_ABORT,        //!< trx was victim of brute force abort
    WSREP_CONN_FAIL,       //!< error in client connection, must abort
    WSREP_NODE_FAIL,       //!< error in node state, wsrep must reinit
    WSREP_FATAL,           //!< fatal error, server must abort
    WSREP_NOT_IMPLEMENTED  //!< feature not implemented
} wsrep_status_t;

/*!
 * @brief log severity levels, passed as first argument to log handler
 */
typedef enum wsrep_log_level
{
    WSREP_LOG_FATAL, //!< Unrecoverable error, application must quit.
    WSREP_LOG_ERROR, //!< Operation failed, must be repeated.
    WSREP_LOG_WARN,  //!< Unexpected condition, but no operational failure.
    WSREP_LOG_INFO,  //!< Informational message.
    WSREP_LOG_DEBUG  //!< Debug message. Shows only of compiled with debug.
} wsrep_log_level_t;

/*!
 * @brief error log handler
 *
 *        All messages from wsrep library are directed to this
 *        handler, if present.
 *
 * @param level   log level
 * @param message log message
 */
typedef void (*wsrep_log_cb_t)(wsrep_log_level_t, const char *);

/*!
 * UUID type - for all unique IDs
 */
typedef struct wsrep_uuid {
    uint8_t uuid[16];
} wsrep_uuid_t;

/*! Undefined UUID */
static const wsrep_uuid_t WSREP_UUID_UNDEFINED = {{0,}};

/*!
 * Scan UUID from string
 * @return length of UUID string representation or negative error code
 */
extern ssize_t
wsrep_uuid_scan (const char* str, size_t str_len, wsrep_uuid_t* uuid);

/*!
 * Print UUID to string
 * @return length of UUID string representation or negative error code
 */
extern ssize_t
wsrep_uuid_print (const wsrep_uuid_t* uuid, char* str, size_t str_len);

#define WSREP_MEMBER_NAME_LEN 32  //!< maximum logical member name length
#define WSREP_INCOMING_LEN    256 //!< max Domain Name length + 0x00

/*!
 * member status
 */
typedef enum wsrep_member_status {
    WSREP_MEMBER_UNDEFINED, //!< undefined state
    WSREP_MEMBER_JOINER,    //!< incomplete state, requested state transfer
    WSREP_MEMBER_DONOR,     //!< complete state, donates state transfer
    WSREP_MEMBER_JOINED,    //!< complete state
    WSREP_MEMBER_SYNCED,    //!< complete state, synchronized with group
    WSREP_MEMBER_ERROR,     //!< this and above is provider-specific error code
    WSREP_MEMBER_MAX
} wsrep_member_status_t;

/*!
 * static information about a group member (some fields are tentative yet)
 */
typedef struct wsrep_member_info {
    wsrep_uuid_t id;                           //!< group-wide unique member ID
    char         name[WSREP_MEMBER_NAME_LEN];  //!< human-readable name
    char         incoming[WSREP_INCOMING_LEN]; //!< address for client requests
} wsrep_member_info_t;

/*!
 * group status
 */
typedef enum wsrep_view_status {
    WSREP_VIEW_PRIMARY,      //!< primary group configuration (quorum present)
    WSREP_VIEW_NON_PRIMARY,  //!< non-primary group configuration (quorum lost)
    WSREP_VIEW_DISCONNECTED, //!< not connected to group, retrying.
    WSREP_VIEW_MAX
} wsrep_view_status_t;

/*!
 * view of the group
 */
typedef struct wsrep_view_info {
    wsrep_uuid_t        uuid;      //!< global state UUID
    wsrep_seqno_t       seqno;     //!< global state seqno
    wsrep_seqno_t       view;      //!< global view number
    wsrep_view_status_t status;    //!< view status
    bool                state_gap; //!< gap between global and local states
    int                 my_idx;    //!< index of this member in the view
    int                 memb_num;  //!< number of members in the view
    int                 proto_ver; //!< application protocol agreed on in the view
    wsrep_member_info_t members[1]; //!< array of member information
} wsrep_view_info_t;

/*!
 * Magic string to tell provider to engage into trivial (empty) state transfer.
 * No data will be passed, but the node shall be considered JOINED.
 * Should be passed in sst_req parameter of wsrep_view_cb_t.
 */
#define WSREP_STATE_TRANSFER_TRIVIAL "trivial"

/*!
 * Magic string to tell provider not to engage in state transfer at all.
 * The member will stay in WSREP_MEMBER_UNDEFINED state but will keep on
 * receiving all writesets.
 * Should be passed in sst_req parameter of wsrep_view_cb_t.
 */
#define WSREP_STATE_TRANSFER_NONE "none"

/*!
 * @brief group view handler
 *
 * This handler is called in total order corresponding to the group
 * configuration change. It is to provide a vital information about
 * new group view. If view info indicates existence of discontinuity
 * between group and member states, state transfer request message
 * should be filled in by the callback implementation.
 *
 * @note Currently it is assumed that sst_req is allocated using
 *       malloc()/calloc()/realloc() and it will be freed by
 *       wsrep implementation.
 *
 * @param app_ctx     application context
 * @param recv_ctx    receiver context
 * @param view        new view on the group
 * @param state       current state
 * @param state_len   lenght of current state
 * @param sst_req     location to store SST request
 * @param sst_req_len location to store SST request length or error code
 *                    value of 0 means no SST.
 */
typedef void (*wsrep_view_cb_t) (void*                    app_ctx,
                                 void*                    recv_ctx,
                                 const wsrep_view_info_t* view,
                                 const char*              state,
                                 size_t                   state_len,
                                 void**                   sst_req,
                                 ssize_t*                 sst_req_len);

/*!
 * @brief apply callback
 *
 * This handler is called from wsrep library to apply replicated write set
 * Must support brute force applying for multi-master operation
 *
 * @param recv_ctx receiver context pointer provided by the application
 * @param data     data buffer containing the write set
 * @param size     data buffer size
 * @param seqno    global seqno part of the write set to be applied
 *
 * @return success code:
 * @retval WSREP_OK
 * @retval WSREP_NOT_IMPLEMENTED appl. does not support the write set format
 * @retval WSREP_ERROR failed to apply the write set
 */
typedef enum wsrep_status (*wsrep_apply_cb_t)   (void*               recv_ctx,
                                                 const void*         data,
                                                 size_t              size,
                                                 wsrep_seqno_t       seqno);

/*!
 * @brief commit callback
 *
 * This handler is called to commit the changes made by apply callback.
 *
 * @param recv_ctx receiver context pointer provided by the application
 * @param seqno    global seqno part of the write set to be committed
 * @param commit   true - commit writeset, false - rollback writeset
 *
 * @return success code:
 * @retval WSREP_OK
 * @retval WSREP_ERROR call failed
 */
typedef enum wsrep_status (*wsrep_commit_cb_t)  (void*         recv_ctx,
                                                 wsrep_seqno_t seqno,
                                                 bool          commit);

/*!
 * @brief a callback to donate state snapshot
 *
 * This handler is called from wsrep library when it needs this node
 * to deliver state to a new cluster member.
 * No state changes will be committed for the duration of this call.
 * Wsrep implementation may provide internal state to be transmitted
 * to new cluster member for initial state.
 *
 * @param app_ctx   application context
 * @param recv_ctx  receiver context
 * @param msg       state transfer request message
 * @param msg_len   state transfer request message length
 * @param uuid      current state uuid on this node
 * @param seqno     current state seqno on this node
 * @param state     current wsrep internal state buffer
 * @param state_len current wsrep internal state buffer len
 * @param bypass    bypass snapshot transfer, only transfer uuid:seqno pair
 * @return 0 for success or negative error code
 */
typedef int (*wsrep_sst_donate_cb_t) (void*               app_ctx,
                                      void*               recv_ctx,
                                      const void*         msg,
                                      size_t              msg_len,
                                      const wsrep_uuid_t* uuid,
                                      wsrep_seqno_t       seqno,
                                      const char*         state,
                                      size_t              state_len,
                                      bool                bypass);

/*!
 * @brief a callback to signal application that wsrep state is synced
 *        with cluster
 *
 * This callback is called after wsrep library has got in sync with
 * rest of the cluster.
 *
 * @param app_ctx application context
 */
typedef void (*wsrep_synced_cb_t)(void* app_ctx);


/*!
 * Initialization parameters for wsrep, used as arguments for wsrep_init()
 */
struct wsrep_init_args
{
    void* app_ctx;             //!< Application context for callbacks

    /* Configuration parameters */
    const char* node_name;     //!< Symbolic name of this node (e.g. hostname)
    const char* node_address;  //!< Address to be used by wsrep provider
    const char* node_incoming; //!< Address for incoming client connections
    const char* data_dir;      //!< Directory where wsrep files are kept if any
    const char* options;       //!< Provider-specific configuration string
    int         proto_ver;     //!< Max supported application protocol version

    /* Application initial state information. */
    const wsrep_uuid_t* state_uuid;  //!< Application state sequence UUID
    wsrep_seqno_t       state_seqno; //!< Applicaiton state sequence number
    const char*         state;       //!< Initial state for wsrep implementation
    size_t              state_len;   //!< Length of state buffer

    /* Application callbacks */
    wsrep_log_cb_t        logger_cb;       //!< logging handler
    wsrep_view_cb_t       view_handler_cb; //!< group view change handler

    /* applier callbacks */
    wsrep_apply_cb_t      apply_cb;        //!< apply  callback
    wsrep_commit_cb_t     commit_cb;       //!< commit callback

    /* state snapshot transfer callbacks */
    wsrep_sst_donate_cb_t sst_donate_cb;   //!< starting to donate
    wsrep_synced_cb_t     synced_cb;       //!< synced with group
};

/*! Type of the stats variable value in struct wsrep_status_var */
typedef enum wsrep_var_type
{
    WSREP_VAR_STRING, //!< pointer to null-terminated string
    WSREP_VAR_INT64,  //!< int64_t
    WSREP_VAR_DOUBLE  //!< double
}
wsrep_var_type_t;

/*! Generalized stats variable representation */
struct wsrep_stats_var
{
    const char*      name;     //!< variable name
    wsrep_var_type_t type;     //!< variable value type
    union {
        int64_t     _int64;
        double      _double;
        const char* _string;
    } value;                   //!< variable value
};


/*! Key part structure */
typedef struct wsrep_key_part_
{
    const void* buf;     /*!< Buffer containing key part data */
    size_t      buf_len; /*!< Length of buffer */
} wsrep_key_part_t;

/*! Key struct used to pass certification keys for transaction handling calls.
 *  A key consists of zero or more key parts. */
typedef struct wsrep_key_
{
    const wsrep_key_part_t* key_parts;     /*!< Array of key parts        */
    size_t                  key_parts_len; /*!< Length of key parts array */
} wsrep_key_t;

/*! Transaction handle struct passed for wsrep transaction handling calls */
typedef struct wsrep_trx_handle_
{
    wsrep_trx_id_t trx_id; //!< transaction ID
    void*          opaque; //!< opaque provider transaction context data
} wsrep_trx_handle_t;

/*!
 * @brief Helper method to reset trx handle state when trx id changes
 *
 * Instead of passing wsrep_trx_handle_t directly for wsrep calls,
 * wrapping handle with this call offloads bookkeeping from
 * application.
 */
static inline wsrep_trx_handle_t* wsrep_trx_handle_for_id(
    wsrep_trx_handle_t* trx_handle,
    wsrep_trx_id_t      trx_id)
{
    if (trx_handle->trx_id != trx_id)
    {
        trx_handle->trx_id = trx_id;
        trx_handle->opaque = NULL;
    }
    return trx_handle;
}


typedef struct wsrep_ wsrep_t;
/*!
 * wsrep interface for dynamically loadable libraries
 */
struct wsrep_ {

    const char *version; //!< interface version string

  /*!
   * @brief Initializes wsrep provider
   *
   * @param wsrep this wsrep handle
   * @param args  wsrep initialization parameters
   */
    wsrep_status_t (*init)   (wsrep_t*                      wsrep,
                              const struct wsrep_init_args* args);

  /*!
   * @brief Returns provider capabilities flag bitmap
   *
   * @param wsrep this wsrep handle
   */
    uint64_t (*capabilities) (wsrep_t* wsrep);

  /*!
   * @brief Passes provider-specific configuration string to provider.
   *
   * @param wsrep this wsrep handle
   * @param conf  configuration string
   *
   * @retval WSREP_OK      configuration string was parsed successfully
   * @retval WSREP_WARNING could't not parse conf string, no action taken
   */
    wsrep_status_t (*options_set) (wsrep_t* wsrep, const char* conf);

  /*!
   * @brief Returns provider-specific string with current configuration values.
   *
   * @param wsrep this wsrep handle
   *
   * @return a dynamically allocated string with current configuration
   *         parameter values
   */
    char*          (*options_get) (wsrep_t* wsrep);

  /*!
   * @brief Opens connection to cluster
   *
   * Returns when either node is ready to operate as a part of the clsuter
   * or fails to reach operating status.
   *
   * @param wsrep        this wsrep handle
   * @param cluster_name unique symbolic cluster name
   * @param cluster_url  URL-like cluster address (backend://address)
   * @param state_donor  name of the node to be asked for state transfer.
   */
    wsrep_status_t (*connect) (wsrep_t*    wsrep,
                               const char* cluster_name,
                               const char* cluster_url,
                               const char* state_donor);

  /*!
   * @brief Closes connection to cluster.
   *
   * If state_uuid and/or state_seqno is not NULL, will store final state
   * in there.
   *
   * @param wsrep this  wsrep handler
   */
    wsrep_status_t (*disconnect)(wsrep_t* wsrep);

  /*!
   * @brief start receiving replication events
   *
   * This function never returns
   *
   * @param wsrep this wsrep handle
   * @param recv_ctx receiver context
   */
    wsrep_status_t (*recv)(wsrep_t* wsrep, void* recv_ctx);

  /*!
   * @brief Replicates/logs result of transaction to other nodes and allocates
   * required resources.
   *
   * Must be called before transaction commit. Returns success code, which
   * caller must check.
   * In case of WSREP_OK, starts commit critical section, transaction can
   * commit. Otherwise transaction must rollback.
   *
   * @param wsrep      this wsrep handle
   * @param trx_handle transaction which is committing
   * @param conn_id    connection ID
   * @param app_data   application specific applying data
   * @param data_len   the size of the applying data
   * @param flags      fine tuning the replication WSREP_FLAG_*
   * @param seqno      seqno part of the global transaction ID
   *
   * @retval WSREP_OK         cluster-wide commit succeeded
   * @retval WSREP_TRX_FAIL   must rollback transaction
   * @retval WSREP_CONN_FAIL  must close client connection
   * @retval WSREP_NODE_FAIL  must close all connections and reinit
   */
    wsrep_status_t (*pre_commit)(wsrep_t*            wsrep,
                                 wsrep_conn_id_t     conn_id,
                                 wsrep_trx_handle_t* trx_handle,
                                 const void*         app_data,
                                 size_t              data_len,
                                 uint64_t            flags,
                                 wsrep_seqno_t*      seqno);

  /*!
   * @brief Releases resources after transaction commit.
   *
   * Ends commit critical section.
   *
   * @param wsrep      this wsrep handle
   * @param trx_handle transaction which is committing
   * @retval WSREP_OK  post_commit succeeded
   */
    wsrep_status_t (*post_commit) (wsrep_t*            wsrep,
                                   wsrep_trx_handle_t* trx_handle);

  /*!
   * @brief Releases resources after transaction rollback.
   *
   * @param wsrep      this wsrep handle
   * @param trx_handle transaction which is committing
   * @retval WSREP_OK  post_rollback succeeded
   */
    wsrep_status_t (*post_rollback)(wsrep_t*            wsrep,
                                    wsrep_trx_handle_t* trx_handle);

  /*!
   * @brief Replay trx as a slave write set
   *
   * If local trx has been aborted by brute force, and it has already
   * replicated before this abort, we must try if we can apply it as
   * slave trx. Note that slave nodes see only trx write sets and certification
   * test based on write set content can be different to DBMS lock conflicts.
   *
   * @param wsrep      this wsrep handle
   * @param trx_handle transaction which is committing
   * @param trx_ctx    transaction context
   *
   * @retval WSREP_OK         cluster commit succeeded
   * @retval WSREP_TRX_FAIL   must rollback transaction
   * @retval WSREP_BF_ABORT   brute force abort happened after trx replicated
   *                          must rollback transaction and try to replay
   * @retval WSREP_CONN_FAIL  must close client connection
   * @retval WSREP_NODE_FAIL  must close all connections and reinit
   */
    wsrep_status_t (*replay_trx)(wsrep_t*            wsrep,
                                 wsrep_trx_handle_t* trx_handle,
                                 void*               trx_ctx);

  /*!
   * @brief Abort pre_commit() call of another thread.
   *
   * It is possible, that some high-priority transaction needs to abort
   * another transaction which is in pre_commit() call waiting for resources.
   *
   * The kill routine checks that abort is not attmpted against a transaction
   * which is front of the caller (in total order).
   *
   * @param wsrep      this wsrep handle
   * @param bf_seqno   seqno of brute force trx, running this cancel
   * @param victim_trx transaction to be aborted, and which is committing
   *
   * @retval WSREP_OK         abort secceded
   * @retval WSREP_WARNING    abort failed
   */
    wsrep_status_t (*abort_pre_commit)(wsrep_t*       wsrep,
                                       wsrep_seqno_t  bf_seqno,
                                       wsrep_trx_id_t victim_trx);

  /*!
   * @brief Appends a query in transaction's write set
   *
   * @param wsrep      this wsrep handle
   * @param trx_handle transaction handle
   * @param query      SQL statement string
   * @param timeval    time to use for time functions
   * @param randseed   seed for rand
   */
    wsrep_status_t (*append_query)(wsrep_t*            wsrep,
                                   wsrep_trx_handle_t* trx_handle,
                                   const char*         query,
                                   time_t              timeval,
                                   uint32_t            randseed);

  /*!
   * @brief Appends a row reference in transaction's write set
   *
   * @param wsrep       this wsrep handle
   * @param trx_handle  transaction handle
   * @param key         array of keys
   * @param key_len     length of the array of keys
   * @param shared      boolean denoting if key corresponds to shared resource
   */
    wsrep_status_t (*append_key)(wsrep_t*            wsrep,
                                 wsrep_trx_handle_t* trx_handle,
                                 const wsrep_key_t*  key,
                                 size_t              key_len,
                                 bool                shared);
   /*!
    * @brief Appends data in transaction's write set
    *
    * This method can be called any time before commit and it
    * appends data block into transaction's write set.
    *
    * @param wsrep      this wsrep handle
    * @param trx_handle transaction handle
    * @param data data  buffer
    * @param data_len   data buffer length
    */
    wsrep_status_t (*append_data)(wsrep_t*            wsrep,
                                  wsrep_trx_handle_t* trx_handle,
                                  const void*         data,
                                  size_t              data_len);


  /*!
   * @brief Get causal ordering for read operation
   *
   * This call will block until causal ordering with all possible
   * preceding writes in the cluster is guaranteed. If pointer to
   * seqno is non-null, the call stores the global transaction ID
   * of the last transaction which is guaranteed to be ordered
   * causally before this call.
   *
   * @param wsrep this wsrep handle
   * @param seqno location to store global transaction ID
   */
    wsrep_status_t (*causal_read)(wsrep_t* wsrep, wsrep_seqno_t* seqno);

  /*!
   * @brief Clears allocated connection context.
   *
   * Whenever a new connection ID is passed to wsrep provider through
   * any of the API calls, a connection context is allocated for this
   * connection. This call is to explicitly notify provider fo connection
   * closing.
   *
   * @param wsrep       this wsrep handle
   * @param conn_id     connection ID
   * @param query       the 'set database' query
   * @param query_len   length of query (does not end with 0)
   */
    wsrep_status_t (*free_connection)(wsrep_t*        wsrep,
                                      wsrep_conn_id_t conn_id);

  /*!
   * @brief Replicates a query and starts "total order isolation" section.
   *
   * Replicates the query and returns success code, which
   * caller must check. Total order isolation continues
   * until to_execute_end() is called.
   *
   * @param wsrep       this wsrep handle
   * @param conn_id     connection ID
   * @param key         array of keys
   * @param key_len     lenght of the array of keys
   * @param query       query to be executed
   * @param query_len   length of the query string
   * @param seqno       seqno part of the action ID
   *
   * @retval WSREP_OK         cluster commit succeeded
   * @retval WSREP_CONN_FAIL  must close client connection
   * @retval WSREP_NODE_FAIL  must close all connections and reinit
   */
    wsrep_status_t (*to_execute_start)(wsrep_t*           wsrep,
                                       wsrep_conn_id_t    conn_id,
                                       const wsrep_key_t* key,
                                       size_t             key_len,
                                       const void*        query,
                                       size_t             query_len,
                                       wsrep_seqno_t*     seqno);

  /*!
   * @brief Ends the total order isolation section.
   *
   * Marks the end of total order isolation. TO locks are freed
   * and other transactions are free to commit from this point on.
   *
   * @param wsrep this wsrep handle
   * @param conn_id connection ID
   *
   * @retval WSREP_OK         cluster commit succeeded
   * @retval WSREP_CONN_FAIL  must close client connection
   * @retval WSREP_NODE_FAIL  must close all connections and reinit
   */
    wsrep_status_t (*to_execute_end)(wsrep_t* wsrep, wsrep_conn_id_t conn_id);

  /*!
   * @brief Signals to wsrep provider that state snapshot has been sent to
   *        joiner.
   *
   * @param wsrep  this wsrep handle
   * @param uuid   sequence UUID (group UUID)
   * @param seqno  sequence number or negative error code of the operation
   */
    wsrep_status_t (*sst_sent)(wsrep_t*            wsrep,
                               const wsrep_uuid_t* uuid,
                               wsrep_seqno_t       seqno);

  /*!
   * @brief Signals to wsrep provider that new state snapshot has been received.
   *        May deadlock if called from sst_prepare_cb.
   *
   * @param wsrep     this wsrep handle
   * @param uuid      sequence UUID (group UUID)
   * @param seqno     sequence number or negative error code of the operation
   * @param state     initial state provided by SST donor
   * @param state_len length of state buffer
   */
    wsrep_status_t (*sst_received)(wsrep_t*            wsrep,
                                   const wsrep_uuid_t* uuid,
                                   wsrep_seqno_t       seqno,
                                   const char*         state,
                                   size_t              state_len);


  /*!
   * @brief Generate request for consistent snapshot.
   *
   * If successfull, this call will generate internally SST request
   * which in turn triggers calling SST donate callback on the nodes
   * specified in donor_spec. If donor_spec is null, callback is
   * called only locally. This call will block until sst_sent is called
   * from callback.
   *
   * @param wsrep   this wsrep handle
   * @param msg     context message for SST donate callback
   * @param msg_len length of context message
   * @param donor_spec list of snapshot donors
   */
    wsrep_status_t (*snapshot)(wsrep_t*    wsrep,
                               const void* msg,
                               size_t      msg_len,
                               const char* donor_spec);

  /*!
   * @brief Returns an array fo status variables.
   *        Array is terminated by Null variable name.
   *
   * @param wsrep this wsrep handle
   * @return array of struct wsrep_status_var.
   */
    struct wsrep_stats_var* (*stats_get) (wsrep_t* wsrep);

  /*!
   * @brief Release resources that might be associated with the array.
   *
   * @param wsrep this wsrep handle.
   */
    void (*stats_free) (wsrep_t* wsrep, struct wsrep_stats_var* var_array);

  /*!
   * @brief Pauses writeset applying/committing.
   *
   * @return global sequence number of the paused state or negative error code.
   */
    wsrep_seqno_t (*pause) (wsrep_t* wsrep);

  /*!
   * @brief Resumes writeset applying/committing.
   */
    wsrep_status_t (*resume) (wsrep_t* wsrep);

  /*!
   * @brief Desynchronize from cluster
   *
   * Effectively turns off flow control for this node, allowing it
   * to fall behind the cluster.
   */
    wsrep_status_t (*desync) (wsrep_t* wsrep);

  /*!
   * @brief Request to resynchronize with cluster.
   *
   * Effectively turns on flow control. Asynchronous - actual synchronization
   * event to be deliverred via sync_cb.
   */
    wsrep_status_t (*resync) (wsrep_t* wsrep);

  /*!
   * @brief Acquire global named lock
   *
   * @param wsrep wsrep provider handle
   * @param name  lock name
   * @param owner 64-bit owner ID
   * @param tout  timeout in nanoseconds.
   *              0 - return immediately, -1 wait forever.
   * @return wsrep status or negative error code
   * @retval -EDEADLK lock was already acquired by this thread
   * @retval -EBUSY   lock was busy
   */
    wsrep_status_t (*lock) (wsrep_t* wsrep, const char* name, int64_t owner,
                            int64_t tout);

  /*!
   * @brief Release global named lock
   *
   * @param wsrep wsrep provider handle
   * @param name  lock name
   * @param owner 64-bit owner ID
   * @return wsrep status or negative error code
   * @retval -EPERM lock does not belong to this owner
   */
    wsrep_status_t (*unlock) (wsrep_t* wsrep, const char* name, int64_t owner);

  /*!
   * @brief Check if global named lock is locked
   *
   * @param wsrep wsrep provider handle
   * @param name  lock name
   * @param owner if not NULL will contain 64-bit owner ID
   * @param node  if not NULL will contain owner's node UUID
   * @return true if lock is locked
   */
    bool (*is_locked) (wsrep_t* wsrep, const char* name, int64_t* conn,
                       wsrep_uuid_t* node);

  /*!
   * wsrep provider name
   */
    const char* provider_name;

  /*!
   * wsrep provider version
   */
    const char* provider_version;

  /*!
   * wsrep provider vendor name
   */
    const char* provider_vendor;

  /*!
   * @brief Frees allocated resources before unloading the library.
   * @param wsrep this wsrep handle
   */
    void (*free)(wsrep_t* wsrep);

    void *dlh;    //!< reserved for future use
    void *ctx;    //!< reserved for implemetation private context
};

typedef int (*wsrep_loader_fun)(wsrep_t*);

/*!
 *
 * @brief Loads wsrep library
 *
 * @param spec   path to wsrep library. If NULL or WSREP_NONE initialises dummy
 *               pass-through implementation.
 * @param hptr   wsrep handle
 * @param log_cb callback to handle loader messages. Otherwise writes to stderr.
 *
 * @return zero on success, errno on failure
 */
int wsrep_load(const char* spec, wsrep_t** hptr, wsrep_log_cb_t log_cb);

/*!
 * @brief Unloads wsrep library and frees associated resources
 *
 * @param hptr wsrep handler pointer
 */
void wsrep_unload(wsrep_t* hptr);

#ifdef __cplusplus
}
#endif

#endif /* WSREP_H */
